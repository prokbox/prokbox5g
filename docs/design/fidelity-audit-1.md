# Fidelity audit 1: owner 7G photos vs our simulator (2026-10-08)

Input for the polish pass (#52). This audit measures the owner's 2.0.5 photos (`docs/reference/7g/owner/`) rectified to 320×240 device pixels and compares them with our current `main` simulator (HEAD `60851a0553`). Now Playing is compared with the Phase 5 shots from the `agent-adabeb765f1d22bea` worktree.

**Images (local only, gitignored, never committed):** `out/fidelity/`
- `<screen>.png`: side-by-side composites (left: rectified and white-balanced 7G; right: ours). Covers `main-menu`, `music-menu`, `artists`, `albums`, `artist-albums`, `songs`, `quickscroll`, `settings`, `backlight-leaf`, `about`, `usb`, `nowplaying`, `np-audiobook`.
- `z_*.png`: 12–16× zooms with a 1 px grid (red every 10 px): `z_hdr_right`, `z_hdr_split`, `z_hdr_left`, `z_values`, `z_chev`.
- `rect/`: every rectified photo at 1× and 4×, plus `diag.json` (quad, lattice residual, edge profiles).
- `ours/`: our captures. `tools/`: the pipeline (stdlib, numpy and ffmpeg; no PIL).

## Method and confidence

1. **Rectification.**
   - The LCD's four edges are fitted with RANSAC lines on luminance steps, from automatic hints or a rough hand-picked quad for rotated or glare-heavy photos.
   - That quad is then **refined on the LCD pixel lattice, which the phone resolves** (≈7–12 photo px per device px; FFT period 10.2 photo px in photo 1171). For 16×16 px blocks, the phase of the period-1 px luminance component locates the pixel centres. A homography plus a cubic correction for lens distortion is fitted to those offsets. The residual is 0.02–0.29 px RMS (`rect/diag.json`).
   - The lattice fixes positions only modulo 1 px. The integer offset is checked against the lit-area edges (column 0 lit and −1 dark, etc.) and corrected where the evidence was strong (`tools/shifts.json`).
   - **Absolute position uncertainty: ±1 px** (±0.5 px on the good frames: `music-menu`, `artists`, `main-hold`, `holdsel`, `chapters`, `backlight-values`, `s04`).
   - **Relative distances inside one frame** (pitch, cap height, word width): **±0.25–0.5 px**.
   - Some lit areas measure 321–322 px wide, probably from glare or bloom at the dark-panel edge. Treat right-edge positions on dark panels as ±1.5 px.
   - Unusable: about-1 (frame clipped) and the cropped NP frames (np-unrated, np-4stars, np-volume, np-scrub; the screen runs off the photo). `s03-repeat` is about 4 px off vertically and is used only for horizontal and colour readings.
2. **Colour.** Each light screen is white-balanced so the 90th-percentile list background maps to 250 grey. Glare (violet reflections, a yellow cast near the top) remains, so photo colours are **±15 per channel, relative only**. Dark panels can't be balanced.
3. **Text metrics.**
   - Measured on the 4× warp after a 1 px box blur (removes the lattice; a symmetric blur doesn't move 50 % edges), flat-fielded against local max/min.
   - Edges are taken at 50 % contrast, so stroke positions are ±0.25 px.
   - Ours is measured the same way on the exact 1× PNG.
4. **Our build.**
   - The main checkout's `build/sim-ui` renders **every header title in SYSFONT** (the 5×8 system font). `scripts/check_fonts.sh build/sim-ui` passes: the fonts are installed and byte-identical.
   - A fresh `tools/configure --ipodui` build in `build/sim-ui-audit` (same HEAD) renders ShellSans 12 Bold correctly. The fault is stale state in that build directory, not the code or the fonts. Swapping back the old 12 px font did not fix it, and the 23:11 shots from 2026-10-07 were fine.
   - All "ours" numbers come from `build/sim-ui-audit`. Its list font is the current one (`15-ShellSans-Bold.fnt` 41 788 B, height 23 / ascent 18, soft i-dot visible).
   - **No screen falls back to the system font in a correct build.** The gate should get a check that catches a SYSFONT header in a stale `build/sim-ui` (see the fix list).

## Prioritised fix list

Ordered by visibility: layout first, then typography, then materials. "Owner" is the file or constant that controls the item.

| # | Fix | 7G | Ours | Δ | Owner | Conf. |
|---|---|---|---|---|---|---|
| 1 | **Now Playing text block is too high**: move every line down ≈11 px | title cap top ≈71, artist ≈91, album ≈109, counter ≈146 (np-normal; np-ab-noart agrees ±1) | 59 / 81 / 98 / 133 | **+12 / +10 / +11 / +13** | `np_draw.h` text-line y constants (Phase 5 branch); spec `now-playing.md` (60/81/98/133 came from Apple's guide art) | high |
| 2 | **USB screen shows the stock Rockbox screen with the logo** in the sim (native-illusion breach; the screen isn't built yet) | dark slate gradient, orange disc, "Connected / Eject before disconnecting." | stock usb_screen + logo | — | Phase 8 USB screen; `apps/gui/usb_screen.c` needs a `HAVE_IPOD_UI` hook until then | high |
| 3 | **About is a blank white page** (header "About" only) | 3 dark pages (capacity, counts, model) | blank | — | Phase 11 About screens | high |
| 4 | **Row pitch pattern**: 7G uses integer rows, **4 × 25 px then 5 × 24 px** (tops 20, 45, 70, 95, 120, 144, 168, 192, 216); ours spreads 24.44 by rounding (tops 20, 44, 69, 93, 118, 142, 167, 191, 216) | baselines 38.25, 63.25, 88.25, 113.25, 137.25, 161.25, 185.25, 209.25, 233.25 (6 photos agree to ±0.25) | 38, 62, 86, 111, 135, 160, 184, 209, 233 | rows 1–6: +1.25, +2.25, +2.25, +2.25, +1.25, +1.25 | `list.c` row-top calculation (`ui_theme.list_rows` = 9); keep baseline = row bottom − 6.75 | high |
| 5 | **Settings list has no scrollbar** (17 rows) | 3 px grey thumb at x ≈148–150, with the selection bar stopping short of it (bar ends ≈147, chevron moves to ≈138–145) | none; full-width bar, chevron at 147–155 | missing | `list.c` scrollbar for split lists; `sbar_gloss` in `theme.c` | med (s04 registration ±1) |
| 6 | **Battery glyph too small** | body **24×12** at x 129–153 (split) / 288.5–312.5 (full width), y 3–15; nub 2 px to x 155 | body 20×10 + outline (x 131–151, y 4–15), nub to 153 | −4 w, −1 h, +2 px right | `header.c` `BATT_W 20 → 24`, `BATT_H 10 → 12`, `BATT_Y 4 → 3`; x anchor so the nub ends at 155 / 315 | high (two photos) |
| 7 | **Play glyph too far right / gap too small** | triangle x 111–122 (split) / 270.5–281.5 (full), y 3–15; **7 px** gap to the battery | x 274–287 (full width), 4 px gap | ≈ +3.5 px | `header.c` play x (gap 4 → 7) | high |
| 8 | **Chevron too small and too far right** | ≈8×13, stroke ≈3 px, x 145–153 (right ink 6.7 px from the list edge), rows +5…+18 in a 24 px row | 8×11, stroke 2 px, x 147–155 | −2 px x, +2 px h | chevron art (`list.c`/`rowglyphs.h`), `theme.chevron_pad` 5 → 7 | high |
| 9 | **Inline value right edge** | right ink ≈153.5 (6.5 px inside the 160 edge) | 155 | −1.5 px | `list.c` value x (`theme` value pad) | med (one usable frame) |
| 10 | **List text left inset** | ink x ≈5.75–6.0 (10 rows, 3 photos) | 7.0 | **−1 px** | `theme.text_pad_x` 6 → 5 | high |
| 11 | **List word width ≈1–1.5 px wider on the 7G** (same strings; cap and x-height already match) | Playlists 65.25, Artists 49.5, Songs 48.5, Genres 54.75, Composers 87.75, Settings 61.75, Podcasts 70.0, Shuffle Songs 107.5 | 64, 48, 47, 53, 86, 61, 69, 106 | +1.0…+1.75 (≈ +2 %) | font generator tracking (`convttf-shell.c` 15 px Bold, `-l` / tracking); about +0.15 px per advance | high |
| 12 | **Selection bar colour**: the 7G bar is a deeper, more saturated blue, about the hue of the value text, with a light top line and a darker bottom row; ours is a light sky blue | relative: bar ≈ value-blue family; 1 px lighter top row; 1 px darker row at the bottom (row 44 for row 0) | sky blue #73C2EF → #1982CE | hue and saturation | `theme.c` `sel_gloss` (move to the blue gel per `index.md`) | med (photo colour) |
| 13 | **Progress bar** thinner and grey-gel; our fill reads as a thick saturated stripe | bar x ≈58–270, y ≈210–221 incl. labels; track light grey, groove | 209–220, x 23–301 incl. labels | +1 px y | `np_draw.c` bar materials (Phase 5) | med |
| 14 | **Two-line rows: thumbnails** are art at ≈55×55, the bar starts right of the thumb; matches ours in layout. 7G sublabel grey is lighter and regular weight; ours is also regular | — | — | small | `theme.list_fg_sub` | low |
| 15 | **Gate check for a stale sim**: grep each `tests/ui` run's `sim.log` / header pixels for SYSFONT use, or do a clean reinstall in `validate.sh` | — | stale `build/sim-ui` draws headers in SYSFONT | — | `scripts/validate.sh` (tooling owner) | high |
| 16 | Hold lock not captured in our shot (`key HOLD` didn't show the lock) | lock 9×≈12 at x 108–117, y 3.5–15; **12 px** gap to the battery | not visible | — | check `header.c` hold redraw in the sim, or the script | low |
| 17 | Header clock / title | "15:12" digits ≈9.5 px tall, x 7–37.5, baseline ≈13 ±1 | "15:09" 9 px, x 7–37, baseline 13 | ≈0 | — (matches) | med |

**Already matching (keep):** header height 20 px, with the 1 px dark line at y 19 (4 photos); split at x 160; list cap height (7G 11.75–12.0 including overshoot, ours 11 + overshoot row) and x-height (7G 8.75, ours 9); list text weight (similar ink density); Settings caption bottom anchored at y ≈217 (7G 217, ours 217); Now Playing bar-row y within 1 px; text block x = 160.

## Per-screen measured deltas

### Header (light screens): `header.c`
| Element | 7G | Ours | Δ | Conf. |
|---|---|---|---|---|
| Height / bottom line | 0–19, line at 19 | same | 0 | high |
| Title / clock x | ink 7 | 7 (6 for "A") | 0 | med |
| Clock digits | 9–9.5 tall, "15:12" 30.5 wide | 9, "15:09" 30 | ≈0 | med |
| Battery body | 24×12, x 129–153 (split), y 3–15 | 20×11 incl. outline, x 131–151, y 4–15 | −4 / −1 | high |
| Battery nub | to x 155 (split) | to 153 | −2 | med |
| Play | 11×12, x 111–122 split / 270.5–281.5 full | 13×13 incl. AA, x 274–287 | +3.5 | high |
| Play → battery gap | 7 px | 4 px | −3 | high |
| Lock (Hold) | 9 wide, x 108–117, y 3.5–15, 12 px before the battery | not shown in sim | — | med |
| Play glyph look | dark grey-blue top, light cyan lower half (gel) | light centre, dark rim | material | med |

### Main menu (`main-hold`, photo 1189): `list.c`, `theme.c`, `preview.c`
- Baselines and rows as fix #4. Text inset 6.0 vs 7. "Settings" 61.75 vs 61, "Shuffle Songs" 107.5 vs 106, "Podcasts" 70.0 vs 69.
- Content difference by design: our menu hides Videos and Photos (owner decision). The photo also has Audiobooks and Alarms added.
- Extras panel art not compared (needs a straight Extras capture from ours with `drift_phase`).

### Music menu (photo 1212)
- Rows: fix #4. Chevron: fix #8 (7G x 145–153, y +5…+18 within a 24 px row).
- **Cover Flow is missing from ours** (hidden deliberately in #38 end review, `0f46d045ab`). Genius Mixes is hidden by design. The 7G list therefore starts at Cover Flow and ours at Playlists.

### Artists (photo 1213, full width, single line)
- 9 rows exactly, same 25/24 pattern. Truncation "…" is the font's ellipsis glyph on the 7G; ours untested here (no long artist names in the fixture library). **Capture request:** none needed; add a long-name fixture.

### Albums / artist's Albums (photo 1211, photo 1214)
- 4 rows of ≈55 px with a thumbnail at the left and the selection bar starting right of it, matching ours (`list_rows_art` = 4, `art_box` 55).
- Title and sublabel positions not measured to sub-pixel (glare). Visual: equivalent.

### Songs (photo 1215)
- 5 two-line rows of 44 px, matching ours.
- Scrollbar: 7G thumb 3 px wide at **x 315–317**, grey ≈#7B7B7B relative, rounded, no track. Ours x 316–318. **Δ −1 px.** Conf. med.

### Quick-scroll overlay (photo 1216/1217)
- See `out/fidelity/quickscroll.png` (visual only; automatic box extraction failed because the black text merges with the box).
- The 7G square is a flat near-black with rounded corners, centred, with a white letter.
- Ours has a **grey-to-black vertical gradient** body; by eye it is similar in size or slightly larger. Owner: `qscroll.c` box colour (make it flat ≈#101010) and size, to be measured from capture request 5. Conf. low.

### Settings (photo 1166–1185): `screens/settings.c`, `screens/panels.c`
| Element | 7G | Ours | Δ | Conf. |
|---|---|---|---|---|
| Rows | 25/24 pattern | 24.44 rounded | fix #4 | high |
| Inline value right edge | ≈153.5 | 155 | −1.5 | med |
| Value colour | blue, same family as the bar | #1F6CD6 | ≈ok | low |
| Scrollbar | present, thumb x 148–150, bar shortened | absent | missing | med |
| Panel caption, last line bottom | ≈217 (s07, s12, s02) | 217 | 0 | med |
| Caption line pitch | ≈14 (3-line Clicker: 176–189 / 192–201 / 205–217) | 18 (2-line: 187–199 / 205–217) | **−4** | med |
| Tile + reflection band | y ≈77–162 | glyph 96–140 inside the tile at 80–156 | tile ≈3 px higher, larger glyph | low |
| Panel title | centred, top ≈18–20 (glare) | 18–33 | ≈0 | low |

The caption pitch is the clearest panel delta: 7G multi-line captions are tight (≈14 px pitch, small regular face), ours ≈18. Owner: `panels.c` caption line spacing (`CAPTION_BASE`, line height); the font is `font_small`.

### Backlight leaf (photo 1186)
Same split layout. Rows follow the 25/24 pattern (selected "10 Seconds" bar at 95–119 = row 3 of the 25-px group). Ours draws 24.44 rows (fix #4). The panel keeps the tile and value as ours does.

### About (photo 1167–1169)
Not built (blank page, fix #3). Target layout from about-2/about-3: dark header with the device name centred; "19.9 GB Used" left and "128 GB Free" right at y ≈75; logo centred above; two label/value columns from y ≈130 at 12 px pitch; page dots at y ≈225.

### USB (photo 1165, photo 1226)
Not built; the sim shows the stock screen with the Rockbox logo (fix #2). Target from usb-b: dark header with the clock centred; orange disc ≈80 px diameter centred near (160, 108); "Connected" at y ≈190, second line y ≈205.

### Now Playing (photo 1203, 1208, 1222; Phase 5 shots)
| Element | 7G | Ours | Δ | Conf. |
|---|---|---|---|---|
| Title line (cap top / bottom incl. descenders) | 70–71 / 82–86 | 59 / 71 | **+11…+12** | high |
| Artist | 90–91 / 103 | 81 / 90 | **+10** | high |
| Album / chapter | 108–109 / 118–119 | 98 / 107 | **+11** | high |
| Counter "n of N" | 145–146 / 154–155 | 133 / 142 | **+13** | high |
| Title → artist → album pitch | 20 / 18 | 22 / 17 | −2 / +1 | med |
| Album → counter (rating row between) | 37 | 35 | +2 | med |
| Text x | 159–160 | 160–162 | ≈0 | high |
| Bar row (labels) y | 210–221 | 209–220 | +1 | med |
| Elapsed label x / remaining end | ≈x 23 / ≈302 | 23 / 301 | ≈0 | med |
| Shuffle shelf | lower-contrast sunken channel, selected cell lighter with blue text | raised boxes with dark outlines | material (spec item 5 already says "inverted") | med |
| Volume bar | track ≈x 60–272, thinner, gel fill | fill from x 44, track to x 298 | ours ≈16 px longer at each end, thicker | low |

The **text-block offset is the single largest layout delta in the audit**. The Apple-guide numbers in `MEASUREMENTS.md` / `now-playing.md` (title top 60, artist 81, album 98, counter 133) do not match the owner's 2.0.5 device and should be replaced by **71 / 91 / 109 / 146** *(±1)*.

### Audiobook Now Playing (photo 1221)
Same text offsets as music (+11…+13). Chapter ticks span the full bar height on the 7G (ours too). The bar spans x ≈60–270.

## Spec numbers to update (they came from rougher estimates)
- `now-playing.md` / `MEASUREMENTS.md`: NP text tops 60/81/98/133 → **71/91/109/146**.
- `index.md` "List geometry" / `music-lists.md`: "≈24.4 px pitch" → **rows 0–3: 25 px, rows 4–8: 24 px** (row tops 20, 45, 70, 95, 120, 144, 168, 192, 216; baseline 6.75 px above the row bottom). "25 px row pitch" in `music-lists.md` and `settings.md` is stale too.
- `music-lists.md`: chevron "≈6×11, 6 px from the right edge" → **≈8×13, 3 px stroke, right ink ≈7 px from the edge**. Text inset x = 7 → **6**.
- `header.md`: battery "24×12 body + 2×4 nub, 3 px from the right edge" is **confirmed** (ours is 20×10, so the code drifted from the spec). Play glyph 12×12 → **11×12**; gap 5 → **7 px**.
- `settings.md`: scrollbar "x ≈155" → **thumb x ≈148–150, 3 px**, and the selection bar stops before it. Caption "y ≈210" → **bottom ≈217, line pitch ≈14**.
- `music-lists.md` scrollbar "≈3–4 px": **3 px, x 315–317** full width.

## Capture requests (new or better owner photos)
1. **Straight-on Now Playing with no glare, whole screen in frame**, in Normal, Rating (unrated and 4 stars) and Volume modes. The current np-unrated, np-4stars, np-volume and np-scrub frames are cropped at the left edge. Phone square to the screen, about 25 cm away, in a dim room with the backlight at full, filling about 70 % of the frame.
2. **About page 1** with the whole screen in frame (about-1 is clipped at both sides).
3. **Settings list, unselected rows only, scrolled to the middle** (e.g. Volume Limit selected), no glare. This confirms the scrollbar thumb geometry and the value right edge in one frame.
4. **Header close-up** (the top 25 px of the screen at 2–3× zoom) with Hold on and something playing, to pin the lock, play and battery shapes and their gel shading.
5. **Quick-scroll overlay at rest** (letter fully shown) on a white list area, straight on.
6. **Settings panel close-up** (the right half) for Shuffle and Clicker, to measure tile size, reflection height and caption metrics without the top-panel glare.
7. Any capture of the **selection bar on a neutral grey card** in daylight: needed for colour, since every current photo has a violet cast.

## Notes
- The owner photos and everything derived from them stay local (`out/fidelity/`, `docs/reference/7g/owner/` is gitignored for images). This document holds only numbers.
- Reproduce: `python3 out/fidelity/tools/batch.py [names]` (rectify), `rows4.py`, `hdr.py`, `zoom.py`, `grid.py`. Ours: `SIM_BUILD=build/sim-ui-audit scripts/shots.sh out/fidelity/scripts/ipodui-fid-{a,b,c}.script out/fidelity/ours/{a,b,c}`.
