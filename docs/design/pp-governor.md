# PP5022 band governor: clock bands, core voltage, 100 MHz

Design brief for the PP5022 battery package the owner agreed on 2026-10-25. **Status (2026-10-25, branch `ppgov-impl`):** G0 done (the core, host tests, the simulator; results in §5.4); G1 built behind a developer switch that defaults off (the marker `pgov_on` in the system folder), no voltage change, no 100 MHz. **Stage 2 (2026-10-26, branch `p19-pgov2`):** the voltage step, band 3 and their apply layer, behind two more markers (`pgov2_on` shadow, `pgov2_apply`), developer builds only; §16. Owner: firmware-platform (top model for the core and the voltage path, per `docs/process.md` model tiers). Branch `ppgov`, cut from `p19`.

*Names: the technical names are used here: **PP5022**, its cores the **CPU** (playback back end) and the **COP** (UI front end once 19-9 lands), the **BCM2722**, the **WM8758**, the PMU (PCF50605 family). In chat: PP1, PP2, VC, DAC.*

**Evidence tags.** [silicon] measured on our unit (the log or session is named). [code] read from our source on `p19` (`7b720c68d0`). [rockbox-derived] from upstream's code or comments. [apple] read from Apple's firmware 1.3 (addresses and constants only, `mac-and-dvs.md` method). [estimate] computed from stated figures. [hypothesis] unproven. [literature] published work, cited from memory, not fetched. [ours] our design choice.

**In one paragraph.** Today the PP5022 runs at 24 MHz with the PLL off, 30 MHz when a rule-based holder needs the PLL (lit screen, a heavy track, ATA DMA, USB), and 80 MHz while anything holds `cpu_boost()`. The core voltage never moves (`DCDC1 = 0xEC`). This package replaces the fixed points with **four bands**: (0) 24 MHz crystal, PLL off; (1) PLL on at one voltage step down; (2) PLL on at normal voltage up to 80 MHz; (3) 100 MHz where the chip's speed grade allows. **The band is the decision; inside a band the governor runs at the band's top and lets the cores sleep** (energy per operation is about flat at a fixed voltage), unless the simulator A/B in §5 shows that a proportional pick inside the band wins. Load is measured per core as busy time over short windows; the busier core sets the band, because both cores share one clock. `clockpol`'s rules stay as **floors** that feed the governor. `cpu_boost()` keeps all 341 call sites and becomes "at least band 2 now". The voltage step has an automatic fallback that survives a reboot. Testing is normal use plus logs, with at most one short bench viability row; power figures are [estimate] proxies.

---

## 0. Owner pre-flight (process.md item 1)

The six decisions below were made by the owner on 2026-10-25 and are designed around, not re-asked:

| # | Decision | Where it lands |
|---|---|---|
| D1 | Two-layer band governor; run at the band top and idle-sleep; band choice is the decision; up on saturation, down on idle, with hysteresis and an immediate up on bursts; drop to band 0 whenever the load fits, **lit or dark**; the relock cost sets the hysteresis | §3, §4 |
| D2 | Load measured per core as busy time over short windows; the busier need sets the band | §4.1, §8 |
| D3 | `clockpol` stays for what it does well (per-track needs, the minute verdict), as floors feeding the governor | §4.4 |
| D4 | One core-voltage step down (`DCDC1` within `0xEB`–`0xEE` only) in the low bands, with an automatic fallback (crash or reboot, failed checks, a codec or audio error, a watchdog), recorded so the next boot doesn't retry | §3.2, §9 |
| D5 | Practical testing: no staged read-only probes and no per-step gos for reversible, bounded changes; normal use plus logs; at most one short bench viability row; power figures are [estimate] proxies | §10, §12 |
| D6 | The audio clock comes from the WM8758, so PLL switching must not glitch audio: cite the evidence or flag a check | §6.3 |

Questions still open are in §15, each with a default that holds until the owner answers.

---

## 1. Goals and non-goals

**Goals**

1. **Spend the least energy per unit of work the hardware allows,** by choosing the voltage and PLL state (the band) from measured load instead of from a list of rules.
2. **Generalise the PLL idle** (Fit-1 C6, `pll_idle.c`, `clockpol.c`): band 0 whenever the measured load fits under the lowest PLL band, lit or dark, not only when no rule holds the PLL.
3. **Use the core-voltage step** the PP5022 brief advertises and Apple never used (`mac-and-dvs.md` §2.4), safely: one step, automatic fallback, remembered across boots.
4. **Use 100 MHz** where the chip's grade allows, for sustained saturation only (heavy decode, library builds), never as the default boost.
5. **Keep every existing behaviour the user sees:** the same responsiveness on a key or a scroll (the first frame within the existing budget), no audio glitch, no new splash, no new storage wake.
6. **Prove the policy in a host simulator first,** against traces, with the shipped C core running inside it.

**Non-goals**

- No change to the bootloader (it already runs at 24 MHz and `pcf50605_init()` rewrites `DCDC1 = 0xEC` at every boot [rockbox-derived]).
- No change to the BCM2722's clock governor (P19 PW4, `p19-video-dvfs`); the two chips are separate domains. §2.4 lines them up for symmetry only.
- No per-core clocks (there is one clock; `p19-ui-on-cop.md` §5) and no COP clock skipping (§13, P-5).
- No voltage step **up** (`0xED`/`0xEE`) in v1; band 3 runs at `0xEC` (§3.3, Q-G3).
- No retirement of `cpu_boost()` call sites. They stay; a later trim may retire some once the logs show the governor alone holds the frame budget (§7.3).
- No meter. No current figure here is measured.

---

## 2. What exists today

### 2.1 The clock mechanism [code]

- `set_cpu_frequency()` (`firmware/target/arm/pp/system-pp502x.c` :404–500) knows four points: 32 kHz, 24 MHz (`CPUFREQ_DEFAULT`, PLL off), 30 MHz (`CPUFREQ_NORMAL`, PLL `0x8A22_0501`), 80 MHz (`CPUFREQ_MAX`, PLL `0x8A12_1403`, `DEV_TIMING1 0x0808`). Every PLL setting passes through 24 MHz, and the lock is polled at 24 MHz with the cores running (`clocks-power.md` §4, §8).
- `cpu_boost()` (`firmware/system.c`) is a global counter under `cpufreq_cl`; 0 → 1 selects 80 MHz, back to 0 selects the unboosted frequency. Never called from an ISR (the lock is not IRQ-safe; `p19-ui-on-cop.md` §4).
- `unboosted_freq()` (`firmware/system.c` :56) returns 24 MHz when no `CPU_PLL_HOLD_*` bit is set, else 30 MHz. The holders: `DEV` (policy off), `LIT` (`pll_idle.c`, from the shell's lit need), `LOAD` (`pll_idle.c`, from `clockpol`'s per-track verdict while PCM plays), `ATA` (each UDMA transfer), `USB` (controller enabled).
- **341** `cpu_boost`/`trigger_cpu_boost`/`cancel_cpu_boost` call sites outside `system.c` (apps, firmware, plugins; `grep` on `p19`), 33 in the shell. The codec's are `codec_thread.c` :257/:426/:493/:546/:630 and `pcmbuf.c` :550/:574/:862 (the rescue).

### 2.2 The policy [code]

`apps/ipodui/clockpol{,_core}.c` decides:
- **per track** (`cp_fits()`): a light codec (MP3, PCM family, FLAC, WavPack, ALAC) at ≤ 48 kHz, 1× speed, no software DSP, not struck this session, may play at 24 MHz; anything else holds 30 MHz (`LOAD`);
- **per minute** (`cp_judge()`): a minute at 24 MHz with more than 4 pcmbuf rescues, or any underrun, strikes the format for the session;
- **lit** (`cp_lit_need()`, 18-5): the developer switch "Lit screen: 30 / 24 MHz" (30 by default), a CPU drift stream, or a lit strike holds 30 MHz lit, with a 2 s release.

### 2.3 What silicon has shown

| Fact | Value | Tag |
|---|---|---|
| PLL lock after power-up from 24 MHz | 16–29 µs last wait, max 95–217 µs, over 16,641–290,217 waits per session | [silicon, 18-5 logs `session log, backup 20261017 ipodui.log*`] |
| PLL power cycles during a library scan (ATA holds) | about 30 a second, no fault logged | [silicon, same] |
| Dark FLAC | 24 MHz with the PLL off, `holds=0 load=0` on every stats line; PLL-off share near 100 % of the dark stretch; 18,310 PLL power-ups by 16:11 | [silicon, `darkplay-1614`] |
| CPU decode, FLAC at 24 MHz | ~55 % of the CPU | [silicon, session 36] |
| CPU idle / COP idle, dark FLAC with the COP decoding | 90.4–91.6 % / 29.5–32.4 % | [silicon, `darkplay-1614`] |
| Host-port CPU copy | ~10.5 PP cycles per 16-bit transfer at 30 and 80 MHz | [silicon, S8] |
| Timers (tick, `USEC_TIMER`) | fixed 1 MHz, independent of the core clock | [rockbox-derived] |

### 2.4 The BCM2722's governor, for symmetry (`p19-video-dvfs`, `battery-levers.md` §4, `power-contract.md` §8)

The BCM session solved the same problem on the other chip the same day. What carries over, and what does not:

