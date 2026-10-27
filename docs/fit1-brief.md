# Fit-1 brief (tracker #71)

*Phase Fit-1 takes the quick and medium wins from `docs/hardware-fit-audit.md`. Section A (storage, buffering, measurement) belongs to fit1-a; this file's section B belongs to fit1-b (branch `fit1-b` from `s12`).*

## B. Codec priority (C2) and PCM prefill (C3)

### B.1 Goal

- **Audio never underruns**, and the UI, buffering and the **video feed thread** are never starved for hundreds of ms. The device logged S26 (no video posts for 420–520 ms while pcmbuf held the codec at 10–14) and S31 (the feed 2.1 s late).
- **Playback starts sooner.** Songs and videos should start, skip and seek 50–300 ms faster.
- Stock builds stay unchanged. The HDD configuration keeps upstream's 1 s prefill.

### B.2 What causes the starvation

Rockbox's scheduler is cooperative: a thread runs until it yields or blocks. The codec yields once per frame (`codec_get_command_callback`), so one frame's decode is one slice, 8–20 ms depending on the codec and the clock.

1. **pcmbuf's ladder** (`apps/pcmbuf.c`, `boost_codec_thread`). The codec moves between priority 5 and 14 whenever the PCM ring is under 70 % of 3 s, which covers every start, seek and skip.
2. **Aging by switch count** (`firmware/kernel/thread.c`, `++skip_count > diff*diff`). A runnable thread `diff` levels below the top runs only after `diff²` switches. With the codec at 10 and the feed at 16 (a gap of 6), that is 37 switches, or 21 after the wake bonus. At 20 ms per codec slice, that is about 420 ms.

`tests/sched` models `switch_thread()` exactly and reproduces S26: a 424 ms wait with 20 ms slices, and 172 ms with 8 ms slices.

### B.3 The policy: fixed bands plus time-bounded aging

The video player's owner asked for this: no dynamic boosts, and a named slot for the feed thread.

| Band | Priority | Threads |
|---|---|---|
| realtime | 4 | piezo (unchanged; never aged past) |
| voice | 12 | voice (unchanged, `PRIORITY_VOICE`) |
| **codec** | **13** `PRIORITY_CODEC` | codec, fixed; pcmbuf no longer changes it |
| **video feed** | **14** `PRIORITY_VIDEO_FEED` | the video player's feed thread (for the video owner to adopt) |
| buffering, audio | 15 | unchanged |
| UI | 16 | main/UI, storage, backlight, scroll (unchanged) |
| system | 18 | usb, power |
| background | 20 | tagcache, artcache |

**Aging rule** (`firmware/kernel/include/sched_age.h`, a pure function used by `switch_thread()`):
- Each thread records `ready_tick`, the tick it last ran or became runnable.
- A runnable thread below the top runs when **either** condition holds:
  - upstream's count rule (`skips > gap²`), kept as a fast path for higher threads that yield often, such as polled ATA DMA;
  - **it has waited `sched_age_limit(p)` ticks**: 2 ticks (20 ms) for p ≤ 16, whatever the gap, or 2·(p − 15) ticks for background threads (60 ms at 18, 100 ms at 20).
- Realtime threads are never aged past.

**The guarantee.** A runnable thread at priority p waits at most `sched_age_limit(p)` plus 1 tick, plus one slice from each thread ahead of it.

**Why audio still wins.**
- The codec is the highest non-realtime band, so it takes the core whenever it is runnable.
- Each lower thread gets at most one slice per aging period.
- The refill is still CPU-boosted below pcmbuf's 2 s watermark (unchanged).
- The model's busiest case gives the codec ≥ 60 % of the core: a 60 fps UI using 23 %, the feed, buffering, and a database build that never blocks. MP3 at 80 MHz needs roughly 15 % [estimate; M0 will measure].

**Rejected options.**
- *Hysteresis boost (stay at 16, step up below 0.25–0.5 s).* It is dynamic, which the video side asked to avoid, and it still needs time-bounded aging to protect the feed during the boost. Fixed bands with the time bound give the same protection with one less moving part.
- *Deadline scheduling.* It needs a per-thread deadline API across upstream code. That cost is not justified while one rule bounds every wait.

