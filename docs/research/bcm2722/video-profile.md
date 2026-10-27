# The recommended video profile

What a video file should look like so that our decoder (bcmfw/h264, on the BCM2722 at the verified 150 MHz) plays it smoothly on every unit, with margin. `scripts/prokbox-convert.py` writes exactly this; anything else that matches it plays the same.

Status: P19 vplayer health (2026-10-26), from sitting 2's silicon readings and vc2sim runs. Re-check after each decoder change and each sitting (bench row `vfeed`).

## The profile

| | Recommended | Why |
|---|---|---|
| Container | MP4 / M4V, `moov` anywhere | our demuxer (apps/video/mp4demux.c) reads both orders |
| Video codec | H.264 **Constrained Baseline** (CAVLC, no B frames, one slice) | the only profile our decoder takes; the 5.5G's own players took the same |
| Level | **1.3** | caps the stream at 768 kbit/s and 396 macroblocks a frame |
| Size | **320×240** (the panel), letterboxed or cropped to fill | every pixel decoded is shown; larger sizes cost the decoder 4× and are scaled down on the chip anyway |
| Frame rate | the source's own, **at most 30** (24, 25, 30; 50 and 60 halved) | the decoder's budget is per frame |
| Bitrate | **600 kbit/s average, 768 kbit/s peak, 1.5 Mbit buffer** | see "Margin" below |
| Reference frames | 3 | costs memory, not time (see the table) |
| Key frames | every **3 s**, no scene-cut key frames (`sc_threshold 0`) | a key picture is the heaviest; 3 s keeps a seek's catch-up under about a second and a half |
| Audio | AAC-LC, 44.1 kHz, stereo, 128 kbit/s | the PP's AAC path plays it at no cost to the BCM |

This is the converter's recipe as it stands (`PROFILE_VERSION` 1); the measurements below confirm it, so it is unchanged.

## Margin: what the decoder sustains

The rule (bench row `vfeed`, `vtrace_pace_ok`): **a clip plays smoothly when 95 % of its pictures decode, convert and reach the panel in under 75 % of a frame**, nothing is skipped, and A/V stays within 50 ms.

**Silicon, sitting 2 (2026-10-25, 150 MHz, merged image `80bc8611`):**

| Clip | Frame | FEED mean | p95 | max | 75 % of the frame |
|---|---|---|---|---|---|
| Stage 6 clip 1 (synthetic bars, 30 fps, 169 kbit/s) | 33.3 ms | 7.6 ms | 8.4 ms | 29.8 ms | 25 ms |
| Stage 6 clip 2 (the *Sintel* excerpt, 24 fps, 414 kbit/s) | 41.7 ms | 18.5 ms | 27.4 ms | 40.2 ms | 31.25 ms |

Both pass. The film clip's time follows its pictures' sizes closely (r = 0.82): **about 11.9 ms plus 3.0 µs a byte** of the picture's packet. Its one 40 ms picture is the 19 KB key picture at 5.1 s; its heavy P pictures (4.5 to 5.3 KB) take 27 to 29 ms.

**What that means for a stream** (dark, textured, fast real footage: the worst kind):

