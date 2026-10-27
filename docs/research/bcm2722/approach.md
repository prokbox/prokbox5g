# BCM2722 video: approach (our own VideoCore firmware)

**Decision (owner, 2026-10-10):** ProkBox runs **its own VideoCore code** on the BCM2722, written from scratch. We don't load Apple's `vmcs.bin` or `.vll` libraries at runtime. Apple's binaries are **reference material only**: they're where we learn the hardware (register addresses, DMA, LCD and TV, SDRAM, the vector unit). Our code is our own and ships under GPL. Issue #60.

## What the reference stack tells us (static, 1.3)

- **H.264 is software on the VideoCore's vector processor, not a fixed-function block.** `h264dec.vll` exports 298 functions. They're a JM-reference-style decoder (`InterpretSPS`, `init_dpb`, `store_picture_in_dpb`, `decode_one_frame` …) with hand-vectorised kernels: `vc_interp16x16*`, `vc_inverseinteger`, `vc_intra_4x4_*`, `vc_LumaEdges`/`vc_ChromaEdges` (deblocking), `vc_parse_*` (CAVLC) and `h264_refstripe_*` (reference-frame striping between SDRAM and internal RAM). `mpg4dec.vll` (236 functions) has the same shape. **Consequence:** "hardware decode" here means *our decoder running on the VideoCore VPU*, with DMA and vector kernels. Nothing in Apple's design needs a magic undocumented decode block.
- `vmcs.bin` (Copyright 2005–06 Broadcom Europe, 2002–04 Alphamosaic) is the RTOS plus drivers: an SDRAM driver, DMA (`dma_memcpy`, `dma_memcpy2d_uncached`, transfer queues), TV DMA, display, and the host message queue. These are the hardware facts we need to recover. We'll write our own small equivalents.
- **Booting our own code is already solved:** Rockbox's `bcm_init()` (`firmware/target/arm/ipod/video/lcd-video.c`) uploads an image to BCM SRAM at 0, then starts it via `0x10000C00` / `0x10000400 = 0xA5A50002`. The same path loads our image instead of the NOR-flash `vmcs`.

## Consequence: our firmware owns the LCD too

While it runs, our image replaces whatever drives the LCD today. So it must at least implement Rockbox's existing contract (`BCMCMD_LCD_UPDATE`/`UPDATERECT` at `0x1F8`, the status word, the framebuffer at `0xE0000`, and sleep). Done right, that removes Rockbox's runtime dependence on Apple's NOR `vmcs`. It also opens a door: the BCM could do scaling and compositing (Cover Flow, transitions) instead of the slow PP→BCM pixel push (`docs/research/lcd-fps-ipodvideo.md`).

