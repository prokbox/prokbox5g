# Phase 4 brief: Music menu on the database

**Goal:** the Music menu becomes real, built from tagcache, at 1 TB scale.
- **Browsing:** Artists → Albums → Songs, Albums, Songs, Genres, Composers, Compilations, Playlists (including On-The-Go), Podcasts and Audiobooks.
- **Shuffle Songs** plays the whole library and replaces the Phase 2 `/Music` folder walk.
- **"Updating Library…"** shows while the database builds.
- **The quick-scroll letter** appears when you spin fast through a long sorted list.

Read first: `CLAUDE.md`, `docs/hardware.md` (tagcache RAM cache notes and the static RAM audit), `docs/design/spec/` (menu trees across main-menu.md, music-lists.md, audiobooks.md, extras.md, settings.md; behaviours in system-screens.md), `docs/static-ram-audit.md` (the tagcache section) and the current `apps/ipodui/`.

**Out of scope:**
- Cover Flow and album art (Phase 7);
- Search: keep it as a stub (Phase 8 polish);
- our own Now Playing: Phase 5. Playing still hands off to the stock WPS.

## Scale rules (non-negotiable)
- **Target library:** 50k tracks and 5k albums. Opening any list must take **under 300 ms** on target-class hardware (in the sim, log the time). Scrolling must never wait on disk.
- **No list is ever materialised as strings.**
  - A view holds an **index array**: one int per row (tagcache seek or idx), about 4 bytes × N, in `core_alloc`, so it is movable. Handles are re-fetched after anything that can move buflib.
  - Labels are fetched **on demand** for visible rows, through a small per-view string cache: about 2× the visible rows, a fixed few KB.
- **Use the tagcache RAM cache** (`HAVE_TC_RAMCACHE`). Turn it on by default in the `HAVE_IPOD_UI` build. The audit's "shrink after load" fix is in scope if small; otherwise file an issue.
- **Sorting matches the 7G:** case-insensitive, leading "The " ignored ("The Beckons" sorts under B), and digits and symbols before A as "#". Sort keys are computed once when the view opens and live in the view's index order. Never re-sort while scrolling.
- **No splash, ever.** Use upstream calls in quiet modes, as the Phase 2 Shuffle Songs fix did.
- **Audiobooks and podcasts are kept apart from music.** They never appear in Artists, Albums, Songs, Genres or Shuffle Songs.
  - **Audiobook:** `is_audiobook` in the metadata, i.e. the M4B brand, ≥2 chapters, or `.m4b`. If that isn't available through tagcache, use genre "Audiobook" or `.m4b`.
  - **Podcast:** genre "Podcast" or a `/Podcasts/` folder.
  - Document whichever rule is used.

## Work split (three agents in parallel, each in its own worktree)

| Agent | Owns | Must not edit |
|---|---|---|
| **A: library** (playback-audiobook) | new `apps/ipodui/library.{c,h}` and `apps/ipodui/sortkey.{c,h}` (pure); tagcache/ramcache tweaks under `#ifdef HAVE_IPOD_UI` | `screens/`, `menu.*`, `list.*` |
| **B: screens** (ui-fidelity) | new `apps/ipodui/screens/browse.c`, `screens/audiobooks.c`, `screens/playlists.c`, the quick-scroll overlay in `apps/ipodui/qscroll.{c,h}`, Music menu wiring in `menu.c` | `library.*`, `sortkey.*` |
| **C: verification** (general-purpose) | `tests/`, `scripts/`, `uisimulator/` | `apps/` |

## Contracts

