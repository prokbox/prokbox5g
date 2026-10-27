# Fit-2 brief: the second core (COP) as a compute engine

*Branch `fit2-cop` from `morning` (b1f6485b48: dev42 + all of Fit-1). Audit item C11 (`docs/hardware-fit-audit.md`). Evidence tags as in `docs/pp5022-reference/`: [rockbox] upstream code or history, [external] someone else's measurement, [hypothesis], [estimate], [ours].*

## 0. Pre-flight

- **Owner's framing (2026-10-14):** the caveats (shared bus, per-core caches, hand-off cost, peripheral ownership) are the normal requirements of a multicore program, not reasons to avoid one. Design the firmware as a deliberately two-core system. The codec is the first client of a general mechanism, not a one-off move.
- **Owner's objective:** keep the two cores evenly loaded so the clock can stay at 24 or 30 MHz instead of one core running at 80 MHz while the other idles. Every gain is measured on the device; nothing here is claimed until Fit-1's M0 columns show it.
- **Agreed caveats:** we don't know whether Apple's firmware decodes on the COP (section 2.2). The gain is unmeasured until the A/B in section 10.
- **Correction (owner):** the video's soundtrack is moving to the BCM2722 (AAC on the chip, straight to the WM8758 over its own I2S). The COP runs the video's audio only as the interim path, and as the fallback for files the chip can't take.
- **Out of scope:** `apps/video/`, `bcmfw/` and `firmware/target/arm/ipod/video/` (another session). No device runs, no push, no full gate.

## 1. Goal

1. Give the PP5022's second ARM7TDMI real work: decode first, then the shell's pure-RAM passes and album art.
2. Make the clock policy aware of two cores (section 7), so work spread over both cores at 30 MHz replaces one core boosted to 80 MHz.
3. Keep the CPU-only path for everything: every client falls back to running on the CPU, and the simulator (one core) runs the same client code.
4. Stock and release builds are unchanged. Fit-2 code is in developer builds only (`HAVE_COP_JOBS`, `HAVE_COP_CODEC`, set with `HAVE_IPOD_UI_DEVMODE`) until the A/B passes.

## 2. Prior art

### 2.1 Upstream Rockbox [rockbox]

