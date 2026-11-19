# Power

*What the codec draws, what each enable bit costs, what feeds it on this board, and the lowest-power playback configuration we know of. Part of WM-C. Sequences are in `bring-up.md` §4–§5; Apple's policy in `stock-firmware-vs-ours.md` §3.*

## 1. What the datasheet gives [datasheet, POWER CONSUMPTION]

Typical figures, quiet signal, load power excluded. All rows but the master row are slave mode, MCLK 12.288 MHz, LRC 48 kHz, no PLL.

| State | Registers (hex) | Supplies | Currents | Total |
|---|---|---|---|---|
| Off | defaults | all 2.5 V | AVDD1 0.008 mA | 0.020 mW |
| Standby (VMID and bias on, no clocks) | R1=009, R49=006 | all 2.5 V | AVDD1 0.115 mA | 0.288 mW |
| Stereo playback to OUT1, slave | R1=009, R2=180, R3=06F | all 3.3 V | DCVDD 6.8, AVDD1 5.8, AVDD2 0.7 mA | 43.9 mW |
| same | | all 2.5 V | DCVDD 4.8, AVDD1 4.3, AVDD2 0.5 mA | **24.0 mW** |
| same | | DCVDD and DBVDD 1.8 V, analogue 2.5 V | DCVDD 3.2 mA | **17.8 mW** |
| **Playback to OUT1, master, PLL on** (MCLK 13 MHz) | R1=029, R2=180, R3=00F, R6=149, PLL set | all 2.5 V | DCVDD 2.18, **DBVDD 3.5**, AVDD1 5.24 mA | **27.3 mW** |
| same | | all 3.3 V | DCVDD 3.06, DBVDD 7.9, AVDD1 7.1 mA | 59.6 mW |

**The master-mode row is the closest to our set-up** (codec as master with its PLL running, OUT1 only, OUT2 off): about **27 mW at 2.5 V**. The seed of this manual said the table had no such row; it does. [datasheet] Two oddities in that row: its DCVDD current at 1.8 V (3.7 mA) is higher than at 2.5 V (2.18 mA), which is implausible, and its PLL registers are printed as "R24–R27", the hex addresses of R36–R39 (quirk 22). The DBVDD current in the master row (3.5 mA at 2.5 V against 0.005 mA in slave mode) is the interface pins driving BCLK and LRC; that row uses BCLK ÷4 (64 fs), ours ÷2 (128 fs), Apple's boot driver ÷8 (32 fs). [datasheet; the link between BCLK rate and DBVDD current is our hypothesis] (`open-questions.md` 7)

Quiescent playback is therefore about 24–27 mW at the supply we run, against about 0.3 mW in our standby: **the DAC standby removes about 99 % of the codec's own draw while paused.** [datasheet; estimate] Power into the headphones comes on top and dominates at high volume.

## 2. Per-block bits during playback

