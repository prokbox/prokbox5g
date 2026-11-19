# Quirks and gotchas

*One list of everything that bites, numbered for citation (the numbers are not in order: items are grouped by topic). Each item names the file with the detail. Numbers are fixed once published: add new items at the end.*

## Control interface

**1.** **Nothing can be read back.** The 2-wire interface is write-only; a read is not even acknowledged. Every value the driver needs later must be shadowed, and every claim about the chip's state rests on the write path and on listening. [datasheet] [rockbox-derived] (`bring-up.md` §1)

**2.** **The write log has had no reader on the device since 2026-10-26.** `wm_write()` still fills the 32-entry `dac_reglog` ring, but the developer "EQ test" that copied it to `ipodui.log` was removed in commit `372d7e6185`; only the simulator's stub serves `dac_reglog_read()`. Bench specs that ask for "the codec register log" (T-I2SGATE, T-CODECOFF in `../pp5022-reference/apple-power-catalog.md`) need a reader first. [ours] (`bring-up.md` §1.3)

## Power-up and power-down (against the datasheet's order)

**3.** **`POBCTRL` is cleared before VMID has risen.** The datasheet clears it at step 14, after the 100 ms VMID wait; our `audiohw_preinit()` clears it (R42 = 0) a few I2C writes after VMID starts charging. The bias then switches to a VMID that is still rising. [datasheet] [rockbox-derived] Audible effect unknown. [hypothesis] (`bring-up.md` §4.2, `open-questions.md` 9)

**4.** **VMID charges through 10 kΩ, not the datasheet's 100 kΩ**, then runs at 500 kΩ. Apple uses 100 kΩ and stays there. [datasheet] [rockbox-derived] [apple-RE]

**5.** **The PLL is enabled before it is programmed, and the interface briefly runs straight from MCLK.** R1 = `0x02F` sets `PLLEN` while R36–R39 still hold their reset values (N = 8, K = `0x3126E9`, no prescaler: with our 24 MHz MCLK that is about 197 MHz, twice the PLL's working point). Then R6 = `0x001` makes the codec master with SYSCLK = MCLK = 24 MHz and BCLK ÷1, so BCLK runs at 24 MHz, above the 12.288 MHz SYSCLK limit, until the rate is set a few writes later. Harmless as far as we know; no data flows then. [datasheet] [rockbox-derived] [hypothesis]

**6.** **No 100 ms wait inside the driver.** The datasheet waits 100 ms for VMID before unmuting; ours unmutes when the audio thread's `pcm_postinit()` and the first volume call run, whenever that is in the boot. Not measured. [rockbox-derived]

## Clocking

**7.** **Our standby turns the PLL off while `CLKSEL` still selects it.** The datasheet's example switches `CLKSEL` to MCLK first, then disables the PLL. Ours has worked on every logged wake (657 over 2026-10-12/13). [datasheet] [silicon] (`bring-up.md` §5.2)

**8.** **As master, the codec keeps BCLK and LRC running with the DACs off.** Only stopping the clock source (our PLL-off), removing MCLK, or slave mode stops them. Anything that needs the clocks must keep the PLL on; anything that wants them stopped must turn it off. [datasheet] [silicon S32, S35] (`bring-up.md` §3)

**16.** **The datasheet disagrees with itself on the 12.288 MHz example's K.** The worked example computes `0x3126E9`; Table 49 prints `0x3126E8`. `int(2^24 × 0.192)` is `0x3126E9`, which is also the register reset value and what we write. [datasheet]

**17.** **The datasheet's 44.1 kHz example is 62 ppm slow.** Table 49's 12 MHz row targets 11.2889025 MHz, not 11.2896 MHz. Apple's boot driver uses exactly that row (44,097.3 Hz); ours is exact. [datasheet] [apple-RE] (`stock-firmware-vs-ours.md` §2)

**18.** **SYSCLK must not exceed 12.288 MHz, and MCLK into the PLL needs a 20 ns minimum cycle.** Our two PLL sets give 11.2896 and 12.288 MHz. [datasheet]

**26.** **The PP's I2C data registers: byte-wide (Rockbox) or one packed word (Apple)?** Rockbox writes the two codec bytes to `0x7000_C00C` and `0x7000_C010`; Apple's boot driver writes one word at `0x7000_C00C`. Both work on silicon. Bench row `i2creg` decides the controller's layout; the decoded Apple values hitting a datasheet PLL row exactly argues for the packed reading. [rockbox-derived] [apple-RE] [hypothesis] (`bring-up.md` §1.2, `../pp5022-reference/gpio-i2c.md` §6)

**31.** **Our BCLK is 128 fs; 16-bit stereo needs 32.** BCLKDIV ÷2 gives 64 bit clocks per channel slot; Apple's boot driver uses ÷8 (32 fs). Both play. Anyone sampling BCLK (the BCM's GPIO 2) must expect 128 fs from us, and a clock-slave receiver configured for Apple's 32 fs would mis-frame our slots: one candidate cause of the full-scale screech the BCM's audio block produced on the codec's clocks (S35). [rockbox-derived] [apple-RE] [hypothesis] (`open-questions.md` 7; `../data-paths.md` cell 27)