**No Apple code (#62):** the path to a display that never loads Apple's `vmcs`, in levels from today's swap to the flash ROM, is in `independence.md`.

## Later: the BCM as the UI's GPU (owner question, 2026-10-10)

Yes, in principle. Once our own image owns the chip, the shell can send **draw commands instead of pixels**. Assets (font atlases, icons, pre-scaled art from `docs/design/artcache.md`) get uploaded once into the BCM's SDRAM. The PP then sends small commands like "blit glyph run", "fill rect", "scroll region by n" and "composite layers A/B at offset t". That attacks the measured bottleneck: today every frame crosses the 16-bit bus pixel by pixel (`docs/research/lcd-fps-ipodvideo.md`). Glides, slide transitions, the Now Playing reflection and Cover Flow (perspective-warped quads on the vector unit) would get cheap and smooth. Apple shipped a `RenderServer.bin` next to the video stack, which hints they used the chip this way for some rendering.

Costs and rules, when we get there:
- **Battery.** Rockbox powers the BCM down with the backlight (FS#9890, >30% saving). A UI GPU must sleep just as hard: wake on input, render, idle. Measure it against `docs/hardware.md`.
- **Two render backends.** The PP renderer stays the reference and fallback, and the simulator needs a BCM model, so `tests/ui/*.script` screenshots keep working (pixel-exact between backends).
- **Asset upload discipline.** Assets go up once and are cached by content hash (bulk I/O, not streamed).
- Sequencing: this comes **after** workstream 1 (bring-up proves the LCD path) and can grow from it incrementally (rect blits → scroll → layered compositing). It doesn't block video.
- **Shell contract (agreed on #60, 2026-10-10).** Our firmware exposes a small, documented **2D job interface**: scale (album art), blend/composite (the reflection and the list and Now Playing layers), fill (including blurred panel fills) and blit into the LCD framebuffer. Each job type comes with a **measured power and latency cost** compared with the CPU path. The shell picks the BCM or the CPU **per operation** behind a `HAVE_*` flag, and the CPU path stays as the fallback (rule 7). The shell session writes the CPU-side hook once the interface exists. **No rendering changes before the power question is answered:** does waking the BCM for a job cost more than the PP5022 doing it, given that LCD updates already pass through the BCM?

## Workstreams, in gating order

0. **ISA and toolchain (go/no-go gate).** Work out whether VideoCore II's scalar and vector encodings match the community-documented VideoCore IV VPU. Test: disassemble `h264dec.vll` with a VC4 disassembler, using its `.dynsym` function boundaries and `.rela.text` as ground truth. If it matches, adopt and extend an existing VC4 toolchain (binutils/GCC port) for VC II. If not, write an assembler plus Ghidra SLEIGH spec from the reference binaries. No C compiler is needed at first: kernels in asm, control code in C once GCC works.
1. **Bring-up firmware ("hello BCM").** Our image boots via `bcm_init()`, sets clocks and SDRAM, and answers `BCMCMD_LCD_UPDATE` by blitting the host framebuffer to the LCD. This is the first point where device time is needed (owner permission + backup), and the minimum that proves we own the chip.
2. **Host protocol (ours).** A simple mailbox plus ring buffer over the two host ports (main and ALT). Commands: open (codec, size, scale/letterbox), feed NAL/VOP packets with PTS, clock and sync, pause, seek and close, plus status and stats. The PP side is a Rockbox driver + demuxer (MP4/M4V); audio stays on Rockbox's codec path.
3. **Decoder.** H.264 Baseline (CAVLC, no B-frames, ≤ 640×480 input, scaled to 320×240), then MPEG-4 Part 2 SP. Start from a clean, permissively licensed or GPL-compatible baseline decoder in portable C, ported to the VPU, and then vectorise the hot kernels (interp, IDCT, deblock) in that order, measured each time.
4. **Scaler and display path,** TV-out last (DAC and encoding).

## Use the hardware fully: modern techniques (owner, 2026-10-10)

**Direction:** the long-term firmware should get everything out of this hardware, however far that takes it from upstream Rockbox. Part of the project's point is to show how far careful, AI-assisted engineering can modernise old hardware. Apple's 2006 design is the floor, not the target.

Apple's decoder is JM-reference-style control code around hand-vectorised kernels. Since then, open decoders and encoders have found much cheaper ways to do the same work. We plan to use them:

1. **Decoder structure from ffmpeg's H.264 decoder (LGPL 2.1+, GPL-compatible), not JM.** Table-driven CAVLC and bit reader, fast paths for skipped and empty macroblocks, DC-only inverse transforms, and no deblocking work where bS = 0.
2. **Decode-friendly encoding.** If we ship a converter (an open question in the Phase 14 brief), it encodes for cheap decoding: x264 `--tune fastdecode`-style settings within Baseline, and exactly 320×240 so the chip never scales. Apple had to play whatever iTunes produced; we choose the input.
3. **Kernel patterns from ffmpeg's NEON/SSE assembly.** Those kernels map closely onto the VPU's 16-lane vectors.
4. **Race to idle.** Decode a few frames ahead in a burst, then clock down or sleep until display time, instead of decoding at a steady pace. This matches the PP side's bulk-I/O rule (`docs/hardware.md`).
5. **No intermediate copies.** Colour conversion and output go straight to the display path.
6. **Measured, not guessed.** The simulator counts instructions for every kernel, and the bit-exact check against ffmpeg guards every optimisation.

**Keeping upgrades possible:**
- Kernels are swappable units, each with its own simulator tests.
- The host protocol carries a version number, so the chip image and the PP driver can move forward independently, and a newer image ships as an ordinary update.

**Limits.** The hardware ceiling doesn't move. H.264 Baseline at about 640×480 is the practical top; HEVC and AV1 are out of reach. The gains are cost per frame. Whether they go to battery life or to higher bitrate and resolution is the owner's call once real timings are measured. No speed-up figures are claimed before then.

## How we verify without the device
- A VideoCore instruction-set simulator (ours, in Python or C) is built alongside the toolchain. It's validated by single-stepping known reference functions against hand-computed results, then runs our decoder on the corpus (`tests/video/`) and checks its output against ffmpeg's YUV, bit-exact.
- On the ARM side, a host build of the demuxer + protocol against a fake BCM.

## Risks
- VideoCore II ISA may differ substantially from VC IV. That's workstream 0, and it gates everything.
- The vector-unit timing is unknown. H.264 at 320×240×30 on the VPU is proven only by Apple's decoder existing. Ours must reach similar efficiency: budget a profiling loop.
- LCD panel init and timing must be recovered from `vmcs.bin` before our image can display anything.
- Size of the effort: this is a multi-phase project, not one spike. MPEG-1/2 on the ARM (mpegplayer) remains the shipping fallback until our decoder lands (ROADMAP Phase 14).
