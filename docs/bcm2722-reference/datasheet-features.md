# BCM2722 datasheet features

The project-wide hardware catalog's BCM half (`docs/hardware-features.md` on main cites it). It holds one row per claim in Broadcom's product brief ([external] 2722-PB01-R, 2005-10-18), in the catalog's shape.

**Status** is one of:
- **used:** by Apple, by us, or by both; the evidence says which;
- **known-unused:** absent from the iPod's board, or never used;
- **unknown:** to be probed;
- **unverifiable:** can't be reached on this device.

**Sources:**
- the display, TV out, audio, peripherals and media rows come from [`brief-map-peripherals.md`](brief-map-peripherals.md), where the evidence and the probe stages are;
- the power, bus, clocks and memory rows come from [`brief-map-power.md`](brief-map-power.md), where the stages PW1–PW8 are.

## Display, TV out, audio, peripherals and media

| feature | source | status | evidence tag | ProkBox benefit | probe |
|---|---|---|---|---|---|
| TV out, composite/S-Video (timing generator `0x1000_2C00`) | brief; Apple @5BCE | used | [apple] | High: TV out for videos | PB-7, PB-8 |
| Video DAC `0x1000_5400` | Apple @5BCE, @13454 | used | [apple] | Needed for TV out | PB-1, PB-8 |
| TV FIFO pixel format | Apple @7C9A | unknown | [hypothesis] | Required for TV out | PB-8 |
| Macrovision | brief; Apple @13454 | used | [apple] | None (we write the off set) | PB-8 |
| TV-path scaling (640 → 704) | brief "scalable output" | unknown | [hypothesis] | Free horizontal scale on TV | PB-9 |
| LCD scaler | brief | known-unused | [apple] | None (vector-unit scaler stays) | — |
| Multiple displays / sub-LCD | brief | known-unused | [apple] | None | — |
| 8/9/16/18/21-bit video, RGB bus | brief | known-unused | [hypothesis] | None (MPU panel) | — |
| Configurable refresh rate | brief | known-unused | [apple] | Use TE sync instead | #52 |
| I²S audio out (`0x1000_4800`) | brief; Apple; S35 | used | [silicon] | Soundtrack/music while PP sleeps | dev45 8a |
| Audio clocks `0x1000_1C34`/`0x1C38` | Apple @5334 | unknown | [apple] | Completes BCM audio path | PB-5 (in 8a-1) |
| AC'97, PCM audio | brief | known-unused | [apple] | None | — |
| Audio in, recording | brief | known-unused | [apple] | None | — |
| 64-voice MIDI synth | brief | known-unused | [apple] | Medium-low: our own synth | after #54 |
| 3D graphics (RenderServer) | brief; Apple RenderServer | used | [apple] | Confirms software 3D route | stage 3d |
| I²C master | brief | known-unused | [hypothesis] | None | PB-4 |
| SPI master | brief | known-unused | [hypothesis] | None | PB-4 |
| UART (Bluetooth, IrDA) | brief | known-unused | [hypothesis] | Low: use the PP's dock UART | PB-4 |
| USB 1.1 slave (`0x1000_6400`) | brief; Apple @149C6 | unknown | [apple] | None expected | PB-3 |
| GPIO, 52 pins | brief; Apple boot map | used | [apple] | Pin census | PB-2, PB-4 |
| GPIO 19 TV enable | Apple @5BCE | used | [apple] | Needed for TV out | PB-8 |
| JTAG, boundary scan | brief | unverifiable | [external] | None | — |
| Camera interface, raw Bayer | brief; Apple @13994 | known-unused | [apple] | None | — |
| SD/MMC/SDIO/Memory Stick | brief | known-unused | [apple] | None | — |
| NAND/NOR boot, second memory I/F | brief | known-unused | [apple] | None | — |
| Unique chip ID | brief | unknown | [apple] | None (note only) | PB-1 (`sys19`) |
| Encrypted code, programmable key | brief | known-unused | [apple] | None | — |
| AES/3DES/RSA/CPRM | brief; Apple passthruhandler | used | [apple] | None (software AES only) | — |
| MPEG-4/H.263 decode | brief; Apple mpg4dec | used | [apple] | Planned (`mpeg4.md`) | — |
| H.264 decode | Apple h264dec; S27 | used | [silicon] | Shipping path | — |
| MPEG-4 encode | brief | known-unused | [apple] | None | — |
| JPEG decode | brief | known-unused | [apple] | High: album-art builder | jpeg-spike S3–S5 |
| JPEG encode | brief | known-unused | [apple] | None | — |
| 3GP/MP4 streaming, MMS | brief; Apple mplayer | used | [apple] | None (PP demuxes) | — |