**What the video side can delete** once the feed thread is created at `PRIORITY_VIDEO_FEED`:
- `VPLAYER_FOLLOW_US`, `VPLAYER_PRIO_TOP` and `VPLAYER_PRIO_FLOOR` (`apps/video/vplayer.h`);
- the per-loop follow block in `vplayer.c` (`codec_thread_get_priority()`, `follow_from`, the `thread_set_priority` calls) and the `VPLAYER_TUNE_FLOOR` tuning bit;
- the "watch: thread priority" log line.

The trace's `codec_prio` column will always read 13, so it can go too. With the shorter prefill (C3), the start/resume `elapsed` leap shrinks from about 430 ms to about 100 ms (0.25 s of PCM, minus what has already played). The slew (`VPLAYER_SLEW`) can stay as a safety net. Until the video side changes, its current code keeps working: it follows the codec to 13 for 3 s, then sits at its floor of 14.

### B.4 Prefill: 1 s → 0.25 s on flash (C3)

`LOW_DATA` is pcmbuf's only start watermark (`pcmbuf_data_critical()` is called only before playback starts). With `HAVE_IPOD_UI && HAVE_FLASH_STORAGE`, `PCMBUF_PREFILL_QSECS` is 1 (0.25 s). Otherwise it is 4 (1 s, upstream), so HDD units and stock builds are unchanged.

**Why 0.25 s survives, M0-style:**
- **Time saved.** Starting sooner saves the time it takes to decode 0.75 s at R× real time boosted: 75–190 ms for R = 4–10 (MP3, AAC), and more for heavy codecs. R is unmeasured until M0.
- **After the start.** The PCM grows at (R·share − 1) per second, with share ≥ 0.6 from the policy above. The 0.25 s buffer only empties if the codec makes **no progress for 250 ms**. Possible causes:
  - *CPU:* ruled out by B.3. Each lower thread takes at most one slice per 20 ms.
  - *Reads:* buffering reads 32 KB chunks back to back. UDMA2 moves 32 KB in about 1.5 ms, and the SD/CF read-access limit is ≤ 100 ms per command, which is less than 250 ms.
  - *A write in flight:* an SD-based adapter may hold busy for up to 250 ms (SDHC) or 500 ms (SDXC) on a write. A shell write (log or resume) landing in the first ~100 ms after a cold start, of lossless audio whose file buffer holds only a chunk or two, is the one case that can underrun. With 1 s, that case delays the start instead.
- **Residual risk.** A skip into an already-buffered track has no storage risk. The open risk is that rare coincidence on cold starts. If the device logs any `pcm underrun` at a start, raise `PCMBUF_PREFILL_QSECS` to 2 (0.5 s), which is one line.

### B.5 Files

| File | Change | Guard |
|---|---|---|
| `firmware/kernel/include/sched_age.h` (new) | pure aging rule | used under `HAVE_SCHED_BANDS` |
| `firmware/kernel/thread.c`, `thread-internal.h` | `ready_tick`; uses `sched_may_run()` | `HAVE_SCHED_BANDS` |
| `firmware/kernel/include/thread.h` | `PRIORITY_CODEC`, `PRIORITY_VIDEO_FEED` | `HAVE_SCHED_BANDS` |
| `firmware/export/config/ipodvideo.h` | defines `HAVE_SCHED_BANDS` | `HAVE_IPOD_UI` |
| `apps/codec_thread.c` | codec created at `PRIORITY_CODEC` | `HAVE_SCHED_BANDS` |
| `apps/pcmbuf.c` | ladder compiled out; prefill knob; `pcm start` / `pcm underrun` log lines | `HAVE_SCHED_BANDS`; `HAVE_IPOD_UI` (+ `HAVE_FLASH_STORAGE`) |
| `tests/sched/` (new) | rule cases and the scheduler model | host |

No changes to `apps/video/`, `buffering.c` or `ata.c`.

### B.6 Device-reality checklist

- **Compiled features:** the shell builds on the iPod Video only. Stock `fw` and the bootloader are unaffected (the flag is never defined there).
- **Memory:**
  - +4 B per thread slot (`ready_tick`).
  - +8 B of statics for the log state, plus the underrun counter outside video builds.
  - No new buffers, and no stack growth beyond a few words.
- **CPU:**
  - One subtraction and compare per skipped thread per switch.
  - The pcmbuf ladder is gone: no more `thread_set_priority` calls during refills.
  - `cpu_boost` is untouched.
