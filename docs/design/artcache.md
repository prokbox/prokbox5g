# Album art cache (`.rockbox/artcache/`)

**Status:** design, for Phase 7 (Cover Flow). The list thumbnails (Phase 4) and Now Playing (Phase 5) also read from it.
**Rule it implements:** "Pre-compute at sync time, never at browse time" (`docs/hardware.md`). Browsing never decodes a JPEG.

## 1. What exists today, and why it is not enough

| Piece | What it does | What it means for us |
|---|---|---|
| `apps/recorder/albumart.c` `search_albumart_files()` / `find_albumart()` | Walks a list of candidate files: `<track>[.WxH].{bmp,jpg}`, `<album>.*`, `cover.*` and `folder.jpg` in the track's directory; then `.rockbox/albumart/<artist>-<album>.*`; then the same names in the parent directory. It tries `.WxH`-sized names first. | It finds sources but caches nothing. Every lookup is several failed `open()`s. We reuse its search order to pick the **source**. |
| `apps/playback.c` `audio_load_albumart()` | Claims AA slots (`playback_claim_aa_slot(dim)`). For each track it does `bufopen(..., TYPE_BITMAP)`, which decodes and resizes **into the audio buffer at buffering time**. Embedded art is used **only if it is JPEG**. | It decodes per track and per slot size, with JPEG overhead (`JPEG_DECODE_OVERHEAD` = 38 KB + `struct jpeg`, see `buffering.c:963`). The stock WPS keeps using it. The shell does **not** claim AA slots. |
| `apps/recorder/jpeg_load.c` | Baseline JPEG only. SOF2 (progressive), arithmetic coding and lossless all return `-4` (around line 1057). It has DCT-domain downscaling (1/2, 1/4, 1/8), plus `resize_on_load()` for the rest. It is reentrant when `JPEG_FROM_MEM` is not defined, because `struct jpeg` lives in the caller's buffer. | It **cannot read progressive JPEGs**, which are common in downloaded covers and in Apple Music `covr` atoms. |
| PNG | `lib/rbcodec/metadata/mp4.c` recognises a PNG `covr` (`AA_TYPE_PNG`), but the core has **no PNG decoder** (only the imageviewer plugin has one). | PNG art cannot be decoded on the device in the core. |
| `apps/recorder/bmp.c` | Reads BMPs of any depth and resizes them. When the size already matches, it is fast. | It is a fallback source format only. |
| `apps/plugins/pictureflow/pictureflow.c` | Has its own cache: `<PLUGIN_DEMOS_DATA_DIR>/pictureflow/%x%x.pfraw`. The name is `mfnv(album)` and `mfnv(albumartist)`, two 32-bit FNV variants. The header is only `{int32 w, h}`, followed by native pixels. Slides are 128×160 on this LCD (`DISPLAY_WIDTH` = max(120, 128) and `DISPLAY_HEIGHT` = 2/3 × 240). It builds incrementally, one album per idle tick (`incremental_albumart_cache()`), and keeps the progress in its config file. | **Ideas we reuse:** the key comes from album and album-artist strings; the format is raw native pixels; the build is incremental and resumable. **What we fix:** it has no invalidation (a changed cover is never noticed), keeps all files in one flat directory, uses 32-bit hashes with no collision check, and uses `splash()` (forbidden by the native-illusion rule). |
| Tagcache | String tags (`tag_album`, `tag_albumartist`, `tag_artist`) are de-duplicated in `database_N.tcd`. Inside one database build, an index entry's seek offset is effectively a unique string ID. Each track also stores `tag_mtime`. | The seek offset **changes on every rebuild or commit**, so it cannot be a persistent key. `tag_mtime` is useful for invalidating embedded art. There is no album-level ID. |

## 2. Sizes

| ID | Used by | Size (px) | Bytes (RGB565) | Notes |
|---|---|---|---|---|
| `ARTCACHE_THUMB` | Album rows in lists (Albums, Artists → Albums) | 55×55 | 6,050 | Stored in the shared `thumbs.pak` (see §4). Confirm against the references in `docs/reference/7g/`. |
| `ARTCACHE_COVERFLOW` | Cover Flow | 128×128 | 32,768 | Matches the Cover Flow texture budget (16 × 128² ≤ 512 KB). |
| `ARTCACHE_NOWPLAYING` | Now Playing (album cover, and chapter images: `chapter-art.md`) | 142×142 **box** | ≤ 40,328 | **Fit, not fill** (Phase 5, D10; owner photo 1222): the source is scaled so its long side fills the box, aspect kept, never cropped, upscaled if smaller, rounded half up (`artcache_fit_rect()`): 16:9 → 142×80, 2:3 → 95×142, 4:1 → 142×36. The bundle stores that fitted w×h (`artbundle_img`), the reader returns it in `bm->width/height`, and a reader's buffer holds the whole box. Centred both ways in the box (tall art centred horizontally ⚠️). One bundle per album with art (`XX/XXXXXXXX.ART`: 512 B header + pixels, 40,840 B for a square cover), written by the builder right after the thumbnail from the same source (a second decode); a dropped album's bundle goes with its record (GC). |
| `ARTCACHE_PANEL` | Main-menu and Music split screen (right half, x 160–319, full height: 160×240) | 300×300 | 180,000 | **H1.** Pan-only drift at up to 8 px/s over a ~7 s dwell travels up to 56 px; the tight axis is the height, 240 + 56 = 296 ≤ 300 (the width leaves 140 px), so ~30–56 px pans work in any direction. 600 B rows keep blits word-aligned. Built only for the **panel set** (§7), in `panels.pak`, not for every album. |

