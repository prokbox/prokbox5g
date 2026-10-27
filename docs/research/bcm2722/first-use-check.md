# First-use video check (0.8.x pre-release)

Status: **built** on `p19-firstcheck`, 2026-10-26 (the "As built" section at the end); the design below is the reviewed draft, kept for its reasons. **A 0.8.0 gate item** (owner, 2026-10-26: before 0.8.0, although the image token already gives every unit 150 MHz). The band ladder is a marked table waiting for sitting 4b's `vbv` results.

## What the gate does today
- `apps/video/vclock_gate.h` opens 150 MHz when the running image's FNV has a clock150 token (`bcmfw/verified.txt`, compiled in).
- The token names the **image**, not the unit. So a release built from a verified image already runs **every** unit at 150 MHz, with no check on that unit.
- Apple ran every 5G/5.5G at 150 MHz ([apple], `hardware.md` §clock), so the part is rated for it.
- The first-use check is therefore a **per-unit safety net**: it catches the one unit in many with marginal silicon, a weak cell or a damaged board, before a whole film plays wrong on it. It does not enable 150 MHz.

## The check
- **When:** at the first video play on a unit (no unit token, or a token for another image), inside the play's own start-up.
- **What:** the merged image's own release path, not the bench images:
  1. `HP_OP_VCLK` up to the top band (Apple's sequence; `clock_band` already checks the core's speed against the crystal, ±12.5 %);
  2. the SDRAM self-test and packet read-back (as stage 6);
  3. a short built-in test stream decoded at 150 MHz, comparing each picture's CRC against its known value (bit-exact, as `s6`'s checked frames). The stream would be about 30 pictures, roughly 20 KB, shipped in the hidden folder (not in the image: the room is 6.5 KB).
- **Pass:** the band was reached and the speed checked, 0 fallbacks, every CRC exact, the read-backs had 0 bad words, and the pictures came within the frame budget. Then the unit token is written: the image FNV, the band, the date and the build.
- **Fail:** step down to the highest band that passes (from `vbv`'s band list) and record that band. If none passes, show the styled video error screen (the owner's rule). Its text also asks the user to Save Diagnostics (Settings › About, the 4th page) and attach the report (owner, 2026-10-26); the reason goes into the log either way, so the report carries it.

## What the user sees
- The film's first frame waits about **1 s**, with "Preparing video…" in the 7G's style. Estimate: switch 11 ms, SDRAM test about 0.1 s, 30 pictures at about 8 ms each.
- After that it never appears again on that unit until the image changes.

## Staying honest
- **Re-check triggers:**
  - the token names a different image (any firmware update with a new image);
  - any decode error, concealed block, clock fallback or `FAULT` during a play invalidates the token at once. That play drops to the band below, and the next play re-checks.
  - Optionally, a re-check after N months or at a battery level below X; this needs `vgov` data.
- **False-pass risk:** a 1 s check can't see temperature or low-voltage margin. The mitigations are the live invalidation above, and Apple's per-band regulator levels (already in the bands). The residual risk is a unit that is marginal only when hot or on a low battery: it would show a decode error once, then step down.
- **Wiped hidden folder:** the token goes with it, and the next play just re-checks (1 s). That's acceptable; no NOR or bootloader writes.

## Release code needed (no devtest)
- PP: a unit-token read and write (`vclock_gate` gains a per-unit input), and the check driver in `vplayer.c`'s open path. It reuses stage 6's read-back code paths that already compile in release; to confirm.
- Image: none new if the CRC readback uses the existing DECODE and read path; otherwise one `HP_OP_CHECK` (about 300 B).
- The test stream: one small file in the zip; buildzip's bench-input check covers it.

## Effort (estimate)
2–3 builder-days plus a review: PP check driver and token, the test stream and its CRCs, host tests (`tests/video`), and a sim script with a forced fail.

## Open until sitting 4's data
- Which bands hold on the owner's unit (`vbv`), and therefore the fallback ladder.
- Whether the governor (`vgov`) can stay on during the check, or the check must hold the top band.

## As built (p19-firstcheck, 2026-10-26)

