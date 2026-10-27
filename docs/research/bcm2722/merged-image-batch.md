# The image batch of 2026-10-25 (p19-image-batch-2)

**What:** one batch of image changes behind one device re-verify (sitting 2): the corrected halfword loads, the hibernate PARK hook, the block clocks, every slot's pitch rounded to 16 pixels, and bench row HWLD; plus the PP-side odd-width fix. It replaces `p19-image-batch` (whose staged upload, SDRAM idle, `sdi` row and warm-swap hand-over are left out: below).

**Tags:** [silicon] measured on the device, [sim] vc2sim only, [ours] our design, [hypothesis].

## 1. Why: the halfword loads (`../../bcm2722-reference/quirks.md` #16, #33)

The 2026-10-25 Port write check (`bench-20261025-1819`) showed `COMMIT` failing with every port readback right. `jref_f16w` loads `px[1..7]` of every 8 with the 32-bit `0x29`/o1 halfword load. On silicon that form **sign-extends**; our model and compiler had it as the zero-extending `ldh`. All 12 logged BCM sums fit that rule exactly [silicon]. The mirror (`0x28`/o1 zero-extends) was quirk #16 since S17. So vc2cc compiled every `uint16_t` read at an offset into a load that, on silicon, returned values of `0x8000` and up as negative numbers.

**The fix** (`tools/bcmre`): vc2dis names `0x28`/o1 `ldh` and `0x29`/o1 `ldsh`; vc2sim decodes them that way (HYP `off16_h`); vc2cc emits `ldh` only in forms proven to zero-extend and keeps signed halfwords as address-then-`ldsh (rs)`. The gp/sp-relative and 48-bit halfword loads are unproven (bench HWLD), so vc2cc no longer emits them: an sp-relative halfword load becomes `lea r15` plus the 16-bit `ldh`.

## 2. The images (before: p19 `caaa9890b0`; after: `p19-image-batch-2`, all items below)

| Image | Before | After | Room |
|---|---|---|---|
| plain/probe `bcmvideo.bin` | 220,900 B `b51bab40` | 220,964 B `948c5a70` | image+bss to `0x34ffc` |
| merged `bcmvideo-jobs.bin` | 218,660 B `cd8e5d96` | 218,692 B `80bc8611` | image+bss to `0x353bc`: **68 B** below the arena (`0x35400`) |
| display-only `jobs_display` | 46,152 B `0aeffaa9` | 46,880 B `f069a36a` | image+bss to `0xc6c0`: 1,344 B below the decode work area, moved from `0xc400` to `0xcc00` (234,496 B, 50,068 above `J_DEC_WORK_MIN`) |
| viz overlay `viz-ovl.bin` | 16,724 B `272e5904` | 16,724 B `313a2202` | — |
| bench / bench_pll | 37,168 `5c3c242d` / 38,256 `d75ef2ce` | 43,872 `fc1b9432` / 44,992 `9c8fedf6` | below the results block |
| hello_3d | 30,200 `5683b011` | 30,212 `e077b5ae` | — |
| hello `98bc5ee7`, hello_rb `b214a817`, hello_dma `28768994`, hello_dma_rb `40864d64`, hello_dma_clk `1df2ae35`, hello_dma_idle `80d34a16`, hello_dma_sby `84ea377d`, drift `1dc7ea24`, probe `84971ead` | (were `96545c17`, `5369cd87`, `4326f8dc`, `26d5742c`, `e62ece11`, `2c3a88ae`, `d1cecb19`, `bd2cb5e4`, `fcf0eff5`) | same sizes | encoding only (`lcd_power_on`, `run_script`) |
| hibstub | 1,072 B `a3d306fd` | unchanged | — |
| staged candidates (`jobs_staged` `2087891f`, `bcmvideo-jobs-staged.bin` `e7f42c0d`) | | removed | |

