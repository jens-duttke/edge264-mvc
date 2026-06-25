#!/usr/bin/env python3
# Synthesize a CABAC stream that reproduces the end-of-slice over-read deadlock
# fixed in edge264_headers.c (worker_loop): a CABAC slice that decodes every
# macroblock of the picture but leaves a non-clean cabac trailing state
# (msb_cache != 0, from the arithmetic engine's look-ahead past the slice's last
# byte). Before the fix that tripped EBADMSG on a *complete* final slice, so
# remaining_mbs was never zeroed, the picture never finalized, the DPB filled and
# the decoder stalled mid-stream. x264/JVT (conformant) terminate cleanly and
# never trigger it; gen_avc.py has no CABAC. So this writes its own minimal CABAC.
#
# The picture is a single 16x16 I_PCM macroblock (raw samples, so only two
# arithmetic bins need encoding: mb_type bin0 = "not I_NxN" and the I_PCM
# terminate). After the PCM samples the decoder re-initialises the arithmetic
# engine (cabac_start), so end_of_slice is read from the bytes we place there:
#   - "clean" mode  -> a proper EncodeFlush => msb_cache == 0  (decodes fine, used to prove the encoder)
#   - "dirty" mode  -> raw 0xFF bytes        => offset huge >= range (end_of_slice = 1)
#                      with msb_cache != 0    => the bug condition.
#
# Usage: gen_cabac_overread.py <out.264> [clean|dirty]
import sys

# ---------- bit writer for the RBSP (SPS/PPS/slice header) ----------
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
    def align_one(self):                 # cabac_alignment_one_bit: pad with 1s to byte boundary
        while len(self.bits) % 8 != 0: self.bits.append(1)
    def trailing(self):                  # rbsp_trailing_bits: stop_one_bit then zero alignment bits
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

# ---------- standard CABAC arithmetic encoder (9.3.4) ----------
RANGE_TAB_LPS = [
  128,176,208,240, 128,167,197,227, 128,158,187,216, 123,150,178,205,
  116,142,169,195, 111,135,160,185, 105,128,152,175, 100,122,144,166,
   95,116,137,158,  90,110,130,150,  85,104,123,142,  81, 99,117,135,
   77, 94,111,128,  73, 89,105,122,  69, 85,100,116,  66, 80, 95,110,
   62, 76, 90,104,  59, 72, 86, 99,  56, 69, 81, 94,  53, 65, 77, 89,
   51, 62, 73, 85,  48, 59, 69, 80,  46, 56, 66, 76,  43, 53, 63, 72,
   41, 50, 59, 69,  39, 48, 56, 65,  37, 45, 54, 62,  35, 43, 51, 59,
   33, 41, 48, 56,  32, 39, 46, 53,  30, 37, 43, 50,  29, 35, 41, 48,
   27, 33, 39, 45,  26, 31, 37, 43,  24, 30, 35, 41,  23, 28, 33, 39,
   22, 27, 32, 37,  21, 26, 30, 35,  20, 24, 29, 33,  19, 23, 27, 31,
   18, 22, 26, 30,  17, 21, 25, 28,  16, 20, 23, 27,  15, 19, 22, 25,
   14, 18, 21, 24,  14, 17, 20, 23,  13, 16, 19, 22,  12, 15, 18, 21,
   12, 14, 17, 20,  11, 14, 16, 19,  11, 13, 15, 18,  10, 12, 15, 17,
   10, 12, 14, 16,   9, 11, 13, 15,   9, 11, 12, 14,   8, 10, 12, 14,
    8,  9, 11, 13,   7,  9, 11, 12,   7,  9, 10, 12,   7,  8, 10, 11,
    6,  8,  9, 11,   6,  7,  9, 10,   6,  7,  8,  9,   2,  2,  2,  2]
TRANS_LPS = [0,0,1,2,2,4,4,5,6,7,8,9,9,11,11,12,13,13,15,15,16,16,18,18,19,19,21,21,
  23,22,23,24,24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,33,33,34,34,35,35,35,
  36,36,36,37,37,37,38,38,63]
TRANS_MPS = [min(i + 1, 62) for i in range(63)] + [63]

class CABAC:
    def __init__(self):
        self.low = 0; self.range = 510
        self.bitsOut = 0; self.first = True; self.out = []
    def _putbit(self, b):
        if self.first: self.first = False
        else: self.out.append(b)
        while self.bitsOut > 0: self.out.append(1 - b); self.bitsOut -= 1
    def _renorm(self):
        while self.range < 256:
            if self.low < 256: self._putbit(0)
            elif self.low >= 512: self.low -= 512; self._putbit(1)
            else: self.low -= 256; self.bitsOut += 1
            self.range <<= 1; self.low <<= 1
    def encode(self, ctx, binVal):       # ctx = [pStateIdx, valMPS]
        q = (self.range >> 6) & 3
        rLPS = RANGE_TAB_LPS[ctx[0] * 4 + q]
        self.range -= rLPS
        if binVal != ctx[1]:
            self.low += self.range; self.range = rLPS
            if ctx[0] == 0: ctx[1] ^= 1
            ctx[0] = TRANS_LPS[ctx[0]]
        else:
            ctx[0] = TRANS_MPS[ctx[0]]
        self._renorm()
    def flush(self):                     # EncodeTerminate(1) + EncodeFlush (9.3.4.5/6)
        self.range -= 2; self.low += self.range
        self.range = 2; self._renorm()
        self._putbit((self.low >> 9) & 1)
        v = ((self.low >> 7) & 3) | 1
        self.out.append((v >> 1) & 1); self.out.append(v & 1)
    def bytes(self):
        bits = list(self.out)
        while len(bits) % 8 != 0: bits.append(0)
        out = bytearray()
        for i in range(0, len(bits), 8):
            b = 0
            for j in range(8): b = (b << 1) | bits[i + j]
            out.append(b)
        return out

