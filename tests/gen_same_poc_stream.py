#!/usr/bin/env python3
# Generate a copyright-safe, all-128, 1x1-macroblock Stereo-High MVC stream that
# reproduces the same-POC / different-frame_num dependent-view pairing collision:
# many short IDR sequences with log2_max_pic_order_cnt_lsb=4 make frames of
# different sequences share a full POC while carrying different frame_num, and
# max_dec_frame_buffering=4 force-bumps a base under a paced consumer while its
# same-POC dependent is mispaired - the pre-fix POC-only primary scan then strands
# frames and overflows the DPB (a clean paced FAIL); the (FrameNum, POC) fix
# resolves it. FRAMES below is the per-frame decode STRUCTURE (nal_unit_type,
# nal_ref_idc, frame_num, pic_order_cnt_lsb) only - technical metadata, no picture
# data; every decoded sample is 128 by spec (see ground_truth.py). Usage:
#   python3 tests/gen_same_poc_stream.py tests/conformance/mvc-synthetic/mvc_same_poc_pairing.yaml
import sys
out = sys.argv[1] if len(sys.argv) > 1 else "tests/conformance/mvc-synthetic/mvc_same_poc_pairing.yaml"

FRAMES = [
    (5, 1, 0, 0), (1, 1, 1, 1), (1, 1, 2, 2), (1, 1, 3, 3), (1, 1, 4, 4), (1, 1, 5, 5), (1, 1, 6, 6), (1, 1, 7, 7),
    (1, 1, 8, 8), (1, 1, 9, 9), (1, 1, 10, 10), (1, 1, 11, 11), (1, 1, 12, 12), (1, 1, 13, 13), (1, 1, 14, 14), (1, 1, 15, 15),
    (1, 1, 16, 0), (1, 1, 17, 1), (5, 1, 0, 0), (1, 1, 1, 2), (1, 0, 2, 1), (1, 1, 2, 4), (1, 0, 3, 3), (1, 1, 3, 6),
    (1, 0, 4, 5), (1, 1, 4, 8), (1, 0, 5, 7), (1, 1, 5, 10), (1, 0, 6, 9), (1, 1, 6, 12), (1, 0, 7, 11), (1, 1, 7, 14),
    (1, 0, 8, 13), (1, 1, 8, 0), (1, 0, 9, 15), (1, 1, 9, 2), (1, 0, 10, 1), (1, 1, 10, 4), (1, 0, 11, 3), (1, 1, 11, 6),
    (1, 0, 12, 5), (1, 1, 12, 8), (1, 0, 13, 7), (1, 1, 13, 10), (1, 0, 14, 9), (1, 1, 14, 12), (1, 0, 15, 11), (1, 1, 15, 14),
    (1, 0, 16, 13), (1, 1, 16, 0), (1, 0, 17, 15), (1, 1, 17, 2), (1, 0, 18, 1), (1, 1, 18, 4), (1, 0, 19, 3), (1, 1, 19, 6),
    (1, 0, 20, 5), (5, 1, 0, 0), (1, 1, 1, 1), (1, 1, 2, 2), (1, 1, 3, 3), (1, 1, 4, 4), (1, 1, 5, 5), (5, 1, 0, 0),
    (1, 1, 1, 2), (1, 0, 2, 1), (1, 1, 2, 4), (1, 0, 3, 3), (1, 1, 3, 6), (1, 0, 4, 5), (1, 1, 4, 8), (1, 0, 5, 7),
    (1, 1, 5, 10), (1, 0, 6, 9), (1, 1, 6, 12), (1, 0, 7, 11), (1, 1, 7, 14), (1, 0, 8, 13), (1, 1, 8, 0), (1, 0, 9, 15),
    (1, 1, 9, 2), (1, 0, 10, 1), (1, 1, 10, 4), (1, 0, 11, 3), (1, 1, 11, 6), (1, 0, 12, 5), (1, 1, 12, 8), (1, 0, 13, 7),
    (1, 1, 13, 10), (1, 0, 14, 9), (1, 1, 14, 12), (1, 0, 15, 11), (1, 1, 15, 14), (1, 0, 16, 13), (1, 1, 16, 0), (1, 0, 17, 15),
    (1, 1, 17, 2), (1, 0, 18, 1), (1, 1, 18, 4), (1, 0, 19, 3), (1, 1, 19, 6), (1, 0, 20, 5), (1, 1, 20, 8), (1, 0, 21, 7),
    (1, 1, 21, 10), (1, 0, 22, 9), (1, 1, 22, 12), (1, 0, 23, 11), (1, 1, 23, 14), (1, 0, 24, 13), (1, 1, 24, 0), (1, 0, 25, 15),
    (1, 1, 25, 2), (1, 0, 26, 1), (1, 1, 26, 4), (1, 0, 27, 3), (1, 1, 27, 6), (1, 0, 28, 5), (1, 1, 28, 8), (1, 0, 29, 7),
    (1, 1, 29, 10), (1, 0, 30, 9), (1, 1, 30, 12), (1, 0, 31, 11), (1, 1, 31, 14), (1, 0, 32, 13), (1, 1, 32, 0), (1, 0, 33, 15),
    (1, 1, 33, 2), (1, 0, 34, 1), (1, 1, 34, 4), (1, 0, 35, 3), (1, 1, 35, 6), (1, 0, 36, 5), (1, 1, 36, 8), (1, 0, 37, 7),
    (1, 1, 37, 10), (1, 0, 38, 9), (1, 1, 38, 12), (1, 0, 39, 11), (1, 1, 39, 14), (1, 0, 40, 13), (1, 1, 40, 0), (1, 0, 41, 15),
    (1, 1, 41, 2), (1, 0, 42, 1), (1, 1, 42, 4), (1, 0, 43, 3), (1, 1, 43, 6), (1, 0, 44, 5), (1, 1, 44, 8), (1, 0, 45, 7),
    (1, 1, 45, 10), (1, 0, 46, 9), (1, 1, 46, 12), (1, 0, 47, 11), (1, 1, 47, 14), (1, 0, 48, 13), (1, 1, 48, 0), (1, 0, 49, 15),
    (1, 1, 49, 2), (1, 0, 50, 1), (1, 1, 50, 4), (1, 0, 51, 3), (1, 1, 51, 6), (1, 0, 52, 5), (1, 1, 52, 8), (1, 0, 53, 7),
    (1, 1, 53, 10), (1, 0, 54, 9), (1, 1, 54, 12), (1, 0, 55, 11), (1, 1, 55, 14), (1, 0, 56, 13), (1, 1, 56, 0), (1, 0, 57, 15),
    (1, 1, 57, 2), (1, 0, 58, 1), (5, 1, 0, 0), (1, 1, 1, 1), (1, 1, 2, 2), (1, 1, 3, 3), (1, 1, 4, 4), (5, 1, 0, 0),
    (1, 1, 1, 1), (1, 1, 2, 2), (1, 1, 3, 3), (1, 1, 4, 4), (1, 1, 5, 5), (1, 1, 6, 6), (1, 1, 7, 7), (1, 1, 8, 8),
    (1, 1, 9, 9), (1, 1, 10, 10), (1, 1, 11, 11), (1, 1, 12, 12), (1, 1, 13, 13), (1, 1, 14, 14), (1, 1, 15, 15), (1, 1, 16, 0),
    (1, 1, 17, 1), (1, 1, 18, 2), (1, 1, 19, 3), (1, 1, 20, 4), (1, 1, 21, 5), (1, 1, 22, 6), (1, 1, 23, 7), (1, 1, 24, 8),
    (1, 1, 25, 9), (1, 1, 26, 10), (1, 1, 27, 11), (1, 1, 28, 12), (1, 1, 29, 13), (1, 1, 30, 14), (1, 1, 31, 15), (1, 1, 32, 0),
    (1, 1, 33, 1), (1, 1, 34, 2), (1, 1, 35, 3), (1, 1, 36, 4), (1, 1, 37, 5), (1, 1, 38, 6), (1, 1, 39, 7), (1, 1, 40, 8),
    (1, 1, 41, 9), (1, 1, 42, 10), (1, 1, 43, 11), (1, 1, 44, 12), (5, 1, 0, 0), (1, 1, 1, 2), (1, 0, 2, 1), (1, 1, 2, 4),
    (1, 0, 3, 3), (1, 1, 3, 6), (1, 0, 4, 5), (1, 1, 4, 8), (1, 0, 5, 7), (1, 1, 5, 10), (1, 0, 6, 9), (1, 1, 6, 12),
    (1, 0, 7, 11), (1, 1, 7, 14), (1, 0, 8, 13), (1, 1, 8, 0), (1, 0, 9, 15), (1, 1, 9, 2), (1, 0, 10, 1), (1, 1, 10, 4),
    (1, 0, 11, 3), (1, 1, 11, 6), (1, 0, 12, 5), (1, 1, 12, 8), (1, 0, 13, 7), (1, 1, 13, 10), (1, 0, 14, 9), (1, 1, 14, 12),
    (1, 0, 15, 11), (1, 1, 15, 14), (1, 0, 16, 13), (1, 1, 16, 0), (1, 0, 17, 15), (1, 1, 17, 2), (1, 0, 18, 1), (1, 1, 18, 4),
    (1, 0, 19, 3), (1, 1, 19, 6), (1, 0, 20, 5), (5, 1, 0, 0), (1, 1, 1, 1), (1, 1, 2, 2), (1, 1, 3, 3), (1, 1, 4, 4),
    (1, 1, 5, 5), (1, 1, 6, 6), (5, 1, 0, 0), (1, 1, 1, 1), (1, 1, 2, 2), (1, 1, 3, 3), (1, 1, 4, 4), (1, 1, 5, 5),
    (1, 1, 6, 6), (1, 1, 7, 7), (1, 1, 8, 8), (1, 1, 9, 9), (1, 1, 10, 10), (1, 1, 11, 11), (1, 1, 12, 12), (1, 1, 13, 13),
    (1, 1, 14, 14), (1, 1, 15, 15), (5, 1, 0, 0), (5, 1, 0, 0), (1, 1, 1, 2), (1, 0, 2, 1), (1, 1, 2, 4), (1, 0, 3, 3),
    (1, 1, 3, 6), (1, 0, 4, 5), (1, 1, 4, 8), (1, 0, 5, 7), (1, 1, 5, 10), (1, 0, 6, 9), (1, 1, 6, 12), (1, 0, 7, 11),
    (1, 1, 7, 14), (1, 0, 8, 13), (1, 1, 8, 0), (1, 0, 9, 15), (1, 1, 9, 2), (1, 0, 10, 1), (1, 1, 10, 4), (1, 0, 11, 3),
    (1, 1, 11, 6), (1, 0, 12, 5), (1, 1, 12, 8), (1, 0, 13, 7), (1, 1, 13, 10), (1, 0, 14, 9), (1, 1, 14, 12), (1, 0, 15, 11),
]

