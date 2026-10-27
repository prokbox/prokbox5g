# USB mount speed: the sub-project log (#81)

The one place to look before trying anything on USB speed or macOS
auto-mount. It records where we have been, what each attempt showed, and
the next best ideas, so we stop circling the same solutions. Update it
after **every** experiment, device round or new finding: add a row to §4
or §7, re-rank §5. Chronology and commit trail: [`usb-history.md`](usb-history.md).
Hardware facts: `pp5022-reference/usb.md`, `data-paths.md` D12-D14.

## 1. Goal and gate

Native macOS (Finder / Disk Arbitration / FSKit) mounts the iPod on its
own, with no helper and no host setting (owner: fix it in the firmware or
the disk layout, never on the host). Gate (`usb-history.md`): ten attach
cycles, no reset or manual step, mount complete in ≤ 15 s median and worst.

## 2. The mechanism (what we are fighting)

- At every mount, Apple's FSKit msdos module walks the **whole FAT** to
  count free clusters (`FATManager updateFATStats`), in **12 KiB READs at
  queue depth 1**, even when FSInfo is valid. [apple-source: msdosfs
  `FATManager.m`]
- Rehearsal on this Mac (2026-10-18, 1 TB sparse images, 512-byte sectors,
  native attach, memory speed) [measured]:

  | Clusters | FAT bytes the host read | Mount at memory speed |
  |---|---|---|
  | 16 KiB (the iPod today) | 233 MB | 2.5 s |
  | 32 KiB | 117 MB | 1.8 s |
  | 64 KiB | 59 MB | 1.1 s |

  So the host's own work is small; **the sweep is bound by our per-command
  rate on 12 KiB reads**, and its size scales exactly with the FAT.
- macOS gives up at **about 20 s** in practice: 2026-10-10 it had read
  106 MB in 19.5 s (8,643 READs of ~12 KB, 5.4 MB/s) when the mount failed
  [measured, `hardware.md`]. No published FSKit constant says 20 s
  (`CHECK_MAX_TIME` is 200 s, for the checker): the limit is observed, not
  documented. The first idea in §5 captures the host's own error to confirm it.
- **What the sweep needs**, at ~18 s usable:

  | Clusters | FAT | Rate needed for 12 KiB READs |
  |---|---|---|
  | 16 KiB | 233 MB | ≥ 13 MB/s |
  | 32 KiB | 117 MB | ≥ 6.5 MB/s |
  | 64 KiB | 59 MB | ≥ 3.3 MB/s |

## 2a. Baseline to protect (do not regress)

Since build `2465b3d092` (static INQUIRY buffer) and the handover fixes, every logged session ([`usb-sessions-catalog.md`](usb-sessions-catalog.md)):
- **No dead enumerations.** `inq2data=1` every time, against 12 DEAF sessions before.
- **Storage handover in 0-3 s.** The worst was 11.45 s (artcache); before the fix it was 54-146 s.
- **Resets down to a floor of about 4 bus resets, with 0-1 bulk resets.** Before, there were dozens to hundreds.
- **Writes clean.** `wrfail=0`, up to 5,989 MB in one session.

Owner: manual mounts now succeed in seconds instead of up to a minute.

Every speed change must keep all four. Re-check them with `scripts/usb_sessions.py` after each device round before judging its speed.

## 3. Rates measured so far

