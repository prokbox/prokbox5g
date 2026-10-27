# Appendix B. Glossary

Terms and abbreviations used in this manual, with the chapter that holds the detail. Definitions here only restate what the chapters say; the evidence tags live in the chapters. Names for the chips follow the technical convention: **PP5022** (its two ARM cores, **CPU** and **COP**), **BCM2722** ("the BCM"), **WM8758** (the audio codec).

## The chips and the board

| Term | Meaning |
|---|---|
| **BCM2722** | Broadcom's multimedia co-processor in the iPod Video: a VideoCore II. Owns the 320×240 LCD panel and the TV output. Chapter 1 |
| **VideoCore II (VC2)** | The BCM2722's processor architecture: a scalar core plus a 16-lane vector unit. Chapters 6 and 7 |
| **PP5022 / PP5021** | The PortalPlayer ARM SoC that is the iPod Video's main processor, the BCM's host |
| **CPU, COP** | The PP5022's two ARM cores (the second is the co-processor) |
| **WM8758** | The audio codec on the board. The BCM's audio pins appear to share its audio bus (chapter 12) |
| **Host** | The PP5022 side of the host port. "Host address" means a PP5022 address (the `0x3000_0000` window, or `0xC000_0000` for SDRAM through the port) |
| **Core address** | An address as the BCM's own core sees it (chapter 2) |
| **5G / 5.5G** | The fifth-generation iPod Video and its late-2006 revision |

## The host side

| Term | Meaning |
|---|---|
| **Host port** | The BCM's 16-bit memory-mapped window on the PP5022 bus, through which the host reads and writes any BCM address (SRAM, SDRAM, peripherals). Chapter 5 |
| **Port registers** | `DATA`, `WR_ADDR`, `RD_ADDR` and `CONTROL`, at host `0x3000_0000`, `0x3001_0000`, `0x3002_0000`, `0x3003_0000`; the second ("ALT") bank has `ALT_DATA`, `ALT_WR_ADDR`, `ALT_RD_ADDR` and `ALT_CONTROL` at `0x3004_0000`–`0x3007_0000`. Chapter 5 |
| **Handshake bits** | The ready/busy bits in `CONTROL`, `RD_ADDR` and `ALT_CONTROL` that the host polls before each access. Chapter 5 |
| **Auto-increment** | After one address write, `DATA` accepts consecutive halfwords at rising addresses |
| **Kick** | The `CONTROL = 0x31` write Rockbox makes after each command word |
| **Bootstrap** | The three-stage host sequence (strap bits, the eight magic bytes, the image stream) answered by the BCM's internal boot ROM. Chapter 5, "Starting an image" |
| **Boot ROM** | The BCM's internal ROM that answers the bootstrap handshake; its contents are not read directly |
| **Command word** | The word at `0x1F8` used by Rockbox's protocol, written as `BCM_CMD(x) = (~x << 16) \| x`. Chapter 5 §3a |
| **Torn read / torn word** | A 32-bit word read through the port while the other side rewrites it, so that its halves come from different values. Chapter 5 §1 and quirk 23 |
| **`HP_ENC` / `HP_VALID`** | Our encoding that stores a 16-bit value as `~v << 16 \| v` so a torn read is detectable. Chapter 5 §3c |
| **GPO32** | A PP5022 GPIO output register; bit 14 powers the BCM. Chapter 4 §5 |
| **Dead port** | An unpowered or absent BCM: every halfword reads `0xFFFF` |
| **NOR `vmcs`** | The BCM firmware image held in the iPod's NOR flash, started by Rockbox's bootstrap |
| **`vmcs.bin`, `RenderServer.bin`, `.vll`** | Apple's VideoCore firmware, render server and loadable libraries; the reference corpus for [apple] facts. Appendix A |
| **`osos`** | Apple's ARM-side firmware image; source of the [apple] host-side facts |
| **`@xxxx`** | An address inside Apple's `vmcs.bin` 1.3 (loads at 0, so file offset = address) |

## Our protocols and designs

| Term | Meaning |
|---|---|
| **VP** | The BCM2722 video host protocol, our design ([`../research/bcm2722/hostproto.md`](../research/bcm2722/hostproto.md)); an implementation of the shared-memory pattern in chapter 5 §3c |
| **JB, job interface** | The display job interface: the shell sends the BCM small jobs (blit, fill, fade, average, scroll, present) instead of pixels ([`../research/bcm2722/jobs.md`](../research/bcm2722/jobs.md)). All [ours]; nothing on silicon yet |
| **Slot** | A picture held in BCM memory, uploaded once by the PP and named by a content key (job interface) |
| **Ring** | A queue in shared memory with one writer per index word: a command ring (host to BCM) and an event ring (BCM to host). Chapter 5 §3c |
| **Warm swap** | Replacing the running image without cutting BCM power, so the panel stays lit ([`../research/bcm2722/warm-swap.md`](../research/bcm2722/warm-swap.md)) |
| **Resident image** | A video image that also answers the host's display commands between sessions. Chapter 17 §3 |
| **Reload / restart** | Chapter 17 §3: a restart repeats a session; a reload is a power cut plus a full boot |
| **Class A/B/C** | The project's resource classes for games: A changes only a few bands a frame, B runs beside music (2D scene, core at 27 MHz), C holds the chip for the session (affine layers and 3D, 150 MHz). Chapter 18 §2 |
| **S1 … S36c** | Device sessions on the 5.5G; the [silicon] evidence is cited by session. Appendix A |
| **dev*N*, stage *Nx*** | Names of our development builds and bench stages cited as evidence; they are in the research notes |

