# UI typeface comparison

Goal: choose the open typeface for the 7G-style shell (see `docs/design/spec/index.md`). There are three roles:

| Role | Size | Weight |
|---|---|---|
| List text | about 16 px | Regular |
| Header title | about 13 px | Bold |
| Secondary text | about 12 px | Regular |

All candidates were converted with `tools/convttf` to 4-bit anti-aliased Rockbox `.fnt` files. Each one was rendered twice:
- in a mock 7G list (26 px rows, blue gradient selection, 22 px silver header), drawn from the real `.fnt` glyph bitmaps;
- in the stock simulator, as its UI font.

## Candidates

| Font | Source | License |
|---|---|---|
| Inter 4.1 | https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip (`extras/ttf/Inter-{Regular,Bold}.ttf`) | SIL OFL 1.1 |
| Source Sans 3 (3.052R) | https://github.com/adobe-fonts/source-sans/releases/download/3.052R/TTF-source-sans-3.052R.zip (`TTF/SourceSans3-{Regular,Bold}.ttf`) | SIL OFL 1.1 |
| TeX Gyre Heros 2.004 | https://mirrors.ctan.org/fonts/tex-gyre.zip (`tex-gyre/opentype/texgyreheros-{regular,bold}.otf`) | GUST Font License (LPPL-based, free) |
| Liberation Sans 2.1.5 | https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz | SIL OFL 1.1 |

Arimo, the Google Fonts version of the same Liberation design, is skipped. It only ships as a variable font, and `convttf` would only pick up its default instance.

## Metrics

All sizes are em sizes in pixels (`-X 72`). The character set is U+0020 to U+024F (Latin up to Extended-B), converted with light hinting (`-L`).

Column notes:
- **cap / x-h / desc:** measured from the ink of `H`, `x` and `p` (pixels at 50% or more coverage).
- **line h:** the font height Rockbox uses as its row height.
- **avg w:** the mean advance over a pangram.
- **chars/row:** how many average characters fit in a 290 px full-width row, and in a 140 px split-screen row.

| Font | Size | line h | cap | x-h | desc | avg w | chars/row 290 / 140 | .fnt bytes |
|---|---|---|---|---|---|---|---|---|
| Inter | 16 | 20 | 12 | 9 | 3 | 7.42 | 39 / 18 | 49 046 |
| Inter Bold | 13 | 17 | 9 | 7 | 3 | 6.65 | 43 / 21 | 38 238 |
| Inter | 12 | 15 | 9 | 7 | 3 | 5.86 | 49 / 23 | 29 398 |
| Source Sans 3 | 16 | 22 | 11 | 8 | 3 | 6.44 | 45 / 21 | 36 866 |
| Source Sans 3 | 17 | 23 | 12 | 9 | 4 | 6.74 | 43 / 20 | 40 900 |
| Source Sans 3 Bold | 13 | 18 | 9 | 7 | 3 | 6.00 | 48 / 23 | 27 978 |
| Source Sans 3 | 12 | 16 | 8 | 6 | 2 | 5.12 | 56 / 27 | 21 618 |
| TeX Gyre Heros | 16 | 24 | 12 | 9 | 3 | 7.00 | 41 / 20 | 38 976 |
| TeX Gyre Heros Bold | 13 | 19 | 10 | 7 | 3 | 6.49 | 44 / 21 | 27 946 |
| TeX Gyre Heros | 12 | 18 | 9 | 7 | 2 | 5.47 | 53 / 25 | 23 334 |
| Liberation Sans | 16 | 19 | 12 | 9 | 3 | 6.93 | 41 / 20 | 46 634 |
| Liberation Sans Bold | 13 | 15 | 9 | 7 | 3 | 6.67 | 43 / 20 | 33 810 |
| Liberation Sans | 12 | 14 | 9 | 7 | 3 | 5.49 | 52 / 25 | 27 708 |

