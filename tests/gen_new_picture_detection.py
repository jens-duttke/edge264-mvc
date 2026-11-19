#!/usr/bin/env python3
# Generate tests/conformance/2d-synthetic/new_picture_detection.264: pictures of two
# I_PCM macroblocks coded as two slices each, where a picture differs from the one
# before it only in one syntax element that 7.4.1.2.4 lists for the detection of a
# new picture. Every picture after the first of a sequence carries an MMCO 5, so that
# frame_num 1 and the picture order count may repeat (7.4.1.2.4 NOTE 1):
#   pic_order_cnt_type 0: an IDR picture, then three pictures of frame_num 1 and
#     pic_order_cnt_lsb 2 differing from the previous one in pic_parameter_set_id,
#     then in delta_pic_order_cnt_bottom;
#   pic_order_cnt_type 1: an IDR picture, then two pictures of frame_num 1 differing
#     in delta_pic_order_cnt[1].
# Each MMCO 5 outputs the pictures before it, so the display order is the decoding
# order. Each picture carries a distinct sample value. Usage:
#   python3 tests/gen_new_picture_detection.py tests/conformance/2d-synthetic/new_picture_detection.264
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_avc
out = sys.argv[1] if len(sys.argv) > 1 else "tests/conformance/2d-synthetic/new_picture_detection.264"

def sps(poc_type):
	d = dict(nal_ref_idc=3, nal_unit_type=7, profile_idc=77, constraint_set_flags=[0, 0, 0, 0, 0, 0],
		level_idc=3.0, log2_max_frame_num=4, pic_order_cnt_type=poc_type, max_num_ref_frames=1,
		chroma_format_idc=1, gaps_in_frame_num_value_allowed_flag=0, pic_size_in_mbs={"width": 2, "height": 1},
		frame_mbs_only_flag=1, direct_8x8_inference_flag=1)
	if poc_type == 0:
		d["log2_max_pic_order_cnt_lsb"] = 4
	else:
		d.update(delta_pic_order_always_zero_flag=0, offset_for_non_ref_pic=0,
			offset_for_top_to_bottom_field=0, offsets_for_ref_frames=[4])
	return d

def pps(pps_id, bottom):
	return dict(nal_ref_idc=3, nal_unit_type=8, pic_parameter_set_id=pps_id, entropy_coding_mode_flag=0,
		bottom_field_pic_order_in_frame_present_flag=bottom, num_slice_groups=1,
		num_ref_idx_default_active={"l0": 1, "l1": 1}, weighted_pred_flag=0, weighted_bipred_idc=0,
		pic_init_qp=26, chroma_qp_index_offset=0, deblocking_filter_control_present_flag=1,
		constrained_intra_pred_flag=0, redundant_pic_cnt_present_flag=0)

def pcm(v, p_slice):
	mb = dict(mb_type=30 if p_slice else 25, pcm_samples={"bits_Y": 8, "bits_C": 8, "Y": [v] * 256, "Cb": [v] * 64, "Cr": [v] * 64})
	if p_slice:
		mb["mb_skip_run"] = 0
	return mb

def picture(v, pps_id, poc, idr_pic_id=None):
	# poc is the pic_order_cnt syntax of the slice header (see gen_avc.py)
	idr = idr_pic_id is not None
	slices = []
	for first_mb in (0, 1):
		d = dict(nal_ref_idc=2, nal_unit_type=5 if idr else 1, first_mb_in_slice=first_mb,
			slice_type=7 if idr else 5, pic_parameter_set_id=pps_id,
			frame_num={"bits": 4, "absolute": 0 if idr else 1}, pic_order_cnt=poc,
			slice_qp_delta=0, disable_deblocking_filter_idc=1, macroblocks_cavlc=[pcm(v, not idr)])
		if idr:
			d.update(idr_pic_id=idr_pic_id, no_output_of_prior_pics_flag=0, long_term_reference_flag=0)
		else:
			d.update(num_ref_idx_active={"override_flag": 0, "l0": 1}, memory_management_control_operations=[{"mmco": 5}])
		slices.append(d)
	return slices

def poc0(bottom):
	return {"type": 0, "bits": 4, "absolute": 2, "bottom": 2 + bottom}

nals = [sps(0), pps(0, 1), pps(1, 1)]
nals += picture(20, 0, {"type": 0, "bits": 4, "absolute": 0, "bottom": 0}, idr_pic_id=0)
nals += picture(50, 0, poc0(0)) # frame_num differs
nals += picture(80, 1, poc0(0)) # only pic_parameter_set_id differs
nals += picture(110, 1, poc0(1)) # only delta_pic_order_cnt_bottom differs
nals += [sps(1), pps(2, 1)]
nals += picture(140, 2, {"type": 1, "delta0": 0, "delta1": 0}, idr_pic_id=1)
nals += picture(170, 2, {"type": 1, "delta0": 0, "delta1": 0}) # frame_num differs
nals += picture(200, 2, {"type": 1, "delta0": 0, "delta1": 1}) # only delta_pic_order_cnt[1] differs

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
