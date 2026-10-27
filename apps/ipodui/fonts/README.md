# Shell fonts

Bitmap fonts for the iPod UI shell, installed to `/.rockbox/fonts/` by `make install` (simulator) and `make zip` (firmware) when the build has `HAVE_IPOD_UI` (hook in `tools/buildzip.pl`). Loaded by `ui_theme_init()` in `apps/ipodui/theme.c`; if a file is missing the shell logs it and uses `FONT_UI`.

| File | Source | Size | Role | Bytes (= RAM) |
|---|---|---|---|---|
| `15-ShellSans-Bold.fnt` | texgyreheros-bold.otf | 15 px (cap 11, x-height 9) | list rows | 41 794 |
| `12-ShellSans-Bold.fnt` | texgyreheros-bold.otf | 12 px (cap 9, x-height 7) | header, Now Playing secondary | 26 934 |
| `12-ShellSans.fnt` | texgyreheros-regular.otf | 12 px (cap 9) | small text | 26 070 |
| `35-ShellSans-Letters.fnt` | texgyreheros-bold.otf | 35 px, A-Z only, cells cropped to the ink and trimmed to the caps (cap 26) plus one row for Q's tail | quick-scroll letter, drawn scaled to a 35 px cap (bilinear, `qscroll.c`); loaded once at shell start | 7 990 |

Total 102,788 B of files, about 103,530 B resident (101.1 KiB: **over** the 100 KB font budget of `docs/hardware.md` by about 1.1 KB (it was 103,188 B before the left-bearing fix, Phase 8a), since the curly quotes and the heavier list weight were added; the owner accepts a little over for readability (2026-10-08); it was 101,354 B / 99.7 KiB). The letters were a 48 px cut (17,066 B) loaded per browse screen until the Phase 4 review.

All three keep Heros' natural proportions at a 73/72 em (15.2 / 12.2 px), where the hinter puts caps and x-height on the 7G's rows, and get the 7G's word widths from letter-spacing (tracking) rather than from wider glyphs. The list font's stems are 0.1 px heavier (`CONVTTF_WEIGHT=0.1`, owner pick 2026-10-08; Round 4 had -0.15): 2.2 px with a grey side column, like the 7G's. See `docs/design/fonts.md`, Rounds 4 and 5.

All are 4-bit anti-aliased, U+0020 to U+024F (Latin up to Extended-B) plus seven punctuation marks in the C1 slots (below), except the letters font: U+0041 to U+005A, without `-A` (cells cropped to the ink) and with `-TA 35 -TD 90` trimming the cell to the capitals (`#` is drawn by `qscroll.c`).

## License and naming

These are converted bitmaps of **TeX Gyre Heros 2.004** (GUST e-foundry), distributed under the GUST Font License (`GUST-FONT-LICENSE.txt`, LPPL 1.3c based). As the license requests for derived works, they carry their own name ("ShellSans") instead of "TeX Gyre Heros".

## Regenerate

The source is `tex-gyre/opentype/texgyreheros-{bold,regular}.otf` from <https://mirrors.ctan.org/fonts/tex-gyre.zip>. `tools/convttf.c` needs Homebrew `freetype` and `pkg-config`.

```sh
apps/ipodui/fonts/gen.sh <dir containing texgyreheros-*.otf>
```

`gen.sh` builds `convttf-shell.c` (the unchanged `tools/convttf.c` plus a few FreeType knobs read from the environment) into a temporary binary and runs:

```sh
export CONVTTF_HDPI=73 CONVTTF_VDPI=73 CONVTTF_GAP=soft CONVTTF_CP1252=1
CONVTTF_TRACK=0.4 CONVTTF_WEIGHT=0.1 \
convttf -A -X 72 -s 32 -l 591 -p 15 -o 15-ShellSans-Bold.fnt texgyreheros-bold.otf
CONVTTF_TRACK=0.45 \
convttf -A -X 72 -s 32 -l 591 -p 12 -o 12-ShellSans-Bold.fnt texgyreheros-bold.otf
CONVTTF_TRACK=0.45 \
convttf -A -X 72 -s 32 -l 591 -p 12 -o 12-ShellSans.fnt      texgyreheros-regular.otf
convttf -X 72 -s 65 -l 90 -TA 35 -TD 90 -p 35 -o 35-ShellSans-Letters.fnt texgyreheros-bold.otf
```

