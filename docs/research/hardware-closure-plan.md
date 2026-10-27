# Hardware closure plan: from open questions to a trusted simulator

Written 2026-10-26 (branch `p19-closure-plan`, from `p19` at `799ff0f8d7`).

**The goal (owner):** "a solid simulation of the hardware done soon, so that we can confidently continue building the software on top of it, and know, with relative certainty, the silicon will behave as expected."

**Names:** PP5022 (CPU, COP), BCM2722 ("the BCM"); in chat PP, PP1, PP2, VC.
**Tags:** [silicon] measured on our unit; [silicon, inferred] fitted to silicon words; [apple] / [apple-RE] read from Apple's 1.3 images (own words, addresses only); [sim] vc2sim or the SDL simulator only; [ours] our design or rule; [hypothesis]; [estimate].
**Sources read for this plan:** both `open-questions.md` files, `bcm2722-reference/{quirks,isa-scalar,isa-vector,memory-access §11,boot-and-clocks,brief-map-power}.md`, `pp5022-reference/*`, `data-paths.md`, `research/bcm2722/*` (chiefly `bench-p19.md`, `warm-swap.md`, `jpeg-spike.md` §16, `battery-levers.md`, `merged-image-batch.md`), the simulator-fidelity audit on branch `simfid` (`docs/sim-fidelity.md`, not yet merged), Apple-RE notes in `~/Git/re/bcm2722/notes/`, and the sitting-2 results in `session log, bench-20261025` (ingested with `scripts/bench_ingest.py --commit 799ff0f8d7 --mkdevtest none`; treated as data).

---

## 0. The plan on one page

**Where we are.** Sitting 2 (2026-10-25, 22:33 and 22:43, build `799ff0f8d7`, merged image `80bc8611`) settled more than any session before it [silicon]:

- the vector variants: an H16 A operand's `+r` x is **`near32`**, not the modelled `dbl16`; the accumulator is **`sat32`**, as modelled;
- all eight halfword-load forms (HWLD) as modelled;
- almost every alignment unknown (ALGN): scalar misaligned stores, vector loads and stores off their row, DMA sub-word units, the port at 2 mod 4 in every form, and the lone trailing halfword (it is lost);
- the display jobs on the chip (drift, drill 2, window push B5), and 150 MHz video (4b, 4c, stage 6).

**What is still open that software depends on**, in order:

1. **Videos plays at 27 MHz in normal use.** That, not the decoder, is the most likely cause of the owner's "kinda laggy". Release builds can never reach 150 MHz as the code stands.
2. **The jdec put form (PUTV) is wrong on silicon,** which turns the display-only image's MAC path off (`J_VPU` 0x23).
3. **The warm swap (B13)** parks and then goes silent.
4. **Eight port alignment checks never ran:** R602 stopped the port half of ALGN.
5. **vc2sim's model lags tonight's readings:** `near32`, vector store rounding, per-row REP rounding, scalar store faults, the lost halfword, and DMA at a misaligned 32-byte unit.
6. **The power probes PW1, PW2 and PW7** failed for reasons in our own probes, not the chip.
7. **The clock bands (`vbv`)** wait for an image that carries them.
8. **The visualizer overlay's ops** (`SPECTRUM`, `BARS`, `ANIM`, and the new `POLYLINE`, `SEGMENTS`, `PALROW`) have drawn only in vc2sim.
9. **Hibernate (PW5, then HW1)** has never run.

**How it closes:** desk work first (Apple-RE lookups and fits against words already logged), one image batch (batch 3), then **two bench sittings**:

| Sitting | Owner time | What it closes |
|---|---|---|
| A, "re-verify and closure" | about 43 min: about 28 min watching or answering, about 15 hands-off | video health, PUTV/MAC, B13 diagnosis, ALGN port part, the viz overlay ops, the power probes, the bands, B6/B7, AAC and store-then-load |
| B, "power, sleep and graphics" | about 40 min: about 25 min attentive, each risky step behind its own go | lvl5, lit24, PW3/PW4, 5i/5s, PW5 → HW1, stage 3d with TE, a long real play |

Allow one short re-run sitting for anything that fails.

**"vc2sim is trusted"** is defined in §5 by four measurable gates:

- **E1:** strict modes on by default;
- **E2:** zero unproven, untrapped forms in any shipped image;
- **E3:** every silicon row's words reproduced by the sim in the gate;
- **E4:** the PP side's equivalents.

**Target: Friday 2026-11-06,** at two sittings a week. **Conservative date: 2026-11-13,** if a sitting finds a new class of quirk (§5.5).

---

## 1. Inventory

Each item has an id, what is known (with tags), whether tonight changed it, and the open question numbers (BCM `oq#`, PP `pp#`). Section 2 ranks them; section 3 says how each one gets settled.

### 1.1 Settled tonight: fold into the manual, then change the sim (no bench needed)

