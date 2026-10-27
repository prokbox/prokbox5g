# BCM2722 Processor Manual (VideoCore II)

*A reverse-engineered, clean-room reference and programming guide. Release deliverable #123; this is a working draft.*

This manual describes the Broadcom BCM2722 multimedia co-processor as it is fitted to the Apple iPod Video (fifth generation, "5G", and its late-2006 revision, "5.5G"). On that device the BCM2722 owns the 320×240 LCD panel and the TV output, and the host (a PortalPlayer PP5022/PP5021 ARM SoC) reaches it through a 16-bit memory-mapped port.

> **First stop for DMA, memory, cache or cross-chip work: [`../data-paths.md`](../data-paths.md)**, the map of every master and target on the board and what reaches what.

> **New to the chip? Start with [`programming.md`](programming.md)**, the programming guide: boot, memory and alignment, the instruction forms, the vector unit, DMA, clocks, the job ABI, writing and testing a kernel, verifying on the device, and the lessons learned.

Broadcom never published a datasheet or programming manual for this part. Everything here was recovered by:

- reading Apple's VideoCore firmware for the iPod Video (`vmcs.bin`, `RenderServer.bin` and the `.vll` libraries from the 1.2.1, 1.2.3 and 1.3 updaters) as a reference corpus, statically and in an instruction-level simulator;
- reading Rockbox's iPod Video LCD driver, which has started the chip since 2006;
- running our own VideoCore II images on a real iPod Video 5.5G (thirty-four device sessions, 2026-10-09 to 2026-10-13, then numbered bench sittings from 2026-10-25) and reading back what the silicon did;
- public material: Broadcom's product brief for the part, the Alphamosaic vector-processor patents, Rockbox tracker tasks, and an independent emulator project's notes (see `sources.md`).

It is written so that someone who has never seen this chip can boot code on it, drive the panel, set up SDRAM, use DMA and the vector unit, write a host-side video driver, and know which statements to trust.

## Scope

**Covered:** the memory map; start-up from the host, clocks, the PLL and the core regulator; SDRAM; the host interface from both sides; the LCD interface and the four panel variants the iPod Video ships with; the DMA engine; interrupts, the system timer and GPIO; the scalar and vector instruction sets, with every place silicon differs from earlier assumptions.

