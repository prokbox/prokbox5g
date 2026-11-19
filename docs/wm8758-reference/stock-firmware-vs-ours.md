# Apple's codec set-up against ours

*How Apple's firmware drives the WM8758, register by register as far as the decoded values go, beside what our driver writes. Apple's values say what Apple's code does; any reason we give is our reading. Addresses: `osos` = Apple's main OS image (1.3 unless noted); the boot driver is the codec code in Apple's boot-time image (firmware 25.1.3). No Apple code is quoted.*

## 1. Where Apple's values come from

| Source | What it shows | Evidence |
|---|---|---|
| osos codec layer `0x1026_5E54` (numbered power modes), back end 3 `0x1028_39B8` picked for board 11 by `0x1026_53A4`; every codec write through `0x1028_40D8` (I2C device `0x1A`) | Power-up (mode 4), full off (modes 1, 2), "running" (mode 3) | [apple-RE] (`../pp5022-reference/apple-power-catalog.md` §2.7, `../pp5022-reference/apple-power.md` §5.3). Board 11 = 5G/5.5G is [hypothesis] |
| Boot driver `0x0018_BCC4`–`0x0018_BD24` | PLL, clock control and interface format at boot | [apple-RE, under our emulator, 2026-11-07]. Its I2C writes pack both data bytes into one word; the values below assume that packed reading, which bench row `i2creg` will confirm (`bring-up.md` §1.2) |
| osos `0x1015_BFF0` | For video, swaps the clock master: WM8758 R6 bit 0 and the PP's `IISCONFIG` bit 25 set opposite | [apple-RE] (`../pp5022-reference/audio.md` §6) |
| Apple's I2C codec helper | Same two-byte format as ours | [external] siggi `17` |

We have not decoded Apple's per-rate clock code (if any) beyond the boot driver's 44.1 kHz set. [unknown] Its volume and EQ writes are recorded in §7. [emulated]

## 2. Clocking and the interface (boot driver vs ours)

