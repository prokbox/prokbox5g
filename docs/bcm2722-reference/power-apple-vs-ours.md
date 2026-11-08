# Power, awake time: Apple's BCM power manager against ours

*Research comparison, 2026-11-05 (branch `p19-vcpower-cmp`). Docs only: nothing here was built or run.*

**Scope.** Only what the BCM spends while the device is awake: the screen lit, or the unlit face, or a video playing. Sleep, hibernate, power-down, standby depths and bypass are out of scope; the rows for them in [`power.md`](power.md) and [`brief-map-power.md`](brief-map-power.md) stand as they are.

**Principle** (owner). Apple shows us HOW to drive the hardware (orders, settle times, tables, which clocks to gate). Our designs decide WHAT we do with it. Where Apple has a proven mechanism we lack, we adopt its method. Where ours is better, we keep ours and say why.

**Sources.** Apple's side is the manual's own record, [`brief-map-power.md`](brief-map-power.md) §2–§6 and the PW1 census (`vmcs.bin` and `osos` 1.2.1/1.2.3/1.3; addresses `@xxxx` are in 1.3). Ours is [`../research/bcm2722/battery-levers.md`](../research/bcm2722/battery-levers.md), [`power-contract.md`](../research/bcm2722/power-contract.md), `apps/video/vclk.{c,h}`, `vpcore.c` (`vclk_*`), `bcmfw/clock.c`, `bcmfw/main.c` (`idle_wait`) and `bcmfw/config.h` (`BCMFW_JOBS_CLK_OFF`). The private RE notes held nothing beyond this (one string list); no Apple code is copied. Tags as in the [evidence legend](index.md#evidence-tags), plus **[apple-RE]** (from Apple's code) and **[model]** (our power model, `scripts/bcm_power_model.py`, `k_bcm` 0.55 mW/MHz). **No current has ever been measured** (#12), so every saving below is a model figure and moves with `k_bcm`.

## What is [silicon] and what is not (as of this note)

| Item | State |
|---|---|
| Apple's running chip at 27 MHz UI, level 5 | [silicon] PW1 context A, 2026-10-25: core word `0x103` (PLL A at 81 MHz, divided by 3), regulator `0xB5`, constant over a 2 s idle plus a 1 s menu scroll; the other three PLLs off; clock words `+2C`–`+48`, `+50`, `+54` all 0 |
| Our image at 27 MHz, level 11, block clocks left on | [silicon] PW1 context B: `0xBB`; `+28`–`+48` read 1 (crystal ÷1; `+3C` `0x8001`); all PLLs off |
| Tick wake from `sleep` (1 ms timer compare 1) | [silicon, indirect] `slp`, sitting 1: 0 lost over two 60 s phases, from the image's own counters. The doorbell and other sources are not shown to fire |
| Level bands 74–150 MHz hold for a decoding session | [silicon] `vbv`, sitting 4c (twice). 59 and 46 MHz: not clean |
| Level 5 for our image (PW3, `lvl5`) | **not run** (owner chose Skip, sitting 1) |
| Idle sleep inside a video session | not run (the player clears the gate for a session) |
| Caches (PW2) | FAIL -3, open; cache stays off |
| Hibernate, bypass (PW5, PW6) | built, not run; out of scope here |
| Current in any state | unmeasured; no meter tests for a long time |

## The comparison

Saving is the model's, in mA at the battery while awake and lit (or playing), against what the build does today. The row designs follow the table.

| # | Mechanism | What Apple does | What we do | Gap | Awake saving (rough) | Risk | Proposal |
|---|---|---|---|---|---|---|---|
| 1 | Load-following clock (`pm_set_policy min`) | A governor over 5–150 MHz in 1 MHz steps; 10 ms windows; about +6 % when busy, down when idle; floor 5 MHz; clients (up to ten) hold a minimum, the TV path asks for full speed [apple-RE @fe00, @fa24, @5916] | PP-side two layers (`vclk.c`): twelve level bands (Apple's table), a client floor from the video session's FEED times, a decode burst's full speed, a 100 ms dwell, an EWMA of the load, top-of-band policy. The chip only switches and meters | None worth closing. In our simulation Apple's policy alone misses hundreds of pictures (a decode is a burst inside a 33 ms frame), and with floors still costs 0.1–0.4 mA more in the shell | 0 | none | **Keep ours.** Lowest modelled energy with 0 misses and 2–3 band moves a minute |
| 2 | Voltage follows clock | 13-row table, level 5 up to 46.9 MHz to 15 above 154 MHz; raise before a clock rise, lower after a fall; 10 ms wait after every regulator write [apple-RE @141ba, @141f0, @5134] | The same table and order in the bands (`clock_band`, with the 10 ms waits and a speed check); video bands 74–150 verified [silicon] | Complete in video. **Not at the crystal**: the lit shell sits at level 11, six steps above Apple's own idle point. Our `lvl5` write also skips the 10 ms wait (our own choice) | Level 5 at the crystal: **−1.7 mA** polling, **−0.66 mA** once idle sleep is in [model; assumes 0.8–1.2 V spread evenly over levels 5–15, hypothesis] | Low–medium. Apple's chip runs this exact point [silicon PW1 A], but our image has never run it. A core that fails shows as job errors or a hang; a cut cures it | **Adopt now as auto-fallback** (design L5 below) |
| 3 | Core `sleep` when idle | `ei; sleep; di` in the RTOS idle loop, 10 ms tick, no other gating on that path [apple-RE; static] | Built into every video and jobs image (`idle_wait`), 1 ms timer-compare tick plus the host doorbell, **gated** by the word `"SLP1"` at `0x16C`; default **off**; cleared for a video session | Not enabled. The tick is proven [silicon, indirect]; the doorbell is not. Ours ticks 100 times as often as Apple | Lit shell: **−2.6 mA** (4.23 to 1.63; `sleep_factor` 0.25, range 0.10–0.40) [model] | Low. Failure shows as a late job, caught by the tick and bad counters | **Adopt now as auto-fallback** (design S1 below) |
| 4 | Sleep inside a video session | Apple's decoder runs under the same RTOS idle, so the core sleeps between pictures [apple-RE; inferred, not seen] | The player clears the gate for a session ("a session polls as proven"), so the core busy-polls between FEEDs | The biggest awake gap. Between pictures the chip burns the full band's current | Clip 1 at the governor's band: 11.24 to 5.96 mA, **about −5.3**; clip 2: 16.98 to 11.14, **about −5.8** [sim, model] | Medium. A sleeping core adds up to 1 ms to each command (tick) and the doorbell is unproven. A frame has 3–8 ms of margin in the floor rule, so it should absorb it; unproven | **Adopt behind a bench row** (row VS) |
| 5 | Block clock gating at start | `vmcs` turns clocks 0–7 off, puts 8 and 9 on the crystal, zeroes the PLLs; a PLL is on only while some clock uses it (use count) [apple-RE @50a8, @4fae] | Merged and display-only images turn off ids 0, 1, 4, 5, 7 (`BCMFW_JOBS_CLK_OFF` `0xE3`, in the image batch). Ids 2, 3 (audio, PLL C) and 6 (USB gate) stay as found. PLLs: all off at the crystal [silicon PW1 B] | PW1 B (before the batch) read all of them on. Whether the current merged image has the five off, and what ids 2, 3, 6 cost, is unread | **0.05–0.3 mA** [guess; no model term: clock-tree power of five to eight idle blocks] | Low for 0, 1, 4, 5, 7 (no user found in Apple's code). Medium for `+3C` (a USB-side gate with an unknown reset meaning) | **Adopt behind a bench row** (row CK: census first, then the extras) |
| 6 | SDRAM idle when not in use | Before parking it flushes the cache, clears cache-control bits 30 and 16, writes the controller command register 0 and waits for bit 15 to clear (8,000 polls); re-initialises on wake. Used for hibernate [apple-RE @14892, @147ca] | SDRAM is "on demand only", but after a video it is left refreshing until the power cut (`memory-contract.md` §3). The drift needs no SDRAM | Refresh runs through lit UI after any video | **0.2–1 mA** [guess: the refresh of a 32 Mbit stacked part every 7.8 µs; self-refresh unknown (#7), unmeasured (#12)] | Medium. A job or the video image touching SDRAM after the idle reads garbage or may stall the bus; contents are lost [hypothesis] | **Adopt behind a bench row** (row SD) |
| 7 | UI clock | Apple's UI idle is 27 MHz, made by PLL A (81 MHz) divided by 3, at level 5 [silicon PW1 A]; the policy's 5 MHz floor is never seen in the menus | 27 MHz straight from the crystal, PLL off, at level 11 | Ours is **better on the clock** (no PLL held on for the same 27 MHz), worse on the level (row 2) | 0 | none | **Keep ours** |
| 8 | Sub-27 MHz clock | Possible in the policy (floor 5 MHz, PLL at VCO >= 75 MHz divided down) [apple-RE] | None; the lowest band is the crystal (a PLL band starts at 46 MHz) | Whether the crystal word's divider works on source 0 is unknown. Once the core sleeps, a slower clock saves only the awake fraction (a few percent) and slows every drift step | **<0.1 mA** after row 3 [model] | Low | **Keep ours** (revisit only if the sleep counters show the core mostly awake) |
| 9 | SDRAM refresh follows the clock | Interval = clock / 128,000 on every change [apple-RE @4d18] | The same; the refresh goes to the 27 MHz value before a change and to the new value after (safe at every clock) | None | 0 | none | **Keep ours** |
| 10 | Adaptive tick | A fixed 10 ms RTOS tick; both timers run while awake [apple-RE] | 1 ms compare tick for BCM-timed animation steps, always | A 10 ms tick when no BCM-timed animation is pending cuts 1,000 wakes a second to 100; needs the image | **0.2–0.5 mA** on top of row 3 [guess: moves `sleep_factor` from 0.25 towards 0.15] | Medium (image change: the merged image is frozen while a device verify is pending) | **Keep ours for now**; carry it in the next image batch (already under "Not taken" in `battery-levers.md`) |
| 11 | Caches (bit 31, SDRAM bit 16) | On from the start; flushed only after writing code [apple-RE @296] | Off (quirk #47: a host write into SDRAM may not be seen by the core) | Speed for SDRAM-heavy work could lower the video band; says nothing for the lit shell | unknown, video only | High (coherence) | **Keep ours** until PW2 answers (it failed -3 on silicon) |
| 12 | PM observability | `pm_get_status`/`pm_show_stats`: core, free, overrun [apple-RE @fcec] | `vclk band`/`vclk sum` lines (MHz, level, idle share, misses, time per band), plus the `batt session` line | None | 0 | none | **Keep ours** |
| 13 | Hibernate / power-down / bypass | `power_control videocore=N`, `GPO32` bit 15 [apple-RE] | Cut; PW5, PW6 built | Out of scope here | n/a | n/a | Out of scope |

## The top five, ranked by saving over risk

1. **Idle sleep in the lit shell** (row 3): −2.6 mA, tick proven, self-monitoring. Adopt now with an auto-fallback.
2. **Idle sleep in a video session** (row 4): about −5 to −6 mA, the largest figure, needs one watched row first (VS).
3. **Level 5 at the crystal** (row 2): −1.7 mA polling, −0.66 mA with sleep; Apple's own idle point. Adopt now with an auto-fallback.
4. **Block clocks off, census first** (row 5): 0.05–0.3 mA, mostly already in the batch; a read-only census makes it safe.
5. **SDRAM idle after video** (row 6): 0.2–1 mA, guessed, medium risk; own row (SD).

The savings of 1 and 3 overlap (the model's A+s row is −3.26 together, not −4.3).

## Auto-fallback designs (rows 2 and 3)

Both are PP-side only. Neither writes the merged image.

**S1: lit-shell idle sleep.** Today's `bcm_boot_res_live()` sets the gate word and reads the idle counters under the port lock. Turn it on at the swap and each wake. Every second, read ticks, ms, sleeps, bad and bells. **Fail** (clear the gate through the same call, latch off for this boot and, after three boots, in the unit's `bcmres.cfg`; one loud log line `chip idle: off why=...`): `bad > 0`; ticks outside 10 % of elapsed ms; sleeps = 0 while jobs are idle; a read that fails twice. The gate stays cleared for a video session until row VS passes.

**L5: level 5.** At the swap and each wake, only while the core word reads the crystal: write `0xA5A5_0000 | (old & 0xF0) | 5`, **wait 10 ms as Apple does** (our current code skips this), read back, run the jobs self-test, then watch the heartbeat for 2 s. **Fail** (any read-back, self-test or heartbeat miss): cut, restart at 11 through the normal ladder, latch `level` off in `bcmres.cfg`, log `chip level: 11 why=...`. Never below 5, never bits `[7:4]`, never with a session open. A video band move already writes the level around its clock changes, and `clock_slow` restores what it found, so a level found at 5 returns at 5.

## Bench row designs

Style of [`bench-p19.md`](../research/bcm2722/bench-p19.md): hands-off, automatic verdict, `bcmtest.log` lines, no meter. **None writes the merged image**: each uses the port to write registers, or the `0x16C` gate word (a RAM mailbox), as `slp` and `lvl5` do. Order in a sitting: CK, then SD (plain image), then VS.

### VS: idle sleep inside a video session (sibling of `vgov`)

- **Sets.** Plays clip 1 and clip 2, each 20 s, under the governor with the top-of-band policy, A (gate off, as today) and B (gate on, `"SLP1"` written after OPEN), A B A B. A seek at 5 s and a pause at 10 s, as `vgov`.
- **Logs.** Per phase: `vs <clip> A1|B1|A2|B2 mhz=.. lvl=.. idle=<permille> ticks=.. ms=.. sleeps=.. bad=.. bells=.. skipped=.. dropped=.. late=.. av_ms=..` and the usual `vclk sum`. Final `vs: PASS|FAIL <reason>`. `power.csv` marks `vs A1`…
- **Pass rule.** In every B phase: `bad` 0; ticks within 10 % of ms; sleeps > 0; skipped = dropped = 0 and no more late pictures than the matching A phase plus 1; A/V within stage 6's 50 ms; band moves <= 20 a minute; the top band again after the seek. B's idle share must exceed A's by at least 100 permille (else "no effect", not a fail). Bells are a reading (the doorbell may not fire).
- **Owner's time.** About 4 minutes hands-off, one question ("Did any play show a garbled picture?").
- **Restore.** The gate word cleared at each phase end and at CLOSE; any fail clears it and latches VS off for the boot. A hung chip: the heartbeat watchdog cuts it, the boot ladder restores.
- **May freeze the chip?** Yes, rarely (a sleep that is never woken). The tick re-arms itself and `slp` showed no loss, so a freeze would mean a mis-wired source; the cut cures it. The screen shows the last frame.
- **On PASS.** Gate on during sessions behind the S1 monitor, a trial week judged by the `batt session` lines.

### CK: block clocks (census, then the extras)

- **Sets.** Step 1 (read only): the PW1 context-B census on the running merged image: the nine clock words `+2C`–`+54`, the four PLLs, the regulator. Step 2: through the port, with the jobs image idle and the panel lit, write `pw 0` to `+0x34` and `+0x38` (ids 2, 3), read back, run the jobs self-test, hold 60 s with the drift running. Step 3, only if step 2 passed: the same for `+0x3C` (id 6). Each word is written back to its found value at the end of its step.
- **Logs.** `ck found +2C..+54 = ...` (decoded as source and divider, per `brief-map-power.md` §5); `ck step N wrote .. read .. selftest ok|fail panel ok|fail`; `ck: PASS|FAIL`.
- **Pass rule.** Step 1: every read completes (a census has no wrong answers); the log states which of ids 0, 1, 4, 5, 7 read 0 (the batch's off is live) and which do not. Steps 2 and 3: read-back matches, the self-test passes, the heartbeat holds, and the drift frame count over 60 s is within 5 % of an unwritten 60 s.
- **Owner's time.** About 2 minutes hands-off; one question ("Did the screen stay right?").
- **Restore.** Each word written back as found; the cut also resets all clock words.
- **May freeze the chip?** Unlikely for step 1 and ids 2, 3 (no user in the UI path); possible for id 6 (reset meaning unknown), hence last and conditional.

### SD: SDRAM idle after video (plain video image, a lab op)

- **Sets.** Plain video image only (the merged one answers "unknown op"). OPEN and CLOSE bring SDRAM up. The core writes a 4 KB pattern at `0xFE3F_6000`, flushes (cache is off), then does Apple's idle: command register `0x1000_3420` = 0 and wait for bit 15 to clear, bounded to 8,000 polls. Hold 1 s, then 10 s. Then the normal bring-up (the OPEN path) and read the pattern back.
- **Logs.** `sd idle polls=N bit15=clear|stuck hold=1|10 s`; `sd back bringup=ok|fail pattern=kept|lost words=..`; `sd heartbeat ok`; `sd: PASS|FAIL`. "kept" or "lost" is a result, not a condition (it answers #7: self-refresh or not).
- **Pass rule.** Bit 15 clears within the bound; the re-bring-up succeeds; after it, a fresh pattern written and read back is right; the heartbeat holds; the core and the port still answer.
- **Owner's time.** About 1 minute hands-off, no question.
- **Restore.** The bring-up (Apple's, already ours) reinstates the controller; a stuck bit or failed bring-up: cut and the normal wake.
- **May freeze the chip?** Yes (the controller may hang the bus if idled while a read is in flight); nothing else touches SDRAM in this op. The cut cures it.
- **On PASS.** A PP-initiated "SDRAM idle" at a video session's CLOSE, with the existing re-bring-up at the next OPEN. That is a later change; it needs an image op only if the command cannot be reached from the merged image, in which case it waits for the next image batch.

### L5 and S1 confirms

The existing `lvl5` and `slp` rows stay as optional confirms of the auto-fallbacks above (pass rules unchanged: level 5 reads back in both B phases and the self-test passes; ticks within 10 %, bad 0). Add one line to `lvl5` for the new 10 ms wait. They are not gates.

## Gotchas found while comparing

1. **Apple's menus do not run at its 5 MHz floor.** The policy has one; the NOR `vmcs` on this unit held 27 MHz (PLL A at 81 MHz, divided by 3) at level 5 through an idle and a menu scroll [silicon PW1 A]. The "5 to 150 MHz" in `brief-map-power.md` is the policy's range, not the UI's.
2. **Apple's 27 MHz holds a PLL on; ours doesn't.** Ours is cheaper at the same clock. Do not copy that part.
3. **Apple's governor alone does not hold video** (hundreds of misses in the sim); Apple's decoder presumably registers a client floor [hypothesis]. Do not port the policy; port the table, the order and the waits.
4. **Our `lvl5` write skips Apple's 10 ms settle.** Apple always waits after a regulator write; add the wait (once per swap or wake it costs nothing).
5. **"A session polls as proven" is a decision, not a measurement.** No test has ever run a session with the gate on.
6. **The idle tick is 100 times Apple's** (1 ms against 10 ms). It is needed only for BCM-timed animation steps; an adaptive tick is an image change.
7. **The PW1 census failed its pass rule** (a 992 µs counter, one differing register), though its reads are valid. A new census row must not reuse that counter rule.
8. **Merged-image freeze.** Rows CK and SD are written so that no merged-image byte changes. Anything that does (the 10 ms tick, an SDRAM op in the merged image) waits for the next batch and its single re-verify.
9. **Dark states are not part of this.** The unlit face window (about 90 s with the backlight off, panel on) is awake for the BCM: rows 2 and 3 apply to it directly.

## Built (branch `p19-vcpwr`, 2026-11-05; not run)

The owner's decision (2026-11-05): all five in tonight; the battery push supersedes the old "no `slp`/`lvl5` labs" rule for awake-time idle states. S1 and L5 are auto-fallbacks in developer builds and **not the default in daily builds until their rows pass** on the unit: a passing row saves `idle on` / `level 5` in `bcmres.cfg`, the switch every resident build already honours. Rows and pass rules: `../research/bcm2722/bench-p19.md` "The VC power rows". Where the build departs from the designs above:

- **L5's settle is in every build** (`bcmfw_res_level`): the daily path already writes level 5 when the switch is on, so Apple's 10 ms wait goes where the write is (+32 B of text in the shipping shape, `_end` unchanged). The self-test and the 2 s heartbeat run in the developer watch after each swap and wake (UI thread, the job owner), not in the wake path (no job calls there).
- **S1's latch:** off for the boot at once; saved off at the third failing start (`bcmvcpwr.txt` counts them). L5 is saved off at its first failure, as designed.
- **The gate in a session stays cleared** outside row `vs`; a `vs` PASS is the evidence for leaving it on behind the watch (a later change).
- **CK's holds are 40 s** (the baseline and each step), its "drift frame count" is a FENCE count every 100 ms against the unwritten baseline (the bench screen has no drift running).
- **SD runs on the merged image through the port**, not as a plain-image op: Apple's idle is a register write and a poll. Its restore is the cut, because the image keeps its own "SDRAM ready" state (`quirks.md` #55); so "kept or lost" (#7) stays unanswered and the after-check is a second play.

**Needs an image change** (none made; for the next batch behind the `vck`/`jvpu` re-verify):
1. An SDRAM idle/wake pair in the merged image (Apple's @14892 idle, @147ca bring-up with the probe words kept, as PW5's plain-image `video_sdram_idle`/`video_sdram_rewake`), or `CLOSE` clearing the image's SDRAM-ready state after a PP-side idle: so the restore needs no cut, `sd` can answer kept-or-lost, and a CLOSE-time idle can go into daily use.
2. The adaptive 10 ms idle tick (row 10), unchanged from the comparison.
