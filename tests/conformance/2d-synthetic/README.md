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
under `EDGE264MVC_THREADS`); with it, single- and multi-thread both match the
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

## `slice_deblock_offsets.264`

Guards the per-slice deblocking parameters under multithreading. The deblocking
of a macroblock uses `slice_alpha_c0_offset_div2` and `slice_beta_offset_div2`
of the slice containing it (8.7). Slices of one picture are decoded in parallel,
and a slice whose predecessor was still being decoded used to leave its
macroblocks to whichever slice completed the picture, which deblocked them with
its own offsets: **wrong pixels that vary from run to run** whenever slices
carry different offsets, while single-threaded decoding stayed correct.

352x288, High, CABAC, all-intra (every picture an IDR), 4 slices per picture,
30 frames; the odd slices of each picture get offsets of +3/+3 instead of the
encoded 0/0. Reproduce:

    ffmpeg -f lavfi -i testsrc2=size=352x288:rate=25 -frames:v 30 \
      -c:v libx264 -profile:v high -pix_fmt yuv420p \
      -x264-params keyint=1:slices=4:qp=34:deblock=0,0 -f h264 base.264
    python3 tests/gen_slice_deblock_offsets.py base.264 slice_deblock_offsets.264

Without the fix the multithreaded passes of this fixture's line FAIL (wrong and
nondeterministic hash); with it, single- and multi-thread both match the
FFmpeg-anchored hash.

## `gap_reference.264`

Guards the content of frames inferred for a `frame_num` gap (8.2.5.2). A 176x144 libx264 encode of `testsrc2` with one reference frame and no B-frames, with picture 6 removed (`tests/gen_gap_reference.py`, which also lists the encoding command), so picture 7 predicts from the inferred frame. The decoder allocated that frame without writing its samples or macroblocks, so the pictures after the gap showed whatever the reused DPB slot held before - a different picture single- and multithreaded, and varying from run to run. FFmpeg, which this line is anchored to, fills the inferred frame with the previous reference picture.

Without the fix this line FAILs (wrong base hash, single- and multithreaded).

## `overlapping_slices.264`

Guards the handling of slices that overlap (non-conformant, 7.4.3 `first_mb_in_slice`), as a stream decoded with a picture parameter set that does not match its slices produces. Ten 1920x1080 IDR pictures, each with a slice A covering the whole picture in flat gray and a slice B covering 16 macroblocks with I_PCM samples of 200, starting in turn at the last 16 macroblocks and at macroblocks 1, 121, 4000 and 2 (`tests/gen_overlapping_slices.py`). Where slice B starts early, a worker thread that began slice A before the parser saw slice B usually decodes past B's start, and has to decode slice A again with its end known. The decoder now ends every slice where the next slice of its picture starts, as FFmpeg does with `next_slice_idx`, so slice B keeps its macroblocks and slice A is cut before them. Before, the overlap went to whichever slice claimed a macroblock first: slice A when decoding single-threaded, often slice B with worker threads. The hash is anchored to the single-threaded decode of the fixed decoder, which multithreaded decoding must match; FFmpeg resolves this overlap differently again, so it does not serve as the oracle.

Without the fix this line FAILs single-threaded (slice A wins) and, depending on the timing, multithreaded.

## `reversed_slices.264`

Guards slices of one picture that arrive out of address order. The pictures of `slice_deblock_offsets.264` with their four slices sent in reverse order (`tests/gen_reversed_slices.py`); arbitrary slice order is legal only in Baseline profile, so for this CABAC stream it is damaged input. Deblocking runs over the whole picture in macroblock order, each macroblock with the filter offsets of its own slice (8.7, 8.7.2.2), so the order of the slices does not change the output, and the hash is the one of `slice_deblock_offsets.264`, which is anchored to FFmpeg. A slice used to wait for any busy slice of its picture with a lower `first_mb_in_slice`, including one that arrived after it, so with worker threads the output varied from run to run; a slice now waits only for slices decoded before it. And a slice decoded before a lower one was deblocked with the offsets of the slice that completed the picture; every macroblock now keeps the offsets of its slice.

Without the fixes this line FAILs, single-threaded and under `EDGE264MVC_THREADS`.

## `overrun_reference.264`

