Liveness regression fixtures (committed, <1 MB each).

These guard against decode-stall, deadlock, or abort bugs that a hash-based
comparison cannot express (a stalled or aborting decoder never reaches a
comparable hash). Each fixture is decoded with a progress guard; the harness
(tests/liveness_check.c, target `make check-liveness`, also run by `make check`)
asserts it delivers the expected number of base-view frames without stalling
(an assert-abort regression instead crashes the harness, which `make` reports as
a failed target).

- mvc_unpaired_base.264: derived from tests/conformance/mvc/MVCDS-4.264 by
  removing exactly one dependent-view coded-slice NAL (nal_unit_type 20). One
  base frame thus loses its POC-matching dependent. ffmpeg decodes the full
  9-frame base view of this stream; edge264 must too (emitting the unpairable
  base alone with zeroed _mvc), not deadlock. Regresses bug M1 (edge264.c
  edge264_get_frame MVC pairing) if the liveness valve is removed.

- dpb_frame_num_gap.264: a frame_num gap (8.2.5.2) where every reference slot
  is already long-term, so no short-term slot can be reclaimed for the inferred
  non-existing frames. edge264 used to abort on assert(sref_slots > 0); it now
  rejects the non-conformant frame with EBADMSG (ffmpeg likewise only reports an
  error). Expected 0 delivered frames; a regressed assert aborts the harness.

- tall_progressive.264: a synthetic single progressive IDR of 1x540 MBs
  (16x8640px), generated with tests/gen_avc.py. Its height exceeds the 528-row
  ceiling that the SPS parser wrongly imposed by evaluating the bound
  "527 << frame_mbs_only_flag" before frame_mbs_only_flag is read (so it was
  always 527 << 0). The clamp mis-sized the frame and the DPB never delivered
  the picture, leaving the decoder spinning on ENOBUFS. ffmpeg decodes the full
  16x8640 frame, and the fixed decoder's output is byte-identical to ffmpeg's
  (207360-byte YUV420p); edge264 must deliver that 1 frame, not stall.
  Regresses bug L2 (edge264_headers.c parse_seq_parameter_set height bound).

- zero_ref_idr.264: a synthetic single all-intra IDR (16x16), generated with
  tests/gen_avc.py with max_num_ref_frames=0 in the SPS - the case x264 emits
  for single-frame / all-intra clips, reproduced without x264 so the fixture
  carries no embedded encoder banner. The IDR is a reference picture
  (nal_ref_idc>0) and 8.2.5.1 marks it used-for-reference, so the reference set
  is 1 while the limit is 0. That tripped the C.4.5 invariant assert in
  parse_slice_layer_without_partitioning, aborting the process. ffmpeg decodes
  the single frame, and the fixed decoder's output is byte-identical to ffmpeg's
  (384-byte YUV420p); edge264 must deliver that 1 frame. Regresses the zero-ref
  fix (edge264_headers.c parse_seq_parameter_set max_num_ref_frames floor) if
  the floor is removed.

- dpb_overflow.264: a synthetic single all-intra IDR (20x20 = 400 MBs / 320x320px),
  generated with tests/gen_avc.py, whose SPS signals level 1.0 (MaxDpbMbs 396).
  Because 400 > 396, the inferred MaxDpbFrames = 396/400 = 0 and so the derived
  max_dec_frame_buffering = 0, yet the IDR is a reference picture occupying one DPB
  slot. The fullness assert in parse_slice_layer_without_partitioning
  (edge264_headers.c, C.4.5) then sees 1 > 0 and aborts the process during slice-
  header parsing, before any macroblock is decoded. ffmpeg decodes the single
  frame of this over-level clip; edge264 must deliver that 1 frame too. Regresses
  the DPB-buffering floor (edge264_headers.c parse_seq_parameter_set, where the
  derived MaxDpbFrames is floored at the reference count) if the floor is removed.

- incomplete_frame.264: a synthetic IDR (30 bytes), generated with tests/gen_avc.py,
  whose SPS declares a 2-macroblock picture (2x1 MBs) but whose only coded slice
  carries just 1 macroblock. The picture therefore never completes
  (remaining_mbs > 0 and next_deblock_addr stays != INT_MAX). edge264_get_frame
  skips not-yet-deblocked pictures, which is correct mid-stream but at end-of-stream
  deadlocked: bump_all_frames kept returning ENOBUFS while the draining caller got
  nothing back, spinning forever. The fix lets get_frame emit such a picture while
  dec->flushing (the same forward-progress valve the MVC unpaired-base path uses),
  so the decoder delivers the partial picture (1) and terminates. This is the class
  of real captured TS/M2TS clips that end mid-frame - ffmpeg conceals the partial
  picture and terminates likewise. Regresses the end-of-stream forward-progress
  valve (edge264.c edge264_get_frame) => stall.

- vui_overread.264: a synthetic SPS+PPS+IDR (43 bytes) generated with tests/gen_avc.py,
  with the SPS NAL's last 2 bytes trimmed afterwards so its VUI over-reads past the SPS
  rbsp - the common encoder bug ffmpeg reports as "Overread VUI by N bits" and decodes
  through anyway. edge264's strict rbsp_trailing_bits check rejected the whole SPS with
  EBADMSG, so the entire stream produced 0 frames. The VUI is the last, non-normative
  element and every decoding-relevant field before it is already parsed and bounds-checked,
  so the fix accepts the SPS (reverting the VUI's max_num_reorder_frames /
  max_dec_frame_buffering to the inferred defaults) and decodes the IDR. Two real captures
  (a Main and a High clip) hit this - ffmpeg flags them "Overread VUI by 8 bits" too and
  decodes them. edge264 must deliver the 1 frame. Regresses the VUI-overread SPS tolerance
  (edge264_headers.c parse_seq_parameter_set) => EBADMSG / 0 frames.
