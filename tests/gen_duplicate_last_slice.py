#!/usr/bin/env python3
# Generate tests/liveness/duplicate_last_slice.264: a damaged stream of 32x16 pictures
# of two I_PCM macroblocks coded as two slices each, where the second slice of the
# IDR picture is sent twice. The copy starts at a macroblock the first one decoded,
# so it ends there without decoding any. max_num_reorder_frames is 0, so a picture
# goes out as soon as it is decoded - single-threaded, before its copied slice ran.
# A P picture follows. Usage:
#   python3 tests/gen_duplicate_last_slice.py tests/liveness/duplicate_last_slice.264
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_avc
out = sys.argv[1] if len(sys.argv) > 1 else "tests/liveness/duplicate_last_slice.264"

sps = dict(nal_ref_idc=3, nal_unit_type=7, profile_idc=77, constraint_set_flags=[0, 0, 0, 0, 0, 0],
	level_idc=3.0, log2_max_frame_num=4, pic_order_cnt_type=0, log2_max_pic_order_cnt_lsb=4,
	max_num_ref_frames=1, chroma_format_idc=1, bit_depth={"luma": 8, "chroma": 8}, gaps_in_frame_num_value_allowed_flag=0,
	pic_size_in_mbs={"width": 2, "height": 1}, frame_mbs_only_flag=1, direct_8x8_inference_flag=1,
	vui_parameters=dict(overscan_appropriate_flag=-1, pic_struct_present_flag=0,
		motion_vectors_over_pic_boundaries_flag=1, log2_max_mv_length_horizontal=15,
		log2_max_mv_length_vertical=15, max_num_reorder_frames=0, max_dec_frame_buffering=1))
pps = dict(nal_ref_idc=3, nal_unit_type=8, pic_parameter_set_id=0, entropy_coding_mode_flag=0,
	bottom_field_pic_order_in_frame_present_flag=0, num_slice_groups=1,
	num_ref_idx_default_active={"l0": 1, "l1": 1}, weighted_pred_flag=0, weighted_bipred_idc=0,
	pic_init_qp=26, chroma_qp_index_offset=0, deblocking_filter_control_present_flag=1,
	constrained_intra_pred_flag=0, redundant_pic_cnt_present_flag=0)

def pcm(v, p_slice):
	mb = dict(mb_type=30 if p_slice else 25, pcm_samples={"bits_Y": 8, "bits_C": 8, "Y": [v] * 256, "Cb": [v] * 64, "Cr": [v] * 64})
	if p_slice:
		mb["mb_skip_run"] = 0
	return mb

def slice_(frame_num, first_mb, v):
	idr = frame_num == 0
	d = dict(nal_ref_idc=2, nal_unit_type=5 if idr else 1, first_mb_in_slice=first_mb,
		slice_type=7 if idr else 5, pic_parameter_set_id=0,
		frame_num={"bits": 4, "absolute": frame_num}, pic_order_cnt={"type": 0, "bits": 4, "absolute": 2 * frame_num},
		slice_qp_delta=0, disable_deblocking_filter_idc=1, macroblocks_cavlc=[pcm(v, not idr)])
	if idr:
		d.update(idr_pic_id=0, no_output_of_prior_pics_flag=0, long_term_reference_flag=0)
	else:
		d.update(num_ref_idx_active={"override_flag": 0, "l0": 1})
	return d

nals = [sps, pps, slice_(0, 0, 40), slice_(0, 1, 80), slice_(0, 1, 80), slice_(1, 0, 120), slice_(1, 1, 160)]

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
