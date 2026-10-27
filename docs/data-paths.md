# Data paths and reachability: who can move bytes where

The map of every bus master on the modded iPod Video 5.5G (PP5022 + BCM2722 + WM8758, 64 MB, flash storage), every memory and register target, and which master can reach which target: whether that works on our unit, how fast it goes, how to make it work, and how it fails. The owner asked for it on 2026-10-15, after several agent sessions went down dead ends on wrong assumptions about DMA and cross-chip paths.

**This file is the first stop before any DMA, memory, cache, cross-chip or bus work.** It summarises and links; the register-level detail lives in the two reference manuals (`docs/pp5022-reference/`, `docs/bcm2722-reference/`). It is also meant to stand on its own as a specification for a from-scratch firmware: each recipe gives the registers, bit meanings, order and timing in our own words. File and function names are given only as evidence that a path was done, never as the explanation.

Diagrams (sources `diagrams/*.d2`, PNG previews beside each SVG):
- [`diagrams/data-paths.svg`](diagrams/data-paths.svg): the overview, chips, memories, what plugs in and the main paths;
- [`diagrams/data-paths-pp.svg`](diagrams/data-paths-pp.svg): the PP5022 side, every master and what it reaches;
- [`diagrams/data-paths-bcm.svg`](diagrams/data-paths-bcm.svg): the BCM2722 side, the host's window, the cores and the panel;
- [`diagrams/data-paths-audio.svg`](diagrams/data-paths-audio.svg): audio, the clocks, and the outputs (headphone jack, dock line out).

## 0. How to develop on this platform (one screen)

**Which side does what.**
- **The PP5022 owns everything except the panel**: storage (IDE), USB, audio (I2S to the WM8758, I2C control), the wheel, power, and the 64 MB SDRAM. Two ARM7TDMI cores, CPU and COP, share one clock, the SDRAM and a 128 KB IRAM, with no cache coherency between them. The CPU takes every interrupt and owns the peripherals; the COP is a compute engine with no interrupts of its own.
- **The BCM2722 owns the panel** and has its own 1.25 MB SRAM and 4 MB SDRAM, a scalar core with a vector unit, and an 8-channel DMA engine. It keeps nothing across a power cut: every wake is a cold upload of its image.
- **The only data path between the chips is the host port**, a 16-bit memory-mapped window at PP `0x3000_0000`. Only the PP masters it. No path is known by which the BCM can write into PP memory; everything the BCM produces, the PP fetches by port reads.
- **Audio has a second shared wire set**: the WM8758 drives the I2S bit and frame clocks to both the PP's I2S block and the BCM's GPIO 2/3. The clocks exist only while the codec's PLL runs.

**What is cheap.**
- PP CPU or COP work in cached SDRAM or in IRAM (single cycle).
- BCM core and vector work in its SRAM: vector row loads and stores move about 31 bytes a cycle at 150 MHz.
- The BCM's LCD DMA: 14.26 ms a full frame, costing the BCM core nothing while it runs.
- DMA the PP side has always used: I2S playback (channel 0), the IDE controller, the USB controller.

**What is expensive.**
- **Every byte across the host port.** About 10 PP cycles per 16-bit transfer by any method: 5.7 MB/s at 30 MHz, 16 MB/s at 80 MHz. A full 320×240 frame is 27 ms of CPU at 30 MHz. Reads are worse: each word is its own handshake.
- PP cache maintenance: whole-cache only (8 KB per operation), per core.
- Boosting to 80 MHz: both cores, every boost. UDMA above mode 2 boosts once per ATA command.
- A BCM power cycle: 0.37–0.62 s from upload to a running image, panel dark.

**What to try first.**
1. Move bytes into the BCM with the **PP CPU's copy**, unboosted, from a thread placed in its own priority band (R1). Give the BCM compressed or small data, and let it do pixel work and the panel itself (R5).
2. Keep the **BCM's cache off** and use word accesses; check `docs/bcm2722-reference/quirks.md` before writing any BCM memory code.
3. Share PP data between the cores through the **uncached alias or IRAM**, and hand bulk buffers over with one cache operation per hand-off (R7).
4. Use only the DMA paths marked [silicon] below. **Do not** build on PP DMA into the host port (D1), on UDMA4 for speed (D3), or on any [unknown] cell without first running the experiment named for it.
5. **Design every experiment so that "nothing happened" cannot pass.** Fill the target with a unique pattern first, read every word back, and count wrong words (D1: S8's "clean" DMA result was a CPU write read back).
6. **New C for the BCM: build it for the Mac too, and compare checksums with vc2sim.**
   - vc4-elf-gcc 6.2.1 has at least one wrong-code bug: `min.l`/`max.l` drop their source, and vc2cc now refuses them (`research/bcm2722/compiler.md`, "Known GCC bugs").
   - A host build that matches the simulator bit for bit confirms the compiler and the simulator for that code (`bcmfw/r3d/host`, `r3d_test.py`).

## 1. Evidence tags

Every claim carries a **status** and a **provenance** tag, and a citation.

| Status | Meaning |
|---|---|
| **[silicon]** | Works (or was measured) on our unit with our code. Cited by session (S*n*; logs in `session log, session`, read only) or by a device log line |
| **[sim]** | Shown only in vc2sim, the UI simulator or host tests |
| **[apple]** | Seen in Apple's firmware (reverse-engineered), not reproduced by us |
| **[dead end]** | Tried and failed; the evidence and the best-known reason are given |
| **[unknown]** | Not settled; the experiment that would settle it is named |
| **n/a** | No physical path |
| **[hypothesis]** | Our inference, never promoted without new evidence |
| **[ours]** | A convention of our own firmware (a memory layout, a protocol, a policy), not a property of the hardware |

| Provenance | Meaning |
|---|---|
| **[measured]** | Our own device, simulator or host experiment established it |
| **[rockbox-derived]** | Known only because Rockbox's (or iPodLinux's) working code does it; we have not checked the register meaning ourselves |
| **[apple-RE]** | Seen in Apple's firmware, read statically by us or reported from an emulator running it (the siggifly notes, `docs/pp5022-reference/sources.md`) |

Example: "UDMA2 reads land in SDRAM [silicon; rockbox-derived]" means it works every day on our unit, but the register sequence is Rockbox's and was never verified bit by bit by us. Items tagged [rockbox-derived] or [apple-RE] are the gaps a clean-room build must re-verify; they are collected in §8.

## 2. Masters: everything that can start a read or write

| # | Master | What it is | Status |
|---|---|---|---|
| M1 | **PP CPU** | ARM7TDMI at 24/30/80 MHz, CPU half of the PP5022. Takes all interrupts. Its cache (8 KB, write-back) covers logical SDRAM `0x0000_0000–0x03FF_FFFF` and flash `0x2000_0000–0x23FF_FFFF`; physical SDRAM at `0x1000_0000` is the **uncached alias** of the same bytes; IRAM and registers are never cached | [silicon; rockbox-derived] (`pp5022-reference/memory-map.md` §6) |
| M2 | **PP COP** | The second ARM7TDMI, same clock and bus, its own cache status array; whether the data array is shared with the CPU is unknown. No coherency with the CPU or with DMA. Woken each tick by the CPU; no interrupts of its own in our design | [silicon S36; measured] (codec decode on the COP), cache facts [rockbox-derived] (`cores-and-threads.md` §3–§4) |
| M3 | **PP general DMA engine** (`0x6000_A000`, channels at `0x6000_B000 + 0x20·n`, n = 0–3) | Memory ↔ peripheral, physical addresses, bypasses the cache, ≤ 64 KB per command, optional request-line pacing. **Channel 0** feeds I2S (request 2). **Channel 3** was our host-port experiment | ch 0 [silicon; rockbox-derived]; ch 3 into the port [dead end] (D1) |
| M4 | **PP second DMA engine** (`0x6000_8000`, channels `0x6000_9000 + 0x20·n`, interrupt 27 [hypothesis]) | A 2-channel controller no open driver uses. Apple's code uploads the BCM image through it in 64 KB pieces; **it works into the BCM on our unit** (cell 14) | [silicon S37; measured] (cell 14); register meanings partly [apple-RE] (`pp5022-reference/dma.md` §6) |
| M5 | **IDE controller DMA** (`0xC300_0400`) | The ATA controller's own bus master: PIO by CPU, or multiword/Ultra DMA into physical SDRAM | UDMA2 [silicon; rockbox-derived]; UDMA4 [silicon S36c; measured], no gain |
| M6 | **USB controller** (ARC/ChipIdea at `0xC500_0000`) | Bus master for its queue heads and transfer descriptors (kept in IRAM) and the data buffers (physical SDRAM) | [silicon; rockbox-derived] (mass storage every session) |
| M7 | **PP I2S block** (`0x7000_2800`) | Not a bus master: a 16-entry TX FIFO filled by DMA channel 0 or by an FIQ handler. A clock **slave** on this board | DMA fill [silicon; rockbox-derived]; FIQ fill [unknown] on our unit (not used) |
| M8 | **The host port bridge** (PP `0x3000_0000–0x3007_FFFF`) | The PP-mastered window onto the BCM's whole address space: SRAM, SDRAM at host `0xC000_0000`, peripherals `0x1000_xxxx`. 16-bit data bus; a 32-bit access becomes two 16-bit cycles, low half first. **Two banks** (main `0x3000–0x3003`, ALT `0x3004–0x3007`), each with its own address latch: Apple keeps one lock per bank and puts bulk display pixels on ALT, commands on main [apple-RE]; latch independence on silicon is stage PW7 (`bcm2722-reference/brief-map-power.md` §7) | [silicon; measured + rockbox-derived] (`bcm2722-reference/host-interface.md` §1) |
| M9 | **BCM scalar core (VPU)** | VideoCore II scalar core, 27 MHz at entry, 150 MHz on PLL A. Cache present (128 Kbit I + 128 Kbit D) but kept **off** by us | [silicon S1 on; measured] |
| M10 | **BCM vector unit** | 64 × 64-byte register file reached only through `vld`/`vst`/`vstr`; strict alignment rules (D5) | [silicon S5 on; measured] |
| M11 | **BCM DMA engine** (`0x1000_0800`, 8 channels × 3 sub-channels) | Chains of 32-byte control blocks; 1-D or 2-D; paced by a peripheral request (DREQ). **Channel 1** = LCD (DREQ 3), **channel 5** = audio FIFO (DREQ 7); Apple also uses 6 (TV) and 7 sub-channel 1 (memcpy) | ch 1 [silicon S5–S7]; ch 5 [silicon S35]; 6, 7 [apple] |
| M12 | **BCM audio block** (`0x1000_4800`) | Serial audio out on GPIO 1 (data); takes bit and frame clocks on GPIO 2/3 as a **clock slave** [hypothesis, strong] | reached the headphones once, as a screech [silicon S35] (D6) |
| M13 | **WM8758 codec** | Not a memory master; the **I2S clock master** of the board. Its PLL makes BCLK and LRCK; with the PLL off (our DAC standby) there are no clocks for anyone | [silicon S32–S36c; measured + rockbox-derived] |

## 3. Targets

### PP side

| Target | Address, size | Notes | Evidence |
|---|---|---|---|
| SDRAM, cached view | logical `0x0000_0000–0x03FF_FFFF` (64 MB) after the remap | What CPU and COP normally use. Bus masters (M3, M5, M6) cannot use logical addresses | [silicon; rockbox-derived] (`memory-map.md` §2–§3) |
| SDRAM, uncached view | physical `0x1000_0000 + x` | The same bytes, uncached. DMA masters take this address. Our cross-core data (`SHAREDBSS`) is linked here | [silicon; rockbox-derived]; cross-core [silicon S36; measured] |
| IRAM | `0x4000_0000`, 128 KB, uncached, single-cycle, shared by both cores | Split by convention: 48 KB core, 80 KB codecs/plugins. Last 256 B are the boot ROM's hand-off area | [rockbox-derived] (`memory-map.md` §4) |
| Peripherals | `0x6000_xxxx` (system), `0x7000_xxxx` (I2S, I2C, wheel), `0xC300_0000` (IDE), `0xC500_0000` (USB) | Registers; CPU-owned in our design | [rockbox-derived] |

### BCM side

