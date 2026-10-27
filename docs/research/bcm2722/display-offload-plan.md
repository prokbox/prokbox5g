# Moving pixel work to the BCM2722: a ranked plan

Research note, 2026-10-23 (owner: *"we're doing everything pretty CPU heavy, from art rendering to UI rendering … it could be done faster, cheaper, and more efficiently on the video chip"*). It inventories what the PP5022 spends on pixels today, what our BCM firmware can already do, and ranks what to move, in what order, and what has to be true first. Nothing here is built; numbers carry their tags and sources.

Companions:
- `docs/bcm2722-reference/graphics.md`: the chip-side figures, the drawing model, §6a's lessons.
- `offload.md`: the drift design.
- `approach.md`: the "UI as GPU" rules.
- `docs/hardware-fit-audit.md` C10: the compositor item.
- `docs/design/lit-24mhz.md`: the lit-clock budget.

## 1. The one fact that changes the power argument

**While the screen is lit, the BCM is already powered and running.**
- It owns the panel, and every update goes through it.
- It powers down with the backlight (FS#9890) and comes back cold [rockbox; silicon].
- Between commands, our image busy-polls today (idle sleep isn't proven: #3, stage 5i).

So pixel work moved to the chip *while lit* costs it little extra energy: its core runs either way [hypothesis until stage 5s's meter readings, #12]. It removes the same work from the PP, which then idles or drops its clock.

`approach.md`'s rule "no rendering changes before the power question is answered" therefore bites only on work that would **wake** the chip on its own: background art building with the screen dark. Lit UI offload can go ahead once L2 is in place (§5).

## 2. Where the PP's pixel time goes

| What | When | PP cost | Source |
|---|---|---|---|
| **Drift pan and half steps** (main menu, split screen) | 13.3 pushes/s, 160×240, whenever lit at rest | ~15 ms of CPU push each at 30 MHz (about 20 % of the CPU), 25 % at 24 MHz; it is "the only sustained CPU-push stream" and **what pins the lit clock at 30 MHz** | [E] `lit-24mhz.md` §2.3; `offload.md` §2 |
| **Drift and panel cross-fades** (`xfade_rect`) | 8 steps per 7 s cover; 6 per highlight change | 1.19 M cycles, 40 ms a 160×240 step unboosted (over its 50 ms slot), plus the push | [E] `offload.md`; [sim] the BCM kernel: bit-exact, < 1 ms |
| **List marquee** | 20/s while a long title is selected | re-renders the whole row each step: 193–288 k instructions, **15–38 % of the CPU** (before F1/F2; since F2 renders the row once, about 5 %: `phase19-brief.md` §1A), plus a 15–35 KB push | [M] harness, `code-audit-1.md` |
| **List glide** | ~30 fps while scrolling, boosted | 230 k–610 k instructions (with quick-scroll) plus a 320×220 E2 push, 14–27 ms of port a frame | [M] |
| **Push/pop slides** | ~6 frames, 190–290 ms, boosted | before frame 1: capture, full redraw and a 429 k-instruction swap, 60–100 ms at 30 MHz; then a memcpy and a full-width push per frame | [M] harness, `code-audit-1.md` F4 |
| **Text** | every row's first appearance | about 30 instructions a cell pixel; a 9-row list about 1.1 M, 44–73 ms at 30 MHz | [M] |
| **Header, chevron, shadow, NP reflection** | on change | 75–90 k a header, 14.6 k a chevron; NP reflection unmeasured | [M]; gap |
| **Album-art decode** (art builder) | after library changes, idle or charging | **26–46 s of decode per large cover** at 24–30 MHz (a 2.6 MB JPEG); each album decoded 2–3 times (thumbnail, Now Playing, panel); a full pass took 28–65 min on the owner's library | [M] `handoff-2026-10-19-crash-fat.md`, `usb-history.md:67` (the I/O part has since been fixed; decode time stands) |
| **Now Playing composition** (`np_compose`) | once per album at build | ~281 k instructions a bundle | [E] `phase11-brief.md` |
| **Pushing pixels at all** | every update | ~10.5 PP cycles a 16-bit transfer: a full frame is 27 ms of CPU at 30 MHz; E2 makes most pushes CPU-free (9.3–10.5 MB/s), but pieces under 32 KB still spin the core | [M] S8, `lit-24mhz.md` §2.2 |

**RAM tied to pixels** (all reserved from the audio buffer; `memory-audit-2026-10-20.md`):

| Buffer | Bytes |
|---|---|
| Drift cover slots | 360,000 (180,000 on a tight 32 MB build) |
| Strip | 180,000 |
| Row-art LRU | 109,428 |
| Art builder: decode block, pinned work, Now Playing read-ahead | about 0.27 MB |
| Cover Flow (P7), planned | 512 KB more |

## 3. What the chip can already do

**Built** (our firmware):
- **Panel push:** full frame and rectangles by DMA from any SRAM source and pitch (`lcd_blit`, [silicon]).
- **Video:** YUV→RGB565 tile conversion and a 2:1 box downscale ([silicon], stage 6).
- **Overlay:** the host framebuffer over video, with a chip-timed slide (`HP_OP_OVERLAY`, [silicon]).
- **Drift ops:** SHOW, PREP, FADE and HALF over two 300×300 slots (`video/drift.c`, [sim], bit-exact).
- **3D:** a textured-triangle renderer (`r3d`, [sim]).
- **Engine 2:** uploads at ~11 MB/s with the PP 97 % free ([silicon] S37).
- **Tooling:** vc2sim with bit-exact checks, and the Mac cross-build check.

**Missing:**
- **A general 2D job interface:** `HP_CAP_JOBS_2D` is defined, unadvertised and unimplemented. SRAM `0x140–0x1DF` is reserved for its header.
- **A content-hashed asset cache.**
- **Primitives:** scroll-rows, slide, fill, alpha blit and glyph runs.
- **Any image decoder** (JPEG), and **arbitrary-ratio scaling.**
- **A merged UI-plus-video SRAM plan:** the video arena's 620 KB would double as 2D scratch when no video is open.
- **Our image as the default** (L2; `bcmfw/verified.txt` is still empty).
- **A TE-synced present on silicon** (#52; stage 3d measures it).

## 4. The ranked moves

Ranked by sustained PP time removed × how often it runs, then RAM returned, then effort.

### 1. The drift on the chip (#119)

- **Gain:** the sustained 10–20 % of the CPU drops to about 0.3 % [E]. That **lets the lit clock drop to 24 MHz** (`lit-24mhz.md`'s blocker). The 360 KB of cover slots move to chip memory: 180 KB per cover upload by E2, about 16 ms, once per 7 s.
- **Built:** the ops and kernels, and stage 5b's A/B.
- **Missing:**
  - stage 5b on silicon;
  - the drift ops in the resident image (an SRAM layout beside video's arena, or SDRAM slots);
  - the shell calling them, with the CPU path kept as the fallback.
- **Effort:** medium. **Risk:** low; the CPU path stays.

### 2. A retained screen: the chip keeps frames and moves them (#75, #76)

The chip keeps the current screen, plus a second frame for the page being entered.
- **Push/pop slides:** the PP renders the new page once (it already does), uploads it once by E2 (~15 ms), and the chip slides the two at 30 fps by DMA offsets. The capture, swap and per-frame push disappear (60–100 ms of start latency and ~6 full pushes per slide).
- **List glide:** the chip scrolls the rows it holds, and the PP uploads only the newly exposed rows. The 320×220 push per frame disappears.
- **Gain:** smoother slides and glides that no longer need a boost.
- **Missing:** a second frame in SRAM, and `SCROLL`, `SLIDE` and `PRESENT` jobs.
- **Effort:** medium. **Risk:** medium (pixel-exact parity with the CPU path).

### 3. The marquee as a window

- The PP renders the full title once into a wide strip (as the audit's F2 already proposes for the PP) and uploads it once.
- The chip pans a window over it at 20/s: the drift's `SHOW` on a different picture.
- **Gain:** the marquee's CPU share (about 5 % since F1/F2 landed; 15–38 % before) becomes about 0.
- **Effort:** small once #1's slots exist.

### 4. JPEG decode on the chip, for the art builder, Photos and Cover Flow

The largest absolute cost: **26–46 s of decode per large cover** [M].

**A baseline JPEG decoder on the chip:**
- Huffman decoding on the scalar core.
- An 8×8 IDCT as a vector kernel. It's new: H.264 baseline uses 4×4 transforms. The IDCT sits in the same family as the proven decoder kernels.
- The proven YUV→RGB tile kernel for colour.
- A streaming area downscale and an ordered dither, in MCU rows. Little memory: no full-size image is ever held.

**Gains:**
- **Decode once, all sizes:** thumbnail 55², Cover Flow 128², Now Playing 142² and its composed card, panel 300², and the blurred fill. That ends today's 2–3 decodes per album.
- **Moves `np_compose` and `artblur` to the chip** in the same pass.
- **Speed** [estimate; needs a spike]: at 150 MHz, roughly 1–2 s for a large cover (~9 Mpx) and 0.1–0.3 s for a typical one, against 26–46 s on the PP (refined in `jpeg-spike.md` §1; the spike's counts replace it). A 28–65 minute library pass would take minutes.
- **Upload:** the file bytes go up by E2 at ~11 MB/s, 0.24 s for a 2.6 MB file.
- **A gap closed:** progressive JPEG becomes possible later on the chip, and today it fails on the PP.

**Power:** this is the one item that wakes the chip with the screen dark, so it waits for stage 5s's numbers (#12). Even so, the chip at 150 MHz for well under a second likely beats the PP for 30 s [hypothesis].

**Order:**
1. A vc2sim spike: our decoder bit-exact against the PP's `jpeg_load.c` output on the art test set, with instruction counts.
2. A device bench.
3. A "decode job" in the 2D interface.

- **Effort:** large. **Risk:** medium (the PP decoder stays as the fallback and the reference).

### 5. Cover Flow on the chip (P7)

- 16 covers of 128² live on the chip as palette textures (8-bit plus a colour table, as `r3d`): 16 KB each, or 32 KB as RGB565.
- The scene is a few textured quads drawn by the `r3d` engine (tessellation instead of perspective correction).
- The PP sends a few KB of commands per frame.
- **Gain:** ~512 KB never reserved on the PP, and Cover Flow at 30 fps, which the PP can't push.
- **Effort:** medium, given `r3d`. Class C at 150 MHz (`graphics.md`).

### 6. Photos and video thumbnails (P14), chip-first

- **Photos:** the chip decodes and downsizes (from #4) for thumbnails at sync time. Full-screen viewing, with zoom and pan, is decoded straight into the chip's frame. The PP never holds a photo.
- **Video thumbnails:** grab the first I-frame through the chip's H.264 decoder, then scale it with #4's scaler.
- **Gain:** no second image pipeline on the PP.

### 7. Text on the chip: a glyph atlas and a glyph-run job

- The fonts (about 103 KB) are uploaded once as 4-bit alpha atlases.
- The PP sends glyph indices and positions, and the chip blends 16 pixels at a time on the vector unit.
- Worth it **after** the PP-side fix F1: skip transparent alpha, about −34 % of text cost (`code-audit-1.md`). Strip caching already means text is drawn only when a row first appears. The recurring text costs are #3's marquee and new rows during a glide.
- **Effort:** large. **Risk:** pixel parity with Rockbox's alpha blend.

### Also worth doing, on the PP and independent of the chip

These are not chip work, but they cut the same costs now:
- **F1:** skip transparent glyph pixels.
- **F2:** render the marquee once and pan it (pairs with #3).
- **Art builder decode-once:** several outputs from one decode, a step toward #4.
- **Measure the NP reflection** (no figure exists).

### Art storage formats (owner decision, 2026-10-23)

- **Album art stays 16-bit (RGB565), the screen's native input** (`docs/bcm2722-reference/lcd.md` §4). The panels' controllers expand it to 18-bit internally; sending 18-bit would take two transfers a pixel and has never been tried.
- **256-colour covers** (8-bit indices and a 512-byte colour table each, as the 3D textures use) **are held in reserve as an emergency option only.** Use them if memory or disk ever forces it, not by default. They would halve every art size (a panel 180,000 → ~90,500 B, the drift slots 360 → 180 KB, the cache ~500 → ~250 MB for 5,000 albums), but they drop below the screen's 65,536 colours and can band on gradients.
- **Not pursued now; options if needed:**
  - block compression (two 16-bit colours per 4×4 block, 4 bits a pixel, a quarter of the size, colours still within 16-bit);
  - colour-table shading for Now Playing's reflection.

  Any of these would first need a quality trial on the owner's covers (locally, nothing committed) against the 16-bit reference.

## 5. What has to be true first

1. **L2: our resident image as the default display driver.** Apple's vmcs can't run our jobs. Stage 5's verified image list (`bcmfw/verified.txt`) needs its first entries. This is P19's first item.
2. **A merged display image and SRAM plan.**
   - Video, the 2D jobs and the drift share one resident image.
   - The video arena (~620 KB SRAM) doubles as 2D scratch and slots when no video is open: video never runs beside the UI's animations, only beside the overlay.
   - Textures beyond SRAM go to SDRAM, which needs our bring-up and has its second-attempt quirk (#7).
   - The resident image's 203 KB PP copy is the price. Keep the image lean.
3. **The 2D job interface** (`HP_CAP_JOBS_2D`):
   - a ring of small ops: `UPLOAD slot`, `BLIT`, `FILL`, `SCROLL`, `SLIDE`, `FADE`, `PAN`, `GLYPHS`, `DECODE`, `PRESENT rect [TE]`;
   - assets keyed by content hash;
   - the epoch bumped (and the cache evicted) when video opens;
   - every op with a pixel-exact CPU fallback (`approach.md`).
4. **Measurements:**
   - power in each chip state (#12, stage 5s);
   - core sleep (#3, stage 5i);
   - the panel's refresh and tearing (#52, stage 3d);
   - SDRAM bandwidth (#7);
   - whether port traffic slows the chip (#44).

## 6. Suggested order

| Step | What | Gate |
|---|---|---|
| 0 | PP-side F1, F2 and decode-once; measure the NP reflection | none |
| 1 | Stage 5b on silicon; drift ops in the resident image; the shell uses them, CPU fallback kept; try the lit clock at 24 MHz | L2, the 18-0 round's power data |
| 2 | The 2D job interface plus the retained screen: second frame, slides, glide scrolling, the marquee window | step 1 |
| 3 | JPEG on the chip: vc2sim spike, then device bench, then the art builder's decode job | stage 5s power numbers |
| 4 | Cover Flow (P7) on `r3d` with chip textures; Photos and video thumbnails (P14) chip-first | steps 2–3 |
| 5 | Glyph atlas and glyph runs, if text still shows in the profiles after step 0 | step 2 |

**Expected result** [estimate]:
- The lit UI's sustained PP load (the drift and the marquee) mostly disappears.
- Slides and glides stop needing a boost.
- Roughly 0.9–1.4 MB of pixel buffers stop being reserved from the audio buffer (drift slots, strip, Cover Flow, Now Playing buffers).
- Art builds shrink from tens of minutes to a few.

Each step is measured before and after, with the CPU path kept as the reference (`approach.md`).
