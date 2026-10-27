# Shuffle over the whole library (#27)

Status: **integrated** (Phase 5, branch `np-p`). The core is `apps/ipodui/shuffle_core.{c,h}` (host-tested by `tests/nowplaying/test_shuffle.c`, including `shuf_group_order()`), the runtime is `apps/ipodui/lib_shuffle.c`, the facade `np_playback.c`. The integration points below are kept as the design record; the deltas from them are listed under "As built".

## Problem
Upstream playlists hold at most `max_files_in_playlist` entries, 2,000 on the PP502x (`settings_list.c`). Today `lib_shuffle_all()` takes a random 2,000-track sample and `lib_play()` takes a 2,000-row window. The 7G shuffles the **entire** library: every song comes once before any repeats.

## Options and their cost at 50,000 tracks

| | Raise the playlist cap | Library-backed shuffle (chosen) |
|---|---|---|
| RAM | `playlist_init()` allocates `indices` (4 B) and, because the device build compiles `HAVE_DIRCACHE` even with dircache off, `dcfrefs` (`struct dircache_fileref`, 12 B) **per slot of the cap**, for the whole session: **16 B/slot**. 2,000 slots (today) = **32,000 B**. 50,000 = **800,000 B** (+768,000 B); 65,536 = **1,048,576 B**. The cap's setting range also stops at 32,000 (upstream edit needed) | **44 B** of iterator state (`struct shuf_songs` / `struct shuf_albums`, measured with `arm-none-eabi-gcc -mcpu=arm7tdmi`), plus the existing 2,000-slot playlist used as a window. No new allocation |
| Start-up | one `tagcache_retrieve(tag_filename)` per track (file names are not in the RAM cache): upstream measures ~10 s per 2,000 on a PP5020, so **~4 min** for 50,000 with the UI blocked, plus `playlist_shuffle()` | 50 file-name reads for the first window (~0.25 s at the same rate), then a refill of 50 every ~3 h of music |
| Flash writes | the control file gets every path (~70 B each): **~3.5 MB per shuffle**, re-parsed by `playlist_resume()` at every boot (seconds) | ~3.5 KB per refill, plus a 48 B resume record |
| Code | none | `shuffle_core.o` **1,284 B** `.text`, 0 data/bss (ARM, `-Os`) |
| Quality | Fisher-Yates over the list (perfect) | keyed Feistel permutation: a true bijection, no repeats until every track has played; at N = 50,000, 0 fixed points and 1 ascending neighbour pair (test output) |

Raising the cap costs most of a megabyte of the audio buffer for the whole session and minutes of blocking I/O per shuffle, and it would still have a cap. The library-backed design costs 44 B and runs in the background.

## The permutation
`shuf_perm_at(p, k)` maps position `k` in `[0, N)` to an item in `[0, N)`:

- a 4-round balanced Feistel network over `4^h >= N` (the domain is < 4N), whose round function is a 32-bit integer hash (two `MUL`s) of the right half XOR a round key. The round keys are derived from the seed.
- **cycle walking**: re-apply the network until the value lands in `[0, N)`. Following a cycle of a permutation from a point inside `[0, N)` back into `[0, N)` is itself a permutation of `[0, N)`, so the map is a bijection for every N (tested at N = 1, 2, 3, 4, 5, 16, 17, 255-257, 2,000, 50,000, 65,536, 65,537 with four seeds).
- **Cost**: measured average and worst Feistel evaluations per lookup: N = 2,000 → 2.05 / 16; N = 50,000 → 1.31 / 10; N = 65,537 (worst case for the domain) → 4.00 / 50. One evaluation is ~60-80 cycles, so a lookup averages a few µs on the PP5022 and stays under ~50 µs in the worst case. Nothing is O(N).
- **Deterministic**: the order is a function of (seed, N). Resume stores (seed, N, lap, pos, first, last), 24 B of it, and the whole struct is plain data.
- **Both ways**: "previous" past the playlist window is position `k - 1`; no inverse is needed.
- **Laps** (Repeat on): lap `L` uses `shuf_lap_seed(seed, L)`, a fresh order. If the new lap would start with the track just played, the next lap seed is taken instead (deterministic, so resume still works).

