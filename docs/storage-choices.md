# Storage choices: flash and hard drive in one build

**Goal (owner, 2026-10-15):** "a single build for both flash and harddrive with identical behaviors, so that it's easier on us, and means users don't have to think about it — the flash mod just becomes a raw benefit, with faster reads, and lower battery usage." Tracker #79.

**Rule:** storage may change *tuning* (timeouts, read sizes, waits, boosts), chosen at runtime from what the drive reports. It never changes what the user sees, except where physics makes a difference unavoidable (§9). Every choice made because the test unit is flash goes in this file, in the same commit as the code.

Today the split is a configure option, `--storage=flash|hdd`. `hdd` defines `IPOD_STORAGE_HDD`, which gives upstream's `HAVE_DISK_STORAGE`, `HAVE_ATA_POWER_OFF`, `HAVE_ATA_SMART` and UDMA2. Flash keeps `HAVE_FLASH_STORAGE` and UDMA4 as the ceiling (`tools/configure:4983-4987`, `firmware/export/config/ipodvideo.h:128-136, 228-237, 331-338`; `docs/hardware.md`). The audit of 2026-10-15 (read-only, HEAD `4f1ee2812b`) is the source of every row below.

Status values: **flag** (decided by the configure split today), **runtime** (decided at runtime already), **agnostic** (same code, but the result depends on the storage), **open** (needs a decision or a measurement).

## 1. Detection

