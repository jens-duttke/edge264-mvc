#!/usr/bin/env python3
# Generate a copyright-safe, all-128, 1x1-macroblock Stereo-High MVC stream that
# reproduces the base-view display-order regression from issue #2: the base view
# is coded with decode order != display order (POCs decoded 0, 2, 6, 4), so a
# higher-POC picture (6) is decoded and its dependent queued before a lower-POC
# picture (4). The removed catch_up_orphaned_bases pass reverse-paired that base
# at queue time, stamping its display rank in decode rather than display order and
# emitting frame 6 before frame 4 (an adjacent swap). Output is now strictly
# base-driven (bump_frame never queues a dependent ahead of its base), so the pair
# is emitted in display order: 0, 2, 4, 6. FRAMES below is the per-frame decode
# STRUCTURE (nal_unit_type, nal_ref_idc, frame_num, pic_order_cnt_lsb) only - no
# picture data; every decoded sample is 128 by spec. Two IDR sequences exercise
# the reorder across a POC reset. Usage:
#   python3 tests/gen_mvc_ordering.py tests/conformance/mvc-synthetic/mvc_display_ordering.yaml
import sys
out = sys.argv[1] if len(sys.argv) > 1 else "tests/conformance/mvc-synthetic/mvc_display_ordering.yaml"

# (nal_unit_type, nal_ref_idc, frame_num, pic_order_cnt_lsb)
# decode order 0, 2, 6, 4 -> display order 0, 2, 4, 6, twice (across an IDR reset).
FRAMES = [
    (5, 1, 0, 0), (1, 1, 1, 2), (1, 1, 2, 6), (1, 0, 3, 4),
    (5, 1, 0, 0), (1, 1, 1, 2), (1, 1, 2, 6), (1, 0, 3, 4),
]

# An I_PCM macroblock carrying a single flat luma value, so each picture has a
# distinct, content-addressable hash and a reordered emission is visible. I_PCM
# mb_type is 25 in an I slice, 30 in a P slice; 16x16 luma + 2x 8x8 chroma (4:2:0).
def pcm_mb(slice_type, y):
    Y = ",".join(["%d" % y] * 256)
    C = ",".join(["128"] * 64)
    skip = "" if slice_type == 2 else "mb_skip_run: 0\n    "  # P slices signal no skip first
    return (f"  - {skip}mb_type: {25 if slice_type == 2 else 30}\n"
            f"    pcm_samples: {{bits_Y: 8, bits_C: 8, Y: [{Y}], Cb: [{C}], Cr: [{C}]}}\n")

def base_slice(nut, fn, poc, nri):
    prefix_type = 14
    slice_type = 2 if nut == 5 else 0   # I for IDR, P otherwise
    idr = "  idr_pic_id: 0\n" if nut == 5 else ""
    anchor = 1 if nut == 5 else 0
    inter_view = 0 if nut == 5 else 1
    extra = ("  no_output_of_prior_pics_flag: 0\n  long_term_reference_flag: 0\n"
             if nut == 5 else "  num_ref_idx_active: {override_flag: 0, l0: 1}\n")
    # distinct luma per display position (POC) so a reorder changes the hash sequence
    mb = pcm_mb(slice_type, 16 + poc * 8)
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
    mb = pcm_mb(slice_type, 24 + poc * 8)
    return (f"- nal_ref_idc: {nri}\n  nal_unit_type: 20\n  non_idr_flag: {0 if nut==5 else 1}\n"
            f"  priority_id: 0\n  view_id: 1\n  temporal_id: 0\n  anchor_pic_flag: {anchor}\n  inter_view_flag: 0\n"
            f"  first_mb_in_slice: 0\n  slice_type: {slice_type}\n  pic_parameter_set_id: 1\n"
            f"  frame_num: {{bits: 10, absolute: {fn}}}\n{idr}"
            f"  pic_order_cnt: {{type: 0, bits: 4, absolute: {poc}}}\n{extra}"
            f"  slice_qp_delta: 0\n  macroblocks_cavlc:\n{mb}")

VUI = ("""  vui_parameters:
    overscan_appropriate_flag: -1
    pic_struct_present_flag: 0
    motion_vectors_over_pic_boundaries_flag: 1
    log2_max_mv_length_horizontal: 16
    log2_max_mv_length_vertical: 16
    max_num_reorder_frames: 1
    max_dec_frame_buffering: 4
""")
H = ("""--- # Copyright-safe structural fixture (all-128 1x1-MB) for issue #2's base-view
# display-order regression: the base view is coded decode-order 0, 2, 6, 4 so a
# higher-POC picture is decoded (and its dependent queued) before a lower-POC one.
# Every decoded sample is 128 by spec, so no original picture data is present.
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
print(f"wrote {out}: {len(FRAMES)} frames (all-128 1x1-MB display-ordering fixture)")
