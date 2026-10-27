# BCM memory ownership, and the merged display image: contract rev 0.1

**Names:** these documents use the technical names: PP5022 (its cores CPU and COP), BCM2722 ("the BCM"), WM8758. In chat with the owner they are PP, PP1, PP2, VC and DAC.

P19 contract, draft for the brief review (2026-10-23). The job interface is `jobs.md` (constants in `bcmfw/jobs.h`); the power states are in `power-contract.md`. Tags: [silicon] measured on the device, [ours] our design, [estimate].

## 1. Who owns what

- **The BCM image owns its memory map.** The regions below are fixed at build time, and `bcmfw/build.py` checks that none overlaps (as it does for the drift and r3d images).
- **Pools inside the map belong to slot classes** (`jobs.h` `J_CLS_*`). The BCM's allocator hands out slots on `ALLOC`. The PP never writes outside a slot it was given, or the rings and framebuffer it already owns.
- **The arenas change hands by mode** (`J_MODE`):
  - **UI:** the shell's pools.
  - **VIDEO:** a video session; the UI pools are evicted, epoch + 1.
  - **GAME:** a Class C game. Video is exclusive of games and of music (owner, 2026-10-23), so video and games never share.
- **Nothing on the BCM is the only copy.** Every slot can be rebuilt from the PP side: the art cache on disk, or the shell's renderers (`jobs.md` §4). That's what makes eviction, power cuts and failures safe.
- **The PP side's memory, across the two cores** (P19 19-9 puts the UI and the job ring on the COP; the COP-ownership audit, 2026-10-24):
  - **Every `core_alloc` happens on the CPU (PP1)**, and so does every pin and unpin: buflib has no lock. The COP UI asks a CPU service to allocate (the driver's state, `vplayer_start`'s memory, upload buffers).
  - **An upload's pixels stay pinned for the whole upload.** The engine-2 wait yields, so an unpinned buffer can move mid-upload.
  - **Cache hand-off:** the core that wrote a pixel buffer cleans it (`commit_dcache` acts on the calling core only) before engine 2 or the other core reads it, and a reader on the other core invalidates first. Buffers are 16-byte aligned (one cache line). This covers slot sources, the PP framebuffer for `bcmjob_resync` and the wake frame, and the video player's overlay.

## 2. SRAM, 1.25 MB (`0x000000–0x13FFFF`): the merged display image

One resident image serves the LCD contract, the video protocol and the jobs (`display-offload-plan.md` §5, prerequisite 2). The plain video image ended at `0x3175C` with its `.bss` when this was written, just short of its decoder arena at `0x32000`. It later grew to `0x32B7C`, **past** that arena, so every OPEN's decoder overwrote `spec_data`, `trace` and `aac_flags`: `build.py` read `config.h` with a regex that took the merged image's `#define` for every image, so its check never fired. Since 2026-10-24 every video image's arena starts at `0x35400`, and `build.py` reads `config.h` through the preprocessor [ours; `build.py` report, the link map].

