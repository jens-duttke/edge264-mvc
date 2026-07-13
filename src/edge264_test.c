#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
	#include <fcntl.h>
	#include <io.h>
	#include <windows.h>
	#include <psapi.h>
	#define dlsym (void*)GetProcAddress
#else
	#include <dlfcn.h>
	#include <fcntl.h>
	#include <sys/mman.h>
	#include <sys/resource.h>
	#include <sys/stat.h>
	#include <sys/types.h>
	#include <unistd.h>
#endif
#include "edge264_internal.h"



/**
 * Using SDL2 without dependency on header file and development library
 */
#define SDL_INIT_VIDEO 0x20
#define SDL_INIT_EVENTS 0x4000
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_WINDOW_SHOWN 0x4
#define SDL_WINDOW_BORDERLESS 0x10
#define SDL_RENDERER_ACCELERATED 0x2
#define SDL_RENDERER_PRESENTVSYNC 0x4
#define SDL_PIXELFORMAT_IYUV 0x56555949
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_BLENDMODE_BLEND 0x1
#define SDL_QUIT 0x100
#define SDL_KEYDOWN 0x300
#define SDLK_ESCAPE 27
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef union SDL_Event {
	uint32_t type;
	struct {
		uint32_t _[4];
		struct {
			int _;
			int32_t sym;
		} keysym;
	} key;
	uint8_t padding[sizeof(void *) <= 8 ? 56 : sizeof(void *) == 16 ? 64 : 3 * sizeof(void *)];
} SDL_Event;
int (*SDL_Init)(uint32_t);
SDL_Window *(*SDL_CreateWindow)(const char *, int, int, int, int, uint32_t);
SDL_Renderer *(*SDL_CreateRenderer)(SDL_Window *, int, uint32_t);
void (*SDL_SetWindowSize)(SDL_Window *, int, int);
void (*SDL_SetWindowPosition)(SDL_Window *, int, int);
void (*SDL_DestroyTexture)(SDL_Texture *);
SDL_Texture *(*SDL_CreateTexture)(SDL_Renderer *, uint32_t, int, int, int);
int (*SDL_SetTextureBlendMode)(SDL_Texture *, int);
int (*SDL_SetTextureAlphaMod)(SDL_Texture *, uint8_t);
int (*SDL_UpdateYUVTexture)(SDL_Texture *, void *, const uint8_t *, int, const uint8_t *, int, const uint8_t *, int);
int (*SDL_RenderClear)(SDL_Renderer *);
int (*SDL_RenderCopy)(SDL_Renderer *, SDL_Texture *, void *, void *);
void (*SDL_RenderPresent)(SDL_Renderer *);
int (*SDL_PollEvent)(SDL_Event *);
void (*SDL_DestroyTexture)(SDL_Texture *);
void (*SDL_DestroyRenderer)(SDL_Renderer *);
void (*SDL_DestroyWindow)(SDL_Window *);
void (*SDL_Quit)(void);
static void *sdl2;

