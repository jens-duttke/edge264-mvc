Liveness regression fixtures (committed, <1 MB each).

These guard against decode-stall, deadlock, or abort bugs that a hash-based
comparison cannot express (a stalled or aborting decoder never reaches a
comparable hash). Each fixture is decoded with a progress guard; the harness
(tests/liveness_check.c, target `make check-liveness`, also run by `make check`)
asserts it delivers the expected number of base-view frames without stalling,
and that the decoder rejects the expected number of NALs (the third column of
the manifest, 0 when left out), and, where a fourth column gives one, that the
base view's samples hash to it. Like a player, the harness skips a rejected NAL,
sends the next one and always ends and drains the stream. Each fixture runs in a
forked child under a wall-clock timeout,
so a deadlock where `decode_nal` itself never returns (which the
in-process progress guard cannot catch) is reported as a clean "deadlock" FAIL
instead of hanging the whole suite, and an assert-abort or crash as a "crashed"
FAIL. The liveness suite is also run
multithreaded (`EDGE264MVC_THREADS=8` and `-1`), where these deadlocks surface.

- mvc_unpaired_base.264: derived from tests/conformance/mvc/MVCDS-4.264 by
  removing exactly one dependent-view coded-slice NAL (nal_unit_type 20). One
  base frame thus loses its POC-matching dependent. ffmpeg decodes the full
  9-frame base view of this stream; edge264 must too (emitting the unpairable
  base alone with zeroed _mvc), not deadlock. Regresses bug M1 (edge264mvc.c
  get_frame MVC pairing) if the liveness valve is removed.

- dpb_frame_num_gap.264: a frame_num gap (8.2.5.2) where every reference slot
  is already long-term, so no short-term slot can be reclaimed for the inferred
  non-existing frames. edge264 used to abort on assert(sref_slots > 0); it now
  rejects the non-conformant frame with EBADMSG (ffmpeg likewise only reports an
  error). The picture before the gap is delivered and the one after it rejected:
  1 frame, 1 rejected NAL; a regressed assert FAILs as "crashed".

- tall_progressive.264: a synthetic single progressive IDR of 1x540 MBs
  (16x8640px), generated with tests/gen_avc.py. Its height exceeds the 528-row
  ceiling that the SPS parser wrongly imposed by evaluating the bound
  "527 << frame_mbs_only_flag" before frame_mbs_only_flag is read (so it was
  always 527 << 0). The clamp mis-sized the frame and the DPB never delivered
  the picture, leaving the decoder spinning on ENOBUFS. ffmpeg decodes the full
  16x8640 frame, and the fixed decoder's output is byte-identical to ffmpeg's
  (207360-byte YUV420p); edge264 must deliver that 1 frame, not stall.
  Regresses bug L2 (edge264mvc_headers.c parse_seq_parameter_set height bound).

- zero_ref_idr.264: a synthetic single all-intra IDR (16x16), generated with
  tests/gen_avc.py with max_num_ref_frames=0 in the SPS - the case x264 emits
  for single-frame / all-intra clips, reproduced without x264 so the fixture
  carries no embedded encoder banner. The IDR is a reference picture
  (nal_ref_idc>0) and 8.2.5.1 marks it used-for-reference, so the reference set
  is 1 while the limit is 0. That tripped the C.4.5 invariant assert in
  parse_slice_layer_without_partitioning, aborting the process. ffmpeg decodes
  the single frame, and the fixed decoder's output is byte-identical to ffmpeg's
  (384-byte YUV420p); edge264 must deliver that 1 frame. Regresses the zero-ref
  fix (edge264mvc_headers.c parse_seq_parameter_set max_num_ref_frames floor) if
  the floor is removed.

- dpb_overflow.264: a synthetic single all-intra IDR (20x20 = 400 MBs / 320x320px),
  generated with tests/gen_avc.py, whose SPS signals level 1.0 (MaxDpbMbs 396).
  Because 400 > 396, the inferred MaxDpbFrames = 396/400 = 0 and so the derived
  max_dec_frame_buffering = 0, yet the IDR is a reference picture occupying one DPB
  slot. The fullness assert in parse_slice_layer_without_partitioning
  (edge264mvc_headers.c, C.4.5) then sees 1 > 0 and aborts the process during slice-
  header parsing, before any macroblock is decoded. ffmpeg decodes the single
  frame of this over-level clip; edge264 must deliver that 1 frame too. Regresses
  the DPB-buffering floor (edge264mvc_headers.c parse_seq_parameter_set, where the
  derived MaxDpbFrames is floored at the reference count) if the floor is removed.