## Modes
- **Songs**: items are tagcache master index entries `0 .. tagcache_ui_count() - 1` at the time the run starts. The iterator yields entry numbers, and **the caller skips** what isn't music: `tagcache_ui_seeks(e, &flag) == NULL` (deleted) or `!class_ok(C_MUSIC, flag)` (books, podcasts). That is a RAM-only check, and the expected number of skips is `total / music` per track, so a skipped entry costs no I/O and nothing per track is stored. `first` (the song the user picked, or `SHUF_NONE` for Music → Shuffle Songs) plays first and is skipped when the order reaches it in lap 0.
- **Albums**: items are the rows of the precomputed album list (`LIB_ALBUMS` ROWS in `database_lib.dat`, with each album's COUNT). The album order is permuted. Within an album the tracks play in album order (disc, track). The caller supplies `album_len(album)` (the COUNT column; 0 = skip). The iterator yields `(album, track-in-album)`.

## Integration points (Phase 5, owners of `library.c` / Now Playing)
None of this is implemented yet. `library.c` is owned by other agents, so this lists what to add there.

1. **Source.** A queue started from Music → Shuffle Songs, or from Songs with more tracks than the playlist cap, is a *library source*. Every other queue (an album, an artist, a playlist ≤ 2,000) stays an ordinary Rockbox playlist, and the shelf's Songs/Albums act on it with `playlist_shuffle()` / `playlist_sort()` as today. (Shelf "Albums" over a small queue: order the playlist by album groups using `shuf_perm` over the groups.)
2. **Start (`lib_shuffle_all()` and the over-cap `lib_play(shuffle)` path).** Instead of sampling: `shuf_songs_init(&st, tagcache_ui_count(), seed = current_tick ^ entropy, first)`, then `play_begin()`, add the first **W = 50** music entries via `play_add()`, `playlist_start()`. Store the iterator, the source and the database stamp (`tagcache_ui_generation()` + `tagcache_ui_commitid()`) in the shell's playback state.
3. **Refill.** On `PLAYBACK_EVENT_TRACK_CHANGE` (audio thread: only set a flag, never insert from there), the UI thread's idle path checks the tracks left ahead in the playlist. With **≤ 10 left**, it appends the next 50 from the iterator. With ≤ 2 left, it does it at once. Otherwise it waits until `storage_disk_is_active()` so the refill rides an existing wake-up (one wake per refill at most, every ~50 tracks, i.e. ~3 h of music). The playlist is kept under the 2,000 cap by deleting the oldest played entries (keep ~100 of history) with `playlist_delete(…, 0)` in the same batch. Each refill is ≤ 50 file-name reads plus one control-file append, and it yields between entries. **Database states:** *ready*, refill as above. *Rebuilding or RAM cache unavailable* (`tagcache_ui_count() < 0`): no refill, the queued window keeps playing, and the next track change retries. *Dirty or changed* (a new generation or commit id): see item 6. Each state needs a sim script once this is wired (checklist item 10).
4. **Previous track.** Inside the window, the playlist handles it. Before the window's first entry, `shuf_songs_prev()` / `shuf_albums_prev()` gives the entry to insert at position 0 (only within the current lap).
5. **Resume.** The playlist control file already restores the window. The iterator struct (44 B) plus the stamp is saved with the window on every refill (the disk is awake then) in the shell's own state, not in `global_status`, to keep upstream untouched. At boot, if the stamp matches, refills continue the same order.
6. **Library changed** (USB sync, a commit: the generation changes). Entry numbers are meaningless after that. The queued window keeps playing (paths stay valid; deleted files are skipped by playback). The next refill restarts the iterator with the new N and `shuf_lap_seed(seed, lap + 1)`. Songs heard before the sync can come back sooner than a full lap; the 7G reshuffles after a sync too. **Albums** needs the new album list (the libfile is rebuilt by the precompute), so it restarts the same way.
7. **Albums: track lists.** `database_lib.dat` has album rows and counts but no per-album track order. Starting each album with `lib_open(LIB_SONGS, album filter)` is an O(N) scan, measured at 2.4 ms for 50k in the sim, so **~1 s on the device per album**, boosted, every ~40 min. Proposed follow-up for the libfile owner: a `LIB_ALBUMS` TRACKS column (master indices grouped by album in album order, 4 B × N = 200 KB on disk, read as one slice by the COUNT prefix sum), so starting an album is one small read.
8. **Shelf → playback.** `np_state` emits one `shuffle_commit` when the Shuffle step is left (not per wheel click). The screen maps it: library source → re-init the iterator in the new mode from the current track (`first` = the current entry) and replace what is queued after the current track. Ordinary queue → `playlist_shuffle()` / `playlist_sort()`. The setting itself is `global_settings.playlist_shuffle` plus a shell-side "albums" flag.

## Device-reality notes
- **Threads:** the iterator is UI-thread data. The audio thread only raises a flag. Refills take the playlist mutex the same way `lib_play()` does today.
- **Storage:** one wake per refill at worst (≤ 1 per ~50 tracks), usually none (it piggybacks on buffering). The raise-the-cap option would have rewritten ~3.5 MB per shuffle.
- **Memory:** no `core_alloc` while playing. The window reuses the existing playlist arrays, and the refill uses `library.c`'s existing play scratch (one file name).
- **Scale:** O(1) per track at any N up to 2^30. The Songs filter's skips are RAM-only.
- **Device checks (P10):** the refill latency (50 file-name reads) during playback, measured with the disk asleep and awake; the boot resume of the iterator after a power-off mid-refill (the control file and the state record must agree: write the state after the control file is synced).

## As built (Phase 5)
- **Trigger:** the track-change and buffering callbacks set flags and post `NP_PB_WAKE` (D7); `lib_shuffle_idle()` refills with ≤ 10 left when storage just ran (a buffering event) or is active, at once with ≤ 2 left. Every refill ends with `playlist_sync()` (control file fsync, then a flush and reload of the tracks ahead, so the audio thread sees the appended entries): the reload is one extra fill per ~50 tracks (device check P10).
- **Database states:** not ready (`tagcache_ui_count() < 0` or busy) → no refill, the window plays on, the next event retries; a new generation with a different stamp (commit id, entries, `SORTKEY_VERSION`, albums sum) → a new lap; the same stamp after a reload (every boot) → the run continues.
- **Boot:** `npshuf.dat` is read once (80 B) at the first idle pass; its stamp is checked at the first refill. A playlist with fewer than `queued`, or more than `queued + 50` entries is another queue (the record is dropped); up to 50 more is a power cut after the control-file sync (a new lap).
- **PREV before the window's first entry** is out of scope for v1 (Revision 1 item 9): the global PREV does what the playlist does.
- **Albums on an ordinary queue** (Revision 1 item 10): `lib_play()` notes album boundaries of the queue it builds (256 B static bitmap, ≤ 2,048 entries); the shelf's Albums calls the guarded `playlist_reorder_ipodui()`: back to insert order, `shuf_group_order()` in RAM, one shuffle command in the control file. ⚠️ A resume replays that command as a plain song shuffle of the same tracks (the control file cannot express the album order); the shelf still reads Albums, and choosing it again restores album order. A queue built elsewhere (no bitmap) plays in order.
- **Albums as a library source** reads each album's tracks from the #49 TRACKS section (two small reads per album: the start pair, then one entry per track), else builds the album-filter view once per album. The slice is not checksummed on this path (it is read in pieces): entries are range-checked.
- **Over-cap Songs:** an unfiltered Songs list played shuffled with more rows than the playlist cap starts a library source with the chosen song first. Filtered lists over the cap still take a sample.
