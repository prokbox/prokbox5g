# The Now Playing visualizer on the BCM2722: FFT, bars, and room

*#126, branches `p19-viz-fft`, `p19-viz-overlay` (the overlay, §8.1) and `p19-viz-bars` (the vector bars, §5.1), 2026-10-24/25. **Simulation only:** nothing here has run on silicon. The design is `docs/design/p19-visualizer.md` (requirements) and `p19-visualizer-styles.md` (the v1 styles and the shared pipeline), both on `main`. Tags: [sim] = vc2sim counts, [mac] = a host run, [estimate], [hypothesis].*

## 1. In short

- **One definition, three runners.** `bcmfw/jobs/jviz.{c,h}` defines the spectrum (window, FFT, power, bands) and the bars. The PP's CPU path, the reference executor's new jobs (`jref.c`, `JREF_VIZ`) and the BCM all compute it **bit for bit** the same.
- **The FFT on the vector unit.** `jv_fft256` and `jv_fft512` (`bcmfw/video/gen_viz.py` → `viz_kernels.S`) handle 256 or 512 real 16-bit samples. They are **bit-exact against the C** on 110 signals [sim]. The C's error against a double-precision FFT leaves a full-scale sine's peak bin **72 dB** above the error floor [mac], which is enough for bars with a 60 dB range.
- **Cost:**
  - The kernel alone is about 2,200 cycles for 512 samples.
  - The whole `SPECTRUM` job (ring, kernel, bands, reply) is **0.37 ms at 27 MHz** and 0.07 ms at 150 MHz. The C fallback takes 4.2 ms at 27 MHz.
  - `BARS` for the v1 frame (24 bars over 320 × 150) takes **1.37 ms at 27 MHz** and 0.25 ms at 150 MHz on the vector unit (`jv_bars16`, §5.1), bit-identical to the C; it was 4.59 ms and 0.83 ms in C with a vector background fill [sim].
