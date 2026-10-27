# iPod classic software 2.0.5: UI catalogue

This catalogue covers the stock UI of the **iPod classic 160 GB (Late 2009, "7G") running software 2.0.5**, the target set in CLAUDE.md. Its job is to let us rebuild menus, wording, order and behaviour as close to stock as possible.
- Wording in "quotes" is verbatim from Apple's guides.
- ⚠️ marks anything not confirmed by a source: it comes from memory, inference, or a source for another model or version.
- ✅ "owner photo `<file>`" marks an item verified on the owner's own iPod classic 160 GB (MC-series model) running **2.0.5** (About page 3 reads "Version 2.0.5 Mac"). Files are in `docs/reference/7g/owner/labeled/` (index: `docs/reference/7g/owner/INDEX.md`). The photos are angled phone shots with a blue colour cast, so colours and pixel positions taken from them are relative (≈). ✅ "owner" alone marks an owner statement (first-hand, 2026-10-06).
- The owner's device has **Time in Title** on, so most photographed headers show the clock instead of (or alternating with) the screen title.
- Owner statements in the spec (`docs/design/spec/`, split from `7g-spec.md` on 2026-10-07) stay authoritative where they disagree with a source; every disagreement is listed in §9.

## 0. Sources and version facts

### Source keys
| Key | Document | Notes |
|---|---|---|
| **G12** | *iPod classic User Guide* (ma1195), 58 pp, © 2012, part no. 019-2403/2012-11. `docs/reference/7g/raw/ma1195_ipod_classic_160gb_user_guide.pdf` | Last edition for the 160 GB 2009 model, so the **2.0.5-era document**. Printed page = PDF page. This is the primary source. |
| **G08** | *iPod classic User Guide* (ma630, 120 GB), 76 pp, 2008-08. `…/7g/raw/ma630_ipod_classic_120gb_en.pdf` | Software 2.0 (Genius launch). |
| **G07** | *iPod classic Features Guide* (2007), 72 pp. `docs/reference/5g/raw/ipod_classic_features_guide_2007.pdf` | Software 1.0.x. Used to tell 1.x changes from 2.x changes. |
| **G5** | *iPod Features Guide (Late 2006)*, 72 pp. `docs/reference/5g/raw/ipod_late2006_features_guide.pdf` | 5G/5.5G baseline (fw 1.2; 1.3 has the same UI ⚠️). |
| **QS09** | *iPod classic Quick Start* (ma1577, 2009-08) | |
| **MM7** | Pogue & Biersdorfer, *iPod: The Missing Manual*, 7th ed. (O'Reilly, late 2008, covers classic 2.0). Section previews via Wayback, e.g. [ch02s09](https://web.archive.org/web/2019/https://www.oreilly.com/library/view/ipod-the-missing/9780596155834/ch02s09.html) | Third-party. Covers nano and classic together, so treat with care. |
| **IMG** | Screen captures and photos in `docs/reference/7g/` (see its `SOURCES.md`). Also the 160 GB guide's embedded screens in `raw/pdf160/`. | |
| **SPEC** | `docs/design/spec/` (owner, first-hand; per-screen files, start at `index.md`) | |
| **OWN** | Owner's photos and video of their own iPod classic 160 GB (Late 2009, MC-series model), software **2.0.5 Mac**, 2026-10-06. `docs/reference/7g/owner/` (`INDEX.md`; labelled copies in `labeled/`, video frames of `photo 1190.mov` in `view/`) | First-hand on the exact target version. Highest-confidence source after explicit owner statements. |

### Version facts (confirmed)
- **2.0.5 is the final iPod classic software.** iPodWiki's firmware table lists the 6G/7G line as 2.0, 2.0.1 (iPod_33), 2.0.2, 2.0.3, 2.0.4 (iPod_35) and **2.0.5 (iPod_38)** ([ipodwiki.com/wiki/Firmware](https://ipodwiki.com/wiki/Firmware)). Wikipedia lists "2.0.5 (6G, 2009)" as the last OS version ([en.wikipedia.org/wiki/IPod_Classic](https://en.wikipedia.org/wiki/IPod_Classic)). Wikipedia calls the 2007–2014 classic one "6th generation"; iPodWiki and this project call the 2009 160 GB model "7th".
- **2.0.3** (≈29 Sep 2009) shipped for the 160 GB 2009 model and brought **Genius Mixes** ([ifun.de](https://www.ifun.de/update-genius-mixe-fur-neuen-classic-5828/)). **2.0.4** (≈28 Dec 2009) brought "various minor bug fixes" ([ifun.de](https://www.ifun.de/update-ipod-classic-software-2-0-4-6031/)).
- **2.0.5** came only on late-production 160 GB units (the "7.5" revision). Its one known change is removing the EU volume cap; there are **no known UI changes vs 2.0.4** ([iPodWiki: iPod classic (7th generation)](https://ipodwiki.com/wiki/IPod_classic_(7th_generation))) ⚠️ (community source; no Apple release notes found). So 2.0.4/2.0.5 UI = G12. (Moot for most items now: the OWN photos are of 2.0.5 itself.)
- Genius (2.0, Sep 2008) and Genius Mixes (2009) per Wikipedia. The 2009 model also added LBA48 support, which is not a UI change.

---

## 1. Global controls and chrome

| Item | 2.0.5 behaviour | Source | Our device |
|---|---|---|---|
| Turn on / off | "Press any button." / "Press and hold Play/Pause" | G12 p.6 | ✅ have |
| Backlight on | "Press any button or use the Click Wheel." | G12 p.6 | ✅ |
| Hold | "Slide the Hold switch to HOLD (an orange bar appears)." Playback continues. A Lock icon shows in the header. ✅ owner photo `main-custom-hold.jpg`: a gray padlock just left of the battery. | G12 p.6–8, OWN | ✅. The lock glyph is in SPEC. |
| Reset | Hold on/off, then Menu+Center ≈6 s "until the Apple logo appears" | G12 p.6, p.48 | ✅ (hardware) |
| Back / to main menu | Menu = back. "Press and hold Menu" = main menu. | G12 p.6 | ✅ to build |
| Additional options | "Press and hold the Center button until a menu appears." "If a menu doesn't appear, no additional commands are available." | G12 p.6, p.26 | ✅ to build (§5.4) |
| Header | Menu title, Lock icon, Play/Pause icon, Battery icon. "An arrow next to a menu item indicates that it leads to another menu or screen." ✅ owner photos: the chevron is drawn **only on the selected row, and only when that row leads somewhere**. Selected leaf rows show none: songs (`songs-list-scrollbar-b.jpg`, `songs-list-2-b.jpg`), Resume/chapters (`audiobook-open-chapters-b.jpg`), hold-SELECT items (`hold-select-popup-np-b.jpg`), a note (`extras-notes.jpg`), an inline-value setting such as Repeat "All" (`settings-03-repeat.jpg`). | G12 p.8, OWN | ✅ (SPEC chrome) |
| Battery states | Icon <20%, about half, full. Charging = **lightning bolt**; charged = **plug**. | G12 p.13 | ✅ |
| Quick scroll | Move thumb fast to "display a letter of the alphabet". Wheel through the alphabet; lift thumb to return. **"Items beginning with a symbol or number appear after the letter Z."** "Not all languages are supported." ✅ owner photos: after Z confirmed (number-led songs are at the very end of Songs, `quickscroll-123-b.jpg`). The overlay is a centred **black rounded square (≈70–80 px) with a big white bold letter** drawn over the list (`quickscroll-letter-S-b.jpg`); for the number group it shows **"123"** with the digits staggered diagonally (1 high, 2 middle, 3 low), not "#" (`quickscroll-123-b.jpg`). | G12 p.9 (G08 p.7 and G07 say *before "A"*; see §9), OWN | ✅ (qscroll exists) |
| Quick scroll in Cover Flow | Works in Cover Flow as well as lists | G08 p.7 | adapt in Phase 7 |
| Clicker | "you can hear a clicking sound through the earphones or headphones and through the iPod classic internal speaker". Set Clicker On/Off. | G12 p.9 (G08 p.8) | adapt: haptic, On/Off (SPEC) |
| Time in title | Optional clock in the title bar (Settings › Date & Time › Time in Title). ✅ owner photos/video: with it on, the header **alternates between the screen title and the clock** ("15:04", 24 h on this device), left-aligned where the title sits on light screens and centred on the dark screens (`usb-connected.jpg`, `usb-connected-b.jpg`, `about-1.jpg` keeps the device name). Video `photo 1190.mov`: clock ≈1.9–4.4 s, then title ≈2 s, alternating; entering a screen showed the title first (Music: 6 s after the push). The swap completes within one 24 fps frame (≤42 ms); a still caught both ghosted (`extras-calendars.jpg`), so it is a hard swap or a very short cross-fade. The exact trigger rule is not proven (the clock stayed up while scrolling). | G12 p.41, OWN | ✅ feasible (RTC) |
| Scrollbar | ✅ owner photos: a thin (≈3–4 px) dark-gray rounded thumb at the extreme right of the list column, no visible track, drawn over the rows; shown on long lists (Settings 17 rows, Music menu 11 rows, Songs) (`settings-04-main-menu.jpg`, `songs-list-scrollbar-b.jpg`, `mov-music-menu-2.jpg`). | OWN | ✅ |
| Selection move | ✅ owner video: the selection bar **animates** between rows over ≈1–2 frames (≲80 ms), with the text colour cross-fading; it does not jump (`mov-main-selection-anim.jpg`). | OWN | ✅ |
| Push transition | ✅ owner (2026-10-06): there is **always a slide, never a cut**. SELECT: the current view leaves to the left and the new one comes in from the right. MENU: the current view leaves to the right and the previous one comes in from the left, sometimes landing in a section's split view. ✅ owner video (Main → Music): the old list slides out left while the new list comes in from the right, **≈170–210 ms**. The header bar does not slide; only its title text swaps at the start. The right art panel does not slide or change (`mov-slide-transition.jpg`). ✅ owner video 2 (`owner/VIDEO-2.md` §D): Main → Extras is the same, ≈170–210 ms, **ease-out**. **Split → full-screen** pushes (Main → Audiobooks, Calendars → month grid) take **≈250 ms**: the incoming page, with its own header, slides in from the right over the panel first, then pushes the old list out; their pops take ≈290 ms. | OWN | ✅ |
| Reveal transition | ✅ owner (2026-10-06), Extras › Clocks: while the item is highlighted the right panel shows the clock page as a fixed background; SELECT slides only the left menu away and the preview expands to full screen; MENU slides the menu back in from the left and the content contracts into the right panel. Note: the photos show the panel (map + large 3D clock face + "Oct 6 2026", `extras-clocks-sel.jpg`) is not pixel-identical to the full Clocks page (blue city band + clock + time over the map, `clocks.jpg`). ✅ **Measured, owner video 2** (`owner/VIDEO-2.md` §A3, labels `clocks-reveal-*`, `clocks-return-*`): The **world map is a fixed shared backdrop** and never moves. SELECT pushes everything else (header, list, the panel's white clock and date) out to the left, while the Clocks page's blue city band slides in from the right. **≈250 ms**, near-linear; the outgoing layer moves faster, a slight parallax. MENU is the exact mirror. So there is no expand or morph: that settles the panel-vs-page mismatch above. Which other screens reveal rather than push ⚠️: the Settings panels show an icon tile, not the destination, so Settings likely pushes. Extras › Calendars → month grid is a normal push (§D3). This is part of the owner's general **navigation model** (✅ owner, 2026-10-06): above the bottom layer every menu is a split screen with a preview of the highlighted item; at the leaf, SELECT reveals the content full-screen; deeper list levels are full-width. Where the split stops per area (and the two places the photos refine the rule) is tabulated in `docs/design/spec/index.md` "Navigation model". | owner, OWN | ✅ |
| Split transition (Cover Flow) | ✅ owner video 2 (`owner/VIDEO-2.md` §A1–A2, labels `coverflow-split-*`, `coverflow-exit-*`). **Entry (SELECT on Music › Cover Flow):** the screen parts at the list/panel seam (**x = 160**). The list and its header strip slide left and the art panel slides right, rigidly, over an already-rendered Cover Flow. **≈210 ms**, **ease-in**; the list leads by ≈1 frame. Then the album text (+170 ms) and the Cover Flow header (+250 ms) appear. **Exit (MENU):** the header and text vanish. The panel slides in from the right first (linear), then the list and header slide in from the left (ease-out) over it. **≈290 ms.** | OWN | ✅ Phase 7 |
| Dark full-screen screens | ✅ owner photos: USB connected, About, Brightness and Screen Lock are full-screen **dark** screens: dark header with a **centred** white title (or the device name / clock) and a light battery outline, over a slate-to-navy gradient body (`usb-connected-b.jpg`, `about-1.jpg`, `brightness.jpg`, `extras-screenlock-new-combination.jpg`). | OWN | ✅ |
| First boot | "the language menu appears", in a split screen with a photo | G12 p.5, IMG `photo-language.jpg` | ✅ feasible |

## 2. Main menu

**Default items, in order:** **Music, Videos, Photos, Podcasts, Extras, Settings, Shuffle Songs, Now Playing**. Now Playing shows only while something is playing.
- Sources: IMG `main-alt.png` (G12 p.5, 2.0.5-era) and MM7 ch02s02. "Choose the sixth menu item (Settings)" (G12 p.51).
- The menu is a split screen with an art panel (SPEC). Its title is "iPod" (MM7 ch02s02). ✅ owner video `photo 1190.mov` ("iPod" alternating with the clock).
- ✅ owner photos/video, **panel per item:** Music = full-height album art with a slow pan, covers changing (`mov-main-panel-music.jpg`); Podcasts = podcast icon + "**25 Podcasts**" (`mov-main-panel-podcasts.jpg`); Videos = film-strip icon + "**No Videos**" (`mov-main-panel-videos.jpg`); Extras = world map, a 3D analog clock face and the date "**Oct 6 2026**" (`main-menu-custom-hold.jpg`); Settings = device name, Apple logo, capacity capsule bar, "**128 GB Free**" (`settings-01-about.jpg`). The panel switches content ≈0.5 s after the selection moves (it doesn't slide). **Updated by owner video 2** (`owner/VIDEO-2.md` §C):
  - The panel updates only once the selection **rests**, skipping items passed quickly. The delay is ≈0.4 s for glyph panels and ≈1.1 s for a cover.
  - The arrival varies: Music art slid in from the right (≈125 ms), Alarms' old art slid out right, Extras cross-fades.
  - **Art drift:** each cover dwells **≈7 s**, then **cross-fades ≈400 ms** to the next. During the dwell it pans **linearly at ≈5–8 px/s** in a per-cover direction, with **≤5 % zoom**.
  - The cycle is not reset by pushes, Cover Flow, or moving between art items.
  - The Audiobooks main-menu item also shows album art. Videos and Photos stay in the stock menu even with no videos/photos synced.
- **iTunes U** appears when such content is synced ("From the main menu, choose iTunes U", G12 p.32) ⚠️ (presence is conditional; position not verified).
- **Voice Memos** appears once a mic or the remote-and-mic earphones have been connected (G12 p.46).
- **Radio** appears with the iPod Radio Remote (G08 p.34).

**Settings › Main Menu** is a checklist: "Choose each item you want to appear in the main menu. A checkmark indicates which items have been added." (G12 p.8)
- Items the guides confirm can be added: **Songs** (G12 p.8), **Screen Lock** (G12 p.43), **Clock(s), Games, Contacts, Calendar, Playlists** (MM7 ch02s11).
- ✅ owner photo `main-menu-custom-hold.jpg`: items added through the checklist go into **fixed canonical slots**, not at the end. With Audiobooks and Alarms added the menu reads **Music, Audiobooks, Videos, Photos, Podcasts, Extras, Alarms, Settings, Shuffle Songs** (Now Playing absent: nothing playing).
- ⚠️ Full option list (the checklist screen itself is not photographed), not verified. Reconstructed from memory of 2.x, so confirm before shipping: Music, Cover Flow, Playlists, Artists, Albums, Compilations, Songs, Genres, Composers, Audiobooks, Search, Genius Mixes, Videos (+ Movies, TV Shows, Music Videos, Rentals, Video Playlists), Photos, Podcasts, iTunes U, Extras, Alarms, Calendars, Clocks, Contacts, Games, Notes, Screen Lock, Stopwatch, Settings, Shuffle Songs, Reset Menu.

| Item | Feasibility on our 5.5G |
|---|---|
| Music, Podcasts, Settings, Shuffle Songs, Now Playing | ✅ core (Phases 4–5, 8) |
| Videos, Photos | ⛔ out of scope (SPEC: hidden). Keep their *slots* in the Main Menu checklist unchecked/hidden. Photo shows stock keeps both rows even with nothing synced (Videos panel "No Videos", `mov-main-panel-videos.jpg`); owner to decide whether we show them the same way. |
| Extras | ✅ partially (§6) |
| Main Menu checklist | ✅ feasible. Cheap: a bitmask setting. |

## 3. Music menu

**Default items, in order:** **Cover Flow, Genius Mixes\*, Playlists, Artists, Albums, Songs, Genres, Composers, Audiobooks, Search**.
- ✅ owner video/photo (`mov-music-menu-1.jpg`, `mov-music-menu-2.jpg`, `music-menu-b.jpg`): with Compilations enabled the 2.0.5 menu is **Cover Flow, Genius Mixes, Playlists, Artists, Albums, Compilations, Songs, Genres, Composers, Audiobooks, Search** (11 rows; it scrolls, with a scrollbar). Split screen with full-height album art at the right.
- Sources: G12 p.26–30 for each item, IMG `photo-music.jpg` (fw 1.0), MM7 ch02s03. The music menu is a split screen with art (SPEC).
- \*"Choose Music > Genius Mixes" (G12 p.7, p.27). Shown only when mixes are synced ⚠️. Position ✅ owner photo `music-menu-b.jpg`: **directly under Cover Flow**.
- **Compilations** is off by default: "you can add a Compilations item to the Music menu" (G12 p.30). When on it sits **between Albums and Songs** ✅ owner photo `music-menu-b.jpg`.
- **Settings › Music Menu** works like Main Menu: checkmarks, plus **"Reset Menu"** ("To revert to the original Music menu settings, choose Reset Menu", G12 p.30).

| Sub-item | 2.0.5 detail | Source | Feasibility / phase |
|---|---|---|---|
| Cover Flow | "displays your albums alphabetically by artist name. You see the album artwork, title, and artist name." Wheel or Next/Prev moves. Center opens the album's track list; Center on a song plays it. ✅ owner video 2 (`owner/VIDEO-2.md` §B): **Light** background and the normal silver header ("Cover Flow" / clock). Centre cover ≈128×128 at x 96–224, y 40–168, with a reflection. 4–5 side covers per side, ≈24 px apart, angled ≈70–75° to face the centre. Album over artist, bold black, centred at y≈198/213. Overlong lines are left-aligned and hard-clipped, with no ellipsis. The centre cover turns while stepping. Alphabetical by artist confirmed. Entry is the **split transition** (§1). ⚠️ SELECT/flip not shown. | G12 p.26, OWN | ✅ Phase 7 |
| Genius Mixes | ✅ owner video 2 (48.0–49.4 s, `labeled/v2-t48.8.jpg`): with **no mixes synced** the item is still listed. It opens a light full-screen page (header "Genius Mixes", a large atom glyph, and the text "Genius Mixes are created for you by iTunes and play songs from your library that go great together. To use Genius Mixes, turn on Genius in iTunes."); its panel in the Music menu shows a cover. With mixes synced: full-screen pages (dark theme: mix name, artist line, 2×2 cover mosaic). "The dots at the bottom of the screen indicate how many Genius Mixes are synced". Center or Play starts a mix. "The Speaker icon appears when the selected Genius Mix is playing." | G12 p.27–28, IMG `genius-mix.png` | ⛔ needs iTunes Genius data. Owner decision: skip, or **adapt** as "genre/decade mixes" with the same visuals. |
| Playlists | User playlists, **On-The-Go**, saved Genius playlists | G12 p.27–28 | ✅ Phase 4. On-The-Go (§5.5) is unplanned. |
| Artists → Albums → Songs | "All Albums" / "All Songs" rows; two-line album rows with thumbnails. ✅ owner photos: Artists is a full-width single-line list starting with **All Albums** (`artists-list-b.jpg`); the artist's Albums list starts with an **All Songs** row (dark tile with a big blue note, subtitle "**432 Songs**"), then two-line album rows (title bold / artist gray) with ≈44 px covers and thin separator lines (`artist-albums-allsongs-b.jpg`). | SPEC, IMG `photo-artist-albums.jpg`, OWN | ✅ Phase 4 |
| Albums, Songs, Genres, Composers | Standard lists. ✅ owner photos: **Albums** = two-line rows (title bold / artist gray regular) with a ≈44 px cover at the left, ≈4 rows per screen; the selection bar starts right of the cover (`albums-list-twoline-b.jpg`). **Songs** = two-line rows (title bold / artist gray), **no thumbnails**, ≈5 rows per screen, thin separators, no chevron on the selected song (`songs-list-scrollbar-b.jpg`). Truncation is "..." appended (also after a period: "arr...."; `songs-list-2-b.jpg`); the selected row marquees (`songs-list-marquee-b.jpg`). All these lists are full-width (no art panel). | G12, MM7, OWN | ✅ Phase 4 |
| Audiobooks | ✅ owner video 2 (55.6 s, `labeled/v2-t57.0.jpg`): the **book list** is full-width, **two-line rows with cover thumbnails** (title bold / author gray), 4 per screen, like Albums. "choose Audiobooks from the Music menu. Choose an audiobook, and then press Play/Pause." Resumes where you left off. Chapters: Next/Prev skip a chapter. "You can also choose the audiobook from the Audiobooks menu and then choose a chapter, or choose **Resume**". Shuffle skips audiobooks. ✅ owner photo `audiobook-open-chapters-b.jpg`: an opened book is a full-width single-line list, **Resume** first, then the chapter titles; no progress hints on these rows. | G12 p.33, OWN | ✅ Phase 6 (see §9 on the chapter list) |
| Search | §5.3 | G12 p.30 | ✅. Not in ROADMAP. |

## 4. Other top-level menus

| Menu | Contents (2.0.5) | Source | Feasibility |
|---|---|---|---|
| **Podcasts** | List of shows → episodes. Episodes are in "reverse chronological order". "a blue dot next to shows and episodes you haven't played yet". Resume per episode. Next/Prev = chapter. Audio Now Playing shows "show, episode, and date information"; "Press the Center button to bring up the scrubber, star ratings, and other information about the podcast." | G12 p.32, IMG `photo-podcasts.jpg` | ✅ Phase 4. Needs a "played" flag and date sort (tagcache has playcount/mtime). |
| **Videos** | Movies, Rentals, TV Shows, Music Videos, Video Playlists, Settings | MM7 ch02s05 | ⛔ out of scope |
| Videos › Settings | **TV Out** (Ask/On/Off), **TV Signal** (NTSC/PAL), **TV Screen** (Widescreen/Standard), **Fit to Screen** (On/Off), **Alternate Audio**, **Captions**, **Subtitles** (On/Off) | G12 p.35 | ⛔ |
| **Photos** | All Photos, Settings, then albums by name | G12 p.38, MM7 ch02s06, IMG `photo-photos-menu.jpg` | ⛔ out of scope |
| Photos › Settings | **Time Per Slide**, **Music**, **Repeat**, **Shuffle Photos**, **Transitions**, **TV Out**, **TV Signal** | G12 p.38 | ⛔ |
| **Shuffle Songs** | "begins playing songs from your entire music library in random order, skipping audiobooks and podcasts" | G12 p.29 | ✅ Phase 4 |
| **Now Playing** | §5.1 | | ✅ Phase 5 |

## 5. Behaviours

### 5.1 Now Playing
**Layout:** shuffle icon (songs/albums), repeat icon (All, or "Repeat Once"), play/pause, battery, album art, song info (title, artist, album, rating stars, "sequence number"), progress bar with elapsed/remaining (G12 p.24; metrics in SPEC).
- ✅ owner photos (`np-normal-b.jpg`, `np-scrubber-b.jpg`): the repeat glyph alone shows when shuffle is Off (dark gray, under the header at right). The title marquee-scrolls, clipped at the text block's left edge with no fade, looping with a wide gap. Long artist/album lines end in "...". **Unrated: the rating row is blank**; rated: small near-black stars (`np-rating-4stars-b.jpg`). Counter "1 of 16".
- ✅ owner photo `np-audiobook-noart-chapterticks-b.jpg`: the **no-art placeholder** is a dark slate-blue square with a large light-gray double note, slanted like real art, with reflection.

**Volume:** "When you see the progress bar, use the Click Wheel to change the volume. If you don't see the progress bar, press the Center button until it appears." (G12 p.25) ✅ owner photos `np-volume-b.jpg`, `np-volume-2-b.jpg`: the volume bar **replaces the whole bar row**: time labels go, a small speaker (no waves) sits at the left and a speaker with waves at the right, glossy blue fill, light-gray empty part, faint reflection; it is longer than the progress bar (≈x 40–280).

**Modes seen in owner photos:** scrubber = blue gradient diamond on an empty track, times stay (`np-scrubber-b.jpg`); rating = **five small black dots**, centred, no time labels, turning into glossy blue stars (`np-rating-unrated-b.jpg`, `np-rating-2stars-b.jpg`); shuffle = dark-gray shuffle icon at the left, then a light-gray rounded shelf with **Off / Songs / Albums**, the selected one in a raised lighter box with **blue text**, the others black bold (`np-shuffle-b.jpg`). The cycle order itself was not filmed.

**Center-button cycle, per G12 p.25.** The table there lists, in this order:
1. **Scrubber bar** ("the playhead on the bar shows the current location", p.6; "a diamond" in G08 p.27)
2. **Genius slider** ("doesn't appear if Genius information isn't available"; move the slider "to Start" / "to the right")
3. **Shuffle slider** ("move the slider to Songs or Albums"; G08: "Choose Songs, Albums, or Off")
4. **Song rating** ("press the Center button until the five rating bullets appear")
5. **Lyrics** ("Lyrics don't appear if you didn't enter them"; "You can scroll through the lyrics as the song plays")

The prose ordering differs:
- G08 p.27: "the scrubber bar, rating bullets, shuffle settings, lyrics, podcast information, and more".
- MM7 ch05s15: shuffle comes "Right before you get to the lyrics screen"; lyrics come "after four or five taps".

| Content type | Stock cycle (best reading of sources) | Our plan (SPEC, owner) | Feasibility |
|---|---|---|---|
| Song | Progress → Scrubber → [Genius] → Rating → Shuffle → [Lyrics] → Progress ⚠️ (rating/shuffle order per G08 and owner; G12's table puts Shuffle before Rating) | Normal → Scrubber → Rating → Shuffle → Normal; 4 s timeout | Genius ⛔ skip. **Lyrics ✅ adapt**: Rockbox can read embedded lyrics (USLT/©lyr) or a `.lrc`. Add as a final step **only when present**, as stock does. |
| Audiobook | Scrubber, Rating. No shuffle (shuffle skips audiobooks). Chapter markers on the bar ("You can view chapter markers on the Now Playing screen", G08 p.34). ✅ owner photos `np-audiobook-noart-chapterticks-b.jpg`, `np-audiobook-volume-b.jpg`: title line = **book title**, artist = author, album line = **current chapter title**, counter "**73 of 648**", bar spans the **whole book** ("0:00" … "**-11:47:55**", h:mm:ss) with **thin vertical chapter ticks**; no "Chapter n of N" text; chapter artwork changes per chapter. ⚠️ The 2.x guides do not mention the 5G's in-cycle **Speed** item, and the audiobook cycle beyond volume was not photographed. | Normal → Scrubber → Rating → Normal | ✅ Phase 5/6 |
| Podcast (audio) | Scrubber, "star ratings, and other information about the podcast" (G12 p.32). Podcast artwork "can change during an episode". | not specified in SPEC | ✅ adapt: same as audiobook plus a description page ⚠️ |
| Video podcast | "You cannot assign ratings to video podcasts." (G12 p.26) | — | ⛔ |

- **Next/Prev:** Next skips to the next song or chapter. Prev restarts; Prev twice goes to the previous song or chapter. Hold to fast-forward or rewind (G12 p.6).
- **Mode timeout:** the guides don't specify one. SPEC: 4 s (owner).

### 5.2 Ratings
1–5 stars, set from the Now Playing rating step with the wheel. "The ratings you assign to songs on iPod classic are transferred to iTunes when you sync." (G12 p.26)
- Feasibility ✅: Rockbox tagcache has a rating field. We can't sync it back to iTunes; a possible substitute is writing it to the file tag or a sidecar, as an owner choice.

### 5.3 Search (letter wheel)
Music › Search.
- Wheel through the alphabet; Center enters a character. Results update live: "if you enter 'b,' iPod classic displays all music items containing the letter 'b'" (a *substring* match).
- "To enter a space, press Next/Fast-forward." "To delete the previous character, press Previous/Rewind."
- "Press Menu to display the results list". Results have type icons: "song, video, artist, album, audiobook, podcast, or iTunes U". "To return to Search (if Search is highlighted in the menu), press the Center button."
- It searches "songs, playlists, album titles, artist names, audio podcasts, and audiobooks", not videos, notes, calendars, contacts or lyrics.
- ✅ owner photos `search-empty-b.jpg`, `search-results-b.jpg`: full-width list. The top row is two-line with a gray **magnifier** tile: **Search** / "**No Search Results**", becoming "**Results for: h**". Result rows carry a gray type icon (music note for songs) with title / artist. A **dark rounded strip** near the bottom (≈y 190–215) holds a white rounded text field at the left (typed text + cursor) and the alphabet **A B C D E F G H I J K L M…** in white bold; the current letter sits in a **blue rounded box**; the letters scroll to keep it visible ("..." at the right end).
- Sources: G12 p.30, IMG `photo-search-quickscroll.jpg`, OWN. **Not in ROADMAP.** Feasibility ✅: tagcache substring search on title/artist/album. Watch the cost on a 1 TB library; search incrementally on the COP.

### 5.4 Hold-center "additional commands" menu
New in 2.0. In 1.x and on the 5G you held Center "until the song title flashes" (G07, G5 p.25).
- Items named in G12: **"Start Genius"** (only "if there's Genius data"), **"Add to On-The-Go"**, **"Remove from On-The-Go"** (inside OTG), **"Browse Artist"** and **"Browse Album"** (from Now Playing) (G12 p.6, 27–29).
- Genius playlists also offer **"Save Playlist"** and **"Refresh"**.
- Look and order ✅ owner photo `hold-select-popup-np-b.jpg` (from Now Playing): **not an overlay**, but a **full-screen ordinary list** under the normal header: **Add to On-The-Go / Browse Album / Browse Artist / Cancel** (Start Genius absent: no Genius data). Order matches the nano 4G list "Start Genius / Add to On-The-Go / Browse Album / Browse Artist / Cancel" (IMG `nano4g-nowplaying-menu.png`), which is drawn as an overlay there. No chevrons.
- Feasibility ✅ except Genius. **Not in ROADMAP.**

### 5.5 On-The-Go
- **Add:** select or play a song (or highlight an album) → hold Center → "Add to On-The-Go".
- **View:** Music › Playlists › On-The-Go.
- **Remove:** hold Center on a song in the list → "Remove from On-The-Go".
- **Clear:** "Choose Music > Playlists > On-The-Go > Clear Playlist, and then click Clear."
- **Save:** "On-The-Go > Save Playlist". "The first playlist is saved as 'New Playlist 1'". The OTG list is then cleared. Saved lists can't be edited.
- Source: G12 p.28. (MM7 says "On-The-Go 1"; we follow Apple ⚠️.)
- Feasibility ✅ using Rockbox playlists (`.m3u8` in a hidden dir). **Not in ROADMAP.**

### 5.6 Genius / Genius Mixes
- **Genius playlists:** made on device from a seed song. Needs Genius data synced from iTunes ("Genius recognizes the song but there aren't at least ten similar songs" → no item). Saved with "the song title and artist". "Refresh" replaces the list.
- **Messages:** "Connect to iTunes to activate Genius"; "Genius is not available for the selected song" (G12 p.27, p.50).
- **Genius Mixes:** see §3.
- Feasibility ⛔ (no iTunes Genius DB). Recommend **skip**, and hide every Genius affordance (slider, menu item, messages).
- Optional owner decision: a local "similar songs" substitute (same genre/artist) under the stock name ⚠️. That risks misleading the user, so we lean to skip.

### 5.7 Cover Flow
See §3. The 2.0.5 visual is in IMG `coverflow-render.png`, `photo-coverflow.jpg`, now ✅ confirmed on the owner's 2.0.5 by video 2 (`owner/VIDEO-2.md` §B; labels `coverflow-*.jpg`). Feasibility ✅ Phase 7.

## 6. Extras

Items ✅ owner photos `extras-1.jpg`, `extras-menu-b.jpg`, `extras-clocks-sel.jpg`: **Clocks, Calendars, Contacts, Alarms, Games, Notes, Screen Lock, Stopwatch** (8 rows). This is **not alphabetical**; MM7 ch02s08's alphabetical list (Alarms, Calendars, Clocks, …) is the nano's ("The Classic and the Nano show these menu items in a slightly different order"). **Voice Memos** appears after a mic has been connected (G12 p.46); there is no top-level Sleep Timer.
- ✅ owner photos: Extras is a **split screen** with per-item art: Clocks = map + 3D clock + date; Calendars = time, calendar page "Oct"/"06", "Tuesday"; Alarms = red alarm clock; Stopwatch = analogue stopwatch. Calendars, Alarms and Stopwatch sub-lists keep the split screen; **Games and Notes are full-width**.
- Clocks uses the **reveal** transition (✅ owner, 2026-10-06; see §1).

| Item | 2.0.5 behaviour (verbatim where quoted) | Source | Feasibility |
|---|---|---|---|
| **Clocks** | ✅ owner photo `clocks.jpg`: no header bar; the top third is a **blue band** with the city ("**Boston**") at the left in white bold, a white analog clock face in the centre overlapping the band's lower edge, and "**15:05**" over "**Today**" at the right; below, a light gray-blue world map on dark navy. "On the Clocks screen, click the Center button and choose Add." Then "Choose a region and then choose a city." The newest clock comes last. A clock's popup offers **Add / Edit**; to delete, "Choose the clock. Choose Delete." | G12 p.41, MM7 ch02s12 | ✅ RTC present. Needs a city/time-zone table (size cost; a small fixed list is fine). |
| **Alarms** | "Choose Create Alarm" → **Alarm** (On), **Date**, **Time**, **Repeat** (e.g. "weekdays"), **Sound**: "Tones" ("select Beep to hear the alarm through the internal speaker") or a playlist; **Label** (e.g. "Wake up"). "Choose the alarm and then choose Delete." | G12 p.42, MM7 ch02s13 | ⚠️ Waking from sleep needs RTC alarm support on the PP5022/5.5G. Rockbox has `HAVE_RTC_ALARM` only on some targets; check. "Beep" through the piezo is feasible. |
| **Sleep Timer** | Extras › Alarms › **Sleep Timer**: Off ⚠️, **15, 30, 60, 90, 120** minutes. ✅ owner photo `extras-alarms.jpg`: the Alarms list is two-line rows inside the split screen: **Create Alarm** / "New Alarm" and **Sleep Timer** / "**Inactive**" (subtitle gray). The value list is not photographed ⚠️. | G12 p.42, MM7 ch02s16, OWN | ✅ Rockbox has a sleep timer |
| **Stopwatch** | Play/Pause starts. **Center records laps** ("Up to three lap times show beneath the overall time"). Play/Pause stops; "choose **Resume**" or "**New Timer**". The log list gives start date/time, total, shortest/longest/average lap, last laps. Center → "**Delete Log**" / "**Clear Logs**". ✅ owner photo `extras-stopwatch.jpg`: list **New Timer / Clear Logs**, then one row per log: date at the left (**M/D/YY**, "12/5/20") and start time right-aligned in **blue** ("16:36"). | G12 p.42–43, OWN | ✅ cheap |
| **Screen Lock** | ✅ owner photo `extras-screenlock-new-combination.jpg`: a **dark** full-screen screen (dark header, centred "Screen Lock"), an open padlock at the left, a dark rounded 4-digit roller "**0 0 0 0**" with the current digit in a blue rounded box, and "**New Combination**" centred below. "**New Combination**" screen (4 digits, wheel + Center; Next/Prev move position) → "**Confirm Combination**" → Screen Lock screen with **Lock** / **Reset**. Locked = "a picture of a lock"; Center to enter the code. Wrong code → "the lock remains". Unlocks when connected to the primary computer. | G12 p.43–44 | ✅ (USB unlock → our USB-connect event) |
| **Contacts** | vCards in the `Contacts` folder; sort set in Settings › Sort By (First/Last) | G12 p.45 | ✅ adapt (vCard parsing; Rockbox has none, so new code) |
| **Calendars** | `.ics`/`.vcs` in `Calendars`; "**To Do's**" sub-item. ✅ owner photo `extras-calendars.jpg`: list **All Calendars / To Do's / Alarms** (value "**Beep**" right-aligned in blue: the calendar alarm sound). ✅ owner video 2 (74.7 s, `labeled/calendar-month.jpg`): All Calendars opens a **full-screen month grid**: header "Oct 2026", a Sun…Sat row, glossy blue day cells, today dark blue, adjacent-month days greyed; reached by a normal push (≈250 ms). | G12 p.45, OWN | ⚠️ costly (iCal parser). Owner decision; recommend Phase 11+ or skip. |
| **Notes** | `.txt` in `Notes` folder (MM7: "under 4 kilobytes"). ✅ owner photo `extras-notes.jpg`: full-width list; a stock note "**Instructions**" exists; no chevron on it. | G12 p.46, MM7 ch10s06, OWN | ✅ cheap (text viewer) |
| **Games** | Built in: "**iQuiz, Klondike, and Vortex**" (+ purchased iPod games). ✅ owner photo `extras-games.jpg`: full-width two-line rows (≈55 px pitch) with a ≈44 px icon at the left, thin separators; subtitles are play counts: "**Never Played**", "**Played 6 Times**", "**Played Once**". | G12 p.43, OWN | ⛔ Apple's games can't be reused. Owner decision: skip, or offer original look-alikes. Rockbox plugins (solitaire, brickmania) exist but would need reskinning. |
| **Voice Memos** | Start Recording / Pause / Resume / "Stop and Save" | G12 p.46 | ⛔ (no mic path; recording is cut in Phase 9) |

## 7. Settings (complete tree)

**Top-level order:** About, Shuffle, Repeat, Main Menu, Music Menu, Volume Limit, Backlight, Brightness, Audiobooks, EQ, Sound Check, Clicker, Date & Time, Sort By, Language, Legal, Reset Settings.
- The first ten rows are confirmed by IMG `photo-settings.jpg` (fw 1.0, rows About…Audiobooks).
- The tail is anchored by MM7 ch02s19: "Language" is "the menu item two rows up from 'Reset Settings'", which puts **Legal** between them. G12 p.51 ("Choose the last menu item (Reset Settings)").
- ✅ owner photos `settings-01-about.jpg` … `settings-17-reset-settings.jpg`: the **full 17-row order is proven** on 2.0.5, Legal included.
- ✅ owner photos: **Settings is a split screen** (list left, per-item panel right; see §7.1), not a full-width list.
- ✅ owner photos: **inline values are blue** (mid-blue, right-aligned) on unselected rows and white on the selected row. Only **Shuffle, Repeat, Sound Check, Clicker, Sort By** show an inline value; **Backlight, Brightness, Audiobooks, EQ** show none in the list (their value is the panel caption). A selected inline-value row shows **no chevron**; rows that open a screen show it (`settings-02-shuffle.jpg`, `settings-03-repeat.jpg`, `settings-07-backlight.jpg`).

| Item | Values (default **bold**) | Sub-screen / behaviour | Source | Feasibility |
|---|---|---|---|---|
| **About** | — | G12: "press the Center button to cycle through the screens of information"; MM7: "all three screens of info". ✅ owner photos `about-1.jpg`, `about-2.jpg`, `about-3.jpg`: **3 pages**, a **dark** full-screen screen. Dark header with the **device name** centred ("<Owner name>’s iPod", curly apostrophe) and the battery. Every page repeats one line: "**19.9 GB Used**" left, glossy Apple logo centre, "**128 GB Free**" right; page dots ● ○ ○ at the bottom. **P1:** a wide rounded capacity capsule bar (Audio segment, then a thin tick per other type, rest light blue-gray) and the legend ■ **Audio** (light blue) ■ **Video** (blue) ■ **Photos** (orange) ■ **Other** (dark red). **P2:** two columns of label/count: **Songs 432, Videos 0, Podcasts 25, iTunes U 0** \| **Photos 0, Games 3, Contacts 0**. **P3:** **Serial Number**, **Model** "MC-series model", **Version** "2.0.5 Mac" (values in a second column from mid-screen, left-aligned). There is no "Format", "Capacity" or "Available" row. **Paging (✅ owner, 2026-10-06): NEXT / PREV move between the pages**; the wheel does nothing; MENU returns to Settings. (G12's "Center button" wording is not what the owner's 2.0.5 does.) | G12 p.9, MM7 ch02s09, OWN, owner | ✅ Show our own version string in stock style. Hide counts for videos/photos or show 0 (owner). |
| **Shuffle** | **Off** / Songs / Albums | Inline value (blue), ✅ owner photo `settings-02-shuffle.jpg`; panel caption "Play Music Library in Sequence" when Off. Cycling on Center ⚠️ (inferred from the missing chevron) | G12 p.29, IMG | ✅ |
| **Repeat** | **Off** / One / All | Inline (blue), ✅ owner photo `settings-03-repeat.jpg`; caption "Repeat Current Playlist" when All | G12 p.29, OWN | ✅ |
| **Main Menu** | checklist | §2 | G12 p.8 | ✅ |
| **Music Menu** | checklist + "Reset Menu" | §3 | G12 p.30 | ✅ |
| **Volume Limit** | — | "choose **Set Volume Limit**" → volume bar, wheel sets the max ("A triangle on the volume bar indicates the maximum volume limit"; Play previews). Center sets. Then Menu = accept with no code, or "**Enter Combination**" screen (4 digits). Change: enter code, wheel, Play/Pause accepts. Remove = drag to max. | G12 p.30–31 | ✅ Not in ROADMAP |
| **Backlight** | ✅ owner photo `backlight-values.jpg`: **Off, 2 Seconds, 5 Seconds, 10 Seconds, 20 Seconds, 30 Seconds, Always On** (no 15; default **10 Seconds** per G12) | list, still in the split screen (panel keeps "Backlight", the timer tile and the current value). **No checkmark**: the current value is simply the highlighted row on entry. | G12 p.8, MM7 ch02s15, OWN | ✅ |
| **Brightness** | slider | "use the Click Wheel to adjust" (left dims). ✅ owner photo `brightness.jpg`: a **dark** full-screen screen, dark header with "Brightness" centred. A dark rounded slider box (≈x 23–297, y 114–155) holds a **crescent moon** at the left, a thin capsule track, and a **sun** at the right; "**0%**" centred below (≈y 208). Panel caption in Settings: the percentage ("0%"). | G12 p.9, G08 p.10, OWN | ✅ (5.5G has backlight PWM brightness) |
| **Audiobooks** | ✅ Slower / **Normal** / Faster (owner photo `audiobooks-speed.jpg`) | **Not a list**: a light full-screen prompt. Heading (bold, black, left): "**How fast should audiobooks play?**"; body (regular, dark gray): "**Playback speed will not affect the pitch of the voice in audiobook playback.**"; near the bottom, a light-gray rounded shelf holding a dark **segmented control Slower \| Normal \| Faster**, the selected segment blue, the others charcoal with white text. Panel caption in Settings: "**Normal Playback**" (`settings-09-audiobooks.jpg`). Stock only affects iTunes/Audible books. | G12 p.33, OWN | ✅ (timestretch, Phase 6). Ours applies to all books, which is a deliberate improvement presented in stock idiom. |
| **EQ** | No inline value; panel caption = current preset ("**Spoken Word**", ✅ owner photo `settings-10-eq.jpg`, spelling confirmed). **Off** + presets ⚠️ (list not photographed): Acoustic, Bass Booster, Bass Reducer, Classical, Dance, Deep, Electronic, Flat, Hip Hop, Jazz, Latin, Loudness, Lounge, Piano, Pop, R&B, Rock, Small Speakers, Spoken Word, Treble Booster, Treble Reducer, Vocal Booster | list. "If you assigned an equalizer preset to a song in iTunes and the iPod classic equalizer is set to Off, the song plays using the iTunes setting." | G12 p.32. Preset names from iTunes/iPod common list ([headfonia](https://www.headfonia.com/eq-to-the-rescue/2/)); MM7: "more than 20 different equalizer presets". Order and exact spelling ("Hip Hop" vs "Hip-Hop") ⚠️ | ✅ Map each to Rockbox EQ bands. Apple's curves aren't published, so approximate them. |
| **Sound Check** | **Off** ⚠️ / On (values ✅ owner photo `settings-11-sound-check.jpg`, inline blue; caption "Use Original Volume Level" when Off; default not proven) | "If you haven't activated Sound Check in iTunes, setting it on iPod classic has no effect." | G12 p.31 | ✅ Rockbox ReplayGain (track) |
| **Clicker** | **On** ⚠️ / Off (values ✅ owner photo `settings-12-clicker.jpg`, inline blue; caption "Play Sound Through Headphones and Speaker" when On; default not proven) | (G12 p.9 says "Settings > General", an editing slip carried over from the nano; G08 p.8: "Choose Settings and set Clicker to Off") | G12 p.9, G08 p.8 | ✅ adapt: haptic (SPEC) |
| **Date & Time** | — | Panel: calendar-page tile (red "Oct" band, "06") and the weekday "**Tuesday**" (✅ owner photo `settings-13-date-time.jpg`). Sub-items: **Date**, **Time** (wheel per field, Center = next), **Time Zone** (city picker), **24 Hour Clock** (On/Off), **Time in Title** (On/Off). G08 (2.0) also lists **DST**; G12 omits it ⚠️. | G12 p.41, G08 p.49 | ✅ |
| **Sort By** | **First** ⚠️ / Last (values ✅ owner photo `settings-14-sort-by.jpg`, inline blue; default not proven) | contacts sort; caption "Sort Contacts By First Name" | G12 p.45 | ✅ if Contacts is built, otherwise hide |
| **Language** | list | Panel: no tile; faded greetings in several scripts drift at different sizes and opacities around a bright "Welcome" (animated) (✅ owner photo `settings-15-language.jpg`) | G12 p.8 | ⚠️ owner: English-only is likely. Keep the item; localisation is a big cost. |
| **Legal** | — | legal text screen. Position ✅ owner photo `settings-16-legal.jpg` (between Language and Reset Settings); panel = large Apple logo (no tile) + "Apple Inc.". Text content ⚠️ | MM7 ch02s19, OWN | ✅ show our own licence text (GPL notice belongs here) |
| **Reset Settings** | — | "choose Reset" (G12 p.9). G12 p.51: "Choose the left item (Reset) and select a language." MM7: "a choice: Cancel or Reset". Reset is followed by the language picker. Panel caption "Reset All Settings" (✅ owner photo `settings-17-reset-settings.jpg`); the confirm screen is not photographed ⚠️. | G12 p.9, p.51, OWN | ✅ |

Developer access to Rockbox menus stays hidden (SPEC).

### 7.1 Settings split screen: right panel (✅ owner photos `settings-01-about.jpg` … `settings-17-reset-settings.jpg`)
Layout (≈, from straight-on photos): the list takes the left half (x 0–159) under the header, 25 px rows, scrollbar at the column's right edge. The panel (x 160–319, full height, no divider) is a dark navy/slate gradient. At the top, the **item name** centred in white bold (≈ list size, centre y≈25). In the middle, a **dark rounded-square icon tile** (≈76×76 px at y≈80–156, centred at x≈240) with a faint **mirror reflection** below it (≈20 px). At the bottom, a **caption** centred in white (≈y 210, wraps to 2–3 lines). The panel updates as the selection moves.

| Row | Tile icon | Caption (as photographed) |
|---|---|---|
| About | (no tile) device name at the top, glossy Apple logo, capacity capsule bar | "128 GB Free" |
| Shuffle | two parallel right arrows | "Play Music Library in Sequence" (Off) |
| Repeat | looping arrow | "Repeat Current Playlist" (All) |
| Main Menu | three list rows with chevrons | "Customize Main Menu" |
| Music Menu | same list icon | "Customize Music Menu" |
| Volume Limit | speaker + vertical level bar | "Set or Lock Volume Limit" |
| Backlight | pie timer | current value, "10 Seconds" |
| Brightness | sun | current value, "0%" |
| Audiobooks | open book | "Normal Playback" |
| EQ | 8 rising bars (wide tile) | current preset, "Spoken Word" |
| Sound Check | speaker | "Use Original Volume Level" (Off) |
| Clicker | iPod with sound waves | "Play Sound Through Headphones and Speaker" (On) |
| Date & Time | calendar page "Oct" / "06" | weekday, "Tuesday" |
| Sort By | contacts book | "Sort Contacts By First Name" |
| Language | (no tile) drifting multilingual "Welcome" words | — |
| Legal | (no tile) large Apple logo | "Apple Inc." |
| Reset Settings | circular arrow | "Reset All Settings" |

Captions for the other values (Shuffle Songs/Albums, Repeat Off/One, Sound Check On, Clicker Off, Sort By Last) are not photographed ⚠️. Our build must use **original icons** and no Apple logo (CLAUDE.md rule 2), so About and Legal need our own glyph.

## 8. System screens, alerts and modes

| Screen | 2.0.5 content | Source | Feasibility |
|---|---|---|---|
| USB connected | Dark header titled "iPod" (or the device name, e.g. "iPod classic"), or the **clock centred** when Time in Title is on, with a light battery outline and charging glyph. Slate-to-navy gradient. Glossy orange disc. "**Connected**" (bold) / "**Eject before disconnecting.**" (regular, with the period). ✅ Glyph, two stock variants (✅ owner, 2026-10-06): a black **30-pin dock-connector pictogram** while there is **no data transfer** (owner photo `usb-connected-b.jpg`, photo 1226; also Apple's `usb.png`), and **two black sync arrows spinning** while there **is** data I/O (owner photo `usb-connected.jpg`, photo 1165). | IMG pdf160 obj643, G12 p.10, OWN, owner | ✅ (SPEC) |
| Syncing | "**Synchronizing**" / "**Do not disconnect**" (sync-arrows glyph). The text also names "**Sync in Progress**" (G12 p.48). | IMG obj644, G12 p.48 | ✅ maps to USB MSC "busy" |
| Ejected | "**Ejecting**" / "**You may now disconnect.**"; then "**OK to Disconnect**" with a progress bar | IMG obj658/659 | ✅ |
| Charging | Header "**Charging**", big battery with bolt. "**Charged**" (plug glyph variant). The guide says the main menu or the large battery means it's safe to disconnect. | IMG obj657/667/668, G12 p.11–12 | ✅ |
| Very low battery | "**Charging, Please Wait...**" / "**Connect to Power**" (warning triangle on an empty battery, no header) | IMG obj669/670, G12 p.12, p.49 | ✅ |
| Restore needed | "**Use iTunes to restore**" | G12 p.50 | adapt: bootloader error screen in the same style |
| Genius alerts | "Connect to iTunes to activate Genius"; "Genius is not available for the selected song" | G12 p.50 | ⛔ (Genius skipped) |
| Lock (Screen Lock) | picture of a lock; Center to enter the combination | G12 p.43–44 | ✅ |
| Updating Library | ⚠️ No guide mention. SPEC wants a small non-blocking row. Stock behaviour after sync is unverified. | SPEC | ✅ |
| Disk mode | Reset, then hold **Center + Play/Pause** "until you see the disk mode screen"; when connected shows "Do not disconnect" | [Apple Support 101325](https://support.apple.com/en-us/101325) | ✅ (bootloader/USB) |
| Diagnostic mode | ⚠️ Reset, then hold **Center + Previous** (community knowledge, no Apple source) | — | skip (System debug menu behind dev combo) |
| Sleep | Hold Play/Pause; auto-off "after a few minutes" idle when not playing (MM7 ch01s20) | G12 p.6 | ✅ |

## 9. Disagreements with SPEC and open questions
1. **Now Playing order.** G12's table has Scrubber → Genius → **Shuffle → Rating** → Lyrics. G08's text and the owner have **Rating → Shuffle**. We keep the owner's order and flag it.
2. **Lyrics step** is stock (every guide) but missing from SPEC. Recommend adding it as the last step, shown only when lyrics exist.
3. **Audiobook selection.** Stock: choosing a book lists **Resume** plus chapters (G12 p.33). ✅ Resolved: owner photo `audiobook-open-chapters-b.jpg` confirms the list with Resume first, and SPEC now follows it (owner decision 2026-10-06).
4. **Quick-scroll symbols.** G12 (2.0.5 era) puts symbol/number items **after Z**; G07/G08/G5 put them **before A**. ✅ Resolved: after Z on 2.0.5 (owner photo `quickscroll-123-b.jpg`), and the overlay glyph for them is a staggered "123", not "#". Check `sortkey.c`.
5. **Settings tree.** ✅ Resolved: Legal confirmed (owner photo `settings-16-legal.jpg`) and added to SPEC.
6. **Clicker.** Stock is On/Off with sound through earphones and speaker. SPEC's On/Off haptic matches.
7. **Genius / Genius Mixes / Games / Videos / Photos / Voice Memos / Calendars**: owner decisions (§3, §6).
8. **About page layout**, **Backlight values**, **Extras order**, **Settings order**, **Genius Mixes position** and the **hold-Center menu** are ✅ resolved by the owner photos (§3, §5.4, §6, §7). Video 2 (`owner/VIDEO-2.md`) resolves the **panel drift**, **Clocks reveal**, **Cover Flow entry and look**, the **Audiobooks book list**, **Genius Mixes with no mixes** and the **Calendars month view**. Still open ⚠️: the **Main Menu and Music Menu checklist screens** (full option lists), the **EQ preset list/order**, the Now Playing cycle order on the device, and the rest of `docs/reference/7g/owner/INDEX.md` § "Still needed".
9. **Photo vs SPEC owner decisions** (kept as the owner's version in SPEC, flagged for the owner): rating-mode dots are small **black** in the photo, SPEC (owner) says grey; the Settings › Clicker caption reads "Play Sound Through Headphones and Speaker" while our Clicker is haptic On/Off (owner); Genius Mixes is in the stock Music menu while Genius is skipped (§5.6); Videos and Photos stay in the stock main menu ("No Videos") while SPEC hides them; the Clocks reveal (owner) vs the photographed panel not being identical to the Clocks page. **Resolved by video 2:** the device keeps the map fixed and pushes everything else, with no expand; the owner said "match the real 7G exactly", so the spec follows the measurement.
