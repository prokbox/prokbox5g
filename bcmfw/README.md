# bcmfw: our VideoCore II image for the BCM2722

`bcmfw` is ProkBox's own firmware for the BCM2722 video chip in the iPod Video 5G/5.5G. It's written from scratch, in C plus a little native VideoCore II assembly, and licensed under the GPL. Apple's binaries are reference material only: nothing from them is in this directory or in the image (issue #60, `docs/research/bcm2722/approach.md`).

Milestone 3 ("hello BCM") answers the commands Rockbox's LCD driver sends (`firmware/target/arm/ipod/video/lcd-video.c`):
- it powers on whichever of the four LCD panels is fitted;
- it copies the host's frame at `0xE0000` to that panel.

So far it has only been proven in the simulator. Decisions and the device checklist are in `docs/research/bcm2722/bringup.md`.

## Build

```sh
bcmfw/build.py                       # -> build/bcmfw/bcmfw.bin, .map, .lst
bcmfw/build.py -D BCMFW_IDLE_SLEEP=1 --out build/bcmfw/idle
bcmfw/build.py -D BCMFW_HOSTPROTO=1 -D BCMFW_JOBS=1 --overlay --out build/bcmfw/do
                                     # the display-only image and its overlay
```

The build needs `vc4-elf-gcc` and our recompiled libgcc (`docs/research/bcm2722/compiler.md`, "Building the toolchain"; `VC4_GCC` and `VC2CC_LIB` override the paths). `tools/bcmre/vc2cc.py` compiles the C, assembles the native `.S` files and lays out one flat image, with these options:
- `--entry` taken from `BCMFW_ENTRY` in `config.h`;
- `--no-float`;
- `--conservative`: only instruction kinds that Apple's own code exercises in the vc2sim self-test.

`lib/` is linked on demand, like an archive.

Current size: **4,020 bytes** loaded (vectors and mailbox 512, code 1,808, tables 1,700) plus 20 bytes of `.bss`. That's 0.31% of the 1.25 MB of internal SRAM. The 16 KB stack sits just under `0xD0000`.

## Test

Every test runs vc2sim with **strict alignment on** (`VC2SIM_STRICT_ALIGN=trap`, the default): an access in a form whose silicon behaviour is unknown stops the test (`AlignTrap`) instead of passing under the model's guess; the host-port models drop a write address's bits 1:0, as the port does. `VC2SIM_STRICT_ALIGN=log` counts such accesses instead, `0` turns the check off. Policy and the known forms: `docs/bcm2722-reference/memory-access.md` §11. `bcmfw/tests/halfload_test.py` holds the halfword loads' readings (quirks #16, #33).

```sh
bcmfw/tests/hello_test.py [-v]       # ~26 s; skips (exit 0) without vc4-elf-gcc
bcmfw/tests/dma_test.py [-v]         # ~16 s; the BCMFW_LCD_DMA=1 build
bcmfw/tests/hostproto_test.py [-v]   # ~14 s; the BCMFW_HOSTPROTO=1 build
bcmfw/tests/bench_test.py [-v]       # ~22 s; the BCMFW_BENCH=1 timing bench
bcmfw/tests/disp_test.py [-v]        # ~20 s; the display kernels against dispref.py
bcmfw/tests/video_test.py [-v]       # several minutes; BCMFW_VIDEO=1 end to end
bcmfw/tests/viz_test.py [--mutations] # ~15 s; the visualizer's FFT, bands and bars (#126)
bcmfw/tests/vclk_test.py             # ~2 min; the level bands (P19 PW4): Apple's order, the speed check, the fallbacks
bcmfw/tests/vcheck_image_test.py     # ~6 min; the first-use check's own C (vcheck.c, bcmvp.c) against the merged image
```

The bench runs the image in `vc2sim` against two models:
- a port of `lcd-video.c`'s host logic: upload, start, frame writes, `lcd_tick`, `bcm_command`, `lcd_sleep` and `lcd_awake`;
- `vc2mmio.LcdPanel`, which records the panel bus and collects pixels. Each captured frame is written to a PNG under `build/bcmfw/test/`, read back, and compared pixel for pixel with the host's framebuffer.

