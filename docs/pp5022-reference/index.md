# PortalPlayer PP5022 and the iPod Video board: a reference

This manual describes the main platform of the Apple iPod Video (fifth generation, "5G", and its late-2006 revision, "5.5G"): the PortalPlayer PP5022 system-on-chip (PP5021 on some early units) and the board around it — SDRAM, boot flash, the ATA storage, the USB port, the BCM2722 video processor seen from the host, the WM8758 audio codec, the PCF50605-family power-management unit, and the clickwheel.

> **First stop for DMA, memory, cache or cross-chip work: [`../data-paths.md`](../data-paths.md)**, the map of every master and target on the board and what reaches what.

PortalPlayer released no public datasheet or programming manual for the PP502x family. What is here comes from:

- the Rockbox source tree, which has run on this hardware since 2006 and is the best working description of it (its register names largely come from the iPodLinux project);
- our own firmware running on a real iPod Video 5.5G (64 MB, flash-modded) in the device sessions listed in `sources.md`;
- an independent emulator project's notes, which ran Apple's firmware against modelled hardware and recorded what it touches;
- Apple's public firmware images, read only for addresses, constants and short identifiers.

It is written so that someone could write their own kernel for an iPod Video from scratch — boot, clocks, memory, interrupts, timers, storage, USB, display, audio, input, power — and know which statements to trust. It is a specification with its gotchas, not a walkthrough of Rockbox's code; Rockbox file names are given as evidence pointers.

Its sibling, `../bcm2722-reference/`, covers the video processor itself.

## Scope

**Covered:** the memory map and the remap windows; the boot chain from the flash ROM to a loaded image and the firmware partition; the PLL, clock selection and Rockbox's frequency switching; the interrupt controller and timers; both cores, the mailbox and the cache; the general DMA engine; GPIO and I2C; the ATA controller and its DMA; the USB controller's PP-side bring-up; the BCM2722 host port as the PP sees it; I2S and the WM8758; the clickwheel; the PMU, battery measurement and charging.

**Not covered, or only in outline:** the SDRAM controller's registers (Apple's boot code sets it up; nothing we run touches it), FireWire (present in silicon, not used for data on the Video), the UARTs beyond their register map (the dock serial line; our unit has no dock connector), the PP's own LCD bridges (used by other iPods, not the Video), the PP5020's and PP5024's differences beyond a few noted ones, and the internals of the USB controller (it is a standard ARC/ChipIdea core; only its PP integration is described).

## Conventions

- Addresses are **physical** PP5022 addresses unless marked "logical". After Rockbox's remap, logical `0x0000_0000` is SDRAM, physical `0x1000_0000` (`memory-map.md`).
- `0x` hex throughout. Bit ranges are `[hi:lo]`, bit 0 the least significant. Registers are 32-bit and little-endian unless stated.
- Register names are Rockbox's (`firmware/export/pp5020.h`, which serves the PP5020, PP5022 and PP5024) so that the evidence can be looked up; they are labels, not PortalPlayer's names.
- "5G" is the original iPod Video (late 2005), "5.5G" the late-2006 revision. "30 GB" units have 32 MB of SDRAM, "60 GB" and "80 GB" units 64 MB.
- "W1S"/"W1C": write 1 to set / write 1 to clear; zero bits are ignored.
- Device sessions are cited as S*n* (bcm2722 sessions) or by the backup folder date where no session number exists.

## Evidence legend

Every fact carries one tag. A fact with two parts (a value and its meaning) may carry two.

| Tag | Meaning |
|---|---|
| **[silicon]** | Measured on a real PP5022 in an iPod Video 5.5G with our own code. Cited by session (`sources.md`). |
| **[apple]** | Read from Apple's code (the flash boot ROM's bootloader, `osos`), statically. Says what Apple's code *does*; meanings attached to it are separately tagged. |
| **[rockbox]** | From the Rockbox source tree or its tracker. Rockbox works on this hardware, so a value Rockbox writes is known to work; the *meaning* Rockbox gives it is only as good as its comment. |
| **[external]** | Other public sources: the iPodLinux project, the siggifly emulator notes, freemyipod. Never redistributed; cited in `sources.md`. |
| **[hypothesis]** | Our inference, not verified. Never promoted without new evidence. |
| **[ours]** | A convention defined by our own firmware (a memory split, a driver policy). Not a property of the hardware; another design can replace it. |

