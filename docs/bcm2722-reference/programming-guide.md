# 16. Programming guide

A route through the manual for someone writing their own firmware for the BCM2722. It adds no new findings: each step says what to do, which chapter holds the facts and their evidence, and, where our own designs exist, which note under `docs/research/bcm2722/` describes them. Designs there are [ours] (choices we made, not properties of the chip) and many have not run on silicon; read the tags.

## 1. Before you write anything

1. Read the [evidence tags and conventions](index.md#evidence-tags). Every statement is [silicon], [apple], [rockbox], [external], [hypothesis], [ours] or [estimate]; build on [silicon] first.
2. Read [14. Quirks](quirks.md) in full. It is the list of places where silicon differs from the obvious assumption (the instruction set, the vector unit, clocks, SDRAM and the port).
3. Skim [15. Open questions](open-questions.md), so you do not spend a session on something already known to be unsettled.
4. For DMA, memory or cross-chip work, read [`../data-paths.md`](../data-paths.md), the map of who can move bytes where.
5. Tools: a disassembler, simulator and compiler exist in this project (`vc2dis`, `vc2sim`, `vc2cc`; [`../research/bcm2722/sim.md`](../research/bcm2722/sim.md), [`compiler.md`](../research/bcm2722/compiler.md), [`isa.md`](../research/bcm2722/isa.md)). The simulator models some things silicon does not; [14. Quirks](quirks.md), "Simulator-only artefacts to avoid".

## 2. Bringing up an image

| Step | What to do | Chapter |
|---|---|---|
| 1. Power and bootstrap | The host powers the BCM (`GPO32` bit 14, wait 50 ms), runs the three-stage bootstrap and streams the image to core address 0 | [5](host-interface.md) §1, "Starting an image" |
| 2. Layout | Vectors in the first `0x200` bytes (point every one at a fault stub), code from `0x200`, a stack below any framebuffer you use | [4](boot-and-clocks.md) §1; [2](memory-map.md) |
| 3. Cache and start | Flush the cache, then write `0x10000400 = 0xA5A5_0002`; the core starts at `0x200` at 27 MHz with the cache off and the 1 MHz counter stopped | [4](boot-and-clocks.md) §1–2 |
| 4. First instructions | Set `gp` and `sp`; write the interrupt priorities (they are not defined at entry); start the 1 MHz counter's clock if you want microsecond timing | [4](boot-and-clocks.md) §1, §3; [10](interrupts-timers-gpio.md) |
| 5. Signal ready | Write a ready word the host can poll (Rockbox's convention is `0x1F8`) | [5](host-interface.md) §3a |
| 6. Faults | Know the exception model before relying on any misaligned or odd access | [10](interrupts-timers-gpio.md) §2; [6](isa-scalar.md) |
| 7. Faster clock (optional) | Apple's PLL sequence takes the core from 27 to 150 MHz; delays calibrated in instructions then change by 5.56× | [4](boot-and-clocks.md) §4 |
| 8. SDRAM (optional) | After the clock switch, run the bring-up sequence (on our unit it came up on the second attempt with mode field 4) | [11](sdram.md) |
| 9. Panel | Identify the panel from its two GPIO pins, run its power-on sequence, set the window, send pixels by CPU or by DMA | [9](lcd.md) |

Our own bring-up image and its decisions: [`../research/bcm2722/bringup.md`](../research/bcm2722/bringup.md); the hardware it assumes: [`hardware.md`](../research/bcm2722/hardware.md); the run-book for device sessions: [`device-session-1.md`](../research/bcm2722/device-session-1.md).

## 3. Running code well

- **Scalar first.** Get a scalar program working with [6. Scalar instruction set](isa-scalar.md) before using the vector unit.
- **Vector code.** Use [7. Vector unit](isa-vector.md), "Rules for writing vector code on silicon", beside [14. Quirks](quirks.md) (the vector-unit section). Several forms behave differently from the model; the list is not complete ([15](open-questions.md) #1, #2).
- **Memory.** Know which memory each access reaches and how wide and aligned it must be: [3. Memory access](memory-access.md). Who may write where, and when, is §8 there.
- **DMA.** [8. DMA engine](dma.md); the LCD frame push is paced by DREQ 3, audio uses channel 5 ([9](lcd.md) §6, [12](audio.md) §3).
- **Interrupts.** No interrupt source has been confirmed on silicon ([15](open-questions.md) #3); polling is the proven route.
- **C.** [6. Scalar instruction set](isa-scalar.md), "A compiler route", and [`../research/bcm2722/compiler.md`](../research/bcm2722/compiler.md).

## 4. Talking to the host

The port carries memory traffic only; every protocol is a software convention in BCM memory ([5](host-interface.md) §3).

| Need | Use | Chapter |
|---|---|---|
| Just drive the panel from the host | Rockbox's command word (`LCD_UPDATE`, `LCD_UPDATERECT`, `LCD_SLEEP`), the 320×240 RGB565 frame at `0xE0000` | [5](host-interface.md) §3a |
| Understand Apple's own transport | Channel rings behind a directory (`0x1F8`, `0x1FC`) | [5](host-interface.md) §3b |
| A full-featured, robust protocol | A shared-memory header, polled command and event rings, one writer per word, torn-read detection | [5](host-interface.md) §3c; design: [`hostproto.md`](../research/bcm2722/hostproto.md), [`protocol.md`](../research/bcm2722/protocol.md) |
| Video | Session lifecycle, bitstream FIFO, A/V sync, failure signatures | [17](host-driver.md); designs: [`pipeline.md`](../research/bcm2722/pipeline.md), [`player.md`](../research/bcm2722/player.md), [`decoder.md`](../research/bcm2722/decoder.md) |

Host rules that cost us sessions are in [5](host-interface.md) §4 ("Things a host programmer must not do") and [17](host-driver.md) §9 (gotchas).

## 5. Display jobs

To draw rather than stream video, the host can send the BCM small jobs and let it do the pixel work. This is a design, not a chip feature, and **nothing in it has run on silicon**:

- The job interface: [`jobs.md`](../research/bcm2722/jobs.md).
- What lives where in BCM memory: [`memory-contract.md`](../research/bcm2722/memory-contract.md).
- Power states for lit and dark use: [`power-contract.md`](../research/bcm2722/power-contract.md); summary in [13. Power](power.md).
- Which of the shell's pixel work to move first: [`display-offload-plan.md`](../research/bcm2722/display-offload-plan.md); measurement plan: [`offload.md`](../research/bcm2722/offload.md), [`bench-p19.md`](../research/bcm2722/bench-p19.md).
- Swapping images without losing the panel: [`warm-swap.md`](../research/bcm2722/warm-swap.md).
- What the chip can sustain for games, Cover Flow and compositing: [18. Drawing on the chip](graphics.md); a worked 3D example: [`render3d.md`](../research/bcm2722/render3d.md).
- Other worked examples of code on the chip: JPEG ([`jpeg-spike.md`](../research/bcm2722/jpeg-spike.md)), MPEG-4 ([`mpeg4.md`](../research/bcm2722/mpeg4.md)), audio ([`audio-spike.md`](../research/bcm2722/audio-spike.md) and [12](audio.md)).
- Running without any Apple code: [`independence.md`](../research/bcm2722/independence.md).

## 6. Shutting down

Power-down order and wake costs: [13. Power](power.md), [4](boot-and-clocks.md) §5. Hand-back to a host that expects the stock display driver: [17](host-driver.md) §3.