Guards a slice that a worker thread decodes past the start of the next slice before the parser has seen that slice. 128 pairs of a 160x64 IDR picture and a P picture of P_Skip macroblocks copying it (`tests/gen_overrun_reference.py`). Each IDR picture has three slices with deblocking on: the second one runs to the end of the picture, and the third one starts a few macroblocks after it, in the first or the second macroblock row; their macroblocks are Intra_16x16 with a DC that changes from one macroblock to the next, so that deblocking across slice edges changes samples. `tests/slice_overrun_check.c` feeds the stream with a pause before every slice that does not start a picture, so that the second slice is decoded, deblocked and published to the end of the picture before its end is known, and the P picture follows the third slice at once. The second slice then has to take back what it published past its end, and restore the samples of the first slice that its deblocking changed, before the P picture reads them. The parser now waits for that before it starts the next picture (`settle_mb_bounds`), a slice waiting for a reference picture learns its end there too, and only the samples of the slices before it are restored, not those of a slice decoding the same rows at the same time. The hash is anchored to the single-threaded decode; FFmpeg resolves the overlap differently.

Without the wait, or without the restored samples, most multithreaded runs of `slice_overrun_check` differ from the single-threaded output.

## `plane_without_d.264`

Guards Intra_16x16 and chroma Plane prediction where the macroblock above-left of the current one (D) is not available. Three 1920x1080 IDR pictures of two slices each (`tests/gen_plane_without_d.py`): the first slice ends with an I_PCM macroblock in the middle of a row, the second one starts after it, and its macroblock one row below that start signals Intra_16x16 Plane prediction for luma and chroma, with its left and upper neighbours in its own slice but its above-left neighbour in the first slice. Plane prediction needs that sample (8.3.3.4, 8.3.4.4), and a macroblock of another slice is not available for intra prediction, so the stream is non-conformant; the JM reference decoder stops on it. The decoder used to select Plane prediction from the availability of the left and upper neighbours alone and read the sample anyway: single-threaded it got the I_PCM sample, with worker threads usually whatever was there before the first slice wrote it, so the output varied from run to run. Plane prediction now falls back to DC prediction from the left and upper neighbours when D is not available, as it already did when one of those is missing. The hash is anchored to the single-threaded decode; FFmpeg reads the sample of the other slice and differs.

Without the fix this line FAILs single-threaded, and multithreaded decoding differs from the single-threaded output from run to run.

## `weighted_denom7.264`

Guards explicit weighted bi-prediction at the largest weight denominator (`luma_log2_weight_denom` and `chroma_log2_weight_denom` 7). A 64x32 IDR picture and a P picture of I_PCM macroblocks with sample gradients over the whole 0..255 range are the two references, and eight B pictures between them, every macroblock skipped (direct spatial prediction, so each one blends both references), carry weight tables near the limits of 7.4.3.2: bright samples blended with weights summing to 127 and large offsets, negative weights and offsets, and the weight of 128 inferred when none is signalled, paired with odd negative weights (`tests/gen_weighted_denom7.py`). The SIMD blends fold the offsets into the weighted sum before the shift, a 16-bit sum that saturates at this denominator, so bright samples came out as at most 127; and as 128 does not fit their 8-bit weights, they halved both weights, putting an odd weight paired with it off by one. Bi-prediction at denominator 7 is now blended apart in plain C, as in 8-276 (`blend_wide`), which leaves the SIMD blends of all other weighted prediction as they were. The hash is anchored to the JM reference decoder. FFmpeg's C code gives the same output (`ffmpeg -cpuflags 0 -i weighted_denom7.264 -f rawvideo -pix_fmt yuv420p weighted_denom7.yuv`), its SIMD code does not.

Without the fix this line FAILs.

## `crop_top_left.264`

Three 64x48 IDR pictures of I_PCM macroblocks with sample gradients, cropped by 8 samples on every side to 48x32 (`tests/gen_crop_top_left.py`), so that the macroblocks along every edge lie partly outside the output picture. Here it pins the cropped output, anchored to FFmpeg's decode; `tests/edge264mvc_test_yuv_check.py` also uses it to check that `edge264mvc_test` reports a differing sample at each corner of the picture. Printing such an edge macroblock used to read the reference YUV outside the picture, before its start for the top rows, so `edge264mvc_test` crashed on a difference in the first frame instead of reporting it; it now prints the cropped-off samples as blanks without reading them.

