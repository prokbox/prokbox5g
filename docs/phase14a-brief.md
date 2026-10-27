# Brief: Phase 14a, Videos shell (prepared and dormant until playback is confirmed)

*Orchestrator brief, drafted 2026-10-12, read-only, against `s9` `0855e49b94` (Rev 0 committed at `942e09ef26`). Process: `docs/process.md` (template, checklist 1–13, lean rules 1–11). Owner decisions of 2026-10-12 are folded in (§1.1). Anything still open is frozen ⚠️ with a default (§10); the brief is not revised mid-phase for owner answers.*

*Rev 1: review folded (2026-10-12). H1–H4, M5–M10, the Lows, the missing tests and the reviewer's ffmpeg 9.0.2 measurements are applied inline; the closing table maps each finding to its sections. The owner's ROADMAP notes `de3626a7a2`/`0f2a3a9e41` (reclassifying old `.mp4`, subtitles, on-device video modes) are recorded as "don't paint into a corner" in §2.*

**Goal (owner, 2026-10-12):** "get the Videos stuff prepped so that when we confirm the Videos can be played, we can put a few on as a test." The player (`apps/video/vplayer.*`, `vpcore`, `.vix`) is built and host-tested; on silicon it waits on the SDRAM fix from the video chip session. 14a is the shell side: a Mac converter, scan-time classification, the lists, posters, the Videos menu, the video screen, Settings › Videos and About. **Everything is behind `HAVE_BCMFW_VIDEO`**, so `fw-ui`, `sim-ui` and stock are unchanged (§7 A1). When the device plays stage 6, the owner converts a few files, drags them on, and watches them from a 7G-style Videos menu.