static int load_SDL2(void) {
	const char *lib, *func;
	#if defined(__wasm__)
		printf("This program does not yet support SDL2 with WASM\n");
		return 1;
	#elif defined(_WIN32)
		sdl2 = LoadLibraryA(lib = "SDL2.dll");
	#else
		sdl2 = dlopen(lib = "/Library/Frameworks/SDL2.framework/SDL2", RTLD_NOW | RTLD_GLOBAL) ?:
			dlopen(lib = "SDL2.so", RTLD_NOW | RTLD_GLOBAL) ?:
			dlopen(lib = "libSDL2.so", RTLD_NOW | RTLD_GLOBAL) ?:
			dlopen(lib = "libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
	#endif
	if (!sdl2) {
		printf("SDL2 is needed for display but not found, please download the library file at https://www.libsdl.org/ and place it in the current folder\n");
		return 1;
	}
	if (!(SDL_Init = dlsym(sdl2, func = "SDL_Init")) ||
	    !(SDL_CreateWindow = dlsym(sdl2, func = "SDL_CreateWindow")) ||
	    !(SDL_CreateRenderer = dlsym(sdl2, func = "SDL_CreateRenderer")) ||
	    !(SDL_SetWindowSize = dlsym(sdl2, func = "SDL_SetWindowSize")) ||
	    !(SDL_SetWindowPosition = dlsym(sdl2, func = "SDL_SetWindowPosition")) ||
	    !(SDL_DestroyTexture = dlsym(sdl2, func = "SDL_DestroyTexture")) ||
	    !(SDL_CreateTexture = dlsym(sdl2, func = "SDL_CreateTexture")) ||
	    !(SDL_SetTextureBlendMode = dlsym(sdl2, func = "SDL_SetTextureBlendMode")) ||
	    !(SDL_SetTextureAlphaMod = dlsym(sdl2, func = "SDL_SetTextureAlphaMod")) ||
	    !(SDL_UpdateYUVTexture = dlsym(sdl2, func = "SDL_UpdateYUVTexture")) ||
	    !(SDL_RenderClear = dlsym(sdl2, func = "SDL_RenderClear")) ||
	    !(SDL_RenderCopy = dlsym(sdl2, func = "SDL_RenderCopy")) ||
	    !(SDL_RenderPresent = dlsym(sdl2, func = "SDL_RenderPresent")) ||
	    !(SDL_PollEvent = dlsym(sdl2, func = "SDL_PollEvent")) ||
	    !(SDL_DestroyTexture = dlsym(sdl2, func = "SDL_DestroyTexture")) ||
	    !(SDL_DestroyRenderer = dlsym(sdl2, func = "SDL_DestroyRenderer")) ||
	    !(SDL_DestroyWindow = dlsym(sdl2, func = "SDL_DestroyWindow")) ||
	    !(SDL_Quit = dlsym(sdl2, func = "SDL_Quit"))) {
		printf("Missing function %s in %s\n", func, lib);
		return 1;
	}
	return 0;
}

static void unload_SDL2(void) {
	#if defined(_WIN32)
		FreeLibrary(sdl2);
	#elif !defined(__wasm__)
		dlclose(sdl2);
	#endif
}



/**
 * Static variables and helper functions
 */
#define RESET  "\e[0m"
#define BOLD   "\e[1m"
#define RED    "\e[31m"
#define GREEN  "\e[32m"
#define YELLOW "\e[33m"
#define BLUE   "\e[34m"

static int display = 0;
static int print_failed = 0;
static int print_passed = 0;
static int print_unsupported = 0;
static int enable_yuv = 1;
static int skip_unsupported = 0;
static int dump = 0; // 0 off, 1 base view, 2 side-by-side (base|dependent)
static int y4m_started = 0; // whether the Y4M stream header was written (per file)
static FILE *msg; // human-readable output: stdout normally, stderr while dumping YUV to stdout
static const char *moveup = "";
FILE *trace_file = NULL;
static Edge264Decoder *d;
static Edge264Frame out;
static const uint8_t *conf[2];
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture0, *texture1;
static int width, height, mvc_display;
static int count_pass, count_unsup, count_fail, count_flag;

static int flt(const struct dirent *a) {
	char *ext = strrchr(a->d_name, '.');
	return ext != NULL && strcmp(ext, ".264") == 0;
}
static int cmp(const struct dirent **a, const struct dirent **b) {
	return -strcasecmp((*a)->d_name, (*b)->d_name);
}



static int draw_frame()
{
	// create or resize the window if necessary
	int has_second_view = out.samples_mvc[0] != NULL;
	if (width != out.width_Y || height != out.height_Y || mvc_display != has_second_view) {
		width = out.width_Y;
		height = out.height_Y;
		mvc_display = has_second_view;
		if (window == NULL) {
			SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
			window = SDL_CreateWindow("edge264_test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
			renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		} else {
			SDL_SetWindowSize(window, width, height);
			SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
			SDL_DestroyTexture(texture0);
			SDL_DestroyTexture(texture1);
		}
		texture0 = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
		texture1 = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
		SDL_SetTextureBlendMode(texture1, SDL_BLENDMODE_BLEND);
		SDL_SetTextureAlphaMod(texture1, 128);
	}
	
	// upload the image to a texture and render!
	SDL_UpdateYUVTexture(texture0, NULL, out.samples[0], out.stride_Y, out.samples[1], out.stride_C, out.samples[2], out.stride_C);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture0, NULL, NULL);
	if (mvc_display) {
		SDL_UpdateYUVTexture(texture1, NULL, out.samples_mvc[0], out.stride_Y, out.samples_mvc[1], out.stride_C, out.samples_mvc[2], out.stride_C);
		SDL_RenderCopy(renderer, texture1, NULL, NULL);
	}
	SDL_RenderPresent(renderer);
	
	// Is user closing the window?
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT ||
			(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
			return 1;
		}
	}
	return 0;
}



/**
 * Write the decoded frame to stdout as a YUV4MPEG2 (Y4M) stream, for piping to
 * an encoder (e.g. ffmpeg | libx264). The self-describing Y4M header carries the
 * dimensions and frame rate, so the pipe is just "| ffmpeg -i - ...".
 * -o dumps the base view; -O writes the two MVC views side by side (base left,
 * dependent right) as one double-width frame - a frame-compatible 3D layout,
 * since there is no open MVC encoder to reproduce a true stereo bitstream.
 */
