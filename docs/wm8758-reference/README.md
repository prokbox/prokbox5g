# WM8758 reference: what the codec offers, what we use, what we leave on the table

*Written 2026-10-24 from the public WM8758B datasheet. Own words, short quotes only. Update it with every device finding (rule: record it with its tag in the same change).*

## Source and tags

- **Datasheet:** Wolfson Microelectronics (now Cirrus Logic), *WM8758B, Stereo CODEC with Headphone Driver and Line Out*, Production Data, **January 2012, Rev 4.4**, 91 pages. It is the WM8758B itself, not a sibling. The 5.5G part is marked WM8758; the "B" is the production revision of the same register map, and our driver's register layout matches this map bit for bit for every register we checked (R1-R3, R4, R6, R7, R10, R14, R18-R22, R41, R42, R44-R57, R61). Section references below are to this document.
- **Where it came from:** `https://pdf.aiema.com/wm8758cbgeflv-Wy7m1veu-roNav7PYq.pdf` (a third-party mirror; Cirrus Logic's own host refused the download). Found through alldatasheet.com's WM8758B listing (474018). The local copy sits outside the repo in the session scratchpad (`.../scratchpad/wm8758-datasheet/wm8758b.pdf`); it was read with `pdftotext` only. Anyone can re-fetch it from the URL above or from any WM8758B mirror, and should check the footer reads "PD, Rev 4.4, January 2012".
- **Our driver:** `firmware/drivers/audio/wm8758.c`, `firmware/export/wm8758.h` (its own header says it was written "based on datasheet for WM8983", a sibling; this document is the first check against the real part). Board-side bring-up is in `docs/pp5022-reference/audio.md`.
- **Tags:** [datasheet] stated by the Rev 4.4 datasheet; [rockbox-derived] from the Rockbox/ipodlinux driver or our own code; [silicon] measured on the device (only where a repo doc shows it); [hypothesis] inferred, unverified. What the datasheet cannot tell us is how the **iPod board** wires the chip (which pins are connected, which supply rails). Anything that depends on wiring is marked so.

## Index

1. Block-by-block status (features table)
2. Register coverage (every register, used or not)
3. Power (per-block bits, lowest-power playback configuration)
4. Corrections to what we believed
5. Open questions and probes

## 1. Features table

Status words as in `docs/hardware-features.md`: *used* (where), *known-unused*, *unwired* (no connection on this board, or none we can prove).

| Feature | Datasheet section | Status | Evidence | ProkBox benefit | Probe |
|---|---|---|---|---|---|
| **5-band EQ**: EQ1 low shelf 80/105/135/175 Hz, EQ2-4 peaks (230-500 Hz, 650 Hz-1.4 kHz, 1.8-4.1 kHz, each narrow or wide), EQ5 high shelf 5.3-11.7 kHz; +-12 dB in 1 dB steps; applies to the DAC path when `EQ3DMODE`=1 | 5-BAND EQUALISER, R18-R22 | **used**: `dac_eq_set()` drives all five (P15); EQ1/EQ5 double as bass/treble | [datasheet]; [rockbox-derived] driver; S1 audible per owner [silicon] | Zero-CPU EQ | Confirm which S1 round (M128ENB 0 or 1) the owner heard the mid bands in (see section 4, item 1) |
| **3D stereo enhancement**: `DEPTH3D` R41[3:0], 16 steps, 0 to 100 % | 3D STEREO ENHANCEMENT | **known-unused** (`THREEDCTRL` defined, never written) | [datasheet]. Allowed in DAC low-power mode (M128ENB=0), so it costs no extra DSP power; the datasheet advises about 6 dB less signal to avoid limiting, which the existing EQ precut path (`dac_precut`) already provides | A hidden "spatial" setting, zero CPU | Listening test; write `0x29` with a depth, `EQ3DMODE` already set |
| **DAC digital limiter** (playback): enable `LIMEN`, threshold -1..-6 dB (`LIMLVL`), attack 94 us-192 ms, decay 0.75 ms-1.5 s (44.1 kHz values; scale with rate), plus a **0..+12 dB boost** (`LIMBOOST`, usable even with the limiter off) | DAC DIGITAL OUTPUT LIMITER, R24-R25 | **known-unused** (`DACLIMITER1/2` defined, never written) | [datasheet] | Hardware safety limiter for Volume Limit and EQ boosts; a free +12 dB digital gain stage for quiet files and Sound Check; loudness compression with no CPU | Register write at 0 dB with a 0 dBFS sine; scope the clip point |
| **ALC and noise gate** (record) | ALC, R32-R35 | **unwired / not applicable to playback**: ALC and noise gate sit in the **ADC** path only. They cannot level music playback | [datasheet] | none for playback | none |
| **ADC high-pass, notch filter** | HIGHPASS FILTER, R14, R27-R30 | unwired (ADC path only) | [datasheet] | none | none |
| **DAC filters**: fixed linear-phase interpolation filter (passband to 0.454 fs, stopband -55 dB, group delay 29/fs); **no filter-shape choice, no de-emphasis** | DIGITAL FILTER CHARACTERISTICS | used (always on; not selectable) | [datasheet]. The earlier "de-emphasis" idea in the catalog is not in this part | none; de-emphasis, if ever wanted for old CDs, is a software biquad | none |
| **DAC soft mute** (`SOFTMUTE`, R10 bit 6): a gain ramp, on by default | DIGITAL PLAYBACK (DAC) PATH | **used**: mute on pause/close, held in standby (`DACCTRL_SOFTMUTE`) | [datasheet]; [rockbox-derived] | Click-free pause; ramp time not specified in the datasheet | Time the ramp with the BCM's LRCK sampler or a scope |
| **Auto-mute** (`AMUTE`): analogue mute after 1024 zero samples, released on the first non-zero sample | AUTO-MUTE | known-unused | [datasheet] | Silences DC/idle hiss in gaps; GPIO1 can flag it ("Amute active") | Listen for hiss on silence; check for a pop on release |
| **DAC oversampling** `DACOSR128`: 0 = 64x, 1 = 128x | DAC Control R10; POWER MANAGEMENT | **used at 128x**: `audiohw_mute(false)` writes `DACCTRL_DACOSR128` | [datasheet] says 64x is "lowest power & best performance" in the DAC table and "slight decrease in noise performance" for the ADC-side text; the two statements disagree. P15 brief kept 128x ("fidelity first") | Lower power, size of saving not given | A/B listen plus a current meter, 64x vs 128x (see section 3) |
| **DSP core low-power mode** `M128ENB`=0 (R7 bit 8): halves the digital core's power, DAC-only | POWER MANAGEMENT, Tables 54-55 | **used by default** (we never set M128ENB in the shipping path) | [datasheet]; [rockbox-derived] | Half the DSP core's digital power. **But** in this mode only EQ bands 1 and 5 plus 3D are permitted | See section 4, item 1 |
| **PLL**: 8-50 MHz in, fractional, `PLLN`/`PLLK` R36-R39, `PLLEN` R1 bit 5 | CLOCKING SCHEMES, PLL | **used**: 12 MHz after the /2 prescaler to 22.5792 or 24.576 MHz; powered off in DAC standby | [datasheet]; [rockbox-derived]; wake 6.4 ms median [silicon] | Power while paused (done) | Lock time is not specified; GPIO1 "PLL lock" output would measure it if the pin is reachable |
| **Sample rates**: 8, 11.025, 12, 16, 22.05, 24, 32, 44.1, 48 kHz only | features list; ADDCTRL R7 | used (all nine) | [datasheet] | **No 88.2/96/192 kHz**: hi-res files need a software resample to 48 kHz or lower | none |
| **Word length / format**: 16, 20, 24, 32 bit; I2S, left- or right-justified, DSP/PCM; BCLK/LRC polarity; L/R swap; mono | DIGITAL AUDIO INTERFACES, R4 | **used at 16-bit I2S** (`AINTFCE_IWL_16BIT`) | [datasheet]; PP side unknown (I2S size field values other than 0 are untested, `audio.md` section 1) | 24-bit playback needs the PP's I2S size field and FIFO format: the codec side is ready | `IISCONFIG[9:8]` probe on the PP (W-8) |
| **Companding, loopback** (A-law/u-law; ADC-to-DAC digital loopback) | R5 | known-unused | [datasheet] | none | none |
| **Output mixers, bypass path**, per-mixer volume | ANALOGUE OUTPUTS, R50-R51 | used (DAC to mixer only); bypass used in `audiohw_enable_recording()` only | [datasheet]; [rockbox-derived] | none | none |
| **OUT1 headphone driver** (16/32 ohm, 40 mW/ch at 16 ohm / 3.3 V): -57..+6 dB analogue gain in 1 dB steps, zero-cross volume, mute bit | HEADPHONE OUTPUTS (LOUT1 AND ROUT1), R52-R53 | **used**: amp plus DAC attenuation split; zero cross on | [datasheet]; [rockbox-derived] | Quality; no zipper noise | none |
| **Headphone common-ground feedback** `HP_COM`/`LINE_COM` (R49 bits 8/7) | Common Mode | **used**: set in `audiohw_preinit()` (AC-coupled output with HPCOM feedback, the datasheet's capacitor configuration) | [datasheet]; [rockbox-derived] | Crosstalk and ground-noise rejection | none |
| **Capless headphone (DC-coupled to a LOUT2 mid-rail reference)** | HEADPHONE OUTPUTS | unwired as far as we know: the driver uses the capacitor configuration; whether the board has the caps is not documented here | [datasheet]; board [hypothesis] | Would save two output caps (hardware only) | Board photo or schematic |
| **OUT2 (LOUT2/ROUT2)**: second pair, headphone or line level | R54-R55 | unwired on our unit (it fed the dock line-out; the dock line-out no longer exists) | [rockbox-derived]; `audio.md` | none | none |
| **OUT3/OUT4**: stereo line or mono mix, 6 dB attenuator | R56-R57 | unwired / unused (muted and not enabled) | [datasheet]; [rockbox-derived] | none | none |
| **Mic preamps, input PGAs (-12..+35.25 dB), MICBIAS (3 mA), ADC, boost** | MICROPHONE INPUTS, R44-R48 | **unwired**: no microphone on the 5.5G; Rockbox's own code comment says "We only have a line-in (I think)" (it was a dock line-in) | [datasheet]; [rockbox-derived] | Voice notes only if a mic or accessory input exists | Board wiring check |
| **Beep generator** | R43 | **does not exist** in this part: the "Beep control" register only routes input bypass paths into the mixers (`DELEN2`, `BYPL2RMIX`, `BYPR2LMIX`) | [datasheet] | none | none |
| **Thermal shutdown** `TSDEN`+`TSOPCTRL` (about 150 C disables outputs) | THERMAL SHUTDOWN, R49 | **used**: set in `audiohw_preinit()`, cleared at close | [datasheet]; [rockbox-derived] | Protection | none |
| **GPIO1 / CSB pin**: input, "Temp ok", "Amute active", PLL clock out, "PLL lock", logic 0/1; pull-up/down, open drain | GPIO CONTROL, R8 | unknown wiring: on the iPod the interface is 2-wire at I2C address 0x1A, so the pin is an ordinary GPIO function if the board leaves it reachable | [datasheet]; board [hypothesis] | Would let the PP read PLL lock or temperature | Trace the pin on the board |
| **Jack detect / output switching** (GPIO1-3 select a pin, per-polarity output enables, debounce) | OUTPUT SWITCHING (JACK DETECT), R9, R13 | known-unused: headphone insertion is detected on the PP side | [datasheet]; [rockbox-derived] | Zero-latency output switching if a jack-sense line reaches a codec GPIO (unknown) | Board wiring check |
| **Per-block power-down** (R1, R2, R3 enables; `SLEEP`; `VMIDSEL`; `BIASEN`; `BUFIOEN`) | POWER MANAGEMENT, Tables 37-38, 55-58 | **used** (DAC standby: PLL, DACs, mixers, OUT1/OUT2 off; VMID 500 k, BIASEN and BUFIOEN on); `SLEEP` unused | [datasheet]; [rockbox-derived]; [silicon] for the wake time | Power while paused | Section 3 |
| **VMID and pop control** (`POBCTRL`, `VMIDTOG`, 100 ms VMID rise rule) | POP MINIMISATION, RECOMMENDED POWER UP/DOWN SEQUENCE | **used**: preinit/close follow the datasheet order (our steps match its 14-step sequence) | [datasheet]; [rockbox-derived] | Click-free power cycle | none |
| **Bias trims** `BIASCUT` (low bias, R61 bit 8), `HALFOPBIAS` (bit 0) | HALFOPBIAS, R61 | `BIASCUT` used during preinit then cleared; `HALFOPBIAS` unused: "Do not use" (saves 0.5 mA, hurts THD+N) | [datasheet]; [rockbox-derived] | A candidate only if fidelity tests allow | A/B listen; `BUFBIAS`/`ADCBIAS` (R61) are listed in the map but not described in the text: leave alone |

## 2. Register coverage

R0 reset (unused). R1-R3 power (used, shadowed). R4 interface (used). R5 companding (unused). R6 clock (used). R7 additional (used). R8 GPIO (unused). R9 jack control (unused). R10 DAC control (used). R11-R12 DAC volume (used). R13 jack enables (unused). R14-R16 ADC control and volume (unused). R18-R22 EQ (used). R24-R25 DAC limiter (**unused**). R27-R30 notch (unused). R32-R35 ALC and noise gate (unused). R36-R39 PLL (used). R41 3D (**unused**). R42 OUT4/ADC and bias (used for pop control). R43 beep/bypass (unused). R44-R48 input path (unused except recording). R49 output control (used). R50-R51 mixers (used). R52-R55 output volumes (used; OUT2 only in `audiohw_set_lineout_volume`). R56-R57 OUT3/OUT4 mixers (written muted). R61 bias (used). The write-only I2C bus means nothing can be read back: every claim about chip state rests on the write log (`dac_reglog`) and on listening.

## 3. Power

**What the datasheet gives** (typical, quiescent signal, load power excluded, slave mode with a 12.288 MHz MCLK, 48 kHz, no PLL in these rows):

| State | Registers | Rails | Total |
|---|---|---|---|
| Off | defaults | 2.5 V | 0.020 mW |
| Standby | R1=009, R49=006 | 2.5 V (AVDD1 0.115 mA) | 0.288 mW |
| Stereo playback, OUT1, clocks on | R1=009, R2=180, R3=06F | 3.3 V: DCVDD 6.8 mA, AVDD1 5.8, AVDD2 0.7 | 43.9 mW |
| same | | 2.5 V: DCVDD 4.8 mA, AVDD1 4.3, AVDD2 0.5 | **24.0 mW** |
| same | | DCVDD 1.8 V, analogue 2.5 V: DCVDD 3.2 mA | **17.8 mW** |

Quiescent playback is therefore about **24 mW at the supply we run** (PMU `D1REGC1`, 2.5 V in Rockbox [rockbox-derived]), against 0.3 mW in standby: our DAC standby (P15) removes about 99 % of the codec's draw while paused. Our real figure is somewhat higher than the table (the codec is I2S master with its PLL running, which the table rows do not include) [hypothesis]. Output stage power into the headphones is on top and dominates at high volume.

**Per-block bits during playback (what each saves):**

| Bit / block | State now | Option | Evidence |
|---|---|---|---|
| `LOUT2EN`/`ROUT2EN` (OUT2), `OUT3EN`/`OUT4EN`, `OUT3MIXEN`/`OUT4MIXEN`, `MICBEN`, `INPPGAEN`, `BOOSTEN`, `ADCEN` | all off during playback (only OUT1, mixers, DACs, PLL, BIAS, BUFIO on) | nothing to gain: already off | [datasheet] "to save power, unused parts of the WM8758B should remain disabled"; [rockbox-derived] driver |
| `DACOSR128` | 128x | 64x: the datasheet calls it the lowest-power setting; the saving is not published | [datasheet] |
| `M128ENB` | 0 (low-power core) | stays 0 unless the EQ needs the peaking bands (EQ2-4) | [datasheet] |
| `VMIDSEL` | 500 k in play and standby | the setting only trades start-up time (10 k fastest, 500 k slowest of the powered options); VMID must stay powered or the amps and the analogue side cannot run, and a full VMID-off standby costs a 100 ms re-charge plus a pop risk | [datasheet] |
| `BUFIOEN` | on | needed to avoid pops when outputs enable | [datasheet] |
| `BIASEN` | on | the amps do not run without it | [datasheet] |
| `HALFOPBIAS` | off | -0.5 mA (AVDD) at a THD+N cost; datasheet says do not use | [datasheet] |
| `PLLEN` | on while PCM runs, off in standby | already exploited | [silicon] wake |
| `SLEEP` (R2 bit 6) | unused | "supply current reduced in device standby mode": for use only when already in standby and the external MCLK cannot be stopped. Our MCLK is the PP's own external-device clock, which the PP could gate; that is the larger saving and is a PP-side question (`DEV_EXTCLOCKS`) | [datasheet]; PP side [hypothesis] |
| Standby with VMID and BIAS off ("Off" row, 0.020 mW vs 0.288 mW) | VMID/BIAS kept on in standby | saves about 0.27 mW more, costs a 100 ms VMID rise and a pop on wake | [datasheet] |

**Lowest-power headphone-playback configuration** (candidate, pending listening tests): DAC-only core with `M128ENB`=0; OUT1 headphone amps, both DACs and both output mixers on, everything else off (as now); `DACOSR128`=0 (64x); PLL and BCLK only while PCM runs (as now); EQ restricted to bands 1 and 5 plus optional 3D while in the low-power core, switching `M128ENB`=1 only when a preset needs EQ2-4, and back to 0 on return to flat; digital supply as low as the board allows (DCVDD is specified down to 1.71 V, the 24.0 -> 17.8 mW row, but whether the 5.5G wires DCVDD to a separately settable rail is unknown). On the datasheet's own numbers the digital core is a large share of the quiescent playback current (DCVDD 4.8 of 9.6 mA at 2.5 V), so the two digital-side levers (core mode, oversampling rate) are the ones to measure.

## 4. Corrections to what we believed

1. **EQ bands 2-4 and `M128ENB`: the shipping path may be in the wrong mode.** The datasheet is explicit: in DAC low-power mode (`M128ENB`=0, DACs only) "3D enhancement with 2-Band equalizer functionality is permitted, where only Band 1 (low shelf) and Band 5 (high shelf) can be used"; "to enable 3D enhancement and 5-band EQ operation, set M128ENB = 1". The comment in `wm8758.h` ("will work when M128ENB is enabled") was right. Our shipping `dac_eq_set()` never sets `M128ENB` (only the developer EQ test, `dev_eq_test()`, does, in its second round), yet `eq.c` routes **every** preset to the codec (`IPODUI_HW_EQ`, `EQ_HW_MAXERR >= 255`). The ROADMAP records the S1 test as passed with "all five hardware bands audible" without saying which round. If the peaks were only audible in the `m128=1` round, the shipping presets are silently missing their mid bands. Action: read the `eqt` lines in the device `ipodui.log` from the S1 run, or repeat it, and set `M128ENB` from `dac_eq_set()` whenever a band 2-4 gain is non-zero. This is a [datasheet] versus [silicon] question.

   **Resolved [silicon], 2026-10-24:** the S1 record is in `apps/ipodui/screens/eq.c` (the `IPODUI_HW_EQ` comment). The owner heard EQ3 (1.1 kHz) and EQ2 (385 Hz) at +12 dB **with M128ENB off**, at 44.1 and 48 kHz, undistorted. On this unit the peaking bands work in the low-power core, so the shipping presets are not missing their mid bands, and the datasheet's restriction does not hold here (or applies only outside the DAC-only path). EQ4 was not called out individually. Keep `M128ENB`=0. An optional bench check is to repeat S1 with a current meter, to see whether band 2–4 gains change the core current.
2. **No de-emphasis, no DAC filter choice, no beep tone generator** (catalog W-6 and the first W-9 reading assumed they might exist). They do not, in this part.
3. **ALC and noise gate are ADC-only** (W-5). The playback-side level control is the separate DAC limiter with its 0..+12 dB boost, which is real and unused.
4. **Maximum sample rate is 48 kHz.** Hi-res FLAC at 88.2/96/192 kHz must be resampled in software whatever the PP's I2S block can do; W-8 only gains word length (24-bit), not rate.
5. **`DACOSR128` is on**, and the datasheet describes 64x as the lowest-power setting (with an inconsistent note on noise). Worth a measured A/B; the P15 decision to keep 128x was "fidelity first", not a datasheet finding.
6. **Datasheet typo:** Table 21 says `SOFTMUTE` "0 = Enabled, 1 = Disabled", but the text and our driver agree on the opposite meaning (1 = muted, bit set to 0 to play). Trust the text.

## 5. Open questions

- Which S1 round carried the audible mid bands (section 4, item 1)?
- Which supply rails feed DCVDD, DBVDD, AVDD1, AVDD2 on the 5.5G, and can the PMU lower DCVDD below 2.5 V?
- What does `DACOSR128`=0 sound and draw like (A/B plus a USB-C meter, `docs/hw-experiments.md` HX-C1 style)?
- What is the real PLL lock time (our 2 ms settle is a guess; wake measured 6.4 ms median, 12.0 ms worst)?
- Is the codec's GPIO1/CSB pin reachable, and what does it connect to?
- Does the board have DC-blocking caps on OUT1 (the code assumes yes)?
- Can the PP I2S block carry 24-bit samples (`IISCONFIG[9:8]`, FIFO format), and would a 24-bit path be audible after the 16-bit resampled path?
- Could the PP gate the codec's MCLK in standby, and is `SLEEP` then needed?
- Limiter timings are given for 44.1 kHz and scale with rate: confirm with a tone burst before relying on them.

## Supply rails on the 5.5G (2026-10-24)

- **Codec rail:** PMU `D1REGC1` feeds the codec, set to 2.5 V (Apple's default is 3.0 V) [rockbox-derived, `pcf50605.c`]. The driver notes that ECO mode is "not allowed as max. current of 5mA is not sufficient", which suggests this one rail also carries the analogue and headphone supplies. AVDD1/AVDD2 have a datasheet minimum of 2.5 V, so if the codec's supplies are all on `D1REGC1`, the rail is already at the analogue floor and cannot go lower.
- **The 1.8 V lead:** `DCUDC1` is set to 1.8 V with the comment "Unknown" [rockbox-derived]. 1.8 V is exactly the datasheet's typical DCVDD. If DCVDD is wired to `DCUDC1`, the 24.0 → 17.8 mW digital-supply saving is already banked. If DCVDD is tied to `D1REGC1`, it can't be lowered without a board change, because AVDD needs ≥2.5 V and DBVDD must be ≥ DCVDD.
- **Probe (read-only, owner hardware):** with a multimeter on WM8758 pin 13 (DCVDD) against ground while playing, read 1.8 V vs 2.5 V. No firmware probe is safe: lowering a shared rail under the analogue floor risks distortion or a brown-out.
- **Status:** [hypothesis] until measured. Expected benefit: small, and likely zero (already banked, or blocked).
