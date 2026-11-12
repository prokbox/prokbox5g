# ADR 0006: 24-bit output and Settings > Audiophile

- Status: Accepted
- Date: 2026-11-10

## Context

The release mixed and output 16 bits, so 24-bit files were reduced. The 24-bit path was proved end to end on silicon (b11 i2s24). The audio chip can run at 88.2 and 96 kHz, past its rated clock.

## Decision

For 0.9.0, output is 24-bit always (#268), including MP3 and AAC (#289). Settings > Audiophile holds the opt-in controls: native 88.2/96 kHz output (off by default), and Stereo width (off by default, once its A/B is proved audible). High-rate output needs a per-unit self-test on first enable, an instant logged fall-back on any codec, PLL, FIFO or clock error, and no use at low battery or temperature extremes. A fall-back shows one acknowledged pop-up over any screen.

## Consequences

- Recorded in `docs/design/changes-from-7g.md` as a deliberate difference, with the pop-up as an exception to "errors are logged, not shown".
- The audio buffer holds fewer seconds per byte; the floor gate runs on 32 and 64 MB.
- Proof needs b12 and a soak (heat, cold, low battery).

## Alternatives considered

- Hi-Res always on: unsafe on out-of-spec clocks.
- No Hi-Res at all: against the audiophile goal.

## Owner decision

"put 24-bit in." (#268, 2026-11-10); a new Settings > Audiophile menu, "an exception to 'curated, not customisable'." (#270, 2026-11-10)

## Links

- #268, #270, #289, #228
- `docs/design/changes-from-7g.md`
