# iPod Classic 7G behavior & look spec — moved

This file used to be the whole spec (193 lines, ≈9k tokens). As of 2026-10-07
(process-audit-2 item 7) it is split into per-screen files under
`docs/design/spec/`, so an agent only loads what its task needs. No content
was dropped or changed in the split; see `docs/design/spec/index.md` for the
provenance note and a verification script's result.

This file is kept at the old path so existing references still resolve.
Read, in order of how cross-cutting they are:

- [`spec/index.md`](spec/index.md) — north star, visual language/gloss,
  navigation model, transitions, global button behaviour, list geometry,
  attribution, naming. Start here.
- [`spec/header.md`](spec/header.md) — header bar, Time in Title,
  battery/play/lock glyphs.
- [`spec/main-menu.md`](spec/main-menu.md) — main menu split screen and
  panel/art drift, main menu tree.
- [`spec/music-lists.md`](spec/music-lists.md) — Music menu, browse lists,
  two-line rows, quick-scroll, chevron/selection-bar rendering, Search.
- [`spec/now-playing.md`](spec/now-playing.md) — SELECT cycle (scrubber,
  rating, shuffle, lyrics), audiobook view, chapter art, volume, the
  hold-SELECT menu.
- [`spec/audiobooks.md`](spec/audiobooks.md) — the Audiobooks menu (book
  list, Resume + chapters).
- [`spec/settings.md`](spec/settings.md) — Settings split screen, the full
  17-row tree, About/Legal, Backlight, Brightness, Audiobooks speed, Clicker.
- [`spec/extras.md`](spec/extras.md) — the Extras menu.
- [`spec/cover-flow.md`](spec/cover-flow.md) — Cover Flow layout and split
  transition.
- [`spec/system-screens.md`](spec/system-screens.md) — dark full-screen
  screens, USB connected (dock glyph vs. sync arrows), charging/boot/resume
  behaviors.

`docs/design/7g-catalogue.md` is unaffected (it's well under the size that
warrants a split) and still cites section names from the spec; those names
now live in the files above.
