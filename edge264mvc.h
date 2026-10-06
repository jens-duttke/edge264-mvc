/**
 * edge264-mvc - H.264 and H.264 MVC (3D) software decoder
 *
 * Copyright (c) 2013-2014, Celticom / TVLabs
 * Copyright (c) 2014-2026 Thibault Raffaillac <traf@kth.se>
 * Copyright (c) 2026 Jens Duttke
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of their
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef EDGE264MVC_H
#define EDGE264MVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// marks the functions exported from the shared library, which is built with
// hidden visibility for everything else
#if defined(EDGE264MVC_BUILD) && defined(_WIN32)
	#define EDGE264MVC_API __declspec(dllexport) // once one symbol is, MinGW exports no others
#elif defined(EDGE264MVC_BUILD) && defined(__GNUC__)
	#define EDGE264MVC_API __attribute__((visibility("default")))
#else
	#define EDGE264MVC_API
#endif

/**
 * Version of this header. A library with the same major version and a minor
 * version at least as high implements it; edge264mvc_api_version() returns
 * the version of the library actually loaded, as (major << 16 | minor << 8 |
 * patch).
 */
#define EDGE264MVC_API_VERSION_MAJOR 2
#define EDGE264MVC_API_VERSION_MINOR 0
#define EDGE264MVC_API_VERSION_PATCH 0
#define EDGE264MVC_API_VERSION (EDGE264MVC_API_VERSION_MAJOR << 16 | EDGE264MVC_API_VERSION_MINOR << 8 | EDGE264MVC_API_VERSION_PATCH)

/**
 * Results of the functions below. They have the same values on every platform
 * and toolchain, and every negative value other than these is reserved.
 */
enum {
	EDGE264MVC_OK = 0,
	// edge264mvc_send_nal: the decoder is full. Receive the frames that are
	// ready, then send the same NAL again. Every such round makes progress: a
	// frame comes out, or a picture that can never be output (e.g. an MVC
	// dependent view whose base view is missing) is dropped to make room.
	// edge264mvc_receive_frame: no frame is ready, send more NALs.
	EDGE264MVC_AGAIN = -1,
	// edge264mvc_receive_frame: every frame was returned after edge264mvc_send_end.
	EDGE264MVC_END = -2,
	// The NAL uses a type or feature this decoder does not support (e.g. the
	// unspecified NAL types 0 and 24-31 carried by some 3D Blu-rays, interlaced
	// coding, or a frame larger than max_frame_pixels). It was skipped; send the
	// next one.
	EDGE264MVC_UNSUPPORTED = -3,
	// The NAL is damaged. It was skipped and the pictures it belonged to are
	// concealed (see EDGE264MVC_VIEW_CONCEALED); send the next one.
	EDGE264MVC_CORRUPT = -4,
	// Memory allocation failed. The NAL was not decoded; it may be sent again.
	EDGE264MVC_NOMEM = -5,
	// An argument is invalid (e.g. a NULL decoder).
	EDGE264MVC_INVALID = -6,
};

/**
 * Threading: call the functions of one decoder from one thread at a time (a
 * player that sends from one thread and receives from another serializes the
 * calls with its own lock). The decoder's worker threads are internal to it.
 * Different decoders are independent and can be used from different threads
 * at the same time.
 */
typedef struct Edge264MvcDecoder Edge264MvcDecoder;

/**
 * Receives one line of the YAML trace of the headers (and macroblocks with
 * log_mbs), for debugging. Calls may come from worker threads.
 */
typedef void (*Edge264MvcLogCb)(const char *line, void *log_arg);

/**
 * Decoder settings. Initialize them with edge264mvc_default_settings before
 * changing any field, so that a newer field keeps its default.
 */
typedef struct Edge264MvcSettings {
	// 0 (default): one worker thread per logical CPU available to the process;
	// 1: decode synchronously inside edge264mvc_send_nal, on the calling thread;
	// N > 1: N worker threads (at most 128 are used).
	int32_t n_threads;
	// Largest frame size in luma pixels, after cropping (as width_Y * height_Y
	// of the frames). Larger frames are reported as EDGE264MVC_UNSUPPORTED, and
	// so is a stream whose coded frame exceeds the limit by more than rounding
	// each dimension up to a multiple of 16. 0 (default): 35651584 (8192x4352),
	// the largest frame any level of H.264 allows.
	int32_t max_frame_pixels;
	Edge264MvcLogCb log_cb; // NULL (default): no trace; needs a library built with the logs variant
	void *log_arg;
	int32_t log_mbs; // 1: include every macroblock in the trace (very large), 0 (default): headers only
	uint8_t reserved[60]; // zero
} Edge264MvcSettings;

/**
 * Flags of a decoded view.
 */
enum {
	// Part of the picture was missing or damaged and was concealed from other
	// pictures (or set to grey). The rest of the picture is decoded normally.
	EDGE264MVC_VIEW_CONCEALED = 1 << 0,
	// The picture is an IDR picture, from which decoding can start.
	EDGE264MVC_VIEW_IDR = 1 << 1,
};

