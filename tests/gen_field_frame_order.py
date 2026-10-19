#!/usr/bin/env python3
# Reorder an MVC stream of field pictures from the order of its access units
# (base top field, dependent top field, base bottom field, dependent bottom
# field) to the order of the frames of each view (base top and bottom fields,
# then the dependent ones), which a transport stream of interlaced 3D video gives
# when its packets hold whole frames of each view and are sent one after the
# other. Each picture keeps the non-VCL NAL units before it. The decoded output
# is that of the stream in access unit order. Usage:
#   python3 tests/gen_field_frame_order.py <in.264> <out.264>
# mvc-jmvc/mvcict1_frame_order comes from mvc-jmvc/mvcict1_fields.
import sys
from gen_slice_deblock_offsets import split_nals

src, out = sys.argv[1], sys.argv[2]
pictures, pending = [], []
for nal in split_nals(open(src, "rb").read()):
	t = nal[0] & 0x1f
	first = (t in (1, 5) and nal[1] & 0x80) or (t == 20 and nal[4] & 0x80) # first_mb_in_slice == 0
	if first:
		pictures.append({"dep": t == 20, "nals": pending + [nal]})
		pending = []
	elif t in (1, 5, 20):
		pictures[-1]["nals"].append(nal)
	else:
		pending.append(nal)
for i in range(0, len(pictures) - 3, 4):
	if [p["dep"] for p in pictures[i:i + 4]] == [False, True, False, True]:
		pictures[i + 1], pictures[i + 2] = pictures[i + 2], pictures[i + 1]
result = bytearray()
for nal in [n for p in pictures for n in p["nals"]] + pending:
	result.extend(b"\x00\x00\x00\x01" + nal)
open(out, "wb").write(result)
print(f"wrote {out}: {len(pictures)} pictures, {len(result)} bytes")
