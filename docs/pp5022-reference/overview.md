# Overview

## 1. The SoC

| Item | PP5022 (iPod Video) | Evidence |
|---|---|---|
| Cores | 2 × ARM7TDMI (ARMv4T: ARM and Thumb, no MMU, no MPU, no FPU, no hardware divide), "CPU" and "COP" | [rockbox] `config.h` (`CPU_ARM7TDMI`, `NUM_CORES 2`) |
| Maximum clock | 80 MHz in Rockbox. Apple's PP5022 driver accepts up to 125 MHz, capped by a speed grade read from `STRAP_OPT_A[6:5]` (`mac-and-dvs.md` §3); what Apple actually requests is not established | [rockbox]; [apple] |
| Clock reference | 24 MHz crystal; PLL; 32.768 kHz | [rockbox] |
| On-chip RAM | 128 KB IRAM at `0x4000_0000`, single-cycle, shared, uncached | [rockbox] `boot-pp.lds` |
| Cache | 8 KB unified, 16-byte lines, write-back, controller-managed; no coherency between cores | [rockbox] |
| Boot | from a 1 MB external NOR flash (Apple's boot ROM) | [rockbox]; [external] |
| External memory | 32 or 64 MB SDRAM at `0x1000_0000` (16-bit [hypothesis]) | [rockbox] |
| Interrupts | two banks of 32 sources, per-core masks, IRQ or FIQ per source | [rockbox] |
| Timers | 2 programmable µs timers, a free-running µs counter | [rockbox] |
| DMA | 4-channel general engine (`0x6000_B000`), a second 2-channel engine (`0x6000_9000`), IDE and USB bus masters | [rockbox]; [external] |
| Peripherals | IDE/ATA (UDMA), USB 2.0 HS OTG (ARC), I2S (in/out), I2C master, 2 UARTs, PWM, a wheel-controller serial port, 12 × 8 GPIO + 32 GPO, two LCD bridges (unused on the Video), FireWire link (unused) | [rockbox] `pp5020.h` |
| Identity | `PP_VER2`/`PP_VER1` (`0x7000_0004`/`0x7000_0000`) read as ASCII; a retail 5.5G reads "PP5022C" | [external] siggi `16` (read on a real 5.5G under Rockbox); **[silicon]** the owner's 5.5G: `PP_VER2` = `0x5050_3530`, `PP_VER1` = `0x3232_4320`, "PP5022C " (sitting 4gh, 2026-11-06) |

### PP5021 vs PP5022

Teardowns and the community have long labelled the iPod Video's chip "PP5021C", and one 5G's own system information names "PP5021C-2" [external] siggi `05`, `16`. A System debug read on a retail 5.5G returns PP5022C [external]. Apple's boot ROM knows only PP5020, PP5022 and PP5026 by name and tests the chip-ID byte for '6' or '2' [external] siggi `16`. Rockbox builds one binary (`CONFIG_CPU PP5022`) for every iPod Video and no difference in behaviour between units has been reported [rockbox]. The owner's 5.5G reads **"PP5022C "** [silicon] (sitting 4gh, 2026-11-06), so on this unit the die says PP5022 whatever the package or the Wiki says. "PP5021" as a marking variant of the same die stays [hypothesis] until a 5G (Initial or Rev A) is read; nothing in this firmware depends on it (the ID is only logged).

The Apple Wiki's fifth-generation model page (copied by the owner, 2026-11-05) lists **"PortalPlayer PP5021C, 90 MHz" for all three revisions**, including the late-2006 5.5G [external]. That conflicts with the "PP5022C" chip-ID read above. Both may be true if the marking and the ID string differ, so the chip-ID read decides (open question 22). For the 5.5G it has: "PP5022C " [silicon] (sitting 4gh, 2026-11-06); the Wiki's "PP5021C" for the late-2006 model is a marking or an error, not the die's ID. Its **90 MHz** rating sits between our normal 80 MHz boost and the 100 MHz probe (`mac-and-dvs.md` §5) [external; unverified against Apple's clock tables].

### Models and identifiers (Apple Wiki, fifth generation) [external]

| | Initial (12 Oct 2005) | Rev A (6 Jun 2006) | Rev B, "Late 2006" / 5.5G (12 Sep 2006) |
|---|---|---|---|
| RAM | 32 MiB (30 GB) / 64 MiB (60 GB) | 32 MiB (30 GB) / 64 MiB (60 GB) | 32 MiB (30 GB) / 64 MiB (80 GB) |
| Drive | Toshiba 1.8″, 4,200 rpm, ATA-66; single platter (30 GB), dual (60/80 GB) | same | same |
| Battery | 650 mAh (30 GB) / 850 mAh (60 GB) | 650 mAh (30 GB) | not given |
| Rated audio / video time | 14 h / 2 h (30 GB); 20 h / 3 h (60 GB) | 14 h / 2 h | 14 h / 3.5 h (30 GB); 20 h / 6.5 h (80 GB) |
| Update family ID | 13 | 20 | 25 |
| Initial firmware | 1.0 (6.0) | 1.1.2 (6.1.2) | 1.2.1 (6.2.1) |