**Out of scope:** the player and the BCM image (video chip session, #60/#64); MPEG-4 Part 2 (`mpeg4.md`); a picture-in-play overlay (waits for the BCM 2-D jobs); soft subtitles; TV out; Photos; Rentals; video playlists; podcast show art (8b; §3.5 makes it a reuse); reclassifying `.mp4` files scanned before 14a; the companion app (#53).

## 0. What exists (reuse, do not rebuild)

| Fact | Where | 14a does |
|---|---|---|
| Player calls, power rules, sim hooks | `docs/research/bcm2722/shell-contract.md` §1–§7 | Follows it exactly |
| `--bcmfw-video` defines `HAVE_BCMFW_VIDEO` (implies `--bcmfw`), sim and device | `tools/configure` :1587 | The only switch |
| Class bits in the tagcache index flag word, set at scan time | `apps/tagcache.c` `ui_classify()` :2440, bits `TAGCACHE_UI_*` 0x20–0x80 (`tagcache.h` :227); FLAG_* use 0x01–0x10 | Adds `TAGCACHE_UI_VIDEO`, `TAGCACHE_UI_TV` |
| **Every site that tests class bits** (Rev 1, H3) | `library.c` `CLASS_MASK` :150 / `class_ok()` :369; Search kind :4703 (non-podcast → `SRCH_SONG`); stats :5069 (else → songs); `lib_shuffle.c` :177 (own mask); `screens/holdmenu.c` :182 (`spoken`); `np_playback.c` :355 (`is_audiobook`); `ab_speed.c` :66 (speed) | Each handled in §3.2a, each with an owner (§5) |
| `.mp4` maps to AAC (:213) and AAC-HE (:226); `.m4v` unmapped | `lib/rbcodec/metadata/metadata.c` | `.m4v` added to the **AAC entry only**, under the flag |
| `mp4.c`: `hdlr` seen per trak; `minf` descended **only for `soun`** (:710); `stts` sums `id3->samples` (:733); `chpl` sets `lead_trim` from the first chapter (:836); `is_audiobook` for ≥ 2 chapters (:906); bitrate from the media data size (:885); chapters (Nero `chpl` and QuickTime `chap` text track, :921); `covr` (:505) | `lib/rbcodec/metadata/mp4.c` | A dedicated `vide` helper (H4), no `is_audiobook`/`lead_trim` for videos (H3, M6) |
| `mp3entry` string pointers are relocated by `MOVE_ENTRY` in `adjust_mp3entry()` | `metadata.c` :542–567 | `tv_show` added there (H2) |
| Libfile: 30 of 32 sections used; `struct libfile_hdr` on stacks (`library.c` :1349, `artfmt.c` :866 and :909, `artcache.c` :4199) and inside `bwork.thdr` (`artcache.c` :319) | `libfile.h`, ROADMAP note | `LIBFILE_MAX_SEC` 48 under the flag; +256 B per copy (§3.3, M8) |
| `panels.pak`: 32 slots of 300×300, header 792 B padded to 1 KB, `artpanel_ent.pad[1]` unused (`pad[0]` is the entry's set, `artcache.c` `PENT_SET`, #58); selection in `library.c` `pre_panels()` with a books share `PANEL_BOOKS` 8 | `artfmt.h` :310–337, `library.c` :4077, :4233 | Videos take a share of the 32 (M9) |
| Art cache over the album table; non-square thumbs fitted on blurred bars (P6 owner) | `artcache.c` :748, :1442, :1828 | Posters ride it (§3.5) |
| Playback reads the playing track's chapters when `chapter_count` > 0 (`playback.c` :1690); `ab_chapter_skip_target()` | `apps/audiobook.h` | The video screen reuses both: no new chapter memory |
| Resume: `tagcache_ui_resumes()` (audio thread, `tagtree.c` :953) | `tagcache.c` :2463 | Tests `id3->is_video` directly (M10) |
| `struct lib_filter` has 4 fields; 5 `{LIB_ANY ×4}` initialisers (`library.c` :3029, :3243, :4454, :4903, :5529) | `library.h` :137 | `season` + `LIB_FILTER_ANY` (M10) |
| About shows Videos 0 and a Video legend square | `screens/about.c` :143, :338 | Real count and segment |
| Main-menu bit `UI_MM_VIDEOS` reserved, not offered | `menucfg.h` | Offered and default-on under the flag |

## 1. Owner pre-flight

### 1.1 Owner decisions (2026-10-12; these override the contract's frozen defaults)
- **D1 Videos menu:** Movies and TV Shows are the rows. **Music Videos is dropped**: no row, no classification target; `stik` 6 is a Movie; the converter offers no music-video kind.
- **D2 Podcasts holds audio and video podcasts.** No Videos › Video Podcasts. Video episodes sit in the existing Podcasts show › episodes lists, mixed with audio episodes. A video episode opens the video screen, an audio one Now Playing. *Videos › Podcasts shortcut: not kept (agreed with the coordinator's default).*
- **D3 TV Shows:** an album-like list with each show's art; **Show › Seasons › Episodes**; season `tvsn`, episode `tves`, show `tvsh`; folder and `SxxEyy` fallbacks (§3.2).
- **D4 Movies:** posters (`covr`, or a sidecar image as music art does); chapters through `chapters.{c,h}`; the video screen skips chapters as audiobooks do, with chapter ticks on the scrubber.
- **D5 Art:** posters drive the same drift panel and thumbnails as album art, precomputed at sync time by the art cache.
- **D6 Converter:** carries poster, chapters and TV tags.

### 1.2 Photos needed (owner's stock 7G, #26; `docs/reference/7g/owner/`)
Settled already: `mov-main-panel-videos.jpg` (film strip + "No Videos"), `about-1/2.jpg` (legend, Videos row), `main-menu-custom-hold.jpg` (Videos slot). **Needed** (empty-state ones are possible today; the others need one movie and one TV episode on the stock unit, which writes to it: only with the owner's go under the #26 rules, otherwise they stay ⚠️):
1. `videos-menu.jpg`: Videos top level (rows, order, any Settings row; settles §3.6).
2. `videos-movies-empty.jpg`, `videos-tvshows-empty.jpg`: the empty-state rows' wording.
3. `videos-settings.jpg` + one per sub-screen (Start Playing values, Fit to Screen): rows, order, inline values.
4. `videos-movies-art.jpg`: Movies list with a poster (row height, thumbnail crop of a 2:3 poster, split or full-width).
5. `videos-tvshows-art.jpg`, `videos-tvshow-episodes.jpg`: show list and an opened show (is there a Seasons level on 2.0.5?).
6. `main-menu-videos-panel-content.jpg` (video): the Videos panel with videos synced (posters drifting, or glyph + count).
7. `video-select-overlay.jpg`, `video-paused.jpg`: SELECT overlay and paused look; with a chaptered movie, the ticks.
8. `podcasts-video-episode.jpg`: a video episode in a Podcasts episode list (any marker).
9. `about-1-video.jpg`: capacity bar with a Video segment.
10. `search-video.jpg`: a Search result list containing a video (settles T9).

## 2. Scope (pieces in §5)
1. Converter (Mac). 2. Scan-time classification + `.m4v`, and every class-bit site. 3. Lists, stats, `.vix` pass in the precompute. 4. Posters in the art cache. 5. Videos menu, TV and Movies screens, Podcasts mix. 6. Video screen, Settings › Videos, About, playback-speed and hold-menu sites. 7. Gate, sim fixtures, scripts.

**Don't paint into a corner (owner ROADMAP notes, `de3626a7a2`, `0f2a3a9e41`):**
- *Reclassifying existing `.mp4` later:* classification is one pure function (`vclass_classify`) of scan-time fields, so a later pass can re-run it per entry; 14a stores nothing a reclassification would have to undo.
- *Subtitles:* soft `tx3g` needs BCM compositing (later). The converter offers **burned-in** subtitles now (`--burn-subs`, §3.1).
- *On-device video modes:* the converter keeps letterbox and `--fill`. A later source-aspect encode with on-chip placement would mean reconverting; the converter's manifest records each output's profile version (§3.1), so a later version can find and redo old files.

## 3. Design

### 3.1 Converter (`scripts/prokbox-convert.py`, Python 3 + the owner's Homebrew `ffmpeg`/`ffprobe`; facts measured on ffmpeg 9.0.2)
- **Profile** (what `bcmfw/h264` decodes, `decoder.md` "Converter settings"; flags from `bcmfw/video/mkclip-real.sh`):
  `-c:v libx264 -profile:v baseline -level 1.3 -preset medium -x264-params ref=3:slices=1:bframes=0 -pix_fmt yuv420p -b:v 600k -maxrate 768k -bufsize 1536k -g <3 s of frames> -sc_threshold 0`; video `scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2,setsar=1` (exactly 320×240; `--fill` centre-crops instead); `fps` = source if ≤ 30, else half (50→25, 59.94→29.97); `-c:a aac -b:a 128k -ac 2 -ar 44100` (AAC-LC; a source with no audio gets `anullsrc`, because `get_mp4_metadata` needs a sound track to index the file); `-movflags +faststart`, `.m4v`. No `+bitexact` except `--bitexact-test` (fixtures).
- **Tags** (ffmpeg 9.0.2 mapping, measured): `title`; `media_type=9|10` → `stik`; `show` → `tvsh`; `season_number` → `tvsn` (int); `episode_sort` → `tves`; `episode_id` → `tven`. **`podcast=1` writes no `pcst`** (H1), so `--podcast --show S` writes `genre=Podcast`, `album=S` and places the file under `Podcasts/<Show>/`. Kind from options, else parsed from the source path by the §3.2 rule. Default with nothing found: Movie.
- **Poster → `covr`** (measured): the poster is first re-encoded to a **baseline JFIF JPEG ≤ 480×720** in a temp file (separate ffmpeg run), then added as a **real JPEG input** with `-map N -c:v:1 copy -disposition:v:1 attached_pic`. Never a lavfi still with `-frames:v:1` (on 9.0.2 that silently dropped the `avc1` trak). Sources: `--poster FILE`, else the source's attached picture, else a sidecar (`<stem>.jpg|png`, `poster.jpg`, `folder.jpg`, `cover.jpg`).
- **Chapters:** `-map_chapters 0` or `--chapters FILE` (ffmetadata). ffmpeg 9.0.2 writes **both** a `chpl` and a QuickTime `text` chapter trak (`tref/chap` from vide and soun) for mp4 and ipod; `chapters.c` reads both. **The first chapter is forced to start at 0** (shifted, or a 0-start chapter inserted, titled as the first) because a `chpl` first chapter after 0 becomes `lead_trim` (M6).
- **Burned-in subtitles (`--burn-subs auto|FILE`, cheap):** `auto` = the source's first subtitle stream or a `<stem>.srt|.ass` sidecar. Text subs through `subtitles=` (needs libass; the converter checks `ffmpeg -filters` and refuses cleanly), bitmap subs (PGS/VobSub) through `overlay`; rendered after the scale so text is sized for 320×240. Off by default.
- **Batch, idempotent, safe:** files or folders, recursive; `-j N` (default 2); output under `-o DIR` (default `~/Desktop/ProkBox Videos`): `Movies/<Title>.m4v`, `TV Shows/<Show>/Season N/<Show> SxxEyy.m4v`, `Podcasts/<Show>/<Title>.m4v`, so the folder rule agrees with the tags. Writes `<name>.part.m4v`, renames at the end. Keeps a manifest `.prokbox-convert.json` in the output folder (source path, size, mtime, options, profile version `1` → output); skips a source whose entry matches and whose output exists (`--force` redoes it). Nothing is embedded in the video for this. Never writes beside or over a source, never uploads anything: the owner's files stay local. `--dry-run` prints the plan.
- `--check FILE` probes any file and says whether the device will show pictures (profile, size, slices, refs, B-frames, audio, an `avc1` trak).

### 3.2 Classification at scan time (no browse-time parsing; the scanner's existing `moov` read)
`mp4.c` fills new `mp3entry` fields (§4.1); `apps/ipodui/vclass_core.c` (pure, host-tested) turns them plus the path into class bits and grouping fields; `ui_classify()` calls it and the scanner stores the grouping in existing tags.
- **Detection (H4):** when a trak's `hdlr` is `vide`, a **dedicated helper** `mp4_vide_entry()` seeks `minf/stbl/stsd` of that trak and reads only the first sample entry's fourcc; it never enters the generic container walk, so no `stts`/`stsz` of the video trak touches `id3->samples`, length or bitrate. `is_video` iff that fourcc is `avc1`/`avc3`. Cost: ≤ 3 small reads per `vide` trak, none for files without one.
- **Videos are never audiobooks (H3):** after the walk, if `is_video`: `is_audiobook` = false (`chapter_count` kept: playback's chapter load keys on it) and `lead_trim` = 0 (M6).
- Rules, first match wins:
1. **Video** iff `is_video`. Else the existing audio rules run unchanged (an m4b with a `jpeg` chapter-image trak stays a book; an audio-only `.mp4` stays a song).
2. **Podcast** (`VIDEO|PODCAST`): `pcst` = 1 (iTunes files; reader kept), genre Podcast(s), or a path component `Podcasts` (existing rule). Show = album tag.
3. **TV** (`VIDEO|TV`): `stik` 10, or a `tvsh` tag, or nearest category folder `TV Shows`, or a filename `SxxEyy`/`NxNN`. Fields, each from the first source that has it:
   - show: `tvsh`; album tag; the folder above `Season N`/`Series N`; the folder below `TV Shows`; the filename text before `SxxEyy` (dots/underscores → spaces, trimmed); **else empty** (M7: the screen shows `LANG_IPODUI_UNKNOWN_SHOW`, as `lib_unknown_album`; no "Unknown Show" text is ever stored).
   - season: `tvsn`; `Season N`/`Series N`/`SNN` folder; `SxxEyy`; else 1.
   - episode: `tves`; `SxxEyy`/`NxNN`; a leading number in the filename (`03 - Title`); else 0 (after numbered ones, by title).
   - Stored: album ← show, discnumber ← season, tracknumber ← episode, title ← `©nam` or the cleaned filename.
   - Patterns (case-insensitive): `S(\d{1,2})[ ._-]?E(\d{1,3})`, `(\d{1,2})x(\d{2,3})`; folder `^(Season|Series|S)\s*(\d{1,2})$`.
4. **Movie** (`VIDEO`): everything else, including `stik` 6 and 9, any `Movies` folder, no tag. One row per file.
- A `stik` beats the folder. Cleaned strings go into `add_tagcache()`'s existing `ui_title[MAX_PATH]` (tagcache stack, :2492), split in two halves for show and title: **no new static** (M7).

### 3.2a Every class-bit site (H3; owners in §5)
| Site | Change under the flag |
|---|---|
| `library.c` `CLASS_MASK` / `class_ok()` | `CLASS_MASK` gains VIDEO: out of Songs, Albums, Artists, Genres, Composers, Compilations, Audiobooks; `C_PODCAST` keeps PODCAST rows (video episodes included) |
| `library.c` Search :4703 | VIDEO rows skipped (no `SRCH_SONG` for a video) ⚠️ T9 |
| `library.c` stats :5069 | VIDEO counted in `vstats`, not songs; video podcast episodes count in `episodes` only (`vstats.vpodcasts` kept for logs; owner T5) |
| `lib_shuffle.c` :177 | mask gains VIDEO: never shuffled into Shuffle Songs/Albums |
| `screens/holdmenu.c` :182 | a VIDEO row gets the podcast-episode items, minus speed |
| `np_playback.c` :355 | `is_audiobook` false for VIDEO (a video podcast is not slowed) |
| `ab_speed.c` :66 | VIDEO → 1× always (the picture follows the audio clock; speed is not offered) |
| `tagcache.c` `tagcache_ui_resumes()` | `id3->is_video ||` first; no `vclass` call on the audio thread (M10) |
| `library.c` `lib_play()` | a podcast playlist started on an audio episode **skips VIDEO rows**; a VIDEO row plays alone through `ui_video_play()` (M10) |

### 3.3 Lists and stats: in the libfile, `LIBFILE_MAX_SEC` 48 (recommended over a separate file)
Classification lives in tagcache, so the lists are ordinary master-index lists built by `rows_build()`, stamped and summed by the existing header, and the album table already feeds the art cache. A separate file would duplicate stamp, sum, reader and art plumbing. Under `HAVE_BCMFW_VIDEO` only: `LIBFILE_MAX_SEC` 48, `LIBFILE_VERSION` 2 (one precompute, no rebuild); header 544 → 800 B; update `lib_hdr_size_check`, the `tracks_hdr()` loop, `artfmt.c` `libhdr_ok()`, the `artcache.c` copies. **Sections: 30 + 8 = 38** (M8), `LIB_PRE_SECTIONS` 38:
- `LIB_MOVIES` ROWS, KEY (2; title order, sort keys as Songs).
- `LIB_TVSHOWS` ROWS, COUNT, KEY, MTIME, TRACKS (5; TRACKS = each show's episodes ordered season, episode, title; seasons derived at open from the show's slice, two reads, as albums).
- `LIBFILE_CAT_VSTATS` (1): `struct libfile_vstats { int32 videos, tv, movies, vpodcasts; uint32 video_mib; }`. `video_mib` uses the existing formula (bitrate × length): the mp4 bitrate already comes from the media data size (:885), so a video's sum ≈ its file size (T5, free). `audio_mib` excludes VIDEO rows.
- Album table rows for every movie and show, `LIBFILE_ALB_VIDEO` (0x08) (+ `LIBFILE_ALB_TV` 0x10); movie key from the path (own group, as an untagged book), show key from the show string.
- **RAM (M8):** not +0. Each `struct libfile_hdr` copy grows 256 B: `library.c` :1349 (tagcache thread, precompute and UI thread readers), `artfmt.c` :866 and :909, `artcache.c` :4199 (art thread), and `bwork.thdr` (art work block, heap). V3 and V4 report `-fstack-usage` worst paths and the headroom of the tagcache, art and UI threads before and after; any thread under 512 B headroom moves its copy into its existing work block instead.

### 3.4 `.vix` pass (contract §3; M5 reworked)
`apps/ipodui/vixpass.c`, called at the end of `lib_precompute()` after the file is written (no `tagcache.c` edit). `vix_path()` needs size and mtime (`vix_file.h` :30) and tagcache stores no size, so the pass keeps a small ledger **`VIX_DIR/vix.dat`** (atomic `.tmp` + rename): `{ uint32 path_hash; uint32 mtime; uint64 size; }` per indexed video (16 B; 500 videos = 8 KB, read whole once). `tag_mtime` equals `dir_get_info().mtime`, the value `vplayer.c` :206 hashes at open, so the ledger and the player agree.
- **Per precompute:** one read of `vix.dat`, one walk of the RAM DB's VIDEO entries. A video whose `(path_hash, tag_mtime)` is in the ledger costs **no I/O**. Only new or changed videos are opened (`fstat` for size), indexed by `vix_build_file()`, and added. Ledger entries no longer in the DB → their `.vix` unlinked (name from the ledger's size/mtime), entry dropped. One `readdir` of `VIX_DIR` only when the ledger is missing or torn (rebuilds it from the directory, deleting orphans).
- Runs **only with audio stopped** (re-checked immediately before each file, no yield between), yields at `ui_precompute_event()`, ≥ 4 KB scratch from the precompute's own block, one batch. A missing index still builds at open (`vplayer_start`), and a ledger the player's open-time build doesn't update just costs one redundant check later (the `.vix` exists: the build is skipped by name).

### 3.5 Posters and the art cache (sync time; reusable for podcast show art, 8b)
- **Sources:** embedded `covr`, else the folder images the music path already looks for, plus `<stem>.jpg|png` and `poster.jpg` beside the file, through the existing `try_embedded` / `search_albumart_files` route. Nothing decoded at browse time.
- **Panel share (M9):** `panels.pak` stays 32 slots (its header is 32 × 24 + 24 = 792 B in a 1 KB block; more slots would break the format, and each is ~180 KB on disk). `pre_panels()` (`library.c`, **V3**) gives videos a fixed share `PANEL_VIDEOS` 8 ⚠️ A2, taken only when posters exist, like `PANEL_BOOKS`. The art cache reads the set as now; `ARTCACHE_PSET_VIDEOS` selects album-table rows with `LIBFILE_ALB_VIDEO`.
- **Portrait in a 300×300 slot:** a 2:3 poster is fitted to the slot's height (200×300), centred; `artpanel_ent.pad[1]` holds the content width halved (`ARTPANEL_CW()` / `ARTPANEL_SET_CW()`, 0 = full slot; `pad[0]` is already the set), so old files read unchanged and `ARTPANEL_VERSION` stays 1. The 160×240 panel pans inside the 200×300 content: ±20 px horizontal, ±30 px vertical, pans biased vertical, zoom ≤ 5 %, same dwell and cross-fade as music (`main-menu.md`).
- **Thumbnails:** the existing non-square rule (fitted on a darkened blurred copy, P6 owner), same size as album thumbnails ⚠️ A1.
- **Reuse line (8b, owner):** podcast show art adds one share (`PANEL_PODCASTS` in `pre_panels()`) and one `ARTCACHE_PSET_PODCASTS` (`LIBFILE_ALB_PODCAST` rows); the content-width path is chosen by the source's aspect, not by "video", so square or portrait show art needs no rework.
- V4 reports `panels.pak`/`thumbs.pak` growth and decode time per poster.

### 3.6 Videos menu (7G 2.0.5; ⚠️ = needs photo, §1.2)
- **Main menu:** `UI_MM_VIDEOS` offered and in `UI_MM_DEFAULT` under the flag, canonical slot after Audiobooks. Panel: posters drifting when any exist; else film strip + "%d Videos" / "1 Video" / "No Videos" from `vstats` ⚠️.
- **Videos** (split screen, as Music): **Movies, TV Shows, Settings** ⚠️ row 3: the owner's photographed 17-row Settings has no video row, so the 7G's video settings sit under Videos. `videos.c` pushes Settings through `ui_setvideos_push()` (§4.5, M10).
- **Movies:** full-width list with thumbnails (Albums idiom), title, sublabel duration ⚠️. Empty → one row "No Movies" ⚠️.
- **TV Shows:** list with show art and episode count (album-like; a grid is not 7G idiom) → **Seasons** ("Season 1" …; skipped when a show has one season ⚠️ T2) → **Episodes** ("%d. Title", sublabel duration, unplayed dot and progress as podcast episodes). Empty → "No TV Shows" ⚠️.
- **Podcasts (D2):** existing lists. A video episode's row carries a small **screen glyph** (our own, sublabel colour, right edge before the duration) ⚠️ T4. Resume and played state are the existing per-entry ones for both kinds: tagcache `lastelapsed`/`lastoffset` (`tagcache_ui_resumes()` true), played = the existing `episode_unplayed()` rule, progress bars as today. For a video, elapsed = the audio's position (the player follows it). Playing on: §3.2a `lib_play()`.
- **Hidden:** Rentals, Music Videos (D1), Video Podcasts (D2), Video Playlists.

### 3.7 Video screen (`apps/ipodui/screens/video.c`; contract §1–§2)
- **Start:** draw nothing; `vplayer_start(path)` **first**, then start the audio (one-item playlist) at the resume position per Start Playing. On false: the audio still starts and the shell shows **Now Playing** for it, logged (`ui_log("video", …)`), nothing else on screen.
- **While playing:** no `lcd_update*`, no backlight calls, no `cpu_boost`, no disk reads. Tick = the main loop's `button_get_w_tmo(HZ/10)`; each tick `vplayer_info()`: `VPC_FAILED` → Now Playing (audio continues), logged; audio track end → back to the list.
- **Keys:** PLAY pauses/resumes (`audio_pause/resume`). Wheel = volume, applied silently while playing; while paused the strip shows the volume bar. **SELECT while paused** toggles the bottom-strip overlay (progress, elapsed, remaining, chapter ticks via the Now Playing scrubber's drawing) as one full-width `lcd_update_rect` band; a second SELECT puts the wheel on the scrubber (seek audio, then `vplayer_seek`). SELECT while playing does nothing visible (interim ⚠️ ledger). **Prev/Next:** chapters via `ab_chapter_skip_target()` over playback's table (seek audio + `vplayer_seek`); no chapters: Prev restarts, Next ends the video (back to the list, marked played). Hold Prev/Next: the audio's rw/ff. Speed is always 1× (§3.2a).
- **MENU:** `vplayer_stop()`, then stop the audio with resume kept (7G), pop to the list.
- State is a ≤ 256 B static; no allocation after the audio starts.

### 3.8 Settings › Videos (`screens/setvideos.c`)
**Start Playing**: Where Left Off (default) / From Beginning (Ask hidden ⚠️ T6). **Fit to Screen**: On/Off stored (`ipodui_video_fit`), handed to the player when it gains a mode (gap: `vplayer_start()` has no FIT/NATIVE yet; converted files are 320×240 so both look the same) ⚠️ T7. Hidden: TV Out, TV Signal, Widescreen, Captions, Subtitles, Alternate Audio.

### 3.9 About
Page 2 Videos = `vstats.videos` (movies + TV episodes only; video podcast episodes count under Podcasts only, never in both rows: owner T5, 2026-10-12); page 1 Video segment = `video_mib`, every video file's size, video podcasts included (it is storage); Audio = non-video rows; Other = used − audio − video.

## 4. Contracts (orchestrator commits these, the LANG IDs and one shared stub `apps/ipodui/p14a_stub.c` on `p14a` before fan-out)
```c
/* 4.1 lib/rbcodec/metadata/metadata.h, struct mp3entry (upstream, guarded);
 * metadata.c adjust_mp3entry(): MOVE_ENTRY(entry->tv_show) under the flag (H2) */
#ifdef HAVE_BCMFW_VIDEO
    bool     is_video;      /* a 'vide' trak whose first stsd entry is avc1/avc3 */
    bool     is_podcast;    /* 'pcst' = 1 (iTunes files; ffmpeg can't write it) */
    uint8_t  stik;          /* iTunes media kind, 0 = none */
    uint8_t  tv_season;     /* 'tvsn', 0 = none */
    uint16_t tv_episode;    /* 'tves', 0 = none */
    char    *tv_show;       /* 'tvsh' (id3v2buf), NULL = none */
#endif
/* 4.2 apps/tagcache.h, beside TAGCACHE_UI_* (no-overlap check extended) */
#define TAGCACHE_UI_VIDEO  0x0100   /* plays on the video screen; never music */
#define TAGCACHE_UI_TV     0x0200   /* with VIDEO: a TV episode */
/* 4.3 apps/ipodui/vclass.h (pure; host test tests/video/test_vclass.c) */
struct vclass_in  { const char *path, *album, *title, *genre, *tvsh;
                    bool is_video, pcst; int stik, tvsn, tves; };
struct vclass_out { int32_t flag; const char *show, *title; int season, episode; };
/* buf: the caller's ui_title[MAX_PATH]; show/title point into it or into
 * 'in'; show "" = unknown (the screen shows the LANG string) */
int32_t vclass_classify(const struct vclass_in *in, struct vclass_out *out,
                        char *buf, size_t len);
/* 4.4 apps/ipodui/library.h + libfile.h (under HAVE_BCMFW_VIDEO) */
enum lib_cat { /* … appended */ LIB_MOVIES, LIB_TVSHOWS, LIB_TVSEASONS, LIB_TVEPISODES };
struct lib_filter { int artist, album, genre, composer;
#ifdef HAVE_BCMFW_VIDEO
                    int season;           /* LIB_TVEPISODES; LIB_ANY = all */
#endif
};
#ifdef HAVE_BCMFW_VIDEO
#define LIB_FILTER_ANY { LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY }
#else
#define LIB_FILTER_ANY { LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY }
#endif
/* every 4-value initialiser (library.c ×5, menu.c, audiobooks.c) became
 * LIB_FILTER_ANY in the contracts step */
bool lib_is_video(int view, int idx);              /* no I/O, from the flag */
bool lib_vstats(struct libfile_vstats *out);       /* as lib_stats() */
#define LIBFILE_ALB_VIDEO 0x08
#define LIBFILE_ALB_TV    0x10
#define LIBFILE_CAT_VSTATS 0x7b
/* artfmt.h: pad[0] is taken (the set, #58), so the content width lives
 * halved in pad[1]: ARTPANEL_CW(e) / ARTPANEL_SET_CW(e, w); 0 = full slot */
/* 4.5 apps/ipodui/screens/video.h */
void ui_video_play(const char *path, int tc_idx);  /* §3.7; never fails visibly */
void ui_setvideos_push(void);                      /* Videos › Settings (V6) */
/* 4.6 settings (settings.h / settings_list.c, guarded) */
int ipodui_video_start;   /* 0 Where Left Off, 1 From Beginning */
bool ipodui_video_fit;    /* default On ⚠️ T7 */
```
**Contracts step as committed (2026-10-12, `p14a`).** Where the code differs from the block above, the code wins:
- `artfmt.h`: `pad[0]` already holds the entry's set (`PENT_SET`, #58), so there is no `uint16_t cw`; the width is stored halved in `pad[1]` behind `ARTPANEL_CW()` / `ARTPANEL_SET_CW()` (widths even; 200 for a 2:3 poster).
- `libfile.h`: `LIBFILE_HDR_BYTES` (544 / 800) added; `library.c`'s size check uses it, so the video build compiles before V3. `struct libfile_vstats` field order `videos, tv, movies, vpodcasts, video_mib`, with `videos` = movies + tv (owner T5).
- `LIB_FILTER_ANY` already replaces every 4-value initialiser (5 in `library.c`, the `ANY` macro in `menu.c` and `screens/audiobooks.c`): `-Wmissing-field-initializers` flagged them in the video build. Same values in the default build.
- `screens/video.h` also declares `ui_videos_screen` (V5, `screens/videos.c`; the main menu's Videos row pushes it). `vixpass.h` (V3-internal): `int vixpass_run(void *scratch, size_t size, bool (*stop)(void))` returning `LIB_PRE_DONE`/`LIB_PRE_WAIT`, ledger entry `struct vixpass_ent`.
- `vclass.h` repeats the class bits as `VCLASS_PODCAST/VIDEO/TV`; `tagcache.c` checks they equal `TAGCACHE_UI_*`.
- `menucfg.h`: `UI_MM_DEFAULT`/`UI_MM_OFFERED` gain Videos under the flag; `screens/checklist.c` (unowned) got `[UI_MM_VIDEOS] = LANG_IPODUI_VIDEOS` so Settings › Main Menu labels the row. Videos sits by bit order after Search and before Podcasts (7G: Music, Videos, Photos, Podcasts).
- New-file stubs are the files themselves (owners replace them whole); `p14a_stub.c` holds only V3's `lib_is_video()` / `lib_vstats()`, which V3 deletes when it implements them.
- LANG IDs appended: `LANG_IPODUI_MOVIES`, `TV_SHOWS`, `NO_MOVIES`, `NO_TV_SHOWS`, `UNKNOWN_SHOW`, `SEASON_N` ("Season %d"), `N_VIDEOS`, `ONE_VIDEO`, `NO_VIDEOS`, `START_PLAYING`, `WHERE_LEFT_OFF`, `FROM_BEGINNING`, `FIT_TO_SCREEN`. Reused: `LANG_IPODUI_VIDEOS`, `LANG_SETTINGS`, `LANG_ON`/`LANG_OFF`, `LANG_IPODUI_N_EPISODES`/`ONE_EPISODE`. A piece needing another string asks the orchestrator (appended at integration).
- `metadata.c` `MOVE_ENTRY(tv_show)` stays with V2 (§5), as do `ui_classify()` and `tagcache_ui_resumes()`.

Trace macro: `UPDATE_TRACE`/`DEBUGF` (never `logf`); trace lines `vclass:`, `vixpass:`, `video:` checked by the scripts; a rule that finds none fails.

## 5. Pieces and ownership (worktrees; one owner per file)

| # | Piece | Agent (tier) | Files (only these) | Read list (≤ 3) |
|---|---|---|---|---|
| V1 | Converter | tooling (sonnet) | `scripts/prokbox-convert.py`, `tests/convert/*` | §3.1; `decoder.md` "Converter settings"; `mkclip-real.sh` |
| V2 | Scan-time classification | playback-audiobook (opus) | `lib/rbcodec/metadata/mp4.c`, `metadata.c` (`.m4v`, `MOVE_ENTRY`); `apps/tagcache.c` (`ui_classify`, `tagcache_ui_resumes` only); `apps/ipodui/vclass_core.c`; `tests/video/test_vclass.c`, `tests/video/test_mp4meta.c` | §3.2, §4.1–4.3; `shell-contract.md` §4 |
| V3 | Lists, stats, Search, Shuffle, `lib_play`, panel share, `.vix` pass | playback-audiobook (opus) | `apps/ipodui/library.c`, `apps/ipodui/lib_shuffle.c`, `apps/ipodui/vixpass.c` | §3.2a–3.4; `libfile.h`; ROADMAP libfile note |
| V4 | Posters, portrait panels, Videos panel | ui-fidelity (opus: visual) | `artcache.c`, `artcache.h`, `artfmt.c`, `screens/panels.c`, `preview.c`, `panelart*` (film strip, our own) | §3.5; `spec/main-menu.md`; `artfmt.h` |
| V5 | Videos menu, Movies/TV screens, Podcasts rows | ui-fidelity (sonnet) | `menu.c`, `menucfg.c`, `screens/videos.c` (new), `screens/browse.c`, `rowglyphs.h` | §3.6; `spec/main-menu.md`; `spec/index.md` split table |
| V6 | Video screen, Settings › Videos, About, speed and hold sites | ui-fidelity (sonnet) | `screens/video.c` (new), `screens/setvideos.c` (new), `screens/about.c`, `screens/holdmenu.c`, `np_playback.c`, `ab_speed.c`, `apps/settings_list.c` (guarded rows) | §3.2a, §3.7–3.9; `shell-contract.md` §1–2; `spec/settings.md` |
| V7 | Gate, fixtures, scripts | tooling (sonnet) | `scripts/validate.sh`, `scripts/shots.sh`, `scripts/gate_map.txt`, `tests/ui/ipodui-p14a-*.script`, `tests/ui/README.md`, `tests/fixtures/videos/mkvideos.sh` | §7; `shell-contract.md` §7; `tests/ui/README.md` |
| — | Contracts (`metadata.h`, `tagcache.h`, `vclass.h`, `library.h`, `libfile.h`, `artfmt.h` `cw`, `video.h`, `settings.h`, `menucfg.h`), `apps/SOURCES`, `english.lang` (appended IDs), stub, ledgers, `hardware.md` | orchestrator | | |

V2 → V3 → V4/V5 share only §4. V6 needs only §4.5 and the stub. **Fixtures (V7)**, from the committed CC BY `bcmfw/video/stage6-real.mp4` via `prokbox-convert --bitexact-test`: 2 movies (one with poster and 3 chapters, the first at 0; one whose source's first chapter is at 2 s), a tagged show with 2 seasons × 2 episodes, an untagged `TV Shows/Show B/Season 1/` show, a `Show.C.S01E02.m4v` file, a nameless `S01E03.m4v`, a video podcast episode beside an audio one, a file with **the audio trak first** (`-map 1:a -map 0:v`), an audio-only `.mp4` (must stay a song), an m4b with a chapter-image trak (must stay a book), a High-profile file (audio-only fallback). Posters are our own synthetic JPEGs.

## 6. Device-reality checklist
1. **Compiled features:** all under `HAVE_BCMFW_VIDEO` (+ `HAVE_IPOD_UI`); new gate dirs `sim-ui-video` (`--ipodui --bcmfw-video`) and `fw-ui-video` (`--ipodui --bcmfw --bcmfw-video`). The sim's video is `vbcm_model.c` + host `bcmfw/h264`; the device path is the player's, untested by 14a until stage 6 passes.
2. **Memory:** `vplayer_start()` before audio start (its `core_alloc` sizes the audio buffer after it); chapters reuse playback's table; screen state static ≤ 256 B; no allocation while audio plays. `vixpass` and the art work use the precompute/art blocks, stopped only (`audio_status()` re-checked immediately before each file, no yield between). The player's block is freed at `vplayer_stop()`; the audio buffer regrows at the next start (`audio_buffer_regrow()`). Libfile header copies +256 B each (§3.3, stack headroom reported).
3. **CPU:** classification: the path rule per video and ≤ 3 small reads per `vide` trak at scan time; an m4a/m4b without a `vide` trak pays nothing (V2 measures the 50k scan before/after). Precompute: O(N) flag tests plus a ledger lookup per video. `.vix`: ~1–2 s per 2-hour film on the PP (estimate), stopped, yielding.
4. **Storage:** browsing: 0 extra reads. Per precompute: 1 read of `vix.dat`, then 0 opens for already-indexed videos; 1 `open` + `moov` read per new or changed video. Playback: the player's 64 KB `.vix` windows ride the audio fills; the screen reads nothing.
5. **LCD:** video owns the panel while playing (pushes suspended); paused overlay = one full-width bottom band per change, ≤ 10/s.
6. **Threads/locks:** classification on the tagcache thread (scanner); lists/`vixpass` on the tagcache thread; art on the art thread; `tagcache_ui_resumes()` on the audio thread reads only `id3` fields; the screen on the UI thread calls only `vplayer_*`, `audio_*`. No RAM-cache pointer held across a yield. `tv_show` survives `copy_mp3entry()` (H2).
7. **Splashes:** none; every failure → log + Now Playing or silent skip.
8. **Persistence:** resume = existing tagcache autoresume at stop/MENU/track end (audio thread, as podcasts); one script per trigger (MENU, power-off while playing, USB connect while playing). `.vix` and `vix.dat` are atomic (`.tmp` + rename); a torn libfile is caught by sums; a torn ledger is rebuilt from one `readdir`.
9. **Scale:** 50k tracks + 500 videos (`tests/perf/lib50k.sh` plus fixture copies): precompute delta, libfile size, ledger size.
10. **Deferred work:** `vixpass` waits for stop: tested in *ready*, *fresh/rebuilding* (lists show "Updating Library..." as today) and *dirty*; a video opened before its index exists builds it at open.
11. **Stock unchanged:** stock `.text` identical; `fw-ui`/`sim-ui` identical except appended `english.lang` IDs; every existing golden unchanged. **Switching builds (T8):** a disk used by `sim-ui-video` then `sim-ui` shows `.m4v` files as songs in the non-video build until a rebuild (the non-video build doesn't know the bits); accepted, the owner's unit runs one build.
12. **Existing paths doing the same job:** every class-bit site is in §3.2a. Resume: tagtree's and the shell's both key on `tagcache_ui_resumes()`, one predicate. Chapter skip: Now Playing and the video screen share `ab_chapter_skip_target()`. Speed: `ab_speed.c` is the one decider. The old mpegplayer path stays trimmed. Libfile header writer: `library.c`; readers: `library.c`, `artfmt.c`, `artcache.c`.
13. **Builder predicates:** `vixpass`: VIDEO bit, not deleted, `(path_hash, tag_mtime)` not in the ledger. Art: album-table rows with `LIBFILE_ALB_VIDEO`, poster from `covr` or sidecar; coverless videos get the film-strip thumbnail. Panel share: `pre_panels()` picks ≤ `PANEL_VIDEOS` video rows with art, most recently played, then newest.

## 7. Acceptance criteria
- **A1** `fw-ui`, `sim-ui`, stock: no change beyond appended LANG IDs; all existing goldens pass.
- **A2** `sim-ui-video` scripts (proposed goldens, two identical runs): `ipodui-p14a-menu` (main panel empty and with posters; Videos rows), `-movies`, `-tv` (show › seasons › episodes, tagged, folder and nameless shows), `-podcasts-mixed` (glyph; video → video screen; audio → Now Playing; an audio episode plays on past a video episode without playing it), `-play` (`expect_video` ≥ 30 playing; pause → overlay `video_shot`; MENU → list; reopen resumes ± 1 s), `-chapters` (Next lands on chapter 2 ± 0.5 s; ticks on the overlay; **speed 1× with the audiobook speed set to Faster**), `-fallback` (High profile → Now Playing, audio plays, log line, no splash), `-settings`, `-about`, `-classify` (audio `.mp4` in Songs, m4b in Audiobooks; **no video in any music list, Shuffle Songs, Shuffle Albums or Search**).
- **A3** Host tests: `test_vclass` covers every §3.2 rule (≥ 30 path/tag cases, empty show). **`test_mp4meta`** (warble): goldens for `is_video`, `stik`/`tvsh`/`tvsn`/`tves`, `is_audiobook` false and `chapter_count` > 0 on the chaptered movie, `lead_trim` 0 on the 2 s-first-chapter file, and **length equal to the audio-first file's ffprobe audio duration ± 1 frame**; a `copy_mp3entry()` check that `tv_show` points into the destination. Converter test: profile, 320×240, 1 slice, no B-frames, **an `avc1` trak present**, `stik`/`tvsh`/`tvsn`/`tves`/`covr` present, **`covr` starts `FF D8 FF E0`**, `genre=Podcast` for `--podcast` (no `pcst` assertion), both chapter forms present with the first at 0 and parsed identically by the host chapter test, an idempotent second run converts 0 files.
- **A4** Stack: V3/V4 report headroom of the tagcache, art and UI threads with the 800 B header (≥ 512 B each).
- **A5** No "Rockbox", "BCM", "VideoCore", "vix" or codec names on screen; every string via `LANG_*`.

## 8. Budgets
`fw-ui-video` vs `fw-ui-video` before 14a: shell + classifier ≤ **24 KB** `.text`; new statics ≤ **512 B** total, none > 256 B; resident RAM: no new permanent block, +256 B per libfile header copy (stacks and `bwork`); disk: `vix.dat` 16 B per video, `panels.pak` unchanged in size (shared 32 slots); converter ≥ 4× realtime on the dev Mac at `-preset medium` (report). Agent budget: 250k tokens / 2 failed fixes; gates via `scripts/gate-run`.

## 9. Device test procedure (owner; only after stage 6 passes on silicon and with the owner's go and a fresh backup, `deploy-ipod`)
1. Build: `build/fw-ui-video` (`--target=22 --type=n --ipodui --bcmfw --bcmfw-video --compiler-prefix=arm-none-eabi-`), `make zip` (copies `bcmvideo.bin`).
2. Mac: `scripts/prokbox-convert.py ~/Movies/test -o ~/Desktop/ProkBox\ Videos` (2–3 short files: one movie with poster and chapters, two TV episodes); `--check` each.
3. Install the zip (backup first), reboot, connect USB, drag the `Movies` and `TV Shows` folders to the iPod's top level, eject. Wait for "Updating Library..." to clear.
4. Settings › Main Menu: Videos on, if the saved mask predates 14a (T1).
5. Videos › Movies › film: pictures within ~1 s; pause, overlay, scrub, Next chapter, wheel volume, MENU, reopen (resumes). TV Shows › show › season › episode. A video podcast in Podcasts. About counts.
6. Copy one unconverted High-profile file: plays as audio in Now Playing; the developer log has the reason.
7. Report: what played, pictures/drops from the developer log, battery over 10 min of video. Roll back by reinstalling the previous zip.

## 10. ⚠️ Owner questions (frozen defaults)
- **T1** Videos in the main menu by default (stock); a saved older mask keeps it off until enabled in Settings › Main Menu. *Default: on for new configs; install step 4.*
- **T2** A show with one season skips the Seasons level. *Default: skip.*
- **T3** An untagged TV file with no show name. *Default: cleaned filename prefix, else folder, else stored empty and shown as "Unknown Show" (LANG).*
- **T4** Video episodes in Podcasts marked by a small screen glyph. *Default: glyph on.*
- **T5** *Owner decision (2026-10-12):* About's Videos row counts movies + TV episodes only; video podcast episodes count under Podcasts only (never in both rows; `vstats.vpodcasts` stays a separate field for logs). The capacity bar's Video segment covers every video file, video podcasts included (storage), from the bitrate × length sum (≈ file size).
- **T6** Start Playing: Where Left Off / From Beginning; Ask hidden. *Default: as stated.*
- **T7** Fit to Screen shown and stored though inert until the player supports it. *Default: shown, On.*
- **T8** `.mp4` entries scanned before 14a keep their class until a rebuild (owner ROADMAP note: a later reclassification). *Default: no automatic rebuild in 14a.*
- **T9** Videos left out of Search in 14a. *Default: excluded (photo 10 settles whether the 7G lists them).*
- **A1/A2** Poster thumbnails fitted on blurred bars; videos take ≤ 8 of the 32 panel slots. *Default: yes.*

## 11. Ledgers at close
`docs/design/changes-from-7g.md`: (a) video podcasts live in Podcasts, no Videos › Video Podcasts (owner D2); (b) Music Videos dropped, `stik` 6 → Movies (owner D1); (c) Rentals and Video Playlists hidden; (d) Show › Seasons › Episodes (owner D3; stock level ⚠️ photo 5); (e) interim: overlay and volume bar only while paused, SELECT while playing shows nothing; (f) Fit to Screen inert; (g) Ask hidden; (h) videos not in Search (if photo 10 shows the 7G lists them); (i) subtitles only burned in by the converter. `docs/changes-from-rockbox.md`: `metadata.c`, `metadata.h`, `mp4.c`, `tagcache.c`, `tagcache.h`, `settings_list.c` edits, all `HAVE_BCMFW_VIDEO`.

## 12. Risks for the brief review
- The `vide` helper's seeks inside a non-faststart file (moov at the end): it must stay inside the trak's bounds and restore the walk's position.
- The libfile version 2 only in video builds: a unit switched between builds rewrites the file once each way (accepted).
- Chapter precedence when a file has both `chpl` and a text trak: the test asserts they agree for converter output; iTunes files with only one form are covered by `chapters.c` today.

## Rev 1 (2026-10-12, brief review)
| Finding | Resolution | Sections |
|---|---|---|
| ffmpeg 9.0.2 facts (chapters both forms; `covr` only from a real JPEG input with `-c copy`; lavfi still drops `avc1`; tag mapping; no `pcst`) | Converter recipe and tests rewritten to match | §3.1, A3 |
| H1 `pcst` can't be produced | `--podcast` writes `genre=Podcast` + `Podcasts/` folder; reader kept; A3 no longer asserts `pcst` | §3.1, §3.2, §4.1, A3 |
| H2 `tv_show` relocation | `MOVE_ENTRY(entry->tv_show)` under the flag (V2), `copy_mp3entry` test | §0, §4.1, §6.6, A3 |
| H3 other class-bit sites; `is_audiobook` on chaptered movies | §3.2a lists every site with an owner (`lib_shuffle.c` V3; `holdmenu.c`, `np_playback.c`, `ab_speed.c` V6); `is_audiobook` cleared for videos, `chapter_count` kept; Search/Shuffle/1× tests | §0, §3.2, §3.2a, §5, A2, A3 |
| H4 generic walk corrupts length | Dedicated `mp4_vide_entry()` reading only the first `stsd` fourcc; audio-first fixture with a length golden | §3.2, §5, A3 |
| M5 `.vix` pass opens every video | `vix.dat` ledger keyed by `(path_hash, tag_mtime)` with size; 0 opens for indexed videos; `tag_mtime` = `dir_get_info().mtime` stated | §3.4, §6.4, §6.13 |
| M6 `lead_trim` from a late first chapter | Converter forces the first chapter to 0; `mp4.c` zeroes `lead_trim` for videos; fixture and test | §3.1, §3.2, A3 |
| M7 untagged show/title storage | `ui_title[MAX_PATH]` reused (no static); empty show stored, LANG string shown | §3.2, §4.3, T3 |
| M8 header growth, section count | 38 sections, `LIB_PRE_SECTIONS` 38; +256 B per copy listed; stack headroom reported (A4) | §0, §3.3, §6.2, A4, §8 |
| M9 poster slots | Stay 32; `PANEL_VIDEOS` 8 share in `pre_panels()` (V3); portrait via the content width in the unused `pad[1]` (halved), no format bump | §0, §3.5, §4.4, §5, §8 |
| M10 contract holes | `lib_filter.season` + `LIB_FILTER_ANY`; `tagcache_ui_resumes()` tests `is_video`; `ui_setvideos_push()`; `lib_play()` skips video rows | §3.2a, §3.6, §4.4, §4.5 |
| Low: metadata lines | AAC :213, AAC-HE :226; `.m4v` on AAC only | §0 |
| Low: `panels.c` path | `screens/panels.c` | §5 |
| Low: T5 free | `video_mib` from the existing formula | §3.3, T5 |
| Low: T8 build switching | Stated in §6.11 and T8 | §6.11, T8 |
| Owner ROADMAP notes | Reclassification-ready, `--burn-subs`, letterbox kept with a profile stamp for later reconversion | §2, §3.1 |

**Rejected:** none. **Chosen between the reviewer's options:** M5 uses a ledger in `VIX_DIR` rather than storing the size at scan time, because tagcache has no size tag and adding one changes the database format; the ledger keeps it to one read per precompute.
