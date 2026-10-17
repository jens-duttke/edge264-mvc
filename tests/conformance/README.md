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
- `mvc-synthetic/`, `mvc-jmvc/`, `2d-synthetic/` - streams for cases the
  conformance bitstreams do not cover, each with its own README naming the
  oracle its hashes are anchored to.
- `manifest.txt` - one line per fixture:
  `<subdir>/<name> <frames> <stereo> <base-hash> <dep-hash|->`
  `frames` = output frame count, `stereo` = 1 for MVC, hashes are
  128-bit FNV-1a (32 hex chars) over the cropped planes in display order.

Only streams edge264-mvc fully decodes are included; interlaced (field /
MBAFF / PAFF) streams and those of higher bit depths or 4:4:4 chroma,
which the decoder does not support, are deliberately absent.

## Provenance

All bitstreams are official ITU-T / JVT conformance vectors - freely
redistributable test material (not commercial content):

    AVCv1 + FRExt: https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/{AVCv1,FRExt}/*.zip
    MVC:           https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/MVC/*.264

The `.264` files are the bitstreams extracted from those sources (the
AVCv1/FRExt zips also ship a reference `.yuv`; the MVC area ships
bitstreams only).

## How the expected hashes are anchored

- **2D: anchored to the ITU reference.** The manifest was generated with
  `conformance_check emit`, which decodes a stream and verifies its hash
  equals the hash of the official ITU reference `.yuv` *before* accepting
  the line (it printed `check=OK`). So every committed 2D hash equals the
  ITU reference output.
- **MVC: anchored to the JMVC reference decoder.** The ITU MVC area
  publishes no reference YUVs, so both views of each committed MVC stream
  were checked against the output of JMVC, the MVC reference software
  (ITU-T H.264.2): the base view and the view the decoder outputs as the
  second one (view order index 1, whose `view_id` is 2 in some streams)
  match it frame for frame.

For MVC streams the harness additionally asserts the decoder's structural
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

For an MVC stream, put the JMVC output of its two views next to it as
`<name>.yuv` (base view) and `<name>.1.yuv` (second view). Keep only lines
whose comment reads `check=OK` - against the ITU reference, or for MVC
against JMVC - and, for MVC, `dep_check=OK pair_err=0 order_err=0`.
Copy the corresponding `.264` here and append the line (without the trailing
`# ...` comment) to `manifest.txt`.