def ctx_init(m, n, qp):
    pre = min(126, max(1, ((m * max(0, min(51, qp))) >> 4) + n))
    return [63 - pre, 0] if pre <= 63 else [pre - 64, 1]

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

QP = 26
mode = sys.argv[2] if len(sys.argv) > 2 else 'dirty'

# ---- SPS (High profile 100, 16x16 frame = 1x1 MB, 4:2:0 8-bit, no scaling/VUI) ----
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
s.ue(0)                     # log2_max_frame_num_minus4 (=> 4 bits)
s.ue(0)                     # pic_order_cnt_type 0
s.ue(8)                     # log2_max_pic_order_cnt_lsb_minus4 (=> 12 bits, no wrap over many frames)
s.ue(1)                     # max_num_ref_frames
s.u1(0)                     # gaps_in_frame_num_value_allowed_flag
s.ue(0)                     # pic_width_in_mbs_minus1 (=> 1 MB wide)
s.ue(0)                     # pic_height_in_map_units_minus1
s.u1(1)                     # frame_mbs_only_flag
s.u1(0)                     # direct_8x8_inference_flag
s.u1(0)                     # frame_cropping_flag
s.u1(0)                     # vui_parameters_present_flag
s.trailing()                # rbsp_trailing_bits
sps = nal(7, 3, s.bytes())

# ---- PPS (entropy_coding_mode_flag = 1 => CABAC; no transform_8x8) ----
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
p.se(QP - 26)               # pic_init_qp_minus26
p.se(0)                     # pic_init_qs_minus26
p.se(0)                     # chroma_qp_index_offset
p.u1(0)                     # deblocking_filter_control_present_flag
p.u1(0)                     # constrained_intra_pred_flag
p.u1(0)                     # redundant_pic_cnt_present_flag
p.trailing()                # rbsp_trailing_bits
pps = nal(8, 3, p.bytes())

# ---- one coded picture: a single 16x16 I_PCM macroblock ----
def gen_picture(idr, frame_num, poc):
    h = BW()
    h.ue(0)                                 # first_mb_in_slice
    h.ue(2)                                 # slice_type = I
    h.ue(0)                                 # pic_parameter_set_id
    h.u(4, frame_num)                       # frame_num (log2_max_frame_num = 4)
    if idr:
        h.ue(0)                             # idr_pic_id
    h.u(12, poc)                            # pic_order_cnt_lsb (log2_max_poc = 12)
    if idr:
        h.u1(0)                             # no_output_of_prior_pics_flag
        h.u1(0)                             # long_term_reference_flag
    # non-IDR non-reference (nal_ref_idc 0) slices carry no dec_ref_pic_marking
    h.se(0)                                 # slice_qp_delta (SliceQPy = 26)
    h.align_one()                           # cabac_alignment_one_bit
    # CABAC: mb_type bin0 = 1 (not I_NxN), then I_PCM terminate = 1
    cab = CABAC()
    cab.encode(ctx_init(20, -15, QP), 1)    # ctxIdx 3 (first I-slice MB)
    cab.flush()                             # I_PCM terminate(1) -> byte aligned
    rbsp = bytearray(h.bytes()) + cab.bytes()
    # pcm samples: 16x16 Y + 8x8 Cb + 8x8 Cr = 384 raw bytes (non-zero, dense)
    rbsp += bytes([0x80] * 384)
    # end_of_slice: the decoder re-inits the engine after PCM and reads this.
    if mode == 'clean':
        c2 = CABAC(); c2.flush(); rbsp += c2.bytes()  # proper flush -> msb_cache clean
    else:
        rbsp += bytes([0xff] * 12)          # offset huge >= range => end_of_slice=1, msb_cache dirty
    return nal(5 if idr else 1, 3 if idr else 0, rbsp)

# Enough not-finalised pictures to overflow the DPB *before* end-of-stream, so the
# stall manifests mid-stream (the end-of-stream forward-progress valve cannot mask
# it). The first picture is the IDR; the rest are non-IDR non-reference I pictures
# with increasing POC so they queue for output and accumulate.
N = int(sys.argv[3]) if len(sys.argv) > 3 else 40
stream = bytearray(sps + pps)
for k in range(N):
    stream += gen_picture(idr=(k == 0), frame_num=0, poc=k * 2)

open(sys.argv[1], 'wb').write(stream)
print(f"wrote {sys.argv[1]} mode={mode} pictures={N} size={len(stream)} (sps={len(sps)} pps={len(pps)})")
