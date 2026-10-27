# Brief: Now Playing (Phase 5, #4) with #27, #35, #39, #49

Execution order 7. Developed on branch **`np` off `nav`**, in parallel with #38. Lifecycle per `docs/process.md`: brief → brief review → contracts commit → fan-out with self-review → one full gate → end review → one owner round → close. Written 2026-10-07 overnight with the owner asleep: every open fact is frozen as ⚠️ **with a default** (list at the end) and is not revised mid-phase.

## 1. Owner pre-flight
- **Answered and authoritative** (spec `now-playing.md`, owner 2026-10-06/07): cycle **Normal → Scrubber → Rating → Shuffle (music only) → Lyrics (if present) → Normal**; audiobooks **Normal → Scrubber → Rating → Normal** (Rating yes, Shuffle no). Wheel = volume in Normal. Scrubber: blue fill disappears, blue gradient **diamond** at the position; **playback jumps once, after the wheel stops** (`NP_SEEK_SETTLE_MS` 250). **A track change in any mode drops to Normal.** Every mode drops to Normal after **4 s** without input (`NP_MODE_TIMEOUT_MS`). Rating: **5 dots** (near-black, photo `np-rating-unrated-b`, owner decision 2026-10-06) turn into **blue gradient stars**, clockwise adds, counter-clockwise removes. Shuffle: shuffle icon + **Off / Songs / Albums** on a light **shelf**, the selection in a **square, raised, button-like box** with blue text. Mode changes: **"the scrubber row slides off-screen to the left"**, "the rating row slides off to the left and the shuffle control appears", "the current row slides off to the left and the normal progress bar returns". Audiobooks: album line = **chapter title**, counter **"73 of 648"**, bar spans the **whole book** with **chapter ticks**, fast wheel **snaps to chapter starts**, **per-chapter art** switches at boundaries (#39). NP is **fully native** (no skin engine, #4 comment 2026-10-06).
- **Still open, frozen with defaults:** NP-1 (lyrics source) → **embedded USLT/©lyr only**; NP-2 (podcast cycle) → **same as audiobooks**; NP-3 (in-cycle Speed) → **none**. Everything else is in §9.
- **Captures still wanted** (owner list `INDEX.md` "Now Playing"): SELECT cycle on video (music, audiobook, podcast), a track with lyrics, Hold on Now Playing. The owner round compares against the 11 NP photos we have.

## 2. Scope
**In:**
1. **The NP screen** (`screens/nowplaying.c`), full-screen push (`ui_push_t(…, UI_T_PUSH)`, 250 ms split→full) under the normal header (title "Now Playing", Time in Title alternation from #38's `header.*`).
   - **Art:** from the art cache only, `ARTCACHE_NOWPLAYING`, drawn with the **perspective slant** (x 10–147; left edge y 34–175, right edge y 43–167), a reflection fading to white over ~25 px, and our slate-blue **note placeholder** (slanted and reflected too) on `NOART`/`PENDING`.
   - **Text** at x 160: title (Bold 15, F2 marquee, clipped, wide loop gap), artist and album (truncated with "…"), small near-black rating stars (blank when unrated), "n of N". The shuffle/repeat glyphs sit at x 267–312, y 23–37.
   - **Bar row:** progress bar x 58–261, y 206–219, glossy blue fill; elapsed at x 23, "−m:ss"/"−h:mm:ss" ending at x ≈ 302.
2. **SELECT cycle** wired through `np_state`/`np_scrub` (prep code on main, host-tested): Scrubber, Rating, Shuffle shelf, Lyrics, the row slides, the timeout, and the drop to Normal on a track change.
3. **Volume overlay:** the wheel in Normal swaps the whole bar row for the volume bar (speaker without waves on the left, with waves on the right, glossy fill, ≈ x 40–280) for `NP_VOLUME_SHOW_MS`.
4. **Audiobook variant:** chapter title, the normal **queue** counter "N of M" (file position in the play queue, e.g. 73 of 648 audiobook files; NOT a chapter count, owner 2026-10-07), whole-book bar and times, chapter ticks (`np_chapter_ticks`), fast-scrub snapping (`NP_SCRUB_SNAP_V`), and per-chapter art (key per chapter, falling back to the book cover). Podcasts use the same variant.
5. **Writes:** ratings to the database (`tagcache_update_numeric(idx, tag_rating, stars × 2)`), seek (`audio_ff_rewind`, once per gesture), volume, and the shuffle setting.
6. **Lyrics** (#35): `metadata_read_lyrics()`, read when the Lyrics mode is entered, wrapped and scrolled by the wheel.
7. **Whole-library shuffle** (#27) per `docs/design/shuffle.md` integration points 1–8: a library source for Shuffle Songs and over-cap Songs, refills, resume, library-changed handling. The **shelf** acts on either kind of queue.
8. **#49:** a `LIB_ALBUMS` TRACKS column in the libfile, so Albums shuffle starts an album with one small read.
9. **Art builder:** `ARTCACHE_NOWPLAYING` bundles for every album, plus per-chapter bundles (#39 hook, `chapter-art.md`).

**Out:** the Audiobooks menu, Resume and the chapter list, speed, and the growable chapter table (P6); Cover Flow (P7); the Hold-SELECT menu (P11: hold SELECT in NP does nothing in P5); Settings › Shuffle/Repeat screens (P8, which reads the same setting); the main-menu "Now Playing" preview panel (#38 B); `.lrc` lyrics.

## 3. Read list per agent (≤ 3 each)
- **N (screen):** `docs/design/spec/now-playing.md`; `docs/design/spec/index.md` (only *Visual language* and *Transitions*); this brief §2–§6. Contract headers: `np_state.h`, `np_scrub.h`, `np_playback.h`. Photos: `docs/reference/7g/owner/labeled/np-*.jpg` (local-only, never committed).
- **P (playback/library):** `docs/design/shuffle.md`; `docs/design/chapter-art.md`; this brief §4–§6. Plus `apps/ipodui/artcache.h` and `libfile.h` as code.
- **T (tooling):** this brief §7; `tests/ui/README.md`; `docs/phase-nav-brief.md` Revision 2 (`record`, `make_video.sh`).

## 4. Contracts (real code, committed on `np` before fan-out by the orchestrator, with a host round-trip test and ONE shared stub)
**From #38 (consume, never edit).** `ui_push_t`/`UI_T_PUSH`, `struct ui_screen_ops` (`enter/leave/draw/input/timeout/tick`), `ui_invalidate_band`, `ui_strip_claim/put` (`anim.h`), header title and Time in Title, `struct ui_gloss` + `ui_paint_gloss` (`paint.h`), and the **global buttons** (Rev 3: NEXT/PREV tap and hold, audiobook chapter skip, PLAY, hold PLAY, hold MENU). NP returns `UI_IGNORED` for those keys and handles only SELECT, MENU (pop) and the wheel.

**Hook requests to #38 (orchestrator applies them on `nav` before `np` branches; ≤ 15 lines):**
- **NAV-H1:** `ui_show_wps()` (`ui_main.c`) pushes `ui_nowplaying_screen` when `audio_status()` or the resume succeeds. Declared in new `apps/ipodui/screens/nowplaying.h`. On `nav` the **shared stub** `screens/nowplaying.c` wraps today's `gui_wps_show()` path and returns <0 from `enter()`, so nav behaves exactly as now. N replaces the file.
- **NAV-H2:** the main loop folds `int ui_nowplaying_idle(void)` (ticks until the next call, or `TIMEOUT_BLOCK`) into its timeout, each pass, on the UI thread. The stub returns `TIMEOUT_BLOCK`. It runs P's refill even when NP isn't shown.

**`np` contracts commit:**
```c
/* apps/ipodui/np_playback.h: UI-thread facade (P implements np_playback.c;
 * the shared stub np_playback_stub.c fills it from audio_current_track()
 * with no lyrics, no chapters, no shuffle, so N can work from hour one). */
#define NP_TXT 128
struct np_track {                  /* copied at a change; never on the stack */
    uint32_t seq;                  /* bumps on track/chapter/metadata change */
    char title[NP_TXT], artist[NP_TXT], album[NP_TXT]; /* album = chapter title */
    bool is_audiobook, has_lyrics; /* has_lyrics: id3->lyrics, no I/O */
    int  stars;                    /* 0..5 (tag_rating / 2) */
    int  index, count;             /* "n of N": queue pos, or chapter of chapters */
    unsigned long length_ms;       /* track, or the whole book */
    struct np_scrub_ctx scrub;     /* chapter starts: playback's fixed carve */
    artcache_key_t art_key;        /* chapter key when it has art, else album */
    long tc_idx;                   /* id3->tagcache_idx (0 = none) */
};
uint32_t      np_pb_seq(void);                        /* cheap poll */
bool          np_pb_snapshot(struct np_track *t);     /* false: nothing playing;
                                                         retries if seq moved */
unsigned long np_pb_elapsed_ms(void);                 /* whole-book for books */
void np_pb_seek(unsigned long ms);
void np_pb_volume_step(int clicks); int np_pb_volume_pct(void);
void np_pb_set_rating(long tc_idx, int stars);        /* rides storage idle */
enum np_shuffle np_pb_shuffle(void); void np_pb_set_shuffle(enum np_shuffle s);
int  np_pb_repeat(void);                              /* REPEAT_* */
int  np_pb_read_lyrics(char *buf, size_t size, bool *truncated);
int  np_pb_idle(void);                                /* refill; NAV-H2 via N */
#ifdef SIMULATOR
void np_pb_sim_pin(long elapsed_ms, int volume_pct);  /* T's np_pin command */
#endif

/* apps/ipodui/lib_shuffle.h (P): library-backed source, docs/design/shuffle.md */
struct lib_shuffle_rec {        /* .rockbox/npshuf.dat, written after the
                                   playlist control file is synced */
    uint32_t magic, version;
    uint32_t db_gen, db_commit; /* tagcache_ui_generation()/commitid() */
    uint8_t  mode, repeat, pad[2]; /* enum np_shuffle */
    union { struct shuf_songs s; struct shuf_albums a; } it;
    uint32_t crc;
};
int  lib_shuffle_start(enum np_shuffle mode, uint32_t first); /* SHUF_NONE */
bool lib_shuffle_active(void);
void lib_shuffle_track_changed(void);   /* audio-thread event: flag only */
int  lib_shuffle_idle(void);            /* refill; ticks to next check */
int  lib_shuffle_set_mode(enum np_shuffle mode); /* shelf commit */
```
- `np_state.h` gains `void np_set_lyrics(struct np_state *st, int lyrics_max);`, called after the text is read on entering Lyrics. N owns it.
- `artkey.h` gains `artcache_key_t artcache_key_chapter(artcache_key_t book, int chapter);` (P), with golden keys in `tests/artcache/`.
- `libfile.h` gains the TRACKS section id and its record (#49, P), and bumps the libfile version (rebuilt by the precompute automatically).
- `english.lang` gains `LANG_NP_N_OF_M` ("%d of %d"), `LANG_NP_SHUFFLE_SONGS`, `LANG_NP_SHUFFLE_ALBUMS` (reuse `LANG_OFF`); N formats with `str()`. The orchestrator resolves the append against #38 B's IDs at merge.
- **Round-trip host tests:** `tests/nowplaying/test_shuffle_rec.c` (write, read, CRC, a stamp mismatch → restart) and the chapter-key goldens.

## 5. Ownership (worktrees; one owner per file)
| Agent | Owns | Must not edit |
|---|---|---|
| **N: NP screen** (ui-fidelity, top model) | `screens/nowplaying.{c,h}` (replaces the stub), new `np_draw.{c,h}` (slant and reflection, bar, diamond, dots and stars, shelf, volume, digit atlas, marquee buffer), new `np_lyrics.{c,h}`, `np_state.*`, `np_scrub.*`, `art/npglyphs.py` → `npglyphs.h` (original glyphs: diamond, star, dot, speakers, shuffle, repeat, note placeholder) | every #38 file (`screen_stack.*`, `anim.*`, `ui_main.c`, `preview.*`, `menu.c`, `header.*`, `paint.*`, `theme.*`, `list.c`, `screens/settings.c`, `screens/panels.c`); P's and T's files |
| **P: playback/library** (playback-audiobook, top model) | `np_playback.c` (replaces `np_playback_stub.c`), `lib_shuffle.{c,h}`, `shuffle_core.*`, `library.{c,h}` (lib_play/lib_shuffle_all → library source; #49 column), `libfile.h`, `artcache.c`, `artfmt.{c,h}` (NOWPLAYING 142×142 ⚠️), `artkey.{c,h}`, `tests/nowplaying/*`, `tests/artcache/*`. Upstream (`playback.c`, `tagcache.c`, `playlist.c`): only `#ifdef HAVE_IPOD_UI` hooks, listed in the self-review | `artcache.h` (#38 A adds `artcache_commit_seq()`; additions go through the orchestrator), screens, `np_draw`, tooling |
| **T: verification** (tooling, Sonnet) | `tests/ui/ipodui-p5-*.script`, `tests/fixtures/np/*` (generated: lyrics in USLT v2.3/v2.4 and ©lyr, a chaptered m4b with a chapter image track, MP3 CHAP+APIC, ratings, 300-char titles), new `uisimulator/common/sim_cmd_np.c` (`np_pin <ms> <vol%>`), `scripts/check_trace.py` NP rules, compose sheets, `np-review.mp4` | `apps/` |

`apps/SOURCES`: additions only, merged by the orchestrator. The `docs/hardware.md` budget table: the orchestrator, at close, from the agents' reported numbers.

## 6. Behaviour and drawing rules (N, with P where marked)
- **Memory:** one **"np" block reserved at shell start** (`core_alloc`, movable, pinned only inside a draw), ≤ **49,152 B**: art source 142×142×2 = 40,328, digit atlas (Bold 12 `0–9 : −`) ≈ 2,304, glyph tiles ≈ 3,000, `struct np_track` ≈ 420, `np_state` ≈ 120, tick bitmap 26 (204 px). *Can't share because* the strip holds the push-slide capture exactly when NP's first frame needs the art, and the art must survive Lyrics without a re-read. Shareable later with the Cover Flow texture cache (P7: NP and Cover Flow are never both on top).
- **Strip use at rest** (`ui_strip_claim`; `kept == false` means rebuild): the F2 marquee buffer (title rendered **once**, capped at 1,200 px + 40 px gap + window → ≤ 50,112 B), and the lyrics text ≤ 8,192 B plus a line table ≤ 1,024 B. **Nothing calls `core_alloc` after shell start.**
- **Art:** at enter or on an `art_key` change (track, or chapter for books with chapter art), `artcache_get(key, ARTCACHE_NOWPLAYING)` into the block. Then render the slant column by column (a fixed-point vertical resample, top y 34→43, height 142→125) and the reflection once, boosted, straight to the framebuffer. Reflections are never stored (`artcache.md` §2). `PENDING` re-polls on `artcache_commit_seq()`, not on a timer. **Reads only while NP is visible and lit**; otherwise the key is marked stale and read on the next show.
- **Redraw bands (full width):** the bar row (y 196–229) at 1 Hz, on scrub, volume and mode slides; the title band (y 58–76) for marquee steps; the info rows on a rating change; the full body only on enter, a track change and Lyrics in/out.
- **Mode slides:** the old row (captured into the strip) slides **off to the left** while the new row slides in from the right, **190 ms ease-out** ⚠️, boosted for the slide only, ≤ 30 fps, frames dropped not queued. Normal↔Scrubber swaps in place (the same bar; the fill goes, the diamond comes). Lyrics in/out is a body push (in from the right, out to the left). The 4 s timeout plays the same slide as SELECT → Normal.
- **Timers** (one source, `tick()`): lit = 1 Hz aligned to the elapsed second, 20 Hz marquee while the title overflows, 30 fps cap while sliding, scrubbing or scrolling lyrics; dark = `TIMEOUT_BLOCK` except the pending settle or mode timeout. Track changes are seen through `np_pb_seq()` at the next tick or input (≤ 1 s).
- **Fonts:** title Bold 15; artist, album, "n of N", times and shelf **Bold 12** ⚠️ (the spec's "Bold 13" would be a fifth font of ~28 KB against a font budget already at 101,354 B); lyrics Regular 12 ⚠️.
- **Shelf commit (P):** a library source → `lib_shuffle_set_mode()` (re-init from the current entry, replace what's queued after it). An ordinary queue → Songs `playlist_randomise` (RAM), Off `playlist_sort` (RAM), Albums: re-queue from the source view in album-shuffled order (≤ cap file-name reads, background, yielding; a single-album queue = Off order). The setting is `global_settings.playlist_shuffle` plus the albums flag in `npshuf.dat` ⚠️.
- **Playback stops** (end of queue) while NP is shown → pop ⚠️. Multi-file books: per-file bar, "n of N" = queue position, until P6 ⚠️.

## 7. Device-reality checklist (numbers; instruction counts from the ELF/Unicorn harness, 1.2–2.0 CPI)
1. **Compiled features:** `HAVE_IPOD_UI`, `HAVE_TC_RAMCACHE`, `HAVE_METADATA_EXTRAS` (lyrics, chapter art), `AB_MAX_CHAPTERS` 1024, `HAVE_DIRCACHE` compiled in but off. The shell never calls `playback_claim_aa_slot()`, and the stock WPS is no longer reached, so the audio thread's `read_bmp_fd` path (2,592 B frame) is gone in normal use (update the P10 audio-stack item).
2. **Memory:** +≤ 49,152 B permanent ("np" block, budget line) + < 128 B static (shuffle state, flags) + the strip at rest (≤ 59 KB of 176,000). **0 B allocated during playback.** Art bundles on disk: 40,328 + 512 B per album → ~165 MB at 4k albums, ~205 MB at 5k. Chapter art: same per distinct image. Chapter-art bundles build **only while stopped**, from the H1 panel transient (785,844 B, freed after; the audio buffer regrows at the next start) ⚠️. NP bundles ride the normal builder (playback-safe, never boosting).
3. **CPU (budgets; self-reviews report measured counts):** 1 Hz tick (bar delta + 2 labels from the digit atlas) ≤ 15k instr = 0.6–1.0 ms at 30 MHz (< 0.1%). Scrub/volume event ≤ 15k, coalesced to ≤ 30/s → ≤ 2.4% at 30 MHz, unboosted. Marquee step (F2 blit) ≤ 8k at 20 Hz → ≤ 0.9%; was ~193k per step (15–26%). One-time title render ≤ 650k (boosted, 10–16 ms). Art slant + reflection ≤ 350k once per art change (boosted, 5–9 ms). Slide frame ≤ 20k (two strip blits) + push. Lyrics line step ≤ 150k (framebuffer `memmove` + one Regular 12 line). A track change in total ≤ 1.3M boosted (≤ 33 ms, one frame late at most). Chapter lookup per tick: incremental from the last chapter, O(1); ticks O(count) once per track (1,024 → ~20k). Shuffle lookup ≤ 50 µs worst.
4. **Storage, per user action:** entering NP: 1 bundle read (0 if the same key is still in the block). Track or chapter change: 1 read, only while lit and visible. Lyrics: 1 open + ≤ 8 KB on each Lyrics entry. Rating: 0 now; the tagcache command queue flushes on `register_storage_idle_func` (rides the next wake; forced at shutdown). Volume and shelf → settings save, which rides idle ⚠️ (P verifies). Shelf on a library source: ≤ 50 file-name reads + a control-file append; ordinary queue Songs/Off: one control-file write. Refill: ≤ 1 wake per 50 tracks, normally none (waits for `storage_disk_is_active()` unless ≤ 2 tracks are left). Seek: the normal rebuffer.
5. **LCD:** full-width bands only. 1 Hz: 320×24 (~2 ms). Marquee: 320×18 at 20 Hz (~30 ms/s of bus). Slide: 320×34 × 6 frames. Track change: one full body push (~16 ms). Lyrics: body per line step, ≤ 30 fps.
6. **Threads and locks:** everything on the UI thread. The audio thread only sets flags (`lib_shuffle_track_changed`, seq bump). `np_pb_snapshot()` copies `audio_current_track()` and re-checks seq (torn read). The chapter column is playback's fixed carve (not movable) and is re-set by `np_set_track` on every seq change. Refills take the playlist mutex as `lib_play()` does and yield between entries. No art or lyrics reads off the UI thread.
7. **Splashes:** P audits every `playlist_*`/`audio_*` call it adds (`playlist_insert_track` errors, `playlist_randomise`) and logs instead. No strings beyond the three LANG IDs. No "Rockbox".
8. **Persistence:** ratings go to the index and the statistics changelog (they survive a rebuild); a rating still queued at a power cut is lost (≤ 1 per visit, accepted). `npshuf.dat` is written after the control file is synced; at boot a stamp mismatch → a new lap (shuffle.md §6). The NP mode never persists (boot → Normal). USB mid-refill: abort at the next yield, retry after.
9. **Scale:** 50k tracks / 5k albums: shuffle O(1) per track, the Songs filter RAM-only; #49 makes an album start one read (~200 KB column on disk at 50k); 1,024 chapters → 26 B tick bitmap; a 648-chapter book in `tests/fixtures/np`.
10. **Deferred or retried work, per database state (ready / fresh or rebuilding / dirty), one script each:** the rating write (`ipodui-p5-rating-{ready,rebuild,dirty}`), the refill (`ipodui-p5-refill-{ready,rebuild,dirty}`), the art `PENDING` → shown on commit (`ipodui-p5-art-pending`), the art read deferred while dark or with storage asleep (`disk_active`, `ipodui-p5-art-asleep`).
11. **Stock firmware unchanged:** stock `.text` byte-identical to `build/fw/.size-baseline`; every upstream hook under `HAVE_IPOD_UI`.

Device-only items go to `ROADMAP.md` P10: seek latency after the settle, the haptic tick per scrub click, refill latency with the disk asleep and awake, the audio-thread stack high-water mark with NP open, the art build time per album at 142², and settings-save timing.

## 8. Acceptance and review artifacts
- `scripts/gate-run` passes (the full gate once, at the end). Host tests: np_state/np_scrub/shuffle (existing) + shuffle record + chapter keys + lyrics wrap. No new warnings, statics < 1 KB, `cpu_boost` balanced (trace), no `core_alloc` after shell start (trace rule), exactly **one seek per scrub gesture** (trace), no boost at rest.
- **Scripts (T):** `ipodui-p5-{normal,marquee,noart,volume,scrub,rating,shuffle,lyrics,timeout,trackchange,audiobook,ab-snap,ab-chapterart,shuffle-songs,shuffle-albums}` plus the item-10 set. Goldens are **proposed**, then captured with `scripts/golden_capture.sh` by the orchestrator after the owner round.
- **Owner morning sheet:** each mode, music and audiobook, **side by side with the owner's photo** (normal ↔ `np-normal-b`, scrubber ↔ `np-scrubber-b`, rating 0/2/4 ↔ `np-rating-*`, shuffle ↔ `np-shuffle-b`, volume ↔ `np-volume-b`, audiobook no-art ↔ `np-audiobook-noart-chapterticks-b`, audiobook chapter art ↔ `np-audiobook-volume-b`), with lyrics shown alone (no reference). Composed under `out/shots/p5-compare/` (local-only; the photos are never committed).
- **`np-review.mp4`** (`record`, ~45 s, 320×240, pinned clock, battery and elapsed): enter from the main menu → marquee → volume → SELECT through Scrubber (scrub, settle, jump) → Rating (3 stars) → Shuffle (Songs) → Lyrics (scroll) → Normal; a 4 s timeout; a track change mid-Rating dropping to Normal; then an audiobook: ticks, a fast-scrub chapter snap, a chapter-art switch.
- The end review lists the pixel differences that remain against each photo.

## 9. ⚠️ Frozen defaults (owner round confirms; not revised mid-phase)
1. **Art size and position:** the spec's photo metrics (x 10–147, left y 34–175, right y 43–167); cache tier **142×142** (was 140: avoids upscaling the 142 px left edge).
2. **Reflection:** a mirrored ~25 px, from ~35% opacity to 0 over white, computed at load.
3. **Volume bar:** ≈ x 40–280, the same 14 px height and gloss as the progress bar; speakers 16×14 original glyphs; shown **2 s** after the last click (`NP_VOLUME_SHOW_MS`); appears and leaves **in place** (no slide).
4. **Slides:** 190 ms ease-out, old row out left / new row in from the right; Normal↔Scrubber in place; Lyrics as a body push; the timeout slides like SELECT.
5. **Fonts:** Bold 12 for artist, album, counter, times and shelf (spec says Bold 13; a new cut costs ~28 KB over the font budget); Regular 12 for lyrics.
6. **Dots:** near-black (photo; the owner's "grey" overruled by the 2026-10-06 decision). Info-row stars: small, near-black, blank when unrated.
7. **Rating scale:** stars × 2 in `tag_rating` (0–10).
8. **Marquee:** the list marquee's speed and 40 px gap rule, window x 160–315, title render capped at 1,200 px.
9. **Lyrics:** embedded only (NP-1); ≤ 8 KB (cut, logged), Regular 12 dark gray on white under the header, wheel scrolls one line per click.
10. **Podcasts:** the audiobook cycle (NP-2); **no Speed step** (NP-3).
11. **"73 of 648" at 0:00 with −11:47:55:** we follow the spec (N = chapter, the whole-book bar), but the photo's 0:00 at chapter 73 doesn't fit a whole-book clock; the owner should check this on video.
12. **Multi-file books:** per-file bar and queue counter until P6.
13. **Shuffle setting:** `playlist_shuffle` + an albums flag in `npshuf.dat`; Albums on an ordinary queue re-queues from the source view.
14. **Chapter art** builds only while stopped (panel transient); the book cover shows until it is built.
15. **Playback stops** while NP is shown → pop to the previous screen.
16. **Hold SELECT** in NP: no action until P11.
17. **Scrubbing** does not boost; slides, art renders and lyrics steps do.

## Revisions
*(Deltas go to running agents by SendMessage, per `docs/process.md`.)*

## Owner note (2026-10-07, after drafting)
- **⚠️ 11 resolved:** the "73 of 648" photo (0:00 / −11:47:55) was taken before playback had started, so the elapsed time hadn't loaded. Keep the whole-book clock as specified. A brief 0:00 before the position loads matches stock and is acceptable.
- **Non-square art keeps its aspect ratio** (photo 1222, `np-audiobook-volume-b`): the wide "Knight Bus" chapter art is drawn **wider than tall**, slanted in the same perspective. It is fitted to the art box, not cropped or stretched to a square, and is vertically centred on the slant, with the reflection under its real bottom edge. The art cache's Now Playing bundle must store the fitted size and aspect (not force 142×142 square), and the slant/reflection renderer must handle any w×h ≤ the box. Add a non-square fixture (wide and tall) to T's scripts.
- **Same photo, also confirms:** the title line marquee-scrolls with the text clipped at both edges mid-scroll; the line under the artist is the chapter title; the rating row is blank (unrated); the volume bar replaces the progress row and has a faint reflection.
- **Correction (owner, 2026-10-07):** "73 of 648" is the **queue position** (file 73 of 648 files in the Audiobooks list), the same counter as music. It is not a chapter counter. No chapter "N of M" is shown on Now Playing. `np_chapter_at()` is still used for the chapter title, the ticks and snapping. The "multi-file N of M" question is moot.

## Revision 1 (brief review: REVISE, 2026-10-07). This overrides anything above.
**High**
1. **Ratings:** `runtimedb` defaults to **on** under `HAVE_IPOD_UI` (stock 7G keeps statistics), via a guarded `settings_list.c` default.
   - `np_pb_set_rating` writes **`tc_idx - 1`** (tagtree's index is 1-based). With `tc_idx == 0` it logs and drops the write, and it checks that the database generation hasn't changed since the snapshot.
   - The UI keeps a `{tc_idx, stars}` override, because `id3->rating` goes stale after a write.
   - The rating-{ready,rebuild,dirty} scripts check that the value lands on the **right file**.
2. **Refill with the screen dark:**
   - The track-change callback posts an event to the UI queue (`button_queue_post` or similar). The disk-awake refill is triggered by a posted buffering event, with no polling.
   - Never hold the playlist mutex across `tagcache_retrieve` or a yield.
   - The shelf's "replace what's queued after current" re-reads `playlist->index` under the mutex.
   - New script `ipodui-p5-refill-dark`.
3. **Now Playing art bundles:**
   - The decode block grows by **20,304 B** (78,284 → 98,588; a permanent block, so it gets a budget line).
   - A **fit** path for `ARTCACHE_NOWPLAYING` (aspect kept, per the owner photo); amend `artcache.md` §2.
   - The reader uses the bundle's own w×h. The renderer maps the centred fitted rectangle through the box's fixed perspective, with the reflection under the slanted bottom edge. Tall art is centred horizontally (⚠️).
   - Fixtures: 16:9, 2:3 and 4:1.

**Medium**
4. **The chapter table under rewrite:**
   - A seqlock on `seq`: a guarded hook makes it odd before the clear in `buf_read_audiobook` (`playback.c` ~1607) and even after it (~1622). Snapshots retry while it is odd or has changed.
   - `np_pb_seek(ms, seq)` drops a stale seek.
   - Chapter changes don't bump `seq`.
   - Add `np_chapter_step()` (incremental from the last index) so the 1 Hz tick stays within 15k instructions.
5. **Strip:** Now Playing claims it under one owner id, with fixed offsets: marquee 50,112, two row captures of 21,760 each, lyrics 9,216 (≈103 KB ≤ 180,000, with a static assert). Lyrics are read after the body-push capture. #38 must guarantee the drift and panel code never claim the strip while Now Playing is on top (a trace rule).
6. **#49:** TRACKS is an **added section with no `LIBFILE_VERSION` bump**; if it is missing, fall back to the album filter. State how much the precompute work block grows for the TRACKS sort. The `npshuf.dat` stamp adds `sortkey_version` and the ALBUMS ROWS checksum.
7. **Chapter art:**
   - Built on the art builder thread after panels, one transient block at a time, with `audio_status()` re-checked immediately before `core_alloc`.
   - The chapter table (69,640) and the art table (8,192) are carved out of that block. Building stops when playback starts.
   - Chapters that share an image get their own index records pointing at the shared bundle.
   - Cap the chapter records against `ARTIDX_MAX_RECS` (65,536).
8. **Counter wording:** every remaining "chapter N of M" text (the `np_track` comment, §6, §9.11, §9.12, the `np_scrub.h` comment) now says **queue position**.
9. **Ownership:**
   - `artcache_commit_seq()` lands on `nav` as **NAV-H3**, owned by #38 A.
   - PREV before the start of the shuffle window is out of scope for v1 (⚠️, noted in `shuffle.md`); #38's global buttons do the normal PREV.
10. **Albums on an ordinary queue:** reorder the playlist **in RAM** by album group through a guarded `playlist.c` hook, using group ids saved when `lib_play` builds the queue (≤ 4 KB, reserved). One control-file rewrite, no file-name reads.
11. **Art read:** read at once while Now Playing is lit and visible (one wake per track change while watched, counted in the storage tally). Mark it stale while dark.

**Low**
12. Bold 12 is fine (its cap height of 9 is within the spec's 9–10). Fix the spec's wording, and add a cap-height measurement to the owner sheet.
13. Pin the "np" block during `artcache_get` I/O, and allocate it before any resumed playback starts.
14. Measure the lyrics step; it may be about 200k instructions.
15. Compact the playlist control file once it passes 64 KB.

**Budget:** "np" block +49,152 B; art decode block +20,304 B; static RAM < 128 B; strip at rest ≈ 103 KB (shared, fixed offsets); precompute block + the TRACKS sort (to be stated by P); chapter-art transient 785,844 B, stopped only.

## Revision 2 (owner, 2026-10-07): an audiobook's counter shows chapters; a book plays alone
A deliberate deviation from stock, because nobody listens to audiobooks back to back.
- **Queue:** starting an audiobook (from Audiobooks, from a list, or on resume) queues **only that book**: its single file, or its files in order for a multi-file book (one album). The book ends by stopping, not by running into the next book. **Exception:** when started from a user playlist, the queue is the playlist as usual.
- **Counter (the "N of M" row):**
  - a file with chapters → **chapter N of M** (from `np_chapter_at`, kept current by `np_chapter_step`);
  - a multi-file book without chapters → the file position within the book (which equals the queue position, since the queue is the book);
  - a book started from a playlist → chapters if the current file has them, else the queue position.
  - Music is unchanged (queue position).
- **This replaces Revision 1 item 8:** the counter text is "chapter or file within the book", not the queue across all audiobooks. Podcasts keep the queue counter and play episodes back to back as normal (owner confirmed, 2026-10-07).
- **Ownership:** the queue rule is P's (`lib_play` for `LIB_AUDIOBOOKS` / audiobook entries). The counter source is N's (`np_draw`). Scripts (T): `ipodui-p5-book-alone` (the book ends and stops; the counter reads chapters), plus the multi-file variant.

## Contracts (committed, 2026-10-08, branch `np`). This overrides §4 and §5 where they differ.
Committed as real code with host round-trip tests and ONE shared stub, before fan-out. The headers below are **frozen**: a change goes through the orchestrator (a delta by SendMessage to the other agents), never a local edit.

**Files**
| File | What | Owner after fan-out |
|---|---|---|
| `apps/ipodui/np_playback.h` | UI-thread facade: `struct np_track`, `np_pb_seq/snapshot/elapsed_ms/chapter_title/art_key/read_lyrics/seek/volume_step/volume_pct/set_rating/shuffle/set_shuffle/repeat/idle`, `NP_PB_WAKE`, the audio-thread seqlock hooks `np_pb_ab_write_begin/end`, `np_pb_sim_pin` | frozen; P implements |
| `apps/ipodui/np_pbcore.{h,c}` | pure: rating map and write guard, `np_rating_ovr`, the counter source `np_counter_pick()`, the seqlock rule | frozen; P |
| `apps/ipodui/np_playback_stub.c` | **the** shared stub (np_playback + lib_shuffle runtime) | P deletes it when `np_playback.c` + `lib_shuffle.c` land (SOURCES swap) |
| `apps/ipodui/lib_shuffle.h`, `lib_shuffle_rec.c` | the library source API and the 80 B `npshuf.dat` record (pure seal/check/restart) | frozen header; P |
| `apps/ipodui/np_layout.h` | strip offsets (owner `UI_STRIP_OWNER_NP`) and the "np" block layout, static asserts | frozen; N |
| `np_state.{h,c}` | + `np_set_lyrics()` | N |
| `np_scrub.{h,c}` | + `np_chapter_step()` (incremental, sorted tables) | N (P calls it read-only) |
| `artkey.{h,c}` | + `artcache_key_chapter()`; goldens `tests/artcache/chapter_keys.txt` (`gen_keys.py --chapters`) | P |
| `artfmt.{h,c}` | NOWPLAYING box 142×142, `artcache_size_fits()`, `artcache_fit_rect()` | P |
| `artcache.h` | doc lines only (fit read, chapter keys) | orchestrator |
| `libfile.h` | `LIBFILE_TRACKS` (#49) | P |
| `apps/lang/english.lang` | `LANG_NP_N_OF_M`, `LANG_NP_SHUFFLE_SONGS`, `LANG_NP_SHUFFLE_ALBUMS` | orchestrator (merge with #38 B) |
| `apps/SOURCES` | + `np_state.c np_scrub.c np_pbcore.c np_playback_stub.c shuffle_core.c lib_shuffle_rec.c` (HAVE_IPOD_UI block) | orchestrator |
| `tests/nowplaying/test_{pbcore,shuffle_rec,np_layout}.c`, `tests/artcache/test_chapkey.c` | round-trip tests | P (`tests/nowplaying/*`, `tests/artcache/*`); N's new host tests go in a new `tests/npscreen/` with its own `run.sh` |

**Decisions**
- **D1 Snapshot result.** `np_pb_snapshot()` returns `enum np_snap { NONE, OK, BUSY }` (the brief had `bool`). BUSY = the seqlock is odd or moved during the copy: keep the old copy and retry at the next tick (threads are cooperative: never spin). NONE = stopped (paused still snapshots) → pop (⚠️ 9.15).
- **D2 Seqlock.** One counter, `np_pb_seq()`, written by the audio thread only: +2 per track change (`PLAYBACK_EVENT_TRACK_CHANGE`), +1/+1 around `buf_read_audiobook()`'s rewrite (~`playback.c` 1607/1622) **and** around the audio buffer's re-carve of the scratch area (it moves the table). Chapter changes within a track do not bump it. `np_track.scrub.ch` points into playback's table and is used only after `np_pb_seq() == t->seq` with no yield in between.
- **D3 Stale-proof writes.** `np_pb_seek(ms, seq)`, `np_pb_read_lyrics(seq, …)` and `np_pb_chapter_title(seq, …)` drop (logged) when `seq` moved.
- **D4 Ratings.** `np_pb_set_rating(tc_idx, db_gen, stars)`: `np_rating_target()` → WRITE (index `tc_idx - 1`), NO_ENTRY (`tc_idx == 0`) or STALE (`tagcache_ui_generation()` moved since the snapshot). Value `stars × 2`; reading `tag / 2` rounds odd values down (⚠️). N keeps one `struct np_rating_ovr` and shows `np_rating_shown()`.
- **D5 Counter (Revision 2).** `np_counter_pick(is_audiobook, is_podcast, queue_index, queue_count, chapter, chapter_count)`: an audiobook (not a podcast) file with chapters → chapter n of m; everything else → queue position (a book plays alone, so the queue is the book); podcasts always queue; no queue → blank. N draws it from `np_track` + `np_chapter_step()`.
- **D6 `np_track` fields.** `index, count` → `queue_index, queue_count` (0-based index); added `is_podcast` and `db_gen`; `art_key` is the cover; the per-chapter key comes from `np_pb_art_key(t, chapter)` (P: the chapter key if the index holds it, else the cover; one index lookup, lit and visible only). `album` = the chapter title at the snapshot; N refreshes it with `np_pb_chapter_title()` at a chapter change.
- **D7 Refill trigger (Revision 1 item 2).** P's audio/buffering-thread callbacks (`PLAYBACK_EVENT_TRACK_CHANGE`, `BUFFER_EVENT_FILL_DONE`, `BUFFER_EVENT_REBUFFER`) set a flag and post `NP_PB_WAKE` (`MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 0x20)`) once per flag with `button_queue_post()`. `ui_input_get()` hands it to `default_event_handler()`, which ignores it; the loop pass then calls `ui_nowplaying_idle()` → `np_pb_idle()` → `lib_shuffle_idle()`. No polling, no #38 change.
- **D8 `npshuf.dat`.** 80 B, magic "IPSF", version 1, native order, checksum `libfile_sum` (the libfile's word sum, not CRC-32). Stamp = `{commitid, entries, SORTKEY_VERSION, albums_sum}` (the ALBUMS ROWS section's sum; ignored for Songs). The RAM-cache generation is **not** persisted (it changes at every boot); it stays RAM-only for the refill's "library changed" check. `queued` (playlist amount after the refill) must match the resumed playlist, else RESTART (a power cut between the control-file sync and the record). `lib_shuf_rec_check()` → RESUME / RESTART (new lap: `shuf_lap_seed(seed, lap + 1)`, new N, `lib_shuf_rec_restart()`) / BAD (ordinary queue). A record with source NONE carries the shelf's albums flag for ordinary queues (⚠️ 9.13).
- **D9 lib_shuffle API.** As §4 plus `lib_shuffle_mode()` (the shelf setting) and `lib_shuffle_resume()` (boot, after the playlist resume). Refill constants: window 50, low 10, urgent 2, history 100.
- **D10 Now Playing art.** `ARTCACHE_NOWPLAYING` is a 142×142 **box** (`artcache_size_fits()`). The bundle holds the source scaled so its long side fills the box, aspect kept, upscaled if smaller (⚠️), rounded half up: 16:9 → 142×80, 2:3 → 95×142, 4:1 → 142×36. `artbundle_img.w/h` and `bm->width/height` are the fitted size; the buffer still holds the box. `artcache_fit_rect()` centres it both ways (tall art centred horizontally, ⚠️); N maps that rectangle through the slant.
- **D11 Chapter key.** `artcache_key_chapter(book, ch)` = FNV-1a 64 over the book key (8 B LE), 0x1F, "ch", the chapter (4 B LE); 0 → 1; book 0 or ch < 0 → 0. Reference `gen_keys.py --chapters`, goldens `chapter_keys.txt` (checked by `tests/artcache/run.sh`).
- **D12 #49 TRACKS.** `cat LIB_ALBUMS, col LIBFILE_TRACKS (4), elem 4`, count `A + 1 + T`: `uint32 start[A + 1]` then `int32 idx[T]` (each album's music tracks in list order). Added section, no `LIBFILE_VERSION` bump; missing → the album filter. Starting an album is **two** small reads (the 8 B start pair, then the slice), not one: a single read would need the COUNT prefix sums in RAM (20 KB at 5k albums) (⚠️). P states the precompute work-block growth for the TRACKS sort.
- **D13 Strip at rest** (`np_layout.h`, owner `UI_STRIP_OWNER_NP`): marquee 0 (1,392×18, 50,112 B), old row 50,112 and new row 71,872 (320×34, 21,760 B each), lyrics text 93,632 (8,192), line starts 101,824 (512 × uint16, 1,024) → **102,848 B ≤ 180,000** (static assert, also against `UI_STRIP_SIZE` where `anim.h` is included).
- **D14 "np" block** (49,152 B): art 0 (40,328), digit atlas 40,328 (2,304), glyph tiles 42,632 (3,072), `np_track` 45,704 (512, `sizeof` asserted: fits on host and target), `np_state` 46,216 (256), tick bitmap 46,472 (28) → 46,500 used, 2,652 spare.
- **D15 Lyrics state.** `np_set_lyrics(st, lyrics_max)`; `< 0` = the read failed: Lyrics leaves this track's cycle.
- **D16 Strings.** Three `LANG_*` phrases appended to `english.lang` (stock `.text` untouched); "Off" reuses `LANG_OFF`.
- **D17 The shared stub** (`np_playback_stub.c`): snapshot from `audio_current_track()` (no lyrics, no chapters, `is_podcast` false), real seek (`audio_ff_rewind`), real volume (`adjust_volume`, honours the volume limit), real rating write, the shelf remembered only (queue unchanged), lyrics −1, chapter title false, art key = cover; `lib_shuffle_*` = no library source. Its seq is bumped by its own lazily registered track-change handler. 1,025 B text, 5 B bss on the target.

**Upstream files P may touch** (each change `#ifdef HAVE_IPOD_UI`, listed in P's self-review, stock `.text` byte-identical): `apps/settings_list.c` (the `runtimedb` default on, Revision 1 item 1); `apps/playback.c` (the D2 seqlock hooks in `buf_read_audiobook()` and around the scratch re-carve, nothing else); `apps/playlist.c` + its `playlist.h` declaration (the album-group reorder hook, Revision 1 item 10); `apps/tagtree.c` only if the rating path needs it. Nothing else upstream (`tagcache.c` included) without the orchestrator.

**Read lists (≤ 3 each; the contract headers are read as code, not counted)**
- **N:** `docs/design/spec/now-playing.md`; `docs/design/spec/index.md` (*Visual language*, *Transitions*); this brief §6, Revisions 1–2 and this section. Code: `np_playback.h`, `np_pbcore.h`, `np_layout.h`.
- **P:** `docs/design/shuffle.md`; `docs/design/chapter-art.md`; this brief Revisions 1–2 and this section. Code: `np_playback.h`, `lib_shuffle.h`, `libfile.h`, `artcache.h`.
- **T:** this brief §7–§8, Revisions 1–2 (scripts: `ipodui-p5-refill-dark`, `ipodui-p5-book-alone` and its multi-file variant, non-square art fixtures 16:9 / 2:3 / 4:1) and this section; `tests/ui/README.md`; `docs/phase-nav-brief.md` Revision 2 (`record`, `make_video.sh`). Code: `np_playback.h` (`np_pb_sim_pin`).

**Ownership (final; one owner per file)**
| Agent | Owns | Must not edit |
|---|---|---|
| **N** (ui-fidelity, top model) | `screens/nowplaying.{c,h}` (replaces the nav stub), new `np_draw.{c,h}`, `np_lyrics.{c,h}`, `np_state.*`, `np_scrub.*` (contract signatures frozen), `np_layout.h` (offsets frozen), `art/npglyphs.py` → `npglyphs.h`, new `tests/npscreen/*` | the frozen headers, every #38 file (§5), P's and T's files, `apps/SOURCES` (request lines) |
| **P** (playback-audiobook, top model) | `np_playback.c` (new; deletes `np_playback_stub.c`), `lib_shuffle.c` (new), `lib_shuffle_rec.c`, `np_pbcore.c`, `shuffle_core.*`, `library.{c,h}`, `libfile.h`, `artcache.c`, `artfmt.{c,h}`, `artkey.{c,h}`, `tests/nowplaying/*`, `tests/artcache/*`, `docs/design/{shuffle,chapter-art,artcache}.md` amendments; the guarded upstream hooks above | the frozen headers (`np_playback.h`, `np_pbcore.h`, `lib_shuffle.h`, `np_layout.h`), `artcache.h`, screens, `np_draw`, tooling |
| **T** (tooling, Sonnet) | `tests/ui/ipodui-p5-*.script`, `tests/fixtures/np/*`, new `uisimulator/common/sim_cmd_np.c` and its registration in `sim_script.c`, `scripts/check_trace.py` NP rules, `scripts/gate_map.txt` NP entries, compose sheets, `np-review.mp4` | `apps/` |

## Revision 3 (end review, 2026-10-08): fix round, split in two
- **The orchestrator approves** P2's `tagcache.c` edits (no changelog export at shutdown; the background export on the charger), which fall outside P's original upstream list.
- **Contract change (frozen header, orchestrator-approved):** `struct np_track` gains a **track identity** separate from `seq` (e.g. `track_id` = the playback TRACK_CHANGE count or a path CRC). `seq` keeps guarding chapter-table validity only. N changes on a track change; on a seq move within the same track it only re-points `scrub.ch`.
- **Fix A (playback; owner: playback-audiobook):**
  - `np_playback.c`/`.h`, `lib_shuffle.*`, `artcache.c`, `artfmt.*`, `playback.c` hooks, `library.*`.
  - Tracing via `UPDATE_TRACE`, with the gate failing on skipped expected rules (the scripts side goes to T).
  - Findings 4, 5, 7 and 8, known items 2, 3 and 8, and the "np" block reserve.
- **Fix B (screen; owner: ui-fidelity):** `screens/nowplaying.*`, `np_draw.*`, `np_lyrics.*`, `np_layout.h`, `art/npglyphs*`. Findings 1 (screen side), 2 and 6, the Lows, and all the owner visual notes in `spec/now-playing.md` and `docs/design/np-reflection.md`.
- **Then T:** scripts, goldens, the gate.
- **Orchestrator approves** fix A's guarded `playlist_delete_range_ipodui()` in `apps/playlist.c`/`.h` (2026-10-08). It fixes the audio-queue overflow behind the Albums shelf and the history trim. Stock binary unchanged.
- **Fix B ⚠️ deviations accepted (orchestrator, 2026-10-08):**
  - an 8-row quadratic feather plus a 2×2 dither on the art reflection (the 4 linear rows left a visible edge);
  - `NP_ROW_Y` 196 → 198 (row height stays 34);
  - the seek settle stays 250 ms (a 500 ms gap between clicks = 2 seeks, consistent with "jumps once after the wheel stops").
- **Parked for the quick-wins / polish batch:**
  - speaker nits (round 2 landed in da5a86cc01: megaphone cone, dark arcs r 6/10): slightly bolder and softer arcs, and the lighter inner face of the mouth, for polish;
  - the art geometry: the 7G art measures ~x 10–145, left edge y 32–167, right edge 42–162 against our 34–175/43–167. That changes the art box contract and the bundles, so fold it into the pre-rendered art+reflection quick win.
- **Owner approval (2026-10-08 ~22:20):** after the final review video and side-by-sides, the owner said 'Nice all looks good!'. Phase 5 merges to main once the full gate passes; remaining visual nits go to polish (#52).
