# Brief: Phase 19, hardware offload

*Names: this brief uses the technical names: **PP5022** (the PP), its cores the **CPU** and the **COP**, the **BCM2722** VideoCore (the BCM) and the **WM8758**. In conversation the owner calls them PP1 (CPU), PP2 (COP), VC (BCM2722) and DAC (WM8758). "The CPU path" is a job's fallback on the PP5022.*

> **DRAFT, revision 1c (2026-10-23).** Not approved. Rev 1c answers the second review (of rev 1b: N1–N5, M1–M8, the lows) and re-bases on P12's tip; rev 1b had folded in the owner's reorder, the placement plan, the owner's answers, contract 4 and the first review (tables at the end). Branch `p19-brief`: rev 1 was cut from `p18-114`; **its base is now P12's tip `c00496f6fa`** (merged at 854bc1b5c6; it contains `p18-114`, `p18-refill` and the 3 s dark drive hold), with `p19-placement` (2597ef495e) merged too. **The merge order is P18 (`p18-114`) → P12 (done, 52bc1b81a0) → P12's gate and merge → P19 branches from that tip** (owner, 2026-10-23: P19 runs straight after P12, ahead of P17, P13 and P22). Milestone **P19: Hardware offload**; issues #72 (the core split), #119, #115, #75, #76, #77, #78, #83; related #113, #114, #108, #6.

*Process: `docs/process.md` (brief template, checklist 1–13, lean rules 1–11, one gate per machine). Open owner questions are frozen as ⚠️ with a default (§0.1). Split by risk: dark playback on one core (19-1) is its own mini-phase with its own review; the job interface, the screen-truth contract (§3.6) and the front-end boundary each get their own review before the screens that use them. Evidence tags: [silicon] measured on our unit (log named), [measured] our counters or the harness, [sim], [code] read from the tree, [rockbox-derived], [apple] / [external], [bcm] the BCM session's contracts or answers, [estimate], [hypothesis], [ours].*

**The placement reference.** Where every job runs in every power state, the wake budget per state and the migration order come from **`docs/design/p19-placement.md`** (rev 0.1, written by the platform session with the BCM session; merged here). This brief ranks, schedules, budgets and accepts that work; it does not repeat the plan's tables.

**Goal (owner, 2026-10-23):** *"get the hardware chip prepping what it needs to — interfaces, contracts, documentation… offload this consistently and safely. I'd love to get that running butter smooth, and especially if it helps us with our RAM overhead and the sleep and power issues."*

