# The WM8758: developer reference

*Part IV (`WM`) of the ProkBox developer manuals, in its source-tree form. It documents the Wolfson WM8758 audio codec (the DAC) as wired in the iPod Video 5G/5.5G, well enough to drive it from scratch. Grown on 2026-11-07 from the one-file seed of 2026-10-24. Own words; short quotes from the datasheet only. Record every device finding here, with its tag, in the same change.*

## 1. Overview (WM-1)

The WM8758 turns the PP5022's PCM into the analogue signal at the headphone socket. On this board it does three jobs:

1. **It is the clock master of the audio bus.** Its own PLL turns the 24 MHz master clock it gets from the PP into the I2S bit clock (BCLK) and frame clock (LRCK), which it drives to the PP's I2S block and to three BCM2722 GPIOs. With the PLL off there are no audio clocks for anyone. [silicon S32–S34] [rockbox-derived] (`../data-paths.md` M13, R9)
2. **It converts and shapes the sound:** DAC digital volume, the 5-band EQ, two oversampling rates, soft mute, an unused limiter and 3D stage. [datasheet]
3. **It drives the headphones** through its OUT1 amplifier, whose analogue gain carries most of the volume control. [datasheet] [rockbox-derived]

The host controls it over I2C at 7-bit address `0x1A`. The bus is **write-only**: nothing can be read back, so the driver keeps a shadow copy of every register it needs. [datasheet] [rockbox-derived]

```
                 24 MHz ext. device clock (MCLK, pin 11)
   PP5022  ─────────────────────────────────────────────►  WM8758
   I2C 0x7000_C000  ── SCLK/SDIN (2-wire, addr 0x1A) ───►  control (write-only)
   I2S 0x7000_2800  ◄── BCLK, LRCK (codec is master) ────  PLL → SYSCLK → BCLKDIV
                    ── DACDAT (16-bit I2S) ─────────────►  DAC → mixers → OUT1 ──► headphone socket
   BCM2722 GPIO 2/3 ◄── BCLK, LRCK (same nets) ─────────   (OUT2 fed the dock line-out; gone on our unit)
   PMU D1REGC1 (2.5 V) ─────────────────────────────────►  supply (which pins: open question 2)
```

What is wired and what is not is the weakest part of this manual: the datasheet describes the chip, not the board. Every wiring statement is tagged [hypothesis] until a board trace or a measurement settles it (`open-questions.md`).

## 2. Files and chapter map

The volume's style guide (`../design/devguide-style.md` §2.3) fixes the Part's chapters. The source files map onto them like this:

| File | Chapters | What it holds |
|---|---|---|
| `README.md` (this file) | WM-1, WM-E | Overview, sources, tags, conventions |
| [`bring-up.md`](bring-up.md) | WM-2, WM-3, WM-4, WM-10, WM-B | The I2C control interface and the write log; clocking and the PLL per sample rate with the values we write; the audio interface; power-up, power-down and our DAC standby, step by step |
| [`registers.md`](registers.md) | WM-A | Every register: fields, reset value, our value, why; the coverage table |
| [`playback-path.md`](playback-path.md) | WM-5, WM-6, WM-7, WM-8, WM-9 | DAC and filters, soft mute, volume split and Sound Check, EQ and 3D, limiter, mixers, outputs, the unwired ADC side |
| [`power.md`](power.md) | WM-C (part) | Datasheet power figures, per-block bits, supply rails, the lowest-power configuration |
| [`features.md`](features.md) | — | Block-by-block status table (used, known-unused, unwired) with probes |
| [`stock-firmware-vs-ours.md`](stock-firmware-vs-ours.md) | — | Apple's codec set-up, register by register, against ours |
| [`quirks.md`](quirks.md) | — | Numbered list of everything that bites |
| [`open-questions.md`](open-questions.md) | WM-D | What is unknown, why it matters, and the probe that answers it |

WM-11 (recipes) has no file yet; the recipes it will hold are spread through `bring-up.md` and `playback-path.md`.

**Reading routes.** Writing a driver from scratch: §3 here, then `bring-up.md` top to bottom, then `registers.md` as the reference. Changing sound or volume: `playback-path.md`. Saving power: `power.md`, then `stock-firmware-vs-ours.md` §4. Before any bench step: `quirks.md` and `open-questions.md`.

## 3. Conventions