## `cavlc8x8_deblock.264`

Guards the boundary strength of edges next to a CAVLC macroblock with an 8x8 transform in a slice without deblocking. A flat 32x32 IDR picture of I_PCM macroblocks, then three High profile CAVLC P pictures of two slices each (`tests/gen_cavlc8x8_deblock.py`): the first macroblock row with `disable_deblocking_filter_idc` 1, holding an inter macroblock with an 8x8 transform whose bottom-left 8x8 block carries only a DC coefficient, the second row with idc 0 and skipped macroblocks. CAVLC codes the 8x8 block as four interleaved 4x4 blocks, and the DC lands in the first of them, not in its bottom row; 8.7.2.1 still filters the edge below the 8x8 block with boundary strength 2, as the block has a coefficient. The decoder used to turn the per-4x4 counts into per-8x8 flags only when deblocking a macroblock, so the neighbour in the slice without deblocking kept its per-4x4 counts and the edge was filtered with strength 0; the flags are now recorded when the macroblock is parsed. The hash is anchored to the JM reference decoder, which FFmpeg matches.

Without the fix this line FAILs.

## `intra8x8_hd_slices.264`

Guards the samples intra 8x8 Horizontal_Down prediction reads. Four High profile IDR pictures of 8x4 macroblocks, one slice per macroblock, each macroblock Intra_8x8 without residual: its first three 8x8 blocks predicted DC, its bottom-right one Horizontal_Down (`tests/gen_intra8x8_hd_slices.py`). Horizontal_Down uses no sample right of its block (8.3.2.2.8), but its load reached the seven samples there, in the next macroblock, which another worker thread decodes at the same time as the next slice - a data race reported by ThreadSanitizer (the samples were never used, so the output did not change). The CI ThreadSanitizer job decodes it with 8 worker threads through `tests/slice_overrun_check.c`. The hash is anchored to the JM reference decoder, which FFmpeg matches.

## `long_term_overflow.264`

Guards the reference marking of a stream that holds more references than `max_num_ref_frames`. A 32x32 stream allowing one reference picture, whose IDR picture is marked long-term, so the sliding window (8.2.5.3) never retires it; each of the following reference P pictures then adds a short-term reference beside it (`tests/gen_long_term_overflow.py`). The two P pictures of I_PCM samples come in two slices each, and a P picture of skipped macroblocks after each shows which reference was kept. Such a stream is not conformant; it aborted on a C.4.5 assert, and a release build kept one reference more than the stream declared. One reference is now discarded as FFmpeg does - here the current picture, the only short-term one - and the picture still counts as a reference for frame_num and for telling its slices apart from the next picture. The hash is anchored to FFmpeg. The JM reference decoder rejects the stream.

Without the fix this line FAILs.

## `idr_after_long_term.264`

Guards the frame_num of an IDR picture. A 32x32 stream allowing one reference picture whose references are all long-term: an IDR picture marked long-term, then two reference P pictures that mark themselves long-term with MMCO 6, followed by a second IDR picture and a P picture of skipped macroblocks predicting from it (`tests/gen_idr_after_long_term.py`). The decoder counts frame_num on across IDR pictures, which continue at the next multiple of MaxFrameNum, and it took that jump for a frame_num gap (8.2.5.2, which concerns non-IDR pictures only); with every reference slot long-term there was no room for the frames inferred for the gap, and the IDR picture and the P picture after it were rejected as corrupt. The hash is anchored to the JM reference decoder, which FFmpeg matches.

Without the fix this line FAILs.

## `cavlc_ac_overflow.264`

Guards where a damaged CAVLC AC block puts a coefficient that lies past its scan. A 32x16 High profile IDR picture of two macroblocks (`tests/gen_cavlc_ac_overflow.py`): an Intra_16x16 macroblock whose first luma AC block, of 15 coefficients, codes one coefficient with total_zeros 15 - a value the table only allows for blocks of 16 coefficients (9.2.3) - and an Intra_8x8 macroblock with a coded 8x8 block without coefficients. The coefficient one past the scan used to be stored outside the 4x4 block, where the next 8x8 block of the slice picked it up, so the intact macroblock beside the damaged one got a pattern it does not code. As with FFmpeg's scan tables, it now lands on the block's own DC position, which the DC of the Intra_16x16 macroblock replaces; the same holds for chroma AC blocks and for an AC block whose coeff_token claims 16 coefficients. The hash is anchored to FFmpeg. The JM reference decoder keeps the coefficient in place of the block's DC instead.

