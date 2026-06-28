#!/usr/bin/env python3
# Independent ground-truth oracle for the synthetic MVC fixtures.
#
# It proves the committed per-view hashes in ../manifest.txt equal the
# spec-derived correct output WITHOUT running the edge264 decoder, so the
# hashes are not "whatever the decoder happens to emit" but an external
# reference the decoder is checked against.
#
# Why the correct output is a single value: every fixture here is a
# 1x1-macroblock (16x16 luma, 8x8 chroma) 4:2:0 8-bit stream whose macroblocks
# carry no residual. The IDR base/dependent macroblocks are I_NxN with all
# neighbours unavailable, so H.264 8.3 infers DC intra prediction, whose value
# with no available samples is 1 << (BitDepth-1) = 128. The later macroblocks
# are zero-motion, no-residual P/B copies of those references, so they stay 128.
# Hence every decoded sample is 0x80, for both views, in every frame.
#
# Per view per frame: Y 16x16 + Cb 8x8 + Cr 8x8 = 256 + 64 + 64 = 384 bytes.
# The harness (tests/conformance_check.c) accumulates one FNV-1a-128 digest over
# the base view of every output frame and another over the dependent view of the
# stereo frames only; this script reproduces both from first principles.
#
# Run from the repo root:  python3 tests/conformance/mvc-synthetic/ground_truth.py
# Exit status is non-zero if any committed hash disagrees with the derivation.

import os, sys

BYTES_PER_VIEW_FRAME = 384  # all 0x80

def fnv128(data):
    a, b = 0xcbf29ce484222325, 0x84222325cbf29ce4
    M, p = 0xFFFFFFFFFFFFFFFF, 0x100000001b3
    for byte in data:
        a = ((a ^ byte) * p) & M
        b = ((b ^ byte) * p) & M
    return f"{a:016x}{b:016x}"

def gt(n_views_frames):
    # digest over n frames of a single view, every sample == 0x80
    return fnv128(b"\x80" * (BYTES_PER_VIEW_FRAME * n_views_frames))

# (total output frames, stereo frames) for each fixture, read off its construction:
#   mvc_base            - 2 stereo access units                 -> 2 base, 2 dep
#   mvc_dep_before_base - same content, dependent NAL reordered -> 2 base, 2 dep
#                         (its hashes MUST equal mvc_base's: reordering NAL units
#                          within an access unit cannot change decoded output)
#   mvc_late_dependent  - 2 base-only AUs then 2 stereo AUs     -> 4 base, 2 dep
#   mvc_dependent_frame_num_gap - 12 stereo AUs (a dependent-view frame_num gap
#                         shifts the last AU's dependent FrameId; all views still
#                         decode, so all 12 are stereo)            -> 12 base, 12 dep
EXPECTED = {
    "mvc-synthetic/mvc_base":                   (2, 2),
    "mvc-synthetic/mvc_dep_before_base":        (2, 2),
    "mvc-synthetic/mvc_late_dependent":         (4, 2),
    "mvc-synthetic/mvc_dependent_frame_num_gap": (12, 12),
}

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    manifest = os.path.join(here, "..", "manifest.txt")
    committed = {}
    with open(manifest) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 5 and parts[0].startswith("mvc-synthetic/"):
                committed[parts[0]] = (parts[3], parts[4])  # base-hash, dep-hash

    fails = 0
    for name, (nframes, nstereo) in EXPECTED.items():
        exp_base, exp_dep = gt(nframes), gt(nstereo)
        if name not in committed:
            print(f"MISSING in manifest: {name}"); fails += 1; continue
        got_base, got_dep = committed[name]
        ok_b = got_base == exp_base
        ok_d = got_dep == exp_dep
        print(f"{name}: base {'OK' if ok_b else 'MISMATCH'} dep {'OK' if ok_d else 'MISMATCH'}")
        if not ok_b:
            print(f"    base expected {exp_base}  committed {got_base}"); fails += 1
        if not ok_d:
            print(f"    dep  expected {exp_dep}  committed {got_dep}"); fails += 1

    # The reordering invariance, stated as an assertion rather than a coincidence:
    if committed.get("mvc-synthetic/mvc_dep_before_base") != committed.get("mvc-synthetic/mvc_base"):
        print("INVARIANCE BROKEN: mvc_dep_before_base hashes != mvc_base hashes"); fails += 1
    else:
        print("invariance OK: mvc_dep_before_base output == mvc_base output (NAL reorder is transparent)")

    print("ALL GROUND TRUTH CONFIRMED" if fails == 0 else f"{fails} MISMATCH(ES)")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
