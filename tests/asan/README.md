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
