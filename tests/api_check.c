// Contract test of the public API (edge264mvc.h), run by `make check`.
//
// The other harnesses decode streams through the API; this one checks the
// promises the API itself makes to a caller: the version and the defaults,
// the INVALID results, the end of a stream (END after send_end, and END
// again), a flush after the end followed by the same stream again, the
// pts / user_data passthrough, the poc 0 of every IDR picture (8.2.1), and a
// caller that holds its latest frame
// while it sends the next NALs - through an end of sequence and a change of
// the frame size - and the frame size limit max_frame_pixels, at the size of
// each stream's frames after cropping and one below, and a seek to an I picture
// that is not an IDR picture. It uses committed MVC and 2D streams, two of
// them cropped, each single-threaded and with EDGE264MVC_THREADS worker
// threads. The struct layouts are checked when it is compiled.
//
// Usage: api_check <stream.264>... [--seek <stream.264>...] (the held-frame
// check uses the last stream before --seek, an end of sequence, that stream
// again, then the first one, which must differ from it in frame size; the
// streams after --seek are the ones whose first non-IDR I picture no later
// picture predicts across, so decoding can resume there)

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edge264mvc.h"

// The struct layouts are the ABI of the major version: bindings in other
// languages mirror them field by field (Oku3D's Rust binding asserts the
// sizes), so a change here needs a new major version. Pinned on 64-bit
// targets, where pointers are 8 bytes.
#if UINTPTR_MAX == UINT64_MAX
#define LAYOUT(type, field, offset) _Static_assert(offsetof(type, field) == offset, #type "." #field " moved");
LAYOUT(Edge264MvcSettings, n_threads, 0) LAYOUT(Edge264MvcSettings, max_frame_pixels, 4)
LAYOUT(Edge264MvcSettings, log_cb, 8) LAYOUT(Edge264MvcSettings, log_arg, 16)
LAYOUT(Edge264MvcSettings, log_mbs, 24) LAYOUT(Edge264MvcSettings, reserved, 28)
_Static_assert(sizeof(Edge264MvcSettings) == 88, "Edge264MvcSettings changed size");
LAYOUT(Edge264MvcView, planes, 0) LAYOUT(Edge264MvcView, pts, 24) LAYOUT(Edge264MvcView, user_data, 32)
LAYOUT(Edge264MvcView, display_order, 40) LAYOUT(Edge264MvcView, poc, 48) LAYOUT(Edge264MvcView, decode_order, 52)
LAYOUT(Edge264MvcView, flags, 56) LAYOUT(Edge264MvcView, reserved, 60)
_Static_assert(sizeof(Edge264MvcView) == 64, "Edge264MvcView changed size");
LAYOUT(Edge264MvcFrame, views, 0) LAYOUT(Edge264MvcFrame, width_Y, 128) LAYOUT(Edge264MvcFrame, height_Y, 132)
LAYOUT(Edge264MvcFrame, width_C, 136) LAYOUT(Edge264MvcFrame, height_C, 140) LAYOUT(Edge264MvcFrame, stride_Y, 144)
LAYOUT(Edge264MvcFrame, stride_C, 148) LAYOUT(Edge264MvcFrame, bit_depth_Y, 152) LAYOUT(Edge264MvcFrame, bit_depth_C, 156)
LAYOUT(Edge264MvcFrame, crop, 160) LAYOUT(Edge264MvcFrame, handle, 176) LAYOUT(Edge264MvcFrame, reserved, 184)
_Static_assert(sizeof(Edge264MvcFrame) == 216, "Edge264MvcFrame changed size");
#endif

#define RED "\e[0;31m"
#define GREEN "\e[0;32m"
#define RESET "\e[0m"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf(RED "FAIL" RESET " " __VA_ARGS__); printf("\n"); failures++; } } while (0)

typedef struct {
	int frames;
	uint64_t hash;
	int pts_errors; // frames whose pts is not one that was sent with a NAL
	int idr_poc_errors; // views of IDR pictures whose poc is not 0 (8.2.1)
	int64_t max_pts;
	int width, height, coded_width, coded_height; // of the first frame
} Output;

