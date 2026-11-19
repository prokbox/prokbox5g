# Bring-up: control interface, clocking, audio interface, power sequences

*Chapters WM-2, WM-3, WM-4, WM-10 and appendix WM-B. Values are the ones our driver writes today (`firmware/drivers/audio/wm8758.c`, ProkBox build). Register details: `registers.md`. Apple's sequences: `stock-firmware-vs-ours.md`.*

## 1. How the host talks to the chip (WM-2)

### 1.1 The wire format

The WM8758 has a 3-wire and a 2-wire control interface, chosen by its MODE pin (low = 2-wire). [datasheet, CONTROL INTERFACE, Table 52] On the iPod it is the 2-wire one: the codec answers at 7-bit address `0x1A` (`0011010`, fixed in 2-wire mode), on the same bus as the PMU at `0x08`. [datasheet] [rockbox-derived] `wmcodec-pp.c` [silicon: every boot plays sound]

One register write is one I2C write transaction of two data bytes:

| Byte | Bits | Content |
|---|---|---|
| address | 7:1, 0 | `0x1A`, R/W = 0 (write) |
| control byte 1 | 7:1 | register number (0–61) |
| | 0 | data bit 8 |
| control byte 2 | 7:0 | data bits 7:0 |

In C: `byte1 = (reg << 1) | ((val >> 8) & 1); byte2 = val & 0xFF`. [datasheet, Figure 42] [rockbox-derived] `wmcodec_write()`

- **Bus timing:** the codec accepts SCLK up to 526 kHz; low pulse ≥ 1.3 µs, high ≥ 600 ns, start/stop set-up and hold ≥ 600 ns. [datasheet, "Control interface timing – 2-wire mode"] The PP's bus speed is not known (`../pp5022-reference/gpio-i2c.md` §6).
- **Write only.** A read (R/W = 1) is not acknowledged: "if ... the R/W bit is '1' when operating in write only mode", the codec drops back to idle. [datasheet] So **no register can be read back**, ever. The driver must remember what it wrote.
- **No reset of its own.** A write of any value to R0 resets every register to its default; the power-on reset does the same when the supplies come up. [datasheet, RESETTING THE CHIP] Neither Rockbox nor ProkBox writes R0 (quirk 21).
- **Writes are ignored while the power-on reset holds the chip** (AVDD1 or DCVDD below threshold). [datasheet, INTERNAL POWER ON RESET CIRCUIT]

### 1.2 The PP side: two layouts that both work

The PP5022's I2C controller at `0x7000_C000` sends one to four bytes per transaction (`../pp5022-reference/gpio-i2c.md` §6). How the data bytes are laid out in its registers is an open question:

- **Rockbox** writes each data byte to its own byte-wide register, four bytes apart: `0x7000_C00C` = byte 1, `0x7000_C010` = byte 2, then sets the count and the start bit. [rockbox-derived] `i2c-pp.c` (`I2C_DATA(X)` at `0x7000_C00C + 4·X`) [silicon: works on every boot]
- **Apple's boot driver** (image version 25.1.3, `0x0018_BCC4`–`0x0018_BD24`) packs both bytes into **one word** written at `0x7000_C00C`. [apple-RE, under our emulator, 2026-11-07] Apple's firmware works on silicon too.

So either the controller takes both layouts, or one of the two readings of the hardware is wrong. [hypothesis] Bench row **`i2creg`** settles it (`open-questions.md` 1); the controller question itself belongs to the PP5022 manual (`../pp5022-reference/gpio-i2c.md` §6).

One cross-check supports the packed reading: decoded as packed words, Apple's boot driver writes PLLK `0x21`/`0x151`/`0x1AC`, which is exactly the datasheet's Table 49 example row for a 12 MHz master clock (§2.3). A wrong decode would be unlikely to land on a datasheet row. [hypothesis, strong]

### 1.3 Shadows, the write path and the write log [ours]

Because nothing reads back, every write in the ProkBox driver goes through one function, `wm_write()`:

1. it takes `dac_mtx`, a recursive mutex that nests over the I2C driver's own lock;
2. it stores the value in a **shadow** for the registers whose state the driver must reconstruct: R1–R3 (`pwr_reg[]`), R10 (`dacctrl_reg`), R7 without `M128ENB` (`addctrl_reg`), R18–R22 (`eq_reg[]`, plus `eq1_reg`/`eq5_reg` for bass and treble);
3. while the DAC is in standby (§5.3), it masks the value so the write cannot power a block back on early (`standby_mask()`); the full value is applied at the wake;
4. it appends `(reg << 9) | value` to `dac_reglog[]`, a 32-entry ring, and calls `wmcodec_write()`.

`dac_reglog_read()` copies the ring out. It was the evidence channel for the S1 EQ spike (`../phase15-brief.md` §3.1). **Since 2026-10-26 nothing on the device reads it** (the developer "EQ test" was removed in commit `372d7e6185`); only the simulator's stub serves it (quirk 2).

**Both cores call this driver** (the volume and EQ from the UI, which may run on the COP; the wake from the audio side). A mutex orders the cores but does not make their caches agree, so every shadow and standby word is in uncached shared memory (`XCORE_BSS`/`XCORE_DATA`). Before that change, a stale cached shadow replayed a full standby wake over music. [silicon, bench sitting 3, 2026-10-26] (`../pp5022-reference/quirks.md` 54)

I2C sleeps: never write the codec from an interrupt, FIQ or tick task. [rockbox-derived] [ours]

## 2. Clocking (WM-3)

### 2.1 Where the clocks come from

- **MCLK** (pin 11) is the PP5022's external device clock, enabled by `DEV_EN.DEV_EXTCLOCKS` and set to **24 MHz** by clearing bits 3:2 of `0x7000_0018`. [rockbox-derived] `wmcodec-pp.c` [silicon: `0x7000_0018` reads `0x5_0200` under our firmware, `../pp5022-reference/clocks-power.md` `preg`]
- **The PLL** multiplies the (optionally halved) MCLK; its output, after a fixed ÷4 and the `MCLKDIV` divider, is **SYSCLK = 256 · fs**. `CLKSEL` (R6 bit 8) picks the PLL rather than MCLK as SYSCLK's source. [datasheet, MASTER CLOCK AND PLL, Figure 40, Table 43]
- **BCLK and LRCK** are outputs (`MS` = 1, R6 bit 0): BCLK = SYSCLK ÷ `BCLKDIV`, LRCK = fs. [datasheet, Table 43]

```
MCLK 24 MHz ─► ÷2 (PLLPRESCALE) ─► f1 = 12 MHz ─► PLL × R ─► f2 ─► ÷4 ─► ÷MCLKDIV ─► SYSCLK = 256·fs
                                                                                     └► ÷BCLKDIV ─► BCLK
```

### 2.2 The arithmetic

The ratio R = f2 / f1 is set by `PLLN` (integer, R36 bits 3:0) and `PLLK` (24-bit fraction, split over R37 bits 5:0, R38 and R39):

    PLLN = int(R)          PLLK = int(2^24 · (R − PLLN))

Constraints: 5 < PLLN < 13; the PLL "performs best" with f2 near 90 MHz and is most stable at N = 8; SYSCLK must not exceed 12.288 MHz; MCLK into the PLL needs a cycle of at least 20 ns (50 MHz). [datasheet, Tables 48–49, "System clock timing"]

Worked number, 44.1 kHz on our clock: SYSCLK = 256 × 44,100 = 11.2896 MHz; with MCLKDIV ÷2, f2 = 4 × 2 × 11.2896 = 90.3168 MHz; R = 90.3168 / 12 = 7.5264; PLLN = 7; PLLK = int(2^24 × 0.5264) = 8,831,526 = `0x86C226`, written as R37 = `0x21`, R38 = `0x161`, R39 = `0x026`. [rockbox-derived values; arithmetic ours, checked with Python 2026-11-07]

### 2.3 The two PLL settings we use [rockbox-derived]

