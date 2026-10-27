# The video player on the PP (plan)

Issue #60 (the BCM side) and #64 (the PP-side driver, coordinated with the main session). This is the plan for everything on the PP5022 between a video file on the flash and `FEED` records for our VideoCore image. The BCM side (decoder, conversion, DMA) is in `pipeline.md`, and the wire format in `hostproto.md`. **Nothing here is in the firmware yet.** The one piece of code is a host prototype of the demuxer, `tests/video/mp4demux.py`, checked against ffprobe and ffmpeg by `tests/video/mp4demux_test.py`.

**Status (2026-10-11):** the plan, plus the demux prototype. The test has 57 checks and 0 failures, and runs in about 2 s:
- **Files:** five corpus clips muxed with an AAC track, both moov-first (`.m4v`, `+faststart`) and moov-last (`.mp4`), plus one rewritten with `co64`.
- **Sample table:** offset, size, PTS and key flag match ffprobe for every packet.
- **Decode:** the Annex B output decodes to the corpus reference frames, bit for bit.
- **Seek:** a seek from mid-file starts on a key sample and stays exact from there.
- **Index:** the sidecar index round-trips.

**Built (2026-10-12, branch `p14-player`):** pieces 1, 3, 4, 5 and 6 are code behind `HAVE_BCMFW_VIDEO` (`tools/configure --bcmfw-video`), with host tests in `tests/video/run.sh` and the whole player running in the simulator (`tests/video/sim_run.sh`). See "As built" at the end. Nothing calls the player until the Videos screen (piece 7) exists, and it has not run on the device.

## Shape of the design

```
sync time (tagcache thread, after USB)          play time
--------------------------------------          ---------
scan .m4v/.mp4/.mov with a 'vide' track         Videos list -> the video screen
  read moov once (bulk)                          audio: Rockbox playback plays the file
  classify: stik / tvsh / ...  -> Videos lists          as an ordinary AAC track (master clock)
  check avcC: Baseline, <= 640x480               video: the player thread
  write the sidecar index (.vix)                   index window <- .vix (bulk)
                                                   sample bytes <- the audio's buffered
                                                                   handle (no second read)
                                                   Annex B into the BCM FIFO (host port)
                                                   FEED records paced by the audio clock
```

Three decisions carry most of the weight:

1. **The audio path plays the file; the video rides along.**
   - **What already works:** Rockbox already plays the AAC track of an MP4. `libm4a`'s demuxer skips non-sound tracks (`lib/rbcodec/codecs/libm4a/demux.c`, "MP4 video files normally put a video trak before the AAC trak").
   - **What we get for free:** resume, bookmarks, speed (timestretch), gapless handling, the Now Playing state and the power policy.
   - **The video player:** a client that follows the audio clock. It never owns playback.
   - **Why:** Apple's design (`approach.md`) and our hostproto §3 ("audio is the master clock") agree on this.
2. **Everything about the file is computed at sync time.** A sidecar index, 12 bytes per sample, is written once, when the library updates (`docs/hardware.md`: "pre-compute at sync time, never at browse time").
   - **At play time:** the player reads the index in bulk windows. It never parses `stsc` or walks `moov`.
   - **Fallback:** parsing at open, for a file that isn't indexed yet.
3. **The file is read once.**
   - **One reader:** video samples come out of the same buffered handle the audio codec reads, through a small read-only accessor.
   - **Why it matters:** a second handle on an interleaved file would read every byte twice. At 768 + 128 kbit/s, that's about 6× the audio's own I/O.

## 1. MP4/M4V demux

### What the files look like

iPod-era files come from iTunes, HandBrake's iPod preset, or ffmpeg with `-f ipod`, and share these traits:
- **Video:** one `avc1` track (Baseline, ≤ 640×480, level ≤ 3.0).
- **Audio:** one `mp4a` track (AAC-LC).
- **Interleave:** the two are interleaved in chunks of about 0.5–1 s.
- **Layout:** `moov` either first or last. No fragments.