| When | Commit | What happened |
|---|---|---|
| 2007-03-04 | `82f9056988` (D. Ankers, "Dual core support for PP502x") | The **codec thread was created on the COP** (`IF_COP(, COP, true)`); the audio and voice threads stayed on the CPU. Shared flags moved to `NOCACHEBSS`. |
| 2007-03-26 | `66258a30a4` (M. Pekkarinen) | **Moved back to the CPU**, 22 days later: "A big step towards playback running on COP (not yet possible because more protection on file system level is necessary)". The codec thread then read files itself; the file system and ATA driver were not safe from the second core. |
| 2008-09-20 | `4e36a2b991` (M. Giacomelli, FS#9318) | **MP3's synthesis filter on the COP** (`mpa.c`, `MPA_SYNTH_ON_COP`), "~90 % speedup according to test_codec on the Sansa … allows MP3 decoding at 30 MHz without boosting". Fixed for seeks two days later (`3dedcfc454`) and made fully atomic (`5c903724fb`). Still in the tree: on PP5022/5024 the subband buffers are in IRAM (uncached), the hand-off is two semaphores in IRAM, and the codec does `commit_discard_dcache()` when the thread quits. |
| 2010 | `30e2f42c82`, mpegplayer | The FFT plugin and mpegplayer's video decoder run on the COP, with explicit `commit_discard_dcache()` / `commit_dcache()` at each hand-off. |
| since | — | The codec thread has been `IF_COP(, CPU)` ever since (`apps/codec_thread.c`). Every interrupt-driven driver is CPU-only. |

**What blocked the codec thread on the COP, and what still would today.** The codec thread is more than the decoder: it owns state the CPU shares with it in *cached* memory.

1. **Buffering** (`bufgetdata`, `bufread`, `bufadvance`): the handle structs are written by the buffering thread on the CPU. In 2007 the problem was file I/O; today it is the same data reached through the buffering layer's cached structs.
2. **pcmbuf**: its indices and chunk descriptors are shared between the codec thread (`pcmbuf_request_buffer`, `pcmbuf_write_complete`) and the PCM DMA interrupt on the CPU, and `pcm_play_lock()` masks the DMA interrupt (`CPU_INT_DIS`), which excludes the handler only when the caller is on the same core as the handler. A COP caller can race a handler already running on the CPU.
3. **The DSP**: its settings are written by the UI thread.

So moving the thread is still blocked, by more than the 2007 reason. Fit-2 moves the **decode** instead (section 5): the codec thread stays on the CPU and keeps all three, and the codec's own code runs on the COP. That is the MP3 synth filter's model (pure compute on the COP, explicit hand-offs) applied to the whole decoder.

### 2.2 Apple's RetailOS [external] (siggi notes `03` §57–58, `04` row 7)

- RetailOS runs both cores. In the emulator's cold boot the **COP works, sleeps and is woken about 15,000 times**, through the mailbox. Unlike Rockbox, Apple takes the **mailbox interrupt** (`CPU_QUEUE` bit 29, cleared by the reading core); the emulator stalled until it modelled that read-to-clear.
- **Apple's audio layer needs the COP.** With one core, a four-slot "voice" pool stays empty and a store through a null voice destroys the IRQ vector; with two cores the pool fills with four live pointers and the click sound plays.
- **Not shown:** which code runs on the COP, or whether it decodes music. [hypothesis] Apple's audio engine (mixing, click and probably decode) runs on the COP. Video codecs (`aacdec`, `h264dec`) run on the BCM, not on either ARM.
- The DRM path's `wake_cop` (`0x000cfb20`) runs late, after the failure the ledger was chasing, so it says nothing about playback.

### 2.3 What that means for us

- Rockbox's only lasting COP success is a **pure-compute split with uncached hand-off buffers**, and it measured large: MP3 unboosted at 30 MHz.
- Apple evidently built its audio around the second core.
- Both point the same way: put compute on the COP, keep shared state and peripherals on the CPU, and make every hand-off explicit.

## 3. Multicore architecture

### 3.1 Core roles

| Core | Owns | Never |
|---|---|---|
| **CPU** | UI and LCD (BCM host port), every interrupt (tick, ATA, I2S/DMA FIQ, wheel, USB), every peripheral, storage and the file system, buffering, pcmbuf, the DSP (for now), settings, the kernel tick | long compute loops when a COP job can take them |
| **COP** | compute: decode, then art decode and scaling, the shell's precompute passes, tag parsing from RAM, DSP later | peripherals, interrupts (it takes only the CPU's per-tick wake), file I/O, `core_alloc`, splashes, the LCD |

### 3.2 Data ownership and message passing

- **One owner at a time.** Every buffer has one owner core at any moment, and ownership changes only at the job channel's four points: **post, done, call, return** (`cop_job.h`). Nothing else is shared, except data that lives in uncached memory with a protocol (IRAM semaphores, `SHAREDBSS` rings, kernel queues).
- **The cache operations at each point** are a table in code (`copj_cache_ops()`, host-tested), not left to client discipline:

| Point | Core | Operation | When |
|---|---|---|---|
| post | poster (CPU) | write back | the job reads cached data the poster wrote (`COPJ_IN`) or loaded code (`COPJ_CODE`) |
| start | worker (COP) | write back + drop (data; code too for `COPJ_CODE`) | same |
| finish | worker | write back | the job wrote cached data the poster reads (`COPJ_OUT`) |
| collect | poster | write back + drop | same |
| call | worker | write back | always: the handler reads or writes the worker's memory through the uncached alias, and a later eviction must not overwrite what it wrote |
| return | worker | write back + drop | the handler wrote the worker's cached memory (`COPJ_CALL_WROTE`) |

- **Never a bare discard.** Every drop writes back first; a plain discard would lose the core's own dirty lines, its stack among them. Maintenance is whole-cache (8 KB) and per core (`cores-and-threads.md` §3).
- **Small values travel uncached:** a call's arguments and results are copied through `io[4]` in the worker struct (`SHAREDBSS`). A handler reaches the worker's buffers only through `cop_uncached()`.
- **Bulk data crosses by pointer with one cache operation per hand-off,** never per element: a 4.6 KB PCM frame, a 64 KB bitstream window, a whole art tile.

### 3.3 Placement

