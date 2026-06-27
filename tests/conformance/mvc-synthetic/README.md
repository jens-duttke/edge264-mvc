# Synthetic MVC structural fixtures

Hand-authored minimal MVC bitstreams that pin edge264-mvc's handling of MVC
NAL-ordering edge cases the published ITU conformance vectors do not cover.
Run by `make check` through the same harness as the rest of the conformance
suite (`tests/conformance_check.c`), single-threaded and multithreaded.

Unlike `../mvc/` (official JVT vectors), these are **not** ITU material: each
is generated from its committed `.yaml` source with `tests/gen_avc.py`. The ITU
MVC area ships no reference YUV and FFmpeg cannot decode the dependent view, so
there is no external file to anchor against - but the expected output is still
**derived independently of the decoder**, not taken on trust (see *Ground truth*
below). On top of the per-view hash and exact frame count, the harness asserts
on every run that each stereo pair is POC-paired (`Poc == Poc_mvc`) and that
frames come out in non-decreasing `DisplayPoc` order.

All streams are 1x1-macroblock, Stereo High (profile 128), CAVLC, two views.

## Ground truth

These fixtures do not bless "whatever the decoder emits". Every macroblock is
residual-free: the IDR macroblocks are I_NxN with all neighbours unavailable, so
H.264 8.3 infers DC intra prediction = `1 << (BitDepth-1)` = 128, and the later
macroblocks are zero-motion, no-residual P/B copies of those references. So the
correct decoded output is **every sample = 128**, for both views, in every
frame, by spec - knowable before running anything.

[`ground_truth.py`](ground_truth.py) recomputes the committed hashes from that
derivation alone (the same FNV-1a-128 the harness uses, over a `0x80` byte
stream of the right length) and checks them against `../manifest.txt`, with no
decoder involved. So the chain is closed end to end: `ground_truth.py` proves
the committed hash is the spec-correct output, and `make check` proves the
decoder reproduces that hash. Run it from the repo root:

    python3 tests/conformance/mvc-synthetic/ground_truth.py

## Fixtures

- **mvc_base** (2 frames) - one anchor IDR + one non-anchor access unit, in
  spec NAL order (base view before dependent view). The in-order reference for
  the reordering-invariance check below.

- **mvc_dep_before_base** (2 frames) - the same content as `mvc_base`, but in
  the IDR access unit the dependent view (NAL type 20) is placed *before* its
  base view (NAL type 5). Reordering NAL units within an access unit cannot
  change the decoded output, so a correct decoder must produce a stream
  byte-identical to `mvc_base` - the two carry the **same** committed hash, and
  `ground_truth.py` asserts that equality as an invariance, not a coincidence.
  This is *structural coverage*, not a fork differential: upstream edge264
  handles it identically (verified). It pins NAL-order-robust view association.

- **mvc_late_dependent** (4 frames) - the first two access units are base-only
  (no NAL type 20); the dependent view appears only from the third. A correct
  decoder emits the early frames as 2D (base alone) and the later ones as
  POC-paired stereo, in `DisplayPoc` order, without stalling on the 2D->stereo
  transition. This is a genuine **fork regression guard**: upstream edge264
  (5b8ba48) stalls after 2 frames on this transition (no forward progress),
  whereas the fork delivers all 4. It pins the fork's forward-progress /
  stereo-pairing fix for a changing view layout.

## Regenerating

After an intentional, reviewed change to decoded output:

    make                                                  # builds conformance_check + gen_avc deps
    python3 tests/gen_avc.py tests/conformance/mvc-synthetic/<name>.yaml \
            tests/conformance/mvc-synthetic/<name>.264
    ./conformance_check emit tests/conformance/mvc-synthetic <name>

Keep the printed line only if its comment reads `pair_err=0 order_err=0`, then
copy it (without the trailing `# ...`) into `../manifest.txt`.
