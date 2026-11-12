# ADR 0015: iAP as a module loaded on demand, in 0.8.3

- Status: Rejected
- Date: 2026-11-11

## Context

iAP is dormant on units without a dock. A loadable module would grow the arena only while a dock session is live.

## Decision

Deferred, not refused: iAP stays resident in 0.8.3 (ADR 0005). The module idea is revisited with the dock rework in 0.9.0, if the +28 KB of `_end` matters.

## Consequences

- No module machinery for variants while resident `_end` stays under the gate cap.

## Alternatives considered

Accepted instead: resident and dormant (ADR 0005).

## Owner decision

"iAP stays resident in 0.8.3." (#205, 2026-11-11, review round). The tracker note adds that making it loadable is "a possible later saving".

## Links

- #205, #236
