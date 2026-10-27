# Algorithms survey: the BCM2722 kernels and jobs against the state of the art

P19, branch `p19-algos`, 2026-10-24. Desk research plus a few measurements in vc2sim and on the Mac (tags: [sim] = vc2sim instruction counts, [mac] = host run, [estimate], [hypothesis]).

**The owner's standing rule:** clever people may have found better methods since the originals were written. So for every BCM kernel and job this file puts the current state of the art next to what we use, and says whether to adopt it.

**The hard constraint:** FADE, AVG and SHADE (`jmix.h`), the JPEG decode's pixels (`jdec`: `jpeg_load.c` and `resize.c`) and the COMMIT hash are **shared definitions**. The PP's CPU path and the BCM must compute them bit for bit the same. A method is therefore one of two kinds:
- **exact:** same results, so either side may adopt it on its own;
- **a new definition:** both sides change together. It must be deterministic integer code, cheap on the ARM7TDMI (no SIMD), and either visually equivalent or signed off by the owner.

**Measurements** (reproducible with `bcmfw/algobench/`):
- `algos_bench.py sum`: the hash candidates;
- `algos_bench.py huff`: the entropy decoders;
- `algos_bench.py prof`: a per-function profile of the real `jdec_one`;
- `fade_identity.c`: the FADE identity.

**Test files:** synthetic grainy covers made with numpy and libjpeg-turbo's `cjpeg -quality 85 -sample 2x2`:
- `g600`: 600 px, 110 KB;
- `g1200`: 1200 px, 369 KB;
- `g3000`: 3000 px, 2.26 MB, close to `jpeg-spike.md`'s 1.9 MB grain cover.

## Adopted (branch `p19-jdec-fast`, 2026-10-24)

Items 2, 4, 6, 9a and 9c are in; all exact. Item 2's AC skip loop followed (branch `p19-jdec-ac`, list item 6 below), then item 1's 8×8 IDCT on the vector unit (branch `p19-vpu-idct`, list item 7). Item 7 (F16w) went in separately as jobs rev 0.2, a new definition by design (`jobs.md` §3). **Proof:** `tests/jpeg/golden.txt` and `golden_once.txt` unchanged on the Mac and in vc2sim over the whole corpus (`jpeg_test.py --all`, the vector colour stage included), the progressive pairs unchanged, the corrupt and truncated files failing as before; the shell's `jpeg_load.o`, `resize.o` and `bmp.o` byte-identical (`docs/changes-from-rockbox.md` 2.29.4).

**Whole decodes** [sim; the art set 300 / 142 / 55 decoded once, and single outputs; *before* is the branch point, whose vc2sim build linked the byte-wise `memcpy`/`memset` (§9c)]:

| Source | Art set once | 300 alone | 55 alone | Entropy decoding (once) |
|---|---|---|---|---|
| `t600_420_q85` (600 px, 31 KB) | 31.75 → **19.07 M** (−40 %) | 20.53 → 9.67 M | 6.44 → 4.16 M | 4.12 → 3.35 M |
| `t3000_420_q85` (3000 px, 0.46 MB) | 125.8 → **78.2 M** (−38 %) | 111.6 → 65.8 M | 92.2 → 53.5 M | 68.0 → 36.8 M (**1.85×**) |
| `grain3000_420_q85` (3000 px, 1.9 MB) | 273.1 → **157.0 M** (−43 %) | 258.9 → 144.6 M | 239.6 → 132.3 M | 198.2 → 101.6 M (**1.95×**) |

