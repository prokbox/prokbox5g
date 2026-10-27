# iPod Classic 7G behavior & look spec

The target is iPod Classic 7G software (firmware 2.0.x) on a 320×240 screen. Values marked *(≈)* are still estimates. Measured values come from Apple's user-guide screen captures; see `docs/reference/7g/MEASUREMENTS.md` (method and confidence) and `SOURCES.md`. The owner's photos and video of their own 7G on **2.0.5** (`docs/reference/7g/owner/INDEX.md`, cited as "photo `<label>`") settle wording, order and behaviour; metrics taken from them are angled-photo estimates and stay marked *(≈)*.


*Split from the single `7g-spec.md` on 2026-10-07 (process-audit-2 item 7) so agents load only what a task needs. This file holds the cross-cutting rules that apply everywhere; per-screen detail lives in [header.md](header.md), [main-menu.md](main-menu.md), [music-lists.md](music-lists.md), [now-playing.md](now-playing.md), [audiobooks.md](audiobooks.md), [settings.md](settings.md), [extras.md](extras.md), [cover-flow.md](cover-flow.md) and [system-screens.md](system-screens.md).*

## Visual language: glossy, rounded 3D (applies everywhere)
- **Era look (owner, 2026-10-07):** "The era had a lot of gradient hues and metallic, 3D-looking images." Art should use rich gradients and metallic sheen, not flat shapes.
- **The "blue gel" material (owner, 2026-10-08; confirmed in zoomed photos 1171, 1203, 1209):** the common theme of **every blue element**, defined once in the shared art materials:
  - a faint lighter line at the top edge;
  - medium blue in the upper part, deepening to the richest blue around the middle;
  - **brightening toward cyan in the lower third**, brightest just above the bottom edge (light glowing through the gel);
  - a 1 px slightly darker bottom edge.
  - Applies to: the **list selection bar**, the **header play/pause glyph**, the progress, scrubber and volume **fills**, the **rating stars**, and the scrubber diamond.
  - Green elements (the battery fill) follow the same gel profile in green.
  - Lighting convention everywhere: light from the top (raised = light top edge, dark bottom edge, shadow below; sunken = the reverse). See `now-playing.md` → "Bar row materials".
  - Now Playing's bars and stars adopt it in Phase 5; the selection bar, the header glyphs and the battery move to it in the polish pass (#52).
From the owner's first-hand knowledge (2026-10-06): most UI parts have a **simulated rounded 3D effect**, a **shinier, lighter top half** over the base colour, which reads as a glossy rounded surface. Apply it **universally** unless a reference shows otherwise:
- the selection bar: **the B gel** (owner chose look B, "midway", 2026-10-20; `UI_MAT_SEL_VARIANT` 2, with A and C kept selectable by the define). On a 24-row bar: top line (139,205,242) (A (156,223,255), C (123,190,239)), row 11 (31,129,213) (A (33,142,222), C (24,113,206)), row 22 (32,129,213) (A (49,174,239), C (8,85,189)), foot edge (18,83,154); B is A and C averaged key for key (`sel_variant()` in `art/materials.py`). The same gel is the header play/pause glyph and, as its green counterpart, the battery fill. Measured from the owner's photos (photo 1212, 1186), the 7G list bar is a deep saturated blue (hue ~207) that darkens steadily to a darker bottom edge, **with no cyan lift** toward the bottom; B sits midway between today's lighter look (A) and that measurement (C);
- the progress bar fill, the scrubber diamond, the volume bar;
- rating stars, the Shuffle "shelf" and its selected box;
- the battery and the play/pause glyphs (already done in Phase 3);
- scrollbar thumbs.

**Recipe** (one shared helper, `ui_paint_gloss()`): a vertical gradient of the base colour, with the top ~45–50% blended toward white (strongest at the top edge) and a soft step at the midline. An optional 1 px darker edge at the bottom gives depth. Colours come from small per-surface descriptors in `theme.c` (`struct ui_gloss`, per-row colours computed), so tuning one descriptor changes every surface that uses it. In use: selection bar (`sel_gloss`), scrollbar thumb (`sbar_gloss`, horizontal).

**Material kit (Phase 8b-2):** the gels, groove, grey gloss and metal are defined once in `art/materials.py` (key colours down the object), generated into `apps/ipodui/material_data.h` and read through `apps/ipodui/material.h` (`ui_mat_rows()` per (material, height) table, `ui_mat_color()` for any other height). In use: Now Playing's gel and groove (unchanged), the selection bar (`UI_MAT_GEL_SKY`, the gel lifted toward sky blue), the header play/pause glyph and the battery's green/red gel (pre-rendered tiles, `apps/ipodui/art/header.py`). The style gallery (`art/gallery.py`) shows every table. `ui_paint_gloss()` remains for the surfaces not yet moved.

