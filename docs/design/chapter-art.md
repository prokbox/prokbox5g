# Per-chapter artwork: parser output and the art cache hook (#39)

Status: the parser half is done (branch `prep-meta`). The art cache integration is **not built yet**. This note describes the hook it should use.

Owner rule (2026-10-06, #39): Now Playing shows the **current chapter's image** and switches at each chapter boundary. Books without chapter images show the book cover. The Resume and chapters list shows titles only, so chapters get **only the Now Playing size** and no thumbnail tier.

## What the parser gives you

`lib/rbcodec/metadata/chapters.h`, built under `HAVE_METADATA_EXTRAS` (the shell build and warble):

```c
struct ab_chapter_art { uint32_t pos; uint32_t size : 24; uint32_t type : 8; };  /* 8 B */
int metadata_read_chapter_art(int fd, const struct mp3entry *id3,
                              const struct ab_chapter *chapters, int count,
                              struct ab_chapter_art *art);
```

- `art[i]` belongs to `chapters[i]`, as `metadata_read_chapters()` returns them for the same file (sorted by start; only `start_ms` is read). The API takes any array of `struct ab_chapter` plus a count, so it works unchanged with the growable chapter table planned for P6, independent of `AB_MAX_CHAPTERS`. Storage is per entry: one 8 B `ab_chapter_art` per chapter in a caller-sized array. `size == 0` means the chapter has no image: fall back to the book cover.
- Only locations are recorded. The parser never reads or decodes an image.
- **MP4/M4B:** the QuickTime chapter image track: a `vide` track listed in the audio track's `tref/chap` (beside the text track), with one JPEG or PNG sample per picture. A chapter gets the image whose sample starts at or before the chapter start (10 ms slack for timescale rounding), so an image track with fewer samples than chapters still maps correctly. The type comes from the sample's magic bytes, not from `stsd`. Nero `chpl` has no artwork.
- **MP3:** an `APIC` sub-frame inside an ID3v2.3/2.4 `CHAP` frame, matched to the chapter by start time. APIC inside an unsynchronised frame or tag is ignored, because its offsets don't map to the file.
- **Cost:** 8 B per chapter in a caller-supplied array, so **5,184 B at 648 chapters** (2,040 B at today's 255 cap). The table is separate from `struct audiobook_chapters`, so playback's chapter buffer (17,348 B) does not grow. The call reads only atom or frame headers, and for MP4 8 magic bytes per distinct image. It uses about 340 B of stack plus the MP4 atom-walk recursion (about 90 B per level, at most 8), with no static state.

## Hook: index time, never playback

1. **Builder (art builder thread, at sync time).** For a track whose `chapter_count > 0`, read the chapter table (68 B per chapter today; only the start times are used) and then call `metadata_read_chapter_art()` (8 B × `chapter_count`). Where those ~19.4 KB live is decided at integration. One candidate is the movable 86,204 B decode block, which is idle between decodes, but the art table must then be copied out before the first decode. Neither may come from a new `core_alloc` while audio plays. For each chapter with `size > 0`, decode the bytes at `pos`/`size` (JPEG or PNG; progressive images go to the Mac-side writer) at the **Now Playing size only**, and store them under the key `artcache_key(book) + chapter index`, a key variant like the probe slots in `artcache.md`. Consecutive chapters that share one image (same `pos`) are stored once and referenced again.
2. **Index record.** Store a per-book flag "has chapter art" so Now Playing knows, without opening the file, whether to look per chapter.
3. **Now Playing (P5/P6).** On the existing chapter-change hook (`ab_chapter_changed()` in `apps/audiobook_core.c`), when the book has chapter art, call `artcache_get(chapter key, NOWPLAYING)`. On a miss (`NOART` or `PENDING`), draw the book cover. Nothing is decoded during playback, and no file is opened beyond the art cache read.

## Open points for the integrating phase

- **The 255-chapter cap** is tracked in its own issue: a growable chapter table replaces the fixed array in P6. Until then, chapters past 255 are dropped (MP4 text tracks are truncated, the ID3 CHAP count and walk stop at 255; Nero `chpl` can't hold more than 255 by its format). The art API needs no change for that table, because it takes `(chapters, count)`. At 648 chapters the art table is 5,184 B, and today's 68 B entries are 44,064 B.
- MP3 files with CHAP frames get `chapter_count` but are **not** marked `is_audiobook`. The classification (M4B brand, the `.m4b` extension, or a genre) is unchanged, so chaptered MP3 podcasts are not reclassified.

## Phase 5 integration: state and design (agent P2, 2026-10-08)

**Built:** the reader half. `np_pb_art_key(t, ch)` returns `artcache_key_chapter(book, ch)` when the index holds an OK record for it (one lookup, lit and visible only), else the cover; Now Playing bundles exist and fit (D10, `artcache.md` §2), so a chapter bundle is read exactly like an album's.

**Built (agent P3, 2026-10-08): the builder half, as designed below**, with these as-built details:
- Only **single-file books** (`LIBFILE_ALB_BOOK`, one track) get a chapter pass: a multi-file book's per-file chapter indices would collide in the chapter keys, so it shows its cover ⚠️. A book whose file has no chapters gets only the done mark (batched in the delta, no block).
- After the art table is read, the records (≤ 1,024 + 1 + 32 × 32 B) are built **over the chapter table** in the same block, sorted and merged in **one index write per book** (with the pending done marks). Grouping (`artchap_plan()`): consecutive chapters with images at one file position share the first one's bundle; a group whose image is a PNG or fails to decode gets no records (cover) ⚠️.
- The cap is checked before any decode (no bundle is written for nothing); at the cap the pass stops and retries that book at the next stopped pass.
- Between images the block stays held (stopped only); the wait gives up at once when playback starts (`chap_wait()`), and the book is redone at the stop.
- The GC's tag is the book key mixed with its mtime (`artchap_book_tag()`), so a book rewritten for another mtime loses its old chapter records at the forced GC after its chapter pass (`chap_stale`), and owner bundles go with them.
- `art_panel_hold` (simulator) holds whichever transient block comes next, panel or chapter.

The design it implements:
- **When:** on the art builder thread, after the panels, only while `audio_status() == 0`, re-checked immediately before each `core_alloc` (no yield between the check and the allocation). One book at a time; stop at the next playback start (keep a `chapters_owed` flag like `panels_owed`). After every `core_free`, `audio_buffer_regrow()`.
- **Transient block (per book):** the chapter table (69,640 B: 1,024 × 68 B) and the art table (8,192 B: 1,024 × 8 B) carved from one `core_alloc` of **77,832 B**; decoding uses the reserved decode block (98,588 B), as album bundles do. Stopped only, so no audio memory is taken.
- **Records** (`struct artcache_rec`, no format change): key = the chapter key; `flags` bit 0 = chapter record; `slot` + `src_hash` = the low and high halves of the **bundle key** (its own key, or, for chapters sharing one image (same `pos`), the first such chapter's key: one bundle, several records); `src_size` + `src_crc` = the book key's halves; `mtime` = the book's `max_mtime`. Chapters without an image get **no** record (PENDING → the cover). The book's own record gets `flags` bit 1 = "chapter pass done for this mtime" (a rewrite of the book's record clears it, so the pass reruns).
- **Reader:** `read_bundle()` follows a chapter record's bundle key (a sharer's record points at the owner's bundle; the header's key is the owner's).
- **Cap:** stop adding chapter records when the index would pass `ARTIDX_MAX_RECS` (65,536) minus the album table's count.
- **GC:** the album table holds only book keys, so after `artidx_gc_mark()` a second sequential pass marks a chapter record live when its book key is live (a sorted array of live book keys that have bit 1, in the decode block: 12,323 books max; past that, keep every chapter record). Dropping a record that owns a bundle removes the file.
- **Host tests to add:** the record packing (bundle key, book key), shared-image records, the cap, and the GC pass, in `tests/artcache/test_artfmt.c`.
