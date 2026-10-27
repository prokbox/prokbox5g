# The battery levers: idle sleep, level 5, the 24 MHz lit clock, the video clock (p19-battery, PW4)

**Names:** PP5022 (CPU, COP), BCM2722 ("the BCM"); in chat PP, PP1, PP2, VC.
**Tags:** [silicon] measured on our unit, [sim] simulator, [ours] our design, [hypothesis], [estimate].
**Companions:** `power-contract.md` §7 (the lit-current model, `scripts/bcm_power_model.py`), `../../bcm2722-reference/brief-map-power.md` §4 and PW3, `../../design/lit-24mhz.md`, `bench-p19.md` "The battery rows".

The model says the BCM costs about **4.2 mA** whenever the screen is lit, almost all of it our image busy-polling at 27 MHz on regulator level 11 [estimate]. Three levers, in the owner's order, and a fourth for video (section 4). **No current here is measured, and none will be for a long time** (owner, 2026-10-25: no meter-based tests). Decisions rest on the simulation's energy proxy, on the proxies the device logs (time-weighted MHz and level, idle share, misses), and on the long-run battery record from regular use (section 4, "The long-run record").

## Savings (the model's central values, mA at the battery, lit) [estimate]

Split from `bcm_power_model.py`'s parameters (the A case: jobs on, 4.23 mA of BCM):

