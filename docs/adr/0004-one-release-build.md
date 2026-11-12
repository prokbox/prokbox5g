# ADR 0004: One release build from 0.8.3, with plain runtime facts

- Status: Accepted
- Date: 2026-11-11

## Context

Releases came as eight zips (stock or modded, flash or hard drive, 32 or 64 MB). Users had to choose, and testing multiplied. Memory and storage are detectable at start-up (#204, #79). The remaining differences are the mods.

## Decision

Ship one image from 0.8.3 (zip `ProkBox5g-<ver>.zip`). RAM comes from `probed_ramsize`, storage from the drive's own report. Behaviour is the same on every unit wherever it costs no speed; only a few drive-timing choices stay as a simple if/else. Battery capacity is asked once at first start. Haptics use the standard click path. The USB screen has one picture. A performance gate (ABAB, within the measured noise band) guards against regressions.

## Consequences

- The configure flags `--storage`, `--battery`, `--dock30`, `--haptics` become developer overrides.
- CLAUDE.md rule 7 is reworded to "per-unit facts detected at start-up, or asked once".
- Every flash-only choice is listed in `docs/storage-choices.md`.

## Alternatives considered

- A unit-profile record: ADR 0013 (rejected).
- A boot loader that picks one of four images: ADR 0014 (rejected).
- Eight builds, as before: more to test and to explain.

## Owner decision

"a PLAIN single build with fewer moving parts. No unit-profile, seal or patching machinery. Use the existing probed_ramsize and ata_is_ssd flag as ordinary start-up values." (#205, 2026-11-11)

## Links

- #205, #204, #79
- `docs/storage-choices.md`
