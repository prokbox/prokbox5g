# ADR 0013: A boot-time unit-profile record for the single build

- Status: Rejected
- Date: 2026-11-11

## Context

When the single build was first designed, every per-unit choice read one frozen record, written once before the COP starts, at an uncached location both cores can read.

## Decision

Rejected. The brief review found real hazards, and the record added moving parts for little gain.

## Consequences

- Plain `probed_ramsize` and storage-flag values replace it (ADR 0004).

## Alternatives considered

Accepted instead: ordinary start-up values.

## Owner decision

"No unit-profile, seal or patching machinery." (#205, 2026-11-11, final direction)

## Links

- #205
