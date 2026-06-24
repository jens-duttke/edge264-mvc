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
