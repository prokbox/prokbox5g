# Phase 4b brief: rich rows, thumbnail art cache, precomputed lists

**Goal:** the album and podcast lists look like the 7G (owner, issue #24).
- **Two-line rows.** On the left, a square image:
  - **mini album art** for albums;
  - the **generic music-note** image for "All Songs";
  - the show art or a generic glyph for podcasts.
- The bold title, then a smaller second line: **"13 Songs"**, **"147 Songs"** or **"7 Episodes"**.
- A **blue unplayed dot** on podcasts and episodes.
- At 50k tracks, lists open fast (#37) because grouped lists and counts are **computed at commit time**, not when a list opens.

Read first: `CLAUDE.md`, **`docs/process.md`** (lifecycle, checklist, ownership, self-review), `docs/hardware.md`, `docs/design/artcache.md` (§2 sizes, §4 layout, §5 keying, §6 invalidation, §8 builder scheduling, §11 API), `docs/design/spec/index.md` (Navigation model, List geometry) and `docs/design/spec/music-lists.md` (two-line rows), and the owner's photos `docs/reference/7g/owner/labeled/artist-albums*`, `podcast*` and `album*` (local).

**Out of scope:**
- Cover Flow, Now Playing art, the main-menu panel art and per-chapter art (#39). Phases 5–7 use the **same cache**, so the format must already allow those sizes, but only `ARTCACHE_THUMB` is built now.
- The host-side Mac art tool (Phase 7b).
- The navigation-model framework (#38), which comes next.

## Work split (worktrees; one owner per file)

| Agent (model) | Owns | Must not edit |
|---|---|---|
| **A: library and commit** (playback-audiobook, top model) | `apps/tagcache.{c,h}` (all edits, guarded); `apps/ipodui/library.{c,h}`; perf harness additions under `tests/perf/` | `artcache.*`, `list.*`, `screens/*`, sim tooling |
| **B: art cache** (firmware-platform, top model) | new `apps/ipodui/artcache.{c,h}`, the builder thread, `.rockbox/artcache/` format; host tests `tests/artcache/` | `tagcache.*`, `library.*`, `list.*`, `screens/*`, sim tooling |
| **C: rows** (ui-fidelity, top model) | `apps/ipodui/list.{c,h}` (row-height variant), `screens/browse.c`, `screens/audiobooks.c`, `paint.*`/`theme.*` (new glyphs and metrics), `art/*.py` | `tagcache.*`, `library.*`, `artcache.*`, sim tooling |
| **D: verification** (tooling, Sonnet) | `uisimulator/common/sim_*`, `scripts/*`, `tests/ui/*` (scripts, library generator art variety, README), `tests/ui/golden/` (proposals only) | `apps/` |

`apps/SOURCES`: additions only, merged by the orchestrator. The `docs/hardware.md` budget table: the orchestrator only; agents report their numbers.

## Contracts

```c
/* library.h: additions (A implements; C calls). Counts come from the
 * commit-time precompute, so they are O(1) per call. */
int  lib_child_count(int view, int idx); /* "13 Songs", "7 Episodes"; -1 if n/a */
bool lib_unplayed(int view, int idx);    /* podcast/episode never played */
uint64_t lib_art_key(int view, int idx); /* artcache_key_t for the row; 0 = none
                                            (B's artcache_key() over the same
                                            strings the list groups by) */
/* Called by tagcache (A's guarded hook) after every successful commit +
 * ramcache load. A implements it and calls artcache_build_request(). */
void lib_committed(void);

/* artcache.h: B implements exactly docs/design/artcache.md §11 (sizes
 * table incl. all four ids, only THUMB built in 4b). C uses: */
int  artcache_get(artcache_key_t key, enum artcache_size size,
                  struct bitmap *bm, void *buf, size_t bufsize);
enum artcache_result artcache_state(artcache_key_t key);
artcache_key_t artcache_key(const char *albumartist, const char *artist,
                            const char *album, const char *first_track_path);
void artcache_build_request(void);
void artcache_build_pause(bool pause);
void artcache_build_status(struct artcache_build_status *st);
```

Each agent keeps **TEMP stubs** of the contract parts it doesn't own (B's artcache for A and C; A's new library calls for C). They go in a separate final commit titled `TEMP-x: … (drop at merge)`.

## Agent notes
- **A, precompute (#37):**
  - At commit, after the sort, write compact per-category group lists to the database directory: album groups and their row ranges, artist → albums, genre → artists, and child counts. Use bounded memory from the commit buffer.
  - `lib_open` maps them instead of scanning. **Target: Albums and Songs at 50k open in ≤ 300 ms on the device estimate.** Measure with `tests/perf/lib50k.sh`.
  - Unplayed state for podcasts: playcount 0 and no `lastelapsed`.
  - `lib_committed()` hook: a single guarded call in `tagcache.c` after a successful commit and reload.
  - Old precompute files stay valid across rebuilds through the database version.
- **B, the art cache, thumbnail tier only:**
  - Follow the format, keying and invalidation in artcache.md. Thumbnails are packed into `thumbs.pak`, giving one fd and no per-row opens.
  - **The builder thread** runs at background priority on the CPU core and **never boosts**. It processes one album at a time and sleeps between them. It pauses on input within 2 s, while the audio buffer is filling, during USB, and when the battery is under 20% and not charging.
  - **Sources:** embedded art (ID3 APIC, MP4 `covr`) and folder images (`cover.jpg`, `folder.jpg`, etc.). Baseline JPEG and BMP only; progressive JPEG and PNG are marked `FAILED`, which shows the note glyph (the Mac tool comes in 7b).
  - **Thumbnail size:** C measures it from the owner's photos (≈ 40–48 px). It is a single entry in B's size table.
  - The format already has room for the other three sizes.
- **C, rows:**
  - A **two-line row** variant in `list.c`: row height, thumbnail box, title and subtitle baselines, all measured from `artist-albums*` and `podcast*`. The subtitle uses the small font in the measured colour, and the selected row's subtitle colour is taken from the photo.
  - Glyphs, original and in the gloss style: the generic **music note** for "All Songs" and for albums with no or pending art, a **podcast** glyph, and the **blue unplayed dot**.
  - Thumbnails go in a per-list LRU of about 16 entries in one `core_alloc`, fetched **only for newly visible rows, and never mid-glide**. While gliding, draw the cached thumb or the glyph; fill in the rest at rest.
  - Smooth scrolling and the quick-scroll letter must keep working with the taller rows.
  - **Which screens use two-line rows:** match the photos. Artist → albums, Albums (⚠️ check), Podcasts, and episodes. Songs stays single-line.
- **D, verification:**
  - Add cover-art variety to `setup_library.sh`: embedded baseline JPEG, `cover.jpg`, PNG, progressive JPEG, no art, and a non-square cover, all original generated images.
  - Add scripts: `ipodui-p4b-rows` (artist albums, Albums, podcasts, episodes) and `ipodui-p4b-artbuild` (fresh database → thumbnails appear as the builder progresses; glyph while pending).
  - **New trace rules:** `art get` lines never appear between `anim start` and `anim end`; `art build` never holds boost; at most one `art get` per newly visible row.
  - Host tests: `tests/artcache/` for keying (golden keys over the fixture tags) and for header and index parsing.

## Device-reality checklist (answers required; reviewers check)
1. **Compiled features:** the art builder uses the JPEG and BMP loaders compiled into the target (`HAVE_JPEG`, `HAVE_BMP_SCALING`). The precompute runs in the commit path (UI_COMMIT), the same on the sim and the device.
2. **Memory during playback:**
   - The builder's decode buffer (about 185 KB, artcache.md §9) and the thumbnail LRU (about 51 KB) are allocated **only when they fit beside playback**. Otherwise the build waits.
   - The precompute uses the bounded commit buffer.
   - Nothing calls `core_alloc_maximum`.
3. **CPU at 50k:** precompute cost per commit, measured in the sim with a device estimate. Art build: about 0.5–2 s per album on the device, so a first build of 5k albums takes 1–3 h, unboosted and in the background.
4. **Storage:** one `thumbs.pak` fd while browsing. The builder reads each source file once and writes in batched appends. Count the flash wake-ups per album.
5. **LCD:** thumbnails are drawn inside row bands, and nothing new is pushed mid-glide.
6. **Threads:** the art thread vs the tagcache commit vs the UI. Readers must handle `thumbs.pak` being appended while they read (index validity), so read under a version or generation check.
7. **Splashes:** none from the JPEG or BMP loaders or the resize code paths. Silence or avoid any that exist.
8. **Persistence:** the cache survives reboots; there is an invalidation path (§6); a corrupt cache leads to a silent rebuild; a power-off mid-append leaves the index consistent.
9. **Scale:** `thumbs.pak` at 5k albums is about 16 MB; the index lookup is O(1) or O(log n).
10. **Deferred work (process item 10):** the builder's pause and resume are tested in each database state (ready, fresh or rebuilding, dirty) and during playback.
11. **Stock firmware unchanged:** stock `.text` is byte-identical.

## Acceptance (orchestrator)
1. The brief review passes before any code is written.
2. One full `scripts/validate.sh` passes at integration, including the new scripts, trace rules and host tests.
3. The end review mostly confirms.
4. **Owner round:** a side-by-side of our rows against `artist-albums*` and `podcast*`, then golden approval. The Phase 4 goldens for the artist and podcast screens are regenerated only after that review.
5. Library open times at 50k are recorded, with Albums and Songs at ≤ 300 ms on the device estimate.

## Revision 1 (after the brief review, 2026-10-06). This section overrides anything above that conflicts with it.

### Mini-phases (split by risk)
- **4b-1, headless core:** A (library precompute) + B (art cache format, builder and keys), with **its own end review** before 4b-2 merges.
- **4b-2, UI:** C (rows) + D (verification). Starts once 4b-1 is merged. D may start its generator and host-test work during 4b-1.

### Decisions
1. **Memory: reserved at shell start, never allocated during playback.**
   - The thumbnail LRU (16 × 40×40 × 2 B ≈ 51 KB) and the builder's decode/scale work buffer (≈ 185 KB, artcache.md §9) are each **one `core_alloc` at shell start**, like the fonts, and are budgeted in `docs/hardware.md`. That is about 236 KB out of a 60 MB audio buffer (≈ 0.4%).
   - The builder therefore runs **during playback too**: unboosted, at background priority, and paused while the audio buffer is filling.
   - The ~2 KB `mp3entry` and any parser state live in that reserved block, **never on the builder's stack**.
2. **No boost from the resize or JPEG paths.** `apps/recorder/resize.c:811/882` calls `cpu_boost` unconditionally. B adds a guarded no-boost option (`#ifdef HAVE_IPOD_UI`, a flag in the resize/JPEG load call or a thread-local "noboost") and owns that `resize.c` edit. Add a trace-rule test.
3. **Precompute lives in `library.c`, not in `commit()`.**
   - It runs on the tagcache thread **after `load_ramcache()` succeeds** (hooking its success path, all call sites including the deferred `ui_reload_pending` one) **and after the deleted-file pass**.
   - It uses the strings in RAM and reuses `lib_open`'s class, compilation and case-merge rules.
   - It has its own fit check and deferred state, and is tested in the ready, fresh and dirty states (process item 10).
   - Commit time and the power-off window are unchanged.
4. **Precompute files are stamped and atomic.**
   - Each header carries the **master commit id, the entry count and `SORTKEY_VERSION`**. The file is written to a temp name and **renamed**.
   - On a mismatch or a missing file, `lib_open` falls back to the current scan.
5. **Live data stays live.** `lib_unplayed()` and progress read the RAM index **live**. Counts come from the precompute and are refreshed by the post-deletion pass.
6. **Album enumeration for the builder.** A's precompute also writes an **album table**: art key, representative entry index and max `tag_mtime` per album. B's builder reads that table. It never touches the UI-thread view pool and never needs filenames in RAM; it resolves the representative's path through tagcache on its own thread.
7. **Keys.** A pure `apps/ipodui/artkey.{c,h}` (owned by B) holds `artcache_key()` and its `norm()`. Include direction: **`library.c` → `artkey.h` / `artcache.h`; `artcache` never includes `library`**.
   - Keys are **precomputed** by A, one per album row, using exactly the grouping strings, including untagged directory keys and podcast shows grouped by album only.
   - `lib_art_key()` returns the precomputed key, with no disk read at browse time.
8. **USB and power-off.** The builder thread has a queue and acks `SYS_USB_CONNECTED`; on USB or power-off it closes every fd and stops at a safe point. The UI's cached `thumbs.pak` fd is closed on `SYS_USB_CONNECTED`/power-off and reopened afterwards.
9. **Index consistency.**
   - `index.dat` is rewritten as a new file plus a rename, and a **generation number** is bumped.
   - Readers re-open on a generation change.
   - `thumbs.pak` is append-only, and index entries point only at complete records.
10. **Taller rows vs the strip.** C makes the strip slot math use the **actual row height of the list** (not the global `row_h`), and logs if glides get disabled. A two-line list must still glide; check that the 160 KB strip holds enough rows at about 50 px, and resize the reservation if needed (budget line).
11. **Ownership changes:** B owns `apps/recorder/resize.c` (guarded edit) and `apps/ipodui/artkey.{c,h}`. A still owns all `tagcache.c` edits (the hook calls into `library.c`). Stub files are named `apps/ipodui/artcache_stub.c` and `library_stub.c`, each with an `apps/SOURCES` line in the TEMP commit.
12. **Ownership:** callers `core_free()` the handle from `artcache_get_handle()`. `get_metadata()` reentrancy: the builder thread calls it with its own `mp3entry` and fd only.

### Checklist corrections
- **Item 2:** see Decision 1. **Nothing** is allocated after playback starts.
- **Item 3:** precompute budget ≤ 2 s on the sim at 50k (≈ device minutes, background). Measure and record it.
- **Item 4:** the artcache.md §6 invalidation sweep costs about 5k `stat`s per update with dircache off. Batch it with the deletion pass and count the wake-ups.
- **Item 7:** the `resize.c` boost (Decision 2). Also check the JPEG and BMP loaders for any splash or yield.
- **Items 8 and 10:** add **USB mid-build**, a **builder blocked by `tagcache_ui_busy()`/a pending commit** (it must pause, not race), and power-off mid-build. Each gets a test script (D).
- **Visual check for C:** does the 7G show a **half-dot** for partly played episodes? Check the photos and mark it ⚠️ if unknown.
- **Absorb issue #40** (A): the Phase 4 review lows, namely the cancel flag after timeout, the import table fit check when stopped, the M2/M3 stop-path tests, the `sim_db` offset and the docs typo.

## Revision 2 (confirmation review: APPROVE-with-edits, 2026-10-06)
- **D10, strip:** the shared strip becomes **7 slots at the two-line row height (≈50 px): 7 × 50 × 320 × 2 = 224,000 B** (was 160,000 B; **+64 KB**, budget line). Slot math uses the list's actual row height. Single-line lists keep their extra slots. C confirms 50 px from the photos and resizes if the measurement differs.
- **D1, buflib:** the reserved blocks are movable.
  - The builder calls `core_pin()` on its block **only for the duration of one decode** and re-fetches the pointer after every yield or sleep. It **never stays pinned across a sleep**, so compaction and audio-buffer shrinking are never blocked.
  - The thumbnail LRU is accessed only from the UI thread and re-fetched per frame.
  - Both blocks are allocated at shell start, **before the audio buffer is set up**.
- **D1, builder I/O during playback:** read or write the disk **only while it is already active for an audio refill** (check `storage_disk_is_active()` or the equivalent). **Never wake storage on its own** while playing. When stopped, normal throttling applies.
- **D3, precompute on the tagcache thread:**
  - It runs in **chunks that yield and drain the tagcache queue** (USB, commit, shutdown), is cancellable, and resumes or restarts later.
  - A **library generation** number is bumped when the new files are complete. Open UI views re-check it and reopen, never mixing old and new.
  - Readers **validate the header**: commit id, count, `SORTKEY_VERSION` and a **checksum**. They don't trust the rename (FAT rename-overwrite is not crash-atomic, `file.c:1098`). A torn or invalid file falls back to the scan, followed by a silent rebuild.

## Revision 3 (owner observation and photos, 2026-10-06). Applies to 4b-2 (C)
- **Bug to fix:** our lists leave a **20 px white strip** at the bottom. The rows are 25 px × 8 = 200 of a 220 px body, and gliding only draws a 200 px window. 7G lists **fill the body exactly**; see the spec's "List geometry".
- **Row styles (C):**
  - single-line **9/screen**;
  - **Songs two-line 5/screen** (title above artist; this corrects "Songs stays single-line" above);
  - **Albums and artist → albums two-line with art 4/screen**;
  - podcasts and episodes two-line.
  
  Measure each pitch and the body height precisely. The glide and the strip cover the **full body**.
- **Strip size:** recompute for the tallest row (≈55 px): 7 slots × 55 × 640 B ≈ **246 KB**, replacing Revision 2's 224 KB. C confirms the number; budget line.
- **Goldens:** every list golden (Phases 2–4) will change. Regenerate them only after the owner's side-by-side review.

## Revision 4 (owner, 2026-10-06)
- **Songs stays single-line.** Revision 3's "Songs two-line 5/screen" is withdrawn. Two-line song rows are ⚠️, pending the owner identifying their context.
- **The priority for C:** single-line lists must **fill the body exactly** (9 rows, pitch and body height measured) and glide across the full body, with no white strip. Two-line rows apply only to **albums (with art), artist → albums, podcasts and episodes**.
- `lib_sublabel()` (requested from A) stays in the contract for those two-line lists and future contexts. Songs views may return NULL.

## Revision 5 (owner, 2026-10-06, after viewing `songs-list-marquee-b`)
- **Songs is two-line after all:** title above artist, 5 per screen, filling the body exactly. This supersedes Revision 4's Songs line. An album's own track list: ⚠️ check (possibly single-line).
- `lib_sublabel()` returns the **artist** for Songs views (required again).
- Every list, single- or two-line, **fills the body exactly** and glides across its full height.