- incomplete_frame.264: a synthetic IDR (30 bytes), generated with tests/gen_avc.py,
  whose SPS declares a 2-macroblock picture (2x1 MBs) but whose only coded slice
  carries just 1 macroblock. The picture therefore never completes
  (remaining_mbs > 0 and next_deblock_addr stays != INT_MAX). get_frame
  skips not-yet-deblocked pictures, which is correct mid-stream but at end-of-stream
  deadlocked: bump_all_frames kept returning ENOBUFS while the draining caller got
  nothing back, spinning forever. The fix lets get_frame emit such a picture while
  dec->flushing (the same forward-progress valve the MVC unpaired-base path uses),
  so the decoder delivers the partial picture (1) and terminates. This is the class
  of real captured TS/M2TS clips that end mid-frame - ffmpeg conceals the partial
  picture and terminates likewise. Regresses the end-of-stream forward-progress
  valve (edge264mvc.c get_frame) => stall.

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
  (edge264mvc_headers.c parse_seq_parameter_set) => EBADMSG / 0 frames.

- cabac_overread.264: 24 single-macroblock I_PCM pictures, synthesized by
  tests/gen_cabac_overread.py (a minimal standalone CABAC encoder, since tests/gen_avc.py
  emits CAVLC only). Each slice decodes its whole picture, but raw 0xFF
  bytes placed after the PCM samples leave the re-initialised arithmetic engine with
  offset >= range (so end_of_slice reads 1) AND a non-zero msb_cache. That non-clean
  trailing is exactly what a dense final CABAC slice leaves when its coded data fills the
  NAL right up to the next start code (the cached reader looks ahead past the slice's last
  byte) - benign, since every macroblock decoded. The strict cabac end check used to trip
  EBADMSG on these COMPLETE slices, so worker_loop never zeroed their remaining_mbs and the
  pictures never finalized; they accumulate undelivered until the DPB overflows and the
  decoder spins ENOBUFS *mid-stream* (before end-of-stream, so the end-of-stream
  forward-progress valve cannot mask it - the decoder delivers 0 frames). The fix ignores
  the trailing slop on a slice whose CurrMbAddr reached the picture end, so all 24 are
  delivered. ffmpeg decodes them too. Found on a real 4K capture whose
  4-slice CABAC frames hit this on some of their final slices. Regresses the cabac
  end-of-slice over-read tolerance (edge264mvc_headers.c worker_loop) => mid-stream stall.

- cabac_orphan.264: an IDR followed by 23 pictures (synthetic CABAC,
  tests/gen_cabac_orphan.py; 1x2 = 2-MB pictures), where picture 1 codes only 1 of its
  2 macroblocks - its slice ends via end_of_slice mid-picture, so remaining_mbs stays > 0
  and it never finalizes (the state a corrupt stream leaves when a slice errors mid-frame).
  The SPS VUI sets max_num_reorder_frames = 0, so each complete picture is output
  immediately; the held incomplete picture has the lowest pending POC, so it is bumped into
  the 16-entry output queue but skipped by get_frame (an unfinished picture is held
  back mid-stream). The following complete higher-POC pictures ARE delivered, so they keep
  bumping and shift the unfinished one out of the queue. Orphaned (still in to_get_frames
  but no longer queued), it made bump_all_frames return ENOBUFS forever at end-of-stream:
  the decoder delivered 23 of 24 and stalled. The fix finalizes and re-queues such an orphan
  so the drain terminates and all 24 are delivered - ffmpeg likewise conceals and emits the
  damaged picture. Found on a real corrupt broadcast capture.
  Regresses the flush-drain orphan recovery (edge264mvc_headers.c bump_all_frames) => stall
  losing the last picture.

- cabac_all_incomplete.264: 24 unfinished two-macroblock pictures generated by
  `python3 tests/gen_cabac_orphan.py tests/liveness/cabac_all_incomplete.264 allincomplete 24`.
  The uncoded lower macroblock is cropped from the displayed frame, leaving a
  deterministic 16x16 I_PCM output. Sixteen pictures used to fill the DPB while
  none was ready for normal output, until the tool's caller-side progress guard
  forced an end-of-stream drain after repeated zero-progress `ENOBUFS` (the guard
  must set `flushing` before feeding the sentinel; otherwise the normal NAL
  fullness gate rejects the sentinel forever) and the last 8 pictures were lost.
  Each unfinished picture is now concealed as soon as no task writes it and the
  next picture has started, keeping its decoded macroblocks, so all 24 are
  delivered like ffmpeg does. The focused harness checks all 24 cropped output
  frames through both regular-file and stdin paths, in single- and multithreaded
  modes.

