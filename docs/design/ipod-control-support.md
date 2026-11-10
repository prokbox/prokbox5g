# Synced libraries in `iPod_Control/Music` (#225): design brief

**Status:** brief for the owner's 0.8.1 cut, 2026-11-08; **built on `p081-ipodctl`** (S1, S2, S3; see §14 for the owner's decisions, what changed from this brief and the measurements as built). Sections 1–13 are the brief as written. The measurements are from the simulator and from the firmware's own file layer on a host FAT image. Device numbers are estimates, tagged as such.
**Branch:** `p081-ipodctl-brief` (on `p081-backports`); implementation `p081-ipodctl`.
**Issue:** #225 (owner, 2026-11-08: full support for libraries written by Apple's sync software, an owner exception to 0.8.1's fixes-only rule).

Tags: **[sim]** measured in the shell simulator; **[fat]** measured on the firmware's own `fat.c`/`file.c` over a RAM FAT32 image (`tests/fatcrash/dirburst.c`); **[model]** a count from [fat] or [sim] turned into device time with the cost model in §2.1; **[silicon]** measured on the device earlier (source named); **[public-doc]** from public format documentation (§5.7); **[hypothesis]** not checked.

## 1. Summary

A user who moves from Apple's sync software keeps a library that is laid out for a database, not for people: every track sits in one of 50 folders (`iPod_Control/Music/F00`…`F49`) under a random four-letter name, an album's tracks are scattered over those folders, and there are no cover image files anywhere under `Music`. The covers live in Apple's artwork database (`iPod_Control/Artwork`), which we don't read.

What the measurements show, in order of size:

1. **The art builder's image-file search is the largest storage cost, and it runs again after every library change.** It makes 21 path lookups per album (19 in the stock order plus 2 for PNG names), and all of them miss in this layout. At 5,400 tracks that is 11,340 lookups and 36,180 storage commands on the first pass [fat], about **43 s on flash** [model]; at 50,000 tracks, 530,062 commands, about **10.5 min** [model]. After any change to the library (one song added) every album is searched again: 10,288 lookups for 541 albums [sim], so about the same cost **at every sync**.
2. **The directory burst slots thrash on deep paths.** `fat.c` reads directories through 2 cached 16 KiB slots (#107). A track path here crosses 4 directories (root, `iPod_Control`, `Music`, `Fnn`), so each open misses both slots 4 times. The scan's open of every track costs 21,600 commands at 5,400 tracks (25.7 s [model]) and 273,535 at 50,000 (5.4 min [model]). With 4 slots it is **50 commands** at 5,400; with 8 slots, **214** at 50,000 [fat]. This is not specific to synced libraries: the drag-and-drop tree (`Music/Artist/Album/track`) has the same 4 levels and the same cost.
3. **Embedded covers decode on the CPU** at 0.5–12 s per panel (rc.7 owner log) and up to 26–46 s for a large cover ([silicon], `docs/phase19-brief.md` row "Art decode"). Covers in Apple's artwork database are already RGB565 at 200×200 and 100×100 [public-doc]. Reading one costs one 80 KB read and a scale, with no JPEG decode.
4. The scan and commit themselves are CPU work that the probe skip and the slots don't touch: 2.1 s + 0.4 s host time at 5,400 tracks and 14.3 s + 7.0 s at 50,000 [sim].

**Recommended 0.8.1 cut** (§8): the probe skip (S1), more burst slots (S2), and covers from Apple's artwork database (S3). Reading tags from Apple's track database instead of the files (S6) and the general fix for the post-change re-search (S4b) go later.

## 2. Today, measured

### 2.1 Method and cost model

- **Fixture.** `tests/perf/ipodctl_library.py` writes the synced layout: `iPod_Control/Music/F00..F49`, random four-letter names (`ABCD.mp3`), each track in a random folder, 10 tracks per album, no image files, the empty `iTunes` and `Artwork` folders. Tracks are the tiny tagged MP3s of `synthetic_library.py`. 30 % of albums carry an embedded 600×600 baseline JPEG that the script draws itself (gradients and bars, our own pixels). At 5,400 tracks every track of such an album carries it; at 50,000 only the first track does (`--art-first`), which keeps the disk small. The art builder reads only that track. Synthetic only: no real music, names or covers.
- **Simulator run.** `tests/perf/ipodctl_sim.sh [tracks]` boots the shell sim on that disk and runs `tests/perf/ipodctl-first.script`: first build, first art pass, one track added, update, second art pass. The sim has no ATA layer, so its times are host times (Apple M3). It reports counts that carry over to the device: lookups, reads and records. The art pass's `exists=` count is new, sim-only instrumentation (`filesystem-sim.c` `sim_exists_count2()`).
- **Storage commands.** `tests/perf/ipodctl_fat.sh` runs `tests/fatcrash/dirburst.c`'s new `ipc`/`tree` modes. They use the firmware's real `fat.c`, `file.c` and disk cache on a RAM FAT32 image with the 5.5G's geometry (2,048-byte BPB sectors, 16 KiB clusters). Each mode counts storage commands and sectors for the walk, an open of every track, and the art builder's exact probe sequence per album, all from a cold cache. It builds the firmware as shipped (2 burst slots), without bursts (upstream), and with 4, 6 and 8 slots.
- **Device time [model].** Each command costs 0.41 ms (single-sector reads on the flash adapter, boosted, [silicon] `docs/handoff-2026-10-19-scan-speed.md`), plus its bytes at 21 MB/s ([silicon] #63 fills, `docs/hardware.md`). CPU-bound sim times are multiplied by **×200–400** (`scripts/check_trace.py` `SIM_FACTOR`, uncalibrated). That factor overstates the scan, because the sim's scan time also includes host file I/O. **A hard drive is not modelled:** each command that misses the drive's own cache adds a seek, so every storage row below is a lower bound for a hard-drive unit.

### 2.2 First run, 5,400 and 50,000 tracks

| Step | 5,400 tracks (540 albums) | 50,000 tracks (5,000 albums) | Source |
|---|---|---|---|
| Scan walk (`check_dir`) | 203 commands, 0.2 s | 373 commands, 0.4 s | [fat] [model] |
| Scan: open of every track (path walk only) | 21,600 commands, **25.7 s** | 273,535 commands, **5.4 min** | [fat] [model] |
| Scan: tag read per track | ~3 reads, ~1.2 ms each: ~6.5 s | ~61 s | [hypothesis]: reads per tag not counted |
| Scan, host time (walk + tags + parse) | 2,149 ms | 14,348 ms | [sim] |
| Commit (first build) | 410 ms host, ×200–400: 1.4–2.7 min | 6,970 ms host: 23–46 min | [sim] [model] |
| Art pass: image-file lookups | **11,373** (21.06 per album) | **104,904** for 4,988 albums (21.03 per album) | [sim] |
| Art pass: storage commands for those lookups | 36,180, **43 s** | 530,062, **10.5 min** | [fat] [model] |
| Art pass: albums with embedded art / with none | 163 / 377 | 1,491 / 3,509 | [sim] |
| Art pass: decodes | 163 albums × (thumb + Now Playing; + panel for the 32 of the set) at 0.5–12 s each | 1,491 × the same | [silicon] rc.7 log; #219 measures VC decode |
| Art pass, host time | 59 s; every per-album line says `ms=0`, so the time is most likely the builder's pacing yields, not work [hypothesis]; not a device estimate | 517 s, the same caveat; 171,373 reads, 76 MB | [sim] |

- **Lookups per album: 21.** These are `search_albumart_files()`'s 19 (track name ×3 extensions, album name ×3, `cover` ×3, `folder.jpg`, `.rockbox/albumart/<artist>-<album>` ×3, the parent's album name ×3, the parent's `cover` ×3) and `png_folder()`'s 2. The harness and the sim agree: 11,340 harness lookups against the sim's 11,373, which includes 33 other lookups made by the pass.
- **Every lookup misses** in the synced layout, by construction: the sync software never writes image files under `Music`.

### 2.3 After a change (one song added)

| Step | 5,400 tracks | Source |
|---|---|---|
| Update scan | 71 ms host; 5,401 lookups, 5,400 verification reads | [sim] |
| Update commit | 150 ms host | [sim] |
| Art pass (`sweep=1`: a full source re-check after any library change, `artcache.c` `source_unchanged()`) | **10,288 lookups** for 541 albums (19 per album), 3,390 reads, 2.2 MB | [sim] |
| Storage for those lookups | ≈ 19/21 of the first pass: **~39 s** at 5,400, **~9.5 min** at 50,000 | [model] |

So the image-file search is not a first-run cost only. A synced library pays it again at **every** USB session that changes anything. The drag-and-drop tree pays it too, but there the search can find something.

### 2.4 The 50,000-track run

First build: scan 14,348 ms, commit 6,970 ms (`db scan ms=21320`), first art pass 104,904 lookups for 4,988 albums [sim]. Some albums share their album and artist strings and so their key, which is why there are 4,988 and not 5,000. After one added song, the update took 3,430 ms (commit 1,630 ms). The post-change art pass (`sweep=1`, 4,989 albums) did not finish inside the script's one-hour limit, so its count at 50k is **not measured**. Per album the probe sequence doesn't depend on library size: the 5,400 run's 19 per album and the [fat] rows stand for it.

### 2.5 Burst slots against path depth [fat]

| Layout, tracks | Opens, 2 slots (as shipped) | No bursts (upstream) | 4 slots | 6 slots | 8 slots |
|---|---|---|---|---|---|
| synced, 5,400 | 21,600 cmd / 25.7 s | 71,118 / 30.9 s | **50 / 0.1 s** | 50 | 50 |
| synced, 50,000 | 273,535 / 5.4 min | 3,446,581 / 25 min | 222,535 / 4.4 min | 81,735 / 1.6 min | **214 / 0.3 s** |
| tree, 5,400 | 21,600 / 25.7 s | 103,671 | 1,080 / 1.3 s | 1,080 | 1,080 |
| tree, 50,000 | 389,368 / 7.7 min | 7,049,634 | 367,176 | 305,272 | 219,464 / 4.4 min |
| Art probes, synced 5,400 | 36,180 / 43 s | 174,120 / 76 s | 2,694 / 3.2 s | 526 | 509 / 0.6 s |
| Art probes, synced 50,000 | 530,062 / 10.5 min | 8,296,958 / 60 min | 440,062 | 440,062 | 34,447 / 41 s |

**Why.** A lookup reads each directory on the path. At 5,400 tracks a synced folder holds 108 names (3.4 KB, one cluster). At 50,000 it holds 1,000 names, which is 32 KB, or two clusters. Opening `/iPod_Control/Music/F23/ABCD.mp3` therefore needs 4 directories (5 clusters at 50k), while 2 LRU slots hold only 2. Each 16 KiB burst also reads a whole cluster even when the directory is 3.4 KB, so a missed slot costs 32 sectors where upstream's single-sector reads cost a few. That is why "2 slots" is barely better than "no bursts" here. The probes add `/.rockbox` and `Music` (the parent's names), so they need 6 or more slots at 50k. With 4 slots the opens stay at 50 commands up to 10,000 tracks and rise to 11,251 at 15,000 and 66,929 at 26,000. With 8 slots they are 131 at 26,000 [fat].

## 3. Speed-ups, ranked by saving against risk

| # | Speed-up | Saving (flash, [model]) | Risk | Size | 0.8.1? |
|---|---|---|---|---|---|
| S1 | **Skip the image-file search** for tracks under `iPod_Control/Music` | 5.4k: 43 s first pass, ~39 s every changed sync; 50k: 10.5 min and ~9.5 min | Low: one test in `artcache.c`, no upstream file | ~20 lines | **yes** |
| S2 | **Burst slots 2 → 8** (or → 4, see ⚠️ Q2) | opens 5.4k 25.7 s → 0.1 s, 50k 5.4 min → 0.3 s; the deleted-file pass and the boot probe gain the same; the tree layout gains too | Low-medium: one define in our own `fat.c` code; +96 KiB pinned (8) or +32 KiB (4) | 1 line + a ledger row | **yes** |
| S3 | **Covers from Apple's artwork database** (§5) | 70 % of albums in the fixture get a cover instead of a glyph; per album ~5 ms of I/O + a scale [estimate] instead of a 0.5–46 s JPEG decode, when chosen first (Q1) | Medium: a new parser of a foreign binary format; mitigated by bounds checks and host tests | ~700 lines + tests | **yes** (owner scope) |
| S4a | The post-change re-search (§2.3) **for synced tracks** | covered by S1 | — | — | with S1 |
| S4b | The same for the **tree**: only re-search albums whose folder changed (a per-album folder signature: the folder's first cluster + entry count, stored in the record) | per changed sync: 5.4k ~39 s → the changed albums only | Medium: needs a record field (format bump) and the FAT signature to be robust | ~150 lines | later (0.9.0) |
| S5 | Order of art work | Already in place: on-screen rows and Now Playing first (`hint_add()`/`run_hints()`), then the panel set. With S3 first for synced tracks (Q1), whole-library covers arrive in minutes, not hours [estimate] | — | — | nothing new |
| S6 | **Tags from Apple's track database** instead of opening each file | after S2, ~1.2 ms of tag reads per track [hypothesis] + parse CPU: 5.4k ~7 s+, 50k ~1 min+ | **High**: a stale database lists files that changed or vanished; it lacks fields we index (ReplayGain, chapters, embedded-art flag, our classifier's inputs); playback still parses the file | large | later, maybe never |
| S7 | Duplicate parses: the builder re-opens and re-parses each album's first track (`source_decode()` → `get_metadata()`) after the scan already did | 1 open + ~3 reads per album: 5.4k ~1 s, 50k ~10 s [model] | Low, but needs an embedded-art flag kept per track by the scan (a tagcache field) | medium | later |
| S8 | A directory-lookup cache in `fat.c` (parent cluster + name → entry, ~64 entries, ~2 KB) instead of more slots | S2's gain for ~2 KB of RAM instead of 96 KiB [estimate: bounded by the 8-slot row] | Medium: upstream-shaped core code; invalidation on every rename, remove and USB | ~250 lines | later (0.9.0), and it would let S2 drop back to 2–4 slots |
| S9 | Ignore-file lookups (`database.ignore`/`.unignore`, 2 per folder) | synced: 51 folders, negligible | — | — | no |

S1 + S2 together cut the first run's storage work at 5,400 tracks from about **69 s to about 0.3 s**, plus tag reads, and at 50,000 from about **16 min to about 1 s**, plus tag reads [model]. The CPU parts stay: scan parse, commit, and decodes.

## 4. S1 and S2 in detail

### 4.1 S1: no image-file search under `iPod_Control/Music`

- In `artcache.c` `find_file()`: if `afb_in_music_dir(w->path)` (already exists, case-insensitive, `afb_core.c`) and the track is **not** in our link folder (`afb_in_link_dir()`: never indexed anyway), return `false` without calling `search_albumart_files()`. `png_folder()` returns `false` for the same paths. `poster_file()` (video posters) is unaffected: videos are never under `Music/Fnn` in the synced layout [hypothesis: a synced video library also uses `Fnn`; then it simply gets no poster file, as today].
- This changes nothing in `apps/recorder/albumart.c`, so the stock firmware is untouched (no upstream edit).
- `source_unchanged()` and `source_sig()` call `find_file()` too. All three skip, so the post-change sweep (§2.3) costs nothing for synced tracks.
- **Expected:** the art pass's `exists=` drops from 11,373 to ~33 at 5,400 tracks (the pass's other lookups). The test script asserts it (`tests/perf/ipodctl-first.script` extended with an `expect` on the trace).
- **What the user loses:** a cover image file a user drops into an `Fnn` folder by hand is not found. That is unreachable in practice: the folders are hidden by the sync software and full of meaningless names.

