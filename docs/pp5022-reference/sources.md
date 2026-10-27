# Sources

## Rockbox (evidence tag [rockbox])

The Rockbox source tree as carried in this repository (GPL-2.0-or-later). Files cited throughout:

| Area | Files |
|---|---|
| Register map | `firmware/export/pp5020.h` (shared by PP5020/PP5022/PP5024; its header credits the iPodLinux project), `firmware/export/config/ipodvideo.h`, `firmware/export/config.h` |
| Start-up | `firmware/target/arm/pp/crt0-pp.S`, `crt0-pp-bl.S`, `app-pp.lds`, `boot-pp.lds`, `firmware/rolo.c` |
| System | `firmware/target/arm/pp/system-pp502x.c`, `system-target.h`, `kernel-pp.c`, `timer-pp.c`, `thread-pp.c`, `firmware/asm/arm/corelock.c`, `firmware/system.c` |
| Audio | `firmware/target/arm/pp/i2s-pp.c`, `pcm-pp.c`, `wmcodec-pp.c`, `firmware/drivers/audio/wm8758.c`, `firmware/export/wm8758.h` |
| Buses | `firmware/target/arm/pp/i2c-pp.c`, `uart-pp.c`, `debug-pp.c` |
| Storage | `firmware/target/arm/pp/ata-pp5020.c`, `ata-target.h`, `firmware/target/arm/ata-as-arm.S`, `firmware/drivers/ata.c`, `ata-common.c` |
| USB | `firmware/target/arm/usb-drv-arc.c`, `firmware/target/arm/pp/usb-fw-pp502x.c` |
| Board | `firmware/target/arm/ipod/button-clickwheel.c`, `backlight-nano_video.c`, `piezo.c`, `power-ipod.c`, `adc-ipod-pcf.c`, `powermgmt-ipod-pcf.c`, `video/lcd-video.c`, `video/lcd-as-video.S`, `video/battery-video.c`, `firmware/drivers/pcf50605.c`, `firmware/drivers/rtc/rtc_pcf50605.c`, `firmware/export/pcf5060x.h`, `firmware/export/hwcompat.h` |
| Boot chain | `bootloader/ipod.c`, `firmware/common/rb-loader.c`, `tools/scramble.c`, `utils/ipodpatcher/` |

Where our firmware changed one of these files, the fact is taken from the upstream behaviour unless tagged [ours].

## The iPodLinux project (evidence tag [external])

The original source of most PP502x register names (credited in `pp5020.h`). Its wiki and kernel sources were not consulted directly for this edition; facts attributed to it here come through Rockbox's comments. A direct pass over its hardware pages is a to-do.

## The siggifly emulator notes (evidence tag [external], cited as "siggi *nn*")

An independent project that emulates the iPod Video (Rust; running Apple's boot ROM, RetailOS and the clickwheel games against modelled hardware, with some reads on a retail 5.5G under Rockbox through a serial console). Its notes are numbered files; the ones cited here are `02` (RetailOS boot), `03` (RTXC and the video coprocessor), `04` (bypass ledger), `05` (chip inventory), `06` (Rockbox as oracle), `07` (the flash images), `08` (differential register map), `09` (what the hardware must supply), `10` (resource image, PP-side addenda only), `15` (register agreement table), `16` (the third bootloader), `17` (the boot matrix). The notes carry a Creative Commons Attribution-ShareAlike 4.0 licence text with no named holder. Facts were restated in our own words; nothing was copied. Their tags distinguish what came from Apple's code under emulation, from Rockbox, from iPodLinux, from hardware reads and from guesses; we carried those distinctions over, and anything they mark as a guess is a [hypothesis] here or omitted.

## Apple's firmware (evidence tag [apple])

Apple's iPod Video firmware 1.3 (`osos`, the ARM operating system image), read for addresses and constants only, as recorded in `../bcm2722-reference/` and our research notes. Not redistributed. Addresses are cited as `osos 0x1xxx_xxxx`. The second DMA engine (`dma.md` §6) was read by us directly on 2026-10-15: `osos.code` from firmware 1.3 disassembled with `arm-none-eabi-objdump` at base `0x1000_0000` (Apple's code pointers in vtables are offsets from that base). Only register addresses, values and their order were recorded; no code was copied. The 2026-10-24 pass for `mac-and-dvs.md` read the same image linked at 0 (code pointers equal file offsets; cited there as `0x1000_0000` + offset), checked against firmware 1.2.1 and 1.2.3, with throwaway scripts kept outside git.

## ARM documentation (evidence tag [external])

ARM7TDMI Technical Reference Manual (ARM DDI 0210): the core's multiplier and the long multiply-accumulate instructions, cited in `mac-and-dvs.md` §1.

## PortalPlayer product briefs (evidence tag [external]/[datasheet])

The PP5002, PP5003, PP5020, PP5022 and PP5024 briefs listed in `datasheet-features.md` §1, read from local copies; never redistributed.

## Our device sessions (evidence tag [silicon])

All on one iPod Video 5.5G (MA450, 80 GB model, 64 MB SDRAM), flash-modded (1 TB), USB-C-modded, 3000 mAh cell. Session numbers follow `../bcm2722-reference/sources.md`; S31–S34 are listed here.

| Session | Date | PP-side results used in this manual |
|---|---|---|
| S2 | 2026-10-10 | host-port command round trip 6–7 µs |
| S8 | 2026-10-11/12 | host-port upload lab: CPU copy and DMA channel 3 into BCM SRAM, at 30 and 80 MHz, all clean; timings in `display.md` §5 |
| S9 onward | 2026-10-12/13 | every cold boot finds the BCM powered and running Apple's flash image |
| S11, S12 | 2026-10-13 | torn 32-bit host-port reads; handshake waits under load (≤ 47 µs, no timeouts) |
| S26–S31 | 2026-10-13 | thread starvation at 30 MHz behind the codec's priority (S31: up to 2.1 s late) |
| S30–S34 | 2026-10-13 | first LCD frame 279.8 ms after BCM power-up |
| S32 | 2026-10-13 | PP DMA into the BCM SDRAM window corrupt; BCM sees the codec's BCLK and LRCK (88,198 edges/s) |
| S33 | 2026-10-13 | word 0 of every DMA block lost; CPU copy clean |
| S34 | 2026-10-13 | DMA lab 256 B–32 KB (all wrong into SDRAM); BCM GPIO 1 not on the PP's I2S data; the scheduling matrix (priority over boost) |
| power logs | 2026-10-12/13 | voltage-only discharge and charge segments; DAC wake times |
| USB/ATA logs | 2026-10-10 to 13 | UDMA 2 negotiated; USB and flash throughput |

Logs are kept in the owner's device backups; they are not part of this manual.
