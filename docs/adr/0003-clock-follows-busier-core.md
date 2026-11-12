# ADR 0003: The clock follows the busier of the two cores

- Status: Accepted
- Date: 2026-10-25

## Context

Both cores share one clock. The old fixed points (24, 30 and 80 MHz) were set by rules and `cpu_boost()` callers, not by measured load.

## Decision

A band governor measures busy time per core over short windows. The busier core sets the band. `clockpol` rules and `cpu_boost()` stay as floors. For 0.9.0 it becomes the release default, Apple-style: whole-MHz steps, decaying boost, fixed-length bursts, pins for USB disk, video and tracks of 176.4 kHz and above. Our own core-voltage step stays developer-only.

## Consequences

- Needs b12 proof of transitions with no felt slowdown before it is the default.
- Battery is judged by long-run logs, not meters.
- High-rate playback must hold its band.

## Alternatives considered

- Per-subsystem fixed rules only (0.8.1): no response to real load.
- Voltage scaling as a default: Apple never does it.

## Owner decision

Decision D2 of the owner-agreed power package, 2026-10-25 (`docs/design/pp-governor.md` section 0); "ideally we'd do something sort of like what Apple does, which is ramp up and down the speed as demand requires it." (#280, 2026-11-10)

## Links

- #280
- `docs/design/pp-governor.md`, `docs/pp5022-reference/apple-dvfs-report.md`
