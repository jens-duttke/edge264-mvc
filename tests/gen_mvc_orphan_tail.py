#!/usr/bin/env python3
# Emit the gen_avc.py YAML for an MVC stream whose tail carries dependent-view
# access units with NO base view - the shape a byte-trimmed 3D Blu-ray SSIF
# leaves behind (the interleaving is chunked, so a prefix cut ends with a
# surplus of dependent-view AUs past the last base AU; a two-file base+dependent
# feed with unequal lengths produces the same).
#
# Under multithreading this used to be fatal: get_frame's orphan-dependent
# valve dropped such a dependent from to_get_frames/output_frames while its
# decode tasks were still running (or while it was still being parsed), the
# parser reallocated the freed DPB slot for the next picture, and the stale
# tasks' remaining_mbs subtractions then corrupted the new occupant's counter -
# the frame never finalized, every later task depending on it stayed un-ready,
# and once all 16 task slots filled the parser deadlocked in its task-slot wait
# (0% CPU, edge264_decode_NAL never returns). Single-threaded decoding of the
# same bytes is fine, so the fixture asserts the multithreaded modes.
#
# Construction: a 16x16-MB two-view body (IDR pair + 3 P pairs), then N
# dependent-only tail AUs alternating reference / non-reference. The reference
# ones chain P-slice dependencies (so a poisoned frame blocks later tasks); the
# non-reference ones take the immediate-output bump into the dependent output
# queue (the only queueing path without a paired base), which is what exposes
# them to the orphan valve mid-decode. Each tail picture is split into 4 slices
# of intra macroblocks so its worker tasks stay busy long enough for the
# drop-then-reallocate race to hit. Every macroblock is residual-free (DC=128),
# the same independent-ground-truth construction as mvc-synthetic.
#
# Expected delivery: the 4 body stereo pairs (4 base frames); every tail
# dependent is dropped (it has no base to pair with), like ffmpeg. Pre-fix,
# multithreaded decoding deadlocks instead (the liveness harness reports it
# via its fork timeout).
#
# Usage: gen_mvc_orphan_tail.py <out.yaml> [num_tail_aus]
import sys

N_TAIL = int(sys.argv[2]) if len(sys.argv) > 2 else 24
W, H = 16, 16                 # picture size in macroblocks
NMBS = W * H
SLICES = 4                    # slices per tail dependent picture
BODY = 4                      # stereo body AUs (1 IDR + 3 P)

def block(lines):
    return "\n".join(lines) + "\n\n"

out = [block([
    "--- # MVC orphan-dependent tail (%dx%d MBs, %d stereo body AUs, %d base-less" % (W, H, BODY, N_TAIL),
    "# dependent tail AUs). See tests/gen_mvc_orphan_tail.py for the full story:",
    "# a trimmed 3D-BD stream ends in dependent-view AUs with no base; the orphan",
    "# valve must not drop one that is still being parsed/decoded, and the DPB",
    "# slot allocator must not reallocate a slot with in-flight tasks. Pre-fix",
    "# this deadlocked multithreaded decoding; expected: %d base frames." % BODY])]