static void receive_all(Edge264MvcDecoder *dec, Output *o) {
	Edge264MvcFrame f;
	while (edge264mvc_receive_frame(dec, &f) == EDGE264MVC_OK) {
		if (o->frames == 0) {
			o->width = f.width_Y;
			o->height = f.height_Y;
			o->coded_width = f.width_Y + f.crop[1] + f.crop[3];
			o->coded_height = f.height_Y + f.crop[0] + f.crop[2];
		}
		for (int v = 0; v < 2; v++) {
			if (f.views[v].planes[0] == NULL)
				continue;
			for (int y = 0; y < f.height_Y; y++)
				for (int x = 0; x < f.width_Y; x++)
					o->hash = (o->hash ^ f.views[v].planes[0][(size_t)y * f.stride_Y + x]) * 1099511628211ull;
			// every NAL was sent with pts = 1000 + its offset and user_data = -pts
			if (f.views[v].pts < 1000 || f.views[v].pts > o->max_pts || f.views[v].user_data != -f.views[v].pts)
				o->pts_errors++;
			// the picture order count restarts at every IDR picture, whose own is 0
			if ((f.views[v].flags & EDGE264MVC_VIEW_IDR) && f.views[v].poc != 0)
				o->idr_poc_errors++;
		}
		o->frames++;
		edge264mvc_release_frame(dec, &f);
	}
}

// Decodes the whole stream and ends it; returns the result of the receive
// after the last frame, which must be END.
static int decode_stream(Edge264MvcDecoder *dec, const uint8_t *buf, size_t size, Output *o) {
	memset(o, 0, sizeof(*o));
	o->hash = 14695981039346656037ull;
	o->max_pts = 1000 + (int64_t)size;
	size_t pos = edge264mvc_find_start_code(buf, size);
	while (pos < size) {
		size_t start = pos + 3;
		size_t next = start + edge264mvc_find_start_code(buf + start, size - start);
		int64_t pts = 1000 + (int64_t)start;
		for (int rounds = 0; edge264mvc_send_nal(dec, buf + start, next - start, pts, -pts) == EDGE264MVC_AGAIN && rounds < 64; rounds++)
			receive_all(dec, o);
		receive_all(dec, o);
		pos = next;
	}
	CHECK(edge264mvc_send_end(dec) == EDGE264MVC_OK, "send_end did not return OK");
	receive_all(dec, o);
	Edge264MvcFrame f;
	return edge264mvc_receive_frame(dec, &f);
}

static uint8_t *load_file(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	uint8_t *m = NULL;
	long n = 0;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
		(m = malloc(n)) != NULL && fread(m, 1, n, f) != (size_t)n) {
		free(m);
		m = NULL;
	}
	fclose(f);
	*size = n;
	return m;
}

static void check_stream(const char *path, int n_threads) {
	size_t size = 0;
	uint8_t *buf = load_file(path, &size);
	CHECK(buf != NULL, "cannot read %s", path);
	if (buf == NULL)
		return;
	Edge264MvcSettings settings;
	edge264mvc_default_settings(&settings);
	settings.n_threads = n_threads;
	Edge264MvcDecoder *dec = NULL;
	CHECK(edge264mvc_open(&dec, &settings) == EDGE264MVC_OK && dec != NULL, "open failed for %s", path);
	if (dec == NULL) {
		free(buf);
		return;
	}
	Output first, again;
	int end = decode_stream(dec, buf, size, &first);
	CHECK(first.frames > 0, "%s (%d threads): no frame", path, n_threads);
	CHECK(end == EDGE264MVC_END, "%s (%d threads): receive after the last frame returned %d, not END", path, n_threads, end);
	Edge264MvcFrame f;
	CHECK(edge264mvc_receive_frame(dec, &f) == EDGE264MVC_END, "%s (%d threads): END was not repeated", path, n_threads);
	CHECK(first.pts_errors == 0, "%s (%d threads): %d views carried a pts or user_data never sent", path, n_threads, first.pts_errors);
	CHECK(first.idr_poc_errors == 0, "%s (%d threads): %d views of IDR pictures had a poc other than 0", path, n_threads, first.idr_poc_errors);
	// a flush after the end starts a new stream (a seek back to the start)
	edge264mvc_flush(dec);
	end = decode_stream(dec, buf, size, &again);
	CHECK(end == EDGE264MVC_END, "%s (%d threads): after flush, the end returned %d, not END", path, n_threads, end);
	CHECK(again.frames == first.frames && again.hash == first.hash,
		"%s (%d threads): after flush, %d frames instead of %d, or other pixels", path, n_threads, again.frames, first.frames);
	edge264mvc_close(&dec);
	CHECK(dec == NULL, "close did not clear the decoder pointer");
	free(buf);
}

