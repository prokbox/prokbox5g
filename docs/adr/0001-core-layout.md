# ADR 0001: Apple core layout: audio on the COP, the rest on the CPU

- Status: Accepted
- Date: 2026-11-05

## Context

The PP5022 has two cores. The earlier plan (19-9) moved the UI to the second core and kept playback on the first. It needed 238 remote-call stubs, each a whole-cache sync point, and every sitting found a new cache-coherency fault. Apple's firmware does the opposite: the second core runs an audio engine.

## Decision

Follow Apple's layout. The CPU runs the UI, file system, storage, USB and the video-chip driver. The COP runs the audio engine. The UI-on-the-COP split was retired and compiled out of the release build.

## Consequences

- The cross-core surface shrinks to 30-50 mostly command-style audio functions.
- Frees 10,880 B of `_end` in the release build.
- A COP that will not take a run falls back to the CPU with a loud log line.
- Audio no longer shares a core with UI or disk stalls.

## Alternatives considered

- Keep the UI on the COP (19-9): rejected, diffuse coherence bugs.
- Everything on the CPU (the 0.7.x layout): shipped only in the early 0.8.0 plan, then superseded.

## Owner decision

"Superseded for 0.8.0 (owner, 2026-11-07): decoding is on the COP by default in every shell build" (`docs/design/core-layout-decision.md`). The retirement of the UI-on-the-COP plan is recorded as "owner decision 2026-11-05, #148" in `docs/release-plan.md`.

## Links

- #148
- `docs/design/core-layout-decision.md`, `docs/pp5022-reference/apple-cop-audio.md`
