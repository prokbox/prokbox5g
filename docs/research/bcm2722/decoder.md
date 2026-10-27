# Our H.264 decoder (`bcmfw/h264`)

Issue #60, workstream 3 of `approach.md`. This is the first slice of our own H.264 Baseline decoder for the BCM2722's VideoCore. It is written from ITU-T H.264 in portable, freestanding C, proven bit-exact against ffmpeg on the host, and then run in `vc2sim`. No ffmpeg or JM code is in it: only the standard and well-known techniques (table-driven CAVLC, a cached bit reader, DC-only transforms, skipping bS = 0 edges).

**Status (2026-10-10, third session):**
- **Host:** all 14 corpus clips are bit-exact, I and P frames, under ASan and UBSan; 23,000 fuzzed streams, no crash and no hang.
- **VideoCore model:** **all 14 clips (212 frames) are bit-exact in `vc2sim` with the vector kernels on** (`--vpu all`, the device configuration), and the portable C build is bit-exact on the two profile clips.
- **Vector kernels (our own):** motion compensation, the inverse transform with its add, the loop filter, and now **intra prediction** (all nine 4x4 modes, 16x16 and chroma). Each is checked against its C reference in `vc2sim` (`test/vpu_test.py`). CAVLC stays scalar, rewritten this round (reader in registers, row-word tables, coefficients written directly).
- **Cost:** the iPod-spec clip needs **1.66 M instructions per frame** on average (2.24 M at the start of this session, 5.55 M at the start of the previous one), worst frame 2.07 M. I-only qp32 needs 2.09 M (from 2.89 M), textured intra (`a_ionly_mandel`) 3.23 M (from 5.44 M), and 640×480 4.20 M per P frame and 5.25 M per I frame (from 5.00 / 7.62 M). The budget at 150 MHz and 30 fps is 5.0 M cycles per frame; only VGA I frames are still over it (estimate 5.5 M cycles).
- **Device session 4's vector rules (2026-10-11):** a 48-bit `vst` lands only from `d00`, and the 48-bit ALU immediate (imm6) is signed (`isa.md`, "Vector stores on silicon"). Five kernels stored from other rows (the IDCT's coefficient clear, 16x16 Vertical, Horizontal, DC and Plane); each now writes its last result straight into rows 0.. (rows that were already consumed), so **no instruction and no REP row was added**: the iPod clip is 1,665,346 instructions per P frame and 1,718,602 per I frame before and after, VGA 4,202,364 / 5,255,807, every clip still bit-exact. No kernel uses a 48-bit ALU immediate (every immediate is the 80-bit form's 16 bits), so the signed imm6 changes nothing. `vpu_test.py` now runs under the silicon profile (a store from another row writes nothing), where the old kernels fail and the new ones pass.
- **Device session 5's rules (2026-10-11):** a 48-bit `vst16` lays out its fields differently from a load, and **`vmulhrs` into an 8-bit D does not clamp on silicon** (it keeps the low 8 bits; `isa.md`, "Vector stores on silicon"). The decoder's stores were already `d00` at offset 0, so they need nothing. Its clips did: the 6-tap rounding `clip((x + 16) >> 5)` (every luma MC phase but the whole sample and the centre j) and the 16x16/chroma Plane rows. Each is now `vmulhr #2048` into 16 bits, then `vadds` D8 = zero + B16, which clamps to 0..255 (the loop filter's form, `ADSN`, device-verified); the zero row is one `vmov` per kernel (`CLIP_ZERO8`, H8 row 63 x 32, free in every MC and Plane kernel). The averages (`vmulhrs #0x8000` of two 8-bit samples, and the 4x4 F2 filter) stay: their results are at most 255. Under vc2vpu's new default (no clamp) the old kernels failed 24 of 42 luma MC cases and 8 of the Plane cases; the new ones pass every `vpu_test.py` case and **all 14 clips (212 frames) are bit-exact** again. **Cost:** iPod clip P frames 1,665,346 → 1,665,841 instructions (+495, +0.03%) and 198,138 → 200,558 REP rows (+1.2%); I frames 1,718,602 → 1,718,630; VGA P 4,202,364 → 4,205,671 (+0.08%), I 5,255,807 → 5,255,836; mandel I 3,246,411 → 3,247,411. The centre j still ends with `vmulms #16`, whose clamp on silicon is not checked (Apple's 6-tap ends the same way; "Device checks", item 3).
- **Device session 6's rule (2026-10-11): `vmulhrs` rounds a tie away from zero** (−16 × 2048 gave −1; `isa.md`). vc2sim now does that for ops 20, 21 and 52 by default. **The decoder and display path don't depend on it either way:** every rounding multiply has a non-negative input (the averages, chroma, the loop filter's strong-filter sums) or is followed by the clamp to 0..255, where −1 and 0 both become 0 (the luma 6-tap, the Plane rows, the colour conversion). Under the new default all 14 clips (212 frames) stay bit-exact, `vpu_test`/`disp_test` (884 cases) and `video_test` pass, and the instruction counts are unchanged (no kernel changed). dev6's bench adds RNDT/RNDU for the rule and MLSC for item 3 below (the centre j's `vmulms #16` clamp).
- **In the image (`BCMFW_VIDEO=1`):** the decoder plays end to end behind the host protocol, with the RGB565 display path; see `pipeline.md`.
- **Device checks needed** before trusting the numbers on hardware: `vmin`/`vmax` and `vadd` 16-bit by a register (operation/width combinations Apple's code never runs in the self-test), 19 modifier combinations, negative-stride vector loads and stores, and the cycle cost of vector instructions. See "Device checks".

## Design

### Scope

| In | Refused cleanly (`H264_E_UNSUPPORTED`) |
|---|---|
| Annex B, NAL types 1, 5, 7, 8 (others ignored) | data partitioning (NAL 2–4) |
| SPS/PPS: profile-independent parsing; High-profile SPS fields if they're Baseline-compatible | CABAC, FMO (slice groups), weighted prediction, 8x8 transform, scaling matrices, interlace, not 4:2:0 8-bit |
| I and P slices, multiple slices, redundant slices skipped | B, SP, SI slices |
| I_4x4 (9 modes), I_16x16 (4), chroma (4), I_PCM | |
| P_16x16/16x8/8x16/8x8/8x8ref0, every sub-partition, P_Skip, up to 16 references | long-term references, MMCO 2/3/4/6 |
| reference list modification (short-term), sliding window, MMCO 1 and 5 | frame_num gaps (`H264_E_UNSUPPORTED`; ffmpeg would conceal) |
| loop filter: idc 0/1/2, alpha/beta offsets, constrained intra prediction, cropping | sizes above 640×480 |

### Files

| File | Stage | What it does |
|---|---|---|
| `h264.h` | | the public API: `h264_arena_size`, `h264_open`, `h264_decode_nal`, `h264_flush`, `h264_frame`, `h264_annexb_next` |
| `h264_dsp.h` | | **the swappable kernels' signatures** (intra, MC, IDCT, loop filter) |
| `h264_int.h` | | internal structs, the inline bit reader, `bs_vlc` |
| `h264.c` | parse | arena layout, emulation-prevention removal in place, NAL dispatch |
| `ps.c` | parse | SPS, PPS |
| `slice.c` | parse/picture | slice header, picture start and finish, reference lists, marking, concealment |
| `mb.c` | parse/mbctl | slice data, macroblock layer, residual parsing and dequantisation, neighbour caches, reconstruction calls |
| `cavlc.c` | parse | a macroblock's residual: every block's coeff_token, levels, total_zeros and run_before, dequantised straight into the blocks |
| `vlc_tables.c` | | **generated** by `gen_tables.py` from the standard's Tables 9-5, 9-7 to 9-10: row-word VLC tables and the 3-bit run_before table |
| `mvpred.c` | mvpred | median and directional prediction, P_Skip |
| `intra.c` | intra | 4x4, 16x16 and chroma prediction; with `H264_VPU_INTRA`, the wrappers that check availability and pick a vector kernel |
| `idct.c` | idct | 4x4 inverse transform + add, DC-only add, luma and chroma DC transforms |
| `mc.c`, `inter.c` | inter | 6-tap quarter-pel luma, eighth-pel chroma, edge clamping |
| `deblock.c` | deblock | bS derivation and edge filters |
| `tables.c` | | scan, dequantisation, chroma QP, CBP mapping |
| `gen_vpu.py` | | **generates** the vector kernels from VRF-position operands; `--check` fails if the output is stale |
| `vpu_asm.py` | | the generator's operand encoder: VRF positions to vc2dis field syntax, each instruction decoded again with `vc2vpu` and compared |
| `vpu_kernels.S`, `vpu_kernels.h` | inter, idct, deblock, intra | **generated**: 96 kernels (3,274 instructions, all straight-line), their dispatch tables and two 32-byte ramp tables |
| `test/vpu_test.py` | | each vector kernel against its C reference, in `vc2sim` |
| `test/vpu_forms.py` | | the vector and scalar forms in the kernels that Apple's code never runs in the self-test (`python3 -I`, needs the corpus) |

### How it works

- **NAL in, picture out.** The caller passes one NAL unit (no start code). The decoder removes emulation-prevention bytes **in place**, so there's no copy and no RBSP buffer. A picture is reported as `H264_FRAME` when its last macroblock is decoded. If a slice of a *new* picture arrives while one is unfinished (a lost slice), the decoder finishes and conceals the old one and returns `H264_FRAME_AGAIN`. The caller takes the frame and passes the same NAL again. The decoder remembers the NAL was already unescaped, so it isn't unescaped twice.
- **Bit reader.** A 32-bit cache that keeps at least 25 bits after a refill. A refill loads the next four bytes as one big-endian word and takes the whole bytes that fit (1 to 4); bits below the count may already hold the stream's next bits, which OR-ing in again doesn't change. Near the end of the RBSP a pure function returns the word with zeros past the end, so a reader copied into a local never has its address taken and stays in registers (the byte loads may alias anything, so a reader reached through a pointer would be stored back after every load). Bytes past the end read as zero, and `bs_overrun()` reports it. The position is a byte index, never a pointer. Exp-Golomb codes up to 25 bits take one `clz` (VC II `msb`, exercised in the self-test), a shift and a subtract.
- **CAVLC.** `cavlc.c` parses a whole macroblock's residual with the cache and count in locals; position, buffer and size are touched only by a refill, and a refill runs only when the next symbol could need more bits than are cached (coeff_token and the trailing-one signs 19, a level 22, total_zeros 9, run_before 11), so each takes more bytes. Every VLC code is split as (leading zeros, suffix); per table there is one 16-bit row word per leading-zero count, base << 5 | shift, and the entry `length << 8 | value` is `e[base + (w >> shift)]`: `clz`, one row load, one shift-add, one entry load. Unused words decode as value 255, which every caller range-checks. run_before for zerosLeft ≤ 6 is a 3-bit table (48 bytes), and above 6 arithmetic. Levels take prefix and suffix from one shift of the cache (the top lz + 1 + suffixLength bits are 1 << suffixLength | suffix), with the suffixLength threshold kept in a register. `gen_tables.py` checks each table is prefix-free with the expected Kraft sum, and decodes every bit string up to the longest code through a Python model of the C lookup; `run.sh` fails if `vlc_tables.c` is stale. Tables: 450 entries (900 bytes), 1,914 bytes with rows and run_before.
- **Macroblock state.** Each macroblock is parsed against an 8-wide cache of its neighbours' 4x4 blocks (TotalCoeff, intra modes, reference indices, MVs). Blocks not yet decoded are marked "not available", which gives the spec's partition-order availability for MV prediction's C neighbour. Coefficients are dequantised while parsing: each level goes straight to its raster position through a per-QP table of `v(qp % 6) << (qp / 6) << 4 | raster index` by scan position, rebuilt only when the QP changes (flat matrices). Products beyond 16 bits wrap; only a non-conforming stream has them, and the vector transform wraps there anyway. Per-block "coded" and "DC only" masks come with them. Then prediction and residual go straight into the picture, with no intermediate macroblock buffer.
- **Fast paths:** skipped macroblocks (no residual work; only their MV goes to the cache, and `store_mb` writes the rest), coded_block_pattern = 0 (only the TotalCoeff caches are cleared), blocks without coefficients (skipped), DC-only blocks (`(dc + 32) >> 6` added to 16 pixels), and loop-filter edges whose four bS are 0 (only the bS derivation runs).
- **Loop filter:** run over the whole picture once all its macroblocks are decoded, because intra prediction needs unfiltered neighbours. Per-macroblock info (QP, filter idc and offsets, slice number, non-zero mask, reference picture per 8x8, MVs) is kept for the whole picture.
- **DPB:** max(num_ref_frames, 1) + 1 frame buffers, so P lists are built from the short-term references by PicNum, then modified. Baseline has no B-frames, so **decode order is output order**: a picture is output as soon as it's finished. The POC is parsed but not used (see "Open items").
- **Vector kernels:** `H264_VPU=1` swaps the C kernels of `mc.c`, `idct.c`, `deblock.c` and `intra.c` for the vector ones behind the same signatures (switches `H264_VPU_MC`, `_IDCT`, `_DBK`, `_INTRA`). The host build is always C; `H264_VPU_REF` keeps the C versions as `<name>_c` for the tests. See "Vector kernels".
- **Errors:** every function returns a code. A slice with an error is dropped from that macroblock on, and its macroblocks are concealed at picture end (copied from the newest reference, or grey) and left out of the loop filter. Every loop is bounded by the macroblock count or by `bs_overrun`.

### Memory

No malloc. The caller provides one arena, and `h264_arena_size(mb_w, mb_h, refs)` sizes it. The context sits at its start (about 4.2 KB, 32-byte aligned so the coefficient blocks are), and the arena ends with 64 bytes of slack because the vector kernels read whole 16- or 32-byte rows. The buffers are laid out when an IDR activates an SPS, and a stream that doesn't fit gets `H264_E_NOMEM`.

| Item | Size | 320×240, 3 refs | 640×480, 3 refs |
|---|---|---|---|
| context (parameter sets, slice header, MB caches, coefficients, dequantisation tables) | about 4,350 B | 4.4 KB | 4.4 KB |
| per-MB info (58 B) + MVs (64 B) | 122 B/MB | 36.6 KB | 146 KB |
| frames: (refs + 1) × 384 B/MB | | 4 × 115.2 KB | 4 × 460.8 KB |
| **arena** (with the 64-byte slack) | | **501,664 B** | **1,993,856 B** |
| stack, deepest path (6-tap centre: 672 B temp, plus MC temps 2 × 256 + 441) | < 2 KB | | |
| image: code / tables (vc2cc, conservative), C kernels | 37,400 / 3,268 B | | |
| image: code / tables, vector kernels (the device build) | 57,474 / 3,646 B | | |
| VRF | the kernels use it as scratch; nothing lives in it between calls | | |

Everything fits `hostproto.md`'s map: the image in the 510 KB image region, the DPB in SDRAM's 2 MB DPB region (640×480 with 3 references just fits; 4 references wouldn't), and the context, macroblock info and stack in internal SRAM. **The split layout:** `h264_set_frames(d, frames, size)` puts the frames in their own region (32-byte aligned planes), and `h264_ctx_size()` / `h264_frames_size()` size the two parts; the video build keeps the context and macroblock info in SRAM (41 KB at 320×240, 151 KB at 640×480) and the frames in SDRAM (`pipeline.md`). `run.sh` decodes three clips in the split layout too.

## Results

### Host (`bcmfw/h264/test/run.sh`)

Each clip is checked frame by frame against ffmpeg's raw yuv420p, in a build with `-fsanitize=address,undefined` (Homebrew clang 23; Apple clang 17's ASan hangs at start-up on this macOS). A mismatch reports the frame, plane, pixel and macroblock.

| Clip | Frames | Result |
|---|---|---|
| `a_ionly_qp8/20/32/44/51`, `a_ionly_mandel` | 6 × 4 (I) | bit-exact |
| `b_ippp_gop10`, `b_ippp_mandel` | 30, 20 | bit-exact |
| `c_fastdecode` | 30 | bit-exact |
| `d_ipod_768k` | 60 | bit-exact |
| `e_vga` (640×480) | 10 | bit-exact |
| `f_slices` (4 slices, offsets, constrained intra) | 12 | bit-exact |
| `g_crop` (176×120) | 10 | bit-exact |
| `h_partitions` (down to 4x4, 4 refs) | 16 | bit-exact |

Macroblock types seen across the corpus: every P type (16x16 down to 8x8ref0, with 4x4 sub-partitions in `h_partitions`), I_4x4 and I_16x16 in both slice types, and reference indices above 0 in five clips. **I_PCM never occurs** (x264 doesn't emit it in Baseline), so that path is unverified.

Two bugs were found on the way: the Hadamard row order in the I_16x16 DC transform, and a wrong edge index in Vertical_Right intra prediction. The second was caught on review before the first run.

**Fuzzing:** `h264_test --fuzz N SEED` takes random corpus clips (a random prefix half the time) and applies 1–8 edits: bit flips, random, 0x00 or 0xFF bytes, random runs and truncation. 3,000 cases (seed 2722, in `run.sh`, after every step of this round) plus 20,000 (seed 1234, on the new CAVLC): **no sanitizer report, no crash, no hang**. The worst case took 78 ms (mean 7 ms) on the host. UBSan caught one left shift of a negative value in the first version of the new level decoder (the value was right; the shift is now unsigned).

### VideoCore model (`bcmfw/h264/test/sim_test.py`)

The same sources go through `vc2cc` (`-O2`, `--conservative`, `--no-float`, bcmfw's `memcpy`/`memset` as a library) and run in `vc2sim`. `--vpu all` (the default) builds the vector kernels, `--vpu none` the portable C decoder. Memory in the simulator's flat space: the arena at 0x800000, one NAL at a time at 0x1000000 (as the FIFO staging window would hold it), and the stack at 0x1F00000. Every frame is read back and compared with ffmpeg's yuv. A wrapper on `Sim.fetch` counts executions per address, and the map turns them into functions and stages (self time); vector instructions are also counted with their REP ("rows").

**All 14 clips are bit-exact in vc2sim with the vector kernels**: 212 frames, 44 I and 168 P. The C build is bit-exact on `d_ipod_768k` and `a_ionly_qp32`.

| Clip (vector kernels) | I frames: M instr/frame (max) | P frames: M instr/frame (max) |
|---|---|---|
| `d_ipod_768k` (320×240) | 1.71 (1.72) | 1.66 (2.07) |
| `a_ionly_qp8` / `qp20` / `qp32` / `qp44` / `qp51` | 2.07 / 2.24 / 2.09 / 1.75 / 1.63 | – |
| `a_ionly_mandel` | 3.23 (3.25) | – |
| `b_ippp_gop10` | 1.73 (1.87) | 1.48 (1.70) |
| `b_ippp_mandel` | 3.23 (3.32) | 1.68 (1.97) |
| `c_fastdecode` (no loop filter) | 1.61 (1.97) | 1.02 (1.13) |
| `e_vga` (640×480) | 5.25 (5.94) | 4.20 (4.46) |
| `f_slices` | 1.07 (1.09) | 0.84 (0.99) |
| `g_crop` (176×120) | 0.70 (0.78) | 0.39 (0.44) |
| `h_partitions` | 1.67 | 1.73 (1.95) |

The previous session's figures are in "Step by step" and in git history (`7dff829`).

The vector kernels themselves are checked one by one by `test/vpu_test.py`: each runs against its C reference on the same random inputs, and memory must come out identical inside and around the block, with the ABI checked on every call. Inputs are uniform random bytes, near-flat blocks (so the filters' conditions pass) and 0/255 extremes (every saturation, and the 6-tap's widest sums). With `--n 10`: MC 70 + 70 calls (every shape and phase, sample strides 9, 21, 32, 88, 160 and 320), transform 300 + 300 blocks, loop filter 400 + 400 edges (both directions, bS 0–4 mixed, alpha, beta and tC0 over Table 8-16/8-17's range), intra 120 + 120 + 120 blocks (every mode with random neighbour availability, so the -1 returns are compared too; strides 88 to 352). Intra was also run with `--n 100` (1,200 blocks per size, 0 failures), and a deliberate change to two intra kernels makes it fail. `run.sh` runs it (and `gen_vpu.py --check`).

## Per-stage profile

### Before this round (scalar C, 2026-10-10, first session)

Stages: **parse** (NAL, headers, CAVLC, macroblock syntax), **mbctl** (neighbour-cache load and store), **mvpred**, **intra**, **inter** (MC including edge clamping), **idct** (transforms and adds), **deblock** (bS and filters). Per macroblock means per coded or skipped macroblock of the picture.

| Clip, frame type | Frames | M instr/frame (max) | per MB | parse | mbctl | mvpred | intra | inter | idct | deblock |
|---|---|---|---|---|---|---|---|---|---|---|
| **d_ipod_768k, I** | 2 | 3.96 (4.31) | 13,194 | 5,902 | 1,073 | 0 | 1,517 | 0 | 1,596 | 3,096 |
| **d_ipod_768k, P** (51% skipped) | 58 | 5.60 (6.97) | 18,679 | 3,831 | 1,113 | 97 | 120 | 7,082 | 1,211 | 5,218 |
| b_ippp_gop10, P | 27 | 5.48 (6.53) | 18,258 | 2,639 | 1,111 | 93 | 102 | 7,688 | 1,060 | 5,558 |
| c_fastdecode, P (no loop filter) | 28 | 4.96 (6.09) | 16,528 | 2,161 | 1,112 | 106 | 47 | 12,132 | 953 | 9 |
| h_partitions, P | 15 | 5.53 (6.51) | 18,437 | 3,795 | 1,111 | 115 | 102 | 6,508 | 1,180 | 5,617 |
| f_slices, P | 10 | 3.53 (4.09) | 11,761 | 857 | 1,125 | 73 | 1 | 5,917 | 267 | 3,508 |
| a_ionly_qp8, I (no filtering at qp 8) | 4 | 4.23 (4.92) | 14,109 | 8,907 | 1,081 | 0 | 1,868 | 0 | 2,236 | 9 |
| a_ionly_qp32, I | 4 | 9.03 (9.15) | 30,110 | 4,049 | 1,075 | 0 | 1,588 | 0 | 2,023 | 21,367 |
| a_ionly_qp51, I | 4 | 8.32 (8.39) | 27,738 | 1,835 | 1,064 | 0 | 1,016 | 0 | 1,540 | 22,274 |
| e_vga (640×480), P | 8 | 23.51 (25.26) | 19,590 | 1,199 | 1,114 | 93 | 69 | 11,888 | 476 | 4,746 |

### Second session's end (vector MC, transform, loop filter)

| Clip, frame type | M instr/frame (max) | per MB | parse | mbctl | mvpred | intra | inter | idct | deblock |
|---|---|---|---|---|---|---|---|---|---|
| **d_ipod_768k, I** | 2.86 (2.97) | 9,548 | 5,902 | 779 | 0 | 1,517 | 0 | 428 | 741 |
| **d_ipod_768k, P** | 2.22 (2.91) | 7,395 | 3,831 | 739 | 97 | 120 | 649 | 287 | 1,482 |
| **a_ionly_qp32, I** | 2.89 (3.01) | 9,642 | 4,049 | 790 | 0 | 1,588 | 0 | 529 | 2,510 |
| a_ionly_mandel, I | 5.44 (5.47) | 18,141 | 9,363 | 806 | 0 | 4,110 | 0 | 1,181 | 2,510 |
| e_vga, I | 7.62 (8.53) | 6,353 | 2,640 | 777 | 0 | 1,335 | 0 | 383 | 1,038 |
| e_vga, P | 5.00 (5.34) | 4,166 | 1,199 | 737 | 93 | 69 | 491 | 155 | 1,237 |

### Now (third session: CAVLC rewrite, vector intra prediction)

Per macroblock, as above; "picture" (about 215) is left out.

| Clip, frame type | M instr/frame (max) | per MB | parse | mbctl | mvpred | intra | inter | idct | deblock |
|---|---|---|---|---|---|---|---|---|---|
| **d_ipod_768k, I** | 1.71 (1.72) | 5,708 | 3,343 | 785 | 0 | 323 | 0 | 293 | 743 |
| **d_ipod_768k, P** | 1.66 (2.07) | 5,533 | 2,206 | 596 | 98 | 25 | 666 | 230 | 1,497 |
| **a_ionly_qp32, I** | 2.09 (2.18) | 6,978 | 2,623 | 795 | 0 | 383 | 0 | 427 | 2,534 |
| a_ionly_mandel, I | 3.23 (3.25) | 10,780 | 5,369 | 812 | 0 | 647 | 0 | 1,207 | 2,534 |
| e_vga, I | 5.25 (5.94) | 4,371 | 1,820 | 782 | 0 | 268 | 0 | 238 | 1,044 |
| e_vga, P | 4.20 (4.46) | 3,496 | 758 | 565 | 94 | 15 | 499 | 99 | 1,250 |
| d_ipod_768k, P, C kernels (`--vpu none`) | 4.15 (5.47) | 13,843 | 2,206 | 596 | 98 | 116 | 5,429 | 1,153 | 4,028 |

Against the second session's end: **parse −42% on iPod P frames (3,831 → 2,206 per MB) and −43% on mandel (9,363 → 5,369)**; **intra prediction −84 to −79%** (1,517 → 323 on iPod I frames, 4,110 → 647 on mandel); P_Skip and the neighbour caches 739 → 596. `h264_residual` is now 1,795 of the 2,206 on iPod P frames: a level costs about 25 instructions, a run_before (with its coefficient write) about 31, a refill about 25.

### Step by step

Each step was measured on its own (`sim_test.py`, iPod clip average over 2 I + 58 P frames, and the qp32 I-only clip); every step was bit-exact on the host and in vc2sim.

| Step | Commit | iPod: I / P / mean (worst P) | qp32 I |
|---|---|---|---|
| start (scalar C) | 220688b | 3.96 / 5.60 / 5.55 (6.97) | 9.03 |
| MC: margins only where the filter needs them; chroma copy and 1-D paths | 43488aa | 3.96 / 5.20 / 5.16 (6.66) | 9.03 |
| bS: intra edges four at a time, nz masks first, MBF_UNIFORM | 6465811 | 3.73 / 4.94 / 4.90 (6.40) | 8.82 |
| **vector MC** | 8cc868b | 3.73 / 3.51 / 3.51 (4.44) | 8.82 |
| **vector transform + add** | 0f4bfb8 | 3.38 / 3.23 / 3.24 (4.11) | 8.37 |
| **vector loop filter** | 3c45f24 | 2.96 / 2.55 / 2.56 (3.25) | 3.22 |
| edge emulation by row spans | c12364e | 2.96 / 2.45 / 2.47 (3.13) | 3.22 |
| bS: one comparison per edge between uniform macroblocks | 51c6875 | 2.96 / 2.31 / 2.34 (2.99) | 3.22 |
| neighbour caches: word MV copies, uniform fill | ed62779 | 2.92 / 2.25 / 2.28 (2.93) | 3.19 |
| loop-filter glue: thresholds once per MB, uniform edges fast | fdd746c | **2.86 / 2.22 / 2.24 (2.91)** | **2.89** |
| **third session** | | | |
| CAVLC: reader in registers, word refills, row-word tables, direct coefficients | a27cdd3 | 2.13 / 1.81 / 1.82 (2.21) | 2.51 |
| cbp = 0 fast path, **vector intra prediction** (measured together) | ea0e617, 736f224 | 1.79 / 1.78 / 1.78 (2.18) | 2.16 |
| intra glue: availability table, 4-bit mode peeks, coded-bit loop | 7c3b4b1 | 1.72 / 1.75 / 1.75 (2.16) | 2.09 |
| P_Skip fast path, word MV fills, QP wrap without a divide | aea9e05 | 1.72 / 1.66 / 1.67 (2.08) | 2.09 |
| CAVLC levels from the leading one's bit index | 8ba81aa | **1.71 / 1.66 / 1.66 (2.07)** | **2.09** |

Inside the CAVLC commit the steps were: the new tables and direct coefficients 2.24 → 1.94 M (mean); the reader kept in registers 1.92; the whole macroblock in one function 1.90; refills only when needed 1.82. Every step was bit-exact on the host and in vc2sim.

(The vector-MC row was measured before the MC kernels switched to multiply-by-register forms, which add two scalar moves per call.)

## Vector kernels

### How they are built

- **Generated, not hand-assembled.** `gen_vpu.py` writes each kernel as a list of instructions whose operands are VRF positions (`H8(y, x)`, `H16`, `V8`, `V16`, with per-REP stepping and a `+rN` offset). `vpu_asm.py` turns them into vc2dis's raw field syntax (d8 coordinates, the 11-bit A field and its t5 block bits, B coordinates), assembles each one, decodes it again with the simulator's operand model and checks that the decoded operands are the ones asked for. An encoding mistake fails at generation time.
- **Our own code.** The kernels follow the H.264 equations directly. Apple's kernels were used only as proof that a data shape works on this VPU (e.g. 16-bit sums written to H16 rows, `vmulhrs` for rounding, setf chains for the filter decision), never as instruction sequences.
- **Straight-line leaf functions**, all 16 lanes at once, every row with REP (a 21-row pass is REP 16 + 4 + 1). The C side only dispatches: by block height and phase for MC (`H264_VMC_LUMA_TABLE`), by bS for the filter.
- **Memory access.** Loads of rows that may be misaligned are always a `vldp8` + `vldq8` pair (Apple's own form; it is right whichever way the line split works). Only the luma filter's horizontal edges and the 16x16 intra kernels use plain `vld8`/`vst8`, on rows that are 16-byte aligned by construction. **A 48-bit `vst8`/`vst16` is only ever from `d00`** (device session 4: from any other row it did not land); `vpu_asm.mem()` refuses anything else, so a kernel computes the rows it stores into rows 0.. . Stores of partial rows are `vstr` with a lane mask from `vbitplanes` (`vc_storeback`'s form), so `dst` needs no alignment. Kernels read up to 32 bytes past a row: the arena has 64 bytes of slack, and `edge_copy`'s buffer 32.
- **Forms.** Where it costs at most a scalar move, a kernel uses an operation form that Apple's kernels run in the self-test: it multiplies by a register rather than by an immediate or a shift, compares against 8-bit VRF rows rather than a scalar, and writes 16-bit results back to 8-bit samples as a saturating add to a zero row. `test/vpu_forms.py` lists what remains (see "Device checks").

### Motion compensation (`h264_mc_luma`, `h264_mc_chroma`)

- **Luma, layout.** The source rows go to VRF rows 0..h+4 at columns 32–63 (32 bytes from x − 2, or 16 from x when fx = 0; only h rows when fy = 0). 16-bit temporaries are H16 rows at x 32 (rows 21–52), 8-bit planes H8 rows at x 0.
- **Luma, the 6-tap** for 16 lanes and all rows at once: (E0 + E5) + 5·(4·(E2 + E3) − (E1 + E4)), all in 16 bits (the sum lies in −2,550…10,710), then `vmulhr #2048` = (x + 16) >> 5 into 16 bits and `vadds` to a zero row: the clip (device session 5: `vmulhrs` into 8 bits wraps on silicon instead of clamping). Horizontal taps are A operands at columns col + k (and one B operand at a 16-column boundary with `+r5`); vertical taps are rows.
- **Luma, the centre j** needs a 6-tap over the unrounded b1 values, whose sum doesn't fit 16 bits (up to about ±470k). The kernel splits b1 = 64·HI + LO (HI = b1 >> 6, LO in 0..63), runs the vertical 6-tap on each half in 16 bits, and combines them exactly: j = clip((tap(HI) + ((tap(LO) + 512) >> 6)) >> 4), the last step a saturating `vmulms #16`. This equals (Σ + 512) >> 10 for every input because the floors nest.
- **Luma, quarter samples** average two planes: `vadd` into 16 bits, then `vmulhrs #0x8000` = (a + b + 1) >> 1. Each of the 16 phases × 3 heights is its own kernel (48), 5 to 86 instructions (16×16 j: 75).
- **Chroma** is separable: H = (8 − fx)·A + fx·B per row, then (8 − fy)·H0 + fy·H1, then `vmulhr #1024` = (x + 32) >> 6 (equal to the 2-D formula). With one zero phase it's one pass and (x + 4) >> 3; with both, a copy. 12 kernels.

### Transform and add (`h264_idct4_add`, `h264_idct4_dc_add`)

One block per call, through the VRF's H/V aliasing. The 16 coefficients load as one 16-bit H row; three `vmov`s copy elements 4–7, 8–11 and 12–15 to the lanes 0–3 of the next rows. Now the 16-bit V columns 0–3 hold coefficient columns (lane = row) and the H rows 0–3 hold coefficient rows. The row pass runs on V16 columns, the column pass on the H16 rows of its result (the transpose is free), +32 is added once to row 0 (it reaches every output), then `>> 6`, a saturating add into the four dst rows, a masked store, and one 32-byte store of zeros (from row 0) clears `c`. 38 instructions against about 140 in C. The DC-only kernel computes (c[0] + 32) >> 6 in scalar code, puts it in a VRF row and does one saturating add: 14 instructions.

16-bit intermediates are exact for every conforming stream (8.5.12.1 bounds them). A non-conforming block with huge coefficients would wrap where the C code doesn't; the decoder's error handling doesn't depend on it.

### Loop filter (`h264_dbk_luma`, `h264_dbk_chroma`)

- **Lanes are the edge's lines.** A vertical edge loads one line per VRF row from pix − 4 and reads p3..q3 as V8 columns 0–7; a horizontal edge loads the rows across the edge and reads them as H8 rows. Chroma uses 8 of the 16 lanes. The same filter body is generated for both directions.
- **The decision** is a chain of `setf` tests narrowing the N flag: the lanes with bS > 0 (a mask from the C wrapper, via `vbitplanes`), then |p0 − q0| < α, |p1 − p0| < β, |q1 − q0| < β. The result is also saved as a −1/0 row, so later conditions (ap, aq, the strong-filter test) can narrow N again and N can be restored. Samples are written under `ifn`.
- **bS < 4.** ap and aq are −1/0 rows: (|x2 − x0| − β) >> 15. tc = tC0 − ap − aq (tC0 is one byte per lane, from the wrapper). Δ = (4(q0 − p0) + (p1 − q1) + 4) >> 3 is one accumulator chain of multiplies by registers, clipped with `vmax`/`vmin`, and added with saturation (q0 − Δ as q0 + (−Δ)). p1 and q1 get their clipped corrections under N narrowed by ap or aq.
- **bS = 4.** All candidate values (p0, p1, p2 strong; p0 weak; the same for q) are computed first as accumulator chains rounded by `vmulhr` (`#0x4000` = (x + 2) >> 2, `#0x2000` = (x + 4) >> 3, exact for non-negative sums), then written: the weak p0 under the filtered lanes, the strong values under N narrowed by ap and by |p0 − q0| < (α >> 2) + 2.
- **No unidentified operation.** Apple's normal-path kernel uses op 59 to compute tc and op 37 (`vclip`); ours uses neither. 8 kernels, 32 to 80 instructions.
- The C wrapper turns the four bS into lane masks (bS = 4 lines to the strong kernel, others to the normal one; an edge with four equal bS goes straight through), and tC0 into 16 bytes.

### Intra prediction (`h264_pred4x4`, `h264_pred16x16`, `h264_pred_chroma8x8`)

26 kernels, 4 to 52 instructions, one per mode and neighbour case; the C wrapper in `intra.c` checks availability (returning -1 as the C code does) and picks the kernel. Written from 8.3's equations; Apple's intra kernels (`vc_intra_4x4_*`, `h264_calculateintrapred16x16`) showed only that `sum`, V8 loads and column reads work on this VPU.

- **4x4, the edge.** E = p[-1,3..0], p[-1,-1], p[0..7,-1] (with e[13] = e[12]) in one H8 row: the left rows are loaded bottom up (a negative stride), so their column 3 reads in edge order, and the top row from dst − stride − 4 is read one column early. Without the above-right samples, the wrapper passes p[3,-1] and a lane mask for e[9..13].
- **4x4, the filters.** F1[i] = (e[i−1] + 2e[i] + e[i+1] + 2) >> 2 is one accumulator chain and `vmulhr #0x4000`; F2[i] = (e[i] + e[i+1] + 1) >> 1 a 16-bit add and `vmulhrs #0x8000`. Diagonal_Down_Left rows are F1[6+y..], Diagonal_Down_Right F1[4−y..], Vertical_Left F2[5..], F1[6..], F2[6..], F1[7..], and Vertical_Right F2[4..], F1[4..], then two rows whose lane 0 is patched under a one-lane mask.
- **Horizontal_Down and Horizontal_Up** alternate F2 and F1 lanes at half speed, which no window gives. Four V8 column writes (F2[k], F1[k+1], F2[k+1], F1[k+2]) make VRF row k exactly [F2[k], F1[k+1], F2[k+1], F1[k+2]], so the H/V aliasing does the interleave. Horizontal_Down's rows are those rows bottom up (row 0 patched in lanes 2–3), stored with a negative stride; Horizontal_Up runs the same filters over the left column top down with p[-1,3] repeated, where zHU's sequence is F2[z/2] (even z) and F1[(z+1)/2] (odd z), the tail values included.
- **DC** (4x4, 16x16, chroma per quarter with 8.3.4's rules) sums under a lane mask with the scalar-result unit (`vmov … ifnz sum rN`, the self-test's form), then fills rows from the register.
- **Vertical / Horizontal:** a row copied down with REP, or a column broadcast into 16 columns (`vmov` V8 stepping), read back as rows.
- **Plane.** H = Σ (x − 7)·p[x,−1] − 8·p[−1,−1] (chroma (x − 3) and 4) is one `vmul … sum` of a ramp row (a 32-byte table) by the top row, V the same with the left column. b, c and a are scalar. Rows are P0[x] = a − 7c + b·ramp[x] plus c·y: a 16-bit matrix of V16 columns, one `vadd` with REP, and the same clip as the 6-tap (`vmulhr #2048` in 16 bits, then `vadds` to a zero row). All intermediates stay within ±20,000.
- **Loads and stores:** 16x16 rows are 16-byte aligned (plain `vld8`/`vst8`), chroma and 4x4 use `vldp`/`vldq` pairs and masked `vstr`.

### Vector work in numbers

Per average iPod-clip P frame: 90 k vector instructions (about 300 per macroblock) covering 198 k REP rows. For qp32 I frames: 269 k instructions, 437 k rows; mandel I frames 337 k, 566 k.

## Against the budget

At 320×240 and 30 fps there are 9,000 macroblocks a second. At about 150 MHz (`hardware.md`) that's **5.0 M cycles per frame, 16,667 per macroblock**, for everything: decode, the display conversion and the host protocol.

- **Instructions.** The iPod clip averages **1.66 M per frame (33% of the budget at IPC 1)**; its worst frame is 2.07 M (41%). I-only streams cost 1.6–2.2 M at QP 8–51 and 3.2 M for the textured mandel clip.
- **Cycles, an estimate.** vc2sim counts instructions, not cycles. Assuming one cycle per scalar instruction and one cycle per 16-lane row of a vector instruction (REP n = n cycles; VC IV's documented VPU issue rate, not measured on VC II), the iPod clip needs about **1.77 M cycles per frame**, qp32 I frames 2.26 M, mandel I frames 3.46 M, and VGA 4.75 M per P frame and 5.53 M per I frame. SDRAM latency, the cache and DMA overlap are not in this model, and IPC below 1 for the scalar code (likely with SDRAM) would raise everything; a device run is the only real number.
- **Real footage (s9-int, 2026-10-12).** Stage 6's second clip (`bcmfw/video/stage6-real.mp4`, a 320×240 24 fps excerpt of the CC BY *Sintel* trailer at about 400 kbit/s; fast motion, dark textured scenes, five cuts), first 60 pictures in vc2sim, bit-exact: I pictures **2.33 M instructions** (worst 2.76 M), about 2.50 M cycles; P pictures **1.86 M** (worst 2.89 M), about 2.02 M cycles. The loop filter is a bigger share than on the synthetic clips (37% of P pictures). At 24 fps the budget is 6.25 M cycles, so this is about a third of it; at 27 MHz (no PLL) the same clip would need 75–110 ms a picture, which is why stage 6 runs it at 150 MHz.
- **Over budget (the remaining gap):** only 640×480 I frames, 5.25 M instructions (worst 5.94 M, about 5.5–6.2 M cycles) against 5.0 M; VGA P frames fit at 4.20 M (worst 4.46 M) but leave only 0.25–0.8 M for the display conversion and the host protocol. 640×480 input still wants a lower frame rate, or a 320×240 encode (the converter settings below). Everything at 320×240 fits, mandel I frames included (3.2 M).
- **What's left, in order (iPod clip, P frames, per MB):** CAVLC and macroblock syntax 2,206 (40%; `h264_residual` 1,795); loop filter 1,497, of which bS is about 470 and the C glue about 780; MC 666, mostly C glue (`h264_inter_pred` 260, edge emulation 265); neighbour caches 596; transform 230. For I frames: parse 3,343, intra prediction 323 (half of it glue), transform glue and the DC transforms (on mandel `add_residual` 360 and the luma/chroma DC dequantisation 420).
- **Next candidates:** the loop-filter glue (whole macroblocks per kernel call, bS in vectors); the CAVLC refill (a byte-swapped RBSP would make it one aligned word load, about 5% of parse on the iPod clip, more on textured intra) and the run_before loop (register pressure: GCC 6 spills the coefficient table pointer); the MC glue; batching transforms per macroblock.

### P19 vplayer health (2026-10-26): silicon against the simulator

Sitting 2 ran stage 6 at 150 MHz on the merged image `80bc8611` (`session log, bench-20261025`). The film clip's 302 FEED times against vc2sim's cycle estimate for the same pictures (all 312 decoded bit-exact, strict alignment on): **FEED = 1.3 ms + cycles / 125.7 per µs, r = 0.99** [silicon + sim]. The estimate is a trustworthy predictor: silicon does about 0.84 of an estimated cycle per clock, and each picture carries about 1.3 ms of conversion and panel start. Measured FEED: clip 1 (synthetic, 30 fps) mean 7.6 ms, p95 8.4, max 29.8; the film (24 fps) mean 18.5, p95 27.4, max 40.2 (its 19 KB key picture). Both meet the vplayer rule (p95 under 75 % of the frame: 25 ms and 31.25 ms).

The film's profile (vc2sim, per macroblock): P pictures 6,397 instructions, the loop filter 39 % (`filter_mb` 834, `strength_inter` 632, the vector kernels and their C glue), CAVLC 29 % (`h264_residual` 1,261), neighbour caches 11 % (`store_mb` 347, `load_neighbours` 343), MC glue (`h264_inter_pred` 256, `edge_copy` 205), `memset` 194; I pictures 7,976, `h264_residual` 2,816 (44 % parse). The heaviest pictures are big-residual ones (cuts), where the parser dominates.

**The next merged-image batch for FEED** (not built tonight: the merged image is frozen behind its re-verify, and its clock150 token names `80bc8611`; any change waits for the token again): bS in vectors for whole macroblocks and the filter's C glue folded into its kernels (about a third of a P picture); the CAVLC refill and `run_before` loop (above); `memset` of only the blocks a macroblock coded; the neighbour caches as word copies. With the fit above, each 1 M cycles saved is 8 ms off a picture on silicon. And for the pacing itself: a STATUS word carrying the chip's own show time per picture (the PP's drift is sampled at its polls, `vtrace.h`), then stage B's `CLOCK` and display queue (player.md).

**Batch 3 (2026-10-26):** done for the loop filter and CAVLC's setup (`merged-image-batch.md` §7): bS from the nzmasks a nibble at a time; one-bS and mixed-bS edges straight to the vector kernels; the luma nC from one pointer. Film clip P pictures 6,007 → 5,403 instructions a macroblock; the 30 fps recipe encode's p95 27.3 → 24.7 ms by the fit, the original clip's 24.4 ms; bit-exact on every clip. The SPS/PPS parsers went `-Os` for room (−5,376 B). Still open: the neighbour caches as word copies, `memset` of only coded blocks, the run_before loop's spills.

**Batch 4 (2026-10-26, branch `p19-dec4`):** C only, no new vector form, bit-exact on every clip. The film clip re-encoded by the converter's recipe at 30 fps: p95 2.946 → **2.552 M cycles, 24.7 → 21.6 ms** by the fit (mean 1.800 → 1.508 M, max 3.512 → 3.091 M); P pictures 5,395 → 4,423 instructions a macroblock, I pictures 8,976 → 7,845. The original 24 fps clip: p95 2.908 → 2.520 M (24.4 → 21.3 ms; mean 1.890 → 1.582 M, max 4.675 → 4.152 M); synthetic clip 1: p95 0.761 → 0.552 M (7.4 → 5.7 ms). What did it:
- **CAVLC:** the block decoder out of line (inlined five times it spilled everywhere and cost 5 KB; out of line it has its own registers: −4.4 KB and faster); `total_zeros` read straight from one table per TotalCoeff (`h264_tzd`, generated, 2.1 KB); a level refills the reader only when its code may not be whole in the cache; a dequantisation table per QP, built once (with adaptive quantisation the QP changes at almost every macroblock, and the rebuild cost 50 instructions a macroblock); the luma blocks an 8x8 at a time.
- **Neighbour caches:** `C()` puts x = 0 on a word and `h264_mbinfo` keeps its byte arrays first, so rows copy as words; no `memset` per macroblock (the current macroblock's MVs are left stale, intra macroblocks store zeros); P_Skip loads only its MV prediction's four neighbours; the Intra_4x4 modes are loaded only by Intra_4x4 macroblocks; a 16x16 partition writes four reference words and one MV.
- **Loop filter glue:** each edge out of line (`filter_edge`, six register arguments), thresholds and tC0 words cached per QP, the macroblock edge reusing them when the QPs match; `MBF_UNI8` (one MV per 8x8): the edges inside an 8x8 take bS from the coefficients only; a skip-to-skip edge without `strength_inter`.
- **MC and residual:** the edge window in the context (a 16-aligned stack array cost every `h264_inter_pred` a realigned frame) and only the rows and columns a phase reads; one residual pass over an inter macroblock's 24 blocks, offsets from a table built with the layout.
- Not kept (measured slower under GCC 6): word copies inside `edge_copy`, a constant-max copy of the block decoder (+1.5 KB for 9 instructions a macroblock), pairing `strength_inter`'s comparisons per 8x8.

Room: the merged image's image+bss ends at `0x332bc`, **8,516 B** below the arena at `0x35400` (was 6,020 B; merged image 214,772 → 212,276 B). The context grows by 4 KB of arena (the per-QP tables and the edge window), which the 320×240 layout has to spare.

**Still open (next batch):** the bit reader's refill (four byte loads; a word-swapped RBSP written by the unescape pass would make it one or two aligned loads); the loop filter in vectors (below); the MC glue (`h264_inter_pred` about 220 a macroblock); `read_mvd` and the macroblock layer.

**Memory, not cycles.** At 320×240 the DPB sits in the SRAM arena (`h264_frames_arena_first`), so MC's reference reads never touch SDRAM: about 1 KB of vector row loads per 16x16 partition, roughly 0.4 MB a picture, all SRAM. A DMA-striped reference window like Apple's `h264_refstripe_*` buys nothing there; it matters only when the DPB moves to SDRAM (640×480, or `HP_OPEN_SDRAM_DPB`): one macroblock row ±16 rows of three references at 640 wide is about 140 KB of arena (it fits, the frames having left it) and perhaps 1–2 KB of code (fits the room), with MVs beyond the window falling back to direct reads. What does touch SDRAM at 320×240 is the **bitstream**: `video.c` decodes each NAL in place in the FIFO, so the unescape pass and every refill's byte loads are uncached SDRAM accesses (roughly 20–30 thousand a heavy picture). Their latency is unmeasured (`open-questions.md` #7); unescaping into an SRAM buffer (the arena has about 100 KB spare at 320×240) would take them off SDRAM in the same pass. Worth a bench row before the next batch.

**The loop filter in vectors: the forms it needs (proposal, not built).** Whole-macroblock bS: 16 lanes = the 16 blocks (raster), from planar per-block arrays (`store_mb` would also write MV x and y as 16 halfwords each); the p side of a vertical edge is the same row shifted one lane, of a horizontal edge four lanes; bS 2 where either TotalCoeff byte is set, else 1 where the reference differs or |ΔMV| ≥ 4, as bytes stored for the C (or a fused kernel) to read a word per edge. Forms not in `tools/bcmre/proven_forms.txt`, each with a start-up check in the J_VPU style (a new `J_VPU_BS` bit: the kernel against `strength_inter` on built-in macroblocks, the C kept if any lane differs) and a bench row like B4V's:
1. `vld16` single at a 2- or 8-byte line phase (the shifted p rows; or a `vldp16`/`vldq16` pair). Check `BSL1`: load a 16-bit ramp at +2 and +8, store, compare all 16 lanes.
2. `vsub Ddiscard AH16 BH16 setf` (3 − |ΔMV| < 0 ⇔ |ΔMV| ≥ 4). Check `BSF1`: lanes of −5…+5 around 4, read the N flags back through a predicated move.
3. `vmov DH8 AH8 Bimm ifn` (bS 1 into the flagged lanes). Check `BSM1`: a known flag pattern, a background byte row, compare.
4. `vsub Ddiscard AH8 BH8 setf` with `ifnz` follow-up (reference bytes differ). Check `BSR1`.
5. `vmax DH8 AH8 BH8` (bS = max of the 2-from-coefficients and 1-from-motion rows). Check `BSX1`: random bytes against the C.
6. `vst8` single, no stride, at an offset (the bS bytes out). Check `BSS1`: store a ramp at offsets 0, 16, 48 and read back, the neighbours untouched.
|ΔMV| itself needs only proven forms (`vsub DH16 AH16 BH16`, `vrsub … Bimm`, `vmax DH16`). Folding the per-edge glue into one kernel per macroblock adds per-lane tC0 from bS (`vsub Ddiscard AH8 Bimm setf` then `vmov DH8 AH8 Breg ifz`; `ifz` itself is unproven: check `BSZ1`) and a scalar loop around the existing filter bodies. Expected: about 300 instructions a macroblock off the film's heavy P pictures (`strength_inter` and filter_mb's bS part), and as much again if the glue folds.

**The checks, built (batch 4, sitting 4):** `bcmfw/video/bschk.c` with `gen_bschk.py`'s seven kernels (`jv_bschk_l1` … `_z1`, one per form above, each reading its result out through forms silicon has run) writes the job block's `J_VPU2` (a bit per form), `J_VPU2_FAIL` (the first failing form and lane) and its got/expected halfwords (`jobs.h`); the bench's `jvpu` step logs them as `jvpu: forms …` lines and its verdict ignores them. All seven agree with the C in vc2sim (strict alignment on). They are **off in the shipped merged image** (`BCMFW_BSCHK` 0, config.h): vc2sim's strict forms mode refuses a form with no line in `tools/bcmre/proven_forms.txt`, so the image's tests would trap at `jobs_init` until the table gains a bench region `jv_bschk_*` and the seven forms as class bench, an edit left to the owner. Built with `-D BCMFW_BSCHK=1` the checks cost 1,840 B of room. The **load timings** need no new form and are in the merged image (`J_LAT_SDRAM`, `J_LAT_SRAM`, `J_LAT_MHZ`, ns × 10 a load over 8 KB of byte and word loads, at the first OPEN after SDRAM comes up; 384 B): the bench logs `jvpu: sdram byte … ns word … ns; sram byte … word … at … MHz`, which decides whether unescaping the bitstream into SRAM is worth a batch.

## Device checks

Our vector code is proven only against vc2sim's model, and the model is pinned only where Apple's kernels exercise it in the self-test. `test/vpu_forms.py` lists every form in `vpu_kernels.S` that no self-test row executes (static: the kernels are straight-line):

1. **Operation/width combinations never run in the self-test:** `vmin` and `vmax` (ops 28/29), 16-bit signed, used to clip Δ and the p1/q1 corrections (16 instructions; isa.md names them with high confidence from Apple's deblocking code, but that path is not in the self-test); `vadd` 16-bit + scalar register (the intra plane kernels, 4).
2. **Modifier combinations** on operations that are run (each modifier is proven on other operations); with item 1, `vpu_forms.py` lists 19 semantic forms: REP on `vmul` by a register (MC, 255 instructions) and on `vadd` with an immediate (15); `vadds` 8-bit + 16-bit under `ifn` (32); `vmov` from a register under `ifn` (8), `ifnz` (9, intra) and with REP (9, intra DC); `vsub … setf ifnz` (8); `vmul` into the discard destination with `acc` (16); `vmulhr` (16-bit and, for the 4x4 F1 filter, 8-bit results), `vrsub`, `vadd` with an immediate without REP or accumulator (45); `vld8` with REP (loop filter 2, 16x16 intra 4).
3. **Data ranges and addressing not seen in Apple's use:** `vadds` D8 = zero + B16 without a predicate, with REP (the MC and Plane clips; `ADSN` checked it under `ifn`); `vmulms #16` on the centre j's sums, which must clamp to 0..255 (session 5 showed `vmulhrs` does not; op 17 is unchecked); `vmul` by negative registers in accumulator chains (−1, −2, −4); 32-byte `vld16`/`vst16` of coefficient blocks and of the ramp tables (32-byte aligned); **negative strides** (the 4x4 intra left columns load bottom up with `vldp`/`vldq` and stride −stride, Horizontal_Down stores the same way); V8 column destinations written from H8 sources at arbitrary columns (Horizontal_Down/Up); `vmul … sum` of a 16-bit ramp by an 8-bit row (the self-test's form has a V8 column there).
4. **Scalar kinds:** none in the kernels. The decoder's C code (`--conservative`) executed 14 instruction kinds the self-test never runs at the second session's end (the scalar ones `mulhd.ss`, the 16-bit `cmp #imm5`, `max #imm5` and `asr rd, rs`, branches `gt`, `hi`, `le`, `ls`, `lt`, and indexed `ldsb`; the vector ones `vasr.1`, `vmulhr.0` by `.w` and `vmax`/`vmin`); this round's C was not re-scanned. The 16-bit `cmp`/`max #imm5` never existed: `7000`–`77ff` is `ld/st (--rs)` (`isa.md`, device session 1), and since the p14-bcm-re merge the assembler emits the 32-bit forms. `bcmfw/tests/forms_test.py` now scans the whole video image statically: no `7000`–`77ff` halfword, no scalar kind outside Apple's static code, and two vector kinds outside it (`valu80 vmax.1`/`vmin.1`, 16 sites in the deblocking kernels), which the bench checks on chip. The CAVLC uses `msb` (exercised) for every leading-zero count.
5. **Timing:** the cost of a vector instruction per REP row, of `vldp`/`vldq` pairs, of `vstr` byte stores, and the scalar IPC.
6. **Stores (device session 4).** Every 48-bit store is now from `d00`, the form COPY proved: still to see on chip are `vst8 d00` with REP 8/16 and a register stride (luma filter, 16x16 intra), `vst16 d00` of a single row (the IDCT clear) and, in the display path, `vst16 d00` with REP 16 and with an offset (`pipeline.md`). The masked stores are the 80-bit `vstr` (`vc_storeback`'s form: D `d00`, the source in A); the bench's check stores from A = row 0 only, while the kernels' A sits at other rows and columns (MC planes at rows 21/37/42 or x 32, IDCT rows 16–19 at x 32, intra rows 16.. and 32..): whether the 80-bit store has the 48-bit form's problem is open, and is the first thing to check (a `vstr` from A at a non-zero row and column). Where the 48-bit stores went is open too (`isa.md`).
7. **Signed imm6:** nothing to check for the decoder or the display path (no 48-bit ALU immediates); the bench's CSUM check covers the rule.

The timing bench image (`bringup.md`, "Device session 2") runs one on-chip check per form in items 1–2 and the data ranges of item 3: 33 checks now (session 4's six diagnostics included), including the display path's (`pipeline.md`, "Device checks"), whose forms `vpu_forms.py` now lists too.

## ProkBox-side impact

- **Demuxer:** MP4/M4V stores H.264 as length-prefixed NAL units plus `avcC` (SPS/PPS). The PP side has to:
  - send `avcC`'s SPS and PPS first;
  - pass each sample's NAL units one by one. Our API takes NALs without start codes, so length prefixes map directly and no Annex B conversion is needed.

  The decoder writes to the NAL buffer (it unescapes in place), which is fine for the FIFO. It reports frames in decode order, which equals display order for Baseline without POC reordering.
- **Converter settings (if we ship one)**, from the profile:
  - **exactly 320×240** (no cropping or scaling);
  - Baseline;
  - **one slice**;
  - `ref=1`–`3`: the DPB is (refs + 1) × 115 KB;
  - a keyint of a few seconds.

  With the vector filter, deblocking costs 20% of a P frame (it was 28% in C). `--tune fastdecode` removes it but costs quality, and the saving is now about 0.4 M instructions per frame, so keeping the loop filter is the default. A lower QP raises CAVLC cost (qp 8 I frames: 63% parse). Vectors that stay inside the picture avoid the clamping path. iTunes-era 768 kbit/s files decode with the same code; they just get no say in these choices.
- **Host protocol:** the decoder's codes map onto `hostproto.md`. `H264_E_UNSUPPORTED` at SPS/PPS/slice maps to `HP_E_UNSUPPORTED`, and the other errors are concealment, counted.

## Open items

1. **I_PCM** is implemented but has no reference test (no encoder in reach emits it in Baseline). A hand-built stream is needed.
2. **POC is ignored**: output is in decode order. That's correct for every Baseline encoder we know of, but a stream with `num_reorder_frames > 0` would come out in the wrong order. Parse the VUI's `max_num_reorder_frames` and refuse, or reorder.
3. **Refused, not decoded:** long-term references (MMCO 2/3/4/6, `long_term_reference_flag`), frame_num gaps (ffmpeg conceals them), FMO, and ASO across non-raster slice orders (it should work but is untested).
4. **Concealment is minimal** (copy the co-located block, no MV recovery), and a lost slice is only noticed at the next picture or at `h264_flush`.
5. **Instructions, not cycles.** vc2sim counts instructions (and, for the vector unit, REP rows). VC II pipeline timing, vector issue rate, SDRAM latency and DMA overlap are unmodelled, so the cycle figures above are estimates; real IPC needs a device run (`bringup.md`'s checklist).
6. **Instruction kinds:** checked (see "Device checks"): the kernels' scalar kinds are all in the self-test; the C code adds eight scalar kinds and the vector forms listed there. A coverage row in the bench (as hello_test has) is still to add.
7. **SRAM/SDRAM split:** done (`h264_set_frames`: context and macroblock info in SRAM, frames in SDRAM). Still open: DMA striping of reference rows into SRAM and row-lagged deblocking, if SDRAM latency (cache off) turns out to dominate on the device.
8. **Integration:** done. The decoder is in the `BCMFW_VIDEO=1` image behind `OPEN` codec 1 and `FEED`, with the RGB565 conversion and the DMA push; it plays end to end in the simulator, pixel-exact (`pipeline.md`).
9. **Gate:** `run.sh` (host, about 35 s; it now also runs `vpu_test.py` and `gen_vpu.py --check`) and `sim_test.py` on one I-only and one P clip (about 30 s with `--frames 6`) are candidates for `scripts/validate.sh`. Both need the corpus: `make_corpus.sh` takes about 2 s.
10. **Vector-kernel device checks:** "Device checks" above.
11. **Instruction-kind coverage of this round's C** (the new CAVLC and macroblock code) against the self-test: re-run the scan of item 4 under "Device checks".
