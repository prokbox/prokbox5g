# BCM2722 video host protocol ("VP"): our design

Issue #60, workstream 2 of `approach.md`. This is how the PP5022 (Rockbox) drives video on our own VideoCore image: opening a stream, feeding it compressed packets, keeping it in sync with the audio, and getting status back. **The design is ours.** It borrows nothing from Apple's text "gencmd" channel (`protocol.md` §2), which is reference only.

**Status (2026-10-10):** the design below, plus a **skeleton** in `bcmfw/hostproto.c` behind `BCMFW_HOSTPROTO=1` (off by default): the header, both rings, the version handshake, `OPEN`/`CLOSE` with a null codec, `FEED` (checked and consumed, not decoded) and `STATUS` (`bcmfw/tests/hostproto_test.py`). **The video build (`BCMFW_VIDEO=1`) adds H.264**: `OPEN` with codec 1, `FEED` packets decoded from the SDRAM FIFO, pictures converted and pushed to the panel, `EV_EOS`/`EV_ERROR`, and the minor-1 reply words; it plays end to end in the simulator (`bcmfw/tests/video_test.py`). The memory layout, timings and the PP driver's contract for video are in **`pipeline.md`**. Still not implemented: `CLOCK`, `PAUSE`, `SEEK`, `STATS`, `SLEEP`. There is no PP-side driver yet.

Numbers and names are in `bcmfw/hostproto.h`. It holds only constants, so the PP driver can include it as is.

## Decisions at a glance

1. **Shared memory, not a message bus.** One small header block at a fixed SRAM address, a command ring (PP → BCM) and an event ring (BCM → PP), all in the BCM's internal SRAM. The PP reaches them through the host port, as it reaches the framebuffer today. Compressed packets go into a separate **bitstream FIFO**, and the rings only carry their descriptors.
2. **Each shared word has exactly one writer.** The PP writes the command ring and its write index, and the event ring's read index. The BCM writes everything else. No locks are needed between the two chips.
3. **Torn-read-proof indices.** The host port is 16 bits wide, so a 32-bit write lands as two halves, and the other side can read a half-updated word. Every shared index carries a 16-bit value as `~v << 16 | v`, the same idea as Rockbox's `BCM_CMD()`. A word that fails the check is "not changed yet", and the reader looks again next time.
4. **Separate from the LCD contract.** `0x1F8` (`BCMCMD_*`), owned by `lcd-video.c`'s tick task, is unchanged. The video driver owns `0x100–0x13F` and the rings. The two Rockbox threads never share a word, so they need no common lock either.
5. **A version word.** The image writes `HP_VERSION = 'VP' << 16 | major << 8 | minor` last at start-up. A different major means "don't use". A higher minor only adds things. The chip image and the PP driver can then move forward independently (`approach.md`, "Keeping upgrades possible").
6. **No wait without a deadline, on either side.** The BCM never waits on the PP: if the event ring is full, it drops the event and counts it. The PP waits only with a timeout per command class, then checks a heartbeat. The failure path is re-uploading the image, never a hang. Audio is never affected.
7. **Audio is the master clock.** Audio stays on Rockbox's codec path. The PP sends its media time, and the BCM schedules frames against it.
8. **Race to idle.** The BCM decodes a few frames ahead in a burst, then sleeps until the next display deadline (`approach.md` item 4).

## 1. Layout

### Header (`HP_BASE = 0x100`, 64 bytes)

The vector table ends at `0xFF` and the LCD mailbox starts at `0x1E0` (`bcmfw/README.md`). An image without the protocol leaves this block zero, because the upload covers it.

