# The display job interface ("JB"): contract rev 0.4 (0.5, the staged upload, in candidate images: §4)

**Names:** these documents use the technical names: PP5022 (its cores CPU and COP), BCM2722 ("the BCM"), WM8758. In chat with the owner they are PP, PP1, PP2, VC and DAC.

P19 contract, draft for the brief review (2026-10-23).
- **Constants:** `bcmfw/jobs.h` (shared by the BCM image, the PP driver, the reference executor and the tests).
- **Plan:** `display-offload-plan.md`.
- **Its memory:** `memory-contract.md`.
- **Its power states:** `power-contract.md`.

Everything here is **[ours]: a design**, except where a line is tagged otherwise. The silicon facts it rests on carry their own tags.

**On silicon so far** [silicon] (bench sitting 2, 2026-10-25 evening, build 799ff0f8d7, merged image `80bc8611`, `bench-20261025-2257`): the job self-test passes (`jobs: self-test rc 0`); `COMMIT` is accepted, with the F16w sum checked, for B5's window slots (31 of 31 pushes by DMA at pitches 320, 300 and 400: 5,947, 5,947 and 2,976 µs, about 186 ns a pixel) and for the UI drift's slot uploads (about 59–60 ms for a cover, 21.7 ms for a smaller one, all `rc=0`); the drift ran on the BCM through three BCM starts (`bcmjob up epoch=1`, `2`, `3`; `vpu=0x3 expect=0x3` each time) with no `fault` and no `FALLBACK` line in `ipodui.log`; drill 2's injected fault (`inject op=0x26 n=2`) was ridden out. Sitting 1's failures (`COMMIT` refused, `st=23`, at B5's step 5.4 and the drift's first upload) were the BCM's own halfword loads (`../../bcm2722-reference/quirks.md` #33), fixed in every image.

## 1. What it is

The shell (the front end) sends the BCM small **jobs** instead of pixels:
- **Drawing:** blit, fill, fade, average, scroll, and present a rectangle to the panel.
- **BCM-timed animations:** a slide, or a window panned across a picture.
- **Pictures:** held in **slots** the PP uploads once and names by a content key.

The BCM does the pixel work and drives the panel. The PP stays the reference, and a fallback for every job (§6).

Why:
- The PP's pixel work is the sustained cost while lit: the drift, the marquee, slides and glides (`display-offload-plan.md` §2).
- Every byte crossing the host port costs about 10.5 PP cycles per 16-bit transfer [silicon S8].
- The BCM is powered whenever the screen is lit anyway (`power-contract.md`).

## 2. Discovery and the handshake

1. The PP starts our image (`bcm_init`; the boot ladder, `pipeline.md`) and sends the video protocol's `HELLO` (`hostproto.md`).
2. If `HP_CAPS` has `HP_CAP_JOBS_2D`, the PP reads `HP_JOBS` (`0x138`, the protocol header's spare word): the job block's address.
3. It reads `J_VERSION`:
   - magic `'JB'`, **a different major: no jobs at all** (the CPU paths everywhere, logged; throughout this file, "CPU path" means the PP's software path, `bcmjob-cpu.c` over jref, run by whichever core issues the job: the COP once 19-9 moves the UI there, not necessarily the CPU core);
   - a higher minor only adds.
4. It reads `J_OPS`, one bit per opcode `0x20`–`0x3F`. **A job whose bit is clear is never sent**; the PP runs its CPU fallback. So an image can implement jobs one at a time, and the shell needs no change as they arrive.
5. It reads `J_EPOCH`, `J_MODE`, `J_SLOT_TAB`/`J_SLOT_N` and the free bytes. Slots from a previous epoch are gone.

**Why not `0x140–0x1DF`:** `hostproto.md` reserved that range for this header. Since then, the test and video images have filled it: the clock record and the idle counters at `0x140–0x14C`, the clock request `0x168`, the idle gate `0x16C`, the timing words `0x170–0x17C`, wait limits `0x180`, the clock-enable record `0x188–0x194`, DMA statistics `0x198–0x1BC`, and the probe, standby and step words `0x1C0–0x1DC` (`bcmfw/config.h`). The job block therefore lives where the image's memory map puts it (`memory-contract.md`: `0xDE000`), announced through `0x138`.

## 3. Transport, order and replies

- **The video protocol's rings and records,** unchanged: `op | n << 8 | tag << 16` and n payload words, at most 32. Jobs use opcodes `0x20`–`0x3F`.
- **One PP-side mutex:** the shell and the video player are different threads, and from P19's 19-9 on, on different cores (the UI and the ring on the COP, the video player on the CPU); both write the command ring, so the lock must work across cores (§6). If it's ever contended in practice, jobs get a second ring (`hostproto.md` §1).
- **In order.** The BCM runs jobs in ring order. A later job sees an earlier job's result.
- **Replies are opt-in:**
  - a job replies if its tag carries `J_TAG_REPLY`;
  - it always replies if it fails, with its status;
  - `FENCE` always replies.

  So the shell can queue a frame's jobs, end them with one `FENCE`, and wait once. One round trip is 6–7 µs [silicon S2], plus the PP's port writes: about 26 transfers for a short record (`hostproto.md` §8).
- **A full ring** (gap G1): `bcmjob_submit()` returns `BCMJOB_BUSY` at once and never blocks or queues on the PP. The caller takes its CPU path for that frame, or drops the frame. The ring is 2,048 words, about 200 drawing jobs.
- **Fences** carry a 16-bit sequence. `FENCE(seq)` retires every earlier job and posts `J_FENCE = seq` (encoded, as all shared indices: `HP_ENC`). The PP can then poll one word instead of reading the event ring.
- **Events:** `J_EV_ANIM` when a BCM-timed animation ends (its tag, frames shown, frames late), and `J_EV_EPOCH` when slots are lost.

## 4. Slots

A slot is a picture in the BCM's memory, in a **class** whose pool and budget are fixed by `memory-contract.md`.

1. **`ALLOC(class, w|h, fmt, key)`.** The key is the PP's content hash: the art cache's album key and size, a strip's text hash, and so on.
   - If the class already holds that key **in this epoch**, the reply says `J_ALLOC_HIT` and nothing needs uploading.
   - Otherwise it returns the slot, its address and its pitch.
   - **The address** (reply word 2, hit or not) is **the slot's first pixel as an address in the BCM's map**: its pool's base (`config.h` `BCMFW_JOBS_*`, each image its own) plus the slot's offset in the pool. That is where the PP's port writes go. One definition everywhere: jref adds a per-pool bus base (`jref_pool_addr()`, default 0) to the offset; `jobs_bcm.c` gives each pool its real address; the host model's pools default to the BCM's real addresses (`jmodel_cfg.pool_addr`, 0 for the defaults: the display-only image's when the model offers `DECODE`, which only that image does, else the merged image's; 0 for the SDRAM classes, which have none yet).
   - **The pitch** (reply word 3, bytes) is `jobs.h` `J_SLOT_PITCH` × 2: the width, except that `J_CLS_DRIFT` rows are the width rounded up to 16 pixels apart (`J_DRIFT_PITCH`, 304 for a 300-wide cover), so they start 32-byte aligned and reach the vector unit (§10, the cost audit). **The PP writes a slot row by row at its pitch**; a slot's bytes in its pool are pitch × h × 2.
   - `J_E_NOSLOT` when the pool is full: the PP frees the least recently used slot it owns, or uses its CPU path.
2. **The PP writes the pixels** straight to that address: by engine 2 for anything of 16 rows or more, about 11 MB/s with the issuing core 97 % free [silicon S37, measured with the CPU issuing; from the COP not yet measured], or by the CPU copy for small ones.
3. **`COMMIT(slot, A, B [, flags])`** marks it `READY` and replies the BCM's own `A, B`. The BCM always checks the slot's **F16w** first (`J_E_SUM` on a mismatch). That's cheap insurance across the port, which has torn words before (`data-paths.md` D7).
   - **F16w** (`jobs.h` `j_f16w()`, rev 0.2; `algorithms.md` §7): over the slot's w × h pixels as 16-bit values h_0 … h_N−1, row by row from row 0, each row from x = 0. **The padding of a wider pitch (`J_CLS_DRIFT`'s 304 for a 300-wide cover) is not summed.** A = Σ h_i mod 2³²; B = Σ (N − i)·h_i mod 2³², the running sum of A.
   - It catches every 1- and 2-pixel error in a slot below 2¹⁷ pixels, swapped pixels, shifted rows and 0x0000 against 0xFFFF. It runs incrementally, so the PP can sum each row as it writes it, or once at art-build time and keep it with the art.
   - An all-black (or empty) slot really sums to 0, 0, and is checked like any other, so a torn upload of a black cover is caught. The only way to skip the check is an explicit 4th word with `J_COMMIT_NOCHECK` (1): for a slot whose bytes aren't pixels, such as a JPEG source in a decode ring. The reply still carries the BCM's sums.
   - The test vectors are in `jobs.h` (`J_F16W_VECTORS`, `J_F16W_SLOT_*`, `J_F16W_WHITE_*`): `jtest.c` checks `j_f16w()` and jref's `COMMIT` against them, `jobs_image_test.py` the image's.
   - Rev 0.1's `COMMIT(slot, fnv)` (two words) is refused, `HP_E_INVAL`.
4. Jobs read only `READY` slots of the current epoch (`J_E_SLOT` otherwise).
5. **`FREE(slot)`** returns it to its pool.

### The staged upload (rev 0.5; sim only since 2026-10-25, open question #67)

**Withdrawn from the images (2026-10-25).** The Port write check (`bench-20261025-1819`) found the cause of the `COMMIT` failures: the BCM's own F16w was wrong, not the upload: `jref_f16w` reads `px[1..7]` of every 8 with the 32-bit `0x29`/o1 halfword load, which **sign-extends** on silicon (`../../bcm2722-reference/quirks.md` #33). Every pixel of `0x8000` and up counted as itself − 65,536; the port writes into every pool read back right (CR1: control 0 wrong, pools 0 of 7 wrong). With the halfword loads corrected (vc2cc, every image regenerated: `merged-image-batch.md`), staging has no reason; its candidate images and bench row CR2 are gone, and `BCMFW_JOBS_STAGED` stays a sim-only option. **Sitting 2 confirmed it** [silicon]: with the corrected images the direct upload's `COMMIT` passes (B5 31 of 31; the drift on the BCM; the note at the top of this file). What follows is the design as it was.

**Why (as reasoned before the Port write check).** On silicon (2026-10-25, both images, both PP write paths) every `COMMIT` failed. The PP's port writes are proven after the start only into the rings (`0xD0000`/`0xD2000`), the host frame (`0xE0000–0x1057FF`) and SDRAM (host `0xC000_0000`) [silicon S12, S28–S34]; every slot pool lies elsewhere in SRAM, mostly below `0xD0000`, where a host write after the start has never been shown [hypothesis: the cause]. The BCM's own DMA moves SRAM ↔ SDRAM with 0 wrong words [silicon PW8]. Apple never checksums an upload and never writes arbitrary SRAM at run time [apple]. So the staged form moves the PP's writes into a proven region and lets the BCM move the bytes the rest of the way, then checks them where the jobs read them. Whichever cause holds (the host write, or the BCM's sum), the reply words now say which.