static uint64_t hash_frame(const Edge264MvcFrame *f) {
	uint64_t h = 14695981039346656037ull;
	for (int v = 0; v < 2; v++) {
		for (int p = 0; p < 3 && f->views[v].planes[0] != NULL; p++) {
			int width = p ? f->width_C : f->width_Y, height = p ? f->height_C : f->height_Y;
			int stride = p ? f->stride_C : f->stride_Y;
			for (int y = 0; y < height; y++)
				for (int x = 0; x < width; x++)
					h = (h ^ f->views[v].planes[p][(size_t)y * stride + x]) * 1099511628211ull;
		}
	}
	return h;
}

// A caller showing the latest frame keeps it until the next one comes, while
// it sends the following NALs. That must not block an end of sequence or a
// change of the frame size (frames held by the caller only limit how far the
// decoder runs ahead), and the held frame must stay as it was until released.
static void check_held_frame(const char *path_a, const char *path_b, int n_threads) {
	size_t size_a = 0, size_b = 0;
	uint8_t *a = load_file(path_a, &size_a), *b = load_file(path_b, &size_b);
	static const uint8_t end_of_seq[] = {0, 0, 1, 0x0b};
	uint8_t *buf = (a && b) ? malloc(size_a * 2 + sizeof(end_of_seq) + size_b) : NULL;
	CHECK(buf != NULL, "cannot read %s and %s", path_a, path_b);
	Edge264MvcSettings settings;
	edge264mvc_default_settings(&settings);
	settings.n_threads = n_threads;
	Edge264MvcDecoder *dec = NULL;
	Output oa, ob;
	if (buf != NULL && edge264mvc_open(&dec, &settings) == EDGE264MVC_OK) {
		decode_stream(dec, a, size_a, &oa);
		edge264mvc_flush(dec);
		decode_stream(dec, b, size_b, &ob);
		edge264mvc_close(&dec);
	}
	if (buf == NULL || edge264mvc_open(&dec, &settings) != EDGE264MVC_OK) {
		free(a), free(b), free(buf);
		return;
	}
	size_t size = 0;
	memcpy(buf, a, size_a), size += size_a;
	memcpy(buf + size, end_of_seq, sizeof(end_of_seq)), size += sizeof(end_of_seq);
	memcpy(buf + size, a, size_a), size += size_a;
	memcpy(buf + size, b, size_b), size += size_b;
	Edge264MvcFrame held, f;
	uint64_t held_hash = 0;
	int holding = 0, frames = 0, stuck = 0, changed = 0;
	for (size_t pos = edge264mvc_find_start_code(buf, size), next; pos < size && !stuck; pos = next) {
		size_t start = pos + 3;
		next = start + edge264mvc_find_start_code(buf + start, size - start);
		for (int rounds = 0; edge264mvc_send_nal(dec, buf + start, next - start, 0, 0) == EDGE264MVC_AGAIN; rounds++) {
			if (rounds == 64) {
				stuck = 1;
				break;
			}
			while (edge264mvc_receive_frame(dec, &f) == EDGE264MVC_OK) {
				if (holding) {
					changed += hash_frame(&held) != held_hash;
					edge264mvc_release_frame(dec, &held);
				}
				held = f, held_hash = hash_frame(&f), holding = 1, frames++;
			}
		}
	}
	edge264mvc_send_end(dec);
	while (edge264mvc_receive_frame(dec, &f) == EDGE264MVC_OK) {
		if (holding) {
			changed += hash_frame(&held) != held_hash;
			edge264mvc_release_frame(dec, &held);
		}
		held = f, held_hash = hash_frame(&f), holding = 1, frames++;
	}
	if (holding) {
		changed += hash_frame(&held) != held_hash;
		edge264mvc_release_frame(dec, &held);
	}
	CHECK(!stuck, "%s (%d threads): send_nal returned AGAIN for good while the caller held a frame", path_a, n_threads);
	CHECK(stuck || frames == oa.frames * 2 + ob.frames, "%s, end of sequence, %s again, then %s (%d threads): %d frames instead of %d",
		path_a, path_a, path_b, n_threads, frames, oa.frames * 2 + ob.frames);
	CHECK(changed == 0, "%s (%d threads): %d held frames changed before they were released", path_a, n_threads, changed);
	edge264mvc_close(&dec);
	free(a), free(b), free(buf);
}

