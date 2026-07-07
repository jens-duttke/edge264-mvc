#!/usr/bin/env python3
# Generator for the mvc_baseless_dependent liveness fixture.
#
# Emits a deliberately corrupt MVC bitstream that carries a subset SPS (NAL type
# 15) and inter-coded dependent-view slices (NAL type 20, P) but NO base-view
# SPS (NAL type 7) and no base-view slices at all - the signature of a real
# stream whose base view is undecodable (ffmpeg reports "sps_id N out of range"
# and resolves width=0/height=0, producing no frame). Reproduced from
# FFmpeg's public 3D/AVC_codec_in_m2ts_not_recognized sample.
#
# Every dependent P slice's inter-view reference resolves to the missing base
# picture; with no base ever decoded (basePic stays -1) the out-of-range
# RefPicList fix-up falls back to the slice's own not-yet-decoded frame slot, so
# each slice's decode task depends on its own frame. In the multithreaded path
# that dependency never clears - the worker never runs the task, the frame never
# completes. The fixture packs ONE picture into 18 single-macroblock slices
# (> the 16 task slots) so the self-dependent tasks pile up and the parser
# blocks forever waiting for a free task slot, reproducing the exact deadlock,
# while keeping the DPB at a single frame (no unrelated fullness pressure).
#
# A correct decoder rejects each base-less inter-coded dependent slice as corrupt
# (EBADMSG) and terminates. Usage:
#   python3 tests/gen_mvc_baseless.py tests/liveness/mvc_baseless_dependent.yaml

import sys

NUM_SLICES = 18  # > 16 task slots, so the self-dependent tasks exhaust the pool
PIC_HEIGHT_MBS = NUM_SLICES  # 1 x NUM_SLICES MBs, one macroblock per slice

HEADER = """--- # MVC dependent view with NO base view (undecodable base).
# A subset SPS (NAL type 15) plus inter-coded dependent-view slices (NAL type 20,
# P) but no base-view SPS (type 7) and no base-view slices - every dependent
# slice references a base picture that is never created. Reproduced from FFmpeg's
# 3D/AVC_codec_in_m2ts_not_recognized sample (ffmpeg: "sps_id 1 out of range",
# width=0/height=0, no frame). Without a base, each dependent P slice's inter-view
# reference falls back to its own not-yet-decoded frame slot, so its decode task
# depends on its own frame; multithreaded, that never clears and the tasks pile
# up until the parser deadlocks waiting for a free task slot (edge264_decode_NAL
# never returns). One picture is split into %d single-MB slices to exceed the 16
# task slots. A correct decoder rejects each base-less inter-coded dependent slice
# (EBADMSG) and terminates, delivering 0 frames (like ffmpeg).

- nal_ref_idc: 3
  nal_unit_type: 15
  profile_idc: 128
  constraint_set_flags: [0,0,0,0,0,0]
  level_idc: 3.0
  chroma_format_idc: 1
  bit_depth: {luma: 8, chroma: 8}
  qpprime_y_zero_transform_bypass_flag: 0
  log2_max_frame_num: 4
  pic_order_cnt_type: 0
  log2_max_pic_order_cnt_lsb: 4
  max_num_ref_frames: 2
  gaps_in_frame_num_value_allowed_flag: 0
  pic_size_in_mbs: {width: 1, height: %d}
  frame_mbs_only_flag: 1
  direct_8x8_inference_flag: 0
  view_ids: [0,1]
  num_anchor_refs: {l0: 0, l1: 0}
  num_non_anchor_refs: {l0: 0, l1: 0}
  level_values_signalled:
    - idc: 3.0
      operation_points: [{temporal_id: 0, target_views: [0,1], num_views: 2}]

- nal_ref_idc: 3
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
"""

SLICE = """
# dependent-view slice %d/%d of the single base-less picture (no base view exists)
- nal_ref_idc: 3
  nal_unit_type: 20
  non_idr_flag: 1
  priority_id: 0
  view_id: 1
  temporal_id: 0
  anchor_pic_flag: 0
  inter_view_flag: 0
  first_mb_in_slice: %d
  slice_type: 0
  pic_parameter_set_id: 0
  frame_num: {bits: 4, absolute: 0}
  pic_order_cnt: {type: 0, bits: 4, absolute: 0}
  num_ref_idx_active: {override_flag: 0, l0: 1}
  slice_qp_delta: 0
  macroblocks_cavlc:
  - mb_skip_run: 0
    mb_type: 0
    ref_idx: {}
    mvds: [[0,0],]
    coded_block_pattern: 0
"""

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/liveness/mvc_baseless_dependent.yaml"
    text = HEADER % (NUM_SLICES, PIC_HEIGHT_MBS)
    for i in range(NUM_SLICES):
        text += SLICE % (i + 1, NUM_SLICES, i)
    with open(out, "w", newline="\n") as f:
        f.write(text)
    print(f"wrote {out} ({NUM_SLICES} base-less dependent slices)")

main()
