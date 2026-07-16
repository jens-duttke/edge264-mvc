#!/usr/bin/env python3
# Synthesize a valid stream whose SEI carries a HANDLED message that consumes
# fewer bytes than its declared payloadSize, reproducing the parse_sei success
# path desync (edge264_sei.c).
#
# On a successful handler, parse_sei only byte-ALIGNED the reader instead of
# advancing to start + payloadSize (as the error/skip path does). A
# forward-compatible payload with trailing reserved data - or any handler that
# short-reads - therefore left the reader mid-payload: the leftover payload
# bytes were re-parsed as bogus follow-on messages, the reader ended mid-syntax,
# rbsp_end failed and the whole SEI NAL misreturned EBADMSG, failing an
# otherwise valid stream (in the logging/trace path, where parse_sei runs).
#
# The message here is payloadType 1 (pic_timing). With HRD and pic_struct all
# absent from the SPS (no VUI), parse_pic_timing reads ZERO bits and returns
# success, so all payloadSize bytes are unconsumed reserved data. SEI RBSP:
#   01 01 80 80  ->  payloadType=1, payloadSize=1, one reserved payload byte
#   0x80, then the rbsp_trailing 0x80. The correct decode skips the reserved
#   byte and finds the trailing; the buggy byte-align leaves it, re-reads 0x80
#   as a bogus payloadType and overruns the trailing -> EBADMSG.
# asan_check asserts this "clean" fixture decodes with zero EBADMSG.
#
# Usage: gen_sei_short_payload.py <out.264>
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
s.u1(0)                     # vui_parameters_present_flag (no HRD / pic_struct)
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

# ---- pic_timing SEI (payloadType 1) with payloadSize 1 but a zero-reading
# handler, so the one payload byte is unconsumed reserved data ----
sei_rbsp = bytes([1, 1, 0x80, 0x80])
sei = nal(6, 0, sei_rbsp)
assert sei == bytes([0, 0, 1, 0x06, 0x01, 0x01, 0x80, 0x80]), sei.hex()

stream = sps + pps + sei
open(sys.argv[1], 'wb').write(stream)
print(f"wrote {sys.argv[1]} size={len(stream)} (sps={len(sps)} pps={len(pps)} sei={len(sei)})")
