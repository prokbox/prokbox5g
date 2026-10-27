# BCM2722 bring-up image ("hello BCM"): decisions and status

Issue #60, milestone 3, which the owner approved on 2026-10-10. Our own VideoCore II image, `bcmfw/`, answers Rockbox's LCD contract. **It has been proven in the simulator only.** No device time has been used yet. The source, the host contract and the switches are in `bcmfw/README.md`. The hardware facts it relies on are in `hardware.md`.

## Result (simulator)

| | |
|---|---|
| Image | 4,020 bytes loaded, plus 20 bytes of `.bss`. That's 0.31% of the 1.25 MB of SRAM. Vectors and mailbox take 512 bytes, code 1,808 and tables 1,700. (4,004 before the pre-decrement fix: `cmp #imm5` became the 32-bit `cmp`.) |
| Bench | `bcmfw/tests/hello_test.py`: **28 rows, 643 cases, 0 failures**, in about 26 s |
| Full `LCD_UPDATE` (panel on) | **211,291 instructions**. That's 2.75 per pixel for 76,800 pixel writes plus 17 window writes. |
| `LCD_UPDATERECT` 100×50 | 14,821 instructions |
| First update (power-on plus frame), host kick to done | panel 0: 136 ms; panels 1 and 3: 268 ms; panel 2: 451 ms. These times are almost entirely the panels' own required delays. |
| Power sequences | Match Apple's handler event for event for all six panel variants, on and off. Every delay is at least as long as Apple's. |
| Instruction kinds | The image executes 70 kinds. 68 of them are executed by Apple's code in the self-test. The other 2 are `bhi` at 16 and 32 bits: both encodings occur in Apple's static code (36 and 75 sites), and condition `hi` runs in the self-test as `addcmpb hi`. Statically, every instruction of every image has a kind that occurs in Apple's code or is device-verified (`forms_test.py`, below). |

**Real timing is unknown until a device run.** The instruction counts are exact for our code. Time is not:
- The simulator counts 100 instructions per µs.
- The core clock at a host start is unknown, because we don't touch the PLL.
- Whether writes to the LCD data register stall the core is also unknown.

On the host side, panel 2's power-on uses 451 ms of lcd-video.c's 500 ms `BCM_LCDINIT_TIMEOUT`. If the first frame takes longer than about 49 ms, the host re-kicks. That's harmless here: we finish, then run one more frame.

## Decisions

1. **Scope.** First light needs only the LCD path. It sets no clocks: the panel bus timing words were computed for 150 MHz, and a slower clock only lengthens the bus cycles. It also uses no SDRAM (the frame is in internal SRAM), no DMA and no cache: the host flushes the cache and leaves it off, so its writes are seen directly. This follows `hardware.md`, "What our bring-up image must do".
2. **The entry point is a single constant.** `BCMFW_ENTRY = 0x200` lives in `bcmfw/config.h`. `build.py` reads it and passes it to `vc2cc --entry`. The layout puts `.vectors` at 0, pads to the entry, and puts `.text` from there, checking that `_start` comes first. `BCMFW_RESET_BRANCH=1` also places `b _start` at 0, in case the device starts at 0. The bench tests both builds.
3. **Vectors.** All 64 words point at one fault stub. The stub writes `0xFA170000` to `0x1F8`, writes the exception frame's pc and sp to `0x1E0` and `0x1E4`, and sleeps. The host sees "not busy" and never hangs on it.
4. **Polling by default; doorbell later.** The doorbell (interrupt source 23, acknowledged by writing `0x10003800`) is only [M], and we don't know which `BCM_CONTROL` bits raise it. So the default build disables every interrupt source and busy-polls `0x1F8` in SRAM. `BCMFW_IDLE_SLEEP=1` sleeps between polls instead. It's woken by timer compare 1 every 1 ms (source 17, vector 49). The handler re-arms itself, so a wake-up can't be lost between `ei` and `sleep`. In the bench the tick was taken 186 times, and 185 of 185 completed returns came back with every register intact (the last tick was still in its handler when the run ended).
5. **Panel power-on happens on the first `LCD_UPDATE`,** not at boot. That's what the contract asks for. It means the boot reports ready at once, and the panel never shows uninitialised RAM. After `LCD_SLEEP`, the next update powers the panel on again.
6. **Panel sequences in our own format.** Each panel has three parts:
   - an *init* part, which both directions run first;
   - an *on* part;
   - an *off* part.

   Apple's handler has the same shape: off re-runs init (`hardware.md`). The prologue is code: interface on, timing words, 1 ms, GPIO 48 as output, the reset pulse, the reset wait, and six sync commands 2 ms apart. Records are 16-bit words: register/value pairs, a bare command, DCS with n parameters, a wait in ms, re-apply timing, the colour table, and the end. Panel values are written zero-extended to 16 bits. For panel 2, Apple writes some of them sign-extended (`0xFFFFE283`). The panel model compares the low 16 bits.
7. **Frames.** The window function writes the full window in Apple's order:
   - DCS panel: `0x2B`, then `0x2A`, then `0x2C`;
   - 0x2xx panels: `0x210–0x213`, then `0x200/0x201`, then `0x202`;
   - 0x4x panel: `0x45`, `0x44`, `0x46`, then `0x20/0x21`, then `0x22`.

   Pixels are then written by `push.S`: two pixels per word load, each one zero-extended, one 32-bit write per pixel, 11 instructions per 4 pixels. `BCMFW_PIXEL_PACE=N` is the fallback if CPU writes overrun the interface. Apple only ever sends pixels by DMA paced by DREQ 3, so this fallback could be needed.
8. **`LCD_UPDATERECT` is ours.** The NOR `vmcs`'s parameters are unknown and Rockbox never sends this command. We define it as: x and y at `0x1F0`, w and h at `0x1F4`, clipped, with a panel window. The register meanings for a sub-rectangle are our reading:
   - DCS: [M].
   - 0x2xx: [L]. `0x210/0x211` are the x range, `0x212/0x213` the y range, `0x200/0x201` the x/y address.
   - 0x4x: [L]. It's a portrait controller on its side: `0x45/0x44` are x start and end, `0x46` is `y1 << 8 | y0`, `0x20` is the y address and `0x21` the x address.

   `BCMFW_RECT_WINDOWED=0` falls back to sending the full frame.
