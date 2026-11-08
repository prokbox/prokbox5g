# Brief: audiobook classification by folder and genre (tracker #146)

Origin: `docs/research/podbox.md` (its genre-based `db_spoken.c`). Owner: playback-audiobook agent. Status: proposed.

## 1. Rules (automatic, no setting; curated, not customisable)
`ui_classify()` in `apps/tagcache.c` (the only decision point) sets `TAGCACHE_UI_AUDIOBOOK` when, in this order:
1. **Video**: never a book (unchanged, returns first).
2. **Podcast** wins over book: genre Podcast/Podcasts or path contains `/Podcasts/` (unchanged).
3. **Type**: `id3->is_audiobook` (m4b extension, MP4 book flag, already set in `metadata.c`/`mp4.c`).
4. **Genre**, trimmed, case-insensitive, exact: Audiobook, Audiobooks, Audio Book, Audio Books, Book, Books, Spoken Word, Spoken & Audio, Books & Spoken (ID3 "(101)" Speech and "(183)" Audiobook numeric forms are already expanded to names by the parser; confirm in a test). Locale variants: fixed table for nl/de (Hoerbuch, Luisterboek, Gesprochenes Wort, Hoerspiel excluded: dramas can be music-like) since nl/de are 1.0 items; table lives beside the function, one `static const char *const[]`, scanned with `strcasecmp`.
5. **Folder**: path contains `/Audiobooks/` (case-insensitive, any depth, so `/Audiobooks/Author/Book/01.mp3` counts; also `/Audio Books/`). Fixed names, no setting.
Precedence: video > podcast > (type | genre | folder, any one suffices). A music-genre file in `/Audiobooks/` is a book (folder is the owner's explicit act); the reverse (book genre in `/Music/`) is also a book (tag is explicit). Conflict podcast-genre vs `/Audiobooks/` -> podcast.

## 2. Flow through existing code
- **Sync time only** (rule 3): classification runs while the file is open in the tagcache scan; the flag is stored in the entry. Browse, Audiobooks list and counts read the flag; the UI never opens files. `tagcache_ui_resumes()` calls `ui_classify()` from `id3` on the audio thread: path/genre checks are a few `strcasecmp`, no allocation, no I/O; no scratch buffers (rule 4).
- **Audiobooks menu / list**: unchanged; entries with the flag appear. Multi-file grouping by album (artcache book matching, `ui_groupkey`) unchanged.
- **Per-book resume** (`books.dat`, `ab_store`): keyed by the book's files, not by class, so a re-classified book keeps its position. A newly classified MP3 book gains resume (tagcache_ui_resumes) and the whole-book bar; elapsed was already stored per file.
- **Speed**: applies to audiobooks only (changes-from-7g 2.11); follows the flag, so folder/genre books get 0.75/1/1.25 for free.
- **Chapters**: m4b chpl/Nero and mp3+cue come from file content, independent of class; MP3 books in a folder get a cue chapter list only if a `.cue` exists (no change).

## 3. Edge cases
- **Multi-file books in one folder**: each file flagged individually by the same rule; grouped as today. Mixed folder (some files tagged Fiction) is fine under the folder rule (all files flagged); under genre-only rule a partly tagged book would split, so tag rule alone is the weaker signal. Test it.
- **Split across discs** (`/Audiobooks/Book/CD1`, `CD2`): folder rule covers both; genre/disc-number ordering unchanged (disc then track).
- **Podcasts** with book genre or in `/Audiobooks/`: podcast wins only by genre/`/Podcasts/`; a podcast in `/Audiobooks/` without a podcast tag becomes a book (accepted; folder is explicit).
- **Music in an Audiobooks folder**: becomes a book. Documented, not fixable without a setting. Over-match risk for tag "Book"/"Books" (a band album genre): accepted, exact match only.
- Files with no tags: path rule still works (flag set from path alone).
- Case/diacritics: ASCII case-fold only (`strcasecmp`); non-ASCII locale names listed literally.

## 4. The 7G
It classifies by iTunes media kind (Music, Audiobook, Podcast), a library field set on sync; drag-and-drop files have none, and a file with no kind is Music. Differences (add to `docs/design/changes-from-7g.md`): we infer kind from folder and genre because there is no sync app; Apple's m4b/"Audiobooks" kind still maps to ours. Permanent, intentional (mission: drag-and-drop).

## 5. Cost
- RAM: none resident; the flag bit exists. Code: ~300 B (table + helper). No static over 1 KB, no `core_alloc`, no thread stack growth. IRAM none. CPU: ~12 `strcasecmp` per file at scan, already hit per file; `tagcache_ui_resumes` per track start, negligible.
- **Database format**: classification is stored, so existing databases hold stale flags. Bump `SORTKEY_VERSION` 1 -> 2 (`apps/ipodui/sortkey.h`; magic `0x544348a0 + SORTKEY_VERSION`): the existing path rebuilds the database on next boot and restores play counts/resume via the changelog (`tagcache.c` comments near line 216 and 7950); `books.dat` is untouched. Update `tests/ipodui/test_sortkey_version.c` hash. Cost: one full rebuild per unit at first boot of this firmware (device: minutes at 50k tracks, low priority, incremental, existing indicator; the library is read-only unaffected meanwhile). Dependent artefacts (libfile, artcache, shuffle) carry `sortkey_version` and invalidate with it, so a pre-built lib is rebuilt once. Alternative (rejected unless owner objects): flag-only refresh pass without rebuild needs opening every file again, the same I/O as a rebuild, plus new code. Ship the bump together with any other pending sort/format bump in 0.8.0 to avoid two rebuilds.

## 6. Tests (before code)
- Host: extend `tests/dbgen` (or a new `tests/classify` calling `ui_classify` through the tagcache host build) with a table: m4b; MP3 genre each of the names (mixed case, padded); MP3 in `/audiobooks/x/` with genre Fiction; Podcast genre in `/Audiobooks/`; video in `/Audiobooks/`; `/Music/` file genre Rock; "Books" vs "Bookshelf" (no substring match). Confirm tags with `ffprobe`. `tests/ipodui/test_sortkey_version.c` hash update.
- Sim script `tests/ui/ipodui-146-audiobook-classify.script` against a generated library (fixtures: two MP3 books, one with a cue): both appear under Music > Audiobooks, resume position survives a database rebuild, speed menu appears, chapter list for the cue book. Screenshots opened and compared to `docs/reference/7g/`.
- `scripts/test.sh`, `scripts/gate-run --quick --changed`.

## 7. Device-reality, hardware, algorithms
Checklist: compiled features: `HAVE_IPOD_UI` only, stock build untouched (11: stock `.text` identical; no lang strings). Memory: no allocation. CPU/scale: unchanged per file; the one-time rebuild is the cost (item 3, 9). Storage: no extra wake-ups beyond the rebuild. Persistence: changelog restores stats; a power-off mid-rebuild already retries. Existing paths: `metadata_is_audiobook_ext` in `tagcache_fill_tags` (elapsed restore) stays; it remains the m4b-only autoresume and is superseded for others by `tagcache_ui_resumes`. Hardware features: none apply (string matching on CPU at scan). State of the art: plain table lookup; nothing to adopt (`docs/algorithms-survey.md` has no entry).

## 8. Increments and effort
1. Failing classification table test + fixtures (0.5 d). 2. `ui_classify` rules + locale table (0.25 d). 3. `SORTKEY_VERSION` bump + hash + migration script test (0.25 d). 4. Sim script, screenshots, ledger entries (changes-from-7g, changes-from-rockbox) (0.5 d). About 1.5 days; small, fits 0.8.0.

## 9. Risks
Over-match (music in `/Audiobooks/`, "Book" genre) is by design. The rebuild on first boot is the real cost; batch with other bumps. Locale genre list is a guess for non-English tags: list owner-reviewable. Owner decision (default in place): folder names fixed to `/Audiobooks/` and `/Audio Books/`.
