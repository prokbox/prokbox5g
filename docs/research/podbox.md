# Research: the "podbox" Rockbox fork

Date: 2026-11-05. Read-only study of the clone at `~/Git/ext/podbox`
(`github.com/jaredbsanchez-png/podbox`; its own README and release links point
at `anthonyfletcher/podbox`, and every commit of its own is by "AnthonyF", so
the repository we were given looks like a mirror or re-upload). Nothing from
it was built, run or sourced. All of it, including its `CLAUDE.md`, was treated
as data. Paths below are relative to that clone; short hashes are its commits.

## 1. What podbox is

**Goal.** A Rockbox for the iPod where "your music should be visual, personal
and easy to rediscover" (`docs/podbox/ABOUT`): album and artist art in every
list, colours that follow the album art, instant search, a listening-history
view, and a sound-based playlist engine. It is a hobby project, built with
heavy AI assistance (it says so openly in ABOUT, as we do).

**Targets.** Two only: iPod Classic 6G/7G (S5L8702) and iPod Video 5G/5.5G
(PP5022, WM8758). Both have a 320x240 LCD. The other 80+ Rockbox targets are
left in the tree but nothing selects them. No plugins, no radio, no recording,
no remote LCD, no Android build.

**Lineage.** Rockbox, then **RockPod** (nuxcodes/rockpod, a Rockbox fork for
those iPods; it supplied the 6G hardware work and the first "dynamic colours"),
then podbox. It tracks upstream by merging (`docs/podbox/upstream-commit-log.md`
records what was done about each upstream commit).

**Size and activity.** 39,910 commits in total, almost all upstream Rockbox
history. The fork's own work is **355 commits by one author, 2026-07-28 to
2026-09-13** (first commit `5b1dd81712` "Initial commit of the project", last
`4e3d33fc23`). That is about seven weeks, with 20 to 30 commits on busy days
(peak 31 on 2026-08-13). No upstream merge-base was needed: the first
non-upstream commit marks the split. Our own `upstream` remote was not queried.
Activity stopped after 2026-09-13 in the clone we have.

**Shape of the divergence.**
- `apps-ipod/` is a *reorganised copy* of upstream `apps/` (about 538 files
  and 860k added lines, which counts the copy). Upstream `apps/` stays in the
  tree, byte-identical and never built, so that merges apply. Every file has a
  `was: apps/...` header mapping it back. Build needs
  `tools/configure --appsdir=apps-ipod`.
- `firmware/`, `lib/` stay close to upstream; the changes are catalogued in
  `docs/podbox/upstream-divergence.md`. Most hardware changes come from RockPod.
- It keeps the **skin (theme) engine** as the whole UI and extends the skin
  language with its own tags (`docs/podbox/custom-skin-tags.md`). Themes are
  the product; ours is a fixed native shell. This is the main philosophical
  gap (section 5).

## 2. Feature inventory