### 4.2 S2: burst slots

- `firmware/common/fat.c` `FAT_DIR_BURST_SLOTS` 2 → 8. The slot buffer is one pinned `core_alloc` at the first mount (`dir_burst_init()`), so this is +96 KiB pinned, once, taken from the audio buffer. At 4 slots it is +32 KiB.
- **Budget check (needs the budget-reviewer):** the 0.8.0 floors measured 11.6 MB (32 MB unit) and 45.2 MB (64 MB unit) minimum against the 7.5 / 39 MB floors (`docs/hardware.md`, P16 gate fix row). The question is the resident cap of 1.5 MB (memory budget floors), which this brief has not re-measured: ⚠️ Q2.
- Correctness is covered by the existing harness: `tests/fatcrash/run.sh`'s `dirburst mut/fault/usb` modes run with any slot count. Add 8 to its geometry loop.
- The LRU victim search is linear over the slots (8 compares per miss): negligible.

## 5. S3: covers from Apple's artwork database

### 5.1 What the files hold [public-doc]

- `iPod_Control/Artwork/ArtworkDB` is a chunk tree like the track database. All integers are little-endian. `mhfd` (header 0x84) holds three `mhsd` (0x60). Index 1 is the image list (`mhli` 0x5c, count at +8) of `mhii` records. Index 2 is an album list, which is empty here. Index 3 is the file list (`mhlf` → `mhif` 0x7c: format id, image size).
- An `mhii` (header 0x98) holds its id at +16 and, at +20, the **8-byte track id** (`dbid`) of the track it belongs to. Its children are `mhod` type 2 containers (header 0x18, type at +12). Each container holds one `mhni` (0x4c). The `mhni` gives the format id at +16, the byte offset into the image file at +20, the image size at +24, vertical and horizontal padding at +28/+30 (signed 16-bit), and height and width at +32/+34. Under the `mhni` is an `mhod` type 3 with the image file's name (UTF-16LE, `:F1028_1.ithmb` style).
- `F<format>_<n>.ithmb` holds fixed-size images back to back, so slot = offset / size. On the 5G/5.5G the cover formats are **200×200 (80,000 B) and 100×100 (20,000 B), RGB565, little-endian, row-major, no row padding**. The ids differ between sources (1028/1029 against 1027/1031), so **ids are never trusted**: the reader goes by the `mhni`'s size, width and height and takes the file name from the string.
- The track database (`iPod_Control/iTunes/iTunesDB`) links a track to its `dbid` (`mhit` +112) and its location (`mhod` type 2, UTF-16LE, `:iPod_Control:Music:F23:ABCD.mp3`). `mhit` +124 is an artwork count and +164 a has-artwork byte (1 yes, 2 no; version 0x0d on). Late-2007 players also link through `mhit` +352 (the `mhii` id, headers ≥ 0x184 only). For a 5G database we join on `dbid`, and on +352 only when the header is long enough and the field is non-zero.
- The 5G's database is plain (no compression, no hash) [public-doc, implied: hashes start with the Classic and nano 3G]. Our own writer proved version 0x13 on silicon (`docs/research/itunes-mode.md` §8).