**33.** **The PLL lock time is not published.** Our 2 ms settle before the DACs come back is a guess inside a measured 6.4 ms median wake. GPIO1's "PLL lock" function could measure it if pin 15 is reachable. [datasheet] [silicon] (`open-questions.md` 6)

## Playback

**9.** **`HP_COM` and `LINE_COM` invert when CSB/GPIO1 is high in 2-wire mode** (unless GPIO1 is configured as a GPIO). We write both as 1, which means "on" only if pin 15 is low. Its level on this board is not known, so neither is whether the headphone common-ground feedback is on. [datasheet] [hypothesis] (`playback-path.md` §7.1, `open-questions.md` 3)

**10.** **`SOFTMUTE`'s sense: the datasheet contradicts itself.** Table 21 prints "0 = Enabled, 1 = Disabled"; the text says soft mute is on by default and is turned off "by setting the SOFTMUTE bit to zero"; the register map gives R10's reset value as `0x000`. No reading makes all three agree. On our unit **bit 6 = 0 plays** (every session plays with R10 = `0x008`), and Rockbox has muted with bit 6 = 1 for years. [datasheet] [silicon] [rockbox-derived] The seed's "Table 21 typo, trust the text" was only half right: the text's default claim does not fit the map either.

**11.** **The EQ peaks work in DAC low-power mode on our unit, contrary to the datasheet.** The datasheet allows only EQ1, EQ5 and 3D with `M128ENB` = 0; the owner heard EQ2 and EQ3 at +12 dB with `M128ENB` = 0 at 44.1 and 48 kHz [silicon, spike S1, 2026-10-11]. EQ4 does **not** work there: alone at +12 dB it was not louder with `M128ENB` = 0 [silicon, bench b11 row m128, 2026-11-10], and with #265's fix (below, 28) the blind b12 `eq4m` A/B was 2/2 correct [silicon, b12, owner]. Whether the chip silently leaves low-power mode when a peak is set (which would cost the power the mode saves) is not known. [datasheet] [hypothesis] (`playback-path.md` §4.1, `open-questions.md` 13)

**12.** **`EQ3DMODE` can only change with both ADCs and both DACs off;** the control interface ignores the write otherwise. We never change it (always 1, the DAC path). [datasheet]

**13.** **Write left, then right with the update bit.** `DACVU` and `OUT1VU` latch both channels; a left write alone does nothing audible until the right write with the bit set. [datasheet] [rockbox-derived]

**14.** **The zero-cross timeout needs `SLOWCLKEN`, and its printed length is wrong.** Without R7 bit 0 a gain change on a silent channel can wait forever. The datasheet gives the timeout as "2 × SYSCLK period" (under 200 ns), which cannot be a timeout; the real value is unpublished. [datasheet]

**15.** **A sample-rate change rewrites the PLL with the output live.** The driver's comment says to disable the output first, but `audiohw_set_frequency()` does not mute and its caller (`sink_set_freq()` from `pcm_apply_settings()`) only guarantees PCM is stopped. A click at a 44.1 ↔ 48 kHz track boundary is possible. [rockbox-derived] [hypothesis] (`open-questions.md` 10)

**22.** **The datasheet's power table labels the PLL registers "R24–R27"** in the master-mode row: the hex addresses of R36–R39 written as if decimal. R24–R27 decimal are the limiter and notch registers. [datasheet] (`power.md` §1)

**23.** **`DACOSR128`: "64× = lowest power & best performance" (DAC table) against "64× ... slight decrease in noise performance" (ADC text).** We run 128×; the trade has not been measured. [datasheet] (`open-questions.md` 5)

