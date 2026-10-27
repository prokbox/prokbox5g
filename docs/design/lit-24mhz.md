# 18-5: 24 MHz with the screen lit (#113, R10)

Design for Phase 18 item 18-5 (`docs/phase18-brief.md` §4.6). Design only: no code exists for anything below. Owner: agent A (firmware-platform), sonnet, per brief §5.

**In one paragraph.** Today a lit screen holds the PLL, so the unboosted CPU runs at 30 MHz whenever the backlight is on. The hold was added because the CPU's copy to the BCM host port is about 20 % slower at 24 MHz. Since #75/#76, full-width pushes of 16 rows or more go by the second DMA engine (E2), so most lit pushes no longer depend on the CPU clock. The design keeps the mechanism in `firmware/pll_idle.c` and moves the *decision* to the shell, per screen state: at 24 MHz the PLL is held only while a sustained CPU-push stream runs (the main-menu drift), with a 2 s release delay, and never for sporadic small CPU pushes. Animations still boost to 80 MHz. A developer switch, "Lit screen: 24 / 30 MHz", selects the policy. **The expected saving is about 1–3 mA, and the no-meter A/B cannot resolve it (its floor is about 8–20 mA), so by the brief's rule the default stays at 30 MHz.** The code still lands, because it costs under 1 KB, makes sleep entry release the PLL immediately, and readies the measurement for the deferred meter.

**Evidence tags.** [silicon] means measured on our unit (the log or session is named). [estimate] means computed from stated figures. [hypothesis] means unproven. [code] means read from the source at the commit named; it is a fact about our firmware, not a hardware claim. Commits: `p18` `fa5736ddf4`; 18-A `p18-a` `7b8e2a5c09`; 18-1 `p18-1` `94bf63e836`; 18-2 `p18-2` `6adf3538ab`.

---

## 1. Today's rule

### 1.1 Every PLL holder

The unboosted frequency is chosen in `firmware/system.c` :56 `unboosted_freq()`. It returns `CPUFREQ_DEFAULT` (24 MHz, PLL off) when no hold bit is set, and `CPUFREQ_NORMAL` (30 MHz from the PLL) otherwise. Upstream's `cpu_idle_mode(true)` still forces 24 MHz, except over ATA or USB holds [code]. `cpu_pll_hold()` (:81) changes the mask under the boost lock and switches the clock at once if no boost is active (:89–94) [code]. The bits are defined in `firmware/export/system.h` :69–75 [code].

| Bit | Set and cleared at | Meaning [code] |
|---|---|---|
| `CPU_PLL_HOLD_DEV` | `apps/ipodui/clockpol.c` :250 (`clockpol_set_enabled()`); initial value `firmware/system.c` :47–48 | The policy is off (the developer row "24 MHz idle: off", and until the shell starts) |
| `CPU_PLL_HOLD_LIT` | `firmware/pll_idle.c` :49–50 (`is_backlight_on(true)`), applied at :57–59; sampled from the power thread step, `firmware/powermgmt.c` :736. On `p18-a`: `pll_idle.c` :59/:68–69, `powermgmt.c` :813 (step) and :909 (kick), and `firmware/backlight.c` :575–580 kicks the power thread on dark→lit | **The backlight is on.** This is the holder 18-5 changes |
| `CPU_PLL_HOLD_LOAD` | `pll_idle.c` :51–52 when `pcm_is_playing()`; the need comes from `clockpol.c` :129 `tell()` → `pll_idle_set_load()` | The playing track does not fit in 24 MHz (codec class, rate, speed, software DSP; per-format strikes) |
| `CPU_PLL_HOLD_ATA` | `firmware/target/arm/pp/ata-pp5020.c` :292 / :364 | One ATA DMA transfer (UDMA2 is rated at 30 MHz). Taken before any UDMA4 boost, so the boost starts from 30 MHz (S36) |
| `CPU_PLL_HOLD_USB` | `firmware/target/arm/pp/usb-fw-pp502x.c` :180 / :193 | The USB controller is enabled |
| (upstream) `cpu_idle_mode()` | `firmware/usb.c` :376 / :382 | Forces 24 MHz in USB slave mode, but ATA and USB holds win |

18-1's `power_sleep()` refuses to sleep while any bit is set or the clock is not `CPUFREQ_DEFAULT` (`p18-1` `power_sleep-pp502x.c` :182–185, `SLEEP_REFUSE_BOOST`/`SLEEP_REFUSE_CLOCK`) [code].

### 1.2 Why the screen-lit hold exists

The shell draws and pushes over the BCM host port. The CPU's copy costs about 10.5 PP cycles per 16-bit transfer at any clock: a full 153,600 B frame took 26,957 µs at 30 MHz and 9,629 µs at 80 MHz (S8) [silicon]. At 24 MHz that rate gives about 33.7 ms per full frame [estimate]. Fit-1 C6 held the PLL while lit so that unboosted pushes kept their speed (`clocks-power.md` §8; `fit1-brief.md` §C) [code]. The rule predates E2 (`hardware-fit-audit.md` finding 7) [code].