- at **30 fps** the p95 picture must stay near **4.3 KB** (25 ms); at 600 kbit/s the mean P picture is about 2.5 KB and the p95 about 2.3 times that, so heavy scenes sit at the line, and lighter material far under it;
- at **24 fps** there is room to 6.4 KB (31.25 ms);
- a key picture of 10 to 20 KB takes 42 to 72 ms: more than a frame, once every 3 s. It is shown up to one frame late and the next pictures catch up (the engine's model, tests/video/test_vplay.c "footage": one hitch per key picture at most). Fewer key pictures (3 s, no scene cuts) is the cheapest fix; a smaller buffer would cap them but costs picture quality, which we don't trade.

vc2sim's profile of the same clip and of candidate encodes (bcmfw/h264/test/sim_test.py, instructions per picture) is in "Simulator runs" below.

## Apple's 5.5G limits, for comparison

Apple's published limits for this iPod, in our words: H.264 up to 768 kbit/s at 320×240 and 30 fps, Baseline profile up to level 1.3, with AAC-LC audio (up to 160 kbit/s, 48 kHz stereo); and a second H.264 class up to 1.5 Mbit/s at 640×480 and 30 fps, a "low complexity" form of Baseline (commonly read as level 3.0), which the 5.5G added. Our profile sits inside the first class. We do not recommend 640×480: our decoder takes 4.2 M instructions for a VGA P picture and 5.3 M for a key picture in vc2sim (decoder.md), against a 5.0 M budget at 30 fps, and the panel shows a quarter of those pixels. A 640×480 file still plays (the chip scales it 2:1), at its own pace.

## Simulator runs

vc2sim (bcmfw/h264/test/sim_test.py, strict alignment on, every picture bit-exact against ffmpeg) on the first 130 pictures of the film clip, re-encoded three ways from its own pictures. "Cycles" is the simulator's estimate (one per scalar instruction, one per vector row). **It predicts silicon closely:** over the original clip's 302 pictures, sitting 2's FEED time is **1.3 ms + cycles / 125.7 per µs** (r = 0.99; vc2sim says 2.16 M cycles mean, 3.32 M p95, 4.95 M max for the original). So silicon runs the decoder at about 0.84 cycles' worth of the estimate per clock at 150 MHz, plus 1.3 ms a picture of conversion and panel start; the "ms" column below uses that fit.

| Encode | Bytes (130 pictures) | Cycles a picture: mean | p95 | max | Silicon ms (fit): mean / p95 / max |
|---|---|---|---|---|---|
| The converter's recipe (600k / 768k / 1.5 M buffer, 3 refs) | 451,063 | 1.97 M | 3.10 M | 4.11 M | 17.0 / 26.0 / 34.0 |
| 500k / 640k / 640k buffer, 1 ref | 369,787 | 1.98 M | 3.27 M | 3.66 M | 17.1 / 27.3 / 30.4 |
| 400k / 512k / 512k buffer, 1 ref | 294,695 | 1.98 M | 3.31 M | 3.44 M | 17.1 / 27.6 / 28.7 |

What it shows:

- **Bitrate barely moves the mean or the p95.** The cost is per macroblock first (CAVLC residual parsing 31 %, the loop filter 25 %, the macroblock bookkeeping 11 % of a P picture), per byte second. Starving the stream would cost picture quality and buy little time, so the recipe keeps its bitrate.
- **The heaviest pictures are scene cuts coded as P pictures** (`sc_threshold 0` keeps a cut a P picture; the cut at 5.1 s, a 19 KB I picture in the original, is a 10.6 KB P picture here, its time half residual parsing and a quarter loop filter): 4.1 M cycles at the converter's rate, about 34 ms on silicon. A smaller buffer caps them (3.4 M at 400k), at a visible cost on cuts. We keep the buffer; one late picture at a cut is a single repeated frame.
- **References cost nothing measurable** (3 against 1).
- **At 24-25 fps the recipe meets the rule** on this worst-case clip (p95 26 ms against 31.25 ms at 24 fps, 30 ms at 25). **At 30 fps, textured real footage sits just over it** (26 ms against 25 ms; light material such as clip 1 is far under): the lever is the decoder's per-macroblock cost, not the stream. The next merged-image batch (decoder.md, "P19 vplayer health") targets exactly the residual parser and the loop filter.
- **Batch 3 (2026-10-26, `merged-image-batch.md` §7):** the loop filter's glue and CAVLC's per-block setup, same kernels, bit-exact. The 30 fps recipe encode of the film clip: p95 3.27 → 2.95 M cycles, **27.3 → 24.7 ms** by the fit, inside the 30 fps rule (25 ms); the original 24 fps clip p95 24.4 ms. Silicon confirms in sitting 3 (`vfeed`).
