#!/usr/bin/env python3
# Emit the gen_avc.py YAML for a stereo MVC stream in which one dependent-view
# slice carries a corrupt header - the shape a 3D Blu-ray rip with a damaged
# right-eye slice has (the base view stays intact, so 2D players never notice).
#
# The damaged slice is the second slice of a two-slice dependent picture. Its
# frame_num and pic_order_cnt are garbage, so the decoder used to take it for a
# new dependent picture after a frame_num gap: it closed the open picture,
# inserted non-existing frames, and seeded PrevRefFrameNum / prevPicOrderCnt of
# the dependent view from the garbage. Every later dependent POC then
# mismatched its base, the base-driven pairing never queued them, and the DPB
# filled until edge264_decode_NAL returned ENOBUFS forever: edge264_test spun
# at 100% CPU on a file, and cut the movie short on stdin.
#
# Expected: the corrupt slice is rejected (EBADMSG) without touching decoder
# state, its picture is concealed from the base view, and all BODY stereo pairs
# are delivered, identically in single- and multi-threaded decoding.
#
# With "idr_pic_id" the stream instead restarts with an IDR access unit every
# IDR_EVERY AUs, and the damaged slice belongs to a dependent IDR picture: its
# frame_num and pic_order_cnt stay intact and only idr_pic_id is garbage. The
# slice is intra, so the frame_num test cannot see it; the decoder used to open
# a second dependent picture with the (FrameNum, POC) of the first, which never
# paired with a base, and jammed the same way.
#
# Usage: gen_mvc_corrupt_dep_slice.py <out.yaml> [idr_pic_id]
import sys

IDR_DAMAGE = sys.argv[2:] == ["idr_pic_id"]

W, H = 16, 16                 # picture size in macroblocks
NMBS = W * H
BODY = 40                     # stereo AUs (1 IDR + 39 P)
BAD_AU = 8                    # AU whose second dependent slice is damaged
BITS = 8
IDR_EVERY = 8 if IDR_DAMAGE else 1 << 30   # BAD_AU is an IDR access unit

def block(lines):
    return "\n".join(lines) + "\n\n"

out = [block([
    "--- # MVC stream with one corrupt dependent-view %s (AU %d of %d)." % (
        "idr_pic_id" if IDR_DAMAGE else "slice header", BAD_AU, BODY),
    "# See tests/gen_mvc_corrupt_dep_slice.py. Expected: %d stereo pairs." % BODY])]
# SPS (base view): level 1.2 keeps the derived DPB small (MaxDpbMbs 891 / 256
# MBs = 3 frames), so the tail reaches the immediate-output fullness path early.
out.append(block([
    "- nal_ref_idc: 3", "  nal_unit_type: 7", "  profile_idc: 66",
    "  constraint_set_flags: [0,0,0,0,0,0]", "  level_idc: 1.2",
    "  log2_max_frame_num: %d" % BITS, "  pic_order_cnt_type: 0",
    "  log2_max_pic_order_cnt_lsb: %d" % BITS, "  max_num_ref_frames: 2",
    "  gaps_in_frame_num_value_allowed_flag: 0",
    "  pic_size_in_mbs: {width: %d, height: %d}" % (W, H),
    "  frame_mbs_only_flag: 1", "  direct_8x8_inference_flag: 0"]))

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

# Subset SPS (MVC, Stereo High 128), 2 views, no inter-view prediction (the
# dependent view predicts temporally only, so a base-less tail dependent P
# slice is legal and reaches the worker pool).
out.append(block([
    "- nal_ref_idc: 3", "  nal_unit_type: 15", "  profile_idc: 128",
    "  constraint_set_flags: [0,0,0,0,0,0]", "  level_idc: 1.2",
    "  chroma_format_idc: 1", "  bit_depth: {luma: 8, chroma: 8}",
    "  qpprime_y_zero_transform_bypass_flag: 0", "  log2_max_frame_num: %d" % BITS,
    "  pic_order_cnt_type: 0", "  log2_max_pic_order_cnt_lsb: %d" % BITS,
    "  max_num_ref_frames: 2", "  gaps_in_frame_num_value_allowed_flag: 0",
    "  pic_size_in_mbs: {width: %d, height: %d}" % (W, H),
    "  frame_mbs_only_flag: 1", "  direct_8x8_inference_flag: 0",
    "  view_ids: [0,1]",
    "  num_anchor_refs: {l0: 0, l1: 0}", "  num_non_anchor_refs: {l0: 0, l1: 0}",
    "  level_values_signalled:",
    "    - idc: 1.2",
    "      operation_points: [{temporal_id: 0, target_views: [0,1], num_views: 2}]"]))
out.append(pps(1))

def intra_mbs(n):
    lines = ["  macroblocks_cavlc:"]
    for _ in range(n):
        lines += ["  - mb_type: 0",
                  "    rem_intra4x4_pred_modes: [-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1]",
                  "    intra_chroma_pred_mode: 0", "    coded_block_pattern: 0"]
    return lines