Power sequences are checked two ways:
- against the facts in `hardware.md`;
- when `vmcs.bin` 1.3 is present, event for event against Apple's own power handler running in the simulator on the same panel model. Our delays must be at least as long as Apple's.

The bench reports cases, instruction counts and the instruction kinds the image executes. Results are in `bringup.md`.

## The host contract

Addresses are in BCM SRAM, as the host sees them through the host port.

| Address | Who writes | Meaning |
|---|---|---|
| `0x000–0x0FF` | image | 64 vector words. Vector k points at its own stub (`bcmfw_vstubs + 8k`), which saves r0-r5 and lr and enters the common handler with its number; except vector 49 (timer compare 1) in the idle-sleep build, and word 0 in reset-branch builds (a branch to `_start`, or to `probe_from0` in the probe). |
| `0x100–0x13F` | both | Video protocol header and ring indices (`BCMFW_HOSTPROTO=1` only; zero otherwise). See `docs/research/bcm2722/hostproto.md`. |
| `0x140`–`0x14C` | image | `BCMFW_CLOCK` builds (dev9): the clock state (`0x434B0000` "CK" \| bit 0 on PLL A at 150 MHz, bit 1 a wait hit its spin limit), the readback (regulator before `<< 24` \| regulator now `<< 16` \| clock select `+0x28`; PLL A M `<< 16` \| control `+0x14`) and the switch times in µs (up in bits 15:0, down in 31:16) |
| `0x168` | host | `BCMFW_CLOCK_CMD` builds: `0x434C4B00` ("CLK") \| 1 = switch to 150 MHz, \| 0 = back to 27 MHz, applied by `BCM_CMD(0x41)` |
| `0x170` | host | `0x4150504C` ("APPL"): use Apple's 150 MHz bus timing words from the next command on; `0x53575000 \| k` ("SWP" k, dev5): the words for sweep clock k (`BCMFW_SWEEP_KHZ_k`: 27, 20, 15 MHz since dev6); anything else: the image's own (`BCMFW_LCD_BUS_KHZ`) |
| `0x174`–`0x17C` | image | The bus timing words last applied (`0x10004010`, `0x10004014`) and the clock in kHz the words in use are computed for (0 = Apple's) |
| `0x180`, `0x184` | image | The last bounded wait that ran out: `kind << 24 \| count` (1 = a counter delay ended on spins because the counter stopped mid-wait, not fatal; 2 = a DMA completion never came, CPU re-send), and the register value it last read. 0 = none |
| `0x188` | image | Clock-enable record: bits 11:8 = `0xC` (valid); bit 0 counter ran at start, 1 started by `0x4C`/`0x54`, 2 started by `0x58`, 4/5 those writes made, 6 implausible rate (spin delays); bits 31:16 counter ticks over `spin(1000)` (3,000 instructions: about 3000 / ticks MHz) |
| `0x18C`–`0x194` | image | `0x10001C4C`, `0x10001C54`, `0x10001C58` as the image found them, before any write |
| `0x198`, `0x19C` | image | DMA build (`BCMFW_DMA_READBACK`), the last push: the LCD interface's `+0x00 << 20 \| +0x04` (control, run length) read right after arming and again one poll in (`0x198`), and at the end (`0x19C`). The run length counts the transfers up (device session 5): it ends at the length written when every transfer left the interface; the control reads `0x325` while the run goes and `0x323` after |
| `0x1A0` | host | DMA build: `0x464F5243` ("FORC") makes the next push post no control block, so it times out and is re-sent by CPU (the device test's forced timeout; the image clears it) |
| `0x1A4`–`0x1BC` | image | DMA build, the last push: its time on the 1 MHz counter, pushes completed, pushes timed out, then channel 1's config, status and sub-channel 0 words and the polls, read at its end |
| `0x1C0` | image | Step word, every build: `0x5053xxxx` (`config.h`, `BCMFW_STEP_*`: start, clock writes, counter check, panel ID, idle, command received, each power-on step and script record, every 16 rows of a CPU push, the DMA wait, done). Probe and bench builds then use it for their own `0x5052xxxx` codes |
| `0x1C4–0x1D0` | image | Probe builds: started-at-0, PASS (bit n = required check n passed), ALL (`0x1FF`), counter ticks over a 1000 spin. Standby builds (`BCMFW_LCD_STANDBY`) use `0x1C8`–`0x1D0` instead: the host's `0x53425900 \| depth` request, the standby state (bit 8: refused) and the last panel power-on's time (bit 31: out of standby) |
| `0x1D4` | image | Bench builds: the bench results block's address (`0xC0000`) |
| `0x1D8` | image | Probe builds: bit n = check n faulted (and was resumed) |
| `0x1DC` | image | Probe builds: the probe v2 results block's address (`0xCB000`) once its header is valid, else 0 |
| `0x1E0`, `0x1E4` | image | Fault pc and sp (the exception frame's pc and address), written by the handler's legacy stop before the status word |
| `0xCB000–0xCB2BF` | image | Probe builds: the probe v2 results block (`docs/research/bcm2722/bringup.md`, "Probe v2") |
| `0x1E8` | image | Info word: `0x5042` in bits 31:16; panel ID in bits 1:0; model in bits 7:4; `0x100` panel powered; `0x200` 1 MHz counter running; `0x400` idle-sleep build; `0x800` DMA build; `0x1000` DMA switched off after a timeout; `0x8000` standby build |
| `0x1EC` | image | LCD updates completed since start |
| `0x1F0`, `0x1F4` | host | `LCD_UPDATERECT` parameters: `x \| y << 16`, `w \| h << 16` |
| `0x1F8` | both | Command and status word (`BCMA_COMMAND`) |
| `0xE0000–0x1057FF` | host | Frame: 320×240 RGB565, rows of 640 bytes |

**Start.** The host uploads the image to 0, writes `0x1F8 = 0`, does the cache flush through `0x10000C00`, then writes `0x10000400 = 0xA5A50002`. The core starts at `BCMFW_ENTRY` (0x200). It:
1. disables interrupts and sets up its stack;
2. clears `.bss`;
3. sets every interrupt priority to 0;
4. checks that the 1 MHz counter is running; if it isn't, makes Apple's counter-clock writes (`0x10001C4C`/`54`, then `58`; `hardware.md`, "After a host start") and checks after each, falling back to spin delays if it still doesn't count;
5. reads the panel ID from GPIO 51/50.

It then writes `0x50420001` (`BCMFW_STATUS_DONE`) to `0x1F8`, which is the "ready" that `bcm_init()` waits for. Apart from those counter-clock writes (`BCMFW_CLK_ENABLE=0` turns them off) it touches no clocks, no PLL, no regulator, no SDRAM, no DMA and not the cache.

**Commands.** The host writes `BCM_CMD(x) = ~x << 16 | x` to `0x1F8`. The image writes `0xFFFF` while it works and `0x50420001` when it's done. lcd-video.c reads both `0xFFFF0000` and `0xFFFF` as busy, and anything else as idle.

| Command | What the image does |
|---|---|
| `LCD_UPDATE` (0) | Powers the panel on if needed, which takes 136–451 ms depending on the panel. Then it sets the full window and writes all 76,800 pixels from `0xE0000`. |
| `LCD_UPDATERECT` (5) | Our definition, since the NOR `vmcs`'s parameters are unknown and Rockbox never sends this command. It clips the rectangle at `0x1F0/0x1F4`, sets a panel window and sends only those pixels. An empty rectangle, or one that's off screen, sends nothing. |
| `LCD_SLEEP` (8) | Runs the panel's power-off sequence and leaves reset (GPIO 48) low. The next `LCD_UPDATE` powers the panel on again. |
| `0xC`, `SELFTEST` (1), TV (2, 3, 4, 14), anything else | Done at once, with no panel traffic. A word that isn't in `BCM_CMD` form is also just acknowledged. |

If the host rewrites the command while the image is busy (lcd-video.c re-kicks after 50 ms), the image doesn't overwrite it. It runs the new command next. The image never leaves `0x1F8` at a value that would make `bcm_command()` wait forever, except after a fault.

**Fault.** An unexpected exception or interrupt writes the frame's pc and sp to `0x1E0/0x1E4`, then `0xFA170000` to `0x1F8`, then sleeps (so does a fault inside the handler). In probe builds, a fault inside a self-check check is recorded and the self-check goes on with the next check; any other fault also fills record U of the results block first. The host sees "not busy", so it won't hang. Nothing in lcd-video.c checks for this value yet.

## Switches (`config.h`, or `-D` on `build.py`)

Each switch stands for something only a device run can settle (`bringup.md`, "First device session").

| Switch | Default | Use |
|---|---|---|
| `BCMFW_PANEL_ID` | -1 (read GPIO) | Force a panel ID, 0–3 |
| `BCMFW_LCD_BUS_KHZ` | 27000 | The clock the LCD bus timing words are computed for, with Apple's formula from Apple's per-panel nanoseconds (`panels.c`, `hardware.md` "Bus timing at the host-start clock" and "Device session 6"). The bus runs at 27 MHz: 40000 keeps every field above Apple's nanoseconds (17.1 ms a frame), 27000 is Apple's nanoseconds exactly (14.2 ms, the candidate), 100000 the dev5 fallback (31.4 ms). 0: Apple's 150 MHz words unchanged |
| `BCMFW_PANEL_MODEL` | 1 | Variant 1 or 2 for panels 1 and 3. Apple's ARM side chooses it; which one the 5.5G uses is unknown. |
| `BCMFW_IDLE_SLEEP` | 0 | 1: `sleep` between polls, woken by a 1 kHz timer interrupt, instead of busy-polling |
| `BCMFW_IDLE_GATED` | 0 | Idle-sleep builds: sleep only while the host has written `"SLP1"` to `0x16C` (the resident video image; stage 5s's image) |
| `BCMFW_LCD_STANDBY` | 0 | 1 (stage 5s's image): `BCM_CMD(0x43)` takes the panel dark with its reset line held high, light or deep as `0x1C8` asks, so the next update wakes it without the reset pulse; every panel power-on timed at `0x1D0` (`docs/bcm2722-reference/host-driver.md`, "Panel standby"). Not with the probe, bench or video builds (their words) |
| `BCMFW_RECT_WINDOWED` | 1 | 0: `UPDATERECT` sends the whole frame (the window registers' meanings are [M]/[L]) |
| `BCMFW_PIXEL_PACE` | 0 | N > 0: one pixel per write, then N spin iterations, in case CPU writes overrun the LCD FIFO |
| `BCMFW_SPIN_PER_US` | 50 | Delay-loop calibration if the 1 MHz counter doesn't run: a full microsecond up to 150 MHz (the chip measured ~27 MHz after a host start) |
| `BCMFW_CLK_ENABLE` | 1 | 1: start the counter's clock with Apple's writes if it doesn't run; 0: no clock-block access at all |
| `BCMFW_RESET_BRANCH` | 0 | 1: also put `b _start` at address 0, in case the core starts at 0 |
| `BCMFW_HOSTPROTO` | 0 | 1: the video host protocol skeleton (`hostproto.c`): header at `0x100`, command and event rings at `0xD0000`, `HELLO`, `OPEN`/`CLOSE` with the null codec, `FEED`, `STATUS`, `PING` |
| `BCMFW_LCD_DMA` | 0 | 1: frames and rectangles go to the LCD by DMA (`dma.c`), paced by the interface's request line. Odd rectangles, and every frame after a DMA timeout, go by CPU. |
| `BCMFW_DMA_TIMEOUT_US` | 100000 | DMA build: give up on a transfer after this long, re-send by CPU and leave DMA off until the next upload |
| `BCMFW_DMA_POLL_US` | 64 | DMA build: how often the completion bit (status bit 8, Apple's LCD mask) is polled |
| `BCMFW_DMA_READBACK` | 1 | DMA build: read the LCD interface's control and run-length registers back after arming and at the end of each push (`0x198`/`0x19C`) |
| `BCMFW_DMA_MARK` | 8 | DMA build: after a timed-out push and its CPU re-send, paint the rectangle's first N rows black, so a photo tells a CPU frame from a DMA frame (0: no mark) |
| `BCMFW_BENCH` | 0 | 1: the timing bench (`bench/`): a probe build that, after reporting ready, times instruction mixes against the 1 MHz counter, runs on-chip checks of the decoder's unproven vector and scalar forms, and fills a results block at `0xC0000` (address also at `0x1D4`; progress in the probe's step word at `0x1C0`). Then it answers commands as the probe does. Implies `BCMFW_PROBE`. `docs/research/bcm2722/bringup.md`, "Device session 2". |
| `BCMFW_BENCH_PLL` | 0 | Bench builds: after the first timing pass, switch the core to 150 MHz with Apple's start-up sequence (clock block, run control, regulator to level 15) and time everything again. Changes clocks and the core supply: off by default, risks in `bringup.md`. |
| `BCMFW_BENCH_ITERS` | 4096 | Bench builds: loop iterations per timed run (the simulator bench uses 256) |
| `BCMFW_VIDEO` | 0 | 1: video (`docs/research/bcm2722/pipeline.md`). The H.264 decoder (`h264/`) behind the host protocol: `OPEN` with codec 1, `FEED` packets from the SDRAM FIFO decoded between command passes, each picture converted to RGB565 on the vector unit (2:1 box above 320×240, letterboxed) and pushed by DMA while the next one decodes. Implies `BCMFW_HOSTPROTO` and `BCMFW_LCD_DMA`; sets up SDRAM at the first H.264 `OPEN`. |
| `BCMFW_SDRAM_REFRESH` | 0xD2 | Video builds: the SDRAM refresh interval in core cycles on the crystal (7.8 µs at 27 MHz); on PLL A it is `BCMFW_SDRAM_REFRESH_FAST`, Apple's 0x493 (7.8 µs at 150 MHz) |
| `BCMFW_VIDEO_PLL` | = `BCMFW_VIDEO` | Video builds (dev9): `OPEN` with `HP_OPEN_FAST_CLOCK` runs the session at 150 MHz (`clock.c`); `CLOSE`, `LCD_SLEEP` and a lost session come back to 27 MHz. 0: the flag is ignored (`pipeline.md`, "The 150 MHz clock") |
| `BCMFW_CLOCK_CMD` | 0 | 1 (stage 4c's image, dev9): `BCM_CMD(0x41)` switches the clock as `0x168` asks, then re-applies the bus timing (Apple's 150 MHz words on the PLL, ours on the crystal); `LCD_SLEEP` comes back to 27 MHz |
| `BCMFW_VIZ` | 0 | 1 (with `BCMFW_HOSTPROTO` and `BCMFW_JOBS`, no video): the visualizer image (#126; job contract: proposed, sim only). The display-only image's layout and jobs with `SPECTRUM` and `BARS` (`jobs/jviz.c`, `video/viz_vpu.c`, the vector FFT `video/viz_kernels.S` from `gen_viz.py`) in place of `DECODE`. Its start-up check sets `J_VPU_FFT` (`BCMFW_VIZ_FFT_FAIL=1`, tests only, makes it fail). `docs/research/bcm2722/visualizer.md` |
| `BCMFW_JOBS_OVL` | `0x13B000` in the display-only image, else 0 | The overlay region (#126; job contract: proposed, sim only): `J_OP_OVERLAY` and its stub (`video/jobs_bcm.c`, 288 B), the region published in `J_OVL`. `build.py --overlay` (or `build_overlay()`) also links the visualizer's overlay (`video/viz_ovl.c`, `OVL_SOURCES`) at the region's base against the image's map: `viz-ovl.bin` (the PP's upload), `.map`, `.lst`. 0 builds the display-only image without the stub. `docs/research/bcm2722/visualizer.md` §8 |
| `BCMFW_PROBE` | 0 | 1: the device-session probe. It reports ready, the counter, where the core started (`0x1C4`) and probe v2's on-chip checks (`probe.S`): nine required instruction forms Apple's code never exercises in the self-test (result at `0x1C8`, expected `0x1FF` at `0x1CC`, progress at `0x1C0`), and four informational checks of `(--rs)` loads and the exception model, with every check's outcome in a results block (`0x1DC`; `bringup.md`, "Probe v2"). It answers every command without panel or GPIO-output traffic. Implies `BCMFW_RESET_BRANCH`. |

## Files

- `config.h`: layout, mailbox, status values and switches. Shared with the `.S` files; float and double are poisoned.
- `hw.h`: the registers we use.
- `crt0.S`: vectors, `_start`, the vector stubs and exception handler (records, resume, legacy stop) and the idle tick.
- `push.S`: the pixel loop (11 instructions per 4 pixels).
- `hostproto.c`, `hostproto.h`: the video host protocol skeleton. The header holds only constants, so the PP-side driver can share it.
- `dma.c`, `dma.h`: the DMA frame push (`BCMFW_LCD_DMA`): registers, the control block, the bounded wait.
- `main.c`: the command loop.
- `lcd.c`: panel detection, power-on and power-off, windows, pixel output.
- `panels.c`: the four panels' sequences, in our own record format.
- `timer.c`: delays.
- `clock.c`, `clock.h`: the core clock (dev9): `clock_fast()` (Apple's PLL switch, from the 4b bench) and `clock_slow()` (ours, the reverse) for `BCMFW_CLOCK` builds (4b bench, video, the stage 4c image).
- `lib/string.c`: `memcpy` and `memset`.
- `probe.S`: the probe's self-check, 13 checks (`BCMFW_PROBE` builds only).
- `build.py`: the build.
- `mkdevtest.py`: builds the probe, hello, hello_rb, hello_dma, hello_dma_rb, bench, bench_pll, hello_dma_clk, drift (stage 5b's mode B), hello_dma_idle (stage 5i) and hello_dma_sby (stage 5s: the idle image plus panel standby) images and writes `firmware/target/arm/ipod/video/bcm-devtest-images.c` for the device test (`--check` verifies the committed file).
- `tests/hello_test.py`: the bench.
- `tests/hostproto_test.py`: the video protocol bench, with a model of the PP-side driver.
- `tests/dma_test.py`: the DMA build's bench (it reuses hello_test's host and panel models, plus `vc2mmio.DmaEngine`).
- `tests/devtest_test.py`: the device test's ARM-side stage logic against vc2sim (`docs/research/bcm2722/device-session-1.md`).
- `bench/`: the timing bench (`BCMFW_BENCH` builds only). `bench.c` runs it and fills the results block (layout in `bench.h`); `bench_scalar.S` holds the scalar checks; `gen_bench.py` writes `bench_gen.S` (the timing kernels and vector checks) and `bench_tab.c` (their descriptors, inputs and expected outputs), both committed (`--check` verifies them); the vector assembler front end is the decoder's `h264/vpu_asm.py`.
- `video/`: the video build (`BCMFW_VIDEO` only). `video.c`: sessions, the packet queue, SDRAM set-up, presenting pictures; `video.h`: the memory map; `disp.c`/`disp.h`: tiling for the display kernels; `gen_disp.py` writes `disp_kernels.S` (committed, `--check`); `dispref.py` is the exact reference for the conversion and the scaler.
- `h264/`: our H.264 decoder (`docs/research/bcm2722/decoder.md`).
- `tests/disp_test.py`: the display kernels in vc2sim against `dispref.py`.
- `tests/video_test.py`: video end to end: a model of the ProkBox driver feeds the corpus over the protocol, every panel frame is compared with ffmpeg's decode through `dispref.py`, and the profile is per stage.
- `tests/vclk_test.py`: the level bands (`clock.c` `clock_band`, `HP_OP_VCLK`) with the counter following the core's clock: Apple's PLL rule and levels, the level at least Apple's for the clock after every write, the refresh, the speed check and the fallbacks (a slow core, a stopped counter), OPEN and CLOSE, the busy meter, two mutants.
- `tests/bench_test.py`: the timing bench against vc2sim: checks, counts, the timer model, silence on the panel and GPIO, and the PLL build's write sequence against `vmcs.bin`'s.