The line heights come from each font's hhea ascender and descender, and they vary a lot (Heros is 24 px for 16 px text). The shell should place text by baseline inside its own 26 px rows rather than rely on `font->height`. To shrink the bitmaps, trim them with `-Ta`/`-Td`.

The `.fnt` sizes are driven by the U+0250 range. Dropping to Latin-1 (`-l 255`) roughly halves them.

## Screenshots

All screenshots are in `out/shots/fonts/`, which is not committed.

| File | What it shows |
|---|---|
| `specimen-mock.png` | Mock 7G main menu (split screen) and Settings list for every candidate, plus Source Sans 3 at 17 px. Shown 2x. |
| `specimen-hinting.png` | Light hinting (`-L`) vs. full hinting for Inter and Source Sans 3. |
| `sim-sheet.png` | Stock simulator using each 16 px font: main menu, Settings and General Settings. Rows are one line height tall (no padding in the stock list). Order: Inter, Source Sans 3, Heros, Liberation. |
| `sim-settings-2x.png` | Zoomed simulator Settings crops. Top row: Inter and Source Sans 3. Bottom row: Heros and Liberation. |
| `<Font>/{main,settings,general}.png` | The individual simulator shots. |

The simulator shots use the stock sim, so they still show stock strings. They are only there to check how the real LCD renderer draws the fonts.

## Assessment

**Source Sans 3**
- It is humanist and open, with a single-storey feel. Of the four it is the closest to the 7G's Myriad-style lettering: soft, slightly condensed and "Apple 2007".
- It is the narrowest (45 characters per full row at 16 px), which matters for long album and book titles.
- It has the smallest files.
- At 16 px the caps are 11 px and look slightly light against the blue selection. At 17 px (cap 12) it matches the visual size of the others, and still fits 43 characters per row.
- The 13 px Bold header is crisp and has the right weight.
- The 12 px secondary text is small (x-height 6) but readable.
- Word spaces are a little tight at 17 px ("Volume Limit"). Converting with `-c` or adding spacing in the shell would fix that if needed.

**Inter**
- It renders most cleanly, with even stems and the best hinting result. It was designed for screens.
- But it is wide (39 characters per row) and has a large x-height. The result looks like a modern web UI rather than a 2007 iPod.
- Its files are the largest.
- With full hinting (no `-L`), the autohinter snaps the 13 px Bold up to cap 11, which makes the header noticeably bigger.

**TeX Gyre Heros**
- This is the "Helvetica-like" option named in `CLAUDE.md`.
- The glyph shapes are good, but at 16 px light hinting produces uneven spacing ("Settlngs" and "Backlight" show `i`/`l` collisions and gaps).
- Its line metrics are the tallest.
- It feels more like a Mac desktop than the iPod.

**Liberation Sans**
- It is metric-compatible with Arial, and it shows: its `R`, `G` and `t` look like Arial.
- It has the same spacing glitches as Heros ("Recordlng" in the simulator).
- It is the weakest match.

All four have good 4-bit anti-aliasing. Hinting choice mattered more than typeface for crispness.

> Round 2 found that the spacing glitches above ("Settlngs", "Recordlng") come from a `convttf` bug, not from the fonts. See "Round 2" below.

## Round 1 recommendation (superseded by Round 2)

Round 1 recommended **Source Sans 3**:

| Role | File | Bytes |
|---|---|---|
| List | `17-SourceSans3.fnt` | 41 KB |
| Header | `13-SourceSans3-Bold.fnt` | 28 KB |
| Secondary | `12-SourceSans3.fnt` | 22 KB |

The reasons:
1. It is the closest in spirit to the real 7G typeface.
2. It fits the most text per row in both the split and full layouts.
3. Its RAM and disk cost is the lowest.
4. Its license is OFL, which allows conversion and redistribution as long as the name "Source" is not used for modified fonts. Our converted bitmaps should get their own name in the shell, for example `17-ShellSans.fnt`, with OFL.txt shipped next to them.

