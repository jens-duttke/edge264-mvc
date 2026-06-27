#!/usr/bin/env python3
# Emit the gen_avc.py YAML for a small-resolution (1x1-macroblock) two-view MVC
# stream long enough to fill the DPB before end-of-stream. A tiny picture infers
# a large reorder window (MaxDpbMbs / PicSizeInMbs, capped at 16), so the
# per-view fullness bump queues a base while its already-decoded dependent still
# sits unbumped - the stall fixed in edge264.c (edge264_get_frame). Pre-fix the
# decoder delivers 0 frames then spins on ENOBUFS; post-fix it delivers all N.
#
# Every macroblock is residual-free (I_NxN DC=128 IDRs, zero-motion no-residual
# P/B copies), so the decoded output is every sample = 128 by spec - the same
# independent-ground-truth construction as tests/conformance/mvc-synthetic.
#
# Usage: gen_mvc_smallres_stall.py <out.yaml> [num_access_units]
import sys

N = int(sys.argv[2]) if len(sys.argv) > 2 else 20

def block(lines):
    return "\n".join(lines) + "\n\n"

out = [block([
    "--- # Small-resolution MVC DPB-fill stall (1x1 MB, two views, %d access units)." % N,
    "# A tiny picture infers a large reorder window, so the per-view fullness bump",
    "# queues a base while its decoded dependent is still unbumped; pre-fix the",
    "# decoder stalls at 0 frames, post-fix it delivers all %d stereo pairs." % N,
    "# Residual-free (every sample = 128 by spec)."])]

# SPS (base view), profile Baseline 66
out.append(block([
    "- nal_ref_idc: 3", "  nal_unit_type: 7", "  profile_idc: 66",
    "  constraint_set_flags: [0,0,0,0,0,0]", "  level_idc: 1.0",
    "  log2_max_frame_num: 6", "  pic_order_cnt_type: 0",
    "  log2_max_pic_order_cnt_lsb: 6", "  max_num_ref_frames: 2",
    "  gaps_in_frame_num_value_allowed_flag: 0",
    "  pic_size_in_mbs: {width: 1, height: 1}", "  frame_mbs_only_flag: 1",
    "  direct_8x8_inference_flag: 0"]))

# PPS id 0 (base view)
def pps(pid):
    return block([
        "- nal_ref_idc: 3", "  nal_unit_type: 8", "  pic_parameter_set_id: %d" % pid,
        "  entropy_coding_mode_flag: 0",
        "  bottom_field_pic_order_in_frame_present_flag: 0", "  num_slice_groups: 1",
        "  num_ref_idx_default_active: {l0: 1, l1: 1}", "  weighted_pred_flag: 0",
        "  weighted_bipred_idc: 0", "  pic_init_qp: 0", "  chroma_qp_index_offset: 0",
        "  deblocking_filter_control_present_flag: 0", "  constrained_intra_pred_flag: 0",
        "  redundant_pic_cnt_present_flag: 0"])
out.append(pps(0))

# Subset SPS (MVC, Stereo High 128), 2 views
out.append(block([
    "- nal_ref_idc: 3", "  nal_unit_type: 15", "  profile_idc: 128",
    "  constraint_set_flags: [0,0,0,0,0,0]", "  level_idc: 1.0",
    "  chroma_format_idc: 1", "  bit_depth: {luma: 8, chroma: 8}",
    "  qpprime_y_zero_transform_bypass_flag: 0", "  log2_max_frame_num: 6",
    "  pic_order_cnt_type: 0", "  log2_max_pic_order_cnt_lsb: 6",
    "  max_num_ref_frames: 2", "  gaps_in_frame_num_value_allowed_flag: 0",
    "  pic_size_in_mbs: {width: 1, height: 1}", "  frame_mbs_only_flag: 1",
    "  direct_8x8_inference_flag: 0", "  view_ids: [0,1]",
    "  num_anchor_refs: {l0: 0, l1: 0}", "  num_non_anchor_refs: {l0: 0, l1: 0}",
    "  level_values_signalled:",
    "    - idc: 1.0",
    "      operation_points: [{temporal_id: 0, target_views: [0,1], num_views: 2}]"]))
