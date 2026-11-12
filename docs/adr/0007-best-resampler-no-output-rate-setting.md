# ADR 0007: Best-quality sample-rate converter for every ratio; no user-set output rate

- Status: Accepted
- Date: 2026-11-10

## Context

0.8.1 used a band-limited converter only for whole-number ratios and Hermite for the rest. Hermite images sit only about 8 dB down and sounded harsh in the b11 listening row.

## Decision

Use a band-limited (polyphase or windowed-sinc) converter on every ratio, sized to run on the COP. Output follows the source's family: play at the highest enabled rate in that family, so conversions stay whole-number (2:1, 4:1). Low rates go up to 44.1 or 48 kHz. There is no output-rate setting. A loud-logged fall-back exists only for CPU starvation.

## Consequences

- Cross-family ratios (for example 48 to 44.1) are costly, and matter only for a future fixed-rate sink such as a dock; row caching waits for that.
- Tests: host goldens (flatness, at least 90 dB image rejection) and a listening arm.

## Alternatives considered

- Keep Hermite for odd ratios: audibly worse.
- A user-set output rate: more knobs, against the curated experience.

## Owner decision

"as high quality a converter as possible always working." and "users will never set the output frequency themselves. Output follows the source's rate family; the only choice is Hi-Res on/off (#270)." (#271, 2026-11-10)

## Links

- #271, #270
