# The playback path: DAC, volume, EQ, mixers and outputs

*Chapters WM-5 to WM-9. The order follows the signal: I2S in, digital processing, DAC, analogue mixer, output driver. Register fields: `registers.md`.*

```
DACDAT ─► digital volume (R11/R12) ─► 5-band EQ + 3D (R18–R22, R41) ─► limiter / boost (R24/R25)
       ─► interpolation filter, 64× or 128× (R10) ─► sigma-delta DAC ─► L/R output mixer (R50/R51)
       ─► OUT1 PGA −57…+6 dB (R52/R53) ─► LOUT1/ROUT1 ─► headphone socket
          └► OUT2 PGA (R54/R55) ─► LOUT2/ROUT2   (off on our unit)
          └► OUT3/OUT4 mixers (R56/R57)          (muted, off)
```

The order of the digital blocks is the datasheet's Figure 22 as far as the text describes it ("digital volume control, graphic equaliser, a digital peak limiter, sigma-delta modulation"). [datasheet] Whether the volume comes before or after the EQ matters for headroom: the EQ precut below assumes the DAC volume attenuates before the EQ can clip. [hypothesis] (`open-questions.md` 13)

## 1. DAC and its filters (WM-5)

- **Interpolation filter:** fixed, linear phase; pass band to about 0.454 fs, stop band about −55 dB, group delay 29/fs. **No filter-shape choice and no de-emphasis exist on this part.** [datasheet, DIGITAL FILTER CHARACTERISTICS] De-emphasis for old CDs, if ever wanted, has to be a software biquad.
- **The filters process 24 bits** internally; the multi-bit sigma-delta DAC uses dynamic element matching. [datasheet] We feed 16-bit words (`bring-up.md` §3).
- **Oversampling** (R10 bit 3, `DACOSR128`): 0 = 64×, 1 = 128×. We run 128× at 48 kHz and below; at 88.2/96 kHz 64× (p083-dacosr64) unless Settings > Audiophile > Hi-Res 128x is on and that rate passed its 128× self-test (0.9.0, `dac_set_osr128()`; a change while playing goes under a soft-mute ramp, assumed done in ~30 ms, not timed on silicon). The datasheet calls 64× "lowest power & best performance" in the DAC table, while the ADC text says 64× gives "a slight decrease in noise performance": the two disagree (quirk 23). The P15 choice of 128× was "fidelity first", not a measurement. [datasheet] [rockbox-derived] (`open-questions.md` 5)
- **Sample-rate coefficients** come from R7's SR field (`bring-up.md` §2.4).
- **88.2/96 kHz at 128×** (p090-osr128, #439): the clock margin and the bench switches are in §1.1 below.
- **Polarity:** `DACLPOL`/`DACRPOL` invert a channel. Unused. [datasheet]
- **Full scale** at the outputs is AVDD1/3.3 V rms: about 0.76 V rms at our 2.5 V supply, 0.91 V rms at 3.0 V. The DAC-to-line-out SNR is 96 dB A-weighted at 2.5 V and 100 dB at 3.0 V. [datasheet, Electrical Characteristics] The supply question is in `power.md` §3.

### 1.1 88.2 and 96 kHz: the clock margin, and the switches for 128× (p090-osr128, #439)

The owner heard a faint crackle only in the silent tail of a 16-bit/96 kHz track with Hi-Res 128x on; the logs showed no feed gap (`pcm fiq rows e=0`) [silicon, owner, 2026-11-16]. What we programme at 88.2/96 kHz, from `audiohw_set_frequency()` (`freq_setups[]`, `pll_setups[]`), against the datasheet's limits (Rev 4.4: System Clock Timing note 1, Audio Interface Timing, the PLL section and Table 49):

| Clock | 88.2 kHz | 96 kHz | Datasheet limit | Margin at 96 kHz | Tag |
|---|---|---|---|---|---|
| MCLK pin (from the PP) | 24 MHz (41.7 ns) | 24 MHz | ≥ 20 ns cycle into the PLL | 2.08× inside | [datasheet] [rockbox-derived] |
| PLL input (`PLLPRESCALE` ÷2) | 12 MHz | 12 MHz | 8–50 MHz input | inside | [datasheet] |
| PLL ratio R (N.K) | 7.5264 (N 7, K `0x86C226`) | 8.192 (N 8, K `0x3126E9`) | 5 < N < 13; "best" f2 near 90 MHz, N 8 | inside; Table 49's 12.288 MHz row | [datasheet] |
| f2 (the PLL itself) | 90.3168 MHz | 98.304 MHz | the datasheet's own examples | the same as at 44.1/48 kHz | [datasheet] |
| PLL out (f2 ÷ 4) | 22.5792 MHz | 24.576 MHz | — | — | [datasheet] |
| **SYSCLK** (`MCLKDIV` ÷1) | **22.5792 MHz** | **24.576 MHz** | **≤ 12.288 MHz** ("no greater than"; 81.38 ns at 256 fs) | **2.00× over** (1.84× at 88.2) | [datasheet] |
| DSP core, low-power (`M128ENB` 0, "half of the normal rate") | 11.29 MHz | 12.288 MHz | a full-rate core at 48 kHz runs 12.288 MHz | at the rated 48 kHz clock | [datasheet]; core = SYSCLK or SYSCLK/2 is our [hypothesis] |
| DSP core, full rate (`M128ENB` 1) | 22.58 MHz | 24.576 MHz | as above | 2.00× over | [hypothesis] |
| DAC modulator at 128× (fs × 128) | 11.29 MHz | 12.288 MHz | 6.144 MHz (48 kHz × 128) | 2.00× over | [estimate] |
| DAC modulator at 64× | 5.645 MHz | 6.144 MHz | 6.144 MHz | at the rated rate | [estimate] |
| BCLK (`BCLKDIV` ÷2, 128 fs) | 11.29 MHz | 12.288 MHz (81.4 ns) | slave tBCY ≥ 50 ns (20 MHz); no master-mode figure | 0.61 of the slave limit | [datasheet] |
| LRCK | 88.2 kHz | 96 kHz | the rate list stops at 48 kHz | outside the listed rates | [datasheet] |

**No legal setting has more margin.** Every audio clock is SYSCLK / 256 in master and slave mode alike (LRC is SYSCLK / 256; `BCLKDIV` divides BCLK only), so any 96 kHz output needs a 24.576 MHz SYSCLK, and the PLL already sits on the datasheet's own rows. What is left to choose is how much of the chip runs at twice its rated clock: 64× keeps the modulator at its 48 kHz rate, `M128ENB` = 0 keeps the DSP core at a full-rate core's 48 kHz clock, the EQ guard (§4.4) keeps the EQ's coefficients at rest. [datasheet] [ours] (quirk 35)

**Cycles per sample do not change with the rate.** Everything is clocked from SYSCLK = 256 fs, so the half-rate core gets 128 SYSCLK cycles per sample at 96 kHz, exactly as at 48 kHz, where it has always worked. The hypothesis that the half-rate core "runs out of time" at 96 kHz × 128× is not what the arithmetic predicts; an absolute-speed (gate timing) limit is, and `M128ENB` = 1 makes that worse (the core at 24.576 MHz), not better. Which way silicon goes is open question 25 (bench b17). **Stereo Width on at 88.2/96 kHz already means the full-rate DSP** (the 3D stage holds `M128ENB`, §4.3), so a unit playing Width at hi-res already runs the b17 row's full-rate arm. [estimate] [hypothesis]

**The switches** (developer only, not saved, each read back through `dac_get_stats()`; release defaults after b17):

| Switch | Default | What it does | Read back (`struct dac_stats`) |
|---|---|---|---|
| `dac_set_m128_hirate(bool)` | on (`DAC_M128_HIRATE_DEFAULT 1`, b18: half rate crackled in the tail, full rate clean) | `M128ENB` = 1 while the output is 88.2/96 kHz at 128×: a third user of #265's OR (quirk 28), so it never clears the bit under EQ band 4 or the 3D stage | `.m128_hr_allow` (the switch), `.m128` (the bit), `.osr`, `.rate_hz` |
| `dac_set_hirate_eq_guard(bool)` | off (`DAC_HR_GUARD_DEFAULT 0`) | the codec EQ held flat at 88.2/96 kHz on every path (§4.4) | `.hr_guard` (the switch), `.hr_bypass` (in force now), `.hw_eq`, `.precut_db` |
| `dac_set_amute(bool)` | off (`DAC_AMUTE_DEFAULT 0`, #327) | R10 `AMUTE` (§2) | `.amute` |
| `dac_set_width3d(int 0..15)` | the saved Stereo Width | R41 depth, `M128ENB` held while non-zero | `.width3d`, `.m128` |

A switch that moves a register while music plays at 88.2/96 kHz goes in under a soft-mute ramp (`dac_hr_switch()`, Hi-Res 128x's ~30 ms), so the bench can flip arms without a stop. The datasheet gives no write-order or mute requirement for `M128ENB` (Table 54 is the whole of it) [datasheet]; we mute anyway because the DSP changes mode under the signal [ours]. Host test: `tests/dacosr/test_hr.c` (75 checks, six red runs in `tests/dacosr/run.sh`).

**The Hi-Res 128x self-test** (`apps/audiophile.c` `ap_try()`) plays 4 s a rate since p090-osr128 (was 0.6 s): 2 s of a ±1 LSB square (−96 dBFS, never exact zeros, so `AMUTE` stays out and the modulator idles on the smallest signal it can get), then 2 s of exact silence. A rate fails on a drain rate more than 3 % off or on a 20 ms gap between DMA pieces (a stopped clock; b16's late FIQs were under 1 ms); the pure parts are `audiophile_test_ms()` and `audiophile_rate_test_ok()` (`tests/audiophile`). The log line: `high rate test <Hz> 128x 4 s: <n> pieces, <Hz> measured, gap <us> us, e=<FIFO empties>: pass|FAIL`. **The codec gives the PP nothing to observe of its analogue output**: the control bus is write-only and no status pin is wired (`GPIO1` could report PLL lock or temperature, open question 3), so a crackle cannot be measured in firmware; the test can only see that the clocks keep running. [ours]

## 2. Mutes

- **Soft mute** (R10 bit 6): ramps the digital gain to zero and back to the volume setting. **Bit 6 = 0 plays, bit 6 = 1 mutes** on our unit [silicon: music plays with `0x008`, every session] [rockbox-derived]. The datasheet's Table 21 prints the opposite, and its text and reset value disagree with each other too (quirk 10). The ramp time is not published. We mute on pause, in standby and at close. [ours]
- **Auto-mute** (R10 bit 2, `AMUTE`): an analogue mute after 1024 consecutive zero samples, released by the first non-zero sample. Off. [datasheet] It could hide idle hiss; a pop on release would be the risk (`features.md`). 1024 samples are 10.7 ms at 96 kHz and 23 ms at 44.1 kHz. It acts only on **exact** digital zeros: a dithered or 24-bit fade that never reaches zero never mutes, so it can only help a tail that is true digital silence. Switch `dac_set_amute()` (#327), default off until the b17 row (§1.1). [datasheet] [estimate]
- **Digital mute** is DAC volume code 0 (R11/R12). **Analogue mute** is bit 6 of R52–R55 (the volume path's mute writes amp code `0x40`). [datasheet] [rockbox-derived]

## 3. Volume (WM-8 in part)

### 3.1 The two gain stages [datasheet]

| Stage | Register | Range | Step |
|---|---|---|---|
| DAC digital volume | R11/R12, 8 bits | 0 = mute, 1 = −127 dB … 255 = 0 dB | 0.5 dB |
| OUT1 headphone PGA | R52/R53, 6 bits | 0 = −57 dB … 57 = 0 dB … 63 = +6 dB | 1 dB, monotonic |

Each pair has an **update bit** (`DACVU`, `OUT1VU`): writes to either side are held until a write with the bit set, so the left value is written first without it and the right one with it, and both change on the same sample. [datasheet, Tables 22, 33] [rockbox-derived]

**Zero cross** (bit 7 of R52/R53): a new gain takes effect at the next zero crossing, so no zipper noise. If no crossing comes, the change happens after a timeout, but only with `SLOWCLKEN` (R7 bit 0) set, which the driver always sets. [datasheet, ZERO CROSS TIMEOUT] The timeout's length as printed ("2 × SYSCLK period") cannot be right (quirk 14).

### 3.2 How we split a volume [ours] (`firmware/export/dac_gain.h`, `dac_gain_split()`)

The volume arrives in tenths of a dB (−90.0 … +6.0 dB; −90.0 is mute). The split:

1. Add the Sound Check trim (§3.3), clamp to +6 dB, round the total to the nearest 0.5 dB (error at most 0.2 dB).
2. **Above −57 dB:** the amp carries the whole dB (rounded up) and the DAC the remaining 0 or −0.5 dB.
3. **At −57 dB and below:** the amp stays at −57 dB and the DAC attenuates the rest, never below code 1 (code 0 is the mute).
4. Mute: DAC code 0, amp code `0x40` (mute bit).

Worked number: a volume of −20.3 dB with no trim rounds to −20.5 dB (41 half-dB steps); the amp gets −20 dB (code 37) and the DAC −0.5 dB (code 254). With a whole-dB volume and no trim this is exactly the stock Rockbox split (`get_volume_params()`, which the non-shell build still uses). [rockbox-derived]

Keeping the DAC at or near 0 dB and taking the volume on the analogue amp keeps the digital signal at full resolution, which is why the split favours the amp. [rockbox-derived comment: "set dac to max and set volume for amp (better snr)"]

### 3.3 Sound Check and the Volume Limit [ours]

`dac_set_gain_db10(gain, ceiling)` applies a per-track loudness trim in the same writes as the volume. A cut always applies (down to −48 dB). A boost applies only up to the ceiling, which is the Volume Limit, never above the +6 dB maximum, and never turns the volume down. `dac_stats.gain_applied_db10` reports what was actually applied. (`../phase15-brief.md` §3.5)

### 3.4 The EQ precut [ours]

When the hardware EQ boosts a band, `dac_eq_set()` takes a **precut** in whole dB (0–12, the preset's largest boost) and subtracts it from both DAC volumes: 2 DAC codes per dB, clamped at code 1. Code 0 (mute) stays a mute. The amp is untouched. So a +12 dB EQ peak on a full-scale signal does not clip, at the cost of up to 12 dB of digital level. [ours] Whether it is needed depends on where the EQ sits relative to the volume (§ top).

## 4. The equaliser and 3D (WM-9)

### 4.1 What the hardware has [datasheet, 5-BAND GRAPHIC EQUALISER, Tables 24–30]

| Band | Register | Type | Frequencies (codes 0–3) | Width |
|---|---|---|---|---|
| EQ1 | R18 | low shelf | 80, 105, 135, 175 Hz | — |
| EQ2 | R19 | peak | 230, 300, 385, 500 Hz | narrow/wide |
| EQ3 | R20 | peak | 650, 850 Hz, 1.1, 1.4 kHz | narrow/wide |
| EQ4 | R21 | peak | 1.8, 2.4, 3.2, 4.1 kHz | narrow/wide |
| EQ5 | R22 | high shelf | 5.3, 6.9, 9, 11.7 kHz | — |

Every band: ±12 dB in 1 dB steps. `EQ3DMODE` (R18 bit 8) puts the EQ and 3D on the DAC path (1, the reset value) or the ADC path. **The control interface refuses to change `EQ3DMODE` unless both ADCs and both DACs are off.** [datasheet] We never change it.

**Low-power mode and the peaks.** With `M128ENB` = 0 (R7 bit 8, the reset value) and only the DACs running, the DSP core runs at half rate and halves its digital power. The datasheet says that in this mode only EQ1, EQ5 and 3D may be used, and that `M128ENB` = 1 is needed for the full five bands. [datasheet, LOW POWER MODE, Tables 54–55] **On our unit the peaks work in low-power mode:** the owner heard EQ3 (1.1 kHz) and EQ2 (385 Hz) at +12 dB with `M128ENB` = 0, at 44.1 and 48 kHz, undistorted at a middling volume. EQ4 was not called out on its own. [silicon, spike S1, 2026-10-11, owner] (`apps/ipodui/screens/eq.c`, the `IPODUI_HW_EQ` comment; quirk 11). So we keep `M128ENB` = 0 unless one of its three users asks: EQ band 4 with a gain, the 3D stage with a depth (#265), or 88.2/96 kHz at 128× with `dac_set_m128_hirate()` on (p090-osr128, §1.1; quirk 28).

### 4.2 How we drive it [ours] (`dac_eq_set()`)

- **Presets.** Settings › EQ offers the 7G's 22 stock presets. Each was fitted on the host (`tools/eqfit`) to the five bands plus a precut; the fit and its worst error against the original 10-band curve are in `apps/ipodui/eq_hw.h`. **Every preset runs on the codec** (owner, 2026-10-11: listeners pick what they like, not a fidelity score); the software EQ handles nothing while a hardware preset is on, so there is never a double EQ. [ours]
- **Writing.** `dac_eq_set(bands, precut)` builds the five register values (gain, frequency code, wide bit for EQ2–4, `EQ3DMODE` on EQ1), writes only the ones that changed, then the precut through the DAC volumes. `eq_writes` in `dac_stats` counts the writes.
- **Off** (`dac_eq_set(NULL, 0)`): EQ2–EQ4 back to their reset value `0x02C` (0 dB, second frequency, narrow); EQ1 and EQ5 back to the bass and treble shadows; precut 0.
- **Bass and treble** (`audiohw_set_bass()`, `…_treble()`, `…_cutoff()`, Rockbox's tone controls) own EQ1 and EQ5 while the hardware EQ is off; while it is on, they update only their shadows, which the next "off" restores. The shell keeps bass and treble at 0 dB. [ours]
- **Standby:** the EQ registers keep their values through our DAC standby, so EQ changes need no wake. [ours; consistent with the datasheet, where standby only clears enables]

### 4.3 3D enhancement [datasheet, Table 31]

R41 `DEPTH3D`: 0–100 % in 16 steps, on the same path as the EQ. Allowed in DAC low-power mode. The datasheet advises about 6 dB less signal to avoid limiting, which the precut mechanism can supply. Written since 0.8.3 by Settings › Audiophile › Stereo Width (`dac_set_width3d()`, depths 0/5/10/15), with `M128ENB` held while the depth is non-zero (quirk 28). **It plays at every rate, 88.2/96 kHz included**: the EQ guard (§4.4) does not hold it (no change without the owner's decision, 2026-11-16). [ours]

**At 88.2/96 kHz** (finding for the owner, p090-osr128): the 3D stage's coefficients come from the same SR code as the EQ's, the 48 kHz set, so every frequency inside it scales by fs / 48 000: **1.84× at 88.2 kHz and 2.00× at 96 kHz** (0.92× at 44.1). The datasheet publishes no frequency for the 3D stage (only `DEPTH3D`, 0–100 % in 16 steps), so there is no corner to quote; whatever band it widens moves up by that factor, an octave at 96 kHz, as the EQ's corners do (80 Hz shelf → 160 Hz, 2.4 kHz peak → 4.8 kHz, 11.7 kHz shelf → 23.4 kHz). The limiter's attack and decay also run 2× faster there (they scale with the rate, §5). And Width at hi-res means the full-rate DSP at 24.576 MHz, twice its rated clock (§1.1). [datasheet] [estimate]; not heard on silicon (b17's effects-on arm).

### 4.4 Corners at 44.1, 88.2 and 96 kHz [datasheet; ours; hypothesis on silicon]

`ADDCTRL` `SR` (R7 bits 3:1) picks the coefficient set of the digital filters, EQ and 3D included: 000 = 48 kHz, then 32, 24, 16, 12 and 8 kHz. **There is no code above 48 kHz.** The driver runs 44.1 kHz with the 48 kHz code (the nearest; the 32 kHz code would be 1.38x off) and, since #270, 88.2 and 96 kHz with it too (`audiohw_set_frequency()`, `freq_setups[]`). A filter designed for 48 kHz clocked at fs has every corner moved by fs / 48000: **0.92x at 44.1 kHz, 1.84x at 88.2 and 2x at 96 kHz** (band 4's 2.4 kHz sits at 4.8 kHz; the 80 Hz shelf at 160 Hz).

- The rate code cannot fix it: no code exists for those rates. [datasheet]
- Band codes can't compensate fully: each band's four choices span about 1.3 octaves and the shift at 88.2/96 is a whole octave, so the bass shelf can't go below 147/160 Hz there, and a peak can't leave its band. [ours]
- What we do (0.9.0, package D): the presets were fitted at 44.1 and 48 kHz (`tools/eqfit` assumption 2), so 44.1 kHz is already in their error figures. The custom EQ editor labels and draws the **real** corners at the current output rate (nominal x fs/48000, `eqlab_db_q8()`'s `shift_q8`), so what the screen shows is what plays.
- **Preset tables per rate family (0.9.0, #352; owner 2026-11-13).** [ours] The presets are one table per output-rate family: **base** (<= 48 kHz), compiled in (`eq_hw_presets[]`), and **hi** (88.2/96 kHz; 176.4/192 kHz sources are converted down to these), an optional text file in the hidden system folder (`eqtable-hi.txt`, format in `apps/ipodui/eqtable_core.h`: a versioned header line `eqtable 1 family=hi n=22`, then the 22 presets in catalogue order, each a custom slot's line, `eqlab_parse()`). It is read once at boot (one open, one read, one 330 B `core_alloc` block that keeps only the parsed table) and refused whole, loudly in the log, when short, long, malformed or of another version. Every apply picks the table of the mixer's output rate (`eqtable_family_of()`); the UI pass re-checks the family while a preset is selected and something plays (`ui_eq_rate_poll()`), and a change only re-picks (a pointer, no file I/O, nothing on the audio thread). **A family without a table plays the selected preset flat** (`dac_eq_set(NULL, 0, 0)`: every band at its register default, no precut, no make-up), logged `eq table family=hi missing: preset bypassed`; custom slots apply at every rate. 0.9.0 ships no hi file, so presets pause at 88.2/96 kHz; 0.9.1 adds the fitted table (#352: about 370 B, approximate, since band codes can't make up a full octave) with no code change. Log lines: `eq table family=hi missing|loaded n=22|refused: <why> line=<n>`, `eq rate family=<f> preset=<n>`; sim trace `eq dac family=<f> hw= precut= makeup=`. The switch lands at the UI pass after the new rate is chosen, so the first tens of milliseconds of a song at a new rate can play with the previous family's curve. [ours; not heard on silicon]
- **The driver's EQ guard (p090-osr128, #439; developer switch `dac_set_hirate_eq_guard()`, default off).** The shell's bypass above covers presets only, by design (custom slots play at every rate: owner, `../design/changes-from-7g.md` 2.66), and lands a UI pass after the rate change, so the first tens of milliseconds of a song at 88.2/96 kHz can play the previous base-table preset. With the switch on, the driver holds the codec EQ flat at 88.2/96 kHz on every path: presets and custom slots (`dac_eq_set()` keeps the request and writes the "off" image), bass and treble (shadow only), the rate change itself (`audiohw_set_frequency()` flattens before PCM starts), Hi-Res 128x's live OSR change, standby, a restart. No band gain, no precut, no make-up, limiter off; the request comes back whole below 88.2 kHz or with the guard off (`dac_hr_sync()`). Off by default because on would silence the owner's custom slots and the 0.9.1 hi table (#352) at those rates; the bench turns it on for the b17 baseline arms, and whether release builds keep it off (or turn it on at 128× only) is the owner's call after b17. Host test `tests/dacosr/test_hr.c` (the guard on: no band gain is ever written at 88.2/96 kHz). [ours]
- Not checked on silicon: that the shift really follows fs (an `eq4m`-style A/B at 96 kHz with band 4 at +12 dB, 2.4 kHz nominal: it should sound like a 4.8 kHz lift). Bench row `eq96`.

## 5. The DAC limiter and volume boost [datasheet, Table 23]

R24/R25, never written:
- **Limiter** (`LIMEN`): threshold −1 … −6 dB (`LIMLVL`; the upper threshold sits 0.5 dB above, the lower 0.5 dB below); attack 94 µs … 192 ms and decay 0.75 ms … 1.536 s per 6 dB at 44.1 kHz, scaling with the sample rate.
- **Boost** (`LIMBOOST`): 0 … +12 dB in 1 dB steps, applied to signals below the threshold. It works **with the limiter off** as a plain digital gain stage.

Uses: a hardware ceiling under the Volume Limit and EQ boosts; a free +12 dB for quiet files and Sound Check; loudness compression with no CPU. Each needs a tone-burst check before use, since the timings are given for 44.1 kHz only (`open-questions.md` 15).

### 5.1 How 0.9.0 uses it (#203) [ours; switch off until heard]

The EQ keeps its loudness (`eqlab_level()`, `dac_eq_set(band, precut, makeup)`): the precut (the summed curve's peak, DAC digital attenuation) stays, and a make-up of `precut - loud` (the K-weighted pink-noise loudness change, clamped to 0..precut) is summed with Sound Check's trim and fitted under the Volume Limit, so it lands mostly on the OUT1 amp. Writes go quiet side first: the larger precut and smaller make-up before the EQ registers, the new levels after. The limiter is the safety net for programme material whose peaks pass the sine-based precut: on only while the EQ is on, `R25 = 0x000` (−1 dB, no boost) then `R24 = 0x1A2` (`LIMEN`, decay 768 ms, the slowest, attack 375 µs, the default). It is **on by default** since b18 (`DAC_LIMITER_DEFAULT 1`; the owner heard it act in `limon` and `eqlim` played clean, [silicon b18, 2026-11-19]; `dac_set_limiter_allow()` turns it off for the bench); the make-up is analogue and after the limiter, so the limiter never acts on it. Host test: `tests/dacosr/test_eqlev.c` (every write's headroom, the summed trim, the cap at the Volume Limit, the limiter's states); `tests/eqlevel` (the loudness plan against a double-precision reference: the 18 boost presets end within 0.12 dB of EQ off).

What only the device can confirm: that the digital volume (where the precut sits) is before the EQ (`open-questions.md` 13, bench `eqord`), that the limiter catches EQ peaks without pumping (`eqlim`, `limon`: heard in b18, clean), and that loudness by ear matches (`eqloud`).

## 6. Mixers and routing (WM-7)

- **Left and right output mixers** (R50/R51): each takes its DAC and, optionally, the bypass path from the input boost stage (−15 … +6 dB, 3 dB steps). We route **the DAC only** (`0x001`); `audiohw_enable_recording()` adds the bypass for monitoring, but recording is not built. [datasheet] [rockbox-derived]
- **Cross-mixing** (R49 `DACL2RMIX`, `DACR2LMIX`) and **R43's bypass cross-routes** would make a mono mix in the analogue domain. Unused. [datasheet]
- **OUT3/OUT4 mixers** (R56/R57): a third stereo pair, a mono mix, or a VMID reference. Muted, no inputs, disabled. [datasheet] [rockbox-derived]

## 7. Outputs (WM-8)

### 7.1 OUT1: the headphones

- LOUT1/ROUT1 drive 16 or 32 Ω, either through DC-blocking capacitors with the `HP_COM` common-ground feedback input, or DC-coupled to a buffered mid-rail reference on LOUT2/ROUT2 ("capless"). [datasheet, HEADPHONE OUTPUTS] The driver assumes the capacitor configuration (`HP_COM` on, OUT2 off). Whether the board has the capacitors is not documented here (`open-questions.md` 11). [hypothesis]
- **`HP_COM`'s polarity depends on a pin.** In 2-wire mode with CSB/GPIO1 held high (and not set up as a GPIO), `HP_COM` and `LINE_COM` are inverted: writing 1 turns the feedback **off**. [datasheet, COMMON MODE DEFAULT] We do not know that pin's level on this board, so we do not know whether our `0x187` turns the feedback on or off (quirk 9, `open-questions.md` 3).
- **Disabled outputs** stay tied to mid-rail through 1 kΩ (`VROI` = 0) or about 30 kΩ (`VROI` = 1, ours). This is what keeps the pins at mid-rail through our standby, without a pop at the wake. [datasheet, Table 41] [rockbox-derived]
- **Thermal shutdown:** with `TSDEN` and `TSOPCTRL` set, all outputs are disabled at about 150 °C. We keep it on while running and clear it at close (the datasheet's power-down step 1). [datasheet] [rockbox-derived] Apple ends its power-up with it off (`stock-firmware-vs-ours.md` §3).

### 7.2 OUT2: the old dock line out

LOUT2/ROUT2 are a second headphone-capable pair on their own supply (AVDD2). On a stock 5G/5.5G they fed the dock connector's line out [rockbox-derived]; on our unit the 30-pin connector has been removed, and the USB-C mod's makers say audio is also wired out over USB-C, presumably from this pair (`../hardware.md`). [hypothesis] Apple's power-up enables OUT2 (`stock-firmware-vs-ours.md` §3); we leave it off unless `audiohw_enable_lineout(true)` is called, which the shell does not do. Whether OUT2 reaches the USB-C board is `open-questions.md` 12.

The datasheet calls OUT2 "speaker" in places (`SPKVU`); on this board read it as "line out".

### 7.3 OUT3/OUT4

Muted and disabled; no known connection on this board. [rockbox-derived] [hypothesis]

## 8. The ADC side (WM-6)

The microphone preamps, input PGAs (−12 … +35.25 dB in 0.75 dB steps), `MICBIAS`, the boost stages, the ADCs, the ADC high-pass and notch filters, the **ALC and the noise gate** all sit on the record path. **None of them can act on playback.** [datasheet] The 5.5G has no microphone; Rockbox's recording code treats L2/R2 as a line input, which was a dock input ("We only have a line-in (I think)") [rockbox-derived]. With the dock connector gone, nothing is known to reach these pins on our unit. [hypothesis] L2/R2 double as GPIO2/GPIO3 for jack detection (R9, R13); unused. [datasheet]