**Runner-up: Inter with full hinting.** Choose it if maximum crispness matters more than period feel.

**Decision (2026-10-06): Source Sans 3 accepted.** The user noted that the real 7G font is Apple's Podium Sans, which is proprietary and unreleased, so it can't be shipped. Source Sans 3 is the closest open alternative. `CLAUDE.md` and `7g-spec.md` have been updated.

Before freezing the sizes, check the 16 vs. 17 px choice against real 7G photos in `docs/reference/7g/` (that folder is still empty).

**Where the files would go:** the repo's `fonts/` folder holds BDF sources built by convbdf, so TTF-derived `.fnt` files don't belong there. Add them to a new `apps/ipodui/fonts/` folder (about 90 KB for the three files), together with `OFL.txt`, and have the install step copy them to `/.rockbox/fonts/`. Load them with `font_load()` at shell start, using the glyph cache (`font_load_ex` with a glyph limit) to bound RAM. These files have not been added yet.

## Reproduce

Build `convttf` (needs Homebrew `freetype` and `pkg-config`, both already installed):

```sh
mkdir -p out/tools
cc -std=c99 -O2 -Wall tools/convttf.c -o out/tools/convttf -lm \
   $(pkg-config --cflags --libs freetype2)
```

Notes on the `convttf` options:
- 4-bit anti-aliasing is always on (header depth 1).
- `-p` is a size in points at `-X` DPI. The default is 60 DPI, so pass `-X 72` to make `-p` the pixel em size.
- `-s` and `-l` take decimal numbers only.

The fonts are unpacked into `out/fonts-src/<family>/`. Then convert them:

```sh
C=out/tools/convttf; S=out/fonts-src; O=out/fonts
OPTS="-X 72 -L -s 32 -l 591"   # drop -L for full hinting
$C $OPTS -p 17 -o $O/17-SourceSans3.fnt      $S/sourcesans3/SourceSans3-Regular.ttf
$C $OPTS -p 16 -o $O/16-SourceSans3.fnt      $S/sourcesans3/SourceSans3-Regular.ttf
$C $OPTS -p 13 -o $O/13-SourceSans3-Bold.fnt $S/sourcesans3/SourceSans3-Bold.ttf
$C $OPTS -p 12 -o $O/12-SourceSans3.fnt      $S/sourcesans3/SourceSans3-Regular.ttf
# The other families are the same with inter/Inter-*.ttf,
# heros/texgyreheros-*.otf and liberation/LiberationSans-*.ttf.
```

`out/fonts/gen.sh` regenerates every candidate.

Two more tools are in `out/`:
- `out/fonts/fntinfo.py` is stdlib-only Python that reads RB12 files. It provides `metrics`, `specimen` (the mock 7G renders) and `sheet` (the PNG contact sheet).
- The simulator run copies `16-<Font>.fnt` into `build/sim/simdisk/.rockbox/fonts/` and writes a temporary `config.cfg` (`font: /.rockbox/fonts/16-<Font>.fnt`, white background, gradient selector, `show icons: off`). It then runs `scripts/shots.sh tests/ui/fonts.script out/shots/fonts/<Font>`, and deletes the `config.cfg` and the copied fonts afterwards.

## Round 2: Helvetica Bold (7G)

According to Wikipedia's *Podium Sans* article, the iPod classic (6G/7G) switched to **Helvetica Bold** for menu and list text. This round therefore compares bold Helvetica clones, with Source Sans 3 kept as a reference.

**Secondary text weight on the 7G: unknown.** I could not confirm from a reliable source whether secondary lines use regular or bold weight, or which size they use. Regular is used below until reference photos land in `docs/reference/7g/`.

### Added source

| Font | Source | License |
|---|---|---|
| Nimbus Sans (URW base35, tag 20200910) | https://github.com/ArtifexSoftware/urw-base35-fonts (`fonts/NimbusSans-{Bold,Regular}.otf` at tag `20200910`) | AGPL-3.0 with an exception that only covers embedding in PostScript/PDF documents |

