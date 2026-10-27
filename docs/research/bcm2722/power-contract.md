# BCM power states: contract rev 0.1

**Names:** these documents use the technical names: PP5022 (its cores CPU and COP), BCM2722 ("the BCM"), WM8758. In chat with the owner they are PP, PP1, PP2, VC and DAC.

P19 contract, draft for the brief review (2026-10-23).
- **Companions:** `jobs.md` (the job interface), `memory-contract.md` (what lives where), `docs/phase18-brief.md` (soft sleep, dark playback), the power budget in `docs/hardware.md`.
- **Tags:** [silicon] measured, [sim] simulator only, [ours] our design, [hypothesis], [estimate].
- **Nothing here measures current.** No state's current draw is known yet (#12). The policy below is written so each measurement changes a number, not the structure.

## 1. The states

| State | Panel | BCM power | Core | What is kept | Wake cost | When |
|---|---|---|---|---|---|---|
| **C1 Lit, active** | on | on | running jobs or video; 27 MHz, 150 MHz only for video or a Class C game with the 4b/4c flags | everything | — | the screen is lit and something draws |
| **C2 Lit, idle** | on | on | busy-polls today [silicon: our images]; asleep between jobs on the 1 ms tick if stage 5i proves it (#3) | everything | 6–7 µs to the next command [silicon S2] | lit, nothing to draw |
| **C3 Dark, light standby** | display off, power circuits on, reset high (our standby, [hypothesis]) | on | idle (C2's way) | SRAM [hypothesis, #50]; SDRAM only if refreshed | about 137 ms to a picture (panel 3, [sim]; skips the reset, its 110 ms and the resync) | a short dark spell, if 5s says it's cheap |
| **C4 Dark, deep standby** | the whole off script, reset high [hypothesis] | on | idle | SRAM [hypothesis] | about 137 ms [sim]: the init and on scripts | as C3, if cheaper |
| **C5 Dark, panel off, BCM up** | `LCD_SLEEP` | on | idle | SRAM [hypothesis] | panel power-on about 265 ms [silicon S7] | none planned; the stage 5s comparison point |
| **C0 Cut** | off | off (`GPO32` bit 14, regulator off) | — | **nothing**: SRAM, SDRAM and the registers are lost [silicon] | **cold**: image upload (the resident copy, 15–34 ms by CPU, about 16 ms by engine 2), bootstrap, panel power-on; **0.37–0.62 s** to the first frame [silicon range, S7 and `hardware-fit-audit.md`]; plus slot re-uploads | today's behaviour whenever dark (FS#9890), and in P18's soft sleep |

**The rule that matters:** the BCM is in C1 or C2 exactly when the panel is lit. That's why lit UI offload costs it little extra (`display-offload-plan.md` §1). Every dark state is a trade between holding something and waking cold.

## 2. Policy, rev 0.1

1. **Dark means cut (C0), by default.** That's today's behaviour, the safe one, and what P18's soft sleep does (screen, video BCM, codec and storage off; `phase18-brief.md` goal 1). It serves the dark-playback goal (the BCM off unless needed, under 25 mA in all) without needing a measurement.
2. **A standby (C3 or C4) is allowed only after stage 5s shows all three:**
   - the panel wakes from it with the right picture (#49);
   - SRAM holds there (#50; 5s's framebuffer check);
   - its current makes holding it cheaper than a cold wake, over the time the user is likely to be dark.

   Even then it's used **only for a window after the screen goes dark** (the backlight timeout, not a sleep). The shape is the same as P18's Hold-wake window: `BCM_STANDBY_WINDOW_S`, a named constant set from the measurement. After the window, cut. A press in the window wakes in about 140 ms with every slot intact.
3. **The clock.** 27 MHz in UI mode. 150 MHz only inside a video session (`HP_OPEN_FAST_CLOCK`) or a Class C game session, each needing the 4b and 4c flags; dropped back at the session's end and before any dark state.
4. **SDRAM.** Off in UI mode unless an SDRAM pool is in use (`memory-contract.md` §3). Its self-refresh sequence is unknown (#7), so the way to stop paying its refresh is the cut. A UI SDRAM pool is therefore released, and SDRAM left idle, before any standby; a standby with SDRAM refreshing is a separate measurement.
5. **Core sleep (C2).** Busy-polling until stage 5i passes. Then the idle image (the 1 ms tick, the host doorbell if it fires) becomes the default, and the job loop sleeps between jobs and animation frames. Stage 3d's idle time per frame is how much a 30 fps animation leaves to sleep through.

### The unlit face window (review M6)

For about 90 s after the backlight goes off, the shell's unlit clock face keeps the panel showing (backlight off) before `lcd_sleep()`, which runs on the CPU (PP1), from the backlight thread or a CPU service the COP UI asks; it reaches `bcm_init` and file reads, so never on the COP (`unlit.c`, `changes-from-7g` 2.25).

- **Its state:** the panel is on, so this is **lit for the BCM**: **C2** (idle: busy-polling today, asleep between jobs once 5i passes), with a C1 burst each minute for the 320×68 time band and on status changes.
- **What survives:** everything. The BCM stays powered, so slots, the retained frames and the epoch are kept.
- **A wake inside the window** is not a cold start: the backlight comes on over the frame the panel already shows. The shell redraws as it does today, and the slots are still there for the drift.
- **Animations:** none run in the window; the unlit face is static apart from its minute update. The drift and marquee are stopped with `ANIM_STOP` when the face appears.
- **After the window,** `lcd_sleep()` cuts the BCM (C0), or holds a standby once stage 5s allows it (§2 item 2).
- **For the dark power-up count** (`bcmjob_dark_ups`, gap G12), "dark" means **the panel is off** (C0, C3, C4, C5). The unlit window is not dark, so nothing in it counts.

## 3. Interactions

**P18 soft sleep:**
- The BCM is cut at sleep entry: after the last frame is shown and any running animation is stopped (`J_OP_ANIM_STOP`), and with the epoch noted.
- The resident image stays in PP RAM through the sleep, so no disk read at wake (`phase18-brief.md`, "Resident BCM image").
- At wake the image is uploaded (P18 18-3 moves this to engine 2) and the last frame is pushed: Q3, "the screen the user left".
- **Slots are re-created lazily, never at wake:** the first frame comes from the PP's framebuffer as today, and the drift's covers go back up at the drift's next cover change, from the art cache. P18's no-disk-at-wake rule holds.

**Dark playback** (music with the screen off): the BCM is cut. Nothing it does is needed. Audio is the PP's, through the DAC.

**Video:** a session holds the BCM in C1, at 150 MHz if allowed. Backlight off during video follows `hostproto.md` §6: `SLEEP`, then the resume time, then the cut. Audio carries on. At wake: `HELLO`, `OPEN`, `SEEK`. The UI's slots are gone after any video (the epoch).

**Games:** a Class C session holds C1 and its own clock, and evicts the UI's pools. At its end it drops the clock and bumps the epoch.

**Failures:** a BCM that misses its deadlines or stops its heartbeat is cut and restarted by the boot ladder (`jobs.md` §6). The state machine always ends a failure in C0.

## 4. Re-upload costs after a cut [estimate]

From engine 2's ~11 MB/s with the CPU 97 % free [silicon S37]:

| Item | Size | Upload |
|---|---|---|
| The image | about 210 KB | about 19 ms |
| A drift cover | 180,000 B | about 16 ms |
| A marquee strip | e.g. 1,200×24 px, 57.6 KB | about 5 ms |
| Cover Flow, 16 covers | 512 KB | about 47 ms (while Cover Flow opens behind its snapshot) |

Each is read from the PP's caches, so a disk read is needed only for what isn't already in RAM (the art cache's panels).

## 5. Measurements owed

Each one decides a number or a branch above:

| # | Question | Stage | Decides |
|---|---|---|---|
| #12 | Current in C1, C2, C3, C4, C5 and C0 | 5s (its timed phases; a second run after 5i for C2 asleep): no meter for a long time (owner, 2026-10-25), so the proxies and the long-run battery log (§8) stand in | whether standby is worth having; the dark-playback budget's BCM share (0 if cut) |
| #3 | Does the core sleep and wake on the tick or doorbell? | 5i | C2 polling vs sleeping; the job loop's idle mode |
| #50 | Does SRAM hold in standby? | 5i, 5s | whether C3/C4 can keep slots |
| #49 | Does the panel wake cleanly from standby, and how fast? | 5s (looks 2–3) | C3/C4 at all; `BCM_STANDBY_WINDOW_S` |
| #51 | Engine-2 image upload at address 0 | 5s | P18 18-3's faster wake |
| #52 | The panel's refresh period (TE) | 3d | `J_PRES_TE`, 30 fps pacing |
| #55 | Our renderer's real cost per pixel at 27/150 MHz | 3d | the job deadlines and the 30 fps budget |
| #7 | SDRAM refresh current; self-refresh | **none yet**: a bench step (`bench-p19.md`) | SDRAM pools in UI mode |
| #44 | Does port traffic slow the BCM? | 3d (panel DMA alongside) and a bench step | job timing under uploads |
| 5b | The drift on silicon (A/B: PP vs BCM) | 5b | plan item 1, the first P19 deliverable |
| #11 | Regulator sweep | last, after 5i | a lower core voltage in C2, maybe |

## 6. Retention and the instant wake frame (P19 19-4)

The owner's wish: a wake shows the last frame at once. What "retained" means in each BCM state, from the evidence we have, and what each kind of wake needs. [ours unless tagged]

### 6.1 What survives, state by state

| State | The BCM's memory: the image, `J_SURF_FRONT`, `J_SURF_BACK`, slots, `J_EPOCH` | The panel's own picture | Evidence |
|---|---|---|---|
| C1, C2: lit | all of it | shown | [ours]: the BCM is powered and nothing resets it. The epoch rule (`jobs.md` §4) says when contents are lost |
| **The unlit face window** (C2, about 90 s, backlight off) | all of it | kept: the face is on the glass | §2, "The unlit face window". The face draws into the PP framebuffer and pushes it (`unlit.c`), so **FRONT and the PP framebuffer then hold the face, not the screen left** |
| C5: panel off, BCM up | all of it | gone: the panel's power-on takes about 265 ms [silicon S7] | [ours] |
| C3, C4: standby | SRAM [hypothesis, #50]; SDRAM only if refreshed | [hypothesis, #49] | stage 5s, not run |
| **Hibernate** (Apple's `videocore=1`: bit 15, then bit 14 low) | SRAM and the parked core [apple: the wake path resumes the parked loop]; SDRAM [hypothesis] | if GPIO 48 and the panel's supply hold [hypothesis] | **PW5**, built (`HP_OP_HIB`, the plain video image only), checked in vc2sim, **not run** (`brief-map-power.md` §3, §9) |
| C0: cut (today after the window, and in P18's soft sleep) | treated as lost. Every start uploads the image again with a new epoch. Whether SRAM is actually blank after a cut has never been read back (`brief-map-power.md` §3) | unknown: PW6's B2 photographs it, not run | [silicon] for the cold start, 0.37–0.62 s to the first frame |

### 6.2 What an instant wake frame needs

1. **A right frame, already somewhere the panel can get it fast.** The PP framebuffer always holds one, by screen truth (P19 brief §3.6). The BCM can hold the screen left in BACK, which nothing uses while the screen is dark or showing the face.
2. **A path to the glass in about one frame:**
   - the panel already shows it: the backlight is enough;
   - the BCM is up with the frame in its memory: one `BLIT` and a `PRESENT`, about 15 ms, nothing over the port;
   - a cold start: 0.37–0.62 s whatever the shell does.
3. **Trust.** An unchanged `J_EPOCH` means FRONT, BACK and the slots are what the PP left there (`jobs.md` §4). A changed epoch means push the PP's snapshot.

### 6.3 Possible now (lit, BCM up)

- **Inside the unlit face window** (the common short wake):
  - before the face's first push, the shell sends `BLIT` FRONT → BACK (0.84 ms on the BCM [sim, `jobs.md` §10], no port traffic). BACK now holds the screen left;
  - at the dismiss, before the top screen's full redraw (`dismissed()` → `ui_invalidate(UI_DIRTY_ALL)`), it sends `BLIT` BACK → FRONT, `PRESENT` with `J_PRES_WAIT`, `FENCE`, and turns the backlight on after the fence;
  - the screen left is on the glass about 15 ms after the press [estimate: 0.8 ms of BLIT, 14.3 ms of push, silicon S7];
  - the PP's redraw (its framebuffer still holds the face) then runs behind and pushes only what changed while the face was up: the header's clock, the battery, Now Playing's time.

  While the redraw runs, the whole screen is BCM-owned (brief §3.6), so nothing pushes the face back. The epoch is checked before the `BLIT` (a video, a game or a cut in between empties BACK); a different epoch means today's full redraw. `jmtest` runs it on the model: the screen kept through the face and back, exact.
- **A wake from a short dark spell inside the window, not showing the face** (if the shell ever skips the face): the panel still shows the screen; backlight on, nothing else.
- **The unlit window ends in C0:** nothing is retained. The cold wake's first frame is the PP framebuffer, the boot ladder's first push. That is the snapshot (below).

### 6.4 What needs PW5's hibernate (SRAM retained)

If PW5 passes on silicon (`0x1F0` still reads `0xA5A5_0000` after the hold, the image answers the wake word, the SRAM CRC is unchanged):
- **Instead of C0 after the window,** the BCM hibernates. A wake costs about 7–17 ms [estimate, `brief-map-power.md` §3]: the pin sequence, then the BCM's clocks back. There is no image upload and no slot re-upload, and BACK still holds the screen left.
- **If the panel kept its picture** (GPIO 48 and its supply held; PW5's look 1): backlight on, and that's the frame. Otherwise the panel's power-on (265 ms [silicon S7]) and then one `PRESENT` from the retained frame. That is still faster than the 0.37–0.62 s cold start, and the port carries nothing.
- **What the images would need** (since 2026-10-25 built in simulation, off until silicon answers: §6.6):
  - `HP_OP_HIB`'s park and wake in the merged and display-only images (today only in the plain video image, a probe);
  - `J_EPOCH` kept across it;
  - the wake answering only if its CRC of FRONT, BACK and the slot pools matches the one taken at park, else a new epoch, so a bad hold costs a cold frame and never a wrong one.

  The CRCs read about 450 KB of SRAM at 27 MHz. With F16w's 3.5 instructions a pixel that is about 30 ms [estimate], so a quicker check (FRONT alone, or the pool headers) may be the trade.
- **The PP side:**
  - a hibernate state between the window and C0: the GPO32 bit 15/14 order and `BCM_STANDBY_WINDOW_S` (§2), set from PW5's current figure (#12);
  - `bcmjob_dark_ups` doesn't count a hibernate wake as a power-up (the BCM never lost power).

### 6.5 The PP's order at a wake: snapshot first, stream behind

1. **The snapshot, before anything else:**
   - inside the window: BACK's frame by `BLIT` and `PRESENT` (§6.3);
   - from hibernate: the retained frame, or nothing to do if the panel held it;
   - cold: the image up (engine 2, about 16 ms), `HELLO`, then the PP framebuffer pushed as the first frame.

   No disk and no re-render before the first frame (P18's no-disk-at-wake rule; Q3, the screen the user left: `phase18-brief.md`).
2. **The backlight** comes on after the first frame is on the glass, so a stale or half-pushed frame is never lit.
3. **Behind it, within about 0.5 s:**
   - the PP redraws its framebuffer if the face overwrote it;
   - only the bands that changed are pushed (or sent as jobs);
   - the drift resumes its cycle;
   - slots go back up lazily: the drift's covers at its next cover change, from `pv.slot`; a marquee strip at the next selection. Never at the wake itself.
4. **Animations** (the drift's steps, a slide, a glide) start only after step 1, from a synced front frame (`jobs.md` §12.5).

### 6.6 The production hibernate: design and simulation (2026-10-25)

**Status:** built and tested in vc2sim only. **Off** on every unit until PW5 passes on it and a bench row has run this stub there (below). Nothing here has touched silicon. [ours; sim]

**The contract.**
- When the release line goes dark (the unlit face window ends, §2), the PP **holds the BCM in hibernate instead of cutting it**, if and only if:
  - the build has the switch (`HAVE_BCM_HIBWAKE`, defined by no build today);
  - PW5's PASS is recorded on this unit (its 1 s hold came back parked, the image answered, SRAM intact);
  - this stub's FNV is recorded as having run its own cycle on the unit (bench row **HW1**, below): the `verified.txt` discipline, never before silicon says yes;
  - and the decision rule (below) says the hold is worth it.
- **On wake** the image resumes with `J_EPOCH`, FRONT, BACK and every slot as they were. The PP shows the retained frame at once: `BLIT` BACK → FRONT (BACK holds the screen left, §6.3), `PRESENT` with `J_PRES_WAIT`, `FENCE`, then the backlight. Snapshot first, stream behind (§6.5).
- **Every doubt is a cold boot and a new epoch.** No `PARKED` after the pins, no answer in time, `GAVEUP`, `REFUSED`, a sum that differs: the PP cuts (bit 15 low, bit 14 low, 100 ms) and runs today's cold path. A bad hold costs today's cold frame, never a wrong one.

**The epoch rule.** A hibernate keeps the epoch only if the image proves its memory held before it answers:
- at the park the stub sums a list of regions the PP chose (`hibwake.h` `hw_sum`: a += w; b += a, over every `stride`-th word);
- at the wake it sums them again, **before** it writes `AWAKE`;
- equal: `AWAKE`, and it returns into the image, which runs on in the same epoch; `bcmjob_dark_ups` does not count it (the BCM never lost power);
- different: it writes `HW_SPOILED` and **never returns** (it spins). The image never runs on memory it can't trust. The PP cuts, cold-boots, and the driver's epoch moves as after any cut.

**The default regions** (`bcm-hibwake.h` `bhw_default_regions`; the PP may choose others):

| Region | Merged | Display-only | Stride |
|---|---|---|---|
| the image, its data and `.bss` (`0x200` up; the mailbox below is live) | to `0x35400` | to `0xC400` | every word |
| the job block and slot table | `0xDE000–0xDE87F` | same | every word |
| BACK (the screen left: the frame the wake shows) | `0x105800–0x12AFFF` | same | every word |
| the slot pools, scratch, FRONT (the face, about to be replaced) | pools `0x35400–0xCB3FF` | pools `0x46000–0xCBFFF`, thumb | one word in 16 |
| the spare and parked AAC tables / the overlay region | `0x12B000–0x13FFFF` whole | overlay whole | every word |

A lost SRAM is lost everywhere, so sampling the pools catches it; a spot of decay inside a cover would be missed (`hibwake_test` shows one such word passing, by design), which is the trade for a wake of 13–23 ms instead of about 60 ms. The stack, the rings, the stub and its block are not summed: corruption there stops the core or the protocol, which the PP's deadlines already turn into a cold boot.

**The pieces** [sim]:
- **The stub** (`bcmfw/hibstub.c`, 1,072 B, linked at `HW_STUB` = `0xDEA00`, its block at `HW_PARAM` = `0xDF400`; `memory-contract.md` §2). It is **PW5's park and restore** (`hostproto.c` `cmd_hib`, the plain video image's probe) without the probe's patterns, bars and allowlist: Apple's `videocore=1` order (PLLs B, C, D, A to `pw 0`; the 14 GPIO event words saved and cleared), `HP_HIB_PARKED`, a busy poll for `HP_HIB_WAKE` bounded by the block's ms and by a counter that stops; then the PLLs by Apple's rule (each divider, its control without the enable, 1 ms, the enables), the event words back, and **the idle tick re-armed** (the counter may have run or reset in the dark, leaving compare 1 behind it; `crt0.S` re-arms only from a match). No SDRAM step: the hook refuses while SDRAM is up.
  - It lives outside every image, uploaded by the PP, so **one copy serves both images** and neither pays its 1 KB. That is what made the merged image's hook fit.
  - Built by `build.py` `build_hibstub` (vc2cc `--conservative`: Apple's instruction kinds only), embedded by `mkdevtest.py` as `bcmdt_hibstub_image`.
- **The hook** (`hostproto.c`, `BCMFW_HIB_PARK`): `HP_OP_PARK` with `HP_PARK_MAGIC, HP_PARK_HIB`. It refuses with `HP_E_STATE` (nothing done) without the stub's magic at `HW_STUB`, and, in the merged image, with SDRAM up; the warm swap's own refusals (a session, the fast clock, a standby) come first. Otherwise it replies OK, finishes the ring pass, waits for the panel's transfer and calls the stub. **Sizes:** the display-only image grows 160 B (45,960 → 46,120 B; 384 B left below the decode work area; FNV `0xef22812c`). The merged image's hook is a **separate commit** (the merged image is frozen until its device verify): with it the merged image is 218,596 B, 64 B *smaller* than today's 218,660 (the hook's `park()` returns, so the compiler stops treating it as `noreturn`), 164 B spare below the arena.
- **The PP side** (`firmware/target/arm/ipod/video/bcm-hibwake.h`, header only, not wired): the decision rule, `bhw_prepare` (the stub, read back word by word; the block), `bhw_park` (the request, `PARKED`, then bit 15 high, 5 ms, bit 14 low), `bhw_wake` (bit 14 high, 5 ms, bit 15 low, the word still `PARKED`, `WAKE`, `AWAKE`), `bhw_drop` (bit 15 low from the hold: both pins low, Apple's "off"). It is `bcmdt_hib_cycle`'s shape split in two, since the dark hold has no fixed length. **A proposal:** the driver's `lcd_sleep()` and `lcd_awake()` (the orchestrator's `lcd-video.c`, `bcmjob-*.c`) would call it; nothing is applied to their files.

**Timing** [sim; 1 cycle an instruction at 27 MHz, a floor]:
- the wake's handshake and second sums: 345,920 instructions in the display-only image, **12.8 ms**; 623,360 in the merged image (its image region is 4.6 times larger), **23.1 ms**;
- plus Apple's 5 ms pin window, then the `BLIT` and the panel's `PRESENT` (about 15 ms, §6.3);
- so **about 33–43 ms from the press to the screen left**, against 0.37–0.62 s cold [silicon]. If the panel loses its picture in the hold (PW5's look 1), add its 265 ms power-on [silicon S7]: still faster than cold, and nothing crosses the port.

**The power cost and the decision rule** (from §7's model; [estimate]: no meter will settle it soon, so the logged proxies and the long-run battery log decide, §8):
- a cold wake costs about **6.1 mA·s** (0.5 s of the BCM polling and the PP waiting); a hibernate wake about **0.43 mA·s**;
- the hold draws, above C0: **0.2 mA** if the chip's clocks stop (PW5 reports the counter `STOPPED`), **1.2 mA** with the panel kept powered, **5.2 mA** if the core keeps polling (`ran`) with the panel kept;
- **the break-even hold** T* = (cold − hibernate wake energy) / hold current: **28 s**, **4.7 s** and **1.1 s** for those three.
- **The rule** (`bhw_choose`, `bhw_drop_now`):
  1. at dark entry, hold only if the expected cost of holding (capped at T*) over the recent dark spells (`bhw_hist`, the last 16, RAM only) is less than one cold wake; with fewer than 4 spells known, hold;
  2. while held, drop to C0 once the hold has lasted T*.

  The cap makes it the ski-rental rule: whatever the spells, it never spends more than twice the better choice in hindsight (the test checks 2,000 random spells: worst ratio 2.0). **Its worst waste is one cold wake's energy per dark spell, about 0.03 mAh a day** at 20 dark entries.
- **What this says** [estimate]: after the 90 s window, holding pays only for spells shorter than T*, seconds to half a minute, which a window of 90 s has already filtered out. So **after the window, hibernate is a speed feature, not a power one**: it buys the instant wake for spells up to T*, at a cost the cap bounds to noise.
- **Where hibernate saves real energy is inside the window**: the unlit face keeps the BCM polling (C2) for 90 s. If PW5 shows the panel keeps its picture through the hold, the BCM can hibernate between the face's minute updates: about **358 mA·s a dark entry, ~2 mAh a day** at 20 entries [estimate], or ~0.7 mAh a day once the idle sleep (5i) has cut C2's polling. That is the hibernate change worth making first, once PW5 and 5i report.

**Tests** [sim]: `bcmfw/tests/hibwake_test.py` (14 rows, 262 cases): the stub's header and size; refusals (no stub, a bad block, bad regions) with nothing written and the image running on; the park's order, the quiet park, the restore order, the tick re-armed, the sums equal, `J_EPOCH` kept, a slot kept (an `ALLOC` hit at the same address), BACK kept and a kept cover reaching the panel, in both images; a changed word in BACK, in a sampled pool word or in the image's data each give `HW_SPOILED` with the image silent after it, and the cold boot that follows starts a new epoch (the slot is gone); an unsampled pool word passes (by design); `GAVEUP` with the image running on; the merged image `BUSY` in a session and refused with SDRAM up after a video; the PP header against the display-only image with a model of the two pins (retained: awake, epoch kept, then a drop; cut, lost, stuck and a spoiled BACK: each the standard cut), Apple's order and 5 ms windows, no port access while bit 15 is high, the other `GPO32` bits kept; the decision rule; the PP header's constants against `hibwake.h` and `hostproto.h`.

**Bench row HW1** (after PW5 passes, with its own owner go; not built in the developer UI yet): the PW5 sequence with the release pieces: `bhw_prepare`, `bhw_park`, a 1 s then 10 s hold, `bhw_wake`, then a `FENCE` and an `ALLOC` hit on a committed slot. Pass: `AWAKE`, epoch kept, the slot hit, the screen left shown from BACK. It records the stub's FNV for `bhw_gate`.

## 7. The BCM's share of lit current: a model (2026-10-25)

`scripts/bcm_power_model.py` (no dependencies; `--md` prints its parameter table with each source and tag, `--json` everything). **Every current below is an estimate**: no state's current has been measured (#12). The structure is meant to survive the measurements; each one replaces a parameter.

**The cases** (the release line: `--ipodui --bcmfw --bcmfw-default --bcmfw-l1 --bcmfw-merged`; one typical lit hour: 35 % drifting, 15 % with a marquee, 240 slides, 2 minutes of glides [estimate]):
- **A:** jobs on (the drift, slides, glides and the marquee drawn on the BCM), our merged image polling between jobs (today);
- **B1:** the CPU drift, our image as the LCD driver only;
- **B2:** the CPU drift, Apple's vmcs as the LCD driver (its 5 MHz floor at level 5, sleeping between commands [apple]);
- **A+s:** A with the idle sleep (5i) and level 5 (PW3);
- **A+24:** A with the lit clock at 24 MHz once the drift no longer holds it at 30 (`lit-24mhz.md`).

| Case | BCM | PP's differing work | Pads | Clock | Sum | vs B1 | BCM share of ~150 mA lit | Lit hours from full |
|---|---|---|---|---|---|---|---|---|
| A | 4.23 | 0.01 | 0.01 | 0 | 4.25 | **−0.50** | 2.8 % | 20.07 |
| B1 | 4.18 | 0.48 | 0.09 | 0 | 4.75 | — | 2.8 % | 20.00 |
| B2 | 0.50 | 0.48 | 0.09 | 0 | 1.07 | −3.68 | 0.3 % | 20.50 |
| A+s | 0.97 | 0.01 | 0.01 | 0 | 0.98 | −3.77 | 0.6 % | 20.52 |
| A+s+24 | 0.97 | 0.01 | 0.01 | −0.70 | 0.28 | **−4.47** | 0.6 % | 20.61 |

(mA at the battery; "lit hours" is continuous lit use from a full 3,000 mAh cell at 150 mA less the difference.)

**What it says** [estimate]:
- **The BCM is about 3 % of lit current** (1.7–4.8 % over the per-MHz range), and **almost all of it is our image's busy-poll at 27 MHz and level 11**, not the jobs. The jobs keep the BCM's vector unit busy about 82 s a lit hour (2.3 %, from the cost audit's per-frame times [sim]), which adds only ~0.05 mA.
- **Jobs on saves about 0.5 mA** averaged over a lit hour (0.26–1.4 mA over the ranges; ~1.4 mA while the drift runs): **0.5 mAh a lit hour, 0.02 % of the cell, about 18 s of dark playback.** Real, but small: the PP's pixel work at a fixed 1.2 V is cheap in energy, even where it is expensive in time.
- **The levers that matter are the BCM's idle**, worth 7 times more: the idle sleep (5i) and Apple's level 5 at 27 MHz (PW3) together save about **3.8 mA** whenever the screen is lit; with the 24 MHz lit clock the jobs make possible, **4.5 mA**: **4.5 mAh a lit hour, ~2.6 min of dark playback, +0.6 lit hours from full**. B2 shows the same thing from the other side: Apple's vmcs, sleeping at 5 MHz, costs ~0.5 mA where our polling image costs ~4.2.
- **So the jobs' value is time, not charge:** they free the PP (no boost for the drift, 24 MHz lit, room for the UI), and they are what lets the lit clock drop. The battery case for the BCM work rests on 5i and PW3.

**Sensitivity** (one at a time over each range; A − B1 in mA; the BCM's share in A):

| Parameter (least sure first) | Range | A − B1 | BCM share |
|---|---|---|---|
| `k_bcm`, the BCM's mW per MHz at 1.2 V | 0.3–1.0 | −0.52 … −0.46 | 1.7–4.8 % |
| `e_pp_cyc_nj`, the PP's energy per cycle | 0.2–1.2 nJ | −0.28 … −1.40 | 2.8 % |
| `mix_drift`, the drift's share of lit time | 15–60 % | −0.26 … −0.81 | 2.8 % |
| `p_leak11`, the BCM's static power | 0.5–6 mW | −0.50 | 2.5–3.7 % |
| `vec_factor`, vector-busy over scalar-busy | 1.2–2.5 | −0.54 … −0.43 | 2.8–2.9 % |

The BCM's share hangs on `k_bcm` and `p_leak11` (no meter will settle them soon: the long-run battery log, §8, is what narrows them, over weeks); the jobs' saving hangs on the PP's energy per cycle and the mix. No plausible value makes the jobs cost charge.

**The measurements that would replace the estimates** (owner, 2026-10-25: no meter-based tests for a long time; the rows below keep their meter readings optional, and the decisions they fed now follow §8's rule: the simulation's proxy, the logged proxies, and the long-run battery log):

| Measurement | Replaces | Where |
|---|---|---|
| **PW1** (power census: the regulator level, the PLLs and block clocks each image leaves running, the counter) | confirms level 11 and which clocks run at 27 MHz: the shape of `k_bcm` and `p_leak11` | `bench-p19.md` PW1 |
| **PW3** (the voltage ladder 11 → 9 → 7 → 5, a meter reading each 20 s) | the BCM's current against voltage: `k_bcm`, `p_leak11`, `v_l*`; whether level 5 holds (the A+s row) | PW3 |
| **PW4** (Apple's clock ladder, a reading each 10 s) | the BCM's current against MHz: `k_bcm` directly | PW4 |
| **PW5** (hibernate, a reading during each hold) | the hold current (`i_hold_*`), the counter `STOPPED` or `ran` (which break-even row applies), whether the panel keeps its picture (`i_panel_hold_ma`), the wake time | PW5 |
| PW6 (the panel with the BCM cut) | a case C: a static lit screen with the BCM off (the largest possible lit saving, the whole ~4.2 mA) | PW6 |
| PW2, PW7, PW8 | none here (caches, the ALT bank, DMA copies: timing, not current) | — |
| **The bench session's `b3ab`** (drift on the BCM 5 min, then off 5 min) with **`power.csv`** and **a USB-C meter** at full battery | A − B1 while drifting, directly (model: ~1.4 mA). Without a meter, `power.csv`'s voltage cannot resolve it (the floor is 8–11 mA: `lit-24mhz.md` §5.3) | `bench-p19.md` Part B |
| Stage 5i and 5s (#12: C0, C1, C2 currents) | the polling idle (`k_bcm` × 27 MHz) against the sleeping one (`sleep_factor`): the A → A+s step | `p18-0-runbook-bcm.md` |
| The meter's "boost on/off, 2 min each" (`hardware.md`, meter calibration) and the lit-24 row (S-M1) | `e_pp_cyc_nj` (the PP's mA per MHz busy) and `lit24_ma` | `hardware.md`, `lit-24mhz.md` §5.4 |
| A meter across 30 backlight cycles a minute, cut against hibernate (after HW1) | the cold wake's energy (`cold_s`, `i_pp_wait_ma`): the break-even's numerator | §6.6 |
| **VTAP** (the visualizer tap's uncached costs) | nothing in this mix: it measures the tap's CPU share (time), not current. It would enter only if the visualizer joins the lit mix, as PP cycles | `bench-p19.md` VTAP |

**The levers, built (p19-battery):** idle sleep and level 5 need no image change (the gate is in `cd8e5d96`; the PP writes the regulator); the 24 MHz lit clock is a PP policy. Each has a developer switch, default off, and a bench row (`slp`, `lvl5`, `lit24`) whose verdict is automatic; a default turns on by §8's rule (`battery-levers.md`).

## 8. The video chip's clock: level bands and a governor (P19 PW4, 2026-10-25)

`battery-levers.md` §4 has the design, the simulation and the sizes; this is the contract.

**Driving the hardware follows Apple; the policy is ours** (owner, 2026-10-25: how Apple drives the chip and how it uses its software on the chip are distinct). Each part is tagged:

| Part | Tag | What, and the source |
|---|---|---|
| The range | driving: [apple] | 5–150 MHz, never above 150 (`pm_set_policy`, @fcec; the setter's clamp, @5134 step 1). Ours uses 27–150 inside it (the crystal is our floor) |
| The levels | driving: [apple] | @141ba's 13-row table, the first row at or above the clock; never a level below it (`../../bcm2722-reference/brief-map-power.md` §4) |
| The order | driving: [apple] | a higher level before the clock goes up, then 10 ms; a lower level after it came down, then 10 ms (@5134 steps 2 and 5; the setter @141f0 waits 10 ms) |
| PLL A | driving: [apple] | M 108 (250 kHz), N = VCO / 250 kHz, the VCO at 75 MHz or more by running at a multiple of the clock and dividing in the core word; 1 ms before the core moves (@4cb8; @5134 steps 3–4) |
| The core word | driving: [apple] | `0x100 | divider` after the lock (@5134 step 4) |
| SDRAM refresh | driving: [apple] | f / 128,000 after the change (@4d18; @5134 step 6). A margin [ours]: 27 MHz's interval before the change too (it only refreshes more often meanwhile) |
| The core on the crystal while PLL A relocks | driving: [ours] | the core never runs on a PLL being reprogrammed: it waits on the crystal (Apple's start-up's own `+0x28 = 1`) |
| The speed check and the fallback | driving: [ours] | after a change, a loop's time against the crystal's (±12.5 %); any stall or wrong speed: the crystal, PLL A off, the level found. Stricter than Apple, never outside its envelope |
| The level bands | ours: policy | the clocks used: each band of the table, at its top or at a clock inside it; each verified on the unit (`vbv`) |
| The two layers | ours: policy | the band selector (up when the band's top saturates, down when the band below carries it at 80 %, 100 ms dwell) and the in-band policy (top of band by default, proportional the A/B), not Apple's 6 % step governor (it missed hundreds of frames in simulation) |
| Top of band with idle sleep | ours: policy | race to idle within a band: the band's voltage sets the energy per cycle |
| The clients' floors | ours: policy | the video floor (FEED p95, misses, a hold that doubles), a burst's top band, the race to the top on a seek, a resume or a wake |
| The decisions on the PP | ours: policy | the merged image has no room for a governor; the chip switches and meters |

- **The states gain a clock dimension.** C0 (lit, polling) and a video session now run at one of twelve level bands: the crystal (27 MHz) at its level, or 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150 MHz at Apple's levels 5–15 [ours on Apple's table]. Dark (C5, cut) is unchanged.
- **Who decides:** the chip switches (`HP_OP_VCLK`: a band and a clock in it; Apple's order, the speed checked, any failure on the crystal at the level found) and meters its busy time; **the PP decides** (`apps/video/vclk.c`), in two layers:
  1. **the band selector** picks the supply level among the verified bands of Apple's table: up one band when the load keeps saturating the band's top clock (95 %), down one band when the band below's top would carry it at 80 % or less, after 100 ms at the band; **a client's floor forces a minimum band** (the video session's from its FEED times and misses; a `DECODE` burst's top band for its run);
  2. **the in-band policy** picks the clock inside the band: **top-of-band** (the band's top clock, idle the rest: race to idle within the band; the default, the voltage-aware choice) or **proportional** (load / 0.8 inside the band), a switch for the A/B (`vplayer_vclk_policy()`, the bench row `vgov`; `vclk_gov_sim.py`).

  Apple's governor is the chip's [apple]; ours is the PP's because the merged image has no room for it [ours, measured]. **The PP's own lit-clock governor (`clockpol.c`) will take the same two-layer shape later.**
- **The gate:** a band is used only once the band-verify row (`vbv`) verified it on this unit (`bcmpassvc.txt`, naming the player's image); 150 MHz is stages 4b and 4c. Until a band is verified, the nearest verified band above serves. With only 150 MHz verified, a session runs as today.
- **Safety:** never above 150 MHz; never a level below Apple's for the clock running (a higher level before a rise, a lower one after a fall, 10 ms each; `vclk_test.py` checks it after every single write); any failed lock or settle returns to 27 MHz at the old level.
- **Expected** [sim, estimate]: video at the session's floor instead of 150 MHz: −30 to −59 % of the BCM's video current (−5 to −16 mA at the battery, polling as today); the shell unchanged.

**The decision rule** (no meter): keep a design or a default when (1) the simulation's energy proxy falls with no more misses, (2) the device's bench rows pass (performance and viability only: bands verify; the governor plays every clip with no skip, no dropped GOP, A/V within stage 6's bound, no more hitches than fixed 150, a stable clock, the top again after a seek and a resume) and its logged proxies agree (no misses; a lower time-weighted MHz and level than fixed 150), and (3) the long-run record (`ipodui.log`'s `batt session` lines, every build: start and end %/mV, lit, dark and playing minutes, the chip's time-weighted MHz, level and idle share); an in-band policy is chosen the same way, on the logged proxies (time per band, mean MHz, idle share, misses), the summary line naming the policy shows no shorter battery life per lit or playing hour over the following weeks. Power is never a bench pass condition.