- cabac_misalign.264: 24 single-MB I_PCM pictures (synthetic CABAC, tests/gen_cabac_misalign.py)
  whose slice headers pad cabac_alignment_one_bit with ZEROS instead of ones. The spec writes
  that padding as 1s, but it is non-normative - it carries no decodable information and ffmpeg
  does not verify it, it just byte-aligns and starts the arithmetic engine. edge264's cabac_start
  used to reject a non-1 padding with EBADMSG and decode 0 macroblocks; the undelivered pictures
  then piled up until the DPB overflowed into a mid-stream stall. The fix accepts the byte-aligned
  position regardless, so every picture decodes (the CABAC data is byte-aligned and valid). Found
  on a real Extended-profile capture whose every slice was rejected: edge264 stalled at
  0 frames, now decodes every picture like ffmpeg. Regresses the cabac_alignment leniency
  (edge264mvc_bitstream.c cabac_start) => mid-stream stall delivering 0 frames.

- mvc_baseless_dependent.264: a subset SPS (NAL type 15) plus 18 inter-coded dependent-view
  slices (NAL type 20, P) with NO base-view SPS (type 7) and no base-view slices - a stream
  whose base view is undecodable (reproduced from FFmpeg's public
  3D/AVC_codec_in_m2ts_not_recognized sample, which ffmpeg rejects with "sps_id out of range",
  resolving width=0/height=0 and no frame), generated by tests/gen_mvc_baseless.py. With no base
  picture ever created (dec->basePic stays -1), each dependent P slice's inter-view reference
  falls back through the out-of-range RefPicList fix-up to the slice's own not-yet-decoded frame
  slot, so its decode task depends on its own picture. Multithreaded, that dependency never
  clears: the worker never runs the task and the frame never completes. The 18 slices code one
  picture (> the 16 task slots), so the self-dependent tasks exhaust the pool and
  parse_slice_layer_without_partitioning blocks forever waiting for a free task slot -
  decode_nal never returns. This is a true internal deadlock, not an ENOBUFS spin, so
  the in-process progress guard cannot catch it; the harness therefore decodes each fixture in a
  forked child under a wall-clock timeout and reports an overrun as a clean "deadlock" FAIL. The
  fix rejects each of the 18 base-less inter-coded dependent slices as corrupt (EBADMSG) and delivers 0
  frames, like ffmpeg. Regresses the base-less dependent-view guard
  (edge264mvc_headers.c parse_slice_layer_without_partitioning) => deadlock (only visible
  multithreaded; the synchronous path force-runs the task and does not hang).

- mvc_orphan_dep_tail.264: a two-view body (1 stereo IDR + 3 stereo P access units, 16x16 MBs,
  residual-free) followed by 32 dependent-view-only tail access units with NO base view,
  generated by tests/gen_mvc_orphan_tail.py - the shape a byte-trimmed 3D Blu-ray SSIF leaves
  behind (its chunked interleaving ends with a surplus of dependent AUs past the last base AU;
  an unequal-length two-file base+dependent feed produces the same). The tail alternates
  reference / non-reference dependents: the reference ones chain P-slice dependencies from
  picture to picture, the non-reference ones take the immediate-output bump into the dependent
  output queue - the only queueing path without a paired base - which exposes them to
  get_frame's orphan-dependent valve; each tail picture is split into 4 uneven slices (a large
  first slice) so its worker tasks are reliably still running when the valve fires. Pre-fix,
  multithreaded decoding deadlocked on ~97% of runs of this fixture (occasionally crashing
  instead): the valve dropped a dependent that was still being parsed/decoded, the parser
  reallocated the freed DPB slot for the next picture, and the stale tasks' remaining_mbs
  subtractions corrupted the new occupant's counter - the frame never finalized, every later
  task depending on it stayed un-ready, and once all 16 task slots filled the parser blocked
  forever in its task-slot wait inside decode_nal (the deadlock diagnosed on a real
  trimmed 3D-BD capture, where dependent-view slices also started failing with spurious EBADMSG
  and remaining_mbs went negative). Single-threaded decoding of the same bytes was always fine.
  The fix is two guards: get_frame's orphan valve defers the drop until the dependent has no
  in-flight tasks and is no longer the picture being parsed, and the DPB slot allocator
  additionally treats any slot still written by a busy task as unavailable (inflight_frames).
  Expected: the 4 body stereo pairs (4 base frames); every tail dependent is dropped (it has no
  base to pair with), like ffmpeg, and the decoder terminates. Regresses either guard
  (edge264mvc.c get_frame orphan valve, edge264mvc_headers.c
  parse_slice_layer_without_partitioning slot allocation) => deadlock or crash (multithreaded).