```c
/* sortkey.h (A; pure C, host-testable by C) */
/* Fills 'key' with a collation key for 'name': lower-cased, accents folded
 * (é->e), leading "the " removed, digits/symbols mapped before 'a'.
 * Keys compare with strcmp. Returns key length. */
int  ui_sortkey(const char *name, char *key, int keylen);
/* Quick-scroll letter for a name: 'A'..'Z', or '#' for digits/symbols. */
char ui_sortletter(const char *name);

/* library.h (A implements, B calls) */
enum lib_cat {
    LIB_ARTISTS, LIB_ALBUMS, LIB_SONGS, LIB_GENRES, LIB_COMPOSERS,
    LIB_COMPILATIONS, LIB_AUDIOBOOKS, LIB_PODCASTS,
};
#define LIB_ANY (-1)
struct lib_filter {             /* ids from lib_id() of a parent view */
    int artist, album, genre, composer;   /* LIB_ANY = unfiltered */
};
enum lib_state { LIB_READY, LIB_UPDATING, LIB_UNAVAILABLE };

enum lib_state lib_state(void);          /* cheap; poll from tick() */
/* Opens a view. Returns a handle > 0, or < 0 on error (logged). The
 * view's memory is core_alloc'd and freed by lib_close(). */
int  lib_open(enum lib_cat cat, const struct lib_filter *f);
void lib_close(int view);
int  lib_count(int view);
/* Label for row idx (copied into buf; the view caches recent rows). */
const char *lib_label(int view, int idx, char *buf, size_t len);
int  lib_id(int view, int idx);          /* id to filter children by */
char lib_letter(int view, int idx);      /* quick-scroll letter */
/* Songs/audiobook/podcast views: replace the playlist with the view's
 * tracks, starting at idx (shuffled if 'shuffle'), and start playback.
 * Quiet: no splash. Returns 0 or < 0. */
int  lib_play(int view, int idx, bool shuffle);
int  lib_shuffle_all(void);              /* all music (not books/podcasts) */
/* Audiobooks/podcasts: listened fraction 0..1000 from the saved resume
 * position, or -1 if unknown/never played. */
int  lib_progress(int view, int idx);
```

## Agent A: library
- **Views:** implement `library.c` over `tagcache_search*`, `tagcache_get_numeric` and the RAM cache.
  - Each view stores its index array and sort-key order.
  - Labels come through a small cache.
  - "Compilations" uses the albumartist "Various Artists" or the compilation flag, whatever tagcache exposes; document the choice.
  - Albums under an artist use album artist when present.
- **Playback:** `lib_play` and `lib_shuffle_all` build the playlist **quietly**. Insert by filename from the view's rows.
  - With a 50k shuffle, start playback as soon as the first track is in, then continue inserting at low priority if that's feasible.
  - If it isn't feasible, measure and document the cost.
  - Remove `ui_shuffle_songs`'s directory walk: Shuffle Songs calls `lib_shuffle_all`.
- **Library state:** `lib_state()` reports whether the database is being built (`tagcache_is_usable`, commit in progress).
  - In the `HAVE_IPOD_UI` build, automatic updates run silently in the background.
  - No stock tagcache splash or prompt may appear. Find and silence each one under `#ifdef HAVE_IPOD_UI`, logging instead.
- **RAM:** turn on the tagcache RAM cache in this build, and measure its RAM on the sim library. If it's small, apply the audit's shrink-after-load fix; otherwise file an issue.

## Agent B: screens
- **A generic browse screen** on `ui_list` plus `lib_*`.
  - Pushing a row opens the child view with the right filter.
  - Under an artist, an "All Albums" row comes first. Under an album or "All Albums", an "All Songs" row comes first.
  - Selecting a song plays the parent songs view from that row. Playback then hands off to the stock WPS via the existing `ui_show_wps`.
- **Music menu** (7G order): Cover Flow (stub), Playlists, Artists, Albums, Compilations, Songs, Genres, Composers, Audiobooks, Search (stub). Podcasts stays in the main menu.
- **Audiobooks:** one row per book, with a small progress hint. A thin bar or "n% played" in `font_small`, styled like the 7G, using the gloss helper if it's a bar. Selecting a book resumes it.
- **Playlists:**
  - `.m3u`/`.m3u8` files under `/Playlists/` plus On-The-Go (the current dynamic playlist), listed without walking the disk on every open: cache the directory listing once per screen.
  - Selecting a playlist plays it. Holding SELECT on a song adds it to On-The-Go, a 7G behaviour; show a brief, non-splash confirmation row.