**Not covered, or only in outline:** TV out on silicon (Apple's register sequence is recovered in chapter 19; nothing has run yet), the BCM's audio output (Apple's driver is described in `audio.md`; nothing run on silicon yet), the cache's geometry, the 3D/graphics, JPEG and crypto features the product brief advertises (they are software on the vector unit, not separate blocks; `graphics.md` sizes what our own drawing code could do, and chapter 19 maps every brief claim), and Apple's own runtime (Nucleus PLUS RTOS, DispmanX-like display server, `gencmd`), which is described only where it reveals hardware.

## Conventions

- Addresses are as the **VideoCore core** sees them unless marked "host". Host addresses are PP5022 addresses (`0x3000_0000` window).
- `0x` hex throughout. Bit ranges are `[hi:lo]`, bit 0 the least significant. Registers are 32-bit and little-endian unless stated.
- "Password": several peripheral registers ignore writes unless bits `[31:16]` hold `0xA5A5`. Tables give the low 16 bits and say "pw" where the password is needed.
- `@xxxx` is an address inside Apple's `vmcs.bin` version 1.3 (it loads at core address 0, so file offset = address). These are pointers into the reference corpus for anyone repeating the work, not code.
- Instruction syntax follows our disassembler (`vc2dis`); `isa-scalar.md` and `isa-vector.md` define it.
- "Panel 3" etc. refer to the panel ID the chip reads from two GPIO pins (`lcd.md`).
- Times and rates measured on silicon are given with the session that measured them.

<a id="evidence-tags"></a>
## Evidence tags

Every fact carries one tag. A fact with two parts (a value and its meaning) may carry two.

| Tag | Meaning |
|---|---|
| **[silicon]** | Measured on a real BCM2722 in an iPod Video 5.5G with our own code. Cited by session (S1 to S34) or by bench sitting (from 2026-10-25), both listed in [`sources.md`](sources.md). Host-side behaviour measured on the same unit (thread scheduling, the audio clock) carries this tag too, with its session. |
| **[apple]** | Read from Apple's code: `vmcs.bin`/`RenderServer.bin`/`.vll` (VideoCore side) or `osos` (ARM side), statically or by running it in our simulator against modelled peripherals. Says what Apple's code *does*; meanings attached to it are separately tagged. |
| **[rockbox]** | From Rockbox source (`firmware/target/arm/ipod/video/lcd-video.c`) or its tracker. |
| **[external]** | Other public sources: Broadcom's product brief, patents, the siggifly emulator notes. |
| **[hypothesis]** | Our inference, not verified. Never promoted without new evidence. |
| **[ours]** | A convention defined by our own firmware: our host protocol (`bcmfw/hostproto.h`), our image's memory layout and behaviour, our host client's policies. Not a property of the chip; a different driver design can replace it. Kept apart from silicon facts so those stay usable on their own. |

When silicon contradicts an earlier reading, the silicon result is stated and the withdrawn reading is noted in `quirks.md`, so nobody re-derives it.

Tags also in use in the chapters, not in the original legend (kept as written; see "Known inconsistencies" below):

| Tag | How the chapters use it |
|---|---|
| **[sim]** | Result from our simulator (`vc2sim`) or another model only, not silicon. Used mostly in the research notes, and in chapter 14 for simulator runs |
| **[estimate]** | A figure computed from measurements or instruction counts, not itself measured. Used in chapter 18 (§3 and elsewhere) |
| **[measured]** | Used in a few entries (chapter 15 #50; chapter 3 §10) for a silicon measurement without a session number; read it as a [silicon]-class result |
| **[apple-RE]** | Used in chapter 15 #11 for a reading of Apple's code; same class as [apple] |

Tags can be combined with a session id, for example `[silicon S7]`, and qualified, for example `[rockbox comment]` or `[apple strings]`. A tag in a qualified form means the base tag. **Rule for editors:** never upgrade a tag without new evidence, and when a silicon result contradicts an earlier reading, state the silicon result and keep the withdrawn reading in chapter 14.

## How to read this manual

Each chapter stands on its own and states each fact once, with its tag, and links to the chapter that holds related detail. Section references such as "§5" mean a numbered section of the chapter named. Words in the glossary ([Appendix B](glossary.md)) are defined once there. Designs of our own (host protocol, job interface, video pipeline, power policy) are not in the reference chapters except as tagged [ours] examples; they live in [`../research/bcm2722/`](../research/bcm2722/) and the programming guide ([chapter 16](programming-guide.md)) points to them.

**Routes through the book:**

- *Get code running on the chip:* 1, 5 (§1 "Starting an image"), 4, 10, 9; then 16 §2. Write scalar code first (6), then vector code (7) with chapter 14 open beside you.
- *Drive video from the host:* add 11 and 8, then 17, with 15 #29–#33 for failures not yet explained.
- *Play audio, or draw for a game or the UI:* 12; 18 and 16 §5.
- *Port VideoCore IV knowledge:* read 14 first.
- *Anything about DMA, memory or caches:* [`../data-paths.md`](../data-paths.md) first, then 3.

## Contents

### Part I. The chip and its address space

| Ch. | File | What it covers |
|---|---|---|
| 1 | [`overview.md`](overview.md) | The chip at a glance: cores, memories, clocks, peripherals, the product brief, the generation |
| 2 | [`memory-map.md`](memory-map.md) | Every known address range and register block, with evidence |
| 3 | [`memory-access.md`](memory-access.md) | Every memory (SRAM, SDRAM, caches, VRF, peripherals) and every way to reach it: the scalar core, the vector unit, the chip's DMA, the host port and the PP's DMA; widths, alignment, ownership rules, measured speeds |

### Part II. Starting the chip and talking to it

| Ch. | File | What it covers |
|---|---|---|
| 4 | [`boot-and-clocks.md`](boot-and-clocks.md) | Power-up from the host, load address and entry, the cache, run control, the 1 MHz counter's clock, PLL, regulator, the switch to 150 MHz and back, power-down order |
| 5 | [`host-interface.md`](host-interface.md) | The PP5022 side (register banks, handshake bits, bootstrap, timing) and the BCM side (doorbell, interrupt line); Apple's transports and Rockbox's command word; our own protocol as a worked example |

### Part III. Instruction sets

| Ch. | File | What it covers |
|---|---|---|
| 6 | [`isa-scalar.md`](isa-scalar.md) | Scalar instruction encodings, ALU ops, conditions, flags, registers, ABI, exceptions, timing |
| 7 | [`isa-vector.md`](isa-vector.md) | The vector unit: VRF, formats, operands, modifiers, operations, memory ops, silicon rules |

### Part IV. Peripherals

| Ch. | File | What it covers |
|---|---|---|
| 8 | [`dma.md`](dma.md) | The 8-channel DMA engine, control blocks, transfer-info bits, completion status |
| 9 | [`lcd.md`](lcd.md) | The LCD "MPU" interface, panel identification, bus timing, the four panel controllers and their power sequences, pixel format, frame push by CPU and by DMA |
| 10 | [`interrupts-timers-gpio.md`](interrupts-timers-gpio.md) | Exception and interrupt vectors, the interrupt controller, the system timer, GPIO |
| 11 | [`sdram.md`](sdram.md) | The SDRAM controller and its bring-up sequence, refresh, the probe, silicon results |
| 12 | [`audio.md`](audio.md) | The serial audio output: Apple's use of it during video, the block at `0x1000_4800`, DMA channel 5, GPIO 1-3, what is known of its wiring, and a proposed bring-up |
| 13 | [`power.md`](power.md) | Everything about the BCM's power in one place: controls, power-down order, wake costs, what is unmeasured (pointers into chapters 4, 9, 17) |

### Part V. Errata and open questions

| Ch. | File | What it covers |
|---|---|---|
| 14 | [`quirks.md`](quirks.md) | One list of every place silicon differs from the simulator, from VideoCore IV, or from an earlier reading |
| 15 | [`open-questions.md`](open-questions.md) | What is not known yet, and how each could be settled |

### Part VI. Programming guide

| Ch. | File | What it covers |
|---|---|---|
| 16 | [`programming-guide.md`](programming-guide.md) | A route through the manual: bringing up an image, the host protocols, display jobs, with pointers to the design notes in `docs/research/bcm2722/` |
| 17 | [`host-driver.md`](host-driver.md) | Writing a host video driver: session lifecycle and timings, the bitstream FIFO, A/V sync on the host, panel ownership, performance, failure signatures |
| 18 | [`graphics.md`](graphics.md) | Drawing on the chip for games, Cover Flow and the compositor: the bounding figures, a drawing model, costs per operation, memory a game can have, latency from input to frame |

### Part VII. Product brief maps

| Ch. | File | What it covers |
|---|---|---|
| 19 | [`brief-map-peripherals.md`](brief-map-peripherals.md) | Product brief map: display, TV out, audio, peripherals and media. Every claim against the iPod's wiring and Apple's code, Apple's TV-out sequence, the boot pin map, leverage, and device stages PB-1–PB-9 |
| 20 | [`brief-map-power.md`](brief-map-power.md) | Product brief map: power, bus, clocks and memory. Every brief claim in that scope with its status, Apple's power manager, hibernate and `GPO32` bit 15, the clock manager, caches, the two host banks, bypass, and device stages PW1–PW8 |
| C | [`datasheet-features.md`](datasheet-features.md) | The product brief's features in the project catalog's shape (feature, source, status, evidence, benefit, probe), cited by `docs/hardware-features.md` |

### Appendices

| App. | File | What it covers |
|---|---|---|
| A | [`sources.md`](sources.md) | Bibliography and the device-session log |
| B | [`glossary.md`](glossary.md) | Glossary: VRF, REP, the host port registers, VP, the job interface and the rest |

## Known inconsistencies

Where the source files disagree, both statements are kept and the disagreement is noted here and at the place it occurs. None has been resolved by choosing.

1. **Conflict: session numbering around S34 and S35.** [`sources.md`](sources.md) lists sessions S1 to S34 (and the legend above says "S1 to S34"), and its S34 row is the "DMA lab" for the PP's DMA into the SDRAM FIFO. [`memory-access.md`](memory-access.md) §7b lists both an S34 and an S35 "DMA lab" with the same kind of result, and cites an S35 log (`session 35 bcmtest.log`). [`audio.md`](audio.md) §8 uses S35 for the first audio-block tone run (and S36c for later runs), and [`open-questions.md`](open-questions.md) #41 cites S35 for the DMA lab. [`../data-paths.md`](../data-paths.md) also cites an S37 (the PP's second DMA engine into SDRAM and the port). So S35 is used for two different experiments, and S35, S36c and S37 are not in the session log. The session log needs reconciling with the logs under the backup folder.
2. **Conflict: legend versus use.** The evidence legend lists six tags; the chapters also use [sim], [estimate], [measured] and [apple-RE] (table above). They are kept as written.
3. **Possible conflict: the same facts in several places.** Rows such as the SDRAM bring-up (S10, S28–S34) and the PP DMA results (S8, S32–S35) appear in chapters 3, 11 and 17 and in Appendix A with different amounts of detail; they agree where compared, but each copy should be checked when a number changes.

## What is missing for release

This is a working draft of a release deliverable. It restates the project's findings; it is not yet a complete developer manual.

**Thin or incomplete chapters**

- **8 (DMA)** is short and mostly [apple]; status bits 9 and 11, the state field, transfer-info bits 0/25/27/28, sub-channel concurrency and other DREQ numbers are unknown (15 #13).
- **10 (interrupts, timers, GPIO)**: no interrupt source is confirmed on silicon (15 #3); most sources and GPIO alternate functions are unmapped (15 #14).
- **11 (SDRAM)**: no speed or latency figures; self-refresh and power-down not documented (15 #7).
- **12 (audio)**: the block's clock direction, the wiring of GPIO 1 and a safe bring-up are unresolved (15 #34–#40); the bring-up in §6 is proposed, not proven.
- **13 (power)**: a gathering chapter. Current draw in every state is unmeasured (15 #12), and the standby states are [ours] and not yet run on silicon.
- **2 (memory map)** and **3 (memory access)**: no consolidated register index (address, name, width, reset value, access); the cache's geometry is unknown (15 #6); the cached and uncached aliases are unknown (15 #8).
- **16 (programming guide)** is a route with pointers, not a tutorial. It needs a complete minimal example (an image that boots and draws to the panel) with source and a build recipe that a stranger can follow, and a worked example for each of the host protocols.
- **TV out**: Apple's full register sequence is recovered from its code (`brief-map-peripherals.md` §2), but nothing has run on silicon; the FIFO format and the field meanings are open (15 #60–#62).
- No chapter yet gives a hardware-level block diagram; [`../diagrams/`](../diagrams/) holds the project's diagrams of the system, video path, memory maps and audio bus.

**Facts that are only modelled or inferred, not on silicon**

- Much of the instruction-set encoding comes from Apple's code and the simulator ([apple], [hypothesis]); kinds never checked on silicon are listed at the end of chapter 6, and chapter 7's untested forms in 15 #1, #2.
- Panels 0, 1 and 2 are [apple]/[hypothesis]; only panel 3 is verified on silicon (15 #17).
- Panel standby, the two overlays in the merged image (DECODE and the visualizer, sharing one region), the staged upload and the display-offload kernels beyond the drift are [ours] and have run in the simulator only (chapters 17, 18; research notes). The merged image as resident display driver, the job interface and the drift have run on silicon (sittings 2 to 4); the warm swap parks but one swap in ten fails (`quirks.md` #41); the 3D render test ran on silicon in stage 3d.
- The PP's DMA into the host port has never been shown to deliver a byte (chapter 3 §7b).

**Open questions that matter most for a developer** (see chapter 15 for all of them)

1. #1, #2: other vector forms and store layouts that differ from the model.
2. #3: interrupts on silicon (affects every sleeping or event-driven design).
3. #6: cache geometry and coherence.
4. #12: power draw by state.
5. #29-#33: video-session failures not yet explained (chapter 17 §8).
6. #34-#41: audio wiring and the PP's DMA into the port.

**Style and licence follow-up**: a review pass for wording (all text is project prose, with no third-party text copied), a check that every figure and address still matches the code, and a decision on how the siggifly-derived [external] items (cited in chapters 5 and a) are attributed in the release.
