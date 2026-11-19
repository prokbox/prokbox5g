# Register summary (WM-A)

*Every register of the WM8758B, from the datasheet's register map (Rev 4.4, Table 59), with what our driver writes and why. Field positions are [datasheet]; the macro names are Rockbox's (`firmware/export/wm8758.h`), checked field by field against Table 59 on 2026-11-07 for every register the driver defines: they match. Our values are [rockbox-derived] unless marked [ours]. Registers are 9 bits wide and write-only (`bring-up.md` §1).*

## 1. Coverage at a glance

| Status | Registers |
|---|---|
| **Written by the shipping build** | R1–R4, R6, R7, R10–R12, R18–R22, R36–R39, R42, R49–R57, R61 |
| Written only by code we never call | R44–R48 (`audiohw_enable_recording()`, `audiohw_set_recvol()`; no recording on our build); R54–R55 also by `audiohw_set_lineout_volume()` |
| **Never written** (reset value, or whatever a previous firmware left: quirk 21) | R0, R5, R8, R9, R13, R14–R16, R24–R25, R27–R30, R32–R35, R41, R43 |
| Do not exist | R17, R23, R26, R31, R40, R58–R60 |

Unused but valuable: R24–R25 (DAC limiter and its +12 dB boost) and R41 (3D). See `features.md`.

## 2. Register by register

"Ours" lists every value the ProkBox build writes, in the order it first writes them.

### Power and clocking