| Address | Writer | Contents |
|---|---|---|
| `0x100` | BCM | `HP_VERSION`. Written last at start-up, so 0 means "no protocol" (an old image, or Apple's NOR `vmcs`). v1.1 = `0x56500101` (minor 1: the video replies, `pipeline.md`). |
| `0x104` | BCM | Capabilities: `HP_CAP_NULL`, `_H264_BP`, `_MPEG4_SP`, `_SCALE`, `_LCD_DMA`, `_JOBS_2D`, `_OVERLAY`, `_AUDIO` (stage 8b), `_AAC` (`0x0800`, stage 9), `_OVERLAY_SLIDE` (`0x1000`) |
| `0x108`/`0x10C` | BCM | Command ring: address, size in words (a power of two) |
| `0x110`/`0x114` | BCM | Event ring: address, size in words |
| `0x118`/`0x11C` | BCM | Bitstream FIFO: the address as the host writes it, size in bytes (a power of two) |
| `0x120` | PP | `CMD_WR`: command ring write index (words, encoded) |
| `0x124` | BCM | `CMD_RD`: command ring read index |
| `0x128` | BCM | `EVT_WR`: event ring write index |
| `0x12C` | PP | `EVT_RD`: event ring read index |
| `0x130` | BCM | `STATE`: `heartbeat << 8 \| state`, encoded. The heartbeat steps every 1.024 ms of the 1 MHz counter, or on every poll if the counter doesn't run. |
| `0x134` | BCM | `FIFO_RD`: bitstream consumed, in 64-byte units, encoded |
| `0x138–0x13F` | | reserved |

`0x140–0x1DF` is reserved for the header of the 2-D job interface (`approach.md`, "Shell contract").

### Records

Both rings carry 32-bit words. A record is a header word `opcode | n << 8 | tag << 16` followed by n payload words (at most `HP_MAX_PAYLOAD` = 32). A record may wrap around the end of the ring. The writer fills the words first, then publishes its index. The host port keeps writes in order, so the reader never sees an index ahead of its data.

- **Commands** have opcodes `0x01–0x5F`. Tags are the PP's own, and are echoed in the reply.
- **Replies** carry `opcode | 0x80` and the tag. Their first payload word is a status (`HP_OK` or `HP_E_*`), followed by the reply's own payload.
- **Events** (`0x60–0x7F`, tag 0) are unsolicited. Bit 7 is clear, so an event can never be mistaken for a reply. The bench found this the hard way: the first draft put events at `0xE1+`, which a reply to an unknown opcode `0x61+` could alias.
- **Opcodes `0x20–0x3F` are reserved** for 2-D jobs. They will use the same ring and record format, under a PP-side mutex, because the shell and the video player are different threads. If that mutex is ever contended in practice, they get a second ring.

## 2. Commands

| Op | Name | Payload | Reply payload | Skeleton |
|---|---|---|---|---|
| `01` | `HELLO` | host `major << 8 \| minor` | version, caps, max payload | yes |
| `02` | `OPEN` | codec, `w \| h << 16`, `mode \| flags`, destination `x \| y << 16`, destination `w \| h << 16` | session, FIFO address, FIFO size; minor 1: queue depth; minor 2 (H.264): the SDRAM set-up word; minor 3: the memory test word (0 unless `HP_OPEN_SDRAM_DPB`). On `HP_E_NOMEM` the reply carries the five SDRAM words instead (see below); later the 2-D cache epoch | null codec; H.264 in video builds |
| `03` | `FEED` | session, FIFO offset, length, PTS (µs, 64-bit: lo, hi), flags | none, unless `FEED_ACK`; errors always reply | checked and consumed; decoded in video builds |
| `04` | `CLOCK` | session, media time (µs, lo, hi), rate (Q16.16) | none | |
| `05` | `PAUSE` | session, on/off | PTS of the frame on screen | |
| `06` | `SEEK` | session, target PTS (lo, hi), flags | ack, once everything queued is dropped | |
| `07` | `CLOSE` | session, flags (keep the last frame on screen) | packets, bytes; minor 1 (video): pictures decoded, shown, decode errors | yes |
| `08` | `STATUS` | session | state, session, packets, bytes, errors, events lost; minor 1 (video): pictures decoded, shown, decode errors, macroblocks concealed, packets queued, PTS on screen (lo, hi); then (p14-player, for stage 6's per-frame log) the last FEED's time on the chip in µs (decode, convert and push), the longest since OPEN, and the core clock in MHz (27 or 150). A reader takes the words it knows | yes |
| `09` | `STATS` | session | decoded, shown, dropped late, dropped corrupt, starvations, decode µs per frame (mean and maximum), core busy % | |
| `0A` | `SLEEP` | flags | the PTS to resume from; the host may then cut power | |
| `0B` | `PING` | anything | the same words | yes |
| `0F` | `AAC` | session, sub-op, its words (below) | per sub-op | video builds (M4) |
| `0D` | `PINS`, four words (PB-2) | mask 0–31, µs, flags 0, mask 32–51 | samples, µs, AND 0–31, AND 32–51, OR 0–31, OR 32–51, 13 words of edge counts (a byte a pin) | the plain video image |
| `13` | `PEEK` (PB-1, PB-3, PW1) | magic, gap µs, 1–15 addresses | two reads of each; `HP_E_INVAL` (the refused address) for any address off the allowlist, nothing read | the plain video image |
| `14` | `TV` (PB-6 to PB-9) | magic, step, standard, arm | reserved: every step `HP_E_UNSUPPORTED` | the plain video image |
| `15` | `PULL` (PB-4) | magic, first pin, count (1–8) | a word a pin: pin, skip flags, the levels under each pull | the plain video image |
| `16` | `CACHE` (PW2) | magic, TIME or COH, cache word (Apple's three), region or memory | TIME: cold µs, warm µs, both sums, flush polls; COH: the core's load, step timeouts, flush polls | the plain video image |
| `17` | `DMACPY` (PW8) | magic, case, seed | DMA µs, core µs, wrong words (DMA, core), channel 7 status, timeout | the plain video image |

Events: `EV_ERROR` (status, PTS), `EV_EOS` (the last frame has been shown), `EV_LOST` (the session ended because of `LCD_SLEEP`; payload: session).

- **Codecs:** 0 is the null codec (test path), 1 is H.264 Baseline (video builds), 2 is MPEG-4 Part 2 Simple Profile.
- **Sizes:** at most 640×480, even dimensions.
- **Modes:** `FIT` scales to fit and letterboxes (keeps the aspect ratio; today 1:1 or 2:1, `pipeline.md`). `FILL` scales to fill and crops (refused for now: it needs a non-integer scale). `NATIVE` is 1:1, centred.
- **`OPEN`'s SDRAM words (minor 2, dev10).** Word 0: `0x5344` (`"SD"`) `<< 16 | step << 12 | attempts << 8 | seen << 4 | flags`. Step: 0 up, 1 command 7 never finished and no probe passed, 2 bit 15 seen but no probe passed, 3 the decoder's SRAM context, 4 its frames in SDRAM. Attempts 1–4: field 8, field 4, then both again with the cache on (Apple's state). Seen: bit n = attempt n+1 saw `+0x20` bit 15. Flags: bit 0 the 1 MHz counter bounded the waits, bit 1 the core was on PLL A. Words 1–3 (only on `HP_E_NOMEM`): the last `+0x20` read, then the probe words read back at `0xFE000000` and `0xFE000200`. After a failed `OPEN` the image is back at 27 MHz, so an `OPEN` without `HP_OPEN_FAST_CLOCK` can follow at once (`video.c`; the player does this, `vpcore.c`).
- **`HP_OPEN_SDRAM_DPB` (`0x400`, dev11, diagnostic).** By default a session decodes into internal SRAM whenever the stream fits there (320×240 with up to 4 references), and uses SDRAM only for the bitstream and the frames of larger streams. This flag puts the frames in SDRAM regardless, and runs a memory test before the decoder opens. Its word (`OPEN` word 5, or SDRAM word 4 on `HP_E_NOMEM`) is `0x4D << 24 | size in 64 KB units << 16 | wrong grid words << 8 | bits`. Bit 0: a vector row loaded from SDRAM read back right. Bit 1: one stored to SDRAM did. Bit 7: the test ran. Stage 6's "6d" pass uses it.
- **`HP_OPEN_C_*` (bits 12–16, dev14, diagnostic).** Bits 12–15 run decoder families in scalar C instead of the vector kernels (`H264_OFF_*` << 12: MC, transform, loop filter, intra). Bit 16 (`HP_OPEN_C_DISP`) does the same for the colour conversion. The pictures are the same either way in vc2sim. Every video session also keeps a guard block at `0xDF000` (`video.h` `VIDEO_GUARD`): after each NAL unit, conversion and panel push the image checks the header's fixed words and `HP_EVT_WR`, and records the first hit and the first picture's planes. Stage 6 reads it at a loss and at the stop. `HP_OPEN_SELFTEST` (bit 17) runs every vector kernel against its C version at `OPEN`; the guard's words 16–20 hold the result (`video.h` `VIDEO_ST_*`).
- **Progress words (dev11).** For a host whose image stopped answering:
  - `0x1C4` = phase `<< 24 |` detail (`video.h` `VIDEO_PH_*`: opening, memory test steps, packet, wrap copy, NAL unit, macroblock, loop-filter row, conversion, panel push, shown, idle);
  - `0x1C8` = packets taken;
  - `0x1CC` = the phase's address;
  - `0x1D0` = frame 0's luma plane, once laid out (SRAM below `0x140000`, SDRAM at `0xFE……`).
  The player reads these words, the fault pc/sp, `INFO`, `HP_STATE` and the status word before it cuts power, and stops at the first port failure. A dead port means the bus is locked, not that the code faulted.
- **`OPEN_OWN_DISPLAY`:** while the session is open, `LCD_UPDATE`/`UPDATERECT` are acknowledged without drawing. On close, the PP sends a full `LCD_UPDATE` to repaint the UI. An on-screen overlay comes later through 2-D jobs composited by the BCM, not through the LCD path.

### `AAC` (op `0x0F`, `HP_CAP_AAC`, M4 / stage 9) [ours]

The AAC-LC decoder (`bcmfw/aac`, Rockbox's libfaad, `audio-spike.md` §7) inside the video image, on the host's bytes in SDRAM. Added by capability, without a minor bump (as `PINS` and `AUDIO`).

| Sub-op | Words after session, sub-op | Reply status | Reply words |
|---|---|---|---|
| `OPEN` (0) | ASC offset, ASC length | `HP_OK`; `HP_E_UNSUPPORTED` (not LC, more than 2 channels); `HP_E_STATE` (the SDRAM tables are not as copied: boot again); `HP_E_INVAL` | rate, channels, `aac_open`'s code |
| `DECODE` (1) | AU offset, AU length (≤ 2,036), PCM offset (word-aligned, 4 KB) | `HP_OK`; `HP_E_CORRUPT` (the decoder refused the AU; the next one decodes); `HP_E_STATE` (no `OPEN`); `HP_E_INVAL` | frames (0 for the priming AU, else 1,024), core µs (1 MHz counter), core MHz, `aac_decode`'s code |
| `RESET` (2) | none | `HP_OK`; `HP_E_INVAL` before an `OPEN` | none |
| `PROBE` (3) | none; **no session needed** (the session word is ignored) | `HP_OK` | bcs/bcc taken bits (bcs 15:0, bcc 31:16), what vc2sim's model expects (`0x9DAD6252`), cases (16) |

- **A session is needed** for everything but `PROBE`: an H.264 `OPEN` puts SDRAM up, sets the clock (`HP_OPEN_FAST_CLOCK`: 150 MHz) and, once per boot, copies the decoder's SDRAM tables in. A null-codec session gets `HP_E_STATE`. The decoder's state outlives a session; a new `OPEN` restarts it.
- **Offsets** are SDRAM offsets (`0xC0000000 +` for the host). A range must lie inside the audio rings (`0x340000–0x37FFFF`) or the AAC test I/O (`0x39C000–0x39FFFF`), without wrapping: anything else is `HP_E_INVAL`.
- **Output:** 1,024 interleaved 16-bit stereo frames (mono on both channels), Rockbox's undithered output stage at depth 29. The host reads them back word by word.
- **Cost:** the call blocks the image's loop for the decode, about 2.2–2.8 ms per stereo AU at 150 MHz in vc2sim (M3 means, worst AU 4.2 ms; 1 IPC, `audio-spike.md`); a video decodes between calls. `STATUS`/heartbeat stay live between calls only.
- **The probe** runs 16 compares (equal, either side of 0 and `0x80000000`, all ones, pointer-like pairs), each followed by a far `bcs` and a far `bcc` (the 32-bit forms), and reports which were taken. `bcs` must be taken exactly when a < b unsigned (`cmp` sets C as a borrow, `quirks.md` #7). The decoder executes both kinds; no earlier check ran them on silicon (`bcmfw/aac/brprobe.S`).

### The bitstream FIFO

- The PP demuxes the file (MP4/M4V) and writes each packet (NAL units with start codes, or a VOP) straight into the FIFO through the host port. It then posts a `FEED` record with the packet's offset and length.
- **Packets start on 64-byte boundaries** and follow each other in order. Packet k+1 starts at `align64(end of k)`, modulo the FIFO size, so a packet may wrap the end. The BCM checks the offset and answers `HP_E_FIFO` with the offset it expected. That catches a lost or duplicated record at once.
- **Flow control without commands.** The BCM advances `FIFO_RD` (64-byte units) when it has finished with a packet. The PP's free space is `size − (written − FIFO_RD·64)`. The PP also limits the number of packets in flight to the queue depth from the `OPEN` reply, so the BCM never has to stop consuming the ring. If it did, a `PAUSE` could get stuck behind `FEED` records.
- **Bulk I/O on the PP** (`docs/hardware.md`): the demuxer reads the file in large chunks with Rockbox's buffering, never per frame. The FIFO is refilled in bursts, about 10 s of 768 kbit/s video per megabyte.

## 3. Clock, sync, pause and seek

- **Clock.** The PP sends `CLOCK` with the audio position (µs) and the playback rate at start, on every play, pause, seek or speed change, and every 500 ms otherwise. Between them, the BCM runs its own media clock: `last + (counter_now − counter_at_receipt) × rate`. Errors under 20 ms are slewed; larger ones jump.
- **Presenting.** A frame is shown once `pts ≤ clock + half a frame`. A frame older than `clock − one frame` is dropped and counted as late. When decoding falls behind, non-reference pictures (`nal_ref_idc = 0`) are skipped first. The audio never waits for video.
- **Pause** stops presenting and keeps the queue.
- **Seek.** `SEEK(target)` drops all queued packets and frames, resets the FIFO positions to 0 and waits for a key packet. The PP then feeds from the keyframe at or before the target. Frames before the target are decoded but not shown, which makes seeks precise. The ack comes after the drop.
- **End of stream.** A `FEED` with `FEED_EOS`, then `EV_EOS` once its last frame has been shown. **Ordering:** the image raises `EV_EOS` inside `video_work` and advances `FIFO_RD` past the EOS packet just after, in the same main-loop pass (`hp_work`), so a host can see `EV_EOS` with `FIFO_RD` one packet short. Nothing in the protocol promises `FIFO_RD` before `EV_EOS`. The PP doesn't depend on it (checked 2026-10-25, P19 PW4): `bcmvp.c` `bvp_refresh` (`FIFO_RD` to retired packets) is a running count that catches up at the next read; `vpcore.c` treats `EV_EOS` as the end (`VPC_DONE`, no FIFO wait), `vpc_stop` closes the session (`bvp_close` resets the FIFO), a seek after the end pushes with the last packet still counted in flight until the next `STATUS` cycle's `bvp_refresh` (one slot of `VP_MAX_IN_FLIGHT` for one cycle), and lockstep (`vpcore.c` `push_body`) re-reads `FIFO_RD` until it moves. `video_test.py`'s robustness rows read `FIFO_RD` one slice after `EV_EOS` for that reason.
- **The host's overlay (dev35, minor 4, `HP_CAP_OVERLAY`).** While a session owns the panel, the image drops `LCD_UPDATE`/`UPDATERECT` (`bcmfw/main.c`), and the PP's LCD driver is held off anyway (`dt_take`). So the shell's own drawing, such as the scrub bar over a paused video, reaches nothing. `OVERLAY` (op `0x0C`: session, `x|y<<16`, `w|h<<16`) shows the host frame the PP has written at `BCMFW_FB` (0xE0000, panel coordinates, 320 pixels a row) over the picture in that rectangle. The image blits it at once and again after every picture, until an `OVERLAY` with `w` or `h` 0. That one puts black where the rectangle covered the borders, then the last picture (still in `VIDEO_FB`) back. An `OPEN` clears it; the PP sends it again after every restart or wake (`vpcore.c` `ovl_apply`). The shell draws as usual and calls `vplayer_overlay(x, y, w, h)` instead of `lcd_update_rect`. While playing, each picture's blit briefly covers the strip before the strip goes back on top. **The slide (2026-10-15, `HP_CAP_OVERLAY_SLIDE` `0x1000`) [ours].** An optional fourth word, `HP_OVL_SLIDE_UP` (`0x10000`) or `HP_OVL_SLIDE_DOWN` (`0x20000`) with the duration in ms in the low 16 bits, makes the image move the band itself, by the 1 MHz counter, eased out (1 − (1 − t)³), stepped from the main loop (`video_anim`). Up: the band rises from its rectangle's bottom edge (from wherever a running slide has it). Down: the band that is up sinks and goes, as a zero-width `OVERLAY` would at the end. The image puts the last picture (`VIDEO_FB`) back under the rows the band leaves, with black where the picture doesn't reach, so a slide works while paused too. A plain `OVERLAY` during a slide up leaves the slide running, so the host can refresh the band's pixels. Without the counter (`BCMFW_INFO_TIMER_OK` clear) or with 0 ms, both happen at once. The host sends the band's pixels once per change; each step is a panel blit on the chip and costs the PP nothing (`vplayer_overlay_slide`).
- **Hidden pictures (dev32).** A `FEED` with `FEED_HIDE` (0x10) is decoded but its picture is never shown, and `shown` doesn't count it. The PP uses it for a seek's catch-up, the frames from the key to the target. Before dev32, stage A showed every picture it decoded, so after every seek the GOP's head played out late (session 28, stage 7: up to 3.5 s behind).

## 4. Errors and timeouts

**On the BCM:**
- It never blocks on the PP. If the event ring is full, it drops the event and counts it (`STATUS` word 5). The PP keeps at most 8 commands awaiting a reply, and the event ring is sized for many more replies than that, so in normal use only unsolicited events can be dropped.
- It services the command ring at least once per macroblock row while decoding: about 1–2 ms at 320×240, and the target command latency is at most 5 ms.
- An unknown or unimplemented opcode gets `HP_E_UNKNOWN`. A record longer than 32 words gets `HP_E_TOOBIG`, and a short payload `HP_E_INVAL`. Every command a client could wait on gets exactly one reply.
- **Decode errors** are concealed (copy from the reference, or skip the macroblock). They're counted, and reported as `EV_ERROR` at most once per second. **Fatal errors** (not Baseline, too large, more references than the DPB holds) put the session in the `ERROR` state with `EV_ERROR(HP_E_UNSUPPORTED)`. The player closes and logs it: errors are logged, not shown in a splash (CLAUDE.md rule 1).

**On the PP:**

| Class | Timeout |
|---|---|
| `HELLO` after `bcm_init` | 50 ms |
| `OPEN` (SDRAM set-up and a clock change, about 12 ms) | 200 ms |
| `CLOSE`, `SEEK`, `SLEEP` | 100 ms |
| `STATUS`, `STATS`, `PING`, `PAUSE` | 50 ms |
| `FEED`, `CLOCK` | no reply: progress shows in `FIFO_RD` and `CMD_RD` |

- A full command ring returns "busy" to the caller at once. The demuxer retries on its next tick.
- **Liveness.** The PP reads `STATE` and `0x1F8` on every tick during playback (10–100 ms) and whenever a wait times out. The BCM is considered dead if the heartbeat hasn't moved over three looks and 200 ms, or if `0x1F8 = 0xFA170000` (fault). **Look often:** the heartbeat is 8 bits of a 1.024 ms count and comes round every 262 ms, so looks that far apart (the first plan said every 500 ms) can see one value on a live image; the simulator caught exactly that (`player.md`, "As built").
- **Recovery.** Log it, close the player's video side, then `bcm_powerdown()`/`bcm_init()` with the same 6 KB image. The LCD contract comes back with it. The player reports that the video can't continue, and audio carries on. After a second failure in one session, video is disabled until reboot (logged).
- Version mismatch (no magic, or another major): no video at all, logged. The UI simply doesn't offer playback, as today.

## 5. Living with the LCD contract

- `0x1F8` keeps Rockbox's semantics exactly: `lcd-video.c` needs no change for the protocol to exist.
- The image's main loop checks the LCD command word and then makes one pass over the command ring, every iteration (`bcmfw/main.c`). LCD commands come first. Decoder work runs in slices between them.
- `LCD_SLEEP` with a session open ends the session (`EV_LOST`) before the panel goes off. The video player should send `SLEEP` first and get the resume PTS (section 6), so `EV_LOST` is the fallback for a sleep it didn't see coming.
- `BCM_CMD(0xC)` and the power cut are unchanged.
- The same uploaded image serves both contracts. `HAVE_BCMFW` (`bringup.md`, "ProkBox side") is still the only switch on the PP side, and the video driver checks `HP_VERSION` after `bcm_init()`.

## 6. Power

- **Race to idle.** Decode until the display queue is full (4 frames) or the FIFO is empty, then `sleep` until the next display deadline (a timer compare). The display push is a DMA transfer (`bringup.md`, "DMA frame push"), and once the channel interrupt is confirmed the core sleeps through it too.
- **Clock per session, not per frame.** A PLL relock costs 1 ms, plus a 10 ms regulator wait (`hardware.md`). So `OPEN` picks the clock for the stream: 150 MHz at first, then the lowest clock that holds the measured decode time with margin. `CLOSE` drops back.
- **SDRAM only when needed.** The LCD path needs none. SDRAM is set up at the first `OPEN` (or the first 2-D upload), and put in self-refresh when idle. The self-refresh register sequence is still unknown, and goes on the static and device list.
- **Sleeping with the backlight.** Rockbox powers the BCM off with the backlight (FS#9890, more than 30% battery). For video, the player sends `SLEEP` and gets the resume PTS, and the PP cuts power as today. Audio keeps playing. On wake: `bcm_init`, `HELLO`, `OPEN`, `SEEK(resume PTS)`, feed from the keyframe before it. The picture comes back in sync with the audio that kept going.
- **Polling.** Until the doorbell (interrupt source 23) is confirmed, the image either busy-polls or uses the 1 ms tick of the idle-sleep build. With the doorbell, there are no wake-ups between commands. In the other direction, the BCM can pulse GPIO 23 (wired to PP GPIOC bit 6) when it posts a reply or an event, so the PP needn't poll either. Both are optional: the protocol works on polling alone.
- **None of this is measured.** The BCM's current draw per state is a device question for `docs/hardware.md`'s budget table.

## 7. Memory map

### Internal SRAM (1.25 MB, `0x000000–0x13FFFF`)

| Range | Size | Use |
|---|---|---|
| `0x000000–0x0000FF` | 256 B | vectors (64 words) |
| `0x000100–0x00013F` | 64 B | VP header (this document) |
| `0x000140–0x0001DF` | 160 B | reserved: the 2-D job interface's header |
| `0x0001E0–0x0001FF` | 32 B | LCD mailbox (`bcmfw/README.md`) |
| `0x000200–0x07FFFF` | 510 KB | image: code, tables, data, `.bss`. The UI images 4–10 KB. **The video image (M4) to `0x30034`**: H.264, display and protocol about 96 KB, the AAC decoder's code 33 KB, its SRAM tables (twiddles, Huffman, short windows) about 21 KB and hot buffers about 41 KB. The load image also carries the AAC decoder's SDRAM tables (48 KB) after `.data` (below) |
| `0x031000–0x0CBFFF` | 620 KB | video build: the H.264 decoder's arena (`BCMFW_VIDEO_ARENA`): context, per-MB info, then the frames when they fit (320×240 with up to 4 references: 617,056 bytes). **Until the first H.264 `OPEN`**, the parked AAC tables sit at `0x134000–0x13FFFF` (`BCMFW_AAC_PARK_END` = `0x140000`; `build.py` reports it), above the arena (crt0 moves them there from the load image before clearing `.bss`; the `OPEN` copies them to SDRAM before the decoder takes the arena) |
| `0x0CC000–0x0CFFFF` | 16 KB | stack (top `0xD0000`) |
| `0x0D0000–0x0D1FFF` | 8 KB | command ring (2,048 words: about 290 `FEED` records) |
| `0x0D2000–0x0D2FFF` | 4 KB | event ring (1,024 words) |
| `0x0D3000–0x0D3FFF` | 4 KB | DMA control blocks, display-queue descriptors |
| `0x0D4000–0x0DBFFF` | 32 KB | the skeleton's bitstream FIFO. The video build reads the SDRAM FIFO directly (DMA staging here is the fallback if the device needs it). |
| `0x0DC000–0x0DFFFF` | 16 KB | video build: the scaler's strip (7.5 KB); the rest spare |
| `0x0E0000–0x1057FF` | 150 KB | LCD framebuffer (Rockbox's contract) |
| `0x105800–0x13FFFF` | 234 KB | second working pool. Video build: the RGB565 picture (150 KB at `0x105800`); later 2-D job scratch |

### SDRAM (4 MB: `0xFE000000` for the BCM, `0xC0000000` through the host port)

| Range | Size | Video session | No session (UI) |
|---|---|---|---|
| `0x000000–0x0FFFFF` | 1 MB | bitstream FIFO (written by the PP) | volatile 2-D cache |
| `0x100000–0x2FFFFF` | 2 MB | DPB, YUV 4:2:0: four 640×480 frames (3 references + current) or seventeen 320×240 frames | volatile 2-D cache |
| `0x300000–0x395FFF` | 600 KB | display queue: 4 × RGB565 320×240, already scaled and converted (planned; the video build uses `0x300000–0x33FFFF` for the copy of a packet that wraps the FIFO) | volatile 2-D cache |
| `0x396000–0x3FFFFF` | 424 KB | resident 2-D cache: glyphs and icons the overlay needs | the same |

**As built (video image, M4; `bcmfw/video/video.h`):** `0x000000` FIFO (1 MB); `0x100000` DPB (2 MB less 16 KB, used only by streams too large for SRAM); `0x300000` wrap copy (256 KB); **`0x340000–0x37FFFF` audio rings** (stage 8b's in at `+0`, out at `+0x20000`; the player's later); **`0x380000–0x395FB3` the AAC decoder's far data**: `0x380000` the long windows (sine, KBD: 8 KB), the inverse-quantisation table (32 KB), the FFT's bit-reversal table (8 KB), then the channel-pair and single-channel elements (40 KB, cleared at the copy), checked by `build.py` against `0x39C000`; **`0x39C000–0x39FFFF` AAC test I/O** (stage 9: the ASC at `+0`, the AU at `+0x800`, the PCM at `+0x1000`); `0x3A0000–0x3FFFFF` the diagnostic dumps (`VIDEO_DUMP_*`). A diagnostic `OPEN`'s memory test (`HP_OPEN_SDRAM_DPB`) overwrites the AAC tables; `AAC OPEN` then answers `HP_E_STATE`.

- **The volatile 2-D cache** (3.6 MB of album art at pre-scaled sizes, font atlases and composited layers) is evicted when a video session opens. The `OPEN` reply bumps a cache epoch, and the shell re-uploads by content hash after `CLOSE`. Assets are uploaded once and cached by hash (`approach.md`).
- **The display queue is RGB565**, converted and scaled at decode time. That's possible because Baseline has no B-frames, so decode order is display order. A frame is then a single DMA from SDRAM to the LCD at its deadline, with no copies in between (`approach.md` item 5).
- **The DPB** limits streams to three reference frames at 640×480. Encodes made for the iPod (Baseline, level 3.0) and our own converter's settings fit. A stream that needs more gets `HP_E_UNSUPPORTED` at its SPS.

## 8. The PP's cost per frame

**Measured on the skeleton** (`hostproto_test.py`, host-port cycles as lcd-video.c makes them: two 16-bit transfers per address setup, two per word):

| Operation | PP: host-port transfers | BCM: instructions |
|---|---|---|
| `FEED` record, index publish and one `CMD_RD` read | 24 | |
| `FEED` with `ACK`, round trip | 38 | 230 |
| `STATUS` round trip | 40 | 181 |
| `PING` round trip | 26 | 101 |
| A 3,052-byte packet (the bench's mean), data + record | 1,553 | |

**Budget at 30 fps, 320×240, 768 kbit/s (3.2 KB per frame on average):**

| PP work per frame | Budget |
|---|---|
| Demux: walk the MP4 sample tables, precomputed at open | ≤ 3,000 cycles |
| Bitstream into the FIFO: about 1,600 transfers at 4–8 cycles each (lcd-fps-ipodvideo.md's model) | 6,400–12,800 cycles |
| `FEED` record and index: 24 transfers | ≤ 200 cycles |
| Liveness and events: one `STATE` read per 10 ms tick, `CLOCK` twice a second | ≤ 300 cycles |
| **Total** | **≤ about 16,000 cycles: 0.5 Mcycles/s, under 2% of the unboosted 30 MHz** |

For comparison, today's mpegplayer sends 76,800 transfers per frame (300,000–600,000 cycles), plus the colour conversion and the decode itself on the ARM. Keyframes are larger (20–40 KB), but they are fed in bursts and the budget holds per second. **Not measured on hardware:** the transfer cost per halfword is the lcd-fps model's estimate.

## 9. The skeleton (`BCMFW_HOSTPROTO=1`)

- `hp_init()` runs before the image reports ready on `0x1F8`. It writes the header, then the version last.
- `hp_poll()` runs once per main-loop iteration. It steps the heartbeat, then handles every complete record in the command ring.
- Codec 0 (null) is the only one offered (`HP_CAP_NULL`). H.264 and MPEG-4 get `HP_E_UNSUPPORTED`.
- `FEED` checks the session, the offset (in order, 64-byte aligned, wrapping) and the length, then advances `FIFO_RD`.
- `OWN_DISPLAY` drops LCD updates, and `LCD_SLEEP` ends the session with `EV_LOST`.
- The default image is unchanged byte for byte: the protocol adds 1,920 bytes when enabled (5,924 in all; the video build is in `pipeline.md`).

**Bench:** `bcmfw/tests/hostproto_test.py`, **10 rows, 100 cases, 0 failures**, about 14 s. The PP side is modelled as a driver that only ever waits with a deadline. It covers:
- the header and the LCD contract on the same image (and a zero header in the default image);
- `HELLO` with a major mismatch, and `PING` with 0–32 words;
- `OPEN`/`CLOSE` error cases;
- 300 `FEED`s wrapping the FIFO, with order errors;
- unimplemented opcodes, an oversize record, and 3,000 commands wrapping both rings;
- torn `CMD_WR` and `EVT_RD` words being ignored;
- a full event ring: drops are counted, the BCM keeps going, and LCD frames still work;
- `OWN_DISPLAY`, then `LCD_SLEEP` ending the session, and a re-upload;
- costs and the heartbeat;
- instruction-kind coverage: the image uses only kinds the self-test exercises, plus the same three accepted forms.

### Device checklist: protocol

1. [ ] **Torn writes.** Write `CMD_WR` from the PP in a tight loop while the BCM reads it. No command may run twice or be skipped. The bench forces torn values. The device shows whether `bcm_write32` writes the low or the high half first, and the encoding handles both.
2. [ ] **Ordering.** Ring data written before the index must be visible to the BCM before the index. A single 16-bit bus implies this [M].
3. [ ] **Heartbeat.** It steps about once per ms, with the busy-poll and the idle-sleep builds alike.
4. [ ] **Round-trip time** for `PING` from the PP, with busy-poll, with idle-sleep, and later with the doorbell.

## 10. The H.264 baseline: ffmpeg files and port order (unverified)

**Unverified:** this list is from memory of libavcodec around FFmpeg 6.x/7.x. Check file names, splits and licences against a pinned release tag before porting. The H.264 decoder files are LGPL 2.1 or later, which is compatible with our GPL v2+ firmware. Keep their copyright headers. Nothing in the H.264 decoder should need `--enable-gpl`, but confirm that per file.

**Port order.** Each step comes with its own vc2sim tests against ffmpeg's output, bit-exact:

1. **Bitstream.** `get_bits.h` (the bit reader), `golomb.h`/`golomb.c` (Exp-Golomb and their tables), and `h2645_parse.c` (splitting into NAL units and removing emulation-prevention bytes). Our own `vc2cc_tests` CAVLC-style reader row is the template for the VPU version.
2. **Parameter sets.** `h264_ps.c`/`h264_ps.h` (SPS and PPS), `h264data.c`/`h264data.h` (tables), and the shared parts of `h264_parse.c`.
3. **Slices and references.** The slice header part of `h264_slice.c`, `h264_refs.c` (reference lists, MMCO, sliding window) and `h264_picture.c`, replaced by our fixed SDRAM DPB.
4. **CAVLC macroblock layer.** `h264_cavlc.c` and `h264_mvpred.h` (motion-vector prediction, P_Skip). ffmpeg builds its VLC tables at run time: we precompute them at build time into `.rodata`, per the "pre-compute, never at browse time" rule.
5. **Reconstruction.** `h264_mb.c` and `h264_mb_template.c`, `h264idct_template.c` (4×4 and DC-only paths), `h264pred_template.c` (intra), `h264qpel_template.c` and `h264chroma_template.c` (motion compensation), and `videodsp_template.c` (edge emulation for motion vectors that point off the picture).
6. **Deblocking.** `h264_loopfilter.c` and the loop-filter functions in `h264dsp_template.c`, keeping the bS = 0 early-outs.
7. **Vectorise in measured order:** interpolation, then IDCT, then deblocking (`approach.md`). The reference shapes for the VPU's 16 lanes are `h264qpel_neon.S`, `h264cmc_neon.S`, `h264idct_neon.S`, `h264dsp_neon.S` and `h264pred_neon.S` under `libavcodec/arm` and `aarch64`.

**Left out:**
- `h264_cabac.c` and `cabac*` (Main profile);
- `h264_direct.c` (B-frames);
- the SEI parsers beyond what recovery points need;
- `error_resilience.c` at first: our concealment is simpler and comes later;
- frame and slice threading;
- hwaccel;
- `AVFrame`/`AVBuffer` plumbing, replaced by our fixed buffers.

MPEG-4 Part 2 (`mpeg4videodec.c`, `h263dec.c`, `mpegvideo*.c`) follows the same pattern later.
