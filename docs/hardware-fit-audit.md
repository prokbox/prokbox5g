# Hardware-fit audit

> **Refreshed 2026-10-20:** see "2026-10-20 re-audit" at the end for the current status and ranking (R1-R14, M1, L2, A3, RUI-0..5). The 2026-10-13 text below is kept as history.

*2026-10-13, branch `hw-fit-audit` from `s12`. Audited builds:*
- *the owner variant `fw-ui` (`flash-64-3000-usbc-haptic`, image 709,856 B);*
- *the video build `fw-bcmfw` (text 811,108 B).*

*Sources: the PP5022 and BCM2722 references (with `p14-dev41`'s `memory-access.md`) and the backup folder logs; extracts in `docs/audit/hw-fit-evidence.md` (E1–E5).*

## Findings

1. **Upstream's weight is in its policies, not its bytes.**
   - Upstream-derived code is about 427 KB of the 710 KB image. Statics are about 350 KB, and 150 KB of that is the framebuffer (E4).
   - Deleting 100 KB would add only 0.16 % to the 62 MB audio buffer.
   - What costs us is HDD-era behaviour: a 5 s idle hold, boosting for I/O, polled waits, a starving priority ladder.
2. **The storage never sleeps while stopped.**
   - `disk_pct` is 100 in 1,466 of the 1,535 rows logged while stopped (E1).
   - While playing it is 7.6–16.7 %.
3. **At 30 MHz, scheduling decides latency, not clock speed.** The starvation comes from two mechanisms:
   - the pcmbuf ladder (`apps/pcmbuf.c:395–421`);
   - aging by switch count (`firmware/kernel/thread.c:1084`, a skipped thread runs only after `diff²` switches).
   
   What the logs show (E2):
   - S26: no posts for 420–520 ms.
   - S31: the feed ran 2.1 s late.
   - S34: unboosted with a priority floor held 16 ms of drift; boosted without one, 70 ms.
4. **The COP is idle.** It runs only MP3 synthesis (`mpa.c:29`); every other thread is pinned to the CPU (E5).
5. **We can't prove battery gains yet.**
   - There is no current sense, so every battery claim needs a measurement first.
   - The core voltage is fixed (DCDC1 1.2 V, `firmware/drivers/pcf50605.c:90`), so energy per cycle is roughly constant across clocks.
   - So a boost costs mostly through wasted cycles and the PLL [hypothesis].

## Candidates

Fields: **Now** (file:line), **Fact**, **Proposal**, **Gain**, **Risk**, **Cost**, **Measure**, **Rule 7** (stock configurations keep working).

### M0. Measurement kit (prerequisite)
- **Now:**
  - `power.csv` holds shares, not CPU time.
  - Database device estimates use an uncalibrated ×200–400 simulator factor.
  - The video thread's "active ‰" includes time it spent pre-empted (E2).
- **Fact:** there is no current sense (`pp5022-reference/open-questions.md` #25). `USEC_TIMER` and `switch_thread` are available hooks.
- **Proposal:**
  - Per-thread CPU µs and idle µs, written to `power.csv` (`idle_pct`) and the developer screen.
  - A tap-to-first-PCM log line.
  - One `lib50k` scan and commit on the device, to calibrate the simulator factor.
  - One owner session with an inline USB-C meter, covering: screen on/off, 30/80 MHz, BCM on/off, disk active/standby.
- **Gain:** turns every estimate below into a number.
- **Risk:** low (about 20 cycles per switch).
- **Cost:** 2 days plus one session.
- **Rule 7:** neutral.

### C1. Storage idle discipline on flash
- **Now:**
  - "Active" holds for 5 s after each access (`firmware/drivers/ata.c:99`).
  - The spindown setting is applied only under `HAVE_DISK_STORAGE` (`apps/settings.c:968`).
  - The shell writes opportunistically "while the disk spins".
- **Fact:** the controller stays ON while stopped (E1); flash has no spin-up, so the 5 s hold is HDD hysteresis.
- **Proposal:**
  1. Log who touches `last_disk_activity` while stopped, and remove the periodic toucher.
  2. Use a 1 s timeout on flash.
- **Gain:** the adapter mostly in STANDBY, fewer opportunistic writes; battery effect per M0.
- **Risk:** low.
- **Cost:** 1 day.
- **Measure:** stopped and dark-playing `disk_pct`, then an A/B in mV/h.
- **Rule 7:** HDD keeps its 5 s hold and spin-up logic.

### C2. A simpler codec priority policy
- **Now:**
  - The codec rises to priority 5–14 whenever pcmbuf is under 70 % of 3 s (`pcmbuf.c:395–421`).
  - Aging is count-based (`thread.c:1084`).
  - The video player works around both by following the codec's priority, plus a floor (dev33/dev39).
- **Fact:** underruns happen only when the ring runs dry. Raising priority above about 0.5 s of PCM just steals from the UI and video (E2; `quirks.md` #41).
- **Proposal:**
  - The codec stays at `PRIORITY_PLAYBACK`.
  - It steps to 8 only below 0.5 s of PCM, with hysteresis.
  - Aging becomes time-based: a thread skipped for 20 ms gets a slice.
- **Gain:** UI and video gaps during starts, seeks and skips drop from hundreds of ms toward 12–16 ms; the player's follow and floor code goes; dev41's unboosted default rests on design.
- **Risk:** medium (heavy codecs at 30 MHz).
- **Cost:** 2–3 days.
- **Measure:** stage 7 underruns and drift, a rerun of the S34 matrix, M0 latencies.
- **Rule 7:** neutral.

### C3. A shorter PCM prefill
- **Now:** playback starts after 1 s has been decoded (`LOW_DATA`, `pcmbuf.c:79–84`, the `MEMORYSIZE > 2` case), with the CPU boosted (`pcmbuf.c:506`).
- **Fact:** the file buffer holds minutes; the 1 s prefill causes video's 430 ms `elapsed` leap (S24/S25, `host-driver.md` §5).
- **Proposal:** a 0.25 s prefill on flash; keep the 3 s ring.
- **Gain:** start and skip faster by the time to decode 0.75 s boosted: an estimated 50–300 ms, codec-dependent, unmeasured.
- **Risk:** low–medium.
- **Cost:** 0.5 day.
- **Measure:** the M0 tap-to-PCM log line; underruns.
- **Rule 7:** HDD keeps 1 s.

### C4. Don't boost for I/O; ATA completion by interrupt
- **Now:**
  - Every `buffer_handle()` boosts (`apps/buffering.c:651`).
  - DMA completion is polled with `yield()` (`firmware/target/arm/pp/ata-pp5020.c:146–158`).
  - IRQ 23 is unused (`storage.md` §3).
- **Fact:**
  - UDMA2 runs at 30 MHz (`storage.md` §2).
  - Polling keeps the core awake through each 64 KB transfer (about 3 ms).
- **Proposal:**
  - (a) No boost in `buffer_handle()` on flash.
  - (b) An IDE interrupt plus a semaphore, keeping the 10 s timeout and never reading status during DMA (`quirks.md` #26).
- **Gain:** about 0 boost during fills and USB copies, and the CPU sleeps through DMA.
- **Risk:** (a) low; (b) medium, because USB shares the path.
- **Cost:** (a) 0.5 day; (b) 2–3 days.
- **Measure:** dark-playing `boost_pct` (nonzero in 6 % of rows today); M0 idle during a USB copy.
- **Rule 7:** HDD keeps the boost it needs for UDMA above 2.

### C5. An event-driven video feed thread
- **Now:** the player is "active" for 326–825 ‰ of a session while the port is busy 0.6–1.7 % (E2). It wakes on short timeouts and polls the chip.
- **Fact:** posts are due at known PTS; the BCM's interrupt line (GPIOC bit 6) is unused; a polled read costs 20–47 µs (`display.md` §2–3).
- **Proposal:**
  - Sleep until the next post's deadline.
  - Poll `STATUS` at most every 100 ms; the heartbeat wraps every 262 ms.
  - Prove the interrupt line later.
- **Gain:** if M0 confirms "active" ≈ CPU, a third to a half of a 30 MHz core during video [estimate].
- **Risk:** low–medium.
- **Cost:** 2–4 days.
- **Dependency:** dev41.
- **Rule 7:** neutral.

### C6. 24 MHz with the PLL off when idle
- **Now:** unboosted is 30 MHz from the PLL. `cpu_idle_mode()` (24 MHz, PLL off, `clocks-power.md` §4) is used only by the trimmed radio.
- **Fact:** the PLL runs at 120 MHz before its /4. PIO and `DEV_TIMING1` have 24 MHz values (`storage.md` §2).
- **Proposal:** idle mode while stopped and dark; try it during dark FLAC playback.
- **Gain:** PLL power plus 20 % fewer idle cycles; size unknown.
- **Risk:** low–medium. UDMA2 at 24 MHz is untested, and the port is 20 % slower.
- **Cost:** 1 day.
- **Measure:** a power-log A/B.
- **Rule 7:** neutral.

### C7. Test UDMA4 on the flash adapter
- **Now:** the mode is capped at UDMA2 because IDENTIFY words 76 and 93 are 0 (`firmware/drivers/ata.c:1009`).
- **Fact:**
  - The flash reads about 22 MB/s while busy; USB gets about 18.5 MB/s end to end (`storage.md` §6).
  - UDMA2's ceiling is 33 MB/s.
  - Modes above UDMA2 need 80 MHz per transfer.
- **Proposal:** force `0x44` from a developer switch; checksummed reads, then writes; fall back on failure.
- **Gain:** maybe 20–50 % faster drag-and-drop and mounts [estimate].
- **Risk:** data integrity.
- **Cost:** 1 day plus a session.
- **Rule 7:** HDD keeps its IDENTIFY logic.

### C8. A backlight wake without a BCM power cut
- **Now:** backlight-off cuts the BCM's power; each wake is a 0.37–0.62 s cold boot, panel dark (`host-driver.md` §3).
- **Fact:** the image can gate its core asleep, but no wake source is proven on silicon (`bcm2722-reference/open-questions.md` #3, stage 5i).
- **Proposal:** `LCD_SLEEP` with the core asleep; cut power only after N minutes dark.
- **Gain:** wake latency from about 500 ms to about 30 ms, and no 96 KB upload per wake. The cost is the BCM's idle current (M0 meter).
- **Risk:** medium; the fallback ladder covers a hung chip.
- **Cost:** 3 days after stage 5i.
- **Dependency:** dev41.
- **Rule 7:** same chip on stock units.

### C9. The BCM decodes AAC for video
- **Now:** the soundtrack decodes on the PP, which is what engages C2's ladder.
- **Fact:**
  - A PCM loop through the BCM's SDRAM works (stage 8b).
  - Driving the WM8758 directly is unresolved: GPIO 1 is likely on its own net (`pp5022-reference/audio.md` §6).
- **Proposal:** the BCM decodes into a PCM ring and the PP's I2S DMA plays it. A direct path only if the wiring is found.
- **Gain:** about zero PP decode load during video, and no contention.
- **Risk:** medium–high.
- **Cost:** 2–3 weeks.
- **Dependency:** dev41.
- **Rule 7:** neutral.

### C10. The BCM as the display and compositing engine
- **Now:** the PP renders and pushes every rectangle: about 27 ms per full frame at 30 MHz (`display.md` §5). Animations boost.
- **Fact:** the BCM has 1.25 MB SRAM, 4 MB SDRAM (idle outside video), panel DMA and a vector unit (`memory-access.md` §1).
- **Proposal:** upload covers once; send scroll, angle and overlay commands; the resident image composites.
- **Gain:**
  - Under 1 KB per frame instead of up to 150 KB.
  - Unboosted animation.
  - The 150 KB PP framebuffer could go.
- **Risk:** high; pixel parity with the CPU path.
- **Cost:** 4–6 weeks.
- **Dependency:** dev41, C8.
- **Rule 7:** keep the CPU path as the fallback.

### C11. The codec on the COP
- **Now:** the COP is idle (E5).
- **Fact:**
  - The two cores' caches are not coherent, and maintenance is whole-cache (8 KB).
  - DMA writes the buffering ring physically (`cores-and-threads.md` §3–4).
  - `cpu_boost` clocks both cores.
- **Proposal:** a codec thread on the COP:
  - invalidate before reading the ring;
  - flush before handing PCM over;
  - cross-core calls through the existing corelock queues.
- **Gain:** the CPU is free for UI and video at 30 MHz, with no contention by construction.
- **Risk:** high; upstream abandoned this approach.
- **Cost:** 2–3 weeks.
- **Decision:** after M0 measures the codec's share and C2 has landed.

### C12. Buffering fitted to flash
- **Now:** 32 KB reads (`apps/buffering.c:70`) in a generic handle system; the flash watermark is 1 s of bitrate (`apps/playback.c:1198`).
- **Proposal:**
  - (a) 256 KB reads.
  - (b) Audiobook fills that end on chapter boundaries (`hardware.md` policy).
  - (c) Replace the subsystem only if M0 shows a gain.
- **Gain:** 8× fewer ATA commands per MB; simplicity if replaced.
- **Risk:** low for (a) and (b); high to replace (gapless, art handles, cue sheets).
- **Cost:** 0.5 day, or 2–3 weeks to replace.
- **Rule 7:** HDD and 32 MB units use the same code; the 32 MB unit simply refills more often.

### C13. Our own library store in place of tagcache
- **Now:**
  - tagcache is 25 KB of code, 19.5 KB of statics and a 17 KB stack.
  - Its RAM cache is 7 MB at 50k tracks.
  - Rebuilding after a cut commit is estimated at 15–45 min (uncalibrated).
- **Proposal:** a compact sorted store written at USB disconnect.
- **Gain:** megabytes for 32 MB units, and simplicity.
- **Risk:** high: statistics, the changelog and resume all live in tagcache.
- **Cost:** 4+ weeks.
- **First step:** M0's calibration.

### Not recommended now
- **Tickless kernel / no per-tick COP pulse** (`kernel-pp.c:36`): under 1 %.
- **DVFS:** the PMU part and its voltage formula are unknown (`pp5022-reference/open-questions.md` #20).
- **Trimming for size alone:** see Finding 1.
- **PP DMA for the port:** it is no faster than the CPU copy and corrupts SDRAM writes (`dma.md` §4).

## Ranking

| Rank | ID | Gain | Cost | Risk | Gain proven? |
|---|---|---|---|---|---|
| 1 | M0 measurement kit | enables all | 2 d + session | low | n/a |
| 2 | C1 storage idle | battery, fewer writes | 1 d | low | symptom measured |
| 3 | C2 codec priority | latency, simpler video | 2–3 d | med | starvation measured |
| 4 | C4a no boost on fill | CPU, battery | 0.5 d | low | partly |
| 5 | C3 prefill 0.25 s | start/skip latency | 0.5 d | low–med | no |
| 6 | C5 event-driven feed | CPU during video | 2–4 d | low–med | after M0 |
| 7 | C12a 256 KB reads | fewer commands | 0.5 d | low | no |
| 8 | C6 24 MHz idle | battery | 1 d | low–med | no |
| 9 | C7 UDMA4 test | USB copy speed | 1 d + session | med | no |
| 10 | C4b ATA interrupt | CPU sleep in DMA | 2–3 d | med | no |
| 11 | C8 BCM wake | wake latency | 3 d | med | after stage 5i |
| 12 | C9 BCM AAC | video CPU | 2–3 wk | med–high | partly |
| 13 | C10 BCM compositor | animation CPU | 4–6 wk | high | no |
| 14 | C11 codec on COP | CPU headroom | 2–3 wk | high | no |
| 15 | C13 own library store | RAM, simplicity | 4+ wk | high | no |

## Phased plan

- **Quick wins (about a week, one mini-phase):**
  - M0, C1, C4a, C3 and C12a.
  - C6 and C7 as developer A/B switches.
  - Guarded edits, logged in `docs/changes-from-rockbox.md`.
  - One owner session covers C1, C3, C6, C7 and the M0 meter.
- **Medium (2–3 weeks):**
  - C2, with its own brief and review (playback core).
  - C5 and C8 after dev41's stage 5i.
  - C4b.
  - Once C2 lands, dev41's follow and floor can go.
- **Big rewrites, each its own phase, gated by M0's numbers:**
  - C9, building on stage 8b.
  - C10, which needs C8.
  - C11, only if the codec takes over about 30 % of a 30 MHz core with C2 in place.
  - C13, only if calibration confirms the commit cost or 32 MB units need the RAM.

**Device reality.** Only C2, C3 and C12 have simulator-testable logic; all timing needs the device. Stock stays unchanged (`HAVE_IPOD_UI`, `HAVE_FLASH_STORAGE`), HDD keeps upstream's disk policies, and video candidates touch only `HAVE_BCMFW_VIDEO` builds.

## 2026-10-20 re-audit

*Refreshed on `headers` `eb2f96dea5` (`fw-ui` `_end` 1,092,208 B; `fw-rel` 1,050,656 B) after the owner's rulings:*
- *the audio-buffer floors replace the 1.1 MB cap (`docs/memory-audit-2026-10-20.md`);*
- *the Rockbox UI will be retired;*
- *generic Rockbox code should be reconsidered where knowledge of the hardware allows better.*

*Full report: `docs/research/memory-2026-10-20/hwfit-reaudit.md`. The loader decision is in `docs/design/module-loader.md`. The sections above are kept as the 2026-10-13 record.*

### Status of the 2026-10-13 candidates

| Old ID | Item | Status now | Evidence |
|---|---|---|---|
| M0 | Measurement kit | **Done** (devstats, per-thread CPU, `idle_pct`, `cop_idle_pct`, `m0 tap2pcm`, disk wakes). Still missing: the inline USB-C meter session, HX-B6 (per-command latency), per-thread *wake* counts | ledger 2.8 |
| C1 | Storage idle hold | **Done**: 1 s on flash. The stopped-state wake storm was a commit retry's failed opens (D11, fixed) | ledger 2.8, D11 |
| C2 | Codec priority | **Done**: the ladder is compiled out; fixed bands (codec 13, video feed 14, buffering 15, UI 16) and time-bounded aging | `thread.c`, `pcmbuf.c` rows |
| C3 | Prefill | **Done**: 0.25 s on flash, 1 s on HDD | `pcmbuf.c:86-99` |
| C4a | No fill boost | **Done** on flash | `buffering.c:660` |
| C4b | ATA completion by IRQ | **Built** (`HAVE_ATA_DMA_IRQ`); whether source 23 fires is **unknown** (DP-4, PP #29) | R8 step 6 |
| C5 | Event-driven video feed | Partly: `vplayer.c:1029` waits with a computed timeout, the port waits still yield-poll. P14 | [code] |
| C6 | 24 MHz, PLL off | **Done**: unboosted runs 24 MHz unless a holder (lit screen, ATA DMA, USB, a track that does not fit) keeps the PLL. Lock time and the saving are unmeasured | clocks-power §8 |
| C7 | UDMA4 | **Closed, no gain**: 21 MB/s either way, plus a boost per command (D3) | S36c |
| C8 | BCM wake without a power cut | Open: needs stage 5i and our image as the default display driver | BCM #3 |
| C9 | BCM decodes AAC | Open (stage 9). BCM session | |
| C10 | BCM compositor | Partly: full-width pushes (≥ 16 rows) now go by the PP's **second DMA engine** (E2, #75/#76), so the CPU sleeps through them. The compositor itself is open | ledger `lcd-video.c` (#75/#76) |
| C11 | Codec on the COP | **Done** (Fit-2): CPU idle ~88–92 % against ~45 % on FLAC; COP idle 41–46 % | S36, fit2 §12 |
| C12 | Buffering fitted to flash | (a) **done**: 256 KB fills. (b) chapter-aligned fills still planned. (c) not done | ledger 2.8 |
| C13 | Own library store | Re-scoped as R6 below: the RAM cost is now set by #85's two snapshots | `hardware.md` #85 |

Done since then as well: 8b-T (cold Thumb, splash buffer on the stack, a 32-bucket disk-cache map), #85 (library generations, statistics journal), #81 (USB batching and serial ATA/USB, D12/D13), FAT ordered write-back, DAC standby (P15), the panic record, automatic battery learning (#70).

### New findings

1. **Directory scans cost one ATA command per 512-byte sector.** `fat.c:2852-2862` (`fat_readwrite(dirstr, 1, ...)`) reads directories one sector at a time, and dircache is compiled out. Each command costs an estimated **L ≈ 1–3 ms** (`usb-speed.md` §4a; unmeasured, HX-B6). So every path walk without dircache (each track open, each `check_dir` in a scan, each entry of the post-USB deleted-file pass, each art-builder lookup) pays roughly 16 directory entries per command. On a 1 TB library a top-level folder of 3–4k artists with long names is ~1,000 sectors, about **1–3 s for a cold lookup near its end** [est]. This is the biggest generic-code cost left on the storage side, and nothing else touches it.
2. **The audio buffer is still a buflib block whose shrink stops playback** (`playback.c:1054`, upstream's own `TODO: Do it without stopping playback`). The shell is built around avoiding it: every reservation at shell start, a second drift slot taken only at start, the single view-pool block, deferrals while paused, and the rule "an allocation while paused restarts the queue". This is now the largest **simplicity** lever, and it also decides how Cover Flow textures, modules and Extras can allocate.
3. **The library update's peak is the RAM lever, not `_end`.** (Owner chose the audio-buffer floor, option 1: the audio file buffer stays 15,079,816 B on 32 MB during the update, ~7.6 MB above the floor; the commit peak below decides whether an update finishes without waiting for a stop.) Measured in the sim at 50k tracks with both snapshots: peak 13,895,312 B, minimum free core heap **8,269,688 B on 32 MB**, which is **~0.77 MB above the 7.5 MB floor**. Audit A's steady-state margin (+13–15 MB on 32 MB) does not include the commit peak; audit C's "~0.8 MB" does. The tagcache RAM layout (~140 B/track, 96 of which is the index) is therefore the RAM lever that matters, ahead of any static.
4. **Library playback still uses Rockbox's path-based playlist.** `lib_play` and Shuffle Songs read up to 2,000 file names from the tagcache's file tag and write a text control file. The 2,000 cap exists because of it; upstream notes ~10 s for 2,000 entries on a PP5020 (`library.c:6407-6410`).
5. **Rockbox's stock UI is still resident in the release build** (§4): 12,910 B of image and 5,998 B of bss [measured, `reach.py --cut`], kept alive by three keystones in `apps/main.c` (`list_init`, `gui_syncstatusbar_init` through the `screens[]` vtable that pins `put_line`/`print_line`/icons/scroll engine, and `viewportmanager_init`), plus `gui_usb_screen_run` → `playlist_resume` → `action_userabort`.
6. **Settings persistence is Rockbox's text `.cfg` with temp-file plus rename** (`settings.c:605-720`). Each save rewrites the whole changed set line by line through `fdprintf` and does create / remove / rename / rename in `.rockbox`. The file starts with `# .cfg file created by rockbox` (a Rockbox string on disk; it is never shown, but it is there). `ab_store.c` already has the fitted pattern: one sector updated in place, no allocation, written while the disk is awake.
7. **The "a lit screen holds the PLL" rule predates E2.** The hold exists because a CPU push is 20 % slower at 24 MHz. Full-width pushes now go by DMA, so the rule can be re-measured (R10).
8. **Wake-ups nobody measured.** The buffering thread wakes every HZ/2 whenever handles exist, even when the buffer is full (`buffering.c:1639`). The power thread wakes at 2 Hz (`POWER_THREAD_STEP_TICKS`), and the battery ADC reads over I2C every 0.4 s (`adc-ipod-pcf.c`). M0 counts CPU time but not wakes per thread.

---

### Ranking by value (2026-10-20)

Value weights battery, responsiveness, RAM-floor margin and simplicity, discounted by risk and by unproven gains.

| Rank | ID | What | Owner value | Gain (tag) | Risk | Effort | Phase | BCM? |
|---|---|---|---|---|---|---|---|---|
| 1 | M1 | Measurement session: inline USB-C meter (per-state mA), HX-B6 latency L, IDE IRQ (DP-4), per-thread wake counter, codec malloc high-water | enables every number below | n/a | low | S + 1 session | P17/P18 | no |
| 2 | R1 | Directory reads in bursts (`fat.c` dir scan) | responsiveness (opens, scans, deleted pass), fewer wakes | ÷8–32 commands per directory [est] | low–med | M | P17 (P16 benefits) | no |
| 3 | R5 | Late pool, or a shrink without a stop, for the audio buffer | simplicity, responsiveness (no 340 ms gaps), lazy reservations | removes a bug class; up to ~1 MB of lazy reservations [est] | med | M (a) / L (b) | P16 | no |
| 4 | R6 | Fit the RAM library (fields, packed index, no second full snapshot) | updates that finish without waiting for a stop on 32 MB units (free-heap margin ~0.77 MB at the commit peak; the audio-buffer floor itself holds) | −2.4 to −7 MB peak at 50k [est] | high | M→XL | P16 | no |
| 5 | R4 | Queue by database index instead of path playlists | responsiveness (play/shuffle start), no 2,000 cap, instant resume | seconds → ms [est] | med–high | L | P16 | no |
| 6 | RUI-1..3 | Retire the Rockbox UI behind ProkBox's developer UI | simplicity, native illusion | release 18.9 KB, dev ~60 KB [measured cut] | low→med | S→L | P9b step | no |
| 7 | R3 | Last-parent directory cache | responsiveness (next-track open) | depth→1 scans per open [est] | low | S | P17 | no |
| 8 | R10 | 24 MHz with a lit screen now that E2 pushes | battery during screen-on use | unknown until M1 | low–med | S + session | P18 | no |
| 9 | A3 | Adapter power-off after long idle (runtime, solid drives) | battery (stopped and dark playback) | unknown until M1 [hyp: several %] | med | S + session | P18 | no |
| 10 | R2 | Open by stored FAT location; deleted-file pass by location | responsiveness (open = 1 sector), post-USB pass minutes → tens of s [est] | | med | L | P16 | no |
| 11 | R8 / RUI-4 | Binary A/B settings and resume record, own settings table | crash safety, fewer writes, −12–16 KB | | med | M | P16/P18 | no |
| 12 | L2 | Our BCM image as the default display driver → C8 wake, #76 second frame, strip framebuffer, textures | responsiveness (wake 500→30 ms), floor (−130 KB FB, −512 KB textures) [hyp] | | high | XL | P19 | **yes** |
| 13 | K1 | Deadline waits for buffering, power thread, ADC | battery (small), hygiene | ~4–5 → ~0.5 wakes/s [est] | low | S | P18 | no |
| 14 | R13 | Art builder decode on the COP | responsiveness during builds, scan wall time | −30–45 % build time [est] | med | L | P19 | no |
| 15 | F5 | 64 KiB clusters (migration plan) | USB mount time (~12 → ~3 s), FS metadata | [est from arithmetic] | high (data) | M + session | P17 | no |
| 16 | R14 | RGB565 × 4-bit alpha fast path | fewer boosted frames | 2–3× text draw [est] | low–med | M | P19 | no |
| 17 | D2 | Codec slot right-size (measure peaks) | floor | 100–300 KB [hyp] | low | S+S | P16 → P9b | no |
| 18 | D1 | Compile out unused DSP stages (keep EQ, tdspeed, resample) | size, IRAM | ~10.1 KB + 264 B IRAM [measured] | low | S | P9b | no |
| 19 | B1 | Chapter-aligned fills for spoken tracks | fewer reloads in books | | low | M | P18 | no |
| 20 | B2 | Watermark, prefill and boost chosen at runtime from the drive | one build (rule 7) | | low | S | P17 | no |
| 21 | K2 | Merge housekeeping threads (scroll goes with RUI-1) | simplicity, ~5.5 KB | | low–med | S–M | P9b | no |
| 22 | C9 | BCM decodes the video soundtrack | video CPU | | med–high | L | P14/P19 | **yes** |
| 23 | C10 | BCM compositor (after L2) | animation CPU, PP framebuffer | | high | XL | P19 | **yes** |
| 24 | F4, audit A/B statics | `MAX_OPEN_DIRS`, bidi buffers, load-on-demand rodata | small floor bytes | 3–28 KB | low–med | S each | P9b | no |

Not recommended now: a tickless kernel (K3); DVFS (PMU unknown); UDMA4 (D3); PP general-DMA into the port (D1: use E2); replacing buffering wholesale; removing diagnostic strings.

#### Quick-win bundle (about a week, one mini-phase, guarded `HAVE_IPOD_UI` edits, ledger rows)
M1's firmware parts (wake counter, codec high-water log), R3, K1, D1, B2, RUI-0 and RUI-1. Then one owner session covers M1 (meter, HX-B6, DP-4), R10 and A3 as developer A/B switches.

#### Medium (2–4 weeks)
R1 (needs A1's L to size the burst), R5(a), R8 with RUI-4, RUI-2/3, B1.

#### Large, each with its own brief and review
R6, R4, R2, R13, F5 (with the owner's backup), L2 and the BCM chain (BCM session).

---

### RUI: retire the Rockbox UI (P9b, #103)

Owner 2026-10-20: the stock Rockbox UI is retired entirely, and ProkBox gets its own developer UI in a black-and-white look like the iPod's disk-mode and diagnostic screens (simple, clean lines, easy to render and read), built as modules (`docs/design/module-loader.md`).

| Stage | What | Resident saving | Risk | Effort |
|---|---|---|---|---|
| RUI-0 | Fix `sizemap.py`'s string-pool attribution; add `reach.py --cut` for these sets to the size report | 0 (accurate numbers) | none | S |
| RUI-1 | **Break the release keystones**: no `gui_syncstatusbar_init`, `viewportmanager_init` or `list_init` under `HAVE_IPOD_UI && !DEVMODE`; slim the `screens[]` vtable (no `put_line`); the shell's USB entry calls `ui_usb_screen_loop` directly; `playlist_resume`'s abort through a shell hook | **~18.9 KB release** (12,910 image + 5,998 bss), the scroll thread gone | low–med (boot order, the USB entry rule "no disk writes at entry") | S–M |
| RUI-2 | **ProkBox developer UI kit**: three primitives drawn by the shell's own list renderer (`devui_list`, `devui_value`, `devui_confirm`, ~3–5 KB [est]), log-style info pages; port `devmode.c` rows first | −3 to −5 KB (cost) | low | M |
| RUI-3 | Rewrite the stock debug screens the owner uses (stacks, buffer, battery, disk/IDENTIFY, USB stats, BCM device test, udmatest) on the kit; delete `debug_menu`, `menu`, `option_select`, `gui/list`, `bitmap/list`, `scrollbar`, the stock splash body. Optionally as the first shell module (audit C) | **~55–60 KB developer build** (60,225 − kit) | med (feature parity of the developer tools) | M–L |
| RUI-4 | **Own settings table** (R8): only the settings ProkBox reads, binary A/B storage, a one-time import; drop `settings_list` and the `.cfg` writer; the developer UI shows raw values | **~12–16 KB** both builds [est] + ~3.8 KB lang | med (migration) | M |
| RUI-5 | Drop `action.o`, the keymaps and `core_keymap` (backlight-on-input moves to the shell's input path); prune lang entries only stock code used (keep the language backend, rule 8) | **~3.8 KB** + up to ~4.6 KB lang | low–med | S–M |

**Totals [measured upper bound for the cut sets, est for replacements]:** release **~35–39 KB** resident (≈ 39,001 cut minus the small replacements); developer build **~75–80 KB**. On a 32 MB unit that is ~5 % of the commit-peak floor margin. The bigger gain is simplicity: one UI toolkit, one input path, no Rockbox strings left to police, and the developer UI becomes movable into a module.

Device-reality notes: RUI-1 changes boot order (the boot logo stays up; check the boot and USB entry on the device); RUI-3's screens need a device pass of each developer tool; RUI-4 needs a power-cut test and an upgrade-in-place test from an existing `config.cfg`.

### Device measurements (M1 and per item)

1. **Meter session (M1):** battery full, charger current deltas: screen lit vs off; 24 vs 30 vs 80 MHz unlit; 24 vs 30 MHz lit (R10); adapter active / standby / powered off (A3); BCM on vs cut. Feeds `BC_BUSY_W` and every battery claim.
2. **HX-B6:** the same 64 MB read as 1-, 8-, 64- and 256-sector commands at UDMA2: gives L (R1, R2, F5) and settles DP-5.
3. **DP-4:** one playback; read `m0 disk sleeps=… irq=I`.
4. **Wakes per thread** (new counter): 30 min dark playback before and after K1.
5. **Scan and open timings:** `db scan ms=`, `disk_cmds` per scan, and a cold-open time for a track in a large folder (R1, R3, R2).
6. **Codec peaks:** malloc and TLSF high-water per codec on the worst files (D2).
7. **Floor trace:** buffer bytes after shell start and at the commit peak, 50k library, 32 and 64 MB (R5, R6).
8. **Play-all start:** `m0 tap2pcm why=play` for a 2,000-row Shuffle Songs (R4).
9. **RUI:** a boot with the keystones removed (logo stays, USB entry has no disk writes); each rewritten developer tool once.