## Power, bus, clocks and memory

| feature | source | status | evidence tag | ProkBox benefit | probe |
|---|---|---|---|---|---|
| Bus bypass mode (host reaches LCD with BCM powered down) | brief: "Bypass mode allows host access…" | unknown | [hypothesis] | lit static screen with BCM off | PW6 |
| Run mode with DVFS (5–150 MHz, 1 MHz steps) | Apple `pm_set_policy min` @fe00 | used | [apple] | pick video clock per stream | PW4 |
| Sleep (core `sleep` instruction idle) | Apple idle loop; brief "sleep" | used | [apple] | idle C2 without polling | stage 5i |
| Hibernate (`videocore=1`, GPO32 bit 15 then bit 14 low) | Apple @14288, host `0x1028_92D4` | known-unused | [apple] | keep SRAM; wake in ~10 ms, not 0.4–0.6 s | PW5 |
| Power-down (`videocore=0` + bit 14 low) | Apple @14244; Rockbox cut | used | [silicon] | today's dark state | — |
| 0.6 V hibernation core | brief | unknown | [external] | lowest non-off state | PW5 + meter |
| Core regulator / SMPS, levels 5–15 | `0x1000_1400` [3:0]; Apple table `0x1FA68` | used | [apple] | level 5 at 27 MHz (we run 11): ~40 % less core power | PW3 |
| Regulator bits [7:4] | `0x1000_1400` | unknown | [silicon] | none known | PW1 |
| Programmable I/O 1.5–2.8 V | brief | unverifiable | [external] | none | — |
| PLL A (core clock) | `0x1000_1C00/14`; S7 | used | [silicon] | 150 MHz video | — |
| PLLs B, C, D ("3 PLLs for external devices") | brief; Apple @14912 | known-unused | [apple] | exact audio clocks (PLL C) | PW1 |
| Block clock words (src [10:8], div [7:0]) | Apple @4d96, @5334 | used | [apple] | find clocks left running | PW1 |
| Audio clocks 24.576 / 2.048 MHz (ids 2, 3) | Apple @13c32 | known-unused | [apple] | BCM audio path | see peripherals |
| SDRAM refresh follows the clock | Apple @4d18 | used | [apple] | — | — |
| I + D caches, 128 Kbit | brief; `0x1000_0C00` | known-unused | [apple] | SDRAM code/data speed | PW2 |
| Caches cover SDRAM only | Apple's flush sites @88a0, @91a4 | unknown | [hypothesis] | no flushes for SRAM rings | PW2 |
| 1.25 MB SRAM | brief; S1 on | used | [silicon] | — | — |
| 4 MB stacked SDRAM | brief; S10, S11 | used | [silicon] | — | — |
| SRAM retained in hibernate | Apple wake path @14288 | unknown | [apple] | slots survive dark | PW5 |
| SDRAM retained in hibernate | Apple probe save/restore @147ca | unknown | [hypothesis] | pools survive dark | PW5 |
| High-speed DMA, SRAM/LCD | brief; `0x1000_0800`; S4–S7 | used | [silicon] | LCD push | — |
| DMA memcpy SRAM ↔ SDRAM (ch 7) | Apple `dma_memcpy` | known-unused | [apple] | textures without the core | PW8 |
| Secondary memory interface (= LCD block CS0) | brief; `lcd.md` | used | [hypothesis] | bypass target | PW6 |
| NAND/NOR flash, NAND boot | brief | unverifiable | [external] | none (not fitted) | — |
| Memory-mapped host port, 16-bit | brief; `0x3000_0000` | used | [silicon] | — | — |
| 3-bit host address (2 banks × 4 regs) | brief; host bits 16–18 | used | [hypothesis] | — | PW7 |
| Dual software channel (ALT bank) | brief; Apple display path on bank 1 | known-unused | [apple] | per-core channels, no shared latch | PW7 |
| Programmable interface type (bootstrap bytes) | brief; #24 | unknown | [hypothesis] | none yet | — |
