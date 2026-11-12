# ADR 0002: The whole playback chain runs on the COP

- Status: Accepted
- Date: 2026-11-10

## Context

Since 0.8.1 the COP takes the PCM interrupt and refill and runs the decode. The PCM insert, DSP, resampler and dither still ran on the CPU's codec thread. A 24/192 FLAC needs about 1.2 M cycles per 4096-sample frame, and cross-core hand-offs cost cache syncs.

## Decision

Put the entire chain (decode, insert, DSP, resample, output) on the COP, next to the interrupt. Shrink first (decoder kernels, a faster first half-band stage, 24-bit output with no dither), then move the chain. A CPU/COP split for hi-res is the last resort, tried only if one core cannot reach the target.

## Consequences

- The CPU is free for UI, disk, USB and the video chip.
- The COP's cached data is invisible to the CPU (quirk 88), so the hand-off and the PCM buffer ownership need a design and tests.
- Gate: real-code fence tests for the co-location (#290).

## Alternatives considered

- Decode on the COP, resample on the CPU: proposed as a lever for 24/192, then ranked last by the owner.

## Owner decision

"hopefully we can shrink everything down and get it all running on the COP." (#282, 2026-11-10); "keep all audio decoding and handling on ONE core (the COP) as much as possible." (#278, 2026-11-10)

## Links

- #282, #278, #290
- `docs/pp5022-reference/quirks.md` (88)
