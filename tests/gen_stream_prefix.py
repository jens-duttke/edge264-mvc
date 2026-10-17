#!/usr/bin/env python3
# Cut an Annex B stream after its first N primary pictures, at the start of the
# next one (a slice of type 1 or 5 with first_mb_in_slice 0), for a short fixture
# from a long conformance vector. The prefix of a stream is a stream of its own,
# whose output is that of the reference decoder for the same prefix. Usage:
#   python3 tests/gen_stream_prefix.py <in.264> <N> <out.264>
# The monochrome fixtures of tests/conformance/2d-synthetic come from the ITU
# vectors HPCAMOLQ_BRCM_B and HPCVMOLQ_BRCM_B (FRExt), with N = 10; mono_after_420 is
# HPCALQ_BRCM_B with N = 2, an end_of_seq NAL (00 00 00 01 0a) and mono_cabac with N = 3.
import sys
from gen_slice_deblock_offsets import split_nals

src, count, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
result, pictures = bytearray(), 0
for nal in split_nals(open(src, "rb").read()):
	if nal[0] & 0x1f in (1, 5) and nal[1] & 0x80: # first_mb_in_slice == 0 (ue(0) is a single 1 bit)
		pictures += 1
		if pictures > count:
			break
	result.extend(b"\x00\x00\x00\x01" + nal)
open(out, "wb").write(result)
print(f"wrote {out}: {min(pictures, count)} pictures, {len(result)} bytes")