## Navigation model: split-screen preview + reveal
**Principle (owner, 2026-10-06):** at every menu level **above the bottom layer** the screen is split: the navigational list on the left, and on the right an underlying image, example or **preview of the highlighted item's feature or page** (album-art pan for Music items, icon tile + caption/value for Settings items, the destination page itself for Extras items such as Clocks). At the **bottom (leaf) layer**, SELECT **reveals** the content: the left menu slides away and the preview becomes full-screen (the *reveal* transition, under "Transitions"). Deeper *list* levels (e.g. Artists → albums → songs) are full-width lists.

Where the split stops, as the owner's photos and video show it:

| Area | Split screen (list + preview) | Full-width or full-screen |
|---|---|---|
| Main menu | ✅ main menu (photos, video) | — |
| Music | ✅ Music menu (photo `music-menu-b.jpg`) | ✅ Artists, artist's Albums, Albums, Songs, Search, an opened audiobook (Resume + chapters) are full-width lists; Now Playing full-screen. ✅ Audiobooks book list is full-width (two-line rows with cover thumbnails, 4 per screen) and Cover Flow is full-screen with its own header (video `VIDEO-2.md` 55.6 s, 33.4 s). ⚠️ Playlists, Genres, Composers not photographed (assume full-width). |
| Settings | ✅ Settings list; ✅ Backlight value list **stays split** (panel keeps the Backlight tile and value, photo `backlight-values.jpg`) | ✅ About (dark), Brightness (dark), Audiobooks speed (light prompt) are full-screen. ⚠️ Main Menu / Music Menu checklists, EQ, Volume Limit, Date & Time, Language, Legal, Reset not photographed. |
| Extras | ✅ Extras list; ✅ Calendars, Alarms, Stopwatch sub-lists stay split | ✅ Clocks page full-screen (reveal, owner; measured on video, `VIDEO-2.md` §A3); ✅ Calendars › All Calendars is a full-screen month grid (video 74.7 s); Screen Lock full-screen dark; Games and Notes full-width lists. ⚠️ Contacts not photographed. |
| Podcasts | ⚠️ (main-menu panel only: icon + "25 Podcasts") | ⚠️ shows/episodes not photographed |

*Photo shows* two refinements of the owner's rule, for the owner to confirm: Music's split stops at the Music menu itself (Artists is not a leaf but is already full-width), and some leaf lists stay split (Backlight values; Extras › Alarms/Calendars/Stopwatch). The catalogue (`7g-catalogue.md` §1 "Reveal transition", §6, §7.1) cites the same photos.

- **Other split screens:** the Music menu, Extras (and its Calendars, Alarms, Stopwatch lists) and **Settings** are split screens too; Artists, Albums, Songs, Games, Notes, Search, opened audiobooks and the hold-SELECT menu are full-width.

## Global chrome

