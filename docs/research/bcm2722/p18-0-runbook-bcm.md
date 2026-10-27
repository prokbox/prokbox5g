# Phase 18-0 runbook: the video-chip part (stages 5i and 5s)

The owner's steps for the BCM2722 half of the 18-0 device round. It answers four questions for Phase 18:
- whether the chip's core can sleep and wake;
- whether its memory holds while it sleeps;
- whether the panel can wake from standby without its 265 ms cold power-on;
- whether the PP's DMA engine 2 can upload the chip's image at boot.

With a USB-C meter, it also gives the current each state draws. The detailed reference for both stages is `device-session-1.md`: "Stage 5i", "Stage 5s" and "Phase 18-0 (proposed)". This file is the short version, meant to be merged into the 18-0 session doc.

## The build

| | |
|---|---|
| Zip | `out/deploy-p18-0/rockbox-9507ca6c6e-p18-0.zip` (4,383,283 B); the earlier `2ef1befcb9` zip is kept as `.superseded` |
| SHA-256 | `fbb201239d42c03c8232fb0e9ad62c10110624dd5196d74303f5bd16463bf864` (in `SHA256SUMS` beside it) |
| Source | `p18-114` at `9507ca6c6e`: the gated P16 + P18 integration, which carries `p14-sleep-spikes` (`88407cf402`). The PP-side 18-0 steps run on the same firmware |
| Configure | `--target=22 --type=n --compiler-prefix=arm-none-eabi- --ipodui --bcmfw-devtest --bcmfw --bcmfw-video --bcmfw-l1` (the dev47 set) |
| Binary | `rockbox.ipod` 1,224,856 B; version `9507ca6c6e-261023` |
| Video-chip images | the device test's eleven, embedded. They match a fresh `mkdevtest.py` build, and only `hello_dma_sby` (8,984 B, fnv `d1cecb19`) is new. Also `.rockbox/bcmvideo.bin` 206,532 B, fnv `0cbae7a2` |

The install covers firmware files only. The bootloader and Apple's firmware stay untouched. The stage 0–5 images haven't changed, so a stage 5 pass flag (`bcmpass5.txt`) from an earlier session still counts.

## Settings on the device

- **Nothing is required.** Leave the BCM boot path and **Resident video image** as they are; the test takes the chip over by itself and gives it back afterwards.
- **Optional, for current readings:** in Developer, set **Power log: on** and **Power log every: 5 s**. Connect a USB-C meter between a plain wall charger (not a computer) and the iPod, with the battery already full.
- **Before each run:** music stopped, Hold off. Then leave the iPod alone until the summary shows: a press can light the screen and spoil a look or a reading.

## Getting there

1. **Settings → About**.
2. Hold **SELECT + Previous (|◀◀)** together for 3 s. The Developer menu opens.
3. Choose **Debug**. The BCM entries are at the top.

Every stage entry first shows a confirm screen. Scroll to its last line (**"Run stage … now (SELECT here)"**) and press **SELECT** to start; **MENU** cancels. When a run ends, its summary list appears: **photograph it**, then press MENU.

## Steps

| # | Entry | Time | What to watch and report |
|---|---|---|---|
| 0 | *(only if step 1 or 2 says "Not available")* **BCM test: stages 0-5 (all but 4b)** | about 5 min | Must end with stage 5 PASS; that writes the flag the next steps need |
| 1 | **BCM test: stage 5i (idle sleep, optional)** | about 3 min | The colour bars twice; the clock corner doesn't visibly change. Then a dark minute with one tiny update every 10 s, two short dark blinks and the summary. **Photograph the summary**, including the line **"Framebuffer words changed: N"** (0 means the chip's memory held while its core slept). Report **stage 5i PASS or FAIL**: it decides step 3 |
| 2 | **BCM test: stage 5s (sleep spikes, optional)** | about 3 min | The confirm screen says **"Chip sleep: on (5i passed)"** or **"off"**: note which. Then the sequence below. **Photograph the summary**: Wake light/deep, Reset/cold, Refused, Framebuffer changed, Upload CPU/E2, Read back wrong |
| 3 | *(only if step 1 passed)* step 2 again | about 3 min | It should now say "Chip sleep: on". Same sequence; this run's meter readings are for a sleeping core (step 2's chip was busy-polling) |
| 4 | Pull the logs | — | See "Files to pull" |