| Data | Where | Why |
|---|---|---|
| job channel, worker structs, the codec's api view (`ci_cop`), small shared counters | `SHAREDBSS` (uncached SDRAM alias): 424 B for the codec client, none over 256 B | both cores, low rate |
| a COP worker's stack | the client's own memory, cached (the codec: the top 9 KB of the codec slot) | only the COP touches it; the kernel writes back the fill pattern before the thread starts and the COP's cache when it exits |
| codec IRAM (the plugin/codec 80 KB) | unchanged; IRAM is uncached and shared | the MP3 synth already shares it |
| core IRAM (48 KB, 8.5 KB free) | **no new use** (rule 3: IRAM is for codecs) | |
| the ring and pcmbuf | unchanged (CPU-owned, cached); the COP reads the ring through its uncached alias, and the CPU reads COP PCM the same way | 10–100 KB/s; uncached reads cost far less than a cache wipe per frame [estimate] |

- **Discovered on the way:** `devstats` (Fit-1 M0) was cached, yet both cores write it. A 16-byte line written back from one core undid the other's counts, and an idle COP may never write its lines back, so **pre-Fit-2 `cop_idle_pct` readings may be stale**. Fixed: `devstats` and `devstats_last_us` are now `SHAREDBSS`.

### 3.4 Scheduling on each core

- The kernel schedules each core separately, with Fit-1's bands and time-bounded aging on both.
- **COP thread set:**

| Thread | Priority | When |
|---|---|---|
| `codec cop` | 13 `PRIORITY_CODEC` | from the first run of a codec load to its unload |
| the MP3 synth (`mp3dec`, upstream) | 16 | MP3 only; on the COP as today |
| future `art cop` | 20 (background) | while a cover is decoded |
| future `scan cop` | 20 (background) | during precompute passes |
| COP idle | — | `core_sleep()`, woken each tick by the CPU |

- A worker belongs to one client thread, the **poster**, which serves the worker's calls only while it waits in `cop_job_wait()`. The codec thread waits there for the whole decode, which matches the CPU codec thread's old role: it runs only when the decode needs something.
- **The cooperative scheduler is the main latency risk.** The CPU serves a call at its next thread switch, so a UI thread that runs 20 ms between yields delays each call by up to 20 ms. Upstream's codec had the same exposure once per frame; the call path has it once per call (section 5.4).

### 3.5 Failure and fallback

