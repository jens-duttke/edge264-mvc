# Committed decode-regression fixtures

Bitstreams + expected results that pin edge264-mvc's decoded output, so a
larger refactor can be proven not to change what the decoder produces.
Run by `make check` (and standalone via `make check-conformance`); the
harness is `tests/conformance_check.c`.

Everything here is self-contained: `git clone && make check` runs the
whole suite offline. No reference YUVs are committed (they are far larger
than the bitstreams) - `manifest.txt` instead carries a 128-bit hash of
each stream's decoded output per view, so the harness recomputes and
compares from the committed `.264` alone.

## Layout

- `2d/*.264` - JVT AVCv1 + FRExt conformance bitstreams (single view).
- `mvc/*.264` - JVT MVC conformance bitstreams (stereo, NAL 14/15/20).
- `manifest.txt` - one line per fixture:
  `<subdir>/<name> <frames> <stereo> <base-hash> <dep-hash|->`
  `frames` = output frame count, `stereo` = 1 for MVC, hashes are
  128-bit FNV-1a (32 hex chars) over the cropped planes in display order.

Only streams edge264 fully decodes are included; interlaced (field /
MBAFF / PAFF) and 4:2:2 conformance streams, which the decoder does not
support, are deliberately absent.

## Provenance

All bitstreams are official ITU-T / JVT conformance vectors - freely
redistributable test material (not commercial content):

    AVCv1 + FRExt: https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/{AVCv1,FRExt}/*.zip
    MVC:           https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/MVC/*.264

The `.264` files are the bitstreams extracted from those sources (the
AVCv1/FRExt zips also ship a reference `.yuv`; the MVC area ships
bitstreams only).

## How the expected hashes are anchored (and the one honest caveat)

- **2D + MVC base view: anchored to the ITU reference.** The manifest was
  generated with `conformance_check emit`, which decodes a stream and
  verifies its base-view hash equals the hash of the official ITU
  reference `.yuv` *before* accepting the line (it printed `check=OK`).
  So every committed base-view hash equals the ITU reference output.
- **MVC dependent view: pinned to the fork's validated output.** The ITU
  MVC area publishes no reference YUVs, and FFmpeg cannot decode the
  dependent view at all, so no external oracle exists. The dependent-view
  hash is therefore pinned to this fork's output, which was validated
  bit-exact against upstream edge264 on the full JVT MVC set (see
  `OKU3D-FORK-FINDINGS.md`). It still catches any refactoring regression -
  any change to the dependent view flips the hash - and is backed by the
  structural assertions below, which need no reference at all.

For MVC streams the harness additionally asserts the fork's structural
guarantees on every run: both views present, each pair POC-paired
(`Poc == Poc_mvc`), and frames emitted in non-decreasing `DisplayPoc`
order.

## Regenerating the manifest

After an intentional, reviewed change to decoded output (rare), or to add
fixtures, rebuild the manifest against the full local conformance corpus
(`conformance/2d`, `conformance/mvc` - gitignored, fetched via the URLs
above). For each stream:

    make            # builds libedge264 + conformance_check
    ./conformance_check emit conformance/2d  <name>   # prints a manifest line, self-checks vs <name>.yuv
    ./conformance_check emit conformance/mvc <name>

Keep only lines whose comment reads `check=OK` (2D, anchored) or, for MVC,
`pair_err=0 order_err=0`; copy the corresponding `.264` here and append
the line (without the trailing `# ...` comment) to `manifest.txt`.
