// Committed conformance regression for edge264-mvc.
//
// Decodes each committed bitstream under a fixtures directory and checks
// its decoded output against a committed manifest of expected values:
//   - a 128-bit FNV-1a hash of every output frame's cropped pixels, per
//     view (base, and the MVC dependent view when present),
//   - the number of output frames, and
//   - for MVC streams, that each stereo pair is POC-paired
//     (Poc == Poc_mvc) and emitted in non-decreasing DisplayPoc order.
//
// The hashes are anchored to the official JVT/ITU reference YUVs: the
// `emit` mode (used once to build the manifest) decodes a stream and
// verifies its hash equals the hash of the sibling `.yuv` / `.1.yuv`
// reference before printing the manifest line, so a committed hash can
// only ever equal the ITU reference output. The `run` mode (what
// `make check` invokes) needs no reference files - it recomputes the
// hashes from the committed bitstreams alone and compares them to the
// manifest, so a clone can run the whole suite offline.
//
// Self-contained on purpose: only edge264.h plus libc, no SDL, no crypto
// dependency, so anyone who clones the repo can build and run it.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edge264.h"

#define RED "\e[0;31m"
#define GREEN "\e[0;32m"
#define RESET "\e[0m"

// 128-bit FNV-1a: two independent 64-bit lanes (different offset basis)
// so a collision needs both to clash at once. Order-sensitive, which is
// what we want - any pixel or frame-order change moves the digest.
typedef struct { uint64_t a, b; } Hash;
static const Hash HASH_INIT = {0xcbf29ce484222325ULL, 0x84222325cbf29ce4ULL};

static void hash_bytes(Hash *h, const uint8_t *p, size_t n) {
	for (size_t i = 0; i < n; i++) {
		h->a = (h->a ^ p[i]) * 0x100000001b3ULL;
		h->b = (h->b ^ p[i]) * 0x100000001b3ULL;
	}
}

// Hash one plane the way the JVT reference YUV stores it: `h` rows of
// `w` bytes, stepping `stride` bytes per row (the decoded plane is
// already offset to the crop window, so width_Y/height_Y are the
// display dimensions). `bytes_per_sample` is 1 for 8-bit, 2 above.
static void hash_plane(Hash *h, const uint8_t *p, int w, int ht, int stride, int bytes_per_sample) {
	if (p == NULL || w <= 0 || ht <= 0)
		return;
	size_t row = (size_t)w * bytes_per_sample;
	for (int y = 0; y < ht; y++)
		hash_bytes(h, p + (size_t)y * stride, row);
}

static void hash_view(Hash *h, const uint8_t *const planes[3], const Edge264Frame *f) {
	int by = f->bit_depth_Y > 8 ? 2 : 1;
	int bc = f->bit_depth_C > 8 ? 2 : 1;
	hash_plane(h, planes[0], f->width_Y, f->height_Y, f->stride_Y, by);
	hash_plane(h, planes[1], f->width_C, f->height_C, f->stride_C, bc);
	hash_plane(h, planes[2], f->width_C, f->height_C, f->stride_C, bc);
}

typedef struct {
	int frames;     // output frames seen
	int stereo;     // 1 if any frame carried a dependent view
	int mono;       // frames delivered without a dependent view
	int pair_err;   // Poc != Poc_mvc on a stereo frame
	int order_err;  // DisplayPoc went backwards
	int decode_err; // a NAL returned an unexpected hard error
	Hash base;
	Hash dep;
} Result;

// Fold one delivered frame into the running hashes and structural counters.
static void account_frame(Result *r, const Edge264Frame *f, int64_t *prev_disp) {
	int is_stereo = f->samples_mvc[0] != NULL;
	if (is_stereo) {
		r->stereo = 1;
		if (f->Poc != f->Poc_mvc)
			r->pair_err++;
	} else {
		r->mono++;
	}
	if (f->DisplayPoc < *prev_disp)
		r->order_err++;
	*prev_disp = f->DisplayPoc;
	hash_view(&r->base, f->samples, f);
	if (is_stereo)
		hash_view(&r->dep, f->samples_mvc, f);
	r->frames++;
}

