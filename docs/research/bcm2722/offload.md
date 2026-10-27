# 2D offload: the measurement plan (one real job, PP vs BCM)

Issue #60. **Status (2026-10-11): plan, plus one prototype kernel bit-exact in vc2sim.** No shell code changes. `approach.md` sets the rule: *no rendering changes before the power question is answered* (does waking the BCM for a job cost more than the PP5022 doing it?). This document is how we answer that for one job, with numbers the device produces.

## 1. The job: the main-menu panel drift

**Picked:** the cover drift on the right half of the main menu and the Music split screen (`apps/ipodui/preview.c`, `nav_core.h`; `artcache.md` §7).
- Each cover dwells 7 s (`NAV_DRIFT_CYCLE`).
- A cycle starts with a **cross-fade** from the old framing: 8 steps in 400 ms (`NAV_DRIFT_FADE_STEPS`, `DF_STEP` = 50 ms), each one `xfade_rect()` over the 160×240 panel, then a push.
- It then **pans** 1 px every 150 ms (`NAV_DRIFT_STEP`): up to 44 pans a cycle (`NAV_DRIFT_PAN_MAX`). Each pan is `blit_window()` (`lcd_bitmap_part` of a 160×240 window out of the 300×300 `ARTCACHE_PANEL` slot), then a push.
- The panel-arrive fade is the same `xfade_rect()`: 6 steps in 200 ms (`NAV_PANEL_XFADE_STEPS`), boosted (`ui_anim_frame()`).

**Why this job:**
- **It runs at draw time, continuously,** whenever the main menu is lit: about 52 panel frames every 7 s, about 7.4 a second, for as long as the owner looks at the menu. A saving here is a saving on the screen people see most.
- **Every frame already crosses the host bus** (76,800 B per push). If the BCM holds the cover, a pan needs no pixels on the bus at all. That is the "draw commands instead of pixels" case `approach.md` describes.
- **It has an exact definition.** The fade's arithmetic is integer and fixed (`xfade_rect()`, per field `(d·(32−w) + s·w + 16) >> 5`), so the BCM path can be pixel-exact with the PP renderer (the two-backend rule in `approach.md`). The pans are copies.

