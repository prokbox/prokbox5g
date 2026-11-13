# Brief: the whole steady-state playback chain on the COP

*Owner direction, 2026-11-10 (road test): co-locate every per-block playback step on the COP; no synchronous COP→CPU calls in steady-state playback. Written after the 0.8.3 road-test fix (branch `p083-roadtest-fix`), which chose the small fix for the morning build. Evidence tags as in `docs/pp5022-reference/`: [silicon], [code], [sim], [hypothesis], [estimate].*

## 1. Why

- The 24/192 FLAC road test made 15,005 COP→CPU calls in about 102 s of audio (about 3.5 a 24 ms block) and waited 46.3 s in them (`codec dsp ... c=15005 cw=46290966`) [silicon, one log]. How much of that wait was healthy full-buffer sleep and how much was CPU latency was not measured. 0.8.3 adds the split (`codec cw win= full= rd= oth= lat=`).
- Every call that wrote the COP's memory (`COPJ_CALL_WROTE`: the window calls, `read_filebuf`) ends in a whole-cache drop on the COP. The COP also takes the PCM FIQ, and until 0.8.3 that drop held the FIQ off for its whole length. 0.8.3 shortens the masked stretch; removing the calls removes the drops.
- A call is served by the codec thread on the CPU. Rockbox threads are cooperative, so a long CPU job (an art pass: one panel took 18.5 s) delays each call until the CPU's next yield [code].

## 2. Target design

### Input: the COP reads the compressed ring itself
- The buffering thread (CPU) keeps filling the ring in big bursts, as today. The CPU keeps the disk and the FAT. Upstream's reason (66258a30a4, file-system protection) still holds.
- The COP reads compressed bytes straight from the ring. It reads either through the uncached alias, or cached after dropping its lines for that span. Measure which is cheaper (R7 item 5 estimates uncached at 10–100 KB/s). The FLAC road test moved about 2 MB/s, so measure it at that rate.
- The read and write positions are single-producer/single-consumer words in uncached memory (R7 item 1). The COP stores "consumed up to X" and the CPU reads it when it next wakes. There is no `cop_job_call` per `request_buffer`/`advance_buffer`/`read_filebuf`.
- Handle wrap and handle boundaries the way `bufgetdata()` does today. A read that would cross the end of the handle's data falls back to one call, as do a track end and a rebuffer.

### Output: pcmbuf's producer and consumer on one core
- The DSP (producer) and the FIQ (consumer) both run on the COP, so the per-window grant and publish no longer cross.
- The COP owns `chunk_widx` and the descriptors it writes. The CPU sees level and position updates through the existing uncached mailbox (`pcm_xcore.h`), at most once a tick.
- The CPU keeps the starts, stops, crossfade, track-change marks and boost decisions. These become commands to the COP, or happen while the COP is quiesced.

### Calls that stay (rare, command-style)
- track change;
- seek;
- settings (DSP configure, through `dsp_xcore_lock`);
- stop and pause;
- the codec's queue messages.

## 3. Tests before code
- A `dspcop`-style two-cache host model of the SPSC ring. Prove that removing each fence makes the test fail (a stale position, a stale data line, a lost write-back).
- `codecplace` with the new path, including the fallback to one call at a boundary.
- Emulator A/B: the 24/192 → 96 kHz reproduction from the 0.8.3 hand-back, with the art pass on and off. Compare `c=`, `cw`, the `fiq e=`/`m=` lines and decode µs/block.

## 4. Risks
- **Buffering's handle model** (`bufgetdata()`'s contiguity, rebuffer, handle moves on a buflib compaction). The CPU may move a handle under the COP, so the COP must hold a pin, or each compaction must quiesce the COP.
- **The cache rules.** One drop per span, never per byte. A wrong fence corrupts audio silently, so the host model comes first.
- **Size.** Code moves; no new statics over 1 KB.

## 5. Not in scope
The disk, the FAT, metadata parsing and the database stay on the CPU.
