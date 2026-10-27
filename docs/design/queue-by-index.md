# Queue by database index (R4, #109, P16 item 16-7)

Status: **design, revision 1, not implemented** (2026-10-21, branch `p16`, after 16-3's safe replaces landed in `playlist.c`). Brief: `docs/phase16-brief.md` §4.7 (rev 3a, M2), checklist §6. Owner: agent B (playback-audiobook). The `playlist.c` guards are shared with agent A (§5 of the brief). Every file:line below is on `p16` at `70a3efd4cb`.

*Revision 1 (2026-10-21, design review: 4 High, 6 Medium, 6 Low). Every finding was checked against the code; all are accepted and applied inline. The table at the end gives the resolution and where it lands. The main changes:*
- *a failed resolve returns a sentinel path that can't exist, so playback skips one track and the queue doesn't end (H1);*
- *the rebase covers every slot, history included (H2);*
- *a long queue gets its memory by `audio_hard_stop()`, falling back to a window that fits (H3);*
- *resume pre-reserves capacity (H4);*
- *the publish hook only sets a flag (M4);*
- *two alternating payload files (M5);*
- *the 32 MB commit-peak margin is stated (M6).*

## 0. What is slow today, exactly

- **Play-all of any list** (`lib_play()` on a song row or a group row) goes through `play_view()` (`apps/ipodui/library.c:3218-3327`). For each row it does one `tagcache_retrieve(tag_filename)` (a disk read: file names are not in RAM, `library.c:6414-6417`) and one `playlist_insert_context_add()`. That call writes an `A:pos:last:path` line to the control file (`playlist.c:483-504`, several small writes per entry). This happens up to `max_playlist_size`, which is **2,000 on this target** (`apps/settings_list.c:1479-1484`, upstream's own note: about 10 s for 2,000 on a PP5020). Larger lists are windowed (`library.c:3243-3249`), or sampled when shuffled (Knuth S, `:3270-3279`).
- **Shuffle of a list of 2,000 rows or fewer** (a genre, an artist, a playlist) takes the same path and is then `playlist_shuffle()`d (`library.c:3323`).
- **Shuffle Songs from the Music menu is already cheap.** `lib_shuffle_all()` (`library.c:3496-3500`) and the over-cap Songs shuffle (`:3405-3412`) are the library source of #27 (`lib_shuffle.c`, `docs/design/shuffle.md`), which adds a 50-entry window (`LIB_SHUF_WINDOW`, `lib_shuffle.h:40`). The comment at `library.c:6414` that lists `lib_shuffle_all()` with the 2,000-row cost is out of date. "A 2,000-row Shuffle" in the brief therefore means shuffling a list of about 2,000 rows.

So the cost is roughly n file-name reads plus n control-file appends, with n capped at 2,000. The goal is 0 reads per entry, one bulk write, and no cap below the library size.

## 1. Representation and memory

### 1.1 Where an entry lives

Upstream keeps one `unsigned long` per queue position in `playlist->indices[]` (`playlist.h:78`):
- bits 31-30: the insert type;
- bit 29: queued;
- bit 28: skipped;
- bits 27-0: a seek into the control file or the `.m3u` file (`playlist.c:203-230`).

`get_track_filename()` (`:1134-1231`) reads the name back from that seek every time something needs it.

**A database entry** (`HAVE_IPOD_UI` only) is an entry of **`PLAYLIST_INSERT_TYPE_INSERT`**, the type every `A:` line gets (`add_track_to_playlist_unlocked()` sets it at `:1355` and stores it at `:1513`). This was fixed in review: L1. Its seek has bit 27 set:

```c
#define PLAYLIST_DBQ_FLAG   0x08000000u     /* seek >= this: a slot, not an offset */
#define PLAYLIST_DBQ_SLOT   0x07FFFFFFu     /* k */
/* word = PLAYLIST_INSERT_TYPE_INSERT | PLAYLIST_DBQ_FLAG | k */
```

- **k** is the slot's insertion sequence number, from 0 for each new queue and increasing with every append.
- **The slot table** (`qtab`, owned by `apps/ipodui/dbqueue.c`) maps k to a 32-bit value. What the value means depends on the block's mode: a master index (IDX), a filename-file seek in the block's generation (SEEK), or an offset into the path sidecar (PATH). `DBQ_LOST` (0xFFFFFFFF) marks a slot that can't be resolved (§3.1).
- **Why INSERT and not APPEND:** within one insert type, `sort_compare_fn()` (`:1676-1697`) orders entries by seek. With the same type as `A:` entries, a database word (seek ≥ 2^27) always sorts after any control-file entry, and database words sort among themselves by k, which is insertion order (the album order of the list). So `playlist_sort()` and unshuffle (`lib_shuffle.c:618-622`) restore the list order. APPEND would have made the comparator's cross-type branches (`:1685-1694`) put database words after every inserted entry, wherever they were added.
- **Unchanged code:** shuffle, delete, move, rotate and `first_index` permute or shift whole words. `randomise_playlist_unlocked()` (`:1617`), `remove_track_unlocked()` (`:1539`), `playlist_delete_range_ipodui()` (`:3910`) and `playlist_reorder_ipodui()` (`:3867`) are unchanged.

### 1.2 Capacity: the cap goes, and how memory is found (H3, H4)

Today `indices[]` is one `core_alloc_ex()` of `max_files_in_playlist × 4` B at `playlist_init()` (`:2072-2076`), which is 8,000 B, and its handle is not kept.

Under `HAVE_IPOD_UI`, a new `playlist_reserve_ipodui(int n, bool keep)`:
- keeps the handle in a static `int`;
- when `n > max_playlist_size`, allocates `n × 4` B, copies `amount` words when `keep` is set, frees the old block and sets `max_playlist_size = n`;
- is shrunk back to the setting's size by the next `playlist_create()` whose queue doesn't need more. Path queues (On-The-Go, `apath.c:271`, `devbcm.c`) never grow it;
- refuses to grow under `HAVE_DIRCACHE`. `dcfrefs` is sized to the setting (`:2079-2082`), so the combination is an `#error` (L3). The shell trims dircache (`IPODUI_TRIM_DIRCACHE`, `ipodvideo-variant.h:58`).

**Where the memory comes from (H3).** Checked against `playback.c:1095-1119`:
- while **paused**, `shrink_callback()` refuses every allocation from a thread other than the audio thread and posts #88's `Q_AUDIO_GIVE` instead, so an allocation from the UI thread fails;
- while **playing**, a shrink stops and restarts the codec (`:1121-`), only for the queue to be replaced a moment later;
- while **stopped**, the buffer is still allocated, and the stopped shrink gives it up.

There were two candidate fixes:
- **(a) Free memory only, falling back to the 2,000 window**, as `lib_generation_pool_prepare()` does (`library.c:342-344`). While audio holds the buffer, free heap is small: the audio buffer is the maximum allocation, and the shell's spare pool is 665,536 B and nearly full when the library is in use (`library.c:357-363`). So (a) would fail at 50k almost every time, and the cap would stay in practice.
- **(b) `audio_hard_stop()` (`playback.c:4516-4528`: stops playback, stops PCM, frees the audio buffer) before `playlist_create()`, only when n exceeds the current capacity and audio is not stopped.** The old track stops a few tens of milliseconds earlier than `playlist_start()` would have stopped it. Today a `play_view()` that has started has already destroyed the old queue anyway (`playlist_create()` at `:3090`). No shrink callback runs, so #88's GIVE path and its floor rule are never involved: there is no paused queue to keep. The next `audio_play()` reallocates the audio buffer from what is left, so the queue's memory is counted under the floor exactly as §1.3 says. `Q_AUDIO_STOP` is a `queue_send`, which serializes behind any posted `Q_AUDIO_GIVE`.

**Chosen: (b), with (a) as the fallback.** If the reserve still fails after the stop (fragmentation, a low-memory variant), `play_view()` builds an **index** queue of the largest window that fits. That is the current capacity, at least 2,000, chosen with today's window or sample logic (`:3241-3279`, kept for this case). It is logged as `dbq reserve fail want= got=`. Book queues are always small and never take the stop path.

**Resume (H4).** At boot, capacity is `max_files_in_playlist` (`:2072`). `dbq_boot()` (§5.3) knows each persisted block's `count`, so `playlist_resume()`, right after `empty_playlist_unlocked()` (`:3331`) and with its temp buffer held, calls `playlist_reserve_ipodui(setting + dbq_total(), false)`, with nothing to copy. Audio is stopped at resume (`ui_playback_resume()` returns early otherwise, `ui_main.c:170-171`), so any shrink takes the stopped path. The `T` case (P3) also calls `playlist_reserve_ipodui(amount + count, true)`, which copies the words, as a backstop. If even that fails, the resume fails with `-19`, logged.

### 1.3 Memory cost

The figures assume a 50,000-row list. Shell-start resident memory is unchanged: `indices[]` starts at 2,000 entries. Everything below exists only while such a queue is current, and it comes from the buflib heap, which means from the audio buffer.

| Item | 2,000 rows | 50,000 rows | Today |
|---|---|---|---|
| `indices[]` (4 B/entry) | 8,000 B | 200,000 B | 8,000 B |
| `qtab` (4 B/entry, movable `core_alloc`) | 8,000 B | 200,000 B | — |
| block table (≤ 4 × 48 B, in the `qtab` allocation) | 192 B | 192 B | — |
| album-group bits (`grp_bits` moved into the `qtab` allocation, n/8 B) | 250 B | 6,250 B | 256 B static (`library.c:3005-3006`) |
| **Total** | **16,442 B (+8,186 B)** | **406,442 B (+398,186 B)** | 8,256 B |
| Disk: `dbq0.dat`/`dbq1.dat` payload | 8,000 B | 200,000 B | control file ≈ 2,000 × 90 B |

**Transient memory** (§4.3), only after a renumbering commit, never at the commit peak:
- a 4 B × n permutation (8 KB / 200 KB);
- a 32 KB chunk;
- the old snapshot released first.

**Audio-buffer floors**, using `docs/hardware.md`'s binding row (sim, 50k tracks, after a commit and a restart):

| Unit | Today | With a 50,000-row queue | Floor | Headroom left |
|---|---|---|---|---|
| 32 MB | 13,988,720 B | 13,590,534 B | 7,500,000 B | 6.09 MB |
| 64 MB | 47,543,152 B | 47,144,966 B | 39,000,000 B | 8.14 MB |

**Commit peak on 32 MB (M6).** The minimum free core heap at the commit peak is **8,269,688 B** at 50k with both snapshots (`docs/hardware-fit-audit.md:300`), which is 769,688 B above the 7.5 MB floor. A live 50,000-row queue takes 398,186 B of it, leaving **371,502 B (≈ 0.37 MB)**. In practice:
- on 32 MB at 50k with a full-library queue, more #88 paused windows decline (Q11's "the update waits"), and more updates wait for a stop. That is the rule working, not a breach;
- 64 MB is unaffected (41.8 MB minimum free at the peak);
- 16-A's floor gate gains two cases: "play-all Songs at 50k" (steady state) and "commit with a 50k queue live" (the peak margin, ≥ 0 required).

16-8 (R6, #110) is the lever that wins this margin back.

There is **no new resident static over 1 KB**. The new statics are `dbqueue.c`'s state (a handle, a mutex, flags, the seq counter: under 64 B) and `playlist.c`'s `indices_handle` (4 B). The 256 B `grp_bits` static goes away.

**Option, not chosen (Q-R4-2):** keep `qtab` on disk only. That saves 200 KB at 50k and costs one more small read per resolve.

## 2. Hooks into `playlist.c`, `playback.c` and the shell

All `playlist.c`/`playlist.h` edits are under `#ifdef HAVE_IPOD_UI`, so the stock build is byte-identical. Each one gets a row in `docs/changes-from-rockbox.md`.

| # | Where | Change |
|---|---|---|
| P1 | `get_track_filename()` `:1150-1151` | When `control_file && (seek & PLAYLIST_DBQ_FLAG)`, call `dbq_filename(k, tmp_buf, sizeof tmp_buf, dir_buf)`. It **always returns 0**: on any failure it writes the sentinel path (§3.1, H1), and it uses `dir_buf` as its own scratch (L5). Then continue at `format_track_path()`, with `utf8 = true`. Still under `playlist_write_lock` (`:1148`). |
| P2 | new `playlist_insert_dbq_ipodui(pl, uint32_t first_k, int count, uint32_t tag)` (`playlist.h`, beside `:173-178`) | Under the write lock: checks capacity, appends `count` words `INSERT\|DBQ\|k` at `amount` (the `PLAYLIST_INSERT_LAST` case of `add_track_to_playlist_unlocked()` `:1395-1402`, without the shift loop), sets `last_insert_pos`, and writes **one** line `T:<first_k>:<count>:<tag>` (3 fields: fits `strp[3]`), then `fsync`. `tag` = payload file (0/1) << 24 \| seq. No per-entry I/O. |
| P3 | `pl_cmds_run()` `:3253-3277`; the command switch in `playlist_resume()` `:3376-3600`; `enum playlist_command` (`playlist.h:45-55`) | `'T'` → `PLAYLIST_COMMAND_DBQ`. The enum value is declared **before** `PLAYLIST_COMMAND_COMMENT` (the field splitter only runs for `current_command < PLAYLIST_COMMAND_COMMENT`, `playlist.c:3612`), under `#ifdef HAVE_IPOD_UI` in `playlist.h` (L2). The case calls `dbq_resume_block(tag, first_k, count)` (§5), then `playlist_reserve_ipodui(amount + count, true)` (H4), then appends as P2 does without writing. On failure: `result = -19; goto out`. |
| P4 | `update_control_unlocked()` `:483-529` | Refuses (`-1`) when an `A`/`Q` seek would reach `PLAYLIST_DBQ_FLAG` (128 MB). This guard is unreachable in practice. |
| P5 | `playlist_init()` `:2072-2076`, `playlist_create()` `:2190`, `playlist_resume()` `:3331` | §1.2. `playlist_create()` calls `dbq_reset()` (new queue, k from 0, the payload file flips, §5). `playlist_resume()` pre-reserves (H4). |
| P6 | `playlist_resume_track()` `:3695-3716` | For a queue with database entries, the CRC scan is bounded to ±64 positions around `start_index`, then falls back to `playlist_start(start_index, 0, 0)`. Unbounded, a missing file at 50k would mean 50,000 resolves. |
| P7 | `playlist_get_track_info()` `:2490-2521` | No change. Database words carry the INSERT type, so they report `PLAYLIST_ATTR_INSERTED`, exactly as `A:` entries do today (L1). |
| P8 | `pl_save_playlist()` `:4103-4120`, `pl_save_update_control()` `:4203` | No change. A save resolves every entry through P1 and rewrites the words as `.m3u` seeks. A sentinel would be saved as a line that doesn't exist, which the next load skips. Only the developer variant can save. |
| S1 | `ui_playback_resume()` `ui_main.c:178` | `playlist_resume() == -1` → `< 0` (M1). Today `-19` and every other negative code except −1 fall through and still play. |
| S2 | `lib_play_held()` `library.c:3409` | The over-cap test reads `global_settings.max_files_in_playlist`, not the now-mutable `max_playlist_size` (M2). Otherwise, after one 50k queue, Songs shuffle would stop taking the library source. |

`playback.c` needs **no required edit**. All of its names come from `playlist_peek()` (`playback.c:1998`, `:2551`; `playlist.c:3148-3189`) through P1. With H1's sentinel, the buffering loop's `file_exists()` probe (`playback.c:2559-2575`) skips the entry with `playlist_skip_entry()` and the queue goes on. A `NULL` from the peek (end of playlist) happens only past the last position. One optional edit is in §3.3.

`library.c`'s `play_view()` (`:3218-3327`) becomes:
1. if `n > max_playlist_size` and `audio_status()`, call `audio_hard_stop()` (H3);
2. `play_begin()` (`:3088`);
3. `playlist_reserve_ipodui(n, false)`, falling back to a window that fits;
4. `dbq_begin(m)`;
5. a RAM loop writing `qtab[k] = row` and `grp_note()`, still skipping video rows (`:3283-3285`) and checking the generation (`:3268`);
6. `dbq_commit()` (§5.2);
7. `playlist_insert_dbq_ipodui()`;
8. `playlist_shuffle()` / `playlist_start()`, as today.

`play_scratch_open()` stays for **book queues only**: `book_note()` (`:3141-3155`) hashes each file's path into the book map. Books have tens of files, so that cost is unchanged.

## 3. Resolving a file name at track load

### 3.1 The resolver

`dbq_filename(k, buf, len, scratch)` in `apps/ipodui/dbqueue.c`:
- Runs on the caller's thread: the audio thread (buffering's peek), the UI thread (track info, the resume CRC) and, in the dock variant, the iAP thread.
- Always runs under the playlist mutex (P1), then takes the dbq mutex. The lock order is **playlist → snapshot reference (try only) → dbq**.
- Reads `v = qtab[k]` and the block's stamp into locals **under the dbq mutex, before any I/O**, then releases it. `qtab` is movable and is never dereferenced across a yield (L4).

| Mode | Condition | Reads | Disk I/O |
|---|---|---|---|
| **IDX, live** | the block's stamp `{commitid, entries, db_path}` all equal the active snapshot's (`tagcache_ui_commitid()`, `tagcache_ui_count()`, `tagcache_ui_db_path()`) and the RAM cache is loaded (L6) | the filename seek from RAM (`tagcache_ui_seeks_ctx()`, `tagcache.c:10012`), then one `pread` of the entry header and name from `<db_path>/database_4.tcd` | open, 1 read (≤ 1 sector, plus directory sectors), close |
| **IDX, on disk** | the stamp names a generation whose directory still holds it (active or retained), but the RAM cache isn't loaded yet or has been replaced and the rebase is still owed | the master-index entry from `<db_path>/database_idx.tcd`, its `FLAG_DELETED` checked, then the name as above | 2 opens, 2 reads |
| **SEEK** | after a renumbering commit, until materialized (§4.3) | `v` = the filename seek in the block's generation | open, 1 read |
| **PATH** | materialized | `v` = offset into `dbqpath.dat` | open, 1 read |
| **lost** | `v == DBQ_LOST`, or none of the above applies | — | none |

**Failures never end the queue (H1).** Checked: `get_track_filename() != 0` makes `playlist_peek()` return `NULL` (`playlist.c:3160-3161`), which the buffering loop treats as end of playlist (`playback.c:2551-2556`, `:2579-2583`). Also, `tagcache_ui_seeks_ctx()` returns `NULL` for a `FLAG_DELETED` entry (`tagcache.c:10019-10020`), which contradicted revision 0's "deleted entries resolve to their name".

So in every failure case (a deleted entry, a lost slot, a read error, a USB-inserted state, a snapshot that can't be referenced), the resolver writes the **sentinel** `"/<dbq>/<k>"` and returns 0.
- `<` and `>` are invalid in FAT names, so `file_exists()` is always false and playback skips that one entry (`playback.c:2559-2575`).
- At the end of the queue, the existing "only skip if it has a successor" rule (`:2568-2569`) ends playback normally.
- The log says `dbq miss k= why=deleted|lost|io|busy`, rate-limited to one line per block per second.
- **A busy snapshot** (a publication in flight: try-acquire fails, `tagcache.c:6596`) first falls back to **IDX, on disk**, which needs no snapshot. It returns the sentinel only if that also fails.
- **Where the sentinel shows:** `playlist_peek()`'s bogus-directory stripper (`:3170-3181`) finds nothing and returns the sentinel unchanged. The CRC of the sentinel is stored as resume info only if the user stops on it, which the skip prevents. In the developer playlist viewer, the sentinel shows as a name.

No fd is kept across calls, so nothing is open across USB or a generation switch.

### 3.2 Stack (L5)

`get_track_filename()` already has `tmp_buf` and `dir_buf` (2 × 261 B) plus locals: about 540 B. Adding P1's call:
- the resolver's own frame is ≈ 80 B, because it builds its tag-file path in the caller's `dir_buf` instead of its own buffer and reads the 8 B entry header into a local;
- the `open()` chain is ≈ 1.3 KB deep (`docs/hardware.md`: "the open path ~1.3 KB deep");
- so the new peak below `get_track_filename()` is ≈ 540 + 80 + 1,300 ≈ **1.9 KB**.

On the audio thread this sits under `audio_load_track()` → `playlist_peek()` (`path_buf` of 261 B at `playback.c:2547`). The path already opens the audio file at the same depth (`:2561`), but that `open()` is not nested inside `get_track_filename()`'s 540 B. So the net new depth is ≈ 540 + 80 B over the existing open path. On the iAP thread (dock variant) the depth is through `playlist_get_track_info()`.

**Measurement plan:**
- `-fstack-usage` plus `scripts/risk/lib/stack_depth.py` (the 8b-T static worst-case tool) on `audio_thread` and `iap` paths, reported against each stack's size;
- the device's per-thread stack high-water marks in the devstats log after the `-dbq-*` scripts' device equivalents;
- **gate:** at least 25 % of each stack left free in the static worst case, otherwise the resolver moves its open into a helper that can't inline and the numbers are reported.

### 3.3 Disk I/O compared with today

| | Today (2,000 rows) | Index queue |
|---|---|---|
| Building the queue | 2,000 tag reads + about 6,000 small control writes | 0 reads; 1 write of 8 KB, 1 small header replace, 1 line, 1 `fsync` |
| Per track load | 1 read of the open control file | 1 open + 1 small read (IDX live, SEEK, PATH) |
| Resume at boot | the whole control file (2,000 lines, ≈ 180 KB) | 2 short lines + one 8 KB (2k) / 200 KB (50k) bulk read |

Track loads already wake the disk for the audio file, so resolves add no wake.

**Optional (commit 6):** S36 looks up the index of every loaded track by **file name** (`find_index()`, `tagcache.c:1621-1650`, called from `playback.c:2730`). For an IDX-live slot the resolver knows `(ctx, idx, name)`, so a `tagcache_ui_note_index()` that fills the `ui_fidx` slot (`:1641`) makes that lookup free. It is measured first.

## 4. Generations and fallback rules

### 4.1 The generation id

The stamp is `{commitid, entries, db_path}`.
- `commitid` (`tagcache.c:10071`) is persistent and monotonic: raised by a commit (`:5652`), and set above every earlier library by a fresh rebuild (`dbgen_newest_commit()`).
- `tagcache_ui_generation()` changes at every boot's RAM load, so it is not the id (`lib_shuffle.h:58-66`).
- All three fields must match for IDX-live (L6), so a salvaged directory or a reload into another directory with the same commit id never passes.
- On-The-Go uses the same commitid rule (`otg.h:37-39`).

### 4.2 The rules

1. **The stamp matches the active snapshot** (all three fields): the rows are valid. Resolve in IDX mode.
2. **A publish changes the stamp.** The hook right after `ui_active = ui_build` (`tagcache.c:5185`) only takes a **passive reference** on the old context (`tagcache_context_acquire_passive()` semantics, `:6566`; an IRQ-safe reference count, no lock) and sets `dbq_rebase_owed = true`. It takes **no** dbq or playlist mutex, because it runs inside `command_queue_mutex` and the window with no yield or allocation (`:5183-5184`) (M4).

   The tagcache thread then runs `dbq_rebase(old, new)` at the top of its next loop, outside that window, unboosted and yielding. It **covers every slot, history included** (H2: Previous, Repeat All and unshuffle reach history slots, and the mode is per block):
   - **Copy out:** take the playlist mutex (`playlist_get_current()`'s lock, through a new `playlist_lock_ipodui()`/`_unlock` pair), then the dbq mutex, and copy the block's seq and `qtab` (n × 4 B) into a transient buffer. Release both. Lock order: always playlist → dbq, never the reverse (M4). If the block isn't resident (no resume yet this boot), read its payload from disk instead, with no playlist lock needed.
   - **Compare** a RAM-only fingerprint per slot in both snapshots: `crc32(title) ^ crc32(album) ^ length ^ mtime` (the `otg_check()` idea, `otg.h:61`, with album and mtime added). A slot deleted in the old snapshot becomes `DBQ_LOST`.
   - **All live slots match** (an incremental commit: upstream's append-only master keeps rows, `build_index()` skips deleted entries in place, `tagcache.c:4313-4335`): the block is re-stamped. Slots deleted in the new snapshot simply resolve to the sentinel.
   - **Any mismatch** (Rebuild Cache, a format change, a salvage): each slot's value becomes its **old filename seek** (`tag_seek[tag_filename]` of the old snapshot), or `DBQ_LOST`. The block's mode becomes **SEEK** on the old stamp. This is a RAM pass with no I/O. Materialization is then owed (4.3).
   - **Apply:** take the playlist mutex, then the dbq mutex. If the block's seq is unchanged, swap in the new values and mode. Otherwise (the queue was replaced meanwhile) drop the result. Release, then persist the change (§5).
   - Drop the passive reference, which frees the old snapshot.
   - Cost: ≤ 2 × n fingerprints; at 50k an estimated ≤ 150 ms of CPU on the device, yielding. To be measured (`dbq rebase n= us=`).
3. **Materialization (SEEK → PATH)**, after the old snapshot is released, so never at the commit peak:
   - a 4 B × n permutation sorted by seek (8 KB / 200 KB) and a 32 KB chunk;
   - one sequential pass over the old directory's `database_4.tcd` (≈ 4 MB at 50k, about 0.6 s on flash);
   - `dbqpath.dat` written with every slot's name (≈ n × 80 B), then `qtab[k]` = offsets, mode PATH, applied with the same playlist → dbq lock order.

   **Ordering guarantee:** the tagcache thread doesn't start `dbgen_prepare()` while a rebase or a materialization is owed. The check sits at the top of the commit path, on the same thread. So the retained old directory can't be recycled under a SEEK block. If the permutation doesn't fit in free heap without touching the audio buffer (`core_allocatable()`), the job waits for #88's paused window or a stop (`maint_want(MAINT_COMMIT)`, brief §3.1), and the next commit waits too, logged as `dbq materialize waits`. That is Q11's behavior.
4. **Boot order (M3).** `init_tagcache()` (`main.c:875`) runs before `playlist_init()` (`main.c:889`), so a boot publish could beat `dbq_boot()`. Two guards cover this:
   - the prepare gate in 4.3 also waits for `dbq_booted`. The flag is set at the end of `dbq_boot()`, and a missing header counts as booted, with nothing owed;
   - `dbq_boot()` itself sets `dbq_rebase_owed` when a persisted block's stamp doesn't match the active snapshot. A block in that state has no old RAM snapshot left, so it goes **straight to materialization from disk**: read the old directory's master index for the n slots in one sequential pass of `database_idx.tcd` to get their filename seeks (SEEK), then materialize as above. If the stamp's directory no longer holds that commit id (two commits since, both while the queue wasn't persisted as PATH, which is only possible after a power cut mid-job), every slot is `DBQ_LOST`. The block is **lost**, and Q-R4-5 applies.
5. **Safety net:** `playlist_resume_track()` still checks the saved filename CRC (`playlist_update_resume_info()` `:4001-4030`), so a wrong resume falls back to a bounded scan (P6). ab_store and statistics key off the path the resolver returned (§6), and the statistics row is looked up in the **current** snapshot, so a stale index can at worst skip a track and never credit the wrong row.

### 4.3 References from other stores

- **On-The-Go** stores paths plus `{idx, check}` records under the commitid rule (`otg.h:33-53`). `otg_play()` (`otg.c:837-869`) still builds a **path** queue, capped at `OTG_MAX`; that is the fallback. Commit 7 (optional) builds an index queue when `otg.idx` is valid.
- **Bookmarks** (`bookmark.c`, developer variant only: `IPODUI_TRIM_BROWSER`, `ipodvideo-variant.h:71`) store the resume index, seed and name CRC (`bookmark.c:537-541`) and need a saved playlist (`:1178`, `:1366-1389`). Saving goes through P1 and P8.
- **ab_store** (`ab_store.h:89-101`) is keyed by `book_key` (tags) and `ab_path_crc(path)`. The resolver returns the same absolute `tag_filename` string that `play_add()` inserted before, so the CRCs are unchanged.

## 5. Persistence and crash safety

### 5.1 Files

All live in `ROCKBOX_DIR "/ipodui/"`, in native byte order.

- **`dbq0.dat` / `dbq1.dat`: two alternating payload files (M5).** A new queue writes the file the current header does **not** name (O_TRUNC). Each block is `{magic, seq, count, mode, crc32(payload)}` followed by `count × uint32`. A refill (commit 7) appends to the current file. `seq` is a fresh 24-bit counter per queue, kept in the header and never reused within 2^24 queues.
- **`dbq.hdr`:** the block table, ≤ 4 entries of `{file, seq, file_off, count, mode, commitid, entries, db_path[24], crc}`, sealed by a CRC. It is always replaced with `safefile_replace(tmp, dest, prev)` (`safefile.h`, 16-3), never written in place. It changes when a block is added, re-stamped, moved to SEEK or materialized. A mode change rewrites the payload into the **other** file first, then the header.
- **`dbqpath.dat`:** written to a temporary file, then `safefile_replace`.

### 5.2 Write order for a new queue

1. `playlist_create()`: a new control file with only its `P:` line (`create_control_unlocked()` `:421-460`).
2. Write the block to `dbq<other>.dat`, then `fsync`.
3. `safefile_replace` `dbq.hdr` so it names that file and seq.
4. P2 appends `T:0:n:<file<<24|seq>`, then `fsync`.
5. `playlist_start()`.

**What a cut leaves:**
- a control file without a `T` line: an empty queue, no resume;
- or a `T` whose file, seq, count and CRC are all confirmed by `dbq.hdr` and the payload.

**M5 case:** a cut in a control-file swap whose recovery puts the **previous** control file back (`safefile_recover(PLAYLIST_CONTROL_FILE, … _PREV)`, `playlist.c:3304-3306`). That control file's `T` names the previous queue's file and seq. The previous payload file is the one the new queue did **not** truncate, so it is intact. The previous header copy is restored the same way, because `dbq.hdr`'s own `safefile` recovery keeps its prev. Any remaining disagreement makes P3 fail with `-19`, and S1 makes that a non-resume. A partial table is never played.

### 5.3 Boot recovery order

This follows brief §3.3, M3.

1. `dbq_recover()`: `safefile_recover()` for `dbq.hdr` and `dbqpath.dat`, in `playlist_init()` next to the existing control-file recovery (`playlist.c:2056-2068`).
2. `dbq_boot()`: loads `dbq.hdr` (≤ 256 B), sets `dbq_rebase_owed` if a stamp isn't the active one, and sets `dbq_booted` (§4.2.4).
3. `playlist_resume()`: pre-reserve (H4), then replay the `T` lines.

### 5.4 Other rules

- **USB:** no dbq write at USB connect or screen entry. Rebase and materialization run on the tagcache thread, which `tagcache_usb_quiescing` stops, and they resume after the disconnect, before the next prepare.
- **Statistics:** the journal paths (`tagtree.c:1130`, `tagcache_ui_update_stats()`) are unchanged.

## 6. Consumer table ("what else used this?")

"Variant" means compiled only in the developer `--ipodui-plugins` or `--dock30` builds (`ipodvideo-variant.h:52-78`).

| # | Consumer | Where | Uses | How it keeps working |
|---|---|---|---|---|
| 1 | Boot resume | `ui_main.c:168-189` → `playlist_resume()` `playlist.c:3283`, `playlist_resume_track()` `:3695`, `lib_queue_resumed()` `library.c:3527`; boot order `main.c:875`/`:889` | control file, `resume_index`, name CRC | `T` replay (P3) after a pre-reserve (H4); S1 makes every negative code a non-resume (M1); recovery order §5.3; `dbq_booted` gate (M3); CRC through P1; bounded scan (P6) |
| 2 | Resume-info save | `playlist.c:4001-4030`, from `playback.c:1684`, `:1711`, `:3626`, `ui_main.c:901` | CRC of the current name | P1 (one small read per change, as today) |
| 3 | Track info | `playlist_get_track_info()` `:2490` | name, attributes | P1; INSERT type → `INSERTED`, as `A:` entries (P7, L1) |
| 4 | Now Playing "n of N" | `np_playback.c:377-378` | positions | Unchanged; N may now be 50,000 |
| 5 | Next/previous and buffering | `playback.c:1998`, `:2551-2583`, `:3137-3152`, `:3308-3327`, `:3765-3797`, `:4560`; `playlist.c:3148`, `:3024`, `:2229`, `:1784` | peek names, position arithmetic | P1 never fails, and a miss is the sentinel → `file_exists()` false → one entry skipped (H1); `NULL` only past the end |
| 6 | Shuffle | `playlist_shuffle()` `:3813`, `randomise_playlist_unlocked()` `:1617`; shelf `lib_shuffle.c:596-629` | permutes words, `S:` | Unchanged |
| 7 | Unshuffle, album order, history | `sort_compare_fn()` `:1676`, `playlist_sort()` `:3941`, `playlist_reorder_ipodui()` `:3867`, `grp_bits` (`library.c:3002-3031`, `:3504-3514`) | seek order, album bits, earlier positions | INSERT type and monotonic k restore list order (L1); `grp_bits` in the dbq allocation (n/8 B); every slot is rebased or materialized, history included (H2) |
| 8 | Repeat | `get_next_index()` `:1784-1873` | positions | Unchanged; wrapped-around slots resolve because every slot is rebased (H2) |
| 9 | Library source (Shuffle Songs) | `lib_shuffle.c:261-340`, `:423-505`; the over-cap test `library.c:3409` | path inserts, `queued` count; the cap | Unchanged in commits 1-6; the over-cap test reads the setting (S2, M2); commit 7 (optional) moves the refill to P2 |
| 10 | On-The-Go | `otg.c:837-869`, `otg.h:33-53` | its own paths and `{idx, check}` | Path queue unchanged; optional index queue in commit 7 |
| 11 | Audiobook queues, per-book resume | `library.c:3100-3210` (`book_ctx`, `book_start()`), `:3516`; `ab_book.c:90-118`; `np_playback.c:294`, `:443-450`, `:661-690`; `playback.c:2160` | names hashed at build, positions, amounts | Books still read their few names at build; small n never takes the hard-stop path; positions and CRCs unchanged |
| 12 | ab_store | `ab_store.h:89-101`, `ab_store.c` | `book_key`, path CRC | Unchanged |
| 13 | Bookmarks | `bookmark.c:537-541`, `:1178-1389` (variant) | index, seed, CRC, saved playlist | Through P1 and P8 |
| 14 | Statistics | `tagtree.c:943-1130`, `tagcache.c:1693` (from `playback.c:2730`), journal | `id3->path` → row in the current snapshot | Unchanged; a sentinel slot never loads, so it is never credited; optional known index (commit 6) |
| 15 | Scrobbler | plugin (variant) | `id3->path` | Unchanged |
| 16 | Album art | `playback.c:2350-2373` (`find_albumart(..., path)`); shell art keyed by tags (`artkey.h:54`) | `id3->path` | Unchanged |
| 17 | `safefile`, #91 control writes | `playlist.c:131-140`, `:421-460`, `:3724`, `:4057-4300`, `:2056-2068`, `:3304-3306` | control-file replace and recovery | `T` is a plain append; `dbq.hdr`/`dbqpath.dat` use `safefile_replace`; alternating payload files keep a recovered previous control file valid (M5) |
| 18 | USB disconnect | `usbscreen.c:1115`, the post-USB scan, `tagcache_usb_quiescing` | removed files, a commit after | No fd held; a removed file → `file_exists()` false → skipped; the commit after → rule 4.2.2 |
| 19 | Commit while playing or paused (#88) | `ui_candidate_publish()` `tagcache.c:5111-5215`; `playback.c:1095-1119` (GIVE), `:1230-` (`audio_on_give`) | renumbering, the snapshot switch, the paused restart | The hook is flag plus passive reference only (M4); rebase off-window with lock order playlist → dbq; the restart re-peeks through P1; the queue's memory lowers the 32 MB peak margin to 0.37 MB, so more windows decline (M6) |
| 20 | Audio memory at queue start | `playback.c:4516` (`audio_hard_stop`), `:1095-1119` | the audio buffer | Hard stop only when a long queue needs more capacity than it has, otherwise a window that fits (H3) |
| 21 | tagnavi / tagtree | `IPODUI_TRIM_TAGTREE` (`ipodvideo-variant.h:74`) | — | Not affected; the stock build has no dbq code |
| 22 | Rockbox playlist viewer (dev combo) | `playlist_viewer.c:194`, `:1244-1304` (variant) | track info per row, full-list search | Through P1; a search over 50k is 50k resolves (minutes, developer only) |
| 23 | iAP (dock variant) | `iap-core.c:734`, `iap-lingo4.c:1488`, `:1830`, `iap-lingo3.c:1107` | track info | Through P1; stack per §3.2 |
| 24 | Now Playing marks | `npmark.c:139-160` | `created_tick`, display index | Unchanged |
| 25 | Shell path-queue builders | `apath.c:271-274`, `devbcm.c:935`, `:1317`, `:1550`, `:2906` | path inserts | Unchanged; `playlist_create()` → `dbq_reset()` and shrink back |
| 26 | Stock browser | `filetree.c:825`, `tree.c:1280` | CRC, peek | Stock: no dbq code; variant: P1 |
| 27 | Search and OTG row paths | `library.c:3329-3364`, `:3545` | `tagcache_retrieve` | Untouched |

## 7. Test plan

**Host** (`scripts/test.sh`; new `tests/dbqueue/` over the pure `apps/ipodui/dbqueue_core.c`):
- header and block seal/check, including a torn tail and the wrong file or seq;
- `T` field packing;
- the mode table: (stamp fields, RAM loaded, directory holds commit, materialized) → mode;
- the rebase decision over **all** slots;
- sort order of INSERT words against `A:` words;
- the `tests/fatcrash` model: a cut after every write in §5.2, in a mode change and in a materialization;
- **M5:** a cut control swap restores the previous control file and its payload file is intact.

Per High finding:
- **H1:** a table test that every failure mode returns 0 and a sentinel that `file_exists()` can't match.
- **H2:** a rebase over a queue whose current position is in the middle, checking the history slots' values.
- **H4:** a reserve with `keep` copying the words.

**Sim scripts** (`tests/ui/`, `build/sim-ui` and `build/sim-ui32`, 50k library from `tests/perf/lib50k.sh` where noted). Traces:
- `lib play n= us= cpu_us=` (`library.c:3324`);
- new `dbq build n= bytes= us= reserve=ok|stop|window`;
- `dbq rebase block= result=stamp|seek|lost n= us=`;
- `dbq materialize n= bytes= ms=`;
- `dbq miss k= why=`.

One script per High:
- **H1 `ipodui-p16-dbq-miss-midqueue`:** queue 20 tracks, delete track 5 on the simdisk and run a commit (FLAG_DELETED), corrupt one slot (lost). Playback plays 1-4, skips 5, continues to 20 and stops normally; no splash.
- **H2 `-dbq-rebase-history`:** play to track 30 of a 2,000-row queue, Rebuild Cache (renumbers). Previous ×3, a Repeat All wrap to track 1, then shelf Shuffle Off. Every file played matches the pre-commit path list (traced CRCs).
- **H3 `-dbq-playall-from-playing` and `-dbq-playall-from-paused`** (`sim-ui32`, 50k): with another queue playing or paused, play-all Songs gives `reserve=stop`, n=50000, "1 of 50,000", no `shrink refused paused` line, and the floor receipts at or above the floors. A variant with a heap pre-filled forces `reserve=window`.
- **H4 `-dbq-resume-50k`:** play-all at 50k, reboot the sim, PLAY resumes the same file and position with n=50000.

Others:
- `-dbq-shuffle-2k` (time, unshuffle order and groups);
- `-dbq-commit-incremental` (`result=stamp`, no reads);
- `-dbq-resume-renumber` (renumber, then a reboot before PLAY: the M3 path, same file, statistics on the new row);
- `-dbq-commit-paused` (#88);
- `-dbq-commit-peak-32` (the 16-A peak case with a 50k queue: margin ≥ 0, logged);
- `-dbq-usb`;
- `-dbq-book`;
- `-dbq-otg`;
- `-dbq-resume-fail` (a damaged header: no resume, Now Playing hidden; M1);
- stock sim: plays a folder, size diff 0.

**Device** (owner's go per rule 5):
- `m0 tap2pcm why=play` on a Shuffle of about 2,000 rows and on play-all Songs, before and after;
- resume after a renumbering commit (Rebuild Cache, reboot);
- devstats disk attribution per track load (no added wake);
- stack high-water marks for the audio thread (§3.2);
- the 16-A device floor check with a full-library queue;
- `validate.sh --quick`; no new strings or splashes.

## 8. Risks and owner questions

**Risks**, highest first:
1. **Wrong file played or credited after a renumbering.** Guarded by the three-field stamp, the all-slot rebase (H2), materialization before recycling, the boot gate (M3), the resume CRC and path-keyed statistics. Tests: `-dbq-rebase-history`, `-dbq-resume-renumber`, fatcrash.
2. **Audio-thread stalls, deadlock or stack overflow in the resolver.** Lock order playlist → try-only reference → dbq; the rebase's copy-out takes playlist → dbq too, and the publish hook takes no lock (M4). It never waits on `ui_publishing`. ≈ 620 B of new depth over the open path, measured per §3.2.
3. **Memory at 50k on 32 MB.** 398 KB steady state, 6.09 MB over the floor. The commit-peak margin is only **0.37 MB**, so updates wait more often (M6). The hard stop (H3) takes the memory without a paused shrink. Recorded in 16-A's gate.

Other risks:
- O(n) resolves in the resume scan (bounded by P6) and in the developer viewer;
- the queue being cut a few tens of milliseconds earlier on a long play-all;
- the sentinel showing as a name in developer tools;
- the `T` line being unknown to a stock build (harmless: no device runs both);
- 16-1 and 16-8 changing publication, so the hook point must be re-anchored when they land.

**Owner questions** (frozen ⚠️ with a default):
- **Q-R4-1:** Queue the whole list, up to the library? **Recommended: yes.** On 32 MB at 50k, this lowers the commit-peak margin to 0.37 MB, so more updates wait. The alternative is a fixed cap, such as 10,000.
- **Q-R4-2:** Slot table in RAM? **Recommended: RAM.** Reading it from disk per resolve saves 200 KB at 50k and costs one more small read.
- **Q-R4-3:** Materialize every slot after a renumbering commit (≈ 4 MB sequential read at 50k, ≈ n × 80 B on disk)? **Recommended: yes**, and the next commit waits for it.
- **Q-R4-4:** Keep the library source for Shuffle Songs, moving only its refill to index entries? **Recommended: yes.**
- **Q-R4-5:** A lost block at boot (only after a badly timed power cut)? **Recommended: don't resume it**, and log it. The alternative is to resume with every slot skipped, which would play nothing.
- **Q-R4-6:** Keep `max_files_in_playlist` at 2,000 for path queues and `OTG_MAX`? **Recommended: yes.**
- **Q-R4-7 (new, H3):** May starting a long list (more than the current capacity) stop the old track a few tens of milliseconds before the new one starts? **Recommended: yes.** The old queue is replaced anyway. The alternative caps queues at what free memory holds, which in practice keeps the 2,000 cap.

## 9. Commits

1. `ipodui: dbqueue core and host test`. Owns `apps/ipodui/dbqueue_core.{c,h}` and `tests/dbqueue/`: formats, sentinel, mode and rebase tables, all-slot rebase, fatcrash cases. Agent B; tests with T.
2. `playlist: database entries behind HAVE_IPOD_UI`. Owns `apps/playlist.{c,h}`: P1-P8 (INSERT type, sentinel contract, enum before COMMENT, `T` replay, the reserve with copy, the resume pre-reserve, the `#error` under `HAVE_DIRCACHE`, `playlist_lock_ipodui()`), plus ledger rows in `docs/changes-from-rockbox.md` (orchestrator). Agent B; agent A reviews.
3. `ipodui: dbqueue runtime`. Owns `apps/ipodui/dbqueue.{c,h}` and `apps/SOURCES`: resolver, alternating payloads, `dbq.hdr`, `dbq_recover()`/`dbq_boot()`/`dbq_booted`. Agent B.
4. `ipodui: play_view queues by index`. Owns `apps/ipodui/library.{c,h}` and `apps/ipodui/ui_main.c`: the hard stop and window fallback, `grp_bits` into the dbq allocation, S1 (M1), S2 (M2), the cap comments at `library.c:3002-3004` and `:6414-6417`. Agent B.
5. `tagcache: rebase and materialize database queues`. Owns `apps/tagcache.c` (UI_COMMIT: the flag-only hook after `:5185`, the owed and booted gate before prepare, the boot-time disk SEEK pass) and `apps/ipodui/dbqueue.c`. Agent B, with its own review.
6. `tagcache, ipodui: known index at load` (optional, measured first). Owns `apps/tagcache.c` and `apps/ipodui/dbqueue.c`.
7. `ipodui: OTG play and library-source refill by index` (optional, after Q-R4-4). Owns `apps/ipodui/otg.c` and `apps/ipodui/lib_shuffle.c`.
8. `tests: dbq ui scripts and floor cases`. Owns `tests/ui/ipodui-p16-dbq-*.script` and `tests/perf/floor-50k.script` (the steady-state and peak cases). Agent T. The orchestrator updates the `docs/hardware.md` budget row.

## Revision 1: review findings → resolution

| Finding | Verified at | Resolution | Where |
|---|---|---|---|
| H1 a failed resolve ends the queue; `FLAG_DELETED` returns `NULL` | `playlist.c:3160-3161`, `playback.c:2551-2583`, `tagcache.c:10019-10020` | Accepted: the resolver always returns 0 with the sentinel `"/<dbq>/<k>"`; the `file_exists()` skip handles it; §3.1's deleted-entry claim corrected | §3.1, P1, row 5 |
| H2 history slots misread after a rebase | mode is per block (rev 0 §4.2) | Accepted: fingerprint and rebase every slot; SEEK then PATH for the whole block; `DBQ_LOST` per slot for deleted ones | §4.2.2-3, row 7 |
| H3 the reserve finds no memory | `playback.c:1095-1119`, `library.c:3218-3327`, `:342-344` | Accepted: `audio_hard_stop()` when the queue needs more capacity than it has, with the window fallback; free-only rejected as the main path because it would keep the cap in practice | §1.2, Q-R4-7, row 20 |
| H4 resume can't grow `indices[]` | `playlist.c:2072` | Accepted: pre-reserve from `dbq_total()` before the replay, plus a copying reserve in P3 | §1.2, P3, P5 |
| M1 `== -1` | `ui_main.c:178` | Accepted: `< 0` | S1 |
| M2 mutable cap in the over-cap test | `library.c:3409` | Accepted: reads the setting | S2 |
| M3 tagcache before `playlist_init()` | `main.c:875`, `:889` | Accepted: `dbq_booted` gate, owed flag at boot, a disk SEEK pass | §4.2.4 |
| M4 lock order | — | Accepted: playlist → dbq everywhere; the hook is flag plus passive reference, no mutex | §4.2.2, §3.1 |
| M5 recovered control file vs truncated payload | `playlist.c:3304-3306` | Accepted: alternating payload files plus a fresh seq per queue | §5.1-5.2 |
| M6 commit-peak margin | `hardware-fit-audit.md:300` | Accepted: 769,688 − 398,186 = 371,502 B; two 16-A cases | §1.3, Q-R4-1 |
| L1 `A:` entries are INSERT | `playlist.c:1355`, `:1513` | Accepted: database words use INSERT; P7 wording fixed; sort reasoning redone | §1.1, P7 |
| L2 enum position and guard | `playlist.c:3612`, `playlist.h:45-55` | Accepted | P3 |
| L3 `dcfrefs` size | `playlist.c:2079-2082` | Accepted: `#error` under `HAVE_DIRCACHE` | §1.2 |
| L4 movable `qtab` | — | Accepted: read into locals under the dbq mutex before I/O | §3.1 |
| L5 stack | `playlist.c:1138-1139` | Accepted: ≈ 1.9 KB below `get_track_filename()`, ≈ 620 B new over the open path; measurement plan and a 25 % gate | §3.2 |
| L6 stamp fields | — | Accepted: commitid, entries and `db_path` | §4.1, §3.1 |

## Implementation deviations (p16-q, 2026-10-21)

What the implementation (commits 546b3855a5..) does differently from rev1, and why.

1. **`audio_stop()`, not `audio_hard_stop()` (H3).** `audio_hard_stop()` frees the audio buffer but leaves `buffer_state` at INITIALIZED (`playback.c:4516-4528`), so the next `playlist_start()` buffered into freed memory: the floor gate showed playback with `audio=0`. A plain stop leaves the buffer to the stopped shrink, which marks it trashed and gives it up when the queue's memory is allocated (1.2's stopped path). The user sees the same thing.
2. **The stop also covers a slot table that doesn't fit.** `play_view()` stops the old track when the list is longer than the capacity *or* `core_allocatable()` can't hold `dbq_need(n)`: while paused, a shrink refuses the UI thread at any length (`playback.c:1095-1119`). The old queue is replaced anyway (Q-R4-7).
3. **`db_path` is 32 B in `dbq.hdr`, not 24.** `"/.rockbox/database_gen123"` needs 26 B. A longer path makes `dbq_begin()` refuse, and the queue falls back to the path queue.
4. **Only the disk passes take a transient, of 8n + 36 KB (not 4n + 32 KB).** After review M-A, the RAM rebase needs no transient: it works in place, a chunk at a time under `dbq_mtx`, and a `split` marks the slots already converted to SEEK. It holds a copy of the values (the table can be replaced or freed while the pass yields) plus the permutation, a 32 KB read chunk and a 4 KB write buffer for `dbqpath.dat`. That is 400 KB at 50k, only after a renumbering, never at the commit peak.
5. **Book queues are database queues too.** They still read their few names, for the book map's CRCs (`book_note()`); positions, CRCs and `ab_store` keys are unchanged.
6. **Path-queue fallback.** When `dbq_begin()` can't allocate (no RAM database, or no memory even after the stop), `play_view()` builds today's path queue, which keeps no album groups. The host tests (`ARTFMT_HOST`) also build the path queue: `LIB_DBQ` is off there.
7. **`dbq.hdr` keeps the previous queue's row.** It stays while it is in IDX mode and its payload file is the one the new queue didn't truncate, so a recovered previous control file resumes (M5). A mode change rewrites the other file and drops it.
8. **Resume checks the T line's seq and count, not its file field.** A mode change moves the payload to the other file, so the header row names the file; first_k must be 0 (one block per queue until commit 7).
9. **The prepare gate retries.** `ui_candidate_begin()` returns false through `ui_retry_later(UI_RETRY_BEGIN, "dbq owed")`, which backs off 2 s, 4 s and so on up to 300 s, so a long wait for memory can delay the next commit by up to that backoff after the job ends.

**The #88 window (4.2.3).** When the rebase or the materialization finds no free heap while audio holds its buffer, `job_alloc()` calls `maint_want(MAINT_COMMIT)` and takes the window's memory with `maint_take(MAINT_COMMIT, need)`, as the art thread's chapter pass does for `MAINT_CHAPTERS`. The job is part of the commit, since the next one waits for it. It calls `maint_done()` when its transient is freed, unless a commit already held the window.

**Code review of p16-q (2026-10-21).** Every finding was accepted.
- **H-A:** `reserve_unlocked()` frees the old block first when nothing is carried over. A keeping grow never runs while audio plays. `play_view()`'s stop test sums every allocation the new queue makes.
- **M-A:** "same" is decided without memory. The rebase works in place and drops the old snapshot as soon as it is done. The begin retry's back-off resets when the gate clears.
- **M-B:** `persist_mtx` serializes the file writes and the resume's reads; `dbq_mtx` is never held across I/O. Lock order: playlist → persist → dbq, and playlist → dbq for the resolver. The job no longer takes the playlist mutex, and `playlist_lock_ipodui()` is gone.
- **M-C:** S1 now treats only -1 and -19 as no-resume (`dbq_resume_refused()`). This replaces rev1's "`< 0`": a torn path queue (-15) resumes what was read, as before #109.
- **M-D:** an index past the new snapshot's entries is a mismatch, never a deletion.
- **Lows:** the sentinel guards in `playlist_peek()`, track info and playback's metadata fallback; 256 words of headroom after a long list; the stray blank line.

**The 32 MB floor minimum (floor-50k, 2026-10-21).** p16 measures 12,217,232 B; with a 50k queue, p16-q measures 6,513,240 B. The timelines show that the drop is a threshold, not the queue's own allocation:
- At the "commit deferred" update, the commit needs 7,475,184 B. p16's free heap is 7,596,576 B, so it commits while playing with 121,392 B to spare.
- With the queue resident, free heap is 7,010,168 B, so the commit is deferred to the stop. The tagcache's deferred path then loads the candidate's RAM (6.93 MB) while playing. At the stop, the commit takes 7,442,112 B from the stopped buffer, which leaves 6,513,240 B.
- p16 with only a 200 KB `floor_reserve` held, and no dbq at all, measures 6,513,248 B: the same drop. Any resident block over about 121 KB at that point causes it.
- The queue costs 405 KB on the device (4 B indices words plus the slot table). In the sim it costs 602 KB, because `unsigned long` is 8 B on the 64-bit host: indices[] takes 394 KB there. That accounts for the 180 KB that wasn't traced.
- Releasing the audio buffer before the queue is allocated doesn't help. The cold-started buffer is bounded by contiguity (13,988,712 B either way), so the queue still comes out of the hole the commit later uses.
- The fix belongs to the commit's deferred path (#85/#88), not to #109. One option is to not load the candidate RAM while playing when the commit will wait for the stop anyway, so that at the stop the commit takes only about 0.5 MB from the buffer.

**The deferred commit's candidate RAM (b5f5b413dd, 2026-10-22).** When a commit waits for the stop, the candidate's RAM cache (6.9 MB at 50k) is no longer loaded while playing. It is loaded after the commit, into the memory the commit frees.

What else used the early-loaded copy, and what happens to it now:
- **The commit:** it drops that copy anyway, and steals it as its buffer only when the copy is bigger than its need. At 50k the need is 7.48 MB and the copy is 6.93 MB, so it was never stolen. Pure cost.
- **Publish latency at the stop:** faster. The candidate-stats retries (`db cand stats rc=-1`, back-off up to 16 s) needed that RAM; now the stats run after the commit.
- **Browse and search during the deferral:** they read the active snapshot, never the candidate. Unchanged.
- **The #88 paused window:** the delayed-commit path commits in the window and then loads the RAM. Unchanged: `after_shrink` is 11,614,800 B (p16 12,217,232 B; the difference is the 50k queue's 602 KB in the sim).
- **The boot-stamp walk, the post-USB update and the initial build:** they load the active snapshot's RAM. The guard skips only a candidate with a commit pending while audio runs.
- **Art:** it follows the publication. Unchanged.
- **The deleted-files pass:** it works from the files on disk, with or without RAM.
- **When deletions publish:** while audio plays, deletions found by an update or a post-USB pass now publish at the stop or in the #88 window, not before the commit, because publishing needs the candidate's RAM. Today they are in the same commit's generation anyway.
- **Every other candidate RAM load goes through the same guard (re-review L1, L2):** the post-USB and boot pass, the reload retry, the "a commit dropped it" reload, and the delayed commit whatever it returns. A torn temp header used to clear the pending commit and leave the candidate without RAM until the next update; now it publishes after the stop.

Measured (sim, 50k):
- **Overall minimum, 32 MB:** 11,614,800 B (6,513,240 B before this commit; p16 12,217,232 B).
- **Overall minimum, 64 MB:** 45,169,232 B (40,067,672 B before; p16 45,771,664 B). The 64 MB drop had the same cause: 47,509,784 − 7,442,112 = 40,067,672 B, the stopped commit's shrink.
- **Remaining cost:** 602,432 B against p16 at both sizes, which is the queue in the sim (405 KB on the device).
- **Stop to publish, deferred commit:** about 1.45 s now, against about 14.4 s before (the retries). With p16, where the commit fits beside playing audio, the publication follows the stop by about 0.15 s.
