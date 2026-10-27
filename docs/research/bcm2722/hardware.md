# BCM2722 hardware: what our own VideoCore image needs

Issue #60, milestone 2. These are the hardware facts our bring-up image needs (start-up, clocks, SDRAM, LCD, the host port, DMA, interrupts and the timer). They were recovered from Apple's `vmcs.bin` 1.3 (`artefacts.md`, reference only) in two ways:

- **statically**, with `tools/bcmre/vc2dis.py`;
- **by running it** in `tools/bcmre/vc2sim.py` against modelled peripherals (`vc2mmio.py`). Every device access is logged with its pc and simulated time.

The register meanings are our reading of what the code does with them. Nothing here is Apple's code: the tables give addresses, values and order only.

Confidence: **[H]** seen directly (the access is in the log, or the instruction is in the listing); **[M]** strong inference; **[L]** guess. `@xxxx` is the `vmcs.bin` 1.3 address it comes from. The start-up register sequence is identical in 1.2.1, 1.2.3 and 1.3 [H]: the device logs of the first 400,000 instructions match access for access.

## Reproduce

```sh
V=~/Git/re/bcm2722/extract/1.3/vc/VideoCore/Boot/vmcs.bin
tools/bcmre/vc2sim.py --boot $V --bcm --max 400000 --hide 0x10001804      # start-up, every device access
tools/bcmre/vc2sim.py --boot $V --bcm --max 400000 --panel 0 --then 0xf672,2 --lcd   # panel 0 power-on
tools/bcmre/vc2sim.py --boot $V --bcm --max 400000 --then 0x12a02,2 --panel 1 --then 0xf672,2 --lcd   # panel 1, model 2
```

`--bcm` is the minimum model `vmcs.bin` boots under (`sim.md`, "Peripheral bus"):

- two "operation done" status bits;
- the SDRAM window;
- the timer and interrupt controller.

The self-test rows "vmcs.bin start-up writes" and "LCD power-on per panel ID" (`vc2boot_tests.py`) check the facts below on every run.

## Address map

| Range | What | Conf. | Source |
|---|---|---|---|
| `0x00000000–0x0013FFFF` | Internal SRAM, 1.25 MB. The image runs from 0. | [H] the image's own pool table: internal heap = [end of image `0x316a0`, `0x140000`) | words at `0x1fc10/0x1fc14`, used @de04 |
| `0x000001F8`, `0x000001FC` | Rockbox's command and status words (`lcd-video.c`). They sit in the unused end of the vector area. `vmcs.bin` (the disk image) stores the address of *its own* host-interface block at `0x1FC` and never uses `0x1F8`. Our image also uses `0x1E0–0x1F7` as a mailbox (`bcmfw/README.md`). | [H] | @d4e6 `st r6,(448+60)` |
| `0x000E0000–0x00105800` | Rockbox's framebuffer: 320×240 RGB565 (640-byte rows), inside internal SRAM. | [H] Rockbox; inside SRAM [H] | `lcd-video.c` |
| `0xFE000000–0xFE3FFFFF` | SDRAM, 4 MB, as the VideoCore sees it. Rockbox reaches the same memory as `0xC0000000` through the host port. | [H] pool table `0x1fc20/0x1fc24` | @84c8 |
| `0x10000000–0x1000FFFF` | Peripherals, in blocks of 0x400 (below). Several registers take a password `0xA5A5` in bits 31:16 on writes. | [H] | |

## Start-up (`vmcs.bin` entry `0x200`)

| # | What | Registers and values | Conf. | Where |
|---|---|---|---|---|
| 1 | gp, sp | `gp = 0x1fc40`; `sp = 0x316a0` (top of `.bss`, 4-aligned) | [H] | @200 |
| 2 | Copy and zero `.data`/`.bss` from an "INI" record list | memory only | [H] | @3e6c |
| 3 | Flush, then enable the cache | `0x10000C00`: write `old \| 0x40000000`, wait for bit 0, write `old` back; then set bit 31 (`0x80000000`) | [H] sequence; [M] meaning | @25a, @224 |
| 4 | Run control | `0x10000400 = 0xA5A50000 \| (old & ~1)`; `0x10000408 = 0xA5A50002` | [H] values; [L] meaning | @b4e |
| 5 | Quiesce | interrupt priorities `0x10001010/14/18/1C = 0`; timer compares `0x10001808–14 = 0`; each DMA channel: `+4 = 0xFFFFFFFF` (clear status), `+0 = 0`; GPIO function selects `0x10002800–0C = 0`, event status `0x10002828/2C = ~0`, edge enables `0x10002830–3C = 0`, `0x10002840–5C = 0`; `0x10004C30 = 0` | [H] | @1433e |
| 6 | Clocks to 150 MHz | see "Clocks" | [H] | @14506 |
| 7 | System tick | timer status `= 0xF`; compare 0 `= counter + 10000` (10 ms); interrupt source 16 priority 1 (`0x10001018` nibble 0) | [H] | @bb2 |
| 8 | SDRAM | see "SDRAM" | [H] | @147ca |
| 9 | RTOS (Nucleus PLUS: `TCC_*`, `SMC_*`, `TCT_*` exports), task creation, then the scheduler; it idles in `ei; sleep; di` | | [H] | @28c8, @2914, @2338 |

The first task is started with `rti` from a frame that the task builder (@1fd4) lays out. Its `sr` word is `0x40000000`, i.e. interrupts enabled.

## Clocks, PLL and regulator

The block is at `0x10001C00`. Every write carries the `0xA5A5` password.

| Register | Use | Conf. |
|---|---|---|
| `+0x00/+0x04/+0x08/+0x0C` | PLL A–D input divider M | [M] |
| `+0x14/+0x18/+0x1C/+0x20` | PLL A–D control. Bits 9:0 = multiplier N; bit 10 = enable, set about 1 ms after N. f = 27 MHz × N / M. | [M]: Apple's request for 150 MHz (`0x8f0d180`) gives M = 108, N = 600, and 27 × 600 / 108 = 150 |
| `+0x28` | core clock select: `0x1` before the switch, `0x101` after | [H] values; [L] fields |
| `+0x2C–0x3C`, `+0x40/+0x44/+0x48/+0x50`, `+0x58` | per-block clock controls (10 consumers); all written `0` at start-up except `+0x58 = 0x101` | [H] values; [L] meaning |
| `+0x4C`, `+0x54` | `+0x4C = 0x501`, about 20 nops, then `0x51B`; `+0x54 = 0` | [H] |
| `0x10001400` bits 3:0 | Regulator level for the core clock. A 13-row table at `0x1fa68` maps frequency to level: 160.9 MHz → 15, 102 MHz → 9, 46.9 MHz → 5. A 10 ms wait follows each change. Bits 7:4 are preserved; Rockbox clears them at power-down. | [H] table and wait; [M] "regulator" |
| `sys19 rd` | Read before the level is chosen. If its low nibble is ≥ 4, the level is forced to 15 (a chip revision?). Modelled as 0. | [H] use; [L] meaning |

The crystal is 27 MHz [M]: a function returns `0x19bfcc0` as the reference (@1490a). Start-up order [H]:

1. write `0x10000400 = 0xA5A50020` (twice);
2. write `+0x4C`/`+0x54`;
3. switch off every block clock and write `+0x28 = 1`;
4. zero each PLL's M and control;
5. set the regulator to 15, then wait 10 ms;
6. write `M = 0x6C` and `N = 0x258`, wait 1 ms, then write `N | 0x400`;
7. write `+0x28 = 0x101`.

