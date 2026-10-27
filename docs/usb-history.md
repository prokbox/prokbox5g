# USB mass storage: history, acceptance gate and next steps (#81)

The engineering record for USB on the owner's flash-modded 5.5G (PP5022, 64 MB, 1 TB flash, USB-C data mod, FAT32). It merges the 2026-10-16 handoff, the 2026-10-17 status note and the 2026-10-17 candidates note. Findings live in their regular homes; this page keeps the order of events, the commits and the open plan.

Where the facts went:
- Controller behaviour and quirks: [`pp5022-reference/usb.md`](pp5022-reference/usb.md) §6, [`pp5022-reference/quirks.md`](pp5022-reference/quirks.md).
- Bus masters, serialization rules, E2: [`data-paths.md`](data-paths.md) R10, D12-D14.
- Experiment results: [`hw-experiments.md`](hw-experiments.md) §7.
- FAT and cluster arithmetic, Apple's mount scan: [`storage-choices.md`](storage-choices.md) §11, [`fat32-migration-plan.md`](fat32-migration-plan.md).
- Code changes in upstream files: [`changes-from-rockbox.md`](changes-from-rockbox.md) §2.9.
- Status: [`ROADMAP.md`](ROADMAP.md) "In flight".

## Scope and evidence boundary

Everything here is from one unit: USB-C data mod, macOS Sequoia or later. It is evidence for the PP5022 ARC controller, this board and this conversion together. It says nothing yet about the stock 30-pin path, another USB-C conversion, a hard-drive unit, Windows or Linux. A clean bulk-IN dTD with repeated host resets could still be a USB-C board, cable or signal problem rather than a driver defect.

| Path | Evidence | Needed before calling USB stable |
|---|---|---|
| USB-C mod (this unit) | macOS mount and large-copy traces | Ten clean attach, mount, read, eject cycles |
| Stock 30-pin connector | None | Same matrix on the second unit |
| Other hosts | macOS only | One Windows and one Linux mount, copy, eject on each path |

## Acceptance gate

Two equal requirements, on the same existing 1 TB FAT32 volume:
1. **Reliable first mount:** ten consecutive attach, mount, representative read, clean eject cycles with no failed first INQUIRY, host reset, disconnect or manual recovery.
2. **Fast native mount:** Finder / Disk Arbitration's own automatic mount completes in at most 15 s, median and worst of the ten. A private `mount_msdos` helper is diagnostic only and never counts. Record enumeration, first command, partition appearance, native mount attempt and completion separately.

Fifteen seconds is not a promise of instant mounting. The host reads about 250 MB of FAT; at the 21-22 MiB/s flash ceiling that alone is near 12 s. A timeout, retry or helper backoff is not part of that bound.

What macOS needs from the device is only what any removable disk provides: stable descriptors and a Bulk-Only interface that recovers from reset; first TEST UNIT READY and INQUIRY replies that retire; correct 512-byte logical sectors, capacity, MBR and FAT32 boot sectors; clean dirty and FSInfo state after eject; sector-aligned small FAT reads fast enough; no disconnect or reset loop during the check. Use Disk Arbitration's appear, mount and disappear events as timestamps. Do not alter macOS settings as a product fix.

## Chronology