| Target | Core address | Host address | Notes | Evidence |
|---|---|---|---|---|
| SRAM | `0x0000_0000–0x0013_FFFF` (1.25 MB) | same, through the port | Image loads at 0, enters at `0x200`. Our layout: vectors, mailbox `0x100–0x1FF`, image to `0x20000`, decoder arena, stack (top `0xD0000`), rings `0xD0000`/`0xD2000`, host frame `0xE0000` (150 KB), `VIDEO_FB` `0x105800` (150 KB) | [silicon]; layout [ours] (`memory-access.md` §2) |
| SDRAM | `0xFE00_0000–0xFE3F_FFFF` (4 MB; bit 22 not decoded) | `0xC000_0000–0xC03F_FFFF` | **Unusable from either side until the image runs the bring-up** (controller `0x1000_3400`, mode field 4 on our unit). Our layout: 1 MB bitstream FIFO at 0, DPB, audio window `0x340000`, AAC far data `0x380000`, dumps | [silicon S10, S11]; layout [ours] (`memory-access.md` §3, `sdram.md`) |
| Peripherals | `0x1000_0000–0x1000_FFFF` | same, through the port | 32-bit accesses only; some need `0xA5A5` in `[31:16]` | [silicon; apple-RE] |
| LCD interface and panel | LCD block `0x1000_4000`, FIFO `0x1000_400C` | — | The panel hangs only off the BCM. Bus at 27 MHz after a host start | [silicon S4–S7] (`lcd.md`) |
| Mailbox | SRAM `0x100–0x1FF` | same | Plain SRAM by convention; `0x1F8` is the command word of the stock protocol | [ours]; [rockbox-derived] for `0x1F8` |
| Caches | `0x1000_0C00` control | through the port | Off at entry (the host flushes and writes 0); we keep them off. Apple flushes only after writing code and never for host-written SRAM data, so the caches probably front SDRAM only (bit 16) [apple-RE; hypothesis]; stage PW2 (`bcm2722-reference/brief-map-power.md` §6). **Code the core writes is not run before Apple's @25a flush**: the warm swap's park, which skipped it, never reached its stub on silicon; every image now flushes before running written code (`bcm2722-reference/quirks.md` #40) [silicon; apple-RE; cause hypothesis]. Bit 0 (flush done) stays set at idle [silicon, PW1] | [silicon; rockbox-derived] |

### The WM8758

| Target | How | Evidence |
|---|---|---|
| Audio data (DACDAT) | I2S from the PP's data-out; and, by some unknown joining, from the BCM's GPIO 1 (S35 heard the BCM) | [silicon]; joining [unknown] (open question BCM #39) |
| Registers | I2C at 7-bit address `0x1A`, 9-bit registers, **write-only** (keep a shadow) | [silicon; rockbox-derived] (`pp5022-reference/audio.md` §3) |

## 4. The reachability matrix

Status letters: **S** silicon, **M** sim only, **A** Apple only, **X** dead end, **?** unknown, **—** no physical path. The number is the per-cell note in §4.1.

| Master ↓ / Target → | PP SDRAM | PP IRAM | PP periph | BCM SRAM | BCM SDRAM | BCM periph | LCD panel | WM8758 data | WM8758 regs |
|---|---|---|---|---|---|---|---|---|---|
| **PP CPU** | S 1 | S 1 | S 1 | S 2 | S 3 | S 4 | — 5 | S 6 | S 7 |
| **PP COP** | S 8 | S 8 | ? 9 | ? 9 | ? 9 | ? 9 | — 5 | — | ? 9 |
| **PP DMA ch 0** | S 10 | ? 11 | S 10 | — | — | — | — | S 10 | — |
| **PP DMA ch 1–3** | X 12 | ? 11 | ? 12 | X 13 | X 13 | ? 13 | — | — | — |
| **PP DMA engine 2** | S 14 | ? 14 | ? 14 | S 14 | S 14 | ? | — | — | — |
| **IDE DMA** | S 15 | ? 11 | — | — | — | — | — | — | — |
| **USB DMA** | S 16 | S 16 | — | — | — | — | — | — | — |
| **BCM core** | — 17 | — | — | S 18 | S 19 | S 20 | S 21 | — | ? 22 |
| **BCM vector** | — | — | — | S 23 | S 23 | — | — | — | — |
| **BCM DMA ch 1** | — | — | — | S 24 | ? 25 | S 24 | S 24 | — | — |
| **BCM DMA ch 5** | — | — | — | S 26 | ? 25 | S 26 | — | S 27 | — |
| **BCM DMA other** | — | — | — | A 28 | A 28 | A 28 | — | — | — |
| **BCM audio block** | — | — | — | — | — | — | — | S 27 | — |
| **WM8758 (clocks)** | — | — | S 29 | — | — | S 29 | — | — | — |

"BCM periph" includes the LCD FIFO and the audio FIFO. "PP periph" in the WM8758 row means the I2S block's clock pins; in the BCM column, GPIO 2/3.

### 4.1 Per-cell notes

**1. PP CPU → its SDRAM, IRAM, peripherals** [silicon; rockbox-derived]. The everyday path. Cached SDRAM through logical addresses; the uncached alias at `0x1000_0000 + x`; IRAM uncached and single-cycle. Peripherals 32-bit, at fixed addresses. Gotchas: a misaligned halfword or word access to SDRAM aborts (data abort, [silicon] P12 bench 2026-10-24: Chute's (then called Airdrop) `strh` at an odd address; `pp5022-reference/quirks.md` #51); the simulator and unicorn accept it, so align every buffer the firmware hands out, to 8 at least. Cache maintenance is whole-cache only and per core; never use the cache's own invalidate bit (Rockbox found it corrupts memory; it rewrites each line's status word instead) [rockbox-derived] (`pp5022-reference/quirks.md` #8–#11).

**2. PP CPU → BCM SRAM, through the port** [silicon S1 on; measured].
- Rate: 153,600 B in 26,957 µs at PP 30 MHz (5.7 MB/s) and 9,629 µs at 80 MHz (16 MB/s), 2-register `ldm`/`stm` [silicon S8]. 8-register bursts gave no gain (27,377 / 9,909 µs) [silicon S8]. One short write with its address: 7.4 µs [silicon S8].
- Recipe: R1. Evidence of use: `firmware/target/arm/ipod/video/bcm-devtest-hal.c` `dt_wr_block`; the boot upload in `bcmfw-boot.c`.
- Gotchas: into SRAM with the BCM at 27 MHz it is bound by the PP's clock (about 10 PP cycles per 16-bit transfer at both clocks) [silicon S8]; into SDRAM the BCM's clock matters too (cell 3, DP-1). Reads: R2.

**3. PP CPU → BCM SDRAM, through `0xC000_0000`** [silicon S10 on; measured].
- Rate, PP at 80 MHz: 17–18 MB/s of port busy time during video with the BCM at 150 MHz, 10–12 MB/s with it at 27 MHz [silicon S33, S34, boosted matrix rows] (`pp5022-reference/display.md` §5). Unboosted rows also logged ~16 MB/s, but the PP had been boosted when each began, so the 30 MHz rate into SDRAM is not pinned (DP-1).
- Correctness: 0 wrong words over thousands of read-back words per session (first and last word of every piece checked; 64 words over the FIFO before play) [silicon S28–S35]. Writes land safely while the BCM core decodes [silicon S12].
- Recipe: R1, after R4 has brought SDRAM up. Evidence: `bcm-devtest-hal.c` `bcm_video_wr_block` (CPU branch); `apps/video/bcmvp.c` `verify_word`.
- Gotchas: undefined before the BCM's bring-up; the host must also respect the chip's ownership rules (`memory-access.md` §8). **A write may not be visible to the core behind a command written to SRAM afterwards** (a late, perhaps posted write): a `FEED` sent the moment its packet was written gave no picture at any band in sitting 4b [silicon symptom, 2026-10-26; mechanism hypothesis, BCM #73]. Write ahead, and read the newest packet's last word back before a close `FEED` (R1 step 6; D16).

**4. PP CPU → BCM peripheral registers, through the port** [silicon; measured + rockbox-derived]. The host flushes the BCM cache (`0x1000_0C00`), starts the core (`0x1000_0400`), and reads GPIO, DMA and LCD registers while the core runs (`memory-access.md` §5). 32-bit accesses only.

**5. PP → LCD panel** — no path. The panel is wired to the BCM; the PP5022's own LCD bridges (`0x7000_3000`, `0x7000_8A00`) are unused on this model [rockbox-derived] (`display.md`), and Apple's firmware never reaches the panel any other way [apple-RE]. Every pixel goes through the BCM. **Candidate path:** the brief's "bus bypass" passes host accesses through to the BCM's memory-mapped peripherals (the LCD bus) while the BCM is powered down [external]. How it is entered here is unknown; the candidates are `GPO32` bit 15, which Apple uses in its hibernate sequence, or automatic entry in power-down [hypothesis]. Stage PW6 (`bcm2722-reference/brief-map-power.md` §8–§9) would turn this cell from "—" into S or X. As built (`p19-pw56`, not run) it is read only: with no register evidence for the entry, it writes nothing into a powered-down port; it reads the port raw and records whether the panel keeps its picture with the BCM cut, the precondition for any bypass use [ours].

**6. PP CPU → WM8758 audio data (I2S)** [silicon; rockbox-derived]. Music plays every session. In practice the CPU does not write the FIFO itself: DMA channel 0 does (cell 10). The PP is a clock slave: it cannot send a bit unless the codec's PLL runs.

**7. PP CPU → WM8758 registers (I2C)** [silicon; rockbox-derived]. Write-only, at most 4 bytes a transaction; our standby wake measured 6.4 ms median, 12.0 ms worst over 657 wakes (including a 2 ms PLL settle we chose) [silicon; measured] (`pp5022-reference/audio.md` §5).

**8. PP COP → SDRAM and IRAM** [silicon S36; measured]. The codec's decode ran on the COP for over an hour with no gaps: CPU idle rose from ~45 % to ~88–92 % with FLAC (`docs/fit2-brief.md` §12, on `s12`). Recipe R7. Gotcha found on the way: counters both cores wrote in cached memory lost counts (a line written back by one core undid the other's), so earlier `cop_idle_pct` readings may be stale [measured] (commit `b4e8fc5194`).

**9. PP COP → peripherals, the BCM port, I2C** [unknown]. The address space is the same, so a physical path exists [hypothesis]. Rockbox's design and ours keep the COP off every peripheral (interrupt handlers live on the CPU, and the COP has no tick) [rockbox-derived]; our job API forbids peripheral access in a job (`firmware/export/cop_job.h`) [ours]. *Settle:* a COP job that reads a harmless register (the BCM's `0x1000_1800` timer through the port, with the CPU idle) and logs it. Not worth trying until a job needs it.

**10. PP DMA channel 0 → I2S FIFO, from SDRAM** [silicon; rockbox-derived]. Every song. Recipe R9. Rate: the audio's own (176.4 KB/s at 44.1 kHz stereo 16-bit), in chunks of about 3.9 ms.

**11. PP DMA engines → IRAM** [unknown], except USB (cell 16). No session has pointed the general DMA, or the IDE controller, at IRAM. *Settle:* a channel-3 memory → memory copy (DP-2) with IRAM as source and destination.

**12. PP DMA channels 1–3 → SDRAM or PP peripherals** [dead end for memory → memory; measured S37]. Channel 3, memory → memory, moved **nothing** (E2-2: "untouched 1024 of 1024", with our FLAGS and with Apple's), while engine 2 copied the same buffers correctly (E2-1). So the general engine does not do memory → memory as we drive it; its proven use stays I2S (channel 0, a peripheral destination). Logs: `session log, session 37b bcmtest-results.log`.

**13. PP DMA channel 3 → BCM port (SRAM or SDRAM)** [dead end].
- Results: S8 "clean" (invalid: see D1); S32 77 % wrong (first word of each block missing, later words shifted); S33 word 0 of every block missing; **S34 and S35: nothing landed**, every size 256 B–32 KB, into the SDRAM FIFO (S34, S35) and into **SRAM** (S35) [silicon] (`session log, session 35 bcmtest.log`: "dma lab: SRAM, … 8192 of 8192 words wrong").
- Engine times (it does run): 58 µs for 256 B to 2.5–2.8 ms for 32 KB, about 12–13 MB/s [silicon S34, S35].
- "BCM periph" by DMA is [unknown]: never tried.
- Recipe and reasons: D1. Evidence: `bcm-devtest-hal.c` `dt_ppdma`, `bcm_video_wr_block` (DMA branch, behind a switch).

**14. PP DMA engine 2 → SDRAM and the BCM port** [silicon S37; measured]. Driven with Apple's sequence (`pp5022-reference/dma.md`, "The second DMA engine"), pattern-verified with guards: SDRAM → SDRAM correct (E2-1, 64 KB in 5.5 ms); into BCM SRAM with the BCM held after its bootstrap (E2-3) and with our image idle (E2-4): **all correct** at 64K/32K/4K/256 B, 0 guard hits; into BCM SDRAM (the bitstream FIFO window after OPEN, E2-7(1)): all correct. 64 KB takes 5.8 ms (~11 MB/s) against 4.1 ms for the CPU's copy at 80 MHz, but the CPU keeps **968/1000** of its time while the engine runs if it works from IRAM, **775/1000** if it touches SDRAM (bus sharing); with PCM running the same, I2S FIFO never starved. Completion: the command word's bit 31 falls (cc00… → 4c00…), status counts down to 0; IRQ 27 not used. The registers at `0x6000_9000–7F` alias `0x6000_9080–FF`. This is the offload path for the port (R1 alternative); the player backend is in progress (dev47+, `VPLAYER_TUNE_E2`). **Not yet run: the boot upload at address 0** (Apple's own use: a whole image in back-to-back 64 KB pieces while the BCM is held, then the start write). Stage 5s (`p14-sleep-spikes`) uploads a 192 KB block both ways and reads every word back before the start [unknown until it runs; `bcm2722-reference/open-questions.md` #51].
- **What Apple does** (static reading of osos 1.3, `pp5022-reference/dma.md` §6.3): every host-port write of 256 bytes or more, the BCM image upload included, goes through this engine. The CPU sets the BCM address with two halfword stores, writes the whole data cache back, then runs the engine in pieces of up to 64 KB (source = the SDRAM buffer, destination = `0x3000_0000`, command `0xCC00_0000 | (bytes − 4)`), sleeping on a semaphore until each piece's completion interrupt; the last < 256 bytes go by CPU halfword stores. No request line, no write-ready poll, no address re-sent between pieces.
- **Targets Apple's driver allows**: one side SDRAM; the other SDRAM, IRAM, or an external address such as the port, but **not** PP peripherals (`0x7xxx_xxxx`). So SDRAM → SDRAM is a legal control; "PP periph" is refused by the driver (a policy, not proven a hardware limit).
- **Which BCM memory**: the image upload lands in SRAM (`0xFC100` onward, with a header at `0xFC000`) while the BCM sits between the bootstrap and the start write [apple] osos `0x1028_7698`. The same write routine serves all of Apple's large port writes, so SDRAM targets are likely [hypothesis].
- *Settle:* the engine-2 port lab, §7.1 (`pp5022-reference/open-questions.md` #11).

**15. IDE DMA → SDRAM** [silicon; rockbox-derived for the registers, measured for the rates].
- UDMA2 negotiated (the adapter leaves IDENTIFY words 76 and 93 at 0) [silicon, every session].
- 21 MB/s reading a 64 MB file at UDMA2 **and** at forced UDMA4, 0 bad words over 8 passes (`udma test rc=0 bad=0 mbs2=21 mbs4=21`) [silicon S36c; measured]. About 22 MB/s from the flash during USB copies [silicon].
- Recipe R8. Evidence: `firmware/target/arm/pp/ata-pp5020.c` `ata_dma_setup`, `ata_dma_finish`, `ata_wait_intrq`.
- Gotchas: never read the ATA status register while DMA runs (it hangs; wait on `IDE0_CFG` bit 3) [rockbox-derived]; read buffers 16-byte aligned [rockbox-derived]; UDMA > 2 needs 80 MHz per transfer [rockbox-derived]. Completion by the IDE interrupt (source 23) is built but its silicon result is [unknown] (DP-4).

**16. USB DMA → SDRAM and IRAM** [silicon; rockbox-derived]. Queue heads and descriptors in IRAM (2 KB-aligned list at `0x4000_0000`), data buffers by physical SDRAM address, flushed or invalidated first. About 18.5 MB/s end to end while busy, after read-ahead [silicon, device logs 2026-10-12/13] (`docs/hardware.md`, "USB mass storage throughput").

**17. BCM → any PP memory** — no known path. The port is mastered by the PP; the BCM can only raise a line (its GPIO 23 to the PP's GPIO C bit 6) [rockbox-derived, apple-RE], and even that is unconfirmed on silicon (BCM open question #28). All data from the BCM reaches the PP by PP reads (R2).

**18. BCM core → its SRAM** [silicon S1 on; measured]. Scales exactly with the core clock (×5.56 from 27 to 150 MHz); an `add`/`ld` pair issues together, about 1.9 instructions a cycle [silicon S6, S7]. Gotchas (D5): misaligned accesses complete from the rounded-down address and then fault imprecisely [silicon S2, S3]; the 32-bit `ldsh rd, (rs+off)` zero-extends [silicon S17].

**19. BCM core → its SDRAM** [silicon S11; measured]. A 64 KB grid over 4 MB read back right; byte and halfword stores right when read back after all the stores, and the host reads the same words [silicon S36c] (commit `8093203dd5`, branch `p14-dev45`). Latency and bandwidth not measured (BCM #7). Open: a store followed **at once** by a load of the same SDRAM word (BCM #46, on `p14-dev45`).

**20. BCM core → its peripherals** [silicon; apple-RE for the register meanings]. Run control, DMA, cache, timer, clocks, GPIO, SDRAM controller, LCD, audio blocks (`memory-access.md` §5). 32-bit only. The 1 MHz counter at `0x1000_1804` is stopped after a host start until `0x1000_1C4C`/`0x54` are written [silicon S2, S4].

**21. BCM core → LCD panel by CPU writes** [silicon S4, S5]. Each pixel as a 16-bit value to `0x1000_417C`: 38.97–49.0 ms a frame, slower than DMA (`lcd.md` §6). Used as the fallback after a DMA timeout.

**22. BCM → WM8758 registers** [unknown], probably no path: Apple's BCM-side codec writer is an empty function and its codec-ID read returns a constant [apple] (`bcm2722-reference/audio.md` §1). Volume stays with the PP.

**23. BCM vector unit → SRAM and SDRAM** [silicon S5 on, S11 for SDRAM; measured]. Every path's alignment rules, known and unknown, are in `bcm2722-reference/memory-access.md` §11 "Alignment" (scalar, vector, the BCM's DMA, the host port), with a bench row ALGN check for each unknown. `vld16`/`vst16` at REP 16 take 16.63/16.50 cycles (≈ 31 bytes a cycle) in SRAM [silicon S7]. Alignment and register-field rules in D5. Whether the vector unit goes through the data cache is unknown (BCM #6), which is one more reason to keep it off.

**24. BCM DMA channel 1 → LCD FIFO (from SRAM)** [silicon S5–S7; measured, following apple-RE values]. A 320×240 frame in 14.26 ms (bound by the 27 MHz LCD bus, not memory); a 100×50 rectangle in 2.05 ms; back-to-back frames, rectangles and command writes after a run all correct. Recipe R5. Evidence: `bcmfw/dma.c` `dma_lcd_start`, `dma_lcd_wait`; `bcmfw/lcd.c` `lcd_blit`, `lcd_blit_wait`.

**25. BCM DMA → or from SDRAM** [silicon PW8 for channel 7's memcpy; unknown for the LCD channel] (BCM #13). Apple's TV and memcpy paths use it [apple]. PW8 (2026-10-25): channel 7 sub-channel 1 with Apple's `dma_memcpy` words copied 16 KB SRAM → SDRAM in 383 µs and SDRAM → SRAM in 575 µs, 0 wrong words, against the core's 1,497 and 1,032 µs (also 0 wrong) [silicon PW8] (`bcm2722-reference/brief-map-power.md`, PW8). Used by R13. *Still open:* the LCD channel's source in SDRAM (photograph the panel).

**26. BCM DMA channel 5 → audio FIFO `0x1000_4804` (from SRAM)** [silicon S35; measured, following apple-RE values]. The tone test looped a 35 KB SRAM buffer for 3,000 ms twice ("played 3000 ms") [silicon S35 bcmtest.log]. Length unit [hypothesis: bytes] (BCM #38).

**27. BCM audio block → WM8758 DAC** [silicon S35, partial]. With GPIO 1–3 on function 2 and the codec awake, the block reported ready (control word `0x0092_0001`) and the owner heard a **full-scale screech** in the headphones (commit `2588e7146a`). So the BCM's data does reach the DAC by some route [silicon]; a clean tone has not yet been heard, and how GPIO 1's net meets the PP's data line is unknown (BCM #39). See D6 and R6.

**28. BCM DMA channels 6 and 7** [apple]. TV (FIFO `0x1000_2C04`) and `dma_memcpy` (channel 7 sub-channel 1, 32-byte units). Never run by us.

**29. WM8758 clocks → PP I2S and BCM GPIO 2/3** [silicon S32–S34; measured]. While PCM plays at 44.1 kHz: GPIO 3 at 88,198 edges/s (2·fs, LRCK), GPIO 2 at 257,000–352,000 edges/s (the bit clock, aliased by our ~630 k samples/s) [silicon S32, S33, S34, S36c]. **No clocks** when PCM is not running and the codec's PLL is off (DAC standby) or stopped (S32 quiet probes; S36c "playing" probes saw 0 edges because the soundtrack never started: D6) [silicon].

## 5. Recipes

Each recipe is what worked on our unit, written so it can be implemented from this page. Register meanings carry their own tags.

### R1. PP CPU → BCM (SRAM or SDRAM) through the port, unboosted, with a priority band

1. **Registers** [rockbox-derived; silicon]: `DATA` `0x3000_0000` (16/32-bit, auto-incrementing), `WR_ADDR` `0x3001_0000`, `RD_ADDR` `0x3002_0000`, `CONTROL` `0x3003_0000`. Only PP address bits 16–18 select the register, so any low address within each 64 KB step works. Do not use the ALT bank (`0x3004_0000`+) after the bootstrap: what it addresses is unknown (BCM #25).
2. **Set the address**: one 32-bit store of the BCM address to `WR_ADDR` (the bus sends it as two 16-bit cycles, low half first). Apple's own driver instead writes both halves as two halfword stores to `0x3001_0000`, low then high, and never polls `CONTROL` on writes [apple-RE]; `e2port_start` writes the high half to `0x3001_0002` (the same register, by the bit 16–18 rule). Ours is kept: bench row CR1 (2026-10-25, session log) wrote the control area and all 7 slot pools after the start with 0 wrong words, by ours and by each of Apple's forms [silicon]. **Host writes into the pools work after the start** (0 of 7 wrong); open question #67's `COMMIT` failure was the BCM's sum (`bcm2722-reference/quirks.md` #33).
   - **Alignment rule** [silicon: inferred from CR1's 301×7 slot, then read directly by ALGN PWD2/PHD2/PAD2 in sitting 2, 2026-10-25]: the write address ignores bits 1:0, in the word, halfword and Apple's halfword-address forms alike. **A lone trailing halfword is dropped** [silicon] (ALGN PLH1/3/5: the port commits halfwords in pairs), so never end a write on an odd halfword store; a word with a zero high half lands (PLW3). Start every write (CPU block, word, PP DMA, engine 2) at a word address; a row whose first pixel sits at 2 mod 4 must merge that pixel into its word (read, modify, write), and an odd last pixel likewise, never a word that spills into the next row. Our images round every slot's pitch to 16 pixels (`J_SLOT_PITCH`), as Apple does [apple-RE]; the PP's port layer refuses a mid-word write (`bcm-devtest-hal.c`, `e2-port.c`).
3. **Wait for write-ready**: poll `CONTROL` bit 1 (`0x02`). **Bound the wait** (we use 20 ms) [ours]: an unpowered BCM reads `0xFFFF` per halfword, and an unbounded wait hangs the PP [silicon; ours]. The rule extends to every wait on the chip, each with a FAULT line when it expires (sitting 4's three silent freezes; `bcm2722-reference/host-driver.md` §2a). Under load the longest wait seen was 47 µs; writes typically 2–6 µs [silicon S12].
   **The BCM address must be 4-aligned:** the port ignores its bits 1:0, so a write starting at 2 mod 4 lands 2 bytes low [silicon, inferred] CR1 2026-10-25 (`bcm2722-reference/memory-access.md` §11 "Alignment", the rules for every path; quirk #34). Apple's callers never write a 2-mod-4 address (4-byte pitches, whole-row bands, 16-byte file pieces) [apple-RE].
4. **Stream the data** to `DATA` as 32-bit stores; each becomes two 16-bit transfers and the address auto-increments. A 2-register `ldm`/`stm` loop is as fast as anything (8-register bursts gave nothing) [silicon S8]. Source word-aligned; length a whole number of words. One address per contiguous run: a separate address per row costs 7.4 µs [silicon S8].
5. **Pad, don't split**: writing whole words overwrites up to 3 bytes past a packet's end; start each packet 64-byte aligned so that is harmless [ours] (`memory-access.md` §7a).
6. **Verify cheaply**: read back the first and last word of each piece (R2) and count wrong words [ours] (`apps/video/bcmvp.c` `verify_word`).
   - **Rule: write packets ahead of their `FEED`s, and before the image reads SDRAM bytes just written, read the newest packet's last word back.** A `FEED` sent the moment its packet was written gave no picture at every band of sitting 4b's first-use check [silicon, 2026-10-26]; the same sequence in vc2sim reproduces it only if the port's SDRAM writes can trail an SRAM command written after them [sim; hypothesis: posted writes] (BCM open question #73, which also names the other candidate). The player's packets go in well ahead of their `FEED`s; the check now reads each packet's first and last word back before any `FEED`, and `bvp_feed` reads back the newest packet's last word when it is posted at once (one read) [ours]. The alternative candidate for the cause is the merged image's first `OPEN` reading the FIFO's first 8 KB from the core before the host writes it (batch 4's `bschk_latency`); a core-side line kept from that read would also explain the symptom. The host-test model of the chip refuses a `FEED` of bytes not yet behind such a read (`VBM_LAND_US`), so the failure shows on the Mac.
7. **Clock and scheduling**: the copy costs about 10 PP cycles per 16-bit transfer at any clock [silicon S8], so a 30 fps, 768 kbit/s stream needs about 0.6 ms of port time a picture unboosted [estimate, ours]. What starves a feed is **priority, not frequency**: a feed thread below the codec's priority ran up to 2.1 s late (S31); placed in its own band it held 16 ms of drift with the PP unboosted (S34) [silicon]. Our band: the feed thread at priority 14, between the codec (13) and buffering (15) (`firmware/kernel/include/thread.h` `PRIORITY_VIDEO_FEED`; commit `5228749847`) [ours]. Boosting is an A/B arm, not the default [ours] (commit `ccd6007bc8`).
8. **At 24 MHz** the port is about 20 % slower; our clock policy holds the PLL while the screen is lit (`docs/fit1-brief.md` §C) [ours].

### R2. PP CPU reads from the BCM

1. Wait for `RD_ADDR` bit 0 (ready for an address); store the BCM address to `RD_ADDR`; wait for `CONTROL` bit 4 (`0x10`, data ready); read `DATA` [rockbox-derived; silicon]. Our port sets the address for **every** word; whether reads auto-increment is untested (BCM #43).
2. Cost: under load the read-address wait was 35–38 µs and the read-data wait 20–47 µs [silicon S12]; idle cost per word is estimated at 0.5–1 µs [hypothesis]. During a video session at 150 MHz the first-use check's 864,000 single-word reads took about 7.7 s: **about 8.9 µs a word** [silicon, sitting 4c, derived from the check's 7993 ms less its decode and set-up], so the check now sums a sample of rows (first-use-check.md). Read as little as possible.
3. **Torn words**: a 32-bit word the BCM rewrites while the PP reads its two halves can mix old and new halves [silicon S11, S12]. Store anything the other side polls as a 16-bit value with its complement in the high half, and treat a word whose halves disagree as "not yet", never as "dead" [ours] (`HP_ENC`/`HP_VALID` in `bcmfw/hostproto.h`).

### R3. Starting the BCM (the PP writes SRAM before the core runs)

Summary only; the full sequence is `bcm2722-reference/host-interface.md` §1 and `pp5022-reference/display.md` §3 [rockbox-derived; silicon every session]: power on (`GPO32` bit 14), wait 50 ms; the bootstrap byte sequence on `CONTROL` and `ALT_CONTROL`; upload the image from SRAM address 0 by R1; write `0x1F8 = 0`; flush the BCM cache (`0x1000_0C00 = 0xC000_0000`, poll bit 0, write 0); start with `0x1000_0400 = 0xA5A5_0002`. The core enters at `0x200` at 27 MHz with its 1 MHz counter stopped [silicon S1, S2, S7]. Engine 2 in place of R1 for the upload is the stage 5s spike (cell 14). A 93 KB image is ready within the 0.37–0.62 s boot; the panel's first frame came 279.8 ms after power-up in S30–S35 [silicon]. Every power cut loses everything.

### R4. BCM SDRAM bring-up (needed before cells 3, 19, 23-SDRAM)

Summary of `bcm2722-reference/sdram.md` [apple-RE values; silicon S10, S11 with our deviations]: run after the switch to 150 MHz (or at 27 MHz with the refresh word scaled); refresh word `+0x24[31:16]` = 7.8 µs in core cycles (`0x0493` at 150 MHz, `0x00D2` at 27 MHz); timing words; cache flushed and off; `+0x30 = 0x80E8`; 200 µs; mode word then command 7 at `+0x20`; the cache stays **off** afterwards even when a cache-on attempt passed (dev46); **a poll timeout is not fatal**: a geometry probe at `0xFE00_0000`/`0xFE00_0200` decides; on failure reset the controller and retry with the other mode field. **Field 8 fails on our unit at both clocks; field 4 passes on the second attempt** [silicon S10, S28–S35]. Raise the refresh word after a clock switch up, lower it before a switch down [ours].

### R5. BCM LCD DMA and its wait discipline

Register meanings are [apple-RE] values made to work on silicon S5–S7 [measured]; `dma.md` and `lcd.md` §6 have the tables.

1. **Window**: send the panel's window commands and the RAM-write index by CPU (panel 3: registers `0x210`–`0x213`, `0x200`, `0x201`, then index `0x202`).
2. **Control block** (32 bytes, 32-byte aligned): next = 0; source = the frame in **SRAM**, word-aligned (32-byte aligned for bursts); destination = LCD FIFO `0x1000_400C`; length in units of the narrower width (4 bytes = 2 pixels; 38,400 for a full frame); transfer information = enable bit 0, destination width 4 bytes (`[14:12]` = 2), destination paced (bit 15), interrupt-on-done (bit 24), uncached (bit 27), peripheral (bit 28), source width (`[6:4]` = 5 for 32-byte bursts when aligned, else 2). For a sub-rectangle: bit 16 (2-D), length = `rows << 16 | w/2`, skip word = `(pitch − w)/2` **as a gap, not a pitch**. x and w must be even.
3. **Channel 1**: config `+0x00 = 0x0903_2101` (enable, DREQ 3, Apple's other bits kept); **clear the whole status word** `+0x04 = 0xFFFF_FFFF` so a stale bit cannot end this wait; start sub-channel 0 by writing the block's address to `+0x38`.
4. **Arm the LCD interface**: `0x1000_4000 = 0x231 | b<<8`; `0x1000_4004 = pixel count`; `0x1000_4008 = 0x1F`; `0x1000_4000 = 0x229 | b<<8`, with b = 1 when using 32-byte source bursts.
5. **Wait** for status bit 8 (`0x100`) on `+0x04`. Bits 0–7 are **not** completion bits; after a full frame the word reads `0x2300` [silicon S4, S5]. Bound the wait (we allow 100 ms, polling every 64 µs).
6. **Finish**: clear the status word, then `0x1000_4000 = 1` (back to command mode).
7. **Discipline** [ours]: the transfer may overlap other work, but the next blit, **any** LCD command and the end of a session must first complete the wait (`lcd_blit_wait`). Don't rewrite the source while it runs: DMA reads it live, so a rewrite tears the frame (no TE wait in our images; no tearing seen by eye) [silicon S7]. On a timeout, stop the channel (`+0x00 = 0`, clear status, `0x1000_4000 = 1`), re-send by CPU, and keep DMA off until the next power-up.

### R6. BCM audio: DMA channel 5 and the audio block as a clock slave

**Status: partly proven.** S35's run reached ready and made sound (a screech); S37 on dev45 (clocks in first, data pin after ready, codec held out of standby) gave a recognisable tone with some noise on layout 2 [silicon S37; owner by ear]. Step 1 also needs **the PP's own PCM playing** (silence is enough) for the whole run, not only standby off: clocks without PCM are unproven (dev46 plays silence through the tones). Register values are [apple-RE] (`bcm2722-reference/audio.md` §2–§3).

1. **The codec must be awake.** Keep the WM8758's PLL running for the whole test (no DAC standby), or there are no clocks (R9).
2. **Listen first**: GPIO 1–3 as inputs; count edges on GPIO 2 and 3 for 3 ms (GPIO level register `0x1000_2820`). Go on only if GPIO 3 shows the frame clock.
3. **Clock pins**: put GPIO 2 and 3 on function 2 (2 bits per pin at `0x1000_2800`).
4. **Block start-up and rate**: `0x1000_4800 = 0x6001`; `+0x10 = 1`; `+0x14 = 0xA508_0010`; `+0x00 = 0`; then `+0x14 = 0xA508_0000 | div`; `+0x00 |= 1`; wait for **bit 23** of `+0x00` (Apple waits up to 65 ms). Bit 23 set means "external clocks seen" [hypothesis, strong]: it set with GPIO 2/3 on function 2 (S35, `0x0092_0001`) and never with them as inputs (S36c, `0x0013_0001`) [silicon].
5. **Data pin last**: only after bit 23 put GPIO 1 on function 2. GPIO 1 floats in every PP state (it follows the BCM's own pulls), so it is not on the PP's data-out net [silicon S34, S36c].
6. **DMA**: channel 5 config `+0x00 = 0x0607_6002` (DREQ 7); a chain headed by a 16-byte zeroed block whose next pointer is itself, so an underrun loops on silence; data blocks with transfer information `0x1000_A041` (+ bit 24 for an interrupt); start sub-channel 0 (`+0x38`), then `0x1000_4800 |= 0x6209`.
7. **Stop**: `0x1000_4800 = 0`; channel `+0x00 = 0`; pins back to inputs (Apple parks GPIO 1 as an output instead).
8. **Safety** [ours]: cap the level (our image caps tones at −40 dBFS and turns the codec down), and never drive GPIO 2/3 as outputs: they carry the codec's clocks.

### R7. Sharing PP memory between the CPU and the COP

Rules [measured on silicon S36 for our job channel; the cache facts are rockbox-derived]:

1. **Small shared state** goes in IRAM or the uncached alias (`0x1000_0000 + logical`); link it there (our `SHAREDBSS`) so no cache operation is ever needed. Locks: Peterson's algorithm on bytes in uncached memory; `SWP` across cores is unproven (PP #8).
2. **Bulk buffers** cross by pointer, with one cache operation at each hand-off, never per element. Our table (`firmware/common/cop_job_core.c` `copj_cache_ops`, host-tested):

| Point | Core | Operation | When |
|---|---|---|---|
| post | poster (CPU) | write back | the job reads data the poster wrote, or code it loaded |
| start | worker (COP) | write back, then drop (instruction cache too for code) | same |
| finish | worker | write back | the job wrote data the poster reads |
| collect | poster | write back, then drop | same |
| call | worker | write back | always: the handler touches the worker's memory through the uncached alias |
| return | worker | write back, then drop | the handler wrote the worker's cached memory |

3. **Never a bare discard**: every drop writes back first, or the core loses its own dirty lines, its stack among them [ours].
4. **Write back** = set the flush bit in `CACHE_OPERATION` (`0xF000_F044` bit 1), wait for `CACHE_CTL` (`0x6000_C000`) bit 15 to clear [rockbox-derived]. **Drop** = rewrite every one of the core's line status words (16 bytes apart over 8 KB, at `0xF000_4000` for the CPU, `0xF000_6000` for the COP) to "valid, clean, unused tag", with interrupts off, from IRAM [rockbox-derived]. Cost per call not measured (fit2 risk R2).
5. **Reading the producer's data uncached** is often cheaper than a drop: the codec's bitstream ring and PCM cross through the uncached alias at 10–100 KB/s [estimate, ours] (`docs/fit2-brief.md` §3.3).
6. Clock: `cpu_boost` changes both cores; a change stalls both [rockbox-derived].
7. **Pixels across cores (BCM MF-4, P19 19-9)** [ours; host model `tests/lcdxcore`; not yet run on silicon with the UI on the COP]. The framebuffer has one producer, the core that draws the shell (`lcd_fb_claim()` on the ui thread). A push of that frame from the other core (the CPU's wake frame in `lcd_awake`, the devtest's hand-back, vplayer's overlay) needs, in order:
   - the producer's write-back, asked for by the consumer (`fe_fb_publish()`, bounded at 50 ms; answered at once when the ui thread is blocked in its input wait or a back-end call, since it wrote back first) **before the LCD port lock** is taken, since the ui thread may be waiting for that lock; or, where the producer then waits on the consumer (the overlay), published by the producer before it posts (`lcd_fb_publish()`);
   - every read through the uncached alias (`lcd_fb_read()`, the default frame `0x1000_0000 + fb`), so the consumer never reads a stale line of its own; engine 2 already reads the uncached alias.
   - Edges: rows are 640 B = 40 whole lines and the frame is 16-byte aligned, so a full-width rect (all engine 2 pushes) is line aligned at both ends; a narrower rect's edge lines are only read, so the shared neighbours do no harm.
   - A fence that times out (a modal loop on the ui thread) costs stale pixels in that one push, never corruption: the consumer only reads.
   - Code: `firmware/target/arm/ipod/video/lcd-fb-xcore.c`, contract in `firmware/export/xcore.h`; developer builds only (`HAVE_FE_FB_HANDOFF`).
8. **Audio output across cores (P19 ucop-fix, 2026-10-26)** [ours; the fault: silicon, bench sitting 3; the mechanism: code reading and the host model `tests/audioxcore`, measured]. The mixer (`pcm_mixer.c`) and the PCM driver (`pcm-pp.c`) are **CPU-only code**: the playback DMA's interrupt is the CPU's FIQ, the PCM lock (`sink_lock()`) masks that FIQ and counts with a non-atomic increment, and the mixer's control words (`downmix_index`, `next_size`, `idle_counter`, `mix_frame_size`) are cached `.bss`/`.data` that the FIQ keeps hot in the CPU's cache. Rules:
   - nothing on the COP starts, stops or feeds a mixer channel or the PCM: a sound the UI asks for on the COP is posted to the CPU (`system_sound_play()` → `ipodui_sound_route()` → `be_post(BE_SOUND)` → `be_svc`). `mixer_channel_play_data()` and `mixer_channel_stop()` carry `XCORE_ASSERT_CPU` (developer builds count `xcore_offcore`; the ucop2 bench row fails on it);
   - a driver both cores call under a cross-core mutex keeps its shared state **uncached too**: the mutex orders the cores but does not make their caches agree. The WM8758 driver's shadows and standby state (`wm8758.c`, `dac_state`, `pwr_reg`, `dac_busy_tick`, …) are `XCORE_BSS`/`XCORE_DATA`;
   - a word a poster writes for a thread on the other core to read (the piezo's click length) is uncached with the queue it posts on (`piezo.c` `duration`);
   - single-word status reads the UI makes every pass (`audio_status()`: `play_status`, `quiet_restart`; `pcm_is_playing()`) are uncached words with one writer and stay direct calls (`febe_rpc.list` "direct:"), not a remote call per read.

### R8. ATA DMA (UDMA2), the PLL hold, and the UDMA4 experiment

Register meanings are [rockbox-derived]; the rates and the hold order are [measured] (`pp5022-reference/storage.md` §1–§3).

1. **Timing**: write `0xC000_0000` to `0x6000_60C4` (IDE clock), clear `IDE0_CFG` bit 28, set `IDE0_PRI_TIMING1` (`0xC300_0004`) to the mode's word (UDMA2 `0x8000_3371`, UDMA4 `0x8000_3071`), set `IDE0_CFG` bit 29. Send SET FEATURES with the mode (`0x42` UDMA2, `0x44` UDMA4).
2. **Before each transfer**, in this order [ours]: (a) hold the PLL so the clock is at least 30 MHz (UDMA2 is rated at 30 MHz; 24 MHz is too slow); (b) **then** boost to 80 MHz if the mode is above 2. The hold first makes the boost start from 30 MHz, so the 24 MHz policy does not count a disk transfer as a codec rescue (S36 counted ~56,000 such boosts in two minutes of a library scan and wrongly struck FLAC) [silicon S36] (commit `d4dcaf1287`; `ata-pp5020.c` `ata_dma_setup`).
3. **Cache**: write back before a write; write back and drop before a read. Read buffers 16-byte aligned, writes 4-byte aligned.
4. **Arm**: clear old INTRQ (`IDE0_CFG |= 0x8`); `IDE_DMA_CONTROL` (`0xC300_0400`) `|= 2`; `IDE_DMA_LENGTH = bytes − 4`; `IDE_DMA_ADDR` = **physical** address (logical + `0x1000_0000`); `IDE_DMA_CONTROL` bit 3 = 1 for a read; `IDE0_CFG |= 0x8000`.
5. **Command**: issue the ATA READ/WRITE DMA command with nIEN clear, then set `IDE_DMA_CONTROL` bit 0 to go.
6. **Wait** on `IDE0_CFG` bit 3 (write 1 to clear), **never** on the ATA status register. We sleep on the IDE interrupt (source 23) in 2-tick slices that re-check bit 3, and fall back to polling after three missed interrupts [ours]; whether the interrupt fires on our unit is [unknown] (DP-4).
7. **Finish**: clear `IDE0_CFG` bit 15 and `IDE_DMA_CONTROL` bits 31 and 0; unboost; release the hold.
8. **UDMA4** (developer switch only): works on our adapter (0 bad words, 64 MB × 8) but reads at the same 21 MB/s as UDMA2, and costs a boost per command [silicon S36c]. Keep UDMA2 (D3).

### R9. I2S playback and its clock dependencies

Diagram: [`diagrams/data-paths-audio.svg`](diagrams/data-paths-audio.svg) (data, clocks, I2C control, and the outputs: OUT1 to the headphone jack with GPIOA bit 7 detect, OUT2 to the dock line out, not on this unit).

Register meanings [rockbox-derived]; behaviours marked [silicon] were seen on our unit (`pp5022-reference/audio.md`, `dma.md` §2).

1. **The WM8758 is the clock master**: its PLL turns the PP's 24 MHz external device clock (its MCLK) into BCLK and LRCK (CLKCTRL `0x145` for 44.1/48 kHz, master bit set). The PP's `IISCONFIG` (`0x7000_2800`) master bit 25 must stay 0, or both chips drive the clocks.
2. **PP I2S block**: reset (bit 31 set, then clear); I2S format, 16-bit, FIFO format 7 (one 32-bit write = one stereo frame, left in the low half); `IISFIFO_CFG` TX request level 4 free, clear both FIFOs; set `TXFIFOEN` (bit 29).
3. **DMA channel 0**: enable the engine (`0x6000_A000` bit 31); route the DMA interrupt to FIQ; set request bit 2 in `0x6000_A008`; `DMA0_PER_ADDR = 0x7000_2840`; flags bit 26; increment = fixed address, 32-bit (`0x2001_0000`); per chunk: `DMA0_RAM_ADDR` = physical buffer address (after a cache write-back), `DMA0_CMD = START | INTR | WAIT_REQ | SINGLE | RAM_TO_PER | request 2 << 16 | (bytes − 4)`. Reading `DMA0_STATUS` acknowledges the interrupt, so read it only in the handler.
4. **Stopping DMA does not stop the transmitter**: `TXFIFOEN` stays set and the PP keeps shifting an empty FIFO while the codec's clocks run [rockbox-derived; silicon S34].
5. **Our DAC standby** (Phase 15) [ours; silicon]: when PCM has been stopped for 5 s (after the mixer's own 3 s silence tail, so about 8 s after a pause), the codec goes to standby: soft mute, then outputs, mixers, DACs and **its PLL off**, VMID kept charged. **With the PLL off there is no BCLK or LRCK** for the PP or the BCM (S32 and S35 quiet probes: 0 edges). After playback stops, the codec parks both clocks high [silicon S34]. Any PCM start wakes it in 6.4 ms median, 12.0 ms worst [silicon]. Never turn the PLL off while DMA runs: the stop path waits for the TX FIFO to drain, which needs the clocks (`docs/phase15-brief.md` §3.2).
6. **Anything that needs the I2S clocks** (the BCM's audio block, a probe of GPIO 2/3) must first hold the codec awake (our `dac_standby_allow(false)`) and confirm PCM is really running.

### R10. USB transfers

Summary [rockbox-derived; silicon every USB session] (`pp5022-reference/usb.md`): queue heads and 32-byte, 32-byte-aligned transfer descriptors (≤ 16 KB each) in IRAM, which has the same address physically and logically; data buffers by physical SDRAM address after a cache write-back or drop. The PHY sequence must follow every controller reset on a PP5022. Hold the PLL while the controller is enabled [ours] (commit `eb3f8d49f6`).

**Rules for USB reads** [silicon]: keep ATA DMA and bulk-IN DMA from overlapping (D12), and load each 64 KiB half before it is sent (D13). A static, page-aligned, uncached INQUIRY reply buffer is required (`pp5022-reference/usb.md` §6). The display's E2 path stays on (D14).

### R11. Durable write order on the ATA drive (the volatile write cache)

`ata.c` enables the drive's volatile write cache at init (SET FEATURES `0x02` when IDENTIFY word 82 bit 5 says it exists) and, upstream, issues FLUSH CACHE (`0xE7`, or `0xEA` with LBA48) only before standby and at shutdown [rockbox-derived]. So the order in which write commands complete is **not** the order in which sectors reach the media: at a power cut the drive or adapter may land any subset of what it holds [hypothesis] (ATA's model of a volatile cache; not yet measured on our iFlash adapter or an iPod hard drive). Every crash-ordering rule of the file layer depends on this.

**Recipe** [ours; sim] (#91 H2, `firmware/common/fat.c` `FAT_WRITE_BARRIER`, shell build only):
1. A **barrier** is `ata_flush()` (FLUSH CACHE), issued only when something was written since the last one.
2. **Automatic**: between FAT sectors and directory sectors, in either direction. FSInfo is a hint and does not count. This orders **separate commits** only: within one disk-cache commit the write-back puts every FAT sector first (`DC_ORDERED_WRITEBACK`), which is right for an allocation (durable before an entry names it) and wrong for a free. So a step where a directory sector must land **before** a FAT change commits it on its own first: `fat_remove()` commits the cleared entries before it frees the chain, and `fat_closewrite()` of a file emptied to size 0 rewrites its entry (no cluster), commits and flushes it, and only then frees the first cluster (#91 M1, `FAT_ENTRY_BEFORE_FREE`; a cut between leaks one cluster instead of leaving the entry on a free one, which a later allocation would cross-link). Truncating to a non-zero size keeps the first cluster: a cut leaves the old size on a shorter chain, which fsck trims.
3. **Explicit** (`fat_barrier()`): after the long-name sectors and before the short entry; after a rename's new entry and before the old one is cleared; after a short entry is cleared and before its long-name entries; after a fresh directory cluster is zeroed and before it is linked; in `file.c` after the rename intent record (which also makes the source's data durable), after an overwrite's victim is removed (#91 H1), before the record's done-mark, and at the start of every `remove()` (a caller's marker written just before is durable before a name goes).
4. **Cost** [sim]: a rename or move is 3 barriers, an overwrite 4, a settings save 8 (safetest's counts); a file write that allocates adds up to 2, a remove 1-2. Device estimate [hypothesis]: a few ms each on the flash adapter, about 10-20 ms on a hard drive (it is spinning anyway when these run). The device log lines `usb ata w85=… w86=… flush n=… fail=… nosup=…` and `usb ata flush max=…us avg=…us` (written at USB entry) measure it.
6. **Failures are counted, not hidden** [ours] (#91 M2): `ata_flush()` returns the FLUSH CACHE error (< 0) and 1 when the drive offers no flush command (`canflush` cleared from IDENTIFY words 80/83: every barrier is then a no-op and none of the orders above hold). Both are counted in `ata_flush_stats` (`fail`, `nosup`), printed in the USB line above, and written once per boot by the shell's log (`ata barrier flush fail=… nosup=… of n=…`). A failed flush is not retried and no write is refused: the cost of a missed barrier is the pre-#91 crash exposure, not data loss on a clean run. SCSI SYNCHRONIZE CACHE now reports a failed flush to the host.
5. **Proof** [sim]: `tests/fatcrash/safetest` with `SAFETEST_REORDER=1` models the drive cache (every cached sector lands or is lost on its own at a cut, 6 outcomes per cut point): all sites pass with the barriers; without any, with only the automatic ones, or without the victim barrier, the run fails (`tests/fatcrash/run.sh`). The sites `trunc-empty` (ftruncate to 0) and `otrunc-temp` (an abandoned `O_TRUNC` temp over `O_TRUNC_THRESH`) fail the chain check with the old free-then-entry order (`FAT_FREE_BEFORE_ENTRY`), strict and reordered.

Open [unknown]: whether our flash adapter reports a volatile cache (word 85 bit 5) and honours FLUSH CACHE, and what one costs; PP open question #32. A non-zero `nosup` in the USB line answers the "offers none" half; `fail` that it errors.

### R12. Drawing for games and Cover Flow (planned; the 3D render test is the first engine)

[estimate; ours] The scene lives on the chip, and the PP sends a command list each frame. `bcm2722-reference/graphics.md` gives the figures and the model.
- **Textures:** upload them once by engine 2 (cell 14: ~11 MB/s, with the CPU keeping 97 % of its time from IRAM) into texture slots in BCM SRAM (about 0.9 MB free beside a compositor) or SDRAM (about 3.5–4 MB: video never runs beside a game; R4 first).
- **Each frame:** a few KB at most through the port (R1), then the chip composes and pushes the dirty rectangle (R5). Full screen: 14.26 ms of panel-bus time [silicon S7], so at most about 70 fps, and 30 fps uses about 43 % of the bus.
- **Completion:** the host polls (no chip-to-host interrupt is confirmed, `open-questions.md` #28), so the PP's cores idle between steps.
- **Do not** push full frames from the PP for full-motion scenes: that costs 27 ms of CPU at 30 MHz each (R1), nearly the whole 30 fps frame.
- **Open:** the tearing signal (#52), a push from any source and pitch (#53), texel fetch cost (#54), SDRAM bandwidth (#7), port contention (#44) and power (#12).
- **The first engine** (`bcmfw/r3d`, the 3D render test, device stage 3d; `research/bcm2722/render3d.md`) [ours; simulator only]:
  - it generates its scene and textures on the chip, so the port carries only a parameter block per run;
  - it renders in SRAM alone, SDRAM off, into two framebuffers in turn;
  - each frame goes to the panel by `lcd_blit` (the chip's LCD DMA, [silicon] through the video player) while the next renders.

### R13. The staged upload: a slot's pixels by SDRAM and the BCM's own DMA (jobs rev 0.5; [sim] only, withdrawn from the images 2026-10-25)

**Withdrawn.** Its premise, that host writes into the slot pools fail after the start, was wrong: CR1 wrote all 7 pools with 0 wrong words [silicon 2026-10-25], and `COMMIT`'s failure was the BCM's sum (`bcm2722-reference/quirks.md` #33). Kept as a recipe for a slot that must come from SDRAM.

**Why** [hypothesis, refuted]: the job `COMMIT` failed on silicon on both images and both PP write paths (open question #67). Host writes after the start were proven only into the rings, the host frame and SDRAM (cells 2 and 3); every slot pool is elsewhere in SRAM, mostly below `0xD0000`.
1. **PP:** `ALLOC` with `J_ALLOC_F_STAGE` (sixth word), only when the image's `J_CAPS` has the magic and `J_CAP_STAGED`. The reply's address is the window, `0xC03A_0000` (host view of SDRAM `0x3A0000`), at the slot's pitch.
2. **BCM, first staged `ALLOC`:** SDRAM up (R4) in UI mode; a failed bring-up answers the rev 0.4 way (the slot's own address) and sets `J_CAP_SDRAM_FAIL`.
3. **PP:** the rows into the window by R1 or engine 2 (cells 3 and 14: both proven into SDRAM), then `COMMIT` with the F16w, as before.
4. **BCM, `COMMIT`:** the window (`0xFE3A_0000`) into the slot by DMA channel 7 sub-channel 1, config `0x0000_E802`, a 32-byte-aligned control block {next 0, source, destination, length in 32-byte units, transfer information `0x0102_5151`}, start by writing the block's address to `+0x58`, done on status bit 14 (`0x4000`), in 16 KB pieces each bounded at 100 ms (cell 25, PW8's exact words); after a timeout, the core copies every word (PW8's other 0-wrong copy). The merged image copies by the core only (no room for the DMA's code). Then the slot's F16w.
5. **Reply:** `A, B, how`; on `J_E_SUM`, the BCM's sums, the slot's first word, how and the window's B: the upload, the move or the sum, told apart (`jobs.md` §4).
- **Cost** [silicon PW8 rates; estimate for a cover]: 6.4 ms by DMA or 11.5 ms by the core at 27 MHz per 300 × 300 upload, plus SDRAM's refresh from the first upload on (power unmeasured).
- **Evidence:** `bcmfw/video/jobs_bcm.c` (`stage_copy`, `stage_before`, `stage_after`), `bcmfw/jobs/jstage.h`, `firmware/target/arm/ipod/video/bcmjob-core.c` (`alloc`, `job_fail`); vc2sim rows `jobs_image_test.py` "staged upload" (both candidates); host test `tests/bcmjob` `t_staged`; device: bench row CR2 (`research/bcm2722/bench-p19.md`).

### R14. An overlay into the image's region (the visualizer's `viz-ovl.bin` and the merged image's two overlays; [sim], host-tested; not yet run on silicon)

A second program, linked at the image's overlay region against that image's map, written by the PP and started by the BCM (`research/bcm2722/jobs.md` §5 "Overlays", `visualizer.md` §8). Every step runs on the job owner's thread.
1. **Is one wanted.** `J_OPS` bit 21 (`OVERLAY`) set and the overlay's ops (bits 19, 20) clear, and no load refused in this epoch. **The merged image has offered the mechanism since batch 3** (it accepts any file whose header carries its ABI word and region; an older statement here, that it never sets bit 21, is withdrawn), for DECODE (`dec-ovl.bin`) and the visualizer (`viz-ovl-m.bin`, linked against the merged image's map).
2. **The file against the build.** The firmware carries a record of the `viz-ovl.bin` built with its embedded display-only image (size, FNV-1a, header ABI word, the byte offset of the image's own copy of that word). The file's first 7 words are the header: magic `'OVL1'`, the ABI word, base, load end, `.bss` end, `init`, `exec`. Refuse a size, magic or ABI word that differs (the file is another build: never again this boot).
3. **The header against the region.** The job block's `J_OVL` (`+0x48`) = base | size >> 12. The header's base must equal it, its load end = base + the file's bytes, its `.bss` end inside the region.
4. **The running image is the paired build.** Read the two aligned words around the recorded offset through the port (R2) and take the 32 bits at the offset (it is 2 mod 4: the stub's 48-bit immediate): they must equal the ABI word. Nothing has been written yet.
5. **Nothing in flight.** If jobs are queued, a `FENCE` and its wait: no overlay op may run while its region is rewritten.
6. **The bytes.** The file streamed through any word-aligned scratch (the visualizer uses the 28 KB of its strip past Now Playing's parts: no allocation beside playback, whose buffer would shrink), each piece to base + offset by engine 2 for its whole 64-byte units and R1 for the rest, under the port lock. FNV-1a and F16w (`jobs.h` `j_f16w`, the file as halfwords) summed piece by piece.
7. **Start it.** A wrong FNV-1a: refuse (no `OVERLAY`). Else `OVERLAY words, A, B` with a reply, deadline `J_T_FENCE_BASE + J_T_OVERLAY` (100 ms). The BCM drops the overlay's ops, checks the length, the F16w (`J_E_SUM`: retry once this epoch), the ABI word against its own and the load end (`HP_E_VERSION`: never retried this boot), runs `init` (the start-up checks) and sets the ops' `J_OPS` bits.
8. **Read back** `J_OPS` and `J_VPU` (`J_VPU_FFT`, `J_VPU_BARS`: the vector paths passed their checks).
9. **Lifetime.** The ops go with the epoch: a restart, `J_EV_EPOCH` (a mode change), a `RESET` the PP puts, or an op the BCM answers `HP_E_UNKNOWN` (its event not yet drained: never counted as a failure). The load is then due again, on the next frame that wants the ops.
- **The merged image has one region for both** (`0x12B000–0x133FFF`, 36 KB) [ours; sim]. A load evicts the other overlay (the driver marks it due again at once); a track change's DECODE wins and the visualizer holds its frame, then reloads; a visualizer load meeting a decode in flight is refused `BUSY`; both files are read on the art thread and put piece by piece by the job owner (the UI pass); `TONE` is refused while any overlay is live, because its buffer is inside the region. Steps 5 to 9 apply to each load. `bcm2722-reference/host-driver.md` §6a; `research/bcm2722/visualizer.md` §8.5; `album-render.md` §5.3.
- **Cost** [sim]: 16,980 B through the port (about 1.5 ms by engine 2, cell 14's rate); the BCM's side 0.77 M instructions (its sum, `init`, the FFT's and the bars' checks), about 28 ms at 27 MHz.
- **Unproven on silicon:** host writes into `0x13B000–0x13FFFF` (CR1 proved the slot pools, 0 wrong words; the region is the same SRAM, above them) and the overlay's own code. A torn write is caught by the F16w.
- **Evidence:** `firmware/target/arm/ipod/video/bcmjob-core.c` (`bjc_ovl_*`), `bcmjob-dev.c` (`bcmjob_viz_ops`), `bcmfw/video/jobs_bcm.c` (`jref_ext`); host test `tests/bcmjob` `t_overlay`; vc2sim `jobs_image_test.py` "overlay"; the pairing at packaging: `bcmfw/ovlpair.py`, `tests/ovlpair`.

## 6. Dead ends and the bad assumptions behind them

### D1. "The PP's DMA can feed the BCM's host port"

- **Assumption:** DMA channel 3, memory → the fixed `DATA` register, 32-bit, free running, delivers the bytes; S8 proved it into SRAM.
- **What happened:** S8's "clean" readback (0 of 48 checks bad) was **invalid**: the CPU methods had just written the same frame to the same place, so a DMA that wrote nothing read back the same [measured, found in review] (commit `6406c23b8c`). Into the SDRAM FIFO: S32 77 % wrong (first word of each block missing, later words shifted); S33 word 0 of every block missing after waiting for write-ready between blocks; S34 **nothing landed** at any size from 256 B to 32 KB. **S35 repeated the lab into SRAM as well: nothing landed there either** ("dma lab: SRAM, 256 … 32768 bytes: every word wrong, first at word 0") [silicon S34, S35].
- **The engine does run**: its busy bit clears after 58 µs (256 B) to 2.5–2.8 ms (32 KB), about 12–13 MB/s [silicon]. Something between the engine and the port drops the writes.
- **Best-known reasons, none tested** [hypothesis]: (a) the engine's 32-bit write to a 16-bit port is not split into two halves the way a CPU store is: try width 16 (`DMAn_INCR[30:28]` = 1); (b) the general engine cannot reach `0x3000_0000` at all, and Apple used its second engine (`0x6000_8000`) for this; (c) the port needs a request line for pacing. The S32/S33 partial data and the S34/S35 nothing differ, which no hypothesis yet explains.
- **Static evidence since (2026-10-15)** [apple; apple-RE] (`pp5022-reference/dma.md` §6): (b) gains weight: Apple's driver for the general engine **refuses** any peripheral address outside `0x7xxx_xxxx`, and Apple sends all port writes through the second engine. (c) loses weight: Apple sets **no** request line for the port. (a) is untouched for the general engine; on the second engine Apple writes no width field at all, only a `[26:24]` code of unknown meaning on each side. Also noted: our channel-3 `FLAGS`/`INCR` (`0x0400_0000`/`0x2001_0000`) differ from Apple's defaults for that engine (`0x2400_0000`/`0x2201_0000`). None of this is silicon; the lab in §7.1 decides.
- **Lesson:** a readback test must use a pattern no other path wrote. The player uses the CPU's copy (since dev39a, commit `69e7ed993f`).
- **Answered (S37):** the general engine was the wrong engine. It moves nothing even memory → memory (cell 12), while the PP's **second** DMA engine, driven as Apple drives it, lands every word in BCM SRAM and SDRAM (cell 14). Stage 5's upload lab no longer repeats S8's method: dev46 pre-fills every checked word with a pattern no method writes, so its DMA rows are now a valid "did it move anything" test.

### D2. "DMA is faster than the CPU"

- **Assumption:** moving port traffic to DMA would speed it up.
- **What happened:** the S8 lab timed both at the same rate: 27.6 ms by DMA against 27.0 ms by CPU for a frame at 30 MHz, 10.4 against 9.6 ms at 80 MHz. The cost is about 10 PP cycles per 16-bit transfer either way [silicon S8].
- **Owner's point (2026-10-15):** the goal of DMA was **offload**, freeing the CPU to sleep or work during the transfer, not speed. "No faster" was never a reason to drop it; delivering nothing (D1) is.
- **Lesson:** when judging a DMA path, measure CPU idle during the transfer, not only its duration.

### D3. "UDMA4 doubles storage throughput"

- **Assumption:** the cap at UDMA2 (because the adapter leaves IDENTIFY words 76 and 93 at 0) costs speed; UDMA4 would roughly double it.
- **What happened:** forced UDMA4 works (0 bad words over 8 × 64 MB) but reads at **21 MB/s, the same as UDMA2** [silicon S36c, `udma test rc=0 bad=0 mbs2=21 mbs4=21`, three runs]. Both are below UDMA2's 33 MB/s, so the limit is not the bus mode. Where it is (the adapter's flash, the controller's per-command overhead, or the CPU's per-command work) is [unknown] (DP-5).
- **And it costs**: UDMA above 2 needs 80 MHz, boosted once per command; during a library scan that was ~56,000 boosts in two minutes [silicon S36].
- **Lesson:** measure the bottleneck before raising a mode. Stay at UDMA2.

### D4. BCM memory-access quirks that bit us

Each is [silicon; measured]; details and evidence in `bcm2722-reference/quirks.md`.

| Assumption | What silicon does | Session | Rule |
|---|---|---|---|
| `ldsh` sign-extends in every form | The 32-bit `ldsh rd, (rs+off)` **zero-extends** | S17, S18 | Compute the address and use `ldsh rd, (rs)` |
| Vector loads take any address | Plain `vld8`/`vld16` align down to their access size; a horizontal B operand's `+rN` aligns x down to 16 bytes | S18–S22 | Misaligned data only through `vldp`/`vldq` pairs |
| r7 is a usable vector base | A base or stride field of 7 means "no register" (address 0): a store landed over SRAM `0x000–0x1FF`, the vectors and the mailbox | S13–S15 | Bases r0–r6 only; keep a guard block away from low SRAM |
| A scalar B operand r7 reads r7 | It reads 0 | S22, S24 | Never name r7 as a vector B operand |
| `vstr` writes every enabled lane | Only the lanes in the 16-byte line of its address | S22, S24 | Store crossing data in two parts |
| Rounding multiplies read B unsigned | `vmulhr` reads B **signed** (`#0x8000` = −1/2) | S26, S27 | Use `vmulhrs`/`vmulhru` or add-and-shift for a half |
| Byte stores into SDRAM corrupt lanes (S35's AAC hypothesis) | Right when read back after all the stores, host view included | S36c | Answered (BCM #45, on `p14-dev45`) |
| A host write lands at the halfword it names | A write starting at 2 mod 4 lands 2 bytes low (the address's bits 1:0 ignored): CR1's odd-width slot rows | 2026-10-25 [silicon, inferred]; sitting 2 [silicon] directly (ALGN) | Every host write and every slot row 4-aligned (pitch a multiple of 4 bytes); odd tails by read-modify-write (`bcm2722-reference/memory-access.md` §11.4) |
| Each halfword written to the port commits | A lone trailing halfword is dropped when the address changes; pairs land | 2026-10-25 sitting 2 [silicon] (ALGN PLH1/3/5) | End every write on a whole word (quirk #35) |
| The BCM's DMA honours any destination address | A 32-byte unit sent to +4 lands at +0, no error; 4-, 2- and 1-byte units at suitable addresses copy exactly | 2026-10-25 sitting 2 [silicon] (ALGN D32M; D4W9, D2H3, D1B5) | Each address a multiple of its unit (quirk #36) |
| A misaligned BCM store faults with nothing written; vector stores land at the exact address | Scalar: stored rounded down, then an imprecise fault; vector: `vst16`/`vst8` and every REP row round down | 2026-10-25 sitting 2 [silicon] (ALGN) | Aligned accesses only (quirks #37, #38) |
| A store then an immediate load of the same SDRAM word returns the stored value | **Open.** The AAC decoder still fails differently run to run (155 of 200 AUs refused) while vc2sim is bit-exact | S36c | BCM #46 (`HP_AAC_MEMSTRESS`, on `p14-dev45`); until settled, don't trust store-then-load in SDRAM from scalar code |

Lesson from all of these [measured]: when silicon and the simulator disagree, dump the chip's bytes and fit variant semantics offline; guessing cost three sessions (S12–S16) where dumps then found three rules in one session each.

### D5. Withdrawn readings about clocks and DMA status

| Assumption | Silicon | Session |
|---|---|---|
| The BCM runs at 150 MHz at entry (later 54, then 79 MHz estimates) | 27 MHz crystal until PLL A is switched in | S7 |
| The LCD bus is ~39 MHz with per-pixel overhead | 27 MHz, Σ(field + 1) bus cycles a pixel, no other overhead | S6, S7 |
| DMA status bit k means channel or sub-channel k done (BCM2835 analogy) | LCD completion is bit 8; bits 0–7 are not completion bits | S4, S5 |
| The 2-D skip word is the pitch | It is the gap after a row (pitch − width) | S5 |
| The LCD run-length register counts down | It counts up | S5 |
| The 1 MHz counter runs after a host start | Stopped until its clock registers are written | S2, S4 |

### D6. Stage 8a: the BCM's audio and the shared I2S clocks

- **Assumption 1: there is a master/slave bit, and the BCM can make its own clocks.** Apple's whole driver writes the audio block in six places only (`0x6001`, `|= 1`, `|= 0x6209`, 0, `+0x10 = 1`, the divider); nothing else in `vmcs.bin` touches `0x1000_48xx`, so there is no master/slave bit [apple]. Our image never asks for Apple's clock-manager clocks. Ready (bit 23) followed the **pin mux**: GPIO 2/3 on function 2 → ready (S35); GPIO 2/3 left as inputs → never ready (S36c, four tries) [silicon]. So the pin mux sets the clock direction: function 2 makes GPIO 2/3 the block's clock inputs [hypothesis, strong] (commit `8a0f88f33d`).
- **Assumption 2: putting GPIO 2/3 on function 2 is what screeched.** S35 played a full-scale screech with GPIO 1–3 on function 2; dev42 then muxed GPIO 1 only (commit `2588e7146a`), and the block never became ready. dev45's reading is that the screech came from the uncapped level or the sample format, not from two drivers on the clock lines [hypothesis]. **Both readings are on record; neither is proven.** What is safe: never configure GPIO 2/3 as **outputs**, and cap the level.
- **Assumption 3: no edges on GPIO 2/3 means the pins are not on the bus.** S36c's stage 8a-0 "playing" probes saw 0 edges and concluded "not on its bus" [silicon S36c bcmtest.log], contradicting S32–S34 (88,198 edges/s on GPIO 3). Cause: the clocks exist only while the codec's PLL runs. They vanish under **DAC standby** (5 s after PCM stops), and in S36c the soundtrack never started with the **COP decode switch** on, so no PCM ran (commits `a51230a9de`, `6e9fecc4a8`). Whether COP decoding itself affects the I2S clocks is **not** shown: the evidence is that PCM didn't run (DP-3).
- **Lessons:** hold the codec awake and confirm PCM is actually playing (mixer running, elapsed moving) before any clock probe; treat "no edges" as "no PCM" until proven otherwise; mux the clock pins before the data pin.

### D7. Torn reads mistaken for a dead port

- **Assumption:** an index word that reads as junk means the port has locked up.
- **What happened:** the client cut the BCM's power on each "stall" in S11–S12; every such stall was gone 7 µs later. A 32-bit read is two 16-bit reads and the BCM had rewritten the word between them [silicon S12].
- **Lesson:** encode polled words so a torn read is detectable (R2); bound waits but don't equate a bad word with death.

### D8. SDRAM bring-up treated like a textbook

- **Assumption (S9):** a command-7 poll timeout is fatal; mode field 7; no controller reset between tries; cache cleared before the flush.
- **What happened:** NOMEM. Following Apple's order exactly, the probe decides and field 4 passes on the second attempt [silicon S9, S10].
- **Lesson:** reproduce Apple's sequence write for write before improving it; let a functional probe, not a status bit, decide.

### D9. "Boosting the PP fixes a starved feed"

- **Assumption:** video stalls at 30 MHz mean the PP is too slow.
- **What happened:** the feed thread sat below the codec's boosted priority and ran up to 2.1 s late (S26, S31). With the thread in its own band, the unboosted PP held 16 ms of drift (S34) [silicon].
- **Lesson:** look at priority and scheduling before frequency (`pp5022-reference/quirks.md` #41).

### D10. Counters in cached memory written by both cores

- **Assumption:** a statistics struct in ordinary memory can be updated by both cores.
- **What happened:** a 16-byte line written back by one core undid the other's counts, and an idle COP may never write its lines back, so `cop_idle_pct` read stale [measured] (commit `b4e8fc5194`).
- **Lesson:** anything two cores write lives in the uncached alias or IRAM (R7).

### D11. Simulator blind spots for storage wake-ups

- **Assumption:** the simulator's disk model shows every storage access.
- **What happened:** on the device the flash never slept while stopped (812 single-sector commands per 30 s from a commit retry's failed opens); the simulator didn't count failed opens, so it never showed them [silicon S36] (`docs/fit1-brief.md` §A.8, commit `c5bca9b058`).
- **Lesson:** storage wake-ups are a device measurement; the simulator now counts failed opens, but treat its silence as weak evidence.

### D12. "ATA and USB DMA can run at the same time"

- **Assumption:** the IDE controller and the USB controller are independent bus masters, so reading the next 64 KiB from flash while the current 64 KiB goes out on Bulk-IN is free overlap.
- **What happened:** long reads stalled with the bulk-IN dTD ACTIVE for ~304 ms and a host reset, at ~166 MiB into a file [silicon] (`docs/usb-history.md`). Turning the 256 KiB prefetch worker off (`fa571b6895`, synchronous read-ahead) got reads past that point at 15.7-17.4 MiB/s, but ordinary multi-64 KiB READs still started the next ATA DMA while the IN was primed. `41103a9158` defers every next ATA read until the preceding Bulk-IN completes (and drops the last read-ahead fill); `USB_STORAGE_NO_READ_OVERLAP` is now the default for the iPod Video. The contention was a material contributor; a full end-to-end stability verdict is still open.
- **Lesson:** on the PP5022 treat ATA DMA and USB DMA as one resource. Serialise them, and give up throughput only on measurement (serial reads cost speed: ~2.5 MiB/s for the first 18 s of a mount scan, see `hw-experiments.md` §7).

### D13. Sending the other half of the double buffer before it was loaded

- **Rule (load-before-send):** a buffer half is read from storage only after the previous Bulk-IN has completed and before it is filtered, translated or sent. Never transmit a half that has not been loaded for this request. Serial, batched and cached reads must all keep it (`23d1223bba`).
- **What happened:** the first serial build (`41103a9158`) switched to the other 64 KiB half without loading it, so a multi-chunk read alternated correct data with stale RAM from an unrelated file [silicon]. Fragmentation cannot put unrelated bytes inside one file, which is how the owner's report was recognised. The media was intact; Mac-side content checks made with that build are invalid.
- **Lesson:** a speed-up that changes who owns a buffer needs a byte-identical read test over fragmented files first (`tests/usb/`).

### D14. "Switch the E2 display DMA off to test USB"

- **Assumption:** the LCD pushes by PP DMA engine 2 (`HAVE_LCD_E2`) are an optional second bus master that can be disabled when chasing USB faults.
- **What happened:** a short `lcd_e2off` trial failed at the first macOS mount, and was read as a hint against E2 [silicon, session 38q]. It was not valid evidence. The panel is physically reachable only through the BCM host port, and E2 is what moves the framebuffer there while freeing the CPU (cell 14).
- **Decision:** the trial is withdrawn. E2 stays on, and USB work looks at the ARC DMA, cache and physical-address discipline and the IDE interaction (D12) instead. Static display work is already quiesced before the storage handover.

### D15. "The keyclick is UI code" (and "a cross-core mutex makes shared state safe")

- **Assumption:** the headphone click belongs to the UI, so it can run wherever the UI runs; the DAC driver was made cross-core by putting its mutex in uncached memory (P19 S0).
- **What happened:** with the UI on the COP (bench sitting 3, build `7e8c140e40-261026`, steps `ucop`/`ucop2`) the owner heard a tone while scrolling with no music, changing a little with each detent and ending about a second after the wheel stopped, and artefacts on going back to the menus with music [silicon]. Each wheel detent started the beep channel on the COP: `mixer_channel_play_data()` reset `idle_counter` in the COP's cache only, so the CPU's FIQ skipped the idle frames' clearing and replayed the click's 256-sample frame (a buzz near 172–188 Hz) for the rest of the idle tail; when the PCM had stopped, the COP ran `mixer_buffer_callback()` and started the DMA itself with its own stale control words [ours, code reading; reproduced in the host model `tests/audioxcore`: 300/300 seeds, up to 43 frames replayed; routed to the CPU 0/300]. Each click also ran `dac_wake()` on the COP, which read a cached standby flag that the CPU had cleared when music started, and replayed the standby wake (PLL, DAC and output power writes, unmute) over live audio [model: 289/300 seeds with the cached flag, 0/300 uncached].
- **Lesson:** code is CPU-only unless it was written for two cores: a driver with an interrupt on one core, a lock that masks one core's interrupts, or cached state it keeps hot. Route requests to its core (R7 item 8), and give every word two cores share an uncached home even under a mutex.

### D16. "A host write is visible to the chip as soon as the next port write is accepted"

- **Assumption:** the port keeps its writes in order and applies them as they are made, so a command written after a packet can only be seen after the packet.
- **What happened** [silicon symptom, sitting 4b, 2026-10-26]: the first-use video check wrote a test packet into the SDRAM FIFO and sent its `FEED` at once. At all seven clock bands the image took the packet and showed no picture, with no error; the player, which writes packets well ahead, was fine. The same sequence in vc2sim reproduces it only if SDRAM writes reach the core behind a host read of SDRAM or 2 ms later [sim]. Mechanism: posted writes [hypothesis]; or the image's first-`OPEN` read of the FIFO's first 8 KB (BCM #73).
- **Rule:** write ahead; when a command follows its data closely, read the data's last word back through the port first (R1 step 6). Models of the chip must not be more permissive than the image (`sim-fidelity.md` §9a).

### D17. "A device-test play hands the chip back to us"

- **Assumption:** stopping a play returns the chip to the resident image, so the shell's jobs resume.
- **What happened** [silicon, sitting 4b]: a stage play boots the video image from its file, so its stop hands the chip to Apple's `vmcs` through the stock wake path (`dt_restore(false)`); the jobs stay off until the next start (the visualizer ran on the CPU, `why=nojobs`; the album panel decoded on the PP). Only a play that used the resident image itself is handed back to it (`bcm_video_hand_back`).
- **Rule:** in a bench, run the steps that need the chip's jobs before the first step that swaps the program (`bcm2722-reference/quirks.md` #48).

## 7. Open questions

The manuals keep the numbered lists; don't duplicate them here:
- PP5022: [`pp5022-reference/open-questions.md`](pp5022-reference/open-questions.md), notably #3 (cache geometry), #8 (`SWP` across cores), #11 (second DMA engine), #12 (DMA into the port: now answered as "SRAM fails too", see below), #13 (request IDs), #16 (UDMA4: answered by S36c).
- BCM2722: [`bcm2722-reference/open-questions.md`](bcm2722-reference/open-questions.md), notably #6 (cache), #7 (SDRAM speed), #13 (DMA with SDRAM), #25/#43 (port finer rules, ALT bank), #28 (BCM → host interrupt), #35–#39 (audio pins), #41 (PP DMA into the port: S35's SRAM row ran, nothing landed), #44 (does port traffic slow the chip), #45/#46 (SDRAM sub-word stores; store-then-load, on `p14-dev45`), #73 (does the core see the port's SDRAM writes before a command that follows them), #74 (the merged image's two overlays on silicon), #76 (the first-use check's figures).

**New questions this map exposed** now live in the manuals' lists; the DP numbers stay as stable names:

| # | Question | Where it lives |
|---|---|---|
| DP-1 | Does the BCM's clock, or SRAM against SDRAM, set the port's write rate at a fixed PP clock? | BCM [#47](bcm2722-reference/open-questions.md) |
| DP-2 | Does the PP's DMA deliver into the port through the **second engine** the way Apple drives it, and do SDRAM → SDRAM (both engines) and channel 3 at width 16 work? Lab: §7.1 | PP [#11](pp5022-reference/open-questions.md), [#12](pp5022-reference/open-questions.md), [#30](pp5022-reference/open-questions.md); BCM #41 |
| DP-3 | Does COP decoding stop or delay the I2S clocks? | BCM [#48](bcm2722-reference/open-questions.md) |
| DP-4 | Does the IDE interrupt (source 23) fire on our unit? | PP [#29](pp5022-reference/open-questions.md) |
| DP-5 | Where is the 21 MB/s storage read limit? | PP [#28](pp5022-reference/open-questions.md) |
| DP-6 | Can the COP touch the BCM port or other peripherals safely? | PP [#31](pp5022-reference/open-questions.md) |
| DP-7 | Can the PP's DMA masters (general, IDE) use IRAM? | PP [#30](pp5022-reference/open-questions.md) |

### 7.1 The engine-2 port lab (DP-2): a spec for the video devtest stages

Goal: find out whether the PP's second DMA engine, driven the way Apple drives it (`pp5022-reference/dma.md` §6.3), delivers data into the BCM's host port on our unit, and how much CPU it frees. Written so the video session can build it as devtest stages; nothing here has run. Register names are those of `dma.md` §6.2 (`E2_CMD` `+0x00`, `E2_STATUS` `+0x04`, `E2_SRC` `+0x10`, `E2_SRC_CFG` `+0x14`, `E2_DST` `+0x18`, `E2_DST_CFG` `+0x1C`; channel 0 at `0x6000_9000`).

**Rules for every step**
- **"Nothing happened" must fail.** Fill the target with pattern `T` and the source with pattern `S`, both derived from a per-run seed and the word index so that no word of `S` equals the `T` word at the same index, and no earlier run's data can match. After the transfer read back **every** word and classify each as: correct (`S[i]`), untouched (`T[i]`), shifted (`S[i−d]` for some small `d`: record `d`), or other. A step passes only if every word is correct.
- **Guards:** 16 words of a third pattern `G` immediately before and after every target region, checked after every transfer. A changed guard ends the stage at once (and, for a BCM target, the BCM is power-cycled and restarted by R3 before anything else).
- **Bounded targets:** before setting the start bit, the code checks the destination against the stage's allowed range (its SDRAM scratch buffer, or the BCM scratch window). No other destination is ever programmed. Never a `0x7xxx_xxxx` address.
- **Bounded time:** each piece has a 50 ms timeout (64 KB at even 2 MB/s takes 33 ms; S34's channel 3 moved 32 KB in 2.5–2.8 ms). On timeout: with IRQ and FIQ masked, clear `E2_CMD` bit 30, then bit 31 (Apple's stop order), read `E2_STATUS` twice, log both, skip the rest of the stage.
- **No interrupt handler needed:** set `E2_CMD` bit 30 as Apple does, but leave sources 26–28 **masked** in `CPU_INT_EN`. Detect completion by polling `E2_STATUS` (bits 30 and 31) and the raw `INT_STAT` (`0x6000_4010`) bits 26–28, and log which of them changed and when. This also settles the interrupt number (27 is [hypothesis]).
- **Cache:** source buffers in SDRAM, 16-byte aligned, programmed by physical address (`addr | 0x1000_0000`); write the whole data cache back before each start (as Apple does) and discard it before reading an SDRAM destination.
- **CPU only**, one fixed clock per run (log it), audio stopped except in E2-7, nothing else touching the port during a stage. Results go to the devtest results log; nothing on screen.
- At the end of the lab: both channels' bits 30/31 clear and master `0x6000_8000` bit 31 clear.

**Steps**

| Step | What | Expected / what each outcome means |
|---|---|---|
| **E2-0** | Read-only dump: `0x6000_8000`–`0x6000_800C`, `0x6000_9000`–`0x6000_90FF` (also compare `0x6000_9000` with `0x6000_9080`), `0x6000_A000`–`0x6000_A00C`, `0x6000_B000`–`0x6000_B07F` | Expected: mostly `0x0000_0001`, repeating every `0x80` (siggi `08`). All `0x0000_0000` or `0xFFFF_FFFF`: the engine may be clock-gated or absent; stop and look for an enable bit (`clocks-power.md`). Record the reset values: the RMW sequence keeps every bit Apple doesn't write |
| **E2-1** | **Control, engine 2 SDRAM → SDRAM**, channel 0, Apple's sequence (master bit 31; channel init by RMW; `E2_SRC`/`E2_DST` = physical buffers; `E2_CMD[15:0]` = bytes − 4; bit 27; bits 30, 31). Sizes 256 B, 4 KB, 32 KB, 64 KB | All correct: the engine runs memory → memory, and the piece times give its raw rate. Only the first destination word holds a source word, the rest untouched: the **destination does not increment** (fine for the port, which is one register for 64 KB). All untouched with `STATUS` unchanged: the engine never started (check E2-0). All untouched but `STATUS` shows completion: the engine runs but writes nowhere we look: compare with E2-2 before going on |
| **E2-2** | **Control, general engine channel 3 SDRAM → SDRAM** (the question in PP #30): `PER_ADDR` = destination, `RAM_TO_PER`, `INCR` range 0 (incrementing) and 32-bit, two arms: `FLAGS` `0x0400_0000` (ours) and `0x2400_0000` (Apple's default). 4 KB | Correct: channel 3 can write memory, so its port failure (D1) is specific to the port. Untouched: the general engine does not do memory → memory (consistent with Apple's driver, which only lets it face `0x7xxx_xxxx`), which separates it from the port question without settling it |
| **E2-3** | **Engine 2 → BCM SRAM, BCM held as Apple holds it**: in the boot stage that runs R3, after the bootstrap and before the start write. Scratch window `0xE0000`–`0xEFFFF` (S35's area) with guards on both sides. Fill `T` by CPU (R1); verify the fill on a sample (first, last and every 64th word, R2). Then `WR_ADDR` = window start as **two halfword stores**, one engine-2 piece of N bytes, destination `0x3000_0000`. After completion wait for write-ready (`CONTROL` bit 1, R1 step 3), then read back every word (R2). N = 256 B, 4 KB, 32 KB, 64 KB. Finally write a CPU control block of a different pattern elsewhere in the window and read it back, to prove the readback path | **All correct: engine 2 delivers into the port** (cell 14 becomes S; D1 gets its answer); go to E2-7. All untouched while E2-1 passed: the port drops engine-2 writes too; go to E2-4/E2-5. First word missing or words shifted (like S32/S33): partial delivery: log `d`, repeat with a write-ready wait before the start |
| **E2-4** | Same as E2-3 but with the BCM **running our image, idle**, scratch window outside the image and its data | Same as E2-3, and tells whether the BCM's state matters. Correct here but not in E2-3 (or the reverse) points at the BCM side, not the PP |
| **E2-5** | Only if E2-3/E2-4 fail: sweep `E2_DST_CFG[26:24]` and `E2_SRC_CFG[26:24]` over {2, 3, 4} each (9 arms, 4 KB each), then one arm with `E2_CMD` bit 27 clear. Same window, same rules | Any arm correct: that field is what the port needs; record which. None: engine-2 port DMA joins D1 as a dead end unless a new idea appears |
| **E2-6** | **Comparison: general engine channel 3 → BCM SRAM at width 16**: same window and rules as E2-4; `INCR` `0x1001_0000` and Apple's-default-like `0x1201_0000`, `FLAGS` `0x2400_0000`; 4 KB | Correct: hypothesis (a) of D1 was right and channel 3 is usable too. Untouched again: (a) is ruled out for channel 3 |
| **E2-7** | After R4, only if E2-3 or E2-4 passed: (1) engine 2 → **BCM SDRAM** scratch, sizes as E2-3; (2) **offload**: during a 64 KB piece, run a counting loop from IRAM that touches no SDRAM, then one that does, and compare counts with an idle baseline of the same length; (3) repeat (2) with audio playing (I2S on general channel 0) and log I2S FIFO underruns and the audio FIQ's worst latency | (1) correct: SDRAM targets work too. (2) gives the CPU fraction actually freed (the port's wait states may stall the bus, so it can be less than 100 %). (3) zero underruns: engine 2 can run beside playback. Any underrun: pieces must be shorter, or the feed must avoid audio refill times |

**What to report:** per step and size: elapsed µs, the classification counts and first wrong index, `E2_STATUS` before/after (both reads), `E2_CMD` after, `INT_STAT` bits 26–28, guard status, PP clock, BCM clock and state.

### 7.2 Is the second engine relevant to video?

**Only if E2-3/E2-4 pass.** Until then the player keeps the CPU copy (R1).

**How the video path would use it** [hypothesis, ours]:
- **Bitstream FIFO in BCM SDRAM:** the feed thread queues a packet (or a run of packets) as one engine-2 transfer, sleeps until the completion interrupt, then publishes the new write pointer by a CPU write. Apple's pattern exactly: CPU sets the address, engine moves ≥ 256 bytes, CPU writes the tail.
- **Overlay band and full-frame uploads** (the shell's overlay over video, menus drawn on the PP): the biggest single gain, since these are large.
- **Image uploads:** the BCM firmware image at each wake, cover art, pre-scaled art.

**What it would save, from the measured port rate** (CPU copy ≈ 10 PP cycles per 16-bit transfer, so about 5.7 MB/s at 30 MHz; S8: 153,600 B in 27.0 ms at 30 MHz, 9.6 ms at 80 MHz) [silicon S8; arithmetic ours]:

| Transfer | CPU copy at 30 MHz | With engine 2 (if it works) |
|---|---|---|
| Bitstream at 768 kbit/s (96 KB/s; ~3.2 KB a picture at 30 fps) | ~0.56 ms a picture, ~17 ms per second ≈ 1.7 % of the CPU | a few tens of µs a picture for setup, interrupt and wake-up [hypothesis] |
| Bitstream at 2.5 Mbit/s (MPEG-4 upper end) | ~55 ms per second ≈ 5.5 % | same few tens of µs a transfer |
| Overlay band 320 × 40 RGB565 (25.6 KB) | ~4.5 ms | CPU free for ~4.5 ms |
| Full frame (153.6 KB) | ~27 ms (a whole 30 fps frame time) | CPU free for ~27 ms |
| BCM image (~93 KB) at each wake | ~16 ms | CPU free during the upload |

The bitstream gain is small in CPU percentage; its value is that the feed thread no longer **occupies** the CPU in its priority band, so the codec and the UI keep running unboosted (R1 step 7, D9). The frame and band uploads are where boosting is tempting today; there the engine would remove the reason to boost. The real gain is what E2-7 (2) measures: if the port's wait states stall the bus, the CPU is freed only partly.

**Risks**
- **Cache:** the engine reads physical SDRAM, so dirty lines must be written back first. Apple writes the **whole** data cache back before every transfer; Rockbox has only whole-cache operations too (`pp5022-reference/cores-and-threads.md` §3). That cost is paid per transfer, which argues for fewer, larger transfers (batch packets; ≥ 256 bytes as Apple does).
- **Sharing the port:** nothing else may touch the port during a piece (command words, R2 reads, LCD commands): Apple holds a per-bank mutex for the whole write. Our port users would need one lock, and the BCM must not consume a FIFO region before the CPU publishes its pointer.
- **I2S DMA:** audio uses the general engine's channel 0, a different engine, so there is no channel conflict; but both share the bus and SDRAM, and engine 2 runs unpaced. Underruns or late audio FIQs are possible [hypothesis]; E2-7 (3) measures them. Shorter pieces are the fallback.
- **Request-line pacing:** Apple uses none for the port, so pacing comes only from the port's wait states. If a request line exists and we need it, its ID is unknown (PP #13).
- **Interrupts:** a production driver needs source 27 (if E2-0/E2-1 confirm it) in the CPU's IRQ dispatch, and a semaphore wake; Apple refuses to run the transfer from the COP, and our design keeps the port on the CPU anyway (DP-6).
- **Clean-room status:** everything about this engine is [apple; apple-RE] until the lab runs; the `[26:24]` fields and bit 27 have no known meaning.

## 8. Clean-room gaps

Everything a from-scratch firmware could not take from our own measurements. Each works on our unit (where marked [silicon]) but the register meanings come from someone else's code.

**[rockbox-derived]: re-verify before relying on the meaning**
- PP memory: the remap windows (`MMAPn`) and their flag byte `0x84`; SDRAM size probe; cacheability mask `0x1C00`; `CACHE_OPERATION` bits and the status-word invalidate; `CACHE_PRIORITY` (meaning unknown even to Rockbox) (`memory-map.md` §3, §6).
- PP cores: the mailbox sleep/wake handshake bits; `PROC_CTL` sleep; Peterson locks as the only cross-core primitive (`cores-and-threads.md` §5).
- PP general DMA register layout (`DMAn_CMD`, `STATUS`, `FLAGS` bit 26, `INCR`), request ID 2 for I2S, "read STATUS acknowledges" (`dma.md` §1–§2).
- IDE: every timing word, `IDE0_CFG` bits 3/4/5/15/28/29, `IDE_DMA_CONTROL` bits, the `0x6000_60C4` IDE clock word, "don't read status during DMA" (`storage.md`).
- USB: the PHY sequence on `0x7000_0028`/`0x7000_0060`, the enable bits, descriptors in IRAM (`usb.md` §2–§3).
- I2S and the WM8758: `IISCONFIG`/`IISFIFO_CFG` fields, the FIFO formats, the codec's power-up order, PLL values per rate, CLKCTRL `0x145`, the external device clock at `0x7000_0018` (`pp5022-reference/audio.md`).
- The BCM port: the register addresses and handshake bits, the bootstrap byte sequence, the start write `0xA5A5_0002`, the cache flush from the host, the power-down order and its two "unknown" steps (`display.md` §1–§3, `boot-and-clocks.md` §5).
- PLL rules: ≥ 96 MHz before the post-divider; `DEV_TIMING1` following the clock; both cores stalled for a `CLOCK_SOURCE` write (`clocks-power.md`).

**[apple-RE]: values copied from Apple's firmware, meanings partly our hypotheses**
- BCM DMA: channel config words, transfer-information bits 0/24/25/27/28, sub-channel registers, status bits 9/11, channel assignments 5/6/7 (`dma.md`).
- BCM LCD: the run-control words `0x231`/`0x229`, `+0x08 = 0x1F`, panel scripts and timing words (`lcd.md`).
- BCM clocks: the PLL A sequence and regulator levels (`boot-and-clocks.md` §4); SDRAM timing words, mode word and control values (`sdram.md`).
- BCM audio: every audio-block value (`0x6001`, `0x6209`, the `0xA508` divider), channel 5's config and silence-loop chain (`bcm2722-reference/audio.md`).
- PP: the second DMA engine (registers, `CMD` bits 25/27/`[23:20]`, the `[26:24]` codes, source/destination order, interrupt 27) and its use for every large port write (`pp5022-reference/dma.md` §6); Apple's SDRAM controller init; Apple's use of PP DMA for the port in 64 KB chunks; Apple's mailbox interrupt (`pp5022-reference/dma.md`, `memory-map.md` §2, `cores-and-threads.md` §5).

## 9. Where the existing documents disagreed

The nine stale or conflicting statements found while building this map were fixed in the source documents in commit `2db26bc5d8` (PP quirks #36, PP open questions #12 and #16, PP `dma.md` §4, `storage.md` §6, `cores-and-threads.md` §7, `display.md` §5, BCM `memory-access.md` §7b and §9, BCM open questions #41, BCM `audio.md` §8, `diagrams/system.d2`). Still open:

- **The port's write rate** (`display.md` §5 against the S8 reading in `memory-access.md` §9): both now state what the evidence supports; which factor slows SDRAM writes at BCM 27 MHz is BCM #47 (DP-1).
- **The S35 screech**: two readings, both recorded in BCM `audio.md` §8 and D6; settled only by a session.
- **BCM #45/#46** (SDRAM byte stores; store-then-load): answered and added on branch `p14-dev45` (commit `8093203dd5`); this branch's copy of #45 stays stale until that branch merges.

## 10. Keeping it current

- **Read this file first** before any DMA, memory, cache, cross-chip or bus work, and check the cell you are about to rely on.
- **Every silicon finding updates this file in the same commit** as its reference-manual entry (`bcm2722-reference/` or `pp5022-reference/`): the matrix cell, its note, and a recipe or a dead end if one changed. A new rate goes in the cell's note with its session.
- Never upgrade a cell's status without a session or log line to cite. An experiment that can't distinguish "worked" from "did nothing" doesn't count (D1).
- When a DP-*n* question is taken into a manual's open-questions list, replace its row here with a link.
- Redraw the data-path diagrams (`diagrams/data-paths*.d2`: the overview and the PP, BCM and audio views) when a cell changes status: `docs/diagrams/render.sh data-paths data-paths-pp data-paths-bcm data-paths-audio`.