### After a host start, on silicon (device sessions 2 and 3, 2026-10-10)

What our images found when Rockbox started them after a power cut, with no clock write of their own:

- **The 1 MHz counter (`0x10001804`) does not count [H].** 0 ticks over a 3,000-instruction spin, in every probe and bench run (11 runs), and again over the bench's whole 1.5 s. So something Apple's start-up does starts it.
- **The core runs at about 27 MHz [M], the crystal.** The bench reports each kernel in its step word and the iPod logs when it sees each one. Kernels 6–19 (5.9 M scalar instructions) took 268 ms (22 MIPS, including loads, stores and taken branches). Kernels 20–68 (6.0 M instructions plus 28.8 M vector rows) took 1,160 ms: 30.0 M instructions-plus-rows per second, which is one per cycle at 30 MHz. The step times are host-polled (10 ms granularity), so ±10%. No PLL runs: Apple's start-up writes PLL A itself.
- **The vector unit runs [M]**: REP rows cost time in proportion to their count, so the VPU is not idle. Its loads and stores reach SRAM for a plain copy; results of vector ALU operations did not (`bringup.md`, "Vector diagnostics (after device session 3)").
- **Which write starts the counter [L→M].** Of Apple's start-up writes before its first counter-timed wait (the 10 ms regulator wait), two look like the counter's: `+0x4C = 0x501`, about 20 nops, `+0x4C = 0x51B` (with `+0x54 = 0` after each), and `+0x58 = 0x101`. 0x1B is 27: 27 MHz / 27 = 1 MHz, so `+0x4C` reads as the counter's prescaler (bit 10 and bit 8 enables, divider in bits 7:0). `+0x58 = 0x101` is the only block-clock control Apple sets non-zero, and Apple writes it with every PLL off, right before relying on the counter. Hello v2 makes the `+0x4C`/`+0x54` writes, checks the counter, then `+0x58`, checks again, and reports which one started it (`bcmfw/README.md`, mailbox `0x188`). `0x10000400 = 0xA5A50020` (step 1) is the remaining candidate; it stays out because the host uses that register to start the core.
- **The LCD interface needs no clock write [M].** Apple writes every other block-clock control to 0 and still drives the panel, and session 2's stage 2 failure is explained without it: the image's spin delays were 17 times too long at 27 MHz, so panel 3's 265 ms power-on took about 4.4 s and the host gave up at 3 s. Hello v2's step word names the access if this is wrong.

### Device session 4 (2026-10-11, unified build 8417548e75)

