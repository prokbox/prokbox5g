# Shell fonts

Bitmap fonts for the iPod UI shell, installed to `/.rockbox/fonts/` by `make install` (simulator) and `make zip` (firmware) when the build has `HAVE_IPOD_UI` (hook in `tools/buildzip.pl`). Loaded by `ui_theme_init()` in `apps/ipodui/theme.c`; if a file is missing the shell logs it and uses `FONT_UI`.

| File | Source | Size | Role | Bytes (= RAM) |
|---|---|---|---|---|
| `15-ShellSans-Bold.fnt` | texgyreheros-bold.otf | 15 px (cap 11, x-height 9) | list rows | 45 068 |
| `12-ShellSans-Bold.fnt` | texgyreheros-bold.otf | 12 px (cap 9, x-height 7) | header, Now Playing secondary | 29 022 |
| `12-ShellSans.fnt` | texgyreheros-regular.otf | 12 px (cap 9) | small text | 28 024 |
| `35-ShellSans-Letters.fnt` | texgyreheros-bold.otf | 35 px, A-Z only, cells cropped to the ink and trimmed to the caps (cap 26) plus one row for Q's tail | quick-scroll letter, drawn scaled to a 35 px cap (bilinear, `qscroll.c`); loaded once at shell start | 7 990 |

Total 110,104 B of files, about 110,850 B resident (108.3 KiB: **over** the 100 KB font budget of `docs/hardware.md` by about 8.3 KiB; +7,316 B for the wider character set of 2026-11-06, below; it was 102,788 B, and 103,188 B before the left-bearing fix, Phase 8a), since the curly quotes and the heavier list weight were added; the owner accepts a little over for readability (2026-10-08); it was 101,354 B / 99.7 KiB). The letters were a 48 px cut (17,066 B) loaded per browse screen until the Phase 4 review.

All three keep Heros' natural proportions at a 73/72 em (15.2 / 12.2 px), where the hinter puts caps and x-height on the 7G's rows, and get the 7G's word widths from letter-spacing (tracking) rather than from wider glyphs. The list font's stems are 0.1 px heavier (`CONVTTF_WEIGHT=0.1`, owner pick 2026-10-08; Round 4 had -0.15): 2.2 px with a grey side column, like the 7G's. See `docs/design/fonts.md`, Rounds 4 and 5.

All are 4-bit anti-aliased, U+0020 to U+0237 (Latin up to Extended-B as far as Heros goes) plus 32 more characters in the C1 slots (below, "Coverage"), except the letters font: U+0041 to U+005A, without `-A` (cells cropped to the ink) and with `-TA 35 -TD 90` trimming the cell to the capitals (`#` is drawn by `qscroll.c`).

## Coverage

What the three text fonts draw (Bold 15, Bold 12 and Regular 12 cover the same set):

| Group | Characters | Why |
|---|---|---|
| ASCII, Latin-1 Supplement, Latin Extended-A in full | U+0020..U+017F (ĸ ŉ Ŧ ŧ new: Heros lacks them) | nl, de, fr, es, it, pt, the Nordic languages, pl, cs, hu, tr; mis-tagged libraries |
| Latin Extended-B, as far as Heros has it | 51 glyphs in U+0180..U+0237, among them ș ț Ș Ț (U+0218..U+021B, ro), ƒ, Ǻ..ǿ, the pinyin vowels | ro and odd tags |
| Music symbols | ♩ ♪ ♫ ♬ (U+2669..U+266C) | subtitle cues ("♪ The Simpsons ♪"), track and podcast titles |
| Typographic punctuation | ‘ ’ ‚ “ ” „ ‹ › « » – — … • · † ‡ ′ ″ ‰, thin space (U+2009) | tags and subtitles typed in word processors; `«»` `„“` `‹›` for fr/de/nl quotes |
| Signs | © ® ™ ° № € £ ¥ ¢ § ¶ ¡ ¿ × ÷ ± ½ ¼ ¾ ¹ ² ³ − ← → ˆ ˜ | prices, ordinals, credits, Spanish punctuation |
| Ligatures in tags | ﬁ ﬂ (U+FB01, U+FB02), ĳ Ĳ, ŉ | text pasted from PDFs and e-books |
| Aliases (no glyph of their own) | en/em/figure spaces → space; six-per-em, punctuation, hair, narrow no-break and medium math spaces → thin space; ‐ ‑ → hyphen; ‒ → en dash; ― → em dash; ʼ → ’; ․ → .; ‧ → · | they look the same at these sizes |
| Raw C1 code points U+0080..U+009F | read as Windows-1252 (€ ‚ ƒ „ … † ‡ ˆ ‰ Š ‹ Œ Ž ‘ ’ “ ” • – — ˜ ™ š › œ ž Ÿ) | ID3v1 tags written in Windows-1252 and read as Latin-1 |