| Bit / block | State now | Option | Evidence |
|---|---|---|---|
| OUT2, OUT3/OUT4 and their mixers, `MICBEN`, input PGAs, boost, ADCs | all off (only OUT1, the L/R mixers, the DACs, the PLL, `BIASEN` and `BUFIOEN` are on) | nothing to gain | [datasheet] "to save power, unused parts ... should remain disabled"; [rockbox-derived] |
| `DACOSR128` | 128× | 64×: called the lowest-power setting; the saving is not published | [datasheet] (`open-questions.md` 5) |
| `M128ENB` | 0 (DSP at half rate) unless EQ band 4 has a gain or the 3D stage is on (#265), or 88.2/96 kHz at 128× with `dac_set_m128_hirate()` on (p090-osr128, on by default since b18; Hi-Res 128x itself is opt-in) | EQ2/EQ3 work at 0; EQ4 needs 1 | [datasheet]; [silicon S1, b11 m128] |
| `VMIDSEL` | 500 kΩ running and in standby | the setting trades only start-up time (10 kΩ fastest); VMID must stay up for the analogue side, and a full VMID-off standby costs a ~100–300 ms recharge and a pop risk | [datasheet] |
| `BUFIOEN` | on | needed against pops when outputs are enabled | [datasheet] |
| `BIASEN` | on | the amplifiers do not run without it | [datasheet] |
| `HALFOPBIAS` | off | −0.5 mA on AVDD, at a "significant" THD+N cost; Wolfson says do not use | [datasheet] |
| `BIASCUT` | off once running | Apple keeps it on (R61 `0x100`) after power-up; the datasheet does not say what it costs or saves outside step 2 | [apple-RE]; effect [hypothesis] (`open-questions.md` 20) |
| `PLLEN` | on while PCM runs, off in standby | already used | [silicon] wake 6.4 ms median |
| `BCLKDIV` | ÷2 (128 fs) | ÷8 (32 fs) as Apple's boot driver: fewer pin edges | [apple-RE]; saving [hypothesis] |
| `SLEEP` (R2 bit 6) | unused | stops MCLK propagating inside the chip; for use only when already in standby and the external MCLK cannot be stopped. Ours can be stopped at the PP (`DEV_EXTCLOCKS`), which is the larger saving, but the AW-5 gate failed on the bench (2 of 6 resumes late, sitting 4gh, 2026-11-06) | [datasheet]; [silicon] |
| Full off in standby (VMID and bias off: the "off" row) | VMID and bias kept on | saves about 0.27 mW more; costs a VMID recharge and a pop risk on every resume. Apple does this in sleep only; so will our P18 sleep (T-CODECOFF) | [datasheet]; [apple-RE] |

## 3. Supply rails on the 5.5G

The chip has four supplies: AVDD1 (2.5–3.6 V, all analogue functions and OUT1/OUT3/OUT4, "the most significant impact on overall power consumption"), AVDD2 (2.5–3.6 V, OUT2 only), DCVDD (1.71–3.6 V, the digital core, "no effect on audio quality") and DBVDD (1.71–3.6 V, the interface pins, must be at least DCVDD). Higher AVDD improves audio quality. [datasheet, POWER SUPPLIES]

- **Codec rail:** PMU `D1REGC1` feeds the codec. Rockbox sets 2.5 V; Apple's default is 3.0 V. **Our shell build now writes 3.0 V (`0xf5`) at init (owner, 2026-11-16, b17); stock builds keep 2.5 V.** [ours] Written in `power_init()` before the codec is enabled, so no pop is expected; the audible effect and the battery cost are still unmeasured. [rockbox-derived] `pcf50605.c` The driver notes that the regulator's ECO mode is "not allowed as max. current of 5mA is not sufficient", which suggests this one rail also carries the analogue and headphone supplies. [hypothesis] AVDD1/AVDD2 have a 2.5 V minimum, so if all codec supplies are on `D1REGC1`, the rail is already at the analogue floor.
- **What 2.5 V cost in sound (the shell build no longer runs at it):** against 3.0 V, the line-out SNR drops from 100 to 96 dB A-weighted and full scale from about 0.91 to 0.76 V rms (−1.6 dB of headroom). [datasheet, Electrical Characteristics] Whether that is audible on headphones is untested.
- **The 1.8 V lead:** `DCUDC1` is set to 1.8 V with the comment "Unknown". [rockbox-derived] 1.8 V is exactly the datasheet's typical DCVDD. If DCVDD is wired to `DCUDC1`, the 24.0 → 17.8 mW digital-supply saving is already banked; if it is tied to `D1REGC1`, it cannot be lowered without a board change.
- **Apple puts the codec LDO in a low-power mode (`010`) in sleep only.** [apple-RE] (`../pp5022-reference/apple-power.md` §2.2) Our codec half of that probe (S-4) has not been run.
- **Probe (read-only, owner hardware):** a multimeter on pins 13 (DCVDD), 14 (DBVDD), 31 (AVDD1) and 26 (AVDD2) against ground while playing. No firmware probe is safe: lowering a shared rail under the analogue floor risks distortion or a brown-out. Status: [hypothesis] until measured (`open-questions.md` 2).

## 4. The lowest-power headphone-playback configuration (candidate)

Pending listening tests and a battery-slope A/B (we do not run current-meter tests for now):

1. DSP core in low-power mode, `M128ENB` = 0 whenever EQ band 4 is flat and the 3D stage is off (#265).
2. OUT1, both DACs and both output mixers on, everything else off (as now).
3. `DACOSR128` = 0 (64×), if inaudible.
4. BCLK at 32 fs (`BCLKDIV` ÷8), if the PP's I2S block and any BCM clock user accept it.
5. PLL and clocks only while PCM runs (as now); MCLK gated in standby once the AW-5 late resumes are understood.
6. DCVDD as low as the board allows (unknown).

On the datasheet's numbers the digital core is a large share of the quiescent playback current (DCVDD 4.8 of 9.6 mA at 2.5 V), so the digital-side levers (oversampling, core mode, interface clock) are the ones to measure. [datasheet; estimate]