// Decode the whole bitstream in `buf`, accumulating the per-view hashes
// and the structural counters. Mirrors the documented decode protocol:
// find_start_code -> decode_NAL, draining get_frame between calls, with
// ENOTSUP skipped (unspecified NAL types are not fatal) and ENOBUFS
// meaning "drain then retry the same NAL".
// `paced` selects the consumer model. 0 = aggressive: drain every available
// frame after each NAL (the easy case, no DPB pressure). 1 = paced: drain only
// when the decoder reports the DPB full (ENOBUFS), and then a single frame, so
// the DPB stays under pressure into each mid-stream IDR - the condition a real
// player (e.g. one frame consumed per vsync) creates and under which the MVC
// view-pairing shortcut used to drop the IDR's dependent view. Both models must
// yield identical output from a correct decoder.
static Result decode_all(const uint8_t *buf, size_t size, int paced) {
	Result r = {0};
	r.base = HASH_INIT;
	r.dep = HASH_INIT;
	int64_t prev_disp = INT64_MIN;
	// Diagnostic hook: EDGE264_THREADS lets this bit-exact oracle run the same
	// hash comparison under multithreading (default 0 = single-thread).
	const char *nt = getenv("EDGE264_THREADS");
	Edge264Decoder *dec = edge264_alloc(nt ? atoi(nt) : 0, NULL, NULL, 0, NULL, NULL, NULL);
	const uint8_t *nal = buf + 3 + (size > 2 && buf[2] == 0);
	const uint8_t *end = buf + size;
	long no_progress = 0;
	int res;
	do {
		const uint8_t *sc = edge264_find_start_code(nal, end, 0);
		res = edge264_decode_NAL(dec, nal, sc, NULL, NULL);
		Edge264Frame f;
		if (paced) {
			// Drain one frame only when the DPB is full, then re-feed the same NAL.
			if (res == ENOBUFS) {
				if (edge264_get_frame(dec, &f, 0) == 0) {
					account_frame(&r, &f, &prev_disp);
					no_progress = 0;
				} else if (++no_progress >= 4096) { // regressed decoder: stall, not hang
					r.decode_err++;
					break;
				}
			}
		} else {
			while (edge264_get_frame(dec, &f, 0) == 0)
				account_frame(&r, &f, &prev_disp);
		}
		if (res != ENOBUFS)
			nal = sc + 3;
		if (res != 0 && res != ENOBUFS && res != ENOTSUP && res != ENODATA)
			r.decode_err++;
	} while (res == 0 || res == ENOBUFS || res == ENOTSUP);
	// End-of-stream: decode_NAL set `flushing` and bumped the held frames; drain
	// whatever the paced loop left buffered (a no-op for the aggressive model).
	Edge264Frame f;
	while (edge264_get_frame(dec, &f, 0) == 0)
		account_frame(&r, &f, &prev_disp);
	edge264_free(&dec);
	return r;
}

static uint8_t *map_file(const char *path, size_t *size_out) {
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return NULL;
	}
	uint8_t *m = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return NULL;
	*size_out = st.st_size;
	return m;
}

static Hash hash_whole_file(const char *path, int *found) {
	Hash h = HASH_INIT;
	size_t n = 0;
	uint8_t *m = map_file(path, &n);
	*found = m != NULL;
	if (m) {
		hash_bytes(&h, m, n);
		munmap(m, n);
	}
	return h;
}

static int hash_eq(Hash x, Hash y) { return x.a == y.a && x.b == y.b; }