# SPS (base view): level 1.2 keeps the derived DPB small (MaxDpbMbs 891 / 256
# MBs = 3 frames), so the tail reaches the immediate-output fullness path early.
out.append(block([
    "- nal_ref_idc: 3", "  nal_unit_type: 7", "  profile_idc: 66",
    "  constraint_set_flags: [0,0,0,0,0,0]", "  level_idc: 1.2",
    "  log2_max_frame_num: 6", "  pic_order_cnt_type: 0",
    "  log2_max_pic_order_cnt_lsb: 6", "  max_num_ref_frames: 2",
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
    "  qpprime_y_zero_transform_bypass_flag: 0", "  log2_max_frame_num: 6",
    "  pic_order_cnt_type: 0", "  log2_max_pic_order_cnt_lsb: 6",
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

# --- body: 1 stereo IDR + (BODY-1) stereo P AUs, single-slice ---
for i in range(BODY):
    idr = (i == 0)
    out.append("# --- body AU %d ---\n" % i)
    out.append(prefix_nal(idr))
    if idr:
        out.append(block([
            "- nal_ref_idc: 3", "  nal_unit_type: 5", "  first_mb_in_slice: 0",
            "  slice_type: 2", "  pic_parameter_set_id: 0",
            "  frame_num: {bits: 6, absolute: 0}", "  idr_pic_id: 0",
            "  pic_order_cnt: {type: 0, bits: 6, absolute: 0}",
            "  no_output_of_prior_pics_flag: 0", "  long_term_reference_flag: 0",
            "  slice_qp_delta: 0"] + intra_mbs(NMBS)))
        out.append(block([
            "- nal_ref_idc: 3", "  nal_unit_type: 20", "  non_idr_flag: 0",
            "  priority_id: 0", "  view_id: 1", "  temporal_id: 0",
            "  anchor_pic_flag: 1", "  inter_view_flag: 0", "  first_mb_in_slice: 0",
            "  slice_type: 2", "  pic_parameter_set_id: 1",
            "  frame_num: {bits: 6, absolute: 0}", "  idr_pic_id: 0",
            "  pic_order_cnt: {type: 0, bits: 6, absolute: 0}",
            "  no_output_of_prior_pics_flag: 0", "  long_term_reference_flag: 0",
            "  slice_qp_delta: 0"] + intra_mbs(NMBS)))
    else:
        out.append(block([
            "- nal_ref_idc: 2", "  nal_unit_type: 1", "  first_mb_in_slice: 0",
            "  slice_type: 0", "  pic_parameter_set_id: 0",
            "  frame_num: {bits: 6, absolute: %d}" % i,
            "  pic_order_cnt: {type: 0, bits: 6, absolute: %d}" % (2 * i),
            "  num_ref_idx_active: {override_flag: 0, l0: 1}",
            "  slice_qp_delta: 0"] + skip_mbs(NMBS)))
        out.append(block([
            "- nal_ref_idc: 2", "  nal_unit_type: 20", "  non_idr_flag: 1",
            "  priority_id: 0", "  view_id: 1", "  temporal_id: 0",
            "  anchor_pic_flag: 0", "  inter_view_flag: 0", "  first_mb_in_slice: 0",
            "  slice_type: 0", "  pic_parameter_set_id: 1",
            "  frame_num: {bits: 6, absolute: %d}" % i,
            "  pic_order_cnt: {type: 0, bits: 6, absolute: %d}" % (2 * i),
            "  num_ref_idx_active: {override_flag: 0, l0: 1}",
            "  slice_qp_delta: 0"] + skip_mbs(NMBS)))

# --- tail: N_TAIL dependent-only AUs, alternating reference / non-reference ---
# frame_num follows 7.4.3: both a non-reference picture and the following
# reference picture carry PrevRefFrameNum+1; only a reference advances it.
prfn = BODY - 1
for j in range(N_TAIL):
    ref = (j % 2 == 0)
    fn = (prfn + 1) % 64
    if ref:
        prfn = fn
    poc = 2 * (BODY + j)
    out.append("# --- tail dependent AU %d (%s) ---\n" % (j, "ref" if ref else "nonref"))
    # Uneven slice split: a large first slice keeps its worker task in flight
    # while the parser races ahead through the small trailing slices, the
    # caller's drain (where the orphan valve fires), and the next AU's slot
    # allocation - the widest possible window for the drop-then-reallocate race
    # this fixture guards.
    bounds = [0, NMBS * 3 // 4, NMBS * 7 // 8, NMBS * 15 // 16, NMBS]
    for s in range(len(bounds) - 1):
        # P slices (temporal dependency chain via RefPicList) of intra
        # macroblocks (real decode work, keeps the worker busy)
        out.append(block([
            "- nal_ref_idc: %d" % (2 if ref else 0), "  nal_unit_type: 20",
            "  non_idr_flag: 1", "  priority_id: 0", "  view_id: 1",
            "  temporal_id: 0", "  anchor_pic_flag: 0", "  inter_view_flag: 0",
            "  first_mb_in_slice: %d" % bounds[s],
            "  slice_type: 0", "  pic_parameter_set_id: 1",
            "  frame_num: {bits: 6, absolute: %d}" % fn,
            "  pic_order_cnt: {type: 0, bits: 6, absolute: %d}" % poc,
            "  num_ref_idx_active: {override_flag: 0, l0: 1}",
            "  slice_qp_delta: 0"] + intra_in_p_mbs(bounds[s + 1] - bounds[s])))

open(sys.argv[1], "w").write("".join(out))
print("wrote %s (%d body AUs + %d tail dependent AUs, %dx%d MBs, %d slices/tail-pic)"
      % (sys.argv[1], BODY, N_TAIL, W, H, SLICES))