On Nimbus's license: the AGPL-3.0 is compatible with GPLv3 but not with GPLv2-only. Rockbox is GPLv2-or-later. If the font is shipped as a separate data file on disk (not compiled in), we would have to ship or offer the OTF source alongside it. This is workable but adds an obligation. TeX Gyre Heros is derived from the same URW design and uses the permissive GUST Font License, so Nimbus brings no visual benefit for that cost.

### The spacing glitch: a convttf bug, fixed with `-A`

Bold weights still showed the glitch: with stock `convttf`, Heros Bold reads as "Volume Limlt" and "Backlight" looks cramped. Changing hinting (`-L` vs. full) made no difference. The cause is in `tools/convttf.c`:
- `glyph_width()` sets each cell to `max(ink width, bitmap pitch)` and ignores the glyph's real advance width.
- The ink is then centred in that cell, and the left side bearing is ignored.

As a result, narrow glyphs lose their side bearings. For example, Heros 16 `l` is 2 px wide instead of 4, and `i` is 3 px instead of 4. So `il` and `tl` touch, while some round letters get extra gaps.

**Fix:** a new `-A` option in a patched copy, `out/tools/convttf-adv.c`. The diff is in `out/tools/convttf-adv.patch`, about 40 lines. With `-A`:
- the cell width is the hinted advance (or the ink extent, if that is wider);
- the bitmap is placed at `bitmap_left`;
- FreeType's row padding is skipped;
- forced-equal digits stay centred.

`tools/` is upstream code, so I have not edited it. You can either commit the patch to `tools/convttf.c` (it is opt-in, so behaviour stays the same without the flag) or keep the converter under `apps/ipodui/tools/`.

Further notes:
- **Kerning:** the Rockbox font format has no kerning, so pair kerning cannot be fixed. With correct advances it is not missed at these sizes.
- **Hinting:** `-L` and full hinting give identical output for the CFF/OTF fonts (Heros, Nimbus). Full hinting is used below.
- **Nimbus clipping:** Nimbus's hhea ascender equals its cap height, so `Å` and accented capitals are clipped. `-Ta -3` adds 3 px of ascent and fixes it. Only the 17 px Nimbus files were regenerated with that flag; the 16 px Nimbus in the 16 px sheet is still clipped.

### Metrics (all with `-A`, `-X 72`)

| Font | Size | line h | cap | x-h | avg w | chars/row 290 / 140 | .fnt bytes |
|---|---|---|---|---|---|---|---|
| TeX Gyre Heros Bold | 16 | 23 | 12 | 9 | 7.93 | 36 / 17 | 41 824 |
| TeX Gyre Heros Bold | 17 | 26 | 13 | 10 | 8.47 | 34 / 16 | 48 848 |
| TeX Gyre Heros Bold | 13 | 19 | 10 | 7 | 6.58 | 44 / 21 | 28 590 |
| TeX Gyre Heros | 12 | 18 | 9 | 7 | 5.70 | 50 / 24 | 24 540 |
| Nimbus Sans Bold (`-Ta -3`) | 17 | 21 | 13 | 10 | 8.47 | 34 / 16 | 35 640 |
| Nimbus Sans Bold | 16 | 17 | 12 | 9 | 7.93 | 36 / 17 | 28 094 |
| Liberation Sans Bold | 16 | 19 | 12 | 9 | 8.02 | 36 / 17 | 51 868 |
| Liberation Sans Bold | 17 | 20 | 12 | 9 | 8.51 | 34 / 16 | 56 716 |
| Source Sans 3 Bold | 17 | 23 | 12 | 9 | 7.84 | 37 / 17 | 46 922 |
| Source Sans 3 Semibold | 17 | 23 | 12 | 9 | 7.56 | 38 / 18 | 45 604 |

Run `python3 out/fonts/fntinfo.py metrics out/fonts/r2/*.fnt` for every variant, including the stock and `-L` builds.