| Range | Size | UI mode | VIDEO mode | GAME mode |
|---|---|---|---|---|
| `0x000000–0x0001FF` | 512 B | vectors, VP header (`0x100`), the image's words (`0x140–0x1DF`), LCD mailbox | same | same |
| `0x000200–0x0353FF` | 212 KB | **the image**: today's video image plus the job code. **The job code's budget is ~15 KB**: the arena moves from `0x32000` to `0x35400` (2026-10-23: `0x35000` until the vector kernels, which took the merged image to `0x3508c`) | same | same |
| `0x035400–0x0CBFFF` | 603 KB | **UI pools:** `J_CLS_DRIFT` 2 × 182,400 B (two 300×300 covers at a 304-pixel pitch, `0x35400–0x8E4FF`), `J_CLS_STRIP` 128 KB (`0x8E600`), `J_CLS_THUMB` 64 KB (`0xAE600`), `J_CLS_APP` 51.5 KB (`0xBE600`), 3 KB spare | **the H.264 arena**: 320×240 with 4 references needs 617,056 B and fits with 416 B to spare [ours; `build.py`'s check]. In the merged image the AAC tables are parked at `0x134000–0x13FFFF` until the first `OPEN` (not in the arena's top, where the UI pools are) | the game's |
| `0x0CC000–0x0CFFFF` | 16 KB | stack | | |
| `0x0D0000–0x0D2FFF` | 12 KB | command ring (8 KB), event ring (4 KB) | | |
| `0x0D3000–0x0D3FFF` | 4 KB | DMA control blocks | | |
| `0x0D4000–0x0DBFFF` | 32 KB | `J_CLS_SCRATCH` (the skeleton's old FIFO; the video build reads its FIFO from SDRAM) | DMA staging fallback | game |
| `0x0DC000–0x0DDFFF` | 8 KB | spare | the scaler's strip | |
| `0x0DE000–0x0DE07F` | 128 B | **the job block** (`HP_JOBS` points here; `J_BLOCK_SIZE`, 64 B until `J_CUR_OP` at `0x40`) | same | same |
| `0x0DE080–0x0DE87F` | 2 KB | **the slot table**: 64 × 32 B (reserved: rev 0.1 publishes none, `J_SLOT_TAB` 0) | same | same |
| `0x0DE880–0x0DE8FF` | 128 B | spare | | |
| `0x0DE900–0x0DE9FF` | 256 B | the warm swap's park stub (`BCMFW_PARK_STUB`), copied in only to park | same | |
| `0x0DEA00–0x0DEFFF` | 1.5 KB | **the hibernate stub** (`hibwake.h` `HW_STUB`, 1,072 B today), uploaded by the PP before a hibernate park (`power-contract.md` §6.6; sim only) | the self-test scratch reaches `0x0DF000` (developer) | |
| `0x0DF000–0x0DF3FF` | 1 KB | spare | the video guard (`0x0DF000`, 240 B) | |
| `0x0DF400–0x0DF4FF` | 256 B | **the hibernate parameter block** (`HW_PARAM`: regions in, sums and times out) | | |
| `0x0DF500–0x0DFFFF` | 2.75 KB | spare (the staged merged candidate: the self-test's 508 B snapshot at `0xDF500`, `BCMFW_JOBS_LOW_SNAP`, written only by a developer self-test `OPEN`) | | |
| `0x0E0000–0x1057FF` | 150 KB | **`J_SURF_FRONT`**: the framebuffer (Rockbox's contract) | the host frame for the overlay | the game's frame A |
| `0x105800–0x12AFFF` | 150 KB | **`J_SURF_BACK`**: the second frame (#76) | the RGB565 picture | frame B |
| `0x12B000–0x13FFFF` | 84 KB | `0x12B000–0x133FFF` spare; `0x134000–0x13FFFF` the parked AAC tables (48 KB) until the first video `OPEN` copies them to SDRAM. (The display-only image: `J_CLS_THUMB` to `0x13AFFF`, then the overlay region; below) | same | game |

**Consequences** [ours]:
- **The drift's covers fit in SRAM.** The drift needs no SDRAM, so no SDRAM refresh power while lit (`power-contract.md`).
- **The drift image's fade planes don't fit beside the slots.** Its `PREP` unpacks two pictures into 16-bit R, G and B planes: 460 KB. In the merged image, `FADE` and `AVG` work on RGB565 surfaces directly, unpacking each 16×16 block in vector registers (`jv_fade16`, `jv_avg16`; `jobs.md` §10, "The vector unit"). They are bit-exact against `jmix.h` in vc2sim [sim, 2026-10-23].
- **The drift and r3d parameter blocks at `0x106000`** sit inside the second frame. In the merged image, job parameters travel in the ring records; that block is not used.
- **The resident image's PP copy grows** with the job code, capped by the arena's start: the image and its `.bss` must end below `0x35400` (218,112 B; batch 3's `2c6dc03a` ends at `0x33c7c`, 6,020 B below). The PP's copy is the load image, which also carries the AAC tables that `crt0` parks in the arena's top, so it can be a little larger: 218,116 B on 2026-10-23, with image+bss ending at `0x3512c` (today 203–207 KB in the builds that carry it; `hardware.md`, "Resident video image"). **Release builds carry no resident image today**: only `HAVE_BCMFW` + `HAVE_BCMFW_VIDEO` builds do (`ui_main.c`). L2 adds it to release builds (review C2).

**The display-only image's own layout** (G9, with `J_OP_DECODE`): no decoder arena, so the decode's work area (236,544 B from `0x0C400` since 2026-10-25, 512 B more image room within the `J_DEC_WORK_MIN` promise of 184,428 B; 237,056 B from `0x0C200` before; 239,104 B from `0x0BA00` until 2026-10-24, when `JDEC_WORK`'s fixed part went from 192 to 190 KB and `J_DEC_WORK_MIN` from 238,640 to 236,592 B to give the image 2 KB) sits below the pools, which start at `0x46000`, and `J_CLS_THUMB` moves to `0x12B000`. Since 2026-10-24 (`jpeg-spike.md` §15) `jdec` streams its outputs, so a decode needs far less than the area: 114,172 B for the NP box, 137,240 B for the panel's 300×300 fill, 184,428 B for the 16:9 art set. `J_DEC_WORK_MIN` and the layout are unchanged until the owner decides whether the area gives bytes back. The table is in `album-render.md` §5.1; the merged image's layout above is unchanged.

**The visualizer (#126, proposed, sim only; `visualizer.md` §8):** it needs about 16 KB (vector FFT, its C, tables, 2 KB of `.bss`), which neither image's resident part has. Its home is now an **overlay at `0x13B000–0x13FFFF`**, the display-only layout's top spare (20 KB, above `J_CLS_THUMB`; `config.h` `BCMFW_JOBS_OVL`, published in `J_OVL`) [sim]:
- Checked free: in the display-only image nothing else is there. `J_SURF_BACK` ends at `0x12B000`, `J_CLS_THUMB` runs `0x12B000–0x13AFFF`, and no AAC tables are parked (they are only in the video images, at `0x134000`). The framebuffer (`0xE0000–0x1057FF`), the stack, the rings and the job block are below. `build.py` adds the region to the display-only layout's overlap check.
- The overlay: 14,560 B loaded (`0x13B000–0x13E8E0`) and 2,080 B of `.bss`, to `0x13F100`; 3,840 B of the region spare (`build.py` keeps at least 1 KB).
- The resident stub (`J_OP_OVERLAY`, `jobs_bcm.c`) costs the display-only image 288 B: image+bss ends at `0xbfe0`, 544 B below the decode work area. No pool moves.
- **The merged image's own region (since batch 3, `BCMFW_JOBS_MERGED_OVL` 1; `album-render.md` §5.3):** `0x12B000–0x133FFF` (36 KB, spare in UI and video mode, below the parked AAC tables at `0x134000`) takes DECODE's overlay (`dec-ovl.bin`, 23,884 B + 532 B `.bss`); its work area is a `J_CLS_DRIFT` slot the PP lends per decode (the drift's own cover parked first: the pool's two places are the output and the work slot, beside the resident panel only when the output is a thumbnail). The loader's 640 B come from moving AAC's `spec_data` (2 KB) to the far bss; the arena stays at `0x35400`. **`HP_OP_TONE`'s buffer (`0x12C000–0x1349CF`) overlaps the region: TONE is refused while an overlay is loaded.** Each image publishes its region in `J_OVL`, and the PP loads only the overlay whose record names that base.
- The `BCMFW_VIZ` image (the display-only layout with `SPECTRUM` and `BARS` resident in place of `DECODE`) stays as the warm-swap alternative and the overlay's cross-check.

## 3. SDRAM, 4 MB: on demand only

SDRAM is up only after our bring-up:
- The bring-up has a quirk: field 8 of the mode word fails on the first attempt and comes up on the second [silicon S32–S34, #7].
- Its refresh and self-refresh power are unmeasured (#7, #12). After the first video, today's image leaves SDRAM refreshing until the power cut (`hardware.md`).

**So the UI pools default to SRAM**, and SDRAM pools are brought up only for what doesn't fit:

| SDRAM range | UI mode (SDRAM up on demand) | VIDEO mode | GAME mode |
|---|---|---|---|
| `0x000000–0x37FFFF` (3.5 MB) | `J_CLS_COVERFLOW` while Cover Flow is open: 16 × 32 KB = 512 KB, or 32 covers in 1 MB; `J_CLS_PANEL`, a panel pool: 8 × 180 KB = 1.44 MB, so the drift can cycle covers without the disk while lit; the rest spare | FIFO, DPB, wrap copy, audio rings (as built, `video.h`) | the game's (`graphics.md` §4: about 3.5 MB of textures) |
| `0x380000–0x39FFFF` (128 KB) | untouched: the AAC decoder's far data and test I/O once loaded | same | same |
| `0x3A0000–0x3CFFFF` (192 KB) | **the staged upload's window** (candidate images only, jobs rev 0.5: `config.h` `BCMFW_JOBS_STAGE`, host `0xC03A_0000`): the PP writes one slot's rows here, `COMMIT` moves them into the slot | diagnostic dumps (the window is idle: no UI slot in VIDEO mode) | game |
| `0x3D0000–0x3FFFFF` (192 KB) | spare | diagnostic dumps | game |

Every SDRAM pool is evicted (epoch + 1) at a video `OPEN` and at a game session. Bringing SDRAM up for the UI is a P19 decision that waits on its power figure (the bench list).

**The staged upload's window** (2026-10-25, open question #67; candidate images only, `jobs.md` §4 "The staged upload"). The first staged `ALLOC` brings SDRAM up in UI mode, in both candidates (the display-only one with `video.c`'s sequence, moved verbatim to `bcmfw/video/sdram_up.h`), and it then refreshes until the power cut, as after a video today. The window is 192 KB, enough for any slot of the SRAM pools (a cover at the drift's pitch is 182,400 B); a larger slot is answered the rev 0.4 way. Why SDRAM: it is the only host-writable region with room that a host write after the start is proven in [silicon S12, S28–S34]; the rings and the host frame are proven too, but in use (the front frame holds the drift's running animation). Why not the slots themselves in SDRAM: the jobs read slots every frame (the drift's FADE reads two covers 30 times a second), and SDRAM's read rate from the vector unit is unmeasured (#7), while a staged slot costs one move per upload (a cover: 6.4 ms by DMA, 11.5 ms by the core [silicon PW8 rates]) and draws from SRAM as before. SDRAM in UI mode costs its refresh power (#7, #12, unmeasured); the candidates accept it to prove the path, and a shipped image could put SDRAM to sleep between uploads once the rewake (PW5's) is proven.

## 4. Budgets per class (rev 0.1)

| Class | Pool | Slots | Who uses it |
|---|---|---|---|
| `J_CLS_DRIFT` | SRAM arena | 2 × 300×300 at a 304-pixel pitch (182,400 B each) | the main menu's and Music's drift (#119) |
| `J_CLS_STRIP` | SRAM arena + `0x12B000` | 212 KB in all | marquee strips (a title rendered once), list row bands for the retained screen |
| `J_CLS_THUMB` | SRAM arena | 64 KB, e.g. 10 × 55×55 | later: thumbnails for BCM-composed rows |
| `J_CLS_SCRATCH` | SRAM `0xD4000` | 32 KB | one job's temporary surface |
| `J_CLS_PANEL` | SDRAM | 8 × 300×300 | the drift's cover pool (#100's `panelpool` on the BCM) |
| `J_CLS_COVERFLOW` | SDRAM | 16–32 × 128×128 | Cover Flow (P7) |
| `J_CLS_GAME` | the arena and SDRAM | all | a Class C game (`J_MODE_GAME`) |
| `J_CLS_APP` | SRAM arena (its spare) | 51.5 KB, 8 slots (56 KB until the drift's pitch) | a Class B module beside music (P12 §2.1; gap G7) |

The budgets are constants in `bcmfw/jobs.h` (`J_BUDGET_*`, `J_SLOTS_*`; gap G8), so the shell can `static_assert` its requests. In UI mode, the arena (`0x35400–0xCBFFF`, 617,472 B) holds DRIFT (364,800 B), STRIP's arena part (131,072 B), THUMB (65,536 B) and APP (52,736 B): 614,144 B, leaving 3,328 B spare (`build.py` checks the layout against the budgets).

**The drift's pitch** (2026-10-24): a `J_CLS_DRIFT` slot's rows are the width rounded up to 16 pixels apart (`J_SLOT_PITCH`; 304 for a cover, `J_DRIFT_PITCH`), so they start 32-byte aligned and a window in phase with the frame (x mod 16 equal) reaches the vector unit: the drift's FADE 47.3 → 3.6 ms and its pan BLIT 2.4 → 0.5 ms at 27 MHz (`jobs.md` §10, the cost audit). It cost 4,352 B. In the display-only image the drift's pool can't move down (the decode work area is 464 B over its minimum), and neither image has another place for a 56 KB pool, so `J_CLS_APP` gave up the 4,608 B (56 → 51.5 KB). The alternatives were a smaller `J_CLS_STRIP`, or the pitch for 300-wide slots only; APP has no user yet.

## 5. What leaves the PP's audio buffer

All of these are reserved from the audio buffer today (`memory-audit-2026-10-20.md`). They count against the floors (≥ 7.5 MB on 32 MB, ≥ 39 MB on 64 MB; P16's measured minimums are 11.61 MB and 44.44 MB).

**The PP keeps its cover memory** (owner, 2026-10-23, P19 Q13, "smooth on every unit"). `pv.slot` and `slot2` stay on every unit, and #92's three-deep bank stays on 64 MB units.
- **What it means:** the drift's offload saves CPU time and power, **not RAM**.
- **On a BCM failure:** the still cover comes from the PP's slot, never from `panels.pak`. Cover Flow is disabled with a small "not available at this time" screen; the rest takes its CPU path (`jobs.md` §7).
- **Uploads after a cut** go straight from the PP slot or the bank entry (RGB565, 300×300, 600 B pitch, contiguous) into the `J_CLS_DRIFT` slot, row by row at the slot's 608 B pitch (`ALLOC`'s reply). No staging buffer is needed.

**Against the release build** (`fw-ui`, review C2), which today has no BCM image in RAM:

| Item | Release today | With L2 and the jobs | Change | Notes |
|---|---|---|---|---|
| Drift cover slots and #92's bank | 2 × 180,000 B, plus the bank on 64 MB | **unchanged** (Q13) | 0 | the offload saves CPU and power, not RAM |
| Resident BCM image, merged (video decoder plus jobs) | 0 | **218,116 B** [measured, `build.py`, 2026-10-23, with the park stub] | **+218 KB** | needed to restart the BCM without a disk read (the boot ladder, P18's wake) |
| *or* the display-only image (G9), with the video image loaded at a video start by a warm swap | 0 | **40,800 B** with `J_OP_DECODE` [measured, 2026-10-23; 21,764 B without] | **+40 KB** | the warm swap is bench row B13 (`warm-swap.md`); its own SRAM layout and the decode work area: `album-render.md` §5.1 |
| Strip (`anim.h`) | 180,000 B | unchanged (Q12) | 0 | |
| PP framebuffer | 153,600 B | unchanged (G10) | 0 | |
| Cover Flow textures (P7, planned) | would add 512 KB | 0: `J_CLS_COVERFLOW` on the BCM | **512 KB never reserved** | plan item 5 |
| Row-art LRU, art builder buffers | as today | unchanged in rev 0.1 | 0 | later with `J_CLS_THUMB` and `J_OP_DECODE` |

**Net against release.** The P19 brief rev 1c's figures (Q13; corrected 2026-10-23, p19-brief `f517e1d9b9`):

| Unit | With the resident video image | With the display-only image |
|---|---|---|
| every unit (32 MB and 64 MB) | about **−220 KB** | about **−47 KB** (−28 KB before the display-only image took `J_OP_DECODE`) |

- **The same on every unit.** Under the owner's default, `pv.slot`, `slot2` and #92's three-deep bank stay on every unit, so the drift's offload frees nothing anywhere. An earlier "64 MB −40 / +152 KB" assumed `slot2` was released, which contradicts that default.
- **What the totals are:** the resident image (218,116 B measured; its image+bss capped by the arena at `0x35400`) plus about 3 KB of driver state; or the display-only image (19,472 B then; 40,800 B since it carries `J_OP_DECODE`, so about 19 KB less saved: `album-render.md` §5) plus that state and the brief's other small items (P19 brief §2A.1).
- On the BCM side alone, L2 costs the image (+218 KB, or +40 KB display-only with `J_OP_DECODE`), and the drift costs nothing in RAM under Q13.
- Cover Flow's 512 KB is never taken, either way.
- **RAM is formally not a P19 goal** (brief rev 1c). G9's display-only image with the warm swap only makes L2's cost smaller (−47 KB with the decode job, instead of −220 KB); bench row B13 decides it.

## 6. L2: our image as the default, and `verified.txt`

**How the default path decides today** (`bcmfw-boot.c`):
- `HAVE_BCMFW_DEFAULT` boots the embedded DMA image (`hello_dma`) without a pass flag only if its FNV-1a is in `bcmfw/verified.txt`, which is compiled in as `bcmdt_verified_fnv[]`. **The file has no entries yet.**
- The resident rung reloads the video image file (`bcmvideo.bin`, checked against `bcmvideo_image.h`'s size and FNV) at every wake. It needs no verified entry; it rides on the boot path being on.

**The merged image exists** (2026-10-23):
- 218,116 B, with image and `.bss` to `0x3512c`, inside the arena from `0x35400` (724 B spare).
- About 10 KB of job code, in budget.
- Every job vector-accelerated and bit-exact in vc2sim (`jobs.md` §10).

**L2's simplest shape** [ours]: **the merged image becomes `bcmvideo.bin`.** It's the video image plus the jobs, so one file serves video, the resident rung and the display jobs. `mkvideo.py` builds it with `BCMFW_JOBS=1` and the shipped flags. The display-only image (G9) replaces it as the resident one only if B13 passes.

**Built (p19-session-img, 2026-10-24)** [ours]: two files, chosen by role, because the probe ops (PEEK to HIB) exist only in the plain image and the merged one has no room for them:
- `mkvideo.py` builds both: `bcmfw/video/bcmvideo.bin` (plain: the shipped flags; 220,900 B, FNV `b51bab40`) and `bcmfw/video/bcmvideo-jobs.bin` (merged: the same plus `BCMFW_JOBS=1`; 218,660 B, FNV `cd8e5d96` since rev 0.4), with `apps/video/bcmprobe_image.h` and `bcmvideo_image.h`. `--check` covers all four files.
- `/.rockbox/bcmvideo.bin` is the resident image and the player's (playback, the resident rung, stages 6 to 9, the DMA lab, B6, B7, B13). It is the merged image only in a build configured with `--bcmfw-merged` (`HAVE_BCMFW_MERGED_VIDEO`): the session's bench build, and since p19-release-noplayer the release line, which has no player. Without the flag it stays the plain image, so nothing changes in any other build until the merged image is verified. (Until `p19-bcm-release` no release build carried a video image at all: `--bcmfw` needed `--bcmfw-devtest`. See "Release and developer BCM builds" below.)
- `/.rockbox/bcmprobe.bin` is always the plain image: every `vplayer_probe()` row (PB-1 to PB-4, PB-7 to PB-9, PW1 to PW8, C1) and the E2 lab boot it.
- The resident rung refuses nothing for being unverified (developer builds): it needs the setting, the offer (`bcmres_offer()`, size and FNV as the build's header) and no failure at an earlier boot. `bcmfw_res_verified()` (on `p19`) only shows in the status row and opens the job gate; an unverified merged image needs the developer marker `bcmjobs.on` for the jobs to run.
- **When the entry lands:** make the merged image the default (`--bcmfw-merged`'s choice without the flag, then retire the flag), rerun `mkvideo.py --check` and `mkdevtest.py`. The run sheet is at the top of `bench-p19.md`; the device side is the Developer row "Bench session" (`apps/ipodui/devbench.c`).

**What the first `verified.txt` entry needs, in order:**

1. **vc2sim gates on the merged image** (after 22:00; heavy). **Done 2026-10-23:** `video_test.py -D BCMFW_JOBS=1` passed 16 rows, 249 cases, 0 failed, the same as the plain video image (the arena move holds every clip):
   - `video_test.py` with `BCMFW_JOBS=1`: every clip bit-exact. The arena moved 1 KB, so the 4-reference streams are the ones to watch.
   - `jobs_image_test.py`, `lab_test.py`, `hostproto_test.py` and `forms_test.py`.
   - `mkvideo.py --check` and `mkdevtest.py --check`.
2. **The attended stages 0–5** on the bench build (`p19-bench-splus`). They prove the LCD contract, DMA and the upload paths with the embedded images. Their pass flag is what every later row needs (card B0).
3. **The merged image on the device:**
   - stage 6 or 7 plays a clip with it as `bcmvideo.bin` (the decoder after the arena move);
   - B13 (the warm swap) and B5 (pushes at any pitch);
   - then B3 on the 19-2 build (the real shell's drift through the jobs, goldens equal on both backends).
4. **The entry:** one line in `verified.txt`, `<name> <fnv> <session, log line>`, for the image that passed:
   - the DMA image (`hello_dma`, the `HAVE_BCMFW_DEFAULT` rung), once stage 5 passes;
   - and, for L2's job path, the merged image's FNV.

   The resident image is checked against `verified.txt` by `bcmfw_res_verified()` in `bcmfw-boot.c` (P19 W0-5/6, branch `p19-w0-56`), as `bcmfw_verified()` checks the embedded images.
5. **Rebuild:** `mkdevtest.py` compiles the list in.

**The catch:** any change to the image changes its FNV, so the default path refuses it until it's listed again. An image that never ran on a device can't become the default.

### Release and developer BCM builds (branch `p19-bcm-release`, 2026-10-25) [ours]

**Two shapes of the same boot path.** `--bcmfw` no longer needs `--bcmfw-devtest`:
- **Developer:** `--ipodui --bcmfw-devtest --bcmfw --bcmfw-video --bcmfw-l1 --bcmfw-merged` (+ modules). The stages, labs, probes, bench rows and all 13 embedded images, as before; byte for byte the same build.
- **Release (since p19-release-noplayer):** `--ipodui --bcmfw --bcmfw-default --bcmfw-l1 --bcmfw-merged` (+ modules, or `--ipodui-release` for the sealed build). It holds only the host-port layer, the take and restore, the boot path (`bcmfw-boot.c`: toggle, attempt marker, strikes, ladder, run-time fallback, `verified.txt`), the resident image's read (`apps/video/bcmres.c`), the job driver (`bcmjob-dev.c`, `bcmjob-core.c`) and the CPU path (`bcmjob-ref.c`, `bcmjob-cpu.c`), and one embedded image, `hello_dma` (8,016 B, the one `verified.txt` lists). No video player, no player's hold (`bcm_video_*`), no Videos shell. Adding `--bcmfw-video` gives the line before (the player, Videos hidden); adding `--bcmfw-devtest` as well gives the developer shape back.
- **Why `--bcmfw-merged` alone, not a new option** [ours]: the jobs need exactly the merged image resident, which is what that flag already names; a separate `--bcmfw-jobs` would have meant two flags for one image and a meaningless `--bcmfw-video --bcmfw-merged` vs `--bcmfw-video --bcmfw-jobs` pair. So `--bcmfw-merged` no longer needs `--bcmfw-video` (device: needs `--bcmfw`; simulator: `--ipodui`), and `ipodvideo.h` keys `HAVE_BCM_JOBS` (device: plus `HAVE_BCMFW_RESIDENT`) or `HAVE_BCM_JOBS_MODEL` (simulator) on `HAVE_BCMFW_VIDEO || HAVE_BCMFW_MERGED_VIDEO`. A build with `--bcmfw-video` defines the same macros as before plus `HAVE_BCMFW_RESIDENT` on the device, which now guards the resident code that `HAVE_BCMFW_VIDEO` guarded.
- `--bcmfw-default` is required without the device test: the toggle and the pass flags are the debug menu's, so nothing else could turn the path on. `--bcmfw-l1` is in the release shape because it is what makes the path safe to ship: every host-port wait bounded (without it `bcm_init()`'s bootstrap waits are Apple's unbounded loops), the early swap and the ladder (`independence.md`: "It needs the swap moved earlier and made a release feature"; item 5 makes `--bcmfw-default` the release setting once `verified.txt` lists the image). Every session build since #62 carried it.

**What a release boot does** (no `bcmfw.cfg`, so the build's default, DMA):
1. Apple's ROM loads Apple's vmcs from NOR; `lcd_init` and the first frames run on it, as on a stock unit.
2. Right after the disk mounts (`main.c`, level 1): `bcmres_offer()` (`apps/video/bcmres.c`, owned by the boot path; it was `vplayer_resident_offer()` until p19-release-noplayer, and the player, when built, plays from the same block) reads `/.rockbox/bcmvideo.bin` (the merged image) in one bulk read into a pinned `core_alloc` block, checked against `bcmvideo_image.h`'s size and FNV (the resident setting has no file: on). Then `bcm_boot_early()` → `bcm_boot_switch()`. At the first boot of a new build (its `rbversion` not the one in `/.rockbox/bcmfw.build`) the build ID is written, then the attempt marker and the strikes are cleared, once (`bcmdt_rearm_due()`, host-tested in `devtest_test.py` `t_release_gates`); a build whose swap brings the boot down never clears them again, and if the ID can't be written nothing is cleared. Then: no swap with the attempt marker present or 3 strikes; `hello_dma` allowed only if `verified.txt` lists it (it does: session 17); the marker is written, then the ladder: **the resident merged image (only if its FNV-1a is in `verified.txt`: `bcmdt_res_gate()`), then `hello_dma`, then Apple's vmcs**. No CPU-push rung and no pass flags in a release build (they name images it doesn't carry). The developer build keeps today's rules (an unverified resident image is tried; the debug menu re-arms).
3. The winner drives the display from then on; every wake reloads it (`flash_vmcs_*`). A fault or a 2 s hang drops one rung (a strike); 30 clean minutes clear the strikes.
4. **Jobs:** `bcmjob_init()` allows them only when the resident image is running **and** its FNV is in `verified.txt`; a release build ignores the developer marker `bcmjobs.on` (a developer build still honours it). The merged image's FNV (`cd8e5d96`) is **not listed yet**, so today a release build doesn't run it at all: `hello_dma` drives the display and the drift is drawn on the CPU. One `verified.txt` line (step 4 above), after a `mkdevtest.py` rebuild, makes it the resident image and turns the jobs on.
5. (Builds with `--bcmfw-video` only.) The video player's 150 MHz session needs the device test's 4b and 4c pass flags; a release build with the player plays at 27 MHz (logged `clock: 27 MHz (no device test in this build)`). 150 MHz in a release needs its own `verified.txt`-style entry later.
6. **Videos is not compiled** in the release line (p19-release-noplayer): no player, no Videos screens or menu item, and the shell's video rows (About's count, the podcast video episodes, the library's Movies and TV) are the plain `--ipodui` build's (`HAVE_BCMFW_VIDEO` off). With `--bcmfw-video` and no device test the Videos bit is linked but not offered (`apps/ipodui/menucfg.h`), as Photos; the developer build and the video simulator offer it.

**Gate:** `scripts/validate.sh` `co_rel` (full gate) builds the release line (no player) + modules (`build/fw-rel-bcm-mod`, `_end` ≤ 1,500,000 B) and the sealed one (`build/fw-rel-bcm`, checked for "developer" strings with `fw-rel`).

**Sizes** (`docs/hardware.md`, "BCM builds: release and developer"): release + modules `_end` **1,330,944 B**, under the 1.5 MB cap by 169,056 B (70,704 B less than with the player, 1,401,648); the developer build 1,808,128 B (p19-release-noplayer, off `a8b468666e`).

**Simulator mirror:** `--ipodui --bcmfw-merged` builds the job model (`HAVE_BCM_JOBS_MODEL`, the 218,112 B reservation) without the player; `ipodui-p19-bcm-model` and `ipodui-p19-drift-still` pass on it as on `sim-ui-video` (p19-release-noplayer). The gate keeps mapping `ipodui-p19-*` to `sim-ui-video`.

## 7. Open items

- The merged image's device checks: §6's steps 2–4.
- SHADE's vector-B multiply (`vmul D16 A16 B16`) is shown only by vc2sim and Apple's corpus; B3 checks it.
- SDRAM in UI mode waits for its power figure.
- Whether the panel pool is worth SDRAM's refresh while lit is a measurement, not a guess.