- `-A` uses real advance widths and left bearings (see `docs/design/fonts.md`, Round 2).
- `-X 72` makes `-p` the pixel em size; `CONVTTF_{H,V}DPI=73` scales it by 73/72 in both directions (fractional sizes).
- `CONVTTF_TRACK` adds letter-spacing in pixels to the unhinted advance before rounding, so a fractional value averages out over a word.
- `CONVTTF_WEIGHT` thickens (positive) or thins (negative) stems horizontally by that many pixels in total, before rendering. Advances come from the unhinted width plus tracking, so the weight does not move letter spacing, except where the ink would overflow the advance: at 0.1 (vs. -0.15) nine Bold 15 advances grow by 1 px, `K X ¨ Î ď Ħ Ĵ Ķ Ǐ`; no other width changes. No fixed shell string with K or X comes near a row edge ("OK" 23 -> 24 px).
- `CONVTTF_GAP=1` keeps separate parts separate. At 73/72 the hinter rounds the x-height up (15 px bold: 8.2 to 9 px) but keeps the i dot on the cap line at its own height, so the dot's bottom (8.86) lands on the stem's top (9.00) and "Music" reads "Muslc". The converter groups the hinted outline's contours into parts and moves a small part (dot, accent, ring, the dot of `!`) away from the larger part it sits on, by whole pixels, until one empty pixel row separates them, with its height snapped to whole pixels. Stems, bowls and capitals never move, so cap height, x-height, advances and line height are unchanged (the i dot now sits one pixel above the cap line, level with the accents). Parts that would have to sink below the baseline (`?`) or rise out of the cell are left as they are.
- `CONVTTF_GAP=soft` (**the default since 2026-10-08**, owner pick over the snapped dot, which looked too sharp next to the anti-aliased letters): the same parts keep their natural (unhinted) height and fractional edges and sit `CONVTTF_GAP_PX` (default 0.75) px above the letter, so the dot anti-aliases like the rest of the text instead of being a hard 2x2 square. Bold 15 "i": dot rows at 88 % / full / 25 % ink above the stem, like the 7G's soft dot. `gen.sh` takes `CONVTTF_GAP`, `CONVTTF_HINT` and `LIST_WEIGHT` (the list font's `CONVTTF_WEIGHT`) from the environment to build variants (`CONVTTF_GAP=1 apps/ipodui/fonts/gen.sh <src> <outdir>` for the snapped dot). `CONVTTF_HINT=light` gives the same bytes as normal for these CFF fonts; `autolight` drops the list x-height to 8.
- `CONVTTF_CP1252=1` fills seven of the C1 control slots U+0080..U+009F, which text never displays, with the punctuation at its Windows-1252 position: `‘ ’ “ ”` (0x91-0x94), `–` `—` (0x96, 0x97) and `…` (0x85). A font must be one contiguous range, and reaching U+2026 directly would add about 7,700 empty entries (offsets and widths, ~23 KB per font). `firmware/font.c` (`font_remap()`, `HAVE_IPOD_UI` only) maps U+2018/2019/201C/201D/2013/2014/2026 onto the slots; the two tables must match. Each glyph is a full-height cell (about 100 B at 15 px), so the set is kept small: "Sam’s iPod" needs U+2019. Cost: +1,730 B over the three text fonts.
- `CONVTTF_LSB=clip` (Phase 8a): a `.fnt` glyph is a cell starting at the pen origin, so a glyph whose ink starts left of it (negative left bearing) can't be placed exactly. `convttf.c -A` clamps the bearing to 0, which moves the whole glyph right by 1–2 px while the advance keeps the ink, so the advance grows too: Regular 12 "Eject" read "E ject", and `/ \ _` stood a pixel apart from their neighbours. The wrapper now drops the ink columns left of the origin instead (the tip of a hook or a slash, at most 2 columns; `CONVTTF_LSB_REPORT=1` lists each glyph and the ink dropped), so stems sit where FreeType put them and advances are the tracked ones. Changed: Bold 15 and Bold 12 24 advances each, Regular 12 16, all `/ \ _` plus the en/em dash slots (bold), `¤`, and accented `I`/`i`, `Ħ`, `ĵ`, `ǰ` forms; Regular 12 also redraws `j` and `ȷ` (no advance change). Cap height, x-height, line height and ascent are unchanged; the letters font has no `-A` and is generated with `CONVTTF_LSB` unset (unchanged).
- `CONVTTF_HINT` (normal, light, auto, autolight, none) selects the hinting; the default (normal) is used. For these CFF fonts light and normal give the same output.

The output is deterministic, so running the script again should reproduce the committed files byte for byte.

After a regeneration, `fntcheck.py <before dir> <after dir> [sheet dir]` compares the old and new fonts: header, line height, ascent, cap height and x-height (ink rows of `H I x n`), every advance width, and lists the glyphs whose pixels changed; with a sheet dir it writes before/after glyph sheets (PNG, 4x) of those glyphs. It exits 1 if a metric changed.