### 1.3 What runs at 30 MHz and what runs at 24 MHz today

| State | Unboosted clock today | Why |
|---|---|---|
| Dark, nothing playing or playing a "light" format (MP3, FLAC, ALAC, WavPack, PCM), no disk transfer, no USB | **24 MHz** | No holder. Runs daily [silicon] |
| **Lit, any screen** | **30 MHz** | `LIT` |
| Playing a format that does not fit (AAC, Vorbis, Opus, WMA, APE...), or one struck this session | 30 MHz, lit or dark | `LOAD` |
| During each ATA DMA transfer | 30 MHz for the transfer | `ATA` |
| USB connected to a host | 30 MHz | `USB` |
| Developer "24 MHz idle: off" | 30 MHz always | `DEV` |
| Any boost (animations, art builder, maintenance windows, database, wake) | 80 MHz | `cpu_boost()` wins over the mask |

**PLL lock time is measured now, not just logged** [silicon, `session log, backup 20261017 ipodui.log*`]: `clk lock` lines show 16–29 µs for the last wait, a maximum of 95–217 µs, over 16,641 to 290,217 lock waits in single sessions. The PLL already powers up about 30 times a second during a library scan (ATA holds), with no fault logged. A lock bit that reads "locked" too early is still possible (`clocks-power.md` §8) [hypothesis]; the numbers are not near 0 µs, which was the warning sign named there.

---

## 2. Push inventory

### 2.1 Which path a push takes

`lcd_update_rect()` (`firmware/target/arm/ipod/video/lcd-video.c` :706) sends a push by E2 only if it is **full-width** (`width == LCD_WIDTH`), at least `LCD_E2_MIN` = 320 × 2 × 16 = 10,240 B (16 rows, :557), from thread context with IRQs on, and the engine is free and enabled (:589). Everything else is the CPU's copy: a full-width push under 16 rows is one contiguous run (:776), and a **narrower rectangle is one address write plus one run per row** (:782–790) [code].

**E2 is not always "CPU asleep"** [code, :605–620]. Pieces are up to 64 KB. Unboosted, a piece of 32 KB or more sleeps through one tick (`sleep(1)`), so the core idles. A smaller piece loops on `yield()`, and with nothing else runnable the core **spins** until the engine finishes. So most E2 *bands* (11–28 KB) keep the CPU awake for their transfer time; what changes is that the time is set by the engine, not by CPU cycles.

### 2.2 Rates used below

| Path | Rate | Tag |
|---|---|---|
| CPU copy | 10.5 cycles per halfword: 0.350 µs at 30 MHz, 0.4375 µs at 24 MHz | [silicon S8] → [estimate] at 24 |
| CPU per-row address (narrow rects) | ~7.4 µs a row (S8, clock not recorded); ~9 µs at 24 MHz if it scales | [silicon] / [estimate] |
| E2 | 9.3–10.5 MB/s typical in daily use (`f3 lcde2` lines: 61,681 KB in 6,030 ms; 29,906 KB in 3,287 ms), 11 MB/s in the S37 lab | [silicon], clock not separated |
| E2 at 24 MHz | ~20 % slower if the engine and port run on the system clock that `CLOCK_SOURCE` also switches | [hypothesis]; the `f3 lcde2` lines at each switch setting settle it (§5.2) |

### 2.3 Inventory: unboosted lit pushes (these feel the lit clock)

Push time = CPU-busy time for CPU pushes, and engine time for E2 (the CPU spins for pieces under 32 KB). Figures are [estimate] from §2.2 unless tagged.

