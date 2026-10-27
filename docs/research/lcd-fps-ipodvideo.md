# iPod Video LCD update throughput: grounded estimate

Goal: predict how smooth our 190 ms slide transitions (up to 320x220 px/frame)
and list glides (320x220) will be on iPod Video 5G/5.5G (PP5022 + BCM2722
LCD bridge, 320x240 RGB565), under Rockbox.

**Headline confidence: LOW-MEDIUM.** No directly-measured `test_fps` fps
numbers for ipodvideo were recoverable from the web in this session (search
access surfaced code mirrors, not the old rockbox.org forum/wiki/FS#
threads that likely contain the real numbers — those appear to be offline
or not indexed). The quantitative table below is a first-principles model
built from reading the actual driver and from *relative* speedup numbers in
the real commit history (which is grounded, high confidence), not from a
measured absolute fps. Treat the table as an informed order-of-magnitude
estimate, not a spec.

## 1. Sources

### 1a. Repo source (read directly, high confidence)

- `firmware/target/arm/ipod/video/lcd-video.c` — `lcd_update_rect()`
  (lines 386-432), the BCM register protocol (`bcm_write_addr`,
  `bcm_write32`, lines 160-184), and the async completion state machine
  (`lcd_tick`, `lcd_unblock_and_update`, lines 195-307).
- `firmware/target/arm/ipod/video/lcd-as-video.S` — `lcd_write_data`
  (pixel push loop, lines 38-66) and `lcd_write_yuv420_lines` (mpegplayer
  blit, lines 93-303).
- `apps/plugins/test_fps.c` — `time_main_update()` (lines 169-210): full
  (`lcd_update()`) and 1/4-screen (`lcd_update_rect` on the centered half
  width x half height) tests, each run for `DURATION = 2*HZ` = 2 s, fps
  computed as `10*HZ*framecount/ticks` (tenths of fps).
- `firmware/export/config/ipodvideo.h`: `LCD_WIDTH=320`, `LCD_HEIGHT=240`,
  `LCD_DEPTH=16` (RGB565), `CPU_FREQ=11289600` (base PLL unit).
- `firmware/target/arm/pp/system-target.h`: `CPUFREQ_DEFAULT=24000000`,
  `CPUFREQ_NORMAL=30000000`, `CPUFREQ_MAX=80000000` — confirms the task's
  framing: unboosted = 30 MHz, boosted = 80 MHz.
- `firmware/kernel/include/tick.h`: `HZ=100`.

### 1b. Repo git history (local clone has full history — high confidence for *relative* deltas, no absolute fps found)

```
d6ad3fbeb5  FS#7763 by Andree Buschmann. Speed up video rendering for iPod Video.
b5f779b2ef  Parts of FS#7951. Faster YUV blit for Nano/Video (pre-asm).
3c1b9d9d70  Assembler optimised lcd_yuv_blit() for iPod Video (FS#7951, Jens Arnold).
ef8a7eaf2a  Major speedup of the iPod Video LCD driver: BCM update is now
            completely shadowed, handled by a tick task as necessary.
            Also fixes occasional UI freezes from stalled BCM updates.
b05066de12  Further optimised LCD data transfer (5..6% speedup, but the
            increase in FPS measured with test_fps is LESS) — makes use
            of the BCM not decoding low address bits, so 32-bit stmia
            bursts work. [This line is the key clue: code-side cycle count
            dropped ~5-6% but measured fps rose by less — i.e. something
            other than the host write loop (the BCM's own glass-push) was
            already co-limiting throughput.]
9daf658ef1  Reintroduce the simple blocking wait-for-completion method for
            the BOOTLOADER only (no tick/interrupts there) — slower than
            the full driver's async method, but faster than the old
            synchronous method, because it writes data first and polls after.
120fb4f319  Speed up YUV blitting on iPod Video by ~8% (FS#8075).
18b5488a4a  Minor speed up (+3%) of iPod Video screen updates. (2010)
c567fc9be2  FS#9890: optional LCD+BCM shutdown after backlight off, saves
            >30% battery — confirms BCM push is a real power/time cost,
            not free.
151748e86e  Original test_fps plugin added ("Test plugin for measuring LCD
            update speed, for developers").
cb4186c989  "Improved FPS test plugin: better precision for low frame
            rates, measure YUV blit framerate on colour targets."
```

No commit message in this history states an absolute fps number for
ipodvideo — only percentage deltas and qualitative notes. This is itself
informative: it confirms the driver's bottleneck moved over time from
"host write loop" (SVN r14882-r15341) to a mixed host+BCM-internal bound
(r15341 onward, per the explicit "FPS increase is less" note).