// Reads one Exp-Golomb code at the start of a slice header, where no
// emulation prevention byte can occur yet.
static unsigned read_ue(const uint8_t *p, size_t size, size_t *bit) {
	int zeros = 0;
	while (*bit < size * 8 && !(p[*bit >> 3] >> (7 - (*bit & 7)) & 1))
		zeros++, (*bit)++;
	(*bit)++;
	unsigned v = 1;
	for (int i = 0; i < zeros && *bit < size * 8; i++, (*bit)++)
		v = v << 1 | (p[*bit >> 3] >> (7 - (*bit & 7)) & 1);
	return v - 1;
}

typedef struct {
	int frames;
	uint64_t hash[1024];
	int32_t decode_order[1024];
} Frames;

static void receive_frames(Edge264MvcDecoder *dec, Frames *o) {
	Edge264MvcFrame f;
	while (edge264mvc_receive_frame(dec, &f) == EDGE264MVC_OK) {
		if (o->frames < 1024) {
			o->hash[o->frames] = hash_frame(&f);
			o->decode_order[o->frames] = f.views[0].decode_order;
		}
		o->frames++;
		edge264mvc_release_frame(dec, &f);
	}
}

// Sends the NALs starting in [from, to), only the parameter sets if asked.
static void send_nals(Edge264MvcDecoder *dec, const uint8_t *buf, size_t size, size_t from, size_t to, int parameter_sets, Frames *o) {
	for (size_t pos = from + edge264mvc_find_start_code(buf + from, size - from), next; pos < to; pos = next) {
		size_t start = pos + 3;
		next = start + edge264mvc_find_start_code(buf + start, size - start);
		int type = buf[start] & 31;
		if (parameter_sets && type != 7 && type != 8 && type != 15)
			continue;
		for (int rounds = 0; edge264mvc_send_nal(dec, buf + start, next - start, 0, 0) == EDGE264MVC_AGAIN && rounds < 64; rounds++)
			receive_frames(dec, o);
		receive_frames(dec, o);
	}
}