- **The counter is started by `+0x4C`/`+0x54` [H].** Every image of every run reports `clock 00xx0c12: counter started by 0x4C/0x54`; `+0x58` was never needed. Found before the write: `4c 00000001 54 00000000 58 00000138`.
- **The counter is a true 1 MHz [H].** The bench's kernel times by the counter sum to 1,499 ms; the host saw kernel 0 to done in 1,499 ms (log 15,630 → 17,129).
- **(Withdrawn in session 6: the timed loop is one instruction, not three; see "Device session 6".)** **The core clock is not 27 MHz; it is at least ~79 MHz [M].** The same 3-instruction `spin` loop (`add`, `cmp`, `bne`) takes 38 µs per 1,000 iterations in probe, bench and hello_dma: 79 M instructions/s. So session 2's "27 MHz" was an instruction rate, not a clock. The bench's dependent 16-bit `add` chain (ALU1) runs at 26.2 M/s, so a dependent add costs about 3 cycles at 79 MHz (no forwarding), and two or four independent chains reach only 50.8 M/s. Hello runs the identical loop at 75 µs (40 M/s); the code is the same bytes at a different address (`0xb74` against `0xd00`, `0xcec`, `0xbc2`), so a fetch or alignment effect is the likeliest cause [L]. Readlog's "3 cycles per spin" reading is kept but now labelled as a lower bound. A cycle-accurate figure needs a kernel at several alignments, or the PLL build (150 MHz by construction).
- **Panel 3, CPU push, device-verified [H]** (owner's visual report): colour order, byte order and orientation of the full frame are correct; `UPDATERECT 200,170 100x50` drew the magenta box inset 20 px from the right and bottom edges, so its window position and size are right too.
- **Panel 3 timings (CPU push, our images) [H]:** first update (power-on and frame) 314.7 ms; full frame 49.0 ms (638 ns per pixel); rect 100×50 3.2–3.5 ms; panel off 68.2 ms; wake frame 314.7 ms. Apple's `LCD_UPDATE` (DMA, 150 MHz) 15.2–15.5 ms (201 ns per pixel).
- **DMA at the stage 3 timeout [M]:** channel 1 `+0x00` read `0x49032101` (written `0x09032101`: bit 30 now set, so bits 31:29 = 2, the value Apple's TV poller waits for), `+0x04` read `0x00002300` (bits 8, 9, 13; bit 0, which we polled, clear), sub-channel 0's block register read 0 (the engine had left our block). Apple's handler (@7b7c) acknowledges the whole word and tests a per-sub-channel mask from its registration table; the mask values are not yet read. Most likely the transfer ran and finished, and completion is bit 8 + k (or state 2), not bit k [M]. **Settled statically (dev4) [H]:** Apple's LCD push registers its completion callback with mask `0x100` (@145e2: `mov r1, #256` before the registration call @7c54), and the generic transfers with `0x800 | 1 << (13 + k)` (@804c); the handler @7b7c acknowledges the whole word. So the session 4 value `0x2300` is "LCD run done" (8) and "sub-channel 0's chain ended" (13): the transfer finished. Apple does not clear the word before posting a block, and does not poll the state field for the LCD. dev4's `dma.c` clears the whole word before arming and waits for bit 8.

### Device session 5 (2026-10-11, unified build 99c9a3781c, dev4 images)

Runs 0-1, 0-2, 0-3, 0-4 and 0-3 again (log with the owner's backup, `bcm-session5-*`; `readlog.py` decodes it).

- **DMA to the panel works [H].** Stage 3 passes: two full frames and the 2-D rect each finished on status bit 8 (`int 00002300`, `done 1, 2, 3`, `timeouts 0`). The owner saw the pattern by DMA with no black band (photo 4), the magenta box at the top left (photo 5, 2-D DMA) and the black band only on the forced-timeout frame (photo 6, the CPU fallback, by design).
- **The LCD run length counts up [H].** `+0x04` reads `0xa5` (165) one 64 µs poll after arming and `0x12c00` (76,800, the value written) at the end; the rect ends at `0x1388` (5,000). The log's "stopped short" was our misreading of a count-down; dev5 reads it as "counted up: all out". 165 transfers in 64 µs is 388 ns each, the same rate as the whole frame. The control reads `0x325` while the run goes and `0x323` after (written `0x329`): bit 3 (go) self-clears, bit 2 reads as busy, bit 1 as done [M].
- **DMA frame time [H]: 31.4 ms** on the BCM counter (408 ns per pixel), rect 100×50 2.05 ms; Apple's `LCD_UPDATE` 15.2 ms (198 ns). Our timing words (for 100 MHz) pace it, not the engine.
- **(Withdrawn in session 6: the bus runs at 27 MHz; the CPU push has pacing of its own.)** **The LCD bus runs near 39 MHz after a host start [M].** Stage 2's pair: Apple's 150 MHz words 48,927 µs, ours 38,966 µs. Panel 3's later word drops from 16 to 11 cycles, so (48,927 − 38,966) µs / 76,800 / 5 = **25.9 ns per bus cycle**, 38.6 MHz: half the ~78 MHz instruction rate, which suggests bus = core / 2 [L]. The rest of each pixel is fixed overhead: about 220 ns on the CPU push and 120 ns by DMA. Session 4's ~25–27 MHz estimate assumed no overhead and is withdrawn.
- **(Explained in session 6: the loop's address, see there.)** **Spin rate differs between images again [M].** spin(1000) took 38 ticks in hello and the bench (78 M instructions/s) and 75 in hello_dma (40 M/s), the same three instructions at another address. In session 4 it was hello that ran slow. No single boundary rule fits all six known loop addresses; dev5 times the identical loop at 16 offsets in every image (`bcmfw/spinprobe.S`, mailbox `0x150`) [open].
- **Vector unit [H]:** the 48-bit `vst16` field layout and the non-clamping `vmulhrs` (`isa.md`, "Vector stores on silicon"). vc2sim now reproduces the session's records word for word.

### Device session 6 (2026-10-11, build p15-dac 77d5ddaa67, dev5 images)

Runs 0-4 twice (log with the owner's backup, `bcm-session6-*`; the last run from line 2100).

- **Stages 0-3 pass; DMA reads "counted up: all out" [H].**
- **Bus sweep, both runs within 5 µs [H]:** a full DMA frame takes **19,954 µs** with the 50 MHz words, **17,118 µs** with 40 MHz and **14,277 µs** with 30 MHz (Apple's `LCD_UPDATE`: 15,198 µs). The owner saw no artefacts at any of them ("roughly speaking": a visual pass on the bars, not a pixel check).
- **The LCD bus runs at 27 MHz, and a DMA pixel costs exactly the later timing word's cycles [H].** Panel 3's later `+0x14` word has 7, 6 and 5 cycles (four fields, each n + 1) at 50, 40 and 30 MHz, and 11 at 100 MHz (session 5: 31,355 µs). Frame = 76,800 × 37.0 ns × cycles fits all four within 0.3% (19.91, 17.07, 14.22 and 31.29 ms predicted), with **no per-pixel overhead**: one bus cycle is 37.0 ns, 27.0 MHz, the crystal. Session 5's 25.9 ns (from the CPU-push pair) is withdrawn: the CPU push has pacing of its own. Consequences:
  - The 30 MHz words keep **every panel 3 field at or above Apple's nanoseconds** on this bus (the 65 ns field gets 2 cycles = 74 ns); 27 MHz words are Apple's nanoseconds exactly, the same 14.2 ms frame.
  - The later word bottoms out at 5 cycles from 30 MHz down to 16 MHz (only its 65 ns field is above one cycle), so a 25 MHz step would repeat 30's frame. Below that, 4 cycles (11.4 ms) is the floor with all four fields at their minimum of one cycle.
  - Apple's 15.2 ms is 5.3 cycles' worth at 27 MHz; with the PLL (stage 4b) Apple's 150 MHz words (16 cycles) would need a ~80 MHz bus for that, so after the PLL switch the bus clock must be re-measured before the words are chosen again.
- **Timing default (dev6) [decision, superseded by dev8's 27 MHz words: "Device session 7"].** Every image (test stages, the `HAVE_BCMFW` boot path, the video pipeline) now uses the **40 MHz words** (`BCMFW_LCD_BUS_KHZ = 40000`): 17.1 ms a frame, every field above Apple's nanoseconds with margin. The test stages and the boot path share their images (a pass flag names the image's hash), so a test-only default would need separate images; the stage 3 sweep selects faster words per frame instead. **27 MHz (14.2 ms, in spec on the measured bus) is the candidate default** once the fine-pattern photos at 27 MHz pass; 100 MHz (`-D BCMFW_LCD_BUS_KHZ=100000`) stays the fallback.
- **The sweep (dev6).** 27, 20 and 15 MHz words: 27 is the candidate (in spec, 14.2 ms); 20 has the same later word with the first word about a quarter under its nanoseconds; 15 takes the later word to 4 cycles (its 65 ns field at 37 ns, 11.4 ms), past the panel's minimum, to find the limit. Each is shown first on the bars with the box (photos 5a–5c), then a **fine pattern** at our own 40 MHz words as the reference (5d) and at each sweep clock (5e–5g): a one-pixel white/black checkerboard (every data bit toggles at every pixel), a `0xAAAA`/`0x5555` checkerboard (neighbouring bits toggle in opposite directions), one-pixel vertical lines and one-pixel horizontal lines (`bcmdt_fine_row`; picture in `device-session-1-photo5d.png`). A marginal bus shows there as speckles, coloured noise, streaks or a shifted line. The devtest simulation checks every one of these frames pixel-exact.
- **Vector unit [H]:** 23 of 24 checks pass; vector stores in the dev5 layout all land. The only failure, MHRN, is one lane: `vmulhrs #2048` of −16 (a tie, exactly −0.5) gave −1, so ties round away from zero (`isa.md`, "Ties round away from zero"). dev6 expects it, and adds informational RNDT, RNDU (ties) and MLSC (`vmulms #16` clamping, the decoder's centre j).
- **The clock line was misread [H].** timer_init's `spin(1000)` is inlined by the compiler as **one** fused `addcmpbne r0, #-1, #0, self` per iteration, not `spin()`'s three instructions. So the 38 ticks are 26.3 M iterations/s of a one-instruction loop, and "~78 MHz" (3,000 / 38) was wrong; the session 4 bullet above is withdrawn. Every measurement now agrees on one picture: a self-dependent instruction completes every ~38 ns (ALU1 26.2 M/s, the clock line, the probe loop's add-cmp-bne chain at 27 M instructions/s), and independent instructions reach ~51 M/s (ALU2, ALU4, MULI, MIX2 all ~0.53 of the dependent time) but no more. The simplest reading: **the core runs near 54 MHz (2 × 27 MHz), issues one instruction per cycle, and a dependent result is ready after 2 cycles** [M]. It is not settled: an ~81 MHz core with 3-cycle latency would let four chains (ALU4) run faster than two, and they don't, which favours 54. The PLL switch (stage 4b, 150 MHz by construction) will settle it. Either way the decoder's 5 M instructions a frame budget assumes the PLL.
- **Why some images time the loop at 75 ticks [H, 12 of 12].** The slow images are exactly those whose inlined loop instruction sits at an address ending in `0xc` within a 16-byte line (`0x…c`, `0x…1c`): session 4 hello (`0xbfc`), session 5 hello_dma (`0xcdc`), session 6 bench (`0xe1c`). The nine fast cases sit at `…0`, `…4`, `…8`, `…a` and `…e` (including `0xc8e`, which crosses a 16-byte line). So a 32-bit taken branch to itself in the last word of a 16-byte line costs a second iteration's time. Every one of the three addresses is also 28 mod 32, so the line may be 32 bytes; no fast case at 12 mod 32 has been seen. The 3-instruction probe loop at 16 offsets shows nothing (its branch is 16-bit). Rule for kernels: keep a hot 32-bit loop-closing branch out of the last word of a 16-byte line (`.align` the loop) until a bench check pins the line size [M].

### Device session 7 (2026-10-11, build p15-dac 87f1e43c0d, dev7 images)

Runs 0-5, 4b and the boot path (logs `session7-*`, complete in `session7b-*`).

- **The PLL works [H]:** stage 4b's 69 kernels all ran 5.56 times faster after Apple's start-up sequence (regulator `bb` → `bf`, PLL A readback `0000006c 00000658`, clock select `00000101`): ALU1 at 135,168 instructions in 5,006 µs before, 901 µs after. **The core runs 27 MHz from a host start and 150 MHz after the switch**, one instruction a cycle on ALU1 in both. Memory and peripheral access scale with it (every kernel, loads and stores included, by the same factor). dev8 still runs every UI image at the start-up clock.
- **The DMA frame is 76,800 × (4 + n) × 37.04 ns [H]**, n the low field of the later timing word: `40000906/40000002` (40 MHz words) 17,099 µs; `40000604/40000001` (27) and `40000303/40000001` (20) both 14,261 µs; `40000202/40000000` (15) 11,422 µs. The first word's fields don't change the frame time. Every sweep frame completed (done counted, `int 2300`, run count `12c00`).
- **The panel showed no difference [owner, by eye]** between the fine pattern at 27, 20 and 15 MHz words and at 40 (photos 5e–5g against 5d). **Decision (dev8): the 27 MHz words, Apple's nanoseconds exactly on this bus, are the default in every image**; 40 and 100 MHz are one-line fallbacks. 20 and 15 MHz are below the panel's minimums and stay out: one unit, by eye, at room temperature is headroom evidence (the 4-cycle floor, 11.4 ms, looked clean), not margin.
- **Host-port upload [H, inferred from stage 5's per-row times]:** about **0.25 µs a pixel** from the PP unboosted (Rockbox's 2-register `ldm`/`stm` loop), so about 19 ms a full frame; session 7's logged "uploads" were mostly the test drawing its pattern (`device-session-1.md`, "Results from session 7"). dev8's upload lab measures it directly.
- **Wake [H]:** 436–458 ms from the image upload to the first frame; the panel's power-on plus first frame is about 283 ms of it (Apple's own wake in the same log: 434 ms).

### The 150 MHz clock in our images (dev9, for session 8)

- **Up** is the 4b sequence, now `bcmfw/clock.c` (the bench calls it; the write sequence still matches `vmcs.bin`'s). **Down** is ours [L]: `+0x28 = 1`, PLL A's control and divider to 0, the regulator back to the level read before the switch (`bb`'s low nibble, `b`), 10 ms. Untested on silicon until stage 4c.
- **Which images switch:** the 4b bench, the video image when `OPEN` carries `HP_OPEN_FAST_CLOCK` (the PP sets it only with 4b and 4c pass flags), and stage 4c's clock-command image. The UI and boot images (hello, hello_dma) never switch: they run on the crystal.
- **What follows the clock in the video image:** the SDRAM refresh (`0xD2` cycles on the crystal, Apple's `0x493` on the PLL; raised after the switch up, lowered before the switch down) and the LCD bus timing (Apple's 150 MHz words on the PLL).
- **Open [stage 4c]:** whether the LCD bus follows the core off the crystal. Apple's 150 MHz words cost 16 bus cycles a pixel on panel 3: about 45 ms a DMA frame if the bus stays at 27 MHz, about 16 ms at half the core (Apple's own 15.2 ms suggests this), about 8 ms at the core's clock. Also open: the PLL's idle draw and the switch's cost against staying at 27 MHz (`pipeline.md`, "Race to idle").

**iFlash (PP side) [H]:** the 1 TB iFlash runs UDMA2 (identify mode `0x42`, word 93 = 0, word 76 = 0); 442 DMA reads and 0 PIO reads in the session.

## SDRAM controller (`0x10003400`)

The order is [H] (vmcs 1.3, re-read for dev10 with `vc2sim --boot --bcm`); the meanings of the fields are [M] unless marked. Apple runs all of it at 150 MHz, after the PLL switch (@14506).

1. **Refresh, then timing.** `+0x24` bits 31:16 = refresh interval in core cycles: `0x0493` (1171 = 7.8 µs at 150 MHz, @14406), read-modify-write keeping bits 15:0. Then `+0x10 = 0x0105008C`, `+0x14 = 0x0105000C` (@1410a; field meanings [L]).
2. **Cache.** In `sdram_init` @147ca, with interrupts off: flush and enable (@296: `0x10000C00` bit 30, wait for bit 0, then bit 31), `0x10000C00 = 0`, then flush and enable again. So bit 31 (the cache) is **on** during the command and the probe. Then `+0x30 = 0x80E8`.
3. **Mode word.** @12d90: `+0x28 = 0x30570000 | a<<12 | b<<8 | c<<2 | 0x30`, then `+0x20 = 7`, then poll `+0x20` bit 15 up to 12,000 times (about 0.56 ms at 150 MHz). **A timeout is ignored**: the probe decides. a = 12 and c = 1. The model byte (@1411e) is a constant `'X'`, so b = 8 (`0x3057C834`). **@12d90 turns b = 7 into 4**, so the fallback word is `0x3057C434`. Guess [L]: a = row bits, b = column bits, so the probe's 0x200 step tests a column wrap.
4. **Geometry probe** (@14842). Save the words at `0xFE000000` and `0xFE000200`, write `0x12345678` and `0x87654321`, and read the first. On a pass it checks a second time, then restores both words. On a failure it restores them, then resets the controller (@14892):
   - flush and enable;
   - clear `0x10000C00` bits 30 and 16;
   - `+0x20 = 0`, then wait up to 8,000 polls for bit 15 to clear.
   Then it sends field 4 and probes again, back to field 8 after that, forever.
5. **Run.** `+0x30 = 0x64`, then `0x10000C00 = (v & ~bit 30) | 0x10000`, which is `0x80010000`.

- `+0x20` is only ever written with 7 (start) and 0 (reset or idle).
- `+0x30` is only ever written with `0x80E8`, `0x64` and 1 (the shutdown at @14318).
- `+0x00`–`+0x0C`, `+0x18`, `+0x1C`, `+0x2C` and `+0x34` are never written.
- No other mode, precharge or refresh commands are sent: command 7 appears to run the whole JEDEC sequence in hardware [M].
- **Our version** is `bcmfw/video/video.c`, `sdram_init`. `pipeline.md`, "The memory map", lists where it differs from Apple's and why (session 9's failure).

**Not needed for bring-up:** Rockbox's framebuffer is in internal SRAM.

## Interrupts and the system timer

- **Vectors [H].** Words `0x00–0x14` are six exception vectors, all pointing at `bkpt; rti` stubs. Words `0x80–0xFC` are 32 interrupt vectors: vector `v = 32 + source` jumps to the address stored at `4·v`. The table is RAM, and handlers are installed by writing it (@14328).
- **Entry and exit [M].** On entry the hardware stores `sr` at `[sp−8]` and the return pc at `[sp−4]`, sets `sp −= 8` and clears `sr` bit 30 (interrupt enable). `rti` reverses this: `sr = [sp]`, `pc = [sp+4]`, `sp += 8`.
- **Exceptions on silicon [H] (device sessions 2–3, `isa.md` "Exceptions on silicon").** Exceptions use the same frame. A misaligned access takes vector 1 (the word at `0x04`). A misaligned load still completes: it loads the word at the rounded-down address into rd, and a pre-decrement form also writes rs back. The exception is imprecise: 2–4 later instructions run first, and the saved pc is the first instruction not run, so `rti` resumes with no adjustment. The bus-error and undefined-instruction vectors are still unmeasured [L].
  - This is the only frame consistent with the code. Nucleus starts tasks through it.
  - In the simulator, 5,006 interrupted contexts out of 5,006 came back with every register intact. The self-test checks this.
  - Light handlers save only what they use. For example, @131fc keeps r0 and r1 in globals and ends in `rti`.
- **Controller `0x10001000` [M].**
  - `+0x00` bits 2:0: current level. Sources at or below it are held off. Nucleus writes it on every interrupt-level change (@207a).
  - `+0x04`: `(priority << 8) | vector` of the highest pending source, or 0 if none. The generic handler (@be2) reads it.
  - `+0x10–0x1C`: a 4-bit priority per source, eight per word, where 0 means disabled (@144b0).
- **Sources (vector = 32 + n):**

| n | Source | Conf. | Where |
|---|---|---|---|
| 0–7 | DMA channel n | [M] | @7dc0 |
| 11 | display TE/vsync. Its handler measures the frame period. GPIO 49 is set up as an edge input next to it. | [M] | @12ae6, @cb1c |
| 12 | unknown (handler @14780) | | @14760 |
| 16–19 | timer compare 0–3. 16 = 10 ms system tick, 18 = software timers (`vclib_timer_*`), 19 = a 32 µs TV line poller | [H] | @bb2, @1d62e, @13a80 |
| 23 | host-port doorbell. The handler acknowledges it by writing `0x10003800 = 1`; a 10 ms software timer also polls the same path. | [M] | @d52a, @d56a |
| 30 | unknown | | @1413e |

- **Timer `0x10001800` [H].**
  - `+0x00`: match status, bit k for compare k (write 1 to clear).
  - `+0x04`: free-running 1 MHz counter. `univ_getstcfreq` returns 1,000,000, and every delay loop counts it in µs.
  - `+0x08/+0x0C/+0x10/+0x14`: compares 0–3.
  - Delay helper: @13bb8 (r0 = µs).

## GPIO (`0x10002800`, 52 pins)

The layout matches the later BCM2835 GPIO block in shape [M; usage H].

| Offset | Use |
|---|---|
| `+0x00–0x0C` | function select, 2 bits per pin, 16 pins per word (0 = input, 1 = output, @13e66) |
| `+0x10` / `+0x14` | set pins 0–31 / 32–51 (write 1s) |
| `+0x18` / `+0x1C` | clear pins 0–31 / 32–51 |
| `+0x20` / `+0x24` | level of pins 0–31 / 32–51 |
| `+0x28` / `+0x2C` | event status (write 1 to clear) |
| `+0x30/+0x34`, `+0x38/+0x3C` | rising- and falling-edge enables [L] |
| `+0x60`, `+0x64/+0x68` | pull mode (0 off, 1 down, 2 up), then a clock strobe of the pin mask. The 150-iteration waits match the BCM2835 GPPUD/GPPUDCLK protocol (@13f7a). |

Pins with a known job [H use; M role]:

- **48**: LCD reset, active low.
- **49**: LCD TE.
- **50, 51**: panel ID.
- **23**: VideoCore→host interrupt line. It is driven high for at least 10 µs, at least 40 µs after it last fell (@1442e). Rockbox wires the ARM end to GPIOC bit 6 (`lcd_init_device`).

## LCD

### Controller (`0x10004000`, an "MPU"-style parallel interface)

| Register | Use | Conf. |
|---|---|---|
| `+0x00` | Control. Bit 0 = interface on. To start an automatic pixel run (@145a2), write `0x231 \| b<<8`, then `+0x04`, then `+0x08`, then `0x229 \| b<<8`. b = 1 when the DMA source reads 32-byte bursts. Reads back `0x325` while a run started with `0x329` goes and `0x323` after it (session 5): bit 3 self-clears, bit 2 busy, bit 1 done. | [H] values; [M] bits |
| `+0x04` | Pixel run length in bus transfers: 76,800 for a 320×240 frame on a 16-bit bus. Reads back the transfers done so far, counting up to the length (session 5) | [H] |
| `+0x08` | Run format word: `0x1F` | [H] value; [L] |
| `+0x0C` | Data FIFO: the DMA destination for pixel runs | [H] |
| `+0x10 + 8k`, `+0x14 + 8k` | Bus timing for chip select k. Fields are cycle counts the code computes from per-panel nanosecond tables and the clock (@12a70, @c87c). Bit 30 of the second word = 16-bit bus: the run length is halved. Values per panel are below. | [H] values; [M] fields |
| `+0x178` | Command / index register write | [H] |
| `+0x17C` | Data / parameter write. 16-bit panels take a 16-bit register value per write; the DCS panel takes one byte per write. | [H] |

The CPU writes commands and parameters straight to `+0x178`/`+0x17C` with no status polling. So the interface stalls the CPU or is faster than the code [M].

### Panel identification

The panel ID is read once at the first power-on (@d142), from **GPIO 51 + 2 × GPIO 50**: bits 19 and 18 of `0x10002824` [H]. A descriptor table per panel holds:

- init scripts made of {16-bit register, 16-bit value} pairs, where register `0xFFF` means "wait value × 100 µs";
- bus timings;
- the window and RAM-write register numbers.

The interpreter is @9264. For the DCS panel it looks commands up in a table of parameter counts (@92a2).

| ID | Controller style | RAM-write / window registers | Bus timing (`+0x10`, `+0x14`) | Reset wait |
|---|---|---|---|---|
| 0 | MIPI DCS (`0x2A/0x2B` window, `0x2C` write, `0x3A=5` 16 bpp, `0x35` TE on) | `0x2C`; `0x2A`, `0x2B` | `0x410D0206`, `0x41010705` | 110 ms |
| 1 | 16-bit index/data, "0x2xx" register map (Renesas-like). Two variants, model 1 or 2 | `0x202`; `0x200/0x201` address, `0x210–0x213` window | `0x41002A19`, `0x40001407` then `0x40000309` | 110 ms |
| 2 | 16-bit index/data, "0x2x/0x4x" map (SSD-like) | `0x22`; `0x20/0x21`, `0x44–0x46` | `0x41012325`, `0x41010605` | 120 ms |
| 3 | as 1, a different panel. Two variants | `0x202`; as 1 | as 1 | 110 ms |

- **[H]: the IDs and descriptors.** Descriptors: ID 0 `0x2225c`; ID 1 `0x23404`/`0x234c4`; ID 2 `0x23130`; ID 3 `0x23274`/`0x23334`. The LCD info block is at `0x22e94` (320×240, then the driver's function table).
- **[M]: the controller families.**
- **The "model" variant [H] mechanism.** `display_control … model=N` (@12a02) selects it, so Apple's ARM firmware decides. **Which model value the 5.5G sends is not known yet**: the answer is in `osos`.
- **Which panel our unit has** can be read without risk once the BCM is powered: `bcm_read32(0x10002824)` from Rockbox, bits 18/19. Rockbox already reads BCM registers this way (`0x10001400`).

### Power sequences (`display_control … power=N`, @f672)

State 0 is off. Going 0 → 2 ("on") does the following [H]:

1. Interface on, timing for the panel.
2. Wait 1 ms; set GPIO 48 to output.
3. Reset pulse on GPIO 48: low 2 ms, high 2 ms, low 2 ms, high, then wait 110 ms (120 ms for ID 2).
4. Six writes of 0 to the command register, 2 ms apart. This resynchronises the bus.
5. The panel's init script.
6. Re-apply the timing (ID 1 and ID 3 switch to a second, shorter timing word).
7. The display-on script, which ends with the full window and the RAM-write command.

2 → 0 runs the sleep script and ends with GPIO 48 low. State 1 is "initialised, display off". Power 4 and −2 also exist and are not needed. Recorded sequences follow. `[xx]` is a bare command, `rr=vvvv` a register write, `cc:pp,…` a DCS command with parameter bytes, `{40xx=…}` a controller write and `waitN` a delay. The order is exact; delays are rounded to the millisecond.

```
panel 0 (DCS), on:
  {4000=1} {4010=410d0206} {4014=41010705} wait1ms GPIO48 output
  GPIO48-lo wait2ms hi wait2ms lo wait2ms hi wait110ms ([0] wait2ms) x6
  2d:<128-byte colour LUT: 32 red 00,03,05..3f; 64 green 00..3f; 32 blue as red> 36:00 26:01 3a:05
  30:00,00,00,ef [11] wait5ms 2a:00,00,01,3f 2b:00,00,00,ef
  {4000=1} {4010=410d0206} {4014=41010705} [29] 35:00 2b:00,00,00,ef 2a:00,00,01,3f [2c]
panel 0, off:  (init script again) [34] [28] [10] wait50ms GPIO48-lo

panel 1 (model 1), on:  timing 41002a19/40001407, reset, sync, then
  0001=0100 0002=2500 0003=02b0 0004=0000 0008=0404 0009=0000 000b=0000 000d=0010 0010=0013
  0011=2600 0012=2200 0013=0802 0014=080c 0015=0816 0016=2200 0031=0000 0080=0031
  0300=0705 0301=0006 0302=0302 0303=0303 0304=0300 0305=0103 0306=001f
  0307=0705 0308=0006 0309=0302 030a=0303 030b=0300 030c=0203 030d=001f
  {4014=40000309} 0400=001d 0401=0001 0007=0001 0205=0060 0110=0001 0100=15b0 0101=0020
  0102=029d 0103=0e5b 0105=0103 0106=0103 0007=0021 0102=02bd wait120ms 0200=0000 0201=0000
  0007=0031 0030=001f wait17ms 0030=03ff 0007=0133 0210=0000 0211=013f 0212=0000 0213=00ef
  0200=0000 0201=0000 [202]
panel 1 (model 2): 0001=0110 0002=1500; gamma 0300-030d as model 1, then 0310-031d (the same
  14 values again) and 0320-032d (0707 0006 0300 0303 0303 0107 001f, twice); after {4014=40000309}
  the order is 0400 0401 0205 0007 (model 1: 0400 0401 0007 0205); 0103=1158
panel 1, off: (init script) 0007=0032 wait51ms 0007=0022 0030=0040 wait17ms 0007=0020 0030=0000
  0100=0580 0101=0060 0102=020d 0007=0000 wait17ms 0100=0700 0100=15b4 GPIO48-lo

panel 2, on:  timing 41012325/41010605, reset (120 ms), sync, then
  0001=011d 0002=0300 0003=1030 0007=0004 0008=0202 000c=0000 0030..0035=0000 0036=0707
  0037=0000 0038=1807 0039=1e08 (timing again) 0007=0004 0013=0000 wait10ms 0011=2202
  000b=e283 0010=2d50 0013=0040 wait50ms 0013=0060 wait50ms 0014=a404 0010=3250 0013=0070
  0071=0317 wait150ms 0044=013f 0045=0000 0046=ef00 0009=0009 0007=0016 wait50ms 0007=0017
  0020=0000 0021=0000 0045=0000 0044=013f 0046=ef00 0020=0000 0021=0000 [22]
panel 2, off: (init script) 000b=6c04 0007=0016 wait50ms 0009=0001 0007=0004 0010=0250
  0013=0060 wait50ms 0013=0040 wait50ms 0013=0000 wait200ms 0010=0000 GPIO48-lo

panel 3 (model 1), on:  as panel 1's timing, reset, sync, then
  0008=0808 0010=0013 0011=0300 0012=0101 0013=0a03 0014=0a0e 0015=0a19 0016=2402 0018=0001
  0090=0021 000d=0010 0003=0230, gamma 0300-030d 0310-031d 0320-032d
  (0707 0207 0700 0303 0301 0707 1f07 0606 0507 0701 0303 0302 0706 1f19 ×2, then
   0607 0206 0700 0302 0302 0707 1a13 0606 0507 0702 0303 0303 0707 1f1f)
  {4014=40000309} 0400=001d 0401=0001 0205=0060 0007=0001 0031=0071 0110=0001 0100=17b0
  0101=0220 0102=009b 0103=1000 0105=0103 0106=0104 0007=0021 0102=00bb 0001=0110 0002=0500
  0007=0031 0030=0007 wait17ms 0030=03ff wait34ms 0007=0072 wait85ms 0007=0173
  0210=0000 0211=013f 0212=0000 0213=00ef 0200=0000 0201=0000 [202]
panel 3 (model 2): gamma 0300-030d (0707 0002 0603 0303 0100 0307 1719, twice),
  0310-031d (0707 0204 0605 0303 0200 0307 1719, twice), 0320-032d (0607 0001 0701 0303 0300
  0707 1f19, twice); 0103=1200 0106=0105 0102=00bd
panel 3, off: (init script) 0007=0172 0030=03ff wait51ms 0007=0120 0030=0000 0100=0780 0007=0000
  0101=0260 0102=00a9 wait17ms 0100=0700 0100=0704 GPIO48-lo
```

The bus timing values were computed at the simulated clock. At a slower core clock the same words give longer, so safer, bus cycles [M].

Settled while writing our image (milestone 3, `bringup.md`) [H]:

- **Shape.** Each panel's handler runs an *init* part on both ways. Power-on is init + on part, power-off is init + off part. Init is the first block of each listing above, up to the gamma registers or the timing re-apply. Panel 0's init ends with `2b:…`; panel 2's ends with `000c=0000`.
- **Widths.** Every access to the LCD interface, GPIO and timer registers is a 32-bit load or store.
- **Sign-extended values.** For panel 2, Apple writes some values sign-extended: `0xFFFFE283`, `0xFFFFA404`, `0xFFFFEF00`. On a 16-bit panel bus the upper half is then evidently ignored [M]. Our image writes values zero-extended.
- **Frame window order** (the window function, @12ba0/@12bf6), then the RAM-write command:
  - panel 0: `2b`, `2a`;
  - panels 1 and 3: `0210–0213`, `0200`, `0201`;
  - panel 2: `0045`, `0044`, `0046`, `0020`, `0021`.
- **Timings, kick to RAM-write.** Measured in vc2sim on our image with the same delays: panel 0 134 ms, panels 1 and 3 266 ms, panel 2 449 ms.

### Bus timing at the host-start clock (dev4)

**Apple's formula (@12a70, @c87c) [H].** `display_control` calls @12a70 with the clock in Hz and the timing variant; it computes `+0x10` from the panel descriptor's table at `+168` and `+0x14` from `+176` (variant 0) or `+184` (variant 1), each four halfwords of nanoseconds n0..n3. With k = clock / 1000 and cyc(n) = max(⌈n·k / 10⁶⌉ − 1, 0), the word is `0x40000000 | cyc(n0)<<24 (6 bits) | cyc(n2)<<16 (6 bits) | cyc(n3′)<<8 (7 bits) | cyc(n1) (7 bits)`, where n3′ = n3 less the nanoseconds that rounding n0 and n2 up already added. Our Python copy reproduces all of Apple's 150 MHz words for every panel (`bcmfw/tests/hello_test.py`, row "bus timing").

| Panel | `+0x10` ns | `+0x14` ns (power-on) | `+0x14` ns (later) | Words at 150 MHz (Apple) | Words at 100 MHz (bcmfw default) |
|---|---|---|---|---|---|
| 0 | 10/45/90/25 | 10/35/10/55 | as power-on | `410D0206 41010705` | `40080204 40000503` |
| 1, 3 | 10/170/5/285 | 0/50/5/145 | 0/65/5/30 | `41002A19 40001407 40000309` | `40001B10 40000C04 40000106` |
| 2 | 10/250/10/240 | 10/40/10/50 | as power-on | `41012325 41010605` | `40001718 40000403` |

**What paces a pixel [M].** Apple's DMA frame takes 201 ns per pixel at 150 MHz, so the pixel writes cannot be paced by panel 3's `+0x10` (72 cycles = 480 ns); the later `+0x14` word (16 cycles, 107 ns, plus DMA overhead) fits. Session 4's reading (638 ns for 16 cycles, so a ~25–27 MHz bus) assumed no per-pixel overhead; session 5's pair measures the cycle directly (below).

**bcmfw's choice (dev4).** `BCMFW_LCD_BUS_KHZ` (default 100,000) recomputes every word with Apple's formula from Apple's nanoseconds for that clock: each field then lasts at least Apple's nanoseconds on any bus clock up to 100 MHz (79 MHz measured, plus 25%), and no field is above Apple's. 0 restores Apple's words. The host can ask for Apple's words at run time (mailbox `0x170` = "APPL"); the image reports the words it applied (`0x174`/`0x178`) and its clock (`0x17C`). The device test's stage 2 now times one frame with each.

**Measured (session 5) [H].** With our 100 MHz words a full frame takes 38,966 µs by CPU and 31,355 µs by DMA; with Apple's 48,927 µs by CPU. The difference puts the bus at ~39 MHz (25.9 ns a cycle; "Device session 5" above), so our words still give every field about 2.6 times the panel's minimum. The model for one pixel by DMA is about (cycles × 25.9 + 120) ns.

**The sweep (dev5, stage 3; dev6 moved it below the new 40 MHz default, see "Device session 6").** Each image also carries the words for 50, 40 and 30 MHz (`config.h` `BCMFW_SWEEP_KHZ_k`, host selection `"SWP" | k` at `0x170`); stage 3 pushes one frame by DMA at each, logs both frame times and holds it for a photo. Panel 3's later word is 7, 6 and 5 cycles there (11 at 100 MHz, 16 at Apple's 150). Expected by the model above: about 23, 21 and 19 ms a frame. 50 and 40 MHz keep every field at or above Apple's nanoseconds on a 39 MHz bus; 30 MHz cuts them by about a quarter (the `b` field gets 52 ns for 65), so a photo with wrong colours or shifted lines there is a result, not a fault. Apple's 15 ms needs the faster bus of the PLL switch (stage 4b) as well as tight words.

**Cross-check [L].** Broadcom's later BCM2835 documents a 32-byte DMA control block of the same shape, but there the 2-D length's low half is in bytes and completion sits in the channel's CS register (bit 1 END, bit 2 INT). Apple's VC II code uses units and the `+0x04` word (above), so we follow Apple's code; the BCM2835 names are hints only.

### How Apple sends a frame

The sequence is [H] from the simulator (@10c9c, @96fc, @145a2):

1. The window function writes the full window (`0..319`, `0..239`) and then writes the RAM-write register number to `+0x178`.
2. A DMA control block is posted with:
   - source: the image in memory;
   - destination: `0x1000400C`;
   - length in 4-byte units, or rows/columns for a sub-rectangle;
   - transfer-info word `0x1900A051` (2-D: `0x1901A021`), paced by the LCD's request line (DREQ 3).
3. The controller is armed: `+0x00 = 0x331`, `+0x04 = 76800`, `+0x08 = 0x1F`, `+0x00 = 0x329`.

Pixels are 16-bit RGB565 [H for the DCS panel: `3a:05`, `36:00`]. The 16-bit panels get the same stream. Their entry-mode registers (for example `0003=1030` on ID 2) set the colour order [M].

## DMA engine (`0x10000800`, eight channels 0x80 apart)

| Offset in channel | Use | Conf. |
|---|---|---|
| `+0x00` | Channel configuration and state. Written with a transfer-info-like word: `0x09032101` for the LCD (DREQ 3 in bits 20:16), `0x0200FF01` for TV, `0xE802` for memcpy. Bits 31:29 read back a state (the TV poller waits for 2), bit 0 = enable. Bits 28:24 identify the owner when a channel is shared. | [H] values; [L] fields |
| `+0x04` | Status, write 1 to clear. Apple's channel handler (@7b7c) reads it, writes the same value back, and calls each of the channel's two registered users whose mask meets it. Masks: the LCD frame push registers **`0x100`** (@145e2), the generic transfers `0x800 \| 1 << (13 + k)` for sub-channel k (@804c), TV `0xFFFFFFFF` (@13a1e). So the LCD's completion is **bit 8**, and bit 13 + k reads as "sub-channel k's chain ended". Session 4 read `0x2300` after our full frame on sub-channel 0 (bits 8, 9, 13; bit 9 unknown). Bit k (what bcmfw polled up to dev3) is not a completion bit. | [H] masks and the session 4 value; [M] meanings |
| `+0x38 + 0x20·k` | Sub-channel k (0–2): writing a control-block address starts it; reading gives the current block | [H] write; [M] |

Control block: 32 bytes, 32-byte aligned (@7fc4). Fields [H] from `dma_transfer_setup_*`:

| Offset | Field |
|---|---|
| `+0` | next block, or 0 |
| `+4` | source |
| `+8` | destination |
| `+12` | length in units of 2^width bytes; 2-D: `rows << 16 \| units per row` |
| `+16` | transfer info |
| `+20` | 2-D strides in units: source in bits 15:0, destination in bits 31:16. Each is the gap after a row (pitch − row width), not the pitch: `dma_transfer_setup_memcpy2d_uncached` (@816e) computes it that way [H] |
| `+24/+28` | 0 |

Transfer-info bits:

- bits 6:4: source width, log2 bytes [H]
- bits 14:12: destination width [H]
- bit 15: destination paced by DREQ [M]
- bit 16: 2-D [H]
- bit 17: destination increments [M]
- bit 27: uncached path. `dma_transfer_setup_memcpy` clears it, the `_uncached` variant keeps it [M].
- bit 0: always set [L]
- bit 24: set on the last block of a chain only; Apple's per-row chains clear bits 24 and 25 on every other block (@831e), so probably "interrupt when done" [M]
- bit 28: set on peripheral destinations [L]
- the length unit is the smaller of the two widths: the 2-D setup divides the row width by the smallest of the three alignments [M]

The widths are the largest power of two up to 32 that divides the length and both addresses. `dma_memcpy(dst, src, n)` uses channel 7 sub-channel 1. Its 4 KB copy gives the block `{0, src, dst, 0x80, 0x01025151, 0, 0, 0}`. It then sleeps on a semaphore that the channel's interrupt releases [H]. `dma_memcpy2d_uncached` builds the 2-D form with base `0x09020101`. Other users: channel 1 = LCD pixels, 5 = audio (FIFO `0x10004804`), 6 = TV (FIFO `0x10002C04`).

## Host port, as the VideoCore sees it

- **Writes land in SRAM [H].** Everything Rockbox writes through the host port lands in VideoCore memory at the address it gives: the image at 0, the command at `0x1F8` and the framebuffer at `0xE0000` (`lcd-video.c`). The host can also read and write peripheral registers directly: `0x10000C00`, `0x10000400` and `0x10001400` (Rockbox `bcm_init`/`bcm_powerdown`).
- **The doorbell [M].** Rockbox writes `BCM_CONTROL = 0x31` after each command word. On the VideoCore side, interrupt source 23 (vector 55) is the host doorbell, acknowledged by writing `0x10003800 = 1` (@d56a). `vmcs.bin` also polls the same path every 10 ms from a software timer.
- **Apple's protocol [H].** The disk `vmcs.bin` uses its own host protocol: a control block whose address it stores at `0x1FC`. Channel bits toggled by the host sit at `+16`, the VideoCore's acknowledgements at `+32`. Eight channels dispatch callbacks (@d464). The VideoCore→host direction uses `+17`/`+33` and a pulse on GPIO 23. **It does not use Rockbox's `0x1F8` command word.** That contract belongs to the NOR-flash `vmcs`, which we do not have: it lives in the iPod's flash, and `aupd` in the updater is encrypted. So the `BCMCMD_*` semantics come from Rockbox's host side only.
- **The ALT bank (`0x3004..7_0000`).** It is only used during Rockbox's bootstrap handshake. Apple's ARM driver selects it per call, so it is probably a second, independent port [M] (`protocol.md` §3).

### Rockbox's contract (`lcd-video.c`, the ground truth)

| Host action | What the image must do |
|---|---|
| Upload to 0, write `0x1F8 = 0`, write `0x10000C00 = 0xC0000000`, wait for bit 0, write `0x10000C00 = 0`, write `0x10000400 = 0xA5A50002`, wait until `0x1F8 ≠ 0` | Start at `0x200`. Once ready, write a value to `0x1F8` that is not 0. The host flushes the cache and leaves it **off**. |
| Write a partial or full frame into `0xE0000+`, write `0x1F8 = BCMCMD_LCD_UPDATE (0xFFFF0000)`, then `CONTROL = 0x31` | Push the whole 320×240 frame to the panel. The first update after power-up must also run the panel power-on (Rockbox allows 500 ms). The host treats `0xFFFF0000` or `0xFFFF` in `0x1F8` as busy, and will rewrite and re-kick after 50 ms. So write `0xFFFF` while busy and any other value when done. |
| `BCMCMD_LCD_SLEEP (BCM_CMD(8))`, then `BCM_CMD(0xC)`, then the host cuts BCM power (`GPO32 &= ~0x4000`) | 8: run the panel's off sequence, then report done. 0xC: report done. Power goes away next. |
| `bcm_powerdown` also clears bits 7:4 of `0x10001400` from the host | Nothing: those are regulator/power bits (above). |
| `UPDATERECT` (5), `SELFTEST` (1), TV commands (2, 3, 14) | Rockbox never sends these. Report done for any unknown command so the host never hangs. |

## What our bring-up image must do (answering `BCMCMD_LCD_UPDATE`)

1. **Entry and stack.** Entry at `0x200` [M] (Apple's image does this; the reset pc is not proven). Set sp in SRAM below `0xE0000`. Leave the vector words `0x00–0x1FF` zero or pointing at a halt stub. Do not touch `0x1F8`/`0x1FC` except as below.
2. **Cache off.** Leave `0x10000C00` alone, so the host's writes to the framebuffer are seen directly. If the cache is enabled later, flush it (bit 30, wait for bit 0) before reading each frame.
3. **Clocks: optional at first.** The panel and bus work at whatever clock the chip starts on, with Apple's timing words (slower is safer). To run at 150 MHz, copy the start-up order in "Clocks". SDRAM is not needed.
4. **Interrupts off.** Write 0 to all source priorities (`0x10001010–1C`); `di`. Poll instead.
5. **Panel ID.** Set the function select of GPIO 48 to output (`0x1000280C` bits 1:0 = 1) and leave 50/51 as inputs. `id = bit19 + 2·bit18` of `0x10002824`.
6. **Power on the panel.** Use the sequence for that ID (above), with the delay loops on the 1 MHz counter `0x10001804`: interface on, timing words, GPIO 48 reset pulse, six zero commands, init script, display-on script.
7. **Signal ready.** Write a non-zero value that is neither `0xFFFF0000` nor `0xFFFF` to `0x1F8`, for example `0x00000001`.
8. **Main loop.** Poll `0x1F8`:
   - On `0xFFFF0000` (`LCD_UPDATE`):
     1. write `0xFFFF`;
     2. set the full window and write the RAM-write command (`0x2C`, `0x202` or `0x22`) to `0x10004178`;
     3. write the 76,800 pixels from `0xE0000` as 16-bit values to `0x1000417C`;
     4. write `0x1`.
   - On `BCM_CMD(8)`: run the off sequence, then write `0x1`. On anything else: write `0x1`.
   - Acknowledge the doorbell at `0x10003800` if it is seen pending (harmless).
9. **Faster later.** Replace step 8.3 with the DMA path (control block to `0x1000400C`, controller run registers) and add 2-D updates. The vector unit can then scale and compose (`approach.md`).

Every step above can be run in `vc2sim` first, against the same models that boot `vmcs.bin`.

## Bring-up image sketch (implemented: `bcmfw/`, decisions in `bringup.md`)

- **Layout.** `0x000–0x1FF`: zero, except the vector words, which point at one halt stub (`bkpt` loop) so a stray exception is visible. Code and tables start at `0x200`. Stack top at `0xD0000`, below the framebuffer. Nothing in SDRAM.
- **Size.** A few hundred instructions: start-up, a µs delay on `0x10001804`, a script interpreter for {register, value} / delay / DCS-parameter records (our own format, holding the four panels' sequences), the frame copy loop and the command loop. Written in vc2dis assembly first; C through `vc2cc` once that is ready (`compiler.md`).
- **Simulator test before any device time.** Add an LCD panel model to `vc2mmio`: it records commands, tracks the window and RAM-write pointer, and collects pixels into a 320×240 buffer. Then run the image with `--boot`:
  1. the host side writes a test frame to `0xE0000` and `0xFFFF0000` to `0x1F8`, as `lcd-video.c` does;
  2. check that the panel model received the expected init sequence for each panel ID;
  3. check that the frame matches pixel for pixel;
  4. check that `0x1F8` goes `0xFFFF` and then to the done value.

  The same harness then measures the frame time in instructions.
- **First device run** (owner permission and a fresh backup, through `bcm_init()`): read the panel ID, upload, one `LCD_UPDATE` of a test pattern. Rockbox's NOR `vmcs` stays the fallback: a power cycle restores it.

## Still unknown (needs the device or more static work)

| Question | How to settle it |
|---|---|
| Which panel ID and which model variant this unit has | Device: read `0x10002824` through the host (read-only, safe). Model: static, in `osos` (`display_control … model=%d`). |
| Reset pc = `0x200` for a host-started image | Device: the first bring-up run. Fall back to a branch at 0. |
| Whether CPU writes to `0x1000417C` are bus-paced; the pixel width per write on the DCS panel | Device (first light). Apple only ever writes there without polling. |
| Field meanings of `0x10000400/0408`, the clock controls `+0x28–0x58`, the `0x10004000` control bits, DMA `+0x00` | Static (more `vmcs.bin` and RenderServer callers) and device tests |
| Interrupt sources 12 and 30; the host doorbell's exact trigger (which `CONTROL` bits) | Static in `osos` (ARM side); device |
| The NOR `vmcs`'s exact ready value and command semantics | Not needed: the contract above is Rockbox's, and our image defines the rest |

**Verdict:** the facts above are enough to write a bring-up image: start-up, panel ID, panel power-on, the CPU pixel path and Rockbox's command loop. Each step can be run in the simulator first. The first device run needs only:

1. a read of the panel ID;
2. one upload through Rockbox's existing `bcm_init()` path, with owner permission and a backup.