| Failure | What happens |
|---|---|
| no COP running (an old bootloader leaves it parked: `cop_main`'s note) | `cop_worker_start()` gives up after 1 s and the codec runs on the CPU through the same api struct |
| switch off | `codec_cop_select()` hands the codec the real api: upstream's exact path |
| the simulator | the switch changes nothing (`codec_cop_available()` is false); `cop_job` runs each job inline on the poster's thread with no cache operation, so client code compiles and runs unchanged |
| a COP-side panic | formatted on the COP, raised by `panicf()` on the codec thread |
| a stuck COP | not detected; the device test watches for it (section 10). A watchdog (a call that takes over 2 s ends the run as `CODEC_ERROR`) is follow-up work |

## 4. The infrastructure (built): `firmware/export/cop_job.h`

```c
bool cop_worker_start(struct cop_worker *w, void *stack, size_t size,
                      const char *name, int priority);
void cop_worker_stop(struct cop_worker *w);
int  cop_job_post(struct cop_worker *w, cop_job_fn fn, void *arg,
                  unsigned flags);          /* COPJ_IN | COPJ_OUT | COPJ_CODE */
void cop_job_wait(struct cop_worker *w, int ticket); /* serves calls */
intptr_t cop_job_call(struct cop_worker *w, cop_call_fn fn, intptr_t *io,
                      unsigned flags);      /* COPJ_CALL_WROTE */
bool cop_job_in_worker(const struct cop_worker *w);
void *cop_uncached(const void *p);
```

- **Rules** (header comment): the worker struct is `COP_SHARED_ATTR`; jobs touch no peripheral; data crosses only at the four points; a handler touches worker memory only through `cop_uncached()`; one poster per worker.
- `cop_job_core.c` holds the slot ring (claim → commit → take → finish → collect/release, FIFO, one writer per index) and the cache table, with no Rockbox calls. `tests/copjob/` checks order, wrap, bad input, every flag at every phase, "nothing on one core" and "never a bare discard". `scripts/test.sh` runs it.
- `cop_job.c`: semaphores (core-safe through the corelock), the worker thread, and a start that checks the COP really runs.
- **Unknown cache behaviours** [hypothesis] (risks R1–R3, section 11):
  - whether `CACHE_OPERATION`'s flush, written by the COP, writes back only the COP's lines;
  - the cost of a whole-cache write-back and of a status rewrite (`commit_discard`) per call;
  - whether the single 8 KB data array is shared between the cores (`cores-and-threads.md` §3: one data window, two status windows).

## 5. The first client (built): the codec's decode on the COP

### 5.1 Design (`apps/codec_cop.c`, hooks in `codec_thread.c` and `codecs.c`)

- At `Q_CODEC_LOAD` with the switch on, the codec gets `ci_cop`, a `SHAREDBSS` copy of the api. Its kernel, cache, string and memory entries are the real ones (core-safe or pure). Its 13 playback callbacks, `panicf` and `logf` are **trampolines**:
  - on the worker they become `cop_job_call()`s to the codec thread;
  - anywhere else (the codec's load and unload entry points, which stay on the CPU) they call straight through.
- At `Q_CODEC_RUN`, `codec_cop_run()` starts the worker once per load (stack: the top 9 KB of the codec slot, which `codec_get_buffer` and the loaders keep back), then posts `codec_run_proc()` with `COPJ_CODE|COPJ_IN|COPJ_OUT` and waits, serving calls. The run's status goes back as before.
- `get_command` is answered on the COP while `codec_queue` (already `SHAREDBSS`) is empty: zero calls per frame in steady play.

### 5.2 What crosses, and how

| Data | Direction | Mechanism |
|---|---|---|
| codec image and statics (written by the loader on the CPU) | CPU→COP, per run | `COPJ_CODE`: CPU write-back, COP write-back + drop (I+D) |
| codec statics after the run (read by its unload on the CPU) | COP→CPU, per run | `COPJ_OUT` |
| bitstream (`request_buffer`) | ring → COP | the CPU writes back (guard copy), returns the **uncached alias** |
| bitstream (`read_filebuf`) | ring → codec buffer | the CPU copies into the uncached alias; COP write-back + drop on return |
| PCM (`pcmbuf_insert`) | COP → DSP | COP write-back at the call; the CPU's DSP reads the uncached alias (IRAM buffers such as MP3's are uncached already) |
| `ci` fields (curpos, filesize, id3, handle, dsp) | CPU → COP | the CPU copies them into `ci_cop` after every call; id3 as its uncached alias |
| command, sizes, return values | both ways | `io[4]`, uncached |

### 5.3 Known gaps (documented, accepted for a developer prototype)

- **Writes into id3.** Two codecs write id3 fields (`shorten.c` length and bitrate, `vgm.c` length). On the COP they land in memory, and the UI may show the old value until the CPU drops that line. Cosmetic.
- **MP3 runs both halves on the COP.** Its synth thread is created on the COP as before, so MP3 is all-COP when the switch is on: the CPU is freed, but the split across cores is lost. Alternative: the trampoline puts the codec's own threads on the CPU (one line), which keeps a split with the roles swapped. The A/B decides.
- **Calls per frame:** about 2–3 (`request_buffer`/`advance_buffer`, `pcmbuf_insert`, `set_elapsed`/`set_offset`). Each costs one COP write-back, two semaphore hand-offs and the CPU's switch latency. Planned next steps if M0 shows waits:
  1. make `set_elapsed`/`set_offset` fire-and-forget;
  2. a ring lease (the COP advances inside a 32–64 KB window the CPU granted, and reports in bulk);
  3. an uncached pre-DSP PCM staging ring, so `pcmbuf_insert` doesn't wait for the DSP.
- **Encoders** (recording) are never selected; the iPod Video has none.

### 5.4 Why this beats moving the thread (and its cost)

- **No pcmbuf, buffering or DSP code runs on the COP**, so none of 2.1's blockers apply.
- **The cost is per-call latency under a busy UI.** The device test's stress case (section 10) is designed to expose it.

## 6. Candidates, ranked

CPU share is the share of one core at 30 MHz during **dark** playback unless noted. Codec costs come from upstream's published PP5022 `test_codec` figures as we remember them [estimate], and M0 replaces them. Decode-only MHz at 44.1 kHz stereo:

| Format | Decode-only MHz (44.1 kHz stereo) |
|---|---|
| MP3 128k (synth on the COP) | 6–9 |
| MP3 (all on the CPU) | 12–16 |
| FLAC | 8–12 |
| ALAC, WavPack | 10–15 |
| Vorbis q5 | 18–25 |
| AAC-LC 128k | 22–30 |
| WMA | 18–25 |
| Opus | 35–45 |

DSP without EQ is 2–4 MHz, resampling 48→44.1 adds about 4, and time-stretch (audiobook speed) adds 10+.

| # | Candidate | CPU share today | Data sharing | Coherency needs | Expected gain |
|---|---|---|---|---|---|
| 1 | **Music decode** (built) | 25–100 % of a 30 MHz core (AAC, Vorbis, WMA, Opus need boosts or sit at 30; MP3 and FLAC fit 24) | bitstream in, PCM out, ~2–3 calls per frame | per run: code + statics; per call: COP write-back; uncached bitstream and PCM | the CPU is near idle in dark playback. AAC, Vorbis and WMA stop boosting (COP ≤ 30, CPU ≤ 6); lit browsing during FLAC or AAC stays at 30 instead of 80 (section 7) |
| 2 | **Shell precompute passes** (library build, sort, grouping) | bursts of seconds to minutes at 80 MHz at sync time; zero during play | big RAM tables in, tables out; no I/O inside the pass | `COPJ_IN|COPJ_OUT` once per pass | the UI stays responsive during a rebuild; the pass at 30 instead of 80, or faster wall-clock |
| 3 | **Album art decode and scale** (on demand, Cover Flow misses) | 100–300 ms per cover at 80 MHz [estimate]; the pre-scaled cache removes most | the CPU reads the JPEG (bulk I/O), the COP decodes into a CPU-owned tile | `IN` (file bytes), `OUT` (tile) once per cover | no frame drops while a cover decodes; browsing unboosted |
| 4 | **DSP and time-stretch** (audiobook speed) | 2–4 MHz; 10–15 with time-stretch or EQ | settings from the UI; pcmbuf | needs DSP settings as messages and pcmbuf hand-off rules first (2.1, 2–3) | 1.5× audiobooks stay at 30 MHz: decode + stretch split, or both on the COP |
| 5 | **Database scan: tag parsing** | minutes at 50k tracks, background priority, I/O-bound on flash | the CPU reads tag blocks in bulk, the COP parses the buffers | `IN`/`OUT` per batch | moderate: parsing overlaps I/O; the UI is less affected |
| 6 | **The video's AAC or MP3 (interim and fallback only)** | 22–30 MHz on top of a 30 MHz video feed | as #1 | as #1 | until the BCM's AAC path works, and for files it can't take: the feed keeps the CPU while the COP decodes. Same code as #1, no extra work |

## 7. Clock policy across two cores

- **Model** [hypothesis]. DCDC1 holds the core voltage fixed at 1.2 V, so energy per cycle is roughly constant (`hardware.md` §clock). What a lower clock saves is the frequency-proportional base: the PLL (off at 24 MHz), the clock tree, and the bus and SDRAM controller while the cores sleep. So **work at 30 beats the same work at 80 when it fits**, and two cores make more work fit:
  - one core needs `f ≥ (Wc + Wd + Wu)/(1 − h)`;
  - two cores need `f ≥ max(Wc + ε, Wd + Wu)/(1 − h)`, where `ε` is the call overhead, about 0.5–1 MHz [estimate: 100–150 calls/s × a few thousand cycles, plus uncached bitstream and PCM reads], and `h` ≈ 0.1 headroom.
- **When it holds:** `W > 27` (one core can't stay at 30), while both shares are ≤ 27 MHz and the 80→30 base saving exceeds the COP's active power. Worked cases [estimate]:

| Case | One core | Two cores | Verdict |
|---|---|---|---|
| dark MP3 128k | 8 + 3 = 11 → 24 MHz | COP 8+1, CPU 3 → 24 MHz | no gain (already split) |
| dark FLAC | 10 + 3 = 13 → 24 MHz | COP 11, CPU 3 → 24 MHz | no gain; maybe a lower duty |
| dark AAC 128k | 26 + 3 = 29 → 30 MHz with boosts | COP 27, CPU 3 → 30 MHz steady | fewer boosts |
| lit browsing + FLAC | 10 + 3 + UI 20 = 33 → boosts to 80 | COP 11, CPU 23 → 30 MHz | **the main win** |
| lit browsing + AAC | 26 + 3 + 20 = 49 → 80 | COP 27, CPU 23 → 30 MHz | **win** |
| Opus | 40 + 3 → 80 | COP 41 → 80 | no clock gain; the CPU is freed |

- **Policy change** (a follow-up commit in `apps/ipodui/clockpol*.c`, Fit-1 C6's owner file; not done here):
  - `cp_track` gains `on_cop` (the codec gets a whole core);
  - the "light" table is judged against the COP's free share instead of the CPU's;
  - pcmbuf's boost watermark is unchanged (it rescues either core, since one clock serves both);
  - `clockpol` gets the per-core idle shares (devstats) as a second verdict input: a minute with COP idle under 10 % means "this format needs 30 MHz", whatever the CPU did.
- **Does an idle COP cost power today?**
  - Rockbox's idle COP sleeps in `PROC_SLEEP` and is woken by the CPU **every tick (100 Hz)** from the Timer 1 handler (`kernel-pp.c`), runs its scheduler and sleeps again: about 100 × a few hundred cycles per second, < 0.1 % of a core [rockbox; estimate].
  - What a sleeping ARM7TDMI core draws on the PP5022 is not documented [hypothesis: clock-gated, near zero dynamic power].
  - MP3 playback already wakes the COP for the synth thread every frame.
  - So the case for offload rests on clock choice and on the CPU sleeping more, not on reclaiming an idle core's power. M0's `cop_idle_pct` (now coherent, 3.3) shows the COP's real duty.

## 8. Device-reality checklist

1. **Compiled features:**
   - `HAVE_COP_JOBS` and `HAVE_COP_CODEC` are in developer builds only (`ipodvideo-variant.h`, with `HAVE_IPOD_UI_DEVMODE`, not the bootloader).
   - Stock, `fw-rel` and bootloaders compile none of it: `codecs.c` and `codec_thread.c` reduce to upstream's expressions.
   - The cross-core path compiles only for the target (`NUM_CORES > 1`, not the simulator); the simulator runs the inline path.
   - The device path is untested until section 10.
2. **Memory:**
   - No `core_alloc`, no new permanent block: the worker's stack is the top 9 KB of the codec slot, kept back only while the switch is on for that load (codec malloc gets 9 KB less).
   - New statics: 424 B `SHAREDBSS` (worker 160, `ci_cop` 164, `msg` 96, status 4), none over 256 B; `devstats` moved (516 B, unchanged size).
3. **CPU:** with the switch on, the decode leaves the CPU. The CPU keeps the DSP, the handlers and the call latency. Scale isn't affected.
4. **Storage:** no change. Every read stays on the CPU's buffering path.
5. **LCD:** none; the COP never touches the port.
6. **Threads and locks:**
   - New: `codec cop` (COP, priority 13), alive from the first run of a load to its unload.
   - The codec thread blocks in `cop_job_wait()`, on a semaphore in `SHAREDBSS`.
   - No mutex is taken on the COP. Semaphores and the codec queue are core-safe (corelock).
   - The audio thread's `codec_queue_send()` waits for the codec's ack exactly as before: the COP notices the message at its next `get_command` and calls the codec thread to handle it.
7. **Splashes:** none. A COP-side `panicf` goes through the codec thread's `panicf` (the shell's existing panic handling).
8. **Persistence:** none. The switch isn't saved (off at every boot), like "24 MHz idle".
9. **Scale:** not library-dependent.
10. **Deferred work:** the switch is read at each codec load. A toggle mid-track applies at the next load; the row says "next" until then.
11. **Stock firmware unchanged:** `fw` and `fw-rel` builds are checked in section 9.
12. **Upstream paths doing the same job:** MP3's `MPA_SYNTH_ON_COP` (kept; see 5.3), mpegplayer and FFT (plugins, trimmed in our builds), `switch_thread_core()` (not used: the thread doesn't move).

## 9. Files and measurements

| File | Change | Guard |
|---|---|---|
| `firmware/export/cop_job.h`, `firmware/common/cop_job.c` | the job channel | `HAVE_COP_JOBS` |
| `firmware/export/cop_job_core.h`, `firmware/common/cop_job_core.c`, `tests/copjob/` | the pure ring and cache table, host test | `HAVE_COP_JOBS` |
| `firmware/common/devstats.c` | `SHAREDBSS` | `HAVE_DEVSTATS` |
| `apps/codec_cop.{c,h}` | the codec client | `HAVE_COP_CODEC` |
| `apps/codec_thread.c` (+24 lines), `apps/codecs.c` (+17) | three hooks; the slot reserve | `HAVE_COP_CODEC` |
| `apps/ipodui/devmode.c` | "Codec on 2nd core" row, with runs, calls and wait ms | `HAVE_COP_CODEC` |
| `firmware/export/config/ipodvideo-variant.h`, `firmware/SOURCES`, `apps/SOURCES` | flags and sources | |

**Measured** (`arm-none-eabi-size`; `fw-ui` before = `morning`):
- `fw-ui` text/data/bss: 735,096 / 8,384 / 359,864 → 740,544 / 8,392 / 360,312. That is **+5,448 B `.text`**: `codec_cop.o` 3,317, `cop_job.o` 1,336, `cop_job_core.o` 410, plus the hooks and the row.
- Stock `fw` and `fw-rel`: `rockbox.bin` is byte-identical with `codecs.c` and `codec_thread.c` at `morning` versus this branch (same tree, `shasum`). The other new files aren't compiled there.
- All nine configs `validate.sh` builds (fw, fw-ui, fw-rel, sim, sim-ui, sim-ui-video, boot, boot-ui; fw-ui-video and fw-alt compile-only) build. The only warnings are those already on `morning` (`core_alloc.c`, `pcm.c`, `tagcache.c`, the linker's `_clzdi2` note), none in Fit-2 files.
- `scripts/test.sh` passes, including `tests/copjob`.

## 10. Device test plan (owner; the developer build of this branch)

**Setup:**
- Power log at 5 s (Developer › Power log every).
- Battery full or the same start level for both arms.
- Backlight off except in the stress case.
- Same playlist in both arms: 10 min MP3 320k, then 10 min FLAC 44.1/16, then 10 min AAC-LC 256k (30 min). Start each arm with a stop, so the codec reloads.

**What M0 shows** (`power.csv`, `hardware.md` columns):
- `idle_pct`, `cop_idle_pct`: per-core load (COP fixed coherent here).
- `cpu1`/`cpu2`: top threads; expect `codec cop` to replace `codec` in arm B.
- `boost_pct`: time at 80 MHz.
- The 24 MHz share: clockpol's verdict lines in `ipodui.log`.
- `mv` drop: battery.
- Underruns: `pcm underrun` lines.

**Arms (each 30 min, then swap the order on a second day):**
- **A:** "Codec on 2nd core: off" (upstream path).
- **B:** "Codec on 2nd core: on". Note the row's `r/c/w` after each format (runs, calls, ms the COP waited).

**Pass:**
1. Zero `pcm underrun` in B.
2. Per format: B's CPU idle ≥ A's + the codec's share; COP idle drops by about the same.
3. B's `boost_pct` ≤ A's.
4. Calls ≤ 4 per frame; wait ≤ 10 % of the run's time (from `c` and `w`).
5. `mv` drop over 30 min in B ≤ A's (within the log's noise: repeat if the two are within 5 mV).
6. Seeks (10 per format), skips and pause/resume work; no glitch at track changes.

**Stress (10 min, B then A):**
- Backlight on, continuous wheel scrolling through Songs (60 fps list), with FLAC then AAC playing.
- Start Settings › Reset/Update Library (the database rebuild) after 2 min.
- **Pass:** no underrun; the UI frame pace no worse than A; record `r/c/w` and the per-core idle.
- The per-call latency risk (3.4) shows here first: if `w` grows past 10 % or underruns appear, step 1–3 of 5.3 are next.

**Per-core time at each clock:** from the same rows, tabulate per arm the share of rows at 24 MHz (clockpol's window lines), at 30 MHz (unboosted, not 24) and at 80 MHz (`boost_pct`), next to `idle_pct` and `cop_idle_pct`. That is the even-load objective's evidence: B should move time from 80 to 30, and from 30 to 24 for formats that now fit.

**If the COP never starts** (the row reads "on" but `r` stays 0 and `cop_idle_pct` ≈ 100): the worker start timed out. Report it; the codec then runs on the CPU through the same path (A's behaviour).

## 11. Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | the COP's `commit_dcache` writes back the CPU's lines too, or not all of the COP's (shared controller, `cores-and-threads.md` §3) | silent PCM or bitstream corruption | the A/B listens for glitches; Rockbox's `switch_thread_core()` and the MP3 synth already rely on per-core flushes |
| R2 | whole-cache write-back plus drop per call is expensive at 100+ calls/s | COP time, fewer gains | `w` and `cop_idle_pct` show it; the 5.3 steps reduce calls |
| R3 | one shared data array: the COP's working set evicts the CPU's lines | CPU slower under load | the stress case measures UI pace |
| R4 | per-call latency under a busy cooperative UI | the decode runs slower than real time during long UI bursts | the 3 s pcmbuf hides bursts; the 5.3 steps; the stress case |
| R5 | shared bus contention (both cores on SDRAM and IRAM) | each core slower than alone | the per-core idle shows the total; MP3's split shows it was a net win upstream |
| R6 | id3 writes by two codecs; codecs that pass pointers through `configure` (none today) | cosmetic; future breakage | noted in `codec_cop.c` |
| R7 | a COP hang (no watchdog yet) | playback stops until a reboot | developer switch only; a 2 s call watchdog is follow-up |

## 12. Device session 36 (2026-10-14/15): first numbers with the COP decode

About 25 h over six boots (`session log, session 36`); the owner played FLAC albums for over an hour with "Codec on 2nd core: on", no gaps heard, no `pcm underrun` line. Coarse figures [ours, one unit, not yet the A/B of section 10]:

| | COP decode on | COP decode off |
|---|---|---|
| CPU core idle (`idle_pct`), FLAC dark | ~88–92 % | ~45 % |
| COP idle (`cop_idle_pct`) | ~41–46 % (`codec cop` 55–68 % of it) | ~99.5 % |
| battery, FLAC dark | ~4.5–5 %/h | ~8 %/h (one short stretch) |

- The battery column is in 1 % steps over short stretches, so it is a hint, not a result: the A/B of section 10 (same voltage band, 30 min arms, repeated) is still owed.
- **Tap to first PCM** (`m0 tap2pcm`): 128–182 ms on a skip, 363–384 ms on a resume at boot; `pcm start prefill_ms` 42–74 ms (C3's 0.25 s prefill).
- **PLL lock** (C6, `clk lock`): 16–27 µs mean, 237 µs worst, over ~56–69k power-ups.
- **The FLAC strike was not the codec.** `clk strike fmt=7 boosts=56362` (and 59763 on another boot) fell in the first two minutes, while the boot's library scan ran beside playback (`tagcache` 91–93 % of the CPU, ~59k ATA commands in 120 s). One boost from 24 MHz per command is what a forced-UDMA4 transfer does (`ata-pp5020.c` boosts for UDMA > 2), and boosts from 24 MHz were the verdict's measure. After the strike FLAC sat at 30 MHz with `boost_pct` 0. The verdict now counts the codec's PCM rescues (the playing PCM falling under pcmbuf's boost watermark) instead, and the UDMA4 boost starts from the PLL hold (30 MHz). With the COP decoding, the CPU's per-core load suggests FLAC fits at 24 MHz without boosts; the next session should show no strike for FLAC. [hypothesis: UDMA4 forced; confirm `/.rockbox/udma4.cfg` and the `udma boot force=` line at the top of the boot's log]
- **Library scan at boot, for a later item:** `db scan` took 97–126 s for n = 718 at every boot, on an unchanged library. That is 2–5× the sim calibration (×200–400 of 50 ms for 322 files in the sim). The rows show the tagcache thread CPU-bound (idle 4–5 %) and command-bound: ~83 ATA commands and ~53 KB per file. Candidates, unattributed: the per-file "already in the database?" lookup (`find_entry_disk`: with no dircache, `find_entry_ram` always misses, so every file costs a walk of the filename tag file from the last hit), `probe_file_format` and the directory walk. Attribute it per file in the sim (failed opens now count as commands) and on the device (`m0` per-thread commands during a boot scan) before changing it. At 50k tracks the same per-file cost would be hours, so it matters before #43.

### 12.0 Decision (owner, 2026-10-18): on by default
On session 36's numbers (no gaps or underruns over an hour of FLAC, the
CPU's idle doubling, a battery hint in its favour), the COP decode is the
default in every shell build: `HAVE_COP_CODEC`/`HAVE_COP_JOBS` now follow
`HAVE_IPOD_UI` instead of `HAVE_IPOD_UI_DEVMODE`, and `codec_cop.c` starts
enabled. The developer row turns it off until the next boot. The proper
battery A/B of section 10 is still owed; if it shows the COP costs battery
on some format, the default can be revisited then. Videos keep their sound
on the CPU (12.1).

### 12.1 Video soundtrack with the switch on (open)
The bcm session (dev45 work, 2026-10-15) saw stage 7's start, resume and
re-enter steps with the switch on, and each time the video's soundtrack
stood still (audio clock frozen at 29,883 ms), so the video froze with it.
Until the cause is found, `codec_cop_select()` keeps a codec load on the
CPU while `vplayer_running()`: a video always decodes its sound as it
would with the switch off. Suspects for the device session, with the
switch on and a video's audio forced to the COP: the m4a codec's resume
path (seek before the first frame), and the player thread at
PRIORITY_VIDEO_FEED holding the codec thread that serves the COP's
callbacks. The end state is unchanged: the video's sound goes to the
VideoCore (stage 8b/9).