**24.** **What this part does not have:** no de-emphasis, no DAC filter choice, no beep tone generator (R43 only routes bypass paths), and the ALC and noise gate act on the ADC only, never on playback. [datasheet]

**25.** **Nothing above 48 kHz.** Hi-res files are resampled in software, whatever the PP's I2S could carry. [datasheet]

**34.** **`VROI` keeps disabled outputs at mid-rail through about 30 kΩ.** Our standby relies on it for a click-free wake; Apple's final R49 has it off (1 kΩ tie). [datasheet] [rockbox-derived] [apple-RE]

## Power-up and reset state

**20.** **Some bits Apple uses are not described in Rev 4.4:** `DELEN`, `OUT1DEL`, `OUT2DEL` (R42), `DELEN2` (R43), `OUT3ENDEL`/`OUT4ENDEL` (R49), `BUFBIAS`, `ADCBIAS`, `HALFI_IPGA` (R61). Apple's power-up sets `DELEN`, `DELEN2` and `OUT3ENDEL` [apple-RE]; we never set them. Treat them as reserved until a sibling datasheet or a test explains them. [datasheet] [hypothesis] (`open-questions.md` 18)

**21.** **We never soft-reset the codec.** Only the power-on reset puts the registers we do not write (R5, R8, R9, R13–R16, R24–R35, R41, R43) at their defaults. If the codec's supply stays up across a warm restart from another firmware (Apple's, or our own reload), those registers keep whatever it left; Apple's power-up, for example, writes R43. Whether `D1REGC1` drops across a restart is not known. [datasheet] [rockbox-derived] [hypothesis] (`open-questions.md` 21)

## Power-down

**19.** **`VMIDTOG` stays set after `audiohw_close()`.** The datasheet requires it to be 0 for normal operation; only the next `audiohw_preinit()` clears it (R42 = `0x004`). Harmless while close is followed by power-off; a future sleep path that powers the codec back up without preinit must clear it first, or VMID cannot charge. [datasheet] [rockbox-derived] [hypothesis]

**29.** **Gating the codec's MCLK and the I2S block in standby made resumes late.** With AW-5 variant b, 2 of 6 resumes found PCM not running and `DEV_EN` still gated; the I2S registers came back intact. The row stays off. [silicon, sitting 4gh, 2026-11-06] (`../pp5022-reference/clocks-power.md`, `awi2`)

**30.** **After playback stops, the codec parks BCLK and LRCK high** (before any playback they were seen low). [silicon S34, S32] (`../pp5022-reference/audio.md` §6)

## Two cores

**27.** **Both cores call the driver, so its shadows must be uncached.** A mutex orders the cores but not their caches: a stale cached shadow once replayed a full standby wake over music. All shadows and standby words are `XCORE_BSS`/`XCORE_DATA`. [silicon, bench sitting 3, 2026-10-26] [ours] (`../pp5022-reference/quirks.md` 54)

**28.** **`M128ENB` is the OR of three requests** (#265, since 0.8.3; the third since p090-osr128): `dac_eq_set()` asks for it while EQ band 4 has a gain (set before EQ4's gain, cleared after it goes flat), the 3D stage asks for it while it has a depth (`dac_set_width3d()`), and the high rates ask for it at 88.2/96 kHz at 128× while the developer switch `dac_set_m128_hirate()` is on (`dac_hr_sync()`; `tests/dacosr/test_hr.c`). The bit is written only when the OR changes and survives rate changes through the shadow. Host test `tests/dacosr/test_m128.c`. (Before #265 it could not be set at all: `dac_eq_set_m128()` had lost its only caller.) [ours]

**35.** **At 88.2/96 kHz the chip runs at twice its rated clock, and no register setting avoids it.** The datasheet caps SYSCLK at 12.288 MHz (256 × 48 kHz); 96 kHz needs 24.576 MHz in master and slave mode alike, since every audio clock is SYSCLK / 256. The PLL, MCLK and BCLK stay inside their own limits. Only the share of the chip at the doubled clock can be chosen: 64× keeps the modulator at 6.144 MHz (its 48 kHz × 128× rate), `M128ENB` = 0 keeps the DSP core at 12.288 MHz, the EQ guard keeps the EQ at rest. Numbers: `playback-path.md` §1.1. [datasheet] [estimate] (#439)

## Cross-references

**32.** **The PP's playback DMA moves whole 32-bit words** (#191): a PP-side rule that affects what reaches the codec. See `../pp5022-reference/quirks.md` 89 and `../pp5022-reference/dma.md` §2.
