# Audio spike: can the BCM2722 play a video's soundtrack?

Issue #60, phase 14. A desk study: no device access and no firmware built for this spike. It answers whether the VideoCore II chip can decode and play a video's sound, how that sound could reach the headphones, and what the device stage 8 should test. The hardware facts are in the reference manual, [`audio.md`](../../bcm2722-reference/audio.md), and the open items are #34–#38 in [`open-questions.md`](../../bcm2722-reference/open-questions.md).

Tags as in the reference manual: **[silicon]** measured on our unit, **[apple]** read from Apple's code, **[rockbox]** Rockbox's source, **[hypothesis]** our inference, **[ours]** our design. Addresses `@xxxx` are in `vmcs.bin` 1.3; `host 0x1xxx_xxxx` are in `osos` 1.3.

## Decision summary

| Path | What it means | Verdict | Confidence |
|---|---|---|---|
| **A. The chip plays the sound** | The BCM decodes the soundtrack and drives its own audio pins straight into the WM8758 | **Go to a device test (8a), not yet to code.** Apple's firmware does exactly this; whether our unit's wiring lets us do it safely only the device can say | Apple's design: high. Wiring: medium |
| **B. The chip decodes, the PP plays** | The BCM decodes into a PCM ring in SDRAM; the PP reads it back over the host port into Rockbox's mixer | **Go (8b)**, as the integration path that keeps today's clock, volume and mixer. Costs the PP about 2–5% of 30 MHz | Medium (port read rate not measured) |
| **C. Today: the PP decodes** | Rockbox's AAC codec on the PP, the BCM shows pictures only | **Keep** as the fallback and the A/B reference (8c) | Proven [silicon S27–S30] |

The AAC decoder on the chip, needed by A and B, is a **port of Rockbox's fixed-point libfaad**, about 8–14 days (§7). **M1 and M2 are done (2026-10-13):** it builds unmodified with vc2cc and is bit-exact in vc2sim against the host build on 1,917 AUs of 9 clips. It needs about 0.4 M instructions per stereo AU (about 2.9 ms at 150 MHz, IPC 1). See §7, "M1 and M2 results". It fits beside the H.264 decoder in time and memory with room to spare on 320×240 material.

## 1. The output path

### What Apple's chip firmware does [apple]

