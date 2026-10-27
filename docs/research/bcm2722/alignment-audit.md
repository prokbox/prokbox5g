# VC alignment audit (P19, 2026-10-25; branch `p19-vc-align`)

The owner's question: "Are we also checking alignment? Both chips need things aligned properly." The PP side is covered (a misaligned 16/32-bit access is a data abort on silicon, `../../pp5022-reference/quirks.md` #51, and the gate runs UBSan's alignment checks). This page audits the VC side: every image's workloads under vc2sim's strict alignment mode, a static pass over every image's listing, the differences from Apple's conventions, and the bench checks that settle each unknown. The rules themselves (what is known and unknown on silicon, path by path) are in [`../../bcm2722-reference/memory-access.md`](../../bcm2722-reference/memory-access.md) §11, "Alignment".

## 1. How

- **Strict mode** (`tools/bcmre/vc2sim.py` `STRICT_ALIGN`; `VC2SIM_STRICT_ALIGN=trap|log`). Traps (or counts) every access of ours whose silicon behaviour §11 marks unknown and lets the known ones run as modelled: a misaligned scalar access other than `ld` through `(rs)` or `(--rs)`; a plain vector load whose first row is not at a multiple of 8 (H16) or of its size (H8, V8), or whose later rows are off their size; a vector store off its size; a `vldp`/`vldq` REP whose stride moves the 16-byte line phase; a 16-bit `vstr` off its size or across a line; a DMA block with units under 4 bytes or an address off its width. Apple's code (a `.vll` or `vmcs.bin` in the simulator) is only counted, never trapped: its pairs are misaligned by design. Bench row ALGN's checks are exempt (`vc2sim.algn_exempt`): they are misaligned on purpose.
- **Scalar accesses were already strict.** vc2sim has always raised on a misaligned scalar access unless the exception model is on (only `hello_test`'s probe rows and the bench turn it on), so every workload that passes proves its scalar accesses aligned. Strict mode adds the vector unit, the pairs and the DMA model.
- **The run** (`tools/bcmre/align_audit.py run`): every `bcmfw/tests/*_test.py`, plus `bcmfw/h264/test/sim_test.py` on four corpus clips (B-frames, partitions, the 768 kbit/s iPod profile, slices), in `log` mode. The AAC decoder's own test (`bcmfw/aac/test`) needs `build/aac`, which no worktree has; the AAC code runs in `video_test` and `hostproto_test` through the merged image.
- **The static pass** (`align_audit.py static`): all 17 images (`mkdevtest.py`'s 14 and `mkvideo.py`'s video, merged and staged) built with listings; every vector memory op through a register base, every pair and `vstr`, every DMA transfer-information constant, and every short loop with a load and a store through post-incremented or pre-decremented bases.

## 2. Strict mode: the result

Every test passed in `log` mode (24 scripts; `video_test` alone ran 36 minutes) apart from three first-run faults of this branch's own, all fixed and rerun: `jpeg_test` needed its progressive corpus linked; `bench_test`'s new ALGN row expected DMA results from a bus with no DMA model; `devtest_test`'s ctypes HAL lacked the new `port_algn` field (a crash). The rerun's one failure is `devtest_test`'s "embedded images match a fresh build": the bench images changed and the image batch regenerates `bcm-devtest-images.c`. **No plain vector access, no scalar access and no DMA block of ours trapped in any workload.** What did trap is two pair forms, both in shipped images:

| Site | Form | Where it runs | Tests that would fail under `trap` | Verdict |
|---|---|---|---|---|
| `jv_yuv16` (jdec's colour stage, `jdec_kernels.S`, display-only and merged images) | a **lone `vldp8`** REP 16 at a stride of 4: the gather of 16 pixels' Y/Cb/Cr | every JPEG the BCM decodes (covers, thumbnails) | `decode_image_test` (56,452), `jskip_test`, `jscan_test` | Unknown on silicon, and **installed without a start-up check**. Right in vc2sim by the pair's split rule (each row's 4 bytes lie in its first line). Bench ALGN **VPG4**; if it fails, the colour stage needs a check (as `idct_check`) or the C |
| The H.264 MC kernels (`h264_vmc_luma*`, `h264_vmc_chroma*`) on the **edge-emulation buffer** (`inter.c` `edge_copy`: a stack buffer at a stride of 21 for luma, 9 for chroma), and `disp_yuv_tile` at strides of 88 and 120 (176- and 240-wide frames) | `vldp8`/`vldq8` pairs REP 2–16 at strides that move the 16-byte line phase row by row | a motion vector pointing past the picture's edge (common), and any video whose width is not a multiple of 32 | `sim_test` (2,712), `disp_test` (512), `video_test` (about 59,000 through the merged and video images: the MC at strides 9 and 21, the colour tile at 88 for its 176-wide clip) | Unknown whether each row splits at its own line or at the first row's. The decoder was bit-exact on silicon from S27 on its clips (which likely included edge blocks; their widths are not recorded here), so probably right [hypothesis]. Bench ALGN **VPQS**; the cheap guard either way: an `edge_copy` stride that is a multiple of 16 (21 → 32, 9 → 16) |

**Strict mode as the tests' default:** not made the default, because the five tests above trap on these two forms (the instruction was to make it the default only if every test passes). `VC2SIM_STRICT_ALIGN=trap` runs every other bcmfw test clean; once VPG4 and VPQS pass on silicon, the two forms move into the known set (`vc2vpu._strict_vmem`) and `trap` can become the default (one line in `scripts/bcmfw_benches.sh`'s environment).

