# ADR 0014: A boot loader that picks one of four images

- Status: Rejected
- Date: 2026-11-11

## Context

The single-build options were A (one image built around a runtime record) and B (a loader choosing among four images).

## Decision

Rejected: B saves almost no code and keeps four images to build, test and explain.

## Consequences

- One image ships (ADR 0004).

## Alternatives considered

Accepted instead: one image.

## Owner decision

"Rejected: option B, a boot loader that picks one of 4 images; the measured code difference between variants is only about 0.5 KB." (#205, 2026-11-11)

## Links

- #205
