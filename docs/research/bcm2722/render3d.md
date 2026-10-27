# The 3D render test: a PS1-style renderer on the BCM2722

A proof of concept for games on the video chip (P12's resource classes B and C, `docs/phase12-brief.md` §2; P19's texture slots, #119). It answers how much real-time 3D the chip can draw for a game. `docs/bcm2722-reference/graphics.md` has the chip-level figures; this file records the renderer, the scenes, what the simulator measured, and what to look for on the device.

Status (2026-10-26): **run on silicon: every frame equals the simulator's** (CRC at 0 ms and 5,000 ms of every scene, at 27 and 150 MHz: "frames unlike the simulator: 0") and **30 fps holds at 150 MHz up to about 1,350 triangles a frame** (§"On silicon"). The device test is stage 3d, described below. This renderer is the reference engine for game developers (owner): its code and figures are the worked example for the SDK's 3D (`docs/design/sdk.md`).

## What it is

Our own software renderer, in C on the BCM's scalar core (`bcmfw/r3d/`). It has no hardware help: the chip has no 3D block.

**Geometry:**
- Q14 rotation from a look-at camera. World units are integers; screen vertices are integers (the PS1's vertex jitter is part of the look).
- Near-plane and guard-band clipping in view space (Sutherland–Hodgman, five planes). Back faces culled. Triangles wholly off one side of the screen, or past the fog, are dropped.
- **Chunk culling:** each object, and each 8×8 block of terrain, carries a bounding sphere. A chunk outside any of the six view planes is skipped before its vertices are transformed.

**Drawing:**
- Painter's ordering table: 512 buckets by mean view depth, drawn far to near, with no depth buffer.
- **Sort bias per triangle:** flat ground goes into the farthest bucket, so it never covers what stands on it. Hilly ground is biased back by one cell.
- **Edge-based tessellation:** an edge near the eye whose far end is more than 1.5× as deep as its near end, and longer than 12 pixels on screen, is split at its view-space midpoint, up to six times.
  - This removes the affine warp of big near ground triangles.
  - The decision belongs to the edge alone, so neighbouring triangles split shared edges alike and no cracks open. A first, per-triangle version left hairline cracks along T-junctions.
- **Textures:** eight 64×64 textures of 8-bit texel indices (grass, flagstones, brick, roof tiles, crate, rock, bark, leaves).
  - They're made at start from integer noise and patterns. No art is stored.
  - Each texel is one of 32 brightness steps on its texture's colour ramp.
- **Shading:**
  - The light is baked per triangle: an ambient term plus a low sun.
  - Fog is per vertex, by view depth.
  - Their product is interpolated across the triangle (Gouraud shading) and picks a row of a 48 × 256 colour table, which blends toward the dusk fog colour. Eight guard rows each side absorb rounding.
- **Affine texture mapping** in 16.16 steps.
- One pixel costs one texel load, one colour-table load and one store.

**Memory** (`bcmfw/config.h`, `BCMFW_R3D_*`; `bcmfw/build.py` checks that none of it overlaps):

| Region | Start | Size |
|---|---|---|
| The image (`hello_3d`, 29 KB) | `0x000000` | to `0x82BC` with its bss |
| Textures | `0x010000` | 32 KB |
| Colour table | `0x018000` | 24 KB |
| Scene vertices | `0x020000` | 2,048 × 8 B |
| Scene triangles | `0x024000` | 4,096 × 16 B |
| View-space vertices | `0x034000` | 2,048 × 20 B |
| Ordering table | `0x03E000` | 512 × 2 B |
| Screen triangles | `0x040000` | 6,144 × 24 B |
| Stack | `0x0CC000` | 16 KB |
| Frame A (Rockbox's framebuffer) | `0x0E0000` | 150 KB |
| Parameter block | `0x106000` | 25 words |
| Frame B | `0x108000` | 150 KB |

All of it fits in SRAM, so SDRAM stays off. Each frame renders into one framebuffer while the other goes to the panel by the chip's LCD DMA (`lcd_blit`, the video player's path, [silicon] since the player ran).

## The scenes

Each scene is built at start from integer formulas. Every camera path loops in 10 s. The counts are per frame, from the simulator:

| # | Scene | Triangles | Drawn a frame | Pixels a frame | Simulator instructions | Estimate at 150 MHz | at 27 MHz |
|---|---|---|---|---|---|---|---|
| 0 | Courtyard: flagstones, crates, pillars, an obelisk; an orbit | 250 | 185 | 42–44 k | 1.04–1.07 M | 7.1–7.4 ms | 40–41 ms |
| 1 | Village: 6 houses, 24 trees, a well, crates; a high orbit | 696 | ~430 | 93–100 k | 2.35–2.51 M | 16–17 ms | 90–96 ms |
| 2 | Valley: a 24×24 height field, 14 trees; a high circle | 1,348 | 1,075–1,090 | 57–58 k | 2.18–2.25 M | 15–15.5 ms | 83–86 ms |
| 3 | Ruins: a 34×34 valley, 16 pillars, 12 trees; a flyover | 2,828 | 2,225–2,255 | 60–63 k | 3.23–3.33 M | 22–23 ms | 123–127 ms |
| 4 | Village walk: scene 1 at eye height round the well, with a head bob and sway | 696 | 183–187 pieces (after splits) | 87–109 k | 2.28–2.63 M | 15.6–18 ms | 87–100 ms |

The estimates use 1.03 cycles per scalar instruction, the S7 bench's figure [estimate]. Three things are not modelled: memory stalls, the cache (off), and port traffic competing with the core (#44). So the device numbers will be higher.

**What the estimates say:**
- **At 150 MHz** every scene renders inside a 33 ms frame, so a game at 30 fps has room.
- **At 27 MHz** only the smallest scene comes near 30 fps: about 40 ms, 25 fps. The rest give 8–12 fps.
- So 3D games are Class C (the chip held at 150 MHz for the session), as `graphics.md` §3 expected.

**Where the time goes:**
- The pixel loop is 14 instructions a pixel: compiled, read from the listing. It was 17 until the texture coordinates moved to 6.26 fixed point, which made every frame 7–13 % cheaper with bit-identical output (see "Findings on the way").
- Scenes 1 and 4 cost the most for their triangle count, because they cover the most pixels: a near ground plane, house walls, and big trees overdrawn back to front.
- Halving overdraw would be worth more than halving triangles.

## The device test: stage 3d

The developer menu entry **"BCM test: stage 3d (3D render, optional)"** (`apps/ipodui/devbcm.c`). It needs a stage 5 pass flag (`bcmpass5.txt`, like 5b, 5i and 5s); the 150 MHz runs also need 4b and 4c flags. It writes no pass flag.

1. Boot `hello_3d`, then `LCD_POWERON` and the backlight. The PP stays unboosted. Then time the TE line for 500 ms ("Holding 30 fps").
2. For each scene, 0 to 4, at 27 MHz:
   - the host writes the parameter block (scene, 10 s, check point 5,000 ms, period 33,333 µs) and kicks `CMD_R3D` (0x44);
   - the image renders the check frame at 5,000 ms off screen and keeps its checksum;
   - it then runs the scene live for 10 s, holding each frame to its 30 fps slot, and checksums the first live frame (0 ms);
   - it returns the results. Per run: frames, run time, rendering time (setup and raster split), the slowest and fastest frame, wait for the panel, idle time before slots, late frames, and per frame the triangles drawn, culled and clipped, pixels, and chunks skipped.
3. With the flags: `CMD_CLOCK` up to 150 MHz, the five scenes again, `CMD_CLOCK` down. If the probe found a period, the walk runs once more with TE-synced presents, at the last clock.
4. **Pass** means every run finished and every check frame's checksum equals the simulator's (`firmware/target/arm/ipod/video/bcm-devtest-r3d.h`, written by `r3d_test.py --write`).
   - A checksum that differs means the chip computed something the simulator does not model. Report it with the log, because it's a silicon finding.
   - With PLL runs it takes about 2 minutes, without about 1.

**The log** (`bcmtest.log`): four lines per run, all prefixed `3d <scene> @<clock>:`, then one checksum line.
- frames, fps and late frames;
- the rendering average, its setup/raster split, and the slowest and fastest frame;
- per frame: triangles drawn, culled and clipped, and pixels;
- the panel wait, idle time, and chunks skipped;
- the checksum line: each check frame's checksum and the simulator's.

The developer power log marks each run (`s3d_<scene><s|f>`, then `s3d_end`).

**What to watch:** five 3D scenes, each 10 s:
- a courtyard orbit;
- the village from above;
- a valley circle;
- a ruined valley flyover;
- a walk through the village at eye height.

At 27 MHz the motion is slow and steppy (7–22 fps). At 150 MHz it should be smooth.

**Questions it answers:**
- the renderer's real cost per pixel and per triangle on silicon (the estimates above are from the simulator);
- whether the core at 150 MHz holds 30 fps for 2,000 triangles a frame;
- how long the panel DMA makes the next frame wait;
- the first silicon run of general scalar C this size outside the video decoder, with 64-bit divides in triangle setup;
- whether a frame drawn on the chip comes out bit-exact with the simulator.

## On silicon (2026-10-26)

[silicon] stage 3d, 2026-10-26 12:42, build 7e8c140e40-261026, image `hello_3d` 30,212 B `e077b5ae`; session log `bcmtest.log`. Stage 3d PASS; the owner watched it and saw nothing wrong.

| # | Scene | Drawn a frame | Pixels | 27 MHz: fps, render avg (setup + raster), max | 150 MHz: fps, render avg (setup + raster), max | Silicon ÷ simulator estimate at 150 |
|---|---|---|---|---|---|---|
| 0 | Courtyard | 186 | 42.7 k | 18.1; 54.9 ms (5.8 + 49.0), 60.0 | **30.0**; 9.9 ms (1.0 + 8.8), 10.8 | 1.36 |
| 1 | Village | 439 | 99.8 k | 7.3; 135.6 ms (12.8 + 122.8), 154.3 | **30.0**; 24.4 ms (2.3 + 22.1), 27.9 | 1.48 |
| 2 | Valley | 1,066 | 58.8 k | 7.7; 128.6 ms (19.2 + 109.4), 142.3 | **30.0**; 23.1 ms (3.4 + 19.7), 25.6 | 1.51 |
| 3 | Ruins | 2,222 | 60.1 k | 4.9; 199.9 ms (35.5 + 164.4), 207.4 | **27.8** (240 late); 35.8 ms (6.3 + 29.5), 37.2 | 1.59 |
| 4 | Village walk | 145 (after splits) | 91.6 k | 9.4; 105.9 ms (21.2 + 84.7), 135.1 | **29.9**; 19.2 ms (3.8 + 15.4), 24.2 | 1.14–1.23 |

- **Bit-exact.** All ten check frames carry the simulator's checksums at both clocks. A large scalar C program (64-bit multiplies and divides in set-up, 10 s scenes back to back, the LCD DMA reading the other framebuffer the whole time) computes on the chip exactly what vc2sim computes. Open question #55 is closed.
- **Time scales with the core clock exactly:** 27 → 150 MHz divides every scene's render time by 5.52–5.58 (the clock ratio is 5.56). The extra cost over the estimate is therefore on-chip cycles (SRAM loads, the uncached core: the cache is off, `boot-and-clocks.md` §2), not a fixed-latency memory: nothing here touches SDRAM.
- **The estimate is low by 1.14–1.64**, more for scenes with many small triangles: cycles ≈ 1.26 × simulator instructions + about 590 cycles per drawn triangle fits all five within ±10 % [fit, 5 points]. Raster costs 25–74 cycles a pixel including span set-up (scene 4's big near triangles 25, scene 3's small ones 74), against the 14-instruction pixel loop: the loop's two dependent loads (texel, then colour table) stall. For planning: **render time at 150 MHz ≈ simulator estimate × 1.5** (× 1.65 for dense terrain).
- **The panel refreshes at 67.8 Hz:** TE on GPIO 49 toggles, 34 rising edges in 500 ms, period 14,740 µs (14,738–14,744), a 58 µs pulse. Not 30 Hz and not 60: a frame on every 2nd edge is **33.9 fps**. The TE-synced walk ran at 33.9 fps with **0 late** frames (19.2 ms of render, 14.7 ms of headroom a frame). Open question #52 is closed.
- **The panel never waited:** 1–5 µs a frame of panel wait at either clock; the 14.3 ms push hides behind rendering, as designed.
- **27 MHz is for small scenes only:** the courtyard reaches 18 fps; everything else 5–9 fps. 3D stays Class C (150 MHz held for the session).
- **Idle is real:** at 150 MHz the courtyard idles 23.4 ms of each 33.3 ms slot, the walk 14.1 ms: the time a core idle sleep could take once proven.

### What it unlocks

1. **vc2sim is trusted for what a game draws.** A game developer can build and check frames on the simulator (or `r3d_host` on the Mac) and get the chip's exact pixels; budgets use the simulator estimate × 1.5 at 150 MHz (the fit above for tighter numbers). This is the correctness half of "vc2sim is trusted" (closure plan §5, E3) for scalar C, now shown on silicon at 150 MHz under load.
2. **30 fps 3D on the chip:** up to about 1,350 triangles or 100 k pixels a frame at 150 MHz, with 9–24 ms of render. Dense scenes (2,200 triangles) need the step-downs in "Holding 30 fps" item 4.
3. **TE pacing at 33.9 fps** is the tear-free rate for games, Cover Flow and full-screen animation; 67.8 fps (every edge) needs a frame under 14.7 ms, which the courtyard (9.9 ms) already makes.
4. **Cover Flow** (`J_OP_TRIS`, a few dozen textured quads) is far inside this budget: the walk's 145 large pieces with 92 k pixels render in 19 ms; a Cover Flow frame has fewer pixels.
5. **Apple's rasteriser is not needed:** reading `RenderServer.bin` (open question #66) was only for a shortfall; there is none.
6. **Next measurements worth having:** B6's isolated per-fetch cost (#54) to split the raster's stall between the texel and colour-table loads, and the vector colour-table pass (§Next) against this scalar baseline.

## Holding 30 fps

The goal (owner, 2026-10-23): hold a steady 30 fps, the rate the owner believes the screen runs at.

### What the screen is

- **The panel refreshes on its own.** It redraws its glass from its own memory on an internal clock, and pulses its tearing-effect (TE) line, GPIO 49, at the start of each refresh.
- **Apple measures that period at run time** rather than assuming it: a running average in its TE handler. It times its writes against the scan (`docs/bcm2722-reference/lcd.md`, "Tearing").
- **The rate is 67.8 Hz** [silicon, stage 3d 2026-10-26: period 14,740 µs]. A frame on every 2nd edge is 33.9 fps.
- **Writes have their own limit:** at most about 70 full frames a second at the 27 MHz bus words (14.26 ms a frame [silicon S7]).
- **So 30 fps is a content rate** (Apple's video peak, and a comfortable game rate), not a limit of the glass. Stage 3d now settles the refresh rate: it times the TE line for 500 ms before the scenes.

### What it takes

1. **A frame budget under 33.3 ms.**
   - At 150 MHz every scene renders in 7–23 ms (simulator estimate), so 30 fps holds with 10–26 ms to spare.
   - At 27 MHz none of the 3D scenes do (40–127 ms).
   - So 3D games are Class C and run the chip at 150 MHz for the session (`graphics.md` §3).
2. **Rendering and the panel in parallel.** Two framebuffers: frame n+1 renders while frame n goes out by DMA. The 14.3 ms push then costs nothing, as long as rendering takes longer than it. Each run's log shows the wait for the panel per frame.
3. **A steady present.** Two ways, both built:
   - **The slot timer** (the default): a frame goes at most every 33,333 µs. Simple, but the present lands anywhere in the panel's scan, so a tear line can show on fast motion.
   - **TE-synced** (stage 3d's extra run):
     - wait for the panel's TE edge, polling GPIO 49, and present on every n-th edge, n = round(33,333 µs / the measured period): 2 at 60 Hz;
     - our write (14.3 ms) outruns a 16.7 ms scan, so started with the scan it stays ahead of it, and there's no tear;
     - it also locks the frame rate to the glass, so frames don't drift against the refresh (judder).
     - It depends on the panel scanning in the same row order as our window writes [hypothesis; the run will show a tear if not].
4. **Headroom management for a real game** (not built yet). A game should watch its render time (the per-frame figure every run logs) and step down before it misses a slot:
   - first pull the fog and far plane in, so fewer chunks pass the view test;
   - then raise the tessellation threshold;
   - then draw at half resolution with each texel written twice. That halves the per-pixel loop cost, the classic PS1 trick for busy scenes.
   - A frame that still misses goes on the next edge. The next slot then counts from it, so one slow frame costs one frame, not a stutter.
5. **Spare time is power.** The run logs idle time per frame before each slot. That's what the core could sleep through if the idle sleep is proven on silicon (stage 5i, open question #3). At 150 MHz a 2,000-triangle frame leaves about 10 ms of every 33 idle.

### What stage 3d reports for it

- `TE: … rising edges …` and `TE: period N us (H Hz) …`: the panel's refresh rate, the answer to "is it 30 Hz?".
- Per run: frames, fps, late frames (missed slots), and the panel wait and idle time per frame.
- The walk once more, TE-synced (`kick R3D: … on the TE line`), at the fastest clock the flags allow. The summary shows "Panel refresh (TE)" and "Walk on TE: fps, late".
- Watch for a tear line during the timer runs and its absence in the synced run. Photograph either one.

## Previews on the Mac

`bcmfw/r3d/host/r3d_host.c` builds the same renderer for the Mac.
- `r3d_test.py` checks that its check frames carry the simulator's checksums, so its pictures are the chip's.
- `build/r3d-host/r3d_host video 30 01234 | ffmpeg -f rawvideo -pix_fmt rgb24 -s 320x240 -r 30 -i - out.mp4` renders all five scenes in about 15 s. The simulator takes about 3 s a frame.
- Videos made so far (kept locally, not in git): `out/r3d/r3d-preview-320x240.mp4` (50 s) and `out/r3d/r3d-walk.mp4`.

## Findings on the way

- **A GCC bug:** `min.l`/`max.l` lose their source register (`compiler.md`, "Known GCC bugs"). vc2cc now refuses them; no earlier image used them.
- **Camera handedness:** looking down +z with y up, the screen's right is -x in a right-handed world. The first build mirrored the picture and culled front faces. With no reference to compare against, only the picture showed it.
- **Painter's sorting:** with mean depth, large ground triangles cover the feet of what stands on them. A per-triangle sort bias fixes it for ground (the classic console answer). A depth buffer would cost 150 KB and a compare per pixel.
- **Tessellation** must be decided per edge, or T-junction cracks show.
- **Constants in the pixel loop cost an instruction each.** vc2cc builds any immediate that doesn't fit in r15 first, and GCC reloads it every pixel. Texture coordinates in 6.26 fixed point (so the 64-texel wrap is the 32-bit wrap) and a shift in place of a mask took the loop from 17 instructions to 14, bit-exact. Read the listing (`build.py` writes `bcmfw.lst`) before guessing at a hot loop's cost.

## Next

- ~~Run stage 3d on the device~~: done 2026-10-26, "On silicon" above.
- **Faster pixels:** the vector unit has no gather (`graphics.md` #54), but it could do the colour-table lookup and stores for 16 pixels at a time if texels were fetched by the scalar core. Worth a bench once stage 3d gives the scalar baseline.
- **Fewer pixels:** front-to-back drawing with a span buffer, or a coarse per-tile depth, to cut the overdraw that dominates scenes 1 and 4.
- **A game:** the SDK's drawing model (`graphics.md` §2) wrapped around this renderer, with Class C's 150 MHz session.
