#!/usr/bin/env python3
# Generate tests/conformance/2d-synthetic/weighted_bipred_reserved.264: a Main profile
# stream whose PPS carries the reserved weighted_bipred_idc 3 (7.4.2.2 allows 0..2).
# An IDR picture at POC 0 and a P picture at POC 8, both of I_PCM macroblocks with
# distinct samples, then a B picture of skipped macroblocks at POC 2, so every
# macroblock blends both references. Implicit weights (idc 2) would give that blend
# 48:16, the default average (idc 0) gives 32:32. Usage:
#   python3 tests/gen_weighted_bipred_reserved.py tests/conformance/2d-synthetic/weighted_bipred_reserved.264
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_avc
out = sys.argv[1] if len(sys.argv) > 1 else "tests/conformance/2d-synthetic/weighted_bipred_reserved.264"

W, H = 2, 2
sps = dict(nal_ref_idc=3, nal_unit_type=7, profile_idc=77, constraint_set_flags=[0, 0, 0, 0, 0, 0],
	level_idc=3.0, log2_max_frame_num=4, pic_order_cnt_type=0, log2_max_pic_order_cnt_lsb=8,
	max_num_ref_frames=2, chroma_format_idc=1, gaps_in_frame_num_value_allowed_flag=0,
	pic_size_in_mbs={"width": W, "height": H}, frame_mbs_only_flag=1, direct_8x8_inference_flag=1)
pps = dict(nal_ref_idc=3, nal_unit_type=8, pic_parameter_set_id=0, entropy_coding_mode_flag=0,
	bottom_field_pic_order_in_frame_present_flag=0, num_slice_groups=1,
	num_ref_idx_default_active={"l0": 1, "l1": 1}, weighted_pred_flag=0, weighted_bipred_idc=3,
	pic_init_qp=26, chroma_qp_index_offset=0, deblocking_filter_control_present_flag=1,
	constrained_intra_pred_flag=0, redundant_pic_cnt_present_flag=0)

def pcm(luma, chroma, p_slice):
	mb = dict(mb_type=30 if p_slice else 25, pcm_samples={"bits_Y": 8, "bits_C": 8, "Y": [luma] * 256, "Cb": [chroma] * 64, "Cr": [255 - chroma] * 64})
	if p_slice:
		mb["mb_skip_run"] = 0
	return mb

common = dict(first_mb_in_slice=0, pic_parameter_set_id=0, slice_qp_delta=0, disable_deblocking_filter_idc=1)
nals = [sps, pps,
	dict(common, nal_ref_idc=3, nal_unit_type=5, slice_type=7, frame_num={"bits": 4, "absolute": 0}, idr_pic_id=0,
		pic_order_cnt={"type": 0, "bits": 8, "absolute": 0}, no_output_of_prior_pics_flag=0,
		long_term_reference_flag=0, macroblocks_cavlc=[pcm(60, 90, False)] * (W * H)),
	dict(common, nal_ref_idc=2, nal_unit_type=1, slice_type=5, frame_num={"bits": 4, "absolute": 1},
		pic_order_cnt={"type": 0, "bits": 8, "absolute": 8}, num_ref_idx_active={"override_flag": 0, "l0": 1},
		macroblocks_cavlc=[pcm(100, 170, True)] * (W * H)),
	dict(common, nal_ref_idc=0, nal_unit_type=1, slice_type=6, frame_num={"bits": 4, "absolute": 2},
		pic_order_cnt={"type": 0, "bits": 8, "absolute": 2}, direct_spatial_mv_pred_flag=1,
		num_ref_idx_active={"override_flag": 0, "l0": 1, "l1": 1}, macroblocks_cavlc=[{"mb_skip_run": W * H}])]

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