### Where it runs
- `apps/video/vcheck.{c,h}`: the check, pure logic over the VP client (`bcmvp.h`), host-tested against the BCM model with our own decoder behind it (`tests/video/test_vcheck.c`).
- `apps/video/vpcore.c`: the backend's `check` hook runs **once**, in `bring_up()` between `HELLO` and the play's own `OPEN`, only when `vpc.check_due` is set at the start (never at a reload, a wake or a restart).
- `apps/video/vplayer.c`: the gate (`gate_now`), the unit's token, `be_check` (stream loaded, PP boosted, ladder run, token written) and `be_unit_bad` (live invalidation). `vplayer_check_due()` tells the shell to draw "Preparing Video..." before `vplayer_start()`; `vplayer_check_summary()` goes into Save Diagnostics ("video check: ...").
- **The check is due** when the image's gate opens the fast clock (a `clock150` token for the running image, or a developer build's 4b/4c flags), the unit has no token naming that image, and `vcheck.bin` is on the disk with the right size. An image without a token plays at 27 MHz and is never checked (nothing to check). Note: the merged image in this build, `23693ff4`, has **no `clock150` token yet**, so a release built today plays at 27 MHz and runs no check until its re-verify writes the token.

### One band (`band_run`)
1. `HP_VCLK_STAT`: the chip's fallback count before.
2. `OPEN` 320x240 `HP_MODE_FIT | HP_OPEN_OWN_DISPLAY`, with `HP_OPEN_FAST_CLOCK` for the top band (the play's own path to 150 MHz); a lower band is `HP_OP_VCLK` 1 + band after `OPEN`.
3. `HP_VCLK_STAT`: the band reached, its clock, no new fallback (`clock_band` checks the core's speed against the crystal at every switch). A fallback fails the band at once.
4. The shell's frame (`FBADDR(0,0)`, or the COP's published one) into the image's host frame and `HP_OP_OVERLAY` over the whole panel: `video.c picture_blit()` sends nothing under an overlay that covers the picture, so the test pictures never reach the glass.
5. Stage 6's SDRAM readback: 16 blocks of the pattern across the session's FIFO, 4 words of each read back: 0 bad words.
6. The 30 pictures. All 30 packets go into the FIFO first, as the player pushes ahead of its posts, and each one's first and last word is read back through the port before any `FEED` (a wrong word fails the band as `sdram`; the `sd` count in the log includes these 60 words). Then one `FEED` at a time; after each, the player's way of waiting (`vpcore.c` `status()`): one `STATUS` in flight, collected, and `FIFO_RD` (which the image moves only once the packet and its picture are through), at most once a millisecond, never back-to-back calls; the `STATUS` sent after `FIFO_RD` moved says how many pictures came. Then the planes are read word by word over the host port and summed on the PP (FNV-1a over 32-bit words, Y then U then V rows). **Where the planes are** comes from words the merged image already writes: the guard's first-picture words (`VIDEO_GUARD` [8..11]: Y, U, V, the luma stride; the U and V offsets are the same for every frame, `h264_layout`) and `VIDEO_MB_PROG_A`, which `present()` sets to each picture's luma at `CONVERT`. Every picture's luma must sit a whole frame (`(U - Y) / 256 x 384`) from the first's. **No image change.**
7. Each picture: 0 decode errors and 0 concealed blocks (`STATUS` 8, 9), the band's clock (`STATUS` 15), its `FEED` time within two frames (66.7 ms: a stall); the P pictures' mean within 25 ms (75 % of a 30 fps frame, vtrace's pace rule). The key picture's parse alone can take longer, so only the mean is held to the frame.
8. `HP_VCLK_STAT` again: still at the band, no fallback. `CLOSE`.

A band whose chip stops answering (port, timeout, a decode that never ends) fails, and the image is cut and booted again before the next band.

### Verdicts
- **PASS** at the first band that holds, highest first: the token is written (`/.rockbox/vcheck.tok`: `vcheck 1`, `image <fnv>`, `band <n>`, `build`, `date`), whole into `vcheck.tmp`, synced, and put in place by `safefile_replace()` (`vcheck.old` as the replace's old name; a cut leaves the old token or the new one). A band below the top caps the play: no `HP_OPEN_FAST_CLOCK`, the governor's mask at most that band, now and in every later play while the token stands.
- **FAIL** (no band): the play fails with `BVP_E_CHECK`; the shell shows "Can't Play Video / Video check failed" asking for a Save Diagnostics report; each band's lines are in the log and the summary in the report.
- **INCONCLUSIVE** (the image's words not where they should be, or the FIFO refusing the stream; the image taking a packet and giving no picture with no error, `nopic`, or answering for the whole wait without taking the `FEED`, `feed`, both with two more log lines, `vck x` and `vck y`, of the image's own words; the stream file missing or wrong; no memory): logged loudly, **no token**, the play goes on at the stock 150 MHz ("Unchecked plays", below), and the next play checks again; after two inconclusive checks in a start, no more until the next start. A bug in the check can't lock a good unit out of video.

### Unchecked plays (p19-vclk-cap, 2026-10-27)
**The question (readiness review, 0.8.0 should-fix):** `vplayer.c` sets `HP_OPEN_FAST_CLOCK` from the image's gate before it looks at the unit's token, and every exit that runs no check, or no conclusive one, leaves it set. So a play with the stream missing or wrong, no memory for the check, or an inconclusive check runs at 150 MHz with no unit token, and only a live fault steps it down.

**Decision: no cap.** Such a play stays at 150 MHz. Apple's own start-up takes every stock unit's chip to 150 MHz (`../../bcm2722-reference/boot-and-clocks.md`, "Apple's start-up sequence" [silicon S7]). We follow Apple in how the hardware is driven, so a 150 MHz play with the live invalidation's step-down behind it is stock behaviour. The check stays a safety net on top of that, not a gate.

**The crystal was tried and rejected** (the first commit on this branch capped these plays at 27 MHz): at 27 MHz the decoder misses frames on most 320x240 footage. Sitting 1 measured a mean `FEED` of 42.7 ms a picture against a 33.3 ms frame, with 69-203 pictures skipped [silicon]. The sim's capped plays restarted 2-4 times in a few seconds. A cap would make strangers' good units stutter to guard against a rare marginal one, and the live step-down already guards against that one.

**What the player does:**
- One line a play gives the reason: `unit check: unchecked (<why>), 150 MHz (stock)`. The ring holds 55 characters of text, so the line is kept short. Each play also logs its top band once, at its first `OPEN`: `clock: play top <N> MHz`.
- An inconclusive check runs again at the **next play**, with the same one-reboot limit and `VCHECK_BUDGET_US` (4 s). After `VPLAYER_VCHECK_INC_MAX` (2) inconclusive checks in a start, the start gives up and logs `unit check: inconclusive 2 times, no more this start`. Later plays log `(gave up)` and run no check until the next start, so a unit that is always inconclusive doesn't pay for a check on every play. A PASS resets the count.
- `vplayer_check_due()` is false when no check will run (the start gave up, or a bench play), so no "Preparing Video..." screen appears for a check that won't run.

**Exits walked** (`check_fast_clock`, `be_check`, `be_unit_bad`):

| Exit | Clock | Reason logged |
|---|---|---|
| gate closed (no `clock150` token) | 27 MHz, as before | - (nothing to check) |
| unit token for this image | its band, as before | - |
| stream missing or wrong size (`stream_there`) | 150 MHz | `stream` |
| two inconclusive checks this start | 150 MHz | `gave up` |
| bench play (`vplayer_bench_play`) | the bench's own | - |
| check due, runs in `be_check` | top until it decides | - |
| `be_check`: no memory | 150 MHz | `no memory` |
| `be_check`: stream bytes wrong | 150 MHz | `stream wrong` |
| `be_check`: INCONCLUSIVE | 150 MHz; the next play checks again | `inconclusive` |
| `be_check`: PASS / FAIL | the band found / the error screen, as before | - |
| live fault on an unchecked play (`be_unit_bad`) | steps down the ladder, as before | the invalidation line |

**The bench keeps its clock:** no reason line and no change while `bench_play` is set (stage 6, `vbv`'s held bands through `vclk_force`). The labs, the probe and the AAC test call `check_fast_clock(false)` and measure at the gate's clock, as before.

**Tests:** `tests/ui/ipodui-p19-vclkcap.script` on `sim-ui-video` forces each exit with the sim's switches: `vcheck.bin` removed; `.sim_vcheck_wrong`, a byte flipped after the read; `.sim_vcheck_nomem`; and `.sim_vcheck_nopic`, the model's `no_pic` knob, which gives INCONCLUSIVE. For each exit it asserts `clock: play top 150 MHz` and the reason. Two more cases: the play after an inconclusive check checks again and gives up, and the play after that runs no check and shows no "Preparing Video...". It also asserts 150 MHz after a PASS.

### Live invalidation (`vpcore.c`, `be_unit_bad`)
Once a play: a decode error or a concealed block in `STATUS` (not a refused stream: the file's, not the unit's), a clock fallback (the governor's switch landing elsewhere, or, ungoverned, `STATUS`'s clock below the session's), or the image lost (`recover()`, a `FAULT` or a dead heartbeat). The token goes at once (`vcheck_store_drop`, after settling any cut replace), the reason is logged, and the play steps down to the ladder's next band: the governor's mask capped, or `HP_OP_VCLK` now when no governor runs (`vpc_vclk_cap`); a lost image reloads on its own terms (27 MHz after a loss on PLL A). The job driver's chip failures (`bcmjob_chip_failures()`: no heartbeat or the fault word) drop the token at the next play's start. The next play checks again. A damaged file's decode errors also re-check the unit: one second, accepted.

### The test stream
`bcmfw/video/mkvcheck.py`: 30 pictures of ffmpeg's `testsrc2` (moving, every partition size, intra blocks in P pictures, the loop filter at QP 38), 320x240 Constrained Baseline, CAVLC, one reference, one slice a picture; packed one picture a packet (SPS and PPS in the first; SEI and AUD left out): 22,672 B. `--check` regenerates it byte for byte with the same x264 (0.165.3222 here). The sums come from ffmpeg's decode; the host test decodes the packed file with `bcmfw/h264` and the check passes against the same table, so two decoders agree. `vcheck_stream.h` carries the file's size and FNV-1a: the player refuses any other file (inconclusive, not a failure). `buildzip.pl` puts it in the hidden folder in every build with the player.

### Time
- Model, at the decoder's fitted FEED (`1.3 ms + cycles / 125.7`, cycles from `vclk.h`'s cost model for this stream: 11.3 ms a picture) and the port's read cost (data-paths.md R2: 0.5-1 us a word, hypothesis): **0.78 s at 0.5 us, 1.21 s at 1 us** a word (about 920,000 reads, almost all the planes: 28,800 words a picture). The planes' readback dominates; the decode is ~0.34 s.
- Simulator (`ipodui-p19-vcheck`, the model's 20 ms decode for the script's shot): about 1 s.
- Worst case on a failing unit: seven bands; a sum failure stops a band at its first wrong picture, a fallback at once.
- Silicon, sitting 4c (the whole planes): `vck 150 PASS ok p29 rc0 30/150 MHz 7993ms`, `w6226/23855`. The decode is about 0.2 s and OPEN, overlay and readback about 66 ms (the sitting 4b bands that timed out at p0 took 466 ms against the 400 ms wait), so the 864,000 single-word reads cost about **8.9 us a read** [silicon, derived], far over the hypothesis. The UI thread waits through it (hang record "ui no progress 6 s").
- **Sampled since (p19-4c-fixes):** each picture's sum takes one whole row of each plane in every macroblock row (`vcheck.h` `vcheck_sample_row`: every macroblock of every picture is in its sum; the row inside the macroblock moves with the macroblock row and the picture, U and V half a block apart, so over the 30 pictures every row offset is read, the rows next to the edges the loop filter touches among them). 2,400 words a picture, 72,000 for the stream. Model at 8.9 us a read and silicon's FEED times: **0.91 s** (`test_vcheck` asserts at most 1 s). `mkvcheck.py` makes the table from the same rows (`sample_row`); `--check` still regenerates both files byte for byte, and `vcheck.bin` is unchanged.

### Memory and power
`vcheck.o` 5.5 KB and `vplayer.o` +2.9 KB text in the player's builds; no new large statics (the token code's 112-byte verdict line); the stream (22,672 B) and the readback's 1 KB come from `core_alloc` for the check only. The PP is boosted for the check (about a second, once per unit and image) and unboosted after. Storage: one small read a play start; one write after a pass; one remove on an invalidation. Identical on HDD and flash.

### Sitting 4b: no picture at any band, and the fix (p19-vcheck-silicon)
**What silicon did** (2026-10-26, build 1663b48a25, merged image `23693ff4`, `bench-20261026-1727`): the check ran inside stage 6's plays and logged `vck 150 FAIL chip p0 rc-2 0/150 MHz 435ms`, then the same at 147 down to 102 MHz (466 ms each), every clip. The band's clock record was fine (150 MHz), the SDRAM readback had 0 bad words; the first picture never came within the 400 ms wait while the image kept answering `STATUS` (a stuck image would have ended at the 200 ms `STATUS` timeout, about 235 ms).

**Not the stage context.** Stage 6's second and third passes run "as the player runs it" (no port mode, no C kernels) and failed the same; the check's own `OPEN` never takes the stage's flags; bands 2 to 7 booted the same bytes the resident path holds. A normal Videos play would have done the same.

**The check's own sequence.** The real `vcheck.c` and `bcmvp.c`, built for the Mac and driven into vc2sim running the unchanged merged image (`bcmfw/tests/vcheck_image_test.py`), pass at 150 MHz with 30 exact sums. With the host's SDRAM writes reaching the image only behind a host read of SDRAM or 2 ms after them, the old sequence (each `FEED` sent the moment its packet was written, `STATUS` back to back) gives silicon's line (405-415 ms, the packet taken, `MB_PROG` idle, no picture) and the new one passes. The player never did that: its packets go in well ahead of their `FEED`s. Whether the port's SDRAM writes really trail an SRAM command on silicon is open (BCM open question #73, with the other candidate: this image's first `OPEN` reads the FIFO's first 8 KB from the core).

**The fix (PP side, no image change):** every packet in the FIFO before the first `FEED` and read back (step 6); the wait by `FIFO_RD` and one `STATUS` in flight at a millisecond's pace; `bvp_feed` reads the newest packet's last word back when it is posted as soon as written (the player behind its buffer); a packet taken with no picture is **inconclusive** at once (about 25 ms, not a 400 ms timeout and a reboot), with the image's words logged. `vbcm_model.c` now refuses a `FEED` of bytes not yet behind such a read (`VBM_LAND_US`, 2 ms or 4 KB written after them); `test_vcheck`'s `t_order` replays the old sequence (no picture, a timeout) and the old check fails 14 rows against the model.

### What sitting 4b's data settles
- **The ladder** (`apps/video/vclock_ladder.h`, marked `VCLOCK_LADDER_PENDING`): today the top band, then 147, 140, 130, 123, 116 and 102 MHz, unmeasured. `vbv`'s bands that hold replace the rows; a band that doesn't hold on the owner's unit comes off.
- **Settled by sitting 4c:** the ladder is measured: 150, 147, 140, 130, 123, 116, 102, 87 and 74 MHz held in both of `vbv`'s runs (`VCLOCK_LADDER_PENDING` 0). 59 and 46 MHz held their band 8.3-8.5 s and then read "ran 150 MHz": the clip's loop re-opened the session at 150 and the held band, only a floor to the governor, never lowered it (`vpcore.c` `vclk_want` now asks a held band outright); they wait for a clean `vbv` run.
- **The port's read cost** for the planes (the time above is a range on a hypothesis): the check's own log line gives each band's milliseconds.
- **Whether the check's picture readback meets the guard and `PROG_A` words as modelled** on silicon: a first run on the bench shows PASS with 30 sums, or INCONCLUSIVE `addr` (then the readback, not the unit, needs a look).
- **The check's polling**: it asks `STATUS` back to back while the chip decodes a picture; if its `FEED` times (`w` in the log) run above the play's, a tick's pause goes between the polls.
- Whether the governor (`vgov`) should run during the check (today it holds each band) and a re-check after N months or at low battery (open, needs `vgov` data).

