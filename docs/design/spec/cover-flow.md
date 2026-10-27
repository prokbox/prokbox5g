# Cover Flow

*Split from `7g-spec.md` (2026-10-07).*

## Cover Flow (✅ owner video `VIDEO-2.md` §B, 33.4–40.8 s; labels `coverflow-*.jpg`)
- **Light theme:** a white / very light grey background and the normal light silver **header** ("Cover Flow", alternating with the clock under Time in Title, plus the play/pause and battery glyphs). It matches Apple's `coverflow-render.png`; it is not black.
- **Centre cover ≈128×128 at x 96–224, y 40–168** *(≈)*, facing front, with a mirror reflection fading out by y≈200.
- **Side covers:** 4–5 per side, ≈24 px apart, rotated ≈70–75° to **face the centre** (inner edge ≈95 px tall, outer ≈128 px), each reflected.
- **Text:** two bold black lines, the **album** (centre y≈198) over the **artist** (y≈213), the same size.
  - Centred when they fit; otherwise left-aligned at x≈11 and **hard-clipped at x≈309, no ellipsis**.
  - The text cross-fades between albums.
- **Motion:** the wheel steps album by album and the centre cover **turns as it travels** (two covers form a "V" mid-step). No momentum after the thumb stops.
- **Order:** alphabetical by artist.
- **Placeholder:** a dark slate square with a light note glyph until the art loads, then a cross-fade.
- ⚠️ Not yet seen: SELECT on a cover (flip to its track list), and quick-scroll in Cover Flow.
