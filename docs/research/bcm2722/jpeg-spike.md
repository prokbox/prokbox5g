# JPEG decode on the BCM2722: spike design (P19 W0-8)

**Status:**
- Design, 2026-10-23.
- **S1 and S2 done the same day** (§7): the decoder runs bit-exact in vc2sim, and the decision is to go on to S3–S5.
- **S3's shared decode** (§8): `jdec`, with bad codes failing (#125), decode-once (rev 2), and progressive files (§9).
- **Streamed outputs** (§15, 2026-10-24): no output is held in the work area; the panel's 300×300 fill needs 137,240 B (378,160 before) and fits the BCM's.
- Plan item 4 of `display-offload-plan.md`; the job is `J_OP_DECODE` (reserved, `bcmfw/jobs.h`).

**Tags:** [silicon] measured on the device, [apple], [ours] our design, [estimate].

**Names:**
- These docs use the technical names: PP5022 (its cores CPU and COP), BCM2722 ("the BCM").
- The reference decoder is the shell's `apps/recorder/jpeg_load.c` and `apps/recorder/resize.c`, pinned at **p12 `69f9aa4e9f`**.

## 1. Why

**The cost today:**
- **Decode is the art builder's cost.** A 2.6 MB cover took 45.8 s of decode at 24–30 MHz [measured]: `f3 art … dms=45762`, `handoff-2026-10-19-crash-fat.md`.
- **Each album is decoded 2–3 times,** once per output size (`artcache.md`).
- **A full library pass** took 28–65 minutes on the owner's library [measured; I/O since fixed, decode not].

The CPU fallback is the shell's planned **decode-once** (P19 W0-10): one decode feeding every size, on the PP. That alone cuts the passes to one. The BCM then makes that one pass much faster and takes it off the PP.

**The BCM decode, estimated** [estimate]:
- **Per decode:** the PP's 45.8 s was 2–3 decodes, so about 400 M cycles each at roughly 45 cycles a source pixel for a ~9 Mpx cover.
- **On the BCM's scalar core at 150 MHz:** the same work, about 2.7 s.
- **With a vector IDCT and colour stage:** about 1–2 s, since Huffman decoding (scalar on any core) dominates.
- **Against today:** about 20–40× faster in wall-clock time, with the PP free meanwhile.
- **Typical covers** (500×500 to 1000×1000) scale down in proportion: about 0.1–0.3 s.
- **This corrects** `display-offload-plan.md`'s earlier guess of "0.1–0.5 s per cover", which held only for typical covers.
- **The spike's instruction counts replace all of these numbers.**

## 2. What the art builder needs from one decode

From `apps/ipodui/artcache.c` and `artfmt.c` at p12, all RGB565, ordered-dithered (`FORMAT_DITHER`):

| Output | Size | Fit | Notes |
|---|---|---|---|
| Panel | 300×300 | fill, centre crop | `panels.pak`; today streamed in bands (`pstream`) |
| Now Playing | 142×142 box | **fit** | then composed on the PP into the 137×187 card with its reflection (`np_draw.c`, ~281 k instructions [estimate]): **stays on the PP** in the spike |
| Thumbnail | 55×55 | fill, centre crop; non-square art fitted on a blurred copy (`artblur.c`) | the blur stays on the PP in the spike |
| Cover Flow (P7) | 128×128 | reserved | built when P7 lands |

## 3. The pipeline on the BCM [ours]

**Source → decoded rows → every output in one pass**, streaming MCU rows. No full-size image is ever held.

1. **The file, in.**
   - The PP uploads the file into an **input ring of 32 KB in SRAM** (`J_CLS_SCRATCH`) by engine 2 (~11 MB/s with the CPU 97 % free [silicon S37]), in 8 KB pieces as the BCM consumes it.
   - The BCM publishes how much it has consumed, encoded (`HP_ENC`), as the video FIFO does.
   - No SDRAM, so no SDRAM bring-up or refresh (`memory-contract.md` §3).
   - A 2.6 MB file is about 0.24 s of engine time.
2. **Headers and Huffman.**
   - Scalar C: the markers, the tables, the bit reader and the restart intervals, **ported from `jpeg_load.c`**.
   - Baseline only, as the PP does today. Progressive JPEG came later, in `jdec`, for both cores (§9); it needs a coefficient store this pipeline doesn't have.
3. **The DCT-domain scale.**
   - `calc_scale()` picks the IDCT size (8, 4, 2 or 1) for the **largest** output: 300 for the panel.
   - The decoded image is then at least 300 px on its short side, and the smaller outputs come from it in the same pass.
4. **IDCT and up-sampling.**
   - The same integer IDCTs (`jpeg_idct8v/8h`, `4v/4h`, `2v/2h`, `1h`, and `16v/16h` for chroma) and the same descale and range limit (`scale_output`, `range_limit`).
   - **Vectorised later, bit-exact**, the way the H.264 IDCT was (`decoder.md`).
5. **One MCU row of YCbCr** (8 or 16 lines, at most ~2,000 px wide after the scale: about 96 KB at 16 lines × 3 B). It lives in the SRAM arena's spare and `J_CLS_STRIP` while a decode runs. Decodes run only when the drift and marquee are idle (lit and idle, or on the charger), so those pools are free; the placement doc owns that rule.
6. **The resizer, once per output, streaming:**
   - Each output keeps its own state, ported from `resize.c` (the area filter the art builder uses, at its fixed-point precision), and consumes the same decoded rows.
   - An output row is finished when its source rows have arrived.
