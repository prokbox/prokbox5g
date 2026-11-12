# ADR 0008: Video chip first, the CPU only as a loud fall-back

- Status: Accepted
- Date: 2026-11-10

## Context

Cover-art building held the CPU (one panel took 18.5 s; an MP3 took 6-7 s to start). The video chip can decode JPEG and scale.

## Decision

Jobs that suit the video chip go there first. Art building moves to a baseline JPEG decode job on the chip (#288). A CPU path is a loud-logged last resort, never the design. Library building still comes first, then playback. VC art may also run on battery while the charge is at least 30 %, including screen-off playback; below 30 % the old rule (lit or charging only) holds. The CPU fall-back keeps the old rule.

## Consequences

- Needs the overlay-swap fix (#257) proved on silicon first.
- The merged video-chip image has little free room; each feature checks it.
- Energy per album is measured by sim proxies and logged duty, not meters.

## Alternatives considered

- Make the art builder yield on the CPU: kept as the short-term fix only.

## Owner decision

"we don't want to regress or give up on the video chip"; "our goal here is to take real and full advantage of the VC -- so software fallback isn't much of a real thing long term." (owner, 2026-10-25, project notes, not a repo doc). Battery allowance: owner confirmed 2026-11-10 (#288).

## Links

- #288, #257, #259
