# ADR 0011: Keep 30 GB free and prune worktrees (dev process)

- Status: Accepted
- Date: 2026-11-11

## Context

Each parallel agent worktree with builds takes 1.5-6 GB. Fifty worktrees once filled the disk (23 GB, 2026-10-12). The general rule was a 5 GB floor.

## Decision

While working on ProkBox, keep at least 30 GB free. Big builds and gates start only above that. Finished, pushed worktrees are pruned as soon as the agent reports, keeping built zips first. Never remove another session's active worktree.

## Consequences

- Sweeps prune pushed, idle worktrees before anything else when free space is low.
- Fewer parallel builds fit on the machine; ccache is shared to compensate.

## Alternatives considered

- The 5 GB floor: too low for this workload.

## Owner decision

"You should set your space floor higher, we've consumed 30gb of space in the last 24 hours." (owner, 2026-11-11; recorded in project notes, not in a repo doc)

## Links

- `docs/process.md`
