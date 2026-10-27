# The video pipeline (`BCMFW_VIDEO`)

Issue #60. This is how a video plays on our own BCM2722 image, end to end: the PP feeds H.264 packets over our host protocol (`hostproto.md`), our decoder (`decoder.md`) decodes them on the VideoCore, the vector unit converts each picture to RGB565 (and scales it 2:1 when it's larger than the panel), and DMA pushes it to the LCD. Everything here is ours: the conversion and the scaler are defined by our own exact reference (`bcmfw/video/dispref.py`), not by Apple's or ffmpeg's code.

**Status (2026-10-10):** plays end to end **in the simulator only**. `bcmfw/tests/video_test.py` drives the `BCMFW_VIDEO=1` image the way the ProkBox driver will, and every frame on the panel model is pixel-exact against ffmpeg's decode converted through `dispref.py`: the iPod clip (all 60 frames), an I-only clip, the 640×480 clip scaled 2:1, a cropped 176×120 clip letterboxed, `NATIVE` mode, and 405 frames of a real 320×180 TV episode (owner-supplied, not in the repo) across scene cuts and a forced I frame. Nothing has run on hardware: SDRAM set-up, the new vector forms and the overlapped DMA are on the device list below. There is no PP-side driver yet.

## Data flow

```
PP (Rockbox)                          BCM2722 (bcmfw, BCMFW_VIDEO=1)
------------                          ------------------------------
demux MP4 -> access units
write AU bytes  ---- host port ---->  SDRAM FIFO 0xFE000000 (1 MB)
FEED record     ---- cmd ring  ---->  hp_poll(): check order, queue (16)
                                      main loop, between command passes:
                                        hp_work(): one packet
                                          h264_annexb_next / h264_decode_nal
                                            context + MB info: SRAM 0x80000
                                            frames (DPB):     SDRAM 0xFE100000
                                          per picture: present()
                                            wait for the previous DMA
                                            [2:1 box -> strip 0xDC000]
                                            YUV -> RGB565 -> SRAM 0x105800
                                            start DMA to the panel window
                                        FIFO_RD += the packet's 64-byte units
FIFO_RD, events <--- header/ring ---  STATE heartbeat, EV_EOS, EV_ERROR
```

- **One packet at a time.** `FEED` only queues (16 deep). The main loop decodes one queued packet per pass, then checks the LCD command word and the command ring again, so an `LCD_UPDATE`, a `STATUS` or a `CLOSE` waits at most one packet (one picture: 17 ms simulated for an iPod-clip frame, 50–60 ms for a 640×480 one). Servicing the ring once per macroblock row, as `hostproto.md` §4 plans, needs the decoder to yield mid-picture; that comes with the display queue (below).
- **DMA overlaps decoding.** `present()` starts the panel transfer and returns; the next picture's decode runs while it goes out, and the next `present()` (or any LCD command, `CLOSE`, `LCD_SLEEP`) waits for it first. A single RGB565 buffer is enough because only conversion writes it, and conversion always waits.
- **Shown as decoded.** Baseline has no B-frames, so decode order is display order. This version shows each picture as soon as it's converted: the PP paces the `FEED`s against its audio clock (see the driver contract). `CLOCK` and a display queue in SDRAM are the next step.

## Memory layout

### Internal SRAM (1.25 MB)