The prototype handles exactly that and refuses the rest with a logged reason:
- fragmented files (`moof`/`mvex`): accepted later if real files need it;
- empty edits (a presentation delay);
- a missing `avcC`.

MPEG-4 Part 2 (`mp4v`) is the BCM's codec 2, which comes later. Until then, those files are classified but not offered (§8).

### The sample table, flattened

| Box | What it gives | Notes |
|---|---|---|
| `stsz` | sample sizes | one fixed size, or one entry per sample |
| `stco` / `co64` | chunk offsets | 32-bit, or 64-bit for files of 4 GB and more (FAT32 caps files at 4 GB − 1, so `co64` appears only from other writers; it's handled, and tested by rewriting a real file) |
| `stsc` | samples per chunk, run-length coded | expanded once: a sample's offset is its chunk's offset plus the sizes of the samples before it in that chunk |
| `stts` | decode durations, run-length coded | DTS in media ticks (`mdhd` timescale; ffmpeg writes 15,360 or 1,200,000, and iTunes 600 or 2,997) |
| `ctts` | composition offsets | Baseline has no B-frames, so it's normally absent; supported anyway (version 0 and 1) |
| `stss` | sync samples | no `stss` means every sample is a key sample |
| `elst` | edit list | the first edit's `media_time` is subtracted, as ffmpeg does; empty edits are refused |
| `avcC` | NAL length size, SPS, PPS | length size 4 in every iPod file; 1 and 2 are parsed too |

**PTS in µs:** `(dts + cts − shift) × 1,000,000 / timescale`, which is exactly what the `FEED` record carries. The prototype matches ffprobe's `pts_time` within 1 µs (rounding) on every packet.

### Annex B, in place

`pipeline.md`'s driver contract, item 3, says how the conversion works:
- **Start codes:** each `[length][NAL]` becomes `00 00 00 01 NAL` while the sample is copied into the FIFO. With 4-byte lengths the size doesn't change, so there's no extra pass.
- **Parameter sets:** SPS and PPS from `avcC` go in front of the first packet, and in front of the first packet after every seek (about 30 bytes).

The prototype's `Movie.packet()` is the reference for that conversion.

### The sample index (sidecar, `.vix`)

**Format (prototype `VIX1`, little-endian, 32-byte header):**
- **Header fields:** magic, version, sample count, PTS units per second (1,000), `w | h << 16`, and the offset and length of `avcC` in the file.
- **Records:** one 12-byte record per sample:
  - the file offset, bits 0–31;
  - the size (24 bits) plus flags (key; offset bits 32–35, for files up to 64 GB);
  - the PTS in **milliseconds**. A u32 in µs would wrap after 71 minutes; ms holds 49 days. The ms PTS is converted back to µs for `FEED`, and ±1 ms is far below a frame.

**Size:**

| Content | Samples | Index |
|---|---|---|
| 22-minute episode at 30 fps | 39.6 k | 475 KB |
| 2-hour film | 216 k | 2.6 MB |

That's small on a 1 TB disk, and never read whole: the player reads the window that matches the bytes being buffered.

**Key table:** a separate small array of `(sample number, PTS)`, built from `stss` while loading. Seeks binary-search it. At GOP 300 a 2-hour film has 720 keys (6 KB), and at GOP 30, 7,200 keys (58 KB). It can be held whole.

**Where it lives:**
- the path: `/.rockbox/ipodui/video/<hash>.vix`. Hidden, like every shell file (CLAUDE.md rule 1);
- the hash: the file's path, size and mtime. A re-copied file gets a new index, and stale ones are swept at the next sync.

**Why a sidecar and not the precompute file:**
- `database_lib.dat` has 32 sections and 30 are in use (ROADMAP: "Library precompute file"). The Videos lists still need sections there, which means raising `LIBFILE_MAX_SEC` first, as that note says.
- Per-sample tables, on the other hand, are per file and large. They belong beside the file, not inside the library.

**Cost:** writing the index at sync time is one bulk read of `moov`. A 2-hour film's `stsz` alone is 864 KB, so the walk takes about 1–2 s on the PP (uncalibrated), on the tagcache thread, at low priority. Opening a file with no index pays the same cost once, behind the 7G's normal loading pause, and the player then writes the index for next time.

## 2. Feeding the BCM

### Where the bytes come from

**Today:** Rockbox's buffering (`apps/buffering.c`) holds the playing file in a `TYPE_PACKET_AUDIO` handle. The codec moves that handle's read index `ridx` as it consumes audio samples. Data behind `ridx` can be reclaimed: `shrink_handle()` moves a packet-audio handle's start up to `ridx`.

**The accessor (an upstream edit, `#ifdef HAVE_BCMFW_VIDEO`, about 30 lines):**
```c
ssize_t bufpeek_at(int handle_id, off_t file_pos, size_t size, void *dest);
```
- **What it does:** copies bytes that are already buffered, `[start, widx)` of the handle, without moving `ridx`.
- **Otherwise:** `ERR_HANDLE_NOT_DONE` if the range isn't buffered yet, or `ERR_INVALID_VALUE` if it's behind `start` (already reclaimed).
- **Locking:** it takes `llist_mutex`, the lock the buffering thread holds while it moves or reclaims handles (`bufread` and `bufgetdata` take none: they rely on the codec being the only reader). It never waits for data and never fills.

**Why it works:** the video side runs *ahead* of the audio. The FIFO holds up to about 10 s, and records go out about 35 ms before display. So the video's file position is normally ahead of `ridx`, inside the buffered window.

**Cases:**
- **Ahead of the buffered data** (`ERR_HANDLE_NOT_DONE`): the buffer is filling. Wait for the next fill, which the audio needs anyway.
- **Behind `start`** (reclaimed): only after a long FIFO stall, or a seek the audio did first. The player falls back to one bulk `read()` of a whole index window (≥ 256 KB) straight from the file, then rejoins the buffer. That's logged as a counter, not shown.

**Flash wake-ups (64 MB unit, about 60 MB of audio buffer):**

| Content | Buffer holds | Flash wakes |
|---|---|---|
| 768 + 128 kbit/s (iPod format) | about 9 minutes of film | once every ~9 minutes |
| 1.5 Mbit/s VGA | about 4.5 minutes | once every ~4.5 minutes |

The 32 MB unit has about half of that. This is the bulk-I/O rule working as intended (`docs/hardware.md`, "Buffering policy"). The audiobook rule ("end a fill on a chapter boundary") has a video counterpart: end a fill on a GOP boundary, so a seek within the buffered range never reloads.

### Into the FIFO

`pipeline.md`'s contract covers the details: 64-byte alignment, wrap, free space from `FIFO_RD`, and at most 16 packets in flight.

**Rhythm:**

| Stage | Data into the FIFO | Records |
|---|---|---|
| **A, today's image** | in bulk when there's room, about 800 KB every 8 s at 768 kbit/s | one per frame, when the audio clock reaches PTS − lead (35 ms at 320×240, 70 ms at 640×480). The PP wakes 30 times a second, on Rockbox's 10 ms tick, and posting a record costs 24 transfers |
| **B, with `CLOCK` and the display queue** (`hostproto.md` §3, `pipeline.md` "Next steps") | as in stage A | batches of up to 16 records about every 0.5 s. The BCM schedules frames against its own media clock, and the PP sleeps between batches |

Stage B is the race-to-idle form on the PP side.

**Cost on the PP:** under 3% of the unboosted 30 MHz core at 30 fps (measured in the simulator: `pipeline.md`, "Host side"). The bulk copy is the biggest item. It wants an assembly loop like `lcd-as-video.S` (two halfwords per word, address auto-increment) rather than C `bcm_write32` calls: that's a measurement item for #64.

## 3. A/V sync

- **Master clock:** the audio. Its position comes from the PCM position Rockbox already reports (`audio_current_track()->elapsed`, or the finer `pcm_get_bytes_waiting`-adjusted position the Now Playing scrubber uses). Video never holds the audio back.
- **Priming:** AAC tracks carry an encoder delay, normally shown by the audio track's `elst` `media_time` (1,024 samples, about 23 ms at 44.1 kHz). The video PTS is already edit-adjusted (§1). The audio clock must be too: check whether Rockbox's `elapsed` counts the priming samples. If it does, subtract them. **Device check:** a clap or flash clip, timed by eye or with the owner's camera at 240 fps.
- **Drift:** `STATUS` words 11–12 give the PTS on screen. The player compares it with the audio clock when the picture went up, and subtracts how late that frame's record went out (start-up and a full BCM delay records; the lead can't fix those). What's left is the record-to-screen path, which the lead absorbs:
  - **First status after an open, seek or resume:** the whole error at once (the path is a constant of the chip and panel).
  - **Then every 500 ms:** under 20 ms nothing; 20–100 ms the lead moves 5 ms; more than 100 ms it jumps.
  - **Decode slower than real time:** a picture more than 150 ms late drops the rest of its GOP, up to the next key packet, so decode order stays intact (contract item 5). The threshold must stay above any path delay the lead hasn't absorbed yet: at 50 ms, a 90 ms panel path reads as slow decoding.
  - **Stage B:** `CLOCK` replaces all of this, and the BCM slews it itself.
  - **As built (`apps/video/vplay.c`, #64 piece 6):** `tests/video/test_vplay.c` runs it against a model BCM on the 10 ms tick. With an 11 ms decode and 17 ms to the panel, every picture goes up within 5.3 ms of its PTS (mean 2.9 ms) over 3 minutes, and through pause, two seeks and FIFO stalls. A 90 ms panel path is absorbed by the first status (error 6.5 ms after). At a 45 ms decode (every frame over budget), 62% of frames are shown, with a mean lag of 138 ms and at most 275 ms: the cost of the 150 ms threshold, to revisit with device timings (the real over-budget case is 640×480 I frames only).
- **Pause:** the audio pauses, and the player stops posting records. The picture stays on screen, because the BCM keeps the last frame.
- **Speed (timestretch):** the audio clock runs at the chosen rate, so pacing records by the clock follows it automatically.
  - **Decode cost:** at 1.5× a 320×240 stream needs 45 decodes a second, about 75 M instructions a second. That's over the 78 M instructions a second we've measured for the core (`hardware.md`, session 4/5) unless the PLL step (stage 4b) lands.
  - **The rule:** up to 1.25× every frame is decoded. Above that, frames are dropped a GOP at a time. Baseline P frames are all references, so there's no cheaper skip. Above 2×, only key frames are shown, as a slideshow.
  - Speed is an audiobook feature (owner); for video it just has to degrade gracefully.

## 4. Seek

1. **Find the key sample:** in the key table (§1), the key sample at or before the target. Its record gives the file offset.
2. **Seek the audio** to the target, using its ordinary seek path. Rockbox rebuffers if the target isn't buffered.
3. **Restart the video session:** the stage A image has no `SEEK` yet, so the player sends `CLOSE`, then `OPEN`. Then it feeds from the key sample, with the parameter sets in front.
4. **Exact landing:** pictures before the target are decoded but not shown. In stage A, the player posts their records immediately (no pacing) and the BCM shows them briefly. In stage B, `SEEK` drops them on the BCM side.
5. **Scrubbing** (the 7G scrubber): show only key frames while the wheel moves (one `FEED` per key, no pacing), then do an exact seek when it rests.

**Cost:** a GOP of 30 at 320×240 is about 50 M instructions, about 0.6 s at the core's present clock, mostly behind the scrubber.

The prototype's `key_before()` and the test's seek case check step 1 and the restart's parameter-set rule. The decode from that point is bit-exact.

## 5. Buffering, 32 and 64 MB units

| | 64 MB unit | 32 MB unit |
|---|---|---|
| Audio buffer (the file, interleaved) | about 60 MB: 9 min at 768+128 kbit/s, 4.5 min at 1.5 Mbit/s | about 28 MB: 4 min, 2 min |
| Index window (from the `.vix`) | 64 KB (5,460 samples, 3 min at 30 fps), refilled with each buffer fill | the same |
| Key table | ≤ 58 KB (2 h, GOP 30); 6 KB at GOP 300 | the same |
| BCM FIFO (SDRAM, not PP RAM) | 1 MB, about 10 s | the same |
| Player state | under 1 KB of statics | the same |

**Allocations:**
- The index window and key table are one movable `core_alloc` block, taken **before** playback starts. A later `core_alloc` would make the audio buffer shrink, which is a gap (`docs/hardware.md`, "Library view pool").
- The PP never holds decoded video. Pictures exist only on the BCM.

## 6. Race to idle

**On the PP:**
- **Flash:** bulk refills only (§2).
- **BCM FIFO:** a top-up every few seconds.
- **Records:** stage B batches them, so between batches the PP sleeps in Rockbox's idle loop, as it does during audio playback.
- **Boost:** none. The demux and the copy fit the unboosted core, so the player takes no `cpu_boost` except around a seek's burst of records.

**On the BCM** (`hostproto.md` §6):
- decode a few frames ahead into the display queue, then sleep until the next deadline;
- one clock per session;
- SDRAM in self-refresh when idle (that sequence is still unknown).

Each piece is measured, not assumed. The developer power log gives the current per state, as in the 2-D offload plan.

## 7. Power states

- **During video the backlight stays on.** The 7G doesn't dim a playing video. The player holds the backlight like Rockbox's mpegplayer does, and gives it back on pause after the normal timeout.
- **Backlight off, hold, or sleep:** Rockbox powers the BCM down with the backlight (FS#9890, more than 30% battery).
  - **Before power-down:** the player sends `SLEEP` (stage B; in stage A it notes the PTS of the last picture shown).
  - **Audio:** keeps playing, unless the 7G's behaviour (pause on leaving the video screen) applies first.
  - **On wake:** `bcm_init`, `HELLO`, `OPEN`, then seek to the audio's current position (§4). The picture comes back in sync.
- **Leaving the video screen (MENU):** the 7G stops the video and remembers the position. The player stops audio and video, the position goes through the normal resume and bookmark path, and the BCM goes back to `LCD_UPDATE` duty: one full `LCD_UPDATE` repaints the UI (contract item 7).
- **LCD sleep from the shell's 90 s timer (#51)** doesn't apply while a video plays (the backlight is held). After a pause it applies as for audio.

## 8. Failure and fallback

Errors are logged, never shown in a splash (CLAUDE.md rule 1). The user sees the 7G behaviour: the item can't be chosen, or playback returns to the list.

| Case | When it's found | What happens |
|---|---|---|
| Not Baseline (avcC profile ≠ 66, or constraint flags), larger than 640×480, CABAC, or more than 3 references at 640×480 | sync time (avcC and SPS) | listed as unplayable: hidden from the Videos lists and counted in the log (the 7G never had such files, because iTunes refused to sync them) |
| `mp4v` (MPEG-4 Part 2) | sync time | hidden until the BCM's codec 2 exists |
| Our image isn't running (Apple's `vmcs` fallback boot, or `HAVE_BCMFW` off) | at boot, `HP_VERSION` = 0 | Videos stays hidden in the main menu |
| Version mismatch (another major) | `HELLO` | as above, logged |
| `EV_ERROR(HP_E_CORRUPT)` | play | logged, the picture is concealed, playback goes on |
| `HP_ST_ERROR`, `HP_E_UNSUPPORTED`, `HP_E_NOMEM` | `OPEN` or play | close, log, back to the list; the file gets a "failed" mark in its index header so it isn't offered again until it changes |
| BCM dead (the heartbeat hasn't moved for 200 ms, or a fault word) | liveness check | re-upload the image and resume at the audio position (§4); a second failure turns video off until reboot (`hostproto.md` §4) |
| Index missing or stale | open | parse at open (§1), write the index, play |
| Index window or file read fails | play | stop at the last good frame, back to the list, logged |

**Audio is never stopped by a video failure,** except when the user leaves the screen.

## 9. The Videos UI (hidden until ready)

**What's already in the shell:**
- `menucfg.h` reserves `UI_MM_VIDEOS`, and the main menu hides it (`menu.c`).
- The spec (`docs/design/spec/main-menu.md`): Videos sits in the canonical slot after Audiobooks, with a film-strip panel.
- **Visibility (#64, per the north star):** once Phase 14 ships Videos, its main-menu item follows Settings › Main Menu, as in stock 2.0.5. With no videos in the library it shows stock's empty state: the panel says "No Videos", a short label that may match, and any full sentence is written in our own words (CLAUDE.md rule 2). Every category keeps its row, and an empty one opens to an empty-state row. This matches the 7G, so no `changes-from-7g.md` entry is needed.
- `--ipodui-plugins` lifts the trims the old mpegplayer path needed; the new player needs none of them.

**Hooks the player needs, all behind `HAVE_BCMFW_VIDEO` (new; it implies `HAVE_BCMFW`):**

1. **Classification at sync time.** A file is a video if it has a `vide` track whose `stsd` is `avc1` (or `mp4v`). The kind comes from iTunes' `stik` atom (`moov/udta/meta/ilst`):

   | `stik` | List |
   |---|---|
   | 9 | Movies |
   | 10 | TV Shows (with `tvsh`, `tvsn` and `tves` for show, season and episode) |
   | 6 | Music Videos |
   | podcast flag (`pcst`) | Video Podcasts |
   | none (drag-and-drop) | Movies (the mission's default) |

   **Folder override (frozen default, #64; frozen until the Phase 14 owner round):** a missing `stik` is overridden only by a folder named after a category: Movies, TV Shows, Music Videos or Podcasts, anywhere in the file's path (the nearest one wins). A file with no `stik` in any other folder goes to Movies. A `stik` always wins over the folder.
2. **The libfile:** the Videos lists need precompute sections. Raise `LIBFILE_MAX_SEC` first (ROADMAP note), or keep the video lists in their own small file next to the `.vix` files.
3. **The video screen:** full-screen, no header, the picture owned by the BCM (`OPEN_OWN_DISPLAY`).
   - **SELECT** shows the 7G's overlay: progress bar, elapsed and remaining time. Until the BCM's 2-D jobs exist (opcodes `0x20–0x3F`), the overlay is drawn by the PP into a strip, and the BCM composites it over the picture. **Interim, because it has no compositing yet:** a bottom-strip `UPDATERECT` while the session is paused.
   - **MENU** exits (§7).
   - **Wheel:** volume, then the scrubber after SELECT, as on the 7G.
4. **Settings › Videos** (7G 2.0.5): TV Out, TV Signal, Widescreen, Fit to Screen, Captions, and Start Playing ("Where Left Off"). For the 5.5G:
   - "Fit to Screen" maps to `FIT`/`NATIVE`;
   - "Start Playing" maps to the resume path;
   - TV Out stays hidden until TV-out exists (`approach.md` workstream 4).
5. **About:** the Videos count and the capacity legend's Video segment (`settings.md`) come from the classification in hook 1.
6. **Strings:** every new string goes through `LANG_*` (CLAUDE.md rule 8). The 7G short labels are reused; full sentences are our own.

**Found while planning (flag for the main session):** today a `.mp4` film with an AAC soundtrack is accepted as **audio**. `metadata.c` maps `mp4` to AAC, and `mp4.c` reads its tracks. So such a film already shows in Songs and plays as sound only. `.m4v` isn't mapped, so those files are invisible. Hook 1's classification should also keep video files out of the music lists. Until then, a film copied over USB shows up as a song.

## 10. The driver, in pieces (#64)

Each piece can be tested on the host before the next one exists. That's the order the main session and this workstream would build them in.

| # | Piece | Where | Tested by |
|---|---|---|---|
| 1 | **VP client:** the header, both rings with the encoded indices, deadlines per command class, liveness, recovery (re-upload) | `apps/video/bcmvp.c` (built: portable, over a port; recovery is the core's) | a host build against vc2sim, a port of `video_test.py`'s `Player`; then device session N, with `PING`/`STATUS` round trips (`hostproto.md` checklist) |
| 2 | **Host-port bulk writer:** the FIFO copy with in-place Annex B | the same, plus an assembly loop beside `lcd-as-video.S` | the PP cost per frame measured on the device against `hostproto.md` §8's model |
| 3 | **Demuxer and index reader:** `.vix` windows, the key table, the parse-at-open fallback | `apps/video/mp4demux.c`, `vix.c` (built, byte-identical to the reference; `.vix` window reads and the key table still to come) (new; not `lib/rbcodec`, which mustn't depend on `apps/`, and this is player logic) | `tests/video/mp4demux_test.py` as the golden: the C port must write byte-identical `.vix` files and Annex B for the same inputs |
| 4 | **Index builder and classifier** at sync time | `apps/video/vix_file.c`'s `vix_build_file()` (built), called from the tagcache thread's precompute step (the hook and the classifier are the shell's) | a sim test over a small video library: lists, kinds and counts |
| 5 | **`bufpeek_at`** | `apps/buffering.c`, under `#ifdef` (one entry in `docs/changes-from-rockbox.md`; built) | a host test: peeks inside and outside the window while the codec reads |
| 6 | **Player engine:** clock, pacing, seek, pause, speed, power hooks | `apps/video/vplay.c` (engine), `vpcore.c` (the glue: index windows, Annex B into the FIFO, STATUS, restarts, power, recovery) and `vplayer.c` (the thread, the audio clock, the backlight hold) — all built | the simulator, with a fake BCM that logs records against the audio clock; then a device clip end to end |
| 7 | **UI:** Videos lists, the video screen, settings | `apps/ipodui/` (`HAVE_BCMFW_VIDEO`) | `tests/ui/ipodui-p14-*.script` screenshots (the video area is a placeholder in the sim) |

**Coordination:**
- Pieces 1–3 are mostly ours (#60). 4 and 7 are the shell's. 5 is a small shared upstream edit. 6 is #64's core.
- The image keeps protocol minor 1 for stage A; stage B (`CLOCK`, `PAUSE`, `SEEK`, the display queue) bumps it to minor 2.
- The PP checks the minor and uses stage B only when it's offered, so the two sides can land in either order.

## Lessons from the 3D render test (2026-10-23)

`docs/bcm2722-reference/graphics.md` §6a lists what building the 3D renderer taught that applies here:
- a Mac build of the decoder compared by checksum with vc2sim;
- the listings of the decoder's hot scalar loops read for reloaded constants;
- checksummed decoded frames on the device;
- presents on the panel's TE edge (every second edge for 30 fps if the panel runs at 60 Hz; stage 3d measures it);
- idle time per frame as the sleep budget.

## Open questions

- **Owner (Phase 14 round):** confirm or change the frozen category-folder override in §9.
- **Device:**
  - the PP's host-port write rate for bulk data;
  - the priming offset of the audio clock;
  - how long a seek's GOP decode takes at the real clock;
  - the BCM's current in each power state.

## As built (p14-player, 2026-10-12)

**Layers** (all behind `HAVE_BCMFW_VIDEO`; the pure ones also build on the host):

| File | Role |
|---|---|
| `apps/video/bcmvp.c` | VP client: header, rings, FIFO packets (in 32-bit words, as `lcd_write_data()` moves them), replies and events, liveness |
| `apps/video/vpcore.c` | The engine wired to the client: `.vix` windows (2,048 records), samples streamed into the FIFO as Annex B in 2 KB pieces with the parameter sets after every `OPEN`, `STATUS` asynchronous (one round trip per tick, never waited on), the power states, one reload of a dead image, then the fallback |
| `apps/video/vplayer.c` | The thread (`PRIORITY_PLAYBACK`, stack in its own block): the audio clock, the backlight hold, the audio's handle for `bufpeek_at`, the `.vix` (built at open if missing), the image from `/.rockbox/bcmvideo.bin` |
| `bcm-devtest-{core,hal}.c` | The device side: `bcm_video_take/boot/off/release` and the bounded port calls, reusing the device test's take, stock-order power-down and wake path |
| `uisimulator/common/sim_video.c` | The simulator side: `apps/video/vbcm_model.c` answers, `bcmfw/h264` decodes, the pictures go on the sim's panel |

**Memory** (core_alloc, pinned while playing; `vplayer_start()` before the audio starts, so the audio buffer is sized after it):
- the player's block: about 32 KB (the core, the index window and pieces) plus the thread's stack;
- the key table, 16 bytes per key: 11.5 KB for a 2-hour film at GOP 300, 115 KB at GOP 30;
- the image, its own size (up to 512 KB; the stage A image is about 72 KB).

Code: about 21 KB with the flag on (`arm-none-eabi-size`), none without.

**What the stage A image forced** (it decodes every `FEED`, in FIFO order, and shows every picture):
- **Drops and the slideshow restart the session.** A dropped GOP or a skipped sample (above 2×) leaves packets in the FIFO that the image would decode before anything after them, and there is no way to skip them. So the core `CLOSE`s and `OPEN`s and pushes again from the next sample posted, parameter sets first. Measured on the model: a 50 ms decode (every frame over budget) on 12 s of the iPod clip makes 11 restarts. Stage B (`SEEK`, a skip flag) removes them.
- **Pictures before a seek's target flash up.** As planned (section 4); stage B hides them.
- **Waking while paused shows the paused picture again:** after a seek, the engine posts up to and including the picture at the target even while paused (`vplay.c`'s `refresh`), so the panel doesn't show the UI's frame until play resumes. Found in the simulator.

**Liveness: look every tick.** `HP_STATE`'s heartbeat is 8 bits of a 1.024 ms count, so it comes round every 262 ms, and looks spaced at a multiple of that see the same value on a live image. The simulator hit it: checks about 524 ms apart called a paused, live image dead twice and stopped the video. The client now looks on every tick (10–100 ms) and calls the image dead only after the beat stayed put over three looks and 200 ms (`tests/video/test_vpcore.c`, "slow ticks"). `hostproto.md` section 4 has the rule.

**Tests:**
- `tests/video/test_vpcore.c` (in `run.sh`): the core, client and engine against the model on real files, every picture checked byte for byte against the reference Annex B. Scenarios: normal, pause, seek, slow decoding (restarts), 2.5× (key frames only), dark and wake, dark while paused, slow ticks, the buffer behind, the image dying once (reloaded) and twice (given back), no protocol, another major; then six corpus files. On the 12 s clip: every picture within 15 ms of its time; about 75,000 host-port transfers a second, mostly the bitstream.
- `tests/video/sim_run.sh`: a `--bcmfw-video` sim plays a 12 s clip with its audio, goes dark while paused, wakes, plays to the end, stops; `video_shot` writes the panel. Not in the gate (it needs its own build).

**For the device (done on `p14-player`, after dev9's merge):** `bcmfw/mkvideo.py` builds the image (`BCMFW_VIDEO=1`) into the committed `bcmfw/video/bcmvideo.bin` and `apps/video/bcmvideo_image.h` (size and FNV-1a; `vplayer.c` refuses any other file), and `make zip` copies it to `/.rockbox/bcmvideo.bin` in `--bcmfw-video` builds; `devtest_test.py` runs `mkvideo.py --check`. OPEN carries `HP_OPEN_FAST_CLOCK` when `bcm_devtest_fast_clock_allowed()` finds the 4b and 4c flags (read once per start). STATUS gained three words (the last FEED's time on the chip, the longest, the clock in MHz) for **stage 6**, a debug entry that plays a synthetic clip through this player and logs every picture (`apps/video/vtrace.c`; device-session-1.md, "Stage 6" and "Session 9"). **Still to do:** the shell's pieces 4 and 7, and the measurements in "Open questions".