| Path / build | Pattern | Rate | Source |
|---|---|---|---|
| Pre-serial (2026-10-10) | mount sweep, 12 KiB | **5.4 MB/s** | hardware.md |
| Overlapped prefetch worker | large reads | 11.2 MiB/s, resets mid-read | hw-experiments §7 |
| Serial read-ahead `fa571b6895` | large reads | 15.7-17.5 MiB/s | hw-experiments §7 |
| True no-overlap `41103a9158` (today's default, + `23d1223bba`) | native mount, first 18 s | **~2.5 MiB/s** | hw-experiments §7 |
| Flash adapter alone | large reads | 21-22 MiB/s | hw-experiments §7 |

The shipping default is the slowest of these on the pattern that matters:
fully serial, one 12 KiB ATA read per host READ, read-ahead compiled out.

From the full catalog of 104 logged sessions ([`usb-sessions-catalog.md`](usb-sessions-catalog.md)), the mount-phase rate (`rd30`, KB in the first 30 s):

| Builds | Path | rd30 rate | Resets |
|---|---|---|---|
| `073c8e4366M` | 8 MB prefetch ring (overlapped) | **9.5-10.1 MB/s** | stalls (fault A) |
| `953be4b67a`, `bcb3e8d326`, `ea10f38c9f`, `5a1d396d87` | 8 MB ring | 7.7-9.3 MB/s | 5-8 per session |
| `2cefd681ce` (4 KiB dTDs) | 8 MB ring | ~3.3 MB/s in 3 of 5 sessions | — |
| `b669d2e493`, `7de657f382` | serial, ring off | ~2.5 MB/s (t18) | floor |
| `2da4d5b355` | serial | 7.8-8.0 MB/s, 64 KB mount reads | likely a helper/manual mount (inference) |

So **the serial default traded 3-4x mount speed for reliability**. Fault A (the ~304 ms active-dTD stall) shows in every ring build from `26e13c10fd` on; it is the reason the ring went off.

Log caveat: since `ad29aae06d` many sessions lost their `usb exit`/`cmd` lines (`log dropped=N`): the exit block wrote more than the 12-line ring before its first flush. Fixed on `usb-batch` (`674b1363de`).

## 4. Tried, and what it showed (do not repeat without a new reason)

| Idea | Result | Verdict |
|---|---|---|
| Overlapped prefetch worker (ATA DMA during USB IN) | faster on big reads, but ~304 ms active-dTD stalls and resets | off by default (D12); Developer A/B only |
| Serial read-ahead (synchronous, no overlap) | 15.7-17.5 MiB/s on big reads; one failure at 345 MiB in the production build | promising, not finished; compiled out today |
| True no-overlap (`USB_STORAGE_NO_READ_OVERLAP`) | stale alternating 64 KiB halves until `23d1223bba`; then clean but ~2.5 MiB/s on the mount | correct, too slow |
| CPU boost held for the session (`usb_boost`) | no usable storage, 68 resets | unusable (removed) |
| USBMODE stream disable | bit never sets on PP5022 | no-op (removed) |
| TXFILLTUNING (0x164) | reads back 0; dead enumeration in 2/5 sessions | never write (removed) |
| 4 KiB dTDs, prime retry, overlay reset | no effect on the first-command timeout | hygiene, kept |
| E2 display path off | first mount failed | withdrawn; E2 stays |
| Static INQUIRY buffer | first reply completes; manual mounts in seconds | **keep, don't touch without a device A/B** (owner) |
| Handover: tagcache/artcache quiesce | 54 s handover → 240 ms | fixed |
| Valid FSInfo / clean flags | FSKit sweeps anyway | not a lever |
| Host helper / fstab / mount_msdos | owner rules out host-side fixes; helper only a diagnostic | not a product path |
| exFAT | owner chose FAT32 | out |

## 4a. Per-command budget (2026-10-18 analysis, estimates unless cited)

One 12 KiB READ on today's serial path, CBW to CSW:

| Step | ms |
|---|---|
| CBW in, IRQ, USB thread wake (REALTIME, CPU already boosted) | 0.10-0.20 |
| CBW parse, SCSI, sector translation (no-op off boot sectors) | ≤ 0.05 |
| ATA: command latency L (unmeasured) + 12 KiB at 21 MB/s (0.57) | **1.0-3.5** |
| USB IN, 3 × 4 KiB dTDs | 0.35-0.5 |
| IN-complete IRQ, CSW, CSW IRQ | 0.15-0.30 |
| Host turnaround to the next CBW (queue depth 1) | 0.2-0.5 |

The anchors (2.25 ms/cmd in 2026-10-10; ~4.7 ms/cmd for today's serial
build at mount start) put L at about 1-3 ms. **L is the term to measure.**
A fully serial design adds flash time and USB time, so its ceiling is
about 11 MB/s (~1.1 ms/cmd), even with perfect batching:

| Fix | ms/cmd | 233 / 117 / 59 MB sweep (s) |
|---|---|---|
| Today (est. 4.7) | 4.7 | 89 / 45 / 23 |
| Demand batching, 256 KiB window | ~1.5 | 28 / 14 / 7 |
| + data IN and CSW chained in one prime | ~1.25 | 24 / 12 / 6 |
| + next window's ATA read in the host gap (serial-safe overlap) | ~1.1 | 21 / 10.5 / 5.3 |

**Conclusion:** 16 KiB clusters cannot be rescued by any serial-safe
firmware change. 32 KiB clusters need batching, and land at about 14 s,
marginal against the 15 s gate. 64 KiB clusters with batching land at
about 7 s. Also found: the storage class already asks for a CPU boost
(`needs_cpu_boost`, `usb_core.c` boosts at SET_CONFIGURATION), so the
retired `usb_boost` held a *second* boost; its failures were probably
something else (fault B), not the clock. Nothing logs the CPU frequency
during USB yet.

## 5. Next best ideas, ranked (re-rank after each result)

1. **Measure on the device (3a), with two small log additions first:**
   - the ATA command time per READ (a histogram: count and µs buckets);
   - the CPU frequency in the `usb mnt` line.

   Then one native-only attach, with `log stream` on diskarbitrationd/fskitd/msdos plus the firmware's `usb mnt` trace. This gives L, the real ms/cmd and the host's error, and decides how far batching can get.
2. **Demand read batching (serial, no overlap)** — *implemented on `usb-batch` (`664b3daec2`), awaiting the §6 device round.* On a sequential miss
   read 64-256 KiB in one ATA command into a raw-sector cache, finish ATA
   before any Bulk-IN, serve the next 12 KiB READs from RAM. Removes the
   per-command ATA latency that dominates 12 KiB reads; honours the
   load-before-send rule. Needs: separate raw cache (the translator edits
   buffers in place), invalidate on writes/reset/disconnect, host tests for
   byte identity. Expected: about 4.7 → 1.5 ms per command, ~8 MB/s (§4a).
3. **32 KiB (or 64 KiB) clusters** — halves (quarters) the sweep. Plan:
   `fat32-migration-plan.md` (verified file backup, rehearsal, format the
   data extent only). 32 KiB needs ≥ 6.5 MB/s; with idea 2 that looks
   reachable; 64 KiB needs only ≥ 3.3 MB/s but sits at the translation
   limit and is untested on Apple's firmware.
4. **Chain the data IN and CSW in one prime** (saves one IRQ/thread round
   trip, ~0.1-0.2 ms/cmd). Touches the ARC driver; after 2 is stable.
5. **Next window's ATA read in the host gap**: start it after the CSW
   while no Bulk-IN is active, and spin before any IN. This is the only
   way past the serial ceiling. It stays within D12/D13, but the CBW-OUT
   DMA during an ATA read is untested. A/B it against the pure serial
   control, last.
6. Low value / done: skipping translation outside boot sectors (< 2 %);
   UDMA4 (measured no faster, D3); 16 KiB dTDs back (< 3 %); boost (already
   on); semaphore instead of the INTRQ spin (slower, LCD-yield risk); a
   FAT-synthesis cache (stale-data risk, no help on the first scan).

## 6. Next device round (owner go + backup)

Install the `usb-batch` build with a backup, then:
1. **Native attach only.** Disable nothing on the host, but don't use the helper's manual mount. Capture `log stream --predicate 'process == "diskarbitrationd" OR process == "fskitd" OR process CONTAINS "msdos"'` from plug-in to the mount or the failure.
2. Unplug, replug, eject cleanly. Then pull the logs and run `scripts/usb_sessions.py`:
   - check the §2a baseline;
   - read `usb stor` (L = the small-read mean minus 12 KiB at 21 MB/s, and the MHz);
   - read `usb pf` (fills, hits);
   - read `mnt t18`/`rd30` (the mount rate).
3. Compare with the §4a predictions: about 1.5 ms per command, or about 8 MB/s. Then decide:
   - **≥ 7 MB/s** → 32 KiB clusters make the gate (117 MB in ~15-16 s); go to `fat32-migration-plan.md`.
   - **Lower** → look at L. If batching barely helps, the cost is per-command USB overhead (idea 4), or the host read isn't sequential.
4. Repeat 3 attaches for variance.
   - Check that the handover stays within 0-3 s (§2a): the 1 MB ring is now allocated at SET_CONFIGURATION, which can make playback give its buffer back (look for an `audio shrink` line).
   - Judge whether batching ran from `usb pf fill=`/`hit=` and `sw pf=batch`.
4a. **Same install, baseline arm:** Developer → "USB prefetch next" → off (plain serial reads, no ring: the `usbfix` behaviour). Then 3 more attaches. Batching stays only if it beats this arm clearly, with no new resets. If batching ever misbehaves, this switch is the way out without a reinstall.
5. **Optional, only if steps 1-4 are clean: INQUIRY buffer alignment A/B.** `inquiry_static` is a 36-byte reply declared `aligned(4096)`. That costs 0-4,095 B of `.bss` padding, and the amount changes with every unrelated edit. It already pushed #85 over the 1,100,000 B `_end` cap once (2026-10-18: a 3,628 B pad).
   - The device proved that the static, uncached, page-aligned reply fixes the stuck first INQUIRY. Which property does it was never separated.
   - Test `aligned(64)` (a cache line; 36 B can't cross a 4 KiB page) on a marker build, with the same uncached alias and still static. Pass: `inq2data=1` and a handover of ≤3 s on 3 attaches, matching §2a.
   - Revert at once if the first INQUIRY stalls. Never ship it without this test (owner rule).

## 7. Findings log (newest first)

- **2026-10-18** — **Independent integrity review of demand batching: no HIGH or MED findings.** Verified to hold:
  - writes, vendor commands and unknown opcodes drop the ring before any data phase;
  - the ring is re-initialised at each configuration and dropped at Bulk-Only reset;
  - the translation is applied exactly once;
  - LBA bounds are kept, including at the end of the disk;
  - a failed read gives a FAIL CSW, never good-looking garbage;
  - no ATA during a bulk-IN, no use after free, and no disk writes during USB.

  LOWs fixed (`b3a913f83c` and the next commit): the write-drop hook is coupled to `HAVE_USB_PREFETCH`; no worker stack in batching mode; `pf ring=` is no longer zeroed by the screen's stats reset; bus registers are sampled in `usb_drv_init()` while clocked. The Developer switch is now three-way: batch / off / worker.
- **2026-10-18** — **`inquiry_static` alignment pad is a budget hazard.** In #85 round 2, a 3,628 B pad in front of the 4 KiB-aligned 36 B INQUIRY reply took `_end` to 1,103,456 B. Resolved by trimming #85 code, not by touching the buffer. An alignment A/B is now §6 step 5.
- **2026-10-18** — **Demand batching implemented** on branch `usb-batch` (`664b3daec2`). It is idea 2: the prefetch ring with no worker thread, filled by the USB thread in line, only between pieces.
  - Ring: 1 MB, or 512 KB if the 1 MB allocation fails. Chunks start at 64 KB and ramp to 256 KB.
  - The host model (`tests/usb/pf_test.c`) runs every case in both modes, byte-checked. On its scan, one storage read served 5.3 READs; that is 128-sector chunks against 24-sector READs, so about 21 on the device.
  - `674b1363de` adds `usb stor small=n/mean_us big=n/mean_us/KB mhz=min-max` (L and the clock), and keeps the exit lines.
  - **Not on the device yet.** It needs one round with a backup (§6).
- **2026-10-18** — Hardware session on PP5022 bus arbitration, the only route past the ~11 MB/s serial ceiling toward USB-2.0 rates:
  - **No known register sets priority between IDE DMA and USB DMA.** Candidates, none shown to matter: CACHE_PRIORITY 0x6000_6044 (Rockbox writes 0x3F; meaning unknown); XMB_RAM_CFG 0x7000_003C (USB init ORs in 0x47A); DEV_TIMING1 0x7000_0034.
  - SDRAM is one shared resource [silicon, E2 lab]: a CPU loop in SDRAM kept 775/1000 of its speed while DMA engine 2 streamed, against 968/1000 working in IRAM.
  - The IDE DMA has no burst field. The ARC's BURSTSIZE (0x160) is never written, so it runs at the reset default; SBUSCFG (0x90) may not exist on this revision.
  - A device-mode IN endpoint whose memory fetch can't keep up should NAK, which fits fault A [guess].
  - The fixed ~304 ms matches no iPod timer, so it is probably the host's: macOS gives up on the IN pipe [guess]. Host-log timestamps around a reset would show it.
  - **Overlap track:**
    - **(1)** Read-back of those registers, logging only: placed, `1c28b29a34`.
    - **(2)** Marker A/B with the worker on: BURSTSIZE.TXPBURST smaller (e.g. 0x08), and/or TXFIFOTHRES raised. Mind the earlier TXFILLTUNING dead-enumeration correlation; only after (1) shows the register exists.
    - **(3)** IDE yields the bus: cap one IDE DMA command at 32-64 KB, or UDMA1, with the worker on. If the stalls vanish at ~17 MB/s, the problem is bus hogging, not protocol.
    - **(4)** CACHE_PRIORITY bit-by-bit, last, with a fresh backup.

- **2026-10-18** — Prior art (web, thin) [external]:
  - Stock iPod Video 5G HDD over USB 2.0 synced at ~2.6-4.7 MB/s (forum reports), so Apple's own disk mode was never fast.
  - Rockbox's 64 KB read buffer exists because 64 KB is the hosts' common transfer size. An iPod Classic commit cut it to 24 KB and reported +8-9 %.
  - Upstream-ish `e13befb925`: the ATA-wait yield let an LCD redraw stall DMA completion; reads went from ~13 to 18 MB/s. We carry that idea as the INTRQ spin in USB mode.
  - PP5020 ATA caps UDMA by CPU clock: UDMA2 at ≥ 30 MHz, UDMA1 at 24 MHz; UDMA4 needs a boost (+~10 % on a stock disk). Holding a boost broke our USB, so check that the PLL hold keeps 30 MHz during USB reads.
  - Nothing published on macOS FSKit request sizes. Advertising SCSI Block Limits (VPD B0) is unlikely to help the sweep: FATManager's 12 KiB is a source constant (`META_RW_BLOCK_LEN = 3 * 4096`), not derived from the device. Low priority.

- **2026-10-18** — Rehearsal (§2): native mount reads exactly one FAT;
  linear in cluster size; host side ~1-2.5 s at memory speed.
  32 KiB alone fails at the 2026-10-10 rate (117 MB / 5.4 MB/s ≈ 22 s);
  it passes at ≥ 6.5 MB/s. Images: 1 TB sparse, `newfs_msdos -F 32 -S 512
  -b 16384|32768|65536`, `hdiutil attach -imagekey
  diskimage-class=CRawDiskImage`.