def base_slice(nut, fn, poc, nri):
    prefix_type = 14
    slice_type = 2 if nut == 5 else 0   # I for IDR, P otherwise
    idr = "  idr_pic_id: 0\n" if nut == 5 else ""
    anchor = 1 if nut == 5 else 0
    inter_view = 0 if nut == 5 else 1
    extra = ("  no_output_of_prior_pics_flag: 0\n  long_term_reference_flag: 0\n"
             if nut == 5 else "  num_ref_idx_active: {override_flag: 0, l0: 1}\n")
    mb = ("  - mb_type: 0\n    rem_intra4x4_pred_modes: [-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1]\n"
          "    intra_chroma_pred_mode: 0\n    coded_block_pattern: 0\n") if nut == 5 else (
          "  - mb_skip_run: 0\n    mb_type: 0\n    ref_idx: {}\n    mvds: [[0,0],]\n    coded_block_pattern: 0\n")
    return (f"- nal_ref_idc: {nri}\n  nal_unit_type: {prefix_type}\n  non_idr_flag: {0 if nut==5 else 1}\n"
            f"  priority_id: 0\n  view_id: 0\n  temporal_id: 0\n  anchor_pic_flag: {anchor}\n  inter_view_flag: {inter_view}\n"
            f"- nal_ref_idc: {nri}\n  nal_unit_type: {nut}\n  first_mb_in_slice: 0\n  slice_type: {slice_type}\n"
            f"  pic_parameter_set_id: 0\n  frame_num: {{bits: 10, absolute: {fn}}}\n{idr}"
            f"  pic_order_cnt: {{type: 0, bits: 4, absolute: {poc}}}\n{extra}"
            f"  slice_qp_delta: 0\n  macroblocks_cavlc:\n{mb}")

