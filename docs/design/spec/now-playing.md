*Split from `7g-spec.md` (2026-10-07). Cross-cutting rules are in [index.md](index.md).*

## Now Playing
- **Layout:** album art on the left, drawn with a **3D perspective slant** that turns it toward the text, not flat. It spans x 9–145. The left edge is y 33–167 (135 px) and the right edge y 44–162 (119 px) (P11, measured on the rectified owner references np-clean-ufo, np-clean-battlecry and photo 1203; `np-reflection.md` §1). A glossy-floor reflection fades gradually below it and runs on **behind the elapsed time and the bar row**, to nothing by y 219; the times, the bars and the mode rows draw on top of it.
  - Track info starts at **x = 160**: title (black bold, cap 11 ≈ 15–16 px, top y 60), artist (top y 81), album (top y 98), rating stars (y 116–125), "n of N" (top y 133).
  - The rating row is **blank when the track is unrated**; once rated, the stars are small and **near-black**, not gray (photos `np-normal-b.jpg`, `np-rating-4stars-b.jpg`).
  - The title marquee-scrolls, clipped at the text block's left edge with no fade, looping with a wide gap; long artist/album lines end in "...".
  - With no artwork, draw our placeholder: a dark slate-blue square with a large light-gray note glyph, slanted and reflected like real art (photo `np-audiobook-noart-chapterticks-b.jpg`).
  - Artist and album are **bold** dark gray ≈ #3C3C3C, cap 9–10 ≈ 13 px. *(Our "Regular 12 for secondary text" does not match Now Playing; use Bold 13.)*
  - Shuffle and repeat state glyphs sit under the header at the right (x 267–312, y 23–37).
- **Progress bar:** x 58–261, y 206–219 (14 px incl. 1 px border). Glossy blue fill, light gray empty part, faint reflection below.
  - Elapsed time is at the left (x 23, bold, 9 px digits) and remaining "−m:ss" at the right (ends at x ≈ 302), vertically centred on the bar (y 210–218).
