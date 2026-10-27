# 1. Overview: the chip at a glance

## Identity and lineage

| | | Evidence |
|---|---|---|
| Part | Broadcom BCM2722 | [external] product brief 2722-PB01-R (2005-10-18) |
| Architecture | VideoCore II, designed by Alphamosaic (Cambridge, UK; acquired by Broadcom in 2004) | [external] brief; [apple] Apple's ARM classes `DisplayDriverVC02`, `LcdDriverVC02`, `DtvDriverVC02` in `osos`; `vmcs.bin` carries Broadcom Europe 2005–06 and Alphamosaic 2002–04 copyright strings |
| ELF machine number | 95 (`EM_VIDEOCORE`) in Apple's `.vll` libraries | [apple] |
| Vendor toolchain | MetaWare ("MetaWare Universal Linker v5.3h") | [apple] `.comment` sections |
| Relation to VideoCore IV (BCM2835) | Same architecture family, **different instruction encoding**: VC IV tools decode VC II code as nonsense. Operation semantics, register conventions and the vector model carry over closely | [apple] (disassembly gate; `isa-scalar.md`) |
| Used in | iPod Video 5G and 5.5G (2005–2009) | [rockbox] |

## Block summary

| Block | What is known | Evidence | Details |
|---|---|---|---|
| Scalar core | 32 × 32-bit registers (r24 gp, r25 sp, r26 lr, r30 sr, r31 pc); variable-length instructions of 16, 32 and 48 bits | [apple]; [external] patent US 7,036,001 | `isa-scalar.md` |
| Issue width | A dependent chain completes one instruction per cycle; independent instructions reach about 1.9 per cycle | [silicon] S6, S7 timing kernels | `isa-scalar.md` "Timing" |
| Vector unit (VPU) | 16 lanes, 8/16-bit elements, a 64 × 64-byte 2-D vector register file (VRF), REP counts, per-lane flags and predication, accumulators, a scalar-result unit; 48- and 80-bit instructions | [apple] corpus + simulation; [external] patents; [silicon] S2–S16 | `isa-vector.md` |
| "Dual ALU" | The brief advertises a 150 MHz dual-ALU core | [external] | the measured dual issue above is consistent with it [hypothesis] |
| Caches | 128 Kbit instruction and data caches; one control register at `0x10000C00` (flush, enable) | [external] size; [apple] register | `boot-and-clocks.md` |
| Internal SRAM | 1.25 MB at `0x0000_0000–0x0013_FFFF` (the brief's "10 Mbit on-chip SRAM") | [apple] Apple's heap table ends at `0x140000`; [external] brief | `memory-map.md` |
| SDRAM | 4 MB, stacked in the package (the brief's "32 Mbit stacked SDRAM"); `0xFE00_0000` for the core, `0xC000_0000` through the host port | [silicon] S11 (aliasing), S10 (host readback); [external] | `sdram.md` |
| Clocks | 27 MHz crystal; the core runs at 27 MHz after a host start; PLL A (×600/108) gives 150 MHz | [silicon] S7 | `boot-and-clocks.md` |
| Core regulator | 4-bit level at `0x10001400[3:0]`, chosen by frequency | [apple]; [silicon] S7 readback | `boot-and-clocks.md` |
| System timer | Free-running 1 MHz counter and four compares | [silicon] S4 | `interrupts-timers-gpio.md` |
| Interrupt controller | 32 sources, 4-bit priority each, a current-level register, a pending-vector register | [apple] | `interrupts-timers-gpio.md` |
| GPIO | 52 pins, BCM2835-like register layout | [apple]; [silicon] panel ID and reset pins S1–S4 | `interrupts-timers-gpio.md` |
| DMA | 8 channels, 32-byte control blocks, 1-D and 2-D transfers, DREQ pacing | [apple]; [silicon] S5 | `dma.md` |
| LCD interface | "MPU" (8080-style) parallel interface, index and data registers, programmable bus timing per chip select, an automatic pixel-run mode fed by DMA | [apple]; [silicon] S4–S7 | `lcd.md` |
| Host interface | 16-bit asynchronous port with two banks ("dual software channel"), address latches, auto-increment data, handshake bits; doorbell interrupt | [rockbox]; [apple]; [external] brief | `host-interface.md` |
| TV out | PAL/NTSC, composite and S-video: a timing generator at `0x1000_2C00`, a video DAC at `0x1000_5400`, a 27 MHz pixel clock, DMA channel 6 and GPIO 19 | [external] brief; [apple] | `brief-map-peripherals.md` §2 |
| Audio | A serial audio output: block at `0x10004800`, FIFO `+0x04` fed by DMA channel 5, pins GPIO 1–3 (function 2). Apple plays video soundtracks through it | [apple]; wiring [hypothesis] | `audio.md` |

## The iPod Video wiring

| Signal | Host (PP5022) side | BCM side | Evidence |
|---|---|---|---|
| Power | `GPO32` bit 14 (`0x4000`): set = BCM powered | — | [rockbox] |
| Bus | `0x3000_0000–0x3007_FFFF`; BCM register select on host address bits 16–18 | host port | [rockbox] |
| BCM → host interrupt | GPIO port C bit 6 (input) | GPIO 23 | [rockbox] host pin; [apple] BCM pin |
| LCD reset | — | GPIO 48, active low | [apple]; [silicon] S4 |
| LCD tearing effect | — | GPIO 49 | [apple] |
| Panel ID | — | GPIO 50, 51 | [apple]; [silicon] S1 |
| LCD bus | — | LCD interface at `0x1000_4000` | [apple]; [silicon] |

## What a host-started image finds

After Rockbox's bootstrap and upload (`host-interface.md`) the core begins at address `0x200` [silicon S1], with:

- the core on the 27 MHz crystal, no PLL running [silicon S2, S7];
- the 1 MHz counter **stopped** until its clock is enabled [silicon S2, S4];
- the cache off (the host flushes it and writes `0x10000C00 = 0` before starting the core) [rockbox];
- SDRAM not initialised [hypothesis: nothing has configured it; an image must run the sequence in `sdram.md` before use];
- the LCD bus usable at once with suitable timing words [silicon S4];
- the vector unit usable [silicon S2 onward].

## Performance figures measured on silicon

| Quantity | Value | Session |
|---|---|---|
| Core clock after host start | 27 MHz (crystal) | S7 |
| Core clock after Apple's PLL sequence | 150 MHz (all 69 timing kernels 5.56× faster) | S7 |
| Dependent 16-bit `add` chain | 1 instruction per cycle | S7 |
| Independent instruction pairs | about 0.53 of the dependent chain's time | S6 |
| Vector op with REP 16 (ALU / `vld16` / `vst16`) | 16.25 / 16.63 / 16.50 cycles | S7 |
| Single-row 48-bit vector store | about 1.5 cycles | S7 |
| LCD bus cycle after host start | 37.04 ns (27 MHz) | S6, S7 |
| Full 320×240 frame to panel 3 by DMA, timing at Apple's nanoseconds | 14.26 ms | S7 |
| Host upload into BCM SRAM from an unboosted PP5022 (2-register `ldm`/`stm` loop) | about 0.25 µs per pixel | S7 (inferred from per-row times) |
| Command round trip, host to BCM and back | 6–7 µs | S2 |
| Bootstrap, upload of an 8 KB image, ready | about 2 ms from upload to ready | S7 |
| H.264 Baseline 320×240 decode, our decoder with vector kernels, 150 MHz | about 7 ms per frame (all scalar C: 31–35 ms) | S15 |
| The same, every kernel on the vectors after the silicon fixes (`quirks.md` #8, #16–#20): synthetic clip | 7 ms per frame (max 31), output bit-exact against ffmpeg | S27 |
| The same: real footage (pans and cuts, 24 fps) | 18 ms per frame (max 39); 312 of 312 shown, A/V max 15 ms, no hitch | S27 |
| The same: a converted episode excerpt, per `FEED` (decode, convert, push) | 14.8–14.9 ms mean, 46 ms max | S29, S30 |
| Video session start: power-down, boot of a 93 KB image, first frame | about 0.61 s (0.37 s with the BCM already off); the first `LCD_UPDATE` after power-up takes 280 ms | S30 (`host-driver.md` §3) |

## Product brief claims not exercised here

Every claim of the brief is mapped against the iPod's wiring and Apple's code in [`brief-map-peripherals.md`](brief-map-peripherals.md) (display, TV out, audio, peripherals, media) and [`brief-map-power.md`](brief-map-power.md) (bypass, power modes, clocks, caches, DMA, host bus).


[external] The brief lists, among others: 2-megapixel JPEG encode and decode, a 64-voice MIDI synthesiser, "graphics acceleration for 3D games", DRM (AES, 3DES, RSA, CPRM), multiple displays (main and sub LCD), and Macrovision on TV out. Apple's corpus shows software for JPEG, AES and a shader-style renderer [apple], consistent with the brief's statement that codecs run in software on the VideoCore rather than in fixed-function blocks. No fixed-function codec block has been found [hypothesis: none exists]. Three more claims are open questions because they could change the power plan or the video path: a bus bypass mode that lets the host reach peripherals with the BCM powered down, sleep and hibernate power modes (0.6 V in hibernation), and output scaling in the LCD controller (`open-questions.md` #56–#59, with the caches).
