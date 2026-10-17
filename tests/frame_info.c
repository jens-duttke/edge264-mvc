// Prints one line per decoded view of a stream: its view, POC, decoding and
// display order, flags, size and a hash of its samples, so that two decodes
// of the same stream compare by more than their samples - two dependent views
// paired with the wrong base views, or a picture flagged concealed in one run
// only, give the same samples (tests/stress_check.py compares them).
//
// Usage: frame_info <stream.264> (EDGE264MVC_THREADS sets n_threads, default
// 1). Unsupported and corrupt NAL units are skipped, as a player does.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "edge264mvc.h"

static uint64_t hash_plane(uint64_t h, const uint8_t *p, int width, int height, int stride, int bytes) {
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width * bytes; x++)
			h = (h ^ p[(size_t)y * stride + x]) * 0x100000001b3ull; // FNV-1a
	}
	return h;
}

static int receive_all(Edge264MvcDecoder *d) {
	Edge264MvcFrame f;
	int received = 0;
	while (edge264mvc_receive_frame(d, &f) == EDGE264MVC_OK) {
		for (int v = 0; v < 2; v++) {
			const Edge264MvcView *w = &f.views[v];
			if (w->planes[0] == NULL)
				continue;
			uint64_t h = 0xcbf29ce484222325ull;
			h = hash_plane(h, w->planes[0], f.width_Y, f.height_Y, f.stride_Y, f.bit_depth_Y > 8 ? 2 : 1);
			h = hash_plane(h, w->planes[1], f.width_C, f.height_C, f.stride_C, f.bit_depth_C > 8 ? 2 : 1);
			h = hash_plane(h, w->planes[2], f.width_C, f.height_C, f.stride_C, f.bit_depth_C > 8 ? 2 : 1);
			printf("view %d poc %d decode_order %d display_order %lld flags %u %dx%d %016llx\n", v, w->poc,
				w->decode_order, (long long)w->display_order, w->flags, f.width_Y, f.height_Y, (unsigned long long)h);
		}
		edge264mvc_release_frame(d, &f);
		received = 1;
	}
	return received;
}

int main(int argc, char *argv[]) {
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <stream.264>\n", argv[0]);
		return 2;
	}
	FILE *file = fopen(argv[1], "rb");
	if (file == NULL) {
		perror(argv[1]);
		return 2;
	}
	size_t size = 0, cap = 1 << 20;
	uint8_t *buf = malloc(cap);
	for (size_t n; buf != NULL && (n = fread(buf + size, 1, cap - size, file)) > 0; ) {
		size += n;
		if (size == cap)
			buf = realloc(buf, cap *= 2);
	}
	fclose(file);
	if (buf == NULL) {
		fprintf(stderr, "%s: out of memory\n", argv[1]);
		return 2;
	}
	Edge264MvcSettings s;
	edge264mvc_default_settings(&s);
	s.n_threads = getenv("EDGE264MVC_THREADS") ? atoi(getenv("EDGE264MVC_THREADS")) : 1;
	Edge264MvcDecoder *d;
	if (edge264mvc_open(&d, &s) != EDGE264MVC_OK) {
		fprintf(stderr, "edge264mvc_open failed\n");
		return 1;
	}
	int status = 0;
	for (size_t pos = edge264mvc_find_start_code(buf, size); pos < size; ) {
		size_t nal = pos + 3;
		size_t next = nal + edge264mvc_find_start_code(buf + nal, size - nal);
		size_t end = next;
		while (end > nal && buf[end - 1] == 0) // trailing_zero_8bits and the zero of a 4-byte start code
			end--;
		int res = EDGE264MVC_OK;
		// every round of AGAIN must make progress, so 32 without a frame mark a decoder fault
		for (int empty = 0; end > nal && (res = edge264mvc_send_nal(d, buf + nal, end - nal, 0, 0)) == EDGE264MVC_AGAIN; ) {
			if (receive_all(d))
				empty = 0;
			else if (++empty >= 32)
				break;
		}
		if (res == EDGE264MVC_AGAIN || res == EDGE264MVC_NOMEM || res == EDGE264MVC_INVALID) {
			fprintf(stderr, "%s: edge264mvc_send_nal returned %d\n", argv[1], res);
			status = 1;
			break;
		}
		receive_all(d);
		pos = next;
	}
	edge264mvc_send_end(d);
	receive_all(d);
	edge264mvc_close(&d);
	free(buf);
	return status;
}