| Item | Today | Plan | Status |
|---|---|---|---|
| Is the drive solid? | `ata_disk_isssd()` reads IDENTIFY word 217 (`firmware/export/ata.h:169`); `ata-common.c:30` measures `spinup_time` | `storage_is_solid()`: true if word 217 == 1, **or** the CFA signature (word 0 = `0x848A`, or word 83 bit 2), **or** words 76 and 93 both zero **and** a measured wake under ~100 ms. Unsure means rotating (safe: tuning conservative, behaviour the same) | **Measured (S37, owner's iFlash adapter):** `w0=045a w76=0000 w83=7404 w93=0000 w163=0012 w168=0000 w217=0000 ssd=1 spinup_ms=0`. So word 217 is "not reported" (as feared), the CFA signature in word 0 is absent, but **word 83 bit 2 (CFA feature set) is set** and `ata_disk_isssd()` says solid (word 163). Detection for this adapter: word 83 bit 2 or `ssd`; the timed-wake fallback is not needed here. Still open: what a stock hard drive reports (#79) |
| Where it is shown | `usbscreen.c:636` prints word 217 (developer) | the boot log line, the developer list | open |

## 2. Driver

| Choice | Why | Flash | Hard drive | Single-build plan | Status |
|---|---|---|---|---|---|
| Idle hold before sleep (`ata.c:100-107`) | flash wakes are cheap | 1 s | 5 s | set at init from `storage_is_solid()`; it drives every "while the disk is awake" rule (§5) | flag |
| ATA power-off, SMART (`HAVE_ATA_POWER_OFF`, `HAVE_ATA_SMART`; `ata.c`, `panic.c:140`, `powermgmt.c:242`, `usb_storage.c`) | upstream hdd features | absent | present | compile both in; power-off is a pin action, harmless on flash; SMART only when IDENTIFY word 82 bit 0 says supported | flag |
| UDMA ceiling (`ATA_MAX_UDMA`) | the adapter may offer more | 4 (falls back to 2 when words 76/93 are 0, `ata.c:1080`) | 2 (`ata-target.h:75`) | 4 for both: already safe at runtime; above mode 2 boosts per transfer (`ata-pp5020.c:153`) | flag; S36c: UDMA4 gives no speed on this adapter (`data-paths.md` D3) |
| Forced UDMA test (`HAVE_ATA_UDMA_FORCE`, `udmatest.c`, `devmode.c:344`) | developer experiment | offered | absent | offer only when solid | flag |
| Sector multiplier under USB (`ata-common.c:331`) | 2048-byte virtual sectors made read-modify-write | the drive's own | raised | the drive's when solid, raised otherwise | flag |
| Sleep before the FireWire reboot (`firmware/usb.c:132`) | park heads | no | yes | always | flag |
| Write-order barriers: FLUSH CACHE at each file-layer ordering point (`fat.c` `FAT_WRITE_BARRIER`, `file.c`; #91 H2, `data-paths.md` R11) | the drive's volatile write cache can reorder writes at a power cut | same barriers; a few ms each [hypothesis] | same barriers; ~10-20 ms each [hypothesis] | identical: not a flash choice. The conservative choice (flush, not trust the order) holds for both; only the cost differs. Measured by the `usb ata … flush` log line | done (sim); device cost open |

### 2.1 Drive power-off when idle (#114, 2026-10-22)

| Choice | Why | Flash (solid) | Hard drive (rotating) | Single-build plan | Status |
|---|---|---|---|---|---|
| Power cut after idle (`ata.c` `spw_off_due()`, `stor_pwr.h`) | the adapter's draw in standby between refills (owner rundown 2026-10-22: ~105 mA average dark) | after the policy's hold from the last access of any thread (the driver's last activity, so the art read-ahead, the stats drain and the idle notices' writes after a fill extend it): `STOR_PWR_HOLD_DARK_S` 3 s (owner, 2026-10-23; was 10 s), `STOR_PWR_HOLD_LIT_S` 60 s; an off period under `STOR_PWR_MIN_OFF_S` 30 s doubles the hold, up to `STOR_PWR_HOLD_MAX_S` 600 s, cleared by one of `STOR_PWR_RELAX_S` 120 s | upstream's: standby after the spin-down time (5 s), power off 2 s later; a spin-up costs 1–2 s and much more current, so no shorter cut | **runtime already**: `spw_solid` from `ata_disk_isssd()` at init picks the policy. Same behaviour (the drive is off when idle, wakes on access); only the timing differs | done (sim); device A/B open |
| Statistics drain while the drive is warm (`tagcache.c` `ui_drain_wanted()`, `storage_power_warm()`; #114, 2026-10-23) | queued play counts and resume points are written when the drive is up anyway, never with a power-up of their own per track | warm = on, or in standby with power (inside the 3 s hold): a read there costs no power-up | warm = on only: a read in standby would spin it up, so a disk keeps the second after a transfer (`storage_disk_is_active()`) | **runtime already** (`spw_solid` in `ata_power_warm()`). Same writes, same journal; only when they run differs. Both still force a drain at a half-full queue | done (sim); device check: `disk_by=tagcache` gone from `power.csv` |
| Refill margin (`playback.c`) | the refill must not run dry while the drive powers up | 1 s + 2 s budget, × speed (≥ 2×): 6 s | spin-up + 5 s + `buffer_margin` (unchanged) | the hdd formula already uses the measured spin-up; the flash one adds the budget | done |
| Sleep entry (18-2, `storage_power_down(..., STOR_PWR_KEEP_LOCK)`) | the sleep's floor | off | off | same | done |
| Developer switch "Drive power" | the A/B | off when idle / standby only | same | same | done |
| Ready wait after a power-on (`ata.c` `ata_power_on()`, `STOR_PWR_READY_S`; #114 H1) | a dead adapter must fail the power-up in seconds, not hang each access | BSY clear within 3 s, else the attempt fails (~7 s for a failed power-up with its retry) | upstream's 30 s (a spin-up) | **runtime already** (`spw_solid`); only the timeout differs. What the user sees on a failure is the same: the access fails, the next one tries again, and after 3 failures in a row the drive stays powered for the session | done (host test); device check: `stor pwr on ms=` |


| Choice | Why | Flash | Hard drive | Single-build plan | Status |
|---|---|---|---|---|---|
| Prefill before sound (`pcmbuf.c:86-93`) | **user-visible**: tap to sound | 0.25 s | 1 s | from `storage_spinup_time()` at runtime: the same look (sound after a wait), the wait as short as the drive allows | flag |
| Buffer watermark (`playback.c:298, 1169-1199`, `audio_thread.c:88`, `audio.h:69`) | a skip is user-visible | fixed 1 s | spin-up + 5 s + anti-skip margin | always the formula; with the spin-up measured near 0 it lands near the flash value | flag |
| CPU boost during fills (`buffering.c:662`) | battery | no boost | boost | a runtime test | flag |
| Fill chunk 256 KB (`buffering.c:72`, `FAT_MAX_TRANSFER_BYTES` `ipodvideo-variant.h:144`) | bulk I/O | yes | yes (untested on a disk) | measure on a disk | agnostic |
| `disk_spindown`, `buffer_margin` settings (`settings.h:537`, `settings_list.c:1732-1742`) | upstream hdd settings | absent | present (never surfaced in the shell) | keep both, ignore when solid, never surface | flag |

## 4. Power

| Choice | Why | Flash | Hard drive | Single-build plan | Status |
|---|---|---|---|---|---|
| Disk-safe floor (`powermgmt-ipod-pcf.c:32-37`) | a spin-up can brown out a sagging cell | **3400 mV** (owner rundown 2026-10-22: played and read to 3,362 mV, log flushed there; flash has no spin-up surge). 0 % = 3420 = "about to switch off" | 3500 mV, 0 % = 3520 (same meaning, higher voltage) | a variable set from the solid flag; battery learning already reads it at runtime (`batcal_core.c:62, 289`) | flag (same value today) |
| Save bookmark and settings at critical battery (`misc.c:579, 614`) | **user-visible**: resume after a critical shutdown | always saves | skips | gate on solid; on a disk this stays a real difference (§9) | flag |
| +5 % voltage correction at boot (`powermgmt.c:272`) | the disk's draw | no | yes | a runtime test | flag |
| Power log forced write and flush below disk-safe (`hardware.md`, powerlog) | logs' last rows | written | lost | gate on solid | flag |

## 5. Piggyback policy ("do it while the disk is awake")

The code is the same on both, but the awake window follows the idle hold (§2): 1 s on flash, 5 s on a disk. So flash sees *more* deferrals and placeholders, not fewer. **Plan:** one policy that compares the expected wait (the measured spin-up time) with a UI budget, instead of asking whether the disk is awake. Size: medium.

| Site | Effect | Status |
|---|---|---|
| `preview.c` `load_first()` / `storage_ok()`: the main-menu panel | #73 fixed (Fit-3a, `9634a68f69`): a set change reads the other set's cover **at once on both**, waking storage once if needed; the flash and `--storage=hdd` sims are pixel-identical (0 of 76,800 differ, 7 shots). Cost differs only: one 180 KB read on flash; on a disk a spin-up of ~1–3 s, **and the read runs on the UI thread**, so the UI would stall that long while playing. Follow-up for the single build: move the set-change read off the UI thread (keep showing the slide-in once ready) before a disk unit ships. **8b-4a:** a second drift slot (+180,000 B, `preview.c` `slot_for_set()`) keeps the other set's last cover, so a set switch after the first visit is a handle swap with no read and no wake: the disk unit's UI-thread spin-up stall on a switch is gone except for the first visit to each set after boot (and on a unit without the second slot). Also fixed: `pv.woke_set` is set on every read, so Books (wake) -> Music (awake) -> Books no longer defers for ever while paused | runtime (same behaviour); hdd stall: first visit per set only |
| `nowplaying.c:866`: Now Playing art deferral | **user-visible** | open |
| `preview.c` `np_cover_read` / thumbnail fallback (8b-4a, :1109-1119, :1155): the main-menu Now Playing row's still cover | **user-visible on HDD**: a user highlight may wake storage once per cover and reads 180,000 B (or the 55 px thumbnail) on the UI thread, a spin-up stall on a sleeping disk; background track changes never wake (they show plain or the kept cover until the next natural wake). Flash: ~40 ms, invisible. Fix candidates: #104 predictive spin-up, read on the loader's worker (module-loader.md) | open (measure on a disk) |
| `lib_shuffle.c:527`, `uilog.c:293, 302`, `batcal.c:376`, `powerlog.c:469`, `ab_store.c:840`, `bcmfw-boot.c:512` | tuning | agnostic |

### 5.1 Paged lists: Notes and Contacts (P12 W3-K review, 2026-10-24)

A Notes or Contacts index larger than the screen's working block is read a page at a time while browsing (one open and one range read: a page load). On flash a page load is a few milliseconds. On a hard drive, a page load that finds the drive spun down (5 s after its last access) waits for a spin-up of 1-3 s **on the UI thread**: the list would freeze mid-scroll. Rule 7 forbids that difference, so the build does the same on both, and only the cost differs:

| Choice | Flash | Hard drive | Why this one | Status |
|---|---|---|---|---|
| Entry read (`load_index()`, notes.c and contacts.c) | one read of the head and every page that fits the working block (the whole index when it fits: then no page load ever) | the same read | free: it is the entry's one read anyway. Typical indexes (a few hundred notes, a few hundred cards on the 16 KB module block) are whole; the resident blocks (8 KB Notes, 7 KB Contacts) hold the head and the first pages | done (sim) |
| Drive held awake while browsing (`xindex_browse_keep()`, xindex.c) | resets the idle hold: the adapter stays out of standby while the list is used | the drive keeps spinning while the list is used: no spin-up between page loads | lowest cost that keeps a scroll smooth. Only when the index is paged (not all in RAM), only while the screen is lit, and for at most `XIDX_BROWSE_HOLD` (30 s) after the last wheel input; `storage_spin()` never wakes a drive that has already stopped. Cost: on a disk, ~30 s more spinning per browse of a large list [est: a few mA·h per session]; on flash, the adapter's active draw instead of standby for the same time [est, small] | done (sim); device: measure the spin cost on a disk unit (#79) |
| Read-ahead of the next page (`keep_window()`) | the next page the way the list moves, when the cache holds it beside the visible pages | the same, only when `xindex_disk_warm()` (a read costs no spin-up) | a steady scroll meets cached rows; never wakes the drive by itself | done (sim) |
| Page-load rate (`LOAD_MAX` = the manifest's `pages` − 2) | a window's pages load only while the last second's loads leave room; a faster spin stops at the cached rows' edge until the budget returns (≤ 1 s) | the same | keeps the accountant's `pages = 8` under the wheel's top acceleration (10 ms clicks, 5 rows each) with 2 loads in reserve for an entry; measured by `ipodui-p12-contacts-spin` (no viol) | done (sim) |
| What is left | none | a list resumed more than 30 s after its last input, or after the screen went dark, can still meet a spun-down drive at the next uncached page (one 1-3 s wait, then smooth again) | the alternative, the whole index in RAM, costs up to 96 KB (Notes) or 256 KB (Contacts) per open list | open: a device check on a disk unit decides whether the hold should follow the backlight instead |

## 6. Background work

| Choice | Effect | Status |
|---|---|---|
| Art builder pacing (`artcache.c:61-73`, `GAP_TICKS` HZ/10 between albums) | on a disk, repeated spin-ups unless batched inside one awake window | open: measure on a disk (medium) |
| Tagcache scan and commit pacing | no storage gate | open: measure on a disk |
| No-change boot digest (#89, `bootstamp.c` `boot_stamp_digest()`): directory reads only, capped at `BOOT_DIGEST_CAP_MS` (1 s); over the cap the boot walks (`why=slow`; never "clean shutdown and no USB" alone, until the device's `boot digest ms=` line is in). Boot window hold `BOOT_HOLD_MS` (3 s), a walk projected past it hands over at once | same rule on both; flash [est 0.2-0.5 s for 418 folders] fits the cap, a disk (seeks, a larger library) may hit it and walk: same look (the boot screen), the walk's time | runtime (same rule); device time open (devmode `boot digest ms=`) |
| Directory bursts (R1, #107; `fat.c` `dir_burst_get()`): the disk-cache lock is held across each burst read (up to 16 KiB, one command), so buffering's FAT lookups and the tagcache wait for it | flash: about one command, ms [est]; disk: a spin-up inside the lock stalls buffering for the spin-up [hyp], although the same read would also spin it up without bursts | open: measure the worst buffering stall on a spinning-disk unit (device checklist on #107). Mitigation if it matters: drop the lock during the ATA read and register the slot only if a generation counter (bumped by every drop) is unchanged |
| Deleted-file pass after USB, with dircache compiled out (`ipodvideo-variant.h:55`) | seconds on flash, minutes of seeks on a disk (a longer "Updating Library…") | recorded tradeoff; dircache costs ~3.9 MB at 50k tracks (large) |

## 7. USB

Sector translation (§2), read-ahead (`hardware.md`, p9-usbperf), flush on eject: the same on both except the sector multiplier.

**Update 2026-10-17:** on the iPod Video the ring's worker is off by default (`USB_PREFETCH_DEFAULT_OFF`) and reads are serial; see §11.

Prefetch ring (#81, `HAVE_USB_PREFETCH`): **the same on both**, same sizes (8 MB ring, 256 KB reads). On flash it overlaps the adapter's ~21 MB/s with the USB side; on a disk the large sequential reads cut per-command seeks and rotational waits for the same scan, so the same code helps both. RAM may change it: a unit whose audio buffer cannot spare 8 MB gets 4 or 2 MB, or the #63 read-ahead.

## 8. Simulator

The sim copies the 1 s / 5 s hold (`artcache.c:160`, `filesystem-sim.c:428`, `SIM_SPINDOWN` / `SIM_ATA_HOLD`). Plan: a script switch (`disk model flash|hdd`) instead of a build flag, and every storage-sensitive script runs under both. Until then: a `--storage=hdd` sim beside the default one.

## 9. What cannot be identical (aim for the same look, not the same timing)

- **Saving at critical battery:** a spin-up can brown out a sagging cell, so a disk keeps its floor and skips the save; flash can save lower.
- **The wait before sound and the fill boost:** a 1–3 s spin-up is physics. The user sees the same thing (a short wait, then sound); only the wait's length differs.
- **SMART and forced UDMA:** only where the drive supports them.
- **The deleted-file pass without dircache:** seconds against minutes, unless memory goes back to dircache.

## 10. Docs that only make sense on flash (to reword as "solid (measured) / rotating (assumed)")

`docs/fit1-brief.md` §B.4, the C1/C4a/C7 table, A.4 item 1; `docs/hardware-fit-audit.md` C1, C3, C7, C12; `docs/hardware.md` estimates ("~5-20 ms each on the flash", the `library.c:6147` figure); `apps/tagcache.c:4054` ("flash never in standby").

## Compile-time flags to become runtime decisions

`IPOD_STORAGE_HDD` (→ `HAVE_DISK_STORAGE` / `HAVE_FLASH_STORAGE`); `HAVE_ATA_POWER_OFF`, `HAVE_ATA_SMART` (compile both, act on capability); `ATA_MAX_UDMA` (fix at 4); `ATA_SLEEP_TIMEOUT_DEFAULT`; `PCMBUF_PREFILL_QSECS`; the fill-boost gate; the watermark branch; the sector-multiplier branch; `battery_level_disksafe`; the save-at-critical gate; `HAVE_ATA_UDMA_FORCE`; the sim's `SIM_SPINDOWN` / `SIM_ATA_HOLD`.

## USB prefetch wait (#81 reliability fix, 2026-10-15)

`PF_WAIT_TICKS` is HZ/5: a READ waits at most 0.2 s for the prefetch worker before reading the ordinary way, so the USB thread can never sit long enough to make the host reset the device. On flash a piece lands in milliseconds. On a hard disk a spin-up (1–3 s) makes pending pieces time out and be read twice until the disk is up: a cost difference only, the same behaviour. A runtime choice from `storage_spinup_time()` could lengthen the wait on a disk if measurements show the double reads matter.

## 11. What macOS does at mount, and cluster size (2026-10-17/18, #81)

**Apple's mount scan [source reading, not a device result].** Apple's published FSKit FAT code (`msdosVolume.FatMount` builds `FATManager`) sets its metadata block to `META_RW_BLOCK_LEN = 3 * 4096` (12 KiB) and always calls `updateFATStats`, which walks the whole FAT to recompute free-cluster data. A valid FSInfo does not skip it, and it is not a repair or a dirty-volume check. That matches the 12 KiB requests we see. The checker's quick clean-bit path is a different phase and does not remove the sweep. We have not shown that this exact revision runs on the development Mac. Sources: [FATManager.m](https://github.com/apple-oss-distributions/msdosfs/blob/main/msdos_appex/FATManager.m), [msdosVolume.m](https://github.com/apple-oss-distributions/msdosfs/blob/main/msdos_appex/msdosVolume.m), [check.c](https://github.com/apple-oss-distributions/msdosfs/blob/main/lib_fsck_msdos/check.c), [msdosFileSystem.m](https://github.com/apple-oss-distributions/msdosfs/blob/main/msdos_appex/msdosFileSystem.m).

**The 20 s deadline is unproven.** Our early sessions assumed an ~18-20 s activation limit. The published source has `CHECK_MAX_TIME 200` s and no universal activation limit. Different stages may have different timeouts, so a native host error (stage, domain, code) must be captured before treating 20 s as the cause. The 15 s acceptance goal in [`usb-history.md`](usb-history.md) is our own bar.

**Cluster-size arithmetic** (this volume has ~62.55 million 16 KiB clusters):

| Cluster | One FAT | BPB sectors per cluster (2048-byte sectors, the on-disk layout) | USB 512-byte view | Transfer-only scan at 21-22 MiB/s |
|---|---|---|---|---|
| 16 KiB (now) | 250 MB | 8 | 32 | 11-12 s |
| 32 KiB | 125 MB | 16 | 64 | 5-6 s |
| 64 KiB | 62.5 MB | 32 | 128 | ~3 s |

Limits: the sector translator (`usb_storage_xlate`) allows at most 128 sectors per cluster in the 512-byte view (64 KiB); `firmware/common/fat.c` handles clusters up to a calculated 128 KiB, an implementation bound and not a proven one. Cluster size, Apple's 12 KiB request, our 64 KiB transfer buffer and the 4 KiB ARC dTD pieces are four different quantities. Apple FATVolume's 32 KiB "optimal I/O size" is not a requirement for 32 KiB clusters. Larger clusters shrink the metadata work; they do not fix a failed INQUIRY.

Migration needs a verified file backup and a recovery path. Do not change cluster size in place. Plan: [`fat32-migration-plan.md`](fat32-migration-plan.md).

**Serial USB read policy (flash, PP5022).** The iPod Video build defaults to no prefetch worker and no ATA/USB overlap (`USB_PREFETCH_DEFAULT_OFF`, `USB_STORAGE_NO_READ_OVERLAP`), because the two bus masters interfere ([`data-paths.md`](data-paths.md) D12). On a hard-disk unit the same code runs; batching larger demand windows ([`usb-history.md`](usb-history.md) step 4) is expected to help there too [hypothesis, no disk measured]. Revisit with the storage-dependent choices above.

## Boot battery guard (2026-10-22)

| Choice | Why | Flash | Hard drive | Single-build plan | Status |
|---|---|---|---|---|---|
| When the boot guard samples the battery (`apps/ipodui/bootbatt.c`, `docs/hardware.md` "Boot battery guard") | the decision must come before any storage work, so a flat cell never spins a disk | idle drive: the sample is a rest reading | the bootloader has just read the firmware, so the drive may still be spinning: the sample can sit some tens of mV lower [hypothesis, unmeasured], and a marginal cell is refused a little sooner | same code and threshold on both. Waiting for the drive to spin down would cost seconds on every HDD boot, and an earlier standby means `storage_init()` before the decision, both worse. Measure the sag on a stock HDD unit (#79). If it matters, give HDD a margin offset chosen at runtime, once storage is detected before the guard. Tuning only: what the user sees is the same | agnostic |

## One-sector records on 2048-byte sectors (2026-10-23)

| Choice | Why | Flash | Hard drive | Single-build plan | Status |
|---|---|---|---|---|---|
| Size of `.rename_intent` and `.bootbatt` (`firmware/common/file.c` `record_create()`) | `record_io()` works in the file layer's sector (`fat_file_sector_size()`, 512 B), not the BPB's (2,048 B on the 5.5G's layout) | the device's files were 0 B [silicon]: the write was skipped, every record refused | the same (the 2,048-byte layout is the 5.5G's, not the drive's) | one size, `DC_CACHE_BUFSIZE`, in both; a too-short record is made again at boot | agnostic (fixed) |

## Now Playing cover render (np-sharp, 2026-10-24)

| Choice | Why | Flash | Hard drive | Single-build plan | Status |
|---|---|---|---|---|---|
| When the queued Now Playing cover renders into `npcover.pak` (`apps/ipodui/artcache.c` `npr_place()`, `docs/design/artcache.md` section 13) | it must never cost a power-up of its own | storage is awake briefly after a refill (about 1 s): the render waits for one of those wakes with the user idle | the disk spins for longer after a refill (about 5 s), so the window comes sooner and lasts longer; the render's ~360 KB of writes ride a spin that is happening anyway | the same rule on both: only with `storage_disk_is_active()` already true, never a wake of its own. Only how often a window opens differs. What the user sees is the same: the thumbnail until the render lands | agnostic |