// A seek to an I picture that is not an IDR picture: flush, then the parameter
// sets, then the stream from that picture's access unit on. Decoding resumes
// there, so the picture is the first one out - the pictures displayed before
// it predict from pictures that were never sent (an open GOP) - and every
// picture that comes out equals the one decoded from the start of the stream.
static void check_seek(const char *path, int n_threads) {
	size_t size = 0;
	uint8_t *buf = load_file(path, &size);
	if (buf == NULL)
		return;
	size_t entry = 0, prev_pos = 0;
	int prev_type = 0;
	for (size_t pos = edge264mvc_find_start_code(buf, size), next; pos < size && entry == 0; pos = next) {
		size_t start = pos + 3;
		next = start + edge264mvc_find_start_code(buf + start, size - start);
		int type = buf[start] & 31;
		if (type == 1) {
			size_t bit = 8;
			unsigned first_mb_in_slice = read_ue(buf + start, next - start, &bit);
			unsigned slice_type = read_ue(buf + start, next - start, &bit) % 5;
			if (first_mb_in_slice == 0 && slice_type == 2)
				entry = prev_type == 14 ? prev_pos : pos; // with the prefix of an MVC base view
		}
		prev_pos = pos, prev_type = type;
	}
	Edge264MvcSettings settings;
	edge264mvc_default_settings(&settings);
	settings.n_threads = n_threads;
	Edge264MvcDecoder *dec = NULL;
	Frames *all = calloc(2, sizeof(Frames)), *seek = all + 1;
	if (entry == 0 || all == NULL || edge264mvc_open(&dec, &settings) != EDGE264MVC_OK) {
		free(all), free(buf);
		return;
	}
	send_nals(dec, buf, size, 0, size, 0, all);
	edge264mvc_send_end(dec);
	receive_frames(dec, all);
	edge264mvc_flush(dec);
	send_nals(dec, buf, size, 0, entry, 1, seek);
	send_nals(dec, buf, size, entry, size, 0, seek);
	edge264mvc_send_end(dec);
	receive_frames(dec, seek);
	edge264mvc_close(&dec);
	int first = 0;
	for (int i = 1; i < seek->frames && i < 1024; i++)
		first = seek->decode_order[i] < seek->decode_order[first] ? i : first;
	CHECK(first == 0, "%s (%d threads): after a seek to an I picture, %d pictures displayed before it came out",
		path, n_threads, first);
	int tail = all->frames - seek->frames, differ = 0;
	for (int i = 0; i < seek->frames && tail >= 0 && i < 1024; i++)
		differ += seek->hash[i] != all->hash[tail + i];
	CHECK(seek->frames > 0 && tail >= 0 && differ == 0,
		"%s (%d threads): after a seek to an I picture, %d frames of which %d differ from the last ones of the %d decoded from the start",
		path, n_threads, seek->frames, differ, all->frames);
	free(all), free(buf);
}

static void discard_log(const char *line, void *arg) {}

// Decodes a whole stream single-threaded with the given max_frame_pixels.
static Output decode_with_limit(const uint8_t *buf, size_t size, int32_t max_frame_pixels) {
	Output o = {};
	Edge264MvcSettings settings;
	edge264mvc_default_settings(&settings);
	settings.n_threads = 1;
	settings.max_frame_pixels = max_frame_pixels;
	Edge264MvcDecoder *dec = NULL;
	CHECK(edge264mvc_open(&dec, &settings) == EDGE264MVC_OK, "open with max_frame_pixels %d failed", max_frame_pixels);
	if (dec != NULL) {
		decode_stream(dec, buf, size, &o);
		edge264mvc_close(&dec);
	}
	return o;
}