When silicon contradicts an earlier reading, the silicon result is stated and the withdrawn reading is noted in `quirks.md`.

## Contents

| File | What it covers |
|---|---|
| [`overview.md`](overview.md) | The chip and the board at a glance: cores, clocks, memories, peripherals, the parts around the SoC, 5G vs 5.5G, PP5021 vs PP5022 |
| [`boot.md`](boot.md) | Reset, the flash ROM, the disk's firmware partition and its image directory, loading and starting an image, starting the COP, the Apple fallback and the key combinations |
| [`memory-map.md`](memory-map.md) | Every known address range, SDRAM size detection, IRAM, the MMAP remap windows, uncached aliases |
| [`clocks-power.md`](clocks-power.md) | Clock sources, the PLL, `CLOCK_SOURCE`, frequency switching (24/30/80 MHz), device enable/reset, the PMU, battery voltage, charging, power-off |
| [`interrupts-timers.md`](interrupts-timers.md) | Vectors, the two-bank interrupt controller, IRQ/FIQ routing, source numbers, the timers and the microsecond counter |
| [`cores-and-threads.md`](cores-and-threads.md) | CPU and COP: identity, sleep/wake, the mailbox, the cache and its maintenance, coherency, locks, what can run on the COP |
| [`dma.md`](dma.md) | The 4-channel DMA engine, the I2S playback channel, the BCM port channel and what our sessions measured; the second DMA engine (`0x6000_8000`) and Apple's use of it for the BCM port |
| [`gpio-i2c.md`](gpio-i2c.md) | The 12 GPIO ports, atomic bit access, the iPod Video's pin assignments; the I2C controller and the devices on the bus |
| [`storage.md`](storage.md) | The IDE controller, PIO and DMA timing, the 5.5G's 2048-byte sectors, the flash mod |
| [`usb.md`](usb.md) | The ARC/ChipIdea controller's PP integration, PHY bring-up, cable detection, charging current |
| [`display.md`](display.md) | The BCM2722 host port from the PP side: addresses, handshake, commands, frame push, power |
| [`audio.md`](audio.md) | The I2S block, its FIFO formats, clocking, the WM8758 over I2C, the I2S bus shared with the BCM2722 |
| [`input.md`](input.md) | The clickwheel controller's serial interface and packet, buttons, the hold switch, the click |
| [`mac-and-dvs.md`](mac-and-dvs.md) | The brief's "integrated MAC" (the cores' multiplier), the PMU's regulator layout and formula candidates, Apple's PP5022 clock driver (125 MHz ceiling, speed grade, the 90 ns `DEV_TIMING1` rule), and the voltage and 100 MHz probes |
| [`quirks.md`](quirks.md) | Every gotcha in one list |
| [`open-questions.md`](open-questions.md) | What is not known, and how each could be settled |
| [`sources.md`](sources.md) | Bibliography and device sessions |
| [`README-render.md`](README-render.md) | Note on rendering this manual to HTML |

## How to use this manual

To bring up your own kernel, read in this order: `overview.md`, `boot.md`, `memory-map.md`, `cores-and-threads.md` (§1–§3), `interrupts-timers.md`, `clocks-power.md`. Keep `quirks.md` open. Then add drivers: `gpio-i2c.md` → `input.md` (buttons) → `display.md` (needs the BCM2722 reference) → `storage.md` → `audio.md` and `dma.md` → `usb.md`.

## Publishing rules for this manual

- No Apple code or binary excerpts beyond addresses, constants and short identifiers. Apple's firmware is never redistributed.
- External notes are cited, not copied; their facts are restated in our words with an [external] tag.
- No project-internal chatter; the hidden firmware folder on the disk is called "the system folder".
