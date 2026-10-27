# Main menu

*Split from `7g-spec.md` "Global chrome" and "Menu trees" (2026-10-07). Cross-cutting rules (navigation model, transitions) are in [index.md](index.md).*

## Split screen

- **Main menu split screen:** the list takes **exactly the left half (x 0–159)** with the header above it. The art panel (x 160–319) runs the **full height, y 0–239**, with no divider line, and shows a slow pan-and-zoom of album art that changes per item. *(For Settings the reference shows the device's capacity panel, i.e. the **device name** at the top, a logo, a capacity capsule bar and "128 GB Free", not a gear (photo `settings-01-about.jpg`). Use our own glyph and the free-space bar. Music and its sub-menus show covers. Podcasts: podcast icon + "25 Podcasts"; Videos: film-strip icon + "No Videos"; Extras: world map + 3D analog clock + date "Oct 6 2026".)*
  - The panel updates only **after the selection rests**: items passed quickly (≈250 ms per step) are skipped. The delay is **≈0.4 s** for glyph panels and up to **≈1.1 s** when a cover must load (✅ owner video `VIDEO-2.md` §C, 9.4→10.5 s, 59.9→60.3 s, 63.8→64.2 s). How the new panel arrives varies on the device: the Music art slid in from the right (≈125 ms), the Alarms glyph faded in while the old art slid out right, and Extras panels cross-faded. **We use a ≈200 ms cross-fade for all highlight updates** *(≈, simplest faithful choice; revisit if the owner prefers the slide)*.
  - **Panel transitions (owner, 2026-10-20; supersedes the all-cross-fade rule above):** when the highlighted row's panel changes inside a split screen (the list stays):
    1. **Slideshow → fixed panel** (a drift — Music, the Music menu, Shuffle Songs, the Audiobooks rows' books, the Videos posters — to a tile, capacity, calendar, Extras, Podcasts or the still Now Playing cover): the slideshow **slides out to the right**, uncovering the fixed panel in place (drawn underneath first).
    2. **Fixed → slideshow:** the slideshow **slides back in from the right** over the fixed panel, which stays put.
    3. **Slideshow → slideshow of another image group** (Music ↔ Audiobooks): a **cross-fade** to the target's current cover (kept in the second slot when present), then that drift continues.
    4. **Fixed → fixed:** the cross-fade, unchanged. Rows of the **same** slideshow: no transition, the drift just continues.
    - ≈200 ms (`UI_PANEL_SLIDE_TICKS`), quadratic ease-out; the moving image carries its split shadow at its left edge (as the parting's panel does), the panel underneath keeps its own at the seam. A fast spin retargets to the newest row, as the cross-fade does. `UI_PANEL_SLIDE 0` (preview.c) restores the cross-fade everywhere. *(Was "≈0.5 s, switches in place", from `photo 1190.mov`.)*
  - **Art drift (Music and the other art panels; ✅ owner video `VIDEO-2.md` §C, 10.5–31.4 s):**
    - Each cover **dwells ≈7 s** (6.8 and 7.3 s measured), then **cross-fades (≈400 ms)** to the next cover. The next cover starts at its own new framing.
    - During the dwell the cover **pans linearly at ≈5–8 px/s**, in a direction that changes per cover (seen: left; right+down; up), with **at most ≈5 % zoom** over the dwell. There is no ease at the ends.
    - The art is drawn ≈240–300 px square (the 160×240 panel shows ≈⅔ of its width) and must be taller than 240 to allow the ≈30 px vertical pans.
    - **The cycle is not reset** by a push (Main → Music keeps the same cover mid-dwell, and the next change comes on schedule), by entering and leaving Cover Flow, or by moving between art items.
  - Items added through Settings › Main Menu go into **fixed canonical slots**, not at the end, e.g. Music, Audiobooks, Videos, Photos, Podcasts, Extras, Alarms, Settings, Shuffle Songs (photo `main-menu-custom-hold.jpg`).

## Menu trees

- **Main:** Music, Videos\*, Photos\*, Podcasts, Extras, Settings, Shuffle Songs, Now Playing (only while something is playing).
  - \* Video and photo playback is not planned. These items are hidden unless that changes. *Photo shows* stock 2.0.5 keeps Videos and Photos in the main menu even with nothing synced (Videos panel "No Videos", video frame `mov-main-panel-videos.jpg`); owner to decide.
  - Items added via Settings › Main Menu take fixed slots (Audiobooks after Music, Alarms after Extras).

- **Split-screen depth (owner, 2026-10-09):** the left list panel casts a **soft shadow onto the right art panel**, as if the list were a window floating above a backdrop. Draw a soft vertical shadow a few px wide, falling from the list's right edge onto the panel. Pre-render it as a per-column alpha table (lean rules 1 and 2) and apply it on every panel draw, including drift, cross-fades and the Settings tiles. Measure its width and strength from owner photos at polish time. **Set (owner, 2026-10-20, after the 8b-3 shots): 10 px, 36 % black at the seam, quadratic falloff** (`art/materials.py`; was 6 px at 30 %).
- **Drift cover order (owner, 2026-10-09):** never show the same cover twice in a row, including when the cycle wraps or the order is reshuffled. With only one cover, hold it rather than fading it into itself.
