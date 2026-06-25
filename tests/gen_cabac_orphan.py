#!/usr/bin/env python3
# Synthesize a CABAC stream that reproduces the flush-drain orphan deadlock fixed
# in edge264_headers.c (bump_all_frames). One picture is left INCOMPLETE (a slice
# that ends, via end_of_slice, after coding only 1 of the picture's 2 macroblocks,
# so remaining_mbs stays > 0 and the picture never finalizes - the same state a
# corrupt broadcast stream leaves when a slice errors mid-frame). It has the
# lowest output POC, so it is bumped into the 16-entry output queue but skipped by
# edge264_get_frame (an unfinished picture is held back mid-stream); the following
# complete higher-POC pictures ARE delivered, so they keep bumping and shift the
# unfinished one out of the queue. Orphaned (in to_get_frames but no longer
# queued), it made bump_all_frames spin ENOBUFS forever at end-of-stream, losing
# the picture. ffmpeg conceals and emits such a damaged picture; the fix finalizes
# and re-queues the orphan so the drain terminates and every picture is delivered.
#
# x264/JVT (conformant) never leave an incomplete picture, and gen_avc.py is
# CAVLC-only, so this writes its own minimal CABAC (I_PCM macroblocks: only a few
# arithmetic bins per MB, samples are raw). The picture is 1x2 MBs (16x32).
#
# Usage: gen_cabac_orphan.py <out.264> [allcomplete] [N]
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
    def se(self, v): self.ue(2 * v - 1 if v > 0 else -2 * v)
    def align_one(self):
        while len(self.bits) % 8 != 0: self.bits.append(1)
    def trailing(self):
        self.bits.append(1)
        while len(self.bits) % 8 != 0: self.bits.append(0)
    def bytes(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for j in range(8): b = (b << 1) | self.bits[i + j]
            out.append(b)
        return out

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
        self.low = 0; self.range = 510; self.bitsOut = 0; self.first = True; self.out = []
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
    def encode(self, ctx, binVal):
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
    def terminate(self, binVal):          # EncodeTerminate (9.3.4.5): end_of_slice / I_PCM bin
        self.range -= 2
        if binVal:
            self.low += self.range
            self.range = 2; self._renorm()       # EncodeFlush (9.3.4.6)
            self._putbit((self.low >> 9) & 1)
            v = ((self.low >> 7) & 3) | 1
            self.out.append((v >> 1) & 1); self.out.append(v & 1)
        else:
            self._renorm()
    def bytes(self):
        bits = list(self.out)
        while len(bits) % 8 != 0: bits.append(0)
        out = bytearray()
        for i in range(0, len(bits), 8):
            b = 0
            for j in range(8): b = (b << 1) | bits[i + j]
            out.append(b)
        return out

def ctx_init(idx, qp):                    # cabac_context_init[0] entries used here
    m, n = {3: (20, -15), 4: (2, 54)}[idx]
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
PCM = bytes([0x80] * 384)                 # one 16x16 I_PCM macroblock (4:2:0 8-bit)

def i_pcm_mb(ctxidx, end_of_slice):
    # one I_PCM MB followed by the end_of_slice terminate that the decoder reads
    # after re-initialising its engine on the next (fresh) CABAC segment.
    c = CABAC()
    c.encode(ctx_init(ctxidx, QP), 1)     # mb_type bin0 = 1 (not I_NxN)
    c.terminate(1)                        # I_PCM terminate -> byte aligned
    seg = bytearray(c.bytes()) + PCM
    e = CABAC(); e.terminate(1 if end_of_slice else 0)   # end_of_slice flag
    return seg, e

def gen_picture(idr, frame_num, poc, complete):
    h = BW()
    h.ue(0)                               # first_mb_in_slice
    h.ue(2)                               # slice_type = I
    h.ue(0)                               # pic_parameter_set_id
    h.u(4, frame_num)                     # frame_num
    if idr: h.ue(0)                       # idr_pic_id
    h.u(12, poc)                          # pic_order_cnt_lsb
    if idr:
        h.u1(0); h.u1(0)                  # no_output_of_prior_pics / long_term_reference
    h.se(0)                               # slice_qp_delta
    h.align_one()
    rbsp = bytearray(h.bytes())
    if complete:
        seg0, e0 = i_pcm_mb(3, end_of_slice=False)  # MB0, then end_of_slice = 0
        rbsp += seg0
        # the end_of_slice=0 bin and MB1's mb_type share one re-initialised engine
        c = CABAC(); c.terminate(0)
        c.encode(ctx_init(4, QP), 1); c.terminate(1)   # MB1 mb_type + I_PCM terminate
        rbsp += bytearray(c.bytes()) + PCM
        e = CABAC(); e.terminate(1)       # end_of_slice = 1
        rbsp += e.bytes()
    else:                                 # INCOMPLETE: 1 of 2 MBs, then end_of_slice = 1
        seg0, _ = i_pcm_mb(3, end_of_slice=True)
        rbsp += seg0
        e = CABAC(); e.terminate(1)
        rbsp += e.bytes()
    return nal(5 if idr else 1, 3 if idr else 0, rbsp)

mode = sys.argv[2] if len(sys.argv) > 2 else 'orphan'
N = int(sys.argv[3]) if len(sys.argv) > 3 else 24

# SPS: High, 1x2 MBs (16x32), 4:2:0 8-bit, 12-bit POC, no scaling/VUI
s = BW()
s.u(8, 100); s.u(8, 0); s.u(8, 10)
s.ue(0); s.ue(1); s.ue(0); s.ue(0); s.u1(0); s.u1(0)
s.ue(0)                                   # log2_max_frame_num_minus4 -> 4 bits
s.ue(0)                                   # pic_order_cnt_type 0
s.ue(8)                                   # log2_max_pic_order_cnt_lsb_minus4 -> 12 bits
s.ue(1); s.u1(0)
s.ue(0)                                   # pic_width_in_mbs_minus1 = 0 (1 wide)
s.ue(1)                                   # pic_height_in_map_units_minus1 = 1 (2 tall)
s.u1(1)                                   # frame_mbs_only_flag
s.u1(0)                                   # direct_8x8_inference_flag
s.u1(0)                                   # frame_cropping_flag
s.u1(1)                                   # vui_parameters_present_flag
# VUI: everything off except bitstream_restriction, which sets
# max_num_reorder_frames = 0 so every complete picture is output immediately
# (no reorder buffering) - that churns the output queue and shifts the held
# incomplete picture out of it, which is what orphans it.
s.u1(0); s.u1(0); s.u1(0); s.u1(0)        # aspect/overscan/video_signal/chroma_loc
s.u1(0); s.u1(0); s.u1(0); s.u1(0)        # timing/nal_hrd/vcl_hrd/pic_struct
s.u1(1)                                   # bitstream_restriction_flag
s.u1(1)                                   # motion_vectors_over_pic_boundaries_flag
s.ue(0); s.ue(0); s.ue(0); s.ue(0)        # max_bytes/max_bits/log2_mv_h/log2_mv_v
s.ue(0)                                   # max_num_reorder_frames = 0
s.ue(16)                                  # max_dec_frame_buffering
s.trailing()
sps = nal(7, 3, s.bytes())

p = BW()
p.ue(0); p.ue(0); p.u1(1); p.u1(0); p.ue(0); p.ue(0); p.ue(0); p.u1(0); p.u(2, 0)
p.se(0); p.se(0); p.se(0); p.u1(0); p.u1(0); p.u1(0)
p.trailing()
pps = nal(8, 3, p.bytes())

# IDR (POC 0, complete) establishes the stream; picture 1 is INCOMPLETE with the
# next-lowest POC; the rest are complete with increasing POC and shift it out of
# the 16-entry queue. In 'allcomplete' mode every picture is complete (proves the
# 2-MB encoder + decodes cleanly with or without the fix).
stream = bytearray(sps + pps)
for k in range(N):
    incomplete = (mode == 'orphan' and k == 1)
    stream += gen_picture(idr=(k == 0), frame_num=0, poc=k, complete=not incomplete)

open(sys.argv[1], 'wb').write(stream)
print(f"wrote {sys.argv[1]} mode={mode} pictures={N} size={len(stream)}")
