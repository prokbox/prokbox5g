# Research: keeping Apple's database in step with ProkBox ("iTunes mode")

Status: research note, 2026-11-05. Not a brief, not a commitment. Sources are cited inline; items marked **[unverified]** come from memory or from one secondary source and must be checked on a spare 5G or against a real iTunes-written database before design depends on them.

## 1. Verdict

- **Priority (owner, scope update):** ProkBox to Apple. After music is added the ProkBox way, Apple's database should list every file Apple's 1.3 firmware can play, so a user who fails back (hold Menu at boot) still has their songs. Everything else is secondary.
- **Feasible, and small if kept narrow.** The 5G/5.5G database format is old and unsigned. No checksum or hash is needed. This is confirmed by two independent sources: the pygpod model table lists "iPod Video 5G/5.5G: read, write, checksum none" ([pygpod on PyPI](https://pypi.org/project/pygpod/)), and the podscribe README targets the 5.5G (A1136) and says "no hash58/checksum" ([menor/podscribe](https://github.com/menor/podscribe)). Hash58 starts with the nano 3G/4G and Classic.
- **A correction to the brief's wording.** On the 5G the folder is `iPod_Control` (music in `iPod_Control/Music/Fxx`, database `iPod_Control/iTunes/iTunesDB`, play counts `iPod_Control/iTunes/Play Counts`). `iTunes_Control` is the later (6G/Classic/touch) name. The repo already uses `iPod_Control` in `docs/fat32-migration-plan.md`.
- **Do the work off the device's browse path.** Write the database at sync time (after the USB scan, on disconnect), as a streamed, atomic rewrite. Do not parse iTunesDB at browse time and do not load it into resident RAM. The writer needs only a small working buffer.
- **No existing project to adopt as code.** No Rockbox fork or plugin reads or writes iTunesDB (see section 3). The best references are documents and PC-side libraries.
- **The real risks are not the format.** They are (1) the `Play Counts` file, whose entries refer to tracks by position, so any rewrite of the track list can misattribute plays; (2) how Apple's firmware behaves with very large libraries and with files at paths it did not create [unverified]; (3) what iTunes does when it next meets a database it did not write.

## 2. What the user sees on the Apple side

Apple's firmware lists only what iTunesDB lists; it ignores files it is not told about. So the writer's job is exactly: for every file ProkBox knows that is MP3, AAC/M4A, ALAC, WAV or AIFF, make sure there is a track record and a master-playlist entry. FLAC, Ogg, Opus, Musepack, WavPack, APE and the rest are skipped; Apple's view of them is empty by design. Audiobooks (m4b) can be listed too, but should be flagged so Apple's firmware shelves them as audiobooks [unverified: media-type flag values].

## 3. Landscape

Licences are as reported by the GitHub API or the project page on the date above. "5G" means iPod Video 5G/5.5G.

| Project | URL | Licence | Language | Scope | State |
|---|---|---|---|---|---|
| libgpod | https://sourceforge.net/p/gtkpod/libgpod | LGPL (COPYING is the GNU Library GPL v2; per-file headers not checked) | C | The reference reader/writer: iTunesDB, ArtworkDB, photos, Play Counts, OTG playlists, hash58/72 for later models. Used by GTKPod, Rhythmbox, Strawberry, Amarok | Last source change 2014-08; frozen but format-complete for the 5G. Mirrors: https://github.com/fadingred/libgpod |
| pygpod | https://github.com/Bionded/pygpod (https://pypi.org/project/pygpod/) | LGPL-2.1 (or later) | Python 3.8+ | Successor to libgpod: read/write iTunesDB and ArtworkDB, journalled crash-safe saves, lock file, `doctor` consistency check, backup/restore, play-count and rating merge, byte-identical re-write of iTunes-written databases. 5G: read/write, no checksum. Only Classic hardware-tested; others tested against libgpod | Beta, v0.2.3 released 2026-10-26, one maintainer, 4 stars. **Best behavioural reference** (its save protocol and `doctor` rules) |
| iOpenPod | https://github.com/TheRealSavi/iOpenPod | GPL-3.0-or-later | Python, PySide6 | Full PC-side sync tool with its own iTunesDB parser and writer, 5G through nano 7G and Classic; FLAC to ALAC; ratings and scrobbling | Active (pushed 2026-11-05), about 500 stars. GPL v3 only mixes with GPL v2-or-later code if the whole becomes v3, so **read, do not copy** |
| podscribe | https://github.com/menor/podscribe | MIT | TypeScript | Writes only the old (no-hash) iTunesDB for the 5.5G A1136, "reimplemented from the documented format": tracks, playlists, audio. Backs up the old DB, fsyncs, fails loudly. States the pitfall that a track must be in the master playlist or the old firmware will not show it | Pre-v0.1, 1 star, 2026-10. **Closest to our exact need**; its own README says to verify against a real iTunes-written DB before trusting it |
| itdb-rebuild | https://github.com/jyelloz/itdb-rebuild | Mixed (MIT/BSD/Apache texts plus COPYING) | Python | Rebuilds an iTunesDB from the files already on an iPod. Same problem shape as ours | Small (8 stars), updated 2026-01 |
| iTunesDB-Parser | https://github.com/raleighlittles/iTunesDB-Parser | none declared | Rust | Read-only dumper including Play Counts; links the wiki docs | 43 stars, 2026-09 |
| itunesdb-rs | https://github.com/stuffandahalf/itunesdb-rs | GPL-3.0 | Rust | Parse and create | Dormant (2024) |
| itunesdb-ts | https://github.com/brandon-james-dev/itunesdb-ts | MIT | TypeScript | Read and write iPod database files | New (2026-10), tiny |
| iTunesDB.Net | https://github.com/Threetwosevensixseven/iTunesDB.Net | MIT | C# | Read/write | Dormant (2022) |
| libitunesdb2 | https://github.com/MCJack123/libitunesdb2 | none declared | C++ | Parser for the latest format | Small, 2026-04 |
| LastPod | https://github.com/christilden/lastpod | none declared | Java | Reads `Play Counts` and scrobbles | Old but updated 2025 |
| ZenPod | https://github.com/ishiichan-dayo/ZenPod | none declared | n/a | Manager for "iPod OS and Rockbox" Classic users, no iTunes | Active (2026-11); evidence of the same demand |
| saltpod | https://github.com/atrouwee/saltpod | "Other" | Python | In-place iTunesDB edit and hash58 signing (Classic) | New, tiny |
| iPodLinux "ITunesDB" wiki | http://www.ipodlinux.org/ITunesDB/ (the host's TLS certificate was invalid when fetched; try the Wayback Machine or a mirror) | Wiki text | docs | The original reverse-engineering notes: chunk tree, header fields per version, Play Counts layout | Historic, the standard reference. Introductory version: https://www.linuxjournal.com/article/6334 |
| Rockbox itself | n/a | GPL | C | **Ignores iTunesDB.** It builds its own database from tags (confirmed in a 2007 thread: https://mail.kde.org/pipermail/amarok/2007-May/003485.html). No plugin, patch or Gerrit change found by web search | The only prior art on the Rockbox side is PC tools: foo_pod can rebuild an iTunesDB so one set of files serves both firmwares (https://hydrogenaud.io/index.php?msg=400841) |
| RockPod | https://github.com/nuxcodes/rockpod | not declared by the API | C | Rockbox fork for Classic and Video. Features are MFi audio, Cover Flow and power management; **nothing about iTunesDB found** | Active to 2026-08, 253 stars |
| podbox (`~/Git/ext/podbox`) | local clone | Rockbox GPL | C | Searched the tree by name for itunesdb, iPod_Control, iTunes_Control and itdb: **no source file matches**. The only hit is a doc commit log | n/a. Not run; read-only grep only |

Searches of the web and GitHub found no Rockbox-based project that writes iTunesDB. We would be first.

## 4. Format facts that matter (5G only)

From the Linux Journal article, podscribe's README and pygpod's docs; field-level detail still needs the wiki or libgpod source:

- Little-endian, chunk-based. Every chunk starts with a four-letter tag and a header length, then a total length, so a reader can skip what it does not know.
- Tree: database header (`mhbd`), then data sets (`mhsd`). Set type 1 holds the track list (`mhlt`) with one track record (`mhit`) per song, each followed by string objects (`mhod`) for title, artist, album, genre and the file path. Set type 2 holds the playlist list (`mhlp`) of playlists (`mhyp`) with items (`mhip`) pointing at track ids. (Later versions add a type-3 podcast copy and more; the exact set the 1.3 firmware insists on is **[unverified]**.)
- **The first playlist is the master playlist and holds every track.** A track missing from it does not appear (podscribe README).
- The file path is stored in the database as a colon-separated string rooted at the volume. Whether Apple's firmware plays a path outside `iPod_Control/Music/Fxx` is **[unverified]** and is the first thing to test. If it refuses, the writer would have to place files in `Fxx` itself, which conflicts with drag-and-drop naming and our cluster plans (section 7).
- Each track needs: unique id, size in bytes, length in milliseconds, file-type code, bitrate, sample rate, and the strings above. Dates use the 1904 epoch and the iPod's local time (pygpod docs).
- Sort tables and letter jump tables: pygpod regenerates "the sort indexes and letter jump tables the iPod browses and quick-scrolls by" on save, with libgpod's rules, which suggests the firmware relies on them in the newer versions. Whether the 5G's database version needs them is **[unverified]**; read an iTunes-written 5.5G database to see.
- **Play Counts.** Between syncs the Apple firmware does not touch iTunesDB. It appends plays, skips, ratings and bookmarks to `Play Counts`, with one entry per track in database order; entries are deltas for counts and absolute values for rating and last-played (pygpod docs). The hosting sync tool merges them, then deletes the file. libgpod renames it to `Play Counts.bak` on write (Rhythmbox/libgpod source). Because entries are positional, **a rewrite that inserts, removes or reorders tracks while a `Play Counts` file is pending misattributes plays.**

## 5. Recommended architecture

### 5.1 The writer (the core deliverable; phase A)

A small C module in `apps/ipodui/` (host-testable core in a separate file, like `audiobook_core.c`), run at sync time:

1. **Trigger.** After the post-USB library scan finds changes (the scan already enumerates new, changed and removed files). Never at browse time, never during playback.
2. **Decide the set.** From the scan, the set of Apple-playable files = extension and codec in {MP3, AAC/M4A, ALAC, WAV, AIFF}. ALAC needs a content check, since `.m4a` may be AAC or ALAC; both play, so no check is needed beyond "not DRM-protected" **[unverified: protected AAC would fail on Apple's side too, harmlessly]**. Do not require exact format subsets (some very high-rate WAV or 24-bit files may not play on Apple's side) beyond a cheap sample-rate and bit-depth filter.
3. **Match, don't rebuild.** Keep a tiny side file of our own (not in Apple's folders) mapping each path we have written to its track id. Existing records, including those iTunes wrote, are never edited except for appending or removing whole records. This keeps Apple's artwork references, play counts and ratings intact.
4. **Consume `Play Counts` first.** If the file exists, read it, store the deltas in our own stats (a future "round trip", section 8), and only then rename it to `Play Counts.bak` before rewriting positions. That removes the misattribution risk. Also drop or leave `OTGPlaylistInfo*` files: pygpod notes they too store positions and the Classic empties them after a change.
5. **Streamed copy-on-write.** Read the old iTunesDB sequentially, write a new file `iTunesDB.new` in the same folder, then fsync and rename. Order of the new file: header, track set with old records, then appended new records (ids allocated above the highest existing id, removed records dropped), playlist set with the master playlist updated and user playlists left alone (remove entries for deleted tracks only). Header and list counts and total lengths are patched at the end. The working set is one record at a time, so RAM is a few KB plus an I/O buffer (use `core_alloc`, per the efficiency rules).
6. **Back up.** First time ever, and whenever the old file's checksum differs from the one we last wrote: copy `iTunesDB` to a dated backup in our hidden folder. Keep the last N backups.
7. **Verify before commit.** Re-parse the new file with the same parser: counts agree, every chunk length nests exactly, every mhip id resolves, the file ends exactly at the header's total length. Only then rename. Any failure leaves the old file in place and logs (no on-screen splash, per rule 1).
8. **Power loss.** The rename is the commit point. A leftover `.new` file is deleted at the next run. FAT32 rename is not strictly atomic, so keep the previous file as `.bak` until the new one has been read back once.
9. **Time cost.** About 0.5 to 1 KB per track [estimate from header sizes plus five strings; measure]. A 20,000-song library is roughly 10 to 20 MB; a 100,000-song one is 50 to 100 MB. A sequential read plus write of that at flash speeds is seconds to a few tens of seconds, once per sync, under `cpu_boost` and only when the scan found changes. An unchanged library costs a stat and a hash of the old file, not a rewrite. If the cost is too high for huge libraries, fall back to an in-place append (see 5.3).

### 5.2 Parser
A strict, bounded reader for the same chunk tree, used by the writer and the verifier. It must reject, never guess, unknown versions: if the database header version is newer than we have verified, the writer stops and logs. This also gives a free read-only health check.

### 5.3 Efficiency options, in order of preference
- **Rewrite only on change** (default above).
- **Append in place** when the only change is new tracks: the track list is not the last thing in the file, so this still shifts the playlist section; a trailing-section move of the playlist set is cheaper than a full copy but not atomic. Use only if measurement shows the full copy is too slow. Do not use for removals.
- **Never** keep the whole database in RAM on the device; the 1.5 MB resident cap and the 32 MB units rule it out. A loadable module is not needed because the writer runs once per sync and holds no state between runs.

### 5.4 Artwork
Skip in the first release. Apple's firmware shows generic art for tracks with none, and the ArtworkDB plus `.ithmb` files are the largest and riskiest part of the format (per-model image sizes and a second database). Revisit once the text-only writer is proven; our own album-art cache is a separate system.

### 5.5 Secondary items (brief, not designed)
- **(a) iTunes to ProkBox import.** After an iTunes sync, music appears in `iPod_Control/Music/Fxx` with obfuscated names. Our tag scan already indexes those files by their tags, so the library rebuild mostly works without reading iTunesDB. Reading iTunesDB would add playlists, ratings and play counts. Future.
- **(c) Round trip.** Reading `Play Counts` (already needed in 5.1 step 4) is the cheap half; writing ratings back means editing track records in place. Future.
- If Apple software later syncs the device it may rewrite the database and delete files not in its library. That is the risk in section 6.

## 6. Risks and fail-safe rules

| Risk | Mitigation |
|---|---|
| Corrupting Apple's database | Streamed copy-on-write, verify by re-parse, rename as commit, keep `.bak`, strict version gate, refuse on any unknown chunk we cannot copy through |
| Wrong plays after a rewrite | Consume `Play Counts` before any positional change (5.1 step 4) |
| Apple firmware refuses or hangs on a large or odd database | Test on a spare 5G with 32 MB and 64 MB; start log-only, then write for a small test folder; cap the Apple view if needed (the firmware's track limit and load time are **[unverified]**) |
| iTunes/Finder next sync treats ProkBox-added tracks as foreign, or offers to restore or erase | Expected: sync apps own the database. Our rule: a sync from iTunes wins; ProkBox rescans and rebuilds its side after, and re-adds its entries next time. Tell the user in plain words in the manual. Test on a real Mac with Finder before release |
| Files at paths Apple did not create | First test (section 4). If unsupported, either place a copy or skip, and record in `docs/changes-from-7g.md` |
| Native illusion | The folder is never named on screen (rule 1 and the memory note on hidden folders). **No menu, no options, no "iTunes mode" name anywhere (owner principle: curated, Apple-like, no back-end settings).** Upkeep runs on its own at USB disconnect, invisibly, with errors logged only. A real need for a user switch does not exist: the feature is safe by construction (verify, backup, version gate), and a user who never fails back is not affected. The one switch is a developer-mode flag, expiring and default on, kept for bench work and for the native install type, where the feature is automatically off because there is no Apple firmware |
| Cluster-size plan | The 32 KB cluster reformat (see `docs/fat32-migration-plan.md`) is a different layout from what Apple's firmware formats. Apple's firmware reads FAT32 with its own limits; confirm it still mounts and plays from a larger-cluster volume before promising the fail-back, otherwise this feature applies only to the Apple-compatible install type |
| Native install type | In the native (no Apple firmware) install type the whole feature is moot and must be off |
| Legal | Interoperability with a file format is fine; no Apple code is used. Write from format descriptions and observed databases. Record provenance tags in the code header. |

Staging, per the bootloader-guardrail habit: phase A0 is parse-only plus a "would write" report in the log; A1 writes to `iTunesDB.new` and never renames; A2 renames, with a fresh backup and an owner go.

## 7. What to borrow, what to write

- **Write ourselves (clean room), in C:** parser, writer and verifier. The code runs inside the firmware's tight RAM and boost budget, in a codebase that is GPL v2 or later. A Python or TypeScript library cannot run on the device.
- **Borrow as knowledge, not code:** the chunk and field layout from the iPodLinux wiki and the Linux Journal article; the save protocol (journal, lock, verify) from pygpod; the master-playlist pitfall and the backup habit from podscribe.
- **Borrow as tools:** pygpod's `doctor`/`check` and libgpod itself as an independent oracle in host tests: write a database with our code, load it with pygpod and libgpod, and compare. Run these on the Mac against fixtures, never on the device. Use a real iTunes-written 5.5G database (from the owner's backups, never committed) as the golden reference.
- **Licence check:** pygpod is LGPL-2.1-or-later and libgpod is LGPL; LGPL code may be combined into GPL v2-or-later work, but porting is unnecessary and a clean-room C writer avoids carrying attribution of a different structure. iOpenPod is GPL-3.0-or-later: its code must not be copied into a GPL v2-or-later tree. Do not copy anything from Apple software or firmware. Confirm per-file headers in libgpod before reuse of anything (only the top-level COPYING was checked).

## 8. Phases and effort

| Phase | Content | Effort |
|---|---|---|
| R0 | Read a real iTunes-written 5.5G database and Play Counts file from the owner's backup; list the exact chunks and version the 1.3 firmware expects; run pygpod `check` on it | 1 to 2 days |
| R1 | Host-side C parser and verifier with golden tests (tests/ harness); round-trip byte-identical on real files; fuzz with truncated and bit-flipped inputs | 3 to 5 days |
| R2 | Host-side writer: add and remove tracks, master playlist; compare against pygpod and libgpod output; property tests | 4 to 6 days |
| R3 | Firmware integration: trigger after the USB scan, `Play Counts` handling, backups, log-only report (A0) | 3 to 4 days |
| R4 | Spare-5G device test: custom path, ordering, large library, power-pull test during rename; go/no-go on writing (A1 then A2) | 2 to 3 days plus owner time |
| R5 | Finder/iTunes behaviour test; manual text; ledger entries in `docs/changes-from-7g.md` | 1 to 2 days |
| Later | Artwork; Play Counts and rating round trip; import of playlists from iTunes | open |

Total for the priority item: about three weeks of one agent's work, gated by the device tests in R4.

## 9. Open owner questions

1. May we use a spare iPod Video (not the daily 5.5G) for R4, including destructive tests (power pull, huge database)? A spare 5G with 64 MB and one with 32 MB would both be ideal.
2. May we use a copy of an iTunes-written database from your backups as a private fixture (never committed or mirrored)? Or can a fresh sync of a small throwaway library from a Mac be made?
3. Should the Apple view list audiobooks (m4b) in its Audiobooks shelf, or leave them out?
4. If Apple's firmware cannot play files at ProkBox paths, do you accept copying (or moving) the playable files into the Apple layout, or should the feature be dropped?
5. Confirm: no user-visible switch at all (developer-mode flag only), and the feature simply runs after every USB disconnect that changed the library. Our recommendation is yes, since the writer is a no-op on an unchanged library.
6. How should we behave when the library is far larger than Apple's firmware copes with: write nothing, write a capped subset, or warn the user once at USB eject?
7. Is "a sync from iTunes wins, then ProkBox rescans" the policy you want, with plain-words text in the user manual?

## 10. Silicon results and decision (2026-11-05)

**Silicon (owner unit, Apple firmware 1.3, all 2026-11-05):**
- A minimal database from our own writer (`tools/itunesdb/mkitdb.py`) lists and plays tracks: `mhbd` version 0x13; a type-1 set (`mhlt`, `mhit` header 0x184, media type at 0xd0: 1 audio, 8 audiobook; file type `' 3PM'`, `' A4M'`, `' B4M'`; string `mhod`s 1, 2, 3, 4, 5, 6); a type-2 set with one master `mhyp`, its `mhip`s and type-100 position `mhod`s. No type-3 set, no sort or jump tables, no artwork database [silicon]. This answers section 4's open questions for a small library.
- Apple's firmware plays a file only under `iPod_Control/Music/`: copies in `F00`, in our own folder `PKB`, with long names, and read-only + hidden all played; locations outside `iPod_Control` failed or crashed the player [silicon]. The location string is colon-separated from the volume root.

**Decision (owner, 2026-11-05: no duplicate files):** no copies. One shared-cluster *link* per Apple-playable song in `iPod_Control/Music/PKB`, with the **park cycle** (`docs/research/apple-fallback-links.md`): the links are removed before a computer is given the disk and made again after the unplug. Songs already under `iPod_Control/Music` (a sync app's copies) are listed in place, without a link.

**Built (branch `p19-applefallback`), behind a developer switch, default off until the owner's device trial:**
- the job (`apps/ipodui/afb.c`, `afb_core.c`, `afb_fat.c`) is part of the sync-time indexer module (`MOD_XINDEX`): after the post-USB library scan, in the boot window, or owed (never while playing). It reads the library (no tag parsing), makes the links and a link table in the hidden folder, and rewrites `iTunesDB` only when the song set changed: streamed to `iTunesDB.new` (16 KB buffer), verified by re-parse (strict version 0x13), the old file kept as `iTunesDB.bak`, the rename as the commit. A database we did not write is kept once in our folder (`iTunesDB.orig`).
- **`Play Counts`** is moved into our folder (the one before kept too) before any rewrite, so its positional deltas never meet a rewritten list. Merging them into our statistics is later work.
- the park runs on the USB ack path (`afb_usb_park()`, before `usb_acknowledge`): the `PKB` folder is dropped with its entries; only its own clusters are freed. A power cut anywhere is recovered at the next boot from the state file (`UNPARKING` means start again).
- the music library never indexes a link: `PKB/database.ignore` is written before the first link, and the job skips the folder.
- ProkBox's own `remove()` of a read-only + hidden file inside `/iPod_Control/Music/PKB/` (any case) drops the entry only (`file_internal.c`), in every build; elsewhere it is upstream's.
- costs and test coverage: `docs/hardware.md` ("Apple fallback").

**Open (device):** the park's time on the ack path, the unpark's time at 10,000 songs (flash and hard drive), Apple firmware with a few thousand tracks, and Gate B (Apple disk mode and a Finder or iTunes sync leaving `PKB` alone). Not built: artwork, merging `Play Counts`, keeping iTunes-written records as they are (today every record is ours, iTunes' play counts and ratings stay only in the kept original).