// max_frame_pixels bounds the frame the caller receives, after cropping, and
// lets the coded frame exceed it only by rounding each dimension up to whole
// macroblocks (which bounds the memory of a stream that crops most of it away).
static void check_frame_limit(const char *path) {
	size_t size = 0;
	uint8_t *buf = load_file(path, &size);
	if (buf == NULL)
		return;
	Output all = decode_with_limit(buf, size, 0);
	int pixels = all.width * all.height;
	int width_mbs = all.coded_width >> 4, height_mbs = all.coded_height >> 4;
	int fits = width_mbs * height_mbs <= pixels / 256 + width_mbs + height_mbs + 1;
	Output at = decode_with_limit(buf, size, pixels);
	CHECK(at.frames == (fits ? all.frames : 0), "%s: with max_frame_pixels %dx%d, %d frames instead of %d",
		path, all.width, all.height, at.frames, fits ? all.frames : 0);
	Output below = decode_with_limit(buf, size, pixels - 1);
	CHECK(below.frames == 0, "%s: with max_frame_pixels one below %dx%d, %d frames instead of 0",
		path, all.width, all.height, below.frames);
	free(buf);
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <stream.264>...\n", argv[0]);
		return 2;
	}
	CHECK(edge264mvc_api_version() >> 16 == EDGE264MVC_API_VERSION_MAJOR, "library major version %u, header %d",
		edge264mvc_api_version() >> 16, EDGE264MVC_API_VERSION_MAJOR);
	CHECK(edge264mvc_version() != NULL && edge264mvc_version()[0] != '\0', "empty version string");
	Edge264MvcSettings settings;
	memset(&settings, 0xff, sizeof(settings));
	edge264mvc_default_settings(&settings);
	CHECK(settings.n_threads == 0 && settings.max_frame_pixels == 0 && settings.log_cb == NULL && settings.log_mbs == 0,
		"default_settings did not set the documented defaults");
	// the INVALID results and NULL tolerance the header documents
	CHECK(edge264mvc_open(NULL, NULL) == EDGE264MVC_INVALID, "open(NULL) is not INVALID");
	CHECK(edge264mvc_send_nal(NULL, (const uint8_t *)"", 0, 0, 0) == EDGE264MVC_INVALID, "send_nal(NULL decoder) is not INVALID");
	CHECK(edge264mvc_send_end(NULL) == EDGE264MVC_INVALID, "send_end(NULL) is not INVALID");
	Edge264MvcFrame frame;
	CHECK(edge264mvc_receive_frame(NULL, &frame) == EDGE264MVC_INVALID, "receive_frame(NULL decoder) is not INVALID");
	Edge264MvcDecoder *none = NULL;
	edge264mvc_close(&none); // accepted
	edge264mvc_close(NULL);
	// a fresh decoder has nothing to give before any NAL
	Edge264MvcDecoder *dec = NULL;
	CHECK(edge264mvc_open(&dec, NULL) == EDGE264MVC_OK, "open with default settings failed");
	CHECK(edge264mvc_receive_frame(dec, &frame) == EDGE264MVC_AGAIN, "receive on a fresh decoder is not AGAIN");
	CHECK(edge264mvc_receive_frame(dec, NULL) == EDGE264MVC_INVALID, "receive_frame(NULL frame) is not INVALID");
	edge264mvc_close(&dec);
	// a log callback needs the logs variant: without it the settings are invalid,
	// never a memory allocation failure
	settings.log_cb = discard_log;
	int with_log = edge264mvc_open(&dec, &settings);
	CHECK(with_log == EDGE264MVC_OK || with_log == EDGE264MVC_INVALID, "open with a log_cb returned %d, not OK or INVALID", with_log);
	if (with_log == EDGE264MVC_OK)
		edge264mvc_close(&dec);
	CHECK(dec == NULL, "open or close with a log_cb left a decoder pointer");
	settings.log_cb = NULL;
	// find_start_code reads only inside the buffer
	const uint8_t sc[] = {7, 0, 0, 1, 5};
	CHECK(edge264mvc_find_start_code(sc, sizeof(sc)) == 1, "find_start_code missed the start code");
	CHECK(edge264mvc_find_start_code(sc, 3) == 3, "find_start_code matched past the end");
	CHECK(edge264mvc_find_start_code(NULL, 5) == 5, "find_start_code(NULL) did not return the size (no start code)");
	const char *nt = getenv("EDGE264MVC_THREADS");
	int threads = nt ? atoi(nt) : 4;
	int streams = 1;
	while (streams < argc && strcmp(argv[streams], "--seek") != 0)
		streams++;
	for (int i = 1; i < streams; i++) {
		check_stream(argv[i], 1);
		check_stream(argv[i], threads);
		check_frame_limit(argv[i]);
	}
	if (streams > 2) {
		check_held_frame(argv[streams - 1], argv[1], 1);
		check_held_frame(argv[streams - 1], argv[1], threads);
	}
	for (int i = streams + 1; i < argc; i++) {
		check_seek(argv[i], 1);
		check_seek(argv[i], threads);
	}
	if (failures) {
		printf(RED "%d API contract checks FAILED" RESET "\n", failures);
		return 1;
	}
	printf("API contract " GREEN "PASS" RESET "\n");
	return 0;
}