### Screenshots (`out/shots/fonts2/`)

| File | What it shows |
|---|---|
| `specimen-17-bold.png` | Mock 7G screens at 17 px: Heros Bold with stock convttf, then with `-A` Heros, Nimbus, Liberation, Source Sans 3 Bold and Source Sans 3 Semibold. |
| `specimen-16-bold.png` | The same at 16 px (Heros, Nimbus, Liberation, Source Sans 3 Bold). |
| `zoom-3x.png` | 3x crops: Heros `-A`, Heros stock, Nimbus `-Ta -3`, Liberation, Source Sans 3 Bold. |
| `sim-sheet.png`, `sim-general-2x.png`, `<Font>/{main,settings,general}.png` | The stock simulator using each 17 px bold font. Sim rows are one line height tall. Order: Heros stock, Heros `-A`, Nimbus, Liberation, Source Sans 3 Bold. |

### Assessment

**TeX Gyre Heros Bold with `-A`**
- It is the most Helvetica-Bold-like: square `R`, flat terminals, tight round letters. The `-A` build removes all collisions.
- At 16 px (cap 12) it reads like the 7G list, and 36 characters fit per row.
- It renders `Å` and other accents correctly.
- The tall line height (23 px) does not matter if the shell places text by baseline. If the file size matters, `-Td` and `-Ta` can trim the bitmaps.

**Nimbus Sans Bold**
- It looks practically the same as Heros.
- It needs `-Ta -3` for accents, and its AGPL license adds an obligation.
- It brings no benefit over Heros.

**Liberation Sans Bold**
- It is clean with `-A`, but the Arial shapes show (`R`, `G`, `t`, `1`).
- It produces the largest files.

**Source Sans 3 Bold and Semibold**
- They are the crispest and narrowest.
- But they are clearly humanist, not Helvetica, so they no longer match the corrected target.

### Revised recommendation

Use **TeX Gyre Heros, converted with `convttf -A`**:

| Role | File | Size |
|---|---|---|
| List text | `16-Heros-Bold.fnt` | 42 KB |
| Header title | `13-Heros-Bold.fnt` | 29 KB |
| Secondary text | `12-Heros.fnt` (Regular, pending a 7G reference) | 25 KB |

- The total is about 96 KB at U+0020 to U+024F, or roughly half that at Latin-1.
- Use 17 px for the list if reference photos show larger caps.
- The license is GUST (free, LPPL-based). Converted files must not reuse the name "TeX Gyre Heros". Ship them as, for example, `16-ShellSans-Bold.fnt`, with `GUST-FONT-LICENSE.txt` next to them, in the proposed `apps/ipodui/fonts/`.

**Fallback:** Source Sans 3 Bold.

Two things need deciding:
- Adopting `-A` into `tools/convttf.c`. Every TTF conversion should use it.
- Re-running Round 1's regular-weight comparison with `-A`, because its spacing criticisms of Heros and Liberation came from the bug.

### Reproduce Round 2

```sh
cc -std=c99 -O2 -Wall out/tools/convttf-adv.c -o out/tools/convttf-adv -lm \
   $(pkg-config --cflags --libs freetype2)
A="out/tools/convttf-adv -A -X 72 -s 32 -l 591"
$A -p 16 -o out/fonts/r2/16-Heros-Bold.fnt out/fonts-src/heros/texgyreheros-bold.otf
$A -p 13 -o out/fonts/r2/13-Heros-Bold.fnt out/fonts-src/heros/texgyreheros-bold.otf
$A -p 12 -o out/fonts/r2/12-Heros.fnt      out/fonts-src/heros/texgyreheros-regular.otf
# Nimbus needs extra ascent for accents:
$A -Ta -3 -p 17 -o out/fonts/r2/17-Nimbus-Bold-asc.fnt out/fonts-src/nimbus/NimbusSans-Bold.otf
```

`out/fonts/gen2.sh` regenerates every Round 2 variant. Simulator shots use the same temporary `config.cfg` procedure as Round 1, with `tests/ui/fonts.script`.

