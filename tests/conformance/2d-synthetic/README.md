# Synthetic 2D robustness fixtures

Small, self-contained bitstreams that pin edge264-mvc's decoded output for
real-world decode-robustness cases the ITU conformance vectors do not cover
(the ITU vectors are conformant by construction, so they never exercise the
non-conformant-but-common patterns real encoders and remuxers emit). Run by
`make check` via `tests/conformance_check.c`; hashes are 128-bit FNV-1a of the
cropped output, same as the rest of `manifest.txt`.

Unlike `2d/` (official ITU vectors) these are anchored to **FFmpeg's** decode:
`conformance_check emit` self-verifies edge264's output against a sibling `.yuv`
produced by `ffmpeg -i <name>.264 -f rawvideo -pix_fmt yuv420p <name>.yuv`, and
the committed hash is only accepted when they match (`check=OK`). The `.yuv` is
not committed (regenerable from the `.264`).

## `over_level_dpb.264`

Guards the over-level DPB/reference-count fix. A stream whose frame size exceeds
its signaled `level_idc` (non-conformant, but extremely common - encoders and
muxers routinely under-declare the level) makes the level-derived `MaxDpbFrames`
smaller than the stream's own signaled `max_num_ref_frames`. Clamping the
reference set down to that made the sliding-window marking (8.2.5.3) retire
pictures the slices still reference, so inter prediction read stale/reused DPB
slots: **silently wrong pixels** in single-thread and a **nondeterministic
multi-thread** decode (buffer-reuse race), with no error flagged. FFmpeg (and
this fixture's reference) honour the signaled reference count regardless of level.

352x288, High, CABAC, 4 reference frames, B-frames, 24 frames; the `level_idc`
in the SPS is downgraded to 1.1 (`MaxDpbMbs = 900`, `900 / 396 mbs = 2 < 4 refs`)
so the bug triggers. Reproduce:

    ffmpeg -f lavfi -i testsrc2=size=352x288:rate=25 -frames:v 24 \
      -c:v libx264 -profile:v high -pix_fmt yuv420p \
      -x264-params ref=4:bframes=2:keyint=100:min-keyint=100:scenecut=0 \
      -f h264 base.264
    # then set SPS level_idc (RBSP byte 2) to 11

Without the fix this fixture's line FAILs (wrong base hash, and nondeterministic
under `EDGE264_THREADS`); with it, single- and multi-thread both match the
FFmpeg-anchored hash.