- **Storage wake-ups:** none added. Log lines go to the RAM ring and flush with the existing storage-idle callback.
- **Threads and locks:** the rule runs inside `switch_thread()` with IRQs off, as before. It reads only `current_tick`.
- **Splashes and strings:** none. The log tags are `pcm`.
- **Simulator:** SDL threads don't use `kernel/thread.c`, so the scheduler change is covered only by the host model. Device timing is listed below.

### B.7 Risks

1. **UI during steady playback.** The codec at 13 sits above the UI, so the UI can wait for one codec burst, bounded at about 20 ms. Upstream's equal priority bounded it by one frame. The worst case is heavy codecs at 30 MHz.
2. **Buffering below the codec.** File-buffer fills during a refill get a slice every 20 ms, and more through the count rule. The file buffer holds minutes of audio, and a codec waiting for data blocks, which lets buffering run.
3. **Background threads gain slightly.** A database build gets a slice at least every 100 ms during a refill. The model gives it 8.5–9 %.
4. **Count rule kept.** Aging is not *replaced*: the time bound is added to it. Removing the count rule would let a polling higher thread hold the core for 20 ms.

### B.8 Measurement and device-check plan

- **Host:** `tests/sched/run.sh` covers the rule cases, the S26 replay under upstream's rule, and the band bounds and codec share.
- **Device** (`ipodui.log`, every start, skip and seek):
  - `pcm start prefill_ms=<decode time of the prefill> level_ms=<PCM at start>`. Expect 25–80 ms for MP3/AAC, where 1 s would have taken about 100–250 ms; compare one boot with the knob at 4.
  - `pcm underrun n=<total> level_ms=<PCM at detection>`. Expect none.
- **Device, video build:**
  - devbcm stage 7: underruns stay 0.
  - `player: clock …; codec priority 13, longest loop gap G ms`: expect G ≤ ~40 ms.
  - No `late` lines (over 100 ms past the planned wait).
  - Rerun the S34 matrix with the player's floor and follow removed.
- **UI:** `lib window … total_ms` p90 (now 360 ms) and `np art read slow` while playing. Clickwheel scrolling should feel no worse during MP3 and FLAC at 30 MHz.
- **Stress:** cold-start, skip and seek repeatedly with CD FLAC and one heavy codec (APE `-c4000` or HE-AAC), during a database update and right after a USB eject, when log flushes and writes land.

# Phase Fit-1 brief (tracker #71)

Quick wins from `docs/hardware-fit-audit.md`, split over two agents. Section A is the platform half (measurement, storage idle, buffering I/O); section B, the scheduling half (C2/C3), is written by its own agent.

## A. Measurement kit, storage idle, fill I/O (M0, C1, C4a, C12a)

Branch `fit1-a` from `s12`. Read list: `docs/hardware-fit-audit.md` (M0, C1, C4, C12), `docs/audit/hw-fit-evidence.md` (E1), `docs/pp5022-reference/storage.md`.

### A.1 Plan