## Round 3: cap heights (Phase 3 polish)

The owner found the shell text about 1 px too big. Measured in `main.png`, the reference list caps are 11 full rows plus a half row (11.5) and the header caps 9 rows; Heros Bold 16 / 13 render 12 / 10. Heros at 15 / 12 px gets the caps right but is 6-8 % too narrow: "Shuffle Songs" 98 px against the reference's 106. The 7G face is a little wider than Heros at the same cap height.

**Fix:** a horizontal resolution of 76 dpi against the vertical 72 (a 1.056 x stretch, applied by FreeType before hinting, so advances and stems scale too). `apps/ipodui/fonts/convttf-hdpi.c` includes the unchanged `tools/convttf.c` and overrides only the `FT_Set_Char_Size` resolution (env `CONVTTF_HDPI`). Result, measured in the simulator against `main.png`:

| | Reference | Bold 16 (old) | Bold 15 @ 76/72 (new) |
|---|---|---|---|
| List cap | 11 (+ half) | 12 | 11 |
| Podcasts / Extras / Settings | 69 / 47 / 62 | 70 / 47 / 61 | 70 / 47 / 61 |
| Shuffle Songs / Now Playing | 106 / 93 | 106 / 94 | 106 / 93 |
| Header "iPod" cap, width | 9, 27 | 10, 27 (Bold 13) | 9, 27 (Bold 12 @ 76/72) |

The text is now placed by baseline (`row_baseline` 15: caps on rows 4-14 of a row) instead of centring the font cell, which had set it 2 px low.

## Round 4: natural proportions, tracking instead of widening

Round 3's 76/72 widening hit the word widths, but at 4x zoom the glyphs looked squashed next to `main.png`: round letters (`o d c S`) too wide and heavy, x-height too low; the 7G letters are taller and narrower with a little more letter-spacing. Measured in `main.png` (coverage of the stems, JPEG blur included), the list caps are about 11.5 px and the x-height about 8.5-9 (the `o` 9 with overshoot); ours was cap 11, x-height 8.

**Fix:** drop the stretch and match widths with tracking. `apps/ipodui/fonts/convttf-shell.c` replaces `convttf-hdpi.c` (still including the unchanged `tools/convttf.c`) and reads four environment knobs: `CONVTTF_HDPI` / `CONVTTF_VDPI` (resolution, so `-p 15` with 73 dpi is a 15.2 px em), `CONVTTF_TRACK` (px added to the unhinted advance before rounding, so fractional tracking averages out over a word), `CONVTTF_WEIGHT` (stem thickening/thinning via `FT_Outline_EmboldenXY` before rendering) and `CONVTTF_HINT`.

Findings from the sweep (`-p 15`, 72-78 dpi, every hinting mode):
- Light and normal hinting are identical for these CFF fonts; the autohinter widens words by 1-3 px and is not better. Unhinted output blurs the cap and x-height tops. Normal hinting is kept.
- At 72 dpi the hinter gives cap 11 / x-height 8; at 73 dpi cap 11 / x-height 9; from 74 dpi the cap jumps to 12. 73 dpi is the only size with the 7G's cap and x-height.
- Tracking 0.3 px (list) and 0.45 px (header) put the word widths on the reference.
- Ink per word is the same as before (the reference, scaled to cap 11, has about the same total ink), but the 7G's stems are slightly thinner relative to its larger letters; `CONVTTF_WEIGHT=-0.15` gives the list font the same stem-to-cap ratio (stem 2.0 px vs. 2.2 before). The header keeps full weight (its reference stems are about 1.9 px).

Measured in the simulator (`tests/ui/ipodui-p2-menu.script`) and from the `.fnt` files (ink extents, 50 % threshold):

