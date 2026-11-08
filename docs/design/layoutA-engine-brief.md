# Core layout A: the audio engine on the COP (hybrid first), the PCM ring and long DMA runs, the bitstream ring, the engine's line region

*Implementation brief, revision 2, 2026-11-06. Branch `p19-layoutA-brief-v2`, from the first brief (`p19-layoutA-brief`, `4c6ae0d5b7`) with the RE of Apple's COP audio (`832cf3497c`, `docs/pp5022-reference/apple-cop-audio.md`) merged. Docs only: nothing built, nothing run on a device. Parent design: `core-layout-decision.md` (§4 feasibility, §5 measurement plan, §7 steps, §8 open points). Names: the **CPU** and the **COP** are the PP5022's two cores; the **WM8758** is the DAC; the **BCM2722** is the video chip.*

**What changed in revision 2.** The RE showed that Apple never takes PCM source 26 on the COP: the CPU owns I2S, DMA0 and the DMA's FIQ, and the COP only computes (`apple-cop-audio.md` §0) [apple-RE]. Our earlier reading ("Apple routes source 26 to the COP") was a misread of the per-core cache set-up. The owner then decided (2026-11-06): *"start looking at the hybrid approach for now, and test the extension. If we can manage that, and get it to work as we originally intended, we'd have matched and beat Apple."* So:
- **the hybrid is the primary target** (§2, §6, §7): the COP runs decode, DSP and `pcmbuf`'s producer side into a PCM ring; the CPU keeps I2S, DMA0 and the source-26 FIQ, and only re-arms long DMA runs;
- **taking the refill off the CPU entirely** (the FIQ on the COP, or an equivalent) is **the extension X1**, a measured upgrade behind one switch, with several candidate mechanisms and a one-sitting probe plan (§12);
- the increments are renumbered E0–E6 then X1 (§14).

**Tags** as in `docs/pp5022-reference/index.md`: [silicon] (our unit, the sitting named), [apple-RE], [rockbox-derived], [code] (read in this tree), [ours] (this design), [estimate], [hypothesis].

---

## 0. Summary

**Goal.** Move the audio *computation* to the COP as one message-driven engine, as Apple does (`apple-dual-core.md` §2, `apple-cop-audio.md` §0): the codec, the DSP and `pcmbuf`'s producer side. The engine fills a **PCM ring**. The CPU keeps the PCM *output*: I2S, DMA0, the source-26 FIQ, the mixer, the click and the viz tap. In dark play its FIQ does nothing but re-arm **long DMA runs** (64 KB, about 2.7 per second) and wake the engine when the ring runs low. The CPU also keeps buffering and the disk, the UI, the BCM2722 driver, the WM8758's I2C, settings and tagcache. The two cores talk through whole-set messages, two seqlocked "audio now" records and two single-writer rings. They never call into each other's code per field or per frame.

**Why now.** Bench 4gh (2026-11-06):
- K-1 parked the COP (100.5 → 0.6 wakes/s, 0 underruns) [silicon].
- Timer 2 runs on the COP as an IRQ (P-2 passed, 1000/1000) [silicon].
- Dark play in layout C (decode on the CPU at 30 MHz) leaves the CPU only ~14 % idle at ~60 thread wakes/s [silicon].
- The visualiser stutter (#156) is the codec thread taking 81 % of the CPU in slices of up to 379 ms [silicon]. The engine removes that competitor from the UI's core.
- Today's PCM output takes **≈ 345 FIQs/s** on the CPU at 44.1 kHz: the mixer hands DMA 1,024-byte frames (256 samples, `MIX_FRAME_SAMPLES`), and `pcm-pp.c` splits each into runs of at most `(sampr >> 6) & ~3` = 688 B (688 + 336; upstream's unrounded 689 + 335 until #191) [code, derived; the bench's `pcmpp.fiq` counter confirms it]. Long runs cut that to ≈ 2.7/s, and that saving does not depend on any unproven silicon.

**No gate on unproven silicon for the hybrid.** Every mechanism E0–E6 needs is proven: the CPU's source-26 FIQ (daily Rockbox playback), `core_wake()` on a parked COP (K-1, 4gh), cross-core posts (sitting 3), DMA from SDRAM and IRAM, a mid-run DMA stop and resume (`dma_tx_stop()`, Rockbox's pause) [silicon/rockbox-derived]. Only the extension (X1) waits for a probe: P-1's retest and its sister probes (§12.5).

**Decisions taken in this brief** (each a default until the owner round, §1):
1. **One engine thread, created on the core the switch names at boot.** It is today's codec thread, created on the COP. Its stack is the existing 9 KB in IRAM (`codec_stack`, IBSS). The CPU route is the same thread on the CPU. No second stack, no runtime migration: a bench arm reboots.
2. **The hybrid split is "produce early, play now".** Anything whose effect may be heard one ring later (decode, DSP, EQ, speed, crossfade, replaygain) is the COP's producer work. Anything that must be heard *now* (the click, the pause fade, the viz tap's view of what is playing, video's soundtrack, plugins) stays with the consumer on the CPU, next to the DMA (§6.6).
3. **DMA runs are long when only music plays and the screen is dark** (64 KB, the 16-bit count field's maximum), short when lit or mixing (8 KB, or the mixer's 1 KB frames), and **cut at once** when a sound must start (§6.5).
4. **Transport:** Rockbox's kernel event queue in uncached memory (`codec_queue`, already `SHAREDBSS`) carries commands. Command bodies sit in an uncached slot pool. Wakes use `core_wake()`, which K-1 relies on. The ring's low-watermark wake is the CPU's FIQ kicking the parked COP directly (§7): no forced interrupt.
5. **Every CPU-side setter sends a whole set** (S2). The DSP, EQ, speed and prescaler settings travel as one *DSP profile* message with a change mask.
6. **The engine never touches a peripheral.** In the hybrid it does not touch DMA0 or a FIQ either. That rules out I2C (the WM8758), the BCM2722, storage and buflib. The CPU wakes the DAC before it sends a command that starts sound.
7. **The bitstream ring is read cached by the COP**, with one cache drop per fill burst (§5). **The PCM ring is written cached by the COP**, written back per committed chunk, and read by DMA from physical memory (§6.2).
8. **Video's soundtrack and USB audio stay on the CPU** (#144). In the hybrid that is natural: the mixer is the CPU's. Only USB audio's DSP needs a hand-back (§11).
9. **The extension is one switch** (`corelayout_pcm_trigger`): which core and which trigger runs the ring consumer. The consumer is written core-agnostic from E3, so X1 adds a trigger and a route, not a redesign (§12.3).

---

## 1. Owner pre-flight

Answered already:
- (A) is the target and 19-9 is retired (owner, 2026-11-05; `core-layout-decision.md` §1).
- **The hybrid is the primary target, and the FIQ-on-COP extension is tested as an upgrade** (owner, 2026-11-06). This answers Q-A4 below.
- The governor plan in `pp-governor.md` §17 is owner-approved (2026-11-06).
- No meter tests (memory: no meter tests).
- Practical tests with auto-fallback, not staged probes (memory: practical tests over ceremony).
- Fix forward, no retreats (memory: forward, not back).

Open, frozen with a default (⚠️), and the one now answered:

| # | Question | Default until answered |
|---|---|---|
| Q-A1 | Video soundtrack and USB audio (#144): keep them on the CPU for 0.8.x, and move them into the engine after A ships? | ⚠️ **Keep them on the CPU** (§11). In the hybrid this costs nothing: the mixer is already the CPU's |
| Q-A2 | Is a reboot between bench arms acceptable? The engine's core is chosen at boot, so a runtime switch would cost a second 9 KB stack and a live thread move | ⚠️ **Boot-time switch**; each ABAB arm starts with a reboot (~5 s) |
| Q-A3 | Give the engine its own 32 KB wrap guard, taken from the audio buffer when the buffer is built? That is 0.08 % of 39 MB, or 0.4 % of a 32 MB unit's 7.5 MB floor | ⚠️ **Yes** (§5.4) |
| Q-A4 | ~~If P-1 fails for real, is the target the hybrid (decode, DSP and `pcmbuf` on the COP; the FIQ and the mixer on the CPU) rather than Fit-2?~~ | **Answered (owner, 2026-11-06): the hybrid is the target**, whatever P-1 shows. The FIQ-on-COP route is the extension X1, gated on its retest (§12) |
| Q-A5 | Add the extension's probes to the next sitting? (P-4 is **repurposed**: the hybrid needs no forced interrupt, so its slot becomes the X-probe set of §12.5, about 3 minutes, automatic, run after the p1 retest) | ⚠️ **Yes** (§12.5) |

---

## 2. The engine's boundary

### 2.1 What runs where (the hybrid)

| COP: the audio engine (producer) | CPU: the PCM output (consumer) and everything else |
|---|---|
| The codec (`codec_run()`, the codec's own code in the codec slot and the IRAM codec window) | **I2S, DMA0 and its FIQ (source 26)**: the ring consumer, long runs, the watermark kick (§6, §7) |
| The DSP: `dsp_process()` and every stage (EQ, timestretch/speed, crossfeed, replaygain, the software prescaler, resample, channel mode, compressor, tone) | **The mixer** (`pcm_mixer.c`): the playback channel (fed from the PCM ring), the beep and voice channels, the pause/stop fade, the viz tap's writer (§6.6) |
| `pcmbuf`'s **producer** side: `pcmbuf_request_buffer()`, `pcmbuf_write_complete()`, crossfade, track-change markers written into the chunk descriptors, the producer's half of the flush | `pcmbuf`'s **consumer** side: play start, pause, stop, the consumer's half of the flush, the position read from the descriptors |
| The engine's half of the bitstream ring (read cursor, guard copy) | Buffering and storage (`buffering.c`, the file layer, ATA); buffering's half of the ring (fills, write cursor, window) |
| The PCM ring's write cursor and the chunk descriptors | The playback control thread (`audio`, `playback.c`): playlist, track decisions, metadata, the codec image load |
| | The UI, the BCM2722 driver and its job ring, the LCD; the WM8758 over I2C (volume, `dac_wake()`, hardware EQ presets); settings, tagcache, resume/status saves, power and governor decisions |

### 2.2 What the engine may call

Only the engine's own region (§8), lib/rbcodec, IRAM helpers, the kernel's queue, semaphore and wakeup primitives on objects in uncached memory, `core_wake()`, the COP's own `core_sleep()` protocol (§7), and the COP's cache operations.

It may **not** call:
- the file layer (`XCORE_FS_ASSERT_CPU` already panics in developer builds);
- buflib (`XCORE_ASSERT_CPU`);
- `tick_add_task`;
- I2C, `dac_*` or `audiohw_*`;
- DMA0, the interrupt controller or `pcm_*` sink functions (the hybrid's consumer owns them; X1 changes this through the switch, §12.3);
- the mixer (`mixer_*`);
- anything under `apps/ipodui/`;
- `global_settings`;
- the BCM2722 driver;
- `splash`/`panicf` drawing. A COP panic posts a record, and the CPU panics with it and logs to the file (Fit-2's `h_panic` pattern).

### 2.3 Every existing cross-call, and what replaces it

[code] count:
- `playback.c`: about 30 `pcmbuf_*`, `codec_*` and `dsp_*` calls;
- the shell: 48 audio-family names, already listed in `febe_rpc.list`;
- the `audio_pcmbuf_*` callbacks that `pcmbuf.c` makes into `playback.c` from FIQ context.

Each becomes one of four things:
- **a command** (§3);
- **a record read** (§4.3). Examples: `pcmbuf_get_bufferfill()` (now `w_idx − r_idx`, two uncached words), `pcm_is_playing()`, `pcmbuf_is_crossfade_active()`;
- **an event** (§3.2);
- **a local call on the CPU**: the consumer side (`pcmbuf_play_start()`, `pcmbuf_pause()`, `audio_pcmbuf_may_play()`, `audio_pcmbuf_position_callback()`, `audio_pcmbuf_track_change()`) stays on the CPU with the FIQ, and so does every `mixer_*` call. In the hybrid these are *not* commands. Under X1 they become commands to the consumer's core through the same shim (§12.3).

E1 lists them in `apps/audio_engine.list` (the `febe_rpc.list` format), each tagged `producer` or `consumer`. The link check (§8.3) then fails on any direct call from CPU code into a producer function in a layout-A build.

---

## 3. Commands and events

### 3.1 CPU → COP commands

One message carries one whole set. The sender writes a slot (§4.2), then posts `{id, slot}` on `codec_queue`.

| Command | Replaces | Body | Sync? | Sender |
|---|---|---|---|---|
| `ENG_LOAD` | `Q_CODEC_LOAD`. **The load itself moves to the CPU:** the `audio` thread runs `codec_load_buf()` or `codec_load_file()` into the codec slot and the IRAM window, then sends this | `afmt`, the entry point, `encoder` | send (ack = the engine has run `codec_main(CODEC_LOAD)`) | audio |
| `ENG_RUN` | `Q_CODEC_RUN` and the producer's half of `pcmbuf_start_track_change()` as **one** message (gapless needs both together) | track key, change type (`TRACK_CHANGE_*`, or none for the first track), the bitstream window (§5), `CODEC_ID3`'s uncached alias, resume elapsed and offset, crossfade parameters (from the DSP profile) | send | audio |
| `ENG_SEEK` | `Q_CODEC_SEEK` | time (ms), or offset | post | audio |
| `ENG_PAUSE` | `Q_CODEC_PAUSE` (the decoder stops producing; the *sound* pauses on the CPU at once, §6.7) | on/off | send | audio |
| `ENG_STOP` | `Q_CODEC_STOP`, the producer's half of `pcmbuf_play_stop()` and the flush paths | flush or keep | send (ack = `w_idx` reset to the consumer's `r_idx`, §6.7) | audio |
| `ENG_UNLOAD` | `Q_CODEC_UNLOAD` | none | send (ack = codec slot written back and dropped on the COP) | audio |
| `ENG_DSP` | `dsp_configure()` from the CPU, `dsp_set_eq_*`, `dsp_set_timestretch()`, `dsp_replaygain_set_settings()`, crossfeed and compressor setters, the software prescaler (`sound.c`'s `dsp_callback` path), crossfade settings read from `global_settings` | the **whole** `struct eng_dsp_profile` (§4.4), a change mask and a sequence number | post; the engine applies it at a frame boundary | audio, ui |
| `ENG_SINK` | none (new) | release the DSP to the CPU (USB audio) or reclaim it (§11) | send | USB |
| `ENG_DATA` | buffering's answer to `EV_REQ` | the new window, or "filled past the offset" | post | buffering |

**Not commands (the CPU does them locally, in the hybrid):**
- **The click and tones** (`beep.c`, `ipodui/click.c`): `mixer_channel_play_data(PCM_MIXER_CHAN_BEEP)` on the CPU, as today (§6.6).
- **The viz tap** (`viz_tap_open()`/`_close()`, `mixer_set_tap()`): on the CPU, as today.
- **Play start, pause and the fade:** the consumer's, on the CPU (§6.7).
- **The WM8758's volume:** I2C on the CPU, unchanged. **The haptic (piezo):** CPU GPIO, unchanged.
- **The position:** a record field (§4.3), written by the consumer.

Under X1, the first three become the commands `ENG_BEEP`, `ENG_TAP` and `ENG_PLAY` to the consumer's core (§12.3); their bodies are specified there.

The engine keeps answering `codec_get_command()` and `loop_track` from its own queue, locally.

### 3.2 COP → CPU events

Each is posted to the `audio` queue, or to `buffering_queue` for `EV_REQ`, by the **engine thread**, never from an interrupt.

| Event | Replaces | Body |
|---|---|---|
| `EV_TRACK_DONE` | `audio_codec_complete(status)` | track key, status |
| `EV_SEEK_DONE` | `audio_codec_seek_complete()` | track key, elapsed |
| `EV_REQ` | `bufseek()` backward or outside the window, `bufgetdata()`'s wait for data not yet buffered, `codec_strip_filesize_callback()` | kind (`SEEK`, `NEED`, `STRIP`), offset, size |
| `EV_LOW` | buffering's deadline poll (`watch_ticks()`) as the trigger | posted once per crossing of the bitstream watermark; the deadline wait stays as the safety net |
| `EV_META` | codec-discovered fields the CPU shows: `set_offset`, VBR bitrate, the frequency change that `DSP_SET_FREQUENCY` implies | track key and the fields |
| `EV_LOG` | `ci->logf`, `panicf` from the engine | an index into the uncached log ring (64 × 48 B, 3 KB, from the engine arena, not a static); a panic sets `fatal`, and the CPU panics and logs (no splash) |

`EV_TRACK_STARTED` and `EV_UNDERRUN` of revision 1 are gone: the marker reaching the DAC and an underrun are both seen by the CPU's own FIQ, so they are local (§6.3).

**Elapsed time and offset are never an event.** They are record fields, read by whoever wants them (Now Playing, `audio_current_track()`, the resume save).

---

## 4. Transport

### 4.1 Wake mechanism

**CPU → COP.** `queue_post()`/`queue_send()` on `codec_queue`. With K-1 (step 1, [silicon] 4gh), a post to a parked COP calls `core_wake(COP)`: one `COP_CTL` write. The ring's watermark kick (§7) is the same wake, issued from the CPU's FIQ. **The mailbox doorbell** (`CPU_QUEUE`, Apple's form, [apple-RE]) is not on this path: a post already costs one register write. It stays `data-paths.md` open question 7, and it is one of the extension's candidates (§12.4 f).

**COP → CPU.** A cross-core post to `audio_queue`/`buffering_queue` (`SHAREDBSS`, and Rockbox's corelock) wakes the CPU through `core_wake(CPU)`. This is proven by sitting 3's `be_post` traffic [silicon]. In steady dark play these are rare: `EV_LOW` once per bitstream fill burst, `EV_META` per track.

### 4.2 Command slots

```c
/* apps/audio_engine.h (E0 contract; ProkBox header) */
#define ENG_SLOTS 8
struct eng_slot {                   /* XCORE_BSS: uncached, 16-byte aligned */
    uint32_t seq;                   /* writer: the sender, odd while filling */
    uint32_t done;                  /* writer: the engine (= seq when consumed) */
    uint16_t id;                    /* ENG_* */
    uint16_t len;
    uint32_t body[12];              /* the set, by value; ≤ 48 B, except the DSP profile (§4.4) */
};
```
- **Allocation.** Slots are allocated on the CPU under a CPU-only mutex. Every sender is a CPU thread.
- **Release.** A slot is free when `done == seq`.
- **A full pool** blocks the sender on `queue_send`. At the rates involved this never happens: ≤ 10 commands a second outside seeks. If it ever does, it is logged (`eng slots full`).
- **No sender's stack crosses.** Stacks are cached memory.

### 4.3 The "audio now" records (seqlocked, one writer each)

There are two records, one per writer. In the hybrid the first is written by the CPU's FIQ and read by CPU threads: no core crossing. It stays uncached and seqlocked anyway, so X1 moves its writer without touching a reader.

```c
struct eng_now_out {        /* writer: the ring consumer only (the CPU's FIQ in the hybrid) */
    uint32_t seq;           /* odd while writing */
    uint32_t key;           /* identity: the track key the position belongs to */
    uint32_t elapsed;       /* ms, from the descriptor of the last run completed */
    uint32_t offset;
    uint32_t started;       /* count of track-change markers reached */
    uint32_t underruns;
    uint32_t runs;          /* DMA runs completed (bench: runs/s) */
    uint32_t mode;          /* PASS / MIX / SHORT (§6.5) */
};
struct eng_now_thr {        /* writer: the engine thread only (COP) */
    uint32_t seq;
    uint32_t state;         /* ENG_ST_STOPPED/LOADED/RUNNING/PAUSED/SEEKING/DSP_RELEASED */
    uint32_t key;           /* the track the codec is decoding */
    uint32_t dsp_seq;       /* the last ENG_DSP sequence applied (and the profile's checksum) */
    uint32_t dsp_sum;
    uint32_t codec_elapsed; /* ci->set_elapsed, for seeks */
    uint32_t freq;          /* the produced rate */
    uint32_t flags;         /* crossfade active */
};
```
- **Identity is kept apart from validity** (the Phase 5 lesson). `key` says *which* track, and `seq` says *whether the read is whole*.
- **Readers.** The reader is the `pgov_idle` form from `pp-governor.md` §17.6: retry while `seq` is odd or has changed. Uncached stores leave in program order, and `volatile` holds the compiler's order. A CPU thread reading `eng_now_out` can be interrupted by the FIQ that writes it: the retry covers that (the FIQ always completes its write).
- **Placement.** Both records are `XCORE_BSS`.
- **Position between runs.** With 64 KB runs the descriptor-based position steps every 372 ms. A reader that needs finer time (Now Playing's bar while lit, where runs are short anyway) adds the DMA's progress: `elapsed + (run_bytes − DMA0_STATUS count) / byterate`. This is a read of a CPU-owned register, valid only in the hybrid; under X1 it is a record field.
- **CPU-written single words** stay direct reads and need no seqlock: `play_status`, the `may_play` bits.
- **The test.** `tests/ppgov/test_idle.c`'s store-by-store harness is reused for both records (§14.2).

### 4.4 The DSP profile (the S2 lesson)

```c
struct eng_dsp_profile {            /* ≤ 256 B; one per ENG_DSP, by value in a big slot */
    uint32_t mask;                  /* ENG_DSPF_EQ | _SPEED | _XFEED | _RG | _PRESCALE | _XFADE | ... */
    int32_t  eq_precut;
    uint8_t  eq_enabled, eq_bands;
    struct { int16_t cutoff, q, gain; } eq[EQ_NUM_BANDS];   /* 10 x 6 B */
    int32_t  speed, pitch;          /* timestretch (audiobook speed) */
    uint8_t  ts_enabled, xfeed_type, channel_mode, compressor;
    int16_t  xfeed_direct, xfeed_cross, xfeed_hf_atten, xfeed_hf_cutoff, stereo_width;
    int32_t  rg_type, rg_preamp, rg_noclip;  /* replaygain mode; the track's gains travel with ENG_RUN */
    int32_t  prescale;              /* the software volume prescaler (sound.c) */
    uint16_t xfade_mode, xfade_in_delay, xfade_in_dur, xfade_out_delay, xfade_out_dur, xfade_mix;
    uint32_t sum;                   /* FNV-1a over the above (logged on both sides) */
};
```
- **The CPU keeps a shadow.** Every setter updates the shadow and sends the whole profile once. Bursts are coalesced: the UI's EQ editor sends at most one profile per 50 ms, and the last one always goes.
- **The engine applies the masked parts in one go between two `dsp_process()` calls.** The measured S2 result was 300/300 torn seeds with one call per EQ band, and 0/300 with one call per set (`tests/audioxcore`).
- **`pcmbuf.c`'s reads of `global_settings.crossfade_*` and `playlist_shuffle`** (lines ~656, 836–854) move into the profile and `ENG_RUN`.
- **Per-track replaygain gains** come from the id3, and ride in `ENG_RUN`.
- **When a change is heard.** A DSP change is heard after the PCM already in the ring has played: up to the ring's level, about 2–3 s. That is **today's behaviour** (the DSP already runs before `pcmbuf`), not a new latency.

---

## 5. The bitstream ring

### 5.1 Shape

Buffering keeps its ring and its handles. One handle, the codec's audio handle (`ci.audio_hid`), is **exported** to the engine as a *window* in file-offset space (`off_t` is 32 bits on FAT32, so every cursor is one atomic word):

```c
struct eng_ring {                   /* XCORE_BSS */
    /* writer: the CPU (buffering). Window fields change only while the
       engine waits for ENG_RUN or ENG_DATA (the handshake orders them) */
    uint32_t win;                   /* window identity: bumps with every new window */
    uintptr_t base;                 /* cached address of the ring's first byte (16-aligned) */
    uint32_t size;                  /* ring bytes (a multiple of 16) */
    uint32_t base_off;              /* the file offset held at ring index base_idx */
    uint32_t base_idx;
    uint32_t filesize;
    volatile uint32_t w_off;        /* filled up to (exclusive); single writer: CPU */
    /* writer: the COP (the engine) */
    volatile uint32_t r_off;        /* consumed up to; single writer: COP */
    volatile uint32_t low_armed;    /* EV_LOW edge flag: COP sets, CPU clears on a fill */
};
```

**The invariant.** The CPU never writes ring bytes that hold offsets in `[r_off, w_off)`. Buffering already never overwrites unread data. The new part is that it reads `r_off` from this struct for the codec's handle, instead of `h->ridx`.

### 5.2 Coherence: one drop per fill burst, not per read

- **The CPU publishes.** After data for `[w_off, w_off + n)` is in SDRAM, the CPU stores `w_off += n`.
  - ATA DMA writes memory directly.
  - Any CPU copy is written back first with `xcore_publish()`. That is buffering's existing per-chunk write-back, 32 KB per chunk.
- **The COP reads cached.** It keeps a private `safe_off`: the `w_off` it saw just before its last cache drop. Bytes in `[r_off, safe_off)` are safe to read cached.
  - They were in memory before the drop.
  - The COP has loaded no line of them since the drop, except from memory.
  - The CPU will not rewrite them until `r_off` passes them.
- **When the reader needs bytes beyond `safe_off`** and `w_off > safe_off`, it runs: `w = w_off` (an uncached load); `xcore_acquire()` (write back, then drop the COP's 8 KB); `safe_off = w`.
- **Cost.** In steady play the writer fills megabytes ahead, so the reader crosses `safe_off` about **once per fill burst** (a few per hour). At a track start or after a seek, when the reader is right behind the writer, it is once per 32 KB chunk it catches up with, and that is boosted time anyway. Each drop is a whole-cache write-back plus 512 status rewrites. Its cost is unmeasured (fit2 risk R2), and E2 logs it (`eng drops= drop_us_max=`).
- **Why not uncached reads** (Fit-2's way). Fit-2's ~40 % cycle overhead came partly from uncached bitstream reads (`core-layout-decision.md` §1 item 6). Cached reads with a rare drop remove that cost.

### 5.3 The `request_buffer`/`advance_buffer` replacement

These run on the COP, inside the codec's `ci` callbacks, with no call to the CPU:

| `ci` callback | Engine implementation |
|---|---|
| `request_buffer(&real, req)` | Clamp to `w_off - r_off`. Run the safe-mark rule. Return a pointer into the ring, or into the **engine's guard** when the span wraps (copy `ring[0..]` there, as `bufgetdata()` does). With 0 bytes available and `w_off < filesize`: post `EV_REQ NEED{r_off, req}` once and wait on the engine queue for `ENG_DATA` (it still answers SEEK/STOP while waiting). Rare: track start on a cold HDD |
| `advance_buffer(n)` | `r_off += n` (one uncached store). If `w_off - r_off` falls below buffering's watermark and `!low_armed`: set it and post `EV_LOW`. The offset reaches the CPU through the record (`ci->set_offset`), not a call |
| `read_filebuf(p, n)` | request + `memcpy` + advance |
| `seek_buffer(pos)` | **Forward within `[r_off, w_off]`: local**, `r_off = pos`. **Backward, or past `w_off`:** post `EV_REQ SEEK{pos}` and wait for `ENG_DATA`, which carries a new window (`win` bumped). The engine then forces a drop and sets `safe_off = 0`. Backward seeks always cross, because the CPU may be reclaiming space below `r_off` at that very moment |
| `seek_complete()` | `EV_SEEK_DONE` |
| `strip_filesize()` | `EV_REQ STRIP` |

### 5.4 The engine's guard

`bufgetdata()` makes a wrapped span contiguous by copying into `GUARD_BUFSIZE` (32 KB) after the ring. **The CPU's other handles** (album art, cue sheets) use that guard too, so the COP must not write it. The engine gets its own 32 KB guard (Q-A3):
- it is carved when buffering builds the buffer (`buffering_reset()`, while stopped), and returned with the buffer: no `core_alloc` during play;
- it sits on whole lines (16-aligned at both ends);
- the CPU never touches it.

### 5.5 Track skips and seeks across the boundary

**Automatic (gapless):**
1. The codec finishes track N and posts `EV_TRACK_DONE{N}`.
2. The `audio` thread opens N+1's window and sends `ENG_RUN{N+1, TRACK_CHANGE_AUTO, window, id3}` as one message. The PCM ring holds seconds, so the round trip of a few ms never gaps.
3. Buffering closes N's handle only after `ENG_RUN` is acked: the engine no longer reads N.
4. The engine writes the track-change marker into the next chunk descriptor. The CPU's FIQ sees it when that chunk's run completes (runs end at markers, §6.4), bumps `started` and calls `audio_pcmbuf_track_change()` locally.

**Manual skip:**
1. The CPU stops the output (§6.7), sends `ENG_STOP{flush}` and then `ENG_RUN{M, TRACK_CHANGE_MANUAL}`, with `ENG_LOAD` in between if the format changes.
2. The producer's flush and its marker run on the COP between the two commands. The old handle closes after `ENG_STOP`'s ack.

**Seek within the track:**
1. The CPU stops the output, sends `ENG_STOP{flush}` and `ENG_SEEK{ms}`, and the codec runs its seek.
2. If the codec's `seek_buffer` stays forward and inside the window, it is local. Otherwise the codec posts `EV_REQ SEEK`, buffering re-windows (`bufseek()` on the CPU, unchanged), and `ENG_DATA` carries the new window.
3. `EV_SEEK_DONE` ends it; the output restarts in `SHORT` mode (§6.5) as soon as the ring holds the prefill.

**Codec loads:**
1. The CPU loads the image into the codec slot (SDRAM, cached) and the IRAM window. It drops its own cache **before** the load (`commit_discard_dcache()`), so no stale line from the last image merges into alignment holes, and writes back **after** it.
2. `ENG_LOAD`: the COP writes back and drops its data cache *and* drops its instruction cache (R7's `COPJ_CODE` row), then calls `codec_main(CODEC_LOAD)`.
3. The CPU does not touch the slot again until the `ENG_UNLOAD` ack, which the COP gives after its own write-back.

**`CODEC_ID3`.** The CPU fills the codec's mp3entry before `ENG_RUN`, writes it back, and does not write it again until `EV_TRACK_DONE`. The codec reads it through its uncached alias, as Fit-2 did for over an hour on silicon (S36). The playing track's `elapsed`/`offset` (`PLAYING_ID3`) are filled by the CPU from `eng_now_out` when it reads them (`audio_current_track()`, the resume save), not written from the FIQ.

---

## 6. The PCM ring and long DMA runs (new in revision 2)

### 6.1 Shape: `pcmbuf`'s buffer becomes a single-producer, single-consumer ring

The PCM ring **is `pcmbuf`'s existing buffer** (≥ 3 s, `MIN_BUFFER_SIZE` = 3 × byterate ≈ 529 KB at 44.1 kHz, plus crossfade) and its 8 KB chunk descriptors. No new buffer is allocated. Cursors are **free-running 32-bit byte counts** (index = count mod size), so the level is one subtraction and wraps cleanly:

```c
struct eng_pcm_ring {               /* XCORE_BSS: uncached, 16-byte aligned */
    /* fixed while stopped (set at pcmbuf init / ENG_STOP ack) */
    uintptr_t base_pa;              /* physical (uncached) address of byte 0; DMA reads here */
    uintptr_t base_va;              /* the producer's cached address of byte 0 */
    uint32_t size;                  /* bytes, a multiple of 64 KB (§6.4) */
    uint32_t desc_pa;               /* the descriptor array's uncached alias */
    /* writer: the producer (COP engine) only */
    volatile uint32_t w_idx;        /* bytes committed: written back, descriptor written */
    volatile uint32_t low_wm;       /* bytes; from the profile, ≥ 2 runs + wake margin */
    /* writer: the consumer (the CPU's FIQ in the hybrid) only */
    volatile uint32_t r_idx;        /* bytes whose run has completed (played to the FIFO) */
    volatile uint32_t arm_idx;      /* bytes handed to DMA (= r_idx + the current run) */
    /* the wait flag: set by the producer, cleared by the consumer */
    volatile uint32_t wm_wait;      /* 1 = the engine sleeps until the level falls below low_wm */
};
```
- **Single writer per word.** `w_idx` and `low_wm`: the producer. `r_idx` and `arm_idx`: the consumer. `wm_wait` is a hand-over flag with one setter and one clearer (the pattern of `eng_ring.low_armed`).
- **The invariants.** The producer never writes bytes in `[r_idx, w_idx)` (committed and not yet played: they belong to the consumer and to DMA). The consumer never hands DMA a byte at or past `w_idx`.
- **Level** = `w_idx − r_idx`; **free** = `size − (w_idx − r_idx)`.
- **Descriptors** (`struct chunkdesc` today: size, elapsed, offset, the track-change flag) are written by the producer before the `w_idx` store that publishes them, and read by the consumer through their uncached alias (a few words per chunk).

### 6.2 Cache handling

- **The COP writes PCM cached** (the DSP's output path is unchanged). Each `pcmbuf_write_complete()` that commits a chunk runs `xcore_publish()` (the COP's whole-cache write-back) and **then** stores `w_idx` (uncached). At 44.1 kHz with 8 KB chunks that is ≈ 21.5 write-backs/s, each in a decode burst the COP is awake for anyway. E5 logs `pub_us_max`.
- **DMA reads physical memory** (`base_pa`): it never sees a cache. `pcm-pp.c`'s `dma_tx_buf_prepare()` `commit_dcache()` is dropped on this path: the CPU's cache holds no PCM, so a CPU write-back there was only cost.
- **The CPU never reads the ring cached.** Its mixer (mix mode, §6.5) reads the playback channel's samples through the uncached alias: 1 KB per 5.8 ms frame, only while mixing (lit, a click, a fade).
- **No line is shared.** The ring starts and ends on 16-byte lines, and the descriptors are read uncached. The COP's own cache may hold lines of an old lap of the ring: they are its own written-back data, and the producer overwrites them only in free space (outside `[r_idx, w_idx)`), so they never alias DMA's bytes.

### 6.3 The consumer: the CPU's FIQ in the hybrid

`fiq_playback()` keeps its shape (ack, add the size, restart or call back). In pass-through mode the "call back" is the ring consumer, `eng_pcm_refill()`, in IRAM:

1. Ack (`DMA0_STATUS`), then `r_idx = arm_idx` (the run that just ended is played).
2. Walk the descriptors the run covered: publish `elapsed`/`offset`/`key` into `eng_now_out`; on a track-change marker, `started++` and the local `audio_pcmbuf_track_change()` hook.
3. `avail = w_idx − r_idx` (one uncached load). If `avail == 0`: an underrun. Count it in `eng_now_out`, play one short silence run (the mixer's existing silence frame), stay armed.
4. `run = min(avail, run_max(mode), bytes to the ring's end, bytes to the next marker)`. Start DMA0 at `base_pa + (r_idx mod size)`; `arm_idx = r_idx + run`.
5. **The watermark kick** (§7): if `wm_wait` and `w_idx − r_idx < low_wm`, clear `wm_wait` and wake the COP.

That is a handful of uncached loads and stores and one register write per run: no mixing, no samples read, no cache work, no kernel call.

**The deadline** at each FIQ is the I2S FIFO's drain: 16 entries = 16 frames = **0.36 ms at 44.1 kHz** [rockbox-derived: `audio.md` §1], the same deadline as today. The run length does not change it; it only changes how often it comes.

### 6.4 Run size: 32, 64 or 128 KB

At 44.1 kHz stereo 16-bit the byte rate is 176,400 B/s.

| Run | Duration (44.1 kHz) | CPU FIQ wakes/s (44.1 kHz; 48 kHz) | Latency it adds without a cut | Ring needed (≥ 3 runs: playing, next, being filled) | Underrun margin at the kick (low_wm 2 s) |
|---|---|---|---|---|---|
| today (688 B + 336 B per 1 KB frame; 689 + 335 before #191) | 3.9 + 1.9 ms | **≈ 345** [code, derived] | – | – | – |
| 8 KB (SHORT, lit) | 46 ms | 21.5; 23.4 | 46 ms | 24 KB | 2 s − 46 ms |
| 32 KB | 186 ms | 5.4; 5.9 | 186 ms | 96 KB | 2 s − 186 ms |
| **64 KB (PASS, dark; the default)** | **372 ms** | **2.69; 2.93** | 372 ms | 192 KB (pcmbuf has ≥ 529 KB) | **≥ 1.6 s** |
| 128 KB | 743 ms | **not possible as one run**: the count field is 16 bits (bytes − 4), so 65,536 B is the largest run [rockbox-derived, `dma.md` §2; Apple's pieces are 64 KB too]. Two 64 KB runs are two FIQs: 2.69/s again | 743 ms | 384 KB | ≥ 1.3 s |

**Choice: 64 KB.** It is the largest single run the engine allows, and it takes the CPU's audio FIQs from ≈ 345/s to ≈ 2.7/s (−99.2 %). 128 KB buys nothing without auto-reload hardware (§12.4 c). 32 KB doubles the wakes for no gain once the cut (§6.5) handles latency.

- **Latency.** Nothing the user does waits for a 64 KB run to end: pause and every new sound cut the run at once (§6.5, §6.7). Without the cut, a click could wait up to 372 ms, which is why the cut is part of E3, not an option.
- **Memory.** +0 B: the runs come out of `pcmbuf`'s existing ring. `pcmbuf_size` is rounded to a multiple of 64 KB so no run is cut short at the wrap (a change of < 64 KB, rounded **down** on a 32 MB unit to stay inside the 7.5 MB floor, **up** on 64 MB).
- **Underrun margin.** The producer is woken when the level falls below `low_wm` (2 s, today's `PCMBUF_WATERMARK`). The consumer checks at run ends only, so it sees the crossing up to one run late: the margin left for the COP to wake and decode the next run is ≥ 2 s − 372 ms ≈ 1.6 s, against a decode rate of many times real time. `low_wm` is clamped to ≥ 2 runs + 100 ms.
- **Runs end at markers** and at the ring's end, so the track-change edge and the position are exact to the chunk, not to the run.
- **Higher rates.** At 96 kHz (if a file and the mixer ever run there) a 64 KB run is 171 ms: 5.9 wakes/s. Still within A-1's ≤ 5/s target only at ≤ 48 kHz; the bench logs the rate per track.

### 6.5 Modes: PASS, SHORT, MIX, and the cut

| Mode | When | Run source | Run size | FIQ work |
|---|---|---|---|---|
| **PASS** | only the playback channel is active, amplitude unity, no tap, **screen dark** | the PCM ring, directly (no copy) | ≤ 64 KB | §6.3 only |
| **SHORT** | as PASS but the screen is lit, or the first runs after start/seek (until the ring holds 2 long runs) | the PCM ring, directly | ≤ 8 KB | §6.3 only |
| **MIX** | any other channel active (beep/click, voice/video soundtrack, plugin), a tap open, or a fade (amplitude ≠ unity) | the mixer's IRAM `downmix_buf` frames (1 KB, 256 samples), mixing the ring's samples (uncached reads) with the other channels, exactly as today | 1 KB frames (today's runs) | today's mixer callback |

- **The cut** (PASS/SHORT → MIX at once). When a channel starts, a tap opens or a fade begins, the CPU (thread context, PCM lock held) runs `dma_tx_stop()`: Rockbox's existing mid-run stop, which snapshots how far the run got and leaves "what's left" to resume [code: `pcm-pp.c`, used by pause today]. `r_idx` advances by the played part, the mixer takes over from that byte, and the first MIX frame starts within one frame: **click latency ≤ 5.8 ms + the cut** (µs), as today. The FIFO's 16 frames cover the restart. Whether the stop-and-restart is click-free at the DAC is [hypothesis] until a1-click; Rockbox's pause/resume uses the same stop [rockbox-derived].
- **Back to PASS** after the mixer's existing idle rule (`max_idle_frames()`: 3 s with no other channel) and only when dark.
- **The lit/dark edge** is a word the backlight code sets (CPU); it changes `run_max` at the next run end. Going dark changes nothing at once; going lit cuts only if a sound starts.
- **Cut fallback** (automatic, logged): if a1-click finds an artefact, the cut waits for the current run in SHORT mode (≤ 46 ms) and PASS is entered only when dark, so the worst click delay is the first detent after waking from dark (≤ 372 ms, logged as `pcm cut wait`).

### 6.6 The mixer, the click and the viz tap: the CPU, and why

| Part | Core (hybrid) | Why |
|---|---|---|
| **Mixer** | **CPU** | It sits between the ring and DMA, so it is the consumer's. Everything it mixes must be heard *now*: putting it on the producer side would delay every sound by the ring's 2–3 s. It runs only in MIX mode, which is lit or interactive, when the CPU is awake for the UI anyway. Video's soundtrack and plugins already drive it from the CPU (§11) |
| **The click** | **CPU** (`ipodui/click.c`, `beep.c`, unchanged) | A detent must sound within a few ms (D15). With the cut it starts within one 5.8 ms frame. On the COP it would be a message to a producer that is seconds ahead of the DAC |
| **The pause/stop fade** (`pcmbuf_fade`, amplitude steps) | **CPU** (the playback channel's amplitude, MIX mode) | Heard at once. Today it is a mixer amplitude ramp already [code: `pcmbuf.c:1463`] |
| **The viz tap** | **CPU** (the mixer's tap, MIX mode while open) | The visualiser must show what is *playing*, not what was decoded 2 s ago. The tap sees the frame going to DMA. It is lit-only, so MIX mode costs nothing in dark play. **Later option**, not in E3: a zero-copy reader of the ring at the DMA's address (no tap, PASS allowed while the visualiser shows) |
| **Crossfade** | **COP** (producer) | Track-to-track mixing happens at write time today; it may be heard a ring later by design |

### 6.7 Start, pause, stop and flush across the two cursors

- **Start.** The CPU starts the output once `w_idx − r_idx` ≥ the prefill (`LOW_DATA`: 0.25 s on flash, 1 s on disk), in SHORT mode. That is the consumer's half of `pcmbuf_play_start()` and of `audio_pcmbuf_may_play()`, both local on the CPU.
- **Pause** (the sound): the CPU fades in MIX mode and stops DMA (`dma_tx_stop()`, which keeps the position): **heard at once**, independent of run size. It then sends `ENG_PAUSE` so the producer stops at the ring's full mark instead of spinning. **Resume** restarts the DMA from the kept position in SHORT mode.
- **Flush** (skip, seek, stop): the CPU stops DMA and fixes `r_idx = arm_idx =` the played point (it owns both). It then sends `ENG_STOP{flush}`. The COP, the only writer of `w_idx`, sets `w_idx = r_idx` (stable: DMA is stopped) and acks. **No core writes the other's cursor.**
- **Underrun** (`avail == 0` at a run end): the FIQ plays silence, counts it and keeps the kick armed. It never waits.

---

## 7. The watermark wake: the CPU's FIQ kicks the COP

**Today** (19-1c, [code]). On a full `pcmbuf` the codec sleeps for `pcmbuf_full_wait_ticks()`: the time the level takes to drain to the low watermark (`cpl_full_wait()`). On the COP after K-1, that timeout is serviced by the CPU's tick. So each refill burst costs a timeout armed on the COP, a CPU tick check and a `core_wake`.

**Hybrid** (`pp-governor.md` §17.3, rewritten for the CPU-owned FIQ). The consumer sees the level at every run end, so it is the natural trigger:
1. **The engine waits.** Finding no free space, the engine stores `wm_wait = 1`, re-checks the level (lost-wake guard), and if the ring is still above `low_wm` it **parks the COP in its own wait loop**: `while (wm_wait && queue_empty(&codec_queue)) core_sleep(COP)`, using thread-pp's `core_sleep()` protocol (`intend_sleep`/`stay_awake`), which is what K-1's park already uses [code]. The engine is the COP's only thread in layout A (the split is refused, §14), so it may hold the core in that loop. No timeout is armed.
2. **The CPU's FIQ kicks it.** In §6.3 step 5, `wm_wait && level < low_wm` → `wm_wait = 0; eng_kick()`. `eng_kick()` is `core_wake(COP)`'s protocol (set `stay_awake`, wait out an `intend_sleep` in progress, clear `COP_CTL`'s sleep bit), made FIQ-safe: `core_wake()` today masks IRQ only, so a FIQ can arrive mid-protocol on the CPU. E5 either proves the FIQ re-entry harmless (the stores are idempotent; [code] read + the host harness) or gives the FIQ its own `eng_kick()` that uses only `stay_awake` and the `COP_CTL` write.
3. **Commands still wake it.** A post to `codec_queue` calls `core_wake(COP)` (K-1), and the loop leaves on a non-empty queue.

**Cost in dark play.** One kick per refill burst. With `low_wm` = 2 s and a ≥ 3 s ring, the COP decodes about 1 s of audio per burst, so **about one COP wake per second of audio from the ring** (more with a larger ring: E5 may grow `pcmbuf` to cut COP wakes further, a memory trade measured, not assumed). The CPU pays nothing extra: the kick rides on a FIQ it takes anyway.

**No forced interrupt.** Revision 1's design (FIQ → `INT_FORCED_SET` → COP IRQ → queue post) and its probe P-4 are dropped: the kick needs no interrupt on the COP, because the engine waits in a core-level loop, not on a kernel object. P-4's slot in the sitting is repurposed for the extension's probes (§12.5, Q-A5).

**Fallback (automatic, logged).** A watchdog in the engine: if no kick arrives within 2 × the expected drain time, it re-arms the 19-1c timeout path (works on the COP since K-1) and logs `eng wm fallback`.

**Acceptance** (`pp-governor.md` §17.5): 10 minutes of dark play with the engine woken only by kicks and commands: 0 underruns, 0 COP timeouts armed, no `eng wm fallback`.

---

## 8. The engine's line region

### 8.1 Objects

- **Engine members** (cached `.data`/`.bss`, on lines of their own; written by the COP only):
  - `codec_thread.o`, `codecs.o` (`ci`);
  - `pcmbuf.o`'s producer state (E1 splits `pcmbuf.c`'s statics into producer and consumer structs; the consumer's go to `.ncbss` or IRAM so X1 can move them);
  - every `lib/rbcodec/dsp/*.o` (`dsp_core`, `dsp_misc`, `dsp_sample_io`, `dsp_filter`, `eq`, `crossfeed`, `tdspeed`, `resample`, `pga`, `channel_mode`, `compressor`, `tone_controls`, `afr`, `pbe`, `surround`, `lin_resample`);
  - `audio_engine.o`.
- **Consumer members (CPU in the hybrid):** `pcm_mixer.o`, `pcm.o`, `beep.o`, `eng_pcm_out.o` (the ring consumer). They stay in the CPU's ordinary memory in the hybrid. **Every word the ring consumer touches in the FIQ** is uncached or IRAM from E3 (`eng_pcm_ring`, `eng_now_out`, `dma_play_data`, the descriptors' alias), which is what lets X1 move it.
- **Already uncached, so exempt from line sharing:**
  - `channels[]`, `active_channels[]` (`pcm_mixer.c`, IBSS);
  - `dma_play_data` (IBSS);
  - `codec_stack` (IBSS);
  - the viz tap's ring (behind the uncached alias);
  - the slots, the records, `eng_ring` and `eng_pcm_ring` (`XCORE_BSS`).
- **What this region fixes.** The producer's statics are cached memory that only the COP writes, on lines no CPU static shares (`data-paths.md` cell 10's lesson). In the hybrid the FIQ's refill chain stays on the CPU, so cell 10's move problem does not arise until X1.

### 8.2 Link script

`firmware/target/arm/pp/app-pp.lds` follows the pattern 2.44 used for the file layer (`changes-from-rockbox.md` §2.44):
- **Two new output-section groups.** `ENGINE_DATA` collects `*codec_thread.o(.data*)`, `*dsp_*.o(.data*)`, … and `ENGINE_BSS` collects the same objects' `.bss* COMMON`.
- **Line boundaries.** Each group is preceded and followed by `. = ALIGN(16);`.
- **Placement.** The groups sit after the file layer's region, so the existing `EXCLUDE_FILE` lists only grow.
- **Guard.** `HAVE_CORE_LAYOUT_A`. A stock build's script is byte-identical.

### 8.3 Gate checks

| Check | Mode |
|---|---|
| `scripts/xcore_line_check.py`: an `ENG_MEMBERS` class next to `FE_MEMBERS` and `FS_MEMBERS` | **strict from E4** for the engine class (the region is new and complete). The front-end class keeps its warn mode |
| `scripts/febe_reach_check.py --roots engine`: a transitive walk from `codec_thread` and the `ci` table | **fails** on any path into the file layer, buflib, `tick_add_task`, I2C/`audiohw_*`/`dac_*`, `pcm_*`/`mixer_*`, `apps/ipodui/` or the BCM driver. It **lists** every cached non-engine static the engine **reads** (stale-read risk), and each needs routing into the profile, an uncached home, or a reviewed `READ_OK` (constant after boot) |
| `scripts/febe_reach_check.py --roots pcmout`: a walk from `eng_pcm_refill` | **fails** on any cached static touched (the consumer must stay movable for X1) |
| `scripts/eng_link_check.py` (new; `febe_link_check.py`'s shape) | fails on a direct call from a non-engine object into a `producer` function listed in `apps/audio_engine.list`, except through the shim |
| `XCORE_ASSERT_ENGINE(what)` (new in `xcore.h`) on the producer's entry points (`pcmbuf_request_buffer`, `pcmbuf_write_complete`, `dsp_process`, `dsp_configure`) | developer builds count `xcore_offengine`; `DEBUG` panics; release: nothing |
| `scripts/xcore_map_check.py` | the slots, the records, `eng_ring` and `eng_pcm_ring` must link in `.ncbss` |

---

## 9. Codec memory and IRAM

| Memory | Lives | Used by (hybrid) | Coherence |
|---|---|---|---|
| Codec slot (code, data, bss) | SDRAM, cached (`codecbuf`) | written by the CPU's loader, then run by the COP only | hand-off at `ENG_LOAD`/`ENG_UNLOAD` (§5.5), I-cache included |
| Codec IRAM window | 80 KB at `0x4000_C000` (`plugin.lds`); full: FLAC 81,008 B (`hardware.md`) | loaded by the CPU, executed and written by the COP | IRAM is uncached and shared: no cache operation |
| Core IRAM, engine part | the 48 KB window (43,024 B used, 6,128 B free at P12 close) | `codec_stack` 9 KB (now the engine thread's, on the COP), DSP `ICODE`/`IBSS`, the COP's idle stack | uncached |
| Core IRAM, consumer part | same window | `fiq_playback` + `eng_pcm_refill` (the CPU's FIQ path), `dma_play_data`, the mixer's channel tables and `downmix_buf` (2 × 1 KB), the CPU's FIQ stack; the COP's FIQ stack (0b, 1 KB) stays reserved for X1 | uncached |
| Core IRAM, CPU part | same window | `main`'s 8 KB stack, the CPU's IRQ stack, `core_sleep`, `switch_thread`, `set_cpu_frequency` | uncached |

**Rule 3 ("IRAM is for codecs") holds.** The engine *is* the codec and its DSP; the consumer's IRAM is today's FIQ and mixer path, unchanged in owner.

**Budget.**
- E0–E6 add **≤ 256 B of IRAM** (`eng_pcm_refill`, the mode switch, the kick).
- They add **0 B** to the codec window.
- No IRAM moves between owners.

**Apple's IRAM overlay windows** (three windows swapped per job, `0x4001_8800`, [apple-RE]) are **not adopted now**. Our codec loader already swaps the codec's IRAM per format, which is an overlay per codec. Revisit them if E5 measures timestretch at 1.5× or a 10-band EQ above about 3 MHz-eq on the COP.

**Banking (P-8, unknown).** The CPU's `main` stack and the COP's `codec_stack` are both in the core IRAM window and both hot during lit play. The datasheet promises a zero-wait cross-bar. E6's bench adds a two-core IRAM read loop (same 32 KB region vs different) before any claim. If it shows contention, the stacks swap regions.

---

## 10. Cache coherency, per object

| Object | Home | Writers | Readers | Rule |
|---|---|---|---|---|
| `codec_queue`, `audio_queue`, `buffering_queue` | `.ncbss` (`SHAREDBSS`) | both (corelock) | both | uncached (exists) |
| `eng_slot[]` | `.ncbss` | the CPU fills, the COP marks done | the COP | uncached; one writer per field |
| `eng_now_out`, `eng_now_thr` | `.ncbss` | the consumer (CPU FIQ); the engine thread (COP) | CPU threads | uncached seqlocks, one writer each |
| `eng_ring` (bitstream) | `.ncbss` | the CPU (window, `w_off`), the COP (`r_off`, `low_armed` set); the CPU clears `low_armed` | both | uncached single-writer words; window fields only under the handshake |
| Bitstream bytes | audio buffer, cached | the CPU (fills, ATA DMA) | the COP | CPU: write back before `w_off`. COP: the safe-mark drop (§5.2) |
| Engine guard | audio buffer, cached, line-isolated | the COP | the COP | private |
| `eng_pcm_ring` | `.ncbss` | the COP (`w_idx`, `low_wm`, `wm_wait` set), the consumer (`r_idx`, `arm_idx`, `wm_wait` clear) | both | uncached single-writer words (§6.1) |
| PCM bytes (`pcmbuf`'s buffer) | audio buffer, cached, line-isolated | the COP | DMA (physical); the CPU's mixer (uncached alias, MIX only) | the COP writes back before `w_idx` (§6.2); the CPU never caches them |
| Chunk descriptors | `pcmbuf`'s area, cached for the writer | the COP | the consumer, through the uncached alias | written back with the chunk, before `w_idx` |
| Producer state (DSP, `pcmbuf` producer, codec statics) | engine region (§8) | the COP | the COP | private; CPU changes only by command |
| Consumer state (mixer, `dma_play_data`, `downmix_buf`, `channels[]`) | IRAM / `.ncbss` | the CPU | the CPU; DMA | uncached anyway (movable for X1) |
| `codec_stack` | IRAM | the COP | the COP | uncached anyway |
| Codec slot | SDRAM, cached | the CPU (load), then the COP | the COP | hand-off with D and I drops (§5.5) |
| `CODEC_ID3` mp3entry | cached (`playback.c`) | the CPU before `ENG_RUN` | the COP through the uncached alias | the CPU does not write it while the codec runs |
| `PLAYING_ID3.elapsed`/`offset` | cached | the CPU only (from `eng_now_out`) | the CPU | no FIQ write any more |
| Viz tap ring and head | uncached alias | the CPU (the mixer's tap) | the shell | unchanged |
| WM8758 shadows (`dac_state`, …) | `XCORE_BSS` | the CPU | the CPU | stays uncached (it costs nothing) |
| `pcm_pp_stats`, the PCM lock's depth | IRAM/`.ncbss` (step 2) | the consumer's core (the CPU in the hybrid) | either | step 2's contract, unchanged |
| `global_settings` | cached | the CPU | the CPU only | the engine never reads it (the reach check fails otherwise) |

**Never in cached memory:** anything both cores write; any word one core's interrupt writes that the other core reads.

**Always line-isolated:** the rings' ends, the guard, `pcmbuf`'s area and the engine region.

---

## 11. The video soundtrack and USB audio (#144)

**Recommendation: keep both on the CPU for now.** (Q-A1) In the hybrid this is the natural state, not a hand-back:
- **Video's soundtrack** (`vplayer.c:1511`) plays on `PCM_MIXER_CHAN_VOICE` through `al_cbs`; its callback runs inside the mixer's refill. The mixer is the CPU's, so it runs against `vplayer`'s state on the same core, as today. Music is stopped first (class C), so the engine is idle. **No `ENG_SINK` is needed for video.**
- **Plugins and `devbcm.c:1676`** drive mixer channels: CPU, unchanged.
- **USB audio** runs `dsp_process()` inside a CPU interrupt, and the DSP is the engine's. That one crossing gets the hand-back: `ENG_SINK release` (music stopped) → the COP writes back and drops its cache, `state = DSP_RELEASED`, ack → the CPU `xcore_acquire()`s and owns the DSP until `ENG_SINK reclaim` (the mirror image). One cache operation per hand-off, as in R7. `XCORE_ASSERT_ENGINE` reads the owner word (`eng_dsp_owner`, `.ncbss`).

**Why not in the engine yet:** no dark-play benefit (video is lit, USB audio is on external power), and Fit-2 saw the decode stand still under video (`core-layout-decision.md` §3).

**Later (step 9).** In the engine, video's audio would survive the CPU-side stalls of #155 (dark storage wakes stalling video threads by 580–680 ms [silicon]) if its PCM is queued ahead through the PCM ring. That is the reason to revisit it once A has shipped.

**Under X1** the mixer moves with the consumer, so video and plugins would mix on the COP: X1 brings revision 1's full sink hand-back back (§12.2).

---

## 12. The extension X1: no CPU wake for audio at all

### 12.1 What X1 adds over the hybrid

| | Hybrid (E6) | X1 |
|---|---|---|
| CPU wakes/s for audio, dark play, 44.1 kHz | ≈ 2.7 (FIQs) + rare events | **≈ 0** (rare events only: `EV_LOW`, `EV_META`) |
| PCM lock | the CPU's (`CPU_INT_DIS`/`EN`), as today | **per core**: the consumer's core masks its own source; locks taken by the engine are local; CPU callers (click, tap) send commands |
| Who wakes the COP at the watermark | the CPU's FIQ (`eng_kick`) | the COP's own trigger, locally (no cross-core wake at all) |

**What the 2.7/s is worth.** In dark play the CPU's 100 Hz tick still wakes it 100 times a second until the coarse tick (`core-layout-decision.md` step 8, T-1) lands. **Until then X1 saves ≈ 2.7 of ≈ 103 audio-plus-tick wakes/s (≈ 3 %)**; after a coarse or stopped tick it is the difference between a CPU that wakes ~3 times a second and one that sleeps through a whole track. X1's value therefore rises with step 8, and the owner's "matched and beat Apple" (§13) is fully true only with both.

### 12.2 X1's costs and risks

- **Unproven silicon** (each probe in §12.5): the COP route, the COP's F bit, the wake from `PROC_SLEEP` on a COP-routed interrupt.
- **The consumer moves**: the mixer, the click and the tap become commands (`ENG_BEEP`: kind, tone, ms, amplitude, the pre-rendered click buffer the COP owns; `ENG_TAP`: open with n/memory/bytes, or close as a send; `ENG_PLAY`: the `may_play` bits). The click gains one cross-core post (tens of µs).
- **Video, plugins and USB audio** must hand the whole consumer back to the CPU (`ENG_SINK` with `pcm_pp_set_core(CPU)` while stopped), revision 1's §10 hand-back.
- **A COP interrupt storm** if the route is ever set while the COP's handler is missing (Apple's COP vector is a trap, `apple-cop-audio.md` §2.1 step 9). The route is set only by the consumer's start on the COP and cleared in Apple's stop order (§3 there).
- **Two FIQ vectors through one pointer** (`fiq_function`, shared): 0b's `fiq_by_core` split already handles it [code].

### 12.3 The one switch

`corelayout_pcm_trigger` (boot-time, release-hidden `config.cfg` key; developer UI row): **`cpu_fiq`** (the hybrid, default) or one of the X1 triggers below. What it changes, and nothing else:
1. **`pcm_pp_set_core(core)`** (step 2's driver, built): which core's mask and priority carry source 26, or none for the timer/poll triggers.
2. **The consumer shim** (`eng_out_*()`, from E1): a local call when the consumer's core is the caller's, a command otherwise. Every `mixer_*`, tap and play/pause call already goes through it from E3.
3. **The trigger stub** that calls `eng_pcm_refill()`: the FIQ, an IRQ branch, or the timer/poll loop.

`eng_pcm_refill()` itself, `eng_pcm_ring`, `eng_now_out` and the mixer's IRAM tables are the same in every setting, because §8.3's `--roots pcmout` check keeps the consumer free of cached statics. Switching hybrid ⇄ X1 is a reboot and a key, and the host tests run the consumer under both shims.

### 12.4 How "the CPU never wakes for audio" could work: seven hypotheses (owner addition)

Apple never tried any of these (`apple-cop-audio.md` §0, §2.1: the COP never writes `COP_INT_PRIORITY`, never reads `COP_FIQ_STAT`, never programs a DMA channel). So Apple's code is no evidence against them; silicon, Rockbox and the register layout are.

| # | Mechanism | For | Against | Discriminating probe and expected result |
|---|---|---|---|---|
| **a** | **Source 26 as a COP FIQ**: route from the CPU (`CPU_INT_DIS`, `COP_INT_PRIORITY \|= DMA_MASK`, `COP_INT_EN`); the COP clears its own CPSR F | The controller has per-core enable and FIQ-select registers, the COP's block mirroring the CPU's at +0x10 [rockbox-derived, `pp5020.h`]; the CPU's FIQ select is proven daily [silicon]; 4gh's failure is explained by F=1 (`cop_cpsr=0x5f`) [silicon + code; quirk 74]; Rockbox already gives the COP a FIQ stack (0b) [code] | Apple never uses it; `COP_INT_PRIORITY`'s FIQ meaning is only inferred (rank 4 (b) in `apple-cop-audio.md` §5); a FIQ arriving at a COP in `PROC_SLEEP` may not wake it (hypothesis (e) there) | **X-a** = `apple-cop-audio.md` §7 R1–R7. Pass: R4 `raw=1 cop_fiq_stat=1 cop_irq_stat=0 cpu_fiq_stat=0`; R5 `cop_n=1`; R6 `cop_fiq≈215 (204..226) cpu_fiq=0`, clean tone, with the COP **parked** between FIQs |
| **b** | **Source 26 as a COP IRQ** (the same route without the priority bit; the COP's `irq_handler` branch) | P-2: Timer 2 routed to the COP as an IRQ counted 1000/1000 and woke the parked COP [silicon]; no FIQ vector sharing, no F-bit discipline | IRQ latency: the refill deadline is the FIFO's 0.36 ms, and the COP masks IRQ in its kernel critical sections; with the engine as the only COP thread those are short, but unmeasured | **X-b**: route as IRQ, 64 KB runs of a tone for 10 s, the COP parked; at each entry read `IISFIFO_CFG` TX free entries. Pass: `cop_irq≈27` (10 s / 372 ms), `fifo_empty=0`, `free_max ≤ 12` (never drained), clean tone. A `free_max` near 16 means latency is marginal |
| **c** | **DMA0 auto-reload / chaining, or the second engine, so runs need no re-arm** | `pcm-pp.c`'s stop says `DMA0_STATUS` "will have been reloaded automatically with size in `DMA0_CMD`" [code]; command bits 29 and 27 and field [26:24] are unexplained (`dma.md` §6.3: Apple never writes bit 29); a channel that restarts itself would make the interrupt optional (one per lap, or none) | Apple re-arms every run from software and builds no descriptor chain [apple-RE, `dma.md` §6.3–§6.4]; the count is 16 bits, so a lap is still ≤ 64 KB; the second engine has never fed I2S and Apple's driver for it is BCM-only [apple-RE, `dma.md` §6.6] | **X-c**: DMA0 on a 4 KB tone buffer, interrupt bit off, for each of {bit 29, bit 27, both}: start, sample `DMA0_STATUS` and `DMA0_RAM_ADDR` every 2 ms for 100 ms. Pass (auto-reload exists): the count reloads and the tone continues past 23 ms with no re-arm; the address wraps to the start (circular) or runs on (linear). Expected [hypothesis]: no reload, one 23 ms blip, as 4gh's one-run blip |
| **d** | **The I2S TX FIFO interrupt (source 10) routed to the COP** as a "run ended" signal | Rockbox's PP5002 and recording paths take `IIS_IRQ` as a FIQ [rockbox-derived, `audio.md` §2] | The TX attention level is the same field that sets the DMA request level (`IISFIFO_CFG[1:0]`); with DMA feeding, the FIFO dips to that level at every request, ≈ 11,000/s, so an enabled TX interrupt would storm. It fires *after* the run, with less slack than source 26 | **X-d**: no enable at all; while 64 KB runs play on the CPU route, sample `INT_STAT` bit 10 every 50 µs for 1 s. Pass (useful): bit 10 rises only near run ends (≈ 3 per second). Expected: it toggles constantly (useless, ranked low) |
| **e** | **The COP polls DMA0 from its own timed sleep** (`COP_CTL = PROC_WAIT_CNT \| n`, bit 30) and re-arms; no interrupt route | `PROC_WAIT_CNT` ("sleep until end of countdown") is defined [rockbox-derived, `pp5020.h:184`]; no routing, no vector; a timed wake just before each run end plus a short spin | The count's unit is unknown; `core_wake()` clears only the sleep bit (31), so **a bit-30 sleep is not ended by `core_wake`**: commands to the engine would wait out the countdown (up to one run, 372 ms); no code in Rockbox or Apple uses it, so its behaviour is unseen | **X-e**: on the COP, `COP_CTL = PROC_WAIT_CNT \| n` for n ∈ {1, 16, 256, 4096, 65535}, `USEC_TIMER` before and after; then n = max with a CPU `core_wake(COP)` 1 ms in. Pass: elapsed ∝ n (unit derived), the core_wake case logged either way (expected: not ended early) |
| **f** | **Timer 2 on the COP paced to the run rate** (one-shot at run length − 2 ms), then spin on `DMA0_STATUS` and re-arm; or the mailbox (`CPU_QUEUE`/`MAILBOX_IRQ` source 4) | Timer 2 → COP IRQ works and wakes the parked COP [silicon, P-2]; Timer 2 and the WM8758's clocks both derive from the 24 MHz crystal, so no long-term drift [hypothesis]; the spin costs ≤ 2 ms per 372 ms (≈ 0.5 % of the COP at its band clock) | Timer 2 has one owner at a time and the backlight fade takes it (`pp-governor.md` §17.3) — dark play has no fade, so time-sharing is possible but must be designed; drift of the DAC's PLL against the crystal is unmeasured; the mailbox's semantics are unknown (open question 7) and no evidence ties it to DMA | **X-f**: 64 KB runs on the COP, re-armed by a Timer 2 one-shot + spin, for 30 s; log `spin_us` min/max and `fifo_empty`. Pass: `spin_us_max ≤ 2,000`, `spin_us_min > 0` (no late wake), `fifo_empty=0`, clean tone; the drift is the slope of `spin_us` over 30 s |
| **g** | **The ring in IRAM instead of SDRAM** | IRAM is uncached and shared: no write-back, and the DMA already reads IRAM daily (`downmix_buf` is IBSS) [silicon] | It changes coherence, not wakes: 6 KB of free core IRAM holds ≈ 35 ms, so runs would be short and **wakes would rise** (≈ 29/s); the 80 KB codec window is the codec's | **No probe.** Rejected for the PCM ring; the mixer's frames stay in IRAM as today |

**Ranking** (likelihood of a CPU that never wakes for audio × value ÷ cost):
1. **(b) COP IRQ**: the route mechanism is proven by P-2; the open question is latency margin, which X-b measures directly. Cheapest code (an `irq_handler` branch).
2. **(a) COP FIQ**: the cleanest end state (no IRQ masking concerns) and the owner's original intent; the retest is ready and its R4 step also answers (b)'s routing question.
3. **(f) Timer 2 pacing**: every part proven except the spin's drift; no new interrupt route; costs a Timer 2 time-share design.
4. **(c) auto-reload**: low prior but the largest payoff (the interrupt becomes optional); cheap to test.
5. **(e) `PROC_WAIT_CNT`**: unknown unit and the `core_wake` caveat; useful mainly as a timer-free variant of (f).
6. **(d) the I2S FIFO interrupt**: likely storms; tested only because it is free.
7. **(g) IRAM ring**: rejected on arithmetic.

### 12.5 The probe plan: one sitting, about 3 minutes, automatic

One developer bench row, `x1-probes`, after the p1 retest's code lands (Q-A5). PCM stopped, DAC held awake, the owner listens with headphones. It runs basic to advanced, logs one line per step (≤ 62 characters), skips dependants on a failure, and asks one question at the end ("Did each tone sound clean? yes / no / which"):

| Order | Probe | Time | Depends on | Settles |
|---|---|---|---|---|
| 1 | **X-d** status sampling (no enable) | 1 s | – | (d) in or out |
| 2 | **X-a R1–R4**: route, clear F, one 8 KB run with F masked, read the four status views | 0.1 s | – | the route itself; FIQ vs IRQ select (feeds both (a) and (b)) |
| 3 | **X-a R5–R7**: take the pending FIQ, 10 s of tone with the COP parked, unroute | 11 s | step 2 shows `cop_fiq_stat=1` | (a), including wake-from-sleep |
| 4 | **X-b**: IRQ route, 64 KB runs, 10 s, FIFO free entries logged | 11 s | step 2 shows the source reaches the COP's view at all | (b) and its latency margin |
| 5 | **X-e**: `PROC_WAIT_CNT` calibration and the `core_wake` case | 2 s | – | (e)'s unit and caveat |
| 6 | **X-c**: auto-reload bit sweep, 3 × 100 ms | 1 s | – | (c) |
| 7 | **X-f**: Timer 2 pacing with spin, 30 s | 31 s | – | (f), and the crystal-vs-DAC drift |

**Reading the results.** If (a) or (b) passes, X1 uses it (prefer (a) if both pass and `free_max` in X-b exceeds 12). If both fail but (f) passes, X1 is the timer-paced COP consumer. If (c) shows auto-reload, the run limit and interrupt plan are revisited for every trigger. Every result, pass or fail, goes into `pp5022-reference/quirks.md` (with its tag), `data-paths.md` cell 10 and `hardware-features.md` (P-19 COP routing, P-20 forced interrupts dropped from this design, P-21 DMA0 auto-reload, P-22 `PROC_WAIT_CNT`) in the same change as the results.

---

## 13. Matching and beating Apple

Dark play, 44.1 kHz stereo, the screen off, audio only (the 100 Hz tick is counted separately, §12.1).

| | Apple 1.3 (`apple-cop-audio.md`) | Today (layout C) | **Our hybrid (E6)** | **Our extension (X1)** |
|---|---|---|---|---|
| Who decodes | the COP's work engine [hypothesis, strong] | the CPU (30 MHz) | the COP | the COP |
| Who takes the PCM interrupt | the CPU (FIQ, source 26) [apple-RE] | the CPU (FIQ) | the CPU (FIQ) | the COP (FIQ, IRQ or timer, §12.4) |
| **CPU wakes/s from audio** | one per DMA run; run length not traced [unknown] | ≈ 345 FIQs + ≈ 60 thread wakes [code; silicon 4gh] | **≈ 2.7** (64 KB runs) + rare events; **target ≤ 5** | **≈ 0** + rare events |
| COP wakes/s | per message from the CPU [apple-RE] | 0.6 (parked, K-1) [silicon] | ≈ 1 per refill burst (the kick) + commands | ≈ 2.7 (its own trigger) + 1 per burst |
| Pause latency | [unknown] | ≤ 1 frame (5.8 ms) | ≤ 1 frame (the cut, then the fade) | same + one post |
| Click latency | [unknown] | ≤ 5.8 ms | ≤ 5.8 ms after the cut (fallback ≤ 372 ms on the first detent from dark, §6.5) | + one post (tens of µs) |
| DSP/EQ change heard after | [unknown] | the PCM buffer's level (2–3 s) | the same | the same |
| Risk | proven on silicon (Apple shipped it) | proven | **low**: every mechanism proven (§0) | **medium**: unproven route or timer pacing; storm risk; consumer hand-back for video/USB/plugins |
| Memory (beyond today) | – | – | +0 B ring (pcmbuf reused, rounded to 64 KB); 32 KB engine guard; ≤ 1 KB uncached statics; ≤ 256 B IRAM | as hybrid; the COP's 1 KB FIQ stack (reserved since 0b) |
| Bitstream to the decoder | [unknown] | local (CPU) | cached reads, one drop per fill burst (§5.2) | same |

**Matching Apple** = the hybrid: the same division of labour (the COP computes, the CPU owns the device), on proven silicon paths. **Beating Apple**: long runs and the watermark kick bound the CPU's audio wakes at ≈ 2.7/s and the COP's at about one per refill burst, with the bitstream read cached; X1 then removes the CPU from audio entirely, which Apple never attempted.

---

## 14. Increments, tests and effort

**Rules for every increment:**
- It sits behind `HAVE_CORE_LAYOUT_A` (developer builds) and the boot-time switches `corelayout_audio_core` (the engine's core) and `corelayout_pcm_trigger` (§12.3) — release-hidden `config.cfg` keys, 19-9's pattern; they expire as the developer UI's rules say.
- **Superseded for 0.8.0-rc.6 (owner, 2026-11-07):** the PCM's FIQ, the refill, the mixer and pcmbuf's consumer run on the COP from boot in every shell build (`apps/audio_cop.c`; `cop-shared-state.md`, layout A). The CPU route remains only as the logged fallback. ~~The CPU route stays the default~~ until A-1 passes. (This is the engine's route. The codec's decode itself is on the COP by default since 2026-11-07, owner: Fit-2's per-run job, `codec_cop.c` `CPL_MODE_COP`; `../changes-from-rockbox.md` 2.67.)
- From E1 on, both routes go through the same API, so every gate tests the CPU route.
- The engine refuses to start on the COP while the UI split is `cop` (as `pgov2_apply` does).
- **E3's long runs ship on their own merits:** they cut the CPU's FIQs in layout C too (decode still on the CPU), so E3 is useful before E5.

### 14.1 Increments

| Inc. | What | Needs | Owner (agent) | Effort |
|---|---|---|---|---|
| **E0** | **Contracts:** `apps/audio_engine.h` (§3, §4, §5.1 and §6.1 structs, `ENG_*` and `EV_*` ids), `apps/audio_engine.list` with producer/consumer tags, one shared stub, a host round-trip test of the slot, record and ring layouts (every field at its natural offset, lean rule 12) | – | playback-audiobook | S (½ day) |
| **E1** | **The boundary on one core.** The `audio` thread's and the UI's producer calls become commands, records and events through the shim; consumer calls go through the consumer shim (local); `pcmbuf.c` split into producer and consumer state; codec loads move to the `audio` thread; the DSP profile and its shadow; positions to `eng_now_out`; DAC wake before sound-starting commands. Everything still runs on the CPU | E0 | playback-audiobook | **M–L** (3–4 days) |
| **E2** | **The bitstream ring:** export the codec's handle window; local request/advance/read/forward seek; `EV_REQ`/`ENG_DATA`; `EV_LOW` replacing the poll as the trigger; the engine's guard; the safe-mark drop (a no-op on one core) | E1 | playback-audiobook (`buffering.c`'s codec-facing paths only) | M (2 days) |
| **E3** | **The PCM ring and long DMA runs on the CPU:** `eng_pcm_ring` cursors over `pcmbuf`'s buffer (rounded to 64 KB); `eng_pcm_refill()` in IRAM; PASS/SHORT/MIX modes and the cut (`dma_tx_stop()`), the lit/dark word; the mixer reading the ring through the uncached alias in MIX; `dma_tx_buf_prepare()`'s write-back dropped on this path; the kick hook (a no-op while the engine is on the CPU); `--roots pcmout` clean | E1 | firmware-platform (`pcm-pp.c`, `pcm_mixer.c`) + playback-audiobook (`pcmbuf.c` consumer side) | **M** (2–3 days) |
| **E4** | **The line region:** the `app-pp.lds` groups; `ENG_MEMBERS` strict; the reach check's engine and pcmout roots and read list; `eng_link_check.py`; `XCORE_ASSERT_ENGINE`; `xcore_map_check` entries | E1 (the members are known) | firmware-platform (lds, `xcore.h`); tooling (scripts) | S (1 day) |
| **E5** | **The engine on the COP, hybrid mode:** `codec_thread_init()` creates the thread on `corelayout_audio_core()`; the producer's write-back per commit (§6.2); the engine's wait loop and the FIQ's `eng_kick()` (§7) with its watchdog fallback; load and unload hand-offs; `ENG_SINK` for USB audio's DSP; COP panic → CPU log; per-core boost bookkeeping per `pp-governor.md` §17 step 4 if landed, else B1's deferral. **No interrupt moves** | E1–E4 | firmware-platform (system, kick) + playback-audiobook (engine) | **L** (3–4 days) |
| **E6** | **Bench sitting A-1** (§14.4); the default flips (one line) on a pass | E5 | orchestrator + owner | owner ~2.5 h |
| **X1** | **The extension:** the trigger chosen by §12.5's results behind `corelayout_pcm_trigger`; the consumer shim's command path (`ENG_BEEP`, `ENG_TAP`, `ENG_PLAY`); per-core PCM lock; the full sink hand-back for video, plugins and USB audio; the route set/clear in Apple's stop order | E6 pass **and** a passing X-probe (a, b or f) | firmware-platform + playback-audiobook | **M** (2–3 days) + bench ~1 h |

Retiring the split (`core-layout-decision.md` step 7) and the coarse tick (step 8) are out of scope here.

### 14.2 Host tests (run by `scripts/test.sh`)

| Test | What | Pass |
|---|---|---|
| `tests/engine/test_layout` (E0) | slot, record and ring layouts, `offsetof`s, sizes ≤ budget, round trip | exact |
| `tests/engine/test_now` (E1) | `eng_now_out`/`_thr` through `tests/ppgov/test_idle.c`'s store-by-store harness, including a reader interrupted by the writer | 0 torn of all schedules; a mutation (no re-check) is caught |
| `tests/engine/test_profile` (E1) | `tests/audioxcore` extended: the profile applied by the engine while a model reads the DSP state, 300 seeds | 0/300 torn; the per-field control still tears (> 0) |
| `tests/engine/test_ring` (E2) | **two-core model** on `tests/febe/cachemodel.c` (per-core 8 KB write-back caches, 16 B lines): CPU writer (chunks, ATA-DMA and copy modes), COP reader (random request sizes, wraps, forward and backward seeks, re-window, track switch), random interleavings | 0 stale bytes over 10⁴ seeds; drops ≤ 1 per burst in steady state; mutations (no drop; drop before snapshot; a CPU write into `[r_off, w_off)`) each caught |
| `tests/engine/test_pcmring` (E3) | the same two-core model: COP producer (cached writes, publish per chunk, markers), a DMA model reading physical memory, the consumer as FIQ (random run sizes, cuts, flush, pause/resume, underrun); then the same with the consumer on the COP (X1's shim) | 0 stale samples at the DMA over 10⁴ seeds; markers reached in order; `w_idx`/`r_idx` each written only by its owner (asserted); mutations (cursor before publish; consumer writes `w_idx`) caught |
| `tests/engine/test_kick` (E5) | the engine's wait loop vs the kick, store by store, with a FIQ arriving inside `core_wake`'s protocol | no lost wake in any schedule; the mutation (no re-check after `wm_wait = 1`) loses a wake |
| `tests/engine/test_engine` (E1/E2) | the engine's state machine with a fake codec emitting a ramp: play, pause, seek, auto and manual skip, crossfade on/off, stop mid-track, DSP release/reclaim | PCM continuous across gapless boundaries; markers and events in order; every slot released |
| `tests/copk1` (exists) | still passes with the engine's wait loop as the COP's only waiter | unchanged |

### 14.3 Simulator

The simulator has one core, so these scripts prove the semantics and the CPU route, not coherence:
- **E1–E3:** every existing playback, audiobook-speed, EQ and Now Playing script passes unchanged.
- **New `tests/ui/ipodui-engine-cmds.script`:** play, pause, seek ×5, skip ×5, an EQ preset cycle, speed 1.0 ↔ 1.5, a click with and without music, the visualiser open/close, a backlight-off period (PASS mode). It checks trace lines from the named macro **`ENG_TRACE`** (`DEBUGF` in the simulator; the `eng` devlog tag on the device), including the mode changes (`pcm mode=PASS/SHORT/MIX`) and cuts. A rule that finds no `eng` line fails.
- **Position checks** use `wait_pos`.
- **No goldens.** There is no UI change.

### 14.4 Bench (A-1 acceptance; `core-layout-decision.md` §5)

**Method.** Power is judged by proxies only (no meter): mV/h by least squares in the 3,635–3,955 mV band; ABAB arms with a reboot each (Q-A2); `idle_pct`, `cop_idle_pct`, `m0 wakes`, `clk` stats, `pcm underrun`, `pcmpp.fiq` per core, `eng_now_out.runs`.

| Row | Arms | What the owner does | Pass |
|---|---|---|---|
| **a1-runs** (E3, before E5) | C, C+E3 | Hold on, dark, 10 min FLAC | `fiq/s`: ≈ 345 → **≤ 3** at 44.1 kHz; `pcm mode=PASS` ≥ 95 % of the time; underruns 0 |
| **a1-dark** | C, A (FLAC album; one MP3 arm pair) | Hold on, dark, 30 min per arm | mV/h(A) ≤ mV/h(C) in both repeats; **CPU wakes from audio ≤ 5/s** (FIQs + COP→CPU events); **CPU idle ≥ 90 %** (C: ~14 % at 30 MHz); COP wakes ≤ 2/s beyond commands; **underruns 0** |
| **a1-lit** (L-1) | C, A | wheel volume in Now Playing 1 min; fast Songs scroll 30 s; the visualiser 1 min | `uilat o100(A) ≤ C`; the UI thread's `m0 slice max` no longer shows the codec (#156: `codec=` absent from the CPU) |
| **a1-stress** (R-1) | C, A | art pass, scroll 60 s, visualiser open/close ×10, USB plug/unplug once | underruns 0; no `xcore_off*` counts |
| **a1-click** | A | dark play 1 min, then turn the wheel (the first detent from dark); scroll with music 20 s; with no music 20 s; back to menus | `pcm cut` per first detent; **beep-to-first-frame ≤ 10 ms** logged; no artefact heard at the cut; no tone after the wheel stops (D15) |
| **a1-pause** | A | pause/resume ×20 in dark PASS mode | `pause_ms ≤ 20` (button to DMA stop); resume continues at the same sample (no skip heard) |
| **a1-dsp** | A | EQ preset cycle ×20 while playing; audiobook speed 1.0 ↔ 1.5 ×20; crossfeed on/off; replaygain mode switch | no glitch beyond one ramp; `dsp_sum` equal on both sides for every set; 0 torn |
| **a1-seek** | A | seek ±30 s ×30; skip ×30; a gapless album across 3 boundaries | no gap at the boundaries; every `EV_SEEK_DONE` within 300 ms (flash); `eng drops` logged |
| **a1-wm** | A | 10 min dark | 0 underruns, 0 COP timeouts armed, no `eng wm fallback`; kicks ≈ refill bursts |
| **a1-video** | A | music → a video → music | the soundtrack plays (CPU mixer), music resumes |
| **a1-resume** | A | power off mid-track while playing; USB connect mid-track | the resume position is within 1 s of what was heard (it comes from `eng_now_out`) |
| **x1-probes** (§12.5) | – | listen, one question | per §12.4 |
| **x1-dark** (after X1) | A, A+X1 | Hold on, dark, 30 min per arm | CPU audio wakes ≈ 0; underruns 0; mV/h(X1) ≤ mV/h(A) |

**A passes when every a1 row passes.** Then the default flips (E6). X1's default flips on x1-dark.

---

## 15. Risks

| # | Risk | Mitigation |
|---|---|---|
| 1 | **The cut is audible** (DMA stop and restart in PASS → MIX) | a1-click; automatic fallback to "SHORT while lit, PASS only dark" (§6.5), logged |
| 2 | **The kick loses a wake** (a FIQ inside `core_wake`'s protocol, or `wm_wait` racing the re-check) | `test_kick`; the engine's watchdog falls back to 19-1c's timeout, logged |
| 3 | **Stale PCM at the DAC** (a cursor published before the write-back) | `xcore_publish()` before the `w_idx` store; `test_pcmring`'s mutation; a1-dark underruns and listening |
| 4 | Stale reads: engine code reading CPU-written cached statics (crossfade settings, `current_tick`'s home, settings-derived tables) | the reach check's **read list** (§8.3); the profile carries every setting |
| 5 | Unmeasured drop and publish cost (fit2 R2): `xcore_acquire()` per safe-mark crossing; `xcore_publish()` per committed chunk on the COP | E2/E5 log `drop_us_max`, `pub_us_max` and the counts; both are per burst or per chunk, not per frame |
| 6 | Gapless and crossfade regress: the marker logic now spans two cores | `ENG_RUN` carries change and run together; runs end at markers; `test_engine`; a1-seek's gapless album |
| 7 | Clock changes from the COP wake the CPU and spin on the PLL lock (`scale_suspend_core()`, `pll_wait_lock()`) | the per-track band (`pp-governor.md` §17.2); the engine's boosts are balanced (`cancel_cpu_boost()` on every wait) and counted |
| 8 | COP panics draw nothing and are lost | a COP panic posts `EV_LOG fatal`; the CPU panics with the message and logs it (no splash) |
| 9 | Persistence: the resume offset is read from a stale `PLAYING_ID3` | `PLAYING_ID3` is filled from `eng_now_out` at every read; a1-resume |
| 10 | Two routes to maintain until the flip | one API (E1), so the CPU route is the shim's local call and every gate exercises it |
| 11 | Coarse position in PASS mode (372 ms steps) | DMA progress interpolation for readers on the CPU (§4.3); runs end at markers |
| 12 | Higher sample rates raise the run rate (96 kHz: 5.9/s) | logged per track; ≤ 48 kHz is the A-1 target; the cap is a hardware limit (§6.4) |
| 13 | X1: the COP route fails, or storms | the hybrid is already the target; the route is set and cleared in Apple's order; §12.4 gives six other mechanisms |
| 14 | Dev builds with the UI split compiled in | the engine refuses the COP while `ucop` is on; the split leaves at step 7 |

---

## 16. Device-reality checklist (`docs/process.md`)

1. **Compiled features.**
   - Device-only: `HAVE_CORE_LAYOUT_A`, `HAVE_COP_COHERENCE`, `XCORE_CHECKS`, `NUM_CORES > 1`.
   - The simulator has one core: it runs the engine API, the ring logic and the PCM modes but no coherence. Coherence is tested by the host two-core models (`test_ring`, `test_pcmring`, `test_profile`, `test_now`, `test_kick`) and on the bench.
   - The CPU route is what the simulator and every gate run.
2. **Memory during playback.**
   - No `core_alloc` while playing. The engine guard (32 KB) and the log ring (3 KB) are carved when buffering builds the buffer, while stopped, and go back with it.
   - The PCM ring is `pcmbuf`'s buffer, rounded to a multiple of 64 KB (down on 32 MB units, up on 64 MB): < 64 KB either way.
   - The slots, records and rings' cursors are < 1 KB of uncached statics, none over 1 KB.
   - The viz tap's memory stays the shell's, as today.
   - The engine thread reuses `codec_stack`, and the Fit-2 worker's 9 KB stack goes.
3. **CPU at 50k.** None: the database stays on the CPU and the engine touches no library data.
4. **Storage wake-ups.**
   - Unchanged in count. Fills are still buffering's; `EV_LOW` replaces the poll as the trigger, and the deadline wait stays.
   - `EV_REQ NEED` can force a fill earlier than the watermark only when the codec has run dry, which is an underrun path today too.
   - Real timing: paused counts as playing, and the 200 ms sleep after a fill is untouched.
5. **LCD.** Untouched: on the CPU, with no new pushes. The backlight's lit/dark edge only sets the run-size word.
6. **Threads and locks.**
   - New: none. The codec thread moves to the COP; the Fit-2 worker goes.
   - Locks:
     - the PCM lock stays the CPU's in the hybrid (step 2's contract; per core under X1);
     - the slot mutex is CPU-only;
     - no mutex is taken from the UI on the engine's behalf;
     - the cursors are single-writer words, not locks.
   - No pointer is held across a yield into engine memory. The CPU holds only slot indices, record copies and ring cursors.
7. **Upstream splashes.** None reachable. COP errors go to `EV_LOG`, then the log file.
8. **Persistence.** The resume and status saves (the `audio` thread, `status_save`, power-off's `audio_stop()`) read `eng_now_out` through the `PLAYING_ID3` fill. Each trigger gets an a1-resume step in the real state (playing): power-off, USB connect, stop.
9. **Scale.** Not applicable: per-track, per-run and per-burst costs only.
10. **Deferred and retried work.** `EV_REQ NEED`, `SEEK`, `ENG_DATA`:
    - **ready:** answered at once;
    - **rebuffering:** the engine waits on its queue and still takes `STOP`/`SEEK`;
    - **stopped mid-wait:** `ENG_STOP` ends the wait and the codec returns.

    What the user sees is unchanged in each: a seek spinner in Now Playing as today, or silence while the data arrives. The PCM side never waits: an empty ring plays silence and counts an underrun.
11. **Stock firmware unchanged.** Every edit is under `HAVE_CORE_LAYOUT_A` or `HAVE_IPOD_UI`, so stock `.text` stays byte-identical. The release shape is unchanged until the flip.
12. **Existing paths doing the same job.**
    - **Fit-2:** `codec_cop.c`'s thunks and `CPL_MODE_COP`/`CPL_MODE_SCREEN`. The engine wins; Fit-2 stays the developer arm until A-1 passes, then is retired with the split. Two COP codec paths never run at once: `codec_cop_select()` refuses while `corelayout_audio_core() == COP`.
    - **19-9's `be_post(BE_SOUND)`** sound route: the click stays a CPU mixer call in the hybrid; under X1 it becomes `ENG_BEEP`.
    - **`pcmbuf_full_wait_ticks()`:** the fallback for §7.
    - **`watch_ticks()`:** the safety net under `EV_LOW`.
    - **`pcm-pp.c`'s short runs (`max_dma_chunk_size`):** kept for MIX mode; PASS/SHORT use the ring's run sizes.
    - **Writers into the PCM sink:** `pcmbuf` (through the ring), `beep.c`, `ipodui/click.c`, `voice_thread.c` (voice off in the shell; asserted), `vplayer.c`, `devbcm.c:1676`, plugins (`plugin.c`'s mixer table). All are CPU mixer callers in the hybrid, and each forces MIX.
    - **Driver state when a path is taken over:** `pcm_pp_set_core()` re-programs both cores' priority and enable while stopped (X1 only). The DAC's standby state stays CPU-owned.
13. **Builder predicate.** Not applicable (no background builder).
14. **Hardware blocks considered.** §17.

**Device-only checks.** These go to `ROADMAP.md` → Device checklist with the §14.4 procedures: every a1 row, x1-probes, x1-dark.

---

## 17. Hardware features (`docs/hardware-features.md`)

| Block | Row | Use here | CPU fallback | Unknowns and their probe |
|---|---|---|---|---|
| **COP as compute engine** | P-7 (used; Fit-2 [silicon] S36) | the producer: decode, DSP, `pcmbuf` | the CPU route (default until A-1) | – |
| **DMA0 long runs** (16-bit count, 64 KB max) | `dma.md` §2 | PASS mode: ≈ 2.7 FIQs/s | today's short runs (MIX) | a1-runs |
| **DMA0 mid-run stop and resume** (`dma_tx_stop()`) | [rockbox-derived] | the cut (§6.5) | SHORT-while-lit | a1-click |
| **Per-core interrupt routing** (source 26 to the COP) | data-paths cell 10; **add P-19** | X1 trigger (a)/(b) | the hybrid | X-a, X-b (§12.5) |
| **DMA0 command bits 27/29** (auto-reload?) | **add P-21** | X1 trigger (c), longer intervals | per-run re-arm | X-c |
| **`PROC_WAIT_CNT`** (timed core sleep) | **add P-22** | X1 trigger (e) | Timer 2 (f) | X-e |
| **Timer 2 on the COP** | P-2 passed, 4gh [silicon] | X1 trigger (f) only; time-shared with the backlight fade (`pp-governor.md` §17.3) | n/a | X-f |
| **Forced interrupts** (`INT_FORCED_SET/CLR`) | P-20 | **not used** by the hybrid (the kick replaces revision 1's forced wake) | n/a | none in this brief |
| **Mailbox and doorbell** (`MBX_MSG_*`, `CPU_QUEUE`; Apple's cross-core semaphores and post [apple-RE]) | none (PP open question 7) | not used: `core_wake` + kernel queues suffice | n/a | later probe; would replace Peterson locks if the set/clear bits prove atomic |
| **Apple's IRAM overlay windows** | none ([apple-RE] `apple-dual-core.md` §2) | not adopted (§9) | resident `ICODE` | revisit on E5's DSP cost |
| **IRAM cross-bar banking** | P-8 (unknown) | the two hot stacks in core IRAM | swap the regions | two-core read loop in the E6 bench |
| **COP clock skipping** | P-5 (known-unused) | later: a finer step for the engine alone | the band per track | out of scope |
| **WM8758 power states** | W-2 (used) | the CPU wakes the DAC before sound-starting commands | unchanged | – |
| **BCM2722 audio out** | B-14, X-3 | not here; video's soundtrack stays on the PP's CPU (§11) | n/a | P14 |

The new catalog rows (P-19, P-21, P-22) and every finding from x1-probes are written into `hardware-features.md`, `pp5022-reference/quirks.md` (quirk 55's rewording, `core-layout-decision.md` §4.1) and `data-paths.md` cell 10 in the same change as the bench results.

---

## 18. State of the art (`docs/algorithms-survey.md`)

| Hot path | State of the art | Adopted? |
|---|---|---|
| Cross-core command queue | Single-producer/single-consumer rings with one writer per cursor (Lamport, 1983) | **Yes** for both rings' cursors. Commands use the existing kernel queue: at ≤ 10/s it costs nothing measurable, and it is proven cross-core |
| Shared status | Seqlock (sequence counter; Lameter, Linux 2.6) | **Yes**: two records, one writer each; harness from `tests/ppgov` |
| Cache maintenance for streaming data | Ownership hand-off with bulk maintenance per generation (DMA-API style) instead of uncached or per-access maintenance | **Yes**: the safe-mark drop (§5.2) and the publish per committed chunk (§6.2). Ranged maintenance does not exist on the PP5022 |
| Audio output scheduling | Large DMA periods with a separate low-latency path for interactive sounds (the "deep buffer" / "fast mixer" split of modern mobile audio stacks) | **Yes**: PASS's 64 KB runs for music, MIX and the cut for sounds that must be heard now (§6.5) |
| Producer wake | Interrupt-driven watermarks instead of periodic polling (tickless designs) | **Yes**: the FIQ's kick (§7), with the 19-1c deadline as the fallback |
| Settings updates | Batched, versioned configuration snapshots (one message per set) | **Yes**: the DSP profile (S2 measured 300/300 torn → 0/300) |
| DSP algorithms (resample, EQ biquads, timestretch) | survey rows "Sample-rate conversion" (half-band polyphase, P19 19-13) and "EQ biquads" (adequate) | unchanged by this brief: the engine moves them, it does not rewrite them |

---

## 19. Ownership, read lists, acceptance

### 19.1 Ownership

| Files | Owner |
|---|---|
| `apps/audio_engine.{h,c}`, `apps/audio_engine.list`, `apps/codec_thread.c`, `apps/pcmbuf.c` (producer/consumer split), `apps/playback.c` (pcmbuf and codec call sites), `apps/buffering.c` (the codec handle's window and the guard), `apps/codecs.c`, `lib/rbcodec/dsp/dsp_misc.c` (the profile apply) | playback-audiobook (top model: core playback) |
| `firmware/pcm_mixer.c` (MIX reading the ring, the cut), `firmware/target/arm/pp/pcm-pp.c` (`eng_pcm_refill`, run modes, the kick), `firmware/target/arm/pp/app-pp.lds`, `firmware/target/arm/pp/thread-pp.c` (`eng_kick`, if needed), `firmware/export/xcore.h`, `firmware/export/corelayout.h` (the two switches), `apps/codec_cop.c` (the refusal), the x1-probes lab | firmware-platform |
| `scripts/xcore_line_check.py`, `scripts/febe_reach_check.py`, `scripts/eng_link_check.py`, `scripts/xcore_map_check.py`, `tests/engine/`, `tests/ui/ipodui-engine-cmds.script`, `apps/ipodui/devbench_core.c` rows | tooling |
| `docs/hardware.md` budget table, ledgers (`changes-from-rockbox.md` 2.63+), `ROADMAP.md` device checklist | orchestrator |

### 19.2 Read lists (≤ 3 each)

- **playback-audiobook:**
  - this brief, §2–§6;
  - `apps/codec_cop.c` (the thunks the engine replaces);
  - `docs/data-paths.md` R7.
- **firmware-platform:**
  - this brief, §6, §7, §9 and §12;
  - `firmware/export/corelayout.h` with `pcm-pp.c`'s step-2 changes;
  - `docs/pp5022-reference/apple-cop-audio.md` §1, §5–§7.
- **tooling:**
  - this brief, §8.3 and §14;
  - `scripts/febe_reach_check.py`;
  - `tests/febe/cachemodel.c`.

### 19.3 Acceptance

- **Host tests.** Every §14.2 test passes in `scripts/test.sh`.
- **Gate.** `validate.sh --quick` passes on each increment; the full gate passes once at integration. `xcore_line_check` is strict for the engine class, and the reach (engine and pcmout roots) and link checks are clean.
- **Budget.** At E5, against the branch base on the developer `fw-ui` (`arm-none-eabi-size`, `nm _end`): ≤ +6 KB text; statics ≤ 1 KB, all uncached; IRAM ≤ +256 B; codec window +0; `pcmbuf` ±< 64 KB (the rounding). The release shape is unchanged (it is guarded). Reported per increment.
- **Bench.** A-1 (§14.4) passes, with a1-runs first. Only then does the default flip. X1 needs x1-probes and then x1-dark.
- **Docs.** `data-paths.md` (cell 10, R7: both rings and the DSP hand-back recipes), `pp5022-reference/quirks.md`, `hardware-features.md` and the ledgers are updated in the same changes.

**Review images and videos.** None: no UI change. Evidence is the trace lines (`ENG_TRACE`, the `eng` devlog tag; every log line ≤ 62 characters) and the bench results file.