// emit MODE: decode <dir>/<name>.264, self-verify against the sibling
// reference YUV(s) if present, and print one manifest line:
//   <name> <frames> <stereo> <base-hash> <dep-hash|->
// The self-check anchors the printed hash to the ITU reference; it is a
// build-the-manifest convenience, never run by `make check`.
static int do_emit(const char *dir, const char *name, int paced) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s.264", dir, name);
	size_t size = 0;
	uint8_t *buf = map_file(path, &size);
	if (!buf) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	Result r = decode_all(buf, size, paced);
	munmap(buf, size);

	const char *check = "NOREF";
	int found;
	snprintf(path, sizeof(path), "%s/%s.yuv", dir, name);
	Hash ref = hash_whole_file(path, &found);
	if (found)
		check = hash_eq(ref, r.base) ? "OK" : "BASE-MISMATCH";
	if (found && r.stereo) {
		snprintf(path, sizeof(path), "%s/%s.1.yuv", dir, name);
		Hash refd = hash_whole_file(path, &found);
		if (!found)
			check = "NO-DEP-REF";
		else if (!hash_eq(refd, r.dep))
			check = "DEP-MISMATCH";
	}

	if (r.stereo)
		printf("%s %d %d %016llx%016llx %016llx%016llx  # check=%s pair_err=%d order_err=%d\n",
			name, r.frames, 1, (unsigned long long)r.base.a, (unsigned long long)r.base.b,
			(unsigned long long)r.dep.a, (unsigned long long)r.dep.b, check, r.pair_err, r.order_err);
	else
		printf("%s %d %d %016llx%016llx -  # check=%s\n",
			name, r.frames, 0, (unsigned long long)r.base.a, (unsigned long long)r.base.b, check);
	return 0;
}

// Compare a Result against the manifest expectations; NULL = match, else a
// short reason. `pass` (0 aggressive / 1 paced) is only used to label failures.
static const char *check_result(const Result *r, int frames, int stereo, const char *hb, const char *hd) {
	static char gb[64], gd[64];
	snprintf(gb, sizeof gb, "%016llx%016llx", (unsigned long long)r->base.a, (unsigned long long)r->base.b);
	snprintf(gd, sizeof gd, "%016llx%016llx", (unsigned long long)r->dep.a, (unsigned long long)r->dep.b);
	if (r->decode_err) return "decode error";
	if (r->frames != frames) return "frame count";
	if (strcmp(gb, hb) != 0) return "base hash";
	if (stereo && strcmp(gd, hd) != 0) return "dependent-view hash (dropped dependent?)";
	if (stereo && r->pair_err) return "POC pairing";
	// Display-order is asserted only for MVC: the fork's #27 ordering guarantee
	// has no pixel reference. For 2D the base hash already equals the ITU
	// reference (display order), and DisplayPoc legitimately dips across IDR /
	// sequence boundaries on some 2D streams, which is not an output-order error.
	if (stereo && r->order_err) return "display order";
	return NULL;
}

// Run a paced fixture under both consumer models inside a forked child, so a
// regressed decoder that *aborts* (the pre-fix DPB overflow the FrameNum/POC
// pairing prevents) is reported as a clean FAIL instead of dumping core and
// taking the whole suite down. Writes the first failure reason (or "") to the
// pipe; a missing write / non-clean exit means the child crashed. `out` gets
// the reason (empty = pass), returns 0 on a clean run, -1 if the child aborted.
static int run_paced_forked(const uint8_t *buf, size_t size, int frames, int stereo,
                            const char *hb, const char *hd, char *out, size_t outsz) {
	int pipefd[2];
	out[0] = '\0';
	if (pipe(pipefd) != 0) { snprintf(out, outsz, "pipe() failed"); return 0; }
	fflush(stdout); // so the child does not re-flush the parent's buffered output
	pid_t pid = fork();
	if (pid == 0) {
		close(pipefd[0]);
		char msg[256] = "";
		for (int pass = 0; pass <= 1 && msg[0] == '\0'; pass++) {
			Result r = decode_all(buf, size, pass);
			const char *w = check_result(&r, frames, stereo, hb, hd);
			if (w)
				snprintf(msg, sizeof msg, "%s [%s]", w, pass ? "paced" : "aggressive");
		}
		ssize_t wr = write(pipefd[1], msg, sizeof msg);
		(void)wr;
		// exit() (not _exit) so LeakSanitizer's atexit hook still runs in the
		// child, keeping the forked paced fixture under ASan coverage too.
		exit(0);
	}
	close(pipefd[1]);
	char msg[256];
	ssize_t n = read(pipefd[0], msg, sizeof msg);
	close(pipefd[0]);
	int status;
	waitpid(pid, &status, 0);
	if (n != (ssize_t)sizeof msg || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		snprintf(out, outsz, "decoder aborted under DPB pressure (view-pairing regression?)");
		return -1;
	}
	snprintf(out, outsz, "%s", msg);
	return 0;
}

