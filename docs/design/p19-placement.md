# P19 workload placement: where every job runs, and what is off in each state

*Plan, rev 0.1 (2026-10-23). Docs and analysis only: nothing here has been built. Written by the platform session together with the BCM session (`bcm2722-hardware-video-decode`), whose contracts on `p19-bcm-prep` (`7f91368864`; answers to this plan's questions at `c1634393d5`, §9.1) it builds on: `bcmfw/jobs.h` and `docs/research/bcm2722/jobs.md` (job interface rev 0.1), `memory-contract.md`, and `power-contract.md` (states C0–C5). The ranked offloads come from `p14-r3d:docs/research/bcm2722/display-offload-plan.md`.*

**The owner's request:** "a detailed plan of where each current job will occur, so we can shut down as much of the iPod in idle as possible, while keeping everything smooth and elegant the whole time."

**The owner's architecture**, which this plan implements:
- **CPU = back end:** codec, buffering, DSP, storage.
- **COP = front end:** UI and input.
- **BCM2722 = display driver.**
- **Dark playback** with a queue: one core plus the DAC, and the drive only for refills. The COP, the BCM and the drive are off, and nothing else comes online unless it is really needed. **Target: under 25 mA average.**
- **Every hardware path keeps a CPU fallback.** For chip-side buffers the owner chose the degraded fallback (`jobs.md` §7, option b, 2026-10-23). The PP keeps no copy, so if the chip fails:
  - the drift goes on, drawn by the CPU (revised 2026-10-25, `jobs.md` §7; the still cover is only the developer row's);
  - Cover Flow is disabled, with a small pop-up saying it isn't available at this time;
  - every other job takes its CPU path.

**Tags:**
- **[silicon]:** measured on this unit, with the log named.
- **[measured]:** our own counters on the device (power log, `m0` lines) or in the harness.
- **[code]:** read from the tree at `p18-114` (`de3a942edb`).
- **[estimate]:** arithmetic from the numbers named.
- **[hypothesis]:** untested.
- **[bcm]:** the BCM session's contracts or answers.

---

## 0. The headline numbers

| Quantity | Value | Tag and source |
|---|---|---|
| Dark FLAC playback, before #114 (drive kept in standby) | ~105 mA average (28.7 h on 3,000 mAh) | [silicon] rundown 2026-10-22, `hardware.md` "Measured discharge curve" |
| Same voltage band (4,141–4,147 mV), dark FLAC, before vs after #114's drive power-off | **39.5 → 13.7 mV/h** (least-squares over ~1 h each, `s` 900–5000) | [silicon] `session log, dark-play run power.csv`, boots 2026-10-22T22:57 and 2026-10-23T14:59 |
| Dark FLAC average now | **~35–40 mA** (105 mA × 13.7/39.5) | [estimate]: one hour per arm; the earlier arm also had `lcd_pct` 28 % against 1.4 %, so the screen's share is mixed in |
| Gap to the 25 mA target | **~10–15 mA** | [estimate] |
| CPU idle / COP idle, dark FLAC (COP decode) | 90.4–91.6 % / 29.5–32.4 % | [silicon] darkplay-1614, four boots, 126–1,994 rows each |
| Thread wakes, dark FLAC minute | **88–95 /s**: `codec` 44, `codec cop` 38, `audio` 2, `ata` 2, `tagcache` 1–2, `power` 0.5, `buffering` 0.03–0.07, `main` 0.017 | [silicon] `ipodui.log.1` lines 212–231 |
| The COP's tick pulse | 100 /s, always (`kernel-pp.c:36`, `core_wake(COP)` on every tick) | [code] |
| Drive power-ups, dark FLAC | **23 in 1.04 h (~22/h)**; `stor_on_pct` 9.8 %; each off follows 13–25 s of power (`stor pwr off ms=12980…25270`) | [silicon] darkplay-1614 |
| Disk traffic, dark FLAC | 59 commands and 1,964 KB per 30 s row (~65 KB/s) | [silicon] |

**What this says about the plan:**
- The biggest single cut is done: #114's drive power-off took roughly two-thirds of the dark current.
- **The rest of the gap is in four places:**
  1. how long and how often the drive is powered (§5.2: about 10 % on-time, mostly the idle hold);
  2. the second core, busy two-thirds of the time on decode plus 100 pulses a second (§4);
  3. the CPU's polling threads (§5);
  4. an unknown base: PMU, DAC/amp, SDRAM refresh, the clickwheel controller, regulator losses.

  Only a meter can split item 4 (§8, prerequisite M-1). **This plan does not claim that placement alone reaches 25 mA.** It removes what placement can remove and names the measurement that finds the rest.

---

## 1. Inventory: every job that runs today

Today's placement is the inverse of the target: **the UI runs on the CPU, and the COP decodes** (Fit-2, on by default since 2026-10-18, `fit2-brief.md` §12.0). Every thread is created with `IF_COP(, CPU)` except the COP decode worker (`apps/codec_cop.c`, `firmware/common/cop_job.c`) and upstream's MP3 synth (`lib/rbcodec/codecs/mpa.c:336`, `IF_COP(, COP)`) [code].

Wake rates are per second unless marked. Costs are a share of one core at the clock named. Memory is resident unless marked transient.

### 1.1 Back end: playback and storage

| # | Job | What it does | Core / thread (priority) | Wakes dark / lit | CPU cost | Memory | Devices | Evidence |
|---|---|---|---|---|---|---|---|---|
| B1 | **Decode** | the codec's frame loop | **COP**, `codec cop` worker (13), stack = top 9 KB of the codec slot | 38 / 38 | **67–70 % of the COP** at 24 MHz on FLAC (~16 MHz-equivalent) | codec slot 1 MB; codec IRAM 80 KB (shared, uncached) | none directly | [silicon] darkplay-1614 `cpu1=codec cop 67–69 %` |
| B2 | **Codec call server + DSP** | serves the COP's `cop_job_call()`s (buffer reads, `pcmbuf_insert`, elapsed); runs `dsp_process()` on the CPU inside `pcmbuf_insert` (`codec_thread.c:262`), reading the COP's PCM uncached | CPU, `codec` (13), ~9 KB stack in IBSS | 44 / 44 | **7 % of the CPU** (~1.7 MHz) | stack 9 KB IRAM | — | [silicon] `cpu2=codec 6.9–7.0 %`; [code] `codec_cop.c:155–170` |
| B3 | MP3 synth | upstream's split MP3 synthesis | COP, `mp3dec` (16) | per frame (MP3 only) | ~1/3 of MP3 decode [rockbox] | small | — | [code] `mpa.c:331` |
| B4 | PCM DMA + FIQ | feeds I2S from pcmbuf | CPU FIQ (no thread) | ~21.5 interrupts/s (8,192 B chunks at 176.4 KB/s) | small | pcmbuf | DAC (I2S), WM8758 clocks | [code] `pcmbuf.c:53`, `pcm-pp.c`; rate [estimate] |
| B5 | Audio engine | track transitions, queue, next-track loads | CPU, `audio`, `queue_wait_w_tmo(HZ/2)` (`audio_thread.c:117`, `playback.c:4263/4273`) | **2 / 2** (always); 7 on a track change | <0.1 % | ~5 KB stack | drive (on a track load) | [silicon] `m0 wakes audio n=120–121`/min; [code] |
| B6 | Buffering | file → audio buffer fills, watermark | CPU, `buffering` | 0.03–0.07 dark (18-A: 0 when full) | burst during a refill | audio buffer (≥ 39 MB floor on 64 MB) | drive | [silicon] n=2–4/min; `hardware.md` wake table |
| B7 | **Storage thread** | ATA idle/standby, power-off timer (#114), idle callbacks | CPU, `ata`, `queue_wait_w_tmo(HZ/2)` (`storage.c:169`) | **2 / 2** (always) | <0.1 % | ~3 KB stack (`ATA_THREAD_STACK_SIZE` ×3) | drive | [silicon] `m0 wakes ata n=120–140`/min; [code] |
| B8 | ATA DMA + IRQ | UDMA2 transfers, PLL hold `ATA` per transfer | CPU (caller's thread) | per command | — | — | drive, PLL (30 MHz) | [code] `data-paths.md` R8 |
| B9 | Drive power (#114) | cut after `STOR_PWR_HOLD_DARK_S` 3 s (was 10 s at the darkplay-1614 measurement; 698181ffbf) / `_LIT_S` 60 s idle | CPU (storage thread) | — | — | — | drive regulator | [code] `stor_pwr.h:95–96`; [silicon] ~15 s on per cycle |
| B10 | Tagcache thread | queue, statistics drain, `maint_poll()`, dbq, path queue, `ui_pre_request`; the boot scan | CPU, `tagcache`, 1/s `SYS_TIMEOUT` | **1 / 1**; scan at boot: 9,500/min | ~0 idle; 91–93 % during a scan | RAM cache ~7 MB at 50k (in the floor) | drive (drain only while it's awake) | [silicon] `m0 wakes tagcache n=60`; `hardware.md` "Not done, and why" |
| B11 | Maintenance windows (#88) | db commit, re-sorts, deferred work in the pause/boot windows | CPU, tagcache thread + boost | 0 outside a window | bursts at 80 MHz | candidate RAM, transient | drive | [code] `apps/ipodui/maint*.c` |
| B12 | Database queue (#109), shuffle refills | queue by index, `lib_shuffle_idle()` refills | **CPU, UI thread** (`np_pb_idle()`, `np_playback.c:786`) | per main-loop pass, which is ~0 dark | small | queue ~405 KB at 50k | drive (rarely) | [code] |
| B13 | Resume saves (`ab_store`) | per-book resume, checkpoints, write-now on pause/stop | **CPU, UI thread** + storage-idle callback (≤ 1 per 30 s) | at storage idle | small | small | drive (piggybacked) | [code] `ab_store.c:794,856–863` |
| B14 | Art builder | JPEG decode → pre-scaled RGB565 caches; panel pool | CPU, `artcache` (background), off 2 s after input | 56/s while busy; 0 idle | **26–46 s of decode per large cover** at 24–30 MHz; `cpu1=artcache 57–80 %` | ~0.27 MB pinned + NP read-ahead 51,752 B | drive | [silicon] run-1137, darkplay-1614 boot 11:24; offload plan §2 |
| B15 | USB storage | MSC, `pf_worker` (test), USB thread | CPU, `usb` (`PRIORITY_SYSTEM`) | per transfer | DMA-bound | USB ring | USB PHY, drive, PLL `USB` | [code] |

### 1.2 Platform: power, clocks, timers

| # | Job | What it does | Core / thread | Wakes dark / lit | Cost | Devices | Evidence |
|---|---|---|---|---|---|---|---|
| P1 | **Kernel tick** | 100 Hz Timer 1; runs the tick tasks; **pulses the COP each tick** | CPU IRQ + COP wake | **100 + 100** | small each | timer | [code] `kernel-pp.c:31–36` |
| P2 | Power thread | voltage filter, charger edges, shutdown, PLL hold evaluation (`pll_idle_poll()`) | CPU, `power` | 0.5 quiet / 2 otherwise | `cpu1=power 2–4 %` when nothing else runs | PMU (I2C), ADC | [silicon] `n=30–33`/min; 18-A |
| P3 | ADC tick | `adc_scan(0..3)` once a second | CPU tick task | 1 (inside the tick) | small | ADC | [code] `adc-pp5020.c:179` |
| P4 | Button tick | repeat and debounce, headphone detect | CPU tick task (100 Hz) | inside the tick | small | GPIO | [code] `button.c:444` |
| P5 | Clickwheel ISR | wheel packets (source 40) | CPU IRQ | per packet | small | opto/wheel controller | [code] `button-clickwheel.c` |
| P6 | USB tick | VBUS debounce | CPU tick task | inside the tick | small | GPIO | [code] `usb.c:785` |
| P7 | Backlight thread | timeouts, PWM fade, LCD sleep after 60 s | CPU, `backlight` | 0 with no timer (18-A) | — | backlight, BCM (LCD sleep) | `hardware.md` wake table |
| P8 | Scroll engine | upstream line scrolling (developer lists only) | CPU, `scroll` | 0 (18-A) | — | — | same |
| P9 | Piezo | clicks (haptic mod) | CPU, `piezo` (real-time) | 0 dark; 1.4/s lit while turning | small | piezo | [silicon] `n=73–84`/min lit |
| P10 | LCD tick | completes BCM updates in flight | CPU tick task, **only while an update is pending** | 0 dark | small | BCM port | [code] `lcd-video.c:372,685` |
| P11 | Clock policy (`clockpol`) | per-track PLL need, strikes, `CP_LIT` | CPU, UI thread pass | per pass | small | PLL | [code] `lit-24mhz.md` §3.2 |
| P12 | DAC standby (P15) | codec to standby 5 s after PCM stops | CPU (pcm path + UI `dac_wait()`) | 0 playing | — | WM8758 | [silicon] wake 6.4 ms median |
| P13 | batcal (#70) | 1 Hz sampling timeout; fit at boot | CPU timeout callback (ISR) | 1 (ISR only) | tiny | ADC value | [code] `batcal.c:377` |
| P14 | bootbatt latch | 10 s timeout near 0 % only | CPU timeout | 0.1 while owed | tiny | drive (one sector) | [code] |
| P15 | Power log (devmode) | 4 Hz sub-sample timeout; ring flush at storage idle | CPU timeout | 4 while on (ISR only) | tiny | drive (piggybacked) | [code] `powerlog.c:765` |
| P16 | uilog flush | log ring → file at storage idle | CPU, storage idle | at storage idle | small | drive (piggybacked) | [code] |
| P17 | P18 soft sleep | `power_sleep()`: both cores asleep, timer polls 60 s, RTC | CPU, UI thread | 1/60 s asleep | — | PMU, RTC, wheel (wake) | [code] `power_sleep.h`; [silicon] sleeplab-1632 |
| P18 | iAP | dock serial (compiled out on this unit, `HAVE_IPOD_DOCK30` off) | CPU, `iap`, 100 ms timeout | — | — | serial | [code] |

### 1.3 Front end: UI and display

| # | Job | What it does | Core / thread | Wakes dark / lit | Cost (PP) | Memory | Devices | Evidence |
|---|---|---|---|---|---|---|---|---|
| F1 | **UI main loop** | input, screens, header, `*_service()` calls | **CPU**, main thread (8 KB stack in IRAM) | **0.017 dark** (unlit face 1/min) / ~6 lit | `cpu2=main 8–24 %` lit | main stack IRAM | wheel, BCM | [silicon] `m0 wakes main n=1` dark, `n=350` lit |
| F2 | **Drift** (panel pan + half steps) | main menu / Music split screen | CPU, UI thread | 0 / **13.3 pushes/s** (20 wakes/s) | **~20 % of the CPU** at 30 MHz; **pins the lit PLL** | 360,000 B cover slots (64 MB) | BCM | `lit-24mhz.md` §2.3, offload plan §2 |
| F3 | Panel cross-fades / rotation | `xfade_rect` 8 steps per 7 s cover; set rotation | CPU, UI thread | 0 / bursts | 40 ms per 160×240 step unboosted | in F2 | BCM, drive (panel pool) | same |
| F4 | **Marquee** | long selected titles | CPU, UI thread | 0 / 20 | **15–38 % of the CPU** + 15–35 KB push per step (`code-audit-1`); the P19 brief says F1/F2 (skip transparent glyph pixels, render once and pan) have landed and puts it at ~5 % [estimate]: **to reconcile with a device count** | strip 184,000 B | BCM | [measured] `code-audit-1.md` |
| F5 | List glide / quick scroll | ~30 fps while turning | CPU, UI thread, **boosted** | 0 / 30 | 230–610 k instructions + 320×220 E2 push | strip | BCM, PLL (boost) | [measured] |
| F6 | Push/pop slides | ~6 frames | CPU, UI thread, boosted | 0 / bursts | 60–100 ms before frame 1 | strip | BCM | [measured] |
| F7 | Text and rows | glyph rendering, row art | CPU, UI thread | 0 / on change | 44–73 ms per 9-row list at 30 MHz | fonts ~102 KB; row-art LRU 109,428 B | — | [measured] |
| F8 | Header + clock | title ↔ time swaps, battery glyph | CPU, UI thread | 0 / ≤ 1 | 75–90 k instructions per header | — | BCM | [measured] |
| F9 | **NP clock** (elapsed, bar, remaining) | Now Playing once a second (`PLAYING_TIMEOUT` HZ/2 for the play glyph) | CPU, UI thread | 0 / 2 | small rect + push | — | BCM | [code] `ui_main.c:103–105` |
| F10 | NP reflection, NP compose | reflection on change; `np_compose` once per album at build | CPU | — | ~281 k instructions per bundle; reflection unmeasured | NP block, `npf_buf` 51,752 B | — | offload plan §2 |
| F11 | Unlit face (Hold) | minute redraw | CPU, UI thread | 1/min | small | — | BCM (lit only) | [code] |
| F12 | Alarm engine (P12, branch `p12`) | due tick; ring with piezo beeps every 200 ms–1 s; ring screen | CPU, UI thread (deadline waits) | 0 until due | small | in the module arena | piezo, RTC alarm, BCM (screen) | [code] `p12:apps/ipodui/alarm.c:400–420` |
| F13 | Sleep Timer (P12) | `sleeptimer_endtick` → pause, then sleep | CPU (powermgmt + UI) | 0 | — | — | — | `p12:docs/phase12-brief.md` row 100 |
| F14 | Class A Extras (P12) | Clocks, Stopwatch, Notes, … on the UI thread only | CPU, UI thread | ≤ 30 fps while visible | unboosted, ≤ 10 % boosted per 10 s | module arena (`MOD_ARENA_SIZE`) | BCM, piezo | `p12` brief §2.1 |
| F15 | `MOD_XINDEX` | sync-time indexers (Notes, Contacts, Calendars) | CPU, in the sync windows | 0 outside windows | burst | arena | drive | `p12` brief §2.3 |
| F16 | Video player | feed, BCM protocol | CPU, `vplayer` (`PRIORITY_VIDEO_FEED`) | per frame | 30 MHz feed | — | BCM at 150 MHz, drive | [code] `vplayer.c:1286` |

**What the inventory shows:**
- **Dark playback runs three always-on 2 Hz pollers that 18-A did not convert:** `audio` (HZ/2), `ata` (HZ/2) and `tagcache` (1 s). That's ~5 thread wakes a second with nothing to do.
- **Most of the wake rate is the codec's split across cores:** ~82 wakes/s, plus the COP's 100 Hz pulse.
- **Back-end work on the UI thread is the obstacle to a COP front end:** shuffle refills, the database queue, resume saves, bootbatt and batcal service, and the art builder's input gate (B12, B13, P11, P14).

---

## 2. Target placement per power state

### 2.1 Blocks: what is powered or clocked

**Columns:**
- **Clock:** the shared PP clock. One clock serves both cores (`data-paths.md` R7.6).
- **BCM:** the power contract's states (`power-contract.md` §1): C1/C2 lit at 27 MHz, C0 cut, C3/C4 standby only after stage 5s passes, 150 MHz only for video and Class C.
- **Drive:** "burst" means powered for a refill, then cut after the hold.

| State | CPU | COP | Clock / PLL | BCM | Drive | DAC / amp | Wheel (opto) | Backlight | LCD panel |
|---|---|---|---|---|---|---|---|---|---|
| **Lit, browsing** (not playing) | back end idle; sleeps | **front end** (UI) | 24 MHz once the drift and marquee are chip jobs (Q-4; [bcm] yes, records are tiny and `PRESENT`'s deadline is chip-side); boost only for glides and slides until the chip's SCROLL/SLIDE land | **C1/C2**, 27 MHz; C2 busy-polls until stage 5i passes, then the core sleeps between jobs [bcm] | lit hold 60 s, then cut | standby (P15) | on | on | on |
| **Lit, Now Playing** | decode + DSP + buffering | UI: NP clock 1/s, marquee as a chip window | 24 MHz (`LOAD` hold → 30 for heavy formats) | C1/C2, 27 MHz | burst | on | on | on | on |
| **Dark playback** | **decode + DSP + buffering + storage; the only core** | **parked**: no threads, no tick pulse (§3.4) | 24 MHz, PLL off (30 only for `LOAD` formats and ATA transfers) | **C0 cut** | burst, ≤ 6/h target (§5.2) | on | on (Hold on: [hypothesis] off, Q-6) | off | off |
| **Paused, lit** | idle | UI | 24 MHz | C1/C2 | lit hold, then cut | standby after 5 s + 3 s tail | on | on | on |
| **Paused, dark** | idle; the maintenance window may run (boosted bursts) | parked | 24 MHz (80 in a window) | C0 (C3/C4 inside `BCM_STANDBY_WINDOW_S` after 5s passes) | cut; on only for a maintenance window | standby | on | off | off |
| **Soft sleep** (P18) | asleep in `power_sleep()`; RTC timer polls 60 s | asleep | PLL off; no holds (sleep refuses otherwise) | C0 | off | standby | on (the wake source) | off | off |
| **USB session** | USB + drive DMA | UI: the USB screen, then parked when dark | 30 MHz (`USB` hold) | C1/C2 while lit; C0 dark | on | standby | on | per timeout | per backlight |
| **Alarm ring** | back end; pauses music (Q6 of P12) | UI: ring screen | 24 MHz | cold wake → C1/C2 | off (the alarm record is already on disk) | standby (the ring is the piezo) | on | on | on |
| **Class A Extra open** | back end (music keeps playing) | UI + the module (front end only) | 24 MHz, ≤ 10 % boosted per 10 s | C1/C2 (shell jobs only; a Class A module sends none) | burst | per playback | on | on | on |
| **Class B game** | back end at the shrunken buffer | game + its COP-side work | 24–30 MHz, declared boost budget | C1, 27 MHz; its job budget through the P19 API, in the UI classes (`J_CLS_STRIP`/`J_CLS_SCRATCH`) within its manifest budget, beside the drift; no `J_CLS_GAME` slots (those exist only in `J_MODE_GAME`) [bcm] | burst (borrow-sized) | on | on (absolute mode allowed) | on | on |
| **Class C game** | the game's session (music paused) | as declared | session boost allowed | **C1 at 150 MHz** (4b/4c flags), `J_MODE_GAME`, epoch+1 at exit | its own policy | per game | on | on | on |
| **Video** (reference) | feed (`vplayer`) | parked or the soundtrack decode (interim, `fit2` §6 #6) | 30 MHz + boosts | C1 at 150 MHz with the 4b/4c flags, else 27 MHz; `J_MODE_VIDEO` evicts the UI pools (epoch+1), slots return lazily after `CLOSE` [bcm] | streaming | on | on | on | on |

### 2.2 Jobs per state (target)

"—" means the job does not run in that state. **CPU** and **COP** are the PP cores, **BCM** is the chip, with **(fb)** marking its CPU fallback.

| Job | Lit browse | Lit NP | Dark play | Paused lit | Paused dark | Soft sleep | USB | Alarm | Class A | Class B/C |
|---|---|---|---|---|---|---|---|---|---|---|
| B1 decode | — | **CPU** | **CPU** | — | — | — | — | — | CPU | CPU (B) / — (C) |
| B2 DSP | — | CPU | CPU | — | — | — | — | — | CPU | CPU / — |
| B5–B7 audio, buffering, storage | CPU (events only) | CPU | CPU (events only) | CPU | CPU | — | CPU | CPU | CPU | CPU |
| B10 tagcache | CPU (events) | CPU | **CPU, events only (no 1 s poll)** | CPU | CPU (window) | — | — | — | CPU | CPU |
| B12/B13 queue, shuffle refill, resume save | **CPU** (moved off the UI thread, §3.2) | CPU | CPU | CPU | CPU | — | — | CPU | CPU | CPU |
| B14 art builder | CPU background; **JPEG on the BCM** once `J_OP_DECODE` lands, lit or on the charger only, streaming MCU rows in SRAM (`J_CLS_SCRATCH`, no SDRAM bring-up) [bcm] (fb: CPU) | same | **— (deferred to lit or charging)** | same | — (charging: CPU) | — | — | — | — | — |
| P2 power | CPU 0.5/s | same | **CPU ≤ 0.1/s** (§5) | same | same | timer poll | 2/s | same | same | same |
| F1 UI loop | **COP** | COP | **— (parked)** | COP | — | — | COP | COP | COP | COP |
| F2/F3 drift, fades | **BCM** (fb: the CPU drift) | — | — | BCM | — | — | — | — | — | — |
| F4 marquee | **BCM window** (fb: COP render-once-and-pan, F2 of the offload plan) | BCM | — | BCM | — | — | — | — | BCM | — |
| F5/F6 glides, slides | **BCM**: a glide is `SCROLL` on `J_SURF_FRONT` + `PRESENT` of the uncovered rows, the PP uploading only the new rows into a `J_CLS_STRIP` slot; a slide is two DMA pushes from 'from' and 'to' at offsets [bcm] (fb: COP, boosted as today) | — | — | same | — | — | — | — | same | game's |
| F7 text, rows | COP | COP | — | COP | — | — | COP | COP | COP | game |
| F9 NP clock | — | **COP small rect + `PRESENT`** | — | — | — | — | — | — | — | — |
| F12 alarm engine | COP (deadline) | COP | **CPU deadline** (it must fire with the COP parked; §3.4) | COP | CPU | RTC alarm | COP | COP | COP | COP |
| Cover Flow (P7) | **BCM** (`J_CLS_COVERFLOW`, SDRAM); fb: **disabled + pop-up** | — | — | — | — | — | — | — | — | Class B/C |

**BCM rows status:** checked by the BCM session on 2026-10-23 (`p19-bcm-prep` `c1634393d5`). It confirmed the rows and made the corrections now folded in: C2 polling until 5i, how glides and slides are built, Class B's slot classes, video's clock flags, and the art decode's memory. Its answers are in §9.1. The **reference executor** (`bcmfw/jobs/jref.c`, tests in `jtest.c`, 1,312 host cases in `jobs_test.py`) exists: `FADE` equals `xfade_rect` at all 256 weights, `AVG` equals `AVG2`, and the scripted scene gives the same checksum on the Mac and in vc2sim. It is also the CPU fallback of rule 2. 7.2 KB of chip code, within the 14 KB budget [bcm].

---

## 3. Delegation rules: where a new job goes

Apply them in order. The first rule that matches decides.

1. **Needed in dark playback → CPU.** Anything that must run with the screen off and music playing lives on the CPU, in a CPU thread that waits on an event or a deadline. That covers decode, DSP, buffering, storage, the power thread, alarms coming due, the sleep timer, resume saves and queue refills. **Dark playback has one core.**
2. **Touches pixels while lit → BCM, through a job, with a CPU fallback.**
   - The job is chosen by its `J_OPS` bit plus the per-boot "slow or failed" mark (`jobs.md` §2, §6).
   - The fallback is the reference executor (`bcmfw/jobs/`), bit-exact, on the front-end core.
   - A job **never wakes the chip on its own** (offload plan §1). If the chip is in C0, the work waits for lit, or takes its fallback when it can't wait.
   - **In C0 the job API answers "chip off" at once and never polls** (contract rule [bcm]). `lcd_sleep()` removes `lcd_tick` (`lcd-video.c:1192`), and liveness checks run only inside a session or a wait, so a cut chip causes **zero PP wakes**.
   - Chip-side buffers have no PP copy: the owner's option (b).
3. **The user sees it or touches it, and it isn't pixels → COP (front end).** Input handling, screen logic, layout, text rendering into PP surfaces, and module (Extras) code. The front end calls the back end only through:
   - queue posts (playback control: play, pause, skip, seek, volume);
   - published state (§3.3).

   It never calls `core_alloc`, the file layer, `storage_*`, tagcache searches that grow the RAM cache, or anything else on P12's Class A deny list (`p12` brief §2.3). **That deny list is the front end's API boundary.**
4. **Touches a peripheral, an interrupt, the file system or the allocator → CPU.** The COP takes no interrupts and owns no peripheral (`fit2-brief.md` §3.1). Two exceptions:
   - the BCM command ring and host port, which the front end owns (§3.5);
   - the piezo for clicks, through a queue post to the `piezo` thread.
5. **Long compute with no I/O → a job.**
   - **Lit:** a COP job, if the COP has headroom over the UI's frame budget.
   - **Dark:** the CPU, in the same thread, at 24 MHz. Never the COP, since waking a parked core costs more than the work saves at these sizes [estimate].
   - Pixel compute (JPEG, scale, blend) goes to the BCM when lit (rule 2).
6. **Background maintenance (database, art, indexers) → CPU, in a window.** Windows are:
   - the paused-dark maintenance window (#88);
   - the boot window;
   - **charging, which beats them all**.

   Each runs in boosted bursts, with "paused is the normal state" (memory). **Never in dark playback on battery** unless the user is waiting on it (a just-synced library).
7. **Periodic → a deadline, never a poll.** A job that has to run periodically:
   - registers a deadline (18-A's pattern: `power_thread_kick()`, `scroll_thread_kick()`, the backlight's `BACKLIGHT_TIMER_SET`);
   - merges into the nearest existing wake (§5.3);
   - and declares its rate per state in §5's table. A new poller needs a row in that table, or review refuses it.
8. **Every hardware path keeps a CPU fallback**, and the fallback runs on the core that rules 1–4 choose: back-end fallbacks on the CPU, front-end fallbacks on the COP.

### 3.1 Cache coherency between the cores (the hard part of a COP front end)

`data-paths.md` R7 and `fit2-brief.md` §3.2 are the rules. With the UI on the COP, they apply to far more data than the codec channel.

| Data | Producer → reader | Rule |
|---|---|---|
| Small shared state: play status, elapsed, volume, battery %, track id, sleep request, the UI's wake flags | CPU → COP | **`SHAREDBSS`** (uncached alias) or IRAM, ≤ 256 B per record. Single-writer words; multi-word records carry a sequence counter (write odd, fields, write even; the reader retries on odd or a change). **Never a cached struct written by both cores** (D10: lost counts) |
| Bulk read-mostly tables: the tagcache RAM cache, view pool, chapter table, queue | CPU → COP | **Generation hand-off.** The CPU builds, writes back, bumps a generation word in `SHAREDBSS`. The COP sees the change and does **write back + drop** of its own cache before the next read. One cache operation per generation, never per element. Writers are never concurrent with readers of the same region: the CPU builds a new candidate and publishes it (#85's candidate/commit already works this way) |
| **buflib / `core_alloc` blocks** | CPU-owned | **The COP never holds a raw pointer across a yield.** Movable blocks can be compacted by the CPU at any time. The COP reads them only while pinned (`core_pin` taken by the CPU for it, or a handle-plus-lock API), or through a copy the CPU hands over. This is the biggest porting risk of the UI move |
| Surfaces the COP draws: framebuffer, strip, row-art LRU | COP → BCM port (and CPU for the E2 engine) | The COP writes back before an E2 transfer starts. CPU copies from the COP read the uncached alias. Pinned for the life of the shell |
| The BCM command ring and host port | COP (front end) and CPU (`vplayer`) | Uncached I/O, so no cache issue. **One PP-side lock** (`jobs.md` §3): Peterson on uncached bytes, since `SWP` across cores is unproven (`data-paths.md` R7.1) |
| Code | — | Modules loaded by the CPU into the arena are posted to the COP with `COPJ_CODE` (write back, then drop data and instruction caches on the COP), the codec path's rule |
| Stacks | own core | cached; the kernel writes back the fill pattern before a thread starts on the other core (as `cop_job` does) |

**Never a bare discard** (R7.3). **The cost of a whole-cache drop is still unmeasured** (fit2 risk R2), which makes it prerequisite M-5: the generation hand-off is cheap only if a drop is cheap at the UI's event rate.

### 3.2 Back-end work that leaves the UI thread first

These run on the UI thread today and must move to a CPU thread before the UI can move to the COP:

| Today on the UI thread | Moves to | Why it must |
|---|---|---|
| `lib_shuffle_idle()`, `ab_store_idle()`, `lib_book_resumed_idle()` (`np_pb_idle()`) | the `audio` thread's event loop, or a small back-end service queue on the tagcache thread | it does file I/O and allocation |
| `ui_log_service()`, `powerlog_service()`, `batcal_service()`, `bootbatt_service()` | the storage thread's idle callback (they already gate on storage being awake) | file writes |
| `clockpol_service()` | the power thread (it already evaluates the PLL holds) | it sets PLL needs |
| art reads for Now Playing (`npf_buf`) and row art | the art builder thread (it already owns the NP read-ahead since npfix) | file I/O |
| `lib_open()` view builds | a CPU job the UI posts; the result is published by generation (§3.1) | `core_alloc`, RAM-cache walks |

This list is the P19 audit item that P12's Class A deny list turns into a check (`p12` brief §2.4).

### 3.3 Front-end ↔ back-end interface (sketch)

- **Commands (COP → CPU):** the existing `audio_*`/`playlist_*` posts, made queue-only, plus `svc_post(kind, args)` for the moved services in §3.2. Nothing blocks the UI on a reply. Results come back as published state, with a wake for the COP.
- **State (CPU → COP):** one `SHAREDBSS` "now" record (seqlock): status, elapsed ms, track id, buffer state, battery, charger, sleep and alarm flags, plus the generation words.
- **Wakes:** a CPU-side post to a COP thread calls `core_wake(COP)` explicitly. **This prerequisite (K-1) replaces the 100 Hz pulse** (§3.4).

### 3.4 Parking the COP

- **Today:** every tick does `core_wake(COP)` (`kernel-pp.c:36`), so an idle COP still wakes 100 times a second to run its scheduler. That's < 0.1 % of a core [rockbox; estimate], but it also keeps the COP's clock, cache and bus activity alive.
- **Target:** the COP is **parked** when it has no runnable thread and no timeout pending.
  - The CPU stops pulsing it.
  - Anything that makes a COP thread runnable calls `core_wake(COP)`: a queue post, a semaphore release, or a deadline the CPU's tick finds due on the COP's behalf.
  - **Prerequisite K-1:** a kernel change in `firmware/kernel` and `kernel-pp.c`, with a host test of "post to a sleeping COP thread wakes it within one tick".
- **Dark playback:** the UI arms nothing. The unlit face's minute isn't drawn (dark), and the alarm's due time lives on the CPU (rule 1). So **the COP is parked for the whole dark spell** [design].
- **Whether a parked ARM7TDMI core draws anything measurable is unknown** [hypothesis: clock-gated by `PROC_SLEEP`]. M-1 answers it.

### 3.5 The BCM from the COP

The BCM session found no protocol obstacle [bcm]. The port at `0x3000_0000` is uncached device I/O [rockbox-derived]. D7's torn reads concern 16-bit halves, not cores. There are three constraints:

1. **One cross-core lock for all port access.** The port has a single auto-increment address register. `lcd-video.c` already has a corelock (`lcd_state.cl`), so the job mutex must be that kind of lock, or nest inside it.
2. **E2 reads SDRAM.** Pixels the COP writes into cached memory must first be written back from the **COP's own** cache, and cache operations are whole-cache and per core. Either the core that wrote the pixels starts the E2 piece, or it writes through the uncached alias.
3. **One reader for replies and events** (or a demultiplex by tag). Simplest: **the COP owns the ring**, and the CPU's video feed hands it records. `lcd_tick` remains only for the CPU fallback path; completion is the job layer's `FENCE` wait.

---

## 4. Codec placement for dark playback

### 4.1 What the device shows (FLAC, dark, 24 MHz, PLL off)

*Clock evidence (reconciled with the P19 brief, rev 1b):* every `clk stats`/`clk lock` pair of the dark run shows `holds=0 load=0 fmt=7`, and the PLL-off share of uptime rose from 43 % at 15:05 to 93 % at 16:11 (cumulative since boot), with 18,310 PLL power-ups by then for ATA transfers [silicon, `darkplay-1614/ipodui.log.1` lines 107–385]. The `load=1` on the boot's `clk lock` line comes before any track. So dark FLAC already runs at 24 MHz with the PLL off; option B's gain is active cycles, the parked COP and wakes, not the PLL.

| | COP decode (today) | CPU decode (switch off) |
|---|---|---|
| CPU busy | 8.4–9.6 % ≈ **2.0–2.3 MHz**-eq (7 % of it the call server + DSP) | **~55 %** ≈ 13 MHz-eq |
| COP busy | 67.6–70.5 % ≈ **16–17 MHz**-eq | ~0.5 % |
| Active core-time, both cores | **~18–19 MHz**-eq | **~13 MHz**-eq |
| Thread wakes | ~82/s across cores + COP pulse 100/s | ~10–20/s [estimate: codec yields per frame] |
| Evidence | [silicon] darkplay-1614 (four boots, 126–1,994 rows) | [silicon, coarse] session 36 (`fit2-brief.md` §12; clock at the time not logged per row) |

**The split costs ~40 % more active cycles than one core doing the work** [estimate from the rows above]. Fit-2 estimated 0.5–1 MHz of overhead (`fit2-brief.md` §7). The device shows ~5 MHz. The likely reasons [hypothesis]:
- the COP reads the bitstream and the CPU reads the PCM through the **uncached** alias, so every word is an SDRAM access;
- ~40 calls a second, each with a cache write-back;
- thread switches on both cores.

**Fit-2's case was never dark playback.** Its own table (§7) says "dark FLAC: no gain". The win it claimed was **lit browsing + a codec**, where one core would have to boost to 80 MHz.

### 4.2 Options

Power model [estimate]:
- DCDC1 holds the core at 1.2 V at every clock (`clocks-power.md` §8), so energy per cycle is roughly constant.
- A higher clock buys nothing per cycle and adds the PLL's own power plus a faster clock tree. A lower clock saves only the frequency-proportional base.
- The cores' idle floor at 24 MHz (crystal, PLL off) is the same whether the work ran fast or slow, because I2S DMA needs the bus clock. So **the idle floor can't drop to 32 kHz while music plays.**

| Option | What | Active core-time | Wakes | Clock | Estimated effect vs today |
|---|---|---|---|---|---|
| **A. Today** | COP decodes; CPU serves calls + DSP | ~18.5 MHz-eq on two cores | ~82/s + 100/s COP pulse | 24 MHz steady | baseline |
| **B. CPU decode, steady 24 MHz, COP parked** (recommended) | the codec thread decodes on the CPU (upstream's path, `codec_cop_select()` off for the load); the COP is parked (§3.4) | ~13 MHz-eq on one core (~55 % at 24 MHz; FLAC fits, ~45 % headroom) | ~10–20/s; COP 0 | 24 MHz steady; `LOAD` hold → 30 only for formats that don't fit | **−5 MHz of active cycles** and a parked core: **~0.5–3 mA** [estimate: ARM7TDMI-class core plus SDRAM traffic at 1.2 V; ±3× uncertain; M-1 decides] |
| **C. CPU decode in bursts at 80 MHz (race to idle)** | decode until pcmbuf is full at 80 MHz, then sleep at 24 MHz | same cycles, compressed | fewer, longer sleeps (~1–2 bursts/s) | PLL on for ~16 % of the time (13/80) | **worse than B** [estimate]: the same cycles at the same voltage, plus PLL power and a lock (16–237 µs [silicon]) per burst, with no lower idle floor to race to. It only helps if the PLL's power is negligible **and** a longer sleep lets something else power down (nothing does: DMA and the DAC stay on) |
| **D. CPU decode in bursts at 24 MHz** (an add-on to B) | B, plus decoding until pcmbuf's high watermark, then a deadline wait until the low one | same as B | **~2–4/s** | 24 MHz | B's gain plus fewer thread switches; it costs pcmbuf headroom on a skip (bounded by the existing watermark) |
| E. Lit: keep the COP decode | as A, lit only | — | — | 30 instead of 80 with a heavy format during browsing | Fit-2's real win. **Moot once the UI is on the COP:** decode is then on the CPU and the UI on the COP in every state |

### 4.3 Recommendation

**B + D: decode on the CPU at a steady 24 MHz, in watermark bursts, with the COP parked, for every dark-playable format that fits** (MP3, FLAC, ALAC, WavPack, PCM).
- Heavy formats (AAC, Vorbis, WMA, Opus) take the `LOAD` hold to 30 MHz as today.
- Opus may need a boost on one core. That needs an M-2 measurement per format, so it is kept as an exception row in `clockpol`'s table.

**Why:**
- the fewest active cycles measured;
- one core, which is exactly the owner's dark-playback picture;
- no new mechanism (upstream's codec path is the fallback that already exists);
- it removes ~80 wakes/s.

**Interim (before the UI moves to the COP):** the codec core is chosen by screen state: lit → COP (Fit-2's lit win), dark → CPU.
- **A loaded codec stays loaded across tracks of the same type** (`codec_thread.c` :470–511), so "at each codec load" alone would leave a FLAC queue started dark on the CPU through hours of lit browsing (P19 review H5). The re-selection points are: playback start, a codec change, and a forced re-select at the first track boundary after the screen state has changed and held for ≥ 30 s.
- The forced re-select must keep gapless playback: a switch of the decode's core at the boundary without a codec reload if `codec_cop.c` allows it; otherwise only at boundaries that are not gapless (an album change).
- While the CPU decodes with the screen lit (until the next re-select point), lit responsiveness relies on the existing boost and the codec thread's priority; M-2's lit stretch and the lit-24 round (B8) report the boost time.
- No mid-track migration.

**The device A/B that decides it** (prerequisite M-2; the `hardware.md` A/B method):
- **Setup:** a fixed FLAC album, Hold on, dark, a fixed volume, a 5 s power log, the same voltage band (3,635–3,955 mV, the flat 40–80 %).
- **Arms:** 30-minute arms in the order A B C A B, with `powerlog_mark` tags:
  - **A:** COP decode (today);
  - **B:** CPU decode with the COP parked (needs K-1; without K-1, run B as "COP decode off" with the pulse still running, and record that);
  - **C:** B with an 80 MHz burst arm (a developer row).
- **Read:** mV/h with `idle_pct`, `cop_idle_pct`, `disk_pct` and `stor_on_pct` matched.
- **Decision rule:**
  - B ships for dark if its mV/h is at or below A's in both repeats.
  - C replaces B only if it beats B by more than the A–A repeat spread.
  - With the USB-C meter (optional), each arm's mA difference replaces the estimates in §4.2.
- Repeat with one AAC album for the `LOAD` formats.

### 4.4 Lit Now Playing lags with MP3 since 19-1c (np-lag, 2026-10-25)

**Report** (owner, build `799ff0f8d7-261026`, developer): Now Playing is slow to follow the wheel while music plays, "probably an MP3"; with FLAC "definitely more responsive by far".

**Causes, ranked** (numbers are [estimate] unless tagged):

1. **CPU contention between the MP3 decode and the UI on one core.** Since 19-1c the whole MP3 decode runs on the CPU, synthesis inline, on the same core as the UI thread.
   - **The decode's share.** MP3 all on one core is 12–16 MHz of decode plus 2–4 MHz of DSP, so **14–20 MHz: 47–67 % of the lit 30 MHz** (58–83 % at 24 with `lit24`). That is Fit-2's table (§6, upstream's PP5022 figures as remembered). 320 kbit/s sits at the top of the range.
   - **The calibration point.** FLAC on the CPU measured 13 MHz-eq (55 % at 24 MHz, §4.1 [silicon]) against the table's 10–16, so the table is in range. FLAC therefore leaves the UI roughly a third more of the core than MP3 does, which fits the owner's A/B.
   - **What the UI needs.** In lit browsing the UI needs up to ~20 MHz at peaks (Fit-2 §7; `main` 8–24 % lit on average, §1.3 F1 [silicon]). MP3 plus the UI is **34–40 MHz on a 30 MHz core**. Before 19-1c the CPU carried only the call server and DSP (~2–3 MHz [silicon], §4.1), which left the UI ~27 MHz.
   - **Why the UI is the one that loses.** The codec's band (13) is above the UI's (16), and the kernel is cooperative. While the codec is runnable, the UI runs only on the scheduler's aged turns: after 2 ticks (20 ms) of waiting, plus the codec's current slice (Fit-1 B.3). Every wait inside a UI frame (an LCD push, a lock) costs another ≥ 20 ms.
   - **How long the codec is runnable.** With 19-1c D the codec decodes in bursts from the low watermark to full. At a 47–67 % share the buffer fills at 0.5–1.1 s of audio per second, so **the codec is runnable 47–67 % of the wall time**, and an input lands inside a burst about half the time or more.
   - **The clock does not help today.** The wheel in Now Playing (volume, scrub) draws without a boost. Only glides and slides boost (`ui_anim_begin()`). `clockpol`'s table still calls MP3 "light" from the time its synthesis ran on the COP, so lit playback stays at 30 MHz.
2. **Cooperative hogs on the CPU** (any format, secondary). `m0 slice max` lines show `artcache` running 68–166 ms and `main` 72–88 ms in one slice [silicon, bench-20261025-2257]. Both were while not playing. The art builder stays off for 2 s after any input (`artcache_note_input()`). A pass that is already running can still hold the core for one slice.
3. **BCM job passes on the UI thread** (drift, stepped PAN marquee, `art_bcm_service()`, the owner passes). They are format-independent, so they don't explain the MP3/FLAC gap. The BCM-resident panel (in progress) moves the panel's decode and read-back off the UI thread.
4. **19-1a's quiet pollers: not a cause.** Input is posted to the button queue by the wheel's interrupt and wakes the UI at once. No poller sits between the wheel and the UI.

**Stopgap (branch `np-lag`; forward-compatible, no placement rollback).** A lit input boosts the PP while the codec decodes on the UI thread's core and something plays (`clockpol_note_input()`, decision `cp_input_boost()`, host-tested).
- The boost is held 1 s after the last event and released on the main loop's next pass (`clockpol_input_wait()` bounds its sleep). It is dropped at once before a system event's handler (USB, sleep, power-off).
- **Why it works.** At 80 MHz the MP3 decode is 18–25 % of the core. Its burst ends ~2.7× sooner, and the codec then sleeps on its full buffer (D). The UI gets the core instead of aged turns.
- **Dark:** never. No extra wake in dark playback (19-1a): a hold is only taken lit, and its one release wake falls inside the backlight's on time.
- **Audio:** unchanged. The codec keeps its band. Raising the UI above the codec was rejected: at 30 MHz the core is oversubscribed, so the decode would starve instead, with only pcmbuf's watermark rescue (and a broken Fit-1 band invariant).
- **Cost** [estimate]: 80 MHz instead of 30 for the seconds of interaction plus 1 s each time. Each probe window reports the held time (`ib=`). Code is ~0.3 KB (clockpol) plus 24 B of bss, and nothing in IRAM.
- **After 19-9** (the UI on the COP) the decode is on the other core, `decode_on_ui_core()` is false, and the boost never fires. Nothing to remove.

**19-9 is the structural fix.** The UI gets a core of its own at 30 MHz with no shared scheduler, and the codec is alone on the CPU (§2, step 9). The owner has pulled it forward. The stopgap covers the gap until then, and the `HAVE_COP_FRONTEND`-off fallback afterwards.

**Developer probe** (`apps/ipodui/uilat.c`; `HAVE_DEVSTATS` device builds only; three `uilat` lines per 60 s window in which Now Playing was lit on top and saw a wheel event or a frame):
- `np n= avg= max= q=QA/QM o33= o100=` gives:
  - wheel-to-pixels ms, from the driver's post (`button_wheel_post_usec()`) to the end of the next LCD push;
  - the queue wait alone;
  - the events over one frame slot and over 100 ms.
- `cpu codec= main= idle= s=` gives the window's CPU share of the codec and UI threads, and the core's sleep (devstats).
- `fr n= late= ib=B/Tms` gives the Now Playing animation frames and the late ones (a whole slot past due), plus the input boosts and their held time.

**Device check:** the same MP3 and FLAC tracks, lit Now Playing, wheel volume up and down for a minute. Read `uilat` lines:
- MP3: `codec=` around 47–67 % with no input and well under that when boosted, `o100=` near 0;
- FLAC: a lower `codec=`.

The pre-fix behaviour can't be reproduced without a switch, so compare against a `799ff0f8d7` build if a before number is wanted.

---

## 5. Wake budget per state

Baseline: §0 (dark FLAC ~88–95 thread wakes/s, 100 tick IRQs/s on the CPU, and 100 pulses/s on the COP).

| Thread / source | Dark play today | **Dark play target** | Lit browse target | Paused dark target | Soft sleep | How |
|---|---|---|---|---|---|---|
| kernel tick (CPU) | 100 IRQ | 100 IRQ (tickless later, §5.4) | 100 | 100 | 0 (timer poll 1/60 s) | — |
| COP pulse | 100 | **0** | as needed | 0 | 0 | K-1 (§3.4) |
| `codec cop` / `codec` | 38 / 44 | **— / 2–4** | — | — | — | §4.3 B+D |
| `audio` | 2 | **0 between tracks** | 0 | 0 | — | deadline wait: the next track's handle event, a skip, or the watermark |
| `ata` (storage thread) | 2 | **0, plus 1 at the power-off deadline** | 0 + deadline | 0 | — | deadline = `min(spindown, stor_pwr off, idle callbacks)` |
| `tagcache` | 1 | **0 in dark play**; 1/s only while work is owed | ≤ 1 while owed | window only | — | the 18-A "not done" list: kick each producer (`ui_drain_kick`, command queue, path queue, precompute request, dbq owed) and make the maintenance window's open time a deadline |
| `power` | 0.5 | **≤ 0.1** (10 s step while playing dark above 15 %, no charger) | 0.5 | 0.1 | poll | voltage moves ~14 mV/h in dark play [silicon], so a 10 s step loses nothing to the 64 s filter; charger, USB and kick edges stay instant |
| `buffering` | 0.03–0.07 | **= drive bursts** (≤ 6/h) | same | 0 | — | §5.2 |
| `main` (UI) | 0.017 | **0 on the CPU** (UI on the COP, parked) | ~6 (COP), 0 at rest with the drift on the chip | 0 | — | the unlit face doesn't draw while dark |
| `backlight`, `scroll`, `piezo` | 0 | 0 | timers only; piezo per click | 0 | — | 18-A, unchanged |
| PCM DMA FIQ | ~21.5 | ~21.5, or **~5.4** with 32 KB chunks | — | 0 (DAC standby) | — | [hypothesis] a larger `PCMBUF_CHUNK_SIZE` for dark play only; prerequisite M-6 (underrun risk at a skip) |
| **Thread wakes total** | **~88–95/s + 100 COP** | **≤ 5/s, COP 0** | — | **≤ 1/s** | **1/60 s** | |

### 5.1 Lit wake budget

| Source | Today | Target |
|---|---|---|
| Drift | 13.3 pushes/s and ~20 wakes/s on the UI core | **0 while a cover pans** (`ANIM PAN` on the chip). 1 per cover change (≤ 1 per 7 s) to `ALLOC`/upload/`FADE` |
| Marquee | 20/s | **0 while panning**: one strip render per title, then an `ANIM`/`BLIT` window (fb: the COP pans a pre-rendered strip, F2) |
| NP clock | 2/s | 1/s, one `PRESENT` of the clock rect, merged with the play-glyph deadline |
| Header clock | per minute | merged into the NP or face deadline |
| Glides | 30 fps boosted | 30 fps; rows rendered on the COP; `SCROLL` and `PRESENT` on the chip; unboosted is the target once 3d measures `J_T_DRAW` |

### 5.2 Drive wakes (the largest controllable dark-playback item)

- **Measured:** 22 power-ups an hour, ~15 s powered each, `stor_on_pct` 9.8 % [silicon].
- *(Measured on the 10 s build; since 698181ffbf the dark hold is 3 s, counted from the last access, and p18-refill removed the track-change and stats-drain power-ups.)* About 10 s of each cycle is `STOR_PWR_HOLD_DARK_S`, the idle hold after the last access. The refill itself moves ~10 MB (1,964 KB/30 s × 2.7 min) at ~21 MB/s, which is **~0.5 s of real transfer** [estimate]. So the drive is **~95 % idle-powered while it's on**.
- **Wake attribution comes first** (M-3). `m0 disk wake by=audio rd n=32` lines say many power-ups come from the `audio` thread at track changes [silicon], not from the watermark. With ~44 MB of buffer and FLAC at ~65 KB/s, the buffer holds ~11 minutes [estimate], so the watermark alone would cause ~5–6 refills an hour.

**Targets:**
1. **Shorten the dark hold** to ~2 s after a refill that reached its target level. Keep 10 s after a partial or failed one, and keep the doubling rule.
2. **Fill the buffer to its ceiling in one burst**, loading whole tracks ahead through the queue, including their metadata and art handles. Then a track change reads nothing.
3. **Piggyback all writes:** resume saves, the stats drain, logs and the latch on the refill's power-on (already the storage-idle rule; keep it).
- **Target:** ≤ 6 power-ups/h and ≤ 1 % `stor_on_pct` for FLAC on 64 MB [estimate]. The 32 MB variant gets ~3× the refills at the same rule.
- **Estimated saving:** if #114's power-off is worth ~65 mA at 100 % → ~0 % (§0), the adapter's powered-idle draw is of that order. Cutting 9.8 % to ~1 % would then save **~5 mA** [estimate; the largest single item left]. Flash-tuned values go in `docs/storage-choices.md` (rule 7); the HDD keeps its spin-up-aware hold.

### 5.3 How wakes merge

1. **One deadline per thread**, the minimum of its sources (18-A's pattern), never a fixed poll.
2. **Ride the refill:** storage-idle callbacks run in the power-on window that a refill opens (`register_storage_idle_func`); nothing opens the drive on its own in dark play except the refill, the alarm record, and the 0 % latch.
3. **Ride the track change:** the audio thread's per-track work (resume checkpoint, next-track prep, scrobble/stat line) is batched into the transition it already wakes for.
4. **Coarse timers in dark:** the power step at 10 s and the batcal sampling inside it. The batcal 1 Hz timeout becomes a read at the power step (its 1 s samples already read a 2 s-old value, per `hardware.md`).
5. **Front-end deadlines don't exist in dark.** If the screen is off, the COP has nothing to draw. A UI timer that has to fire in the dark (alarm, sleep timer) is a back-end deadline by rule 1.

### 5.4 Later: a tickless CPU in dark play

The 100 Hz tick is the remaining floor: 100 IRQs/s plus the tick tasks (button, ADC, USB debounce).
- **Dynamic tick** (program Timer 1 for the next deadline while every thread waits) would cut it to roughly the DMA FIQ rate.
- It touches `firmware/kernel`, every tick task and the button repeat logic: **a large, risky change.** It should be considered only if M-1 shows the tick costs ≥ 1 mA. Out of scope for P19's first round.

---

## 6. Smoothness guarantees

| Guarantee | Target | Which placement keeps it | Risk and mitigation |
|---|---|---|---|
| **Never block the front end** | the UI core never waits on storage, allocation or a back-end lock | the UI on the COP and the §3.2 moves. The front end's only waits are its input queue, its deadline and a `FENCE` (≤ 20 ms + its jobs, `jobs.md` §6) | a COP that waits on a CPU lock held through a disk read. Rule: the front end takes no back-end lock; it reads published state |
| **Wake to first frame** | ≤ 0.5 s | from C0, measured today: **0.37 s** cold start (hello_dma) and **436–458 ms** from upload to first frame (S7) [silicon]. The planned path [bcm estimate], **~0.32–0.34 s**: power, bootstrap and the image over E2 (~19 ms, P18 18-3); ready (~2 ms); kick `LCD_POWERON` (panel waits 265–280 ms [silicon S30]); **upload the framebuffer over E2 inside those waits** (~14 ms); first push 14.3 ms. Slots are re-created lazily (power contract §3) | the one overlap that must hold is the frame upload inside the panel's waits. Without it, the S7 path is at the edge. C3 standby would save ~130 ms more, if 5s proves it (owner Q-3) |
| **NP clock never skips** | every second's digit shown, lit | the COP draws one rect per second from the published elapsed (no back-end call); the chip `PRESENT`s it; the drift and marquee are off the PP so the COP has slack | a COP frame stall > 1 s. The deadline is computed from elapsed, not counted: a late tick draws the right second |
| **Scroll ≥ 30 fps** | glides and quick scroll | rows rendered on the COP (230–610 k instructions per frame at 30–80 MHz [measured]); `SCROLL`+`PRESENT` on the chip; fb: today's boosted path | the COP's per-frame row render at 24 MHz unboosted is unknown: keep the boost on glides until M-4 (3d's `J_T_DRAW`, #55) shows the budget |
| **Gapless audio, no underrun** | 0 `pcm underrun` lines | the back end alone on the CPU; the UI can no longer delay a codec call (fit2 §3.4's cooperative-scheduler risk disappears in dark, and while lit the codec thread no longer serves cross-core calls) | a CPU starved by a heavy format and a lit glide's `pcmbuf_insert`: the `LOAD` hold and pcmbuf's boost watermark rescue it as today |
| **Tap to sound** | ≤ 0.5 s | unchanged (`m0 tap2pcm` 128–182 ms skip, 363–457 ms resume [silicon]) | a parked COP adds nothing, since decode is on the CPU |
| **Chip failure** | the screen stays right | `jobs.md` §6: the frame is finished on the fallback, the chip is restarted by the boot ladder, and it's logged. The drift falls back to the moving CPU drift (2026-10-25, after build A's bench showed a frozen cover); owner option (b), the still cover, remains only for when the PP has no slot memory for the CPU drift (after §7's move), with Cover Flow off with the pop-up | — |

---

## 7. Memory

Under the BCM memory contract (`memory-contract.md` §5, owner option b):

| Buffer (from the audio buffer today) | Today | After P19 | Saved |
|---|---|---|---|
| Drift cover slots | 360,000 B (64 MB) / 180,000 B (32 MB) | 0 (`J_CLS_DRIFT` on the chip; fb: a still cover read from `panels.pak` into the framebuffer) | 180–360 KB |
| Strip | 184,000 B | ~40 KB (one row band) | ~140 KB |
| Cover Flow textures (P7, planned) | +512 KB planned | 0 (`J_CLS_COVERFLOW`, BCM SDRAM; fb: disabled) | 512 KB never reserved |
| Resident image copy | 203–207 KB | ~221 KB | −14 KB |
| **Net** [bcm estimate] | | | **~310 KB (32 MB), ~490 KB (64 MB)**, plus Cover Flow's 512 KB never taken |

**What the placement adds** [estimate]:
- **A COP UI thread stack.** The main thread's 8 KB IRAM stack (`app-pp.lds`) stays with the CPU's back-end main. The COP UI needs its own ~8 KB (`hardware.md` "Thread stacks with a splash": 4,144 B worst path + 400 B panic tail, so 8 KB has margin). It goes in **IRAM if it fits** (IRAM is uncached and shared, 45,808/49,152 B used, so **it does not fit beside today's users**) or in cached DRAM from the shell's pinned block. **DRAM it is: +8 KB of the floor.**
- **`SHAREDBSS` "now" record and generation words:** ≤ 256 B.
- **The service queue (§3.3):** ≤ 512 B.
- **The CPU decode in dark** reuses the codec slot. The COP worker's 9 KB stack sits at the top of the codec slot and is simply unused.

**Floors:** ≥ 7,500,000 B (32 MB) and ≥ 39,000,000 B (64 MB).
- Arithmetic minimums after npfix: **11,558,368 / 44,391,824 B** (`hardware.md` budget table; the floor gate to re-measure).
- After P19's net savings: about **11.86 MB / 44.87 MB** [estimate].
- The floor gate (`tests/perf/floor_gate.sh`) must add the case "chip failed, drift on its still-cover fallback, while playing" to prove option (b) takes no RAM back.

---

## 8. Migration order

| Step | Move | Prerequisites | Fallback |
|---|---|---|---|
| **0** | **Measure** | M-1 per-state current with the meter, or voltage A/Bs (§8.1); M-3 drive-wake attribution | — |
| **1** | **Quiet back end:** `audio`, `ata`, `tagcache` and `power` to deadline waits (§5); batcal into the power step | the 18-A pattern; the tagcache producer list (`hardware.md` "Not done, and why") | each thread keeps its old timeout under a developer row for an A/B |
| **2** | **Drive policy:** dark hold ~2 s after a full refill; whole-track fill to the ceiling; track-change reads batched into the refill | M-3 | the hold constants back to 10/60 s (`stor_pwr.h`) |
| **3** | **Codec core by screen state** (interim): dark loads decode on the CPU | M-2's A/B (§4.3) | `codec_cop_select()` default on, as today |
| **4** | **K-1: park the COP** (no tick pulse, explicit `core_wake(COP)` on posts) | a host test; a device soak with the COP decode on (posts must wake it) | the per-tick pulse (one `#define`) |
| **5** | **L2 + the drift on the chip** (BCM plan item 1) | `verified.txt` entry after stages 0–5 on the device; 5b's A/B; the merged image (`memory-contract.md` §6) | the CPU drift while the PP still holds slot memory; still cover (owner b) once it does not |
| **6** | **Lit at 24 MHz** (`CP_LIT_STREAM_HOLD` = 0) | step 5 (the drift no longer pins the PLL); `lit-24mhz.md` §5.2's frame-budget round | the developer switch back to 30 |
| **7** | **Retained screen:** strip, slides, glides, marquee window (plan item 2–3) | 3d (#52 TE, #55 cost per pixel), the job executor bit-exact in vc2sim | the COP/CPU paths as today |
| **8** | **Back-end services off the UI thread** (§3.2) | the Class A deny list running in `modpak.py check` (P12) | none needed: the same code on another CPU thread |
| **9** | **The UI to the COP** | steps 4, 8; M-5 (drop cost); the buflib pin rule (§3.1); a COP UI stack | **a build flag** (`HAVE_COP_FRONTEND`): the UI back on the CPU's main thread, the codec back on the COP while lit (today's arrangement) |
| **10** | **Art decode on the chip** (`J_OP_DECODE`) while lit or charging | 5s power numbers; the vc2sim spike (plan item 3) | the CPU art builder, unchanged |
| 11 | Cover Flow on the chip (P7) | step 7, SDRAM power figure (#7) | disabled + the "not available" pop-up (owner) |
| 12 | (if M-1 says ≥ 1 mA) a tickless CPU in dark | everything above | the 100 Hz tick |

Steps 1–4 reach "dark playback = one core, no BCM, drive in bursts" **without moving the UI**. Steps 5–7 are the lit savings. Step 9 is the owner's full architecture, and is the riskiest, so it comes last, behind a flag.

### 8.1 Device measurements this plan needs (adds to the BCM bench list)

| # | Measurement | Decides | How |
|---|---|---|---|
| M-1 | Current per state: dark play today; BCM C0 vs C2; drive on-idle vs off; COP parked vs pulsed; tick; Hold-on wheel | the split of the ~35–40 mA; which of §5's items matter | USB-C meter state differences with the battery full (`hardware.md` "USB-C meter calibration"), or 30 min voltage A/Bs |
| M-2 | Codec A/B (§4.3) | B vs A vs C | power log, 5 s, ABCAB |
| M-3 | Drive-wake attribution in dark play | §5.2 policy | `m0 disk wake by=`, `stor pwr on reason=`, per-track times |
| M-4 | COP row render at 24 MHz | glide boost | the shell's frame counters under the COP front end |
| M-5 | Whole-cache write back + drop cost, per core | the generation hand-off's rate | a timed loop in the sleep lab |
| M-6 | Larger PCM DMA chunks in dark play | FIQ rate | underrun count at skips with 32 KB chunks |
| (bcm) | 5s, 5i, 3d, #12, #50, #49 | `power-contract.md` §5 | BCM session's stages |

---

## 9. Open questions

### 9.1 BCM session answers (2026-10-23, `p19-bcm-prep` `c1634393d5`; its tags)

| # | Question | Answer |
|---|---|---|
| 1 | Chip cost per job at 27 MHz | [estimate, S7 and the sim]:<br>• **drift half step:** 2.87 ms of compute, plus 8.6 ms of LCD bus for a 160×240 push (core free);<br>• **marquee pan** of a 320×24 window: ~0.04 ms vectorised plus 1.4 ms of bus (~2.8 ms with the scalar executor);<br>• **slide frame** (320×220): no compute, two DMA pushes at offsets, ~13 ms of bus;<br>• **`SCROLL`** (320×220): 0.34 ms vectorised but 25 ms scalar, so **it must be vectorised**.<br>On the PP, a record is 8–12 words, ~20–25 port transfers, **under 10 µs** at 30 MHz, against 13.4–15.2 ms for today's 160×240 CPU push. PP time saved: the drift at rest about 200 ms/s (20 % at 30 MHz); the marquee 15–38 % (or ~5 % after F1/F2: to reconcile); the glide and slide boosts and their 14–27 ms E2 pushes; NP's 1 Hz bar negligible |
| 2 | Lit panel with the chip cut | **Probably not** [hypothesis]. The chip drives GPIO 48 (panel reset) and the panel bus, and the panel's supply path is undocumented. A 10 s bench test is on its list. The realistic lit-idle saving is **C2 with the core asleep** (after 5i); the panel refreshes from its own memory between updates [silicon] |
| 3 | Wake in under 0.5 s | ~0.32–0.34 s planned path (§6); the frame upload must overlap the panel's waits |
| 4 | Lit at 24 MHz | **Yes**, once the drift and marquee are on the chip [estimate; the lit-24mhz round decides]. E2's rate at 24 MHz is unmeasured; assume ~20 % slower (like the port), fine for 40 ms deadlines |
| 5 | COP as the front end | no protocol obstacle; three constraints (§3.5) |
| 6 | BCM wakes in dark play | **zero with the chip cut** (§3, rule 2) |
| 7 | Currents [hypothesis, unmeasured #12] | FS#9890's "> 30 %" for cutting chip and backlight together suggests **chip + panel ~10 mA** at 27 MHz while busy-polling. The core alone is plausibly 2–5 mA at 27 MHz and 10–25 mA at 150 MHz. C2 asleep is plausibly < 1 mA for the core (panel unknown). C0 ≈ 0. **Dark play budgets 0 mA for the chip** |

**What this means for the plan:**
- The dark budget's chip share is 0.
- The lit-idle saving from C2-asleep (after 5i) may be a few mA [hypothesis], which makes stage 5i the lit state's most valuable BCM measurement.
- `SCROLL` must be vectorised before glides move to the chip.

### 9.2 For the owner (each with the default the plan follows until answered)

| # | Question | Default |
|---|---|---|
| **Q-1** | Dark playback decodes on the CPU and parks the COP (§4.3), reversing Fit-2's default **for dark only**, if M-2 confirms it? | **Yes, once M-2 confirms;** lit keeps the COP decode until the UI moves (step 9) |
| **Q-2** | The UI on the COP (step 9) is the riskiest change: every UI data path becomes cross-core. Build it last, behind a flag, and drop it if M-1 shows the lit-state gain is under ~2 mA? | **Yes:** steps 1–8 first; step 9 behind `HAVE_COP_FRONTEND`, kept only if it measures |
| **Q-3** | Waking from a cut chip measures 0.37–0.46 s today and is planned at ~0.33 s (target 0.5 s). Also hold the chip in standby (C3/C4) for `BCM_STANDBY_WINDOW_S` after the screen goes dark, to wake ~130 ms faster, if 5s proves it's cheap? | **The cut, with the frame upload overlapped with the panel's waits** (~0.32–0.34 s planned [bcm estimate], under budget); the standby window (~130 ms faster) only if 5s shows it's cheap |
| **Q-4** | Lit at 24 MHz once the drift is a chip job (steps 5–6)? | **Yes**, after `lit-24mhz.md` §5.2's frame round |
| **Q-5** | Dark-play drive hold of ~2 s after a full refill (from 10 s) and whole-track fills (§5.2)? | **Yes**, flash only (`storage-choices.md`); HDD unchanged |
| **Q-6** | Power down the clickwheel controller while **Hold is on** in dark playback, waking on the Hold-off edge? It saves an unknown current and costs a re-sync at Hold-off [hypothesis] | **Measure first** (M-1); no change until it's shown to be ≥ 0.5 mA |
| **Q-7** | Art building and database maintenance never run in dark playback on battery: they wait for lit, a pause window or the charger? | **Yes,** except a just-synced library the user is waiting on (rule 6) |
| **Q-8** | A tickless CPU (§5.4) only if the tick measures ≥ 1 mA? | **Yes** |

---

## 10. Device-reality checklist (for the steps above, as they're built)

| Item | Answer for this plan |
|---|---|
| Compiled features | No new features. K-1 and `HAVE_COP_FRONTEND` are new flags; the stock build is unchanged |
| Memory during playback | −310/−490 KB (BCM moves) +~9 KB (COP UI stack, shared records); floors kept with ~4 MB / ~5 MB headroom [estimate] |
| CPU at 50k | unchanged: library work stays on the CPU in windows; view builds become CPU jobs (§3.2) |
| Storage wake-ups | dark target ≤ 6/h (from 22/h); no new opener |
| LCD | lit pushes move to chip jobs; dark: C0 |
| Threads and locks | one PP-side BCM ring lock (Peterson); no new back-end lock visible to the front end |
| Splashes | none; chip failures logged (`jobs.md` §6) |
| Persistence | resume saves piggyback on refills, as today |
| Scale | the generation hand-off is O(1) per publish at 50k |
| Stock firmware | untouched; Apple's firmware stays the recovery path |
| Device-only checks | M-1…M-6 and the BCM stages; nothing in this document is proven on silicon except the rows tagged [silicon] |