out.append(pps(1))

intra_mb = ["  macroblocks_cavlc:",
            "  - mb_type: 0",
            "    rem_intra4x4_pred_modes: [-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1]",
            "    intra_chroma_pred_mode: 0", "    coded_block_pattern: 0"]
p_mb = ["  macroblocks_cavlc:", "  - mb_skip_run: 0", "    mb_type: 0",
        "    ref_idx: {}", "    mvds: [[0,0],]", "    coded_block_pattern: 0"]
b_mb = ["  macroblocks_cavlc:", "  - mb_skip_run: 0", "    mb_type: 3",
        '    ref_idx: {"0":0,"4":1}', "    mvds: [[0,0],[0,0]]",
        "    coded_block_pattern: 0"]

for i in range(N):
    idr = (i == 0)
    out.append("# --- AU %d ---\n" % i)
    # prefix NAL (NAL 14) for the base view
    out.append(block([
        "- nal_ref_idc: 3" if idr else "- nal_ref_idc: 2", "  nal_unit_type: 14",
        "  non_idr_flag: %d" % (0 if idr else 1), "  priority_id: 0", "  view_id: 0",
        "  temporal_id: 0", "  anchor_pic_flag: %d" % (1 if idr else 0),
        "  inter_view_flag: 1"]))
    # base view slice
    if idr:
        out.append(block([
            "- nal_ref_idc: 3", "  nal_unit_type: 5", "  first_mb_in_slice: 0",
            "  slice_type: 2", "  pic_parameter_set_id: 0",
            "  frame_num: {bits: 6, absolute: 0}", "  idr_pic_id: 0",
            "  pic_order_cnt: {type: 0, bits: 6, absolute: 0}",
            "  no_output_of_prior_pics_flag: 0", "  long_term_reference_flag: 0",
            "  slice_qp_delta: 0"] + intra_mb))
    else:
        out.append(block([
            "- nal_ref_idc: 2", "  nal_unit_type: 1", "  first_mb_in_slice: 0",
            "  slice_type: 0", "  pic_parameter_set_id: 0",
            "  frame_num: {bits: 6, absolute: %d}" % i,
            "  pic_order_cnt: {type: 0, bits: 6, absolute: %d}" % i,
            "  num_ref_idx_active: {override_flag: 0, l0: 1}", "  slice_qp_delta: 0"] + p_mb))
    # dependent view slice (NAL 20)
    if idr:
        out.append(block([
            "- nal_ref_idc: 3", "  nal_unit_type: 20", "  non_idr_flag: 0",
            "  priority_id: 0", "  view_id: 1", "  temporal_id: 0",
            "  anchor_pic_flag: 1", "  inter_view_flag: 0", "  first_mb_in_slice: 0",
            "  slice_type: 2", "  pic_parameter_set_id: 1",
            "  frame_num: {bits: 6, absolute: 0}", "  idr_pic_id: 0",
            "  pic_order_cnt: {type: 0, bits: 6, absolute: 0}",
            "  no_output_of_prior_pics_flag: 0", "  long_term_reference_flag: 0",
            "  slice_qp_delta: 0"] + intra_mb))
    else:
        out.append(block([
            "- nal_ref_idc: 2", "  nal_unit_type: 20", "  non_idr_flag: 1",
            "  priority_id: 0", "  view_id: 1", "  temporal_id: 0",
            "  anchor_pic_flag: 0", "  inter_view_flag: 0", "  first_mb_in_slice: 0",
            "  slice_type: 1", "  pic_parameter_set_id: 1",
            "  frame_num: {bits: 6, absolute: %d}" % i,
            "  pic_order_cnt: {type: 0, bits: 6, absolute: %d}" % i,
            "  direct_spatial_mv_pred_flag: 0",
            "  num_ref_idx_active: {override_flag: 1, l0: 2, l1: 2}",
            "  slice_qp_delta: 0"] + b_mb))

open(sys.argv[1], "w").write("".join(out))
print("wrote %s (%d access units)" % (sys.argv[1], N))