def dep_slice(nut, fn, poc, nri):
    slice_type = 2 if nut == 5 else 0
    idr = "  idr_pic_id: 0\n" if nut == 5 else ""
    anchor = 1 if nut == 5 else 0
    extra = ("  no_output_of_prior_pics_flag: 0\n  long_term_reference_flag: 0\n"
             if nut == 5 else "  num_ref_idx_active: {override_flag: 0, l0: 1}\n")
    mb = ("  - mb_type: 0\n    rem_intra4x4_pred_modes: [-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1]\n"
          "    intra_chroma_pred_mode: 0\n    coded_block_pattern: 0\n") if nut == 5 else (
          "  - mb_skip_run: 0\n    mb_type: 0\n    ref_idx: {}\n    mvds: [[0,0],]\n    coded_block_pattern: 0\n")
    return (f"- nal_ref_idc: {nri}\n  nal_unit_type: 20\n  non_idr_flag: {0 if nut==5 else 1}\n"
            f"  priority_id: 0\n  view_id: 1\n  temporal_id: 0\n  anchor_pic_flag: {anchor}\n  inter_view_flag: 0\n"
            f"  first_mb_in_slice: 0\n  slice_type: {slice_type}\n  pic_parameter_set_id: 1\n"
            f"  frame_num: {{bits: 10, absolute: {fn}}}\n{idr}"
            f"  pic_order_cnt: {{type: 0, bits: 4, absolute: {poc}}}\n{extra}"
            f"  slice_qp_delta: 0\n  macroblocks_cavlc:\n{mb}")

