# ADR 0005: iAP stays resident and dormant; OUT2 is always on

- Status: Accepted
- Date: 2026-11-11

## Context

Dock support was missing from the modded zips, and several users call a missing dock a deal-breaker (#236). The one build must serve a 30-pin unit and a USB-C unit alike.

## Decision

Compile iAP into every build for every unit. It stays dormant until an accessory speaks, so a USB-C unit never wakes it. OUT2 and the accessory rail are on whenever the codec runs. Per-unit variant code may stay resident while resident `_end` is under the gate cap (1,750,000 B); 2.0 MB is the absolute ceiling.

## Consequences

- Cost: about +28 KB of `_end`, paid by every unit.
- A dormancy bench row on the USB-C unit measures the idle cost of iAP and OUT2.
- The dock rework (Apple-exact line-out behaviour, the volume-swing fix) lands in 0.9.0 (#236).

## Alternatives considered

- iAP as a module loaded on demand: ADR 0015 (deferred).

## Owner decision

"iAP stays resident in 0.8.3. You can do the same for other code paths if we stay below 2MB, that's the real deal breaker." and "OUT2 line-out is always on, like Apple." (#205, 2026-11-11)

## Links

- #236, #205