- **SELECT cycle**, as described by the owner (first-hand, 2026-10-06). This is authoritative and overrides the earlier assumptions; there are no clean reference captures of these modes.
  1. **Normal:** the blue progress bar, with elapsed time on the left and remaining time (−) on the right. **The wheel controls volume.**
  2. **SELECT → Scrubber:** the blue fill **disappears**, and a **blue gradient diamond marker** sits at the current position on the empty track. The wheel moves the marker left and right, which seeks. **Playback jumps once, after the wheel stops**, not live while turning (owner, 2026-10-07). A track change in any SELECT mode drops back to the normal bar (owner, 2026-10-07). The time labels stay in place: current position on the left, remaining time on the right.
  3. **SELECT → Rating:** the scrubber row **slides off-screen to the left**. **Five grey dots**, centered on the screen, mean unrated. *(Photo shows small **black** dots, `np-rating-unrated-b.jpg`; the owner's "grey" is kept pending confirmation.)* Scrolling **right (clockwise)** turns dots into **blue gradient stars** one at a time, and scrolling **left (counter-clockwise)** turns them back into dots. Confirmed by the owner.
  4. **Music only: SELECT → Shuffle:** the rating row slides off to the left and the shuffle control appears; the wheel changes the shuffle setting. Options are **Off / Songs / Albums** (owner confirmed). **Look** (owner): a **shuffle icon** on the left, then the three options side by side on what looks like a **shelf**, with a **square, button-like box** around the selected one. The wheel moves the box. *(Our drawing is original; exact metrics to be tuned by eye.)* *Photo shows* (`np-shuffle-b.jpg`) the shelf light gray and rounded, the shuffle icon dark gray, the selected option's box raised and lighter with its text **blue**, the others black bold; consistent with the owner's description.
  5. **SELECT → Lyrics** (only when the track has lyrics, from the embedded USLT/©lyr tag): the lyrics are shown full-screen over the art and text, and the wheel scrolls them. *(Stock 2.0.5 step, added per the owner, 2026-10-06.)*
  6. **SELECT → back to Normal:** the current row **slides off to the left** and the normal progress bar returns, with the wheel back on volume.
  - **Order decision (owner):** Scrubber → Rating → Shuffle (music only) → Lyrics (if present) → Normal. The 2012 guide lists Shuffle before Rating, but the owner's memory and the 2008 guide put Rating first, and **the owner's order wins**.
  - **Audiobooks skip the Shuffle step** (owner, 2026-10-06), so their cycle is Normal → Scrubber → Rating → Normal. Audiobooks **do** show Rating (owner confirmed). Shuffle and repeat state glyphs also show under the header.
  - **Every mode drops back to Normal after a few seconds without input** (owner, 2026-10-06). The owner puts it at about **3–5 s**, so we use **4 s** as a single theme constant (`np_mode_timeout`).
- **Wheel:** changes volume. The volume bar **replaces the whole bar row**: the time labels go, a small speaker (no waves) sits at the left and a speaker with waves at the right, with a glossy blue fill, light-gray empty part and faint reflection; the bar is longer than the progress bar, ≈x 40–280 *(≈, photos `np-volume-b.jpg`, `np-volume-2-b.jpg`)*.
- **Buttons:** prev and next short-press skip; a long press seeks; for audiobooks, prev and next move by **chapter**.
- **Audiobooks (owner's photos):** the album line shows the **chapter title**, the counter reads like **"73 of 648"**, which on the stock 7G is the queue position (file 73 of the 648 files in the Audiobooks list). **Deliberate deviation (owner, 2026-10-07): ours shows the chapter, "Chapter N of M"-style as "N of M", for a chaptered book. Starting a book queues only that book, not the whole Audiobooks list** (see the Phase 5 brief, Revision 2), and the progress bar spans the **whole book** (e.g. "-11:47:55"), with **chapter tick marks**. There is no "Chapter n of N" line.
  - **Chapter artwork (owner, 2026-10-06):** when a book has per-chapter images, Now Playing shows the **current chapter's image**, switching at each chapter boundary; otherwise the book cover. The "Resume" plus chapters list shows **titles only**, with no chapter thumbnails. (#39)
  - **Scrubbing (owner, 2026-10-06):** a slow wheel moves normally. A **fast** wheel **snaps between chapter checkpoints** (chapter starts), so a long book can be crossed quickly.

## Hold-SELECT menu

- **Hold-SELECT menu:** a **full-screen ordinary list** under the normal header (not an overlay): **Add to On-The-Go, Browse Album, Browse Artist, Cancel** from Now Playing (Start Genius first only with Genius data, which we skip) (photo `hold-select-popup-np-b.jpg`).
- **Art reflection (owner, 2026-10-08, after the Phase 5 screenshots):** make the reflection **a little bigger**, and have it **fade out further down**, more gradually. Ours (~25 px, from ~35% to 0, cut at y 196) feels too sharp. Aim for a longer, softer falloff, e.g. starting ~30% and reaching 0 over ~40–45 px, possibly with a slight blur. Keep it clear of the bar row, or let it fade to nothing just above it. Applies to the placeholder and to non-square art too. It is computed once per art change, so the cost is negligible.
- **Reflection geometry (owner, 2026-10-08):** the reflection must obey floor-mirror optics. The art stands on a glossy floor, so **each column mirrors about its own bottom (contact) point**: for column x with bottom edge y_b(x), the pixel at y_b(x)+d shows the art pixel at y_b(x)−d (faded with d).
  - The reflection's top edge therefore coincides with the art's slanted bottom edge, with no gap or overlap.
  - Its outline follows the same perspective as the art. The far (right) side is shorter, so the reflection's lower edge rises toward the right, mirroring the art's top edge in the opposite direction, toward the same vanishing point.
  - Not a vertical flip of the whole image about one horizontal line.
  - Applies to the placeholder and to fitted non-square art (mirror about the fitted art's own bottom edge).
- **Exact reflection spec:** `docs/design/np-reflection.md`. It covers the geometry, the exponential perspective fade, the limit (y ≤ 219, behind the bar row, P11) with feathering, the slide-band background and tests, and supersedes the two bullets above where they differ.
- **Shiny metallic bars and stars (owner, 2026-10-08, after the Phase 5 screenshots):** the gradients are a good start, but the originals look more **3D and shiny**:
  - **Bars** (progress, scrubber track, volume): a rounded **glossy capsule**:
    - a bright specular highlight band across the upper third;
    - a darker lower half with a subtle bottom rim light;
    - a defined 1 px darker outline;
    - an inner shadow on the empty track;
    - a glossy blue fill with its own highlight.
    - Both the fill and the empty track read as polished metal or plastic.
  - **Rating stars:** shiny 3D metallic blue: a lighter top-left facet, a darker lower-right facet, a specular glint, and a thin darker edge, like the glossy stars in photo 1206. The small dots and the text-block stars stay plain.
  - Precompute the per-row colour tables and star tiles once (the lean rules), so there is no runtime cost beyond today's blits.

## Bar row materials: detailed from the owner photos (2026-10-08, zoomed crops of photo 1206, 1208, 1209). Authoritative.
Lighting convention for every pre-drawn object: **light from the top**. Anything raised has a light top edge and a dark bottom edge, and casts a soft shadow downward. Anything sunken has a dark top inner edge and a light bottom inner edge.

**1. Flat reflections under the bar-row objects (head-on, not slanted).**
- The progress bar (fill and track), the scrubber track, the volume bar and the rating stars each show a **gentle mirror reflection directly below them**: the object's rows mirrored about its own bottom edge, about 4–6 px tall, starting at ~25–30% opacity and fading to 0.
- Under the blue fill the reflection reads as a **pale cyan strip** (clearly visible in photo 1209). Under the empty grey track it is very faint.
- **No reflection** under the speaker glyphs, the time labels or the shuffle icon.
- Same maths as `docs/design/np-reflection.md`, but with no perspective: s = 1, and α(d) = A0·exp(−K·d/h_obj) with h_obj the object's height, A0 ≈ 0.28, a 2-row feather.

**2. The bar fill (blue): glossy, gel-like and 3D** (photo 1209 zoom):
- top ~30%: medium blue with a faint lighter top line (the top highlight);
- middle: the deepest royal blue;
- bottom ~30%: **brightening toward cyan**, brightest just above the bottom edge (a glow from light passing through), then a 1 px slightly darker bottom edge;
- the left and right ends are slightly rounded or darkened;
- the result reads as a glossy gel or polished bar, not a flat gradient.

**3. The bar track (empty part): a recessed groove**, not a raised strip:
- a 1 px darker top line (the inner shadow);
- a light grey body, very slightly darker at the top;
- a 1 px darker bottom line;
- ends square.

**4. Rating stars (corrected 2026-10-08 after comparing with photo 1206):**
- **Shape:** classic, **fat, regular five-point stars** with broad arms and a large body. Not thin or pointy.
- **Size and spacing:** ~13–14 px wide, set close together (a gap of ~4 px between stars, pitch ~18 px), and centred in the bar row.
- **Colour:** a **bright azure blue**, with only a **subtle, smooth gel gradient** (slightly lighter in the upper half, slightly richer lower down).
- **Edges:** soft, anti-aliased. **No dark outline, no faceting, no heavy shading.** At a glance they read as clean, bright blue stars with a gentle gloss.
- A faint flat reflection below, at the same low opacity as the bars.
- The empty slots are a plain small near-black dot (~3 px).
- Our first attempt (small, navy outline, busy facets) looked like embossed badges. That is wrong.

**5. The shuffle shelf: OUR CURRENT VERSION IS INVERTED.** The correct model (photo 1208):
- **The shelf is a sunken channel:** a rounded rectangle (radius ~4) recessed into the panel, with a light-grey fill slightly darker than the background, a **dark top inner edge** (1–2 px soft inner shadow) and a **light bottom inner edge** (1 px highlight). The unselected options ("Songs", "Albums") sit **inside the channel**, flat, in near-black bold text, with no borders between them.
- **The selected option is a raised metallic button inside the channel:**
  - a rounded rect (radius ~3) the size of its cell;
  - a body gradient from a lighter top to a slightly darker bottom (a satin metal or plastic look);
  - a 1 px light top edge;
  - a 1 px darker bottom and right edge;
  - a **soft drop shadow** (1–2 px, ~20–30% black) falling below it onto the channel floor (visible as the darker smudge under "Off" in photo 1208);
  - **blue** label text.
  - No dark outline box: today's dark border around the selection is what makes ours read as "popped in".
- When the selection moves, the button moves across the channel; the channel itself never changes. The shelf's slide-in animation is unchanged.

All of the above is pre-drawn once (row colour tables, a star tile, the shelf channel and button tiles) per the lean rules, using the shared materials in the art generator (light from the top, the same gloss and shadow constants as the Settings tiles), so the whole UI stays consistent.
- **Lyrics page layout (owner, 2026-10-08; reference `lyrics-screenshot.png`, a web photo of a 5.5G-era iPod showing lyrics):**
  - **The song title, centred, at the top of the body** directly under the header (ours: Bold 12, the header-title style).
  - Below it, the **lyrics, left-aligned**, in Regular 12 with tight line spacing (~17–18 px pitch, about 9 lines visible), wrapped at the right margin.
  - A **scrollbar on the right edge**, in **our standard UI scrollbar style** (the same thumb and track as the lists; the fidelity audit measures the 7G thumb at 3 px wide), not the photo's older 5.5G style.
  - The header stays our normal 7G header (title or clock, play glyph, battery).
  - The wheel scrolls line by line (as built). The scrollbar thumb is sized and positioned by the visible lines over the total.
- **Shuffle glyph crossing (owner, 2026-10-08; photo 1208):** where the two shuffle arrows cross, one arrow has a **small split (a gap) on either side of the other**, so one visibly passes over the other, like a road overpass. Use this in **every crossed-arrows shuffle glyph**: the Now Playing shelf icon, the small shuffle indicator under the header, and the Settings → Shuffle tile when on (Songs/Albums). Rounded ends, per the rounded-glyph direction.
- **Volume speaker glyphs (owner, 2026-10-08; zoomed photo 1209):** both speakers are a small square **magnet body** plus a **flared cone** opening to the right, with a **small separate dot centred just in front of the cone's mouth** (the "little centre bit"). The left (quiet) speaker is exactly that, with no waves. The right (loud) speaker adds **two concentric sound arcs** beyond the dot. Dark grey, softly anti-aliased and rounded; slightly larger than ours. Ours is currently a flat wedge with no dot. **Refinement (owner, 2026-10-08):** the centre dot is the speaker's **dust cap**. The cone's **front rim (the speaker edge) peeks out above and below the dust cap**, as a thin edge, taller than the dot, curving slightly. So the glyph reads as a real speaker: body, cone, rim, then the dust cap in front.
- **Remaining-time minus sign (owner, 2026-10-08):** in the original there is a **very slightly larger gap** between the "−" and the digits of the remaining time (e.g. "− 4:06" spacing is a touch wider than ours). Add ~1 px after the minus in the digit atlas or layout, and measure it against the straightened photos (fidelity audit).
- **When the queue ends (owner, 2026-10-08, corrected):** when a playlist, album or book finishes with Repeat off (or a lone book stops at its end), Now Playing **pops back to the previous screen** (which may be the main menu), as in the brief's default 9.15. The main menu's **"Now Playing" row disappears** once nothing is playing or paused, and must refresh as soon as the main menu is shown again.
- **Auto-return to Now Playing on idle (owner, 2026-10-08):** while music is playing (or paused, ⚠️ default: playing only), if the user leaves the wheel idle in any menu or list for a while, Now Playing **appears automatically**, pushed on top of the current screen, so MENU goes back to where they were.
  - It happens **a little before** the backlight timeout and the unlit clock face.
  - ⚠️ Default delay: `max(5 s, backlight timeout − 3 s)`, and 8 s when the backlight is "Always On". The owner didn't know the exact stock figure; check it on the 7G.
  - Not on Now Playing itself, the USB screen, a game, or a text or value editor in progress.
  - Implemented from `ui_nowplaying_idle()` (NAV-H2), which already runs every main-loop pass: no new timer source.
  - **Applies to everything that plays** (owner, 2026-10-08): music, audiobooks and podcasts alike.
- **Layering (owner, 2026-10-08; photo 1206):** the album-art reflection lies **behind** the bar row. The rating stars (and the bars, shelf and speakers) are drawn **on top of** the reflection where they overlap; the reflection shows around them. Implemented through the saved art+reflection band background (`np-reflection.md` §6) for every mode and slide frame.
- **Chapter counter vs title (owner, 2026-10-08):** the "N of M" counter is the **position of the current chapter entry** in the file's chapter list. It is never parsed from the title. Books often have a Preface, an Introduction or Opening Credits before "Chapter 1", so "Chapter 1" may legitimately be entry 2 or 3. The only rule: the title and the counter must come from the **same entry**, updated together.
- **Speaker glyphs: exact direction** (orchestrator, from a 16× comparison of the rectified photo 1222 against ours, 2026-10-08; supersedes the earlier speaker bullets where they differ). Device pixels at 1×; build from a 4× supersampled vector, then downsample, so the edges are naturally anti-aliased; near-black (#1c1c1c) on white.
  - **Quiet (left) speaker, ~10 w × 11 h:**
    - **Body:** a rectangle 3 w × 5 h, vertically centred.
    - **Cone:** from the body's right edge (5 h), flaring with straight diagonal edges to a **mouth 11 h** over 4 px.
    - **The mouth is part of the cone silhouette:** its tips (the top and bottom 3 rows of the mouth) extend **one more column to the right**. That is the "rim peeking around" the dust cap.
    - **Gap:** between those tips, a **1 px light slit** (background-coloured, ~5 h, centred) separates the cone from the dust cap.
    - **Dust cap:** a **2 w × 3 h rounded nub, immediately right of the slit**, vertically centred. It is NOT a separate dot floating 2–3 px away, and there is NO separate grey rim block.
  - **Loud (right) speaker:** the identical speaker, then **two thin circular arcs centred on the dust cap**, radii ≈ 5 px and ≈ 8 px, each spanning about ±50°. The stroke is ~1 px with anti-aliasing (≈1.5 px visually), the same near-black, and there's a clear ~2 px gap between the arcs. The outer arc's height is about the cone mouth's height or slightly more (≈13 px). The arcs are **light and elegant**, not chunky bands with a doubled outline.
  - **Placement:** the quiet speaker sits ~4 px left of the bar's left end, and the loud speaker ~4 px right of its right end, both vertically centred on the bar.

- **SELECT after a change (owner, 2026-10-09, device):** on any SELECT-cycle page (Scrubber, Rating, Shuffle, ...), if something was changed on that page (scrubbed, rated, toggled), SELECT returns to the Normal view. Only an untouched page advances to the next mode.
