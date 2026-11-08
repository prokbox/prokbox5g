# What both cores touch during a COP decode run (#197)

*Audit, 2026-11-07, branch `p19-cop-default` (from `d5036ff28d`). 0.8.0 decodes on the COP by default (`docs/changes-from-rockbox.md` 2.67). #196 found one object written cached by one core and uncached by the other (the codec's id3, 2.68; quirk 90). This file lists every object both cores touch while a decode runs on the COP, with a verdict for each. Tags as in `docs/pp5022-reference/index.md`: [code] read in this tree, [build] from the map or symbol table, [model] a host model, [hypothesis].*

## Result

- **Release build (`fw-ship`): no hazard left.** 26 rows are **safe**: uncached on both cores, one core's alone, or a hand-off fenced by `copj_cache_ops()`. One row (the codec's id3) was a hazard and is **fixed** by #196. The fences behind the slot hand-off are now held by a host model (`tests/codecid3/test_handoff.c`).
- **Four rows are unknown.**
  - **U1:** a COP crash.
  - **U2:** in-place writes to the ring by codec loaders not read to the end.
  - **U3:** developer builds only: the UI on the COP if a stale marker selects it.
  - **U4:** developer builds only: the PCM interrupt on the COP if a bench row left it there.

  None needs a bench row of its own. Each is settled by reading code, or by a log line every sitting already writes (below).
- **The morning heap corruption is still unexplained by this audit.** No row lets the COP write cached memory outside the codec slot. U3 and U4 are the only cross-core paths that could reach buflib's headers, and both exist only in developer builds, which the bench runs. Read the sitting's log for them first (U3, U4).

## How it was enumerated

1. **What runs on the COP during a run** [code]: `cop_job.c` `worker_thread()`/`run_slot()`, `codec_cop.c` `run_job()` → `codecs.c` `codec_run_proc()` → the codec image. The codec reaches the firmware only through the API it was handed, `ci_cop`:
   - the `t_*` callbacks: on the COP they marshal into the worker and run the `h_*` handler on the CPU's codec thread;
   - the entries copied as they are from `ci`: `sleep`, `yield`, `create_thread`, `thread_thaw`, `thread_wait`, the semaphores, the cache operations, the string and memory functions, `qsort`;
   - the COP's own scheduler and idle loop.
2. **The call-graph walk, `scripts/cop_shared_check.py BUILD_DIR`** [build]. It uses `febe_reach_check.py`'s walker on `rockbox.elf` and `rockbox.map`, from those roots:
   - it follows direct branches, function addresses in literal pools, and the function tables a reached function names;
   - a function handed to `cop_job_call()` runs on the CPU, so it ends a path;
   - it lists every cached, writable static that reached code may write (logical SDRAM below `0x1000_0000`; the uncached alias at `0x1000_0000` and IRAM at `0x4000_0000` are exempt).
   - **`fw-ship`:** 89 functions reached, **1** cached static written: `probed_ramsize`, by `system_init()` from `cop_main()` at boot only. **`fw-ui`** (developer): the same 89 and the same 1.
   - The crash path is reviewed apart (U1). With `UIE`/`panicf`/`panic_record_save` left in the walk, 54 statics appear, all reached through `__div0 → UIE → panic_record_save`.
   - Roots absent by name are inlined or IRAM aliases: `commit_discard_dcache`, `core_sleep`, `t_logf` (no logf in the release), `debugf`.
3. **By hand** for what the walk cannot see:
   - memory reached through a pointer that is not a static's address: the codec slot, the ring, the codec's id3, PCM output, stacks, the `io` arrays;
   - the code inside the `.codec` images, whose statics live in the codec slot;
   - the uncached and IRAM objects the walk exempts.
   - I grepped `COP_SHARED_ATTR`, `SHAREDBSS_ATTR`, `NOCACHEBSS_ATTR`, `IBSS_ATTR` and `cop_uncached` across `apps/`, `firmware/` and `lib/rbcodec/`, and every `ci->` field and callback the codecs use.
   - For in-place writes: every `request_buffer` user in `lib/rbcodec/codecs`, and the loaders it hands the ring to.
