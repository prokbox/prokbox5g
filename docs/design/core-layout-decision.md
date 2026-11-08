# Core layout: align with Apple, or finish the UI-on-COP reversal?

*Design study, 2026-11-05, owner-requested. Branch `design-corelayout`, from `p19` `190c533450` with `research-applesleep` `fc897438e7` merged. Docs and one cheap size prototype only; nothing written to a device. Names: the **CPU** and the **COP** are the PP5022's two cores (PP1 and PP2 in conversation), the **BCM2722** is the video co-processor (VC), the **WM8758** is the DAC.*

**Tags** (as in `docs/pp5022-reference/index.md`):
- **[silicon]**: measured on our unit, with the log or sitting named.
- **[apple-RE]**: read by us from Apple's `osos` 1.3 (`apple-power.md`, `apple-dual-core.md`).
- **[rockbox-derived]**: from Rockbox's code or history.
- **[code]**: read in this tree.
- **[build]**: measured from a build made in this study.
- **[estimate]** and **[hypothesis]**: as they say.

---

## 1. Summary and recommendation (plain English)

**The question.** Apple's firmware runs the menus, the file system, the disk, USB and the video chip on the main core, and gives the second core one job: an audio engine. The second core sleeps until there is audio work, and Apple routes the audio DMA interrupt to it. Our current plan goes the other way: it moves the UI to the second core (19-8/19-9) and keeps playback on the main core. Should we switch to Apple's layout?

**What this study found.**

> **Superseded for 0.8.0-rc.6 (owner, 2026-11-07):** the audio output is on the COP too. The PCM DMA's FIQ, the refill, the mixer and pcmbuf's consumer run there from boot in every shell build, proved at each boot with a logged CPU fallback (`apps/audio_cop.c`; `cop-shared-state.md`, layout A). This goes past Apple, whose FIQ stays on the CPU (§ below).
>
> **Superseded for 0.8.0 (owner, 2026-11-07):** decoding is on the COP by default in every shell build, the release included (`codec_cop.c` `place_mode = CPL_MODE_COP`, Fit-2's per-run job; Apple's core layout for the audio computation). The CPU keeps the UI, file system, disk, USB and the BCM driver. A COP that will not take a run falls back to the CPU with a "COP FAILED" log line. The findings below describe the tree as studied (2026-11-05); `docs/changes-from-rockbox.md` 2.67 has the change.