**Considered, not picked first:** album-art downscale (`artcache.md` §8, `apps/recorder/resize.c`) and the blurred-cover fill (`apps/ipodui/artblur.c`). Both run in the art builder, once per album, only while the user is idle (#58). Offloading them would shorten a background job, not change what the user sees or a cost paid every second. They are the second candidate, after this one has a verdict, and JPEG decode (not the resize) dominates there.

## 2. Cost today: the PP path

Per 160×240 panel frame. Cycle counts are **static estimates** (ARM7TDMI timings, cache hits), from `xfade_rect()` compiled with the firmware's own flags (`arm-none-eabi-gcc -Os -mcpu=arm7tdmi`). The push uses `lcd-fps-ipodvideo.md`'s model (4–8 PP cycles per pixel); neither is measured on the device yet (§6 measures both).

| Work | Per pixel | Per frame (38,400 px) | ms at 30 MHz | ms at 80 MHz (boosted) |
|---|---|---|---|---|
| `xfade_rect()`, pixel changed | 31 cycles: 2 `ldrh`, 2 `mla`, 6 ALU, `strh`, loop | 1.19 M | 40 | 15 |
| `xfade_rect()`, pixel equal (skipped) | 19 cycles | 0.73 M | 24 | 9 |
| `blit_window()` (a pan: copy the window) | ~2–4 cycles (`lcd_bitmap_part` row copies) | 0.08–0.15 M | 3–5 | 1–2 |
| Push PP → BCM (`lcd_update_rect`, 160×240) | 4–8 cycles (model) | 0.15–0.31 M | 5–10 | 2–4 |
| BCM → LCD | | | Apple vmcs: ~7.6 ms (198 ns/px, `hardware.md`); our image at the 40 MHz words: ~8.6 ms (223 ns/px, session 6) | |

**Per 7 s cycle (8 fade frames + 44 pans):** about 11–12 Mcycles of fades plus 10–20 Mcycles of pans and pushes: **roughly 21–32 Mcycles, 3–4.5 Mcycles a second, 10–15% of the unboosted 30 MHz PP**, the whole time the menu is lit. Each cycle also reads one 180 KB panel from storage. That read is the same in both paths, so it isn't counted here.

## 3. Cost offloaded: the BCM path

The PP uploads each cover once. After that it sends one small job per frame: "fade step k", or "show the window at (x, y)".

**Fade step: the prototype kernel** (`bcmfw/video/gen_xfade.py` → `xfade_kernels.S`, driver `xfade.c`, test `bcmfw/tests/xfade_test.py`).
- **Format.** The BCM keeps the panel as three 16-bit planes (R5, G6, B5 per lane): the state (what the panel shows now) and the incoming picture.
- **Per field,** exactly as `xfade_rect()` computes it: `vmul` s·w, `vmul` d·(32−w), `vadd`, then `vmulhr #2048`, which is `(acc + 16) >> 5` for acc ≥ 0. The new fields go back to the state planes and, packed with two `vmul`s and two `vadd`s, to RGB565.
- **Bit-exact** against a port of `xfade_rect()` in vc2sim (silicon profile, the default since session 5). It passes 42 of 42 steps: whole 160×240 panels, cover-like, random and extreme pictures, both fade lengths (8 and 6 steps), every weight `nav_xfade_weight()` produces.
- **Cost:** 38 instructions per 16×16 block (28 of them REP 16 vector ops), ~453 cycles using the device's own REP timings (bench B4K: ALU 16.25, `vld16` 16.63, `vst16` 16.50 cycles per REP-16 op; scalar ~1). A 160×240 step is **7,363 instructions, ~70 k cycles: 0.89 ms at ~78 MHz** (the core clock the counter shows), **0.46 ms at 150 MHz**. The PP's 15–40 ms becomes under 1 ms.

**Pan: no compute.** It's a 2-D DMA of the 160×240 window from the BCM's copy of the 300×300 cover (RGB565, 600 B pitch) straight to the LCD. That's the same engine and register sequence as the device-verified `UPDATERECT` (session 4 onward), with a different source address and pitch: ~8.6 ms at the 40 MHz words, LCD-bus bound, with the BCM core idle meanwhile.

**Upload, once per cover.** 300×300 RGB565 is 180,000 B: 45,000 data-port words, 90,000 host-port transfers. At 4–8 PP cycles each, that's **0.36–0.72 Mcycles: 12–24 ms of PP time at 30 MHz, once per 7 s.** The BCM then makes the fade planes for the fade window (the incoming picture at its first framing; the outgoing is the state already held).
- With the vector unit this needs `vlsr`/`vand`, which aren't device-checked yet (LSR8 rides in the session-7 bench; `vand` has no check). It's ~0.1 ms once they're checked.
- As scalar C until then: ~40 k pixels × ~10 instructions, ~5 ms once per cover.

**PP per frame, offloaded:** one job record and a doorbell, ~26–40 host-port transfers (`hostproto.md` §8: a `PING` round trip is 26), **~200 cycles**.

**Per 7 s cycle, offloaded:** PP ~0.4–0.75 Mcycles (the upload dominates), **about 0.1 Mcycles a second, 0.3% of 30 MHz**. The BCM does 8 × 0.89 ms of kernel plus about 5 ms of unpack. Its LCD-bus time is unchanged (52 pushes either way).

**BCM memory:** two 300×300 covers (360 KB) plus state and source planes for the 160×240 window (2 × 230 KB): about 820 KB of the BCM's 4 MB SDRAM (`hardware.md`). The menu and a playing video never share the display, so the video pipeline's buffers can take the same space.

## 4. Wall time per frame

| Frame | PP path (today) | BCM path |
|---|---|---|
| Fade step | 15–40 ms PP compute + 5–10 ms push + ~8 ms LCD | ~0.9 ms BCM kernel + ~8.6 ms LCD |
| Pan | 3–5 ms copy + 5–10 ms push + ~8 ms LCD | ~8.6 ms LCD |
| Cover change | storage read only | storage read + 12–24 ms upload (overlaps the previous cover's pans) |

The fade frames are due every 50 ms, and the PP path fits that only boosted. The BCM path frees the PP for list scrolling and input during the drift.

## 5. Forms (device evidence)

The prototype uses only forms the device has checked, with one exception:
- **Checked:** `vld16` into any row (every bench case's input), `vmul D16 A16 Breg rep` (MULR), `vadd D16 A16 B16` and `#imm rep` (ALUX, ADIR), `vmulhr D16 A16 #imm` on non-negative A (MHR4), and `vst16` from block-0 rows under the silicon store layout (COPY, COP8). Session 6's tie-rounding finding (MHRN, a −0.5 rounds to −1) doesn't reach this kernel: every accumulator is non-negative, and positive ties round up under either rule.
- **Not yet value-checked:** the REP 16 form of `vst16` (stepping rows 0–15 from d00). It's also the display path's store; `vld16` with REP and stride is the same encoding family as LD8R's checked `vld8` REP. **Add a bench check for `vst16` REP 16 (rows 0–15, stride) to the next session's bench.** Until it passes, the kernel can store one row at a time (16 single stores per plane, +~50% kernel time, ~1.3 ms per step).

## 6. Battery method

**What exists:**
- **Nothing measures current on this iPod.** The PCF50605 path in Rockbox reads battery voltage only. `CURRENT_*` in `ipodvideo.h` are estimates, and `CURRENT_BACKLIGHT` is marked FIXME.
- **The developer power log** (merged into p15-dac at f26d48666f). It writes `/.rockbox/power.csv` every 5, 30 or 60 s (set in the Developer list), with columns `s,mv,pct,charger,charging,play,boost_pct,bl_pct,lcd_pct,disk_pct,dac_allow,dac_sb,dac_sleeps,dac_wakes,vol_db,dropped,mark`. `powerlog_mark(const char *tag)` (`#include "ipodui/powerlog.h"`) puts a phase mark on the next row from any thread on the CPU (PP1); marks from the COP are not taken. It does nothing while the log is off; commas and quotes become `_`, and tags are cut to 21 characters. The simulator command is `powerlog_mark <tag>`. How to run an A/B: `docs/hardware.md`, "Rundowns and A/B tests". It samples voltage, not current.
- `battery_bench` is compiled out of the shell.

**Voltage alone can't see this difference.** The expected saving is a few mA against a ~3000 mAh cell, so the voltage over a test of minutes moves less than the ADC's step.

**Method A (primary): an inline USB power meter, on a charge-only supply.**
- Charge the iPod fully, then put an inline USB-C meter (1 mA resolution or better) between a plain charger (no data lines, so no USB mode) and the iPod.
- With the battery full, the meter reads the system's draw.
- Run A/B/A/B/A/B, each phase 120 s with a 20 s settle, all inside one devtest stage (§8). The screen shows the phase name and a countdown in large type, and the owner notes each phase's mean (or photographs the meter's average at the end of each phase).
- **Controls:**
  - backlight on at a fixed brightness in both modes (the drift only runs lit);
  - audio stopped;
  - **no storage access:** both covers preloaded into RAM before phase 1, so A and B read nothing;
  - our BCM image in **both** modes (A pushes frames through `UPDATERECT`, B runs jobs), so only the work differs, not the firmware;
  - the PP clock is logged every phase: unboosted in both, except where A must boost to keep 50 ms fades, and then it's logged.
- Optional third mode C: the PP path with Apple's vmcs, a baseline against the stock bridge.

**Method B (cross-check, coarse): voltage slope.**
- The same A/B phases at 15 min each, battery mV from `power.csv` at 5 s.
- Fit the slope per phase; only a large difference shows.
- Useful only if no meter is to hand.

**The test's own log is the power log for this test.** `bcmtest.log` gets one line per phase:
- name, start and end (`USEC_TIMER`);
- battery mV at start and end;
- PP busy µs: time inside the job path, summed with `USEC_TIMER` around each frame's work;
- BCM busy µs: the BCM counter around each job, read back;
- frames drawn, frames late (more than 10 ms past due);
- CPU frequency.

The stage also calls `powerlog_mark()` at each phase boundary (tags such as `s5_A1`, `s5_B1` … `s5_end`), with the power log set to 5 s. `power.csv` then lines up with the meter readings (method A) and gives method B its slope per phase, with `boost_pct`, `bl_pct` and `disk_pct` confirming the controls held (disk 0, backlight constant).

## 7. Decision criteria

Offload the drift (behind a `HAVE_*` flag, CPU path kept: `approach.md`, rule 7) only if **all** hold:
1. **Pixel-exact:** the BCM path's frames equal the PP path's, in the simulator for every `tests/ui` script that shows the drift, and on the device by checksum readback (§8).
2. **Energy:** with Method A, mean current in B is lower than in A by more than twice the spread of the A phases (at least 3 alternations each). If it's equal or higher, the drift stays on the PP: the job is real, but this hardware doesn't reward moving it.
3. **No new latency:** no late fade frames in B (A's count may be non-zero), and the first frame after a cover change is no later than in A (the upload overlaps the previous cover's pans).
4. **Sleep unchanged:** the BCM still powers down with the backlight (FS#9890), and the drift's resident covers are re-uploaded on wake (one 12–24 ms upload, logged), never kept by holding the BCM awake.

If 1–3 pass and 4 needs work, the decision waits; it isn't a reason to keep the BCM up.

## 8. Device test design (specified only, not implemented)

**A devtest stage "5: 2D offload"**, gated like 4b: it runs only after stages 3 and 4 have passed for this image hash. It uses the hello_dma image plus `xfade_blk` and a job handler.
1. **Pictures:** two synthetic 300×300 covers made by the test from a seed (gradients plus a fine checkerboard corner), never owner art. They go into PP RAM, and B uploads them to the BCM (upload time logged).
2. **Exactness:** run one full cycle (8 fade steps + 44 pans) both ways.
   - The BCM writes each output frame to an SDRAM buffer as well as the LCD; the PP reads back an FNV hash over the host port and compares it with its own frame.
   - Log `exact k/52`. PHOTO 7 is taken mid-pan in B.
3. **Timing:** per frame, PP busy µs (A and B), BCM kernel µs and DMA µs (B), and wall time. Log the mean and maximum per kind (fade, pan, upload).
4. **Power soak (optional, owner choice at the stage prompt):**
   - Method A's A/B × 3 at 120 s, with the phase name and countdown drawn in large type on the left half.
   - The owner records the meter's mean per phase in the run's notes; the log carries the phase marks to line them up.
5. **Fallbacks:** a job timeout ends B, logs it, re-draws by the PP path and doesn't fail the stage (as stage 3's forced timeout does). A hash mismatch fails the stage.
6. **Runtime:** about 1 minute without the soak, about 15 minutes with it.

**Prerequisites, all in the same runbook as session 7 or later:**
- the `vst16` REP 16 bench check (§5);
- `vlsr`/`vand` checks, if the upload unpack is to be vectorised;
- an owner yes on having a USB-C inline meter.

## 8a. As built: stage 5b's mode B (p14-bcm-5b, 2026-10-12)

Stage 5b (dev9's A/B scaffold) now runs a real mode B instead of the push-only stub. It differs from §8 in a few places, each for a reason:
- **One image for both modes:** `drift` (`BCMFW_DRIFT=1`: hello_dma plus the drift ops, 10,368 bytes, sha256 `68cba97d…e069`). A and B run the same firmware, so only the work differs. The stub stays for a HAL without the image or the cover memory (logged as such).
- **The drift as the shell runs it** (s8-int 91b06b7784, with 8b's half steps from p8b-smooth 9fcbf9aed4), from a parameter table in `bcm-devtest-core.c` (`struct drift_def`, `drift_dirs`): the NAV_DRIFT_* values, `drift_start`, `nav_xfade_weight`, `xfade_rect`, `half_window`/`AVG2`, and `HALF_SLOW_US`/`HALF_SLOW_MAX`.
  - **A cycle:** 8 fade steps 50 ms apart, then 44 pixels 150 ms apart. Each pixel is shown whole, then 80 ms later (`NAV_DRIFT_HALF`) half-way to the next. Pixel 0's whole frame is the fade's last step, so a cycle has 8 + 43 + 44 = 95 frames, about 13.6 a second.
  - **The guard:** A turns its half steps off after 4 composes in a row over 25 ms, as the shell does until reboot (here, until the stage ends). B keeps its half steps and logs whether its op would have tripped the same guard.
  - **Change detection:** `xfade_test.py` fails if the shell's fade (this branch's source), or the half step's `AVG2`, `HALF_SLOW_*` and `NAV_DRIFT_HALF`/`NAV_DRIFT_STEP` (at 91b06b7784, by `git show`), stop reading as ported.
- **What B does:**
  - **Once per cycle, in the 150 ms gap after the last pan:** the PP uploads the next cover (300 rows of 600 bytes) to one of two slots. Then `PREP` unpacks the panel as it stands, and the next cover's first window, into 16-bit R/G/B planes. This is scalar code, about 25 ms at 27 MHz (estimated).
  - **Each fade step:** `FADE` runs the vector kernel over the 150 blocks and pushes the panel.
  - **Each half step:** `HALF` stages the next pixel's window into an aligned buffer (scalar, any alignment, the shell's odd-start case included), then averages it with the panel on the vector unit, and pushes.
  - **Each whole pixel:** `SHOW` copies that staged window onto the panel (a vector copy) and pushes it. A window that was never staged is copied by scalar code instead.
  - The PP's per-frame work is three parameter words and a command.
- **No read-back hash on the device.** Instead, the simulator checks every frame of both modes against a Python port of the shell's drift, so A and B are bit-exact by test.
- **The half step's kernel, `half_blk`,** works per channel on bytes.
  - **How it computes:** a vld16 row holds the pixels' low bytes in H8 columns 0–15 and the high bytes in 16–31. R = hi >> 3; G = (hi − R·8)·8 + (lo >> 5); B = lo − (lo >> 5)·32. Each channel's sum then goes through `vmulhr #32767`, which is floor(v/2) on 0..126, exactly AVG2.
  - **Forms:** vlsr D8 (LSR8), vmul D16 A8 Breg (MU8R, WRAP), vadd D16 A16 B8 (AD68), vadd D16 A16 B16 (ALUX), vmul D16 A16 Breg (MULR) and vmulhr #imm on non-negative values (MHR4). The decoder checks LSR8, AD68 and WRAP passed in session 7.
  - **Not yet checked on the device:** the byte split of a 16-bit load. That's the simulator's VRF model, which the decoder's display path also relies on, and session 9's stage 6 is its first run on silicon.
- **Forms:** `forms_test` passes the drift image with 0 unproven kinds.
  - The kernel uses single-row `vst16` stores of block-0 rows, all device-verified (COPY, COP8).
  - The REP 16 store variant (`xfade_blk_rep`, session 8's informational check) is built with `BCMFW_DRIFT_REP_STORE=1`. It is bit-exact in the simulator and not the default.
  - The source is held as 16-bit planes, not 8-bit, so every load and store stays 32-byte aligned: the device-checked alignment.
- **Cost per half step** (the same estimate): `half_blk` is 59 instructions, ~510 cycles a block. A step is about 9,800 instructions, ~78 k cycles: 2.9 ms at 27 MHz, 0.5 ms at 150 MHz.
  - **The staging copy:** about 155,000 scalar instructions, roughly 6–8 ms at 27 MHz. It moved here from the whole pixel's `SHOW`, which is now a vector copy (~3,600 instructions).
  - **The op as a whole:** in the simulator, 5.5 ms including the push. Far under the shell's 25 ms guard.
- **Cost per fade step** (simulator instruction counts × the device's REP timings):
  - single stores: about 17,400 instructions and ~74 k cycles, 2.7 ms at 27 MHz;
  - REP stores: about 8,200 instructions and ~71 k cycles.

  The single stores are nearly as fast, because a one-row store costs about 1.5 cycles against a REP 16 store's 16.5.
- **Memory (BCM SRAM, checked by `build.py`):**

  | Region | Address range |
  |---|---|
  | image | below 0x10000 |
  | cover 1 | 0x10000–0x3BF20 |
  | state planes | 0x40000–0x78400 |
  | fade planes | 0x78400–0xB0800 |
  | half-step staging | 0xB0800–0xC3400 |
  | stack | 0xCC000–0xD0000 |
  | framebuffer | 0xE0000–0x105800 |
  | parameters | 0x106000 |
  | cover 0 | 0x108000–0x133F20 |
- **Logged per phase:** frames, late frames, PP busy (composing and uploading; B's cover uploads count), BCM busy (A: the push's DMA time; B: the op and its push, as the image times them), and the PP's wait for the BCM. `readlog.py` turns the totals into per-frame figures.
- **Memory on the PP:** the two covers (360,000 bytes) come from a new HAL call, `scratch`. On the device that's `core_alloc`, for the stage only.

## 9. Files

| Path | What |
|---|---|
| `bcmfw/video/gen_xfade.py` → `xfade_kernels.S` | The kernels' generator and output: `xfade_blk` (single-row stores) and `xfade_blk_rep` (REP 16) |
| `bcmfw/video/xfade.c`, `xfade.h` | One panel step, block by block, and the RGB565 → planes unpacking |
| `bcmfw/video/drift.c` | The drift image's ops (`SHOW`, `PREP`, `FADE`, `HALF`), on `CMD_DRIFT` (0x42) |
| `bcm-devtest-core.c` | Stage 5b: the drift table, modes A and B, the per-phase log |
| `bcmfw/tests/xfade_test.py` | Both fade kernels bit-exact against `xfade_rect()`, the half step and the staged copy against `AVG2` at every x parity, the shell-source checks, the cost lines (~55 s; not yet in `bcmfw_benches.sh`) |
| `bcmfw/tests/devtest_test.py` | `t_5b`: both modes, every frame against the shell's drift; `t_5b_guard`: A's half steps off, B's "would trip"; `t_5b_stub` |