| BCM2722 (PW4) | PP5022 (this brief) |
|---|---|
| Twelve **level bands**: a voltage level each, the band's top whole MHz used | Four bands; the same "top of the band" rule (D1) |
| A **declared client floor** sets the band when there is one (the video session's FEED times; a `DECODE` burst) | `clockpol`'s per-track floor, the holders, `cpu_boost()` (§4.4, §7) |
| The **load governor** is the safety net for undeclared load: proportional, the top at 0.95, snapped up to a band, **down one band after 100 ms**, an EWMA (¼) of the busy share | Same shape (§4.2); the parameters are swept in our own simulator, not copied |
| Simulated against silicon FEED traces (`vclk_gov_sim.py`); the 100 ms dwell was what made it work (0 misses, transitions ÷9) | Simulated against our load traces (§5); the dwell and EWMA are the first things swept |
| Only **verified** bands are used (`vbv` writes `bcmpassvc.txt`) | A band is used once it has passed its built-in check on this unit, and is dropped for good on a failure (§9) |
| The chip switches and meters; the PP decides (the image had no room) | Everything on the PP; the CPU's tick samples, the power thread applies |
| Decision rule: lower proxy in simulation with no more misses; device rows for viability only; the `batt session` lines over weeks | **Adopted unchanged** (§10.3) |
| Apple's own `min` policy (10 ms windows, ±6 %) missed hundreds of frames on our video | A reminder that a plain interval governor chases bursts; why bursts go up at once through `cpu_boost()` |

---

## 3. The bands

### 3.1 The band table

| Band | Clock source | Operating points (MHz) | Top | `PLL_CONTROL` at the top | `DEV_TIMING1` | `DCDC1` | Exists when |
|---|---|---|---|---|---|---|---|
| **0** | 24 MHz crystal, PLL off and unpowered | 24 | 24 | – | `0x0303` | `0xEB` if DVS is live, else `0xEC` | always |
| **1** | PLL, low voltage | 30 | 30 | `0x8A22_0501` (M 5, N 1, /4, VCO 120) | `0x0303` | `0xEB` | DVS is live on this unit (§9) |
| **2** | PLL, normal voltage | 30, 48, 60, 80 | 80 | `0x8A12_1403` (M 20, N 3, /2, VCO 160) | `0x0303` below 60 MHz; `0x0808` at 60 and 80 | `0xEC` | always |
| **3** | PLL, normal voltage | 100 | 100 | `0x8A12_1903` (M 25, N 3, /2, VCO 200) | `0x0808` | `0xEC` | `STRAP_OPT_A[6:5]` is 0 or 1 **and** the built-in check passed (§3.3) |

Notes on the table:

- **Intermediate points** (band 2's 48 and 60) are used only by the proportional arm (§5). 48 MHz is M 8, N 1, /4 (VCO 192); 60 MHz is M 5, N 1, /2 (VCO 120). Both keep the PP5022's VCO ≥ 96 MHz rule [rockbox-derived] and fall in Apple's recipe ranges (27–60 MHz: /4; 60–125 MHz: /2) [apple, `mac-and-dvs.md` §3.1].
- **`DEV_TIMING1`** follows Apple's 90 ns rule, n = ⌊89 / period_ns⌋, written only when n > 3, raised before a faster clock and lowered after a slower one [apple, `mac-and-dvs.md` §3.3], **floored at Rockbox's value at or below the same clock** so no point ever runs with less margin than today: 60 MHz → rule 5, Rockbox has no 60 MHz value, but 80 MHz uses `0x0808`, so 60 uses `0x0808` too [ours, conservative]; 80 MHz → rule 7, Rockbox `0x0808` kept; 100 MHz → rule 8 = `0x0808`. Below 60 MHz the rule writes nothing and Rockbox's `0x0303` stays.
- **The PLL words and DEV_TIMING1 as built** (`pgov_core.c`, host-tested): 30 and 80 MHz keep Rockbox's words (run daily [silicon]); 48, 60 and 100 MHz take Apple's recipe as written, N 12 (`0x8A22_600C`, `0x8A12_3C0C`, `0x8A12_640C`) [apple]. DEV_TIMING1 is Apple's rule exactly (period = ⌊10⁶/f_kHz⌋ ns, n = ⌊89/period⌋, cap 15) floored at Rockbox's value, so **48 MHz gets `0x0404`** (the rule's n = 4 > 3), not `0x0303` as the table above says; 60/80/100 get `0x0808`. Every Rockbox word has bit 17 set (`0x8A02_0000` base), which the field table in `clocks-power.md` §2 does not name.
- **Band 1 starts with one point, 30 MHz** (the only PLL clock the current code has run unboosted for months [silicon, daily]). A higher low-voltage top (48 MHz) is Q-G1.
- **Band 0 at low voltage** is included (D4 says "the low bands"): the crystal clock at `0xEB` is the dominant dark-playback state, so it is where the step is worth most.

### 3.2 The voltage step

- Under the favoured formula (A, 0.6 V + 0.05 V × code) `0xEB` is 1.15 V; under B (0.9 V + 0.025 V × code) it is 1.175 V. **Both lie inside the PP5022's 1.14–1.32 V window**, and so do `0xEC`–`0xEE` [hypothesis for the formula, `mac-and-dvs.md` §2.3]. `0xEA` and `0xEF` fall outside under A and are never written.
- **Only the low bands use it, at ≤ 30 MHz.** The rated window presumably holds at the rated 100 MHz [hypothesis]; at 24–30 MHz the logic has 3–4× the timing slack it has at the rated clock, which is why one step down at these clocks is the least risky place to use DVS [estimate, reasoning only].
- **What it is worth** [estimate]: core dynamic power scales with V². One step is −8.2 % (A) or −4.1 % (B) of the core's dynamic power. The core's share of the chip's 60–80 mW MP3-decode figure (`wb_5022_Brief_Mar05.pdf` p. 3) is unknown, so the saving is "a few milliwatts in the dominant playback state", low single-digit percent of runtime (`mac-and-dvs.md` §4). No measurement in this package changes that bracket.
- **Rules (hard, in code):** write only `0xEB` or `0xEC`; keep the mode bits `[7:5]`; never touch `DCDC2`–`DCDC4`; read back after every write; raise **before** the clock goes above band 1, lower **after** it is back in band 0 or 1 (the same order as `DEV_TIMING1` and as Apple's BCM2722 driver [apple, `brief-map-power.md` §2]).

### 3.3 Band 3 (100 MHz)

- **Gate:** read `STRAP_OPT_A` (`0x7000_0008`) once at boot. Apple's PP5022 driver maps `[6:5]` to a maximum: 0 → 133, 1 → 100, 2 → 80, 3 → 66 MHz [apple]; band 3 exists only for 0 or 1. The meaning "rated maximum of this die" is [hypothesis]; a read of a strap register is harmless.
- **Built-in check, at the first entry of each boot:** the 10,000-iteration `subs`/`bne` loop timed by `USEC_TIMER` must take 380–420 µs (501 µs at 80 MHz [external, siggi `08`]), and a 32 KB CRC pattern on the running core must match. A failure returns to 80 MHz at once, logs, and records band 3 as off on this unit (§9.4).
- **Caps (band 3 is left, or not entered, while any of these hold):** an ATA DMA transfer (the UDMA timing register assumes 80 MHz, `storage.md`), any host-port push by the CPU or E2 (the BCM2722 port's setup timing has been seen only at ≤ 80 MHz), USB enabled. These come from the holders that already exist (`CPU_PLL_HOLD_ATA`, `USB`) plus one new cap bit set around `lcd_update_rect()`'s push (§8.2).
- **When it is used:** only when band 2's top saturates (§4.2) or a declared floor asks for it (a library build in a maintenance window, `maint.c`). A plain `cpu_boost()` means band 2's top, not band 3.
- **Voltage:** `0xEC`. Whether 100 MHz wants `0xED` is not tested in v1 (Q-G3).

### 3.4 Transition costs (relock and settle)

| Change | What happens | Cost | Tag |
|---|---|---|---|
| Band 0 → any PLL point | PLL power-up and enable, lock polled at 24 MHz with the cores running, then both cores stalled for the `CLOCK_SOURCE` write | lock 16–29 µs typical, 95–217 µs worst; the stall a few cycles | [silicon] lock; [code] stall |
| PLL point → another PLL point (band 2 ↔ 2, 2 ↔ 3, 1 ↔ 2) | step to 24 MHz, retune, relock, select | same lock cost (the retune is a relock) | [code]; [silicon] for lock |
| PLL → band 0 | stall, `CLOCK_SOURCE` to crystal, PLL off, power off | a few µs | [code] |
| Apple's equivalent | fixed 255 µs wait with the post-divider at /8, then the real divider | 255 µs, never polled | [apple] |
| `DEV_TIMING1` | one store, ordered as above | ~0 | [code] |
| `DCDC1` raise | one PMU write over I2C (mutex), read back, then **wait one tick (10 ms) before any clock above band 1** | I2C ~0.2–0.5 ms [estimate]; the settle is unknown, so 10 ms is chosen (Apple's BCM2722 driver waits 10 ms per regulator change [apple]) | [ours] |
| `DCDC1` lower | after the clock is in band 0/1; one write, read back; no wait (lowering under a slower clock is the safe direction) | I2C only | [ours] |
| PLL start energy | unknown | – | [hypothesis] |

**What this means for the hysteresis.** A PLL relock costs at most ~0.25 ms of slower running, so on time alone even a 100 ms dwell costs under 0.3 %. The unknown is the PLL start's energy; the ATA holder already cycles the PLL ~30 times a second without a fault. So the rule is: **PLL transitions are not rationed by time cost, but are kept to a few per second outside ATA bursts by the dwell**, and the simulator counts them (a flapping arm loses even if its proxy is lower). **Voltage changes are rationed hard**: a raise costs a 10 ms wait on the first burst, so the voltage drops only after **2 s** in bands 0/1 without a boost (§4.3).

---

## 4. The band selector and the in-band policy

### 4.1 Measuring load (per core)

- **Idle accounting, always on (not only `HAVE_DEVSTATS`):** each core stamps `USEC_TIMER` on entry to and exit from `core_sleep()` and adds the difference to its own `pgov_idle_us[core]` (SHAREDBSS, one writer per core). About 10 instructions per sleep [estimate]. When `HAVE_DEVSTATS` is on, `devstats.idle_us[]` and these words are the same counters (one hook, not two).
- **Sampling:** the CPU's tick (100 Hz, already running) reads both words every tick, computes each core's busy share over the 10 ms sample, scales it to "MHz needed" at the clock that ran (`busy × f`), and keeps an EWMA with weight ¼ (time constant ≈ 35 ms) [ours; the BCM2722's governor kept the same weight]. **No clock change happens in the tick** (cpu_boost's lock is not IRQ-safe): when the decision changes, the tick calls `power_thread_kick()` (P18 18-A), and the power thread applies it. A decision change is rare by construction (§4.2), so this adds no steady wake.
- **The need:** `need = max(need_cpu, need_cop)` (D2). After 19-9 the CPU carries decode and the COP the UI; before it, the COP may carry decode (Fit-2) or nothing. Both are handled by the same max.
- **Memory stalls** do not scale with the clock (`DEV_TIMING1` lengthens with it), so "busy × f" overstates what a faster clock buys. The selector compensates only through headroom (0.8) and the saturation test; the simulator models it with a memory-bound fraction m (§5.2).

### 4.2 The selector (between bands)

Per sample, with `top(b)` the band's top MHz and `H = 0.8` the headroom [literature: schedutil's 1.25× margin]:

```
want = lowest live band b with need_ewma <= H * top(b)
     , raised to every floor (§4.4) and to band 2 while cpu_boost() > 0
     , lowered to every cap (§3.3)

UP   immediately when want > band, and also when the band is saturated:
     the running core's busy share >= 0.95 in 2 consecutive samples
     (the EWMA lags a step change; saturation does not wait for it)
DOWN one band at a time, only after want < band has held for
     D_down(band): 2 -> 1 or 0: 200 ms;  1 -> 0: 500 ms;  3 -> 2: 100 ms
```

- **Bursts go up at once** through `cpu_boost()` (§7): the wheel, a glide, a slide, a pcmbuf rescue, a seek. The measured path never delays them.
- **Band 0 whenever the load fits** (D1): `need_ewma ≤ 0.8 × 24 = 19.2 MHz` with no PLL floor or boost, lit or dark. This is what generalises today's rule-based PLL idle.
- **The dwell values are starting points** taken from the BCM2722 result (100 ms made it work) and lengthened for the PLL start's unknown energy; §5 sweeps them (50–1,000 ms) before any code.

### 4.3 The voltage layer

- The voltage follows the band with its own dwell: low (`0xEB`) only after **2 s** continuously in bands 0/1 with no boost and no floor above band 1; normal (`0xEC`) before any move to band 2 or 3.
- **A boost while the voltage is low** (the only latency cost of DVS): the boosting thread raises the clock to band 1's top at once (no I2C), then, outside the boost lock, raises `DCDC1`, reads it back, sleeps one tick, and only then selects band 2's top. The first frame of a slide after a quiet stretch therefore starts at 30 MHz for ≤ ~11 ms, inside the 33 ms first-frame budget [estimate]. A COP-side boost (after 19-9) cannot use the I2C driver [hypothesis; the driver has never run on the COP], so it posts the raise to the CPU (`be_post`, `p19-ui-on-cop.md` §3) and runs at band 1's top until it lands.
- **Storage writes run at normal voltage** (§9.2): an ATA write sets a floor of "band 1 clock at `0xEC`" for its duration.

### 4.4 Floors (clockpol, holders, clients)

Every existing reason to hold the PLL becomes a **floor**: "the band may not go below b". The governor never goes below a floor; it may go above it on load.

| Floor source | Today | Floor | Notes |
|---|---|---|---|
| `clockpol` per track: the track doesn't fit 24 MHz | `LOAD` → 30 MHz while PCM plays | band 1 (band 2's 30 MHz point if band 1 is not live) | Pre-emptive: avoids a minute of rescues at every track start; the measured load can still raise it |
| `clockpol` minute verdict: a strike | the format → 30 MHz for the session | same, for that format | Unchanged rule; the governor's own saturation path usually acts first |
| ATA DMA transfer | `ATA` → 30 MHz | band 1 (clock), voltage normal for writes | UDMA2 is rated at 30 MHz [rockbox-derived] |
| USB enabled | `USB` → 30 MHz | band 1 | Undocumented ARC clocking [hypothesis] |
| Lit screen, developer switch at 30 | `LIT` → 30 MHz | band 1 | **Default changes to 24** (D1: band 0 lit or dark); the switch stays as a developer floor for comparison (Q-G4) |
| Policy off (developer "24 MHz idle: off") | `DEV` → 30 MHz always | band 1 at `0xEC`; also disables DVS | The governor's own off switch |
| Maintenance window, library build | `cpu_boost()` → 80 | band 2 top; band 3 allowed if saturated | `maint.c`, `artcache.c`, `bootstamp.c` keep their boosts |
| Sleep entry (18-1 `power_sleep()`) | refuses unless 24 MHz and no holds | band 0 at **normal voltage** before entry | Q-G5 |

`pll_idle.c` keeps its role as the place holders are sampled (2 Hz power step plus kicks) and turns them into floors; `cpu_pll_hold()` keeps its signature so its callers in `ata-pp5020.c` and `usb-fw-pp502x.c` need no edit.

### 4.5 In-band policy

Two arms, decided by the simulator (§5), with a constant `PGOV_INBAND` selecting the winner in the build:

- **T, top of band (the owner's premise):** a live band always runs at its top. At a fixed voltage the core's energy per operation is about constant, so finishing sooner and sleeping costs the same core energy, and the idle power is lower if the clock tree is gated in sleep [literature: race to idle].
- **P, proportional inside the band:** the lowest operating point of the band with `need_ewma ≤ 0.8 × point`. It wins if the clock tree, bus and SDRAM controller draw power proportional to the clock even while the cores sleep, or if work is memory-bound (it does not finish sooner at a higher clock).

The difference matters only in band 2 (band 0, 1 and 3 have one point each). If band 1 is never live on a unit, band 2's 30 MHz point is what P uses for the light loads that today run at 30.

---

## 5. Simulator first: the A/B (top of band vs proportional)

### 5.1 The harness

- **The shipped core runs in the simulator.** `firmware/pgov_core.c` is pure C (no kernel calls), compiled into the target and into `tests/ppgov/sim` on the Mac. The simulator feeds it 10 ms samples and applies its decisions to a model of the PP5022; no parallel Python re-implementation of the policy.
- **Traces:** one line per 10 ms sample: µs of work demanded per core at a reference clock, the boost count, the floors and caps active, the screen state. Two sources:
  1. **Synthetic, from silicon figures, first** (no device time needed): dark FLAC (55 % at 24 MHz, refills every few minutes with ATA holds), dark AAC and Vorbis (the 30 MHz holds' share, `clockpol` strikes), dark MP3 at 1.5× speed (time-stretch), lit Now Playing idle (the 1 Hz bar, the title marquee, E2 bands), lit browsing (glides boosted, list pushes, the drift as a BCM2722 job and as a CPU stream), a library scan (30 ATA holds a second), a video session's PP side (the FEED and the audio decode), the unlit clock face.
  2. **Recorded, from normal use** once the first build is on the device: the developer trace ring (§11.3) writes the same format.
- **The PP5022 model**, all parameters named and bracketed [estimate]:
  - core energy per cycle ∝ V²; idle-sleep core power ≈ 0;
  - clock-tree, bus and SDRAM-controller power ∝ V² × f × (1 − g × idle share), with **g** (how much of it core sleep gates) swept 0–0.8;
  - PLL power **p_pll** while powered, swept 0.5–3 mW;
  - a PLL start's energy **e_start**, swept 0–50 µJ;
  - memory-bound fraction **m** of the work, swept 0–0.4 (time at f = t₀ × ((1 − m) × f₀ / f + m));
  - voltage formula A or B;
  - transition times from §3.4.
- **Misses:** audio (the codec's backlog against the PCM buffer: a miss is the buffer emptying), UI (a frame's work not done inside its 33 ms slot), and the rescue count (the pcmbuf watermark firing).

### 5.2 Arms

| Arm | What it is |
|---|---|
| R | today's rules (24 / 30 / 80, `clockpol`, the holders), as the baseline |
| T | the governor, top of band |
| P | the governor, proportional in band 2 |
| T−1, P−1 | the same with band 1 absent (DVS not live): the unit where the voltage step fails |
| T+3, P+3 | with band 3 live |

Swept per arm: the dwell (50–1,000 ms), the EWMA weight (⅛–½), the headroom (0.7–0.9), the voltage dwell (0.5–5 s).

### 5.3 The decision rule (the BCM2722's, adopted)

1. **In simulation**, an arm replaces another when its energy proxy is lower **with no more misses**, across the whole parameter grid. Transitions above 20 PLL starts a minute outside ATA bursts count as a loss.
2. If T and P swap places inside the grid (the answer depends on g or m), **T ships** (the owner's premise, one point per band, fewer transitions), and the logs carry the band times needed to revisit it.
3. **On the device**, the bench viability row (§10.2) passes and the logged proxies agree (no misses; band-0 share in dark at or above today's ~90 %).
4. **Over regular use**, the `batt session` lines show no shorter life per lit or playing hour. Power is never a bench pass condition.

The simulator's report (`scripts/pgov_sim.py`, a thin wrapper that runs the C harness over the grid and prints the table) and its chosen constants are committed with the core, before any target code is switched on.

### 5.4 Results (G0, 2026-10-25) [estimate: model and traces are synthetic]

**Harness.** `tests/ppgov/sim.c` runs `firmware/pgov_core.c` unchanged. Ten traces, all synthetic, each calibrated where a silicon figure exists: dark FLAC (55 % at 24 MHz, session 36; a flash refill every 120 s, ~33 transfers a second for 2 s; a 0.5 s track-load boost every 240 s), dark AAC / Vorbis / MP3 at 1.5× (27 / 28.6 / 25.5 MHz, [estimate] from upstream's codec tables, with `clockpol`'s 30 MHz floor), dark paused, lit Now Playing (the 1 Hz bar; the marquee is a BCM2722 pan), lit browsing (wheel bursts and boosted slides from session 38's f3 counts), a boosted fast spin, a 40 s-at-80-MHz library scan with ATA every third sample, and the video session's PP side. The codec model is the real one: pcmbuf 3 s, the 2 s rescue watermark that boosts until full, frames decoded only when a frame's space is free. UI frames run on the COP with a 33 ms deadline. A PLL start or retune costs Apple's fixed 255 µs [apple] (our polled lock is 16–217 µs [silicon]). The day mix weights the traces 40/10/5/5/10/10/12/3/3/2 % [ours]. The energy proxy: core 40 mW busy at 80 MHz/1.2 V, k_clk 6/12/24 mW at 80 MHz (an added sweep: the clock-tree share decides T against P), g 0/0.4/0.8, p_pll 0.5/1.5/3 mW, e_start 0/10/50 µJ, formula A/B, m 0/0.2/0.4: 486 model points. The policy grid: dwell (band 2) 20–1,000 ms (band 1 = 2.5×, band 3 = ½×), saturation run 2/4/8/16 samples, H 70/80/90/95 %, a down threshold (below), EWMA ⅛–1/32: 1,152 configurations per arm. Run it with `build/ppgov/sim --grid` (built by `tests/ppgov/run.sh`'s commands; ~2.5 min).

**Two findings the brief's parameters did not survive.**
1. *The EWMA ¼ at 10 ms is shorter than a codec frame.* At 80 MHz a 93 ms FLAC frame decodes in one sample at 100 % busy, so the ¼ EWMA jumps 20 MHz every frame and never decays under 19.2 MHz: dark FLAC sat at 80 MHz for 99.9 % of the run (+66 % on the proxy). A ⅛ weight (τ ≈ 75 ms) and a 16-sample saturation run fix it; the 2-sample saturation test fired on every frame burst at 24 MHz.
2. *"busy × f" overstates a memory-bound load measured at the higher clock* (§4.1 predicted it): a 27 MHz load measured at 48 MHz reads ~29–31 MHz and never comes back to 30. The selector now goes **up by H and down by a wider threshold** (`down_pct`, 110 %: step down when the need measured above fits 110 % of the lower point; a wrong guess comes back up through H).

**Verdict by the decision rule (§5.3).**

| Pair | P lower than T at | Pick (fewest changes within 0.5 % of the lowest energy, no more misses than R in any trace, ≤ 20 PLL starts a minute outside ATA) | Day mix vs R (mid model) |
|---|---|---|---|
| T−1 / P−1 (G1: no band 1, no band 3) | **486 / 486** | P−1: dwell 100 ms, sat 16, H 95 %, down 110 %, EWMA ⅛ | **P−1 0.995**, T−1 1.076 (T−1 below R at 28/486 points) |
| T / P (band 1 live, DVS) | 450 / 486 | P: dwell 50 ms, sat 16, H 95 %, down 100 %, EWMA 1/32 | P 0.954, T 1.008 |
| T+3 / P+3 | 450 / 486 | as P | P+3 0.955 |

- **G1 ships P** (`PGOV_INBAND_PROP`): P−1 wins at every model point. With DVS (G2) T and P swap at 36 points (low k_clk with high g), so rule 2 would ship T there; the G2 increment re-runs the grid with its own build and decides then. The owner's premise (T) fails here because band 2 spans 30–80 MHz at one voltage while the clock tree's idle share is not gated to zero (g < 1 in every bracket).
- **Chosen constants** (`pgov_params_default()`): dwell 100 ms out of band 2, 250 ms out of band 1, 50 ms out of band 3; H 95 %; down 110 %; EWMA ⅛; saturation 16 samples ≥ 95 %; voltage dwell 2 s (not swept in G1). The energy optimum sits at the shortest dwell swept (20 ms); only the transition count separates the configurations there, so the pick is the fewest changes within 0.5 %.
- **Per trace, P−1 against R** (mid model, `tests/ppgov/sim_expected.txt`): lit Now Playing **−16 %**, lit browsing **−11 %** (band 0 lit, D1), dark FLAC +0.2 %, paused 0, fast spin 0, library +0.7 %, **AAC +5 %, Vorbis +11 %, video +11 %** (loads just under 30 MHz that touch 48), MP3 1.5× 0. No underrun, rescue, UI miss or unfinished work in any trace or arm. Lit browsing makes 19.5 PLL starts a minute (R 0.5): the 20-a-minute rule's edge.
- **What G1 is for**, then: the lit clock (band 0 lit) and the measured load replacing the rules, at no loss on the day mix; the voltage step (G2) is where the proxy gain is (P 0.954). The heavy-format loss is the next policy item: a declared floor point that holds the load with no rescue in a minute could stay put (the codec's buffer is the deadline), to be tried in the simulator before G2.

---

## 6. Hardware features (required section; `docs/hardware-features.md`)

### 6.1 Catalog rows that carry this work

| Row | Block | Use here | CPU fallback | Unknown, and what settles it |
|---|---|---|---|---|
| **P-3** | Core voltage scaling (`DCDC1`) | Bands 0 and 1 at `0xEB` | `0xEC` always (today) | The formula (A or B) and the margin: not settled by firmware; the built-in checks and the fallback (§9) carry the risk instead of a probe, per D5. The meter reading of `mac-and-dvs.md` §5.4 step 0 stays optional |
| **P-2** | 100 MHz | Band 3 | Band 2's 80 MHz | The grade (`STRAP_OPT_A`, read at boot) and the 100 MHz check (§3.3), both built in |
| **P-6** | Intermediate clocks | Band 2's 48 and 60 MHz points (PLL) | 30/80 | The PLL-free taps (`CLOCK_SOURCE` codes 3–6: 33, 48, 20/40 MHz, open question #9) would give a band 0.5 without the PLL's power. **Not in v1**: Apple decodes codes 2 and 4 through a second PLL at `0x6000_6048` [apple], so whether a tap is PLL-free is unknown. Probe: switch the selector and time the loop against `USEC_TIMER`, reversible; a later increment if the simulator says a 48 MHz PLL-free point would win |
| **P-5** | COP clock skipping | **Not used** | – | It reads as a per-instruction stall, not a frequency or voltage change (`datasheet-features.md` row 3); the COP is parked in the dark anyway (19-1d) |
| **P-7** | The COP as compute engine | The second load source (§8) | – | – |
| **P-4** | Module clock gating | Not this package (adjacent standby saving) | – | – |
| **W-2** | WM8758 PLL and power states | The audio clock is the WM8758's, so PLL switching on the PP5022 does not touch it (§6.3) | – | – |
| (none) | PMU watchdog (`OOCC1` bit 3 `WDTRST`) | **Not used**: its timing and reset semantics are unknown [hypothesis]; the software watchdog in §9.3 covers the case | – | Would need the PMU datasheet (open question #20) |

### 6.2 Read-only facts the code reads at boot

`STRAP_OPT_A` (grade), `PP_VER1` (the chip-ID byte, `'2'` for PP5022), `DCDC1` (must read `0xEC` before DVS is ever armed). These are reads of registers, not probe stages.

### 6.3 The audio clock (D6)

**Evidence that PLL switching does not reach the audio clock:**
- The WM8758 is the I2S clock master (`CLKCTRL_MS`); its MCLK is the PP5022's external device clock taken from the **24 MHz crystal**, `0x7000_0018` bits 2–3 cleared (`wmcodec-pp.c`), and the PP's I2S block is a slave (`IISDIV` unused) [rockbox-derived] (`clocks-power.md` §8, `audio.md`).
- The core voltage is `DCDC1`; the WM8758 runs from `D1REGC1` (an LDO) [rockbox-derived], which nothing here touches.
- **On silicon:** the dark FLAC run powered the PLL up 18,310 times (ATA holds) with no `clockpol` strike recorded (a strike would have set `load=1`; every stats line shows `load=0`) [silicon, `darkplay-1614`]. That rules out a rescue storm, not a single click.

**What can still glitch, flagged as checks:**
- During each `CLOCK_SOURCE` write both cores are stalled with IRQ and FIQ off (a few cycles) [code]. PCM is refilled by FIQ (`pcm-pp.c`) [code]; the I2S FIFO covers far more than a few cycles [estimate]. **Check:** the bench row's underrun counter over ≥ 1,000 transitions (§10.2), and the owner's ears.
- A voltage write's effect on the PP's I2S block (same die) is unknown [hypothesis]. **Check:** the same row with DVS live; any underrun or codec error at low voltage is also a DVS fallback trigger (§9.3).

---

## 7. `cpu_boost()`: wrap, don't replace

### 7.1 The rule

`cpu_boost()` keeps its signature, counter and lock, and its meaning becomes **"at least band 2's top, now"**: `boost_counter > 0` is a floor of band 2 at the top point under either in-band arm. Release (back to 0) hands the band back to the governor, which steps down by its dwell instead of dropping at once. All 341 call sites stay; the stock build compiles upstream's code (as `set_unboosted()` does today under `HAVE_PLL_IDLE`).

### 7.2 Why wrap

- **Bursts must not wait for a measurement.** The wheel and slides need their first frame within 33 ms; a 10 ms sample plus an EWMA lags by 20–40 ms. `cpu_boost()` is already the "this is a burst" signal at exactly the right places (`anim.c`, the rescue in `pcmbuf.c`, the codec's seek).
- **Replacing 341 sites** (many upstream, many in plugins) is a large, risky edit for no measured gain.
- **Band 3 is not a boost:** a boost never goes above 80 MHz; band 3 comes only from saturation at band 2 or a declared floor (§3.3). So every existing boost keeps today's 80 MHz behaviour, and UDMA4's boost (S36) is unchanged.

### 7.3 Later

The logs record boosted milliseconds per call site class (anim, codec, maintenance). Where the governor's band already covers a boost (a long library build that saturates band 2 anyway), a later trim may drop the call. Not in this package.

---

## 8. The 19-9 COP split

### 8.1 Load per core

- After 19-9 the CPU is the back end (decode, storage, the BCM2722 feed) and the COP the front end (the UI loop). Each has its own idle word (§4.1); the governor takes the larger need (D2). Before 19-9 the same code sees the COP's decode (Fit-2, a developer A/B row) or a parked COP (19-1c/d).
- **Dark playback** (19-1's end state): the COP is parked by K-1 (no pulses), so its busy share is 0 and the CPU's decode sets the band: FLAC at ~55 % of 24 MHz [silicon] → need ≈ 13 MHz → band 0. Heavy formats → `clockpol`'s floor → band 1 → (if the measured load fits under 19.2 MHz after the floor is retired by a clean minute) band 0. The minute verdict stays the safety: a strike puts the floor back.
- **Lit:** the UI's work on the COP and decode on the CPU each count separately; the band follows the busier one, as schedutil does for a performance domain [literature].

### 8.2 What 19-9 changes for the governor

| Item | Handling |
|---|---|
| A COP-side `cpu_boost()` while the voltage is low | Clock to band 1's top at once; the voltage raise is posted to the CPU (§4.3). The COP never calls the I2C driver |
| The tick that samples is the CPU's | The COP's idle word is SHAREDBSS and single-writer; the CPU reads it uncached. A parked COP's word simply stops changing |
| The host-port cap for band 3 (§3.3) | Set around the push wherever it runs (the COP owns the ring after 19-9, `p19-ui-on-cop.md` §6); a cap word per core, OR-ed |
| `set_cpu_frequency()` from either core | Already supported (`scale_suspend_core()` holds the other core) [code]; the governor applies bands only from the power thread (CPU), boosts from either |
| K-3 per-core boosted µs (`p19-ui-on-cop.md` §4) | Read by the `pgov` summary line when present |

---

## 9. Safety and fallback

### 9.1 Invariants (enforced in `pgov_core.c`, host-tested after every step)

1. The clock never exceeds the ceiling of the voltage that is set: at `0xEB`, 30 MHz; at `0xEC`, 80 MHz, or 100 MHz with band 3 live.
2. `DCDC1` is only ever `0xEB` or `0xEC`, mode bits unchanged; `DCDC2`–`DCDC4` are never written.
3. Voltage up before the clock goes up; voltage down after the clock is down; `DEV_TIMING1` the same way.
4. Every PLL retune passes through 24 MHz (Rockbox's rule 2).
5. A band that failed its check on this unit is never used again until a developer reset (§9.4).

### 9.2 When DVS is armed (the voltage may go low)

All of: `DCDC1` read `0xEC` at boot; the persistent record (§9.4) allows it; ≥ 60 s since boot (the boot, the database load and the first fill run at normal voltage); not on USB; the battery above the disk-safe level (3,500 mV); no storage write in flight (writes force normal voltage, §4.3), since a marginal core corrupting a FAT or database write is the one failure that outlives a reboot.

### 9.3 Runtime fallback triggers (any one, at once)

| Trigger | Detected by |
|---|---|
| `DCDC1` read-back is not what was written | the write path |
| An integrity check fails | while the voltage is low, the power thread CRCs a 4 KB slice of the kernel's `.text` each minute against the CRC taken at boot at normal voltage (rotating through the image; ~25 k cycles, ~1 ms a minute at 24 MHz [estimate]) |
| A codec error | the codec's error return (`codec_thread.c`), counted while low |
| An audio underrun, or a rescue storm (> 4 a minute) | `pcmbuf`'s counters, as `clockpol` reads them |
| A software watchdog | the tick sees the codec or main-loop heartbeat stalled > 2 s while low: it sets normal voltage by the raw polled PMU write `power_sleep()` already uses, only if the I2C mutex is free, otherwise it reboots (`system_reboot()`); either way the record below attributes it |
| A data abort, undefined instruction or panic | the exception path sets normal voltage (raw write) before logging (no splash) |
| Band 3's check | §3.3 |

**The action:** normal voltage at once, DVS off for the rest of the boot, a `pgov fb` line, and the persistent record updated at the next storage-idle write.

### 9.4 The persistent record (no retry of a failed setting)

- **Where:** one field in the existing boot stamp (`apps/ipodui/bootstamp.{c,h}`, `boot_stamp.dat`), written at its existing early-boot settle and clean-shutdown writes. **No new file and no new write per boot.** (If the stamp's owner refuses, a 1-sector `pgov.dat` written only on a state change is the fallback.)
- **Contents:** `dvs` = untried / trial / ok / off; a strike count; the code that failed; `b3` = untried / ok / off.
- **Arming before trying:** the voltage is lowered in a boot only **after** the settle write has recorded "armed this boot". A crash before that write cannot happen at low voltage.
- **Attribution:**
  - a runtime trigger (§9.3) is attributable: `dvs = off` at once, permanently;
  - an **unclean end** of an armed boot (a crash, a hang the owner reset with MENU+SELECT, a battery pull) is ambiguous: one strike skips DVS for the next boot; **two strikes within five armed boots** turn it off (Q-G2);
  - a clean end after ≥ 1 h at low voltage counts toward `ok` (10 such boots), which only stops the trial log lines.
- **Recovery:** a developer row resets the record. Rockbox's `pcf50605_init()` writes `DCDC1 = 0xEC` at every boot and a PMU standby restores the defaults [rockbox-derived], so no state survives a power cycle except the record. Apple's firmware (hold MENU) is untouched.

---

## 10. Testing (D5)

### 10.1 Host tests, `tests/ppgov/` (run by `scripts/test.sh`)

- **Selector rows:** each band's entry and exit; the headroom edge (19.2 MHz in, 19.3 out); saturation up after 2 samples; the dwell per band (a gap of dwell − 1 ms never steps down); floors and caps combined; a boost → band 2's top at once; release → the dwell; band 3 only from saturation or a floor, never from a boost; a cap leaves band 3 at once.
- **Voltage rows:** the 2 s low dwell; a boost while low gives band 1's top, then the raise, then band 2 (ordering checked on every step); invariants 1–4 after every single decision over 10⁶ random steps.
- **Fallback rows:** every trigger → normal voltage and off-for-boot; the record's transitions (armed, strike, two strikes in five, off, ok, reset); "lower only after armed".
- **Tables:** `DEV_TIMING1` by Apple's rule with Rockbox's floor; the PLL words for 30/48/60/80/100 against M/N/post and the VCO ≥ 96 MHz rule; the grade table.
- **Per-core:** need = max of the two; a parked COP (frozen word) reads as idle.
- **The A/B simulator** (`tests/ppgov/sim`) runs the synthetic traces at fixed parameters as a regression (its table is checked in; a policy change that moves it fails until the table is updated with a reason).
- **No simulator script** in `tests/ui/`: the UI simulator has no PLL (`HAVE_PLL_IDLE` is target-only). The shell's floors are already covered by `tests/clockpol`.

### 10.2 The one bench viability row, `pgov`

Owner side, on-device prompts, one results file (`bench-feedback-on-device` style); about 15 minutes; no meter; no owner go per step (the change is reversible and has its net):
1. A mixed playlist (MP3, FLAC, AAC, one hi-res FLAC), the screen lit: browse, spin fast, open Now Playing, 5 minutes.
2. Hold on, dark, 5 minutes.
3. Lit again, a library-heavy action (Search, Songs fast spin), 2 minutes.
4. Prompts: "Did the audio skip or click?" (No / Once / Often), "Did anything feel slower?" (No / Slightly / Yes).
- **Automatic verdict from the log:** 0 underruns; 0 fallbacks (or, if one fired, the record shows it and the unit continued at normal voltage); ≥ 1,000 band transitions logged across the row; the time per band; the band-3 check result if the grade allows it.
- Pass = both prompts "No" and the automatic verdict clean.

### 10.3 Normal use

The `pgov sum` and `batt session` lines (§11) over the following weeks; the decision rule in §5.3.

---

## 11. Logging

All through `ui_log()` (62-character lines) to `ipodui.log`, flushed at the existing storage-idle write. No splash, no new write.

### 11.1 Lines

| Line | When | Fields |
|---|---|---|
| `pgov boot grade=G strap=0x.. dvs=STATE strikes=N b3=STATE` | boot | the gate inputs and the record |
| `pgov sum t=S b0=‰ b1=‰ b2=‰ b3=‰ vlow=‰` | hourly, before sleep, at power-off (with `batt session`) | time share per band and at low voltage |
| `pgov tr up=N dn=N pll=N vch=N boost=N sat=N` | same | transitions, PLL starts, voltage changes, boosts, saturation ups |
| `pgov miss und=N resc=N codec_err=N` | same | the misses |
| `pgov fb why=readback\|crc\|codec\|underrun\|dog\|abort\|unclean\|b3check band=B code=0x..` | at each fallback | the trigger |
| `pgov b3 loop_us=N crc=ok\|bad` | first band-3 entry per boot | the check |
| `clk lock …` | as today | lock times |

### 11.2 The battery record

If `batt session` (`p19-video-dvfs` 7ed3dc2824) has merged, its line gains nothing (62 characters are spent); `pgov sum` is written beside it with the same `at=` trigger, so `scripts/bench_ingest.py` can join them. The ingest's report gains a "PP clock" section (time per band, transitions, fallbacks), owned by tooling.

### 11.3 The developer trace ring (for the simulator)

Developer builds only, off by default (Developer › "Clock trace"): 6 B per 10 ms sample (busy ‰ per core, band, boost, floors), 4,096 samples (24 KB, ~41 s), allocated with `core_alloc` **at boot only, before the audio buffer is sized**, so it never shrinks a playing buffer; written as `pgov_trace.bin` at the next storage-idle write when full. Its format is the simulator's trace format (§5.1).

---

## 12. Size and RAM budget

| Item | `.text` | Statics | Runtime RAM |
|---|---|---|---|
| `pgov_core.c` (selector, voltage layer, invariants, record logic) | ≤ 2,000 B | ≤ 96 B | 0 |
| Glue: `pgov.c` (tick sampling, power-thread apply, PMU write, checks, logs) | ≤ 1,500 B | ≤ 96 B (band times, counters, CRC table pointer) | 0 |
| `set_cpu_frequency()` generalised to a point table (5 PLL words, `DEV_TIMING1` rule) | ≤ 400 B | 0 (const table ~40 B in `.rodata`) | 0 |
| Idle accounting in `core_sleep()` | ≤ 48 B (inline in existing code; no new IRAM function) | 8 B per core | 0 |
| `clockpol` floors (replaces `tell()`'s two needs) | ±100 B | 0 | 0 |
| Developer trace ring | developer builds only | 0 | 24 KB at boot when on |
| **Total, release** | **≤ 4 KB** | **≤ 256 B**, none over 1 KB | **0** |

No new thread (the power thread applies; the tick samples). No IRAM taken from the codecs: `core_sleep()`'s few added instructions sit where it already lives.

---

## 13. State of the art (required section)

`docs/algorithms-survey.md` has no row for clock or voltage governors. **Proposed row** (for the orchestrator to add): *"CPU frequency and voltage selection | today: rules (`clockpol`) plus boost counting | state of the art: schedutil with utilisation clamping, EAS's energy model, interactive-style input boost | adopted: a schedutil-shaped utilisation rule over a four-band energy model, input boost via `cpu_boost()`, simulator-chosen dwell | P19/ppgov"*.

| Work | What it does | Here |
|---|---|---|
| **Weiser et al., "Scheduling for reduced CPU energy", OSDI 1994** [literature] | Interval-based speed setting (PAST: next interval's speed from the last interval's busy share) | The origin of every interval governor, ours included; its known weakness (chasing bursts) is why bursts bypass it |
| **Govil, Chan, Wasserman, MobiCom 1995; Pering, Burd, Brodersen, ISLPED 1998** [literature] | Compared interval predictors (AVG_N, peak-tracking), found simple averages with fast-up work best | The EWMA plus immediate up on saturation |
| **Linux ondemand / conservative (Pallipadi and Starikovskiy, OLS 2006)** [literature] | up_threshold, jump to max on high load, step down gradually | The saturation test and one-band-at-a-time down |
| **Android "interactive" governor (2010 onward)** [literature] | Jump to a high speed on input (`input_boost`), hold a minimum sample time before going down | `cpu_boost()` from the input and animation paths, and the per-band dwell |
| **schedutil (Linux 4.7, 2016)** [literature] | Frequency from the scheduler's utilisation with a 1.25× margin; a performance domain's frequency follows the max utilisation of its CPUs; a rate limit; an I/O-wait boost | **Adopted in shape:** headroom 0.8 (= 1.25×), the max over both cores (one clock = one domain), the dwell as rate limit, the ATA floor as its I/O boost |
| **Energy Aware Scheduling (Linux 5.0, 2019)** [literature] | An energy model (power at each operating point per domain) used to place tasks and pick frequencies | Our band table plus the simulator's model is a small energy model; task placement is fixed by the owner (decode on the CPU, UI on the COP), so only the frequency half applies |
| **Race to idle vs DVS: Miyoshi et al., "Critical power slope", ICS 2002; Le Sueur and Heiser, "DVFS: the laws of diminishing returns", HotPower 2010; Albers and Antoniadis, SODA 2012** [literature] | At a fixed voltage, running fast and sleeping wins when idle power is low; voltage scaling is where slowing down pays; memory-bound work gains little from a higher clock | **The basis of D1:** inside a voltage band, run at the top (T) unless idle-time clock power is large (the P arm tests exactly this); the voltage step lives in the low bands; m in the simulator models the memory-bound case |
| **Apple's PP5022 clock driver** [apple] (`mac-and-dvs.md` §3) | A set-frequency method resident in IRAM; three PLL recipes by range (12–27, 27–60, 60–125 MHz) from the 24 MHz reference; the PLL programmed with the post-divider at /8, a fixed 255 µs wait, then the real divider and the PLL selected, interrupts off throughout; `DEV_TIMING1` from a 90 ns rule, raised before speeding up and lowered after; a per-chip ceiling read from `STRAP_OPT_A[6:5]`; **the core voltage never changed on a PP5022** | **Adopted:** the recipe ranges for the new points, the 90 ns rule (floored at Rockbox's values), the grade gate, the raise-before/lower-after order. **Not adopted:** the fixed 255 µs wait (we poll the lock bit and have measured it, 16–217 µs [silicon]; the /8 soft start is noted as a fallback if a lock fault ever appears). **Beyond Apple:** the voltage step. Apple's *policy* (which client asks for which kHz) has not been traced; a static search of the method's callers is a cheap, device-free follow-up |
| **Apple's BCM2722 `min` policy** [apple] (`brief-map-power.md` §2) | 10 ms windows, +6 % when busy, −6 % when idle, 5–150 MHz, clients' floors | Same family; our own BCM simulation showed it misses bursts without client floors, which is why floors and `cpu_boost()` lead here |

Not adopted, with the one-line reason: **PELT-style geometric decay** (a 32 ms half-life is close to our EWMA; the extra state buys nothing at 100 Hz); **util_est / uclamp** (no per-task tracking in Rockbox's kernel and nothing to gain with two fixed roles); **EAS task placement** (placement is the owner's fixed decision); **per-core DVFS** (one clock).

---

## 14. Files, ownership and increments

### 14.1 Files

| File | Change | Owner |
|---|---|---|
| `firmware/export/pgov.h` | **Contract, committed before fan-out:** bands, floor and cap ids, `pgov_set_floor()`, `pgov_set_cap()`, `pgov_get_stats()`, the record struct | A (firmware-platform) |
| `firmware/pgov_core.c` | Pure policy (host-tested, simulated) | A |
| `firmware/pgov.c` | Glue: tick sampling, apply, PMU, checks, logs | A |
| `firmware/target/arm/pp/system-pp502x.c` | Point table, `DEV_TIMING1` rule, band 3 check, under `HAVE_PP_GOV` (upstream file; ledgered) | A |
| `firmware/target/arm/pp/thread-pp.c` | Idle stamps in `core_sleep()`, under `HAVE_PP_GOV` | A |
| `firmware/system.c` | `set_unboosted()`/`set_boosted()` route through the governor under `HAVE_PP_GOV` | A |
| `firmware/pll_idle.c` | Holders become floors | A |
| `firmware/drivers/pcf50605.c` | `pcf50605_set_core(code)` with the hard limits, and its read-back | A |
| `firmware/export/config/ipodvideo.h` | `HAVE_PP_GOV`, `HAVE_PP_GOV_DVS` (inside `HAVE_IPOD_UI`, target-only) | A |
| `apps/ipodui/clockpol{,_core}.c` | Floors instead of needs; the lit switch as a floor | A |
| `apps/ipodui/bootstamp.{c,h}` | The record field (one reviewed commit with its owner) | A with C (ui-fidelity) |
| `apps/ipodui/devmode.c`, `apps/lang/english.lang` | "Clock trace" and "Reset clock record" developer rows (`LANG_*` appended) | C writes, orchestrator merges |
| `tests/ppgov/`, `scripts/pgov_sim.py` | Host tests, the simulator and its traces | A (tests), T (tooling) for the script and `bench_ingest.py` |
| `docs/pp5022-reference/clocks-power.md`, `quirks.md`, `docs/data-paths.md` | Findings with tags, at each increment | A |
| `docs/hardware.md` budget table, ledgers | At close | orchestrator |

### 14.2 Increments (each ships with its safety net, no staged probes)

| # | Increment | Ships when |
|---|---|---|
| **G0** | Contract header, `pgov_core.c`, host tests, the simulator over the synthetic traces; the A/B report and the chosen constants | the host tests pass and the report is committed (no device) |
| **G1** | The governor live with bands 0 and 2 only (no voltage change, no band 3); idle accounting; floors; logs; trace ring | G0, `validate.sh --quick`; then normal use |
| **G2** | Band 1 and band 0 at `0xEB`: the voltage layer, the checks, the record, the watchdog | G1's logs show no regression for a week of use |
| **G3** | Band 3 behind the grade and its check | G2 |
| **Bench** | The `pgov` row (§10.2), once, with G2 or G3 on the unit | — |

G2 and G3 were built together as **stage 2** (§16): one increment, a shadow switch and an apply switch, instead of two staged increments.

---

## 15. Risks and open questions

### 15.1 Risks

| # | Risk | Mitigation / default |
|---|---|---|
| R1 | **The "flat inside a band" premise fails**: clock-tree and SDRAM-controller power scale with the clock while the cores sleep, or work is memory-bound, so 80 MHz idling costs more than 30 MHz working | The P arm and the g/m sweep (§5); rule 2 ships T only if the answer is grid-dependent, and the band logs allow a revisit |
| R2 | **Silent corruption at low voltage** (a marginal core flips a bit that reaches disk) | Low voltage only at ≤ 30 MHz, inside the rated window under either formula; storage writes at normal voltage; rotating code CRC; codec errors and underruns as triggers; arming before trying |
| R3 | **False strikes** (an unrelated crash or a battery pull) turn DVS off | Two strikes in five armed boots (Q-G2); a developer reset |
| R4 | **The first frame after a quiet stretch** starts at 30 MHz for ≤ ~11 ms while the voltage rises | Within the 33 ms budget [estimate]; the voltage dwell (2 s) keeps it to the first key after a pause; logged as `boost` with `vch` |
| R5 | **I2C contention** on a raise (the power thread reading the ADC holds the bus for ms) | Priority inheritance on the mutex; the raise runs outside the boost lock; the watchdog's raw write only when the mutex is free |
| R6 | **The COP cannot drive I2C** (after 19-9) | COP boosts post the raise to the CPU (§4.3) |
| R7 | **Band 3 timing outside ATA and the host port** (SDRAM at `0x0808`, the wheel, I2C) | The built-in check plus the caps; any fault → off on this unit; Q-G3 |
| R8 | **Flapping** (the PLL started many times a second outside ATA) | Dwell, EWMA, the simulator's 20-a-minute loss rule; `pgov tr pll=` in every summary |
| R9 | **Measurement lag** misses a decode burst | The pcmbuf rescue boosts (today's backstop); `clockpol`'s per-track floor at track start |
| R10 | **The PP5021C (5G)** has not run any of this | Same code path; the grade read and the checks decide per unit; the record keeps a failing unit at normal voltage |
| R11 | **The PLL start's energy** is unknown | e_start swept in the simulator; the ATA path already does ~30 starts a second |
| R12 | **The lit default moves from 30 MHz to band 0** (D1), against 18-5's "default 30 until a meter" | D1 is the owner's decision; the developer switch keeps 30 one press away; Q-G4 confirms |

### 15.2 Device-reality checklist

1. **Compiled features:** `HAVE_PP_GOV` and `HAVE_PP_GOV_DVS` are target-only inside `HAVE_IPOD_UI`; the UI simulator compiles none of it; the policy is tested on the host and in `tests/ppgov/sim`.
2. **Memory:** 0 B at runtime in release; ≤ 256 B of statics; the developer ring allocates at boot only, before the audio buffer.
3. **CPU:** ~10 instructions per core sleep, ~50 per tick, one power-thread pass per decision change; the CRC 1 ms a minute while low [estimate].
4. **Storage:** no new wake; the record rides the boot stamp's existing writes; the logs ride the storage-idle write.
5. **LCD:** no push changes; one cap bit around the push for band 3.
6. **Threads and locks:** the tick samples (read-only); the power thread applies under `cpufreq_cl`; a boosting thread raises the voltage outside the boost lock; the I2C mutex; single writers for every word.
7. **Splashes:** none; panics are logged.
8. **Persistence:** the record (§9.4) survives a reboot by design; a power-off mid-change leaves `DCDC1` restored by the next boot's `pcf50605_init()`.
9. **Scale:** independent of the library.
10. **Deferred work:** none.
11. **Stock firmware unchanged:** every upstream edit is under `HAVE_PP_GOV`; the stock build keeps upstream's `set_unboosted()`; two developer strings appended to `english.lang`.
12. **Existing paths doing the same job:** `unboosted_freq()`/`cpu_pll_hold()` (become floors; the governor is the only applier), `clockpol`'s needs (become floors), the lit switch (a floor), upstream's `cpu_idle_mode()` (honoured as band 0 except under the ATA/USB floors, as today), `cpu_boost()` (wrapped), 18-1's sleep refusal (the governor goes to band 0 at normal voltage first). The BCM2722's governor is a separate domain.
13. **Builder predicate:** not applicable.
14. **Hardware blocks:** §6.

### 15.3 Owner questions (each frozen at its default until answered)

- **Q-G1. Band 1's top:** 30 MHz only (default), or also try 48 MHz at `0xEB` once G2 has a clean week?
- **Q-G2. Unclean ends of armed boots:** one strike skips the next boot, two in five armed boots turn DVS off (default)? Or off at the first?
- **Q-G3. Band 3 in v1:** on where the grade allows, entered only on band-2 saturation or a declared floor, capped off during ATA DMA, port pushes and USB, at `0xEC` (default)? Or defer band 3?
- **Q-G4. The lit clock:** confirm that D1 supersedes 18-5's "lit default 30 MHz until a meter" (default: yes; the developer switch stays).
- **Q-G5. Sleep entry:** raise to normal voltage before 18-1's soft sleep (default), or allow the soft sleep at `0xEB`?
- **Q-G6. Clearing an "off" record:** only the developer row (default), or also a firmware update?

---

## 16. Stage 2: PLL off at band 0, 100 MHz, the core-voltage step (2026-10-26)

**Status (branch `p19-pgov2`, from `p19` `a25c29b79f`):** built, host-tested, not run on silicon. Two runtime layers, each behind its own marker in the system folder, on top of `pgov_on`: **`pgov2_on`** runs the stage-2 policy as a **shadow** (decisions and time per band logged, nothing applied); **`pgov2_apply`** as well **applies** it, with a watchdog that reverts to the stage-1 band. Compiled only in developer builds (`HAVE_PP_GOV_DVS` needs `HAVE_IPOD_UI_DEVMODE`): a release build has none of it (its `pgov.o` and `pgov_core.o` are byte-for-byte the G1 sizes, 1,884 B and 2,348 B of `.text`). This section supersedes G2/G3 of §14.2 as one increment with two switches; the policy (§3–§4, §9) is unchanged, this section is how it reaches the hardware.

Owner's three items and where each landed:

| Item | State before | Stage 2 adds |
|---|---|---|
| (a) PLL off at the bottom band, run from the 24 MHz crystal | **Already true since Fit-1 C6 and G1** [code]: `set_cpu_frequency()`'s default case selects the crystal, clears the PLL's enable bit and its power (`DEV_INIT2 &= ~INIT_PLL`); G1 selects band 0 lit or dark whenever the load fits (D1) | Band 0 at the low core voltage (`0xEB`), and the audit of what must not run from the PLL (§16.6) |
| (b) 100 MHz where the speed grade allows | The grade was read and logged only (`gov boot grade=`) | Band 3: the point, its gate, its caps, its built-in check, its record (§16.1, §16.4) |
| (c) a core-voltage step tied to the band | The policy's voltage layer existed in `pgov_core.c`, compiled for the host only | The apply layer (§16.2–§16.3), the PMU writes, the watchdog, the persistent record on disk (§16.5) |

### 16.1 The register sequences

Tags as in the header. "Ours" marks a choice; nothing in this section is [silicon] yet except the parts G1 and Fit-1 already run daily (the 24/30/80 MHz words and the lock times of §2.3).

**Clock up, from band 0 or between PLL points** (`set_cpu_frequency()`, IRAM, both cores stalled around each `CLOCK_SOURCE` write) [rockbox-derived order, `clocks-power.md` §4]:

1. `DEV_INIT2 |= INIT_PLL` (PLL power), `PLL_CONTROL |= 0x8800_0000` (enable).
2. Stall both cores; `CLOCK_SOURCE = 0x2000_2222` (all four selectors on the 24 MHz crystal); `DEV_TIMING1 = 0x0303`; release.
3. `PLL_CONTROL = word` (table below).
4. Poll `PLL_STATUS` bit 31 for lock. Rockbox's 30/80 MHz words: unbounded, as upstream (16–217 µs measured [silicon]). **Apple's words (48, 60, 100 MHz): bounded at 2 ms** [ours]; a miss selects the crystal (the default case: PLL off, `DEV_TIMING1 0x0303`) and counts in `pgov_pll_lock_fails()`; the governor then reverts (§16.5).
5. Stall; `DEV_TIMING1` for the new clock (raised **before** the faster clock is selected); `CLOCK_SOURCE = 0x2000_7777` (PLL); release.

| Point | `PLL_CONTROL` | M / N / post | VCO | `DEV_TIMING1` | Tag |
|---|---|---|---|---|---|
| 30 MHz | `0x8A22_0501` | 5 / 1 / 4 | 120 | `0x0303` | [rockbox-derived], [silicon] daily |
| 48 MHz | `0x8A22_600C` | 96 / 12 / 4 | 192 | `0x0404` | [apple] recipe and 90 ns rule |
| 60 MHz | `0x8A12_3C0C` | 60 / 12 / 2 | 120 | `0x0808` | [apple] recipe; timing floored at Rockbox's 80 MHz value [ours] |
| 80 MHz | `0x8A12_1403` | 20 / 3 / 2 | 160 | `0x0808` | [rockbox-derived], [silicon] daily |
| **100 MHz** | **`0x8A12_640C`** | 100 / 12 / 2 | 200 | **`0x0808`** | [apple] recipe (`mac-and-dvs.md` §3.1) and 90 ns rule (n = 8, §3.3) |

Apple programs a new PLL word with the post-divider at /8, waits a fixed 255 µs and then writes the real divider [apple]. **Not adopted**: we poll the lock as Rockbox does and bound the wait; the /8 soft start is the documented fallback if `lockf=` (§16.7) ever counts a miss on silicon.

**Clock down to band 0** [rockbox-derived]: `PLL_CONTROL |= 0x0800_0000`; stall; `CLOCK_SOURCE = 0x2000_2222`; `DEV_TIMING1 = 0x0303` (lowered **after** the slower clock is selected, inside the same stall); release; `PLL_CONTROL &= ~0x8000_0000` (PLL off); `DEV_INIT2 &= ~INIT_PLL` (PLL unpowered).

**Core voltage, `DCDC1`** (PMU register `0x1B` at I2C address `0x08`):

| Step | What | Tag |
|---|---|---|
| Read | `pcf50605_read(0x1B)` must give the mode bits `[7:5]` = `111` (what `pcf50605_init()` writes) | [rockbox-derived] layout `[7:5]` mode, `[4:0]` code; [apple] composes `code \| mode << 5` |
| Write | `pcf50605_write(0x1B, code)`, code `0xEC` (normal) or `0xEB` (one step down) only; never `DCDC2`–`DCDC4` | [ours], limits from `mac-and-dvs.md` §2.3 |
| Read back | the write counts only if the register reads back the code | [ours] |
| Raw path | the same write and read-back with the I2C block's registers directly (`pmu_raw_core_voltage()`, `pmu_raw-pp502x.c`: ≤ 4-byte transactions, quirk 22; every wait bounded by `USEC_TIMER`, 2 ms) | [rockbox-derived] registers |
| Voltage per code | `0xEC` 1.20 V; `0xEB` 1.15 V (formula A) or 1.175 V (B); both inside the PP5022's 1.14–1.32 V window | **[hypothesis]** (§3.2) |

**What Apple does that we do not.** Apple never writes `DCDC1` on a PP5022 [apple, `mac-and-dvs.md` §2.4]: the voltage step is beyond Apple's envelope, so there is no Apple sequence to follow for the PP's own rail. On its non-PP5022 path Apple writes `DCDC4 = 0x30` with every core-voltage change [apple]; its meaning is unknown ([hypothesis]: a ramp-rate or current-limit field). **We do not write `DCDC4`**: one step of 25–50 mV is within any converter's normal regulation, and an unknown field is not written on a guess. If the bench row shows a fault at the step, `DCDC4` is the first suspect, recorded in Q-G7.

### 16.2 Ordering rules and settle times

| Direction | Order | Settle | Tag |
|---|---|---|---|
| **Up** (voltage and clock) | 1. `DCDC1 = 0xEC`, read back; 2. the settle; 3. the clock up (`DEV_TIMING1` raised inside it, at 24 MHz, before the faster clock) | **10 ms** from the read-back, timed by `USEC_TIMER`, before any clock above band 1 (30 MHz) | [ours]; Apple's BCM2722 driver waits 10 ms after each regulator change [apple, `brief-map-power.md` §4]; the PCF50605's own slew is unknown [hypothesis] |
| **Down** (clock and voltage) | 1. the clock down to ≤ 30 MHz (`DEV_TIMING1` lowered with it); 2. the low code recorded (below); 3. `DCDC1 = 0xEB`, read back | none (lowering the rail under a slow clock is the safe direction) | [ours] |
| Clock only, inside a voltage | `set_cpu_frequency()`'s own order (§16.1) | the PLL lock (16–217 µs) | [rockbox-derived], [silicon] |
| A raise from band 0 | 24 → 30 MHz at once (30 MHz is within the low code's ceiling), then the settle, then the target | – | [ours] |

**Where the order is enforced (three places, each sufficient for its own path):**

1. **`pgov_hw_next()`** (`pgov_core.c`, pure, host-tested): the power thread asks it for one step at a time (voltage up / wait / clock / voltage down) toward the decision and does exactly that step. It never returns a clock above what the rail carries at that instant, never a voltage down while the clock is above 30 MHz.
2. **`pgov_clamp_hz()`** inside `set_freq_tracked()` (`system.c`): every clock change of the boost, holder and governor paths passes this one choke point under the boost lock. In apply mode it cuts any request to the rail's ceiling (30 MHz while low or settling; 80 MHz under band 3's caps). A path that bypasses the governor (upstream's `cpu_idle_mode()` while USB is connected) is clamped too, and the clamp books the raise (§16.6).
3. **`cpu_pgov_lower()`** (`system.c`): the low code is recorded in the apply layer's state **under the boost lock and before the PMU write**, only while the clock is ≤ 30 MHz and nothing boosts. A boost that lands between that point and the write is clamped to 30 MHz. A raise is recorded only **after** its read-back. So the state any reader trusts is always the lowest the rail may be at.

### 16.3 The apply layer's state and concurrency

| Datum | Writer | Readers | Rule |
|---|---|---|---|
| `core2` (the stage-2 decision) | the CPU's tick (`pgov_core_step()`), boost/floor paths under the boost lock with IRQs off | the power thread | as G1's `core` |
| `hw.vcode`, `hw.v_at_us` (the rail) | the power thread (raise after read-back; lower under the boost lock before the write), the tick's watchdog (raw raise) | `pgov_clamp_hz()` under the boost lock, the tick | lowest-possible semantics (§16.2) |
| `hw.mhz` | refreshed from `cpu_frequency` at each step | `pgov_hw_next()` | – |
| `port_busy` | `lcd_awake()` (true, before the port's first access), `lcd_sleep()` (false, after the BCM2722 is powered down) | the clamp, the tick's caps | set before use, cleared after |
| the record (`pgov.dat`) | the storage thread's idle callback, or `system_flush()` at a clean shutdown | the shell at boot | written only when changed, only while the disk spins anyway |

**Cores.** The governor applies from the CPU's power thread only. A boost from either core gets its point from the decision and the clamp (both pure state); it never drives the PMU's bus. A fault reported from any thread or core (`pgov2_fault()`) only marks the decision and kicks the power thread; the raise is the power thread's, or the tick's watchdog's. The COP never runs I2C [hypothesis carried from §4.3, R6].

### 16.4 The speed grade

- **Detection:** `STRAP_OPT_A` (`0x7000_0008`) bits `[6:5]`, read at boot: 0 → 133, 1 → 100, 2 → 80, 3 → 66 MHz, Apple's own table in its PP5022 clock driver's constructor [apple, `mac-and-dvs.md` §3.2]. That the field is the die's rated maximum is **[hypothesis]**: it is how Apple's driver uses it, not a datasheet statement.
- **Our unit's value: unknown.** G1's boot line (`gov boot grade=G max=M strap=0x…`) already logs it; the first log from a unit with `pgov_on` answers it. The stage-2 boot line repeats it (`gov s2 boot … grade=`).
- **Safe default:** band 3 does not exist unless **all** of: the grade is 0 or 1; the strap does not read all ones (no such register); the record does not say band 3 failed on this unit; and, at its first entry in each boot, the built-in check passes. A unit graded 80 or 66 MHz never runs 100 MHz, and that is the design, not a fault.
- **The built-in check** (first entry per boot): a CRC-32 over 32 KB of read-only code (the 32 KB-aligned block holding `pgov_core_step`, `.text` or the `.rodata` after it) is taken just before the switch; at 100 MHz the 10,000-iteration `subs`/`bne` loop is timed with IRQs off against `USEC_TIMER` (380–420 µs passes; 501 µs at 80 MHz [external, siggi `08`]; four cycles per iteration from the cache) and the CRC is taken again. A mismatch, a time outside the window or a lock miss: back to 80 MHz, stage 1 for the boot, and `b3 = off` in the record for good (until the developer reset). A pass marks `b3 = trial`.
- **Caps (band 3 left at once, or not entered):** the BCM2722's host port in use (the display awake: `lcd_tick` reads `BCMA_COMMAND` every tick while it is on, and the push paths' port timing has only been seen at ≤ 80 MHz [hypothesis that it would fail]), an ATA DMA transfer (`CPU_PLL_HOLD_ATA`), USB (`CPU_PLL_HOLD_USB`). **So band 3 runs only while the display sleeps**: dark decode that saturates 80 MHz, a dark library build. The cap is synchronous: `lcd_awake()` calls `pgov_port_cap(true)` before its first port access, which re-clamps the clock under the boost lock; `cpu_pll_hold()`'s ATA/USB bits are clamped in the same choke point.
- **Voltage at 100 MHz:** `0xEC`. A step up (`0xED`) is not in this increment (Q-G3).

### 16.5 Fail-safes: the watchdog, the revert and the record

**Revert target.** The stage-1 policy keeps running on every sample in apply mode, so its decision is always current. A revert sets stage 2 back to a shadow (DVS off, band 3 off if that was the cause) and the power thread applies stage 1's decision at normal voltage for the rest of the boot.

| Trigger | Detected by | Action |
|---|---|---|
| **A raise owed and not confirmed in 250 ms** (the decision wants `0xEC`, the rail is not confirmed there: the power thread starved, hung or failed) | the CPU's tick, every 10 ms (`pgov_dog_step()`, pure, host-tested) | **the tick never writes the PMU** (review B2): it reverts the policy (normal voltage decided, band 1 gone), books `fb=DOG` and kicks the power thread, which raises under the I2C lock and applies stage 1. Until the rail is confirmed, the clamp holds every clock at band 1's top, which the low code carries, so a hung power thread costs speed, never correctness |
| A `DCDC1` read-back mismatch | the write path | raise (driver, else raw, both under the I2C lock), revert (`READBACK`); a raise that still fails is logged (`rail unconfirmed`) and the clamp holds band 1's top: no blind reset (review B2c) |
| A PLL lock miss at 48/60 MHz | the bounded wait | crystal at once; revert for the boot (`LOCK`, not recorded: not a voltage fault) |
| Band 3's check or a 100 MHz lock miss | §16.4 | back to 80 MHz, revert, `b3 = off` |
| An underrun in a 24 MHz playback window | `clockpol`'s minute judge | `pgov2_fault(UNDERRUN)`: the decision raises at once, the power thread reverts |
| A planned end (power-off, reboot, ROLO) | `sys_poweroff()`/`sys_reboot()`/`rolo_load()` → `pgov2_shutdown()` | disarm, **the rail raised synchronously** (`pgov2_rail_normal()`), the record's "armed" cleared, written by `system_flush()`; `cancel_shutdown()` lets stage 2 arm again |
| **Any restart** (review S1: `system_reboot()` from the bench's watchdog, the SDK, a panic, an exception) | `system_reboot()` itself, first thing | `pgov2_rail_normal()`: a thread with IRQs on writes `0xEC` with the driver under the I2C lock; a panic or exception path (IRQs off, nothing else running) writes it raw. Apple's firmware and our boot never start on the low rail |
| **Any storage command** (review S2: PIO and DMA, flash and hard drive) | `ata.c`'s mutex, wrapped | `pgov2_storage()`: the rail raised before the command if low (synchronously, under the I2C lock), band 3 capped, no lowering while one is in flight (VDOWN's re-check) |
| Soft sleep entry | `pgov_settle()` | normal voltage before the sleep (Q-G5's default), re-armed by the power thread after the wake |

**The record (`pgov.dat`, 16 B: magic, version, §9.4's fields, a checksum).** Not in the boot stamp: the stamp is removed and rewritten by the library's code on its own schedule, and its owner is another package; a file of our own written only when it changes is §9.4's fallback. The rules:

- **Arming before trying:** in apply mode the voltage may go low only after `armed = 1` **is on disk**. The write is queued on the storage-idle callback, so it rides a spin the disk makes anyway: on flash at the next refill, on a hard drive at its next spin (the low voltage simply starts later). No new wake.
- `armed` is cleared at a planned power-off or reboot (`system_flush()` runs the callback) and when stage 2 is switched off. **A boot that finds `armed = 1`** counts a strike (§9.4: one skips the next boot, two in five armed boots turn DVS off).
- A runtime trigger writes `dvs = off` with its code at the next disk spin.
- A read that fails (missing, short, bad checksum) starts a fresh record; the shadow mode never writes it.

**Not built in this increment** (each named in §9.3): the rotating `.text` CRC while low and the codec-error trigger (they need edits in the codec thread); the watchdog covers a hang, the underrun trigger covers audio, and every restart raises the rail first (above).

### 16.6 Interactions

**`cpu_boost()`.** Unchanged meaning (§7): band 2's top now. While the rail is low, a boost gets 30 MHz at once (the clamp), the raise is booked and the power thread (priority `PRIORITY_SYSTEM`) is kicked: one PMU write and read-back (~0.5–1 ms [estimate]), the 10 ms settle, then 80 MHz. A first frame after a quiet stretch therefore starts at 30 MHz for ~11 ms (R4); the shadow's `rse=` counts how often that would happen before anything is applied.

**The COP.** Its boosts take the same clamp; it never touches the PMU. Its idle word feeds both policies unchanged.

**Audio DMA and the I2S clock.** The WM8758 is the I2S master; its MCLK is the PP5022's device clock taken from the 24 MHz crystal, and the PP's I2S block is a slave [rockbox-derived, §6.3]. PCM refill is FIQ-driven from the I2S FIFO. **Nothing in the audio path runs from the PLL**, so band 0 (PLL off) and every PLL retune leave the sample clock alone; Fit-1 C6's dark runs already crossed 18,310 PLL power-ups in a session with no strike [silicon, `darkplay-1614`]. The voltage step's effect on the I2S block (same die) is the open item: the underrun trigger and the bench row's ears (§16.8).

**What must not run from the PLL in band 0, and what needs it:**

| Block | Clock | In band 0 | Tag |
|---|---|---|---|
| `USEC_TIMER`, the tick | 1 MHz from the crystal | runs | [rockbox-derived] |
| WM8758 MCLK, I2S | crystal device clock | runs | [rockbox-derived] |
| PMU I2C (battery ADC, our `DCDC1` writes) | the I2C block's divider; its source [hypothesis] | runs: the power thread reads the battery ADC over it at 24 MHz throughout dark play since Fit-1 C6 | [code] |
| Clickwheel serial | – | runs (dark and lit band 0 since Fit-1 / G1) | [silicon, daily] |
| ATA UDMA | timing tables assume ≥ 30 MHz | **needs the PLL**: `CPU_PLL_HOLD_ATA` floors band 1 for every transfer; capped at 80 MHz for band 3 | [rockbox-derived] (UDMA2 at 30 MHz) |
| USB (ARC controller) | undocumented | **needs the PLL**: `CPU_PLL_HOLD_USB` floors band 1; capped at 80 MHz | [hypothesis] |
| BCM2722 host port | PP bus cycles | runs at 24 MHz lit (D1, `lit-24mhz.md`); capped at 80 MHz | [code]; the 100 MHz cap [hypothesis] |
| The second PLL (`0x6000_6048`) | – | never touched | [apple] exists; unused by us |

### 16.7 Logging (tag `gov`, prefix `s2`, ≤ 62 characters)

| Line | When |
|---|---|
| `s2 boot mode=shadow\|apply grade=G b1=0/1 b3=0/1 dcdc1=ec` | at the marker read |
| `s2 rec dvs=S b3=S code=N hist=H strike=0/1 ok=0/1` | same; `strike=1` means the last armed window ended uncleanly |
| `s2 arm=0/1 mv=N` | at each arm/disarm (§9.2's conditions: ≥ 60 s up, no USB, ≥ 3,500 mV, the record) |
| `s2 band=B/MHz v=eb\|ec from=M why=… util=%` | at each stage-2 decision change (shadow and apply alike; `ui_log` folds repeats within 1 s) |
| `s2 b3 loop_us=N crc=ok\|bad` | band 3's first entry |
| `s2 fb why=N` | at each fallback the power thread handles (the code, `PGOV_FB_*`); the tick's watchdog cannot log, so its revert shows as `dog=` and `fb=N/5` in the next `s2 tr` |
| `s2 sum b0= b1= b2= b3= vlow=` (‰), `s2 tr vch= rse= fb=N/code dog= lockf=` | with every `gov sum`/`clk stats` line (5 min) and at power-off |

### 16.8 Measurements (`arm-none-eabi-size`, 2026-10-26)

| Build | Item | Before (`a25c29b79f`) | After | Change |
|---|---|---|---|---|
| `fw-ui` (developer) | `rockbox.elf` text / data / bss | 1,111,292 / 9,648 / 334,488 | 1,119,204 / 9,656 / 334,952 | **+7,912 / +8 / +464 B** |
| `fw-ui` | `_end` (resident image end) | `0x153E10` | `0x155E10` | +8 KB (alignment granule) |
| `fw-ui` | `pgov.o` text / bss | 1,884 / 98 | 6,959 / 217 | +5,075 / +119 B (statics ≤ 40 B each) |
| `fw-ui` | `pgov_core.o` text | 2,348 | 3,932 | +1,584 B (the voltage layer and the apply layer now compiled) |
| `fw-ui` | `set_cpu_frequency` (IRAM) | 664 B | 860 B | **+196 B IRAM** (the 100 MHz point and the bounded lock) |
| `fw-rel` (release) | `pgov.o`, `pgov_core.o` | 1,884, 2,348 | 1,884, 2,348 | **0**; no stage-2 symbol in the ELF |

Over §12's 4 KB code budget in developer builds (stage 2 is ~6.7 KB of glue and policy, half of it log formatting and the apply layer's checks); release is untouched. The 196 B of IRAM in developer builds is the one cost on the codecs' memory; if stage 2 ships to release it is the first thing to trim (one bounded-wait routine for all points instead of two).

No runtime RAM: no allocation, no buffer. CPU: one more policy step per tick (~the G1 step), one watchdog check per tick in apply mode; the power thread's apply runs only on a decision change.

### 16.9 Risks (stage 2)

| # | Risk | Mitigation / default |
|---|---|---|
| S2-1 | The `DCDC1` formula is unknown, so `0xEB` may be 1.15 V, at the window's floor | One step only, only at ≤ 30 MHz; the watchdog, read-back and record; the meter reading of `mac-and-dvs.md` §5.4 step 0 stays optional |
| S2-2 | The settle (10 ms) is borrowed from the BCM2722's driver, not measured on this PMU | The bench row's boost-from-dark steps exercise it hundreds of times; a fault after a raise points here first |
| S2-3 | `DCDC4` (Apple's companion write on other chips) is not written | Q-G7; first suspect on any step fault |
| S2-4 | ~~A storage command starting at the low voltage~~ | **Closed (review S2):** every command under `ata.c`'s mutex raises the rail first, synchronously |
| S2-5 | ~~The tick's raw PMU write cutting into a transaction~~ | **Closed (review B2):** the tick never writes the PMU; the COP does use this bus (the WM8758's volume writes), so a raw write is only made with the I2C lock held or with nothing else running (a panic) |
| S2-6 | ~~A reboot as the watchdog's last resort~~ | **Closed (review B2c):** no reset from the watchdog; the clamp holds band 1's top on an unconfirmed rail |
| S2-11 | A power-thread stall leaves a boost at 30 MHz while the rail is unconfirmed | Speed only; the clock follows the moment the raise is confirmed (`cpu_pgov_reboost()`, review S3) |
| S2-7 | 100 MHz SDRAM and peripheral timing (`DEV_TIMING1 0x0808`, Apple's own value at 100 MHz) | The built-in check, the caps (display asleep, no ATA, no USB), `b3 = off` for good on a failure |
| S2-8 | The strap's meaning | A wrong reading only ever allows the check to run; the check decides |
| S2-9 | An underrun with an unrelated cause turns DVS off for good | Rare by design (24 MHz windows only); the developer reset clears it (Q-G6) |
| S2-10 | Band 3 never runs lit, so it only helps dark saturation and dark library builds | Intended for v1; the port's 100 MHz timing is a separate bench question (Q-G8) |

### 16.10 What needs silicon

Each is [hypothesis] until a log or the bench row answers it:

1. The unit's speed grade (`gov boot grade=`, already logged by G1).
2. `DCDC1` reads `0xEC` at boot with mode bits `111` (`s2 boot dcdc1=`).
3. The PP5022 runs correctly at `0xEB` at 24 and 30 MHz (decode, UI, storage, I2C): no fallback across the bench row and a week of logs.
4. The 10 ms settle is enough after a raise (no fault after `rse=` events).
5. Apple's 100 MHz word locks within 2 ms (`lockf=0`) and the check passes (`s2 b3 loop_us=380–420 crc=ok`).
6. No click or underrun at the voltage step (the bench row's ears; no `clk strike` and no `fb=` with code 4).
7. The power-thread latency of a raise under load (the dog never fires: `dog=0`).

### 16.11 Device-reality checklist (stage 2)

1. **Compiled features:** `HAVE_PP_GOV_DVS` only in developer target builds (`HAVE_IPOD_UI_DEVMODE`, `HAVE_PP_GOV`); the simulator compiles none of it (host tests cover the logic).
2. **Memory during playback:** 0 B allocated; +119 B of statics in `pgov.o`, none over 40 B.
3. **CPU at 50k:** independent of the library.
4. **Storage wake-ups:** none new: the record rides the storage-idle callback and `system_flush()`.
5. **LCD:** no push changes; one call in `lcd_awake()` and one in `lcd_sleep()`.
6. **Threads and locks:** the tick samples and watches; the power thread applies; the boost lock guards the clamp and the lowering; the I2C mutex for PMU writes; no new thread.
7. **Splashes:** none; every outcome is a `gov s2` log line.
8. **Persistence:** `pgov.dat` (16 B), written only on change; the shadow never writes it.
9. **Scale:** n/a.
10. **Stock firmware unchanged:** every upstream-file edit is under `HAVE_PP_GOV_DVS` (`system.c`, `powermgmt.c`, `i2c-pp.{c,h}`, `system-pp502x.c`); the stock build compiles none of it. Apple's firmware (hold MENU) untouched; no bootloader change.
11. **`cpu_boost` balanced:** no new boost or release; the clamp only lowers a requested clock.
12. **Existing paths doing the same job:** `pgov_set_core_voltage()` (G1's unbuilt placeholder) replaced by `set_core_voltage()` in the apply layer; `pmu_raw` gained one function beside the panic path's.

### 16.12 Owner questions added

- **Q-G7. `DCDC4`:** leave it untouched (default), or mirror Apple's `0x30` companion write from its other chips if the step ever faults?
- **Q-G8. Band 3 lit:** keep 100 MHz to the dark display (default), or add a lit port-timing bench row later?
- ~~Q-G9~~ (ATA at low voltage): settled by review S2, every storage command at the normal voltage.

### 16.13 Adversarial review fixes (2026-10-26, on `83e7bab119`)

The review found stage 2 safe to merge as a shadow; apply mode was held until these were fixed. Each is fixed on `p19-pgov2` and covered by a host test where the logic is pure.

| # | Finding | Fix | Test |
|---|---|---|---|
| B1 | A boost on the COP (FE_COP: `anim.c`, `nowplaying.c`, `search.c`) read a stale cached `hw` and could select 80 MHz on `0xEB`; it also wrote `core2` into its own cache | The decisions (`core`, `core2`), the rail state (`hw`), `mode2`, `rec_rw`, the port and storage flags and the fault word are `XCORE_BSS`/`XCORE_DATA` (uncached; `scripts/xcore_objects.txt` lists them). **A COP boost never writes the policy**: it selects at most band 2's top within what the rail carries (`pgov_cop_boost_mhz()`), books `cop_boost`, and the CPU's tick applies the policy's boost. The same holds for G1's `core` (its COP boosts wrote it too). Belt and braces: `pgov2_apply` is refused (shadow instead, logged) while the UI split is, or is set to be, `cop` | `test_cop_reader`: over 200k samples a COP boost from the live state never exceeds the rail's ceiling; the same reader on a stale snapshot does (negative control) |
| B2a | The tick's raw `0xEC` could land between VDOWN's read and write: `0xEB` written while `hw` said normal | The tick never writes the PMU. VDOWN holds the I2C lock across the re-check, the write and the read-back, and re-checks inside it: still applying, the rail recorded low, the decision still low, no storage command, the clock ≤ 30 MHz (`pgov_vdown_recheck()`); a refusal un-records the lowering | `test_vdown_interleave`: each interleaving event makes the re-check refuse |
| B2b | The COP drives the same I2C bus (`setvol` → `wmcodec_write` → `pp_i2c_send`); `i2c_bus_free()` read the mutex owner without its corelock | `i2c_bus_free()` removed; raw writes only under `i2c_lock()` or with nothing else running | – |
| B2c | The watchdog's reboot was a `DEV_RS` reset with no flush and left `DCDC1` at `0xEB` for Apple's firmware | No reboot from the watchdog (above); every restart raises the rail first (S1) | `test_dog` (fires once, never a reset code) |
| S1 | Reboot paths (`system_reboot()` callers: the bench's watchdog, the SDK, panic, the exception handler; ROLO; `pgov2_shutdown()` only kicked the power thread) could leave `0xEB` | `system_reboot()` calls `pgov2_rail_normal()` first (locked write from a thread, raw from a panic or exception); `rolo_load()` and `pgov2_shutdown()` raise synchronously | `test_reboot_raise` |
| S2 | PIO ATA was unguarded (`CPU_PLL_HOLD_ATA` is DMA-only) | `ata.c` wraps its mutex: every command, PIO or DMA, flash or hard drive, raises the rail first and caps band 3 (`pgov2_storage()`) | covered by `test_vdown_interleave` (the storage flag) |
| S3 | A boost stayed at 30 MHz after the raise when `gov_on()` was false (`cpu_idle_mode()`, USB) or the boost landed inside the settle | The clamp kicks the power thread whenever it cuts a clock; the apply pass ends with `cpu_pgov_reboost()` | – |
| S4 | The device test, the job driver and the boot switch drive the port with `port_busy` false | Band 3 is also capped while the BCM2722 is powered (`GPO32` bit 14), and each of those owners calls `pgov_port_cap(true)` when it takes the port | – |
| nits | `pmu_raw_core_voltage()` unguarded; the record written in place; `shutting` never cleared | Guarded by `HAVE_PP_GOV_DVS`; the record is written to `pgov.tmp`, synced, closed (checked) and renamed over `pgov.dat`; `cancel_shutdown()` clears `shutting` | `test_stage2_misc` (the format) |

Sizes after the fixes (`arm-none-eabi-size`, against `a25c29b79f`): developer `fw-ui` text +8,764 B, `_end` +8,288 B, `pgov.o` 7,691 B text and 239 B of statics (uncached); `set_cpu_frequency` +196 B and `system_reboot` +8 B of IRAM; release `fw-rel` +152 B text (G1's COP-safe boost), no stage-2 symbol; stock `fw` none. Builds: `fw-ui`, `fw-rel`, `fw-ui-mod`, `sim-ui`, `fw`; `scripts/xcore_map_check.py` passes on `fw-ui`, `fw-rel`, `fw-ui-mod`.

**Re-review (on `509250ee8e`): safe for an apply-mode bench.** Follow-ups landed: **R1** `panic_shell_exit()` raises the rail first (raw, IRQs off), so a boot-guard halt on the charger no longer leaves `0xEB` for Apple's recovery; **R2** a failed `rolo_load()` (`rolo_error()`) calls `pgov2_shutdown_cancelled()`; **R3** at boot a sound `pgov.tmp` is preferred over `pgov.dat` (the write is temp, sync, remove, rename, and a broken write never checks), so a cut between remove and rename keeps the strike history (`pgov_rec_pick()`, `test_rec_pick`); **R4** an apply pass that ends inside a settle sleeps a tick instead of re-boosting in a spin (`pgov_hw_settle_left()`, `test_settle_left`); **nit** the raw write on a COP restart is skipped while the I2C mutex is held (`i2c_held()`).
