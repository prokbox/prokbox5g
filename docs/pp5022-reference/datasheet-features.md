# Datasheet features: what PortalPlayer claimed, and what we use

PortalPlayer never published a full datasheet or register manual. What exists publicly is a set of **three-to-five page product briefs** (the "PP5022 / PP5020 / PP5024 digital media management system-on-chip" sheets). This page extracts every feature those briefs claim and sets each against our manuals and code. It is the PP5022 counterpart to the BCM2722 factsheet pass.

## 1. Sources

All fetched 2026-10-23 through the Wayback Machine; the original links are listed on the Rockbox wiki page "PortalPlayer". Local copies (untrusted, text-extracted only) are in the session scratchpad, `.../scratchpad/pp-datasheets/`.

| Tag | Document | URL | Date on sheet |
|---|---|---|---|
| B22 | PP5022 brief, 3 pp. (**our chip**) | `http://web.archive.org/web/20060908220503/http://www.portalplayer.com/products/documents/5022_Brief_Mar05.pdf` | 03/02/05 |
| B20 | PP5020 brief, 4 pp. | `http://web.archive.org/web/20061202104042/http://www.portalplayer.com/products/documents/5020_Brief_0108_Public.pdf` | 01/08/04 |
| B24 | PP5024 brief, 5 pp. (adds analog PMU, DAC, NAND) | `http://web.archive.org/web/20060707072303/http://www.portalplayer.com/products/documents/5024_Brief_Mar05.pdf` | 03/02/05 |
| B03 | PP5003 brief (older family, for context: names a vector processor, SD security) | `http://web.archive.org/web/20070218233330/http://portalplayer.com/products/documents/5003_brief_0108_Public.pdf` | 01/08/04 |
| B02 | PP5002 brief (older, context only) | `http://web.archive.org/web/20061202104706/http://www.portalplayer.com/products/documents/5002_brief_0108_Public.pdf` | 01/08/04 |

Not found: any register-level datasheet, TRM, or NVIDIA-hosted PortalPlayer document. The only other public hardware knowledge is the iPodLinux / Rockbox register work already folded into these manuals (`sources.md`). The briefs give **feature names and headline numbers only, no addresses and no page-level detail**, so "page" below means the brief and its section.

Caveats that apply to every row:

- The briefs describe the **die**, not the iPod Video board. A feature can exist in silicon yet have no pins, no external part or no clock on the 5.5G (for example S/P-DIF, SD/MMC, image-sensor input and the LCD controllers have no wiring we know of here).
- The brief is marketing: numbers are "up to". The 5022 brief says 100 MHz; Apple and Rockbox top out at 80 MHz. Voltages are the vendor's recommended window, not a promise for our silicon.
- Everything the brief claims carries the tag **[datasheet]**. Status tags follow `index.md`.

## 2. Status key

- **used**: we use it (where).
- **known, unused**: our manuals know the block or register; nothing in the firmware uses it.
- **unknown to our docs**: the sheet claims it and no page of ours mentions it.
- **likely absent / unverifiable here**: the die may have it; the iPod Video board does not wire it, or we cannot reach it.

## 3. Feature table

### 3.1 Processors, clocks, power