| Reg | Name | Reset | Fields (bit: name) | Ours | Why |
|---|---|---|---|---|---|
| R0 `0x00` | Software reset | — | any write resets all registers | never | The power-on reset is relied on (quirk 21) |
| R1 `0x01` | `PWRMGMT1` | `0x000` | 7 `OUT4MIXEN`, 6 `OUT3MIXEN`, 5 `PLLEN`, 4 `MICBEN`, 3 `BIASEN`, 2 `BUFIOEN`, 1:0 `VMIDSEL` (00 off, 01 100 kΩ, 10 500 kΩ, 11 10 kΩ) | `0x02F` (preinit, 10 kΩ fast charge); `0x02E` (postinit, 500 kΩ); standby `0x00E` [ours]; close `0x028` then `0x000` | PLL, bias and buffers on; VMID at 500 kΩ while running to cut VMID current. Shadowed |
| R2 `0x02` | `PWRMGMT2` | `0x000` | 8 `ROUT1EN`, 7 `LOUT1EN`, 6 `SLEEP`, 5:4 `BOOSTEN` R/L, 3:2 `INPPGAEN` R/L, 1:0 `ADCEN` R/L | `0x180`; standby `0x000` [ours]; close `0x000` | Headphone amps only. `SLEEP` unused (`power.md` §2). Shadowed |
| R3 `0x03` | `PWRMGMT3` | `0x000` | 8 `OUT4EN`, 7 `OUT3EN`, 6 `LOUT2EN`, 5 `ROUT2EN`, 3 `RMIXEN`, 2 `LMIXEN`, 1 `DACENR`, 0 `DACENL` | `0x00F`; standby steps `0x003`, `0x000` [ours]; `0x06F` only if `audiohw_enable_lineout(true)` | DACs and output mixers; OUT2 is off on our unit. Shadowed |
| R4 `0x04` | `AINTFCE` | `0x050` | 8 `BCP`, 7 `LRP`, 6:5 `WL` (00 16, 01 20, 10 24, 11 32 bit), 4:3 `FMT` (00 right-j., 01 left-j., 10 I2S, 11 DSP), 2 `DLRSWAP`, 1 `ALRSWAP`, 0 `MONO` | `0x010` | I2S, 16-bit (the PP's I2S block runs 16-bit) |
| R5 `0x05` | Companding | `0x000` | 5 `WL8`, 4:3 `DAC_COMP`, 2:1 `ADC_COMP`, 0 `LOOPBACK` | never | Not needed |
| R6 `0x06` | `CLKCTRL` | `0x140` | 8 `CLKSEL` (1 = PLL), 7:5 `MCLKDIV` (÷1, 1.5, 2, 3, 4, 6, 8, 12), 4:2 `BCLKDIV` (÷1, 2, 4, 8, 16, 32), 0 `MS` | `0x001` (preinit, briefly), then per rate `0x145`–`0x1E5` (`bring-up.md` §2.4) | PLL clocking, SYSCLK = 256 fs, BCLK = 128 fs, codec is master |
| R7 `0x07` | `ADDCTRL` | `0x000` | 8 `M128ENB` (0 = DSP low-power mode), 3:1 `SR` (48, 32, 24, 16, 12, 8 kHz), 0 `SLOWCLKEN` | per rate `0x001`–`0x00B`; `M128ENB` by its three users (quirk 28) | Filter coefficients for the rate; slow clock for the zero-cross timeout. Shadowed without `M128ENB` |
| R8 `0x08` | GPIO | `0x000` | 8 `GPIO1GP`, 7 `GPIO1GPU`, 6 `GPIO1GPD`, 5:4 `OPCLKDIV`, 3 `GPIO1POL`, 2:0 `GPIO1SEL` (input, temp ok, amute active, PLL clock out, PLL lock, logic 1, logic 0) | never | The CSB/GPIO1 pin's wiring is unknown (`open-questions.md` 3) |
| R36 `0x24` | `PLLN` | `0x008` | 4 `PLLPRESCALE` (÷2 on MCLK), 3:0 `PLLN` | `0x017` (44.1 family) or `0x018` (48 family) | 24 MHz MCLK halved to 12 MHz |
| R37 `0x25` | `PLLK1` | `0x00C` | 5:0 `PLLK[23:18]` | `0x021` or `0x00C` | `bring-up.md` §2.3 |
| R38 `0x26` | `PLLK2` | `0x093` | 8:0 `PLLK[17:9]` | `0x161` or `0x093` | |
| R39 `0x27` | `PLLK3` | `0x0E9` | 8:0 `PLLK[8:0]` | `0x026` or `0x0E9` | |

### Jack detect (unused)

| Reg | Name | Reset | Fields | Ours | Why |
|---|---|---|---|---|---|
| R9 `0x09` | Jack detect control | `0x000` | 8:7 `JD_VMID`, 6 `JD_EN`, 5:4 `JD_SEL` (GPIO1–3) | never | Headphone insertion is sensed on the PP side [rockbox-derived] |
| R13 `0x0D` | Jack detect control 2 | `0x000` | 7:4 `JD_EN1`, 3:0 `JD_EN0` (output enables per jack state) | never | As above |

### DAC path

| Reg | Name | Reset | Fields | Ours | Why |
|---|---|---|---|---|---|
| R10 `0x0A` | `DACCTRL` | `0x000` | 6 `SOFTMUTE`, 3 `DACOSR128` (0 = 64×, 1 = 128×), 2 `AMUTE`, 1 `DACRPOL`, 0 `DACLPOL` | `0x008` playing (`0x000` at 88.2/96 kHz at 64×); `0x040` muted, standby and close | 128× at 48 kHz and below; at 88.2/96 kHz 64× (p083-dacosr64) unless Settings > Audiophile > Hi-Res 128x is on and that rate passed its 128× self-test (0.9.0, `dac_set_osr128()`; a change while playing goes under a soft-mute ramp, assumed done in ~30 ms, not timed on silicon); soft mute on pause, standby and close. Bit 6 = 0 plays [silicon]; the datasheet's own statements on this bit disagree (quirk 10). Shadowed |
| R11 `0x0B` | `LDACVOL` | `0x0FF` | 8 `DACVU`, 7:0 volume (0 = mute, 1 = −127 dB, … 255 = 0 dB, 0.5 dB steps) | per volume, minus 2 × precut (`playback-path.md` §3) | Fine and low-end volume; EQ headroom |
| R12 `0x0C` | `RDACVOL` | `0x0FF` | as R11 | as R11, with `DACVU` | `DACVU` on the second write updates both sides together |
| R18 `0x12` | `EQ1` (low shelf) | `0x12C` | 8 `EQ3DMODE` (1 = EQ and 3D on the DAC path), 6:5 `EQ1C` (80, 105, 135, 175 Hz), 4:0 `EQ1G` | `0x100` \| cutoff \| gain; bass when the hardware EQ is off | Always on the DAC path |
| R19 `0x13` | `EQ2` (peak 1) | `0x02C` | 8 `EQ2BW` (1 = wide), 6:5 `EQ2C` (230, 300, 385, 500 Hz), 4:0 `EQ2G` | preset value, or `0x02C` (flat) when off | |
| R20 `0x14` | `EQ3` (peak 2) | `0x02C` | 8 `EQ3BW`, 6:5 `EQ3C` (650, 850 Hz, 1.1, 1.4 kHz), 4:0 `EQ3G` | as R19 | |
| R21 `0x15` | `EQ4` (peak 3) | `0x02C` | 8 `EQ4BW`, 6:5 `EQ4C` (1.8, 2.4, 3.2, 4.1 kHz), 4:0 `EQ4G` | as R19 | |
| R22 `0x16` | `EQ5` (high shelf) | `0x02C` | 6:5 `EQ5C` (5.3, 6.9, 9, 11.7 kHz), 4:0 `EQ5G` | preset value; treble when the EQ is off | |
| R24 `0x18` | `DACLIMITER1` | `0x032` | 8 `LIMEN`, 7:4 `LIMDCY`, 3:0 `LIMATK` | 0.9.0 (#203): `0x1A2` (on, decay 768 ms, attack 375 µs) while the EQ is on and the switch allows; `0x032` otherwise. Switch default on since b18 (`DAC_LIMITER_DEFAULT 1`) | `features.md`, `playback-path.md` §5 |
| R25 `0x19` | `DACLIMITER2` | `0x000` | 6:4 `LIMLVL` (−1 … −6 dB), 3:0 `LIMBOOST` (0 … +12 dB; works with the limiter off) | 0.9.0 (#203): `0x000` (−1 dB, no boost), written before `LIMEN` | Was known-unused |
| R41 `0x29` | `THREEDCTRL` | `0x000` | 3:0 `DEPTH3D` (0 … 100 % in 16 steps) | never | Known-unused |

EQ gain codes (`EQnG`, all five bands): `00000` = +12 dB, 1 dB per step, `01100` = 0 dB, `11000`–`11111` = −12 dB. [datasheet, Table 30] `EQ_GAIN_VALUE(g)` = `(12 − g) & 0x1F`; `EQ_CUTOFF_VALUE(c)` = `(c − 1) << 5` with `c` 1–4. [rockbox-derived]

### ADC path (unwired on our unit)

| Reg | Name | Reset | Fields | Ours | Why |
|---|---|---|---|---|---|
| R14 `0x0E` | `ADCCTRL` | `0x100` | 8 `HPFEN`, 7 `HPFAPP`, 6:4 `HPFCUT`, 3 `ADCOSR128`, 1:0 polarity | never | No recording |
| R15–R16 `0x0F`–`0x10` | `LADCVOL`, `RADCVOL` | `0x0FF` | 8 `ADCVU`, 7:0 volume | never | |
| R27–R30 `0x1B`–`0x1E` | Notch filter 1–4 | `0x000` | `NFU`, `NFEN`, coefficients `NFA0`, `NFA1` | never | |
| R32–R34 `0x20`–`0x22` | ALC control 1–3 | `0x038`, `0x00B`, `0x032` | `ALCSEL`, `ALCMAX`/`MIN`, `ALCHLD`/`LVL`, `ALCMODE`/`DCY`/`ATK` | never | ALC is ADC-side only |
| R35 `0x23` | Noise gate | `0x000` | 3 `NGEN`, 2:0 `NGTH` | never | |
| R44 `0x2C` | `INCTRL` | `0x003` | 8 `MBVSEL`, 6/2 `R2_2INPPGA`/`L2_2INPPGA`, 5/1 `RIN2`/`LIN2`, 4/0 `RIP2`/`LIP2` | recording only: `0x044` | Rockbox's line-in path (dock) |
| R45–R46 `0x2D`–`0x2E` | Input PGA gain L/R | `0x010` | 8 `INPGAVU`, 7 `ZC`, 6 `MUTE`, 5:0 gain (−12 … +35.25 dB, 0.75 dB) | recording only | |
| R47–R48 `0x2F`–`0x30` | ADC boost L/R | `0x100` | 8 `PGABOOST`, 6:4 `L2_2BOOSTVOL`/`R2_2BOOSTVOL` | recording only: `0x050` | |

### Mixers and outputs

| Reg | Name | Reset | Fields | Ours | Why |
|---|---|---|---|---|---|
| R42 `0x2A` | `OUT4TOADC` | `0x000` | 8:6 `OUT4_2ADCVOL`, 5 `OUT4_2LNR`, 4 `VMIDTOG`, 3 `OUT2DEL`, 2 `POBCTRL`, 1 `DELEN`, 0 `OUT1DEL` | `0x004` then `0x000` (power-up); `0x010` (close) | Pop control (`bring-up.md` §4). `DELEN`, `OUT1DEL`, `OUT2DEL` are named in the map but not described in Rev 4.4 (quirk 20) |
| R43 `0x2B` | Beep control (bypass routing) | `0x000` | 8 `BYPL2RMIX`, 7 `BYPR2LMIX`, 2 `DELEN2` | never | No beep generator exists despite the name; Apple writes `DELEN2` during power-up (`stock-firmware-vs-ours.md` §3) |
| R49 `0x31` | `OUTCTRL` | `0x002` | 8 `HP_COM`, 7 `LINE_COM`, 6 `DACL2RMIX`, 5 `DACR2LMIX`, 4 `OUT4ENDEL`, 3 `OUT3ENDEL`, 2 `TSOPCTRL`, 1 `TSDEN`, 0 `VROI` | `0x187` (running); `0x101` (close) | Common-ground feedback (polarity caveat, quirk 9), thermal shutdown, 30 kΩ tie-off for disabled outputs |
| R50 `0x32` | `LOUTMIX` | `0x001` | 4:2 `BYPLMIXVOL` (−15 … +6 dB), 1 `BYPL2LMIX`, 0 `DACL2LMIX` | `0x001` | DAC only |
| R51 `0x33` | `ROUTMIX` | `0x001` | as R50, right | `0x001` | |
| R52 `0x34` | `LOUT1VOL` | `0x039` | 8 `OUT1VU` (`HPVU`), 7 `LOUT1ZC`, 6 `LOUT1MUTE`, 5:0 gain (0 = −57 dB … 57 = 0 dB … 63 = +6 dB, 1 dB) | `0x140` (preinit), then `0x080` \| gain, or `0x0C0` (mute) | Headphone amp; zero-cross on |
| R53 `0x35` | `ROUT1VOL` | `0x039` | as R52, right | as R52 with `OUT1VU` (`0x180` \| gain) | Update bit on the second write moves both sides together |
| R54 `0x36` | `LOUT2VOL` | `0x039` | 8 `OUT2VU` (`SPKVU`), 7 `ZC`, 6 `MUTE`, 5:0 gain | `0x140` (preinit) | OUT2 stays off |
| R55 `0x37` | `ROUT2VOL` | `0x039` | as R54 | `0x140` (preinit) | |
| R56 `0x38` | `OUT3MIX` | `0x001` | 6 `OUT3MUTE`, 3 `OUT4_2OUT3`, 2 `BYPL2OUT3`, 1 `LMIX2OUT3`, 0 `LDAC2OUT3` | `0x040` | Muted, no inputs |
| R57 `0x39` | `OUT4MIX` | `0x001` | 7 `OUT3_2OUT4`, 6 `OUT4MUTE`, 5 `OUT4ATTN`, 4 `LMIX2OUT4`, 3 `LDAC2OUT4`, 2 `BYPR2OUT4`, 1 `RMIX2OUT4`, 0 `RDAC2OUT4` | `0x040` | Muted, no inputs |
| R61 `0x3D` | `BIASCTRL` | `0x000` | 8 `BIASCUT`, 6 `HALFI_IPGA`, 5:4 `BUFBIAS`, 3:2 `ADCBIAS`, 0 `HALFOPBIAS` | `0x100` (preinit), `0x000` (postinit) | Low bias during start-up only. `BUFBIAS`, `ADCBIAS`, `HALFI_IPGA` are not described in the text: leave at 0. `HALFOPBIAS`: "do not use" |

The headphone-volume value `0x140` written at start-up is the datasheet's step-4 value: update bit set, mute set, gain 0 (−57 dB). Note the datasheet names the OUT1 and OUT2 update bits `HPVU` and `SPKVU` in Table 33 and Table 35, and `OUT1VU`/`OUT2VU` in the register map; they are the same bits.