- **Transitions (owner, 2026-10-06, authoritative):**
  1. There is **always a slide, never a cut**, between views.
  2. **Into** something (SELECT): the current view moves out to the **left** while the new view slides in from the **right**.
  3. **Back** (MENU): the current view slides out to the **right** while the previous view slides in from the **left**. Backing out of an underlying section can land in that section's **split view** (list + preview).
  4. **Exception, reveal:** when the destination is the preview already shown in the right panel (e.g. Extras › Clocks ✅), SELECT slides the left menu away to the left and the preview **expands** to full screen. MENU reverses it: the menu slides back in from the left and the content **contracts** into the right panel. Settings panels show an icon tile, not the destination, so Settings items use the normal slide; other Extras items ⚠️ unconfirmed (default normal slide). *Photo shows* the Clocks panel (map + large 3D clock + date, `extras-clocks-sel.jpg`) is not pixel-identical to the full Clocks page (blue city band, `clocks.jpg`), so the expand must also morph or crossfade between the two layouts; owner to confirm.
     - *✅ Measured (owner video `VIDEO-2.md` §A3, 67.79–68.04 s; labels `clocks-reveal-01..08`):* the **world map is a fixed shared backdrop**: the panel's map is the right half of the Clocks page's map and never moves. SELECT slides **everything else left** (header, list, the panel's white clock and "Oct 6 2026") while the Clocks page's **blue city band** ("Boston", small dark clock, "22:34 / Today") slides in from the right.
     - Timing: **≈250 ms**; the band near-linear at ≈55–60 px/frame, the outgoing layer faster (≈85–110 px/frame, a slight parallax). Nothing scales or morphs. MENU is the exact mirror (≈250 ms).
     - *Conflict, for the owner:* the owner described the preview "expanding". The device shows a shared-backdrop push. Per the owner's decision "match the real 7G exactly", build the measured version.
  5. **Split (Cover Flow only; ✅ owner video `VIDEO-2.md` §A1/§A2, labels `coverflow-split-*`, `coverflow-exit-*`):**
     - **Entry (SELECT on Music › Cover Flow):** the screen **parts at x = 160** (the list/panel seam). The left half (list column with its header strip) slides **left** and the right half (art panel) slides **right**, rigidly, with no fade. Cover Flow is **already rendered underneath** at its final geometry, with no header.
       - **Duration ≈210 ms**, **ease-in** (accelerating). Per frame the list moves 20/32/81/27 px and the panel 5/34/33/40/48 px; the list leads by ≈1 frame.
       - Build: both halves 160 px in 200 ms, quadratic ease-in, panel delayed one frame *(≈)*.
       - Afterwards the album text appears (+170 ms), then the Cover Flow header (+250 ms, faded in), and real covers cross-fade over the placeholders once loaded (≈1.5 s on the device).
     - **Exit (MENU):** the Cover Flow header and text vanish first (≈1–2 frames). Then the **panel slides in from the right** (linear, ≈65 px/frame, briefly overshooting to x≈116). Then the **list + header slide in from the left with an ease-out** (+32/+49/+61/+10/+8 px) and draw over the panel, so the seam lands at x = 160.
       - **≈290 ms total.** The panel arrives showing a wider view of its current cover and settles to the drift framing as it lands *(uncertain)*.
  6. **Split ↔ full screen, the parting (owner, 2026-10-20: this is the 7G's own behaviour):** every push or pop where one side is a split screen and the other a full screen (Music menu → Albums, Artists, Songs…; main menu → a full screen; Settings → About and its other full sub-screens; the same pops back). Into the full screen, the split screen **parts at the seam** (x = 160) over its full height, nothing of it staying behind: the left column (its header part, x 0–159, and the list) slides out to the **left** as one piece, the right column (the whole panel, y 0–239, with its shadow) out to the **right** (owner, device 2026-10-20). The full screen's body is **uncovered in place**, already drawn underneath and never moving; its header band shows cleared (the body background) until the halves are off frame, then **the full screen's header bar slides down from above the top edge** (its status glyphs with it). Back to the split screen, the mirror: the full screen's header slides **up and out** first (the band clears behind it), then the halves close **in** with their header parts over the full screen (list from the left edge, panel from the right edge) and meet at the seam; the full screen stays put underneath. It goes by geometry, not by push or pop; system screens (Charging) keep the plain slide. Both halves move rigidly, no fade. Durations: the halves keep the measured 250 ms (parting) and 290 ms (closing), the header bar 120 ms, all quadratic ease-out (brief O7). Split ↔ split (the list column slides) and full ↔ full (the body slides) keep the normal slide; the Clocks reveal (item 4) keeps its own motion. Build: `ui_main.c` `UI_SLIDE_PART` (0 brings back the old full-width slide for A/B), `UI_PART_OPEN_TICKS`/`UI_PART_CLOSE_TICKS`/`UI_PART_HDR_TICKS`.
  - **Timing:** a slide takes **≈170–210 ms** (measured on video `photo 1190.mov`, Main → Music; again ≈170–210 ms, **ease-out**, for Main → Extras in `VIDEO-2.md` §D1); we use **190 ms**, ease-out, for slides. **Split → full-screen pushes** (Main → Audiobooks, Calendars → month) take **≈250 ms** and their pops ≈290 ms (§D2–D3); since 2026-10-20 they move as the parting (item 6). *(Superseded reading of the same video: the full page sliding in over the panel first.)*
  - *Video detail (Main → Music, split → split):* the **header bar does not slide** (its title text swaps at the start of the move), and the **right art panel stays put**; only the list area slides.

## Original assets we need to make
These must be our own work, not copies of Apple's:
- Boot glyph (2–3 variants)
- Battery and play glyphs
- Chevron
- Header and highlight gradients
- Default "no artwork" note glyph
- USB screen glyph
- Fonts: the 6G/7G uses **Helvetica Bold**. Apple's Podium Sans was the font of color iPods only up to the September 2007 refresh, i.e. the 5G/5.5G. **Decided 2026-10-06: TeX Gyre Heros**: Bold 15 for lists (cap 11), Bold 12 for the header (cap 9), Regular 12 for secondary text, all at natural Heros proportions on a 73/72 em with letter-spacing to match the reference x-heights and word widths (Rounds 3-4 in `docs/design/fonts.md`). Source Sans 3 Bold is the fallback. See `docs/design/fonts.md`.

## Owner decisions, batch of 2026-10-06 (authoritative)
- **Rating dots (unrated):** follow the photo (`np-rating-2stars-b.jpg`): the darker, near-black dots, not grey.
- **Videos / Photos:** **hidden** in the main menu; we have no video or photo support.
- **Clicker:** stock wording and options **Speaker / Headphones / Both / Off**, mapped: Speaker = haptic tick (piezo line, #23), Headphones = audible click mixed into the audio output, Both = both, Off = none.
- **Genius Mixes:** stock position in the Music menu, **directly under Cover Flow**. Hidden until our own mixes exist (#33).
- **Clocks reveal:** match the real 7G exactly. ✅ Now measured from the owner's video (`VIDEO-2.md` §A3; see Transitions item 4): a fixed shared map with a push of everything else, ≈250 ms.
- **Where the split view stops:** confirmed. Music splits only at the Music menu level (Artists and deeper are full-width), while the leaf lists in Settings and Extras (Backlight, Alarms, …) keep the right panel.

## List geometry (owner photos `artists-list-b`, `songs-list-scrollbar-b`, `albums-list-twoline-b`, 2026-10-06)
- **Lists fill the body (≈220 px under the header) exactly, with whole rows and no leftover strip.** At rest the last visible row sits flush with the bottom edge, and while scrolling the full body moves.
- **Plain lists** (Artists, Genres, Composers, Settings, …): single-line, **9 rows per screen**, ≈24.4 px pitch. **The priority fix:** single-line rows must reach the bottom of the available area. *(Our 25 px pitch with 8 rows plus a 20 px gap is wrong.)*
- **Songs (Music → Songs, and song lists generally):** **two-line rows**, the title (bold) above the artist (smaller, grey; white on the selected row), **5 per screen** (≈44 px), filling the body exactly with the last row flush at the bottom. Confirmed by the owner from `songs-list-marquee-b` (header "Songs"), 2026-10-06. ⚠️ Check an album's own track list separately (it may be single-line, since the artist is implied).
- **Albums** (and artist → albums): **two-line rows with album art** on the left (≈ row height), the album title (bold) above the artist, **4 per screen**, ≈55 px.
- **Podcasts and episodes:** two-line, with art or dots (see #24).
- Measure each pitch exactly from straight-on photos, scaling by the 320 px screen width. If 220 isn't an exact multiple, it's likely the header or body height that differs; measure both. Where photos disagree with `MEASUREMENTS.md` (25 px pitch from the guide's main menu, which only had 8 items), the owner's 2.0.5 device wins.

## Attribution on the device (2026-10-06)
- **Settings → Legal** (a stock 2.0.5 screen) carries the credits. It opens with the notice's heading *"This is not Apple software"* and its first paragraph, then the Rockbox GPL credit, the font licences and the long disclaimer's independence and trademark text (`docs/legal.md` section 2, verbatim). It's the stock-appropriate place, reached deliberately and never seen in normal use, so the native illusion holds.
- **First-boot notice (owner, 2026-10-24; not on the 7G):** once after each new build, before anything else in the shell (nothing plays or shows behind it), a light, headerless page with every line centred: the heading *"This is not Apple software"* in Bold 15, then the notice text of `docs/legal.md` section 2 in Regular 12 (14 px pitch, 5 px paragraph gaps; the block centred, 217 px tall in English), *"Press Select to continue."* in the dim grey. Only Select dismisses it, and not in its first 3 s; the wheel scrolls it only if it overflows; every other button does nothing. Select saves the acknowledgement once (`ipodui notice ack`, the build's hash); a USB session while it is up saves nothing. `apps/ipodui/notice.c`, `screens/legal.c`; script `ipodui-legal-notice`. Settings › Legal and this notice are the only screens that may name Rockbox (CLAUDE.md rule 1).
- **Settings → About** shows the build line and version, "ProkBox5g 0.11" (`LANG_IPODUI_VERSION_STRING`), in the slot where stock shows the software version. Only About and this string say "ProkBox5g"; the device title and headers stay "ProkBox".

## Naming for public builds (owner, 2026-10-06)
- "Cover Flow" is an Apple trademark (USPTO 3297109). Personal builds keep the stock label; **public builds use "Album Flow"**. The label is a single replaceable string. See #43.

## Global button behaviour (owner, 2026-10-07; authoritative)
- **Click wheel:** scrolls lists, with acceleration. In Now Playing it controls volume (or the current mode).
- **SELECT:** acts on **release**. **Hold SELECT** on an item opens the stock hold menu (P11).
- **MENU:** back one level, on release. **Hold MENU jumps straight to the main menu**, with a pop-to-root transition (slide back).
- **PLAY/PAUSE:** play or pause from anywhere. **Hold PLAY = sleep** (off).
- **NEXT / PREV from anywhere, menus included:** a tap skips to the next track, or restarts/goes to the previous track (the 3 s rule), **without leaving the current screen**. **Hold** fast-forwards or rewinds while held. In audiobooks, NEXT/PREV move by chapter (see Now Playing).
- No Rockbox conventions anywhere: no quickscreen, no long-press context button, no "left = back".
