Liveness regression fixtures (committed, <1 MB each).

These guard against decode-stall/deadlock bugs that a hash-based comparison
cannot express. Each fixture is decoded with a progress guard; the harness
(tests/liveness_check.c, target `make check-liveness`, also run by `make check`)
asserts it delivers the expected number of base-view frames without stalling.

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