### What stage 5s shows, in order (hands off)

| Phase | Screen | Meter (if used) |
|---|---|---|
| **Look 1** | the colour bars, 15 s: the reference | — |
| A | dark 20 s | reading A: panel on, backlight off |
| B | dark 20 s | reading B: light standby |
| **Look 2** | the bars, after the light wake | — |
| | two short dark blinks (the other light cycles) | — |
| C | dark 20 s | reading C: deep standby |
| **Look 3** | the bars, after the deep wake | — |
| | two short dark blinks | — |
| D | dark 20 s | reading D: panel off, chip powered |
| **Look 4** | the bars, after a full panel power-on | — |
| E | dark 20 s | reading E: chip power cut |
| | four brief flashes of the bars (the boot uploads: CPU, engine 2, CPU, engine 2) | — |
| | the normal screen, then the summary | — |

**At looks 2, 3 and 4, compare with look 1:**
- same colours and brightness;
- nothing shifted, no stripes, no flicker;
- not washed out or inverted.

**During each wake,** say whether anything odd showed: a white flash, garbage, or a slow fade-in.

**If a look never comes,** with the screen still dark more than 5 s after its phase should have ended, note which one. The stage gives up by itself, cuts the chip's power and brings the normal screen back.

**With a meter,** write down the five readings A–E for each run (steps 2 and 3), in mA at the charger's voltage.

## Files to pull

All are in the hidden system folder at the root of the iPod's disk. Copy them off over USB after the last run:
- `bcmtest.log`: the full trace. Each run is one block starting `=== BCM test`. The lines that matter are `5i …` and `5s …`, and each block ends with a `(bcmtest.log this boot: …)` line.
- `bcmtest-results.log`: the summaries, one block per run.
- `power.csv`: only if the power log was on. The marks `s5s_on`, `s5s_light`, `s5s_deep`, `s5s_reset`, `s5s_cut` and `s5s_end` give the phase times, to line up with the meter readings.
- the photographs of the summaries (steps 1, 2 and 3) and the meter notes.

## If something goes wrong

- **Any hang:** hold **MENU + SELECT** for 6–8 s until the iPod restarts. Nothing the test does persists: the chip's memory is volatile, and the test writes only its two logs. A restart goes through Apple's own start-up of the chip.
- **A dark screen after a restart:** wait out the backlight timeout and press a button. Or hold **Play/Pause** to switch the iPod off, then switch it on again.
- **The test aborts by itself:** every wait has a limit. On any timeout or fault it cuts the chip's power and brings back the normal display driver.
- **A failed 5i or 5s changes nothing in everyday use.** Neither stage changes a setting: 5i's pass flag only unlocks chip sleep in 5s and lets the Developer list's **Video chip idle sleep** be turned on, which stays off until someone turns it on.

## What the results answer

These are the open questions in `docs/bcm2722-reference/open-questions.md`:

| # | Question | Answered by |
|---|---|---|
| 3 | Do the 1 ms tick and the host doorbell wake the sleeping core? | 5i's `5i counters`, `5i doorbell` and `5i hold` lines (ticks, doorbells, bad) |
| 50 | Does the chip's memory hold while the core sleeps? | 5i's "Framebuffer words changed", and 5s's framebuffer check after each standby |
| 49 | Does the panel wake from standby with the right picture, and how fast? | looks 2–3, and the `5s light` / `5s deep` lines: wake time from the host, and the panel's power-on time from the chip. vc2sim predicts about 137 ms against 267 ms cold |
| 12 | What does each state draw? | meter readings A–E, from step 2 (busy core) and step 3 (sleeping core) |
| 51 | Can engine 2 upload the boot image? | the `5s upload engine 2: … words read back wrong` line, and the four flashes after phase E (each upload starting the image) |

Results go into `docs/bcm2722-reference` (#3, #12, #49–#51 and the quirks) and `docs/data-paths.md`, tagged [silicon]. Last comes the regulator sweep (open question #11). It isn't in this round, and waits until 5i has answered whether the core wakes.
