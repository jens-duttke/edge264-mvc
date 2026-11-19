#!/usr/bin/env python3
# Generate the fixtures of tests/conformance/mvc-synthetic in which the two views of an
# access unit carry different frame_num values, which 7.4.3 allows, since it constrains
# frame_num per view (H.7.4.3); only their picture order counts must match (H.8.1). Every
# view component is one I_PCM macroblock whose luma and chroma samples all carry one
# value, distinct per view component (ground_truth.py derives the expected hashes).
#   mvc_2d_then_3d: a coded video sequence of the base view alone, three reference
#     pictures, then a stereo one of three access units (a menu before a stereo movie).
#     The base view counts its frame_num on from the first sequence, the dependent view
#     starts from 0.
#   mvc_nonref_dependent: the dependent view codes non-reference pictures after its IDR
#     picture, so its frame_num stays 1 while that of the base view goes on.
#   mvc_dependent_gap: the dependent view skips a frame_num value (gaps allowed in its
#     subset SPS only), the base view does not.
#   mvc_nonref_dependent_lost_slice: mvc_nonref_dependent with pictures of two
#     macroblocks coded as two slices each, of which the second slice of the dependent
#     view of the third access unit is missing (a damaged stream). That macroblock is
#     concealed from the base view of its access unit (ground_truth.py, EXPECTED_ROWS).
# Usage:
#   python3 tests/gen_mvc_view_frame_num.py tests/conformance/mvc-synthetic/<name>.264 <name>
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_avc
out, variant = sys.argv[1], sys.argv[2]
width = 2 if variant == "mvc_nonref_dependent_lost_slice" else 1 # macroblocks, one slice each

sps = dict(nal_ref_idc=3, nal_unit_type=7, profile_idc=100, constraint_set_flags=[0, 0, 0, 0, 0, 0],
	level_idc=3.0, chroma_format_idc=1, bit_depth={"luma": 8, "chroma": 8}, qpprime_y_zero_transform_bypass_flag=0,
	log2_max_frame_num=4, pic_order_cnt_type=0, log2_max_pic_order_cnt_lsb=6, max_num_ref_frames=2,
	gaps_in_frame_num_value_allowed_flag=0, pic_size_in_mbs={"width": width, "height": 1},
	frame_mbs_only_flag=1, direct_8x8_inference_flag=1)
ssps = dict(sps, nal_unit_type=15, profile_idc=128, view_ids=[0, 1], num_anchor_refs={"l0": 0, "l1": 0},
	num_non_anchor_refs={"l0": 0, "l1": 0},
	gaps_in_frame_num_value_allowed_flag=1 if variant == "mvc_dependent_gap" else 0,
	level_values_signalled=[{"idc": 3.0, "operation_points": [{"temporal_id": 0, "target_views": [0, 1], "num_views": 2}]}])
def pps(pps_id):
	return dict(nal_ref_idc=3, nal_unit_type=8, pic_parameter_set_id=pps_id, entropy_coding_mode_flag=0,
		bottom_field_pic_order_in_frame_present_flag=0, num_slice_groups=1,
		num_ref_idx_default_active={"l0": 1, "l1": 1}, weighted_pred_flag=0, weighted_bipred_idc=0,
		pic_init_qp=26, chroma_qp_index_offset=0, deblocking_filter_control_present_flag=1,
		constrained_intra_pred_flag=0, redundant_pic_cnt_present_flag=0)

def pcm(v, p_slice):
	mb = dict(mb_type=30 if p_slice else 25, pcm_samples={"bits_Y": 8, "bits_C": 8, "Y": [v] * 256, "Cb": [v] * 64, "Cr": [v] * 64})
	if p_slice:
		mb["mb_skip_run"] = 0
	return mb

def view_component(view, idr_id, frame_num, poc, v, ref=True, slices=width):
	idr = idr_id is not None
	mvc = dict(non_idr_flag=0 if idr else 1, priority_id=0, view_id=view, temporal_id=0,
		anchor_pic_flag=1 if idr else 0, inter_view_flag=0)
	nals = [dict(mvc, nal_ref_idc=3 if ref else 0, nal_unit_type=14)] if view == 0 else [] # prefix NAL unit
	for first_mb in range(slices):
		d = dict(nal_ref_idc=3 if ref else 0, first_mb_in_slice=first_mb, slice_type=7 if idr else 5, pic_parameter_set_id=view,
			frame_num={"bits": 4, "absolute": frame_num}, pic_order_cnt={"type": 0, "bits": 6, "absolute": poc},
			slice_qp_delta=0, disable_deblocking_filter_idc=1, macroblocks_cavlc=[pcm(v, not idr)])
		if idr:
			d.update(idr_pic_id=idr_id, no_output_of_prior_pics_flag=0, long_term_reference_flag=0)
		else:
			d.update(num_ref_idx_active={"override_flag": 0, "l0": 1})
		nals.append(dict(d, nal_unit_type=5 if idr else 1) if view == 0 else dict(d, **mvc, nal_unit_type=20))
	return nals

nals = [sps, ssps, pps(0), pps(1)]
if variant == "mvc_2d_then_3d":
	for fn, v in enumerate((20, 40, 60)): # base view alone
		nals += view_component(0, 0 if fn == 0 else None, fn, 2 * fn, v)
	for fn, (b, d) in enumerate(((80, 90), (100, 110), (120, 130))): # stereo, from an IDR access unit
		idr_id = 1 if fn == 0 else None
		nals += view_component(0, idr_id, fn, 2 * fn, b) + view_component(1, idr_id, fn, 2 * fn, d)
elif variant in ("mvc_nonref_dependent", "mvc_nonref_dependent_lost_slice"):
	for fn, (b, d) in enumerate(((20, 30), (40, 50), (60, 70), (80, 90))):
		idr_id = 0 if fn == 0 else None
		# the dependent view refers to its IDR picture alone, so its frame_num stays 1
		lost = variant == "mvc_nonref_dependent_lost_slice" and fn == 2
		nals += view_component(0, idr_id, fn, 2 * fn, b) + view_component(1, idr_id, min(fn, 1), 2 * fn, d, ref=fn == 0, slices=width - lost)
elif variant == "mvc_dependent_gap":
	for fn, (b, d) in enumerate(((20, 30), (40, 50), (60, 70), (80, 90))):
		idr_id = 0 if fn == 0 else None
		# the dependent view skips frame_num 1
		nals += view_component(0, idr_id, fn, 2 * fn, b) + view_component(1, idr_id, fn + (fn > 0), 2 * fn, d)

with open(out, "wb") as f: # the NAL loop of gen_avc.main
	for nal in gen_avc.map_dicts(nals):
		f.write(b"\x00\x00\x00\x01")
		bits = 1 << 1
		bits = bits << 2 | nal.nal_ref_idc
		bits = bits << 5 | nal.nal_unit_type
		if nal.nal_unit_type in gen_avc.gen_bits:
			bits = gen_avc.gen_bits[nal.nal_unit_type](bits, f, nal)
		if 1 << nal.nal_unit_type & 0b1110011011001111111110:
			bits = bits << 1 | 1
		num = bits.bit_length() - 1
		bits ^= 1 << num
		bits <<= -num % 8
		f.write(gen_avc.escape(bits.to_bytes((num + 7) // 8, byteorder="big")))
print(f"wrote {out}")
