# 18. Drawing on the chip: games, Cover Flow and the compositor

What the BCM2722 can do for 2D and 3D drawing on the iPod Video, worked out from the measured figures in the rest of this manual. The chip has no fixed-function graphics or 3D blocks. The product brief's graphics features are software on the vector unit ([`index.md`](index.md), "Scope"). Every drawing engine is therefore our own code on the core and the vector unit, fed by the host.

This chapter feeds three consumers:
- the Extras/Games SDK: P12's resource classes B and C (`docs/phase12-brief.md` §2 and §2A, on branch `p12`);
- P19's texture slots (#119): the main-menu drift, Cover Flow and the second frame (#76);
- Cover Flow (P7).

Status: the figures are silicon measurements, or estimates computed from them, and each is tagged. One engine exists: the 3D render test (`bcmfw/r3d`, device stage 3d, [`../research/bcm2722/render3d.md`](../research/bcm2722/render3d.md)), **run on silicon 2026-10-26: every check frame equals the simulator's, and 30 fps holds at 150 MHz** (§3). It is the reference engine for game developers.

## 1. The figures that bound any engine

| Quantity | Value | Evidence |
|---|---|---|
| Screen | 320×240 RGB565, one panel; the panel keeps its own picture RAM, so a rectangle can be updated alone | [silicon] every session; `lcd.md` |
| Full frame, chip → panel by the chip's DMA | **14.26 ms** at the 27 MHz LCD bus with timing words computed from Apple's nanoseconds; Apple's own `LCD_UPDATE` 15.2 ms | [silicon] S5–S7; `lcd.md` |
| LCD bus once the core is on the PLL | Unknown: whether the bus clock follows the core to 150 MHz is not recorded | open (`lcd.md`) |
| Core clock | 27 MHz from the crystal, or 150 MHz after Apple's PLL sequence; every kernel runs ×5.56 faster | [silicon] S7, S8 |
| Vector unit, one REP 16 op (16 rows × 16 lanes = 256 16-bit elements) | ALU 16.25, `vld16` 16.63, `vst16` 16.50 cycles: about **15.7 element operations a cycle** | [silicon] S7 bench |
| Scalar core | 1 dependent instruction a cycle; independent pairs about 0.53 of that | [silicon] S6, S7 |
| A 160×240 cross-fade step (the drift kernel) | ~70 k cycles: 0.47 ms at 150 MHz, 2.6 ms at 27 MHz | [estimate] instruction counts from vc2sim with the S7 timings; bit-exact in the simulator; not run on silicon (`offload.md` §8a) |
| SRAM | 1.25 MB at `0x0000_0000–0x0013_FFFF` | [apple]; [external]; [silicon] all images |
| SDRAM | 4 MB, host window `0xC000_0000`, after our bring-up | [external]; [silicon] S11 (a 64 KB grid over all 4 MB read back right) |
| SDRAM bring-up | Field 8 of the mode word fails on the first attempt at both clocks on our unit; the second attempt comes up | [silicon] S32–S34 (`open-questions.md` #7) |
| SDRAM bandwidth from the core, the vector unit and DMA | Unknown | open (#7) |
| Host → chip by the PP CPU | 27 ms of CPU for a 153.6 KB frame at 30 MHz, 9.6 ms at 80 MHz (about 10 PP cycles per 16-bit transfer) | [silicon] S8 (`data-paths.md` R1) |
| Host → chip by the PP's DMA engine 2 | 64 KB in 5.8 ms (~11 MB/s). Meanwhile the PP CPU keeps 968/1000 of its time working from IRAM, or 775/1000 touching SDRAM | [silicon] S37 (`data-paths.md` cell 14) |
| Command round trip, host → chip → host | 6–7 µs | [silicon] S2 |
| Chip → host interrupt | Not confirmed; the host polls | open (#28) |
| Does host port traffic slow the chip? | Unknown | open (#44) |
| Tearing | Our DMA pushes without waiting for the panel's tearing-effect signal. In normal UI use no tearing was visible. Apple averages the TE period at run time and races the panel's scan (`lcd.md`, "Tearing"). **The panel refreshes at 67.8 Hz** (TE period 14,740 µs, 58 µs pulse on GPIO 49). A TE-synced present (poll GPIO 49, start the DMA on the edge, every 2nd edge) gives **33.9 fps with 0 late frames** | [silicon] S7, by eye; stage 3d 2026-10-26 (TE and the synced walk); [apple] |
| Power in any drawing state | Unmeasured | open (#12); stage 5s gives the first readings |

## 2. What follows for a drawing model

The ranked plan for moving the shell's pixel work (the drift, slides, glides, marquee, text, album-art decode, Cover Flow, Photos) to the chip, with its prerequisites, is [`../research/bcm2722/display-offload-plan.md`](../research/bcm2722/display-offload-plan.md).

**The scene lives on the chip; the host sends small commands** [hypothesis, from §1]. A PP-pushed full frame costs 27 ms of CPU at 30 MHz, nearly a whole 30 fps frame. A command list costs microseconds. So a game that changes much of the screen every frame belongs on the chip. A game that changes a few bands (Class A: about 25 KB a frame) does not need it (`games.md` §6).

The model this suggests (an outline, not a design):
- **Texture slots:** pictures uploaded once at entry, by engine 2 so the PP's CPU stays free. The slots are the same idea as #119's, shared with the drift, Cover Flow and the second frame.
- **Layers:**
  - a background drawn from a row-colour table, or a tile map over a texture slot;
  - up to two scrolling tile layers;
  - a sprite list;
  - an optional affine layer (rotate and scale, for "Mode 7" floors).
- **A per-frame command list** from the PP (tens of bytes to a few KB): layer scroll offsets, sprite positions, frame numbers and flags, fills, and a "present" with a dirty rectangle.
- **Present:** the chip composes into its framebuffer and pushes the changed rectangle to the panel. For full-motion scenes it waits for the panel's tearing signal (§5).

## 3. Cost per operation [estimate]

Estimates from §1's measured rates. One frame at 30 fps has 900 k core cycles at 27 MHz and 5 M at 150 MHz.

| Operation | Cost | Notes |
|---|---|---|
| Copy a full-screen layer (opaque tiles, scroll) | ~10 k cycles (300 blocks × 2 REP ops) | vector `vld16`/`vst16` |
| 64 sprites of 16×16 with a colour key | ~5 k cycles | about 4 vector ops a block |
| Alpha blend the whole screen | ~140 k cycles: 5 ms at 27 MHz, under 1 ms at 150 MHz | twice the drift kernel |
| Affine layer, whole screen | ~770 k cycles (about 10 scalar cycles a pixel): too slow at 27 MHz, about 5 ms at 150 MHz | the vector unit has no known gather; per-pixel texture fetch is scalar (#54) |
| Present the whole screen | 14.26 ms of LCD-bus time, with the core free meanwhile | the panel bus, not compute, is the ceiling |
| Textured triangles (PS1-style: affine texturing, a painter's ordering table, palette textures shaded through a colour table) | Our renderer in vc2sim (`render3d.md`): 14 instructions a pixel (the compiled loop: two loads through the texture and the colour table, one store), plus about 0.5 k a triangle for transform, clip and set-up | **[silicon, stage 3d 2026-10-26]** 145 to 2,222 triangles a frame (43–100 k pixels) in **9.9–35.8 ms at 150 MHz** (30 fps up to about 1,350 triangles; 27.8 fps at 2,222) and 55–200 ms at 27 MHz; frames bit-exact with vc2sim. Raster 25–74 cycles a pixel with span set-up; ≈ 1.26 cycles per simulator instruction + about 590 a drawn triangle; time scales exactly with the core clock. Plan with the simulator estimate × 1.5 (`render3d.md`, "On silicon") |

**Sustainable rates:**
- **The panel bus caps the rate:** about 70 full frames a second, before any tearing-signal wait. 30 fps full-screen uses about 43 % of the bus. 60 fps would need about 86 %, and only with tearing-signal sync is it clean (§5).
- **A 2D scene** (tiles, sprites and blends) fits easily at 30 fps with the core at 27 MHz. That suits Class B, which runs beside music and should avoid the PLL's power cost.
- **Affine layers and 3D** need 150 MHz, which suits Class C, the chip held for the session.

## 4. Memory a game can have

| Region | Size | Available to a game |
|---|---|---|
| SRAM | 1.25 MB | A compositor image (tens of KB, to be sized) plus two 150 KB framebuffers and a stack leave **about 0.9 MB** for textures, tile maps and sprite sheets [estimate; layout ours]. The resident video image (206 KB) is not needed while a game runs: a game session would boot the compositor image in its place, or the two would be merged and the layout re-planned |
| SDRAM | 4 MB | **About 3.5–4 MB** for any game [estimate]. Video playback never runs beside a game or music (owner, 2026-10-23), so the video player's FIFO and frame buffers never compete. The one possible sharer is on-chip music audio, if that is ever built (the audio rings and AAC tables, about 384 KB in today's video layout), beside a Class B game. A Class C game has it all |
| Upload time | ~11 MB/s by engine 2 | 1 MB in about 90 ms, 4 MB in about 0.36 s [estimate from S37]: inside Class B's 0.5 s entry behind a snapshot |

**Everything is lost on a power cut** [silicon]. If the chip is powered down while the screen is dark, a game's textures must be uploaded again at wake, or the chip must stay powered (unknown cost, #12; stages 5i/5s measure the first numbers).

## 5. The host's share, and the latency chain

**The PP can idle while the chip draws** [estimate]: one command list a frame is microseconds of port time. With no confirmed chip-to-host interrupt (#28), the host learns that a frame is done by polling the status word at its next step (a 6–7 µs round trip). This fits the SDK's loop, where `step()` returns the time until the next step. The chip's own core busy-polls between commands today. Whether it can sleep instead is stage 5i's question (#3).

**Input to visible frame** [estimate], with the chip drawing:

| Stage | Time |
|---|---|
| Wheel or button into the 100 Hz tick | up to 10 ms |
| Wait for the next `step()` at 30 fps | 0–33 ms |
| `step()` and the command list | under 1 ms of host time for a typical game |
| Composition on the chip | 1–5 ms (2D), up to about 25 ms (3D at 150 MHz) |
| Tearing-signal wait, if used | 0 to one panel refresh period (unknown, #52) |
| Push to the panel | 14.3 ms for a full frame; rows land top to bottom |

That is typically 30–45 ms and at worst about 70 ms for a 2D game. A PP-pushed full frame adds 27 ms of CPU at 30 MHz on top.

## 6. Engines, in order of cost

1. **2D layers, sprites and blends.** The drift's ops generalised; it shares #119's texture slots. The smallest step. It needs a device bench of the few new vector forms and a run of stage 5b (the drift on silicon).
2. **Column renderers:** a raycaster or a voxel heightmap. These suit 16 lanes, and need no triangle setup.
3. **Triangles:** built as the 3D render test (`bcmfw/r3d`, `render3d.md`). It's scalar C:
   - fixed point and a look-at camera;
   - near-plane and guard-band clipping, back-face and per-chunk sphere culling;
   - edge-based tessellation of near, depth-steep triangles in place of perspective correction;
   - a painter's ordering table with a sort bias for ground;
   - affine palette texturing.

   It's Class C at 150 MHz. Its device stage 3d holds each scene to 30 fps and compares two checksummed frames per scene with the simulator's.

## 6a. Lessons from the 3D render test that carry to video and the UI

From building `bcmfw/r3d` (`research/bcm2722/render3d.md`). Each lesson reaches beyond 3D:

- **Build chip C for the Mac too, and compare checksums with vc2sim.** A match confirms the compiler and the simulator for that code. It caught nothing here only because a GCC bug had already been found another way (`research/bcm2722/compiler.md`, "Known GCC bugs": `min.l`/`max.l` lose their source). The H.264 and AAC decoders are the next candidates.
- **Read the listing of every hot loop.** vc2cc builds any immediate that doesn't fit in r15 first, and GCC reloads it on every pass. Three of the pixel loop's 17 instructions were mask constants. Shifts in place of masks (texture coordinates in 6.26 fixed point) made it 14, with identical output. The decoder's scalar loops (CAVLC, deblocking, the AAC decoder) probably pay the same tax at 27 MHz.
- **Checksummed frames on the device.** Stage 3d compares two frames per run with the simulator's checksums. A video stage can do the same with decoded pictures: a bit-exact check of the decoder on silicon, not a look.
- **Present on the panel's TE edge.** The panel refreshes on its own clock (`lcd.md`, "Tearing"). Starting the frame's DMA on the TE edge, with a write faster than the scan, avoids tearing, and presenting on every n-th edge locks the frame rate to the glass. Video at 30 fps on a 60 Hz panel is every second edge; at 25 fps it can't lock, and a frame-repeat pattern decides the judder.
- **Measure idle time per frame.** The time a frame waits for its slot is what the core could sleep through (stage 5i, #3). The video player can log it the same way.
- **Double-buffer every full-screen animation.** Render one frame while the DMA sends the other, so the panel's 14.3 ms is hidden. That's the pattern for the drift and Cover Flow (#119).
- **Pixels, not triangles, set the cost.** Overdraw made the village cost as much as 2,250 terrain triangles. A compositor or Cover Flow saves most by not painting background that a cover will hide.
- **Palette pictures through a colour table.** 8-bit texels with 32 brightness rows: half the memory of RGB565, and fades and dimming cost no arithmetic per pixel. Usable for Cover Flow's covers (16 KB at 128×128) and UI fades.

## 6b. Apple's own engine: `RenderServer.bin`

Apple shipped a second boot image, `RenderServer.bin`, for the iPod Games (2006) [apple]. Its strings name a vector-aligned allocator, a "shader machine" with uniforms and shader types, texture uploads and a front buffer. On the host, `osos` carries about 80 OpenGL ES 2.0-shaped `gl*` entry points with Apple fixed-point extensions, and names the engine an OpenGL ES 2.0 engine for the VideoCore II [apple]. So commercial 3D games ran on this chip as software, the same route as ours, behind a GL-shaped API that we don't need (the SDK's command list is smaller). Comparison and details: `brief-map-peripherals.md` §5. Its rasteriser is the reference to read if stage 3d falls short (`open-questions.md` #66).

## 7. Open questions this chapter adds

Listed in [`open-questions.md`](open-questions.md):
- **#52** the tearing signal on silicon;
- **#53** pushing to the panel straight from any source and pitch;
- **#54** the cost of per-pixel scalar texture fetches from SRAM and SDRAM.

Also relevant: #3, #7, #12, #28 and #44.
