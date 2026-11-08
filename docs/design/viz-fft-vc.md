# The visualiser's FFT on the VideoCore: a kernel from proven forms

*#126, branch `p19-vizfft` (from `p19` at 93ce6f6e35), 2026-11-05. Docs and host code only: no `bcmfw/` source and no image changes; the merged image (`23693ff4`) stays frozen. Host model and tests: `tests/vizfft/`. Background: `docs/research/bcm2722/visualizer.md` (the first kernel, the overlay, the bars), `p19-visualizer.md` and `p19-visualizer-styles.md` (what the shell shows).*

*Tags: [silicon] measured on the owner's unit; [apple] Apple's code runs it on silicon; [apple-RE] read from Apple's code; [sim] vc2sim; [mac] a host run (`tests/vizfft`); [model] our vector-unit model in `tests/vizfft/vfft_vpu.c`; [build] a build artefact or map; [estimate]; [hypothesis].*

## 1. In short

- **Where things stand.** The visualiser's spectrum is already a BCM job: `SPECTRUM` (`0x33`) in the visualiser overlay `viz-ovl-m.bin`, loaded into the merged image's overlay region at `0x12B000`. Its vector FFT (`jv_fft512`) has never passed its start-up check on silicon. Sittings 4e and 4f read `J_VPU` 0x53: the overlay's `J_VPU_BARS` (0x10) and `J_VPU_FLIP` (0x40) were set, `J_VPU_FFT` (0x8) was clear (§2.2) [silicon]. So today each frame's FFT runs **in C on the BCM**, about 112k cycles (4.2 ms at 27 MHz) [sim]. The PP computes a spectrum only on its last-resort CPU path (256 samples, 15 fps).
- **The proposal: a new 512-sample kernel built only from forms silicon has run.** It is a four-step FFT on one 16 × 16 block, like today's kernel. It differs in two ways. The column stages become **one transpose and the same row stages again**: the transpose is bench GTHR's `vmov`, silicon-proven. The vector-operand accumulator chains become **separately floored `vmulhd` products**. All 26 of its forms are filed `silicon` or `apple` in `tools/bcmre/proven_forms.txt`, which the test checks [mac]. Today's kernel has 31 forms that silicon has never run.
- **Cost:** 771 vector instructions, **about 1,930 cycles: 71 µs at 27 MHz, 13 µs at 150 MHz** [model; cycle rule §7]. That is less than today's kernel (2,428 cycles [sim]). The whole `SPECTRUM` job should take about 7.2k cycles, **0.27 ms at 27 MHz** [estimate], against 4.2 ms for today's C fallback. At 30 fps the BCM's share for the spectrum falls from about 12.5 % to under 1 %.
- **Precision:** 44–51 dB SNR on test signals, an error floor 81–86 dB below full scale, and every bin within 7.4 units of a double-precision DFT [mac]. Today's arithmetic (one floor per sum) is 3.5 dB better. Both are far inside the bars' 48 dB display range (§5.3).
- **Size and bands:** unchanged. 512 samples at 44.1 kHz (86 Hz bins, an 11.6 ms window), and the shell's 24 log-spaced bands from 50 Hz to 16 kHz (§4).
- **Slot-in:** the kernel goes in the **visualiser overlay**, rebuilt with batch 5's image regeneration, which rebuilds `viz-ovl-m.bin` anyway. The merged image's sources don't change, and neither do the job protocol or `SPECTRUM`'s contract. Only the definition of the 512-sample FFT moves to the new arithmetic: the overlay's C fallback and its start-up check follow `tests/vizfft/vfft_ref.c` (§9).
- **Bench row VFFT** (§10), hands-off, in the overlay's load. It says whether today's failure was the kernel or the BCM's C, and bisects the new kernel by phase if it ever disagrees. It also checks the five forms that would make the kernel exact or smaller.
- **Provenance** (§6.7). Apple's code is a baseline that gives evidence of what a form does, not a limit on which forms we use. 16 of the proposal's 26 kinds are also Apple-used, and 10 are proven only on our silicon; none is unproven, and it uses none of today's suspect forms. Today's kernel has 46 forms. The five ranked suspects for its start-up failure each get a single-instruction probe in VF3, with the words expected under each reading.

## 2. Where things stand

### 2.1 The pipeline today

| Step | Where | What |
|---|---|---|
| Tap | PP, the PCM FIQ (`pcm_mixer.c` `mixer_set_tap`, `apps/ipodui/viz_tap.c`) | each mixer frame (256 stereo frames, about 172 a second) copied into a 1,024-frame ring through the uncached alias: 182 ARM instructions a mixer frame [qemu, `visualizer.md` §11.6] |
| Window read | PP, the UI thread, 30 times a second (`viz.c` `FPS_BCM`) | the newest 512 frames, downmixed `(L + R) / 2`, written in **split order** (evens, then odds): 8,795 ARM instructions [qemu] |
| Upload | PP → the BCM's SRAM through the host port | 1,024 B into a 512 × 1 `J_CLS_STRIP` slot (`viz.c` `spectrum_bcm`) |
| `SPECTRUM` | BCM, `viz-ovl-m.bin`'s `exec` (`bcmfw/video/viz_ovl.c`) | window, FFT, power, 24 bands as `jviz_l2` levels, replied (`bcmjob_spectrum`) |
| Smoothing, auto-gain, peaks | PP (`viz_core.c` `viz_dyn_step`) | state that must survive a BCM restart |
| `BARS` / `POLYLINE` / `SEGMENTS` / `PALROW`, `PRESENT` | BCM | the styles' pixels (BARS on the vector unit, `J_VPU_BARS`) |

The CPU path (`viz.c` `N_CPU` 256, 15 fps) runs only when the overlay is missing or held: `viz CPU FALLBACK why=noop|wait|lost|...`. It computes `jviz_power_ref` on 256 samples, 46,521 ARM instructions a frame [qemu]. When the overlay is up, the PP does no FFT work at all: its cost is the tap, the window read and the 1 KB upload.

### 2.2 On silicon

- **Sitting 4e:** with `viz-ovl-m.bin` loaded, the shell logged `bcmjob overlay ok … vpu=0x53` [silicon; `ipodui.log`, cited in `apps/ipodui/devbench.c` `bench_vpu_verdict` and commit f9faecfd2e]. 0x53 is the merged image's own 3 (`J_VPU_CHECKED | J_VPU_SHIFT`) plus the overlay's `J_VPU_BARS` (0x10) and `J_VPU_FLIP` (0x40).
- **Sitting 4f:** the jvpu step's overlay line read "FFT 0, BARS 1, FLIP 1" [silicon; owner's report, 2026-11-05].
- **What that means:** the overlay loads and runs on silicon, its code included. This answers part of open question #74. The vector bars agree with their C on silicon, but `viz_vpu_init` (both sizes; a square wave, noise and the saturating worst case, all against `jviz_raw_ref_t`) disagreed. So `exec` passes `0` as the FFT hook, and `SPECTRUM` runs `jviz_power_ref` in C on the BCM [silicon behaviour, `viz_ovl.c`].
- **Why it failed is unknown.** The check compares the kernel with the C, both computed on the BCM, and neither has a golden answer from the Mac. Either side could be wrong:
  - **The kernel** uses 31 forms that `proven_forms.txt` files as `guard`, never run on silicon. They are listed in §6.6: the V16 column stages, the accumulator chains with a vector B, the windowing `vmulhd` at REP 16, and the stepped H16 A at `+r`.
  - **The C** (`jviz.c`, compiled by vc2cc) runs about 106k scalar instructions a frame. The halfword-encoding audit cleared it ("encoding only", `docs/research/bcm2722/merged-image-batch.md` §3), but no silicon run has checked it against a golden either.