| Item | Change | Files |
|---|---|---|
| M0 kit | `devstats`: per-thread CPU µs and per-core idle µs, sampled at `switch_thread()` from `USEC_TIMER` (target only); per-thread disk commands, sectors and PIO fallbacks plus a 16-entry ring of disk **wakes** (who woke the disk, read or write, how many sectors), sleep and sleep-failure counts; a tap-to-first-PCM stamp (play, skip, seek or resume request → `pcm_play_data()`/unpause). Compiled only in developer builds (`HAVE_DEVSTATS` ⇐ `HAVE_IPOD_UI_DEVMODE`): stock, release and the bootloader have none of it. | `firmware/export/devstats.h`, `firmware/common/devstats.c`, hooks in `firmware/kernel/thread.c`, `firmware/drivers/ata.c`, `firmware/pcm.c`, `apps/playback.c`, `uisimulator/common/filesystem-sim.c` (the sim's disk model) |
| M0 output | `power.csv` gains columns **after** `mark` (old column numbers stay valid): `idle_pct`, `cop_idle_pct`, `cpu1`, `cpu1_pct`, `cpu2`, `cpu2_pct`, `disk_cmds`, `disk_kb`, `disk_wakes`, `disk_by`. `ipodui.log` lines: `m0 tap2pcm ms=… why=…`, `m0 disk wake by=… rw=… n=… play=…` (stopped or paused only, rate-limited), `m0 disk sleep fail=…`, `db scan ms=…`/`db commit ms=…` (calibration). All RAM until the existing storage-idle flush. | `apps/ipodui/powerlog*.{c,h}` (the M0 lines too), `apps/tagcache.c` (two guarded log calls) |
| C1 | Find and remove what keeps the flash awake while stopped (sim attribution + the code + E1), then a **1 s** idle hold on flash (`HAVE_FLASH_STORAGE` with `HAVE_IPOD_UI`); hdd keeps 5 s and its spin-down setting. | `firmware/drivers/ata.c`, the cause's file |
| C4a | No `trigger_cpu_boost()` in `buffer_handle()` on flash (`HAVE_IPOD_UI && !HAVE_DISK_STORAGE`); hdd keeps it. | `apps/buffering.c` |
| C4b | ATA DMA completion by the IDE interrupt (source 23): the waiting thread sleeps on a semaphore; 2-tick slices re-read `IDE0_CFG` bit 3, the 10 s timeout stays, three transfers in a row without the interrupt switch back to polling. Every shell build (hdd too: same controller); PIO is untouched. | `firmware/target/arm/pp/ata-pp5020.c`, `system-pp502x.c` |
| C7 | A forced UDMA4 on the flash adapter as a guarded developer experiment (A.6). | `firmware/drivers/ata.c`, `ata.h`, `ata-pp5020.c`, `apps/ipodui/udmatest.{c,h}`, `devmode.c` |
| C12a | Background fills read **256 KB** per `read()` (urgent reads for a waiting codec stay 32 KB, so start and seek latency don't grow); the FAT layer's per-command cap becomes a byte cap of 256 KB (was 256 FAT sectors), so one fill read is one ATA command when the clusters are contiguous. | `apps/buffering.c`, `firmware/common/fat.c`, `firmware/export/config/ipodvideo.h` |

### A.2 Risks

- **DMA length.** A 256 KB read is one UDMA command of 512 ATA sectors. `IDE_DMA_LENGTH` is a 32-bit register and upstream already issues 128 KB (512 B FAT sectors) or 512 KB (2048 B sectors) reads on this controller during the tagcache RAM-cache load, so this is in range, but it is unproven on this adapter: `m0` logs PIO fallbacks and soft resets per thread, and the owner session checks them. Fallback: lower `FAT_MAX_TRANSFER_BYTES`.
- **1 s idle hold.** More standby/wake cycles on flash: each wake is a soft reset (~20 ms). Worth it only if wakes stay rare; the wake ring measures it.
- **No fill boost.** Metadata parsing for the next track runs at 30 MHz; the audit's E1 already shows unboosted dark playback in 94 % of rows.
- **thread.c.** The C2 agent edits the aging rule in the same function; my hooks sit in other hunks (entry and the idle loop).
- **C4b interrupt routing.** Source 23 has never been used on silicon (`storage.md` §3). If it doesn't fire, each transfer waits up to a 20 ms slice, three such transfers switch the driver back to polling (logged), so the worst case is three slow transfers.
- **C7 data safety.** Forced UDMA4 may corrupt transfers. It is off by default, needs two presses, writes go by PIO while it is forced (a bad transfer can spoil a read, never the flash), and the first retry or test mismatch drops it and removes its config file.
- **Static RAM.** `power.csv`'s ring grows from 4,044 B to 6,348 B (rows 20 → 32 B, 192 rows kept so an A/B half stays free of forced writes); `devstats` adds about 0.5 KB. Developer builds only; the release build has neither.

### A.3 Measurement

- Before/after on `fw-ui` (`arm-none-eabi-size`), `fw-rel`, `fw-alt`; stock `fw` must stay byte-identical.
- Sim: the attribution over a 60 s stopped, dark idle (who touches storage), and the reads per MB of a fill (`read()` calls and bytes).
- Device (owner session, below): `disk_pct` stopped and dark-playing, wakes per hour stopped, ATA commands per MB, tap-to-PCM, `idle_pct`.

### A.4 Device-reality checklist

1. Compiled features: `HAVE_DEVSTATS` only in developer builds (`fw-ui`, sims); the 1 s hold, no-boost and 256 KB reads in every `--ipodui` flash build; hdd builds keep upstream's timings and boost. The sim has no ATA: its disk model counts accesses and models the hold.
2. Memory: no allocation; reads land in the existing ring.
3. CPU: about 20 cycles per thread switch in developer builds.
4. Storage: the point of C1; C12a cuts commands per MB by up to 8×.
5–7. No LCD, lock or splash change. Hooks never yield and hold no lock beyond an IRQ mask around a few words.
8. Persistence: none new (logs ride the existing flush).
9. Scale: none.
11. Stock unchanged: every edit is under `HAVE_IPOD_UI` or `HAVE_DEVSTATS`.
12. Existing paths: `ata_get_xfer_stats()` (USB-session deltas) stays; `devstats` is per thread and session-wide.

### A.6 C7: the UDMA4 experiment

- **Switch:** Developer › "UDMA4 experiment: off". SELECT twice within 5 s turns it on (`/.rockbox/udma4.cfg`; the next boots keep it); one SELECT turns it off. The adapter must list UDMA4 in IDENTIFY word 88 (ours: `w88=043f`, UDMA0-5).
- **While on:** reads by UDMA4 (80 MHz per transfer, as upstream does above UDMA2), writes by PIO. The first command that needs a retry drops to UDMA2 before the retry, the config file goes, `udma fell back resets=R` is logged.
- **Test:** Developer › the row under it, SELECT twice, playback stopped, about a minute: writes `/.rockbox/udmatest.bin` (64 MB, word *w* = *w*·K + C), reads it once at UDMA2 (checks the write, gives the "before" MB/s) and 8 times at UDMA4, comparing every word, then deletes it. The row then reads `UDMA4 test: pass, A -> B MB/s` or `FAIL pass N bad=W` (a failure turns the switch off). `ipodui.log`: `udma test rc= bad= mbs2= mbs4= resets=`. Music is never read or written.
- **Measure:** MB/s from the test (read time only), USB copy rates (`usb ms`/`usb exit` lines) with the switch off and on, and the CPU during a copy from `power.csv` (`idle_pct`, `cpu1`).

### A.7 Measured (simulator and builds)

- **C12a:** a 30 MB WAV buffered in the simulator: buffering's `read()` calls 967 → 127 (31.5 → 4.1 per MB). On the device one call is one ATA command when the clusters are contiguous.
- **C1:** a 120 s stopped, dark simulator idle after boot shows no periodic storage access from the shell (the only writer is the simulator's own `sim` thread, every 30 s). The 5 s toucher behind E1 is therefore device-only or state-dependent; the device session's `m0 disk wake` lines name it. The 1 s hold and the attribution ship now.
- Sizes: see the hand-back report (the orchestrator updates the budget table).

### A.8 Session 36 follow-ups (C1)

- **Stopped, the flash never slept** (`disk_pct` 100, `disk_by` tagcache, 812 commands of 512 B per 30 s, for hours): the tagcache thread retried a delayed commit every second after its temp file was gone (a scan cut by a power-off left a torn temp file; the retry removed it as "incorrect tmpheader" but stayed due). Each retry is two failed opens in `/.rockbox`, i.e. a scan of that directory's sectors: ~28 single-sector reads a second. Every nothing-to-commit return now clears the flag. The simulator never showed it because its disk model didn't count failed opens; now it does (a planted torn temp file: 57 failed opens in 30 s before, none after).
- **Playing, a wake per track change** (`m0 disk wake by=audio rd n=1`, ~150 single-sector commands): the runtime statistics looked the new track up at its track change (`tagtree_buffer_event` → `find_index` → a walk of the filename tag file from its start, sector by sector), long after the track was buffered and the disk had gone to standby. The audio thread now looks a track ahead up at load time, while the disk is awake for that file, into a 4-slot memo per database generation; the walk keeps its position within a generation, so album neighbours cost a sector or two. Sim: the lookups all happen at load time, none at the track changes.
- Left as they are: `by=main` reads around skips and resumes with the screen lit (Now Playing art); the next track's own load (one wake while the file buffer refills).
- **Log retention:** `ipodui.log` now keeps the last five boots (`.1` to `.5`, 16 KB each before a mid-boot roll) and starts each file with `boot build=<id> rtc=<date time>`. A half-full ring is written by the UI thread whenever the disk is awake anyway and audio is stopped (a disk that never sleeps never ran the idle flush, so S36's 2 h stopped boot logged nothing); never during playback.

### A.5 Owner's device session

1. Install the `fw-ui` build (developer mode), Developer › Power log every 5 s, Power log on.
2. **Stopped, dark, 10 min:** stop playback, lock Hold, screen off. Expect `disk_pct` mostly 0 and `m0 disk wake` lines naming nothing or the expected writer (power log flush at most every 5 min).
3. **Dark playback, 10 min** of one album: expect `boost_pct` ≈ 0 during fills, `disk_cmds`/`disk_kb` giving about 4 commands per MB (was ~32), no `pio` and no soft resets in `m0 disk` lines.
4. **Tap to PCM:** from the Songs list, start 5 tracks, skip 5 times, seek twice, pause/resume twice; read the `m0 tap2pcm` lines.
5. **Calibration (M0):** with a 50k library copied over (`tests/perf/lib50k.sh` output), Settings › Reset Library; the `db scan ms=` and `db commit ms=` lines over the sim's numbers give the factor.
6. **USB-C meter (optional):** an inline USB-C meter on a charged unit (charger draw included, so read the delta between states): 2 min each of screen on/off, 30/80 MHz (Developer › boost), disk active (copy) and standby, BCM on/off; note the mA per state next to `power.csv`'s marks.
7. **C4b:** after any playback, an `m0 disk sleeps=… irq=I` line with I > 0 and no `polling` means the interrupt works; compare `idle_pct` during a fill and a USB copy with a build before (expect the CPU core idle through DMA).
8. **C7 (optional, after a backup of anything not on the computer):** Developer › UDMA4 test (SELECT twice). On `pass`, try the switch on for a USB copy each way and a day of use; on any `FAIL` or `udma fell back` line, leave it off and send the log.
9. Copy `.rockbox/power.csv` and `ipodui.log` off over USB.
*Sections are written by each item's agent on its own branch and merged by the orchestrator.*

## C. 24 MHz idle with the PLL off (audit item C6, branch `fit1-c`)

### Goal
When nothing needs more, run the PP5022 from its 24 MHz crystal with the PLL powered down (`CPUFREQ_DEFAULT`), instead of the 30 MHz PLL setting that upstream uses for every unboosted moment (`CPUFREQ_NORMAL`). Boosts still go to 80 MHz from the PLL, and they wait for the lock.

### What the clock touches (evidence in `docs/pp5022-reference/clocks-power.md` §8)
| Block | At 24 MHz with the PLL off | Handling |
|---|---|---|
| Tick, `udelay`, user timer | Timers count a fixed 1 MHz (`TIMER_FREQ`, `USEC_TIMER`), whatever the core clock is | nothing to rescale |
| I2S and audio | The WM8758 is the I2S clock master (`CLKCTRL_MS`). It gets its MCLK from the PP's external device clock, set to 24 MHz from the crystal (`wmcodec-pp.c`) | no PLL dependency |
| ATA | Upstream rates UDMA2 at 30 MHz and UDMA1 at 24 MHz (`ata-target.h`) | the PLL is held for each DMA transfer (`ata-pp5020.c`). PIO stays at 24 MHz, which the bootloader already does |
| USB | The ARC controller's clocking is not documented | the PLL is held while the controller is enabled (`usb_enable()`) |
| Clickwheel serial, I2C | Never reconfigured across upstream's 30/80 MHz switches, and both work in the bootloader at 24 MHz | nothing |
| LCD / BCM host port | About 10.5 PP cycles per 16-bit transfer, so 20 % slower | a lit screen holds the PLL |

### Policy
- **Firmware** (`firmware/system.c`): a hold mask (`cpu_pll_hold()`). Unboosted, the CPU runs at 24 MHz only when the mask is empty. Each holder owns one bit:
  - `DEV`: the developer switch is off, and the bit is also set from boot until the shell starts the policy;
  - `LIT`: the backlight is on;
  - `LOAD`: PCM is playing a track that doesn't fit;
  - `ATA`: one DMA transfer;
  - `USB`: the controller is enabled.
- **Sampling** (`firmware/pll_idle.c`): the power thread's existing 2 Hz step samples `LIT` and `LOAD`, so the policy adds no wake-ups.
- **Shell** (`apps/ipodui/clockpol.c`, logic in `clockpol_core.c`, host-tested):
  - **What fits at 24 MHz:**
    - the codec is in the light class: MP3/MP2/MP1 (synthesis on the COP), WAV/AIFF/W64/AU, FLAC, WavPack or ALAC;
    - the sample rate is at most 48 kHz;
    - speed and pitch are both at 1×;
    - there is no software EQ, crossfeed or surround.
  - **Everything else plays at 30 MHz** (AAC, HE-AAC, Vorbis, Opus, WMA, APE and others).
  - **Adaptive check:** each minute played at 24 MHz is judged. More than 4 rescues per minute (the playing PCM falling under pcmbuf's boost watermark after it had reached it; S36: counting every boost from 24 MHz struck FLAC for a library scan's transfers), or any PCM underrun (counted only in video builds), puts that format at 30 MHz for the rest of the session.
- **Developer switch:** the row "24 MHz idle: on/off" in developer mode. It is not saved, so it is on again at the next boot. A release build has no switch, and the policy is on there.
- **Logs:**
  - `ipodui.log`: two `clk` lines (ui_log keeps 62 characters per line), written every 5 min while the counters move, and again when the switch changes or a format is struck:
    - `clk <why> rtc=HH:MM:SS idle=N% ent=N bfi=N`: the RTC time (compare it with the line's tick seconds to check the tick), the share of time since boot spent at 24 MHz, the number of switches down to 24 MHz, and the number of boosts that started from 24 MHz;
    - `clk lock n=N us=N max=N holds=X load=N fmt=N`: the PLL power-ups that waited for the lock, the last and the longest lock time in µs, the holds, and the current track's need and format;
  - `power.csv` marks: `clk on`, `clk off`, `clk strike N`. No new column, so it merges cleanly with fit1-a's columns. fit1-a can add an `idle24_pct` column later from `cpu_frequency == CPUFREQ_DEFAULT`.

### Config dimensions
`HAVE_PLL_IDLE` is defined in `ipodvideo.h` under `HAVE_IPOD_UI` (not in the simulator or the bootloader). That covers every iPod Video variant: flash and HDD, 32 and 64 MB.
- **HDD:** its writes use PIO (DMA writes are only for SSDs). PIO at 24 MHz is what the bootloader runs.
- **5G (PP5021C):** Rockbox builds it as `CONFIG_CPU == PP5022`, with the same PLL code. It is not separately verified on 5G silicon.
- **Stock build:** compiles to the same bytes (`fw` `.text` 1,142,212 before and after; the stock bootloader hash is unchanged).

### Risks
1. The PLL lock bit could read stale right after power-up. The firmware logs every lock time, and a lock time near 0 µs points to this.
2. UDMA timing while the hold switches: the switch happens before the command and after the transfer ends.
3. The fit table comes from upstream benchmarks [estimate], not from measurements on this device. The adaptive check limits the cost of a wrong entry to boosts, not dropouts.
4. A PLL power-up for each ATA DMA transfer while idle costs one lock wait per command. This matters only if C4a removes the 80 MHz boost on fill.
5. The UI is 20 % slower for up to 0.5 s after the backlight comes on, until the `LIT` hold is sampled. Animations boost anyway.

### Measurement (owner, device)
1. **A/B in `power.csv`:**
   - Run it at a 30 s interval: stopped and dark for 1 h with the switch on, then 1 h with it off.
   - Then play FLAC or MP3 dark for 1 h, on and off.
   - Compare mV/h, and the `idle_pct` in the `clk` lines.
2. **Audio dropouts:**
   - Play dark for 30 min each: MP3 128k, MP3 320k, FLAC 44.1/16, and AAC as a control.
   - Listen for gaps, and check that no `clk strike` appears for MP3 or FLAC.
3. **Tick accuracy:** with the switch on, stop and go dark for at least 10 min, toggling the switch off and on at the start and at the end. Each toggle writes a `clk` line. Between the two lines, the tick seconds and the `rtc=` time should advance by the same amount, within 1 s.
4. **USB:** plug in while dark and idle. The disk should mount, and a 1 GB copy should give the same throughput as with the switch off.
5. **Lock time:** read `lock_us` and `max_us` from the `clk` lines. A plausible value is tens to hundreds of µs.
