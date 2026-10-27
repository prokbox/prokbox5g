# Brief: Phase 16, library and maintenance

> **DRAFT, revision 3 (2026-10-20, after the brief review and the memory audit).** Not approved: the owner pre-flight (§0) is next. Based on **`integ-1020`** (`366fd190d5`: `main` 11b8a83945 + `np-select` + 8b-T); every file:line below is on that base unless it says otherwise. Branch `p16-brief`. It becomes the brief after the owner pre-flight (§0) and a `budget-reviewer` brief review. No code exists for any item below. Milestone **P16: Library & maintenance**; issues #88, #89, #87, #91, #90, #92, #100, #84, #47.

*Revision 3a (2026-10-20, brief review of rev 3: 3 High, 4 Medium, 2 Low; table at the end):*
- *#88's window never takes the audio buffer below the floor (it waits otherwise; new Q11, default no breach while paused);*
- *R5's pool (a) only for small tenants, and (b) time-boxed;*
- *R1 is readdir-only bursts, invalidated by writes, with dirty sectors winning;*
- *R6 keeps #85's A/B generations;*
- *R4 has its own section, after 16-3;*
- *the floor gate's worst cases are named, with a device floor check in P16;*
- *buffer labels fixed; line references re-anchored;*
- *issues #105–#110.*