| Set | Rates | R36 PLLN | R37 | R38 | R39 | R | f2 | SYSCLK at ÷2 | Error |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 44.1 family | `0x017` (prescale, N = 7) | `0x021` | `0x161` | `0x026` | 7.526400 | 90.3168 MHz | 11.2896 MHz | < 0.01 ppm |
| 1 | 48 family | `0x018` (prescale, N = 8) | `0x00C` | `0x093` | `0x0E9` | 8.192000 | 98.304 MHz | 12.288 MHz | < 0.01 ppm |

Set 1's K is the datasheet's worked example (and the chip's reset value of R37–R39), with the prescaler added because our MCLK is 24 MHz, not 12. [datasheet] The measured frame clock under Rockbox at 44.1 kHz was 88,198 edges/s on the BCM's sampler, that is 44,099 Hz to the counter's resolution. [silicon S32, S33, S34] (`../pp5022-reference/audio.md` §5)

Apple's boot driver uses N = 7 **without** the prescaler and K = `0x86A3AC` (R37 `0x21`, R38 `0x151`, R39 `0x1AC`): the datasheet's Table 49 row for a **12 MHz** MCLK. With a 24 MHz MCLK that would put f2 near 181 MHz, twice the PLL's working point, so Apple's MCLK is most likely 12 MHz. [apple-RE values; MCLK hypothesis] That row targets 11.2889025 MHz, not 11.2896, so Apple's 44.1 kHz runs about **62 ppm slow** (44,097.3 Hz). [datasheet; arithmetic ours] Ours is exact. See `stock-firmware-vs-ours.md` §2.

### 2.4 Every rate, as we program it [rockbox-derived]

`audiohw_set_frequency()` writes R36–R39 (the set), then R6, then R7. BCLKDIV is ÷2 for every rate.

| Rate | Set | MCLKDIV | R6 `CLKCTRL` | R7 SR field | R7 `ADDCTRL` | BCLK |
|---|---|---|---|---|---|---|
| 48 kHz | 1 | ÷2 | `0x145` | 48 kHz | `0x001` | 6.144 MHz |
| 44.1 kHz | 0 | ÷2 | `0x145` | 48 kHz | `0x001` | 5.6448 MHz |
| 32 kHz | 1 | ÷3 | `0x165` | 32 kHz | `0x003` | 4.096 MHz |
| 24 kHz | 1 | ÷4 | `0x185` | 24 kHz | `0x005` | 3.072 MHz |
| 22.05 kHz | 0 | ÷4 | `0x185` | 24 kHz | `0x005` | 2.8224 MHz |
| 16 kHz | 1 | ÷6 | `0x1A5` | 16 kHz | `0x007` | 2.048 MHz |
| 12 kHz | 1 | ÷8 | `0x1C5` | 12 kHz | `0x009` | 1.536 MHz |
| 11.025 kHz | 0 | ÷8 | `0x1C5` | 12 kHz | `0x009` | 1.4112 MHz |
| 8 kHz | 1 | ÷12 | `0x1E5` | 8 kHz | `0x00B` | 1.024 MHz |

- R6 = `CLKSEL` (PLL) | `MCLKDIV` | `BCLKDIV` ÷2 | `MS`. R7 = SR | `SLOWCLKEN` (bit 0, needed for the zero-cross timeout, `playback-path.md` §4); `M128ENB` (bit 8) is added only if `dac_eq_set_m128(true)` was called, which nothing does today (quirk 28).
- **SR selects filter coefficients, not the clock.** It is "approximate": a rate with no SR code uses the nearest one, and the filter corners scale with the real rate. That is why 44.1 kHz uses the 48 kHz code. [datasheet, Table 46]
- **No rate above 48 kHz exists** on this part. [datasheet, features list] Hi-res files must be resampled in software (`../design/resample-plan.md`).
- **BCLK = 128 · fs**, 64 bit clocks per channel slot for 16-bit samples. Apple's boot driver uses ÷8, that is 32 · fs, exactly 16 per slot (`stock-firmware-vs-ours.md` §2). Whether the slower clock saves measurable power is open (`open-questions.md` 7). [rockbox-derived; apple-RE; power hypothesis]
- The driver's comment says the output should be disabled before a rate change; the driver itself does not mute (quirk 15).

## 3. The audio interface (WM-4) [rockbox-derived] [datasheet]

R4 `AINTFCE` = `0x010`: **I2S** format (bits 4:3 = `10`), **16-bit** words (bits 6:5 = `00`), no channel swap, normal BCLK and LRC polarity. The reset value `0x050` would be I2S at 24 bits. [datasheet, Table 42]

- The codec is **master** (R6 `MS` = 1); the PP's I2S block must stay slave (`IISCONFIG` bit 25 = 0), or both chips drive the clocks. [rockbox-derived] (`../pp5022-reference/audio.md` §1)
- **In master mode, disabling the DACs does not stop BCLK and LRC.** Only removing the clock source or switching to slave mode stops them. [datasheet, MASTER AND SLAVE MODE OPERATION] Our standby stops them by turning the PLL off, which is what SYSCLK runs from. [silicon S32, S35: no edges in standby] After playback stops the codec parks both clocks high. [silicon S34]
- The codec side supports 20, 24 and 32-bit words and left-justified, right-justified and DSP formats. A 24-bit path is blocked on the PP side, where the I2S size field (`IISCONFIG[9:8]`) values other than 0 are untested. [datasheet; PP side hypothesis] (`open-questions.md` 16)
- The codec warns against feeding non-zero data while the DAC initialises, or stopping data at a non-zero value without a mute first: both pop. [datasheet, Figure 11 note 2] Our standby enters only after PCM has been stopped for `DAC_IDLE_MS`, so the data is silence. [ours]
- The PP's playback DMA must move whole 32-bit words (#191). That is a PP-side rule; see `../pp5022-reference/quirks.md` 89 and `../pp5022-reference/dma.md` §2.

## 4. Power-up and power-down (WM-10, WM-B)

### 4.1 The datasheet's order

To keep pops out, the datasheet recommends this order. [datasheet, RECOMMENDED POWER UP/DOWN SEQUENCE]

| Step | You do | The codec does |
|---|---|---|
| 1 | Bring the supplies up and let them settle | power-on reset releases; all registers at default |
| 2 | `BIASCUT` = 1 (R61) | low bias while starting |
| 3 | `HP_COM` = 1, `LINE_COM` = 1 (R49) | common-ground feedback on (but see quirk 9) |
| 4 | R52–R57 = `0x140` | outputs muted, PGAs at minimum |
| 5–6 | Enable OUT1 (and OUT2 if used) | output stages on, muted |
| 7 | `POBCTRL` = 1 (R42) | bias from AVDD, not yet from VMID |
| 8 | Enable the DACs and mixers | |
| 9 | `VMIDSEL` = `01` (100 kΩ), `BIASEN` = 1, `BUFIOEN` = 1 (R1) | VMID starts charging |
| 10 | Set up the interface, PLL, DACs | |
| 11 | **Wait 100 ms** | VMID rises |
| 12–13 | Unmute OUT1 (and OUT2), set volume | |
| 14 | `POBCTRL` = 0 | bias from VMID (low noise) |

Power-down: thermal shutdown off; `VMIDTOG` = 1 (fast VMID discharge); `VMIDSEL` = `00` and `BUFIOEN` = 0; wait for VMID to discharge; R1–R3 = 0; remove the supplies. The VMID charge and discharge times depend on the VMID capacitor and the `VMIDSEL` impedance: about 300 ms to mid-rail with a 4.7 µF capacitor, more than 6 s to discharge without `VMIDTOG`. [datasheet, Tables 2–3]

### 4.2 Our power-up [rockbox-derived] (`audiohw_preinit()`, `audiohw_postinit()`)

`audiohw_preinit()` runs from `pcm_init()` through `audiohw_init()`; `audiohw_postinit()` runs later from the audio thread's `pcm_postinit()`. Nothing in the driver waits between writes; the gap between the two halves is however long boot takes (not measured).

| # | Register | Value | Meaning | Datasheet step |
|---|---|---|---|---|
| 1 | R61 `BIASCTRL` | `0x100` | `BIASCUT` | 2 |
| 2 | R49 `OUTCTRL` | `0x187` | `HP_COM`, `LINE_COM`, `TSOPCTRL`, `TSDEN`, `VROI` | 3 (plus thermal shutdown and 30 k tie-off) |
| 3 | R52–R55 | `0x140` each | OUT1/OUT2 muted at −57 dB, update bit set | 4 |
| 4 | R56, R57 | `0x040` each | OUT3/OUT4 muted, no mixer inputs | 4 |
| 5 | R2 `PWRMGMT2` | `0x180` | `LOUT1EN`, `ROUT1EN` | 5 |
| 6 | R42 `OUT4TOADC` | `0x004` | `POBCTRL` | 7 |
| 7 | R3 `PWRMGMT3` | `0x00F` | both DACs, both mixers | 8 |
| 8 | R1 `PWRMGMT1` | `0x02F` | `PLLEN`, `BIASEN`, `BUFIOEN`, `VMIDSEL` = `11` (**10 kΩ**) | 9, but 10 kΩ and with the PLL on (quirks 4, 5) |
| 9 | R4 `AINTFCE` | `0x010` | I2S, 16-bit | 10 |
| 10 | R6 `CLKCTRL` | `0x001` | master, clocked straight from MCLK for now (quirk 5) | 10 |
| 11 | R36–R39, R6, R7 | 44.1 kHz set (§2.4) | PLL to 90.3168 MHz; R6 `0x145`; R7 `0x001` | 10 |
| 12 | R50, R51 | `0x001` each | DAC to its output mixer | 10 |
| 13 | R42 `OUT4TOADC` | `0x000` | `POBCTRL` off | 14, **but before any 100 ms wait** (quirk 3) |
| *postinit* | R1 | `0x02E` | `VMIDSEL` = `10` (500 kΩ, lower VMID current) | — |
| | R61 | `0x000` | `BIASCUT` off | — |
| | R10 `DACCTRL` | `0x008` | soft mute off, 128× oversampling | — |
| *first volume* | R11, R12, R52, R53 | per volume | DAC and amp gain, zero-cross on, update bits on the right channel | 12 |

Where ours differs from the datasheet's order (each is in `quirks.md`):
- `VMIDSEL` 10 kΩ (fast charge) instead of 100 kΩ, then 500 kΩ in use (quirk 4).
- `PLLEN` set before the PLL is programmed, and R6 = `0x001` briefly runs SYSCLK and BCLK straight from the 24 MHz MCLK (quirk 5).
- `POBCTRL` cleared at the end of preinit, a few I2C writes after VMID starts charging, not after the 100 ms wait (quirk 3).
- No wait in the driver before unmuting (quirk 6).
- `LINE_COM` set although OUT2 is never enabled on our unit (harmless). [hypothesis]

Whether any of these causes an audible pop at boot is not recorded (`open-questions.md` 9).

### 4.3 Our power-down [rockbox-derived] (`audiohw_close()`)

| # | Register | Value | Meaning | Datasheet step |
|---|---|---|---|---|
| 1 | R10 | `0x040` | soft mute | (before 1) |
| 2 | R49 | `0x101` | `HP_COM`, `VROI`; thermal shutdown and `LINE_COM` off | 1 |
| 3 | R42 | `0x010` | `VMIDTOG` | 2 |
| 4 | R1 | `0x028` | `PLLEN`, `BIASEN`; VMID off, `BUFIOEN` off | 3 |
| 5 | — | sleep 300 ms | VMID discharges | 4 |
| 6 | R2, R3, R1 | `0x000` each | everything off | 5 |

From DAC standby the same calls run, but `wm_write()`'s standby mask keeps the PLL, DACs, mixers and outputs off, so the PLL does not come back for the discharge. [ours] `VMIDTOG` is left set afterwards; the datasheet requires it to be 0 for normal operation, and only the next `audiohw_preinit()` clears it (R42 = `0x004`) (quirk 19).

## 5. Our DAC standby [ours] (`HAVE_DAC_STANDBY`, Phase 15)

### 5.1 Policy

`dac_idle_poll()` (audio thread and UI) puts the codec in standby when PCM has been stopped for `DAC_IDLE_MS` = 5 s, counted after the mixer's own 3 s silence tail, so about 8 s after a pause. `dac_wake()` brings it back before every PCM start. `dac_busy_now()` from the PCM stop path keeps an underrun's stop from finding a stale busy time. The developer switch "DAC standby" (`dac_standby_allow()`) wakes the codec and holds it awake until the next boot. Anything that needs the I2S clocks (the BCM's audio block, a clock probe) must hold the codec awake first (`../data-paths.md` R9 item 6).

Apple never does this: its codec stays fully powered through every pause and goes fully off only in sleep. [apple-RE] (`stock-firmware-vs-ours.md` §3)

### 5.2 Entering standby (`dac_standby_enter()`, under `dac_mtx`)

| # | Write | Why |
|---|---|---|
| 1 | R10 = shadow \| `SOFTMUTE` | mute; the data is already silence, so no ramp is waited for |
| 2 | R2 = shadow without `LOUT1EN`/`ROUT1EN` | headphone amps off first |
| 2b | R3 without `LOUT2EN`/`ROUT2EN` (only if they were on) | the line out |
| 3 | R3 without the mixers | |
| 4 | R3 without the DACs | |
| 5 | R1 without `PLLEN` | PLL off: BCLK and LRCK stop. Only safe with PCM stopped: the PP's stop path waits for its FIFO to drain, which needs the clocks |
| 6 | (`HAVE_PWR_I2SGATE`) `DEV_EN` &= ~(`DEV_EXTCLOCKS` \| `DEV_I2S`) | AW-5: gate the MCLK and the I2S block. **Off**: the bench found 2 of 6 resumes late [silicon, sitting 4gh, 2026-11-06] (`../pp5022-reference/clocks-power.md`, `awi2`) |

What stays on: VMID at 500 kΩ, `BIASEN`, `BUFIOEN`, and `VROI`, which ties every disabled output to VMID through about 30 kΩ so the headphone pins stay at mid-rail. [datasheet, Table 41] [rockbox-derived] The EQ registers keep their values (no wake needed to change them). [ours]

The order is ours, modelled on the WM8978 sibling driver's documented sequence because no WM8758 datasheet was at hand when it was written (driver comment, 2026-10-11). The Rev 4.4 datasheet gives no standby sequence of its own; ours follows its general rule (outputs off before the blocks that feed them, enabled last on the way up, VMID untouched). [datasheet; ours]

The datasheet's clock-switch example changes `CLKSEL` to MCLK before turning the PLL off; we turn the PLL off with `CLKSEL` still selecting it (quirk 7). It has worked on every wake logged.

### 5.3 Waking (`dac_standby_wake()`, under `dac_mtx`, busy-waits)

| # | Write | |
|---|---|---|
| 0 | (`HAVE_PWR_I2SGATE`) clocks ungated, I2S registers compared with the saved copy | |
| 1 | R1 = shadow (PLL on), then `udelay(2000)` | **2 ms settle: our guess**; the datasheet gives no lock time |
| 2 | R3 + DACs | |
| 3 | R3 + mixers, then the full shadow (OUT2 if it was on) | |
| 4 | R2 = shadow | headphone amps |
| 5 | R10 = shadow | unmute, unless a caller muted meanwhile |

**Measured:** 6.4 ms median, 9.5 ms 95th percentile, 12.0 ms worst over 657 wakes, the 2 ms settle included. [silicon, device logs 2026-10-12/13] (`../pp5022-reference/clocks-power.md`) The shell logs `dac standby=… sleeps=… wakes=… wake_us=…` lines (`apps/ipodui/ui_main.c`).

## 6. Sleep and full off (not built yet)

The P18 soft sleep plans to take the codec fully off, VMID included, the way Apple does in sleep: mute OUT1/OUT2, R49 = `0x180`, R1–R3 = 0, and at wake Apple's anti-pop order with its 100 ms wait (bench probe T-CODECOFF, `../pp5022-reference/apple-power-catalog.md` §5). [apple-RE; plan]