Characters above U+0237 reach the font through the 32 C1 control slots U+0080..U+009F, which text never displays as themselves: a font is one contiguous range, and reaching U+266A directly would add about 9,800 empty offset and width entries (~29 KB per font). The slot table is `firmware/include/font_shell_map.h`, read by both the converter (`convttf-shell.c`) and `firmware/font.c` (`font_remap()`, `HAVE_IPOD_UI` only), so the two can't drift. A slot whose Windows-1252 character is in the set keeps that position; the slots of Windows-1252 letters that already have a glyph in range (Š Œ Ž š œ ž Ÿ ƒ) and the five unassigned ones carry the rest. All 32 slots are used; more characters need either fewer aliases-turned-glyphs or a second range (below).

Anything else (Greek, Cyrillic, CJK, emoji, zero-width characters) draws as the default character, a space, as before; the Notes and Contacts readers draw a box or '?' for it (`font_has_glyph()`).

**Not covered, with the cost if wanted later.** Heros has 49 of the 57 basic Greek letters and no Cyrillic. Both would need the range extended to U+045F (+552 offset and width entries, ~1.7 KB per font) plus about 145 glyphs (~16 KB at 15 px, ~10 KB at 12 px): about 41 KB over the three fonts, with Cyrillic from another face. Better as a separate on-demand font than in these. Zero-width characters (U+200B..U+200D, U+2060, U+FEFF) can't be drawn as nothing: `tools/convttf.c` drops zero-width glyphs, so they draw as a space.

`apps/ipodui/fonts/fntsheet.py <font dir> <out.png>` draws the set from the `.fnt` files the way `font_remap()` maps it (1x and 4x, plus sample strings) and exits 1 if any glyph of the set is missing; `tests/ui/ipodui-font-coverage.script` shows the same characters from tags in the simulator (header, list rows, Now Playing).

## Glyph sources

Every glyph comes from **TeX Gyre Heros** except these ten, which Heros lacks; they come from **DejaVu Sans 2.37** (Bold for the bold fonts, Book for Regular 12), fitted to Heros by `shell_fallback[]` in `convttf-shell.c`:

| Character | Fit (scale, extra stem weight, vertical move) |
|---|---|
| ĸ U+0138, ŉ U+0149, ŧ U+0167 | 94 %, -, - |
| Ŧ U+0166 | 100 %, -, - |
| thin space U+2009 | 100 %, -, - (advance only) |
| ′ U+2032, ″ U+2033 | 100 %, -, - |
| ♩ U+2669 | 100 %, +1.0 px, - |
| ♫ U+266B | 100 %, +0.75 px, +0.63 px up |
| ♬ U+266C | 100 %, +0.38 px, - |

♪ U+266A, ﬁ, ﬂ, №, ←, → and the rest are Heros' own. DejaVu rather than an OFL face (Source Sans 3 has most of these too): OFL glyphs would make the whole font a modified OFL font, to be distributed under the OFL only, which the GUST licence of the Heros glyphs doesn't allow; the Bitstream Vera licence of DejaVu allows its glyphs in a font under another licence, with its notice kept and without the names "Bitstream" or "Vera" (`DEJAVU-FONT-LICENSE.txt`). No Apple font is used, extracted or traced.