- incomplete_ref_dependency.264 and incomplete_ref_dependency_eos.264: 177-byte and
  114-byte two-picture CAVLC streams generated through:

      python3 tests/gen_incomplete_ref_dependency.py /tmp/incomplete.yaml 17
      python3 tests/gen_avc.py /tmp/incomplete.yaml tests/liveness/incomplete_ref_dependency.264

  (use width 10 and the `_eos` output name for the second fixture). Their reference IDR ends
  cleanly after one macroblock, leaving `remaining_mbs > 0` after its only task exits. Every
  slice of the following picture references that incomplete IDR. With width 17, the first 16
  P tasks fill the fixed task pool and the parser blocks reserving the 17th. With width 10,
  EOS/flush waits on ten permanently pending tasks before the pool can saturate. In both cases
  every worker sleeps on `task_ready`, and no task targets the IDR any longer. The quiescent
  scheduler valve deterministically conceals only dependency slots with no possible writer,
  recomputes `ready_tasks`, and lets both pictures drain. Pre-fix the forked liveness harness
  reports a hard deadlock after 15 seconds; post-fix both fixtures deliver 2 frames in
  single-thread, 8-thread, and auto-thread modes. These minimize the scheduler/DPB condition
  from a long MVC decode where an asynchronous slice error orphaned a base-view reference.
  Regresses `progress_or_wait` => hard MT deadlock.

- mvc_truncated_dependent.264: the first 57250 bytes of
  `tests/conformance/mvc/MVCDS-4.264` (`head -c 57250`), which ends in the middle
  of the last dependent-view slice. At end-of-stream that dependent picture is
  incomplete with no task left to finish it, and its base view was held for it
  forever: `get_frame` only paired a base with a complete dependent, so the
  drain returned `ENOBUFS` without end. `bump_all_frames` now conceals every
  incomplete picture once no task writes it, before the drain - which also makes
  the emitted pixels independent of the DPB slot the picture got (the undecoded
  part used to show whatever the slot held), so single- and multithreaded
  decoding agree.

- self_reference.264: a copy of `tests/asan/reflist_oob.264` (a leading B slice
  with no reference picture), run under the liveness harness and with threads.
  The out-of-range `RefPicList` fix-up replaced the missing references with slot
  0, which is the current picture: single-threaded the slice read its own
  undecoded samples, multithreaded its task waited forever on its own decoding
  progress. `parse_ref_pic_list_modification` now never substitutes (nor keeps)
  the current picture, and rejects a slice that has no other picture to refer to
  (0 frames, 1 rejected NAL).

- reordered_damaged_slices.264: a 417-byte stream generated with
  tests/gen_reordered_damaged_slices.py - one 176x160 IDR picture coded as ten
  slices of one macroblock row each, sent in the order 1..9, 0 with the last
  byte of slices 1..9 cut off. A damaged slice does not defer its deblocking
  turn, and `slice_turn` let it wait for any busy slice of its picture with a
  lower `first_mb_in_slice` - including slice 0, which arrived after it and
  which no worker could start while all 8 of them waited this way, so
  `edge264mvc_send_nal` never returned. A slice now waits only for slices
  decoded before it, as decoding single-threaded, and the picture is concealed
  and delivered (1 frame).

- mvc_requeue_dependent.264: a 1x1-macroblock MVC stream generated with
  tests/gen_mvc_requeue_dependent.py (from the .yaml next to it): 8 paired
  reference access units, 16 base pictures without dependent view, an access
  unit whose dependent view comes before its base view and takes the last free
  DPB slot, then 20 base pictures. The base view of that access unit finds the
  DPB full, so `make_room` runs `bump_all_frames`, whose re-slot loop queued the
  waiting dependent view without marking it for output. A caller receiving one
  frame per round (`tests/partial_receive_check.c`, run by `make check`) then
  got it queued a second time and delivered as a pair, tripping the assertion
  in `get_frame`, or, with dependent views after it, saw the slot of a held
  dependent view reused. The loop now marks every picture it queues.

- rejected_first_slice.264: a 32x32 IDR picture, a reference P picture of two
  slices of I_PCM samples whose first slice modifies its reference list to a
  picture that does not exist, and a P picture of skipped macroblocks
  (tests/gen_rejected_first_slice.py). The decoder rejects the first slice before
  its reference marking runs; the second slice still belongs to the same picture
  (7.4.1.2.4), but the decoder compared its nal_ref_idc with whether the marking
  had run and opened a new picture for it, with the same POC, which never came
  out, so the decoded half was lost and the next picture predicted from the
  concealed one. 3 frames, 1 rejected NAL; the hash pins the decoded bottom half
  in the P pictures.

- duplicate_last_slice.264: 32x16 pictures of two I_PCM macroblocks coded as two
  slices each, the second slice of the IDR picture sent twice, with
  `max_num_reorder_frames = 0` (tests/gen_duplicate_last_slice.py). The copy starts
  at a macroblock the first one decoded, so it ends there without decoding any.
  It still flagged its picture as concealed, though there was nothing to conceal:
  single-threaded the picture was already output when the copy ran, with worker
  threads usually not, so the flag differed between the two (`make check-stress`
  compares the flags of every run). 2 frames, no rejected NAL.