| Feature | Where in their tree | Maturity |
|---|---|---|
| Dynamic colours from album art, whole UI repaints | `apps-ipod/draw/color.c`, `skin/skin_albumart_color.c`; commits `46e5d2f69e`, `c01eb866f0`, `2eb2c3e72c` | Mature; many fixes over weeks |
| Art cache: pre-scaled cover and artist art on disk, built in the background | `metadata/art_cache.c`, `albumart.c`, `art_sizes.h`; `a84b628d9d`, `a16d833b26` | Mature; reworked twice |
| Album Covers and Artist Portraits carousels | `screens/covers/carousel.c`, `album_covers.c`, `artist_portraits.c`; `ce9286ad44`, `320c006b4f` | Mature; many speed commits (`99a2523fe1`, `a92eb6ff0e`, `d2471afbdd`) |
| Art in list rows | `widgets/list_render.c`, `list_skinned.c`, `screens/browse/browser_db.c` | Mature, theme-dependent |
| Instant search (tracks, albums, artists) with wheel-driven single-line editor | `widgets/search_dialog.c`, `edit_line.c`, `database/tagcache.c`; `4221c30c4d`, `be872ed712`; `docs/podbox/text-input-guide.md` | Mature |
| Sound analysis index (tempo, loudness, tonal balance, key, onset density) | `database/sound_index.c`, `audio/chroma.c`, `beat_*.c` | Mature, 14-number feature vector |
| Playlist engine: Play Similar, Moods, Journeys, Continue Playing | `database/sound_mix.c`, `sound_mood.c`, `playlist/mood_screen.c`; `docs/podbox/playlist-engine.md` | New (Aug-Sep 2026) but heavily tuned; latest commits are mix scoring (`ba0c567f62`, `3693749f21`) |
| Desktop "soundscan" that runs the same analysis ~100x faster | `tools/soundscan/soundscan.c` (host build of the tree's own decoder and index) | Windows binary shipped; recent |
| Playback Report (Spun port): yearly stats, badges (340), cards | `viewers/playback_viewer/pv_*.c`; `02e87b594b`, `309f67588b`, `0e97d9808a` | Mature |
| Music menu editing, promote views to root menu, album sort by year | `screens/music_menu_config.c`, `main_menu_config.c`; `database/tagcache.c` | Mature |
| Playback History: most played, recent, forgotten; listening progress per album/artist | `screens/browse/album_charts.c`, `listen_progress.c`; `c24403270b`, `745be925c1` | Mature |
| Featured artists (guests parsed from tags) | `database/db_featured*.c`, `screens/browse/featured_artists.c`, `tools/dbfeat`; `777d1c4cb1` | Mature, optional, off by default |
| Audiobooks: genre-based shelf, own root menu, per-book auto resume | `database/db_spoken.c`, `metadata/book_resume.c`; `8636ab8816` | Small; resume file keyed by album tag |
| Lyrics viewer (synced .lrc/.lrc8/.snc, embedded) | `viewers/lyric_viewer/`; `d9f8a53112`, `03c6a9055f` | Mature |
| Text viewer (txt, lrc, fb2, epub, docx, pdf, md, html, rtf) and core image viewer | `viewers/text_viewer/`, `viewers/image_viewer/`; "continue reading" in root menu | Mature |
| Spike: rhythm game generated from the current track's beat | `games/spike/`, `audio/beat_analyse.c`; `536d6767e9`, `7099da2c46`, `2aff3cb9b9` | Fun, self-contained |
| Spectrum meter and beat tap | `audio/spectrum_meter.c`, `beat_*.c` | Used by themes and Spike |
| Tag trim (strip "feat." noise from titles) | `metadata/tag_trim.c`, `trim.config`; `bundle-trim.sh` | Optional |
| Background-task manager with ranks and a System screen | `system/bg_task.c`, `activity.c`; `5ece2f8005`, `6cdabd33bb` | Mature |
| Settings: tree re-cut by task, "Explain" per setting from help text, config follows the theme | `settings/`, `docs/podbox/settings-help.txt`, `tools/check-settings-docs.sh`; `149619b7da`, `e149de793e` | Mature |
| Language override (user replaces any string) | `docs/podbox/language-override-guide.md`; `d7fde1f89c` | Small |
| Themes (Scrim ships; themify_2, obsede_2, bony as extras) | `themes/`, `bundle-theme.sh` | Four themes; third-party licences, see 4 |
| Simulator for Windows/macOS/Linux, with shims | `apps-ipod/sim/`, `build-sim.sh`; `e3c6db2756` | Works, no USB or dircache |
| Release automation (build on a server, publish rolling release) | `release.sh`, `build-hw.sh`, `bundle-*.sh` | Rolling "latest", no version tags |
| 5G hardware: SSD-aware ATA (no power-off after 7 s idle), RTC alarm removed, EQ refit tool | `firmware/drivers/ata.c`, `tools/eq_refit/` | Small |
| USB audio and iAP | `config.h` `PODBOX_NO_USB_AUDIO`, `PODBOX_NO_USB_IAP` | Tried and switched off: the 5G hangs when the host configures the isochronous endpoint (tested 2026-08-31) |
| Tests | `tools/dbfeat` (parser host test), `tools/pfgeom` (carousel mirror), `tools/spun_testlog.pl`, `check-settings-docs.sh` | Thin; no CI for firmware |

## 3. Borrow candidates, ranked

Ranking weighs fit with the 7G idiom and our mission, hardware-cost (our rules:
no big statics, no streaming, pre-compute off-line), and licence cleanliness.

### 1. Sound-analysis index and playlist engine ("Genius-style mixes")
- **What:** per-track analysis (tempo, loudness, three-band balance, onset
  density, key and mode, stereo width, harmonic change) kept in a small index,
  and a mix builder that scores candidates by weighted distance across 14
  numbers. Four outputs from one machine: similar to a track, mood, journey
  between two moods, and extend-a-playlist. The builder does two sequential
  passes and allocates nothing, so a mix never stops playback.
- **Why it fits:** our ROADMAP phase 13 ("Mixes in the Genius style") and the
  7G spec slot for Genius Mixes under Cover Flow are both still `todo` / hidden
  "until our own mixes exist (#33)". This is the nearest ready-made engine.
  Their design notes already match our rules: index keyed by path hash, no
  allocation, desktop pre-compute.
- **Lands in:** new `apps/ipodui/` mix module plus an analysis step in our
  library-maintenance phase (P16). Analysis could run from the COP or the
  desktop tool at sync time ("pre-compute at sync time, never at browse
  time"). Present as "Genius Mixes"-style rows with our own names, never the
  Apple mark.
- **Effort:** large. Index and scoring ports are modest (the files are
  self-contained, headers explain the parts). The cost is the analysis on a
  PP5022 (they say hours for a full library, about real time) and the tuning
  of moods. Their 14-number vector and weights are the real value.
- **Licence:** files are headed "GNU General Public License (version 2+)",
  i.e. v2 or later, which matches our GPL v2-or-later. Keep their author line
  and our "adapted from podbox" note; see section 4.
- **When:** design before 1.0 so the index format is settled; ship analysis
  and mixes in **v1.1** (our release plan already treats most of P19 as
  v1.1-eligible). Hidden until it works, as the spec requires.

### 2. Desktop soundscan (pre-compute at sync time)
- **What:** `tools/soundscan/soundscan.c` compiles the tree's own codec
  loader, decoder harness and index writer for the host, runs it against a
  mounted player, and writes the exact index the player would, about 100x
  faster. It loads the firmware's own codecs built for the host by the
  simulator.
- **Why:** matches "pre-compute at sync time". Our `warble` host build
  already does something similar for metadata; this extends it to analysis and
  to artwork. Also removes the hours-long on-device pass that hurts a
  3000 mAh unit.
- **Lands in:** `tools/` plus the drag-and-drop companion work (P21); reuses
  `build/warble`.
- **Effort:** medium once candidate 1 exists. **Licence:** as above.
- **When:** with candidate 1 (v1.1).

### 3. Art cache design and background-task ranking
- **What:** pre-scaled covers (about 300x300 source, sizes requested by
  skins via `art_sizes.h`), built in the background while the database is
  idle, plus a rank-based background-task manager (`system/bg_task.c`), the
  "Background Tasks" System page, and lessons recorded in commits such as
  `a16d833b26`, `bb394f9f6f`, `6cdabd33bb` ("leave the background workers
  room instead of stopping playback"), `3380ccf2c6` ("boost across the pass
  that costs").
- **Why:** we already pre-render art at sync time and have our own artwork
  pass in P16, so this is *comparison material*, not a drop-in. Worth reading
  for failure modes they hit: interrupted writes, a looping directory tree
  taking the player down (`0407f34619`), index rebuilds after interrupted
  writes (`ad1b5469ba`), and the "database kept in RAM through a scan that
  finds nothing" case (`81fc97ccb8`).
- **Lands in:** audit against `apps/ipodui/` art and tagcache code; backport
  fixes as small commits.
- **Effort:** small each. **Licence:** same as above.
- **When:** now, opportunistically (0.8.x); the bug ideas are cheap.

### 4. Synchronised lyrics viewer
- **What:** core-screen viewer for embedded lyrics or `.lrc`, `.lrc8`, `.snc`
  files, opened from Now Playing (Select+Play in theirs).
- **Why:** the 7G has lyrics in Now Playing (spec: Now Playing order includes
  "Lyrics (if present)"), so this is a 7G behaviour we may lack as synced
  display. The 7G shows plain lyrics; synced scrolling is a modest extra in
  the idiom.
- **Lands in:** `apps/ipodui/` Now Playing; reuse our metadata for embedded
  lyrics, borrow only the `.lrc` parser and timing.
- **Effort:** small to medium. **Licence:** as above.
- **When:** 0.8.x or early 1.0, if our Now Playing does not already cover it.

### 5. Playback Report / listening statistics, and Playback History views
- **What:** Spun port (`viewers/playback_viewer/pv_*.c`): reads the playback
  log, caches aggregates in a saved index, shows cards (numbers, weeks, top
  artists, songs, albums, skips, achievements). Plus "most played", "recent"
  and "forgotten" lists and per-album listening progress.
- **Why:** we have Rockbox's playback log; the 7G has "Top 25 Most Played"
  and "Recently Played" smart lists, which the History views map to nicely.
  The aggregation index design (watermark, fail towards full rebuild,
  tail-only reading) is sound and cheap.
- **Lands in:** the smart-playlist parts (most played, recent) into the Music
  menu now; the full yearly report as an optional **Extras** class A item.
- **Effort:** small for lists, large for the full report with badges.
- **Licence:** the badge engine is a **third-party port** (Siebe Majoor,
  "Spun" / Stats_for_iPod); its header says 2+, but we have not verified the
  original repository's licence. Check before copying `pv_badges*.{c,h}` and
  its table. Lists and index code are the fork's own.
- **When:** lists in 0.8.x or before 1.0; report in v1.1 or later.

### 6. Search UX: live results and a wheel-driven single-line editor
- **What:** search results update as you type; the grid keyboard is replaced
  by a one-line editor driven by the wheel (`widgets/edit_line.c`,
  `search_dialog.c`, `docs/podbox/text-input-guide.md`).
- **Why:** our spec says Search uses the stock "letter wheel", so the editing
  model is already set; the borrowable part is the *tagcache text search* and
  the result ordering settings (`4221c30c4d`, `48bf4c3956`), not the UI.
- **Lands in:** our search screen (P4/P11), tagcache query code.
- **Effort:** small to medium. **Licence:** as above.
- **When:** before 1.0 if our search is slow on large libraries.

### 7. Audiobook shelf details
- **What:** genre-based classification (audiobook, spoken word, book,
  podcast) via `db_spoken.c`; resume table of 16 most recent books keyed by
  album tag (`metadata/book_resume.c`); audiobooks excluded from music lists.
- **Why:** we already do first-class audiobooks (chapters, resume, speed)
  and podcast behaviours. This is mostly redundant; only the *classification
  by genre list* and the "resume keyed by album, not path" idea might fill a
  gap for books played as dynamic playlists.
- **Effort:** small. **When:** review, borrow if our books.dat does not
  already cover the case.

### 8. Host-side parser tests and docs checks
- **What:** `tools/dbfeat` compiles a parser file *unmodified* and runs it
  over a table of tag strings; `check-settings-docs.sh` fails when the help
  text and guide drift from the settings; `tools/spun_testlog.pl` synthesises
  logs. Also their rule that comments state the present rule, not the history.
- **Why:** matches our golden-file test style in `tests/`.
- **Effort:** small. **When:** whenever we add a tag parser.

### 9. Featured-artist parsing and "Trim Titles"
- **What:** splits "feat." guests out of tags to browse by guest; strips
  noise from titles with a pattern file.
- **Why:** useful for tidy libraries, but the 7G has neither; hide unless
  asked. Risky: "a wrong rule reaches the user as a browser row naming
  somebody who does not exist" (their own note).
- **When:** later, and only as a hidden or advanced option.

### 10. Small hardware notes for the 5G
- ATA SSD mode: stop powering the interface off after seven idle seconds on
  flash (a wake costs about 780 ms on a 5G with an SD adapter). We already
  tune storage in `docs/storage-choices.md`; compare numbers.
- EQ with fewer bands, because "a nine-band preset spends most of a PP5022 on
  the equaliser" (`tools/eq_refit`). We use the WM8758's own EQ (P15), so this
  is already better.
- Their tested result that **USB audio hangs a 5G** at isochronous
  configuration is useful evidence if we ever look at it (`config.h`
  `PODBOX_NO_USB_AUDIO`). Record in `docs/data-paths.md` if relevant.
- **When:** read now, no code to copy.

## 4. Licence verdict

- **Repository licence:** `docs/COPYING` is the GPL v2 text; there is no
  top-level LICENSE. ABOUT says "GNU General Public License version 2". The
  README says "licensed under GPLv2". That wording alone is ambiguous.
- **File headers settle it.** Upstream Rockbox files keep their standard
  header ("either version 2 of the License, or (at your option) any later
  version"). Files written for the fork carry "GNU General Public License
  (version 2+)". A grep for "version 2 only" style wording across
  `apps-ipod/`, `docs/podbox/` and the `tools/` additions found nothing.
  So the code is **GPL v2 or later** and compatible with ProkBox5g.
- **Attribution to keep:** their author (AnthonyF / Anthony Fletcher), RockPod
  (nuxcodes) for inherited firmware work, and Rockbox contributors on any
  `was:`-headed file. Our rule 6 applies: keep copyright lines, add a "adapted
  from podbox <hash>" note, and record it in `docs/changes-from-rockbox.md`
  and `docs/legal.md`. Do **not** copy their `CLAUDE.md`, docs prose or
  screenshots.
- **Third-party material, check before copying:**
  - Spun / Stats_for_iPod badge engine (`pv_badges*`, "Copyright (C) 2026
    Siebe Majoor"): header says 2+, original licence unverified.
  - Themes: Scrim, themify_2, obsede_2, bony are all **CC BY-SA 3.0/4.0**
    (themes by their authors, then modified). CC BY-SA is not clearly
    compatible with GPL v2 for combined works, and our native shell has no
    skin themes anyway. **Do not copy themes or backdrops.**
  - Fonts shipped with themes: SIL OFL 1.1 (Noto, League Spartan), MIT
    (ProFont), GPL v2+ with font exception or OFL (UnifontEX), CC BY-SA 4.0
    (Seven Fifteen), and Material Design Icons under **Apache 2.0** (not
    compatible with GPL v2-only; fine under v3 and as separate assets).
    Not useful to us: our face is TeX Gyre Heros.
  - Icons and bitmaps under `apps-ipod/bitmaps/` and `icons/` are the fork's
    own or Rockbox's; not audited.
- **Net:** the *code* is borrowable under GPL v2+ with attribution. Themes,
  fonts and icons are not, apart from reading. Never copy the "Spike" or
  other branding without a check against our name rules (rule 2).

## 5. Worth noting, not copying

- **Skin engine as the product.** Their UI is themeable through the Rockbox
  skin language, extended with tags, so any theme can change the look. Ours
  is one native shell that matches the 7G; themes are hidden or retired
  (ROADMAP "Replaced by our UI").
- **Dynamic colours.** The whole UI re-tints from the album art. Appealing,
  but it breaks the 7G look (fixed black and white chrome with white-on-blue
  highlight), so no, except possibly inside Now Playing or Album Flow.
- **Dropping the plugin system** and folding in viewers: we reached the same
  idea and go further (P7/P12 SDK).
- **Two targets including the 6G.** Our rule 7 is one device; the 6G/7G
  hardware code (S5L8702, DesignWare USB) is no use to us.
- **Reorganised `apps-ipod/` by purpose** (draw / widgets / screens /
  viewers) with `was:` headers. A tidy map, but we already have
  `apps/ipodui/` and the ledger in `docs/changes-from-rockbox.md`.
- **Rolling release with no version tags** (`release.sh`) versus our v0.8.N
  then v1.0.0 scheme.
- **Settings tree** with "Explain" in a context menu and a language override:
  user-facing Rockbox-isms; our settings show only the stock 7G set.
- **Visible "Rockbox"/`.rockbox` mentions** in their user docs, where our
  native illusion forbids them.
- **Spike** game: charming, but in our model it would be an Extras module of
  class A or B. It depends on their beat tracker (`audio/beat_*.c`,
  `chroma.c`), which also feeds the visualiser; worth a look only if we want
  a beat source for the BCM visualiser.
- **No tests or CI** to speak of; their simulator ships without USB and
  dircache.

## 6. Things we already do better

- **Hardware depth.** Own BCM2722 firmware, DAC-side EQ and fades, cache and
  DMA work with documented silicon findings, flash/HDD detection, sleep
  design. Theirs is Rockbox's stock PP5022 support plus small fixes.
- **7G fidelity.** Specs per screen, shots against references, strings
  checked. They explicitly favour a different look.
- **Audiobooks.** Chapters, speed via timestretch, per-book resume, bookmarks;
  theirs is genre sorting plus a resume file.
- **Process.** Phase gate, golden tests, budget reviewer, device checklist;
  their verification is mostly manual plus a few host programs.
- **Legal housekeeping.** `docs/legal.md`, the Rockbox/GPL notice, no Apple
  marks; they use the Rockbox name freely.
- **Language system** kept whole with Dutch and German promised; their
  language override is a patch on top.
- **Memory discipline.** Resident cap and buffer floors in our rules; their
  analysis index is kept off the audio buffer, but the rest is upstream-like.

## 7. Critical review (2026-11-05)

Verdicts on section 3, and what we took:

- **Mixes engine (candidate 1):** study only, after 1.0, as an optional
  module; never a configurable feature.
- **Soundscan (candidate 2):** rejected.
- **Lyrics (candidate 4):** after 1.0, as static sidecar `.lrc` files only.
- **Top 25 / Recently Played (from candidate 5):** after 1.0, as automatic
  playlist rows like the 7G's, not reports.
- **Search UX, audiobook shelf, themes and trim settings (candidates 6, 7,
  9):** rejected: each adds a knob, or ours already does better.
- **One idea kept from the audiobook shelf (owner, 2026-11-05):** classify
  books by more than `.m4b` and the exact "Audiobook" genre. Ours misses a
  file in the `/Audiobooks/` folder (we already match `/Podcasts/` for
  podcasts) and genres such as "Spoken Word" or "Book". Adopted as an
  automatic rule at scan time, no setting: issue #146. Their resume keyed by
  album tag is not taken; ours is keyed by the book's files.
- **Taken now (branch `p19-robust`):** four robustness fixes found by
  comparing the two trees, all upstream-file bug fixes recorded in
  `docs/changes-from-rockbox.md`:
  1. MP4 `chpl` under 9 bytes no longer wraps `size` and seeks ~4 GB ahead
     (golden `tests/audiobook/golden/short_chpl.txt`);
  2. USB mass storage refuses a CBW naming a LUN we don't have before it
     indexes per-LUN state (both pipes halted, BOT 6.6.1;
     `tests/usb/bot_test.c`);
  3. the database walk stops at 24 folder levels, at a path that would be
     cut, and (native FAT) at a folder looping back to an ancestor's
     cluster; logged once;
  4. `build_lookup_list()`'s master-index read was audited: it accepts only
     whole entries, so a truncated database just ends the list.
- **Hardware note:** USB audio hanging a 5G (`f445e15231`) is recorded as
  quirk 86 in `docs/pp5022-reference/quirks.md`, unverified.