Without the fix this line FAILs.

## `conceal_wide_islice.264`

Guards the concealment of a damaged I slice on x86. Two CAVLC IDR pictures of 140x1 I_PCM macroblocks with deblocking, each a single slice that codes one macroblock more than the picture has, so that it fails at its end (`tests/gen_conceal_wide_islice.py`). The concealment blends the macroblocks of the last row of a damaged I slice with their DC prediction, each by an error probability that grows from the start of the slice; in a slice this long the first macroblock gets the weight 0. The SSE blend multiplies with signed 8-bit weights, where the 128 for the decoded samples reads as -128, so that macroblock came out with all samples 0, while the NEON and generic builds kept it as decoded; the blend is now skipped for it. Concealment has no reference decoder: the hash is the output of the generic C backend (`-DSIMD=4`), which the SSE and NEON builds match.

Without the fix this line FAILs.

## `implicit_weight_far_poc.264`

Guards the picture order count distances of implicit weighted bi-prediction. A 32x32 Main profile stream with `weighted_bipred_idc` 2: an IDR picture at POC 0 and a P picture at POC 32767, both of I_PCM macroblocks with distinct samples, then a B picture of skipped macroblocks at POC 65534 (`tests/gen_implicit_weight_far_poc.py`). Its reference lists start with the P picture and the IDR picture, so 8.4.2.3.1 uses `tb` = 32767 and `td` = -32767, which 8.2.1 allows, clipped to 127 and -128. The decoder clipped each distance from the B picture to 16 bits before subtracting them, so the 65534 to the IDR picture became 32767 and `td` came out 0, which blends both references equally. The distances are now kept in 32 bits and clipped once. The hash is anchored to the JM reference decoder, which FFmpeg matches.

Without the fix this line FAILs.

## `sps_fewer_refs.264`

Guards the reference set when an SPS lowers `max_num_ref_frames` without an IDR picture. A 32x32 stream whose SPS allows four reference pictures: an IDR picture and three reference P pictures of I_PCM samples, then the same SPS allowing one - a new SPS that 7.4.1.2.1 allows only at an IDR picture - and a non-reference and a reference P picture of skipped macroblocks (`tests/gen_sps_fewer_refs.py`). The four references of the old SPS stayed, so the next picture aborted on the C.4.5 assert on the reference count, and a release build held four references where one is allowed. The oldest references beyond the new limit are now dropped when a slice starts, as the sliding window would. The hash is anchored to the JM reference decoder, which FFmpeg matches.

Without the fix this line FAILs.

## `ref_flip.264`

Guards temporal direct prediction from concealed macroblocks. The ITU conformance stream CABA3_SVA_B with the `nal_ref_idc` of its non-reference slices changed from 0 to 1 (`tests/gen_ref_flip.py`): its B slices then read memory management operations from their other bits, frame_num gaps appear and two slices fail, so parts of pictures are concealed, and later B pictures predict with temporal direct prediction from pictures holding such macroblocks. A concealed macroblock left its reference pictures at 0 rather than -1, so temporal direct prediction took it for a reference to DPB slot 0 and predicted from whatever picture held that slot, instead of from the first picture of the list as 8.4.1.2.3 does for a colocated block without reference. Which picture that was depended on when the caller had received earlier frames, so the decoded pictures differed between callers and with worker threads. Concealed macroblocks now carry no reference, like intra ones. No reference decoder conceals alike, so the hash is the decoder's output; the line is checked under both consumer models.

Without the fix this line FAILs.

## `aux_picture.264`

Guards the auxiliary coded pictures that carry an alpha plane. The ITU conformance stream BA3_SVA_C with a sequence parameter set extension (NAL type 13, `aux_format_idc` 1) after its SPS and, after every picture, a slice of type 19 holding a copy of its slice data (`tests/gen_aux_picture.py`). 7.4.1 lets a decoder ignore both, as an auxiliary picture has no effect on the decoding of the primary ones, and the JM reference decoder and FFmpeg do. edge264-mvc reported them as unsupported: a caller that skips such NAL units, as the API asks, decoded the stream correctly, but the stream counted as unsupported, and `edge264mvc_test` without `-k` stopped at the first of them. Both are now ignored. The output must equal that of BA3_SVA_C, whose hash the line shares; the hash is anchored to the JM reference decoder.

