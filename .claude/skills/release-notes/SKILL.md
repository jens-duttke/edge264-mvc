---
name: release-notes
description: Use when writing the notes for an edge264-mvc release - the annotated tag message and the GitHub Release body, or when asked to cut/publish a version. Covers the house format every previous release follows (summary line, section taxonomy, the bold noun-phrase bullet lead, what Verification must state), the public register, and the mechanical traps that have silently mangled a release twice - `git tag -F` dropping every `###` heading unless `--cleanup=verbatim` is passed, and `gh release create` needing an explicit `--title`.
---

# Write the notes for an edge264-mvc release

The release notes are written **once**, as the annotated tag message, and the GitHub
Release body is generated from it with `--notes-from-tag`. Tag annotation and Release
body must therefore be identical - if they diverge, the tag is the one that was wrong.

## The format

Every release since the first fork release follows this shape:

```markdown
<one-line summary>, since <previous tag>.

### Added
- **<noun phrase naming the capability>.** <prose>

### Fixed
- **<noun phrase naming the defect>.** <prose>

### Verification
- <one bullet: what was measured>
- No public API or ABI change - `edge264.h` is unchanged since <tag>.
```

**Summary line.** One sentence, no heading, ending in `since <previous tag>.` when the
release follows one. Either a topic phrase (`MVC display-order fix since v2026.07.11.`)
or a verb phrase (`Fixes a multithreaded decode deadlock on a damaged reference frame
that no task can complete, since v2026.07.18.`). It names what the release is about, not
how it was implemented.

**Sections.** `### Added`, `### Changed`, `### Fixed`, `### Performance`, `### Verification`.
Use only the ones that apply; `### Verification` is always last and never omitted.
A stream class that the decoder used to reject belongs under `Added`, a wrong behaviour
on streams that already decoded belongs under `Fixed`.

**Bullet lead.** Every bullet in a non-Verification section opens with a **bold noun
phrase of roughly two to ten words**, naming the defect or the capability, with the
period inside the bold. Never a full sentence, and never phrased as the outcome
("... no longer fails"). Real leads from previous releases:

```markdown
- **Permanent `ENOBUFS` jam on a corrupt MVC dependent-view slice header.**
- **SSE temporal-direct motion-vector rounding.**
- **Held MVC base at an end_of_sequence NAL.**
- **Multithreaded deadlock on a base-less dependent-view tail.**
- **FMO PPS reported as ENOTSUP.**
```

The prose after the lead runs in this order: what the code did, what that cost the user,
what happens now. Cite spec clauses (7.3.3, 8.2.5.3, H.10.1.1) and public API names
directly; credit an external report or PR at the end of the bullet, e.g.
`([PR #12](https://github.com/jens-duttke/edge264-mvc/pull/12) · @cbusillo)`.

**Verification.** One bullet carrying the measurements, plus the API/ABI bullet. The
measurement bullet states, as far as it applies: that decoded output is unchanged or
byte-identical against the previous tag, that the change is inert on the full JVT
conformance set (with the figures, e.g. `113 pass / 117 unsupported / 1 pre-existing
false positive`), that multithreaded output stays bit-exact to single-threaded, that
`make check` and the AddressSanitizer suite are green, and which committed regression
fixture guards the fix. Claim only what was actually run - this fork's value is that its
claims are measured. If a fix has no committable fixture, say so rather than implying one.

## Register

Public-facing prose, so plain terms over the internal register: "the output is unchanged"
rather than "bit-exact" in the summary, "multi-threaded decode" rather than `threads = -1`.
Genuine domain terms (DPB, POC, open-GOP, access unit) and public API names stay. Name the
function and the generic failure mode; never name a private sample file, a local path, or
a disc title. Plain hyphen as the only dash.

## Mechanics - the traps that have bitten twice

```sh
# 1. write the notes to a file, then tag VERBATIM: without --cleanup=verbatim git strips
#    every line starting with '#', which silently deletes all '###' headings
git tag -a --cleanup=verbatim -F notes.md vYYYY.MM.DD main
git push origin vYYYY.MM.DD

# 2. --title is mandatory; a pushed tag alone is NOT a release
gh release create vYYYY.MM.DD --title vYYYY.MM.DD --notes-from-tag --verify-tag --latest

# 3. VERIFY both artifacts actually carry the headings
git tag -n99 vYYYY.MM.DD
gh release view vYYYY.MM.DD --json body --jq .body
```

If the tag annotation turns out wrong after the fact, `gh release edit <tag> --notes-file`
repairs the public Release body on its own, but leaves the annotation diverging; bringing
the annotation back in line needs a forced retag plus a forced tag push, which is a
destructive operation - ask the user before doing it.

## Checklist

1. Previous tag identified, and every commit since it has its README fix-table row.
2. Notes drafted in a file: summary line, the sections that apply, noun-phrase leads,
   Verification with real measurements and the API/ABI bullet.
3. Tag created with `--cleanup=verbatim`, pushed.
4. Release created with `--title` and `--verify-tag --latest`.
5. Both artifacts read back and confirmed to carry the `###` headings.
6. Downstream consumers propagated (see the release-downstream skill).
