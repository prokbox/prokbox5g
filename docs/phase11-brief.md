# Brief: Phase 11, Stock 2.0.5 completeness, plus Quick Wins part 2 (#30, #31; QW2 from roadmap row 7a)

*Orchestrator brief, drafted 2026-10-09; **Revision 1** (brief review) 2026-10-09, see the table at the end. Branch `p11` is cut from `main` **after** `p6` and `p8a-int` have both merged. References are anchored by **function or symbol name**; a line number, where given, is a hint tagged with its source tree (`p6:` = `b22a1d9e7b`, `8a:` = `cb8123f95e`) and moves after the merge; an untagged line number is `p6:`. Process: `docs/process.md` (brief template, device-reality checklist 1–13, lean rules 1–11). Anything still open is frozen as ⚠️ with a default (§10) and goes to the owner round. The brief is not revised mid-phase for owner answers. Issue numbers #30/#31 come from `docs/owner-questions.md`; the orchestrator confirms them on Forgejo.*

**Goal.** Fill in the stock 2.0.5 screens that are still stubs or missing, so the device that goes into P10 testing behaves like a 7G everywhere a user can reach:
- Search, the hold-SELECT menu and a real On-The-Go;
- every Settings sub-screen;
- the system screens 8a left out;
- the podcast behaviours.

Alongside that, **QW2** moves Now Playing art and its reflection to build time, so a track change costs one read and one blit.

---

## 0. What already exists (reuse it; do not rebuild)

