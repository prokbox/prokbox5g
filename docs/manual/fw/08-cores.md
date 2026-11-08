---
title: "FW-8 The CPU and the COP"
weight: 80
description: "What runs on which of the PP5022's two cores, in 0.8.0 and in the target design."
---

> **Draft.** This edition has only the part of FW-8 that every contributor needs: which core runs what, and the rules for memory both cores touch. Threads, scheduling and the kernel come in a later edition. The layout below is the one 0.8.0 ships, decided on 2026-11-07 (0.8.0-rc.6).

The PP5022 has two ARM7TDMI cores, identical in silicon, sharing one bus and one memory map. Rockbox calls them the **CPU** and the **COP** (coprocessor), and so does this reference. The names describe a convention, not a difference: either core can run any code. What differs is that the CPU starts at reset and the COP waits until the CPU wakes it. [rockbox-derived]

Getting the split between the two cores right is the largest design question in ProkBox. This chapter says where it stands, so that you put new work on the right core.

## 8.1 Why the split is hard

The two cores have separate caches, and the PP5022 does nothing to keep them coherent. [silicon] If both cores use the same data through their caches, each can see a stale copy, and the result is wrong without any error. Every byte that crosses between the cores therefore has to be written back from one cache and dropped from the other at exactly the right moment, or kept in memory that neither caches.

That has a direct consequence for design: the fewer things that cross between the cores, the fewer places that can go wrong. A layout where the cores exchange a few messages a second is far easier to keep correct than one where hundreds of functions are called across. The project learnt this the hard way. An earlier plan, known as 19-9, put the user interface on the COP; it needed 238 cross-core call stubs, and two faults on real hardware came from data shared between the cores by accident. [silicon] That plan is retired.

`docs/data-paths.md` records which part of the player can move bytes where, and what has been proven. Read it before touching anything that crosses the cores.

## 8.2 The layout in 0.8.0

In release 0.8.0 **both ends of the audio pipeline run on the COP**: the decoder, and the audio output. The CPU runs everything else, including the audio processing between them.

| Core | Runs in 0.8.0 |
|---|---|
| **CPU** | The user interface, the file system, the disk, USB, the BCM2722 driver and playback control. In the audio pipeline, the producers: the codec thread's DSP (equaliser, resampler, speed, replaygain) and `pcmbuf_insert`, the beep and the Clicker. Every start, stop, pause and seek of the output |
| **COP** | The codec's decoding, as a job per run. The audio output: the interrupt of DMA channel 0 (source 26, taken as a FIQ) and its refill (`fiq_playback`), the mixer's frame mix, and the PCM buffer's consumer callback |

This goes past Apple's own firmware, which also runs its audio engine on the second core but keeps the audio DMA's interrupt on the main core. [apple-RE]

**The decode** (since 0.8.0-rc.5):

- **The decode moves as a job.** The codec is loaded once, on the CPU, into the codec slot. Each run of it, normally one track, is posted to a worker thread on the COP, which runs it and hands it back (`apps/codec_cop.c`; the job machinery is `firmware/common/cop_job.c`). The default is set in one place: `place_mode = CPL_MODE_COP` in `apps/codec_cop.c`, and the modes are listed in `apps/codec_place_core.h`. Decoding on the CPU, and choosing the core by the screen state, remain as developer modes, for comparison. For MP3, the synthesis step runs as a second thread on the COP beside the decoder.
- **The codec's calls into the firmware cross back to the CPU.** Whenever the codec asks for more of its file, or hands over decoded audio, the COP stops and the CPU's codec thread does the work. So the DSP and `pcmbuf_insert` run on the CPU, and only the decoding itself runs on the COP.
- **Some decoding stays on the CPU.** An encoder (recording) always runs on the CPU, and so does the decoding of a video's soundtrack. Its audio still goes through the PCM buffer to the output on the COP, like any track.
- **A COP that will not decode is never silent about it.** If the COP's worker will not start, or will not take a job, that run decodes on the CPU instead and the log says `COP FAILED (start)` or `COP FAILED (post)`. A worker that never started also sets a latch, `cop_down`, for the rest of that boot. Every later track then decodes on the CPU at once, each one logged `COP FAILED (down)`, instead of stalling every track start for a second while it tries the dead core again.

**The output** (since 0.8.0-rc.6):

- **It is moved and proved at every boot.** After `audio_init()` and before anything can play, `audio_cop_init()` (`apps/audio_cop.c`) routes source 26 to the COP with `pcm_pp_set_core(COP)`, then plays a few milliseconds of silence. The COP must take at least 8 FIQs within 200 ms, and the CPU none. If the move is refused, or the FIQs do not arrive, the output goes back to the CPU for the rest of that boot and the log says `COP FAILED (pcm …): audio output on the CPU`.
- **The CPU still starts and stops it.** Every start, stop, pause and seek takes the PCM lock, a corelock that either core can hold; on the CPU it waits for a FIQ running on the COP to finish before it returns.
- **The code is in every release build.** `HAVE_CORE_LAYOUT_A` is defined for every shell build (`firmware/export/config/ipodvideo-variant.h`). The parked-COP experiment that came with it stays a developer switch, off by default.

Both fallbacks keep music playing. They are a safety net that logs loudly, not a mode to ship on.

**The user interface stays on the CPU.** The machinery of the retired 19-9 plan (`HAVE_UI_THREAD_SPLIT`) is compiled only into developer builds; a release contains none of it.