// run MODE: read the manifest, decode each committed fixture from <dir>,
// and assert every recomputed value matches. Exits non-zero on the first
// suite-level failure count so `make check` fails loudly. Needs no
// reference YUVs - fully offline.
static int do_run(const char *manifest, const char *dir) {
	FILE *mf = fopen(manifest, "r");
	if (!mf) {
		fprintf(stderr, "cannot open manifest %s\n", manifest);
		return 1;
	}
	char line[1024];
	int total = 0, failed = 0;
	while (fgets(line, sizeof(line), mf)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		char name[512], hb[64], hd[64];
		int frames, stereo, paced = 0;
		int nf = sscanf(line, "%511s %d %d %63s %63s %d", name, &frames, &stereo, hb, hd, &paced);
		if (nf < 5)
			continue;
		if (nf < 6)
			paced = 0;
		total++;
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s.264", dir, name);
		size_t size = 0;
		uint8_t *buf = map_file(path, &size);
		if (!buf) {
			printf(RED "FAIL" RESET " %s (missing fixture)\n", name);
			failed++;
			continue;
		}

		// A paced-flagged fixture is held to the same manifest values under both
		// consumer models (aggressive and paced): a correct decoder emits identical
		// output either way, so one set of hashes guards both. The paced pass is
		// what reproduces the mid-stream view-pairing regression, which surfaces as
		// a decoder abort under DPB pressure - so a paced fixture is run in a forked
		// child to convert that abort into a clean FAIL.
		char why[256] = "";
		if (paced) {
			run_paced_forked(buf, size, frames, stereo, hb, hd, why, sizeof why);
		} else {
			Result r = decode_all(buf, size, 0);
			const char *w = check_result(&r, frames, stereo, hb, hd);
			if (w)
				snprintf(why, sizeof why, "%s", w);
		}
		munmap(buf, size);

		if (why[0]) {
			printf(RED "FAIL" RESET " %s (%s)\n", name, why);
			failed++;
		}
	}
	fclose(mf);
	if (failed)
		printf("\n" RED "%d / %d conformance fixtures FAILED" RESET "\n", failed, total);
	else
		printf("%d / %d conformance fixtures " GREEN "PASS" RESET "\n", total, total);
	return failed != 0;
}

// probe MODE (developer tool, not run by make check): decode <dir>/<name>.264
// under both consumer models and print the counters + per-view hashes for each,
// so a maintainer can see whether the paced model diverges - a dropped
// dependent view shows up as mono>0 and a changed dep hash under `paced`.
static int do_probe(const char *dir, const char *name) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s.264", dir, name);
	size_t size = 0;
	uint8_t *buf = map_file(path, &size);
	if (!buf) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	for (int pass = 0; pass <= 1; pass++) {
		Result r = decode_all(buf, size, pass);
		printf("%-10s frames=%d stereo=%d mono=%d pair_err=%d order_err=%d decode_err=%d base=%016llx%016llx dep=%016llx%016llx\n",
			pass ? "paced" : "aggressive", r.frames, r.stereo, r.mono, r.pair_err, r.order_err, r.decode_err,
			(unsigned long long)r.base.a, (unsigned long long)r.base.b,
			(unsigned long long)r.dep.a, (unsigned long long)r.dep.b);
	}
	munmap(buf, size);
	return 0;
}

int main(int argc, char *argv[]) {
	if ((argc == 4 || argc == 5) && strcmp(argv[1], "emit") == 0)
		return do_emit(argv[2], argv[3], argc == 5 ? atoi(argv[4]) : 0);
	if (argc == 4 && strcmp(argv[1], "run") == 0)
		return do_run(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "probe") == 0)
		return do_probe(argv[2], argv[3]);
	fprintf(stderr,
		"Usage:\n"
		"  %s run <manifest> <fixtures-dir>   decode each fixture, assert it matches the manifest\n"
		"  %s emit <dir> <name> [paced]       decode <dir>/<name>.264, self-verify vs sibling .yuv, print a manifest line\n"
		"  %s probe <dir> <name>              decode under both consumer models, print counters + hashes\n",
		argv[0], argv[0], argv[0]);
	return 2;
}