| # | Feature (brief claim) | Source | Our status | Evidence | Potential benefit | Cheap probe |
|---|---|---|---|---|---|---|
| 1 | Two ARM7TDMI cores, "200 MIPS" total (160 for PP5020) | B22 p1; B20 p1 | **used** (CPU + COP; COP runs the codec, `data-paths.md` cell 8) | [datasheet] [silicon] | none new | none |
| 2 | **Up to 100 MHz per core** (PP5020: 80) | B22 p2 | **known, unused**. Rockbox stops at 80 MHz. **Apple's PP5022 clock driver accepts up to 125 MHz** and reads a per-chip speed grade from `STRAP_OPT_A[6:5]` (133/100/80/66 MHz); it uses `DEV_TIMING1` `0x0808` at 100 MHz, Rockbox's current 80 MHz value (`mac-and-dvs.md` §3) | [datasheet]; [apple] for the driver; [hypothesis] for the grade's meaning | 25 % more cycles for the boosted state (heavy codecs, the video feed, library builds). Little battery value at a fixed voltage: energy per cycle is about constant | `mac-and-dvs.md` §5.2 (read the grade, read-only), then §5.5 `pll100` (owner's go): PLL `0x8A12_1903`, `DEV_TIMING1` `0x0909` then `0x0808`, timed loop plus CRC load |
| 3 | **Independent clock-skipping on the COP** | B22/B20/B24 p2 | **known, unused**: `CPU_CTL`/`COP_CTL` low byte = "cycles to skip on next instruction" (`pp5020.h`), our `cores-and-threads.md` knows sleep/wake; skipping as a throttle is not used | [datasheet]; [rockbox] register comment | Run the COP at a fraction of the clock while the CPU stays full-speed (or the reverse), a finer power step than 24/30/80 MHz and no PLL change; useful for the COP-resident codec when it only needs ~40 % of 30 MHz | Write skip counts to the low byte of `COP_CTL` between instructions of a COP counting loop and time it against `USEC_TIMER`. The header's example stalls one `nop` for 128 cycles, so it looks like a per-instruction stall, not a duty cycle, and probably saves little energy unless the stalled core also stops drawing power; settle that with the voltage log |
| 4 | **Clock programmable 32 kHz to 100 MHz** | B22 p2, p3 | **used** partly: 32 kHz, 24 MHz, 30 MHz and 80 MHz exist in `system-pp502x.c`; our `open-questions.md` #9 notes selectors #1, #3, #4, the DIV fields and "fixed 16/33/48 MHz taps" are not understood | [datasheet]; [rockbox] | Intermediate frequencies (e.g. 48 MHz without the PLL) for finer power steps | Open question #9's probe: switch the selector and time against `USEC_TIMER` |
| 5 | **"System-enabled dynamic voltage scaling" and an "ultra-low 1.2 V core"; core window 1.14-1.32 V** | B22 p1, p2, p3 | **known, unused**. Core fixed at `DCDC1 = 0xEC`. **Apple does not scale the PP5022's core voltage either**: its only DCDC1 writer skips the PP5022 (`mac-and-dvs.md` §2.4). PMU field layout settled: `[7:5]` mode, `[4:0]` code; LDOs 0.9 V + 0.1 V × code; core converter 0.6 V + 0.05 V × code favoured (low-medium confidence). Under both candidate formulas `0xEB`–`0xEE` lie inside the window | [datasheet]; [rockbox]; [apple]; [hypothesis] for the core formula | One step down at 30 MHz: −4 to −8 % core dynamic power, a few mW in the dominant playback state [estimate] | `mac-and-dvs.md` §5.3 `pmu reg` (read-only), §5.4 `dvs step` (owner's go; a meter on the core rail settles the formula). Never outside `0xEB`–`0xEE`; disk idle, USB off |
| 6 | **Modular suspend/resume; "shutdown of most functional modules when not in use"; 300 % power reduction vs PP5020 (their words)** | B22 p2 | **known, partly used**: `DEV_EN`/`DEV_EN2` and `DEV_RS` are written at init (`system-pp502x.c`), and `clocks-power.md` lists the bits, but we do not audit which blocks stay clocked after boot (FireWire, USB1, serial, I2C, PWM...). Not done on the idle path | [datasheet]; [rockbox] for the registers | Possible small standby saving: clear `DEV_EN` bits for blocks nothing uses (FireWire link, second USB port, unused UART, PWM when the click is off, LCD bridge); each saves a clock tree | Dump `DEV_EN`, `DEV_EN2`, `DEV_INIT1/2` after boot; clear one candidate bit at a time in a test build and confirm nothing else breaks (USB, clickwheel, piezo); compare idle voltage drop over 30 min. See `clocks-power.md` §5 |
| 7 | Standby power (listed "TBD") | B22 p3 | not usable (no number published) | [datasheet] | none | measure ourselves (`open-questions.md` #25) |
| 8 | 8 KB unified cache per core | B22 p2 | **used** | [datasheet] [rockbox] | none | none |
| 9 | "Efficient cross-bar giving **zero-wait-state access to internal RAM**" | B22 p2 | **used**: IRAM single-cycle, shared; the cross-bar means both cores can hit IRAM in the same cycle on different banks (**banking is not in our docs**) | [datasheet]; [rockbox] for single-cycle | If IRAM is multi-bank, the COP and CPU placing their hot data in different banks would stop them stalling each other (today `cores-and-threads.md` warns the cores "compete for the same IRAM") | Both cores run a read loop over IRAM at stride 4: same 4 KB region vs different 32 KB regions; compare iteration time. Cheap, read-only |
| 10 | **128 KB on-chip SRAM** | B22 p2 | **used** (96 KB on PP5020) | [datasheet] [rockbox] | none | none |
| 11 | **"Integrated MAC for ultra-efficient encode and decode"** | B22 p1, p2 only (the PP5020 and PP5024 briefs do not use the word; the PP5003 brief's "MAC" is an Ethernet MAC) | **answered: it is the ARM7TDMI's own multiplier** (the "M": `MLA`, `SMULL`, `SMLAL`). Apple's firmware uses no coprocessor instructions and no unknown peripheral in its codec paths; its IRAM synthesis loop is pure `SMULL`/`SMLAL` (`mac-and-dvs.md` §1). The one unmapped block Apple touches, `0x6000_3000`, is a permutation table (bus arbitration [hypothesis]), not arithmetic | [datasheet]; [external] ARM DDI 0210; [apple] | None beyond what Rockbox's ARM codec assembly already does | `mac-and-dvs.md` §5.1 `mmio scan` (read-only census of unmapped pages) closes the last gap; §5.2 reads `0x6000_3000` |
| 12 | **Six DMA channels** | B22 p2; B20; B24 | **used / known**: four in the general engine (`0x6000_B000`) plus two in the second engine (`0x6000_9000`), exactly six; engine 2 proven for the BCM port (`dma.md`, `data-paths.md` cell 14) | [datasheet]; [silicon] S37 for engine 2 | already covered. The brief's count matches what we found, which supports reading the second engine as part of the stock six | none |
| 13 | JTAG port, "full in-circuit emulation and flash programming" | B22 p2 | **likely absent / unverifiable** as a service: pins exist on the die; we do not know whether they are routed on the 5G board (test pads?) | [datasheet] | Debugging and bricked-unit recovery; would be the "recovery path" without Apple's firmware | Look for test pads on the board photos (`docs/reference` teardowns); not a firmware item |
| 14 | Boot BIOS EEPROM (2 KB, optional) / **boot controller** | B22 diagram | **known**: iPod boots from NOR flash through Apple's ROM (`boot.md`) | [datasheet] [rockbox] | none | none |

### 3.2 Memory

| # | Feature | Source | Our status | Evidence | Potential benefit | Cheap probe |
|---|---|---|---|---|---|---|
| 15 | SDRAM controller: **two banks, up to 128 MB each, 16-bit bus**; 1.8 V and 2.5 V mobile SDRAM | B22 p2; B20 p2 | **known**: 32/64 MB, registers undocumented (`open-questions.md` #1, #10) | [datasheet]; [rockbox] | Confirms nothing beyond 64 MB is a limit of the controller; a 128 MB SDRAM upgrade is plausible if a second bank/density is wired (a hardware mod, not firmware). Also confirms SDRAM timing is tunable (`DEV_TIMING1`) | `DEV_TIMING1` sweep while running a memory pattern test (#10). Read-only readback of the SDRAM controller after Apple's ROM (#1) |
| 16 | **Two banks of NOR flash up to 128 MB each**, 8/16-bit | B22 p2 | **known**: 1 MB NOR holds Apple's ROM (`boot.md`) | [datasheet] | none for ProkBox (read only; writing is the recovery risk) | none |
| 17 | Serial EEPROM support / 2 KB boot EEPROM | B24 p2 | **likely absent** on the iPod Video (boots NOR) | [datasheet] | none | none |

### 3.3 Audio

| # | Feature | Source | Our status | Evidence | Potential benefit | Cheap probe |
|---|---|---|---|---|---|---|
| 18 | **I2S stereo codec interface, master or slave modes** | B22 p2 | **used**: the WM8758 is I2S master; the PP is slave and receives BCLK/LRCK (`audio.md`, `i2s-pp.c`) | [datasheet] [silicon] | A PP-master mode (`IISDIV`, `clocks-power.md` §5, "used only when PP is I2S master") would let the PP choose sample rates without the codec's PLL; we do not need it | none |
| 19 | I2S **receive** (RX) path, microphone / line-in recording | B22 diagram ("I2S RX/TX"); B20 "encoding of MP3 and ACELP" | **known, unused**: `IISFIFO_RD` exists in `pp5020.h`; the 5.5G has the WM8758 mic/line-in ADC but no mic on the board (dock line-in is not wired for audio on our USB-C unit) | [datasheet] [rockbox] | Voice-note recording or audiobook dictation if a mic or the dock input exists. Very low priority for a playback product | I2S RX test only with a mic: skip |
| 20 | **S/P-DIF** optical in/out | B22 p2 diagram, p2; B20 p2 | **unknown to our docs** (no S/P-DIF register is listed in `pp5020.h` or our manuals) | [datasheet]; no S/P-DIF pin or transceiver on the iPod Video; the dock has line-out and serial, not S/P-DIF | **likely absent / unverifiable.** If the die had it and a pin were reachable, bit-perfect digital out; with USB-C already on board this is moot | none practical |
| 21 | Real-time decode of MP3, WMA, AAC, ACELP.NET, JPEG, MJPEG, **MPEG-4 simple-profile**; "high-speed encode" of MP3 and ACELP.NET and JPEG | B22 p1; B20; B24 | the *software* codecs are PortalPlayer's firmware, not a hardware block; Rockbox has its own for each. The brief does not show a hardware decoder, only the MAC (row 11) | [datasheet] | none directly. MPEG-4 SP at CIF suggests the MAC is meant for video; relevant if videos "come back" (project note) | see row 11 |
| 22 | Stereo DAC, headphone amp, line mixer (**PP5024 only**) | B24 | **likely absent**: the iPod Video uses an external WM8758 | [datasheet] | none (PP5022 has no analog block) | none |

### 3.4 Storage and USB

| # | Feature | Source | Our status | Evidence | Potential benefit | Cheap probe |
|---|---|---|---|---|---|---|
| 23 | **Dedicated dual-channel ATA-66 IDE controller with its own DMA engine; up to four drives** | B22 p1, p2 | **used**: UDMA2 negotiated; UDMA4 forced works (0 bad words) but reads 21 MB/s, same as UDMA2 (`storage.md`, open question #28). **"ATA-66" means UDMA mode 4 is the chip's ceiling**, so the 21 MB/s limit is adapter or per-command overhead, not a silicon cap | [datasheet] [silicon] | Confirms we are at chip limit (66 MB/s theoretical); the remaining question is overhead, not capability. The "dual-channel" detail means a **second drive** would be possible on the same controller, relevant only to an exotic mod | DP-5 timing of large vs small commands (`data-paths.md`) |
| 24 | **Hi-Speed USB 2.0 host *and* device (OTG), "any transfer speed"** | B22 p1, p2; B20 | **used** as device (mass storage, `usb.md`). **Host/OTG: known, unused**: ARC controller registers allow host mode, Rockbox never uses it on this port | [datasheet] [rockbox] | Our USB-C mod means the connector could do **host mode**: plug in a USB flash drive or SD reader to import/export music with no computer (the mission's "easy to copy back off or share"), or a USB DAC/headphone path if a UAC host stack existed. Needs an OTG ID resolver and a 5 V VBUS source on the board (probably absent: the board has charger-in only) | Read `PORTSC1`/`OTGSC` (read-only) and see whether the ID pin state is visible; check whether the LTC4066 can source VBUS (it can't, it is a charger); mostly a hardware question |
| 25 | **Integrated SDMMC controller, "enables SDIO compatibility"** (PP5022, PP5024; not in the PP5020 brief) | B22 p1, p2; B24 diagram | **known, unused**: `dma.md` records request ID 13 = SD host controller for Sansa; the iPod Video has no SD slot, so no pins | [datasheet]; [rockbox] request ID; **likely absent on the board** | Not for the iPod Video (no slot). If a mod board exposed the pins, a second storage target. Unverifiable without a pin trace | none |
| 26 | **CompactFlash interface; XIO-emulated SmartMedia/NAND interface; "High-speed NAND I/F, MLC/SLC, multi-bit ECC, up to 16 GB" (PP5024)** | B22 p2, B24 p1-2 | **unknown to our docs** as a native NAND controller. Our unit uses a CF/SD-style IDE flash adapter on the ATA bus | [datasheet] | Direct NAND via XIO would skip the adapter's controller. No pins on this board; also needs a flash translation layer. Out of scope | none |
| 27 | **FireWire (IEEE1394a) link layer** (PP5020 brief only; B22 shows an "iLink" host box) | B20 p1; B22 diagram | **known, unused**: `DEV_FIREWIRE`, `FIREWIRE_IRQ`, `0xC600_0000` (`memory-map.md` §1); the dock's FireWire pins carry charge power only | [datasheet] [rockbox] | None. Power: make sure its clock is gated (row 6) | See row 6 |
| 28 | **Four ATA drives incl. CD-ROM / CD-R/W** | B22 p1 | not applicable | [datasheet] | none | none |

### 3.5 Display and video

| # | Feature | Source | Our status | Evidence | Potential benefit | Cheap probe |
|---|---|---|---|---|---|---|
| 29 | **DTV controller: TFT panels up to 640x480, 18-bit RGB565, 60 Hz** | B22 p2; B20 p2 | **known, unused** by design: the iPod Video's panel hangs off the BCM2722, not the PP (`display.md`). `DEV_LCD`, `CLCD_CLOCK_SRC` exist as names only | [datasheet]; [rockbox] names | A native LCD controller path could bypass the BCM host port (10.5 PP cycles per 16-bit write) if the panel could be driven by the PP: **no**, the panel is wired to the BCM. Keep it as a note | none |
| 30 | Integrated STN LCD controller; bridge interface to intelligent LCDs (1/4/8-bit) | B22; B20; B24 | **known**: the bridge blocks (`LCD1`/`LCD2`) exist and drove earlier iPods' LCDs; unused on the Video | [datasheet] [rockbox] | none | none |
| 31 | **TV out: CCIR 601/656 8-bit digital video to an external NTSC/PAL encoder** | B22 p1, p2; B20; B24 | **likely absent here**: the Video's TV-out is the BCM's own encoder (`bcm2722-reference/overview.md`), not the PP's CCIR port | [datasheet]; [hypothesis] no pins | none on this board | none |
| 32 | **Video *input*: CCIR-compatible digital video input for image sensors** | B22 p1, p2; B24 | **unknown to our docs**; no pins on the iPod | [datasheet] | none (no camera). Only if the BCM camera-interface were wired to the PP, which we have no evidence of | none |
| 33 | Four **PWM** outputs "for contrast and brightness control, tone generation" | B22 p1-p2; B20 | **partly used**: one PWM channel drives the piezo/haptic click (`PWM0_CTRL`, `input.md`); a **second to fourth channel and hardware backlight PWM are known, unused**: our backlight uses software PWM from a timer on GPIOL7 (`gpio-i2c.md`) | [datasheet] [rockbox] | **Hardware PWM for the backlight and the haptic** would remove a timer interrupt and its CPU wakeups (the `hardware-fit-audit.md` K1 item: wake-ups nobody measured), and give smoother fades. Needs the backlight line to be routable to a PWM pin, which on the 5.5G is GPIOL7 (not a PWM pin as far as we know) | Read the words after `PWM0_CTRL` (`0x7000_A000`; further channels are [hypothesis], addresses unknown) for non-zero and probe whether GPIOL7 can be muxed. Low risk |

### 3.6 Peripherals and I/O

| # | Feature | Source | Our status | Evidence | Potential benefit | Cheap probe |
|---|---|---|---|---|---|---|
| 34 | **8-bit, 4-channel on-chip ADC** (battery, thermal, touchscreen); `ADC_CLOCK_SRC` | B22 p1, p2; B20; B24 ("10-bit" PMU ADC separate) | **known, unused**: `ADC_CLOCK_SRC` is "name only" (`clocks-power.md` §5); the iPod reads battery through the PMU's 10-bit ADC over I2C (`clocks-power.md` §8). The on-die ADC could sample a thermistor or the battery without an I2C round trip, but needs input pins routed to it | [datasheet] [rockbox] name | A battery-voltage read without I2C would save the 400 ms-cached scan cost and a PMU transaction; a temperature read would let us derate charging (the LTC4066 has no readback we use). Only if the board wires any pin to ADC0-3 | Enable `ADC_CLOCK_SRC`, read the ADC register block (unknown address) for non-zero; grep Apple `osos` for its use; risk low (read) |
| 35 | I2C master **and slave** mode | B22 p2 | **used** (master; `i2c-pp.c`); **slave mode unknown to our docs** | [datasheet] | none (no second master) | none |
| 36 | RS-232 serial (UARTs), three-wire controller (TWC/SLINK) | B22 p1, p2 | **known**: UART0 on the dock, UART1 (`uart-pp.c`); TWC is a CD-ROM/CDDA interface, no use here | [datasheet] [rockbox] | UART as a **debug log channel** over the dock pins is gone on the USB-C unit; no gain | none |
| 37 | **13 dedicated GPIOs for navigation; 5x8 key matrix up to 40 buttons; four opto sensor inputs for 3D joysticks / XYZ** | B22 p2; B20; B24 | **used / known**: the click wheel is read through the "opto" serial block (`input.md`); the matrix and the other opto inputs are not wired | [datasheet] [rockbox] | none (the controller is the Apple wheel block) | none |
| 38 | **Infrared remote support** | B22/B20/B24 | **likely absent** (no IR receiver on the board) | [datasheet] | none | none |
| 39 | **RTC** (PP5024 integrated, 32 kHz osc, 32-bit second counter, battery-backed 128-bit scratch-pad registers) | B24 p4; B22 diagram "RTC" | **known**: `RTC` register `0x6000_5014` in `pp5020.h` exists as a name. The iPod's real clock is the PMU's, over I2C (`clocks-power.md` §8). **The PP5024 sheet says its RTC has 128 bits of battery-backed scratch registers; our open question #33 asks for exactly such a scratch byte** | [datasheet] for PP5024; the PP5022 diagram only says "RTC" | A free, battery-backed byte store would hold the drained latch the boot guard needs, readable before storage (open question #33). Applies only if the **PP5022**'s own RTC has the same scratch registers: the 5022 brief does not claim them | Read `0x6000_5014` repeatedly and over a power cycle (does it keep counting? does it survive a standby?); look for neighbouring words that are writable and survive PMU standby (question #33's own probe). Cheap, read/write one word |
| 40 | Timers, `USEC_TIMER` | (not claimed) | **used** | [rockbox] | none | none |
| 41 | **Unique ID** block (PP5024 diagram) | B24 diagram | **unknown to our docs** | [datasheet] | A per-unit serial without reading the disk | Read around `PP_VER` (`0x7000_0000`...) for a stable per-unit value; trivial |
| 42 | Security: PP5003 has "hardware-enabled security with Memory Stick/MagicGate/SD secure" | B03 | **not claimed for PP5022** (absent from B22) | [datasheet] for PP5003 only | none | none |
| 43 | Microsoft WMP10 DRM "PlaysForSure ready", Java MIDP 2.0, PictBridge | B24 | software features of the PP5024 firmware, not hardware | [datasheet] | none | none |

### 3.7 Analog and power blocks (PP5024 only)

The PP5024 integrates what the iPod Video does with the PCF50605 and WM8758: a DC-DC with charge pumps (1.2 V core at 10 mV steps, 1.8 V memory at 25 mV steps), five LDOs, a Li-ion charger (100-400 mA in 50 mA steps, 4.2 V in 50 mV steps), a 15 V LED backlight driver (2.5-40 mA in 2.5 mA steps), a 10-bit PMU ADC, a stereo DAC (93 dB, 2x40 mW headphone amp) and an analog mixer [B24 p3-4]. **None of that exists on the iPod Video**: it uses a PP5022 plus discrete PCF50605/50607, WM8758 and LTC4066 (`overview.md` §2). The only lesson worth drawing is the **voltage resolution** a PortalPlayer-style regulator offers (10 mV core steps), which supports row 5: if the PCF50605 family has comparable steps, a lower core voltage at 30 MHz is a realistic probe.

## 4. Things the briefs tell us that our docs did not

1. **An "integrated MAC"** is advertised on the PP5022 brief only. Desk study: it is the cores' own long multiply-accumulate, not a block (row 11, `mac-and-dvs.md`).
2. **100 MHz** is the claimed maximum (row 2), not 80 MHz.
3. **Dynamic voltage scaling** is a stated design feature, and the core window is 1.14-1.32 V (row 5).
4. **Per-core clock skipping** is advertised as a feature (row 3), not just a register curiosity.
5. The **six DMA channels** match our four-plus-two finding, which is welcome confirmation (row 12).
6. **On-die 4-channel ADC, four PWM channels, SDMMC controller** are claimed for the PP5022 (rows 33, 34, 25); our manuals have one PWM and an ADC name only.
7. **ATA-66** is the chip's stated IDE ceiling, so UDMA4 is the correct maximum and the 21 MB/s read limit is not a controller cap (row 23).
8. **USB OTG host** is a stated capability (row 24).
9. The PP5022 brief's **power claim ("300 % reduction vs PP5020")** is mostly the 1.8 V to 1.2 V core move, which Apple's PMU setting already uses.

## 5. Ranked shortlist: most promising unused features

| Rank | Feature | Why | First step |
|---|---|---|---|
| 1 | ~~**Hardware MAC** (row 11)~~ **Closed by desk study:** the cores' own multiplier, already used. Only the read-only `mmio scan` remains | – | `mac-and-dvs.md` §5.1 |
| 2 | **Core voltage scaling** (rows 5, 6) | The 1.2 V core is the single largest controllable power term and is fixed today (Apple did not scale it either). One step lower at 30 MHz is a small battery gain for the dominant playback state | `mac-and-dvs.md` §5.3, then §5.4 `dvs step` (owner's go); question #20 is half settled |
| 3 | **100 MHz clock** (row 2) | Faster boosted state for heavy codecs and the video feed; Apple's driver allows up to 125 MHz subject to a per-chip speed grade | `mac-and-dvs.md` §5.2 (grade), then §5.5 `pll100` (owner's go) |
| 4 | **Module clock gating audit** (row 6) | Probably free, small savings: switch off the FireWire link, second USB port, unused serial, PWM when idle | Dump `DEV_EN*`, clear bits one at a time in a test build |
| 5 | **IRAM cross-bar banking** (row 9) | Free throughput: the two cores stop colliding on IRAM if they use different banks; helps the COP-resident codec | Two-core read-loop test, different banks vs same bank |
| 6 | **Battery-backed RTC scratch registers** (row 39) | Would solve open question #33 (a drained latch readable before storage), a feature we already want | Read/write one word near `RTC`, survive a PMU standby |
| 7 | **Hardware PWM for the backlight/haptic** (row 33) | Removes the software-PWM timer wake-ups (audit item K1). Risky only if GPIOL7 cannot be muxed | Probe the PWM1-3 registers and the GPIO mux |
| 8 | **USB OTG host** (row 24) | Matches the mission (copy music to or from a USB stick with no computer) if the USB-C mod can supply VBUS | Read `OTGSC`/`PORTSC1`; check board VBUS source, mostly a hardware question |
| 9 | **COP clock skipping** (row 3) | Finer COP power steps without a PLL change; limited value if it is only a stall | `COP_CTL` skip loop timed against `USEC_TIMER` |
| 10 | **On-die ADC** (row 34) | Battery or temperature read without I2C, only if a pin is wired | Read the block after enabling `ADC_CLOCK_SRC` |

Not worth chasing on this board: S/P-DIF, SD/MMC, CompactFlash/NAND controllers, CCIR video in/out, JTAG, IR remote (no pins or parts), and all PP5024 analog blocks (the iPod uses external parts).

## 6. Surprises

- **The briefs are extremely thin.** No register map, no timing diagrams, no clock tree. There is no more detailed public PortalPlayer datasheet. Our `pp5022-reference/` is already more detailed than the public sources.
- The PP5022 brief lists "Hi-Speed USB 2.0 with **host** and device support" and "SDMMC (SDIO)", neither of which Apple used on the iPod Video.
- "**Integrated MAC**" appears only in the PP5022 brief and has never shown up in the Rockbox or iPodLinux register tables, because it is the cores' multiplier (`mac-and-dvs.md`).
- The 5024 brief says its integrated RTC has 128 battery-backed scratch bits, directly relevant to our open question #33.