| Range | Size | Use in a video build |
|---|---|---|
| `0x000000–0x0001FF` | 512 B | vectors, VP header (`0x100`), LCD mailbox (`0x1E0`) |
| `0x000200–0x011DDF` | 73,184 B | **the image**: 72,064 B loaded (code 66,220, tables 5,332), `.bss` 1,120 (incl. the 512-byte chroma scratch). 14% of the 510 KB image region, **5.6% of SRAM**. The build fails if it reaches `0x80000`. |
| `0x080000–0x0CBFFF` | 304 KB | decoder context and per-macroblock info: 41 KB at 320×240, 151 KB at 640×480 (`h264_ctx_size`) |
| `0x0CC000–0x0CFFFF` | 16 KB | stack (the decoder's deepest path is under 2 KB) |
| `0x0D0000–0x0D2FFF` | 12 KB | command and event rings |
| `0x0DC000–0x0DDDFF` | 7,680 B | the scaler's strip: 16 rows of the half-size picture (Y 320×16, U and V 160×8) |
| `0x0E0000–0x1057FF` | 150 KB | Rockbox's framebuffer, untouched: the UI comes back on `CLOSE` with one `LCD_UPDATE` |
| `0x105800–0x12AFFF` | 150 KB | **the RGB565 picture**, 320×240, rows 640 bytes apart; DMA reads it |
| `0x12B000–0x13FFFF` | 84 KB | spare |

### SDRAM (4 MB: `0xFE000000` for the BCM, `0xC0000000` through the host port)

| Range | Size | Use |
|---|---|---|
| `0x000000–0x0FFFFF` | 1 MB | bitstream FIFO, written by the PP (header: `HP_FIFO_BASE = 0xC0000000`, `HP_FIFO_SIZE = 0x100000`) |
| `0x100000–0x2FBFFF` | 2 MB − 16 KB | the DPB (`h264_set_frames`): (refs + 1) × 115 KB at 320×240 (461 KB with 3 references), 1.84 MB at 640×480 with 3 references; 4 references at 640×480 don't fit (`H264_E_NOMEM`, refused at the SPS) |
| `0x2FC000–0x2FFFFF` | 16 KB | slack: the display path reads whole 32×16 tiles, up to 16 rows past a plane |
| `0x300000–0x33FFFF` | 256 KB | linear copy of a packet that wraps the FIFO's end (packets above 256 KB that wrap are dropped as errors) |
| `0x340000–0x3FFFFF` | 768 KB | spare (the display queue and the 2-D cache of `hostproto.md` §7 later) |

The hot data stays in SRAM: the decoder context, macroblock info and caches, the stack, the vector scratch, the RGB565 picture. SDRAM holds what is big and streamed: the bitstream, the reference pictures, the wrap copy. The simulator charges nothing for SDRAM; on the device its latency (cache off) is a question for the timing bench (`bringup.md`).

**SDRAM set-up** runs once, at the first H.264 `OPEN` (after the switch up when `OPEN` asks for 150 MHz), in Apple's order as dev10 re-read it from vmcs 1.3 (`hardware.md`, "SDRAM controller"): the refresh interval (`+0x24`), the timing words (`+0x10`/`+0x14`), the cache flushed and off, `+0x30 = 0x80E8`, then the mode word and command 7, the geometry probe, and `+0x30 = 0x64` with `0x10000C00` bit 16. The refresh interval follows the clock: 210 cycles at 27 MHz, Apple's 1,171 at 150 MHz, both 7.8 µs.

**Session 9 (2026-10-12): the first run on silicon failed.** `OPEN` answered `HP_E_NOMEM` for both clips at 150 MHz. Our dev9 code differed from Apple's in four ways:
- it treated command 7's poll timeout as fatal, where Apple ignores it and lets the probe decide;
- its fallback mode word used field 7, where Apple's @12d90 turns 7 into 4;
- it had no controller reset before the retry;
- it wrote `0x10000C00 = 0` before the flush instead of after.

dev10 follows Apple on all four. It also adds:
- a 200 µs wait before the first command;
- waits bounded by the 1 MHz counter (5 ms) instead of loop counts;
- four attempts (field 8, field 4, then both with the cache on as Apple has it) instead of Apple's endless loop;
- a report in `OPEN`'s reply (`hostproto.md`, "OPEN's SDRAM words").

**Session 10 (2026-10-12): SDRAM came up, but decoding died.** SDRAM came up on attempt 2 (field 4) and the 6s readback was right. Then, about 250 ms into each clip, the host port itself timed out, with no picture decoded. A software hang leaves the port answering; this didn't, so the bus locked. The suspect is the vector unit's access to SDRAM (device list item 3 below): with the frames in SDRAM, motion compensation, the transform, the loop filter and the conversion all load and store vector rows there.

**dev11: the frames in SRAM.** The decoder's arena is internal SRAM from `0x20000` (the image must end below it) up to the stack at `0xCC000`, 704 KB. The context takes 41 KB at 320×240, and the frames follow it whenever they fit: 113 KB each, references + 1 of them, so up to 4 references. The converter caps them at 3. Larger streams (640×480) put their frames in SDRAM as before. SDRAM keeps the bitstream FIFO and the wrap copy, both scalar-only. Stage 6's "6d" pass runs clip 1 with `HP_OPEN_SDRAM_DPB` to measure the vector unit against SDRAM directly (`hostproto.md`). A lost image is read word by word before its power is cut. A loss at 150 MHz reloads at 27 MHz.

The player retries a failed 150 MHz `OPEN` once at 27 MHz (the image switches back down on any failure). Stage 6 logs the words and a "6s" readback: a pattern written through the host port into the FIFO, read back at 64 places.

## The display path

### Colour conversion (exact)

`dispref.py` defines it; the kernels match it bit for bit. BT.601, limited range (what every iPod-era H.264 encoder writes), with integer coefficients scaled by 4096 and chosen even, so the −128 chroma offsets fold into constants exactly:

```
yt = (Y * 4769) >> 8
R  = clip((yt + ((V *  6538) >> 8)                       - 3567 + 8) >> 4)
G  = clip((yt + ((U * -1604) >> 8) + ((V * -3330) >> 8)  + 2169 + 8) >> 4)
B  = clip((yt + ((U *  8262) >> 8)                       - 4429 + 8) >> 4)
rgb565 = (R >> 3) << 11 | (G >> 2) << 5 | (B >> 3)        (>> floors)
```

Every intermediate fits a signed 16-bit lane (the reference checks it). Black (16, 128, 128) is `0x0000`, white (235, 128, 128) `0xFFFF`. Chroma is repeated (pixel (x, y) takes chroma (x >> 1, y >> 1)); RGB565 truncates. We chose our own definition over matching swscale because swscale's RGB565 output dithers, and its exact tables are an implementation detail; ours is written down, small, and testable for every input.

### Scaling and placement

- **`FIT`** (the default): a picture up to 320×240 is shown 1:1, centred, with black borders; a larger one (up to 640×480) is halved by a 2×2 box on each plane, `(a + b + c + d + 2) >> 2`, then converted. The output is `(w/2) & ~1` × `(h/2) & ~1`, so 640×480 fills the panel. Other sizes are letterboxed at half size (e.g. 480×360 shows as 240×180); arbitrary ratios would need a polyphase scaler, which isn't worth it for iPod content.
- **`NATIVE`**: 1:1, centred, cropped to 320×240 (the centre of a 640×480 picture).
- **`FILL`**: refused (`HP_E_UNSUPPORTED`), since it needs a non-integer scale.
- **Placement:** x = ((320 − w′) / 2) & ~1, y = (240 − h′) / 2. The borders are cleared once per picture size (a black 320×240 push); after that only the picture's rectangle goes out, through the panel window.

### The kernels (`bcmfw/video/gen_disp.py` → `disp_kernels.S`)

Generated through the decoder's operand encoder (`vpu_asm.py`, which assembles each instruction, decodes it again and compares the operands), straight-line, no accumulator chains under REP.

- **`disp_yuv_tile`**: one 32×16 tile in 85 instructions (70 vector; one more since device session 4, eight more since session 5, below). Chroma is repeated horizontally without a shuffle: each chroma row is loaded twice, into the low and high halves of a 16-bit row (U in VRF rows 0–7, V in rows 8–15, before the luma arrives there), so one `vst16 d00` with REP 16 writes `u0 u0 u1 u1 …` to a 512-byte scratch (U at +0, V at +256), and `vld8` brings it back as full-width rows. Even and odd picture rows load as separate blocks (stride 2 × pitch), so each chroma row serves both with every operand stepping one row per REP. Then 16 rows at a time: `vmulm` for the luma term and the chroma terms, `vadd`, `vmulhr #0x1000` into 16 bits then `vadds` to a zero row (= clip((q + 8) >> 4); device session 5: `vmulhrs` into 8 bits wraps instead of clamping, so the clip is a separate saturating add), `vlsr`, and the packing `vmul` by 2048 and 32 plus two `vadd`s, written as 16-bit rows and stored with `vst16`. **Stores only from `d00`** (device session 4: a 48-bit store from any other row did not land): the last `vadd` (+ B) runs in two REP 8 halves into rows 0–7, each stored from `d00` (left 16 pixels, then right at +32), one instruction more per tile and the same REP rows.
- **`disp_box2_16`**, **`disp_box2_8`**: 32×32 → 16×16 and 32×16 → 16×8 in 9 instructions (6 vector). A 32-byte row loaded as a 16-bit vector deinterleaves itself (even bytes in the low half, odd in the high), so the four samples of an output are already lane-aligned: two `vadd`s, one more, and `vmulhrs #0x4000` into rows 0.. (the consumed input), stored from `d00`. It needs 32-byte aligned rows (the decoder's frames are); otherwise `disp.c`'s scalar box does the same arithmetic.
- **Tiling (`disp.c`):** whole tiles; up to 31 columns and 15 rows past the picture are written into the RGB565 buffer's margin (never pushed) and read from the source (the 16 KB slack).

**Bench:** `bcmfw/tests/disp_test.py`, **884 cases, 0 failures, about 40 s**, under session 4's silicon profile (a 48-bit store from a row other than `d00` writes nothing; `--model` for the corpus reading; the kernels before the change fail every row of it): random tiles at every source alignment, every (Y, U) and (Y, V) pair at the other chroma's 0/128/255, both box kernels on random and 0/255 data, whole pictures with cropped origins (and nothing written past the tiles), and the scaled path on both its vector and scalar branches. A one-unit change to one constant in the kernel fails it.

## Session control in the image

- **`OPEN`** with codec 1: validates size and mode, sets up SDRAM once, opens the decoder (`h264_open` in SRAM, `h264_set_frames` in SDRAM). The reply carries the session, the FIFO's host address and size, and (minor 1) the queue depth, 16.
- **`FEED`**: order and length checked as in the skeleton, then queued; `HP_E_BUSY` if 16 are queued (the PP never lets that happen). `FIFO_RD` moves when the packet has been decoded, in order.
- **Decoding a packet:** Annex B NAL units (`h264_annexb_next`; leading garbage is skipped). A packet that wraps the FIFO is copied to the wrap area first. Each finished picture is presented, with the PTS of the packet that held its first slice.
- **Errors** are concealed by the decoder and counted (`STATUS` word 8). `EV_ERROR(HP_E_CORRUPT, pts)` at most once a second (every 64 errors without the counter). `H264_E_UNSUPPORTED` or `H264_E_NOMEM` before the first picture is fatal: `EV_ERROR(HP_E_UNSUPPORTED or HP_E_NOMEM)`, state `HP_ST_ERROR`, and the remaining packets are consumed without decoding, so the PP never stalls.
- **`FEED_EOS`**: after its packet, `h264_flush` shows a picture still in progress, then `EV_EOS(session, last PTS)`.
- **`CLOSE`** waits for the last DMA, and replies packets, bytes and (minor 1) pictures decoded, shown and errors. **`LCD_SLEEP`** ends the session as before (`EV_LOST`).
- **`STATUS`** (minor 1): words 6–12 are pictures decoded, shown, decode errors, macroblocks concealed, packets queued, and the PTS of the picture on screen (lo, hi).

The protocol version is now 1.1 (`0x56500101`): minor 1 only adds reply words in a video session; the null-codec skeleton replies as before.

## Timings

**Running it.** `bcmfw/tests/video_test.py` (the corpus clips, `NATIVE`, 12 fuzz cases: about 10 minutes). Real content runs the same way from Annex B excerpts beside their ffmpeg decode: `ffmpeg -i FILE -ss T -t D -an -c:v copy -bsf:v h264_mp4toannexb -f h264 build/video/real/x.264`, then `ffmpeg -i x.264 -f rawvideo -pix_fmt yuv420p x.yuv`, then `video_test.py --only clips --clips build/video/real/x.264`. Excerpts stay under `build/` and are never committed. The host decoder test (`bcmfw/h264/test/h264_test`, ASan/UBSan) was bit-exact on the same excerpts, in both arena layouts.

`video_test.py` counts every instruction by address and splits it by function into stages: **decode** (every function of `bcmfw/h264` and its vector kernels), **convert** (`disp_yuv_tile`, `disp_convert`), **scale** (`disp_box2_*`, `disp_scale2`), **push** (window, DMA start and wait, the LCD helpers), **glue** (protocol commands, `video.c`), **memops** (`memset`/`memcpy`: the border clear and the decoder's own), and **poll** (the main loop when there's nothing to do: excluded from the work per frame). Per frame = between two consecutive `present()` calls. Cycle estimates count each vector instruction once per REP row (`decoder.md`'s model; unmeasured on the device).

Thousands of instructions per frame, mean (max); `video_test.py`, all frames of each clip:

| Clip | Frames exact | decode | convert | scale | push | glue | memops | **work** | cycle estimate |
|---|---|---|---|---|---|---|---|---|---|
| iPod clip `d_ipod_768k` (320×240, 768 kbit/s) | 60/60 | 1,585 (2,032) | 14 | 0 | 0.3 (2) | 4 (32) | 63 (141) | **1,666 (2,109)** | 1.88 M (2.35 M) |
| I-only `a_ionly_qp32` | 4/4 | 1,520 (2,077) | 14 | 0 | 1 (2) | 44 (58) | 67 (141) | **1,650 (2,210)** | 1.88 M (2.49 M) |
| `e_vga` 640×480 → 320×240 (2:1) | 10/10 | 3,720 (5,581) | 14 | 12 | 0.6 (2) | 22 (146) | 236 (335) | **4,005 (6,007)** | 4.63 M (6.50 M) |
| `g_crop` 176×120, letterboxed | 10/10 | 375 (739) | 5 | 0 | 0.4 (2) | 6 (29) | 24 (96) | **410 (790)** | 0.48 M (0.88 M) |
| real 320×180 TV episode, 3.3 s with two scene cuts (owner-supplied, not in repo) | 98/98 | 1,122 (3,435) | 11 | 0 | 0.3 (2) | 3 (112) | 49 (131) | **1,190 (3,600)** | 1.35 M (3.83 M) |
| the same episode, 10.3 s across a forced I frame (GOP 300) | 307/307 | 1,537 (3,812) | 11 | 0 | 0.3 (2) | 2 (133) | 47 (129) | **1,600 (3,990)** | 1.79 M (4.20 M) |

**Device session 4's store rule, the cost (2026-10-11, same clips, before → after):** decode unchanged to the instruction (the decoder's kernels gained nothing: `decoder.md`); convert +150 per 320×240 frame (one instruction per tile: 13,792 → 13,942 on the iPod clip, 14,100 → 14,250 on VGA), its REP rows unchanged (122 k cycles); the box scaler unchanged. Work per frame: iPod clip 1,665,530 → 1,665,909 (+0.02%), VGA 4,004,291 → 4,005,298. The rest of the difference is the merged session-4 image code, not the kernels: the DMA wait's step words and report (push −134 / glue +363 per frame on the iPod clip; the push maximum, the first frame's, fell from 7.2 k to 2.0 k with the merged `lcd.c`/`dma.c`, not yet looked into). Every frame of every clip, the two real excerpts included, is still pixel-exact.

**Device session 5's clip, the cost (2026-10-11, same clips, before → after):** `vmulhrs` into 8 bits wraps on silicon, so each of the tile's six colour clips became `vmulhr` into 16 bits plus a `vadds` to a zero row (one `vmov` per tile makes it): eight instructions per tile. Convert 13,942 → 15,142 instructions per 320×240 frame (+1,200), 122 k → 137 k cycles (+12%); VGA 14,250 → 15,450. Decode 1,585 k on the iPod clip (+0.03%, `decoder.md`). Work per frame: iPod clip 1,665,909 → 1,667,607 (+0.1%, including the merged dev5 image code), VGA 4,005,298 → 4,009,246. Every frame of every clip is still pixel-exact, now under the silicon model (`disp_test.py` and `vpu_test.py` run vc2vpu's session-5 default).

- **Convert:** 14 k instructions per 320×240 frame (150 tiles × 77 + the tiling loop), about 122 k cycles counting REP rows: 2.4% of the budget. **Scale:** 12 k instructions, 58 k cycles for 640×480 (and the conversion then runs on the half-size strip).
- **Push:** 7 k instructions the first time (window set-up plus the black border clear), a few hundred after that: the transfer itself overlaps the next decode. In the model the 320×240 transfer takes 7.7 ms (10 pixels/µs, a guess for the LCD bus).
- **Memops:** the decoder's per-picture `memset`s and, once per size, the 150 KB border clear (141 k).
- **Glue:** packet queue, NAL scanning, events: 3 k per frame.
- **Real content:** live action decodes at the corpus's cost; the costliest frames are the I frames at scene cuts (3.4–3.6 M instructions) and at the forced GOP boundary (4.0 M), still under the budget. Every frame of both excerpts is pixel-exact on the panel, including the letterbox: the SPS codes 320×192 with 12 rows of frame cropping, and the 320×180 picture lands at y = 30 with black bars.

**Against the 5.0 M budget** (150 MHz, 30 fps):

- **320×240 (the iPod format): fits with room.** 1.66 M instructions per frame on average (33%), 2.1 M worst; about 1.9 M cycles by the REP-row estimate. The display path adds 2.5%.
- **Real 320×180 episodes: fit.** 1.2–1.6 M on average, under 4.2 M cycles for the worst scene-cut I frame.
- **640×480: on average only.** 4.0 M instructions (80%), but I frames reach 6.0 M (4 of 10 frames above 5.0 M cycles by the estimate). The decode is what's over (`decoder.md`); the scaler and conversion cost 0.2 M. A VGA source wants a frame queue to absorb I frames, or a lower frame rate.
- These are instructions in the simulator. SDRAM latency with the cache off, the real vector issue rate and the scalar IPC are unknown until the timing bench runs on the device.

**Host side:** per frame, 3,138 host-port transfers for the iPod clip (writing 4.4 KB of bitstream, plus 24 per `FEED` record), 2,604 for the real 320×180 episode, 4,434 for 640×480. At 4–8 PP cycles per transfer (`hostproto.md` §8) that is 12–25 k cycles per frame: under 3% of the unboosted 30 MHz PP at 30 fps.

## The ProkBox driver contract

What the Rockbox player's video driver must do (modelled by `video_test.py`'s `Player`, which is the reference for these rules).

1. **Attach.** After `bcm_init()`, read `HP_VERSION`: magic `'VP'`, major 1, minor ≥ 1, and `HP_CAPS` with `HP_CAP_H264_BP`. Otherwise offer no video (logged, no splash). Read the ring and FIFO addresses from the header; never hard-code them.
2. **Open.** `OPEN(codec 1, w | h << 16, HP_MODE_FIT | HP_OPEN_OWN_DISPLAY [| HP_OPEN_FAST_CLOCK], 0, 320 | 240 << 16)`, timeout 200 ms (the first one sets up SDRAM). `HP_OPEN_FAST_CLOCK` only with stage 4b and 4c pass flags ("The 150 MHz clock"). Keep the reply's FIFO size and queue depth. Even sizes up to 640×480; anything bigger is refused by the decoder at its SPS.
3. **Packets: from MP4 to `FEED`.** One access unit per `FEED`: one MP4 sample (`stsz`/`stco`/`stts` give its offset, size and time), written as Annex B.
   - **NAL units:** an MP4 sample is a run of `[length][NAL]` pairs, with the length field `lengthSizeMinusOne + 1` bytes wide (from `avcC`; 4 in every iPod file). The PP copies the sample into the FIFO through the host port anyway, so it writes `00 00 00 01` in place of each 4-byte length as it goes: same size, no extra pass. (A 1- or 2-byte length grows by 2–3 bytes per NAL; the PP computes the packet's length first.)
   - **Parameter sets:** `avcC` holds the SPS and PPS (counts, then 16-bit lengths and the NALs). Put them, with start codes, in front of the first packet and of the first packet after every seek (about 30 bytes); the decoder re-parses identical ones at no cost.
   - **The record:** FIFO offset, length, PTS in µs (64-bit, from `stts` and the track's timescale), flags `KEY` on sync samples (`stss`), `EOS` on the last.
   - The decoder takes NAL units in any packetisation, so an Annex B elementary stream (`.264`) also works: split it at access units as `video_test.py` does.
4. **Buffers.** The FIFO is 1 MB of SDRAM: 10 s of 768 kbit/s video. Packets start 64-byte aligned, follow each other in order, and may wrap the end (the BCM copies a wrapping packet, up to 256 KB, to its wrap area). Free space = size − Σ align64(len) of the packets not yet consumed; consumption shows in `FIFO_RD` (64-byte units, encoded). At most 16 packets in flight (the queue depth).
5. **Cadence: data in bulk, records on time.** Write bitstream into the FIFO in large bursts from Rockbox's buffering (`docs/hardware.md`: bulk I/O), whenever it has room. Post each `FEED` record when the audio clock reaches the packet's PTS minus a lead of one frame plus the decode time (about 35 ms at 320×240; 70 ms for 640×480). The image shows a picture as soon as it's decoded, so the `FEED` cadence is the display cadence. A record is 24 host-port transfers. Late by more than a frame: still feed it (decode order matters for P frames); drop at the PP only whole GOPs, from the next key packet.
6. **Sync hooks.** `STATUS` words 7 and 11–12 give the pictures shown and the PTS on screen: poll it every 500 ms with the liveness check, and adjust the lead if the shown PTS drifts from the audio clock. `CLOCK` (BCM-side scheduling against a media clock) and `PAUSE`/`SEEK` are the next protocol step; until then, pause = stop posting records, seek = `CLOSE`, `OPEN`, feed from the key packet at or before the target.
7. **End.** Post the last packet with `EOS`, wait for `EV_EOS` (it carries the last PTS), then `CLOSE` (100 ms) and send one `LCD_UPDATE` so the UI repaints.
8. **Errors.** `EV_ERROR(HP_E_CORRUPT)`: log and carry on (concealed). `STATE` = `HP_ST_ERROR` or `EV_ERROR(HP_E_UNSUPPORTED / HP_E_NOMEM)`: close, log, return to the UI with the audio still playing. Liveness as in `hostproto.md` §4: the heartbeat must move within 200 ms; a picture takes up to 60 ms (640×480), so a busy image never trips it.
9. **Cost on the PP** (measured, see "Host side"): about 3,100 host-port transfers per frame for the iPod clip, under 3% of the unboosted PP.

## The everyday path: our image as the display driver (dev7)

The display, not video, is what every user sees every second, so it's the first thing our image does for real. Policy, decided on #60 with the main session (2026-10-11):

**What gates it.**

| Step | Build | At run time | Who decides |
|---|---|---|---|
| Opt-in (now) | `--bcmfw` (test builds) | the debug toggle "DMA (stage 3 or 5)" and a stage 3 or stage 5 pass flag naming the embedded image | the owner, per unit |
| Default (after session 7) | `--bcmfw --bcmfw-default` | no toggle file: the DMA image, if a stage 5 pass flag names it **or** `bcmfw/verified.txt` lists its FNV-1a | the build: an image is listed only after it passed stage 5 on a real unit, with the session named |
| Developer switch | any | "BCM boot path: Apple vmcs (forced)" writes `apple`; Apple's vmcs whatever the build | the owner |

Stage 5's pass flag is the bar for the default because stage 5 is the daily-use test: partial updates as a list scrolls, full frames, sleep/wake, and a stop with its fallback. Stage 3 alone stays enough for the opt-in. `verified.txt` exists so a unit that never ran the device test (a fresh install, #43's public build) can still get the default once one unit has verified that exact image; its hash ties the listing to the bytes.

**Display timing.** Every image uses the 27 MHz words (dev8; 14.2 ms a full frame, Apple's nanoseconds exactly on the measured bus), adopted after session 7's fine-pattern photos at 27/20/15 MHz showed no difference from 40. The 40 MHz (17.1 ms) and 100 MHz words are one-line fallbacks (`BCMFW_LCD_BUS_KHZ`).

**Fallback (every path ends on Apple's NOR vmcs with the normal screen).**
1. **At boot:** `bcmfw.try` is written before the swap, and only a confirmed first frame removes it. If the image never gets ready, faults or never finishes the first frame, the power is cut, Apple's vmcs is reloaded, and the marker stays: no later boot swaps until a mode is chosen again.
2. **At a wake:** `bcm_init()` reloads Apple's vmcs if ours isn't ready within 1 s or reports the fault word.
3. **In use:** `lcd_tick()` treats the fault word, or one update busy for 2 s, as a stop; the next `lcd_update_rect()` cuts the power and wakes onto Apple's vmcs. `bcm_command()` is bounded to 1 s.
4. **Strikes:** each boot that fell back at run time is a strike (dev8: not one asked for, "test fallback" or the device test taking the BCM; logged "test fallback: no strike") (`bcmfw.strikes`, written with the idle flush, so never an extra spin-up). At 3, no boot swaps until a mode is chosen again. A boot that runs our image for 30 minutes without a fallback clears them.

**Sleep and wake with the backlight.** Unchanged from stock: `lcd_sleep()` sends `LCD_SLEEP` (our image runs the panel's power-off script) and `0xC` (acknowledged), then cuts the BCM's power; `lcd_awake()` re-uploads whatever `flash_vmcs_*` points at (our image while active) and sends the first `LCD_UPDATE`, which powers the panel on. Stage 5 times this cycle (`wake N:` lines).

**The update contract (lcd-video.c with `HAVE_BCMFW`, our image active).**
- The PP writes pixels into the BCM's framebuffer at `0xE0000` as before, and adds each rectangle to a pending union.
- The kick (thread or tick) sends **`UPDATERECT`** with the union at `0x1F0`/`0x1F4` when it is smaller than the screen, otherwise `LCD_UPDATE`. Our image sets a panel window and pushes only those pixels by 2-D DMA (session 5: a 100×50 rect in 2.1 ms, against 17.1 ms for a frame).
- Busy is the command word still holding the kick, or `0xFFFF`. A rectangle written while the BCM is busy waits in the union; the tick kicks it when the BCM is done.
- A stalled update (busy past 50 ms) is re-kicked with the whole frame. The first update after a wake is always the whole frame.
- After a DMA timeout the image re-sends the frame by CPU and stays on CPU push until it is next uploaded (stage 3's forced timeout; the black band is its mark).
- With Apple's vmcs, and in any build without `HAVE_BCMFW`, the code is the stock code, textually unchanged.

**Not done yet (the release step).** The boot path lives in the device-test code (`bcm-devtest-*.c`, `HAVE_BCMFW_DEVTEST`), which a release build never includes. Making the image the default in a release means moving the swap, the flags and the embedded DMA image out of the test code into a small `bcmfw-boot.c` with its own flag. That waits for session 7's stage 5 result.

**For the shell (flags, not changes).**
- **Partial updates now pay off on the panel side too:** a 320×56 two-row update pushes in about a quarter of a frame's time. Keep updates full-width and partial-height where possible (one host-port write run per update, `lcd-fps-ipodvideo.md`).
- **The PP's upload is now the slower leg.** Stage 5 logs the host upload per rectangle kind and a whole frame (`host upload, whole frame:`). If it is well above 17 ms, the next gain is on the PP side (the upload loop, or BCM-side scroll/blit jobs from `approach.md`), not on the panel.
- **Tearing:** our DMA reads the framebuffer live, so an upload that lands while the previous push runs can show half old, half new for one frame. It is invisible in menus; during slides it might show. If it does, the image can snapshot the union into a second buffer before pushing (about 150 KB per frame, cheap for the VPU), or the PP can wait for not-busy before uploading the next animation frame.
- **Kick latency:** a rectangle that arrives while the BCM is busy waits for the next tick (up to 10 ms). The BCM's interrupt line to the PP (GPIO C bit 6, `lcd-video.c`) could kick it at once; not needed until animations show it.

### Session 7: what the upload explains (dev8)

The owner's wishes after session 7: thumbnails load slowly while scrolling, and the panel panning isn't perfectly smooth. How much of each the host upload explains:

- **The upload is about 0.25 µs a pixel unboosted** (inferred from stage 5's per-row times; dev8's lab measures it). Session 7's "130 ms for the list area" was the test drawing its own pattern; the list area's bus time is about **18 ms unboosted** (perhaps 7 ms boosted, if the PP's loop is the limit), then **16 ms of push** (27 MHz words: about 13). Upload and push already overlap across frames (the next upload runs while the panel takes the last), so a list scroll is bounded near **30–55 updates a second**, not 7.
- **Thumbnails:** a row's thumbnail is a small rectangle: about 3,000–5,000 pixels, roughly 1 ms of upload and 1 ms of push. A visible delay in thumbnails appearing is the shell's art path (finding, reading and scaling the art; the main session's 8b), not the display path: the upload explains at most a few milliseconds a row.
- **Panel panning:** each drift frame is a 160×240 rectangle (38,400 pixels): about 10 ms of upload unboosted plus 240 per-row address set-ups (the lab measures their cost), then about 7 ms of 2-D push. The PP's own cross-fade and compositing is 40 ms a frame unboosted (`offload.md`), and the drift steps one pixel per frame at about 7 frames a second. **The steps' cadence and the PP's compositing explain the unevenness more than the upload does.** Upload changes alone won't make it smooth; the shell can boost during the drift, pan by half-pixel blends, or move the cross-fade to the BCM (`offload.md`).
- **What dev8 changes on this path:** nothing yet by default. `BCMFW_UPLOAD = 1` (8-register bursts) and a PP DMA upload wait for session 8's lab numbers; the wake saves the framebuffer upload (about 19 ms) by powering the panel first.

**Wake (dev8).** With our image, `lcd_awake()` kicks `LCD_POWERON` (ours, `0x40`) as soon as the image is up, uploads the framebuffer during the panel's power-on, then kicks the frame once the power-on is done (`bcm_poweron_wait()`, bounded by the first-update limit; a timeout is a stopped image). The panel's own power-on waits (Apple's script, about 265 ms) dominate a wake (about 283 ms of 436–458 ms in session 7) and stay. Shorter wakes need the panel kept powered through sleep (its sleep-in/sleep-out commands instead of a power cut) at some standby cost, a measurement for later.

## The 150 MHz clock (dev9)

Session 7's stage 4b ran Apple's PLL switch on the device: every timing kernel ran 5.56 times faster, the 1 MHz counter was unchanged, and the core starts on the 27 MHz crystal (`hardware.md`, "Device session 7"). The decoder's 5.0 M cycles-per-frame budget assumes 150 MHz. dev9 puts the switch in the video image; the UI and boot images stay on the crystal.

**In the image** (`bcmfw/clock.c`, `BCMFW_CLOCK` builds):
- **Up:** `clock_fast()` is the 4b bench's sequence, moved out of `bench.c` unchanged (the bench calls it; `bench_test` still matches its writes against `vmcs.bin` 1.3's): run control, `+0x4C`/`+0x54`, the ten block clocks off and `+0x28 = 1`, PLLs B, C, D, A off, the regulator to level 15 and 10 ms, PLL A at M = 0x6C, N = 0x258, 1 ms, enable, then `+0x28 = 0x101`.
- **Down:** `clock_slow()` is ours, the reverse [L]: `+0x28 = 1` (the core back onto the crystal; Apple's start-up makes the same write before it turns the PLLs off), PLL A off, the regulator back to the level read before the switch, 10 ms. Stage 4c is its first device run.
- **What follows the clock:** the SDRAM refresh interval (`video.c`: `0x493` cycles at 150 MHz, Apple's value; `0xD2` at 27 MHz; both 7.8 µs). It goes up *after* the switch up and down *before* the switch down, so it's never too slow for the clock that's running. The LCD bus timing (`lcd.c`): **Apple's own 150 MHz words** while the core is on the PLL, ours (27 MHz) on the crystal. If the bus stays on the crystal, Apple's words are only slower (about 45 ms a frame); if it follows the core, they're the ones Apple's firmware drives this panel with. Stage 4c measures which.
- **The mailbox:** `0x140` state (`'CK' << 16`, bit 0 on the PLL, bit 1 a wait hit its spin limit), `0x144`/`0x148` the readback (regulator before/after, clock select, PLL A), `0x14C` the switch times (µs, up in the low half, down in the high).

**The video session** (`hostproto.h`): `OPEN` with `HP_OPEN_FAST_CLOCK` (`0x200`) switches up before SDRAM's first command and the decoder start; `CLOSE`, `LCD_SLEEP` and a lost session switch down. Without the flag the session runs on the crystal exactly as before (`video_test.py` checks both: identical pictures, the refresh and timing writes in order).

**The host's gate.** The PP sets `HP_OPEN_FAST_CLOCK` only when `bcm_devtest_fast_clock_allowed()` says yes: a stage 4b pass flag naming the embedded 150 MHz bench (`/.rockbox/bcmpass4b.txt`, new in dev9: "4b bench_pll size fnv") **and** a stage 4c pass flag naming the clock-command image (`bcmpass4c.txt`). 4b proves the switch; 4c proves the panel and the switch down. Until both exist on a unit, its video runs at 27 MHz: a 320×240 clip's 1.9 M cycles a frame is 70 ms there, so only about 14 fps (the player logs it and plays at what it gets; no splash). A release build would replace the flags with `bcmfw/verified.txt`-style hashes, as the boot path does.

**Stage 4c** (devtest, dev9; needs stage 1 in the run and a 4b flag): the clock-command image (`hello_dma` + `BCMFW_CLOCK_CMD`, `CMD_CLOCK` = `0x41` with the request in `0x168`). A DMA frame on the crystal, up, a frame and a 2-D rectangle on the PLL (photos 4c-1, 4c-2), down, a frame on the crystal again (photo 4c-3; it must take within 10% of the first), up once more, then `LCD_SLEEP`, which must leave the core on the crystal. Its frame on the PLL answers the bus question (`readlog.py` prints it): about 45 ms means the bus stays on the crystal (then the panel words for 150 MHz can be ours at 27 MHz, and a video frame's push costs 14.2 ms of DMA, overlapped with decoding); about 16 ms means half the core (75 MHz: recompute our words for it); about 8 ms means the core's own clock.

### Race to idle: when the UI image should run at 150 MHz

The question (approach.md, "Use the hardware fully", item 4): for UI work that is bursty (a 2D job, a cross-fade frame, a scaled thumbnail), is it cheaper to switch to 150 MHz, finish 5.56 times sooner and idle, or to stay at 27 MHz? Nothing is decided without measurements. The UI image stays at 27 MHz until then.

**What to measure** (each on the device, each a later devtest stage or a stage 5b variant):
1. **The switch's own cost.** `0x14C` gives the image-timed switch up and down (the 10 ms regulator wait dominates: about 11 ms up, 10 ms down). A burst shorter than about 20 ms of 27 MHz work can't win on time.
2. **The PLL's idle draw.** The same idle image (no panel traffic, `BCMFW_IDLE_SLEEP` on) held at 27 MHz and at 150 MHz, 120 s each, A/B ×3, with the developer power log at 5 s and `powerlog_mark()` at each phase boundary (`docs/hardware.md`, "Rundowns and A/B tests", from `p10-powerlog`). The USB-C meter on a charge-only supply (`offload.md` §6, method A) is the primary reading; `power.csv`'s voltage slope (method B) is the coarse cross-check.
3. **The work's draw at each clock.** One real job (the drift's cross-fade frame, `offload.md`) run continuously at each clock, same A/B method: gives energy per job at 27 and at 150 MHz.
4. **The latency budget.** At 7.4 jobs a second, a job has 135 ms. At 27 MHz the cross-fade kernel takes about 2.6 ms (`offload.md`'s 0.89 ms at 78 MHz, rescaled to the measured clock): it never needs 150 MHz for time; at 150 MHz about 0.5 ms.

**Decision rule.** Per job type: switch up only if (energy per job at 150 MHz + the switch's energy) < energy per job at 27 MHz, with the switch's energy counted once per burst, not per job; and never if the switch's time would make a frame late. Expected outcome, to be checked rather than assumed: the UI image stays at 27 MHz (its jobs are short and the regulator wait is long), and only sustained work (video decode) runs at 150 MHz. Recorded in `hardware.md` once measured.

### Idle sleep: the unlit clock face (stage 5i)

**The lever.** The everyday image (`hello_dma`) is built with `BCMFW_IDLE_SLEEP=0`: whenever the BCM is powered, its core busy-polls the command word on the 27 MHz crystal. The BCM stays powered while the LCD is on, and the shell keeps the LCD on for about 90 s after the backlight goes off: the 7G's unlit clock face (owner-approved in 8a, #51), updated about once a minute. The battery day (2026-10-12) had the LCD on with the backlight off for 18% of its on-battery samples (133 of 736 in `power.csv`). Apple's `vmcs` sleeps on a 10 ms tick between its work. Sleeping our core keeps the feature and takes away the spin.

**The image.** `hello_dma_idle` (`bcmfw/mkdevtest.py`) is `hello_dma` with `BCMFW_IDLE_SLEEP=1` and `BCMFW_IDLE_DOORBELL=1`. Between commands the core sleeps (`ei; sleep; di`). It wakes on:
- **timer compare 1, every 1 ms** (source 17; `bringup.md` item 4). A command waits at most one tick. That's under 1 ms against `lcd-video.c`'s 10 ms tick and a 14.2 ms frame, so no visible cost.
- **the host doorbell, if it is one** (source 23 [M]; acknowledged at `0x10003800` as Apple's handler does). Rockbox already writes `BCM_CONTROL = 0x31` after every command word. If that raises source 23, a command wakes the core at once, and the tick could later be 10 ms (as Apple's) or more for free. Nobody knows which host write raises it; stage 5i's count answers that.

The image counts what it does: `0x140` ticks, `0x144` doorbells, `0x148` sleeps, `0x14C` sleeps after which r0–r5 were not what they were before the sleep (the interrupt return checked on every wake). These share the clock record's words, so no image has both. The busy-poll images are unchanged (same hashes).

**Stage 5i** (devtest, gated like 5b on a stage 5 flag naming `hello_dma`; never taken by the boot path by itself) measures:
1. **Latency, both images:** 60 no-op commands (SELFTEST) and 60 clock-sized `UPDATERECT`s, each after a 12 ms gap so the core is asleep, timed kick to done by a tight poll. Pass: the idle image's slowest rect within the busy image's slowest plus a tick plus 2 ms.
2. **The unlit face, idle image:** 60 s with the panel on and the backlight off, a clock `UPDATERECT` every 10 s. Pass: ticks within 10% of the time held, no sleep with r0–r5 changed, no update later than the busy image's slowest plus a tick plus 2 ms. Power-log marks `s5i_hold` and `s5i_end` bound the hold.
3. **Two sleep/wake cycles** on the idle image.

The doorbell verdict is logged either way: never seen, seen for every command, or for some only. The pass flag `bcmpass5i.txt` names `hello_dma_idle`.

**Power A/B (after 5i passes).** The saving is the core's spin while the BCM is up and idle. Voltage slope can't see it over minutes (`offload.md` §6), so the meter is the reading:
- **Set-up:** a full battery; the USB-C meter (1 mA or better) between a charge-only supply and the iPod; music stopped; the developer power log at 5 s.
- **Phases:** the unlit face held for 120 s on `hello_dma`, then on `hello_dma_idle`, A/B ×3. Each phase is bounded by `powerlog_mark()` (`s5i_A1`, `s5i_B1` … `s5i_end`; `docs/hardware.md`, "Rundowns and A/B tests"), and the owner notes the meter's mean per phase.
- **Second set:** the same with the backlight on and the main menu idle, the everyday case between drifts.
- **Decision:** if B draws measurably less with no late frames, `hello_dma_idle` becomes the boot path's DMA image: its own stage 5 run, then `bcmfw/verified.txt`, so the boot path's flag names it. If the doorbell fires, the tick goes to 10 ms in a follow-up image, with the same A/B.
- **Without a meter:** a coarse day-long A/B with the boot path on each image, each on a similar usage day, read from `power.csv` per powered-on hour (only a large difference shows).

## Device checks

Added by this work, for the device runbook (`bringup.md`):

1. **SDRAM set-up:** the sequence completes, the geometry probe passes (stage 6's `stage 6 SDRAM:` lines say on which attempt and whether `+0x20` bit 15 was seen), and data written through the host port at `0xC0000000` reads back (`6s readback`) and decodes. Session 9 failed here ("The memory map", above); dev10 is the fix. Refresh: run a 1 MB pattern for a minute at the start-up clock with `BCMFW_SDRAM_REFRESH` = 210; then try Apple's 1,171 only after the PLL switch.
2. **Vector forms** the display kernels add (now in the timing bench, 33 vector checks with session 4's diagnostics): `vmulm` D16 A8 by a signed immediate with REP (`MLMI`), `vlsr` D8 A8 (`LSR8`), `vadd` D16 A16 + B8 with REP (`AD68`); data ranges: 16-bit products that wrap (`WRAP`, the R5 << 11 packing) and `vmulhrs #0x1000` over −4,429…12,000 (`CLRQ`). The bench also gained the intra kernels' four forms (`ADRG`, `MVNZ`, `MVRP`, `MH8I`).
3. **Vector memory to and from SDRAM:** `vld8`/`vldp8`/`vldq8` from `0xFE……` addresses (the conversion reads the DPB directly), `vld16` with a stride from SDRAM (the scaler), and whether the VPU sees SDRAM with the cache off at all. If it doesn't, the fallback is DMA-staging rows into SRAM (`hostproto.md` §7's plan).
4. **`vst16` with a stride** into SRAM at 32-byte alignment (the RGB565 rows and the chroma scratch), all from `d00` since device session 4: `vst16 d00` with REP 16 and stride 32 (the scratch), and with REP 8, a register stride and an offset of +32 (the RGB565 halves); the box scaler's `vst8 d00` with REP 16/8, and a `vst16` followed at once by `vld8` of the same bytes (the chroma repeat goes through memory: any write buffer must be ordered with the next load).
5. **DMA overlap:** a panel transfer running while the core decodes (SDRAM and SRAM traffic from the core during the transfer), a 2-D transfer for a sub-rectangle at an odd x (letterboxed widths), and the timeout path re-sending by CPU.
6. **Panel windows** at arbitrary x/y for every panel ID (the letterbox rectangle); the model follows our readings ([M]/[L] in `bringup.md`).
7. **Timing:** the decode's real cycles with the DPB in SDRAM (cache off), and the conversion's (77 instructions, about 800 vector rows per 512 pixels).

## Next steps

1. **On the device:** the checks above, then a real clip end to end.
2. **Scheduling:** `CLOCK`, `PAUSE`, `SEEK` and a display queue (converted pictures in SDRAM, shown at their PTS), which also lets the decoder yield between macroblock rows.
3. **Power:** sleep between pictures (race to idle); the PLL per session is in (dev9, "The 150 MHz clock"), behind stages 4b and 4c.
4. **ProkBox side:** the PP driver (`HAVE_BCMFW`), the MP4 demuxer feeding it, and the player UI (`player.md` on `p14-video-plan`). The display path comes first ("The everyday path" above).
