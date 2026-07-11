#!/usr/bin/env python3
# Generate tests/liveness/mvc_unpaired_base_eos.264: the committed
# mvc_unpaired_base.264 (MVCDS-4 with one dependent-view slice removed, so one
# base picture has no POC-matching dependent) with a trailing end_of_sequence
# NAL (type 10) appended.
#
# mvc_unpaired_base tests the unpaired base being flushed at the *end of buffer*
# (buf >= end). This fixture tests the same unpaired base held at an
# *end_of_sequence NAL* instead - the point a multi-clip 3D player reaches when a
# clip ends on its end_of_seq unit rather than on the buffer boundary. Without
# parse_end_of_sequence setting the flushing valve, the held base is never
# emitted and the draining caller spins ENOBUFS forever (stall, losing the last
# base frame); with the valve the base is flushed and all 9 base frames come out.
import os

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "liveness", "mvc_unpaired_base.264")
dst = os.path.join(here, "liveness", "mvc_unpaired_base_eos.264")

data = bytearray(open(src, "rb").read())
# Annex-B end_of_sequence: 3-byte start code + NAL header (type 10, nal_ref_idc 0).
data += bytes([0x00, 0x00, 0x01, 0x0A])
open(dst, "wb").write(data)
print("wrote %s (%d bytes, +4 for the trailing end_of_sequence)" % (dst, len(data)))