### 5.2 Which track database

`afb.c` replaces Apple's `iTunesDB` with our own once ProkBox has run, and keeps a foreign one, written by the sync software, once as `<hidden folder>/afb/iTunesDB.orig` (`afb.c` ~l. 484). The artwork database's `dbid`s belong to the **foreign** database. The reader uses:

1. `iPod_Control/iTunes/iTunesDB` if afb's state says it is not ours;
2. else the kept `iTunesDB.orig`;
3. else no synced covers (logged once: `art syncdb none`).

⚠️ **Risk:** a user who re-syncs with Apple's software after ProkBox has run gets a new foreign database and a new `ArtworkDB`. afb then keeps the new one only if no `.orig` exists (`afb.c`: "kept once"). The reader must compare the `ArtworkDB`'s own track ids with the database it picks; a mismatch rate above 50 % means the wrong database, and the reader then stops. The afb owner decides whether `.orig` should follow a newer foreign database (a separate issue).

### 5.3 The map (no browse-time parsing)

Built by the art builder thread at the start of a pass, only when the inputs changed:

1. Its inputs are the album table's representative paths, which the builder already has. Their FNV-64 hashes are upper-cased first, since FAT names are case-blind. It sorts them in a transient block of 16 B per album: 8.6 KB at 540 albums, 80 KB at 5,000.
2. One streamed pass over the track database's type-1 set (`mhlt`/`mhit`/location `mhod`) turns a matching location into its `dbid`. Then sort by `dbid`.
3. One streamed pass over `ArtworkDB`'s image list resolves a matching `dbid` to the best image: the largest square-or-padded RGB565 image with `size == w×h×2`, `w, h ≤ 240`. That rejects the 720×480 UYVY TV-out format by size. The result is file index, offset, size, w, h and padding.
4. Write `.rockbox/artcache/syncart.map` (8.3 name): a header with each input's size and a CRC of its first and last 8 KB, then the sorted 24 B entries `{path hash 8, file index 2, w 1, h 1 (÷2: up to 510 px), offset 4, size 4, pad v/h 2+2}`. Writing goes through `safefile_replace()`, and a file table follows the header (the `F*.ithmb` names, at most 16).

