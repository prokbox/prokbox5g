# ADR 0017: No plugins as they are; games through the sandboxed game kits

- Status: Accepted
- Date: 2026-11-12

## Context

The firmware ProkBox5g is built on runs small programs called plugins, with full access to the player's memory, screen and storage. That is fine for a general-purpose firmware but it clashes with our aims: a curated experience, a fixed memory budget for playback, and a player that a faulty add-on cannot lock up or corrupt.

## Decision

Existing plugins will not run in ProkBox5g as they are. Games and apps come through our own game kits (the first is planned for 0.9.2), which run in a sandbox with a declared size and memory class, and only the access they are given.

## Consequences

- A broken or greedy game cannot take memory from playback or damage the library.
- Existing plugins cannot simply be copied across; a game needs porting to a kit.
- The kits need their own documentation and tests; the work is tracked on the roadmap.

## Alternatives considered

- Keep the plugin loader: no limit on what a plugin can touch, and every plugin has to be checked against the memory budget by hand.
- Keep it behind a hidden developer switch: the code and its testing cost stay, for something we would not support.

## Owner decision

Owner, 2026-11-12: Rockbox plugins as they are are not planned; games and apps come through ProkBox5g's own game kits, which run in a sandbox for safety.

## Links

- `docs/user/roadmap.md` (Not planned), `docs/roadmap/overrides.tsv`