*Revision 3 (2026-10-20, the memory audit and re-audit, owner rulings):*
- *the budget rule is now the audio-buffer floor (≥ 7.5 MB on 32 MB, ≥ 39 MB on 64 MB; `_end` ≤ 1.5 MB backstop), replacing the 11,888 B headroom caps (§6);*
- *new sub-phases from `docs/memory-audit-2026-10-20.md` and the re-audit, ordered by value and risk:*
  - *16-A the floor gate (#105), first;*
  - *16-B burst directory reads (R1, #107) and 16-C a late allocation pool (R5, #108), early because they unblock #88/#89;*
  - *16-6 the module loader spike (#106, `docs/design/module-loader.md`);*
  - *16-7 queue by index (R4, #109) and 16-8 the RAM library fit (R6, #110), each with its own brief section;*
- *Q4 is now 'a few seconds' (owner);*
- *the snapshot-first pattern and its three rebuild moments live in the boot window (16-2);*
- *links to #100 and #104.*

*Revision 2 (2026-10-20):* #100 panel rotation folded into 16-4 beside #92 (they share `panels.pak` v2): a larger on-disk pool, the set re-chosen at each restart and every 3 h, books gain a random share; Q8–Q10 added.*

*Revision 1 (2026-10-20, brief review: 2 High, 5 Medium, 3 Low; every finding applied inline, table at the end). Main changes: the stamp never writes at USB (a RAM flag) and trusts a directory-entry digest, not a shutdown alone (H1); the shrink's real semantics: a partial shrink only, the refill happens at the shrink, a hard shrink loses the queue (H2); per-site replace plans and a named boot recovery order (M1–M3); per-sub-phase size caps (M4); lazy signature migration (M5).*

*Process: `docs/process.md` (brief template, checklist 1–13, lean rules 1–11, one gate per machine). Open owner questions are frozen as ⚠️ with a default (§0.1); the brief is not revised mid-phase for them. Split by risk: the two playback-touching and crash-consistency items are their own mini-phases with their own review, ahead of the art and UI items.*

**Goal (owner, 2026-10-19):** *"iPods barely are ever in the 'stopped' mode … this process runs so infrequently and so quickly, at the end of the day, that it is hardly noticed. I'd love it to be quick enough to happen prior to boot up."* Today library and artwork maintenance is designed around a stop that a 7G-style user never makes, a no-change check costs ~18 s at every boot, and four writers still replace files in a way that left duplicate directory entries on the device. P16 makes maintenance:

1. **Land while paused** (#88). An update found while paused publishes after a short paused-and-dark wait, by taking part of the paused audio buffer (a partial shrink: playback restarts at the exact position and pauses again at once, refilling a smaller buffer then) (Revision 1, H2). The same window lets the chapter-art books stop waiting for a stop.
2. **Cost almost nothing at a no-change boot** (#89). A clean shutdown with no USB session since, and a matching directory-entry digest, skip the probe walk; anything else walks, faster than today, measured first (Revision 1, H1).
3. **Hide inside the boot screen** (#87), boosted and time-capped, then continue in the background; later behind instant resume (#86, P18).
4. **Never leave duplicate entries** (#91). Every replace in the shell either removes first with a recovery path or uses a two-phase replace that boot recovers.
5. **Rebuild only what changed** (#90), **keep several covers per set** (#92) so the drift varies while paused without waking storage, and **rotate the sets** (#100, owner 2026-10-20: *"Refresh the set on a timer, maybe every 3hrs, or on restart -- we want people to not necessarily constantly see the same albums on there, or the same audiobook covers."*).
6. **Feel stock after USB** (#84): a brief busy moment hosting the post-USB scan and read-only housekeeping, with the library browsable throughout; and one range read per artist list (#47).

---

## 0. Owner pre-flight

### 0.1 Open questions (frozen ⚠️ with a default until the owner round)

| # | Question | ⚠️ Default | Why it matters |
|---|---|---|---|
| **Q1** | When may maintenance take the paused audio buffer? | **Paused ≥ 3 min and the user idle** (`user_idle()`: unlit face or backlight off, no key held; `artcache.c` :228–235), never while playing, never on the charger with the screen lit and a key in the last `INPUT_QUIET` | Too eager stops the paused queue under the user's thumb; too late and the library lags behind a long pause |
| **Q2** | The window restarts the paused queue: the codec restarts at the exact position and pauses again at once, and the refill happens **then** (in the dark), not at PLAY; the buffer stays smaller until the next `audio_reset_buffer()` (Revision 1, H2). Acceptable? | **Yes**, if PLAY afterwards starts as fast as a normal resume (`m0 tap2pcm` 190–440 ms, `ipodui.log` 2026-10-19), with no pop, the DAC staying in standby through the restart, and the audiobook resume record and statistics unchanged | The user-visible cost of #88: one disk wake while dark, and a smaller buffer until the next track start |
| **Q3** | Skip the boot walk when the stamp is clean, no USB session happened since the last completed walk, and the directory-entry digest matches? | **Yes**, with a forced walk after an unclean end, every 10th boot, and from Rebuild Cache. *Superseded 2026-10-23 by the owner: the stamp is written when the walk settles and removed at USB connect, before the ack; a crash without a USB session no longer forces a walk; the 10th-boot backstop stays.* (npfix) | The digest (Revision 1, H1) catches edits made while ProkBox wasn't running (Apple disk mode, a card reader); an in-place tag edit that keeps size and mtime is the residual, bounded by the 10th-boot walk |
| **Q4** | How long may the boot screen grow for maintenance? | **A few seconds** (owner 2026-10-20: *"We can, on restart, hold on the boot screen for a few seconds… The Apple firmware spends up to 5-10 seconds before letting the user into the main menu"*), time-capped (a named constant, start 3 s), then hand over and continue in the background (the lists stay browsable: generations, #85); full boots become rare with #86 | The boot hold also hosts snapshot rebuilds (16-2) and the panel rotation (#100) |
| **Q5** | Covers per set kept resident (#92)? | **3 on 64 MB; on 32 MB, 2 only if the USB prefetch ring keeps its 8 MB size, else 1** (Revision 1, L1; Revision 3a: N = 2 is 720 KB in total, +360 KB over 8b's two slots): 2 sets × N × 180,000 B = 1.08 MB (2.2 % of the 48.6 MB audio file buffer at 50k; +720 KB over 8b's two slots; the floor is 39 MB); on 32 MB the audio buffer is 15.1 MB at 50k (floor 7.5 MB), but 720 KB on top of the commit peak's 8.27 MB free heap pushes the 8 MB USB ring (`hardware.md`, #81) to its 4 MB fallback, so the slots are sized after the ring's need is known | Variety while paused vs audio buffer |
| **Q6** | #84 housekeeping: what may be removed? | **Count `._*` sidecars only (logged); delete nothing** until the owner says yes (Revision 1, L3). If yes later: only `._*` files in music folders whose data file exists beside them; `.Trashes`, `.Spotlight-V100`, `System Volume Information` are never touched. FSInfo free count corrected only if it differs | Standing rule: checks only, never a repair of the user's volume; no media deleted |
| **Q7** | #84 look | **A small activity glyph in the header** (the 8-dot glyph, 8b material kit) while the post-USB scan runs, plus the 8b footer only if a list is truly empty; never a blocking screen | Stock shows a spinner after disconnect (#84 body) |
| **Q8** | Rotation interval (#100) | **At every restart and every 3 h of uptime** (`PANEL_ROTATE_TICKS`, a named constant) | Too often churns covers mid-listen; too rarely and the owner sees the same albums for weeks (today: only at a library change) |
| **Q9** | On-disk pool size | **~100 albums** (≈ 18 MB of `panels.pak` at 180,000 B each), lowered automatically when free space is under 1 GB (flash and HDD alike) | Disk use vs variety |
| **Q11** | May #88's paused window take the audio buffer below the floor (7.5 MB on 32 MB, 39 MB on 64 MB) for a library update? (Revision 3a, H1) | **No.** The window allocates only if the audio buffer **after** the shrink is still ≥ the floor; otherwise the update waits (for a stop, or for a smaller commit). On 32 MB at 50k the headroom is ≈ 7.58 MB (15,079,816 − 7,500,000) against a commit's ~7 MB extra, so large updates usually wait there; 64 MB has ≈ 9.63 MB | A breach would make every later refill smaller until `audio_reset_buffer()`, against the owner's floor |
| **Q10** | Books' random share | **Up to 3 of the 8 book slots** random, after recently played and newest (today books have no random share) | Variety for audiobook listeners vs keeping the current book visible |

### 0.2 Captures (stock 7G, #26 rules: read-only)
- C1: video of the stock 7G right after a USB disconnect (spinner position, duration, what's browsable).
- C2: stock boot to the main menu, timed (for Q4).

---

## 1. What already exists (read it; reuse it, do not rebuild)

| Area | Where | Reuse |
|---|---|---|
| **Commit while audio holds memory** | `ui_commit_buffer()` `tagcache.c` :4515–4575: while audio is on, the commit waits (`ui_commit_pending`, "commit waits for playback to stop") unless the reload and the commit fit in free memory; retried each second; `ui_generation_buffer()` :4838 checks right before `core_alloc_ex` | #88 adds one predicate to these two checks; no new allocator |
| **Publishing a candidate** | `ui_candidate_publish()` :5012 (needs `tc_stat.ramcache` loaded, `ui_retry_due`, the pool prepared); the active library stays readable during an update (lists read the published context, `library.c` `lib_state()` :2230–2238; 8b brief rev 3 O12) | Unchanged; #88 only lets it run while paused |
| **Taking memory from paused playback** | `playback.c` `shrink_callback()` :1014–1112: an allocation that needs the audio buffer stops playback and, **under `HAVE_IPOD_UI`, resumes paused** at `id3->elapsed/offset` (:1061–1068, P6 fix round item 4); `give_up` frees the buffer (`AUDIOBUF_STATE_TRASHED`, :1108–1111) | #88 and the chapter books use exactly this, deliberately, once per window |
| **Paused/idle policy** | `artcache.c` `user_idle()` :228–235, `io_ok()` :239, `art_hold()` :248 | Q1's predicate is built from these, shared by #88 and the art builder (one definition) |
| **Chapter books waiting for a stop** | `artcache.c` :3596–3611: paused, a book with chapter images needs `chap_bytes` > the idle decode block and returns `CHAP_LATER` ("a book with images waits for a stop") | Runs inside the same window |
| **Boot / post-USB check** | `tagcache.c` thread :10255–10295: `check_file_refs()` (after USB only, `ui_rescan` :10131), then `tagcache_build()` → the read-only probe walk (`tagcache_build` :9255–9310, lazy clone, `ui_probe_clean`), `check_deleted_files()` after USB; timing logged as `db scan ms=` :9323 | #89 decides whether the probe walk runs at all; #87 moves it into the boot window |
| **Scan speed work** | `docs/handoff-2026-10-19-scan-speed.md` (`._` skip, scan name table, one master fd, lazy clone; dircache "evaluated, not recommended now", fix 8) | 16-0 measures what the remaining 18 s is |
| **Clean shutdown** | `tagcache_shutdown()` :10427–10445 (statistics flush, `db off done=` log); `ui_main.c` power-off path | #89's stamp is written here, after the flush |
| **Library window log** | `ui_main.c` :777–800 (`lib window empty_ms= total_ms=`, T6 fix in 8b-T) | Device evidence for #87/#88 |
| **Rename-over undo** | `firmware/common/file.c` :1010–1174 (`rbfs_rename_fault {rc, undo}`, victim removal undone on failure, 5293bbb415) | #91 relies on it as the last line of defence, not as the design |
| **Safe replace already done** | `artfmt.c` :341–355 (`artfile_publish`: prev removed first, rename, restore on failure); `settings.c` :153–155 (`.old` removed first under `HAVE_IPOD_UI`) | #91's pattern for the others |
| **Panel art** | `preview.c` `load_first()` :490–519 (`woke_set` :101/:504–509), one 300×300 RGB565 slot (180,000 B) from `ui_preview_reserve`; panels.pak (`ARTPANEL_VERSION 1`, `artfmt.h` :313), sets interleaved; 8b adds slot 2 and the `woke_set` fix (`p8b-4a`, #73) | #92 builds on 8b's slot 2 (dependency: 8b merged first) |
| **Panel set selection (#100)** | `library.c` `pre_panels()` :4835–4880: music = recently played (`PANEL_PLAYED`), newest (`PANEL_ADDED`), then random by rendezvous hashing with a seed that moves only **every 8 commits** (:4873, `tagcache_ui_commitid_ctx(ctx) >> 3`); books = recently played and newest only (`PANEL_BOOKS` = 8, no random share); chosen only at a library precompute. `artcache.h` :150–160: at most `LIBFILE_PANELS_MAX` (32) albums, 300×300 RGB565 (180,000 B) each in `panels.pak`, built by the art builder at the start of a pass | #100 keeps the precompute's choice as the *pool* order and moves the per-rotation pick into the shell |
| **Art keys and versions** | `artkey.h` :54 `artcache_key()`, `ARTIDX_VERSION` / `ARTPAK_VERSION` / `ARTPANEL_VERSION` (`artfmt.h` :141/:285/:313) | #90 adds a source signature beside the key, not a new key |
| **USB release** | `usbscreen.c` :1115 (`SYS_USB_DISCONNECTED`), log `usb release tick= scan_pending=` | #84's busy moment starts here |

## 2. Out of scope
- Sleep and instant resume (#86, P18); #87 only leaves the hook for "at wake".
- Native macOS mount and the cluster migration (#81, P17); FSInfo is only *checked/corrected* here (#84), never the FAT layout.
- dircache's return (scan-speed fix 8: evaluated, not recommended now).
- The host-side log pull-and-clear (#96, Tooling): mentioned because it removes the logs P16 adds, nothing else.
- Any repair of the user's volume, and deleting anything that could be media.

## 3. Contracts (committed to `p16` before fan-out, with host tests and one shared stub)

### 3.1 `apps/ipodui/maint.h` (new): the paused maintenance window
```c
/* One policy for every job that needs memory or a disk wake while the
 * user is not listening: #88's commit and reload, the chapter books. */
enum maint_job { MAINT_COMMIT, MAINT_CHAPTERS, MAINT__N };
bool maint_window_open(void);          /* Q1: paused >= MAINT_PAUSE_TICKS and user_idle(); stopped: true */
void maint_want(enum maint_job j);     /* a job is owed (any thread; one bit) */
bool maint_wanted(void);
void maint_done(enum maint_job j, bool ok);  /* logs "maint job=… ok=… ms=…" */
long maint_paused_since(void);         /* tick of the last pause, -1 playing/stopped */
```
- **Identity vs validity:** the window is re-checked **immediately before** each allocation (checklist 2), never cached across a yield.
- **Trace:** `UPDATE_TRACE("maint …")` (sim) and `ui_log("maint", …)` (device); a trace rule that finds no `maint` line fails.
- Host test `tests/maint/`: the predicate over a table of (status, pause age, backlight, unlit, key, charger) rows.

### 3.2 `apps/ipodui/bootstamp.h` (new): the no-change boot (#89) (Revision 1, H1)
```c
struct boot_stamp {            /* /.rockbox/boot_stamp.dat, CRC'd */
    uint32_t magic, version;
    uint32_t fsinfo_free, fsinfo_next;  /* FAT FSInfo at the last walk */
    uint32_t dir_digest;                /* CRC over (name, size, mtime,
                                         * first cluster) of every entry in
                                         * the music tree, raw directory
                                         * reads, no file opens */
    uint32_t db_generation;             /* the active generation (#85) */
    uint16_t boots_skipped;             /* Q3: forced walk at 10 */
    uint16_t flags;                     /* CLEAN_SHUTDOWN */
    uint32_t crc;
};
bool boot_stamp_trust(void);   /* at boot, before the tagcache thread starts:
                                * read the stamp, then REMOVE it (spoil);
                                * true only if CLEAN, the digest matches a
                                * fresh one, and boots_skipped < 10 */
void boot_stamp_usb_seen(void);/* the USB event handler: a RAM flag only,
                                * usb_since_walk = true; no I/O (memory rule
                                * "no disk writes at USB screen entry") */
void boot_stamp_clean(void);   /* tagcache_shutdown(), after the flush: writes
                                * only if !usb_since_walk (cleared when a walk
                                * completes) and never while
                                * tagcache_usb_quiescing */
```
- **No write at USB connect or entry** (H1): the only writes are the remove at boot and the write at a clean shutdown.
- **Digest cost:** ~418 directories × ~0.4 ms per directory read ≈ 0.2–0.5 s on flash, measured in 16-0 before 16-2 commits to it; a digest slower than ~1 s drops back to "skip only when clean and no USB" with the 10th-boot walk (⚠️ in the owner round with the number).
- Written by remove-then-rename of a temporary (§3.3), never in place.
- Host test: round trip, CRC, every flag combination → trust/no trust; the digest over a synthetic tree (rename, size, mtime, added and removed entries all change it).

### 3.3 `apps/ipodui/safefile.h` (new): one replace primitive (#91)
```c
/* Replace dest with tmp so that a power cut at any point leaves either
 * the old or the new file, never two directory entries and never none:
 *   1. write and close tmp; 2. remove dest.prev; 3. rename dest -> dest.prev;
 *   4. rename tmp -> dest; 5. remove dest.prev.
 * Boot recovery (safefile_recover()): dest missing and dest.prev present
 * -> rename prev back; tmp present -> remove it. */
int  safefile_replace(const char *tmp, const char *dest, const char *prev);
void safefile_recover(const char *dest, const char *tmp, const char *prev);
```
- Never renames over an existing name (no reliance on `rbfs_rename`'s overwrite path).
- **Boot recovery order** (Revision 1, M3): `safefile_recover()` for the playlist control file runs **before** `playlist_resume()`; the journal's and the generation's recovery run **before** the tagcache thread opens them (`dbgen` select, journal open); the audiobook store's before `ab_store` loads; On-The-Go's before `otg` loads. Each is named in the sub-phase that adds it, with a script that cuts power between steps 3 and 4.
- Host test with the `tests/fatcrash` crash model: a cut after every write, every path ends with exactly one valid `dest` and no duplicate entry.

## 4. Work items, by sub-phase

### 4.0 16-0 Measure first (agent T, tooling; sonnet)
- **Scan breakdown on the device** (devmode `ui_log`): directories, entries, failed opens, filename-table lookups, ms per stage of the probe walk. Today's line `db scan ms=18380 n=1451` (`ipodui.log`, boot 21:09, 2026-10-19) counts tracks, not folders: the volume holds 418 directories and 3,801 entries (host scan 2026-10-19). The handoff's per-read cost (~0.35–0.41 ms, single sectors) predicts the order of magnitude; 16-2 needs the split.
- **Pause/resume cost**: `m0 tap2pcm` after a window (trashed buffer), on flash; sim stand-in with the device's timing (checklist 4).
- **Sim commands** (own `sim_cmd_maint.c`): `pause_age <s>`, `user_idle <0|1>`, `maint_expect <job> <ok>`, `boot_unclean`; scripts per §7.

### 4.0a 16-A The audio-buffer floor gate (#105) (agent T, tooling; **first**)
- `scripts/validate.sh`: boot the 32 MB and 64 MB sims with the 50k library (`tests/perf/lib50k.sh`), log the audio file buffer after shell start and at a commit peak (a sim command), and fail under the floor (7,500,000 / 39,000,000 B).
- Replace `P9_END_MAX` and the `P8B_*` gates with the backstops (`_end` ≤ 1,500,000 B, `rockbox.ipod` ≤ ~1,250,000 B).
- Print the gap before `inquiry_static`, and fix sizemap's merged string-pool attribution (RUI-0).
- **Worst cases traced (Revision 3a, M3):**
  - after #88's shrink;
  - with `chap_bytes` allocated;
  - with the 785,844 B panel transient, now also possible in paused windows through §4.4's top-up;
  - with the loader arena;
  - with Now Playing open.

  The USB prefetch ring is excluded, because it is taken only while audio is idle.
- **A device floor check inside P16** (the developer buffer view, at the end of 16-1 and of 16-4), not only #111 in P10.
- **Device twin:** the buffer line in the developer buffer view (M1, #111).

### 4.0b 16-B Burst directory reads (R1, #107) (agent A firmware-platform, opus; own review)
- **Why:** `fat.c:2852-2862` reads a directory one 512 B sector per ATA command, and dircache is compiled out. Each path walk, scan `check_dir`, post-USB deleted-file entry and art lookup pays about 16 entries per command, at an estimated 1-3 ms each.
- **What:** read directory clusters in bursts into a small per-scan buffer. The burst size comes from 16-0's or M1's measured per-command latency.
- **Expected gain:** est ÷8-32 commands. It directly shortens #89's walk and #84's post-USB pass (the device saw 42-140 s rescans on 2026-10-20).
- **Rules:** guarded under `HAVE_IPOD_UI`, with a ledger row.
- **Coherence (Revision 3a, H3): not read-only safe as first written.**
  - `fat_readdir`'s `filestr_cache` (`fat.c:2851-2862`) stays coherent with directory writes through `file.c`'s stream sync.
  - The `dc_*` write-back holds dirty sectors (`fat.c:385-412`).
  - A burst buffer that bypasses both would serve stale entries, and a free-slot search over them would recreate duplicate entries (#91).
- **So R1 is:**
  - **readdir-only bursts** (never the free-slot or create path);
  - **invalidated on any write to that directory** (create, rename, remove, size/mtime update);
  - **dirty disk-cache sectors always win** over burst copies.
- **Tests:** a fatcrash/host test that writes into a directory during a scan and checks no stale or duplicate entry. Device: commands per scan and per-command latency (#111).

### 4.0c 16-C A late allocation pool, or a shrink without a stop (R5, #108) (agent B playback-audiobook, opus; own review)
- **Why:** the audio buffer is a buflib block whose shrink stops playback (`playback.c:1054`). #88's window (16-1) is built on a partial shrink, so this item is designed **before** 16-1, and 16-1 uses its result.
- **Option (a):** a reserved late pool for on-demand allocations: the loader arena, the #92 slots, lazy reservations.
- **Option (b):** a shrink that keeps playing.
- **Choice:** measured first. If neither is ready, 16-1 proceeds with the partial-shrink design in §4.1.
- **What R5 can and can't do (Revision 3a, H2):** R5 alone does **not** unblock #88.
  - Pool (a) is reserved at shell start, so it costs floor permanently and can't hold a ~7 MB commit. It is only for **small tenants**: the 64 KB loader arena, the #92 slots, `chap_bytes`.
  - #88 needs (b), a shrink that keeps playing, or the existing partial shrink.
  - **(b) is time-boxed** (one design spike, its own review). If it isn't proven, 16-1 ships on the partial shrink with the floor rule.

### 4.1 16-1 Updates while paused (#88) + chapter books (agent B playback-audiobook, **opus**; own review before 16-2 merges)
- **Floor rule (Revision 3a, H1; Q11):** the partial shrink today checks only `need + AUDIO_BUFFER_RESERVE` (256 KB, `playback.c:910`). The window must also check that the audio file buffer **after** the shrink stays ≥ the floor (7,500,000 / 39,000,000 B) and otherwise not open (the update waits for a stop or a smaller commit, logged). The buffer stays smaller until `audio_reset_buffer()`, so the check covers the whole time until then. Script: `ipodui-p16-paused-floor` (32 MB sim, 50k: the window declines; 64 MB: it opens).
- `ui_commit_buffer()` :4552–4575 and `ui_generation_buffer()` :4838: while audio is on, **if `maint_window_open()`**, allocate anyway, **only with `core_alloc_ex()` and front/back shrink hints, never `core_alloc_maximum()` or `allocate_tempbuf()` while paused** (Revision 1, H2). Re-check the window right before the call (no yield between).
- **What the shrink really does** (H2, `playback.c` :1106–1139): a *partial* shrink (`core_shrink` front or back, :1115–1126) posts `Q_AUDIO_PLAY` and, under `HAVE_IPOD_UI`, `Q_AUDIO_PAUSE` at once (:1127–1135): the codec restarts at `id3->elapsed/offset`, pauses, and **refills a smaller buffer immediately** (the disk read happens at the shrink, in the dark), and the buffer stays smaller until the next `audio_reset_buffer()`. A *hard* shrink (`give_up`, :1106–1111, when the buffer would fall under `AUDIO_BUFFER_RESERVE`) returns **before** the resume is posted and loses the paused queue: the window must never ask for that much (need + reserve checked first; otherwise the commit keeps waiting).
- **Never open the window** during video (BCM playback), during a P15 fade or a crossfade; the script checks the audiobook resume record (`ab_store`), the statistics journal and the DAC standby (no pop, no un-standby) are unchanged by the restart.
- **PLAY during a commit** plays from the smaller buffer; the commit continues; the log says so (`maint job=commit play_during=1`).
- The reload after the commit (`load_ramcache`) and the candidate's RAM cache use the same window; the published generation then holds memory the audio buffer gets back at the next `audio_reset_buffer()` (play): state it in the log.
- `artcache.c` :3626–3635: `CHAP_LATER` books run their `core_alloc(chap_bytes)` inside the window (`maint_want(MAINT_CHAPTERS)`), freed before the window closes.
- One window serves both jobs back to back (one buffer loss, not two).
- **What the user sees** (checklist 10): paused, dark: nothing (one disk wake for the restart's refill); on PLAY: a normal resume, then the updated lists. Playing: as today (the update waits). Stopped: as today (immediate), now also for the chapter books (`maint_window_open()` is true when stopped: a script for it, L2).

### 4.2 16-2 The no-change boot (#89) and the boot window (#87) (agent A firmware-platform, **opus**; own review)
- **The boot window's jobs (Revision 3, owner):** the stamp check and walk, the art top-up, the panel rotation (#100) and the **snapshot batch** for on-demand full screens (`docs/design/module-loader.md`; never the main menu or split lists). All within Q4's few seconds, boosted.
- **The same batch routine** runs in the post-USB busy moment (#84) and behind a visible "Updating Language…" after a language change.
- `bootstamp` (§3.2): read and removed at boot before the tagcache thread starts; the USB handler sets the RAM flag only; written in `tagcache_shutdown()` :10427 after `ui_shutdown_flush_done` when no USB session happened since the last completed walk; the thread's check (:10255–10295) skips `tagcache_build()`'s probe when `boot_stamp_trust()` holds (digest, FSInfo pair and active generation match).
- When the walk runs, apply what 16-0 finds (e.g. the two failed ignore-file opens per directory, `handoff-…-scan-speed.md` :110–111, cached per boot).
- **Boot window (#87):** start the check boosted while the boot screen shows; hold the screen up to Q4's cap if the check is nearly done (progress known from the directory count); otherwise hand over. Art top-up (`artcache` pass) starts after, as today.
- Hook for P18: `maint_boot_window()` callable at wake.

### 4.3 16-3 Safe replaces (#91) (agent A, sonnet for the call-site edits after A writes `safefile`; own review)
Call sites still renaming over an existing name (`rename(` on `integ-1020`):

| File:line | What | Plan |
|---|---|---|
| `dbgen.c` :690 | generation publication (the manifest A/B pair) | **Remove only the stale manifest of the pair, then rename** (the other manifest is the recovery; no three-rename) (Revision 1, M2) |
| `dbjournal.c` :513, :578, :753 | journal rotation, checkpoint, export (:734 already removes first, without recovery) | `safefile_replace` + recovery at journal open |
| `otg.c` :387, :677, :785 | On-The-Go index, list, saved playlist | `safefile_replace`; recovery at `otg` load |
| `playlist.c` :410 | control file → `.old` (**the device's duplicate `.playlist_control.old`, 2026-10-19 18:45**) | remove `.old` first under `HAVE_IPOD_UI` (no recovery needed: a backup copy) |
| `playlist.c` :3704, :4234 | control file swap | `safefile_replace` under `HAVE_IPOD_UI`; :3704 keeps upstream's meaning (the swap of a working control file) (M1); ledger rows |
| `playlist.c` :4067 | saved playlist write, **in the user's own folders** | remove-then-rename, the temporary as the only residue; recovery for `/Playlists` only, never a scan of user folders (Revision 1, M1) |
| `ab_store.c` :175, :545 | audiobook store | `safefile_replace`; recovery at store load |

- Crash tests (`tests/fatcrash`, the victim-fail harness): each site, a cut after every write.

### 4.4 16-4 Art: signatures (#90) and several covers per set (#92) (agent C ui-fidelity / art, opus for the format)
- **#90:** each record keeps a 32-bit signature of its source (size, mtime and a CRC of the first 4 KB of embedded art or the cover file): a pass rebuilds a record only when the signature changes; `ARTIDX_VERSION` 2; existing records are trusted and their signatures **filled lazily inside maintenance windows under a per-window cap** (no rebuild storm, never at boot) (Revision 1, M5).
  - *As shipped (2026-10-22, #90 review M1/M2):* no format change (`ARTIDX_VERSION` stays): the record's existing kind, path hash, size and CRC fields form the signature, and a spare bit of `src_type` (`ARTREC_T_SIG2`) marks new-style CRCs over the source's **first 8 KB and last 8 KB** (all of it up to 16 KB), written by every album build and by the lazy fill. Older records keep their first-16 KB CRC and are compared in that style (no record mismatches on upgrade, no rebuild storm). The fill cap is per window (16), and a record the fill cannot sign is marked `ARTREC_T_UNSIGNED` (tried once, trusted), so the owed walk ends. **Known limits:** no source mtime is stored, so a same-size edit inside the unread middle of a source larger than 16 KB (SIG2), or past the first 16 KB of an old-style record (a bottom-up BMP edited in its top rows, an embedded JPEG changed past 16 KB), is only picked up by Rebuild Cache or a size change.
- **#92:** `panels.pak` grouped per set (`ARTPANEL_VERSION` 2: a header per set, covers contiguous) so a set refresh is **one** read of N × 180,000 B; the drift keeps N covers per set (Q5) in slots allocated at shell start (8b's two slots become 2N), rotates through them while paused without storage, refreshes a set on the next natural disk wake. Fallback to 8b's two slots if the allocation fails (32 MB, low memory). `panels.pak` v2 is rebuilt **from the existing art cache inside a maintenance window**, never at boot (M5); the slots are sized after the USB ring's need (L1).
- **#100 rotation (Revision 2):** `panels.pak` v2 holds a **pool** (Q9, ~100 albums plus up to ~20 books, ordered by the precompute: played, newest, then the rendezvous order with a seed no longer tied to commits) and a small **set index** (which pool entries form the current music and book sets, 32 + 8). At each restart and every `PANEL_ROTATE_TICKS` (3 h, Q8) of uptime the shell re-picks the set from the pool: recently played and newest first, then fresh random picks that avoid the previous set's random picks, for music **and** books (Q10). A rotation does **no image work**: it writes the set index (one sector, §3.3 replace) and bumps `panel_seq` so readers reload; the drift's resident covers (#92) refresh at the next natural disk wake. The art builder **tops the pool up** only inside maintenance windows (§3.1), never at boot or while playing, one album per window step (each ~180 KB decode and write, the same band decode as today, `artcache.h` :160–163). The set index lives with the pool so a rotation survives a power cut (old or new index, never neither).
  - *As shipped (2026-10-22):* the pool's own covers are in `panelpool.pak` (in place, free slots only) and the set index is `panelpool.idx` (safefile); `panels.pak` stays the 32-entry default set an 8b build reads (#92 review M1), maintained as before. The pool order is appended to `LIBFILE_CAT_PANELSET` col 1 (no new section, `LIBFILE_VERSION` unchanged). Rotation time is RTC wall clock (P18 Q8); it runs only at the boot's first pass or a natural storage wake. Music keeps 10 random places whatever the books take (`ARTPOOL_MUSIC_RAND`). Details: `docs/design/artcache.md` §7.

### 4.5 16-5 USB moment (#84) and range reads (#47) (agents D ui-fidelity, B)
- **#84:** from `usbscreen.c` :1115 on disconnect: the header activity glyph (Q7) while `scan_pending`; housekeeping in the same awake window: FSInfo checked against the FAT count the scan already has, corrected if different (one sector write, logged); `._*` sidecars counted per Q6 (deletion only after the owner's yes). No repair, no media.
- **#47:** artist album lists from one range read of the precompute file (`library.c`, the `tracks_hdr` section reader as the model), counted per open (lean rule 10).

### 4.6 16-6 The module loader spike (#106) (agent A firmware-platform + T tooling; opus)
- Our own loader (`docs/design/module-loader.md`), not Rockbox's plugin path:
  - Thumb modules linked with `--just-symbols`;
  - one 64 KB transient arena reserved at shell start (or from 16-C's pool) and counted in the floor;
  - `modules.pak` read with one aligned DMA read on an existing worker;
  - build-ID and Fletcher-32 checks;
  - static linking in the sim.
- **Tenant A:** the cold Settings pack, drawn live.
- **Tenant B:** a ProkBox "Loader & storage" developer screen, in the black-and-white disk-mode look (owner), as the first snapshot (seeds #103).
- **Snapshot batch:** shared with 16-2's boot window, the post-USB moment and "Updating Language…".
- **Gates:** pixel-identical screens; `_end` −12 KB; the floor holds; zero splashes; boost balanced.
- **Device:** load latency over 50 entries while playing, 0 underruns, an `lc_open` A/B; the batch time.
- **Intent preload** (highlight → load) shares its "reachable next" map with #104 (P17).

### 4.7 16-7 Queue by database index (R4, #109) (agent B; own brief section, large; **after 16-3**)
- **Today:** `lib_play` and Shuffle Songs build Rockbox's path playlist through `play_begin()`/`play_add()` (`library.c:3075-3093`, `playlist_insert_context_*`), up to 2,000 names. The 2,000 cap and its cost are described in the comment at `library.c:6407-6410`.
- **Change:** queue by tagcache index, so play-all and shuffle start in ms and the 2,000 cap goes.
- **Requirements (Revision 3a, M2):**
  - **Resume by index** is tied to a **library generation id**, with a filename fallback after a commit changes indices.
  - **References** from On-The-Go, bookmarks and `ab_store` (paths today) keep working, with the same generation rule.
  - **The control-file path stays as the fallback** (and for files outside the library).
  - **Order:** after 16-3, which touches the same `playlist.c` swaps (:3704, :4234).
- **Own review.** Device: `m0 tap2pcm why=play` on a 2,000-row Shuffle; resume after a commit that renumbers.

### 4.8 16-8 Fit the RAM library (R6, #110) (agent B; own brief section, large, high risk)
- **Moved to P17 (owner, 2026-10-22):** not urgent: the 32 MB floor breach seen with #109 was the deferred commit loading its RAM while playing (fixed in b5f5b413dd), and both floors pass with margin. P16 closes without it; the section below stays as its starting brief.
- **Goal:** reduce the commit peak: 13.9 MB at 50k with two snapshots, minimum free heap 8.27 MB on 32 MB. Est −2.4 to −7 MB by keeping only the fields the shell reads and a packed index.
- **Keep #85's A/B generations (Revision 3a, M1).** `lib_state` (`library.c:2230`) keeps the published library readable during an update, so shrink both snapshots by packing. Any single-snapshot design needs its own review and an owner question.
- **Why it matters:** under the floor rule the audio buffer already holds, so this decides whether updates finish while paused or playing instead of waiting.
- **Order:** last in P16, after 16-1 shows how often updates wait.

## 5. Ownership and read lists

| Agent | Model | Owns | Reads (≤ 3) |
|---|---|---|---|
| T tooling | sonnet | `sim_cmd_maint.c`, `tests/maint`, `tests/bootstamp`, scripts | this brief §3, `tests/ui/README.md`, `handoff-…-scan-speed.md` |
| B playback-audiobook | opus | `tagcache.c` (UI_COMMIT paths), `maint.c`, `library.c` (#47) | §3.1, `playback.c` :1014–1112, `docs/hardware.md` memory rows |
| A firmware-platform | opus (16-2), sonnet (16-3 sites) | `bootstamp.c`, `safefile.c`, call sites in §4.3, `playlist.c` guards | §3.2–3.3, `handoff-…-crash-fat.md`, `firmware/common/file.c` :1010–1174 |
| C art | opus | `artcache.c`, `artfmt.*`, `preview.c` (#92) | §4.4, `artfmt.h`, 8b's `p8b-4a` slot-2 notes |
| D ui-fidelity | sonnet | `usbscreen.c`, header glyph | §4.5, spec `music-lists.md`, #84 |

Orchestrator: `docs/hardware.md` budget table, ledgers, ROADMAP.

## 6. Device-reality checklist
1. **Compiled features:** dircache off in the shell (Phase 9); the sim takes the same probe path. Stamp code only under `HAVE_IPOD_UI`.
2. **Memory:** #88 is a deliberate allocation while paused, through the shrink (resume paused); checked immediately before, inside the window only; the space returns at the next `audio_reset_buffer()`. #92: 2N slots at shell start only (never while paused): 1.08 MB at N = 3 (720 KB more than 8b's two slots), from the audio file buffer (48.6 MB on 64 MB at 50k; 41.8 MB is the minimum free heap at the commit peak, not the audio buffer); N = 2 on 32 MB is 720 KB in total, +360 KB over 8b's two slots (15.1 MB audio buffer; 8.27 MB free heap at the commit peak; Q5). No new resident statics over 1 KB; `_end` estimate below.
3. **CPU:** the window's commit and reload run boosted (as today's stopped commit); the boot window boosted; nothing per frame.
4. **Storage:** #88 adds one disk wake per window (the commit; it would happen at the next stop anyway); #89 removes ~18 s of reads per clean boot; #92 replaces per-change 180 KB reads with one bulk read per set refresh at a natural wake.
5. **LCD:** only the header glyph (#84) and the boot screen (unchanged pixels); partial header pushes.
6. **Threads:** the commit runs on the tagcache thread; the window predicate is read there; `maint_want` from the art thread; the shrink stops audio from the allocating thread (`shrink_callback` handles both cases).
7. **Splashes:** none (playback's resume path under `HAVE_IPOD_UI` logs).
8. **Persistence:** a power cut inside the window: the old generation stays (generations, #85); `safefile` recovery; the stamp spoiled at boot. USB mid-window: `tagcache_usb_quiescing` cuts the commit as today; the stamp is spoiled.
9. **Scale:** 50k tracks: commit and reload sizes from `ui_commit_need()`; the window must fit them in the freed audio buffer (64 MB: yes; 32 MB: measure with `tests/perf/lib50k.sh`).
10. **Deferred work, per state:** #88 in *ready* (update found while paused), *fresh* (initial build while paused: the window lets it finish), *dirty* (USB cut) — one script each; #89 in clean, unclean, after-USB, 10th boot.
11. **Stock unchanged:** `playlist.c` edits under `HAVE_IPOD_UI`; ledger rows.
12. **Existing paths doing the same job:** upstream's autoupdate (`tagcache_autoupdate`) stays the trigger; the stamp only skips the probe; `rbfs_rename`'s undo stays as a backstop.
13. **Builder predicate:** the chapter books owed = `CHAP_LATER` returns (name every writer); #90's rebuild predicate = signature mismatch or missing record.

**Budget (Revision 3, owner 2026-10-20; was Revision 1's 11,888 B headroom under `P9_END_MAX`):** the binding rule is the **audio file buffer floor**: ≥ 7,500,000 B on 32 MB units and ≥ 39,000,000 B on 64 MB units, measured in the sim at 50k tracks after every runtime reservation (16-A builds the check). Today it is 15,079,816 / 48,634,248 B. `_end` ≤ 1,500,000 B and the firmware file ≤ ~1,250,000 B are backstops. Every sub-phase still reports its text/bss/reservation deltas and keeps statics small (lean rules); new runtime reservations (the #92 slots, the loader arena) are listed in `hardware.md` and appear in the floor trace. The 4 KiB `inquiry_static` step no longer binds. *Revision 1's caps (16-1 ≤ 3 KB, 16-2 ≤ 3 KB, 16-3 ≤ 2 KB, 16-4 ≤ 3 KB) stay as targets, not gates.*

## 7. Acceptance
- **Host:** `tests/maint`, `tests/bootstamp`, `tests/safefile` (with the fatcrash model), artfmt v2 round trip and v1 migration.
- **Sim scripts:** `ipodui-p16-paused-update` (pause, age the pause, go idle: the update publishes, the queue resumes paused at the same position, PLAY plays it), `-paused-lit` (lit: nothing), `-chapters-paused`, `-chapters-stopped` (L2), `-paused-restart-state` (H2: resume record, statistics, DAC standby unchanged, no pop), `-boot-clean` (no `db scan` probe), `-boot-unclean`, `-boot-afterusb`, `-boot-tenth`, `-usb-moment`, `-drift-paused-variety`, `-panel-rotate` (a restart and a 3 h tick re-pick both sets with no decode, books included), `-pool-topup-window` (top-up only in a window), `-artist-range` (one read per open).
- **Device (P10 list):** a long pause after copying new music → the new album appears without PLAY; resume latency (Q2); clean reboot without the 18 s (`db scan` absent, `lib window`); unclean end walks; a full day with no duplicate entries (host scan); USB disconnect moment; drift variety while paused for 10 minutes.

## 8. Risks
- **Playback glitches** on the buffer release/reload: the shrink path is proven for paused queues (P6), but a window right at a track boundary or during a crossfade/fade-out (P15 fades) needs its own script; never open the window during a fade.
- **Crash consistency:** the stamp is the one new "trust" decision; a stale stamp hides host edits. Mitigation: spoil-first at boot and at USB, forced walks (Q3), and Rebuild Cache.
- **USB interplay:** the spoil must not write at USB screen entry (memory rule); a window open at connect is cut by quiesce.
- **Memory on 32 MB:** the commit and reload at 50k may not fit the freed buffer: then the update keeps waiting for a stop, logged (no worse than today).
- **Format change (#92):** a pak v2 build while an old v1 is resident: readers reload by `panel_seq`; migration rebuilds once.

## Revisions
| Rev | Date | Change |
|---|---|---|
| draft | 2026-10-20 | First draft for the owner pre-flight and the brief review |
| 1 | 2026-10-20 | Brief review (2H/5M/3L), all applied inline; table below |
| 2 | 2026-10-20 | #100 panel rotation folded into 16-4 (pool, set index, rotation at restart and every 3 h, books' random share); Q8–Q10; disk budget |
| 3a | 2026-10-20 | Brief review of rev 3 (3H/4M/2L), all applied inline; table below |
| 3 | 2026-10-20 | Memory audit and re-audit, owner rulings: audio-buffer floor rule replaces the 11,888 B caps (§6); 16-A floor gate (#105) first; 16-B burst directory reads (#107) and 16-C late pool (#108) before 16-1; 16-6 loader spike (#106); 16-7 queue by index (#109); 16-8 RAM library fit (#110); Q4 a few seconds; snapshot batch in the boot window, post-USB and "Updating Language…"; links #100, #104 |

### Revision 1: review findings → resolution
| Finding | Resolution | Where |
|---|---|---|
| H1 stamp wrote at USB connect; a clean shutdown alone can't see edits made while ProkBox wasn't running | Spoil = remove at boot before the tagcache thread; USB sets a RAM flag only; clean write only with no USB since the last walk and never while quiescing; a directory-entry digest (name, size, mtime, first cluster; raw dir reads; ~0.2–0.5 s, measured in 16-0) must match; 10th-boot walk kept | §3.2, §4.2, Q3 |
| H2 shrink semantics misdescribed | Partial shrink only (core_alloc_ex with front/back hints; never core_alloc_maximum/allocate_tempbuf paused); the refill happens at the shrink; buffer smaller until audio_reset_buffer; give_up loses the queue and is never requested; no window during video, fades or crossfades; restart-state script; PLAY during a commit | §4.1, Q2, §7 |
| M1 playlist.c :4067 writes in user folders; :3704 meaning | :4067 remove-then-rename with the temp as residue, recovery for /Playlists only; :3704 keeps upstream meaning | §4.3 |
| M2 dbgen.c :671 is an A/B pair | Remove only the stale manifest; the other is the recovery | §4.3 |
| M3 boot recovery order unnamed | Control file before playlist_resume(); journal and generation before the tagcache thread; ab_store and otg before their loads | §3.3 |
| M4 budget headroom | 11,888 B; per-sub-phase caps; the next trim named before 16-4 | §6 |
| M5 #90/#92 migrations | Signatures filled lazily in windows under a cap; pak v2 rebuilt from the art cache in a window, never at boot | §4.4 |
| L1 Q5 on 32 MB vs the USB ring | N=2 only if the 8 MB ring keeps its size, else N=1; slots sized after the ring | Q5, §4.4 |
| L2 chapter books when stopped | Script `-chapters-stopped` | §4.1, §7 |
| L3 Q6 deletion | Count only; delete only after the owner's yes | Q6, §4.5 |

### Revision 3a: review findings → resolution
| Finding | Resolution | Where |
|---|---|---|
| H1 #88's window can breach the 32 MB floor (the shrink checks only need + 256 KB; 7.58 MB headroom vs ~7 MB commit) | The window opens only if the audio buffer after the shrink stays ≥ the floor; otherwise it waits; Q11 (default: no breach); script `-paused-floor` | §4.1, Q11 |
| H2 R5 doesn't unblock #88 alone | Pool (a) only for small tenants (arena, #92 slots, chap_bytes); #88 needs (b) or the partial shrink; (b) time-boxed | §4.0c |
| H3 R1 not read-only safe (filestr_cache, dc_* dirty sectors) | readdir-only bursts, invalidated on any write to that directory, dirty sectors win; a write-during-scan test; device counts | §4.0b |
| M1 R6 vs #85 generations | Keep A/B, shrink by packing; single snapshot needs its own review and an owner question | §4.8 |
| M2 R4 under-specified | Own section: resume by index tied to a generation id with filename fallback; OTG, bookmarks, ab_store; control file as fallback; after 16-3 | §4.7 |
| M3 16-A worst cases | Named (after shrink, chap_bytes, panel transient in windows, arena, NP open; USB ring excluded); a device floor check in P16 | §4.0a |
| M4 buffer labels, Q5 wording | 41.8 MB is the commit-peak free heap; N = 2 = 720 KB total, +360 KB over 8b | §6, Q5 |
| L1 line refs | Re-anchored on p16-brief HEAD (dbgen :690, dbjournal :513/:578/:753, otg :387/:677/:785, ab_store :175/:545, usbscreen :1115, CHAP_LATER :3626; the R4 write site is library.c:3075-3093) | §3.3, §4 |
| L2 header issues | #105–#110 listed | header |