| | Reference (`main.png`) | Round 3 (15 @ 76/72) | Round 4 (15 @ 73/73, track 0.3, weight -0.15) |
|---|---|---|---|
| List cap / x-height | 11.5 / 8.5-9 | 11 / 8 | 11 / 9 |
| Music / Videos / Photos / Podcasts | 42-43 / 51 / 52 / 69 | 43 / 51 / 53 / 70 | 44 / 51 / 52 / 69 |
| Extras / Settings | 47 / 62 | 47 / 61 | 46 / 61 |
| Shuffle Songs / Now Playing | 106 / 93 | 106 / 93 | 106 / 93 |
| `l` stem (px coverage) | about 2.2 at cap 11.5 | 2.2 | 2.0 |
| Header cap / x-height | 9 / 7 | 9 / 7 | 9 / 7 |
| Header "iPod" / "Now Playing" (`nowplaying.png`) | 27 / 75 | 27 / 76 | 27 / 76 |

File sizes: 40 890 + 26 656 + 25 818 = 93 KB. The list font's line height grows from 22 to 23 (ascent 18); text is placed by baseline, so rows do not move.

## Round 5: the i dot (and the curly apostrophe)

Round 4's 73/72 em made the list "i" a dotless 11 px bar ("Muslc", "Settlngs"). Cause: the CFF hinter rounds the x-height zone up (15 px bold: 8.20 to 9.00 px; 12 px: 6.58 to 7.00) but aligns the dot to the cap-height zone with its own height, so its bottom lands at 8.86, under the stem's top (unhinted the gap is 1.0 px). Accents (é ü ñ Å) end 0.25-0.66 px above the bowl and leave a grey row. In `main.png` the 7G's "i" has a 2 px dot, a 1 px gap and an 8.5 px stem.

**Fix:** `CONVTTF_GAP=1` in `convttf-shell.c` moves small parts (dots, accents, rings, the dot of `!`) away from the letter by whole pixels until one empty row separates them, height snapped to whole pixels; the letter itself never moves. Bold 15 "i": stem rows 9-17 (x-height 9), gap row 8, dot rows 6-7, one row above the cap line, level with the accents. Line height, ascent, cap height, x-height and every advance are unchanged (`apps/ipodui/fonts/fntcheck.py`); 85 / 76 / 123 glyphs change in Bold 15 / Bold 12 / Regular 12. `?` keeps its dot (moving it would sink it under the baseline).

The owner found that hard square dot too sharp next to the anti-aliased letters and picked `CONVTTF_GAP=soft` (2026-10-08, variants in `out/font-idot/variants.png`): the mark keeps its natural height and fractional edges, 0.75 px above the letter, so the Bold 15 dot reads 88 % / 100 % / 25 % ink over the stem, close to the 7G's soft dot (about 20 / 100 / 70 / 30 % in `main.png`). Same metrics, same sizes.

`CONVTTF_CP1252=1` adds `‘ ’ “ ” – — …` in the C1 slots (U+0091-0097, U+0085), mapped by `firmware/font.c` under `HAVE_IPOD_UI`, so "Sam’s iPod" no longer shows a space: +1,730 B, which takes the fonts to 103,084 B, about 1.4 KB over the 100 KB budget. See `apps/ipodui/fonts/README.md`.

**Weight (owner pick, 2026-10-08):** the list font (Bold 15) goes from `CONVTTF_WEIGHT=-0.15` to `+0.1` (`out/font-idot/weights-8x.png`, variants -0.15 / -0.07 / 0 / +0.1). The `l` stem is now 2.20 px with a ~20 % grey side column (was 2.00 px, no grey), against the 7G's 2.2-2.3 px with ~25 %. Cap, x-height and line height unchanged; nine advances grow by 1 px where the ink would overflow (`K X ¨ Î ď Ħ Ĵ Ķ Ǐ`), every other width is unchanged (letter spacing comes from the unhinted advance plus tracking), so menu and Settings word widths are as before. +104 B (fonts 103,188 B, about 1.5 KB over the 100 KB budget, accepted by the owner for readability). The 12 px fonts keep weight 0.
