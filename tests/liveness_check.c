// Committed liveness regression for edge264-mvc.
//
// Decodes each (possibly deliberately-damaged) bitstream under a fixtures
// directory and asserts it delivers the expected number of output (base-view)
// frames WITHOUT stalling. This guards against M1-class deadlocks: an MVC base
// frame whose POC-matching dependent view is missing (a dropped/corrupt
// dependent NAL on a damaged 3D stream) must not block output forever - the
// decoder has to emit the unpairable base alone so a draining caller always
// makes forward progress (ffmpeg likewise decodes the base view of such a
// stream and terminates).
//
// The harness drives the documented decode protocol with a PROGRESS GUARD, so
// a regressed (stalling) decoder fails cleanly with "stall" instead of hanging
// the test suite. Manifest lines: "<name> <expected_base_frames>".
//
// Self-contained: only edge264.h + libc, like tests/conformance_check.c.
// Usage: liveness_check run <manifest> <fixtures-dir>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edge264.h"

#define RED "\e[0;31m"
#define GREEN "\e[0;32m"
#define RESET "\e[0m"

// Re-feeding the same NAL this many times with neither a delivered frame nor a
// non-ENOBUFS result means the decoder cannot make progress => stall.
#define STALL_LIMIT 4096

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

// Returns delivered base-frame count, or -1 if the decoder stalled.
static int decode_count(const uint8_t *buf, size_t size) {
	// EDGE264_THREADS lets the liveness suite run the damaged-stream fixtures
	// under multithreading (default 0), guarding the multithreaded teardown and
	// MVC-pairing deadlock fixes against regressions.
	const char *nt = getenv("EDGE264_THREADS");
	Edge264Decoder *dec = edge264_alloc(nt ? atoi(nt) : 0, NULL, NULL, 0, NULL, NULL, NULL);
	const uint8_t *nal = buf + 3 + (size > 2 && buf[2] == 0);
	const uint8_t *end = buf + size;
	int frames = 0, res;
	long no_progress = 0;
	do {
		const uint8_t *sc = edge264_find_start_code(nal, end, 0);
		res = edge264_decode_NAL(dec, nal, sc, NULL, NULL);
		int delivered = 0;
		Edge264Frame f;
		while (edge264_get_frame(dec, &f, 0) == 0) { frames++; delivered++; }
		if (res == ENOBUFS) {
			if (delivered == 0 && ++no_progress >= STALL_LIMIT) { frames = -1; break; }
			// ENOBUFS => drain (done above) then re-feed the same NAL
		} else {
			no_progress = 0;
			nal = sc + 3;
		}
	} while (res == 0 || res == ENOBUFS || res == ENOTSUP);
	edge264_free(&dec);
	return frames;
}

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
		char name[512];
		int expected;
		if (sscanf(line, "%511s %d", name, &expected) != 2)
			continue;
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
		int got = decode_count(buf, size);
		munmap(buf, size);
		if (got < 0) {
			printf(RED "FAIL" RESET " %s (stall: no forward progress)\n", name);
			failed++;
		} else if (got != expected) {
			printf(RED "FAIL" RESET " %s (delivered %d frames, expected %d)\n", name, got, expected);
			failed++;
		}
	}
	fclose(mf);
	if (failed)
		printf("\n" RED "%d / %d liveness fixtures FAILED" RESET "\n", failed, total);
	else
		printf("%d / %d liveness fixtures " GREEN "PASS" RESET "\n", total, total);
	return failed != 0;
}

int main(int argc, char *argv[]) {
	if (argc == 4 && strcmp(argv[1], "run") == 0)
		return do_run(argv[2], argv[3]);
	fprintf(stderr, "Usage: %s run <manifest> <fixtures-dir>\n", argv[0]);
	return 2;
}