static void write_plane(const uint8_t *p, int stride, int w, int h) {
	for (int y = 0; y < h; y++)
		fwrite(p + (size_t)y * stride, 1, w, stdout);
}
static void write_plane_sbs(const uint8_t *l, const uint8_t *r, int stride, int w, int h) {
	for (int y = 0; y < h; y++) {
		fwrite(l + (size_t)y * stride, 1, w, stdout);
		fwrite(r + (size_t)y * stride, 1, w, stdout);
	}
}
static void dump_frame(void)
{
	int sbs = dump == 2 && out.samples_mvc[0] != NULL;
	if (!y4m_started) {
		// frame rate from the SPS VUI (time_scale / 2 / num_units_in_tick for a
		// progressive frame); fall back to 24000/1001 when the stream omits it
		// (Y4M requires a rate - override downstream with ffmpeg -r if wrong).
		uint32_t ts = d->sps.time_scale, nu = d->sps.num_units_in_tick;
		int have = ts != 0 && nu != 0 && nu < 0x40000000u; // guard 2*nu against int overflow
		int fnum = have ? (int)ts : 24000;
		int fden = have ? (int)(2 * nu) : 1001;
		fprintf(stdout, "YUV4MPEG2 W%d H%d F%d:%d Ip A1:1 C420mpeg2\n",
			out.width_Y << sbs, out.height_Y, fnum, fden);
		y4m_started = 1;
	}
	fputs("FRAME\n", stdout);
	if (sbs) {
		write_plane_sbs(out.samples[0], out.samples_mvc[0], out.stride_Y, out.width_Y, out.height_Y);
		write_plane_sbs(out.samples[1], out.samples_mvc[1], out.stride_C, out.width_C, out.height_C);
		write_plane_sbs(out.samples[2], out.samples_mvc[2], out.stride_C, out.width_C, out.height_C);
	} else {
		write_plane(out.samples[0], out.stride_Y, out.width_Y, out.height_Y);
		write_plane(out.samples[1], out.stride_C, out.width_C, out.height_C);
		write_plane(out.samples[2], out.stride_C, out.width_C, out.height_C);
	}
}



static int check_frame()
{
	// check that the number of returned views is as expected
	if ((out.samples_mvc[0] != NULL) != (conf[1] != NULL)) {
		printf("Number of returned views (%d) does not match number of YUV files found (%d)\n", (out.samples_mvc[0] != NULL) + 1, (conf[1] != NULL) + 1);
		moveup = "";
		return -2;
	}
	
	// check that each macroblock matches the conformance buffer
	int cropt = out.frame_crop_offsets[0];
	int cropr = out.frame_crop_offsets[1];
	int cropb = out.frame_crop_offsets[2];
	int cropl = out.frame_crop_offsets[3];
	int pic_width_in_mbs = (cropl + out.width_Y + cropr) >> 4;
	int pic_height_in_mbs = (cropt + out.height_Y + cropb) >> 4;
	for (int view = 0; view < 2 && conf[view] != NULL; view += 1) {
		int id = view ? out.FrameId_mvc : out.FrameId;
		for (int row = 0; row < pic_width_in_mbs; row += 1) {
			for (int col = 0; col < pic_height_in_mbs; col += 1) {
				for (int iYCbCr = 0; iYCbCr < 3; iYCbCr++) {
					int stride = (iYCbCr == 0) ? out.stride_Y : out.stride_C;
					int depth = (iYCbCr == 0 ? out.bit_depth_Y : out.bit_depth_C) > 8;
					int sh_width = (iYCbCr > 0 && out.width_C < out.width_Y);
					int sh_height = (iYCbCr > 0 && out.height_C < out.height_Y);
					const uint8_t *p = (view ? out.samples_mvc : out.samples)[iYCbCr];
					const uint8_t *q = conf[view] +
						(iYCbCr > 0) * (out.bit_depth_Y == 8 ? out.width_Y : out.width_Y << 1) * out.height_Y +
						(iYCbCr > 1) * (out.bit_depth_C == 8 ? out.width_C : out.width_C << 1) * out.height_C;
					int xl = max(col * 16 - cropl, 0) >> sh_width;
					int xr = min(col * 16 - cropl + 16, out.width_Y) >> sh_width;
					int invalid = 0;
					for (int y = max(row * 16 - cropt, 0) >> sh_height; xl < xr && y < min(row * 16 - cropt + 16, out.height_Y) >> sh_height; y++)
						invalid |= memcmp(p + y * stride + (xl << depth), q + y * (out.width_Y >> sh_width << depth) + (xl << depth), (xr - xl) << depth);
					if (invalid) {
						printf("Erroneous macroblock (id %d, row %d, column %d, %s plane):\n",
							id, row, col, (iYCbCr == 0) ? "Luma" : (iYCbCr == 1) ? "Cb" : "Cr");
						for (int y = (row * 16 - cropt) >> sh_height; y < (row * 16 - cropt + 16) >> sh_height; y++) {
							for (int x = (col * 16 - cropl) >> sh_width; x < (col * 16 - cropl + 16) >> sh_width; x++) {
								// FIXME 16 bit
								printf(y < 0 || y >= out.height_Y >> sh_height || x < 0 || x >= out.width_Y >> sh_width ? "    " :
									p[y * stride + x] == q[y * (out.width_Y >> sh_width) + x] ? " %3d" :
									RED " %3d" RESET, p[y * stride + x]);
							}
							printf("\n");
						}
						printf("Expected macroblock:\n");
						for (int y = (row * 16 - cropt) >> sh_height; y < (row * 16 - cropt + 16) >> sh_height; y++) {
							for (int x = (col * 16 - cropl) >> sh_width; x < (col * 16 - cropl + 16) >> sh_width; x++) {
								printf(y < 0 || y >= out.height_Y >> sh_height || x < 0 || x >= out.width_Y >> sh_width ? "    " :
									" %3d", q[y * (out.width_Y >> sh_width) + x]);
							}
							printf("\n");
						}
						moveup = "";
						return -2;
					}
				}
			}
		}
		conf[view] +=
			(out.bit_depth_Y == 8 ? out.width_Y : out.width_Y << 1) * out.height_Y +
			(out.bit_depth_C == 8 ? out.width_C : out.width_C << 1) * out.height_C * 2;
	}
	return 0;
}