### 1c. Web (search tool; mixed success — see confidence notes)

- Could not find rockbox.org wiki/forum/FS# pages with literal test_fps
  numbers for ipodvideo; the search tool mostly returned a third-party git
  mirror (`git.lenord.me`) re-serving the same commit history already in
  (1b), not forum discussion.
- One real external data point found: a user report of **23.9 fps**
  mpegplayer playback at **280x210**, 1698 kbps video, on an iPod Classic
  (same PP502x/BCM2722 LCD-bridge family as iPod Video, different exact
  clock/model) — [r/IpodClassic](https://lr.in.psf.lt/r/IpodClassic/s/z2eDXEU6Hz).
  This is *decode+blit* combined, capped by the "Limit FPS" default and by
  H.264-ish decode cost, not by the LCD bus — so it's only a loose sanity
  check that ~24 fps of full-frame-ish YUV blit+push is comfortably
  achievable in practice; it does not bound pure `lcd_update()` throughput.
- No BCM2722 datasheet with bus clock/wait-state numbers was found (it's
  an unpublished/NDA Broadcom part reverse-engineered by iPodLinux; public
  docs don't give bus timing).

## 2. How `lcd_update_rect` actually moves pixels (code-grounded mechanism)

- **Bus width:** 16 bits (`BCM_DATA`/`BCM_WR_ADDR`/etc. are `volatile
  unsigned short` at base; the comment in `lcd-video.c:39-41` explains the
  BCM's 3 address bits map to PP5022 address bits 16..18 and low bits
  aren't decoded, so 32-bit `stmia` bursts to a *fixed* address work and
  land as sequential 16-bit bus cycles). **There is exactly one 16-bit bus
  transaction per pixel, minimum** — the `stmia` optimization only cuts
  ARM-side instruction/address-recompute overhead, not the number of
  underlying bus transfers.
- **Per-pixel cost:** `lcd_write_data` (lcd-as-video.S:38-66) unrolls to 4x
  `ldmia r0!,{r2-r3} / stmia lr,{r2-r3}` pairs per 16 pixels — i.e. load 2
  words (4px) from the framebuffer in SDRAM, store 2 words (4px) to the
  fixed BCM data port, repeated 4x, then a decrement/branch. No readiness
  poll inside the hot loop — only one poll before each burst-destination
  address is set (`bcm_write_addr`, lines 160-165), so the loop assumes the
  BCM's input side can absorb back-to-back 16-bit writes at whatever rate
  the PP5022's external memory controller's fixed wait-state count allows
  for that chip-select region.
- **No DMA.** The entire transfer is ARM core `ldm`/`stm` instructions;
  nothing in `lcd-video.c`/`lcd-as-video.S` touches a DMA controller for
  this path.
- **Two-stage, mostly-async pipeline, confirmed by the code and by commit
  `ef8a7eaf2a`:** (1) host writes pixel data into the BCM's own SRAM
  command-parameter area, then kicks `BCMCMD_LCD_UPDATE`; (2) the BCM then
  autonomously pushes that buffer out to the physical glass, tracked by a
  **tick task** (`lcd_tick`, called every system tick) that polls
  `BCMA_COMMAND` and only re-kicks when the BCM reports idle, with a
  stall/re-kick safety net at `BCM_UPDATE_TIMEOUT = HZ/20` = **50 ms**
  (code comment: "must be guaranteed to be >~20 ms"). Critically,
  `lcd_update()` in the main firmware **does not block** waiting for stage
  (2) to finish — it returns once stage (1)'s pixel write completes (or
  immediately posts `LCD_NEED_UPDATE` if the BCM is still busy from the
  prior frame). The blocking "wait for finish" poll (`bcm_command`,
  `lcd-video.c:492-507`) exists only for sleep/wake/shutdown, and a
  separate always-blocking variant exists **only in the bootloader**
  (`#else /* BOOTLOADER */` branch, lines 290-306; commit `9daf658ef1`)
  because the bootloader has no tick/interrupts to run the async state
  machine.
- **Fast vs. slow update path — directly relevant to strip animations.**
  `lcd_update_rect` (lines 386-432) has two branches:
  - `width == LCD_WIDTH` (320): one `bcm_write_addr` call, then **one**
    `lcd_write_data(addr, width*height)` call over the fully-contiguous
    framebuffer block (rows are contiguous in memory when width==stride).
    This is the fast path — it's what both `lcd_update()` and any
    full-width partial-height rect (like our 320x220 body) use.
  - `width != LCD_WIDTH` (e.g. a 160px-wide half strip): a `do { }
    while(--height)` loop that calls `bcm_write_addr` **once per row**
    (re-polling the write-ready bit each time) before writing that row's
    160 pixels. A 160x220 strip therefore pays **220 extra
    address-setup-and-poll operations** that a 320-wide update of the same
    height does not pay. Net effect: half-width strips transfer ~half the
    pixels of a same-height full-width rect, but not at double the fps —
    the per-row overhead eats back some of that theoretical 2x.

## 3. First-principles estimate

Bytes/frame (RGB565, 2 bytes/px):
| Region | Pixels | Bytes | Path |
|---|---|---|---|
| Full 320x240 | 76,800 | 150.0 KB | fast (contiguous) |
| 320x220 body | 70,400 | 137.5 KB | fast (contiguous, width==LCD_WIDTH) |
| 160x220 half-strip | 35,200 | 68.75 KB | slow (220 per-row address resets) |

Minimum bus transactions = 1 x 16-bit cycle per pixel (no amortization
possible — see §2). Per-transaction cost is gated by the PP5022 external
memory controller's wait-state count for that chip-select region, which is
not published; it is some small fixed number of **core clock cycles**
(interleaved with the SDRAM framebuffer read's own cycles). Because Rockbox
routinely `cpu_boost()`s around LCD-heavy UI work on this target
specifically *because it measurably helps*, the wait-state count is
core-clock-relative, not an independently-clocked async bus — so raising
CPU_FREQ does shorten the host-side stage (1) time close to linearly.
However, commit `b05066de12`'s explicit note that a 5-6% host-side cycle
reduction produced a *smaller* fps gain is direct evidence that stage (2)
(the BCM's autonomous glass push) becomes co-limiting, especially for
large/full-screen updates — so **fps vs. CPU clock is sub-linear**, most
sub-linear for the full 320x240 case and least sub-linear for small rects.

Modeled per-pixel host cost: ~4-8 core cycles (order-of-magnitude guess
for an ldm/stm pair into a waited-state external bus of this era, no
independent measurement). Applying that with a sub-linear CPU-scaling
correction (full-screen most capped, partial updates least capped) gives
the ranges below. These are **model-based point estimates with wide error
bars**, not measurements.

## 4. Conclusion table (confidence: LOW-MEDIUM — modeled, not measured)

| Update region | @ 30 MHz (unboosted) | @ 80 MHz (boosted) |
|---|---|---|
| (a) Full 320x240 | ~45-55 fps | ~65-85 fps |
| (b) 320x220 body (fast path) | ~50-60 fps | ~75-100 fps |
| (c) 160x220 half-strip (slow path) | ~70-95 fps | ~120-165 fps |

Note (c) is faster in raw fps than (b) despite the per-row overhead
penalty, simply because it moves half the pixels — but its *per-pixel*
efficiency is worse, and it costs real CPU time that a full-width rect of
the same area would not.

## 5. Implications for our animations

- **190 ms slide, 320x220 body:** at (b)'s rates, that's roughly
  **10-11 frames unboosted, 14-19 frames boosted**. Either is enough frames
  for a visually smooth 190 ms transition — raw LCD bus throughput is very
  unlikely to be the bottleneck for this animation, even unboosted.
- **List glides, 320x220:** same numbers apply; smooth scrolling is bus-
  throughput-feasible at both clocks.
- **Reveal/split animations using 160x220 half-strips:** also comfortably
  fast in raw fps (c), but prefer **full-width, partial-height rects** over
  half-width strips wherever the design allows — the fast contiguous path
  avoids 220 redundant BCM address-reprogram operations and is the more
  CPU-efficient way to push the same-sized region.
- **The real risk is almost certainly upstream of the bus write**, not the
  bus write itself: compositing/alpha-blending/scaling cost for each
  transition frame (pure CPU math before `lcd_update_rect` is even called)
  is far more likely to be the limiting factor than the ~1 bus-cycle-per-
  pixel transfer analyzed here. Boost the CPU during the transition
  regardless (Rockbox already does this convention for LCD-heavy UI work
  on this target).
- **No DMA exists for this path** — it cannot be added to the LCD push
  without a new driver; this is a hard structural fact, not a tuning knob.
- **COP (second core) offload** would not speed up the bus transfer itself
  (one shared BCM bus/chip-select, serial regardless of which core issues
  the stores), but could usefully move YUV/compositing math or next-frame
  preparation off the main core, improving overall frame pacing/consistency
  during an animation even though it wouldn't raise the bus throughput
  ceiling in the table above.
- **Fewer frames are not obviously needed** on throughput grounds alone
  given the table above, but should still be considered if upstream
  render/compose cost (not covered by this research) turns out to be the
  actual limiter.
