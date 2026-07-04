#!/usr/bin/env python3
# Synthesize a valid stream ending in a tiny recovery_point SEI, reproducing the
# false EBADMSG fixed in parse_sei (edge264_sei.c).
#
# parse_sei skips an unhandled SEI message by consuming payloadSize bytes, but
# the skip loop was bounded by the bitstream reader's REFILL pointer (gb.CPB)
# instead of its read position. An SEI NAL small enough to sit entirely in the
# bit cache has CPB == end already, so the guard skipped nothing: the payload
# byte was re-parsed as a bogus follow-on message, the reader was left
# mid-syntax, rbsp_end failed and the whole SEI NAL returned EBADMSG - failing
# an otherwise valid stream. Real 3D Blu-rays close every base-view access unit
# with exactly such a tiny recovery_point SEI, so this fired on essentially
# every access unit (only in the logging path - parse_sei runs only with a log
# callback, which is why the fixture lives in tests/asan, decoded by
# asan_check.c with a log callback set).
#
# The SEI RBSP here is byte-identical to the one shipped by those discs:
#   payloadType = 6 (recovery_point, unhandled -> skipped), payloadSize = 1,
#   one payload byte 0xc4, then the rbsp_trailing 0x80 -> a 5-byte NAL that fits
#   wholly in the cache, so CPB == end when the skip runs.
# The stream is a valid SPS + PPS + that SEI (no slice needed: the defect is the
# SEI NAL misreturning EBADMSG). asan_check asserts this "clean" fixture decodes
# with zero EBADMSG; the buggy skip returns EBADMSG on the SEI and fails it.
#
# Usage: gen_sei_recovery_point.py <out.264>
import sys

class BW:
    def __init__(self): self.bits = []
    def u(self, n, v):
        for i in range(n - 1, -1, -1): self.bits.append((v >> i) & 1)
    def u1(self, v): self.bits.append(v & 1)
    def ue(self, v):
        v += 1; n = v.bit_length()
        for _ in range(n - 1): self.bits.append(0)
        for i in range(n - 1, -1, -1): self.bits.append((v >> i) & 1)
    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)
    def trailing(self):
        self.bits.append(1)
        while len(self.bits) % 8 != 0: self.bits.append(0)
    def bytes(self):
        assert len(self.bits) % 8 == 0
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for j in range(8): b = (b << 1) | self.bits[i + j]
            out.append(b)
        return out

def emulation_prevent(rbsp):
    out = bytearray(); zeros = 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3); zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return out

def nal(nut, ref_idc, rbsp):
    return b'\x00\x00\x01' + bytes([ref_idc << 5 | nut]) + emulation_prevent(rbsp)

# ---- SPS (High profile, 1x1 MB frame, 4:2:0 8-bit, no scaling/VUI) ----
s = BW()
s.u(8, 100)                 # profile_idc High
s.u(8, 0)                   # constraint flags + reserved
s.u(8, 10)                  # level_idc 1.0
s.ue(0)                     # seq_parameter_set_id
s.ue(1)                     # chroma_format_idc 4:2:0
s.ue(0)                     # bit_depth_luma_minus8
s.ue(0)                     # bit_depth_chroma_minus8
s.u1(0)                     # qpprime_y_zero_transform_bypass_flag
s.u1(0)                     # seq_scaling_matrix_present_flag
s.ue(0)                     # log2_max_frame_num_minus4
s.ue(0)                     # pic_order_cnt_type 0
s.ue(8)                     # log2_max_pic_order_cnt_lsb_minus4
s.ue(1)                     # max_num_ref_frames
s.u1(0)                     # gaps_in_frame_num_value_allowed_flag
s.ue(0)                     # pic_width_in_mbs_minus1  (1 MB wide)
s.ue(0)                     # pic_height_in_map_units_minus1 (1 MB tall)
s.u1(1)                     # frame_mbs_only_flag
s.u1(0)                     # direct_8x8_inference_flag
s.u1(0)                     # frame_cropping_flag
s.u1(0)                     # vui_parameters_present_flag
s.trailing()
sps = nal(7, 3, s.bytes())

# ---- PPS (CABAC) ----
p = BW()
p.ue(0)                     # pic_parameter_set_id
p.ue(0)                     # seq_parameter_set_id
p.u1(1)                     # entropy_coding_mode_flag = CABAC
p.u1(0)                     # bottom_field_pic_order_in_frame_present_flag
p.ue(0)                     # num_slice_groups_minus1
p.ue(0)                     # num_ref_idx_l0_default_active_minus1
p.ue(0)                     # num_ref_idx_l1_default_active_minus1
p.u1(0)                     # weighted_pred_flag
p.u(2, 0)                   # weighted_bipred_idc
p.se(0)                     # pic_init_qp_minus26
p.se(0)                     # pic_init_qs_minus26
p.se(0)                     # chroma_qp_index_offset
p.u1(0)                     # deblocking_filter_control_present_flag
p.u1(0)                     # constrained_intra_pred_flag
p.u1(0)                     # redundant_pic_cnt_present_flag
p.trailing()
pps = nal(8, 3, p.bytes())

# ---- recovery_point SEI (payloadType 6, payloadSize 1), byte-identical to the
# real disc SEI: RBSP 06 01 c4 80 -> a 5-byte NAL fully inside the bit cache ----
sei_rbsp = bytes([6, 1, 0xc4, 0x80])
sei = nal(6, 0, sei_rbsp)
assert sei == bytes([0, 0, 1, 0x06, 0x06, 0x01, 0xc4, 0x80]), sei.hex()

stream = sps + pps + sei
open(sys.argv[1], 'wb').write(stream)
print(f"wrote {sys.argv[1]} size={len(stream)} (sps={len(sps)} pps={len(pps)} sei={len(sei)})")