static int drain_frames(int *res, int *quit)
{
	int drained = 0;
	while (!edge264_get_frame(d, &out, 0)) {
		drained++;
		if (dump)
			dump_frame();
		if (conf[0] != NULL && check_frame()) {
			*res = EBADMSG;
			break;
		} else if (display && draw_frame()) {
			*res = ENODATA;
			*quit = 1;
			break;
		}
	}
	return drained;
}

static int keep_decoding(int res)
{
	// -k: mirror a real player's decode loop by skipping an unsupported NAL
	// (ENOTSUP) and continuing, instead of stopping at the first one. Real 3D
	// Blu-rays carry per-access-unit unspecified NALs (type 24) that edge264
	// reports ENOTSUP by design; without this the tool halts at the first one and
	// never reaches the dependent view.
	return res == 0 || res == ENOBUFS || (res == ENOTSUP && skip_unsupported);
}

static int finish_decode_result(int res, const uint8_t *end1)
{
	edge264_flush(d);
	if (res == ENOBUFS || (res == ENODATA && conf[0] != NULL && conf[0] != end1))
		res = EBADMSG;
	return res;
}

static int decode_mapped_input(const uint8_t *nal, const uint8_t *end0, const uint8_t *end1, int *quit)
{
	nal += 3 + (nal[2] == 0); // skip the [0]001 delimiter
	int res, stuck = 0;
	y4m_started = 0; // one Y4M stream header per file
	do {
		const uint8_t *end = edge264_find_start_code(nal, end0, 0);
		res = edge264_decode_NAL(d, nal, end, NULL, NULL);
		int drained = drain_frames(&res, quit);
		if (res != ENOBUFS)
			nal = end + 3;
		// Progress guard (the caller contract requires one so ENOBUFS cannot
		// spin forever). A DPB that fills a few NALs before end-of-stream can
		// reject the next NAL with ENOBUFS while get_frame drains nothing - the
		// pending frames only come out once the flush sentinel (buf >= end) sets
		// `flushing`. The normal loop reaches that only when nal hits end0, which
		// it never does here (nal is pinned on the un-accepted NAL), so force the
		// sentinel after a stall. Inert on well-formed streams (every ENOBUFS
		// there drains at least one frame, resetting the counter).
		stuck = (res == ENOBUFS && drained == 0) ? stuck + 1 : 0;
		if (stuck > 64) { nal = end0; stuck = 0; }
	} while (keep_decoding(res));
	return finish_decode_result(res, end1);
}

#define STREAM_CHUNK ((size_t)64 * 1024)
#define STREAM_MAX_NAL_SIZE ((size_t)64 * 1024 * 1024)
#define STREAM_MAX_BUFFER_SIZE (STREAM_MAX_NAL_SIZE + STREAM_CHUNK + 4)
#define STREAM_PAD 32

typedef struct StreamBuffer {
	uint8_t *alloc;
	uint8_t *data;
	size_t len;
	size_t cap;
	int fd;
	int eof;
	int saw_start_code;
} StreamBuffer;

static int stream_reserve(StreamBuffer *s, size_t need)
{
	if (need <= s->cap)
		return 0;
	if (need > STREAM_MAX_BUFFER_SIZE) {
		errno = EFBIG;
		return -1;
	}
	size_t cap = s->cap ? s->cap : STREAM_CHUNK;
	while (cap < need) {
		cap *= 2;
		if (cap > STREAM_MAX_BUFFER_SIZE)
			cap = STREAM_MAX_BUFFER_SIZE;
	}
	uint8_t *alloc = malloc(cap + 2 * STREAM_PAD + 15);
	if (alloc == NULL) {
		errno = ENOMEM;
		return -1;
	}
	uint8_t *data = (uint8_t *)(((uintptr_t)alloc + STREAM_PAD + 15) & ~(uintptr_t)15);
	if (s->data != NULL)
		memcpy(data, s->data, s->len);
	free(s->alloc);
	s->alloc = alloc;
	s->data = data;
	s->cap = cap;
	memset(s->data + s->len, 0, STREAM_PAD);
	return 0;
}