9. **Re-kicks.** After a command, the image writes "done" only if `0x1F8` still holds its own busy value. If the host rewrote the command meanwhile, which lcd-video.c does after 50 ms of busy, the new command runs next. The race between that read and the write is a few instructions wide, and losing it costs one stale frame until the next update.
10. **Code generation.** bcmfw builds with `vc2cc --conservative`. GCC uses `abs` for `x != 0`, `addscale6` for `y * 320`, post-increment `ldh` and 48-bit pc-relative stores for globals. None of these is executed by Apple's code in the self-test, so each becomes a sequence of proven kinds. The cost is 76 bytes. The bench's coverage row enforces the rule when the corpus is present.
11. **Delays** run on the 1 MHz counter. If the counter is seen stopped at start, they fall back to a spin loop (`BCMFW_SPIN_PER_US`). Bit `0x200` of the info word tells the host which one is in use.

## Power: polling vs sleeping vs the doorbell

- **Busy-poll (default).** The core runs continuously while the BCM is powered, reading one SRAM word in a tight loop. Rockbox already powers the BCM down with the backlight (`lcd_sleep`, FS#9890), so this costs only while the display is on, at whatever the start-up clock is. It's the simplest path for first light, because it depends on no interrupt.
- **Idle-sleep (`BCMFW_IDLE_SLEEP=1`).** The core sleeps except for a short handler once per millisecond and the frames themselves. That adds at most 1 ms of latency per command, against lcd-video.c's 10 ms tick. It should draw much less while the display is on and idle. It depends on `sleep` waking on timer compare 1, which Apple's idle loop does with its 10 ms tick ([M]).
- **Doorbell (later).** If source 23 is confirmed on the device, the core can sleep until the host writes `BCM_CONTROL = 0x31`. That removes the 1 kHz tick: zero wake-ups between frames.
- **Not measured.** None of these has been measured. The BCM's supply current is a device question for `docs/hardware.md`'s budget table.

## ProkBox side: made behind `HAVE_BCMFW` (branch `p14-bcm-p9`, developer test builds)

Status (2026-10-10): items 1 to 4 are in, as an opt-in path (`configure --bcmfw`, which needs `--bcmfw-devtest`), off at run time until the debug menu's toggle is set and the needed stage of the device test has passed (`device-session-1.md`, "The boot path"). Differences from the proposal below:
- **Where the swap runs.** `lcd_init_device()` runs before the disk is mounted, so it can't read the toggle, the pass flag or the attempt marker. The swap runs once at the shell's start (`bcm_boot_switch()`, from `ipodui_main()`), after `main.c`'s RIGHT-held Developer escape. It is the same `bcm_powerdown()`/`bcm_init()` cycle, done by the device test's bounded code (`bcmdt_boot_swap()`).
- **Images.** The embedded images are the device test's (`bcm-devtest-images.c`: `hello`, `hello_rb`, `hello_dma`, `hello_dma_rb`), and only the one a pass flag names, with a matching size and FNV-1a.
- **Fallbacks.** `bcm_init()` checks for `0x50420001` after the bounded ready wait and reloads the NOR vmcs otherwise; `lcd_tick()` treats the fault word or one update busy for 2 s as a stop, and the next `lcd_update_rect()` power-cycles onto the NOR vmcs. `bcm_command()` is bounded to 1 s. A boot-loop marker (`bcmfw.try`) covers a hang during the swap.
- Item 5 (`LCD_UPDATERECT`) is done in dev7: with our image active, partial updates go out as `UPDATERECT` with the union of what was written (`pipeline.md`, "The everyday path", which also has the default-mode policy, strikes and the forced-Apple switch). Item 6 (info word on a debug screen) is not done; the device test's summary shows the info word.

The original proposal, for reference. These are the orchestrator's to make: lcd-video.c is upstream Rockbox, and edits there must be `#ifdef`-guarded.

1. **Image source.** Add a `HAVE_BCMFW` build flag, documented in `docs/hardware.md`. When set, `lcd_init_device` points `flash_vmcs_offset`/`flash_vmcs_length` at an embedded `bcmfw_image[]` instead of `flash_get_section('vmcs')`. The array would be about 4 KB of `.rodata`, generated from `build/bcmfw/bcmfw.bin` at build time. The NOR section stays as the fallback.
2. **Switch at boot.** The BCM is already running Apple's NOR `vmcs` when Rockbox starts (`GPO32_VAL & 0x4000`), because Apple's boot code started it. To change to our image, run one `bcm_powerdown()` and `bcm_init()` cycle in `lcd_init_device`. The display is blank for 140–450 ms, panel-dependent. The bootloader stays unchanged.
3. **Handshake check.** `bcm_init()`'s `while (bcm_read32(BCMA_COMMAND) == 0) yield();` has no timeout. Add one (HZ/2), then check for `0x50420001`. On a timeout or `0xFA170000`, log the fault words at `0x1E0–0x1E8` and re-upload the NOR `vmcs`.
4. **Timeout.** Consider `BCM_LCDINIT_TIMEOUT = HZ*3/4` under `HAVE_BCMFW` (panel 2 uses 451 ms). With our image a re-kick is harmless anyway.
5. **Optional.** Send `LCD_UPDATERECT` for partial updates, with its parameters at `0x1F0/0x1F4`. This only shortens the BCM→panel leg. The PP→BCM bus is still the bottleneck (`docs/research/lcd-fps-ipodvideo.md`), so it's low value now.
6. **Developer mode.** Show the info word, `0x1E8` (panel ID, model, counter running), in the developer debug screen.

`bcm_powerdown()`'s write to `0x10001400` and the `BCM_CMD(0xC)` are harmless with our image.

## Gate wiring (done: `scripts/bcmfw_benches.sh`, called by `scripts/validate.sh`)

Wired on `p14-bcm-p9`: the five benches below plus `devtest_test.py`, in parallel (about 55 s wall), and `vc2sim --selftest` and `vc2dis --roundtrip` when Apple's 1.3 corpus is present. Each prints a `skip` note without the toolchain or the corpus. The full gate always runs them; `--quick` only when `bcmfw/`, `tools/bcmre/` or the BCM device-test files changed. The proposal it follows:

```sh
# BCM2722 tools and our VideoCore image (each skips cleanly without vc4-elf-gcc)
python3 -I tools/bcmre/vc2cc_tests.py          || fail "vc2cc_tests"
python3 -I bcmfw/tests/hello_test.py           || fail "bcmfw bench"
python3 -I bcmfw/tests/dma_test.py             || fail "bcmfw DMA bench"
python3 -I bcmfw/tests/hostproto_test.py       || fail "bcmfw video protocol bench"
python3 -I bcmfw/tests/realframes_test.py      || fail "bcmfw real-screen bench"
python3 -I bcmfw/tests/forms_test.py           || fail "bcmfw encoding-class gate"
```

`forms_test.py` builds every image we put on the device (the three `mkdevtest.py` embeds, the DMA build, the bench and its PLL build) and checks every instruction, not only the executed ones: none in `7000`–`77ff` (pre-decrement, `isa.md`), and with the corpus, every kind in Apple's static code or on `tools/bcmre/device_verified.txt`, except the forms a check region tests (`compiler.md`, "Conservative mode"). It takes about 3 s.

The bench takes about 26 s with Apple's `vmcs.bin` present, or about 15 s without it (the reference and coverage rows are skipped). The vc2sim self-test, `--check` and `--roundtrip` need the reference corpus under `~/Git/re`, so they stay a manual check (`sim.md`).

## First device session: checklist

This needs the owner's permission and a fresh backup (CLAUDE.md rule 5; memory: no 5.5G deploy yet). It goes through Rockbox's existing `bcm_init()` path, from a test build or plugin. The NOR `vmcs` stays the fallback: a power cycle restores it.

1. [ ] **Panel ID.** With the NOR `vmcs` running, `bcm_read32(0x10002824)` bits 19 and 18 give ID = bit19 + 2·bit18. This is read-only.
2. [ ] **Model variant** for panels 1 and 3. Answer it statically in `osos` (`display_control … model=`), then set `BCMFW_PANEL_MODEL`.
3. [ ] **Entry.** Upload and start. Does `0x1F8` become `0x50420001`? If it stays 0, the reset pc isn't 0x200: build with `BCMFW_RESET_BRANCH=1`.
4. [ ] **Info word** `0x1E8`: magic `0x5042`, the expected panel ID, bit `0x200`. If that bit is 0, the counter doesn't run at start-up: time a known delay from the host and set `BCMFW_SPIN_PER_US`.
5. [ ] **First frame.** Send a test pattern: colour bars, a 1-pixel border, and text in one corner. Check:
   - orientation;
   - colour order: RGB565 as written;
   - byte order;
   - no 1-pixel shift.
6. [ ] **If the frame is torn, partial or garbled:** CPU writes are overrunning the LCD interface. Build with `BCMFW_PIXEL_PACE=1`, then increase it (DMA is the real fix, `hardware.md` "How Apple sends a frame").
7. [ ] **Timing.** Time `LCD_UPDATE` from the host (kick to done), both with the panel on and for the first update after power-on. Compare against `BCM_UPDATE_TIMEOUT` (50 ms) and `BCM_LCDINIT_TIMEOUT` (500 ms). This gives the real core speed: 211,291 instructions per frame.
8. [ ] **Sleep and wake.** `LCD_SLEEP`, `0xC`, power cut, then `lcd_awake()`. The panel should turn off cleanly with no flash, and come back with a correct frame.
9. [ ] **`LCD_UPDATERECT`** at the four edges and the corners, on the fitted panel. If the result is wrong, use `BCMFW_RECT_WINDOWED=0` and note the panel.
10. [ ] **Fault path.** No `0xFA170000` at any point. If one appears, read the pc and sp at `0x1E0/0x1E4` and look them up in `build/bcmfw/bcmfw.map`. A probe build also leaves record U (vector, sr, r0-r5) in its results block (probe v2 below).
11. [ ] **Idle-sleep build.** Does `sleep` wake on compare 1 (vector 49)? Updates keep working, and the BCM current drops, if it can be measured.
12. [ ] **Encodings run only by our image,** checked by the frame working at all: `bhi` at both lengths (used in the pixel loop and in rect clipping). Both occur in Apple's code. The 16-bit `cmp rd, #imm5` this item used to list doesn't exist: session 1 showed its encoding is `ld/st (--rs)` (`isa.md`), and images now use the 32-bit `cmp`.
13. [ ] **Upper bits.** Apple writes sign-extended values for panel 2, which suggests the upper 16 bits of `0x1000417C` writes are ignored ([M]). We don't depend on this, since we zero-extend, but note what you see.

## DMA frame push (`BCMFW_LCD_DMA=1`, simulator only)

Our own code (`bcmfw/dma.c`) on the path `hardware.md` describes ("How Apple sends a frame"). The default build is still the CPU push, and its image is unchanged byte for byte, until a device run confirms the DMA path.

- **Frame.** The window and RAM-write command go out by CPU as before. Then one 32-byte control block is posted on channel 1, sub-channel 0: source `0xE0000`, destination the LCD FIFO `0x1000400C`, 38,400 units of 4 bytes, 32-byte source bursts, destination paced by DREQ 3. The interface is then armed for a 76,800-transfer run (`+0x00 = 0x331`, `+0x04`, `+0x08 = 0x1F`, `+0x00 = 0x329`).
- **Rectangles** use one 2-D block: rows of w/2 units, with the rest of each 640-byte row skipped (skip = pitch − width, which is how Apple's 2-D setup computes the stride word). Full-width rectangles are 1-D with bursts. The engine moves 4-byte units, so a rectangle with an odd x or width goes by CPU. Rockbox's host always rounds x and width to even, so for lcd-video.c every update is DMA.
- **Wait.** The core polls the channel's completion bit every 64 µs. The wait is counted in polls, so it also ends if the 1 MHz counter doesn't run. After 100 ms it stops the channel, puts the interface back in command mode, re-sends the window and the pixels by CPU, and leaves DMA off until the next upload (info bit `0x1000`). The host never sees anything but a slower frame.
- **SDRAM:** none. The control block is in the image's `.bss` (32-byte aligned), and the engine reads only the framebuffer. The bench checks every page it reads.

**Bench** (`bcmfw/tests/dma_test.py`, with `vc2mmio.DmaEngine` and the LCD FIFO model): **19 rows, 529 cases, 0 failures**, about 16 s. It covers full frames for all six panel variants, with power-on still identical to Apple's event for event; the host's rectangle path; 2-D `UPDATERECT`s at the edges, including odd ones by CPU; a dead request line, which exercises the timeout and the CPU re-send; sleep and wake; back-to-back updates with a re-kick; and coverage. The DMA image executes 64 instruction kinds, all exercised by the self-test apart from the same three accepted forms.

| Per update, panel on | CPU push | DMA: core work | DMA: total, polling at the modelled bus rate |
|---|---|---|---|
| Full frame | 211,291 instructions | **240** | 7,024 (7.7 ms at 10 Mpixel/s) |
| `UPDATERECT` 100×50 | 14,821 | **276** | 660 (0.5 ms) |

The image grows by 500 bytes, to 4,520 (plus 56 of `.bss`). The bus rate is a guess: the model moves 5 words (10 pixels) per µs. The total depends on it, the core work does not. On the device, the polling can become `sleep` until the channel interrupt (source 1), once that is confirmed. Then the core does about 240 instructions per frame instead of 211,291.

### Device checklist: DMA build

Run after items 1 to 7 above pass with the CPU build. The device test's stage 3 (`device-session-1.md`) covers items 1 to 6: its log line per push gives the completion and timeout counts, the push time on the BCM's own counter, and channel 1's config, status and sub-channel words (mailbox `0x1A4`–`0x1BC`, DMA builds only), and photos 4 to 6 show items 2 to 4 and the CPU fallback.

1. [ ] **Completion.** One `LCD_UPDATE` with `BCMFW_LCD_DMA=1`. Info bit `0x1000` must stay clear. If it's set, the wait timed out. Read DMA channel 1 at `0x10000880` (config) and `+0x04` (status), and the sub-channel word at `+0x38`, from the host. Suspects, in order: the status bit's meaning [M], sub-channel 0 [L], the channel config value [L], the run arming [L].
2. [ ] **Pixel order in a FIFO word.** The model sends the low halfword first [M]. If neighbouring pixel pairs are swapped, swap the halves.
3. [ ] **Length unit.** The model counts units of the smaller width (4 bytes) [M]. If only an eighth of the frame arrives, or the transfer runs eight times too long, the unit is the source width (32 bytes).
4. [ ] **2-D skips.** `UPDATERECT` 100×50 at (10, 20), and at the edges. A sheared rectangle means the stride word is a pitch, not a skip.
5. [ ] **Back to command mode.** After a DMA frame, `LCD_CTRL = 1` must leave the interface taking window commands: a second frame, then an `UPDATERECT`, must both be right.
6. [ ] **Frame time** from the host (kick to done) with DMA against the CPU build. This gives the real bus rate the model guesses.
7. [ ] **Tearing.** The host may write the next frame while a transfer runs only if it ignores the busy status. lcd-video.c doesn't. Check that a fast animation shows no torn frames.

## Probe v2 (`BCMFW_PROBE=1`; simulator only so far)

Session 1's probe faulted inside its own check 7: the 16-bit `cmp r0, #5` it tested was `0x7050` = `ld r0, (--r5)`, a misaligned load with r5 = `0x7F` (`isa.md`). v2 keeps the old mailbox contract and adds an exception handler and a results block, so a fault is named, recorded and survived.

**Handler (every build, `crt0.S`).** Vector k points at its own 8-byte stub (`bcmfw_vstubs + 8k`: `push r0-r5, lr`, `bl bcmfw_exc_common`, `nop`). The common handler gets the vector from lr. Vector 49 stays the idle tick. In reset-branch builds word 0 is code; in the probe, `probe_from0` treats any entry after the first (step word already non-zero) as vector 0. Then:
- a fault inside a self-check check fills that check's record, sets its fault bit (block word 5 and `0x1D8`) and resumes at the next check through a crafted `rti` frame (the frame's sr, the resume pc; sp back to the self-check's). The saved pc is never used, so any pc offset works;
- a fault outside any check fills record U, then the legacy stop: `0x1E4` = frame address, `0x1E0` = frame pc, then `0x1F8 = 0xFA170000`, then `sleep`. Non-probe builds only do the legacy stop;
- a fault while the handler runs (its busy flag set) goes straight to the legacy stop.

The handler uses only kinds Apple's code has and the self-test runs. `forms_test` doesn't exempt it.

**Mailbox.** `0x1C0` step (`0x50520002 + n << 8` before check n), `0x1C4` from-0, `0x1C8` PASS (bit n = *required* check n passed), `0x1CC` ALL = `0x1FF`, `0x1D0` ticks, `0x1D8` FAULTED (bit n = check n faulted), `0x1DC` = `0xCB000` once the block header is valid, else 0. Informational checks never touch PASS/ALL, so the session-1 ARM code (`pass == all`) works unchanged.

**Results block at `0xCB000`** (704 bytes, 32-bit words; `build.py` fails if it overlaps the image, stack, bench block, host-protocol rings or framebuffer):

| Word | Meaning |
|---|---|
| 0 | `0x50524232` ('PRB2') |
| 1 | nchecks (13) \| REC_WORDS (12) << 8 \| version (2) << 16 |
| 2, 3 | required mask `0x1FF`, informational mask `0x1E00` |
| 4, 5 | pass bits (all checks), fault bits |
| 6, 7 | `TIMER_CLO` before and after the 1000-iteration spin (7 − 6 = ticks) |
| 8–19 | record U: a fault outside any check |
| 20 + 12i | record of check i |

Record: [0] status = state (bits 7:0: 0 not run, 1 pass, 2 fail, 3 fault) \| vector << 8 (fault only) \| index << 16 (`0xFF` for U) \| `0xC5` << 24; [1] frame sr; [2] frame pc; [3] frame address (sp at handler entry); [4–9] r0–r5 at the exception as the stub saved them (pass/fail: at the end of the check); [10] detail; [11] detail2. Frame words are 0 for pass/fail. If the chip's `push` order were the reverse of vc2sim's, r0–r5 would show up reversed.

**Checks.**

| # | Kind | Detail, detail2 | What it tells us |
|---|---|---|---|
| 0–6 | abs, not, mulhd.ss, 3-op eor, addscale6, `ldh (rs)++`, 48-bit pc-relative ld/st | 0 | required, as in v1 |
| 7, 8 | `bhi` 16-bit, then 32-bit: not taken, then taken | 0 | required |
| 9 | session-1 replay: r0 = 5, r5 = `0x7F`, raw `0x7050`, nops around it | address of the `0x7050`; r5 after (pass only; on a fault r5 is word 9) | expected to fault: the vector number for a misaligned access, the pc offset (frame pc − detail; session 1 suggests 6), and whether r5 was decremented before the fault |
| 10 | raw `0x7050` aligned, r5 = &buf + 8 | r0, r5 | pass = pre-decrement word load works as `isa.md` says, so `ldst_predec ld` can go on `device_verified.txt` |
| 11 | `ldb`/`stb (--rs)` (Apple's memmove), two bytes backwards | copied halfword; (src ptr − src) \| (dst ptr − dst) << 16 | pass = Apple's pre-decrement byte forms behave as modelled |
| 12 | `ld r0, (r1)` with r1 = &buf + 1, then four 32-bit `add16` with distinct results in r2–r5 | address of the load; r0 (pass only) | fault: frame pc − detail says whether the offset counts bytes (2 + 4k) or instructions; r2–r5 show whether any later instruction ran first; the status gives the misaligned vector. No fault: the chip allows unaligned loads, and r0 shows how |

**Simulator result** (default HYP: misaligned vector 1, pc offset 0): PASS = ALL = `0x1FF`, FAULTED = `0x1200`. Checks 0–8, 10, 11 pass; 9 and 12 fault with vector 1, frame pc = the instruction, r5 = `0x7F` in 9. The same with pc offsets 0/2/4/6 and vectors 1, 2, 5, 17, 63 (`hello_test`, "probe v2" rows), and for a fault outside a check (record U, legacy words), through vector 0 after the start, and inside the handler.

## Next

- ~~DMA pixel path, then 2-D rect DMA~~: done in the simulator, above. Next: sleep on the DMA interrupt instead of polling.
- The doorbell interrupt, once item 11 has passed.
- Clocks at 150 MHz, if frame time needs it.
- A device-test image that runs `vc2cc_tests`' functions and reports through the mailbox. That settles the remaining VC IV-derived semantics (`compiler.md`).

## Device session 2: the timing bench (`BCMFW_BENCH=1`, simulator only so far)

Device session 1 tells us whether our image runs. Session 2 tells us **how fast the chip really is**. Every speed figure so far is an instruction count from `vc2sim`, which charges one microsecond per 100 instructions and nothing extra for a vector REP row. The decoder's budget (`decoder.md`, "Against the budget") assumes one cycle per scalar instruction and one per vector row, at about 150 MHz. None of that has been measured. The bench image measures it on silicon, and in the same run checks the instruction semantics that the decoder relies on and that Apple's code never exercises.

Source: `bcmfw/bench/` (`README.md`, "Files"). Switches: `BCMFW_BENCH`, `BCMFW_BENCH_PLL`, `BCMFW_BENCH_ITERS`.

### What the image does

The bench is a probe build: no panel, no GPIO output, no DMA, no SDRAM, no cache, and in the default build no clock or regulator access. It starts as the probe does. It reports ready (`0x1F8 = 0x50420001`), and the probe's self-checks run first (`0x1FF` required; probe v2 above). Then it runs, in order:

1. **Vector checks (27).** One per vector form in the decoder's and the display path's kernels that no self-test row executes: the semantic forms `bcmfw/h264/test/vpu_forms.py` lists (it now scans `bcmfw/video/disp_kernels.S` too):
   - `vmin`/`vmax` 16-bit;
   - REP on `vmul` by a register and on `vadd #imm`;
   - `vadds` D8 A8 B16 under `ifn`;
   - `vmov` from a register under `ifn`;
   - `vsub … setf ifnz`;
   - `vmul` into the discard destination with `acc`;
   - `vmulhr`, `vrsub`, `vadd #imm` without REP;
   - `vmul` D16 A8 by a register;
   - `vld8` with REP;
   - the intra-prediction kernels' four: `vadd` D16 A16 by a register (`ADRG`), `vmov` D8 from a register under `ifnz` (`MVNZ`) and with REP (`MVRP`), `vmulhr` into 8 bits (`MH8I`);
   - the display path's three (`pipeline.md`): `vmulm` D16 A8 by a signed immediate with REP (`MLMI`), `vlsr` D8 (`LSR8`) and `vadd` D16 A16 + B8 with REP (`AD68`).

   Plus the data ranges in decoder.md's list (`vmulhrs #2048` on negative sums, `vmul` by negative registers in accumulator chains) and the display path's two (`vmul` products past 16 bits keep their low 16 bits, `WRAP`; `vmulhrs #0x1000` over the colour conversion's range, `CLRQ`), and one harness check (`vld16`/`vst16` of 32 aligned bytes). Each check loads its rows with `vld16` and stores them with `vst16`. Flags and the accumulator are set and read back only with forms the self-test runs: `vbitplanes setf`, `vsub … setf`, `vmov … clra acc`, `vadd … acc`, `vmov … ifnz` and `vmulhru … ifn`. So a failed bit points at the form under test. Expected outputs come from `gen_bench.py`'s own model of the semantics in `isa.md`, not from vc2sim. The bench checks that vc2sim agrees; the device checks that silicon does.
2. **Scalar checks (9),** for the scalar kinds the decoder's C executes beyond the probe's ten (decoder.md, item 4):
   - `max` with an immediate (the 32-bit `max rd, rs, #u5`; there is no 16-bit `max #imm5`);
   - `asr rd, rs`;
   - the 32-bit `bgt`, `blt`, `ble` and `bls`;
   - `ldsb` at `(rs)`, `(rs+ri)` and `(rs+off)`.
3. **Timing (69 kernels).** Each kernel is a loop of a known instruction mix, unrolled 32 (scalar) or 8 (vector) times, with one `addcmpbne` per iteration. The kernel reads the 1 MHz counter around its own loop. Each kernel runs 3 times with 4,096 iterations, and the fastest and slowest runs are kept. The counts are in the table, and the simulator bench checks them instruction for instruction.

| Tags | What |
|---|---|
| `ALU1` | dependent 16-bit `add` chain: **the clock reference** |
| `ALU2`, `ALU4`, `MIX2` | 2 and 4 independent `add` chains; independent `add`/`ld` pairs (dual issue) |
| `A32D`, `MV48` | dependent 32-bit three-operand `add`; 48-bit `mov #imm32` (fetch width) |
| `MULD`, `MULI` | dependent `mul`; 2 independent `mul` chains |
| `LDTP`, `LDLT`, `LDUS`, `STTP`, `STLD`, `LDBX` | SRAM load throughput, load latency (pointer chase), load-use, stores, store-then-load, indexed `ldb` |
| `BRTK`, `BRCT`, `BRNT`, `CALL` | taken `b`, taken and not-taken `beq`/`bne`, `bl` + `rts` |
| `PERI` | reads of a peripheral register (GPIO level) |
| `CAVL` | a CAVLC-style bit reader: 16 instructions per symbol (show 8 bits across a word boundary, table length, advance), 4 symbols per iteration, no data-dependent branch |
| `ADD*`, `AD8*`, `ADS*`, `MUL*`, `MHS*`, `MIN*`, `MAX*` | `vadd` D16, `vadd` D8, `vadds` D8 A8 B16, `vmul` D16 by a register, `vmulhrs` D8 `#2048`, `vmin`, `vmax`; each at REP 1, 2, 4 and 16 (suffix `1 2 4 X`) |
| `LD8*`, `L16*`, `LPQ*`, `ST8*`, `S16*`, `STR*` | `vld8`, `vld16`, a `vldp8`+`vldq8` pair misaligned by 5, `vst8`, `vst16`, and `vstr` (the decoder's masked store, each after a scalar `mov`); REP 1, 4 and 16, 64-byte stride |
| `VDEP`, `VACC`, `VSMX` | dependent `vadd` chain; `vmul … acc` chain; `vadd` REP 4 alternating with a scalar `add` |

4. **Optional, `BCMFW_BENCH_PLL=1`:** switch to 150 MHz (below), then time all 69 kernels again into a second table.

The bench takes about 38 M cycles if every scalar instruction and every vector row costs one cycle. That's 0.25 s at 150 MHz, or 1.4 s if the chip starts on the 27 MHz crystal. Afterwards, the image answers commands exactly as the probe does.

**Image:** 22,576 bytes loaded (code 13,740, tables and check data 7,884, data 440), plus 9,520 bytes of `.bss` (a 9 KB vector buffer). That's 2.5% of SRAM. The PLL build is 23,216 bytes.

### The results block (`0xC0000`, words; layout in `bench/bench.h`)

| Word | Meaning |
|---|---|
| 0 | magic `0x50424E31` ("PBN1") |
| 1 | state: 1 running, 2 done |
| 2, 3, 4 | kernels (69), iterations per run (4096), runs (3) |
| 5 | flags: `1` counter runs, `2` PLL build, `4` switch ran to its end, `8` a wait in the switch ended on its spin limit (the counter stopped) |
| 6, 7 | vector checks passed and expected (bit k = check k, in `bench_tab.c` order) |
| 8, 9 | scalar checks passed and expected (`0x1FF`) |
| 10, 11 | the probe's required checks again (`0x1FF`) |
| 12, 13, 14 | the first mismatching vector word: `check << 16 \| word` (or `0xFFFFFFFF`), what was read, what was expected |
| 15–19 | PLL build: `0x10001400` before and after, then PLL A's divider, PLL A's control and the core clock select, read back |
| 20, 21, 22 | table 1's word offset (32), table 2's (PLL build, else 0), words per entry (8) |
| 23 | the vector check count |
| 24, 25, 26 | the vector check records' word offset (`0x800`; 0 in images before them), words per record (12), and the late-pass bits |
| 32… | table 1, then table 2: per kernel `{tag, instructions, measured ops, vector rows, fastest ticks, slowest ticks, instructions per iteration, 0}` |

The mailbox holds the block's address at `0x1D4`. The probe's step word `0x1C0` reads `0x50521000 + k` during vector check k, `0x50522000 + n` during scalar check n, `0x50523000 + k` while timing kernel k, `0x50524000 + n` at step n of the clock switch, `0x50525000 + k` while timing at 150 MHz, and `0x50520004` when done. A hang or a fault names its place.

### Vector diagnostics (after device session 3)

Session 3 passed only COPY: VMAX's output still held the `0xA5A5A5A5` fill, so its vector store never landed, while the vector kernels' wall time (about one cycle per instruction and row, near 27 MHz) shows the VPU does execute. Six checks now run before the others, each with forms Apple's code runs (MXSU's `vmax` aside), and each separating one cause:

| Check | What it does | If only it fails |
|---|---|---|
| COP8 | `vld16`/`vst16` through row 8, no ALU op | row 8 addressing differs from the model |
| SUMR | `vld16` row 0, then `vmov … sum r5` and a scalar `st`: the VRF read back through the scalar-result unit, no vector store | the read-back path itself |
| CNST | 48-bit `vmov d08, #52`, no `vld`, then `vst16` | ALU result or its store |
| CSUM | CNST, plus row 8 summed into r5 (word 0) | r5 right but the row wrong: the store after an ALU op is lost; r5 wrong: the ALU result never reaches the VRF |
| ALUX | `vld16` row 0, `vadd` on row 16, `vst16` row 0 | any ALU op stops the next store |
| MXSU | VMAX, plus its row summed into r5 | as CSUM, for `vmax` |

Every check (old and new) leaves a 12-word record at word `0x800` of the results block: tag; flags (bit 0 pass, bit 1 pass on a re-read after `spin(20000)`, about 2 ms at 27 MHz; words checked in bits 15:8; the first bad word in 31:16); `out[0..3]` read at once and again after the delay; the first bad word and its expected value. A check that fails at once and passes late means the vector store is posted and the scalar side read too early. A check with a scalar read-back stores it at `out[0]` (the rest of that 32-byte slot keeps the fill) and its rows after it. The step word reads `0x50521000 + k` during check k, as before.

The device test logs two lines per check, `B4V k TAG flags got0..3 bad want` and `B4VL k late0..3`, then `B4V late <bits>`. `readlog.py` prints them as a table and one verdict line: "VPU dead", "VPU memory path dead", "row 8 addressing", "posted stores (late pass)", "ALU results lost", "the vst after an ALU op is lost", or "encodings wrong for: …" when every diagnostic passes and named checks don't. Logs from images without records (session 3) are read with that image's 18-check order.

### How to read the numbers

Ticks are microseconds. With `I` instructions, `ops` measured instructions, `rows` vector rows and `t` the fastest run:

- **Clock.** f ≈ `I / t` of `ALU1`, in MHz, if a dependent 16-bit `add` issues once per cycle. That is the usual case for an in-order core with forwarding, but it is an assumption. The PLL build removes it: after the switch the clock is 150 MHz by construction, so the chain's real CPI is 150 · t₂ / I. The start-up clock is then 150 · t₂ / t₁ (from `ALU1` in both tables), with no assumption. All of this assumes the 1 MHz counter runs from the crystal and not from the core clock. If the core's apparent speed doesn't change across the switch while the readbacks show the switch happened, the counter is the suspect.
- **Cycles per instruction:** `t · f / ops`. Scalar kernels include their loop branch (1 in 33). `ALU2`/`ALU4`/`MIX2` near 0.5 means two independent instructions issue per cycle (dual issue, reported for later VideoCores and never measured on this one). Near 1 means single issue.
- **Vector cost:** fit `t · f / (ops) = a + b · REP` over REP 1, 2, 4 and 16: `a` is the issue cost of an instruction, `b` the cost per row. The decoder's estimate is a = 0, b = 1. Memory kernels give the same fit for `vld`/`vst`. `LPQ` gives it for a misaligned pair, and `STR` for the masked store (minus one scalar `mov` per store, from `ALU1`).
- **Noise:** slowest/fastest above 1.02 means something interfered. Polling from the host port is the prime suspect, so the ARM side polls only every 10 ms.
- **Counter stopped** (flag 1 clear, ticks 0): nothing is timed. The checks are still valid.

The numbers describe this configuration: code and data in internal SRAM, cache off (the host leaves `0x10000C00 = 0`), no SDRAM, no DMA running. Cache-on, SDRAM latency and DMA overlap need their own kernels, once SDRAM is set up (`hardware.md`, "SDRAM controller").

### How the results replace the decoder's estimates

`decoder.md` gives per-stage instruction counts from vc2sim, with vector instructions and REP rows counted separately (`sim_test.py`). Its cycle estimate (2.34 M per iPod-clip frame) is instructions + rows. With the bench:

1. **Scalar stages.** For parse (CAVLC, 52% of a P frame), cycles = instructions × CPI(`CAVL`). For the other C stages (macroblock control, MV prediction, intra prediction, the deblocking and MC glue), use a mix: CPI = the average of `ALU1`/`ALU2` for ALU work, weighted with `LDUS`, `STTP` and `BRCT` by the stage's share of loads, stores and branches. vc2sim's `--cover` counts give those shares per stage.
2. **Vector stages.** cycles = Σ over each kernel's instructions of (a_op + b_op · REP). Take a and b per class (`ADD`/`MUL`/`MHS`/`MIN` for ALU ops, `LD8`/`LPQ`/`ST8`/`STR` for memory). The decoder's kernels are straight-line and fully REP-annotated, so the per-kernel totals are static.
3. **Budget.** The budget is f / 30 cycles per frame at 30 fps: 5.0 M only if the clock is 150 MHz. If the PLL build shows a slower start-up clock, either the decoder build switches clocks (the PLL sequence, once it's proven safe) or the budget shrinks to that clock.
4. Then decoder.md's "Against the budget" gets a measured column, and the next optimisation target is chosen on cycles instead of instructions.

### Simulator result

`bcmfw/tests/bench_test.py`: **13 rows, 433 cases, 0 failures, about 24 s** (built with 256 iterations; `--device` also runs the 4,096-iteration build, which takes minutes in vc2sim). It checks:

- the image reports ready before it benches;
- it reaches done and then answers `LCD_UPDATE` with no panel traffic;
- all 27 vector and 9 scalar checks pass, and so do the probe's nine required ones;
- corrupting one expected word fails exactly that check and reports the word, and a changed scalar input fails exactly that check;
- every entry has its tag and counts, and each kernel's declared instructions per iteration equal what the simulator executes;
- the default build writes only the interrupt controller and reads only the counter and the GPIO level;
- the PLL build's clock writes equal `vmcs.bin` 1.3's start-up sequence, write for write. The reference is extracted from the simulator log at run time; without the corpus, the list in `hardware.md` is used;
- with the corpus: each vector check's instruction has the form it names and its harness uses only forms the self-test runs. The image executes no scalar kind outside the self-test except the ones its checks test (and hello_test's three accepted encodings).

**Times under the simulator's model:** every kernel shows 1.00 instruction per 10 ns (±2 ticks: time moves every 32 instructions), and the clock estimate is 99.4 MHz against the model's 100. Vector REP 1 and REP 16 cost the same, because vc2sim doesn't charge rows. So the simulator proves the plumbing, the counts and the checks. **Only the device gives the speed.**

### The PLL variant: risks (off by default)

`BCMFW_BENCH_PLL=1` repeats Apple's start-up write for write: run control `0x10000400 = 0xA5A50020` twice, `+0x4C`/`+0x54`, ten block clocks off and `+0x28 = 1`, PLLs B, C, D and A off, the regulator to level 15, 10 ms, then PLL A at M = 108, N = 600, 1 ms, enable, and `+0x28 = 0x101`. The waits are bounded: if the counter stops, a spin limit ends them and flag 8 says so. The risks:

- **Run control** (`0x10000400` bit 1 cleared, bit 5 set) has meanings we rate [L]. Apple's image writes this while it runs, but if it stops a host-started core, the step word stays at `0x50524001` and the ARM's timeout cuts the power.
- **Block clocks off.** We don't know what the ten consumers are. If one of them clocks the host port, the ARM's reads fail: the port-error path, with bounded waits, cuts the power. If one clocks the timer, flag 8 is set and the second table reads near 0.
- **Regulator level 15** is the highest core voltage in Apple's table (for 160.9 MHz). It is held for under a second, until the power cut, and it's the level Apple's own start-up sets every boot.
- **PLL fields** are [M]. If M and N don't mean what we think, the core could run far from 150 MHz. Too fast: a hang or a fault, again ended by the power cut.
- **Persistence:** none. All of it is volatile BCM state, reset by the power cut that ends every stage. Nothing touches the NOR flash, the disk or the panel. The SDRAM controller isn't set up, so its refresh, which depends on the clock, doesn't matter.

Run it only after the default bench has passed, as its own step, with the owner's go.

### ARM side: proposal (not implemented here)

**Implemented on `p14-bcm-p9b`** (runbook: `device-session-1.md`, "Stages 0-4 (timing)" and "Stage 4b"), with these differences from the proposal below: stage 4 needs only stage 1 in the same run, so it still runs when stage 2 or 3 fails; the overall limit is 20 s (25 s for 4b), sized from the bench's 38 M-cycle count at 27 MHz; stage 4 writes a pass flag (`bcmpass4.txt`), and stage 4b needs that flag (not a stage 4 pass in the same run), plus stage 0 and an on-device confirm; the summary screen shows only the check counts and the clock, and the derived CPI figures come from `bcmfw/bench/readlog.py` on the host instead of the ARM. The log lines are `B4H` (header words), `B4K` (one per kernel) and `B4R` (clock).

The device test's files are on this branch, but `p14-bcm-p9` has since added stage 3 (DMA) to the same files: `bcm-devtest-core.c` +345 lines, `-hal.c`, `.h`, and a regenerated `-images.c`. A stage added here would conflict with all of them. So it belongs on `p14-bcm-p9` (or after p9 is merged back), as:

- **Images.** `mkdevtest.py` adds `bench` (`BCMFW_BENCH=1`, entry at 0 or `0x200` like the probe), and `bench_pll` (`BCMFW_BENCH_PLL=1`) for a separate entry. Together they add about 41 KB to `bcm-devtest-images.c`, in the developer build only.
- **Stage 4, "BCM test: stage 4 (timing)":**
  1. After stages 0 and 1 pass, `boot(bench)`, with the same image check, ready wait (`T_READY`), fault and info checks as the probe.
  2. Poll the step word `0x1C0` every 10 ms (`h->sleep_ms(10)`), until it reads `0x50520004`. Fail on the fault word in `0x1F8`, on no change of the step word for 3 s (log the step, which names the kernel or check), or after 15 s in total.
  3. Read the header and the table(s) with `rd()`: 32 + 69 × 8 words, about 600 reads, twice that for the PLL build. Every read is bounded as today.
  4. Pass if: state 2, magic, vector pass = all, scalar `0x1FF`, probe `0x1FF`, and every entry with `0 < fastest ≤ slowest`.
  5. End as every stage does: `bcm_off(alive)` (stock order), the power cut, then Apple's vmcs restored by the stock wake path.
- **Stage 4b, "(150 MHz)":** a separate menu entry that runs only after stage 4 has passed in the same run. It boots `bench_pll` and gets the same treatment, plus a check of flags 2 and 4, flag 8 clear, and the read-back words. If the step word stops in `0x50524xxx`, the log names the switch step.
- **Log** (the same `/.rockbox/bcmtest.log`, one line per item, so a cut-off run still shows its progress):
  - the header words;
  - the check bitmaps and the first mismatch;
  - per kernel: `TAG instr ops rows min max`, plus the derived f (kHz, integer: `I·1000/t` of `ALU1`) and cycles per op ×1000 (`t·f_kHz/ops`), computed on the ARM with 64-bit integers.
- **Summary screen:**
  - "Clock: N MHz (est.)";
  - "Vector checks: x/18";
  - "Scalar checks: 1ff/1ff";
  - "ALU2 cpi", "CAVL cycles/symbol", "vadd REP16 cycles/row";
  - the PLL entry adds "Clock at start: N MHz (measured)".
- **Bench:** `devtest_test.py` gets the rows its other stages have: a pass on each panel ID, a bench that hangs mid-kernel (timeout, cut, restore), a failed check (stage fails, nothing after it), and a dead port.

### Device checklist: session 2

1. [ ] **Done.** State 2, magic, flags bit 1 set. If the step word stops, it names the kernel or check: look it up in `bench_tab.c` (kernel and check order).
2. [ ] **Checks.** Vector `0x3FFFF`, scalar `0x1FF`, probe `0x1FF`. For a failing vector check, the first-mismatch words give the check and the word. Map it to the decoder's kernels with `vpu_forms.py` (which kernels use that form) before trusting their output on the device.
3. [ ] **Clock estimate** from `ALU1`, and slowest/fastest under 1.02 for every kernel.
4. [ ] **Scalar picture:** dual issue (`ALU2`/`ALU4`/`MIX2`), load latency (`LDLT`) and load-use (`LDUS`), the cost of a taken branch (`BRTK`/`BRCT` against `BRNT`) and of a call (`CALL`), a peripheral read (`PERI`), and cycles per symbol for `CAVL`.
5. [ ] **Vector picture:** a and b per class from REP 1/2/4/16, and whether a scalar instruction issues under a REP 4 vector instruction (`VSMX` against `ADD4` + `ALU1`).
6. [ ] **Decoder budget** recomputed as above, and decoder.md updated.
7. [ ] **PLL build,** only with the owner's go, after 1 to 3 pass: flags `0x7`, regulator `0xA5A5000F`, clock select `0xA5A50101`, then the start-up clock and the true CPI of `ALU1`.