| Id | Item | Tonight's reading | Was |
|---|---|---|---|
| S-1 | H16 A operand's `+r` x (oq —, quirk #30; row ASHX) | **`near32`**: x rounds to the nearest 32 [silicon] (`bcmtest.log` B4V 49, both stage 4b runs: `46034350 8d6eaadf a5ea4233 68512e4a`, the `near32` row of `bench-p19.md`'s table exactly) | `dbl16` modelled (vc2sim default) |
| S-2 | Accumulator saturation (quirks #31, #32; ACSA) | **`sat32`**, as modelled [silicon] | three candidates |
| S-3 | The jobs' lane shift (GTHR) and `J_VPU` on the merged image | GTHR as modelled; merged `J_VPU` = 3 (checked, shift) [silicon]. The merged image has no `DECODE`, so no MAC bit is expected there | `J_VPU` 1 (sitting 1) |
| S-4 | Halfword loads (HWLD: `HL16`, `HS16`, `H28O`, `H29O`, `H48H`, `H48S`, `HSP0`, `HSP1`) | all eight as modelled [silicon]. The 48-bit op 2/op 6 forms and the sp-relative `2d4x`/`2d5x` forms are now proven | four [hypothesis] |
| S-5 | X15S, REPA (quirks #31, #32) | pass under the updated model [silicon] | FAIL in sitting 1 |
| S-6 | Visualizer bars (BARV) | as modelled [silicon]: `jv_bars16`'s forms are proven | [sim] |
| S-7 | Misaligned scalar loads, every form (oq #5; LW1C, LWR2–LWX2, LHR1–LHP1) | fault on vector 1 after reading the rounded-down address; 4 later instructions run; post-increment forms write the base back [silicon]. As modelled | [hypothesis] except LW1C |
| S-8 | Misaligned scalar stores (oq #5; SWR1–SHO1) | **fault on vector 1, imprecise** (saved pc +10 or +12), **and the store lands at the rounded-down address** [silicon] | modelled: a precise fault, nothing stored |
| S-9 | Plain vector loads off their row (oq #68; VL64, VL62, VL88, VL84, VL82) | `vld16` rounds down to 32, `vld8` to 16 [silicon], as modelled | [hypothesis] |
| S-10 | REP with a stride that is not a multiple of the row (VLS4) | **each row rounds down on its own** [silicon] | modelled: the base rounds once |
| S-11 | A lone `vldp` across a line (oq #21; VPGX); a lone `vldp8` REP at stride 4 (VPG4); a pair at a moving phase (VPQS) | `vldp` alone drops the lanes past its line (oq #21: no); VPG4 right; each pair row splits at its own line [silicon], all as modelled | [hypothesis] |
| S-12 | Misaligned vector stores (oq #68; VS68, VS64, VS62, VS88, VS84) and `vst8`'s layout (oq #2; VS80) | `vst16` at +2/+4/+8 and **`vst8` at +4/+8 both land rounded down to 32** (not 16 for `vst8`); `vst8` at +0 lands exactly [silicon] | modelled: stored at the exact address |
| S-13 | BCM DMA sub-word units (oq #69; D32C, D4W9, D2H3, D1B5) | 4-, 2- and 1-byte units copy exactly [silicon]: Apple's unit rule works | [apple-RE] only |
| S-14 | A 32-byte unit at an address that is not a multiple of 32 (D32M) | **copies wrong** (`13121110 17161514`) [silicon]. Apple never sets one up | modelled: exact |
| S-15 | Port writes at 2 mod 4 by halfword data, and in Apple's forms (oq #70; PHD2, PAD2) | **2 bytes low, like word data** (PWD2) [silicon]; 16-byte blocks at +4/+8/+12 exact (P164–P16C) | PWD2 [silicon, inferred] only |
| S-16 | **A lone trailing halfword** (oq #43; PLH1, PLH3, PLH5) | **lost**: the odd last halfword keeps the old fill when the next write goes to a new address (`+20 f010` where `1000` was written). PLW3 (our zero-padded word tail) lands exactly [silicon] | [hypothesis] |
| S-17 | 150 MHz video and its gates (4b, 4c, stage 6) | both flags earned; clip 1 twice and clip 2 at 150 MHz, 0 skipped [silicon] | 27 MHz, 69–203 skipped (sitting 1) |
| S-18 | Display jobs on the chip (b3, drill 2, b5, jobs self-test) | pass. B5 pushes windows at pitches 320/300/400 from rows 31 and 41 at 185 ns a pixel [silicon]. Merged `80bc8611` is recorded as verified on branch `p19-reverify-fix` (owner-accepted: B13 gates only the swap path) | `COMMIT` failing (quirks #33, #34) |
| S-19 | Interrupts on silicon (oq #3), part | `slp` (sitting 1): the 1 ms timer compare wakes the core from `sleep`, and doorbells wake it; 0 lost over two 60 s phases [silicon, indirect: the image's own tick and doorbell counters; no per-source entry count yet]. DMA-completion and other sources stay open (V-8). The programming guide still lists interrupts as unconfirmed: fold this | none confirmed |
| S-20 | PP DMA into the port (oq #41, pp #11, #12; DP-2) | already closed by S37 (engine 2 works, the general engine moves nothing even memory to memory) [silicon]. The text of oq #41 is stale: point it at `data-paths.md` D1 | — |
| S-21 | PP cores' caches (pp quirks #51, #53; rows C1–C3) | sitting 1: C2 shows separate arrays and no coherency; C3 shows the COP stays awake under `yield()` polling; C1 shows engine 2 runs from the COP [silicon]. `pp5022-reference/quirks.md` still says [hypothesis] | — |

### 1.2 Open, and software depends on it

| Id | Item | Known | Open |
|---|---|---|---|
| **V-1** | **Video player health** (owner) | **Normal Videos playback ran at 27 MHz:** every play in the owner's logs says `video clock: no stage 4c pass flag` or `void: the embedded image differs` [silicon, `ipodui.log.2`, `.3`, `ipodui.log`]. At 27 MHz the converted-file profile misses its frame: FEED mean 42.7 ms against 33.3 ms (sitting 1, `battery-levers.md` §4) [silicon]. **Release builds have no path to 150 MHz at all:** `vplayer.c` `check_fast_clock()` sets 27 MHz whenever `HAVE_BCMFW_DEVTEST` is off [ours, code reading]. At 150 MHz, clip 2 (the *Sintel* excerpt, 24 fps, a 41.7 ms frame) gave per-FEED work of mean 18.5 ms, p50 18.4, p95 27.4, max 40.2 ms; A/V mean 19.6 ms, max 33.5 ms; 48 hitches in 287 intervals; the PP player loop's longest gap 40–49 ms [silicon, tonight's stage 6 V6 records]. The H.264 kernels all run on the vector unit (guard word "C kernels" = 0) [silicon]; the MAC bit plays no part in H.264 | Is FEED p95 under 33.3 ms on a **converted** 30 fps file (stage 7 has never had one: "no test video" in both sittings)? Where do the hitches come from (decode tail, the PP loop's 40–49 ms gaps, or pacing)? How long does a control take to act (pause, scrub, menu) while decoding? Do the S28–S30 stalls (oq #29–#33) still happen on today's image? |
| **V-2** | **MAC off: the display-only image's `J_VPU` reads 0x23** (expected 0x27) | The start-up check gates the IDCT, the put and the fill together (`jpeg-spike.md` §16) [ours]. **PUTV fails on silicon** in every run: first word `db64a505` where `a664a505` is expected. The `a5` fill bytes are right; the copied bytes are wrong [silicon]. So the V8 D at `+r` with y = 8 (`jv_put`'s copy into rows 8–15) is the leading cause, not the two-chain IDCT [hypothesis, strong]. Sitting 1's 0x5 predates the vector put (branch `p19-jdec-put`, 2026-10-25). The `p19-vplayer-health` agent is on it | The V8 D `+r` y rule; whether the two-chain IDCT alone passes |
| **V-3** | **B13: warm swap park** | `PARK` replied OK (`st=0 n=1 f=3`: panel on, timing word 1). The warm word at `0x13C` stayed 0 for 60 ms; the power-down then got no answer and cut [silicon, `bcmtest.log` 1600–1604]. `park()` runs inside the same `hp_poll()` right after the reply (`hostproto.c` 3030–3035) [ours]. The path is `lcd_blit_wait()`, `di`, `IC_PRIO` = 0, a word copy of a 30-byte stub to `0xDE900`, then a call into it. 20 of 20 swaps pass in vc2sim [sim]. Another agent is root-causing it on `p19-b13` | Which step stops: the LCD wait, the copy, the jump into code just written (instruction fetch or cache), or the stub's own store |
| **V-4** | ALGN port part 2 never ran | `ALGNP 12 R602: port error -1` ended the port checks [silicon]. The PP port layer now refuses a mid-word write (image batch 2), which is what R602 does on purpose [hypothesis, strong] | R602, R604, R608, R64W (engine-2 rows at odd pitches); **E2ND/E2DR** (can a PIO tail overtake engine 2's last words?); **RX13/RX1W** (the PP's read-modify-write row writer, which the shipped `write_row` relies on) |
| **V-5** | Port reads: auto-increment (oq #43, second half) | Apple sets `RD_ADDR` once per call and reads on [apple-RE `osos` 0x10287a6c]; we set the address per word [ours] | Whether reads auto-increment. A yes roughly halves read-back cost: the Now Playing panel read-back is about 45 k words at 55–85 µs a word (`bench-p19.md` PANELAB) [silicon] |
| **V-6** | PW7: the ALT bank | Data written through ALT landed: the main bank read all four areas 256/256. ALT reads came back 18/0/0/0. The ALT ready flag was seen, and a frame takes the same time on both banks (9.6 ms) [silicon, sitting 1] | Why ALT reads fail (our read sequence against Apple's: Apple polls `RD_ADDR` bit 0 before the address and ends with a dummy `RD_ADDR` read [apple-RE `osos` 0x10287a6c]). Is ALT a usable second write channel for the drift and video (oq #25)? |
| **V-7** | Clock bands (`vbv`, `vgov`; battery lever 4) | Twelve bands built and gated by `vbv` flags; the merged image `80bc8611` lacks `HP_CAP_VCLK`, so both rows skip [ours; ingest "no vclk lines"] | Which bands hold on this unit; whether the governor keeps video smooth |
| **V-8** | Interrupts other than the tick and doorbell (oq #3 rest, #14) | DMA completion as an interrupt is unproven; our images poll | Needed only if an image moves from polling to DMA interrupts (power) |
| **V-9** | AAC on the BCM (oq #45, #46) | Stage 9 failed every run (0 of 200 AUs) with failures that differ between runs [silicon, S36c and later]. Not yet run since the `0x29` sign-extension fix (quirk #33), which affects every unsigned halfword of 0x8000 and above loaded at an offset [silicon]; libfaad does such loads [hypothesis] | Does stage 9 pass on batch 2's images? |
| **V-10** | B6/B7: texel fetch, memory bandwidth, port contention (oq #7 part, #44, #54) | Built, never run [ours] | Per-fetch cost (the affine and 3D budgets), SRAM/SDRAM rates, and whether port traffic slows the core |
| **V-11** | SDRAM size and aliases (oq #42, #43 part) | 4 MB; core address bit 22 is not decoded, so `0xFE40_0000` reads `0xFE00_0000` [silicon S11] | Higher aliases; the host's `0xC040_0000`. Software impact: an off-by-4 MB pointer silently aliases instead of faulting |
| **V-12** | PUTV-adjacent forms in shipped kernels not yet row-checked (oq #1) | `vasr.1`, `vmax.1`/`vmin.1`, `vmulhr.0`, `vmulm.1`, `vadd.2` (80-bit), `.1.t` operands; `vstr` with 16-bit lanes (unused) [hypothesis] | Which of them shipped images use (the §6 tool answers this). Each one used and not under a start-up check needs a row |
| **V-13** | `vmulhr`/`vmulhru` tie rounding (oq #23, quirk #12) | `vmulhrs` rounds ties away from zero [silicon S6]; the others are assumed to match [hypothesis] | A row only if a shipped kernel uses them on ties |
| **V-14** | **The visualizer overlay and the BCM-timed jobs on silicon** | BARV's vector forms are proven [silicon, tonight]. The overlay image (`viz-ovl.bin`), the `SPECTRUM` and `BARS` jobs end to end, Mirror's `J_BARS_FLIP` (`p19-viz-flip`), `J_OP_ANIM` (BCM-timed animation) and the new overlay ops `POLYLINE`, `SEGMENTS` and `PALROW` (branch `p19-viz-ops`) have run only in vc2sim [sim]. The FFT has no silicon row (`J_VPU_FFT`, proposed) | Does each op draw right on the panel, at its frame budget, and does its start-up check pass? |
| **V-15** | Store-then-load in SDRAM (oq #46) | memprobe's byte and halfword stores into SDRAM were right, with every store done before its loads [silicon S36c]. A store followed at once by a load of the same word is unchecked [hypothesis: it can return stale data] | `HP_AAC_MEMSTRESS` answers it. It is built for stage 9 and rides with V-9 |

### 1.3 Open: power, sleep, battery (software depends on it for battery life, not for function)

| Id | Item | Known | Open |
|---|---|---|---|
| P-1 | PW1: power census | FAIL in sitting 1 on the probe's own rules: the counter read 991–992 µs in 1,000 PP µs against a ±5 bound, and `0x1000_0408` read 0 by the port but `0x4000` by the core [silicon]. The census values themselves are recorded (`brief-map-power.md` §9) | Whether the short count is the PP's bounded port read [hypothesis]. **PW5 is locked behind a passing PW1**, so this rule blocks hibernate |
| P-2 | PW2: caches (oq #6, #59) | FAIL with `-3` and no step line [silicon] | Every cache question (gain on SDRAM, host coherence, bit 16). Matters for games and texture-heavy jobs |
| P-3 | Core level 5 at 27 MHz (`lvl5`; quirk #26, oq #11) | Not run: owner chose Skip in sitting 1 [silicon: not run] | Does level 5 hold with the jobs self-test |
| P-4 | Lit clock 24 MHz (`lit24`, `lit24ui`) | Not run [silicon: not run] | UI misses at 24 MHz |
| P-5 | PW3/PW4: Apple's voltage and clock ladder | Built, not run; wave 2 needs the owner's go | Feeds `vbv` (V-7) |
| P-6 | Stage 5i and 5s: sleep with the BCM powered, panel standby depths (oq #49, #50) | Built, never run; `slp` covered the tick-wake part [silicon] | SRAM retention while the core sleeps; whether the panel wakes right from light and deep standby (137 ms against 267 ms cold [sim]) |
| P-7 | PW5: hibernate by Apple's sequence (oq #57, #50, #7) | Built, not run. Apple's wake path expects SRAM and registers to survive `GPO32` bit 14 low with bit 15 high [apple-RE] | Does `0x1F0` hold `0xA5A5_0000` through 1, 10 and 60 s; does SDRAM hold. Needs its own go (a pin Rockbox never drove) |
| P-8 | HW1: the release hibernate (stub, `HP_PARK_HIB`, `bhw_*`) | Built, not in a session build [ours] | Runs only after PW5 passes |
| P-9 | SDRAM self-refresh and power-down (oq #7) | Two working choices without it: refresh at the 27 MHz word, or cut and re-`OPEN` [silicon] | Only matters if PW5 shows SDRAM must hold through hibernate |
| P-10 | Power draw in every state (oq #12, pp #25, #26) | **Out of scope:** no meter for a long time (owner, 2026-10-25) | — |
| P-11 | PP: DVS step, 100 MHz, speed grade (pp #20, #35, #37) | Probes built (`mac-and-dvs.md` §5); 5.4 and 5.5 write the PMU and the PLL | Curiosity until a battery or performance need appears |

### 1.4 Open: PP side (software depends on it)

| Id | Item | Known | Open |
|---|---|---|---|
| PP-1 | IDE completion interrupt (pp #29, DP-4) | Built: we sleep on source 23 with a polling fallback [ours] | No `m0 disk … irq=` line has ever been logged; tonight's logs have only `m0 disk wake` lines [silicon: absent] |
| PP-2 | FLUSH CACHE honoured (pp #32, R11) | The barrier lines exist (`usb ata w85=…`, `ata barrier flush …`) [ours] | No log seen yet. The file layer's crash ordering rests on it |
| PP-3 | Soft sleep with the drive off: the first read after wake (pp #38) | [hypothesis]: registers lost, re-init before the first command | One long sleep, then a play from outside the buffer |
| PP-4 | `CACHE_OPERATION`'s invalidate bit (pp #4) | Rockbox's workaround is slow [rockbox] | Curiosity unless the COP move (PP2 front end) needs fast invalidates |
| PP-5 | The 21 MB/s storage ceiling (pp #28, DP-5) | UDMA2 = UDMA4 = 21 MB/s [silicon S36c] | Curiosity: scan speed only |

### 1.5 Curiosity, documentation, or a later feature (no software depends on it now)

| Id | Item | Notes |
|---|---|---|
| C-1 | TV out (oq #15, #60–#62; PB-6 to PB-9) | A 5G feature to bring back later; needs an AV cable and a TV |
| C-2 | GPIO census (oq #63; PB-2, PB-4) | PB-2 found 0 wired pins with the known pins reading off: weak; a repeat only if a feature needs a pin |
| C-3 | The BCM's USB block (oq #64; PB-3) | `0x1000_6400` = 0 with the cable in and out: probably not bonded [silicon] |
| C-4 | Bus bypass (oq #56; PW6) | Apple never uses it [apple-RE]; only after PW5 |
| C-5 | The BCM's audio path to the WM8758 (oq #34–#40, #48, #65; pp #21) | Audio offload is not on the roadmap's critical path; play stays on PP1 (front end / back end split) |
| C-6 | The ISA's unnamed operations (oq #18–#20, #22) | Only if a kernel wants them |
| C-7 | Bootstrap bytes, the two banks' history, Apple's transport (oq #24, #26, #27) | Clean-room manual completeness |
| C-8 | Panels 0, 1 and 2 (oq #17) | Only panel 3 is on our unit. Nothing on this unit can settle it; it matters for a public 5G/5.5G release (#43), where a tester's unit with another panel runs stage 5 and B5. Until then the manual keeps the other panels tagged [apple]/[rockbox] |
| C-9 | Run control `0x1000_0400`/`0408` bits (oq #10), regulator bits `[7:4]` (oq #11), the `0x10004C00` block (oq #16) | Census material |
| C-10 | `RenderServer.bin`'s rasteriser (oq #66) | Only if stage 3d falls short |
| C-11 | PP: MMAP layout, local vector table, mailbox IRQ, `SWP` atomicity, clock selectors, USB PHY, wheel bits, `EXTONWAK`, I2C speed, arbiter table (pp #2, #5–#10, #14, #15, #17–#19, #22–#24, #33, #34, #36) | Clean-room material; none blocks a feature today. pp #7 (mailbox) and #8 (`SWP`) become relevant when the UI moves to the COP |
| C-12 | PP: SDRAM controller init, the flash part (pp #1, #27) | Only for a Native install without Apple's ROM; pp #27 is risky on the only unit |

### 1.5b The programming guide's thin-evidence list, mapped

`p19-bcm-guide`'s `programming.md` §13 ("Where the evidence is thin") names ten gaps. Each one is in this inventory:

| # | Guide's gap | Ids here | Rank (§2) |
|---|---|---|---|
| 1 | Display-only `J_VPU` 0x23 against 0x27 (the IDCT/MAC check) | V-2 | 4 |
| 2 | B13's park | V-3 | 5 |
| 3 | ACSA and ASHX not folded into the model | S-1, S-2 → §3.1 | 2 |
| 4 | HWLD's 48-bit and sp-relative loads; misaligned stores; `vst8` layouts | S-4, S-8, S-12 → §3.1 | 2 |
| 5 | The lone halfword (PLH1), the ALT bank, sub-word DMA units | S-16, V-6, S-13 | 2 (S-16, S-13), 8 (V-6) |
| 6 | Interrupts never confirmed; the cache never run on | S-19 (tick and doorbell, indirect), V-8; P-2 (PW2) | 13; 8 |
| 7 | SDRAM latency and bandwidth; store-then-load | V-10 (B7), V-15 | 11; 7 |
| 8 | Bands, governor, PW3/4/5 not run (viability only, no meter) | V-7, P-5, P-7 | 6; 10 |
| 9 | Overlays, SPECTRUM/BARS, ANIM only in vc2sim; plus the new ops `POLYLINE`, `SEGMENTS`, `PALROW` (`p19-viz-ops`) | V-14 | 6b |
| 10 | Panels 0–2 unverified | C-8 | 14 (not settleable on this unit) |

### 1.6 Items of the old list that are stale

- oq #41 (PP DMA into the port): closed by S37. Point it at `data-paths.md` D1 and DP-2, and keep only "IRQ 27" and "fields `[26:24]`" open, under pp #11.
- oq #43: the lone halfword is answered (S-16). Auto-increment stays open (V-5).
- oq #0b (loop filters, intra 16×16): answered in S24 to S27 (quirks #18–#20). Strike it.
- pp quirks #51, #53: answered by C2 and C3 (S-21). Fold them.

---

## 2. Ranking by impact on software

Ranks weigh what the item blocks or risks against the features in flight: the display jobs and the drift (shipping), video (shipping), the visualizer (shipping), power and battery (in progress), and games and 3D (later).

| Rank | Id | Blocks or risks | Why this rank |
|---|---|---|---|
| 1 | **V-1** video health | Videos "kinda laggy" today; **release builds can't reach 150 MHz** | A user-visible defect in a shipped feature. The main cause is a gate, not the chip |
| 2 | **S-1 … S-16** model folding | Everything built on vc2sim | Until the sim takes tonight's readings, a kernel or writer can pass in the sim and fail on silicon in exactly the ways just measured |
| 3 | **V-4** ALGN port part 2 | The shipped PP row writer (`write_row`'s read-modify-write) and engine-2 slot uploads | The drift's own upload is engine 2 at 160 × 240. E2ND/E2DR decide whether a PIO tail can overtake the DMA |
| 4 | **V-2** PUTV / MAC | BCM album-art decode speed (the display-only image) | Cost only (C is exact), and only once the display-only image is resident (V-3) |
| 5 | **V-3** B13 | The display-only resident image (G9: 19 KB of PP RAM instead of about 218 KB), and a video start without 0.37–0.62 s of dark screen | Not a shipping blocker: the merged image is verified and resident. It is the largest RAM and polish lever, though |
| 6 | **V-7** + P-5 bands and ladder | Video and lit battery life (`vgov`) | Battery is a pillar (the 3,000 mAh mod), but 150 MHz works today |
| 6b | **V-14** viz overlay ops on silicon | The visualizer (shipping) and its new ops | Each op falls back to the CPU on a failed start-up check, so the risk is speed and a "FALLBACK" line, not pixels. Still, nothing but BARV's forms has drawn on the panel |
| 7 | **V-9 + V-15** AAC stage 9, store-then-load | Audio offload; also a test of the `0x29` fix on a large C program | Likely explained by quirk #33. One hands-off run settles it |
| 8 | **P-1, P-2, V-6** PW1, PW2, PW7 | P-1 blocks PW5; P-2 the cache question for games; V-6 a second port channel | Probe bugs first; cheap |
| 9 | **P-3, P-4, P-6** lvl5, lit24, 5i/5s | Lit and dark battery levers | Viability rows; defaults stay off until they pass |
| 10 | **P-7, P-8** PW5 → HW1 | Instant wake from dark with the BCM retained | Battery and polish; needs explicit gos |
| 11 | **V-10, V-5** B6/B7, read auto-increment | Games, 3D, the Now Playing panel read-back | Budgets are guesses until measured |
| 12 | **PP-1, PP-2, PP-3** IDE IRQ, FLUSH, soft-sleep first read | Battery (CPU sleeps through DMA), data safety, wake reliability | Settled by logs from regular use, at no bench cost |
| 13 | V-8, V-11, V-12, V-13, P-9 | Latent model gaps | Closed by the §6 tool and strict traps rather than by rows |
| 14 | P-10, P-11, PP-4, PP-5, C-1 … C-12 | Nothing today | Curiosity or later features |

---

## 3. How each item gets settled

The owner's rule, in order:

1. Look up how Apple drives the hardware (`~/Git/re/bcm2722/notes/`, the reference's [apple] and [apple-RE] rows).
2. Run a bench row only if silicon must say.
3. Change vc2sim when the item is settled.

"Desk" means no device time.

### 3.1 The model folding (S-1 … S-21): desk only

**Fold:**
- `quirks.md`: #30 becomes `near32`; #31 and #32 confirm `sat32`; new entries for S-8, S-10, S-12, S-14, S-15 and S-16.
- `memory-access.md` §11: every row ALGN answered moves from Unknown to its reading.
- `isa-vector.md`: ASHX, ACSA and HWLD.
- `open-questions.md`: #2, #5, #21, #43 (half), #68, #69, #70 and #3 (half) answered; #0b and #41 struck (§1.6).
- `pp5022-reference/quirks.md` #51 and #53, from C2 and C3.
- `data-paths.md` D4 and the port rows.

**vc2sim changes**, each with a regression test that replays tonight's words (§6.2):

| Reading | vc2sim change | Knock-on |
|---|---|---|
| S-1 `near32` | `vc2vpu.py` `v_a16_reg_x` default `dbl16` → `near32`; the bench model with it | `bench-p19.md` says the kernels are exact under both rules. Re-run `jobs_image_test`, `viz_test` and `jidct_test` under `near32` to confirm |
| S-4 HWLD | `vc2cc` may again emit the 48-bit and sp-relative halfword loads | The merged image has only **68 B** of room (`merged-image-batch.md` §2). Shorter forms are worth having |
| S-7, S-9, S-11, S-13 | Move these forms into the known set (`STRICT_SCALAR_KNOWN`, `vc2vpu._strict_vmem`) | Strict mode stops trapping them. Our code still never uses the faulting scalar forms |
| S-8 | A misaligned scalar store: an imprecise fault (2–4 later instructions run) **and** the rounded-down store lands | Known form; still a fault, so code must never use it |
| S-10 | REP rows each round down on their own | Known form |
| S-12 | `vst16` and `vst8` off their row land at the address rounded down to 32 | Known form. Our rule (plain vector stores only aligned) stands, and the sim now produces the wrong data silicon would |
| S-14 | A DMA unit at an address that is not a multiple of its width: **trap** (strict), and model the wrong copy for the bench row | Apple never does it [apple-RE `vmcs` 0x83a4]; keep it refused |
| S-15, S-16 | The host-port models (`tests/bcmjob`, `hello_test.py`'s harness and the jobs model) drop bits 1:0 **and drop a lone trailing halfword** when a new address follows | Our writers already avoid both. The model makes a regression fail |
| — | Also, `vc2mmio` takes the DMA config's bit 0 as the enable, and PW8's word leaves it clear (`bench-p19.md` ALGN, "[sim]") | Fix the DMA model so the five ALGN DMA checks run in the sim |

**Owner time:** none.

### 3.2 V-1: video player health

**Apple-RE lookup:**
- Apple runs a load governor from 5 to 150 MHz with the matching regulator level (`brief-map-power.md` §2, `boot-and-clocks.md` §4) [apple-RE]. It never plays at a fixed crystal clock.
- Apple's video presents on the panel's TE edge (`lcd.md`, "Tearing"; `player.md` line 303) [apple]. Our pacing does not, which bears on the hitches.

**Desk, before any row:**
1. **The release clock gate** (software, flagged for the owner): a release build needs a verified-clock record that does not depend on devtest-only images. Proposal [ours]:
   - extend `bcmfw/verified.txt` so an image's line names the bands silicon verified (from `vbv`, plus 150 MHz from 4b and 4c);
   - `mkdevtest.py` compiles the list in, as it does for the verified boot images;
   - `check_fast_clock()` reads the compiled list in every build;
   - the re-verify (or a full session) earns the line, as it does now.
   
   Until this exists, every release build plays at 27 MHz, and every new image voids the flags until a re-verify reruns s05, s4b and s4c (about 10 min).
2. **Fit FEED against the sim's instruction counts.** Clip 2's 302 V6 records against vc2sim's per-frame instruction counts for the same file give cycles per instruction on silicon (prediction ≈ count / 150 MHz × CPI). With that ratio known, the sim predicts FEED for any file. This is how the performance half of "trusted" is measured (§5, E3-T).
3. **Read the hitch source** from the V6 records: are hitches tied to long FEEDs (decode), to the PP loop's 40–49 ms gaps, or to neither (pacing)? Desk analysis of tonight's log first.

**Bench (sitting A):**
- **`s7` with a converted file** (new prerequisite: `bcmtest/stage7.m4v`, about 60 s made by the shell's own converter settings, `phase14a-brief.md`: 320 × 240, 30 fps, baseline, 600–768 kbit/s; ideally an excerpt of the episode the owner found laggy).
- **Add to `s7`** [new lines]: a `ctl <action> ms=` latency per scripted control (from the action to the first changed picture, and to the audio's response). Add the FEED p50/p95/max to the summary line, which `vtrace` already computes for stage 6.
- **`vbv`, `vgov`** once the image carries `HP_CAP_VCLK` (V-7).
- **Owner time:** s7 about 2 min watched (one question); vbv 2 min watched; vgov 4 min watched.

**Expected readings per hypothesis:**

| Hypothesis | s7 FEED p95 | ctl latency | Normal play's log |
|---|---|---|---|
| H1: the 27 MHz gate is the whole story | < 33.3 ms (likely 20–28 ms, scaled from clip 2) | ≤ 150 ms | `video clock: 150 MHz` once the gate fix lands |
| H2: the decode tail at 30 fps | p95 < 33.3 ms but max > 33.3 ms, with skips | normal | skips in dense scenes |
| H3: PP-side control lag | normal | > 150 ms, matching the loop gaps | the `longest loop gap` lines track the latency |

**Criteria for "Videos is healthy"** [ours, proposed for the owner]:
- (a) every normal play logs the verified band (150 MHz, or a `vbv` band under `vgov`), with no session restart to get there;
- (b) on the converted-file profile, FEED p95 ≤ 25 ms (75 % of the 33.3 ms frame: the governor's floor rule, `battery-levers.md` §4) and FEED max < 33.3 ms over a 60 s excerpt;
- (c) 0 skipped and 0 GOPs dropped; A/V max ≤ 50 ms after the first second (stage 6's bound); hitches ≤ 2 % of intervals;
- (d) every `s7` control acts within 150 ms;
- (e) `J_VPU` as expected for each image (3 merged; 0x27 display-only, so MAC on, once V-2 lands).

**vc2sim when settled:** a timing calibration constant (CPI by kernel class), stored beside the model. `scripts/vclk_fit.py` already reads V6 records and can carry it. The gate then reports the predicted FEED p95 for the corpus clips at each band.

### 3.3 V-2: PUTV and the MAC bit

**Apple-RE lookup:** Apple's code never puts a V8 D at a `+r` whose y is not a multiple of 16. Its `vclib` routines move whole aligned rows [apple-RE, `memory-access.md` §11.5]; check `vc2forms --apple-kinds` at the operand level to confirm.

**Desk:**
1. Fit PUTV's logged words (`B4V 47 PUTV 00008000 db64a505 fb26a50e b84ca5fb c96ca573`, `bcmtest.log` line 214 and `bcmtest.1.log` line 307) against candidate rules for a V8 D's `+r` y, as `vpu_fit_test.py` did for ASHX. Candidates: y aligned down to 16 (then rows 8–15 land on rows 0–7), y ignored, and y as a multiple of 16 rounding to the nearest.
2. Change jdec's put to Apple's shape whichever rule fits. `bench-p19.md` already names the fallback layout: the copy in rows 0–15, no D `+r` y off 16. Re-check room: the display-only image has 1,344 B.
3. Confirm the two-chain IDCT alone passes the start-up check under the fitted rule in vc2sim.

**Bench:** PUTV rides in stage 4, so no new owner time. Add one row, **PUTY** [new]: a V8 D at `+r` y = 0, 8, 16, 24 into four columns, so the fitted rule is checked directly. Expected: under "aligned down to 16", y 8 lands on row 0 and y 24 on row 16; under "as given", each lands where asked.

**Pass:** `J_VPU` reads 0x27 on the display-only image (B5's `lab5` line).

**vc2sim when settled:** the D `+r` y rule (a new `v_d_reg_y` switch, its default the silicon reading). Strict mode traps a V8 D at `+r` y off 16 until then.

**Owner time:** 0 extra.

### 3.4 V-3: B13 (in progress on `p19-b13`; this plan only frames it)

**Apple-RE lookup:**
- (a) Apple flushes the whole cache after writing code (a relocated library @88a0, a library start @91a4) [apple-RE `brief-map-power.md` §6]. We copy the stub, then call it, with no flush.
- (b) Apple's own park (hibernate) runs from code already in place. It moves the stack to SRAM, writes `0x1F0`, and busy-polls [apple-RE §3]. It never writes code and then jumps to it.
- (c) PW1 read `0x1000_0C00` = `0x0000_0001` in context B tonight. Bit 0's meaning is open (oq #6); if it enables the instruction cache, then (a) is the rule we break.

**Hypotheses, each with the reading that would show it:**

| H | What stops | Breadcrumb it would leave |
|---|---|---|
| H1 | `lcd_blit_wait()` never returns (it waits on a status that idle DMA never sets) | step word stuck at "blit wait" |
| H2 | The jump runs stale or partly fetched bytes at `0xDE900` (instruction fetch or cache) | "copied" written, `PRK` never written |
| H3 | The stub's `st r0, (r1)` lands elsewhere | the stub's first breadcrumb present, `0x13C` still 0 |
| H4 | The core faults (`di` state, a vector) | the fault hook's record (bench builds) |

**Bench (sitting A, b13 with breadcrumbs)** [new lines, desk first]:
- `park()` writes a step word to a free mailbox word before each step (blit wait, `di`, masks, copy, call). The stub's first instruction writes a second word.
- The PP logs both after the 60 ms timeout, before the cut.
- A variant arm adds Apple's cache flush (`0x1000_0C00 = read | 0x4000_0000`, poll bit 0) between the copy and the call.
- Expected: H2 clears with the flush arm; H1 shows "blit wait"; H3 shows the stub's own word.

**vc2sim when settled:** whichever rule it was. For H2, the sim refuses to execute bytes stored since the last flush (a "fresh code" trap), which is cheap and catches every future relocation.

**Owner time:** about 2 min watched, one question.

### 3.5 V-4: ALGN port part 2

**Apple-RE lookup:** Apple waits for each DMA piece before its PIO tail [apple-RE `osos` 0x10286fb4]. That makes E2DR Apple's way and E2ND ours.

**Desk:** let the bench's port checks bypass the port layer's mid-word refusal (a bench-only write path, as `bcm_video_wr_variant` already is for CR1), so R602 runs as designed. Make each port check independent, so one `-1` cannot end the rest.

**Bench:** in stage 4 (rides in `s05`), so no new owner time.

**Expected:**

| Check | Expected (§11.4) | If not |
|---|---|---|
| R604, R608 | exact | — |
| R602 | rows 1, 3 and 5 land 2 bytes low | — |
| R64W | exact | — |
| E2ND | if it differs from E2DR, the shipped engine-2 path must wait for write-ready before any tail (Apple's order) | — |
| RX13, RX1W | exact | **the shipped `write_row` is wrong**: fix it before anything else |

**vc2sim when settled:** the port model gains the tail-overtake rule if E2ND differs. Otherwise nothing changes, and the rows become fixtures (§6.2).

### 3.6 V-5: port read auto-increment

**Apple-RE:** `osos` 0x10287a6c sets the address once a call, then reads words on, ending with a dummy `RD_ADDR` read [apple-RE]. Strong evidence that it works.

**Bench row RDAI** [new; stage 4 port part, hands-off]:
- the image lays a 64-word pattern;
- the PP sets `RD_ADDR` once and reads 64 words, Apple's way;
- then reads per word as today, and compares.

**Expected:** H-yes, 64/64 right and about half the time per word; H-no, word 0 right and the rest repeat or are stale.

**vc2sim/port model:** auto-increment on reads if H-yes.

**Software:** a faster read-back (PANELAB's Now Playing panel, the self-test's read-back).

### 3.7 V-6: PW7, the ALT bank

**Desk:** compare our ALT read with `osos` 0x10287a6c's sequence for bank 1: the `RD_ADDR` bit 0 poll before the address, the per-word `CONTROL & 0x10` poll, two 16-bit reads, and the trailing dummy read. Fix the probe to Apple's exact order.

**Bench:** PW7 rerun (hands-off, under 1 min).

**Expected:**
- ALT reads 256/256: our sequence was the fault, oq #25 closes, and ALT is a second channel;
- still wrong: ALT is write-only for us. That is still useful, since writes work [silicon].

**vc2sim:** a two-bank host model (the devtest host model has one bank today).

### 3.8 V-7 and P-5: bands and ladders

**Apple-RE:** Apple's level table at `0x1FA68` and its order, the level 10 ms before a clock rise and 10 ms after a fall [apple-RE], are already built into `clock_band`.

**Desk:** put `HP_CAP_VCLK` into the merged image (batch 3). The 68 B of room is the constraint; S-4's shorter halfword loads may pay for it.

**Bench:** PW3 and PW4 in sitting B (the wave 2 go; PW4 is locked behind PW3); `vbv` and `vgov` in sitting A. They do not depend on PW3/PW4: `vbv` verifies each band by playing it.

**Expected:**
- `vbv: PASS: N of 11 bands hold`, with N ≥ 6 [estimate] (the 87 MHz band and up should carry clip 1);
- `vgov` PASS: governed plays look like the 150 MHz play.

**vc2sim:** none. These are silicon envelopes; the sim's timing calibration (§3.2) predicts which band a file needs.

### 3.9 V-9: AAC stage 9

**Apple-RE:** none needed.

**Bench:** stage 9 (a Debug row, hands-off, about 2 min) on batch 3.

**Expected:**
- H (#33 explains it): 200 of 200 AUs match at both clocks;
- otherwise, `HP_AAC_STATE` names the first checkpoint that differs, as oq #46 planned.

**vc2sim:** none if it passes.

### 3.9b V-14: the visualizer overlay and its ops

**Apple-RE:** none applies. These are our own designs on forms whose semantics are now proven (BARV) or are ordinary scalar C.

**Desk:** make sure each op has a start-up check and a `J_VPU` bit (or a C path) in the overlay image, and that `proven_forms.py` (§6.1) lists the overlay's forms.

**Bench row VIZ** [new; sitting A, about 3 min watched]:
- The overlay boots.
- Each op draws a known frame, then a checksum read-back is compared with the sim's: `SPECTRUM`, `BARS`, `BARS` flipped, `ANIM` (one second of a timed animation), and `POLYLINE`, `SEGMENTS` and `PALROW` from `p19-viz-ops`.
- Each op's frame time is logged against its budget.
- The owner answers one question: "Did each pattern look right?"
- Lines: `viz <op> sum A/B ms=N`.

**Expected:** every sum equal to the sim's; frame times within budget; no `viz FALLBACK` line.

**vc2sim when settled:** each op's checksum becomes a fixture (§6.2).

### 3.10 P-1, P-2: PW1 and PW2 (probe fixes)

**Desk:**
- PW1's counter rule: time the BCM's 1 MHz counter against the BCM's own loop, not against a PP window that includes two bounded port reads, or widen the bound to ±10 µs with that reason given.
- Record `0x1000_0408`'s port/core difference as a finding (a register the port reads as 0).
- PW2: add a step line before each sub-step, so `-3` names its step.

**Bench:** PW1 and PW2 rerun in sitting A (hands-off, about 3 min).

**Expected:**
- PW1 PASS, which unlocks PW5;
- PW2's checksums right under all three cache words, with the timings and the coherence readings logged.

**vc2sim:** if PW2 shows the cache gains on SDRAM, the sim needs no cache model as long as images keep it off. If an image turns it on, the sim adds a "stale after host write" trap (no flush between a host write and a core read of cached SDRAM).

### 3.11 P-3, P-4, P-6, P-7, P-8: the sleep and power rows (sitting B)

| Row | Apple-RE | Expected | Owner time | Go |
|---|---|---|---|---|
| `lvl5` | Apple's table asks level 5 at 27 MHz, and its running chip read `0xB5` [apple, silicon PW1 A] | level 5 reads back; self-test passes in both B phases | 80 s hands-off, 1 question | yes (the risk is a hung core, cured by the cut) |
| `lit24`, `lit24ui` | — | B's misses ≤ A's + 20 per 1,000 frames | `lit24` hands-off; `lit24ui` 2 × 1 min of drift and scroll, hands-on | no |
| 5i, 5s | Apple's RTOS idles `ei; sleep; di` [apple-RE] | SRAM framebuffer intact after a minute asleep; a correct picture after a light and a deep standby wake, at about 137 ms against 267 ms cold [sim] | about 5 min, watching each wake | no |
| PW5 | Apple's `videocore=1` order, `GPO32` bit 15 then bit 14 [apple-RE `brief-map-power.md` §3] | `0x1F0` = `0xA5A5_0000` after 1, 10 and 60 s; SRAM CRC equal; SDRAM reported | about 3 min watching the bars | **yes, its own**, for steps 3–9 |
| HW1 | the same, our release pieces | both holds come back `AWAKE`, epoch kept, slot hit | about 1 min | yes, after PW5 passes |

**vc2sim when each settles:**
- a `sleep` that keeps SRAM;
- a hibernate model that keeps SRAM and registers with bit 14 low and bit 15 high, and loses everything on a cut (the sim already models the cut).

### 3.12 V-10: B6/B7 and stage 3d (games, later)

**Bench:** B6 (texel fetch) and B7 (bandwidth, contention) in sitting A, hands-off, about 3 min. Stage 3d with its TE run (oq #52, #55) in sitting B, about 5 min watched.

**Expected:** numbers rather than verdicts:
- per-fetch ns in SRAM and SDRAM at 27 and 150 MHz;
- the % slowdown under port traffic;
- 3d's checksummed frames equal to `bcm-devtest-r3d.h`;
- TE edges on GPIO 49 at the panel's period.

**vc2sim:** the memory latencies go into the timing calibration (§3.2). A checksum mismatch in 3d is a model bug in a large C program at 150 MHz, which is exactly what E3 must catch.

**Stage 3d result (2026-10-26) [silicon]:** PASS, every check frame equals vc2sim's at 27 and 150 MHz (oq #55 answered); TE 67.8 Hz, the synced walk 33.9 fps with 0 late (oq #52 answered); cost 1.14–1.64 × the estimate, scaling exactly with the core clock, fitted as 1.26 × instructions + about 590 cycles a drawn triangle (`render3d.md`, "On silicon"). For E3 this is the scalar-C correctness evidence at 150 MHz under load; the timing fit joins the decoder's (FEED = 1.3 ms + cycles / 125.7) in §3.2. Still open in V-10: B6/B7 (per-fetch cost, bandwidth, port contention).

### 3.13 PP-1 … PP-3: from regular use, no bench

**Desk:** confirm the shipped developer build logs:
- `m0 disk … irq=I` at the end of a play;
- the `ata barrier flush` line once per boot;
- `usb ata flush max=…` after a USB session;
- `sleep drive off rc=` with the first `m0 disk` after a soft sleep.

None of them appears in tonight's logs, so check they are compiled in and not filtered.

**Owner:** normal use (play an album; one USB copy; leave it asleep more than 1 min; play a track not in the buffer). The agent ingests.

**Expected:**
- `irq` > 0 with no `polling`;
- `nosup=0 fail=0`;
- the first read after a wake at UDMA2 with DMA counted.

**PP sim when settled:** the storage model's wake cost and barrier rules (`sim-fidelity.md` entry 6).

### 3.14 V-8, V-11, V-12, V-13, P-9: no new rows; strict traps instead

- **V-11:** vc2sim traps any SDRAM access at or above `0xFE40_0000` (or the host's `0xC040_0000`), rather than aliasing it.
- **V-12, V-13:** the §6 tool lists which of these forms shipped images use. Any form used and not under a start-up check gets a row in the next stage 4 build, or the kernel avoids it.
- **V-8:** our images poll DMA, so nothing to settle until an image uses a DMA interrupt.
- **P-9:** only if PW5 shows SDRAM must hold.

---

## 4. The two sittings

Both follow the rules in place:
- no meter; viability and performance only;
- every step is an on-screen script with wheel answers (`devbench`), and the results write themselves;
- a fresh backup and the owner's go before each install;
- Apple's firmware stays the way back.

Picks are re-verify style: the bench session gets two new modes, generated with `scripts/bench_script.py`, so the confirm screen lists the run and its time.

### 4.0 Before sitting A: batch 3 (agent, desk; about 2 days)

One image batch behind one re-verify, as batch 2 was:

1. The model folding (§3.1) and its regression fixtures (§6.2).
2. PUTV fitted, jdec's put fixed (§3.3), and the PUTY row.
3. B13's breadcrumbs and the flush arm (§3.4), merged from `p19-b13` once its desk work is done.
4. ALGN's port part with the bench bypass, plus RDAI (§3.5, §3.6).
5. PW1's rule, PW2's step lines, PW7's Apple-order read (§3.7, §3.10).
6. `HP_CAP_VCLK` in the merged image (§3.8).
7. `s7`'s FEED summary and control latencies (§3.2).
7b. The VIZ row over the overlay image with `p19-viz-flip` and `p19-viz-ops` merged (§3.9b); `viz-ovl.bin` in the zip.
8. The release clock gate, if the owner agrees (§3.2, desk item 1). Otherwise sitting A still measures at 150 MHz on the developer build.
9. `scripts/validate.sh` prints `GATE PASSED`, and `budget-reviewer` passes.

**Files on the device:**
- the zip's `bcmvideo.bin` (merged) and `bcmprobe.bin` (plain), sizes and FNVs checked against the headers;
- `bcmtest/clip.mp4` and `clip2.mp4`;
- **new: `bcmtest/stage7.m4v`** (a converted 30 fps excerpt, about 60 s);
- `bcmjobs.on`.

### 4.1 Sitting A: re-verify and closure (about 40 min)

**Have ready:** battery charged, a USB-C cable for the pull afterwards. Unplugged during the run.

| # | Step | Kind | Time | Owner | Closes |
|---|---|---|---|---|---|
| 1 | `pre` → `s05` (the flags are void on a new image): stages 0–5, including stage 4's bench with ALGN parts 1 and 2, PUTV, PUTY, RDAI, HWLD | watched | 5 min | watch, 1 answer | V-2 (PUTV), V-4, V-5; re-checks S-1 to S-16 on the new model |
| 2 | `jobs`, `jvpu` | hands-off | 1 min | — | S-3; `J_VPU` 3 |
| 3 | `s4b`, `s4c` | hands-off, then watched | 3 + 1 min | 1 answer | the 150 MHz flags |
| 4 | `b3`, `drill2` (offered) | watched | 3 min | outside the session, then Go on | the display jobs on batch 3 |
| 5 | `s6` | watched | 1 min | 1 answer | the clips at 150 MHz; FEED fit data (§3.2) |
| 6 | **`s7`** with `stage7.m4v` | watched | 2 min | 1 answer | **V-1**: FEED p95, control latency |
| 7 | **`vbv`** | watched | 2 min | 1 answer | V-7 bands |
| 8 | **`vgov`** | watched | 4 min | 1 answer | V-7 governor |
| 9 | `b13` (breadcrumbs, flush arm) | watched | 2 min | 1 answer | **V-3** |
| 10 | `b5` | watched | 1 min | 1 answer | V-2: `J_VPU` 0x27 on the display-only image |
| 11 | PW1, PW2, PW7, PW8 | hands-off | 4 min | — | P-1, P-2, V-6 |
| 12 | B6, B7 | hands-off | 3 min | — | V-10 |
| 13 | Stage 9 (AAC, with `HP_AAC_MEMSTRESS`) | hands-off | 2 min | — | V-9, V-15 |
| 14 | **VIZ**: the overlay ops on the panel (§3.9b) | watched | 3 min | 1 answer | V-14 |

- **Total:** about 43 min. About 28 min watching or answering (steps 1, 3–10, 14), about 15 min hands-off (steps 2, 3's first part, 11–13).
- **Earns:** the verified line for batch 3's merged image, if `s05`, `jobs`, `jvpu`, `s6`, `b5` pass. B13 gates only the swap path, under the owner's rule of sitting 2.

### 4.2 Sitting B: power, sleep and graphics (about 40 min, every risky step behind its own go)

**Have ready:** battery charged, unplugged. Nothing else.

**Prerequisite:** sitting A's PW1 PASS (it unlocks PW5). HW1 must be in the session build (it is "not in a session build yet").

| # | Step | Kind | Time | Owner | Closes |
|---|---|---|---|---|---|
| 1 | `lvl5` | hands-off after its go | 80 s | go, 1 answer | P-3 |
| 2 | `lit24`, `lit24ui` | hands-off, then hands-on | 1 + 3 min | drift and scroll as the screen asks | P-4 |
| 3 | PW3, then PW4 | hands-off after the wave 2 go | 90 s + 1 min | go | P-5 |
| 4 | 5i, then 5s | watched | 6 min | look after each wake, answer | P-6, oq #49, #50 |
| 5 | **PW5** (holds 1 s, 10 s, 60 s) | watched | 3 min | **its own go**; are the bars still shown? | P-7 |
| 6 | HW1 (only if PW5 passed) | watched | 1 min | go | P-8 |
| 7 | Stage 3d, with its TE run | watched | 5 min | 1 answer; a photo optional | V-10, oq #52, #55 |
| 8 | `drill1`, `drill3` | watched | 2 min | as the screen asks | the jobs' restart and backlight drills |
| 9 | A long real play: 10 min of an owner episode, used normally (pause, scrub, menu) | regular use | 10 min | normal use | V-1's stall questions (oq #29–#33) on today's image |

- **Total:** about 40 min. About 25 attentive: steps 2's second part, 4–9, of which step 9 is ordinary viewing.
- **PW6 (bypass):** only if PW5 passed and the owner agrees on the day. It is curiosity (C-4), so leave it out by default.
- **TV probes (PB-6 to PB-9):** not in either sitting (C-1). They need an AV cable and a TV.

### 4.3 After each sitting

1. `scripts/bench_ingest.py <dir> --commit <build>`.
2. Fold every reading into the manual (quirks, §11, the open questions), with its tag, in the same change.
3. Commit the silicon words as fixtures (§6.2). Change vc2sim, and re-run the gate.
4. Anything that failed gets a short re-run, in the next sitting or in a 10-minute re-verify.

---

## 5. Exit criteria: "vc2sim is trusted"

Each criterion is a gate check, or a list of rows whose silicon words the sim must reproduce. A criterion is met when its check is green in `scripts/validate.sh`.

### E1. Strict modes on by default (BCM)

| Mode | Today | Required |
|---|---|---|
| `VC2SIM_STRICT_ALIGN=trap` | default since 2026-10-25 [ours] | stays default; its known set equals the forms §11 marks [silicon] |
| Strict forms: executing an instruction or vector form that is neither proven on silicon nor exempt (a bench check region or a start-up-check-guarded kernel) traps | static gate for scalar kinds only (`forms_test.py`); **vector forms are informational** | a runtime trap in vc2sim for scalar, vector and DMA forms, default on, from the proven table (§6.1) |
| Strict port: drop bits 1:0, drop a lone trailing halfword, refuse a DMA tail before write-ready if E2ND says so | bits 1:0 only [ours] | all three, in every host-port model (`tests/bcmjob`, the devtest harness, the jobs model) |
| Strict memory: trap SDRAM accesses at or above 4 MB; trap code executed from bytes written since the last flush (if B13 shows H2) | none | both |

**Measure:** `validate.sh` runs every bcmfw test with all four modes on, and a mutation row proves each mode fires.

### E2. Every form the shipped images use is proven or trapped

**The shipped images:**
- merged `bcmvideo.bin`;
- display-only `jobs_display`;
- plain/probe `bcmprobe.bin`;
- `hello_dma` (the release boot path's DMA rung);
- the viz overlay;
- `hibstub`.

**The forms:**
- scalar instruction kinds (`vc2sim.cover_key`);
- vector forms at `vpu_forms.py`'s operand level (operation, widths, direction, `+r`, stepping, REP, predication) over **every** kernel file, not only `vpu_kernels.S` and `disp_kernels.S`;
- BCM DMA forms (unit width × address alignment × direction × 2-D);
- host-port forms (start alignment, data width, tail shape, bank).

**Measure:** `scripts/proven_forms.py` (§6.1) reports each image's forms against `tools/bcmre/proven_forms.txt`. The gate fails on any form that is unproven **and** neither exempt nor trapped. **Target: 0.**

Today's count is unknown. Vector forms are not gated, and `device_verified.txt` has 13 scalar kinds, last updated 2026-10-11.

### E3. Model against silicon: the rows whose words must match

The sim must reproduce, word for word, the silicon record of each row below. The words live in committed fixtures (§6.2), so this is a regression test in every gate, not a one-off:

| Group | Rows | State after sitting 2 |
|---|---|---|
| Vector checks (stage 4 `B4V`) | X15M, X15Z, X15S, MHDU, VCLP, R48E, D16R, REPA, RNDT, RNDU, MLSC, ASHF, ASHG, **ASHX** (`near32`), ACSA, GTHR, BARV, **PUTV**, PUTY (new), the REP-16 `vst16` layout | all match but ASHX (model still `dbl16`) and PUTV (unfitted) |
| Halfword loads | HL16, HS16, H28O, H29O, H48H, H48S, HSP0, HSP1 | match |
| Alignment (BCM side) | LW1C … LHP1 (13), SWR1 … SHO1 (8), VL68 … VLS4 (7), VPG4, VPGX, VPQS, VS68 … VS84 (6), D32C … D32M (5) | 13 + 6 + … match; **SWR/SHR (8), VLS4, VS (5), D32M** do not, until §3.1 lands |
| Alignment (port) | PWD0 … PAD2 (5), P164 … P16C (3), PLH1 … PLW3 (4), R602 … R64W (4), E2ND, E2DR, RX13, RX1W, RDAI (new) | PLH1/3/5 do not (model: commits); R602 onward not yet run |
| Port and jobs | CR1 (`cr1: PASS`), jobs self-test, `J_VPU` per image (3 merged, 0x27 display-only) | CR1 not re-run on batch 2; `J_VPU` display-only 0x23 |
| Whole programs | stage 4 kernels 69/69; stage 6 clip 1 checked frames; **stage 9 AAC 200/200** and MEMSTRESS; **stage 3d** checksummed frames; B5's windows; **VIZ**: every overlay op's checksum (`SPECTRUM`, `BARS`, flipped `BARS`, `ANIM`, `POLYLINE`, `SEGMENTS`, `PALROW`) | stage 9, 3d and VIZ not yet |
| Swap and power | B13 20/20 with the injected fault handled; PW5 and HW1 `AWAKE` (a sim of the park and wake, with the pin's effect taken from silicon) | B13 FAIL; PW5 not run |
| **E3-T, timing** (the performance half) | stage 6 clips 1 and 2 and `s7`: vc2sim's instruction counts × the fitted CPI predict FEED mean within ±15 % and p95 within ±20 %; B6/B7's per-access costs within ±25 % of the calibration | not yet calibrated |

**Measure:** `readlog.py` over the fixtures prints no "NOT as modelled" line, and `bench_test.py` replays every fixture green. Rows marked "informational" today stay informational for the bench's pass, but are **not** informational for E3.

### E4. The PP side's equivalents

From the simulator-fidelity audit (`docs/sim-fidelity.md` on `simfid`; merge it first):

| Check | Required | Today |
|---|---|---|
| Alignment | every gate sim build that runs UI scripts is UBSan-strict (no report-only zones in `apps/ipodui` or `firmware/target/arm/ipod`) | strict only in the listed zones |
| ILP32 | every struct written to disk or the wire has a `_Static_assert` (grep gate) | 41 asserts, no gate |
| Arena size | the sim's screen arena asserted equal to the device's (`sim-fidelity.md` #12) | unverified |
| Cross-core | `tests/copjob` gains the two-cache model that C2 measured (separate arrays, no coherency, per-core write-back), and a missing write-back fails with the address and the core | single-core sim |
| Port model shared | the PP-side job packer and vc2sim use one port-rule module (E1's strict port) | separate models |
| PP silicon rows | C1, C2, C3 pass (done); PP-1 `irq` > 0; PP-2 `nosup=0 fail=0`; PP-3 the first read after a soft sleep clean | logs not seen |
| Timing | the instruction-budget gate (`tests/perf`) covers every UI path the owner reports as slow; the mixer at device frame size (`-DSIM_DEVICE_MIXER`) in one audio gate run | partly |

### 5.5 Target date and assumptions

**Today:** Monday 2026-10-26.

| Date | What | Who |
|---|---|---|
| Mon 10-26 – Wed 10-28 | §3.1 folding and fixtures; the PUTV fit; B13 desk (`p19-b13`); ALGN bypass, RDAI, PW1/2/7 fixes; `HP_CAP_VCLK`; `s7` lines; `proven_forms.py` (E2) and the strict port/memory modes (E1); batch 3 and its gate | agents, parallel worktrees |
| Thu 10-29 | **Sitting A** (about 40 min) | owner, after the go and the backup |
| Fri 10-30 – Sat 10-31 | ingest; fold; fixtures; fixes for whatever failed; HW1 into the session build | agents |
| Sun 11-01 | **Sitting B** (about 40 min) | owner, with each go |
| Mon 11-02 – Wed 11-04 | ingest; fold; E3-T calibration; E4 items; a batch 4 if any form or rule changed | agents |
| Thu 11-05 | a short re-run sitting (≤ 20 min) for failures | owner |
| **Fri 11-06** | **E1–E4 green: "vc2sim trusted (v1)"** | — |

**Assumptions:**
- two sittings a week of at most 45 min each, plus ordinary use for PP-1 to PP-3;
- each sitting is preceded by one image batch that passes the gate the day before;
- B13's root cause is found on `p19-b13` before batch 3, or its breadcrumbs find it in sitting A;
- no sitting finds a new **class** of quirk (a new kind of divergence, not a new instance of a known one). Each new class adds about one week: one batch and one sitting. **Conservative date: Fri 2026-11-13.**

**What "trusted (v1)" does not cover:**
- power figures (no meter);
- TV out;
- bus bypass;
- panels other than 3;
- the BCM's audio path;
- cycle-exact timing: E3-T is a calibrated estimate with stated bounds.

Each of these stays tagged in the manual as it is today.

---

## 6. The tooling that keeps it true

### 6.1 `scripts/proven_forms.py` and `tools/bcmre/proven_forms.txt` [proposed]

**The table:** one line per form:

```
<domain> | <form key> | silicon <row> <date> | apple-static | trap | guard <J_VPU bit>
```

- The domains are `scalar`, `vector`, `dma` and `port`.
- It replaces and extends `device_verified.txt`, whose scalar kinds it imports.
- The vector key is `vpu_forms.py`'s operand-level key. DMA keys come from the transfer-information words `align_audit.py static` already decodes. Port keys come from the PP port layer's trace in `tests/bcmjob`.

**The script:**
1. Builds (or reads from `build/bcmfw`) each shipped image's listing.
2. Enumerates its forms statically, extending `vc2forms.scan` and `vpu_forms.py` to every kernel file (`jdec_kernels.S`, the jobs kernels, the viz kernels, the hibstub).
3. Adds the forms a vc2sim run of each image's workload executes, using strict mode's `log` counts.
4. Prints a per-image table: proven / Apple-static-only / guarded by a start-up check / trapped / **unproven**.
5. Exits non-zero on any unproven form outside a bench check region.

**Note on "Apple-static" forms:** these count as proven for **encoding existence** only, not for semantics. The `0x29`/o1 load was in Apple's code 1,047 times and still had the opposite sign extension from our model (quirk #33). So a form whose semantics our code relies on needs a silicon row, or a start-up check, unless Apple's own simulated self-test cases (22,163 of them) pin the semantics.

**Gate:** a step in `scripts/validate.sh` after the bcmfw builds.

**Cost:** about one agent day. The pieces exist (`vc2forms.py`, `vpu_forms.py`, `align_audit.py`, `forms_test.py`); the work is the table, the unification, and the vector and DMA coverage.

### 6.2 Silicon words as fixtures [proposed]

- `scripts/bench_ingest.py --fixtures` writes every `B4V`, `ALGN`, `ALGNB`, `ALGNP`, `cr1`, `pw*` and `V6` record of a sitting into `tests/silicon/<date>-<build>/`, with the image FNV.
- `bcmfw/tests/bench_test.py` (and a port-model test for the `ALGNP` rows) replays them in vc2sim and fails on any word the model does not produce.
- E3 is then a gate check, and a later model change that breaks a silicon reading fails at once.
- The V6 records feed `vclk_fit.py`'s CPI calibration (E3-T).

### 6.3 Stale-knowledge checks [proposed, small]

- **`scripts/oq_lint.py`:** every open-question item that names a bench row whose fixture exists is reported as "answered on silicon, fold it". Every item tagged [hypothesis] that cites a fixture is reported too. This would have caught oq #41's stale text and pp quirks #51/#53.
- **The PP/BCM port rules in one module:** `tests/bcmjob`'s model and vc2sim's port model import one rule file, so E1's strict port is defined once (`sim-fidelity.md` #9).
- **`bench_ingest`'s "stale" label:** a FAIL flag over a reading the model now has (branch `p19-sitting2-docs`). Merge it, so the ingest stops listing settled words as anomalies.

### 6.4 What the owner sees

Nothing changes in the product. Developer builds gain the new bench modes ("Closure A", "Closure B"), generated by `bench_script.py` like today's re-verify, each with its confirm screen and its sum of minutes.

---

## 7. Questions for the owner

1. **The release clock gate (§3.2, desk item 1):** may a release build trust a compiled-in record of verified clock bands per image FNV, earned by the re-verify? Without it, shipped Videos plays at 27 MHz.
2. **The video criteria (§3.2):** FEED p95 ≤ 25 ms and max < 33.3 ms on the converted profile, hitches ≤ 2 %, controls ≤ 150 ms. Too strict, or right?
3. **Sitting B's gos:** `lvl5`, PW3/PW4 (wave 2), PW5 and HW1. Give them on the day, or now in principle?
4. **`stage7.m4v`:** which excerpt? Ideally the one that felt laggy.
