# Phase 6 brief: Audiobooks (#5)

Execution order 8. Developed on branch **`p6` off `np`** (`np` merges to `main` first; the 2026-10-08 spec notes cited here are on `main`, so the orchestrator rebases `p6` onto `main` before fan-out). Lifecycle per `docs/process.md`: brief → brief review → contracts commit → fan-out with self-review → one full gate → end review → one owner round → close. Written 2026-10-08 overnight with the owner asleep: every open fact is frozen as ⚠️ **with a default** (§11) and is not revised mid-phase.

## 1. Goal
Books become first-class, as on the 7G 2.0.5: a book list with per-book progress, the opened book (**Resume** + chapters), per-book resume that survives power-off and a database rebuild, speed (Slower / Normal / Faster) via time-stretch, and **whole-book** position, bar and counter across multi-file books. Closes #5, #48 (option 2) and #50; resolves Phase 5 frozen defaults 9.12 (multi-file books) and the chapter-table ownership issue.

**Out:** MP3→m4b merging (#36, Mac side; context only: it shrinks the multi-file case, it doesn't remove it); a podcast episode list redesign (NAV-4 badge stays); a Speed step in the Now Playing cycle (NP-3); bookmarks UI; chapter art for multi-file books (P5 ⚠️ stays: cover only); Cover Flow; Hold-SELECT (P11).

## 2. Owner pre-flight
**Answered (apply as written):**
- Opened book = full-width single-line list, **Resume** first, then the chapters; no chevron on Resume, no hints on chapter rows (`audiobook-open-chapters-b.jpg`; #5 comment 2026-10-06). Chapter = play from it.
- Book list = two-line rows with cover thumbnails (title bold / author gray), ~4 per screen, like Albums (catalogue §Audiobooks, owner video 2 `labeled/v2-t57.0.jpg`). PLAY/PAUSE on a book row resumes it directly.
- **Counter (owner 2026-10-08):** "N of M" = the **position of the current chapter entry** in the book's chapter list, never parsed from the title; title and counter come from the **same entry**, updated together. Podcasts keep the **queue** counter even with chapters (2026-10-07). Chaptered MP3 podcasts stay podcasts.
- **A book plays alone** (only its files are queued, unless started from a user playlist) and ends by stopping; Now Playing then pops (spec "When the queue ends"). The bar spans the **whole book** with chapter ticks; the album line is the chapter title.
- **Auto-return to Now Playing on idle applies to books** (owner 2026-10-08); already built (`ui_nowplaying_idle()`), P6 only adds a book script.
- Settings › Audiobooks: light full-screen prompt "How fast should audiobooks play?", body text, **Slower | Normal | Faster** segmented control; panel caption "Normal Playback" (`audiobooks-speed.jpg`, `settings-09-audiobooks.jpg`).
- Fast scrub snaps between chapter starts (2026-10-06). Chapter art in Now Playing only (#39).

**Open, frozen:** AB-1 → ⚠️1, AB-2 → ⚠️2, NP-3 → ⚠️3. **Captures still wanted** (owner "Still needed" list, INDEX line 107): the Audiobooks book list before opening a book (decides ⚠️4/⚠️5); an audiobook SELECT-cycle video (NP-3).

## 3. Design
### 3.1 The packed chapter table (#48 option 2)
- Under `HAVE_METADATA_EXTRAS` (shell + warble) `struct audiobook_chapters` **becomes** a packed, variable-length table (same type name, so `mp3entry.audiobook` and every caller keep compiling): a header, `cap` 8-byte entries `{uint32 start_ms; uint16 title_off; uint8 title_len; uint8 flags}` and a UTF-8 **title pool** (no NULs stored; `ab_ch_title()` copies and terminates).
- Capacity: **1,024 entries + 24,576 B pool = 32,784 B** per table (was 69,640 B fixed). Titles are cut to 63 B as today; when the pool runs out, later titles are shortened to what is left (min 0 B), counted in `hdr.cut` and traced; an empty title draws `LANG_IPODUI_CHAPTER_N` "Chapter %d" with the **entry position** ⚠️6.
- Parsers (`mp4.c`, `id3tags.c` CHAP) write through `ab_ch_put()`, sort with the existing stable insertion sort over **8 B entries** (8.5× less memmove than 68 B; the reversed-1,024 worst case drops from ~35 MB to ~4.2 MB, accepted ⚠️7).
- **Stock is byte-identical:** without `HAVE_METADATA_EXTRAS` the accessors (`ab_ch_start()`, `ab_ch_title_ptr()`, `ab_ch_put()`) are macros over today's array, so `apps/audiobook.c`, `audiobook_core.c` and the parsers compile to the same stock code. Verified by the checklist item 11 cmp.
- `metadata_read_chapter_art()` takes `const struct audiobook_chapters *` instead of `(const struct ab_chapter *, count)` (only `start_ms` is read). The art builder's transient block shrinks by 36,856 B.

### 3.2 Chapter-table ownership (P5 known issue)
- Playback's single table can be rewritten for a track that isn't the one the user hears (buffer-ahead / finalise ordering), so Now Playing could show the next file's chapters.
- Fix: **two slots** in the audio scratch area (`2 × 32,784 = 65,568 B`, **4,072 B less than today's one 69,640 B table**). Each slot carries an **owner** = the track identity (`ab_owner`: the id3 buffering handle plus a playback load serial; never reused within a run), separate from the slot's **seq** (validity, the P5 seqlock, now per slot). The writer fills the slot whose owner is not the user-current track; `audio_current_track()->audiobook` points at the slot whose owner matches.
- Readers (N, the stock `audiobook.c`) check **identity first** (`owner == np_track.track_id`'s owner) and **validity second** (seq even and unchanged across the copy). Identity mismatch = "no chapters yet" (ticks hidden, counter falls back to the book map, §3.3), never the other slot's data.

### 3.3 Books: identity, the book map and `books.pak`
- **Book identity** = the library's book key (`lib_art_key()` of the book row: `artcache_key()` over album + album artist; folder key when untagged). String-derived, so it survives a rebuild. **Validity** of anything cached for a book = `{max_mtime, file_count}` of its files.
- **Book entries:** a single-file book's entries are its chapters. A multi-file book's entries are, file by file in disc/track order, that file's chapters if it has any, else **the file itself** (title = track title). So the counter is "entry N of M" across the whole book, and a 163-file book without embedded chapters counts files.
- **`books.pak`** (in `ARTCACHE_DIR`, built at sync time): per book, the packed entry table with **book-relative** start times, plus a file map `{uint32 start_ms; uint16 first_entry}` per file (≤ 1,024 files ⚠️8). Blob ≤ 38,928 B. Index: sorted `{book_key, max_mtime, file_count, offset, size}`, 24 B per book, rewritten by temp + rename with a generation word (readers re-check it, the `thumbs.pak` lesson).
- **Built** by the art builder's existing chapter pass (P3), extended from single-file books to every book, **only while stopped** (`audio_status()` re-checked with no yield before each `core_alloc`, `audio_buffer_regrow()` after each free). It already opens each single-file book for its chapter art; a multi-file book costs one open per file. Stale or missing blob → **fallback**: entries = files (titles and lengths from the RAM database, 0 file reads), traced `ab pak miss`.
- **Book block** (shell, permanent, allocated at shell start before any playback): the playing book's file map and summary, **6,160 B** (`1,024 × 6 + 16`). Filled at `lib_play()` of a book (from the blob already read for the opened book, else 1 index lookup + 1 read, else the fallback). Gives Now Playing the whole-book position `file_start[q] + elapsed`, the book-wide counter `first_entry[q] + local_entry + 1` of `entries`, and the whole-book tick bitmap (precomputed into the existing 28 B `NP_BLK_TICKS`).

### 3.4 Now Playing for books
- A queue flagged as a book (`lib_queue_is_book()`, P5): bar, elapsed and remaining span the **whole book**; ticks from the book map; the album line and the counter come from the **same entry** (single-file: the playback slot's current chapter; multi-file: the book entry for (file, local chapter)).
- Scrub: slow = whole-book position; fast = snap to entry starts (within the file from the slot, across files from the file map; crossing a file is a queue skip + resume offset, one seek per gesture as in P5). New `np_pb_seek_book(book_ms, track_id)`.
- Not a book queue (song, podcast, a book file from a user playlist): P5 behaviour unchanged (per-file bar; podcast = queue counter).
- Speed shows nowhere on Now Playing ⚠️3; the clock is **media time** (the bar runs faster at 1.25×) ⚠️9.

### 3.5 Speed
- Setting `ipodui_ab_speed` (0 Slower / 1 Normal / 2 Faster) in `settings_list.c` under `#ifdef HAVE_IPOD_UI` (hidden from stock menus), default Normal. Factors **75 / 100 / 125 %** ⚠️2 (`PITCH_SPEED_100` units).
- Applies to **audiobooks only** (not podcasts ⚠️10, not music). The factor is set from a guarded hook where the codec starts a track (audio thread), so it changes on the track boundary, not at the user-visible track change ~1 PCM buffer later; changing the setting during a book applies at once.
- `timestretch_enabled` defaults on under `HAVE_IPOD_UI`, so tdspeed's buffers (**57,344 B**, 2 × 12,288 + 2 × 16,384) are allocated **once at boot, before shell start** (the "no `core_alloc` after shell start" trace rule must still pass). At 100 % the stage is inactive (tdspeed passes through; verify `dsp_proc_activate` false), so music and Normal books pay no CPU.

### 3.6 The per-book resume store (`books.dat`)
- **Where:** `ARTCACHE_DIR "/books.dat"`, outside the database, so it survives a rebuild and a changelog loss. Header (magic "IPAB", version 1, count) + records sorted by book key, **32 B each**: `{book_key 8; file_crc 4 (path CRC of the resume file); file_idx 2; flags 2 (STARTED, FINISHED); elapsed_ms 4; offset 4; book_ms 4; book_len_ms 4}` with an 8-bit check in `flags`' high byte. 16 records per sector, never straddling one. Cap 2,048 books (64 KB) ⚠️11.
- **Writes:** an existing record is updated **in place** (one sector, atomic at sector level; a torn write costs only that record, which then reads as its previous value or as unplayed, logged). A new book is inserted via `books.tmp` + rename. Triggers: pause, stop, track change within a book, USB connect and shutdown → **write now** (pause/stop wake the flash once, the user is acting ⚠️12); while playing, a checkpoint **rides the next buffer fill** (`register_storage_idle_func`), never its own wake. The audio-thread event only copies the position into a 32 B pending record; the write runs on the storage-idle/UI side.
- **Finished:** `book_ms ≥ book_len_ms − 30 s` at stop, or the book's queue ending → FINISHED; Resume then starts from the beginning ⚠️13.
- **Resume:** `lib_play()` of a book looks up its record (in RAM since the list open), finds the file by `file_crc` (rebuild-safe; `file_idx` fallback), seeks `elapsed_ms`. No record → start of the book.
- **Migration:** on the first P6 boot with the database ready, books without a record are seeded once from today's tagcache stats (`tag_lastelapsed`/`tag_lastoffset`, the most recently played file); retried at the next boot if the database isn't ready. tagcache stats keep being written as before (podcasts still use them).

### 3.7 The Audiobooks screens
- **Book list** (`LIB_AUDIOBOOKS`): two-line rows with thumbnails (Phase 4b). The progress hint becomes **dots** in the podcast badge position (NAV-4): **unplayed** = full blue dot (no record), **partially played** = half-filled blue dot (STARTED, not FINISHED), finished = none ⚠️4. Replaces the Phase 4 bar hint. Progress = `books.dat` join on `lib_art_key()` per row (no database pass; `lib_progress()` stays for podcasts).
- **SELECT on a book** pushes the **Book list**: header = book title ⚠️5; rows **Resume**, then the entries (§3.3) in order; Resume selected. SELECT on Resume = resume; on an entry = play the book from that entry's start. A book with **one file and no chapters** skips the list and resumes ⚠️1. Returning from Now Playing keeps the selection.
- The Book list's table is a **chapter view** in the library view pool (`blob ≤ 38,928 B`, inside the pool's 64 KB headroom, static-asserted; never a new `core_alloc` while playing). If the opened book is the playing one, it uses the same blob already read.
- **Settings › Audiobooks** (`screens/settings.c` row): the full-screen prompt per spec; wheel or PREV/NEXT moves the segment, SELECT/MENU saves and returns; caption follows the value: "Slower Playback" / "Normal Playback" / "Faster Playback" ⚠️14.
- All strings are `LANG_IPODUI_*`, **appended** to `english.lang` (Resume, the heading and body, Slower/Normal/Faster, the three captions, "Chapter %d").

## 4. Contracts as real code (committed by the orchestrator before fan-out)
| File | Content | Round-trip host test |
|---|---|---|
| `lib/rbcodec/metadata/chapters.h` | packed `struct audiobook_chapters` (`ab_ch_hdr`, `ab_ch_entry`, `AB_CH_POOL`), `ab_ch_size(cap, pool)`, `ab_ch_init/put/sort/start/title/find`, the stock macros, the new chapter-art signature, `struct ab_owner` | `tests/audiobook/test_abpack.c`: put/sort/title, pool exhaustion and `cut`, 1,024 entries, stock-macro equivalence |
| `apps/ipodui/ab_store.h` | `struct ab_rec` (32 B, static-asserted), `ab_rec_check()`, `ab_store_find/get/put/pending()`, `enum ab_rec_state` | `test_abstore.c`: pack/check, torn record, bsearch, insert order |
| `apps/ipodui/ab_book.h` | `books.pak` header/index/blob structs, `struct ab_book_map`, pure `ab_book_pos()`, `ab_book_counter()`, `ab_book_ticks()`, `ab_book_entry_at()`, `ab_speed_factor()` | `test_abbook.c`: position across files, counter = entry position (Preface/Intro before "Chapter 1"), ticks, files-as-entries fallback |
| `apps/ipodui/np_playback.h` (delta) | `struct np_track` gains `struct np_book_pos book` (`active`, `book_ms`, `len_ms`, `entry`, `entries`); `np_pb_seek_book()` | existing `tests/nowplaying` layout check (`np_track` ≤ 512 B) |

**One shared stub**, `apps/ipodui/ab_stub.c` (no record, no pak, Normal speed), deleted by B in its first real commit. No `TEMP-` commits.

**Contract lessons applied:**
- **Identity vs validity** (P5 Rev 3): every cache names both. Chapter slot: identity `ab_owner`, validity per-slot `seq`. Book: identity book key, validity `{max_mtime, file_count}`. Resume record: identity book key + `file_crc`, validity the record check. Now Playing: `track_id` changes only on a real track change; a seq move on the same track only re-points the chapter.
- **The trace macro is `UPDATE_TRACE`** (`apps/ipodui/anim.h`; logf + DEBUGF). Required lines: `ab slot w=%d owner=%lu n=%d cut=%d`, `ab slot read owner=%lu ok=%d`, `ab pak hit|miss book=%08lx`, `ab open book=%08lx reads=%d`, `ab store w=%s book=%08lx ms=%lu` (`now`/`idle`), `ab speed %d`, `ab resume book=%08lx file=%d ms=%lu`. `check_trace.py` rules (T): no `ab store w=now` while playing except at a track change; `reads ≤ 2` per open with the pak; zero `core_alloc` after shell start; the gate fails on a skipped expected rule.

## 5. Ownership (one owner per file)
| Agent | Owns | Must not edit |
|---|---|---|
| **M** (playback-audiobook, top model): metadata + playback | `chapters.c` and the contract's implementation, `mp4.c`, `id3tags.c` (#50, CHAP via `ab_ch_put`), `apps/audiobook.c` + `audiobook_core.c` (accessors only), `apps/playback.c` guarded hooks (two slots, owner, the speed hook call site), `tests/audiobook/*` and their fixtures | frozen headers, `apps/ipodui/*` |
| **B** (playback-audiobook, top model): books | new `ab_store.c`, `ab_book.c`, `ab_speed.c`; `artcache.c` (chapter pass → `books.pak`, new art signature), `library.{c,h}` (book progress, resume, chapter view, book map), `np_playback.c`, `settings_list.c` + `settings.c` guarded lines (speed setting, `timestretch_enabled` default), `tests/abbook/*` | frozen headers, screens, `np_draw`, M's files |
| **U** (ui-fidelity, top model): screens | `screens/audiobooks.c` (dots, Book list), `screens/browse.c` (book rows only), `screens/settings.c` (Audiobooks prompt + caption), `screens/nowplaying.c` + `np_draw.c` (whole-book bar, counter source), `art/` dot glyphs, `apps/lang/english.lang` (appended) | frozen headers, B's and M's files |
| **T** (tooling, Sonnet) | `tests/ui/ipodui-p6-*.script`, `tests/fixtures/ab/*`, `uisimulator/common/sim_cmd_ab.c` (`ab_pin_store`, `ab_power_cut`), `scripts/check_trace.py` P6 rules, `scripts/gate_map.txt`, compose sheets, `ab-review.mp4` | `apps/`, `lib/` |

`apps/SOURCES`: additions only, inside `#ifdef HAVE_IPOD_UI`, requested from the orchestrator. `docs/hardware.md` budget table: orchestrator at close.

**Read lists (≤ 3 each; contract headers are read as code, not counted)**
- **M:** this brief §3.1–3.2, §4; `docs/design/chapter-art.md` (API + cost); issue #50 text (`scripts/forgejo.py show 50`).
- **B:** this brief §3.3–3.6, §4; `docs/design/chapter-art.md` (P2/P3 sections); `docs/design/shuffle.md` (book flag, `npshuf.dat`).
- **U:** this brief §3.4, §3.7; `docs/design/spec/audiobooks.md` + `settings.md` (Audiobooks item); `docs/design/spec/now-playing.md` (Audiobooks + 2026-10-08 counter note).
- **T:** this brief §7–§9; `tests/ui/README.md`; `docs/phase-nav-brief.md` Revision 2 (`record`, `make_video.sh`).

## 6. #50: ffmpeg's non-synchsafe CTOC
In an ID3v2.4 tag, when a frame's synchsafe size overruns the tag or lands on bytes that are not a frame ID (`[A-Z0-9]{4}`), padding or the tag end, retry the size as plain big-endian and accept it only if **that** lands on a valid frame ID, padding or the tag end; else keep today's behaviour. `HAVE_METADATA_EXTRAS` only. Fixture: an ffmpeg-written MP3 with CHAP + CTOC (size 0x6a0) and ≥ 3 chapters; golden from `ffprobe -show_chapters`; a v2.4 tag with correct synchsafe sizes is unchanged (existing goldens).

## 7. Device-reality checklist
1. **Compiled features:** the packed table and #50 under `HAVE_METADATA_EXTRAS`; everything else `HAVE_IPOD_UI`. `HAVE_DIRCACHE` off on the device: `books.pak`/`books.dat` are opened by path, 1 open each per list open. Sim and device run the same code; the power-cut test is sim-only (`ab_power_cut`) → P10 item.
2. **Memory:** playback chapter area **69,640 → 65,568 B** (−4,072, audio scratch). Book block **+6,160 B** permanent, allocated at shell start. tdspeed **+57,344 B** permanent, at boot before shell start (stock allocates it only when enabled). Art builder chapter transient **77,832 → 40,976 B** (stopped only). Chapter view ≤ 38,928 B inside the view pool's headroom (no growth). Statics < 128 B (pending record 32 B, flags). **0 B allocated while playing.** The audio buffer loses 57,344 + 6,160 B once at boot and regains 4,072 B: net **−59,432 B** (≈ 0.2 % of 32 MB). B and U state each reservation's sharing per lean rule 9.
3. **CPU:** Audiobooks list open: 300 books × (O(1) key + 9-step bsearch) ≈ 45k instructions ≈ **0.6 ms at 80 MHz**; 2,000 books ≈ 0.3M ≈ 4 ms; replaces Phase 4's two master-index passes (at 40k entries ~80k record visits). Book map: O(1) per tick. `ab_book_ticks()` ≤ 1,024 entries once per book start. Packed sort worst case 4.2 MB memmove (~50 ms, rare). **Time-stretch at 75/125 %: unknown on PP5021** → measure codec-thread load in the sim's ELF harness and add a P10 item (AAC 64 k and MP3 128 k, 30 min unboosted, no underrun).
4. **Storage per action:** list open: books.dat 1 read (≤ 64 KB) + pak index 1 read. Book open: **1 blob read** (pak hit), else 0 reads (files-as-entries fallback from the RAM database). Resume: 0 extra (record in RAM, map from the blob). Pause/stop: 1 write (wakes once). Playing: checkpoints ride buffer fills (0 wakes of their own). `books.pak` build: stopped only, ≤ 1 open per file of a new or changed book. Speed change: settings save rides idle.
5. **LCD:** the dot glyphs are pre-rendered tiles; the Book list is the standard list (25 px rows); the speed prompt redraws only the segmented shelf band on a change. No new animation beyond the list's.
6. **Threads and locks:** audio thread writes slots (seqlock + owner) and the pending record (word copy under `disable_irq`); art builder thread writes `books.pak` (temp + rename, generation); UI thread reads both; the storage-idle callback writes `books.dat`. No mutex taken from the UI across a yield; the playlist mutex is never held across file I/O.
7. **Splashes:** none. Missing or corrupt `books.dat`/`books.pak` → logged, books read as unplayed / fallback entries.
8. **Persistence:** power-off mid-update loses at most one record (sector-atomic in-place write; inserts via rename). Power-off while playing loses at most one buffer-fill interval of position (⚠️12; a clean low-battery shutdown loses nothing). USB connect flushes the pending record before the host mounts. A database rebuild keeps every resume point (book key + file CRC). Re-tagging a book's album changes its key → shows unplayed (⚠️11; old record dropped only after a successful full rebuild).
9. **Scale:** 50k tracks / 5k albums / 300 books via `tests/perf/lib50k.sh` + a books add-on (300 books, 30 multi-file of 20–400 files, 3 with 1,024 chapters): report list open and book open times in the sim and the device estimate.
10. **Deferred or retried work, per database state** (*ready*, *fresh or rebuilding*, *dirty*), each with a script: the `books.pak` pass (waits for stop), the tagcache migration seed (waits for ready), a resume record written while the library updates (book key from strings, so valid in every state).
11. **Stock unchanged:** stock `.text` cmp-identical to `build/fw/.size-baseline` with `english.lang` held equal; the chapters accessors are macros on the stock path.

## 8. Acceptance criteria
- Host tests: test_abpack, test_abstore, test_abbook, the #50 golden, all existing chapter goldens unchanged (`scripts/test.sh`). `scripts/gate-run` passes (the full gate once, at the end). No new warnings, statics < 1 KB, `cpu_boost` balanced.
- Scripts (T), each with trace rules: `ipodui-p6-booklist` (dots: unplayed / partial / finished), `-open` (Resume + chapters, entry 2 is "Chapter 1" in the Preface fixture), `-nochap` (AB-1 path), `-multifile` (whole-book bar and counter across a file boundary, files-as-entries and per-file-chapters variants), `-resume-{ready,rebuild,dirty}`, `-powercut`, `-speed` (prompt, caption, `ab speed` trace, podcast stays 100), `-slot-owner` (a two-book queue from a playlist: never the next file's chapters), `-autoreturn-book`, `-pak-miss`.
- Now Playing: title and counter from the same entry on every chapter change; podcasts unchanged (P5 goldens hold).

## 9. Review artifacts
- Screenshots composed against the owner photos: `audiobook-open-chapters-b.jpg` (Book list), `np-audiobook-noart-chapterticks-b.jpg` and `np-audiobook-volume-b.jpg` (whole-book bar, ticks, counter), `audiobooks-speed.jpg` (prompt), `settings-09-audiobooks.jpg` (caption); the book list against `labeled/v2-t57.0.jpg` until a photo exists.
- `ab-review.mp4` (~60 s): Music › Audiobooks → open a book → Resume → chapter skip across a file boundary → fast scrub snapping → Settings › Audiobooks Faster → back to Now Playing → stop → the dot turns half.

## 10. Phase-close items for the orchestrator
Budget-table lines: book block 6,160 B; tdspeed 57,344 B; chapter area 65,568 B; chapter transient 40,976 B; `books.dat` ≤ 64 KB and `books.pak` (~3–40 KB per book) on disk. P10 device checklist: time-stretch CPU, power-cut resume, flash wakes per hour of book playback.

## 11. ⚠️ Frozen defaults (owner round confirms; not revised mid-phase)
1. **AB-1:** a book with one file and no chapters resumes directly on SELECT (no one-row list). Multi-file books always open the list (files are entries).
2. **AB-2:** Slower 0.75×, Normal 1.0×, Faster 1.25×.
3. **NP-3:** no Speed step in the cycle; no speed indicator on Now Playing.
4. **Book list progress:** full blue dot = unplayed, half dot = partially played, none = finished; in the podcast badge position.
5. **Book list header** = the book title; Resume is always shown (an unplayed book's Resume starts at 0).
6. **Empty chapter title** (or pool exhausted): "Chapter %d" with the entry position.
7. **Packed sort** stays the stable insertion sort (8 B entries).
8. **Book map cap** 1,024 files; a bigger book falls back to the per-file bar and queue counter (traced).
9. **Clock under speed:** media time; the bar runs faster at 1.25×.
10. **Speed scope:** audiobooks only; podcasts and music always at 1.0×.
11. **books.dat** cap 2,048 books; a re-tagged book starts unplayed.
12. **Checkpoints:** write now on pause, stop, track change, USB and shutdown; while playing, ride buffer fills (a hard power cut while playing can lose up to one fill interval).
13. **Finished** = within 30 s of the end, or the book's queue ended; Resume on a finished book starts over.
14. **Settings caption** follows the value: "Slower Playback" / "Normal Playback" / "Faster Playback".

## Revisions
*(Deltas go to running agents by SendMessage, per `docs/process.md`.)*

## Revision 1 (brief review: REVISE, 2026-10-08). This overrides anything above.
**High**
1. **One resume authority.** `playback.c` ~1836–1845 auto-resumes any `is_audiobook` file from tagcache stats when elapsed and offset are 0, which would override `books.dat` (chapter 1 at 0 ms, Resume on an unplayed or finished book, files-as-entries, fast scrub across files). Under `HAVE_IPOD_UI` **the shell is the only resume authority for book queues**: a guarded hook skips that auto-resume when `lib_queue_is_book()` (B picks the hook form: `autoresume` forced off for the book queue, or explicit resume info with `skip_resume_adjustments`; stated in the self-review). Books outside a book queue keep upstream behaviour. T adds `ipodui-p6-start-at-zero` (chapter 1 of a book with old tagcache stats starts at 0 ms; Resume on FINISHED starts at 0).
2. **0 B allocated while playing, for real.**
   - Book-row state (unplayed / partial / finished) goes into the existing **2 B/row progress column** of the book view, filled at `lib_open()`; **no RAM copy of `books.dat`** (§3.6 "in RAM since the list open" is withdrawn).
   - `books.dat` keeps a **sector-first-key index** (≤ 1 KB: 128 sectors × 8 B at the 2,048 cap), loaded into a fixed part of the book block (book block **6,160 → 7,184 B**), so a resume lookup is **one sector read**.
   - `pool_reserve()` (`library.c` ~739–760) **refuses to grow while `audio_status()` is non-zero**; the chapter view then builds with a smaller entry cap that fits the free pool (traced `ab view cap=%d`). The "inside the 64 KB headroom, static-asserted" claim is removed.
3. **Two-slot rules (replace §3.2).** Buffer-ahead never writes the table: `buf_read_audiobook` runs only for the user-current track (`:2678`, `:2825`). The real windows are the yields between the table write and `id3_write(PLAYING_ID3)`: `audio_start_codec` (~2682–2690) and `id3_mutex_lock` (~2840).
   - **Writer:** always fills the slot **not referenced by `id3_get(PLAYING_ID3)->audiobook`**, then points the new id3 at it.
   - **Reader identity:** the slot that `audio_current_track()->audiobook` references, with `pb_track` (the TRACK_CHANGE count = `np_track.track_id`) unchanged across the copy. **Validity:** that slot's seq (even, unchanged). `ab_owner` is **dropped** from the contract.
   - `scratch_mem_init` clears both slots (count 0, seq even).
   - The per-slot seqlock hooks (`np_pb_ab_write_begin(int slot)`, `np_pb_ab_write_end(int slot)`, `uint32_t np_pb_ab_seq(int slot)`) go into the `np_playback.h` contract delta, with matching weak no-op stubs in `sim_cmd_np.c` (T) so the stock sim links.
4. **`books.pak` coverage.** Today `run_chapters` (`artcache.c` ~2330–2335) skips coverless books, books P5 already passed, and multi-file books.
   - Every `LIBFILE_ALB_BOOK` row is **owed** whenever the pak is missing, has the wrong version, or its index `{max_mtime, file_count}` for that book is stale, **independent of art records and of the chapter-art done bit**.
   - Format: **append-only blob file** `books.pak` + a separate index file `books.idx` (temp + rename, generation word). Compaction (copy live blobs into `books.pak.tmp`, rename, rewrite the index) runs in the same stopped pass when dead bytes exceed 50 % ⚠️15.
   - Costs (B states measured values): listing a book's files = the library's TRACKS section (#49; 2 small reads); blob assembly while stopped = one per-file parse table 32,784 + the blob 38,928 + the art table 8,192 = **79,904 B** transient (stopped only; replaces 77,832 B); compaction = sequential copy, ≤ 1 blob in RAM.
   - **Single-file pak miss:** the Book list reads the chapters straight from the file into the chapter view (≤ 2 reads for the header walk + title samples coalesced by chunk; measured), so AB-1 never misfires on a chaptered book whose blob isn't built yet. Multi-file miss keeps files-as-entries.

**Medium**
5. **Timestretch on:** forced on at ipodui init, **before shell start** (an existing `config.cfg` can hold "timestretch enabled: off"); a settings default alone is not enough.
6. **Speed:** the factor is applied codec-side (`dsp_configure(TIMESTRETCH_SET_FACTOR)` from the codec track-start hook), or, if M finds that unsafe, from the UI thread as the upstream pitch screen does, stated in the self-review. Books are classified from **RAM only**: `id3->is_audiobook` or `TAGCACHE_UI_AUDIOBOOK`, and not a podcast. `dsp_set_timestretch()`'s write of `global_status.resume_speed` must not trigger a status save (guarded; trace `ab speed` shows no save).
7. **`books.dat` writes (replace §3.6 "Writes"):**
   - Insert = write `books.tmp`, remove `books.dat`, rename; open recovers from `books.tmp` when `books.dat` is missing.
   - A sector write is **not** assumed atomic (16 records at risk): the header is stored twice (sector 0 and the last sector, each with a sequence and checksum); each record carries its own check; an update writes one record and reads it back. A torn sector loses at most its 16 records' latest checkpoints, which then read as previous or unplayed (logged).
   - `register_storage_idle_func` callbacks are one-shot on the storage thread (small stack): **only in-place 32 B record writes there**; inserts run on the UI thread (`np_pb_idle`).
   - Track change within a book = **pending**, not write-now. Write-now stays for pause, stop, USB and shutdown.
   - While playing: checkpoints ride wakes, **capped at one forced wake per 30 min** if no fill came (a 60 MB fill at 64 kbps lasts ~2 h) ⚠️12 (revised).
8. **Book map:** each file entry also stores a path CRC (`{start_ms 4, first_entry 2, path_crc 4}` = 10 B; map 10,240 B at 1,024 files → book block **11,280 B** incl. the 1 KB store index). Now Playing compares it with the playing track; a mismatch falls back to the per-file bar and queue counter (traced). `lib_queue_resumed()` (boot resume) must refill the book block (`ab map src=resumed`).
9. **Owner fidelity:** `spec/now-playing.md` defines the counter from the **file's** chapter list. The book-wide counter for multi-file books is a **new frozen default ⚠️16**, not owner wording; single-file books follow the owner note exactly.
10. **Stock identity for the parsers:** titles are decoded in place (`mp4.c` ~1090, ~1336; `id3extras.c` ~374), so the contract adds `ab_ch_title_buf(ab, i)` / `ab_ch_commit(ab, i, start, len)`, which expand to the old array code in stock. Callers that change: `np_playback.c` ~205–209, `artcache.c` `chapter_book`, `audiobook.c`, `audiobook_core.c`. **M's ownership row is corrected:** CHAP lives in `lib/rbcodec/metadata/id3extras.c`; #50 touches `id3tags.c` (the frame walk and the count walk ~915). M owns `chapters.c`, `mp4.c`, `id3extras.c`, `id3tags.c`, `audiobook.c`, `audiobook_core.c`, the `playback.c` hooks; B owns `np_playback.c` and `artcache.c` call-site changes.

**Low**
11. Past 2,048 books: evict the oldest FINISHED, then the oldest STARTED (`ab store evict`).
12. Seed only books with stats; retry when the database becomes ready in the same boot (not only at the next boot).
13. The memory rule is **"no `core_alloc` while `audio_status()` is non-zero"**, not "after shell start" (§3.5, §4, §7.2).
14. Trace lines added: `ab map src=pak|fallback|resumed`, `ab seed n=%d`, `ab store evict book=%08lx`, `ab view cap=%d`; `ab slot w=%d` names the slot index (owner field removed).
15. Pool titles are cut on a UTF-8 boundary, and each title gets a fair share of what's left (`remaining / entries_left`, min 0), not first-come.
16. `HAVE_METADATA_EXTRAS` is defined explicitly in `chapters.h` (from `HAVE_IPOD_UI || WARBLE`) and used everywhere; `test_abpack` builds both with and without it.
17. The device has **64 MB**: net −59,432 B (now −64,552 B with the 11,280 B book block) ≈ **0.1 %** of RAM.

**Updated numbers:** book block 11,280 B; chapter transient 79,904 B (stopped only); playback slots 65,568 B; tdspeed 57,344 B (boot). Audio buffer net −64,552 B.
**New ⚠️:** 15. pak compaction at > 50 % dead bytes, in the stopped pass. 16. Multi-file books count book-wide entries ("entry N of M" across files). 12 revised: track-change saves pending; forced checkpoint ≤ 1 wake per 30 min while playing.

## Revision 2 (owner answers, 2026-10-08). These override the ⚠️ defaults.
- **⚠️16 resolved:** multi-file books show a **whole-book** counter ("N of M" across the book) and a **whole-book bar**, so the book feels like a single m4b.
- **AB-2 confirmed:** Slower / Normal / Faster = **0.75× / 1× / 1.25×**.
- **AB-1 changed:** selecting **any** book, including a single-file book with no chapters, **opens its page first** (Resume, then the chapter list). SELECT on Resume continues. There is no direct resume from the list.
- **⚠️13 confirmed:** a book counts as finished **within 30 s of the end**, or when its queue ends; it then shows no dot, and Resume starts over.
- **Book list (owner, 2026-10-08):** the 7G book list shows **no played/unplayed dots** at all. It doesn't track that. The only per-book state is whether a **resume spot** exists: the opened book's page shows **"Resume" only when there is a saved position** (unlike ⚠️5's "always shown"). Drop the dot/half-dot UI and the 2 B/row state column use for dots. `books.dat` still stores positions; "finished within 30 s" then simply means "clear the resume spot".

## Contracts (committed, 2026-10-09, branch `p6`). This overrides §4 and §5 where they differ.
Committed as real code with host round-trip tests and ONE shared stub, before fan-out. The headers below are **frozen**: a change goes through the orchestrator (a delta by SendMessage to the other agents), never a local edit. Stock `rockbox.ipod` is byte-identical to `p6-brief`'s (both built with `make VERSION=p6pin`, `cmp` equal); `english.lang` is untouched so far (U appends). `scripts/test.sh` passes (new `tests/abbook`), sim-ui and fw-ui build with no new warnings.

**Files**
| File | What | Owner after fan-out |
|---|---|---|
| `lib/rbcodec/metadata/chapters.h` | `HAVE_METADATA_EXTRAS` defined here; the packed `struct audiobook_chapters` (16 B header, `struct ab_ch_entry` 8 B, NUL-terminated pool), `AB_CH_SIZE`, `AB_CH_TABLE_SIZE` (32,784), accessors `ab_ch_cap/start/set_start/title_ptr/title_buf`, `ab_ch_init/reset/expect/commit/put/sort/find/title`; the stock forms (array expressions, `reset/expect/commit` = nothing); the chapter-art API takes the table | frozen |
| `lib/rbcodec/metadata/abpack.c` (+ `lib/rbcodec/SOURCES`) | the packed table, pure; empty in stock | M (API frozen) |
| `apps/ipodui/ab_book.h` | `books.idx` / `books.pak` structs, blob build/parse, `ab_path_crc()`, the book block (`struct ab_book_map` + `struct ab_store_index`), pure rules, `enum ab_speed`, runtime entry points (`ab_book_init/block`, `ab_speed_track_start/changed`, `ab_shell_owns_resume`, `ab_page_*`), the `AB_TR_*` trace lines | frozen |
| `apps/ipodui/ab_book_core.c` | its pure half | B |
| `apps/ipodui/ab_store.h` | `books.dat`: `struct ab_store_hdr` (double header), `struct ab_rec` (32 B), pure rules, runtime `ab_store_load/find/get/put/pending/idle/seed` | frozen |
| `apps/ipodui/ab_store_core.c` | its pure half | B |
| `apps/ipodui/ab_stub.c` | **the** shared stub (no record, no pak, no page entries, Normal speed, no map, `np_pb_seek_book` false) | B deletes it (SOURCES swap) |
| `apps/ipodui/np_playback.h` (delta) | `struct np_book_pos` (+ 28 B tick map) as `np_track.book`; `np_pb_seek_book()`; per-slot `np_pb_ab_write_begin/end(int slot)`, `np_pb_ab_seq(slot)`, `NP_AB_SLOTS`, `NP_AB_SLOT_ALL` | frozen; B implements |
| `apps/ipodui/np_scrub.h` | chapter column `const uint32_t *` | frozen; U |
| `apps/ipodui/np_layout.h` | `NP_BLK_TRACK_SIZE` 512 → 576 | U (offsets frozen) |
| `uisimulator/common/sim_cmd_np.c` | weak per-slot stubs | T |
| `tests/abbook/{run.sh,test_abpack.c,test_abstore.c,test_abbook.c}` | round trips (test_abpack built packed and stock) | M: test_abpack; B: the other two |
| Callers converted now (the tree must build): `mp4.c`, `id3extras.c`, `chapters.c`, `audiobook.c`, `audiobook_core.c`, `wps.c`, `skin_tokens.c`, `playback.c`, `np_playback.c`, `artcache.c`, `warble.c`, `test_scrub_real.c`, np tests | accessor form, stock-identical | per the ownership table |

**Decisions**
- **D1 Packed table.** Header `{int16 chapter_count, curr_chapter_idx; uint16 cap, pool_size, pool_used, cut, expect, spare}` = 16 B (same field names, so `ab->chapter_count` callers compile unchanged). Variable length: the owner calls `ab_ch_init(ab, cap, pool)` once (playback's scratch init, the art builder, warble, the chapter view with a smaller cap); `metadata_read_chapters()` resets and reads at most `ab_ch_cap()` entries. `sizeof()` is the header only: size with `AB_CH_SIZE()`. `id3_read_chapters()` takes titles first-come today (no count yet): M passes the CHAP count to `ab_ch_expect()`.
- **D2 Titles stay NUL-terminated in the pool** (deviation from §3.1 "no NULs", +1 B per title): `ab_ch_title_ptr()` is a C string in both layouts, so stock callers keep pointer code and nobody copies; `pool[0]` is the shared empty title. The last 64 B of the pool are decode room (never handed out): 24,512 B usable.
- **D3 Commit API.** `ab_ch_set_start(ab, i, ms)` + decode into `ab_ch_title_buf(ab, i)` + `ab_ch_commit(ab, i)` (the length is measured), or `ab_ch_put()`. Replaces Rev 1 item 10's `ab_ch_commit(ab, i, start, len)` so stock keeps its store order. Fair share = left / (expect − i), min 0, cut on a UTF-8 boundary, counted in `cut` and `AB_CH_F_CUT`. Measured: 1,024 titles of 63 B are all cut to 22 B; titles ≤ 22 B are never cut at 1,024 entries; every existing chapter golden is unchanged.
- **D4 `np_scrub` column is `uint32_t`** (the packed column; on the 64-bit sim `unsigned long` is 8 B). Test arrays changed with it.
- **D5 Stock identity.** All stock-compiled callers use the accessors except `skin_tokens.c`, which keeps its old lines under `#ifndef HAVE_METADATA_EXTRAS` (the accessor form moved register allocation). Proven by `cmp` of `rockbox.ipod`, `VERSION` pinned.
- **D6 Slots.** `np_pb_ab_write_begin/end(slot)` bump that slot's seq (`NP_AB_SLOT_ALL` = both, for the scratch re-carve) **and** `np_pb_seq()` (the screen's poll is unchanged). Until M lands two slots, `playback.c` keeps one table, initialised in `scratch_mem_init`, written as slot 0.
- **D7 `books.pak` blob** = 24 B header + structure-of-arrays map (`file_start_ms[n]`, `file_crc[n]`, `file_first[n]`: 10 B per file) + the trimmed table (cap = entries, pool = used, padded to 8). Max **43,048 B** (§3.3's 38,928 assumed 6 B per file without the CRC). Blobs and their sizes are 8-aligned; read into an 8-aligned buffer. Sum = `ab_word_sum()` (seeded, index-mixed), not CRC.
- **D8 `books.idx`** = 32 B header (`generation`, `pak_size`, `dead_bytes`, sum) + 24 B records `{key, max_mtime, file_count, flags, offset, size}`, sorted, `ab_idx_find()`.
- **D9 Book block 11,336 B** (Rev 1: 11,280): map 10,296 (24 B head incl. `src`/`gen`, the 28 B tick map, 10 B × 1,024 files) + store index 1,040 (sectors, count, clock, gen, 128 first keys). 8-aligned base (round the `core_alloc` up, as `artcache.c` does).
- **D10 `books.dat` record:** `book_len_ms` becomes `stamp` (the store's write clock), because eviction needs "oldest" and Rev 2 dropped the list progress that used the length. Check = CRC-8 (poly 0x07) over the record with the check byte masked, never 0 (zeroed slots fail). Header: both copies carry `seq` (wrap-safe pick) and a sum. A FINISHED record keeps no position (Rev 2: the resume spot is cleared). Eviction order: bad check first (lost anyway), then the oldest FINISHED, then the oldest STARTED.
- **D11 Whole-book ticks** are precomputed at map fill at `AB_TICK_RES` = `NP_BAR_W` = 216 bits into the block, copied into `np_track.book.ticks`, and rescaled by `ab_ticks_scale()` to the drawn bar (a tick may sit 1 px from a direct computation; merging as before).
- **D12 `np_track`** grows by `struct np_book_pos` (52 B on the host: 528 B); `NP_BLK_TRACK_SIZE` 512 → 576, `NP_BLK_END` 46,564; the reflection band still fits (test_np_layout asserts it).
- **D13 Counter source** = `ab_counter_source(book_queue, map_ok, is_audiobook, is_podcast, file_chapters, queue_count)`: BOOK (book queue whose map matched the playing file's path CRC; whole-book for multi-file, the chapters for single-file), QUEUE for podcasts always, FILE (P5 per-file chapters) for a chaptered book file outside a book queue or a map mismatch, else QUEUE, NONE with no queue. The counter is the entry position (`ab_book_entry_at()` / `ab_book_entry()`), and the album line is that same entry's title.
- **D14 Speed.** `enum ab_speed` 0/1/2, `ab_speed_factor()` 7,500 / 10,000 / 12,500 (`PITCH_SPEED_100` units); `ab_speed_for_track()` = books only, never podcasts. Setting `global_settings.ipodui_ab_speed` (B: `settings.h` + `settings_list.c`, guarded). M's guarded `playback.c` call `ab_speed_track_start(id3)` at codec track start (B implements; no status save); M's guarded auto-resume skip calls `ab_shell_owns_resume()` (Rev 1 item 1; B implements with `lib_queue_is_book()`).
- **D15 Book page** (`ab_page_open/has_resume/entries/entry_title/resume/play_entry/close`): U's Book list calls it; B implements it over the library chapter view. Rev 2: every book opens its page; "Resume" only when `ab_page_has_resume()`. The stub returns −1 from `ab_page_open()` (U then plays the book with `lib_play()`, as Phase 5).
- **D16 Trace lines** are the `AB_TR_*` macros in `ab_book.h`, logged with `UPDATE_TRACE`: `ab slot w=%d n=%d cut=%d`, `ab slot read slot=%d ok=%d` (owner dropped, Rev 1 item 3), `ab pak hit|miss book=%08lx`, `ab open book=%08lx reads=%d`, `ab map src=pak|fallback|resumed|none book=%08lx files=%d entries=%d`, `ab map mismatch q=%d`, `ab view cap=%d`, `ab store w=now|idle book=%08lx ms=%lu`, `ab store evict book=%08lx`, `ab seed n=%d`, `ab speed %d` (percent), `ab resume book=%08lx file=%d ms=%lu`. T's rules match the text before the first `%`; a rule that finds none of its lines fails.
- **D17 Finished** = `ab_finished(book_ms, len)`: within 30 s of the end; a book shorter than 30 s only at its very end; unknown length never. The book's queue ending passes `book_ms = len`.
- **D18 The stub** is the only one; no `TEMP-` commits.

**Scripts (T), adjusted for Rev 2:** `ipodui-p6-booklist` checks there are **no** dots; `-open` checks "Resume" is shown only with a saved spot; `-nochap` checks a single-file book without chapters opens its page (AB-1 changed); `-start-at-zero` (Rev 1 item 1); the rest as §8.

**Read lists (≤ 3 each; the contract headers are read as code, not counted)**
- **M:** this brief §3.1–3.2, §6, Rev 1 items 1, 3, 6, 10, 15, 16 and this section; `docs/design/chapter-art.md` (API + cost); issue #50 (`scripts/forgejo.py show 50`). Code: `chapters.h`, `np_playback.h` (slot hooks), `ab_book.h` (speed and resume hooks, `AB_TR_SLOT_*`).
- **B:** this brief §3.3–3.6, Revisions 1–2 and this section; `docs/design/chapter-art.md` (P2/P3); `docs/design/shuffle.md` (book flag). Code: `ab_book.h`, `ab_store.h`, `np_playback.h`, `chapters.h`, `library.h`.
- **U:** this brief §3.4, §3.7, Revision 2 and this section; `docs/design/spec/audiobooks.md` + `settings.md` (Audiobooks item); `docs/design/spec/now-playing.md` (Audiobooks + the 2026-10-08 counter note). Code: `ab_book.h` (page API, counter source, ticks, speed), `np_playback.h`, `np_layout.h`.
- **T:** this brief §7–§9, Revisions 1–2 and this section (scripts, trace lines); `tests/ui/README.md`; `docs/phase-nav-brief.md` Revision 2 (`record`, `make_video.sh`). Code: `ab_book.h` (`AB_TR_*`), `ab_store.h` (file layout, for `ab_pin_store` / `ab_power_cut`).

**Ownership (final; one owner per file)**
| Agent | Owns | Must not edit |
|---|---|---|
| **M** (playback-audiobook, top model): metadata + playback hooks | `chapters.c`, `abpack.c`, `mp4.c`, `id3extras.c`, `id3tags.c` (#50), `apps/audiobook.c`, `audiobook_core.c`, `apps/playback.c` guarded hooks (two slots + writer rule + both cleared in `scratch_mem_init`, `ab_speed_track_start()` call, `ab_shell_owns_resume()` skip, `AB_TR_SLOT_W`), `wps.c` / `skin_tokens.c` (accessor lines only), `lib/rbcodec/test/warble.c`, `tests/audiobook/*` + their fixtures (#50), `tests/abbook/test_abpack.c` | frozen headers, `apps/ipodui/*` |
| **B** (playback-audiobook, top model): books, store, library, Now Playing data, speed | new `ab_store.c`, `ab_book.c`, `ab_speed.c`; `ab_book_core.c`, `ab_store_core.c`; deletes `ab_stub.c`; `artcache.c` (books.pak/idx pass, compaction), `library.{c,h}` (book page, chapter view, map fill, resume, `lib_queue_resumed()` refill), `np_playback.c` (slot reader, `np_track.book`, `np_pb_seek_book()`, store triggers), `ui_main.c` (only the `ab_book_init()` call and the forced `timestretch_enabled`), `apps/settings.h` + `settings_list.c` + `apps/settings.c` guarded lines (the speed setting; no status save), `tests/abbook/test_abstore.c`, `test_abbook.c`, `tests/nowplaying/*`, `tests/artcache/*` | frozen headers, screens, `np_draw`, M's files |
| **U** (ui-fidelity, top model): screens | `screens/audiobooks.c` (Book list via `ab_page_*`, no dots), `screens/browse.c` (book rows only), `screens/settings.c` (Audiobooks prompt + caption), `screens/nowplaying.c` + `np_draw.{c,h}` (whole-book bar, ticks rescale, counter source), `np_scrub.{c,h}` (column type frozen), `np_layout.h` (offsets frozen), `apps/lang/english.lang` (appended), `tests/npscreen/*` | frozen headers, B's, M's and T's files |
| **T** (tooling, Sonnet) | `tests/ui/ipodui-p6-*.script`, `tests/fixtures/ab/*`, new `uisimulator/common/sim_cmd_ab.c` (+ its registration in `sim_script.c`), `sim_cmd_np.c`, `scripts/check_trace.py` P6 rules, `scripts/gate_map.txt` P6 entries, compose sheets, `ab-review.mp4` | `apps/`, `lib/` |

`apps/SOURCES` and `lib/rbcodec/SOURCES`: additions only (inside `#ifdef HAVE_IPOD_UI` for apps), requested from the orchestrator. `docs/hardware.md` budget table: orchestrator at close (book block 11,336 B; blob ≤ 43,048 B; playback slots 2 × 32,784 = 65,568 B; tdspeed 57,344 B).

## Revision 3 (owner round, 2026-10-09). These override anything above.
- **Finished:** pausing within 30 s of the end finishes the book (⚠️13, D17): **confirmed**.
- **Book list:** 2-line rows with the cover and the author under the title. A separate UI agent does this after the fix round (not part of it).
- **Resume row with no saved spot:** hidden (**confirmed**, Rev 2).
- **Now Playing in the main menu** (owner: "Now Playing should persist as long as we haven't STOPPED: an audiobook ended, a playlist ended without repeat, etc. If the queue is cleared, and we aren't paused mid-song/book, Now Playing disappears. If we have, since startup/boot, played a song, keep Now Playing until such a state occurs."). `ui_now_playing_visible()` (`ui_main.c`):
  - **shown** while playing or paused mid-track; ~~after a reboot, while a restored paused queue can be resumed (`resume_index` set) ⚠️17 (default; owner to confirm)~~;
  - **Overturned by the owner (#58, 2026-10-09):** after a reboot the row is **hidden until something has played in this boot**. PLAY still resumes the restored queue, and the row appears once playback starts. `ipodui-np-row-paused` checks both (hidden after the reboot, shown after PLAY);
  - **hidden** after a natural end of the queue (a book ended, a playlist ended with repeat off: playback clears the resume info), after the queue is stopped or cleared (a `PLAYBACK_EVENT_STOP_OR_PAUSE` that is not a pause, this boot), and at boot with nothing to resume;
  - a saved books.dat spot alone does **not** keep the row: the book page's Resume covers it.
  - Scripts: `ipodui-np-queue-end` (a 1-track queue plays out: Now Playing pops, row gone), `ipodui-p6-book-end` (a single-file book plays out: pops to the page, row gone, book finished), `ipodui-np-row-paused` (paused: row stays; paused + reboot: row hidden until PLAY resumes). Traced `np pop stopped`, `np row visible=N`.