7. **Colour and dither:** `resize.c`'s YCbCr→RGB565 conversion and its ordered dither (`FORMAT_DITHER`), ported per output row.
8. **Out:**
   - Finished output rows go to an **output ring of 32 KB** (`J_CLS_STRIP`), each tagged with its output and row number.
   - The PP drains it by reads into its own buffers, the ones the art builder already has (its decode block, the panel band).
   - Reads cost 0.5–1 µs a word when the port is idle (unmeasured), and far more under load [silicon S12, `data-paths.md` R2]. All four outputs are about 270 KB, about 70 ms of reads when idle.
   - Whether port reads auto-increment (#43) would halve that; the spike's bench row measures it.

**Memory while a decode runs** [ours]:

| What | Size |
|---|---|
| Input ring | 32 KB |
| MCU row | ≤ 96 KB |
| Resizer state, four outputs | about 12 KB |
| Output ring | 32 KB |

About 172 KB in all, inside the UI arena's pools.

## 4. The job: `J_OP_DECODE` (0x30, reserved now; defined for rev 0.2)

**Payload:**
- the input ring's slot and the file's length;
- the number of outputs (≤ 4), each with `w | h << 16`, fit or fill, and dither on;
- the output ring's slot.

**Progress words** (job block, encoded):
- `J_DEC_IN_RD`: input consumed, in 8 KB units;
- `J_DEC_OUT_WR`: output rows written;
- `J_DEC_STATE`: running, done, or failed with a code.

**Events:** `J_EV_DECODE` at the end, with its status, the image's size and the instruction count.

**Errors:**

| Code | Meaning |
|---|---|
| `HP_E_UNSUPPORTED` | progressive, arithmetic or 12-bit |
| `HP_E_CORRUPT` | a bad stream; the art builder marks the album `FAILED`, as today |
| `J_E_RANGE` | outputs too large for the rings |

**Deadline:** `J_T_DECODE` = 2 s **per 1 Mpx of source**; a stalled input or output ring stops the clock. On a timeout or a BCM failure, that album takes the CPU path (decode-once on the PP).

**Power:** only lit-and-idle or on the charger (`bcm_power_want(BCM_WANT_DECODE)` refuses on battery while dark; `power-contract.md`).

## 5. Bit-exact plan against the shell's decoder

**The rule:** every output pixel equals the PP's, the same as the drift and the jobs' FADE (`jobs.md` §5). Possible because every stage is integer C with fixed rounding.

**Steps:**
1. **The reference on the Mac:** build `jpeg_load.c` + `resize.c` at `69f9aa4e9f` for the Mac with a small harness (`bcmfw/jpeg/host/`). It calls the same entry the art builder does (`clip_jpeg_fd` with the art builder's flags and sizes) and writes each output's RGB565.
2. **The BCM port, scalar:** the same sources' arithmetic in `bcmfw/jpeg/`. It's a **port, not a rewrite**: the Rockbox code is GPL, as is ours. It's built by vc2cc (conservative; the `min.l`/`max.l` guard applies) and run in vc2sim on the corpus. **Every output must be byte-identical** to the Mac reference.
   - Also built for the Mac, the cross-check that caught the GCC bug (`compiler.md`).
   - This is the spike's exit gate for correctness.
3. **The vector kernels** (IDCT, colour) after the scalar port passes, each bit-exact against it. This is the decoder's own pattern (`decoder.md`).
4. **Tolerance, only if forced:** a ±1 LSB per channel allowance, only for a stage that can't be matched, documented per stage, owner's call. No such stage is expected: the H.264 port matched ffmpeg bit for bit.

**The corpus** (generated, in git as a script plus checksums, never media):
- `tests/jpeg/make_corpus.sh` makes baseline JPEGs with ffmpeg (`-f lavfi testsrc2`, `smptebars`, noise):
  - subsampling 4:2:0, 4:2:2, 4:4:4 and greyscale;
  - qualities 50, 85 and 95;
  - restart intervals on and off;
  - sizes 55, 142, 299, 300, 301, 600, 1000, 1500 and 3000 px, odd and non-square;
  - corrupt variants: truncated, and with a bad Huffman code.
- **The owner's own covers** may be run locally for timing, never committed and never on a simulator disk (the media rule).

## 6. The spike's steps and exit criteria

| Step | What | Exit |
|---|---|---|
| S1 | Mac harness of the shell's decoder plus resizer; the corpus | per-output checksums for every corpus file (the golden table) |
| S2 | Scalar BCM port in vc2sim (after 22:00: heavy) | every output byte-identical to S1; instruction counts per source Mpx and per output |
| S3 | `J_OP_DECODE` in the executor (`jref.c`) and the model (`jmodel.c`) | the art builder's job sequence runs in the simulator and its outputs equal S1 |
| S4 | Vector IDCT and colour kernels | bit-exact against S2; the new instruction counts |
| S5 | A device bench row (the P19 bench list): decode a corpus file and two of the owner's covers on the BCM, read back, compare checksums with S1; time the upload, decode and read-back; port reads with and without auto-increment (#43) | equal checksums; the timing that decides placement |

**Decision after S2:** if the scalar port on vc2sim shows under about 3 s per large cover at 150 MHz [estimate from S2's counts], keep going to S3–S5. Otherwise stop at the PP's decode-once (W0-10) and record why.

## 7. Results: S1 and S2 (2026-10-23)

### S1: the reference

**What it is:** `bcmfw/jpeg/host/jpegref` links `build/sim-ui`'s own objects of `jpeg_load.c`, `resize.c` and `bmp.c`, with POSIX stubs. It calls `clip_jpeg_fd` as `artcache.c` does (`FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE`).

**The corpus** (`tests/jpeg/make_corpus.sh`, generated with ffmpeg and libjpeg-turbo's `cjpeg`): 29 files.
- Sizes 55–3000, with 640×360 and 333×500.
- 4:2:0, 4:2:2, 4:4:4 and grey; qualities 50, 85 and 95; restart intervals.
- One progressive file, one truncated file, one with a bad Huffman code.
- Noise, and a photo-like 3000 px cover with fine grain (1.9 MB).

**The golden table** (`tests/jpeg/golden.txt`, by `golden.py`): 95 outputs at 300×300, 142×142 and 55×55, plus the fitted 142 box for non-square sources.

**What the shell's decoder does** [ours, from the reference]:
- It **refuses progressive files** (status −4).
- It **decodes truncated and corrupt files to a full picture** rather than failing.

The port must do the same, and does.

**The ARM-only code** in `jpeg_load.c` (the range limit, `MULTIPLY16` above ARMv4) keeps the generic C's results on the PP5022 (ARMv4), so the simulator's generic build is the PP's reference. A device check (S5) confirms it.

### S2: the port in vc2sim

**The sources are unchanged.** `jpeg_load.c` is built with `JPEG_FROM_MEM` (the file in memory, as it will be in the BCM's input ring), along with `resize.c` and `bmp.c`.
- They build against `bcmfw/jpeg/env`: the simulator's own macro values, and a small libc shim for vc4-elf-gcc.
- `bcmfw/jpeg/jpegmem.c` is the one entry point.
- About **15.6 KB of code**, built by vc2cc in conservative mode.

**Bit-exact:**
- **On the Mac**, with gcc 16 and Apple clang: 92 of 92 outputs (95 with the grain cover).
- **In vc2sim** (`bcmfw/tests/jpeg_test.py`; `--all` is heavy): every output equals the golden table. That's 86 outputs up to 1000 px, plus 1500 px, 3000 px and the grain cover, with no tolerance needed.

**Cost in vc2sim** (instructions; no memory stalls modelled):

| Source | 300×300 | 142×142 | 55×55 |
|---|---|---|---|
| 600 px test pattern, 4:2:0 q85 | 18.2 M | 10.8 M | 4.7 M |
| 1000 px | 51.5 M | 18.5 M | 7.6 M |
| 3000 px test pattern (0.46 MB) | 98 M | 84 M | 79 M |
| 3000 px photo-like (1.9 MB) | **215 M** | 201 M | 196 M |
| 500 px pure noise, 4:4:4 (worst case) | 105 M | 55 M | 37 M |

- **Entropy decoding dominates big files.** The 55 px output of the grain cover still costs 196 M, so a vector IDCT (S4) gains little there.
- **Decode once** (P19 W0-10's shape: one decode feeding every output) is what saves time: about 215 M for all three outputs, instead of 611 M.

**Time** [estimate, 1 instruction a cycle]:
- **A 2 MB, 9 Mpx photo-like cover:** about **1.4 s at 150 MHz** for the panel output, or about 1.5 s decoded once for all outputs. That's against the PP's measured 45.8 s for a 2.6 MB cover.
- **Typical covers** (≤ 1000 px): 0.1–0.35 s at 150 MHz.
- **At 27 MHz,** everything is 5.6× slower: the decode job wants the 150 MHz clock, on the charger as §4 says.

### The decision

Under §6's rule (about 3 s or less per large cover at 150 MHz), **go on to S3–S5**:
- **S3:** `J_OP_DECODE` in jref and the model, with decode-once outputs.
- **S4:** only if S5's timing asks for it. The Huffman decoder is the hot spot, not the IDCT.
- **S5:** the bench row, which also measures the memory stalls vc2sim doesn't model.

## 8. S3: `J_OP_DECODE` and the shared decode, `jdec` (2026-10-23)

**One definition for both sides** (the orchestrator's rule: DECODE mirrors the CPU path in pixels and in failures):
- `bcmfw/jobs/jdec.{c,h}` is the art builder's decode. It runs the shell's decoder and resizer in their file mode (`clip_jpeg_fd`, exactly as `artcache.c` calls them).
- The decoder's `read()` and `lseek()` are mapped by `bcmfw/jpeg/env` to a **source the caller gives**: the PP's file, the BCM's input ring, or memory (`jdec_mem_src`).
- The CPU path (P19 W0-10's decode-once) and the BCM's job then compile the same file. Pixels and errors can't drift apart.

**The decoder as `jdec` builds it: `JPEG_JDEC`** [ours]
- `bcmfw/jpeg/env` defines it; the shell's build doesn't. The shell's twin of `jdec` will (it compiles `jpeg_load.c` and `resize.c` with `-DJPEG_JDEC` and `-I bcmfw/jpeg`).
- Under it, and only under it, `jpeg_load.c` and `resize.c` gain the hooks in `bcmfw/jpeg/jhook.h`: the bad-code count, the rows tap, the push resizer, and progressive files (`bcmfw/jpeg/jprog.h`, included by `jpeg_load.c`).
- **The shell's build is unchanged:** `jpeg_load.o` and `resize.o` built with `build/fw-ui`'s exact flags (arm-none-eabi-gcc 16, `-Os`, ARM7TDMI) are byte-identical before and after (`objdump -d` and `-s`).

**What it does:**
- **Per output:** resize the source to `dw × dh` (the shell's sizes: fill, fit, the portrait panel, all computed by the caller, never in `jdec`), then keep the window `(cx, cy, cw, ch)` of that. This is what `decode_run` and `ps_decode` do today.
- **Rev 1** decoded once per output. **Rev 2** (below) decodes once per source, behind the same interface.

**Failures** (codes in `jdec.h`, and the job's in `jobs.h`):

| Case | `jdec` | Job | How it's found |
|---|---|---|---|
| Arithmetic coding, 12-bit, lossless, hierarchical; odd subsampling; a progressive file past `JDEC_PROG_MAX` (§9) | `JDEC_E_UNSUPPORTED` | `J_E_DEC_UNSUPPORTED` (26) | the decoder's −4 / −3; the art builder already treats −4 as terminal for that source |
| The stream ends before its EOI | `JDEC_E_TRUNCATED` | `J_E_DEC_TRUNCATED` (28) | `jdec` watches the bytes the decoder reads for an EOI after the **last** SOS. An EXIF thumbnail's own SOS and EOI don't count (corpus `t600_exif_trunc`). If the decoder stops just before the EOI, the rest is read to find it |
| A bad Huffman code (#125) | `JDEC_E_CORRUPT` | `J_E_DEC_CORRUPT` (27) | the decoder counts each code no table has (`huff_errors` in `struct jpeg`; it still decodes it as a zero) and returns `JPEG_E_BADHUFF` (−13) after the decode (corpus `t600_badhuff`) |
| Anything else the decoder rejects; an output not its asked size | `JDEC_E_CORRUPT` | `J_E_DEC_CORRUPT` (27) | the decoder's other negative statuses |
| The work area: short of `jdec_work()`, or of a progressive file's store | `JDEC_E_NOMEM` | `HP_E_NOMEM` | before decoding; `JPEG_E_PROGMEM` (−14) |

After an error the outputs' pixels are unspecified (decode-once writes the smaller outputs while it decodes); the caller drops them.

**The bad-code change in `jpeg_load.c`** (all under `JPEG_JDEC`):
1. `#include "jhook.h"` after `jpeg_load.h`.
2. `struct jpeg`: `int huff_errors`.
3. `HUFF_BAD(p)`: `huff_errors++` under the guard, `((void)0)` without it; called first in the "error in Huffman" branch of `huff_decode_dc` and of `huff_decode_ac`.
4. `DECODE_END(p, rows, bm_size)` on both of `clip_jpeg_fd`'s `return bm_size`: `(bm_size)` without the guard; with it, `decode_end()`, which returns `JPEG_E_BADHUFF` if the count is above 0.

### Decode-once (rev 2) [ours]

**The definition** (`jdec.h`; the reference for both sides):
1. **The primary output** is the largest by area, `dw × dh`; the first of equals (`jdec_primary`).
2. **It is decoded exactly as a single decode at its size:** `clip_jpeg_fd(FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE)` at `dw × dh`. So its pixels are rev 1's, bit for bit. That call picks the IDCT scale (1/8, 1/4, 1/2 or 1) for its size and makes the picture's **rows** at that scale: the source's size times the scale, rounded down.
3. **Every other output is the rows resized to its `dw × dh`** by `resize.c`, with `resize_on_load`'s own arithmetic: area average down, linear up, per axis; dithered; YUV to RGB after the resize, as for the primary.
   - At the rows' own size, it takes `clip_jpeg_fd`'s unscaled path instead (YUV to RGB, then `output_row_8_native`), as a single decode at that size would.
4. **Each output's window** is then kept, as before.

**What follows from it:** an output whose own IDCT scale equals the primary's gets the same rows a single decode at its size would. So it equals that single decode, bit for bit. The test checks this on every such output. In the corpus, 38 of the 101 outputs are primaries or share the primary's scale, and all 38 equal `golden.txt` (35 pictures, and 3 primaries that fail: the truncated and bad-code files).

**How:** one pass of the decoder, with the rows fanned out (`bcmfw/jpeg/jhook.h`).
- **The rows tap** (`jpeg_set_rows_tap`): `store_row_jpeg` hands each row to `jdec` before the primary's resizer takes it. `begin` gives the rows' size and kind first.
  - At the end, `decode_end` makes any rows the primary didn't pull: an upscaling primary may stop short.
- **`resize_push`** (in `resize.c`): `scale_v_area` and `scale_v_linear` with their loops turned inside out, so they take one source row at a time. It's the same arithmetic in the same order, and the horizontal scalers and output rows are `resize_on_load`'s own.
  - Each output row goes to `jdec`, which keeps its window. No full-size bitmap is held for the smaller outputs.
- **Memory:** `JDEC_WORK(primary)` plus `JDEC_WORK_OUT(dw)` = `50 × dw + 512` bytes for each other output. `jdec_work(out, n)` is the sum. For the art set: about 380 KB for the panel (its 180 KB bitmap, the decoder's 64 KB read buffer and state, its rows), plus 7.6 KB and 3.3 KB.

**Why `tests/jpeg/golden_once.txt` is trustworthy:**
- It is made by the Mac build of `jdec` itself (`jpeg_test.py --make-once`), and only written when three checks agree on every line:
  - **The primary equals `golden.txt`:** the S1 reference, the unchanged decoder.
  - **The plain way agrees** (`bcmfw/jpeg/jdoracle.c`): the primary alone, with every row kept whole in memory, then each other output by the shell's **unchanged, pulling `resize_on_load`** over those rows. `jdec_run`'s streamed resizers give the same pixels on all 101 outputs, so `resize_push` is `resize_on_load`.
  - **`J_OP_DECODE`** through the executor gives the same pixels.
- And every output whose own IDCT scale is the primary's equals `golden.txt` (above).
- The rest are derived deterministically from those rows, by integer code, on both sides.

**Checked:**
- **The Mac:** jdec, J_OP_DECODE and the oracle. 101 single outputs (rev 1's expectations) and 101 decoded once per file all pass.
- **vc2sim:** `jdec` built by vc2cc, every output equal to the Mac's, single and decoded once.
- **Code** (vc2cc): 17.8 KB with decode-once, and 21.8 KB with progressive (from 15.4 KB).

**Instruction counts in vc2sim** (the art set: 300, 142 and 55; once is one `jdec_run`, singles are three):

| Source | Singles | Once | Saved |
|---|---|---|---|
| 3000 px photo-like (`grain3000`, 1.9 MB) | 742 M | **273 M** | 63 % |
| 3000 px test pattern (`t3000`) | 300 M | **126 M** | 58 % |
| 1500 px | 118 M | 76 M | 36 % |
| 600 px, 4:2:0 q85 | 39.4 M | 31.6 M | 20 % |
| 142 px | 24.1 M | 20.3 M | 16 % |
| 333×500 (with the 95×142 fit, four outputs) | 64 M | 66 M | −2 % |

- **Large covers are where the time goes, and they gain the most.** The smaller outputs ride on the primary's entropy decoding instead of repeating it: about 1.8 s at 150 MHz for a 2 MB photo-like cover, down from 4.9 s [estimate, one instruction a cycle].
- **Small sources gain little or lose a few percent.** A 333 px source decodes at full scale for the 300 panel. The 55 and 142 outputs are then area-averaged from 333×500 rows instead of decoded at 1/4 or 1/2. That costs about as much as the decodes it saves.
  - A possible later refinement: a second IDCT scale for small outputs. It isn't done, because it would change the definition, and the cost is small in absolute terms.

**Still to do:**
- **S3b, the BCM image:** done: `J_OP_DECODE` in the display-only image over an input ring (`album-render.md`; `decode_image_test.py`).
- **The shell's twin of `jdec`** (W0-10): compile `jdec.c`, `jpeg_load.c` and `resize.c` with `JPEG_JDEC`, and size its block with `jdec_work()`.

## 9. Progressive JPEG in `jdec` (2026-10-23) [ours]

**Done, on both sides together:** `jdec` decodes progressive (SOF2, Huffman) files. A progressive file gives **the pixels of the baseline file with the same coefficients**, at every size, single or decoded once.

**The memory problem:**
- A progressive file's scans each touch every block, so the whole picture's coefficients must be held until the last scan. That's 128 bytes a block: about 27 MB for a 3000 px 4:2:0 cover.
- The BCM has 4 MB of SDRAM (and §3's pipeline uses none). The PP's decode block is about 100 KB, and its resident cap is 1.5 MB.

**The idea:** keep only what the scaled IDCT reads.
- For each component, the baseline path keeps the zig-zag coefficients `0 .. K−1`, where `K = max(k_need, 1)` for that component's IDCT scale. `k_need` is `jpeg_load.c`'s, for the largest output.
  - Luma: 1 at 1/8, 4 at 1/4, 24 at 1/2, 63 at full scale.
  - 4:2:0 chroma is one scale up.
- **The store keeps exactly those, as 16-bit integers.** It's the same set, so the IDCT gets the same input and the pixels follow.
- **The coefficients past K** are never output. But a refinement scan's correction bits go to the coefficients that are already nonzero. So decoding one needs to know, for every coefficient in its band, whether it is nonzero.
  - Those get one bit each: `ceil((64 − K) / 8)` bytes a block.
- **A component with K = 1** (DC only) needs no bits at all. Every one of its AC scans is skipped whole, by scanning for the next marker, without being decoded.
  - That's the common large-cover case: luma at 1/8 for a ≥ 2400 px source.

**Store size**, per block: 2 bytes if K = 1, else `2K + ceil((64 − K) / 8)`. Below, for the panel's 300 px primary:

| Source | Scale (luma) | K luma / chroma | 4:2:0 store | 4:4:4 store | Full coefficients (4:2:0) |
|---|---|---|---|---|---|
| 600 px | 1/2 | 24 / 63 | 657 KB | 873 KB | 1.1 MB |
| 1000 px | 1/2 | 24 / 63 | 1.8 MB | 2.4 MB | 2.9 MB |
| 1199 px | 1/2 | 24 / 63 | 2.5 MB | 3.4 MB | 4.1 MB |
| 1200 px | 1/4 | 4 / 24 | 0.9 MB | 1.0 MB | 4.1 MB |
| 1500 px | 1/4 | 4 / 24 | 1.4 MB | 1.6 MB | 6.5 MB |
| 2000 px | 1/4 | 4 / 24 | 2.5 MB | 2.9 MB | 11.4 MB |
| 2399 px | 1/4 | 4 / 24 | 3.6 MB | 4.1 MB | 16.5 MB |
| 2400 px | 1/8 | 1 / 4 | 0.9 MB | 0.5 MB | 16.5 MB |
| 3000 px | 1/8 | 1 / 4 | 1.3 MB | 0.8 MB | 25.9 MB |
| 4000 px | 1/8 | 1 / 4 | 2.4 MB | 1.4 MB | 45.8 MB |
| 6000 px | 1/8 | 1 / 4 | 5.4 MB | 3.2 MB | 103 MB |

**The cap: `JDEC_PROG_MAX` = 2 MB** (`jdec.h`; the decoder's `JPEG_PROG_MAX` in `jhook.h`, asserted equal), shared.
- Past it, a file fails `UNSUPPORTED` (−4) on both sides, whatever their memory. So both refuse the same files.
- Under it, a file whose store doesn't fit the caller's work area fails `NOMEM` (`JPEG_E_PROGMEM`). A caller that adds `JDEC_PROG_MAX` to `jdec_work()` never sees that.
- The cap is a placement decision, not a property of the decoder (see the open questions).

**How** (`bcmfw/jpeg/jprog.h`, included by `jpeg_load.c` under `JPEG_JDEC`; T.81 G.1.2, with the refinement following libjpeg's):
- **Markers.** `process_markers` takes SOF2 like SOF0, and stops at each SOS for `prog_scan`. Between scans it is called again: DHT is re-derived, DRI is allowed, a DQT is refused (−4), and the EOI ends the scans.
- **The bit reader.** In a progressive file, `fill_bit_buffer` goes to `prog_fill`. RSTn handling is the same, but any other marker, or the end of the data, stops it, and zeros follow. The marker it stopped at is handed back to `process_markers`.
- **Scans.**
  - DC first (interleaved over MCUs, or per component in its own block raster) and DC refine.
  - AC first with EOBRUN, and AC refine: the correction bits, and the new ±1 coefficients.
  - Restart intervals: none after a scan's last unit.
  - Bad codes, and runs past the band, count as bad codes (CORRUPT).
- **The store** is at the end of the decoder's buffer, zeroed: per component, a `bw × bh` MCU-padded block grid, each component's part 8-aligned (vc2sim caught an unaligned 16-bit access the Mac allowed).
- **The rows.** After the last scan, `store_row_jpeg` builds each block from the store (`prog_block`: DC and AC times the quantiser, in the baseline path's own order and positions). Then it runs the unchanged IDCT, upsampling, rows tap and resizers.
- **A truncated progressive file** decodes the scans that came, then fails `TRUNCATED`, like a baseline one.

**Checked** (`tests/jpeg/make_prog.sh`, into `build/jpeg-prog`; `jpeg_test.py` runs it):
- **Pairs:** each progressive file with its baseline twin, both from the same source picture and settings by cjpeg.
  - Cases: 4:2:0, 4:4:4, grey, 4:2:2 at 333×500, restart intervals, 142 px (full scale), 1000 px (1/2), 1500 px (1/4), 3000 px (1/8, luma's AC scans skipped).
  - Two scan scripts of our own: DC with successive approximation and uneven AC bands, and DC per component with a refinement band that straddles the kept coefficients, at 600, 1000 and 1500 px.
- **The Mac:** every progressive file equals its twin at 300, 142 and 55 and as the art set decoded once, through `jdec` and J_OP_DECODE. The corpus's own `t600_prog` equals `t600_420_q85`.
- **vc2sim:** the art set decoded once, equal to the Mac.
- **The refused cases:** a 4000 px 4:2:0 file fails UNSUPPORTED (2.4 MB > the cap), and a truncated one fails TRUNCATED.

**Cost in vc2sim** (the art set decoded once; the baseline twin's in the same build for comparison):

| File | Progressive | Baseline twin |
|---|---|---|
| 600 px 4:2:0 | 48.4 M | 31.7 M |
| 600 px 4:4:4 | 63.5 M | 36.9 M |
| 600 px grey | 34.1 M | 22.1 M |
| 1000 px 4:2:0 | 119 M | 73.8 M |
| 1500 px 4:2:0 | 172 M | 76.3 M |
| 1500 px, our script B (DC per component) | 104 M | 76.3 M |
| 3000 px 4:2:0 (luma's AC scans skipped) | 206 M | 126 M |

- **Progressive costs 1.3–2.3× its twin.** Every coefficient's bits are still decoded, except the AC scans of a DC-only component, and the refinement scans re-read the history. The scans are also smaller files' worth of Huffman work spread over more passes.
- **The 3000 px cover:** about 1.4 s at 150 MHz [estimate], under §6's 3 s rule.
- **The refused 4000 px file** costs 2.3 M (the headers and the size check).

**Open questions:**
- **Where progressive runs.** On the BCM, the store needs SDRAM (§3's pipeline is SRAM-only), so a progressive job would bring SDRAM up. On the PP, a store up to 2 MB is above the resident cap. It would need a transient block, borrowed as Extras class B borrows RAM, or a lower cap. Either way the cap must be the same number on both sides.
- `jobs.h` still lists "progressive" under `J_E_DEC_UNSUPPORTED`'s comment. It now means "past `JDEC_PROG_MAX`" (not edited here: not this session's file).

## 10. The fast decode (2026-10-24, branch `p19-jdec-fast`) [ours]

`algorithms.md` items 2, 4, 6 and 9 ("Adopted" there has the method, the exactness argument and the per-function numbers). Exact: `golden.txt`, `golden_once.txt` and the progressive pairs are unchanged on the Mac and in vc2sim (`jpeg_test.py --all`), and the shell's `jpeg_load.o`, `resize.o` and `bmp.o` are byte-identical.

- **The entropy decoder** (`JPEG_JDEC`, baseline scans): 9-bit tables giving the symbol and, for short codes, the coefficient; the bit buffer in registers; the input buffer read in place; the old refill rule, so markers, stuffing and the end of the data behave as before. Progressive scans keep the old decoder.
- **The colour stage** on the BCM's vector unit (`jv_yuv16`, through the decoder's `jdec_yuv565` hook; the Mac and the PP keep the C).
- **`memcpy`/`memset`** word-wise in the image and in the test (they were byte-wise in both).

**Instruction counts in vc2sim** (the art set decoded once; the branch point's figures are §8's):

| Source | Branch point | Now | Saved |
|---|---|---|---|
| 3000 px photo-like (`grain3000`, 1.9 MB) | 273.1 M | **157.0 M** | 43 % |
| 3000 px test pattern (`t3000`) | 125.8 M | **78.2 M** | 38 % |
| 600 px, 4:2:0 q85 | 31.8 M | **19.1 M** | 40 % |

- **Time** [estimate, one instruction a cycle]: a 2 MB photo-like cover's whole art set about 1.05 s at 150 MHz (was 1.8 s), 5.8 s at 27 MHz.
- **Progressive** is unchanged in method; its art set runs a little faster from the word-wise `memset` (the 3000 px pair: 206 M in §9's table, 191 M now).
- **The display-only image:** image+bss 0xb5a0 → 0xb5c0, 1,088 B below the decode work area; jdec's own build leaves out the file loaders it never calls (`JDEC_ALONE`, 2.6 KB) to make the room.
- **Next exact targets:** `jdec_io_read`'s per-byte EOI scan (17.6 M of the grain cover's 157 M), then the AC skip loop's per-symbol cost (the multi-symbol table, if the work area can spare 16 KB).

## 11. The reader's EOI scan (2026-10-24, branch `p19-jdec-io`) [ours]

`jdec_io_read` fed every byte it read through the truncation rule's state machine (#125: whole if an EOI follows the last SOS): about 9 vc2sim instructions a byte, the largest cost §10 left. The rule now lives in `bcmfw/jobs/jscan.h`, with its byte-wise definition `jscan_ref()` beside the fast form `jscan_bytes()`.

- **The method:** only the byte after an 0xFF can change the state, so the runs between 0xFFs are skipped. Four aligned words a turn, each tested with the "has a zero byte" trick on `~w` (Hacker's Delight 6-1, as in glibc's and musl's `memchr`): `(0xFEFEFEFE − w) & w & 0x80808080` is nonzero exactly when a byte of `w` is 0xFF, with both constants held in registers so the test is a three-operand `sub`, an `and` and the group's `or`s: 19 instructions for 16 bytes. Each 0xFF and the byte after it take the old byte-wise step.
- **Exact by construction:** the same bytes are fed, in the same chunks, and the state after every chunk is the old one. Truncation is still decided by the bytes the decoder reads, not by a shortcut. Checking only the stream's last bytes, or asking the entropy decoder which marker it stopped on, would give different answers on some files: an SOS in junk after the EOI inside the last read, a stray FF D9 in corrupt entropy data, or a decoder that stops early. So neither was adopted.
- **Proof:**
  - `golden.txt` and `golden_once.txt` are unchanged (`jpeg_test.py --all`, the Mac and vc2sim).
  - The new `bcmfw/tests/jscan_test.py` compares the fast scan with `jscan_ref()` over a 190 KB mixed buffer. The buffer holds the bytes near the rule (FF, DA, D9, 00, FE, 7F, 80, 01), runs with no FF, and the EXIF file. It is fed in random chunks at every alignment, with skips, from a random start state: 3,000 seeds on the Mac and 6 in vc2sim, with no state different.
  - It also decodes seven edge files made from the corpus, through jdec on the Mac and in vc2sim. They cover an EOI whose FF ends one of the decoder's 64 KiB reads and whose D9 starts the next, that file cut after the FF, the EOI one byte either side, and data after the EOI (plain; with an SOS inside the last read; with an SOS past it). Each must give the status the old scan gave.
  - Six mutants of `jscan.h` must each fail one or both checks, and all six do: a word lane never tested, the byte after an FF skipped, the pending FF dropped at a read boundary (only the split edge catches it end to end), the last 16 bytes read as a group, stuffed FF 00 kept pending, and an SOS that keeps an earlier EOI.
- **The rule's edges, as the old code had them and the new keeps them:** an SOS in junk after the EOI makes a file truncated if that junk is inside a read the decoder makes. `ended()` stops at the first EOI, so junk past the decoder's last read is never seen (`t600_trail_sos` against `t600_trail_sos_far`). A skip (the decoder's `lseek` past its buffer) clears the pending FF.
- **The shell:** no upstream file changed. `jpeg_load.o`, `resize.o` and `bmp.o` from a `--ipodui` firmware build are byte-identical before and after, and so are their `objdump -d` and `-s` (rebuilt without ccache).

**Instruction counts in vc2sim** (the art set decoded once; *before* is §10's *now*):

| Source | `jdec_io_read` before | after | Whole decode before | after | Saved |
|---|---|---|---|---|---|
| `grain3000_420_q85` (1.9 MB) | 17.63 M | **3.40 M** (5.2×) | 157.0 M | **142.7 M** | 9.1 % |
| `t3000_420_q85` (0.46 MB) | 4.77 M | **0.93 M** (5.2×) | 78.2 M | **74.4 M** | 4.9 % |
| `t600_420_q85` (31 KB) | 0.57 M | **0.16 M** (3.7×) | 19.07 M | **18.66 M** | 2.2 % |

- **Per byte:** 9.0 → 1.7 instructions on the grain cover. The 16-byte loop is 1.2 a byte; the rest is about 60 instructions for each 0xFF (one in 120 bytes of these files' entropy data, mostly the stuffed FF 00) and its realignment.
- **Time** [estimate, one instruction a cycle]: the 1.9 MB cover's art set 1.05 → 0.95 s at 150 MHz.
- **The display-only image:** image+bss 0xb660 → **0xb700** (+160 B; 768 B below the decode work area at 0xBA00). The merged image is unchanged at 0x3518c (below 0x35400).
- **Not adopted:**
  - **A vector form**, testing 16 lanes of `vld8` against 255 and reducing through the scalar-result unit. It would need forms not yet checked on the device in this combination, and reads that cross 16-byte lines, which quirks.md treats as unverified. It would save at most about 2 M of the grain cover's 143 M (1.4 %).
  - **Finding the FF in a word from the mask** (count trailing zeros) instead of a byte walk: about 0.3 M on the grain cover.
- **Next:** the copy in the source's `fetch` (`memcpy` from the ring into the decoder's buffer, about 1.1 M of the grain cover's `memcpy` 1.65 M). Scanning while copying, or letting the decoder read the ring in place, would remove most of what is left of the reader.


## 12. The AC skip loop (2026-10-24, branch `p19-jdec-ac`) [ours]

After §11, the grain cover's art set cost 142.7 M instructions, and 95 M of it was `store_row_jpeg`: mostly the AC coefficients a scaled IDCT doesn't keep (from `k_need` on). They are still decoded one symbol at a time to stay in sync. At 1/8 scale (luma of a ≥ 2400 px cover), every AC symbol is one of these: 2.74 M symbols on that cover, about 19 instructions each, plus the fills.

**What changed** (`jpeg_load.c` under `JPEG_JDEC`; the shell's objects are byte-identical):
- **A skip-only table**, `ac_skip` (stb_image's `fast_ac` idea with the value left out). A discarded coefficient needs only its length, never its value. So `fix_huff_fast()`, as it builds each AC table's 9-bit `ac_fast`, also fills 512 `uint16_t` entries of `adv << 8 | t`:
  - `t` is the bits the symbol takes, code and magnitude together (1–24; 255 when no code of up to 9 bits starts there);
  - `adv` is how far `k` moves: run + 1, 16 for ZRL, and 64 for an end of block (and for any other zero size, as `huff_decode_ac` takes it).
  - No extend or sign step. 2 KB for the two tables, in `struct jpeg` (the work area).
- **The bit count as `u = 32 − bits`** in the loop. The next 9 bits are `(buf << u) >> 23`, two shifts. A symbol's bits are all in the buffer when `u + t ≤ 32`; a fill is due when `u > 24`. A symbol is then five instructions: shift, shift, `ldh`, `bmask` and one `addcmpb` that adds and tests `u`. Then `asr`, one `addcmpb` that adds and tests `k`, and the fill test: 8, plus a `mov` and a branch back when no fill follows (vc2sim, grain cover; below).
- **DC-only components** (`k_need` ≤ 1) go straight to the skip loop, instead of taking their first AC symbol in the keeping loop.
- **The reader.** A limit pointer stands in for `buf_left` and `marker_val` (`lim` is the last place with two bytes after it, or 0 while a restart marker is pending). Inside the skip loop, which makes no calls, the cursor and `lim` live in registers. A fill from the buffer is then one compare, two loads and two FF tests, 12 instructions instead of 17.
- **Everything else goes to `hb_skip_one()`** (noinline): a symbol whose bits aren't all in the buffer, a code longer than 9 bits, and a fill that `fill_bit_buffer` must make (near a 0xFF, a pending marker or the buffer's end). That function is the old skip code without its short-code branch, which takes the same bits as the code-only branch. Keeping this out of line keeps the loop's exits close enough for the fused `addcmpb` branches.

**Exact by construction:**
- The skip takes a symbol only when its bits are all in the buffer, and then the bit-serial decoder would add no bits before taking them either. The fill test is `check_bit_buffer`'s (`bits < 8`, as `u > 24`). So the same fills happen at the same symbols, and the stream position, `marker_ind`, restarts, stuffing and the end of the data are unchanged.
- A symbol whose bits aren't all there, a code longer than 9 bits, and a fill only `fill_bit_buffer` can make go the general way (`hb_skip_one`, the old skip code).
- So does a buffer that claims more than 32 bits. Only a corrupt file's restart sets that (below).
- Shift counts stay in 0–24, clear of `isa-scalar.md`'s unexercised counts of 32 and above.

**Proof:**
- `golden.txt` and `golden_once.txt` are unchanged: `jpeg_test.py --all`, 244 outputs, on the Mac and in vc2sim.
- **`bcmfw/tests/jskip_test.py`** (new) decodes 287 files at 55 (1/8), 142 (1/4) and the art set once, and compares the fast decoder with **the decoder before this change** (`jpeg_load.c` at `10caf83824`, built from git), on the Mac and (11 files) in vc2sim. Every status and every pixel must be the same. The files:
  - the corpus sources, corrupted: bit flips in the entropy data, runs of noise, a stuffed zero dropped, restart markers put a few bytes before real ones, and cuts;
  - three files made with optimised Huffman tables (`cjpeg -optimize`, as `make_corpus.sh` makes the corpus). The standard tables' ZRL is 10–11 bits, so it always goes the general way; in two of these it is 6, 8 and 9 bits, so ZRL takes the skip table. The third restarts at every MCU.
- **The shell's own bit-serial loop** (`JDEC_BITSERIAL`) is compared too. It agrees on every file except 15 with restart markers put early. There, a restart sets the bit count to `marker_ind`, which can reach thousands. Both of the shell's loops then shift by more than the word, which is undefined in C, and they differed in the same way before this change: an **upstream bug** (`search_restart` never bounds `bitbuf_bits`). The new loop leaves that state to the old code.
- **Eight mutants** must each fail the golden table or the corrupt-file comparison, and all eight do: a skip one bit long, one bit short, ZRL one coefficient short (only the optimised tables reach it), a run one long (in the table and in `hb_skip_one`), kept coefficients skipped (`k_need <= 4`), a fill reading past the buffer, and a pending restart marker ignored.
- **Three edits jdec's results can't show** are not mutants:
  - Taking a symbol whose last magnitude bit is still to come: the fill it needs comes one symbol later, and fills commute with taking bits.
  - Widening the guard past 32: every file that reaches that state fails CORRUPT whatever the skip does.
  - A fill a few bits early: the skip loop fills from the buffer only away from 0xFF bytes and markers, so an early fill reads the bytes a later one would, and a restart's marker search starts past data either way.
- `decode_image_test`, `jobs_test`, `jobs_image_test`, `forms_test` (every new form an allowed kind), `hostproto_test`, `jscan_test`, `mkdevtest --check`, `mkvideo --check` and `gen_jdec.py --check` all pass.

**Instruction counts in vc2sim** (the art set decoded once; *before* is §11's *after*, re-measured at the branch point):

| Source | `store_row_jpeg` before | after | Whole decode before | after | Saved |
|---|---|---|---|---|---|
| `grain3000_420_q85` (1.9 MB) | 95.26 M | **64.96 M** | 142.73 M | **115.50 M** | 19.1 % |
| `t3000_420_q85` (0.46 MB) | 35.19 M | **29.50 M** | 74.36 M | **69.16 M** | 7.0 % |
| `t600_420_q85` (31 KB) | 3.085 M | **2.951 M** | 18.66 M | **18.55 M** | 0.6 % |

- **The grain cover:** a skipped symbol takes 8 instructions plus the loop's back edge (a `mov` and a branch the compiler adds, on the 71 % of symbols with no fill), about 9.4 on average instead of about 19, and a fill 12 instead of 17. Time [estimate, one instruction a cycle]: its art set 0.95 → **0.77 s** at 150 MHz.
- **The 600 px cover** decodes at 1/2 scale and skips almost nothing; it gains a little from the cheaper fills.
- A first form kept the cursor in registers across the whole block (114.7 M on the grain cover), but every call site then marshalled it, and the image no longer fitted once merged with `p19-bcm-prep`. The form above gives up 0.8 M on the grain cover for 288 B of image.
- **Work area:** `struct jpeg` +2,048 B. `JDEC_WORK` is unchanged: on the Mac, every corpus file's art set and single outputs still decode in exactly `jdec_work()` bytes, with at least 68 KB of the fixed 192 KB unused (70 KB before). `J_DEC_WORK_MIN` and the pools are unchanged.
- **The display-only image** (after the merge with `p19-bcm-prep` at `9e6b2399dd`, whose own changes take it to 0xb900): image+bss 0xb900 → **0xb9a0** (+160 B; 96 B below the decode work area at 0xBA00, which didn't move). The merged image has no decoder (0x352ac, below 0x35400).
- **The PP** (arm-none-eabi-gcc 16, `-Os`, ARM7TDMI, `--ipodui` flags plus `-DJPEG_JDEC -I bcmfw/jpeg`): `jpeg_load.o` text 18,839 → **19,055 B** (+216). It can't build as Thumb at all, before or after: the range limit's inline ARM asm (`mvnhi`, line 227) is ARM-only. In ARM the barrel shifter folds `k += len >> 8` into one `add`, and a symbol is about 11 instructions.

**Not adopted:**
- **Multi-symbol tables** (two symbols a look; Choueka–Klein–Perl, Huff0 X2). Measured on the grain cover's 2.74 M skipped symbols, which average 4.5 bits:
  - Exact pairing needs three runtime checks: both symbols' bits present, no fill the old decoder would make between them, and `k` still below 64 after the first.
  - With 9-bit windows, 53 % of the symbols pair (61 % if the fill between them is replayed after). A pair-capable entry costs about 11 instructions a look against 9 for a single, so the estimated loop cost is 24.2 M against 21.9 M now: a loss.
  - With 12-bit windows (2 × 16 KB of 32-bit entries), 71–80 % pair, for about 19.7 M: 2.2 M saved, under 2 % of the decode, for 32 KB of work area. Not worth it.
- **libjpeg-turbo's `decode_mcu_fast`** relies on a 57-bit buffer and fills ahead of need. On a 32-bit core the buffer gives at most one 16-bit fill per 8 bits used. Its fill-ahead is the read-ahead §10 already ruled out, because it changes when bytes are read.
- **EOB-run shortcuts:** baseline scans have no EOB runs. Progressive scans' runs are already skipped whole for DC-only components (§9).

**Where the grain cover's 116 M goes now:**
- `store_row_jpeg` 65 M: the skip symbols about 27 M, fills about 9 M, and the per-block glue about 18 M (spilled block state; DC; the IDCT calls).
- `scale_h_area` 19.2 M, `resize_on_load` 4.7 M, `hb_slow` 3.6 M, `jdec_io_read` 3.5 M.

**Next exact targets:** the per-block glue, where register pressure spills the block's state (about 80 instructions a block). Then `scale_h_area` (§3 of `algorithms.md`: the alpha cut).

## 13. The 8x8 IDCT on the vector unit (2026-10-24, branch `p19-vpu-idct`) [ours]

After §12 the IDCT was the largest stage: 48 % of a 600 px cover decoded to 300, 74 % decoded to 600 (`algorithms.md` §1). The 8x8 one (`jpeg_idct8v` then `jpeg_idct8h`: the luma at full size, a 4:2:0 file's chroma at half size, every component of a 4:4:4 or grey file at full size) now runs on the BCM's vector unit, **bit for bit the C's for any int16 coefficients**.

**The kernel** (`bcmfw/video/gen_jdec.py` → `jdec_kernels.S` `jv_idct8x4`; the method is `algorithms.md` §1):
- jpeg_load.c's 1-D islow is linear, so each output is Σ M[n][j]·x_j plus its rounding term, with every M[n][j] a 16-bit constant. Each product is split exactly by the 32-bit accumulator (`isa.md` "The 32-bit accumulator" [apple-RE, sim]): `vmul … x15` adds its low 16 bits at bit 0, `vmulhd … acc` its floored high half at bit 16. One REP 8 pair makes all eight products of an output (A, the data, steps a row; B, M's row as a V16 column, steps a column), so the sum is exact mod 2^32, as the C's.
- Four blocks a call, as two groups of 16 lanes. Pass 1 reads the int16 workspace value from the accumulator's halves, `(hi << 5) | (lo >> 11)`, and writes it as a V16 column, so the transpose is free. Reading both halves instead of prescaling (the prototype's way, `algobench/vpu_islow.py`) removes its limit of |coefficient| ≤ 2,047: no block ever takes the C path, and the decoder needs no range check.
- Pass 2 seeds 514 << 16 (the C's rounding and level shift), then `vasr #2` and `vclip #255` into an H8 row: the C's `range_limit(sum >> 18)` for any 32-bit sum.
- The constants are 128 bytes (two rows of M a load at stride 0, an odd row's lanes moved down). The coefficients are zeroed after the load and the pixels stored by `jobs_kernels.S`'s `jv_store16` (single block-0 `vst16`s, the device-checked store).
- About 600 instructions a call, vector ops counted as one: 155 a block (measured at 600, 0.87 M for 5,625 blocks, the row ends' part-filled calls included). The C is 656 for a DC-only block and 1,623 for one with AC terms.

**The decoder** (`apps/recorder/jpeg_load.c`, `JPEG_JDEC` with `JDEC_ALONE`: jdec's own builds only; the hook `jdec_idct8`, `bcmfw/jpeg/jhook.h`, NULL on the Mac and absent on the PP's twin):
- In `mcu_row_fast`, an 8x8 block's coefficients go straight to the kernel's places (`zag16`: rows 16 halfwords apart; the places start at 0 and the kernel leaves them 0, so the C's per-block clear goes too). Its IDCT is `idct8_take`: four taken run the kernel (`idct8_run`), and the pixels are put where `jpeg_idct8h` would have written them. The 8x8 blocks of an MCU form a ring, so finding the next block's place is one load.
- The row's last blocks run at its end, and a grey file's (one block an MCU) before the MCU's luma is copied to its chroma bytes.
- The batching state (1,120 B) lives in `struct jpeg`, so in the work area. 1,120 is a multiple of 32, which keeps every later buffer's alignment (the colour stage stores rows in place when they are 32-byte aligned).

**The gate** (`bcmfw/video/jdec_vpu.c`): `jdec_vpu_init` runs a start-up check and installs the kernel only if it agrees with the C (`jdec_idct8_c`: `jpeg_idct8v` then `jpeg_idct8h`) on every pixel of four blocks: a hash of the index (any int16), the extremes −32,768 and 32,767, and two rounding probes. Each probe is one column whose pass-1 output 0 is exactly 16 << 11 and 16 << 11 − 1 before the shift, rounding term included, so a rounding term one off either way moves a whole row of pixels (random blocks almost never show one: a ±1 in the workspace rarely crosses a pixel). It also checks that the coefficients came back 0. `jobs.h` `J_VPU_MAC` (0x4) records a pass for the PP (images that don't decode leave it clear). `config.h` `BCMFW_JDEC_VPU_IDCT=0` keeps the C IDCT; `BCMFW_JOBS_MAC_FAIL=1` (tests only) makes the check fail. On silicon, a form that misbehaves fails the check, and jdec keeps the C: exact either way.

**Room in the display-only image** (the hard part: it had 96 B below the decode work area at 0xBA00). Cut exactly elsewhere (option (a); the shell's objects byte-identical):
- jdec's default Huffman tables (for MJPEG without a DHT) stored as bytes and widened on copy: −1,236 B of rodata.
- The shell's art-builder pause and no-boost hooks left out of `JDEC_ALONE` builds (nothing there sets them): about −230 B.
- With the kernel (276 B of code, 128 B of constants and 16 of alignment), the decoder's side (about 620 B) and the start-up check (about 440 B), image+bss 0xb9a0 → **0xb9e0**: 32 B below the work area, which didn't move. `J_DEC_WORK_MIN`, the pools and the memory contract are unchanged. The merged image (no decoder) is byte-identical.
- The work area: `JDEC_WORK` is unchanged. Every baseline corpus decode, art set and single outputs, still needs at most `jdec_work()` bytes; measured on the Mac with `jdec_run`'s own size check removed, the least spare in the fixed 192 KB is 66,944 B (68,056 before).

**Proof:**
- `tests/jpeg/golden.txt` and `golden_once.txt` unchanged: `jpeg_test.py --all`, on the Mac and in vc2sim.
- **`bcmfw/tests/jidct_test.py`** (new, with `bcmfw/jpeg/jidct.c`): the kernel block by block in vc2sim against jpeg_load.c's own C, built by vc2cc with it. The blocks are the ones jdec sends the kernel on 13 corpus decodes (the Mac's jdec dumps them: 99,028; 8,000 sampled by default) and 2,800 made ones (any int16, the old ±2,047/2,048 limit, sparse, DC only, the extremes, the probes). Every pixel equal, the coefficients left 0, and the C equal to the generator's model of it.
- **Mutations** (`gen_jdec.py --mutate`): a rounding term one off (1025), pass 2's low halves dropped (no `x15`), a pixel in the wrong lane. Each makes blocks differ (11, 228 and 272 of 560) and the start-up check fail. With the check in place the decoder then takes the C, so `jpeg_test.py` stays exact: the gate working.
- `decode_image_test.py`'s new gate row: as built `J_VPU` = 7 and two DECODEs with 8x8 blocks call `jv_idct8x4`; built with `BCMFW_JOBS_MAC_FAIL=1`, `J_VPU` = 3 and the same decodes never call it. Golden either way.
- The shell's `jpeg_load.o`, `resize.o` and `bmp.o` are byte-identical (`cmp`, `objdump -d -s`; a `--ipodui` firmware build without ccache).
- `forms_test`: every instruction kind in the display-only image is in Apple's code (a first form's `vclip.2`, into a V8 column, wasn't, hence the H8 rows). `decode_image_test`, `jobs_test`, `jobs_image_test`, `hostproto_test`, `jscan_test`, `jskip_test`, `bench_test`, `mkdevtest --check`, `mkvideo --check` and `gen_jdec.py --check` pass.

**Instruction counts in vc2sim** (one warm-up decode first, so the start-up check isn't counted; *before* is §12's *after*):

| Source and output | Before | After | Saved |
|---|---|---|---|
| `t600_420_q85` → 300 (8x8 chroma) | 9.15 M | **7.15 M** | 21.8 % |
| `t600_420_q85` → 600 (8x8 luma) | 27.80 M | **24.26 M** | 12.7 % |
| `t600_420_q85`, the art set once | 18.55 M | **16.55 M** | 10.8 % |
| `t3000_420_q85`, the art set once (1/8 and 1/4: no 8x8) | 69.16 M | **69.32 M** | −0.2 % |
| `grain3000_420_q85`, the art set once | 115.50 M | **115.67 M** | −0.1 % |

- **Per 8x8 block** at 600: the C was 784 instructions on average (4.41 M); now about 150 in the kernel, 170 for the put and 30 for the batching. The put (64 byte stores into the 4-byte pixels) now costs as much as the transform.
- **Decodes with no 8x8 block** pay about 0.7 instructions a block (the block's coefficient pointer is a load now) and the bigger `struct jpeg`'s move: +0.1–0.4 %.
- Time [estimate, one instruction a cycle]: a 600 px cover's art set 0.124 → 0.110 s at 150 MHz.

**Not done** (no room left, 32 B; each is a next step):
- **The 4- and 16-point twins** (`algorithms.md` §1): the luma at half size (−20 % of 600 → 300 [estimate]) and the 4:2:0 chroma at full size (−52 % of 600 → 600 [estimate]). Each needs about 0.7–1 KB more image. That is a memory-contract change (option (c)): for example, `JDEC_WORK`'s fixed part 192 → 190 KB lowers `J_DEC_WORK_MIN` by 2,048 to 236,592 B and lets `BCMFW_JOBS_DEC_WORK` move to 0xC200 with the drift pool where it is (memory-contract.md §2, album-render.md §5, jobs.h, config.h, jdec.h, build.py's layout check, the PP's job split); 66.9 KB of that fixed part is still unused.
- **DC-only blocks without the kernel** (62 % of the 8x8 blocks at full size): about −120 instructions a block on average, about 70 B of code.
- **A grey file's batches** of four (now one block a call: grey 600 → 600 only −3.4 %), by putting the luma into all three bytes.
- **The put on the vector unit**: needs the row buffer 32-byte aligned and a per-component lane mask.

**Silicon:** every new form is unchecked on the chip. The kernel depends on bench rows X15M, X15S, X15Z and MHDU (`bench-p19.md`), which check the accumulator's halves, `vmulhd` and the carry; on VCLP, `vclip`'s lower bound; and on REPA, its exact REP 8 pair (`vmul … x15 rep8`, `vmulhd … acc rep8`, A an H16 row at `+r4` and B a V16 column, both stepping), which shows whether every repetition adds into the accumulator. Not yet covered by any row: D's `+r` on V16 and H8. The start-up check covers them all in the meantime. If silicon doesn't accumulate across repetitions, `gen_jdec.py --unrolled` builds the REP-1 form (sixteen ops an output in a subroutine: about 130 B more, so it needs room too).

## 14. The 4- and 16-point IDCTs on the vector unit, and 2 KB for the image (2026-10-24, branch `p19-idct416`) [ours]

After §13, jdec still ran two IDCTs in C: the 4-point (the luma of a 4:2:0 file decoded to half size, every component of a 4:4:4 one) and the 16-point (the 4:2:0 chroma at full size, 59 % of a 600 → 600 decode). Both now run on the vector unit, **bit for bit the C's for any int16 coefficients**.

**The layout change** (owner-approved, a memory-contract change). `jdec.h` `JDEC_WORK`'s fixed part 192 → 190 KB (`JDEC_WORK_FIXED`), so `jobs.h` `J_DEC_WORK_MIN` (`JDEC_WORK(142, 142)`, the NP box) 238,640 → 236,592 B, and `config.h` `BCMFW_JOBS_DEC_WORK` 0xBA00 → 0xC200 with the drift pool where it was (0x46000): the work area is 237,056 B, 464 above the minimum as before, and the image gets 2,048 B more. `build.py` now also checks that `J_DEC_WORK_MIN` is `JDEC_WORK(142, 142)`. `memory-contract.md` §2 and `album-render.md` §5 have the new numbers.
- Every baseline corpus art set and single output still decodes in exactly `jdec_work()` bytes (124 runs on the Mac; the progressive file needs its coefficient store beyond it, as before).
- With `jdec_run`'s size check removed, the least spare in the fixed part, after this section's 3,264 B of batches in `struct jpeg`: **62,712 B** (`noise500_420_q85` to 300; on the Mac, whose `struct jpeg` is a little larger than the BCM's).

**One kernel for three sizes** (`gen_jdec.py` → `jv_idct4n(px, coef, d)`): the §13 kernel with a descriptor per size (`jv_idct_d4`, `d8`, `d16`: M, the outputs, pass 1's group step, pass 2's groups, the first output rounding with 1023).
- **4 points.** The blocks keep the 8-point's coefficient places; the 4 × 4 coefficients are the first rows and columns there, and M's columns 4–7 are 0, so the coefficients the decoder stores outside the 4 × 4 (zigzag positions below `k_need`, which the C ignores too) meet zero constants. The REP 8 products are unchanged; four outputs a pass.
  - **One rounding difference, found by the block test:** `jpeg_idct4v` rounds its odd part alone, floor((odd + 1024) / 2048), and adds it to the even part shifted, (x0 ± x2) << 2, for outputs 0 and 1 but subtracts it for 2 and 3. Added, that is the whole sum + 1024, >> 11; subtracted, the whole sum + **1023**, >> 11 (−floor(a / b) = floor((−a + b − 1) / b)). So pass 1's rounding term now comes from a register, 1023 from output 2 on. The 8- and 16-point round the even part before combining, so all their outputs take 1024.
- **16 points** (eight inputs, sixteen outputs): pass 1 writes group g's sixteen columns into rows 16 + 16g (rows 16–47), pass 2 runs four groups of 16 lanes (one block's sixteen rows each), and block q's pixels land at H8 x 16q: two stores, rows 0–15 at x 0, then x 32 moved down.
- M for 16 points fits 16 bits (largest 11,529); the matrices come from gen_jdec's models of the C (`idct4_1d`, `idct16_1d`), checked linear and checked against the C block by block.
- A call of four blocks is 364 instructions at 4 points, 634 at 8 and 1,501 at 16 (vector ops counted as one): 91, 159 and 375 a block.

**The decoder** (`jpeg_load.c`, `JDEC_ALONE` builds only):
- A batch for each component scale in `struct jpeg` (2 × 1,632 B, a multiple of 32 so later buffers keep their alignment); a block whose two scales are equal and 4, 8 or 16 points goes to its batch, four at a time, through a ring of the MCU's blocks of that scale.
- **DC-only blocks skip the kernel**: `idct_take` checks the block's 63 AC places (two rows a turn) and, when all are 0, fills the C's one value directly, `range_limit(128 + ((int16(dc << 2) + 16) >> 5))`: what the C's all-zero shortcuts make at every size (`jpeg_idct4v`'s, 8v's and 16v's columns give `int16(dc << 2)` and 0, the horizontal passes `(w + 4112) >> 5`).
- **Grey files batch four blocks too**: the pixels go to all three bytes, so the per-MCU copy of Y into Cb and Cr, which now runs before them, is overwritten with the same bytes. No per-MCU flush.
- The put is unrolled by four with its strides fixed (about 2.7 instructions a pixel; a general put was 5).

**The gate:** the start-up check runs four blocks at each size (a hash of the index, the extremes, and two rounding probes per size, each one column whose pass-1 output 0 is 16 << 11 and 16 << 11 − 1 before the shift). Any failure keeps the C for all sizes; `J_VPU_MAC` and `BCMFW_JDEC_VPU_IDCT` are as before.

**Room:** image+bss 0xb9e0 → **0xbf20**, 736 B below the work area at 0xC200. The merged image is byte-identical.

**Proof:**
- `golden.txt` and `golden_once.txt` unchanged: `jpeg_test.py --all`.
- `jidct_test.py` at every size: the blocks jdec sends the kernel on 16 corpus decodes, and 1,400 made blocks a size; kernel against the C, the C against the model, coefficients left 0.
- Mutations: `round`, `x15`, `lane`, `store16` (the 16-point's blocks 2 and 3 not stored) each make blocks differ and fail the start-up check; `m4` and `m16` (a matrix constant one off) make blocks differ, and `m16` also fails the check. A constant one off rarely moves a pixel of the check's four blocks; it is the image's own data, which the test checks.
- `decode_image_test.py`'s gate row decodes t300 at 142 (4 and 8 points), h333x500 at 95 × 142 and t142 at its own size (16 points): as built `jv_idct4n` runs, built with `BCMFW_JOBS_MAC_FAIL=1` it never does; golden either way.
- The shell's `jpeg_load.o`, `resize.o` and `bmp.o` byte-identical; `forms_test` (every kind Apple's) and the other bcmfw gates pass.

**Instruction counts in vc2sim** (one warm-up decode first; *before* is the decoder before §13, then after §13):

| Source and output | Before §13 | After §13 | Now | Saved in all |
|---|---|---|---|---|
| `t600_420_q85` → 300 (4-point luma, 8-point chroma) | 9.15 M | 7.15 M | **5.96 M** | 34.8 % |
| `t600_420_q85` → 600 (8-point luma, 16-point chroma) | 27.80 M | 24.26 M | **10.05 M** | 63.8 % |
| `t600_420_q85` → 142 (4-point chroma) | 7.83 M | 7.85 M | **7.27 M** | 7.2 % |
| `t600_420_q85`, the art set once | 18.55 M | 16.55 M | **15.37 M** | 17.1 % |
| `t600_444_q85` → 300 (4-point, all components) | 12.23 M | 12.25 M | **8.54 M** | 30.2 % |
| `t600_grey_q85` → 600 (8-point, batched now) | 24.16 M | 23.40 M | **22.56 M** | 6.6 % |
| `t3000_420_q85`, the art set once | 69.16 M | 69.32 M | **69.41 M** | −0.4 % |
| `grain3000_420_q85`, the art set once | 115.50 M | 115.67 M | **115.83 M** | −0.3 % |

- **At 600 the IDCT went from 74 % of the decode to about 30 %:** the 16-point's 16.2 M in C is now about 0.4 M in the kernel (the blocks with AC terms), plus 2.1 M of DC fills (most of the chroma blocks of a smooth picture are DC only: 256 byte stores each) and 0.8 M of puts.
- **The 3000 px covers** (1/8 and 1/4 scale: no block takes the kernel) pay about 0.3 %: the coefficient pointer's load (0.15 M), the bigger `struct jpeg`'s move, and branch-length effects. vc2cc fuses a compare and branch into one `addcmpb` only when the target is close enough, and the decoder's growth moved a few targets in `jdec_io_read`, `yuv565` and `output_row_8_native` out of reach (one instruction more in their loops: +0.11 M on the grain cover's reader, +0.36 M on the grey file's output rows). Those functions' code is unchanged.
- Time [estimate, one instruction a cycle]: a 600 px cover's art set 0.124 → 0.102 s at 150 MHz; its full-size decode 0.185 → 0.067 s.

**Not done:**
- **The put and the DC fill on the vector unit** (now the largest IDCT costs: 64 or 256 byte stores a block). They need the row buffer 32-byte aligned and a per-component lane mask.
- **A grey file's output rows** (`output_row_8_native`, 59 % of its decode) stay scalar.
- **The code-layout cost** above: ordering the jdec functions so the hot loops' branches stay fused would recover it.

## 15. Streamed outputs: the panel fits the work area (2026-10-24, branch `p19-jdec-stream`) [ours]

**The change:** no output is held whole any more. Before, the primary (the largest output) was decoded into a `dw × dh` bitmap in the work area by `clip_jpeg_fd`'s own resizer, then its window copied out; only the other outputs streamed (`resize_push`, §8). Now the decoder makes **only the rows** (the rows tap's rows-only mode, `jhook.h` `only`), and every output, the primary too, is a `resize_push` over them. Each output row goes through the **row sink** (`jdec.c` `fan_emit`), which writes the window's part of it to the destination's row at its pitch: a BCM slot on the BCM (`J_OP_DECODE` hands `jdec` the slot's first window pixel and its pitch, so a DRIFT slot is written at 304 pixels a row, with no staging), the caller's buffer on the PP.

**Why it is still exact:** `resize_push` is `resize_on_load`'s arithmetic in the same order, and at the rows' own size `clip_jpeg_fd`'s unscaled path (§8), so the primary's pixels are the single decode's. The primary still picks the IDCT scale. Checked: `golden.txt` (the unchanged decoder, S1) on every single decode and every primary, `golden_once.txt` unchanged, the oracle now recomputing the primary too by the shell's pulling `resize_on_load` over the kept rows (`jdoracle.c`, through `jdec_tap_also`).

**In `jpeg_load.c`** (all under `JPEG_JDEC`; the shell's objects are byte-identical): with the tap's `only` set, no bitmap (`bm_size` 0), no resizer, `decode_end` pulls every row through the tap, and the call returns 1; a buffer short of `struct jpeg` or of the rows answers `JPEG_E_NOMEM` (−15), which `jdec` reports as `JDEC_E_NOMEM` (the PP's twin then takes it) instead of a corrupt file. A progressive file's store no longer keeps room for a resizer that doesn't run.

**Where the memory goes** (the BCM's sizes, vc2cc; the Mac's `struct jpeg` is 88,256 B):

| Consumer | Before (rev 2) | After |
|---|---|---|
| `struct jpeg`: the 64 KB read buffer | 65,536 | 65,536 |
| `struct jpeg`: the entropy decoder's fast tables (`dc_fast`, `ac_fast` 8,192; `ac_skip` 2,048) | 10,240 | 10,240 |
| `struct jpeg`: the IDCT batches (§14) | 3,264 | 3,264 |
| `struct jpeg`: Huffman tables and their derived tables (7,616), quantisation tables (512), the rest | 8,512 | 8,512 |
| **`struct jpeg` in all** | **87,552** | **87,552** |
| The decoder's rows: one MCU row at the primary's scale, 3 B a pixel (600 → 300: 7,296; 3000 → 300: 2,256; 600 → 142: 1,824; 500 → 300, full scale: 24,576) | the same | the same |
| The primary's output | `2 dw dh`: 180,000 (300 × 300), 40,328 (142 × 142) | **0** (streamed into its slot) |
| The primary's resizer | `resize_on_load`'s three rows, `48 dw`: 14,400 at 300 | `resize_push`, `JDEC_WORK_OUT`: 15,512 at 300 |
| Each other output's resizer (`JDEC_WORK_OUT` = `50 dw + 512`) | 7,612 (142), 3,262 (55) | the same |
| The progressive store | off on the BCM (`J_E_DEC_UNSUPPORTED`); on the Mac up to `JDEC_PROG_MAX` beyond | the same |
| The input | the 32 KB ring, a `J_CLS_SCRATCH` slot outside the work area | the same |

**The budget, `jdec_work()`** (what the PP checks before it sends, `bcmjob_dec_work()`):
- before: `JDEC_WORK(primary)` = `2 dw dh + 12 dw + 194,560`, plus `JDEC_WORK_OUT` for each other output;
- now: `JDEC_WORK_BASE` (92,160: `struct jpeg` and its alignment, checked in `jpeg_load.c`) + `JDEC_WORK_ROWS(primary dw)` (`96 dw + 768`: 16 lines of `2 dw + 16` pixels, 3 B each, which bounds the rows at every scale but 1/8; at 1/8 the rows are at most 4 lines of a source more than 8 `dw` wide, inside the bound until the source passes about 64 `dw`, 19,200 px for the panel, where the decoder answers NOMEM) + `JDEC_WORK_OUT` for **every** output.

| Request | Before | Now | Fits 237,056 B? |
|---|---|---|---|
| NP box, 142 × 142 | 236,592 | **114,172** | yes (both) |
| Panel fill, 300 × 300 | 378,160 | **137,240** | **yes** (was refused) |
| Art set, 300 + 142 + 55 | 389,034 | **148,114** | **yes** |
| 2:3 cover's fill, 300 × 451 (window 300 × 300) | 468,760 | **137,240** | **yes** |
| 16:9 cover's fill, 534 × 300, with its box 142 × 80 and thumbnail 98 × 55 | 534,392 | **184,428** | **yes** |
| The widest fill alone under `J_DEC_WORK_MIN` | 142 wide | **980 × 300** (a 3.27:1 cover; with its box and thumbnail, about 870) | |
| A 16:1 cover's fill, 4,800 × 300 | 3,132,160 | 794,240 | no: HP_E_NOMEM, the twin |

A portrait fill costs what the square does: the bound follows `dw`, never `dh`.

**Measured** (the Mac, each decode in a work area of exactly `jdec_work()` bytes, filled with a pattern: `jdtest --tight`, the longest run left untouched):
- every baseline corpus decode, 98 single decodes and 30 sets decoded once (the progressive file needs its store beyond, as before), plus four panel cases (the 16:9 and 2:3 fills, alone and with their sets), decodes in exactly `jdec_work()` with the same pixels as in 8 MB;
- **the least spare: 4,800 B** (`noise500_420_q85` → 300, a 500 px source at full scale: 32 MCUs of 16 lines); the 600 px covers leave 23,984 B, the 3000 px ones 30,705 B. The BCM's `struct jpeg` is 704 B smaller, so its spare is that much more.
- what the panel's 300 × 300 really uses, then: about 106.5 KB from a 3000 px cover and 113.3 KB from a 600 px one, against 378 KB of budget before.

**On the BCM** (`decode_image_test.py`, the image in vc2sim with its own work area, the file streamed through the ring): the 300 × 300 fill from `t600_420_q85` and `t3000_420_q85`, the art set from `t3000` in one job, the 16:9 set (534 × 300 cropped to 300 × 300 at x 117, the 142 × 80 box, the 98 × 55 thumbnail cropped at x 21) and the 2:3 fill (300 × 451 cropped at y 75) all decode, every window equal to the Mac's `jdec` (the CPU twin's code) cropped the same way; the 300-wide windows land in DRIFT slots at their 304-pixel pitch. Instructions: 6.3 M (600 → 300), 62 M (3000 → 300), 75 M (3000, the set), 53 M (the 16:9 set), 28 M (the 2:3 fill) [sim, read in 100 k-instruction steps]. **Mutation:** the sink writing rows at the window's width instead of the slot's pitch fails all five 300-wide windows.

**The contract** (decided 2026-10-24, the owner's option (b)): `J_DEC_WORK_MIN` is now **184,428 B**, the 16:9 art set (a 534 × 300 fill plus the NP box and the thumbnail): wide art, and room for video later. The work area stays 237,056 B; the 52,628 B above the promise go back to the pools only when a pool needs them. A single fill wider than about 623 × 300 (2.08:1) now goes to the twin. `build.py` now checks that `J_DEC_WORK_MIN` holds at least `jdec_work()` of the NP box (114,172), instead of equal to rev 2's formula.
- `J_DEC_WORK_MIN` could drop to **114,172** (the NP box) or, to promise the panel and the whole set, **148,114** (the square art set) or **184,428** (the 16:9 set);
- the work area could then give back up to **122,884 B** (to the NP box), **88,942 B** (the square set) or **52,628 B** (the 16:9 set) to the pools or the image.

**Room:** image+bss 0xbfe0 → **0xc020** (+64 B: the rows-only checks, less the primary's copy loop; the oracle's tap is left out of the BCM image), **480 B** below the work area at 0xC200 (on `p19-bcm-prep` with the visualizer overlay's stub). The merged video image is unchanged (`mkvideo.py --check`).

**Cost** [sim]: the primary now goes through `resize_push` and the row sink instead of `resize_on_load` and a final window copy: +0.5 % on a 600 px cover's art set (15.37 → 15.44 M), +1.2 % on `t3000` (69.41 → 70.22 M), +0.7 % on `grain3000` (115.83 → 116.66 M). The single decodes move about as much.

**Proof:** `jpeg_test.py --all` (244 outputs, 0 differ; `golden.txt` and `golden_once.txt` unchanged), `decode_image_test.py` (the panel row above), `jidct_test`, `jskip_test`, `jscan_test`, `jobs_test` (jscene unchanged), `jobs_image_test`, `forms_test`, `mkdevtest.py --check`, `mkvideo.py --check`; the shell's `jpeg_load.o`, `resize.o` and `bmp.o` byte-identical against `70fa9d7b7c` (`--ipodui` firmware flags, no ccache; `cmp`, `objdump -d -s`).

**Found on the way** [ours, host]: the shell's bit-serial entropy decoder (the stock loop, which `jdec` runs only under `JDEC_BITSERIAL` in `jskip_test.py`; the shell itself runs it on the PP) reads up to about 1 KB **before** its `struct jpeg` on some corrupt files (a restart marker met early, the bit count past the buffer's width: `peek_bits` on a negative count indexes `look_nbits` out of range). It was hidden on the Mac because the bitmap sat before the state; with the state now first in the work area it read an unmapped page. Harmless on the targets (the BCM's image and the PP's heap lie before the work area; the value only feeds a decode that fails as corrupt), so `jdtest` now puts 64 KB of memory before its work area as the targets have. Not fixed in the decoder (upstream code, the shell's objects stay identical).

## 16. The put and the DC fill on the vector unit (2026-10-25, branch `p19-jdec-put`) [ours]

After §14 the largest IDCT-side costs left were the **put** (a batch's pixels into the MCU-row buffer, about 2.7 instructions a pixel in C) and the **DC fill** (a DC-only block's one value: 64 or 256 byte stores; 2.1 M of a 600 → 600 decode). Both now run on the vector unit (`gen_jdec.py` → `jv_put`, `jv_fill`; `jhook.h` `jdec_put`, `jdec_fill`), **byte for byte the C's**.

**The method.** The row buffer holds 4-byte pixels (`struct uint8_rgb`: Y, Cb, Cr, pad), each block writes one byte of each. A 32-byte line (8 pixels) loaded as one H16 row puts byte `4k + c` in column `(c >> 1) + 16 (c & 1) + 2k` (`isa-vector.md` "VRF layout"). Each block row's line (two for 16 points, the second at x 32) is loaded whole, the block's own component bytes are written in the VRF, and the line is stored whole: a read-modify-write, one block after another, so blocks that share lines (Cb and Cr of an MCU, two 4-point blocks side by side) stay exact.
- **Put:** `jv_idct4n`'s px (rows 48–63, reloaded) holds pixel column k of a block as an H8 row, lanes n = the block's rows; one `vadd` into the **V8 column** of pixel k writes lane n into the copy's row n, and nothing else. Two moves a column pair (k and k + 8).
- **Fill:** one H8 `vmov` of the value, REP 16, predicated on the component's lanes (`vbitplanes` 0x5555 or 0x55, shifted by `t >> 1`, then `ifnz`): no column loop at all.
- The copy sits in rows `16 − size … 15`, so `jv_store16` entered at row 16 − size (new labels `jv_store16_8`, `jv_store16_12`; `jobs_kernels.S`, no byte changed) stores exactly the block's rows with the device-checked single block-0 `vst16`s.
- Consecutive blocks in one line share one load and one store (the put's merge: Cb then Cr).
- Grey: Y into bytes 0–2 (the put: three passes; the fill: two masks).

**The decoder** (`jpeg_load.c`, `JDEC_ALONE` only): `idct_run` calls `jdec_put`, `idct_take`'s DC-only blocks `jdec_fill`; the C `idct_put`/`idct_fill` left jdec's builds (they live on as `jidct.c` `jdec_put_c`, `jdec_fill_c`: the test's reference and the Mac dump's hooks). The lines must be 32-byte aligned: jdec's builds pad the MCU-row buffer's row step to 32 bytes (`store_row_jpeg`, `clip_jpeg_fd`'s `decode_buf_size`; at most 16 B a row, only 4:4:4 and grey files at 4 points with an odd MCU count), and the batching runs only on a 32-byte aligned buffer.

**The gate** (`jdec_vpu.c`): the start-up check now compares row buffers. At each size the four test blocks go through `jv_idct4n` + `jv_put` into one 2 KB buffer and through `jdec_idct_c` (the decoder's C IDCT, which writes the row buffer itself) into its twin, both filled with a pattern first; every byte is compared, so a byte written into the wrong lane or component is caught. The block places make blocks share lines (8 points: Y and Cb in one line; 16: Y with Cr, Cb with the fourth byte; 4: two to a line). Then `jv_fill` against a C fill of the same places, two blocks grey. A failure keeps the C path for everything (IDCT, put and fill together); `J_VPU_MAC` and `BCMFW_JDEC_VPU_IDCT` as before. The check costs 124 k instructions at start (113 k before).

**Room:** image+bss 0xc020 → **0xc1e0**, 32 B below the work area at 0xC200 (kernels 400 B, the check +150 B; the C put and fill −196 B). The work area, pools and the overlay region are unchanged; the merged image is byte-identical (`mkvideo.py --check`).

**Instruction counts in vc2sim** (one warm-up decode first; *before* is `0703923a32`):

| Source and output | Before | After | Saved |
|---|---|---|---|
| `t600_420_q85` → 300 | 6.04 M | **5.53 M** | 8.4 % |
| `t600_420_q85` → 600 | 10.58 M | **8.31 M** | 21.4 % |
| `t600_420_q85`, the art set once | 15.44 M | **15.12 M** | 2.1 % |
| `t600_444_q85` → 300 (4 points) | 8.62 M | **8.27 M** | 4.0 % |
| `t600_grey_q85` → 600 | 23.70 M | **21.57 M** | 9.0 % |
| `t3000_420_q85`, the art set once | 70.22 M | 70.64 M | −0.6 % |
| `grain3000_420_q85`, the art set once | 116.66 M | 117.08 M | −0.4 % |

- The 3000 px covers decode at 1/8 (no batched blocks); their +0.42 M is all `scale_h_area` in the test build, a branch vc2cc no longer fuses (`addcmpb` → `cmp`/`beq`, its relaxation only grows sizes) after the code before it moved. The display-only image's `scale_h_area` is unchanged but for address constants, so on the BCM it costs nothing.
- Per block (8 points, vector ops as one): a put about 60 instructions in the kernel for 64 pixels (C 170), a fill about 45 (C 110); 16 points about 130 and 85 (C 690 and 400). At 4 points the vector put and fill only about match the C (16 pixels a block), but keeping the C for them didn't fit the image.

**Proof:**
- `golden.txt` and `golden_once.txt` unchanged: `jpeg_test.py --all`.
- `jidct_test.py`: 900 put and fill calls (4, 8, 16 points; row steps 32–192 B; one to four blocks at random places and components, grey or not, shared lines) against `jdec_put_c` / `jdec_fill_c`, every byte of the buffer. Mutations `mask` (the fill's lane mask one lane on), `stride` (the put's interleave stride 3) and `dc` (the fill's value one off) each make calls differ and fail the start-up check, as the six IDCT mutations still do.
- The shell's `jpeg_load.o`, `resize.o`, `bmp.o` byte-identical (`--ipodui` firmware, no ccache; `cmp`, `objdump -d -s`). `forms_test`, `decode_image_test`, `jskip_test`, `jscan_test`, `jobs_test`, `jobs_image_test`, `viz_test`, `bench_test`, `mkdevtest --check`, `mkvideo --check`.

**Silicon:** unchecked. The new forms (a V8 D at +r with a y not a multiple of 16; the masked H8 `vmov` at +r, REP 16, `ifnz`) are listed in `gen_jdec.py`; bench row **PUTV** (`bench-p19.md`) checks them. The start-up check covers them in the meantime.