1. **Discovery.** `J_CAPS` (job block `0x4C`) = `J_CAPS_MAGIC` (`'CA' << 16`) | `J_CAP_STAGED`. Older images never write the word, so it counts only with the magic. `J_CAP_SDRAM_FAIL` is set if the window's SDRAM didn't come up.
2. **`ALLOC(class, w|h, fmt, key lo, key hi, J_ALLOC_F_STAGE)`**: an optional sixth word. An image with the form answers with **the window's address as the host port writes it** (`0xC03A_0000`, `config.h` `BCMFW_JOBS_STAGE_HOST`), the slot's own pitch, and `J_ALLOC_STAGED` (`0x2`) in the flags word. It answers as rev 0.4 (the slot's own address, no flag) on a hit, for a slot larger than the window (192 KB), or when SDRAM won't come up. An image without the form ignores the sixth word. **So the PP's path is the same either way: it writes where `ALLOC` said, at the pitch it said.** The first staged `ALLOC` brings SDRAM up (`video.c`'s sequence, `data-paths.md` R4; at most 4 attempts of 5 ms, `J_T_STAGE_UP`).
3. **The window holds one upload**: the last staged `ALLOC`'s slot, until its `COMMIT`, its `FREE`, or an epoch change (`RESET`, a mode change, a restart). A `COMMIT` of a slot whose window was taken since is checked as its slot stands (`J_E_SUM`, how = `J_HOW_DIRECT`). The PP's driver uploads one slot at a time (`ALLOC` waits, then the rows, then `COMMIT`, then the next `ALLOC`, in ring order), so this never happens in use.
4. **`COMMIT`** of the window's slot first moves the window into the slot: the display-only image by DMA channel 7 sub-channel 1 with Apple's `dma_memcpy` words (PW8's: config `0x0000_E802`, transfer information `0x0102_5151`, 32-byte units, status bit 14), in 16 KB pieces (PW8's proven size), each bounded at 100 ms; after a timeout the core copies every word (PW8's other 0-wrong copy). The merged image has no room for the DMA's code below its arena, so its core copies. Then the slot's F16w is checked as before.
5. **The replies.** On success (with `J_TAG_REPLY`): `A, B, how` (`J_HOW_DIRECT` 0, `J_HOW_DMA` 1, `J_HOW_CORE` 2). On `J_E_SUM`: `A, B` (the BCM's sums of the slot), the slot's first word (pixels 0 and 1), `how`, and the window's B (0 when direct; 0 in the merged image, which has no room for its sum). **Reading them:** the BCM's sums equal the PP's but the reply still failed: impossible (the check is the same compare); the window's B equal to the PP's B and the slot's not: the move failed; the window's B different too: the bytes never arrived whole over the port; the first word right but the sums different: suspect the BCM's sum itself.
6. **The PP's log** (`bcmjob-core.c`): every failed job's line now has its status (`bcmjob fault op=0x22 why=failed st=23`), and a `J_E_SUM` a second line `bcmjob sum slot=… pp=A,B bcm=A,B w0=… how=… win=…`; discovery logs `bcmjob caps=0x1 staged`; a staged request answered without the window logs `bcmjob alloc slot=… why=not staged`.
7. **Cost** [sim; rates silicon PW8]: a cover's move 6.4 ms by DMA or 11.5 ms by the core at 27 MHz, once per upload, beside its 11.7 ms sum; SDRAM refreshing from the first upload (its power unmeasured, `memory-contract.md` §3). Image room: the display-only candidate's decode work area starts 2 KB later (`0xCC00`, still 234,496 B ≥ `J_DEC_WORK_MIN`); the merged candidate moved the self-test's 508 B snapshot to `0xDF500` and ends 4 B below its arena.
8. **Where it is:** only in the candidate images (`BCMFW_JOBS_STAGED`: `mkdevtest.py` `jobs_staged`, `mkvideo.py` `bcmvideo-jobs-staged.bin`); the live images keep rev 0.4 and their FNVs. The bookkeeping is `bcmfw/jobs/jstage.h`, shared by the image and the host test's chip. Bench row CR2 proves it (`bench-p19.md`).
9. **Open before it ships:** a warm swap from the display-only candidate to the video image after a staged upload hands SDRAM over already up, and the video image's `OPEN` runs its bring-up again on a live controller [unknown on silicon; CR2 cuts the power between images, and B13 never stages]. A shipped form should tell the video image (a header word) or idle SDRAM before the park.

**The epoch** goes up whenever slot contents are lost:
- every start (a power cut loses all BCM memory [silicon]);
- a video `OPEN` (the video session takes the arena);
- a Class C game session;
- `RESET`.

The PP compares `J_EPOCH` before relying on slots, and re-uploads by key after a change. **Pixels are always re-creatable on the PP side** (the art cache on disk, the shell's renderers). A slot is a cache, never the only copy.

## 5. The jobs (rev 0.4)

| Op | Job | Result | Notes |
|---|---|---|---|
| `0x20` | `FENCE seq` | `seq` | always replies |
| `0x21` | `ALLOC` | slot, address, pitch, flags | §4 |
| `0x22` | `COMMIT slot, A, B` | A, B | F16w (§4); 0, 0: not checked |
| `0x23` | `FREE` | | |
| `0x24` | `BLIT src rect → dst point` | | optional key colour (`J_BLIT_KEY`) |
| `0x25` | `FILL dst rect, colour` | | |
| `0x26` | `FADE dst ← a × (1 − w) + b × w` | | **the shell's `xfade_rect()` arithmetic exactly** (weight 1–256; 256 is a copy of b). Optional `J_FADE_SHADE_B \| n` and n alpha bytes: b's first n columns shaded first, `ui_panel_fade(raw)` exactly |
| `0x27` | `AVG dst ← (a + b) / 2` | | the shell's `AVG2` exactly (the drift's half step) |
| `0x28` | `SCROLL surf rect by dy` | | rows move; the uncovered rows keep their pixels (the PP fills them) |
| `0x29` | `PRESENT surf rect [TE] [WAIT]` | µs waited | the rect to the panel. `J_PRES_TE` starts on the panel's next tearing-effect edge (`lcd.md`, "Tearing"; stage 3d measures the period) |
| `0x2A` | `ANIM SLIDE / PAN` | event at the end | timed by the BCM's 1 MHz counter at 30 fps; drawn into `J_SURF_FRONT`, then its rect pushed. Rev 0.3: PAN's stepped form (`J_PAN_STEP`), the marquee: whole steps on its own grid, a pause on a rest window (below, "Shell recipe: the marquee"). **Rev 0.4:** a SLIDE's `from` may be `J_SURF_FRONT` itself (the page on the panel), its offsets never decrease, at most 48 (§12) |
| `0x2B` | `ANIM_STOP` | frames shown | one animation runs at a time: a new `ANIM` replaces the running one, which sends no event. The panel keeps the last frame |
| `0x2D` | `SHADE surf rect, alpha[w]` | | column c darkened by alpha[c]/256 (w ≤ 32), **`ui_shadow_apply()` exactly**: the split shadow (8b-3) |
| `0x2C` | `STATS` | counts | |
| `0x2F` | `RESET` | | epoch + 1 |
| `0x30–0x32` | `DECODE` (JPEG), `GLYPHS`, `TRIS` | | **reserved**: plan items 4, 7 and 5. Numbers fixed now |
| `0x33` | `SPECTRUM src, n \| nb << 16, edges[nb + 1]` | nb band levels | **proposed, sim only** (rev 0.3 + proposed; `J_OPS` bit 19; #126, `visualizer.md`): the Hann-windowed spectrum of n = 256 or 512 samples in split order (evens, then odds) from a slot the PP rewrites each frame; band b's level is 16 log2 of its largest bin power (0–511, 0.19 dB a step), **`jviz.h` exactly**. The PP keeps the smoothing, peaks, auto-gain and beat |
| `0x34` | `BARS dst rect, n \| barw \| gap \| cap_h, bot \| top, cap \| bg, flags, heights[n], peaks[n]` | | **proposed, sim only** (`J_OPS` bit 20): n ≤ 32 bars centred in the rect, a vertical gradient, optional background, peak caps and rounded tops, upside down with `J_BARS_FLIP` (`0x8`, only where `J_VPU_FLIP` is set: Mirror's reflection, `visualizer.md` §12.5) (`J_BARS_*`), **`jviz_bars_draw()` exactly** |
| `0x35` | `OVERLAY words, A, B` | | **proposed, sim only** (`J_OPS` bit 21; the display-only image): the overlay the PP wrote into the image's overlay region (`J_OVL`), checked (length, F16w, header) and started; its ops' `J_OPS` bits set until the epoch moves. Below, "Overlays" |
| `0x36` | `POLYLINE dst rect, src, off \| step << 16, n \| flags << 16, gain \| half << 16, rgb \| width << 16` | | **proposed, sim only** (`J_OPS` bit 22, from the visualizer's overlay; #126, `visualizer.md` §13): the Scope's line. n (2–1024) points spread across the rect (w ≤ 320; point i at column i (w − 1) / (n − 1)), point i's row from element off + i·step of slot `src`: int16 samples (row h/2 − (v·gain >> 16), gain Q16) or bytes (`J_POLY_U8`: the row itself), clamped; with `half`, the elements are a split-order window (element e at e/2 if even, else half + e/2: `SPECTRUM`'s slot as it stands). The connected line under a width × width square brush (1–3). **`jviz_poly_draw()` exactly**. 8 words, no reply |
| `0x37` | `SEGMENTS dst rect, cx \| cy << 16, n \| a0 << 8 \| astep << 16 \| width << 24 \| flags << 28, r0 \| lmax << 8 \| rring << 16 \| capd << 24 \| ringd << 28, c0 \| c1 << 16, cap \| ring << 16, lengths[n], peaks[n]` | | **proposed, sim only** (`J_OPS` bit 23): the Radial's rays. n ≤ 48 segments from the centre (signed, rect coordinates), segment i at angle index (a0 + i·astep) mod 48 (0 up, clockwise, 7.5° a step, a built-in Q14 sin table), its pixels r0 … r0 + min(len, lmax) − 1 out under a width-square brush, the gradient c0 → c1 over lmax (the bars' `jviz_mix565`); `J_SEG_RING` a dot (ringd) at rring on every angle first, `J_SEG_CAP` a dot (capd) at r0 + min(peak, lmax) last, `J_SEG_ROUND` dots of 3+ without corners; clipped to the rect. **`jviz_segs_draw()` exactly**. 48 segments: 32 words, no reply |
| `0x38` | `PALROW dst rect, lut, nb, levels[nb]` | | **proposed, sim only** (`J_OPS` bit 24): the Spectrogram's newest rows (after a `SCROLL`): nb (≤ 96, ≤ w) cells across every row of the rect, cell b columns b·w/nb … (b + 1)·w/nb − 1 in `lut[level b]`, `lut` a READY slot of 256 RGB565 entries uploaded once an epoch. **`jviz_pal_draw()` exactly**. No reply |

**Surfaces:** a slot, `J_SURF_FRONT` (the frame the panel shows next: today's framebuffer at `0xE0000`) or `J_SURF_BACK` (the second frame, #76). The panel keeps its own picture: nothing reaches the glass until a `PRESENT` (or an animation) pushes it.

**What each job means is defined by the reference executor** (`bcmfw/jobs/`, item 5 of the P19 prep), which is also the CPU fallback. The BCM's implementation must match it **bit for bit** in vc2sim before it ships, the same gate the drift kernels passed (`offload.md` §8a).

### Overlays (proposed, sim only; #126, 2026-10-24)

An image can take ops it has no room for resident as an **overlay**: a second program, linked at the image's overlay region against the image's own map, which the PP writes into the region and starts with `OVERLAY`. The display-only image has one region and one overlay so far, the visualizer's (`SPECTRUM`, `BARS`; `visualizer.md` §8). Everything here is [sim].

- **Discovery.** `J_OPS` bit 21 (`OVERLAY`). The job block's `J_OVL` (`0x48`) is the region, `base | size >> 12` (both whole 4 KB), valid only with bit 21 set. The display-only image: `0x13B005`, 20 KB at `0x13B000`. The merged image clears bit 21 today; with `BCMFW_JOBS_MERGED_OVL` (next re-verify batch) it offers it, region `0x12B000–0x133FFF` (below its parked AAC tables), for DECODE (below).
- **The image.** A build writes it with the image (`bcmfw/build.py --overlay`: `viz-ovl.bin`, a whole number of words). Its first words are the header (`jref.h` `struct jref_ovl`): `J_OVL_MAGIC` (`'OVL1'`), the ABI word, the base, the load end, the `.bss` end, then its `init` and `exec` entries. The PP checks the magic and that the base and the `.bss` end lie inside `J_OVL` before it writes anything.
- **The load.**
  1. The PP writes the image at the region's base through the port, as it writes a slot's pixels (the CPU's copy or engine 2), **only while no overlay op is queued**: at once after an epoch change (its ops are already gone), or after a `FENCE` retires. The region is the PP's to write then; nothing on the BCM touches it.
  2. `OVERLAY words, A, B`: A and B are the image's F16w as halfwords (`j_f16w`, §4). The BCM drops the overlay's ops first, then checks, in order: the length (7 header words up to the region; else `HP_E_INVAL`), the sum (`J_E_SUM`: a torn or corrupt upload), the header (`HP_E_VERSION`: the ABI word is not this image's, or the load end isn't `base + 4 × words`, a truncated image).
  3. Then it runs the overlay's `init`. The visualizer's clears its `.bss`, runs the FFT's and the vector bars' start-up checks (`J_VPU_FFT`, `J_VPU_BARS`; about 0.61 M instructions together, 23 ms at 27 MHz; `visualizer.md` §5.1), sets `J_VPU_FLIP` (its `BARS` takes `J_BARS_FLIP`) and offers `SPECTRUM` and `BARS`. The BCM sets the `J_OPS` bits of the ops the overlay offers that the image itself lacks, and replies `HP_OK` (no payload: the PP reads `J_OPS`).
- **Dispatch.** Every op the image doesn't define reaches the stub (jref's `JREF_EXT`, `jref_ext()`): an overlay op whose bit is set runs in the overlay, as one of jref's own (counted in `J_DONE`, replies as usual); anything else is `HP_E_UNKNOWN`, as before.
- **Optional ops (the line ops, 2026-10-26).** The visualizer's overlay also offers `POLYLINE`, `SEGMENTS` and `PALROW`, each only if its start-up scene sums right (`viz_vpu.c` `viz_draw_init`: the C on the BCM against the Mac's build of the same C), so one failed check takes one op off and leaves the overlay loaded. The PP never reloads for them (`bcmjob-core.c` `struct ovl`: `want` = `SPECTRUM | BARS` decides a load, `opt` = the line ops come with it). The driver's ok line ends `lines=PSR` (a `-` for each op not offered); `HP_E_UNKNOWN` for any of the five drops them all, as for `SPECTRUM`.
- **Per-op cost [sim]:** a 1 × 1 `FILL`, ring to reply, is 403 instructions (0.015 ms at 27 MHz), so a frame of a few jobs needs no batching. The host model's 2 ms a draw job (`jmodel.h` `JM_COST_DRAW`) is a deadline-scaled allowance, not the BCM's cost.
- **Lifetime.** The overlay's ops last until the epoch moves: `RESET`, a mode change, a restart (which rewrites `J_OPS`). **The region is a cache, as the slots are**: after an epoch change the PP loads the overlay again when it next needs it, as it re-uploads its slots. A failed load leaves no overlay ops.
- **The ABI word** is vc2cc's `__vc2_abi` (`compiler.md`, "Fixed-base links"): the CRC-32 of `J_OVL_ABI` and the addresses, in the image, of what the overlay imports (`build.py` `OVL_IMPORTS`: `jref_f16w`, `jv_store16`). So an overlay loads only into the image build it was linked against: the PP ships the two from one build, and an image rebuilt with any import moved refuses the old overlay (`HP_E_VERSION`) instead of running it. The overlay shares `struct jref` with the image (it is compiled with the image's flags); `J_OVL_ABI` moves when that struct, the header or the import list changes.
- **The fallback** is the ops' own: a PP that can't load the overlay (bit 21 clear, a refused load) runs `SPECTRUM` and `BARS` on its CPU path (§7), the same levels and pixels.
- **Beside DECODE.** The decode work area (`0xC400–0x45FFF`) and the region are apart (`build.py` checks the layout), and jobs run one at a time from the ring, so an overlay op never runs inside a `DECODE` (§6); the region keeps its bytes through one (`decode_image_test.py` "DECODE beside the overlay").
- **The PP's side (built, #126 `p19-viz-device`):** `bcmjob-core.c` `bjc_ovl_begin/piece/end` and `bcmjob-dev.c` `bcmjob_viz_ops()`: the file (`viz-ovl.bin` in the system folder) checked against the record of the build that embeds the display-only image (size, FNV-1a, ABI word), the header against `J_OVL`, the running image's own copy of the ABI word read in place (the stub's immediate, at the recorded offset), a `FENCE` if jobs are in flight, the bytes streamed through the caller's scratch (engine 2 for whole pieces), `OVERLAY`, then `J_OPS` and `J_VPU` read back. Refusals log `bcmjob overlay refused why=nofile|read|file|region|image|sum|version|wire`; success `bcmjob overlay ok ops=... vpu=... fft=vector|c bars=vector|c[ flip]` (`bcmjob_bars_flip()` then answers from `J_VPU_FLIP`). `J_E_SUM` retries once in the epoch; `HP_E_VERSION` and a bad file never again this boot. An epoch change, a `RESET` the PP puts, or `HP_E_UNKNOWN` for `SPECTRUM`/`BARS` (the BCM dropped them before the PP drained its event; never counted) drops the ops and makes the load due again. Deadline `J_T_OVERLAY` (80 ms, plus a fence's base). **No call waits for the BCM (0.8.0, sitting 4c):** the `FENCE` and `OVERLAY` are posted and their answers polled on the owner's later passes (`BJC_OVL_AGAIN`, `bcmjob-core.h`; the other instance's ops are dropped before the `FENCE`, so none is queued behind it), within the same deadlines and with the same lines. The recipe: `data-paths.md` R14. Packaging: `make zip` ships `viz-ovl.bin` only beside the image it pairs with (devtest shell builds), and `bcmfw/ovlpair.py` fails the zip on any mismatch. Tests: `tests/bcmjob` `t_overlay`, `tests/ovlpair`.
- **The PP keeps one overlay's loader state in one struct** (`bcmjob-core.c` `struct ovl`: the ops it may add and those it added, the region's base and size, the file's words, tries, a refusal for the boot, the build's record of the file), so a second region (another image's overlay) is a second instance.
- **Cost in the image:** the stub, 288 B (`jobs_bcm.c` `jref_ext`, `ovl_drop`, the region's surface, the `J_OVL` word, jref's ext path). Tests: `jobs_image_test.py` "overlay" (refused loads; `SPECTRUM`/`BARS` bit-exact against `jviz.c` on the Mac and against the `BCMFW_VIZ` image, case for case; the epoch).
- **The merged image's overlay: DECODE** (built 2026-10-26, sim; MERGED IMAGE, the next re-verify batch, `config.h` `BCMFW_JOBS_MERGED_OVL`, 0 today so the verified `80bc8611` builds unchanged; `album-render.md` §5.3). Region `0x12B000`, 36 KB (`J_OVL` `0x12B009`); the overlay is `video/dec_ovl.c` (`dec-ovl.bin`, 23,820 B; imports `bcmfw_service`, `jv_store16`, `_12`, `_8`: `build.py` `DEC_OVL_IMPORTS`, the merged image's export list). Its `init` sets jref's own DECODE hook (the merged image has jref's DECODE op, hook unset), `J_OPS` bit 16, and `J_CAPS` = `J_CAPS_MAGIC | J_CAP_DEC_LENT`; `ovl_drop()` clears all three. **Its wire, two additions:** the work area is a slot the PP lends, `J_CLS_DRIFT` 300×300 keyed `J_DEC_WORK_KEY` (`'WORK'`, hi 0), ALLOCed before the DECODE and FREEd behind it (none lent: `HP_E_NOMEM`); and for a ring source the record's `len` is clipped to the ring (jref's bound) and the file's length goes in the job block's **`J_DEC_LEN`** (`0x50`, plain, written before the record; 0: the source slot holds the whole file). The display-only image ignores both. Resident cost 640 B (the stub and S3c's service), paid by moving AAC's `spec_data` (2 KB) to the far bss. Tests: `decode_image_test.py` "merged image: DECODE as an overlay" (13 cases, golden).

### The drift as jobs (review H2)

Since 8b-3, every write into the panel rectangle also re-darkens its 10 shadow columns (`ui_shadow_apply`, `material_data.h`'s alpha table). The drift's pixel operations map one to one:

| The shell (`preview.c`) | Jobs | Pinned by |
|---|---|---|
| `pan_to()`: `blit_window()` then `shadow_lcd()` | `BLIT` from the cover slot, then `SHADE` | `jtest`: SHADE equals `ui_shadow_apply` |
| `half_window()`: `AVG2` of two framings, then the shadow | `AVG`, then `SHADE` | `jtest`: equals `half_window` |
| `ui_panel_fade(src, …, raw)`: fade past the shadow, shaded source columns faded in | `FADE` with `J_FADE_SHADE_B \| 10` | `jtest`: equals it at 18 weights |
| `ui_panel_fade(…, !raw)` | `FADE` | `jtest`: equals `xfade_rect` at all 256 weights |
| `ui_panel_compose()` (a key drawn, then shadowed) | the PP draws, uploads to a slot, then `BLIT` + `SHADE`, or keeps today's push | |

**The drift's policy stays on the PP, with the UI (the COP from 19-9):** `nav_core.c` (the cycle, P16's hold-still, the books set, the slot-2 swap, #92's bank, the half-step guard). It only decides *which* jobs to send, so none of it changes the pixels the jobs make.

**One definition (G5, widened):** `jmix.h` now also holds the shade. The shell's `ui_shadow_apply()`, `xfade_rect()` and `AVG2` call it, so the CPU frames and the BCM's can't drift apart.

**Bench B3 is re-specified.** Stage 5b's port (`bcm-devtest-core.c`, 2026-10-12) predates 8b-3's shadow, P16's hold and #92's bank, so its B pixels are no longer today's drift. B3 runs **the merged image's jobs driven by the real shell** (the P19 19-D1 build, with a developer A/B switch: jobs or the CPU path). Parity is checked by the drift's `tests/ui` scripts, which must produce identical goldens on both backends in the simulator, the model running the executor. On the device it's the same A/B with the power log.

### Shell recipe: the marquee (19-5, rev 0.3)

The CPU renders a long title's label once (F2) and uploads it into a `J_CLS_STRIP` slot. `ANIM PAN`'s stepped form then moves a window over it, timed by the BCM. While it runs the shell has no marquee wakeups at all: that is the saving (about 5 % of the CPU while a long title is selected and lit [estimate, the brief], to about 0).

**What F2 does** (`apps/ipodui/list.c` `ui_list_tick`/`mq_step`; Now Playing's `marquee_step` is the same rule):
- the selection settles; the label rests for `MQ_DELAY` (1 s);
- then `mq_x` grows by `MQ_STEP_PX` (2 px) every `MQ_STEP_TICKS` (50 ms);
- when `mq_x` reaches the lap L (the label's width + `MQ_GAP`, 40 px), it goes back to 0 and rests for `MQ_PAUSE` (1 s), then steps again;
- at rest (`mq_x` 0) the list shows its row as drawn, the label ellipsized; Now Playing shows the title's head (offset 0 of its strip).

So a lap lasts 1,000 + (K − 1) × 50 ms, where K × 2 is the first multiple of 2 at or past L.

**The stepped PAN** (`jobs.h` `J_PAN_STEP`) is that rule, word for word:

| Word | The marquee's value |
|---|---|
| kind | `J_ANIM_PAN` |
| slot | the strip |
| x0\|y0 | the lap's start in the slot (tw, 0) |
| x1\|y1 | x0 + L (y1 unused) |
| w\|h | the text cell: tw × ch |
| dst xy | the cell on the screen |
| period | the step: 50 (ms, below 65,536) |
| loop | `J_PAN_STEP` \| 1 (laps until `ANIM_STOP`; `J_PAN_STEP` alone: one lap, ending at rest) |
| step px \| pause ms << 16 | 2 \| 1000 << 16 |
| rest x\|y | the rest window: (0, 0), the cell at rest |
| start ms | where the first frame falls in the timeline: 1,000 when sent at F2's first step (below) |

At t ms into a lap: the rest window while t < pause, then step k = (t − pause) / 50 + 1 at x0 + 2k, for k = 1 … K − 1. t is start plus the time since the job ran. Frames come every 50 ms on that grid, so each step is drawn once.

**The slot** (W × ch, `J_CLS_STRIP`):
- **Columns [0, tw):** the cell at rest, copied from the PP's framebuffer: the selected row as F2 last drew it (bar, ellipsized label).
- **Columns [tw, tw + L + tw):** F2's own strip, rows [ty, y1) of `mq_step`'s: the bar, the label at 0 and again at L. `mq_step` renders it in LCD-wide segments; render them into a W-pitch buffer instead (each segment's `fb_ptr` = buffer + tw + k × 320, stride W).
- **W:** tw + L + tw rounded up to 16 px. A strip's pitch is its width; rounded, its rows start 32-byte aligned and the frames reach the vector unit: 0.26 ms a frame against 0.58 ms in C [sim, the cost audit]. The extra columns are never shown.
- **Now Playing:** its rest is the head itself, so there is no rest cell: x0 = 0 and rest = (0, 0), and the strip is its 1,392-px marquee image (already a multiple of 16).
- **The key:** a hash of the label, tw, ch, the font and the bar's colours. Selecting the same title again is then an `ALLOC` hit, with no upload.
- **Budget:** the list's longest label (`LABEL_MAX` 96) makes about 1,000 × 18 × 2 = 36 KB; Now Playing's strip is 50 KB. The merged image's strip pool is 128 KB (`memory-contract.md` §4).

**The sequence.** For a selected label wider than its cell (`mq_w` > 0), while lit:
1. **When.** At F2's first step (`mq_next` due, `mq_x` 0), instead of `mq_step`, if `bcmjob_have(J_OP_ANIM)` and `bcmjob_have(J_OP_ANIM_STOP)` hold and the image's minor is 3 or more. **The scrollbar:** `mq_step` paints the bar over the cell, so only while the bar is hidden or clear of the cell (it appears only with list motion, which stops the pan anyway, and hides at settle + 1 s, F2's first step: hide first).
2. **The slot:** `bcmjob_slot(J_CLS_STRIP, W, ch, key)`, then `bcmjob_upload()`, which is skipped on a hit. Only the first step renders the strip, as F2's does; a selection that never waits a whole second uploads nothing.
3. **The animation:** `bcmjob_submit(J_OP_ANIM, tag, …, J_PAN_ARGS)` with start = 1,000. The BCM's first frame is step 1, as F2's. Record the time sent and start; the phase is needed only to take over (below).
4. **While it runs:** the list's tick returns no marquee deadline (`TIMEOUT_BLOCK` for it), so there are no wakeups, no rendering and no pushes. **The BCM owns the cell rect:** the shell pushes nothing over it without stopping the pan first.
5. **Stopping** (selection change, a glide or a jump, the list leaving the screen or a slide over it, any redraw of the selected row's band such as art arriving or the bar over the cell, backlight off, sleep, the USB screen):
   - `bcmjob_submit(J_OP_ANIM_STOP, tag, &tag, 1)`, `bcmjob_fence()`, `bcmjob_wait(seq, J_T_FENCE_BASE + J_T_SMALL)`;
   - then the shell's own drawing, as today. The fence matters: a push (the LCD contract) and the ring aren't ordered, so without it a frame drawn after the push would overwrite it.
   - The panel keeps the last frame until the shell pushes.
   - **Backlight off:** F2 parks at 0 (`redraw_selected`); do the same after the stop. The BCM is cut when dark anyway (`power-contract.md`), so a stop that answers `OFF` is fine: the restart's first frame is the PP's, the row at rest.
   - **A stop that keeps the selection** (a band redraw): push first, then send the `ANIM` again with start = the current phase. Never the reverse.
6. **Faults:** every one ends in F2, from the current phase:
   - **`OFF` or `NOJOB` at the submit:** F2 for this selection.
   - **`BUSY`** (a full ring): F2 draws this step; try again at the next step, with start = the phase.
   - **`FAILED`:**
     - an `ALLOC` with `J_E_NOSLOT`: free the shell's previous marquee slot and try once more;
     - `J_E_SUM`: `bcmjob_upload()` uploads once more;
     - anything else: F2 for this selection.
   - **`bcmjob_errors()` up after the `ANIM`'s fence** (a refused record): F2.
   - **An epoch change while it runs** (`bcmjob_on_epoch`: a cut, a restart, a developer stage):
     - the animation is gone with the epoch;
     - the restart shows the PP's framebuffer, the row at rest;
     - the shell forgets the slot and takes over with F2 at the phase;
     - the next selection uploads again by key.
   - **A `TIMEOUT` on the stop's wait:** `bcmjob_resync()` of the row band (review M3), then the slow-job rule.
7. **The phase and `mq_x`** (one definition: `jref.c`):
   - phase u = (start + now − sent) mod lap;
   - u < 1,000: `mq_x` = 0, the next step 1,000 − u from now;
   - else `mq_x` = 2 × ((u − 1,000) / 50 + 1), the next step 50 − (u − 1,000) mod 50 from now.

   Taking over from F2 mid-lap goes the other way: start = 1,000 + (`mq_x` / 2 − 1) × 50 at a step's moment, or start = the time already rested, while at rest.

**Identical to F2:**
- **Pixels:** the slot holds F2's own pixels and the pan copies them unchanged into the cell's rows. Nothing else on the screen is touched: the frame's present is widened to whole pixel pairs, so the LCD DMA takes the list's cell at x 5. The column either side is what the shell pushed.
- **Offsets and times:** the same offsets in the same order, at the same nominal times.
- **What differs:** F2's steps are ticks (`mq_next = now + 5`), so a late tick delays the rest of the lap. The BCM's steps are an exact 50 ms grid from the `ANIM`, so it never drifts.
- **Checks:**
  - `jtest`: every frame of three laps against F2's timing, for laps odd and even;
  - `jmtest`: the model's panel against F2, through the API the simulator wraps, so the simulator's shell (later) gets the same frames on both backends;
  - `jobs_image_test.py`: the BCM's frames against the window, in vc2sim.

**Cost** [sim, estimate]:
- **The BCM:** about 4,640 instructions a frame, 0.26 ms at 27 MHz. At 20 frames a second that is 0.5 % of the BCM.
- **The PP, per selection:** one upload (about 36 KB at most, about 3 ms by engine 2 [silicon S37's rate]), then three records.
- **The PP, per step:** nothing. F2 renders the window and pushes a 320 × 25 band (8,000 pixels through the port, about 10.5 cycles a transfer [silicon S8]).

**On silicon:** nothing new to bench before the shell uses it. The frames run the kernels the other copies use (`jv_group16`, `jv_gather16` behind the lane shift's self-check), and the present is the LCD DMA's 2-D form (the cost audit's "PRESENT the panel"). Its first device check is the shell's: the marquee on the merged image with the developer A/B switch (jobs or the CPU path), as B3 is for the drift.

**The shell as built** (2026-10-25, branch `p19-6shell`; `apps/ipodui/mq_jobs.{c,h}`, `screens/nowplaying.c`): Now Playing's title only (the list's marquee stays F2 on the CPU). Sent when the developer switch says jobs (the drift's A/B row: auto is the device's verified image, never the simulator's model unless a script asks), ANIM, ANIM_STOP, ALLOC and FENCE are offered, `bcmjob_minor()` (new: J_VERSION's minor read at discovery) is 3 or more, and no DECODE is in flight. The start waits for its own FENCE (25 ms at most, once per selection) to see a refused record. Stops: a band over the title, a full draw, a body slide, dark, a new title, leaving, and `ui_fb_settle()` over the window (which also writes the phase's frame into the framebuffer, so a capture is the glass). Deviation: a BUSY start is retried at the next lap's first step, not the next step. Checked [sim]: `ipodui-p19-mq-parity` (187 steps, every one equal to F2's at the same phase), `-mq-pan` (no title pushes while it runs; the slot kept: no second upload), `-mq-cpu` (minor 2 and no ANIM: F2).

## 6. Deadlines, liveness and failure

**Deadlines** (the PP's waits, `jobs.h` `J_T_*`):

| Wait | Deadline |
|---|---|
| Small jobs (`ALLOC`, `FREE`, `STATS`, `ANIM`) | 5 ms |
| `COMMIT` | `J_T_COMMIT(bytes)`: 5 ms plus 1 ms per started 2 KB of the slot. Its F16w check reads the whole slot, 11.7 ms at 27 MHz for a 300 × 300 cover [sim, §10's audit]; the deadline (a cover: 94 ms) was set for FNV-1a's 40 ms and is kept |
| `DECODE` | `J_T_DECODE`, 2 s per started MB of the file at 150 MHz (`album-render.md` §8) |
| `SPECTRUM` (proposed) | `J_T_SPECTRUM`, 8 ms: the C reference's 4.2 ms at 27 MHz for 512 samples, twice over [sim]; the vector FFT takes 0.37 ms |
| `BARS` (proposed) | `J_T_BARS` = `J_T_DRAW`: the v1 frame (24 bars over 320 × 150) takes 4.6 ms at 27 MHz [sim, estimate] |
| `OVERLAY` (proposed) | `J_T_OVERLAY`, 80 ms: the visualizer's load is 0.77 M instructions (its sum, `init`, the start-up checks), about 28 ms at 27 MHz [sim] |
| A drawing job | 8 ms (a full screen at 27 MHz [estimate from `graphics.md` §3]) |
| `PRESENT` | 40 ms (a full frame, 14.3 ms [silicon S7], plus two TE periods) |
| A `FENCE` | 20 ms, plus its jobs' deadlines |

Nothing waits without a deadline, on either side (`hostproto.md` §4). The BCM never waits on the PP: a full event ring drops the event and counts it.

**What the job block tells a waiting PP** (`jobs.h`):
- **`J_DONE`** counts every job run since start, success or failure, when it ends; **`J_ERRORS`** counts the failures among them (and `J_LAST_ERR` names the last). So the PP sees the jobs it sent drained when `J_DONE` has moved by their number, whatever their status.
- **A failed job still retires its `FENCE`**, so a frame's wait says `OK` either way. The shell sees asynchronous failures through **`bcmjob_errors()`** (`J_ERRORS`, 2026-10-24): monotonic while the BCM runs, from 0 again after every restart, which always brings a new epoch; while cut, the last value read. Save the epoch and the count before a frame's jobs; after its fence, the same epoch and a larger count is that many failures. A different epoch means the count may have restarted, so the new value is the new baseline (and the frame is resynced anyway).
- **`J_CUR_OP`** (`0x40`, `HP_ENC`) is the op running now, `0x20`–`0x3F`, or 0 between jobs. The BCM sets it before a job starts and clears it when it ends. A PP whose wait runs out reads it with the heartbeat: a live heartbeat and a non-zero `J_CUR_OP` names the slow job, so only that op takes its CPU path. The host model mirrors it (`jmodel_cur_op()`).

**A running `DECODE` (S3c, `album-render.md` §8):** it holds the job loop for its whole run, but services the main loop as it goes, at most every 2 ms: the heartbeat, the running animation's frames and the LCD contract's command word (the PP's pushes). **It never runs another ring record:** a job queued behind a `DECODE` waits for it, in ring order, and nothing re-enters the executor. `J_CUR_OP` reads `DECODE` meanwhile.

**Liveness:** as for video. The PP checks the protocol's heartbeat (`HP_STATE`) and the LCD contract's fault word (`0x1F8 = 0xFA170000`) whenever a wait runs out. A live heartbeat with a missed deadline is a **slow job** (log, retry once, then that job's CPU path for the rest of the boot). A dead heartbeat or the fault word is a **BCM failure**.

**A failed job** (2026-10-24, the same shape as a slow one; `bcmjob.h` `BCMJOB_FAIL_MARK`): a job's reply with a status other than `HP_OK` is logged (`bcmjob fault op= why=failed`) and the op tried again; its **second failure in the boot**, across restarts, marks it failed, and it takes its CPU path for the rest of the boot (`bcmjob_have()` false, `bcmjob_submit()` `NOJOB`). The caller's normal outcomes never count: `J_E_NOSLOT` (free a slot), `J_E_SLOT` (a stale slot), `J_E_SUM` (a torn upload, uploaded again), `J_E_MODE` (video or a game holds the pool) and `J_E_DEC_*` (that source; the PP's twin decides). Everything else counts, request errors (`HP_E_INVAL`, `J_E_RANGE`) included: the PP can't tell a bad record from a BCM that disagrees with the executor, and the CPU path is the executor, so a bad record costs only that op's offload. *Proposed by the 19-2 follow-up; the contract had the mark but no rule.*

**While the BCM starts** (a cold start, until its `HELLO` and the job block are read, §2): the PP has no job block yet, so every job call answers **`OFF`** at once and nothing is queued to run after the start. A wait whose fence was lost to a cut or a restart answers `OFF` too. The model's state accounting counts the start as C1 (busy), but jobs are taken only once it ends.

**On a BCM failure** (outside a video session):
1. The PP marks jobs unavailable and finishes the frame on the CPU path, so the screen stays right.
2. The boot ladder takes over (`pipeline.md`): a power cut and our image again (the resident copy), Apple's vmcs after strikes. The new image's epoch differs, so the shell re-uploads what it still needs, by key.
3. Logged, never a splash (CLAUDE.md rule 1).
4. After a second failure in one boot, jobs stay off until reboot.

**While lit** (gap G2): a dead BCM can't drive the panel, so the restart happens at once, with 0.37–0.62 s of dark panel [silicon range]. The restart's first frame is the PP's framebuffer (the boot ladder's existing first push). **A slow but live BCM is never cut while lit**: its slow job takes the CPU path for the rest of the boot.

**One port lock** (gap G3): every writer to the host port, on either core, takes `bcm_port_lock()` (`bcmfw/jobs/bcmjob.h`) for each whole piece. That covers a ring record, an engine-2 slot piece, an LCD command or push, and a video-feed packet. The port has a single auto-incrementing address register, so interleaved pieces corrupt each other; Apple holds a per-bank mutex for a whole write [apple-RE]. On the device, the lock is lcd-video.c's existing port mutex and `lcd_state` corelock, recursive, never a second lock. **It is cross-core only once the mutex and its counters live in uncached shared memory** (`SHAREDBSS_ATTR`, as `ata_mutex` is): today `lcd_port_mtx` is a cached static, so the two cores can each see a stale copy. That move is P19 19-9's step 0 (the COP-ownership audit, 2026-10-24); until it lands the lock is only safe with every port writer on one core.

**Status (2026-10-24, `p19-cop-rows`):**
- The video feed now takes the lock for each piece: `bcm_video_rd`, `_wr`, `_wr_block` and `_flush` in `bcm-devtest-hal.c`, and the developer stages' port calls (`dt_hal`).
- A DMA block keeps the lock until its end is seen, on the same thread. The feed thread flushes before it blocks and before it exits.
- `bcm_devtest_owned` and `display_on` remain, and debug builds check them on every piece.
- On the device, the lock is still lcd-video.c's `lcd_port_mtx`, in cached `.bss`. That is safe across cores only after the step-0 coherence package moves it (with `bcm_devtest_owned` and `bcmfw_active`) into `SHAREDBSS`.
- Bench row C1 (`bench-p19.md`) holds the lock on the CPU's thread while a COP job runs the piece.

**A failure inside a stateful job** (review M3). `SCROLL` and the animations change the BCM's front frame step by step. Rules:
- **A `BUSY`, `OFF` or `TIMEOUT` anywhere in a frame's jobs:** the shell calls `bcmjob_resync()` for the whole frame (or the region it was changing). The PP framebuffer (always complete, 153,600 B) goes to the front frame and the panel through the LCD contract, today's push. The jobs already queued for that frame may still run, but the resync comes after them in order, so the panel ends on the PP's frame.
- **The BCM dies or is cut during an animation:** the animation is lost with the epoch. The restart's first frame is the PP framebuffer, which already holds the page being slid in. **A slide cut short jumps to its final frame**, by design; the user sees the new screen without the motion.
- **The BCM dies or is cut between a `SCROLL` and its `PRESENT`:** the same. The restart shows the PP's frame, which the shell had already scrolled.
- The model checks all three (`jmtest`: a death mid-slide, a full ring mid-scroll, a cut mid-scroll), and the retained screen's own paths (§12.5).
- **The resync's order** (2026-10-24): the model's `bcmjob_resync` lets the ring drain first (within `J_T_FENCE_BASE` + `J_T_PRESENT`), then pushes, so the front frame ends on the PP's too, not only the panel. **The device driver doesn't yet:** `bcmjob-core.c` on `p19` pushes at once, so a job still queued (a `SCROLL` before a `BUSY` `PRESENT`) runs after the push and leaves the front frame behind the panel. 19-L2's item; until then §12's "synced" rule covers it.

**Developer stages** (review M4). `bcm-devtest` swaps images and writes inside the merged map (the drift and r3d parameters at `0x106000`, r3d's textures at `0x010000`). Its HAL brackets every stage:
- `take()` calls `bcmjob_suspend(true)`: every job call answers `OFF`, nothing is queued, and any animation stops.
- `restore()` calls `bcmjob_suspend(false)`: the driver re-runs `HELLO`, reads the job block again, bumps the epoch, pushes the PP's frame, and calls `on_epoch`.
- The Debug menu's "BCM boot path" screens, which switch images, take the same path.
- The model implements this, and `jmtest` checks it.

**Video is the exception:** a failure during a video session follows the video-failure rule. Video stops, audio stops, and the styled error screen shows (memory: video-failure-error-screen; `hostproto.md` §4). UI jobs never trigger it.

## 7. The CPU fallback (mandatory)

The owner's rule: **every hardware path keeps a CPU fallback.**

**Compute jobs** (blit, fill, fade, average, scroll, the animations):
- The fallback is the reference executor itself, compiled for the PP. It runs the same job on the PP's framebuffer, and the shell pushes the result as today.
- The shell's choice per job is the `J_OPS` bit, plus a per-boot "slow or failed" mark.
- The simulator (`uisimulator`) always takes this path, so `tests/ui` screenshots stay pixel-exact between backends (`approach.md`).

**Memory roles: decided (owner, 2026-10-23).**
- **The degraded fallback (option b)** decides what the user sees on a BCM failure.
- **Q13 ("smooth on every unit")** decides the memory: the PP keeps its cover slots (`pv.slot`, `slot2`, and #92's bank on 64 MB). So the drift's offload saves CPU and power, not RAM. The still cover and every re-upload after a cut come from PP RAM, never from `panels.pak`.

On a BCM failure:
- **The drift goes on, on the CPU** (revised 2026-10-25, bench b3: build A's first drift frames failed on silicon and the still cover read as frozen): the same frames as with the jobs off, from the PP's cover slot (Q13 keeps it, so no RAM is lost), for the rest of the boot (`drift cpu why=off|slow|noop|failed`). The still cover (the current window, no pan, no fade) is left only for the developer row's "still". The drift's `J_ERRORS` check takes its baseline per frame, before the frame's jobs, in the same epoch (§6, `bcmjob.h`), so a failure from before the frame (the bench's self-tests, another client between frames) is never the drift's.
- **Cover Flow is disabled.** It doesn't fall back to the album list. A small pop-up screen says Cover Flow isn't available at this time. That's the owner's own exception to "log, don't show", like the video-failure screen. The words go through LANG, in our own words, and belong to the shell.
- **Everything else takes its CPU path** (the reference executor on the PP's framebuffer) and is logged.

Since Q13, the PP's buffers stay reserved anyway, so the fallback costs no extra RAM. RAM is not a P19 goal (brief rev 1c): L2 costs about 220 KB on every unit, or about 28 KB with G9's display-only image (`memory-contract.md` §5).

## 8. Versioning and growth

- `J_MAJOR` changes only for incompatible changes. The PP refuses a different major (the CPU everywhere).
- `J_MINOR` adds jobs and fields. Old PP drivers ignore what they don't know, and new drivers check `J_OPS` before using a job.
- **While the contract is a P19 draft** (major 0), a minor may also revise a job; the BCM image and the PP driver ship together. The revisions (`jobs.h`):
  - **0.1:** the first draft.
  - **0.2:** `DECODE` (`0x30`) and its input ring; `COMMIT`'s sum is F16w, two words in the record and the reply (§4), in place of FNV-1a. A two-word `COMMIT` is refused (`HP_E_INVAL`).
  - **0.3** (2026-10-24, 19-5): PAN's stepped form (`J_PAN_STEP`, three more words: the marquee), and an animation's frames at its own interval (the stepped PAN's step; every other animation's `J_ANIM_FRAME_US`, as before). Only an addition on the wire, but an image before 0.3 would read the stepped record as a linear PAN run once, so **the PP sends it only when `J_VERSION`'s minor is 3 or more** (the CPU path otherwise).
  - **0.3 + proposed `SPECTRUM`/`BARS`** (#126, sim only; not a revision): `SPECTRUM` (`0x33`, `J_OPS` bit 19) and `BARS` (`0x34`, bit 20), the Now Playing visualizer's, and `J_VPU_FFT` (`0x8`). Additions only, built only into the visualizer image (`BCMFW_VIZ`), the display-only image's overlay (below) and the host model (`JREF_VIZ`); no shipped image sets their bits, and a PP finds them by those bits. `J_MINOR` moves when an image that offers them ships. Design, numbers and room: `visualizer.md`.
  - **0.4** (2026-10-24, the retained screen, §12): every animation frame draws into `J_SURF_FRONT`, each part a `BLIT`. A SLIDE from `J_SURF_FRONT` slides the frame as the `ANIM` found it, moved on in place from the offset drawn last; its offsets never decrease (`J_E_RANGE`); `J_SLIDE_MAX` is 48, so the record fits `HP_MAX_PAYLOAD` (50 never fitted). Before it, the BCM drew a slide into the frame it was reading (every frame after the first read the one before, and a right or down slide smeared its first too), while the host model drew into the panel: the two disagreed. A PP of rev 0.3 or before never sent a slide from the front frame, so nothing it sends changes meaning.
  - **0.4 + proposed `OVERLAY`** (#126, sim only; not a revision): `OVERLAY` (`0x35`, `J_OPS` bit 21), the job block's `J_OVL` (`0x48`) and `J_OVL_ABI`, in the display-only image (§5, "Overlays"). With the visualizer's overlay loaded, that image sets `SPECTRUM`'s and `BARS`' bits too. Additions only; `J_MINOR` moves with `SPECTRUM`/`BARS`.
  - **0.5, the staged upload** (2026-10-25, open question #67; candidate images only, §4 "The staged upload"): `ALLOC`'s optional sixth word `J_ALLOC_F_STAGE` and its reply flag `J_ALLOC_STAGED`; `COMMIT` moves the window into the slot before its check; `COMMIT`'s reply adds `how` (`J_HOW_*`) and its `J_E_SUM` reply the BCM's words (`J_SUMREP_*`); the job block's `J_CAPS` (`0x4C`). Additions only on the wire: an older image ignores the sixth word and answers as 0.4, and an older PP never sends it. **`J_MINOR` stays 4** until an image with the form ships (the candidates publish minor 4; a PP finds the form by `J_CAPS`), then moves to 5.
- Opcodes are never reused. The reserved ones (`0x30–0x32`) keep their meaning.
- The BCM image and the PP driver ship together today. Versioning lets a BCM image move ahead on its own (`approach.md`, "Keeping upgrades possible").

## 9. Testing without a device

- **Item 5 of the P19 prep:**
  - a C reference executor and record encoder (`bcmfw/jobs/`), built for the Mac and the PP alike;
  - a host test that encodes jobs into a ring image, byte for byte as the PP will;
  - a decoder that runs them through the executor, with golden checksums.
- The BCM's implementation runs the same records in vc2sim, and its surfaces must equal the executor's (the drift and r3d pattern: a Mac build and a simulator that agree).
- A shell agent can build against `jobs.h` and the executor before any BCM code exists.

## 10. The PP-side API and the host model (contract 4)

`bcmfw/jobs/bcmjob.h` is the API the shell calls. There are two implementations:
- **the device driver,** P19 19-L2, in `firmware/target/arm/ipod/video/`;
- **the host model,** `bcmfw/jobs/jmodel.c`, over the reference executor. The simulator wraps it (`uisimulator/common/sim_bcm_job.c`), and `jmtest.c` tests it.

**What the API gives:**

| Call | What | Gaps |
|---|---|---|
| `bcmjob_have(op)` | may this job go now: the image has it, it isn't marked slow or failed this boot, and the BCM is up | |
| `bcmjob_submit(op, tag, payload, n)` | queue one job, never blocking: `OK`, `BUSY`, `OFF` or `NOJOB` | G1 |
| `bcmjob_fence`, `bcmjob_wait(seq, ms)` | one wait per frame: `OK`, `TIMEOUT` or `OFF` | |
| `bcmjob_errors()` | `J_ERRORS`: failed jobs since the BCM's start, compared within one epoch (§6) | |
| `bcmjob_slot`, `bcmjob_upload` | ALLOC, then the pixels by engine 2 (16 rows or more) or CPU, then COMMIT with their F16w (`j_f16w()`); skipped on a hit | |
| `bcmjob_epoch`, `bcmjob_on_epoch(fn)` | the driver calls `fn(epoch)` in the ring owner's thread context (the UI thread: the COP from 19-9) when it cuts or restarts the BCM: the shell forgets its slots and needn't poll | G13 |
| `bcm_port_lock`/`unlock` | the one port lock (§6) | G3 |
| `bcm_power_want(why, hold_ms)`, `bcm_power_release(why)` | the only way to power the BCM while dark: a decode (on the charger only), a paused Class C game, a video's own sleep. Everything else follows the backlight and the mode; the shell never asks | G11 |
| `bcmjob_dark_ups(why)` | power-ups while dark, by reason; the driver logs `bcm power ups dark=N decode=a game=b video=c` once per hour of dark time. Dark playback must read 0 | G12 |
| `bcmjob_decode(req, res)`, `bcmjob_read()`, `bcmjob_dec_verdict()` | `DECODE` for the shell (2026-10-24): a `jdec_src` streamed through the input ring into slots allocated by class and key, blocking with a deadline and a cancel hook, refused dark on battery; then the slots read back; every answer mapped to `jdec`'s codes or "run the CPU twin" (`album-render.md` §11, §14 the shell's recipe, §15 the driver's steps). **Rule:** read back box-sized results only, never a full panel (port reads under load cost 55–85 µs a word; `host-driver.md` §9) | |

**The trace lines** (the gate matches these):
- `bcmjob submit op= tag= n=`
- `bcmjob fence seq=`
- `bcmjob fault op= why=busy|off|nojob|timeout|failed|refused|aborted`
- `bcmjob decode len= n=`, `bcmjob decode done st= err= passes= bytes= ms=`
- `bcm power state=C0|C1|C2|C3|C4|C5 why=lit|dark|want|fault` (the model emits C0 and C1; the device driver adds the standbys once 5s allows them)
- `bcm power want why=`
- the shell's `drift cpu why=`, `drift still why=developer` and `coverflow unavailable why=`

All are under 111 characters.

**The host model** (brief §3.4):
- **Pixels:** every job runs through the executor, so `tests/ui` goldens match the CPU path.
- **Time:** modelled. A job takes `JM_COST_*` ms, a PRESENT 14 ms, a cold start 400 ms, and an engine-2 upload is timed at 11 MB/s. An animation draws its frames on the BCM's grid: every `J_ANIM_FRAME_US` (33 ms), or a stepped PAN's step (rev 0.3; `jref_anim_frame()` returns the interval), into the panel; `jmodel_anim_frames()` counts them, as `ANIM_STOP`'s reply does on the BCM.
- **Faults:** `jmodel_fault()` clears an op, makes one slow (40× its deadline; `COMMIT`'s scales with the slot, `J_T_COMMIT`) or failing, stops the heartbeat, or cuts the power.
- **The job block's words it mirrors:** `J_CUR_OP` (`jmodel_cur_op()`: the op whose modelled time is running, 0 between jobs) and `J_DONE`/`J_ERRORS` (the executor's `done` and `errors`). ALLOC's address is the BCM's (§4).
- **Accounting:** the state per ms (C0/C1/C2), cold starts, jobs done and dark power-ups.
- **Tests:** `jmtest.c` checks each of these, including an hour of dark playback with 0 power-ups, a decode refused on battery and allowed on the charger, a slow job marked only past its own deadline, every job `OFF` during a cold start, `J_ERRORS` across a fence and a restart, an op marked failed on its second failure (and never for a full pool), and a dead BCM restarted, then jobs off after the second failure. Its DECODE cases (2026-10-24) cover `bcmjob_decode()`: the art set's sizes against the work area, outputs equal to the CPU twin's (read back in bands, F16w checked), a hit, the reduced set (n > 1 with hits: only the misses decoded, equal to the twin given that set, the hit's pixels and state untouched by a good, failed or cancelled decode, the work checked on the misses), two ring passes, each source verdict, a read error, a cancel mid-stream and after it, a cut and a dark screen mid-decode, D-3, `NOJOB`, `BUSY`, `TIMEOUT` and its mark, and a ring nobody feeds. Since rev 0.3 also the marquee's sequence (the shell recipe, §5): the upload, the frames against F2's own timing, `ANIM_STOP` and the row at rest, a hit, a full ring, a mid-lap start, a cut, a stale slot, dark, and a cleared op.

**The executor in the PP firmware** (gap G4): build `bcmfw/jobs/jref.c` into `firmware/` under `HAVE_BCM_JOBS`, from `firmware/SOURCES` (as `bcm-devtest-images.c` is), with no IRAM.
- **What a build compiles in** (`jref.h`): the PP's CPU path needs neither the vector hooks nor `DECODE`, so they are behind two macros.
  - `JREF_DECODE`: `J_OP_DECODE` and its hook (`HP_E_UNKNOWN` without it). The host model and the decode tests define it (`jmodel.h` refuses to build without it).
  - `JREF_HOOKS` (implies `JREF_DECODE`): the vector hooks' fields and calls too. Only the BCM image defines it (`build.py`). Without it, every copy is one `copy_rest()` call, which is the same rows in the same order (the 16-row groups only shape the hook's calls).
  - They change `struct jref`, so the name of `jref_setup` (which `jref_init` calls) carries them (`jref_setup_hooks`, `jref_setup_decode`): a program built with a mix fails to link instead of misreading the struct. **The simulator's model build** (`HAVE_BCM_JOBS_MODEL`) needs `-DJREF_DECODE` on `jref.c`, `jmodel.c` and every file including `jref.h`; the device build needs nothing.
- **Size:** **4,242 B** as the PP builds it (`arm-none-eabi-gcc -mthumb -mcpu=arm7tdmi -Os -ffunction-sections -ffreestanding`, gcc 16), no data; 5,560 B before the split [measured 2026-10-24]. The largest parts: `jref_exec` 1,976 B, `op_mix` 564, `jref_anim_frame` 542, `anim_check` 276. It needs no 64-bit divide (`__aeabi_uldivmod`).
- **State:** about 3 KB (the slot table and pools' descriptors), held by the driver.
- **The shell's drift** calls `jmix.h` (header-only) for its fade and half step (gap G5), so its CPU frames and the BCM's are one definition.

**A display-only resident image** (gap G9) [estimate]:
- **Size:** 42,964 B with `J_OP_DECODE` and its S3c service (`album-render.md`; progressive compiled out; 42,832 B before 2026-10-24's fixes), 21,304 B without it [measured, 2026-10-24, with the park stub and the vector kernels], without the video decoder and AAC.
- **The saving:** about 190 KB of pinned PP RAM against the ~210 KB resident video image.
- **The cost:** a video start would then load the video image. Swapping images on a running BCM without cutting the panel is the warm swap (`warm-swap.md`). The core parks in a stub outside every image, the host uploads the video image, and the stub jumps to it; GPIO 48 is never touched. It passes in vc2sim [sim] and is untried on silicon. With it, a start is about 15–40 ms [estimate]; without it, a cold start takes 0.37–0.62 s.
- **The decision:** keep the resident video image until a bench row proves the warm swap; then Q2 can take the RAM.

**The vector unit** [sim, 2026-10-24]: `BLIT` (opaque and with its key colour), `FILL`, `SCROLL`, `FADE`, `AVG`, `SHADE` and the animation frames run their 32-byte-aligned 16-pixel columns on the vector unit, in 16-row groups or row by row.

**How it fits together:**
- **The kernels** are in `video/jobs_kernels.S`, generated by `video/gen_jobs.py`. The generator checks every instruction's encoding by decoding it again. `gen_jobs.py --mutate NAME --out DIR` writes a deliberately broken kernel into DIR, to show the tests catch it. A mutant never goes over the tracked kernel (the generators refuse it): a gate runs several benches at once, and one that built an image while a mutant sat in `bcmfw/video` took it in (decode_image's strict-alignment trap from jidct_test's `puty`, 2026-10-26). The tests build a mutant through `bcmfw/tests/mutant.py`, which substitutes it for the tracked file in their own process only (`vc2cc.SUBST`).
- **jref** reaches them through four hooks (`jref.h`): `copy` (BLIT, SCROLL, SLIDE and PAN), `fill`, `mix16` (FADE, AVG) and `shade16`. Each does the aligned middle of what it is given and returns the span `[lo, hi)` it did; C does the columns either side and the rows a hook can't take. So an unaligned left edge no longer sends a whole job to C.
- **A source out of phase** (2026-10-24): the destination's columns must start 32-byte aligned, but a group's or a block's source may sit at any phase. `jv_gather16` fetches it with two aligned loads and a lane shift (below). `jv_rows` (rows one at a time: overlapping BLITs, BLITs under 16 rows) still needs the source in phase.
- **The host model builds without the hooks** (`JREF_DECODE` only), so its results and the jscene golden (0x73428E8F since rev 0.4; 0x82E48043 before, when its slide drew into the panel) are unchanged; `jtest` and vc2sim's jscene build the PP's way, with neither.
- **Order:** every result equals the pixel-by-pixel rev 0.1 executor, overlaps included.
  - A 16-row group reads all its rows before writing any. FADE and AVG take groups only when that equals row order: the destination is another surface, or the same rect of a source.
  - BLIT between rects that don't overlap goes in groups. The last group starts over rows already copied when h isn't a multiple of 16: their sources never change, so they copy the same again (`RUN_AGAIN`).
  - Overlapping rects of one surface go a row at a time in the safe order, each row whole (hook and C) before the next. A row that overlaps itself (`dy == 0`) is a memmove, in C.
  - SCROLL goes in groups the old way (bottom up when rows move down). The last group starts over moved rows only once |dy| ≥ 16, when their sources are never overwritten.
  - SLIDE and PAN draw as two rects, A then B, which is the pixel order. They use groups only when drawing from another surface; drawn from the frame itself, they stay in C.
  - SHADE runs whole 16-column blocks, the columns past w with alpha 0. That leaves a pixel exactly as it is (k = 256), and the block stays inside its row. So the 10-column shadow strip goes to the vector unit too.
  - FADE with b's shaded columns (`J_FADE_SHADE_B | n`) runs whole on the vector unit since 2026-10-24: a block holding shaded columns passes `jv_mix16` its 16 factors, and the kernel shades each channel of b inside its mix (below). Only the columns before d's first 32-byte boundary stay in C, shaded there as before.
- **The C copy** (the rest, and every unaligned job) moves two pixels a word when source and destination share their phase. That is the same as pixel by pixel, because a word read early differs only when d is exactly one pixel past s, which the same phase rules out.

**The kernels:**

| Kernel | What it does | Size |
|---|---|---|
| `jv_store16` | the common tail: 16 single block-0 `vst16`s, the device-checked store form, the base moved a row at a time | 33 |
| `jv_group16` | a 16-row group of columns copied (each column's 16 rows loaded or gathered, then stored); with a key, `jv_key16` | 28 |
| `jv_key16` | the key colour, without flags: t = \|hi − kh\| + \|lo − kl\| (0 exactly when s is the key), m = (t − 1) >> 15 (−1 or 0), d′ = s + (s − d)·m, which is exactly d or s in wrapping 16-bit lanes. s is fetched into F (any phase), d loaded into B | 42 |
| `jv_rows` | rows in order, each column loaded then stored (row 0 only); the source moves 32 B a column (a copy) or 0 (FILL from one row of the colour) | 25 |
| `jv_mix16` | (a·(32 − wb) + b′·wb + 16) >> 5 per channel; a and b each loaded, or gathered into F and moved to A or B. FADE: wb = max(1, (weight + 4) >> 3), 32 at 256 (b exactly). AVG: wb = 16 less 1 before the shift, which is (a + b) >> 1 exactly: the sum is never 16 more than a multiple of 32, so no rounding ties. `jv_avg16` is gone. b′ = b, or with factors (a 7th argument, on the stack; the raw FADE) (c·k) >> 8 per channel, fused into the mix: the factor row loaded REP 1 into ACC's first row and read unstepped (`jv_shade16`'s form), c·k into TMP, its high bytes times wb into ACC, a·wa into TMP | 122 |
| `jv_shade16` | c·k per channel, with k = 256 − alpha per lane as a vector B. The floor(/256) is the product's high byte, read as an H8 operand: exact for products below 65,536 (at most 63 × 256) | 26 |
| `jv_phase16` | a source out of phase: its address aligned down, p = its phase in pixels, and the flags of lanes 16 − p … 15 (`vbitplanes` of 0xFFFF0000 >> p) | 7 |
| `jv_gather16` | F = 16 rows of the 16 pixels p past an aligned base: two aligned `vld16` REP 16 side by side (rows 32–47, x 0 and x 32), then `vadd F, A16+r2, #0` at x = p and, on the flagged lanes, at x = 16 + p. The base moves 32 B on, so a row of columns gathers column after column | 8 |

Sizes are in lines of the generator's output (instructions and labels).

**The lane shift** (`jv_gather16`; the state of the art is the unaligned-load idiom: AltiVec's `lvsl`/`vperm`, NEON's `vext`, SSSE3's `palignr`, two aligned loads and a shift or permute):
- The VPU has no permute, but its register file is 2-D and an H16 vector keeps each lane's high byte 16 columns past its low byte. So two aligned 32-byte columns loaded at x 0 and x 32 of the same rows make each row [lo0 hi0 lo1 hi1], and an H16 **A** operand at x = p reads pixels p … 15 of the first on lanes 0 … 15 − p; at x = 16 + p it reads pixels 0 … p − 1 of the second on lanes 16 − p … 15. The second move is predicated on those lanes.
- **The forms weighed:**
  - **`vldp`/`vldq` pairs** (Apple's misaligned load): a REP-16 strided pair isn't checked on silicon, and a plain `vld16` aligns its address down (`quirks.md` #17a);
  - **a B operand's `+r`**: silicon aligns a horizontal B's x down to 16 (#17b), so it can't shift by p;
  - **a D (or store) at a lane offset**: a D's +r is unchecked, the store's only checked form is a block-0 row at offset 0 (S5–S7), and two 16-lane writes at 16 − p overlap;
  - **V16 columns with REP r0** (move whole pixel columns, p and 16 − p of them): two unchecked forms (V16 A operands stepping by column, REP r0);
  - **an A operand at a constant x** (S22, S27): checked, but needs 16 copies of the code (about 640 B), past the merged image's room;
  - **chosen: an A operand at `+r2`.** Two ALU instructions, the ADIR form (`vadd D16 A16 #imm rep`) and MVNZ's predicate, plus an A `+r`, which vc2sim applies as given and silicon hasn't run yet (bench ASHF/ASHG, `bench-p19.md`).
- **Not reused:** the second column of one step is the first of the next, but keeping it means reading the row as [lo1 hi1 lo0 hi0], and the wrap of x past 63 isn't checked either. So each column loads both (one extra `vld16`, about 16.6 cycles of about 100).
- **Order:** the extra pixels a gather reads either side of the source rect are never stored. A group still reads all its rows before writing any, so the hazards are those of the in-phase groups: shifted sources come only from another surface, or from rects that don't overlap (BLIT), or not at all (SCROLL has dx = 0).

**Forms used** (the display-only image still passes `forms_test.py`: every kind is Apple's or device-verified):
- **Device-checked:** loads are `vld16` REP 16 with a stride, or REP 1 into row 0 (S4). Stores are single 48-bit `vst16`s of block-0 rows (S5–S7). The ALU forms are gen_xfade's (MU8R, MULR, ALUX, AD68, ADIR, LSR8, MHR4).
- **New, unproven on silicon** (Apple kinds, shown only by vc2sim and Apple's corpus):
  - `vdist D16 A8 Breg` and `vasr D16 A16 #15`: the key's test;
  - `vsub D16 A16 B16`;
  - `vmul D16 A16 B16` with a vector B, stepped (the key's mask) as well as SHADE's unstepped factor row, and since 2026-10-24 the raw FADE's (the same kind, at x 0 of row 48: ACC's first row; its REP-1 `vld16` there too). No new kind: `forms_test.py` passes unchanged;
  - the lane shift (2026-10-24): `vadd D16 A16+r2 #0 rep16` (an A operand at a register's lane offset), the same under `ifnz` after `vbitplanes r5 setf` (MVNZ's predicate on a 16-bit D at REP 16), and `vld16` REP 16 into rows 32–47. Bench rows **ASHF** and **ASHG** check the first two (`bench-p19.md`).
- **Tried and dropped:** an 80-bit `vmax.1`. It is in no Apple kind, so `forms_test.py` refused it in the display-only image; the `vasr` mask replaced it.

**Exactness** (`jobs_image_test.py`, 696 cases since the stepped PAN; 609 since the raw FADE and the self-check; 495 after the lane shift; 231 before): the rows "SCROLL", "FADE, AVG, SHADE" and "BLIT, FILL, ANIM" compare every pixel of the surfaces involved with Python models of `jmix.h` and of the pixel-by-pixel rev 0.1 executor. They cover:
- weights 1, 3, 4, 100, 252, 255 and 256, and FADE with 4, 10 and 32 shaded columns;
- in-place and three-surface rects, slots at other pitches (a 300-wide drift slot at 304, in and out of phase), unaligned heads, the last rows;
- BLIT apart, overlapping both ways, within one row both ways, keyed (35 % key pixels) in groups, again and in C, and keyed overlaps;
- FILL aligned, unaligned, single rows, in a slot;
- SLIDE in all four directions at aligned and unaligned offsets, from slots and BACK, and from the frame itself;
- PAN windows that line up and that don't;
- **the stepped PAN** (rev 0.3, the row "stepped PAN", 80 cases): the marquee's frames at rest, at steps 1, 2, the middle and the last, and after a wrap, for windows 150–271 wide, 16 and 18 rows, laps odd and even (up to Now Playing's 1,240), steps of 2 and 3 px, three destination phases, strips rounded to 16 px (the vector unit) and odd (C), a rest band below the lap; and one whole lap, its frame count and its rest at the end. The `store` mutation fails 18 of them;
- **the raw FADE at every phase** (2026-10-24, in the row below: 96 cases): 0–32 shaded columns (p, 16 − p and 17 + p at phase p, alpha 0–255), in place toward the cover and with three surfaces, odd widths, aligned and unaligned heads (one or two factor blocks, some shaded columns in C), weights 1–256, the last rows in C;
- **every phase 0–15** (the row "out of phase", 355 cases): BLIT from a 320-pitch strip and from a 304-pitch cover, keyed BLIT, a BLIT beside its source in one surface, FADE with three surfaces and in place toward the cover, AVG's two framings a pixel apart and AVG from two phases of one slot; odd widths, aligned and unaligned heads, the last group again and last rows in C.

**Mutation checks** (`gen_jobs.py --mutate`, then the test):

| Mutation | Fails |
|---|---|
| `store`: `jv_store16` drops row 15 | 49 cases in five rows |
| `rows`: `jv_rows` always moves s by 32 (FILL reads past its colour row) | 5 |
| `rowstep`: `jv_rows` reads every row from the first source row | 6 (SCROLL 4, BLIT 2) |
| `key`: `jv_key16` drops its · m | 2 (the keyed groups) |
| `avg`: `jv_mix16` drops AVG's − 1 | 2 (both vector AVGs); 36 with the phase row |
| `shift`: `jv_gather16` takes its second half one lane late (x 17 + p) | 127: every case at phases 1–15 (120 in the phase row), none at 0 |
| `lanes`: `jv_phase16` flags one lane too many for the second half | 127, the same cases |
| `shade` (2026-10-24): `jv_mix16` leaves b's blue unshaded in a shaded block (k = 256: a wrong factor) | 42 (40 raw FADEs in the phase row, 2 in the FADE row) |
| `klane` (2026-10-24): `jv_mix16` takes its factors 16 lanes on (the next block's: wrong lanes) | 42, the same rows |

**Since the self-check** (2026-10-24), `shift` and `lanes` fail 5 cases, all in the self-check row: the broken gather fails the image's own check at start, so `J_VPU_SHIFT` stays clear and every out-of-phase job takes the exact C path. That is the gate working; the phase row's 120 cases still test the gather at every phase whenever the check passes, so a fault the check's two phases miss is still caught there. The other mutations, over the rows FADE/SHADE, out of phase, self-check, BLIT and SCROLL: `store` 236, `key` 18, `avg` 37, `rows` 4, `rowstep` 6.

With the phase row the older mutations fail more cases: `store` 185, `rows` 5, `rowstep` 6, `key` 18 (16 of them the keyed BLIT at every phase).

**The merged image's room** (`BCMFW_VIDEO=1 BCMFW_JOBS=1` with `IDLE_SLEEP`, `IDLE_GATED`, `IDLE_DOORBELL`; the arena at `0x35400` stays put):

| | Load image | image + bss ends | Spare below the arena |
|---|---|---|---|
| Before (jref's DECODE) | 218,116 B | `0x3512c` | 724 B |
| After the kernels | 217,924 B | `0x3506c` | 916 B |
| After the contract fixes (pool bus bases, `J_CUR_OP`, the drift's pitch; 2026-10-24) | 218,020 B | `0x350ec` | 788 B |
| Before the lane shift (F16w, jdec's fast decode; `70af21dc52`) | 218,244 B | `0x351cc` | 564 B |
| After the lane shift (`jv_phase16`, `jv_gather16`; 2026-10-24) | 218,404 B | `0x3526c` | 404 B |
| After the self-check (`J_VPU`; 2026-10-24) | 218,564 B | `0x3532c` | 212 B |
| After the raw FADE's kernel (2026-10-24) | 218,756 B | `0x353ec` | **20 B** |
| After the stepped PAN (rev 0.3, 19-5; 2026-10-24) | 218,756 B | `0x353ec` | **20 B** |
| After rev 0.4's slide (every animation part a `BLIT`; in-row right `BLIT`s in words; 2026-10-24) | 218,660 B | `0x3539c` | **100 B** |

- **What paid for it:** jref's PAN `progress()` was the merged image's only caller of `__udivdi3` (1,258 B of library code). A 16-step long division gives exactly t·65536/ms (checked against the 64-bit form over 54 M cases).
- **The other cuts:** AVG now runs on FADE's kernel (`jv_avg16`, 308 B, gone). The key loop moved into the kernel. `span()` is shared, not inlined four times. `op_mix` and `op_shade` walk pointers instead of recomputing `at()`. SCROLL, BLIT and the animations share one copy run.
- **The display-only image:** 21,764 → 21,304 B. With DECODE (today's image): 43,064 → 43,216 B for the lane shift, image+bss to `0xb700` (was `0xb660`; the limit is `0xBA00`).
- **The self-check and the raw FADE** (2026-10-24): the check 160 B of code, `kbuf` 32 B more (two factor blocks); `jv_mix16`'s fused shade about 150 B and its C glue (`factors()` shared by both hooks, which paid back SHADE's per-group loop) about 40 B. The display-only image: image+bss `0xb7a0` → `0xb860` → **`0xb900`**, 256 B below `0xBA00`. The merged image had 20 B left; rev 0.4 (§12) gave back 192 B by drawing every animation part through `op_blit`, so it has 100 B now. Candidates: `jv_mix16`'s shaded path as one out-of-line copy for G and B (they share operands: about 60 B, for two calls a block), or `shift_check`'s loop folded into one pass.
- **The stepped PAN** (rev 0.3, 19-5): its check and its frame cost 62 B (`anim_check` 500 → 554, `jref_anim_frame` 854 → 862, with SLIDE's saving below), the frame interval and the widened present 40 B in `jobs_tick`. Paid in the same files, so the merged image ends where it did (`0x353ec`, 20 B spare); the display-only image is 32 B bigger (45,344 B; image+bss to `0xbf40`, below its decode work at `0xc200`):
  - `jobs_init` calls `jref_setup`, which takes the three surfaces by pointer: they were 48 bytes copied onto the stack (`jobs_init` 534 → 500, the setup 104 → 80). `jref_init` keeps its by-value form for every other caller (`bcmjob-cpu.c` on `p19`, the host tests, the model) as an inline wrapper in `jref.h`, which costs an image nothing unless it is called;
  - SLIDE draws its two rects with one pair of calls for both directions, in place of two pairs (about 90 B); rev 0.4 replaced that with two `op_blit` calls;
  - `jobs_tick` draws into the front frame itself, not into a copy of its descriptor (46 B), and presents with one call (14 B): 316 → 296 with the stepped PAN's additions;
  - tried and dropped: a 32-bit lerp for the linear PAN (+4 B), `rect_ok` out of line (−6 B net, a call for every check), and the frames' descriptors in rodata (+32 B).
- **The lane shift's 160 B:** `jv_phase16` and `jv_gather16` (about 90 B), the phase tests and out-of-line calls in three kernels; `span()` lost its source check. The plain video image (`mkvideo.py`, fnv `8c5d2dff`) doesn't hold the jobs and is unchanged.

**On silicon:**
- All but the forms listed as new above are forms the drift's kernels use.
- The lane shift's A `+r` was the one addressing silicon had never run. **Bench rows ASHF/ASHG ran on 2026-10-25 (`bench-20261025-1430/bcmtest.log`) and the A operand's lane offset is not as modelled** (word 0: ASHF `be7cdd8b` against the wanted `02b37aaf`, ASHG `6b918b77` against `5a86a5ea`; `bcm2722-reference/quirks.md` #30). The self-check below caught it, as designed.
- **The lane shift's self-check** (2026-10-24) [sim]: `jobs_init` runs one out-of-phase gather through the real kernel (`jv_group16`, one column, stride 0, from `kbuf` onto itself) at p = 5 and p = 13, the bench rows' phases, and compares the 16 pixels with the pattern in C. It writes the job block's new word **`J_VPU`** (0x44) once: `J_VPU_CHECKED` always, plus `J_VPU_SHIFT` if the gather agreed. Without `J_VPU_SHIFT`, `span()` requires every source in phase with the destination (its `phases` argument, the addresses xored, 0 mod 32): the rule before the lane shift, so those jobs take the exact C path. Images before this word read 0 there. The BCM image has no text log; the word is the record. The PP driver logs it once per BCM start, at discovery and never while the BCM is down (`bcmjob vpu=0x3 expect=0x3`, p19 63afabe326, 2026-10-24). The expected value is `bjc_vpu_expected(J_OPS)`: 3, or 7 for an image offering DECODE. A smaller word adds `why=degraded`, or `why=unchecked` for 0 (an image from before the word). Neither is a fault, because the jobs still run. The bench session also reads it itself (`devbench.c` `jvpu`, and B5 for the display-only image). Costs about 160 B of code (the check and `span()`'s test) and 32 B of `kbuf` (now 32 halfwords, shared with the raw FADE's factors). The check's loads use a stride of 0 (one 32-pixel row read 16 times), a value silicon hasn't run with `vld16` REP 16 [sim]; if it misbehaved, the check would fail and the C path would be taken, the safe direction, and ASHF/ASHG (no stride) would tell the two causes apart. `jobs_image_test.py`'s row "lane-shift self-check" checks both outcomes: as built, `J_VPU` = 3 and the out-of-phase BLIT, FADE and AVG run `jv_gather16`; built with `BCMFW_JOBS_SHIFT_FAIL=1` (the check expects the pixels one lane late, tests only), `J_VPU` = 1, `jv_gather16` never runs, and every pixel is still exact. (The display-only image, which the row builds, reads 7 and 5 since its IDCT check, below, sets `J_VPU_MAC` too.)
- **The self-check on silicon** (2026-10-25) [silicon]: the fallback works. The merged image published `J_VPU` = 0x1 (checked, `J_VPU_SHIFT` clear; 0x3 was expected), the display-only image 0x5 (checked and `J_VPU_MAC`, shift clear; 0x7 was expected), and the shell logged `bcmjob vpu=0x1 expect=0x3 why=degraded` (`bench-20261025-1430/ipodui.log`, line 26). Neither is a fault: the out-of-phase jobs took the exact C path. The cause is the A operand's `+r` (ASHF/ASHG fail on the same silicon), so the stride-0 `vld16` REP 16 loads of the check are not blamed. Until the rule is known, the lane shift stays off on silicon.
- **The lane shift by transposes** (2026-10-25, branch `p19-vpu-silicon`) [sim; its forms silicon-proven]: `jv_gather16` no longer takes a lane offset from an A operand's `+r` x (an H16 A's is aligned on silicon, quirks #30). It loads the 32 pixels of 16 rows (rows 32–47 and 48–63 x 0), transposes them into rows 16–47 x 32 with two `vmov`s from stepping V16 B operands, moves rows p … p + 15 to rows 32–47 x 0 through an H16 A at `+r` = 64 p (a y offset only) and transposes back into F: four REP-16 ops where the old form had two, but only forms the 2026-10-25 run proved (a stepping V16 B: REPA; an A's `+r` y: REPA, R48E). `jv_phase16` now just returns 64 p (no flags; r5 is no longer clobbered). Exact under every A-operand rule in vc2sim; the start-up check passes under each, and fails under each rule silicon fits for the old gather (`jobs_image_test.py` "under each A +r x rule"). Cost (the audit, estimated cycles, vector against the C path the failed check left): out-of-phase BLIT drift pan 25,586 against 66,016 (2.6×); AVG drift half step 141,635 against 625,232 (4.4×); FADE drift 118,111 against 1,316,344 (11×), the raw FADE 119,446 against 1,374,064 (11.5×); the marquee BLIT 4,488 against 13,723 (3.1×). Bench row GTHR runs the four ops on silicon.
- **The IDCT's self-check** (2026-10-24, branch `p19-vpu-idct`) [sim]: the display-only image's `decode_init` runs jdec's start-up check (`jdec_vpu.c` `idct_check`: four blocks at each size, 4, 8 and 16 points, through the kernel and through the C IDCT, rounding probes included) and sets **`J_VPU_MAC`** (0x4) if every pixel agreed: jdec's IDCTs then run on the vector unit, else all in C, exact either way (`jpeg-spike.md` §13-14). Images that don't decode leave it clear. `decode_image_test.py`'s gate row checks both outcomes: as built `J_VPU` = 7; with `BCMFW_JOBS_MAC_FAIL=1` (tests only) `J_VPU` = 3 and the decodes never call the kernel.
- Bench row B3 (the drift on the merged image, jobs against the CPU path, the `tests/ui` goldens on both) is their first device check.
- The open question of mismatches at 16-byte boundaries in the decoder's kernels (`isa-vector.md`, "Open") applies here too. These kernels never cross a 16-byte line within a row in their loads or stores, but B3 decides.

**As built on silicon, 2026-10-25 (bench session, build A: p19 `4e57b3b9a1`; merged image fnv `cd8e5d96`, display-only image fnv `f7772048`; `bench-20261025-1430/bcmtest.log`, `bench-20261025-1430/ipodui.log`)**
- **`ALLOC`** replied on the device [silicon].
- **`COMMIT` has not yet passed on silicon** [silicon]. It failed for the shell's UI drift (an engine-2 upload of a 160 x 240 slot; `bcmjob fault op=0x22 why=failed`, then a resync and `panel drift still why=failed`) and for B5's `win_slot` (CPU writes, 300 x 300 and 400 x 160), on both images.
- **The cause is open.** The status word was not logged. The candidates [hypothesis] are the write path (the slot arriving torn or short) and the BCM-side F16w sum; open question #67 in `../../bcm2722-reference/open-questions.md`. A port write-check probe (write a slot, read it back, then `COMMIT`) is planned, and `bcmjob` should log the status word.

**The Port write check, 2026-10-25 sitting 1 (`bench-20261025-1819`, display-only image fnv `2087891f` and merged `e7f42c0d`): the root cause** [silicon]
- Every port write read back right (CR1: the control area and all 7 pool addresses 0 wrong; Apple's write forms the same), yet `COMMIT` answered `J_E_SUM` for 5 of 8 slots whose every pixel read back right.
- **Cause:** the BCM's own F16w was wrong, not the upload: `jref_f16w` reads `px[1..7]` of every 8 with the 32-bit `0x29`/o1 halfword load, which **sign-extends** on silicon (`../../bcm2722-reference/quirks.md` #33). Every pixel of `0x8000` and up counted as itself − 65,536. All 12 logged BCM sums (CR1's 8, CR2's 4 direct) fit exactly, both words; 8×1 of `0xFFFF` gave `0x0000fff8 0x0007ffdc` against the PP's `0x0007fff8 0x0023ffdc`; the one passing case (8×1 by engine 2) had no pixel of `0x8000` or more. The model had the two offset halfword loads swapped (`quirks.md` #16, #33).
- **Fix:** vc2sim decodes `0x28`/o1 as zero- and `0x29`/o1 as sign-extending; vc2cc emits `0x28`/o1 for unsigned halfwords and avoids the unproven gp/sp-relative and 48-bit halfword loads; every image regenerated (`merged-image-batch.md`). `bcmfw/tests/halfload_test.py` holds the line.
- **The second fault, CR1's 301×7 strip slot:** its pitch, 602 bytes, put rows 1, 3 and 5 at 2 mod 4, and the host port's write address ignores bits 1:0 (`quirks.md` #34), so those rows landed one pixel early (906 bad pixels, by the CPU and engine 2 alike). Now every class's pitch is the width rounded up to 16 pixels (`J_SLOT_PITCH`), `bcmjob_upload` merges an odd first or last pixel into its word, and the PP's port layer refuses a mid-word write.
- Sitting 2 re-verifies the batch: `COMMIT` on every class and size (CR1), bench HWLD in stages 4 and 4b.

### Cost audit

`bcmfw/tests/jobs_image_test.py --audit` single-steps the display-only image through `hp_poll` for each job. The merged image runs the same code. For each job it counts every instruction, including the ring read, the dispatch and the reply.

**The cycle estimate** uses `isa-vector.md`'s S7 silicon timing:
- a vector instruction of REP n costs about n + 0.5 cycles (16.25–16.63 at REP 16);
- a single-row store costs about 1.5;
- each scalar instruction counts as 1 cycle. That is a floor: SRAM loads and taken branches may cost more.

[sim, estimate; 2026-10-24]

| Job (the shell's typical case) | Instructions before | Instructions after | Vector | C share | Cycles after | 27 MHz | 150 MHz | Before, 27 MHz | After the lane shift: instructions, vector, C share, cycles, 27 MHz (2026-10-24) |
|---|---|---|---|---|---|---|---|---|---|
| BLIT the drift's pan: 160 × 240 from a 300 × 300 cover (x 70: out of phase) | 352,975 | 65,903 | 0 | 100 % | 65,903 | 2.44 ms | 0.44 ms | 13.07 ms | **10,350, 3,015, 22 %, 20,858, 0.77 ms** (3.2×) |
| the same, in phase (x 64), at the 304 pitch (2026-10-24) | 65,896 | 8,774 | 2,550 | 27 % | 12,299 | 0.46 ms | 0.08 ms | 2.44 ms | 9,030, 2,550, 25 %, 12,555, 0.47 ms (the phase test: +2 %) |
| SHADE the shadow strip, 10 × 240 | 64,714 | 4,831 | 510 | 82 % | 8,911 | 0.33 ms | 0.06 ms | 2.40 ms | = |
| AVG the drift's half step, two framings of the cover | 621,015 | 625,023 | 0 | 100 % | 625,023 | 23.15 ms | 4.17 ms | 23.00 ms | **25,464, 9,450, 16 %, 131,439, 4.87 ms** (4.8×) |
| FADE the drift, raw (10 shaded columns), toward a cover | 1,344,753 | 1,335,485 | 0 | 100 % | 1,335,485 | 49.46 ms | 8.90 ms | 49.81 ms | **217,272, 7,425, 92 %, 297,934, 11.03 ms** (4.6×; the 16 columns before the first block stay in C) |
| FADE the drift toward a cover | 1,273,764 | 1,277,768 | 0 | 100 % | 1,277,768 | 47.32 ms | 8.52 ms | 47.18 ms | **22,266, 8,250, 18 %, 111,891, 4.14 ms** (11.8×) |
| the same, in phase (x 64), at the 304 pitch (2026-10-24) | 1,277,861 | 17,696 | 7,500 | 23 % | 97,946 | 3.63 ms | 0.65 ms | 47.33 ms | 19,566, 7,500, 20 %, 99,816, 3.70 ms (+2 %) |
| BLIT 160 × 200 from a 320-pitch slot | 294,215 | 7,659 | 2,210 | 28 % | 10,714 | 0.40 ms | 0.07 ms | 10.90 ms | 7,892, 2,210, 26 %, 10,947, 0.41 ms |
| FADE 160 × 200 toward a 320-pitch slot | 30,338 | 31,606 | 6,000 | 65 % | 95,806 | 3.55 ms | 0.64 ms | 3.50 ms | 33,114, 6,000, 62 %, 97,314, 3.60 ms |
| SCROLL the list, 320 × 220 by a row (25) | 14,281 | 13,068 | 4,420 | 16 % | 19,178 | 0.71 ms | 0.13 ms | 0.74 ms | 13,562, 4,420, 15 %, 19,672, 0.73 ms |
| SCROLL the split list, 160 × 220 by 25 | 7,678 | 7,740 | 2,210 | 28 % | 10,795 | 0.40 ms | 0.07 ms | 0.39 ms | 7,974, 2,210, 27 %, 11,029, 0.41 ms |
| FILL a list row, 320 × 25 | 16,458 | 4,347 | 1,000 | 14 % | 4,847 | 0.18 ms | 0.03 ms | 0.61 ms | = |
| FILL the full frame | 155,136 | 37,675 | 9,600 | 4 % | 42,475 | 1.57 ms | 0.28 ms | 5.75 ms | = |
| FILL 150 × 25 at x 5 (unaligned) | 7,958 | 3,497 | 400 | 53 % | 3,697 | 0.14 ms | 0.02 ms | 0.29 ms | = |
| BLIT FRONT → BACK, the full frame | 698,558 | 15,046 | 5,100 | 16 % | 22,096 | 0.82 ms | 0.15 ms | 25.87 ms | 15,614, 5,100, 14 %, 22,664, 0.84 ms |
| BLIT a marquee strip, 200 × 16 at x 100 (phases differ) | 29,678 | 13,666 | 0 | 100 % | 13,666 | 0.51 ms | 0.09 ms | 1.10 ms | **3,333, 221, 82 %, 4,104, 0.15 ms** (3.3×) |
| BLIT with a key colour, 160 × 200 | 358,215 | 9,171 | 3,380 | 23 % | 30,361 | 1.12 ms | 0.20 ms | 13.27 ms | 9,040, 3,380, 23 %, 30,230, 1.12 ms |
| PRESENT the full frame (the job; the DMA runs on) | 550 | 547 | 0 | 100 % | 547 | 0.02 ms | < 0.01 ms | 0.02 ms | = |
| PRESENT the panel, 160 × 240 (a 2-D DMA) | 609 | 606 | 0 | 100 % | 606 | 0.02 ms | < 0.01 ms | 0.02 ms | = |
| ANIM SLIDE, one 320 × 240 frame (offset 26) | 1,568,488 | 337,347 | 0 | 100 % | 337,347 | 12.49 ms | 2.25 ms | 58.09 ms | **104,486, 4,815, 88 %, 121,294, 4.49 ms** (2.8×; rect B is drawn from the frame itself, in C) |
| ANIM PAN, one 200 × 16 frame | 13,661 | 5,965 | 0 | 100 % | 5,965 | 0.22 ms | 0.04 ms | 0.51 ms | **2,755, 221, 79 %, 3,526, 0.13 ms** (1.7×) |
| COMMIT a 300 × 300 cover, checked: F16w (rev 0.2, 2026-10-24; before: FNV-1a) | 1,082,147 | 315,979 | 0 | 100 % | 315,979 | 11.70 ms | 2.11 ms | 40.08 ms | = |
| FENCE, ALLOC (a hit), FREE, STATS, ANIM_STOP, RESET; ANIM's own job | 279–630 | 279–633 | 0 | 100 % | 279–633 | ≤ 0.02 ms | < 0.01 ms | | = |

**After the raw FADE** (2026-10-24, measured on this tree against `d6f21a2b48`; [sim, estimate]):

| Job | Instructions, vector, C share, cycles, 27 MHz: before | After |
|---|---|---|
| FADE the drift, raw (10 shaded columns), toward a cover | 217,272, 7,425, 92 %, 297,934, 11.03 ms | **24,850, 8,355, 21 %, 115,428, 4.28 ms** (2.6×) |
| SHADE the shadow strip, 10 × 240 | 4,845, 510, 82 %, 8,925, 0.33 ms | **2,457, 510, 64 %, 6,537, 0.24 ms** (factors once a job) |
| FADE the drift toward a cover (out of phase) | 22,266, 8,250, 18 %, 111,891, 4.14 ms | 24,468, 8,250, 21 %, 114,093, 4.23 ms (+2 %) |
| the same, in phase (x 64) | 19,566, 7,500, 20 %, 99,816, 3.70 ms | 21,708, 7,500, 23 %, 101,958, 3.78 ms (+2 %) |
| AVG the drift's half step | 25,464, 9,450, 16 %, 131,439, 4.87 ms | 27,666, 9,450, 19 %, 133,641, 4.95 ms (+2 %) |
| FADE 160 × 200 toward a 320-pitch slot | 33,114, 6,000, 62 %, 97,314, 3.60 ms | 34,872, 6,000, 62 %, 99,072, 3.67 ms |

**The marquee** (rev 0.3, 19-5, 2026-10-24) [sim, estimate]. The rows are the stepped PAN, 290 × 18 (the list's text cell at x 5) from a strip of 992 px (the cell at rest, a 400 px lap, the cell's width again, rounded to 16), at step 7 (source x 304 against x 5: out of phase):

| Job | Instructions | Vector | C share | Cycles | 27 MHz | 150 MHz |
|---|---|---|---|---|---|---|
| `ANIM PAN` stepped (the job) | 514 | 0 | 100 % | 514 | 0.02 ms | < 0.01 ms |
| `ANIM PAN` stepped, one frame | **4,644** | 682 | 61 % | **7,025** | **0.26 ms** | 0.05 ms |
| the same, its present not widened (x 5: the CPU push) | 19,253 | 682 | 91 % | 21,634 | 0.80 ms | 0.14 ms |
| the same, from a strip of 981 px (odd: the C copy) | 15,606 | 0 | 100 % | 15,606 | 0.58 ms | 0.10 ms |
| `ANIM PAN` linear, one 200 × 16 frame (before: 2,755, 3,526) | 2,753 | 221 | 79 % | 3,524 | 0.13 ms | 0.02 ms |

- **The present:** a frame's present at an odd x can't use the LCD DMA (`dma_lcd_start`: word-aligned pixel pairs), and `lcd_blit` pushed it with the CPU: 66 % of the frame. The stepped form presents its rect widened to an even x and width, so the DMA takes it; the columns either side are the panel's own (`jobs.h` `J_PAN_STEP`).
- **The strip's width:** rounded to 16 px, the strip's rows start 32-byte aligned, and the window's 16-row group gathers on the vector unit (`jv_gather16`; the second group goes again over the first's last rows, `RUN_AGAIN`). An odd width is all C, 2.2× slower.
- **At 20 frames a second:** 5.2 ms of the BCM's second at 27 MHz (0.5 %), or 0.9 ms at 150 MHz.
- **What it replaces on the PP:** F2's step, every 50 ms: an `lcd_bitmap_part` of the cell into the framebuffer, the scrollbar over it, and a push of the 320 × 25 row band through the port (8,000 pixels at about 10.5 cycles a transfer [silicon S8]).

The plain FADE and AVG pay about 14 instructions a block: `jv_mix16` loads its factor argument and tests it once a channel, and the hook passes it. The self-check's `span()` test adds about 0.1 % to the BLITs. Other rows are unchanged.

**After rev 0.4: the retained screen** (2026-10-24, §12; [sim, estimate]; the SLIDE and SCROLL rows of the table above are superseded by these):

| Job | Instructions, vector, C share, cycles | 27 MHz | 150 MHz |
|---|---|---|---|
| ANIM SLIDE, one 320 × 240 frame from BACK into the frame (the row above; `to` is the front frame) | 45,004, 5,730, 66 %, 64,969 | 2.41 ms (was 4.49) | 0.43 ms |
| **ANIM SLIDE push, 320 × 220 out of the front frame, a step of 27** (odd: the in-place move pixel by pixel) | 253,096, 854, 99 %, 256,043 | 9.48 ms | 1.71 ms |
| **ANIM SLIDE pop, the same, a step of 27** | 310,860, 854, 99 %, 313,807 | 11.62 ms (18.08 before the word path) | 2.09 ms |
| ANIM SLIDE push, a step of 26 (even: words) | 106,121, 854, 98 %, 109,068 | 4.04 ms | 0.73 ms |
| ANIM SLIDE pop, a step of 26 | 164,106, 854, 99 %, 167,053 | 6.19 ms | 1.11 ms |
| BLIT BACK → FRONT 320 × 220 (a slide's landing) | 14,646, 4,760, 15 %, 21,226 | 0.79 ms | 0.14 ms |
| **SCROLL the list 320 × 220 by −8, −17, +23** (glide steps) | 13,601–14,335, 4,420–4,580, 15–16 %, 19,711–20,525 | 0.73–0.76 ms | 0.13–0.14 ms |
| BLIT BACK → FRONT 320 × 17 (a glide's new rows) | 2,466, 680, 28 %, 3,406 | 0.13 ms | 0.02 ms |
| BLIT BACK → FRONT 8 × 220 (the scrollbar column) | 10,242, 0, 100 %, 10,242 | 0.38 ms | 0.07 ms |
| PRESENT the list 320 × 220 (the job; the DMA, about 13 ms, runs on) | 623, 0, 100 %, 623 | 0.02 ms | < 0.01 ms |

- **A retained slide frame** moves the whole old part in place, in C: the vector kernels go left to right with every group read before it is stored, which is safe for a left move but needs C's head before the hook, and a right move needs the columns from the end. Rev 0.4 copies words when the step is even and keeps the key test out of the right move's loop: 18.1 → 11.6 ms for a pop frame. Every frame still fits 33 ms at 27 MHz with the DMA alongside. **Next, if a bench row shows it matters:** a group pass for left moves (C's head first, no last group again) and a mirrored kernel for right moves, about 4× on these frames [estimate].
- **A glide frame** costs the BCM about 1.3 ms at 27 MHz (SCROLL, two BLITs, PRESENT's set-up, FENCE) against the PP's 14–27 ms engine-2 push of the view; the PP uploads 17 rows (10.9 KB, about 1 ms by engine 2) and the bar (3.5 KB, CPU) instead of 140.8 KB. The scrollbar column's BLIT is the frame's largest part: 8 pixels wide, all C.

**The last column** is the lane shift's (`jv_gather16`), measured on the tree it starts from (`70af21dc52`, whose own figures differ from "after" by a few per cent where `jmix.h`'s one-multiply FADE and jdec's changes landed since): the ratios in it are against that tree. Bold rows are the out-of-phase ones; "=" is unchanged. The in-phase rows pay about 2 % for the phase test (two instructions a column, eight a FADE block).

"Before" is the tree at `8d7438c` (rows16, mix16 and shade16 only), except COMMIT's: FNV-1a (`fnv_px`) just before rev 0.2. The audit now steps the core to `hp_poll`'s entry before it sends each job; stopped inside a pass, the core could otherwise run the job before the profile starts.

**Findings:**
1. **The drift's cover slots never reached the vector unit.** A 300 × 300 slot's rows were 600 B apart, which isn't a multiple of 32, so only one row in four could line up for any window. The drift's FADE and AVG stayed in C: 47–49 ms and 23 ms a step at 27 MHz, 8.5–8.9 and 4.2 ms at 150 MHz.
   - **Done (2026-10-24): drift slots at a 16-pixel pitch** (`J_SLOT_PITCH`, 304 for a cover; `J_BUDGET_DRIFT` 364,800 B, 4,352 B more, paid by `J_CLS_APP`, `memory-contract.md` §4). jref, the model's upload and `jobs_bcm.c` honour it; the PP driver's `bcmjob_upload` writes rows at `ALLOC`'s pitch (19-L2).
   - **The gain needs the window in phase with the frame:** the cover's window x and the panel's x (160) equal mod 16. In phase, the FADE is **3.63 ms (13× less)** and the pan BLIT 0.46 ms (5.3× less) [sim]. Out of phase (x 70, the rows above) nothing changes: 47.3 ms. AVG's two framings are a pixel apart, so they can never both be in phase: the half step stays 23 ms in C.
   - **The decision (the orchestrator, 2026-10-24): pans stay smooth, rests are aligned.**
     - The moving drift window is **never** snapped to 16 px steps. Smooth motion is the point ("smooth and cared for"), and a pan BLIT at C speed (2.44 ms) fits a 33 ms frame.
     - The P19 shell places a cover's **rest position** (where it sits while it fades) at a 16-px-aligned x. That makes every FADE in phase: 3.63 ms instead of 47.3 ms.
     - For the AVG half step, it picks the framing that is in phase at the rest position.
     - A pixel-exact rest position is invisible; a stepped pan is not.
     - If this doesn't capture most of the gain, the next step is a vector path for out-of-phase sources (an unaligned load and a lane shift).
   - **Done (2026-10-24): the lane shift** (`jv_gather16`, "The lane shift" above). Out of phase, the FADE toward a cover is **4.14 ms (11.8× less; in phase 3.70)**, the pan BLIT at x 70 0.77 ms (3.2×; in phase 0.47), AVG's half step **4.87 ms** (4.8×), whichever framing is in phase [sim]. The rests stay aligned (in phase is still the fastest), but a rest that isn't, or a fade that starts mid-pan, no longer costs 13× more. The raw FADE (10 shaded columns) was 11.0 ms: its first 16 columns (the shaded 10 and the 6 to the first block) were still per-pixel C, 92 % of its instructions.
   - **Done (2026-10-24): the raw FADE's shaded block on the vector unit**, the shade fused into `jv_mix16`'s channels (per-column factors built once a job): **4.28 ms (2.6×)**, 0.14 ms over the plain FADE [sim].
2. **The C share is now edges, unaligned phases and the drift's slots.**
   - Where source and destination line up, the table's C share (every instruction outside the kernels, the hooks' set-up and the ring included) is 4–28 % for BLIT, FILL and SCROLL.
   - SHADE's 82 % was mostly its hook building the 16 factors again for each group. **Done (2026-10-24)** with the raw FADE: both hooks build them once a job (jref passes the alpha words on the first group only), 0.33 → 0.24 ms.
   - The marquee strip (200 × 16 at x 100 from x 37) and SLIDE at easing offsets that aren't multiples of 16 can't line up. Their C copy now moves words: 2.2× and 4.6× faster than before. **Since the lane shift (2026-10-24)** their groups gather on the vector unit: the strip 0.15 ms (3.3×), a SLIDE frame 4.49 ms (2.8×; its rect B is drawn from the frame itself and stays in C), a PAN frame 0.13 ms. What C keeps is the columns before and after the aligned blocks, and `jv_rows`'s out-of-phase rows.
   - A SLIDE frame now fits a 33 ms frame at 27 MHz (12.5 ms, was 58 ms). Since rev 0.4 (every part a `BLIT`) the row's frame is 2.41 ms; the retained slide's frames are in the table below (§12).
3. **`jv_store16` is the vector copies' main cost** (50–56 % of a copy's cycles): 16 single stores and 16 base moves a group.
   - A REP-16 `vst16` would be one instruction of about 16.5 cycles instead of about 40. Session 8's informational check of that form is the bench row to promote first.
4. **Per-job overhead is small:** about 280–630 instructions (10–25 µs at 27 MHz) for the ring read, the dispatch's copy of the record into `rec[]`, `jref_exec`'s checks and surface lookups, `publish()` and the reply.
   - That is under 1 % of any drawing job, so no cut was worth the bytes. `jobs_dispatch` copies the payload twice (`hp_poll`'s `args[]`, then `rec[]`), about 2 instructions a word.
5. **PRESENT's setup is about 550–600 instructions**, including `lcd_window` and `dma_lcd_start` (13–15 %): 0.02 ms. The transfer itself runs on the DMA engine, 14.3 ms for a full frame [silicon S7]. `lcd_blit` waits for the previous transfer (`lcd_blit_wait`) only at the next PRESENT or LCD command, so drawing jobs right after a PRESENT overlap its DMA. Those jobs draw into the frame the DMA is still reading, so a job that changes the presented rect before the transfer ends can tear. The shell should FENCE (or present with `J_PRES_WAIT`) before redrawing that rect.
6. **COMMIT's FNV-1a check of a 300 × 300 cover took 40 ms at 27 MHz, 7.2 ms at 150 MHz**: over `J_T_SMALL`'s 5 ms deadline at either clock. FNV-1a is serial: two multiplies a pixel, 12 instructions.
   - **Done (2026-10-24):** the check stays, and COMMIT's deadline scales with the slot's bytes: `J_T_COMMIT(bytes)`, 5 ms plus 1 ms per started 2 KB (§6), about twice the measured rate (a cover: 94 ms). The model's slow-job marking uses it, and the PP driver reads the same constant.
   - **Done (rev 0.2, 2026-10-24): F16w replaces FNV-1a** (§4; `algorithms.md` §7). A cover's check is **315,979 instructions, 11.7 ms at 27 MHz and 2.1 ms at 150 (3.4× less)** [sim]. `j_f16w()` takes eight pixels a turn: 3.5 instructions a pixel against the plain loop's 7, whose counter and compare cost more than the sums. A loop over words on two chains (W = Σ words and H = Σ high halves, A and B rebuilt from them) measured the same 3.5, so the simple form stayed. The images grow by 160 bytes (`0xA0`): the display-only image's image+bss ends at `0xb640` (was `0xb5a0`), the merged image's 160 bytes higher too [sim].
7. **Cheap wins taken:**
   - every hook returns a span, so unaligned heads stay vector;
   - SHADE's whole blocks put the shadow strip on the vector unit (13× fewer instructions, 7× fewer cycles);
   - BLIT's last group goes again (no C rows for apart BLITs of 16 rows or more);
   - word copies in C;
   - rows the hook did whole are skipped in C;
   - pointer walks in `op_mix`/`op_shade`;
   - AVG on FADE's kernel;
   - the 64-bit divide gone.

   Every result stays bit-exact (the rows above, and jscene's golden).

## 11. Gaps from the P19 brief (§3.7), and where each is closed

| Gap | Closed in |
|---|---|
| G1 a full ring | §3, §10: `BCMJOB_BUSY` at once, the frame's CPU path |
| G2 a failure while lit | §6: restart at once, the PP framebuffer first; a slow live BCM never cut while lit |
| G3 one lock | §6, `bcmjob.h` `bcm_port_lock` |
| G4 the executor in the PP build | §10: `HAVE_BCM_JOBS`, 4.2 KB Thumb without the BCM's hooks and `DECODE` (`JREF_HOOKS`, `JREF_DECODE`) |
| G5 one FADE/AVG definition | `bcmfw/jobs/jmix.h`, used by `jref.c`; widened by review H2 to the shade (`ui_shadow_apply`); the shell's `xfade_rect()`, `AVG2` and `ui_shadow_apply()` call it (a shell change for P19) |
| H2 the split shadow | `SHADE` and `FADE`'s shade flag (§5, "The drift as jobs"); B3 re-specified |
| M3 failure mid-SCROLL or mid-ANIM | §6: resync from the PP framebuffer; a slide cut short jumps to its end; `bcmjob_resync`; every path in §12.5 |
| M4 developer stages | §6: `bcmjob_suspend` around `take()`/`restore()` |
| G6 SLIDE easing | `jobs.h` `J_ANIM_SLIDE` now carries the shell's per-frame offsets (up to 48 since rev 0.4: the record's limit): frame k shows offset k; §12 builds the table from the shell's easing |
| G7 a Class B pool | `J_CLS_APP` (51.5 KB, 8 slots; 56 KB until the drift's pitch), `memory-contract.md` §4 |
| G8 class budgets as constants | `jobs.h` `J_BUDGET_*`, `J_SLOTS_*` |
| G9 a display-only resident image | §10: 42,964 B with `DECODE` (21,304 B without it); the warm swap built and passing in vc2sim (`warm-swap.md`); waits for bench row B13 |
| G10 the PP framebuffer stays | `memory-contract.md` §5: 153,600 B kept; the strip saving there is the shell's strip buffer, not the framebuffer |
| G11 a power request API | §10, `bcm_power_want` |
| G12 dark power-ups | §10, `bcmjob_dark_ups` and the hourly log line |
| G13 epoch notice | §10, `bcmjob_on_epoch` |
| G14 the PP side per power state | `docs/design/p19-placement.md` (the placement plan) |


## 12. Shell recipe: retained pages, slides and glides

P19 19-4 (#75, #76, #78 b). The wiring of the shell on `p19` (`apps/ipodui/ui_main.c`, `list.c`, `preview.c`) to rev 0.4, function by function. [ours: a design; the executor, the model and the merged image run it in `jmtest` and `jobs_image_test.py`'s row "retained" (§12.7); nothing here has run on silicon.]

### 12.1 What is retained, and who holds the truth

- **`J_SURF_FRONT`** is the page the panel shows. Outside a slide in flight it equals the PP framebuffer pixel for pixel: every LCD push writes it, and every job frame below ends equal to the CPU's frame.
- **`J_SURF_BACK`** (`0x105800`, 320 pixels a row, both images) holds a slide's new page while the slide runs, and a glide's newly shown rows for one frame. Nothing else in UI mode uses it (`bcmjob_selftest` is a developer row; a video session takes it, with a new epoch).
- **The PP framebuffer stays the truth** (brief §3.6) and always holds the final framing: the new page from the moment a slide starts, every glide frame as the CPU composes it. So `bcmjob_resync()` of any rect is always the right picture.
- **The pixels cross the port once:** a slide's new page by engine 2 (140,800 B for the 320 × 220 body, about 13 ms [estimate from silicon S37's 11 MB/s]); a glide frame's new rows and the scrollbar column (about 1 ms).
- **The PP puts them in BACK with `bcmjob_upload_rect(J_SURF_BACK, …)`** (`bcmjob.h`, rev 0.4): not a job, so the bytes land at once, outside ring order. Rule: **nothing queued may still read that rect**. The previous glide frame's fence has retired, and a slide has landed, before BACK is written again.
- **Its answers** (the driver review, 2026-10-24): `OK`; `OFF`; `FAILED` for another surface, a rect outside the frame or `pitch < w`; **`BUSY`** when the call's time budget (about 4 ms: the driver's `BJC_RECT_US`, the model's `JM_RECT_MS`) ran out with rows left, or a `DECODE` is in flight. `BUSY` is never counted; the caller draws that frame by its CPU path. A rect larger than the budget (about 44 KB, 68 full rows) goes up in bands, one call each.

### 12.2 The per-transition decision

`slide_start()` decides **once per slide**, before the new screen draws (the BCM path draws no capture, so it can't fall back to the CPU's frames later). The BCM path needs all of:
- `bcmjob_have()` for `J_OP_ANIM`, `J_OP_ANIM_STOP`, `J_OP_BLIT`, `J_OP_PRESENT` and `J_OP_FENCE`;
- a plain slide: `slide.part == 0` and not `slide.reveal`. The split ↔ full parting (`part_frame()`) and the Clocks reveal (`reveal_frame()`) keep their CPU frames;
- a table of at most `J_SLIDE_MAX` (48) frames; every slide today has at most 10 (290 ms);
- no slide or glide jobs still unretired;
- no `bcmjob_decode()` running (the art builder's: its uploads would answer `BUSY`).

Otherwise it's today's CPU slide: `slide_capture()`, `slide_shift()`, `rect_xfer()`, frame for frame the executor's (§12.7).

`glide_frame()` decides **per frame** (§12.4). It can change its mind at any frame, because the CPU composes every frame into the framebuffer either way.

### 12.3 Push or pop (and `ui_slide_band()`'s page slide)

`slide_start()` → `slide_capture()`, the BCM path:
1. `ui_fb_settle(rect)` as today, then `ui_anim_begin()` for the compose only. Note `t0 = current_tick` for the log line.
2. `draw_dirty(s, UI_DIRTY_ALL)`: the new screen into the framebuffer. **No strip claim, no `rect_xfer()`**: the old page is the front frame.
3. `bcmjob_upload_rect(J_SURF_BACK, …, FBADDR(slide.x, y), LCD_WIDTH)` for the rect in bands of at most 64 full-width rows (four calls for the 220-row body), each within the call's budget. The framebuffer is static, so it can't move during engine 2's wait. A `BUSY` from any band (the budget, or a `DECODE` in flight) is §12.5's first row: no motion, the new page at once (there is no capture for the CPU's slide).
4. **The offset table, from the shell's own easing.** Factor `slide_frame()`'s cubic ease-out into a pure `slide_want(el_ms, dur_ms, w)`: `w` once `el_ms >= dur_ms`, else `w - w * rest / 1024` with `inv = 1024 - el_ms * 1024 / dur_ms` and `rest = inv * inv / 1024 * inv / 1024`. It's the same integers as today's ticks for every CPU frame (both ×10). Then offset k = `slide_want(10 + 100 * k / 3, slide.ticks * 1000 / HZ, slide.w)` for k = 0, 1, …, up to the first k that gives `slide.w`, so the last frame is the whole new page. That's the CPU's first frame (one tick in), then a frame every 33.3 ms, the BCM's `FRAME_US`. `jmtest`'s `slide_table()` and `jobs_image_test.py`'s `slide_offsets()` are the same function.
5. **One record and its fence:** `ANIM` with tag `J_TAG_REPLY | n` and `{ J_ANIM_SLIDE, J_SURF_FRONT, J_SURF_BACK, J_XY(slide.x, slide.y), J_XY(slide.w, slide.h), dir, 33, count, offsets… }`, where dir is 0 for a push (`slide.dir > 0`: everything moves left) and 1 for a pop. Then `bcmjob_fence()` and `bcmjob_wait(seq, J_T_FENCE_BASE + J_T_SMALL)`.
   - The BCM draws frame 0 in its next pass. Each frame is `jref_anim_frame()` into the front frame plus a push of the rect.
   - Log `anim first ms=<(current_tick - t0) * 10> kind=push|pop|band src=bcm` (`ui_log("anim", "first ms=%ld kind=%s src=%s", …)`, as the reveal's line).
6. Outside the rect, as today: the header push (`push_above && slide.y > 0`) and `ui_panel_sync()`.
7. **Mark the rect owned** (brief §3.6) until it lands: every CPU push clips against it, and `ui_fb_settle()` over it lands it (§12.5). `bcmjob_owned_set()` holds one rect today, the drift's panel, and a split screen's list-column slide runs beside it. **PP item:** a second owned rect, or the per-band mask the brief names.
8. Set `slide.bcm = true`, `slide.epoch = bcmjob_epoch()`, `slide.errors = bcmjob_errors()`, `slide.end = t0 + 1 + (count − 1) × 10 / 3 ticks + 1`. Then `ui_anim_idle()`: **the PP holds no boost and draws nothing while the BCM slides.** The main loop wakes at `slide.end`, or at the first input.

`slide_frame(finish)`, the BCM path:
- **Before `slide.end` and not `finish`:** if `bcmjob_epoch() != slide.epoch`, the BCM was cut or restarted, and its first frame was the framebuffer (the new page). Clear the owned rect, set `slide.dir = 0`, done. Otherwise return.
- **At `slide.end`, or on input (`finish`, the main loop's "land a slide first"), it lands:**
  - `ANIM_STOP`, `BLIT` BACK → FRONT over the rect, `PRESENT` of the rect with `J_PRES_WAIT`, `FENCE`;
  - then `bcmjob_wait(seq, J_T_FENCE_BASE + J_T_SMALL + J_T_DRAW + J_T_PRESENT)`.
  - The landing is one sequence for both cases. At the natural end the rect already holds the new page, and the BLIT and PRESENT repeat it (0.8 ms of BCM time and an invisible push of the same pixels). On input it jumps to the new page, as the CPU's `slide_frame(true)` does.
  - Clear the owned rect, set `slide.dir = 0`.
- **The tear rule (host-driver.md §9 MUST)** holds by construction: the landing presents with `J_PRES_WAIT`, so its fence retires only after the panel's DMA has read the rect, and nothing writes the rect before that fence. The ANIM's own frames are 33 ms apart against a 14.3 ms push.

### 12.4 A glide

`ui_list_tick()` → `glide_frame()`:
1. **Compose as today:** rows from the strip slots, `draw_overlay()`, `draw_scrollbar()`, into the framebuffer. Everything except the final `ui_lcd_push()`.
2. **Decide (BCM vs CPU).** `d = pos − l->shown_pos`, a new field: the pos the panel shows. Take the BCM frame only if every one of these holds:
   - `bcmjob_have()` for `SCROLL`, `BLIT`, `PRESENT` and `FENCE`;
   - `l->synced` (below);
   - the previous BCM frame's fence retired: `bcmjob_wait(l->seq, J_T_FENCE_BASE + J_T_PRESENT)`. It's normally already done, since the frame is 30 ms after it;
   - 0 < |d| < `view_h(l)`;
   - the quick-scroll letter is not shown (`ui_qscroll` on that list's overlay: a fixed square over moving rows);
   - no slide in flight over the rect.

   Fast-spin jumps (`l->jump`) are whole-row steps and qualify.
3. **The BCM frame:**
   - `bcmjob_upload_rect(J_SURF_BACK, …)` for the newly shown rows: the bottom `d` rows of the view when the list moves up (d > 0), the top −d otherwise. Then the scrollbar's column, the full view height.
   - Then the jobs: `SCROLL` FRONT, view rect, by −d; `BLIT` BACK → FRONT over the rows; `BLIT` BACK → FRONT over the column; `PRESENT` FRONT view rect with `J_PRES_WAIT` (`| J_PRES_TE` once stage 3d measures the line); `FENCE` into `l->seq`.
   - No wait in the frame: the next frame waits.
   - Every one of these must return `BCMJOB_OK`. Any other answer is a fault (§12.5).
4. **The CPU frame:** `ui_lcd_push(l->jump ? "anim:jump" : "anim:scroll", …)` as today. That push writes the front frame through the LCD contract, so `l->synced` becomes true **only if no jobs of the list were unretired** at the push (its last fence had retired, or there were none). Otherwise it stays false.
5. **The trace stays the same for both:** the BCM frame logs the same `update anim:scroll y= h=` line, through `UPDATE_TRACE` without the push, and counts in `modclass_push()`. `check_trace.py`'s frame counts then hold on either backend.
6. **The first frame of a glide** (from `start_glide()`): log `anim first ms=<since the wheel event> kind=glide src=bcm|cpu`, after the frame's fence (BCM) or push (CPU).
7. `l->shown_pos = pos` either way.

**Why the framebuffer still composes every frame:** it keeps the truth, and it is the CPU fallback at any frame. The saving is the push: about 140,800 B by engine 2 every frame (14–27 ms) becomes about 14 KB plus five records [estimate]. Whether the compose can drop the frame's boost is a bench question (the `anim frame … cpu=` trace at 30 MHz).

### 12.5 Faults, every path → resync

| Where | What the shell sees | What it does |
|---|---|---|
| `slide_start()`, the upload (a band's `BUSY` included) or the `ANIM` | `OFF`, `BUSY`, `FAILED`, or the fence `TIMEOUT` | the framebuffer already holds the new page: `bcmjob_resync(rect)`, no motion (the M3 jump), `src=cpu` is not logged (nothing moved) |
| mid-slide, the BCM cut or restarted | `bcmjob_epoch() != slide.epoch` | nothing: the restart's first frame was the framebuffer, the final page (`jmtest`: a cut and a death mid-slide) |
| mid-slide, a job of it failed | `bcmjob_errors() != slide.errors` | land now |
| the landing, `ANIM_STOP` `BUSY` | the ring is full | `bcmjob_resync(rect)` now, and try the landing again on the next pass. The ANIM runs to its own last frame meanwhile, which is the same new page, so the panel can show at most a few intermediate frames again (`jmtest`: "a full ring at the landing") |
| the landing, any other fault (`OFF`, `TIMEOUT`, a new epoch or a new error after its fence) | | `bcmjob_resync(rect)`. A dead BCM's `TIMEOUT` cuts and restarts it, and the restart shows the framebuffer (`jmtest`: "a death mid-slide") |
| a glide frame, an upload's `BUSY` (its budget, or a `DECODE` in flight: not counted), any job `OFF`, `BUSY` or `NOJOB`, its fence `TIMEOUT`, or `bcmjob_errors()` grown since the frame | | the CPU frame for this one (§12.4 step 4), `l->synced = false` |
| the next glide frame with `!l->synced` and jobs available | | `bcmjob_resync(view)` once, after the list's last fence retired. Then `l->synced = true` and BCM frames again (`jmtest`: "the BCM cut mid-glide": CPU frames through the cold start, then one resync, then jobs) |
| an epoch change (`bcmjob_on_epoch()`) | | `l->synced = false` for every list, `slide` as above |

**`bcmjob_resync`'s order.** Its contract is to come after the jobs already queued. The model now does that (§6). The device driver doesn't yet: `bcmjob-core.c` pushes at once. Until 19-L2 fixes it, the "synced only after a retired fence" rule above makes the shell safe on either.

### 12.6 What the shell must not do

- Write BACK while a job that reads it is queued (§12.1).
- Redraw a presented rect before its `J_PRES_WAIT` fence retired (host-driver.md §9).
- Send a SLIDE whose offsets decrease (`J_E_RANGE`), or more than 48 of them.
- Send two animations at once. The image runs one: a SLIDE replaces a running PAN, so 19-5's marquee stops before a slide, as `marquee_reset()` already does on a screen change. The drift (per-frame jobs, no `ANIM`) runs on beside a split screen's list-column slide.

### 12.7 Exactness, and the tests

| Check | Where | What |
|---|---|---|
| each frame the BCM draws = the shell's CPU slide at that offset | `jobs_image_test.py` row "retained", vc2sim | push and pop of the 320 × 220 body (190 and 290 ms), a full-screen push (250 ms), a split list column (160 × 220), and a band up and down against the model. Each drawn frame is compared with `cpu_slide()`, a Python transcription of `slide_shift()` and `rect_xfer()`, and the panel after the last |
| each glide frame = the CPU's compose | the same row | the shell's steps (`scroll.c` `ui_scroll_step()`, three ticks a frame) over seven wheel moves, 24 frames: `SCROLL`, the rows and the bar through BACK, `PRESENT` `J_PRES_WAIT`; front frame and panel. Also an unaligned `PRESENT` rect |
| in-row right `BLIT`s in words, odd heads, keyed | row "BLIT, FILL, ANIM" | the pop's move |
| the recipe on the host model | `jmtest` | slides frame by frame against the CPU's formula; a death mid-slide and a full ring at the landing, both ending on the final page; a glide equal to the CPU's frames, the BCM cut mid-glide, CPU frames, a resync, then jobs again; the resync before a queued `SCROLL`; `bcmjob_upload_rect`'s `FAILED` (`pitch < w`), its `BUSY` past the budget and during a `DECODE`, a page in bands; the screen kept in BACK through the unlit face and shown again at the wake (`power-contract.md` §6) |
| the executor itself | `jtest`, `jscene` (golden 0x73428E8F, host and vc2sim) | the slide from the front frame in place, offsets that go back refused |

A mutation (the slide reading the front frame without its last offset, as before rev 0.4) fails 29 of the row's 188 cases.

### 12.8 Costs and savings [sim, estimate]

The BCM's frame costs are in §10's table "After rev 0.4". Per transition:
- **A slide:** today the PP draws the capture and swaps it (60–100 ms set-up), then about 6 full pushes of the rect by engine 2 (14–27 ms each), boosted. With the recipe: one 13 ms upload, one record, then nothing until the landing's four records. The BCM spends 4–12 ms a frame of its 33 ms.
- **A glide frame:** about 14 KB up instead of 140.8 KB, five records, 1.3 ms of BCM time.
- **Wire:** rev 0.4 changes no record layout. `bcmjob_upload_rect` is PP-side only.
- **Room:** against the marquee's tree the merged image shrinks by 96 B (218,756 → 218,660 B; 100 B spare below the arena), the display-only image by 128 B (45,344 → 45,216 B).

### 12.9 Left for the wiring sessions

- **The shell** (`p19`):
  - §12.3–12.5 in `ui_main.c` and `list.c`;
  - `slide_want()` factored out of `slide_frame()`;
  - `shown_pos`, `synced` and `seq` in `struct ui_list`;
  - a second owned rect.
- **The driver** (19-L2):
  - `bcmjob_upload_rect` in `bcmjob-core.c`: engine 2 to `J_SURF_BACK_ADDR` at a 640-byte pitch, under the port lock. The simulator's `sim_bcm_job.c` wrapper needs it too;
  - `bcmjob_resync` after the ring (§6).
- **The bench:** the merged image's FNV changes (`cd8e5d96`, 218,660 B), so `bench-p19.md`'s run sheet line is updated. B3's A/B gains slides and glides.