**Cost [estimate]:** the track database at ~1–1.5 KB per track, so 5–8 MB at 5,400 tracks (0.3–0.4 s at 21 MB/s) and 50–75 MB at 50,000 (2.5–3.6 s). `ArtworkDB` ~0.5 KB per image. The walk is mostly skipping, with one FNV per location string. 64 KB streamed reads go through a `core_alloc` buffer, or through the art builder's existing decode block, which is free between decodes. **RAM:** transient only, ≤ 24 B per album (≤ 120 KB at 5,000 albums) plus the read buffer. Allocated only in the builder's maintenance window under the same rules as its decode block (device-reality 2). Nothing permanent.

### 5.4 The source

- New `enum artsrc_kind` value **`ARTSRC_SYNCDB = 3`** (`apps/ipodui/artfmt.h`; the name is neutral, rule 2). The record's fields: `src_hash` is the FNV of the image file's name, `src_size` the image bytes, `src_crc` the sig2 CRC of the image bytes, and `src_type` a new `AA_TYPE_RAW565` in the shell's own range.
- **Decode:** read the image's rows (400 B per row) straight into `resize.c`'s row callback, which takes 565 in and gives ARGB rows out, through the existing fill, crop, fit and dither code. The content rectangle is the `mhni`'s w×h inside the stored pixmap, offset by the padding: [public-doc], to be checked on a real database. No JPEG overhead: the 98,588 B decode block is larger than needed.
- **Byte order:** RGB565 LE should equal `fb_data` on this target byte for byte [hypothesis]. The host test checks it against a reference image: the device bench row (§9) shows a known cover side by side with its embedded original.
- **Size:** at most 200 px. Thumbnails (55), Cover Flow (128) and Now Playing (142 box) only downscale. **Panels (300×300) upscale 1.5×** and look soft. See Q1.
- **Validation:** offset + size ≤ the image file's length; size = w×h×2; w, h ≤ 240; the string is a plain `F<digits>_<digits>.ithmb` name (no path separators). Anything else is `ARTREC_NOART` for that album, with one log line per pass, never a splash.

### 5.5 Source order (⚠️ Q1)