/**
 * One view of a decoded frame.
 */
typedef struct Edge264MvcView {
	// Y, Cb and Cr planes, already cropped. NULL in a frame without this view
	// (the dependent view of a 2D frame, or of an MVC base whose dependent
	// view was missing).
	const uint8_t *planes[3];
	int64_t pts; // values given to edge264mvc_send_nal with the first NAL of this picture
	int64_t user_data;
	int64_t display_order; // strictly increasing in output order, per view
	int32_t poc; // picture order count as coded, reset by every IDR
	int32_t decode_order; // increasing in decoding order, per decoder
	uint32_t flags; // EDGE264MVC_VIEW_*
	uint32_t reserved;
} Edge264MvcView;

/**
 * A decoded frame, in display order. For MVC streams it carries both views of
 * one access unit. Its planes stay valid until edge264mvc_release_frame.
 */
typedef struct Edge264MvcFrame {
	Edge264MvcView views[2]; // [0]: base view, [1]: dependent view (MVC)
	int32_t width_Y; // after cropping
	int32_t height_Y;
	int32_t width_C;
	int32_t height_C;
	int32_t stride_Y; // in bytes, between rows of a plane
	int32_t stride_C;
	int32_t bit_depth_Y;
	int32_t bit_depth_C;
	int32_t crop[4]; // pixels removed from the coded picture {top, right, bottom, left}
	void *handle; // internal
	uint8_t reserved[32];
} Edge264MvcFrame;

/**
 * Library version, as (major << 16 | minor << 8 | patch) and as a string.
 */
EDGE264MVC_API uint32_t edge264mvc_api_version(void);
EDGE264MVC_API const char *edge264mvc_version(void);

EDGE264MVC_API void edge264mvc_default_settings(Edge264MvcSettings *settings);

/**
 * Allocates a decoder with the given settings (NULL for the defaults).
 * Returns EDGE264MVC_OK, EDGE264MVC_NOMEM or EDGE264MVC_INVALID (a negative
 * setting, or a log_cb when the library was built without the logs variant).
 */
EDGE264MVC_API int edge264mvc_open(Edge264MvcDecoder **decoder, const Edge264MvcSettings *settings);

/**
 * Stops the worker threads and frees the decoder, including the frames not
 * released yet. Sets *decoder to NULL. Accepts NULL.
 */
EDGE264MVC_API void edge264mvc_close(Edge264MvcDecoder **decoder);

/**
 * Sends one NAL unit, without its 00 00 01 start code. The bytes are copied
 * (or decoded) before returning, so the buffer can be reused at once. pts and
 * user_data are passed through to the views of the picture this NAL starts.
 *
 * Returns EDGE264MVC_OK, EDGE264MVC_AGAIN (receive frames, then send the same
 * NAL again), EDGE264MVC_UNSUPPORTED or EDGE264MVC_CORRUPT (skipped, send the
 * next NAL), EDGE264MVC_NOMEM or EDGE264MVC_INVALID.
 */
EDGE264MVC_API int edge264mvc_send_nal(Edge264MvcDecoder *decoder, const uint8_t *nal, size_t size, int64_t pts, int64_t user_data);

/**
 * Signals the end of the stream: edge264mvc_receive_frame returns every frame
 * still held, then EDGE264MVC_END. Sending a NAL afterwards starts a new
 * stream. Returns EDGE264MVC_OK or EDGE264MVC_INVALID.
 */
EDGE264MVC_API int edge264mvc_send_end(Edge264MvcDecoder *decoder);

/**
 * Returns the next frame in display order. Returns EDGE264MVC_OK,
 * EDGE264MVC_AGAIN (no frame ready, send more NALs), EDGE264MVC_END (after
 * edge264mvc_send_end, every frame was returned) or EDGE264MVC_INVALID.
 *
 * When edge264mvc_send_nal returned EDGE264MVC_AGAIN, or after
 * edge264mvc_send_end, this waits for the worker threads to finish the frames
 * that are due instead of returning EDGE264MVC_AGAIN at once.
 */
EDGE264MVC_API int edge264mvc_receive_frame(Edge264MvcDecoder *decoder, Edge264MvcFrame *frame);

/**
 * Gives a received frame back to the decoder, which may then reuse its memory.
 * Every received frame must be released, and frames held by the caller limit
 * how far the decoder can run ahead.
 */
EDGE264MVC_API void edge264mvc_release_frame(Edge264MvcDecoder *decoder, const Edge264MvcFrame *frame);

/**
 * Discards every picture and the decoding state, e.g. to seek. Decoding
 * resumes at the next IDR picture or recovery point. Frames already received
 * stay valid until released.
 */
EDGE264MVC_API void edge264mvc_flush(Edge264MvcDecoder *decoder);

/**
 * Returns the offset of the first 00 00 01 start code in buf[0..size), or size
 * if there is none. Reads only inside the buffer.
 */
EDGE264MVC_API size_t edge264mvc_find_start_code(const uint8_t *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
