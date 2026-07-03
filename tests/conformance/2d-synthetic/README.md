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

## `pps_scaling_fallback.264`

Guards the PPS scaling-list fall-back fix (H.264 Table 7-2). A PPS that sets
`pic_scaling_matrix_present_flag = 1` with **every** `pic_scaling_list_present_flag[i] = 0`
over an SPS with `seq_scaling_matrix_present_flag = 0` must derive its scaling
lists from **Fall-Back Rule Set A** - the `Default_4x4_Intra/Inter` and
`Default_8x8_Intra/Inter` matrices (tables 7-3/7-4), *not* the flat-16 lists the
SPS carries. edge264 used to inherit the SPS's `Flat_16` for the absent lists
(rule set B), so every coefficient dequantized with the wrong weighting -
whole-picture colour-block corruption on any stream using this legal, common PPS
shape (observed on a commercial 3D Blu-ray, in the plain AVC base view and,
through inter-view prediction, the dependent view). This is the mirror image of
the (rejected) upstream PR #26, which wrongly changed the *SPS* flat-16 default;
here it is the *PPS* fall-back that was wrong. FFmpeg (the reference) applies
rule set A.

128x96, High, CABAC, 8x8 transform, B-frames, 6 frames. x264 does not emit this
shape, so it is produced by encoding a flat-CQM stream and bit-patching the PPS
(set the flag to 1, insert the eight absent-list flags), which leaves the encoded
coefficients dequantized under rule A instead of flat-16:

    ffmpeg -f lavfi -i testsrc2=size=128x96:rate=25 -frames:v 6 \
      -c:v libx264 -profile:v high -preset veryslow -pix_fmt yuv420p \
      -x264-params 8x8dct=1:cabac=1:bframes=1:keyint=6:no-scenecut=1:cqm=flat \
      -f h264 syn.264
    # then flip PPS pic_scaling_matrix_present_flag 0->1 and insert 8 zero
    # pic_scaling_list_present_flag bits (see patch_pps_scaling.py in the fix notes)

Without the fix this line FAILs (the base hash equals the flat-16 decode, i.e. the
un-patched stream's output); with it, it matches the FFmpeg-anchored rule-A hash.