- **The player plays audio.** `MPLAYER.VLL` imports all eight `audio_play*` entry points of `vmcs.bin` (`audio_playopen` @48AA, `audio_play` @450A, `audio_playstart` @49B4, `audio_playpause` @49A4, `audio_playclose` @47C8, `audio_playgetclock` @481E, `audio_playgetendclock` @485C, `audio_playgetlatency` @4890). It loads a decoder by `get_aud_dec_func_table`, which `AACDEC.VLL` (Ittiam's AAC decoder, `AACDecode_Ittiam`) exports.
- **The output is a block at `0x1000_4800`.** The audio driver writes it, and it is fed by DMA channel 5 into the FIFO at `+0x04`. Start-up (@13900) writes `+0x00 = 0x6001`, then `+0x10 = 1` and `+0x14 = 0xA508_0010`, then `+0x00 = 0`. Opening at a rate (@14020) writes a divider to `+0x14` (`0xA508_0000 | ⌊6,144,000/fs⌋/4`), sets bit 0 and polls bit 23 for up to 65 ms. Starting ORs `0x6209` into `+0x00` (@138E0), and closing writes 0 (@13954).
- **DMA:** channel 5's config is `0x0607_6002` (DREQ 7). Its 32-entry ring of control blocks starts with a 16-byte silence block that links to itself, so an underrun plays silence. Data blocks carry transfer information `0x1000_A041` (destination paced, 4-byte destination, 16-byte source bursts).
- **Clocks:** opening asks Apple's clock manager (@52B6) for 24.576 MHz (id 2) and 2.048 MHz (id 3) (@13C32). Which registers those reach has not been traced (open question #36).
- **Pins:** the host command `audio_enable 1` (string @BE80, handler @136A6) sets **GPIO 1, 2 and 3 to function 2**. `audio_enable 0` sets GPIO 1 back to a plain output.
- **No codec control:** the routine that would write a DAC's registers (@13FC4) is an empty return, and the codec-ID read (@1411E) returns a constant. The chip never touches the WM8758; the host keeps volume. The host command `audio_control` (@136A2) is a no-op returning 1. `univ_audiopresent` (@138F8) returns 1, and every record entry is a stub.

### What Apple's ARM firmware does [apple]

- **Audio follows the player.** `osos` sends `audio_enable %d` (string at host `0x101F_9148`, sender host `0x101F_90F4`): **1** before `mp_play`/`mp_selectplay` (host `0x101F_8EAC`, `0x101F_A3DC`), **0** on pause and stop (host `0x101F_91F0`, `0x101F_A2FC`, `0x101F_A56C`, `0x101F_A5D8`).
- **The player is asked about its audio.** `osos` never sends `noaudio`. Its debug dump queries `mp_get_stats audio` beside `mp_get_stats video` (host `0x101F_9754`, header "Audio" at host `0x101F_A16C`).
- **The file goes to the chip whole.** The player is started as `mp_selectplay passthru:%s %d`: the chip pulls the file's bytes through `PASSTHRUHANDLER` and demuxes it itself.

**Reading [hypothesis, strong]:** on the stock 5G/5.5G, the BCM decodes the video's AAC and plays it out of GPIO 1–3. The PP's job during video is to serve file blocks and set the WM8758's volume.

**Traced since (§8):** `osos` touches nothing on the PP5022's own I²S, pin groups or GPIO around `audio_enable 1` or `0`, and no register that releases the PP's data pin is known. The evidence points to separate data lines (GPIO 1 on a net of its own).

### Where the pins go

- **[rockbox]** The WM8758 is the I²S **clock master** (`wm8758.c`: `CLKCTRL_MS`) with a single audio interface, and the PP5022's I²S is a slave (`i2s-pp.c`).
- **[hypothesis, medium]** BCM GPIO 1–3 are wired to that same interface. The BCM would take the WM8758's bit and frame clocks and drive the data line while the PP's I²S is idle. Three things fit this:
  1. Apple's divider is wrong for 44.1 kHz (2.4% fast), which matters only if the block is not the clock master.
  2. The BCM never programs the codec.
  3. `audio_enable` is sent for LCD playback too, not only for TV out.
- **The alternative,** a path only to TV out or the line/dock output, is weaker (point 3).
- **"Nowhere"** contradicts Apple sending `audio_enable` at all.
- **Settled only on the device,** read-only first (stage 8a-0, §6).

**Confidence:** a BCM audio output exists and Apple used it [apple, high]. It goes to the WM8758 [hypothesis, medium]. The PP-side switching is unknown.

## 2. Path B: reading PCM back to the PP

- **Rate:** 44.1 kHz × 2 channels × 16 bits = **176.4 KB/s**, which is 44,100 32-bit words a second.
- **How Apple reads** [apple, host `0x1028_7A6C`]: it sets the read address once (two halfwords to `RD_ADDR`), then for each word polls `CONTROL` bit 4 and reads `DATA` twice (the address auto-increments). That is about 8 instructions plus 3 port accesses per word.
- **Write cost on the same port:** about 0.125 µs per byte from an unboosted PP (0.25 µs per pixel) [silicon S7], which is about 0.5 µs per word.
- **Read cost:** a read adds the poll and the chip's read latency, so **0.5–1.0 µs per word** [hypothesis].
- **Total:** **22–44 ms of PP time per second of audio, 2.2–4.4% of a 30 MHz PP**, or about 0.7–1.3 MHz-equivalent. At 48 kHz, scale by 1.09.
- **Bursts:** read in 4–8 KB bursts (one address set-up per burst) on the video thread's existing wake-ups. That is 23–46 ms of audio per burst, so 20–40 bursts a second, cheaper than per-frame round trips.
- **PP DMA:** the PP5022's DMA can move port data. Apple uses it for bulk writes (host `0x1028_6FB4`, 64 KB chunks); reads by DMA have not been seen in Apple's code or tried by us [hypothesis]. If it works, the CPU cost drops to the set-up.
- **Hazard:** torn words (`quirks.md` #23) affect only words the chip rewrites during the read. The ring's PCM is written once before the host reads it. The ring indices need the `HP_ENC` check, as today [ours].
- **Against decoding on the PP:**
  - Rockbox's AAC-LC 128 kbit/s decode is "cheap at 30 MHz" (`docs/hardware.md`). We estimate it at **10–20 MHz-equivalent** of the PP's 30 MHz [hypothesis; not measured in this repo. Rockbox's `test_codec` plugin measures it on the device: stage 8c].
  - So the readback costs about **one tenth to one twentieth** of decoding on the PP.
- **Verdict:** path B saves the PP most of its audio work at the price of a small, predictable port load. It changes nothing in Rockbox's mixer, volume, clock or sync.

## 3. Cost on the chip

**AAC-LC, 128 kbit/s stereo, one 1024-sample frame** (23.2 ms at 44.1 kHz). Our estimate for libfaad's fixed-point C compiled by vc2cc [hypothesis; first measured in vc2sim, §7]:

| Stage | Work per frame (stereo) | Instructions |
|---|---|---|
| Bitstream and Huffman | about 2,048 coefficients, codebook walks | 80–150 k |
| Inverse quantisation (`BIG_IQ_TABLE`) and scale factors | 2,048 table lookups and multiplies | 15–30 k |
| M/S, intensity, TNS, PNS (when present) | per band | 10–40 k |
| IMDCT: a 2048-point via a 512-point complex FFT, per channel | about 2,300 butterflies plus pre/post twiddle, ×2 | 110–160 k |
| Windowing, overlap-add, output to 16-bit | 4,096 multiply-adds, 2,048 stores | 25–40 k |
| **Total** | | **240–420 k** |

- **Instructions to cycles:** VC II runs about 1 instruction per cycle on dependent chains and up to 1.9 on independent ones [silicon S6, S7].
- **The 64-bit multiplies:** libfaad's `MUL_R`/`MUL_C`/`MUL_F` are 32×32→64 multiplies. They compile to `mul` + `mulhd.ss` plus a combine (`isa-scalar.md`), several instructions where ARM needs one `smull`. Allowing for that: **0.4–0.8 Mcycles per frame, 2.7–5.3 ms at 150 MHz**.
- **Against the per-picture budget:**

| | ms per picture slot |
|---|---|
| Frame period at 30 fps | 33.3 |
| Real decode + convert + push today, mean (S29, S30) | 14.8–14.9 (max 46) |
| Audio at 30 fps (1.44 AAC frames per picture) | 3.8–7.7 |
| **Mean headroom left** | **about 11–15** |

- **Peaks:** a 46 ms picture already overruns its slot today. Audio must therefore never wait on video.
  - The PCM ring must hold at least **250 ms** (11 frames, 44 KB) ahead of the DMA (path A) or of the host's reads (path B).
  - The decoder fills the ring between pictures, with priority to audio when the ring falls below about 100 ms.
  - At 24 fps (41.7 ms per slot) there is more room.
- **Below full clock (8b):** at 100 MHz the audio costs 1.5× (4–8 ms per frame), still inside the mean headroom of 320×240 material.
- **Vectors:** if vc2sim shows more than 5 ms per frame, vectorise the IMDCT's FFT butterflies and the windowing first (16 lanes, 16-bit; they need the 32-bit-precision care that libfaad's `REAL_BITS` asks for). The H.264 work shows the cost of getting vector kernels right on silicon (`quirks.md` #8, #16–#20), so do this only if measured necessary.
- **Memory beside the decoder** [ours, `bcmfw/video/video.h`, `config.h`]:

| Region | Use today | Free for audio |
|---|---|---|
| SRAM `0x20000–0xCC000` (arena, 688 KB) | decoder context and frames at 320×240: about 41 KB + 5 × 113 KB | about 80 KB |
| SRAM `0x12B000–0x140000` (above the picture at `0x105800`, 153,600 B) | unused in the video image (the drift image's cover slot overlaps it; that is a different image) | 84 KB |
| SDRAM `0xFE34_0000–0xFE3F_FFFF` (after `VIDEO_WRAP`) | unused | 768 KB |

- **Placement:**
  - Hot state (spectral buffers, overlap, IMDCT work buffer, about 40–50 KB) goes in SRAM.
  - The 32 KB inverse-quantisation table and the bulk of the element state go in SDRAM if SRAM is short.
  - The PCM ring (0.5 s: 88 KB) and an access-unit ring (64 KB) go in SDRAM.
  - Code (32–42 KB, §7) raises the image from 93 KB. `BCMFW_VIDEO_ARENA` (now `0x20000`) moves up by about 48 KB, taken from the arena's 80 KB slack.
- **640×480:** frames live in SDRAM, so the SRAM arena has room there; the time budget is the limit.

## 4. Sync

- **Path A: the chip becomes the A/V master.**
  - Its clock is the audio DMA's progress through the PCM. That is how Apple's `audio_playgetclock` works: it reads channel 5's current block and `+0x24` [apple].
  - Pictures are timed against that clock on the chip. The host's `id3->elapsed`-based clock, the pacing of `FEED` records against it, and the catch-up rules in `host-driver.md` §5 [ours] give way to a clock word the host reads (`HP_ENC`-encoded) for the progress bar and resume.
  - **The host keeps:**
    - **Volume:** the WM8758, over the PP's I²C. Apple's chip never writes the codec either [apple].
    - **Pause:** stop the block (`+0x00 = 0`), keep the ring.
    - **Seek:** flush both rings and restart at a key frame, audio and video together.
    - **The sample rate:** the WM8758 stays the clock master, so the host must set it to the soundtrack's rate before `audio_enable`.
    - **The PP's own I²S:** it must be quiet while the chip drives (open question #35). Rockbox's playback must be stopped, not merely paused, during a path-A session, or the two would fight over the data line.
  - Speed changes (timestretch) are not available on path A.
- **Path B: nothing changes.**
  - The PCM enters Rockbox's mixer like a codec's output, so `elapsed`, pause, seek, volume, timestretch and the existing host-paced video sync all keep working.
  - The chip's only new duty is to stay ahead of the host's reads, which the 250 ms ring covers.

## 5. Power

- **Path A or B, the PP's side:** the PP stops decoding AAC.
  - Today it spends roughly 10–20 MHz-equivalent on the soundtrack [hypothesis], on top of demuxing and feeding the chip.
  - Path A removes all of it. Path B removes most of it and adds about 1 MHz-equivalent of port reads.
  - Either way the PP stays unboosted for more of the playback and sleeps more between its 10 ms ticks.
- **The chip's side:** it is already powered and at 150 MHz for the whole video. The audio adds 4–8 ms of work per 33 ms.
  - If our image sleeps when idle (it waits on the host; `interrupts-timers-gpio.md`), that is roughly 12–24% more active time on a core whose draw has never been measured (open question #12).
  - If it polls, it adds nothing.
- **Estimate:** a small net saving, likely in the low single-digit percent of playback time [hypothesis].
  - The ARM7TDMI at 30 MHz doing integer DSP is probably less efficient per useful operation than a 150 MHz VideoCore built for media.
  - The saving is not large enough to justify path A's sync rewrite on power grounds alone.
- **How to measure:** the battery log's 30-minute A/B (`docs/hardware.md`, "A/B"), path C against B, with `boost_pct` matched.

## 6. Device stage 8 (the owner judges by ear)

All sub-stages run with the volume preset low (WM8758 at −30 dB) and start from the video image already up.

### 8a-0: listen (read-only, no output driven)

**Our image:**

1. Sets GPIO 1–3 to inputs (`0x1000_2800` bits `[7:2]` = 0).
2. While the **host plays a 1 kHz tone through Rockbox as usual**, samples the level register `0x1000_2820` bits 1–3 in a tight loop for 100 ms. It counts edges per pin against the 1 MHz counter.
3. Reports each pin's toggle rate:
   - fs (44.1 kHz) means a frame clock;
   - 32–64 × fs means a bit clock;
   - irregular toggling means data;
   - 0 means not connected to the I²S bus.
4. The host then **stops its I²S** (pcm stop, `IISCONFIG` transmit disabled) but keeps the WM8758 awake (DAC standby held off).
5. The image applies pull-up, then pull-down (`+0x60` mode, `+0x64`/`+0x68` strobe with Apple's 150-iteration waits, @13F7A) to the data candidate, and reads it.
   - If it follows the pull, the line floats and is safe to drive.
   - If it holds a level, something drives it: **stop, do not run 8a-1**.

**Logs:** toggle counts, the pull results and the audio block's reset value (`0x1000_4800 +0x00/+0x14`). This answers questions #34 and #35 with nothing driven.

### 8a-1: tone (only if 8a-0 found a floating data line)

**Init sequence, Apple's, in order:**

1. GPIO 1–3 to function 2.
2. Block start-up: `0x1000_4800 = 0x6001`, `+0x10 = 1`, `+0x14 = 0xA508_0010`, `+0x00 = 0`.
3. Set the rate: `+0x14 = 0xA508_0000 | 34` (44.1 kHz), `+0x00 |= 1`, then wait for `+0x00` bit 23 for up to 65 ms. Log a timeout and continue: as a slave the block may never report it.
4. DMA channel 5:
   - `+0x00 = 0x0607_6002`, `+0x04 = 0xFFFF_FFFF`;
   - a self-linked 16-byte silence block (`ti 0x1000_A040`), then a 4,410-frame PCM block, looped, `ti 0x1000_A041`;
   - start by writing the first block's address to `+0x38`.
5. `0x1000_4800 |= 0x6209`.

**The tone:**

- The image makes it itself (no file): **440 Hz for 3 s, then 880 Hz for 3 s**, at −20 dBFS.
- Then 440 Hz left only, then 880 Hz right only, to settle channel order and format (#37).

**The owner reports what they hear:**

- the right pitches;
- an octave or fifth off (a rate or word-length error);
- noise (format or bit order);
- silence.

**Stop:** `+0x00 = 0`, channel 5 `+0x00 = 0`, GPIO 1 to function 1. Then the host restarts its I²S and a normal track must play. That last step is the "nothing broken" check.

**Logs:** bit 23, channel 5's `+0x20/+0x24/+0x2C` at intervals (#38), and any DMA status.

#### dev45: the clock pins (after session 36c)

Session 36c's four tones never played: dev42 left GPIO 2 and 3 as inputs, and the block never reported ready (control word `0x0013_0001`). Session 35, with all three pins on function 2, reported ready (`0x0092_0001`) and played. Apple's driver has no master/slave bit (it writes only `0x6001`, `0x6209`, `+0x10 = 1` and the divider), so the block is most likely a clock slave whose function-2 pins 2 and 3 are its clock inputs; S35's screech was the uncapped level or the format (`docs/bcm2722-reference/audio.md` §5, "Clock direction").

dev45's tone:
- listens to GPIO 2 and 3 for 3 ms and muxes nothing unless GPIO 3 shows the codec's frame clock;
- then puts GPIO 2 and 3 on function 2, waits for bit 23, and only then puts GPIO 1 (data) on function 2 and plays;
- reports the control word before, after the rate and while playing, `GPIO_FSEL0` and the edge counts.

Stage 8a-1 keeps the codec awake (`dac_standby_allow(false)`, put back after), since its standby stops the clocks. Stage 8a-0 waits until PCM really runs (mixer playing, elapsed moving) before its "playing" probe, and 8a-1 unlocks only if that probe saw the clocks on GPIO 2 and 3 as well as GPIO 1 floating in all three states.

### 8b: PCM from a file, while the video plays at 30 fps (owner's choice for dev39)

- **The clip:** a 30 fps clip whose soundtrack is **16-bit PCM**, so this stage tests transport, timing and the two output paths without a decoder.
- **Feeding:** the host posts the PCM in packets beside the NALs (a second SDRAM ring at `0xFE34_0000`). The chip copies them into its 0.5 s PCM ring between pictures, at **below full clock (100 MHz)** to leave the margin of a real decoder.
- **Path B (always):** the host reads the PCM back in 4 KB bursts into a Rockbox mixer channel. The owner listens for clicks or gaps, and the log records:
  - read µs per KB;
  - ring fill (minimum);
  - PP busy %;
  - picture lateness against stage 7.
- **Path A (only if 8a-1 played cleanly):** the same session with the chip playing the ring through the block, and the host's PCM stopped. The log records the chip's audio clock against its picture clock.

### 8c: A/B with today's path

The same clip with AAC (path C: Rockbox decodes on the PP), back to back with 8b.

- **Rockbox's `test_codec`** on the AAC file, unboosted and boosted, gives the PP's real AAC cost (fills the [hypothesis] in §2 and §5).
- **The owner judges by ear:** the same sound, lip sync on the flash/beep clip, no clicks.
- **The battery A/B** (30 min each) follows if 8b holds.

### Test files (made with ffmpeg; synthetic, no owner media)

```sh
# 8a: no file (the image makes the tones). Reference WAVs for the owner to compare on the PC:
ffmpeg -f lavfi -i "sine=f=440:d=3" -f lavfi -i "sine=f=880:d=3" \
  -filter_complex "[0][1]concat=n=2:v=0:a=1,volume=-20dB" -ar 44100 -ac 2 ref_440_880.wav

# 8b: 30 fps Baseline video, 16-bit PCM soundtrack; a white flash and a 20 ms 1 kHz beep each second
ffmpeg -f lavfi -i "testsrc2=s=320x240:r=30,drawbox=c=white:t=fill:enable='lt(mod(t,1),0.034)'" \
  -f lavfi -i "aevalsrc='if(lt(mod(t,1),0.02),0.3*sin(2*PI*1000*t),0.1*sin(2*PI*440*t))':s=44100:d=30" \
  -t 30 -c:v libx264 -profile:v baseline -level 3.0 -pix_fmt yuv420p -g 30 \
  -c:a pcm_s16le -ac 2 stage8b_pcm.mov
# 8c: the same pictures with AAC-LC 128 kbit/s
ffmpeg -i stage8b_pcm.mov -c:v copy -c:a aac -b:a 128k -movflags +faststart stage8c_aac.m4v
```

Our demuxer (`tests/video/mp4demux.py`, the player's MP4 reader) has so far been tested only with `mp4a` tracks. The `.mov` PCM track (`sowt`/`lpcm` sample entry) needs a check in the host tests first. If it is refused, 8b carries the PCM as a sidecar `stage8b_pcm.wav` cut from the same `aevalsrc`, which tests the same transport.

## 7. The AAC decoder on the chip: a port of libfaad

The owner chose a port, after 8a/8b, not a decoder from scratch.

### Source and scope

- **Rockbox's `lib/rbcodec/codecs/libfaad`** (GPL C, `FIXED_POINT`), built with `LC_ONLY_DECODER`. That removes MAIN, SSR, LTP, LD and error resilience.
- **Without `CODEC_AAC_SBR_DEC`/`PS_DEC`:** no SBR or PS.
- **The IMDCT:** `ff_imdct_calc` from `lib/rbcodec/codecs/lib/mdct.c` + `fft-ffmpeg.c`, C paths only (the ARM assembly headers excluded).

### Toolchain

`bcmfw/build.py` drives vc2cc over vc4-elf-gcc, the same path that builds the all-C H.264 decoder bit-exact on silicon (`compiler.md`). What vc2cc already avoids carries over:

- the offset `ldsh` form (`quirks.md` #16);
- r7 as a vector base or B operand (#8, #19, vector code only);
- misaligned accesses (#2).

`--conservative` covers pc-relative globals. 64-bit arithmetic goes through vc2libgcc (360 of 360 cases pass, `compiler.md`). `fixed.h`'s generic C `MUL_*` macros apply. One small vc2cc-safe inline (`mul` + `mulhd.ss` + shift) for `MUL_R`/`MUL_C`/`MUL_F` is the first optimisation, if needed.

### The adapter (about 200 lines, new, ours)

- **In:** raw AAC access units from an SDRAM ring that the host fills beside the NALs. The `esds` AudioSpecificConfig comes once at `OPEN`.
- **Out:** interleaved 16-bit PCM into the PCM ring.
- **No OS:**
  - libfaad's `faad_malloc` maps to a bump allocator over a static arena;
  - no stdio (`error.c` strings dropped or kept as numbers);
  - `FAAD_STATIC_ALLOC` already puts the big buffers in statics, which the linker map places.

### Size

ARM figures are measured from this repo's `build/fw` (`arm-none-eabi-size -A` on `libfaad.a` and `libcodec.a`). The VC II figures are estimates.

| | ARM (LC-only subset) | VC II estimate | Placement |
|---|---|---|---|
| Code: libfaad objects without `ps_*`/`sbr_*` (`.text` + `.icode`) | 30.5 KB | | |
| Code: `mdct` + `fft-ffmpeg` | about 4.5 KB | | |
| **Code total** | **35 KB** | **32–42 KB** (VC IV code is about ARM's size or smaller; vc2cc adds 2.6% plus 2 bytes per global access) | SRAM, in the image |
| Tables: inverse quantisation (`specrec` `.rodata`, `BIG_IQ_TABLE`) | 33.0 KB | same | SDRAM (or the 1 KB small table at some cost) |
| Tables: Huffman 8.3 KB, windows 9.2 KB, MDCT twiddles 16.4 KB, misc 1 KB | 35 KB | same | SRAM, or SDRAM for the cold half |
| State: element structs (`sce`/`cpe`, 2 × 20.4 KB) | 41 KB | same, can drop to one | SDRAM / SRAM |
| State: time buffers and overlap 16 KB, spectral 16 KB, IMDCT work 16 KB, bit buffer 2 KB | 50 KB | same | SRAM (hot) |
| Rings: PCM 0.5 s (88 KB), access units (64 KB) | — | 152 KB | SDRAM |

For comparison, Apple's Ittiam decoder is 36.6 KB of code and 7 KB of data (`AACDEC.VLL` sections) [apple]. Our estimate is in its range.

### Cycles

§3 estimates **0.4–0.8 Mcycles per frame** (2.7–5.3 ms at 150 MHz) in scalar C, against about 11–15 ms of mean headroom per 30 fps picture. Vectorise the IMDCT/FFT or the windowing only if vc2sim shows more than 5 ms per frame.

### Milestones and days

[ours, estimate]

| # | Milestone | Done when | Days |
|---|---|---|---|
| 1 | **Builds in vc2sim** | libfaad LC-only + C IMDCT + adapter compile with `bcmfw/build.py`; a vc2sim harness decodes a raw-AU file | 2–3 |
| 2 | **Bit-exact** | PCM from vc2sim equals the host build of the same sources (warble/`tests/` harness) on the test clips (8c's AAC track, a 48 kHz clip, a mono clip, a short-window-heavy clip such as castanets) | 1–2 |
| 3 | **Measured, optimised if needed** | cycles per frame from vc2sim's counters at 150 and 100 MHz; the `MUL_*` inline; vector IMDCT only if more than 5 ms per frame | 1–3 |
| 4 | **On silicon** | a dev image decodes a clip's AUs from SDRAM and returns a PCM hash equal to the host's (one device session); then 8b's path with AAC instead of PCM | 1–2 |
| 5 | **In the player** | the host feeds AUs beside the NALs; path B into the mixer (or A if 8a passed); seek, pause and sleep/wake; the guard words cover the audio rings | 3–4 |
| | **Total** | | **8–14** |

### M1 and M2 results (2026-10-13): builds, runs, bit-exact

**Both done, in vc2sim [ours].** Code in `bcmfw/aac/`; tests: `bcmfw/aac/test/run.sh` (host reference, then vc2sim; `SIM_AUS=0` decodes every AU).

**How it is built.**
- The libfaad files and `mdct.c`/`fft-ffmpeg.c`/`mdct_lookup.c` are compiled **from their upstream location, unmodified** (list: `bcmfw/aac/SOURCES`). `aac_config.h` is force-included (`-include`): it stands in for Rockbox's `config.h` and `codeclib.h` (no `CPU_ARM`, so every `MUL_*`/`MULT31` is the generic C one). `bcmfw/aac/sys/` holds stand-ins for the libc headers libfaad includes.
- `FIXED_POINT`, `LC_ONLY_DECODER`, no SBR/PS. The same flags go to vc2cc and to the host compiler (`-funsigned-char` on the host, since `char` is unsigned on VC II).
- **No allocator:** Rockbox's libfaad keeps all its state in statics (`FAAD_STATIC_ALLOC`), and in the LC build nothing calls `faad_malloc`. So no arena is needed, and the arena high-water mark is 0. A future call would fail to link.
- One alignment fix: `IBSS_ATTR` is `aligned(4)`. The bit reader reads its byte buffer (`bits.c` `static_buffer`) as words, and VC II faults on a misaligned word (`quirks.md` #2).
- The adapter `aac.c` (about 120 lines) has these calls:
  - `aac_open(asc)`;
  - `aac_decode(au) → 1024` interleaved 16-bit stereo frames (mono goes to both channels; 0 frames for the priming AU, as libfaad reports it);
  - `aac_reset()`, which clears the overlap and primes again (`NeAACDecPostSeekReset` only matters for SBR).
- The 16-bit conversion is Rockbox's undithered output stage at codec depth 29: `(x + 2¹³) >> 14`, clipped.
- **64-bit arithmetic works as it stands:** the generic `MUL_R/C/F` compile to `mul` + `mulhd.ss`, and `DIV_R`'s `int64` division links libgcc's `__divdi3` (1,268 bytes). No float survives (`--no-float` passes). No blocker.

**Bit-exactness (M2).** For each AU, vc2sim's output is compared byte for byte with the host build of the same sources: the 16-bit PCM, and also libfaad's 32-bit `time_out` for both channels.

| Clip (5 s, synthetic) | Encoder | AUs | vc2sim vs host |
|---|---|---|---|
| `sine_st128`: 440/660 Hz, 44.1 kHz stereo, 128 kbit/s | ffmpeg | 217 | bit-exact |
| `pink_st128`: independent pink noise per channel | ffmpeg | 217 | bit-exact |
| `white_mono64`: white noise, mono, 64 kbit/s (PNS) | ffmpeg | 217 | bit-exact |
| `chirp_st128`: swept harmonics, tremolo | ffmpeg | 217 | bit-exact |
| `clicks_st48k`: castanets-like bursts, **48 kHz** (48 short-window channel-frames) | ffmpeg | 236 | bit-exact |
| `stage8c_st128`: 8c's 440 Hz bed + 1 kHz beeps | ffmpeg | 217 | bit-exact |
| `chirp_at128`: music-like with transients (50 short-window channel-frames) | Apple AudioToolbox | 219 | bit-exact |
| `voice_at_mono32k`: mono, **32 kHz** | Apple AudioToolbox | 160 | bit-exact |
| `sine_mono128`: the video tests' soundtrack (`long_ipod.mp4`'s source) | ffmpeg | 217 | bit-exact |

**1,917 of 1,917 AUs are bit-exact.**

The host reference is checked three more ways:
- **Against ffmpeg's float decoder** (`refcheck.py`): the tonal clips agree to 65–79 dB SNR. The PNS/noise clips agree in level to within 0.04 dB (median) per 1024-frame block; their waveforms differ by design, since each decoder makes its own noise. This check caught a wrong output scale during the port.
- **Its sanitizer build** (address, undefined) runs clean and equals the -O2 build.
- **`aac_reset`** matches a fresh open within 1 LSB. The residue is PNS's static noise generator, which nothing restarts.

Rockbox's own ARM build is **not** a bit-exact reference: its `MUL_*` are `smull` asm that round differently (`MUL_F` truncates). It was not compared.

**Sizes** (vc2cc, `--conservative`):

| | Bytes | Of which |
|---|---|---|
| Code (`.text`) | **33,908** | `syntax.c` 8.5 K, `huffman.c` 4.6 K, `filtbank.c` 3.5 K, `fft-ffmpeg.c` 3.4 K, `specrec.c` 3.1 K, `pns.c` 2.1 K, `mdct.c` 1.4 K, `__divdi3` 1.3 K, adapter 0.6 K |
| Tables (`.rodata`) | **64,228** | `iq_table` 32,900, `revtab` 8,192, `sincos_lookup0/1` 8,200, sine and KBD long windows 8,192, `drc_pow2_table` 1,300 |
| Tables (`.data`, non-const in libfaad) | **6,794** | Huffman codebooks |
| State (`.bss`) | **82,562** | `sce` + `cpe` 40,896, `transf_buf`, `s_time_buf_1024` and `s_fb_intermed` 8,192 each, `spec_coef1/2` 4,096 each, bit buffer 2,060 |
| Stack | **724** (high-water mark) | |

These match the §7 estimates: code 33.9 KB against 32–42 KB, tables 71 KB against 68 KB.

**Cycles (M1's measurement; M3 refines it).** vc2sim counts instructions. As for the H.264 decoder (`decoder.md`), cycles ≈ instructions at an IPC of 1, with SDRAM latency unmodelled.

| Clip | Instructions per AU (1024 frames), mean | Worst AU | ms at 150 MHz (mean / worst) | ms at 100 MHz (mean) |
|---|---|---|---|---|
| Stereo 128 kbit/s, ffmpeg (5 clips) | 372–435 k | 451 k | 2.5–2.9 / 3.0 | 3.7–4.4 |
| Stereo 128 kbit/s, AudioToolbox | 486 k | **745 k** | 3.2 / **5.0** | 4.9 |
| Mono (3 clips) | 209–222 k | 458 k | 1.4–1.5 / 3.1 | 2.1–2.2 |

- **Per 30 fps picture** (1.44 AUs): stereo costs about **3.6–4.7 ms of the 33.3 ms slot** at 150 MHz, inside the 11–15 ms mean headroom. The worst single AU (5.0 ms) is a transient-heavy AudioToolbox frame (TNS + short windows); the 250 ms ring absorbs it.
- **No vectorisation is needed by the 5 ms rule:** the mean is under 3.3 ms on every clip.
- **Where the time goes (stereo):**

| Stage | Share |
|---|---|
| IMDCT/FFT | 25–33% |
| Inverse quantisation | 15–19% |
| Windowing and overlap | 13–17% |
| Bitstream and Huffman | 10–13% |
| `memcpy`/`memset` | 10–13% (about 50 k instructions per AU) |
| Stereo, PNS and TNS | 2–21% (TNS dominates the AudioToolbox clip) |
| Output conversion | 5–7% |

- **The cheap M3 wins:**
  - libfaad's per-frame clears and copies (the `memset`/`memcpy` share);
  - a word-unrolled `memset`;
  - the `MUL_*` inline (32.5 M `mulhd.ss` over the 1,917 AUs, about 17 k per AU).

**Instruction kinds** (vc2sim coverage, `hostproto_test.py`'s rule).
- `vc2cc.listing` marks **no** instruction as a form absent from Apple's static code.
- The decoder executes 121 kinds. 11 of them never run in the vc2sim self-test:
  - `mulhd.ss`, `bhi` (16- and 32-bit): **device-verified** (probe v2);
  - `asr rd, rs`, 32-bit `bgt`/`blt`/`ble`/`bls`, indexed `ldsb`: **verified by the timing bench's scalar checks** since S3 (`isa-scalar.md`);
  - **32-bit `bcs` and `bcc` (conditions cs/cc after `cmp`)**: **unproven** (273 k and 143 k executions over the 1,917 AUs).
- **Where the unproven branches come from:**
  - unsigned compares in upstream code: `fft-ffmpeg.c`'s `pass()` loop bound (`w < w_end`, pointers) and `huffman_spectral_data`;
  - the video image has the same two kinds, in `hp_poll` and the SPS/slice-header parsers. It decodes bit-exact on silicon, which is implicit evidence only, not a per-kind check.
  - Avoiding them would mean editing upstream files, so they are recorded for M4 instead.
- **Shift counts of 32:**
  - `specrec.c`'s inverse quantiser executes `asr` by 32 (`exp = −32` in zero, IS and PNS bands). That count is an untested edge case on VC II (`isa-scalar.md`), and vc2sim masks it to 5 bits.
  - Every such shift in the clips is of 0: counted with an instrumented host build, 1.25 M shifts by ≥ 32, none of a nonzero value. So the output does not depend on how the silicon treats the count.
  - The host sanitizer build therefore leaves shift checks off.

### What M3 and M4 (silicon) need

- **M3 (vc2sim, no device):**
  - remove the `memset`/`memcpy` share and inline `MUL_*`, keeping bit-exactness;
  - measure again at both clocks.
  - Optional: move `bcs`/`bcc` out of the hot loops by having vc2cc emit `cmp` + `bhi`/`bls` with swapped operands, or `addcmpb cs/cc`, whichever the self-test proves.
- **M4 (one device session, read-only for the audio pins):**
  1. **Placement:**
     - Code (34 KB) and the hot state (`transf_buf`, `s_time_buf_1024`, `s_fb_intermed`, `spec_coef1/2`, the bit buffer: about 35 KB) go in SRAM, beside the H.264 arena.
     - `iq_table` (33 KB), `revtab`, the window tables and `sce`/`cpe` (41 KB) go in the free SDRAM, outside the stage 8b PCM rings at `0x34_0000–0x3A_0000`.
     - `bcmfw/build.py` needs per-section placement for that (`compiler.md`, "Placement of sections in SDRAM").
  2. **A probe check** of 32-bit `bcs`/`bcc`, taken and not taken, with C as the borrow (`quirks.md` #7). Or the vc2cc rewrite above.
  3. **A dev image** that decodes a clip's AUs from SDRAM and reports the PCM's FNV against the host's (`aac_test` prints it). Then the real IPC: decode µs per AU against the 1 IPC estimate here, with SDRAM tables.

### M3 results (2026-10-13): 14% fewer instructions, still bit-exact

**Done, in vc2sim [ours].** All in the adapter and its config; no upstream file changed.

- **`MUL_*` without the carry** (`aac_config.h`): fixed.h's portable `MUL_R/C/F/Q2` and `_MulHigh` round a 64-bit product, `(a·b + 2^(s−1)) >> s`, which vc4-elf-gcc built as 10 instructions with a `cs` branch for the carry. `aac_config.h` reads fixed.h first and replaces them with the same value from the product's two halves: `hi·2^(32−s) + ((lo >> (s−1)) + 1) >> 1` (exact for 2 ≤ s ≤ 32, modulo 2^32 as fixed.h's cast). 7 instructions, no branch.
- **Copies and clears** (`aac.c`, redirected by `aac_config.h`): word-unrolled `memcpy` (8 words a turn; 8 halfwords a turn for halfword-aligned blocks) and `memset` (16 words a turn). The 10 KB second-`ic_stream` copy of every channel pair is only halfword-aligned, and bcmfw's libc copied it a byte at a time: that was most of the old `memcpy` share. libc share: 10–13% → 5–8%.
- **The 16-bit clip**: one unsigned test for both limits.
- **Bit-exactness:** the host build's output is byte-identical to M2's on all 9 clips (so the multiply rewrite changes nothing), and vc2sim equals the host on **1,917 of 1,917 AUs**.

| Clip | M2 mean / worst (k instr.) | **M3 mean / worst** | ms at 150 MHz (mean) | ms at 100 MHz (mean) |
|---|---|---|---|---|
| `chirp_at128` (AudioToolbox, transients) | 486 / 745 | **414 / 635** | 2.76 | 4.14 |
| `pink_st128` | 435 / 451 | 373 / 387 | 2.48 | 3.73 |
| `sine_st128` | 419 | 345 / 372 | 2.30 | 3.45 |
| `chirp_st128` | 396 | 343 / 369 | 2.28 | 3.43 |
| `stage8c_st128` | 390 | 334 / 375 | 2.23 | 3.34 |
| `clicks_st48k` (48 kHz) | 372 | 322 / 355 | 2.15 | 3.22 |
| mono (`sine_mono128`, `white_mono64`, `voice_at_mono32k`) | 209–222 | 183–196 / 392 | 1.22–1.31 | 1.83–1.96 |
| **All clips** | 348 | **300** (worst AU 635 k = 4.2 ms at 150 MHz) | | |

Stereo at 150 MHz: **2.2–2.8 ms per AU**, 3.1–4.0 ms per 30 fps picture. Where the time goes now (stereo): IMDCT/FFT 30–38%, inverse quantisation 16–20%, windowing 11–14%, bitstream and Huffman 12–16%, libc 6–8%, output 4–6%, TNS up to 21% (AudioToolbox clip). The rest is upstream code; the next step, if silicon's IPC with SDRAM tables asks for one, is a vector IMDCT.

**`bcs`/`bcc`:** not steered. The remaining sites are five `cmp` + 32-bit `bcs`/`bcc` in upstream code (`huffman.c` ×4, `fft-ffmpeg.c` `pass()`); avoiding them needs either upstream edits or a vc2cc peephole across `cmp` and the branch (swap the operands, use `bhi`/`bls`), which also changes the video image's proven code. M4's probe settles them on silicon instead; the peephole is the fallback if it fails.

### M4 (2026-10-13): in the video image, ready for the device [ours]

**What is built** (`p14-aac-m4`): every video image carries the decoder; vc2sim decodes through the protocol byte-exact (`video_test.py`'s AAC row: the probe, then 6 AUs of `sine_st128`, 16/16 checks; the whole suite 215/215). **The device step is stage 9** (`device-session-1.md`, "Session 34 or later"). Not yet run on silicon.

- **Placement** (`bcmfw/build.py` `AAC_FAR`, vc2cc's new far sections `.xdata`/`.sdbss`):
  - SRAM, in the image: the code (33 KB), the hot buffers (`transf_buf`, `s_time_buf_1024`, `s_fb_intermed`, `spec_coef1/2`, the bit buffer, the spectral data: about 41 KB), the twiddles, the Huffman codebooks, the short windows. Image+bss now ends at `0x30034`; the H.264 arena moves from `0x20000` to `0x31000` (620 KB; a 320×240 stream with 4 references needs 617,056 bytes, `build.py` checks it).
  - SDRAM at `0x380000` (`video.h` `VIDEO_AAC_*`, inside the old stage-8b area, which uses only `0x340000–0x368000`): the sine and KBD long windows (8 KB), `iq_table` (32 KB), `revtab` (8 KB), then `cpe` and `sce` (40 KB), to `0x395FB4`. The window ends at `0x39C000`; `0x39C000–0x39FFFF` is the test's I/O.
  - Getting the tables there: `.xdata` rides in the load image after `.data` (the image is 202,180 bytes, from 96,008); `crt0` parks it at the top of the H.264 arena before clearing `.bss`; the first H.264 `OPEN` copies it into SDRAM once SDRAM is up, before the decoder takes the arena, and clears `.sdbss`. `AAC OPEN` checks an FNV-1a of the SDRAM copy (a diagnostic memory test wipes it: `HP_E_STATE`, boot again).
- **Protocol:** `HP_OP_AAC` (`0x0F`, `HP_CAP_AAC`): `OPEN`, `DECODE`, `RESET` with an H.264 session open; `PROBE` without (`hostproto.md`, "AAC"). Decided: a session is required, because SDRAM, the tables and the clock come with it, and M5 decodes beside a video anyway.
- **The probe:** `bcmfw/aac/brprobe.S`, 16 compares, each followed by a far `bcs` and a far `bcc`; vc2sim and the C model agree on `0x9DAD6252`.
- **Host side:** `vplayer_aac_test()` (`apps/video/vplayer.c`) and the developer entry **BCM test: stage 9 (AAC on the chip)** (`apps/ipodui/devbcm.c`): the CPU's port copy (DMA stays off), every AU's PCM read back word by word, per-AU FNV-1a against `aac9.ref` (`bcmfw/aac/test/mkdevclip.sh`), at full clock and at 27 MHz, chip µs per AU from its own counter.

**Still for M4 on the device:** the probe's result, bit-exactness on silicon, and the real time per AU with the tables in SDRAM. Then 8b's path with AAC instead of PCM.

### Risks

- **The 64-bit multiply expansion** (cycles): measured, no longer an estimate. About 17 k `mulhd.ss` per AU; the inline is an M3 option.
- **`int64` division in `DIV_R`:** links `__divdi3`; not in the per-AU profile's top functions.
- **Stack depth in the Huffman paths:** measured at 724 bytes.
- **IPC with tables in SDRAM:** unknown until M4. The 1 IPC estimate has about 1.7× headroom against the 5 ms rule (2.9 ms mean stereo).
- **The AU ring's torn-index hazard:** the existing `HP_ENC` handles it.

## 8. Path A: releasing the PP's I2S data line

A desk study after session 32 (no device access). The question: can a register write on the PP5022 put its I²S data-out pin into high impedance, so that the BCM can drive the WM8758's data line without two outputs fighting? Sources: Rockbox's PP code, and `osos` 1.3 disassembled as ARM at `0x1000_0000` (`arm-none-eabi-objdump -b binary`); addresses below are `osos` host addresses.

### Short answer

- **No known register makes the PP's I²S data pin an input or high-Z.** Neither Rockbox nor Apple's `osos` has one. Both only stop the transmitter, and what the pin does then is not documented anywhere we have.
- **Apple doesn't need one, as far as the evidence goes.** `audio_enable 1` touches nothing on the PP's I²S, and the BCM pin that would carry the BCM's data (GPIO 1) does not look like it sits on the PP's data net (below). So the data line probably isn't shared at all, and "releasing" it is the wrong question. The right one is where GPIO 1 goes (open question #39).
- **So no PP register sequence is proposed.** The next step is a check of GPIO 1 on the BCM side, read-only (below), before anything is driven.

### What Rockbox does with the PP's I²S [rockbox]

| Step | Register writes | Source |
|---|---|---|
| Pin groups | `DEV_INIT2 &= ~0x300` ("normal outputs for CDI and I2S pin groups"), `DEV_INIT1 &= ~0x0300_0000` | `firmware/target/arm/pp/wmcodec-pp.c` `audiohw_init()` lines 51–55 (and `ascodec-pp.c` 36–40) |
| Device | `DEV_RS \|= DEV_I2S` then `&= ~DEV_I2S` (pulse); `DEV_EN \|= DEV_I2S`, `DEV_EN \|= DEV_EXTCLOCKS` (`DEV_RS` `0x6000_6004`, `DEV_EN` `0x6000_600C`, `DEV_I2S` = bit 11) | `wmcodec-pp.c` 57–65; `firmware/export/pp5020.h` 147–157 |
| Controller | `IISCONFIG` (`0x7000_2800`) bit 31 `IIS_RESET` pulsed; bus format `[11:10]` = I²S, size `[9:8]` = 16 bit, FIFO format `[6:4]` = 7; `IISFIFO_CFG` (`0x7000_280C`) levels and `TXCLR` (bit 8) / `RXCLR` (bit 12). Bit 25 `IIS_MASTER` is left clear on the iPod (the WM8758 is master: `CLKCTRL_MS`) | `i2s-pp.c` `i2s_reset()` 52–92; `pp5020.h` 397–494; `drivers/audio/wm8758.c` 224–225, 441–443 |
| Start | `IISCONFIG \|= IIS_TXFIFOEN` (bit 29), once, at init | `pcm-pp.c` `sink_dma_init()` 505–513 |
| Stop | only DMA channel 0 stops (`DMA0_CMD &= ~START`) and the FIFO drains; `TXFIFOEN` **stays set** | `pcm-pp.c` `dma_tx_stop()` 158–165, `sink_stop_pcm()` |
| Boot (iPod Video) | `DEV_INIT1 = 0`, `DEV_INIT2 = 0x4000_0000`, every device pulsed through `DEV_RS` | `system-pp502x.c` 496–509 |

- `pp5020.h` names no pin-direction or tri-state bit for the I²S block. `IISCONFIG` bits 24, 26, 2 and 3 are marked "????".
- The PP5022's GPIO ports (`GPIOx_ENABLE`/`OUTPUT_EN` at `0x6000_D000`+) have no documented mapping to the I²S pins. Rockbox never touches a GPIO for the iPod's I²S.
- So with Rockbox "stopped", the PP's transmitter is still enabled and still clocked by the WM8758's BCLK/LRCK whenever the codec's clocks run. It sends whatever an empty FIFO underruns to (zeros or the last word: not documented) [hypothesis]. **Rockbox's playback stop does not quiet the pin.**

### What Apple's `osos` does with the PP's I²S [apple]

The I²S driver object is built by the static constructor at host `0x1025_DE24`, at `0x149D_D48C`, by `0x1018_C85C` with base `0x7000_2800` (FIFO write `+0x40`, FIFO read `+0x80`). `0x100D_483C` returns it. Its virtual methods are at `0x1018_BD70`–`0x1018_C85C`.

- **Init** (`0x1018_C6D8`):
  1. checks a chip byte at `0x6000_0000` (`0x55`);
  2. enables and resets "device 10" through Apple's device-control object (our reading: `DEV_EN`/`DEV_RS` `DEV_I2S` [hypothesis]);
  3. calls `0x100C_8748`: `DEV_INIT1 &= ~0x0300_0000`, then `DEV_INIT2 &= ~0x100` (bit 8 only, where Rockbox clears `0x300`);
  4. pulses `IISCONFIG` bit 31;
  5. builds its DMA descriptors and FIFO objects.
- **Control** (`0x1018_BE14`, a switch on a message id):

  | Id | What it writes |
  |---|---|
  | 0 | FIFO format `IISCONFIG[6:4]`, `IISFIFO_CFG[1:0]`/`[5:4]` |
  | 1 | bus format `[11:10]` |
  | 2 | size `[9:8]` |
  | 3 | `IISCLK[8:0]` (`0x7000_2808`) |
  | 4 | bit 25 (set or clear) |
  | 5 | device enable/disable |
  | 8 | receive state: `RXFIFOEN` bit 28 + `IRQRX` bit 0, `RXCLR` |
  | 9 | transmit state: `TXFIFOEN` bit 29 + `IRQTX` bit 1 |

  Starting transmit sets `TXCLR`, writes 16 zero words to the FIFO, then sets bits 1 and 29. Stopping clears bits 29 and 1 (`0x1018_C488`).
- **Nothing in the driver touches a pin.** No write to `DEV_INIT1/2`, a GPIO port or the pad registers outside init.
- **Nothing in `osos` sets `DEV_INIT2` bit 8 or 9.** Every `0x7000_0000`-based access and every literal `0x7000_0020` in the image was checked. The writers are:
  - boot (`0x1028_2554`, keeps only bits 31–29);
  - the I²S init above;
  - PLL power (bit 30);
  - an LCD-side `&= ~0x4000` (`0x1027_6E4C`);
  - a whole-register restore after sleep (`0x1000_0654`).
- **Clock master can be swapped.** The WM8758 driver object (constructor `0x1015_D9BC`, I²S at `+0x44`) has a method (`0x1015_BFF0`) that sets either:
  - codec master: WM8758 R6 bit 0 set (`0x1015_D614`), and the PP's `IISCONFIG` bit 25 clear; or
  - PP master: R6 bit 0 clear, bit 25 set.

  This matches Rockbox's `IIS_MASTER` sense. When Apple uses the second mode is not traced.
- **The `audio_enable` call sites call nothing on the PP's audio hardware.**
  - **`audio_enable 1`** (`0x101F_8EAC`): the code just before it sends BCM commands (`mp_set_policy min`, `mp_show_stats …`, via `0x101B_013C`). The code just after it updates a play-state record (`0x101C_E5CC`).
  - **`audio_enable 0`** (`0x101F_91F0`): followed by two calls into a player-manager object (`0x1017_7814` → `0x1017_79D8`, a virtual call through its second child).
  - These paths contain no I²S or GPIO access, and no I²S call was found from the video player in this pass. They do not reach the I²S driver directly; an indirect route through the player manager is not ruled out.

### What the BCM side says [apple] + [silicon S32]

- **Apple drives GPIO 1 when audio is off.** `audio_enable 0` sets GPIO 1 to **function 1 (output)**, at whatever level its output latch holds, and leaves GPIO 2 and 3 in function 2 (vmcs @136C2).
  - The BCM runs for as long as the screen is on (it is the LCD controller).
  - So after any video, Apple keeps GPIO 1 driven through every later song the PP plays.
  - If GPIO 1 shared a net with the PP's data output, every song after a video would have two outputs fighting.
- **GPIO 1 stayed idle while the PP played** (session 32, stage 8a-0: 0 edges a second). A pin on the PP→WM8758 data net would show the soundtrack's bits.
- **GPIO 3 toggled at 88 k edges a second** (2·fs: the frame clock, LRCK). **GPIO 2 was faster than our 631 k samples a second can resolve.** That is a bit clock aliased, or data.
  - Apple leaves GPIO 2 and 3 in function 2 for good after the first video.
  - Function 2 is therefore very likely a non-driving clock input on both pins [hypothesis].
  - So GPIO 2 is more likely BCLK than data.

**Reading [hypothesis, medium–strong]:**

- **Wiring:** GPIO 3 = LRCK in, GPIO 2 = BCLK in, GPIO 1 = the BCM's serial data out, **on a net of its own**.
- **How it reaches the WM8758:** something outside both chips merges it with the PP's data before the WM8758's single DACDAT input. The candidates:
  - **a selector:** a mux or switch, set by a PP GPIO;
  - **a gate:** both data outputs idle low (Apple writes zeros to the FIFO before starting the transmitter, and parks GPIO 1 as an output);
  - **the PP's I²S input:** the BCM's data goes to the PP's DIN, the WM8758's ADCDAT net. That needs the ADC's output to be off or high-Z, which we have not checked.
- **Release:** none of these needs the PP's data pin released.

### Candidate PP writes, if a shared line were ever found [hypothesis, none known to tri-state]

None of these is known to put the pin into high-Z, and none should be used to protect a driven line until the device check below says so:

1. `IISCONFIG &= ~(IIS_TXFIFOEN | IIS_IRQTX)`: Apple's own "transmit stopped" (`0x1018_C55C`). Most likely the pin is still driven (low or the last bit).
2. `DEV_RS |= DEV_I2S` (hold the block in reset) or `DEV_EN &= ~DEV_I2S`: the pad's state in reset or with the clock gated is unknown.
3. `DEV_INIT2 |= 0x100`: the "I2S pin group" bit that both firmwares clear for "normal outputs". Setting it selects an undocumented pad mode, which could be GPIO, another function or high-Z. Apple never sets it, so it is the least known of the three.

To restore: Rockbox's `audiohw_init()` → `i2s_reset()` → `sink_dma_init()` sequence (`DEV_INIT2 &= ~0x300`, `DEV_RS` pulse, `DEV_EN |= DEV_I2S`, `IIS_RESET` pulse, formats, `TXFIFOEN`).

### The device check (read-only on the BCM's side; nothing driven)

An extension of `HP_OP_PINS` on GPIO 1 only, with the pull registers of `interrupts-timers-gpio.md` §5 (`0x1000_2800 +0x60` mode: 0 off, 1 down, 2 up; strobe the pin mask through `+0x64`, with Apple's 150-iteration waits; then mode and strobe back to 0):

1. **GPIO 1 as input, pull off:** read its level 1,000 times (the value of the idle net).
2. **Pull up:** read. Then **pull down:** read. Then pull off.
3. **Repeat 1–2 in three PP states:**
   - (a) PP audio fully off (DAC standby, no clocks);
   - (b) PP playing a tone (Rockbox, `TXFIFOEN` set);
   - (c) PP transmitter stopped with `IISCONFIG &= ~TXFIFOEN` and the WM8758 clocks still running.

**Reading:**

| Result | Meaning | Next |
|---|---|---|
| Follows the pull in (a), (b) and (c) | Nothing drives GPIO 1's net: it floats into an input (a mux/gate input, or nothing) | Driving GPIO 1 from the BCM cannot fight the PP. 8a-1's tone may go ahead with the owner's go; whether it is heard tells us whether a selector must be set |
| Held at one level in all three | Something drives it (a pull resistor, another output) | Stop. Find out what before driving |
| Follows the pull in (a) or (c) but toggles or is held in (b) | GPIO 1 does share the PP's data net after all (contradicting S32) | Stop. Then path A needs a proven PP release: repeat the test with each candidate write above and keep only one under which the pin follows the pull |

**Also log:**

- GPIO 2 and 3's levels under the same pulls in state (a), to confirm they are inputs on nets the WM8758 drives only while its clocks run.
- The PP's `GPIOA`–`GPIOL` `INPUT_VAL` registers before and after `audio_enable 1` on Apple's firmware, if a session ever runs it, to look for a selector GPIO.

## What only the device can answer

These are open questions #34–#40: where GPIO 1–3 go, whether the data line floats, the block's clock and control bits, DMA channel 5's length unit and progress registers, where GPIO 1's net goes, and what the PP's data pin does when its transmitter is stopped. Also the PP's real AAC cost (8c) and the port's real read rate (8b).