**The architecture P19 builds toward (owner, 2026-10-22, #72):**
- **CPU = back end:** codec, buffering, DSP, storage, every interrupt and peripheral.
- **COP = front end:** UI and input.
- **BCM2722 = display driver:** the panel, composition, pixel jobs.
- **Dark playback** (screen off, a playlist queued): the CPU and the WM8758 run, plus the drive briefly for each refill (#114). The COP, the BCM2722 and the drive stay off; nothing else powers up unless a key, a refill or an alarm needs it.

**What P19 is for, plainly.** Its wins are **power** and **smoothness**. **RAM is not a P19 goal.** The drift keeps its PP cover memory on every unit (D-4, smoothness first), so the BCM2722 offloads return no RAM, and L2 (our BCM2722 image resident in PP RAM) **costs** RAM: about 220 KB with the resident video image, about 28 KB once the display-only image is proven (§2A.1). Every floor still holds with a wide margin.

1. **Power: dark playback toward 25 mA.** Dark FLAC measured **~35–40 mA** with the old 10 s drive hold [estimate, placement §0]; the 3 s hold now on `p12` should bank most of the ~5 mA drive item, so **~31–36 mA** is the expected starting point [estimate, unmeasured until M-1's "before" run]. Thread wakes fall from **~88–95 /s plus 100 COP pulses /s** to **≤ 5 /s with the COP at 0** (with option B+D, 19-1c); drive power-ups from ~22 /h to **≤ 6 /h**; **zero BCM2722 power-ups** in dark playback. **25 mA is a tracked target, not an acceptance gate:** the placement plan says plainly that placement alone may not close the gap, and M-1 finds the rest.
2. **Smoothness.** Slides start within one frame (≤ 33 ms from the key to the first moving frame, against 60–100 ms of set-up today [measured, harness, `code-audit-1.md` F4]); glides hold 30 fps unboosted; the drift and the marquee cost the PP5022 under 1 % between them; the lit clock at 24 MHz once the drift leaves the CPU.
3. **RAM: not a goal; bounded cost.** About **−220 KB** to the audio buffer on every unit with the resident video image, about **−28 KB** with the display-only image (§2A.1). Cover Flow's ~512 KB never becomes a PP reservation, and is **kept out of every net figure**.

---

## 0. Owner pre-flight

### 0.0 Decided

| # | Decision | Source |
|---|---|---|
| **D-1** | **Memory-role offloads use the degraded fallback** (`jobs.md` §7, option b). When the BCM2722 is unavailable: **the drift shows a still cover**, logged; **Cover Flow is disabled**, and selecting it shows a small styled pop-up saying, in our own words, that Cover Flow is not available at this time. That pop-up is a **deliberate exception to "log, don't show"**, like the video-failure screen, and **the only new user-visible text in P19** (an appended `LANG_*` string; no "chip", "video processor" or "Rockbox"). It also shows when the BCM2722's SDRAM bring-up fails (#7). **Every other job falls back to its CPU path and is logged**, with no visible change. (The PP keeps its cover memory: D-4.) | Owner, 2026-10-23 |
| **D-2** | P19 runs straight after P12, ahead of P17, P13 and P22 | Owner, 2026-10-23 |
| **D-4** | **The drift keeps its PP cover memory on every unit** (Q13, decided by the owner 2026-10-23: *"We want to keep users feeling smooth and cared for… making the experience be too different, especially on core features like that, is not ideal."*). `pv.slot` (the shown cover) and `pv.slot2` (the other set's last cover, which gives the no-I/O Music↔Audiobooks switch of 8b-4a) stay on every unit, and the #92 bank keeps its 3-deep rotation on 64 MB (`bank_swap()`, `preview.c` :744–770, `PV_COVERS` 3). Uploads to the BCM2722 come from these blocks; the still cover, repaints and re-uploads need no disk. **So the drift offload claims no RAM**; it is for CPU time and power | Owner, 2026-10-23 (Q13; e361c0d19e); scope per the second review N1 |
| **D-3** | **No art building and no database maintenance in dark playback on battery.** They wait for a lit spell, a pause window or a charger; the one exception is a just-synced library the user is waiting on (placement Q-7). This also settles where BCM2722 JPEG decode may run (lit or charging only) | Owner, 2026-10-23 |

### 0.1 Open questions (frozen ⚠️ with a default until the owner round)

IDs are stable: rev 1a's Q1–Q11, the first review's Q12 and Q13 (Q13 now decided, D-4), the second review's Q14, and the placement plan's own Q-3, Q-5, Q-6 and Q-8. Duplicates are merged into one row.

| # | Question | ⚠️ Default | Why it matters |
|---|---|---|---|
| **Q1** (= placement Q-1) | **Dark playback decodes on the CPU, with the COP parked** (placement option B + D: a steady 24 MHz, watermark bursts), reversing Fit-2's default **for dark only**? | **Owner, 2026-10-24: decoding is pinned to the CPU (PP1) permanently, with no CPU↔COP migration; the UI moves to the COP (19-8/19-9, see `docs/design/p19-ui-on-cop.md` on p19-uicop). 19-1c ships the CPU plain path (no COP view, MP3 synthesis inline) plus D; the screen rule and COP decode remain developer A/B rows for M-2 only. Q1 decided.** **Yes, once M-2 confirms** by the placement plan's rule (§4.3: B ships if its mV/h is at or below arm A's in both repeats). **The re-selection points are defined in 19-1c** (a loaded codec stays loaded across same-type tracks, so "at codec load" alone is not enough). **The rationale is fewer active cycles, a parked COP and ~80 fewer wakes a second, not the PLL:** the dark FLAC run already ran at 24 MHz with the PLL off (§1A) | The COP decode costs more active cycles than the CPU doing the work [hypothesis, two sessions; §1A] |
| **Q2** | **L2 makes our BCM2722 image resident in PP RAM in release builds** (every backlight wake re-uploads it). Release `fw-ui` carries **none today**, so this **adds up to 217,088 B** (the image's `0x35000` cap) to release | **Keep the resident video image** (the merged image: LCD, video protocol, jobs) **until the warm-swap bench row (B13) proves a swap without cutting the panel; then take the display-only image** (~20–25 KB, saving ~190 KB [bcm estimate]) **where it pays, which is likely every 32 MB unit** | The largest RAM cost P19 adds |
| **Q3** (= placement Q-4) | **The lit clock at 24 MHz** once the drift is a BCM2722 job and `lit-24mhz.md` §5.2's frame budgets pass with zero rescues, with no meter? | **Yes;** the developer row keeps 30 MHz one press away | 18-5 kept 30 MHz because the CPU-pushed drift pinned the PLL |
| **Q4** | **A USB-C inline meter for M-1?** | **No meter by default:** an overnight run through the learned discharge table (±1 % of capacity ≈ ±3 mA over 10 h [estimate]) and 30-minute voltage A/Bs; a meter only if the unknown base (placement §0, item 4) must be split | Only a meter can split the base |
| **Q6** (= placement Q-2) | **The UI on the COP** (placement step 9), the riskiest change: build it last, behind `HAVE_COP_FRONTEND`, and drop it if M-1 shows the lit gain under ~2 mA? | **Yes:** everything else first; kept only if it measures, and only after a device week with no corruption | A coherency bug there is silent data corruption |
| **Q7** | **Tearing-synchronised presents** (`J_PRES_TE`, BCM #52) | **Only for full-motion work:** slides, glides, Cover Flow, Class B/C games; static updates push at once | Up to one panel period of latency |
| **Q8** | **Pixel parity of BCM2722-composed frames** | **Every frame identical to the CPU path:** `SLIDE` carries the shell's per-frame offsets; `FADE`, `AVG` and the split shadow (`SHADE`, `J_FADE_SHADE_B`) share one definition with the shell (`jobs/jmix.h`, 3d3f4d5db1) | Goldens are captured from the CPU path in the simulator |
| **Q9** | **One attended bench session, batched with the P18 Sleep lab rerun** | **Yes:** about 90 minutes attended, then the unattended runs. Install only with the owner's go and a fresh backup | One install and one backup |
| **Q12** (review C1) | **The shell's strip** (`anim.h` `UI_STRIP_SIZE`, 180,000 B; nine owners: the panel highlight cross-fade, the drift's prefetched next cover, Now Playing's marquee, mode-slide rows, body page and lyrics, the list marquee, the USB screen frames, On-The-Go reads and writes over 64 KB, and the full-screen split capture) | **Keep the strip at its size; the BCM2722 is used for speed only. No strip RAM saving is claimed** | Shrinking it would break static asserts in `preview.c`, `otg.c`, `ui_main.c`, `np_layout.h` and `usbscreen.c`, and without the BCM2722 the CPU path would re-render text every frame |
| **Q14** (second review N1) | **Redesign the cover bank to give RAM back?** A 2-deep rotation, and a set switch that needs I/O after a power cut | **No** (D-4: smoothness first) | Would return 180–360 KB on 64 MB at the cost of a visible pause at a set switch after a cut |
| **Q-3** (placement) | **Waking the BCM2722 from a cut:** the cut with the frame upload overlapped (~0.32–0.34 s planned [bcm estimate]), or also a standby window (`BCM_STANDBY_WINDOW_S`, ~130 ms faster)? | **The cut with the overlapped upload;** the standby window only if stage 5s shows it is cheap and the panel and SRAM survive it | Standby costs current while dark |
| **Q-5** (placement) | **Whole-track fills to the ceiling, with track-change reads batched into the refill?** (The dark hold itself is already 3 s on `p12`, owner 2026-10-23; was 10 s) | **Yes, flash only** (`docs/storage-choices.md`); the hold stays 3 s unless M-3 shows the hold, not the refill count, still dominates | Most of the ~5 mA drive item is banked by the 3 s hold; the remainder is refill count |
| **Q-6** (placement) | **Power the clickwheel controller down while Hold is on** in dark playback? | **Measure first** (M-1); no change unless ≥ 0.5 mA | Unknown current; a re-sync at Hold-off |
| **Q-8** (placement) | **A tickless CPU in dark playback?** | **Only if M-1 shows the 100 Hz tick costs ≥ 1 mA** | Large, risky kernel change |

*Withdrawn or decided:* rev 1a Q5 (BCM2722 JPEG in the dark: settled by D-3); Q10 and Q13 (decided as D-4); Q11 (if L2 slips: moot, the core track comes first).

### 0.2 Captures and owner sessions
- **The bench (§6):** one attended session batched with the P18 rerun, and the unattended runs. Read-only for the library; the only writes are the logs and the developer marker files the session already uses.
- **No 7G capture:** nothing in P19 changes what the user sees, except D-1's pop-up (styled by P7).

---

## 1. What already exists (read it; reuse it, do not rebuild)

| Area | Where | Reuse |
|---|---|---|
| **The placement plan** | `docs/design/p19-placement.md`: inventory (§1), placement per state (§2), delegation and coherency (§3), codec placement (§4), wake budgets (§5), smoothness (§6), memory (§7), migration steps 0–12 (§8), M-1..M-6 (§8.1), BCM answers (§9.1) | The reference for every package |
| **Cross-core job channel** | `firmware/export/cop_job.h`, `cop_job{,_core}.c`, `tests/copjob` (Fit-2 §4) | The only cross-core mechanism |
| **Codec decode on the COP** | `apps/codec_cop.c`, `codec_cop_select()`; on by default since 2026-10-18 | 19-1c changes the selection rule |
| **Codec load lifetime** | `apps/codec_thread.c` :470–511: a loaded codec stays loaded across tracks of the same type; a new load only on a type change [code] | 19-1c's re-selection points |
| **The codec's full-buffer poll** | `apps/codec_thread.c` :236–262: when pcmbuf is full the codec waits `queue_wait_w_tmo(&codec_queue, NULL, HZ/20)`, at least 20 wakes a second [code] | 19-1c's "D" replaces it |
| **MP3 synthesis on the COP** | `lib/rbcodec/codecs/mpa.c` :28–29 `MPA_SYNTH_ON_COP` (a compile-time define), its thread at :336 [code; upstream since 2008] | 19-1c's runtime switch |
| **The COP codec run** | `apps/codec_cop.c` `codec_cop_run()` :484, `codec_cop_reserve()` :110–113, `sync_fields()` :116 [code] | 19-1c's per-run core choice |
| **Deadline waits** | P18 18-A ("Wake budget", `hardware.md`) | 19-1a extends the pattern |
| **Drive power-off** | #114 (`stor_pwr.h`: **`STOR_PWR_HOLD_DARK_S` 3 s** (owner, 2026-10-23; was 10), 60 s lit); **`p18-refill`**, merged into `p12` (a fill cut short by a message carries on; the simulator's drive model) | 19-1b builds on it |
| **Clock policy** | `apps/ipodui/clockpol{,_core}.c`, `firmware/pll_idle.c`, `cpu_pll_hold()`; `clk stats` / `clk lock` log lines | 19-1c, 19-3 |
| **COP park for sleep** | P18 18-1's park handshake | K-1 (19-1d) |
| **Second DMA engine (E2)** | `data-paths.md` cell 14: ~11 MB/s; the CPU keeps 968/1000 of its time while running from IRAM, **775/1000 when its code touches SDRAM** [silicon S37]; unmeasured at 24 and 30 MHz | every slot, frame and image upload |
| **The BCM2722 job interface and API** | `bcmfw/jobs.h`, `jobs.md`, `memory-contract.md`, `power-contract.md`, and contract 4: `bcmfw/jobs/bcmjob.h` (the PP-side API), `jref.c` (the executor), `jmodel.c` (host model), `jmix.h` (FADE/AVG/SHADE) [bcm] | §3 |
| **Drift ops on the BCM2722** | `bcmfw/video/drift.c`, bit-exact in vc2sim, never on silicon; **its stage 5b port predates the split shadow and P16's drift changes** | 19-2 re-expresses the drift as jobs |
| **The split shadow** | `ui_shadow_apply()` (`material.c` :128), applied on every panel write (`preview.c` :427–493): 10 re-darkened columns; `nav_core.c`'s drift | `J_OP_SHADE` and `J_FADE_SHADE_B` (3d3f4d5db1) |
| **Overlay slide** | `HP_OP_OVERLAY`, `HP_CAP_OVERLAY_SLIDE` [silicon] | the pattern for `ANIM SLIDE` |
| **3D renderer** | `bcmfw/r3d`, `render3d.md` | 19-7, P12's Class C |
| **Resident video image** | developer builds only (`HAVE_BCMFW` + `HAVE_BCMFW_VIDEO`, `ui_main.c` :1187–1188; `hardware.md` "Resident video image"); release `fw-ui` (`validate.sh` :361, plain `--ipodui`) has none | 19-2 (L2, Q2) |
| **Stage tests** | `apps/ipodui/devbcm.c`: 5i, 5s, 5b, 3d; **they swap images and write `0x106000` / `0x010000`, inside the merged map** | the bench; the job client suspends around them (§3.1) |
| **The A/B pattern** | P18 H3 (`device-session-p18-0.md` §3.1) | M-2 |
| **F1 / F2** | F1 `blend_two_colors()` endpoints (5428706ad6, #55); F2 the marquee blits a pre-rendered label (89d2cf0457) | §1A's marquee |
| **Extras classes and SDK** | P12 §2, §2A | 19-8's link check; `J_CLS_APP` for Class B |

## 1A. Baseline (placement §0 and §4.1 are the source)

| Quantity | Value | Tag |
|---|---|---|
| Dark FLAC before #114 | ~105 mA (28.7 h on 3,000 mAh) | [silicon, rundown 2026-10-22] |
| Before vs after #114, same voltage band | 39.5 → 13.7 mV/h | [silicon, darkplay-1614] |
| **Dark FLAC now** | **~35–40 mA; gap ~10–15 mA** | [estimate, placement §0] |
| **Clock, dark FLAC** | **24 MHz with the PLL off:** `clk lock … holds=0 load=0 fmt=7` on every stats line of the dark run; the PLL-off share of uptime rose from 43 % at 15:05 to 93 % at 16:11 (cumulative since boot, so the dark stretch itself is near 100 %); 18,310 PLL power-ups by 16:11 (ATA transfers) | [silicon, `darkplay-1614/ipodui.log.1` lines 107–385] |
| CPU idle / COP idle, dark FLAC with the COP decode | 90.4–91.6 % / 29.5–32.4 % | [silicon, darkplay-1614] |
| **Active cycles, FLAC** | COP decode ~18–19 MHz-equivalent on two cores; CPU decode ~13 MHz-equivalent on one: about 40 % more with the split | **[hypothesis]:** the two arms come from different sessions and tracks (darkplay-1614 vs session 36); M-2 measures it |
| **Thread wakes, dark FLAC** | 88–95 /s (`codec` 44, `codec cop` 38, `audio` 2, `ata` 2, `tagcache` 1–2, `power` 0.5) plus 100 COP pulses /s (`kernel-pp.c:36`) | [silicon; code] |
| Drive | 23 power-ups in 1.04 h; `stor_on_pct` 9.8 %; 13–25 s powered each; ~0.5 s of real transfer per refill. **That was the 10 s hold; the 3 s hold on `p12` is unmeasured** | [silicon; estimate] |
| Where the gap is | (1) the drive's idle-powered time; (2) the COP busy two-thirds of the time plus its pulses; (3) the CPU's polling threads; (4) an unknown base (PMU, WM8758 and amp, SDRAM refresh, the clickwheel controller, regulator losses) | [placement §0] |
| Drift (lit) | 13.3 CPU pushes/s of 160×240, ~15 ms each at 30 MHz: 10–25 % of the CPU at rest; pins the lit clock at 30 MHz | [estimate, `lit-24mhz.md` §2.3] |
| **Marquee (lit), current** | **~5 % of the CPU** while a long title is selected | [estimate; Wave 0 measures] |
| Slide set-up | 60–100 ms at 30 MHz before frame 1 | [measured, harness] |
| Art decode | 26–46 s per large cover at 24–30 MHz; each album decoded 2–3 times (two `decode_pinned()` call sites); **the art builder is the largest lit CPU and battery cost the owner has met** | [silicon; code; owner] |
| Pixel RAM | `pv.slot` and `pv.slot2`, 180,000 each; #92 bank 720,976 (64 MB, `PV_COVERS` 3; none on 32 MB); strip 180,000; LCD framebuffer 153,600 (bss); row-art LRU 109,428; art decode block ~164,120; NP bundle copy 51,752; **resident BCM2722 image: 0 in release `fw-ui`** | [measured; code] |
| Floors | ≥ 7,500,000 B (32 MB), ≥ 39,000,000 B (64 MB); npfix arithmetic minimums 11,558,368 / 44,391,824 B | [sim, floor gate] |
| Size | `rockbox.ipod` 880,240 B, `_end` 1,157,744 B (P16 close, `fw-ui`); backstops 1,250,000 / 1,500,000 | [measured] |

**The marquee, reconciled.** `code-audit-1.md` F2 measured **15–38 % of the CPU** (193–288 k instructions a step, 20 steps a second) when every step re-rendered the whole selected row. **That cost is gone:** F2 landed in 89d2cf0457, and each step now blits the pre-rendered label and pushes only the text rows (320×23 = 14,720 B, below E2's 32 KB threshold, so the CPU's copy, ~2.6 ms at 30 MHz). **~5 % is current** [estimate], almost all port push, which a BCM2722 pan removes (19-5). F1 cuts the label's one-time render, not the per-step cost. The BCM session's plan now cites ~5 % too (7829baf4c4). The Wave 0 harness replaces the estimate with a measurement.

**The clock, reconciled (review H6).** Rev 1 and 1a read `clk lock n=0 … load=1` as "the PLL held at 30 MHz for the whole run". That line is the **boot line** (`clk on`, before any track); every later `clk stats`/`clk lock` pair in the dark run shows `holds=0 load=0 fmt=7` and a rising PLL-off share. **The dark FLAC run was at 24 MHz with the PLL off**, as the placement plan says. So PLL-off is not a new lever for FLAC; 19-1c's case rests on active cycles, the parked COP and wakes. Hi-res FLAC (above 48 kHz or 24-bit) has not been seen at 24 MHz and may need the `LOAD` hold; zero underruns is the hard gate. Both documents now quote this figure.

## 2. Out of scope
- **Videos and Photos screens** (P14); 19-10 is design only.
- **The Cover Flow screen** (P7); P19 builds its foundations and the availability query (19-7).
- **The soundtrack on the BCM2722** (stage 8b/9) and H.264: the BCM session's track (#60).
- **Any bootloader change.** P19 needs none.
- **Apple's firmware and the recovery path:** untouched; hold MENU still reaches it.
- **DVFS**; **#108**; **#114's drive power-off and `p18-refill`'s fill fixes** (P18's; P19 adds only the dark hold and refill shape, after `p18-refill` merges).
- **Shrinking the strip, the PP framebuffer or the cover memory** (Q12, G10, D-4, Q14).
- **A tickless CPU** unless Q-8's condition holds.

---

## 2A. Ranked packages

Ranked by the owner's order of need: **dark playback first** (power, no BCM2722 dependency), then the BCM2722 display track as its prerequisites land from the device session, then the COP front end last (riskiest). Step numbers are placement §8's.

| Rank | ID | Package | Step | Needs the BCM2722? | Wave |
|---|---|---|---|---|---|
| **1** | **19-1** | **Dark playback on one core:** (a) quiet pollers; (b) drive policy; (c) codec core by screen state; (d) park the COP (K-1); **with** (e) art decode-once on the CPU and (f) M-1 (overnight baseline) and M-2 (codec A/B, ABCAB) | 0–4 | **No** | 0, then 1 |
| 2 | 19-2 | L2 (our image as the default display driver) **and the drift on the BCM2722** (#119), with the screen-truth contract (§3.6) | 5 | yes | 1 |
| 3 | 19-3 | The lit clock at 24 MHz | 6 | via 19-2 | 1 |
| 4 | 19-4 | A retained screen: second frame, slides, glides, `PRESENT` (#75, #76, #78 b) | 7 | yes | 2 |
| 5 | 19-5 | The marquee as a BCM2722 pan | 7 | yes | 2 |
| 6 | 19-6 | JPEG decode on the BCM2722 (lit or charging only, D-3); 19-1e is its CPU path | 10 | yes | 0 (vc2sim spike), 3 |
| 7 | 19-7 | Cover Flow foundations | 11 | yes | 4 |
| 8 | 19-8 | Back-end services off the UI thread and the front-end boundary, still on the CPU | 8 | no | 2 |
| 9 | 19-9 | The UI on the COP (`HAVE_COP_FRONTEND`, Q6) | 9 | for the port lock | 3 |
| 10 | 19-10 | Photos and video thumbnails (design only) | – | yes | 4 |
| 11 | 19-11 | Glyph atlas, only if Wave 0 profiles still show text | – | yes | 4 |
| 12 | 19-12 | Tickless CPU, only under Q-8 | 12 | no | later |

Each package gives its savings (CPU/COP time, RAM, mA, tagged), prerequisites, deciding measurements (§6) and fallback. **Every package keeps a CPU path; none removes it.**

### 19-1. Dark playback on one core — rank 1 (placement steps 0–4)
**Steps 1–4 deliver "dark playback = the CPU and the WM8758, the drive in bursts, the COP parked, the BCM2722 cut" without moving the UI and without any BCM2722 work.** Own mini-phase and review; first to merge after P12.

- **19-1a Quiet pollers (step 1).** `audio`, `ata` (the storage thread), `tagcache` and `power` move to deadline waits (18-A's pattern):
  - `audio` sleeps between tracks until the next handle event, a skip or the watermark (owner B);
  - `ata` until `min(spindown, stor_pwr off, idle callbacks)`, in `storage.c` (owner A);
  - `tagcache` wakes only while work is owed: each producer kicks it (owner B for `tagcache.c`; the producer kicks in shell files are C's, after P12 merges);
  - `power` (owner A, `firmware/powermgmt.c`) waits **`min(10 s, the sleep timer's end, the idle-off deadline)`** while playing dark above 15 % with no charger, batcal's sample folded into the step; **the battery filter's 8-step catch-up cap is raised** to match, so its time constant holds at the longer step (review M2). **It lands after `p12-alarmfix` merges**, which is editing `handle_sleep_timer()` now.
  - **Saves:** ~5–6 wakes/s [silicon counts]; mA small [hypothesis]. **Fallback:** each thread's old timeout under a developer row.
- **19-1b Drive policy (step 2),** on #114 and `p18-refill` (both now on `p12`). **The 3 s dark hold has landed** (owner, 2026-10-23), so most of the ~5 mA drive item [estimate, placement §5.2] is already banked, unmeasured. What 19-1b adds: fills to the ceiling with whole tracks ahead (metadata and art handles included), so a track change reads nothing; pending writes ride the refill's power-on. **Target:** ≤ 6 power-ups/h and ≤ 1 % `stor_on_pct` (FLAC, 64 MB). **Saves:** the remainder, ~1 mA [estimate]. Flash only (Q-5). **Prerequisite:** M-3 on the 3 s build. **Fallback:** `stor_pwr.h`'s constants (the hold back to 3 s / 60 s; fills as `p18-refill` left them).
- **19-1c Codec placement by screen state (step 3)** (owner B). Four deliverables:
  - **Owner, 2026-10-24: decoding is pinned to the CPU (PP1) permanently, with no CPU↔COP migration; the UI moves to the COP (19-8/19-9, see `docs/design/p19-ui-on-cop.md` on p19-uicop). 19-1c ships the CPU plain path (no COP view, MP3 synthesis inline) plus D; the screen rule and COP decode remain developer A/B rows for M-2 only. Q1 decided.**
  - **(i) The core per run, without a reload (review N4).** The codec stays loaded; **the core is chosen per `Q_CODEC_RUN` in `codec_cop_run()`** (`codec_cop.c` :484): dark → the CPU (upstream's path), lit → the COP (Fit-2's lit win, until the UI moves). Four changes:
    1. bind `ci_cop` whenever the COP is available, separately from "this run is on the COP";
    2. `codec_cop_reserve()` (:110–113) always reserves the COP stack;
    3. the CPU branches call `sync_fields()`. **Today `t_advance_buffer`/`t_seek_buffer` on the CPU leave `ci_cop.curpos` stale, a latent bug in the current switch-off path too**; fixed and host-tested here;
    4. the id3 alias is set per run.

    **Re-selection points:** playback start, a codec change, and **each track boundary** (a new `Q_CODEC_RUN` with no gap). **No codec reload ever** (the fallback that reloaded the codec is dropped: `codec_load_file()` would power the drive up in the dark). Never mid-track. **"Screen state" for the 30 s rule (review M8):** *dark* = the backlight is off **and** the panel is off after the unlit face's window (`lcd_sleep()`), its 30 s timer starting at `lcd_sleep()`; *lit* = the backlight is on, its timer starting at backlight-on. A boundary moves the decode only when the state has held ≥ 30 s at that boundary.
  - **(ii) "D": a wake at the low watermark instead of the HZ/20 poll (review N2).** When pcmbuf is full the codec waits with `queue_wait_w_tmo(..., HZ/20)` (`codec_thread.c` :236–262), at least 20 wakes a second. Replaced, under `HAVE_IPOD_UI` (`codec_thread.c` and `pcmbuf.c`, ledger rows), by a wait that `pcmbuf.c` wakes when the buffer drains to its low watermark; messages still wake it at once. **Without D the ≤ 5 wakes/s gate cannot be met**; it applies to B+D only.
  - **(iii) MP3 synthesis by the same rule (review N3).** `mpa.c`'s `MPA_SYNTH_ON_COP` (:28–29, the thread at :336) puts MP3's synthesis filter on the COP whatever the screen state. A runtime switch in the codec API that `mpa.c` reads **at track start** (dark: synthesis inline on the CPU; lit: on the COP as today) makes MP3 obey 19-1c. An upstream edit under `HAVE_IPOD_UI`, ledgered; **not a layering breach** (the codec reads a field of its own API, nothing from `apps/`). Owner B.
  - **(iv) Lit responsiveness while the CPU decodes** (until the next boundary): FLAC is ~55 % of the CPU at 24 MHz [silicon, session 36]; the budget is list navigation within one frame and glides at 30 fps with the existing boost; boost time per lit minute reported; B8 gets a CPU-decode lit arm.
  - **Which formats the COP-0 gate covers:** the light formats that decode on the CPU when dark: **MP3 (with iii), FLAC, ALAC, WavPack, PCM/WAV/AIFF**. Heavy formats (AAC, Vorbis, WMA, Opus) stay on the COP until the UI moves and are reported, not gated.
  - **Saves:** ~5 MHz of active cycles for FLAC [hypothesis, §1A], ~80 wakes/s with D; **~0.5–3 mA** [estimate, ±3×]. **Prerequisite:** M-2. **Fallback:** `codec_cop_select()` on and D off (developer rows, and a marker file at boot).
- **19-1d Park the COP: K-1 (step 4)** (owner A: `kernel-pp.c`, and `firmware/kernel/thread.c` with the queue code, which `p12` changed). The COP parks with no runnable thread and no pending timeout: the CPU stops pulsing it every tick (`kernel-pp.c:36`), and anything that makes a COP thread runnable calls `core_wake(COP)` (a queue post, a semaphore release, a deadline the CPU's tick finds due for the COP). **Saves:** 100 pulses/s; a parked ARM7TDMI core's current is unknown [hypothesis], M-1 answers. **Prerequisites:** a host test ("a post to a sleeping COP thread wakes it within one tick"); a device soak with the COP decode on; P18's agent A reviews. **Fallback:** the per-tick pulse (one `#define`).
- **19-1e Art decode once on the CPU** (owner C). One JPEG decode feeds every output size (55², 142² and its card, 300², the blurred fill) in MCU-row order through one scaler pass. **Saves:** half to two-thirds of art-build decode time [estimate]; RAM unchanged. Never in dark battery playback (D-3). It is also 19-6's CPU path and reference. **Pinned base:** `artcache.c` and `jpeg_load.c` at `p12`'s tip `c00496f6fa`. **Fallback:** today's per-output decode (a developer row); goldens identical either way.
- **19-1f Measurements first** (on a `HAVE_DEVSTATS` developer build, which the per-thread wake counts need):
  - **M-1** overnight **before** (the 3 s build, `c00496f6fa`) and after 19-1a–e.
  - **M-2** arms **A B BD C A B BD** (30 min each, the 40–80 % band, a lit stretch per arm with boost time reported): A = COP decode (today); B = CPU decode with the COP parked (or "COP decode off" with the pulse still on if K-1 is not ready, recorded as such), the HZ/20 poll kept; **BD = B with D**; C = BD with 80 MHz bursts. One FLAC album, then **one MP3 album** (with iii) and one AAC album.
  - **M-3** drive-wake attribution on the 3 s build.
  - **New log lines (review M3):** `m0 cop wakes n=` (COP wakes per window); a **windowed** PLL-off share (`clk win s= pll_off=%`, from raw ticks per window) in place of the cumulative `clk stats idle=%`; and **19-1's own dark BCM2722 counter**, `lcd wakes dark=N` (`lcd-video.c`: LCD wakes while the backlight is off), since `bcm power ups dark=` arrives only with 19-2.
- **Expected** [estimate]: ~31–36 mA on the 3 s build − ~1 (whole-track fills) − ~0.5–3 (one core, D) − a little (pollers) = **~27–34 mA**. It may not reach 25 mA; the unknown base needs M-1's state differences (Q4).
- **Acceptance (gated):** M-2's BD-vs-A difference in BD's favour (or within the A–A spread); **0 underruns**; **PLL off ≥ 90 % of dark time** (windowed line); **0 dark BCM2722 power-ups** (`lcd wakes dark=0`); for BD on the light formats: **thread wakes ≤ 5/s and `m0 cop wakes` 0**; drive ≤ 6 power-ups/h. 25 mA is tracked, not gated.

### 19-2. L2 and the drift on the BCM2722 (#119) — rank 2 (step 5)
- **L2:** the merged image (LCD path, video protocol and jobs, ~14 KB of job code) becomes the release display driver (#115), gated by `bcmfw/verified.txt` (stages 0–5 on the device; a new image needs a new entry). **Its PP copy is new in release:** up to **217,088 B** (the `0x35000` cap) with the resident video image (Q2), or ~20–25 KB with the display-only image once the warm swap is proven (B13). **The floor must count it:** `HAVE_BCM_JOBS` is target-only, so the simulator's floor gate cannot see the block. A simulator-only define, **`HAVE_BCM_JOBS_MODEL`** (set by `tools/configure` for `sim-ui` builds with `--ipodui`), makes the host model reserve `BCM_RESIDENT_BYTES` (the image's size from the `bcmfw` build) at shell start; the device logs `floor dev audio= bcm_res=` at boot (review M5).
- **Boot sequence under L2 (review L2):** (1) the firmware brings the BCM2722 up with the small boot image, which shows the boot glyph; the battery guard decides before storage; (2) the disk mounts; (3) the resident image is read in one bulk read and swapped in: **today a cold restart, a 0.37–0.62 s dark gap after the glyph**, and 20–40 ms if the warm swap works [hypothesis]; (4) **USB connected at boot:** the swap waits until the USB session ends (no disk reads at USB entry, owner rule), and the USB screen runs on the boot image.
- **The drift as jobs** (jobs.md §5): `nav_core.c`'s drift policy stays on the CPU and chooses the jobs; covers in `J_CLS_DRIFT` slots in BCM2722 SRAM; `FADE`/`AVG` on RGB565 with **the split shadow** (`J_OP_SHADE`, `J_FADE_SHADE_B`, 3d3f4d5db1) so that `ui_shadow_apply()` and `nav_core.c`'s drift are reproduced exactly (review H2). **Uploads** go by E2 straight from `pv.slot`, `pv.slot2` or a bank entry, already RGB565 at a 600 B pitch, so **there is no separate staging buffer** (D-4). **Other cover users** keep their blocks: the Audiobooks drift uses the same jobs and fallback; `np_cover_read()` (`preview.c` :1324) keeps reading into the slot; the Videos poster keeps its CPU path in P19.
- **The unlit face (review M6; `power-contract.md`, e608c4a27b):** for the first ~90 s after the backlight goes off the unlit face keeps the panel up. That window is **C2** (idle, its core asleep once 5i passes) with a **C1 burst for each minute update**; every slot survives it and a wake inside it is not cold. Animations stop when the face appears (the drift's `ANIM_STOP` runs **before** the face's first push). After the window: `lcd_sleep()` and C0. **G12's "dark" is panel-off**, so the window is not counted as a dark power-up.
- **Saves:** 10–25 % of the CPU while lit on split screens → ~0.3 % [estimate]; BCM2722 cost a 2.87 ms half step plus 8.6 ms of bus with its core free [bcm estimate]. **RAM: none** (D-4). It unblocks 19-3.
- **Prerequisites:** the merged image; **B3: the real shell driving the merged image, with the drift goldens identical on both backends** (stage 5b's old port predates the shadow and P16's drift and is not used); BCM #53 or a `BLIT`; the screen-truth contract (§3.6) with its scripts.
- **Fallback (D-1):** a clear `J_OPS` bit, a slow or failed job, a BCM2722 failure or the developer row → **the still cover**, drawn from `pv.slot` (no disk), logged `drift still why=`.

### 19-3. The lit clock at 24 MHz — rank 3 (step 6)
`CP_LIT_STREAM_HOLD` = 0 once 19-2 removes the only sustained CPU push stream. **Saves:** 1–3 mA while lit [estimate]. E2's rate at 24 and 30 MHz is unmeasured (B2). **Prerequisites:** 19-2; B8 (with its CPU-decode lit arm). **Fallback:** the developer switch back to 30 MHz.

### 19-4. A retained screen (#75, #76, #78 b) — rank 4 (step 7)
- The BCM2722 keeps `J_SURF_FRONT` and `J_SURF_BACK`. A push or pop renders the new page once on the CPU, uploads it once by E2 (~15 ms), and `ANIM SLIDE` moves the two at 30 fps with the shell's per-frame offsets. A glide `SCROLL`s the held rows (**vectorised**: 0.34 ms against 25 ms scalar [bcm estimate]); the CPU uploads only newly exposed rows. `PRESENT rect [TE]` replaces per-row address writes.
- **Saves:** the 60–100 ms slide set-up, ~6 full pushes per slide and the glide's 14–27 ms E2 push per frame; slides and glides stop boosting [estimate]. **RAM: none** (Q12: the strip stays; the framebuffer stays, G10). **Smoothness is the win.** The first moving frame is logged `anim first ms= kind= src=bcm|cpu` (review L3).
- **Stateful ops and faults (review M3):** a full ring, `OFF` or a timeout during `SCROLL` or `ANIM` invalidates the retained surfaces; `bcmjob_resync()` redraws the whole frame from the PP framebuffer (which holds the final framing, §3.6); a death mid-slide jumps to the final frame.
- **Prerequisites:** 19-2, §3.6; B1, B4. **Fallback:** the executor on the CPU, frame for frame.

### 19-5. The marquee as a BCM2722 pan — rank 5 (step 7)
The CPU renders the label once (F2) and uploads it into `J_CLS_STRIP`; `ANIM PAN` moves a window at 20 steps a second. **Saves:** ~5 % of the CPU while a long title is selected and lit → ~0 [estimate]. **Prerequisites:** 19-2's slots, BCM #53. **Fallback:** F2's CPU path.

### 19-6. JPEG decode on the BCM2722 — rank 6 (step 10)
A baseline decoder (Huffman on the scalar core, an 8×8 IDCT vector kernel, the YUV→RGB tile kernel, an area downscale and an ordered dither in MCU rows), every output size in one pass; `J_OP_DECODE` (`0x30`), `J_T_DECODE` 2 s, `bcm_power_want(decode)` only on a charger or lit (D-3; `BCMJOB_REFUSED` on battery in the dark). **Saves:** art builds from 26–46 s per cover to ~0.1–0.5 s at 150 MHz [estimate; the vc2sim spike measures]. **RAM:** at most the ~164 KB decode block, only once `DECODE` is the default and the CPU path takes its block on demand in a maintenance window (review M8). **Fallback:** 19-1e on the CPU, the reference.

### 19-7. Cover Flow foundations — rank 7 (step 11)
16 covers of 128² in `J_CLS_COVERFLOW` (BCM2722 SDRAM, on demand while Cover Flow is open); `TRIS` (`0x32`) on `r3d`'s span code; an availability query (`J_OPS`, the per-boot failure mark, and SDRAM brought up). **Saves:** ~512 KB never reserved (not counted in any net figure). **Fallback (D-1):** the styled pop-up when the BCM2722 or its SDRAM is unavailable (P7 owns the look).

### 19-8. Back-end services off the UI thread, and the boundary — rank 8 (step 8)
The services in placement §3.2 leave the UI thread while it is still on the CPU; then the interface (placement §3.3) and the coherency rules (§3.5) become `apps/ipodui/fe_be.h`, enforced by a link check. On the CPU first, so regressions show up as ordinary bugs. **Prerequisites:** P12 merged; P12's Class A deny list in `modpak.py check`.

### 19-9. The UI on the COP — rank 9 (step 9)
The shell's loop as a COP thread, its ~8 KB stack in DRAM (IRAM is full); the CPU keeps interrupts, playback, storage and `vplayer`; **the COP owns the BCM2722 ring** (§3.5). **Q6:** kept only if M-1 shows ≥ ~2 mA lit gain. **Prerequisites:** 19-1d, 19-8; M-4, M-5; B10. **Fallback:** `HAVE_COP_FRONTEND` off.

### 19-10, 19-11, 19-12
- **19-10** (design): a `DECODE` with output sizes, a full-screen decode into the BCM2722's frame, an I-frame grab through its H.264 decoder.
- **19-11:** `GLYPHS` (`0x31`) only if Wave 0 profiles still show text after F1/F2.
- **19-12:** placement §5.4, only under Q-8, with its own review.

### 2A.1 RAM against release `fw-ui` (second review N1; D-4, Q12)

**RAM is not a P19 goal.** With D-4 the drift offload frees nothing, so the table is a cost table.

| Item | Release `fw-ui` today | After P19 | Every unit (64 MB, 32 MB) |
|---|---|---|---|
| `pv.slot`, `pv.slot2` (180,000 each); #92 bank (720,976, 64 MB only) | as today | unchanged (D-4, Q14) | 0 |
| Strip (`UI_STRIP_SIZE`) | 180,000 | unchanged (Q12) | 0 |
| LCD framebuffer | 153,600 | unchanged (G10) | 0 |
| Executor state | 0 | ~3 KB in the driver's block, **not a static** | −3,000 |
| **Resident image, video (Q2 default)** | **0** | ≤ 217,088 | −217,088 |
| **Net with the video image** | | | **≈ −220 KB** |
| Resident image, display-only (after B13) | 0 | ~25,000 | −25,000 |
| **Net with the display-only image** | | | **≈ −28 KB** |
| Later: art decode block (19-6 default) | ~164,120 | taken on demand | ≤ +164 KB |
| 19-9 only: COP UI stack and shared records | 0 | +8 KB, ≤ 768 B | −8.8 KB |

**Against the review's figures.** The second review gives 64 MB ≈ −40 KB (video image) and ≈ +152 KB (display-only), and 32 MB two-slot ≈ −220 / −28 KB. The 32 MB figures match this table. The 64 MB pair implies about 177 KB more returned on 64 MB, which this brief cannot derive under D-4's default (it would need `pv.slot2`'s 180,000 B released on 64 MB, where the other set's covers also sit in its bank). **Flagged for the orchestrator;** this brief uses the default as stated. `memory-contract.md` §5 (e608c4a27b) assumes the PP keeps no cover copy and is being amended by the BCM session (§3.2).

**Cover Flow's 512 KB is in no row:** it is a reservation P7 never makes, not a saving. **Every floor holds:** the worst case (any unit, video image) leaves about 11.34 MB on 32 MB and 44.17 MB on 64 MB against 7.5 / 39 MB [estimate from the npfix minimums]. On 32 MB the display-only image (B13) is the likely default (Q2).

---

## 3. Contracts as code

All BCM2722 contracts are pinned at **`p19-bcm-prep` e608c4a27b** (contract 4 at dba1697bc2, the gap closures at b1ac4de32e, W0-1 done at ad243fc6ef, and the rev 1a review's BCM items: H2's shade op at 3d3f4d5db1, M3/M4 at 812ee1d828, the contract text for H2, M3, M4, C2 and M6 at e608c4a27b; `jobs_test` passing). Each lands on the phase branch before fan-out with its host test and the shared stub, and names its trace lines (shell: `UPDATE_TRACE`/`DEBUGF`, never `logf`).

| # | Contract | Files | Status |
|---|---|---|---|
| 1 | Job interface ("JB") rev 0.1 | `bcmfw/jobs.h`, `docs/research/bcm2722/jobs.md` | draft; gaps closed (§3.7) |
| 2 | Memory ownership and the merged image | `docs/research/bcm2722/memory-contract.md` | draft |
| 3 | Power states | `docs/research/bcm2722/power-contract.md` | draft |
| 4 | **PP-side API, executor and host model** | `bcmfw/jobs/bcmjob.h`, `jref.{c,h}`, `jmodel.c`, `jmix.h`, `jtest.c` (1,332 cases, pinning `SHADE`, raw `FADE` and `AVG` against verbatim copies of the shell's code), `jmtest.c` (39 cases: BCM-timed animations, resync, ring full, death mid-slide, busy and cut mid-scroll, suspend/resume); the scene agrees on the Mac and in vc2sim | **done** [bcm] |
| P | **Placement per power state** | `docs/design/p19-placement.md` (merged) | the reference |
| 5 | Screen truth (P19's own) | §3.6 | written with 19-2 |
| 6 | Front-end/back-end boundary (P19's own) | `apps/ipodui/fe_be.h`, `docs/data-paths.md` R7 | written with 19-8 |

### 3.1 Job interface and API (contracts 1 and 4)
**What they fix:** jobs ride the video protocol's rings as opcodes `0x20–0x3F`; discovery `HELLO` → `HP_CAP_JOBS_2D` → `HP_JOBS` (`0x138`) → `J_VERSION` (`'JB'`; a major mismatch means the CPU path everywhere) → `J_OPS` (a clear bit means that op's CPU path) → `J_EPOCH`, `J_MODE`, the slot table, with the job block at `0xDE000`; in-order execution, a pollable `FENCE`; slots by `ALLOC(class, size, key)` → upload → `COMMIT(fnv)`; jobs `BLIT`, `FILL`, `FADE` (with `J_FADE_SHADE_B`), `AVG`, `SCROLL`, `PRESENT [TE]`, `ANIM SLIDE/PAN` (per-frame offsets, ≤ 50), `ANIM_STOP`, `SHADE`, `STATS`, `RESET`; `DECODE`, `GLYPHS`, `TRIS` reserved; deadlines `J_T_SMALL` 5 ms, `J_T_DRAW` 8 ms, `J_T_PRESENT` 40 ms, `J_T_FENCE_BASE` 20 ms, `J_T_DECODE` 2 s. **The shell's API (`bcmjob.h`):** `bcmjob_have(op)`; `bcmjob_submit()` never blocks (`OK`, `BUSY`, `OFF`, `NOJOB`); `bcmjob_fence()`/`bcmjob_wait()`; `bcmjob_slot()`/`bcmjob_upload()` (E2 for 16 rows or more, then `COMMIT` with FNV-1a; skipped on a key hit); `bcmjob_epoch()`/`bcmjob_on_epoch(fn)` (called in thread context when the driver cuts or restarts the BCM2722); `bcm_port_lock()`/`unlock()`; `bcm_power_want(why, hold_ms)`/`release()`; `bcmjob_dark_ups(why)`. Trace lines: `bcmjob submit op= tag= n=`, `bcmjob fence seq=`, `bcmjob fault op= why=busy|off|nojob|timeout|failed|refused`, `bcm power state= why=`, `bcm power want why=`, `drift still why=`, `coverflow unavailable why=`.

**Resync (review M3, `bcmjob_resync(rect)`):** any `BUSY`, `OFF` or `TIMEOUT` during a frame resyncs the whole frame from the PP framebuffer (the CPU path draws it, then one push), so a stateful op (`SCROLL`, `ANIM`) cut short never leaves a half-moved surface; a death mid-slide jumps to the final frame (jobs.md §6).

**Failure rules:** a slow job (live heartbeat) retries once, then takes its CPU path for the boot; a dead heartbeat or the fault word is a BCM2722 failure: the boot ladder restarts it at once with the PP framebuffer as its first frame (a blank panel for 0.37–0.62 s, no text; a slow, live BCM2722 is never cut while lit); after a second failure in one boot, jobs stay off until the next boot. Video keeps its own failure rule.

**Developer stages (review M4):** the `devbcm` stages (5i, 5s, 5b, 3d) and the Debug "BCM boot path" screens swap images and write `0x106000`/`0x010000`, inside the merged map. They call **`bcmjob_suspend()`** around their take and restore: `bcmjob_have()` is false, every caller takes its CPU path, slots are forgotten. Resume runs `HELLO`, starts a new epoch, pushes the PP frame, then calls `on_epoch` (812ee1d828).

### 3.2 Memory ownership (contract 2)
One merged image (≤ `0x35000`); SRAM maps per mode (UI / VIDEO / GAME): UI pools in the 618,496 B arena with 4 KB spare (`J_CLS_DRIFT` 2 × 180,000 B, `J_CLS_STRIP`, `J_CLS_THUMB`, `J_CLS_APP` 56 KB in 8 slots for Class B), `J_CLS_SCRATCH`, `J_SURF_FRONT` `0xE0000`, `J_SURF_BACK` `0x105800`; SDRAM pools on demand (`J_CLS_COVERFLOW`, `J_CLS_PANEL`), evicted at a video `OPEN` or a game; class budgets as constants in `jobs.h` (`J_BUDGET_*`, `J_SLOTS_*`); nothing on the BCM2722 is the only copy.

**Restated against release `fw-ui`** (e608c4a27b, review C2): the resident copy is L2's own cost, ≤ 217,088 B (the `0x35000` cap), new to release; no strip saving; nets about **+143 KB (64 MB)** and **−37 KB (tight 32 MB)**, or **+335 / +155 KB** with G9's display-only image and a warm swap. **Q13 is decided (D-4):** the PP keeps `pv.slot`, `pv.slot2` and the #92 bank on every unit. `memory-contract.md` :77, :83 and `jobs.md` §7 still say the PP keeps no copy and reads `panels.pak`; **the BCM session is amending them** (coordinator, rev 1c). `J_CLS_PANEL` is unused in P19.

### 3.3 Power states (contract 3) and the BCM session's answers
C0 cut, C1/C2 lit active and idle, C3/C4 dark with the panel in light or deep standby, C5 dark with the panel off and the BCM2722 up. **Dark means cut;** a standby only after stage 5s proves it, for `BCM_STANDBY_WINDOW_S`; 27 MHz in UI mode, 150 MHz only for video, Class C or a lit or charging decode; core sleep only after 5i. **P18:** cut at sleep entry; slots come back lazily after the first frame.

**Answers** (placement §9.1 [bcm]): a wake from a cut in ~0.32–0.34 s with the upload overlapped; **a lit panel with the BCM2722 cut is probably impossible** (it drives the panel's reset, GPIO 48, and its bus), so **the lit-idle saving is C2 with its core asleep** (after 5i), making 5i the lit state's most valuable measurement; **zero BCM2722 wakes in dark playback when cut** (`bcm_power_want` is the only dark request: a charger decode, a paused Class C game, a video's own sleep); plausible currents, all unmeasured (#12): the BCM2722 plus panel ~10 mA busy-polling at 27 MHz, its core alone 2–5 mA at 27 MHz, under 1 mA asleep.

### 3.4 The host model in the shell simulator (contract 4)
`uisimulator/common/sim_bcm_job.c` (owner T) wraps `jmodel.c`: pixels through the executor (goldens stay exact), modelled time (a job `JM_COST_*`, `PRESENT` 14 ms, a cold start 400 ms, E2 at 11 MB/s), faults from scripts (clear, slow, fail, dead, cut) **plus a ring-full injection** (M3), per-ms power accounting and dark power-ups. Under the simulator-only `HAVE_BCM_JOBS_MODEL` it also reserves `BCM_RESIDENT_BYTES` at shell start, so the floor gate counts the resident image (§2A.1).

### 3.5 Front-end/back-end boundary and coherency (P19's own, with 19-8)
1. **One owner core per structure;** the front end never dereferences back-end cached memory.
2. **Small shared state** in one `SHAREDBSS` "now" record with a seqlock; identity separate from validity (a track id vs the seqlock, `process.md` item 3).
3. **Bulk tables** by generation hand-off (the CPU builds, writes back, bumps; the COP writes back and drops before its next read); never a bare discard.
4. **buflib:** the COP never holds a raw pointer across a yield; it reads only pinned blocks or copies.
5. **File I/O** only through the CPU's services, in bulk reads.
6. **Atomicity (review M7):** an uncached alias makes a word visible to both cores, **not** a read-modify-write atomic. Each shared word has **one writer**; anything with two writers uses a corelock (or Peterson's algorithm); `SWP` only if PP #8 answers yes. Counters written by both cores live in IRAM or the uncached alias as per-core words summed by the reader (D10).
7. **One port lock:** `bcm_port_lock()` (`lcd-video.c`'s mutex plus its corelock `lcd_state.cl`) covers the ring, E2 uploads, LCD commands and the video feed, because the port has one auto-increment address register.
8. **E2 and the COP's cache:** pixels the COP writes into cached memory are written back from **the COP's own** cache before an E2 piece starts; either the core that wrote them starts the piece, or it writes through the uncached alias.
9. **The COP owns the ring** once the UI moves; the CPU's video feed hands it records; `lcd_tick` serves only the CPU path.
10. **The link check:** an `apps/ipodui/` object referencing a back-end symbol outside `fe_be.h` fails `validate.sh`.

### 3.6 Screen truth: who holds the pixels of each region (reviews H1, N5, M7; P19's own, written with 19-2)
**The rule.** For every screen region exactly one place holds the truth: **the PP framebuffer** (the default) or **a BCM2722 surface** while a BCM2722 animation owns that region (the drift's panel, a marquee row, a slide or glide in flight). Owned regions are marked in a per-band "BCM-owned" mask.

**Readers.** Every reader of the framebuffer as "what is on screen" first calls **`ui_fb_settle(rect)`**:
- If no owned band overlaps `rect`, it returns at once (no `ANIM_STOP`, no wait).
- Otherwise **the CPU computes the framing from its own time-based drift state** (`nav_core.c`: cover, pan offset, fade weight, the shadow) or the marquee's offset or the slide's final page, **writes that framing into the PP framebuffer** from data it already holds (`pv.slot`, the label), and issues `ANIM_STOP` **only to confirm** (the returned frame count must match the computed framing; a mismatch is logged, the computed framing wins).
- **Timeout:** `ANIM_STOP` waits `J_T_SMALL`; on `TIMEOUT`, `OFF` or `BUSY` the settle proceeds with the computed framing and calls `bcmjob_resync(rect)`, which pushes it.
- It clears the mask for `rect`. Nothing is read back across the port (D7).

**Writers (review N5).** Any CPU push of the full screen or of a rect over an owned band would paint stale pixels over the BCM2722's animation. **Every CPU push (`lcd_update()`, `lcd_update_rect()`, `ui_lcd_push()`) clips against the owned mask, or the caller settles first.** Named: the slide path's `lcd_update()` (`ui_main.c` :505) and every `ui_lcd_push()` caller.

| Reader or writer | Where | Gets pixels by |
|---|---|---|
| Panel highlight cross-fade | `ui_panel_fade()` (`preview.c` :472) | settle, then the CPU fade; or the BCM2722 `FADE` with `J_FADE_SHADE_B` when the drift is on it |
| The drift's cover fade | `preview.c`, `nav_core.c` | owned by the BCM2722 while on; the CPU path draws from `pv.slot` |
| In-place shadow | `preview.c` :451 | settle first (it re-darkens framebuffer columns in place) |
| Slideshow memmove | `preview.c` :1943–1956 | settle first |
| Slide capture | `ui_main.c` :491 (`rect_xfer`) | settle; with 19-4 a BCM2722 slide needs no capture (`J_SURF_FRONT` holds the page) |
| Slide push | `ui_main.c` :505 (`lcd_update()`) | clip against the mask, or settle first |
| `ui_panel_sync()` | `ui_main.c` :331, :506, :518 | settle |
| `ui_panel_draw_now()` | `menu.c` :668, `settings.c` :483, `language.c` :110, `checklist.c` :257 | settle, then the CPU repaints the panel's known framing |
| Scrollbar blend | `list.c` :655 | settle (a blend over the framebuffer) |
| Lyrics row | `nowplaying.c` :1819, :1823 | settle |
| `ui_lcd_push()` callers | `anim.h` :82 and its callers | clip against the mask |
| The unlit face's appearance and dismiss | the face's paths | `ANIM_STOP` comes before the face's first push (M6); settle at dismiss |
| `bcm_video_hand_back()` | the video player's return | settle before the hand-back repaints the whole frame; the session has already bumped the epoch |
| G2's restart (first frame) | the boot ladder | a dead BCM2722 cannot be asked: a settle with `ANIM_STOP` skipped, owned bands repainted from the CPU's own data |
| P18 wake | `sleep.c` | the BCM2722 was cut at entry after a settle; the framebuffer is truth |
| Module snapshots | `modload.c` :1572, :1645 | settle |

**Enforced by a gate check:** a `validate.sh` grep, run in the full gate and `--quick`, requires that every `FBADDR` / `lcd_panel()` site in an ownable band either calls `ui_fb_settle()` in the same function or carries a `/* not ownable: <why> */` note; a new site without either fails. **Coordinate with the `gate-speed` branch,** which also edits `validate.sh` and `check_trace.py` (owner T, one reviewed commit).

**Scripts:** one `ipodui-p19-truth-*` script per row, each with a fault injected mid-drift (and mid-slide for the capture and the slide push), checking the settled frame against the CPU-path golden. Checklist item 12's twin covers readers **and writers** (§7).

### 3.7 Gaps: all closed or placed

| # | Closed in |
|---|---|
| G1 full ring | `BCMJOB_BUSY` at once; the frame takes its CPU path (jobs.md §3, §10) |
| G2 failure while lit | restart at once with the PP framebuffer first; a slow live BCM2722 never cut while lit (jobs.md §6); the shell side is §3.6's restart row |
| G3 one lock | `bcm_port_lock()` (the LCD mutex plus corelock) |
| G4 the executor in the PP build | `HAVE_BCM_JOBS`, from `firmware/SOURCES`, no IRAM: **3,640 B Thumb**, ~3 KB of state held by the driver [measured]; **the state is not a static** (≤ 1 KB rule): a block in the job client's allocation at shell start |
| G5 one arithmetic | `jobs/jmix.h` for `FADE`, `AVG` **and `SHADE`** (`J_OP_SHADE` `0x2D` is `ui_shadow_apply()` exactly; `FADE` with `J_FADE_SHADE_B` is `ui_panel_fade()`'s raw fade; the half window); `jtest` pins each against verbatim copies of the shell's code. **Widened scope:** the shell's `ui_shadow_apply()`, `xfade_rect()` and `AVG2` all call `jmix.h` (a P19 shell change), and `nav_core.c`'s drift policy chooses the jobs |
| G6 slide easing | per-frame offsets in `J_ANIM_SLIDE` |
| G7 Class B pool | `J_CLS_APP`, 56 KB, 8 slots |
| G8 budgets | `J_BUDGET_*`, `J_SLOTS_*` in `jobs.h`; the UI pools fit the arena with 4 KB spare |
| G9 display-only image | ~20–25 KB, saves ~190 KB; needs a warm swap (stop, upload, start, GPIO 48 held), untried [hypothesis]: 20–40 ms, else a cold 0.37–0.62 s. **Bench row B13**; Q2 |
| G10 strip and framebuffer | no strip or framebuffer saving claimed (Q12) |
| G11 power requests | `bcm_power_want()` is the only dark request; everything else follows the backlight and the mode |
| G12 dark power-ups | hourly `bcm power ups dark=N decode=a game=b video=c`; must read 0 in dark playback (19-1's acceptance) |
| G13 epoch notice | `bcmjob_on_epoch(fn)` in thread context |
| Review M3 / M4 | `bcmjob_resync(rect)`; `bcmjob_suspend()` around developer stages (812ee1d828) |
| G14 PP side per state | placement §2.1 |

### 3.8 Shared stub
No `HP_CAP_JOBS_2D`: every job takes its CPU path, the drift is still and Cover Flow unavailable (D-1). Committed with the contracts; no agent writes its own stub; the gate refuses `TEMP-` commits.

---

## 4. Waves

**Base (reviews H4, M1).** `p12`'s tip `c00496f6fa` (merged here at 854bc1b5c6) contains `p18-114`, `p18-refill` and the 3 s dark hold. `main...p12` changes `artcache.c`, `pll_idle.c`, `lcd-video.c`, `ipodvideo.h`, `firmware/SOURCES`, `firmware/kernel/thread.c`, `sim_script.c`, `check_trace.py`, `validate.sh`, `devmode.c` and `english.lang`. Every Wave 0 branch is cut from `c00496f6fa` and rebased as `p12` moves; nothing merges into P19 before P12's gate and merge. **Two other branches overlap:** `p12-alarmfix` (`powermgmt.c` `handle_sleep_timer()`) and **`gate-speed`** (`validate.sh`, `check_trace.py`, `tests/perf/floor_gate.sh`, `scripts/shots.sh`, `tests/ui/README.md`, `anim.{c,h}`, the simulator's `SOURCES`).

### Wave 0, timed by dependency (review recommendation)
| When | Item | Owner | Output |
|---|---|---|---|
| **Start now** from `c00496f6fa` | W0-1 | BCM session | the merged image within its job budget; `SCROLL` vectorised; the warm-swap stage for B13; the contract amendments (§3.2, C5 in the `bcm power state=` trace) |
| Start now | W0-7 | BCM session | 19-6's vc2sim JPEG spike, `jpeg_load.c` pinned at `c00496f6fa` |
| Start now | W0-8 | orchestrator | the bench runbook (`docs/device-session-p19-0.md`), on the 3 s build, batched with the P18 rerun |
| Start now | W0-6 | A | the device driver for `bcmjob.h` in `firmware/target/arm/ipod/video/`; `jref.c` in the PP build (G4); `lcd wakes dark=` |
| Start now | W0-5 (new files only) | T | `sim_bcm_job.c` over `jmodel.c`, `sim_cmd_bcm.c`, `HAVE_BCM_JOBS_MODEL`; the harness counts. Edits to `sim_script.c`, `check_trace.py`, `validate.sh` wait for `gate-speed` |
| Start now | W0-3a | A | K-1's host test and `kernel-pp.c`/`thread.c`; the `ata` deadline in `storage.c`; `m0 cop wakes`, the windowed `clk win` line |
| **After rev 1c** (N2–N4 written) | W0-2 | B | 19-1c (i)–(iv): per-run core choice, D (`codec_thread.c`, `pcmbuf.c`), the `mpa.c` switch; `audio`'s deadline; the M-2 arm driver from a marker file |
| **Rebase first** | W0-9 | C | 19-1e decode-once, rebased on `c00496f6fa`'s `artcache.c` |
| Rebase first | W0-4 | D | 19-1b whole-track fills on the 3 s hold; M-3's lines |
| **After `p12-alarmfix` and P12 merge** | W0-3b | A | the `power` step in `powermgmt.c` (19-1a) |
| After P12 merges | W0-10 | C | the shell's `ui_fb_settle()` calls (§3.6) and the `tagcache` producer kicks in shell files |

### Wave 1: after the bench (M-1, M-2, M-3; B1–B3, B5, B13)
19-1's defaults and its review; 19-2 (L2, the drift, §3.6) and its review; 19-3's trial (B8).

### Wave 2: after Wave 1 merges
19-4, 19-5; 19-8. The boundary and the retained screen each get their own review before Wave 3.

### Wave 3
19-6's `DECODE` on silicon; 19-9 behind its flag (M-4, M-5, B10).

### Wave 4
19-7; 19-10's note; 19-11 if needed. 19-12 only under Q-8.

---

## 5. Ownership and read lists

| Agent | Model | Owns | Reads (≤ 3) |
|---|---|---|---|
| H BCM session (#60) | opus | `bcmfw/` (the image, the job ring, `jobs/`, the memory map), `bcmfw/hostproto.h`, `docs/bcm2722-reference/`, the stages' BCM2722 side | this brief §3; `p19-placement.md` §9.1; `graphics.md` |
| A firmware-platform | opus (K-1, the driver, 19-9's kernel side), sonnet (19-1a `ata`/`power`) | the `bcmjob.h` device driver, the PP build of `jref.c` (H owns its source), `lcd-video.c` (incl. `lcd wakes dark=`), `kernel-pp.c`, **`firmware/kernel/thread.c` and the queue code** (K-1), `thread-pp.c`, `cop_job.c`, `pll_idle.c`, `clockpol.c` (since 18-5; the windowed `clk win` line), `storage.c` (the `ata` deadline), **`firmware/powermgmt.c`** (the power step, after `p12-alarmfix`), `ipodvideo.h` (`HAVE_*`) | `p19-placement.md` §3.4, §5; `data-paths.md` R1, R5, R7; `cores-and-threads.md` §2–3 |
| B playback-audiobook | opus (19-1c (i) and (ii)), sonnet (the rest) | `apps/codec_cop.c` (per-run core, `sync_fields()` fix), **`apps/codec_thread.c` and `apps/pcmbuf.c`** (D), **`lib/rbcodec/codecs/mpa.c`** and the codec API field (iii), **`apps/tagcache.c`'s wait**, the M-2 driver, `audio`'s deadline, the placement input to `clockpol` (one reviewed commit with A) | `p19-placement.md` §4; Fit-2 §5, §7; `codec_thread.c` :440–520 |
| D #114's owner | sonnet | `stor_pwr.h`, the refill shape | `p19-placement.md` §5.2; `storage-choices.md`; `hardware.md` "Wake budget" |
| C ui-fidelity | opus (§3.6, `fe_be.h`), sonnet (call sites) | after P12 merges: `preview.c`, `nav_core.c` (drift, settle), **`material.c`** (`ui_shadow_apply()` calls `jmix.h`), `nowplaying.c` (lyrics settle), `list.c`, `anim.c`, `ui_main.c` capture, `artcache.c`, `np_draw.c`, `fe_be.h`, the settle calls in `menu.c`, `settings.c`, `language.c`, `checklist.c`, `modload.c`, `devmode.c` rows, P19's one `LANG_*` string (appended) | §3.6; `p19-placement.md` §3.1–3.3; the spec file of the screen touched |
| T tooling | sonnet | `sim_bcm_job.c`, `sim_cmd_bcm.c`, the harness counts, `tests/bcmjob`, the truth and fault scripts, trace rules | §3.4, §3.6; `tests/ui/README.md` |

**Shared files:**

| File | Rule |
|---|---|
| Every file `main...p12` changes | branches from `p12`'s tip; merged into P19 only after P12's gate and merge |
| `kernel-pp.c`, `thread-pp.c`, `cop_job.c`, `stor_pwr.h` | A (D for `stor_pwr.h`); P18's code in them reviewed by P18's agent A |
| `apps/SOURCES`, `firmware/SOURCES` | additions only; the orchestrator merges |
| `apps/lang/english.lang` | one string appended after P12's block (D-1's pop-up); the orchestrator merges |
| `lcd-video.c` / `bcmfw/hostproto.h`, `bcmfw/jobs.h`, `bcmfw/jobs/` | A / H; a protocol change is one commit reviewed by both |
| `sim_script.c` registration line, `shots.sh`, `check_trace.py`, `validate.sh`, `tests/perf/floor_gate.sh`, `tests/ui/README.md`, the simulator's `SOURCES` | T, **after `gate-speed` merges** (it edits the same files, plus `anim.{c,h}`) |
| `powermgmt.c` | A, after `p12-alarmfix` merges |
| `docs/hardware.md` budget table, ledgers, `ROADMAP.md`, `docs/storage-choices.md` | orchestrator at close |
| `docs/design/p19-placement.md` | the platform session with H; P19 agents propose changes through the orchestrator |
| `docs/data-paths.md`, the hardware manuals | whoever finds a silicon fact records it in the same change, tagged |

---

## 6. Device bench plan

**One attended session** (~90 minutes: P18's rerun of `device-session-p18-0.md` Part 2, then ~30 minutes of P19 items) **plus the unattended runs.** Install only with the owner's go and a fresh `.rockbox` backup (`deploy-ipod` skill). Dark-playback items first.

| # | Measurement | How | Decides |
|---|---|---|---|
| **M-1** | **Dark-playback baseline**, before (the 3 s build, `c00496f6fa`) and after 19-1 | a `HAVE_DEVSTATS` developer build; overnight, 8–10 h from full, Hold on, Repeat all, mostly FLAC, dark; mAh through the learned table; per-state differences by 30-minute voltage A/Bs (BCM2722 C0 vs C2 lit, drive held vs off, COP parked vs pulsed, the tick, the Hold-on wheel), or a meter (Q4) | the split of ~35–40 mA; Q6, Q-6, Q-8 |
| **M-2** | **Codec A/B**, A B BD C A B BD | 3.5 h in the 40–80 % band, a fixed FLAC album, Hold on, 5 s power log, `powerlog_mark` per arm, `HAVE_DEVSTATS`; **a lit stretch per arm with boost time reported**; then **one MP3 album** and one AAC album | Q1; D; the MP3 switch |
| **M-3** | drive-wake attribution, on the 3 s build | `m0 disk wake by=`, `stor pwr on reason=` | 19-1b; Q-5 |
| B1 | stage 5i: the BCM2722 core asleep, SRAM held (#3, #50) | P18 runbook V1 | C2 asleep; 19-4 retention |
| B2 | stage 5s: current per state (#12), standby wake (#49), E2 image upload at address 0 (#51), **E2's rate at 24 and 30 MHz** | P18 runbook V2/V3 | Q-3; 19-3's E2 margin |
| B3 | **The drift on the merged image, driven by the real shell** (the 19-2 build, with a developer jobs/CPU A/B row); parity means the drift's `tests/ui` goldens are identical on both backends (shadow and P16's drift included). **It no longer uses stage 5b's old port** | the 19-2 build | 19-2 go |
| B4 | stage 3d: TE period (#52), cost per pixel (#55), 3D at 27/150 MHz | developer entry | Q7; `J_T_DRAW`; 19-4, 19-7 |
| B5 | BCM #53: a push from a texture window at a pitch | H's bench entry | 19-2, 19-5 zero-copy |
| B6 | BCM #54: texel fetch cost | timing kernel | 19-7 |
| B7 | BCM #7 / #44: SDRAM bandwidth and refresh; port contention | timing kernels | 19-7, uploads during animations |
| B8 | lit 24 MHz feasibility, **with a CPU-decode lit arm** | `lit-24mhz.md` §5.2's round on a 19-2 build | Q3; 19-1c's lit budget |
| B9 | a lit panel with the BCM2722 cut (10 s) | H's step | C2 as the lit-idle floor |
| B10 | DP-6 / PP #31: the COP touching the BCM2722 port | a COP job, read then write with readback | 19-9 |
| **B13** | **Warm swap:** stop the BCM2722 core, upload another image, start it, GPIO 48 held, panel picture kept | H's stage | Q2 (the display-only image) |
| M-4 | COP row render at 24 MHz | frame counters under 19-9 | 19-9 |
| M-5 | whole-cache write-back + drop cost per core | timed loop in the sleep lab | 19-9 |
| M-6 | *(optional)* 32 KB PCM chunks in dark play | underruns at skips | FIQ rate |
| B11, B12 | *(optional)* E3 lab (#83); PP #8 `SWP` | their rows | – |

Every row is designed so that "nothing happened" cannot pass (`data-paths.md` §0 rule 5).

---

## 7. Device-reality checklist
1. **Compiled features:** `HAVE_COP_PARK_IDLE`, the screen-state codec selection and `HAVE_COP_FRONTEND` are target-only (the simulator has one core); BCM2722 jobs only with `HAVE_BCM_JOBS` (target) and through `jmodel.c` in the simulator; BCM2722 code itself is tested in vc2sim and on the bench. Stock builds compile none of it.
2. **Memory:** 19-1 adds nothing. 19-2 adds the resident image (≤ 217,088 B, or ~25 KB, §2A.1) and ~3 KB of driver state at shell start, both counted in the floor (`HAVE_BCM_JOBS_MODEL`'s reservation in the simulator; `floor dev` on the device); it frees no cover memory (D-4). 19-9 adds 8 KB plus ≤ 768 B. CPU-path buffers not already reserved are taken through `maint_take()` in a window. The floor gate adds "BCM2722 failed, drift still, while playing" (D-1 takes no RAM back). Each package reports the floor-50k minimum before and after.
3. **CPU:** instruction counts per package; 19-1c reports CPU load at 24 MHz per format (FLAC ~55 % [silicon]; hi-res FLAC unmeasured) and lit boost time while the CPU decodes; submission is O(1).
4. **Storage:** 19-1b: ≤ 6 power-ups/h on the 3 s hold; nothing opens the drive in dark play except the refill, the alarm record and the 0 % latch; 19-1c never reloads a codec (no `codec_load_file()` in the dark); D-3 keeps art and database work out; with D-4 no fallback, repaint or re-upload needs the disk; 19-6 reads each JPEG once.
5. **LCD:** pushes become jobs; E2 the bulk path; frame budgets in §8; the dark display costs nothing (cut).
6. **Threads and locks:** 19-1a changes wait conditions only; K-1 parks only with nothing runnable and no timeout, and every post wakes the COP; `bcm_port_lock()` is the one port lock; nothing submitted from interrupt context; the job client suspends around developer stages (§3.1).
7. **Splashes:** none, with one owner-approved exception (D-1's Cover Flow pop-up, the only new user-visible text). A BCM2722 restart while lit is a blank panel for 0.37–0.62 s with no text.
8. **Persistence:** developer marker files only (M-2 arms, `HAVE_COP_FRONTEND`'s switch, the failure strike), written while the drive is on.
9. **Scale:** 19-1 independent of the library (the whole-track lookahead is bounded by the buffer); 19-6 per cover; 19-8's proxied reads at 50k tracks (`tests/perf/lib50k.sh`).
10. **Deferred work:** art and database work in dark battery play waits for lit, a pause window or a charger (D-3); a slow job takes its CPU path for the boot; a failed BCM2722 restarts at once; slots come back lazily by key after any epoch change, never at wake; the codec core changes only at its re-select points (19-1c). Each state has a script and says what the user sees.
11. **Stock unchanged:** every edit under `HAVE_IPOD_UI` or a P19 `HAVE_*`; stock `.text` byte-identical with `english.lang` held equal.
12. **Existing paths doing the same job:** Fit-2's always-COP selection vs 19-1c; the per-tick COP pulse vs K-1; old poller timeouts vs 19-1a; the CPU drift vs 19-2; CPU slides and glides vs 19-4; F2 vs 19-5; per-output decode vs 19-1e and 19-6; the overlay slide vs `ANIM SLIDE`; `xfade_rect()`, `AVG2`, `ui_shadow_apply()` and `nav_core.c` vs `FADE`/`AVG`/`SHADE` (one definition, `jmix.h`); **every writer into the BCM2722's frame** (the LCD path, the video hand-back, the overlay, the job ring, the developer stages) through one port lock and the epoch rule. **Twin: every reader and every writer of the framebuffer over an ownable band** (§3.6's table) settles or clips; the `validate.sh` grep enforces it. **Also:** upstream's compile-time `MPA_SYNTH_ON_COP` vs 19-1c (iii)'s runtime switch (the switch wins under `HAVE_IPOD_UI`; stock keeps the define); the codec's `HZ/20` full-buffer poll vs D's watermark wake (D wins under `HAVE_IPOD_UI`). **Frozen-kernel takeover:** K-1 replaces the COP wake the tick provided; a COP thread with a timeout is "due", so the COP is not parked.
13. **Builder predicate:** 19-1e and 19-6 run exactly for `maint_wanted()`'s art items, now also gated by D-3.

---

## 8. Budgets

| Item | Target | Measured by |
|---|---|---|
| 19-1 | ≤ 3 KB `.text` | `arm-none-eabi-size` per object, `fw-ui` |
| 19-2 driver + 19-6's PP side (`bcmjob_decode()`, `bcmjob_read()`) + the PP build of `jref.c` + settle | **≈ 14 KB `.text`** (owner, 2026-10-24; was ≤ 9 KB for 19-2 and ≤ 2 KB for 19-6): the decode is the BCM path for every cover, so its driver half is worth its size. Measured (bench build, Thumb, after the review fixes): `bcmjob-core.o` 11,831 B (5,789 before 19-6), `bcmjob-ref.o` 4,320 B: 16,151 B, plus 264 B of device glue in `lcd-video.o`. Linker-dropped until the shell calls it. **Follow-up after P19:** a loadable art builder (the art cache's build path plus the decode glue as a module) takes it out of the resident image | same |
| 19-3 / 19-4 + 19-5 / 19-8 / 19-9 | ≤ 1 / 6 / 6 / 3 KB `.text` | same |
| Any single static | ≤ 1 KB (the executor's ~3 KB of state is an allocation) | statics check |
| New IRAM | 0 B | link map |
| **Runtime reservation (reviews C2, N1)** | **a cost, not a saving:** the measured net within ±10 % (or ±16 KB, whichever is larger) of §2A.1's figure (−220 KB video image, −28 KB display-only); **every floor held with the resident block counted** | floor gate (with the host model's reservation) and the device's `floor dev` line |
| Floors | ≥ 7,500,000 B (32 MB), ≥ 39,000,000 B (64 MB) at 50k tracks | `tests/perf/floor_gate.sh` |
| `_end` / `rockbox.ipod` | ≤ 1,500,000 B / ≤ ~1,250,000 B | size report |
| **Dark playback (gated)** | M-2 in BD's favour; 0 underruns; PLL off ≥ 90 % of dark time; **0 dark BCM2722 power-ups**; for BD on the light formats: wakes ≤ 5/s, COP wakes 0; drive ≤ 6/h and ≤ 1 % on | M-1, M-2 on `HAVE_DEVSTATS`; `lcd wakes dark=` (then `bcm power ups`), `m0 wakes`, `m0 cop wakes`, `stor pwr`, `clk win` |
| Dark playback (tracked) | < 25 mA | M-1 |
| Lit | 24 MHz default after 19-3; drift + marquee < 1 % of the CPU | B8; harness |
| Smoothness | first moving frame ≤ 33 ms (`anim first ms=`); glides 30 fps unboosted | harness, device log |

- **Agents:** hand back by ~250k tokens or after 2 failed fixes; gates only through `scripts/gate-run`; propose goldens, never write them.

## 9. Acceptance
- **Host:** `tests/copjob` for K-1; 19-1c's per-run core choice (no reload, no mid-track switch, `ci_cop.curpos` correct after a CPU-side `advance_buffer`/`seek_buffer`), D's watermark wake (no lost wake, messages still immediate), the `mpa.c` switch read at track start; `tests/bcmjob` for the device driver against `bcmjob.h`; contract 4's `jtest`/`jmtest` in `scripts/test.sh`.
- **Simulator scripts** (`ipodui-p19-*`): dark playback switching the codec core at its re-select points, with a gapless album across a switch; poller wake counts in the trace; drift, slide, glide and marquee with jobs and with each injected fault (clear, slow, fail, dead, cut, **ring full**), pixel-identical to the CPU goldens; **one truth script per §3.6 reader** with a fault mid-drift; a death mid-slide ending on the final frame; D-1's still drift and pop-up with `J_OPS` cleared and with SDRAM failing; dark playback logging 0 power-ups; a developer stage suspending and resuming the job client on a new epoch.
- **Gate check:** §3.6's `validate.sh` grep passes.
- **Trace rules:** every job-exercising script logs at least one `bcmjob` line (a rule that finds none fails) and no `bcmjob fault` unless it injected one.
- **Device:** M-1 before and after 19-1, M-2, M-3, then B1–B13 as each wave needs; a day of normal use on Wave 1's build with no `bcmjob fault` and no `pcm underrun`; Apple's firmware still reached with hold MENU.

## 10. Risks, rollback and safety rules

**Safety rules (binding):**
- **Every offload keeps a CPU path;** nothing is deleted from it in P19.
- **A BCM2722 failure degrades gracefully** (D-1): compute jobs fall back to the executor on the CPU; the drift turns still; Cover Flow shows its pop-up; a failed BCM2722 restarts through the boot ladder; a second failure turns jobs off until the next boot.
- **The video-failure error screen applies only to video.** The only other visible failure is D-1's pop-up.
- **No device writes without the owner's go,** and a fresh backup first. No `ipodpatcher`. Apple's firmware stays bootable.

**Risks:**
- **19-1 may not reach 25 mA** (the unknown base): tracked, not gated; M-1 before and after; no package claims mA it has not measured.
- **A core switch at a boundary breaks gapless** (19-1c): the switch is per `Q_CODEC_RUN` with no reload; a host test and a gapless script across a switch; the fallback is the developer row (placement fixed for the session), never a reload.
- **D loses a wake** (19-1c ii): a missed watermark wake would stall the codec and underrun; messages still wake the codec at once, and the old `HZ/20` timeout stays as a long safety timeout (e.g. 1 s) under `HAVE_IPOD_UI`; 0 underruns gates.
- **The latent `ci_cop.curpos` bug** (19-1c i) exists today on the switch-off path: fixed and host-tested here; noted in the ledger.
- **Lit browsing on a dark-started CPU decode** boosts more: reported in M-2 and B8; the re-select at the next boundary bounds it.
- **A parked COP missing a wake** (K-1): host test, device soak, one-`#define` fallback.
- **Hi-res FLAC at 24 MHz:** unmeasured; the `LOAD` hold covers it; 0 underruns gates.
- **Screen truth** (§3.6): a reader that skips the settle shows a stale drift or a wrong capture; the "every reader" twin and one script per reader.
- **COP coherency** (19-9): 19-8 first, the link check, single-writer words, seqlocks, generations, the buflib pin rule, M-5, `HAVE_COP_FRONTEND`.
- **Port lock order:** `bcm_port_lock()` everywhere; a review item for 19-2 and 19-9.
- **RAM:** L2 with the video image costs ~220 KB on every unit (§2A.1); floors hold; B13 and the display-only image bring it to ~28 KB.
- **The warm swap may fail** (B13): then the display-only image costs a cold 0.37–0.62 s at each video start, and Q2 stays on the video image.
- **A lit panel needs the BCM2722 powered:** the lit-idle saving depends on 5i.
- **A BCM2722 restart while lit** blanks the panel for 0.37–0.62 s.
- **Developer stages inside the merged map:** the client's suspend and new epoch (§3.1).
- **Port contention** (#44) and E2 at 24 MHz: B7, B2.
- **SDRAM bring-up** (#7): Cover Flow's availability includes it.
- **Simulator blind spots:** the simulator runs the executor through the host model; timing is modelled.
- **P12, P18, `p12-alarmfix` and `gate-speed` conflicts:** Wave 0 is cut from `c00496f6fa` and timed per §4; P18's owners review K-1 and the drive shape.

**Rollback:** every package behind a `HAVE_*` flag and a developer switch; a marker file at boot restores the previous default; `J_OPS` turns any BCM2722 job off without a rebuild.

---

## Revisions

### Revision 1c (2026-10-23): the second review (of rev 1b)

Base re-merged from `p12` `c00496f6fa` (854bc1b5c6: `p18-refill`, the 3 s dark hold).

| Finding | Resolution |
|---|---|
| N1 drift RAM vs how covers are stored | D-4 (`pv.slot`, `pv.slot2`, the 3-deep bank kept on every unit); the drift offload claims no RAM; §2A.1 a cost table (−220 / −28 KB); RAM stated as not a goal; Q14 added (default no); the summary and §2A.1 now agree; strip 180,000 B. The review's 64 MB figures could not be derived under D-4 and are flagged (§2A.1) |
| N2 the codec's HZ/20 poll | 19-1c (ii) "D": a watermark wake in `codec_thread.c` + `pcmbuf.c`, owner B; M-2's BD arm; the ≤ 5 wakes/s gate for BD only |
| N3 MP3 synthesis on the COP | 19-1c (iii): a runtime switch read by `mpa.c` at track start, ledgered, owner B; an MP3 album in M-2; the COP-0 gate's formats listed; checklist 12 |
| N4 re-select without a reload | 19-1c (i): the core per `Q_CODEC_RUN` in `codec_cop_run()`, the four changes, the latent `ci_cop.curpos` bug noted; no reload ever; "screen state" defined (M8) |
| N5 writers and missed readers | §3.6: the writer rule (clip or settle), the missed readers added, a `validate.sh` grep check (coordinated with `gate-speed`), the checklist twin extended to writers |
| M1 stale base | the 3 s hold in §1, 19-1b, Q-5 and the fallback; the estimate re-based (~31–36 mA start); M-1's "before" on the 3 s build; `artcache.c` pinned at `c00496f6fa` |
| M2 the power step | `min(10 s, sleep-timer end, idle-off deadline)`, the catch-up cap raised, `powermgmt.c` to A, after `p12-alarmfix` |
| M3 measurability | `m0 cop wakes`, the windowed `clk win` line, `lcd wakes dark=` for 19-1, `HAVE_DEVSTATS` for the bench |
| M4 contract 2 vs Q13 | Q13 decided (D-4) in §3.2; the BCM session amends its contracts |
| M5 the simulator's floor flag | `HAVE_BCM_JOBS_MODEL` |
| M6 owners and overlaps | owners for the `tagcache` wait, `pcmbuf.c`, `mpa.c`, `powermgmt.c`, `thread.c` and the queue, `material.c`; the `gate-speed` overlap listed |
| M7 settle | the CPU's time-based framing, `ANIM_STOP` only to confirm, no wait without overlap, timeout behaviour |
| M8 | in N4 |
| Lows | line refs fixed (`ui_main.c` :1187–1188, `menu.c` :668, `modload.c` :1572/:1645, `ui_main.c` :491, `material.c` :128); "nine owners"; C5 missing from the `bcm power state=` trace passed to the BCM session (W0-1) |
| Wave 0 timing | §4's table: start now / after rev 1c / rebase first / after `p12-alarmfix` and P12 |

### Revision 1b (2026-10-23): owner changes and the adversarial review of rev 1a

**Owner changes:**
- **Names:** the technical names throughout (PP5022, CPU, COP, BCM2722, WM8758), with a one-line glossary to the owner's conversation names (owner's correction, 2026-10-23).
- **Dark playback first:** placement steps 1–4, art decode-once and M-1/M-2 form package 19-1, ranked first. `p19-placement.md` (merged) is the placement reference, and its numbers are the baseline.
- **Q-7 decided** as D-3. The placement questions are merged in under stable IDs.
- **The BCM session's answers:** §3.3.
- **Contract 4 and the gap closures:** §3.1, §3.4, §3.7.
- **The BCM session's review items** (e608c4a27b): the shade op and the widened G5; B3 re-specified (the real shell drives the merged image; goldens identical on both backends); `bcmjob_resync()` (M3) and `bcmjob_suspend()` (M4); the memory contract restated against release `fw-ui` (C2; the 32 MB difference from Q13 is stated in §2A.1); the unlit-face window (M6).

**Review findings → resolution:**

| Finding | Resolution |
|---|---|
| C1 strip owners | Q12: the strip stays; no strip saving claimed; removed from RAM and acceptance |
| C2 resident image new to release | §2A.1 re-based on release `fw-ui`, image ≤ 217,088 B; floor counts it (host-model reservation, `floor dev`); G9 likely for 32 MB; Q2 restated; acceptance from the corrected math; RAM called secondary |
| H1 framebuffer truth | §3.6 screen-truth contract, `ui_fb_settle()`, one script per reader, checklist 12 twin; rev 1a's "unchanged first frame" withdrawn |
| H2 split shadow | `J_OP_SHADE`, `J_FADE_SHADE_B` (3d3f4d5db1); G5 covers `ui_shadow_apply()` and `nav_core.c`; B3 on the merged image's jobs with parity |
| H3 cover pixels under D-1 | Q13: one 180,000 B PP slot on 32 MB, the #92 bank on 64 MB; no staging buffer (E2 from the slot or bank); each slot user named |
| H4 Wave 0 not independent | base moved to `p12`'s tip (bad6e30f21); Wave 0 cut from it, coordination per file, `jpeg_load.c` pinned at 69f9aa4e9f, merge order stated |
| H5 codec stays loaded | 19-1c's re-select points, gapless rule, lit budget; B8 CPU-decode arm; M-2 lit boost; placement §4.3 corrected |
| H6 clock reading | reconciled from the raw `clk stats`/`clk lock` lines: 24 MHz, PLL off [silicon]; Q1's rationale restated; placement §4.1 quotes the same figure |
| M1 | cycle comparison tagged [hypothesis]; hi-res FLAC noted; 0 underruns the hard gate (PLL-off FLAC did run, H6) |
| M2 | E2 at 24/30 MHz in B2; 968 vs 775 per mille stated |
| M3 | stateful-op invalidation, mid-slide jump, ring-full injection |
| M4 | the job client suspends around developer stages and resumes on a new epoch |
| M5 | 25 mA tracked; gated acceptance listed |
| M6 | the unlit-face window; `ANIM_STOP` before the face's push; power-ups counted from the cut |
| M7 | single writer per word or a corelock (§3.5 item 6) |
| M8 | art decode block ≤ ~164 KB |
| M9 | Cover Flow's 512 KB out of every net; SDRAM failure also disables it |
| M10 | the bank's role decided by Q13 |
| L1 | the pop-up is the only new text; restart blank stated |
| L2 | boot sequence under L2 |
| L3 | `anim first ms= kind= src=` |

### Revision 1a (2026-10-23)
Contracts 1–3 pinned and checked; gaps listed; D-1 recorded; RAM from `memory-contract.md` §5 (superseded by 1b's re-base).

### Revision 1 (2026-10-23)
First draft from the BCM session's ranked plan, Fit-2, 18-5's lit-clock design, the module-loader decision, P12's classes and SDK, and the owner's architecture (#72).
