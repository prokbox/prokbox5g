# ADR 0016: No skin or theme engine

- Status: Accepted
- Date: 2026-11-12

## Context

The firmware ProkBox5g is built on lets users restyle every screen with skins and themes. ProkBox5g's interface is a fixed, hand-tuned shell: fonts, metrics, list behaviour, transitions and the way the screens redraw are all fitted to the original player's look and to this hardware's screen and memory limits. People ask whether skins will come back.

## Decision

We will not bring back a skin or theme engine. The look is part of the product, and it is not a setting.

## Consequences

- Nothing in the shell has to be written as a loadable, parsed description of a screen, so the screens stay small, fast to draw and predictable on the player's memory budget.
- Users cannot restyle the player. Features that look like themes (for example a wallpaper choice) are judged one by one as part of the original look, not as a path to skins.
- The user docs say so on the "Not planned" part of What's coming.

## Alternatives considered

- Port the old engine as it is: it assumes a generic screen model, and the shell would have to be rebuilt around it.
- A small cut-down skin format of our own: still a second drawing path to keep matching the original look, for little gain.

## Owner decision

Owner, 2026-11-12: implementing the skin engine again would be prohibitively difficult, especially given the highly tuned nature of the current interface.

## Links

- `docs/user/roadmap.md` (Not planned), `docs/roadmap/overrides.tsv`
