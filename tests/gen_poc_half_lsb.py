#!/usr/bin/env python3
# Generate tests/conformance/2d-synthetic/poc_half_lsb.264: four reference pictures
# (an IDR picture, then three P pictures) of one I_PCM macroblock each, with
# MaxPicOrderCntLsb 16 and pic_order_cnt_lsb 0, 8, 0, 8. Every step differs from the
# previous picture by exactly MaxPicOrderCntLsb / 2, upwards and downwards, which
# equation 8-3 counts as +8 both ways: the picture order counts are 0, 8, 16, 24 and
# the display order is the decoding order. There is no VUI, so the DPB holds every
# picture until the end of the stream and the output order rests on the counts
# alone. Each picture carries a distinct luma value. Usage:
#   python3 tests/gen_poc_half_lsb.py tests/conformance/2d-synthetic/poc_half_lsb.264
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_avc
out = sys.argv[1] if len(sys.argv) > 1 else "tests/conformance/2d-synthetic/poc_half_lsb.264"

sps = dict(nal_ref_idc=3, nal_unit_type=7, profile_idc=77, constraint_set_flags=[0, 0, 0, 0, 0, 0],
	level_idc=3.0, log2_max_frame_num=4, pic_order_cnt_type=0, log2_max_pic_order_cnt_lsb=4,
	max_num_ref_frames=1, chroma_format_idc=1, gaps_in_frame_num_value_allowed_flag=0,
	pic_size_in_mbs={"width": 1, "height": 1}, frame_mbs_only_flag=1, direct_8x8_inference_flag=1)
pps = dict(nal_ref_idc=3, nal_unit_type=8, pic_parameter_set_id=0, entropy_coding_mode_flag=0,
	bottom_field_pic_order_in_frame_present_flag=0, num_slice_groups=1,
	num_ref_idx_default_active={"l0": 1, "l1": 1}, weighted_pred_flag=0, weighted_bipred_idc=0,
	pic_init_qp=26, chroma_qp_index_offset=0, deblocking_filter_control_present_flag=1,
	constrained_intra_pred_flag=0, redundant_pic_cnt_present_flag=0)

def pcm(luma, p_slice):
	mb = dict(mb_type=30 if p_slice else 25, pcm_samples={"bits_Y": 8, "bits_C": 8, "Y": [luma] * 256, "Cb": [128] * 64, "Cr": [luma] * 64})
	if p_slice:
		mb["mb_skip_run"] = 0
	return mb

def picture(frame_num, lsb, luma):
	d = dict(nal_ref_idc=2, nal_unit_type=5 if frame_num == 0 else 1, first_mb_in_slice=0,
		slice_type=7 if frame_num == 0 else 5, pic_parameter_set_id=0,
		frame_num={"bits": 4, "absolute": frame_num}, pic_order_cnt={"type": 0, "bits": 4, "absolute": lsb},
		slice_qp_delta=0, disable_deblocking_filter_idc=1, macroblocks_cavlc=[pcm(luma, frame_num > 0)])
	if frame_num == 0:
		d.update(idr_pic_id=0, no_output_of_prior_pics_flag=0, long_term_reference_flag=0)
	else:
		d.update(num_ref_idx_active={"override_flag": 0, "l0": 1})
	return d

nals = [sps, pps, picture(0, 0, 40), picture(1, 8, 80), picture(2, 0, 120), picture(3, 8, 160)]

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