## License and naming

These are converted bitmaps of **TeX Gyre Heros 2.004** (GUST e-foundry), distributed under the GUST Font License (`GUST-FONT-LICENSE.txt`, LPPL 1.3c based), plus the ten DejaVu Sans glyphs above under the Bitstream Vera Fonts licence (`DEJAVU-FONT-LICENSE.txt`; the DejaVu changes are in the public domain). As both licences request for derived works, they carry their own name ("ShellSans") instead of "TeX Gyre Heros", "DejaVu", "Bitstream" or "Vera".

## Regenerate

The sources are `tex-gyre/opentype/texgyreheros-{bold,regular}.otf` from <https://mirrors.ctan.org/fonts/tex-gyre.zip> and `ttf/DejaVuSans{-Bold,}.ttf` from <https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip>, in one directory. `tools/convttf.c` needs Homebrew `freetype` and `pkg-config`.

```sh
apps/ipodui/fonts/gen.sh <dir containing texgyreheros-*.otf and DejaVuSans*.ttf>
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
- `CONVTTF_CP1252=1` fills the C1 control slots U+0080..U+009F with the 32 characters of `font_shell_slot[]` (`firmware/include/font_shell_map.h`; see "Coverage"). It began (Phase 3) with seven, the curly quotes, en/em dash and ellipsis, for "Sam’s iPod"; the 2026-11-06 pass filled the rest. Each glyph is a full-height cell (about 110 B at 15 px, 70 B at 12 px): +1,730 B for the first seven, +7,316 B for the other 26 and ĸ ŉ Ŧ ŧ over the three text fonts.
- `CONVTTF_FB=<file>`: the fallback face (DejaVu Sans) for the glyphs of `shell_fallback[]` ("Glyph sources"). The converter asks for the main font's `.` as a placeholder, loads it unrendered, and renders the fitted DejaVu outline in its slot, so tracking, `CONVTTF_WEIGHT` and `CONVTTF_LSB` apply as to any glyph. A missing or unreadable face stops the run.
- `CONVTTF_LSB=clip` (Phase 8a): a `.fnt` glyph is a cell starting at the pen origin, so a glyph whose ink starts left of it (negative left bearing) can't be placed exactly. `convttf.c -A` clamps the bearing to 0, which moves the whole glyph right by 1–2 px while the advance keeps the ink, so the advance grows too: Regular 12 "Eject" read "E ject", and `/ \ _` stood a pixel apart from their neighbours. The wrapper now drops the ink columns left of the origin instead (the tip of a hook or a slash, at most 2 columns; `CONVTTF_LSB_REPORT=1` lists each glyph and the ink dropped), so stems sit where FreeType put them and advances are the tracked ones. Changed: Bold 15 and Bold 12 24 advances each, Regular 12 16, all `/ \ _` plus the en/em dash slots (bold), `¤`, and accented `I`/`i`, `Ħ`, `ĵ`, `ǰ` forms; Regular 12 also redraws `j` and `ȷ` (no advance change). Cap height, x-height, line height and ascent are unchanged; the letters font has no `-A` and is generated with `CONVTTF_LSB` unset (unchanged).
- `CONVTTF_HINT` (normal, light, auto, autolight, none) selects the hinting; the default (normal) is used. For these CFF fonts light and normal give the same output.

The output is deterministic, so running the script again should reproduce the committed files byte for byte.

After a regeneration, `fntcheck.py <before dir> <after dir> [sheet dir]` compares the old and new fonts: header, line height, ascent, cap height and x-height (ink rows of `H I x n`), every advance width, and lists the glyphs whose pixels changed; with a sheet dir it writes before/after glyph sheets (PNG, 4x) of those glyphs. It exits 1 if a metric changed.
