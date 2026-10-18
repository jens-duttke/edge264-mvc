/**
 * libFuzzer harness: decode arbitrary bytes as an Annex-B stream through the
 * public API, following the contract a player relies on - send each NAL,
 * receive the frames on EDGE264MVC_AGAIN and send the same NAL again, end the
 * stream and receive the rest - and read every byte of every output plane, so
 * that out-of-bounds reads, hangs and contract violations surface under
 * AddressSanitizer and UndefinedBehaviorSanitizer.
 *
 * Build (clang): see `make fuzz`, then run e.g.
 *   ./fuzz_decode -max_len=262144 -timeout=30 -rss_limit_mb=4096 corpus tests/conformance/2d
 * EDGE264MVC_FUZZ_THREADS sets n_threads (default 1: decode on the calling
 * thread, which keeps every finding reproducible).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "edge264mvc.h"

static volatile uint8_t sink;

static int receive_frames(Edge264MvcDecoder *dec) {
	Edge264MvcFrame frm;
	int n = 0;
	while (edge264mvc_receive_frame(dec, &frm) == EDGE264MVC_OK) {
		uint8_t acc = 0;
		for (int view = 0; view < 2; view++) {
			if (frm.views[view].planes[0] == NULL)
				continue;
			for (int p = 0; p < 3; p++) {
				int w = p ? frm.width_C : frm.width_Y;
				int h = p ? frm.height_C : frm.height_Y;
				int stride = p ? frm.stride_C : frm.stride_Y;
				int bytes = (p ? frm.bit_depth_C : frm.bit_depth_Y) > 8 ? 2 : 1;
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w * bytes; x++)
						acc ^= frm.views[view].planes[p][(size_t)y * stride + x];
			}
		}
		sink = acc;
		edge264mvc_release_frame(dec, &frm);
		n++;
	}
	return n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	Edge264MvcSettings settings;
	edge264mvc_default_settings(&settings);
	const char *env = getenv("EDGE264MVC_FUZZ_THREADS");
	settings.n_threads = env ? atoi(env) : 1;
	Edge264MvcDecoder *dec;
	if (edge264mvc_open(&dec, &settings) != EDGE264MVC_OK) {
		// without a decoder every input would pass untested
		fprintf(stderr, "edge264mvc_open failed (EDGE264MVC_FUZZ_THREADS=%s)\n", env ? env : "unset");
		abort();
	}
	size_t pos = edge264mvc_find_start_code(data, size);
	while (pos < size) {
		size_t start = pos + 3;
		size_t next = start + edge264mvc_find_start_code(data + start, size - start);
		// the contract: every AGAIN round makes progress, so a run of rounds
		// without any frame (each dropping an undeliverable picture) is bounded
		for (int empty_rounds = 0; edge264mvc_send_nal(dec, data + start, next - start, (int64_t)start, 0) == EDGE264MVC_AGAIN; ) {
			empty_rounds = receive_frames(dec) ? 0 : empty_rounds + 1;
			if (empty_rounds > 32) {
				fprintf(stderr, "EDGE264MVC_AGAIN rounds without progress\n");
				abort();
			}
		}
		receive_frames(dec);
		pos = next;
	}
	edge264mvc_send_end(dec);
	receive_frames(dec);
	Edge264MvcFrame frm;
	if (edge264mvc_receive_frame(dec, &frm) != EDGE264MVC_END) {
		fprintf(stderr, "the end of the stream was not reported\n");
		abort();
	}
	edge264mvc_close(&dec);
	return 0;
}
