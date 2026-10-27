# Phase 16 close-out notes (DRAFT, 2026-10-22)

*Draft per-issue notes for the tracker. Nothing is closed. Paste each note as the issue's release comment once its branch is merged and the full gate passes. Rows marked **(pending merge)** are on `p16-artist` or `p16-art` and are written from those branches' commits; drop the marker when they merge. All device checks need the owner's go and a fresh backup (CLAUDE.md rule 5, `docs/process.md` device-reality checklist).*

Milestone summary: Library & maintenance. Updates land while paused, the boot walk is skipped when nothing changed, replaced files are crash-safe, the queue is held by database index, and the post-USB moment is a small glyph instead of a wait. #110 (fit the RAM library) moved to P17 (owner, 2026-10-22). Ledgers: `docs/changes-from-rockbox.md` 2.21, `docs/design/changes-from-7g.md` 2.37, 3.19-3.21. Budget: `docs/hardware.md` (floor bullets, queue, bank and precompute rows).

## Merged on p16

### #105 audio-buffer floor gate
- **Shipped:** `tests/perf/floor_gate.sh` in `validate.sh`, `floor-50k.script` for 32 and 64 MB, named worst cases, `_end` 1.5 MB and `rockbox.ipod` 1.25 MB backstops. Minimums at close: 11,614,800 B (32 MB), 45,169,232 B (64 MB).
- **Device checks owed:** the floor on the developer buffer view (M1 / #111) after a boot, after a commit and restart, and with a full-library queue, on a 32 MB unit if one is available (the test unit is 64 MB).

### #107 burst directory reads (R1)
- **Shipped:** `fat_readdir()` reads up to 16 KiB per command; dirty cache sectors win; invalidated on a write to the directory.
- **Device checks owed:** ATA commands per scan and per-command latency (the 16-0 scan trace lines); `db scan ms=` against the 18 s and the 42-140 s rescans seen on 2026-10-20; no stale or duplicate entry after copying files during a scan.

### #88 updates land in the paused window
- **Shipped:** `Q_AUDIO_GIVE` and the silent restart; commit, reload and RAM-cache gates take room in the window; chapters kept across the restart; no stats or iAP events during it.
- **Device checks owed:** a long pause after copying new music: the album appears without PLAY; resume latency (Q2) and `m0 tap2pcm` after a window (trashed buffer, flash); no click or glitch on the restart; battery cost of a window.

### #87 boot window and #89 no-change boot
- **Shipped:** boot stamp over the directory digest, a completed walk vouches for the library, `BOOT_HOLD_MS` 3000 hold of the boot screen, `tagcache_boot_window()`.
- **Device checks owed:** clean reboot without the 18 s (`db scan` absent, `lib window` log); an unclean end walks; a changed folder is noticed; the hold length against the 5-10 s Apple spends; a full day with no duplicate entries (host scan).

### #91 safe replaces and crash-ordered FAT writes
- **Shipped:** safe replace for the overwrite renames, ordered directory entries, rename intent record resolved at boot, FLUSH CACHE write barriers (`ata_flush_stats`), alias-aware remove and rmdir.
- **Device checks owed:** barrier cost on the unit's drive (`ata_flush_stats` in the log: n, max, total; fail and nosup must be 0 on flash); a real power cut during a playlist save and during a library commit, then boot; no stray `_prev`, `.tmp` or intent leftovers.

### #106 module loader (off by default)
- **Shipped:** loader, `modules.pak`, storage-thread jobs, cold Settings and Loader & storage as tenants. Default build identical; on only with `-DIPODUI_WITH_MODULES`.
- **Device checks owed (all only when the loader is built in):** load latency over 50 entries while playing with 0 underruns (flash and HDD), the stand-in-to-real swap time, the storage thread's `stk=%`, the snapshot batch time, the Fletcher cost, the error page with the pack removed, USB connect and exit with a module open.

### #109 queue by database index
- **Shipped:** the queue holds database indices (about 405 KB at 50k on the device), short control file, rebase in place, deferred commits load their RAM after the commit.
- **Device checks owed:** `m0 tap2pcm why=play` on a 2,000-row Shuffle and on play-all Songs, before and after; resume after a renumbering commit (Rebuild Cache, reboot); devstats disk attribution per track load (no added wake); audio-thread stack high-water; load latency over 50 entries while playing; stop-to-publish time of a deferred commit (about 1.45 s in the sim).

### #84 post-USB busy glyph
- **Shipped:** 8-dot header glyph while the post-USB scan and read-only housekeeping run; sidecar (`._`) count and FSInfo check logged.
- **Device checks owed:** the disconnect moment on the 5.5G with a macOS-written volume (sidecars present): glyph appears, library browsable, glyph ends with the scan; compare the feel with a 7G capture if one exists.

### Battery colours and the plug glyph (owner request, 2026-10-22)
- **Shipped:** fill green above 35 %, amber at 35 % or less, red at 20 % or less with a 2 px sliver; dark plug on the light header when on power and not charging (7G ledger 3.19).
- **Device checks owed:** the three colours on the real panel at the real thresholds (amber and red are hard to reach; check the colours in a dev override or on a discharge), the plug on a charger that is full, the glyph in sunlight.

## Pending merge

### #47 one range read per artist (merged)
- **Shipped on the branch:** the artist's album range read once from the precompute (+80.6 KB file at 50k, 31 of 32 section slots), shared Unicode case folding for grouping, tag-file order, dedupe, Search and art keys (Latin, Extended-B, Greek, Cyrillic, Vietnamese). Case-insensitive, accent-sensitive; the 7G's own behaviour is unconfirmed (7G ledger 2.37).
- **Merge notes:** an existing database re-sorts at its next commit; the precompute header word triggers one background rewrite; art keys for non-ASCII capitals change (books are read under the old key until the next checkpoint). Hardware.md section-slot note: the next added section needs a larger header.
- **Device checks owed:** artist-open time at the real library size (one range read, no scan fallback: look for `lib precompute artist albums unassigned=` in the log, it must be absent); a real library with accented and capitalised names groups as intended; art still shows for books after the key change.

### #92 several covers per set (merged)
- **Shipped on the branch:** `panels.pak` regrouped (stays version 1 on disk), a bank of 2 more covers per set on 64 MB (720,976 B), N = 1 on 32 MB; regroup only in a maintenance window.
- **Device checks owed:** drift variety while paused for 10 minutes; no disk wake caused by a bank swap; the bank's RAM against the floor (the floor receipt is owed at the merge gate, see hardware.md); the regroup is silent (no audio effect) in the paused and stopped windows.

### #90 art source signatures (merged)
- **Shipped on the branch:** an album whose tags changed keeps its art when the source signature matches (CRC of the first and last 8 KB for new records); older records still compare by the first 16 KB; no-change pass at 50k about 500 ms.
- **Known limit:** no source mtime, so a same-size edit in the unread middle of a large source is picked up only by Rebuild Cache.
- **Device checks owed:** retag an album over USB: its cover is kept and no decode runs (`art sig keep`); replace a cover: rebuilt; the no-change pass time on the real library.

### #100 panel-set rotation (merged)
- **Shipped on the branch:** `panelpool.pak` / `panelpool.idx`, the shown set picked with no I/O (pinned played covers first, then random picks), rotated at restart and on a 3 h tick.
- **Device checks owed:** the shown set after a restart differs; a played album gets pinned; the pool top-up happens only in a window; disk use of the pool.

## Moved

### #110 fit the RAM library: moved to P17
- Owner, 2026-10-22: not urgent. The 32 MB floor issue it was meant for was the deferred-commit RAM load, fixed on #109. Keep #85's A/B generations when it is picked up. Update the issue's milestone to P17.

## Before the close gate
1. Merge `p16-artist` and `p16-art` into `p16`; drop the "(pending merge)" markers in `docs/changes-from-rockbox.md`, `docs/design/changes-from-7g.md`, `docs/hardware.md` and this file.
2. Re-run `scripts/upstream_changes.sh p16`; reconcile the 2.21 table.
3. One full `scripts/validate.sh` (`GATE PASSED`), then the `budget-reviewer` agent; fill the owed floor, `rockbox.ipod` and `_end` figures in `docs/hardware.md`.
4. Owner round, then release each issue with `scripts/forgejo.py release N "summary" --close`.
