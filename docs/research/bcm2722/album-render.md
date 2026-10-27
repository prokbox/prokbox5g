# The current album's art on the BCM at a track change (design rev 0.1; S3b built)

**Names:** these documents use the technical names: PP5022 (its cores CPU and COP), BCM2722 ("the BCM"), WM8758. In chat with the owner they are PP, PP1, PP2, VC and DAC.

**Tags:** [silicon] measured on the device, [sim] vc2sim only, [ours] our design, [estimate], [hypothesis].

**Companions:** `jobs.md` (the job interface, §10 the PP-side API), `memory-contract.md` (what lives where), `power-contract.md` (states C0–C5), `jpeg-spike.md` §7–8 (the decoder on the BCM, `jdec`), `warm-swap.md` (G9's display-only image), and the placement plan (`forge/p19-placement:docs/design/p19-placement.md`, row B14, step 10). §12 says where this design differs from the plan.

**Status (2026-10-23):** the design is a proposal for the P19 brief review. Its BCM half, S3b (`J_OP_DECODE` over an input ring, in the display-only image), is built and passes in vc2sim (§13). The shell's contract is in `bcmjob.h` and the host model (§11, 2026-10-24); the shell's wiring (§14) and the device driver's half (§15) belong to another session.

## 1. The goal and the answer in short

**The owner's goal:** when a song starts, its album's art is rendered quickly on the BCM, so the Now Playing preview (the main menu's Now Playing row, `preview.c` `ui_preview_nowplaying`) is never the stretched 55 px thumbnail. Today that row shows, in order:
1. the album's panel-set image (300×300, `ARTCACHE_PANEL`), if the album is in a panel set;
2. else the 55×55 list thumbnail scaled up bilinear to 240×240 (`np_art_draw()`): **the stretched thumbnail**;
3. else the plain panel.

The Now Playing screen itself shows its composed bundle (`NP_COMP_W` × `NP_COMP_H`, 137×187, from the 142 px box) or the placeholder until the art builder has built it. On the CPU, a large cover takes 26–46 s to build [silicon, placement B14].

**The answer:**

| Question | Answer |
|---|---|
| Trigger | the next track known while lit and storage is awake (render ahead), else the track change; debounced; one render in flight (§2) |
| Source | the art builder's order (embedded first by default); no decode when the art cache already holds the output (§3) |
| Call | `J_OP_DECODE` with the file streamed through a 32 KB input ring; outputs at the art builder's own sizes (§4) |
| Where | the display-only image (G9) resident; **the merged image as an overlay** (§5.3, in the merged image since batch 3) |
| Slots | the NP box in `J_CLS_THUMB` (read back, freed); the panel in `J_CLS_DRIFT`, **resident for the epoch** beside the drift's cover (§6, §13.8 as built); keyed by album and output |
| Power | lit only; dark does nothing (§7) |
| Time | 8–24 M instructions for typical covers [sim]: 0.3–0.9 s at 27 MHz, 55–160 ms at 150 MHz [estimate]; the decode yields to the LCD pushes, the animation and the heartbeat as it runs (S3c, §8, built) |
| Fallback | the CPU path, unchanged (§9) |
| Units | the same on 32 and 64 MB, flash and HDD (§10) |
| Shell | `bcmjob_decode()`, `bcmjob_read()` and `bcmjob_dec_verdict()` in `bcmjob.h` (§11, built in the host model); the recipe for the seams (§14); the driver's steps (§15) |

**What rev 1 can do now, and what waits:**
- **Now** (`jdec` with streamed outputs, built 2026-10-24, `jpeg-spike.md` §15): the Now Playing box (fit into 142×142) **and the preview's panel window** (the 300×300 fill, and fills up to about 980×300), alone or together in one job. `jdec` writes each window row straight into its slot, so its work is the decoder's state, its rows and a resizer per output: 114,172 B for the box, 137,240 B for the panel, 148,114 B for the square art set, inside the 237,056 B work area with no layout change (§5).
- **Before that** (rev 2), the work area held the largest output whole (`JDEC_WORK(dw, dh)` = `dw × dh × 2` plus 190 KB): the box fitted and the panel's 378,160 B did not, so the PP refused it and the CPU path decoded it.

## 2. The trigger

**Render ahead, then at the change** [ours]:
1. **Ahead:** when playback knows the next track (its id3 is loaded at a buffer fill) and the screen is lit and storage is awake (the fill has it spinning), the art thread renders the next track's album if the art cache lacks its outputs. This is npfix's read-ahead (`artcache_np_request()` at a fill's end) extended to a render. At the change, the art is already there: no wait on either storage type (§10).
2. **At the change:** if the new track's album still lacks its outputs (no fill since, the first track, a jump), the art thread renders it once the screen is lit and storage is awake. It never powers storage up for this; it waits for the next natural wake, as `np_may_read()` does today.

**One render in flight, debounced:**
- A render starts only after the track has been current for **400 ms** (`ALBUM_RENDER_SETTLE`, a named constant). A user skipping through tracks at 2–4 a second starts nothing.
- A newer track supersedes a queued render. A render already on the BCM is **aborted** while it still streams (the PP writes `J_DEC_ABORT`; the BCM fails it at its next read), or runs to its end if its file is already in the ring (≤ 32 KB: at most one small decode, ~0.3 s at 27 MHz [estimate]).
- **The same album** costs nothing: the art cache already holds the outputs (§3), or the BCM's slot answers `ALLOC` with `J_ALLOC_HIT` (§6).

**What fast changes cost:** at most one decode that was in flight when the user moved on, plus its streamed bytes. Nothing is read back, composed or written for a superseded track.

## 3. The source

**The art builder's order, unchanged** (`artcache.c` `source_decode()`):
- With the default setting (`album art: prefer embedded`, `settings_list.c`), the track's embedded picture first (`try_embedded()`: the blob at `id3.albumart.pos`, `size` bytes), then the folder image (`find_file()`, `try_file()`). With `prefer image file`, the other way round. A video's own `covr` first in either case.
- **A JPEG goes to the BCM**; BMP and PNG never do (the art builder takes BMP on the CPU; PNG is terminal today).
- **`J_E_DEC_UNSUPPORTED`** (progressive, arithmetic, 12-bit, odd subsampling) sends that source to the CPU twin (`bcmjob_dec_verdict()`: `BCMJOB_DEC_TWIN`): the display-only image has no progressive store, so a progressive cover may still decode on the PP, and the twin's own answer (the decoder's −4) is the terminal one (`jobs.h`: "the PP's twin decides"; corrected 2026-10-24, this line said terminal). **`J_E_DEC_CORRUPT`** and **`J_E_DEC_TRUNCATED`** mean "try the next source", exactly as on the CPU, unless the PP's own feeding stalled (`res.stalled`: then the twin). `jdec` is one definition for both sides, so the BCM and the CPU fail the same files the same way (`jpeg-spike.md` §8).

**No decode when the cache has it:**
- **The Now Playing screen:** `artcache_np_take()` or `artcache_get(…, ARTCACHE_NOWPLAYING, …)` answers `OK` with a composed bundle: shown, nothing decoded.
- **The preview:** `np_cover_read()` finds the album in a panel set: `panels.pak`'s 300×300 image is read, nothing decoded.
- **Only a miss renders.** After a render the shell hands the outputs to the art cache (§11), so the album never decodes twice.

## 4. The `DECODE` call

### 4.1 The input ring (S3b, built)

The PP streams the file through a ring in BCM memory, so a 2.6 MB cover needs no 2.6 MB slot (`jpeg-spike.md` §3, step 1).

- **The ring** is a `J_CLS_SCRATCH` slot, 128 × 128 "pixels" = 32,768 B, allocated once per epoch and committed with sum 0. Its bytes must be a power of two from 4 to 128 KB.
- **The source word** of `J_OP_DECODE` is the ring's slot `| J_DEC_RING` (0x100); `len` is the file's length, which may exceed the ring.
- **Two job-block words** carry the indices, `HP_ENC`'d like every shared index, each `J_DEC_IDX(pass, units)` = pass << 12 | 64-byte units mod 4096:

| Word | Writer | Meaning |
|---|---|---|
| `J_DEC_IN_RD` (0x38) | BCM | the pass it reads and the units it has consumed. **A new pass** means the BCM went back to byte 0: the PP starts the file again at ring byte 0 |
| `J_DEC_IN_WR` (0x3C) | PP | the pass it writes, `(pass, 0)` first; the units written (the last one partial), never more than a ring ahead of `J_DEC_IN_RD`; or `J_DEC_ABORT` (0xFFFF) when it can't read the file |

- **Passes:** `jdec` reads the file from byte 0 once for the size probe and once per output (rev 1, `jdec.h`). A file of at most the ring's bytes streams **once** and later passes read it again in place; a longer one streams once per pass. Passes count 0–14; the BCM sets `J_DEC_IN_WR` to `J_DEC_IDLE` (pass 15) at a job's start, and the PP writes it only after it sees a new pass, so a stale abort or an old pass can't leak into the next job.
- **Stall and abort:** no new bytes for `J_DEC_STALL_MS` (1.5 s), or an abort, is a read error: the job fails `J_E_DEC_CORRUPT`. The PP knows which it was (it aborted, or its read failed).
- **The PP's side:** engine 2 writes pieces of up to 8 KB (whole 64-byte units, the file's last piece partial) under the port lock, then advances `J_DEC_IN_WR`. At ~11 MB/s [silicon S37], a 33 KB file is 3 ms of engine time per pass; a 2 MB cover 0.18 s per pass, well under the decode's own time.
- **Why the PP re-streams rather than the BCM keeping the file:** a 2 MB cover doesn't fit SRAM beside the work area. The decode-once inside cuts the passes to two (probe, decode), and the probe could be served from the ring's first 32 KB; that's for `jdec`'s owner.

**How jref sees it:** jref bounds `len` by the source slot's bytes. The display-only image's glue (`jobs_bcm.c`, `decode_prepare()`) keeps the file's length, hands jref the slot with `len` clipped to the ring, and its decode hook reads the file's length. A ring that isn't a ready scratch slot of a legal size fails `J_E_SLOT`. jref and `jdec.h` are unchanged.

### 4.2 Outputs and sizes: the art builder's rules

`DECODE` takes the art builder's sizes, computed by the shell, never by the BCM (`jdec.h`):

| Output | Box | Rule (`artcache.c`) | `dw × dh` | Window kept | Destination |
|---|---|---|---|---|---|
| **NP box** | 142×142 (`ARTCACHE_NOWPLAYING`) | **fit** (`w->fit`, `artcache_fit_rect()`): the long side fills the box, aspect kept | e.g. 142×142, 142×80 (16:9), 95×142 (2:3) | all of it | a `J_CLS_THUMB` slot, `dw × dh` |
| **Panel window** (the preview) | 300×300 (`ARTCACHE_PANEL`) | **fill** (`decode_run()` step 2): the short side fills, at most `FILL_MAX` (4) × the box; portrait art narrower than 9/10 fills `panel_cw()` columns (`ps_decode()`'s portrait rule) | 300×300 for square art; up to 1200×300 | the panel's centre 300×300 (`(dw − 300)/2`, `(dh − 300)/2`), or for the preview alone the 160×240 the panel shows | a `J_CLS_DRIFT` slot (300×300) or a `J_CLS_STRIP` slot (160×240) |

- **Both in one job** when both are missing: `n = 2`. Rev 1 decodes once per output; the decode-once inside makes the second nearly free.
- **The shell composes the NP bundle** (`np_compose`, the slant and the reflection, ~281 k instructions on the PP [placement F10]) from the read-back box, as the art builder does after its own decode. The BCM doesn't compose (no job for it), and the composed card is the art cache's to store.
- **The thumbnail** (55×55) isn't rendered here: it exists for any album the library lists, and the preview no longer falls back to it once the panel window exists.

### 4.3 The key

Slots are named by a 64-bit content key (`J_OP_ALLOC`). For a render [ours]:
- **key = the album's art key** (`artcache_key_for_id3()`, the strings the album lists group by) **XOR a per-output tag** in its top byte: `0xA1` the NP box, `0xA2` the panel, `0xA3` the preview window. So one album's outputs never collide, and two albums never share a slot.
- **The source's signature isn't in the key.** Slots live one epoch (a lit spell at most, §6), and the art cache's own index notices a changed folder image (`ARTREC_T_SIG2`) before the next render.

## 5. Where `DECODE` lives: the budget

**The cost:** `jdec` with the shell's decoder and resizer is ~15.4 KB of code (vc2cc, conservative forms); the image grows by 19,036 B (load 21,764 → 40,800 B) with the ring reader, the glue and the decoder's tables [measured, `build.py`, 2026-10-23]. The work a decode needs is `jdec_work()` (`jdec.h`, `jpeg-spike.md` §15): `92,160 + 96·dw + 768` B for the decoder's state and rows at the largest output's width `dw`, plus `50·dw + 512` B for each output's resizer: 114,172 B for 142×142, 137,240 B for 300×300, 148,114 B for the square art set. Until 2026-10-24 the largest output sat whole in the work area, `JDEC_WORK(dw, dh)` = `dw·dh·2 + 12·dw + 194,560` B: 236,592 B for 142×142, 378,160 B for 300×300 (with a 192 KB fixed part before §14, 238,640 and 380,208).

| Option | Code room | Work room | Cost | Verdict |
|---|---|---|---|---|
| **A. The display-only image** (G9: `BCMFW_HOSTPROTO` + `BCMFW_JOBS`) | its image is 21 KB; the old video arena's place is free | 237,056 B between the image and the pools (239,104 until 2026-10-24), after moving `J_CLS_THUMB` to the top spare | +19 KB of resident PP RAM (40,800 B in all, against 218,116 B for the merged image) | **chosen; built** |
| B. A separate decode image by the warm swap | free | free | a swap each way (15–40 ms [estimate], B13 unproven); while swapped no display jobs run unless the decode image carries them, which makes it option A again | no |
| C. SDRAM-resident work (and code) | code from SDRAM is untried [hypothesis] | 4 MB | SDRAM bring-up in UI mode (its first-attempt quirk [silicon S32–S34]), refresh until the cut with an unmeasured current (#7), the cache off for the session (`video.c`), slower loads | the fallback if the streaming inside slips (§5.2), after its power figure |
| D. Shrink the merged image | 724 B spare below the arena at `0x35400`; the H.264 arena has 416 B of slack | none | moving the arena 16 KB loses 320×240 with 4 references (617,056 B) | no |

**Recommendation: A.** `DECODE` exists only in the display-only image; the merged image keeps `J_OPS` bit 16 clear and the shell's CPU path. That ties the album render to G9 becoming the resident image, which waits on bench row B13 (the warm swap on silicon). Until then the render is the CPU path's, and nothing here costs the merged image a byte.

### 5.1 The display-only image's layout (built)

The pools' addresses are each image's own (the PP learns them from `ALLOC`), so the display-only image lays its SRAM out for the decode (`config.h` `BCMFW_JOBS_DO_*`; `build.py` checks it):

| Range | Size | What |
|---|---|---|
| `0x000000–0x00C1FF` | 49,664 B | the image and its `.bss` (up to; `build.py` reports how far below the work area it ends) |
| `0x00C400–0x045FFF` | 236,544 B | **the decode work area** (from `0x00C200`, 237,056 B, until 2026-10-25: 512 B more for the image, within the promise) (`J_DEC_WORK_MIN` = 184,428 B since 2026-10-24: the 16:9 art set, the owner's choice, wide art and room for video later; 236,592 B before, rev 2's `JDEC_WORK(142, 142)`, plus 464 B; from `0x00BA00` and 238,640 B until 2026-10-24). Since the streamed outputs (`jpeg-spike.md` §15) the NP box needs 114,172 B and the 16:9 art set 184,428 B; whether the minimum drops and the area gives bytes back is the owner's call |
| `0x046000–0x09F0FF` | 364,800 B | `J_CLS_DRIFT` (two covers at the 304-pixel pitch, `jobs.h` `J_SLOT_PITCH`) |
| `0x09F200–0x0BF1FF` | 131,072 B | `J_CLS_STRIP` (its arena part) |
| `0x0BF200–0x0CBFFF` | 52,736 B | `J_CLS_APP` (`J_BUDGET_APP`, 51.5 KB since the drift's pitch) |
| `0x0CC000–0x0DFFFF` | | stack, rings, DMA blocks, `J_CLS_SCRATCH` (the input ring), the job block, the park stub: as before |
| `0x0E0000–0x12AFFF` | | `J_SURF_FRONT`, `J_SURF_BACK`: as before |
| `0x12B000–0x13AFFF` | 65,536 B | `J_CLS_THUMB` (no AAC tables are parked here in this image) |

**The merged image** keeps `memory-contract.md` §2's layout, its pools after the drift's moved up by `0x600` (2026-10-24): 218,020 B, image and `.bss` to `0x350ec` [measured].

### 5.3 DECODE in the merged image, as an overlay (built 2026-10-26, sim; in the merged image since batch 3, `2c6dc03a`)

**Why:** the verified merged image (`80bc8611`) is the resident display driver, and §5's recommendation A tied the album render to the display-only image becoming resident (B13, the warm swap). The merged image lacks DECODE's code (no room: 68 B left) and any fixed work area in UI mode (its arena holds the UI pools). On the device today, therefore, `bcmjob_have(J_OP_DECODE)` is false, `art_bcm_ready()` is false and np-sharp's panel decodes on the CPU (`ps_decode()`), is written to `npcover.pak`, and the preview reads it back from the file (`artcache_panel_window()`); the BCM's only part is the drift's upload of the panel-set cover.

**The forward option, chosen:** DECODE loads into the merged image as an **overlay** (`J_OP_OVERLAY`, the visualizer's mechanism):

| Piece | Where | Cost [measured, vc2cc] |
|---|---|---|
| The loader (`jref_ext`, `JREF_EXT`) and the decode's S3c service (`bcmfw_service`, `hp_heartbeat`) | resident, merged image | 640 B of image; the image had 68 B. One AAC frame's spectral data (`spec_data`, 2 KB, video sessions only) moves to the far bss (`build.py` `AAC_FAR_MERGED_OVL`): image+bss to `0x34e3c`, **1,476 B below the arena, which doesn't move** (H.264 keeps 320×240 with 4 references) |
| The overlay (`video/dec_ovl.c`: jdec, the decoder, the ring reader, the vector colour stage) | `0x12B000–0x133FFF` (36 KB, spare in UI and video mode; the parked AAC tables start at `0x134000`) | 23,820 B loaded + 532 B `.bss`; 12,512 B of the region spare |
| The work area | **a slot the PP lends**: a `J_CLS_DRIFT` slot keyed `J_DEC_WORK_KEY` (300×300, 182,400 B: the drift's own cover's place, uploaded again after) | none resident. A set over 182,400 B is refused by the PP (the 16:9 art set in one job, 184,428 B, goes out as single decodes; the 16:9 fill alone is 171,404 B) |
| The file's length | `J_DEC_LEN` (job block `0x50`, plain): the merged image has no `decode_prepare()`, so the record's `len` is clipped to the ring | none resident |

- jref's own DECODE op is already in the merged image (`JREF_HOOKS` implies `JREF_DECODE`) with its hook unset; the overlay's init sets the hook, `J_OPS` bit 16 and `J_CAPS` `J_CAP_DEC_LENT` (the PP's cue to lend the slot and write `J_DEC_LEN`); every load and epoch drops all three (`ovl_drop()`).
- **Checked in vc2sim** (`decode_image_test.py` row "merged image: DECODE as an overlay", 13 cases): unknown before the load; offered after; `HP_E_NOMEM` with no slot lent; with the slot lent, the NP box (142×142) and the 300×300 panel into the other `J_CLS_DRIFT` slot equal `tests/jpeg/golden.txt` bit for bit (t600_exif, 7.8 M instructions, 2 passes); a `RESET` drops DECODE and the cap.
- **On since batch 3** (`merged-image-batch.md` §7): `BCMFW_JOBS_MERGED_OVL` defaults to 1; `mkvideo.py` writes `dec-ovl.bin` (23,884 B `8be7f8d2`) and its record into `apps/video/bcmvideo_image.h` (`BCMVIDEO_DECOVL_*`: size, FNV-1a, ABI word, the image's copy of it, the region's base); `make zip` ships it beside the merged image only if `bcmfw/ovlpair.py --dec` pairs them. The merged image `2c6dc03a` ends at `0x33c7c`, 6,020 B below the arena.
- **The PP's half, as built:** the driver's loader is per instance (`bjc_ovl_set/due/begin/piece/end(k, …)`; `BJC_OVL_DEC` beside the visualizer's `BJC_OVL_VIZ`), due only while the running image's `J_OVL` base is the record's, so the merged image loads `dec-ovl.bin` and never `viz-ovl.bin` (and the display-only image the reverse). The shell loads it from `art_bcm_service()` (the UI thread, the job owner) through the strip's tail past `NP_STRIP_END` (27,936 B: the 23,884 B file in one read), at most a try an epoch (see the sitting 3 fix below for when). With `J_CAP_DEC_LENT` read back, `bcmjob_decode()` ALLOCs the work slot (`J_CLS_DRIFT`, 300×300, `J_DEC_WORK_KEY`) after the outputs, writes `J_DEC_LEN`, clips the record's `len` to the ring, checks the set against `J_DEC_WORK_LENT` (182,400 B), and FREEs the slot behind the DECODE whatever the answer. The art thread first asks the owner to park the drift's own cover (`dj_cover_park()`); the drift takes no new cover slot while such a decode runs. A full pool (`why=nowork`), a load refused, or an overlay gone unseen (`HP_E_UNKNOWN`: the instance's ops dropped, due again) all take the CPU's decode, logged. `HP_OP_TONE` answers `HP_E_BUSY` while an overlay is loaded (its buffer overlaps the region). Host test: `tests/bcmjob` `t_dec_lent`. 
- **Sitting 3 (2026-10-26), RESPANEL [silicon]: DECODE on the chip never ran.** `bcmjob up ops=0020bfff` (OVERLAY bit 21 set, DECODE bit 16 clear), no `bcmjob overlay load`, no BCM decode; the new album's panel went to the PP (`art panel cpu ms=20320`), and the owner's "Sharp, steady" judged the PP's panel. Cause: the load ran only while the Now Playing row showed (`np_shown`), but np-sharp's decode starts off the row (the album just played, the user elsewhere), saw no DECODE and took the CPU. The model offered DECODE at once, so no script saw it.
- **The fix (built 2026-10-26, sim; branch `p19-s3-panel`): the decode orders itself after the load.** Before any decode it would send to the BCM (the panel, the NP box, the resident re-decode) the art thread calls `art_bcm_ready_ovl()`: while a load is due (`ovl_due`, the pass's look at `bcmjob_dec_ovl_due()`), it sets `ovl_owed`, wakes the UI loop (`np_pb_ab_wake()`), and waits **1 s at most** (`OVL_WAIT`), polling its cancel hook. The pass loads it (about 50 ms: the read and the BCM's start-up checks; 30–40 ms in the model) and answers. **The buffer:** Now Playing's own strip (owner NP) any pass, its tail being nobody's; otherwise the strip under a new owner `UI_STRIP_OWNER_OVL`, **only at rest** (`!slide.dir && !ui_anim_active()`: no slide, glide or fade in flight, whose frames read the strip). The last owner then finds the strip not kept and rebuilds by its own fallbacks (a list's rows, the drift's prefetched cover: `preview.h`), and Now Playing can no longer find stale parts "kept" (the old load claimed owner NP from the main menu). The row's own early load stays (no ask needed when its panel comes). **The PP net:** a load not done within the bound, refused, no longer due or with no strip takes the CPU, logged once a boot per cause: `FAULT bcm decode overlay why=wait|refused|gone|strip: the PP decodes`; a request still standing is served at the next rest, for the next decode. A refused load is tried twice an epoch at most (the driver's two tries; a missing file stays due to the driver, and each try is an `open()` on the UI thread). `art_bcm_resident_wanted()` counts a due load as DECODE to come, so the re-decode after a wake is queued before the load. TONE is unchanged (refused while an overlay is loaded): the overlay now loads at the first lit decode of an epoch, so a bench TONE step later in the same epoch meets `HP_E_BUSY` more often.
- **The model (`sim_bcm_job.c`) holds the gate** (`bcm_decode ovl on`, the default): DECODE is offered only after the shell's load in the epoch (lines `bcmjob overlay load k=1 …`, `bcmjob overlay ok k=1 … lent=1`), a decode without it is `NOJOB` (`bcmjob fault op=0x30 why=nojob`), and each decode lends a `J_CLS_DRIFT` work slot (`why=nowork` when the pool is full), freed behind it. `bcm_decode ovl off` is the display-only image's way; `bcm_decode ovl refuse` a missing file. Scripts: `ipodui-p19-dec-ovl` (the load asked by the boxes; off the row after a cut, loaded under its own owner, the panel on the BCM; refused, the PP's net said once), and `dec-bcm`, `panel-resident` (the reload after a cut) expect the load. `bench_ingest.py` flags a RESPANEL answered with no overlay load and no decode on the chip. **Next: RESPANEL again on silicon.**

### 5.2 The panel window: three ways, in order

1. **The decode-once inside of `jdec`** (W0-10 / 19-1e): streaming rows into the destination. **Done (2026-10-24, branch `p19-jdec-stream`, `jpeg-spike.md` §15):** every output, the largest too, is written row by row into its slot at the slot's pitch; the panel's 300×300 fill needs 137,240 B, the square art set 148,114 B and the 16:9 one 184,428 B, all inside the 237,056 B area as laid out, exact against the CPU twin in vc2sim (`decode_image_test.py`). The PP no longer refuses the panel (`bcmjob_dec_work()`). Fills wider than about 980×300 (covers past 3.27:1) still go to the twin. Options 2 and 3 are not needed for the panel.
2. **Borrow `J_CLS_DRIFT` while the preview is still** [ours]: the work area and the drift pool are adjacent (599,552 B together), and the Now Playing row shows a still cover with no pan. A `DECODE` flag would evict the drift's two slots for the job; the PP keeps its covers (Q13), so re-uploading them costs ~32 ms when the drift resumes. Needs a jref change (per-class eviction) and an epoch-like notice for one class.
3. **SDRAM** (option C) after its power figure (#7).

## 6. The slot: class, lifetime, eviction

| Output | Class | Bytes | Lifetime | Eviction |
|---|---|---|---|---|
| NP box | `J_CLS_THUMB` (64 KB, 16 slots) | ≤ 40,328 | until read back and handed to the art cache; kept for the epoch so a return to the same album hits | the next album's box (`J_E_NOSLOT` → `FREE` the oldest render, then `ALLOC` again); any epoch change |
| Panel window | `J_CLS_DRIFT` (300×300), the pool's **resident panel** (as built 2026-10-26; a portrait panel, fewer columns, is read back and freed as before) | 182,400 (pitch 304) | the epoch: the drift and the Now Playing row `BLIT` it by key, no upload, no read-back | a panel decode of another album drops it first (the pool: the drift's own cover + the resident); any epoch change, then a re-decode (never an upload) |
| The work area (merged image only, §5.3) | `J_CLS_DRIFT` 300×300 keyed `J_DEC_WORK_KEY`, lent by the PP | 182,400 | one DECODE | freed behind it; the drift uploads its own cover again |
| The ring | `J_CLS_SCRATCH` (its one slot) | 32,768 | allocated at the first render of an epoch, kept | an epoch change; any other user of the scratch slot frees it first (one owner at a time) |

**Epochs:** a cut (dark), a video `OPEN` (`J_MODE_VIDEO` evicts every UI slot), a game session or a restart loses every slot. Nothing is lost that matters: the outputs were handed to the art cache at the render, and the next render (if any) re-allocates.

## 7. Power

Following `power-contract.md` and the brief's D-3:
- **Lit means the BCM is up** (C1/C2): the render may run. It never asks for power.
- **Dark means nothing is done.** The BCM is cut (C0); the album render never calls `bcm_power_want()`. The art builder's own background decodes on the charger (`BCM_WANT_DECODE`) are a separate path and unchanged.
- **The unlit face window** is lit for the BCM (C2, `power-contract.md` §2) but the user isn't looking: no render starts there; one already running finishes.
- **At the next lit spell** a missing render runs once the screen is lit and storage is awake (§2): the placeholder shows for one render's time.
- **Dark power-ups for this feature: 0** (`bcmjob_dark_ups(BCM_WANT_DECODE)` doesn't count it, because it never runs dark).

## 8. The clock and the timing budget

**Measured in vc2sim** (`decode_image_test.py`, the whole job: probe, decode, ring service; no memory stalls modelled):

| File | Outputs | Passes | Instructions | At 27 MHz [estimate] | At 150 MHz [estimate] |
|---|---|---|---|---|---|
| 300 px, 5.9 KB | 142×142 | 1 (in place) | 8.5 M | 0.31 s | 57 ms |
| 640×360, 20 KB | 142×80 | 1 (in place) | 8.2 M | 0.30 s | 55 ms |
| 600 px EXIF, 33 KB | 142×142 | 2 | 13.0 M | 0.48 s | 87 ms |
| 600 px 4:4:4, 45 KB | 142×142 + 55×55 | 3 | 23.9 M | 0.89 s | 159 ms |
| 3000 px photo-like, 1.9 MB (`jpeg-spike.md` §7) | 300×300 | — | ~215 M (+10 %) | ~8.8 s | ~1.6 s |

**The clock:** `power-contract.md` §2.3 holds UI mode at 27 MHz; 150 MHz only inside video and Class C sessions with the 4b/4c flags. **This design asks for a third case** [ours, for the brief review]: a lit `DECODE` raises the clock to 150 MHz for the job and drops it at the job's end, under the same flags. Without them, renders run at 27 MHz and the shell's estimate (below) keeps only small covers on the BCM.

**The frame:** the shell's animations run at ~30 fps (33 ms a frame), and Now Playing's clock ticks once a second. **A rev 1 `DECODE` holds the BCM's loop for its whole run**: no LCD-contract push, no `ANIM` frame, no other job until it ends. 0.3–0.9 s at 27 MHz is a visible freeze; 55–160 ms at 150 MHz is a missed clock tick at worst.

**So S3c: the decode yields** [ours; built 2026-10-24, sim]:
- **The hook:** the decoder calls `yield()` once per MCU row (`jpeg_load.c`) and once per output row (`resize.c`). `bcmfw/jpeg/env/jenv.h` makes it call `jdec_yield` (`jdec.h`), NULL by default (the Mac and vc2sim decode tests). The display-only image sets it to `decode_yield()` (`video/jobs_bcm.c`), which runs `main.c`'s `bcmfw_service()` at most every 2 ms by the 1 MHz counter.
- **The service** is what the main loop does between jobs, minus the ring: the heartbeat (`hp_heartbeat()`, so the PP's liveness check stays green), the running animation's next frame (`jobs_tick()`), and a command word the PP wrote (the LCD contract's `LCD_UPDATE`/`LCD_UPDATERECT` pushes, handled with the loop's BUSY/DONE handshake).
- **The choice: never the command ring.** Running a ring record inside a running job would re-enter `jref_exec` on the same state and break ring order. So a job queued behind the `DECODE` waits for it, exactly as ring order says; the shell sends nothing behind a lit decode that it needs drawn meanwhile, and its frame takes the LCD contract's push or its CPU path (`bcmjob_resync`). `J_CUR_OP` reads `DECODE` all the while (`jobs.md` §6).
- **Grain:** a 600 px source to 142 is ~76 k instructions a row (2.8 ms at 27 MHz); the 3000 px cover to 300 is ~0.7 M a row (26 ms at 27 MHz, 4.7 ms at 150 MHz) [sim-derived estimates]. So frames and pushes come within one row's time; a large cover's rows still make the animation stutter at 27 MHz.
- **Checked** (`decode_image_test.py`, row "S3c", the counter run at 27 MHz's pace): ANIM (a looping pan), DECODE and ANIM_STOP from one ring pass, so every frame ANIM_STOP counts was drawn inside the decode. **12 frames during the 300 px decode (8.6 M instructions, ~0.4 s) and 19 during the 600 px EXIF one (13.2 M, ~0.6 s)**, about 30 fps; an `LCD_UPDATERECT` written mid-decode is served before the decode ends; the heartbeat moves; the outputs equal golden. With the hook unset, 0 frames and 6 of its 16 cases fail.
- **Cost:** 168 B of the display-only image's image+bss; the merged image (no `DECODE`) doesn't build it.

**The shell's gate, before S3c** (now only for a clock tick or a stutter-free big cover; S3c lifts the freeze) [ours]: a lit render goes to the BCM only if its estimate, **4 M + 36 M per source Mpx, per output** (the probe is nearly free) at the current clock, is at most **150 ms** (`ALBUM_RENDER_FREEZE_MS`). The figures fit the table above [sim: 600 px 13.0 M, 640×360 8.2 M, two outputs of 600 px 23.9 M]; they overestimate large sources, whose cost per Mpx is lower (entropy-bound: 11–25 M per Mpx at 3000 px, `jpeg-spike.md` §7). At 150 MHz the gate is ~22 M instructions, the box from a cover up to ~700 px square; at 27 MHz ~4 M, nothing but tiny sources, so at 27 MHz the render effectively waits for S3c. Everything else stays with the background builder, as today. The shell reads the source's size from its SOF before submitting (the art builder already parses it).

**The deadline:** `J_T_DECODE` (2 s per started MB, `jmodel`) assumes 150 MHz. At 27 MHz the driver scales it by 150/27.

## 9. The fallback: the CPU path

Every case below leaves the user exactly where today's firmware does:

| Case | What happens |
|---|---|
| `bcmjob_have(J_OP_DECODE)` false: the merged image is resident, the BCM is off, or `DECODE` was marked slow or failed this boot | no render; the art builder builds the album in the background (CPU, or the BCM on the charger) |
| `J_E_DEC_CORRUPT`, `TRUNCATED` | the next source in §3's order; none left: the album's record says so, as the builder's would |
| `J_E_DEC_UNSUPPORTED` | the CPU twin on that source now (progressive may decode there); its verdict is the record's |
| `HP_E_NOMEM` (an output past the work area) | refused by `bcmjob_decode()` before anything is sent (`bcmjob_dec_work()` over `J_DEC_WORK_MIN`): that output on the CPU path; the box still renders |
| `BCMJOB_REFUSED` (dark on battery, D-3) | the CPU path, if the shell's own rules allow a CPU decode then |
| `BCMJOB_ABORTED` (the cancel hook: a newer track, the user back) | neither side: the render stays queued for its next chance |
| `BCMJOB_TIMEOUT` (past the deadline) | the driver's rule (`jobs.md` §6): `DECODE` is marked slow for the boot and the BCM restarted if it stopped answering; the CPU path from then on |
| the estimate over the gate (§8) | the background builder |

The pixels can't differ: `jdec` is one definition on both sides (`jpeg-spike.md` §8), so a box rendered on the BCM equals the one the CPU would have built, bit for bit [sim: the golden table].

## 10. The same on every unit (CLAUDE.md rule 7)

- **32 and 64 MB:** the render uses no new PP RAM. The streamed pieces come from the art builder's existing decode block (~164 KB, pinned while it works) and the read-back box goes into its slot buffer; no `core_alloc`, nothing taken from the audio buffer. Identical on both.
- **Flash and HDD:** the render reads its source only when storage is awake, on both. Rendering **ahead** at the buffer fill (§2) is what keeps the user's view identical: the next track's art is ready at the change whether the fill took a spin-up or not. A jump to an unbuffered track waits for the read that playback itself needs, on both. `docs/storage-choices.md` gets no entry: nothing here is tuned for flash.
- **The resident image** is the same on every unit (40,800 B with `DECODE`).

## 11. The shell's contract (in `bcmjob.h` since 2026-10-24)

**The API is `bcmfw/jobs/bcmjob.h`**, "DECODE" section; the host model implements it (`jmodel.c`, 40 `jmtest` cases); §14 is the shell's recipe and §15 the device driver's steps. It is a thin layer over the existing wire ops (no `jobs.h` change):

| Call | What |
|---|---|
| `bcmjob_decode(req, res)` | one decode, blocking, on the art thread. `req`: the source as a `struct jdec_src` (the CPU twin's own type, so the fallback takes the same object), up to `J_DEC_MAX` outputs (`dw`, `dh`, the window, and the slot's class and key), flags, a deadline, a cancel hook. `res`: each output's READY slot and its F16w, the source's size, the passes and bytes streamed |
| `bcmjob_read(slot, x, y, w, h, px, pitch)` | a slot's rect back by port reads, in bands of any height |
| `bcmjob_dec_verdict(r, res)` | the answer in `jdec`'s terms: `JDEC_OK`, the file's `JDEC_E_CORRUPT`/`_TRUNCATED`, `BCMJOB_DEC_TWIN` (run the CPU twin now), `BCMJOB_DEC_DEFER` (called off) |
| `bcmjob_dec_work(out, n)` | the work a set needs (`jdec_work()`: 114,172 B for the NP box, 137,240 B for the panel's 300×300 fill, 184,428 B for the whole 16:9 art set; `jpeg-spike.md` §15). The call checks the set it would send, the misses: a request none of whose outputs fits alone is refused before any traffic (a fill wider than about 980×300, alone); a reduced set over `J_DEC_WORK_MIN` answers `HP_E_NOMEM` once the `ALLOC`s have answered, with no `DECODE` sent and the misses freed. A set that fits only once a hit is dropped is decoded |
| `bcmjob_dec_set(out, res, set)`, `bcmjob_dec_primary(set, n)` | the reduced set (`res.miss[]`, the misses in request order) and its decode-once primary (the largest `dw × dh`, the first of equals) |

**Hits and the reduced set** (2026-10-24): an output already `READY` this epoch (its key, size and class) is a hit. It is never a `DECODE` destination and never freed by the call, so a failed, aborted or timed-out decode leaves it as it was. The `DECODE` gets the misses only, in request order; each missed slot then holds exactly what `jdec_run()` writes given **that reduced set**, which is the set the CPU twin must be given to agree (`bcmjob_dec_set()`). The reduced set's primary is its own: when the request's largest output hits, the largest miss becomes the primary and is decoded as a single decode at its size. Every output a hit: nothing is decoded or streamed.

**Where the pixels go, and why** [ours]: always into BCM slots, then back to the PP for what must last.
- `DECODE` can only write surfaces, so the slot is the first stop anyway, and it is also the **instant display**: the preview `BLIT`s it with no upload, in the same epoch.
- **The art cache file is the product.** Every slot dies at the next cut, and on battery every backlight-off is a cut (C0, `power-contract.md`). A cover that lived only in a slot would decode again at every lit spell; read back once and stored (the NP bundle, `npcover.pak`'s panel), it never decodes again ("decode once", §3).
- The read-back is cheap next to the decode: a 142 × 142 box is about 10,000 port words, 5–10 ms idle [hypothesis, `data-paths.md` R2], and it is what the CPU path's own compose and write need as input anyway.

**Against the rev 0.1 proposal** (this section before 2026-10-24): the source is a `jdec_src` (not an fd), so one object serves both sides and the shell's own reads (embedded blob offsets, folder files) stay the shell's; the driver allocates the outputs by class and key (a decode may have to power the BCM first, and a dark BCM has no slots to hand out); a cancel hook replaces `bcmjob_decode_abort()` (the art builder already keeps its call-off state, `npr_abort`); `BCMJOB_ABORTED` is `-7`; the verdict helper is new.

**In the art cache** (unchanged from the proposal; not on `forge/p19` yet):
- `artcache_render_album(key, id3)`: queue a render for the album (the latest call wins); the art thread runs it under §2's conditions, in §3's source order, through `bcmjob_decode()` when §14's eligibility holds, and otherwise leaves it to the builder.
- On success it composes the NP bundle from the read-back box, writes it as the builder would (same file, same tag), bumps `artcache_commit_seq()`, and keeps the panel window's slot for the preview (§6). Now Playing's existing `art_pending` / commit-sequence path then shows it without new code.
- **The preview** gains one step between its 1 and 2: the album's panel-window slot this epoch, `BLIT` to the panel (a job, no upload).

**Trace lines** (under 111 characters, as `jobs.md` §10's):
- the driver's: `bcmjob decode len=N n=N`, `bcmjob decode done st=N err=N passes=N bytes=N ms=N`, `bcmjob fault op=0x30 why=refused|aborted|…`;
- the shell's: `album render key=%016llx why=ahead|change src=emb|file out=%d`, `album render done key=… ms=%u passes=%u` / `album render fault key=… why=busy|off|nojob|timeout|failed|aborted|gate|refused`.

## 12. Against the placement plan

The plan's row B14 puts JPEG decode on the BCM "once `J_OP_DECODE` lands, lit or on the charger only, streaming MCU rows in SRAM (`J_CLS_SCRATCH`, no SDRAM bring-up)", step 10, with the CPU art builder as the fallback.

**Where this design agrees:** SRAM only, no SDRAM; the input ring in `J_CLS_SCRATCH`; lit or charging only, never dark on battery (D-3); the CPU builder unchanged as the fallback; one definition of the pixels.

**Where it differs:**
1. **A foreground user.** The plan has one user, the background art builder. This adds a second: the current album at a track change, while lit, with a debounce and an abort (§2). It competes with nothing on the CPU (the work is on the BCM) but it does take the art thread for its read-back and compose (~20–30 ms).
2. **The image.** The plan's rows assume the resident image carries the jobs. `DECODE` fits only the **display-only** image (§5), so step 10 depends on G9 and bench row B13, not just on the job landing.
3. **"While the drift and marquee are idle."** The spike's §3 (and the plan by reference) runs decodes only then. A track change on a lit screen isn't idle, so the decode yields to the LCD pushes and the animation as it runs (S3c, §8, built); jobs queued behind it still wait.
4. **The clock.** The power contract allows 150 MHz only for video and Class C; this asks for a lit `DECODE` as a third case (§8).
5. **Memory while decoding.** The spike's §3 budgets ~172 KB in the arena's spare and `J_CLS_STRIP`. Rev 1 `jdec` needs the whole output plus 190 KB instead, so the display-only image gives it a fixed 237 KB area and borrows no pool. The streaming inside brings the spike's figure back.

## 13. S3b as built, and what's open

**Built (2026-10-23):**
- `bcmfw/jobs/jdec_ring.{c,h}`: `jdec`'s reader over the input ring (a new `struct jdec_src`; `jdec.h` unchanged).
- `bcmfw/video/jobs_bcm.c`, its `DECODE` section: the decode hook (`jdec_run` over the ring or a memory slot, the work area) and `decode_prepare()`; `J_OPS` bit 16 in the display-only image only.
- `bcmfw/jobs.h`: `J_DEC_RING`, `J_DEC_IN_RD`/`WR`, `J_DEC_IDX`, `J_DEC_ABORT`, `J_DEC_IDLE`, `J_DEC_STALL_MS`, `J_DEC_WORK_MIN`.
- `bcmfw/config.h`: the display-only layout (§5.1); `bcmfw/build.py`: `jdec` and the decoder built into that image (as `jpeg_test.py` builds them), and the layout check.
- `firmware/target/arm/ipod/video/bcm-devtest-images.c`: the regenerated display-only image (40,800 B; only that entry changed).

**Checked** [sim]:
- `bcmfw/tests/decode_image_test.py` (new): corpus files streamed by a PP model through the ring, each output's FNV-1a, of the bytes read back, equal to `tests/jpeg/golden.txt`, and `COMMIT` with the read-back's F16w accepted (jobs rev 0.2; before it, `COMMIT` checked the golden FNV-1a itself). A file in place, files over 2 and 3 passes with the ring wrapping, two outputs in one job, the NP box's 16:9 fit; progressive, the PP's abort, a ring that isn't a scratch slot and an output past the work area fail with their codes; a good decode after all of them. 30 cases. Mutation-checked: without the new pass, 5 cases fail; with wrapped reads off by 64 bytes, every wrapping output fails.
- `jobs_image_test.py` (131 cases, the display-only image now offers `DECODE`; the warm swap with the larger image), `jobs_test.py`, `hostproto_test.py` (110 cases), `forms_test.py` (the display-only image: 0 unproven forms; the merged image unchanged at 218,116 B), `mkdevtest.py --check`.

**Open:**
1. ~~**S3c, the yield**~~ (§8): built 2026-10-24. The ring still waits for a running `DECODE` by design; a large cover's row time (26 ms at 27 MHz) is the stutter left.
2. **The decode-once inside** (`jdec`'s owner): the panel window in the same work area (§5.2). When it lands, `J_DEC_WORK_MIN`'s comment and §4.2's destinations follow its `JDEC_WORK`.
3. **The 150 MHz decode** (§8): a power-contract change for the brief review.
4. ~~**jref's ring source natively**~~ (2026-10-24): not needed. The host model streams the ring with the same glue as the image (`jmodel.c` `dec_prepare()` and `model_decode()`, after `jobs_bcm.c`'s `decode_prepare()`): a ring `DECODE` holds the model's loop until the PP has fed every pass, then the hook runs over the streamed file. jref, `jdec.h` and `jdtest.c` are unchanged.
5. **On silicon:** the ring's port writes and the BCM's reads of host-written SRAM are the same paths as slot uploads [silicon S37 for engine 2], but the ring is untried; it joins bench row B3's build.
   - **As built on silicon, 2026-10-25** (`bench-20261025-1430/bcmtest.log`): a slot upload's `ALLOC` replied, but **`COMMIT` has not yet passed** [silicon]: it failed for the UI drift (engine-2 upload, 160 x 240) and for B5's `win_slot` (CPU writes, 300 x 300 and 400 x 160), on the merged and the display-only image. The status was not logged, so the cause is open: the write path or the BCM-side F16w sum [hypothesis] (open question #67; a port write-check probe is planned). **Answered and fixed:** the BCM's F16w read 7 of every 8 pixels with the `0x29`/o1 halfword load, which sign-extends (`quirks.md` #33). **Sitting 2 (2026-10-25 evening, build 799ff0f8d7, `bench-20261025-2257`) [silicon]: `COMMIT` passes.** B5 pushed 31 of 31 windows by DMA (pitch 320, 300 and 400 at 5,947, 5,947 and 2,976 µs) with no failure at step 5 (sitting 1: step 5.4, `st=23`), and the UI drift's uploads all returned `rc=0` and ran on the BCM (`bcmjob vpu=0x3 expect=0x3`, no fault or `FALLBACK` line in `ipodui.log`).
6. **Size margin:** the display-only image ends 1,120 B below the work area (2026-10-24; 3,344 B before rev 2 of `jdec` and S3c). A bigger `jdec` moves the work area up and below `J_DEC_WORK_MIN` only if the drift pool also moves; `build.py` fails the build first.
7. ~~**The panel stream's dither phase**~~ (fixed 2026-10-25, branch `p19-6shell`: `ps_row_32x()` and `ps_row_8()` dither at `s->x0 + col` and the picture's row; the 16:9 fixture's panel, `x0` = 117, read 449beaa7 on the CPU against the BCM's 5ac79827 before the fix and 5ac79827 on both after, `ipodui-p19-dec-bcm.script`) (found 2026-10-24 while writing §14) [ours, from the code]: `ps_decode()`'s row writers (`ps_row_32x()`, `ps_row_8()` through `output_row_8_native()`) dither each crop row from column 0 of the crop, but a full decode dithers from column 0 of the `dw`-wide picture (`DITHERXDY(col, dy)`, `bmp.h`, a 16-column pattern). So a panel streamed from a fill wider than the slot, with `x0 = (dw − 300)/2` not a multiple of 16, differs by the dither's least significant bits from the same window of `jdec` (or of the BCM, or of the builder's non-stream crop). Square art (`x0` = 0) is unaffected. For np-sharp's `npcover.pak` to be bit-identical whichever side decodes, the row writers should dither at `s->x0 + col`: a one-line change in each, which slightly changes (invisibly) the panels built from now on. It is the shell session's to make; nothing here depends on it until the panel fits the work area.
8. **The shell's half** (§14) and **the driver's** (§15): another session's. **The shell's half, as built (2026-10-25, branch `p19-6shell`):** `apps/ipodui/art_bcm.{c,h}` and `artcache.c`'s `bcm_fit()` / `bcm_panel()` / `ps_bcm_rows()`, n = 1 per size. Where it differs from the recipe:
   - **The slot is freed after the read-back** (on the UI thread's pass, `art_bcm_service()`: the art thread is not the job owner), not kept for the epoch: nothing BLITs it yet (the preview's instant swap, step 7, is not wired), and a kept 300 x 300 panel would hold one of the drift pool's two covers.
   - **Not while a UI job client runs:** a decode starts only while the drift doesn't own the panel and no marquee PAN runs; the drift takes its CPU frames while one is in flight (a FENCE behind a decode would make it still for the boot, and a decode's failure would raise `J_ERRORS`, which the drift reads as its own).
   - **Dark:** no call at all while the BCM is cut (`bcmjob_power_gen()` 0): the CPU path where np-sharp's own rules allow it, as before. In the unlit window (backlight off, the panel still up for the BCM) the render goes to the BCM, which is np-sharp's usual case on the charger.
   - **The deadline:** §8's estimate from the source's size (4 M + 36 M instructions a source Mpx at 27 MHz), doubled, plus a second, at least `BCMJOB_T_DECODE(len, 27)`, at most 60 s; never the driver's 3 s cap.
   - **Read-back:** the box only, by default. The panel's path (in bands of the stream's rows, on the art thread, stopping between bands when the user comes back) is built, **on in developer (devtest) builds** so the device logs both timings every time, and **off in release builds** (`art_bcm_panel_set()`, the coordinator and the owner 2026-10-25): its read-back is about 45 k port words, 2.5-3.8 s of PIO under load, likely more than the CPU's decode. **Rule:** the panel goes on the BCM only when read-back + decode < the CPU's decode on silicon (bench row PANELAB, `bench-p19.md`), or when a later design keeps the panel resident on the BCM (a BLIT, no read-back). Each panel logs its CPU ms, or its BCM decode and read-back ms when the switch is on. **Since 2026-10-26 the switch is on in every build:** the panel stays resident (the next bullet), which is the rule's second condition; its read-back runs only for the file, in the background.
   - **The end state, as built (2026-10-26, branch `p19-panel-resident`; sim):** the panel's slot stays READY as the drift pool's **resident panel** (`drift_jobs.c` `dj_resident*()`: one resident beside the drift's own cover; installed by the job owner's pass, `art_bcm_keep()` → `art_bcm_service()`; a new album's panel decode drops the old resident first; an epoch loses it). The drift BLITs it by key with no upload. The Now Playing row arrives by the BCM's FADE to it (`dj_resident_show()`, 6 steps, the CPU's `ui_panel_fade(raw)` on the BCM) and stays its BLIT; after a slide or a direct draw, or when the decode lands while the row shows, it swaps to it. On the row's path nothing is read: no file, no slot (`np_cover_draw()` returns at once for a resident album). The CPU's own picture is drawn only when the CPU must take the panel back (`np_bcm_settle()`: a slide, a takeover, a hide, a cut). **The file** (`npcover.pak`, what outlives a cut) is read back from the slot only by the art thread at idle, never while the row shows (`art_bcm_np_shown()`; `npr_place()`; between bands); a later run finds the slot a hit (no decode) and reads it. After an epoch the row queues `ARTCACHE_NPR_RESIDENT`: the BCM decodes the same source again (`npr_resident()`, nothing written, never an upload). Proof lines: `FAULT art bcm read off thread|while shown` never appear (`ipodui-p19-panel-resident.script`). Timings for silicon: bench row RESPANEL. The merged image's half: §5.3.
   - **The end state (forward plan, the owner 2026-10-25: never give up on the video chip):** the panel stays **resident on the BCM**. The DECODE writes it into its `J_CLS_DRIFT` slot and it stays there: the preview and the drift `BLIT` it from that slot, with no read-back at all, as long as the epoch lasts. The PP then keeps only what must outlive a cut: the file, written by the CPU twin in the background or from the box-sized pieces it already holds. What this needs: the preview's instant swap (§14 step 7) and the drift's cover upload reading a slot by key instead of `pv.slot`; slot lifetime and eviction alongside the drift's two covers (§6); and re-decoding after an epoch change instead of re-uploading. Until then, the switch above and bench row PANELAB decide the read-back path.

## 14. Shell recipe: decode a cover on the BCM

For the session that wires the shell (`apps/ipodui/artcache.c`, read-only here; names from `forge/p19`). The contract is `bcmjob.h`'s DECODE section (§11).

### 14.1 Where the seam goes: `decode_run()` step 2, not above it

Both users reach the decoder through one chain:
- **np-sharp's panel:** `npr_run_impl()` → `npr_decode` (the function pointer, today `npr_decode_pp()`) → `panel_stream()` → `source_decode()` → `try_file()` / `try_embedded()` → `decode()` → `decode_pinned()` → `decode_run()` → step 2 → `ps_decode()`.
- **The NP box** (the album render, and the builder's `np_bundle()`): `np_write()` sets `w->fit` → `decode_pinned()` → `decode_run()` → step 2 (fit) → `img_decode()`; then `np_compose(w, dbuf)` from `w->out_off`.

**Splice at step 2**, where `img_decode()` / `ps_decode()` is called with the final `dw × dh`. Everything above it stays one definition for both sides: the source order (`source_decode()`), the signature (`src_crc()`), the step 1 probe (the CPU's header read, which also yields the decoder's −4 terminal verdict), and the size rules (`artcache_fit_rect()`, the fill with `FILL_MAX`, `panel_cw()`). The BCM never computes a size (`jdec.h`).

**`npr_decode` stays the seam it was meant to be:** `npr_decode_bcm()` sets a flag on the work (`w->bcm = true`), calls `npr_decode_pp()`, and clears it. With the flag clear, nothing changes anywhere. The album render sets the same flag around its `np_write()`.

### 14.2 Step by step

1. **Eligible?** Check all of these before any BCM traffic; any "no" is the CPU code below the splice, exactly as today:
   - `type == AA_TYPE_JPG` (BMP and PNG never go to the BCM);
   - `aaflags == 0`: an embedded picture with `AA_FLAG_ID3_UNSYNC` or `AA_FLAG_VORBIS_BASE64` isn't a JPEG stream as stored, and the BCM reads the bytes as they are;
   - `w->bcm`, and `bcmjob_have(J_OP_DECODE)` (false with the merged image, the BCM off, or `DECODE` marked this boot);
   - `bcmjob_dec_work(&o, 1) <= J_DEC_WORK_MIN`. **Since `jdec` streams its outputs (§5.2, 2026-10-24) the NP box and the panel both pass**: a 300 × 300 fill needs 137,240 B, the box with it 148,114 B. Only fills wider than about 980 × 300 (covers past 3.27:1; `FILL_MAX` allows 1200 × 300) still take the CPU path at once, with no BCM traffic. Until then (rev 2) the panel's 378,160 B failed this check;
   - for the track-change render only: §8's estimate gate.
2. **The source:** a `struct jdec_src` over the builder's own fd. `len` is the embedded blob's `size` (the `len` passed to `decode()`), or the file's size when `len` is 0 (`try_file()` has it as `rec->src_size`):
   ```c
   struct art_jsrc { int fd; off_t off; };
   static int ajs_start(void *c)
   {
       struct art_jsrc *s = c;
       return lseek(s->fd, s->off, SEEK_SET) == s->off ? 0 : -1;
   }
   static long ajs_fetch(void *c, void *buf, uint32_t n)
   {
       return read(((struct art_jsrc *)c)->fd, buf, n);
   }
   static int ajs_skip(void *c, uint32_t n)
   {
       return lseek(((struct art_jsrc *)c)->fd, n, SEEK_CUR) < 0 ? -1 : 0;
   }
   ```
   The same object is what the CPU twin takes once the art builder moves to `jdec_run()` (W0-10).
3. **The outputs**, from step 2's own numbers (one per call, or the art set in one call; see "n > 1" below):
   - **fit** (the NP box): `dw = w->fitted.w`, `dh = w->fitted.h`, the window the whole picture; `J_CLS_THUMB`;
   - **fill** (the thumbnail; the panel): `dw × dh` from the fill rule, the window `((dw − W)/2, (dh − H)/2, W, H)`; a panel in `J_CLS_DRIFT` (or `J_CLS_STRIP` for the preview's 160 × 240 alone, §6);
   - **portrait panel** (`w->ps_cw`): `dw = ps_cw`, the window `(0, (dh − H)/2, ps_cw, H)`; the bars are the shell's at read-back (`ps_bars()`);
   - **key:** §4.3's (the album's art key, the output's tag in the top byte).

   **n > 1 (2026-10-24: the driver sends only the misses).** The whole art set fits the work area (184,428 B for a 16:9 cover, §5.2), and a hit is never written, so the recipe can send the set in one call even when the cache misses only some sizes: the hits keep their pixels, and the misses are decoded together in one pass of the file. What the shell must keep in step:
   - a decode-once job's outputs other than the primary are resized from the primary's rows, so they differ from single decodes at their own sizes. The BCM's pixels equal `jdec_run()` given **the reduced set** (`res.miss[]`, `bcmjob_dec_set()`), so the CPU path must call `jdec_run()` with that same set, not with the request's and not one output at a time, wherever both sides must agree bit for bit (np-sharp's `npcover.pak`, the NP bundle);
   - the primary is the reduced set's largest; with one miss (or n = 1) the BCM's pixels are the shell's single decode at `dw × dh` (rev 1's equality, `jdec.h`): today's builder's pixels, bit for bit. Until the CPU path decodes sets, send n = 1 per size where the builder's own single decodes must match.
4. **The call:**
   ```c
   struct art_jsrc a = { fd, off };
   struct jdec_src src = { len ? len : (uint32_t)filesize(fd), ajs_start, ajs_fetch, ajs_skip, &a };
   struct bcmjob_dec_req q = { &src, &o, 1, flags, 0, art_dec_cancel, w };
   struct bcmjob_dec_res res;
   int v = bcmjob_dec_verdict(bcmjob_decode(&q, &res), &res);
   ```
   - **Thread:** the art thread only (`art_thread_id`). The call blocks up to its deadline: it feeds the ring and yields about every millisecond. Never call it from the UI thread.
   - **flags:** the track-change render uses `BCMJOB_DEC_NO_WAKE` (lit only, §7). np-sharp and the background builder use 0; `npr_place()` already limits them to the charger while playing.
   - **cancel:** np-sharp: `npr_busy && npr_abort`, the same state `npr_called_off()` reads, which `art_gate()` sets when the user comes back. The track-change render: a newer render queued (the latest wins, §2). Also `usb_owns_disk()`, as `ps_row_8()` checks.
   - **deadline:** 0, the driver's default (`BCMJOB_T_DECODE` at the BCM's clock).
5. **Act on the verdict:**

   | `v` | Step 2 does | Matches the CPU path's |
   |---|---|---|
   | `JDEC_OK` | read back (step 6), then return `true` as the CPU branch would (`w->out_off = 0` for the fit; the window into `slot_buf(w)` for a thumbnail; the panel's bands, step 6) | a successful `img_decode()` |
   | `JDEC_E_CORRUPT`, `JDEC_E_TRUNCATED` | return `false`, `w->source_terminal` left false; with `res.read_err` set `w->source_retry = true` | `img_decode()` failing `rc <= 0`: `try_*()` moves to the next source |
   | `BCMJOB_DEC_TWIN` | fall through to the CPU code below the splice, unchanged (UNSUPPORTED, NOMEM, RANGE, OFF, NOJOB, BUSY, TIMEOUT, REFUSED, a stalled CORRUPT, a failed read-back) | itself: the CPU's −4 sets `w->source_terminal` as today |
   | `BCMJOB_DEC_DEFER` | `w->lit = true`, return `false` | the user came back: `npr_run_impl()` keeps the render queued (`PANEL_DEFERRED`) |

   The user sees the same thing either way: the same pixels on success, the same next source on a bad file, and the CPU's own answer whenever the BCM has none.
6. **Read back, check, store:**
   - **The box:** `bcmjob_read(res.out[0].slot, 0, 0, cw, ch, dbuf, cw)` into the pinned decode block, where `img_decode()` would have left it. Sum the rows with `j_f16w()`. They must equal `res.out[0].a`, `b`; on a mismatch read once more, then treat it as `BCMJOB_DEC_TWIN`. Then `np_compose(w, dbuf)` and `np_write()`'s bundle write, unchanged.
   - **The thumbnail:** the same, straight into `slot_buf(w)` (W × H). This replaces step 3's crop copy.
   - **The panel** (once the work fits): read in bands of `s.rows` rows into `s.band` (the same band `ps_decode()` sizes from the decode block). Put the portrait bars, extend `s.crc`, and write each band as the rows of `ps_format` do (`ps_place()`, `ps_flush()`). Then the tag, the padding and the `fsync` are unchanged. The BCM's slot is the row producer in place of `img_decode(..., &ps_format)`.
   - **On the charger in the dark** (no `NO_WAKE`), the driver keeps the BCM up for `BCMJOB_DEC_HOLD_MS` after an OK: read back, then `bcm_power_release(BCM_WANT_DECODE)`.
   - **The slot** stays READY for the epoch: the preview's instant swap (step 7) and a hit if the album comes back. On `J_E_NOSLOT` for the next album, `FREE` the oldest render's slot and call again. `bcmjob_on_epoch()` forgets them all.
7. **Snapshot first, stream behind** (memory: snapshot-first-stream-behind). No frame ever waits on `bcmjob_decode()`:
   - **At once:** what the cache already has. The preview (`ui_preview_nowplaying()`) shows the panel-set image or `npcover.pak` through `np_cover_read()`, else the stretched 55 px thumbnail (`np_art_draw()`). Now Playing shows `artcache_np_take()`'s composed bundle, else its placeholder.
   - **Behind:** the art thread decodes. On OK it writes the cache as the CPU path does, then bumps the sequences readers already watch: `npc_seq` / `artcache_npcover_seq()` for the preview, `artcache_commit_seq()` for Now Playing's `art_pending`. The sharp cover swaps in at their next draw. For the preview in this epoch, a `BLIT` from the slot shows it with no file read (§11). Later lit spells read the file.
8. **The power rule (P19 D-3), as the shell sees it:**

   | State | `bcmjob_decode()` | The shell then |
   |---|---|---|
   | lit | runs | — |
   | the unlit face window (lit for the BCM, the user not looking) | runs | the track-change render doesn't start one (§7) |
   | dark, battery | `BCMJOB_REFUSED`, nothing powered | the CPU path, only where its own rules already allow a CPU decode (np-sharp: `npr_place()` stopped and idle; never while playing on battery) |
   | dark, charger, `NO_WAKE` | `BCMJOB_OFF` | the track-change render waits for the next lit spell |
   | dark, charger | powers the BCM for the job (`bcmjob_dark_ups(BCM_WANT_DECODE)` counts it), holds it for the read-back | release the want after reading back |

## 15. What the PP driver needs (`bcmjob-core.c`, the other session's)

`bcmjob_decode()` on the wire, step by step. The host model (`jmodel.c`) and `decode_image_test.py`'s `Stream` class are the references; `jmtest`'s DECODE cases are what the driver's host test (`tests/bcmjob/`) should mirror.

1. **Before any traffic:**
   - one decode in flight (`BUSY` for a second caller);
   - `n` in 1..`J_DEC_MAX` and every window inside its `dw × dh` (else `FAILED`, `J_E_RANGE`);
   - some output's `bcmjob_dec_work()` alone ≤ `J_DEC_WORK_MIN` (else `FAILED`, `HP_E_NOMEM`: no reduced set could fit). A NOMEM sent would count toward the op's failed mark.
2. **Power (D-3):**
   - dark on battery: `REFUSED`, traced `bcmjob fault op=0x30 why=refused`;
   - dark on the charger with `BCMJOB_DEC_NO_WAKE`: `OFF`;
   - otherwise, on the charger: `bcm_power_want(BCM_WANT_DECODE, cold start + deadline + margin)`, then wait for the job block (the HELLO) within the cold-start bound. At the end: on OK, re-want for `BCMJOB_DEC_HOLD_MS` (the read-back); otherwise release.
3. **`bcmjob_have(J_OP_DECODE)`:** `J_OPS` bit 16 and the slow/failed marks. Answer `NOJOB` or `OFF` as `bcmjob_slot()` does.
4. **The ring, once per epoch:** `ALLOC` `J_CLS_SCRATCH`, `BCMJOB_DEC_RING_W` × `BCMJOB_DEC_RING_W` (32 KB), key `BCMJOB_DEC_RING_KEY`, then (unless a hit) `COMMIT slot, 0, 0, J_COMMIT_NOCHECK`. Keep the slot and its epoch, and forget them in the epoch callback. `J_E_NOSLOT` (another scratch user) is `FAILED`.
5. **The outputs:** `ALLOC` each (its class, `cw × ch`, its key, `J_TAG_REPLY`). A miss joins the reduced set (`res.miss[]`, request order); a hit is never written or freed by this call. If all of them hit, `COMMIT … J_COMMIT_NOCHECK` each for its sums and answer OK with no `DECODE`. On a failed `ALLOC`, `FAILED` with its code (free the misses taken). The reduced set's `bcmjob_dec_work()` over `J_DEC_WORK_MIN`: `FAILED`, `HP_E_NOMEM`, the misses freed, no `DECODE` sent.
6. **Before submitting:** read `J_DEC_IN_RD` (`HP_ENC`'d; a torn word is "not yet", R2) and keep its pass as the last one served.
7. **The record:** `J_OP_DECODE`, tag `| J_TAG_REPLY`, payload `ring | J_DEC_RING, len, n`, then per miss, in request order, `J_XY(dw, dh), J_XY(cx, cy), J_XY(cw, ch), slot, J_XY(0, 0)` (`J_DEC_ARGS(miss_n)` words; the payload's n is the misses'). Ring full: `BUSY`.
8. **Feed the ring** on the calling thread until the reply, exactly as `Stream.service()`:
   - a pass in `J_DEC_IN_RD` that is neither the current one nor the last served is **new**: `src->start()`, written = 0, publish `J_DEC_IN_WR = HP_ENC(J_DEC_IDX(pass, 0))`. Never write `J_DEC_IN_WR` before seeing a new pass: the BCM sets it to `J_DEC_IDLE` at the job's start;
   - then pieces: at = written mod ring; size = min(`BCMJOB_DEC_PIECE`, the room, ring − at, len − written), where the room is ring − (written − consumed × 64) and consumed is `J_DEC_IN_RD`'s units unwrapped mod 4096 against the units written. Whole 64-byte units except the file's last piece. Each piece goes by engine 2 under `bcm_port_lock()`, then `J_DEC_IN_WR = HP_ENC(J_DEC_IDX(pass, ceil(written / 64) & 0xFFF))`;
   - **a read error** (`start` < 0, `fetch` short): write `HP_ENC(J_DEC_ABORT)` and set `res->read_err`;
   - **between pieces:** the cancel hook (if it says stop while passes remain: `J_DEC_ABORT`; else wait for the end), and a yield. If the gap between two services ever reaches `J_DEC_STALL_MS`, set `res->stalled`.
9. **The deadline** runs from the record's queueing: `req->ms`, or `BCMJOB_T_DECODE(len, J_POWER's MHz)`. Past it, write `J_DEC_ABORT` and apply the slow rule: a live heartbeat with `J_CUR_OP` = `0x30` is a slow miss, marked on the second this boot; a dead heartbeat is the BCM failure path. Answer `TIMEOUT`. A cut or a restart meanwhile (a new epoch) answers `OFF`.
10. **The reply:** status, then `sw | sh << 16`. `J_E_DEC_*` never count toward the failed mark (already the rule); other failures do.
11. **OK:** `COMMIT slot, 0, 0, J_COMMIT_NOCHECK` per output (deadline `J_T_COMMIT(bytes)`); the reply's A and B go to `res->out[k]`. The slot is READY for `BLIT`, and the shell checks its read-back against those sums.
12. **Not OK:** queue a `FREE` per miss this call allocated (never a hit). Ring order runs it after the `DECODE` ends, even past a timeout, so no slot is reused while the job may still write it, and no partial pixels are left READY.
13. **`bcmjob_read()`:** per row, port reads (R2) from `ALLOC`'s address + y × pitch + x × 2, under the port lock per row. Check the epoch before and after (`OFF` if it moved).
14. **Traces:** `bcmjob decode len= n=`, `bcmjob decode done st= err= passes= bytes= ms=`, and `bcmjob fault op=0x30 why=busy|off|nojob|timeout|failed|refused|aborted`.
15. **Build glue:**
    - `bcmjob.h` now includes `jdec.h`, so every includer needs `bcmfw/jobs` on its path. `drift_jobs.o` has it (`-idirafter`); check `firmware.make`'s `bcmjob-*.o`, `tests/bcmjob/`, `devbcm.c` and `sim_bcm_job.c`.
    - The simulator renames the model's entry points (`sim_bcm_job.c`). Add `bcmjob_decode` and `bcmjob_read` to its list, with wrappers that sync the model's clock first. Until then the model's own definitions are linked under the real names, without the clock sync.

**As built (branch `p19-decode-driver`, 2026-10-24; under review by the driver's owner).** What the first implementation taught the contract:
- **Threads (decided: the split, 2026-10-24).** The art thread posts the call (`struct bjc_req` in uncached memory: the caller writes only the `a_*` words, the owner only the `o_*` words) and then only does pure checks, its pinned 8 KB piece buffer, the file's `start()`/`fetch()` when the owner asks, and its cancel poll, waiting on a semaphore (10 ms) and kicking the owner. The ring's owner does everything else in its passes (`bjc_apply()` on `SYS_BCMJOB_REQ`): `enter()`, the epoch, power, the ALLOCs/COMMITs, the DECODE record, the feed, the reply, the FREEs and `bcmjob_read`'s port reads into the caller's buffer. **Budget:** one 8 KB piece (about 0.73 ms by engine 2) or about 2 ms of read rows per owner pass, nothing while a frame is due (`bcmjob_frame_due_hook()`, set by the shell). Two exceptions still run inside a pass (the driver re-review, 2026-10-24, noted, not fixed): `enter()`'s HELLO (about 20 ms) on the first call of an epoch, and the restart paths after a cut; both are rare (once per epoch or per fault), not per piece. **Every end releases the art thread** (answer, cancel → ABORTED, deadline, cut, restart, epoch, suspend; an owner that never takes the call within `BJC_ACK_MS` → withdrawn, OFF). For 19-9 the call, result and read buffer need cross-core hand-offs (`xcore_publish`/`acquire`) and a compare-and-swap for the withdrawal. **Built in S10 (branch `p19-s10-handoff`, 2026-10-25):** see the next item; no compare-and-swap was needed.
- **The hand-off across cores (S10, as built).** `bcmjob-core.c` carries the state table; `host-driver.md` § "The decode hand-off across cores" has the rules.
  - **The call lives in the request record** (`struct bjc_call` inside `struct bjc_req`, so it is `XCORE_BSS` on the device), never in the caller's frame. The caller fills it before `a_seq` and copies `res` out after `o_seq`; the owner copies it once at the take and writes `ret`/`res` before `o_seq`.
  - **The piece buffer:** the caller pads the file's last word (the owner used to do this, which wrote into the caller's memory) and publishes the piece before `a_have`. Engine 2 reads it from memory; the CPU's copy reads it through the uncached alias. It is 16-byte aligned at both ends (`jd_dec_buf`).
  - **The read's `px`:** the caller publishes the span before the post. The owner writes rows through the uncached alias, except the span's first and last cache lines when they hold memory outside the read: those pixels go to the record's edge lines, and the caller copies them in after it acquires the span. The art builder's buffers need no alignment change.
  - **Withdraw:** the caller sets `a_quit`, then waits up to `BJC_ACK_MS` for `o_busy` to stop naming the call. The owner sets `o_busy` before it checks `a_quit` (both sides fence: Dekker), and checks `a_quit` again at every row. An answer that lands during the wait stands. No acknowledgement within the bound counts as **leaked**: TIMEOUT, traced `bcmjob withdraw unacked`, and the piece buffer stays pinned until the owner acknowledges (every call `BUSY` meanwhile).
  - **The hooks:** the owner (`struct bjc_owner`) gets `cross`, `publish`, `acquire` and `uncached`. While `cross()` is false (today: both on the CPU), none of this runs.
- **Engine 2 for ring pieces:** the LCD's 10 KB minimum (`LCD_E2_MIN`) would send every 8 KB piece by CPU, so the driver gets its own threshold (`lcd_e2_push_min`, `upload_any`), not a changed LCD threshold.
- **The 8 KB piece buffer** is `core_alloc`ed and pinned per decode; no room answers FAILED, `HP_E_NOMEM` (use the twin), after freeing the outputs.
- **Cancel before the job's first pass** is held until the pass is seen: the BCM resets `J_DEC_IN_WR` to idle when the job starts, so an earlier ABORT would be lost. A file larger than the ring always aborts on cancel ("passes remain" can't be known).
- **The ring once per epoch** can't see another scratch user free and reuse the slot within an epoch (none exists today); the model re-checks the slot.
- **Size (accepted, 2026-10-24):** driver + jref about 14.8 KB Thumb (`bcmjob-core.o` 10,487 B with the split); the brief's budget row says about 14 KB, decode being the BCM path for every cover; a loadable art builder is the follow-up after P19. Linker-droppable until the shell calls it.
- **Read-back under load is the weak spot:** a 142 × 142 box takes 0.55–0.86 s at 55–85 µs a word (hundreds of owner passes).