## The scalar core and the vector unit

| Term | Meaning |
|---|---|
| **Scalar core** | The 32-bit core with 16- and 32-bit (and longer) instruction encodings. Chapter 6 |
| **`gp`, `sp` (r24, r25)** | Global and stack pointer by convention; set both first at entry. Chapters 4 and 6 |
| **VPU** | The vector unit. Chapter 7 |
| **Lane** | One of the 16 parallel 16-bit processing units in the VPU |
| **VRF** | Vector register file: 64 rows by 64 bytes, 2-D, addressed by (y, x), readable horizontally or vertically. Chapter 7 |
| **H / V vector** | A vector read horizontally along a VRF row (H) or vertically down rows (V); the views alias the same storage |
| **REP** | The repetition count (1, 2, 4, 8, 16, 32, 64 or from r0) with which a vector instruction repeats over successive VRF rows |
| **Flags** | Per-lane Z, N, C flags, set by `setf` and used for predication |
| **Accumulator** | One per lane |
| **Scalar-result unit** | Reduces the enabled lanes (sum, min or max) into a scalar register |
| **48-bit / 80-bit** | The compact and full vector instruction lengths |
| **`vld16`, `vst16`, `vmulhrs` …** | Vector load, store and multiply mnemonics as our disassembler writes them. Chapter 7 |
| **`vc2dis`, `vc2sim`, `vc2cc`** | Our disassembler, instruction-level simulator and C compiler for VideoCore II ([`../research/bcm2722/sim.md`](../research/bcm2722/sim.md), [`compiler.md`](../research/bcm2722/compiler.md)) |
| **`pw` / password** | Many peripheral registers ignore writes unless bits `[31:16]` hold `0xA5A5` |
| **Imprecise exception** | A fault reported after the faulting instruction has already had its effect (misaligned access, vector 1). Chapter 10 |

## Peripherals and memory

| Term | Meaning |
|---|---|
| **SRAM** | The BCM's internal memory, from core address 0. Chapters 2 and 3 |
| **SDRAM** | The 4 MB stacked SDRAM at `0xFE00_0000` (core) / `0xC000_0000` (host). Chapter 11 |
| **FIFO** | In chapter 17, the bitstream buffer in SDRAM that holds compressed pictures |
| **Cache control** | Register `0x10000C00`: flush and enable. Chapter 4 §2 |
| **1 MHz counter** | The free-running microsecond counter; needs its clock started by `0x10001C4C` / `+0x54`. Chapter 4 §3 |
| **PLL A … D** | The four PLLs; PLL A takes the core to 150 MHz. Chapter 4 §4 |
| **Regulator** | The core-supply level register at `0x10001400` ("regulator" is [hypothesis]) |
| **DMA channel / control block** | The 8-channel DMA engine at `0x1000_0800` and the descriptor it reads. Chapter 8 |
| **DREQ** | The request line that paces a DMA channel from a peripheral |
| **MPU interface** | The LCD controller's "MPU" bus interface at `0x1000_4000`. Chapter 9 |
| **Panel ID** | A value (0–3) the chip reads from two GPIO pins to tell the four LCD panel variants apart. Chapter 9 §2 |
| **TE** | The panel's tearing-effect signal (GPIO 49, interrupt source 11) |
| **Window / RAM-write index** | The rectangle and the register selected before pixels are sent to the panel. Chapter 9 §6 |
| **Host frame** | The 320×240 RGB565 frame the host writes at `0xE0000` for Rockbox's protocol |
| **GPIO 1–3 / GPIO 23** | The audio pins (function 2) / the pin that pulses the host's interrupt line. Chapters 5 and 12 |
| **Audio block** | The serial audio output at `0x1000_4800`, fed by DMA channel 5. Chapter 12 |
| **I²S, LRCK, BCLK** | The serial audio bus and its word and bit clocks |
| **Nucleus PLUS, `gencmd`** | Apple's VideoCore RTOS and its text command service; described only where they reveal hardware |

## Video terms

| Term | Meaning |
|---|---|
| **GOP** | Group of pictures: a run of pictures decodable from a key picture |
| **NAL** | Network abstraction layer unit: the packaging unit of an H.264 bitstream |
| **A/V sync** | Keeping decoded pictures in step with the audio the host plays. Chapter 17 §5 |
