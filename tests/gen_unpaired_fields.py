#!/usr/bin/env python3
# Drop field pictures from an Annex B stream of field pictures, which leaves the
# other field of their frames without its pair: a second field dropped leaves its
# first field alone, a first field dropped leaves its second field to start a
# frame of its own, and the last field dropped leaves the stream ending on a first
# field. A picture starts at a slice of type 1 or 5 with first_mb_in_slice 0,
# counted from 0. Usage:
#   python3 tests/gen_unpaired_fields.py <in.264> <out.264> <picture> [<picture>...]
# 2d-synthetic/unpaired_fields is the ITU vector CVFI1_SVA_C (CAVLC fields, top
# field first) with the pictures 5, 8 and 13 dropped, the last of its 14 fields.
import sys
from gen_slice_deblock_offsets import split_nals

src, out, drop = sys.argv[1], sys.argv[2], {int(p) for p in sys.argv[3:]}
result, picture = bytearray(), -1
for nal in split_nals(open(src, "rb").read()):
	if nal[0] & 0x1f in (1, 5) and nal[1] & 0x80: # first_mb_in_slice == 0 (ue(0) is a single 1 bit)
		picture += 1
	if nal[0] & 0x1f in (1, 5) and picture in drop:
		continue
	result.extend(b"\x00\x00\x00\x01" + nal)
open(out, "wb").write(result)
print(f"wrote {out}: {picture + 1 - len(drop)} of {picture + 1} pictures, {len(result)} bytes")