| Register | Apple (boot) | Ours (44.1 kHz) | Difference and what it implies |
|---|---|---|---|
| R36 `PLLN` | `0x007`: N = 7, **no prescaler** | `0x017`: N = 7, prescaler ÷2 | Apple feeds the PLL its MCLK directly; we halve ours |
| R37–R39 `PLLK` | `0x021`, `0x151`, `0x1AC` (K = `0x86A3AC`, R = 7.525935) | `0x021`, `0x161`, `0x026` (K = `0x86C226`, R = 7.526400) | Apple's K is the datasheet's Table 49 row for MCLK 12 MHz → 11.2889 MHz. With f1 = 12 MHz that gives fs = 44,097.3 Hz, **62 ppm slow**; ours is exact to 0.01 ppm [datasheet; arithmetic ours] |
| PP MCLK (`0x7000_0018` bits 3:2) | not decoded; **12 MHz implied** (with 24 MHz and no prescaler, f2 would be about 181 MHz, double the PLL's working point) | cleared: 24 MHz | [hypothesis] for Apple; [rockbox-derived] [silicon `preg`] for ours |
| R6 `CLKCTRL` | `0x14D`: PLL, MCLKDIV ÷2, **BCLKDIV ÷8**, master | `0x145`: PLL, MCLKDIV ÷2, **BCLKDIV ÷2**, master | Same SYSCLK (256 fs). Apple's BCLK = 32 fs (1.41 MHz), exactly 16 bits per channel; ours 128 fs (5.64 MHz). Both play; whether the slower clock saves power is `open-questions.md` 7 |
| R4 `AINTFCE` | `0x010`: I2S, 16-bit | `0x010` | Same |

## 3. Power-up: Apple's mode 4 against our preinit and postinit

Apple's order (osos, back end 3) [apple-RE] with our reading of each value [datasheet field names]:

| Apple step | Register | Apple | Apple's meaning | Ours (`bring-up.md` §4.2) |
|---|---|---|---|---|
| 1 | R61 | `0x101` | `BIASCUT` + `HALFOPBIAS` | `0x100` (`BIASCUT` only) |
| 1 | R49 | `0x186` | `HP_COM`, `LINE_COM`, `TSOPCTRL`, `TSDEN` | `0x187` (the same plus `VROI`) |
| 1 | R42 | `0x006` | `POBCTRL` + `DELEN` | `0x004` (`POBCTRL`) |
| 1 | R43 | `0x004` | `DELEN2` | never written |
| 2 | R2 | `0x180` | OUT1 on | `0x180` |
| 2 | R3 | `0x060` | **OUT2 on** (outputs before the DACs) | — (OUT2 never on) |
| 3 | R49 | `0x10F` → `0x10D` → `0x104` | `HP_COM` with `OUT3ENDEL`, `TSOPCTRL`, `TSDEN`, `VROI`; then `TSDEN` off; then only `HP_COM` + `TSOPCTRL` | — |
| 3 | R43 | `0x000` | `DELEN2` off | — |
| 4 | R3 | `0x06F` | OUT2, mixers, DACs | `0x00F` (no OUT2) |
| 4 | R1 | `0x02D` | PLL, `BIASEN`, `BUFIOEN`, **VMID 100 kΩ** (the datasheet's `01`) | `0x02F` (VMID 10 kΩ), later `0x02E` (500 kΩ) |
| 5 | — | **wait 100 ms** | the datasheet's step 11 | no wait in the driver |
| 6 | R52–R55 | "0 with update"; the emulator run in §7 records R52 `0x000`, R53 `0x100` (OUT1 at −57 dB, not muted), R54 `0x039`, R55 `0x139` (OUT2 at 0 dB) [emulated] | OUT1 and OUT2 volumes set, update bits | `0x140` at preinit, real volume later |
| 6 | R61 | `0x100` | **`BIASCUT` kept on** while running | `0x000` at postinit |
| 6 | R49 | `0x100` | final: `HP_COM` only; **thermal shutdown off**, `LINE_COM` off, `VROI` off | `0x187` kept while running |
| — | R42 | not shown as cleared after step 1 in the catalog's decode | `POBCTRL`/`DELEN` end state not recorded | `0x000` at the end of preinit |

What the comparison says:

1. **Apple follows the datasheet's order more closely**, including the 100 ms VMID wait and the 100 kΩ `VMIDSEL` of step 9. Ours charges VMID ten times faster (10 kΩ), clears `POBCTRL` before VMID has risen, and does not wait. [apple-RE] [rockbox-derived] Neither order has been checked for a pop on our unit (`open-questions.md` 9).
2. **Apple uses the output-delay bits** (`DELEN`, `DELEN2`, `OUT3ENDEL`) during power-up. Rev 4.4 names them in the register map but does not describe them; they look like the anti-pop "delayed enable" features of the WM89xx family. [apple-RE; meaning hypothesis] (quirk 20, `open-questions.md` 18)
3. **Apple's running state differs from ours in six places:** `BIASCUT` on (ours off), thermal shutdown off (ours on), `LINE_COM` off (ours on), `VROI` off (ours on), OUT2 enabled (ours off) and VMID at 100 kΩ (ours 500 kΩ). Which of these change sound or current is unknown. [apple-RE] (`open-questions.md` 20)
4. Apple briefly sets `HALFOPBIAS` during power-up, the bit Wolfson says not to use; it is cleared by the final R61 write. [apple-RE] [datasheet]
5. **Mode 3** writes nothing; it records "running". [apple-RE]

## 4. Power-down, pause and sleep

| | Apple | Ours |
|---|---|---|
| Pause or stop | **Nothing**: the codec stays fully powered while awake [apple-RE] | DAC standby 5 s after PCM stops: soft mute, OUT1, mixers, DACs, PLL off; VMID, bias and buffers kept; wake 6.4 ms median [ours] [silicon] |
| Sleep / pad parking (modes 1, 2) | Mute OUT1/OUT2; R49 = `0x180` (`HP_COM`, `LINE_COM`); R1, R2, R3 = 0 (**VMID off**); then `DEV_EXTCLOCKS` cleared [apple-RE] | No sleep yet; planned to copy Apple's full off with its 100 ms wake (T-CODECOFF) |
| Shutdown | (mode 1 as above) | `audiohw_close()`: soft mute, R49 `0x101`, `VMIDTOG`, VMID off, **300 ms** discharge wait, R1–R3 = 0 [rockbox-derived] |
| Codec MCLK (`DEV_EXTCLOCKS`) | Set around every mode change; cleared with the codec's power in modes 1 and 2 [apple-RE] | Always on; the AW-5 gate in standby failed on the bench (2 of 6 resumes late) [silicon, sitting 4gh, 2026-11-06] |
| I2S block (`DEV_I2S`) | Gated only when the audio engine stops for disk mode, never on a pause [apple-RE] | Always on |

**We already beat Apple on pause**: Apple's codec draws its full playback power (about 24–27 mW on the datasheet's figures) through every pause, ours about 0.3 mW after 5 s. [apple-RE; datasheet; estimate]

Apple does not use `VMIDTOG` or a timed discharge wait in the decoded mode 1 [apple-RE as decoded]; ours does both.

## 5. Video: the clock master swap

For video playback Apple's code can make the PP the I2S master and the codec the slave (R6 bit 0 and `IISCONFIG` bit 25 set opposite). [apple-RE] We never do: the codec stays master, and the BCM's audio block listens to its clocks (`../bcm2722-reference/audio.md`, `../data-paths.md` R9).

## 6. What we could take from Apple

| Idea | Gain | Risk | Tag | Probe |
|---|---|---|---|---|
| BCLK at 32 fs (R6 `0x14D` at 44.1/48 kHz, the matching MCLKDIV elsewhere) | Fewer edges on BCLK, which also reaches the BCM's pins; DBVDD current | The PP's I2S slave and the BCM's audio block must accept 16 clocks per slot | [apple-RE]; gain [hypothesis] | `open-questions.md` 7 |
| MCLK at 12 MHz, PLL without prescaler | Half the MCLK pin's edges | Needs the PP's clock-select field decoded (`0x7000_0018`); our exact K must be recomputed (R = 7.5264 needs no prescaler) | [hypothesis] | `open-questions.md` 8 |
| The datasheet's power-up order with the 100 ms wait | A quieter boot, if ours pops | 100 ms on the first play after boot (inside boot time) | [datasheet]; [apple-RE] | `open-questions.md` 9 |
| Full codec off in sleep (VMID too) | About 0.27 mW more than our standby | A pop or a 100+ ms wake: sleep only | [apple-RE] | T-CODECOFF |
| Not Apple's K | Apple's 44.1 kHz is 62 ppm slow; ours is exact | — | [datasheet] | none: keep ours |

## 7. EQ and volume: what Apple 1.3 writes [emulated]

**Source:** emulator run, 2026-11-07. Apple's firmware 1.3 (Settings › About reads "Version 1.3") booted in the iPod Video emulator (branch `i2c-trace`, commit `69e137b`), driven through its menus from the control socket, with one test track (a generated 440 Hz tone) added to the disk so that something could play. Every I2C send the controller started was logged with the emulated clock, whichever routine sent it, so each burst can be tied to the wheel or button event before it. Values are decoded as 7-bit register plus 9-bit value. Most of Apple's codec writes pack both data bytes into one word store; reading them that way is hypothesis H2 (`bring-up.md` §1.2, bench row `i2creg`). Every value below that came from such a write depends on H2. The eight writes from Apple's byte-store routine (the power-off at 137 ms) do not.

### 7.1 The answer

| Question | Apple 1.3 in the emulator |
|---|---|
| Codec EQ, R18–R22 | **Never written**, at boot, on any preset change or during playback. The codec's EQ stays at reset: `EQ3DMODE` = 1 (EQ on the DAC path) and all five bands at 0 dB, so it passes audio flat |
| DAC limiter, R24/R25 (`LIMEN`, `LIMLVL`, `LIMBOOST`) | **Never written**: the limiter stays off and `LIMBOOST` stays at 0 dB |
| DAC volume, R11/R12 | **Never written**: the digital volume stays at its reset value, 0 dB |
| Output volumes, R52–R55 | **The only registers the volume changes** (R52/R53, OUT1, the headphone amplifier). R54/R55 (OUT2) are written only at boot |
| Selecting a preset | **No codec write at all**, whether a track is playing (Off, Flat, Bass Booster, Dance, and Flat again) or nothing is (Flat, with no music on the disk). No codec write came within 5 s of any of these selections |
| Precut or gain change per preset | **None on the codec, and none in the samples either** (§7.4): the volume range and ceiling (R52/R53 gain `0x3B`, +2 dB) are the same under every preset, and the samples sent to the codec carry each boost at full size, with no precut. Boosting presets clip the samples on loud material |

**What follows:** Apple 1.3 does its EQ in software, on the samples, and leaves the codec's EQ and limiter alone. It takes no headroom for it: §7.4 measures the samples. [emulated]

### 7.2 Write table, per step

| Step (emulated time) | Writes, in order | Decoded |
|---|---|---|
| Boot, 31 ms | R0 `0x001`; R61 `0x101`; R49 `0x186`; R54, R55 `0x179`; R56, R57 `0x040`; R42 `0x006`; R43 `0x004`; R2 `0x180`; R3 `0x060`; R42 `0x00F`, `0x00D`; R43 `0x000`; R42 `0x004`; R3 `0x06F`; R1 `0x02D`; R4 `0x010`; R6 `0x14D`; R7 `0x000`; R14 `0x000`; R44 `0x000`; R45 `0x040`; R46 `0x140`; R6 `0x14D`; R36 `0x007`; R39 `0x1AC`; R38 `0x151`; R37 `0x021`; R50, R51 `0x015`; R10 `0x003`; R47, R48 `0x070` | Software reset first. Power-up as in §3 (mode 4); OUT2 muted at 0 dB. PLL and clocking as in §2. R50/R51: DAC to the output mixers, bypass gain field at 0 dB but bypass not routed. **R10 `0x003`: `DACLPOL` and `DACRPOL` set, both DAC channels inverted**, 64× oversampling, no soft mute. ADC side (R14, R44–R48) set although nothing records |
| Boot, 131 ms (100 ms later) | R52 `0x000`; R53 `0x100`; R54 `0x039`; R55 `0x139`; R61 `0x100`; R42 `0x000` | OUT1 at −57 dB, OUT2 at 0 dB, both unmuted, update bit on the right side; `BIASCUT` kept on; output delays off |
| Boot, 137 ms (Apple's byte-store routine) | R52 `0x040`; R53 `0x1C0`; R54 `0x040`; R55 `0x1C0`; R49 `0x180`; R1, R2, R3 `0x000` | Both outputs muted; `HP_COM`, `LINE_COM`; **codec fully off, VMID too** (modes 1 and 2 in §4) |
| Boot, 1.04 s | R54 `0x0B9`; R55 `0x1B9` | OUT2 at 0 dB with zero cross, unmuted |
| First button press after boot | R52 `0x09E`; R53 `0x19E`; R6 `0x14D` (twice); R4 `0x010` (twice); R1 `0x02D`; R39 `0x1AC`; R38 `0x151`; R37 `0x021`; R36 `0x007`; R6 `0x14D`; R7 `0x000` | OUT1 at −27 dB; clocks and PLL again; VMID 100 kΩ, bias, buffers and PLL on. **R2 and R3 are not written** (see 7.3) |
| Play pressed on a track (EQ Off) | R52 `0x099`; R53 `0x199`; 142 ms later R6 `0x14D` (twice) | OUT1 set to the stored volume (−32 dB); clock control rewritten unchanged. Track restart: R6 `0x14D` once |
| EQ → Flat, Bass Booster, Dance, Off (while playing) | **none** | — |
| EQ → Flat (nothing playing, no music on disk) | **none** | — |
| Each volume step, any preset | R52 `0x080` \| g, then R53 `0x180` \| g | `LOUT1ZC`, `ROUT1ZC` on; update bit on the second write moves both sides together. g runs from `0x00` (−57 dB) to `0x3B` (+2 dB), 1 dB per step; some wheel steps rewrite the same g and a fast spin can skip values |
| Volume at its lowest step | R52 `0x040`; R53 `0x1C0` | Mute bits set; zero cross off on the left write, on in the right |

Volume sweeps recorded in this run, all inside that range and under that ceiling: EQ Off, −28 → −26 dB, and later +2 → −22 → +2 dB; Flat, −28 → −32 dB, and later +2 → −8 → +2 dB; Bass Booster, −27 → +2 dB → mute; Dance, −51 → +2 dB. After boot, 618 of the run's 684 codec writes were to R52/R53, and every volume change went to those two registers alone.

### 7.3 What this run cannot show, and the oddities it found

- **H2.** Every packed write above, including all the volume writes, depends on hypothesis H2 until bench row `i2creg` settles it. The decoded values form a coherent picture (a valid PLL set, a 1 dB volume ladder), which supports it but does not prove it.
- **The software EQ itself.** Its exact curves, and whether it runs on the CPU or the COP, are not on the bus. Its effect on levels is measured in §7.4.
- **R2 and R3 stay 0.** After the full power-off at 137 ms, nothing in this run turns OUT1, the mixers or the DACs back on, yet tracks play and the volume is written. On the device something must re-enable them. The emulator may not reach that path (headphone sensing, for example), or Apple may re-enable them through a route we have not seen. Not settled. [emulated, unexplained]
- **R10 = `0x003`** (both DAC polarities inverted) is a single boot write, and no later write changes it. Our driver writes `0x008`. An inversion on both channels is inaudible on its own; whether the board inverts again after the DAC is not known. Whether the board inverts again is `open-questions.md` 24.
- **Where Apple's codec writes come from.** The emulator reports `0x0018_BD24` as the send site for every packed write. In this run the OS image is loaded straight to SDRAM, with no Apple bootloader in the chain, and the same site carries the volume writes minutes into playback. So the "boot driver" of §1 is resident code in the running OS, not only boot-time code. The reported PC does not land on a store at that offset of the OS image, so the exact site is unverified. The power-off at 137 ms goes through a second routine that stores one byte per data register, which is the same routine Apple uses for the PMU.
- **Settings not exercised:** Sound Check, Volume Limit, the other 18 presets, line out and the dock.

### 7.4 What the software EQ does to the samples [emulated]

**Source:** emulator run, 2026-11-07, Apple 1.3, emulator branch `i2c-trace` commit `1513e12` (its `wav PATH i2s` control-socket tap, which records the I2S stream the codec receives, 16-bit, bit for bit).

**Test track.** Generated here, no outside media: a 1 kHz sine at −6 dBFS on both channels plus independent pink noise at −20 dBFS RMS per channel. Stereo, 44.1 kHz, 10 minutes, encoded as a 320 kbit/s CBR MP3 (decoded peak −0.25 dBFS, no clipping in the source).

**Method.**
- One capture per preset, each started 2 s or more after the preset was chosen. Each choice was confirmed by screenshot: the highlighted row before the press, and the list opening on that preset when it was entered again.
- R52/R53 stayed at `0x099` (−32 dB) for every capture, from the I2C log.
- The tap carries **the samples the codec receives**, before any codec volume. Apple's volume is analogue (R52/R53, §7.1) and the digital volume R11/R12 is never written, so the volume step cannot change these numbers.
- Each measure is a 10 s window starting 1.5 s into the steady part:
  - the 1 kHz level by a Goertzel filter in 1 s blocks, as a sine peak in dBFS, averaged over both channels;
  - the noise as RMS with 1 kHz notched out (broadband), and in 1/3-octave bands at 63 Hz, 250 Hz, 4 kHz and 10 kHz;
  - clipping as samples at full scale.

| Preset | 1 kHz (dBFS) | Δ 1 kHz vs Off | Δ noise, broadband | Δ 63 Hz | Δ 250 Hz | Δ 4 kHz | Δ 10 kHz | Full-scale samples per 10 s (of 882,000) |
|---|---|---|---|---|---|---|---|---|
| Off | −6.02 | 0 | 0 | 0 | 0 | 0 | 0 | 0 (peak 30,497) |
| Flat | −6.02 | 0.00 | −0.1 | +0.4 | +0.1 | +0.1 | 0.0 | 0 |
| Bass Booster | −5.83 | +0.19 | +4.4 | +6.1 | +2.1 | +0.1 | 0.0 | 170 |
| Dance | −3.26 | +2.76 | +2.5 | +5.3 | +1.7 | +4.6 | +2.0 | 1,962 |
| Treble Booster | −5.36 | +0.66 | +2.1 | 0.0 | −0.1 | +4.5 | +5.8 | 3 |
| Loudness | −6.38 | −0.35 | +3.9 | +4.6 | +0.2 | −4.0 | +3.6 | 19 |

**What it shows.**

1. **Off and Flat are unity.** The 1 kHz level under Off (−6.02 dBFS) matches the source file's (−6.03). Flat matches Off within the noise of the measure. Apple applies no gain, no headroom and no attenuation when the EQ is off or flat.
2. **No precut.** Every boosting preset raises the level: the broadband noise goes up 2.1 to 4.4 dB, and the 1 kHz tone goes up wherever the preset's curve is above 0 dB there (Dance +2.8 dB). No preset lowers the overall level to make room. The 1 kHz change is only the curve's own value at 1 kHz.
3. **Apple's boosting presets clip the samples** on a hot track. This track peaks at −0.25 dBFS, about where mastered pop sits. Dance put 0.2 % of its samples at full scale, Bass Booster 0.02 %. None clipped under Off or Flat.
4. **The curves' shapes**, read at four bands, are what the names promise: Bass Booster about +6 dB at 63 Hz, falling to 0 by 4 kHz; Treble Booster about +5 to +6 dB at 4–10 kHz; Dance boosts both ends; Loudness boosts both ends and cuts the upper mids (−4 dB at 4 kHz).

**For our design.** Our codec presets carry a fitted precut (`playback-path.md`), so on loud material ours will play quieter than Apple's under the same preset, and will not clip where Apple's does. Matching Apple's loudness would mean dropping the precut and accepting the clipping. Keeping it is the better-sounding choice. The difference is an owner question, not a bug.

**What this cannot see.**

- **Resolution.** The same window measured twice on the source file, or at two places in it, differs by up to about 0.4 dB per 1/3-octave band (pink noise is random) and under 0.1 dB at 1 kHz. Band deltas under 0.5 dB are noise.
- **The tap is upstream of the codec's digital path.** It shows Apple's software EQ exactly as the codec receives it. It does not show the codec's own digital filtering or anything after it. The emulator's codec output model (`--audio-wav`, OUT1) is not claimed to be bit-exact to the part, and was not used.
- **Starved frames.** The emulator does not feed the I2S model in real time: 54 to 83 % of each capture's frames are runs of all-zero frames, where the FIFO ran dry. The script drops runs of eight or more all-zero frames before measuring. That splices the stream at 210 to 368 points per capture, which adds small clicks; it cannot change the 1 kHz level or the band levels by more than the resolution above. It would also remove true digital silence, but this track has none.
- **The frame clock is modelled as 88.2 kHz** (the emulator assumes a 24 MHz MCLK; Apple's set-up implies 12 MHz, §2). That changes the WAV header's rate, not the samples, so the analysis reads them at 44.1 kHz. The 1 kHz tone lands on the 1 kHz bin at the source level when read that way, which confirms the rate.
- **One track, one volume step, six presets.** The other 16 presets, Sound Check and Volume Limit were not measured. Neither was any interaction between the EQ and the volume in software: at −32 dB nothing suggests one, but a step near the top was not tried.