static int stream_fill(StreamBuffer *s)
{
	if (s->len == STREAM_MAX_BUFFER_SIZE) {
		errno = EFBIG;
		return -1;
	}
	size_t size = STREAM_MAX_BUFFER_SIZE - s->len;
	if (size > STREAM_CHUNK)
		size = STREAM_CHUNK;
	if (stream_reserve(s, s->len + size))
		return -1;
	int n;
	do {
		#ifdef _WIN32
			n = _read(s->fd, s->data + s->len, (unsigned)size);
		#else
			n = read(s->fd, s->data + s->len, size);
		#endif
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return -1;
	if (n == 0)
		s->eof = 1;
	else
		s->len += n;
	memset(s->data + s->len, 0, STREAM_PAD);
	return 0;
}

static size_t stream_find_start_code(const uint8_t *data, size_t start, size_t end, size_t *delimiter)
{
	const uint8_t *limit = data + end;
	const uint8_t *found = edge264_find_start_code(data + start, limit, 0);
	if (found == limit)
		return SIZE_MAX;
	if (found > data + start && found[-1] == 0)
		found--;
	if (delimiter != NULL)
		*delimiter = found[2] == 1 ? 3 : 4;
	return found - data;
}

static int stream_next_nal(StreamBuffer *s, const uint8_t **nal, const uint8_t **end, size_t *consume)
{
	size_t delimiter, start;
	while ((start = stream_find_start_code(s->data, 0, s->len, &delimiter)) == SIZE_MAX) {
		if (s->eof) {
			if (s->len == 0 && s->saw_start_code)
				return 0;
			errno = EBADMSG;
			return -1;
		}
		for (size_t i = 0; i + 3 < s->len; i++) {
			if (s->data[i] != 0) {
				errno = EBADMSG;
				return -1;
			}
		}
		if (s->len > 3) {
			memmove(s->data, s->data + s->len - 3, 3);
			s->len = 3;
		}
		if (stream_fill(s))
			return -1;
	}
	s->saw_start_code = 1;
	for (size_t i = 0; i < start; i++) {
		if (s->data[i] != 0) {
			errno = EBADMSG;
			return -1;
		}
	}
	if (start != 0) {
		memmove(s->data, s->data + start, s->len - start);
		s->len -= start;
	}

	size_t search = delimiter;
	while (1) {
		size_t next = stream_find_start_code(s->data, search, s->len, NULL);
		if (next != SIZE_MAX) {
			if (next == delimiter) {
				errno = EBADMSG;
				return -1;
			}
			if (next - delimiter > STREAM_MAX_NAL_SIZE) {
				errno = EFBIG;
				return -1;
			}
			*nal = s->data + delimiter;
			*end = s->data + next;
			*consume = next;
			return 1;
		}
		if (s->eof) {
			if (s->len <= delimiter) {
				errno = EBADMSG;
				return -1;
			}
			if (s->len - delimiter > STREAM_MAX_NAL_SIZE) {
				errno = EFBIG;
				return -1;
			}
			*nal = s->data + delimiter;
			*end = s->data + s->len;
			*consume = s->len;
			return 1;
		}
		if (s->len - delimiter > STREAM_MAX_NAL_SIZE + 3) {
			errno = EFBIG;
			return -1;
		}
		search = s->len > 3 ? s->len - 3 : delimiter;
		if (search < delimiter)
			search = delimiter;
		if (stream_fill(s))
			return -1;
	}
}

static void stream_consume(StreamBuffer *s, size_t size)
{
	memmove(s->data, s->data + size, s->len - size);
	s->len -= size;
	memset(s->data + s->len, 0, STREAM_PAD);
}

static int decode_stream_input(int fd, const char *name, const uint8_t *end1, int *quit)
{
	StreamBuffer s = {.fd = fd};
	if (stream_reserve(&s, STREAM_CHUNK)) {
		perror(name);
		*quit = 1;
		return finish_decode_result(EBADMSG, end1);
	}
	const uint8_t *nal = NULL, *end = NULL;
	size_t consume = 0;
	int current = 0, res = 0, stuck = 0;
	y4m_started = 0; // one Y4M stream header per file
	do {
		if (!current) {
			int next = stream_next_nal(&s, &nal, &end, &consume);
			if (next < 0) {
				int error = errno;
				if (error == EFBIG)
					fprintf(msg, "%s: NAL unit exceeds the 64 MiB stream-input limit\n", name);
				else if (error == EBADMSG)
					fprintf(msg, "%s: invalid Annex B byte stream\n", name);
				else {
					errno = error;
					perror(name);
					*quit = 1;
				}
				moveup = "";
				res = EBADMSG;
				break;
			}
			if (next == 0) {
				nal = s.data;
				end = s.data;
				consume = 0;
			}
			current = 1;
		}
		res = edge264_decode_NAL(d, nal, end, NULL, NULL);
		int drained = drain_frames(&res, quit);
		if (res != ENOBUFS) {
			if (consume != 0)
				stream_consume(&s, consume);
			current = 0;
		}
		stuck = (res == ENOBUFS && drained == 0) ? stuck + 1 : 0;
		if (stuck > 64) {
			nal = s.data;
			end = s.data;
			consume = 0;
			current = 1;
			stuck = 0;
		}
	} while (keep_decoding(res));
	free(s.alloc);
	return finish_decode_result(res, end1);
}



static int decode_file(const char *name0)
{
	// process file names
	if (trace_file)
		fprintf(trace_file, "\n--- # %s\n", name0);
	int input_stdin = strcmp(name0, "-") == 0;
	const char *extension = strrchr(name0, '.');
	if (!input_stdin && (extension == NULL || strcmp(extension + 1, "264") != 0))
		return 0;
	int len = input_stdin ? 1 : extension - name0;
	char name1[len + 5], name2[len + 7];
	snprintf(name1, sizeof(name1), "%.*s.yuv", len, name0);
	snprintf(name2, sizeof(name2), "%.*s.1.yuv", len, name0);
	
	// open and memory map all input files
	int quit = 0;
	conf[0] = conf[1] = NULL;
	#ifdef _WIN32
		int fd0 = -1;
		int stream_input = input_stdin;
		HANDLE f0 = INVALID_HANDLE_VALUE, f1 = INVALID_HANDLE_VALUE, f2 = INVALID_HANDLE_VALUE;
		HANDLE m0 = NULL, m1 = NULL, m2 = NULL;
		void *v0 = NULL, *v1 = NULL, *v2 = NULL;
		if (input_stdin) {
			if ((fd0 = _fileno(stdin)) < 0 || _setmode(fd0, _O_BINARY) < 0) {
				perror(name0);
				quit = 1;
			}
		} else if ((f0 = CreateFileA(name0, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE ||
			(m0 = CreateFileMappingA(f0, NULL, PAGE_READONLY, 0, 0, NULL)) == NULL ||
			(v0 = MapViewOfFile(m0, FILE_MAP_READ, 0, 0, 0)) == NULL) {
			printf("Error opening file %s: %lu\n", name0, GetLastError());
			quit = 1;
		}
		if (enable_yuv && !input_stdin) {
			if ((f1 = CreateFileA(name1, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE) {
				printf("%s%s not found\n", moveup, name1);
				moveup = "";
			} else if ((m1 = CreateFileMappingA(f1, NULL, PAGE_READONLY, 0, 0, NULL)) == NULL ||
				(conf[0] = v1 = MapViewOfFile(m1, FILE_MAP_READ, 0, 0, 0)) == NULL) {
				printf("Error opening file %s: %lu\n", name1, GetLastError());
				quit = 1;
			}
			f2 = CreateFileA(name2, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (f2 != INVALID_HANDLE_VALUE &&
				((m2 = CreateFileMappingA(f2, NULL, PAGE_READONLY, 0, 0, NULL)) == NULL ||
				(conf[1] = v2 = MapViewOfFile(m2, FILE_MAP_READ, 0, 0, 0)) == NULL)) {
				printf("Error opening file %s: %lu\n", name2, GetLastError());
				quit = 1;
			}
		}
		const uint8_t *buf0 = v0;
		const uint8_t *end0 = v0 != NULL ? v0 + GetFileSize(f0, NULL) : NULL;
		const uint8_t *end1 = v1 != NULL ? v1 + GetFileSize(f1, NULL) : NULL;
	#else
		int fd0 = -1, fd1 = -1, fd2 = -1;
		int stream_input = input_stdin;
		struct stat st0, st1, st2;
		uint8_t *mm0 = MAP_FAILED, *mm1 = MAP_FAILED, *mm2 = MAP_FAILED;
		if (input_stdin) {
			fd0 = STDIN_FILENO;
		} else if ((fd0 = open(name0, O_RDONLY)) < 0 || fstat(fd0, &st0) < 0) {
			perror(name0);
			quit = 1;
		} else if (S_ISREG(st0.st_mode)) {
			if ((mm0 = mmap(NULL, st0.st_size, PROT_READ, MAP_SHARED, fd0, 0)) == MAP_FAILED) {
				perror(name0);
				quit = 1;
			}
		} else {
			stream_input = 1;
		}
		if (enable_yuv && !input_stdin) {
			if ((fd1 = open(name1, O_RDONLY)) < 0) {
				printf("%s%s not found\n", moveup, name1);
				moveup = "";
			} else if (fstat(fd1, &st1) < 0 ||
				(conf[0] = mm1 = mmap(NULL, st1.st_size, PROT_READ, MAP_SHARED, fd1, 0)) == MAP_FAILED) {
				perror(name1);
				quit = 1;
			}
			fd2 = open(name2, O_RDONLY);
			if (fd2 >= 0 && (fstat(fd2, &st2) < 0 ||
				(conf[1] = mm2 = mmap(NULL, st2.st_size, PROT_READ, MAP_SHARED, fd2, 0)) == MAP_FAILED)) {
				perror(name2);
				quit = 1;
			}
		}
		const uint8_t *buf0 = mm0 != MAP_FAILED ? mm0 : NULL;
		const uint8_t *end0 = mm0 != MAP_FAILED ? mm0 + st0.st_size : NULL;
		const uint8_t *end1 = mm1 != MAP_FAILED ? mm1 + st1.st_size : NULL;
	#endif
	
	// print the success counts
	if (!quit) {
		if (count_flag > 0)
			fprintf(msg, "%s%d " GREEN "PASS" RESET ", %d " YELLOW "UNSUPPORTED" RESET ", %d " RED "FAIL" RESET ", %d " BLUE "FLAGGED" RESET " (%s)\n", moveup, count_pass, count_unsup, count_fail, count_flag, name0);
		else
			fprintf(msg, "%s%d " GREEN "PASS" RESET ", %d " YELLOW "UNSUPPORTED" RESET ", %d " RED "FAIL" RESET " (%s)\n", moveup, count_pass, count_unsup, count_fail, name0);
		moveup = "\e[A\e[K";
		
		// decode the entire file and FAIL on any error
		int res = stream_input ? decode_stream_input(fd0, name0, end1, &quit) :
			decode_mapped_input(buf0, end0, end1, &quit);
		// FIXME interrupt all threads before closing the files!
		
		// print the file that was decoded
		count_pass += res == ENODATA;
		count_unsup += res == ENOTSUP;
		count_fail += res == EBADMSG;
		count_flag += res == ESRCH;
		if (res == ENODATA && print_passed) {
			fprintf(msg, "%s%s: " GREEN "PASS" RESET "\n", moveup, name0);
			moveup = "";
		} else if (res == ENOTSUP && print_unsupported) {
			fprintf(msg, "%s%s: " YELLOW "UNSUPPORTED" RESET "\n", moveup, name0);
			moveup = "";
		} else if (res == EBADMSG && print_failed) {
			fprintf(msg, "%s%s: " RED "FAIL" RESET "\n", moveup, name0);
			moveup = "";
		} else if (res == ESRCH) {
			fprintf(msg, "%s%s: " BLUE "FLAGGED" RESET "\n", moveup, name0);
			moveup = "";
		}
	}
	
	// close everything
	#ifdef _WIN32
		if (v0 != NULL) UnmapViewOfFile(v0);
		if (v1 != NULL) UnmapViewOfFile(v1);
		if (v2 != NULL) UnmapViewOfFile(v2);
		if (m0 != NULL) CloseHandle(m0);
		if (m1 != NULL) CloseHandle(m1);
		if (m2 != NULL) CloseHandle(m2);
		if (f0 != INVALID_HANDLE_VALUE) CloseHandle(f0);
		if (f1 != INVALID_HANDLE_VALUE) CloseHandle(f1);
		if (f2 != INVALID_HANDLE_VALUE) CloseHandle(f2);
	#else
		if (mm0 != MAP_FAILED) munmap(mm0, st0.st_size);
		if (mm1 != MAP_FAILED) munmap(mm1, st1.st_size);
		if (mm2 != MAP_FAILED) munmap(mm2, st2.st_size);
		if (fd0 >= 0 && !input_stdin) close(fd0);
		if (fd1 >= 0) close(fd1);
		if (fd2 >= 0) close(fd2);
	#endif
	return quit;
}



int main(int argc, char *argv[])
{
	// read command-line options
	const char *file_name = "conformance";
	int benchmark = 0;
	int help = 0;
	int n_threads = -1; // multithreaded by default (auto-detect cores); -V forces single-thread
	int trace = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-") == 0) {
			file_name = argv[i];
		} else if (argv[i][0] != '-') {
			file_name = argv[i];
		} else for (int j = 1; argv[i][j]; j++) {
			switch (argv[i][j]) {
				case 'b': benchmark = 1; break;
				case 'd': display = 1; break;
				case 'f': print_failed = 1; break;
				case 'k': skip_unsupported = 1; break;
				case 'm': n_threads = -1; break;
				case 'o': dump = 1; break;
				case 'O': dump = 2; break;
				case 's': n_threads = 0; break;
				case 'p': print_passed = 1; break;
				case 'u': print_unsupported = 1; break;
				case 'v': trace = 1; break;
				case 'V': trace = 2; n_threads = 0; break;
				case 'y': enable_yuv = 0; break;
				default: help = 1; break;
			}
		}
	}
	// while dumping YUV to stdout, skip the reference comparison and keep every
	// human-readable line off stdout (else it corrupts the piped video stream)
	if (dump)
		enable_yuv = 0;
	msg = dump ? stderr : stdout;
	#ifdef _WIN32
		if (dump && _setmode(_fileno(stdout), _O_BINARY) < 0) {
			perror("stdout");
			return 1;
		}
	#endif

	// print help if any argument was unknown
	if (help) {
		printf("Usage: " BOLD "%s [video.264|directory|-] [-hbdfkmoOspuvVy]" RESET "\n"
			"Decodes a video, stdin (-), or all videos inside a directory (./conformance by default),\n"
			"comparing their outputs with inferred YUV pairs (.yuv and .1.yuv extensions).\n"
			"Stream input buffers one NAL unit at a time, limited to 64 MiB.\n"
			"-h\tprint this help and exit\n"
			"-b\tbenchmark decoding time and memory usage\n"
			"-d\tenable display of the videos (requires SDL2)\n"
			"-f\tprint names of failed files in directory\n"
			"-k\tkeep decoding past unsupported NALs instead of stopping (e.g. the\n"
			"\ttype-24 units real 3D Blu-rays carry, which a player skips)\n"
			"-m\tmulti-threaded decoding, auto-detecting cores (this is the default)\n"
			"-o\twrite decoded frames as YUV4MPEG2 (Y4M) to stdout for piping to an\n"
			"\tencoder, e.g. | ffmpeg -i - -c:v libx264 out.mp4 (base view only)\n"
			"-O\tlike -o but side-by-side (base|dependent) for frame-compatible 3D\n"
			"-s\tforce single-threaded decoding\n"
			"-p\tprint names of passed files in directory\n"
			"-u\tprint names of unsupported files in directory\n"
			"-v\tenable output of headers to file trace.yaml (large)\n"
			"-V\tadd output of macroblocks to trace.yaml (very large, implies -vs)\n"
			"-y\tdisable comparison against YUV pairs\n"
			, argv[0]);
		return 0;
	}
	
	if (trace) {
		trace_file = fopen("trace.yaml", "w");
		setvbuf(trace_file, NULL, _IONBF, BUFSIZ);
	}
	
	// load SDL2 if requested
	if (display && load_SDL2())
		return 1;
	
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	d = edge264_alloc(n_threads, trace ? (int(*)(const char*, void*))fputs : NULL, trace_file, trace > 1, NULL, NULL, NULL);
	
	// check if input is a directory by trying to move into it
	if (strcmp(file_name, "-") == 0) {
		decode_file(file_name);
	} else if (chdir(file_name) < 0) {
		decode_file(file_name);
	} else {
		#ifdef _WIN32
			DIR *dp;
			struct dirent *ep;
			dp = opendir(".");
			while ((ep = readdir(dp)) && !decode_file(ep->d_name));
			closedir(dp);
		#else
			struct dirent **entries;
			int n = scandir(".", &entries, flt, cmp);
			while (--n >= 0 && !decode_file(entries[n]->d_name))
				free(entries[n]);
			free(entries);
		#endif
	}
	
	if (count_flag > 0)
		fprintf(msg, "%s%d " GREEN "PASS" RESET ", %d " YELLOW "UNSUPPORTED" RESET ", %d " RED "FAIL" RESET ", %d " BLUE "FLAGGED" RESET "\n", moveup, count_pass, count_unsup, count_fail, count_flag);
	else
		fprintf(msg, "%s%d " GREEN "PASS" RESET ", %d " YELLOW "UNSUPPORTED" RESET ", %d " RED "FAIL" RESET "\n", moveup, count_pass, count_unsup, count_fail);
	edge264_free(&d);
	
	// close SDL if enabled
	if (display) {
		SDL_DestroyTexture(texture0);
		SDL_DestroyTexture(texture1);
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		SDL_Quit();
		unload_SDL2();
	}
	
	// closing information
	if (benchmark) {
		clock_gettime(CLOCK_MONOTONIC, &t1);
		int64_t time_msec = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
		int64_t cpu_msec = 0;
		long mem_kb = 0;
		#if defined(_WIN32)
			HANDLE p = GetCurrentProcess();
			FILETIME c, e, k, u;
			GetProcessTimes(p, &c, &e, &k, &u);
			cpu_msec = ((int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10000;
			PROCESS_MEMORY_COUNTERS m;
			GetProcessMemoryInfo(p, &m, sizeof(m));
			mem_kb = m.PeakPagefileUsage / 1000;
		#elif !defined(__wasm__)
			struct rusage rusage;
			getrusage(RUSAGE_SELF, &rusage);
			cpu_msec = (int64_t)rusage.ru_utime.tv_sec * 1000 + rusage.ru_utime.tv_usec / 1000;
			mem_kb = rusage.ru_maxrss / 1000;
		#endif
		fprintf(msg, "time: %.3lfs\nCPU: %.3lfs\nmemory: %.3lfMB\n", (double)time_msec / 1000, (double)cpu_msec / 1000, (double)mem_kb / 1000);
	}
	if (trace_file)
		fclose(trace_file);
	return 0;
}