# SPS/PPS/subset-SPS/PPS header, with the log2_max*, max_num_ref_frames and the
# VUI bitstream_restriction (max_num_reorder_frames / max_dec_frame_buffering)
# matched to the real clip - these set the reorder / DPB-buffering window that
# decides when a base is force-bumped under paced pressure (the strand condition).
VUI = ("""  vui_parameters:
    overscan_appropriate_flag: -1
    pic_struct_present_flag: 0
    motion_vectors_over_pic_boundaries_flag: 1
    log2_max_mv_length_horizontal: 16
    log2_max_mv_length_vertical: 16
    max_num_reorder_frames: 1
    max_dec_frame_buffering: 4
""")
H = ("""--- # Copyright-safe structural replica (all-128 1x1-MB) of a real 3D-Blu-ray
# menu clip's decode structure: 24 short IDR sequences with log2_max_pic_order_cnt_lsb=4,
# so frames of different sequences collide on full POC with different frame_num.
# max_dec_frame_buffering=4 makes a base force-bump under paced pressure while its
# same-POC dependent partner is mispaired - the (FrameNum, POC) fix resolves it.
# Generated by transcode_structure.py from the clip's header structure only; every
# decoded sample is 128 by spec, so no original picture data is present.
- nal_ref_idc: 3
  nal_unit_type: 7
  profile_idc: 66
  constraint_set_flags: [0,0,0,0,0,0]
  level_idc: 4.1
  chroma_format_idc: 1
  bit_depth: {luma: 8, chroma: 8}
  log2_max_frame_num: 10
  pic_order_cnt_type: 0
  log2_max_pic_order_cnt_lsb: 4
  max_num_ref_frames: 2
  gaps_in_frame_num_value_allowed_flag: 1
  pic_size_in_mbs: {width: 1, height: 1}
  frame_mbs_only_flag: 1
  direct_8x8_inference_flag: 0
""" + VUI + """- nal_ref_idc: 3
  nal_unit_type: 8
  pic_parameter_set_id: 0
  entropy_coding_mode_flag: 0
  bottom_field_pic_order_in_frame_present_flag: 0
  num_slice_groups: 1
  num_ref_idx_default_active: {l0: 1, l1: 1}
  weighted_pred_flag: 0
  weighted_bipred_idc: 0
  pic_init_qp: 0
  chroma_qp_index_offset: 0
  deblocking_filter_control_present_flag: 0
  constrained_intra_pred_flag: 0
  redundant_pic_cnt_present_flag: 0
- nal_ref_idc: 3
  nal_unit_type: 15
  profile_idc: 128
  constraint_set_flags: [0,0,0,0,0,0]
  level_idc: 4.1
  chroma_format_idc: 1
  bit_depth: {luma: 8, chroma: 8}
  qpprime_y_zero_transform_bypass_flag: 0
  log2_max_frame_num: 10
  pic_order_cnt_type: 0
  log2_max_pic_order_cnt_lsb: 4
  max_num_ref_frames: 2
  gaps_in_frame_num_value_allowed_flag: 1
  pic_size_in_mbs: {width: 1, height: 1}
  frame_mbs_only_flag: 1
  direct_8x8_inference_flag: 0
""" + VUI + """  view_ids: [0,1]
  num_anchor_refs: {l0: 0, l1: 0}
  num_non_anchor_refs: {l0: 0, l1: 0}
  level_values_signalled:
    - idc: 4.1
      operation_points: [{temporal_id: 0, target_views: [0,1], num_views: 2}]
- nal_ref_idc: 3
  nal_unit_type: 8
  pic_parameter_set_id: 1
  entropy_coding_mode_flag: 0
  bottom_field_pic_order_in_frame_present_flag: 0
  num_slice_groups: 1
  num_ref_idx_default_active: {l0: 1, l1: 1}
  weighted_pred_flag: 0
  weighted_bipred_idc: 0
  pic_init_qp: 0
  chroma_qp_index_offset: 0
  deblocking_filter_control_present_flag: 0
  constrained_intra_pred_flag: 0
  redundant_pic_cnt_present_flag: 0
""")


parts = [H]
for nut, nri, fn, lsb in FRAMES:
    parts.append(base_slice(nut, fn, lsb, nri))
    parts.append(dep_slice(nut, fn, lsb, nri))
open(out, "w").write("".join(parts))
print(f"wrote {out}: {len(FRAMES)} frames (all-128 1x1-MB same-POC pairing fixture)")
