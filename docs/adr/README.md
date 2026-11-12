# Architecture decision records

An ADR is a one-page record of a decision that shapes the firmware or the way we work:
what we chose, why, what it costs, and what we turned down. The code says what is; an ADR
says why. Read the index before reopening a settled question.

## When to write one

- The decision is hard to reverse, or touches several subsystems.
- The owner made a call that later work must respect.
- A real alternative was rejected and someone may propose it again (write it as a Rejected ADR).

Keep each ADR to one page. Copy `0000-template.md`. Quote the owner only from a doc or a
tracker issue, with the date. The ADRs ship in the public tree: no personal names, paths,
hosts or media titles.

## Numbering

Numbers are four digits and never reused. Take the next free number **on `main`** (look at
this index there, not on your branch). If two branches take the same number, the second to
merge renumbers. `scripts/adr_check.py` fails on duplicate numbers, a file without an index
row, an index row without a file, a bad status, and (with `--main REF`) a number that `REF`
already gives to a different file. It also runs inside `scripts/release_docs.py check`.

## Status values

- **Proposed**: written, not yet agreed.
- **Accepted**: agreed; the code and docs follow it.
- **Rejected**: a real option that was turned down or deferred, kept so it is not rediscovered.
- **Superseded by NNNN**: replaced by a later ADR; keep the file and point at the new one.

The status line in the file (`- Status: ...`) and the index table must agree.

## Index

| ADR | Title | Status | Date |
|---|---|---|---|
| [0001](0001-core-layout.md) | Apple core layout: audio on the COP, the rest on the CPU | Accepted | 2026-11-05 |
| [0002](0002-whole-audio-chain-on-cop.md) | The whole playback chain runs on the COP | Accepted | 2026-11-10 |
| [0003](0003-clock-follows-busier-core.md) | The clock follows the busier of the two cores | Accepted | 2026-10-25 |
| [0004](0004-one-release-build.md) | One release build from 0.8.3, with plain runtime facts | Accepted | 2026-11-11 |
| [0005](0005-iap-resident-out2-on.md) | iAP stays resident and dormant; OUT2 is always on | Accepted | 2026-11-11 |
| [0006](0006-24-bit-output-audiophile.md) | 24-bit output and Settings > Audiophile | Accepted | 2026-11-10 |
| [0007](0007-best-resampler-no-output-rate-setting.md) | Best-quality sample-rate converter for every ratio; no user-set output rate | Accepted | 2026-11-10 |
| [0008](0008-vc-first.md) | Video chip first, the CPU only as a loud fall-back | Accepted | 2026-11-10 |
| [0009](0009-versions-from-tags-docs-gated.md) | Versions come from git tags; docs and changelog are generated and gated | Accepted | 2026-11-11 |
| [0010](0010-unplayable-greyed-not-hidden.md) | Unplayable files are shown greyed out, never hidden | Accepted | 2026-11-10 |
| [0011](0011-disk-floor-and-worktree-pruning.md) | Keep 30 GB free and prune worktrees (dev process) | Accepted | 2026-11-11 |
| [0012](0012-album-flow-design.md) | Album Flow: menu entry only, a SLANT renderer, a 32-cover cache on the video chip | Accepted | 2026-11-11 |
| [0013](0013-unit-profile-record.md) | A boot-time unit-profile record for the single build | Rejected | 2026-11-11 |
| [0014](0014-boot-image-picker.md) | A boot loader that picks one of four images | Rejected | 2026-11-11 |
| [0015](0015-iap-as-module.md) | iAP as a module loaded on demand, in 0.8.3 | Rejected | 2026-11-11 |
| [0016](0016-no-skin-theme-engine.md) | No skin or theme engine | Accepted | 2026-11-12 |
| [0017](0017-no-plugins-as-is-sandboxed-game-kits.md) | No plugins as they are; games through the sandboxed game kits | Accepted | 2026-11-12 |
