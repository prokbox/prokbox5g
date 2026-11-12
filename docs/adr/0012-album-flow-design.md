# ADR 0012: Album Flow: menu entry only, a SLANT renderer, a 32-cover cache on the video chip

- Status: Accepted
- Date: 2026-11-11

## Context

The 7G has an album browser that flips through cover art. The video chip draws every frame; there is no new resident RAM on the main processor.

## Decision

Open Album Flow from Music (first row); a Main Menu row is optional and off by default. Render with a new `SLANT` column job (an exact perspective quad), not `TRIS`, which is affine and kept for games. Keep 32 covers (32 KB each) in the chip's SDRAM while it is open. Albums without art show a placeholder. There is no CPU renderer: if the chip is unavailable, a styled pop-up appears; on a release image not cleared for jobs the row is hidden. The last screen is saved for an instant open.

## Consequences

- `SLANT` needs an overlay of 14 KB or less that evicts the visualiser and the decoder while open (the art pass pauses).
- Needs the overlay-swap fix (#257) proved first.
- Status: design and prototype only; 0.9 for testing, 1.0 to finish.

## Alternatives considered

- `TRIS` quads: affine distortion.
- A CPU renderer: against ADR 0008.
- A tilt trigger: the 7G had none.

## Owner decision

"Entry: only the menu item. Music > Album Flow is the first row ... There is no tilt or other trigger." and "VC cover cache: 32 slots, 1 MB of J_CLS_COVERFLOW." (Album Flow brief rev 2, owner decisions 2026-11-11, every default accepted)

## Links

- `docs/design/spec/cover-flow.md`, `docs/launch-story.md`, `docs/phase19-brief.md` (19-7)