**Then, `p19-b13` (2026-10-25, B13's cause; `warm-swap.md` §8, `quirks.md` #40):** every image flushes the cache (Apple's @25a) before running code written after its start: the park stub, the PP's hibernate stub, the overlay. Park and `PARK` are compiled `-Os` out of line. Merged `80bc8611` → **`88e710cb`** (218,564 B, image+bss to `0x3533c`: 196 B below the arena); plain/probe `948c5a70` → `7b65c853` (220,260 B); display-only `f069a36a` → `05c1e914` (46,848 B). The re-verify runs on `88e710cb`.

**`hello_dma` was `4326f8dc`, the one image `bcmfw/verified.txt` lists** (the release boot path's DMA rung). It is now `28768994`, behaviour-identical on silicon (§3: its two changed loads feed the 16-bit panel bus and a small delay), but the FNV no longer matches: until sitting 2 re-passes stage 5 and the owner lists the new FNV, a release build's default boot path finds no verified DMA image. `verified.txt` is untouched.

**Size pressure:** the merged image has 68 B left below its arena. The tile kernel's padded copy for odd chroma strides did not fit (the merged image overflowed by 28 B), so `disp_convert` takes the C for such frames instead; the H.264 plane strides are padded at the source, which made that copy unnecessary for our own decoder. The display-only image needed 2 KB more: its decode work area moved up (above).

## 2a. The batch's items

"Driving" items follow Apple's way of driving the hardware, with the [apple] / [apple-RE] row that sets it; "ours" items are our designs on top of it (owner, 2026-10-25: Apple for how to drive the hardware, ours for what we do with it). An item beyond Apple's envelope is marked so and gated by its bench row.

| Item | Kind | Images (effect) | Basis |
|---|---|---|---|
| Halfword loads: `0x28`/o1 for unsigned, address + 16-bit `ldsh (rs)` for signed; no gp/sp-relative or 48-bit halfword loads | driving: [apple] forms only | every image but hibstub (encoding) | `isa-scalar.md` rows `0010 1000`/`0010 1001` [apple] + [silicon] 2026-10-25; Apple uses `0x29`/o1 1,047 times, `0x28`/o1 225 (vc2forms), both proven now; the 2dxx and 48-bit halfword forms stay out until HWLD |
| Bench row HWLD (8 checks) | ours: design (bench) | bench, bench_pll (+768 B) | reads every form, informational |
| PARK hook in every jobs image (`BCMFW_HIB_PARK`) | ours: design (PARK and hibernate as features); its stub's sequence is Apple's | merged −64 B | `power-contract.md` 6.6 [apple-RE] the park's order; needs PW5 on the unit before the PP asks |
| Block clocks off at start (`BCMFW_JOBS_CLK_OFF` `0xE3`: ids 0, 1, 4, 5, 7) | driving: [apple-RE] | merged +32 B, display-only +64 B | Apple's start-up (`vmcs` @`50a8`) turns all eight off and its running context reads 0 in every one (PW1 [silicon]); we turn off only five of Apple's eight and keep the audio (ids 2, 3) and USB (id 6) clocks as found, a subset of what Apple's own context shows off |
| Every slot's pitch rounded to 16 pixels (`J_SLOT_PITCH`) | driving: [apple-RE] | merged and display-only (pitch) | Apple's VC image pitches are width rounded to 16 pixels, 32 bytes (vmcs `0x4a76`); host pitches to 4 bytes (osos `0x10286a1c`); `alignment-audit.md` §4 rank 1 |
| PP port writes 4-aligned: `write_row` (an odd first or last pixel by read-modify-write of its word), the port layer refusing a mid-word write | driving: [apple-RE] aligned port writes; the tail by aligned word read and write | firmware only | Apple never writes the port at 2 mod 4 (`alignment-audit.md` §4); Apple's tail is a lone halfword store (osos `0x10287be8`), which is [ours, beyond Apple] to rely on until ALGN PLH1 shows a lone halfword commits: we use only aligned whole-word reads and writes, which every session has proven |
| DMA units and control blocks | driving: [apple-RE] (unchanged) | — | our LCD push picks 32-byte units when address, pitch and length allow, else 4 (a power of two dividing all three, within Apple's rule of the largest up to 32); control blocks 32-byte aligned (vc2mmio refuses others) |
| jdec's colour stage from aligned planes (three plain `vld8`s into H8 rows; no lone `vldp8` gather), with a start-up check, `J_VPU_YUV` | driving: [apple-RE] plain loads only 32-aligned, `vldp` only with its `vldq` (vmcs `0x1d080`); the kernel and its check: ours | display-only (+, with the check) | `alignment-audit.md` §4 rank 7 |
| H.264 plane strides padded to 32/16 (`H264_PAD_MBW`); the MC edge window at strides 32/16; `disp_convert` in C for an odd chroma stride | driving: [apple-RE] strides multiples of 16 | plain and merged | `alignment-audit.md` §4 rank 6; Apple's pairs always run at strides that are multiples of 16 |
| The silicon-safe vector kernels (branch `p19-vpu-silicon`): the lane shift by transposes, the two-chain IDCT, bench rows ASHX, ACSA, GTHR | ours: design (vector kernels), fitted to silicon's 2026-10-25 words | merged and display-only; bench | `quirks.md` #30–#32 fitted; `vpu_fit_test.py` |
| Strict alignment `trap` by default in every test; host-port models drop bits 1:0 | ours: design (bench and tests) | — | `memory-access.md` §11 |
| vc2mmio: DMA config bit 1 enables (PW8's `0x0000E802`) | tooling | — | PW8 [silicon] |

**Speed of the silicon-safe vector paths against the C** (vc2sim instructions, `p19-vpu-silicon`): out-of-phase BLIT 2.6×, AVG 4.4×, FADE 11×, the marquee (stepped PAN) 3.1×. Both start-up checks pass in vc2sim: `J_VPU` reads 3 in the merged image (CHECKED, SHIFT) and **0x27** in the display-only image (CHECKED, SHIFT, MAC, and the new YUV).

## 3. The audit: every function whose code changed, and what it did on silicon

Method: every image built before and after the compiler change alone; functions compared by instruction text and halfword-load encoding (`0x29`/o1 → `0x28`/o1, sp-relative `2d5x` → `lea` + 16-bit `ldh`). Then every bcmfw test run with the **old** encodings on the **new** model (silicon as now understood), counting each `0x29`/o1 load that read `0x8000` or more, per function. "Wrong on silicon" means an unsigned halfword of `0x8000` or more was loaded by `0x29`/o1 and the sign changed a result; "encoding only" means the function's results are the same either way (values below `0x8000`, or only the low 16 bits used).

| Function | Images | Verdict | Evidence |
|---|---|---|---|
| `jref_f16w` (COMMIT's and OVERLAY's F16w) | merged, display-only, the staged candidates | **was wrong on silicon (unsigned ≥0x8000 via 0x29/o1)** | 1.29 M of 2.71 M traced loads ≥0x8000; the 12 sitting-1 sums; every `COMMIT` of a slot holding such a pixel answered `J_E_SUM`, and every job on it then failed (`J_E_SLOT`). The visualizer overlay's load sum the same |
| `viz_bars` (the vector bars' setup), `viz_bars_init` | viz overlay | **was wrong on silicon (unsigned ≥0x8000 via 0x29/o1)**, caught | 291 and 12 loads ≥0x8000 (bar colours); 81 of 128 frames differ, the start-up check fails, so the overlay keeps the C bars (`jviz_bars_draw`, right). Never shown wrong; the vector bars never ran |
| `r3d_render` (with `raster` inlined) | hello_3d | **was wrong on silicon (unsigned ≥0x8000 via 0x29/o1)** | 218,590 traced loads ≥0x8000 (texels); r3d_test: 12 failures over its 10 scene cases (every CRC differs). Device stage 3d's pictures were not the model's |
| `cone`, `prism`, `terrain`, `r3d_scene` | hello_3d | encoding only [hypothesis for silicon] | sp-relative `2d5x` loads (unproven form) now by address; 0 loads ≥0x8000 traced in `r3d_scene` |
| `emit` | hello_3d | encoding only | 0x29 → 0x28 |
| `jviz_bars_draw` | viz overlay | encoding only | 2,607 loads ≥0x8000 (colours), stored back as halfwords: viz_test's C bars 0 of 61 differ with the old encoding |
| `fft` | viz overlay | encoding only | 56,604 loads ≥0x8000; kernel and C FFT 0 differ against the Mac with the old encoding (the values are stored as 16 bits) |
| `jviz_raw_power`, `jviz_bands`, `jref_spectrum`, `viz_vpu_init` | viz overlay | encoding only | 0 loads ≥0x8000 traced |
| `jpeg_idct2v` | display-only | encoding only | `int16_t` read and written back as 16 bits (GCC used `ldh`): 11,768 loads ≥0x8000, every decode's output golden with the old encoding |
| `jdec_run`, `jdec_work`, `jdec_primary`, `clip_jpeg_fd`, `fan_begin`, `fan_emit`, `resize_push_init`, `recalc_dimension`, `anim_check` | display-only (`anim_check` merged too) | encoding only | 0 loads ≥0x8000 in 18,000 traced; decode outputs golden |
| `jref_exec` | merged, display-only | encoding only | 0 of 33 traced; job record fields |
| `run_script`, `lcd_power_on` | every image | encoding only on silicon | `run_script` loads panel register values with `0x29`/o1: `0xE283`, `0xA404`, `0xEF00` came out `0xFFFF....`, but the panel bus is 16 bits and stage 5 passed on every panel (S7–S17); `lcd_power_on` reads a delay (0 traced ≥0x8000) |
| `hp_poll`, `pw_up`, `pw_restore`, `hib_restore`, `memprobe` | plain (`memprobe` merged too) | encoding only | 0 traced ≥0x8000 (protocol fields, saved register values) |
| `ifilter_bank` | plain, merged | encoding only [hypothesis for silicon] | an sp-relative `2d5x` load (unproven form) now by address |
| H.264: `h264_residual`, `h264_mv_pred`, `h264_finish_picture`, `filter_mb`, `strength_inter`, `store_mb` | plain, merged | encoding only | video_test with the old encodings on the new model: every picture bit-exact against ffmpeg (16 rows, 251 cases). `strength_inter` read 159,235 and `h264_mv_pred` 7,422 halfwords ≥0x8000 by `0x29`/o1 (motion vectors, used only through 16-bit differences), the others none; the decoder was also bit-exact on silicon (S27 onward) |
| AAC: `aac_memcpy`, `NeAACDecDecode`, `huffman_spectral_data`, `individual_channel_stream`, `window_grouping_info`, `quant_to_spec`, `reconstruct_channel_pair`, `reconstruct_single_channel`, `pns_decode`, `ms_decode`, `is_decode`, `ff_imdct_half` | plain, merged | encoding only | video_test's AAC row with the old encodings: 6 of 6 AUs byte-exact. `aac_memcpy` read 503 halfwords ≥0x8000 and stored them back as halfwords; the others 0 (table offsets and counts). Not yet run on silicon (stage 9) |

**Not affected:** the jobs' pixel paths (BLIT, FADE, AVG, SHADE, SCROLL, the drift, jdec's output) read RGB565 by words, vectors or `memcpy`: none had a `0x29`/o1 load. H.264's `int16_t` data always went through `ldsh (rs)`. The hibernate stub has no halfword loads.

## 4. Left out of the batch

- **The staged upload** (`0b5bb46c75`), **SDRAM idle between staged uploads** (`e4dbe9bec9`), **bench row `sdi`** (`4f2b1e17a5`): staging has no reason (the pools take host writes; `COMMIT`'s failure was the sum). Bench row CR2 and the candidate images are removed; `BCMFW_JOBS_STAGED` stays a sim-only option.
- **The hibernate test fix** (`fa3fa5a36b`): both its parts depend on staging (rows that hibernate after a staged upload; the display-only image's decode work area at `0xCC00`, which only the staged layout has).
- **SDRAM idle at every warm-swap hand-over** (`f07b669549`): its code is the staged upload's SDRAM idle (`sd_down`, `jobs_sdram_down`, under `BCMFW_JOBS_STAGED`), so it cannot come alone. The case it would still cover without staging is the plain image leaving SDRAM up at its PARK after a video, then a later merged image's `OPEN` bringing SDRAM up on a running controller. In a `--bcmfw-merged` build the plain image is only the developer probes' image, so the case arises only after a developer probe row, and no failure of it has been seen. Left out; if wanted, it is a small change of its own (idle a running controller at a jobs image's warm start).

## 5. What sitting 2 must check

- Stage 4 and 4b: HWLD's eight records (`bench-p19.md` "HWLD": `H28O` zero-, `H29O` sign-extending confirm #16 and #33; `H48H`, `H48S`, `HSP0`, `HSP1` settle the unproven forms); ASHX, ACSA, GTHR; ALGN's checks (`alignment-audit.md` §5), PLH1 above all (whether a lone halfword write commits).
- Stage 5 with `hello_dma` `28768994` (then the owner's `verified.txt` line).
- The Port write check (CR1): `slots 8 pass`, the 301 × 7 slot at pitch 608.
- The merged image's re-verify (`88e710cb` since `p19-b13`; `80bc8611` before) (stage 6, B13, B5, the jobs self-test, J_VPU 3, the drift B3): `COMMIT` must now pass; the drift and the panel art must show.
- The display-only image `f069a36a` in B5 and B13, J_VPU `0x27`.
- A 176-wide video (padded strides) and a JPEG cover through the display-only image's colour stage.

**What sitting 2 showed** (2026-10-25 evening, `bench-20261025-2257`; details in `bench-p19.md`, "Sitting 2 results") [silicon]: HWLD all eight forms as corrected; ASHX `near32`, ACSA `sat32`, GTHR pass; ALGN 54 of 61 checks (the port part stopped at R602), PLH1: a lone halfword is **dropped**; stage 5 passed (22:33 attempt); the merged image `80bc8611`: jobs self-test, J_VPU 3, the drift on the BCM with `COMMIT` accepted, stage 6 at 150 MHz with nothing skipped, B5 31 of 31 pushes. **Not achieved:** B13 (never parked), so no verified line; the display-only image `f069a36a` read J_VPU **0x23**, not 0x27 (the IDCT check fails; `../../bcm2722-reference/open-questions.md` #72). CR1 and the 176-wide and JPEG checks were not in this re-verify run.

## 6. Tests (2026-10-25, strict alignment `trap`)

Every `bcmfw/tests/*_test.py` (26, `vpu_fit_test` and `halfload_test` among them), `bcmfw/h264/test/sim_test.py` on four clips, `tests/bcmjob`, `tests/autochecks`, `tests/bench_ingest`, `tests/bench_script`, `tests/aligncast`; firmware dev (build A's config), release and sim-ui builds. Results in the branch's last commit message.

## 7. Batch 3 (2026-10-26, branch `p19-batch3`): DECODE's overlay, the level bands, the decoder's hot paths

One regeneration behind one re-verify (sitting 3: `bench-p19.md`, "Sitting 3 run sheet").

| Image | Before (batch 2 + `p19-b13`) | After | Room |
|---|---|---|---|
| merged `bcmvideo-jobs.bin` | 218,564 B `88e710cb` | **214,772 B `2c6dc03a`** | image+bss to `0x33c7c`: **6,020 B** below the arena (`0x35400`, unmoved) |
| DECODE overlay `dec-ovl.bin` (new) | — | 23,884 B `8be7f8d2`, ABI `9bfaa981` (the image's copy at byte 92,250) | 532 B `.bss`; 12,448 B of `0x12B000–0x133FFF` spare |
| plain/probe `bcmvideo.bin` | 220,260 B `7b65c853` | 215,940 B `2ba3a9ea` | — |
| display-only `jobs_display` | 46,912 B `e02c831e` | 46,944 B `f45ecffc` (`jobs_ovl_live`) | — |
| viz overlay `viz-ovl.bin` | 19,380 B `17f8e310` | 19,380 B `4d625306`, ABI `bb2aae47` | its record gains the region's base |
| every other devtest image | | unchanged (`hello_dma` `28768994`) | |

**Items:**

| Item | Kind | Basis |
|---|---|---|
| DECODE as the merged image's overlay (`BCMFW_JOBS_MERGED_OVL` 1; `album-render.md` §5.3): the loader, `dec-ovl.bin` built and recorded by `mkvideo.py`, shipped by `make zip` behind `ovlpair.py --dec` | ours: design | `decode_image_test.py` "merged image: DECODE as an overlay"; `tests/bcmjob` `t_dec_lent` |
| The PP's loader per instance (`bjc_ovl_*(k, …)`, the visualizer's and DECODE's), due only for the running image's region; `bcmjob_decode()` lends the `J_CLS_DRIFT` work slot (`J_DEC_WORK_KEY`), writes `J_DEC_LEN`, clips `len`; the shell parks the drift's cover first | ours: design | `tests/bcmjob` |
| `HP_OP_TONE` refused (`HP_E_BUSY`) while an overlay is loaded: its buffer `0x12C000–0x1349CF` overlaps the region | ours: safety | — |
| The level bands, `HP_OP_VCLK`, the in-band clock, the busy meter on packets (from `p19-video-dvfs` 039cc9a44e, 9a9ef83aed, 86af55bd7e) | driving: [apple-RE] (Apple's frequency-set order, `power-contract.md` §8); the bands and the governor ours | `vclk_test.py`; the arena kept at `0x35400` (the branch had moved it to `0x35580`) |
| The loop filter: bS a nibble at a time from the nzmasks; uniform and mixed edges straight to the vector kernels | ours: C glue around the same kernels (no new form) | `sim_test.py`: 14 corpus clips and the film clip bit-exact |
| CAVLC: the luma blocks' nC from one pointer | ours: C | as above |
| The SPS and PPS parsers `-Os` out of line (`H264_COLD`) | room | −5,376 B |

**Room moves this batch:** (1) AAC's `spec_data` (2 KB, one frame's spectral data, video sessions only) to the far bss (`build.py` `AAC_FAR_MERGED_OVL`), which pays for the overlay loader (640 B); (2) the SPS/PPS parsers `-Os` and out of line, −5,376 B. The bands cost about 300 B, the decoder's new glue about 800 B. The arena did not move.

**The decoder** (vc2sim estimate; the silicon fit of `decoder.md`, FEED = 1.3 ms + cycles / 125.7 per µs):

| Clip | Pictures | Before: mean / p95 / max cycles | After | Fit p95 before → after |
|---|---|---|---|---|
| the film clip re-encoded by the converter's recipe at 30 fps (textured footage) | 130 | 1.980 / 3.271 / 3.724 M | 1.800 / 2.945 / 3.574 M | 27.3 → **24.7 ms** (the rule: 25 ms at 30 fps) |
| the original film clip (`stage6-real.mp4`, 24 fps) | 312 | 2.16 / 3.32 / 4.95 M (`decoder.md`, "P19 vplayer health") | 1.890 / 2.902 / 4.675 M | 27.0 → 24.4 ms (rule at 24 fps: 31.25 ms) |

Both clips decode bit-exact against ffmpeg, as do the 14 corpus clips. The gain is the loop filter's glue (P pictures: 1,524 → 1,096 instructions a macroblock with the kernels' wrappers) and a little of CAVLC (1,575 → 1,502); no vector form was added, so no `J_VPU` check was needed. Silicon checks it in sitting 3's `vfeed` and `s6`.
