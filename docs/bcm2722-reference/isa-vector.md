# 7. Vector unit (VPU)

## Model

The vector unit follows the architecture of the Alphamosaic patents [external: US 7,036,001 and the 2-D register-file patents] and VideoCore IV's VPU in *model*, with VideoCore II's own encodings [apple].

| Feature | Value | Evidence |
|---|---|---|
| Lanes | 16 | [apple]; [external] patent: 16 parallel 16-bit pixel units |
| Vector register file (VRF) | 64 rows × 64 bytes, 2-D, addressed by (y, x); readable horizontally and vertically | [apple] corpus addresses rows and columns 0–63 and nothing beyond; nothing proves it is not larger [hypothesis: exactly 64 × 64] |
| Element types | 8-bit (read unsigned), 16-bit (read signed) | [apple] (controls: 809 failures with signed 8-bit) |
| Repetition | REP 1, 2, 4, 8, 16, 32, 64, or r0 | [apple] |
| Flags | Per-lane Z, N, C; set with `setf`, used by predication | [apple]; [silicon] S4 onward |
| Accumulators | One per lane, **32 bits** as two 16-bit halves (see "The 32-bit accumulator") | [apple]; [silicon] S4 onward (the high half, through `acc`); the low half [apple-RE, sim]; [silicon] 2026-10-25 (session log `bcmtest.log`): X15M and X15Z as modelled, `acc x15` (X15S) not (quirks #31) |
| Scalar-result unit | Σ, min or max of the enabled lanes into a scalar register | [apple]; [silicon] S4 (`sum`) |
| Instruction lengths | 48 bits (compact) and 80 bits (full) | [apple]; [external] patent |

### VRF layout

Element i, byte k of a **horizontal** (H) vector at (y, x) is VRF byte (y, x + i + 16k); of a **vertical** (V) vector at (y, x), byte (y + i, x + 16k). So an H8 vector is 16 consecutive bytes of one row; an H16 vector covers 32 bytes of a row with each element's low byte in columns x … x + 15 and high byte in x + 16 … x + 31; a V8 vector is one byte from each of 16 rows. Coordinates wrap modulo 64. The H and V views alias the same storage and Apple's code relies on it (writing a V column and reading it as part of an H row) [apple].

Consequence: a 32-byte memory row loaded as one H16 vector is **deinterleaved**: even bytes land in the low halves, odd bytes in the high halves [apple; used by our box filter, correct in simulation; silicon S11 for a plain 16-bit row].

**A lane shift from the layout** [sim, 2026-10-24; **wrong on silicon**, quirks #30, kept as the record of the idea]: load two consecutive aligned 32-byte rows of 16-bit pixels as H16 vectors at (y, 0) and (y, 32). Row y then holds [lo0 hi0 lo1 hi1], 16 bytes each. An H16 read at (y, p), 0 ≤ p < 16, gives on lanes 0 … 15 − p pixels p … 15 of the first row: each lane's high byte sits 16 columns past its low byte, inside the same chunk. At (y, 16 + p), lanes 16 − p … 15 are pixels 0 … p − 1 of the second. Two moves, the second predicated on those lanes (`vbitplanes`), give the 16 pixels from p: the VPU's form of an unaligned load (AltiVec `vperm`, NEON `vext`), with no `vldp`/`vldq`. The offset must come through an **A** operand (x is a register's `+r`, or a constant): a horizontal B operand's `+r` x is aligned down to 16 on silicon (`quirks.md` #17b). It is right only if an H16 A operand's +r x is taken as given, which silicon refuted (ASHF/ASHG).

**A lane shift by transposes** [sim, 2026-10-25]: load the 32 pixels of 16 rows (pixels 0–15 in rows 32–47 x 0, 16–31 in rows 48–63 x 0); `vmov` an H16 D stepping a row from a V16 B stepping a column, REP 16, twice, so rows 16–47 x 32 hold the 32 pixel columns as rows (the H/V aliasing makes the transpose free); `vadd` rows 32–47 x 0 from an H16 A at `+r` = 64 p (a **y** offset: REPA, R48E), stepping; `vmov` F back from the V16 columns. Four REP-16 ops, exact whatever rule an A or B operand's +r x follows. `jv_gather16` (`gen_jobs.py`) uses it; bench row GTHR runs its four ops.

**A per-lane factor without a spare row** [sim, 2026-10-24]: a 16-bit vector B operand read **unstepped** (one row for every REP step) can sit in a row the same instruction's D doesn't write, even when the register file is otherwise full. The job kernels' fused shade (`jv_mix16`, `bcmfw/video/gen_jobs.py`) loads 16 factors REP 1 into the first row of a 16-row accumulator (x 0), multiplies a channel by them into the neighbouring 16 rows (x 32), then overwrites the accumulator: (c·k) >> 8 is that product's high byte, read as an H8 operand, so no shift is needed. Same kinds as `jv_shade16`'s; bit-exact in vc2sim, unchecked on silicon beyond those kinds.

**A gate at start** [sim, 2026-10-24]: an image that relies on an unchecked form can test it on itself before use. `jobs_bcm.c`'s `shift_check()` runs the lane shift once at start and publishes the result (`jobs.h` `J_VPU`); the jobs fall back to C if it disagrees. **On silicon (2026-10-25) it did disagree (the lane shift above), and the fallback worked:** the merged image published `J_VPU` 0x1 (checked, `J_VPU_SHIFT` clear), the display-only image 0x5, and the shell logged `bcmjob vpu=0x1 expect=0x3 why=degraded` (session log `ipodui.log`, line 26). **Sitting 2 (the transpose gather, build 799ff0f8d7) [silicon]:** the merged image published 3 (checked, shift: "checked 1, shift 1, MAC 0"; it does not decode, so MAC is never set) and the shell logged `bcmjob vpu=0x3 expect=0x3` at every BCM start; the display-only image published **0x23** (checked, shift, colour stage), its IDCT check failing (`J_VPU_MAC` clear; quirks #32; cause under investigation). Its loads use `vld16` REP 16 at a stride of 0.

## Formats

Halfwords hw1 … hw5, each written msb first. op9 = hw1 `[8:0]`.

| Halfword | 80-bit ALU (op9 ≥ `0x20`) | 48-bit ALU | Memory (op9 < `0x20`) |
|---|---|---|---|
| hw1 | `0010 000o oooo oooo` | `0010 001o oooo oooo` | same, op9 < `0x20` |
| hw2 | D: `dddd dddd` coordinate · `rrr` +r(rrr) (7 = none) · `ttttt` t5 (A's block bits) | same | load: D coordinate `[15:8]`; `[7:0]` 80-bit as ALU, 48-bit offset bits `[14:7]` (store: see "Stores") |
| hw3 | A low 5 bits `[15:11]` · I `[10]` (immediate) · B `[9:0]` (or imm low 10) | A 5 bits · I · B 10, or I = 1: `F RRR iiiiii` (SETF, REP, imm6) | offset `[15:9]` · REP `[8:6]` · base r0–r7 `[5:3]` · stride `[2:0]` (r0–r6, 7 = none) |
| hw4 | B ext `[15:10]` (or imm high 6) · A high 6 `[9:4]` · A +r `[3:1]` · inc `[0]` | — | 80-bit loads only |
| hw5 | `h F PPP C c 0 RRR SS rrr` (modifiers) | — | as ALU |

All fields of every vector instruction in Apple's corpus are assigned, and the corpus reassembles byte-identically [apple]. 5% of vector bytes are operations not yet named.

### Operands

| Operand | Decoding | Evidence |
|---|---|---|
| D | Coordinate d8 = hw2 `[15:8]` = `y5 y4 x5 x4 c3 c2 c1 c0`, c = y for H, x for V; +r = hw2 `[7:5]`. **D's direction and width come from the opcode's `.w` field** (op9 `[2:1]`: 0 H8, 1 H16, 2 V8, 3 V16) | [apple] (control: 430 failures from t5 instead) |
| D discard | A 16-bit D in x-block 1 (`d10`) writes nothing; used as a dummy destination for accumulator chains, `setf` and `sum`. An 8-bit `d10` is a real location | [apple] (control: 597) |
| t5 (hw2 `[4:0]`) | Belongs to **A**: bit 0 x += 32, bit 1 y += 16, bit 2 y += 32, bit 3 x += 16 [hypothesis for bit 3 in 80-bit ops], bit 4 = V (A modes 0/2) or "A steps per REP" (modes 4–7). In a **48-bit** op (not an `x` form) bit 3 instead gives the instruction's +r to A | [apple] (control: 843); bit 3 in 48-bit ops [apple-RE, sim] (control: 120 of 120) |
| A, 80-bit | A11 = hw4 `[9:4]` : hw3 `[15:11]`, + t5 block, + r(hw4 `[3:1]`). A11 `[10]` = 1: H8 at (dy = A `[9:6]`, dx = A `[4:0]`), steps per REP iff t5 bit 4. A11 `[10:8]` = 0 with t5 bit 4: V8 column A `[4:0]`, steps iff A `[5]`. `a000` without bit 4: block origin (in practice A unused). A11 `[10:8]` = 2/3 with t5 bit 4: V16 column A `[3:0]`; without: H16 row A `[3:2]`·4 + A `[7:6]` (mode 3 steps per REP, mode 2 does not) | [apple] (controls 133–628 each) |
| A, 48-bit | 5 bits: the row (H) or column (V) index in D's direction and width, plus the t5 block | [apple] |
| B vector | Coordinate hw3 `[9:2]` (d8 layout), hw3 `[0]` = V, hw4 `[11]` = 16-bit, hw4 `[10]` = steps per REP, +r = hw4 `[14:12]`. hw3 `[1]`: in a 48-bit op (not an `x` form), B takes the instruction's +r; in an 80-bit op unknown (it occurs only on B operands that name a register) | [apple] (control: 542); hw3 `[1]` in 48-bit ops [apple-RE, sim] |
| +r of a 48-bit op | The one +r field (hw2 `[7:5]`) goes to A when t5 bit 3 is set and to B when hw3 `[1]` is (both may), and then not to D. In the corpus a non-`x` 48-bit op names a register exactly when one of the two bits is set (17 ops; none in the other 694). The `x` forms (op9 bit 0) differ, unknown | [apple-RE, sim]: Apple's 6-tap centre j needs it (bench R48E); [silicon] 2026-10-25 as modelled (R48E, session log `bcmtest.log`) |
| B scalar | hw4 `[15]` = 1: B = r(hw4 `[14:12]`) + signed 12-bit offset | [apple] |
| B immediate | I = 1: imm16 = hw4 `[15:10]` : hw3 `[9:0]`, signed (the `vmulhr` family reads it unsigned). 48-bit: **imm6, signed** | [apple]; [silicon] S4 (imm6 52 wrote −12) |
| +rN | The register holds a VRF offset y·64 + x, added **field by field**: x wraps mod 64 without carrying into y. Only r0–r6 are reachable (7 = none). One form, `vmov.1 d0r, a000, b00.t+r3` (16-bit B, +r as a lane offset into an H row), behaved on silicon as if +r were ignored, while Apple's 16-bit B operands with +r work; the rule is open (`quirks.md` #17). On an **H16 A** operand, +r's x is **not** applied as given on silicon (quirks #30: vc2sim `v_a16_reg_x`, `dbl16`); on an **H8 A** it is (the IDCT check's `jv_put`, [silicon, indirect]); +r's y is applied on A (REPA, R48E). A **16-bit horizontal D** at +r has its x aligned down to 16, as a horizontal B's is: Apple's `vc_interp16xN_jvert` writes `d25.01+r0` with x = 2 or 3 and reads the row back at x 32. On silicon an H16 A operand's x is **not** applied as given (quirks #30) | [apple] (control: 33); [silicon] S18 (one form); **A's +r [silicon] 2026-10-25 (session log `bcmtest.log`): not as modelled** (ASHF/ASHG, quirks #30); 16-bit D [apple-RE, sim]; [silicon] 2026-10-25 as modelled (D16R) |
| inc | hw4 `[0]` steps **D** by one row (H) or column (V) per REP | [apple] (control: 618) |

### Modifiers (hw5 of 80-bit operations)

| Bits | Field | Evidence |
|---|---|---|
| `[7:5]` | REP: 0 none, 1 ×2, 2 ×4, 3 ×8, 4 ×16, 5 ×32, 6 ×64, 7 = r0 | [apple] high |
| `[14]` | SETF | [apple] high |
| `[13:11]` | Predicate: none, `ifz`, `ifnz`, `ifn`, `ifnn`, `ifc`, `ifnc` | [apple] high for `ifnz`, `ifn` |
| `[10]` | `clra`: clear the enabled lanes' accumulators first | [apple] high |
| `[9]` | `acc`: add the result into the accumulator's **high half**; the result written is the high half (A >> 16) | [apple] high |
| `[15]` | `x15`: add into the accumulator's **low half** (bit 0): the result's low 16 bits zero-extended, or, with `acc` too, the signed result; carries pass into the high half. The result written is the low half (A & 0xFFFF) | [apple-RE, sim] high; [silicon] 2026-10-25: X15M and X15Z as modelled; the signed `acc` form (X15S) is not (quirks #31) |
| `[4:3]` | Scalar-result function: 1 `sum`, 2 `min`, 3 `max`, 0 unknown | [apple] high (`sum`) |
| `[2:0]` | Scalar-result destination r0–r6 (7 = none) | [apple] |
| `[8]` | Unknown (never set) | — |

48-bit immediate form: hw3 `[5:0]` imm6 (signed), `[8:6]` REP, `[9]` SETF [apple; silicon S4].

**One repetition** executes as: enabled lanes from the predicate; read A, B; compute; `clra`; `acc`; saturate if the op saturates (to D's width: 0…255 for 8-bit, signed 16-bit); `setf` updates Z/N/C of **enabled lanes only**, from the full-precision result; the scalar-result unit reduces enabled lanes; write D's enabled lanes, truncated to D's width [apple].

## Operations (op6 = op9 `[8:3]`)

| op6 | Name | Semantics | Evidence |
|---|---|---|---|
| 4 | `vmov` | D = B | [apple]; [silicon] |
| 8 / 9 | `vadd` / `vadds` | A + B; `vadds` saturates to D's width | [apple]; [silicon] (`vadds` into 8 bits clamps 0…255, S5) |
| 10 / 11 | `vsub` / `vsubs` | A − B | [apple]; [silicon] |
| 12 | `vrsub` | B − A | [apple]; [silicon] |
| 14 | `vmul` | A × B, low bits (products past 16 bits keep their low 16 bits [hypothesis]) | [apple]; [silicon] S4 onward (by a register, with REP and `acc`) |
| 16 / 17 | `vmulm` / `vmulms` | (A × B) >> 8; 17 saturating | [apple] |
| 20 / 21 | `vmulhr` / `vmulhrs` | (A × B) / 65536 rounded, B unsigned. Ties (exactly −0.5, +0.5) round **away from zero**. `vmulhrs` into an 8-bit D does **not** clamp: it keeps the low 8 bits | [apple]; [silicon] S5 (no clamp), S6 (ties, for `vmulhrs`; [hypothesis] that `vmulhr` shares it) |
| 24 | `vshl` | A << B | [apple] |
| 26 | `vasr` | arithmetic A >> B | [apple] |
| 27 | `vlsr` | logical A >> B on A's width | [apple] |
| 28 / 29 | `vmin` / `vmax` | signed | [apple]; [silicon] S4 onward |
| 30 | `vdist` | \|A − B\| | [apple] |
| 37 | `vclip` | clamp A to 0 … B | [apple-RE, sim]: Clip1 of Apple's centre j (min-only fails every case), Clip3(0, 51, ·) of the QP index; [silicon] 2026-10-25 as modelled (VCLP, session log `bcmtest.log`) |
| 18 | `vmulhd` | (A × B) >> 16, floored, both signed: the high half of a product whose low half `vmul … x15` adds | [apple-RE, sim] (a rounded high half fails 45 of 60); [silicon] 2026-10-25 as modelled (X15M, MHDU, session log `bcmtest.log`) |
| 19 | `vmulhdsu` | (A × unsigned B) >> 16, floored | [apple-RE] medium (16.16 scales); [silicon] 2026-10-25 as modelled (MHDU) |
| 51 | `vmulhduu` | (unsigned A × unsigned B) >> 16, floored | [apple-RE] medium (one site); [silicon] 2026-10-25 as modelled (MHDU) |
| 44 | `vbitplanes` | lane i flag ← bit i of B (16-bit lane mask); with `setf` drives predication | [apple]; [silicon] |
| 52 | `vmulhru` | (A × B + 0x8000) >> 16 on non-negative A | [apple] medium |
| 5, 6, 13, 31, 34, 42, 43, 53, 59, 61, 62 | unnamed | 34 is probably add with carry (it follows a `vadd … setf` on the low words of 32-bit pairs); 42 reads a table (alpha/beta in Apple's deblocking); 59 computes a clipping bound in Apple's chroma deblocking | [apple-RE] low–medium |

Pairs: odd op6 is the saturating twin of the even one for 8/9, 10/11, 16/17 [apple, partly hypothesis]. For the multiplies, odd op6 is the **unsigned-B** twin instead: 18/19, 20/21 (`vmulhr` reads B signed, `vmulhrs` unsigned and does not clamp: quirks #11, #20), and op6 bit 5 makes A unsigned (51), as in the scalar `mulhd.ss/.su/.uu` [apple-RE; silicon for 20/21]. Op9 bit 0 (48-bit only) is an extra modifier, meaning unknown.

## The 32-bit accumulator

Each lane's accumulator is 32 bits, a high and a low half [apple-RE, sim]; [silicon] 2026-10-25: the halves, the carry-free forms and `vmulhd` as modelled (X15M, X15Z, MHDU), but not `acc x15` (X15S, quirks #31):
- `acc` adds the result to the high half and writes the high half. While the low half is 0 this is the plain accumulator every earlier kernel uses.
- `x15` adds at bit 0 and writes the low half: the result's low 16 bits zero-extended, or the signed result when `acc` is set too. Carries and borrows pass into the high half.
- `clra` clears both halves.

So a 16 × 16 → 32 product is two instructions, `vmul … x15` (low half) then `vmulhd … acc` (floored high half), and a chain of such pairs is an exact 32-bit multiply-accumulate; read the high half from the last op's D, the low half with `vmov …, #0 x15`.

Evidence, all in Apple's code (`docs/research/bcm2722/isa.md` has the table): the H.264 centre half-pel (`vc_interp16xN_jhoz`/`jvert`), whose 21-bit sums only come out right this way (vc2sim: 120 cases against the H.264 definition, all pass; each choice has a control); a 16.16 scale (`expand_range_16s`); a 16.16 ramp (vmcs.bin), which needs the carry from the low half; a 32-tap audio filter (vmcs.bin); a 32-bit planar multiply-accumulate (RenderServer.bin); and Apple's own VPU context switch, which saves and restores exactly the two 16-bit halves (`vmov …, #0 acc` / `vmov …, #0 x15`, restored with `clra acc` / `x15`).

**Our use: jdec's IDCTs** (4, 8 and 16 points, `jv_idct4n`, `bcmfw/video/gen_jdec.py`; `docs/research/bcm2722/jpeg-spike.md` §13-14) [sim]:
- **A whole dot product in one pair.** With REP 8, A (an H16 data row) and B (a V16 column of constants) both step, and every repetition adds into the same accumulator, as "One repetition" above describes: `vmul … x15 rep8` adds eight low halves, then `vmulhd … acc rep8` eight high halves. The low half's D (written by the last repetition of the first op) is final, because the second op only adds at bit 16; the high half's D is the second op's last. That is an exact Σ of eight 16 × 16 products mod 2³², in two instructions.
- **Reading both halves.** The jpeg islow's pass 1 keeps bits 11..26 of its sum: `(hi << 5) | (lo >> 11)` with `vshl`, `vlsr` and `vadd` on the two readouts. It needs no prescale of the inputs, so any int16 works, where the high half alone would give bits 16..31.
- **Seeding.** `vmov D, #imm clra x15` (or `vmov D, rN clra x15`, a term per output: the 4-point rounds two outputs with 1023) puts a rounding term in the low half (zero-extended, so it can be up to 65535), and `vmov D, #imm clra acc` one in the high half (514 << 16).
- **A constant per output, down a column.** One `vld16 … rep16` at a stride of 0 copies a row of 16 constants down 16 rows, so V16 column j holds constant j on every lane: a vector B of per-output constants with no scalar loads.
- **Wrap:** the readouts depend only on bits 0..31, so the kernel is exact whether or not the high half wraps at 16 bits.
- **On silicon** (2026-10-25): X15M, X15Z and MHDU pass (the halves and `vmulhd`), REPA's lanes 1–7 accumulate as modelled, and the display-only image's start-up check passed (`J_VPU` 5). But the accumulator **saturates** past ±2^31 (X15S, REPA lane 0; quirks #31, #32), where C's sum wraps, and the 16-point's eight products can reach 81,678 × 32,768. So **`jv_dot8` sums them in two REP 4 chains** (each at most 42,170 × 32,768 with the seed: under 2^31): chain 1 from the caller's seed gives its high half TH1 and low half TL1; chain 2 starts from TL1 alone (`vmov …, TL clra x15`), adds the other four, and writes the low half and a high half; `vadd` adds TH1 in wrapping 16-bit lanes. That is the sum mod 2^32 under every saturation rule, about 1% more instructions a call. The start-up check (`jdec_vpu.c` `idct_check`, `J_VPU_MAC`) compares four blocks with the C at start, rounding probes included, and now a column of 32767s that drives the 16-point's sum past 2^31, so a one-chain kernel fails it on a saturating chip.

**Our use: the visualizer's FFT** (`jv_fft256`, `jv_fft512`, `bcmfw/video/gen_viz.py`; `docs/research/bcm2722/visualizer.md`) [sim]:
- **A 16-bit value times 2^15 into the accumulator, exactly:** `vshl …, #15 clra x15` adds `(x & 1) << 15` to the low half (x15 zero-extends the result's low 16 bits), and `vasr …, #1 acc` adds `x >> 1` to the high half. Together they add `x · 2^15`, which a single multiply can't (32768 isn't a signed 16-bit B). The FFT's split step seeds `S · 2^15` this way and then adds two products, so `(S·2^15 + Wr·T + Wi·D) >> 16` is one floor of an exact sum.
- **A complex multiply by a scalar twiddle** is four ops a component: `vmul d_r, #wr clra x15`, `vmul d_i, #-wi x15`, `vmulhd d_r, #wr acc`, `vmulhd d_i, #-wi acc` (the last writes the high half). With Q15 twiddles the readout is `(d·w) / 2`: the stage's halving comes free.
- **`clra` acts on every repetition** ("One repetition" above), so an `acc` chain can't be REP'd across independent outputs: each output's chain is REP 1. Adds, subtracts and shifts between whole rows or columns are REP h.
- **Four-step on the 2-D VRF:** transforms down the H16 rows, then along the V16 columns of the same block. The transpose is free (the rows and columns alias), and every butterfly works on 16 independent transforms with no permute. The real FFT's partner pairing (`Z[m − k]`) needs a lane reversal, which the VRF only does as V16 column moves (16 single moves a component, then one REP-8/4/2/1 rotation).
- **Kinds:** `vmulhd` into a V16 D with an immediate and `acc` is not among Apple's (20 sites; `forms_test.py` counts it). The rest are Apple kinds. The image's start-up check (`viz_vpu.c`, `J_VPU_FFT`) keeps the C if any of them misbehaves.

**Our use: the visualizer's bars** (`jv_bars16`, `bcmfw/video/gen_viz.py`; `docs/research/bcm2722/visualizer.md` §5.1) [sim]:
- **Broadcast down the columns.** A per-row value, such as each row's gradient colour or its index, sits as one H16 row (lane k is row k's). One `vadd` into a V16 D stepping a column per REP, from that unstepped H16 A, copies it into all 16 columns of a block, so row k holds row k's value on every lane. This is the transpose of `vld16 … rep16` at a stride of 0, which copies a row down the rows.
- **Per-pixel select without predication.** Flags are per lane, not per lane and row, so a REP-16 op can't be predicated per pixel. A −1/0 mask `m = (a − b) >> 15` (`vsub`, `vasr #15`) with `X = C + (C − X)·m` (`vsub`, `vmul` by the stepped mask, `vadd`) keeps X where m is −1 and takes C where it is 0. A range test is `min(r − first, last − r) >> 15` (`vmin`).
- **Per-lane select with predication.** Where the condition is per lane only (a bar that covers the whole 16-row group, the background's lanes), `setf` on one REP-1 op (`vsub` into a discarded D, or `vbitplanes`), then one REP-16 move under `ifn` or `ifnz`.
- **Kinds:** one non-Apple kind, `vmin` 16-bit with a stepped 16-bit B at REP 16 (the deblocking kernels' family; `vmin` 16-bit has run on silicon since S4). Bench row BARV (`bench-p19.md`) runs one block through every form the kernel uses; the start-up check (`viz_bars_init`, `J_VPU_BARS`) keeps the C bars if any of them misbehaves.

**Settled [silicon] sitting 2 (2026-10-25 evening): `sat32`.** Bench ACSA read the `sat32` words exactly (`80017ffe 0bb57fff 0000ffff 0000ffff`, session log `bcmtest.1.log` and `bcmtest.log`, B4V 50); `read16` and `hi16` miss in the low halves (quirks #31). The visualizer's FFT (proposed, sim only) reads bits 0..31 and assumes wrapping; its sums can pass 2^31 with extreme inputs, so on silicon it relies on its start-up check until it is reworked like `jv_dot8`.

## Silicon check of the modelled forms, 2026-10-25 (build A)

Bench stage 4's vector checks (B4V 34 to 46), read from session log `bcmtest.log` (the same results again in the stage 4b run), the first run on silicon of these forms [silicon]:

| Check | Form | Result |
|---|---|---|
| X15M, X15Z | low-half add, zero-extended; `clra x15` | as modelled |
| MHDU | `vmulhd`, `vmulhdsu`, `vmulhduu` | as modelled |
| VCLP | `vclip` | as modelled |
| R48E | +r of a 48-bit op | as modelled |
| D16R | 16-bit horizontal D at +r (x aligned down to 16) | as modelled |
| RNDT, RNDU, MLSC | the rounding and scaled multiplies | as modelled |
| `vst16` REP 16 | store layout, rows 0 to 15, 128 words | as modelled |
| ASHF, ASHG | A operand's +r lane offset | **not as modelled**: an H16 A's +r x is aligned (quirks #30) |
| X15S | `acc x15` | **not as modelled**: the sum saturates (quirks #31) |
| REPA | REP 8 accumulating | accumulates; **saturates** past 2^31 (quirks #32) |

**Fitting the failures** (`bcmfw/tests/vpu_fit_test.py`): every check's instructions run in vc2sim under each variant of two new switches, against all 47 records' words. For an H16 A's +r x (`v_a16_reg_x`), `dbl16` (aligned down to 16 lanes, i.e. 32 bytes) and `near32` (nearest 32) fit; as given, aligned to 16 bytes and dropped do not [silicon]. For the accumulator (`v_acc_sat`), `sat32` (32 bits, saturating at every add), `read16` (wider, the high half saturated when written) and `hi16` (the high half saturating at every add) fit; the wrapping model does not [silicon]. vc2sim's defaults, `dbl16` and `sat32`, were the [hypothesis] choices; bench rows ASHX and ACSA (`gen_bench.py` `vpu_fit_cases`) give a different first record under each variant, and `readlog.py` names the one silicon follows. **Sitting 2 (build 799ff0f8d7, session log) [silicon]: ASHX reads `near32`, not the default `dbl16`** (`46034350 8d6eaadf a5ea4233 68512e4a`); **ACSA reads `sat32`**, the default. GTHR (the jobs' lane shift by two transposes and a `+r` y shift) passes, and so do ASHF, ASHG, X15S and REPA in this build's image (quirks #30, #31). PUTV (jdec's put and fill forms) still fails, informational. No check loads a scalar halfword, so sitting 1's scalar finding (`0x29`/o1 sign-extends) cannot move these words.

## Memory operations

op9 < `0x20`: kind = op9 `[4:3]` (0 load, 1 store, 2 `vldp`, 3 `vldq`), width/direction = op9 `[2:1]` (0 H8, 1 H16, 2 V8 — a V8 load puts memory byte i in row y + i of one column) [apple].

| op9 | Name | Evidence |
|---|---|---|
| `0x00` / `0x02` / `0x04` | `vld8` / `vld16` / `vld8v` | [apple]; [silicon] |
| `0x08` / `0x0A` / `0x0C` | `vst8` / `vst16` / `vst8v` | [apple]; [silicon] (`vst16`) |
| `0x10–0x14` / `0x18–0x1C` | `vldp*` / `vldq*`: the lanes of a misaligned access in the first / second 16-byte line. Apple issues `vldq` exactly when the bytes cross a line | [apple] (whether `vldp` alone fetches crossing lanes cannot be told from the corpus); [silicon] S2 (pair misaligned by 5 timed) |
| `0x170` (80-bit) | `vstr`: store A's enabled lanes (bytes) at r(base) (+= stride per REP); D supplies only A's t5 block | [apple]; [silicon] timed |
| others | rare, unnamed | — |

### Addressing (48-bit loads)

Address = r(base) + offset, the offset a signed 15-bit field in **8-byte units** [apple: control fails with 1, 2 or 16]. Each repetition adds r(stride) to the address and steps D one row (H) or column (V).

**Base and stride fields of 7 mean "no register" (value 0)**: a base of 7 addresses `0 + offset`, not r7 [silicon S15: a kernel using r7 as base wrote its scratch over SRAM `0x000–0x1FF`]. Only r0–r6 can be a base or a stride.

### Stores (48-bit): a different field layout

A 48-bit `vst16` does **not** use the load's fields the same way [silicon S5, reproduced word for word by a model in S5 and S6]:

| Field | A load reads it as | A store uses it as |
|---|---|---|
| hw2 `[15:8]` | D coordinate | address offset in **32-byte units** (0…8,160; unsigned) |
| hw2 `[4:0]` | offset bits `[14:10]` | the source's t5 block bits [apple only; not checked on silicon] |
| hw3 `[15:11]` | offset bits `[6:2]` | the **source row** within block 0 |
| hw3 `[10:9]` | offset bits `[1:0]` | unknown (0 in every store we have run) |

So `vst16` with "source row s, offset o" stores 32 bytes of H16 row s to r(base) + o. Apple's own stores agree [apple: three stores at `0x1E6B8` with hw2 `0006/0106/0206` store rows 48–50 to +0/+32/+64]. The earlier reading (store from D at the load's offset) made every store from a row other than 0 land at the wrong address [silicon S3–S5].

`vst8`'s layout has not been checked on silicon; store `vst8` only from row 0 at offset 0 until it is [silicon-derived rule]. A `vst16` with REP 16 and a stride steps the source row as a load steps D: [silicon] 2026-10-25 (session log `bcmtest.log`, build A): the store check writes rows 0 to 15, 128 words, as modelled.

### SDRAM

A vector row loaded from and stored to SDRAM (`0xFE…`) reads and writes correctly [silicon S11].

## Timing (silicon, S7)

| Operation | Cycles |
|---|---|
| ALU op, REP 16 | 16.25 |
| `vld16`, REP 16 | 16.63 |
| `vst16`, REP 16 | 16.50 |
| Single-row 48-bit store | about 1.5 |

So a vector instruction costs about one cycle per 16-lane row plus a small issue cost. Whether scalar instructions issue under a running REP and the exact per-instruction issue cost (fit over REP 1/2/4/16) are in the S7 log but not summarised here.

## Rules for writing vector code on silicon

1. Never use r7 as a memory base or stride; it reads as 0 [S15].
2. Encode 48-bit `vst16` with the store layout above [S5].
3. `vmulhrs` into 8 bits wraps; to clip, multiply into 16 bits with `vmulhr`, then `vadds` into an 8-bit D from a zero row [S5].
4. Expect ties of the rounding multiplies to go away from zero [S6].
5. The 48-bit imm6 is signed: values 32–63 are −32…−1 [S4].
6. Keep 32-byte alignment for 16-bit row loads and stores where possible: all silicon checks so far used it.
7. A misaligned row needs the `vldp`/`vldq` pair.

## Open

- **Picture mismatches on silicon.** Our H.264 decoder's vector kernels (transform, intra prediction, motion compensation, loop filter) disagree with their scalar C versions on silicon though they agree in simulation; the first differences sit at 16-byte boundaries (column 16 of rows 4 and 8) in several kernels [silicon S14–S16]. Suspected: a load or store width or alignment rule not yet modelled [hypothesis]. Until found, treat any kernel that crosses a 16-byte boundary within a row as unverified.
- hw3 `[1]` of an 80-bit B; t5 bit 3 in 80-bit ops; hw5 `[8]`; scalar-result function 0 (Apple's audio filter uses it to sum low-half accumulators); op9 bit 0 of 48-bit ops and the `x` forms' +r. Decoded, silicon pending: hw5 `[15]` (`x15`), ops 18/19/51/37, the 48-bit +r enables, a 16-bit D's +r x (bench X15M, X15S, X15Z, MHDU, VCLP, R48E, D16R).
- Unnamed ops (5% of Apple's vector bytes); the 6-tap filter's 80-bit loader form; 80-bit stores.
- 32-bit vectors (none known); memory width code 3 (`vmem06`, probably a V16 load).
- `vst8` and `vst8v` store layouts; store t5 block bits on silicon.
- The rule for an **A** operand's `+rN` x on silicon: it is **not** applied as given (bench ASHF/ASHG, 2026-10-25; quirks #30). Aligned like a horizontal B operand's is the [hypothesis]; the jobs' lane shift stays on the C path until it is settled.
- Whether the VRF is shared by any other unit, and the cost of saving/restoring it (Apple's runtime "checks out" the VRF per task).