- **Scope, Radial, Spectrogram (§13, 2026-10-26):** `POLYLINE`, `SEGMENTS` and `PALROW` (`0x36`–`0x38`, plain C in the overlay, each behind its start-up check): every v1 style on the BCM, each frame inside 33 ms at 27 MHz with `PRESENT`; the overlay 19,380 B, ABI unchanged.
- **The contract:** two new jobs, **`SPECTRUM` (`0x33`)** and **`BARS` (`0x34`)**, marked proposed and sim only (`jobs.h`, `jobs.md` §5): no shipped image offers them, a PP finds them by their `J_OPS` bits (19, 20), and `J_MINOR` stays at 3 (the marquee's rev) until an image that offers them ships. The FFT is not its own job (§6).
- **Room:**
  - The visualizer needs about **16 KB**: 5.9 KB of vector code, 3.6 KB of C, 4.7 KB of tables and 2 KB of `.bss`.
  - Neither existing image has that resident: the merged image has 20 B to spare and the display-only image 832 B.
  - **Built as an overlay of the display-only image** (§8): linked at `0x13B000`, the layout's spare top 20 KB, against the image's map; 16,724 B loaded plus 2,060 B of `.bss`, 1,696 B of the region spare (the vector bars took 2,144 B of the 3,840 B). The PP loads it with `OVERLAY` (`0x35`) when the visualizer opens. The image's stub costs 288 B; `DECODE` stays resident [sim].
  - Also built as a separate image, `BCMFW_VIZ` (the display-only image with `SPECTRUM` and `BARS` in place of `DECODE`): the warm-swap alternative, and the overlay's cross-check.

## 2. The pipeline, and who does what

| Step | Where | Why there |
|---|---|---|
| Tap the PCM after the DSP, downmix `(L + R) / 2`, take the last n = 256 or 512 samples | PP1 (the mixer hook, as Rockbox's FFT plugin) | the audio is on the PP |
| Write them to a slot in **split order** (`x[0], x[2] … x[n-2], x[1], x[3] … x[n-1]`) | PP1, through the host port (1 KB a frame at 512, 30 KB/s at 30 fps) | free on the PP: it's only the index it writes to; the real FFT wants evens and odds apart, and the vector unit can't deinterleave lanes cheaply (it has no permute) |
| Hann window, FFT, power, bands | BCM vector unit (`SPECTRUM`) | the heavy, regular part |
| Smoothing (attack/release), peak hold and fall, auto-gain, beat, monstercat | PP (on the levels from `SPECTRUM`'s reply) | state: it must survive a BCM restart (slots are a cache), and Mirror, Cover and Aura need the same frame struct (the styles doc: "the frame struct is the contract") |
| Draw the bars | BCM (`BARS`) | pixels next to the frame |
| Push the rect | BCM (`PRESENT`, the existing job) | |

The PP's CPU path runs the same `jviz.c` (`jviz_power_ref`, `jviz_bands`, `jviz_bars_draw`), so a fallback frame equals a chip frame (`jobs.md` §7).

## 3. The FFT kernel

**State of the art and what fits:**

| Method | What it gives | Taken? |
|---|---|---|
| A real FFT of n points as an n/2-point complex FFT of `x[2k] + i x[2k+1]`, plus a split step (`X[k] = E[k] + W_n^k O[k]`) | half the work of a complex FFT of n points | **yes** |
| Four-step (Bailey) / matrix FFT: n/2 = rows × 16, transforms down the rows, a twiddle per element, transforms along the lanes | on a 16-lane SIMD with a 2-D register file, every butterfly works on whole vectors, with no permutes; the transpose is free (H16 rows and V16 columns alias in the VRF) | **yes**: this is what makes the vector unit fit |
| Radix-2 decimation in frequency inside each 16-point (or 8-point) transform | the twiddles are scalars per row (or column): immediates, no table loads | **yes** |
| Radix-4 / split-radix | about 25 % fewer real multiplies, half the stages | **not yet.** Here the 16 lanes are 16 independent transforms, so op count, not flops, is what costs, and the trivial twiddles (1, −i) are already special-cased. A radix-4 16-point would save about 10 % of the kernel's ops [estimate] for more generator code. A later polish |
| CMSIS-DSP q15 scaling (per-stage ÷2, `arm_cfft_q15`) vs block floating point | per-stage scaling never overflows and needs no data-dependent exponent; block floating point keeps about 1 bit per stage on quiet signals, but needs a max-reduction per stage and an exponent per frame | **per-stage ÷2** (CMSIS's rule). The PP's auto-gain and the log bands cover the quiet signals |
| KISS FFT fixed point (per-stage ÷2 with rounding, `C_FIXDIV`) | the same scaling, rounded | floored here: the accumulator floors for free, and rounding would cost an op per chain; the error budget below is met anyway |

**The arithmetic** (`jviz.c` is the reference; `gen_viz.py`'s docstring has the kernel's view):
- **Window:** a periodic Hann, Q15 scaled to a peak of **23,000** (`JVIZ_HANN_PEAK`), as a table in split order. `x = (pcm × h) >> 16` (one `vmulhd`) keeps `|x[2k] + i x[2k+1]| < 2^14`, so no add overflows 16 bits. That leaves 0.7 % for rounding.
- **Layout:** m = n/2 complex points as rows × 16 lanes (8 × 16 for 256 samples, 16 × 16 for 512). Element `16 r + l` is at row r, lane l. Real parts are at VRF x 0 and imaginary parts at x 32 of the same rows, in four 16-row blocks: B0 data, B1 transposed result, B2 twiddles then partners, B3 temporaries.
- **Step 1, down the rows** (lane-parallel; radix-2 DIF):
  - Each stage is REP h `vsub`/`vadd`/`vasr` over whole rows. Then each b row is multiplied by its scalar twiddle `W_16^e`: `e = 0` is a shift, `e = 4` (−i) a swap and a negation, and anything else **four exact products a component**.
  - Each product is `vmul … x15` (its low half, zero-extended) plus `vmulhd … acc` (its floored high half). The pairs sum `dr·wr − di·wi` exactly in 32 bits, and the D is the high half: **one floor** of the exact sum, which is `(d × w) / 2` with Q15 twiddles.
  - The last stage (h = 1) is unscaled, into B1 in natural order.
- **Step 2:** every element is multiplied by `W_m^(n1·k2)` (Q15, a table), which supplies the last stage's halving. This is a 16-iteration loop with A at `+r4`.
- **Step 3, along the lanes:** the same stages over **V16 columns**, the last stage into B1's columns in natural order. Then the partners: B2 is B1 with its columns reversed, which gives `Z[m − k]` for every row but 0, and B3 is B2 rotated one column (row 0's partner). That is 32 single moves and 4 stepped moves a component. The pairing needs a lane reversal, which the VRF can only do as column moves.
- **The split and the power, a row at a time:**
  - `S, D, T, U` (sums and differences of Z and its partner).
  - `Xr = (S·2^15 + Wr·T + Wi·D) / 2^16`, `Xi = (U·2^15 + Wi·T − Wr·D) / 2^16`, each one exact 32-bit sum floored once. `S·2^15` goes into the accumulator as `vshl #15 x15` (the low half) plus `vasr #1 acc` (the high half).
  - `|X|^2 = Xr² + Xi²` exactly: its low half written by the second `x15` op and its high half by the last `acc` op.
  - The power's halves go to memory (two 16-row stores, `jv_store16`). The C unpacks them (`jviz_raw_power`): bin `k = k2 + rows·k1` is at row k2, lane k1.
- **Scaling:** every stage halves, so the output is `X / m`. `|X| < 2^15`, and `|X|^2` fits 32 bits unsigned.

**Exactness** (`bcmfw/tests/viz_test.py`) [sim, mac]:
- The kernel's raw output equals the C reference's (`jviz_raw_ref`, built by vc2cc into the same program), every bin, on **110 signals**: sines (on and between bins; full scale, −12 dB and −41 dB), chirps, Gaussian and uniform noise, silence, DC, full-scale squares, a clipped sine (×4), Nyquist, and impulses (including at the window's zeros). Both sizes are covered.
- The C built for the Mac (ctypes) equals the vc2sim build on all of them.
- **Mutations** (`gen_viz.py --mutate`), each caught:
  - `sign`: a twiddle's imaginary part with the wrong sign (step 3) changes 34 of 38 signals;
  - `scale`: one stage's halving dropped (step 1) changes 37 of 38;
  - `lane`: the partner block's column 3 taken from the wrong column changes 32 of 38.

**Precision against a double-precision FFT** of the same windowed input, `X / m` [mac]:

| Class | n = 256 SNR | n = 512 SNR | Worst bin error (units) |
|---|---|---|---|
| sines (all levels) | 50.3 dB | 47.5 dB | 5.3 |
| chirps | 52.4 dB | 49.4 dB | 5.8 |
| noise | 48.5 dB | 45.8 dB | 7.7 |
| clipping (squares, ×4 sine, DC, Nyquist) | 57.5 dB | 54.7 dB | 5.7 |

- **What matters for bars:** the mean error power per bin is about 2 units² at worst, against a full-scale sine's peak bin at about 3.3 × 10^7. The floor is **72 dB** below full scale at both sizes.
- **Where it gets coarse:** a −41 dB sine reads 11–14 dB SNR, because its bins sit near the floor. At a 60 dB display range that is the bottom of the bars.
- The floors' bias (floor rather than round) costs about 1 dB. Rounding would cost an op per chain.

## 4. Bands

- `SPECTRUM` takes nb ≤ 32 band edges (bin indices). Band b is the **largest** power among bins `e[b] … e[b+1] − 1`, which keeps the bars crisp (the styles doc's choice for narrow bands).
- Its level is `jviz_l2`: `16·floor(log2 P)` plus a 4-bit mantissa's table value (`round(16·log2(1 + f/16))`). That is 0–511, a step of 0.19 dB, and integers only.
- The PP maps levels to bar heights with its auto-gain, so `SPECTRUM` replies raw `l2` values (two to a word) and takes no gain or floor.
- 1/3-octave edges from about 50 Hz are the PP's to choose. At 512 samples a bin is 86 Hz, so the low bands are one bin wide.
- The styles doc's 1024-sample window would need m = 512 = 32 × 16: two blocks of data, which would leave the VRF too tight for this layout's partner blocks [estimate]. It's open until the bass bars are judged on the device.

## 5. The bars

`jviz_bars_draw` (`BARS`):
- n ≤ 32 bars of `barw` pixels, `gap` apart, centred in the rect.
- Bar i is `height[i]` rows from the bottom. Row t is coloured `bot + (top − bot)·f/256` per channel, with `f = t·256/h` (one division a job), so the gradient spans the rect's height and a short bar shows only its bottom.
- `J_BARS_BG` fills the rect first. `J_BARS_ROUND` insets each bar's top row by a pixel at both ends (the 7G's soft caps). `J_BARS_CAP` draws `cap_h` rows of `cap` from `peak[i]` down.
- `J_BARS_FLIP` (`0x8`, offered when `J_VPU_FLIP` is set; §12.5) draws the same frame upside down: rect row r lands on row h − 1 − r. Mirror's reflection.
- Checked three ways:
  - against a Python model of this definition (61 frames, every flag, odd widths, heights past h; the same 61 again flipped) [mac];
  - through the image's ring against the Mac build, pixel for pixel (`jobs_image_test.py`'s viz row) [sim];
  - through the host model (`jmtest`).
- **Smoothing, peaks and decay are the caller's** (the PP), as the styles doc's pipeline has them. `BARS` draws one frame.
- **Cost in C:** the v1 frame (24 bars of 10 px with 3 px gaps, 320 × 150, background, caps, rounded tops) is 121k instructions, 124k cycles: **4.59 ms at 27 MHz, 0.83 ms at 150 MHz** [sim]. The background goes through the existing FILL hook (`jv_rows`); the bars are a per-row colour table and word-pair spans in C. That is now the fallback (§5.1).

### 5.1 The bars on the vector unit (`jv_bars16`) [sim]

*Branch `p19-viz-bars`, 2026-10-25.* `BARS` runs on the vector unit when its start-up check agrees (`J_VPU_BARS`), **pixel for pixel `jviz_bars_draw`**. The kernel is `jv_bars16` (`bcmfw/video/gen_viz.py` → `viz_kernels.S`); the C driver is `viz_bars` (`bcmfw/video/viz_vpu.c`), jref's new bars hook (`jref.h`, `jviz.h` `jviz_bars_fn`).

**The definition, per pixel.** The cap's colour if its row lies in its column's cap rows; else the gradient's colour of its row if the row is at or below its column's first bar row (the bar's top, one row lower on a rounded bar's two edge columns); else `bg` (`J_BARS_BG`, inside the rect); else the pixel is kept. That equals `jviz_bars_draw` exactly, because its bars never share a pixel and its caps are drawn last. So every pixel depends only on where it is, and a block can be drawn twice with the same result.

**The layout.**
- The frame is cut into 16-row groups, top down. When `h` isn't a multiple of 16, the last group moves up to end on the rect's last row and overlaps the one before; drawing those rows twice gives the same pixels.
- In each group, the kernel walks the 16-pixel column blocks of the surface, from the block that holds the rect's first pixel. The lanes outside the rect are kept, so a rect at any `x` needs no special case.
- The C does the geometry once a frame:
  - per column (lane): its first bar row and its cap's first and last rows, with `0x4000` meaning none;
  - per block: the least first bar row, the groups (as bits) that hold a bar's top, the groups that hold cap rows, and the background's lane mask.
  - About 8k instructions for the v1 frame.

**Per group** (`jv_bars16`, once a group):
1. The 16 rows' colours on the vector unit: per channel `c0 + ((c1 − c0)·f >> 8)` (a `vmul` by a register, `vasr #8`, a `vadd`), packed with `vmul #2048` and `#32`. `f` per row comes from the C (`t·inv >> 16`, `jviz_bars_draw`'s).
2. The colours and the row indices broadcast down the V16 columns of one row each, one REP-16 move each: GC and RI, where row k holds row k's value on every lane.

**Per block:**
- **X**, one of four cases:
  - `bg` on every lane: no load. If the block before was `bg` alone, X already holds it and the move is skipped too.
  - The block loaded, then `bg` on the mask's lanes (`vbitplanes`, `vmov … ifnz`).
  - The block loaded, so the pixels where nothing is drawn are kept.
  - Nothing to draw: the block is skipped.
- **The bars.** If a bar's top falls in this group: `m = (RI − first) >> 15` (−1 above the bar, 0 in it), then `X = GC + (GC − X)·m`. That is select by multiply, as `jv_key16` does: five REP-16 ops. Otherwise, the lanes whose bar covers the whole group take GC: `vsub … setf` on `first − (rr + 1)`, then `vadd … ifn`, one REP-1 op and one REP-16 op.
- **The caps,** in the groups that hold cap rows: `m = min(RI − cap first, cap last − RI) >> 15`, then `X = cap + (cap − X)·m`. Seven ops.
- **The store:** `jv_store16`, the single block-0 `vst16`s (the device-checked store).

**Where it runs.** On rects of 16 to 256 rows, at most 21 blocks wide (320 pixels at any phase), whose surface rows are 32-byte aligned with a pitch of whole blocks. Any other frame goes to the C, which also reports the errors.

**Exactness** (`viz_test.py`) [sim, mac]:
- **128 frames,** every pixel of the surface identical to `jviz_bars_draw` in vc2sim and to the Python model:
  - the v1 frame, aligned and at x 7;
  - 120 random frames over all eight flag combinations: bar widths 1–12 (odd and even) and gaps 0–5, rects at any `x` (partial blocks at both edges), heights 16–256 (one group to sixteen, overlapping last groups), bar heights 0, 1, 2, full and past the rect, peaks over their bars, at and above the rect's top, and over bars of 0, random gradients;
  - 6 frames the kernel declines (15 and 1 rows, 257 rows, a pitch not of whole blocks, 22 blocks wide, bars wider than the rect), which the C draws.
- **Through the images** (`jobs_image_test.py`, the viz and overlay rows): BARS through the ring equals the Mac's `jviz_bars_draw`, with `jv_bars16` running on every frame it takes. With the checks built to fail (`BCMFW_VIZ_FFT_FAIL`, which now fails both), it equals the Mac on the C, and the overlay's frames equal the `BCMFW_VIZ` image's, case for case.
- **Mutations** (`gen_viz.py --mutate`), each caught by the frames *and* refused by the start-up check:
  - `mask`: the select's mask 1, not −1, for a kept pixel: 31 of 32 frames differ;
  - `grad`: the gradient rows of the group below: 31 of 32;
  - `cap`: a pixel in either cap bound is capped: 27 of 32.

**The start-up check** (`viz_bars_init`): five frames on a 32 × 18 surface, against `jviz_bars_draw`, every pixel.
- The frames: a rect at x 3 (two partial blocks) with every flag; the same without the background, so lanes are kept; the full width (whole blocks, no load); then the first two again with `J_BARS_FLIP` (the flipped addresses, loaded and stored).
- What they cover: two overlapping groups; heights 0, 1 and full; peaks above the rect, at its top and over a bar of 0; rounded tops; a two-row cap.
- Cost: 56.5k instructions, about 60k cycles, 2.2 ms at 27 MHz, once per load (33k before the two flipped frames). It runs in the overlay's `init` beside the FFT's, and in `BCMFW_VIZ`'s `jobs_init`. Any difference leaves `J_VPU_BARS` clear and the C bars in place.

**Cost** [sim, v1 frame: 24 bars, 320 × 150, every flag]:

| | Instructions (vector) | Cycles | 27 MHz | 150 MHz |
|---|---|---|---|---|
| `BARS` job, C with the vector FILL (before) | 121,032 (6,000) | 124,032 | 4.59 ms | 0.83 ms |
| `BARS` job, vector (`BCMFW_VIZ`; the overlay 1.36 ms) | 23,809 (4,419) | 36,954 | **1.37 ms** | **0.25 ms** |
| `viz_bars` alone | 22,466 (4,414) | 36,088 | 1.34 ms | 0.24 ms |
| `viz_bars` alone, no background | 20,838 (3,430) | 33,848 | 1.25 ms | 0.23 ms |
| `jviz_bars_draw` alone (C, no FILL hook) | 103,647 | 103,647 | 3.84 ms | 0.69 ms |

- Where the cycles go (with the background): the kernel about 27k, of which the stores (`jv_store16`, 16 single stores a block, about 200 blocks) take about 8k; the C geometry about 8k.
- The REP-16 `vst16` with a stride (bench-p19.md "The REP-16 vector store", not yet adopted) would take about 5k off.
- The original estimate was about 1.5 ms. Two things beat it: the predicated path for groups whose bars cover them, and skipping the move of a `bg` block the block before left in X.

## 6. Two ops, not one "spectrum and bars" op

- **The PCM has to cross anyway.** Only the PP has the audio, and the BCM is off in dark playback (P19 D-3). The visualizer runs only while Now Playing is lit, so the BCM is already up for the display. A slot rewritten each frame (1 KB) is the cheapest path: one port write burst, no new transport.
- **The state belongs on the PP.** Peaks, decay, auto-gain and beat must survive an epoch change or a restart, and only the PP survives both. The other v1 styles need the levels too (Mirror's bars, Cover's bass, Aura's loudness). So the levels must come back to the PP, which a combined op would hide.
- **Fallbacks stay independent.** An image might offer `BARS` without `SPECTRUM` (or the FFT's start-up check might fail): the PP runs whichever half is missing on its own CPU, with the same result.
- **The cost of splitting** is one reply round trip a frame, tens of microseconds on the ring [estimate]. It is well inside a 33 ms frame.

`SPECTRUM` replies with the levels: `bcmjob_spectrum()` in `bcmjob.h` submits it and waits (implemented in the host model). `BARS` has no reply: it goes through `bcmjob_submit()`.

## 7. Costs and frame budgets (30 fps: 33.3 ms a frame)

[sim; cycles per isa-vector.md's S7 timing: a vector op of REP n about n + 0.5 cycles, scalar ops 1 cycle as a floor]

| Work | Instructions (vector) | Cycles | 27 MHz | 150 MHz |
|---|---|---|---|---|
| `jv_fft256` alone | 786 (659) | 1,354 | 50 µs | 9 µs |
| `jv_fft512` alone | 1,262 (1,071) | 2,188 | 81 µs | 15 µs |
| unpacking the powers (C), 256 / 512 | 1,418 / 2,826 | same | 53 / 105 µs | 9 / 19 µs |
| **`SPECTRUM` job, 256, vector** (ring to reply, 12 bands) | 4,782 (659) | 5,350 | **0.20 ms** | 0.04 ms |
| **`SPECTRUM` job, 512, vector** (24 bands) | 9,039 (1,071) | 9,966 | **0.37 ms** | 0.07 ms |
| `SPECTRUM` job, 512, C (the check failed) | 112,390 | 112,390 | 4.16 ms | 0.75 ms |
| the C reference FFT alone, 256 / 512 | 49,140 / 106,015 | | 1.8 / 3.9 ms | 0.33 / 0.71 ms |
| `BARS`, the v1 frame, C with the vector FILL (before §5.1; the fallback) | 121,032 (6,000) | 124,032 | 4.59 ms | 0.83 ms |
| **`BARS`, the v1 frame, vector** (§5.1) | 23,809 (4,419) | 36,954 | **1.37 ms** | 0.25 ms |
| **A visualizer frame's compute** (512-sample `SPECTRUM` + `BARS`, both vector) | | about 47k | **1.74 ms (5 %)** | 0.31 ms (1 %) |
| the same with the C bars (before §5.1) | | about 134k | 5.0 ms (15 %) | 0.9 ms (3 %) |

- **The rest of the frame:** `PRESENT` of a 320 × 150 rect runs by DMA in about 9 ms [estimate: 14.3 ms for a full frame, silicon S7, scaled by rows], beside the compute.
- **At 27 MHz the visualizer fits with about 70 % of the frame free** even with the C bars; with the vector bars the compute is 5 % of the frame.
- The C fallbacks on the BCM (4.2 ms for the FFT, 4.6 ms for the bars) would still fit.
- **On the PP:** 106k ARM7 instructions for the 512 FFT, about 1.3 ms boosted [estimate]. That matches the styles doc's 3–5 % CPU estimate.

## 8. Room

**What it needs** (`build/bcmfw/viz/bcmfw.map`, by unit):

| Part | Bytes |
|---|---|
| Vector code (`jv_fft256`/`512` 2,844; `jv_fft_cols` 2,632, the unrolled column stages and partner moves; the loops 394) | 5,870 |
| C (`jviz.c`'s FFT reference, bands, bars; `jref_viz.c`'s `SPECTRUM` and `BARS`; `viz_vpu.c`'s hook and check) | about 3,600 |
| Tables (window, four-step and split twiddles at both sizes: 4,608; `W_16`, `log16`, descriptors) | 4,712 |
| `.bss` (the raw output buffer, the check's samples; 32-byte aligned) | 2,048 |
| **Total** | **about 16.2 KB** |

**Where it doesn't fit resident:**
- **The merged image** (video + jobs) has 20 B below its arena at `0x35400` (`jobs.md` §10).
- **The display-only image** (`BCMFW_HOSTPROTO + BCMFW_JOBS`) had 832 B below `DECODE`'s work area at `0xC200`.

### 8.1 Built: an overlay of the display-only image [sim]

The visualizer's code, tables and `.bss` run from the display-only image's **overlay region, `0x13B000–0x13FFFF`** (`config.h` `BCMFW_JOBS_OVL`; `jobs.md` §5, "Overlays"; `memory-contract.md` §2).

- **The region is free.** In the display-only layout, `J_SURF_BACK` ends at `0x12B000` and `J_CLS_THUMB` runs `0x12B000–0x13AFFF`. No AAC tables are parked in that image (only the video images park them, at `0x134000`, and they offer no overlay). The framebuffer (`0xE0000–0x1057FF`), the stack, the rings and the job block are all below. `build.py` adds the region to the layout's overlap check.
- **The link.** vc2cc's fixed-base link (`compiler.md`, "Fixed-base links"): `build.py build_overlay()` links `OVL_SOURCES` at the region's base against the image's link map. Two names resolve into the image (`OVL_IMPORTS`: `jref_f16w` and the vector store `jv_store16`); libgcc is linked into the overlay itself. The overlay is compiled with the image's flags (`JREF_HOOKS`, `JREF_EXT`), so it shares `struct jref` and calls the image's FILL hook through it. The code is the `BCMFW_VIZ` image's, unchanged: `jviz.c`, `jref_viz.c` (`SPECTRUM` and `BARS`, now one file for every runner), `viz_vpu.c` and the vector FFT. Only `video/viz_ovl.c` is new: the header and two entries.
- **Size** (`viz-ovl.map`):

  | Part | Bytes |
  |---|---|
  | Part | Before the vector bars | With them (§5.1) |
  |---|---|---|
  | header (`struct jref_ovl`) | 28 | 28 |
  | text (vector kernels and C) | 9,804 | 11,964 (`jv_bars16` 506; `viz_bars` 1,086 and its helpers 498; its start-up check 282) |
  | rodata (the tables) | 4,728 | 4,732 |
  | **loaded** (`viz-ovl.bin`, the PP's upload) | **14,560** | **16,724** |
  | `.bss` (the raw output, the check's samples) | 2,080, to `0x13F100` | 2,060, to `0x13F960` |
  | spare in the region | 3,840 | **1,696** (`build.py` keeps at least 1 KB) |

  The vector bars keep their per-frame tables (2.4 KB of lanes, 336 B of block bounds, one group's 64-byte rows) and the start-up check's two 32 × 18 surfaces on the stack, not in the region.

- **The start-up checks** (`viz_vpu.c`, in the overlay's `init` and in `BCMFW_VIZ`'s `jobs_init`):
  - The FFT's runs both sizes on a full-scale square and on noise, against the C. It sets **`J_VPU_FFT`** and installs the vector FFT only if they agree; otherwise `SPECTRUM` runs the C, exact either way.
  - The bars' (§5.1) sets **`J_VPU_BARS`** the same way; otherwise `BARS` runs the C.
  - `BCMFW_VIZ_FFT_FAIL=1` (tests only) makes both fail.
- **The stub in the image**, `J_OP_OVERLAY` (`jobs_bcm.c`): **288 B**. The display-only image's image+bss end moves from `0xbec0` to `0xbfe0`, 544 B below the decode work area. No pool moves. It checks the length, the F16w the PP sends (as `COMMIT` does), the header's ABI word and load end, runs the overlay's `init` (the FFT's start-up check, `J_VPU_FFT`) and sets the `J_OPS` bits of what it offers (19 and 20). `RESET` and a mode change drop them. In C it is about three times the ~100 B first guessed. The address constants (each a 48-bit `mov`) and the `JREF_EXT` call path in jref cost most of it. A hand-written stub might reach ~150 B; that is a later trim if the image needs the room.
- **The merged image is unchanged** (byte-identical; `mkvideo.py --check`). It has no room for the stub, and its parked AAC tables sit inside where the region would be.
- **Costs** [sim, 27 MHz]: `SPECTRUM` and `BARS` cost what they cost in `BCMFW_VIZ` (0.37 ms for 512 samples; 1.36 ms for the v1 frame on the vector unit, §5.1, 4.3 ms in C before). The load costs 0.59 M instructions on the BCM (about 22 ms; almost all of it is the two start-up checks, the FFT's about 0.5 M and the bars' 33k, the F16w about 25k), plus the upload: 16.3 KB through the port, about 3 ms of the PP's CPU at 30 MHz or 1.5 ms by engine 2 (`data-paths.md`, cells 9 and 14).
- **Tests:**
  - `jobs_image_test.py` "overlay", 220 cases. Before any load, `SPECTRUM` is refused (`HP_E_UNKNOWN`).
    - Refused loads leave no ops: a flipped bit (`J_E_SUM`), another ABI version, a patched ABI word, a truncated image (`HP_E_VERSION`), under the header or over the region (`HP_E_INVAL`).
    - Loaded, `SPECTRUM` (256 and 512) and `BARS` equal `jviz.c` on the Mac and the `BCMFW_VIZ` image's replies and frames, case for case. That holds with the FFT check passing and built to fail.
    - `RESET` drops the ops, and a reload restores them.
  - `decode_image_test.py` "DECODE beside the overlay": a DECODE with the overlay loaded gives the golden pixels and leaves the region's bytes and `J_OPS` alone, and a `SPECTRUM` after it still equals the Mac.
  - `forms_test.py` counts the overlay's forms: the same 20 unchecked vector sites as `BCMFW_VIZ`.
  - `vc2cc_tests.py` `t_fixed_base` tests the link itself.

### 8.2 The other homes (not built)

1. **The `BCMFW_VIZ` image, warm-swapped in** (built, but no PP drives the swap).
   - It is the display-only image's layout and jobs, with **`SPECTRUM` and `BARS` in place of `DECODE`** (`config.h`, `build.py` `VIZ_SOURCES`): 38,808 B with the vector bars (36,548 B before), with image and `.bss` ending at `0xad20`, every pool where the display-only image keeps it (`build.py` checks the layout). `DECODE`'s work area sits unused.
   - The shell parks the display-only image and boots this one when the visualizer view opens, then swaps back when it closes (`HP_OP_PARK`; `warm-swap.md`).
   - A swap is a new epoch, so the PP re-uploads its slots (the Now Playing cover). While it runs, covers decode on the PP's twin.
   - The overlay does better: no swap, the slots survive, and `DECODE` stays resident.
2. **Inside the merged image.** Only after cuts elsewhere: 16 KB is about 7 % of it, and the arena can't move.

### 8.3 The PP's recipe: loading the overlay (for the shell session)

**When.** On opening the visualizer view, while Now Playing is lit. The BCM is up for the display then, and the visualizer runs only while Now Playing is lit (§6).
- Not at boot. 14.5 KB of upload and a 20 ms check are wasted if the user never opens the view.
- Once loaded, keep it: the region has no other user. Closing the view doesn't unload it, so reopening costs nothing until the epoch moves.

**Steps** (the job driver, `bcmjob-*`):
1. Check `J_OPS` bit 21, then `J_OVL`: its base must equal the overlay's header word 2, and its end must reach the header's `.bss` end (word 4). Check the header's magic (word 0). Any mismatch: the CPU path (below).
2. Make sure no overlay op is queued: at once after an epoch change (its ops are gone already), or after a `FENCE` retires.
3. Write `viz-ovl.bin` at the base through the port, as a slot upload goes: the CPU's copy, or engine 2 in pieces.
4. Send `OVERLAY words, A, B` with `J_TAG_REPLY`. A and B are the file's F16w over halfwords; the driver can store them with the file at build time.
5. On `HP_OK`, read `J_OPS`. Bits 19 and 20 now say `SPECTRUM` and `BARS` are there. Log `J_VPU` once (bit `J_VPU_FFT`: the FFT is on the vector unit; clear, the BCM runs the C, the same levels).
6. On any error, log it (`J_E_SUM`: retry the upload once; `HP_E_VERSION`: the PP's overlay and the BCM's image come from different builds, never retry) and take the CPU path for this epoch.

**After an epoch change** (`J_EV_EPOCH`, `J_EPOCH` moved, a BCM restart, the warm swap back from the video image): treat the overlay as gone. That is the same moment the slots are re-uploaded. If the visualizer is showing, load it again with them; if not, load it lazily on the next open. Between the epoch change and the reload, frames take the CPU path.

**Shipping.** The overlay is valid only against the image build it was linked with: its ABI word hashes the image's addresses. The PP must carry both from one `bcmfw` build: the image (`jobs_display`, `bcm-devtest-images.c` today) and its `viz-ovl.bin`, like the merged image's `.bin` and header (`mkvideo.py`). Not done yet: nothing ships the overlay, and the ABI check refuses a mismatch instead of running it.

**The fallback is the CPU** (`jobs.md` §7; §2 above): `jviz_power_ref`, `jviz_bands` and `jviz_bars_draw`, the same levels and pixels. It runs when the image has no bit 21 (the merged image, an older image), a load is refused, or before the first load. The PP's FFT costs about 1.3 ms boosted for 512 samples [estimate], and the bars are drawn into the PP's frame and pushed as any other rect.

**Trims, if room is short** [estimate]:
- The 256-sample twiddle tables are rows 0, 2, 4 … of the 512-sample ones (`W_128^(l k2) = W_256^(l·2k2)`; the split's `W_256^k = W_512^(2k)`). Loading them at a 64-byte stride saves 1 KB.
- Looping `jv_fft_cols`' stage groups saves about 1.5 KB.
- Leaving the test-only `jviz_fft_ref` out saves 128 B.

### 8.4 Shipping rule: a build check, not a convention

The overlay is linked against the **display-only image** (its imports, `jref_f16w` and `jv_store16`, resolve into that image's map), not the merged `bcmvideo.bin`. Before the overlay ships (the orchestrator, 2026-10-24), the packaging step (`make zip`, through `mkvideo.py`/`mkdevtest.py`) must **refuse** to package a `viz-ovl.bin` whose header ABI word differs from the `__vc2_abi` of the display-only image built into the same firmware. A mismatch fails the build, and nothing ships with the wrong pairing.

**Built (`p19-viz-device`, §12):** `bcmfw/mkdevtest.py` links `viz-ovl.bin` against `jobs_display`'s map, writes it to `bcmfw/video/viz-ovl.bin` and a record `bcmdt_viz_ovl` beside the embedded image (size, FNV-1a, ABI word, the byte offset of the image's own copy of the word, the image's FNV-1a); `--check` covers the file. `tools/buildzip.pl` runs `bcmfw/ovlpair.py` (no VC toolchain) and copies the file only in devtest shell zips; any disagreement (file and record, record and image, the header's ABI word and the image's) fails `make zip`. A release embeds no display-only image and ships no overlay. Host test: `tests/ovlpair` (11 cases).

### 8.5 The release line: the merged image's own overlay, swapped with DECODE

**The gap (budget review, 2026-10-26):** the release line runs the merged image (`bcmvideo-jobs.bin`, FNV `23693ff4`), which has no SPECTRUM or BARS and no display-only image beside it, so its zip shipped no overlay and the visualizer always took the CPU path (12–20 % CPU unboosted). The owner's rule for 0.8.0: the visualizer draws on the BCM.

**Built (`p19-relviz`), the merged image unchanged (still `23693ff4`; `dec-ovl.bin` still `aa89be9d`):**
- The merged image's overlay mechanism is generic. `jobs_bcm.c`'s loader takes any file whose header carries the image's ABI word (`__vc2_abi` over `DEC_OVL_IMPORTS`) and region, and offers whatever ops its `init` returns. So `mkvideo.py` links `viz_ovl.c` a second time against the merged image's map and exports (`build.py build_overlay(kind='viz')`; it needs only `jv_store16` of them), at the same region (0x12B000). This gives `bcmfw/video/viz-ovl-m.bin`: 19,380 B, FNV `a83ba1b5`, 17,476 B of the 36 KB region spare. Its record is `BCMVIDEO_VIZOVL_*` in `apps/video/bcmvideo_image.h`.
- **Pairing:** `ovlpair.py --viz-m` checks it, and `make zip` ships it beside the merged image in every build with `HAVE_BCMFW_MERGED_VIDEO` (release or devtest). A mismatch refuses the zip. The display-only `viz-ovl.bin` still ships only in devtest zips.
- **vc2sim** (`jobs_image_test.py --only ovl_merged`, 76 cases): the file loads into the merged image, and SPECTRUM (256 and 512) and BARS (flip included) equal `jviz.c` on the Mac on the vector FFT and bars. The line ops are offered. The display-only overlay is refused. The swap below works in both directions, and RESET drops both.

**One region, two overlays: the swap.**
- A load replaces what the region held (the BCM drops the other's ops at OVERLAY). The PP's driver now marks the evicted instance due again at once (`bcmjob-core.c bjc_ovl_begin`, "bcmjob overlay evict k=K by=L"), so the two alternate without spending tries.
- **Who loads when (the shell, `art_bcm.c`):** both files are read on the art thread (no file read on the UI thread, the freeze audit's rule), and each piece is put by the job owner's pass (`bcmjob_ovl_read/put(kind, …)`, one handshake for both kinds).
  - DECODE's overlay loads for a decode (`art_bcm_ready_ovl`).
  - The visualizer's loads when the visualizer asks (`art_bcm_viz_ask()` from the frame, served from the art thread's pump, `art_bcm_viz_serve()`).
  - Both run on the art thread, so a DECODE load, its decode and a visualizer reload are never interleaved. A visualizer load that meets a DECODE in flight is refused BUSY with no try spent.
- **The open view owns the region (0.8.0, sitting 4c):** on the device (build `fa3d8edb58`) the visualizer was bursty, "very fast moments, then slow": the art pass rebuilding every album's Now Playing box asked for DECODE's overlay at each decode, evicting the visualizer's, which held its frame, reloaded and was evicted again. Now, while the view is open (`viz_start()` to `viz_stop()`, `art_bcm_viz_view()`), the art side neither loads DECODE's overlay nor runs a DECODE: the background passes wait (`artcache.c blocked()`: "art build wait viz"), a decode marked for the BCM (a box, np-sharp's panel, a resident's re-decode, a chapter's box) is deferred to its next chance and never handed to the PP, one in flight is cancelled, and the UI pass never takes a DECODE load's first piece. The close logs the deferrals once ("art chip work paused: visualizer open (n deferred)") and wakes the art thread; DECODE's overlay loads when a decode next needs it. A parked CPU decode (Now Playing lit) now serves the view's overlay ask from its park, so the view no longer waits out `VIZ_WAIT_MAX` behind it. Script: `ipodui-p19-vizown`.
- **Before the view opens, a decode still wins:** an evicted visualizer's view holds its last frame (no draw, no CPU analysis) while its reload is owed, and the art thread reloads it (three 8 KB pieces, about 50–110 ms in the simulator). It does the same while the BCM restarts under a running view.
- **No UI pass waits on a load (0.8.0, sitting 4c: the Now Playing counter stalled while a cover went to the BCM):** the begin's FENCE (only with jobs in flight) and the end's OVERLAY reply (the BCM's sums, `init` and start-up checks, about 30 ms) are posted, and the call answers `BJC_OVL_AGAIN` / `BCMJOB_OVL_AGAIN` until they are done, within the old bounds and with the old lines (`bcmjob-core.h`). The pass puts the same piece again at its next passes, the art thread waking them; a withdrawn piece's reply is still looked for (`bcmjob_ovl_poll()`). Each put is timed ("art bcm overlay pass k= off= us= rc=") and the pass's longest BCM work a minute is logged ("bcmjob pass max us=N what=W": the overlay puts, the park, drop, resident and FREE, and the owner's DECODE pass). Script: `ipodui-p19-ovlpass`; host test `tests/bcmjob` (t_overlay: a slow OVERLAY and FENCE, an abandoned end, the timeout).
- **Bound:** past `VIZ_WAIT_MAX` (3 s) the CPU draws, logged loudly ("CPU FALLBACK why=wait"), until the overlay is back.
- **Other nets, all loud:** no overlay for the running image (`why=noop`), and a refused load (twice in an epoch: "FAULT bcm viz overlay not loaded", then `why=lost`).
- TONE's refusal is unchanged and covers both kinds: `HP_OP_TONE`'s buffer lies inside the region, so the image refuses it (`HP_E_BUSY`) while any overlay's ops are live (`hostproto.c`, `jobs_ovl_live()`).

**Simulator:** the job model takes its capabilities from the image, as on silicon. SPECTRUM, BARS and the line ops are offered only once the visualizer's overlay is loaded in this epoch ("bcm_viz ovl on|off|refuse", on by default, beside "bcm_decode ovl"). It models the shared region's eviction and the BUSY refusal during a decode. Scripts:
- `ipodui-p19-relviz`: the BCM path through the overlay, a cut and its reload, and no CPU fallback.
- `ipodui-p19-relviz-refuse`: the file missing gives the loud CPU net.

Both pass on `sim-ui-video` (the gate) and on the release-config simulator (`build/sim-rel-bcm`).

**On silicon:** the merged image needs no re-verify (it is byte-identical). The overlay itself has its first silicon run on the next sitting. Its kernels are `viz-ovl.bin`'s (§9's forms), and its start-up checks keep the C on the BCM if a vector form misbehaves.

## 9. Forms and silicon

The image's vector forms are **unchecked on silicon**: `forms_test.py` counts them, as for the video images.
- Apple kinds in vc2sim: everything from the IDCT (`x15`/`vmulhd acc` chains, V16 B stepping, `vld16` past row 15), plus:
  - V16 A, B and D stepping in `vadd`/`vsub`/`vasr` (REP 2/4/8);
  - `vrsub #0`;
  - `vshl #15 x15` and `vasr #1 acc`;
  - immediates as B in `x15`/`acc` chains.
- One non-Apple kind (20 sites): `vmulhd` into a **V16 D** with an immediate and `acc` (step 3's twiddles).
- **The vector bars** (§5.1, `gen_viz.py`'s docstring lists them) add one non-Apple kind at one site (`forms_test.py`: the overlay now has 2 kinds, 21 sites): `vmin` 16-bit with a stepped 16-bit B at REP 16, the decoder's deblocking kind. `vmin`/`vmax` 16-bit have run on silicon since S4.
  - **Device-checked:** the loads, the stores (`jv_store16`), MULR, ALUX and `vbitplanes`.
  - **Unchecked on silicon:** `vadd` D16 by a register (ADRG's, first silicon run pending) and at REP 16; `vmul` D16 by an immediate; `vasr #8` and `#15`; the broadcast (`vadd` into a V16 D stepping a column per REP from an unstepped H16 A); `vmov` D16 from a register at REP 16 under `ifnz`; `vsub` into a discarded D with `setf`, then `vadd … ifn` at REP 16; `vsub`/`vrsub` with an unstepped H16 B row; `vmul`, `vrsub` and `vmin` with a stepped 16-bit B at REP 16.
  - Bench row **BARV** (`bench-p19.md`, informational) runs one block through every one of them in the kernel's order.
- **The start-up checks** (§8) keep the C if any of them misbehaves.
- **Bench rows to add with the next device session:**
  - the column-stage `vmulhd … acc` into V16;
  - `vshl #15 x15` + `vasr #1 acc` against `x · 32768`;
  - REP-8 V16 stepping in `vadd`.

## 10. Open

**Shell side built (branch `viz`, 2026-10-25):** `apps/ipodui/viz.{c,h}` and `viz_core.{c,h}` take the tap's window by play position, send it to `SPECTRUM` through a 512 x 1 `J_CLS_STRIP` slot and draw with `BARS` into `J_SURF_FRONT` at x 0, y 30, 320 x 160 (every row 32-byte aligned), `PRESENT`, `FENCE`, the rect owned (`bcmjob_owned_set`) while the view shows; the CPU path (jviz.c on the PP, 256 samples, 15 fps, unboosted) whenever the ops are missing, logged as `viz CPU FALLBACK why=...`. The device path is built too (§12): the driver's `SPECTRUM` reply and `OVERLAY` load, so a device running the display-only image takes the BCM path; the merged image (no `OVERLAY`) logs `why=noop`. Mirror's reflection is drawn on the BCM too since `p19-viz-flip` (§12.5); Scope, Radial and Spectrogram since `p19-viz-ops` (§13: `POLYLINE`, `SEGMENTS`, `PALROW`); Cover and Aura are listed and hidden.


- **Not built on the PP:** the tap, the decimation and downmix, the split-order write and the frame pacing; the levels-to-heights mapping, smoothing, peaks and auto-gain; the CPU fallback's wiring. These are the shell session's (`bcmjob_spectrum()` exists only in the host model).
- **Built since (§12):** the overlay's load (§8.3) and shipping `viz-ovl.bin` with its image (§8.4). Still not built: an `OVERLAY` in the host model (`jmodel.c` offers `SPECTRUM` and `BARS` directly; `tests/bcmjob`'s chip has one).
- The stub in assembly (§8.1: 288 B in C, perhaps ~150 B by hand), if the display-only image needs the room.
- The 1024-sample window (§4). Radix-4 (§3).
- The vector bars' stores: the REP-16 `vst16` with a stride, once its bench row passes, would take about 5k cycles (0.2 ms at 27 MHz) off the v1 frame (§5.1).
- ~~Whether `BARS` should grow Mirror's reflection~~: settled by `J_BARS_FLIP` (§12.5).

## 11. The PP's PCM tap (`p19-pcm-tap`, a proposal for the shell session)

*Built and host-tested, 2026-10-24: `apps/ipodui/viz_tap{,_core}.{c,h}`, `HAVE_VIZ_TAP` (the shell build). No screen opens it yet. This covers the first item of §10, the tap with its downmix, decimation and split order, plus the CPU fallback's cost. Tags as above, plus [qemu] for a dynamic ARM instruction count (`tests/viztap/insn.sh`).*

### 11.1 Where the samples come from

| Candidate | Context | What it sees | Lead on the ear | Ring it needs | Taken? |
|---|---|---|---|---|---|
| **The mixer's output** (`pcm_mixer.c` `mixer_buffer_callback()`, the existing `mixer_set_tap()` hook) | the PCM callback: **FIQ** on the PP5022, at the start of each DMA frame | the frame the DAC gets next: after the DSP, the volume ramps and fades, and any click or beep mixed in | **0 to 1 mixer frame**: 0.02–5.79 ms, mean 2.90 ms at 44.1 kHz [mac, `tests/viztap` lag model], plus the I2S FIFO (16 entries, ≤ 0.36 ms) and the DAC's filter | n + 2 frames: **1,024 frames, 4 KB** | **yes** |
| The playback channel's buffer hook (`mixer_channel_set_buffer_hook()`, what Rockbox's FFT and oscilloscope plugins use) | the same FIQ, once per pcmbuf chunk (up to 8 KB, 46 ms) | pcmbuf's chunks before the mixer: no clicks, no amplitude | up to a chunk (46 ms), and the plugin has to track the play position inside it | a whole chunk at least | no: coarser, and over the 33 ms frame |
| **(b) after the DSP**, tagged with a pcmbuf position and served by the play position | the codec thread, per DSP output block | every decoded sample | none, once mapped; untagged it leads by pcmbuf's fill: **2,000–3,000 ms** (watermark 2 s, buffer 3 s) [mac, model] | the whole pcmbuf depth to serve the play position: about 3 s × 44.1 k × 2 B = **265 KB** mono | no: 65× the memory, a copy of every decoded sample (not only while the view shows), the mapping depends on pcmbuf's chunk descriptors, and crossfade rewrites queued audio in place, so tagged samples can differ from what plays |

The mixer tap is the cheapest exact place: it sits where the bytes go to the DAC, so sync needs no position bookkeeping at all, and it already exists (the developer build's audio-path test).

**When the tap sees a frame** (`pcm_mixer.c`): the mixer double-buffers. At the start of frame k's DMA (the FIQ, status `STARTED`) it mixes frame k + 1, which plays when k ends. So every frame written before the current one is playing or played: the writer publishes `head` = the frames written before this one (the end of the frame at the DAC), then copies the new frame behind it. A read takes the n frames ending at `head`, so the window ends inside the frame now playing (§11.6).

**Decimation:** none on this device. `viz_tap_dec()` is 1 up to 64 kHz, 2 up to 128 kHz, 4 above. The design wants about 44.1 kHz (512 samples = 11.6 ms, 86 Hz bins, bands to 16 kHz), and the WM8758 path tops out at 48 kHz (94 Hz bins), so decimating 48 kHz to 24 kHz would only lose the top bands. At 2 or 4 (other hardware), consecutive frames are averaged (a box filter, cheap and enough for bars). Rates under 32 kHz are not upsampled: the PP picks band edges from `f->rate`, as §4 says it should.

**Downmix:** `(L + R) / 2`, floored, on the reader's side. The ring keeps stereo, so the FIQ does one copy and the XY scope (styles doc #7) can read L and R later.

### 11.2 The API (`viz_tap.h`)

- `size_t viz_tap_bytes(unsigned n)`: 4,143 B on the device for n = 512 or 256 (the ring of 1,024 frames, its 20-byte header on its own cache line, alignment). The simulator's mixer frames are 1,024 frames long, so 16,431 B there.
- `int viz_tap_open(unsigned n, void *mem, size_t bytes)`: n is 256 or 512. Refuses `VIZ_TAP_E_BOOK` while a book plays, `E_SIZE`, `E_NOMEM`, `E_BUSY`. CPU only.
- `unsigned viz_tap_read(int16_t *out, unsigned opts, struct viz_tap_frame *f)`: the newest n samples in **split order** (`out[k] = x[2k]`, `out[n/2 + k] = x[2k+1]`), ready for `SPECTRUM`'s slot or `jviz_power_ref()`. Returns flags, 0 for a fresh window of sound:
  - `STALE`: `head` hasn't moved since the last read (paused, stopped, or read twice inside a mixer frame);
  - `SILENT`: every sample within ±2 (digital silence or 1-LSB dither). After a stop the mixer pads 3 s of zeros, which reads as `SILENT`, then the head stops, which reads as `STALE`;
  - `UNDERRUN`: fewer than n frames since open; the older part is zeros;
  - `TORN`: the writer overran the window twice during the copy; zeros. A reader that keeps up never sees it: the ring holds one spare frame for a write that lands mid-read, and a second overrun is retried once;
  - `BOOK` (zeros), `CLOSED`.
  - `f`: `head`, `rate` (the samples' rate), `peak` (the window's largest |x|) and `shift`.
- **Gain:** the design puts auto-gain on the levels (§2: on the PP, on `SPECTRUM`'s reply, with a slow decay), not on the samples, so the tap applies none. It offers one exact precision aid instead: `VIZ_RD_NORM` shifts the window up by whole bits until its peak is at least half scale (at most 8 bits). A shift of s bits multiplies the power by 4^s, which is exactly 32·s `jviz_l2` units, so the caller subtracts `32 * f->shift` from each band level and the bars are unchanged. Quiet passages keep more of the FFT's 16 bits: §3's −41 dB sine at 11–14 dB SNR becomes a −5 dB one. In the test, a −30 dBFS 3 kHz sine is shifted 5 bits, and its band reads 397 − 160 = 237 against 237 unshifted.
- `void viz_tap_close(void)`: unhooks under the PCM lock, then frees. CPU only.

### 11.3 Memory: never a core_alloc beside playback

The brief asked for a `core_alloc` only while open. But the visualizer opens while a track plays, and any `core_alloc` beside a full audio buffer stops and restarts playback to shrink it (`playback.c` `shrink_callback`; 340 ms measured, `docs/hardware.md` "Library view pool"). So:
- **the caller passes the memory** (`mem`, at least `viz_tap_bytes(n)`, pinned, untouched until close). The shell can take it from a block it already holds while Now Playing is up, or from the screen arena (`ui_arena_alloc()`, freed when Now Playing leaves). With the arena, re-sum `tests/arena`'s deepest chains through Now Playing (4,143 B of 9,216);
- **with `mem` NULL**, the tap follows the screen arena's fallback rule (`screen_stack.h`): a `core_alloc` only when audio is stopped or `core_available()` covers it. Otherwise `VIZ_TAP_E_NOMEM`, and never a shrink (host-tested).

There is nothing static beyond 17 B of `.bss` (the ring pointer in `.ncbss`, the handle, n, the last head, the track check).

### 11.4 Cores and the hand-off (for 19-9)

- **The writer** is the PCM callback, the CPU's FIQ, today and after 19-9.
- **The ring and its header are reached through the uncached alias** (`UNCACHED_ADDR`, `0x1000_0000 + x`), on both sides. So a reader on the COP (the UI after 19-9) sees the FIQ's stores with no cache operation per frame (`xcore.h` rule 3; `data-paths.md` R7). Open cleans and drops the CPU's cache once, so no dirty line of the memory is evicted over the ring; close does the same, so no stale line outlives it.
- **One writer per word:** `head` is stored only by the FIQ, as one 32-bit store; the reader only loads it. The writer's count `w` is the FIQ's own. No lock, and nothing on the audio side ever waits.
- **The hook pointer:** `pcm_mixer.c`'s `mixer_tap` moved to `XCORE_BSS` (`.ncbss`), so an opener's store is the copy the FIQ loads. `mixer_set_tap()` now takes the PCM lock, so once `mixer_set_tap(NULL)` returns on the CPU, no callback is inside the old tap (the FIQ is masked on that core while it runs; the simulator's audio thread mixes holding the lock). That is what lets close free the memory at once, without the tick's wait the audio-path test used.
- **Open and close are CPU-only** (`XCORE_ASSERT_CPU`): buflib is CPU-only, and the PCM lock masks only the caller's core. After 19-9 the COP's UI asks the CPU to open and close (whatever 19-9 uses for buflib), and reads directly.
- **The book check** reads `np_pb_track()` (one load) and, on a change, `np_pb_is_book()` over `audio_current_track()` and the RAM index. That is the same data the Now Playing snapshot reads, so it moves with the snapshot's own 19-9 solution.

### 11.5 Audiobooks

`np_pb_is_book()` (new in `np_playback.c`, split out of `np_pb_snapshot()` with no change in behaviour) is the one definition: the parser's `is_audiobook` (M4B brand, chapters), or the scan's audiobook or podcast class in the RAM index (`TAGCACHE_UI_AUDIOBOOK`, `TAGCACHE_UI_PODCAST`), and never a video. It is exactly `np_track.is_audiobook`, which selects the audiobook Now Playing cycle, so the cycle and the tap can't disagree. It uses no codec or extension guess. The tap refuses to open on it, and if the queue moves on to a book while it's open (`np_pb_track()` changes), reads return zeros with `BOOK` until music plays again.

### 11.6 Costs

**Closed: zero.** `mixer_tap` is NULL. The callback does one load and a branch not taken, which the developer build already did; the shell's release build gains that one load per mixer frame (172 per second). No memory is held.

**Resident** (`fw-ui`, `--ipodui`, against `p19` `bf3b6c7fdf`): **+52 B text, +8 B data, −64 B bss** (alignment) as built, because nothing calls the tap yet and `--gc-sections` drops it. Wired by a screen it adds the two objects: `viz_tap.o` 1,072 B text and 17 B bss, `viz_tap_core.o` 1,080 B text, so **about 2.2 KB of text and 17 B of bss**. Stock build: text/data/bss identical (1,144,956 / 12,268 / 629,036).

**Open** [qemu: dynamic ARM instructions, `-Os` as the firmware builds; cycles at CPI 1.2–2.0, as `scripts/risk`]:

| Work | Instructions | Per second | 80 MHz | 30 MHz |
|---|---|---|---|---|
| **The FIQ's copy**, one 256-frame mixer frame (`viz_ring_write()` + the firmware's IRAM `memcpy`, `ldm`/`stm`) | **182** | 172 frames: 31k instructions, plus 44k uncached SDRAM word stores [estimate: 2–6 extra cycles a store] | **0.15–0.4 %** | 0.4–1.0 % |
| **A read**, 512 samples (downmix, split, peak) | **8,795** (7,010 at `-O2`) | 30 reads: 264k, plus 15k uncached word loads [estimate: 4–8 extra cycles] | **0.5–0.8 %** | 1.3–2.2 % |
| **The CPU fallback** (`jviz_power_ref()` + `jviz_bands()`), 512 samples, 24 bands | **101,237** | 30 frames: 3.0 M | **4.5–7.6 %** (1.5–2.5 ms of a 33.3 ms frame) | 12–20 % |
| the same, 256 samples, 12 bands | 46,521 | 1.4 M | 2.1–3.5 % | 5.6–9.3 % |

- The FIQ's share is bounded and short: about 0.7–1.9k cycles per 5.8 ms frame, stores included (9–24 µs at 80 MHz, 24–63 µs at 30 MHz) [estimate], beside the mixing the same callback already does. The next DMA frame is started before the callback runs, so the audio never waits on it, and nothing on the audio side waits on the reader.
- The fallback's 101k instructions agree with §7's 106k vc2sim count and the styles doc's 100k-cycle estimate. At 80 MHz it is 4.5–7.6 %, the top of the design's 3–5 %. That fits the lit-only, boosted visualizer: `docs/hardware.md` rule 2 allows boosting while something animates, and here the boost is held only while the view shows. Unboosted at 30 MHz, 512 samples cost up to 20 %; use 256 samples, or render at 15 fps, if the BCM's `SPECTRUM` is missing on battery. With the BCM's ops the PP's share is the copy and the read, about 1 %.
- **Device bench rows** for the next session: the uncached store and load cost per word (unmeasured, the estimates above), and the FIQ's time with the tap open (`USEC_TIMER` around `mixer_buffer_callback()`, as `DEVSTATS_PCM_WB` does).

### 11.7 Tests

- **Host** (`tests/viztap/run.sh`, in `scripts/test.sh`; 46 checks):
  - ring sizing and the decimation rule;
  - wraparound with uneven frame sizes over 400 writes, every read checked sample by sample, at n = 512 and 256 with dec 1, 512 with dec 2, and 256 with dec 4;
  - a reader with no writer (`UNDERRUN | STALE | SILENT`, zeros);
  - a writer 40 frames ahead (only the newest window, no mix);
  - torn reads: one mid-read frame is intact, three once are retried, three twice give `TORN` and zeros;
  - dither as silence; the normalising shift and its exact undo through `jviz_power_ref()`/`jviz_bands()` (the CPU path), with the sine's bin found where it belongs;
  - **the lag:** the mixer's double buffer modelled frame by frame, with 514 reads at about 30 fps at random phases. The window's end leads the DAC by 0.02–5.79 ms, within one mixer frame and far inside the 33 ms requirement. The post-DSP alternative modelled from pcmbuf's watermark leads by 2–3 s;
  - `viz_tap.c` over stubs: the size and memory refusals, no `core_alloc` while playing without free memory, open and close hook and unhook, a book and a podcast refused, a fresh window from mixer frames, stale with no new frame, a book reached mid-open (zeros) and back to music.
- **Simulator** (`tests/ui/ipodui-p19-viztap.script`, the `viz_tap` script command in `sim_cmd_audio.c`): while the mini library's Concert A plays, 48 reads at 512 give 46 fresh, 0 torn, peak 3,895, at 44.1 kHz, and 256 works too. The mini audiobook (M4B) is refused at open. After `audio_stop` there are no fresh reads (silent, then stale).
- **ARM counts:** `tests/viztap/insn.sh [-Os|-O2]` (qemu with the risk tool's plugin; a measurement, not a gate).

## 12. As built on the device (`p19-viz-device`, 2026-10-25)

*Simulation and host tests only; nothing here has run on silicon.*

### 12.1 The driver
- **`SPECTRUM`'s reply** (`bcmjob-core.c` `bcmjob_spectrum`): the record, a reply awaited by tag (the driver's reply boxes now keep 17 words: status and 32 levels), the levels unpacked two to a word. Deadline `J_T_FENCE_BASE + J_T_SPECTRUM`. A failure the shell sees holds that frame (its window is 512 samples and the PP keeps only the 256-sample tables) and pins the CPU path until the epoch moves.
- **The overlay load:** `data-paths.md` R14 and `jobs.md` §5, "Overlays". The shell asks once a frame through `bcmjob_viz_ops(scratch)`; the driver loads at most once an epoch (twice after a torn upload), streaming the file through the strip's 28 KB past Now Playing's parts, so nothing is allocated beside playback. `op_ok` now admits ops up to `0x3F`.
- **Log lines:** `bcmjob overlay load bytes=16980 at=0013b000 epoch=N`, `bcmjob overlay ok ops=... vpu=0x.. fft=vector|c bars=vector|c`, `bcmjob overlay refused why=nofile|read|file|region|image|wire`, `... refused st=N why=sum|version|inval`, `bcmjob overlay gone op=0x33|0x34`; the shell's `viz CPU FALLBACK why=noop|nojobs|spectrum|lost|bars`.
- **Host test** (`tests/bcmjob` `t_overlay`, against a chip built as the display-only image, `JREF_EXT`): the load and its bytes, `J_OPS` read back, `SPECTRUM` 512 and 256 equal to `jviz.c`, `BARS` run; re-upload after a restart, a `J_EV_EPOCH`, a `RESET` and an early `HP_E_UNKNOWN` (never counted); refusals for a torn upload (one retry), another image (nothing written), another file, a wrong FNV-1a, `HP_E_VERSION`, a header past the region; a fence before the write.

### 12.2 The FFT under a saturating accumulator
Silicon's accumulator saturates past ±2^31 (quirks #31, #32); `jviz.c` wraps. `gen_viz.py` `chain()` now sums step 2's two products, the split's three terms and the power's two squares as two chains joined in wrapping 16-bit lanes (the IDCT's `jv_dot8` form): exact for any 16-bit operands. Only step 2 can pass 2^31 in one chain, and only with tables past |W| ≤ 1 (y = (−32768, −32768) times −32768 twice is 2^31); with the shipped tables no input reaches it, so the kernel before was already exact on real audio. The split is bounded by step 3's halving (±(2^31 − 32,768): a corner search and 2 M random points agree) and the power by the same (no Z and W give X = (−32768, −32768)); they are split anyway, so no bound rests on another step's scaling.
- **Exact** against the C on 110 signals under every rule that fits silicon (`v_acc_sat` sat32, read16, hi16 × `v_a16_reg_x` dbl16, near32) and the old wrapping model, with strict alignment trapping.
- **The start-up check** (`viz_vpu_init`) adds the worst case at both sizes: every sample 32767 and every table word −32768 (a crafted `struct jviz_tabs` on the stack, checked against the new `jviz_raw_ref_t`). The kernel passes it under every rule; the kernel before (`--mutate acc1`, byte for byte) and `tw1` (step 2 alone) fail it under each saturating rule and pass only under the wrapping model.
- **Cost:** 256 samples 1,354 → 1,474 cycles, 512 2,188 → 2,428; the `SPECTRUM` job (512, 24 bands) 10,126 cycles, 0.38 ms at 27 MHz. The overlay: **16,980 B** loaded (+256), 1,440 B of the region spare, FNV-1a `70f858ce`, ABI `431c2106` (against the image batch's `jobs_display`); its load 0.77 M instructions, about 28 ms at 27 MHz.

### 12.3 The PP's tables
`JVIZ_N256_ONLY` (set by `apps/ipodui/viz_jviz.c` on the device): the PP's CPU path runs 256 samples, so the 512-sample window and twiddle tables leave its build: **−3,072 B** of firmware text (`viz_jviz.o` 8,244 → 5,104 B). The host model, the tests and the BCM keep both sizes.

### 12.4 Full screen (the owner's layout, 2026-10-25: 320 × 240 at (0, 0))
Nothing caps `BARS` below 240 rows: the vector bars take 16–256 rows (`VIZ_BARS_ROWS_MAX`), at most 21 blocks wide; heights and peaks are bytes; `jref_bars` checks only the surface; the driver checks no rect; nothing in the overlay or the kernel is sized by rows. `viz_test` adds the full-screen frame (pixel for pixel against `jviz_bars_draw` and the model).
- **`BARS`, 320 × 240, 24 bars, every flag** [sim]: 44,422 cycles on the vector unit (1.65 ms at 27 MHz), 163,519 in C (6.06 ms).
- **A frame** at the shell's 30 fps (33.3 ms): `SPECTRUM` 10.1k + `BARS` 44.4k ≈ 55k cycles = **2.0 ms at 27 MHz**, plus `PRESENT` of the full frame by DMA, 14.3 ms bound by the LCD bus [silicon S7]: 16.3 ms even run back to back, **about half the frame**. With both C fallbacks, 10.2 + 14.3 = 24.5 ms, still inside. So the **27 MHz band holds with margin**; every higher band (46 … 150 MHz) only shortens the compute (0.37 ms at 150 MHz).

### 12.5 Mirror on the BCM: `J_BARS_FLIP` (built, `p19-viz-flip`, 2026-10-25) [sim]
**What Mirror is** (the shell's, `viz.c` `draw_cpu`, `viz_core.c`): a vertical reflection. `viz_mirror_split(H)` gives the reflection rh = 3H/10 rows and the bars bh = H − rh − 1, with one background row between. The bars are the Bars style's frame drawn into the top bh rows, heights and peaks scaled by bh/H. The reflection is a second frame, rh rows, drawn under the gap with each bar's (scaled) height clamped to rh, colours faded 5/8 toward the background (`viz_fade565`, one fixed blend per colour, no per-row fade), no caps, no rounding, then turned upside down. At 320 × 240: bars 167 rows, gap at row 167, reflection 72 rows.

**The flag:** `J_BARS_FLIP` (`0x8`) draws `BARS`' frame upside down, rect row r at row h − 1 − r. The fade needs nothing new (the PP sends the faded colours), so the reflection is one `BARS` with `J_BARS_BG | J_BARS_FLIP`, and Mirror on the BCM is `BARS` (the bars), `FILL` (the gap row), `BARS` (the reflection, flipped), `PRESENT`, `FENCE`.
- **By addressing only, no second pass.** In C (`jviz_bars_draw`) the row base and step are negated (base at row h − 1, step −pitch); the background fill is the same either way. On the vector unit (`viz_vpu.c` `viz_bars`) the kernel `jv_bars16` is **unchanged**: each 16-row group of rect rows rr … rr + 15 is handed the surface block at rows h − 16 − rr … h − 1 − rr (top down, every row 32-byte aligned, the same positive stride: Apple's row addressing), with its row k told it is rect row rr + 15 − k (that row's gradient `f` and index). The kernel's masks are per row and its group bounds cover the same set of rows, so it computes the flipped block exactly. No new vector form, no negative stride.
- **Cost** [sim]: the full-screen frame flipped is 30,277 instructions, ~45.3k cycles, against 30,472 and ~45.5k upright (the same 6,205 vector instructions): **no extra cycles**. The full-screen Mirror (320 × 167 bars + 320 × 72 flipped) is 24,663 + 11,914 instructions, ~55.1k cycles, 2.0 ms at 27 MHz. The overlay grows **100 B** (`viz-ovl.bin` 16,980 → 17,080: the C's two branches, the group loop's row mapping, two more start-up frames); the start-up check grows to 56.5k instructions (+0.9 ms at 27 MHz, once per load). On the PP (`arm-none-eabi-size`, the developer build against `p19-viz-device`): `viz_jviz.o` (`jviz_bars_draw`) +32 B, `bcmjob-core.o` (`bcmjob_bars_flip()`, the `J_VPU` word kept) +68 B, `viz.o` +641 B (the Mirror's BCM path: three records a frame, the shared Mirror geometry, the fallback line; the CPU path lost its row swap). The merged image `bcmvideo-jobs.bin` is unchanged (FNV 80bc8611, `mkvideo.py --check`).
- **Capability:** an overlay before the flag would ignore the bit and draw upright, so the overlay (and `BCMFW_VIZ`'s `jobs_init`) sets **`J_VPU_FLIP`** (`0x40`) in `J_VPU`, whichever path draws `BARS`; the driver reads it with the ops (`bcmjob overlay ok … flip`) and the shell asks **`bcmjob_bars_flip()`** (`bcmjob_pp.h`): `BARS` offered and `J_VPU_FLIP` set. The overlay's ABI word does not change (431c2106): it is the image's interface (imports, `J_OVL_ABI`), which the frozen display-only image checks, and the flag is an argument bit inside the overlay's own op.
- **The shell** (`viz.c`): Mirror goes to the BCM when `bcmjob_bars_flip()`; with `BARS` offered but no flip it logs `viz FALLBACK mirror on CPU why=noflip` once and draws on the CPU. The CPU path itself now draws the reflection with `JVIZ_BARS_FLIP` too, so the old row swap (and its 640 B stack row) is gone.
- **Tests:** `viz_test` (122 Mac frames against the model, half flipped; 135 vc2sim frames with flipped full-screen, the Mirror pair at 320 × 240 and random flags over all four bits; the start-up check; the flip's cost), `jobs_image_test` (the Mirror pair and a flipped bare frame through the image's ring, against the Mac; `J_VPU_FLIP` read back), `jmtest` (the flipped frame is the upright one upside down), `tests/bcmjob` (`bcmjob_bars_flip()` after a load, gone after a restart, false for an overlay before the flag). The simulator script command `bcm_flip 0|1` stands in an overlay before the flag.

## 13. Scope, Radial and Spectrogram on the BCM: `POLYLINE`, `SEGMENTS`, `PALROW` (built, `p19-viz-ops`, 2026-10-26) [sim]

All five v1 styles now draw on the VC. The three new ops sit in the visualizer's overlay beside `SPECTRUM` and `BARS`, and the host model offers them too. They are numbered `0x36`–`0x38`, at `J_OPS` bits 22–24. The wire formats are in `jobs.h` and `jobs.md` §5.

### 13.1 What each op draws

The definitions are in `jviz.h` and are plain C on every runner: `jviz_poly_draw`, `jviz_segs_draw` and `jviz_pal_draw`.

| Op | Style | Message | What it draws |
|---|---|---|---|
| `POLYLINE` | Scope | 8 words | n points (2–1024) spread across the rect (w ≤ 320). Point i's row comes from element off + i·step of a slot: int16 samples, centred, `gain` Q16; or bytes (`J_POLY_U8`). With `half`, the elements are read as a split-order window, so `SPECTRUM`'s slot serves as it stands and nothing more is uploaded. Each column covers the rows between its point and the next column's, so the line is connected. A width 1–3 line is that path under a square brush. |
| `SEGMENTS` | Radial | 8 words + n lengths + n peaks (bytes); 48 segments = 32 words | n ≤ 48 rays from (cx, cy). Ray i is at angle index (a0 + i·astep) mod 48: index 0 points up and steps go clockwise, using a built-in Q14 sine table. Each ray runs from r0 out to min(len, lmax) under a width-square brush, with the bars' gradient c0 → c1 over lmax. Optionally: a dot ring at rring (drawn first), a cap dot at r0 + min(peak, lmax) (drawn last), and round dots. |
| `PALROW` | Spectrogram | 5 words + nb levels (bytes) | nb ≤ 96 cells across the rect's rows. Cell b's colour is `lut[level b]`, where the LUT is a slot of 256 RGB565 entries uploaded once an epoch. One frame is `SCROLL`, `PALROW`, `PRESENT`, with no row upload. |

**Why plain C, not vector:**
- Line work is per pixel, data-dependent and short-run, which suits the scalar core.
- No new vector form is introduced, so nothing waits on a silicon bench row.
- The BCM runs exactly the code the PP's fallback runs.

**The start-up check** (`viz_vpu.c` `viz_draw_init`, run in the overlay's `init`):
- Each op draws a fixed 40 × 30 scene. The scene covers clipping at every edge, every width, both sources, split order, more and fewer points than columns, round dots, the ring and the caps.
- The pixels' FNV-1a must equal `JVIZ_CHK_POLY`, `JVIZ_CHK_SEGS` or `JVIZ_CHK_PAL`, the sums the Mac's build of the same C gives. `viz_test` checks that the constants still hold.
- A mismatch (a miscompile, or a core that differs) leaves that op's bit clear. The PP then draws that style on its CPU, logged, and the overlay stays loaded for the others.
- The check costs 157k instructions, 5.8 ms at 27 MHz, once per load.

### 13.2 Costs per frame at 27 MHz [sim]

Through the image's job ring (`jobs_image_test`), the shell's frames:

| Style | Jobs | Compute | With `PRESENT` (14.3 ms) |
|---|---|---|---|
| Scope 320 × 240 | `FILL`, `POLYLINE` 3 px, `POLYLINE` 2 px | about 5 ms (a sine; up to 7.2 ms on noise, `viz_test`) | about 20 ms |
| Radial 320 × 240 | `FILL`, `SEGMENTS` (48 rays, caps, ring) | 7.7–9.5 ms | about 24 ms |
| Spectrogram | `SCROLL`, `PALROW` (24 cells, 320 × 3) | `PALROW` 0.2 ms | about 16 ms |

Clipped to 198 rows under the volume overlay, the costs are about the same: Scope 3.2–7.5 ms, Radial 8.7–10 ms.

- Every style fits a 30 fps frame (33.3 ms) at 27 MHz. Higher clock bands only shorten the compute.
- **The fixed cost per job is small:** a 1 × 1 `FILL` takes 403 instructions from ring to reply, 0.015 ms. So a frame of a few jobs needs no batching.
- The host model charges 2 ms per draw job (`JM_COST_DRAW`). That is a deadline-scaled allowance, not the BCM's cost. A 24-`FILL` frame timed out in the model only because of it.

### 13.3 Room in the overlay

**The size now:**
- 19,380 B loaded, with 1,092 B of the 20 KB region spare (`build.py` keeps 1,024 B).
- FNV-1a `36fc02ba`. The ABI word stays `431c2106`, since the image's imports are unchanged.
- The display-only image (`f069a36a`) and the merged image (`80bc8611`) are unchanged.

**What made the room:**
- **The FFT's `.bss` moved to the stack** (−2 KB): the raw output and the check's samples. The deepest path, the FFT's start-up check, takes about 6 KB of the 16 KB stack.
- **The 256-sample tables are derived, not loaded** (−1.5 KB; `JVIZ_OVL`, `jviz_derive256`). The relations are exact: w₂₅₆(t) = w₅₁₂(2t) for the window's evens and odds, and every other row of the 512-sample twiddles. `SPECTRUM` at 256 samples copies them into its stack first, which raises it from 0.20 ms to 0.43 ms. The shell's BCM path uses 512 samples.
- The test-only references `jviz_fft_ref` and `jviz_raw_ref` are left out.
- Smaller code: the scene check is data, not stores, and `surf()` and `dot()` are out of line.

`POLYLINE`, `SEGMENTS` and `PALROW` together cost about 2.9 KB of code and 0.3 KB of data.

**Margins and trims:**
- The 1,024 B margin is kept.
- The next trims, if the region needs them, are in §8.3: looping `jv_fft_cols` saves about 1.5 KB.

### 13.4 The PP's API (`firmware/export/bcmjob_pp.h`, `bcmjob-viz.c`)

- `bcmjob_have_polyline()`, `bcmjob_have_segments()` and `bcmjob_have_palrow()` report whether each op is offered now. Call `bcmjob_viz_ops()` first, as for `SPECTRUM`, so the overlay is loaded.
- `bcmjob_viz_polyline(&struct bcmjob_poly)`, `bcmjob_viz_segments(&struct bcmjob_segs)` and `bcmjob_viz_palrow(&struct bcmjob_palrow)` queue one record each, with no reply. They return as `bcmjob_submit()` does; `SEGMENTS` over 48 and `PALROW` over 96 return `FAILED`.
- The same file serves the device and the simulator.
- The driver takes the line ops from an overlay load but never reloads for them, and its ok line ends `lines=PSR`. Its loader state is one `struct ovl`, ready for a second region.

### 13.5 The shell (`apps/ipodui/viz.c`)

**Scope:**
- The jobs are `FILL`, a 3-pixel halo `POLYLINE` and a 2-pixel core `POLYLINE`, reading `SPECTRUM`'s slot in split order: off = the rising zero crossing, n/2 points, gain 2·`SCOPE_A`.
- The PP waits for the last frame's fence before it rewrites the slot.
- A frame with no new samples keeps the last line for up to 3 frames (at 30 fps, half the reads fall between two of the mixer's chunks). After that, or when silent, the line is flat.
- Behaviour change: under the volume overlay the rect is the clip's 198 rows, and the line centres in them.

**Radial:**
- `FILL` and one `SEGMENTS`: 48 rays, with the 24 bands down the right and mirrored down the left, r0 34, up to 78 long.
- Caps sit 3 px clear of the ray's tip, and there is a dim dot ring at radius 27.

**Spectrogram:**
- `SCROLL` and `PALROW`. The LUT is `heat(0..255)`, and a band's level is its heat as a byte.
- The `BLIT` path stays for an image without `PALROW`.

**The CPU fallbacks** call `jviz_poly_draw` and `jviz_segs_draw` with the same values, so a fallback frame equals a chip frame. The fallback is logged once as before (`CPU FALLBACK why=style`).

### 13.6 Tests

- **`viz_test`:** 118 frames, every pixel and status, across an independent Python model, the Mac's C and vc2sim's C. Cases include the full screen at 240 and 198 rows and random frames over every option and edge. Also checked: the sine table, the scene sums, `viz_draw_init` (7, and 0 when told to fail), and the costs.
- **`jobs_image_test` "overlay":** the shell's frames through the ring against the Mac, pixel for pixel, for the overlay and the `BCMFW_VIZ` image alike. It also checks the `J_OPS` bits (none when the check fails), the per-job cost and `SPECTRUM` at 256 samples with the derived tables.
- **`tests/bcmjob`:** the wrappers' frames equal the CPU's, and a failed `SEGMENTS` check leaves `POLYLINE` and `PALROW` offered.
- **`ipodui-p19-viz`:** no style falls back to the CPU, and ops `0x36`–`0x38` are sent.
