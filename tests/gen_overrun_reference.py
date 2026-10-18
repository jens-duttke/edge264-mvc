#!/usr/bin/env python3
# Generate tests/conformance/2d-synthetic/overrun_reference.264: 128 pairs of a 160x64
# IDR picture and a P picture of P_Skip macroblocks copying it. Each IDR has three
# slices with deblocking on: S0 from macroblock 0 to a, A from a to the end of the
# picture, and B from macroblock k (inside A, a + 1 to a + 8) to the end, with a in the
# first macroblock row in every other pair and in the second one otherwise. A and B
# overlap (non-conformant, 7.4.3 first_mb_in_slice), so A ends where B starts. The
# macroblocks are Intra_16x16 with DC coefficients that change from one macroblock to
# the next, so that deblocking across slice edges changes samples. Decoded with pauses
# before every slice that does not start a picture (tests/slice_overrun_check.c), a
# worker thread decodes, deblocks and publishes A to the end of the picture before the
# parser sees B, while the P picture follows B at once. With --422 the stream is High
# 4:2:2 and the macroblocks also carry chroma DC coefficients, so that deblocking changes
# the 16 chroma rows of a macroblock (overrun_reference_422.264). Usage:
#   python3 tests/gen_overrun_reference.py tests/conformance/2d-synthetic/overrun_reference.264
#   python3 tests/gen_overrun_reference.py --422 tests/conformance/2d-synthetic/overrun_reference_422.264
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_avc
c422 = "--422" in sys.argv[1:]
args = [a for a in sys.argv[1:] if a != "--422"]
out = args[0] if args else "tests/conformance/2d-synthetic/overrun_reference.264"

W, H = 10, 4
sps = dict(nal_ref_idc=3, nal_unit_type=7, profile_idc=66, constraint_set_flags=[0, 0, 0, 0, 0, 0],
	level_idc=3.0, log2_max_frame_num=4, pic_order_cnt_type=2, max_num_ref_frames=1, chroma_format_idc=1,
	gaps_in_frame_num_value_allowed_flag=0, pic_size_in_mbs={"width": W, "height": H},
	frame_mbs_only_flag=1, direct_8x8_inference_flag=1)
if c422:
	sps.update(profile_idc=122, chroma_format_idc=2, bit_depth={"luma": 8, "chroma": 8}, qpprime_y_zero_transform_bypass_flag=0)
pps = dict(nal_ref_idc=3, nal_unit_type=8, pic_parameter_set_id=0, entropy_coding_mode_flag=0,
	bottom_field_pic_order_in_frame_present_flag=0, num_slice_groups=1,
	num_ref_idx_default_active={"l0": 1, "l1": 1}, weighted_pred_flag=0, weighted_bipred_idc=0,
	pic_init_qp=36, chroma_qp_index_offset=0, deblocking_filter_control_present_flag=1,
	constrained_intra_pred_flag=0, redundant_pic_cnt_present_flag=0)

def textured(addr, seed):
	# I_16x16_2_0_0 (DC prediction, no AC), with 16 DC coefficients that differ per macroblock
	v = (addr * 7 + seed * 13) % 9 - 4
	if c422: # I_16x16_2_1_0, with the 8 chroma DC coefficients of each component
		u = (addr * 5 + seed * 3) % 7 - 3
		return dict(mb_type=7, intra_chroma_pred_mode=0, mb_qp_delta=0,
			coeffLevels=[{"nC": 0, "c": [v * 3, -v, v, 2 - v % 3] + [0] * 12},
				{"nC": -2, "c": [u * 4, -u, u, 1, 0, 0, 0, 0]}, {"nC": -2, "c": [-u * 3, u, 0, -1, 0, 0, 0, 0]}])
	return dict(mb_type=3, intra_chroma_pred_mode=0, mb_qp_delta=0,
		coeffLevels=[{"nC": 0, "c": [v * 3, -v, v, 2 - v % 3] + [0] * 12}])

def slice_I(first, last, seed, idr_pic_id):
	return dict(nal_ref_idc=3, nal_unit_type=5, first_mb_in_slice=first, slice_type=7,
		pic_parameter_set_id=0, frame_num={"bits": 4, "absolute": 0}, idr_pic_id=idr_pic_id,
		pic_order_cnt={"type": 2}, no_output_of_prior_pics_flag=0, long_term_reference_flag=0,
		slice_qp_delta=0, disable_deblocking_filter_idc=0, slice_alpha_c0_offset=0, slice_beta_offset=0,
		macroblocks_cavlc=[textured(a, seed) for a in range(first, last)])

def slice_P():
	return dict(nal_ref_idc=3, nal_unit_type=1, first_mb_in_slice=0, slice_type=5,
		pic_parameter_set_id=0, frame_num={"bits": 4, "absolute": 1}, pic_order_cnt={"type": 2},
		num_ref_idx_active={"override_flag": 0, "l0": 1}, slice_qp_delta=0,
		disable_deblocking_filter_idc=0, slice_alpha_c0_offset=0, slice_beta_offset=0,
		macroblocks_cavlc=[{"mb_skip_run": W * H}])

nals = [sps, pps]
for p in range(128):
	a = 1 if p % 2 == 0 else W + 2
	k = a + 1 + p // 2 % 8
	nals += [slice_I(0, a, p, p % 2), slice_I(a, W * H, p + 1, p % 2), slice_I(k, W * H, p + 2, p % 2), slice_P()]

with open(out, "wb") as f: # the NAL loop of gen_avc.main
	for nal in gen_avc.map_dicts(nals):
		f.write(b"\x00\x00\x00\x01")
		bits = 1 << 1
		bits = bits << 2 | nal.nal_ref_idc
		bits = bits << 5 | nal.nal_unit_type
		bits = gen_avc.gen_bits[nal.nal_unit_type](bits, f, nal)
		bits = bits << 1 | 1
		num = bits.bit_length() - 1
		bits ^= 1 << num
		bits <<= -num % 8
		f.write(gen_avc.escape(bits.to_bytes((num + 7) // 8, byteorder="big")))
print(f"wrote {out}")