- Bench row VFFT (§10) settles which side was wrong. The design below doesn't depend on the answer: it replaces the unproven forms rather than waiting to learn which one failed.
- **The likelier cause is a misread instruction, not the method** (§6.7). In vc2sim, today's kernel passes its check under every alternative reading the simulator models that silicon hasn't refuted. It fails under five readings no one has measured. In rank order: `x15` with a vertical A operand (which neither Apple's code nor our silicon has ever run), a V16 A stepping across a REP, `acc` into a V16 D, an H16 D's `+r` y, and the REP 16 `vmulhd`. VF3's single-instruction probes say which reading silicon follows.

### 2.3 The overlay region

- The merged image's single overlay region is `0x12B000–0x133FFF` (36,864 B), shared by DECODE's overlay and the visualiser's [build; `data-paths.md` R14].
- `viz-ovl-m.bin` is 19,380 B, FNV `a83ba1b5` against `23693ff4`. Batch 5 (`p20-batch5`) regenerates it as `925d2eb7` [build].
- The open view owns the region: a decode waits while the visualiser shows (`visualizer.md` §8.5).

## 3. The data path

| Hop | Who moves it | Bytes a frame | Rate | Cost |
|---|---|---|---|---|
| PCM → tap ring | PP CPU (FIQ, `memcpy`, uncached) | 1 KB a mixer frame | 172/s | 0.15–0.4 % of the CPU at 80 MHz [qemu + estimate] |
| Ring → window, split order | PP (UI thread, `viz_tap_read`) | 1,024 B | 30/s | 8.8k ARM instructions [qemu] |
| Window → slot | PP CPU, host port writes (2-register `stm`; engine 2 for whole 64 B units) | 1,024 B | 30/s, 30 KB/s | about 0.18 ms of CPU at 30 MHz, 0.06 ms at 80 MHz (5.7 and 16 MB/s, [silicon S8]); about 0.5 % of a 30 MHz CPU |
| `SPECTRUM` record | PP → job ring | 15 words (2 + 13 for 24 bands' 25 edges) | 30/s | small |
| Slot → VRF | BCM vector loads (`vld16` REP 16, stride 32 B) | 1,024 B samples + 3,072 B tables | 30/s | in the kernel (§7) |
| Raw → levels | BCM scalar C (`vfft_bands`: the band's largest power, `jviz_l2`) | reads 2 × 185 halfwords | 30/s | part of the job's 7.2k cycles [estimate, §7] |
| Levels → PP | reply (status + 12 words) | 52 B | 30/s | one reply poll |

Notes:
- **The PCM has to cross.** Only the PP has the audio. The BCM's audio block isn't in the audio path, and the BCM is off in dark playback. The visualiser runs only while Now Playing is lit, so the BCM is already up for the display.
- **Split order is free on the PP** (it is only the index the read writes to), and it saves the vector unit a deinterleave it can't do cheaply. It is unchanged.
- **The slot is SRAM in the jobs arena.** The cache is off (the state the host leaves), so the host's port writes are seen directly [silicon: every image].
  - The PP commits the slot once (`J_COMMIT_NOCHECK`) and rewrites it between jobs. Its write and the job's read are ordered by the ring, as for every slot.
  - Open question #73's posted-write case concerns SDRAM, not this SRAM slot.
- **Nothing else crosses back:** only the 24 levels.

## 4. FFT size and bands

### 4.1 What the shell shows

- **Bands:** `VIZ_BANDS` = 24, edges from `viz_edges()` (`apps/ipodui/viz_core.c`), log-spaced from 50 Hz to min(16 kHz, rate / 2). Every band is at least one bin wide.
- **Window:** `N_BCM` = 512 samples at 30 fps on the BCM path; `N_CPU` = 256 at 15 fps on the CPU path.
- **The edges at 512 / 44.1 kHz** (bins of 86.13 Hz) [mac, `tests/vizfft` prints them]:

`1 2 3 4 5 6 7 8 9 10 11 12 13 14 16 21 26 34 42 55 68 88 110 142 185`

- **What that means:** the 14 bands below about 1.2 kHz are one bin each (86–1,206 Hz in 86 Hz steps). Above that the spacing is truly logarithmic, up to bin 185 (15.9 kHz).
- **Each band's level:** the largest power among its bins, as `jviz_l2` (16 log2 of the power, a 4-bit mantissa: 0.19 dB steps, 0–511).

### 4.2 Why 512 samples

| n | Bin | Window (latency) | VRF | Verdict |
|---|---|---|---|---|
| 256 | 172 Hz | 5.8 ms | half a block a component | the bass collapses: bands below 2.4 kHz would be single bins, and fewer than 24 bands fit (`viz_edges` clamps) |
| **512** | **86 Hz** | **11.6 ms** | **one 16 × 16 block a component** | **the shell's size today; one block per operand keeps every stage a whole-block op, with room for the partner, the tables and temporaries in the 64-row VRF** |
| 1024 | 43 Hz | 23.2 ms | two blocks a component (m = 512 = 32 × 16) | the data, partner, tables and temporaries need about 7 blocks of 4: a pass would spill the VRF to SRAM and back [estimate]; 2 KB a frame through the port |

- **Latency:** the frame is 33.3 ms, and the tap lags the DAC by 0–5.8 ms [mac, `visualizer.md` §11.1]. The 11.6 ms window adds half its length, about 6 ms, of perceived lag. 1024 would double that for a finer bass that bars with 86 Hz steps do not need.
- **If the bass bars are judged too coarse on the device,** the route is a 1024-point FFT run as two 512-point halves plus one radix-2 combine (a decimation in time across two kernel calls), not a larger single block. That is a later item, not this design.
- **Rates:** at 48 kHz the bins are 94 Hz and the same layout holds. Below 32 kHz, `viz_edges` takes fewer bands, unchanged. Above 64 kHz the tap decimates (`viz_tap_dec`).

## 5. The arithmetic: window and fixed point

Element j of a 256-point sequence sits at row j >> 4, lane j & 15. Every lane holds 16 bits. Adds and subtracts wrap. `vmulhd` gives the floored high half of a 16 × 16 product [silicon: MHDU]. The accumulator is 32 bits and saturates at ±2^31 [silicon: quirks #31, #32].

### 5.1 The definition (`tests/vizfft/vfft_ref.c`)

1. **Window:** `z[j] = (x[2j] · w[j]) >> 16 + i (x[2j+1] · w'[j]) >> 16`.
   - The periodic Hann table (in split order, Q15 scaled to a peak of **23,000**) keeps `|x_even + i x_odd| < 2^14`: the largest windowed sample is 32,767 × 23,000 / 65,536 = 11,500, and the complex magnitude is under 16,264.
   - So no add in the stages below overflows 16 bits.
2. **Step 1:** a 16-point radix-2 decimation in frequency down the rows, all 16 lanes at once.
   - Stages h = 8, 4, 2 compute `a' = (a + b) >> 1` and `b' = (a − b) · W_16^e / 2`.
   - The twiddle W_16^e is an immediate:
     - e = 0 is a shift;
     - e = 4 (−i) is a swap and a negation;
     - any other e is four products a component, summed exactly in the accumulator and floored once. The Q15 twiddle's floor by 2^16 is the halving.
   - The last stage (h = 1) writes `a ± b` unscaled into bit-reversed rows, so the block ends in natural order: row k2, lane n2.
3. **Step 2:** each element times the four-step twiddle `W_256^(n2 · k2)` (Q15, a table). The floor by 2^16 is step 1's last halving.
4. **The transpose:** row n2, lane k2.
5. **Step 3:** the same 16-point transform down the rows, its last stage halved. Z[k] then sits at row k >> 4, lane k & 15, which is natural order.
6. **The split:** with the partner `P[k] = Z[(256 − k) & 255]`, S = Zr + Pr, D = Zr − Pr, T = Zi + Pi, U = Zi − Pi:
   - `Xr = (S·2^15 + T·Wr + D·Wi) / 2^16`
   - `Xi = (U·2^15 + T·Wi − D·Wr) / 2^16`
   - W = W_512^k = e^(−2πik/512), Q15, natural order.
7. **Power:** `|X|² = Xr² + Xi²`, exact in 32 bits. The low half and the high half are stored separately; bin k is at k.

Every stage halves (CMSIS-style per-stage scaling, as `arm_cfft_q15`): eight halvings for 256 points, and the split's /2. The output is **X / 256**. Per-stage scaling never overflows and needs no data-dependent exponent. The PP's auto-gain and the log bands deal with quiet signals, and the tap's `VIZ_RD_NORM` can shift a quiet window up by whole bits exactly (`visualizer.md` §11.2).

### 5.2 Headroom, stage by stage

| Point | Bound on a component | Why |
|---|---|---|
| windowed z | < 2^14 (11,500 per part) | Hann peak 23,000 |
| step 1, stages h = 8, 4, 2 | a' < 2^14; d = a − b < 2^15 | each stage halves |
| step 1 twiddle sums | \|dr·wr − di·wi\| ≤ \|d\|·\|w\| < 2^30 | far below the accumulator's 2^31 |
| step 1 last stage (unscaled) | < 2^15 | two halved values summed |
| step 2 | < 2^14 | Q15 multiply halves |
| step 3, halved last stage | < 2^14 | as step 1 |
| S, D, T, U | < 2^15 | sums of two < 2^14 |
| X / 256 | ≤ 23,000 per part in principle; the test's largest is 11,502 | the window sum |
| power | < 2^31; the largest seen is 1.32 × 10^8 (6.2 % of 2^31) [mac] | so the accumulator never saturates |

### 5.3 Two roundings: the proposal (SEP) and today's (EXACT)

Step 1's and step 3's twiddles are immediates. Their exact four-product chains use forms silicon has run, so both modes keep them. The modes differ only where a twiddle comes from a table (step 2, the split):

- **`VFFT_SEP` (proposed).** Each product is floored on its own by a plain `vmulhd` (silicon), and the floored products are added in 16 bits:
  - step 2: `zr = ⌊yr·wr⌋ − ⌊yi·wi⌋`, `zi = ⌊yr·wi⌋ + ⌊yi·wr⌋`;
  - split: `Xr = S>>1 + ⌊T·Wr⌋ + ⌊D·Wi⌋`, `Xi = U>>1 + ⌊T·Wi⌋ − ⌊D·Wr⌋`.
  - Two or three floors per output instead of one: a larger and slightly biased error.
- **`VFFT_EXACT` (today's `jviz.c`, bit for bit; the test checks it).** Each output is one exact accumulator sum, floored once. It needs four accumulator forms with a vector B operand that silicon has never run (§6.6).

Against a double-precision DFT of the same windowed samples, 68 signals [mac, `tests/vizfft`]. Floor is the mean error power of a bin, in dB below full scale (23,000 units):

| Class | SEP SNR | SEP floor | EXACT SNR | EXACT floor | Worst bin (both) |
|---|---|---|---|---|---|
| sines (0, −12, −41 dBFS) | 44.5 dB | −82.8 dB | 48.2 dB | −86.5 dB | 5.2 units |
| sweeps | 46.3 | −81.7 | 50.1 | −85.6 | 6.4 |
| noise (uniform, Gaussian, pink) | 42.2 | −80.9 | 45.8 | −84.4 | 6.4 |
| silence and 1–2 LSB dither | — | −85.6 | — | −89.1 | 3.2 / 2.3 |
| clipping (×4 sines, square, DC, Nyquist, impulses) | 50.9 | −83.2 | 54.2 | −86.5 | 6.1 |
| music-like (chord, kick, hats, voice, a loud mix) | 36.8 | −81.9 | 40.2 | −85.3 | 7.4 |

**Per band** (the 24 bands, each band's largest power, model against double, every signal) [mac]:

| Band level (dB above 1 unit²) | SEP worst | EXACT worst | Bands | Bound |
|---|---|---|---|---|
| ≥ 70 dB | 0.01 dB | 0.01 dB | 23 | 0.02 dB |
| 60–70 dB | 0.03 | 0.03 | 44 | 0.05 |
| 50–60 dB | 0.10 | 0.06 | 182 | 0.17 |
| 40–50 dB | 0.29 | 0.20 | 206 | 0.54 |
| 30–40 dB | 0.72 | 0.60 | 119 | 1.83 |
| 20–30 dB | 2.29 | 1.94 | 65 | 7.96 |
| < 20 dB | 11.8 | 15.2 | 104 | none (the floor) |

- **The bound** follows from the test's amplitude check: every band's amplitude is within E = 6 units (SEP at most 5.1, EXACT 3.5), so a band at A units is within 20 log10(A / (A − E)) dB. The test holds every row to it.
- **What the display sees:** full scale is about 87 dB. A loud band sits at 70–85 dB, and the shell shows the 48 dB below its auto-gain top. So the bottom of the bars sits around 25–40 dB, where SEP's error is 0.7–2.3 dB and EXACT's 0.6–1.9 dB: a pixel or two in a 228-row bar.
- **The verdict:** SEP is enough. EXACT would buy 3.5 dB of floor, which the bars can't show. It stays an option behind the bench row (§10, VF3), not a goal.

## 6. The vector unit layout

### 6.1 The register file in one paragraph

- The VRF is 64 rows of 64 bytes. An **H16** vector is the 16 halfwords of one row, at x 0 (bytes 0–31) or x 32 (bytes 32–63). A **V16** vector is one column of 16 rows: element i is row y + i's halfword in lane x & 15 [apple; isa-vector.md "VRF layout"].
- The two views alias the same bytes. So a REP 16 `vmov` that writes H16 rows from V16 columns, each stepping, **transposes a 16 × 16 block in one instruction**. That is bench GTHR's form [silicon, sitting 2].
- **Layout:** real parts at x 0, imaginary parts at x 32 of the same rows, in four 16-row blocks: B0 rows 0–15, B1 16–31, B2 32–47, B3 48–63.

### 6.2 The block plan

| Phase | B0 (0–15) | B1 (16–31) | B2 (32–47) | B3 (48–63) |
|---|---|---|---|---|
| load, window | samples → z (in place) | window | — | — |
| step 1 | z, in place | ← y (last stage, bit-reversed rows) | — | d temporaries (≤ 8 rows) |
| step 2 | ← ⌊yr·wr⌋, ⌊yi·wi⌋ → z' | y | twiddles W_256 | ⌊yr·wi⌋, ⌊yi·wr⌋ |
| transpose | z' | ← z'ᵗ | — | — |
| step 3 | ← Z (last stage, halved) | in place | — | d temporaries |
| partner | Z | LR, then the partner P | T = Zᵗ, then P's rows at 32–48 | T2 |
| split | Z, then products | P, then T; U → Xi | W_512 (natural) | S → Xr; D |
| power | lo products, then the power's halves | Xi | hi products | Xr |
| store | rows 0–15 at x 0 → raw | | | |

Row 63 x 0 holds the lane-0 flag row during the partner phase.

### 6.3 Butterflies, twiddles, bit reversal

- **A butterfly stage** is three REP h instructions a component over whole rows: `vsub` d into B3, then `vadd` and `vasr #1` into a. Each of the 16 lanes is an independent transform, so no lane ever talks to another.
- **The b rows' twiddles** are REP 1 instructions, one row at a time, because each row has its own e:
  - e = 0: `vasr #1`;
  - e = 4: `vasr` plus `vrsub #0`;
  - otherwise four instructions a component: `vmul … #w clra x15`, `vmul … #w' x15`, `vmulhd … #w acc`, `vmulhd D, … #w' acc`.
  - `clra` acts on every repetition, so an accumulator chain can't be REP'd across rows [apple-RE; `isa-vector.md`].
- **The twiddles of steps 1 and 3** are immediates (W_16^e, 16-bit signed in the 80-bit form [apple]): there are no table loads.
- **The four-step twiddle** is one table, `tw` (256 + 256 halfwords; today's `jviz_tw512`, unchanged), loaded REP 16 into B2.
- **The split's** `W_512^k` table is stored in natural order: today's `jviz_ws512`, transposed.
- **Bit reversal costs nothing:** the last stage of each transform writes its two outputs straight to their bit-reversed rows (REP 1 adds and subtracts into chosen D rows). So the block comes out in natural order, with no permutation pass.

### 6.4 The transpose instead of column stages

- **Today's kernel** ran step 3 along the lanes as V16 column operations (`jv_fft_cols`). That is 12 forms silicon has never run, among them `vmulhd` into a V16 D with an immediate and `acc`, which isn't among Apple's kinds.
- **The proposal** transposes B0 into B1 (two REP 16 `vmov`s: `vmov DH16++ AH8 BV16++ rep`, silicon in GTHR), then runs **the same row-stage code as step 1**.
- **What it costs:** 33 cycles for the transposes.
- **What it buys:** step 3's arithmetic is unchanged, the row forms are already proven, and the output lands in natural order: Z[16 k1 + k2] at row k1, lane k2.

### 6.5 The partner block: a lane reversal by transposes

The split needs `Z[256 − k]` beside `Z[k]`. In natural order, the partner of (row a, lane b) is (row 15 − a, lane 16 − b) for b ≠ 0, and (row (16 − a) & 15, lane 0) for b = 0. Rows can be read in any order: a REP 1 move names its source row. Lanes can't be permuted: the vector unit has no shuffle. So the lane reversal goes through the transpose:

```
T   = Zᵗ                      2 transposes (REP 16)         T[b][a]  = Z[a][b]
T2[b] = T[(16 - b) & 15]      32 row moves (REP 1)          rows permuted
LR  = T2ᵗ                     2 transposes                  LR[a][b] = Z[a][(16 - b) & 15]
P[a] = LR[15 - a]             32 row moves, into rows 33..48; row 32 = LR[0]
B1  = rows 33..48             2 copies (REP 16)             P, right for every lane but 0
lane 0: B1 = rows 32..47 ifn  2 predicated copies (REP 16)  lane 0 from one row back
```

- **Lane 0's partner is one row further** than the other lanes'. So P's rows are laid down once with a copy of P[15] in the row in front (row 32). Rows 32 + a then hold lane 0's partner for row a.
- **One predicated REP 16 copy a component** takes lane 0 from there. The flag is N on lane 0 only, from a `vsub … setf` of a loaded row (−1, 0, 0, …) minus a zero register. Those are BARV's forms: `vsub Ddiscard AH16 Breg setf` and `vadd DH16++ AH16++ Bimm ifn rep` [silicon].
- **The tests have teeth here:** skipping the fix is caught by 65 of 68 signals [mac, mutation `lane0`].
- **The cost:** 236 cycles, an eighth of the kernel. It is the price of doing the reversal with proven forms.
- **The alternative,** 16 V16 column moves a component (today's way), uses `vadd DV16 AV16 Bimm`, which silicon has never run.

### 6.6 The forms

**The proposal (SEP), 26 kinds, every one `silicon` or `apple`** [mac: the test reads `proven_forms.txt`]. The last column says whether the form also occurs in Apple's static code (§6.7 has the method): "Apple-used" adds confidence in what the form does, "silicon only" is fine as it stands.

| Form key | Insns | Class (evidence) | Apple's code [apple-RE] | Where |
|---|---|---|---|---|
| `mem48 vld16 rep stride nooff` | 8 | apple | Apple-used: 101 sites, @100c0c (aacdec), its FFT's loads @1039ba | samples, tables |
| `mem48 vld16 single nostride nooff` | 1 | apple | Apple-used: 32, @101552 (h264dec) | the lane-0 flag row |
| `mem48 vst16 rep stride nooff` | 2 | apple; REP 16 from rows 0–15 [silicon 2026-10-25] | Apple-used: 41, @103b46 (aacdec) | raw out |
| `vmulhd DH16 AH16 BH16 - 80` | 192 | silicon (MHDU) | silicon only (Apple's vector-B `vmulhd`s all accumulate) | window, step 2, split, power high halves |
| `vadd/vsub DH16++ AH16++ BH16++ rep 80` | 34 / 32 | apple / silicon (BARV) | Apple-used: @112270 (mpg4dec) / @1085cc (h264dec) | butterflies, sums |
| `vasr DH16++ AH16++ Bimm rep 80`, `vasr DH16 AH16 Bimm - 80` | 32, 56 | silicon (BARV) | Apple-used: @10281c (mpg4dec) / silicon only | halvings |
| `vmul Ddiscard AH16 Bimm clra x15 80` | 40 | silicon (X15M) | silicon only (Apple's `clra` chains start on other ops) | twiddle chains |
| `vmul Ddiscard AH16 Bimm x15 80`, `vmulhd Ddiscard AH16 Bimm acc 80`, `vmulhd DH16 AH16 Bimm acc 80` | 40 each | apple | Apple-used: @109d68, @109d72, @109d8c (h264dec, the centre half-pel) | twiddle chains |
| `vrsub DH16 AH16 Bimm - 80` | 14 | silicon (RSB0) | silicon only (Apple: the same op on V16, @113ce in vmcs) | W = −i |
| `vadd DH16 AH16 BH16 - 80`, `vsub DH16 AH16 BH16 - 80` | 32, 32 | silicon (ALUX; H.264) | silicon only in this exact shape (Apple's own adds carry `+r` or other widths) | last stages |
| `vmov DH16++ AH8 BV16++ rep 80` | 6 | silicon (GTHR) | silicon only at x 0; **Apple reads a stepped V16 B at x 32 this way** (h264dec @10a860): C1 below | transposes |
| `vadd DH16 AH16 Bimm - 80` | 66 | silicon (ADDI) | Apple-used: @1029e6 (mpg4dec) | row moves |
| `vadd DH16++ AH16++ Bimm rep 80` | 3 | silicon (ADIR) | Apple-used: @15a52 (vmcs) | block copies |
| `vsub Ddiscard AH16 Breg setf 80` | 1 | silicon (BARV) | silicon only | lane-0 flag |
| `vadd DH16++ AH16++ Bimm ifn rep 80` | 2 | silicon (BARV) | silicon only | lane-0 fix |
| `vmul DH16++ AH16++ BH16++ rep 80` | 2 | silicon (BARV) | silicon only | power low halves |
| `vmov Ddiscard AH8 BH16 clra acc 80`, `… x15 80`, `… acc 80` | 16, 32, 16 | silicon (X15S, X15Z, ACSA) | Apple-used: @1021e6 (h264dec), @1002ba (slideshow), @10297e (mpg4dec) | the power's exact sum |
| `vmov DH16 AH8 Bimm x15 80`, `vmov DH16 AH8 Bimm acc 80` | 16, 16 | silicon (X15M) | Apple-used: @102a28 (mpg4dec), @107762 (h264dec) | reading the halves |

Tally: 16 of the 26 kinds are Apple-used as well as filed `silicon`/`apple`; 10 are silicon only. None is neither. The proposal uses **none of the suspect forms of §6.7**: no V16 A operand, no accumulator op with a vertical operand, no `+r` anywhere. Nothing in it is flagged.

- **The power without a vector-B accumulator form:** each square's low half comes from `vmul` (REP 16) and its high half from `vmulhd` (REP 1). Four `vmov`s into the accumulator (`clra acc`, `x15`, `x15`, `acc`) then sum them exactly, carry included, and two `vmov #0` read the low and the high halves back. These are exactly the moves bench rows X15S, X15Z and ACSA ran.

**What the proven-forms table can't see:** coordinates. Two coordinate variants of proven forms are new on our silicon:
- **C1:** the transpose's V16 source at x 32 (GTHR read x 0). Apple's H.264 code reads a stepped V16 B at x 32 in this same REP move (`vc_mangle` @10a860, h264dec) [apple-RE], so what it does is not in doubt; only our silicon hasn't run it.
- **C2:** `vld16` into x 32. Apple's AAC FFT loads and stores 16-bit rows at x 32 (@10454c, @103b4c, aacdec) [apple-RE].

H16 ALU operands at x 32 are proven by `jv_mix16` [silicon]. The bench row checks C1 and C2 directly (§10, VF4), and its phase dumps would catch either.

**The forms today's kernel uses that silicon has never run** (31, all `guard` under `J_VPU_FFT`): every one is in §6.7's table, with what we believe it does, its evidence and its rank as a suspect for the 4e/4f failure.

**Forms only the upgrades would need** (bench row VF3; none is in the proposal):

| # | Form | Apple's code [apple-RE] | Buys |
|---|---|---|---|
| F1–F4 | `vmul Ddiscard AH16 BH16 clra x15`, `vmul Ddiscard AH16 BH16 x15`, `vmulhd Ddiscard AH16 BH16 acc`, `vmulhd DH16 AH16 BH16 acc` | **all four Apple-used**: @100118, @100136 (slideshow), @1029b0, @1029ba (mpg4dec); Apple's AAC FFT computes its twiddle products with exactly these chains, the twiddle row at `+r` (@1042d4, @1042de, aacdec) | the EXACT mode: one floor per output, +3.5 dB of floor; +208 cycles, +1.9 KB of code [model] |
| F5 | `vmulhd DH16++ AH16++ BH16++ rep` (REP 16, the elementwise high product) | not Apple-used (none of Apple's `vmulhd`s repeats); our silicon has run the REP 1 op (MHDU) | the 192 REP 1 `vmulhd`s become 12 REP 16 ones: about −90 cycles and **−1.8 KB of code** [model arithmetic] |

F1–F4 now carry Apple's evidence of what they do, so only a VF3 pass stands between them and the EXACT mode. The design still proposes SEP, because EXACT's 3.5 dB is below what the bars can show (§5.3).

### 6.7 Provenance, and the suspects in today's kernel

**How Apple's code is used here (owner, 2026-11-05).** Apple's VideoCore code is our trusted **baseline, not the ceiling**. It is over fifteen years old and shaped by the tools and time its authors had. It does not show the vector unit's limits, and our kernels may well be more efficient than anything in it. So:
- where Apple uses a form, that is **evidence of what the form does** and adds confidence;
- "Apple never used it" is **no reason to avoid a form**, and we never retreat to Apple's subset;
- when one of our kernels fails, the **first suspect is our understanding of an instruction**: probe it on its own, learn its real behaviour, and correct the manual (`docs/bcm2722-reference/isa-vector.md`).

**Method [apple-RE].** The scan covered:
- every function of the six libraries of firmware 1.3 (`aacdec`, `h264dec`, `mpg4dec`, `slideshow`, `mplayer`, `passthruhandler`), swept from each symbol to the next;
- `vmcs.bin` and `RenderServer.bin`, traced from their entry points.

Each vector instruction's operand form was computed as `proven_forms.txt` writes it (`vc2vpu.form_keys`), giving 805 operand forms. "Apple-used" below means the form occurs in that static code. That is wider than `proven_forms.txt`'s `apple` class, which means only that Apple's self-test executes the form. Addresses are link addresses in the named library, or offsets in `vmcs.bin`. The scan scripts stay outside git with the rest of the reference material.

#### 6.7.1 Today's kernel, form by form

`jv_fft256`, `jv_fft512`, `jv_fft_tw`, `jv_fft_cols` and `jv_fft_split` (`bcmfw/video/viz_kernels.S`, generated by `gen_viz.py`) use 46 operand forms. The provenance column reads:
- **both:** Apple-used and run on our silicon;
- **silicon only**;
- **Apple only**;
- **neither**.

Confidence is how sure we are of what the form does.

| Forms (sites) | What we believe it does | Evidence | Provenance | Confidence |
|---|---|---|---|---|
| `mem48 vld16 rep stride nooff` (16) | loads REP rows of 16 halfwords, one row per stride; each row's address rounds down to 32 bytes | Apple @100c0c, its FFT @1039ba (aacdec); every kernel of ours [silicon]; quirks #38 | both | high |
| `vadd/vsub DH16++ AH16++ BH16++ rep` (20 + 20) | wrapping 16-bit add or subtract, row by row | Apple @112270 (mpg4dec), @1085cc (h264dec); BARV [silicon] | both | high |
| `vasr DH16++ AH16++ Bimm rep` (20), `vadd DH16++ AH16++ Bimm rep` (2) | arithmetic shift right; adding an immediate | Apple @10281c (mpg4dec), @15a52 (vmcs); BARV, ADIR [silicon] | both | high |
| `vmul Ddiscard AH16 Bimm x15` (24), `vmulhd Ddiscard AH16 Bimm acc` (24) | adds the product's low half at bit 0, then its floored high half at bit 16: together, the exact product | Apple @109d68, @109d72 (h264dec, the centre half-pel); X15M [silicon] | both | high |
| `vadd/vsub DH16 AH16 BH16` (26 + 24), `vasr DH16 AH16 Bimm` (40), `vrsub DH16 AH16 Bimm` (10) | the same ops at REP 1; `vrsub #0` negates | ALUX, H.264, BARV, RSB0 [silicon]; Apple uses these ops in other operand shapes (@10a5c4, @10a5d8 h264dec; @10d5a vmcs) | silicon only | high |
| `vmul Ddiscard AH16 Bimm clra x15` (24) | clears the accumulator, then adds the product's low 16 bits, zero-extended | X15M [silicon] | silicon only | high |
| `vadd/vsub DV16 AV16 BV16` (16 + 16), `vasr DV16 AV16 Bimm` (60) | the REP 1 ops down a column: element i is row y + i of lane x | H.264 `vidct4_add` [silicon] | silicon only | high |
| `vmulhd DH16 AH16 Bimm acc` (24) | the chain's last op: D gets the accumulator's high half | Apple @109d8c (h264dec) | Apple only | high |
| `vasr Ddiscard AH16 Bimm acc` (2), `vshl Ddiscard AH16 Bimm clra x15` (2) | A · 2^15 into the accumulator: `A >> 1` into the high half, `(A & 1) << 15` into the low | Apple @109d58, @109d4e (h264dec) | Apple only | high |
| `vmov Ddiscard AH8 BH16 clra x15` (5) | clears, then puts B's 16 bits, zero-extended, into the low half (where the second chain starts) | Apple @102974 (mpg4dec); X15Z ran the form without `clra` [silicon] | Apple only | high |
| `vmul DH16 AH16 BH16 x15` (2), `vmul Ddiscard AH16 BH16 x15` (2), `vmulhd DH16 AH16 BH16 acc` (5) | the vector-B chain: as with an immediate, but B is a row, one value per lane | Apple @100122, @100136 (slideshow), @1029b0 (mpg4dec); Apple's FFT @1042d4 | Apple only | high |
| `vrsub DV16 AV16 Bimm` (7) | negates a column | Apple @113ce (vmcs) | Apple only | high |
| `vmul DH16 AH16 BH16 clra x15` (1) | as the vector-B chain, starting it; D gets the low half | Apple has the discard-D twin (@100118, slideshow) | neither | medium-high |
| `vmulhd DH16++ AH16++ BH16++ rep` (4: the window, REP 8 and 16) | the floored high product, row by row | MHDU ran it at REP 1 [silicon]; none of Apple's `vmulhd`s repeats | neither | medium-high |
| `vadd DV16 AV16 Bimm` (34), `vadd DV16++ AV16++ Bimm` (2, REP 1) | copies a column (adds #0) | Apple adds on V16 A (@100c18, aacdec) | neither | high |
| `vadd DH16 AH16+r++ BH16`, `vsub DH16 AH16+r++ BH16`, `vadd DH16 AH16+r++ Bimm`, `vrsub DH16 AH16+r++ Bimm` (2, 2, 6, 2) | an H16 A at `+r`, r a whole number of rows (y only, x 0); the step bit is idle at REP 1 | an A's `+r` y: REPA, GTHR (stepped, REP > 1) and R48E (48-bit) [silicon]; Apple has 140 unstepped H16 A `+r` sites (@100142, aacdec) | neither (as a whole) | medium-high |
| `vmul DH16 AH16+r++ BH16 clra x15`, `vmul Ddiscard AH16+r++ BH16 x15`, `vmulhd DH16 AH16+r++ BH16 acc`, `vmulhd DH16+r AH16+r++ BH16 acc` (2 each) | step 2's chains with the data row at `+r` | as the row above; Apple's FFT puts `+r` on A and B together (@104446, @104478, aacdec) | neither | medium |
| `vadd DH16+r AH16+r BH16` (3), `vmulhd DH16+r AH16 BH16 acc` (1), `vmul DH16+r AH16 BH16 x15` (1) | **an H16 D at `+r`: the result lands r's y rows down** | Apple writes an H16 D at `+r` in 216 places, its FFT at y 2, 4 and 6 (@1039f2, aacdec); our silicon has run only an x offset (D16R at x 3, which can't tell "aligned" from "dropped": quirks #28) | neither (Apple-used for y) | medium |
| `vadd/vsub/vasr DV16++ AV16++ … rep` (14 + 14 + 14, REP 2, 4 and 8), `vadd DV16++ AV16++ Bimm rep` (6) | **a V16 A that steps one column per repetition**, as D and B do | Apple steps a V16 A at REP (@103258 h264dec, @1a34a vmcs); our silicon has stepped a V16 D (BARV) and a V16 B (GTHR), never a V16 A | neither (Apple-used for A) | medium |
| `vmulhd Ddiscard AV16 Bimm acc` (20) | the floored high half of a V16 A's product, into lane i's high half | Apple accumulates from a V16 A with other multiplies (@10df0, vmcs) | neither | medium |
| `vmulhd DV16 AV16 Bimm acc` (20) | **`acc` writing a V16 D**: element i gets lane i's high half | Apple writes a V16 D from the accumulator with `vmul` (@109fa2, h264dec), `vmulm` and `vshl`, never with `vmulhd` | neither | medium |
| `vmul Ddiscard AV16 Bimm clra x15`, `vmul Ddiscard AV16 Bimm x15` (20 + 20) | **`x15` with a vertical A**: element i's low product half goes into lane i's accumulator | nothing: Apple never pairs `x15` with a vertical operand (its vertical accumulator ops are `acc` only: @109fa2 h264dec, @10df0 vmcs), and our silicon has run `x15` only with H operands | neither | **low** |

Tally: 7 forms are both, 8 silicon only, 8 Apple only and 23 neither.

#### 6.7.2 The suspects, and what flipping each does in the model

The start-up check runs six cases: a square, noise and the saturating worst case, at 256 and at 512 samples. It fails if any one halfword differs.

Today's kernel and its check were run in vc2sim under **every alternative reading the simulator has a switch for** (`vc2vpu.HYP_ALTERNATIVES`: 25 switches, 40 alternatives), one at a time. They were also run under six alternatives added for this question [sim, 2026-11-05; the probe scripts are outside git]:

| Alternative reading | Check | Halfwords off per case (of 256 or 512) | Status of the alternative |
|---|---|---|---|
| any of 36 switch alternatives (B `+r`, load and store alignment, the A `+r` x rules, the saturation rules, a vertical D's `+r` y, the 48-bit `+r`, …) | passes | 0 | none: the kernel is robust to every one |
| `x15` adds the sign-extended result, or acts as `acc` | fails | 125–499 | refuted: X15M, X15Z [silicon] |
| `vmulhd` rounds instead of flooring | fails | 105–382 | refuted: MHDU [silicon] |
| every `+r` on an unstepped operand ignored | fails | all | contradicted by Apple's 216 H16 D `+r` sites [apple-RE] |
| **`x15` ignored when the A operand is vertical** | fails | **30–91** | open: no evidence either way |
| **a V16 A does not step across a REP** | fails | 127–429 | open: Apple uses it; our silicon never has |
| **`acc` ignored when D is vertical** (a plain `vmulhd` written) | fails | 107–390 | open: Apple does the same with other multiplies |
| **an H16 D's `+r` y aligned down to 16** | fails | all | open: Apple's use says y is applied |
| an H16 A's `+r` y dropped | fails | 120–446 | unlikely: GTHR, REPA and R48E apply y [silicon] |
| no interlock when an op reads in the other orientation straight after a write | passes | 0 | none: the kernel never does that back to back |

**What 4e/4f rule out.** `J_VPU_BARS` passed in the same load, with the same stack and stores. So these all work:
- the overlay load and its code flush;
- vector loads seeing scalar stores;
- `vld16` and `vst16` at REP 16;
- the H16 REP ALU forms and a stepping V16 D;
- the C-side machinery the two checks share.

**Ranking**, most likely first:
1. **`x15` with a V16 A** (40 sites, the twiddles in `jv_fft_cols`). Neither Apple nor our silicon has ever run it. If it fails, most bins stay right: "a few bins off" is what a check sees when it fails on every signal. The accumulators are per lane, but that a vertical operand's element i feeds lane i's low half is our inference only.
2. **A V16 A stepping at REP 2–8** (48 sites, every column butterfly). Apple uses it, so it exists. Our silicon has stepped only a V16 D and a V16 B.
3. **`acc` into a V16 D through `vmulhd`** (20 sites). Apple writes a V16 D from the accumulator, but only through other multiplies.
4. **An H16 D at `+r` y** (5 sites, in `jv_fft_tw` and the split). Apple relies on y being applied, but our silicon has never checked it, and the only D `+r` row we have (D16R) can't tell.
5. **The REP 8/16 `vmulhd` with all three operands stepping** (the window, 4 sites). The model can express no alternative reading that fails. It is listed because no one has run it.
6. **The C** (`jviz_raw_ref_t` on the BCM, about 106k scalar instructions with no golden). Bench row VF0 settles it.

**The proposal (SEP)** uses none of suspects 1–5. Its only vertical operand is the transpose's B (GTHR's form, which Apple also uses at x 32), and it has no `+r`. Nothing in it is flagged, and the design is unchanged.

#### 6.7.3 Apple's spectral kernels, and what is worth taking

Apple's code has three families of FFT-like kernels [apple-RE]:
- **AAC's FFT and IMDCT** (`aacdec`): a 512-point and a 64-point complex FFT, with their own twiddle tables, under the IMDCTs (`vc_limdct2048` @1046ee, `vc_limdct256` @10487e). The FFT is `vc_lfft512down` @103c16 and `vc_lifft512down_blocky` @1039a0, with its middle stages at @104220 and the general stage at @104534.
- **MPEG-4's IDCT** (`mpeg4_idct` @101ac0, `vc_IDCT` @1127b4, `mpg4dec`).
- **H.264's 4 × 4 and 8 × 8 integer transforms** (`vc_inverseinteger`, h264dec).

Its instruction choices, in our words:
- **Precision.** AAC's FFT works in 32-bit fixed point. Each value is a pair of halfword rows. A butterfly is a halfword add or subtract that sets the flags, then a carry-in add on the high halves.
- **Twiddle products.** A 32 × 16 product is one exact accumulator chain. The unsigned low half times the twiddle goes in with the rounding multiply, the signed high half with `vmul … x15` and `vmulhd … acc`. The twiddle is a **vector B row at `+r`**, and the low and high halves are read from the chain's own D writes. These are F1–F4's forms, with `+r`.
- **Scaling.** Each stage shifts its 32-bit values right by one through the accumulator: a logical shift of the low half, plus the high half shifted left by 15.
- **Reordering.** Bit reversal and the change of stride go **through memory**, as strided 16-row loads and stores at chosen offsets. The FFT has no vertical ALU operands at all.
- **MPEG-4's IDCT** multiplies by Q16 constants (up to 0xB505) with the rounding, unsigned-B multiply, on V16 operands at `+r`.

**Is any of it worth mirroring?** Not the arithmetic:
- 32-bit data would roughly double the kernel, for precision the bars can't show (§5.3).
- Reordering through memory would add SRAM traffic that our in-VRF transposes avoid.

The evidence is worth a lot, though:
- Apple's FFT is the strongest evidence yet that the vector-B accumulator chains (F1–F4) behave as we model them. That leaves the EXACT mode one VF3 pass away.
- MPEG-4's Q16 rounding multiply is a **consideration only**. If a later precision pass wants twiddles one bit finer, that is how Apple got there. It is not a safety or correctness fix, so the model and the tests are unchanged.

## 7. Cost

**The rule:** a vector instruction of REP n costs n + 0.5 cycles, and a scalar instruction 1. This is the rule `visualizer.md` §7 used for today's kernel, from silicon S7's REP 16 timings (16.25–16.63 cycles); the REP 1 issue cost is open question Q5. Counts from the model, which issues every instruction the kernel will [model]:

| Phase | Vector insns | Rows (Σ REP) | Scalar | Code bytes | Cycles |
|---|---|---|---|---|---|
| load, window | 36 | 96 | 5 | 344 | 119 |
| step 1 | 189 | 291 | 0 | 1,890 | 386 |
| step 2 | 68 | 128 | 2 | 672 | 164 |
| transpose | 2 | 32 | 0 | 20 | 33 |
| step 3 | 191 | 323 | 0 | 1,910 | 419 |
| partner | 76 | 196 | 2 | 756 | 236 |
| split | 76 | 256 | 2 | 752 | 296 |
| power | 130 | 160 | 0 | 1,300 | 225 |
| store | 3 | 48 | 3 | 22 | 53 |
| **total (SEP)** | **771** | **1,530** | **14** | **7,666** | **1,930: 71.5 µs at 27 MHz, 12.9 µs at 150 MHz** |
| total (EXACT) | 958 | 1,642 | 16 | 9,536 | 2,137: 79.1 µs, 14.2 µs |

Against what runs or would run [sim unless marked]:

| Path | Cycles a frame | 27 MHz | 150 MHz | At 30 fps |
|---|---|---|---|---|
| today on silicon: `SPECTRUM` with the C FFT (`J_VPU_FFT` clear) | 112,390 | 4.16 ms | 0.75 ms | 12.5 % of the BCM at 27 MHz |
| today's kernel `jv_fft512` alone, had it passed | 2,428 | 90 µs | 16 µs | |
| **the proposed kernel alone** | **1,930** [model] | **71 µs** | **13 µs** | 0.2 % |
| **the proposed `SPECTRUM` job** | **about 7.2k** [estimate] | **0.27 ms** | **0.05 ms** | **0.8 %** |
| the PP's CPU path (256 samples, 12 bands; 15 fps) | 46.5k ARM insns [qemu] | | | 2–3.5 % of an 80 MHz PP |

- **How the job estimate is made:** today's 512-sample job is 10,126 cycles. Take off its kernel (2,428) and its unpacking pass (2,826; with natural-order output the bands read the raw halves directly), add the new kernel (1,930), and add about 400 for the bands' two halfword loads a bin.
- **The PP's side doesn't change:** the tap, the read, the upload, and one reply poll.

## 8. Memory

| What | Bytes | Where |
|---|---|---|
| VRF | all 64 rows (4 blocks) during the kernel | the vector unit (no save: the kernel owns it for the job's length, as every job kernel does) |
| tables: window 1,024, `tw` 1,024, `ws` (natural order) 1,024, the lane-0 row 32 | 3,104 | overlay `.rodata` |
| code, SEP | 7,666 (5,866 with F5) | overlay `.text` |
| raw output (512 halfwords, 32-byte aligned) | 1,056 | stack, as `viz_spectrum` today |
| bench diagnostics (§10) | about 96 | overlay `.bss` |

**Against the overlay budget** [build + estimate]:
- The region is 36,864 B; `viz-ovl-m.bin` is 19,380 B, leaving about 17 KB.
- The new kernel replaces `jv_fft256`, `jv_fft512` and `jv_fft_cols` (5,870 B), so the overlay grows about 1.8 KB (none with F5).
- The tables are the same size: `ws` changes layout, not size.
- The 256-sample C path's derived tables (`jviz_derive256`) take `W_256^k` from `ws[2k]` in the new layout.
- About 15 KB would stay spare.

## 9. How it slots in

**Recommendation: the visualiser overlay, rebuilt with batch 5's regeneration; not the merged image.**

- **Why the overlay.**
  - The overlay is a separate file the PP loads at runtime, linked against the image's exports and paired by `ovlpair.py` at `make zip`. Its kernel can change without touching a byte of the merged image.
  - Batch 5 (`p20-batch5`) already rebuilds `viz-ovl-m.bin` (`a83ba1b5` → `925d2eb7`), so the new FFT rides the same device verify: one sitting, no extra image freeze (the merged-image freeze rule).
  - If batch 5 slips, the overlay can be rebuilt against `23693ff4` alone, because its imports don't change: the proposed kernel calls nothing in the image (it stores with its own `vst16 rep`), so the bars' `jv_store16` stays the only import.
- **Why not the merged image:** about 11 KB of kernel and tables resident, in an image whose arena can't move, for a job that runs only while one view is open. The overlay region exists for exactly this.
- **What changes in the next `bcmfw` batch (not now):**
  1. A generator emits the kernel from the model's instruction list. `tests/vizfft/vfft_vpu.c` is written as that list, one call per instruction, in order. It would be `gen_viz.py`'s FFT section, or a new `gen_vfft.py` beside it, as the team prefers. vc2sim's strict forms mode then runs it without a trap, every form being filed.
  2. `jviz.c`'s 512-sample FFT (the overlay's C fallback, and the start-up check's reference) takes the SEP arithmetic: `vfft_ref.c` is plain integer C written to compile under vc2cc. The 256-sample path (the PP's CPU fallback) is unchanged. The PP never compares its 256-sample frames with the BCM's 512-sample ones, so no live equality breaks.
  3. **The start-up check** (`viz_vpu_init`) compares the kernel with the C, as today, on a square, noise and a music-like mix. **And both with FNV-1a goldens computed on the Mac**, so a wrong C can't hide a right kernel (or the reverse). The old saturating worst case (tables of −32768) goes: SEP never sums products in the accumulator except the power, which is bounded by the window (§5.2).
  4. `SPECTRUM` keeps its op, arguments and reply. n = 512 runs the kernel when `J_VPU_FFT` is set; n = 256 runs the C (no PP sends 256 to the BCM: `viz.c` sends `N_BCM`).
  5. **`J_VPU_FFT` (0x8)** keeps its meaning: the overlay's FFT runs on the vector unit. The log line `bcmjob overlay ok … fft=vector` is the shell-side proof. The bench's jvpu line already reports it ("FFT n").
  6. **The bench diagnostics block** (`struct vfft_diag`, §10) sits in the overlay's `.bss`. Its address goes in the overlay's build record (`apps/video/bcmvideo_image.h`, beside `BCMVIDEO_VIZOVL_*`), so the PP reads it through the port without any change to the job block or the image.
- **The program-swap stub fix** (the warm swap, `quirks.md` #40–#41, chapter 21) is independent. The overlay load isn't a program swap; it runs the image's existing code flush (`hp_code_flush`) before `init`, which sittings 4e and 4f exercised (the bars' check ran).

## 10. Bench row VFFT (the next sitting; informational, hands-off)

- **Where it runs:** in the overlay's `init`, after its start-up checks, once per load. The visualiser's soak (`vizsoak`) and `uviz` already load the overlay during the sitting, so it costs the owner no time.
- **What the PP does:** its jvpu step (or a new `jfft` step beside it) reads the diagnostics block and logs one line per record.
- **The verdict:** never the session's. Every record is information, and a failure only keeps the C, as today.

| Record | What it runs | Pass | What a failure says |
|---|---|---|---|
| **VF0 C golden** | the BCM's C reference (SEP, 512) on three fixed frames (full-scale square, noise, the loud mix), FNV-1a of each raw output | equals the Mac's constants | the BCM's scalar C miscomputes the FFT: a compiler or scalar-form finding (look at the C's loads and multiplies first) |
| **VF1 today's kernel** | `jv_fft512` and `jviz_raw_ref` on the same frames, each against Mac goldens of today's arithmetic | both equal | answers 4e/4f: which side failed. If the kernel did, VF2's stage method applied to it localises the form |
| **VF2 phases** | the new kernel stopped after each of its 9 phases (a diagnostic entry per phase that stores all 64 VRF rows, 4 KB, to a stack buffer), FNV-1a against the model's VRF at that point (`vfft_vpu.c` gives them) | every phase equals | the first differing phase, row, x, lane, got and expected: one instruction group to look at |
| **VF3 single-instruction probes** | the suspects of §6.7.2, each probed alone on known inputs, every result row logged as words (VX15, VACD, VSTP, HDRY, HARY, MHDR below); then F1–F5, one 16 × 16 block each against the C, bschk-style | each probe's words equal one hypothesis's | the probe's words **name the semantics silicon follows**: correct `isa-vector.md` and vc2sim to it, then regenerate the kernel. F1–F4 failing: EXACT stays off; F5: keep the REP 1 `vmulhd`s |
| **VF4 coordinates** | C1: transpose a known block from x 32 and back; C2: `vld16` into x 32 then store (Apple uses both: §6.6) | round trip exact | the layout moves those operands to x 0 (one extra block copy each) |
| **VF5 timing** | the kernel 64 times, the 1 MHz counter, at the clock the view runs (the 27 MHz band) | — | the REP 1 issue cost (Q5): the cost table's only assumption |

- **Expected:** VF0, VF2 and VF4 pass, and the shell logs `fft=vector` with `J_VPU` 0x5b.
- **The order** follows the bench sessions' rule, basic first: VF0 → VF1 → VF2 → VF3 → VF4 → VF5. If VF0 fails, VF1 and VF2 are skipped with a reason (the reference is suspect), and VF3–VF5 still run. VF3's probes never depend on one another: each loads its own inputs.
- **Silicon-only forms (the VF2 note):** the proposal's 10 silicon-only kinds (§6.6) have each run on our unit, but not in this kernel's sequences or coordinates. VF2's per-phase dumps cover them as they are used here, so a silicon-only form that misbehaves in context shows as the first differing phase, not as a bare check failure.

**VF3's probes.** All share one input block, loaded by `vld16` REP 16 from a 32-byte aligned table:
- rows 0–15 at x 0 hold `a(r, l) = int16((16 r + l) · 2971 + 12345)`;
- the same rows at x 32 hold `b(r, l) = int16((16 r + l) · 40503 + 777)`;
- every row it reads but doesn't load is cleared first (`vmov DH16++ AH8 #0 rep`, VRF clear's form).

Each probe stores its result rows with `vst16` REP 1 (aligned) and logs them as 8 words a row, lane 0 in the low half of the first word. The expected words below are computed on the Mac from those formulas [mac]. The first word of each row usually tells the hypotheses apart, but every word is logged.

| Probe | Instructions (alone) | Logged | Words if the model holds (first 4 of 8) | Words under the alternative (first 4 of 8) |
|---|---|---|---|---|
| **HDRY** (an H16 D's `+r` y; suspect 4) | rows 16–47 at x 0 set to 0x5555; r3 = 1344 (y 21, x 0); `vadd DH16+r3(row 16) AH16(row 0) #0` | rows 16, 32, 37 | row 37 = row 0: `3bd43039 530a476f 6a405ea5 817675db`; rows 16 and 32 `55555555 …` | y aligned to 16: row 32 holds row 0; `+r` dropped: row 16 holds it |
| **VX15** (`x15` with a V16 A; suspect 1) | `vmul Ddiscard AV16(row 0, lane 3) #-23170 clra x15`; `vmulhd Ddiscard AV16(same) #-23170 acc`; read the halves with `vmov DH16 #0 x15` (row 32) and `vmov DH16 #0 acc` (row 33) | rows 32, 33 | lo `258c50ec ceccfa2c 780ca36c 214c4cac`; hi `fb80e2a4 2d37145b 046deb91 dba31d49` (lane i: a(i, 3) · −23170, exact) | `x15` ignored on a vertical A: lo `00000000 …`, hi as modelled. Any other pattern (all products in lane 3, a lane permutation) says how a vertical operand's elements reach the accumulators |
| **VACD** (`acc` into a V16 D; suspect 3) | `vmov Ddiscard AH8 #0x1234 clra acc`; `vmulhd DV16(row 16, lane 2) AV16(row 0, lane 3) #-23170 acc`; `vmov DH16 #0 acc` into row 34 | lane 2 of rows 16–31 (as 16 halfwords), row 34 | column `0db4f4d8 3f6b268f 16a1fdc5 edd72f7d`; row 34 the same (0x1234 + ⌊a(i, 3) · −23170 / 2^16⌋) | `acc` ignored on a V16 D: column `fb80e2a4 2d37145b 046deb91 dba31d49`, row 34 `12341234 …` |
| **VSTP** (a stepping V16 A; suspect 2) | `vadd DV16++(row 16, lane 0) AV16++(row 0, lane 0) #0 rep 4` | rows 16, 17 | row 16 `3bd43039 530a476f 00000000 00000000`; row 17 `f584e9e9 0cba011f 00000000 00000000` (lanes 0–3 = a(r, 0..3)) | A not stepped: row 16 `30393039 30393039 0 0`, row 17 `e9e9e9e9 e9e9e9e9 0 0` (every lane a(r, 0)) |
| **HARY** (an H16 A's `+r` y, unstepped) | r3 = 320 (y 5); `vadd DH16(row 40) AH16+r3(row 0) #0` | row 40 | row 5: `dc44d0a9 f37ae7df 0ab0ff15 21e6164b` | row 0: `3bd43039 530a476f 6a405ea5 817675db` |
| **MHDR** (the window's REP 16 `vmulhd`; suspect 5, and F5) | `vmulhd DH16++(row 48) AH16++(row 0, x 0) BH16++(row 0, x 32) rep 16` | rows 48, 63 | row 48 `e9db0092 f4de11b5 0ad62dcd d539df00`; row 63 `feb905ee 0a11eea7 205ff3df dedb040c` (⌊a · b / 2^16⌋) | rounded instead: row 48 `e9db0092 f4de11b6 0ad62dce d539df01`; not stepped: every row equals row 48 |

- **HDRY goes first** among VF3's probes (orchestrator, 2026-11-05): Apple's own FFT (@1039f2) and 216 other sites rely on an H16 D's `+r` y, so its reading is the strongest-evidenced suspect and the cheapest to settle. VX15 and VACD follow: the top-ranked suspects, which neither Apple's code nor our silicon has ever shown.
- **A probe that matches neither column is the most useful record.** Its words are the real semantics: fit them with vc2sim variants (the `vpu_fit_test.py` method), add a quirks entry, and pull the kernel and the model to it.

## 11. Open silicon questions

| # | Question | Settled by |
|---|---|---|
| Q1 | Why today's FFT check fails on silicon: the kernel (which of its 31 unproven forms; ranked in §6.7.2) or the BCM's C | VF0, VF1, VF3's probes (BCM open question #77) |
| Q7 | Does `x15` on an op with a V16 A add element i's low half into lane i's accumulator? | VF3 VX15 |
| Q8 | Does a V16 A step one column per repetition, as V16 D and B do? | VF3 VSTP |
| Q9 | Does `acc` on an op with a V16 D write lane i's high half to element i? | VF3 VACD |
| Q10 | Is an H16 D's `+r` y applied as given (Apple's code relies on it)? And an unstepped H16 A's? | VF3 HDRY, HARY |
| Q2 | The four accumulator forms with a vector B (F1–F4; Apple-used, §6.6) | VF3 |
| Q3 | `vmulhd` at REP 16 with stepped H16 operands (F5; today's window) | VF3 MHDR |
| Q2a | F1–F4 promoted (batch 5 follow-up): the probe sketch and each reading's words are in `isa-vector.md` "Promotion candidates"; not yet in the bench | a later sitting's bench row |
| Q4 | A V16 source at x 32 in the transpose; `vld16` into x 32 (C1, C2) | VF4 (VF2 would also catch them) |
| Q5 | The issue cost of a REP 1 80-bit vector instruction (the cost model assumes 1.5 cycles; S7 timed only REP 16) | VF5 |
| Q6 | Whether scalar instructions issue under a running REP (the 14 setup instructions are counted as serial) | VF5, against the model's count |

## 12. Files and tests

- `tests/vizfft/vfft.h`: the definition's interface and the model's.
- `tests/vizfft/vfft_ref.c`: the definition, plain integer C (both modes), the bands from raw output.
- `tests/vizfft/vfft_vpu.c`: the VRF model and the kernel, one call per instruction, each with its form key and cost.
- `tests/vizfft/vfft_tabs.c`: the tables, with Python's rounding.
- `tests/vizfft/vfft_test.c`, `run.sh` (run by `scripts/test.sh`, so by the gate). On 68 signals, all passing [mac]:
  1. the tables equal `jviz_tab.h`;
  2. EXACT equals `jviz_power_ref` on every bin (the model reproduces today's arithmetic, layout and all);
  3. the kernel on the model equals the definition, bit for bit, in both modes;
  4. SEP uses only `silicon`/`apple` forms, and EXACT's others are exactly F1–F4;
  5. precision against a double DFT, per class and per band level (§5.3), each held to its bound;
  6. the bands and `l2` equal `jviz_bands`/`jviz_l2`;
  7. three deliberate kernel bugs (the lane-0 fix skipped, step 2's sine sign, step 3's bit reversal) are each caught by 65 of 68 signals;
  8. the cost table above, SEP held under today's 2,428 cycles.