- **Reflections are not stored.** The renderer computes them when it loads a cover (about 128×42 blends, roughly 2 ms at 30 MHz). That happens once per load, not once per frame. Storing them would add about 30% more disk and would tie the cache to one background colour.
- **Sizes are a table, not code.** Each size has a numeric ID and `{w, h}`. The bundle header lists the sizes it contains. If a theme metric changes, the builder regenerates only the sizes that are missing or wrong.
- **Aspect ratio:** thumbnails, Cover Flow and panels fill the square, then centre-crop. A non-square cover is cropped, not letterboxed, so a 1:1 tile is always fully covered (the 7G behavior). Now Playing is the exception: it **fits** (row above).
- **Decode block** (reserved at shell start, movable, pinned only during a decode and the bundle write after it): the larger of the thumbnail fill (`JPEG_DECODE_OVERHEAD` + 4 × 55² × 2 + 3 × 4 × 4 × 55 + 4,096 = 78,284 B on the target) and the Now Playing fit (47,348 + 142² × 2 + 3 × 4 × 4 × 142 + 4,096 = **98,588 B**): **+20,304 B, permanent** (Phase 5 budget line). The fit decodes straight to the fitted size, so it never needs `FILL_MAX` × the box.
- **VERSION** is `IPAC 3` from Phase 5: an older cache (no bundles) is wiped, bundle directories included, and rebuilt once.

## 3. Pixel format

- RGB565, native byte order (`fb_data` on this target), row-major, no padding (stride = `w*2`).
- Each image's data starts on a **512-byte boundary** in the file. A read then lands sector-aligned and can go straight into a `core_alloc` buffer, or straight into a texture slot.
- **Dithering:** downscale in 8-bit-per-channel (`struct uint32_argb` in `resize.c`), then do ordered dithering to 565 once, at build time (`FORMAT_DITHER`). Gradients in dark covers then don't band.

## 4. File layout

```
.rockbox/artcache/
  VERSION              text: "IPAC 2\n" (bumping it wipes the cache; 1 had 40 px thumbs)
  index.dat            sorted index (below)
  thumbs.pak           fixed-size slot array of THUMB images (55*55*2 = 6050 B per slot, 512-aligned slot = 6144 B)
  panels.pak           H1: the panel set's PANEL images: a 1 KB header (struct artpanel_hdr: one 24 B entry
                       per album of the set, key, mtime, source CRC and size, state, slot) + up to 32 slots
                       of 300*300*2 + 16 B tag, 512-aligned = 180,224 B (format: apps/ipodui/artfmt.h)
                       #117: once published it is written IN PLACE: a panel goes into a slot the header
                       does not name (its old tag zeroed first, then pixels, tag, fsync), then the 792 B
                       header is rewritten at 0 (fsync). Only its creation and the maintenance regroup
                       (#92) still write a new copy (.tmp, renamed; the old one kept as .prev)
  00/ … ff/            256 shard directories (first byte of the key)
    1A2B3C4D.ART       one bundle per album: header + COVERFLOW + NOWPLAYING (+ PANEL)
  noart.ART            the "no artwork" glyph, pre-rendered at every size (our own asset)
```

**Bundle `XXXXXXXX.ART`** (an 8.3 name, so FAT needs no LFN entries):

```c
#define ARTCACHE_MAGIC   0x43415049  /* "IPAC" */
#define ARTCACHE_VERSION 1
#define ARTCACHE_MAX_IMG 4

struct artcache_img {          /* 12 bytes */
    uint8_t  size_id;          /* enum artcache_size */
    uint8_t  format;           /* 0 = RGB565 native */
    uint16_t w, h;
    uint16_t reserved;
    uint32_t offset;           /* from file start, multiple of 512 */
};

struct artcache_hdr {          /* padded to 512 bytes on disk */
    uint32_t magic, version;
    uint64_t key;              /* full 64-bit key (the file name holds only bits 32..63) */
    uint8_t  probe;            /* collision probe index, normally 0 */
    uint8_t  n_img;
    uint8_t  src_kind;         /* 0 none, 1 image file, 2 embedded in audio file */
    uint8_t  src_type;         /* AA_TYPE_JPG / BMP / PNG */
    uint32_t src_size;         /* bytes of source image (file size, or embedded blob size) */
    uint32_t src_offset;       /* embedded: offset in the audio file */
    uint32_t src_crc;          /* CRC32 of the source: first 16 KB, or (src_type
                                  ARTREC_T_SIG2) first 8 KB + last 8 KB */
    uint32_t src_path_hash;    /* FNV-1a 32 of the source path (relative to root) */
    uint32_t builder;          /* 0 device, 1 host tool; plus builder version in high bits */
    struct artcache_img img[ARTCACHE_MAX_IMG];
    char     keystr[];         /* "albumartist\x1Falbum\0", for collision checks; <= 400 bytes */
};
```