1. **Today's default is neither Fit-2 nor Apple's layout.** Since 19-1c (owner, 2026-10-24) decode is pinned to the CPU (`codec_cop.c` `place_mode = CPL_MODE_CPU`), and the UI is inline on the CPU (`IPODUI_UI_CORE_DEFAULT 0`). So **everything runs on the CPU**, and the COP does nothing except get woken 100 times a second by the tick (K-1 has not landed) [code]. Fit-2 (decode on the COP) survives only as a developer A/B mode.
2. **The UI move was never needed for dark-playback power.** In dark playback, plan B (the UI on the COP) and today's layout are identical: the decode runs on the CPU and the COP has nothing to do. `p19-placement.md` §8 already says it: steps 1–4 reach one-core dark playback "without moving the UI". **19-9 exists to fix lit smoothness**: Now Playing lagged once the MP3 decode shared the CPU with the UI (np-lag, §4.4).
3. **Apple's layout fixes the same lit problem from the other side, and it is cheaper to keep correct.** Moving the *audio* off the UI's core isolates the UI as well as moving the *UI* off the audio's core does. But the audio pipeline crosses cores through about 30–50 functions, mostly control messages. The UI crosses through **238 remote-call stubs**, and each call is a whole-cache sync point on both cores. On top of that, ~110–180 cached statics are still unrouted, and two silicon faults have come from it so far: the replayed click tone in sitting 3, and the false-sharing OOM panic in 4f.
4. **"The CPU can't sleep" was wrong, and not for a layout reason.** Rockbox's CPU already sleeps in `PROC_SLEEP` between interrupts (`core_sleep()`), exactly as Apple's idle loop does (`apple-power.md` §5.1). Apple also keeps both timers running while awake, and stops them only in light sleep [apple-RE]. Where Apple differs is *what work each core has*, not *how the cores idle*.
5. **The PCM interrupt can be routed to the COP.** Quirk 55 ("the PCM FIQ is CPU-only") describes Rockbox's wiring, not the silicon: each core has its own enable and FIQ-select registers, and Rockbox's COP already has a FIQ stack. (**Corrected 2026-11-06:** Apple does *not* route source 26 to the COP; its audio DMA interrupt is the CPU's FIQ, so the COP route is our own extension; `../pp5022-reference/apple-cop-audio.md` §0.) It is unproven on our unit; probe P-1 (§5) settles it in one bench row.
6. **Dark-playback power is about the same in every layout** (one core decoding at 24 MHz, the other asleep) **if** the cross-core path carries no per-frame calls. Fit-2's measured ~40 % cycle overhead came from uncached bitstream and PCM reads plus ~40 calls a second. Apple's layout removes both: the PCM never leaves the COP, and the bitstream is read cached with one drop per refill.

**Recommendation.**
- **Target: (A), Apple's layout.**
  - The CPU keeps the UI, the file system, the disk, USB, the BCM2722 driver and the playback control thread.
  - The COP runs the audio engine: decode, DSP, the mixer, `pcmbuf` and the PCM DMA interrupt.
  - The COP is woken only by work; the CPU sleeps between interrupts, with a coarse tick in dark play later, if measured.
- **19-9 is retired as the target.** Its default is never flipped. The split machinery stays in developer builds only until (A) lands, then it goes.
- **0.8.0 ships (C) as it is today**: the UI inline on the CPU, decode on the CPU, and the np-lag input boost. Two changes:
  - **19-9 comes off the 0.8.0 gate**;
  - **`HAVE_UI_THREAD_SPLIT` and the `--wrap` stubs are compiled out of the release build.** That frees **10,880 B** of `_end` [build] in a build that sits within a few KB of the 1,500,000 B cap and never runs the split (default 0).
- K-1 (park the COP) lands in either case.
- This is a forward move to a better-fitted target, not a rollback: the UI never shipped on the COP, and (A) reuses Fit-2's silicon-proven COP decode (S36, over an hour).

---

## 2. The three layouts

| | **(A) Apple-aligned** | **(B) Current target (19-9)** | **(C) Today, as shipped** |
|---|---|---|---|
| CPU | UI, FS, disk, USB, BCM2722 ring and port, `audio`/`buffering`/`tagcache`/`power` threads, the tick | playback back end: decode, DSP, PCM FIQ, buffering, storage, `be_svc` (238-stub call server), power transitions of the BCM2722 | everything: UI inline, decode, DSP, PCM FIQ, storage, BCM2722 |
| COP | **audio engine**: codec, DSP, `pcmbuf`'s producer side, filling a PCM ring. **Revised 2026-11-06 (owner): the hybrid is the target**: the CPU keeps I2S, DMA0, the source-26 FIQ and the mixer, re-arming 64 KB runs; the FIQ on the COP is the extension X1 (`layoutA-engine-brief.md` §6, §12) | UI front end (`ui` thread, 8 KB stack), BCM2722 job ring | idle; pulsed 100/s by the tick (K-1 not landed) [code] |
| Dark play | COP decodes; CPU sleeps between its tick and refills | CPU decodes; COP parked (after K-1) | CPU decodes; COP pulsed (parked after K-1) |
| Status | design (this study); Fit-2's COP decode proven on silicon (S36) | built behind a switch; ucop2 FAILED in sitting 3, the 4f boot panicked (false sharing) [silicon] | shipping (0.7.x, p19) |

Fit-2 (the decode on the COP with a CPU call server for every playback callback, the DSP and the FIQ on the CPU) is the halfway point between (C) and (A). It is the developer mode `CPL_MODE_COP` and is used below as (A)'s measurable upper bound.

---

## 3. Comparison

Numbers are [estimate] unless tagged. "MHz-eq" = a share of a core at the clock named.

| Criterion | (A) Apple-aligned | (B) UI on the COP | (C) Today |
|---|---|---|---|
| **Dark play: cores awake** | COP: decode in watermark bursts + FIQ. CPU: `PROC_SLEEP` between the 100 Hz tick, drive bursts and ≤ 0.1/s power steps | CPU: decode + FIQ + tick. COP parked | same as B, COP pulsed until K-1 |
| **Dark play: active cycles (FLAC)** | ~13 MHz-eq on the COP (CPU decode measured 13 MHz-eq, same core type) + < 0.5 MHz-eq on the CPU, **if** no per-frame crossing. Upper bound Fit-2: 18–19 MHz-eq [silicon, darkplay-1614] | ~13 MHz-eq on the CPU [silicon, session 36 coarse; `p19-placement.md` §4.1] | same as B |
| **Dark play: wakes/s** | COP: FIQ ~21.5 (5.4 with 32 KB chunks) + 2–4 codec bursts; CPU: tick 100 + ≤ 5 thread wakes | CPU: tick 100 + FIQ ~21.5 + 2–4 codec + ≤ 5; COP 0 | B + COP pulse 100 until K-1 |
| **Dark play: SDRAM traffic** | bitstream read cached on the COP (one write-back + drop per refill generation, a few per hour), PCM written back by the COP before its own DMA (`dma_tx_buf_prepare()` already does it on the calling core). No uncached per-word reads | all one core, cached | same |
| **Dark play: IRAM** | unchanged: the codec's IRAM, the DSP's `ICODE`/`IBSS` and `dma_play_data` (IBSS) are already in IRAM, which is uncached and shared, so the COP runs them as is [code]. Apple-style overlays are not needed: our codec loader already swaps the codec's IRAM per codec (an overlay per format) | unchanged | unchanged |
| **Dark play: verdict** | ≈ B, ±1 mA [estimate]. A second core taking interrupts costs what a `PROC_SLEEP` wake costs (unknown, M-1) | baseline | B + the pulse until K-1 |
| **Lit UI responsiveness** | UI alone on its core with direct calls: no RPC, no sync points. Decode cannot take the UI's turns (np-lag gone without the input boost) | UI alone on the COP, but every back-end call is a whole-cache sync point on both cores (ucop2: 127 remote calls/s; "slightly less responsive than inline" [silicon, sitting 3]); `be_svc` shares the codec's band | decode and UI share the CPU; np-lag mitigated by the input boost (80 MHz for 1 s after input) |
| **Audio robustness: UI or disk stalls** | **best**: the decode and the FIQ are on a core that runs nothing else. The CPU's IRQs-off windows (ATA PIO, cache drops, I2C) never delay the PCM FIQ. The bitstream ring holds minutes; only control (a track change, a codec load from disk) waits for the CPU | decode shares the CPU with `artcache` (68–166 ms slices [silicon]), tagcache, buffering, USB; pcmbuf's depth and the codec band protect it | same as B, plus the UI's own hogs (`main` 72–88 ms slices [silicon]) |
| **Cross-core surface** | ~30–50 functions, mostly commands: play/pause/stop/seek, track change, `dsp_configure`, volume and EQ sets, clicks, the viz tap (48 audio-family names called by the shell, ~30 by `playback.c`/`codec_thread.c`) [code count] | **238 remote-call stubs** (`p19-copfileio` `93ce6f6e35`), each a whole-cache write-back + drop on both cores; ~110–180 cached statics still flagged by `febe_reach_check.py`; MF-4 pixel fences; S10 job-driver hooks | none |
| **Bug class** | narrow and audio-local: the mixer's and pcmbuf's state written by one core (the COP); commands by message; the S2 lesson applies (one message per EQ set) | broad: any shell static on a line the CPU also writes (quirk 88, 4f's OOM), any CPU-only driver the shell reaches (quirk 54/55, sitting 3's tone and DAC wake), any module that links without the wraps | none |
| **Code size (`_end`)** | drops the split: **−10,880 B** [build], and the 8 KB `ui` stack at run time; adds an engine boundary (~3–5 KB) and reuses `cop_job` (1.8 KB) and `codec_cop` (4.7 KB, of which the call-server thunks ~2 KB go): **net ≈ −6 to −9 KB** | +10,880 B over no split [build] + 8 KB stack (runtime); grows with each routed family | the split is compiled into the release but never run (default 0): 10,880 B carried for nothing |
| **BCM2722 job driving** | on the CPU, as Apple (whose E2 transfer object refuses to run on the COP [apple-RE]) and as today: no MF-4 fences, no S10 cross hooks, E2 started and polled by the core that drew the pixels | COP owns the ring; CPU keeps power transitions; MF-4 fences (50 ms timeout, stale-pixel fallback); E2 from the COP needs B10 | on the CPU |
| **Effort** | **L**: the audio pipeline made core-agnostic, a COP audio engine, a buffering ring interface, commands by message (§7). Fit-2's machinery is the starting point | **M remaining but open-ended**: the 110–180 statics, then bench 4's six rows; each sitting has found a new coherence class | none |
| **Risk** | medium, local to audio: the PCM FIQ on the COP is unproven on our unit (P-1); playback.c's track-change handshake with pcmbuf; the video soundtrack (Fit-2 saw it stand still under video, cause open) | high, diffuse: silent corruption classes (quirk 88), found one sitting at a time | low |
| **0.8.0** | not in 0.8.0 | currently a gate item; ucop2 has not passed on silicon | **ship this** |

---

## 4. Feasibility answers

### 4.1 Can the I2S DMA-complete interrupt be routed to the COP?

**Yes in principle; unproven on our unit.**

- **The controller is per core.** Every core has its own enable mask, its own set/clear registers and its own FIQ select; the raw status is common [rockbox-derived] (`interrupts-timers.md`; `pp5020.h`). The COP's registers:
  - `COP_INT_EN` `0x6000_4034` (set);
  - `COP_INT_DIS` `0x6000_4038` (clear);
  - `COP_INT_PRIORITY` `0x6000_403C` (a 1 bit selects FIQ);
  - `COP_INT_EN_STAT` `0x6000_4030`;
  - `COP_FIQ_STAT` `0x6000_400C`.
- **The route on the COP.** The PCM DMA is source 26 (`DMA_MASK`); I2S is source 10 (`IIS_MASK`, the older non-DMA path). Routing it means:
  - `CPU_INT_DIS = DMA_MASK` once;
  - then, on any core, `COP_INT_PRIORITY |= DMA_MASK; COP_INT_EN = DMA_MASK`.
- **Apple does not do this (corrected 2026-11-06).** The call read earlier as "the COP enables source bit 26 at priority `0x3F`" is the per-core cache set-up, and the COP's FIQ target at IRAM `0x4000_0000` is a trap, not a handler. Apple routes source 26 to the **CPU's** FIQ, owns I2S on the CPU and forwards COP calls to it [apple-RE] (`../pp5022-reference/apple-cop-audio.md` §0). Apple's audio is a hybrid: the COP computes [hypothesis, strong], the CPU refills the DMA. The COP route is ours to prove (P-1 retest, `apple-cop-audio.md` §7). Apple's second DMA engine (the BCM2722 port) refuses the COP; the general engine that feeds I2S does not [apple-RE].
- **Rockbox's side is ready but unused.**
  - crt0 points the COP's FIQ-mode SP at `cop_fiq_stack` (`crt0-pp.S` `cop_init`), but reserves no space for it: the label is the top of the CPU's `fiq_stack`, the 1 KB under `#if 0` [code, corrected 2026-11-05, §5.1]. (A) must reserve it.
  - The FIQ vector jumps through one patchable pointer, `fiq_function`, which both cores share [code].
  - `dma_play_data` lives in IBSS (IRAM: uncached and shared) [code].
  - `irq_handler` already dispatches a COP branch (Timer 2) [code].
- **Rockbox never did it.**
  - `pcm-pp.c` records the starting core (`dma_play_data.core = processor_id()`, "This will become more important later - and different !", `6077e5b7c8`), but nothing ever reads it.
  - The locks still say "dual core support will require other measures" [rockbox-derived].
  - The codec *thread* ran on the COP for 22 days in 2007 and was moved back for file-system safety (`66258a30a4`) [rockbox-derived]. That reason does not apply to (A), where the COP never touches the file system.
- **What has to change in `pcm-pp.c`.**
  - **The lock.** `dma_tx_lock()`/`_unlock()` write `COP_INT_DIS`/`COP_INT_EN`. These are set/clear registers, so a write from either core is atomic [rockbox-derived for the set/clear form; [hypothesis] that a CPU write to `COP_INT_DIS` masks the COP's source].
  - **The depth counter.** `sink_lock()`'s `++locked` is not atomic across cores. It becomes COP-only (CPU callers send a message), or one counter per core summed by the reader.
  - **The FIQ start.** `sink_start_pcm()` sets `fiq_function` and starts DMA0 from the COP.
  - **The write-back.** `dma_tx_buf_prepare()`'s `commit_dcache()` then writes back the COP's cache, which is the right core because the COP produced the PCM [code].
- **Quirk 55 should read** "single-core **by Rockbox's wiring**". The silicon has per-core routing [rockbox-derived]; Apple does not use the COP route (corrected 2026-11-06) [apple-RE]. The edit is proposed here and left for the manual's owner.

**Settles it:** probe P-1 (§5).

### 4.2 Can the CPU idle in `PROC_SLEEP` with the tick slowed or stopped in dark play?

- **It already sleeps between interrupts.** `core_sleep()` writes `PROC_SLEEP` whenever no thread is runnable (`thread-pp.c`) [code], and Apple's idle loop does the same [apple-RE]. In dark FLAC today the CPU is 90 % idle with the COP decoding (Fit-2), or ~45 % with the CPU decoding [silicon]. **What wakes it is the 100 Hz tick plus the work it owns.** In (A) the decode is not the CPU's work.
- **Apple keeps its timers running while awake** and stops them only in light sleep (`apple-power.md` §2.2 step 7, §6) [apple-RE]. No evidence was found that Apple runs tickless during playback. So **the tick is not what separates us from Apple.** The 100 Hz tick is the same floor in every layout here.
- **Two ways to cut the tick, independent of the layout:**
  1. **Coarse tick in dark play** (M):
     - program Timer 1 to 50–100 ms while the screen is dark and advance `current_tick` by N per interrupt;
     - tick tasks run 10–20 times a second, and timeouts fire up to N ticks late.
  2. **Dynamic tick** (L, `p19-placement.md` §5.4): program Timer 1 for the next deadline. It touches `firmware/kernel` and every tick task.
- **What then services the tick-driven work:**

  | Work | In dark play with a coarse tick |
  |---|---|
  | Wheel and buttons | interrupt-driven: the wheel's I2C source 40 posts at once [code]. `button_tick`'s repeat and the hold-to-power-off count ticks: count in ms instead (`USEC_TIMER`), or accept ±N ticks |
  | ADC (`adc_scan`, 1/s) | runs at the coarse rate, still ≥ 1/s |
  | USB insert | the GPIO interrupt (sources 32–34) is the edge; the tick's debounce runs coarsely, and a 100 ms debounce is fine |
  | `timeout_register` users (batcal 1 Hz, powerlog 4 Hz, bootbatt) | fire late by ≤ N ticks; batcal moves into the power step anyway (`p19-placement.md` §5.3) |
  | Backlight, `lcd_tick`, scroll | not running in dark [code: removed or idle in dark] |
  | Kernel timeouts (`queue_wait_w_tmo`, `sleep`) | resolve at N-tick granularity. The audio and storage deadlines are seconds long |
  | **The COP's deadlines in (A)** | the codec's "pcmbuf full" wait (`queue_wait_w_tmo(..., pcmbuf_full_wait_ticks())`, `codec_thread.c:268`) must not depend on the CPU's tick. Either **Timer 2 routed to the COP** (Rockbox already dispatches `TIMER2` on the COP, `system-pp502x.c:206`, `timer-pp.c:73` [code]) or a wake from the COP's own PCM FIQ at the low watermark, through a forced IRQ (FIQ handlers cannot post to kernel queues) [hypothesis] |

- **Decision:** measure the tick's share first (row T-1, §5). Only build the coarse tick if T-1 shows it matters. That is `p19-placement.md` Q-8's rule, unchanged.

### 4.3 What does DSP on the COP cost?

- **Cycles: the same as on the CPU** (identical ARM7TDMI cores, one clock).
  - Fit-2 measured the CPU's `codec` thread, call server + DSP together, at 6.9–7.0 % of 24 MHz ≈ 1.7 MHz-eq for FLAC at default settings [silicon, darkplay-1614]. The DSP alone is below that.
  - Upstream's PP5022 table puts MP3 DSP at 2–4 MHz-eq [rockbox-derived, `fit2-brief.md` §6].
  - EQ, crossfeed and timestretch (audiobook speed) add more; unmeasured.
- **Memory.** The DSP's hot code and sample buffers are `ICODE`/`IBSS` (IRAM, uncached, shared) [code], so there is no cache cost on either core.
- **Coherence.** The DSP's configuration is written by setters on the CPU (the UI's EQ and speed, the audio thread's `dsp_configure`). In (A) every setter becomes **one message per set** to the engine: the S2 lesson, where one remote call per EQ band tore 300/300 seeds and one call per set tore 0/300 [measured, `tests/audioxcore`]. The software prescaler lives with the DSP on the COP; the WM8758's hardware volume stays an I2C write on the CPU.
- **Net change versus Fit-2: a saving.** Fit-2 ran the DSP on the CPU over PCM it read uncached from the COP. In (A) the DSP reads the decoder's output from the COP's own cache.

---

## 5. Measurement plan (next bench)

No current meter (memory "no meter tests"). Every power row uses:
- the power log at 5 s;
- the voltage-slope method: mV/h by least squares in one band (3,635–3,955 mV, the flat 40–80 %), arms in ABAB order, with `powerlog_mark` tags;
- `idle_pct`, `cop_idle_pct`, `stor_on_pct` and `disk_pct` matched between arms;
- the `m0 wakes` per-thread counters, and `clk stats` for the clock.

Lit rows use the `uilat` probe (`np n= avg= max= o100=`) and `underruns=`.

| Row | Arms | What the owner does | Reads | Decides | Needs |
|---|---|---|---|---|---|
| **P-1 COP audio FIQ** (developer lab, ~1 min) | — | Developer › bench: the lab stops playback, routes source 26 to the COP's FIQ (`CPU_INT_DIS`, `COP_INT_PRIORITY`, `COP_INT_EN`), has the COP start DMA0 on a 1 kHz tone in a 64 KB buffer for 10 s with 8 KB chunks, counts FIQs per core, then restores the CPU route | `p1 cop_fiq=N cpu_fiq=M expect≈215 tone=` + "Did the tone sound clean?" | **pass:** N within ±5 % of 215, M = 0, clean tone. ~~This is the gate for (A)~~ **Since 2026-11-06 it gates only the extension X1**: the hybrid needs no COP route (`layoutA-engine-brief.md` §12.5) | a small lab (S), no playback code changed |
| **P-2 COP Timer 2** (lab, 10 s) | — | none (automatic) | `p2 cop_t2=N expect=1000` at 100 Hz | the COP's own deadline source in (A) | lab (S) |
| **D-1 dark FLAC: C vs Fit-2** (= M-2 arms A/B) | C (`CPL_MODE_CPU`), F (`CPL_MODE_COP`) | fixed FLAC album, Hold on, dark, fixed volume; 30-min arms C F C F | mV/h, idle %, COP idle %, wakes | (A)'s **upper bound** against today. If F ≤ C already, (A), which removes F's call-server overhead, is at worst level | existing developer row |
| **D-2 dark FLAC: pulsed vs parked COP** | C, C + K-1 | same | mV/h, COP wakes | what a woken idle core costs: the term that separates (A) from (B)/(C) in dark play | K-1 (19-1d) |
| **T-1 tick share** | C, C with a 10 Hz coarse tick (developer switch) | same | mV/h, CPU wakes | whether §4.2's coarse tick is worth building | the switch (M); defer if D-2 shows a woken idle core is near zero |
| **L-1 lit Now Playing, MP3 320** | C (input boost on), F, B (`ucop`) | wheel volume up and down in Now Playing for 1 min, then scroll Songs fast for 30 s | `uilat np avg/max/o100`, `cpu codec= main=`, `ib=` | lit responsiveness: F approximates (A)'s lit behaviour (the decode is off the UI's core) | existing switches |
| **R-1 audio under stress** | C, F, B | music playing; start an art pass (`artcache` busy), scroll fast for 60 s, open and close the visualiser, plug and unplug USB once | `pcm underrun` count, `underruns=`, `m0 slice max` | robustness against UI and disk hogs | existing |
| **A-1 (after (A)'s first build)** | C, A | D-1, L-1 and R-1 again | as above | (A) ships when: dark mV/h ≤ C's in both repeats; L-1 `o100` ≤ C's; R-1 underruns 0 | (A) step 3 (§7) |

### 5.1 Sitting 4h: the probe sitting as built (owner decision, 2026-11-05)

The owner adopted (A) on one condition: **prove the CPU is essentially asleep with the screen off while music plays.** Sitting 4h is the short probe sitting that proves or refutes the feasibility: Developer › Bench session › **Core layout probe** (`apps/ipodui/devbench_cop.c`, `devbench.h` `bench_probe_picks`; the design and the register order in `../pp5022-reference/interrupts-timers.md` §6). 19-9 (the UI on the COP) is retired. Three rows, basic to advanced, independent of each other (every way out of a row puts the route back from the CPU's side, so a failure strands nothing):

| Row | Kind, owner time | Logs | Pass | Evidence today |
|---|---|---|---|---|
| **p2** (P-2) Timer 2 on the COP | automatic, silent, ~15 s | `p2 cop_t2=N cpu_t2=M expect=1000` | `N` 980–1020, `M` = 0 | [rockbox-derived] the dispatch (`timer-pp.c`, `irq_handler`'s COP branch); on silicon [hypothesis] |
| **p1** (P-1) the PCM DMA's FIQ on the COP | a ~1 kHz tone in the headphones for 10 s, then "Did the tone sound clean?" (~40 s) | `p1 cop_fiq=N cpu_fiq=M expect=E rate= tone= us=`; `p1 cop_cpsr= fbit= stack= dog=` | `N` within ±5 % of `E` (215 at 44.1 kHz), `M` = 0, answer "Yes" | [apple-RE] Apple's COP enables source 26; the COP taking it as a FIQ [hypothesis, strong]; our unit [hypothesis] |
| **duty** the CPU's duty, dark | hands off, ~15 min: music playing, screen off, Hold on; arms C F C F (decode on the CPU = today; decode on the COP via `codec_cop_set_place`), 30 s settle + 3 × 60 s windows | per window: the CPU's wakes/s (`core_sleep` exits) and IRQs by source (tick, Timer 2, USB, IDE, wheel, GPIO, other), thread wakes (the `m0 wakes` counters, top three), CPU and COP idle ‰, the clock (`clk` stats: ‰ at 24 MHz, entries, boosts from idle, PLL locks, MHz), disk wakes, screen, Hold, mV; power.csv marks `duty C1` … | a reading, no verdict | the counters [code]; the arms' numbers [silicon] after the sitting |

**Owner time:** about 1 min with the iPod (play an album first, start the run, headphones for the tone, the answer, Hold on/off), plus about 15 min hands off. Nothing touches the video chip or its image, and no step earns a verified line.

**What the duty row decides.** "Essentially asleep" gets a number: with the decode off the CPU (arm F, the nearest existing shape to (A)), the CPU's wakes per second and idle ‰, and how much of the remainder is the 100 Hz tick (the `tick` IRQ count against all wakes). If the tick is most of what is left, the coarse tick (§4.2, T-1) is the next lever and (A) needs nothing else from the CPU side; if threads are, the `thr` line names them. Expected [hypothesis, from Fit-2's silicon numbers]: arm C ~450 ‰ CPU idle; arm F ~900 ‰ with the tick the largest single source.

**Two code findings** (`interrupts-timers.md` §6.3): crt0 reserves no COP FIQ stack (`cop_fiq_stack` is the top of the CPU's `fiq_stack`; the space is under `#if 0`), and the FIQ vector's target is one word shared by both cores. (A) step 2 must reserve the COP's FIQ stack; the probe brings its own for the row.

**Decision after the sitting** (unchanged from below): (A) proceeds if p1 passes and the duty row's arm F is within the A–A spread of arm C or better on CPU wakes and idle; if p1 fails, Fit-2 with the FIQ on the CPU.

**Decision rule.**
- **(A) proceeds** if P-1 passes and D-1 shows F's dark cost within the A–A repeat spread of C, or at most F's known call-server overhead (~5 MHz-eq) above it.
- **If P-1 fails**, the fallback target is Fit-2 with the FIQ kept on the CPU, and decode on the COP only while lit (`CPL_MODE_SCREEN`). Even then, 19-9 is not resumed.

---

### 5.2 Sitting 4gh results (2026-11-06, build `56cd4e2096`) [silicon]

| Row | Reading | What it means for (A) |
|---|---|---|
| **p2** (P-2) | `cop_t2=1000 cpu_t2=0` in 10.0 s: **PASS** | the COP has its own deadline source |
| **p1** (P-1) | `cop_fiq=0 cpu_fiq=0`, `cop_cpsr=0x5f` (F set); the owner heard a blip, not a tone | **no verdict on the route**: the COP's F bit was never cleared (quirk 74), so DMA0 played one 8 KB piece and the FIQ that would re-arm it was masked. Fixed (the COP's start-up clears F in layout (A) builds; the probe checks it); p1 then pcop rerun |
| **k1** (K-1) | COP 100.5 → **0.6** wakes/s, 3 pulses in 150 s, 0 underruns in both arms | K-1 works. The row printed FAIL because its rule wanted 0 pcm rescues in the baseline too; both arms had 44 (decode on the CPU, so not the park's). The rule is now A/B (parked no worse than pulsed); the rescues are a refill-pacing finding |
| **duty**, arm C | CPU idle **136 ‰**, CPU wakes **60/s**, COP wakes **100/s** (pulsed), tick 100/s the only IRQ, decode on the CPU at 30 MHz, dark | far busier than the ~450 ‰ expected, but the audio was a video's soundtrack (#154), not ordinary music. **Arm F did not run** ("playback did not start" at its codec load on the COP): the decision's key input is still missing |

**The decision stays open**: it needs p1 to pass (after the fix) and arm F. Next sitting: p1, pcop, duty arm F (`docs/bench-reruns.md`).

## 6. What 0.8.0 should ship

1. **The layout: (C) as it ships today.** UI inline on the CPU, decode on the CPU, the np-lag input boost (`clockpol_note_input()`), `IPODUI_UI_CORE_DEFAULT 0`. **Superseded 2026-11-07 (owner): 0.8.0 decodes on the COP** (`CPL_MODE_COP`); the UI stays inline on the CPU (`changes-from-rockbox.md` 2.67).
2. **19-9 comes off the 0.8.0 gate** (`docs/release-plan.md` §1, row "UI on the COP"). The rows `ucop`, `uck1`, `uck2`, `ufeel`, `upanel` and `ucop2` leave the 0.8.0 bench.
3. **The split leaves the release build.** `HAVE_UI_THREAD_SPLIT` and `febe_rpc.opts` apply to developer builds only (`ipodvideo-variant.h`, `apps/apps.make`).
   - **Measured** [build] on the release shape (`RELEASE_SHAPE` + `RELEASE_STOCK`, this tree):
     - with the split: `_end` **1,495,456 B**;
     - with `HAVE_UI_THREAD_SPLIT`, `HAVE_FE_FB_HANDOFF` and the `--wrap`s removed: **1,484,576 B**, so **−10,880 B**;
     - by section: `.text` −12,224, `.rodata` −736, `.ncbss` −2,688, `.bss` +4,768 (the settings and statics go back to cached);
     - it built with no new warnings.
   - `p19-copfileio` reports its release shape at 1,499,232 B against the 1,500,000 B cap. So this is also the 0.8.0 headroom fix.
   - The split itself needs no change: it still compiles out as `ucop-fix2` N3 left it.
4. **K-1 (19-1d) lands if it is ready.** It removes the COP's 100 pulses a second in every layout, and (A) needs it.
5. **Keep from the 19-8/19-9 work:**
   - the file layer's own cache lines (`p19-copfileio`), and `xcore_line_check.py` / `febe_reach_check.py`, which the audio engine's region will reuse;
   - the uncached WM8758 shadows;
   - the CPU-only assertions on the mixer and the file layer.

   They protect (A) as well.

---

## 7. Migration steps for (A)

> **Superseded for steps 3–6 (2026-11-06).** After the RE of Apple's COP audio (`../pp5022-reference/apple-cop-audio.md`: Apple takes source 26 on the CPU, never on the COP) and the owner's decision to build the hybrid first, the implementation order is **`layoutA-engine-brief.md` §14.1** (revision 2):
> - **E0** contracts (S, ½ day); **E1** the boundary on one core (M–L, 3–4 days); **E2** the bitstream ring (M, 2 days); **E3** the PCM ring and long DMA runs on the CPU (M, 2–3 days; 64 KB runs, ≈ 345 → ≈ 2.7 CPU FIQs/s at 44.1 kHz); **E4** the line region (S, 1 day); **E5** the engine on the COP in hybrid mode (L, 3–4 days; no interrupt moves); **E6** the A-1 bench (owner ~2.5 h; CPU audio wakes ≤ 5/s dark, 0 underruns).
> - **X1**, the extension (the CPU never wakes for audio): after A-1 and a passing probe from the brief's §12.5 one-sitting set (COP FIQ, COP IRQ, Timer 2 pacing, DMA auto-reload, `PROC_WAIT_CNT`, the I2S FIFO interrupt). M, 2–3 days.
>
> Step 2 below (`pcm-pp.c` core-agnostic) stays built and becomes X1's route switch; it is no longer a prerequisite of the engine. The table below is kept as the original plan.

Each step keeps (C) working behind one switch until A-1 passes. The switch is the developer row that already selects the decode's core; a release-hidden `config.cfg` key follows the 19-9 pattern.

| Step | Move | Prerequisites | Size of change |
|---|---|---|---|
| **0** | P-1 and P-2 on silicon; D-1, D-2 and L-1 on the current build | the two labs | S |
| **1** | **K-1:** the COP parks with no runnable thread and no pending timeout; posts and semaphore releases call `core_wake(COP)` | host test (`p19-placement.md` §3.4) | M |
| **2** | **`pcm-pp.c` core-agnostic:** a `pcm_core` chosen at `sink_dma_init()`; the lock masks that core's source 26 through the set/clear registers; the depth counter is owned by the engine's core; `fiq_function` is set and DMA0 started on that core. The CPU route stays the default | P-1 | S–M, `firmware/target/arm/pp/pcm-pp.c` (upstream file: ledger) |
| **3** | **The COP audio engine:** the codec, `pcmbuf`, the DSP and the mixer run on a COP thread (Fit-2's worker grown into an engine; `codec_cop.c`'s callback thunks retired). Commands CPU → COP: play, pause, stop, seek, track change, `dsp_configure`, EQ set, volume prescaler, click, viz tap open/close. Events COP → CPU: track finished, position, underrun, codec request. Each goes as a post into an uncached queue, with state in one seqlocked "audio now" record (the `fe_be_core` seqlock, reused) | steps 1–2 | L, `apps/codec_thread.c`, `apps/pcmbuf.c`, `apps/playback.c` (the pcmbuf handshake), `firmware/pcm_mixer.c` |
| **4** | **Bitstream ring:** buffering (CPU) fills, the codec (COP) reads cached and drops its cache once per fill generation; the read and write cursors are single-writer uncached words. `request_buffer`/`advance_buffer` become local ring operations on the COP | step 3 | M, `apps/buffering.c` codec-facing calls only |
| **5** | **Audio-engine line region:** the engine's objects (`pcmbuf`, `pcm_mixer`, `dsp_*`, the codec's statics) linked on whole lines of their own, checked by `xcore_line_check.py`; `XCORE_ASSERT_COP` on the engine's entry points (developer builds) | step 3 | S |
| **6** | **A-1 on the bench**, then the default flips to (A) | A-1 rules (§5) | one line |
| **7** | **Retire the split:** `fe_be` remote calls, `febe_rpc_stubs`, MF-4 fences, `be_svc`, the 8 KB `ui` stack. The ledgers record it | step 6 | removal (≈ −10.9 KB `_end` [build]) |
| **8** | (if T-1 says so) **coarse tick in dark play** on the CPU; the COP's deadlines from its own PCM FIQ (Timer 2 dropped, `pp-governor.md` §17.3) | step 6, T-1 | M |
| 9 | (later) video soundtrack on the COP engine (Fit-2's stall under video is the open risk), USB audio (#144: `usb_audio` runs `dsp_process()` in an interrupt on the CPU) | step 6 | M |

**Status (2026-11-05, branch `p19-corelayout-k1`):**

| Step | State | Evidence |
|---|---|---|
| **0b** (the COP's FIQ stack, per-core dispatch) | built: `crt0-pp.S` reserves 1 KB; `fiq_by_core` follows Apple's `PROCESSOR_ID` stub (`osos 0x1000_015C`) | [code]; device: rows p1 and pcop |
| **1** K-1 | built behind `corelayout_set_park()`, off by default | host test `tests/copk1` [model]; device: row k1, COP wakes 100.5/s → 0.6/s, 0 underruns [silicon, 4gh] |
| **2** `pcm-pp.c` core-agnostic | built behind `pcm_pp_set_core()`, the CPU by default. **The start and the lock run on the caller's core**, which is the CPU in step 2 and the engine's thread from step 3, not on `pcm_core` | [code]; device: row pcop (needs p1) |
| 2 → real music | **not yet**: the refill chain is the CPU's cached memory (`data-paths.md` cell 10); it needs step 5's line region before the music's FIQ can move | [code] |

All of it is in developer builds only (`HAVE_CORE_LAYOUT_A`). The release shape's `_end` is unchanged at 1,455,904 B [build].

**The clock governor across these steps** (2026-11-06): steps 4, 5 and 8 meet the governor plan in `pp-governor.md` §17. With the codec reading the ring on the COP (step 4) and the engine's objects on its own lines (step 5), the clock is decided by per-core request words with immediate raises (B1 removed), a band per track and a busy-time step-down (§17.2, order items 4–5); the codec's full-buffer poll becomes the PCM FIQ's watermark wake (order item 6). Step 8 is order items 7–8: T-1 first, and a COP timer source only if T-1 justifies it; Timer 2 and `PROC_WAIT_CNT` sleeps were dropped for the reasons in §17.3.

**Device-reality checklist for (A)** (`docs/process.md`):
- **Compiled features:** the engine is in every shell build; the (C) route stays as the fallback.
- **Memory during playback:** unchanged (the COP worker's 9 KB stack is already in the codec slot).
- **CPU at 50k:** none; the database stays on the CPU.
- **Storage wake-ups:** unchanged (buffering is the same).
- **LCD:** unchanged and on the CPU.
- **Threads and locks:**
  - one new COP thread replaces the Fit-2 worker, so the thread slots are unchanged;
  - the PCM lock becomes per-core-owned;
  - no kernel object both cores write sits in cached memory.
- **Splashes:** none.
- **Persistence:** none.
- **Stock firmware:** untouched.
- **Device-only checks:** P-1, P-2, D-1, D-2, T-1, L-1, R-1 and A-1.

---

## 8. Open points

- **Is the B2 call server worth keeping as the fallback?** (A)'s step 3 retires Fit-2's per-callback calls. Fit-2 stays the developer arm only until A-1 passes.
- **Hold-on dark play in (A):** the wheel controller's interrupt reaches the CPU, and the COP never needs it.
- **Mailbox doorbell** (`CPU_QUEUE`, open question 7): Apple posts to its COP engine through it. It is an optional cheaper wake than `core_wake()` for step 3; probe it later.
- **The `0x14xx_xxxx` shared view** Apple uses: not needed. `XCORE_BSS` on the `0x1000_0000` alias does the same job.

## Sources

- `docs/design/p19-placement.md` §0, §1, §4, §5, §8.
- `docs/design/p19-ui-on-cop.md`: as built, sitting 3, ucop-fix2, the switch.
- `docs/pp5022-reference/apple-power.md` and `apple-dual-core.md`.
- `interrupts-timers.md` and `cores-and-threads.md` §7.
- `quirks.md` 54, 55 and 88 (88 came in with `p19-copfileio` `93ce6f6e35` as a second "60"; renumbered for 0.8.0).
- `docs/data-paths.md` R7.
- Code: `apps/codec_cop.c`, `firmware/common/cop_job.c`, `firmware/target/arm/pp/pcm-pp.c`, `crt0-pp.S`, `kernel-pp.c`, `system-pp502x.c`, `thread-pp.c`, `apps/pcmbuf.c`, `codec_thread.c`.
- The size prototype: `build/fw-ship` vs `build/fw-ship-nosplit`, release shape, this tree. The flags were reverted after the build.