| Push (`kind`, file:line) | Size | Path | How often | Time at 30 MHz | Time at 24 MHz |
|---|---|---|---|---|---|
| **Drift pan step** (`drift`, `preview.c` :1368 via `push_panel()` :222) | 160 × 240 = 76,800 B, 240 rows | **CPU, per row** | **13.3/s** (75 ms half-pixel steps, `nav_core.h` :123–124), whenever a split screen (main menu, Music, ...) is lit and at rest | 15.2 ms (13.4 + 1.8 row setup) → **~20 % of the CPU** | 19.0 ms → **~25 % of the CPU** |
| Drift cover fade step (`drift`, `preview.c` :1275) | 76,800 B | CPU, per row | 8 steps 50 ms apart per cover, one cover per 7 s cycle | 15.2 ms | 19.0 ms |
| Split-screen header (`header`, `ui_main.c` :332, width 160 via `ui_header_push_rect()`) | 160 × 20 = 6,400 B | CPU, per row | On change: Time in Title (title until 2.5 s idle, then once a minute, `header.c` :502), play state, battery step | 1.3 ms | 1.6 ms |
| Header busy glyph (`header.c` :443) | `BUSY_W` × 20, under 1 KB | CPU | Each step of the post-USB scan glyph (#84) | < 0.1 ms | < 0.1 ms |
| Library footer (`libfoot`, `menu.c` :529) | 160 × 18 = 5,760 B | CPU, per row | On a count change (rare) | 1.1 ms | 1.4 ms |
| Row art fill (`art`, `list.c` :846) | 55 × 55 = 6,050 B | CPU, per row | Up to 4 per album view, after a glide ends | 1.5 ms | 1.8 ms |
| Now Playing stars (`np:stars`, `nowplaying.c` :2535) | 320 × 12 = 7,680 B | CPU, one run (under 16 rows) | On a rating change | 1.3 ms | 1.7 ms |
| Full-width header (`header`, `ui_main.c` :332) | 320 × 20 = 12,800 B | E2 (spin) | On change, as above | ~1.3 ms | ~1.3–1.6 ms |
| Selection move without glide (`band`, `ui_main.c` :340) | ≤ 2 rows, ≤ 70,400 B | E2 | Per click at rest | ≤ 7 ms | ≤ 7–8.5 ms |
| List marquee (`marquee`, `list.c` :980/:1085) | 320 × 24/25/44/55 rows = 15,360–35,200 B | E2 (spin, or a tick's sleep at 55 rows) | **20/s** while a long selected label scrolls (`MQ_STEP_TICKS` 5) | 1.5–3.4 ms | same to +20 % |
| Now Playing title marquee (`np:title`, `nowplaying.c` :1837) | 320 × 18 = 11,520 B | E2 (spin) | **20/s** while a long title scrolls | ~1.1 ms | ~1.1–1.4 ms |
| Now Playing bar and times (`np:row`, `nowplaying.c` :1910/:2546) | 320 × 34 = 21,760 B | E2 (spin) | 1/s while playing | ~2.1 ms | ~2.1–2.6 ms |
| NP glyphs, album/chapter, counter (`np:glyphs`, `np:chapter`, `np:counter`) | 320 × 18 = 11,520 B | E2 (spin) | Per track or chapter | ~1.1 ms | ~1.1–1.4 ms |
| Whole screen (`lcd_update()`, `ui_main.c` :321; unlit face) | 153,600 B | E2, 3 pieces, 2 tick sleeps | Boot, a return without a strip, the unlit face (dark) | ~15 ms of engine time | ~15–18 ms |

**Per screen, at rest and lit** [estimate]:
- **Main menu and other split screens:** the drift dominates, with 13.3 CPU pushes a second, about 200 ms/s at 30 MHz and 250 ms/s at 24 MHz, plus its compose (blit or half-pixel blend, plus the split shadow: ~33k instructions per shadowed image, `hardware.md` table). This is the only **sustained** CPU-push stream in the shell.
- **Plain lists (no panel):** only E2 bands, plus a marquee at 20/s on a long label (~30–70 ms/s of engine time).
- **Now Playing:** E2 only (title marquee ~22 ms/s, bar ~2 ms/s), stars by CPU when rated.
- **Sporadic CPU pushes** (header on split screens, footer, row art, stars): 1–2 ms each. At 24 MHz each costs 0.3–0.4 ms more, far below a 33 ms frame.

### 2.4 Boosted pushes (unaffected by the lit clock)

These run at 80 MHz under `ui_anim_begin()` (`anim.c` :68–75) or another boost [code]: list glides (`anim:scroll`, `list.c` :802, E2), push/pop slides (`anim:slide`, `ui_main.c` :708–728), panel slides and highlight cross-fades (`anim:pslide`, `anim:xfade`, `preview.c` :1718/:1756, 160 wide so CPU at 80 MHz), the Now Playing slide (`anim:np`, `nowplaying.c` :2064), the USB screen (USB holds the PLL anyway). At 24 MHz, the only change is that the first boost from 24 MHz waits for the PLL's power-up and lock: 16–217 µs [silicon], against a 33 ms frame.

---

## 3. Design

### 3.1 The decision is per screen state, in the shell

**Rejected: a hold per push.** Taking `LIT` in `lcd_update_rect()` for each CPU push would mean an upstream-file edit in the LCD driver and a PLL power cycle 13 times a second on the main menu. Each cycle is two core stalls plus a 16–217 µs lock [silicon], plus an energy cost for the PLL's start that is unknown [hypothesis]. All that to save 0.3–0.4 ms per sporadic push, or 3.8 ms per drift step. A short hold buys nothing for small pushes, and per-step holds on the drift are a continuous hold with extra switching.

**Chosen: a lit need, per screen state.** The shell knows when a sustained CPU-push stream runs. Today that is only the drift: a split screen visible, lit and at rest, with its pan or fade schedule running. The rule:

```
lit_needs_pll = switch_30
             || (stream_active && CP_LIT_STREAM_HOLD)
             || (lit_strike && playing)
```

- **E2 pushes and sporadic small CPU pushes take no hold.** They run at whatever the clock is.
- **A sustained CPU stream** (the drift) keeps the hold while `CP_LIT_STREAM_HOLD` is 1. That constant is set by the device frame-budget round (§5.2): if the drift fits its budget at 24 MHz, it becomes 0 and the main menu runs at 24 MHz too.
- **Composing stays boosted as today.** Nothing in this design changes a `cpu_boost()` call.
- **`lit_strike`** is the audio guard (§4.1).

**Hysteresis.** The need is taken at once on a rising edge. It is released only after it has been false for `CP_LIT_RELEASE` = 2 s. The drift's own pauses (a set's hold, `HOLD_CAP` 2 s, `preview.c` :79) and a boosted highlight slide therefore never release it. The worst case is one power-up per 2 s; in practice it is one per screen change. Time cost: under 0.01 % of the CPU [estimate, from the lock times]. The ATA holder already cycles the PLL about 30 times a second during scans without harm [silicon].

### 3.2 Where each part lives

| Part | File | Detail |
|---|---|---|
| Mechanism (unchanged in kind) | `firmware/pll_idle.c`, `firmware/export/pll_idle.h` (ProkBox files, `HAVE_PLL_IDLE` only) | New `pll_idle_set_lit(bool needs_pll)`, the twin of `pll_idle_set_load()`: it stores the need and kicks the power thread on a change (`power_thread_kick()`, from 18-A). `pll_idle_poll()` takes `LIT` when `lit_needs_pll && is_backlight_on(true)`. Initial need: **true**, which is today's behaviour until the shell says otherwise |
| Policy (pure, host-tested) | `apps/ipodui/clockpol_core.{c,h}` | `cp_lit_need()` (the rule above) and `cp_lit_step()` (the 2 s release), no kernel calls |
| Policy (glue) | `apps/ipodui/clockpol.c` | Evaluated in `clockpol_service()` (UI thread, every main-loop pass, as today); writes `pll_idle_set_lit()` only on a change; a `clk lit need=N why=stream\|switch\|strike\|e2 n=` line on each change and a toggle count in the 5-min `clk` line |
| Stream signal | `apps/ipodui/preview.{c,h}` | A read-only getter `ui_panel_drift_active()` from the existing scheduler state (no new state) |
| Dark step | `clockpol_dark_now()` | For 18-2's sleep entry: clears the need and kicks, so `LIT` drops before `power_sleep()` instead of at the next quiet power step (up to 2 s; the Sleep lab polls up to 8 s for it today, `p18-2` `sleeplab.c` :139–141) |
| Developer switch | `apps/ipodui/devmode.c` | "Lit screen: 30 MHz" / "Lit screen: 24 MHz" (below) |

**Developer switch.** One row beside "24 MHz idle", developer mode only, not saved (each boot starts at the default, like the neighbouring rows). Strings as `LANG_*` appended to `apps/lang/english.lang` (brief Revision 4 N2; owner question Q-L3). Each toggle writes `powerlog_mark("lit 24")` or `powerlog_mark("lit 30")` and a `clk` line, so the device round needs no hand-noted times. "24 MHz idle: off" (the `DEV` bit) still overrides everything.

**Default: 30 MHz** (switch_30 = true). See §5.3 and §6.

**Optional developer arm B (off by default): drift pushed full-width.** Pushing rows 0–239 at full width (153,600 B, E2) instead of the 160-wide panel (76,800 B, CPU) lets the drift run at 24 MHz with the core idle in two tick sleeps of each step. It doubles the bytes on the port, about 20 % engine duty. Whether that saves energy against the CPU's copy can only be answered by a meter [hypothesis]. It also relies on the left half of the framebuffer always matching the screen at a drift step (§8, R8). It is a one-line change to `push_panel()`, behind a developer marker, for the meter session only.

### 3.3 Not in 18-5

- **`lcd_e2_push()` spinning on small pieces** (§2.1). Waiting on the engine's completion (IRQ 27, unused today, `data-paths.md` cell 14) would let the core sleep through every band at any clock. That is an `lcd-video.c` change for its owner (#60/A), noted as a follow-up.
- **The drift itself** is the largest lit CPU consumer (20–25 % of the CPU on every split screen). A BCM-side pan (our image scrolls a cover it already holds) would remove it. That is a BCM-session question (#60), flagged as a ProkBox impact.

---

## 4. What else used this?

Everything that relied on "lit ⇒ at least 30 MHz unboosted":

| Dependent | Effect at 24 MHz lit | Handling | Tag |
|---|---|---|---|
| **Audio decode headroom while browsing** | Heavy formats are unaffected: `LOAD` still holds 30 MHz lit or dark. Light formats now decode at 24 MHz *while the UI also works unboosted*, a combination never run before (lit was always 30 MHz). The codec has priority 13, above the UI's 16 (`clocks-power.md` §4), so the UI stalls before audio does. But a shortfall shows up as pcmbuf rescues, which boost to 80 MHz and cost more than 30 MHz would | `judge()` (`clockpol.c`) closes its window on a lit edge and tags it lit or dark. A strike in a **lit** window sets `lit_strike` for the session: lit-and-playing goes back to 30 MHz, and the format is **not** struck for dark playback. Logged `clk lit-strike fmt=`. Device check: rescues = 0 in every §5.2 run | [hypothesis] until §5.2 |
| **Clickwheel and scroll response** | The wheel's serial and I2C are not reconfigured across clock changes and work at 24 MHz in the bootloader and dark today [silicon, daily]. Glides and slides boost; the first frame waits 16–217 µs more for the lock [silicon]. Static selection moves are E2 bands (§2.3). Piezo/haptic click timing uses the fixed 1 MHz timers | None needed; the §5.2 first-frame check | [silicon] / [estimate] |
| **USB** | `USB` holds the PLL while the controller is enabled, lit or not; the USB screen is unchanged | None | [code] |
| **P16 art builder and maintenance windows** | They boost (`artcache.c` :323, `maint.c` :211, `bootstamp.c` :286). Between boosts they now fall to 24 MHz instead of 30 when lit, so each boost adds a PLL power-up (16–217 µs). Their unboosted parts run 20 % slower; they work mostly while the user is idle (dark), so the effect is small. The clock policy judges codec rescues, not boosts from idle (S36 fix), so this raises the `bfi` count without causing false strikes | None; watch `bfi` and `art ... noboost=` in the §5.2 logs | [code] / [estimate] |
| **The BCM's own clock** | The BCM runs from its own clock (27 MHz, then 150 MHz under our image) [silicon S1, S2]. Only the PP side of the port slows, by about 20 % for CPU copies [estimate]. Our host-protocol bounds (20 ms write-ready, 2 s "update busy" fallback) have a wide margin | None | [silicon] / [estimate] |
| **Wake (backlight or soft-sleep)** | The wake composes boosted, and the resident image upload (15–34 ms unboosted) moves to E2 in 18-3 | None; a D4 run with each switch setting (§5.2 step 9) | [code] / [estimate] |
| **P18 sleep (18-1/18-2)** | `power_sleep()` refuses any hold and any clock other than `CPUFREQ_DEFAULT`. Today `LIT` drops only at the power thread's next step after dark (up to 2 s quiet). With 18-5, `clockpol_dark_now()` clears the need and kicks. 18-1 is unchanged | 18-2's entry calls `clockpol_dark_now()` at its dark step (a one-line call, agent C) | [code] |
| **18-A's power-thread kick on dark→lit** (`p18-a` `backlight.c` :575–580) | Still needed: it makes the power thread sample `LIT` within milliseconds of lighting. With 24 MHz chosen, the sample may now find no need (no hold). `pll_idle_set_lit()` adds a kick on a need change, which is at most one per 2 s through the hysteresis and needs no wake-budget row change (the main thread's lit rate is already 20/s while a panel drifts) | **18-5 merges after `p18-a`.** Without the kick, a need change would wait up to 2 s | [code] |
| **HDD variant, 32 MB units, 5G (PP5021C)** | HDD: `ATA` unchanged, and PIO already runs at 24 MHz. 32 MB: no difference. 5G: 24 MHz while lit is as unverified as 24 MHz dark already is (`fit1-brief.md` §C) | None; not a storage choice (`storage-choices.md` unchanged) | [code] |

---

## 5. Verification

### 5.1 Host and simulator (no PLL in the simulator: `HAVE_PLL_IDLE` is target-only)

1. **Host test, `tests/clockpol`:** rows for `cp_lit_need()` (switch × stream × `CP_LIT_STREAM_HOLD` × strike × playing) and `cp_lit_step()`: the need is taken at once, released at exactly 2 s, a 1.9 s gap never releases, and two edges 0.5 s apart give one take; `lit_strike` is set only by a strike in a lit window, and a dark strike still strikes the format.
2. **Simulator script `tests/ui/ipodui-p18-5-lit-clock.script`:** boot to the main menu, wait for the drift, then Music › Songs, Now Playing, back, the backlight off and on, and the switch both ways. A sim-only trace line `clk lit need=N why=` on each need change; rule `litclk` in `scripts/check_trace.py` (tooling): need = 1 only while `panel drift step=` lines flow (with the hold constant at 1), at most one change per screen change, no release sooner than 2 s, and the switch at 30 forces 1.
3. **Push-cost model in `check_trace.py` (tooling, report only):** each `update` line is classified by the §2.1 rule (full-width and ≥ 16 rows → E2, otherwise CPU), costed with the §2.2 rates at 24 and 30 MHz, and summed per second per script. Run over `ipodui-p8b-pan-cost`, `-panel-slide`, `-drift-swap`, `-fastspin`, `-list-edge`, `ipodui-p3-scroll`, `ipodui-p5-titlescroll` and `ipodui-p8b-np-tick`. This checks §2.3 against what the shell really pushes before any device time is spent.
4. Unchanged and still green: `ipodui-p8b-pan-cost` (`p8bpan`), the boost-balance rule in every animation script.

### 5.2 Device: frame budgets at 24 MHz (the round that decides `CP_LIT_STREAM_HOLD`)

**The build.** A developer build with `-DUI_PTRACE`, plus a small summary so the trace reaches `ipodui.log`. `UPDATE_TRACE` goes to `logf`, which target builds do not keep. Per trace kind, it keeps n, sum, max and over-budget counts in RAM (under 200 B, no statics over 1 KB) and logs one `pt kind= n= mean_us= max_us= over= jumps=` line a minute. Same commit as the tooling (§7).

**Pass/fail thresholds** (each measured at "Lit screen: 30" and "24", same build, same playlist; compose + push from the existing `panel cost` and `anim frame` lines):

| Animation (script it mirrors) | Clock during it | Pass at 24 MHz |
|---|---|---|
| Drift pan (`p8b-pan-cost`), main menu lit, 3 min, a light-format track playing | 24 (if `CP_LIT_STREAM_HOLD` = 0) | Compose + push: mean ≤ 45 ms and max ≤ 70 ms of the 75 ms step; **0 skipped pixels** (`jumps`); gap p95 ≤ 85 ms |
| Drift cover fade | 24 | Each step ≤ 35 ms of its 50 ms; 8 steps delivered per cover |
| Drift set swap (`p8b-drift-swap`) | 80 (boosted) then 24 | No more than 1 frame later than at 30 |
| Panel slide and highlight cross-fade (`p8b-panel-slide`), 20 moves | 80 | Key → first frame no more than 1 ms later than at 30; mean frame ≤ 34 ms; no frame over 50 ms after the first |
| List glide (`p3-scroll`, `p8b-fastspin`), slow and fast spin, 10 s each | 80 | Same as the panel slide; `anim start`/`anim end` balanced |
| Push/pop slide, 10 times | 80 | Same |
| List and NP title marquee (`p5-titlescroll`), 1 min | 24 | Each step ≤ 25 ms of its 50 ms; no step late by more than 1 tick on average |
| NP bar (`p8b-np-tick`), 1 min | 24 | ≤ 20 ms a second |
| Every run, playing a light format | | `pcmbuf_rescues` and underruns = 0; no `clk strike` or `lit-strike` |
| Backlight wake, 10 times | boosted | `lcd_awake()` to the first frame no more than 5 ms later than at 30 |

**Outcome rules.** All pass → `CP_LIT_STREAM_HOLD` = 0 (24 MHz everywhere lit when the switch says 24). Only the drift rows fail → keep it at 1 (the drift holds 30 MHz; everything else runs at 24). Any boosted row fails, or rescues appear → 18-5 is a no-go at 24 MHz for that state, and the cause is logged.

**Side measurements from the same logs, free:** `f3 lcd` and `f3 lcde2` lines (MB/s at each setting) settle whether the CPU copy is 20 % slower at 24 MHz [estimate → silicon] and whether **E2 also slows** [hypothesis → silicon]; and `clk lock` gives more lock times.

### 5.3 Device: the no-meter A/B, and why it cannot decide

**Method (brief §4.6).** Paired, equal-duty lit runs, each at least 2 h: backlight always on, a fixed menu-and-playlist duty, power log every 5 s, alternated ABBA. The voltage drop of each run is read through the measured table (p16 `4b8163e9e6`: 2.80 h per 10 % at playback load, about 105 mA if the cell holds 3,000 mAh; 10 % ≈ 294 mAh).

**Its resolution** [estimate, the assumptions stated]:
- **Lit load:** about 150 mA (range 130–190): the 105 mA screen-off playback figure plus backlight, BCM and panel. A 2 h run then uses about 300 mAh, about 10 %, so it fits in one band of the table.
- **Table slope** (steps 80, 65, 40, 30, 40, 75, 115, 90, 90, 125 mV per 10 % from 0 % to 100 %): 2.4–3.3 mAh per mV in the steep upper bands (60–100 %), 7.4–9.8 mAh per mV in the flat middle (20–50 %).
- **Endpoint noise:** the ADC step is 5.9 mV. Readings averaged by powermgmt in the same state give about ±3 mV per endpoint, so about ±4 mV per run drop and about ±6 mV for an A−B difference. Converting through the table adds about ±5–10 mAh after ABBA ordering, because consecutive runs sit in different bands.
- **One pair** in the steep band: σ ≈ 15–22 mAh over 2 h, so σ ≈ 7.5–11 mA. **The smallest difference it can show (2σ) is about 15–22 mA** (about 45–65 mA in the flat band).
- **Four ABBA pairs** (16 h of lit runs, about 2 full charges in the steep band alone): σ ≈ 4–5.5 mA, a floor of **about 8–11 mA**.
- **Expected saving: 1–3 mA** (§6). That is 3–20 times below the floor. Showing 2 mA at 2σ would take about 80 pairs, around 320 h of lit runs.

**Plainly: the no-meter A/B cannot resolve this saving.** Under brief §4.6, **the default stays at 30 MHz**, and the question is listed for the deferred meter. With a meter, a charger-current difference at full battery with the switch at 24 and at 30, 2 min each, three times, should resolve about 1–2 mA [estimate]. The A/B voltage runs are therefore **not** scheduled. What the power log can still show cheaply is a **regression**: `boost_pct` and `idle_pct` from the §5.2 runs reveal a policy that rescues or boosts more at 24 MHz, without needing any current reading.

### 5.4 Device steps to add to `docs/device-session-p18-0.md` (a later round, "Round P18-5", after 18-5 merges)

*About 50 minutes attended, one session. Needs the owner's go; the agent backs up first (`deploy-ipod` skill).*

1. **Pre-flight (agent, with your go):** back up, check the backup, install the 18-5 `UI_PTRACE` developer build. Battery above 60 %, charger unplugged.
2. **Settings for the round:** Developer › Power log: on, every 5 s. Settings › Backlight: Always On, for this round only (the agent restores your value at the end). Start the fixed light-format playlist (MP3) at your usual volume.
3. **Developer › Lit screen: 30 MHz** (it marks the log). Then, in order:
   1. Main menu, hands off, **3 min** (drift and fades).
   2. Scroll the main-menu highlight up and down slowly, **20 moves** (panel slides).
   3. Music › Songs: slow spin **10 s**, fast spin **10 s**, then 10 enter/back pairs.
   4. Select a song with a long title, open Now Playing, **1 min**.
   5. Back to the main menu; switch Music ↔ Audiobooks **5 times** (drift swap).
   6. Settings › Backlight: 5 seconds. **10 times**: wait for the screen to go dark (about 10 s), then press MENU once (backlight wakes). Then set Backlight back to Always On.
4. **Developer › Lit screen: 24 MHz**, and repeat steps 3.1–3.6 exactly.
5. **Developer › Lit screen: 30 MHz**, and repeat steps 3.1–3.2 only (the A in ABA, to catch drift over time).
6. **Close:** restore the backlight setting; power log off; the agent pulls `ipodui.log*` and `power.csv` and checks the §5.2 thresholds (`pt`, `panel cost`, `anim`, `f3`, `clk` lines).
7. **Watch with your eyes during step 4:** any drift stutter, a slide that feels late, an audio glitch. Note the time; the log does the rest.

No voltage A/B runs (§5.3). If a meter session (S-M1) ever happens, add one row: "Lit screen 24 vs 30, main menu, full battery, 2 min each, three times".

---

## 6. Estimated saving

**Assumptions** [estimate]:
- A1: the cell holds 3,000 mAh (unverified; 2,800 mAh would mean 98 mA playback).
- A2: the lit load is about 150 mA (range 130–190).
- A3: core voltage fixed (DCDC1 1.2 V), so energy per instruction is about constant and work done at 24 MHz costs the same as at 30. What 24 MHz saves is the PLL's own power plus 20 % of the clock tree, bus and SDRAM-controller power that runs while the cores sleep (`clocks-power.md` §8) [hypothesis]. Bracketed at 2.5–8 mW at the core rail, it reaches the battery through the regulator at about 85 %, giving **ΔI ≈ 1–3 mA, about 2 mA central**.
- A4: the share of lit time that leaves 30 MHz is 100 % if the drift passes at 24 MHz. With the stream hold it covers only the non-drift screens, guessed at 40–60 % of lit time, which halves the figures below.

| Case | ΔI | Per lit hour | Continuous lit use from full |
|---|---|---|---|
| Low | 1 mA | 1 mAh (0.03 % of the cell; ~0.6 min of screen-off playback) | 20.0 h → 20.13 h (**+8 min**) |
| Central | 2 mA | 2 mAh (0.07 %; ~1.1 min of playback) | 20.0 h → 20.27 h (**+16 min**) |
| High | 3 mA | 3 mAh (0.10 %; ~1.7 min) | 20.0 h → 20.41 h (**+25 min**) |
| Central, lit load 190 mA | 2 mA | same | 15.8 h → 15.96 h (+10 min) |

For a typical day with about 1 h lit, that is about 2 mAh, 0.07 % of the battery. Real but small: 1–2 % of lit runtime.

---

## 7. Commit split and owners

Each commit is guarded by `HAVE_PLL_IDLE` (defined only inside `HAVE_IPOD_UI`, `ipodvideo.h` :281–360, not in the simulator or the bootloader) or lives in `apps/ipodui/`. The stock build stays byte-identical (with `english.lang` held equal, checklist 11), and the bootloader is untouched. Budget: ≤ 1 KB `.text` (brief §7), 0 B of runtime reservation, statics under 32 B (need, release deadline, toggle count, strike flag). Order: after `p18-a` (the kick) and with or after `p18-2` (the `clockpol_dark_now()` call).

| # | Commit | Files | Owner |
|---|---|---|---|
| 1 | `firmware: pll_idle takes a lit need from the shell (18-5)` | `firmware/pll_idle.c`, `firmware/export/pll_idle.h` | A |
| 2 | `ipodui: lit clock policy, stream hold and lit strikes (18-5)` | `apps/ipodui/clockpol.c`, `clockpol.h`, `clockpol_core.{c,h}`; the `ui_panel_drift_active()` getter in `preview.{c,h}` (C's file: a read-only accessor, C signs off) | A |
| 3 | `ipodui: developer row "Lit screen: 24 / 30 MHz"` | `apps/ipodui/devmode.c`; two `LANG_*` strings appended to `apps/lang/english.lang` (C writes, the orchestrator merges) | A, C |
| 4 | `ipodui: sleep entry drops the lit need at the dark step` | `apps/ipodui/sleep.c` (one call) | C |
| 5 | `tests: clockpol lit need and release rows` | `tests/clockpol/test_clockpol.c` | A |
| 6 | `tooling: litclk rule, push-cost model, lit-clock script` | `scripts/check_trace.py`, `tests/ui/ipodui-p18-5-lit-clock.script`, the sim-only trace line | T |
| 7 | `ipodui: UI_PTRACE device summary lines` | `apps/ipodui/ptrace.h` plus a small `ptstat` unit, compiled only with `-DUI_PTRACE` | A (C reviews) |
| 8 | (optional) `ipodui: developer arm, drift pushed full width` | `apps/ipodui/preview.c` `push_panel()` behind a developer marker | C |
| 9 | `docs: 18-5 lit clock` at close | `docs/pp5022-reference/clocks-power.md` §8 (the lit rule; lock times as [silicon]); `docs/data-paths.md` R1 step 8; the device round in `docs/device-session-p18-0.md` | A; ledgers and `hardware.md` by the orchestrator |

**Ledger rows (orchestrator):**
- `docs/changes-from-rockbox.md`: amend the Fit-1 C6 row (`pll_idle.c` is ours, but the row lists the policy) with "the lit holder follows the shell's lit need (18-5)". Add a row for `apps/lang/english.lang` (+2 developer strings, appended).
- `docs/design/changes-from-7g.md`: none (nothing user-visible).
- `docs/hardware.md`: the clock row and the budget table ("lit: 30 MHz default; 24 MHz developer switch; the decision waits for the meter").
- `docs/storage-choices.md`: none.

**Device-reality checklist, in short.**
1. Compiled features: target `--ipodui` only; the simulator has no PLL, so it tests only the policy.
2. Memory: 0 B reserved; statics under 32 B.
3. CPU: boosts unchanged; at 24 MHz the unboosted lit work runs about 25 % longer.
4. Storage: no change.
5. LCD: no push changes (arm B is developer-only).
6. Threads: the need is written by the UI thread and applied by the power thread through `cpu_pll_hold()` under the boost lock; each field has a single writer.
7. Splashes: none.
8. Persistence: the switch is not saved.
9. Scale: independent of the library.
10. Deferred work: none.
11. Stock: unchanged.
12. Existing paths: `pll_idle_poll()` stays the only applier of `LIT`, and `clockpol` the only source of both needs.
13. Builder predicate: not applicable.

---

## 8. Risks and owner questions

| # | Risk | Recommended default |
|---|---|---|
| R1 | The premise "E2 = CPU asleep" holds only for pieces of 32 KB or more; bands spin (§2.1) | Accept for 18-5 (wall time is set by the engine); the IRQ-27 wait is a follow-up for `lcd-video.c` |
| R2 | The drift misses its budget at 24 MHz, so the main menu (likely the most-lit screen) keeps 30 MHz | `CP_LIT_STREAM_HOLD` = 1 until §5.2 passes; the saving then covers only the other screens |
| R3 | Light codec plus UI at 24 MHz causes rescues, and 80 MHz bursts cost more than 30 MHz did | `lit_strike` returns lit-and-playing to 30 MHz for the session; §5.2 requires 0 rescues |
| R4 | A lock bit that reads early [hypothesis] | 18-5 adds at most one power-up per 2 s, against about 30 a second already seen in scans with no fault [silicon] |
| R5 | E2 also slows at 24 MHz [hypothesis] | Still inside the budgets in §5.2 (bands ≤ 3.4 ms at 30 MHz); measured from `f3 lcde2` |
| R6 | 18-A not merged: a need change waits up to 2 s | Merge order `p18-a` → 18-5 (§7) |
| R7 | The saving can never be confirmed without a meter, so the switch stays a developer curiosity | Accepted: the code is small and helps sleep entry; the meter decides |
| R8 | Arm B pushes a left half that might not match the screen | Developer-only and off; before any default, a sim check that the left-half framebuffer equals the last pushed copy at each drift step |

**Owner questions** (each with a recommended default, frozen until answered):
- **Q-L1. Land 18-5 now with the default at 30 MHz, or defer the whole item to the meter session?** *Recommended: land it* (≤ 1 KB, `clockpol_dark_now()` removes up to 2 s from every sleep entry, and the device round costs about 50 minutes). The default changes only after a meter reading.
- **Q-L2. If a meter later shows the saving but the drift misses its budget, is 24 MHz everywhere except the drift acceptable?** *Recommended: yes* (the stream hold).
- **Q-L3. Developer-row strings: `LANG_*` as the brief says (Revision 4 N2), or literals like the neighbouring "24 MHz idle" row?** *Recommended: `LANG_*`*, appended at the end of `english.lang`.
- **Q-L4. Skip the voltage A/B runs (§5.3), since they cannot resolve the saving?** *Recommended: skip*, and keep only the boost and idle regression guard from the §5.2 logs.
- **Q-L5. Include developer arm B (drift pushed full width by E2) for a future meter session?** *Recommended: yes, off by default*, since the meter session can then compare three arms at once.
- **Q-L6 (BCM session, #60, no 18-5 dependency).** The drift costs 20–25 % of the CPU on every split screen while lit. A pan done by our BCM image would remove that cost and is a larger power lever than the clock. *Recommended: add it to the BCM session's question list.*
