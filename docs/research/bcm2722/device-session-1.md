# BCM2722 device sessions: runbook (session 11 next)

Issue #60. This is the first time our own VideoCore image (`bcmfw/`) runs on the real chip, on the owner's 5.5G. One install covers both sessions: **session 1** (stages 0 to 3: does our image run, and does it draw) and **session 2** (stage 4: how fast the chip really is, and whether the instruction forms the decoder relies on behave as we think; plus the optional stage 4b). This file supersedes the session-1-only version of itself; `bringup.md` ("Device session 2") explains the bench and how its numbers feed the decoder budget. It's a **test firmware**: the Phase 9 ProkBox plus hidden entries in the developer debug menu (ten test runs, and the boot path's toggle). Nothing about normal use changes, and nothing runs unless you start it by hand. The boot path (our image in place of Apple's at every boot) stays off until you turn it on after a stage has passed.

## The build

| | |
|---|---|
| Branch | `p14-bcm-dev9` = `p14-bcm-dev8` (the 27 MHz default, the upload lab, power-on-first wakes, no strike for a deliberate fallback) + dev9 (the 150 MHz clock in the video image, stages 4c and 5b, the 4b pass flag, the REP 16 store record). Head and the zip's sha256: the #60 comment and the hand-back report; the main session merges dev8 and dev9 together |
| Configure | `--target=22 --type=n --compiler-prefix=arm-none-eabi- --ipodui --bcmfw-devtest --bcmfw` in `build/fw-bcmfw` |
| Artefact | `build/fw-bcmfw/rockbox.zip` |
| Install kind | firmware files only (`.rockbox/`), like the 2026-10-09 install. **Bootloader and Apple firmware untouched.** |
| Embedded images | `probe` 8,668 B; `hello`, `hello_rb` 7,072 B; `hello_dma`, `hello_dma_rb` 8,016 B (unchanged from dev8); `bench` 30,192 B (stage 4; dev9 adds the REP 16 store record); `bench_pll` 31,184 B (stage 4b); `hello_dma_clk` 9,056 B (stage 4c, new). **LCD timing in every image: the 27 MHz words** (Apple's nanoseconds on the measured 27 MHz bus, 14.2 ms a DMA frame; dev8, from session 7's photos). 40 MHz (`-D BCMFW_LCD_BUS_KHZ=40000`, 17.1 ms) and 100 MHz (31.4 ms) are one-line fallbacks. Stage 3's sweep still selects 27, 20 and 15 MHz words one frame at a time (27 now equals the default; the photos are optional). **Every image's hash changed** (the timing words, and `hello_dma`'s new `LCD_POWERON`), so the pass flags from session 7 no longer match: session 8 earns them again. sha256 in `bcm-devtest-images.c` |

## Session 8: one install, in this order (about 40 minutes; 55 with 5b)

The pass flags must be earned again (every image's hash changed since session 7), and step 2 does that. Steps 1 and 9 need no BCM code at all (`independence.md`, §5 steps 1 and 2, on `p14-noapple-plan`). Step 7 (5b) is optional and only if the owner wants a battery measurement this session.

| # | Debug entry or action | Time | Photograph or report |
|---|---|---|---|
| 1 | **Cold boot, watched.** After the install, reset (MENU+SELECT) and watch from power-on to the menu | 1 min | A short phone video, or a description: what shows before our boot glyph (Apple logo, white, black?) and for how long. It settles whether Apple's flash ROM starts Apple's vmcs before our firmware runs (`independence.md` §1) |
| 2 | **BCM test: stages 0-5 (all but 4b)** | about 5 min, up to 20 s of it dark | Photos 1–6 as before; 5a–5g optional (27 MHz is the default everywhere, so 5a/5e should match 5/5d). Stage 5: report whether the scrolling pattern looked clean, whether the screen went dark and came back with the pattern **6 times** (3 the old way, 3 with the panel powered first), and whether the normal screen came back by itself. Photograph the summary (two screens: scroll). The **upload lab** runs inside stage 5 (no photos: its `lab …` lines are the result) |
| 3 | **BCM test: stage 4b (150 MHz, optional)**, only if step 2 passed stage 4 | about 25 s | The re-run at 150 MHz for the new bench's hash. The summary shows "Clock before switch" and "Clock after switch" (about 27 and 150 MHz) and, new, "**Pass flag: stage 4b written**" (it unlocks step 4) |
| 4 | **BCM test: stage 4c (150 MHz screen, optional)**, only if step 3 passed | about 40 s | Scroll to the last line and press SELECT, as for 4b. Three photos: **4c-1** the test pattern while the chip runs at 150 MHz (any colour change, shift or streaks?), **4c-2** the magenta box at the top left, still at 150 MHz, **4c-3** the pattern again after the chip is back at 27 MHz. Photograph the summary: "Stage 4c: PASS", the three frame times and "Pass flag: stage 4c written" |
| 5 | **BCM boot path: DMA (stage 3 or 5)**, then restart | 1 min | Photograph **status**: "Running: hello_dma" |
| 6 | Use the iPod normally on our image | 5–10 min | As session 7 (tearing check first: main-menu drift, fast list scrolls), plus: **let the backlight time out and wake it 5 times**, and say whether waking feels any quicker than before (the panel now powers up while the first frame is uploaded). Report the scrolling and the panel panning compared with session 7 |
| 7 | *(optional)* **BCM test: stage 5b (drift A/B, optional)** | about 14 min, hands off | Only with a stage 5 pass from step 2. First: music stopped, **Developer › power log on at 5 s** (or a USB-C meter between a plain charger and the iPod, with the battery full: note its reading at the end of each phase). The screen shows the drifting test picture throughout; leave it alone. Report nothing; the log and `power.csv` are the result |
| 8 | **BCM boot path: test fallback** | 10 s | A dark blink, then the normal screen. **Status** should show "This boot: fell back: … test fallback: no strike", and **"strikes 0 of 3"** |
| 9 | **Hold MENU at boot** (Apple's firmware), with the boot path still on | 1 min | A photo of Apple's menu, then reset (MENU+SELECT) back to ProkBox. It shows the recovery path survives our boot path being on (`independence.md` §5 step 2) |
| 10 | **BCM boot path: build default**, then restart | 1 min | As before |

**What decides what:**
- **The upload lab** (stage 5, `readlog.py`'s everyday section, lines `lab …`): the host's full-frame upload by Rockbox's own loop, by 8-register bursts and by the PP's DMA engine, each unboosted and boosted, each with a readback check; then the cost of one row's address setup. It decides `BCMFW_UPLOAD` (0 stock, 1 bursts) and whether the PP DMA is worth a driver (only if its readback is `ok`). It also answers PP-bound vs bus-bound: if boosting the PP (30 → 80 MHz) cuts the time about 2.7 times, the PP's loop is the limit; if not, the bus or the BCM's port is. That is the **before/after** for the upload: stock (before) against bursts and PP DMA (after), same frame.
- **Upload vs drawing.** Stage 5's upload lines give the bus time and the time spent drawing the test pattern separately (session 7 logged their sum).
- **Wakes 4–6** (power-on first) against wakes 1–3: the time saved by uploading during the panel's power-on.
- **Stage 4c's PLL frame** (`readlog.py` prints the verdict): about 45 ms means the LCD bus stays on the crystal, about 16 ms half the core, about 8 ms the core's clock; it sets the bus words for video at 150 MHz. **4c-3 must match photo 4** and its frame time the first one's: the switch down left the chip as it was. A 4b flag plus a 4c flag let a video session run at 150 MHz (`pipeline.md`, "The 150 MHz clock").
- **Step 8's strikes** must stay at 0.
- **Step 7 (5b)** is a measurement, not a pass/fail: A (the PP composes and uploads each drift frame) against B. **In session 8's build, B is a stub**: no image there carries the BCM cross-fade job, so B only pushes what the chip already holds. That's the floor any offload can reach, not the offload itself. The real B (the `drift` image, `offload.md` §8a) is on `p14-bcm-5b`, for a later session.

## Phase 18-0 (proposed): the sleep spikes (`p14-sleep-spikes`)

What Phase 18 (sleep and instant resume) needs to know from the video chip: whether its core can sleep and wake, whether its memory survives that, whether the panel can wake without its 265 ms power-on, what each state draws, and whether engine 2 can upload the chip's image at boot. The tests are stage 5i (now with a memory-hold check) and the new **stage 5s**; both are developer-menu entries and need a stage 5 pass flag. The regulator sweep (open question #11) is not in this round: it comes last, after 5i has answered whether the core wakes.

**Build:** `p14-sleep-spikes`, configured like the dev47 build (`--ipodui --bcmfw-devtest --bcmfw --bcmfw-video --bcmfw-l1`). Firmware files only; bootloader and Apple firmware untouched. The images already on the device for stages 0–5 are unchanged, so a stage 5 pass flag from an earlier session still matches. New: `hello_dma_sby` (8,984 B, fnv `d1cecb19`).

**Optional, for the current figures:** a USB-C meter between a plain charger and the iPod (not a computer), battery full, and Developer › power log on. Note the meter's reading in each 20 s dark phase of stage 5s; the power log's marks (`s5s_on`, `s5s_light`, `s5s_deep`, `s5s_reset`, `s5s_cut`, `s5s_end`) give the phase times.

| # | Debug entry or action | Time | What to watch and report |
|---|---|---|---|
| 0 | *(only if step 1 says "Not available")* **BCM test: stages 0-5 (all but 4b)** | about 5 min | As in session 8 step 2: the stage 5 pass flag |
| 1 | **BCM test: stage 5i (idle sleep, optional)** | about 3 min | As before (the "Stage 5i" section below): the pattern twice, a dark minute, two dark blinks. **Photograph the summary**; the new line is **"Framebuffer words changed: N"** (0 means the chip's memory held while its core slept) |
| 2 | **BCM test: stage 5s (sleep spikes, optional)** | about 3 min | Its confirm screen says "Chip sleep: on" if step 1 passed, "off" otherwise. Then, hands off, in this order: **look 1** the colour bars, 15 s; dark 20 s (*meter: panel on, unlit*); dark 20 s (*meter: light standby*); **look 2** the bars after the light wake; two short dark blinks; dark 20 s (*meter: deep standby*); **look 3** the bars after the deep wake; two blinks; dark 20 s (*meter: panel off, chip up*); **look 4** the bars after a full panel power-on; dark 20 s (*meter: chip power cut*); four brief flashes of the bars (the uploads); the normal screen and the summary. **For looks 2, 3 and 4, compare with look 1**: same colours and brightness, nothing shifted, no stripes, no flicker, no washed-out or inverted look; and say whether anything odd showed during each wake (a white flash, garbage, a slow fade-in). If look 2 never comes (the screen stays dark more than 5 s after the light phase), note it: the stage times out and the normal screen returns. **Photograph the summary** (Wake light/deep, Reset/cold, Refused, Framebuffer changed, Upload CPU/E2, Read back wrong). With a meter: the five readings, in order |
| 3 | *(only if step 1 passed)* step 2 again | about 3 min | The same, now with "Chip sleep: on": a second set of meter readings for the sleeping core (the first set had it busy-polling) |
| 4 | Pull the logs | — | `bcmtest.log` and `bcmtest-results.log` (the `5i …` and `5s …` lines), and `power.csv` if the power log ran |

**What the results answer** (`docs/bcm2722-reference/open-questions.md`):
- #3: whether the 1 ms tick and the host doorbell wake the core. From 5i: ticks, doorbells, bad.
- #50: whether SRAM holds while the core sleeps. From 5i's framebuffer check, and 5s's after each standby.
- #49: whether the panel wakes from standby with the right picture, and how fast. The looks, plus the `5s light` / `5s deep` lines, which give the wake time from the host and the panel's own power-on time from the chip.
- #12: what each state draws. The meter readings.
- #51: whether engine 2 can upload the boot image. From `5s upload engine 2: … words read back wrong`, and the image starting.

**Risks:** the same as stage 5i. Our image runs on the chip; every wait is bounded; any failure ends with the power cut and Apple's display driver back, the normal screen. A standby that confuses the panel is cleared by the stage's own full power-on (look 4) and, at the latest, by the power cut at the end. Nothing is written except the two logs; stage 5s writes no pass flag.

## dev47: engine 2 for the player, and a log that says when it loses lines (after session 37)

- **Session 37's detailed log was lost inside the boot**, not by the roll: the 14:07 boot ran stage 7, 8a-0, 8a-1, stage 9 and the E2 lab, and its `bcmtest.log` kept only the boot block and one play's ring, while `bcmtest-results.log` arrived whole. `dt_log` ignored every failure, so nothing said why. Now it retries a failed open once and counts lines, failed opens, failed writes or closes, and the bytes it wrote; **every results block ends with `(bcmtest.log this boot: N lines, B bytes written, file F bytes; …)`**. B ≠ F means writes were lost without an error (the forced UDMA4 write is the suspect: its test verified reads only). Run one stage with UDMA4 forced and one without, and compare.
- **Engine 2 in the player** (`VPLAYER_TUNE_E2`, 0x10): the port's blocks of 256 B to 64 KB from DRAM go through the PP's second DMA engine as session 37's lab ran it (the address as two halfwords, the whole data cache committed, configuration code 2 both ways, command bits 27 and 26 and the count, then start), but with **no interrupt bit** (E2-7(3): then no source rises, so the audio's FIQ is untouched) and no masking. The end is the status busy bit and the start bit falling, polled at the next port access; a block that never ends stops the engine and the CPU copies from then on. Smaller blocks and IRAM sources go by the CPU.
- **The first start after a boot finds no PPS** (session 37c, with a clean log): the host's first packet carried SPS and PPS (`params: … NAL types 384`, bits 7 and 8), went first at FIFO offset 0, matched the chip's copy byte for byte, and nothing was aborted; yet the chip decoded nothing and every slice failed at `slice.c:54` (no valid PPS) until the watchdog's restart sent the parameters again (first picture 3.1 s in). Only the first OPEN after a boot does the SDRAM bring-up, the switch to 150 MHz and the AAC tables' placement. The image now records, cleared at every OPEN, the SPS and PPS units it saw and how they parsed, the valid ids at the first error and the first packet's NAL types (guard words 56-59); the host prints them under the first decode error (`sdram: error: SPS|PPS<<8 seen…`, `valid PPS ids…`, `packets, first packet's NAL types`).
- **The chip-27 rows** of stage 7's A/B show 0 pictures (session 37c): at 27 MHz the chip needs 81-167 ms a picture, so a start 30 s into the file never catches up. Expected; the rows measure the port (DP-1), not playback.
- **The DMA lab** gains a fourth row, "engine 2, SDRAM": the player's own path into the FIFO, each block's pattern naming its row and size.
- **Stage 7's A/B** gains "unboosted, engine 2" and "unboosted, engine 2, chip 27". The player's pieces are at most 2 KB (`VPC_PIECE`), where engine 2 is slower than the CPU's copy (367 µs per 4 KB against about 128 µs per 2 KB, both at 80 MHz); what it buys is a free CPU. The `port DMA ms, blocks` and `thread active permille` lines tell the cost.

## dev46: the data-path audit's fixes

From the read-only audit against `docs/data-paths.md`:
- **Stage 5's upload lab** fills the checked words with a pattern no method writes before each method, so a method that moves nothing reads back BAD (S8's "clean" DMA was the CPU's earlier write; D1). Host case: "PP DMA moves nothing over the CPU's frame".
- **The port's PP DMA is off by default** (`vdma_on` false) and the DMA lab turns it off again when done.
- **Stage 7's A/B** runs the CPU's copy only, as the PP's boost by the chip's clock: unboosted, boosted, unboosted at chip 27, boosted at chip 27 (4 rows). The unboosted chip-27 row answers DP-1.
- **SDRAM bring-up** turns the BCM cache off again after the probe even when a cache-on attempt passed.
- **8a-1** plays silence on the PP's own PCM for the whole run, with the codec held out of standby, so the codec's clocks surely run (R9.6). **8a-0** logs "no PCM ran, so the pins can't be judged" instead of "not on its bus" when the soundtrack never started.
- **The display kernel's zero row** comes from r3, not r7 (a scalar B operand naming r7 reads 0, D4).
- The player counts single-word port reads on their own (35-85 µs each under load, R2), and its DMA comments say dead end.

## dev45: stage 7 measures the video, not whether the audio started (after session 36c)

Session 36c's stage 7 failed steps 1, 2 and 7, and three A/B rows, with 0 pictures. In each one the soundtrack's position did not move: at the start (elapsed 0 for 4.4 s while "playing"), and after MENU and re-enter (29,883 ms for 2 s). Step 7's chip decoded with no errors, but the player's still watchdog read the waiting pictures as the chip's fault and reloaded the image. The COP decode switch was on for some of that session's runs, and 8a-0 saw the PP's audio only with it off.

- **The still watchdog stands down while the audio's clock stands still** (`VPC_AUDIO_STILL_US`, 1 s): it logs `video: audio stands still (audio ms, posted)` once per stall, and does not restart or reload. A real still now also logs `video: still: audio ms, ms since it moved`. Host test `audio stands still` in `tests/video/test_vpcore.c`.
- **The stages that play audio (6, 7, 8a-0/0b, 8b) turn the COP decode switch off for the run** and restore it after (`stage: the COP decode switch is off for this run` in the log).
- **Stage 7 waits for the soundtrack** after each start and resume (up to 4 s) and logs `stage 7: audio runs N ms after the start` or `stage 7: audio did not run in 4000 ms (status, PCM)`.
- **Step 1's other fault**, slices with no PPS (`slice.c:54`, `H264_E_STATE`) on the first start of a fresh boot, cured by the reload: still open. New lines make the next log decide it: `video: params: bytes, NAL types` at every start (bit 7 SPS, bit 8 PPS), `video: packet dropped part way (sample, with params)`, and at the first decode error `video: error: host aborts, params packet at`.

## Stage 9's dump-and-diff (branch `p14-aac-dump`, after session 36c)

Session 36c ruled out the AAC decoder's transport, memory pattern and tables, yet 155 of 200 AUs were refused, and the refusals differed between runs (`bcm2722-reference/open-questions.md` #46). Stage 9 now runs three times: full clock, **full clock with byte copies** (`AAC_FLAG_BYTE_COPY`: the decoder's `memcpy`/`memset` a byte at a time), then 27 MHz. Each run adds:

- `stage 9: memstress SRAM … ; SDRAM …`: 7 counts per memory (`HP_AAC_MEMSTRESS`). Each pattern stores and then loads the same place at once: a word, a byte, a halfword, a byte into a word, and a halfword into a word. The last two patterns are the decoder's own halfword copy and clear. All zero is right; vc2sim prints all zeros. A non-zero SDRAM count with zeros in SRAM is the finding, and the line after it names the first failure.
- `stage 9: trace AU k [i of n]: ele=… info=… cpe=… ics1=… ics2=… gain=… sect=… sf=… sfh=… flags=… spec=… spech=… err=…`: libfaad's parse checkpoints (`HP_AAC_STATE`) for AUs 0–2 and the first wrong ones, six at most per run.
- `stage 9: AU k: ic_stream copy WRONG …`: the second `ic_stream` did not hash the same as the first right after its copy. Every AU is checked.
- On the summary screen (so in `bcmtest-results.log`): "copies wrong N of M" and "stress wrong: SRAM n, SDRAM n" per run.

**After the session:** `bcmfw/aac/test/aac_dev_sim.py --diff bcmtest.log`. It replays each run in vc2sim, with the device's image and clip and the run's flags. For each traced AU it prints the first checkpoint where the chip leaves the simulator, what that checkpoint covers, and everything that agreed before it. For example, `spec` differing after `sf`/`sfh` agreed points at the spectral Huffman decode, and `ics2` differing from `ics1` points at the copy. If the byte-copy run passes while the others fail, the decoder's word and halfword copies into SDRAM are the cause.

## dev44: results that survive reboots (after session 36)

Session 36 lost stage 7, 8a-1 and 9's results: the owner rebooted about six times over the session, and `bcmtest.log` kept one previous generation. From dev44:

- **`bcmtest-results.log`** (system folder): every stage's summary screen, as shown, appended as one block headed `=== <stage>: build <id>, <date time>, up <s> s ===`. It is never rolled at boot. Past 256 KB it becomes `bcmtest-results.1.log`, so two files' worth stay. Pull this file first after a session.
- **`bcmtest.log`** keeps five previous boots (`bcmtest.1.log`, newest, to `bcmtest.5.log`), not one.
- **Stage 8a-1** asks after each tone what was heard ("A clean, quiet tone", "Noise or a screech", "Nothing", "Not sure"). The answer goes into the log line, the summary and so the results file.

## Session (proposed): the resident video image (`p14-resident`)

The video image (`bcmvideo.bin`, 96,456 B, fnv `370a4e04`; install it with the build) becomes the display driver at boot in place of `hello_dma`, so a video starts with no power cycle (`docs/hardware.md`, "Resident video image"). It needs the boot path on (**BCM boot path: DMA**, as in session 8 step 5) and the Developer list's **Resident video image: on** (the default). **Video chip idle sleep** stays off unless stage 5i has passed. Build: `build/fw-ui-video` (`--ipodui --bcmfw --bcmfw-video --bcmfw-devtest`).

| # | Action | What to check and report |
|---|---|---|
| 1 | Restart (MENU+SELECT) and time power-on to the main menu, twice: once with **Resident video image: off** (Developer list), restart, then **on**, restart | Both times. The boot pays one 96 KB read and an 88 KB longer upload (expected +50–60 ms all told). `bcmtest.log`: `boot path: active: bcmvideo …` and `boot path: swap N ms …, image read N ms, at N ms since start`. **Status** (Debug → BCM boot path: status): "Running: bcmvideo", "Resident: running" |
| 2 | Use the menus for a minute, let the backlight time out and wake it 5 times | No tearing or garbage; each wake as before (it uploads the 96 KB image again: about 15 ms more) |
| 3 | Play the episode from the Videos menu, MENU out, play it again; then the 2-minute movie | **No dark gap** at the start or the end: the menu stays lit until the first picture, and the menu comes straight back. `ipodui.log`: `start ms: open …, image 0, scan …, boot N` with N about 10–30 (it was 656 in session 31), and `video: resident: no boot; idle sleeps …`. The player's log: `video: panel handed back, image kept` at each stop |
| 4 | During a video, let the screen go dark, then wake it | As before: the picture comes back in sync (that wake boots the image again); MENU out still hands back without a dark blink |
| 5 | *(after stage 5i passes)* **Video chip idle sleep: on**, restart; menus for a minute, one video | Status "Idle: sleeps N, ticks N, bells N, bad 0", sleeps rising; menus as responsive as before. A freeze of 2 s then a dark blink means the core never woke: the fallback took `hello_dma` (status "This boot: fell back: …"); turn it off |
| 6 | *(optional, idle drain)* Developer list → power log on at 5 s; screen-on idle on a menu (backlight held on, or the unlit face), 30 min with idle sleep **off**, restart, 30 min with it **on** | `power.csv` with the two halves marked; compare `mv` drop per hour with `bl_pct` and `lcd_pct` equal ("Rundowns and A/B tests" in `docs/hardware.md`). With the screen off the BCM is powered down either way |

**If the resident rung fails at boot** (a dark screen of 1–4 s, then the menu on `hello_dma`): status "Resident: failed at this boot", `bcmres.fail` written, later boots skip the rung until the setting is chosen again. Attach `bcmtest.log`.

## dev42 continued (branch `dev42-cont`, after session 35)

Session 35's logs are in `session log, session 35` (`bcmtest.log`, `ipodui.log`). This branch follows up on three of its failures. The video image is now **203,588 bytes, fnv `0ff8365b`** (`apps/video/bcmvideo_image.h`). Build `build/fw-ui-video` as for stage 9.

**(b) "video: sample source failed (32850, 0)", `why=-4`.** Three plays resumed at 18:15 (1,095,191 ms) of the owner's episode gave up 0.5 s in, with 5 KB through the port.
- The engine's `posted` is the seek's own key. The catch-up aim (capped at 1 s) put the target past 1,096,095 ms, so the seek chose key 32,850 (27,772 bytes), and the first push failed. It wasn't 93 samples into a GOP: the key before 32,850 is 32,760.
- Index: the episode's `.vix` is byte-identical whether built at open time or through vixpass's scratch (4 KB to 1 MB), and `--check-index` matches the demuxer, 0 of 38,940 records differing. The window around 32,850 (records 32,786 onward, file bytes 393,464–418,040) is nowhere near a 16-bit or 64 KB limit. No 16-bit field on that path; ruled out on the host.
- Leading hypothesis: the push probes the sample's last byte (buffered), then reads its pieces from the audio's buffer. At a resume, the audio's own seek resets that buffer (`rebuffer_handle`: start = end = the new offset), so a piece then reads "not buffered", which push_body counted as a failure. The simulator's timing doesn't hit it.
- Fixed: a piece that answers "not buffered" drops the packet and waits (logged once: `video: sample's bytes left the buffer (sample, at)`). Any other source failure logs `video: source: why, sample`, `size, rc` and `bytes asked, from` (why: 1 index window, 2 empty record, 3 probe, 4 `bvp_pkt_begin`, 5 piece). It then moves on at the next key (`video: source: on from the next key`), up to 3 times without a picture between, before giving up. The end-of-play log adds `video: end: source skips, piece waits`.
- **Check:** play the episode resumed at 18:15 three times. Expected: it plays; at most one `sample's bytes left the buffer` line, or `source:` lines that name the cause.

**(c) Stage 9: AU 0 refused (status 8, code −101 = libfaad error 1, gain control), AU 1 wrong, AU 2 refused (error 12).** The AU and PCM addresses were the same for every AU. Every run booted fresh already (`power-down: stock order` before each `video image up`), and the tables were placed by that boot's first OPEN, because stage 9 opens an H.264 session before the AAC OPEN. So stale tables were unlikely. Error 1 means a gain-control flag read as set. The decoder's channel elements (`sce`, `cpe`) live in SDRAM and are written by byte stores, which no session has checked in SDRAM (`open-questions.md` #45). Changes:
- The AU and its PCM rotate through 32 slots of 8 KB from `0x340000` (`memory-access.md` §3).
- Each AU is read back over the port: `stage 9: AU n reads back wrong at word …`.
- `bcm_video_off()` runs before the boot.
- The AAC OPEN line always logs status, code, rate and channels, and `tables same/DIFFER (parked, SDRAM, file)`, which is the FNV-1a of the parked copy and of SDRAM's copy against the image file's bytes.
- `stage 9: memprobe SRAM/SDRAM ok/WRONG: …` comes from the new `HP_AAC_MEMPROBE`: byte and halfword stores, with the host's own read of the SDRAM words.
- Refused AUs are counted, and the run goes on past them. A new summary line gives `refused AUs, AUs read back wrong, OPEN code, tables, memprobe`.
- **Check:** run stage 9 and photograph the summary. Read the log in this order: memprobe SDRAM (WRONG settles #45 and says the channel elements must leave SDRAM or be written by words), tables, read-back errors, then refused and matched counts.

**(d) Stage 7 step 2 (pause 2 s, resume): drift 303 ms, 1 GOP dropped.** The resume seek did fire: `resumed past the pictures (ms), seeking (256, 4226)`. It went to the key before the audio, about 90 pictures back in stage7.m4v's 3 s GOPs. The seek's first picture came 1,499 ms later and 512 ms late, with 121 push waits, and its GOP dropped. Fixed: when that key is behind the pictures already sent, the engine decodes on from where it is, hidden, up to the audio. The log says `resumed past the pictures (ms), decoding on` (`vp_catch_up`). Past a key, it still seeks. The `seek's push waits` line now splits "data not yet read" from "FIFO full".
- **Check:** stage 7 step 2 passes, with drift under 50 ms, no GOP dropped and `decoding on` in the log.

Host tests: `tests/video/run.sh` (with new test_vpcore cases for the buffer reset, a failed read, a file that fails for good, resume decode-on and resume past a key), `hostproto_test`, `devtest_test`, `bcmfw/aac/test/run.sh`, `bcmfw/h264/test/run.sh`, `video_test --only aac` (MEMPROBE and the tables in vc2sim) and `mkvideo --check`/`mkdevtest --check`. All pass.

## Session 34 or later (proposed): stage 9, AAC on the chip (`p14-aac-m4`)

M4 of the AAC port (`audio-spike.md` §7). One debug entry, about 10 seconds, silent: nothing is played and no pin is driven. The chip decodes a short synthetic clip's access units and sends the PCM back; the iPod compares each block's hash with the computer's decode of the same bytes.

**Before the session (on the computer):**
1. Build the firmware: `build/fw-ui-video` (`--target=22 --type=n --ipodui --bcmfw --bcmfw-video --bcmfw-devtest --compiler-prefix=arm-none-eabi-`), `make -j10 && make zip`. The zip carries the new video image: **202,180 bytes, fnv `bc9d13d8`** (`apps/video/bcmvideo_image.h`; the player refuses any other file).
2. Make the test files: `bcmfw/aac/test/mkdevclip.sh` (default: the first 200 access units, 4.6 s, of `chirp_at128`, the AudioToolbox stereo 128 kbit/s clip with transients; synthetic, no owner media). It writes `build/aac/device/aac9.au` (75,027 bytes) and `aac9.ref` (1,612 bytes; PCM fnv `33d8cfc9`). Never committed.
3. Install the zip as usual (backup first), and copy `aac9.au` and `aac9.ref` into the system folder's `bcmtest` folder, beside `stage7.m4v`.

**On the iPod:** Developer › **BCM test: stage 9 (AAC on the chip)**, then SELECT on "Run stage 9 now". The screen stays on the menu while the chip works (the BCM is held, as in the DMA lab), then shows the summary. **Photograph it** (it scrolls: two photos).

| Line | Means |
|---|---|
| `full clock: PASS` / `FAIL` | every AU's frames and PCM hash equal the computer's, at the session's clock (150 MHz when the 4b and 4c pass flags are on the disk, else 27 MHz: the `MHz` line says which) |
| `200/200 AUs matched`, `first wrong: AU n` | how many matched; the first that didn't |
| `n us/AU at m MHz`, `(max k us)` | the chip's own time per AU (its 1 MHz counter around the decode; tables in SDRAM) |
| `27 MHz: …` | the same again with the chip on its crystal |
| `bcs/bcc probe: PASS` / `FAIL` | the 32-bit `bcs`/`bcc` after `cmp` (the two branch kinds no check has run on silicon) took exactly the branches vc2sim models |

The log (`bcmtest.log`) has a `stage 9 …: bcs/bcc probe` line per run (taken bits against the model's `9dad6252`), a summary line per run (matched, first wrong, rate and channels, MHz, µs per AU and the worst, the PCM hash against the host's `33d8cfc9`), and up to four `AU n differs` lines with the frame count and hash against the reference.

**What decides what:**
- **Probe PASS:** `bcs`/`bcc` become [silicon] (`isa-scalar.md`, "Verified on silicon"). **FAIL:** the decoder's five sites (`huffman.c` ×4, `fft-ffmpeg.c` `pass()`) and the video image's few must change before M5: vc2cc can emit `cmp b, a` + `bhi`/`bls` instead (both verified kinds); the stage's AU results then say whether the wrong branches mattered.
- **200/200 at both clocks:** M4's first half is done: libfaad bit-exact on silicon, with its big tables and channel elements in SDRAM. A mismatch at one clock only points at the clock switch, at both at the decoder or the tables' copy (the first wrong AU says which part of the clip).
- **µs per AU at 150 MHz** against vc2sim's 2.5 ms (375 k instructions, `chirp_at128`'s mean, at 1 IPC): the real IPC with SDRAM tables. Under 5 ms per stereo AU keeps the 5 ms rule (§7); more says move the tables back to SRAM (`build.py` `AAC_FAR`, one line each) or vectorise the IMDCT.

## Session 32 (proposed): dev39

dev39 is one complete build (based on s12 = 35295c5782, with the shell's overlay-while-playing and volume-bar commits). The video image is 95,816 bytes, fnv 9a16d6c8; install bcmvideo.bin with it.
1. **BCM test: stage 7.** Its eight steps, then a ten-run matrix, 8 s each: the PP boosted or not, the feed by PP DMA or the CPU, the thread's floor or not, at the chip's 150 MHz; then two runs at 27 MHz. The matrix rows don't count toward PASS. Each run's log closes with `video: port …`, `thread active permille`, `thread late …` and `watch: CPU MHz` lines.
2. **BCM test: stage 8a-0 (audio pins, listen).** Nothing is driven. It shows edges per second on GPIO 1–3 with the audio stopped, then with the soundtrack playing.
3. **BCM test: stage 8b (a tone through the chip).** A 440 Hz tone goes through the chip at half gain and back, over the soundtrack while the video plays, first at chip 150 MHz and then at 27 MHz. Listen for a steady tone and any picture stutter.
4. The episode and the 2-minute movie from the Videos menu, played normally with the new default (PP unboosted, DMA, floor). MENU out of each.

Not in dev39: the tone on the BCM's own audio pins (8a-1). It waits for 8a-0's result and the owner's go, because it drives pins the PP's I²S may share.

**Session 31 (2026-10-13, `session log, session 31`, 0b4e7dcd67 = dev38 + overlay):** the player's thread was starved.
- "Thread late" ran up to 2.1 s just before every still.
- The port readback was clean (8,869/1,444/1,965 words, 0 wrong) and decode errors were 0.
- The PP sat at 30 MHz between the audio's boosts.
- A start took about 4.7 s: open 7 ms, image 39, scan 47, boot 656, the rest starved.
- dev39 raises the thread's floor above buffering for the whole session. It feeds the port by PP DMA, which session 8's lab measured as clean and as fast as the CPU's copy, so the CPU can sleep. The PP's boost is an A/B arm.

## Session 30 (proposed): dev34

Stage 7, then the episode and the excerpt from the Videos menu. Look for:
- `sdram: still: …`: the chip's counters, its progress word, and the session's first packet as the FIFO holds it next to what the host wrote;
- `sdram: refused: …`: the same, on a refusal;
- `video: seek's first picture (ms, late ms)` and `video: seek's push waits`.

**Session 29 (2026-10-13, `session log, session 29`, cedcaab49e = dev33 + the error screen, image d4a7e259): stage 7 6/8 PASS, no PCM underrun in any step.** The owner: it "seemed to go smoother than before".

| Step | Result | Drift max | Notes |
|---|---|---|---|
| 1 start | PASS, 69 pictures | 18 ms | only after the still watchdog restarted the session (below) |
| 2 pause 2 s, resume | FAIL | 113 ms | the watchdog fired at the resume (11 in flight, shown stuck at 70): the restart's catch-up came 519 ms late, a GOP dropped, and the lead overshot to 100 ms early |
| 3 scrub +30 s | PASS | 29 ms | |
| 4 PREV | PASS | 0 ms | the watchdog fired again: 52 posted, 0 in flight, 0 shown |
| 5 the audio's own FF | PASS | 20 ms | session 28: 1,535 ms; `FEED_HIDE` fixed it |
| 6 dark, wake | PASS | 22 ms | |
| 7 MENU, resume | PASS | 15 ms | session 28: 3,488 ms |
| 8 to 5 s before the end | FAIL | 75 ms | the seek's first picture was 1.25 s late, a GOP dropped; then every picture 50–70 ms early; state 3 is `VPC_DONE` (the last picture up), not a failure |

Findings:
- **Every session started at the file's first sample showed nothing** (step 1's start, step 4's PREV; session 28's episode, refused at its first NAL, also started there). Restarts elsewhere played. In both still cases the chip had taken every packet (0 in flight) and shown none. Both files open the same way: SEI (701 B) and a 239-byte IDR, then 7-byte P_Skip slices. The real image decodes and shows those frames pixel-exact in vc2sim. The cause is open. dev34's `chip_report` logs the STATUS counters (decoded, errors, concealed, queued) and the image's progress word. It also reads the session's first packet back from the FIFO and logs it beside the host's bytes. That will tell "the chip never got the right bytes" apart from "the chip refused or failed to show them".
- **The watchdog's clock ran through pauses.** It now restarts whenever the player isn't playing. The test model doesn't reproduce the step 2 firing (its first picture after a resume comes within 66 ms), and the device had 11 records in flight. So the step 2 firing may also have been a real stall; the report will say.
- **A dropped GOP's re-push wasn't counted as lateness.** `restart_from` re-writes about 2 s of packets inside the post, after the engine has taken that record's lateness. `sync()` then read the delay as a path error and moved the lead by about 64 ms (step 8: 50–70 ms early from then on). `vp_add_lateness` now adds the re-push time.

## Session 29 (run 2026-10-13): dev32/dev33, the session 28 fixes

**Session 28 (2026-10-13, `session log, session 28`, dev31 / e12d5a4505): three faults on real converted files.**

- **The episode (118 MB, 38,940 samples) was refused at once with `why=5` (HP_E_UNSUPPORTED).** The guard showed `checks 2`: one NAL unit, then the idle check. So the image refused the session's *first* NAL, which is the SPS from the avcC. Everything checked off the device agrees the stream is fine:
  - its avcC is byte-identical to the excerpt's, which plays;
  - our host decoder decodes its first 120 frames bit-exact;
  - our demuxer indexes it cleanly (433 keys);
  - it plays without a decode error in the simulator, through the real buffering, m4a codec and decoder (`build/sim-ui-video`, owner's file kept local).

  The cause is open. dev32 makes the image record the refusal: the check (`h264_refused_at`: file << 16 | line), the NAL's type, length and first 8 bytes, and the packet's offset, length and first 4 bytes, in guard words 48–55. The player logs these as `video: refused …` / `sdram: refused: …` and puts them in `vplayer_info`. It then retries once with a reloaded image, reading every byte from the file instead of the audio's buffer. The retry is logged as `video: refused, again from the file`.
- **No picture for the first ~7.5 s of stage 7 and of the excerpt from the menu.**
  - The excerpt is black only for its first 2 s; it fades in by 3 s.
  - The emulator runs the real image over the excerpt's first 200 frames pixel-exact (`video_test --clips`), at 17 ms a frame.
  - Stage 7's clock trace has no entries in its first 3 s, so the PCM never read as playing in that window.
  - The paused-redraw seek in step 3 brought pictures back at once. The cause is open.

  dev32 adds a watchdog (`VPC_STILL_US`, 2 s). Playing, with 3 or more records out and the picture still, it logs `video: still: …` (posted, pushed, in flight, statuses, shown, status pending) and restarts the session where the audio is.
- **Late pictures after every jump.** Stage 7 drift maxima: 1,535 ms after the audio's own fast forward, 3,488 ms after a resume, 1,776 ms at the end. Two things combined:
  - Stage A showed every picture it decoded, so a seek's catch-up from its key played out, late, on screen.
  - A resume seek arrived about 1.5 s stale, just under the jump detector's 1.5 s threshold.

  dev32 adds `FEED_HIDE` for catch-up frames (hostproto.md). It aims a playing seek past the audio by the image's measured catch-up time, s = n₀·per·fr/(fr − per), capped at 1 s. It logs every seek and detected jump (`video: seek …`, `video: audio jumped …`).

Log hygiene: outside stage 6/7, the guard's detail no longer goes to the shell's log. One `video: guard (hit at, checks)` line remains, written only when it changes.

## Session 28 (run 2026-10-13): a converted file from the Videos menu

**Session 27 (2026-10-13, `session log, session 27`): stage 6 PASS, all three passes.**

| Pass | Shown | Skipped, GOPs dropped | A/V max | Hitches | Chip, a frame |
|---|---|---|---|---|---|
| Clip 1, checked | 240/240 | 0, 0 | 21 ms | 0 | 7 ms (max 31) |
| Clip 1 | 240/240 | 0 | 22 ms | 0 | 7 ms |
| Clip 2 (real footage) | 312/312 | 0 | 15 ms | 0 | 18 ms (max 39) |

The pictures are bit-exact against ffmpeg. The clock trace shows the codec boosted to 12–14 during the refill, with the player's longest loop gap now 12–13 ms. The owner: "no lag, and the whole thing looked pretty darn smooth." Every vector kernel is on the vectors and right on silicon; the decode, display and pacing path for a 320×240 Baseline stream is done.

**What stage 6 hasn't exercised** is the shell's everyday path (`apps/ipodui/screens/video.c`, `shell-contract.md`). Stage 6 starts the player itself, plays two short clips start to end at the default position, and never pauses, seeks, dims or leaves. Untested on silicon:

| Path | What could go wrong | Where |
|---|---|---|
| **Start from the Videos menu**, first time and **resuming** mid-file | the pre-roll with a real file's index; the resume seek right after start (`vplayer_seek(elapsed)`) | `ui_video_play` |
| **Pause / resume** (PLAY) | the paused picture held; the backlight timeout applies when paused; resume in sync | `vp_pause` |
| **SELECT overlay** while paused, then **scrub** with the wheel and release | UPDATERECT over a paused picture; the redraw by `vplayer_seek` at the same place; a scrub's seek | `strip_hide`, `scrub_apply` |
| **PREV / NEXT** (chapters, or restart / end), **held** FF / RW | seeks the shell announces, and the audio's own rw/ff the player must follow | `skip_key`, vpcore's seek detection |
| **Dark and wake**: pause, let the backlight time out (the BCM goes off), wake | the reboot, the boot-time seek (dev24), the paused picture shown again | `VPC_ASLEEP` |
| **End of file** | the last picture, EOS, back to the list | `video_end` |
| **MENU** mid-play, then play again | the BCM given back and the normal screen repainted; the resume point kept | `vplayer_stop` |
| **A long file** (10 min or more) | thermal, battery (`power.csv`), A/V drift over minutes, buffering refills mid-file (the codec boost again, now harmless) | — |
| **The converter's output** | Baseline, 320×240, ≤ 768 kbps, refs ≤ 3 (so the frames stay in SRAM); a stream with more references or 640×480 takes the SDRAM DPB and the 2:1 scaler, whose kernels (`disp_box2_*`, `disp_scale2`) have never run on silicon | — |

**Owner checklist (one sitting, about 20 minutes; a converted `.m4v` of 10 minutes or more, 320×240, plus a short one):**
1. **Videos → the long file.** Watch the first 10 s: is it smooth, and is the sound in sync (lips, hits)?
2. **PLAY to pause.** Does the picture hold? Then PLAY again: does it resume in sync?
3. **Paused, SELECT.** Does the bar appear over the picture? SELECT again and turn the wheel to scrub about a minute ahead. Release (SELECT). Does it jump there, in sync, without a grey or green flash?
4. **NEXT** (end, or the next chapter), then go back to the file. **PREV** restarts it. **Hold NEXT** for 3 s (fast forward), release: in sync?
5. **Pause and wait** for the backlight to go off (the timeout setting), then press a button. Does the same picture come back, and play on in sync?
6. **MENU** mid-play: the list repaints normally. **Play the file again**: does it resume where it was?
7. **Let the short file play to its end**: last picture, then back to the list.
8. **Let the long file play 10 minutes** untouched: any stutter, drift or warmth? Report the battery percentage before and after.
9. Copy off `bcmtest.log`, `ipodui.log*` and `power.csv`. The player logs to `ipodui.log` outside stage 6, so the pull needs it.

**Stage 7 (dev31, `p14-dev31`): the checklist's mechanical part, scripted.** Debug menu, **BCM test: stage 7 (a real video's controls)**, plays `bcmtest/stage7.m4v` (a `prokbox-convert.py` output, not in git; the install copies it) for about 45 s. Hands off. It drives the player the way `screens/video.c` does:
1. start;
2. pause 2 s, resume;
3. paused: the overlay's redraw (a seek to the same place), then a scrub +30 s, resume;
4. PREV (restart);
5. the audio's own fast forward +20 s (a held NEXT; the player must follow);
6. pause, `backlight_off()` for 3 s (the BCM goes off), wake, resume;
7. MENU (stop) and re-enter at the resume point;
8. a seek 5 s before the end, played out.

Each step logs to `bcmtest.log` after the run: `stage 7 step n PASS/FAIL: what at t ms; elapsed a -> b; pictures (V6 #i-#j); drift max after its first 15 pictures; totals skipped, GOPs dropped, late; state`. The step passes when pictures moved, the player is alive and the drift stayed within 50 ms. The V6 trace, the player's lines and its clock trace follow. The owner's checklist then keeps only what a script can't judge: look, sound, overlay drawing, the wheel, and the 10-minute run.

The first SDRAM-DPB and 2:1-scaler run (a 640×480 file) belongs in a later stage, once the 320×240 path is proven on real files.

## Session 27: the last kernel, and a stall that was our logging (dev29, `p14-dev29`)

**Session 26 (2026-10-13, `session log, session 26`, two runs):**
- Pictures are right in every pass, and clip 2 looked good. The start stutter remained.
- **The clock trace shows the clock is right; the player stalled.** There were no samples and no posts for 420–520 ms about 100 ms in, while elapsed and wall time agreed.
- The stall is stage 6's own logging. Every `player:` line was an open/write/close of `bcmtest.log` on the player thread, and the guard's ten lines at the third picture landed right there. The everyday player logs to the in-memory ring, so it was never affected.
- Only `pic1.yuv` was dumped: a restart's OPEN rewrote the dump-flag word.
- The luma loop filter fails on call 2 (bS 2, the normal filter). Rebuilt from the self-test's generator, that call's input gives C `0x68` at the failing pixel as on the chip, and one reading reproduces the chip's `0x65`: `vmulhr` takes B signed (`quirks.md` #20).
- Clip 2 ran at 29 ms a picture, with the luma loop filter in C.

**dev29** (image 92,648 B, fnv `c61191de`):
- **Luma loop filter on the vectors:** the average is `(p0 + q0 + 1) >> 1` by add and shift. **Every kernel is on the vectors** (`h264_vpu_bad` = 0), and gen_vpu refuses `vmulhr` with an immediate ≥ `0x8000`. vc2sim's default is the new reading.
- **The start stall's main cause (the main session's read-only analysis): pcmbuf's codec boost.** While the PCM buffer refills after a start, pcmbuf raises the codec thread to about priority 10. The scheduler then runs our video thread, at 16, only after `diff²` skipped passes (`thread.c`), so it barely ran for ~0.5 s. **The video thread now follows the codec's priority** (never above `PRIORITY_PLAYBACK`), as upstream keeps the voice thread level with the codec. This matters for the everyday player too.
- **`vpcore` no longer busy-loops on "not buffered yet"** for the parameter sets: `vpc_source.wait` (the player sleeps a tick) lets buffering run.
- **Stage 6's player lines are buffered** (64 lines in the player's block) and written after the session, with the clock trace. A line per file write was a second, smaller stall. The clock trace adds the codec's priority and the longest gap between the player loop's turns.
- **The dump flags are ORed in**, so a restart keeps the self-test's dumps.

**Owner steps:** **BCM test: stage 6** (three passes). **Expect no stutter at the start** (the codec boost and the logging both covered; the trace's loop gaps should stay under ~50 ms), 0 skipped on clip 1, and clip 2 faster than 29 ms a picture now that the luma loop filter is on the vectors.

## Session 26: a video clock that slews (dev28, `p14-dev28`)

**Session 25 (2026-10-13, `session log, session 25`): all six passes exact** (Y `ef7ebbd4` U `d9232e8c` V `952f7e14` and the second picture; converted `044663c4`). The vectors run at 8 ms a picture, and in steady state pictures run 3–20 ms late. The start-up leap remained under dev27's clock: elapsed jumped about 430 ms roughly 100 ms in, then drained back over a second, so the PCM chunks written during the prefill carry the decoder's position. Each pass dropped a GOP (12–26 skipped). The dumps were still missing, and the `ui_log` ring overflowed, so the diagnostics never reached a file.

**dev28** (image unchanged, 92,620 B, fnv `a9bd0477`):
- **The video clock is wall time since the PCM started**, pulled toward the elapsed-based estimate by at most 5% of the time passed (`VPLAYER_SLEW`). Only a jump past 1.5 s snaps it (a seek the shell didn't announce); seeks and pauses re-anchor. A start-up leap can't drop a GOP any more, in stage 6 or the everyday player.
- **Stage 6 diagnostics go to `bcmtest.log`:**
  - a start-up clock trace (`player: clock at … ms: elapsed …, pcm on/off, video clock …, elapsed-based ±…`) for the first 3 s;
  - the dump-skip reason;
  - each failing self-test's failing call.
- **Stage 6 measures what ships** (owner): pass 1 is clip 1 with the self-test and dumps, pass 2 clip 1 as the player runs it, pass 3 clip 2.

**Owner steps:** **BCM test: stage 6** (three passes). Expect no stutter at the start, 0 skipped, and every picture right.

## Session 25: the DAC's clock, and the luma loop filter in C (dev27, `p14-dev27`)

**Session 24 (2026-10-13, `session log, session 24`):**
- **Chroma is exact in every pass**, so the chroma plane fix (`quirks.md` #19) is confirmed.
- **Luma is exact only where the luma loop filter ran in C** (passes 2 and 5). The self-test names only the luma loop filter. Its first wrong byte is row 4, col 14 (p1 of the vertical edge): chip `0x65`, C `0x68`, source `0x69`. `0x65` is what the bS 4 filter computes for p2, which suggests the p-side store landed one lane over, but the log doesn't say which call failed.
- **No dumps were written**, and the reason isn't known; dev27 logs it.
- **A start stutter:** the audio clock leapt about 420 ms between pictures 2 and 3. dev26 froze the clock until `elapsed` first moved, while the audio was really playing: `elapsed` is the DAC position at the start of the PCM chunk playing (`pcmbuf.c` stamps the chunks), and its first update came late.

**dev27** (image 92,620 B, fnv `a9bd0477`):
- **Audio clock:** it extrapolates from `elapsed`'s last change only while the playback channel is really playing (`mixer_channel_status`). Before the PCM starts it holds at `elapsed`, and a stall holds after 600 ms. Any leap of more than 100 ms against the extrapolation is logged (`audio clock leap …` in `ipodui.log`).
- **The luma loop filter runs in C again** (`H264_BAD_DBK_LUMA`); the chroma filter stays on the vectors. The self-test still tests the vector version.
- **Dumps:** the player logs why it skipped one (`dump n skipped: state, flags`).

**Owner steps:** **BCM test: stage 6**. Every pass should match pass 2's picture exactly (Y `ef7ebbd4` U `d9232e8c` V `952f7e14`), with no stutter at the start. Copy off both logs (`ipodui.log` now matters too) and any dumps.

## Session 24: every vector kernel fitted (dev26, `p14-dev26`)

**No device session in between: the session 22 dumps were enough.** vc2sim now models four silicon readings, each fitted byte for byte to a chip dump, and Apple's 22,163 self-test cases pass under all of them together:
- plain loads align down to their size;
- a horizontal B operand's +rN aligns its x down to 16;
- a `vstr` writes only within its address's 16-byte line (both loop filters, q side lost at line boundaries);
- a scalar B operand in r7 reads as 0 (intra 16×16 plane, off by exactly −7c).

**Session 23 (2026-10-13, `session log, session 23`), meanwhile:** colours right in every pass, and pass 1's luma exact. Pass 1's chroma was still off: vector intra chroma, whose plane mode also used the r7 scalar; the self-test missed it because its input gave c = 0. Every pass also restarted its session about 0.25 s in: picture 0 measured **exactly 200 ms** late. That is the player's audio clock: while the codec still buffered, "playing" with the elapsed time stuck ran the extrapolation to its 200 ms cap. The restart then re-ran both self-tests in OPEN. In pass 2 (all C, over budget) a later restart showed a grey band until the next IDR; still open.

**dev26** (image 92,616 B, fnv `ffb3dbca`):
- **MC luma:** its 6-tap sums put every misaligned tap in an A operand, with the aligned 16-bit sum as B.
- **Loop filters:** the vertical edge stores p from `pix − 4` and q from `pix`, each inside one line.
- **Plane modes (luma and chroma):** 7c (or 3c) goes in r3.
- **No kernel runs in C any more** (`h264_vpu_bad` = 0). gen_vpu refuses the scalar r7 form too.
- **Audio clock:** `audio_us` extrapolates only once the elapsed time has moved since the start or a seek (the 200 ms artefact behind session 23's restarts).
- **The self-test and its guard words run once per image start**, not again on a session restart.
- **Cross-fade kernels** (stage 5b's drift image): their r7 multipliers become immediates. Only the drift image changes (`mkdevtest`); every flagged stage image is byte-identical.
- vpu_test: every kernel passes under the silicon readings. Two test realism fixes: MC destination strides stay on 16-byte lines, as the decoder's do; and tC0 is derived from bS, as Table 8-17 does.

**Owner steps:** **BCM test: stage 6**. **Every pass, pass 1 included, should match pass 2's picture exactly** (fingerprints Y `ef7ebbd4` U `d9232e8c` V `952f7e14`), at about 7 ms a picture on the vectors. **The self-test should report no wrong kernel in either run.** There should be no restart and no skips at the start; pass 2 (all C) is over budget and may still drop late GOPs.

## Session 23: alignment, and a pre-roll (dev24, `p14-dev24`)

**Session 22 (2026-10-13, `session log, session 22`):** intra 4×4 is fixed. The transform is not: its rows 1–3 still read coefficients 0–3 with plain loads from `r3 = r2 + 8, 16, 24`. That is **alignment**, not a dropped offset: a plain `vld16` aligns its address down to 32 bytes. Fitting the per-kernel dumps in vc2sim gives two rules, now its defaults:
- **a plain vector load aligns down to its size** (`v_ld_align = width`);
- **a horizontal B operand's `+rN` aligns its x down to 16 bytes** (`v_b_reg_x_align = 16`).

Under them vc2sim reproduces the chip's transform and **MC luma (all 1,536 bytes)** exactly, and Apple's 22,163 self-test cases all pass. The loop filters (some edges left unfiltered on silicon) and intra 16×16's plane mode (off by 1–3) are not explained by these rules.

**Frame skips (the main session's read-only analysis):** all 14 skips, the dropped GOP and the late posts came from the first second. The audio started while the image was still booting (about 300–600 ms), so picture 0 went up about 520 ms late.

**dev24** (image 90,636 B, fnv `d14fbc98`):
- **Transform:** one aligned `vld16` of the 16 coefficients, then rows 1–3 staged from row 0 through 8-bit A operands. A 16-bit H row holds its lanes' low bytes at x 0–15 and their high bytes at x 16–31, so both halves are moved.
- **Still wrong on silicon, run in C until fitted** (`H264_BAD_*`): MC luma, both loop filters, and intra 16×16's plane mode. The self-test keeps testing their vector versions, and vpu_test reports MC luma as "known bad on silicon".
- **Pre-roll:** `vplayer_start` returns once the session is up (bounded at 2 s), so the audio starts after the boot. Waking from dark and reloading now seek past the boot time.
- **Tests:** test_vpcore 6a (a 500 ms boot): 0 skipped and 0 GOPs dropped at start, and after a wake from dark.

**Owner steps:** **BCM test: stage 6**. **Pass 1 should now match pass 2's picture exactly** (the fingerprints too), at a few ms more per picture than before, and no pass should skip at the start. Copy off the logs and dumps.

## Session 22: offsets that silicon drops (dev23, `p14-dev23`)

**Session 21 (2026-10-13, `session log, session 21`):** the cleared-VRF self-test still found the same six kernels wrong, and **the clean VRF dumps equal vc2sim's in every test except the transform and the luma loop filter**. The transform's dump shows the cause directly: **VRF rows 1–3 all hold coefficients 0–3**. dev20's `vld16 d0r, (r2+8r)` loaded every row from `(r2)`: the offset was dropped, exactly as dev19's `b00.t+r3` had dropped its register offset (hence the identical outputs). Running the whole image self-test in vc2sim with both offsets dropped (a vector memory op's offset without a stride register, and a B operand's +rN without ++) reproduces the chip's records for the transform (`v6d C6c`) and intra 4×4 (`v64 C66`) exactly.

**But these readings are not general.** Apple's code fails 548 of its 22,163 self-test cases under them, and Apple's decoder works on silicon, so some narrower distinction separates Apple's uses from ours (perhaps the offset's size or alignment). vc2sim keeps the Apple-consistent defaults (`v_mem_off_nostride`, `v_b_reg_nostep` = `applied`), with `ignored` available. Intra 16×16, MC luma and both loop filters are not explained yet.

**dev23** (image 90,504 B, fnv `ba537646`):
- the transform computes each row's address with a scalar `add` and loads with no offset. The intra 4×4 filters stage `e[i+1]` through an A operand (any x) into row 50 instead of B `+r5`. gen_vpu refuses both forms. Every kernel test passes under both readings;
- **every** failing self-test kernel dumps its own source, vector and C areas, plus the failing call's index (`selftest<n>.bin`, 11 × 8 KB at SDRAM `0x3E0000`), so the remaining four can be fitted offline like the transform;
- logs (owner): `bcmtest.log` rolls to `bcmtest.1.log` at each boot (`bcm_boot_early`, main thread); stage 6 deletes the last run's dumps first and logs `stage 6 run N since boot: build …, video image … fnv …`.

**Owner steps:** **BCM test: stage 6**; say whether pass 1 (vectors) looks right. Copy off `bcmtest.log`, `bcmtest.1.log`, the dumps and `ipodui.log*`.

## Session 21: the same start on chip and simulator (dev22, `p14-dev22`)

**Session 20 (2026-10-13, `session log, session 20`):** every fingerprint was as in session 19. The VRF dumps differ from vc2sim's in every test, but mostly through **state neither side shares**. The chip's VRF holds whatever it held since boot (high bytes `03`/`04` in 16-bit lanes), and vc2sim's starts at zero. Two unrelated kernels (transform DC, intra 4×4) show the same 1,004-lane difference pattern, and the colour kernels' VRF differs while their outputs are right. So a whole-VRF diff can't name an instruction yet. Separately, every decoder kernel passes vc2sim's tests with the VRF pre-filled with `03` or with random bytes, so stale lanes alone don't explain the wrong outputs.

**dev22** (image 90,344 B, fnv `f3e7d3d1`): the self-test runs twice. The second run clears the whole VRF to zero (`video_vrf_clear`) before every vector kernel, as vc2sim starts, and the VRF dumps come from that run. The log adds `stage 6 self-test from a cleared VRF: wrong: …`.
- **"none" there** while the first run is wrong: the kernels read lanes that silicon never writes (a write that silicon drops, which vc2sim models as made).
- **Still wrong:** the clean dumps differ from vc2sim's only where the chip computed something else, and the first such byte names the instruction.

**Owner steps:** **BCM test: stage 6** as before; copy off `vrf1.bin`/`vrf2.bin` and the other dumps with the logs.

## Session 20: the vector registers themselves (dev21, `p14-dev21`)

**Session 19 (2026-10-13, `session log, session 19`):** pass 2 (all C) matched every expected fingerprint and ffmpeg's frame 0 byte for byte, which confirms the C path and the dump selection on silicon. The vector passes were still wrong, drifting through each GOP (the owner saw pixels below the moving bar change in time with the counter, refreshing each second). **The self-test dump was byte-identical to session 18's**, although dev20 replaced the transform's register-offset row split with four plain `vld16` loads. So "all rows equal row 0" is not about filling the rows; it is how they are read or computed. The other five kernels still differ by 1–3 levels at their first pixel. Session 18's conclusion about the `+rN` form (`quirks.md` #17) is therefore unproven.

**dev21** (image 90,184 B, fnv `343f5027`): after each self-test kernel's vector run, `video_vrf_dump` writes the VRF's rows 0–31 (both 32-byte halves) to SDRAM. Only the first block uses a store straight from where it is; the rest is moved there with `vmov` first. The player saves the eleven dumps as `bcmtest/vrf<n>.bin` (22,528 bytes) next to the other dumps. vc2sim's dump of the same run is the reference: the first VRF byte where the chip differs names the instruction.

**Owner steps:** **BCM test: stage 6** as before; copy off `vrf1.bin` and `vrf2.bin` with the other dumps and the logs.

## Session 19: the vector transform's row split (dev20, `p14-dev20`)

**Session 18 (2026-10-13, `session log, session 18`): the all-C decode is bit-exact on silicon** (`pic2.yuv` equals ffmpeg's decode byte for byte). The owner: pass 2 "almost perfect", pass 6 (only the transform in C) "looked good again", every pass with the vector transform "a bit off". The self-test dump (C now right) gave the vector transform's exact input and output. The chip's 16 bytes are reproduced exactly when **all four coefficient rows equal row 0**. The kernel split its one-row load with `vmov.1 d0r, a000, b00.t+r3`, and on silicon a 16-bit B operand ignores its +rN. No vc2sim hypothesis or structural variant of the transform reproduced it; the row-split search did, uniquely. The dump and fingerprints also came from a restarted session's first picture (frame 60), not the clip's.

**dev20** (image 90,056 B, fnv `fe4953f3`):
- `h264_vidct4_add` loads each coefficient row with its own `vld16` (offsets 8, 16, 24). gen_vpu refuses any `.t+rN` B operand.
- vc2sim keeps +r applied by default: Apple's own kernels use 16-bit B operands with +r and are right only with it, so the rule is narrower than "ignored" and still open. `HYP v_b16_reg` = `ignored` reproduces the S18 dump. Every kernel test passes.
- The dump and the fingerprints come from the first session after the image starts.
- The reference records `quirks.md` #17, the S18 confirmation of #16, and #14's explanation.

**Owner steps:** **BCM test: stage 6**, pass by pass. **Every pass should now look right**, with the vector passes at about 7 ms a picture. The self-test should report no wrong kernel.

## Session 18: signed halfwords (dev19, `p14-dev19`)

**Session 17 (2026-10-13, `session log, session 17`): the cause of the colour.** The self-test dump's source area matches a Python model of the self-test byte for byte, so the chip's scalar arithmetic, byte stores and copies are right. The C inverse transform's 16 output bytes (`255 0 0 255 / …`) are reproduced exactly by an interpreter of its 66 instructions in one way only: **the 32-bit `ldsh rd, (rs+off)` loads zero-extended, while the 16-bit `ldsh rd, (rs)` sign-extends.** Negative coefficients read as 65,513 and saturated. The decoder reads signed 16-bit data this way everywhere (coefficients, motion vectors, dequantisation), hence the wrong luma and chroma in every pass. The all-C pass also wasn't repeatable between runs: half-read values depend on whatever sits next to them. Stage 4's bench had checked `ldsb` in all three forms but never `ldsh` with an offset. The vector transform still differs from the right result by a few levels (`109 110 106 105` against `108 105 103 103`); that is the next question.

**dev19** (image 89,896 B, fnv `c2094805`):
- **vc2cc** emits `ldsh` only as `ldsh rd, (rs)`. Every offset, index or post-increment operand puts the address in r15 first. The stage images don't change (`mkdevtest --check`), so their pass flags hold.
- **vc2sim** models the offset form as zero-extending (`HYP ldsh_off_zext`). Apple's simulated code still passes all 22,163 cases under that reading.
- The reference manual records it (`docs/bcm2722-reference/quirks.md` #16) and corrects #8's sessions.

**Owner steps:** **BCM test: stage 6**. Pass 2 (all C) should now show **true colour**. Say pass by pass which ones look right; the dumps and logs come off as in session 17.

## Session 17: the pixels themselves (dev18, `p14-dev18`)

**Session 16 (2026-10-13, `session log, session 16`):** all six passes play, with the colour wrong in all of them. The scalar test passes every case. **No fingerprint matches, luma included, in any pass**, and each kernel choice gives a different wrong first picture (an I-frame). The self-test's kernels disagree with C at the first pixel of their blocks by a few levels (intra 4×4 `64`/`66`, 16×16 `65`/`66`, MC `66`/`68`, loop filter `68`/`67`), and the C transform saturates (`ff` against `6d`). The forms the decoder uses beyond Apple's code (asr by register, the 32-bit `bgt`/`blt`/`ble`/`bls`, indexed `ldsb`) were checked on this chip by stage 4's bench (`scalar 1ff`). After three rounds of guessing at the cause, dev18 reads the pixels.

**dev18** (image 89,832 B, fnv `49b336d9`): with `HP_OPEN_DUMP` the image copies the first picture's planes (320×240 I420, tightly packed) to SDRAM `0x3A0000`. The self-test copies its three areas (source, vector's, C's; 64×24 each) at its first failing test to `0x3C0000`. Before the session ends, the player reads them over the host port into `bcmtest/pic<n>.yuv` and `bcmtest/selftest<n>.bin` beside the clips, for passes 1 (vectors) and 2 (all C). vc2sim checks the dumped picture against ffmpeg's decode.

**Owner steps:** **BCM test: stage 6** as before, then let the main session copy the two `pic*.yuv` and two `selftest*.bin` files off with the logs.

## Session 16: where the colour goes wrong (dev17, `p14-dev17`)

**Session 15 (2026-10-13, `session log, session 15`): all six passes play.** The vector passes run at 7 ms a picture (all C 35 ms). The colour is wrong the same way in all six, all-C included: "a circle, weirdly distorted and coloured, and a scrolling bar near the bottom". About 14 pictures are skipped and 15 posts late per pass, an ARM pacing question for later. The self-test still finds the decoder's vector kernels disagreeing with C (`0x01ad`); the first case is the C transform giving `ff` where the vector one gives `6d`. A C result that saturates is what a signed right shift running as a logical one would give.

**The reading:** vc2sim decodes and converts pixel-exactly, vectors and C alike, and the panel takes RGB565 right (stages 2–5). So something common to every pass differs on silicon: the decoder's scalar C (parsing, dequantisation, the DC transforms) or our compiler's scalar instructions.

**dev17** (image 89,512 B, fnv `6d32e7a1`):
- **a scalar arithmetic test** at `OPEN`: 16 cases (signed shifts, signed multiply and divide, sign-extending loads, compares, clipping), each computed by the chip from volatile inputs against the compiler's constant folding (guard words 36–38);
- **fingerprints**: FNV-1a of the first two decoded pictures' Y, U and V planes and of the first converted picture (words 40–46). For clip 1 the right values (ffmpeg's decode and `dispref.py`) are:
  - picture 1: Y `ef7ebbd4`, U `d9232e8c`, V `952f7e14`;
  - picture 2: Y `5439a4dd`, U `2fc98770`, V `07a1ed3d`;
  - converted picture 1: `044663c4`.

  vc2sim checks the method against ffmpeg on the corpus;
- the guard is also read once the third picture is up and at the clip's end. Stage 6 reads the player's report before stopping it, which is why session 15's log had no `stage 6 self-test` lines.

**Owner steps:** the same as session 15. The log decides: a failing scalar case names the instruction, and the fingerprints say whether luma, chroma or only the conversion goes wrong.

## Session 15: the colour kernel's base register (dev16, `p14-dev16`)

**Session 14 (2026-10-13, `session log, session 14`): the first moving video.** The passes played exactly when the colour conversion ran in C: pass 2 (all C) showed 200 pictures, pass 3 (C conversion) 224. Every pass with the vector conversion lost the image. The colour was still garbled; the owner didn't note which passes. The self-test, which survived thanks to dev15:
- **low SRAM was changed by the colour conversion only** (flat and ramp), from `0x004`, with `80808080`: the flat test's chroma;
- the decoder kernels disagreed with C on silicon (`0x01ad`: transform, intra 4×4 and 16×16, MC luma, both loop filters); the first was the transform at row 8, col 16, vector `6d` against C `ff`.

**The cause:** `disp_yuv_tile` was the only kernel with **r7 as a vector memory base** (`vst16 d00, (r7)+=r4 rep16` and four `vld8` from `(r7…)`). On silicon a base field of 7 reads as no register, as the stride field's 7 does. So the chroma scratch went to 0, 32, … 480, over the vectors, the VP header and the mailbox, and was read back from there. No other kernel uses base 7. vc2sim now models it (`vc2vpu.py` `v_mem_base7` = `zero`; the old image fails there as it did on the chip), and the assembler refuses base r7 (`vpu_asm.py` `mem()`).

**dev16:**
- the colour kernel uses r6 (`gen_disp.py`);
- the self-test records each failing kernel's first wrong byte and the coefficient buffer's address, for the decoder mismatches;
- the player keeps the fullest guard read.

Image 88,588 B, fnv `71f59ead`.

**Owner steps:** **BCM test: stage 6**, the same six passes. Please say **pass by pass** which ones move and whether their colour is right. Pass 1 (all vectors) should now play. If passes 1 and 3 look garbled and pass 2 is right, the decoder kernels are the next fix; the self-test lines name them.

## Session 14: the self-test that keeps OPEN alive (dev15, `p14-dev15`)

**Session 13 (2026-10-13, `session log, session 13`):** every pass of both runs gave up at the first `OPEN`: `stopped, BCM given back (-2, 0)` (`BVP_E_TIMEOUT`), no SDRAM report, and the event index read `d4d4d4d4`. dev14's self-test ran inside `OPEN` in every pass, the all-C one too. The reading: **a vector kernel under test wrote into low SRAM, over the VP header**, so `OPEN`'s reply could never be read. That is session 12's damage, now inside a test we control. The guard wasn't read, because a give-up at start skipped it.

**dev15:** around every kernel test the image snapshots `0x004–0x1FF` and compares it afterwards. It records which tests changed it, the first word's address and value, and puts the words back, so `OPEN` replies with the verdict. The player reads the guard block on every give-up too. Image 88,488 B, fnv `a4d3000c`.

**Owner steps:** the same as session 13 (below): **BCM test: stage 6**, and say which of the six passes look right. The decisive lines are `stage 6 self-test: low SRAM changed by: …; first at …` (which kernel writes where it mustn't) and `stage 6 self-test: … wrong: …` (which computes wrong).

## Session 13: stage 6, the kernels on silicon (dev14, `p14-dev13`)

**Session 12 (2026-10-13, logs in `session log, session 12`):** every pass failed the same way, paced, lockstep and as-session-11 alike. The owner saw **still, green, distorted pictures**; in some a face was just recognisable. The reading:

- **The overrun theory is refuted, and so is the port reading behind it.** There were 0 handshake timeouts, the longest wait was 47 us, and the packet readback had 0 wrong words in every mode. Each "stall" was ridden out in about 7 us. The port never failed. `index_at()` returned −1 for an invalid index word, and −1 is also `BVP_E_PORT`, so a **torn `HP_EVT_WR`** read as a dead port. Session 11's losses were the same thing.
- **The chip overwrites its own VP header and mailbox (`0x100–0x1FF`) while the first picture is decoded and shown.** The junk is repeated bytes (`23232323`, `10101010`, `08080808`, or zeros on the synthetic clip) in 8-byte runs 160 bytes apart: the shape of a 320×240 picture's chroma plane. The image keeps beating, but the ARM can no longer read its events, so it counts 0 pictures.
- **vc2sim decodes the same image right** (`video_test.py`, every clip bit-exact, the header intact). So silicon and the simulator disagree about some instruction. The prime suspects are the vector kernels that write chroma (intra, loop filter, transform) or the colour conversion. Low chroma values give exactly the green the owner saw.

**The build:** dev13 (the review's fixes) plus dev14. **The video image changes** (88,232 B, fnv `f9cceeb5`; dev15: 88,488 B, `a4d3000c`): it carries the scalar C version of every vector kernel, chosen per `OPEN` (`HP_OPEN_C_*`, `hostproto.h`). It also keeps a **guard block** at `0xDF000` that checks the header after every NAL unit, conversion and panel push, and records the first hit: the phase, the check, the words found, and the first picture's planes. Every pass also runs a **self-test** at `OPEN` (`HP_OPEN_SELFTEST`, `selftest.c`): each vector kernel (transform, intra, MC, loop filter, and the colour conversion on flat 128 and on ramps) against its C twin on the same input, every byte of a 64×24 area compared. The log's `stage 6 self-test` line names the kernels that disagree on this silicon, independent of what the screen shows. The stage images are unchanged. ARM side: a torn index is no longer a port failure. One that stays torn for 25 polls reloads the image as a protocol failure, with its word logged.

| # | Debug entry or action | Time | Report |
|---|---|---|---|
| 1 | **BCM test: stage 6** | about 1.5 min | Six passes of clip 1 (8 s each): (1) vectors, as session 12; (2) everything in C; (3) only the colour conversion in C; (4) only intra prediction in C; (5) only the loop filter in C; (6) only the inverse transform in C. **Say which passes look right (true colour, the counter moving).** No photos: the log has the rest |

**What decides what:**
- **The self-test line names kernels.** Those are wrong on silicon, and the first wrong byte (vector against C) says how. That decides the fix on its own; the passes confirm it on the screen.
- **Pass 2 right, pass 1 wrong.** A vector kernel is wrong on silicon. The single-family passes name it: the one that comes out right when its family runs in C. If none of 3–6 does, MC (in pass 2 only) or two families together are to blame, and the next build splits them. The guard line names the phase that hit the header.
- **Pass 2 wrong too.** It isn't the kernels. The guard line's planes (Y, U, V and stride) and the phase of the hit point at the frame layout or the panel push. For example, U planes near 0 would mean a pointer that silicon computes differently from vc2sim (a 16-bit multiply, say).
- **`stage 6 guard: header intact` but still wrong colours.** The header damage is gone, and the colour fault is in the conversion or the panel path alone (compare pass 3).

## Session 12: stage 6, the host port under load (dev12, `p14-dev12`)

**Session 11 (2026-10-13, logs in `session log, session 11`):** all three passes failed with 0 pictures decoded as the player counted them. The owner saw "little colour screens that looked like distorted frames". The reading:

- **The image was never dead.** Every post-mortem's `HP_STATE` word (`0x130`) is a valid encoded word, `PLAYING`, with a heartbeat that moved between losses (`d2fc2d03`, `88fc7703`, `b6fc4903`, …). The status word read `50420001`. What failed was one ARM-side handshake: `bvp_poll`'s read of `HP_EVT_WR` waited more than `DT_PORT_US` (20 ms), and `recover()` cut the power at once (`why` −1 = `BVP_E_PORT`).
- **Memory is healthy.** 6d's memory test passed: 4096 KB by aliasing, 0 grid words wrong, vector load and store right (`4d400083`). The 6s readback was 0 wrong of 256 words over 4 MB, and 6d's reload decoded a picture into SDRAM (`fe100000`) and reached the conversion. The frames' place (SRAM or SDRAM) made no difference.
- **The post-mortem words around `0x1C8–0x1E8` aren't trustworthy.** Clip 2's read back `23232323` and `10101010`, which aren't in the bitstream. Clip 1's skip pattern (`IDLE 1` with `PROG_N 0`) can't happen in `video_work()` unless `dec` or `st.fatal` is garbage. Both point at a port that is out of step, not at a stray write on the chip.
- **The suspect: unpaced CPU writes into SDRAM through the host port while the BCM decodes.** `bvp_pkt_write` streams each packet with `lcd_write_data` (ldm/stm bursts, one handshake per block), and the player pushes packets ahead, so writes overlap decoding. Apple's `osos` never does this. Its port write (`0x10287be8`, 1.3) takes a lock, sends 256 bytes and up through PP DMA in 64 KB chunks (`0x10286fb4`), and writes only the tail by halfword. Its reads (`0x10287a6c`) wait for `CONTROL` bit 4 per word, as ours do. Every session that passed wrote into SRAM, never into SDRAM under load. An overrun would explain all three symptoms: corrupt pictures, a stalled handshake and junk reads.

**The build:** s12 plus `p14-dev12`. **ARM side only**: `bcmvideo.bin` is unchanged (80,200 B, fnv `85ae307d`) and so are the stage images, so every pass flag holds.

- **A port stall is ridden out.** If the image still answers `HP_STATE` within 250 ms, the stall is logged and play goes on, up to 8 times a session (`vpcore.c` `ride_out`). The image is lost only when that fails. This and the give-up below are the everyday player's behaviour too, not only stage 6's; only the paced, lockstep and readback modes are stage 6's alone. dev13 makes a stall that breaks a write part way safe to ride out (a withdrawn command, the event index written again, the half-written packet dropped).
- **The handshakes are recorded** (`bcm_video_port_stats`): the longest wait of each kind (read address, read data, write), timeouts, and at the last timeout the address, `CONTROL` and `RD_ADDR`.
- **The player gives up on a fatal stream error.** The image's `EV_ERROR` with `HP_E_UNSUPPORTED` or `HP_E_NOMEM`, or `STATUS` in `HP_ST_ERROR`, ends the video and the audio plays alone (the High-profile case from 14a).

| # | Debug entry or action | Time | Report |
|---|---|---|---|
| 1 | **BCM test: stage 6** | about 1.5 min | Four passes, each with the packets read back: (1) clip 1 with **paced** writes (256-byte pieces, each with its own handshake), (2) clip 1 in **lockstep** (nothing written while the chip decodes), (3) clip 2 with **both**, (4) clip 1 written **as session 11** did. Say which passes showed a clean picture. **No photos needed**: the summary is in `bcmtest.log` |

**What decides what:**
- **Passes 1–3 play and pass 4 shows `packet readback` errors or port timeouts.** The overrun is confirmed. The everyday player keeps the mode that worked. If paced writes alone pass, it costs nothing measurable. PP DMA with Apple's chunking is the follow-up.
- **Only lockstep plays.** The port can't take writes while the VPU is busy at all. The FIFO is then filled between pictures (the race-to-idle design allows it), and paced or DMA writes become a speed question.
- **Everything fails with `packet readback` right but port timeouts.** The writes are fine and the reads stall. The `stage 6 port: last timeout on …` line (`CONTROL` and `RD_ADDR`), together with `stall ridden out` times, decides between a longer read bound and Apple's trailing `RD_ADDR` read.
- **`packet readback` wrong in every pass.** Host writes into SDRAM corrupt even when paced and idle, against the 6s readback. The first wrong word's address and value come next.

## Session 11: stage 6 with the frames in SRAM (dev11, `p14-dev11`)

**Session 10 (2026-10-12):** SDRAM came up (attempt 2, field 4, 150 MHz) and the 6s readback was right, 64 of 64. Both clips then lost the image about 250 ms in: the host port timed out, nothing was decoded, and the reload died the same way. `pipeline.md`, "The memory map", has the reading.

**The build:** s9 plus `p14-dev11`. Only the video image changes (`bcmvideo.bin`, 80,200 B). The stage images are byte-identical, so the pass flags hold.

| # | Debug entry or action | Time | Report |
|---|---|---|---|
| 1 | **BCM test: stage 6** | about 1 min | Three passes: clip 1, clip 2, then "6d" (clip 1 again with its frames in SDRAM). Watch clips 1 and 2 for stutter. **No photos needed**: every summary line is also in `bcmtest.log` as `stage 6 summary: …`, and the player's own lines as `player: …` |

**What decides what:**
- **Clips 1 and 2 PASS, frames in SRAM.** The decoder works on silicon. Session 10's lock-up came from the frames being in SDRAM.
- **Clips 1 or 2 lost.** `stage 6 lost …` names the phase (macroblock, loop filter, conversion, panel push) and its address. "The port read nothing" means a bus lock. A reload at 27 MHz that plays means the clock is the cause.
- **6d.** The `stage 6 memory test` line has the size by aliasing, grid errors, and the vector row in each direction. A hang there shows as `stage 6 lost …: memory test, vector load/store`, which answers device item 3 directly. If 6d plays, SDRAM frames work too, and the 640×480 path is open.

## Session 10: stage 6 again, with SDRAM fixed (dev10, `p14-sdram`)

**Session 9 (2026-10-12):** stages 0–5 passed and the boot path ran. Stage 6 played no picture: `OPEN` answered `HP_E_NOMEM` both times (`ipodui.log`: `video: stopped, BCM given back (6, 0)`), the first SDRAM set-up ever run on the chip. `pipeline.md`, "The memory map", has the cause and the fix. The audio played; the `power-down: stock order` line after `video image up` is the player handing the BCM back, not a fault.

**The build:** s9 plus `p14-sdram`. Only the video image changes (`bcmvideo.bin`, 79,112 B, FNV-1a `2f668201`). Every stage image is byte-identical (`mkdevtest.py --check`), so the 2/3/4/4b/4c/5 pass flags still hold and stage 6 still runs at 150 MHz.

| # | Debug entry or action | Time | Report |
|---|---|---|---|
| 1 | **BCM test: stage 6** | about 40 s | As session 9, step 4: photo 6-1 at counter 5, watch clip 2, photograph the summary. The summary now ends with **"SDRAM: up; readback 0/64 bad"**, or which step failed |
| 2 | Only if step 1 still fails: nothing more on the device | | The log's `stage 6 SDRAM` lines carry the register words for the next build |

**What decides what:**
- **`stage 6 SDRAM: up, attempt n`.** Attempt 1 (field 8, cache off) is the expected case. Attempt 2 means field 4. Attempts 3–4 mean the controller needs Apple's cache-on state.
  - `bit 15 seen 0` with a pass means command 7's done bit isn't bit 15 on silicon, and the probe passed anyway.
- **`stage 6 SDRAM at 150 MHz: … again at 27 MHz`.** SDRAM failed on the PLL and worked on the crystal. The clip still plays, at 27 MHz (the `V6` lines' clock says so). The next build looks at the order of the switch and the set-up.
- **`stage 6 readback (6s)`.** 64 words written through the host port into the FIFO and read back. Any wrong word means the host's view at `0xC0000000` and the BCM's at `0xFE000000` don't agree. The first wrong one's offset and value point at a column or row bit.
- **Pictures.** With SDRAM up, stage 6's own bounds from session 9's plan apply (this section's step 1 = session 9's step 4).

## Session 9 (draft): one install, video and level 1

**Status: the build exists: `s9-int`** (dev9 `c7c29d8e7e` + `p14-player` `622d4d61aa` + `p14-bcm-l1` `fbc9481b6e`, merged at `dbb3aebe5f`, then stage 6's real clip). Still to do: merge session 8's integration branch (`s8-int`) into it, then the full gate. It merges

- **`p14-player`** (the video player, stage 6, `bcmvideo.bin` in the zip; it already contains dev9 `c7c29d8e7e`), and
- **`p14-bcm-l1`** (`fbc9481b6e`, #62 level 1: `bcmfw-boot.c`, bounded host-port waits, the early swap, the fallback ladder; also built on dev9),

with `s8-int` to come (with whatever session 8 changes). The two branches' conflicts (`bcm-devtest-{core,hal}.c`, `bcm-devtest.h`, `tools/configure`, the ledger) were pure additions on both sides and are resolved on `s9-int`. `bcmfw-boot.c` is `#include`d into `bcm-devtest-hal.c` above the video section, so stage 6's `bcm_devtest_6_allowed()` keeps seeing `bcmfw_flag()` and `bcmfw_dma_images`. **Configure:** `--ipodui --bcmfw-devtest --bcmfw --bcmfw-video --bcmfw-l1`. After the merge: `bcmfw/mkdevtest.py --check` and `bcmfw/mkvideo.py --check` (every embedded image and the video image unchanged), the full gate, then this install.

**What the zip adds:** `.rockbox/bcmvideo.bin` (the player's image, 78,376 B; its size and FNV-1a are compiled into the firmware, so a stale file is refused) `.rockbox/bcmtest/clip.mp4` (stage 6's first clip: 8 s of ffmpeg's test pattern with a seconds counter and a 440 Hz tone; synthetic, made by `bcmfw/video/mkclip.sh`) and `.rockbox/bcmtest/clip2.mp4` (the second: 13 s of real footage, an excerpt of the *Sintel* trailer, © Blender Foundation, CC BY 3.0, with fast motion, pans and five cuts, at 24 fps; made by `bcmfw/video/mkclip-real.sh`, credited in `README.md`). **Nothing from anyone's library is used.**

**Level 1 changes one familiar behaviour:** "test fallback" now goes down **one rung** (our CPU-push image first; a second press reaches Apple's vmcs), and the boot swap happens right after the disk mounts (hold **Next** while booting to skip it).

| # | Debug entry or action | Time | Photograph or report |
|---|---|---|---|
| 1 | **Cold boot, watched** (level 1's early swap): reset, watch to the menu | 1 min | Where the dark interval falls (inside the boot screen?); `independence.md` §5 step 3 |
| 2 | **BCM test: stages 0-5 (all but 4b)** | about 5 min | As session 8 (photos optional where session 8 settled them). Earns the stage 5 flag stage 6 needs, if the merge changed any image |
| 3 | **BCM test: stage 4b**, then **stage 4c**, only if the hashes changed since session 8 | about 1 min | As session 8. Both flags are what let stage 6 run at 150 MHz; without them it runs at 27 MHz (still a valid run, see below) |
| 4 | **BCM test: stage 6 (play a clip, optional)** | about 40 s | Scroll to the last line and press SELECT. **Clip 1 (8 s, test pattern): photo 6-1 when the counter shows 5.** **Clip 2 (13 s, real footage): just watch it.** Report: any stutter, a jerky pan, a frozen or repeated picture, or a cut that lands late; did the sound play throughout, and did it stay with the picture (the music hits land on the cuts)? Then **photograph the summary**: "Stage 6 PASS/FAIL" and, per clip, pictures, A/V, hitches, chip ms, clock |
| 5 | **BCM boot path: DMA (stage 3 or 5)**, restart; stopwatch reset → menu 3 times, then the same with **Apple vmcs (forced)** | 5 min | `independence.md` §5 step 4 (boot time, swap on vs off) |
| 6 | **BCM boot path: fail next boot's every rung**, restart | 1 min | Status photo (Apple's vmcs, marker kept); step 5 of §5 |
| 7 | **BCM boot path: next wake never answers**, let the backlight time out, wake it | 1 min | No freeze, a dark blink at most; status "Running: hello" (§5 step 6) |
| 8 | **BCM boot path: fail next boot's DMA rung**, restart | 1 min | Status "Running: hello" (the CPU-push rung; needs a stage 2 flag) (§5 step 7) |
| 9 | Recovery drills, **owner's call**: (a) MENU+SELECT during the swap's dark interval with step 6's entry armed; (b) hold Next at boot, choose Apple vmcs (forced); (c) Disk Mode reached and left without changes | 5 min | Photos of each end state (`independence.md` §5, "Recovery drills") |
| 10 | **BCM boot path: build default**, restart | 1 min | As before |
| 11 | **BCM test: stage 5i (idle sleep, optional)**, owner's call | about 3 min | Scroll to the last line and press SELECT. Leave the iPod alone: the pattern, then the light goes off for a minute (the screen stays on, dark), then two dark blinks and the normal screen. **Photograph the summary** (no-op and rect times busy/idle, ticks, doorbells, bad, the flag). See "Stage 5i" below |

About 25 minutes without the drills, 30 with them; step 11 adds 3.

**What decides what:**
- **Stage 6 PASS** = both clips pass: each played to the end; after the first second every picture within 50 ms of the audio; no skipped picture and no dropped GOP (`apps/video/vtrace.c`). The log has a `stage 6 clip n: …` line before each clip's lines, and one `V6` line per picture (PTS, A/V drift, the chip's time for that packet, the clock); `readlog.py` prints, per clip, the drift by second, the slowest packets and the pacing. A FAIL is a result, not a fault: the lines say which bound failed.
- **Pacing (informational, not in the verdict):** between two pictures the screen moved on by the drift's change against the pictures' own interval. A **hitch** is a picture more than half its interval (and at least 15 ms) late against the one before: a visible stutter. On the device the records come from STATUS polls a tick (10 ms) apart, so a few ms of jitter is the poll. The owner's "any stutter?" is the check the numbers are judged against. In the simulator the hitch counts are meaningless (the headless sim's scheduling is irregular under load: 32 and 40 hitches on a run whose deterministic host model, `tests/video/test_vpcore.c`, shows none).
- **The clock line** says whether the session ran at 150 MHz (4b and 4c flags present) or 27 MHz. Clip 1 at 27 MHz needs about 11 ms a picture in the simulator, so it keeps up at 30 fps either way. **Clip 2 needs 150 MHz:** on the VPU model (`sim_test.py`, its first 60 pictures, bit-exact) real footage costs about 2.5 M cycles an I picture and 2.0 M a P picture (2.9 M at most), that is about 13–19 ms at 150 MHz against 41.7 ms a picture at 24 fps, but 75–110 ms at 27 MHz. **So without the 4b and 4c flags clip 2 is expected to FAIL** (skipped pictures and dropped GOPs while the audio carries on): a result that confirms the 150 MHz switch is needed, not a fault.
- **Photo 6-1** is the check that the decoded picture is right on the real panel (colour bars, the circle, the counter), which no log line can show.
- Steps 5–9 are `independence.md` §5's level 1 proofs; their log lines are listed there.

## Results from session 1

The first run (2026-10-10, log kept with the owner's backup):
- **Stage 0 passes** every time: panel ID 3 (`gpio lev1 000d0000`), model variant 1 (info word bits 7:4), `0x10001400 = 0xb5`, Apple's `LCD_UPDATE` 15.2–15.96 ms.
- **Entry 0x200 confirmed.** The probe reached `bcmfw_main` and its self-check (step `50520702`: inside check 7).
- **Stage 1 faulted in all 8 runs**: `fault words: pc 00000a5a sp 000cffec info 50420013 step 50520702`. The info word has no counter bit (`0x200`): no TIMER_OK. `timer_init()` runs and the info word is published before the self-check, so its counter test said "not running". `timer_init()`'s own 1000-iteration loop (`addcmpbne`) has no pre-decrement site, so that test ran as written: over that loop the counter at `0x10001804` did not change (stopped without Apple's set-up, too slow to tick in a few µs, or the wrong address). The next run shows it directly (the `P2` line's `clo` words and `ticks` show the counter directly).
- **Diagnosis.** The 16-bit range `0x7000–0x77ff` is `ld`/`st (--rs)`, pre-decrement, not `cmp #imm5` (Apple's `memmove` proves it; `isa.md`). Check 7's `0x7050` was `ld r0, (--r5)` with r5 = `0x7f`, a misaligned load that faulted. The fault pc is that instruction + 6. Our compiler emitted 8–13 such sites per image, so every image of that build was affected.

## Results from session 2 (2026-10-10, image eee599cddc)

Two logs: stages 0-1 and 0-2 (session 2), then 0, 1 and 4 (session 3). Copies are with the owner's backups (`bcm-session2-*`, `bcm-session3-*`).

- **Stage 1 passes, every run**: required checks `1ff/1ff`, informational `0c00/1e00`, faulted `1200` (checks 9 and 12, as designed), entry `0x200`, command round trip 6–7 µs. The `P2` lines settle the exception model (`isa.md`, "Exceptions on silicon"): a misaligned load takes **vector 1**; the load still **completes from the address rounded down** and writes its register; the exception is **imprecise**, taken 2 (check 9) or 4 (check 12) instructions later with the saved pc on the first instruction not run; a pre-decrement (`ld r0, (--r5)`) **writes rs back** even though its access faults; aligned `ld (--rs)` and `ldb`/`stb (--rs)` behave as modelled. vc2sim now does all of this by default, and `tools/bcmre/device_verified.txt` lists the 13 kinds the probe proved.
- **The 1 MHz counter is stopped** in every image (`ticks 0`, `clo 00000000 00000000`), the bench included.
- **Stage 2 timed out, 2 of 2 runs**: hello booted and reported ready, then its first `LCD_UPDATE` (panel power-on and frame) did not finish within 3 s. No exception (fault words 0); hello v1 wrote no step word, so the log could not say where it was. Power cut and restore were clean.
- **Stage 4 ran to done in 1,528 ms**, with no stall: scalar checks `1ff/1ff`, the probe's `1ff/1ff`, but **vector checks 1 of 18**: only the plain copy (`vld16`/`vst16` of row 0) passed; check 1 (`vmax`) left its output at the fill pattern. Every timing is 0 (the counter is stopped), so the clock estimate reads 0.
- **The core runs at about 27 MHz**, from the bench's step times as the iPod saw them: 22 MIPS over the scalar kernels, 30 M instructions-plus-vector-rows a second over the vector ones (`hardware.md`, "After a host start").

**Diagnosis.**
1. *Stage 2* [high confidence]: not a hang. With the counter stopped, hello's delays were spin loops calibrated at 150 iterations per µs, about 450 MHz worth of instructions: 17 times too long at 27 MHz. Panel 3's power-on waits 265 ms, so it would have taken about 4.4 s, past the host's 3 s. No register in hello's CPU path is polled, so nothing else could spin for ever; an LCD-interface stall (its clock off) can't be ruled out from the log, which is what hello v2's step word is for.
2. *Counter* [medium]: Apple's start-up starts it. The candidates are `0x10001C4C = 0x51B` (27 MHz / 27 = 1 MHz) and `0x10001C58 = 0x101` (`hardware.md`).
3. *Vector checks* [open]: the VPU runs (its rows take time), loads and stores reach SRAM for a copy; what's lost is the result of an ALU operation, or a store issued behind one. The next bench separates the possibilities (below).

## Results from session 4 (2026-10-11, unified build 8417548e75)

Runs 0, 0-1, 0-1-2, 0-1-2-3, 0-1-2-3-4 and 0-1-4 (log with the owner's backup, `bcm-session4-*`; decode with `readlog.py`). Facts with confidence levels are in `hardware.md` ("Device session 4") and `isa.md` ("Vector stores on silicon").

- **Counter running** in every image, started by `+0x4C`/`+0x54`; a true 1 MHz (the host's clock agrees to 0.1%).
- **Stage 1 PASS** every run: `1ff/1ff`, faulted `1200`; P2 `clo 61/d1`, 112 ticks.
- **Stage 2 PASS: our image drew on the real panel.** The owner saw the test pattern, then the same pattern with the magenta box inset at the bottom right: colours, byte order, orientation and the `UPDATERECT` window (200,170 100×50) are device-verified on panel 3. Frame 1 314.7 ms, frame 2 49.0 ms, rect 3.5 ms, panel off 68.2 ms, wake frame 314.7 ms; Apple's 15.5 ms. "Too slow for the boot path's timeouts: no pass flag" is correct and stays: the boot path's 500 ms first-update limit holds, but 49 ms frames are 3.2 times Apple's.
- **Stage 3 FAIL**: the DMA wait ran out at 100 ms (1,562 polls) and the CPU re-sent the frame. Channel 1 read `cs 49032101 int 00002300 sub0 00000000`: the channel shows Apple's "state 2", the block register is cleared and bits 8/9/13 of `+0x04` are set, while we polled bit 0. So the transfer probably ran and our completion test was wrong [M]. Whether DMA pixels reached the panel is not known: the CPU re-send drew the same image.
- **Stage 4**: done in 1,541 ms; vector `52/ffffff`, scalar and probe `1ff/1ff`. vc2sim now reproduces all 24 vector records word for word: the 48-bit imm6 is signed, and a 48-bit `vst16` from a D other than `d00` does not land where modelled. ALU results do reach the VRF.
- **Clock**: the same 3-instruction spin runs 79 M instructions/s (probe, bench, hello_dma) or 40 M/s (hello, same bytes at another address); dependent adds run 26 M/s. The core clock is at least ~79 MHz, not the 27 MHz session 2 inferred [M].

**Frame speed.** 49.0 ms is 638 ns per pixel against 2.75 instructions per pixel in the simulator: the push is paced by the LCD bus, not by the loop. The bus timing words are Apple's, computed in cycles for 150 MHz from per-panel nanosecond tables (@12a70); our clock is slower, so every bus cycle is longer than the panel needs. Two levers, in order: (1) recompute panel 3's timing words from Apple's nanosecond table for the measured clock (no clock write; expected to approach Apple's 201 ns per pixel, about 15–20 ms per frame); (2) stage 4b's 150 MHz (owner-gated) gives the same bus timing as Apple by construction, about 15.5 ms per frame, and about 1.9 times the instruction rate if the clock is now ~79 MHz (not 5.5 times). The pass-flag threshold is not lowered: it was designed for the boot path, and 49 ms frames are genuinely too slow for it.

## Results from session 5 (2026-10-11, unified build 99c9a3781c, dev4)

Runs 0-1, 0-2, 0-3, 0-4, then 0-3 again (log with the owner's backup, `bcm-session5-*`). Facts in `hardware.md` ("Device session 5") and `isa.md` ("Vector stores on silicon").

- **Stages 0 to 3 PASS.** DMA works: frames 1 and 2 and the 2-D rect finished on bit 8, no timeouts; the owner saw photo 4 with no band, photo 5's box at the top left and photo 6's black band (the forced CPU fallback). The `run` lines' "stopped short" was a misreading: the register counts **up** to the run length (`0x12c00` = 76,800 at the end).
- **Frame times:** by DMA 31.4 ms (Apple 15.2 ms); by CPU with our words 39.0 ms, with Apple's 48.9 ms. So the bus runs at ~39 MHz and our 100 MHz words leave each transfer about 2.6 times the panel's minimum.
- **Stage 4: `8ebbd6/ffffff`, 15 of 24.** Every failure is explained, and vc2sim now reproduces the run word for word: a 48-bit `vst16` uses the field a load reads as D for the address (32-byte units) and the load's offset field for the source row. The bench's second and later rows (`vst16 d00, (r1+32)` …) therefore stored rows 1–3 over the first row (CSUM, MXSU, ADIR, MULR, MU8R, SUBZ, LD8R), and COP8's `vst16 d08` wrote row 0 at +256. MHRN: `vmulhrs` into an 8-bit D keeps the low 8 bits instead of clamping. The store-locating lines show the address rule directly.
- **Spin rate:** 78 M instructions/s in hello and the bench, 40 M/s in hello_dma: the same loop at another address (in session 4 hello was the slow one). Open; dev5 measures it.

## Results from session 6 (2026-10-11, build p15-dac 77d5ddaa67, dev5)

Runs 0-4 twice (log with the owner's backup, `bcm-session6-*`). Facts in `hardware.md` ("Device session 6") and `isa.md` ("Ties round away from zero").

- **Stages 0 to 3 PASS**, the `run` lines read "counted up: all out".
- **Bus sweep:** 50 MHz words 19,954 µs a frame, 40 MHz 17,118 µs, 30 MHz 14,277 µs (Apple 15,198 µs); the owner saw no artefacts on photos 5a–5c. The times fit 37.0 ns per bus cycle exactly: **the LCD bus runs at 27 MHz** (not ~39), so the 30 MHz words were already within the panel's nanoseconds.
- **Stage 4 FAIL, 23 of 24:** every check but MHRN passes, so the session 5 store layout is confirmed. MHRN failed in one lane: `vmulhrs` rounds a tie (−16 × 2048, exactly −0.5) **away from zero** to −1, where we expected 0. 4b stayed locked.
- **The clock line was misread:** the timed loop is one inlined `addcmpbne`, not three instructions, so "78 MHz" was 26 M loop iterations/s. The 75-tick images are those whose loop instruction sits at `…c` in a 16-byte line (3 of 3; 9 fast cases elsewhere). The core most likely runs near 54 MHz with 2-cycle dependent results [M]; 4b will settle it.

## Results from session 7 (2026-10-11, build p15-dac 87f1e43c0d, dev7)

Runs 0-5, then 4b, then the boot path (logs `session7-*`, complete in `session7b-*`). Facts in `hardware.md` ("Device session 7").

- **Every stage passed:** 0-3; stage 4 with vector 24/24 (scalar and probe `1ff`); stage 5; and **stage 4b**: all 69 kernels ran 5.56 times faster after the switch, so the core runs **27 MHz from a host start and 150 MHz after Apple's PLL sequence** (PLL A readback `0000006c 00000658`, clock select `00000101`). The summary's "Clock: 27.0 MHz (est.)" was table 1, taken *before* the switch; table 2's ALU1 rate (150,019 kHz) is only in the log. dev8's summary shows both.
- **Bus sweep:** 27 MHz words 14,261 µs (BCM-timed), 20 MHz 14,261 µs, 15 MHz 11,422 µs, 40 MHz 17,099 µs. Every frame completed (done count up, `int 2300`, run count `12c00`: all out). The frame is 76,800 × (4 + the second word's low field) × 37.04 ns: `…/40000002` 6 cycles, `…/40000001` 5 (27 and 20 MHz alike), `…/40000000` 4. So 15 MHz is faster simply because its words drop a cycle, and 20 MHz isn't faster than 27 because both keep 5. The first word's fields (strobe widths) don't change the frame time. **The owner saw no difference between 5e, 5f, 5g and 5d** (fine pattern): the 27 MHz words are dev8's default; 15 MHz looking clean is headroom evidence only, not margin (below the panel's minimums, one unit, room temperature).
- **Stage 5's "upload" times were mostly the test drawing its own pattern**, not the bus. The pattern is drawn a whole 320-pixel row at a time whatever the rectangle's width, and every kind cost about the same per row: list area 590 µs a row (220 rows), two rows 589 (56), progress 582 (8), clock 524 (16). The width-dependent part is the bus: 66 µs for 268 more pixels, **about 0.25 µs a pixel unboosted (roughly 19 ms a full frame)**; the rest, about 520 µs a row (1.6 µs a pixel at 30 MHz), is the drawing. The "full frame: upload 52,649 µs" against the first whole frame's 140,913 µs is a factor of 2.68 = 80/30: the PP was boosted for that loop (something else held a boost); the drawing scales with the clock. dev8's stage 5 times the bus and the drawing separately, and the upload lab measures the bus alone.
- **Wakes:** 436–458 ms from the image upload to the first frame shown: the image boots and reports ready in about 2 ms; the test's pattern upload, about 141 ms (mostly drawing; the real wake uploads the framebuffer, about 19 ms of bus); the panel's power-on and first frame, about 283 ms (stage 3's first frame: 282.7 ms). The power-on's own waits (Apple's script, about 265 ms) dominate. Apple's own wake in the same log: 434 ms (`restore` start to done). dev8 kicks `LCD_POWERON` first and uploads during it.
- **The fallback:** stop detected in 13 µs, Apple's vmcs back in 582 ms.
- **Owner, steps 3-5:** our image as the everyday path showed **no tearing and no flicker** ("looking good", "pretty smooth"). Two wishes: thumbnails load slowly while scrolling, and the panel panning isn't perfectly smooth yet (see `pipeline.md`, "Session 7: what the upload explains"). "Test fallback" counted a strike (1 of 3); dev8 doesn't count deliberate fallbacks.

## What changed for session 8 (branches `p14-bcm-dev8` and `p14-bcm-dev9`)

dev9, on top of dev8's list below:
- **The 150 MHz clock** in its own file (`bcmfw/clock.c`): the 4b bench's switch, unchanged (the bench calls it), and our switch back down [L]. The **video image** runs a session at 150 MHz when `OPEN` carries `HP_OPEN_FAST_CLOCK` (SDRAM refresh and the LCD words follow the clock; `CLOSE`, `LCD_SLEEP` and a lost session come back to 27 MHz). The UI and boot images never switch.
- **The 4b pass flag** (`/.rockbox/bcmpass4b.txt`, "4b bench_pll …"): stage 4b now writes one. It gates stage 4c, and with 4c's own flag (`bcmpass4c.txt`) the video session's 150 MHz.
- **Stage 4c** (new debug entry, confirm like 4b): the clock-command image `hello_dma_clk`; a DMA frame on the crystal, up, a frame and a 2-D rectangle on the PLL, down, a frame again, up, then `LCD_SLEEP` (which must come down by itself). Three photos.
- **Stage 5b** (new debug entry, optional): the drift offload A/B scaffold, phases marked in the developer power log (`powerlog_mark`: `s5_A1`, `s5_B1` … `s5_end`) when the build has it; 3 pairs of 120 s phases after a 20 s settle; backlight on, PP unboosted, no disk reads. Mode B is a stub (push only).
- **The bench's store-locating check** gains an informational record: one `vst16 d00` REP 16 with a 64-byte stride from rows 0–15 (the display path's and the drift kernel's store). `readlog.py` says whether all 16 rows landed (128 words, +0 to +988 bytes).
- **Tests:** devtest 829 cases (4c on panels 1 and 3, 4c refused without a 4b flag, 4b and 4c in one run, the 4b flag's tag, 5b with its marks and the stub), dma 607 (the clock image: Apple's sequence up, ours down, the words follow, sleep comes down), video (the same pictures with and without the fast flag; refresh and timing writes in order).

dev8:

- **27 MHz LCD words in every image** (`bcmfw/config.h`), 40 and 100 MHz as one-line fallbacks.
- **Stage 5:** the bus and the drawing timed separately; the **upload lab** (Rockbox's 2-register loop, 8-register bursts `lcd_write_data_burst8`, the PP5022's DMA engine on channel 3 to the fixed data port; each unboosted and boosted, 48 words read back each; then 240 two-pixel rows for the per-row address cost); **three more wakes with `LCD_POWERON` first**, the frame uploaded during the panel's power-on.
- **`LCD_POWERON`** (`0x40`, ours) in the images, and in `lcd-video.c`'s wake with our image: the kick goes out before the first frame's upload, and the frame's kick waits for it (bounded like a first update; a timeout counts as a stopped image).
- **`BCMFW_UPLOAD`** in `lcd-video.c` (0: Rockbox's `lcd_write_data`, the default; 1: the 8-register bursts), set from the lab's numbers after session 8. Without `HAVE_BCMFW` the code is the stock code.
- **Strikes:** "test fallback" and the device test taking the BCM drop to Apple's vmcs by the same path but count no strike (logged "test fallback: no strike"); the rule is `bcmdt_strike_next()`, host-tested.
- **4b summary:** "Clock before switch" and "Clock after switch".
- **Tests:** devtest 780 cases (the lab on both panels; a PP DMA that writes nothing, one that never ends, a HAL without the lab; the strike rule; six wakes each pixel-exact).

## What changed for session 7 (branches `p14-bcm-dev6` and `p14-bcm-dev7`)

dev7 (the everyday path):
- **Stage 5**, after stage 3 in the same run (**BCM test: stages 0-5 (all but 4b)** runs 0-4, then 5). It boots `hello_dma` again and uses it the way the display driver does every day: a **scroll storm** of 240 `UPDATERECT`s in four kinds (the list area 320×220, two rows 320×56 moving down the list, a 260×8 progress bar, a 52×16 clock; the pattern scrolls up 2 rows per update), each with its host upload and its push timed; **10 full frames**; **3 sleep/wake cycles** in the stock order (`LCD_SLEEP`, `0xC`, power cut, then a fresh boot of the image and the power-on frame); then a **stop on purpose**: `BCM_CMD(0x7F)` with the mailbox word `0x164` = `STOP` makes the image take its real fault stop (the fault word), the host notices, cuts the power and reloads Apple's vmcs, and the fallback is timed. Pass: every rect counted, no DMA timeout, full frames under 40 ms, wakes under 1 s, the fault word seen. Writes `bcmpass5.txt`.
- **Partial updates by `UPDATERECT` in the boot path.** With our image running, `lcd-video.c` sends the union of what was written since the last kick as `UPDATERECT` (2-D DMA of just those pixels); a stalled update is re-kicked with the whole frame. Without `HAVE_BCMFW` the code is the stock code.
- **Boot path modes:** "build default" (removes the toggle; off in this build), "Apple vmcs (forced)" (the developer switch, any build), "CPU push (stage 2)", "DMA (stage 3 or 5)". **Strikes:** each boot that falls back at run time counts one (`bcmfw.strikes`); at 3 no boot swaps until a mode is chosen again; 30 clean minutes clear them. `--bcmfw-default` (not in this build) makes the DMA image the default when a stage 5 flag or `bcmfw/verified.txt` allows it.
- **Tests:** devtest 724 cases (stage 5 on panels 1 and 3 with the storm's last frame pixel-exact, stage 5 without stage 3, an image that hangs mid-storm), hello 1,011, dma 585, hostproto 100, forms, `video_test` end to end.

dev6:

- **LCD timing default: the 40 MHz words in every image** (test stages, the boot path, the video image): every panel field at or above Apple's nanoseconds with margin, 17.1 ms a frame. The test images and the boot path share images (the pass flag names the image's hash), so there is no separate test default; the sweep picks faster words per frame. 27 MHz (Apple's nanoseconds exactly on the measured bus, 14.2 ms) becomes the default if photos 5e–5g say so; 100 MHz (`-D BCMFW_LCD_BUS_KHZ=100000`) stays the fallback.
- **Bus sweep (stage 3):** 27, 20 and 15 MHz words (25 MHz would repeat 30's frame: the pacing word stops at 5 cycles from 30 MHz down to 16). First the bars and box at each (5a–5c), then a **fine pattern** at our own 40 MHz (5d, the reference) and at each sweep clock (5e–5g): one-pixel checkerboards and one-pixel lines (`device-session-1-photo5d.png`). 15 MHz is deliberately past the panel's minimum (11.4 ms): artefacts there are a result.
- **Stage 4:** MHRN expects the tie away from zero. Four informational checks: STBK (as before), **RNDT** and **RNDU** (ties: −16, −48, −80, −2,032, 16, 48, −15, −17 through `vmulhrs` and `vmulhr`), **MLSC** (`vmulms #16` into 8 bits: does the decoder's centre j clamp?). The nine decoder and display-path checks from p14-h264 (ADRG … CLRQ) run on silicon for the first time and are informational for this run. **Expected: "Checks: vector 24/24" and Stage 4 PASS**, which unlocks 4b; readlog prints the rounding and clamping verdicts and names any decoder form that fails.
- **Clock line:** now "N M loop iterations/s", not a MHz figure.
- **Tests:** every bench at 0 failures (devtest 649 cases, hello 1,011, dma 585, bench 503, hostproto 100, realframes 140, forms), the decoder's `vpu_test` and `disp_test` (884 cases), `video_test` end to end, and `sim_test` (all 14 clips) under the new rounding.

## What changed for session 6 (branch `p14-bcm-dev5`)

- **Vector stores (stage 4).** Every bench store uses the session-5 layout (`vpu_asm.st16_fields`); no staging. COP8 now stores row 8 that way. A new **informational** check, STBK, stores rows 16, 33 and x 32 (t5 block bits, seen only in Apple's code): it is reported but left out of the pass mask. MHRN expects the low 8 bits. The store-locating check stores five tagged sources with the layout, plus the old `vst16 d08` as a control (expected: row 0 at +256). **Expected: "Checks: vector 24/24" and Stage 4 PASS**, which writes the stage 4 pass flag and unlocks 4b.
- **DMA verdict.** `run` lines now say "counted up: all out"; readlog re-reads older logs.
- **Bus-timing sweep (stage 3).** After photo 5, three more DMA frames with Apple's nanoseconds recomputed for 50, 40 and 30 MHz, each held for a photo (5a, 5b, 5c), then our own words again. Expected about 23, 21 and 19 ms a frame. 50 and 40 MHz stay at or above the panel's minimums on a 39 MHz bus; 30 MHz goes about a quarter under, so artefacts there are possible and harmless (pixels only). A DMA timeout ends the sweep without failing the stage.
- **Spin alignment probe (every image).** spin()'s loop timed at 16 offsets of a 64-byte block; two `spin loop ticks` lines per image.
- **Tests:** every bench at 0 failures (hello 1,011 cases, dma 585, devtest 629, bench 466, hostproto 100, realframes 140, forms, vc2sim self-test 22,163, vc2cc 1,787, vc2dis `--roundtrip` 0 fails).

## What changed for session 5 (branch `p14-bcm-dev4`)

- **DMA completion (stage 3).** Apple's LCD push waits for DMA status **bit 8** (its callback mask `0x100`, @145e2); sub-channel k's chain end is bit 13 + k (@804c). Session 4's `int 00002300` was bits 8, 9 and 13: the frame had finished while we polled bit 0. `hello_dma` now clears the whole status word before each push and waits for bit 8 (`hardware.md`, "DMA engine").
- **Did DMA pixels reach the panel?** Two answers. (1) Log: after each DMA push a line `DMA frame 1: run 76800: XXXXX 1 poll in, 00000 at end, ctrl 329/329: <verdict>` gives the LCD interface's run-length register read back. "counted down: pixels out" settles it without a photo; "latch: see photo" means the register doesn't count. (2) Photo: after a timeout, the CPU re-send now paints the **top 8 rows black**. Photo 4 (DMA) has the yellow top border and the red/green corner squares; photo 6 (the forced timeout) has a black band across the top instead.
- **LCD bus timing (stage 2 and 3).** The images now use timing words recomputed from Apple's nanosecond tables for 100 MHz (Apple's are for 150 MHz; `hardware.md`, "Bus timing at the host-start clock"); every field still lasts at least Apple's nanoseconds. Stage 2 adds two frames after photo 3: one with Apple's words, one with ours, both logged with their times. The picture doesn't change. **Projection, unmeasured:** a full frame from 49 ms to about 34 ms.
- **Store-locating check (stage 4).** Four `B4S` lines: where a `vst16` from rows 0, 8, 16 and 32 lands (`isa.md`, "The store-locating check").
- **Tests:** every bench at 0 failures (hello 999 cases, dma 585, devtest 585, bench 464, hostproto 100, realframes 140, forms, vc2sim self-test 22,163, vc2cc 1,787, vc2dis `--check` 483/483 and `--roundtrip` 0 fails).

## What changed for session 3 (branch `p14-bcm-dev3`)

- **Hello v2 and every other image** start the counter's clock if it doesn't run: Apple's `0x4C`/`0x54` writes, a check, then `0x58`, another check; no PLL, regulator or run-control write. Each image reports which write started it, the clock registers as it found them, and the counter ticks over a 3,000-instruction spin (so the core clock) at `0x188`–`0x194`. If the counter still doesn't count, delays are spins of **50 iterations per µs** (a full µs up to 150 MHz, about 5.5 times too long at 27 MHz: panel 3's power-on then takes about 1.5 s, inside the 3 s limit).
- **A step word in every image** (`0x1C0`): clock writes, counter check, panel ID, each power-on step, each script record, every 16 rows of the push, the DMA wait, command received, done. A stall on the panel bus is named to the record.
- **Bounded waits**: a counter delay that runs out on its spin bound is recorded at `0x180` (and delays fall back to spins); a DMA timeout too.
- **The bench's vector section** has finer checks (see "Reading the log").
- **The log**: after `info …`, two `clock` lines; after every `fault words` line, two `image words` lines (the step named, the bounded-wait and clock words). `readlog.py` decodes them all.

## What changed since the first run

- **The toolchain is fixed.** `vc2dis`, `vc2cc` and `vc2sim` decode `0x7000–0x77ff` as `(--rs)`; nothing emits a 16-bit `cmp`/`max #imm5`. `bcmfw/tests/forms_test.py` rejects any instruction kind that Apple's static code doesn't contain and the device hasn't verified, in every embedded image. All seven images are rebuilt.
- **Probe v2** (`bringup.md`, "Probe v2"). Each vector has its own stub and a handler that records a fault and resumes at the next check. Nine required checks (`PASS = ALL = 1ff`); four informational ones (9–12) that measure the exception model and pre-decrement loads and never fail the stage. Checks 9 (session 1's `0x7050`, replayed) and 12 (an unaligned `ld`, then four adds) are expected to fault. Every outcome goes into a results block at `0xCB000`.
- **Stage 1 reads the block.** Pass now means: ready, step `50520003`, `PASS = ALL = 1ff`, and no unexpected fault (record U). The debug summary shows "Self-check: 1ff/1ff  faulted 1200". A fault in any probe or bench image also logs the block after the `fault words` line.
- **New log lines** (all hex but the check number):
  - `P2 req 1ff/1ff info 0c00/1e00 flt 1200 clo AAAAAAAA BBBBBBBB ticks N`: required and informational pass bits over their masks, the fault bits, the counter before and after the 1000-spin, and ticks;
  - `P2C i status pc sp sr detail detail2`, then `P2R i r0 r1 r2 r3 r4 r5`: one pair per informational check, and per required check that didn't pass. status is `c5` `ii` `vv` `ss`: check index, vector, state (1 pass, 2 fail, 3 fault);
  - `P2U …` and `P2UR …`: the same for record U (a fault outside any check), only when set;
  - `P2 none: block …`: no valid block (for example, an image that faulted before it set one up).
- **How to read them:** `bcmfw/bench/readlog.py bcmtest.log` prints the checks as a table (name, state, vector, pc, pc − detail for 9 and 12, r5 for 9, how many of 12's adds ran) and one verdict line for each open question: the **misaligned-access vector**, the **saved-pc offset** (bytes, or instructions if 9 and 12 disagree in bytes) and the **pre-decrement semantics** (was r5 decremented before the fault; do aligned `ld (--rs)` and `ldb`/`stb (--rs)` behave as modelled). Attach its output to #60.

## What can and can't happen

**Our image cannot change anything that survives a power cut.**
- It lives in the BCM's internal SRAM, which loses its contents when the power goes.
- It touches the panel interface, the BCM's GPIO pins (panel reset and the panel-ID inputs), the interrupt controller and the timer. From session 3 on, every image may also write two clock-block registers if the 1 MHz counter doesn't run (`0x10001C4C`/`54`, then `58`, Apple's own start-up values; volatile like everything else on the chip). None touches a PLL, the regulator, the run control, SDRAM or the cache. The stage 4 `bench` image touches even less: no panel, no GPIO output, no DMA, only the interrupt controller, the timer and one GPIO level read. **Only the optional stage 4b image (`bench_pll`) touches clocks, the run control and the core regulator** (see stage 4b). Only the stage 3 image (`hello_dma`) uses the DMA engine: channel 1, reading its own framebuffer in internal SRAM and writing the LCD FIFO. The bench checks all of this on every run, page by page for DMA.
- It has no way to write the iPod's NOR flash (where Apple's firmware and vmcs live) or the disk. The BCM is a slave on the PP's bus.
- The files the test writes are all in `/.rockbox/`: the log `bcmtest.log`, and the pass flags `bcmpass2.txt`, `bcmpass3.txt`, `bcmpass4.txt` and `bcmpass5.txt` (one line each: stage, image name, size, checksum). The boot path adds its toggle `bcmfw.cfg`, its attempt marker `bcmfw.try` and its strike count `bcmfw.strikes`.
- Stage 5's stop on purpose (dev7) is the image's own fault stop, armed by two separate words (a mailbox word and a command no stock driver sends); nothing else can trigger it.

**What could go wrong on the hardware side:**
- The panel power-on and power-off sequences are ours. In the simulator they match Apple's own handler event for event, for all four panel IDs (six variants), with every delay at least as long as Apple's.
- For panel IDs 1 and 3 there are two variants ("model 1" and "model 2"), and we don't know which one the 5.5G uses. They differ only in gamma values and register order. The worst case is wrong colours or contrast, not damage.
- The worst realistic outcome of a wrong sequence is a white, black or garbled screen for a few seconds. The test then cuts the power and Apple's vmcs runs its own sequence again.

## Before you start

1. Claude installs the test build with the deploy-ipod skill. You ask for it, and it backs up `.rockbox/` first. Keep the backup path it reports.
2. Make sure the battery is above 50%, and **unplug USB**: with USB connected the iPod is in disk mode and the menu can't be reached.
3. Have a phone camera ready, in a room that isn't too bright. Photograph the screen straight on.
4. Optional, so the screen stays lit between photos: set the backlight timer to the longest value.

## Steps

### Getting to the debug menu

1. Go to **Settings → About**.
2. Hold **SELECT (centre) + Previous (|◀◀)** for 3 seconds. The Developer menu opens.
3. Choose **Debug**. The first entries are:
   - **BCM test: stage 0 (read)**
   - **BCM test: stages 0-1 (probe)**
   - **BCM test: stages 0-2 (picture)**
   - **BCM test: stages 0-3 (DMA)**
   - **BCM test: stages 0-4 (timing)**
   - **BCM test: stages 0, 1, 4 (timing only)**: for re-running the bench without the photo holds
   - **BCM test: stage 4b (150 MHz, optional)**
   - **BCM test: stages 0-5 (all but 4b)**: 0-4, then stage 5 (the everyday path, dev7)
   - **BCM test: stage 4c (150 MHz screen, optional)** (dev9): 0, 1, then 4c; needs a 4b pass flag
   - **BCM test: stage 5b (drift A/B, optional)** (dev9): 0, then 5b; needs a stage 5 pass flag
   - **BCM boot path: status / build default / Apple vmcs (forced) / CPU push (stage 2) / DMA (stage 3 or 5) / test fallback** (see "The boot path" below)

### Stage order and what each needs

**Session 5: 0, then 0-1, then 0-2, then 0-3, then 0-4. Not 4b** (still the owner's call, and not needed for this session's questions). Sessions 1 to 4 used the same order. Each entry runs its stages in order, and a stage runs only if the stage it needs passed **earlier in the same run**:

```
0 (Apple's vmcs, read-only) --> 1 (probe) --> 2 (picture) --> 3 (DMA)
                                       \
                                        +--> 4 (timing bench; no panel I/O)
3 (DMA) --> 5 (the everyday path, dev7)

4b (150 MHz switch, optional): stage 0 in the same run, and a stage 4
   pass flag (bcmpass4.txt) that names this firmware's bench image
4c (150 MHz with the panel, dev9): stage 1 in the same run, and a 4b
   pass flag (bcmpass4b.txt) naming this firmware's 150 MHz bench (or
   4b passing earlier in the same run)
5b (drift A/B, dev9): a stage 5 pass flag (bcmpass5.txt) naming this
   firmware's hello_dma (or stage 5 passing earlier in the same run)
```

So stage 4 does **not** need stages 2 or 3: it never drives the panel. If stage 2 or 3 fails in a 0-4 run, stage 4 still runs. If stage 1 fails, everything after it is skipped, and the log says `stage N skipped: needs stage M to pass first`. Re-running earlier stages costs only a few seconds.

The only saved state is the pass flags: a run that passes stage 2 writes `bcmpass2.txt`, stage 3 `bcmpass3.txt`, stage 4 `bcmpass4.txt`, stage 5 `bcmpass5.txt`, and since dev9 stage 4b `bcmpass4b.txt` ("4b bench_pll …") and stage 4c `bcmpass4c.txt` ("4c hello_dma_clk …"). A run that includes a stage and doesn't pass it removes that stage's flag (and a stage 2 failure also removes stage 3's). Stage 5b writes no flag (it measures).

When a run ends, you see a summary list. Press MENU to go back. The full log is in `/.rockbox/bcmtest.log`.

### Stage 0 (read): about 1 second

**What happens:** with Apple's vmcs still running, it reads the panel ID and redraws the screen once (an ordinary update), timing it.

**What you'll see:** nothing changes. A summary appears: "Passed through stage 0", "Panel ID: N".

**Photograph:** the summary screen.

**If it fails:** stop and send the log. Nothing has been uploaded.

### Stages 0-1 (probe): about 2 seconds

**What happens:**
1. Stage 0 runs.
2. Apple's vmcs is powered down in the normal order (the same as when the backlight goes off).
3. Our probe image is loaded. It reports:
   - that it started;
   - whether the core began at address 0 or 0x200;
   - that the 1 MHz counter runs;
   - the result of the self-check: nine required checks, and four informational ones (two of which are expected to fault and be resumed).

   It answers one command. It never talks to the panel.
4. It's powered down, and Apple's vmcs is reloaded by the normal wake path.

**What you'll see:** the screen goes dark for about a second, then the debug screen comes back, then the summary. The summary shows "Passed through stage 1", "Self-check: 1ff/1ff  faulted 1200", an info word starting `5042`, and "from0: 0" (or 1).

**Photograph:** the summary screen.

### Stages 0-2 (picture): about 45 seconds

**What happens:** stages 0 and 1 run. Then our `hello` image is loaded (or `hello_rb` if the probe saw the core start at 0) and drives the panel.

**What you'll see, in order:**

| Time | Screen | Do |
|---|---|---|
| ~3 s dark | the switch-over | wait |
| up to 15 s | **Photo 1: the test pattern** (below) | photograph it, then press SELECT to move on, or wait |
| up to 10 s | **Photo 2: the same, plus a magenta box** with a white outline, bottom right | photograph, SELECT or wait |
| ~2 s | dark: the panel is asleep | (optional photo: it should be evenly dark, with no flash) |
| up to 10 s | **Photo 3: the pattern and the box again**, after the panel wakes | photograph, SELECT or wait |
| < 1 s | the same picture redrawn twice (Apple's bus timing, then ours): no visible change | wait |
| ~1 s dark | the switch back to Apple's vmcs | wait |
| | the debug screen, then the summary | photograph the summary |

**The expected pattern** (photo 1: `device-session-1-photo1.png`; photo 2 and photo 3: `device-session-1-photo2.png`, both in this folder). The bench produced these images pixel for pixel from the same code:
- **A 1-pixel yellow border** on all four edges. A missing edge means the picture is shifted by a pixel.
- **Four 4×4 corner blocks** just inside the border: red at top left, green at top right, blue at bottom left, white at bottom right. They show the orientation.
- **Eight colour bars** across the top, from left to right: red, green, blue, white, cyan, magenta, yellow, grey. If blue comes first, red and blue are swapped (colour order).
- **Three ramps** in the middle: red, then green, then blue, each dark on the left and bright on the right. Smooth ramps mean the byte order is right. Stripes or jumps mean the bytes are swapped.
- **A white "F"** at the bottom left. If it's mirrored or upside down, the panel scan direction is different.

**Photograph:** each of the three pictures, plus the summary. Also note anything that looks wrong: tearing, flicker, a partial or garbled picture, a white flash at power steps.

### Stages 0-3 (DMA): about 140 seconds

**What happens:** stages 0 to 2 run as above. Stage 3 runs only if stage 2 passed in the same run. It loads `hello_dma` (or `hello_dma_rb`), which sends pixels to the panel with the BCM's DMA engine instead of the CPU, and then:
1. the test pattern, by DMA (the panel powers on first);
2. one more full frame, timed on the BCM's own 1 MHz counter;
3. a 100×50 rectangle at (10, 20), over the colour bars, by 2-D DMA;
4. the **bus-timing sweep** (dev6): the same frame three more times by DMA, with timing words for 27, 20 and 15 MHz; then a fine pattern four times, with our own 40 MHz words and then each sweep clock's. Each frame is timed and held for a photo; then the image's own words again;
5. a **forced timeout**: the test tells the image not to start the next transfer, so the image waits its full 100 ms, stops the channel and re-sends the frame by CPU. This frame has no box, so you only see the change if the CPU fallback drew it;
6. one more frame (DMA stays off after a timeout until the next upload, so this one goes by CPU too);
7. power-down and Apple's vmcs again.

**What you'll see after stage 2's pictures:**

| Time | Screen | Do |
|---|---|---|
| ~1 s dark | the switch-over | wait |
| up to 15 s | **Photo 4: the test pattern**, the same as photo 1, yellow top border and red/green top corners present | photograph, SELECT or wait |
| up to 10 s | **Photo 5: the pattern plus a magenta box at the top left**, over the first colour bars (`device-session-1-photo5.png`) | photograph, SELECT or wait |
| up to 8 s each | **Photos 5a, 5b, 5c: the same picture** at faster bus timing (27, 20, 15 MHz words). It should look exactly like photo 5. Look for wrong colours, a shifted or doubled line, or noise, most likely in 5c | photograph each, SELECT or wait |
| up to 8 s | **Photo 5d: the fine pattern** at our own 40 MHz words, the reference (`device-session-1-photo5d.png`): top left a one-pixel black/white checkerboard, top right a two-colour checkerboard, bottom left one-pixel vertical lines, bottom right one-pixel horizontal lines. Each quarter should look like an even texture | photograph close up, SELECT or wait |
| up to 8 s each | **Photos 5e, 5f, 5g: the fine pattern** at 27, 20 and 15 MHz. Compare each with 5d: speckles, coloured noise, streaks, or a quarter whose texture changes mean that clock is too fast. 5e (27 MHz) is the one that decides the default | photograph each close up, SELECT or wait |
| up to 10 s | **Photo 6: the pattern without any box, with a black band across the top 8 rows**: drawn by the CPU fallback | photograph, SELECT or wait |
| ~1 s dark | the switch back to Apple's vmcs | wait |
| | the summary | photograph it |

**What to look for** (the DMA checklist in `bringup.md`): photo 4 must match photo 1 exactly. Neighbouring pixels swapped in pairs means the halfword order in a FIFO word is reversed. Only the top eighth of the picture means the length unit is wrong. A box in photo 5 that is slanted or sheared means the 2-D stride is a pitch, not a skip. If photo 6 still shows the box, the CPU fallback did not draw. A black top band in photo 4 or 5 means that frame timed out and was re-sent by CPU.

The sweep's log lines read `bus sweep 27000 kHz 40000604/40000001: frame N us, BCM N us` (panel 3's words; 20 MHz `40000303/40000001`, 15 MHz `40000202/40000000`), and `bus sweep fine …` for the fine frames (the reference at 40 MHz `40000906/40000002`). Expected frame times: about 14.2, 14.2 and 11.4 ms, and 17.1 ms for the reference; `readlog.py` lists them. If one ends `DMA TIMED OUT`, the sweep stops there, that frame was redrawn by CPU (with the black band), and the forced-timeout step is skipped; the stage still passes.

**The summary** adds "DMA frame/rect" (host-timed), "BCM counter" (the push itself), "Forced" (about 100 ms plus a CPU frame), "DMA done 3, timeouts 1" and the channel registers. If the stage stops with "DMA completion bit 8 never set", the engine never reported a finished transfer: the image fell back to the CPU, so the picture was still drawn. The log line `DMA frame 1: … ch1 cs … int … sub0 …` then holds the register values the next build needs (checklist item 1).

### Stages 0-4 (timing): about 140 seconds plus up to 20 seconds dark

**What happens:** stages 0 to 3 run as above (stage 2 and 3 failures don't stop stage 4). Then Apple's vmcs is powered down in the normal order, and our `bench` image is loaded. It reports ready like the probe, then, without any panel or bus traffic:
1. runs **37 vector checks** (24 required; 13 informational: STBK, RNDT, RNDU, MLSC and the nine decoder forms on their first silicon run), **6 store-locating checks** and **9 scalar checks** of instruction forms the decoder uses and Apple's code never exercises, plus the probe's checks again (nine required);
2. **times 69 kernels** (instruction mixes: dependent and independent adds, loads, stores, branches, a CAVLC bit reader, vector ops at REP 1 to 16, vector loads and stores), each 3 times with 4,096 iterations, against its own 1 MHz counter;
3. writes a results block in its SRAM and says "done" in its step word.

Meanwhile the iPod polls the step word every 10 ms and logs every new step it sees. It sends the bench no commands until it says done. Then it reads the results block (about 600 words) into the log, powers the bench down in the normal order, and reloads Apple's vmcs.

**Limits:** the bench has 1 s to say ready, then **3 s without any new step** or **20 s in total**. Either limit, or a fault, cuts the power at once (no commands to a stuck chip) and reloads Apple's vmcs. Why those numbers: the bench is about 38 M cycles if every instruction and vector row costs one cycle, which is 0.25 s at 150 MHz or 1.4 s on the 27 MHz crystal. Its largest kernel is 1.7 M cycles (62 ms at 27 MHz), and the step word moves for every kernel. So 3 s is 48 times the slowest kernel, and 20 s is 14 times the whole bench at 27 MHz (3.5 times even if everything costs 4 cycles).

**What you'll see:** after stage 3's photos and the switch back, the screen goes **dark** (the bench has no panel I/O) for about 1 to 3 seconds, at most about 22 s, then the debug screen, then the summary. The stage 4 part of the summary is only counts and the clock:
- "Stage 4: PASS" (or FAIL);
- "Checks: vector 24/24, scalar 9/9". The count is of the 24 required checks; the informational ones are only in the log. **Session 7 expects 24/24 and "Stage 4: PASS"**, which writes the stage 4 pass flag (so 4b becomes available). If it shows fewer, the log's `B4V` records name the checks;
- "Probe checks: 9/9, kernels 69/69";
- "Clock: N MHz (est.)": the ALU1 estimate, which assumes one dependent add per cycle;
- "Pass flag: stage 4 written" (or cleared);
- on a FAIL, "Last step: 5052xxxx", the step word it stopped at.

**Photograph:** the summary screen. Everything else is in the log.

**Pass** means: the block's magic and "done" state, every vector, scalar and probe check passed, the counter ran, and every kernel has a time with 0 < fastest ≤ slowest. A FAIL still logs everything the bench wrote, so a failed check is a result, not a lost run.

### Stage 5 (the everyday path): about 25 seconds, after stages 0-4

**What happens:** after stage 4 (or straight after stage 3 if 4 isn't in the run), the iPod loads `hello_dma` again and drives it as ProkBox's display driver would:
1. the test pattern, and the power-on frame;
2. a **scroll storm**: 240 partial updates in turn: the list area (320×220), two rows (320×56) moving down the list, a progress bar (260×8) and a clock (52×16). Each upload carries the pattern moved up 2 rows, so the picture scrolls. Each is uploaded over the host port and pushed with `UPDATERECT`; both times are logged per kind;
3. the last storm frame stays up 2 s; then 10 full frames (the pattern again), held 5 s;
4. **3 sleep/wake cycles**: dark (the stock power-down order), then the image reloaded and the pattern back, each held 2 s;
5. **the stop**: the iPod tells the image to stop as if it had faulted. It must see the fault word, cut the power and bring Apple's vmcs back with the normal screen. The log times both.

**What you'll see:** the pattern, then about 3–5 s of the pattern scrolling in bands (the list area, a two-row band moving down, small strips at the bottom and top right), a 2 s pause, the pattern, three dark/pattern blinks, then the normal screen. **No photos**; report anything that looked wrong (a band in the wrong place, garbled pixels, a frozen screen).

**The summary** adds: "Stage 5: PASS", "Upload/full/list: N/N/N us" (the host upload of a whole frame, the slowest full push, the slowest list-area push), "Rects 240, wakes 3 (max N ms)", "Stop seen N us, fallback N ms" and "Pass flag: stage 5 written".

### Stage 4b (150 MHz, optional): about 25 seconds

**Not part of the first run. Running it is the owner's call during the session**, only after stage 4 has passed. The entry refuses to start otherwise ("Not available: no stage 4 pass flag", or "void: …" if the flag came from a different build).

**What happens:** stage 0, then the `bench_pll` image. It runs the same checks and timings as stage 4, then switches the BCM's core to 150 MHz by repeating Apple's own start-up sequence write for write (run control, ten block clocks off, PLLs B, C, D and A off, the core regulator to level 15, 10 ms, PLL A at 27 MHz × 600 / 108, 1 ms, enable, the core onto PLL A), and times all 69 kernels again. Comparing the two ALU1 times gives the start-up clock without assuming anything about the core: start-up clock = 150 MHz × (time at 150 MHz) / (time at start-up). Same limits (3 s without progress, 25 s in total), same power cut and restore.

**Confirm step:** the entry first shows what it does and the risk pointer. **Scroll down to the last line, "Run stage 4b now (SELECT here)", and press SELECT.** SELECT on any other line does nothing; MENU cancels with nothing done.

**The risks, in plain words** (from `bringup.md`, "The PLL variant"):
- **The chip could stop.** One of the writes changes a run-control register whose meaning we have only partly worked out. If it stops our code, the step word stays at the switch's first step, and after 3 s the iPod cuts the BCM's power. Nothing is lost.
- **The iPod could lose contact with the chip.** We don't know what the ten block clocks feed. If one of them drives the port the iPod talks through, reads fail, the bounded reads give up, and the power is cut. If one drives the chip's timer, the second table reads near zero and the log says the counter stopped (flag 8).
- **Higher core voltage, briefly.** Regulator level 15 is the highest in Apple's table. It's held for under a second, until the power cut, and it's the level Apple's own start-up sets at every boot.
- **A wrong clock.** If our reading of the PLL fields is wrong, the core could run well away from 150 MHz. Too fast means a hang or a fault, again ended by the power cut.
- **Nothing persists.** All of it is the BCM's own volatile state, reset by the power cut that ends the stage. Nothing touches the NOR flash, the disk or the panel.

**What you'll see:** about 25 s of dark screen at most (usually 1 to 3 s), then the summary: "Stage 4b: PASS", the checks, "Clock: N MHz (est.)" (the start-up estimate from the first table), and "**Clock at start: N MHz (measured)**". **Photograph** it.

**Pass** additionally means: the image says it is the 150 MHz build, the switch ran to its end, no wait in it hit its spin limit, and the second table is complete. The readback words (regulator, PLL A, clock select) are logged, not judged: the bench checklist compares them with `0xA5A5000F` and `0xA5A50101`.

### Stage 4c (150 MHz with the screen on, optional): about 40 seconds

**Only after stage 4b has passed** (its pass flag, new in dev9, unlocks the entry; "Not available: no stage 4b pass flag" otherwise). The same confirm screen as 4b: scroll to the last line, "Run stage 4c now (SELECT here)", press SELECT.

**What happens:** stage 0, the probe (where the core starts), then the clock-command image `hello_dma_clk` (the everyday DMA image plus one command, `CLOCK`). The test pattern on the 27 MHz crystal; then the chip switches to 150 MHz exactly as in 4b; a full frame (**photo 4c-1**) and the magenta box by 2-D DMA (**photo 4c-2**) at 150 MHz, with Apple's own 150 MHz bus timing; back to 27 MHz (our sequence: the core onto the crystal, PLL A off, the regulator back); the pattern again (**photo 4c-3**); then up once more and the sleep command, which must bring the chip back to 27 MHz by itself. The usual power cut and Apple's vmcs at the end.

**The risks** are 4b's (the PLL, the regulator at level 15, briefly), with two additions:
- **The panel at 150 MHz.** If the LCD bus follows the core, Apple's words are the ones Apple's firmware uses on this panel every day; if it doesn't, they're only slower. A wrong guess shows as a bad picture in 4c-1/4c-2, nothing more.
- **Our switch down is new.** If it leaves something wrong, 4c-3 or the last check shows it, and the power cut at the end resets everything the switch touched. Nothing persists.

**Pass** means: every switch ran to its end with no spin-limited wait, the state words say where the core is, every DMA transfer completed, the frame after the round trip took within 10% of the first, and the sleep command left the core on the crystal. The pass flag `bcmpass4c.txt` names the image.

### Stage 5b (the drift A/B, optional): about 14 minutes

**Only with a stage 5 pass flag** (or stage 5 in the same run). A measurement for the 2-D offload decision (`offload.md`), not a pass/fail test. Before it: music stopped; **Developer › power log on, 5 s** (its CSV is `/.rockbox/power.csv`); optionally a USB-C meter on a plain charger with the battery full.

**What happens:**
- First the test pattern, then 20 s to settle.
- Then six 120 s phases: A1, B1, A2, B2, A3, B3. Each phase draws the main menu's panel drift, as the shell runs it: an 8-step cross-fade into the next generated "cover", then pans that move a pixel every 150 ms, each pixel in two half steps (8b), a new cover every 7 s.
  - **A:** the PP draws each frame, uploads the 160×240 panel and pushes it.
  - **B:** the chip does the same work: the PP uploads each cover once and then sends one small command a frame.
  - **Stub B:** in builds before p14-bcm-5b, or if the PP can't get the cover memory, B only pushes what the chip already holds, and the log says "B is a stub".
- Each phase boundary is a mark in the power log (`s5_A1` … `s5_end`).
- The log gives, per phase: frames, late frames, the PP's busy time, the chip's, and the PP's wait. It also gives the half steps: their longest compose, and whether A's guard turned them off. For B, whether its op would have tripped that guard.

**What you'll see:** the right half of the screen drifting through fading, slowly panning pictures for the whole 14 minutes. With the real B (p14-bcm-5b), the A and B phases look exactly the same; that's the point. Then the normal screen comes back. Don't touch the iPod meanwhile.

### Stage 5i (idle sleep, optional): about 3 minutes

**Only with a stage 5 pass flag naming `hello_dma`** (or stage 5 in the same run). Whether our image can sleep its core between commands, the lever for the unlit clock face (`pipeline.md`, "Idle sleep"). **What happens:**
- `hello_dma` (the busy-poll image) shows the pattern and answers 60 no-op commands and 60 small updates (the clock's corner).
- Then `hello_dma_idle` (the same image, asleep between commands) does the same.
- Then the light goes off for a minute with the screen on, one small update every 10 s.
- Then two sleep/wake cycles, and the normal screen.

**What you'll see:** the colour bars twice (the clock corner doesn't visibly change), a dark minute, two dark blinks, the summary.

**In the log:**
- `5i hello_dma: …` and `5i hello_dma_idle: …`: avg and max per kind.
- `5i counters …` and `5i doorbell: …`: never seen, every command, or some.
- `5i hold: …` (two lines: ticks against the time held, doorbells, sleeps, bad; the updates and how many were late).
- `5i wake n: …`, then `stage 5i PASS` and the flag `bcmpass5i.txt`.

**Pass** means:
- the idle image's updates are no more than a tick (1 ms) plus 2 ms slower than the busy image's;
- the tick count matches the minute held within 10%;
- no sleep came back with a register changed;
- no update in the dark minute was late.

**If it fails,** nothing changes for everyday use: the boot path never takes this image by itself.

### Stage 5s (the sleep spikes, optional): about 3 minutes

**Only with a stage 5 pass flag naming `hello_dma`**; the image (`hello_dma_sby`: the idle image with panel standby) sleeps its core between commands only if a stage 5i flag says the tick wakes it.

**What happens:**
1. The cold start: the pattern; look 1.
2. The meter phases, 20 s each. Light standby (display off, power circuits on, the reset line high) three times, the first time with a look after the wake. Then deep standby (the whole off script, reset high) three times, the same way. Panel 3 has both depths; on panels 1 and 2 the light one is refused and logged.
3. Then `LCD_SLEEP` with the chip kept up; look 4 after the full power-on. Then the chip's power cut.
4. Then the boot upload: a 192 KB block (the image, then a filler) by CPU, by engine 2, by CPU, by engine 2. Each time, every word is read back before the start, then the image starts and shows the pattern.

**In the log:**
- `5s cold: …`
- `5s meter: …` at each phase.
- `5s light: dark … ms, framebuffer N words changed; wake … us (panel … us)`, and the same for `5s deep: …`.
- `5s reset: …`, then the two `5s wakes: …` summary lines.
- `5s idle: …`, with chip sleep on only.
- `5s upload CPU: …` and `5s upload engine 2: … us, N words read back wrong (first at …), 3 pieces`, then `5s upload: …` twice.
- Then `stage 5s PASS`.

**Pass** means:
- every command answered, every wake came out of standby, not a cold power-on, and the framebuffer was unchanged after every dark spell;
- the deep depth was accepted;
- no word was read back wrong after either upload, and both uploads started the image;
- with chip sleep on, no sleep came back with a register changed.

The pictures are the owner's to judge: the stage cannot see the panel. No pass flag is written; nothing uses the results by itself.

### Stage 3d (the 3D render test, optional): about 1 minute, or 2 with 150 MHz

**Needs a stage 5 pass flag naming `hello_dma`.** The 150 MHz runs also need stage 4b and 4c flags naming this firmware's `bench_pll` and `hello_dma_clk`; the confirm screen says whether they will run. The image is `hello_3d` (30,200 B, fnv `5683b011`): our PS1-style renderer ([`render3d.md`](render3d.md)).

**What happens:**
- Five 3D scenes, 10 s each, held to 30 fps:
  - a courtyard orbit;
  - a village from above;
  - a valley circle;
  - a flight down a ruined valley;
  - a walk through the village at eye height.
- First the chip times the panel's TE line for half a second (no picture change). This measures the screen's real refresh rate.
- Then the five scenes on the crystal (27 MHz), then, with the flags, all five again at 150 MHz.
- If the TE line was found, the walk runs once more, each frame started on the panel's refresh (tear-free if our reading is right).
- Then the normal screen and the summary.

**What you'll see:**
- At 27 MHz the motion is slow and steppy. The renderer manages about 7–22 fps there (simulator estimate).
- At 150 MHz it should be smooth.
- Report anything that isn't a clean picture: torn or striped frames, wrong colours, holes in the ground, flicker, or a scene that freezes.
- **Photograph the summary:** fps per scene at each clock, "Frames unlike the simulator", "Panel refresh (TE)" and "Walk on TE".
- **Tearing:** during fast turns, a horizontal break line through the picture is tearing. Say whether you see one in the timer runs, and whether it's gone in the last (TE-synced) walk.

**In the log:** per run, four lines prefixed `3d <scene> @<clock>:`:
- fps and late frames;
- render time and its setup/raster split;
- triangles and pixels per frame;
- the panel wait and idle time.

Then one checksum line (`3d <scene>: crc …`) with each check frame's checksum and the simulator's. The power log marks `s3d_<scene>s` / `s3d_<scene>f`, then `s3d_end`.

**Pass** means every run finished, and the two checksummed frames of every run match the simulator's. A mismatch is a silicon finding (the chip computed something vc2sim doesn't model): keep the log. No pass flag is written.

**Risks:** the same as stage 4c for the 150 MHz part. The image never touches SDRAM or the regulator beyond 4c's switch, and every wait is bounded. At the end the chip's power is cut and Apple's display driver returns.

### Stage 6 (clips through the player, optional): about 40 seconds

**Only in `--bcmfw-video` builds, with a stage 5 pass flag**, and with `.rockbox/bcmvideo.bin` and `.rockbox/bcmtest/clip.mp4` present (the zip puts both there). Music stops first. **What happens:** the real video player (`apps/video/vplayer.c`) takes the BCM, boots our video image (at 150 MHz if the 4b and 4c flags allow it), and Rockbox's normal audio path plays the clip while the picture follows it. When it ends, the normal screen comes back and the summary shows. **What you'll see:** the colour-bar test pattern with a white counter counting 0 to 7, for 8 seconds, with a steady tone; then the summary. **Photograph the screen when the counter shows 5** (photo 6-1), then the summary.

**Then clip 2 (s9-int):** 13 s of real footage (the *Sintel* trailer excerpt) at 24 fps, if `.rockbox/bcmtest/clip2.mp4` is present: watch for stutter, jerky pans, frozen pictures, and sound drifting from the picture.

**In the log:** `=== BCM test, stage 6 (play a clip) ===`, then per clip `stage 6 clip n: …`, four `stage 6 …` summary lines (pictures, A/V, pacing, the chip's time and clock) and one `V6 <pts ms> <drift us> <chip us> <MHz> <shown>` line per picture; then `=== end: stage 6 … ===`. dev10 adds per clip the `stage 6 SDRAM: …` lines (the `OPEN` reply's SDRAM words, decoded; `hostproto.md`, "OPEN's SDRAM words") and `stage 6 readback (6s): …`. The player's own lines (`video: …`, `video: sdram: …` in hex) are in `ipodui.log`.

## Reading the log

`/.rockbox/bcmtest.log` grows by one block per run. Each line starts with the milliseconds since that run began. Every line is written to disk before the step it names, so if the iPod hangs, the last line says where.

Here's an example from the simulator, stages 0-2:

```
     0 === BCM test, stages 0-2 ===
     0 stage 0: Apple vmcs running, read-only
     0 gpio lev1 00000000 -> panel ID 0; 0x10001400 = 00000000; status 50420001
     2 Apple LCD_UPDATE: 2100 us
     2 stage 0 PASS
       ...
   655 boot hello: 4004 bytes, fnv 8784ae26
   705 status 50420001 after 28 us
   705 info 50420210: panel 0, counter running
   705 kick LCD_UPDATE 1 (panel power-on + frame)
   841 frame 1 done in 136188 us: PHOTO 1 (pattern)
       ...
 38410 === end: passed through stage 2 ===
```

| Line | Meaning |
|---|---|
| `gpio lev1 … -> panel ID N` | Checklist item 1: which panel is fitted |
| `status 50420001 after N us` | Our image started (checklist item 3) |
| `fault words: … step …`, then `FAIL ready (timeout)` | It never reported ready. `step` says how far the probe got: 0 means it never reached `_start`, `50520001` means it did, `5052nn02` (nn in hex: 00–0c) means it stopped inside self-check nn |
| `info 5042xxxx … counter running/STOPPED` | Checklist item 4: if STOPPED, delays fall back to a spin loop, which is slower and therefore safer |
| `probe: self-check X of 1ff, faulted F` | Each missing bit of X is a required form that behaves differently on the chip: bit 0 `abs`, 1 `not`, 2 `mulhd.ss`, 3 three-operand `eor`, 4 `addscale6`, 5 post-increment `ldh`, 6 pc-relative `ld`/`st`, 7 16-bit `bhi`, 8 32-bit `bhi`. F: bit n = check n faulted and was resumed; `1200` (checks 9 and 12) is the expected result |
| `P2 …`, `P2C …`, `P2R …`, `P2U …` | Probe v2's results block (see "What changed since the first run"). Read them with `readlog.py` |
| `clock 00xx0Cyy: counter started by 0x4C/0x54` (or `…by 0x58`, `ran at start`, `still STOPPED after 0x4C and 0x58`) | Which of Apple's writes started the 1 MHz counter (hello v2 and every later image) |
| `clock: N ticks over spin(1000) (~M MHz); found 4c … 54 … 58 …` | The counter over 3,000 instructions, so the core clock if the counter is right; the three clock registers before any write |
| `image words: step 5053xxxx (<where>)` | Where a hello v2 image was when the host gave up or it faulted: `power-on step n`, `panel init/on/off script record n`, `CPU push, row n`, `DMA wait`, `command n received`, `command done` |
| `image words: limit LLLLLLLL val VVVVVVVV clk CCCCCCCC` | A bounded wait that ran out (`01…`: the counter stopped mid-delay, not fatal; `02…`: DMA completion never came), the last value read, the clock record |
| `frame 1 done in N us` | Checklist item 7: the first update, including panel power-on (about 136 ms for panel 0 in the simulator, up to 451 ms for panel 2) |
| `frame 2 done in N us` | One full frame with the panel on. 211,291 instructions, so N tells us the real core speed |
| `fault words: pc … sp …` | Our image hit an exception. Look the pc up in `build/bcmfw/devtest-<image>/bcmfw.map` |
| `power-down: cutting power (image not answering)` | The watchdog path: the image hung or faulted, so the power was cut |
| `restore: done` | Apple's vmcs is back |
| `DMA frame 1: BCM N us (counter running), done 1, timeouts 0, polls P; ch1 cs … int … sub0 …` | Stage 3: N is the push on the BCM's own counter (DMA checklist item 6); `done` must count up, `timeouts` stay 0 (item 1). The three register words are read at the end of the push |
| `forced timeout: BCM N us …, done 3, timeouts 1` | The forced timeout: N about 100,000 or more, then the CPU re-send |
| `pass flag: 2 hello 00000fa4 …` / `pass flag: stage 3 cleared` | A stage's pass flag written or removed |
| `boot path: …` | The boot path at a boot, or a later fallback (see below) |
| `stage N skipped: needs stage M to pass first` | The dependency graph at work: N was asked for, M didn't pass in this run |
| `bench step 5052xxxx: <what>` | Stage 4/4b progress: each new step word the iPod saw (polled every 10 ms, so fast steps are skipped). `50521kkk` vector check k, `50522000` scalar checks, `50523kkk` timing kernel k, `50524nnn` the 150 MHz switch at step n (1 run control, 2 block clocks, 3 PLLs off, 4 regulator, 5 PLL A, 6 core onto PLL A, 7 readback), `50525kkk` kernel k at 150 MHz, `50520004` done. k is in hex |
| `bench stopped at step … (<what>): no progress for 3 s` / `over the time limit` | The stage 4 watchdog: the step names the kernel or check it hung in, then `fault words`, then the power cut |
| `bench done in N ms` | Ready to done, host-timed. At 4,096 iterations: about 250 ms at 150 MHz, 1.4 s at 27 MHz, if one cycle per instruction and row |
| `B4H n w0 … w7` | The results header, words n to n+7, in hex, exactly as the bench wrote it (layout: `bcmfw/bench/bench.h`): magic `50424e31`, state 2, kernels `45` (69), iterations `1000` (4,096), runs 3, flags, vector pass/all, scalar pass/all, probe pass/all, first mismatch, got, want, the five 4b readbacks, the table offsets, entry size, vector check count |
| `bench checks: vector 3ffff/3ffff (18/18), scalar 1ff/1ff, probe 1ff/1ff` | Bit k = check k in `bench_tab.c` order. A missing vector bit: the form under test behaves differently on the chip. Map it to the decoder's kernels before trusting their output on the device |
| `bench first mismatch CCCCWWWW got … want …` | The first failing vector check (CCCC) and word (WWWW), what it read and what the model expected; `ffffffff` if none |
| `B4K t k TAG insns ops rows min max per_iter` | One timing entry: table t (1, or 2 at 150 MHz), kernel k (decimal), its tag, instructions in the timed loop, measured instructions, their vector rows, the fastest and slowest of 3 runs in µs, instructions per iteration |
| `B4R clock N kHz (ALU1 …)` | The clock estimate: ALU1's instructions × 1000 / fastest µs |
| `B4R start-up clock N kHz (150 MHz x t2 / t1 us)` | Stage 4b only: the start-up clock measured against 150 MHz |
| `150 MHz switch: flags …; regulator … -> …`, then `150 MHz switch: PLL A … …, clock select …` | Stage 4b only: flags should be `7` (counter, PLL build, switch done; 8 = a wait hit its spin limit) |
| `pass flag: 4 bench 00004fd0 …` / `pass flag: stage 4 cleared` | Stage 4's flag written or removed |

**Reading stage 4 properly:** `bcmfw/bench/readlog.py bcmtest.log` turns the `B4H`/`B4K` lines back into tables: the checks, the first mismatch named by its check (e.g. `VMAX`), the clock estimate, and per kernel the cycles per measured instruction, cycles per vector row and slowest/fastest. For a run that stopped, it names the last step's kernel or check by tag. `bringup.md` ("How to read the numbers" and "Device checklist: session 2") says what each figure means for the decoder.

To read the log: plug in USB, open the iPod in Finder, and look in `.rockbox` (press Cmd+Shift+. to show hidden folders). Or ask Claude to copy the file off during the deploy-ipod backup step.

## The boot path (after a pass)

This build can also make **our** image the one the iPod uses all the time, in place of Apple's vmcs: at every boot, and at every backlight wake after the screen sleeps. It is off by default and does nothing until you turn it on.

**It switches only when all of these hold:**
- the toggle (below) is set to CPU push or DMA (in a `--bcmfw-default` build, no toggle file means DMA; this build's default is off);
- the matching pass flag exists: `bcmpass2.txt` for CPU push (a run of stages 0-2 or 0-3 passed stage 2), `bcmpass5.txt` or `bcmpass3.txt` for DMA (a run passed stage 5 or stage 3); the default (no toggle) takes only `bcmpass5.txt`, or an image `bcmfw/verified.txt` lists;
- fewer than 3 strikes in `bcmfw.strikes` (run-time fallbacks, one per boot);
- the flag names an image built into this firmware, with the same size and checksum. A flag from an older or newer test build is void, so a firmware update needs the stage run again;
- no attempt marker (`bcmfw.try`) is left over from a boot that didn't finish.

**Turning it on:**
1. Run **BCM test: stages 0-2** (for CPU push) or **stages 0-3** (for DMA) until it passes, and check the photos. The pass flag says only that the image answered correctly; the photos are what say the picture was right.
2. In the same Debug list, choose **BCM boot path: CPU push (stage 2)** or **BCM boot path: DMA (stage 3 or 5)**. A status screen shows the toggle, which image is running now, whether each stage's flag is valid, and the marker. "Takes effect at the next boot."
3. Restart (hold MENU + SELECT). After the boot glyph the screen goes dark for about half a second, then the ProkBox main menu appears. That frame came from our image.
4. Check **BCM boot path: status**: "Running: hello" (or `hello_dma`, `hello_rb`, `hello_dma_rb`), and "This boot: active: …". Then let the backlight time out and press a button: the wake reloads our image, and the screen must come back correctly.

**At each boot the path:**
1. writes `bcmfw.try`, so it is on disk before anything touches the chip;
2. powers Apple's vmcs down in the normal order, starts our image, sends the current screen, and waits for the frame to be reported done (every wait has a limit: 1 s for ready, 3 s for the power-on frame);
3. only then removes `bcmfw.try`, and from then on every wake reloads our image.

**What a fallback looks like.** In every case you end up on Apple's vmcs, with the normal screen, for the rest of that boot, and the log says why (`boot path: fell back to Apple's vmcs: …`):
- **At boot** (our image never got ready, faulted, or never finished the first frame): a dark screen for 1 to 4 seconds, then the normal screen. `bcmfw.try` is kept, so later boots don't try again until you choose a mode in the debug menu, which clears the marker.
- **At a backlight wake** (our image doesn't come up): the wake takes about 1 second longer, then the screen comes back on Apple's vmcs.
- **While in use** (the fault word, or one screen update busy for 2 s): the screen freezes for up to 2 s, then the next screen change shows a short dark blink and the normal screen again.
- **If the iPod hung or restarted during the switch:** the next boot finds `bcmfw.try` and doesn't switch (`boot path: skipped: the last attempt did not finish`).
- **BCM boot path: test fallback** exercises the real in-use fallback on purpose: it marks our running image as stopped, and the status screen's own draw takes the fallback (dark blink, then "Running: Apple vmcs" when you reopen the status).

Fallbacks after boot are kept in memory and written to `bcmtest.log` when the disk next goes to sleep (or at power-off), so they never spin the disk up by themselves. The status screen's "This boot:" line shows the latest one at once.

**Partial updates (dev7).** While our image runs, a screen change that covers less than the whole screen goes to the panel as `UPDATERECT`: only those pixels cross the LCD bus. Menus and Now Playing's progress bar use it all the time.

**Strikes (dev7).** Each boot that falls back while in use writes one strike to `bcmfw.strikes` (with the idle flush, never an extra spin-up). At 3, boots stop swapping (`boot path: skipped: too many run-time fallbacks`) until a mode is chosen again in the Debug list, which clears them; a boot that runs our image for 30 minutes without a fallback also clears them.

**Turning it off:**
- **BCM boot path: build default** in the Debug list, then restart. The toggle file is removed, so the build's default applies: off in this build, and the next boot leaves Apple's vmcs alone.
- **BCM boot path: Apple vmcs (forced)**: Apple's vmcs in any build, including a `--bcmfw-default` one, until another mode is chosen.
- **If the screen is unusable with our image** (garbled but "confirmed"): hold **Next (▶▶|)** while the iPod boots. The Developer list opens before the shell starts, so before the boot path runs, with Apple's vmcs still driving the screen. Go to Debug → **BCM boot path: Apple vmcs (forced)** (or **build default** in this build).
- **From a computer:** delete `.rockbox/bcmfw.cfg` over USB.
- **Undo everything:** ask Claude to restore the backup with the deploy-ipod skill (below). That brings back the previous firmware files.

## Aborting

**At any moment:** hold **MENU + SELECT** for about 6 to 8 seconds, until the screen goes off and the iPod restarts. It's safe because:
- nothing the test does is persistent: the BCM's SRAM is volatile, and our code never writes the NOR flash, the settings or the database;
- the ProkBox bootloader has no BCM code at all (`bootloader/ipod.c`). At every boot it relies on Apple's flash boot code having loaded Apple's vmcs, so a restart goes through Apple's own BCM start-up.

**If a restart ever shows a dark screen:** wait for the backlight timeout and then press a button, or hold Play/Pause to switch the iPod off and switch it on again. Both run the stock power-down, then reload Apple's vmcs from NOR.

**The test also aborts by itself.** Every wait has a limit:
- 1 s for "ready";
- 3 s for a power-on frame;
- 1 s for a frame;
- 2 s for sleep;
- 3 s without a new bench step, 20 s for the whole bench (25 s for 4b);
- 20 ms for any single bus handshake.

On a timeout, a fault or a dead bus it cuts the BCM's power, then reloads Apple's vmcs through the normal wake path.

## Uninstall (back to normal ProkBox)

Ask Claude: **"restore the previous firmware from the backup"**, or **"deploy the normal main build"**. The deploy-ipod skill:
1. backs up `.rockbox/` again, which keeps `bcmtest.log`;
2. either copies the earlier backup's files back over `.rockbox/`, or unpacks a normal `build/fw-ui/rockbox.zip` built from `main`;
3. keeps `config.cfg`, the database and bookmarks, as every install does.

The bootloader and Apple's partition were never touched, so there's nothing else to undo.

## Rollback checklist

- [ ] The iPod boots to the ProkBox main menu after a restart.
- [ ] After a backlight timeout and a button press, the screen comes back correctly (Apple's vmcs reload works).
- [ ] Settings → About shows the expected version after the uninstall.
- [ ] `bcmtest.log` has been copied off the iPod (attach it to #60).
- [ ] Photos are attached to #60: the stage 0 and stage 1 summaries, photos 1–6, the stage 2 and 3 summaries, the stage 4 summary, and the 4b summary if it was run.
- [ ] `bcmfw/bench/readlog.py` output for the stage 4 (and 4b) runs is attached to #60.
- [ ] Music plays. The test never touches playback, but it's a good final check.
- [ ] If the boot path was used: photos of the first boot with it on, and of a wake; the status screen; and whether a fallback ever happened (the log's `boot path:` lines).
- [ ] The boot path is **off** again (Debug → BCM boot path: off), unless the owner wants to keep it.

## Residual risks

1. **Panel sequence for the fitted panel.** It's verified only against Apple's handler in the simulator. Stage 0 tells us the panel ID before anything of ours runs. For IDs 1 and 3, the model variant is a guess (model 1); a wrong guess affects only gamma.
2. **Entry address.** If the core starts somewhere other than 0 or 0x200, the probe never reports ready. The watchdog cuts power after 1 s, and stage 2 never runs.
3. **CPU writes overrunning the LCD interface.** Apple sends pixels only by DMA. If photo 1 is torn or garbled, the next build sets `BCMFW_PIXEL_PACE`. It's harmless: just a bad picture.
4. **A power cut with the panel on.** This happens only on the abort path (image hung). It's the same thing that happens when the battery is pulled. The panel then goes through a full reset pulse and power-on in Apple's sequence.
5. **The restore itself.** It uses the stock `lcd_awake()`/`bcm_init()`, which runs at every backlight wake today. In this build its "ready" wait is bounded to 1 s, but the bus handshakes inside it are not: unchanged stock code. If Apple's vmcs ever failed to come back, the screen would stay dark until MENU+SELECT. **A hang right after the log line `restore: Apple vmcs (stock wake path)` means exactly this:** reset with MENU+SELECT. The restore keeps the BCM to itself until Apple's vmcs is back, so the backlight thread can't start a second load meanwhile. A failure inside stage 0 itself (Apple's vmcs not answering) also power-cycles the chip rather than handing it back as it is.
6. **Reset while our image is running.** We can't prove from our own code whether MENU+SELECT power-cycles the BCM. If it doesn't, Apple's boot code still runs first. And if our `hello` image were still running afterwards, it would answer Rockbox's commands correctly, because it implements the same contract, until the next backlight sleep reloads Apple's vmcs.
7. **Switching off during a test.** Don't. While the test owns the BCM, a switch-off skips the display power-down (harmless: the power goes anyway), and a USB plug-in ends the current hold and is handled after the test.
8. **The bench needs the VideoCore toolchain.** `scripts/validate.sh` runs `bcmfw/tests/devtest_test.py` (36 rows, including stages 4 and 4b), `bcmfw/tests/bench_test.py` and the other bcmfw benches through `scripts/bcmfw_benches.sh`, but a machine without `vc4-elf-gcc` skips them with a note.
9. **Timing is unmeasured.** Every delay in our image is at least as long as Apple's at the simulated clock. An unknown slower start-up clock only makes them longer.
10. **The boot path's in-use fallbacks are device-only code.** The swap itself (`bcmdt_boot_swap`) and the pass flags run in the bench against vc2sim, including images that never get ready, fault or hang. The hooks in `lcd-video.c` (the wake's ready check, the tick's hang detection, the update's fallback) are target code with no simulator. **test fallback** exercises the in-use path on the device; the wake path is exercised only if our image actually fails at a wake.
11. **The stock host-port handshakes are unbounded.** `bcm_read32()` and the bootstrap waits in `bcm_init()` are stock code without limits. A core that stops leaves the host port working in the simulator model, and Apple's own design relies on that; if a stopped core ever wedged the port on the device, the screen would hang until MENU+SELECT, and `bcmfw.try` (at boot) keeps the next boot on Apple's vmcs. After boot there is no marker: turn the path off from the Developer list (hold Next at boot).
12. **DMA timing on the device.** A stage 3 pass means the engine finished every transfer within 100 ms. The bus rate is still a model guess until the log's `BCM … us` figures come back.
13. **Stage 4's limits are sized from a model.** The 3 s and 20 s limits assume the chip is at least as fast as the 27 MHz crystal with at most a few cycles per instruction. A chip far slower than that would be cut off as "stalled" with nothing harmed; the log's last steps then show how far it got per second, and the limits can be raised.
14. **The simulator bench runs stage 4 with 64 iterations**, not the device's 4,096: vc2sim takes minutes for the full count. The stage code doesn't depend on the count, and `bench_test.py --device` runs the full build by itself. The device run's numbers are the first at 4,096.
15. **Stage 4b's register effects are unknown on silicon** (see its risks above). The simulator only proves the write sequence equals Apple's and that every wait is bounded; it doesn't model what the clocks do.
16. **The ARM polls during the timing.** Every 10 ms it reads two words through the host port. If the port's arbitration slows the core, the slowest/fastest ratio goes above 1.02; that shows in `readlog.py`'s `max/min` column.

## For the next session

What session 7's log will tell us, and what to do with it:
- **Photos 5e–5g against 5d.** If 5e (27 MHz) matches 5d, `BCMFW_LCD_BUS_KHZ` becomes 27000 for every image: 14.2 ms a frame, Apple's nanoseconds exactly on the 27 MHz bus, faster than Apple's own 15.2 ms. 5f (20 MHz) tests the first timing word below its nanoseconds; 5g (15 MHz, 11.4 ms) the pacing word below its 65 ns field. The first clock that shows artefacts marks the panel's real margin.
- **Stage 4.** 24/24 confirms the tie rule on MHRN and unlocks 4b. readlog's verdict line then says how RNDT and RNDU round (`vmulhr` sharing the rule or not) and whether MLSC clamps: if it doesn't, the decoder's centre j needs its own clip (`decoder.md`, device check 3). Any of the nine decoder forms named as wrong goes back to the generator before the decoder runs on the chip; all nine passing promotes them to required (`gen_bench.py` `FIRST_SILICON`).
- **Clock lines.** `M loop iterations/s` near 26 (or 13 for an image whose loop sits at `…c`); nothing to act on until 4b.
- **4b** (owner's call once stage 4 passes): measures the start-up clock directly (54 or ~80 MHz?) and runs the bench at 150 MHz. After it, the LCD bus clock must be measured again before choosing words for the PLL clock.
- **Stage 5 (dev7).** A pass plus a clean step 4 (normal use on our image) lists `hello_dma`'s FNV-1a in `bcmfw/verified.txt` (with this session named), and the release step can start: move the boot path out of the device-test code into its own file and flag (`pipeline.md`, "The everyday path"). The `host upload` figures say whether the PP's upload is now the slower leg; tearing reported in step 4 means the image should snapshot the union before pushing.
- `BCMFW_IDLE_SLEEP` (item 11) and the panel model variant are still not part of this session.