| Need | Already there | P11 does |
|---|---|---|
| Settings tree, tiles, captions | `apps/ipodui/screens/settings.c` has all 17 rows, tile panels, blue inline values (Shuffle/Repeat/Sound Check/Clicker/Sort By), the generic split value list `choice_screen` (Backlight), and the Audiobooks light prompt (`speed_screen`, `sp_draw_control()`). The captions are already LANG strings, including `LANG_IPODUI_CAP_MAIN_MENU`, `_MUSIC_MENU`, `_VOLUME_LIMIT`, `_LEGAL` and `_RESET` | Re-point the 9 `STUB()` rows (Main Menu, Music Menu, Volume Limit, Brightness, EQ, Date & Time, Language, Legal, Reset; p6 `items[]`) and the About row to real screens. Reuse `choice_screen` for Language, EQ and Time Zone. The contract commit moves the segmented control (`sp_*`) into a shared `setwidgets.c` (§4.8) for the Reset prompt. Sort By becomes a saved setting (today it's the RAM `sort_last`) |
| About identity | `screens/stub.c` `ui_about_screen` (`.name "about"`). Devmode arms on `top->ops == &ui_about_screen` (8a Rev 1 M6, hook H3) | The real About screen **replaces the stub under the same symbol and name**. The stub's definition moves out of `stub.c` |
| Capacity, device name | `screens/panels.c`: `free_refresh()` (`volume_size()`, FSInfo in RAM, 30 s TTL, sim-pinned), the GB formatter, the 130 × 12 capsule painter, `ui_device_name()` | The contract exports the formatter and the capsule (§4.8). About uses them |
| Dark full-screen chrome | `header.c` `ui_header_draw_dark()` (8a); the USB gradient and disc in `usbscreen.c` / `usbscreen_data.h` | About, Brightness, Volume Limit, Charging/Charged, Connect to Power and OK to Disconnect all use them |
| Time in Title | `header.c` alternation, `ui_header_clock()` (12/24 h from `global_settings.timeformat`), and `ui_header_set_time_in_title()` / `ui_header_time_in_title()`, **a RAM stub** (`tt.off`, zero-initialised, so the clock is **on** until something calls the setter) | Back it with the new `ipodui_time_in_title` setting, **default Off** (§1.2 T1). Shell start calls `ui_header_set_time_in_title(global_settings.ipodui_time_in_title)` before the first header draw |
| Date & Time panel | `cap_date()` (weekday) and `UI_TILE_CALENDAR` | Add the sub-menu and its screens |
| Menus | `menu.c`: a per-item `visible()` hook, `labels_init()` with `*_lang[]` tables, static menu slots (`MENU_SLOTS`, P6 fix), and Search as `M_STUB` | The checklists drive `visible()`. Search points at `ui_search_screen` |
| On-The-Go | `screens/playlists.c` has the On-The-Go row and `ui_otg_add()`. **But OTG today *is* the current playlist**, so adding to it changes what plays, which isn't stock. `library.c` `play_view()` shows how to build a queue from paths (`playlist_insert_context_*`, capped at `max_playlist_size`) | Make it a separate store (§3.2) and add Save/Clear/Remove. Playing it builds a queue from its paths and **never** hands the store file to the playlist engine (Rev 1 H-2) |
| Hold SELECT | `browse.c` `browse_input()`: holding a track adds it to OTG with an inline confirm row. `nowplaying.c` input handler, the "hold SELECT: nothing until P11" branch (p6 :2145) | Both open the hold menu instead |
| Browse Artist/Album | `np_track.tc_idx` / `db_gen` (`np_playback.h:97`). Library filters match the **representative** entry's tag (`library.h`), so the playing track's master index works directly as `f.album` / `f.artist` | No new library code |
| Podcasts | `lib_unplayed()` (shows and episodes), `O_NEWEST` (episodes ordered by `tag_mtime` descending), `lib_progress()`, the dot (`ui_rowart_dot`), per-episode tagcache resume (`tagcache_ui_resumes()`). A **non-stock** glossy progress bar on episode rows (`audiobooks.c` `ui_book_hint_draw`) | Mostly verify, drop the bar (⚠️ F-P1) and fix the dot semantics |
| Volume limit | Upstream `global_settings.volume_limit` (`settings_list.c`), clamped in `firmware/sound.c` `sound_set_volume()` and by `np_playback.c` `adjust_volume()`. The clamp applies only when the volume is next **set** | UI plus a combination, plus an immediate `sound_set_volume(global_status.volume)` after every limit write and an EQ precut (Rev 1 M-5). No new enforcement path |
| Shared paths, 12/24 h, brightness, EQ, RTC | Upstream `timeformat`, `brightness` / `backlight_set_brightness()`, the `eq_*` settings and DSP, `set_time()` | The shell UI maps stock values onto them |
| Low battery / charging | `apps/misc.c` `clean_shutdown()` already `DEBUGF`s the low/empty shutdown messages under the shell (8a :414–429). **`charging_splash()` is dead in the shell:** its only callers are `misc.c` `list_stop_handler()` (reached from upstream `menu.c`/`tree.c`/`onplay.c` and `BUTTON_MULTIMEDIA_STOP`, none on a shell path) and `shortcuts.c`. Hold-PLAY on the charger goes through `ui_main.c` `global_hold()` → `charging_sleep_prepare()` (8a :518–576): pause, save resume and settings, backlight off on the release | Two guarded low/empty calls in `misc.c`; the Charging screen hooks into `ui_main.c` (Rev 1 H-1). `charging_splash()` is left untouched |
| Eject | `usb_storage.c` SCSI START STOP UNIT handler sets `ejected[lun]` on eject (8a :1049) and posts no event | One guarded read-only getter |
| NP art | `np_art_render()`, `np_placeholder_render_glyph()`, the reflection as `np-reflection.md` specifies (limit 205, feather, blur; `np_draw.h:67–83`), the 142-box fitted bundle (`artfmt.h` "ART" bundles), and the `NP_BLK_ART` 40,328 B area | QW2 runs the **same** render on the builder thread and stores the result |
| Sim pins | 8a's `hold_switch`, `backlight_expire`, `sim_clock`, `clock_format`, `battery`, `charger`, `usb_connect`, `disk_active`; P6's `wait_pos` and `mem mark` | Reuse them. New commands are listed in §8 |

**A latent bug P11 must not repeat.** `browse.c` `browse_enter()`, `audiobooks.c` (book page enter), `playlists.c` (enter) and `settings.c` `choice_screen` enter each `core_alloc` a screen state on enter (p6 :301, :180, :131, :373). If audio is playing **or paused**, that shrinks the buffer, and a paused queue stops for good: the same failure P6's fix round found in `menu.c`. P11 adds about a dozen screens, so it adds a **screen arena** (§4.1) and moves the four states above into it.

---

## 1. Owner pre-flight

**Already decided (not reopened):**
- Settings order and the 17 rows; Backlight values; the Audiobooks prompt; About is 3 dark pages turned with NEXT/PREV, the wheel does nothing, MENU returns.
- The hold menu from Now Playing is a full-screen list (Add to On-The-Go / Browse Album / Browse Artist / Cancel).
- The Search empty and results looks (`search-empty-b.jpg`, `search-results-b.jpg`).
- Legal carries the GPL/font/not-affiliated text.
- Settings → Language is real but lists English only (COMP-6). Reset shows the language picker afterwards (COMP-7).
- Sub-screens nobody photographed default to the split value-list pattern (NAV-1).
- Podcasts keep the queue counter.
- **Audiobooks have no played/unplayed dots; podcasts do** (P6 Rev 2, G12 p.32).
- The device name is a stored setting.
- Battery goal: match or beat Apple firmware 1.3.

**Captures used:** `search-*-b.jpg`, `hold-select-popup-np-b.jpg`, `settings-0x/1x-*.jpg`, `about-1..3.jpg`, `brightness.jpg`, `usb-connected*.jpg`, `extras-screenlock-new-combination.jpg` (the combination roller look), `np-normal-b.jpg`, `np-volume-b.jpg`.

### 1.1 Owner pre-flight question list (ask in one round; each has a default that is frozen in §10)

Each group is one trip through the owner's 7G. The capture that settles it is in brackets.

**A. Search** *[video: Music › Search, enter "h", then "a", then PREV, then NEXT; scroll the whole result list; MENU; SELECT on the top row; MENU again]*
- **S1.** Is a result a match when **a word starts with** the typed text, or when it appears **anywhere**? The guide says "containing", but the photo shows "Haiku" and "Halcyon" at the top for "h", which a substring match over 432 songs almost certainly wouldn't. *Default: word-prefix.*
- **S2.** Are results one list in alphabetical order, or grouped by type (artists, then albums, then songs, …)? Where do playlists go? *Default: one alphabetical list, playlists last.*
- **S3.** What is the letter wheel's character set? Digits after Z? Does it wrap? *Default: A–Z then 0–9; it clamps at both ends (no wrap); NEXT adds a space.*
- **S4.** After MENU shows the full results, does SELECT on the top "Search" row bring the letter strip back, and does a second MENU leave Search? *Default: yes to both, per G12.*
- **S6.** (Ours, not stock: stock never caps.) When the results hit the memory cap, how is that shown? *Default: a last gray row "More..." (no chevron); SELECT on it brings the letter strip back so the user types more (⚠️ S6).*

**B. Hold menu and On-The-Go** *[stills: hold SELECT on a song in Songs, an album in Albums, an artist in Artists; video: Music › Playlists › On-The-Go with 2 songs, scroll to the end, Save Playlist, then Clear Playlist (back out of Clear)]*
- **H1** (COMP-9). Does the item set change by context? *Default: songs and Now Playing get Add + Browse Album + Browse Artist + Cancel; album, artist, genre, composer and playlist rows get Add + Cancel; rows inside On-The-Go get Remove + Browse Album + Browse Artist + Cancel.*
- **H1b.** Browse Artist on a compilation track: the album artist or the track artist? *Default: the album artist (⚠️ H7). Books and podcasts never get Browse Album/Artist.*
- **H2.** After Browse Album/Artist from Now Playing, where does MENU go? *Default: Now Playing and the hold menu leave the stack; the browse list is pushed over the screen Now Playing was opened from, and MENU returns there. This also avoids two Now Playings on the 8-deep stack.*
- **H3.** Confirm On-The-Go is **separate** from what's playing: adding doesn't change the queue and never starts playback. *Default: separate, never starts playback (stock).*
- **H4.** On-The-Go list layout: two-line or single-line rows? Are "Save Playlist" and "Clear Playlist" at the top or the end? *Default: two-line (title/artist) like Songs; Save Playlist then Clear Playlist at the end.*

**C. Checklists and Volume Limit** *[video: Settings › Main Menu to the end; Music Menu to the end; Volume Limit through each screen without setting a code]*
- **C1** (COMP-1). The full Main Menu and Music Menu lists, Reset Menu, and how a checkmark looks. *Default: offer only items we ship (§3.4); a blue checkmark right-aligned on checked rows; Reset Menu as the last row of both.*
- **C2** (COMP-3). The Volume Limit screens. *Default: G12's flow (§3.5) on a dark full-screen bar screen; the combination roller copies the Screen Lock photo.*
- **C3.** A forgotten combination: stock needs a restore. Ours? *Default: Reset Settings keeps the limit and the code; the only recovery is editing `config.cfg` over USB (never shown on the device). The code is stored in plain text there.*

**D. About, Date & Time, Reset** *[stills: Settings › Date & Time list, the Date, Time and Time Zone screens; the Reset Settings confirm (back out); Legal page 1]*
- **D1.** About page 3: Serial Number and Model have no reliable source on our unit. *Default: two hidden text settings (`ipodui_serial`, `ipodui_model`, set in `config.cfg`); a row with an empty value is hidden. Version reads "ProkBox 0.11" from a build define (working name; one string, #43).*
- **D2.** About page 2: show Videos/Photos/Games/Contacts as 0 (stock look), or hide them? What about "iTunes U"? *Default: show 0 for Videos, Photos, Games, Contacts; leave out iTunes U.*
- **D3.** The Reset Settings confirm: its look, and what survives. *Default: a light prompt like Audiobooks with a segmented **Reset | Cancel** (G12: "the left item (Reset)"), Cancel preselected. Kept: device name, volume limit and code, serial/model, statistics, resume points, books.dat. Then the language picker, then the main menu.*
- **D4** (COMP-4). Time Zone: region then city, shared with Clocks? A DST row? *Default: two levels, one shared table (~80 cities); no DST row (G12 has none).*
- **D5.** The Date and Time edit screens. *Default: the dark roller from the combination screen, one box per field; SELECT moves to the next field; MENU saves.*
- **T1.** Factory default of Time in Title. *Default (owner, §1.2): **Off**; the user can turn it on. Reset Settings restores Off. Header goldens that show the clock only because of the old default are recaptured once (§7 criterion 8).*

**E. System screens** *[video: put the 7G to sleep while it's on a wall charger; eject from Finder while connected]*
- **E1.** When does the big "Charging"/"Charged" battery show? *Default: on a PLAY-hold (sleep) while on power, because this hardware can't power off while charging: `ui_main.c` `global_hold()` saves as today (`charging_sleep_prepare()`) and pushes the Charging screen. It stays until unplug, USB or a key; the backlight timeout, the unlit face and LCD sleep then run as for any screen.*
- **E2.** After a Finder eject, stock returns to its menu while still plugged in. Leaving USB mode on eject is a USB-core change. *Default: stay on the USB screen, showing "OK to Disconnect", until the cable is pulled. Exit-on-eject is deferred to a separate USB mini-phase.*

**F. Podcasts** *[still: Podcasts › a show's episode list, with one episode half-played]*
- **P1** (NAV-4). Episode row layout; is there a half-played marker; does the dot clear on first play or on finishing? *Default: drop our non-stock progress bar. Full blue dot = never started; no dot once started (today's `lib_unplayed()` rule); no half dot.*
- **P2.** Episode order: release date vs our file-mtime proxy (Finder keeps the download mtime). *Default: keep `tag_mtime` newest-first.*

**G. EQ** *[still: Settings › EQ, top to bottom]*
- **COMP-2.** The preset list and its spelling. *Default: Off, then the 22 iTunes presets in the catalogue's order, with the published iTunes 10-band gains.*

**H. QW2**
- **Q1.** The coordinator's art box (x 10–145, y 32–167 left / 42–162 right) disagrees with the spec and the code (x 10–147, y 34–175 / 43–167; `now-playing.md`, `np_draw.h:60–65`). *Default: keep the spec (measured from photos). QW2 overlays both on the rectified photo 1203 and reports which fits; the owner picks at the round.*

---

### 1.2 Owner answers (2026-10-09)
**All defaults accepted for the first run and device testing; the owner may revisit some of them later** (S1–S4, H1–H4, C1–C3, D1–D5, E1–E2, P1–P2, COMP-2, Q1). Make each one easy to change **in the source**, never at a runtime cost (owner):
- **Prefer compile-time constants, `#define`s and `const` tables.** They cost nothing on the device, and a later change is a one-line edit.
- Add a runtime setting only where stock 2.0.5 itself has one.
- Never keep a choice switchable at runtime just in case: no extra branches in hot paths, no extra RAM or reads.
- If keeping a choice easy to change would cost performance, battery or RAM, hard-code it and accept more effort later. One change:
- **T1: Time in Title defaults to Off** (the user can turn it on). Reset Settings restores Off.
  - This changes the factory default the shell build ships with, and every header golden that shows the clock because of that default. Scripts that need the clock turn it on (or pin it) explicitly.
  - The goldens are recaptured once, in this phase's integration, with each change explained.

- **QW2: Now Playing keeps plain white around non-square art** (owner, 2026-10-09). There is no blurred fill on Now Playing; only list thumbnails get it (P6).

## 2. Scope

**In (P11):**
1. **Search** (Music › Search, and a main-menu item through the checklist): letter strip, live results with type icons, NEXT = space, PREV = delete, MENU = full results.
2. **Hold-SELECT menu** from Now Playing and from every list; **On-The-Go** as its own store: add, remove, view, play (a queue built from its paths, never the store file itself), Save Playlist ("New Playlist N" → `/Playlists`), Clear Playlist (with confirm).
3. **Settings › Main Menu / Music Menu** checklists with Reset Menu, driving `menu.c`.
4. **Volume Limit** with a combination (a shared roller widget that P12 Screen Lock reuses), plus the limit triangle on Now Playing's volume bar.
5. **System screens not done in 8a:** Charging / Charged (pushed by the shell's hold-PLAY-on-charger sleep in `ui_main.c`; `charging_splash()` is dead in the shell and untouched), Connect to Power (low/empty battery at shutdown), OK to Disconnect (after eject, inside the USB screen). 8a already did USB Connected idle and I/O (`usbscreen.c`), so that is **not** redone.
6. **About** (3 pages), **Date & Time** (Date, Time, Time Zone, 24 Hour Clock, Time in Title), **Legal**, **Reset Settings**, **Language** (English-only list, also used after Reset), **Brightness** (photographed, cheap), **EQ** (preset list → Rockbox EQ). Brightness and EQ are not in the roadmap row, but they are the last two Settings stubs; cut them first if size is tight.
7. **Podcast behaviours:** dots, newest-first, per-episode resume, verified with scripts; the non-stock bar dropped (⚠️ F-P1).
8. **Screen arena** (§4.1), and the four existing `core_alloc` screen states moved into it.

**In (QW2, a parallel track):**
1. Now Playing art and reflection **pre-rendered by the art builder** into the NP bundle, so a track change is one read plus one blit.
2. Pre-rendered **placeholders** (the note, and the book glyph for coverless books) with their reflection, shown at once until real art swaps in.
3. Art geometry checked against the spec (⚠️ Q1).
4. Speaker glyph nits (`now-playing.md` "Speaker glyphs: exact direction").

**Out (named so nobody builds it):**
- Cover Flow (P7; the checklist rows stay hidden). Genius / Genius Mixes and every Genius item in the hold menu.
- All of Extras (P12), including Screen Lock. P11 only builds the roller it will reuse.
- Contacts (Sort By is stored but does nothing yet). Videos/Photos.
- Leaving USB mode on eject (⚠️ E2; a USB-core mini-phase).
- iAP compile-out. Real multiple languages. Language's animated "Welcome" panel and the Date & Time calendar-page polish (8b).
- Metallic tiles and gel polish (8b). The remaining-time minus-sign gap (8b).
- Podcast Now Playing description page (NP-2: no). A bootloader low-battery screen. "Use iTunes to restore".
- EQ curve tuning by ear (P10/8b). Search inside Cover Flow.
- A native panic screen (8a D5 stands).

---

## 3. Design

### 3.1 Search (L: core + library; U1: screen)
- **Matching** (pure `search_core.c`, host-tested):
  - The query is folded once with the `ui_sortkey()` fold rules: lower case, Latin accents folded, whitespace collapsed; "The " is **not** dropped.
  - A haystack byte is folded through a 256-entry table. UTF-8 lead bytes go through the same 2-byte fold `sortkey.c` uses.
  - **Word-prefix match** (⚠️ S1): the query must match at position 0 or right after a non-alphanumeric byte.
  - A multi-word query ("ha ku") needs every word to prefix-match some word, in order.
- **What is searched** (G12: "songs, playlists, album titles, artist names, audio podcasts, and audiobooks"):

  | Kind | Source | Row label / second line |
  |---|---|---|
  | Artist | the `LIB_ARTISTS` rows (precompute ROWS) | artist / – |
  | Album | the `LIB_ALBUMS` rows | album / album artist |
  | Song | **title tag walk** (`tagcache_ui_walk(tag_title)`), music class | title / artist |
  | Audiobook | the `LIB_AUDIOBOOKS` rows (books, not chapters) | book / author |
  | Podcast | title walk, podcast class (episodes) | episode / show |
  | Playlist | `/Playlists` names, read once at Search entry (≤ 64, as `playlists.c`) | name / – |

- **Group-row source buffer (Rev 1 M-3).** The Artist, Album and Audiobook kinds need their precompute ROWS (≈ 34 KB at 5k albums / 3k artists / 300 books). At Search entry L reads those sections straight from the precompute file (≤ 4 section reads, lean rule 11) into **slack of the existing view pool**, as one pinned pseudo-view owned by Search and freed on leave. **Never** `lib_open(LIB_ALBUMS…)` on the whole library at entry (that builds views and may grow the pool). If the slack can't hold them without growing the pool while `audio_status() != 0`, the group kinds that don't fit are skipped and the result is marked capped (the "More..." row below). Shares with: the other views in the pool (Search is one screen; while it is live, the browse views under it keep theirs).
- **Results.**
  - A library view `LIB_SEARCH` in the **existing view pool**. A row is `SRCH_ROW(kind, idx)` in 4 B; playlists are kept in the screen's arena state, not in the view.
  - Order (⚠️ S2): one list, alphabetical by sort key (each source is already in sortkey order, so this is a k-way merge on 8-byte key prefixes), with playlists appended.
  - **Only one result view is alive** at a time; Search never holds a previous view for undo.
  - The pool never grows while `audio_status()` is non-zero (P6 Rev 1 rule). Results are then capped at what fits, traced `search cap=<n>`.
  - **A capped result is shown, never silently truncated** (⚠️ S6): the alphabetical tail is missing, so the list ends with a gray single-line row "More..." (`LANG_IPODUI_SEARCH_MORE`, no chevron, no icon). SELECT on it brings the letter strip back. Refining usually removes the cap.
- **Incremental.**
  - A query that extends the previous one (a letter or space appended) **refines in place**: the result set can only shrink, so L compacts the same view's row array (keeping matches, in order), O(results), with no second view and no allocation.
  - PREV (delete) re-runs from the full sources into the same view.
  - A search runs only when a character is entered or deleted, never on a wheel step.
- **Lean-rule exception, stated.** Search is browse-time work (lean rule 11 says precompute at sync time). It is the sanctioned exception because every haystack string is **already in RAM** (the tagcache RAM cache and the ROWS loaded once at entry): **0 storage reads per keystroke**, the CPU cost is bounded and measured (§6 item 3), and it runs only on a character entered or deleted.
- **Yield and abort.** Yield every 4,096 entries. A new key cancels and restarts. A `tagcache_ui_generation()` change aborts: RAM cache pointers die at a yield.
- **Library state.** While `LIB_UPDATING`, the result row shows "Updating Library..." (the Music-menu status string) and no rows.
- **Screen** (`screens/search.c`, full-width):
  - Top row: two-line, gray magnifier tile, "Search" / "No Search Results" or "Results for: <q>".
  - Result rows: two-line with a gray type icon. COMP-8 default: one icon per kind, original art, in `rowglyphs.py`.
  - A dark rounded strip at y ≈ 190–215 holds the white text field (query plus cursor) and the letters (⚠️ S3), with the current letter in a blue rounded box. The letters scroll so the box stays visible, with "..." at the right end.
  - SELECT enters a letter; NEXT adds a space; PREV deletes; MENU hides the strip (full list); SELECT on the top row brings it back; a second MENU pops (⚠️ S4). SELECT on a result opens it as `open_child()` would, or plays a song or episode from the result.
  - **Drawing:** a wheel step redraws and pushes only the strip band (320 × 26). A new result set redraws the list body once; the written reason is that the content changed.
### 3.2 On-The-Go store (L) and its screens (U1)
- **Files** (hidden; the format is documented for #53):
  - `/.rockbox/ipodui/otg.m3u8`: UTF-8 paths, one per line, in plain `.m3u8` form (so it can be read over USB and Save is a byte copy). **It is never handed to the playlist engine** (Rev 1 H-2): the engine keeps seek offsets into its file, re-reads it, and the resume state names it, so a Remove, Clear or Save on the store would corrupt playback or resume.
  - `/.rockbox/ipodui/otg.idx`: a header plus 8 B per line (`master idx`, `check`).
- **Identity vs validity.**
  - *Identity* of an entry = its path line (authoritative across rebuilds).
  - *Validity* of the whole idx = `otg_idx_hdr.commitid == tagcache_ui_commitid()`. A different commit id (a database commit since the idx was written) triggers a resolve on the next open. Rev 1 L-3.
  - *Validity* of one cached master index = `check` == `otg_check(idx)`, a hash of the entry's RAM title seek string and `tag_length`. That needs no tagcache edit and catches a record that moved within one commit id.
  - A mismatch means unresolved: the row shows the path's file name until it's resolved.
  - **Append order:** an add writes the list lines, then the idx records, then the idx header (count, sum, commitid): **2 writes to otg.idx** per append (records, then header). A cut between them leaves count < lines, which the reader treats as "re-resolve the tail".
- **Add** (UI thread, user action):
  - A song: `lib_path()` (as today), then one append to each file.
  - A group (album, artist, genre, composer, playlist row): its tracks through one pinned `lib_scratch_open()` search, paths read in master-index order, with lines buffered and written in ≤ 4 KB appends (lean rule 11).
  - Capped at `OTG_MAX` 2,000 lines in total, traced `otg add n=<k> reads=<r> writes=<w>`.
  - **Never touches the current playlist** (⚠️ H3).
- **Remove:** rewrite both files through `.tmp` + rename (≤ 2,000 lines, a user action).
- **Clear:** truncate both, after a confirm. The confirm reuses the Reset prompt's segmented control: **Clear | Cancel**.
- **Save:**
  - Find the first free `N` for `/Playlists/New Playlist N.m3u8` (one `opendir` of `/Playlists`).
  - Copy `otg.m3u8` in bulk (32 KB reads into the **list strip**, see the sharing rule below), write `.tmp`, rename, then clear.
  - G12: the saved list is then not editable as OTG.
- **Play** (Rev 1 H-2): `otg_play(pos)` builds a **fresh current playlist from the store's paths**, the way `library.c` `play_view()` builds one: read `otg.m3u8` in 32 KB chunks into the list strip, `playlist_insert_context_add()` each line, capped at `OTG_MAX` (2,000, = `max_playlist_size`), then start at `pos`. The playlist engine and the resume state then refer only to their own control file, so editing, clearing or saving the store never touches what plays. Rejected: copying the store to a second play file and `playlist_create()`-ing it, because that writes up to 180 KB to flash on every play.
- **Resolve** (Rev 1 M-2): the first time the OTG list opens with a stale commit id or unresolved rows, **one bulk pass** reads the filename tag file once in 32 KB chunks and hashes each name against an open-addressed set of the OTG path CRCs (2,000 × 8 B = 16 KB), boosted, with yields. **One pass, never passes of 512** (each would re-read the ≈ 3 MB file). It happens only after a database commit.
- **Where the big buffers live** (Rev 1 M-2): the resolve's 32 KB read buffer + 16 KB set (48 KB) and Save's/Play's 32 KB copy buffer go in the **list strip** (180,000 B, `docs/hardware.md`), not in the 8 KB screen arena (which can hold neither). **Sharing rule:** the strip is claimed only while nothing draws from it (the resolve runs before the OTG list's first draw; Save and Play run between frames, with no glide in flight); while claimed, nothing is drawn from the strip, and after release the list **redraws in full** (written reason: the strip's row ring was overwritten). A tagcache-thread alternative for the resolve (handing off through a temp file) is a ⚠️ for the end review if the device times it badly.
- **Screens:**
  - Playlists keeps "On-The-Go" first.
  - The OTG list is a `LIB_OTG` view (rows = resolved master indices; unresolved rows carry the line number with a flag bit), with Save Playlist and Clear Playlist at the end (⚠️ H4).
  - SELECT on a track plays OTG from it (`otg_play()`, above). Holding a row opens the hold menu with Remove.

### 3.3 Hold-SELECT menu (U1; one hook in `nowplaying.c` by Q)
- A full-screen **ordinary list** under the normal header (`hold-select-popup-np-b.jpg`): no overlay, no chevrons, Cancel last.
- Items come from the pure `ui_hold_items()` (⚠️ H1).
- It's pushed with `UI_T_PUSH`. Cancel or MENU pops.
- **The SELECT release that opened it is ignored** (Rev 1 L-6): the hold menu opens on the long-press while SELECT is still down, so its enter() sets `swallow_release`, and the first SELECT release is dropped (otherwise it would act on the first row, Add).
- **Books and podcasts** (the playing track's class, or the held row's view) never get Browse Album / Browse Artist; `ui_hold_items()` drops both bits (a book is browsed through Audiobooks, an episode through its show).
- **Compilations** (⚠️ H7): Browse Artist uses the track's **album artist**, not the track artist. That is what `f.artist = tc_idx - 1` already gives: `LIB_ARTISTS` filters key on the album artist, else the artist (`library.h`), so a compilation track lands on its album artist (e.g. "Various Artists"). No extra code; the alternative (track artist) would need a new filter.
- **Add** confirms nothing visible: stock just returns; `UPDATE_TRACE("hold add ...")` logs it.
- **Browse Album** from Now Playing (⚠️ H2): `ui_pop()` the hold menu, `ui_pop()` Now Playing, then push `LIB_SONGS` with `f.album = tc_idx - 1`, titled with the album. **Browse Artist** pushes `LIB_ALBUMS` with `f.artist`, `UI_ALL_SONGS`. If `db_gen` is stale (a rebuild since), both stay unavailable (the items are hidden).
- **From lists:** `browse.c` replaces its hold-add (`browse_input()` `long_press && !release`) with `ui_push(&ui_hold_screen, &arg)`. The inline confirm-row code goes.
- **From Now Playing:** the `nowplaying.c` hold-SELECT branch (p6 :2145, hold SELECT with a track) calls `ui_hold_open_np(tc_idx, db_gen)`. One call; Q applies it (Q owns the file).

### 3.4 Main Menu / Music Menu checklists (U2a)
- A split value list in the `choice_screen` style (NAV-1): the panel keeps the Main Menu / Music Menu tile and caption. SELECT toggles the row's checkmark in place and saves; it doesn't pop. **Reset Menu** is the last row: it restores the default mask and puts the selection back on row 0.
- **Offered rows** (shipped screens only, ⚠️ C1), in **canonical order** (`main-menu-custom-hold.jpg`: added items take fixed slots):
  - Main Menu: Music, Playlists, Artists, Albums, Compilations, Songs, Genres, Composers, Audiobooks, Search, Podcasts, Extras, Settings, Shuffle Songs.
  - Music Menu: Playlists, Artists, Albums, Compilations, Songs, Genres, Composers, Audiobooks, Search.
  - Cover Flow, Genius, Videos, Photos and the Extras children keep reserved bits and are never offered until their phase ships.
- **Defaults:**
  - Main = Music, Podcasts, Extras, Settings, Shuffle Songs (today's menu).
  - Music = everything but Compilations, which is **off per G12** p.30. The owner's own unit has it on, and it's one tap. This changes the Music-menu goldens; T recaptures them.
- **Main menu wiring:** `menu.c` `main_items[]` gains rows for every offered item. Each row's `visible()` is `ui_mm_visible(UI_MM_x)`. Now Playing keeps `ui_now_playing_visible`. Settings is never hidden: unchecking it is refused (stock keeps Settings).
- **Panels:** Music-family items show `ui_preview_drift`; Podcasts shows `ui_preview_podcasts`; Search shows the drift.

### 3.5 Volume Limit and the roller (U2a; triangle by Q)
- **Flow** (G12 p.30–31, ⚠️ C2):
  - **No code set:** a dark full-screen titled "Volume Limit", with a volume bar showing the current limit as a small triangle. The wheel moves the limit; PLAY plays or pauses the current queue as a preview; SELECT sets it, which pushes **Enter Combination**. There, MENU accepts with no code; four digits plus SELECT set the code.
  - **Code set:** Volume Limit opens **Enter Combination** first. Wrong code: the digits reset to 0000 with a double clicker tick, and it stays locked. Right code: the limit screen, where PLAY/PAUSE accepts the change.
  - **Remove:** move the limit to max.
- **Storage:** `global_settings.volume_limit` (upstream, already enforced) and `ipodui_vlimit_code` (−1 = none, else 0–9999).
- **Apply at once** (Rev 1 M-5): upstream clamps only when a volume is next set, so after **every** `volume_limit` write (the limit screen, setting or removing the code, and Reset's restore of the kept limit) the writer calls `sound_set_volume(global_status.volume)`, which clamps the output immediately. One helper, `ui_vlimit_apply()`, in `vlimit.c`; `reset.c` calls it too.
- **Roller widget** (`roller.c`, shared with P12 Screen Lock and with Date/Time): N fields with ranges, the current field in a blue rounded box (`extras-screenlock-new-combination.jpg`). The wheel changes the field; SELECT goes to the next field (NEXT/PREV also move between fields); one band push per step.
- **The triangle on Now Playing's volume bar** (Q, `nowplaying.c` `draw_volume()`): drawn at `ui_vol_frac(volume_limit) * VOL_W / 1024` when a limit is below max.
- **`ui_vol_frac()` has one owner** (Rev 1 L-8): the **contract commit** implements it for real (not a stub) in `np_playback.c`, next to `np_pb_volume_pct()` and with the same linear `sound_min..sound_max(SOUND_VOLUME)` mapping that the NP bar's fill uses (`draw_volume()`: `np_pb_volume_pct() * VOL_W / 100`). U2a's limit screen and Q's triangle both call it; neither re-derives the mapping.

### 3.6 About, Legal, Reset, Language, Date & Time, Brightness, EQ (U2b; U2a wires the rows)
- **About** (`screens/about.c`, symbol `ui_about_screen`, `.name "about"` **unchanged**):
  - Dark header with the device name centred; on every page "X GB Used" left, our wheel logo centre, "Y GB Free" right, and page dots.
  - P1: the capsule bar with Audio/Video/Photos/Other segments, legend squares.
  - P2: two columns of counts.
  - P3: Serial Number / Model / Version (⚠️ D1).
  - NEXT/PREV **on release, with SELECT not held** slide between pages (190 ms, always a slide). The wheel does nothing. MENU pops.
  - **The devmode chord** (SELECT+PREV, 3 s) must still arm on every page, and the page must not change when it fires; a script checks this.
  - **Counts:** Songs, Podcasts (episodes) and Audio bytes come from `lib_stats()` (precompute, no I/O, §4.3). Used and free come from `free_refresh()`. "Other" = used − audio. Videos/Photos/Games/Contacts show 0 (⚠️ D2).
- **Legal:** a full-width light text page. LANG paragraphs `LANG_IPODUI_LEGAL_1..6` (Rockbox GPL v2+ notice, TeX Gyre Heros GUST licence, Source Sans OFL, not affiliated with Apple, trademark line), word-wrapped once at enter into the arena; the wheel scrolls it.
- **Reset Settings:**
  - The light prompt (⚠️ D3, `setwidgets.c`). **Reset** calls `settings_reset()`, then restores the kept list, then **targeted applies**: backlight timeout, brightness, volume, replaygain, EQ, clicker, timeformat, `ui_header_set_time_in_title()`. Then `ui_vlimit_apply()` (the kept limit, clamped at once), `settings_save()`, then the Language list, then `ui_pop_to_root()`. Time in Title returns to its default, **Off**.
  - **Never `settings_apply(true)`.** It reloads fonts and skins, which allocates; checklist item 2.
- **Language:** `choice_screen` with one row, "English" (the language's own name, `LANG_IPODUI_LANG_NAME`). SELECT keeps it.
- **Date & Time:**
  - A split value list: Date, Time, Time Zone, 24 Hour Clock (inline On/Off → `timeformat`), Time in Title (inline On/Off → `ipodui_time_in_title`).
  - Date and Time are dark roller screens (⚠️ D5). Order follows the language: English is US M/D/Y.
  - Time Zone: region list → city list (two `choice_screen`s, ⚠️ D4) from `tzcities.h` (generated by `art/tz/tzcities.py`, about 80 cities, names as LANG phrases, standard offset in minutes). **The city table is append-only** (Rev 1 L-5): a city's index is its stable id and is what `ipodui_tz_city` stores; display order (by region, then name) comes from a separate generated `const uint8_t` order array, so adding or re-sorting cities never changes a saved setting. The generator refuses to reorder or delete existing rows. Changing the city shifts the RTC by the offset difference (`set_time()`), because the RTC holds local time on Rockbox. Default city: New York (owner's clock photo shows Boston, same zone).
- **Brightness:** the dark screen from `brightness.jpg` (moon, capsule track, sun, "N%" at y ≈ 208). The wheel calls `backlight_set_brightness()` live; MENU saves. One band push per step.
- **EQ:** `choice_screen`: Off plus the 22 presets (⚠️ COMP-2). A preset writes the 10 Rockbox EQ bands at the iTunes centre frequencies from a 22 × 10 `int8` table (220 B rodata) and sets `eq_enabled` (Off → false). The caption is the preset name (replacing today's On/Off caption).
  - **Precut** (Rev 1 M-5): presets have positive gains, and `eq_precut` defaults to 0, so a preset could push the output above the volume limit and clip. Each preset also sets `eq_precut` to **its own maximum positive band gain** (0 when none is positive; same 0.1 dB units as the band gains), precomputed into a 22-entry `uint8` column of the table (22 B). Off restores precut 0.
  - **Battery:** Off is the default. U2b checks whether Rockbox's EQ skips zero-gain bands; if not, the preset activates only the bands it needs. The codec-thread load per preset is measured in the sim harness and goes to P10.

### 3.7 System screens (U2b)
- **`sysscreen.c`**: dark chrome (the USB gradient plus `ui_header_draw_dark()`). Glyphs (big battery with bolt, big battery with plug, empty battery with warning triangle) are 4-bit alpha from `art/sys/sysscreen.py` → `sysscreen_data.h`, ≤ 3 KB.
- **Charging / Charged** (⚠️ E1; Rev 1 H-1). The hook is in **`apps/ipodui/ui_main.c`**, not `misc.c`: `charging_splash()` is dead in the shell (§0) and stays untouched.
  - **Where:** `global_hold()`'s hold-PLAY branch with `charger_inserted()` keeps calling `charging_sleep_prepare()` (pause, resume point, `status_save(true)`) and then pushes **`ui_sys_charging_screen`** with `ui_push()`. The `sleep_on_release` branch no longer calls `backlight_off()`/`ui_unlit_suppress_next()`: on the PLAY release it only clears the flag and swallows the release, so the Charging screen is what the user sees.
  - **An ordinary pushed screen, not a nested blocking loop**, so the main loop keeps running the backlight timeout, the unlit face (#51) and LCD sleep exactly as for any other screen. It shows "Charging" (bolt) or "Charged" (plug) and swaps when `charging_state()` changes.
  - **It pops** on any key (the key is swallowed; playback stays paused, as today's sleep), on charger removal, or on USB (the USB screen takes over as usual). The screen below is redrawn by the normal pop.
  - **Wakes:** its `timeout()` returns **10 s while lit** (one `charging_state()` poll per wake, lean rule 5) and `TIMEOUT_BLOCK` while dark; the charger-removal and USB events arrive as events, not polls. LCD push: 1 full on enter, 1 band when the state changes.
- **Connect to Power:** the two `misc.c` low/empty `DEBUGF` branches in `clean_shutdown()` also call `ui_sys_low_battery()`. These are the **only** `misc.c` edits in P11 (Rev 1 L-10). It runs on the **UI thread** (`sys_poweroff()` → `SYS_POWEROFF` → `default_event_handler()` → `clean_shutdown()`; Rev 1 L-7). The screen may be dark at that moment, so it first turns the backlight on (which wakes the LCD through the normal path) and, if `lcd_active()` (compiled with `HAVE_LCD_SLEEP`) is still false, skips the draw and only logs. Otherwise one full draw, `lcd_update()`, then `sleep(2*HZ)`, and the shutdown continues. No header ("Charging, Please Wait..." / "Connect to Power", G12 obj669/670).
- **OK to Disconnect** (⚠️ E2): `usbscreen.c` polls `usb_storage_all_ejected()` (new guarded getter, §4.6) on its existing `HZ/2` idle wake. When every LUN is ejected, the text changes to "OK to Disconnect" and the idle glyph stays. It stays in USB mode until `SYS_USB_DISCONNECTED`.
- **Time in Title:** `header.c` `tt.off` becomes `!global_settings.ipodui_time_in_title`. `ui_header_set_time_in_title()` stays as the apply hook. The setting defaults to **Off** (§1.2 T1). Because `tt` is zero-initialised (clock on), **shell start calls `ui_header_set_time_in_title(global_settings.ipodui_time_in_title)` before the first header draw** (U2b, in `ui_main.c` next to the Charging hook), so a boot with the default never flashes a clock.

### 3.8 Podcasts (L; rows U1)
- Already true, and covered by scripts: shows alphabetical, episodes newest `tag_mtime` first (⚠️ P2), the show dot when any episode is unplayed, per-episode resume through tagcache stats (`tagcache_ui_resumes()`), episodes playing back to back, the queue counter.
- **Changes:**
  1. `ui_podcasts_arg.progress = false` and episode lists drop `ui_book_hint_draw` (⚠️ P1). `ui_book_hint_*` goes if nothing else uses it.
  2. `lib_progress_refresh()` is still called after Now Playing, so a just-started episode loses its dot without a reopen.
  3. L confirms `lib_unplayed()` reads as "never started": no play count, no saved position, not playing now.

### 3.9 QW2: pre-rendered Now Playing art (Q)
- **Builder** (`artcache.c`, NP bundle part only):
  - For each bundle the predicate (checklist item 13) selects, decode and fit as today, then call **`np_art_render()` on the builder thread** into a composed image of the slanted card plus its reflection, against `NP_WHITE` (the NP body background).
  - Rect: x 10–147 × y 34–204 = **138 × 171 px = 47,196 B**, plus the reflection band rows 198–204 already inside it.
  - The bundle's NOWPLAYING image becomes this composed image. The fitted 142² source is no longer stored.
  - **Per-bundle version, in existing fields** (Rev 1 L-2): `artbundle_hdr.version` = **1** fitted (today's `ARTBUNDLE_VERSION`), **2** composed; `artbundle_img.format` = `ARTBUNDLE_FMT_NPCOMPOSED` (1) on that image; the 16-bit `NP_LAYOUT_HASH` goes in that image's `artbundle_img.reserved`. **No new header field.** Readers accept both versions indefinitely. **Never bump the global "IPAC 3"**: that would wipe thumbs and panels and rebuild 5k albums.
  - **Builder memory** (Rev 1 L-2): the existing NP decode block is already **98,588 B** (`artcache.c`: JPEG overhead + 40,328 + 6,816 + 4,096). After the decode, the fitted source (40,328) and the composed output (47,196) together need **87,524 B**, which fits: **no banding, no growth**.
  - Same windows as today: stopped, or the fill-done window; per-chapter bundles too.
- **Draw:**
  - On a track change Now Playing reads the bundle into `NP_BLK_ART` in **one `read()`** (47,196 B + header) and blits it with `lcd_bitmap_part()`, one `memcpy` per row.
  - **`NP_REFL_BAND` is aliased, not copied** (Rev 1 M-4): the band (x 10–147, screen rows 198–204, 138 × 7 × 2 = 1,932 B) is byte-identical to the composed image's rows 164–170 (screen rows 198–204), and the strides match (both 138 px), so `m->band` points at `art + 164 * NP_COMP_W`. The separate band region after `struct np_scr` goes. **No `np_art_render()` at track change** (it stays for the fallback below, which renders into the same block and so also leaves the band in place).
  - **Fallback:** an old bundle (fitted, previous version) is rendered at runtime as today, traced `np art legacy`, until the builder upgrades it.
- **Placeholders:**
  - The note and book-glyph composed images are produced **at build time** by `art/np/npph.py`, which mirrors `np_placeholder_render_glyph()` + `np_art_render()` and is checked pixel-identical by a host test.
  - Stored compiled in: the card part RLE-coded per row, the reflection rows raw or delta-coded; target ≤ 16 KB rodata each.
  - Decoded straight into the framebuffer: no RAM block, no disk.
  - **Fallback if RLE compresses badly** (Rev 1 M-6): the slanted card has a gradient on every row, so per-row RLE may not reach 16 KB. If `npph.py` reports more than 16 KB for either glyph, Q ships **no placeholder rodata** and instead renders the placeholder **once into the NP block** (`np_placeholder_render_glyph()` + `np_art_render()`, ≈ 281k instructions, once per coverless track, boosted, at the first frame), traced `np art src=ph-render`. That costs one frame's budget once, no rodata and no RAM beyond the block.
  - First frame of a track: if the art isn't in `NP_BLK_ART` yet, draw the placeholder (glyph from `is_audiobook`), then swap in the real art the frame after its read. (A compiled placeholder goes to the framebuffer, not the block, so until the swap `band_ok` is false and a band restore re-decodes the placeholder's rows 198–204 from rodata, 7 rows.) A hard swap (⚠️ Q2) in one band push.
  - **No placeholder flash** when the read finishes before the first frame's push. Q orders the read before the first compose when the disk is awake (`storage_disk_is_active()`).
- **Geometry:** keep `NP_SLANT_*` (⚠️ Q1). Deliver a sheet overlaying both candidate boxes on rectified photo 1203.
- **Speakers:** regenerate the quiet and loud speaker tiles in `art/npglyphs.py`: 4× supersampled vector, then downsample; body 3 × 5, cone to an 11 px mouth, rim tips, 1 px slit, 2 × 3 dust cap; loud arcs at r ≈ 5 and 8, ±50°; ~4 px from the bar ends; near-black #1c1c1c.
- **Pixel identity:** a composed bundle blitted must equal today's runtime render pixel for pixel, the same code on another thread. So **every existing NP golden with real art stays unchanged**. Only the speaker goldens and the placeholder-first frames change.

---

## 4. Contracts (real code; the orchestrator commits these on `p11` before fan-out)

The `p11: contracts` commit holds:
- every header below;
- the `settings.h` / `settings_list.c` additions (§4.7), so nobody else edits those two files;
- the P11 `english.lang` block;
- `setwidgets.c` (code moved verbatim from `settings.c`);
- the screen arena implementation plus its host test, the `struct ui_screen` mark field, and the arena block's allocation in `ui_main.c` at shell start (after that, `ui_main.c` belongs to U2b, §5);
- `ui_vol_frac()` implemented for real in `np_playback.c` (§3.5, Rev 1 L-8);
- **one shared stub**, `apps/ipodui/p11_stub.c`, with sections L / U1 / U2a / U2b / Q, each deleted by its owner in the commit that adds the real code;
- the `apps/SOURCES` lines (inside `#ifdef HAVE_IPOD_UI`).

**Merge note for the orchestrator (Rev 1 M-1):** when `p6` and `p8a-int` merge, keep **P6's** `menu.c` (static `MENU_SLOTS`, the core_alloc only as a third-nested fallback). 8a's `menu.c` still has the unconditional `core_alloc(sizeof(struct menu_state))` in the menu enter (8a :213); a merge that takes 8a's hunk reintroduces the paused-queue bug.

No `TEMP-` commits. Round-trip host tests that ship in the contract commit: `tests/search/` (fold and match tables, against a corpus), `tests/arena/` (mark/reset LIFO, a failed enter() leaving the top unchanged, `ui_pop_to_root()` from depth 8, alignment), `tests/menucfg/` (masks → visible order), `tests/otgfmt/` (idx header pack and check, torn tail).

**Trace macro:** shell code uses `UPDATE_TRACE` (`anim.h`) or `ui_log`. `misc.c` and `usb_storage.c` hooks use `DEBUGF`. `logf` is never the only trace. Every rule fails when its anchor lines are absent.

### 4.1 `apps/ipodui/screen_stack.h` (addition): the screen arena
```c
/* P11: screen states without core_alloc after shell start. One block of
 * UI_ARENA_SIZE, core_alloc'd and pinned in ui_main.c at shell start, before
 * anything can start playback. Shares with nothing (permanent; a new budget
 * line; can't share because it lives as long as the stack).
 *
 * MARK-BASED (Rev 1 M-1). ui_push_t() records the arena top in
 * s->arena_mark BEFORE calling ops->enter(). After ops->leave() returns
 * (ui_pop, ui_pop_to_root, each level) AND after an enter() that fails,
 * screen_stack.c resets the top to that mark. So whatever a screen took
 * (one block or several) is released with it, a failed enter() leaks
 * nothing, and screens never free arena memory themselves.
 *
 * Fallback: a screen that needs more than ui_arena_free() may core_alloc
 * ONLY when audio_status() == 0, re-checked with no yield before the
 * allocation, traced "arena full need=%u". Its leave() core_free()s the
 * block and then calls audio_buffer_regrow() (playback.h), so the next
 * playback start sizes the audio buffer anew. */
#define UI_ARENA_SIZE   8192
void  *ui_arena_alloc(size_t bytes);    /* 8-aligned, zeroed, from the top;
                                           NULL if it doesn't fit (no fallback here) */
size_t ui_arena_free(void);
/* screen_stack.c only: */
uint16_t ui_arena_mark(void);
void     ui_arena_reset(uint16_t mark); /* asserts mark <= top */
/* struct ui_screen gains:  uint16_t arena_mark;  (set by ui_push_t) */
```

**Arena sizing (contract commit).** `screen_stack.c` (or each screen's file) carries `NP_STATIC_ASSERT`-style asserts per state, and the arena host test sums the deepest stacks. Sizes below are the brief's estimates; the contract commit replaces them with the real `sizeof` and adds `_Static_assert(sizeof(struct X) <= N)` for each:

| State (owner) | Bound (assert ≤) | Note |
|---|---|---|
| `struct playlists` (U1) | 2,400 | ≈ 2.3 KB: `short off[64]` + `char names[2048]` + `struct ui_list` |
| `struct browse` (U1) | 512 | `struct ui_list` (viewport + hooks) + qscroll + filter |
| `struct book_page` (U1) | 320 | `struct ui_list` + 1 flag |
| `choice_screen` state (U2a) | 384 | |
| OTG list (U1) | 320 | rows live in the view pool |
| hold menu (U1) | 256 | |
| Search (U1) | 2,560 | query + the ≤ 64 playlist names packed as `struct playlists` packs them |
| checklist, roller screens, About, Date & Time lists, Brightness, EQ, Language, Reset (U2a/U2b) | 384 each | |
| Legal (U2b) | 2,048 | line-break table of the wrapped paragraphs |
| Charging (U2b) | 64 | |

Deepest stacks the host test sums (the main menu and Music menu use P6's static `MENU_SLOTS`, which take no arena):
- **Playlists → On-The-Go → hold → Browse Album → Songs:** 2,400 + 320 + 256 + 512 + 512 = **4,000 B**.
- **Music → Search → Album → Songs → hold → Browse Album:** 2,560 + 512 + 512 + 256 + 512 = **4,352 B** (the worst reachable chain on the 8-deep stack).
- **Settings → Date & Time → Time Zone region → city:** 384 × 4 = 1,536 B; **Settings → Legal:** 2,432 B.

The test enumerates these chains and asserts the worst sum + 8-byte alignment slack per level (8 × 8 B) ≤ `UI_ARENA_SIZE`. Margin at 8,192: ≈ 3.7 KB.

### 4.2 `apps/ipodui/search.h`
```c
#ifndef IPODUI_SEARCH_H
#define IPODUI_SEARCH_H
#include <stdbool.h>
#include <stdint.h>

#define SRCH_MAX_Q      32              /* folded bytes */
enum srch_kind { SRCH_ARTIST, SRCH_ALBUM, SRCH_SONG, SRCH_BOOK,
                 SRCH_EPISODE, SRCH_PLAYLIST, SRCH_KINDS };
#define SRCH_ROW(k, idx)   (((uint32_t)(k) << 29) | ((uint32_t)(idx) & 0x1fffffffu))
#define SRCH_ROW_KIND(r)   ((enum srch_kind)((uint32_t)(r) >> 29))
#define SRCH_ROW_IDX(r)    ((int32_t)((uint32_t)(r) & 0x1fffffffu))

struct srch_query {
    char    folded[SRCH_MAX_Q + 1];     /* NUL-terminated, words split by ' ' */
    uint8_t len;
};
/* pure (search_core.c; host-tested, ELF instruction-counted) */
int  srch_fold_query(const char *utf8, struct srch_query *q);   /* returns len */
bool srch_match(const char *hay, const struct srch_query *q);   /* word-prefix (⚠️ S1) */
bool srch_extends(const struct srch_query *prev, const struct srch_query *q);

/* library.c (L). Search entry: loads the group kinds' precompute ROWS
 * (<= 4 section reads, no lib_open) into view-pool slack; returns a
 * handle, < 0 if nothing fits (group kinds then skipped, result capped). */
int  lib_search_begin(void);
void lib_search_end(void);               /* frees the ROWS and the result view */
/* Builds or refines THE one LIB_SEARCH view (returned; the same handle
 * every call). view >= 0 and srch_extends(prev, q): compacts view's rows
 * in place (the set only shrinks; no second view, no allocation).
 * Otherwise rebuilds it from the full sources. Yields every 4096 entries;
 * returns < 0 if 'cancel' says so or the tagcache generation changed
 * (caller retries). Never grows the pool while audio_status() != 0.
 * Rows sorted by sort key across kinds. *capped = true when rows were
 * dropped (the screen then shows "More..." last, ⚠️ S6). */
int  lib_search(const struct srch_query *q, int view, bool *capped,
                bool (*cancel)(void));
/* Trace: "search q=<len> kind_n=<a>,<al>,<s>,<b>,<e> n=<rows> us=<t>
 *         yields=<y> refine=<0|1> cap=<0|n>" */
#endif
```
`library.h` gains `LIB_SEARCH` and `LIB_OTG` in `enum lib_cat`, appended after `LIB_PODCASTS` (no renumbering). `lib_label()`, `lib_sublabel()`, `lib_play()` and `lib_path()` accept both: a search row resolves by kind; `lib_play()` on a song or episode row plays the matching rows of that kind from there.

### 4.3 `apps/ipodui/library.h` additions (L)
```c
/* Library totals for About (P11): computed by lib_precompute() at sync time
 * (bitrate x length summed per music track, no file reads) and kept in a
 * 24 B static when the file is written or found current; no I/O here.
 * WRITTEN on the tagcache thread (lib_precompute), READ on the UI thread
 * (Rev 1 L-4): a sequence counter guards it. The writer increments seq
 * (odd), stores the fields, increments seq (even). lib_stats() copies the
 * fields between two reads of seq and retries while seq is odd or changed
 * (no lock, no yield; a retry costs a 20 B copy). */
struct lib_stats {
    int32_t  songs, episodes, books;     /* tracks of each class */
    uint32_t audio_mib;                  /* sum(bitrate kbps * length ms) / 8 / 2^20 */
    int32_t  gen;                        /* tagcache_ui_generation() it is for */
};
bool lib_stats(struct lib_stats *out);   /* false before any precompute */
/* library.c static: struct { volatile uint32_t seq; struct lib_stats s; } (24 B) */
/* libfile.h: LIBFILE_CAT_STATS 0x7d, col 0, one struct libfile_stats (16 B);
 * an ADDED section (LIBFILE_VERSION stays 1). */
```

### 4.4 `apps/ipodui/otg.h` (L)
```c
#ifndef IPODUI_OTG_H
#define IPODUI_OTG_H
#include <stdint.h>
#include <stddef.h>
#define OTG_DIR      ROCKBOX_DIR "/ipodui"
#define OTG_LIST     "otg.m3u8"          /* in OTG_DIR: a plain playlist */
#define OTG_IDX      OTG_DIR "/otg.idx"
#define OTG_MAX      2000                /* = max_files_in_playlist on this target */
#define OTG_MAGIC    0x544f5049u         /* "IPOT" */
struct otg_idx_hdr {                     /* 20 B */
    uint32_t magic, version;             /* version 1 */
    uint32_t count;                      /* records; must equal the list's lines */
    uint32_t sum;                        /* libfile_sum of the records */
    int32_t  commitid;                   /* tagcache_ui_commitid() at resolve;
                                            != current -> resolve (Rev 1 L-3) */
};
/* An append writes the list lines, then the records, then this header
 * (2 writes to otg.idx). count < lines after a cut -> resolve the tail. */
struct otg_rec { int32_t idx; uint32_t check; };   /* 8 B; idx -1 = unresolved */
uint32_t otg_check(int32_t idx);         /* hash(title string, tag_length); RAM only */
int  otg_count(void);                    /* cached; no I/O after the first call */
int  otg_add_track(long tc_idx);         /* 1-based tc_idx as np_track; returns added or < 0 */
int  otg_add_row(int view, int idx);     /* a song or a group row: all its tracks, capped */
int  otg_remove(int pos);
int  otg_clear(void);
int  otg_save(char *name, size_t len);   /* "New Playlist N" -> /Playlists, then clear */
int  otg_open_view(void);                /* LIB_OTG view; resolves once if needed:
                                            ONE bulk pass, buffers in the list
                                            strip (32 KB read + 16 KB set) */
int  otg_play(int pos);                  /* a fresh current playlist built from the
                                            store's paths (playlist_insert_context_*,
                                            as play_view(), capped OTG_MAX), then
                                            start at pos. NEVER playlist_create()s
                                            the store file (Rev 1 H-2) */
/* Trace: "otg add n=%d reads=%d writes=%d", "otg resolve n=%d unresolved=%d
 *         bytes=%lu reads=%d us=%lu", "otg play n=%d pos=%d reads=%d",
 *         "otg save name=%s n=%d", "otg clear" */
#endif
```

### 4.5 `apps/ipodui/screens/holdmenu.h` (U1) and `menucfg.h` (U2a)
```c
/* holdmenu.h */
enum ui_hold_item { UI_HOLD_ADD_OTG, UI_HOLD_REMOVE_OTG, UI_HOLD_BROWSE_ALBUM,
                    UI_HOLD_BROWSE_ARTIST, UI_HOLD_CANCEL, UI_HOLD_ITEMS };
enum ui_hold_ctx  { UI_HOLD_NP, UI_HOLD_SONG, UI_HOLD_GROUP, UI_HOLD_IN_OTG };
struct ui_hold_arg {                     /* copied by enter() */
    enum ui_hold_ctx ctx;
    long tc_idx;                         /* 1-based, 0 = none (identity) */
    int  db_gen;                         /* validity of tc_idx */
    int  view, idx;                      /* the row held (lists) */
    int  otg_pos;                        /* UI_HOLD_IN_OTG */
    bool spoken;                         /* a book or podcast episode (track class
                                            or held view): no Browse Album/Artist */
};
unsigned ui_hold_items(const struct ui_hold_arg *a);     /* pure bitmask (⚠️ H1);
                                            drops BROWSE_* when spoken */
/* holdmenu.c enter() sets a swallow flag: the SELECT release of the hold
 * that opened it is dropped (Rev 1 L-6). */
extern const struct ui_screen_ops ui_hold_screen;        /* arg: struct ui_hold_arg */
void ui_hold_open_np(long tc_idx, int db_gen);           /* nowplaying.c hook */

/* menucfg.h (bits are persisted: frozen, append-only) */
enum ui_mm_bit { UI_MM_MUSIC, UI_MM_COVERFLOW, UI_MM_PLAYLISTS, UI_MM_ARTISTS,
    UI_MM_ALBUMS, UI_MM_COMPILATIONS, UI_MM_SONGS, UI_MM_GENRES, UI_MM_COMPOSERS,
    UI_MM_AUDIOBOOKS, UI_MM_SEARCH, UI_MM_GENIUS, UI_MM_VIDEOS, UI_MM_PHOTOS,
    UI_MM_PODCASTS, UI_MM_EXTRAS, UI_MM_ALARMS, UI_MM_CALENDARS, UI_MM_CLOCKS,
    UI_MM_CONTACTS, UI_MM_GAMES, UI_MM_NOTES, UI_MM_SCREENLOCK, UI_MM_STOPWATCH,
    UI_MM_SETTINGS, UI_MM_SHUFFLE, UI_MM_BITS };
enum ui_mu_bit { UI_MU_COVERFLOW, UI_MU_GENIUS, UI_MU_PLAYLISTS, UI_MU_ARTISTS,
    UI_MU_ALBUMS, UI_MU_COMPILATIONS, UI_MU_SONGS, UI_MU_GENRES, UI_MU_COMPOSERS,
    UI_MU_AUDIOBOOKS, UI_MU_SEARCH, UI_MU_BITS };
#define UI_MM_DEFAULT ((1u<<UI_MM_MUSIC)|(1u<<UI_MM_PODCASTS)|(1u<<UI_MM_EXTRAS)| \
                       (1u<<UI_MM_SETTINGS)|(1u<<UI_MM_SHUFFLE))
#define UI_MU_DEFAULT ((1u<<UI_MU_PLAYLISTS)|(1u<<UI_MU_ARTISTS)|(1u<<UI_MU_ALBUMS)| \
                       (1u<<UI_MU_SONGS)|(1u<<UI_MU_GENRES)|(1u<<UI_MU_COMPOSERS)| \
                       (1u<<UI_MU_AUDIOBOOKS)|(1u<<UI_MU_SEARCH))
#define UI_MM_OFFERED /* shipped only (⚠️ C1): MUSIC..COMPOSERS except COVERFLOW,
                         AUDIOBOOKS, SEARCH, PODCASTS, EXTRAS, SETTINGS, SHUFFLE */
#define UI_MU_OFFERED /* PLAYLISTS..SEARCH */
bool ui_mm_visible(enum ui_mm_bit b);    /* setting & offered; SETTINGS always */
bool ui_mu_visible(enum ui_mu_bit b);
```

### 4.6 `apps/ipodui/vlimit.h`, `roller.h`, `sysscreen.h`, the `misc.c` and `usb_storage.c` hooks
```c
/* vlimit.h (U2a; ui_vol_frac implemented by the contract commit) */
int  ui_vol_frac(int vol);               /* 0..1024 along the NP volume bar; pure;
                                            np_playback.c, beside np_pb_volume_pct(),
                                            the same sound_min..sound_max mapping
                                            (Rev 1 L-8) */
bool ui_vlimit_active(void);             /* volume_limit < max volume */
bool ui_vlimit_locked(void);             /* ipodui_vlimit_code >= 0 */
void ui_vlimit_apply(void);              /* sound_set_volume(global_status.volume):
                                            call after EVERY volume_limit write
                                            (Rev 1 M-5; reset.c calls it too) */
extern const struct ui_screen_ops ui_vlimit_screen;

/* roller.h (U2a; P12 Screen Lock reuses it) */
#define UI_ROLLER_MAX 4
struct ui_roller_field { short min, max, value; uint8_t digits; };
struct ui_roller {
    struct ui_roller_field f[UI_ROLLER_MAX];
    uint8_t n, cur;                      /* fields, the boxed one */
    bool dark;                           /* dark screens (combination, date) */
};
enum ui_roller_res { UI_ROLLER_BUSY, UI_ROLLER_DONE, UI_ROLLER_CANCEL };
enum ui_roller_res ui_roller_input(struct ui_roller *r, const struct ui_input *in,
                                   int *band_y, int *band_h);    /* pure */
void ui_roller_draw(const struct ui_roller *r, int y, int label_lang);

/* sysscreen.h (U2b) */
extern const struct ui_screen_ops ui_sys_charging_screen;
                                         /* an ordinary pushed screen (Rev 1 H-1):
                                            timeout() 10 s lit, TIMEOUT_BLOCK dark;
                                            pops on a key (swallowed), charger
                                            removal, USB. No nested loop. */
void ui_sys_low_battery(bool empty);     /* UI thread (clean_shutdown via
                                            sys_poweroff); backlight on first, skip
                                            the draw if the LCD stays asleep; else
                                            draw + lcd_update + 2 s; never allocates */

/* apps/ipodui/ui_main.c (U2b, after the contract commit):
 *   global_hold(), hold PLAY with charger_inserted(): charging_sleep_prepare();
 *     ui_push(&ui_sys_charging_screen, NULL);
 *   the sleep_on_release branch: clear the flag, swallow the release; no
 *     backlight_off() / ui_unlit_suppress_next() any more;
 *   shell start: ui_header_set_time_in_title(global_settings.ipodui_time_in_title)
 *     before the first header draw (Rev 1 M-7).
 * apps/misc.c, guarded #ifdef HAVE_IPOD_UI (U2b), the ONLY misc.c edits
 *   (Rev 1 L-10): in clean_shutdown()'s two low/empty DEBUGF branches,
 *   ui_sys_low_battery(empty). charging_splash() and list_stop_handler()
 *   are not touched (dead in the shell).
 * firmware/usbstack/usb_storage.c, guarded #ifdef HAVE_IPOD_UI (U2b): one
 *   read-only getter, no other change: */
bool usb_storage_all_ejected(void);      /* every LUN has ejected[lun] set; read-only */
```

### 4.7 Settings (contract commit, all `#ifdef HAVE_IPOD_UI`, appended next to P6's `ipodui_*`)

| Field | Kind | Default | Config name |
|---|---|---|---|
| `ipodui_main_menu` | INT 0..0x3ffffff | `UI_MM_DEFAULT` | "ipodui main menu" |
| `ipodui_music_menu` | INT 0..0x7ff | `UI_MU_DEFAULT` | "ipodui music menu" |
| `ipodui_vlimit_code` | INT −1..9999 | −1 | "ipodui volume code" |
| `ipodui_time_in_title` | OFFON | **false** (owner, §1.2 T1) | "ipodui time in title" |
| `ipodui_tz_city` | INT 0..TZ_CITIES−1 | New York's stable id | "ipodui time zone" (stable id = append-only table index, §3.6) |
| `ipodui_sort_last` | OFFON | false | "ipodui sort by last" |
| `ipodui_eq_preset` | INT 0..22 | 0 (Off) | "ipodui eq preset" |
| `ipodui_serial`, `ipodui_model` | TEXT 16 | "" | "ipodui serial", "ipodui model" |

These are reused upstream and not added: `volume_limit`, `timeformat`, `brightness`, `backlight_timeout`, `eq_*`. All are hidden from stock menus (the shell never calls `root_menu()`).

### 4.8 Moved or exported by the contract commit
- `screens/setwidgets.{c,h}`: `sp_corners`, `sp_wrap`, `sp_draw_control` → `ui_seg_draw(const short *lang, int n, int sel, bool shelf)`, `ui_prompt_draw(int head_lang, int body_lang)`. Moved verbatim; the Audiobooks prompt's golden must stay identical.
- `screens/panels.h`: `const char *ui_fmt_gb(unsigned long kib, char *buf, size_t len)`, `void ui_capsule_draw(int x, int y, int w, int h, const uint16_t *seg_kib_frac, int nseg)`.
- `screens/screens.h`: `extern const struct ui_screen_ops ui_search_screen, ui_otg_screen, ui_checklist_screen, ui_datetime_menu_screen, ui_brightness_screen, ui_legal_screen, ui_reset_screen, ui_language_screen, ui_eq_screen;` (`ui_about_screen` already exists).
- `np_layout.h` (Q's after the contract; Rev 1 M-4): `NP_BLK_ART_SIZE` → `NP_COMP_W * NP_COMP_H * 2` (138 × 171 × 2 = 47,196), so `NP_BLK_END` 46,564 → **53,432**. The block holds more than the `NP_BLK_*` regions: `nowplaying.c` places `struct np_scr` at `NP_SCR_OFF = NP_ALIGN4(NP_BLK_END)` and, today, the 1,932 B reflection band at `NP_BAND_OFF` after it. With the band **aliased** onto the composed image's rows 164–170 (§3.9), the block is `NP_BLK_SIZE = NP_ALIGN(NP_SCR_OFF + sizeof(struct np_scr), 512)`; `sizeof(struct np_scr)` is ≤ 656 B today (49,152 − 46,564 − 1,932), so **≤ 54,272 B** (+5,120). `nowplaying.c`'s assert becomes `NP_STATIC_ASSERT(NP_SCR_OFF + sizeof(struct np_scr) <= NP_BLK_SIZE)`; the `NP_BAND_OFF` region and its assert go. If aliasing proves impossible, the band stays and the size is taken from `NP_BAND_OFF + NP_REFL_BAND_SIZE` (≤ 56,320 B), reported.
- `artfmt.h` (Q; Rev 1 L-2): **no new header field.** `#define ARTBUNDLE_VERSION_FITTED 1` (today's `ARTBUNDLE_VERSION`), `ARTBUNDLE_VERSION_COMPOSED 2`; `#define ARTBUNDLE_FMT_NPCOMPOSED 1` for `artbundle_img.format`; the NP image's `artbundle_img.reserved` holds the 16-bit `NP_LAYOUT_HASH`. Readers accept versions 1 and 2.

### 4.9 `english.lang` P11 block (contract commit; appended, `*: none` / `ipodui: "<text>"`)

| Group | IDs (new) → text |
|---|---|
| Search | `_NO_SEARCH_RESULTS` "No Search Results"; `_RESULTS_FOR` "Results for: %s"; `_SEARCH_MORE` "More..." (the capped-results row, ⚠️ S6) |
| Hold / OTG | `_ADD_TO_OTG` "Add to On-The-Go"; `_REMOVE_FROM_OTG` "Remove from On-The-Go"; `_BROWSE_ALBUM`; `_BROWSE_ARTIST`; `_SAVE_PLAYLIST`; `_CLEAR_PLAYLIST`; `_CLEAR` "Clear"; `_NEW_PLAYLIST_N` "New Playlist %d" (Cancel reuses stock `LANG_CANCEL`) |
| Checklists | `_RESET_MENU` "Reset Menu" (row labels reuse the existing menu IDs) |
| Volume | `_ENTER_COMBINATION` "Enter Combination"; `_VOLUME_LIMIT` (exists) |
| About | `_GB_USED` "%s GB Used" (`_GB_FREE` exists); `_AUDIO`, `_VIDEO`, `_PHOTOS`, `_OTHER`; `_VIDEOS`, `_GAMES`, `_CONTACTS`; `_SERIAL_NUMBER`, `_MODEL`, `_VERSION`; `_VERSION_STRING` "ProkBox 0.11" (⚠️ D1) |
| Legal | `_LEGAL_1` … `_LEGAL_6` (the paragraphs) |
| Reset / Language | `_RESET` "Reset"; `_LANG_NAME` "English" |
| Date & Time | `_DATE`, `_TIME`, `_TIME_ZONE`, `_24_HOUR_CLOCK`, `_TIME_IN_TITLE`; `_TZ_REGION_*` (≈ 8), `_TZ_CITY_*` (≈ 80, generated list) (months reuse stock `LANG_MONTH_*`, weekdays `LANG_IPODUI_SUNDAY…`) |
| EQ | `_EQ_ACOUSTIC` … `_EQ_VOCAL_BOOSTER` (22) |
| System | `_CHARGING` "Charging"; `_CHARGED` "Charged"; `_CHARGING_WAIT` "Charging, Please Wait..."; `_CONNECT_TO_POWER` "Connect to Power"; `_OK_TO_DISCONNECT` "OK to Disconnect" |

That's about 150 phrases (+ the ~80 cities). The audit rule from 8a holds: no literal reaches the screen.

### 4.10 Trace lines and `check_trace.py` rules (T)

| Line | From | Rule |
|---|---|---|
| `search q= ... n= us= yields= refine= cap=` | library.c | p11-search: `refine=1` for every appended-letter step after the first; `yields ≥ n_entries/4096` |
| `otg add/resolve/play/save/clear` | otg.c | p11-otg: `reads ≤ 1 + tracks` per add; **one** `otg resolve` line per open, with `reads = ⌈bytes / 32 KB⌉` (one pass); `otg play` never followed by a `playlist_create` of `otg.m3u8`; no `otg` write while the OTG screen glides |
| `hold open ctx=<c> items=<mask>`, `hold act <item>` | holdmenu.c | p11-hold: the mask equals `ui_hold_items()` for the context |
| `arena push s=<name> n=<b> free=<f>`, `arena full need=<b>` | screen_stack.c | **every** `ipodui-*` script: 0 `arena full` while audio is playing or paused; and 0 screen-state `core_alloc` after shell start (P6's `mem mark`) |
| `vlimit set=<db> code=<0\|1> applied=<vol>`, `vlimit unlock ok=<0\|1>` | vlimit.c | p11-vlimit: a locked change needs `unlock ok=1` first; every `vlimit set` carries `applied` ≤ the limit |
| `about page=<n>` | about.c | p11-about: the chord script still shows `devmode enter` |
| `sys charging state=<charging\|charged>`, `sys charging pop=<key\|unplug\|usb>`, `sys lowbatt empty=<0\|1> drawn=<0\|1>`, `usb ejected` | sysscreen.c, usbscreen.c | p11-sys: `sys charging` follows the existing `sleep: charging` line; the backlight-off and LCD-sleep lines still appear after it (no nested loop) |
| `np art src=<bundle\|legacy\|ph-note\|ph-book\|ph-render> reads=<n> us=<t>` | nowplaying.c | qw2: `reads ≤ 1` and `src=bundle` after the builder pass; no `np_art_render` at a track change unless `legacy` |

---

## 5. Ownership (worktrees, one owner per file)

**Concurrency:** at most 5 agents at once on the 8 GB Mac; **one gate per machine** (`~/.cache/rockbox-gate.lock`). **Wave 1:** L, U1, U2a, U2b, Q. **Wave 2:** T starts when the first wave-1 agent hands back (expected L, the smallest). In wave 1 each agent writes **its own** `tests/ui/ipodui-p11-<area>-*.script` files (disjoint names) using existing sim commands plus those the contract commit adds (§8). T then owns the shared tooling and the review sheets.

Agents iterate with `make -j2` in their own `build/sim-ui` and single `scripts/shots.sh` runs, set `JOBS=2`, and use **at most 2** `scripts/gate-run --quick --changed` runs (the lock queues them). Build `fw-ui` only before handing back.

| Agent (tier) | Owns | Must not edit |
|---|---|---|
| **L** library and data (opus) | `apps/ipodui/library.{c,h}` (LIB_SEARCH, LIB_OTG, `lib_stats`, podcast dot semantics, the stats section), `libfile.h`, new `search_core.c`, `otg.c`, `tests/search/*`, `tests/otg/*`, `tests/perf/search50k.sh`; deletes p11_stub §L | screens, menu.c, artcache.c, contract headers |
| **U1** lists and popups (opus) | new `screens/search.c`, `screens/holdmenu.c`, `screens/otglist.c`; `screens/playlists.c` (arena, OTG row → `ui_otg_screen`, `ui_otg_add` removed or forwarded to `otg_add_track`); `screens/browse.c` (hold → menu, arena, podcast rows); `screens/audiobooks.c` (podcast arg, hint removal, book-page arena); `art/rowglyphs.py` + `rowglyphs.h` (magnifier tile, 6 type icons); deletes p11_stub §U1 | library.c, settings.c, menu.c, nowplaying.c |
| **U2a** Settings tree (sonnet) | `screens/settings.c` (row wiring, Sort By setting, EQ caption, choice → arena), new `screens/checklist.c`, `menucfg.c`, `vlimit.c`, `roller.c`, `art/check/check.py` + `checkglyph.h`; `apps/ipodui/menu.c` (main/music items, `visible()`, Search target); settings **default values** only in `settings_list.c` if a contract default proves wrong (via the orchestrator); deletes p11_stub §U2a | header.c, about/datetime/sys files, nowplaying.c |
| **U2b** info and system screens (opus) | new `screens/about.c` (and removes `ui_about_screen` from `stub.c`; owns `stub.c`), `legal.c`, `reset.c`, `language.c`, `datetime.c`, `brightness.c`, `eq.c`, `sysscreen.c`, `sysscreen_data.h`, `art/sys/*`, `art/tz/tzcities.py` + `tzcities.h`; `apps/ipodui/header.c` (Time in Title setting); **`apps/ipodui/ui_main.c`** after the contract commit (the Charging push and release change in `global_hold()`, the Time in Title apply at shell start; Rev 1 H-1, M-7); `apps/ipodui/usbscreen.c` (OK to Disconnect); `apps/misc.c` (the two low/empty calls only) and `firmware/usbstack/usb_storage.c` (the getter only), guarded, §4.6; deletes p11_stub §U2b | settings.c, menu.c, library.c, NP files, `np_playback.c` |
| **Q** QW2 Now Playing art (opus) | `apps/ipodui/np_draw.{c,h}`, `np_layout.h`, `screens/nowplaying.c` (art paths, the reflection-band alias, **plus** the one `ui_hold_open_np()` call in the hold-SELECT branch, p6 :2145), the NP-bundle part of `artcache.c` (and all of `artcache.c` for this phase), `artfmt.{c,h}`, `art/npglyphs.py` + `npglyphs.h`, new `art/np/npph.py` + `npph_data.h`, `tests/npscreen/*`, `ipodui-qw2-*.script`; draws the volume-limit triangle with `ui_vol_frac()`; deletes p11_stub §Q | every other screen, library.c, rowglyphs |
| **T** tooling (sonnet, wave 2) | `scripts/check_trace.py` (§4.10), `scripts/gate_map.txt`, `uisimulator/common/sim_cmd_p11.c` beyond the contract commands (incl. `time_in_title on\|off`, §8), the **explicit-clock-on list** and the edits that put `time_in_title on` into the scripts on it (§7 criterion 8), `tests/ui/README.md`, compose sheets, `tests/review/p11-*.mp4`, the cross-area script `ipodui-p11-tour.script` | `apps/`, `firmware/`, `lib/` |

**Orchestrator:**
- the contract commit (§4): headers, settings, LANG, `setwidgets.c`, the arena (incl. its `ui_main.c` allocation and the `struct ui_screen` field), `ui_vol_frac()` in `np_playback.c` (its only P11 edit), and `apps/SOURCES`;
- the `p6` + `p8a-int` merge before `p11` is cut, keeping P6's `menu.c` `MENU_SLOTS` (§4 merge note);
- the `english.lang` merges (append order L, U1, U2a, U2b, Q);
- `uisimulator/common/SOURCES` and the `sim_script.c` registration lines;
- `docs/hardware.md` budget lines and the ROADMAP P10 list at close;
- a re-check after merge that `stub.c` still exports `ui_stub_screen` and that only `about.c` defines `ui_about_screen`.

**Read lists** (≤ 3 each; contract headers are read as code and don't count):
- **L:** this brief §3.1, §3.2, §3.8; `library.h` header comment; `docs/hardware.md` rows "Library view pool" and "Library list open".
- **U1:** this brief §3.1–§3.3; `docs/design/7g-catalogue.md` §5.3–§5.5; `docs/design/spec/music-lists.md`.
- **U2a:** this brief §3.4, §3.5; `docs/design/spec/settings.md`; `7g-catalogue.md` §7 rows Main Menu to Volume Limit.
- **U2b:** this brief §3.6, §3.7; `docs/design/spec/system-screens.md` + `settings.md` About; `7g-catalogue.md` §8.
- **Q:** this brief §3.9; `docs/design/np-reflection.md`; `docs/design/spec/now-playing.md` (layout and speaker bullets).
- **T:** this brief §4.10, §7, §8; `tests/ui/README.md`; `docs/phase-nav-brief.md` Revision 2 (`record`, `make_video.sh`).

---

## 6. Device-reality checklist (numbers; agents re-measure on their branch)

1. **Compiled features.**
   - Everything is under `HAVE_IPOD_UI`. Hooks in upstream files (`misc.c`: two calls; `usb_storage.c`: one getter; `settings*.{c,h}`) are guarded.
   - `HAVE_DIRCACHE` is compiled on the device and off by default, so OTG and Search paths go through FAT path lookups; the sim has no dircache. Item 1 → P10 (OTG add of a 400-track artist timed on the device).
   - `CONFIG_CHARGING == CHARGING_MONITOR`, `HAVE_POWEROFF_WHILE_CHARGING` off, so `ui_main.c`'s `charging_sleep_prepare()` branch (hold PLAY with the charger in) is compiled and live on the device; the sim reaches it with `charger on` + PLAY hold. `charging_splash()` is compiled but unreachable from the shell (Rev 1 H-1).
   - `HAVE_BACKLIGHT_BRIGHTNESS` is on for ipodvideo (`cap_brightness`).
   - `HAVE_RTC` gives `set_time()`.
2. **Memory.**
   - **New permanent blocks, both at shell start before any playback:**
     - screen arena **8,192 B**, pinned. It can't share because it lives as long as the stack; the panel scratch and strip are transient or claimed per frame.
     - NP block **+5,120 B** at most (49,152 → ≤ 54,272: composed art 47,196 replaces the 40,328 fitted source, `struct np_scr` ≤ 656 B follows it, and the 1,932 B reflection band is aliased onto the composed rows instead of stored; Rev 1 M-4). Q reports the exact `NP_BLK_SIZE`.
   - **No `core_alloc` while audio plays or is paused** in any P11 path:
     - search results go in the existing view pool (one result view, refined in place), capped and shown as capped; the group ROWS (≈ 34 KB) go in pool slack, never via `lib_open` (Rev 1 M-3);
     - the OTG resolve (32 KB read + 16 KB set, **one pass**) and the Save/Play copy buffers go in the **list strip** under the §3.2 sharing rule (Rev 1 M-2); the 8 KB arena holds neither;
     - the four old screen states move to the arena, which is mark-based (a failed enter() leaks nothing; Rev 1 M-1). The only fallback `core_alloc` (arena full, audio stopped, re-checked with no yield) is freed in leave() followed by `audio_buffer_regrow()`.
   - Statics: `lib_stats` + its sequence counter 24 B; `otg_count` cache 8 B; the search cancel flag; roller and about state go in the arena. Shell static RAM rises ≈ 100 B, every static < 1 KB.
   - **Indirect allocations:** none. Reset never calls `settings_apply(true)` (font/skin loads). Language never calls `lang_core_load()` (English only). EQ goes through `dsp_*` setters that use preallocated state; U2b confirms no allocation in `eq_*_set`.
   - **Art builder** (Rev 1 L-2): composing needs the fitted 40,328 B source and the 47,196 B output at once = 87,524 B. The existing NP decode block is already **98,588 B**, so both fit after the decode: **no banding, no growth**, stopped-or-window only as today.
   - **Disk per album: unchanged in practice.** The NP image grows 6,868 B (47,196 vs 40,328), but files are allocated in 32 KB clusters on this card (`docs/hardware.md`) and a 512 B header + 47,196 B image still takes the same 2 clusters as before (Q confirms per bundle kind and reports any kind that crosses a cluster boundary). RAM per album: 0 (one composed image resident in the NP block).
   - Placeholders: rodata ≤ 32 KB for both (Q reports the exact size), or 0 with the render-once fallback (§3.9).
3. **CPU** (device = 80 MHz boosted; ×400 sim factor, uncalibrated).
   - **Search at 50k tracks / 5k albums / 3k artists / 300 books / 100 shows:**
     - haystack ≈ 50k titles × 22 B + ≈ 0.18 MB of group labels ≈ **1.28 MB**;
     - `srch_match` budget **≤ 6 instructions per byte** (ELF harness), so ≈ 7.7 M instructions, plus a ≈ 1 M merge;
     - **≈ 0.11 s** at 80 MHz, **≤ 0.25 s** with DRAM stalls, for the first character;
     - a refining character is O(previous results), < 20 ms at ≤ 5k rows;
     - boosted while searching only, with ≥ 12 yields per full pass, cancelled by a new key.
     - L measures `tests/perf/search50k.sh` in the sim (thread CPU µs) and reports the device estimate.
   - **OTG resolve** (rare, after a database commit): **one pass** over ≈ 3 MB of filename tag file, hashed ≈ 15 M instructions ≈ 0.2 s, plus ≈ 96 reads of 32 KB.
   - **OTG play:** ≤ 180 KB read in 32 KB chunks + ≤ 2,000 `playlist_insert_context_add()` calls, as a 2,000-row library play.
   - **Search** is the lean-rule-11 exception (browse-time work, strings already in RAM, 0 reads per keystroke; §3.1).
   - **Hold menu, checklists, settings screens:** O(1) per event.
   - **QW2 track change:** one 47 KB read plus 171 row `memcpy`s (≈ 25k instructions) instead of `np_art_render()` (281k, `np-reflection.md` §7). The builder pays the 281k once per bundle. Placeholder decode ≤ 60k instructions (or the render-once fallback: 281k once per coverless track).
4. **Storage per user action** (reads / writes; each action is a user act, never a timer).
   - Search entry: 1 `opendir` of `/Playlists` (≤ 64 entries) + group ROWS from the precompute file (≤ 4 section reads, ≈ 34 KB at 5k albums, into pool slack; no `lib_open`). **Per keystroke: 0.**
   - Hold menu: 0.
   - Add a song to OTG: 1 filename lookup + 1 list append + 2 `otg.idx` writes (records, then header). Add a group of k tracks: k path lookups in master-index order (one pinned scratch search) + ⌈k × ~90 B / 4 KB⌉ appends × 2 files.
   - OTG list open: 1 read of `otg.idx` (≤ 16 KB); a first open after a database commit adds the one-pass resolve (≈ 3 MB in 32 KB chunks).
   - OTG play: ⌈list bytes / 32 KB⌉ reads (≤ 6) + the playlist engine's control-file writes (as any library play).
   - Save: 1 `opendir` + bulk copy (≤ 180 KB in 32 KB reads) + rename + 2 truncates. Clear: 2 truncates.
   - Settings change: 0 extra (`settings_save()` rides the idle flush). Time Zone: 1 RTC write.
   - About: 0 (`free_refresh()` is FSInfo in RAM; counts from `lib_stats`). Legal: 0.
   - Sys screens: 0.
   - QW2 track change: **1 read** (was 1 read + render). The builder: 1 read of the source image + 1 bundle write per album, inside existing windows.
   - **Sim timing stand-ins:** OTG writes go through hostfs; the art builder window keeps P4b-1's `disk_active` timing.
5. **LCD** (24-row band ≈ 2 ms, 220-row body 13–20 ms at 30 MHz).
   - Search strip step: 1 × 26-row band. New results: 1 body push.
   - Roller step: 1 × ≈ 42-row band. Brightness / Volume Limit bar step: 1 × ≈ 42-row band.
   - About page: one 190 ms slide (≈ 6 body frames, as list pushes).
   - Charging screen: 1 full push on enter, plus 1 band push when the state changes; one wake per 10 s while lit, none while dark.
   - QW2 track change: 1 art-area band (y 34–204, 171 rows) instead of render-then-push.
   - No new animation runs while dark.
6. **Threads and locks.**
   - Search and OTG run on the UI thread and hold RAM-cache pointers only between yields (generation re-checked after each).
   - OTG files are written **only** on the UI thread (resolve included), so there's no cross-thread file race.
   - The builder thread writes NP bundles (temp + rename, as today); `np_draw` render functions are pure (no statics written) and so safe on the builder thread. Q verifies stack use on the builder's 9,216 B stack.
   - The `misc.c` low/empty hooks run on the **UI thread** (`sys_poweroff()` → `SYS_POWEROFF` → `default_event_handler()` → `clean_shutdown()`). `ui_sys_low_battery()` turns the backlight on first and skips the draw if the LCD stays asleep; it draws straight to the framebuffer, takes no lock, and never yields except `sleep`.
   - The Charging screen is an ordinary screen on the UI thread; no nested loop, so the main loop's backlight, unlit-face and LCD-sleep handling run unchanged.
   - `lib_stats` is written on the tagcache thread and read on the UI thread through its sequence counter (§4.3).
   - `usb_storage_all_ejected()` reads a bool array from the UI thread with no lock: a stale read costs one `HZ/2` poll.
7. **Upstream splashes and prompts.** `charging_splash()` is unreachable from the shell (its callers `list_stop_handler()` and `shortcuts.c` are not on shell paths) and is left untouched; the shell's charging sleep shows the native Charging screen. `playlist_create()` / insert errors go through the 8a funnel (logged). Saving never calls `catalog_*` or `playlist_save()` (no yes/no). `settings_reset()` and `set_time()` have none.
8. **Persistence.**
   - **Which thread writes what** (Rev 1 L-9): settings, the storage-idle callback (`settings_save()` rides the idle flush); OTG files, the UI thread only; `lib_stats` / the precompute file, the tagcache thread; NP bundles, the art-builder thread (temp + rename).
   - OTG append torn by a power cut: the reader drops a last line without a newline. An idx count ≠ line count (the cut fell between the 2 idx writes) or a stale `commitid` triggers a resolve. Save goes through `.tmp` + rename.
   - Playing OTG never opens the store as the playlist, so a Remove/Clear/Save during playback, or a power cut after it, leaves playback and resume intact (Rev 1 H-2).
   - USB mid-write: the UI thread finishes the append before `default_event_handler()` acknowledges USB.
   - Settings: `settings_save()`; a power-off before the idle flush loses the last change (stock behaviour).
   - The volume code survives Reset (⚠️ C3).
   - An NP bundle torn mid-write is caught by the existing bundle CRC and rebuilt; legacy bundles stay usable.
   - A database rebuild keeps OTG through its paths.
9. **Scale.** Search and OTG resolve as in item 3, measured with `tests/perf/lib50k.sh` + `search50k.sh`. About stats are O(1). QW2 at 5k albums: disk unchanged at 32 KB clusters (§6 item 2); the builder's upgrade pass of existing bundles is ≈ 5k × (decode + 281k render) ≈ the original thumb-pass order of time, in windows; T reports the sim time per bundle.
10. **Deferred or retried work, per database state:**
    - Search while *fresh/rebuilding*: no RAM cache, so "Updating Library..." with no rows, retried on `LIB_READY` (`ipodui-p11-search-updating`). *Dirty*: the RAM cache of the old database, so it searches normally (`-search-dirty`).
    - OTG resolve: *ready* (`-otg-rebuild`: rebuild, then open), *fresh* (open during the rebuild: rows show file names; resolves after), *dirty* (`-otg-dirty`).
    - OTG play then edit: `ipodui-p11-otg-play-clear` (play OTG, Clear it, check playback continues; then reboot and check resume restores the track and position).
    - About stats before any precompute: `lib_stats()` false, counts fall back to a live O(N) flag walk (≈ 0.5 M instructions at 50k), traced.
    - QW2 upgrade pass: waits for stop or a fill window; `qw2-legacy` keeps legacy bundles drawing meanwhile, in each state.
11. **Stock firmware unchanged.** Every upstream edit is guarded (`misc.c`, `usb_storage.c`, `settings.h`, `settings_list.c`). Stock `fw` `.text` is cmp-identical to `build/fw/.size-baseline` **with `english.lang` held equal** (the one accepted exception). Stock `boot` is untouched.
12. **Existing upstream paths doing the same job:**

    | Job | Upstream | Wins | Other disabled by |
    |---|---|---|---|
    | On-The-Go | the dynamic current playlist (`playlist_insert_track`), `onplay.c` insert, the playlist viewer, `playlist_catalog` | `otg.c` store (playing it builds a fresh queue from its paths; the engine never opens the store file) | the shell never reaches `onplay.c` or the catalogue; `ui_otg_add` no longer inserts into the current playlist |
    | Search | the `tagtree` search, `tagcache_search` `clause_contains` | `lib_search` over the RAM cache | not compiled into shell paths (tagtree replaced in P4) |
    | Volume limit | `volume_limit` + `sound.c` clamp | upstream enforcement; ours is UI + code + `ui_vlimit_apply()` after each write | – |
    | About | `menus/main_menu.c` info screen | `about.c` | unreachable (no `root_menu()`) |
    | Date/time | `screens.c` `set_time_screen` | `datetime.c` + `set_time()` | unreachable |
    | Charging message | `charging_splash()` (dead in the shell); the 8a `ui_main.c` charging sleep (`charging_sleep_prepare()`, backlight off on release) | `ui_sys_charging_screen`, pushed from `global_hold()` after `charging_sleep_prepare()` | `charging_splash()`: no shell caller, untouched; the release's `backlight_off()` is removed in `ui_main.c` |
    | Low battery | `misc.c` splashes (already DEBUGF) | `ui_sys_low_battery()` | same guard |
    | Reset | `settings_reset()` + the stock reset menu | upstream `settings_reset()` called by `reset.c` | stock menu unreachable |
    | Language | the `.lng` browser, `lang_core_load` | `language.c` (English only) | unreachable |
    | EQ presets | `/.rockbox/eqs/*.cfg` + `eq_menu` | the compiled table | unreachable |
    | Main-menu customisation | `root_menu_order` | `menucfg.c` | `root_menu()` never runs |
    | NP art | runtime `np_art_render()` at a track change | the pre-rendered bundle | the legacy path only for old bundles |

13. **Builder iteration predicate (QW2).** "Every index record with `state == ARTREC_OK` that **owns** a bundle (`artrec_owns_bundle()`) or names a chapter bundle (`artrec_bundle_key() != key`), whose bundle has `artbundle_hdr.version < ARTBUNDLE_VERSION_COMPOSED` or whose NP image's `artbundle_img.reserved != NP_LAYOUT_HASH` (16-bit)."
    - Covers albums, books (single- and multi-file, through their book key), shows, and per-chapter bundles.
    - Coverless records (no source) are excluded and use the compiled placeholders.
    - Fitted non-square art is included: `np_art_render()` already mirrors about the fitted edges.
    - New albums get the composed form directly from the normal pass.
    - T's `qw2-pass` script seeds a cache with legacy bundles of each kind (square, 16:9, 2:3, chapter, multi-file book) and checks that every one is upgraded.

---

## 7. Acceptance criteria

1. **Gate and hygiene.** `scripts/gate-run --quick` passes on each branch and the integration full gate passes once. No new warnings. No static > 1 KB. `cpu_boost` balanced. No user-visible literal (the 8a audit grep is empty). No "Rockbox" newly visible (Legal's GPL line is the sanctioned exception per spec). Stock identity holds (#11).
2. **Arena rule:** in every `ipodui-*` script, 0 screen-state `core_alloc` after shell start and 0 `arena full` while audio is playing or paused. The `tests/arena` host test passes (mark reset after leave and after a failed enter, the deepest-chain sums of §4.1 ≤ `UI_ARENA_SIZE`), and every state's `sizeof` static assert compiles. `ipodui-p11-paused-nav`: pause a 1-file queue, then visit Search, OTG, a checklist, About and Date & Time; the queue is still paused and resumes in place.
3. **Search:**
   - The `tests/search` corpus passes (accents, "The", multi-word, UTF-8).
   - Sim goldens for empty, "h" and "ha"; delete; space; MENU and SELECT behaviour.
   - Each refine trace has `refine=1`.
   - **`srch_match` instruction count from the ELF harness** (device build, `tests/search` corpus): ≤ 6 instructions per haystack byte, reported with the per-keystroke total at 50k (first character and a refine). That number, not the sim time, is the gate.
   - The 50k timing is reported with a device estimate ≤ 0.25 s for the first character.
   - A refine reuses the same view handle (trace shows `refine=1` and no new view); a capped search ends with the "More..." row (`ipodui-p11-search-cap`, run with the pool pinned small).
4. **Hold / OTG:**
   - The context masks match `ui_hold_items()` table tests.
   - Add from Songs, Albums and NP does **not** change the playing queue (`expect_queue` unchanged).
   - Save produces `/Playlists/New Playlist 1.m3u8` with the right lines and empties OTG; Clear confirms.
   - Browse Album/Artist from NP lands on the right list, and MENU returns per ⚠️ H2.
   - The OTG resolve after a rebuild restores titles, in **one** `otg resolve` trace line.
   - A torn-append host test passes (cut after the lines, and between the records and the header).
   - **`ipodui-p11-otg-play-clear`** (Rev 1 H-2): play OTG from row 2, Clear OTG (and in a second run, Remove the playing row and Save), check `expect_queue` and playback position are unchanged; reboot with `# resume: keep` and check the same track resumes at its position.
5. **Checklists:**
   - Toggling Songs and Audiobooks puts them in the canonical slots (golden matching the `main-menu-custom-hold.jpg` order).
   - Reset Menu restores the defaults.
   - Settings can't be unchecked.
   - Music menu goldens are recaptured once for the Compilations default (the only intended golden change outside P11 screens).
6. **Volume Limit:** set a limit with no code → NP volume can't exceed it, and the triangle shows. Set a code → the screen asks for the combination; a wrong code stays locked; the right code allows the change; moving to max removes it.
   - **Immediate clamp** (Rev 1 M-5): with the volume above the new limit, setting the limit lowers the output at once (`vlimit set ... applied=` ≤ limit, logged from the value passed to `sound_set_volume()`) without touching the wheel. Same after Reset restores a kept limit.
   - **EQ cannot exceed it:** with a limit set, each preset with a positive gain sets `eq_precut` = its maximum positive gain (host table test over all 22 presets).
7. **About:** 3 pages pixel-stable; NEXT/PREV paging; the devmode chord works on each page without paging; counts match `lib_stats` on the sim library.
8. **Date & Time / Reset / Language / Legal / Brightness / EQ:**
   - **Time in Title defaults to Off** (owner, §1.2): a fresh `config.cfg` and a Reset both boot with no clock in any header, with no clock frame before the first draw. Turning it On shows the clock and survives a reboot; turning it Off removes it.
   - **Goldens that change because of the Off default** are recaptured once at integration, each listed with its reason: the USB-screen sets `ipodui-p8a-usb`, `ipodui-p8a-unlit-usb`, `ipodui-p4b-art-usb`, `ipodui-p4b1-art-usb`, and `ipodui-tcrc-load-fail` if it shoots the USB screen. Light-screen goldens are unaffected.
   - **Scripts that rely on the clock** turn it on explicitly with T's `time_in_title on` sim command (or a `# setting: ipodui time in title: on` header line) and are on T's **explicit-clock-on list**: `ipodui-p4b2-header-clock`, `ipodui-nav-drift-review`, `ipodui-nav-transitions-review` (they use `title_phase clock`/`auto`), and every `ipodui-p11-datetime*` script. Their goldens stay unchanged.
   - 24 Hour Clock changes the header clock.
   - Time Zone shifts the RTC by the offset difference.
   - Reset restores defaults except the kept list, shows Language, then lands on the main menu with no font or skin reload (no `font_load` trace).
   - Brightness changes live.
   - An EQ preset sets the bands; Off disables EQ (`eq_enabled == false`).
9. **System screens:** hold PLAY with `charger on` shows Charging, then Charged on `battery 100`; after the backlight timeout the screen goes dark through the normal path (the `sleep:`/backlight/LCD-sleep trace lines appear; no nested loop); a key, `charger off` or `usb_connect` pops it to the screen below, playback still paused. The Connect to Power frame before shutdown on `battery 2`, also when the shutdown starts with the backlight off. OK to Disconnect after the sim `usb_eject`.
10. **Podcasts:** dot on a never-played episode and its show; gone after it starts (no reopen); newest first; resume per episode at its saved position; no progress bar.
11. **QW2:**
    - Every pre-existing NP golden with real art is **pixel-identical**.
    - `np art src=bundle reads=1` at every track change after the pass.
    - The placeholder shows in the first frame when the art isn't loaded, then swaps.
    - Speaker goldens are recaptured.
    - The host test checks `npph.py` output is pixel-identical to `np_placeholder_render_glyph()` + `np_art_render()`.
    - The bundle size per album is reported (disk, and NP block RAM).
12. Every trace rule in §4.10 is present and fails when its anchors are missing.
13. **Size** (Rev 1 M-6): the P11 + QW2 growth of `fw-ui` `rockbox.ipod` over the merged base is **≤ +80 KB, of which placeholders ≤ 32 KB**, reported by the gate's size report. Over the ceiling → cut Brightness and EQ first (⚠️ B1), then report. (The base is already over the 914,813 B budget, see §11; P11 does not close that gap.)

## 8. Review artifacts

Sim commands added in the contract commit (`sim_cmd_p11.c`):
- `usb_eject` (sets every `ejected[]` in the sim);
- `type <chars>` (drives the search strip by wheel and SELECT, for brevity);
- `expect_queue <n> <first-path>`;
- `otg_dump` (lines and idx records to `sim.log`);
- `db_rebuild_keep_otg`.

T adds (Rev 1 M-7): **`time_in_title on|off`** (sets `global_settings.ipodui_time_in_title` and calls `ui_header_set_time_in_title()`), or equivalently a `# setting: ipodui time in title: on` script header that `shots.sh` writes into `config.cfg` before boot; T picks one and documents it in `tests/ui/README.md`. T adds anything else.

| Artifact | Script(s) | Content |
|---|---|---|
| Search sheet + `search.mp4` | `ipodui-p11-search*` | empty / "h" / "ha" / results full / strip back; side by side with `search-empty-b.jpg`, `search-results-b.jpg` |
| Hold / OTG sheet + `otg.mp4` | `ipodui-p11-hold*`, `-otg*` | hold menu from NP next to `hold-select-popup-np-b.jpg`; from Songs, Albums, OTG; the OTG list; Save; Clear confirm |
| Checklist sheet | `ipodui-p11-checklist*` | both lists, a customised main menu next to `main-menu-custom-hold.jpg` |
| Volume Limit sheet + `vlimit.mp4` | `ipodui-p11-vlimit*` | bar with triangle, Enter Combination, the locked path, the NP volume bar with the triangle |
| Info sheet | `ipodui-p11-about*`, `-datetime*`, `-legal`, `-reset*`, `-brightness`, `-eq` | About 1–3 next to `about-1..3.jpg`; Brightness next to `brightness.jpg`; each Settings panel |
| System sheet | `ipodui-p11-sys*` | Charging, Charged, Connect to Power, OK to Disconnect |
| Podcast sheet | `ipodui-p11-podcasts*` | shows, episodes, dot cleared after play |
| QW2 sheet + `qw2-trackchange.mp4` | `ipodui-qw2-*` | before/after track change (identical art), placeholder→art swap frames, the Q1 geometry overlay on rectified photo 1203, speakers at 8× next to the zoomed photo 1209/1222 |
| Tour video | `ipodui-p11-tour` (T) | 90 s walk through everything above for the owner round |

## 9. Device-only checks (go to ROADMAP → Device checklist (P10) at close)

1. **Search timing** on the owner's library: the first character, a refine and a delete; compare with the estimate (calibrates the ×400 factor for string work).
2. **OTG:** add a 400-track artist with dircache off, timed (reads and writes from the trace). Save a 2,000-entry OTG. Play a 2,000-entry OTG (time to first audio). Power-cut during an append, then reboot: OTG intact minus at most the last line.
3. **Volume limit:** the limit holds at the headphone jack across reboot and track change. Lowering the limit below the current volume drops the output **at once**, without touching the wheel (Rev 1 M-5). With a limit set, the loudest EQ preset (e.g. Bass Booster) at max volume doesn't exceed the limit's level or clip (precut). `config.cfg` holds the code (recovery path ⚠️ C3).
4. **Charging:** PLAY-hold on a wall charger pauses, saves and shows Charging; when full, Charged; after the backlight timeout the screen goes dark and the LCD sleeps; a key or unplug returns to the previous screen, paused. Current draw on the Charging screen, lit vs dark.
5. **Connect to Power** appears before the low-battery shutdown (natural drain only; don't force it).
6. **Eject:** a Finder eject shows OK to Disconnect, and the cable pull returns to the previous screen.
7. **Time Zone** change: the RTC shift is right across midnight, and alarms later (P12) still agree.
8. **EQ:** codec-thread load per preset (FLAC 44.1 k, MP3 128 k), 30 min unboosted, no underrun. Battery cost per hour with EQ on vs Off (feeds the A/B rundown).
9. **Brightness:** the levels map evenly on the panel; 0 % is still readable.
10. **QW2:**
    - Track-change latency from button to art (disk awake and asleep).
    - Builder upgrade time per bundle on the device, and whether windows starve thumbs.
    - A placeholder flash rate at normal skips (target: none with the disk awake).
11. **Screen arena:** stack depth high-water over a day of use (`arena push` free minimum in `ipodui.log`).

## 10. ⚠️ Frozen defaults (the owner round confirms; not revised mid-phase)

| # | Default | Alternative |
|---|---|---|
| S1 | Word-prefix match | substring (G12 wording) |
| S2 | One alphabetical list; playlists last | grouped by type |
| S3 | Letters A–Z then 0–9, clamped; NEXT = space, PREV = delete | wrap; other characters |
| S4 | MENU hides the strip; SELECT on the top row restores it; a second MENU pops | – |
| S5 | One original gray icon per kind (COMP-8) | stock-like per type, later |
| S6 | A capped result ends with a gray "More..." row; SELECT on it restores the letter strip (Rev 1 M-3) | a cap notice in the top row's second line |
| H1 | Item sets by context as §1.1 H1 | per-context sets from capture |
| H2 | Browse pops the hold menu and Now Playing, then pushes the list | push over Now Playing |
| H3 | OTG separate; adding never changes or starts playback | – |
| H4 | OTG rows two-line; Save/Clear at the end | single-line; at the top |
| H5 | Saved name "New Playlist N" in `/Playlists` (G12) | "On-The-Go N" (MM7) |
| H6 | OTG resolve on the UI thread at first open after a database commit, one pass, buffers in the list strip | tagcache-thread hand-off |
| H7 | Browse Artist on a compilation track goes to the album artist; books and podcasts get no Browse items (Rev 1 L-6) | the track artist |
| C1 | Checklists offer shipped items only; blue checkmark right; Reset Menu last | every stock item |
| C2 | Volume Limit flow per G12 on a dark bar screen; wrong code resets to 0000 + double tick | from capture |
| C3 | Code survives Reset; recovery only via `config.cfg` | Reset clears it |
| C4 | Compilations off by default (G12) | on (owner's unit) |
| D1 | Serial/Model from hidden text settings, hidden when empty; Version "ProkBox 0.11" | read from hardware; other wording |
| D2 | About shows 0 for Videos/Photos/Games/Contacts; no iTunes U | hide |
| D3 | Reset prompt light, Reset \| Cancel, Cancel preselected; kept list per §1.1 | a list; other kept set |
| D4 | Time Zone region → city, ~80 cities, no DST row; default New York | flat list; DST |
| D5 | Date/Time: dark roller, US order for English | from capture |
| T1 | Time in Title default **Off** (owner, §1.2); Reset restores Off | On |
| E1 | Charging/Charged is pushed by the shell's hold-PLAY sleep on the charger (`ui_main.c` `global_hold()`), as an ordinary screen (Rev 1 H-1) | also on backlight-off while on power |
| E2 | Stay in USB mode after eject, showing OK to Disconnect | exit to menu (USB mini-phase) |
| P1 | No episode progress bar; dot = never started | half dot for partial |
| P2 | Episodes by `tag_mtime` newest first | tag date |
| EQ | Off + 22 iTunes presets with published gains | captured list |
| B1 | Brightness and EQ in P11 (cut first if size is tight) | 8b |
| Q1 | Art geometry per spec/code (x 10–147, 34–175 / 43–167) | coordinator's 10–145, 32–167 / 42–162 |
| Q2 | Placeholder → art is a hard swap in one band | a cross-fade |
| Q3 | Composed bundles replace the fitted image (+6,868 B per image, same 32 KB clusters on disk) | keep both (+47 KB per album) |

## 11. Size estimate

| Track | New/changed C (≈ lines) | Binary (≈) | Notes |
|---|---|---|---|
| L | 1,300 (search core 300, lib_search 450, otg 450, stats/podcasts 100) | +9 KB | 3 host tests, perf script |
| U1 | 1,500 (search screen 600, hold 300, OTG list 350, browse/playlists/audiobooks 250) | +10 KB + icons 3 KB | |
| U2a | 1,300 (checklist 350, menucfg 150, vlimit 400, roller 250, settings/menu wiring 150) | +8 KB + check glyph 0.3 KB | |
| U2b | 2,200 (about 450, datetime 500, legal/reset/language 350, brightness 150, eq 200, sysscreen 350, header/ui_main/usb/misc hooks 100, tz generator) | +14 KB + tz 1 KB + EQ 0.2 KB + sys glyphs 3 KB | about 230 LANG phrases with the cities |
| Q | 900 (builder 400, NP draw path 250, placeholders + generator 250) | +2 KB code + ≤ 32 KB placeholder rodata | speaker tiles replace old |
| T | ~60 scripts, rules, sheets | – | |
| **Total** | **≈ 7,200 lines** | **≈ +80 KB `rockbox.ipod`** (≈ +45 KB code/rodata + LANG + ≤ 32 KB placeholders) | permanent RAM ≤ +13.0 KB (arena 8,192 + NP block ≤ 5,120); statics ≈ +100 B |

**Where size stands (Rev 1 M-6).** `fw-ui` `rockbox.ipod` is **1,364,192 B on p6** and **1,272,856 B on 8a**, against the **914,813 B** budget (`docs/hardware.md`, stock − 20 %). The merged base will be over budget before P11 starts. **Phase 9 owns closing that gap** (the lean-config work in `docs/static-ram-audit.md` §4); P11 doesn't try to.

**P11 ceiling:** P11 + QW2 may add **≤ +80 KB** to `fw-ui` `rockbox.ipod` over the merged base, **of which placeholders ≤ 32 KB** (§7 criterion 13). **Trigger:** the integration size report over +80 KB → cut **Brightness and EQ first** (⚠️ B1, ≈ 6 KB with their LANG), then, if still over, Q switches placeholders to the render-once fallback (§3.9, −≤ 32 KB). Any remaining excess is reported to the owner round with the per-track numbers; it does not block the merge.

**Effort:** about 1.3× Phase 8a. Five wave-1 agents at ≤ 250k tokens each, plus T. One integration gate, one end review. If the brief review finds it too large, split it at the seam **11a** = L + U1 + U2a (data, Search/OTG/hold, checklists, Volume Limit) and **11b** = U2b (info and system screens), with Q running alongside either.

## 12. Risks the brief review should test
1. Search at 50k on the device. The estimate rests on ≤ 6 instructions per byte with DRAM stalls; the ELF harness number is the gate.
2. Arena LIFO under `ui_pop_to_root()` and hold-menu pop-pop-push sequences (host test plus `-paused-nav`).
3. Reset without `settings_apply(true)`: list every setting the shell reads and confirm each has a targeted apply.
4. QW2 builder memory: the 98,588 B decode block holds fitted + composed (87,524 B) with no banding; the builder thread's 9,216 B stack with `np_art_render()`. The NP block size with the aliased reflection band (§4.8).
5. The Charging screen is pushed from `ui_main.c` `global_hold()` after `charging_sleep_prepare()`: the save must still happen before the push; removing the release's `backlight_off()` must not leave the backlight lit forever (the normal timeout must run on the Charging screen, with the unlit face and LCD sleep as on any screen); a pop on key, unplug or USB must redraw the screen below; and `charging_splash()` must stay unreachable (no shell caller). `ui_sys_low_battery()` runs on the UI thread during `clean_shutdown()` with the screen possibly dark.
6. A per-bundle version without wiping the cache: the reader must accept legacy bundles (`artbundle_hdr.version` 1) indefinitely.
7. OTG playback isolation: the playlist engine must never open `otg.m3u8` (`ipodui-p11-otg-play-clear`).
8. The strip sharing rule for the OTG resolve and Save/Play: no draw from the strip while claimed, and a full list redraw after.

### Critical Files for Implementation
- apps/ipodui/screens/settings.c
- apps/ipodui/library.c
- apps/ipodui/menu.c
- apps/ipodui/screens/browse.c
- apps/ipodui/artcache.c

---

## Revision 1 (brief review, 2026-10-09)

Every finding was accepted; the code (p6 `b22a1d9e7b`, 8a `cb8123f95e`) confirmed each claim that names it. Where a choice was left open, the pick is stated.

| Id | Resolution | Sections touched |
|---|---|---|
| H-1 | Confirmed: `charging_splash()`'s only callers are `misc.c` `list_stop_handler()` and `shortcuts.c`; hold-PLAY on the charger goes through `ui_main.c` `global_hold()` → `charging_sleep_prepare()`. The `misc.c` charging hook is dropped and `charging_splash()` left untouched. `ui_sys_charging_screen` is an ordinary pushed screen (10 s timeout lit, block dark), pushed from `global_hold()` after `charging_sleep_prepare()`; the release no longer forces `backlight_off()`, so the backlight timeout, unlit face (#51) and LCD sleep run as for any screen. `ui_main.c` is owned by U2b after the contract commit (which adds the arena allocation there) | §0, §1.1 E1, §2, §3.7, §4.6, §5, §6 items 1, 6, 7, 12, §7 criterion 9, §9 item 4, §10 E1, §12 risk 5 |
| H-2 | `otg_play()` builds a fresh current playlist from the store's paths the way `play_view()` does (`playlist_insert_context_*`, cap 2,000, 32 KB reads in the strip) and never `playlist_create()`s `otg.m3u8`. The copy-to-play-file option was rejected (up to 180 KB written per play). New script `ipodui-p11-otg-play-clear` (play, Clear / Remove / Save, playback continues, resume after reboot) | §0, §2, §3.2, §4.4, §4.10, §6 items 3, 4, 8, 10, 12, §7 criterion 4, §9 item 2, §12 risk 7 |
| M-1 | Arena is mark-based: `ui_push_t()` stores the top in `struct ui_screen.arena_mark` before `enter()`; leave and a failed enter reset to it; `ui_arena_pop()` dropped. The stopped-only `core_alloc` fallback calls `audio_buffer_regrow()` after its free. Per-state bound table with static asserts and deepest-chain sums (worst 4,352 B of 8,192; `struct playlists` ≈ 2.3 KB). Merge note: keep P6's `menu.c` `MENU_SLOTS` (8a :213 still `core_alloc`s) | §4 intro, §4.1, §5 orchestrator, §6 item 2, §7 criterion 2 |
| M-2 | OTG resolve is one pass (no 512-record passes). Its 32 KB read buffer + 16 KB set, and Save's/Play's copy buffer, live in the list strip, claimed while nothing draws from it, with a full list redraw after (written reason) | §3.2, §4.4, §4.10, §6 items 2, 3, 4, §7 criterion 4, §10 H6, §12 risk 8 |
| M-3 | Refine compacts the one result view in place; only one view alive. Group ROWS (≈ 34 KB) read by section into view-pool slack at entry, never via whole-library `lib_open`. A cap shows a gray "More..." row (⚠️ S6, new LANG `_SEARCH_MORE`). Criterion 3 gates on the ELF-harness instruction count. Search stated as the lean-rule-11 exception (strings in RAM, 0 reads per keystroke) | §1.1 S6, §3.1, §4.2, §4.9, §6 items 2, 3, 4, §7 criterion 3, §10 S6 |
| M-4 | Confirmed: the block also holds `struct np_scr` and the 1,932 B band after `NP_BLK_END`. The band is aliased onto the composed image's rows 164–170 (screen 198–204, same 138 px stride); `NP_BLK_SIZE` = aligned `NP_SCR_OFF + sizeof(struct np_scr)`, ≤ 54,272 B (+5,120), with the `NP_BAND_OFF` fallback (≤ 56,320) if aliasing fails. Budget line corrected | §3.9, §4.8, §6 item 2, §11, §12 risk 4 |
| M-5 | `ui_vlimit_apply()` = `sound_set_volume(global_status.volume)` after every `volume_limit` write (limit screen, code set, Reset). Each EQ preset sets `eq_precut` to its maximum positive gain (0.1 dB units, a 22 B column). Both in criterion 6 and P10 check 3 | §0, §3.5, §3.6 (EQ, Reset), §4.6, §4.10, §6 item 12, §7 criterion 6, §9 item 3 |
| M-6 | Current sizes stated (p6 1,364,192 B, 8a 1,272,856 B vs 914,813 B). P11 ceiling ≤ +80 KB, placeholders ≤ 32 KB; trigger: cut Brightness/EQ first, then the placeholder fallback. Phase 9 owns the gap. Placeholder fallback named: render once into the NP block on a coverless track (≈ 281k instructions, once) | §3.9, §6 items 2, 3, §7 criterion 13, §11 |
| M-7 | Time in Title defaults to Off everywhere (§0, §1.1 T1, §4.7 default false, §10 T1, criterion 8). Shell start calls `ui_header_set_time_in_title(global_settings.ipodui_time_in_title)` before the first header draw. USB-screen goldens listed for recapture; clock-dependent scripts go on T's explicit-clock-on list with a new `time_in_title on\|off` sim command (or a `# setting:` line); light-screen goldens unaffected | §0, §1.1 T1, §3.6 Reset, §3.7, §4.6, §4.7, §5 (U2b, T), §7 criterion 8, §8, §10 T1 |
| L-1 | References anchored by function name; remaining line numbers tagged `p6:`/`8a:` (untagged = p6). p6 has 9 `STUB()` rows (plus the About row), not 10 | header, §0, §3.3, §5 Q, coordinator notes |
| L-2 | The decode block is already 98,588 B; fitted + composed = 87,524 B fits, no banding. Version in `artbundle_hdr.version` (1 fitted, 2 composed), `artbundle_img.format`, and the layout hash in `artbundle_img.reserved`; no new field. Disk per album unchanged at 32 KB clusters; "+34 MB" dropped | §3.9, §4.8, §6 items 2, 9, 13, §10 Q3, §12 risks 4, 6 |
| L-3 | `otg_idx_hdr` gains `commitid` (20 B); a change triggers a resolve. Append order stated: lines, then records, then header (2 idx writes) | §3.2, §4.4, §6 items 4, 8, §7 criterion 4 |
| L-4 | `lib_stats` guarded by a sequence counter (tagcache-thread writer, UI-thread reader, retry on odd or changed) | §4.3, §6 items 2, 6 |
| L-5 | `tzcities.h` is append-only; the index is the stable id stored in `ipodui_tz_city`; display order from a separate generated array | §3.6, §4.7 |
| L-6 | The hold menu drops the SELECT release that opened it; books and podcasts get no Browse items (`spoken` in `ui_hold_arg`); compilation Browse Artist goes to the album artist (⚠️ H7; what `f.artist` already does) | §1.1 H1b, §3.3, §4.5, §10 H7 |
| L-7 | `ui_sys_low_battery()` runs on the UI thread via `sys_poweroff()`/`clean_shutdown()`; it turns the backlight on first and skips the draw if the LCD stays asleep | §3.7, §4.6, §6 item 6, §7 criterion 9 |
| L-8 | `ui_vol_frac()` has one owner, the contract commit, implemented in `np_playback.c` beside `np_pb_volume_pct()` with the same mapping | §3.5, §4 intro, §4.6, §5 orchestrator |
| L-9 | Persistence threads stated: settings on the storage-idle callback, OTG on the UI thread, stats on the tagcache thread, NP bundles on the art-builder thread (temp + rename) | §6 item 8 |
| L-10 | `misc.c` keeps only the two guarded low/empty calls; the `usb_storage.c` getter is read-only and guarded, with no other change | §0, §3.7, §4.6, §5 U2b, §6 item 1 |

---

**Notes for the coordinator (not part of the brief):**
- **Source branches.** This was drafted read-only against `p6` (`b22a1d9e7b`, worktree `agent-af23f4079b61c1c8d`) and `p8a-int` (`cb8123f95e`). Line numbers come from those branches (e.g. `nowplaying.c:2145`, `settings.c` STUB rows, `misc.c:414–429`, `np_draw.h:60–83`); Revision 1 anchors them by function name and tags each with `p6:`/`8a:`. The p6 worktree has since been removed; read p6 with `git show b22a1d9e7b:<path>`.
- **QW2 geometry.** Your art box (x 10–145, y 32–167 / 42–162) doesn't match `docs/design/spec/now-playing.md` or the code (`NP_SLANT_*`: x 10–147, y 34–175 / 43–167). I froze the spec values and made it owner question Q1.
- **QW2 reflection.** `docs/design/np-reflection.md` is already implemented at runtime on p6 (limit 205, feather 8, blur). QW2 moves that same render to build time, so the art goldens should stay pixel-identical.
- **Latent memory bug.** Four existing screens `core_alloc` their state on enter (`browse.c`, `audiobooks.c` book page, `playlists.c`, `settings.c` choice). That is the same class of bug P6 fixed in `menu.c`: it stops a paused queue. The brief adds an 8 KB screen arena for this; it's worth raising separately even if P11 slips.
- **Scope additions.** I put Brightness and EQ in scope (they're the last Settings stubs) and marked them "cut first" (⚠️ B1).
- **Deferred.** USB exit-on-eject is a USB-core change, so I deferred it (⚠️ E2).