How it was checked: on 2026-11-07, sitting 4gh-b8 on a 5.5G passed the `p1` and `pcop` rows, which take the audio DMA's interrupt on the COP and return it to the CPU, and the `heap` row, an album decoding on the COP and then on the CPU with the heap checked every two seconds. [silicon] That proves the route on the bench; the release layout is soaked before 0.8.0 ships. The code statements in this chapter were checked by reading the 0.8.0-rc.6 source; `docs/changes-from-rockbox.md` §2.67 and §2.70 record the two changes.

## 8.3 What comes next

The hybrid target decided on 2026-11-06 kept the audio output on the CPU and moved the producers to the COP. 0.8.0 does the opposite for the output end: the output is on the COP, and the producers, the DSP and `pcmbuf_insert`, are still on the CPU. Moving them as well would leave the CPU with no audio work at all while music plays. The open design is in `docs/design/layoutA-engine-brief.md`, with the comparison of layouts in `docs/design/core-layout-decision.md`; read both before working on the audio pipeline.

## 8.4 Memory both cores touch

Every track now crosses between the cores twice: the decode's job on the way in, and the PCM buffer on the way out. These are the rules for memory both cores touch, and the reason for each:

- **Shared memory is uncached on both cores.** It is not enough for one core to bypass its cache. An uncached store goes to memory but does not touch the other core's cache; that core goes on reading its old copy, and once it writes any word of the line, its write-back puts old values back over the other core's stores. [ours] Declare shared statics with `XCORE_BSS` or `XCORE_DATA` (`firmware/export/xcore.h`), or `SHAREDBSS_ATTR`, which place them in memory neither core caches, or put them in IRAM, which neither core caches either. Shared memory you reach through a pointer needs 16-byte lines of its own, and **both** cores must reach it through the uncached alias. This is quirk 90 in `docs/pp5022-reference/quirks.md`.
- **Memory carved out again needs one write-back and drop first.** If a buffer is reused as shared memory, write back and drop the CPU's cache over it once before the first uncached use, or a dirty line from its earlier use can land on it later. [ours]
- **A producer writes back before it publishes.** The CPU writes audio into the PCM buffer through its cache, which is fast. It calls `commit_dcache()` before it hands a chunk over, and again after a crossfade writes into the buffer, and the mixer reads every source through the uncached alias (`pcm_mix_src()` in `firmware/export/pcm_xcore.h`), so no line of the buffer sits in the COP's cache to go stale on the next lap.
- **The FIQ posts; the CPU applies.** The FIQ on the COP never writes the playing track's record and never calls the dock code. It posts each chunk's position to a mailbox in uncached memory, guarded by a sequence count (a seqlock, `pcm_xcore.h`), and a tick task on the CPU applies it.
- **The FIQ makes no kernel calls.** No corelock, mutex, queue, semaphore, wake-up, sleep or yield, no tick list and no allocator: a FIQ that takes a lock its interrupted thread holds deadlocks its own core. The gate enforces this and the rule above: `scripts/cop_shared_check.py --fiq` walks everything the FIQ's code can reach in the firmware builds and fails on any kernel call, or on any cached static that code writes.
- **Memory that is cached on both cores changes hands only at a fence.** The codec slot is the one such area: the CPU loads the codec into it through its cache, and the COP runs the codec through its own. It stays correct only because the job machinery writes back and drops both caches at each hand-off (`copj_cache_ops()`). A host model, `tests/codecid3/test_handoff.c`, loses data when any one of those fences is removed, so a change that weakens one fails the tests. A second model, `tests/audiocop/`, does the same for the output: it fails if the write-back, the uncached read, the uncached indices or the mailbox's sequence check is taken away.
- **Do not add calls from one core to the other.** If something must cross, it crosses as a message or a job, not as a function call into code running on the other core.

The first rule came from a real defect: the codec's tag record (its "id3", with the track's length and elapsed time) was written uncached by the codec on the COP and through its cache by the CPU, so each could undo the other's writes. It was found by reading code and shown by a host model, not seen on a player, and 0.8.0 fixes it: the record now has lines of its own and both cores reach it uncached (`apps/codec_id3.c`, `docs/changes-from-rockbox.md` §2.68).

`docs/design/cop-shared-state.md` lists every object both cores touch, for the decode and for the output, with a verdict for each and the open questions; the output's open questions are settled by the soak before release. Read it, and add a row to it, before you change anything the codec, the COP's worker or the audio FIQ can reach.

**What to do in your own code:**

- **Put new work on the CPU**, in the thread that already owns the area, unless you are working on the audio engine itself.
- **Code the audio FIQ calls runs on the COP.** A mixer channel's callback, and anything it calls, must follow the FIQ's rules above. Build the firmware and run `scripts/cop_shared_check.py --fiq build/fw` before you send the change; the gate runs it too.
- **Never share a cached variable between cores.** If the COP writes it, nothing the CPU writes may sit on the same cache line. `docs/pp5022-reference/quirks.md` lists the faults this has caused.

## 8.5 Gotchas

**FW-8.5.1. A memory allocation while music plays can stop the music.**
Playback keeps the rest of the heap as its audio buffer. If code allocates memory while a track is playing, the heap takes the space from the audio buffer, and playback stops and restarts to shrink it: an audible gap, and a brief "stopped" state that screens watching playback can see. Opening the visualiser did exactly this until 2026-11-06. The fix is the rule: **allocate what a feature needs once, at start-up, before the audio buffer exists**, and keep it. The visualiser's overlay buffer, 8,208 bytes, is now allocated once when the BCM2722 driver starts, and never freed. If it is lost, the visualiser falls back to drawing on the CPU rather than allocate again while music plays. [sim]