## 3. The sites

### 3.1 Dynamic (strict log, our code)

Produced by `python3 tools/bcmre/align_audit.py report OUT` after a run (per site: function, form, count, tests, first address). This audit's run, our code only, grouped: the `jv_yuv16` gather (one site, 56,452 executions in `decode_image_test`, 8,800 in `jskip_test`/`jscan_test` through their own kernel builds); the MC pairs on the edge-emulation buffer (24 sites in `sim_test`: `vldp8`/`vldq8` REP 2–16 at strides 9 and 21, addresses on the stack at `0x1eff9b4`–`0x1effb14`); the display tile's pairs (16 sites in `disp_test` at strides 88 and 120). Apple's code (the self-tests' `.vll` and `vmcs.bin` runs) counted 0 sites under the final rules: its pairs run at strides that are multiples of 16.

### 3.2 Static (listings)

| Kind | Where | What alignment it can see | Verdict |
|---|---|---|---|
| Plain `vld16`/`vst16`/`vld8`/`vst8` through a register base | 35 functions: the H.264 kernels (`h264_vpred*`, `h264_vidct4*`, `h264_vdbk_*`), the display path (`disp_box2_8/16`, `disp_yuv_tile`'s scratch), the job kernels (`jv_mix16`, `jv_shade16`, `jv_key16`, `jv_group16`, `jv_gather16`, `jv_store16*`, `jv_rows`), jdec (`jv_idct4n`, `jv_put`, `jv_yuv16`'s dither row), the cross-fade (`xfade_blk`, `copy_blk`, `half_blk`), `pw_sumr`, `video_vpu`/`video_vrf` | Whatever the caller hands in. Every immediate offset is a multiple of 32 (the 48-bit form counts 8-byte units; none of ours is off 32 except `vld16 +16` rows that read the second half of a 32-byte pair, which round down by quirk #17a only if misused) | Under strict mode none trapped in the workloads (§2); the callers align: 16-byte macroblock planes and 32-byte-aligned pools and frames (`config.h`), job surfaces at 32-byte pitch for the drift class, the jobs falling back to C for out-of-phase rests |
| `vldp8`/`vldq8` pairs | 88 functions: the decoder's MC, intra and loop filters, and `disp_yuv_tile` (chroma and luma rows at any alignment) | Any base; REP strides from the picture's planes | Silicon-implicit at strides that are multiples of 16 (the decoder bit-exact S27; colour right on screen since S16). **At strides of 88 or 120** (`disp_test`'s 176- and 240-wide frames) **unknown**: bench ALGN VPQS |
| A lone `vldp8` REP 16 at a stride of 4 | `jv_yuv16` (jdec's colour stage, `gen_jdec.py`), display-only and merged images | A 4-aligned source: each row's 4 bytes in its first line | **Unknown on silicon**, and `jdec_vpu_init` installs the colour stage **without a start-up check** (only the IDCT is checked). Bench ALGN VPG4; if it fails, `jdec_yuv565` needs a check like `idct_check` |
| `vstr` | none in our code | — | — |
| DMA transfer words | `dma_lcd_start` (`0x1900_A0x1`: 4- or 32-byte units), the audio tone (`0x1000_A041`: 4-byte), the staged upload (`0x0102_5151`: 32-byte) | aligned frames, a 35 KB SRAM buffer, the 32-byte-aligned SDRAM window and slots | Known (§11.3). The static pass's two "1-byte unit" hits (`0x0903_2101`, `0x1D1E_5101`) are a channel config and a constant, not transfer words |
| Scalar copy loops | `memcpy` (`bcmfw/lib/string.c`: words only when both pointers are 4-aligned), `aac_memcpy` (words only when the two pointers agree mod 4, after a byte head), `memmove` (bytes), `memset` (byte head to 4), `_start`'s `.bss` clear, `aac_image_place`, `resize_*` (jpeg's scaler, `uint32_t` rows), `r3d_render`, `memprobe`, the H.264 C reference (`h264_mc_*_c`, `edge_copy`, `filter_mb`), AAC's `pns_decode`, `tns_decode_frame`, `ifilter_bank` | Every word loop runs on `uint32_t`/`int32_t` arrays or after an alignment test | Aligned; and any misaligned scalar access would have stopped the workloads (§1) |
| Casts of byte pointers to wider types | `h264/intra.c` `fill_row`, `fill_block` and the top-row copies (4-byte columns of 16-byte-aligned planes), `jviz.c`'s fill (a byte head to 4 first), `jdec_vpu.c` `yuv565` (word copy only when `dst & 3` is 0) | 4-aligned by construction | Aligned |

## 4. Differences from Apple's conventions, ranked

Apple's conventions are in `memory-access.md` §11.5 ([apple-RE]). Where ours differs:

| Rank | Ours | Apple's | Risk | Rule to adopt | ALGN check |
|---|---|---|---|---|---|
| 1 (high) | `J_SLOT_PITCH` gives every class but the drift's a pitch of w pixels, so an odd-width slot has its odd rows at 2 mod 4, and `bcmjob_upload`'s row path (and CR1/CR2's) writes them there: they land 2 bytes low (quirk #34; CR1's 301 × 7, 906 bad pixels on both PP paths). CR2's 301 × 7, direct and staged, at pitch 602 too | Host image pitches rounded up to 4 bytes (osos 0x10286a1c); VC image pitches to 16 pixels, 32 bytes (vmcs 0x4a76) | Silent corruption of any odd-width slot (an odd-width cover, a strip, text) | **R-A1/R-A2:** every slot row 4-aligned, pitch a multiple of 4 bytes (32 where vectors walk it: round16(w) pixels, as the VC's own images); assert a 4-aligned start in the port layer, and merge a 2-mod-4 head by read-modify-write. In hand on `p19-image-batch-2` (the `ALLOC` pitch and the PP writer) | PWD2, PHD2, PAD2, R602 against R604/R608 |
| 2 (settled) | The odd last pixel goes by read-modify-write of its word (`bcm-port-block.h` `bcm_port_block`, under `dt_wr_block`; `write_row` and `cr_write_row` likewise): nothing past the row, no lone halfword. Until 2026-10-26 it went as a 32-bit word with a zero high half (2 bytes past the row), and `lcd_write_data` dropped an odd last halfword outright | A lone 16-bit halfword store, nothing past the end (osos 0x10287be8); the file service never writes past its range | Apple's lone halfword is **lost** on this unit (PLH1, PLH3, PLH5, sitting 2: quirk #35), so Apple's form is not adopted; ours writes only whole aligned words | **R-A3 (adopted):** never a lone halfword; an odd tail by read-modify-write of its word. vc2sim's port models lose a lone tail and count it (`hello_test.py` `Bench.write_bytes`, `tests/bcmjob`), so a writer that sends one fails | PLH1, PLH3, PLH5 (lost), PLW3 (the old word tail lands); R64W not run |
| 3 (medium-low) | After an engine-2 block the next port access (the tail's address) waits only for the engine's end, then the port's write-ready poll that `dt_wr`/`bcm_video_wr` do anyway | Each DMA piece waited to completion before the PIO tail and before the next address (osos 0x10286fb4) | A tail overtaking the engine's last words, if the port's write-ready does not cover them | **R-A4:** wait for engine 2's end and the port's write-ready before the tail or any new address (`vdma_wait_end` already does both; keep it so) | E2ND against E2DR |
| 4 (low) | Port writes of any 4-aligned range and length into memory the VC is using (rings, slots, the host frame) | The file service and the rings keep every port write 16-aligned and a multiple of 16; heads and tails under 16 bytes travel in the message | None seen (CR1: every region and form exact) | **R-A5** (optional): keep it; revisit only if a check below fails | P164, P168, P16C |
| 5 (low) | Slot heights exact | The VC's images round the height up to 16 rows (vmcs 0x17c48) | A 16-row vector kernel over an exact-height slot could read or write past it [hypothesis]; the job kernels bound their rows today | **R-A6** (optional): allocate round16(h) rows for slots a 16-row kernel walks | — (a review, not a bench row) |
| 6 (low–medium) | Pair loads at row strides that move the line phase: the MC's edge-emulation buffer (strides 21 and 9) and the display tile for widths that are not multiples of 32 (88, 120) | Never: Apple's strides are multiples of 16 | Wrong pixels in blocks whose motion vectors cross the picture's edge, and in non-32-wide videos, if the split follows the first row | Give `edge_copy`'s buffer a stride of 32 (luma) and 16 (chroma), and round plane strides up to 16 bytes; cheap either way | VPQS |
| 7 (low) | A lone `vldp` gather at stride 4 (`jv_yuv16`), installed without a start-up check | `vldp` only with its `vldq`, plain loads only when 32-aligned (vmcs 0x1d080) | Wrong colours in every JPEG the BCM decodes, if the lone `vldp` does not load the first-line lanes | A start-up check for `jdec_yuv565` like `idct_check` | VPG4, VPGX |
| — | LCD rectangles: x and width rounded even, 4-byte units | Any x and width in 2-byte units; Apple itself pushes whole rows | None: ours is stricter | — | — |
| — | Bitstream packets 64-byte aligned | The VC pulls the bitstream through 16-byte file-service writes | None: ours is stricter | — | — |

## 5. The bench checks

Bench row **ALGN** (`bench-p19.md`, "Planned silicon row: alignment"): at the end of stage 4, never failing it. Part 1 runs 41 checks on the bench image (`bcmfw/bench/algn.S`, `algn.c`): scalar loads and stores at +1/+2/+3 in every form, plain vector loads and stores at +2/+4/+8, a REP at stride 40, the two lone-`vldp` forms, a pair at a phase-moving stride, and DMA channel 7 at 32-, 4-, 2- and 1-byte units; each fault is caught by the image and recorded, a hang named by the host's 2 s timeout. Part 2 runs 20 host-port checks (the coordinator's ALGN1 to ALGN6): writes at +0/+2/+4 by words, halfwords and Apple's forms, 16-byte writes at +4/+8/+12, lone halfwords followed by other writes, 301 × 7 at pitches 602/604/608 by engine 2 and a tail, engine 2 then at once a new address, and rectangles at x 1 by read-modify-write. `readlog.py` (via `algnlog.py`) names the reading each fits; `bench_ingest.py` reports them. Adding a check: a routine and a table line in `algn.S` (or a `pa_cases` row in `bcm-devtest-core.c`) and its readings in `algnlog.py`.

**Expected lines** (vc2sim's model for part 1, the §11.4 rules for part 2):

```
ALGN 0 LW1C 00010103 …     faults (vector 1) after reading rounded down, 4 later instructions ran   (the control, S2/S3)
ALGN 20 VL68 00000001 …    reads rounded down to 32                                                 (the control, #17a)
ALGN 36 D32C 00000001 …    copies exactly in N us                                                   (the control, PW8)
ALGNP 0 PWD0 bad 0 low2 0 of 144                                                                    (the control)
ALGNP 1 PWD2 bad ~129 low2 ~128 of 144 …   lands 2 bytes low: the address's bits 1:0 ignored       (quirk #34)
ALGNP 12 R602 bad 906 …    rows 1, 3, 5 two bytes low;  R604, R608: bad 0
```

The rest are the open questions: each line names the reading silicon gives.

## 6. Open after this audit

- Every **unknown** in `memory-access.md` §11 has its ALGN check; none has run on silicon yet (sitting 2).
- **Strict mode as the bcmfw tests' default:** waits for VPG4 and VPQS (§2).
- The AAC decoder's C under strict mode beyond what the merged image's tests exercise (no `build/aac` corpus in any worktree).
- vc2mmio takes a DMA channel's config bit 0 as its enable, but PW8's silicon word for channel 7 (`0x0000_E802`) leaves it clear and works: `jobs_image_test.py`'s staged rows patch bit 1 in as the enable (peek_test's [hypothesis]); the bare model refuses the start. A model fix, not an image one.
- **The host-port model honours a 2-mod-4 write address**, so `jobs_image_test.py`'s staged and direct 301 × 7 rows pass in simulation while CR1/CR2 fail on silicon. The tests' port model should drop the write address's bits 1:0 (quirk #34) so the sim shows what silicon does; left for the image batch, which owns the writer fix (`p19-image-batch-2`).

## 7. The PP's port writers against sitting 2 (2026-10-26, branch `p19-sim-trust`)

**The two port rules** [silicon, sitting 2, `bench-20261025-2257` `bcmtest.1.log`, bench row ALGNP]:

- **Bits 1:0 of the write address are ignored.** PWD2, PHD2 and PAD2 (word data, halfword data, Apple's forms, all at +2) each read `bad 129 low2 128 of 160`: the block lands 2 bytes low. P164/P168/P16C (16-byte blocks at +4/+8/+12) are exact.
- **A lone trailing halfword is lost.** PLH1, PLH3 and PLH5 (1, 3 and 5 halfwords by 16-bit data writes, then a word at a new address) each read `bad 1`: the last halfword keeps the fill (`+20 f010` where `1000` was written; `+24 f012`, `+28 f014`). PLW3 (three halfwords sent as a word plus a zero-padded word) is exact. Apple's own writer sends a lone 16-bit tail ([apple-RE] osos 0x10287be8, `host-write-path.md`), which this unit loses; ours never sends one.

**The PP's own limit:** `lcd_write_data` (`lcd-as-video.S`, Rockbox's) handles an even count from a word-aligned source only. An odd count never sends its last halfword (the loop tests bits 16, 8, 4, 2); a source at 2 mod 4 is read from the word below (an ARM7TDMI `ldm` ignores bits 1:0), so every pixel goes one early.

**The fix, at the lowest layer:** `dt_wr_block` (every CPU block write: the device test, the job driver through `jd_wr_block`, the video player through `bcm_video_wr_block`) now goes through `bcm_port_block()` (`firmware/target/arm/ipod/video/bcm-port-block.h`): a mid-word address refused (as before, counted in `dt_half_writes`); the even part under one address, by the fast loop from an aligned source or word by word built from halfword pairs; an odd last halfword merged into its word by a read and a word write (nothing past the block changes). `e2port_start` now refuses a byte count that is not a whole number of words (every caller already checked `e2port_fits`). `bcm_video_wr_variant` (CR1's Apple forms) refuses an odd count. `lcd_blit_yuv` (plugins only) starts an odd x at the next even column (`HAVE_IPOD_UI` builds).

**The models:** `tests/bcmjob`'s port stub and `bcmfw/tests/hello_test.py`'s `Bench.write_bytes` (shared by bench, devtest, jobs_image, lab and peek tests) drop bits 1:0 and lose a trailing halfword, counting it; `tests/bcmjob` and `hello_test` fail on any count. `apps/video/vbcm_model.c` (the player's model) is stricter: it refuses an odd count, an unaligned address or source (the `bvp_port` contract). `tests/bcmport` runs `bcm_port_block()` against a port model with both rules and an ARM-`ldm` `lcd_write_data`: every count 0–19 at both source phases lands exactly; the old writer fails 87 of the same 120 blocks (the control).

| Writer | Count and alignment | Verdict |
|---|---|---|
| `dt_wr_block` (`bcm-devtest-hal.c`) | any n, any halfword source (before: n even and source aligned assumed, not checked) | **fixed** (`bcm_port_block`) |
| `jd_wr_block` (`bcmjob-dev.c`) | its unaligned loop dropped an odd last halfword and set an address per word | **fixed** (now `dt_wr_block`) |
| `vwr_block` / `bcm_video_wr_block` | CPU path: `dt_wr_block`; PP DMA path only for aligned source, `bytes & 3 == 0`; engine 2 only when `e2port_fits` | ok (CPU path fixed beneath) |
| `cr_write_row` (`vplayer.c`, CR1) | a 2-mod-4 row start merged by RMW, then `bcm_video_wr_block(a, s + 1, n)`: a source at 2 mod 4, so the old CPU copy sent every pixel one early (`ldm` from the word below) | **live bug in CR1's rows 1, 3, 5, fixed** beneath (a re-run of CR1 on batch 2's build would have shown it) |
| `write_row` (`bcmjob-core.c`) | the same shape through `jd_wr_block`, whose unaligned loop was right; odd ends by RMW | ok |
| `bcmjob_upload` / `bcmjob_upload_rect` runs | `bytes & 3 == 0` checked; whole 320-pixel rows | ok |
| command ring (`bcmjob-core.c` `put`, `bcmvp.c` `ring_write`) | `2 * run` halfwords of 32-bit words | ok |
| DECODE ring (`bcmjob-core.c` `dec_write`) | padded to whole words (`m = (n + 3) & ~3`, the pad zeroed); only the file's last piece is short | ok |
| bitstream FIFO (`bcmvp.c` `bvp_pkt_write`) | `hw = ((m + 3) >> 2) << 1`: whole words; `m` a multiple of 4 but for the packet's last piece; `wire.pace` 128 | ok |
| `vpcore.c` SDRAM check (512), audio block (`bytes / 2`, stereo frames), overlay rows (x and w forced even, rows from the 320-wide framebuffer) | even counts, aligned rows | ok |
| `aac_put` (`vplayer.c`) | multiple of 4 by contract | ok |
| warm swap, boot uploads (`dt_boot`, `dt_boot_how`, `bcm_video_boot`) | images padded to whole words (`mkdevtest.py`, `mkvideo.py`); `((bytes + 3) >> 1) & ~1` | ok |
| PB-6 TV BMP (`bcm-devtest-hal.c`, `bytes / 2`) | an odd byte or halfword of a BMP was lost | ok now (odd halfword by RMW beneath; an odd byte stays the BMP's padding) |
| `bcm_e2_cpu_copy`, DMA lab, LAB_STREAM, devbcm labs | multiples of 4 checked or constant | ok |
| engine-2 starts (`e2port_start`: `lcd_e2_push_min`, `dt_e2_upload`, `dt_port_algn`, coplab `dt_e2_job`, `vwr_block`) | all behind `e2port_fits` (whole words) | ok; `e2port_start` now refuses a partial word itself |
| `lcd_update_rect` | x and width rounded even; framebuffer rows aligned | ok |
| `lcd_blit_yuv` / `lcd_write_yuv420_lines` | width even; **an odd x landed every line pair one pixel left** (mid-word address) | **fixed** (`HAVE_IPOD_UI`); plugins only (mpegplayer, test_fps) |
| flash vmcs upload (`lcd-video.c`) | `((len + 3) >> 1) & ~1` | ok |
| device-test stages (`bcm-devtest-core.c`: fine/pattern/rect uploads, everyday rects, drift A/B, `pa_fill`) | x and w even in every table (rect 100, drift 160/300, everyday 320/260/52) | ok |
| `bcm_video_wr_variant` (CR1, Apple's forms) | n even by its callers (`2 * CR_WORDS`) | ok; now refuses an odd n |
| `dt_pa_cpu` (ALGNP), ALT lab | lone halfwords on purpose (the bench rows that measure the rule) | bench only, exempt |
