#!/usr/bin/env python3
# Generate tests/conformance/2d-synthetic/aux_picture.264 from the committed ITU
# vector BA3_SVA_C.264 (one slice per picture) by adding an alpha plane as an
# auxiliary coded picture: a sequence parameter set extension (NAL type 13,
# 7.3.2.1.2, aux_format_idc 1) after every SPS, and after every primary picture
# a slice of type 19 carrying a copy of its slice data. 7.4.1 lets a decoder
# ignore both, as the auxiliary picture has no effect on the primary pictures,
# so the output must equal that of BA3_SVA_C. Usage:
#   python3 tests/gen_aux_picture.py tests/conformance/2d/BA3_SVA_C.264 \
#     tests/conformance/2d-synthetic/aux_picture.264
import sys
from gen_slice_deblock_offsets import split_nals


def read_ue(data, bit):
	zeros = 0
	while not data[bit >> 3] >> (7 - (bit & 7)) & 1:
		zeros += 1
		bit += 1
	value = 0
	for _ in range(zeros + 1):
		value = value << 1 | (data[bit >> 3] >> (7 - (bit & 7)) & 1)
		bit += 1
	return value - 1, bit


def ue(value):
	code = bin(value + 1)[2:]
	return "0" * (len(code) - 1) + code


def rbsp_to_nal(header, bits):
	bits += "1" # rbsp_stop_one_bit
	bits += "0" * (-len(bits) % 8)
	rbsp = bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))
	out, zeros = bytearray([header]), 0
	for b in rbsp:
		if zeros >= 2 and b <= 3: # emulation_prevention_three_byte
			out.append(3)
			zeros = 0
		out.append(b)
		zeros = zeros + 1 if b == 0 else 0
	return bytes(out)


src, out = sys.argv[1], sys.argv[2]
result = bytearray()
for nal in split_nals(open(src, "rb").read()):
	result.extend(b"\x00\x00\x00\x01" + nal)
	nal_type = nal[0] & 0x1f
	if nal_type == 7:
		sps_id, _ = read_ue(nal[1:], 24) # after profile_idc, the constraint flags and level_idc
		ext = (ue(sps_id) + ue(1) # aux_format_idc: alpha blended with the primary picture
			+ ue(0) # bit_depth_aux_minus8
			+ "0" # alpha_incr_flag
			+ format(255, "09b") + format(0, "09b") # alpha_opaque_value, alpha_transparent_value
			+ "0") # additional_extension_flag
		result.extend(b"\x00\x00\x00\x01" + rbsp_to_nal(0x60 | 13, ext))
	elif nal_type in (1, 5):
		# the auxiliary picture follows the primary one in its access unit (7.4.1.2.3)
		result.extend(b"\x00\x00\x00\x01" + bytes([nal[0] & 0x60 | 19]) + nal[1:])
open(out, "wb").write(result)
print(f"wrote {out}")