4. **What this cannot catch:**
   - a pointer to cached memory stored in an uncached object and dereferenced on the COP: none found by reading;
   - code reached through a function pointer loaded from a non-static place;
   - writes inside the codec images other than through the API (covered by the slot's fences);
   - hardware: whether each core has its own data array (quirk 87, open), and DMA.

Placement [build]: addresses from `fw-ship` at `d5036ff28d`. `.ncbss`/`.ncdata` sit at `0x10xx_xxxx` (the uncached alias), IRAM at `0x4000_xxxx`, the codec slot at `0x03F0_0000` (`codecbuf`, cached).

## The table

| object | file:line | written by CPU (cached/uncached, when) | written by COP (cached/uncached, when) | read by each core how | line sharing with other objects | verdict |
|---|---|---|---|---|---|---|
| `ci_cop`, the API the codec sees | `apps/codec_cop.c:92` (`.ncbss` 0x1016368c) | uncached: whole copy at `codec_cop_select()` (codec stopped); `sync_fields()` after every callback, while the COP waits | never (codecs write no API field; the `libtremor` `ci->` writes are its own codec-setup struct) | COP uncached; CPU uncached | n/a (uncached on both) | **safe**: one writer at a time, uncached both |
| `worker` (ring, `call_*`, semaphores, counters) | `apps/codec_cop.c:91` (`.ncbss` 0x10163734) | uncached: post, collect, `serve_call()` | uncached: take, finish, `cop_job_call()` (`call_state` last) | both uncached | n/a | **safe**: protocol host-tested (`tests/copjob`); uncached stores leave in order (quirk 81) |
| `run_status`, `msg` | `apps/codec_cop.c:93-94` (`.ncbss`) | never (reads after the job's semaphore) | uncached: the job's result; panicf/logf text | CPU uncached | n/a | **safe** |
| `starting`, `start_mtx`, `start_mtx_ready` | `firmware/common/cop_job.c:98-100` (SHAREDBSS) | uncached: worker start | uncached: the new thread reads `starting` | uncached | n/a | **safe** |
| `cop_down` and the other `codec_cop.c` statics (`real`, `bound`, `on_cop`, `place_mode`, `place`, `runs`) | `apps/codec_cop.c:96-110`, `.bss` (cached) | cached: codec thread only | never (the `t_*` COP paths read only `worker`) | CPU cached | shares lines with other CPU statics; the COP writes no cached static (walk) | **safe**: one core's alone |
| `codec_id3` pointer and CODEC_ID3 itself | `apps/playback.c:221`, slot in the audio scratch area | uncached: track start, `set_elapsed`/`set_offset`, a seek | uncached: shorten/vgm/asap/mpa/atrac3_rm fields | both uncached | lines of its own (`codec_id3_place()`) | **fixed** (#196, 2.68; `tests/codecid3`) |
| codec slot: image, codec `.data`/`.bss`, `codec_get_buffer()` area | `codecbuf` 0x03F00000, `apps/codecs.c` | cached: load (`lc_open`, `*(c_hdr->api) = api`), a run on the CPU, the unload's entry point; uncached: `read_filebuf()` into the codec's buffer during a COP run | cached: the whole run | each cached, after the fences | the slot is cached on both cores; consistent only through the fences | **safe**: hand-off at `COPJ_POST` (CPU commit), `COPJ_START` (COP commit, drop data and code), `COPJ_FINISH` (COP commit), `COPJ_COLLECT` (CPU commit, drop), `COPJ_CALL` (COP commit), `COPJ_RETURN` + `COPJ_CALL_WROTE` (COP commit, drop). [model] `tests/codecid3/test_handoff.c` (below) |
| the COP worker's stack | top 12,288 B of the slot (`codec_slot_top()`) | never while bound (kept back: `codec_cop_reserve()`); after unbinding, a later codec may use the bytes | cached: the worker, also after `COPJ_FINISH` (its loop) | COP only | inside the slot | **safe**: kept back while bound; the thread's exit writes the COP's cache back before `thread_wait()` returns (`thread-pp.c`), so no dirty line survives into the next load |
| `io[]` call arrays | the COP's stack | never | cached, then copied into `worker.call_io` (uncached) | COP | n/a | **safe** |
| `c_hdr`, `curr_handle`, `codec_size` | `apps/codecs.c` `.bss` 0x001209e0 | cached: load and close only (codec stopped) | never | COP cached in `codec_run_proc()` | one line with each other, CPU-written | **safe**: the COP reads after `COPJ_START`'s drop; never written during a run |
| `ci` (the CPU's API, `codecs.c`) | `apps/codecs.c` `.data` 0x001141bc | cached: playback fields, `cop_assist` | never | CPU only (the COP has `ci_cop`) | CPU-only lines | **safe** |
| the ring (buffering's buffer, the audio buffer) | `apps/buffering.c` | cached: fills, handle headers in the ring (`struct memory_handle`, `ridx`/`widx`) | uncached reads through the alias `h_request_buffer()` returns; **writes** only by codecs that change their file in place: `mod.c:509-540` (pattern conversion and its 0xfe marker, interior bytes only) | COP uncached; CPU cached | data starts 4-aligned after its handle header: the first and last line of a handle's data can share a line with a header the CPU writes | **safe** for reads (`commit_dcache()` before the hand-out; buffering never writes a live handle's bytes; the CPU drops its whole cache at `COPJ_COLLECT` before any CPU run reads the ring again); in-place writes: **unknown** U2 |
| PCM out of the codec (`pcmbuf_insert` input) | codec slot or IRAM | never | cached; the `COPJ_CALL` commit before each insert | CPU uncached (`cop_uncached()` in `h_pcmbuf_insert()`) | in the slot | **safe** |
| `pcmbuf`, the mixer, DSP (EQ, resampler, timestretch, replaygain) | `apps/pcmbuf.c`, `lib/rbcodec/dsp/*` (DSP state in IRAM) | the codec thread, inside `h_pcmbuf_insert()`/`h_configure()` | never: `pcmbuf_insert` and `configure` are callbacks to the CPU; `codec_set_replaygain()` ends in `ci->configure` | CPU | n/a | **safe**: CPU only |
| `codec_queue`, `codec_queue_sender_list` | `apps/codec_thread.c:105-106` (SHAREDBSS) | uncached: posts and sends | never; `t_get_command()` reads `queue_empty()` | both uncached | n/a | **safe**: a racing read only defers a command by one frame |
| buffering's handles and read positions | `apps/buffering.c` | cached: the buffering and codec threads | never: `advance_buffer`/`seek_buffer`/`read_filebuf`/`request_buffer` are callbacks to the CPU | CPU | n/a | **safe**: CPU only |
| MP3's `cop_assist` | `codecs.h` field in `ci`/`ci_cop` | `choose()` before each run | never | codec reads at run start (`ci_cop` uncached) | n/a | **safe** |
| MP3's synthesis hand-off (`stream`, `frame`, `synth`, `sbsample_prev`, `die`, the two semaphores) | `lib/rbcodec/codecs/mpa.c:44-58` (IBSS) | IRAM, uncached: a run on the CPU (synthesis inline) | IRAM: the decode and the synthesis thread, both on the COP with assist | IRAM | n/a (IRAM is not cached, quirk 11) | **safe** |
| the synthesis thread's stack, SPC's emulator thread | mpa/spc `.bss` in the slot | never | cached: COP threads made at load | COP | in the slot | **safe**: they end at unload and the exit writes back (as the worker) |
| kernel: `__thread_entries`, `__cores`, `core_semaphores`, `cop_next_tmo_check` | `firmware/kernel/thread-common.c:46-51`, `thread-pp.c:160` (IRAM) | IRAM | IRAM | IRAM | n/a | **safe** |
| `all_queues`, `current_tick`, `threadalloc` | `queue.c:32`, `tick.c:38`, `thread-common.c:68` (SHAREDBSS) | uncached | uncached | uncached | n/a | **safe** (Rockbox's dual-core design) |
| `ui_log`/`logf` rings | `apps/ipodui/uilog.c` | cached: CPU only | never (`t_logf`/`t_panicf` format into `msg` and call the CPU; no logf in the release) | CPU | n/a | **safe** |
| `devstats`, `devstats_last_us` (developer builds) | `firmware/common/devstats.c:39-41` (SHAREDBSS) | uncached: its threads' slots, `thr_wakes` of a thread it wakes | uncached: its threads' slots, `thr_wakes` of a CPU thread it wakes (`semaphore_release`) | uncached | n/a | **safe**: no line write-back; two cores adding to one `thr_wakes` at once may lose one count (a statistic) |
| `pgov_idle[2]` (developer builds) | `firmware/pgov.c:60` (SHAREDBSS) | uncached | uncached, its own core's slot (`core_sleep()`) | seqlock | n/a | **safe** (quirk 81; `tests/ppgov`) |
| K-1 park (`cop_parked`, `k1_sent`, `k1_held`) | `firmware/target/arm/pp/kernel-pp.c:31-32` (`.bss`, developer builds) | cached: its tick and threads | never | CPU | CPU lines | **safe**: CPU only |
| `heapchk` walks | `apps/ipodui/heapchk.c` | CPU, after the COP's job is collected | never | CPU | n/a | **safe**: CPU only |
| `probed_ramsize` | `system-pp502x.c` `.bss` | never after boot | cached: `system_init()` from `cop_main()`, at boot, before any run | CPU | one byte | **safe**: boot only (the walk's one finding) |
| the COP's crash path | `UIE`, `panic_record_save()`, `panicf()` | n/a | cached: panic statics, the panic record; the walk says the path can reach the file layer | n/a | many | **unknown** U1 |
| the UI thread on the COP (`HAVE_UI_THREAD_SPLIT`, developer builds) | `apps/ipodui/fe_be.c` `mode_read()` | — | if a stale `ui_split` marker or the config key puts the UI on the COP: ~110–180 unrouted cached statics (2.44) | — | many | **unknown** U3 |
| the PCM interrupt on the COP (`pcm_pp_set_core`, layout A step 2, developer builds) | `firmware/target/arm/pp/pcm-pp.c:70-74` | — | the mixer's cached control words (quirk 55) | — | — | **unknown** U4 |

**Counts:** 26 safe (the ring counted safe for reads; its in-place case is U2), 1 fixed (#196), 4 unknown, 0 hazards open.

## The hand-off model (`tests/codecid3/test_handoff.c`)

Of all the rows, only the codec slot is cached on both cores, so its verdict rests on `copj_cache_ops()` (the real function, from `cop_job_core.c`). The model has two caches of 16-byte lines, with random replacement and no snooping. It runs one decode as `codec_cop.c` drives it:
- the load, over a slot whose earlier use left dirty CPU lines and stale COP lines;
- the COP's run, writing statics and its stack;
- a `read_filebuf` call;
- the finish and collect, and a run on the CPU;
- a second COP run of the same load.

With the real operations, 2,000 runs lose nothing. Dropping each phase's operations, one at a time, loses data:

| phase dropped | loss over 2,000 runs |
|---|---|
| POST | COP saw a stale load in 2,000; CPU saw stale statics in 2,000; COP missed the CPU run's writes in 2,000 |
| START | stale load in 77; missed the CPU run's writes in 1,314 |
| FINISH | CPU saw stale statics in 2,000; missed the CPU run's writes in 2,000 |
| COLLECT | stale statics in 1,429; missed the CPU run's writes in 191 |
| CALL | stale `read_filebuf` in 2,000 |
| RETURN | stale `read_filebuf` in 1,965 |

`run.sh` requires every one of these failures on each run, so a change that makes a fence unobservable fails the suite.

## The unknowns and what settles them

- **U1, a crash on the COP.** A divide by zero or an undefined instruction on the COP runs `UIE` → `panic_record_save()` there. Those write cached panic statics and, by the walk, can reach the file layer (`core_load_key_remap`, possibly a false table edge). After a crash the system is going down, so the question is only whether the CPU's panic screen and record survive a COP crash.
  - **Settle it by reading code:** `panicf()`/`UIE` on a dual-core PP, and whether `panic_record_save()` uses the file layer from the COP. Alternatively, force a COP divide by zero in the emulator (#178).
  - No bench row: it changes no decode behaviour.
- **U2, in-place writes to the ring.** Found: `mod.c` converts its pattern data and writes a marker in place, at interior offsets only (≥ 471 B into the file), never in a handle's first or last line. Read and found read-only: libgme's `Rom_load()` (nsf, gbs, hes, kss, sgc, ay, vgm). Not read to the end: `libasap` `ASAP_Load()`, `cRSID_processSIDfile()`, `AyumiRender_LoadFile()` (vtx).
  - **The failure needs two things:** a codec writing its file's first or last bytes in place, and the CPU writing the neighbouring handle header in the same 16-byte line. Then the CPU's line write-back would undo the codec's bytes. That corrupts the emulator's view of its own file, not the heap.
  - **Settle it by reading those three loaders.** If one writes its header in place, give the ring's handle data lines of their own (align data to 16 in `buffering.c`).
- **U3, the UI on the COP in a developer build.** 19-9 is retired (#148) and the default is 0 (the main thread). A `ui_split` marker or the config key left on the unit by bench 4's rows would put the UI on the COP beside the decode, with 2.44's unrouted cached statics.
  - **Settle it from the morning sitting's `ipodui.log`:** its boot line `fe be_svc up mode=<mode>`. Any mode other than the main thread's makes U3 the first suspect for the heap corruption.
  - No new row: every boot writes the line.
- **U4, the PCM interrupt on the COP in a developer build.** The `pcop` row (and `p1`, its probe) moves source 26 to the COP's FIQ, where the mixer's control words are cached (quirk 55), and puts it back.
  - **Settle it from the same log:** `pcop` says "pcop: the PCM did not move back to the CPU (playing?)" when its restore fails (`devbench_core.c`). Its absence after `pcop` means the later rows ran with the FIQ on the CPU.
  - No new row.

## What changed for this audit

- `scripts/cop_shared_check.py` (new): the walk above.
- `tests/codecid3/test_handoff.c` (new), with `tests/codecid3/run.sh`: the hand-off model, and the six dropped-fence runs required to fail.
- No firmware change. No hazard was open in the release build after #196.

## Layout A (0.8.0-rc.6): the PCM's FIQ on the COP

*Branch `p19-audio-cop`, 2026-11-07. Owner decision: 0.8.0 puts the audio output on the COP, Apple's layout for the end of the audio pipeline. The I²S DMA0 FIQ, pcm-pp's refill, the mixer and pcmbuf's consumer callback all run on the COP from boot. The CPU keeps the producers (the codec thread's DSP and `pcmbuf_insert`, the beep, the Clicker), the UI, the file system, storage, USB and the BCM driver. Bench rows p1 and pcop proved the FIQ route on silicon in sitting 4gh-b8, but they used a private buffer: neither the mixer nor pcmbuf ran on the COP until this change.*

### The audit

There are two walks from the FIQ's roots: `fiq_by_core`, `fiq_playback`, the mixer's callbacks, and every channel callback (`pcmbuf_pcm_callback`, `beep_get_more`, `voice_pcm_callback`, `al_more`).
- **The script:** `scripts/cop_shared_check.py --fiq`.
- **A by-hand read** covering everything the walk can't see: memory reached through pointers, the ring, the descriptors, the id3.

The table lists 25 rows: 15 safe or not applicable, 8 hazards (H1–H8) in the release as it stood, 1 unreachable in the release (2g), and 3 unknowns.

| | What | Failure on the COP | rc.6 fix |
|---|---|---|---|
| H1 | `max_dma_chunk_size` (pcm-pp.c), cached; the CPU sets it microseconds before the first FIQ | The COP reads the old value or 0, and `DMA0_CMD` gets `0 − 4`: every config bit set | IRAM (`IBSS_ATTR`) |
| H2 | The idle tail's rate (`max_idle_frames()` read the sink's rate through the COP's cache) | Wrong idle-tail length after a rate change | `mix_idle_max`, uncached, set by the CPU before every start and at each rate change |
| H3 | pcmbuf's audio and descriptors; crossfade's in-place mixing | Stale audio from the ring's last lap; a wrong chunk size | The CPU writes back before publishing (`commit_chunks()`, after crossfade writes, at a channel's start in `mixer_channel_play_data()`). The mix reads sources through the uncached alias (`pcm_mix_src()`). The descriptors are uncached on whole lines of their own, after one CPU commit-and-discard at `pcmbuf_init()` |
| H4 | pcmbuf's indices and hand-off words (`chunk_ridx`/`chunk_widx` on one line, `current_desc`, `chunk_transidx`, `fade_out_complete`, `pcm_underruns`, the ring geometry) | A replayed or lost chunk; a seek's reset undone; a stalled codec | `XCORE_BSS` |
| H5 | The track-change notice and the may-play check (`track_change`, `play_status`, `pause_on_track_change`, `position_key`) | Gapless changes delayed or lost; play past a pause-on-change | `XCORE_BSS`. `PLAY_STATUS_XCORE` widened to layout A. The FIQ bumps the counter through `audio_pcmbuf_track_change_fiq()`, which can't reach the queue |
| H6 | The beep's state and its callback table; the Clicker's buffer | A beep cut short or long; a silent first click | `XCORE_BSS`. The channel start writes the buffer back |
| H7 | The playing id3's position, written by the FIQ (Now Playing, resume, the dock, the video's A/V clock) | Elapsed frozen or jumping; neighbouring id3 fields overwritten | An uncached seqlock mailbox (`pcm_xcore.h`): the FIQ posts, and the CPU's tick applies it to the id3 and the dock code |
| H8 | The mixer's control words (`downmix_index`, `next_size`, `idle_counter`, `mix_frame_size`) | Repeated or skipped frames at start or resume; line fights with `sound_prescaler`, `spw_allow` and the panic text | `XCORE_BSS`/`XCORE_DATA` |

**The safe rows:**
- the driver's state, the per-core targets and the lock (IRAM);
- DMA0's source, `downmix_buf` (IRAM);
- the mixer's channel tables (IRAM);
- pcm.c's callbacks and `pcm_playing` (`SHAREDBSS`);
- the visualiser's tap (uncached);
- the codec's standby words (`WM_XBSS`);
- the start, stop, pause, USB, power-off, restart, standby and sleep paths;
- the developer hooks.

Recording, voice, software volume and bit-perfect are compiled out or absent. A video's soundtrack goes through pcmbuf like any track (H3–H5, H7). The stage 8b audio loop (`al_more`) runs only in a developer test and is unreachable in the release.

### The boot proof and the fallback

`apps/audio_cop.c`, after `audio_init()` and before anything can play:
1. `pcm_pp_set_core(COP)`.
2. 256-byte pieces of uncached silence play. At least 8 FIQs must arrive on the COP within 200 ms, with none on the CPU.
3. Otherwise the PCM goes back to the CPU for the boot, and the log says "COP FAILED (pcm …): audio output on the CPU": a loud safety net, never the plan.

### Proof that each hazard class is closed (host and build)

- **`scripts/cop_shared_check.py --fiq BUILD`**, run in the gate on `fw-ship` and `fw-ui`. It fails on any cached static the FIQ's code writes, and on any kernel or blocking call it makes: corelocks, mutexes, queues, semaphores, wake-ups, sleep or yield, the tick list, the allocator, the dock code. A FIQ that takes a corelock its interrupted thread holds deadlocks its own core.
  - Red on rc.5's code with the FIQ moved: 13 statics, 14 calls.
  - Green after the fixes: 25 functions in `fw-ship`, 24 in `fw-ui`.
  - Two functions are reviewed table lookups: `pcm_get_frequency` reads a field, and `pcm_play_stop_int` calls only `ops.stop` = `sink_dma_stop`, which is walked.
- **`tests/audiocop/`**: two caches of 16-byte lines over one memory, with the real `pcm_xcore.h`:
  - the stale ring;
  - two indices in one line;
  - the position, under every interleaving of a post and a take.

  The rc.6 design passes 300 seeds. Each piece taken away fails on some seed: no write-back, the mix reading through its cache, cached indices, the FIQ writing the id3, a reader with no sequence check. Mutating the header's sequence checks also fails it.

### Unknowns (settled by the soak, b-soak)

- **U1. FIQ latency on the COP.** `commit_discard_dcache()` masks the COP's IRQ and FIQ for a whole write-back at each decode job's fence. The FIFO allows about 363 µs. The soak's underrun count and late-FIQ watchdog measure it.
- **U2. The lock's store order.** The COP's IRAM store of `pcm_fiq_busy` must land before its interrupt-controller read. pcop's lock counters were clean; the soak keeps counting.
- **U3. The cost of the write-backs.** One whole-cache write-back per published chunk and per crossfade write, on the CPU. To be measured ABAB against an uncached DSP output.
