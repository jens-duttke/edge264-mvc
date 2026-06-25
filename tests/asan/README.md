AddressSanitizer crafted-bitstream regression fixtures (built/run by `make check-asan`).

The SEI parser (parse_sei) runs ONLY when a log callback is set (otherwise SEI
NALs go to ignore_NAL), so the harness (tests/asan_check.c) decodes these with
a log callback. Built with -fsanitize=address; run under a timeout.

- sei_payloadtype_oob.264: an SEI whose payloadType is 206 (> 205). Guards M2:
  parse_sei indexed payloadType_names[]/parse_sei_message[206] (size 206) and
  used a signed `payloadType <= 205` guard, so an overflowed (negative)
  payloadType also slipped through. ASAN catches the out-of-bounds read if the
  bound check / unsigned typing is removed.
- sei_payloadsize_dos.264: an SEI with an unsupported payloadType and a huge
  payloadSize. Guards M5: the unsupported-payload skip loop ran payloadSize
  times regardless of the bytes left in the NAL (a multi-second CPU-burn on a
  crafted stream). The fix stops it at end-of-NAL; a regression spins billions
  of iterations and is caught by the timeout wrapper.
- reflist_oob.264: a 29-byte synthetic stream - a leading non-IDR B slice (so
  basePic is still the -1 sentinel) with weighted_bipred_idc == 2 (implicit
  weights), a ref_pic_list_modification that resolves to no existing picture, and
  num_ref_idx_active larger than the available references. The resulting
  RefPicList holds out-of-range slots (the -1 basePic and surplus entries), which
  initialize_context's implicit-weight init then used as indices into the
  picture-keyed 0..31 stack/array buffers (td.q[pic], MapPicToList0[pic], ...),
  overrunning them (edge264_headers.c:241). Real over-active-ref captures crash
  the same way (a level-3 480/720/1080 clip and HDTV samples found in a sweep).
  The fix clamps every out-of-range referenced RefPicList entry to an in-range
  slot; ffmpeg decodes such non-conformant streams without crashing. ASAN catches
  the overrun if the clamp is removed.
