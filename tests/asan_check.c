// Committed memory-safety regression for edge264-mvc, built with
// AddressSanitizer (`make check-asan`).
//
// Decodes each bundled crafted bitstream under tests/asan/ with a LOG CALLBACK
// SET, because the SEI parser (parse_sei) only runs in the logging path - the
// default decode routes SEI NALs to ignore_NAL. Under ASAN, a regression that
// reintroduces an out-of-bounds access aborts with a clear report (non-zero
// exit); a regression that reintroduces the unbounded payloadSize skip loop is
// caught by the `timeout` wrapper in the Makefile target (the loop spins
// billions of iterations). With the fixes in place every fixture decodes
// cleanly and quickly, so the harness exits 0.
//
// Fixtures (tests/asan/manifest.txt lists the names):
//   sei_payloadtype_oob  - SEI with payloadType > 205; guards the unguarded
//                          payloadType_names[] / parse_sei_message[] index (M2).
//   sei_payloadsize_dos  - SEI with a huge payloadSize for an unsupported type;
//                          guards the unbounded skip loop (M5).
//
// Self-contained: only edge264.h + libc. Usage: asan_check run <manifest> <dir>

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

// A non-NULL log callback is required so the decoder selects the log-enabled
// parsers (parse_sei_log); it deliberately does nothing with the strings.
static int logcb(const char *s, void *a) { (void)s; (void)a; return 0; }

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

static void decode_all(const uint8_t *buf, size_t size) {
	Edge264Decoder *dec = edge264_alloc(0, logcb, NULL, 0, NULL, NULL, NULL);
	const uint8_t *nal = buf + 3 + (size > 2 && buf[2] == 0);
	const uint8_t *end = buf + size;
	int res;
	do {
		const uint8_t *sc = edge264_find_start_code(nal, end, 0);
		res = edge264_decode_NAL(dec, nal, sc, NULL, NULL);
		Edge264Frame f;
		while (edge264_get_frame(dec, &f, 0) == 0) {}
		if (res != ENOBUFS)
			nal = sc + 3;
	} while (res == 0 || res == ENOBUFS || res == ENOTSUP);
	edge264_free(&dec);
}

static int do_run(const char *manifest, const char *dir) {
	FILE *mf = fopen(manifest, "r");
	if (!mf) {
		fprintf(stderr, "cannot open manifest %s\n", manifest);
		return 1;
	}
	char line[1024];
	int total = 0;
	while (fgets(line, sizeof(line), mf)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		char name[512];
		if (sscanf(line, "%511s", name) != 1)
			continue;
		total++;
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s.264", dir, name);
		size_t size = 0;
		uint8_t *buf = map_file(path, &size);
		if (!buf) {
			printf(RED "FAIL" RESET " %s (missing fixture)\n", name);
			fclose(mf);
			return 1;
		}
		// If a memory-safety regression is present, ASAN aborts here.
		decode_all(buf, size);
		munmap(buf, size);
	}
	fclose(mf);
	printf("%d / %d asan fixtures " GREEN "PASS" RESET " (no sanitizer error)\n", total, total);
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc == 4 && strcmp(argv[1], "run") == 0)
		return do_run(argv[2], argv[3]);
	fprintf(stderr, "Usage: %s run <manifest> <fixtures-dir>\n", argv[0]);
	return 2;
}