### 2026-10-10 to 10-15: first sessions
- Mount scan measured at ~5.4 MB/s in 12 KB reads; 1 MB reads ~7.5 MB/s; read-ahead in 2 x 64 KB ~18.5 MB/s while busy (#63).
- 2026-10-15: prefetch wait cut to HZ/5 so the USB thread cannot sit long enough to trigger a host reset ([`storage-choices.md`](storage-choices.md) §7).
- Branch lineage: `next4` -> `fsinfo-fast` (FSInfo and dirty logging, reset logging) -> `usbfix` (ARC driver fixes `f4db3af867`, SCSI/BOT fixes `857404d2b3`, LCD by DMA engine 2 `c61c9dc639`/`b2e7e13c59`, flush fix `ea36e78e09`).

### 2026-10-16: two faults
- **Fault A, mid-transfer stall (every build, incl. `26e13c10fd`).** At each host reset during a READ: a bulk IN dTD primed and ACTIVE (`e20 t80`, overlay `ov80`), ~4 KB of a 16 KB transfer sent (`ol23`/`ol24` = 23-24 x 512 B left), then nothing for ~304 ms (`i304`), then the Mac resets. All resets genuine (`rstreal`), `uei=0`. Idle resets every ~11.5 s, unexplained. Fields: `struct usb_drv_rst_event` in `firmware/export/usb_drv.h`.
- **Fault B, dead at enumeration (`ea36e78e09`, 2 of 5 sessions).** One session saw no command at all. One saw `inq=1` only: the 36-byte INQUIRY reply sat primed and ACTIVE for 108 s and the host never collected it; macOS stopped at `IOSCSILogicalUnitNub`. Apple's disk mode mounted the same disk fine right after, so disk, FAT and Mac were not the cause.
- Suspect list for B: TXFILLTUNING write (`f4db3af867`; read-back 0), LCD pushes by DMA engine 2, other ARC changes (deferred "lost completion", bounded prime, flush retry). TXFILLTUNING was made untouched by default.
- 15:05: rolled back to `26e13c10fd` (firmware cmp-identical to backup).
- **15:35, handover root cause.** `usb hand` log: the storage handover took 54.2 s (`req=-120 ack=54200`, 7 acks); the host's first CBW was held, macOS timed out every ~11.5 s and reset 4-5 times. Each slow handover coincided with a `db scan ms=~60000` ending. Installed `f4efa21c2a`. A 1 GB read dropped off the bus after ~900 MB.
- **16:14, tagcache fix (`0ded0466a6`).** tagcache acknowledges the handover at once and runs its changelog export after disconnect. macOS mounted at 16:14 without `Device not configured`; a 128 MiB read of `Audiobooks/Mythos.m4b` took 16.01 s (8.0 MiB/s), no reset. One clean sample only; fault A not cleared.
- **16:29, boost A/B unusable.** Baseline: the whole 841 MiB `Mythos.m4b` read without disconnecting at 11.2 MiB/s. With `usb_boost` ProkBox enumerated at high speed but repeatedly failed to expose storage (`Device not configured`); 68 genuine resets, same active-dTD signature (`session 38o`). Marker removed in Apple disk mode.
- **16:49, E2-off trial withdrawn.** A short `lcd_e2off` trial failed at the first macOS mount, but E2 is the panel's only physical path and the trial is not evidence against it. Kept enabled ([`data-paths.md`](data-paths.md) D14). Storage baseline for the flash adapter stays 21-22 MiB/s.
- **17:08, host automounter race.** The helper (`scripts/macos/ipod-automount.c`, polls `diskutil list external physical` every 3 s, then `mount_msdos -o noowners`) sleeps another 27 s after one failed mount, so a manual mount can win for about 30 s. A helper defect, not a verdict on ProkBox. A 64 MiB manual read ran at 9.3 MiB/s; a 256 MiB read disconnected at 166 MiB.
- **17:34, stream-disable no-op.** Diagnostic `e576031066` showed the active dTD starts at page offset `0xb60`, so the stall is not a first-4-KiB page-pointer fault. `usb_sdis` requested USBMODE.SDIS; the log read `sdis=1/2` (USBMODE `0x02`, not `0x12`). PP5022 ignores the bit; the A/B changed nothing. 167 resets in 351 s, same `ov80`, `b=b60`, ~304 ms (`session 38s`).
- **18:53, `fa571b6895`.** Makes the synchronous read-ahead the PP5022 default and turns the 256 KiB prefetch worker off, so it cannot overlap ATA DMA with bulk-IN DMA. Controls then ran 15.7-17.4 MiB/s, including 384 MiB past the old 166 MiB failure point. The no-marker production build still failed at 345 MiB on `The Complete Fiction of H.P. Lovecraft.m4b` after 19.74 s (17.5 MiB/s). One "automatic" mount was in fact manual; one disappearance was a physical unplug.
- **`41103a9158`.** Corrects the description above: `fa571b6895` only stops the worker, ordinary multi-64 KiB READs still start the next ATA DMA while the current IN is primed. This build defers every next ATA read until the preceding bulk-IN completes and drops the final read-ahead fill. The session switch line reads `overlap=0` (normal builds `overlap=1`).

### 2026-10-16 to 10-17: ARC driver and INQUIRY experiments
- `2cefd681ce` 4 KiB dTDs (`USB_ARC_SMALL_DTD`); `b9d19f7166` prime retry; `cc16c920ec` queue-head overlay reset at configuration; `6de6b2986a` ARC re-init at Video USB-C attach (installed 2026-10-16). Results in [`hw-experiments.md`](hw-experiments.md) §7. The 4 KiB dTDs, retry and overlay reset did not remove the first-command timeout.
- Attach re-init run: macOS configured at high speed but timed out the first Bulk-IN INQUIRY before any READ. Next control: a page-aligned static SDRAM INQUIRY reply through the uncached PP alias (`4379f2597d` Developer row, `2465b3d092` made it the default and added mount-read tracing). Later sessions record `inq2data=1`: the first INQUIRY reply completes.
- The failing session also showed a handover defect: `db scan ms=154000` plus hundreds of thousands of ATA DMA reads while the host waited and no READ had come. Fixed by `248ee66c81` (tagcache quiesce gate set before the GUI acknowledges) and `ec96be8910` (safe recovery), with `5a1d396d87` (cancel and resume commits for USB handover). `artcache` also got a stop gate armed before the acknowledgement (`7d5e7fc086`); an earlier closed session had a 2.8 s handover whose last acker was `artcache`, inside a panel decode.
- `23d1223bba`, serial read fix: `41103a9158`'s serial path switched to the other 64 KiB half without loading it, so a multi-chunk read alternated correct data with stale RAM from another file. `read_pending` now loads the next half after the preceding IN completes and before it is filtered and sent. Mac-side content checks made with the affected build are invalid; writes and device playback stayed good evidence that the media was intact. The serial build is the correctness control.
- First closed session on `23d1223bba`: real music writes (`wr=19173`, `wrfail=0`), `pf ring=0`, `inq2data=1`. Native FSKit mount still timed out; the first-18-s snapshot reached only ~46 MiB. Serial reads alone cannot finish the FAT pass in time.
- Developer row `USB ARC attach init: legacy|normal` (volatile, next cable session). `legacy` keeps the second `usb_drv_init()` at attach; `normal` removes it, as upstream `5af0a50031` did for dock compatibility. Session logs now report first and last speed, configuration count and speed changes.
- Research note (source, not device): Apple's published FSKit FAT code sweeps the FAT in 12 KiB requests at every mount; see [`storage-choices.md`](storage-choices.md) §11.

### 2026-10-17 evening: cache work and Music maintenance
- `7de657f382`, `2da4d5b355` Music hold action and progress; `3ae3b3dcc7` non-destructive full update (`Q_UPDATE`, working database kept until commit; the first, destructive rebuild committed 1,449 entries but failed its RAM-library reload); `0ba0dff05c` Build Artwork; `4b2228b2dd` wait-reason reporting; `fd4011efa7` maintenance stops active or paused playback first; `accf0d5a4a` first progress fix (incomplete: hints uncounted, counter reset under one label, final work hidden); `7868773a30` phased progress and scan/art scheduling (`tagcache_scan_active()`, retained events in `io_go()`). See [`design/spec/music-lists.md`](design/spec/music-lists.md).
- Installed `fd4011efa7`, SHA-256 `4b9b857bcdc2ca6209f0c35e8e0d9fab60775064713db55d172ca37f727a6496`, backup `session log, backup 20261017 `.
- Installed successor `7868773a30-261018` (verified 2026-10-17 22:17), SHA-256 `b9f73d3972e03502c8d33a28638e632565c6fa40fd77dc19b437b6a9bbca6b9e`, backup `session log, backup 20261017/.rockbox/`. Only `.rockbox/rockbox.ipod` replaced, by atomic rename; prior firmware and log compared identical; new image compared byte for byte. It had not rebooted into it when written: no device success or performance claim. No cache, database or music deleted. The disk number changes between sessions (it was `disk6s2` at `/private/tmp/ipod_mount`); resolve it each time.
- Evidence kept: earlier good session 1,449 tracks, scan 153.5 s including commit 16.48 s, `lib window empty_ms=16630`, then `art pass start total=235`. On `fd4011efa7`: RAM library ready after `empty_ms=650`, art began at uptime 5 s with 235 albums, 27 images decoded before the last log line, a second art pass at 313 s, scan 361.41 s with a 100 ms commit. That shows scan/art overlap and a restarted pass; the restart cause is unproven. `f3 art n=1 dms=26403/30937` is ~26/31 s CPU decode for single images; for `n>1` the times are aggregate deltas.
- Art is keyed per album by one representative track; a fresh album usually decodes once for the thumbnail and again for Now Playing, panels may add a third. Decode-once with several outputs is not done. `0/235` did not prove a dead decoder: `run_early()` does real work before `done` increments. The owner's library `100% -> 0% -> 100%` is not explained by retained logs. Paused playback is excluded by `playing()` but can keep buffers and defer book work via `audio_status()`, so exclusivity claims were too strong. Cache persistence and a full pass on the owner's library are still device acceptance work (#85).
- Validation of that successor: `build/fw-ui`, `build/fw` and the simulator compiled; host art/UI tests and `tests/usb/run.sh` passed (BOT stress, ARC reset, temporary-image macOS translation/mount/fsck, which is not a physical automatic mount); sim scripts `ipodui-art-maintenance-progress`, `ipodui-art-maintenance-screen`, `ipodui-p4b1-art-usb`, `ipodui-p4b1-art-dirty` passed.

### 2026-10-17 to 10-18: migration planning
- Owner chose FAT32, not exFAT; 32 KiB clusters are the default candidate, 64 KiB an alternative. Plan: [`fat32-migration-plan.md`](fat32-migration-plan.md). A read-only look at the Linux backup host found ~950 GiB free and working rsync and dosfstools.
- Owner correction, 2026-10-18: about 78 GB of files; no full-disk or data-partition image. Back up and hash-verify the file tree plus small recovery captures (partition map, Apple firmware and bootloader, geometry). Rollback reformats to the original geometry and restores files; the original sector allocation is not preserved. Nothing was backed up, formatted or written in planning.

## Rules that came out of it
- Preserve **load-before-send** in every serial or batched read experiment ([`data-paths.md`](data-paths.md) D13).
- Do not use `usb_boost`, `usb_sdis`, `lcd_e2off` or TXFILLTUNING markers.
- Keep E2 enabled.
- Native Finder / Disk Arbitration mounting is the goal; manual `mount_msdos` is diagnostic.
- Do not eject or unmount without the owner's permission. Never wipe the art cache to install firmware.
- Never fsck-repair, reformat, touch the MBR, or install a bootloader in this work. A device failure must be recoverable through Apple disk mode.
- Change one thing per device build.

## Device round procedure
The owner must say go (see the `deploy-ipod` skill). Per install:
1. Copy the logs (`.rockbox/*.log*`, `*.csv`) to `the backup folder (one subfolder per session)`.
2. `rsync -a` the system folder to the backup folder.
3. `unzip -o rockbox.zip` onto the mount, `cmp` every file in the zip, `sync`, `diskutil eject`. Delete `._*` litter in the system folder.

Read results with `python3 scripts/usb_sessions.py <logdir>/ipodui.log*` (one row per session, verdict, raw reset records) and `--mac 30` (the Mac's mount attempts). The automount helper mounts at `/private/tmp/ipod_mount` (launchd `dev.prokopto.ipod-automount`; if it hangs the owner runs `sudo launchctl kickstart -k system/dev.prokopto.ipod-automount`). If our firmware won't mount, the owner reboots (MENU+SELECT) and holds MENU for Apple disk mode; that is how `26e13c10fd` was restored.

Developer markers (empty files in the system folder) that are gone from the code: `usb_sdis`, `usb_txf0`/`usb_txf8`/`usb_txf20`, `usb_pfoff`, `usb_pf64`, `usb_boost`, `lcd_e2off` (its reader removed 2026-10-26). All retired as fixes; see [`hw-experiments.md`](hw-experiments.md) §7.

The session exit log has `usb mnt cfg2tur= cfg2inq= inq2data=`. `inq2data=0` after an observed INQUIRY means a reply that was primed but never retired; a nonzero value followed by a late `cfg2rd` points to native mount and FAT I/O. Tests: `tests/usb/run.sh` (bot, pf, reset, arc_fix, xlate); `scripts/validate.sh` for the full gate.

## Next steps, in order
1. **Native failure capture.** On the next owner-controlled attach, timestamp enumeration, partition appearance, probe, check, load and mount; capture the Disk Arbitration / FSKit error domain and code plus the firmware counters, without the manual helper. Extend `usb_sessions.py` rows with first-command latency, first-INQUIRY latency, enumeration-to-disk, native mount latency, resets, link speed, automatic vs manual. Match the OS module to the published Apple source.
2. **Ten-cycle run** of the current build against the acceptance gate; record the `legacy|normal` attach-init choice and the INQUIRY buffer choice for each cycle. A pass supports stale controller state as the cause; a failure stops us polishing the helper around a dead endpoint.
3. **Image rehearsal** of the cluster change on a disposable image ([`fat32-migration-plan.md`](fat32-migration-plan.md)).
4. **Serialized demand read batching** (highest-value firmware candidate). Current `USB_STORAGE_NO_READ_OVERLAP` compiles out `usbstor_ra_fill` and its call, so with the worker off a sequential read has no filled cache to hit and pays storage-command latency per small host request. A/B: on a demand miss read a larger contiguous window synchronously (64, then 128, 256 KiB), finish ATA before any Bulk-IN, serve later small requests from RAM. Requirements: a separate unmodified raw-sector cache or explicit ownership states (the sector translator mutates outgoing buffers in place; never translate twice); never reuse memory still owned by USB DMA; keep load-before-send; invalidate on every write and mutating command, reset, disconnect and new session; key by LUN and geometry; bound reads at the device end; align and maintain the cache for PP DMA; drop read-ahead for scattered I/O. Acceptance: multi-chunk reads byte-identical (fragmented files, mixed writes), reset and stale-completion tests pass, uncached host read matches a known source. Measure native auto-mount time, not helper latency, and claim no speedup before a same-volume device A/B. Batching saves per-command overhead; it cannot remove USB transfer time.
5. **Verify host-visible geometry and clean state** (read-only): translated primary and backup BPB, FSInfo signatures and bounds, FAT[1], MBR extent and capacity; compare with Apple disk mode. Never mark an unverified filesystem clean.
6. **Separate link speed and command latency from throughput.** Some early sessions were `hs=0`. Chase PHY/ARC init or another cable or hub only if an attach shows that signature.
7. **Audit flush failures.** `SYNCHRONIZE CACHE` reports a `usbstor_flush_all()` failure, but `START STOP UNIT` discards it and sends GOOD, which could claim a good eject after a failed flush. Test with error injection on the host harness, log the status, check short and failed reads against BOT residue and sense.
8. **Return to ATA/USB overlap** only after first response is reliable: use the no-overlap build as the control for a >= 1 GiB read, restore overlap step by step, tune toward 21-22 MiB/s.
9. **Classify board versus firmware** once a candidate is stable: second cable or hub, Linux and Windows, then the stock 30-pin unit.
10. **Volume layout** as a separate, reversible migration (step 3).

Do not repeat: the alternating 64 KiB wrong-file data in `ipod_disk_brief.md` was the stale-buffer bug, not a macOS or flash-card fault; better FSInfo and clean flags are not proven to skip the FATManager sweep; the 4 KiB dTD, prime retry and overlay experiments are not undifferentiated fixes.

## Other open threads
- #84: busy screen after USB disconnect while the library rescans; safe dirty-flag clear after a clean eject; macOS and Windows litter removal (checks only, never auto-repair).
- E3 lab (branch `e3-lab`): its 288 B static IRAM must borrow codec IRAM only while it runs, before it ships.
- Upstream `e61aa0cd37` (usb_storage buffer allocated at the storage handover) is not in our tree.
- The video player's E2 path was refactored in `c61c9dc639` and is not device-tested.
- Owner question open: USB-C power meter.
- Stock 30-pin and other-OS runs are deferred, not passed.