This line passes without the fix too, as the harness skips unsupported NAL units; `make check-test-results` fails without it, where `edge264mvc_test` must count the stream as passed.

## `mono_cabac.264`, `mono_cavlc.264`

Guard monochrome (4:0:0) decoding. The first 10 pictures of the ITU conformance streams HPCAMOLQ_BRCM_B (CABAC) and HPCVMOLQ_BRCM_B (CAVLC), both High profile with 8x8 transforms and scaling matrices, cut by `tests/gen_stream_prefix.py` from the vectors of the FRExt set. edge264-mvc reported `chroma_format_idc` 0 as unsupported. A monochrome stream codes no chroma: no `intra_chroma_pred_mode`, codes 0 to 15 of Table 9-4 (b) for `coded_block_pattern` in CAVLC, no chroma bins in CABAC, no chroma residual, PCM samples or weights. Its pictures come out as 4:2:0 with chroma planes of the neutral value 128, as the JM reference decoder writes them. The CABAC contexts of `coded_block_pattern` and the deblocking strength read the coded blocks of the neighbouring macroblocks, which were only set up for 4:2:0. The hashes are anchored to the JM reference decoder, whose output for both is the first 10 frames of the ITU reference.

Without the fix these lines FAIL.

## `mono_after_420.264`

Guards the switch from 4:2:0 to monochrome within one decoder. The first 2 pictures of the ITU conformance stream HPCALQ_BRCM_B (4:2:0), an end_of_seq NAL and the first 3 pictures of `mono_cabac.264`, both parts cut by `tests/gen_stream_prefix.py`. Both parts share the output format, so the new SPS did not reallocate the frame buffers, and the monochrome pictures reused buffers whose chroma planes still held 4:2:0 samples instead of the neutral value. The same happened to a caller decoding a monochrome stream after a 4:2:0 one with `edge264mvc_flush` in between. A change between 4:0:0 and any other chroma format now counts as a format change; the decoder remembers which of the two its buffers were allocated for, as the end_of_seq NAL (like `edge264mvc_flush`) clears the active SPS. The hash is anchored to the JM reference decoder's output for each part on its own; JM decoding the whole stream writes a 4:2:0 picture output after the change with damaged chroma.

Without the fix this line FAILs.

## `sp_slices_a.264`, `sp_slices_b.264`

Guard SP slices. The first 40 pictures of the ITU conformance streams SP1_BT_A and sp2_bt_b (Extended profile, CAVLC), cut by `tests/gen_stream_prefix.py`; every tenth picture is an SP slice with `sp_for_switch_flag` 0. edge264-mvc reported SP slices as unsupported, and a caller skipping them concealed those pictures and every picture predicted from them. An SP slice parses as a P slice, but its P macroblocks, P_Skip included, do not add a residual to their prediction: every 4x4 block transforms its prediction, adds the scaled levels, requantizes the sum with QS and replaces the prediction with the inverse transform of the result (8.6.1), and the macroblock deblocks as an intra one (8.7.2.1). The pictures hold only P slices, so the output of the prefix is the first 40 frames of the ITU reference.

Without the fix these lines FAIL.

## `fmo_maps.264`

Guards slice groups (FMO). The first 10 pictures of the ITU conformance stream FM1_BT_B (Baseline, CAVLC), cut by `tests/gen_stream_prefix.py`, whose eight PPSs cover the seven slice group map types (8.2.2): interleaved, dispersed, foreground with left-over, box-out, raster scan, wipe and explicit. edge264-mvc reported a PPS with more than one slice group as unsupported, and one with a `pic_parameter_set_id` above 3. A slice of a slice group takes the next macroblock of its group (8.2.2.8), whose neighbours belong to the slice only if they are in the same group and not before its first macroblock, and the picture is deblocked once all its macroblocks are decoded, as its slices interleave. The hash is anchored to the JM reference decoder, whose output for it is the first 10 frames of the ITU reference.

Without the fix this line FAILs.