**`index.dat`**
- It is an array of 16-byte records, sorted by key: `{ uint64 key; uint32 thumb_slot; uint8 state; uint8 probe; uint16 gen; }`.
- `state` is one of:
  - `PENDING`
  - `OK`
  - `NOART` (we looked and there is no art, so the UI uses the note glyph and never retries)
  - `FAILED` (undecodable, for example a progressive JPEG on the device)
- 5,000 albums take 80 KB. The UI binary-searches it on disk through a 4 KB page cache, and never loads it whole into a static.
- `gen` is the library-update generation. It is used for garbage collection (§6).

**FAT32 directory performance.**
- Rockbox's `fat.c` looks up a name by scanning the directory linearly, and dircache may be off on this build (`docs/hardware.md`).
- One flat directory of 5k to 15k files (pictureflow's layout) means each `open()` scans hundreds of KB of 32-byte directory entries, and long file names make that 2 to 3 times worse.
- Sharding on the first key byte gives 256 directories of about 20 entries each at 5k albums. Using 8.3 upper-case hex names means one directory entry per file.
- Thumbnails live in **one** pack file, so scrolling the Albums list costs `lseek` and `read`, with no `open` per row.

## 5. Keying

```
artist  = tag_albumartist if non-empty and not "<Untagged>", else tag_artist
key_str = norm(artist) + "\x1F" + norm(tag_album)
key     = FNV-1a-64(key_str)
file    = shard "%02X" of key>>56, name "%08X" of (key>>32)&0xFFFFFFFF   (probe>0 → last hex digit range reserved, see below)
```

- **Use the same strings the Albums list groups by.** Phase 4 groups albums by (canonical album artist, album) from tagcache, and this key must be built from exactly those strings. Then the UI never opens a track just to find its art. Put `artcache_key()` and the grouping code in one helper.
- **`norm()`:** trim the ends, collapse runs of whitespace, and fold ASCII case. Do not fold Unicode, because it is too expensive and the host side can't match it exactly. The key is UTF-8, as tagcache stores it.
- **Untagged album** (`tag_album` is empty or `<Untagged>`): the key is `"dir\x1F" + the directory path of the first track`, so a folder of untagged rips still gets its `cover.jpg`.
- **Compilations:** with no album artist, every track artist forms a separate album. That matches how the Albums list will group them, so it is consistent. Tag compilations with an album artist.
- **Collisions:** 64 bits across 5k albums gives a probability of about 7×10⁻¹³, and we still check.
  - `keystr` in the header must equal the expected string, or the entry counts as a miss.
  - On a real collision the builder sets `probe = 1..15` and uses `key + probe` for the file and index entry.
  - `artcache_get()` follows probes until `keystr` matches. This path should never run in practice, but it keeps a wrong cover from appearing.
- **Don't use tagcache seek offsets.** They change on every commit.

## 6. Invalidation

- **The source is picked in the stock order,** so stock and shell agree. It is the first of:
  1. an image file found by `search_albumart_files(id3, "", …)` (no size string) for the album's first track
  2. embedded art in that track (`id3->has_embedded_albumart`, `id3->albumart.{type,pos,size}`)
- If `global_settings.album_art == AA_PREFER_EMBEDDED`, swap the two.
- **Fingerprint:** `{src_kind, src_path_hash, src_size, src_offset, src_crc}`.
  - We **don't use the FAT mtime**: a Mac writes FAT timestamps in local time, so host and device can disagree by a timezone offset or by FAT's 2-second rounding.
  - CRC32 over the first 16 KB of the image bytes catches a replaced cover of the same size. Since #90 (review M2) new records mark `src_type` with `ARTREC_T_SIG2` and the CRC covers the first 8 KB and the last 8 KB (all of it up to 16 KB); older records keep the first-16 KB CRC and are compared in that style. Limit: a same-size edit inside the unread middle of a large source is not seen until Rebuild Cache (no source mtime is stored). A record the lazy fill cannot sign gets `ARTREC_T_UNSIGNED` and is not tried again.
- **When we check (only during library update, never at browse time):**
  1. **Changed albums:** after a tagcache commit, any album whose tracks were added or whose `tag_mtime` changed gets a full check (source search, size and CRC). Embedded art can change only when the audio file changes.
  2. **Sweep:** on each library-update pass, `stat()` the recorded source of every `OK` and `NOART` entry, about 5k stats. A size change or a missing file leads to a full check. Covers replaced with a same-size file and no tag change are caught by an occasional full re-CRC (once every N updates).
  3. **Format or size-table change:** a new `VERSION` wipes the cache. A new size ID only adds images to existing bundles.
- **Garbage collection:** each library update bumps `gen` and stamps it on every album it sees. Entries whose `gen` is old get their bundle deleted, their thumb slot freed (a free list sits at the end of `index.dat`), and are dropped from the index.
- **As built (4b-1, H1 #41.1):** the final commit of a pass drops the records of albums no longer in the table (the GC streams the table and walks the whole index: ~6 reads of a 1.6 MB index at 50k). **While audio plays it is deferred**: the commit keeps every record (`art build gc deferred stale=N`; a stale record is never looked up, so no harm), and once playback stops (paused counts as stopped) the builder runs the pass again: every album is up to date, so it does no source I/O, and its final commit collects (`art build commit ... gc=1 playing=0`). The sweep's stamp is recorded only by a commit that collected. Tested in the ready, fresh and dirty states: `tests/ui/ipodui-h1-pre-{ready,fresh,dirty}.script`.

## 7. Main-menu panel set

*As built in H1 (docs/phase-nav-brief.md, Part 1 and Revision 1).*

- **The set** (≤ 32 albums, `LIBFILE_PANELS_MAX`) is chosen **at every library precompute** (library.c `pre_panels()`, on the tagcache thread, from the album table it just assembled) and written as the `LIBFILE_CAT_PANELSET` section of `database_lib.dat` (libfile.h; 24 B per album, the album-table record). Music albums with a key only (no books, no shows), one per key:
  1. up to 12 **most recently played** albums (`tag_lastplayed`, newest play first; one top-K pass over the master index, a key computed only for a track that enters the list);
  2. up to 10 **newest** albums (`tag_mtime`);
  3. the rest **random**: the lowest scores of a hash of the album key and an epoch that moves every 8 commits (rendezvous hashing), so adding or deleting other albums leaves these alone and the builder redoes few panels per update (host test: deleting an album outside the set changes none of it).
- **The images** (`ARTCACHE_PANEL`, 300×300, §2) are built by the art builder thread **only while audio is stopped** (`audio_status() == 0`; paused counts as playing here, because the block below would shrink a paused buffer), after a full pass: it drops entries no longer in the set, then for each album whose thumbnail record is OK and whose panel is missing or stale (another mtime, source CRC or size), decodes its source again (stock order, as §6) at the 300 px fill size into a **transient** block, centre-crops it in place and writes the slot. The block is allocated per album and freed at once: `JPEG_DECODE_OVERHEAD` + 4 × 300 × 300 × 2 (`FILL_MAX`, for non-square sources) + 3 × 4 × 4 × 300 (resize line buffers) + 4,096 = **785,844 B on the target** (47,348 + 720,000 + 14,400 + 4,096). If playback starts, the rest waits for the next stop (`panels_owed`). Never boosts (the builder thread's resize boost is off). Albums with no art or undecodable art get an entry with no slot (no retry until their source changes). Trace: `art build panel key=K slot=S state=T boost=0`, `art build panels set=N built=B ms=T`.
- **Reading** (UI thread): `artcache_panel_count()` (re-reads the 1 KB header only after the builder changed it, or after USB) and `artcache_panel_get(i, bm, buf, size)` (one open + lseek + 180,000 B read into the caller's buffer, tag checked; never decodes or allocates). The caller (#38 agent A's panel scheduler) decides when storage may be woken: panel reads only at a cover change (~7 s), during playback only while storage is awake.
- **Drift is pan-only (owner, 2026-10-07):** integer-pixel pans of the 160×240 window across the 300×300 image, 5–8 px/s, ~7 s dwell, ~400 ms cross-fade between two 300×300 slots reserved at shell start by #38 (360,000 B). No zoom, no per-frame resample. It holds no `cpu_boost`.
- **Owner (2026-10-06):** the panel art "does this weird zoom and move thing" (a Ken Burns-style drift); the 2026-10-07 decision above makes it pan-only.
- **Rotation and the pool (#100, Phase 16-4).** The precompute also writes a **pool order** after the books count in the set's col 1 (no new section): the pinned (recently played, newest) and up to 100 music albums and 20 books by rendezvous hashing with a **fixed** seed (`libfile.h` `struct libfile_pool`). `panels.pak` stays the default set above, version 1, which an 8b build reads (rollback). The pool's other covers are in `panelpool.pak` (same slot layout, written **in place** into slots the published index does not name, fsynced, then the index), built only in a maintenance window (`regroup_window()`, `maint_take(MAINT_PANELS)`, the floor), `POOL_STEP` per step, at most 128, fewer under 1 GB free (32) and none under 256 MB (Q9). `panelpool.idx` (4,640 B, `safefile_replace()`) holds the **shown set** in its first 1 KB (an `artpanel_hdr`, magic `IAPS`; a slot ≥ 32 names a `panelpool.pak` slot), then the pool table, the pinned keys, the shown random picks and the last rotation's RTC time. The UI's readers read that 1 KB, else `panels.pak`'s header. A **rotation** (`artpool_pick()`, no image work, one index write) happens at the first pass or storage wake after boot and every `PANEL_ROTATE_SECS` (3 h) of RTC time (time asleep counts), only while storage is already awake (never a wake of its own); pinned first, 10 music places and 3 of 8 book places (Q10) left to random picks drawn fresh (none of the previous set's). After a build the shown set is refreshed (holes filled, the random picks kept). Known costs: a cover in the pool is one 180,224 B in-place write plus a 4,640 B index; a `panels.pak` cover likewise (#117, below); the drift's bank refresh is one read per run of consecutive slots of one file (a rotated set is rarely consecutive: up to 4 reads at a wake instead of 1).
- The Settings panel is **not** a gear. Per the owner photo `settings-01-about.jpg` it shows the device name, our own logo, a capacity capsule bar and "N GB Free" (see the spec, Main menu split screen).

## 8. When to build: on the device vs. on the host

| | **On device** (background library update) | **Host-side Mac tool** (`tools/artcache/`, run on sync) |
|---|---|---|
| Formats | Baseline JPEG and BMP only. Progressive JPEG and PNG (including Apple `covr` PNGs) end as `FAILED`. | Everything, through Pillow or `sips`/CoreImage. |
| Quality | `resize_on_load` (area or bilinear) plus ordered dither. Fine. | Gamma-aware Lanczos plus dither. Better. |
| Speed | At 30 MHz, unboosted, a 600–1000 px baseline JPEG with DCT 1/2 to 1/4 prescale takes roughly 0.5–2 s per album (to be measured). 5k albums: **about 1–3 h** the first time, then incremental. | 5k albums in about 1–2 min. |
| Battery | Real cost. Best done while on USB power or charging. | None. |
| Key correctness | **Exact.** It uses the same tagcache strings the UI groups by. | Risky. The host reads tags with mutagen, which differs from Rockbox's parser on ID3v1 fallback, multi-value fields and whitespace. Needs a shared normalization spec and golden tests. |
| Self-sufficient | Yes. Works whether files arrive by Finder copy, rsync or anything else. | Only if the user runs the tool on each sync. |

**Recommendation: the on-device builder is the authority, and the host tool is an optional accelerator that writes the same format.**

- **Phase 7 ships the device builder first.** The device must stay correct with no Mac in the loop, and only the device sees the exact tagcache strings.
- **Phase 7b adds `tools/artcache/artcache.py`.**
  - It writes bundles and index records with `builder = 1`, using the same key function. A golden test in `tests/artcache/` checks that the host and `artcache_key()` agree on the fixture tags.
  - When the device builder meets a host-written entry, it checks the fingerprint (size and CRC) and the `keystr`. If they match, it keeps the entry, so the work isn't redone.
  - The host tool is also the **only way to get progressive JPEG and PNG art**. Albums the device marks `FAILED` show the note glyph until it runs.
- This gives correctness without the Mac, and quality and speed with it.

**Device builder scheduling:**
- **Thread:** a separate `artcache` thread at `PRIORITY_BACKGROUND`, on the CPU core. It is created only while there is work, then exits, so its stack is freed.
  - Note that the tagcache thread boosts during a build (`tagcache.c:5052` and `5248`). The art builder must **not**.
  - It runs unboosted, sleeping between albums.
- **Trigger:** the end of a tagcache commit. There is no event for this today, so add a tiny `#ifdef HAVE_IPOD_UI` callback in `tagcache.c` `commit()`, or poll `tagcache_get_stat()->commit_step` from the shell.
- **Throttling:** process one album, then `yield()`/`sleep(HZ/10)`. Pause while any of these holds:
  - the user pressed a button in the last 2 s (stopped or paused; during playback see below)
  - the audio buffer is filling (avoid storage contention)
  - the battery is below 20% and not charging
- Run at full rate while charging or on USB power.
- **During playback (#58 idle build, owner 2026-10-09; replaces 4b-1's fill windows):**
  - While the user interacts (backlight lit, no clock face), the builder does **nothing**: no reads, no writes, no CPU. Its wait blocks on an event (`art build wait lit`), never a poll. This keeps the UI free of hitches and the refills free of storage contention.
  - While the user is idle (the unlit clock face is up, or the backlight is off, and no key is held) it runs **continuously** at `PRIORITY_BACKGROUND`, unboosted, with the usual gap between albums, until the backlog is done. It may wake storage on battery (the backlog is finite). Wakes: `artcache_note_idle()` (the UI, on `SYS_IPODUI_BACKLIGHT_OFF`) and `PLAYBACK_EVENT_STOP_OR_PAUSE`.
  - On the charger it also builds while lit once no key came for 2 s (the backlight may stay on there): one timed wake, not a poll.
  - A user who comes back mid-album stops it before the album's next read or write (`io_go()`); nothing of it is kept, and it is redone at the next idle. A panel stops at its next band write and is redone the same way.
  - The albums the user saw PENDING (the hint ring: list rows on screen, the Now Playing album) go first once idle. **Now Playing art that is PENDING while the user watches stays the placeholder until the next idle** (no single build in a fill window: that was the only option with any playback risk).
  - The final commit's GC, the chapter-art pass and the books pass still wait for playback to stop (unchanged).
  - Once the backlog is done the thread sleeps with no timeout: no poll and no wake (the audio refills no longer post to it) until a request (a database commit or rescan), USB or a power-off.
  - A pause counts as stopped: the stopped throttle (2 s after a key, 20 % battery) is unchanged.
- Shown to the user only as the 7G "Updating Library…" row. Never a splash.
- **COP:** a later option is to decode and scale on the COP while the CPU does the file I/O. Do this only after measuring (`docs/hardware.md`, Second core). File I/O stays on the CPU.

## 9. Memory during the build

All of this is one movable `core_alloc("artcache-build", …)` block, held only while building. There are no statics.

*As built in 4b-1 (brief Revisions 1-2 and the end review), this differs:* both blocks are reserved at shell start. Everything the builder reads or writes files into (index reader, album-table chunk, `mp3entry`, paths, the slot, a 4 KB page; 28,248 B + the 6,144 B slot on the target) shares one block with the thread's 9,216 B stack, pinned for good like any running thread's stack, so no I/O ever targets memory buflib may move: **43,608 B pinned**. The decode buffer (`JPEG_DECODE_OVERHEAD` 47,348 B + the 4× fill-size bitmap 24,200 B + line buffers 10,560 B + 4,096 B slack = **86,204 B**, movable) is pinned only for one decode. The UI's reader block is 8,256 B (movable). All of these derive from `artcache_sizes[ARTCACHE_THUMB]` (55×55) in `artcache_reserve()`. The final commit's GC streams the album table through the decode block in chunks of `decode_bytes / 8` keys (10,775), each sorted and merged with one sequential walk of the index, so it has no album limit.

| Part | Bytes |
|---|---|
| JPEG decode working memory: `JPEG_DECODE_OVERHEAD` (38 KB) plus `struct jpeg` (it goes into the caller's buffer because `JPEG_FROM_MEM` is not set, so the decoder is thread-safe next to playback's own decodes) | about 48 KB |
| Intermediate: decode **once** to the largest size needed (240² for panel albums, otherwise 140²), in 8-bit-per-channel ARGB | 140² × 4 = 78 KB (panel: 230 KB) |
| Each output size, downscaled from the intermediate (not re-decoded), with dithering into an RGB565 buffer | 39 KB at most |
| `resize_on_load` line buffers (3 × w × 4) | about 3 KB |
| Header, I/O and CRC buffer | 16 KB |
| **Total** | **about 185 KB typical, about 340 KB for a panel album** |

- This comes out of the audio buffer only while building, and that is acceptable.
- If `core_alloc` fails (playback has a full buffer), skip and retry later. Never shrink playback.
- The UI's own **read path** needs no decode memory: `artcache_get()` reads straight into the caller's buffer.

## 10. Disk-space estimate (32 KB clusters)

| Item | Per album | 5,000 albums |
|---|---|---|
| Bundle: 512 B header + 128² + 140² (rounded up to 512) = about 72.5 KB, which takes **3 clusters** | 96 KB on disk | 480 MB |
| Thumb slot in `thumbs.pak` (55×55, 512-aligned) | 6 KB | 30 MB |
| Index record | 16 B | 80 KB |
| Panel images (H1: `panels.pak`, 1 KB + 32 × 180,224 B) | — | 5.77 MB |
| **Total** | | **about 500 MB** (0.05% of 1 TB) |

The cluster slack (96 KB on disk against 72.5 KB of data) is tolerable. Packing Cover Flow and Now Playing images into a big pack file would save about 115 MB, but compaction becomes hard when albums change. Separate bundles are simpler to invalidate, and disk is not the constraint here.

## 11. API for the UI shell

`apps/ipodui/artcache.{c,h}`. It is UI-side code and may use `apps/`, so it is **not** in `lib/rbcodec`.

```c
enum artcache_size {
    ARTCACHE_THUMB = 0,      /* 55x55  */
    ARTCACHE_COVERFLOW,      /* 128x128 */
    ARTCACHE_NOWPLAYING,     /* 140x140 */
    ARTCACHE_PANEL,          /* 300x300, panel set only (H1) */
    ARTCACHE_NUM_SIZES
};

enum artcache_result {
    ARTCACHE_OK      =  0,
    ARTCACHE_NOART   =  1,   /* known: album has no art -> draw note glyph */
    ARTCACHE_PENDING =  2,   /* not built yet -> draw note glyph, may appear later */
    ARTCACHE_ERR     = -1,   /* I/O, bad header, buffer too small */
};

typedef uint64_t artcache_key_t;

/* Key from the same strings the album lists group by. NULL/"" handled. */
artcache_key_t artcache_key(const char *albumartist, const char *artist,
                            const char *album, const char *first_track_path);
artcache_key_t artcache_key_for_id3(const struct mp3entry *id3);

/* Dimensions for a size id (from the size table). */
void artcache_dim(enum artcache_size size, struct dim *out);

/* Synchronous read into a caller buffer (>= w*h*2 bytes, any alignment;
 * 512-aligned is fastest). Fills bm->{width,height,data}. Never decodes,
 * never splashes. One open+lseek+read (thumbs: no open, pak fd is cached). */
int artcache_get(artcache_key_t key, enum artcache_size size,
                 struct bitmap *bm, void *buf, size_t bufsize);

/* Same, but allocates a movable core_alloc handle the caller owns
 * (core_free when done). Returns handle > 0, or <= 0 with result in *res. */
int artcache_get_handle(artcache_key_t key, enum artcache_size size,
                        enum artcache_result *res);

/* Lookup only (index.dat), for deciding whether to show a glyph. */
enum artcache_result artcache_state(artcache_key_t key);

/* Builder control (called by the shell's library-update logic). */
void artcache_build_request(void);     /* after tagcache commit */
void artcache_build_pause(bool pause); /* e.g. during USB, Cover Flow open */
struct artcache_build_status { int done, total; bool running; };
void artcache_build_status(struct artcache_build_status *st);

/* H1: the panel set's 300x300 images (§7). UI thread only. */
int artcache_panel_count(void);
int artcache_panel_get(int i, struct bitmap *bm, void *buf, size_t size);
```

**Usage rules:**
- **Thumbnails:** the list keeps an LRU of about 16 thumbs (3.2 KB each, about 51 KB, in one `core_alloc` block) and calls `artcache_get()` only for rows that become visible.
- **Cover Flow:** a texture cache of 16 × 128² (512 KB, the budget table) is filled from `artcache_get()`. A loader runs ahead of the scroll direction.
- **Now Playing:** use `artcache_key_for_id3(current id3)`, then `artcache_get(NOWPLAYING)`. The shell does **not** use `playback_claim_aa_slot()`, so playback never decodes art for the shell. If the result is `PENDING` or `NOART`, draw the note glyph.
- Never call `artcache_get()` from the audio or codec thread. Callers supply their own buffers (`docs/hardware.md`: no shared scratch).

## 12. Tests and verification
- **Host unit test:** `artcache_key()` against the golden keys (`tests/artcache/keys.txt`). The Mac tool must match the same file.
- **Simulator:** a fixture library of 20 albums mixing baseline JPEG, progressive JPEG, PNG `covr`, embedded MP3 APIC, `cover.jpg` and no art. Run the builder, then check:
  - the states are `OK`/`FAILED`/`NOART` as expected
  - replacing `cover.jpg` triggers a rebuild after a library update
  - deleting an album triggers garbage collection
- **Device:** measure ms per album unboosted, the total build time and the battery cost for the real library, and record them in `docs/hardware.md`.

## 13. The Now Playing row's sharp cover (`npcover.pak`, np-sharp)

**Owner, 2026-10-24:** "Keep the stretched if it's worse, but make sure the sharp is queued." The main menu's Now Playing row shows the playing album's 300 px panel when one is built: in the shown set, in `panels.pak`'s default set or in the pool (`artcache_panel_window()`). An album played since the last precompute is in none of them, so the row stretches the 55 px thumbnail. That stand-in stays, and a sharp panel for that album is now queued and rendered in the background.

- **File.** `npcover.pak` uses `panels.pak`'s layout (a 1 KB front, then 300 × 300 slots of 180,224 B, the pixels and then the tag), with its own magic (`ARTNPC_MAGIC`, "IAPC") and at most two slots: 1 KB + 2 × 180,224 B = 361,472 B on disk. The entries run most recent first, so a new render takes a free slot or the oldest entry's slot (`artnpc_slot()` and `artnpc_put()` in `artfmt.c`; host tests in `tests/artcache/test_artfmt.c`).
- **Writes.** A render is written in place, in this order:
  1. `npc_seq` + 1;
  2. the slot's old tag is zeroed and fsynced;
  3. the pixels in bands of the reserved decode block (`ps_decode()` through `panel_stream()`, the decode `panel_build()` uses too);
  4. the tag, then fsync;
  5. the 1 KB header, then fsync;
  6. `npc_seq` + 1 again.

  A reader checks the tag against the entry and `npc_seq` around the read, so it never takes another album's pixels or a half-written slot.
- **Queue.** One album is queued at a time, and the newest request wins (`artcache_npcover_queue()`). There are two reasons to queue:
  - `why=row`: the row found no panel (`PENDING`). The row draws only while it is highlighted.
  - `why=played`: the album has played for 30 s (`artcache_rotate_poll()`, 1 Hz).

  Skipped tracks queue nothing, so a run of shuffled skips never asks for one render per track. A failed render is remembered in RAM until USB or the next boot.
- **When it runs.** The render runs in the idle builder's loop, outside a pass, only when `npr_place()` and `!blocked()` both hold (the queue's post, the 1 Hz edge and the idle post check the same):
  - storage is already awake: it never spins the disk up on its own account;
  - the user is idle, and Now Playing is not lit;
  - while playing, only on a charger. P19 D-3 rules out art work in dark playback on battery;
  - there is no USB session, sleep hold or pause request.

  It wakes on a fill's `ART_Q_NP` post, the idle post (`artcache_note_idle()`) and a rising edge of `npr_place()` in the 1 Hz poll. Before it decodes, it checks again that the queued album still plays (its key equals `audio_current_track()`'s, otherwise the request is dropped) and that no panel exists in `panels.pak`, `panelpool.idx` or `npcover.pak`. The decode is never boosted (`resize_boost_cap(0)`), and `art_gate()` parks it if the user comes back while audio plays.
- **Eviction and calling a render off (review M1–M3).** When both slots are taken, the album that holds the chosen slot leaves the header first (written and fsynced, `art npcover evict`), before any pixel is written. So a power cut, a failed decode or a deferral leaves that album absent: it is rendered again when queued, and never named over an untagged slot. `npr_have()` also checks the slot's tag, so a named slot without its pixels counts as missing.

  A render in progress is called off (DEFERRED, still queued; `art npcover abort`):
  - if the disk went to sleep during an `art_gate()` park, because its next read would spin the disk up;
  - if Now Playing is lit, or there was input since the render began, while paused or stopped, where `gate_hold()` does not park.

  `art_gate()` sets `npr_abort`. The source is then read as at its end, the JPEG decoder stops at its next MCU row (`jpeg_set_abort_hook()`), and `io_go()` and `ps_flush()` write nothing more. They also refuse whenever the disk is not awake during a render.

  An album whose thumbnail record says no art, or art the device cannot decode, is dropped before any source I/O.
- **Events during a render (review H1).** `io_go()` now takes every queued event, not only the first. A request or a USB session that it takes outside a pass (in the rotation or a render) is turned into a pass right after (`art build resume idle-io`). Before, it was folded into `pass_events`, which only a pass reads, so it was lost.
- **USB (review L1).** The host may replace a cover, so `npcover.pak` is removed once on the way out of each USB session (`art npcover reset why=usb`). Albums still without a panel are rendered again when queued.
- **The row.** `artcache_panel_window()` looks in `npcover.pak` third. Its memo of a conclusive miss also records `npc_seq`, so a render that lands clears the memo. A whole read whose tag names another album counts as conclusive too and is memoised the same way. While the stand-in is up, `ui_panel_tick()` redraws the row once `artcache_npcover_seq()` moves, but only while storage is still awake. Otherwise it waits for the next natural wake.
- **Traces:** `art npcover queue|have|drop|evict|start|build|abort|reset key=K [why= from= state= slot= ms=]`, and `art panel window ... from=npcover`. Scripts: `tests/ui/ipodui-np-sharp-{render,gate,skip,mru,events,cut}.script`. The test knob `IPODUI_NP_NO_PANEL=built` leaves only `npcover.pak` to answer, both for the window and for the builder's check.
- **Threads (review L4).** `npr_key` and `npc_seq` are plain volatiles with no lock. They are safe because the UI, tagcache and builder threads all run on the CPU. Moving the UI to the COP (#72) needs atomics or a lock here.
- **Cost:** about 3.5 KB of `.text`, 65 B of statics and no new RAM block, because the render streams through the decode block the builder already reserves. On disk the file is at most 361,472 B.

**Hardware features.** Today the PP decodes the cover in bands of the reserved block, the same path the panel builds use. P19 (19-6) brings the BCM2722's `DECODE` job, which takes over through the same entry point: `npr_decode` is one function pointer (`npr_decode_pp` now, `bcmjob_decode` later, with the PP path kept as its fallback). It may run only when lit or on a charger (D-3), which `npr_place()` already enforces. The COP is not used for this. In dark playback it is parked, and when the screen is lit it is the UI core, so a decode there would compete with the UI.

**State of the art.** The decode is the baseline JPEG path. The IDCT is the scaled AAN/LLM integer form, so a large cover is decoded at 1/2, 1/4 or 1/8 scale in the DCT domain. The Huffman decoder uses 8-bit lookahead tables. The scale-down is an area average (`resize.c`), which is the correct anti-aliased method for large reductions on an integer CPU. `docs/algorithms-survey.md` (section 3 and the BCM2722 kernel table) rates these at or near the state of the art for an ARM7TDMI. The open improvements apply to every panel build and do not belong to this feature: the multi-symbol Huffman tables with a register-resident refill (about 2× fewer instructions for big covers), dropping the unused alpha channel from the resizer (exact), and an optional Lanczos3 or Mitchell downscale.