- **Quick-scroll letter (`qscroll.c`):**
  - When the wheel velocity is high on a list longer than about 50 rows, show a large rounded translucent square in the middle of the list with the current row's `lib_letter`.
  - It's a glossy grey square with a big white letter; take the look from the spec, and mark any guesses ≈.
  - It hides about 0.5 s after the fast scroll stops.
  - It draws into the glide frame, so it adds no extra full updates.
- **Loading feedback:** while `lib_state() != LIB_READY`, the Music menu shows an inline "Updating Library…" row, styled like the 7G. Browsing lists may be empty during an update.
- **Empty lists:** show nothing, like the 7G. Never show an error splash.

## Agent C: verification
- **A larger generated library,** `tests/ui/setup_library.sh`. Keep the mini library (`setup_music.sh`) working.
  - **Size:** about 300 short, low-bitrate tracks with real tags: 40+ artists, 60 albums, several genres and composers.
  - **Sorting edge cases:** "The …" artists, accented names, numeric and symbol names ("!!!", "10dd"), and one compilation album (various artists).
  - **Playlists:** a few `.m3u` playlists in `/Playlists`.
  - **Non-music:** two podcasts (genre Podcast, under `/Podcasts`) and three audiobooks (`.m4b`), one with a saved resume position if one can be seeded.
  - **Speed:** generate it fast (ffmpeg with very short tracks, at most 5 s each), make it idempotent, and keep it under `build/` only.
- **Making the sim build the database deterministically:**
  - Find how to trigger a tagcache build or commit in the sim: a setting in `config.cfg` (`tagcache_autoupdate`, `tagcache_ram`) or an initial build on first boot.
  - Add a sim-script command, `wait_db [timeout_ms]`, that blocks until tagcache is usable, with `sim_script.c` reading the state through a small sim-only hook. Document it.
- **Host tests:** in `tests/ipodui/`, test `sortkey.c`:
  - "The Beckons" sorts under b;
  - accents fold;
  - "10dd" and "!!!" map to '#';
  - ordering is stable;
  - empty and NULL input are handled.
  
  Use a reference stub until A's file lands, as in Phase 3.
- **UI scripts:**
  - `ipodui-p4-browse` covers Artists → artist → All Albums → album → song plays → WPS → back.
  - `ipodui-p4-sort` scrolls the Artists list ends to check "#" first and "The" placement.
  - `ipodui-p4-qscroll` takes a fast spin through Songs and catches the letter overlay; mark it `# golden: skip` where timing-sensitive.
  - `ipodui-p4-audiobooks` and `ipodui-p4-playlists` cover those screens.
  - `ipodui-p4-update` captures the "Updating Library…" state right after the first boot of a fresh library.
- **Performance trace:** A logs `lib open <cat> n=<count> ms=<t>` and `lib fetch n=<rows>` per frame. Add `check_trace.py` rules:
  - `ms` ≤ 300 in the sim (a sanity check);
  - per-frame label fetches ≤ visible rows + 2 during glides, which proves only visible rows are fetched.

## Acceptance (orchestrator)
1. `scripts/validate.sh` passes, including the new host tests, scripts and trace rules. Goldens for at-rest shots are added after a visual review.
2. The budget-reviewer passes:
   - view RAM is about 4 B per row plus a small cache;
   - no string materialisation;
   - no splash;
   - quiet playlist building;
   - statics stay within 4 KB total;
   - the RAM cache size is measured and recorded.
3. Audiobooks and podcasts never show up in music lists or in Shuffle Songs (checked by script).
4. Side-by-side review of the Music, Artists and Songs lists against the references and photos in `docs/reference/7g/`.