- **Default (the issue's order):** for a synced track, embedded art first, then the synced database. No image-file search (S1).
- **Option B:** synced database first for the sizes up to 142 px, which are every album's thumbnail, Cover Flow and Now Playing image; embedded first for the 300 px panels of the panel set (32 + pool) only. Saves 2 JPEG decodes per album that has both sources: at 5,400 tracks, 163 albums × 2 × 0.5–12 s ≈ **3 min–1 h** of CPU on the first pass [model], and the same on every Rebuild. Cost: a second record source for the panel, which the record has no field for, so it needs a flag bit, and a cover that may differ between the panel and the list if the user changed the embedded art since the last sync.

### 5.6 Invalidation

- The map is rebuilt when either input's size or CRC header changes. A rebuild sets the art index's `sweep_commit` to 0, so the next pass re-checks every album that has a synced source (one map lookup each, no file I/O beyond the map), plus every NOART album under `Music`: a new sync may have added covers.
- A record with `ARTSRC_SYNCDB` re-signs from the map entry (size, CRC of the image bytes). A cover replaced at the same offset is caught by the CRC.
- **Rollback:** an older build reads a kind-3 record as "not a file source, unchanged unless a file appears". With S1 absent there, it re-searches and finds nothing, so the record is kept. No harm, no wipe.

### 5.7 Sources (format facts only; clean-room)

- iPodLinux wiki, "ITunesDB" (ArtworkDB section): http://www.ipodlinux.org/ITunesDB/ (read from a saved copy of that page: https://github.com/raleighlittles/iTunesDB-Parser/tree/main/itunesdb_docs).
- Album-art format ids for the iPod Video: https://jvgomg.github.io/podkit/devices/ipod-internals; artwork corruption (offsets past the file end): https://jvgomg.github.io/podkit/devices/artwork-corruption.
- Image sharing and native pixel format: https://keyj.emphy.de/?p=45.
- No GPL or LGPL implementation source was read (libgpod is LGPL; only its prose README was read, for the device-generation table).

## 6. Contracts (committed before any code)

```c
/* apps/ipodui/artfmt.h */
enum artsrc_kind { ARTSRC_NONE = 0, ARTSRC_FILE = 1, ARTSRC_EMBEDDED = 2,
                   ARTSRC_SYNCDB = 3 };

/* apps/ipodui/syncart.h: Apple's artwork database as an art source */
struct syncart_ent {            /* 24 B, sorted by path_hash */
    uint64_t path_hash;         /* FNV-64 of the upper-cased track path */
    uint16_t file;              /* index into the map's file table */
    uint8_t  w2, h2;            /* width / 2, height / 2 */
    uint32_t offset, size;      /* in that .ithmb */
    int16_t  pad_v, pad_h;      /* the mhni's padding */
};
/* Builds or confirms .rockbox/artcache/syncart.map for these albums'
 * representative paths; false: no usable database (logged once). */
bool syncart_prepare(const char *const *paths, int n,
                     bool (*pace)(void));
/* The image for a track; false: none. Reads the map only. */
bool syncart_lookup(const char *path, struct syncart_ent *out,
                    char *file, size_t file_len);
```

A host test (`tests/syncart/`) builds a synthetic `ArtworkDB` + `.ithmb` + track database from the public layout. The fixture is ours: a script writes the chunks from §5.1's tables. It round-trips the map and checks every bound in §5.4, including truncated chunks, offsets past the end, wrong sizes, UYVY-sized entries and a missing `Artwork` folder.

## 7. Device-reality checklist

1. **Compiled features:** dircache is compiled out of every shell variant, so every lookup is a FAT walk, which is the [fat] model. S2's define is the same in the sim, but the sim has no ATA layer: only the harness and the device show its effect. The burst code is `HAVE_IPOD_UI && !BOOTLOADER` only.
2. **Memory:** S1 none. S2 +96 KiB (or +32 KiB) pinned at the first mount, before playback, so it shrinks the audio buffer once and never grows back; the budget-reviewer checks the floors and the resident cap. S3 is transient, ≤ 120 KB + a 64 KB read buffer, taken only in the builder's window with the same check-then-allocate rule as its decode block (no yield between the `audio_status()` check and the allocation), and freed before the pass goes on. While audio plays (paused counts), the map build waits like the decode does.
3. **CPU:** the map build walks ≤ 75 MB of database at 50k with one FNV per location string; its device time is unmeasured. The host test times the parser on a synthetic 50k database, and that time ×200–400 is the estimate the implementation must report before it merges. It yields through the builder's pace points (`ART_PACE()`) and runs on the builder thread, never the UI thread.
4. **Storage:** S1 removes 21 lookups per album per first pass and 19 per album per changed sync. S2 removes ~4 commands per open. S3 adds 2 streamed reads of the databases per map rebuild (only after a sync changed them) and 1–2 reads per album built.
5. **LCD:** none.
6. **Threads and locks:** everything runs on the art builder thread. The map is read only by that thread, with no UI-thread access. S2 changes nothing in the disk-cache lock (`docs/storage-choices.md` row on bursts: the lock is held across a burst; more slots means fewer bursts).
7. **Upstream splashes:** none. Errors are logged (`art syncdb ...`).
8. **Persistence:** the map is written with `safefile_replace()`. A power cut leaves the old map or none, and the next pass rebuilds it. USB mid-build stops the pass at its next pace point (existing), and the map is re-validated against its inputs at the next pass.
9. **Scale:** 5,400 and 50,000 measured above. The map at 50k is ≤ 1.2 MB on disk (all tracks) or 120 KB (album representatives only, the default).
10. **Deferred work:** the map build waits for the builder's window in the *ready*, *fresh* and *dirty* database states alike, because it keys on the album table the builder already has. Scripts: `ipodctl-first.script` covers fresh; `-ready` (a second boot) and `-dirty` (a cut commit) variants are to be added. The user sees the no-art glyph until the album's cover is built, exactly as today.
11. **Stock firmware unchanged:** S1 and S3 are `apps/ipodui/` only. S2 changes our own `fat.c` define, which sits under `HAVE_IPOD_UI && !BOOTLOADER`, so the stock image is byte-identical.
12. **Existing paths doing the same job:** `search_albumart_files()` (the stock search, still used for every other path), `find_albumart()` (playback's AA slots, which the shell doesn't claim), and pictureflow's own cache (a plugin, not shipped). afb (`apps/ipodui/afb*.c`) owns `iPod_Control/iTunes` and keeps the foreign database. S3 only reads it, never writes, and never touches `iPod_Control/Artwork`.

## 8. Recommended cut

**0.8.1:**
- S1 (the probe skip).
- S2 at 8 slots if the budget-reviewer clears +96 KiB, else 4. At 4 the 5,400-track user gets the full gain and a 50k user little.
- S3 with the default order (embedded first).
- Host tests and the sim scripts here.
- A bench row on a real synced library (§9).

**0.9.0:** S4b (re-search only changed folders, for every library), S8 (directory-lookup cache, then S2 back to fewer slots), S7 (one parse per album), Q1 option B if the owner wants it.
**Later or never:** S6 (tags from the track database).

## 9. Acceptance and checks

- **Sim:** `tests/perf/ipodctl_sim.sh 5400` shows the art pass's `exists=` at ≤ 40, down from 11,373, and the post-change pass's at ≤ 40, down from 10,288. Before S1, the same script must fail its assertion (the gate's red run).
- **[fat]:** `tests/perf/ipodctl_fat.sh` with the new default slot count shows the open rows at the 8-slot column. `tests/fatcrash/run.sh` stays green with 8 slots (`mut`, `fault`, `usb`).
- **Host:** `tests/syncart/` round trip plus every malformed case; a synthetic cover comes out pixel-identical to its source at 200×200 → 128×128 against a reference scale.
- **Device (bench row, b10/b11):** a 5.5G with a library synced by Apple's software, at least 1,000 songs. Rows: `art build done` lines with `exists=` (device: the count of lookups) and `ms=`; `art syncdb map n= ms=`; the first-run time against a run on 0.8.0's image; a side-by-side photo of one album's list thumbnail from the synced database against the same album's embedded cover (byte order, padding); one re-sync from the computer, then the map rebuild and the cover change. A hard-drive unit, when one is available, for the seek cost the model leaves out.

## 10. Hardware features and state of the art

- **Hardware:** the video chip's JPEG decode (#219, P19 `J_OP_DECODE`) is the path for embedded covers and is not changed here. S3 removes the decode for synced covers altogether, which is cheaper than any decoder. PP DMA does not apply, because storage reads already go through ATA DMA (UDMA2). The COP is not used: the builder is I/O-bound once S1/S2 land.
- **State of the art:** directory lookups are served by name caches in every modern kernel (a dentry cache), which is S8 here. More slots is the cheap interim. For resizing, area averaging (`resize.c`) is the right method for 200 → 55/128/142 (`docs/algorithms-survey.md` §3 "Downscale"). The 1.5× upscale to 300 uses `resize.c`'s linear path. A better upscaler (Lanczos) is not worth it on a 565 source.

## 11. Risks

- **Wrong track database** (§5.2): covers on the wrong albums. Mitigated by the match-rate check; the bench re-sync row tests it.
- **Format variants:** other writers (third-party sync tools) may leave stale offsets (podkit's corruption note). Every entry is bounds-checked; a bad one becomes NOART, never a crash.
- **Panel quality** from 200 px sources (Q1).
- **S2's memory:** +96 KiB pinned, permanently.
- **Unmeasured hard-drive cost:** the model is flash-only.

## 12. Not verified

- Device times: every [model] row is counts × a flash cost measured elsewhere. The HDD cost is not modelled.
- Tag reads per track in the scan (assumed ~3).
- The CPU share of the scan's host time (the ×200–400 factor overstates it).
- The ArtworkDB facts marked [public-doc]. No real `ArtworkDB` was available to this session. Still open: byte order, padding semantics, which format ids a 5.5G gets, whether tracks of one album share one `mhii`, and whether iTunes 7+ writes the +352 link on a 5G database.
- The 50,000-track post-change art pass in the sim (§2.4: it outran the one-hour script limit).

## 13. Owner questions (each answerable on its own)

- **Q1.** For synced albums that have both an embedded cover and one in Apple's artwork database: use the database's ready-made 200 px image for lists, Cover Flow and Now Playing (saves a JPEG decode each, minutes to an hour on a first run), and the embedded cover only for the large main-menu panel? Default: embedded first everywhere, as the issue says.
- **Q2.** Directory burst slots: 8 (+96 KiB of RAM, full gain at 50,000 songs) or 4 (+32 KiB, full gain up to ~10,000 songs)? Default: 8 if the budget review passes.

## 14. As built (0.8.1, `p081-ipodctl`)

### 14.1 Owner decisions (issue #225, 2026-11-08)

- **Q1 yes.** For a synced track the cover database's 200 px image feeds the list thumbnails, Album Flow and the Now Playing screen; embedded art (decoded) feeds only the panel-sized images: the main menu's 300 px panels and, by a later answer (owner, 2026-11-08), its **Now Playing row's cover** (`npcover.pak`): the track's embedded art when it has some, else the synced 200 px image. `tests/perf/ipodctl-np-both.script` (embedded wins; red on the earlier rule, which took the synced image) and `-np-synced.script` (synced only), both through an album's second track.
- **Q2: 8 burst slots**, subject to the budget review. Re-measured at 65k below; the slot count stays at 8 (owner), and §14.4 asks the follow-up question.
- **Stress ceiling 65,000 songs** (was 50,000).
- **ProkBox is read-only toward `iPod_Control`.** The cover writer for the stock firmware (S4) and turning the stock-firmware library writer (#137) on by default were proposed and **dropped**. #137 stays a developer-only experiment, off by default. The format module has a reader only; the synthetic test databases come from a test-only generator in `tests/syncart/` (never linked into the firmware).
- **A Build Artwork / Rebuild Cache pass stays awake on the charger** (no idle power-off while external power is in; the backlight still times out); on battery the normal idle power-off stays, and the pass resumes at the next power-on. Already today's rule (the idle timer's charger hold in `handle_auto_poweroff()` and the shell's `sleep_tier_pick()`), now pinned by `tests/ui/ipodui-art-maintenance-charger.script` and `-battery-off.script` (`docs/design/spec/music-lists.md`, `docs/hardware.md` "Idle power-off on external power").

### 14.2 S2 at 65k [fat]

`tests/perf/ipodctl_fat.sh 5400 65000` (cold disk cache, 16 KiB clusters; device time from the §2.1 flash model: 0.41 ms per command + 21 MB/s).

| Opens of every track | 2 slots (0.8.0) | 8 slots (0.8.1) | 12 slots | 16 slots |
|---|---|---|---|---|
| synced, 5,400 | 21,600 cmd / 25.7 s | **50 / 0.1 s** | — | — |
| synced, 65,000 | 393,487 / 7.8 min | **11,632 / 14 s** | 289 / 0.3 s | 289 |
| tree, 5,400 | 21,600 / 25.7 s | **1,080 / 1.3 s** | — | — |
| tree, 65,000 | 588,451 / 11.7 min | **418,547 / 8.3 min** | 176,523 | 13,055 / 16 s |

The art probes (synced 65k: 805,990 → 552,251 commands) no longer run for synced tracks (S1). Why 8 slots fall short at 65k: a synced folder then holds 1,300 names (3 clusters), so an open's path needs about 6 clusters and the 50 `Fnn` folders take turns evicting each other. In the tree, `/Music` itself spans many clusters and a lookup walks them up to the artist.

**Memory:** one pinned `core_alloc` at the first mount, 131,104 B (+98,304 B over 2 slots); `_end` unchanged (1,453,728 B, `fw-ui`). The simulator has no `fat.c`, so the floor gate can't see it: device estimate ≈ 11.05 / 43.88 MB (P12's device estimate less the `_end` growth since and these 98,304 B; budget review M2) against 7.5 / 39 (`docs/hardware.md`, "Directory burst slots").

**Harness:** `tests/fatcrash/run.sh` runs its burst tests with 8 slots and with a 2-slot build (with 8, its small folders never evict). Its fault test now starts each case from a remount: with 8 slots the folder stayed in a slot through `dc_empty()`, and the injected fault was never read (the red run that exposed it).

### 14.3 The cover database, checked on a real file

A real cover database from a later model (the owner's stock unit; read locally with `tests/syncart` `test_artdb --survey`, nothing kept, no ids, names or pictures recorded) confirmed the chunk layout of §5.1 and corrected one assumption:

- Chunks and header lengths as §5.1: `mhfd` 0x84 (version word 2, 3 children), `mhsd` 0x60 ×3, `mhli` 0x5c, `mhii` 0x98, `mhod` 0x18, `mhni` 0x4c, `mhla` 0x5c, `mhlf` 0x5c, `mhif` 0x7c. About six `mhni` per `mhii` (one per format), no two images naming the same slot.
- **The `mhni`'s width and height are the picture, not the slot.** The padding is the rest of the slot, and a slot's rows can be wider than the slot: 55 × 55 pictures stored in a 56-pixel stride (6,160 B); a 48-wide picture with `pad_h` 7 in the same slot. The brief's rule `size == w × h × 2` would have dropped every non-square cover. The reader now takes slot = picture + padding (padding ≥ 0), size = whole rows, stride = size / (2 × slot height) ≥ slot width (`artdb_img_geom()`), and the art builder shows the whole slot, padding included (how the padding splits between the sides is not known). That model's formats were 128 × 128, 56 × 56 (55 × 55 in a 56 stride), and 320 × 320, which the reader skips (sides over 240).
- **Byte order: RGB565 little-endian.** Read as LE, horizontally adjacent pixels differ by 38 and 50 on average (two formats, 40 images each); read as BE, by 151 and 170.
- Not in that sample: a song database (only the `Artwork` folder was kept), so the `dbid` link and the +352 link are still [public-doc]. The 5G's own format ids (1028/1029), its sizes and its padding remain [hypothesis] until a 5.5G's synced library is read (§9 bench row).

### 14.5 S1 and S3 as built: measured and tested

**5,400 tracks** [sim], `tests/perf/ipodctl_sim.sh 5400` at the integrated head (2026-11-09):

| | Brief (before) | As built |
|---|---|---|
| Image-file lookups, first art pass | 11,373 | **0** |
| Image-file lookups, pass after one song added | 10,288 | **0** |
| Albums with a synced cover in the map | — | 379 of 540 (the fixture's 70 %) |
| Map build, host | — | 20 ms first, 40 ms after the change (×200–400: ~4–16 s on the device [model]) |
| Map on disk (`syncart.map`) | — | 9,608 B |
| Synced thumbnails holding the fixture's pixels (`tests/syncart/check_thumbs.py`) | — | 379 of 379 |

**65,000 tracks** (fork B's run on `p081-ipodctl-art`; figures from its report, not re-run here): map working memory about 250 KB, transient, in the builder's window only (never while audio plays or is paused), against the floors in `docs/hardware.md`; shell code +13.1 KB. The budget review checks both.

**Stack** (`scripts/risk/stack_usage.sh` build, `scripts/risk/lib/stack_depth.py build/risk-stack-fw art_thread syncart_build syncart_lookup`): the art thread's worst chain is 6,008 B (through a synced decode into the image reader and the storage driver), plus the 400 B yield tail, of its 12,288 B stack (52 %). The map build alone reaches 3,248 B (`syncart_build` 1,208 B frame → `artdb_songs` 1,144 → its string decoder 568), a lookup 2,848 B; both sit inside that worst chain.

**Tests** (each shown red first):
- `tests/syncart/run.sh`: the reader (179 checks: round trip, damaged input, bad images, padded slots, the song walk, fuzz) and the map (35 checks). Padded slots: red before the geometry fix (6 failed).
- `tests/perf/ipodctl_sim.sh`: S1's lookup count on every art pass (red on 0.8.0's code: 11,166 / 10,281); the synced thumbnails' pixels.
- `ipodctl-np-both` / `-np-synced` (300 tracks `--front`): Now Playing's cover through an album's second track; red on the earlier rule (synced image for an album with embedded art) and with the by-album lookup broken (no synced cover reached Now Playing).
- `ipodctl-resync` (1,000 tracks `--resync-root`): a re-sync's owed re-check survives a pass cut after 5 albums; red with the owe file ignored (44 of 72 synced thumbnails kept the first sync's picture), green 72 of 72.
- `tests/ui/ipodui-art-maintenance-charger` / `-battery-off`: §14.1.

### 14.4 Owner question (open, for the morning)

- **Q3.** Directory burst slots at 65,000 songs: 8 slots (as decided, +96 KiB) open a synced library's every track in 11,632 commands (about 14 s on flash); 12 slots (+160 KiB in all, +64 KiB more than 8) make it 289 (0.3 s). The drag-and-drop tree barely gains from either (8.3 min at 8; 16 slots would be needed). **Recommendation:** keep 8 for 0.8.1 and build S8, the directory-lookup cache (parent folder + name → entry, about 2 KB; brief §3), for 0.9.0. It serves both layouts at any size, and would let the slots go back to 2–4 (returning 64–96 KiB). Default if unanswered: 8 now, S8 in 0.9.0.