def intra_in_p_mbs(n):
    # mb_skip_run precedes every coded macroblock in a CAVLC P slice; mb_type 5
    # is I_NxN in a P slice (real intra decode work, no reference reads).
    lines = ["  macroblocks_cavlc:"]
    for _ in range(n):
        lines += ["  - mb_skip_run: 0", "    mb_type: 5",
                  "    rem_intra4x4_pred_modes: [-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1]",
                  "    intra_chroma_pred_mode: 0", "    coded_block_pattern: 0"]
    return lines

def skip_mbs(n):
    return ["  macroblocks_cavlc:", "  - mb_skip_run: %d" % n] + ["  - {}"] * (n - 1)

def prefix_nal(idr):
    return block([
        "- nal_ref_idc: %d" % (3 if idr else 2), "  nal_unit_type: 14",
        "  non_idr_flag: %d" % (0 if idr else 1), "  priority_id: 0", "  view_id: 0",
        "  temporal_id: 0", "  anchor_pic_flag: %d" % (1 if idr else 0),
        "  inter_view_flag: 1"])

def base_slice(i):
    if i % IDR_EVERY == 0:
        return block([
            "- nal_ref_idc: 3", "  nal_unit_type: 5", "  first_mb_in_slice: 0",
            "  slice_type: 2", "  pic_parameter_set_id: 0",
            "  frame_num: {bits: %d, absolute: 0}" % BITS, "  idr_pic_id: %d" % (i // IDR_EVERY),
            "  pic_order_cnt: {type: 0, bits: %d, absolute: 0}" % BITS,
            "  no_output_of_prior_pics_flag: 0", "  long_term_reference_flag: 0",
            "  slice_qp_delta: 0"] + intra_mbs(NMBS))
    return block([
        "- nal_ref_idc: 2", "  nal_unit_type: 1", "  first_mb_in_slice: 0",
        "  slice_type: 0", "  pic_parameter_set_id: 0",
        "  frame_num: {bits: %d, absolute: %d}" % (BITS, i % IDR_EVERY),
        "  pic_order_cnt: {type: 0, bits: %d, absolute: %d}" % (BITS, 2 * (i % IDR_EVERY)),
        "  num_ref_idx_active: {override_flag: 0, l0: 1}",
        "  slice_qp_delta: 0"] + skip_mbs(NMBS))

def dep_slice(i, first_mb, n, frame_num, poc, idr_pic_id):
    if i % IDR_EVERY == 0:
        return block([
            "- nal_ref_idc: 3", "  nal_unit_type: 20", "  non_idr_flag: 0",
            "  priority_id: 0", "  view_id: 1", "  temporal_id: 0",
            "  anchor_pic_flag: 1", "  inter_view_flag: 0",
            "  first_mb_in_slice: %d" % first_mb,
            "  slice_type: 2", "  pic_parameter_set_id: 1",
            "  frame_num: {bits: %d, absolute: 0}" % BITS, "  idr_pic_id: %d" % idr_pic_id,
            "  pic_order_cnt: {type: 0, bits: %d, absolute: 0}" % BITS,
            "  no_output_of_prior_pics_flag: 0", "  long_term_reference_flag: 0",
            "  slice_qp_delta: 0"] + intra_mbs(n))
    return block([
        "- nal_ref_idc: 2", "  nal_unit_type: 20", "  non_idr_flag: 1",
        "  priority_id: 0", "  view_id: 1", "  temporal_id: 0",
        "  anchor_pic_flag: 0", "  inter_view_flag: 0",
        "  first_mb_in_slice: %d" % first_mb,
        "  slice_type: 0", "  pic_parameter_set_id: 1",
        "  frame_num: {bits: %d, absolute: %d}" % (BITS, frame_num),
        "  pic_order_cnt: {type: 0, bits: %d, absolute: %d}" % (BITS, poc),
        "  num_ref_idx_active: {override_flag: 0, l0: 1}",
        "  slice_qp_delta: 0"] + skip_mbs(n))

for i in range(BODY):
    out.append("# --- AU %d%s ---\n" % (i, " (second dependent slice damaged)" if i == BAD_AU else ""))
    out.append(prefix_nal(i % IDR_EVERY == 0))
    out.append(base_slice(i))
    fn, idr = i % IDR_EVERY, i // IDR_EVERY
    out.append(dep_slice(i, 0, NMBS // 2, fn, 2 * fn, idr))
    if i != BAD_AU:
        out.append(dep_slice(i, NMBS // 2, NMBS // 2, fn, 2 * fn, idr))
    elif IDR_DAMAGE:
        out.append(dep_slice(i, NMBS // 2, NMBS // 2, fn, 2 * fn, idr + 4321))
    else:
        out.append(dep_slice(i, NMBS // 2, NMBS // 2, fn + 100, 2 * fn + 90, idr))

open(sys.argv[1], "w").write("".join(out))
print("wrote %s (%d stereo AUs, corrupt dependent slice in AU %d)" % (sys.argv[1], BODY, BAD_AU))
