# 19. Product brief map: display, TV out, audio, peripherals and media

Broadcom's two-page product brief for the BCM2722 ([external] 2722-PB01-R, 2005-10-18) lists what the chip offers a handset maker. This chapter takes each claim about the display, TV out, audio, the serial and general-purpose interfaces, the camera and card interfaces, DRM and the media formats, and says what the iPod Video actually wires and what Apple's code actually does with it. For each it proposes how ProkBox could use it, and, where the answer needs the device, an experiment ready to build as a Developer › Advanced › Video chip › Probes stage.

The other half of the brief (bus bypass, the power modes, the clock manager, the caches, DMA and the host bus) is mapped in [`brief-map-power.md`](brief-map-power.md). The two chapters don't repeat each other.

**How the evidence was gathered (2026-10-23).**
- Static reading of Apple's `vmcs.bin` 1.3, `RenderServer.bin` 1.3 and the `.vll` libraries with `tools/bcmre/vc2dis.py`, plus their symbol tables and strings.
- Apple's own TV, clock and pin code **run in `vc2sim --boot`** against modelled peripherals, which logs every register access with its pc. Results from such runs are [apple] (Apple's code, executed), as in the rest of this manual; what the bits *mean* is tagged separately.
- `osos` 1.3 (Apple's ARM firmware) disassembled with `arm-none-eabi-objdump` to see which commands the host sends.
- `vmcs.bin` 1.2.1 has the same TV, video-DAC and USB register references at the same count (11), so the findings hold across versions [apple].
- Public pinout material for the dock connector and the headphone jack ([external], cited where used).

`@xxxx` is an address in `vmcs.bin` 1.3 (loaded at 0, so file offset = address; gp = `0x1FC40`). `host 0x1xxx_xxxx` is in `osos` 1.3. No Apple bytes, listings or text are reproduced here; register values are stated as facts about the hardware.


## Feature catalog (for `datasheet-features.md`)

Status: **used** (wired and driven by Apple's code), **known-unused** (on the chip or board but never used, or shown absent), **unknown** (needs a probe), **unverifiable** (no way to check on this board).

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

---

## 1. The claim table

Status words: **wired and used** (the iPod has it and Apple's code drives it), **present, unused** (the block is on the chip and reachable but Apple never uses it), **absent on the iPod** (not wired or no code), **software** (the brief's feature is code on the core, not a block), **open** (needs the device).

| # | Brief claim | Status on the iPod Video | Key evidence | Leverage for ProkBox | Experiment |
|---|---|---|---|---|---|
| 1 | PAL/NTSC analog TV output, composite or S-Video | **Wired and used.** A timing generator at `0x1000_2C00`, a video DAC block at `0x1000_5400`, a 27 MHz pixel clock, DMA channel 6 and GPIO 19. Apple's full on/off sequence for NTSC and PAL is recovered (§2) | [apple] @5BCE, @58FC, @7C9A, @13B08, run in vc2sim; [external] dock pins 8–10 and the headphone jack's fourth contact | **High.** TV out for videos (and later Photos) over a composite AV cable in the headphone jack. The owner's unit has no dock connector, so composite only | PB-1, PB-6, PB-7, PB-8, PB-9 |
| 2 | Macrovision support | **Used by Apple.** 23 parameter words at `0x1000_5484`–`0x1000_54DC`, one set per standard and level, and an all-zero "off" set | [apple] @13454 | None needed: we leave it off (write the off set). Not a circumvention: it's our own output | in PB-8 |
| 3 | Multiple displays up to XGA (main and sub LCD) | **Present, unused.** The LCD interface has timing words per chip select; Apple uses chip select 0 only. One panel is fitted | [apple] `lcd.md` §1; the TV parameter table treats the TV as display 2 | None (one panel) | — |
| 4 | Up to 24-bit internal precision; 8/9/16/18/21-bit video; RGB external data bus | **Partly used.** The iPod's panels use the 8080-style interface at 16 bits (8 bits on panel 0). The "RGB data bus" looks like the TV timing generator driving a parallel RGB panel: Apple's code picks a 9 MHz pixel clock when no TV standard is set | [apple] @5BCE, @13C70 (9 MHz option); [hypothesis] the shared generator | None on this board | — |
| 5 | Scalable image output with pixel-level interpolation | **Not in the LCD interface.** Apple writes nothing to the LCD block beyond the known registers. The TV generator takes a separate input size (640 wide) and output size (704 wide), so the scaling claim most likely belongs to the TV path | [apple] every `0x1000_40xx` access in `vmcs.bin`; `0x1000_2C08`/`0x2C0C` values | LCD: keep scaling on the vector unit. TV: possibly free horizontal scaling | PB-9 (#58, #62) |
| 6 | Configurable refresh rate | **Not a BCM setting on this board.** The MPU panels refresh from their own oscillator; Apple measures the period from the TE pin. Apple's LCD record carries 60 as a nominal rate | [apple] `lcd.md` "Tearing"; the panel record at `0x22E94` | Present on the TE edge (`graphics.md` §5) | #52 |
| 7 | I²S, PCM and AC'97 audio interfaces | **I²S out wired and used** for video soundtracks (the block at `0x1000_4800`, `audio.md`). No AC'97 or PCM-mode code exists | [apple] `audio.md`; [silicon] S32–S36c | Path A (the chip plays sound) once #35/#39 settle; this chapter traces Apple's two audio clocks to `0x1000_1C34`/`0x1C38` (§4.2) | dev45 8a; PB-1 reads the clock words |
| 8 | Audio in, multiple-format recording | **Absent on the iPod.** Apple's record calls are empty stubs; no input pins are muxed | [apple] `univ_audiorecord*` stubs, @13710 | None | — |
| 9 | 64-polyphony MIDI player and music synthesiser | **Software, not shipped.** No synth, MIDI or wavetable code in any Apple VideoCore image; `.mid` appears only in the media player's generic extension table | [apple] strings and symbol tables of all eight images | **Medium-low.** Our own wavetable synth on the chip for `.mid` files and Extras game music (§4.4) | none until #54 |
| 10 | High-quality graphics acceleration for 3D games | **Software.** `RenderServer.bin` is Apple's second boot image for the iPod Games: an OpenGL ES 2.0-style engine with a "shader machine", run on the scalar core and vector unit; no 3D block | [apple] RenderServer strings; `osos` exports ~80 `gl*` entry points and identifies the engine as an Apple OpenGL ES engine for the VC02 (§5) | Confirms our route (software renderer on the chip; `graphics.md`, `render3d.md`) | stage 3d |
| 11 | I²C master, SPI master | **Absent on the iPod** [hypothesis, strong]: no code, no pin mux, pins pulled down | [apple] boot pin map (§6.1) | None | PB-4 maps the pins |
| 12 | UART for Bluetooth, IrDA | **Absent on the iPod** [hypothesis, strong], as #11 | [apple] §6.1 | **Low** for a Bluetooth mod: the PP5022's dock UART is the practical route (§6.3) | PB-4 |
| 13 | USB 1.1 slave | **Present; one status bit read.** A block at `0x1000_6400`; Apple's `usb_inserted` reads bit 24. Whether its pins reach the connector is unknown; the iPod's USB is the PP5022's | [apple] @149C6 | None expected | PB-3 |
| 14 | 52 GPIO pins | **Seven have known jobs** (1–3 audio, 19 TV, 23 host interrupt, 48–51 panel). Apple pulls down 0, 1 and 4–47 at boot, the mark of unconnected inputs | [apple] boot pin map (§6.1); [silicon] S1, S32–S34 | Pin census | PB-2, PB-4 |
| 15 | JTAG and emulation, boundary scan | **Not reachable** [hypothesis]: dedicated balls under a 281-ball BGA; no test pads known | [external] package; no evidence on the board | None | — |
| 16 | Camera interface (two sensors, raw Bayer, CCIR 601/656, windowing) | **Absent on the iPod.** `camera_control` exists as a command and returns 0 without touching hardware | [apple] @13994 | None | — |
| 17 | SD, MMC, SDIO, Memory Stick PRO | **Absent on the iPod**: no code, no strings, no pin mux | [apple] §6.1, §7 | None | — |
| 18 | NAND/NOR flash including NAND boot; second external memory interface | **Absent as boot paths.** The BCM boots from the host (bootstrap over the host port); the "secondary memory interface" is what drives the panel. The NOR `vmcs` lives in the PP's flash, not the BCM's | [rockbox]; [apple] `host-interface.md` | None | — |
| 19 | DRM: unique chip ID, encrypted code with a programmable key, AES/3DES/RSA/CPRM | **Software AES only.** Apple decrypts protected video on the chip with a software AES in `passthruhandler.vll`. No chip-ID or key register is read anywhere; the only identity read is `sys19` (a revision or speed grade) | [apple] symbols; every peripheral constant in `vmcs.bin` (§8) | Note only. No work planned | — |
| 20 | VGA MPEG-4 encode/decode at 30 fps | **Decode only** in Apple's firmware (MPEG-4 SP and H.263 in `mpg4dec.vll`; H.264 Baseline in `h264dec.vll`). No encoder | [apple] symbols | Ours: H.264 decode built and on silicon; MPEG-4 SP planned (`mpeg4.md`) | — |
| 21 | 2-megapixel JPEG encode and decode | **Not used by Apple on the chip.** Photos reach the chip already decoded (RGB565 or YUV 4:2:2 caches) | [apple] slideshow symbols | Ours: JPEG decode spike (`jpeg-spike.md`), the album-art win | — |
| 22 | 3GP/MP4 streaming, MMS | **Container support is generic middleware.** The player's extension table names many formats; only AAC, H.264 and MPEG-4/H.263 decoders ship | [apple] mplayer strings | Ours demuxes on the PP | — |
| 23 | Raw Bayer camera data | **Absent** (no camera) | as #16 | None | — |
| 24 | Very low power: MPEG-4 encode from 10 mW, CIF at 38 mW | Encode figures don't apply. Decode power is unmeasured | [external] | `power.md`, #12 | HX-C1 |

---

## 2. TV out

### 2.1 What the board brings out

| Output | Where | Evidence |
|---|---|---|
| Composite | Dock connector pin 8; the headphone jack's fourth (sleeve-side) contact, used by Apple's composite AV cable | [external] dock pinout (pinouts.ru "iPod dock connector", The Apple Wiki "30-pin Connector"); jack order left, right, ground, video from a 5G owner's measurement (pinouts.ru audio/video-out reports) |
| S-Video (Y and C) | Dock pins 10 (luminance) and 9 (chrominance) | [external] as above |
| Component | Not supported on the 5G/5.5G | [external] forum reports; no component mode in Apple's code [apple] |

**The owner's unit has no dock connector** (USB-C mod; `pp5022-reference/index.md`). On that unit TV out can only be composite through the headphone jack, if the jack's video contact is still wired. A stock 5G/5.5G (the public-release configuration) also has the dock's composite and S-Video.

### 2.2 Apple's host commands

The TV is **display 2** in Apple's display server [apple: `osos` sends `display_control 2 dac=0`; the TV parameter table is separate from the LCD's]. `display_control` takes these parameters for the TV, each with its own handler (table at `0x220B4`):

| Parameter | Handler | Effect | Evidence |
|---|---|---|---|
| `power`, `dac` | @5AAE (shared) | Requests the output on (any non-zero value) or off; the change is applied by the update routine @5BCE | [apple] |
| `mode` | @5A32 | 0 = 525-line (NTSC-class): 704 × 480; 1 = 625-line (PAL-class): 704 × 576. Also loads the Macrovision set | [apple]; standard names [hypothesis] |
| `encoding` | @5B9E | 1–6. Values 1–4 select the 625-line mode, 5–6 the 525-line mode, and the value picks the Macrovision set. `osos` only ever sends **1** or **5** (host `0x10163BC8`, `0x101C772C`) | [apple]; which broadcast variant each value is [hypothesis] |
| `svideo` | @5B7C | Sets a flag that becomes bit 7 of `0x1000_5404`. `osos` never sends it, so Apple runs with the default; in simulation the default is on | [apple]; [hypothesis] meaning |
| `wide` | @5E38 | Picture aspect 16:9 (1) or 4:3 (0). Changes no register directly; it's used when composing the picture | [apple] |
| `width`, `height`, `backlight` | @5E62, @5A00, @58B8 | Stored for the compositor | [apple] |

Rockbox's names for the NOR `vmcs` (the small image Apple's boot ROM loads) include `TV_PALBMP` (2), `TV_NTSCBMP` (3) and `TV_MVOFF` (14), with a BMP at host `0xC020_0000` and the TV framebuffer at `0xC000_0000` [rockbox]. That's a second, independent TV path, in Apple's boot image (PB-6).

### 2.3 The hardware Apple programs

Recovered by running @5A32, @5AAE and @5BCE in `vc2sim` with the panel-agnostic `--bcm` machine [apple]. Field meanings are [hypothesis] unless stated.

**The sequence, NTSC (PAL in brackets):**

| Step | Register | Value | Meaning |
|---|---|---|---|
| 1 | `0x1000_5404` read | bit 0 must be 1 before the Macrovision set is written | DAC present or powered [hypothesis] |
| 2 | `0x1000_5484`–`0x1000_54DC` | 23 words: the Macrovision parameter set for the standard and level, or the all-zero off set (word 19 = 4) | Macrovision N0–N22 [hypothesis, strong: 23 words, and the values follow the well-known parameter layout] |
| 3 | `0x1000_1C30` | `0xA5A5_0501` | Pixel clock: the crystal (27 MHz) ÷ 1. Low bits are the divider: `…0502` gives 13.5 MHz, `…0503` 9 MHz [apple, clock-manager id 1 at @13C70] |
| 4 | `0x1000_2810` (GPIO set) | `0x0008_0000` | **GPIO 19 high.** Apple sets GPIO 19 as an output, low, at boot, and raises it only while the TV is on [apple] |
| 5 | `0x1000_5404` | `0xA0` (`0xE0`) | bit 5 on, bit 6 = 625-line, bit 7 = the S-Video flag |
| 6 | `0x1000_2C08` | `0x00F0_0280` (`0x0120_0280`) | Input: 240 (288) lines per field, 640 samples per line |
| 7 | `0x1000_2C0C` | `0x01E0_02C0` (`0x0240_02C0`) | Output: 480 (576) lines, 704 samples |
| 8 | `0x1000_2C10` | `0x09A2_183F` (`0x0A02_383F`) | `[31:20]` 154 (160) = 858 (864) − 704: the samples per line less the active ones, at 13.5 MHz. `[19:10]` 134 (142). `[9:0]` 63: a 64-sample sync, 4.74 µs, the standard horizontal sync width |
| 9 | `0x1000_2C14` | `0x0170_9406` (`0x0180_B005`) | `[31:20]` 23 (24) = (525 − 480) ÷ 2 rounded: blank lines before the picture. `[19:10]` 37 (44). `[9:0]` 6 (5): the number of equalising pulses (6 for 525-line, 5 for 625-line) |
| 10 | `0x1000_2C18` | `0x41` (`0x43`) | bit 1 = 625-line; bit 0 and bit 6 [unknown]. Read back, **bit 31 is the field**: @59E4 tests it to tell odd from even fields |
| 11 | `0x1000_2C00` | `0x0003_3125` (both) | Control: bit 0 enable [hypothesis]. On a first start Apple waits for bit 31, then bit 30, then any of bits 31/29/28, and writes `0x7777_7777` to the FIFO once |

The timing words come straight from ITU-R BT.601 numbers (858/864 samples at 13.5 MHz, 525/625 lines), computed by @5BCE from the active size; that's strong support for the field readings [hypothesis, strong].

**Off** (`dac=0`, then @5BCE): `0x1000_1C30 = 0xA5A5_0000` (clock off), GPIO 19 low (`0x1000_2818 = 0x0008_0000`), `0x1000_5404 = 0x80` (`0xC0`): bit 5 cleared, then `0x1000_2C00 = 0`. The teardown at @13B08 also writes 0 to `0x1000_2C00` and `0x1000_5404` and clears DMA channel 6's status [apple].

**Around it** [apple, meaning hypothesis]: @58FC makes two requests through a helper at @10056 with 150,000,000 and 35,000,000 while the TV is on, and withdraws them at off. These are most likely the power manager's floors: the core at 150 MHz and another clock at 35 MHz. Apple's 32 µs TV line poller runs on timer compare 3 (`interrupts-timers-gpio.md` §3).

**Feeding pixels** [apple @7C9A]: DMA channel 6 (config `0x0200_FF01`) sends a chain of control blocks to the FIFO at `0x1000_2C04`, transfer information `0x1900_A001` plus the source width, which is the LCD channel's word. The chain rotates through four small "stripe" buffers that the display server fills a band at a time ("Image", "MPU" and "TV" stripes are named in the image). So the frame is never held whole for the TV: the chip composes bands just ahead of the scan. The FIFO's pixel format is **unknown**; Apple's display server composes in YUV 4:2:2 and RGB565 [apple: image types], and its idle word `0x7777_7777` is a hint, not an answer (PB-8).

The same driver is in `RenderServer.bin` [apple: identical register set], so iPod Games could show on a TV too.

### 2.4 Feasibility design for ProkBox TV out

**Verdict:** feasible, with every register Apple writes known. Three unknowns remain, and each has a stage: the FIFO's pixel format (PB-8), whether the 640 → 704 step is a scaler or a border (PB-9), and whether the owner's jack still carries video (PB-8, by eye).

**What it would look like to the user** (7G idiom; `docs/design/5g-to-7g-delta.md` row 49, `research/bcm2722/shell-contract.md`): Videos › Settings › **TV Out** (Ask / On / Off), **TV Signal** (NTSC / PAL), **Widescreen**. Today they're hidden (`design/changes-from-7g.md` 2.31). They'd appear once PB-8 passes on the owner's unit, never before.

**Chip side** [ours, proposed]:
- A **TV session** in the video image, beside the LCD path: `HP_OPEN` with a TV flag.
- The decoder already produces YUV 4:2:0 pictures. A new vector kernel writes interlaced bands at 640 (or 704) × 240/288 per field into SRAM stripes, using the existing YUV → output conversion or none if the FIFO takes YUV.
- DMA channel 6 chains the stripes to the FIFO, as Apple does; the field bit at `0x1000_2C18` (bit 31) paces the producer, so no 32 µs interrupt is needed (interrupts are still unproven on silicon, #3).
- **Budget** [estimate]: a 320 × 240 source upscaled 2× in both directions is 640 × 480 = 307 k pixels a frame, 9.2 M pixels a second; at one vector op per 16 pixels and about 4 ops a pixel group, about 2.3 M cycles a second for the conversion: under 2 % of 150 MHz. The decode itself is unchanged (7–18 ms a frame, S27). TV out needs the 150 MHz clock throughout (Apple asks for it), so it's a Class C state like video.
- **The LCD during TV out:** Apple blanks the panel and shows a "TV out" screen. We'd do the same: one static frame, panel then off, which saves the LCD DMA.
- **Memory:** the stripe ring is 4 × 1,280 bytes (two lines at 640 × 2 bytes). Nothing else new.

**Host side:** the PP sends the same `FEED` stream; only the session's output changes. Audio stays on the PP (or path A when proven). The settings are three bytes in the shell's config.

**Effort** [estimate]: after PB-6–PB-9 pass, 3–5 days for the chip side and the session, 1 day for the shell settings, plus a device session.

**Risks:**
- The headphone jack's video contact may be gone on the owner's modded unit; then TV out is for stock units only (and the release).
- Apple shipped composite on the jack with headphones in as well (the setting is "On"), so a short to ground on that contact is an expected load [hypothesis: video DACs are current-mode and tolerate it]. Our first stages still require the AV cable, not headphones.
- No interlace flicker filter: UI text on a TV may flicker. A 1-2-1 vertical filter in the band kernel costs one more vector op a pixel group.

---

## 3. The LCD controller

**Apple's complete write set** [apple: every `0x1000_40xx` access in `vmcs.bin` 1.3, statically]:
- the control word at `+0x00` (`|= 1` at power-on; `0x231`/`0x229` with bit 8 for a run; `3` written back by the run-done handler @14780 after it sees bit 1; 0 at shutdown @6392);
- `+0x04` run length: the byte count halved when the chip select's timing word 1 has bit 30 (the 16-bit bus) set (@14660);
- `+0x08` run format: copied from the panel record (`0x1F` for every panel; record at `0x22E94`);
- `+0x0C` the FIFO (DMA destination only);
- the timing words at `+0x10`/`+0x14` for chip select 0;
- index and data at `+0x178`/`+0x17C`, which Apple reaches as `0x1000_4140 + 0x38`/`+0x3C`.

Nothing else. **There is no scaler, colour-depth or refresh control written by Apple in the LCD block**, so open question #58 is answered for the LCD: if a scaler exists there, Apple never used it, and we'd have to find it blind [apple]. The scaling claim most likely belongs to the TV/RGB timing generator at `0x1000_2C00`, which has separate input and output sizes (§2.3; #62).

**What the other display claims mean here:**
- *Multiple displays up to XGA:* the per-chip-select timing words allow several MPU panels; the iPod has one [apple]. XGA would be the RGB/TV generator's limit, not the MPU path's.
- *8/9/16/18/21-bit video, 24-bit internal precision:* bus widths of the MPU and RGB interfaces. Ours is 16-bit RGB565 (panel 0 takes bytes). `+0x08 = 0x1F` is the only format word ever written; its bits may select packing [hypothesis].
- *Configurable refresh rate:* a property of the RGB generator (pixel clock 9 MHz in Apple's non-TV case). MPU panels refresh themselves; the record's 60 is nominal.

**Leverage:** none new for the panel. Keep the vector-unit scaler (`pipeline.md`). The TE-synced present (`graphics.md` §5) is the real refresh lever.

---

## 4. Audio

### 4.1 What's wired
I²S output only (`audio.md`): GPIO 1 data, GPIO 2/3 the WM8758's clocks [silicon S32–S34]. No AC'97 or PCM-mode code, no input path: `univ_audiopresent` returns 1, every record call is an empty stub, and nothing reads an audio FIFO [apple]. "Audio in" and "multiple-format recording" are not on this board.

### 4.2 Apple's two audio clocks reach `0x1000_1C34` and `0x1000_1C38`
Apple's clock manager maps its clock ids to block-clock registers through a 10-entry switch at @5334 [apple]:

| Id | Register | Apple's user |
|---|---|---|
| 0 | `0x1000_1C2C` | — |
| 1 | `0x1000_1C30` | TV pixel clock (27 or 9 MHz) |
| 2 | `0x1000_1C34` | audio, 24.576 MHz (`audio.md` §2) |
| 3 | `0x1000_1C38` | audio, 2.048 MHz |
| 4 | `0x1000_1C40` | — |
| 5 | `0x1000_1C44` | — |
| 6 | `0x1000_1C3C` | USB status read toggles its bit 15 (§6.2) |
| 7 | `0x1000_1C48` and `0x1000_1C50` | — |
| 8 | `0x1000_1C28` | — |
| 9 | `0x1000_1C58` | — |

All are password writes (`0xA5A5` in `[31:16]`). For a clock from the crystal the low byte is the divider and `0x5` in `[11:8]` the source [apple: 27 MHz → `0x501`, 13.5 → `0x502`, 9 → `0x503`; the 1 MHz counter's `0x51B` = ÷27 agrees]. Apple's audio requests use mode 3, which in our simulation wrote nothing: they likely need a PLL output that the simulated boot hadn't started [sim]. This half-answers #36 (§9): the block's clocks are `0x1000_1C34`/`0x1000_1C38`. Since the block runs as a clock slave on the WM8758's clocks, they're probably its internal FIFO-side clocks [hypothesis]. PB-1 reads both words.

### 4.3 MIDI and the synthesiser
**No synth exists in Apple's VideoCore code** [apple]. A search of every string and symbol in `vmcs.bin`, `RenderServer.bin` and the six `.vll` files finds no MIDI, wavetable, voice or synthesis names. `.mid` appears once, in `mplayer.vll`'s table of file extensions, next to formats the iPod never played (`.amr`, `.qcp`, `.evrc`, `.wma`): Broadcom's generic middleware table, with no handler behind it.

### 4.4 Design sketch: our own synth on the chip [ours, proposed]

**Purpose:** `.mid` files with a General MIDI set (Rockbox's MIDI codec runs on the PP and is costly at our clocks), and music and effects for Extras games (P12 classes B and C) without waking the PP's codec path.

**Voices:** 32 to 64. Each voice holds a sample pointer, a 16.16 phase and step, a loop start and end, an ADSR envelope stepped once per 32-sample block, and left/right gains.

**Rendering, in 32-sample blocks:**
- *Scalar version (works today):* per voice and sample, a phase add, two sample loads, a linear interpolation, the envelope and gain multiply, and an accumulate. About 10 cycles [estimate], so 64 voices × 44,100 × 10 ≈ 28 M cycles a second: about 19 % of 150 MHz. 32 voices at 22,050 Hz is about 5 % at 150 MHz, or 26 % at 27 MHz.
- *Vector version:* the mix, envelope and gain stages map to 16 lanes directly (one lane per output sample, accumulators in the VRF). The sample fetch is a gather, which the vector unit doesn't have (#54). With a voice's pitch ratio at most 2, its 32 outputs need at most 65 consecutive source samples: load them as rows, then pick per lane. Whether a per-lane select is cheap is #54's question.

**Memory** [estimate]: a compact GM set (8-bit or ADPCM, 1–2 MB) in SDRAM, uploaded once by engine 2 (~0.2 s); voice state of 64 × 64 bytes in SRAM; output ring 2 × 4 KB.

**Output paths:**
- **Path A**, the chip's own I²S (`audio.md` §6), once #35/#39 are settled: the PP can sleep while it plays.
- **Path B**, a PCM ring in SDRAM read back by the PP into Rockbox's mixer (stage 8b's plumbing): about 2–5 % of the PP at 30 MHz.

**Effort** [estimate]: 4–6 days for a scalar synth with a small instrument set; a SoundFont-subset converter at sync time (no parsing on the device, rule 3).

**Value to the owner:** medium-low. It's after TV out, JPEG decode and path A.

---

## 5. Graphics: what `RenderServer.bin` is

- **A second boot image** of 104,540 bytes, loaded instead of `vmcs.bin` [apple: `osos` names both files side by side].
- It carries the same RTOS, DMA, LCD and TV drivers, and its display-control strings include a TV power-on with S-Video [apple].
- **What it adds** [apple, strings and symbols]:
  - a vector-aligned allocator family (`gldCalloc`, `gldVecMalloc`, `gldMallocSlow`);
  - a "ShaderMachine" with bound shaders, uniforms and shader types;
  - texture uploads with a sub-data header and a four-character `TEXF` tag;
  - a front buffer named `FrontBufferA`.
- **The host side** in `osos` [apple]:
  - about 80 `gl*` entry points of an OpenGL ES 2.0 shape: buffers, textures (2-D and 3-D), shader sources, uniforms, vertex attributes, `glDrawArrays`/`glDrawElements`, stencil and scissor;
  - Apple fixed-point extensions: `…xAPPLE`/`…xvAPPLE` uniform and attribute calls, `glFinishObjectAPPLE`;
  - version strings for an OpenGL ES 2.0 engine for the "VCO2", with GLSL 1.10;
  - the games' own names: "eApp", `GamesPlatformID`, `RenderWidth`/`RenderHeight`.

**Reading** [hypothesis, strong]: Apple's iPod Games (2006) ran on a shader-style OpenGL ES engine. The PP made the GL calls; the BCM's RenderServer ran a software rasteriser whose per-pixel work was a small interpreted or compiled shader program. There's no fixed-function block.

**Compared with ours** (`graphics.md`, `render3d.md`):

| | Apple's RenderServer | Our r3d |
|---|---|---|
| API | GL ES 2.0-style calls over the host link | Our own scene and command list |
| Pixel work | "Shader machine": programmable [apple names; the inner loop not read] | Fixed affine palette texturing, 14 instructions a pixel [sim] |
| Depth | Not known (stencil calls exist) | Painter's ordering table, no depth buffer |
| Status | Shipped on 5G hardware | Simulator only (stage 3d pending) |

**What it tells us:** the chip carried commercial 3D games at this panel size, so the rate figures in `graphics.md` §3 are plausible. A GL-shaped API is not something we need: the SDK's command list is smaller. If stage 3d's figures come out short, reading RenderServer's rasteriser loop is the next reference (its vector share, 907 vector lines in a 32 k-line sweep, is about half of `vmcs.bin`'s) [apple].

---

## 6. Serial interfaces and GPIO

### 6.1 Apple's boot pin map
Recovered by running `vmcs.bin` 1.3's start-up in `vc2sim --boot` and replaying every write to `0x1000_2800`–`0x1000_286C` [apple]:

| Pins | Function | Pull | Driven | Job |
|---|---|---|---|---|
| 0 | input | down | — | unknown; treated as unused |
| 1 | **output** | down | low | audio data (function 2 during video) |
| 2, 3 | input | **none** | — | the WM8758's bit and frame clocks [silicon S32] |
| 4–18 | input | down | — | treated as unused |
| 19 | **output** | down | low; **high while the TV is on** | TV-out enable [hypothesis: an external video buffer or switch] |
| 20–22 | input | down | — | treated as unused |
| 23 | **output** | down | pulsed | interrupt to the host |
| 24–47 | input | down | — | treated as unused |
| 48 | **output** | **up** | high | panel reset |
| 49 | input | none | — | panel TE |
| 50, 51 | input | none | — | panel ID straps |

No pin is ever put on function 2 or 3 except GPIO 1–3 for audio [apple: the only callers of the function-select helper @13E66 with 2 are in `audio_enable`]. **So Apple's code never uses an I²C, SPI, UART, SD/MMC, Memory Stick or camera function on any GPIO.** A pin that's pulled down and never read is an input nobody drives: that's the usual way to park an unconnected pin. The pins without a pull are exactly the ones something outside drives [apple; reading: hypothesis, strong].

### 6.2 USB 1.1
A block at `0x1000_6400`. The `usb_inserted` command (@C610 → @149C6) reads its word 0 bit 24 and replies with it. Around the read it clears `0x1000_1C3C` bit 15 if set and sets it again afterwards (password writes) [apple]. Nothing else touches the block. The iPod Video's USB connector is wired to the PP5022's controller [rockbox]; whether the BCM's USB pins are bonded to anything is unknown. PB-3 reads the bit with USB-C in and out.

### 6.3 What this means for a Bluetooth-modded iPod
Bluetooth mods for these iPods take analog audio from the headphone or line output, and, when they carry remote control, use the dock connector's serial lines (the accessory protocol). Both are on the PP5022's side (`pp5022-reference/gpio-i2c.md`, the dock UART routing). The BCM's UART claim doesn't help:
- its pins aren't known;
- they're under a BGA;
- Apple never muxed them;
- the BCM is powered down whenever the screen is dark.

**Use the PP's UART** for any Bluetooth accessory work [hypothesis, strong]. PB-4 still maps which BCM pins are connected, for completeness.

### 6.4 JTAG and boundary scan
Dedicated balls on the 281-ball package [external]. No test pads for them are known on the iPod Video board, and no code refers to them. Not usable without board-level rework [hypothesis].

---

## 7. Camera, card and NAND interfaces

All absent on the iPod Video, with three kinds of evidence:
- **Code:** `camera_control` is a registered command whose handler (@13994) returns 0 and touches nothing; `led_control` and `motor_control` are empty too (@141A8, @141AA) [apple]. No SD/MMC/SDIO/Memory Stick strings, symbols or register blocks exist in any image. The only peripheral blocks Apple's code addresses are run control, DMA, cache, interrupts, regulator, timer, clocks, GPIO, TV, SDRAM, host interface, LCD, audio, `0x1000_4C00`, video DAC and USB (`memory-map.md`).
- **Pins:** no alternate function is selected on any GPIO but 1–3 (§6.1).
- **Boot:** the BCM is started by the host's bootstrap and upload every time (`boot-and-clocks.md`); there's no NAND on its side. The brief's "second external memory interface" is the bus that drives the panel's chip select.

The `mplayer.vll` strings for recording, stills and `.amr`/`.jpg` capture are Broadcom middleware that Apple shipped unused [apple].

---

## 8. DRM

Note only; no work is planned.
- **Chip ID:** no code in any image reads an ID, OTP or key register. Every peripheral constant in `vmcs.bin` 1.3 falls in the blocks listed in §7 [apple]. The one identity-like read is the `sys19` instruction, used to pick the regulator level and TV timing variants (`memory-map.md`, "A special register") [apple]; its meaning is open.
- **Encrypted code:** Apple's images are plain ELF and raw code; nothing is decrypted at load [apple].
- **Crypto:** `passthruhandler.vll` decrypts protected media on the chip with a **software AES** (`aes_init`, `aes_decipher`, S-boxes, `vcaes_decipher`, a per-stream crypto record), with the key supplied by the host [apple]. No 3DES, RSA or CPRM code exists. The brief's "DRM-ready" means "a programmable core you can run crypto on".

---

## 9. Media claims against Apple's use and ours

| Brief claim | Apple on the iPod | ProkBox today |
|---|---|---|
| VGA MPEG-4 decode at 30 fps | `mpg4dec.vll`: MPEG-4 Part 2 and H.263 (short header), decode only, with an enhancement-frame entry point [apple] | Plan only (`research/bcm2722/mpeg4.md`) |
| (H.264, not in the brief) | `h264dec.vll`, Baseline [apple] | Built; bit-exact; 7–18 ms a frame on silicon (S27) (`decoder.md`, `pipeline.md`) |
| MPEG-4 encode, recording | Absent: no encoder; record calls stubbed [apple] | — |
| 2 MP JPEG encode/decode | Not on the chip: photos arrive pre-decoded as RGB565 or YUV 4:2:2 caches (`photo_cache_*`), with transitions (cube, page turn, dissolve…) done on the chip [apple] | JPEG **decode** spike: bit-exact in vc2sim, S3–S5 next (`jpeg-spike.md`): the album-art builder's win |
| 3GP/MP4 streaming, MMS | The chip's player demuxes from the host's file system; its extension table is generic [apple] | The PP demuxes (`pipeline.md`) |
| Multiple-format audio playback | AAC (Ittiam) only [apple] | AAC-LC port (libfaad) built; on-chip failures under study (#46) |
| Raw Bayer | Absent | — |

---

## 10. Leverage, ranked by value to the owner

1. **TV out** (high). The register sequence is complete; three unknowns have cheap stages. It brings back a 5G feature Rockbox never had, in the 7G's settings idiom, and the release's stock units get S-Video and dock composite for free. Gate: PB-7, then PB-8 with the owner's go.
2. **Finish the chip's own audio output** (high for battery, already in flight). The clock mapping in §4.2 removes one unknown from #36; with path A proven, the video soundtrack (and later a synth) plays while the PP sleeps. Gate: dev45 8a, PB-1's clock reads.
3. **A wavetable synth** for `.mid` files and Extras game music (medium-low), once #54's fetch cost is measured.

Also confirmed, saving effort: **no LCD scaler to chase** (#58), and **no BCM serial interface for a Bluetooth mod**: that work belongs on the PP.

---

## 11. Device experiments

Each stage runs from **Developer › Advanced › Video chip › Probes** as a numbered stage of the BCM device test (`bcmfw/mkdevtest.py`, the host's `bcm-devtest`), logs to `bcmtest.log` under the prefix given, and follows `hw-experiments.md` §1's safety classes (R0 read-only, R1 reversible, R2 pin/clock/power changes, R3 brick risk) and its rule that "nothing happened" can't pass.

**Common to every stage:**
- **Canary first:** read the 1 MHz counter (`0x1000_1804`) twice, 100 µs apart; it must advance by 90–110. Read the panel ID (`0x1000_2824` bits 19:18); it must equal the ID the host detected at boot. A failed canary voids the run.
- **Every register read twice;** two different values are logged as `volatile`, not as a value.
- **Never written by any stage:** the FIFO addresses (`0x1000_2C04`, `0x1000_400C`, `0x1000_4804`) except where a step says so; the regulator (`0x1000_1400`); PLL registers (`0x1000_1C00`–`0x1000_1C24`); the SDRAM controller; GPIO 2 and 3's function (never output); GPIO 48–51; GPIO 23; any GPIO fsel word other than the one bit pair a step names.
- **Recovery:** every stage ends with a BCM power cut (`GPO32` bit 14 clear for ≥ 50 ms) and the normal cold boot of the image. A hang is a result: the host's 2 s watchdog (`BCMFW_HANG_TIMEOUT`) cuts power and logs `<prefix>: hang at step N`.
- **New host-protocol ops** [ours, proposed]:
  - `HP_OP_PEEK` (`0x13`): up to 15 addresses (two reads each must fit the 32-word reply) and a gap between the two reads. The image refuses any address outside a compiled allowlist with `HP_E_INVAL` and reads nothing. **Built** (`bcmfw/hostproto.c` `peek_allow`).
  - `HP_OP_TV` (`0x14`): runs one named TV step from a fixed table; it has no free register write. **Built image side, not run** (PB-7 to PB-9 below): off by default, a one-time code from `HP_TV_UNLOCK` must be echoed, each step checks every write against its own allowlist, and every exit restores what it wrote. Request and reply in `hostproto.h`.

Ranked safest first. Wave 1 (PB-1 to PB-3) is read-only.

**Built** (branch `p19-pb-probes`, not yet run on silicon): PB-1 to PB-4 as Developer › Advanced › Video chip › Probes rows, and PB-5 in stage 8a-1. Rows, prefixes, owner steps and the deviations below are in `../research/bcm2722/bench-p19.md`, "The brief probes"; vc2sim checks in `bcmfw/tests/peek_test.py`. PB-6 is not built. PB-7 to PB-9 are built image side (branch `p19-probes2`, `HP_OP_TV` in `bcmfw/hostproto.c`, vc2sim checks in `peek_test.py --only tv`) and PP side (branch `p19-tv-rows`: three Developer › Advanced › Video chip › Probes rows, PB-8 locked until PB-7 passes and PB-9 until a PB-8 arm shows bars; `apps/ipodui/devbcm_probes.c`, logic in `devbcm_tv.h`, host tests in `tests/tvprobe/`; cards in `bench-p19.md`). Nothing has run on silicon.

### PB-1 · Census of the brief's blocks · R0 · no go needed · prefix `pb1:` · **built**
**Question:** the reset and idle values of the TV, video-DAC, USB, clock and audio registers (#60, #61, #64, #65; extends HX-A18).

**BCM side:** `HP_OP_PEEK` with the allowlist below, at the boot clock (27 MHz), then again after `OPEN` has switched to 150 MHz (dev45 image).
- *Pass 1a, registers Apple reads:* `0x1000_2C18`, `0x1000_5404`, `0x1000_6400`, `0x1000_1C3C`, `0x1000_4800`, `0x1000_2820`, `0x1000_2824`.
- *Pass 1b, registers Apple only writes,* read back, only if 1a didn't hang: `0x1000_2C00`, `0x1000_2C08`–`0x1000_2C14`, `0x1000_5484`–`0x1000_54DC` (23 words), `0x1000_1C28`–`0x1000_1C58` (13 words), `0x1000_4810`, `0x1000_4814`, `0x1000_2800`–`0x1000_280C`, `0x1000_2860`.

**PP side:** send 1a, log, send 1b, log; power cut; cold boot.

**Log:** `pb1: <clock> <addr> <v1> <v2|volatile>`, one line each, then `pb1: done 1a=<n> 1b=<n>`.

**Pass:** every allowlisted word logged twice with the canaries good. *Informative signatures:* `0x1000_5404` bit 0 (DAC present); `0x1000_2C18` bit 31 static (no TV running) — if it toggles between reads with the TV off, the timing generator runs from reset. `0x1000_1C30` = 0 (TV clock off). The Macrovision words' reset state (#61). `0x1000_6400` bit 24.

**Fail:** any hang (logged with the address in flight); a canary failure.

**Safety:** reads only, of blocks Apple's code reads or writes. **vc2sim pre-check:** the stage runs against `vc2mmio`, which logs the access list; the list must equal the allowlist, in order, with no write.

### PB-2 · GPIO level census · R0 · owner: on cue · prefix `pb2:` · **built** (backlight-off state skipped); **run 2026-10-25: 0 wired pins**
**Question:** which BCM pins move with a board event (#63).

**BCM side:** `HP_OP_PINS` extended to pins 32–51 (a second mask word) with flags 0 (no fsel change, no pull). Sample `0x1000_2820`/`0x1000_2824` for 500 ms; reply with the AND and OR of the levels and the edge count per pin (all 52, not only the first four).

**PP side:** take one snapshot in each state, the owner advancing with SELECT: idle; music playing; USB-C cable in; USB-C out; AV cable in the jack; headphones in; hold on; backlight off with the BCM kept up (if the build allows; else skip).

**Log:** `pb2: <state> and=<w0>,<w1> or=<w0>,<w1> edges=<pin>:<n>,…` (non-zero pins only).

**Pass:** the known pins read as expected: 2/3 edges only while music plays, 49 toggling (TE), 50/51 equal to the panel ID, 48 high. Any other pin whose level or edges change with exactly one stimulus is a wired pin, named by that stimulus.

**Fail:** the known pins don't read as expected (the census is invalid).

**Safety:** reads only; nothing driven. **vc2sim:** the extended `HP_OP_PINS` decode (52 pins, two masks) is unit-tested against `vc2mmio` with scripted levels.

**On silicon, 2026-10-25 (build A, session log `bcmtest.log`)** [silicon]: seven states taken (idle, music, USB-C in, USB-C out, AV cable, headphones, hold); the card ended "FAIL, 7/7 states" with **0 wired pins found**. The log line says the known pins read off, so the "known pins as expected" pass rule was not met and the census is not valid by its own rule. Treat "no pin moved" as weak evidence until a census with the known pins reading right is repeated (open question #63).

### PB-3 · USB block status · R0, then R1 · no go needed · prefix `pb3:` · **step 1 built** (step 2, R1, not built); **step 1 run 2026-10-25: no change**
**Question:** does the BCM's USB block see the USB-C connection (#64)?

**Step 1 (R0):** read `0x1000_6400` and `0x1000_1C3C` twice, with USB-C in, then out (the owner on cue).

**Step 2 (R1), only if step 1 shows no difference:** Apple's sequence:
1. Read `0x1000_1C3C`.
2. If bit 15 is set, write `(value & ~0x8000) | 0xA5A5_0000`.
3. Read `0x1000_6400`.
4. If bit 15 had been set, write `value | 0xA5A5_8000`.
5. Read `0x1000_1C3C` back; it must equal the value read in 1.

**Log:** `pb3: usb=<in|out> step=<1|2> 1c3c=<v> 6400=<v>`.

**Pass:** bit 24 follows the cable: the BCM's USB is bonded to the connector's sense line. Unchanged in both steps: not wired (the expected result).

**Safety:** step 2 restores the clock word and is read back. Power cut after. **vc2sim:** step 2's write list against Apple's @149C6 run in `vc2sim`: equal.

**On silicon, 2026-10-25 (build A, session log `bcmtest.log`)** [silicon]: step 1 read `0x1000_6400` = `00000000` with USB-C in and again out; bit 24 unchanged. By the pass rule, the BCM's USB block is not bonded to the connector (open question #64). Step 2 (R1) was not run.

### PB-4 · GPIO pull test · R1 · no go needed · prefix `pb4:` · **built** (50/51 read unpulled as the control: they are on the never-touch list)
**Question:** which of the pins Apple pulls down are connected to anything (#63; HX-A13's BCM half).

**BCM side:** for each pin p in {0, 4–18, 20–22, 24–47} (never 1, 2, 3, 19, 23, 48–51), with p already an input (Apple's and our boot leave it so; the stage checks its fsel field is 0 and skips it otherwise):
1. Pull up: `0x1000_2860 = 2`, wait 150 cycles, `0x1000_2864/68 = 1 << p`, wait 150, write 0 to the strobe.
2. Wait 1 ms; read the level twice.
3. Pull down (mode 1) the same way; wait 1 ms; read twice.
4. Restore pull down, Apple's boot state.

**PP side:** one request per 8 pins (bounded at 50 ms each); log; at the end a power cut.

**Log:** `pb4: gpio<p> up=<l,l> down=<l,l> -> float|high|low|volatile`.

**Pass:** classification per pin: *float* (follows both pulls: unconnected or tri-stated), *high*/*low* (holds against a pull: driven or strapped), *volatile* (a signal). The check pins in the control arm, GPIO 50/51 (straps, no pull in Apple's map), must read *high/low*, not *float*.

**Safety:** pulls are weak (tens of kΩ) and Apple applies the same pull-down to these pins at every boot; the up-pull is the only new state, held 1 ms per pin. No pin is driven. **vc2sim:** the write sequence against Apple's pull helper @13F7A: the same register order and waits.

### PB-5 · *Reserved* (audio clocks) · **built** in 8a-1
Folded into dev45 stage 8a-1: add reads of `0x1000_1C34` and `0x1000_1C38` to its reply, before the rate, while playing and after close (prefix `8a1:`). No new writes.

### PB-6 · Apple's NOR `vmcs` TV pattern · R2 · **owner's go**, AV cable and a TV · prefix `pb6:` · **PP row built, not run**
**Question:** does the owner's unit still carry composite video on the headphone jack, using only Apple's code?

**Host side only** (the NOR `vmcs`, which Rockbox loads at every BCM power-up, no image of ours):
1. Build a 320 × 240 24-bit uncompressed BMP of colour bars in the PP's RAM; write it to host `0xC020_0000` through the port.
2. Write `BCMCMD_TV_NTSCBMP` (or `_PALBMP`, per the owner's TV) to `0x1F8`; poll for completion (≤ 2 s).
3. Write `BCMCMD_TV_MVOFF`.
4. Hold 10 s; the owner reports what the TV shows.
5. Power cut; cold boot.

**Log:** `pb6: std=<ntsc|pal> cmd=<ack|timeout> owner=<bars|grey|nothing|noise>`.

**Pass:** bars on the TV: the jack carries video, and TV out is worth building for this unit. Nothing on the TV is ambiguous (BMP format or the jack); go to PB-8.

**Safety:** only Apple's code drives the pins, exactly as on a stock iPod; the BMP format is a guess, and a wrong one shows nothing. AV cable in, **no headphones**. **vc2sim:** none (the NOR image isn't in our corpus).

*As built* (branch `p19-pb6`; card PB-6 in `research/bcm2722/bench-p19.md`): the row **BCM probe PB-6: TV with Apple's own pattern**, not locked, before PB-7. If our image drives the display it steps down to Apple's `vmcs` first (the device test's fallback, for the rest of that boot). The BMP is a 14-byte file header and the 40-byte `BITMAPINFOHEADER`, bottom-up, 24-bit BGR, eight 75 % bars ([hypothesis]: the format Apple's command expects is unknown). The owner answers on the lit screen after the power cut and the cold boot, as in PB-8, not during the hold.

### PB-7 · Timing generator without output · R2 · **owner's go** · prefix `pb7:` · **image side and PP rows built, not run**
**Question:** does our image start the TV timing generator, with nothing reaching the jack (#60)?

**BCM side** (`HP_OP_TV`, step "timing"), all values from §2.3; DAC and GPIO 19 untouched:
1. Read `0x1000_5404`, `0x1000_2C18`.
2. Write `0x1000_1C30 = 0xA5A5_0501`.
3. Write `0x1000_2C08 = 0x00F0_0280`, `0x1000_2C0C = 0x01E0_02C0`, `0x1000_2C10 = 0x09A2_183F`, `0x1000_2C14 = 0x0170_9406`, `0x1000_2C18 = 0x41`.
4. Write `0x1000_2C00 = 0x0003_3125`.
5. For 1 s, count changes of `0x1000_2C18` bit 31 against the 1 MHz counter.
6. Write `0x1000_2C00 = 0`, then `0x1000_1C30 = 0xA5A5_0000`.

Repeat with the PAL words.

**PP side:** send, wait ≤ 1.5 s, log, power cut. *As built:* 500 ms a standard, one boot; the PP waits the window + 2.02 s and calls a hang only when neither `HP_TV_PROGRESS` nor the heartbeat moves for 2 s.

**Log:** `pb7: std=<ntsc|pal> 5404=<v> ctl=<v during> fields=<n>/s`.

**Pass:** 59–61 field changes a second for NTSC and 49–51 for PAL (or half, if bit 31 is a frame bit): the generator runs at BT.601 timing from our image. **Fail:** 0 changes, with the control word logged.

**Safety:** the DAC's enable (bit 5 of `0x1000_5404`) stays clear and GPIO 19 stays low, so no analog output is enabled [hypothesis]. One clock register written and then cleared. **vc2sim:** the image's access log must equal Apple's @5BCE run (§2.3) minus the DAC and GPIO writes; `vc2mmio` gets a rule that toggles bit 31.

### PB-8 · Composite output and the FIFO format · R2 · **owner's go**, AV cable and a TV · prefix `pb8:` · **image side and PP rows built, not run**
**Question:** the pixel format of the TV FIFO, and a picture from our own image (#60, #61).

**BCM side** (`HP_OP_TV`, step "output", one format arm per run):
1. PB-7 steps 1–3.
2. Macrovision off: if `0x1000_5404` bit 0 = 1, write the off set to `0x1000_5484`–`0x1000_54DC`: 23 words, all 0 except word 19 = 4.
3. GPIO 19: fsel field (pin 19 in `0x1000_2804` bits 7:6) to 1 (output, as Apple's boot) if not already; write `0x1000_2810 = 0x0008_0000`.
4. Write `0x1000_5404 = 0xA0` (Apple's NTSC value).
5. Write `0x1000_2C00 = 0x0003_3125`. Wait for bit 31, then bit 30 of `+0x00` (≤ 10 ms each).
6. Feed for 8 s by DMA channel 6 (config `0x0200_FF01`), a self-linked chain from one line buffer in SRAM to `0x1000_2C04`, transfer information `0x1900_A001 | 2 << 4`. The buffer holds 8 vertical bars in the arm's format:
   - *a,* UYVY;
   - *b,* YUYV;
   - *c,* RGB565 pairs;
   - *d,* one 32-bit word per pixel, Y in the top byte.

   Log `0x1000_2C00` and channel 6's status every 100 ms.
7. Off, Apple's order: `0x1000_5404 = 0x80`; `0x1000_2C00 = 0`; channel 6 `+0x00 = 0`, `+0x04 = 0xFFFF_FFFF`; `0x1000_2818 = 0x0008_0000` (GPIO 19 low); `0x1000_1C30 = 0xA5A5_0000`.

**PP side:** one arm per request, owner's verdict between arms (SELECT = bars, PLAY = wrong colours, MENU = nothing); power cut after each arm. *As built:* one arm a row press, 4 s of bars, the verdict picked from a list (Colour bars / A garbled picture / Nothing).

**Log:** `pb8: arm=<a-d> ctl=<v…> dma=<status…> owner=<bars|colours|nothing>`.

**Pass:** one arm shows 8 bars with the right colours: the format is known and TV out is feasible on this unit. **Fail:** every arm shows nothing while PB-6 passed: the DMA or format is wrong, and the next step is to read Apple's band writer (@7C9A's callers).

**Safety:**
- AV cable in, no headphones.
- GPIO 19 is driven exactly as Apple drives it on every boot; no other fsel field is written.
- The DAC enable is set only during the 8 s window, and the order is Apple's.

**vc2sim:** the access list must equal Apple's on/off sequences (§2.3) plus the DMA set-up.

### PB-9 · Does the TV path scale? · R2 · **owner's go** · prefix `pb9:` · **image side and PP rows built, not run**
**Question:** is the 640 → 704 step hardware interpolation or a border (#62, #58)?

**BCM side:** PB-8 with the passing format, a buffer of 1-pixel vertical lines every 32 input pixels, and `0x1000_2C08` low half set in turn to 640, 704, 352 and 320.

**PP side:** the owner photographs the TV for each. *As built:* 640 then 704 only, 4 s each in the arm that showed bars; the owner says for each whether the lines reach both edges or leave a band, then which looked right.

**Log:** `pb9: in=<w> owner=<photo id>`.

**Pass:** the lines fill the same screen width at every input width, softened at 352/320: a scaler with interpolation. A narrower picture with borders means windowing.

**Safety:** as PB-8, one word changed. **vc2sim:** as PB-8.

**How the image side was built** (`HP_OP_TV`, `bcmfw/hostproto.c` `cmd_tv`) [ours]; where it departs from the steps above:
- **On order.** OUTPUT follows §2.3's table (Apple's run): Macrovision set, pixel clock, GPIO 19, DAC, timing words, control, then PB-8's two start waits and the DMA. PB-8's list puts the timing words before the Macrovision set; its own vc2sim rule asks for Apple's sequence, so §2.3 wins. TIMING is PB-7's order (clock, timing words, control).
- **Off order.** The strict reverse of the on order: channel 6 stopped and its status cleared, control 0, DAC `0x80`/`0xC0`, GPIO 19 low, clock `0xA5A5_0000`. §2.3's "Off" paragraph and PB-8 step 7 list the same writes in two other orders; the reverse stops the feed before the consumer. Then every register written is put back from its saved value, backwards through the on order (the pin's level before its function), and read back. Clock words are put back as `0xA5A5_0000 | low half` (they read back without the password, quirks.md #25), `0x2C18` without bit 31 (the field) and channel 6's config without bits 31:29 (the state).
- **Left out** because §2 doesn't document them: Apple's one-off `0x7777_7777` FIFO write and third wait (bits 31/29/28) on a first start; PB-9's 352 and 320 input widths (only the 640/704 pair is built, as asked); Apple's 150/35 MHz power requests (no clock change here); any sub-channel other than 0 for channel 6 (Apple's isn't read: 0 is the LCD's, proven on silicon).
- **Pattern** [ours]: BT.601 colour bars (or a white line every 32 pixels) in one line buffer at SRAM `0x12B000`, block at `0x12BC00`; byte order in memory UYVY = U Y0 V Y1, YUYV = Y0 U Y1 V, RGB565 two pixels a word (low half first), Y32 = Y in bits 31:24.
- **Bounded:** each start wait 10 ms, the window at most 1 s (TIMING) or 8 s (OUTPUT), all on the 1 MHz counter (the op refuses with `HP_E_STATE` if the counter isn't running) with a loop count behind each. The image's heartbeat keeps stepping in the window; `HP_TV_PROGRESS` (`0x138`) gives the phase and the ms.
- [sim] vc2sim has no TV model: the tests give `0x2C18` bit 31 a timed toggle and `0x2C00` bits 31/30 once enabled; its DMA model treats DREQ 0 (channel 6's config) as always ready, so the line repeats at the model's paced rate. Whether channel 6 is paced on silicon is open (#60).

**Needing the owner's explicit go:** PB-6, PB-7, PB-8 and PB-9 (R2: they enable a clock, and three of them drive the video output). Wave 1 (PB-1–PB-4) needs only the owner on cue for PB-2's states and PB-3's cable.