- **Registers** are named `Rn` with the decimal number the datasheet uses, plus the hex address and Rockbox's macro name where useful: R6 `0x06` `CLKCTRL`. Values are the 9-bit register contents in hex (`0x145`).
- **A write** is 16 bits on the wire: the 7-bit register number, then the 9 data bits (`bring-up.md` §1).
- **"Ours"** means the ProkBox build with the shell (`HAVE_IPOD_UI`). Where stock Rockbox differs, the text says so.
- **Rates** use Rockbox's names: the 44.1 kHz family (44.1, 22.05, 11.025 kHz) and the 48 kHz family (48, 32, 24, 16, 12, 8 kHz).

## 4. Evidence tags

| Tag | Meaning here |
|---|---|
| [datasheet] | Stated by the WM8758B datasheet, Rev 4.4 (§5). It describes the part, not our board |
| [rockbox-derived] | From the Rockbox/ipodlinux driver or its comments: a value it writes is known to work; the meaning a comment gives is only as good as the comment |
| [apple-RE] | Read from Apple's firmware, statically or under our emulator; the address is given. Says what Apple's code does, not why |
| [silicon] | Measured on our unit with our code; the sitting or log is named |
| [hypothesis] | Our inference, unproven; `open-questions.md` names the probe |

Markers that are not evidence: [ours] (a policy of our firmware), [sim] (seen only in a model), [estimate] (computed, not measured). Never upgrade a tag without a cited measurement. Where sources disagree, both are kept and the disagreement goes into `quirks.md` or `open-questions.md`.

## 5. Sources (WM-E)

- **Datasheet:** Wolfson Microelectronics (now Cirrus Logic), *WM8758B, Stereo CODEC with Headphone Driver and Line Out*, Production Data, **Rev 4.4, January 2012**, 91 pages. Section and table numbers in this manual are to this document. The 5.5G's part is marked WM8758; the "B" is the production revision of the same register map, and the driver's register layout matches the map bit for bit for every register we use (checked field by field, `registers.md`).
  - Where to get it: `https://pdf.aiema.com/wm8758cbgeflv-Wy7m1veu-roNav7PYq.pdf` (a third-party mirror; Cirrus Logic's own host refused the download), found through alldatasheet.com's WM8758B listing (474018). Check the page footers read "PD, Rev 4.4, January 2012". The repo keeps no copy. It was re-fetched for this edition on 2026-11-07 and read with `pdftotext -layout`.
- **Our driver:** `firmware/drivers/audio/wm8758.c`, `firmware/export/wm8758.h` (its header says it was written "based on datasheet for WM8983", a sibling part), `firmware/export/dac_ext.h`, `firmware/export/dac_gain.h`. The PP side: `firmware/target/arm/pp/wmcodec-pp.c`, `i2c-pp.c`, `pcm-pp.c`.
- **The PP5022 manual:** `../pp5022-reference/audio.md` (I2S block, board bring-up, the shared clocks), `../pp5022-reference/gpio-i2c.md` §6 (the I2C controller), `../pp5022-reference/clocks-power.md` (the wake measurement), `../pp5022-reference/apple-power.md` §5.3 and `../pp5022-reference/apple-power-catalog.md` §2.7 (Apple's codec power modes).
- **Cross-chip:** `../data-paths.md` (M13, "The WM8758", R9), `../bcm2722-reference/audio.md` (the BCM on the codec's clocks).
- **Catalog and history:** `../hardware-features.md` §3 (rows W-1 to W-16), `../phase15-brief.md` (DAC standby, hardware EQ, Sound Check), `../changes-from-rockbox.md` (every edit to the driver).
- **Apple's boot driver,** decoded under our emulator on 2026-11-07 (`stock-firmware-vs-ours.md` §2).

## 6. Revision history

| Date | Change |
|---|---|
| 2026-10-24 | Seed: features table, register coverage, power, corrections, open questions, from the Rev 4.4 datasheet |
| 2026-11-07 | Grown into a manual: control interface, clocking per rate, power sequences against the datasheet, every register, the playback path, Apple's set-up against ours, quirks and open questions with probes. Corrections: the power-up order is not the datasheet's in four places (`quirks.md` 3–6); the datasheet has a master-mode power row after all (`power.md` §1); `HP_COM`'s meaning depends on a pin we have not traced (quirk 9) |