Common to all: codename **M25**; family ID **6**; gestalt IDs `0x000B0005` / `0x000B0010`; USB product ID **`0x1209`** (matches both Apple fw 1.3 and ProkBox, read on the owner's 5.5G 2026-11-05 [silicon]); last firmware 1.3 (6.3); WM8758 codec; 320×240 2.5″ panel; piezo clicker; composite video on the headphone jack.

Notes:
- The 5.5G's rated video time roughly doubled on similar cells (3 h → 6.5 h for the large model), which fits firmware 1.2's BCM2722 power management (`../bcm2722-reference/brief-map-power.md`) [hypothesis].
- The family, update-family and gestalt IDs are what a populated `iPod_Control/Device/SysInfo` would carry. The owner's unit has an empty one, which may be why Finder's sync page never finishes loading (#137) [hypothesis].

Differences from the PP5020 (iPod 4G/Color/nano-era parts) that matter [rockbox]: 128 KB vs 96 KB IRAM; a PLL with a post-divider that must run ≥ 96 MHz (the PP5020 has an unlock write instead); `PROC_CNT_SEC` exists only from the PP5022; a PHY sequence after the USB controller reset is needed only on the PP5022.

## 2. The board (iPod Video 5G / 5.5G)

| Part | Function | Connection to the PP | Evidence |
|---|---|---|---|
| SDRAM | 32 MB (30 GB units) or 64 MB (60/80 GB); Samsung K4M51163PC seen on 64 MB boards | SDRAM controller | [rockbox]; [external] siggi `05` |
| NOR flash, 1 MB | Apple's boot ROM, system configuration, `vmcs`/`diag`/`disk`/`logo` images | external bus, `0x0`/`0x2000_0000` | [rockbox]; [external] |
| 1.8" ATA drive | storage (flash adapters in modded units) | IDE controller `0xC300_0000`; power GPO32 bit 30 | [rockbox] |
| Broadcom BCM2722 | video processor; owns the 320×240 LCD and TV out | 16-bit host port `0x3000_0000`; power GPO32 bit 14; interrupt to GPIOC bit 6 (unused); also on the I2S clock nets | [rockbox]; [silicon] S32–S34; `../bcm2722-reference/` |
| Wolfson WM8758 | audio codec, headphone amp, line out; I2S clock master | I2C `0x1A`; I2S `0x7000_2800`; MCLK = 24 MHz device clock | [rockbox] |
| PMU, PCF50605 family (Rockbox's driver; iPodLinux and a published BOM say PCF50607, believed register-compatible) | regulators, RTC, 10-bit ADC (battery), power-on/standby | I2C `0x08` | [rockbox]; [external] siggi `05` |
| Linear LTC4066 | USB power manager / Li-ion charger | GPIOA2, GPIOL2 (control); GPIOB0, GPIOL3/4 (status); PMU ADC ch. 7 | [rockbox] `power-ipod.c` |
| Wheel controller | capacitive wheel and five buttons | serial port `0x7000_C100`; raises the I2C interrupt | [rockbox] |
| Backlight driver | LED backlight, 32 levels | GPIOB3, GPIOD7 (pulse), GPIOL7 | [rockbox] |
| Piezo (haptic on our unit) | click | PWM `0x7000_A000` | [rockbox]; [ours] |
| Battery | Li-ion, 400 mAh (30 GB), 600 mAh (60/80 GB); ours 3000 mAh | PMU ADC ch. 2 | [rockbox]; [ours] |
| 30-pin dock | USB, FireWire power (charging only), line out, serial (UART0) | – | [rockbox]; removed on our unit [ours] |

Boards: 820-1763-A (5G), 820-1975-A (5.5G); both are model A1136 [external] siggi `05`.

### 5G vs 5.5G

| Item | 5G | 5.5G | Evidence |
|---|---|---|---|
| Disk block layout | 512-byte partition map | **2048-byte** partition map and file systems (drive still 512-byte) | [rockbox] ipodpatcher, `ipodvideo.h`; [external] |
| Hardware version (flash `0x405C`) | `0x000B_0005` | `0x000B_0011` | [external] siggi `17` |
| Flash bootloader | identical apart from the configuration page | | [external] |
| Apple firmware | same OS image (1.3), different updater family (20 vs 25) and image headers | | [external] |
| Software-visible chip differences | none known | | [rockbox] |

## 3. Clock and power domains (summary)

The core clock (both cores and, it seems, the bus) comes from `CLOCK_SOURCE`: 24 MHz crystal, PLL (Rockbox: 30 or 80 MHz), or 32 kHz. Timers count µs independently of it. The codec's MCLK is the 24 MHz device clock. The BCM2722 has its own 27 MHz crystal and PLL. Core voltage 1.2 V (PMU DCDC1); I/O 3.0–3.3 V. Details in `clocks-power.md`.

## 4. Where Rockbox stops

Rockbox has never brought this platform up from a cold reset: it always starts after Apple's ROM has configured SDRAM, clocks, the BCM and the wheel block. Its drivers are therefore a **lower bound** on what a kernel must do once the ROM has run, and say nothing about a ROM replacement. [external] siggi `05`, quoting the Rockbox wiki; [rockbox] (by construction)
