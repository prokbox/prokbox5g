# Brief: Audiobooks Sort By (Title | Author) (#59, revised)

## Revision 2 (2026-10-10, owner): the scope that ships
This section replaces the rest of the brief wherever they differ. Series grouping is dropped (owner); everything below the rule is the original series brief, kept only as notes for a possible later return.

**Scope**
- Settings › Audiobooks becomes a short split-screen submenu (the `datetime.c` pattern, `PANELART_BOOK` tile on both rows): **Playback Speed** (today's prompt, unchanged) and **Sort By**, a blue inline value cycled by SELECT: **Title | Author**, default **Title**. The top-level Audiobooks caption stays the speed caption ("Normal Playback").
- Title: books by title, then author (today's order, unchanged). Author: by author, then title. Author = album artist, else artist (what the book row's second line already shows; untagged sorts as "Unknown Artist", as in Artists). Collation is the music lists' `ui_sortkey()`, so books collate exactly like Albums and Artists (a leading "The" is ignored, "A"/"An" are not: "The Lighthouse Keeper" files under L, "A Field Guide to Tones" under A).
- The list keeps its 2-line rows with covers. In Author order the quick-scroll letter is the author's.
- Out of scope: series, Search (still merges books by title from the title-order ROWS), the book page, the stock firmware.

**Data (precompute, tagcache thread, no allocation, no rebuild)**
- The existing `LIB_AUDIOBOOKS` ROWS, COUNT, KEY, MTIME, TRACKS stay byte-for-byte in title order (Search, `lib_book_tracks()`, the art builder and the album table read them).
- **Added** sections, `cat = LIBFILE_CAT_BOOKS_AUTHOR (0x7c)`: ROWS (int32 reps), COUNT (uint32), KEY (2 x uint32): the same books, in author order. Both orders are stored, so switching Sort By reads a different slice, never sorts and never rebuilds. 27 of 32 section slots are used today; this makes 30.
- `pre_books_author()` runs after the categories: reads back the title-order ROWS (one read), sorts integer records (8-byte author key prefix, full author compare only on a prefix tie, with a same-seek fast path; title row breaks ties), then writes ROWS and re-reads/permutes COUNT and KEY one at a time. Peak 16 B/book inside the existing 20 B/entry block. `LIBFILE_VERSION` stays 1. `pre_current()` redoes a file that has audiobook ROWS but no author ROWS, once (the upgrade costs one ordinary precompute).
- Reader: `lib_open(LIB_AUDIOBOOKS, NULL)` in Author order reads the 0x7c sections instead (same 3 reads as today). A file without them falls back to title order from the file (not marked damaged), traced `lib books order=fallback`; a scan (no file) is title order, as today. Never sorted by author at browse time.

**Contracts**
- `libfile.h`: `LIBFILE_CAT_BOOKS_AUTHOR`.
- `library.h`: `enum lib_book_order { LIB_BOOKS_BY_TITLE, LIB_BOOKS_BY_AUTHOR }`, `void lib_set_book_order(int order)` (UI thread; changes `lib_generation()` when the order changes, so a stacked list reopens and keeps its selection through `lib_find()`).
- Setting: `int ipodui_ab_sort` (0 Title, 1 Author), "ipodui audiobook sort", `settings.h`/`settings_list.c` under `HAVE_IPOD_UI`, hidden from stock menus, saved by `settings_save()`, part of Reset Settings. The shell calls `lib_set_book_order()` at start (`ui_settings_init()`), on change and after a reset.
- Strings (appended to `english.lang`): `LANG_IPODUI_AB_PLAYBACK_SPEED` "Playback Speed", `LANG_IPODUI_AB_SORT_TITLE` "Title", `LANG_IPODUI_AB_SORT_AUTHOR` "Author", `LANG_IPODUI_CAP_AB_SORT_TITLE` "Sort Books By Title", `LANG_IPODUI_CAP_AB_SORT_AUTHOR` "Sort Books By Author"; "Sort By" reuses `LANG_IPODUI_SORT_BY`.
- Trace: `lib books order=title|author|fallback n=%d` on every unfiltered books open.

**Files:** `apps/ipodui/{libfile.h,library.h,library.c}`, `apps/ipodui/screens/{settings.c,reset.c}`, `apps/settings.h`, `apps/settings_list.c` (guarded lines), `apps/lang/english.lang` (appended), `tests/ipodui/test_library.c`, `tests/fixtures/ab/make_ab_fixtures.py` (a `sort/` set), `tests/ui/ipodui-ab-sort.script`.

**Tests**
- Host (`tests/ipodui/test_library.c`): both orders from the file, author ties by title, artist fallback, counts and keys follow their rows, author letters, fallback to title with no file.
- UI: `ipodui-ab-sort`: the submenu, the Sort By value cycling, the book list in Title and in Author order (fixture `sort/`: books whose title order and author order differ, one author with two books, one book with an artist but no album artist), and the setting surviving a reboot. The same script checks that Playback Speed still opens the unchanged prompt.

---

## Original brief: sort and series (notes only)

*Owner decision, 2026-10-10: **series grouping is dropped**, because the owner's books carry no series tags and the standard series fields (©mvn, SERIES, MVNM) aren't parsed today. Scope is now **Sort By (Author | Title)** only; Settings › Audiobooks holds Playback Speed and Sort By. The series sections below stay as notes for a possible later return. Not yet brief-reviewed; the remaining defaults in §9 still need the owner's OK.*

Small phase, scheduled after #58 and alongside Phase 9. Branch `ab-series` off `main` (85afd4d4ec). Lifecycle per `docs/process.md`: brief → brief review → contracts commit → fan-out → one gate → end review → one owner round. Open facts are frozen as ⚠️ defaults (§9).

## 1. Goal
- **Settings › Audiobooks** becomes a short list: **Playback Speed** (today's prompt), **Sort By** (Author | Title), **Group Books By** (Individual | Series).
- **Series mode:** each series becomes one row: series name, then the author (or Various Authors), with the cover of the series' first book. Selecting it lists the series' books in series order. Books with no series stay as their own rows.
- **Top-level order:** Author, then Title; or Title, then Author.
- Every order and series group is **precomputed at commit time, never at browse time**. **No library rebuild.**

**Out of scope:** searching by series name (⚠️8); series inside Search results; editing series tags on the device; Cover Flow.

## 2. Where series metadata comes from

| Field | Sources today | In tagcache? |
|---|---|---|
| **Series name** | MP4 `©grp` (`mp4.c` `MP4_cgrp`), ID3 `TIT1`/`TT1` (`id3tags.c` taglist), Vorbis/APE `GROUPING`/`CONTENTGROUP`/`CONTENT GROUP` (`metadata_common.c`) → `mp3entry.grouping` | **Yes.** `tag_grouping` is a sorted, de-duplicated string tag and is kept in RAM (`tagcache_ui_str(tag_grouping, …)` works). **Catch:** when a file has no grouping, `add_tagcache()` stores the **title** there instead (`tagcache.c` ~2604–2610). |
| Series name, other sources | MP4 `©mvn`, `----:com.apple.iTunes:SERIES`; ID3 `MVNM`, `TXXX:SERIES` | **Not parsed** (`parseuser()` knows only ALBUM ARTIST and ReplayGain; the MP4 `----` handler knows composer, iTunSMPB, MusicBrainz and album artist) |
| **Series number** | MP4 `©mvi`, `----:…:SERIES-PART`; ID3 `MVIN` ("3/7"), `TXXX:SERIES-PART` | **Not parsed, and not stored anywhere**: there is no spare numeric tag |

**Name rule (pure, host-tested):** a book has a series when the grouping string of its representative file (`rep`, the first file in folder order) exists and differs from that file's title (`strcasecmp`). Series identity is the name alone (sort-key equal, case variants merged), so one series can have several authors. A book whose real grouping equals its first file's title counts as "no series" (traced).

**Priority, at scan time and for audiobook files only** (`TAGCACHE_UI_AUDIOBOOK`), guarded by `HAVE_IPOD_UI`:
- **Name:** `©grp`/`TIT1`/`GROUPING`, then `©mvn`/`MVNM`, then `SERIES` (MP4 freeform or TXXX), then upstream's title fallback. The `©mvn` fallback is limited to books because classical music uses ©mvn for movement names.
- **Number:** `©mvi`/`MVIN`, then `SERIES-PART`. Read the integer part only ("2.5" → 2; ties then fall to track, then title). Range 1–126; 127 means ≥127; 0 means none.
- The parsers fill two new `mp3entry` fields under `HAVE_METADATA_EXTRAS`: `series` (char *) and `series_part` (int16). Stock is untouched; checklist 11 cmp.

**Where the number lives: shell bits in the index flag word.** Upstream uses `0x001F`, the shell uses `0x00E0`, and the upper 16 bits hold RAM-only attributes (`tagcache.c` ~1243 keeps only the low 16 on disk). Bits `0x0100–0x8000` are free:
- `TAGCACHE_UI_SERIES_READ 0x0100`: scanned by a series-aware build.
- `TAGCACHE_UI_SERIES_NO(f) ((f >> 9) & 0x7f)`.
- Extend the `tagcache_ui_flag_check` static assert to cover them.
- Cost: no format change, **no TAGCACHE_MAGIC or SORTKEY_VERSION bump**, 0 B of RAM, and the precompute reads it straight from RAM.

**Rebuild warning and the cheapest path.** Bumping TAGCACHE_MAGIC would force a full rebuild: about **15–45 min at 50k tracks** (hardware.md), which the owner wants to avoid. Instead:
1. **Names via ©grp/TIT1/GROUPING work at once** for every book already on the device.
2. **Numbers for existing books: a one-time in-place catch-up**, not a rescan.
   - The art builder's stopped-only pass gets a new predicate: *every `LIBFILE_ALB_BOOK` row whose rep entry lacks `SERIES_READ`*. That covers coverless and multi-file books, and is independent of `books.pak` state (checklist 13).
   - For each, it reads that **one file's tags** (`get_metadata`, no chapters, no art).
   - It posts `tagcache_ui_set_series(idx, no)` to the tagcache queue. The tagcache thread writes the flag word in place (the same index-write path as numeric updates, **without** `FLAG_DIRTYNUM`) and updates the RAM copy.
   - Master indices don't change and no dead entries are created. The pass is resumable: the bit is its progress marker.
   - Cost at 5k books: about 5k opens × 5–20 ms ≈ **0.5–2 min**, stopped only, background.
3. **©mvn-only names for existing books** show up only when the file is re-scanned (re-copied or re-tagged). Changing a string tag needs a re-add (dead entries) or a rebuild. Owner Q1 decides whether that matters.

**Considered and rejected: series fields in books.idx/books.pak at book-pass time.**
- Names need strings the precompute must sort and the list must draw. Storing them would grow the 24 B records about 3×.
- The list order would wait for a stopped-only pass that never runs for someone who always has audio playing.
- It needs a books.idx → precompute → artcache trigger loop.

The flag-word path keeps one source of truth in RAM; the art builder is used only for the one-time catch-up.

## 3. Precompute design (tagcache thread, inside the existing reserved block, no allocation)
**Invariant:** the existing `LIB_AUDIOBOOKS` ROWS, KEY, COUNT, MTIME and TRACKS sections stay **byte-for-byte in title order**:
- Search merges book rows by sort key (`lib_search_begin`, `srch_cat`).
- `lib_book_tracks()` searches ROWS.
- The art builder reads the album table.

New data is **added** (LIBFILE_VERSION stays 1, as with #49 and #58). `pre_current()` redoes a file that lacks it, so the upgrade costs one ordinary precompute and no rebuild.

`pre_books_order()` runs after `pre_cat(LIB_AUDIOBOOKS)`, on its r book rows (read back from ROWS, r × 4 B):
1. **Author rank:** sort the books by `eff_artist` sort-key prefix (ties: full `cmp_labels`), then walk once to give each distinct author a rank. Every later sort uses **integer keys** (rank, title row), so there are no string ties. A book's title rank is its row index.
2. **Series groups:** books that have a series, sorted by (series name key, series number with 0 last, rep disc/track, title row). Neighbours with an equal name form one group. Groups with **< 2 books** stay individual (⚠️1). A group's author is the common author, or `VARIOUS` when any member's author compares different.
3. **Top lists for Series mode:** individual books plus series entries, in two orders:
   - Title order: name key, then author.
   - Author order: author rank, with `VARIOUS` sorting by the literal `lib_various_authors[] = "Various Authors"`, the same pattern as `lib_unknown_artist`; ties by name.

**Sections:** 27 of the 32 `LIBFILE_MAX_SEC` slots are used today; this adds **2**, leaving 3 spare. Add a static assert on the count.
- `(LIB_AUDIOBOOKS, LIBFILE_ORDERS = 5)`, elem 4: `r, t; by_author[r]; top_title[t]; top_author[t]`. A top entry is a book row `b` or `0x80000000 | s`.
- `(LIBFILE_CAT_SERIES = 0x7c, col 0)`, elem 4: `S; {name_rep, first_book_row, count | flags << 16}[S]; start[S+1]; members[M]` (book rows in series order).
- Header `reserved` becomes `series_stamp`: the number of audiobook entries with `SERIES_READ`. `pre_current()` recounts it with one flag walk (about 50k × 10 instructions ≈ 6 ms on the device) and redoes the file on a mismatch, so a power cut between the catch-up and its precompute can't leave a stale order.
- At the end of the catch-up pass (or when it stops), `tagcache_ui_precompute_request()` is called once if anything was written.

**Library API (contract):**
- New `LIB_SERIES` appended to `enum lib_cat`: a series' books, with `f.album` = the series row's `lib_id()`. The view has class `C_BOOK`, group `G_ALBUM`, rows = book reps, with KEY and COUNT gathered. `ab_page_open()` works on it unchanged, because it checks `cls`/`group` and uses `rows[idx]`.
- A series row in the top list is `LIB_SER_ROW 0x20000000 | name_rep`. That id is stable across precomputes, like book ids. Static-assert that entries < 2^29. Finding a series by id is a linear search over ≤ ~1k table entries on SELECT, which is not a sort.
- `lib_open(LIB_AUDIOBOOKS, NULL)` reads `global_settings.ipodui_ab_sort` and `ipodui_ab_group`. It loads ROWS, KEY and COUNT as today, plus the one slice it needs, then gathers in O(n). `lib_book_order_changed()` bumps the library generation, so a stacked list reopens and keeps its selection through `lib_find()`.
- On a series row: `lib_label` gives the name; `lib_sublabel` gives the author or `lib_various_authors`; `lib_art_key` gives the first member's key (owner: first book's cover); `lib_child_count` gives the member count; `lib_row_tracks` gives all members' files in series order, capped at 2,000 (for the Hold menu).
- **`lib_letter()`** follows the sort: in Author mode it gives the author's letter. Today it uses the row's title label, which would be wrong.
- **Fallback:** with no current file (or no new sections), the list shows Individual/Title from the scan, as today, traced `lib books order=fallback`. It never sorts by author at browse time, and reopens when the file arrives.

## 4. Settings and screens
**Settings:** `settings.h` and `settings_list.c` under `#ifdef HAVE_IPOD_UI`, hidden from stock menus:
- `INT_SETTING ipodui_ab_sort` 0 Title / 1 Author, default **Title** (⚠️2).
- `INT_SETTING ipodui_ab_group` 0 Individual / 1 Series, default **Individual** (⚠️2).

**Settings › Audiobooks** becomes a split-screen submenu, following the `datetime.c` pattern (`ui_menu_screen` over a 3-item `ui_menu`, value rows cycled by SELECT, `PANELART_BOOK` tile on each row):
- **Playback Speed** opens `speed_screen` unchanged.
- **Sort By** and **Group By** are inline blue values, cycled by SELECT, saved with `settings_save()` plus `lib_book_order_changed()`.
- The top-level Audiobooks caption stays the speed caption ("Normal Playback", stock photo).

**Strings:** `LANG_IPODUI_*`, appended at the end of `english.lang`. Full sentences in our own words.

| id | English |
|---|---|
| `LANG_IPODUI_AB_PLAYBACK_SPEED` | Playback Speed |
| *(reuse)* `LANG_IPODUI_SORT_BY` | Sort By |
| `LANG_IPODUI_AB_GROUP_BY` | Group Books By |
| `LANG_IPODUI_AB_AUTHOR` / `_AB_TITLE` | Author / Title |
| `LANG_IPODUI_AB_INDIVIDUAL` / `_AB_SERIES` | Individual / Series |
| `LANG_IPODUI_VARIOUS_AUTHORS` | Various Authors |
| `LANG_IPODUI_CAP_AB_SORT_AUTHOR` / `_TITLE` | List Books By Author / List Books By Title |
| `LANG_IPODUI_CAP_AB_GROUP_SERIES` / `_INDIVIDUAL` | One Row For Each Series / One Row For Each Book |
| `LANG_IPODUI_ONE_BOOK` / `_N_BOOKS` | 1 Book / %d Books *(only if Q3 is yes)* |

**Screens:**
- `browse.c` gets an `is_books(b)` helper: unfiltered `LIB_AUDIOBOOKS` or `LIB_SERIES`, both with the two-line cover style.
- SELECT on a series row pushes `ui_browse_screen` with `LIB_SERIES`, header = series name. Its rows are ordinary book rows that open the book page.
- `shown_label`/`browse_sublabel` map `lib_various_authors` to the LANG string.
- Series rows have a chevron and no thumbnail fallback change (book glyph).
- The Hold menu on a series row shows the group menu.

## 5. Existing paths affected (checklist 12)
- **Book page:** unchanged. Opened from a `LIB_SERIES` view, the same `ab_page_*` calls apply. `ab_page_open()` must return −1 for a series row (browse never sends one). MENU from the page goes back to the series list.
- **Multi-file books:** series and number come from the rep file only. TRACKS, the book map and `books.pak` are untouched.
- **Drift book panel set (#58):** chosen by play recency and newness, keyed by book key. Unaffected.
- **Search:** reads base ROWS in title order, which is preserved. A book found by search opens its own page; series rows never appear (⚠️8).
- **Resume and `books.dat`:** keyed by book key plus path CRC. The catch-up writes flags in place, so master indices, the playing queue, boot resume and `lib_queue_is_book` don't move. Scan-time re-adds behave as today, and statistics are carried over by the existing `FLAG_RESURRECTED` matching (verify in a test).
- **Unused progress pass (found while reading):** `has_progress(LIB_AUDIOBOOKS)` still runs `progress_compute()`, two passes over the database, on every book-list open, but nothing outside `library.c` uses it (Rev 2 removed the dots). Remove it for books in this phase; that is about 0.2 s saved per open on the device, measured before and after. Podcasts keep it.
- **Upstream paths doing the same job:** nothing upstream groups series. tagtree's grouping views are unreachable from the shell. The art builder's book pass and the new catch-up predicate are separate predicates in one pass, not two builders.

## 6. Device-reality checklist (numbers at 5k books, 50k tracks)
1. **Compiled features:** parsers under `HAVE_METADATA_EXTRAS`; everything else under `HAVE_IPOD_UI`. Same code in the sim and on the device. No `HAVE_DIRCACHE` dependency: the catch-up opens files by path from `tag_filename`.
2. **Memory:** 0 B permanent.
   - The precompute uses the reserved 20 B/entry block: a 50k block is ~1 MB, and the book work needs ≤ 5k × 12 B recs + 5k × 8 B ranks ≈ 100 KB inside its spare 8 B/entry.
   - `lib_open` gather scratch ≤ 20 KB inside the view pool's 64 KB headroom; the pool is never grown while audio runs.
   - Catch-up: the builder's existing `mp3entry` and stack, while stopped.
   - Statics < 64 B.
3. **CPU:**
   - Precompute book work: 4 integer-key heap sorts of ≤ 5k items (~65k compares each) plus one ranking pass with string compares. Device estimate ~0.3–1 s unboosted, added to today's ~5 s, yielding through `pre_step`.
   - `lib_open` gather: O(n) ≈ 0.1M instructions ≈ 1.3 ms.
   - `pre_current` stamp walk: ≈ 6 ms.
   - Measure all of these with the new perf fixture (§7).
4. **Storage per action:**
   - Opening the list: ROWS 20 KB + KEY 40 KB + COUNT 20 KB + one order slice ≤ 20 KB, in 4–5 `read()` calls (lean rule 11, bulk).
   - Opening a series: the series section, read once (≤ ~40 KB).
   - Catch-up: one tag read per book, once ever, while stopped.
   - The database file grows by about 70 KB (426 KB today at 50k).
5. **LCD:** standard list and split-screen menu. No new animation.
6. **Threads:** the flag write runs only on the tagcache thread (queued command). The art builder only posts. The precompute holds no RAM pointer across a yield. The UI thread reads only the file.
7. **Splashes:** none. Bad sections fall back to the scan, logged.
8. **Persistence:** settings are saved on change. A power cut mid-catch-up loses nothing: the bit marks progress and `series_stamp` forces the redo. Through a rebuild the flags are recomputed at scan. USB connect stops the pass, which resumes on the next stopped pass.
9. **Scale:** extend `tests/perf/lib50k.sh` with `--books 5000 --series 600` (series of 2–40 books, 5% Various Authors, 10% multi-file, 20% unnumbered). Report precompute and `lib_open` sim times plus device estimates for all 4 modes.
10. **Deferred work:** the catch-up waits for stop. One script each for *ready*, *fresh/rebuilding* (the pass must not run; flags come from the scan), and *dirty*.
11. **Stock unchanged:** `.text` cmp-identical with `english.lang` held equal. All upstream edits are guarded.
12. See §5.
13. **Catch-up predicate:** every `LIBFILE_ALB_BOOK` rep without `SERIES_READ`, whatever its art, chapter-done or `books.pak` state.

## 7. Contracts, tests, acceptance
**Contracts** (committed before fan-out, with a host round-trip test and one shared stub):
- `libfile.h`: `LIBFILE_ORDERS`, `LIBFILE_CAT_SERIES` layout, `series_stamp`.
- `library.h`: `LIB_SERIES`, `LIB_SER_ROW`, `lib_various_authors`, `lib_book_order_changed()`.
- `tagcache.h`: `TAGCACHE_UI_SERIES_*`, `tagcache_ui_set_series()`.
- `metadata.h`: `series` / `series_part`.
- Pure `ab_series_core.c`: name rule, number parse ("3", "3/7", "2.5", "IV"→0), grouping and ordering over arrays.
- Trace lines: `lib books order=file|fallback mode=%d%d n=%d`, `ab series catchup n=%d done=%d`, `tc series set idx=%d no=%d`.

**Host tests:**
- `test_series.c`: name rule including the title-fallback case; Various Authors; single-book series stays individual; order number → track → title; integer keys equal full-string order.
- Parser goldens: an m4b with ©grp + ©mvi, an MP3 with TIT1 + MVIN, an MP3 with TXXX:SERIES/SERIES-PART, and an MP3 with ©mvn only, as music, which must **not** be grouped.

**UI scripts:**
- `ipodui-ab-settings`: the submenu, values cycle, captions.
- `-sort-author` and `-sort-title`: letters under quick scroll.
- `-series`: series row, Various Authors, series list order, book page from it, back.
- `-series-fallback`: no file → Individual/Title.
- `-catchup-{ready,fresh,dirty}`.
- `-search-book`: Search unchanged.
- `-resume-in-series`: a saved spot survives a mode switch.
- Plus goldens.

**Acceptance:**
- Gate passes; no rebuild trace (`tagcache: rebuild`) on upgrading a P6 database.
- The book list reads ≤ 5 `read()` calls in every mode.
- 0 `core_alloc` while audio runs.
- P6 and P11 goldens unchanged.

## 8. Ownership and size
| Agent | Owns |
|---|---|
| **D** (top model): metadata + database | `mp4.c`, `id3tags.c`, `metadata_common.c` (`series` fields), `metadata.h` delta, `tagcache.{c,h}` guarded lines (scan-time chain, flag bits, `set_series` command), parser goldens |
| **L** (top model): library + builder | `library.{c,h}`, `libfile.h`, new `ab_series_core.c`, `artcache.c` catch-up predicate, drop the book progress pass, `settings.h`/`settings_list.c` guarded lines, `test_series.c`, perf fixture |
| **U** (Sonnet): screens + tooling | `screens/settings.c` (submenu), `screens/browse.c` (`is_books`, series rows), `english.lang` (appended), `tests/ui/ipodui-ab-*.script`, `gate_map.txt`, `check_trace.py` rules |

**Size:** about 1,100–1,500 lines including tests. Roughly 1 day per agent in parallel after the contracts commit; one gate; small risk overall. The highest-risk piece is D's in-place flag write: it gets its own self-review against checklist items 6 and 10.

## 9. Owner pre-flight (⚠️ default applies until answered)
- **Q1. How are your books' series tagged?** Please send `ffprobe` output for one book.
  - ⚠️ Default: ©grp/TIT1 for names (works at once), ©mvi/MVIN/SERIES-PART for numbers (catch-up).
  - If your names are only in ©mvn or TXXX:SERIES, existing books need either a re-copy or a one-time books-only re-add (it leaves about 5–10k dead index entries, ~0.5 MB of RAM cache, until the next rebuild).
  - Also tell us if your grouping field holds something other than a series (genre-like text would merge unrelated books).
- **Q2. Defaults.** ⚠️ Sort By **Title**, Group By **Individual** (stock 7G and today's behaviour).
- **Q3. Book count on a series row?** ⚠️ No: the second line shows the author, like book rows.
- **Q4. "Author" means** ⚠️ the album artist, else the artist (today's book second line). Composer/narrator is not used.
- **Q5. A series with only one book in the library:** ⚠️ it stays a plain book row (⚠️1); a series needs ≥ 2 books.
- **Q6. Various Authors sorting** in Author mode: ⚠️ under "V", like Unknown Artist sorts under "U".
- ⚠️8: series names are not searchable (follow-up issue).