| Lever | Alone | Notes |
|---|---|---|
| 1. Idle sleep (5i) | **−2.60** (BCM 4.23 → 1.63) | `sleep_factor` 0.25 (0.10–0.40) is the unknown |
| 2. Level 5 (PW3), polling | −1.73 (→ 2.50) | voltage² from 1.04 V to 0.80 V [hypothesis: levels span 0.8–1.2 V evenly] |
| 2 after 1 | **−0.66** (1.63 → 0.97) | the model's A+s row: −3.26 for both |
| 3. Lit clock 24 MHz | **−0.70** averaged (the model applies `lit24_ma` 2.0 to the drift's 35 % share of lit time); up to **−2 (1–3)** on any lit screen that is not drawing a CPU stream | the model's A+24 assumes 18-5's policy was already in for the other screens; it was not, so lever 3 is worth up to the whole `lit24_ma` |

`k_bcm` moves everything: at 0.3 mW/MHz the BCM's A is 2.6 mA and A+s 0.7; at 1.0, 7.2 and 1.45.

## 1. Idle sleep (stage 5i)

**What exists** [ours, built before p19-battery]: every video image (plain and merged) is built with `BCMFW_IDLE_SLEEP`, `BCMFW_IDLE_GATED` and `BCMFW_IDLE_DOORBELL` (`bcmfw/mkvideo.py`). The main loop's `idle_wait()` sleeps (`ei; sleep; di`) between command polls only while the host has written `"SLP1"` to `0x16C`; the 1 ms timer compare 1 and the doorbell (source 23) wake it; `jobs_tick()` runs every pass, so BCM-timed animations step at 1 ms granularity. The boot path writes the gate at the swap and at every wake when `bcmres.cfg` says `idle on` (Developer › "Video chip idle sleep", default off); the player clears it for a session and sets it at the hand-back.

**What silicon has shown:** nothing yet. Stage 5i has never run; no interrupt source is confirmed on the chip (`open-questions.md` #3) [hypothesis that it works: Apple's RTOS idles the same way, [apple]].

**Merged image:** no change needed: the gate is already in `cd8e5d96`.

**PP side (done, p19-battery):** `bcm_boot_res_live()` turns the gate on and off live (RAM only; a wake keeps the choice), and `bcm_boot_res_read()` reads the idle counters, both under the LCD port lock. The bench row `slp` uses them. The default stays **off** until `slp` passes.

## 2. Level 5 (PW3)

**What silicon has shown:** a host-started image runs at `0xBB`, level 11 [silicon S7; PW1 context B, 2026-10-25]; Apple's running `vmcs` ran this unit's chip at `0xB5`, level 5, with the core at 27 MHz [silicon S1; PW1 context A]. Whether **our** image holds at 5 is PW3's question; PW3 has not run.

**Merged image:** no change needed. The PP writes the regulator through the port (data path 4, `docs/data-paths.md`); the image's `clock_fast()` reads the level it finds, raises to 15 before PLL A, and `clock_slow()` writes back what it found, so a level set by the host survives a video session [ours, `bcmfw/clock.c`].

**PP side (done):** `bcmres.cfg` line `level 5` (Developer › "Video chip core level 5", default **off**). At the swap and at every wake (a cut resets the chip to 11) `bcmfw_res_level()` checks the core clock word `0x1000_1C28` reads the crystal (`[10:8]` = 0), then writes `0xA5A5_0000 | (old & 0xF0) | 5` to `0x1000_1400` and reads it back. Never below 5, never bits `[7:4]`, never off the crystal. No 10 ms wait: the clock does not change, so nothing depends on the settle [ours; Apple waits 10 ms around a clock change, [apple]]. The read-back is logged at the boot (`boot path: resident core level N`).

**Risk:** a core that misbehaves at level 5 would show as job failures or a hung display; the run-time fallback (cut, the next rung) clears it, and the setting stays off until the bench row passes.

## 3. The 24 MHz lit clock

**What existed:** `firmware/pll_idle.c` held the PLL (30 MHz) whenever the backlight was on (`CPU_PLL_HOLD_LIT`); `docs/design/lit-24mhz.md` designed the shell policy but nothing was built [code].

**PP side (done):** 18-5's design, built:
- `pll_idle_set_lit()`: the shell's lit need; true until told (today's behaviour).
- `clockpol.c`: need = switch at 30 (Developer › "Lit screen: 30 / 24 MHz", **30 by default, not saved**) or a CPU drift stream (`ui_panel_drift_cpu()`: a drift step the CPU drew within 1 s; not when the BCM's jobs draw it) or a lit strike while playing. Taken at once, released after 2 s (`cp_lit_step`). A light format that misses a lit minute at 24 MHz sets `lit_strike` for the session instead of striking the format for dark playback. Host rows in `tests/clockpol`.
- Animations still boost to 80 MHz; nothing about `cpu_boost()` changes, so slides and glides look the same.

**Merged image:** none.

**Why the default stays 30:** the saving is 1–3 mA, below what `power.csv`'s voltage resolves in one sitting (`lit-24mhz.md` §5.3). The rule is the proxies' and the long-run record's (below): the row's B phases at 24 MHz with no hold and no strike, then a week of daily use with the switch at 24 whose battery lines show no loss against the weeks before. With jobs on, the drift is the BCM's, so once that holds the main menu idles at 24 MHz too.

## The bench rows

Part B of the bench session, after `jvpu2` (the merged image is the resident display driver there). `bench-p19.md` "The battery rows" has the owner's side. Each row: A B A B, 60 s each, power.csv marks `<row> A1`, `B1`, `A2`, `B2` and `<row>` at the end (the log runs at 5 s rows for the row and is put back), a still screen. The rows as built still end on a meter question (`<row> meter: …`); with no meter, "No meter" is the answer, and the verdict is the automatic one below.

| Row | A | B | Automatic verdict | Answers |
|---|---|---|---|---|
| `slp` | gate off (polling) | gate on | every B phase: ticks within 10 % of its milliseconds, sleeps > 0, bad 0, the image still answering. **This answers #3** for the tick, and the bells count says whether the doorbell fires | B lower by 2 mA+ / under 2 / no difference / higher |
| `lvl5` | the level found (11) | level 5 (an owner go first; the gate off for both) | the level reads back in every phase; the job self-test passes at the end of each B | B lower by 1 mA+ / under 1 / no difference / higher |
| `lit24` | switch 30 | switch 24 | B reads 24 MHz, A 30 or more; holds logged (a USB data link or a heavy format keeps 30: a setup fail, not the policy) | B lower / no difference / higher |

`slp` and `lvl5` also ask "Did the screen stay right?" (Yes / Garbled / Froze).

**What turns a default on** (the owner's call after the rows; proxies plus the long-run battery log, no meter): `slp` PASS (every B phase woken on the tick, the image answering, nothing disturbed) → "Video chip idle sleep" default on for a trial week; `lvl5` PASS (the level reads back, the job self-test passes at 5) → "level 5" default on for a trial week (with idle sleep, the `A+s` row); `lit24` PASS (24 MHz held, no strike) → the switch's default to 24 for a trial week. Each trial keeps its default only if the week's `batt session` lines (section 4) show no shorter battery life per lit hour than the weeks before, and no user-visible fault in the log.

## 4. The video chip's clock: level bands and a governor (PW4)

**What there was:** two clocks, the crystal (27 MHz) or PLL A at 150 MHz on level 15, and a single gate (stages 4b and 4c). On silicon the 27 MHz session missed (FEED mean 42.7 ms against 33.3, sitting 1) and 150 MHz played clean (sessions 27–38). Apple's `vmcs` instead runs a load governor over 5–150 MHz with a matching supply level (`../../bcm2722-reference/brief-map-power.md` §2, §4, §5) [apple].

### The cost model [silicon]

`scripts/vclk_fit.py`. The FEED times at 27 and 150 MHz give the same cycles a frame (clip 1: 1.15 M at 27, 1.13 M at 150; clip 2: 2.58 against 2.75 M, its 27 MHz run skipped 203 pictures, a different mix): **the decode scales with the core clock** [silicon]. Mean cycles a frame ≈ **8.95 × pixels + 94.1 × bits a frame** (least squares over clip 1, clip 2 and stage 7's file at 150 MHz; residuals +7.5, −16.1, +16.1 %). It is kept only as a sanity floor (`vclk_model_cycles`): never settle a stream below the clock its mean needs.

| Clip [silicon, V6 lines] | fps, kbit/s | at 150 MHz: mean, p95, p99, max (ms) | Mcycles: mean, p95, p99 | Lowest band its floor reaches |
|---|---|---|---|---|
| clip 1 (`stage6-clip.mp4`) | 30, 169 | 7.6, 8.4, 12.6, 31.2 (the first frame's set-up) | 1.13, 1.26, 1.90 (its I frames) | 74 MHz (level 7): the I frames hold it above 59 |
| clip 2 (`stage6-real.mp4`) | 24, 414 | 18.4, 27.2, 28.8, 39.8 | 2.75, 4.09, 4.32 | 116–140 MHz |
| stage 7 (`stage7.m4v`, prokbox-convert) | 30?, 600 | 14.8, 22.5, 31.9, 35.6 | 2.21, 3.37, 4.79 | 150 MHz (its p99 is already near the frame) |

### The bands [ours on Apple's values]

The chip runs one of twelve **level bands**: the crystal (27 MHz, at the level found there: 11 after a start, 5 with lever 2), then the top whole MHz of each of Apple's level bands at that level: **46 (5), 59 (6), 74 (7), 87 (8), 102 (9), 116 (10), 123 (11), 130 (12), 140 (13), 147 (14), 150 (15)**. Within a band the voltage is the same, so its top clock costs the least energy per unit of work; a band is one regulator write and one PLL lock away from the next. PLL A by Apple's rule (M 108, N = VCO / 250 kHz, the VCO ≥ 75 MHz: 46, 59 and 74 run their VCO at twice the clock and divide by 2); the refresh f / 128,000 [apple]. Only **verified** bands are used: a band-verify row (`vbv`) writes `bcmpassvc.txt` (one line a band, naming the player's image by size and FNV); 150 MHz stays stages 4b and 4c's flags; until a band is verified, the nearest verified one above it serves.

### Apple's governor against ours, in simulation [sim, estimate]

`scripts/vclk_gov_sim.py`: every 10,000 µs the busy share, each change costing what our `clock_band` does (1 ms of lock on the crystal, 10 ms of regulator settle busy-waiting per level change, before a rise and after a fall) [apple @5134]. Workloads: the two stage 6 clips' FEED traces from silicon (`tests/vclk/traces/`, played 4 times, a FEED out 35 ms before its picture and late past half a frame), a minute of the lit shell (the drift's steps, the marquee, a retained slide every 15 s, one album-art `DECODE` with its 150 ms cap; the cost audit's cycles [sim]), and the same minute with 10 s of load no client declares (a game's 30 MHz). The energy proxy is the power model's per-level current (`bcm_power_model.py --vclk`), idle asleep (`sleep_factor` 0.25):

| Governor | clip 1: MHz, level, mA, misses, changes/min | clip 2 | shell (crystal at level 5) | undeclared load |
|---|---|---|---|---|
| fixed 150 (today's video) | 150, 15, 11.8, 0, 0 | 150, 15, 16.0, 0, 0 | — | — |
| fixed 27 | 27, 5, 2.5, **764**, 0 | 27, 5, 2.5, **505**, 0 | 27, 5, 0.99, **2**, 0 | 27, 5, 1.28, **375**, 0 |
| Apple's (+6 % busy at ≥ 0.9, −6 % idle, 1 MHz, 5–150) | 45, 5.3, 4.0, **495**, 6,002 | 45, 5.3, 4.0, **330**, 6,000 | 6.3, 5.0, 0.63, **10**, 2,334 | 18, 5.1, 1.7, **464**, 3,509 |
| Apple's + client floors | 143, 13.6, 11.0, 6, 188 | 150, 15.0, 16.1, 12, 47 | 6.5, 5.0, 0.67, 7, 2,315 | 18.6, 5.1, 1.7, 461, 3,490 |
| 1: proportional (f × util / 0.8) | 19, 5.1, 1.9, **365**, 5,379 | 19, 5.1, 1.9, **240**, 5,408 | 5.9, 5.0, 0.59, 16, 1,637 | 11.7, 5.0, 1.2, 529, 3,293 |
| 1 + 2: fast up (≥ 0.95: the top), down a band a window | 125, 12.1, 18.8, 120, 5,994 | 125, 12.1, 18.9, 90, 5,996 | 59, 7.2, 6.1, 55, 5,984 | 114, 11.3, 16.8, 238, 5,998 |
| 1–3: snapped to the level bands | 94, 9.2, 12.8, 41, 5,481 | 94, 9.2, 12.8, 37, 5,503 | 61, 7.1, 7.0, 305, 2,785 | 86, 8.7, 11.4, 107, 4,847 |
| 1–4: 100 ms at a band before going down | 134, 12.4, 11.0, 0, 632 | 148, 14.5, 18.6, 0, 922 | 43, 6.0, 1.9, 0, 137 | 63, 7.6, 4.2, 0, 297 |
| 1–4 + the busy share's EWMA (1/4) | 56, 5.7, 4.0, 9, 444 | 113, 10.4, 12.3, 61, 775 | 29, 5.1, 1.08, 1, 21 | 32, 5.1, 1.44, 3, 29 |
| 5: the clients' floors only (else the crystal) | **88, 8.5, 5.9, 0, 1.8** | **123, 10.9, 11.1, 0, 3.4** | **27, 5.0, 1.01, 0, 3** | 27, 5.0, 1.30, **373**, 3 |
| this branch's first design: a client's floor when declared, else 1–4 + EWMA | 88, 8.5, 5.9, 0, 1.8 | 123, 10.9, 11.1, 0, 3.4 | 30, 5.2, 1.15, 0, 23 | 33, 5.2, 1.51, 3, 31 |
| **ours, two layers (read every 100 ms): band selector + top-of-band** (the default) | **89, 8.6, 5.96, 0, 14.7** | **123, 10.9, 11.14, 0, 5.7** | **30, 5.2, 1.16, 0, 23** | **33, 5.2, 1.51, 3, 25** |
| ours, two layers: band selector + proportional in the band (the A/B) | 78, 8.6, 5.69, 0, 14.7 | 125, 11.6, 11.75, **1**, 13.7 | 30, 5.2, 1.14, 0, 23 | 32, 5.2, 1.49, 3, 32 |

Polling between frames (a session today, `--poll`), the two in-band policies: clip 1 top **11.24 mA**, proportional **10.17 mA** (−10 %), clip 2 top **16.98**, proportional **18.03 mA** (+6 %, and its one miss); the shell and undeclared load within 1 % of each other.

**What it says:**
- **Apple's governor alone does not hold our video** (hundreds of misses): a frame's decode is a burst inside a 33 ms frame, so a 10 ms window sees full load, then none, and a 6 % step chases it. Apple's own decoder presumably registered a client floor [hypothesis].
- **The improvements, one at a time:** proportional alone is worse (it drops straight to the floor of 5 MHz between bursts); fast-up fixes the misses but flaps at 6,000 changes a minute, each a 1–11 ms stall; the level bands cut the misses again; **the 100 ms dwell is what makes it work** (0 misses, transitions /9); the EWMA halves the energy again and the transitions /10. Each was kept because it lowers the proxy or the misses against the step before (owner's rule).
- **The clients' floors carry it.** The video session's floor (it starts at the top; after a steady 3 s it drops straight to the lowest verified band whose scaled FEED p95 keeps 25 % of the frame and whose scaled longest FEED keeps 10 %; a miss puts it a band above, and that band then waits 6, 12, 24 s; a seek, a resume or a wake puts it back at the top) and a `DECODE` burst's full speed for its run give **0 misses with 2–3 changes a minute, and the lowest energy on every declared workload**. The load governor adds nothing where a client declared its load and costs 0.1–0.4 mA in the shell (it leaves the crystal for its bursts). It is kept as **the safety net for undeclared load** (373 → 3 misses).
- **So ours, in two layers** (the owner's structure, 2026-10-25):
  1. **The band selector** picks the supply level: a verified band of Apple's 13-row table. **Up** one band when the work keeps saturating the band's top clock (the load, f × the busy share, at 95 % of the top or more; the load rises at once and falls by an EWMA of 1/4 a reading); **down** one band when the band below's top would carry the load at 80 % or less, after **100 ms** at the band (95 against 80 is the hysteresis). **A client's floor forces a minimum band** at once: the video session's (from its FEED times and misses), a `DECODE` burst's top band for its run. The chip changes the level before a rise and after a fall, with Apple's 10 ms wait.
  2. **The in-band policy** picks the clock inside the band (no level change: only PLL A's 1 ms lock). Two, a switch: **top-of-band** (the band's top clock, idle the rest: race to idle within the band; the **default**, the voltage-aware choice) and **proportional** (f = load / 0.8 inside the band, above the band below's top; up at once, down only by 10 % or more).
- **Top against proportional** [sim, estimate]: with the core asleep between frames they are within 5 % (the energy within a band is nearly all the voltage's, which is the point of the bands); with the core polling (today's session) proportional saves 10 % on the light clip but costs 6 % and a miss on real footage (it runs under the floor band's top, where the floor was measured). So **top-of-band is the default**; proportional is the A/B the device's proxies decide (the bench row `vgov` plays every clip under both).
- The window N = **3 s** for the video floor: one GOP of prokbox-convert's output (so the window holds an I frame, the worst frame), 90 samples at 30 fps for the p95, and a switch's 11 ms is under 0.4 % of it.
- **The PP's lit-clock governor** (`clockpol.c`, the 24 MHz lit clock) will follow the same two-layer shape later: a band selector over the PP's own voltage steps, an in-band policy for the clock.
- **Expected saving against today's 150 MHz video** [sim, estimate]: clip 1 **−5.9 mA asleep / −16.1 mA polling** (−50 / −59 %), clip 2 **−4.8 / −10.1 mA** (−30 / −37 %); the shell unchanged (it stays on the crystal; `DECODE` bursts at the top). Polling is today's session (the idle gate is cleared for a session); the asleep column is what idle sleep in a session would add. Against the BCM's share of lit current (~4 mA in the shell) these are large: during video the BCM at 150 MHz polling is the model's **27 mA**, so the floor's 11–17 mA is the biggest single BCM lever on video [estimate, `k_bcm` 0.55 mW/MHz; at 0.3 or 1.0 everything scales with it].
- **A cheaper idle point** [estimate]: polling, 46 MHz at level 5 (3.95 mA) is below the crystal at level 11 (4.18 mA), and the crystal at level 5 (lever 2, 2.47 mA) is below both. Lever 2 first; 46 MHz as the shell's floor only if PW1 shows PLL A's own draw is small (unknown) [hypothesis].

### Where it runs: the PP, by size [ours]

Apple's governor runs on the chip. Ours could not: in the merged image a governor on the chip (its 10 ms tick, the EWMA, the floors' protocol) measured **+1,152 B** of image+bss against **484 B** of room (100 B below the arena plus the arena's 384 B of slack), with the vc2cc compiler's code density. So **the chip switches bands and meters; the PP decides**:
- **Image** (`bcmfw/clock.c` `clock_band`, `HP_OP_VCLK`, `HP_CAP_VCLK`; the main loop's busy meter): a band in Apple's order (a higher level and 10 ms before the clock rises; the core on the crystal while PLL A locks, 1 ms; the core word `0x100 | div`; a lower level and 10 ms after it falls), then **the core's speed checked** against its crystal speed (a 20,000-turn loop must take 27/f of its crystal time, ±12.5 %); any stalled wait or wrong speed puts the core on the crystal, PLL A off, at the level found there. SDRAM's refresh goes to 27 MHz's value (the most frequent, safe at every clock) before a change and to the new clock's after; the panel gets Apple's 150 MHz bus words on PLL A (longer at a lower clock: safe) [ours]. The busy meter: the decoder's packets' time (the main loop times each `hp_work` that ran a packet; an idle pass reads no counter). A band takes an optional clock inside it (whole MHz above the band below's top; the VCO at 2 or 3 times the clock below 75 MHz), for the in-band policy; inside a band no regulator write. The reply carries the band and its clock, the meter (busy µs, the counter), regulator writes, PLL locks and fallbacks. **Size:** the merged image **219,012 B (+352 B of load image)**; the arena moves **`0x35400` → `0x35580`** (617,088 B, 32 B over the 617,056 a 320×240 stream with 4 references needs) and the UI pools with it (DRIFT `0x35580`, STRIP `0x8E780`, THUMB `0xAE780`, APP `0xBE780` to `0xCB580`). The plain image 221,156 B (+256). The staged candidate 219,592 B fits too. The bench and clock-command images (4b, 4c) are byte for byte unchanged (`mkdevtest.py --check`), so their pass flags stand.
- **PP** (`apps/video/vclk.c`, `vpcore.c`): the two layers (`select_band`, `select_mhz`), the video floor, the burst request, the gate, the log lines; HP_OP_VCLK's reading 10 times a second in a session. The in-band policy is `vplayer_vclk_policy()` (the bench row's parameter; top by default) and `scripts/vclk_gov_sim.py`'s `2-layer: top` / `2-layer: prop`.

**What the earlier designs left, removed:** the single-layer PP governor (a declared floor *or* the load governor), refactored into the two layers above; the race-then-settle stepping (operating points 54/81/108/150, the steady-window step down one point, `vclk_frame`/`vclk_event`/`vclk_idr`), its tests, the race-to-idle-as-design plan, and the chip-side governor (`clock_gov_pass`, its mailbox state): replaced by the bands, the PP's floor and governor above. The race-to-idle *row* went with them (no meter). The cost model and the trace fit stayed.

### The logs

- `vclk band from=<MHz> to=<MHz> lvl=<level> why=floor|burst|up|down|fail util=<permille> floor=<MHz>`: every band move the selector asks for (the bands named by their top clocks), and every fallback. In-band clock changes are counted, not logged.
- `vclk sum policy=top|prop mhz=<> lvl=<x.y> idle=<permille> bands=<> clocks=<> lvl_changes=<> fails=<> misses=<> s=<>` and `vclk time <MHz>=<ms> … ms`: at a play's end and when the screen goes dark: the in-band policy, the time-weighted clock and level, the chip's idle share (its meter), the band moves and clock changes, the chip's misses (skips and dropped GOPs), the time at each band.
- `scripts/bench_ingest.py`'s "video clock" section lists them, counts the band moves by why, gives each play's means with its policy, and flags a fallback or a play with misses.

### The long-run record

`power.csv` (developer builds, the log on) records per row the time, battery mV and %, charger and charging, playing or paused, the DAC standby, the volume, the shares of the interval boosted, backlit, LCD awake, disk active and storage on, and Fit-1's CPU idle and busiest threads [code: `powerlog.c` `sample`]. It has no BCM column and is off by default. **Added: one `ipodui.log` line a session in every build** (`apps/ipodui/battlog.c`): at power-off, before sleep and at most hourly:

`batt session start=<%>/<mV> end=<%>/<mV> min lit=<> dark=<> play=<> vc_mhz=<> vc_lvl=<x.y> vc_idle=<permille or -> at=off|sleep|hour`

The chip's figures cover the lit time (it is cut dark): the governed plays' time-weighted MHz and level, the crystal at its level for the rest; its idle share from the governed plays' meter.

### The decision rule (no meter)

A design or a default is kept when **(1) in simulation** (`vclk_gov_sim.py`) it lowers the energy proxy with no more misses than the one it replaces, **(2) on the device** the bench rows pass (bands verify; under the governor no skip, no dropped GOP, A/V within stage 6's 50 ms, no more hitches than fixed 150, ≤ 20 changes a minute, the top again after a seek and a resume) and the logged proxies agree (no misses, a lower mean MHz and level than fixed 150), and **(3) over regular use** the `batt session` lines show no shorter battery life per lit or playing hour than before it. Power is never a bench pass condition.

**Until then:** the governor acts only on verified bands; with only 4b and 4c's 150 MHz verified it has nothing between, and a session plays at 150 MHz as today (`be_vclk` gives no mask). The image change waits for the next merged-image batch.

## Not taken (candidates)

- **Image:** an adaptive tick (10 ms when no BCM-timed animation is pending, once the doorbell has been seen this boot) would cut the 1,000 wakes a second; and `J_POWER` could report `J_PWR_IDLE_SLEEP` while the gate is set. Neither is needed by a lever above, and either would need the merged image's re-verify, so neither is built.
- **Block clocks:** PW1 found our image leaves the block clocks `+2C`–`+48` running on the crystal where Apple's `vmcs` has them off [silicon PW1]. Turning unused ones off is a further lit lever, for a later image change after the census says which feed what.