- *Entropy decoding* is `store_row_jpeg` (with `huff_block` and the MCU loop inlined), `fill_bit_buffer`, `jpeg_getc`, `hb_fill` and `hb_slow`; *before*, the same functions.
- **Time** [estimate, one instruction a cycle; the vector ops' REP is counted as one]: the 1.9 MB cover's art set 1.82 → 1.05 s at 150 MHz (10.1 → 5.8 s at 27 MHz); the 3000 px pattern 0.84 → 0.52 s; a 600 px cover 0.21 → 0.13 s.
- **Where the rest goes now** (the grain cover's art set): `store_row_jpeg` 95 M (the AC skip loop and per-block glue, about 450 instructions a block for this grain), `scale_h_area` 19 M, `jdec_io_read` 17.6 M (its per-byte EOI scan: since cut to 3.4 M, item 5 below), the IDCTs 6.6 M.
- **Of the 31.75 → 19.07 M for t600:** the colour stage 8.96 → 1.15 M, `memset`/`memcpy` 4.7 → 0.5 M (§9c and the inline block clear), entropy 4.1 → 3.4 M.

**What each change is:**
1. **§2 + §9a, the entropy decoder** (`jpeg_load.c`, `JPEG_JDEC`). One 9-bit table per Huffman table (`len << 4 | nb`, symbol, and the sign-extended value for code + magnitude ≤ 9 bits); the bit buffer in registers; the 64 KB input buffer read in place. **The fill rule is the old one:** 16 bits are added exactly when `check_bit_buffer` would add them (an entry is used only when its bits are already in the buffer, and then the old decoder would not have refilled either). So the stream position, `marker_ind` and the restart search are the old ones, byte for byte, in corrupt files too; that is why the survey's 25-bit and word refills are not used (they read ahead). Two bytes come straight from the buffer unless either is 0xFF, a restart marker is pending or the buffer is nearly empty, when `fill_bit_buffer` itself runs. Codes over 9 bits and bad codes take the old walk. Per-block component lookups moved to once per MCU row, and the coefficient clear is inline: on 1/8-scale covers that per-block glue was larger than the Huffman work.
   - **One deliberate change:** a Huffman table with more codes than its lengths can hold (no encoder writes one) made `fix_huff_tbl` write past its lookahead table (memory corruption, no defined result). In jdec it now counts as a bad code (`JDEC_E_CORRUPT`).
   - **Not adopted:** the 12-bit multi-symbol skip table (16 KB more in `struct jpeg`, which lives in the decode work area, for an estimated 4 % of a big cover) and the word refill (it changes when bytes are read). The tables cost 8 KB of the work area: every corpus file's art set still decodes in exactly `jdec_work()` bytes (checked on the Mac against the branch point: the same results), and `decode_image_test.py` decodes in the image's own work area.
2. **§4, the colour stage on the VPU** (`bcmfw/video/gen_jdec.py` → `jdec_kernels.S` `jv_yuv16`; `jdec_vpu.c`; the hook `jdec_yuv565`, `bcmfw/jpeg/jhook.h`, NULL on the Mac and the PP). 16 pixels a call: one `vldp8` REP 16 with a stride of 4 gathers each pixel's bytes into its own row, so the V8 columns are Y, Cb and Cr with a lane per pixel; 33 REP-1 ALU ops; one `vst16` from `d00`. A destination that isn't 32-byte aligned gets the row through a 32-byte buffer. The unscaled rows (600 → 300): 7.22 → 0.57 M for 90,000 pixels, **80 → 6.3 instructions a pixel** (about 7 cycles with the gather's REP 16). After a resize, `output_row_32_native_fromyuv` keeps its scalar 32-bit `SC_OUT` and hands 16 pixels at a time to the kernel: 8.4 → 2.7 M for the art set (3.2×). **Mutation check:** `gen_jdec.py --mutate g`, `dither` and `clamp` each make `jpeg_test.py` fail (9, 9 and 7 of 9 outputs).
3. **§6, FADE** (`bcmfw/jobs/jmix.h`): `(d << 5) + (s − d)·w`. `fade_identity.c` (now against the two-multiply form it replaced): 10,995,784,921 cases, 0 differ. `jobs_image_test.py`'s FADE rows (the vector `jv_mix16` against `jmix.h`) pass. The merged image is 32 B smaller.
4. **§9c, the test libc:** the survey was wrong that the image used `bcmfw/lib/string.c`: the display-only image linked `bcmfw/jpeg/libc.c`'s byte-wise `memcpy`/`memset` too (its own unit wins over the library). Now `libc.c` leaves both to `lib/string.c`, which the image links as a library and `jpeg_test.py` (`vc_build()`) and `algos_bench.py` link as a unit, so their counts are the image's.
5. **§9a, the reader's EOI scan** (branch `p19-jdec-io`; `bcmfw/jobs/jscan.h`, `jpeg-spike.md` §11). Only the byte after an 0xFF can change the truncation rule's state, so the runs between 0xFFs are skipped four aligned words a turn with the "has a zero byte" test on `~w`, `(0xFEFEFEFE − w) & w & 0x80808080` (Hacker's Delight 6-1; glibc's and musl's `memchr`). Exact: the same chunks, the same state after each. `jdec_io_read`, the art set decoded once [sim]: grain3000 17.63 → **3.40 M** (whole decode 157.0 → **142.7 M**), t3000 4.77 → **0.93 M** (78.2 → **74.4 M**), t600 0.57 → **0.16 M** (19.07 → **18.66 M**). Proof: the goldens unchanged (`jpeg_test.py --all`); `jscan_test.py` (the fast scan against the byte-wise definition on the Mac and in vc2sim, seven edge files made from the corpus, six mutants, all caught). Image +160 B (0xb660 → 0xb700). libjpeg-turbo never scans: it meets markers in the entropy decoder (`unread_marker`). jdec keeps the scan, since that would not give the same accept/reject on every file (§11).
6. **§2 again, the AC skip loop** (branch `p19-jdec-ac`; `jpeg_load.c` `fix_huff_fast()`, `huff_block()` and `hb_skip_one()`, `jpeg-spike.md` §12). A coefficient the scaled IDCT drops needs only its length, not its value (stb_image's `fast_ac` with the value left out). So a 9-bit `uint16_t` table per AC table (2 KB in the work area) gives the bits a symbol takes, code and magnitude together, and how far `k` moves. The loop holds `u = 32 − bits`, so the next 9 bits are two shifts and the "all bits present" test is one `addcmpb`: about 9.4 vc2sim instructions a symbol instead of about 19. DC-only components skip from their first AC symbol. A limit pointer replaces `buf_left` and `marker_val`, with the cursor in registers inside the loop, so a fill takes 12 instructions instead of 17. Everything else goes to the noinline `hb_skip_one()`, the old code. Exact: a symbol is taken only when the bit-serial decoder would take it with no fill, so the fills are the same. The art set decoded once [sim]: grain3000 142.7 → **115.5 M** (store_row_jpeg 95.3 → 65.0 M), t3000 74.4 → **69.2 M**, t600 18.66 → **18.55 M**. Proof: the goldens unchanged (`jpeg_test.py --all`); `jskip_test.py` (287 corrupt and optimised-table files against the decoder before the change, on the Mac and in vc2sim; eight mutants, all caught). Image +160 B (0xb900 → 0xb9a0, merged with `p19-bcm-prep`). **The multi-symbol table, re-measured, is still not adopted.** On the grain cover's skipped symbols (4.5 bits on average), exact pairing needs three runtime checks: bits present, no fill between, and `k` below 64. With 9-bit windows it costs more than it saves (about 24.2 M against 21.9 M). With 12-bit ones, 2 × 16 KB, it would save about 2.2 M (under 2 %).

7. **§1, the 8x8 IDCT on the vector unit** (branch `p19-vpu-idct`; `bcmfw/video/gen_jdec.py` → `jdec_kernels.S` `jv_idct8x4`; the hook `jdec_idct8`, `bcmfw/jpeg/jhook.h`; `jpeg-spike.md` §13). jpeg_load.c's islow as a matrix, each output's eight 16 × 16 products summed exactly in 32 bits by one REP 8 pair (`vmul … x15`, `vmulhd … acc`), four blocks a call; pass 1 reads the int16 workspace from the accumulator's halves, so there is no prescale and no ±2,047 limit: every 8x8 block of a baseline scan takes it. About 155 instructions a block in the kernel (plus about 200 to batch the blocks and put their pixels) against the C's 656–1,623. Gated by a start-up check against the C (`J_VPU_MAC`). [sim] 600 → 300: 9.15 → **7.15 M** (−21.8 %); 600 → 600: 27.80 → **24.26 M** (−12.7 %); the 600 px art set once 18.55 → **16.55 M**. Decodes with no 8x8 block: +0.1–0.4 %. Proof: the goldens unchanged (`jpeg_test.py --all`); `jidct_test.py` (99,028 corpus blocks and 2,800 made ones against jpeg_load.c's C in vc2sim; three mutants, each caught by the test and by the start-up check). Room: the default Huffman tables as bytes and the shell's pause hooks out of jdec's build (exact, about 1.5 KB) pay for it: image+bss 0xb9a0 → **0xb9e0**, 32 B below the work area. The 4- and 16-point twins needed a memory-contract change (`jpeg-spike.md` §13).
8. **§1 again, the 4- and 16-point IDCTs** (branch `p19-idct416`; `jv_idct4n`, one kernel with a descriptor per size; the hook `jdec_idct(px, coef, size)`; `jpeg-spike.md` §14). The work area's fixed part 192 → 190 KB gave the image 2 KB (`J_DEC_WORK_MIN` 236,592 B, the work area from 0xC200; owner-approved). The 4-point keeps the 8-point's layout with M's columns 4–7 at 0, and rounds its outputs 2 and 3 with 1023 in pass 1 (`jpeg_idct4v` subtracts a separately rounded odd part); the 16-point writes two 16-row groups in pass 1 and four in pass 2. DC-only blocks skip the kernel (the C's one value), grey files batch four blocks. [sim] 600 → 300: 9.15 → **5.96 M** (−34.8 %); 600 → 600: 27.80 → **10.05 M** (−63.8 %); the 600 px art set once 18.55 → **15.37 M** (−17.1 %); the 3000 px covers +0.3–0.4 % (code layout and the pointer load). Exact: the goldens; `jidct_test.py` at every size, six mutants caught. image+bss **0xbf20**, 736 B below the work area.

**The display-only image** (`BCMFW_HOSTPROTO=1 BCMFW_JOBS=1`): image+bss 0xb5a0 → **0xb5c0**, 1,088 B below the decode work area at 0xBA00. The entropy decoder's code (+1.5 KB) and the kernel with its glue (+0.6 KB) are paid for by leaving the file loaders jdec never calls out of jdec's own build (`JDEC_ALONE`, 2.6 KB).

## Rows for `docs/algorithms-survey.md`

| task | chip | current method | state of the art | fit | exact or new definition | expected gain | recommendation |
|---|---|---|---|---|---|---|---|
| JPEG Huffman decode | BCM + PP (shared `jpeg_load.c`) | 8-bit lookahead, a 16-bit refill through a call per byte, up to 3 buffer checks per coefficient, skipped ACs decoded one at a time | libjpeg-turbo `decode_mcu_fast`, stb_image `fast_ac` (code + run + value in one lookup), multi-symbol tables (Choueka–Klein–Perl; Huff0 X2), a register-resident 32-bit refill | scalar on both; tables 23 KB per AC table (one shared set per scan) | **exact** (same symbols, same coefficients; checked) | **1.5–2.0× fewer instructions** for entropy decoding [sim]; about 2× for whole big-cover decodes [estimate] | **adopt** (both sides) |
| COMMIT hash | BCM + PP | FNV-1a byte-wise, 5.5 instr/B on VC, about 10 cycles/B on ARM7 | Fletcher-class sums over 16-bit words with 32-bit accumulators (Fletcher 1982; Maxino & Koopman 2009), or xxHash32 | ARM: 1.5 cycles/B; VC scalar: 1.9 instr/B; a VPU form is possible | **new definition** (no pixels change) | BCM: 2.8× fewer instructions in C, about 5× hand-written; PP: about 6×, and **0** per upload if the hash is cached with the art | **adopt**, together with caching the hash at build time |
| YCbCr → RGB565 + ordered dither | BCM (PP keeps C) | scalar per pixel, about 80 instr/px on VC (22% of a 600 → 300 decode) | libyuv `I420ToRGB565Dither`, libjpeg-turbo NEON colour conversion | every intermediate fits a 16-bit lane, so it suits the VPU directly | **exact** | about 30× on that stage [estimate], so about 20% off a typical cover decode | **adopt** (BCM vector kernel; exact ARM micro-tweaks too) |
| FADE (565) | PP + BCM | packed `0x07e0f81f`, two multiplies per pixel | the same packing with one multiply: `(d<<5) + (s−d)·w` (exact mod 2³²) | ARM: one `MLA`; VPU: one accumulator chain | **exact** (checked on 10.6 G cases) | ARM about 25–30% fewer instructions per FADE pixel; VPU about 10% | **adopt** (ARM now; VPU with the pitch-304 change) |
| JPEG IDCT (1/2/4/8/16) | BCM + PP | LLM "islow" (CONST_BITS 13, 32-bit intermediates); ARM asm on the PP | libjpeg-turbo SIMD islow (16×16→32 multiply-accumulate), AAN "ifast" 16-bit, IJG 9 scaled N×N | the VPU's 32-bit accumulator (`x15` + `vmulhd`, decoded P19) gives an exact 16×16→32 MAC: 2 ops a product, eight in one REP 8 pair | **exact** (bit-exact block by block at every size and on the goldens [sim]) | 600 → 300 −34.8 %, 600 → 600 −63.8 % of the decode [sim] | **adopted**, 4, 8 and 16 points (2026-10-24, gated by a start-up check, `J_VPU_MAC`) |
| Area resize | BCM + PP | 24-bit fixed-point weights, 32-bit accumulators, 4 channels (alpha included) | libyuv `ScalePlaneBox`, Pillow-SIMD's 16-bit fixed-point convolution, stb_image_resize2 | exact needs 32-bit lanes (not VPU); 16-bit weights would vectorise | dropping the unused alpha channel is **exact**; 16-bit weights are a **new definition** (±1 LSB) | exact: about 20–25% of the resize stage (10–20% of a decode); new: about 10× on the stage | **adopt** the alpha cut; the vector resizer only with the owner's sign-off |
| Thumbnail blur | PP (build time) | 3 sliding box passes (R 5, 6, 5) per axis, O(1) per sample: Wells/Kovesi | the same (Getreuer 2013's survey); extended box (Gwosdek 2011); dual-filter / Kawase (GPU: downsample, blur, upsample) | ARM fine; not shared with the BCM | the reduced-resolution form is a **new definition** (pixels change) | 5–10× on a rare, build-time-only job | **no** (prototype only if build time matters) |
| AVG2, SHADE | PP + BCM | SWAR `(a&b)+(((a^b)&0xf7de)>>1)`; per-channel `(c·k)>>8` | the same (Hacker's Delight SWAR) | already optimal on ARM; the VPU lacks a known `vand` | — | VPU: only through alignment and the REP-16 store | **keep** |
| Out-of-phase source (BLIT, FADE, AVG, the animations) | BCM | in phase: one aligned `vld16` per column; out of phase (source and destination x differ mod 16): all in C | the unaligned-load idiom: two aligned loads and a shift or permute (AltiVec `lvsl`/`vperm`, NEON `vext`, SSSE3 `palignr`) | the VPU has no permute, but its 2-D VRF does it: two aligned columns side by side in one row, then two H16 A operands at a `+r` lane offset, the second predicated (`vbitplanes`) | **exact** (a copy; jmix.h unchanged) | the drift's pan 3.2×, its FADE toward a cover out of phase 11.8×, AVG's half step 4.8×, a SLIDE frame 2.8× [sim] | **adopted** (2026-10-24, `jobs.md` §10; A's `+r` unchecked on silicon: bench ASHF/ASHG) |
| H.264 CAVLC levels | BCM | clz + row tables; a level costs about 25 instr | ffmpeg `cavlc_level_tab` (prefix + suffix of small levels in one lookup) | scalar, about 1 KB table | **exact** | levels about 2× cheaper, about 5–10% of parse [estimate] | **prototype** |
| H.264 deblock / IDCT / MC | BCM | per-edge and per-4×4 kernels plus C glue (deblock 1,497 instr/MB, 780 of it glue) | ffmpeg/x264 NEON: a whole MB edge per vector, bS in vectors, `idct_add16` batched by nnz | 2-D VRF suits whole-MB batching | **exact** | deblock about 1.5–2× (10–20% of a P frame) [estimate] | **prototype** (already in `decoder.md`'s plan) |
| JPEG reader I/O | BCM + PP | `jpeg_getc` called per byte, plus `jdec_io_read`'s copy | read the input buffer directly with an inline refill (libjpeg-turbo, our own H.264 reader) | scalar | **exact** | 9–30% of a decode (part of the Huffman row) | **adopt** (with the Huffman change) |
| Visualizer FFT (256/512 real samples) | BCM + PP (shared `jviz.c`) | new (#126): no earlier code | real FFT via an n/2 complex FFT; four-step (Bailey) for SIMD; radix-4 / split-radix; CMSIS-DSP q15 per-stage ÷2 vs block floating point; KISS FFT fixed point | VPU: four-step over the 2-D VRF (radix-2 DIF down rows, then along V16 columns; free transpose), exact 32-bit twiddle sums (`x15` + `vmulhd`); ARM: the same C | **one definition** (bit-exact kernel vs C on 110 signals [sim]); 72 dB from full scale to the error floor against a double FFT [mac] | `SPECTRUM` 512: 0.37 ms vs 4.2 ms in C at 27 MHz (11×) [sim] | **adopted** for the proposed SPECTRUM job (`visualizer.md`); radix-4 a later polish (about 10 % [estimate]) |

## Summary table

| # | Item | Ours | State of the art | Fits VPU / ARM7 | Exact? | Gain | Recommendation |
|---|---|---|---|---|---|---|---|
| 1 | JPEG IDCT | islow LLM, 32-bit | turbo SIMD islow, AAN 16-bit, IJG scaled | VPU: 32-bit MAC via `x15` + `vmulhd` (P19) / ARM: asm already | exact | kernel 91 / 159 / 375 instructions a block at 4 / 8 / 16 points vs the C's 354 / 1,017 / 5,606; 600 → 300 −34.8 %, 600 → 600 −63.8 % [sim] | **adopted** at all three sizes (2026-10-24, gated; silicon rows X15M… and a REP-accumulate row pending) |
| 2 | JPEG Huffman | 8-bit LUT, 16-bit refill, per-symbol skip | turbo fast path, stb `fast_ac`, multi-symbol | scalar both | exact | **1.5–2.0×** [sim]; measured 1.85–1.95× on big covers; the skip-only table then took another 20 % off the grain cover | **adopted** (2026-10-24; the skip-only table too; the multi-symbol skip not, re-measured) |
| 3 | Area resize | 24-bit weights, 4 channels | box/convolution with 16-bit weights | VPU only as a new definition | alpha cut exact | 20–25% of stage; 10× as new definition | adopt the alpha cut; vector as sign-off |
| 4 | YCbCr → 565 + dither | scalar 80 instr/px | libyuv dithered 565, NEON | VPU: all 16-bit / ARM: tweaks | exact | about 30× on stage, about 20% of a decode; measured 80 → 6.3 instr/px (unscaled rows) | **adopted** on the BCM (2026-10-24; the ARM tweaks not yet) |
| 5 | Thumbnail blur | 3-box cascade | same; extended box; dual-filter | ARM | new definition | 5–10× (build time) | no |
| 6 | FADE/AVG/SHADE | packed 2-mul fade; SWAR avg | 1-mul packed fade | ARM `MLA`; VPU acc chain | exact | ARM about 25–30% per pixel | **adopted** (fade, `jmix.h`, 2026-10-24) |
| 7 | COMMIT hash | FNV-1a bytes | Fletcher over 16-bit words with 32-bit sums; xxHash32 | both (VPU later) | new definition | PP about 6× (0 if cached); BCM 3.4× [sim] | **adopted** (rev 0.2: F16w) |
| 8 | H.264 | per-block kernels and glue | whole-MB batching; level tables | VPU, 2-D VRF | exact | 10–20% of a frame | prototype |
| 9 | Other | per-byte reader calls; `k_need` drops a coefficient; byte-wise test libc | inline reader; full kept box | — | exact / new | see §9 | (a) and (c) **adopted** (2026-10-24) |
| 10 | Out-of-phase sources | C copy and C FADE/AVG | two aligned loads and a shift (`vperm`, `vext`, `palignr`) | VPU: 2-D VRF, A `+r` lane offset | exact | pan 3.2×, FADE 11.8×, AVG 4.8× [sim] | **adopted** (2026-10-24; silicon: ASHF/ASHG, and the image's own start-up self-check, `J_VPU`) |
| 11 | The raw FADE (shaded columns) | the shaded block in C, per pixel | loop fusion (Halide): shade inside the fade, each pixel loaded once; per-column factors built once a job | VPU: the factor row as an unstepped vector B, the product's high byte as an H8 operand | exact | raw FADE 2.6× (11.03 → 4.28 ms), SHADE 1.4× [sim] | **adopted** (2026-10-24, §6) |
| 12 | Visualizer FFT and bands | none (new, #126) | real-via-complex FFT, four-step for SIMD, CMSIS q15 per-stage scaling, radix-4/split-radix | VPU: four-step on the 2-D VRF, exact 32-bit twiddle sums; ARM: the same C | one definition, bit-exact [sim] | `SPECTRUM` 512 11× faster than its C (0.37 vs 4.2 ms at 27 MHz) [sim] | **adopted** in the proposed SPECTRUM job (sim only; `visualizer.md`) |

## Top 3 adoptions (speed and battery, no visual change)

1. **The JPEG entropy decoder's fast path (§2), with the inline reader (§9a).**
   - Exact: same symbols and coefficients, so the definition stays.
   - 1.5–2.0× fewer instructions for entropy decoding in vc2sim; the large-cover decode is 88% entropy work.
   - Expect about 2× on big covers: about 1.4 s → about 0.7 s at 150 MHz for a 2 MB cover [estimate].
   - The PP's twin compiles the same file and gains about as much (it decodes the covers it builds: the 45.8 s case).
   - It touches `jpeg_load.c` (upstream Rockbox file), so it goes in `docs/changes-from-rockbox.md`.
2. **The COMMIT hash: cache it with the art, and replace FNV-1a with "F16w" (§7).** F16w: **adopted** in jobs contract rev 0.2 (2026-10-24).
   - Caching takes the PP's per-upload hash to zero: pre-compute at build time (rule 3). That alone is exact and needs no BCM change.
   - The new sum costs about 1.5 cycles/B on the PP instead of about 10, when it must be computed at all.
   - On the BCM, 1.01 M → 0.36 M instructions in C for a 180 KB cover [sim]. At `jobs.md`'s one cycle an instruction that is 40 → 13 ms at 27 MHz (7.2 → 2.4 ms at 150). Hand-scheduled on two dependence chains at about 1.9 IPC it is about 4–7 ms at 27 MHz and under 1.3 ms at 150 [estimate]; the VPU form, about 1.6 ms at 27 MHz.
   - It is a new definition but changes no pixels: a contract rev (`J_OP_COMMIT`), not an owner decision.
3. **YCbCr → RGB565 with the ordered dither as a VPU kernel (§4), exact.**
   - Every intermediate fits a 16-bit lane, and the rounding identities are proved below.
   - About 20% off a typical (≤ 1000 px) cover's decode on the BCM.
   - **Quick win alongside it:** FADE's one-multiply identity on the PP (§6), exact, about 25–30% fewer instructions per pixel of the drift's CPU path.

**Owner sign-off needed only for the pixel-changing options, none of which is recommended now:**
- a 16-bit IDCT definition (§1);
- the vectorised 16-bit-weight resizer (§3);
- the reduced-resolution blur (§5);
- restoring the corner coefficient that `k_need` drops (§9b).

---

## 1. The JPEG IDCT (scaled 1, 2, 4, 8 and 16)

**Ours:**
- `jpeg_load.c`'s LLM ("islow") IDCT: CONST_BITS 13, PASS1_BITS 2, products and sums held in 32 bits.
- Separate vertical and horizontal passes per scale (`jpeg_idct{1,2,4,8,16}{v,h}`), with the column shortcut for all-zero AC columns.
- On the PP the ARM assembly version (under `CPU_ARM`), which gives the generic C's results (`jpeg-spike.md` S1).

**Its cost** [sim, `algos_bench.py prof`, real `jdec_one`]:
- Before the entropy work (§2): **600 px → 300:** idct8h 2.56 M + idct4h 1.16 M + idct8v 1.00 M + idct4v 0.83 M = 5.55 M of 32.0 M (17%). **1200 px → 55:** 1/8 luma and 2×2 chroma: 1.05 M of 46.9 M (2%).
- **Now** (2026-10-24, after §2's adoption the IDCT is the largest stage): `t600_420_q85` **→ 300: 4.44 M of 9.26 M (48%)** (idct8h 1.44, idct4h 1.16, idct8v 1.01, idct4v 0.83); **→ 600: 20.60 M of 27.86 M (74%)** (idct16h 11.91, idct16v 4.28: the 4:2:0 chroma upsampled by the IDCT; idct8h 2.49, idct8v 1.92).
- Big covers decode at 1/8, where the IDCT is DC only. So the IDCT matters for small and medium sources, and most at full size.

**State of the art:**
- **Loeffler–Ligtenberg–Moschytz (LLM) is still the accurate integer IDCT everyone ships** ([LLM 1989](https://doi.org/10.1109/ICASSP.1989.266596): 11 multiplies per 1-D 8-point transform).
  - libjpeg-turbo's SIMD islow (`simd/arm/jidctint-neon.c`, `simd/x86_64/jidctint-sse2.asm`; [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)) is **bit-exact with the C islow**.
  - It gets there with a 16×16 → 32 multiply-accumulate (`vmull/vmlal`, `pmaddwd`).
- **AAN ("ifast")** (Arai, Agui, Nakajima, *Trans. IEICE* E71, 1988) is the 16-bit-friendly form. Five multiplies per 1-D transform; libjpeg-turbo's `jidctfst` SIMD uses `vqdmulh`/`pmulhw`. It is less accurate (IJG documents it as such), and its results differ from islow.
- **Reduced IDCTs** (libjpeg-turbo `jidctred`: 4×4, 2×2, 1×1; IJG libjpeg 9's `jpeg_idct_NxN` for every N from 1 to 16, [ijg.org](https://ijg.org/)) are what our scaled IDCTs already are.
- Nothing newer beats LLM/AAN on operation count for an exact 8-point IDCT. The newer work is the H.264/HEVC integer transforms, which are different transforms and don't apply to JPEG.

**Fit (P19, 2026-10-24):**
- **VPU:** the accumulator is 32 bits (`isa.md` "The 32-bit accumulator" [apple-RE, sim]). A 16 × 16 → 32 product is `vmul … x15` (its low half) plus `vmulhd … acc` (its floored high half), so an exact multiply-accumulate is 2 vector ops a product: the same building block as turbo's `vmlal`/`pmaddwd`.
- **Prototype** (`bcmfw/algobench/vpu_islow.py`, not in any image): jpeg_load.c's 8×8 islow, four blocks at a time (an MCU's 2 × 2 luma).
  - Each output's sum is one accumulator chain over its 8 inputs (the even/odd structure doesn't save ops here, because each product costs the same either way). Pass 1 prescales its inputs by 16 and its constants by 2, so the high half is (A + 1024) >> 11 with no extra op; that needs |F| ≤ 2,047, which every baseline file meets (the corpus' largest is 1,024), and the decoder can route any other block to the C path. Pass 2 takes any int16, as C does: the high half, `vasr #2`, then `vclip #255`.
  - The transpose is free: pass 1 writes each output as a V16 column, and pass 2 reads the rows (the VRF's H/V aliasing). Pass 2 writes V8 columns, so the result is 16 rows of 16 pixels, the MCU itself.
  - **548 vector ALU ops for 4 blocks: 137 a block** (pass 1 244, pass 2 304), plus about 8 load/store rows a block.
  - **Bit-exact** in vc2sim against jpeg_load.c's own C (`idct_bench.c`, built on the Mac) on **56,574 blocks** dumped from the corpus (`t600_*`, `w640x360`, `noise500_*`, `h333x500_420_q95`, at full scale) and 900 random, extreme (±2,047) and sparse four-block sets [sim]. The Python reference agrees with the C on every corpus block [mac]. Controls: `x15` sign-extended, a rounded `vmulhd` or a min-only `vclip` each break 45–60 of 60 sets.
  - The same C on the BCM (vc2cc, vc2sim): **656 instructions for a DC-only block, 1,623 for one with AC terms** (1,017 averaged over the corpus blocks).
- **ARM7:** already hand assembly. Nothing to gain without SIMD.

**Exactness:** exact: the same definition, so the BCM can adopt it on its own (the PP keeps its C/ARM path). Any 16-bit form (AAN, or LLM with Q15 constants) would still be a new definition.

**Gain** [estimate from the counts above; vector ops counted as one instruction, as in §2]:
- `t600 → 300`: the 8-point part (2,888 chroma blocks, 848 instructions each) goes to about 145 a block: −2.0 M (22% of the decode). A 4-point twin for the luma at 1/2 (5,625 blocks, 354 each), about 25 a block [estimate, not prototyped]: −1.85 M (20%). Together about **9.3 → 5.4 M (−42%)**.
- `t600 → 600`: the 8-point luma (5,625 blocks, 784 each): −3.6 M (13%); a 16-point twin for the upsampled chroma (2,888 blocks, 5,606 each) at about 4× the 8-point's ops [estimate]: −14.5 M (52%). About **−65%**.
- Big covers (1/8): nothing.
- Not counted: jdec's glue. Blocks must be batched four at a time, and the output becomes planar rows (the vector colour stage then reads planar Y, Cb, Cr). DC-only blocks (62% of the corpus blocks at full scale) are better served by a constant fill than by the full program.

**Adopted: 8×8 (2026-10-24, branch `p19-vpu-idct`; `jpeg-spike.md` §13), then 4 and 16 points (§14).**
- The kernel differs from the prototype in three ways. (1) Pass 1 reads the accumulator's two halves, `(hi << 5) | (lo >> 11)`, instead of prescaling its inputs, so any int16 coefficient works and the |F| ≤ 2,047 check and its C path are gone. (2) The eight products of an output are one REP 8 pair, A and B stepping, with M's row loaded as V16 columns at stride 0: the code is about 290 B instead of about 5.5 KB unrolled. (3) Pass 2 writes H8 rows, so every kind is Apple's (`forms_test`).
- In the decoder: the 8x8 blocks are batched four at a time straight into the kernel's coefficient places, and the decoder puts the pixels into its 4-byte pixels (the output stays interleaved; no planar rows).
- The silicon question stays open: bench rows X15M, X15S, X15Z, MHDU and VCLP cover the accumulator; nothing yet covers REP accumulating across repetitions. The image's start-up check falls back to the C if any form misbehaves.
- **Then the 4- and 16-point twins** (branch `p19-idct416`, `jpeg-spike.md` §14): one kernel, `jv_idct4n`, with a descriptor per size, after the work area's fixed part gave the image 2 KB. The 4-point rounds its pass-1 outputs 2 and 3 with 1023 (the C subtracts a separately rounded odd part there). DC-only blocks skip the kernel and grey files batch in fours. 600 → 300 −34.8 %, 600 → 600 −63.8 % in all.
- **Next:** the put and the DC fill on the vector unit (now the largest IDCT costs), and keeping the jdec loops' branches fused (code layout, about 0.3 % on big covers).

## 2. JPEG Huffman decoding: today's hot spot

**Ours** (`jpeg_load.c`):
- The bit buffer is `unsigned long` in `struct jpeg`. `fill_bit_buffer()` is a call that adds 16 bits through two `d_getc` (each a `jpeg_getc()` call over the 64 KB read buffer).
- Every symbol: `check_bit_buffer(8)`, an 8-bit peek into `look_nbits[]` (an `int` table) and `look_sym[]`; a second `check_bit_buffer(s)` for the magnitude bits, then `HUFF_EXTEND`.
- Coefficients past `k_need` (every AC at 1/8 scale) are still decoded **one symbol at a time**, only to be dropped.

**Its cost** [sim, `prof`]:
- **1200 px → 55:** `store_row_jpeg` 27.2 M + `fill_bit_buffer` 5.9 M + `jpeg_getc` 4.4 M + `jdec_io_read` 3.9 M = **41.4 M of 46.9 M (88%)**.
- **600 px → 300:** about 14.7 M of 32 M (46%), plus the memsets.
- `jpeg-spike.md`'s grain cover spends 196 M on its 55 px output for the same reason.

**State of the art:**
- **libjpeg-turbo's `decode_mcu_fast`** (`jdhuff.c`).
  - A lookup entry packs `nbits << 8 | symbol` (one load, no second table).
  - The bit buffer lives in registers, and `FILL_BIT_BUFFER_FAST` refills whole bytes, checking each for `0xFF`.
  - The fast path runs only while at least 512 bytes of input remain and no marker has been seen. Otherwise it falls back to the careful decoder, so corner cases keep their exact behaviour.
- **stb_image** ([stb_image.h](https://github.com/nothings/stb/blob/master/stb_image.h)): `FAST_BITS 9`, and **`fast_ac[]`**, where one 9-bit lookup gives the run, the sign-extended value and the total length (code plus magnitude) for the common short codes.
- **Multi-symbol tables.**
  - Choueka, Klein and Perl, "Efficient variants of Huffman codes in high level languages" (SIGIR 1985), and Hirschberg & Lelewer, [*Efficient decoding of prefix codes*, CACM 1990](https://doi.org/10.1145/77556.77566): a k-bit table entry decodes several whole symbols.
  - Yann Collet's Huff0 `HUF_decompress4X2` decodes two symbols per lookup ([FiniteStateEntropy](https://github.com/Cyan4973/FiniteStateEntropy)).
- **Refill design:** Fabian Giesen, ["Reading bits in far too many ways"](https://fgiesen.wordpress.com/2018/02/19/reading-bits-in-far-too-many-ways-part-1/) (2018): a left-aligned 32- or 64-bit buffer, refilled to at least 25 bits so one check covers a whole symbol plus its magnitude bits. Our own H.264 reader already does this (`decoder.md`, "Bit reader").

**What was measured** (`bcmfw/algobench/huffbench.c`, decoding baseline 8-bit Huffman scans with no restarts):
- **The replica:** `dec_ref` replicates `jpeg_load.c`'s reader and block loop, with its 16-bit refill, 8-bit lookahead, slow path, `k_need` skip and zeroing.
- **The fast form:** `dec_fast` uses:
  - a register-resident 32-bit left-aligned buffer, refilled byte by byte to at least 25 bits, with the same byte rules (`FF 00` is `FF`; past the end reads as 0);
  - a 9-bit `len << 8 | sym` table;
  - stb-style `fac[]` for kept ACs (run, value, length);
  - a **12-bit multi-symbol skip table** for the dropped ACs. An entry holds the bits used, the coefficients advanced and an EOB flag, and is taken only when it cannot cross k = 64, so the stopping rule is the replica's.
  - The slow path (codes over 9 bits, and bad codes, which eat 17 bits as the replica does) is the replica's walk.
- **Both** write the same dequantised coefficients, and the per-block checksum (over the kept coefficients) is compared.

| File | k_need luma / chroma (scale) | Mac ref → fast | vc2sim ref → fast (instructions) |
|---|---|---|---|
| g600 | 0 / 4 (1/8, the big-cover case) | 0.8 → 0.5 ms (1.6–1.7×) | 9.05 M → 4.60 M (**1.97×**) |
| g600 | 4 / 24 (1/4) | 0.8 → 0.5 ms (1.5×) | 9.67 M → 5.21 M (**1.85×**) |
| g600 | 24 / 63 (1/2) | 1.2× | 12.01 M → 7.41 M (**1.62×**) |
| g600 | 63 / 63 (full) | 1.0× | 13.86 M → 9.06 M (**1.53×**) |
| g1200 | 0 / 4 | 2.9 → 1.8 ms (1.6×) | 31.4 M → 16.4 M (**1.92×**) |
| g3000 | 0 / 4 | 22.9 → 11.3 ms (2.0×) | not run (about 200 M; the Mac result agrees) |

- Every case gives the same checksum. (vc2sim does not zero bss, so `algos_bench.py huff` times the second replica run, whose first-block state matches.)
- The Mac's out-of-order core hides much of the replica's overhead. The VC II is in order at about one instruction a cycle, so its instruction counts are the better guide, and the ARM7 is like it [estimate: 1.5–2× there too].

**What remains in the fast form** [sim, profile]:
- The byte-at-a-time refill is about 23% of `dec_fast`, at about 11 instructions a byte.
- A word fast path would cut it to about 4: load 4 bytes; a has-zero-byte test of `~w` finds any `0xFF`; assemble big-endian, since there is no byte swap.
- That is a further 10–15%, the libjpeg-turbo refinement.

**Fit:** scalar on both chips; the vector unit plays no part (Huffman decoding is serial).

**Tables:** about 23 KB per AC table and 6 KB per DC table, rebuilt per DHT. The 12-bit multi-skip table is the big one (16 KB as `uint32`; it packs into `uint16`, 8 KB).
- The decoder's block is about 380 KB on the BCM, and the PP's twin sizes its work area with `jdec_work()`. So the tables come out of the work area, not static RAM (rule 3: no large statics).
- Build only the tables a scan uses. Two AC tables at 8 + 1 + 1 KB each is about 20 KB.

**Exact:** yes. Same symbols, same coefficients, the same stopping and error rules (bad codes still count `huff_errors`, and a truncated stream still reads zeros), so `golden.txt` and `golden_once.txt` must not change. That is the acceptance test.

**Gain:** 1.5–2.0× fewer instructions in entropy decoding.
- With §9a's inline reader removing `jpeg_getc`/`jdec_io_read` (17.9% of the 1200 → 55 decode), the big-cover decode should roughly halve: 46.9 M → about 22 M for g1200 → 55 [estimate].
- A 2 MB cover: about 1.4 s → 0.7 s at 150 MHz on the BCM.
- The PP's twin gains about as much [estimate].

**Recommendation: adopt, on both sides.**
- It is a change to `jpeg_load.c` (an upstream file), guarded and recorded in `docs/changes-from-rockbox.md`.
- Use libjpeg-turbo's two-speed structure: the fast path when at least 512 bytes remain and no marker is pending, the existing code otherwise. That keeps the corner cases byte-for-byte the old code's.

## 3. The area resize (`resize.c`)

**Ours:**
- `scale_h_area` / `scale_v_area`: an exact area average with weights `dw · (2²⁴ / sw)`, 32-bit accumulators, rounding `>> 22` and `>> 24` (`SC_OUT`). Four channels (`struct uint32_argb`), **alpha included**.
- Linear when upscaling. The decode-once push form (`resize_push`) runs the same arithmetic.

**Cost** [sim, 600 → 142]: `scale_h_area` 1.62 M + `resize_on_load` 1.05 M + `output_row_32_native_fromyuv` 1.51 M = 4.2 M of 21.5 M (20%).

**State of the art:**
- **Exact integer box filters:** libyuv's `ScalePlaneBox` and `ScaleAddRow` (sums in 16/32 bits, then one reciprocal multiply; [libyuv](https://chromium.googlesource.com/libyuv/libyuv)).
- **Pillow-SIMD** (separable convolution with 16-bit fixed-point coefficients and SIMD multiply-accumulate; [pillow-simd](https://github.com/uploadcare/pillow-simd)).
- **stb_image_resize2** (float, SIMD; [stb](https://github.com/nothings/stb/blob/master/stb_image_resize2.h)).
- Area averaging is what all of them do for downscaling. The difference is the coefficient width.

**Fit:**
- **VPU:** our 24-bit weights need 32-bit products, which the VPU lacks (§1), so an exact vector resize is out.
- **A 16-bit definition** (coefficients in Q8–Q12 per tap, two partial taps at each area's ends, 16-bit sums with 8-bit pixels) vectorises well. 16 output columns a vector; the vertical pass is one accumulator chain over rows. About 10× on the stage [estimate]. But the pixels change by about ±1 LSB: **a new definition, owner sign-off**.
- **ARM7:** the 32-bit form suits it.

**Exact wins:**
- **(a) Drop the alpha channel.** JPEG outputs never read it: `output_row_32_native_fromyuv` ignores it, and `output_row_32_native` reads it only when `alpha_offset > 0`. A three-channel `struct uint32_rgb` path for the JPEG/jdec case removes a quarter of the multiply-accumulates in both passes: about 20–25% of the stage [estimate], identical pixels.
- **(b)** `FILL_BUF` checks per source pixel. A row-at-a-time fast path when `part->len` covers the row saves a little more.

**Recommendation:** **adopt (a)**, a guarded edit of `resize.c` under `JPEG_JDEC`/`HAVE_IPOD_UI`. The vector resizer only if the owner accepts a new definition.

## 4. YCbCr → RGB565 with the ordered dither

**Ours:**
- `yuv_to_rgb()` (`jpeg_common.h`): `(128y + 64 + 179v′) / 128` etc., then `clamp_component`.
- Then `output_row_8_native` or `output_row_32_native_fromyuv`: per channel `(31r + (r >> 3) + δ) >> 8` (G: `63g + (g >> 2)`), with δ = `dither_table[x & 15] ^ (dither_table[y & 15] & 0xAA)`, packed to 565.

**Cost** [sim, 600 → 300, the unscaled path]: `clip_jpeg_fd`'s conversion loop 3.88 M + `output_row_8_native` 3.34 M = **7.2 M of 32 M (22%)**: about **80 instructions a pixel**.

**State of the art:**
- libyuv's `I420ToRGB565Dither` and `ARGBToRGB565DitherRow` (a 4×4 dither matrix added before truncation, SIMD).
- libjpeg-turbo's NEON/SSE2 colour converters (`jdcolext`, 16-bit fixed point, `vqdmulh`) and its table form `jdcolor.c` (precomputed `Cr_r`, `Cb_b`, `Cr_g + Cb_g`).
- The vector form is standard. What matters is staying exact.

**Exact identities** (proved here, so the VPU form equals the C bit for bit):
- **The divide.** `(128y + 64 + c) / 128` with C's truncating division, then clamped to 0..255, equals `clamp(y + ((64 + c) >> 7))` with an arithmetic shift:
  - for a non-negative numerator, division and shift agree;
  - for a negative one, both clamp to 0 (truncation gives 0 for −127..−1 and the shift gives −1; either clamps to 0);
  - and `128y` is a multiple of 128, so it comes out of the floor.
- **The dither's multiply.** `31r + (r >> 3) = (249r) >> 3`, and `63g + (g >> 2) = (253g) >> 2`, since `248r/8 = 31r` exactly.
- **The ranges fit 16-bit lanes:**
  - `179·(v − 128)` ≤ 22,912;
  - `−43u′ − 91v′` within ±17,152;
  - `227u′` within ±29,056;
  - `249·255 = 63,495` and `253·255 = 64,515` fit an unsigned 16-bit lane, and `vlsr` is logical.

**The VPU form** (per 16 × 16 pixels, every op REP 16, about 16.5 cycles each):
- **Load:** Y, U and V as H8 rows.
  - The rows today are interleaved 3-byte `struct uint8_rgb`. Under `JPEG_JDEC` the BCM build can make them planar (a layout change inside the decoder, same values).
  - Or gather them with a stride-3 REP-16 `vld8` into the 2-D VRF, reading the Y/U/V columns as V8 vectors. That needs misaligned `vldp`/`vldq` [hypothesis: REP with a misaligned stride].
- **Compute:** U′ and V′ (2 ops); R, G and B (5–6 ops each: `vmul`, an accumulator chain for G, `+64`, `vasr 7`, `+y`, a saturating add into 8 bits from a zero row, the device-checked clamp form); dither (4 ops a channel: `vmul` 249/253, `vlsr`, `+δ`, `vlsr 8`); pack (`vmul` 2048 and 32, two adds); store.
- About **38 ops per 256 pixels, about 630 cycles, about 2.5 cycles a pixel**, against about 80 scalar instructions [estimate]. δ is one 16-lane row per output row, built in scalar code (there is no known vector XOR).

**ARM7 (exact, PP side):** replace the signed `/ YFAC` with `>> 7` (gcc adds a bias fix-up for signed division by 128: about 2 instructions a channel) and use the 249/253 forms (one multiply instead of a multiply, a shift and an add). That is about 10% of the stage [estimate].

**Recommendation: adopt.** The BCM's `jdec` gets a vector `output_row` for both the unscaled and the resized path (the latter after resize, as now), plus the ARM tweaks. Check with `golden.txt` and `golden_once.txt`, unchanged.

## 5. The thumbnail blur (`artblur.c`)

**Ours:**
- Nearest stretch to W × H into 8-bit planes.
- Then per axis three sliding box passes (R 5, 6, 5: σ ≈ 5.8), the last one darkening, with edges clamped. Separable, O(1) per sample.
- 4×4 Bayer dither to 565.
- PP, build time only (the art builder). **Not a shared definition.**

**State of the art:**
- **Box cascades are still the CPU state of the art for large-σ Gaussians:** Wells 1986 ([PAMI](https://doi.org/10.1109/TPAMI.1986.4767776)); Kovesi, "Fast almost-Gaussian filtering" (DICTA 2010), which picks mixed box widths for a target σ, as our 5/6/5 does; Getreuer's survey ([IPOL 2013](https://www.ipol.im/pub/art/2013/87/)).
- **Extended box filtering** (Gwosdek, Grewenig, Bruhn and Weickert, SSVM 2011, LNCS 6667) is a closer Gaussian at the same cost.
- **StackBlur** (Klingemann 2004) gives a triangle kernel in one pass.
- **Kawase** (GDC 2003) and the **dual filter** (Bjørge, SIGGRAPH 2015, "Bandwidth-efficient rendering") are GPU methods that save texture fetches. Their CPU equivalent is downsample → small blur → upsample.

**Fit:** ARM only. A reduced-resolution blur (downsample by 4, R ≈ 1–2 box cascade, bilinear upsample, then the same darkening and dither) would do about 5–10× less work. For σ ≈ 5.8 on a darkened, dithered backdrop it is visually close, but the pixels change (the goldens): **a new definition, owner sign-off**.

**Recommendation: no.** It is already O(1) per sample, and it runs rarely and at build time. Revisit only if the art builder's time budget asks for it.

## 6. FADE, AVG and SHADE

**Ours:**
- **FADE** (`jmix_fade_px`): the packed `0x07e0f81f` form, `(d·(32 − w) + s·w + bias) >> 5`. Two multiplies a pixel on ARM (`MUL` + `MLA`).
- **AVG2:** SWAR, two pixels a word.
- **SHADE:** per-channel `(c·k) >> 8`.
- **On the VPU** (`jv_mix16`): unpack R, G and B (G costs 4 ops: hi/lo bytes, no `vand`), two multiplies and an add per channel, `vmulhr`, repack: about 30 ops, about 610 cycles per 16 × 16 block.

**A cheaper exact FADE** (new here):
- `d·(32 − w) + s·w = (d << 5) + (s − d)·w`. This holds **in packed modular arithmetic**: the packed word is an ordinary integer, the borrows of `s − d` between fields are linear, and the true per-field results never overflow their fields. So the packed sum is the same integer mod 2³².
- `fade_identity.c` checks the new `fade1` against `jmix_fade_px` on **10,636,293,096 cases: 0 differ** [mac].
- **ARM7:** one `MLA` instead of `MUL` + `MLA`. gcc 16 makes the loop 14 instructions instead of 20 [compiled, arm7tdmi -O2]: **about 25–30% fewer** per pixel on the drift's CPU path. The shell's `xfade_rect()` calls `jmix.h`, so one header edit serves both, and the BCM's C path gets it too.
- **VPU:** an accumulator chain (`vmul … clra acc`, then `vmul … acc`) drops the separate `vadd` per channel, about 3 of 30 ops (10%) [estimate]. The VPU's real FADE lever is still `jobs.md` finding 1 (a 304-pixel pitch for the 300-wide slots, about 13×) and finding 3 (the REP-16 store).

**State of the art for 565:**
- The `0x07e0f81f` spread is the standard trick in embedded GUIs and needs no citation.
- SWAR averaging: Warren, *Hacker's Delight*, §2-5.
- No exact formulation with fewer than one multiply per pixel is known at our 5-bit weight precision.
- On the VPU, a decoded `vand`/`vor` (if any of the unnamed ops 5, 6, 13 … is one) would shorten the unpack by about 40%. That is a RE question, not an algorithm.

**AVG2 and SHADE:** already minimal on ARM. On the VPU, AVG runs on FADE's kernel. Nothing better without bitwise ops.

**Out of phase (adopted 2026-10-24, `jobs.md` §10):** a source whose x differs from the destination's mod 16 (AVG's second framing always does) used to send the whole job to C. The kernels now fetch it the way SIMD ISAs do an unaligned load: two aligned loads and a lane shift (AltiVec `lvsl`/`vperm`, NEON `vext`, SSSE3 `palignr`). The VPU has no permute, but its 2-D register file gives one: the two 32-byte columns sit side by side in one VRF row, and an H16 A operand read at x + p, then at x + 16 + p on the flagged lanes, is pixels p … p + 15. Exact by construction (a copy). FADE toward a cover out of phase: 48.8 → 4.1 ms at 27 MHz; AVG's half step 23.2 → 4.9 ms [sim].

**The raw FADE, fused (adopted 2026-10-24, `jobs.md` §10):** FADE with b's first n columns shaded (`J_FADE_SHADE_B`, the drift's fade toward a raw cover) used to run those columns, and the 6 up to the first block, per pixel in C: 92 % of the job's instructions. Its block now runs on `jv_mix16`, with the shade **fused into the fade** (Halide-style loop fusion: each pixel loaded once, b's channels shaded where the fade has already unpacked them). Per channel of b, `(c·k) >> 8` is one `vmul` by the factor row, read as the product's high byte, as `jv_shade16` does it; then that byte feeds the fade's own multiply. That is 7 vector ops a shaded block (one REP-1 load and one `vmul` a channel, plus the R channel's widening), where a separate shade pass would be about 17, and no extra store. Exact by construction: the same integers as `jmix_shade_px` then `jmix_fade_px`. The factors (k = 256 − alpha, 256 past the shaded columns: c·256 >> 8 = c) are built **once a job**, on its first group, for two blocks; SHADE's hook does the same now (it rebuilt them every group). The raw FADE toward a cover: 11.03 → **4.28 ms** at 27 MHz (2.6×); SHADE's strip 0.33 → **0.24 ms** [sim]. The price: the plain FADE and AVG pay a test a channel and the factors' argument, about 14 instructions a block (+2 %: 4.15 → 4.23 ms, 4.87 → 4.95 ms).

**Recommendation: adopt** the one-multiply FADE in `jmix.h` (exact, both sides, no golden changes). Keep AVG and SHADE.

## 7. The COMMIT hash

**Ours:** FNV-1a over the slot's bytes, two steps per pixel (`jref.c` `fnv_px`).
- **BCM:** 1.08 M instructions for a 300 × 300 cover (40 ms at 27 MHz, 7.2 ms at 150), over `J_T_SMALL`'s 5 ms (`jobs.md` finding 6).
- **PP:** it hashes every upload. gcc turns the multiply into shift-adds: about 16 instructions plus an `ldrh` per pixel, **about 10 cycles a byte**, so about 1.8 M cycles (23 ms at 80 MHz) per cover.

**What it must catch** (integrity, not security):
- a short or partial upload;
- shifted or duplicated rows (pitch errors);
- swapped halfwords or bytes;
- stale data;
- stuck or floating bus lines (all-`FFFF`).

**Candidates** (`bcmfw/algobench/sumbench.c`, 18,000 random bytes, scaled to 180 KB) [sim; ARM from the arm7tdmi -O2 listings]:

| Hash | VC instr/B | 180 KB on VC | ARM7 cycles/B (approx.) | Properties |
|---|---|---|---|---|
| FNV-1a bytes (today) | 5.50 | 1.01 M | about 10 | good random mixing; serial multiply chain |
| Fletcher-32 (mod 65535, deferred) | 1.95 | 0.36 M | about 1.6 | Maxino & Koopman's best cheap checksum, but **0x0000 ≡ 0xFFFF** (ones' complement), so a black pixel turned white is invisible |
| Words, both sums mod 2³² | 0.94 | 0.17 M | about 0.8 | **blind spot:** any even count of bit-31 errors at even word distances cancels; a stuck D31 line is missed half the time |
| **F16w** (16-bit halfwords into 32-bit sums A, B; no reduction) | 1.94 | 0.36 M (about 0.2 M hand-scheduled) | about 1.5 | **all 1- and 2-halfword errors detected** for slots below 2¹⁷ halfwords (256 KB); no single-value aliasing |
| xxHash32 | 2.50 | 0.46 M | about 3 (two 32-bit `MUL`s a word) | excellent mixing; no `ror` on VC (3 ops) |
| wyhash | — | — | — | needs 64 × 64 → 128 multiplies: poor on both |
| CRC-32 (slicing-by-4) | about 2.5 [estimate] | about 0.46 M | about 5 | HD ≥ 3 at this length (Koopman's tables); 4 KB table; no vector form |

**F16w, the definition:**
- Over the slot's bytes as little-endian halfwords `h_i` (the pixels themselves), `i = 0 … N − 1`:
  - A = Σ h_i mod 2³²;
  - B = Σ_i (N − i)·h_i mod 2³² (the running sum of A).
- **The reply:** carry both words (a contract rev: `COMMIT slot, A, B`). Or fold to `B ^ rotl(A, 16)`, which keeps only the ≈ 2⁻³² random-error rate.

**Its properties:**
- **One error** (|δ| < 2¹⁶) changes A.
- **Two errors** δ₁ at i, δ₂ at j: A unchanged forces δ₂ = −δ₁ (|δ₁ + δ₂| < 2¹⁷). Then ΔB = (j − i)·δ₁ ≡ 0 mod 2³² needs `(j − i)·|δ₁| = 2³²`. With |δ₁| ≤ 2¹⁶ − 1 that needs `j − i ≥ 2¹⁷`, beyond any slot. So **HD ≥ 3**, as for Fletcher, without Fletcher's 0x0000/0xFFFF alias.
- **A stuck line** of bit k adds about k·2^b per word to A, nonzero for any realistic count.
- **Permutations** of distinct values change B.

**Cost of F16w:**
- **ARM7:** per word, `and` (mask register), add, add, add (shifted operand), add: about 6 cycles a word with `ldm`, **about 1.5 cycles a byte (about 6× FNV-1a)**.
- **VC scalar:** 1.94 instructions a byte in C (13 ms at 27 MHz at one cycle an instruction). By hand, about 7 instructions a word on two dependence chains, about 1 cycle a byte at 1.9 IPC: a 180 KB cover then costs about 4–7 ms at 27 MHz and about 0.7–1.3 ms at 150 [estimate].
- **A VPU form** [estimate, prototype]:
  - load 8-row groups as H16 (the lo/hi byte planes read as unsigned H8);
  - column sums by a REP-8 accumulate, and row-weighted sums by a ramp `vmul … acc`;
  - lane totals through the scalar-result `sum` (≤ 30,600 per lane, safe in 16 bits).
  - About 60 cycles per 256 B, **about 1.6 ms at 27 MHz**. It depends on `sum` reducing the lanes exactly [hypothesis].

**The bigger PP win, and exact:** compute the slot's sum once when the art is built (`artcache`), store it with the art, and pass it to COMMIT. The PP's per-upload cost then goes to **zero** (rule 3: pre-compute at build time, never at browse time). This is independent of the hash chosen.

**Recommendation: adopt.**
- Cache the sum at build time, now; it is exact.
- Switch FNV-1a → F16w with both words in the reply at the next contract rev (19-L2).
- Keep FNV-1a for the firmware-image checks (`verified.txt`, `mkvideo.py`): those are one-off and already recorded.

**Adopted (jobs contract rev 0.2, 2026-10-24; the orchestrator's approval).**
- **The contract:** `COMMIT slot, A, B` replies `A, B`; 0, 0 means "compute and reply, don't check"; a mismatch is `J_E_SUM`. A rev 0.1 `COMMIT` (two words) is refused (`jobs.md` §4, §8).
- **The input, made explicit:** the slot's w × h pixels row by row, **the padding of a wider pitch left out** (`J_CLS_DRIFT`'s 304 for 300 wide). So the sum doesn't depend on the pitch, and the PP can sum its own unpadded rows.
- **One function:** `jobs.h` `j_f16w()`, header-only and incremental (a = b = 0, then the pixels in pieces of any size). jref, the host model, the B5 window lab (`vplayer.c`) and the PP driver all use it.
- **Test vectors** in `jobs.h`: the empty input, one pixel, 0x0000 against 0xFFFF, a pair and the pair swapped, a 300 × 300 pseudo-random slot (`J_F16W_SEED`) with its padding set, and 300 × 300 of 0xFFFF where both sums wrap. Checked by `jtest.c` (the function and jref's `COMMIT`) and `jobs_image_test.py` (the image's `COMMIT`).
- **Measured** (`jobs_image_test.py --audit`, the display-only image) [sim]:

| COMMIT a 300 × 300 cover | Instructions | 27 MHz | 150 MHz |
|---|---|---|---|
| FNV-1a (rev 0.1) | 1,082,147 | 40.08 ms | 7.21 ms |
| F16w, the plain loop | 632,176 | 23.41 ms | 4.21 ms |
| **F16w, eight pixels a turn (`j_f16w()`)** | **315,979** | **11.70 ms** | **2.11 ms** |

- The plain loop is 7 instructions a pixel: `ldh`, two adds, and a counter, compare and branch. Eight a turn is 3.5. A loop over words on two chains measured the same 3.5 (W = Σ words and H = Σ high halves, plus their running sums; then A = W − (H << 16) + H, and B = 2·(ΣW − (ΣH << 16) + ΣH) − H). So the simple form stayed: it needs no alignment and no odd-width tail. vc2sim counts one cycle an instruction, so the two chains' dual issue on silicon is not in these numbers.
- **Cost:** 160 bytes more image in both the display-only and the merged images.
- **Not done yet:** caching the sum with the art at build time (the PP side), and the VPU form.

## 8. H.264 (`decoder.md`)

**Ours** (per macroblock, iPod clip P frames):
- CAVLC 2,206 instructions (40%): clz + row-word tables, a level about 25, a run_before about 31, a refill about 25.
- Loop filter 1,497, of which bS is about 470 and the C glue about 780.
- MC 666, mostly glue.
- Transform 230: one 38-instruction kernel per 4×4, plus a DC-only kernel.

**State of the art** (ffmpeg `libavcodec/h264_cavlc.c`, `arm/h264dsp_neon.S`, `x86/h264_deblock.asm`; x264's `common/arm`; [FFmpeg](https://github.com/FFmpeg/FFmpeg)):
- **CAVLC levels:** `cavlc_level_tab[suffix_length][1 << 8]` decodes level_prefix and level_suffix for the common small levels in **one lookup**, falling back to the clz path for the rest. Exact. Our 25 → about 10 instructions a level, about 5–10% of parse [estimate].
- **Deblocking:** one call filters a whole macroblock edge (16 pixels across 16 lanes), with α, β, tc and bS as vectors and masks. Vertical edges go through a transpose (on our VRF, the H/V aliasing makes it free). ffmpeg also computes bS with SIMD (`h264_loop_filter_strength`). That is what `decoder.md`'s next candidate already names. About 1.5–2× on the loop filter, 10–20% of a P frame [estimate].
- **Transform:** `h264_idct_add16` / `add8` batch a macroblock's 16 blocks by their nnz flags, so the dispatch and glue are paid once. With the VRF's 64 rows, 16 coefficient blocks fit at once: about 230 → about 80 a macroblock [estimate].
- **MC:** separable 6-tap with the 16-bit horizontal intermediate reused for the centre position j (ffmpeg's `mc22` paths); edge emulation only for blocks crossing the picture edge. Ours already reuses; the remaining cost is glue.
- **Fit:** all exact (H.264 is bit-exact by specification) and VPU-friendly.

**Recommendation: prototype**, in order: the level table (scalar, small), the whole-MB deblock, then batched transforms. These are the decoder's own next candidates, now with a reference implementation to compare against.

## 9. Anything else

**(a) The JPEG reader's I/O** [sim; exact]
- `jpeg_getc()` is a function call per byte, and `jdec_io_read` copies the input into the 64 KB read buffer.
- Together that is **17.9% of 1200 → 55** (8.4 M of 46.9 M) and 9% of 600 → 300.
- Reading the BCM's input ring (or the PP's file buffer) directly, with the refill inlined into the fast path of §2, removes most of it. It goes with §2.

**(b) `k_need` drops the kept box's last coefficient** [ours, reading `jpeg_load.c`]
- `k_need = zig[(decode_h << 3) + decode_w]` is the zig-zag index of the box's **last** needed coefficient. But the loop keeps only `k < k_need`.
- So the corner coefficient of each scaled box is never used: (1,1) at 2×2, (3,3) at 4×4, and (7,7), zig-zag 63, at full scale.
- The effect is a slight loss of fidelity (a missing diagonal frequency), not a crash.
- Fixing it (`<=`) changes pixels on both sides and every golden: **owner sign-off**, low priority. The progressive store (`jpeg-spike.md` §9) inherits the same K.

**(c) The test harness's byte-wise libc** [sim]
- `bcmfw/jpeg/libc.c`'s `memset`/`memcpy` copy a byte at a time: 9.6% (memset) and 3.3% (memcpy) of the 600 → 300 counts.
- The BCM image links `bcmfw/lib/string.c` (word-wise), so `jpeg_test.py`'s instruction counts overstate the image's cost by about 10%.
- Either link `lib/string.c` in the test, or note it next to the numbers.
- **Correction (2026-10-24):** the display-only image did not link `lib/string.c` for these: `jpeg/libc.c`, one of its own units, defined them first, so the image ran the byte-wise ones too. Both now take `lib/string.c` ("Adopted", item 4).

**(d) The block zeroing**
- `MEMSET(block + 1, 0, zero_need * sizeof(int))` zeroes `int`s over an `int16_t` block: twice the bytes needed. It stays inside the block only because the IDCT workspace is larger.
- Halving it is exact and saves a little at full scale.

**(e) Already in `jobs.md` and confirmed here** as the larger display levers, ahead of any new arithmetic:
- the 304-pixel slot pitch (finding 1);
- the REP-16 `vst16` (finding 3).

## Sources

- **IDCT:** Loeffler, Ligtenberg, Moschytz, ICASSP 1989, [doi:10.1109/ICASSP.1989.266596](https://doi.org/10.1109/ICASSP.1989.266596). Arai, Agui, Nakajima, *Trans. IEICE* E71(11), 1988. [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) (`simd/`, `jidctint.c`, `jidctfst.c`, `jidctred.c`). [IJG libjpeg 9](https://ijg.org/).
- **Huffman:**
  - [libjpeg-turbo `jdhuff.c`](https://github.com/libjpeg-turbo/libjpeg-turbo) (`decode_mcu_fast`).
  - [stb_image.h](https://github.com/nothings/stb/blob/master/stb_image.h) (`fast_ac`).
  - Choueka, Klein, Perl, SIGIR 1985.
  - Hirschberg & Lelewer, CACM 33(4) 1990, [doi:10.1145/77556.77566](https://doi.org/10.1145/77556.77566).
  - [FiniteStateEntropy / Huff0](https://github.com/Cyan4973/FiniteStateEntropy).
  - F. Giesen, [Reading bits in far too many ways](https://fgiesen.wordpress.com/2018/02/19/reading-bits-in-far-too-many-ways-part-1/).
- **Resize and colour:** [libyuv](https://chromium.googlesource.com/libyuv/libyuv) (`ScalePlaneBox`, `I420ToRGB565Dither`); [Pillow-SIMD](https://github.com/uploadcare/pillow-simd); [stb_image_resize2](https://github.com/nothings/stb/blob/master/stb_image_resize2.h).
- **Blur:**
  - Wells, IEEE PAMI 8(2) 1986, [doi:10.1109/TPAMI.1986.4767776](https://doi.org/10.1109/TPAMI.1986.4767776).
  - Kovesi, DICTA 2010.
  - Gwosdek et al., SSVM 2011.
  - Getreuer, [IPOL 2013](https://www.ipol.im/pub/art/2013/87/).
  - Kawase, GDC 2003.
  - Bjørge, SIGGRAPH 2015 course "Moving Mobile Graphics".
- **Checksums:**
  - Fletcher, IEEE Trans. Comm. 1982, [doi:10.1109/TCOM.1982.1095369](https://doi.org/10.1109/TCOM.1982.1095369).
  - Maxino & Koopman, IEEE TDSC 6(1) 2009, [doi:10.1109/TDSC.2007.70216](https://doi.org/10.1109/TDSC.2007.70216).
  - [Koopman's CRC tables](https://users.ece.cmu.edu/~koopman/crc/).
  - [xxHash](https://github.com/Cyan4973/xxHash); [wyhash](https://github.com/wangyi-fudan/wyhash); [FNV](http://www.isthe.com/chongo/tech/comp/fnv/).
- **H.264:** [FFmpeg](https://github.com/FFmpeg/FFmpeg) (`libavcodec/h264_cavlc.c`, `arm/h264dsp_neon.S`, `x86/h264_deblock.asm`); [x264](https://code.videolan.org/videolan/x264).
- **Blend:** H. S. Warren, *Hacker's Delight*, 2nd ed., §2-5.
