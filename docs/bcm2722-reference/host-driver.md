# 17. Writing a host video driver

What a PP5022-side driver must know to run video on the BCM2722 with its own image: the session's life from power-down to hand-back, how to feed compressed pictures, how to keep them in step with audio played by the host, who owns the panel, what it costs, and how it fails. This is a spec sheet, not a walk-through: the register-level material is in [`host-interface.md`](host-interface.md), [`boot-and-clocks.md`](boot-and-clocks.md), [`sdram.md`](sdram.md), [`lcd.md`](lcd.md) and [`dma.md`](dma.md), and is linked, not repeated. Every memory path, its widths and its speed, and the rules for who writes where, are in [`memory-access.md`](memory-access.md).

**Silicon against [ours].** Everything tagged **[ours]** is a property of our host protocol (`bcmfw/hostproto.h`; design in `docs/research/bcm2722/hostproto.md`, `pipeline.md`, `player.md`) or of our image, not of the chip. A different driver can replace all of it. The **[silicon]** facts hold for any design. Host-side behaviour measured on the same unit (Rockbox's threads, its audio clock) is tagged [silicon] with its session, and [rockbox] where it comes from Rockbox's code. Sessions are S1 to S34 ([`sources.md`](sources.md)); the logs are `session log, session`, read only.

## 1. What any design has to provide

| Job | Silicon constraint | Our answer [ours] |
|---|---|---|
| Get code onto the chip | Cold bootstrap every time; the chip keeps no state across a power cut ([`boot-and-clocks.md`](boot-and-clocks.md) §5) | A ~93 KB image (93,352 B in S28–S30) uploaded at every session start and every wake |
| A command path | The port carries memory traffic only; no doorbell is confirmed on silicon ([`open-questions.md`](open-questions.md) #3, #28) | Polled rings in SRAM, encoded indices ([`host-interface.md`](host-interface.md) §3c) |
| Compressed data | 4 MB SDRAM, host-writable at `0xC000_0000` only after the image has initialised it ([`sdram.md`](sdram.md)) | A 1 MB bitstream FIFO at SDRAM offset 0 |
| Pictures on the panel | Only the BCM drives the panel ([`lcd.md`](lcd.md)) | The image converts and DMA-pushes each picture itself |
| A/V sync | The BCM has no audio path in use; the host plays audio ([`overview.md`](overview.md)) | Host-side clock; the host decides when each picture is posted |

## 2. The port under a video load

- **Transfers are 16 bits.** A 32-bit word goes as two halves, low half first ([`host-interface.md`](host-interface.md) §1). [ours] Our client writes whole 32-bit words, so a packet is padded to a multiple of 4 bytes; up to 3 bytes past its end are overwritten, which is harmless because the next packet starts 64-byte aligned.
- **Bulk writes into SDRAM while the core decodes are safe.** Unpaced `ldm`/`stm` streaming, 256-byte paced pieces and lockstep writing (nothing while the chip decodes) all read back with 0 wrong words (the first and last word of every written piece checked) [silicon S12]. The pre-play readback (64 words over the 1 MB FIFO) was 0 wrong in every session since [silicon S28–S30]. In S30 the session's first packet, read back from the FIFO at every stall, matched the host's bytes (first 16 bytes compared) [silicon S30]. Apple writes through the PP's DMA in 64 KB chunks [apple]; that is a speed choice, not a correctness need.
- **Throughput is bus-bound, and it scales with the PP's clock.** A full 320×240 frame (153,600 bytes) took 27.0 ms by the CPU's 2-register loop, 27.4 ms by 8-register bursts and 27.6 ms by the PP's DMA (channel 3, memory to the fixed data port, 32-bit, free running, 32 KB pieces) with the PP at 30 MHz. With the PP at 80 MHz it took 9.6, 9.9 and 10.4 ms. That is about 350 and 125 ns per 16-bit pixel. Every method read back clean (0/48) [silicon S8, two runs]. So PP DMA is no faster than the CPU's copy, but it frees the issuing core while a block moves. **Caveat:** in that lab the CPU methods had already written the same frame to the same place, so the PP DMA's clean readback does not show its bytes arrived; into the SDRAM FIFO it read back wrong in S32–S33 and landed nothing in S34's lab, and the player's default has been the CPU's copy since dev39a ([`memory-access.md`](memory-access.md) §7b; [`open-questions.md`](open-questions.md) #41). A 30 fps, 768 kbps stream needs about 3.2 KB a picture (0.6 ms of port time unboosted). A 36 KB IDR needs about 6.5 ms. [ours] dev39 moves blocks of 256 bytes and up by DMA. It starts each block and returns; the next port access waits for it, so the host prepares its next piece (two staging buffers) while the last one moves. Small blocks, and every block after a DMA that never ends (100 ms bound), go by CPU. The source must be word-aligned and cache-cleaned (`commit_dcache`, on the core that wrote the pixels: cache maintenance on the PP5022 acts only on the calling core, or an uncached alias). Load per session is in the player's log (`video: port …`, `thread active permille`), and stage 7 ends with an A/B over the PP's boost, the copy and the thread's priority.
- **Handshake waits** stay at or below 47 µs under load, with no timeouts [silicon S12].
- **Torn reads are the only port hazard found.** A 32-bit word the BCM rewrites while the host reads it can mix halves (`quirks.md` #23). In S11–S12 a torn event index read as a port failure, and the client cut power each time; every such "stall" was gone at the next look, about 7 µs later [silicon S12]. Encode every word the other side polls so a torn read is detectable (our `HP_ENC(v) = ~v << 16 | v`, checked by `HP_VALID`) and treat a bad word as "not yet", never as "dead". Our client reloads only after 25 consecutive torn reads (`VPC_TORN_MAX`) [ours].
- **A dead port reads `0xFFFF` per halfword** ([`host-interface.md`](host-interface.md) §1). Bound every wait (our bound: 20 ms per handshake, then a 250 ms ride-out while `HP_STATE` still answers, at most 8 a session) [ours].

- **A write into SDRAM may not be seen by the core behind a command that follows it** [silicon symptom, sitting 4b; mechanism hypothesis]. Writing a packet and sending its `FEED` at once produced no picture at any clock band, while packets written well ahead played. Write ahead of the `FEED`, and read the newest packet's last word back before a `FEED` that follows it closely (§4; `quirks.md` #47; `open-questions.md` #73).

### 2a. Every chip wait is bounded, and a timeout logs a FAULT [ours]

A host wait on the chip with no bound can freeze the PP, and a freeze leaves nothing to read. Sitting 4 (2026-10-26, build 207956fb84) ended in three silent freezes with no panic. Their cause was a branch to address 0 on the PP (open question #75), but the hunt also walked every wait on the chip reachable from the UI, art, drift and video threads and found two that could freeze the same way. The rule it left: **every wait is bounded, a timeout marks the chip stopped so that the next push power-cycles it, and each timeout logs one `FAULT <who> wait=<what> ms=<n>` line.**

| Wait | Bound | On expiry |
|---|---|---|
| Port handshakes (write-ready, read-ready) | 20 ms | port error; no retry under the lock (§2) |
| `lcd_sleep()`: the update in flight to end (it held the port lock for ever when the chip never went idle) | 1 s | chip marked stopped; the sleep goes on |
| `lcd_awake()`: the first update after a wake | 3 s | chip marked stopped; a late release is dropped |
| `bcm_command()` | bounded in every shell build (before, only in some) | chip marked stopped |
| `lcd_tick()`: one update busy | 2 s with Apple's `vmcs` (it had no stopped check and re-kicked a wedged update for ever); 6 s in job mode (our image's own heartbeat check ran only inside a job wait). The hang clock starts at a request after idle | chip marked stopped |
| `bjc_suspend_request()` (the job driver being suspended) | 500 ms | `FAULT` for an unacknowledged suspend; likewise for a leaked art call |
| The DECODE overlay's read and put (§6a) | 1 s | the load fails; the CPU path is the safety net |
| `vplayer_stop()`'s join of the player thread | 3 s | cannot be cut short (its stack is in use), so it logs a `FAULT` |

Mechanics [ours]: the first timeout of a boot records its cause; the job owner's pump logs the `FAULT` line (woken by a post, so it needs no polling); the line reaches the log file at the next UI pass whatever the disk and audio are doing, and the bench session flushes the log before each step's hold, so the line before a hang survives it.

**Hang record and jump-to-0 catcher.** A wait that still ends up stuck is caught from the PP's side: a tick task watches registered threads' progress counts and, past a thread's bound, writes a sealed hang record (interrupted pc, stacks, mutex owners, the log tail) without resetting; and a branch to address 0, which used to spin for ever with IRQs masked, now ends in an exception report. See `../pp5022-reference/quirks.md` #56 and #57. The simulator cannot produce either failure (`../sim-fidelity.md` §9a).

## 3. Session lifecycle

### Timeline [silicon S30 logs, our client]

| Step | What happens | Time |
|---|---|---|
| Take the panel | Host stops its own LCD driver (Rockbox: `lcd_state.display_on = false`, its tick task removed: `dt_take`) [ours, on [rockbox] state] | — |
| Power down what runs | Backlight off; regulator `0x10001400 &= ~0xF0`; `LCD_SLEEP`; `BCM_CMD(0xC)`; cut `GPO32` bit 14; stay off ≥ 50 ms ([`boot-and-clocks.md`](boot-and-clocks.md) §5). Ours stays off 100 ms. Skip when the BCM is already off (after a backlight sleep) | logged about 60 ms after the request |
| Boot | Bootstrap, upload the image, start at `0x200`, wait for the ready word ([`host-interface.md`](host-interface.md) "Starting an image") | — |
| First frame | Host writes its own frame to `0xE0000`, sends `LCD_UPDATE`; the first one after power-up also powers the panel on | 279.76–279.99 ms for that one command, six boots [silicon S30]; consistent with the panel's power-on in [`lcd.md`](lcd.md) §5 |
| Image up | Backlight back on | 612–616 ms after the request with a power-down, 370 ms from an already-off BCM [silicon S30, six boots]; backlight dark about 0.55 s |
| `HELLO` | Version check (major must match) [ours] | — |
| `OPEN` | The image switches to 150 MHz (PLL A), brings SDRAM up, opens the decoder. On `HP_E_NOMEM` it is back at 27 MHz and the host retries without `HP_OPEN_FAST_CLOCK` [ours]. SDRAM came up on attempt 2 (mode field 4) in every S28–S30 session, as in S10 (`OPEN` word `0x5344_0233`) [silicon] | budget 200 ms; ~12 ms expected (`hostproto.md` §4) |
| `FEED` … | Packets into the FIFO, a record each (§4) | — |
| `STATUS` | Counters and progress, polled (§8) | — |
| `CLOSE` | Ends the session; the image keeps running and SDRAM stays up [ours] | — |

The audio must not start before the image is up, or the first pictures are late by the boot time: in S22 audio began while the image was still booting, and picture 0 went up about 520 ms late [silicon S22, read-only analysis]. Our client counts boot, `HELLO` and `OPEN` as `boot_us` and aims the first post past the audio by it [ours].

### Restart, reload, hand back [ours]

- **Restart** = `CLOSE` + `OPEN` on the running image. It is the current image's only way to drop queued packets or start at another FIFO place (`SEEK`, `PAUSE`, `CLOCK`, `STATS` and `SLEEP` answer `HP_E_UNKNOWN`). Used for every seek, dropped GOP and still-picture recovery. An `OPEN` clears the overlay (§6), so the host re-sends it.
- **Reload** = power cut and a full boot (§3 timeline), then a session where the audio is. It clears state a restart keeps: the VRF, the image's statics and the SDRAM set-up (`CLOSE` already drops the clock to 27 MHz and `OPEN` raises it). In S30 a stall that a pause and a restart did not cure was cured by leaving the video and coming back, which is a reload [silicon S30]. dev36 escalates by itself: a restart that has not moved the picture 1 s later becomes a reload (`VPC_STILL_RELOAD_US`); three restarts with no picture between give up (`VPC_STILL_MAX`).
- **Backlight sleep**: Rockbox cuts the BCM's power with the backlight [rockbox, FS#9890]. The wake is a reload plus a seek to the audio; it held sync in S29 and S30 (stage 7 step 6: drift 22 and 36 ms) [silicon].
- **Which image a stop hands back to depends on which image played** [silicon, sitting 4b; ours]. A device-test play (a bench stage) boots the video image from its file, so its stop is the generic hand-back to Apple's program (`dt_restore(false)`: the stock wake path). Our job driver then has no jobs, so the shell's chip jobs stay off (the visualizer runs on the CPU, the panel decodes on the PP) until the next start of our resident image. The resident hand-back (`bcm_video_hand_back`: `CLOSE` acknowledged, idle gate, `dt_restore(true)`, a full-frame update) applies only when the resident image itself played. Consequence for a bench: put every step that needs the chip's jobs before the first step that swaps the program (`quirks.md` #48).
- **Hand back**: `CLOSE`, power-down in the stock order (the image's `LCD_SLEEP` handler ends any session with `HP_EV_LOST`), cut, then the host's own wake path (Rockbox's `bcm_init` with the NOR `vmcs`) repaints its UI.

### The resident image [ours; the merged image has run resident on silicon since sitting 2, 2026-10-25]

The video image can also be the host's display driver between sessions, because it answers the same command word as the boot image (`LCD_UPDATE`, `LCD_UPDATERECT`, `LCD_POWERON`, `LCD_SLEEP`, the ready word `0x5042_0001`, the info word at `0x1E8`) from the same panel code. Then the lifecycle above loses its power steps:

| Step | Resident | Separate image (above) |
|---|---|---|
| Boot | once, by the host's boot path, checked like any image (ready, info magic, panel ID, first frame with the panel on); every backlight wake uploads it again from the host's copy (the cut at sleep is unchanged) | at every session start |
| Take the panel | stop the host's LCD driver; clear the idle gate (below) | the same |
| Start | `HELLO`, `OPEN` (no power-down, no upload, backlight left on) | power-down, ≥ 50 ms off, upload, ready, first frame, `HELLO`, `OPEN` |
| Stop | `CLOSE` (it must be acknowledged: an open session with `HP_OPEN_OWN_DISPLAY` drops the driver's updates), set the idle gate, give the panel back to the driver, which sends the whole frame (`LCD_UPDATE`) over the last picture | `CLOSE`, power-down, cut, the driver's wake path |
| Dark, reload, give-up | as above: a cut and a full boot; after a reload of the same bytes the stop still hands back | the same |

A re-attach reads the ring indices from the header, so the host's client state is rebuilt from the running image; `OPEN` resets the FIFO's order and the overlay. `CLOSE` drops the clock to 27 MHz and puts the panel's bus timing back for it; SDRAM stays initialised (and refreshed) until the next cut.

**Idle gate:** the image sleeps its core between commands only while the host has written `0x534C_5031` (`"SLP1"`) to `0x16C` (`BCMFW_IDLE_GATED`); the word is zero after every upload, and the host keeps it zero during a session. Counters (timer compare 1 ticks, doorbells, sleeps, sleeps that returned with r0–r5 changed) sit at `0x1D4`, `0x1D8`, `0x1DC`, `0x1FC` in this image (`0x140`–`0x14C` hold its clock record). Whether the tick or the doorbell wakes the core on silicon is open ([`open-questions.md`](open-questions.md) #3), so our host leaves the gate clear by default.

### Panel standby [ours; stage 5s spike, not yet run on silicon]

`BCMFW_LCD_STANDBY` images add command `0x43` (`LCD_STANDBY`). The host writes `0x5342_5900 | depth` to `0x1C8` (1 light, 2 deep), then sends the command.
- The image runs the depth's dark sequence and leaves GPIO 48 (the panel's reset) high.
- `0x1CC` then reads the depth, with bit 8 set if the request was refused: no depth was asked, the panel is off, or this panel has no light sequence.
- The next `LCD_UPDATE`, `LCD_UPDATERECT` or `LCD_POWERON` wakes the panel without the reset pulse, its wait or the bus resync.
- `LCD_SLEEP` from standby only drops the reset line.
- `0x1D0` holds every panel power-on's time on the 1 MHz counter, with bit 31 set when it came out of standby.
- These are the probe's words: no image has both.

Sequences, open questions and the test: [`lcd.md`](lcd.md) §5 and [`open-questions.md`](open-questions.md) #49.

## 4. Feeding compressed pictures [ours]

- **FIFO**: 1 MB at SDRAM offset 0 (`0xC000_0000` host, `0xFE00_0000` core). Address and size come from the header (`HP_FIFO_BASE`/`SIZE`) and the `OPEN` reply.
- **Packets** (one access unit in Annex B form, parameter sets prepended at a key) start 64-byte aligned, in order: packet k+1 at `align64(end of k)` modulo the size. A packet may wrap the end; the image linear-copies a wrapped packet into a 256 KB area at SDRAM `0x30_0000` before decoding, so **a wrapped packet must be ≤ 256 KB** (larger ones are a decode error). A `FEED` at any other offset gets `HP_E_FIFO` with the expected offset.
- **Write ahead, then read back before a close `FEED`** [ours; silicon symptom, sitting 4b]. Put a packet in the FIFO well before its `FEED` (the player does: its packets go in ahead of their posts). When a `FEED` follows its packet closely, read the packet's last word (and the first) back through the port first; the read is behind the write on the same port and so orders it. `bvp_feed` does this for the newest packet when it is posted as soon as it is written. A model of the chip must not let the core see bytes the host has not yet read back or let settle: the host-test model refuses such a `FEED` (`VBM_LAND_US`: 2 ms, or 4 KB written after them). Mechanism [hypothesis]: posted SDRAM writes (`quirks.md` #47, `open-questions.md` #73).
- **`FIFO_RD`** counts 64-byte units consumed, encoded, in a 16-bit wrapping counter (2¹⁶ × 64 B = 4 MB, four times the FIFO, so never ambiguous). The image advances it only after the packet is **decoded**, so it doubles as a progress signal. Free space = size − (bytes written − 64·`FIFO_RD`).
- **Queue depth**: 16 records in flight (`OPEN` reply word). A 17th gets `HP_E_BUSY`.
- **The core reads the FIFO at `0xFE00_0000` with its cache off** and sees host writes at once [silicon S10, S12]. A design that enables the cache must flush before reading host-written data ([`boot-and-clocks.md`](boot-and-clocks.md) §2).
- **One-picture lag.** Our decoder closes a picture only when the next picture's first slice arrives (or at `FEED_EOS`, which flushes). A session fed one packet shows nothing; each picture appears when its successor is decoded. Any Annex B decoder without access-unit delimiters or an explicit "end of picture" behaves this way; the host's lead must cover one extra frame.
- **Hidden pictures** (`HP_FEED_HIDE`, minor 4): decoded for reference, never shown and not counted in `shown`. Used for the frames between a seek's key and its target (§5).
- **Flags**: `KEY` on sync samples, `EOS` on the last (answered by `HP_EV_EOS` once the last picture is on the panel).

## 5. Clocking and A/V sync

The host plays audio; the BCM only shows pictures. Our design keeps the clock on the host and paces the posts, so the image presents each picture as soon as it is decoded [ours]. The facts below bind any design that does the same on Rockbox.

**Thread priority** [rockbox; silicon S26–S27]:
- While the PCM buffer refills after a start, seek or wake, pcmbuf boosts the codec thread (down to priority 5, typically 10–14 in the S26–S30 traces).
- A feeding thread at a fixed `PRIORITY_PLAYBACK` (16) then barely runs: no posts for 420–520 ms about 100 ms in, and a GOP dropped at every start [S26].
- Following the codec's priority fixed it: longest loop gap 12–13 ms [S27]. Our client follows only for 3 s after a start, seek or wake, and never above `PRIORITY_PLAYBACK_MAX + 3` (8), so a starved PCM still wins later [ours, dev33].

**The audio clock** [rockbox; silicon S23–S26, S30]:
- `id3->elapsed` is the DAC position at the start of the PCM chunk now playing (pcmbuf stamps chunks of 8 KB, about 46 ms). Extrapolate from its last change, and only while the playback mixer channel is really playing (`mixer_channel_status`). Before the PCM starts, extrapolation runs ahead: S23 showed picture 0 exactly 200 ms late at the extrapolation's cap.
- The prefill's chunks carry the decoder's position, ahead of the speaker: about 100 ms in, `elapsed` leaps about 430 ms, then drains back over about 1 s [S24, S25]. Following it drops a GOP at every start.
- A clock that runs on wall time from the PCM's start, pulled toward the `elapsed`-based estimate by at most 1/20 of the time passed, and snapped only past 1.5 s, holds within ±25 ms of the estimate from the first 50 ms (S30 trace) [ours, dev28].
- A stalled stream's estimate holds after 600 ms rather than running on [ours].

**Seeks and jumps:**
- H.264 can start only at a key. The frames from the key to the target must be decoded but not shown. If they are shown, the head of the GOP plays out late: 1.5–3.5 s behind after a fast forward, a resume and the end seek [silicon S28].
- Aim a seek past the audio by the catch-up time. With n₀ frames from key to target, per = decode time per picture, fr = frame period: s = n₀·per·fr / (fr − per), capped at 1 s (and the cap whenever per ≥ ¾·fr) [ours, dev32]. After this, drift after a jump stayed at 15–36 ms [silicon S29, S30].
- A seek's first picture came 0.3–1.5 s after the seek, and at most 173 ms late except once (769 ms) [silicon S30].
- Detect the audio's own jumps (a held fast forward, a repeat) by comparing the clock's advance with wall time: back by more than 300 ms or forward by more than 1.5 s [ours].
- A resume a little past the pictures (the PCM ran on through the pause) needn't go back to the key. In 3 s GOPs, the seek went about 90 pictures back. Its first picture came 1.5 s later and 512 ms late, and the GOP's tail dropped (stage 7 step 2: drift 303 ms) [silicon S35]. When the key before the audio is behind the pictures already sent, decode on from there, hidden, up to the audio. Aim past it by the same formula, with n₀ counted from the last picture sent [ours, dev42; not yet run on silicon].
- A re-push after a dropped GOP takes time inside the post; count it as lateness, not as a path error, or the lead drifts (every picture 50–70 ms early after one drop) [silicon S29, fixed in dev34].

## 6. Panel ownership

- **During a session the image owns the panel** [ours]. With `HP_OPEN_OWN_DISPLAY` it acknowledges `LCD_UPDATE`, `LCD_UPDATERECT` and `LCD_POWERON` without drawing (`bcmfw/main.c`). The host's own LCD driver is held off as well (§3), so anything the host UI draws reaches nothing.
- **Overlay** (`HP_OP_OVERLAY` `0x0C`, minor 4, `HP_CAP_OVERLAY`, dev35) [ours]. The host writes its pixels into its frame at `0xE0000` (panel coordinates, 320-pixel rows) and sends the rectangle. The image blits it at once and again after every picture, until an `OVERLAY` with zero width or height, which puts black borders and the last picture back. While playing, each picture briefly covers the strip before it is redrawn. **The slide (2026-10-15, `HP_CAP_OVERLAY_SLIDE` `0x1000`) [ours].** An optional fourth word, `HP_OVL_SLIDE_UP` (`0x10000`) or `HP_OVL_SLIDE_DOWN` (`0x20000`) with the duration in ms in the low 16 bits, makes the image move the band itself, by the 1 MHz counter, eased out (1 − (1 − t)³), stepped from the main loop (`video_anim`). Up: the band rises from its rectangle's bottom edge (from wherever a running slide has it). Down: the band that is up sinks and goes, as a zero-width `OVERLAY` would at the end. The image puts the last picture (`VIDEO_FB`) back under the rows the band leaves, with black where the picture doesn't reach, so a slide works while paused too. A plain `OVERLAY` during a slide up leaves the slide running, so the host can refresh the band's pixels. Without the counter (`BCMFW_INFO_TIMER_OK` clear) or with 0 ms, both happen at once. The host sends the band's pixels once per change; each step is a panel blit on the chip and costs the PP nothing (`vplayer_overlay_slide`).
- **Black first**: an `OPEN` resets the picture geometry, so the session's first picture blacks the whole panel before it is drawn [ours].
- **Dark time**: the takeover's power cycle leaves the backlight off for about 0.55 s (§3) [silicon S30]. The backlight is the host's (`_backlight_hw_enable`), not the BCM's.
- **Tearing**: pictures go out by DMA without waiting for TE, as the UI does ([`lcd.md`](lcd.md) §6) [ours].

### 6a. One overlay region, two overlays [ours; vc2sim, not yet run on silicon]

The release line runs the merged image, whose spare SRAM holds **one** overlay region (`0x12B000–0x133FFF`, 36 KB). Two programs are linked for it against the merged image's map: DECODE (`dec-ovl.bin`, the JPEG and album decode) and the visualizer (`viz-ovl-m.bin`, SPECTRUM and BARS). A host driver for it must follow these rules (design: `../research/bcm2722/visualizer.md` §8.5, `album-render.md` §5.3):
- **A load evicts the other.** The chip drops the other overlay's ops at `OVERLAY`. The driver marks the evicted overlay due again at once, so the two alternate without spending retries.
- **Decode wins on demand.** A track change's DECODE evicts the visualizer; the view holds its last frame (no draw, no CPU analysis) while its reload is owed, and reloads after the decode (three 8 KB pieces, about 50–110 ms in the simulator). A visualizer load that meets a decode in flight is refused `BUSY` without spending a try.
- **Who reads, who puts.** Both files are read on the art thread (no file read on the UI thread) and put piece by piece by the UI pass, which is the job owner. Both kinds go through one handshake, so a decode load, its decode and a reload are never interleaved.
- **TONE is refused while any overlay is live** (`HP_E_BUSY`; its buffer is inside the region).
- **Bounds and nets.** Past 3 s without the overlay back the CPU draws, logged loudly (`CPU FALLBACK why=wait`); a missing overlay for the running image (`why=noop`) and a refused load (`FAULT bcm viz overlay not loaded`, then `why=lost`) are loud too. The CPU path is a last resort, never the plan.
- **Pairing.** An overlay runs only in the exact image it was linked against (the ABI word); the packaging step refuses a mismatched pair (`bcmfw/ovlpair.py`).

## 7. Performance

| Quantity | Value | Evidence |
|---|---|---|
| Decode, convert and push, 320×240 Baseline, 150 MHz, synthetic clip | 7 ms a picture mean (max 31) | [silicon S27] |
| The same, real footage (pans, cuts, 24 fps) | 18 ms mean (max 39) | [silicon S27] |
| The same, a converted episode excerpt (stage 7), per `FEED` | 14.8–14.9 ms mean, 46 ms max | [silicon S29, S30] |
| The excerpt in vc2sim | about 17 ms simulated per picture | `device-session-1.md`, session 28 |
| All decoder kernels in scalar C instead | 31–35 ms a picture | [silicon S15] |
| Full-frame push to the panel by DMA (overlapped with decoding) | 14.26 ms | [silicon S7] ([`lcd.md`](lcd.md) §6) |
| At 27 MHz (no PLL) | about 70 ms a picture for 320×240, about 14 fps | [hypothesis] from the 5.56× clock ratio ([`boot-and-clocks.md`](boot-and-clocks.md)) |

**Idle**: the video image busy-polls its command word and ring between packets (`BCMFW_IDLE_SLEEP` is 0 in `bcmfw/mkvideo.py`) [ours]. An idle-sleep build (timer compare 1 every 1 ms, optionally the host doorbell, source 23) exists for the UI image, and no session has run it yet. Power draw in any state is unmeasured ([`open-questions.md`](open-questions.md) #3, #12).

## 8. Failure modes and their signatures

What the host can see: the `STATUS` reply (decoded, shown, errors, concealed, queued, the PTS on screen, the last `FEED`'s time on the chip, the core clock), `FIFO_RD`, the heartbeat in `HP_STATE` (8 bits of a 1.024 ms count: it wraps every 262 ms, so look more often than that), and the image's progress words [ours]:

| Word | Contents [ours] |
|---|---|
| `0x1C4` | phase `<< 24 \|` detail: `0x0F` idle (packets done), `0x20` packet, `0x21` wrap copy, `0x30` NAL (type), `0x31` macroblock (address), `0x32` loop-filter row, `0x50` convert, `0x51` blit, `0x5F` shown |
| `0x1C8` | packets taken |
| `0xDF000` +4·48…55 | the session's refusal (`"RF"`) or, from dev37, its first decode error (`"ER"`): the check (file `<< 16 \|` line), the NAL's type, length and first 8 bytes, the packet's offset, length and first 4 bytes |

| # | Signature | What was seen | Cause | Recovery that worked |
|---|---|---|---|---|
| a | **Refusal at the first NAL** | `HP_E_UNSUPPORTED` (`why=5`), guard "2 checks": the SPS refused. The file's avcC was byte-identical to one that played [silicon S28] | open ([`open-questions.md`](open-questions.md) #29) | none yet; dev32 retries once from the file |
| b | **Nothing at a file's first sample** | The session started at sample 0 (SPS, PPS, a 701-byte x264 SEI, a 239-byte IDR, then 7-byte P_Skip pictures). The chip took every packet (0 in flight; progress `0x0F` idle), decoded 0, counted `0x22`/`0x33` errors, about one per packet. Restarts at later keys played [silicon S29, S30] | open (#30) | a restart at the next key |
| c | **"N in flight, shown stuck"** | The chip still beats and answers `STATUS`, but takes packets slowly or not at all. Progress snapshots land inside a macroblock (`0x3100_0048`: 18 decoded, 16 in flight, 14 queued). Another snapshot: NAL type 5 with 1 packet taken [silicon S30, `ipodui.log`] | open (#32) | a restart at the audio's position [S30]; a reload where a restart did not help |
| d | **Grey pictures** | The decoder's concealment after decode errors; cleared by the next IDR (every 3 s in these files) [silicon S30, owner] | the errors' cause is open (#30, #32) | the next IDR |
| e | **Held last frame** | The chip stopped presenting. The owner saw the freeze at the same spot on replays, so it is deterministic [silicon S30, owner] | open (#31) | a reload (leaving and re-entering) cured a stall in the same run |
| f | **Nothing from a one-packet session** | The one-picture lag (§4) | understood [ours] | feed ahead, or `EOS` |
| g | **"Sample source failed" at a resume far into a file** | Three plays resumed at 18:15 of a 21-minute episode stopped within 0.5 s with `why=-4`, with nothing pushed but control traffic (5 KB through the port). The first push after the seek, at the next key (sample 32,850, 27,772 bytes), was the one that failed. The `.vix` and the demuxer are byte-identical on the host, and the simulator plays the same seek [silicon S35] | open. Leading hypothesis: the audio's own seek resets its buffer between the sample's probe (its last byte buffered) and its pieces, so a piece reads "not buffered" and counted as a failure | dev42: such a piece drops the packet and waits; any other source failure logs why and moves on to the next key, three times without a picture, then gives up |
| h | **Packet taken, no picture, no error** | A `FEED` sent the moment its packet was written: `MB_PROG` idle, the image still answering `STATUS`, 400 ms of waiting, at every clock band [silicon, sitting 4b] | a late SDRAM write seen after the command (mechanism [hypothesis], `quirks.md` #47, `open-questions.md` #73) | write ahead, read the newest packet's last word back before a close `FEED` (§4) |
| — | Torn index read as a dead port | Valid heartbeat, junk in one index word [silicon S11, S12] | client bug (§2) | encoded indices |
| — | Header and mailbox overwritten with chroma-like bytes | `0x100–0x1FF` overwritten in 8-byte runs 160 bytes apart [silicon S12–S14] | a vector store with base r7 (`quirks.md` #8) | fixed in the image |

The failures above came only from real converted files (S28–S30). Synthetic and short real clips played bit-exact against ffmpeg [silicon S27], and the image decodes the S28–S30 streams pixel-exact in vc2sim, so a/b/c/e are differences between silicon and the simulator, or between the chip's state and the model's, that are not yet located.

### 8a. The per-start field line, and forcing a job to fail [ours]

**One line per BCM start.** Beside the older `bcmjob vpu=0x3 expect=0x3`, the driver logs (release builds too, so field logs carry it) `bcmjob start vpu=0x3 expect=0x3 vpu2=0xff lat=286/136@150 latw=210/98`. `vpu2` is the merged image's `J_VPU2` (`0x54`); when a form disagreed, `fail=<form> lane=<n> got=0x.. exp=0x..` follows (`J_VPU2_FAIL`/`GOT`/`EXP`). `lat=` is `J_LAT_SDRAM`/`J_LAT_SRAM` byte-load times (ns x 10) at `J_LAT_MHZ`, `latw=` the word loads; they appear only once the image has measured them (`J_LAT_DONE`, after the first OPEN), so most start lines omit them. An image that predates the words leaves them 0: `vpu2=0x0` and nothing after. That is one to three port words (8.9 us each) per start. `scripts/bench_ingest.py` lists the line per start under "BCM starts".

**Forcing a failure (developer builds only).** Developer > Advanced > Video chip > Fault injection holds two rows, "fail visualiser frames" and "fail decodes", each stepping 1, 2, off. They arm the driver's `bcmjob_dev_inject_fail` for the next N jobs of the kind (the visualiser's SPECTRUM, BARS and line ops; DECODE): the real reply path returns the BCM's error, so the driver logs `bcmjob fault op=0x.. why=failed`, the visualiser logs `CPU FALLBACK` (once) and draws on the CPU. The count expires by itself once its jobs have run; "off" disarms what is left and also forgives the op (clears its failed mark and count), so the return to the chip is shown without a reboot. One failed job is logged and the op tried again; two reach `BCMJOB_FAIL_MARK`, which marks the op failed for the rest of the boot (the CPU path stays until "off"). The simulator's model has the same hook (`jmodel_fail_next`) and a script command, `bcm_jobfail viz|decode <n>` (`tests/ui/ipodui-p19-jobfail.script`). Both compile out of a release build.

## 9. Gotchas, in one list

- Every BCM wake is a cold boot of about 0.37–0.62 s, panel dark; plan the UI around it (§3). An image that is also the display driver (§3, "The resident image") removes it from session starts, not from backlight wakes.
- Never treat a torn word as a port failure; encode what you poll (§2).
- Don't start audio before the image is up (§3).
- Keep the feeding thread level with a boosted codec during refills, and only then (§5).
- Don't trust `elapsed` for the first second after a start: slew, don't follow (§5).
- Decode a seek's catch-up frames hidden, and aim past the audio by their decode time (§5).
- Expect a one-picture lag from an Annex B decoder (§4).
- Bound every host wait, and give the chip a way back: restart, then reload, then give up (§3).
- Read the heartbeat more often than its wrap (§8).
- Write packets ahead of their `FEED`s and read the newest back before a close one (§4, `quirks.md` #47).
- Every chip wait has a bound and a FAULT line (§2a). A wait with no bound is a frozen PP with no record.
- A stage play leaves the chip on Apple's program: chip-job steps go before any program swap (§3).
- One overlay region: a load evicts the other, and TONE is refused while any overlay is live (§6a).
- **MUST, with the display jobs** (`docs/research/bcm2722/jobs.md`): never redraw a rectangle you have just presented until its push is done.
  - Why: `PRESENT` starts the panel DMA and returns. The next jobs can draw into the same frame while the DMA is still reading it, which shows as a tear [ours; the job executor's cost audit, 2026-10-24].
  - How: put a `FENCE` after the `PRESENT`, or present with `J_PRES_WAIT`, before any job writes into that rect again.
  - P19 also keeps this as a shell rule (the P19 brief §3.6, "screen truth").
- **Two cores, one port** (P19 19-9 puts the ring on the COP). A lock both cores take must live in uncached shared memory (`SHAREDBSS_ATTR`), and so must every flag both cores write; a cached static gives each core its own stale copy [ours; the COP-ownership audit, 2026-10-24].
- **Cache maintenance is per core** on the PP5022: the core that wrote a buffer cleans it before another core, or a DMA engine, reads it; a reader on the other core invalidates first [pp5022-reference; ours].
- **The pixel hand-off across cores** (BCM MF-4; built and host-tested, `tests/lcdxcore`; not yet run with the UI on the COP) [ours; host model]. The core that draws the framebuffer is its producer; any push of it from the other core is a consumer:
  - **The producer writes back before the consumer reads.** A CPU push asks the COP for it (`fe_fb_publish()`, bounded) before it takes the port lock, never under it; a hand-off where the COP then waits on the CPU (the video overlay) is published by the COP before it posts.
  - **The consumer reads through the uncached alias** (`lcd_fb_read()`), for its port copy and for engine 2 alike, so it never reads a stale line of its own.
  - **Engine 2's source is 16-byte aligned at both ends**: only full-width rects go to the engine, and the frame's 640-byte rows are whole cache lines.
  - **A fence that times out pushes anyway**, uncached: at worst stale pixels for one push, logged (`fb fence timeout`), never a hang and never corruption.
- **The decode hand-off across cores** (P19 S10, `bcmjob-core.c`'s state table; built and host-tested, not yet run with the owner on the COP) [ours; host model, tests/bcmjob]. For a call that one thread posts and the ring's owner runs on the other core:
  - **Keep the call out of the caller's frame.** Put its parameters and its answer in the uncached request record. The caller copies them in before it posts and out after the owner's done word. The owner then never writes into a stack that may already be dead.
  - **Only one side writes a given buffer.**
    - The caller writes the file pieces and the owner only reads them, so the caller also pads the last word, and publishes each piece before it says the piece is there.
    - The owner writes the read-back rows and the caller only reads them. The caller publishes the rows' span before posting, so none of its dirty lines can land over them, and acquires the span after the end.
  - **The owner reaches the caller's memory through the uncached alias, and engine 2 reads memory directly.** So the owner's own cache never holds a line of the caller's, and the owner never needs cache maintenance.
  - **Edges:** a read-back row can start or end in a cache line it shares with other memory of the caller's core. The owner puts those pixels in the uncached record instead, and the caller copies them in on its own core. This removes the alignment requirement for read-back buffers. Piece buffers are 16-byte aligned at both ends.
  - **Giving up needs an acknowledgement.** The owner sets busy, then checks quit; the caller sets quit, then checks busy. Both sides fence between the two steps (Dekker; uncached accesses are in program order on these in-order cores, the same thing Rockbox's Peterson corelock relies on). Once the caller sees busy clear after its quit, the owner never touches the caller's memory again. One writer per word, so no compare-and-swap is needed.
  - **Bound the wait** (`BJC_ACK_MS`). If no acknowledgement comes within it, keep the memory (the piece buffer stays pinned) and log loudly, rather than freeing it under a running pass.
- **Read back from the BCM only what is small.** Port reads cost about 55–85 µs a word under load [silicon, device logs], so a 142 × 142 box (about 10,000 words) is fine, but a full panel read-back under load could take seconds. Rule (the orchestrator's decision, 2026-10-24): `bcmjob_read()` is for box-sized results only, never a whole panel; a panel stays on the BCM and is shown from there, or is made on the PP.
  - **And never on a path the user waits on** (the orchestrator, 2026-10-24): a 142 × 142 box read back under load takes 0.55–0.86 s (hundreds of owner passes at the per-pass budget), fine for the background art builder only. Anything the user is waiting for is shown from the BCM's slot, or made on the PP.
- **Engine 2 from the COP is unproven.** Apple's transfer object refuses to run on the COP, for a reason not yet known. Until the bench row passes on silicon, start engine 2 from the CPU, or give the COP a CPU service for pixel pushes [hypothesis].

## 10. The first-use video check [ours; run on silicon in sitting 4b]

The token that opens 150 MHz names an **image**, not a unit, so a release already runs every unit at 150 MHz (Apple ran every part there [apple]). The check is a per-unit safety net: it catches the rare unit with marginal silicon before a whole film plays wrong on it. Full design and as-built notes: `../research/bcm2722/first-use-check.md` (`apps/video/vcheck.{c,h}`).

**What it does.** Once per unit and image, inside the first play's start-up (never at a reload, a wake or a restart; never in a bench play), after `HELLO` and before the play's `OPEN`, it runs the merged image's own release path one clock band at a time, highest first:
1. `OPEN` 320×240 (the top band by the play's own path to 150 MHz; a lower band by `HP_OP_VCLK` after `OPEN`), then check the band's clock and that the chip fell back nowhere.
2. Cover the panel with an overlay, so the test pictures never reach the glass.
3. The SDRAM readback (16 blocks of a pattern across the FIFO, 4 words each: 0 bad).
4. A built-in 30-picture test stream (22,672 B, `vcheck.bin`; ffmpeg's `testsrc2`, Constrained Baseline, one picture a packet): all packets into the FIFO and their first and last words read back, then one `FEED` at a time with the player's way of waiting (`FIFO_RD` and one `STATUS` in flight, at a millisecond's pace).
5. Read each picture's planes over the port and sum them on the PP (FNV-1a) against the table from ffmpeg's decode, with 0 decode errors, 0 concealed blocks, the band's clock, and each `FEED` within two frames (the mean for P pictures within 25 ms).

**Verdicts.**
- **PASS** at the first band that holds: the unit token (image FNV, band, date, build) is written whole, synced and put in place by a safe replace. A band below the top caps this play and every later play while the token stands.
- **FAIL** at every band: the play fails with the check's error and the styled error screen asks for a Save Diagnostics report.
- **INCONCLUSIVE** (the image's words not where they should be, the FIFO refusing the stream, a packet taken with no picture and no error, a missing or wrong stream file, no memory): logged loudly, **no token**, the play goes on at the image's clock and the next play checks again. A bug in the check cannot lock a good unit out of video.
- **Budget:** at most one reboot of the chip per band (a band whose chip stops answering fails; the image is cut and booted before the next band), and a 4 s budget for the check as a whole; the model predicts 0.8–1.2 s, the plane readback dominating ([estimate]; the port's read cost is unmeasured). An image without a `clock150` token plays at 27 MHz and is not checked.
- **Live invalidation:** a decode error or concealed block (a refused stream is the file's fault and does not count), a clock fallback or a lost image drops the token at once and steps the play to the next band; the next play checks again.

**Sitting 4b's result and the fix** [silicon, 2026-10-26, build 1663b48a25, merged image `23693ff4`]: the first version failed at every band on every clip: the image took each packet, decoded nothing and kept answering `STATUS`. It was not the bench context (a normal play would have done the same), and replaying the old sequence against the image in vc2sim with late SDRAM writes gives the same line. The cause is the late write of §2 and §4 (`quirks.md` #47; mechanism a hypothesis, with open question #73's second reading still not excluded). The fix needed no image change: all packets in and read back before the first `FEED`, the player's wait, and a "packet taken, no picture" outcome that is inconclusive at once (about 25 ms, not a 400 ms timeout and a reboot). The ladder of bands below the top (147, 140, 130, 123, 116 and 102 MHz) is unmeasured and marked pending until the voltage-band bench says which hold.
