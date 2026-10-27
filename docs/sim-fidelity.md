# Simulator fidelity audit: PP5022 and shell side

Written 2026-10-25 (branch `simfid`, from `p19`). Companion to the BCM-side audit (`vc2sim` strict mode, `docs/bcm2722-reference/memory-access.md` §11).

**Why.** The P12 bench found a misaligned halfword store that aborts on silicon and passes on macOS (`quirks.md` #51). The BCM side hid two more (a sign-extending halfword load form, host-port writes dropping address bits 1:0). The pattern: **the simulator or a host model accepts something the device does not, and nothing says so.** This file lists every such place we know of, ranks them, and proposes the cheapest check that makes the sim *refuse* what silicon might do differently.

**Scope.** `uisimulator/` (SDL sim, `build/sim*`), the host tests (`tests/*/run.sh`, `warble`) and `scripts/validate.sh`. Docs plus two trivial checks; everything marked *proposed* is for the owner to rank.

Evidence tags as in `docs/pp5022-reference/index.md`: [silicon] measured on our unit, [apple], [rockbox] source reading, [hypothesis] unverified, [ours] our convention. "Sim" = macOS arm64 build, 64-bit host, Apple clang/gcc, SDL threads.

## Ranked summary

Rank = (how likely the divergence bites) x (how silent the failure is) x (how expensive on the device). "Cost" is the proposed check's cost.

| # | Area | Sim vs device in one line | Risk | Guard today | Proposed check (cost) |
|---|---|---|---|---|---|
| 1 | Cross-core coherence, the COP | Sim is one core, no cache; device has two cores, two non-coherent caches (quirks #8, #52) | **High**, silent data corruption | `HAVE_COP_COHERENCE` placement, `tests/copjob`, `coplab` | Sim "shared-object lint" + a cache-line aliasing model in `tests/copjob` (M) |
| 2 | Memory layout / IRAM / uncached alias | Sim has one flat heap; device has cached logical SDRAM, an uncached alias, 32 KB IRAM | **High**, hang or stale data | `tools` stack/IRAM size reports, size gate | Linker-map check: every `IRAM_ATTR`/uncached object listed against a whitelist (S) |
| 3 | Alignment, remaining gaps | Strict via align shim and UBSan; gaps: opt-out `-fsanitize-recover` in the module twin, packed structs, `memcpy` hides, asm | **High**, data abort | `scripts/align-shim`, `tests/aligncast`, `sim_ubsan.sh` STRICT | Extend baselines (see 3 below) (S) |
| 4 | Type sizes, `long`, pointers, struct layout | Host `long`/pointers 64-bit; device ILP32 | **High**, layout, overflow, format bugs | 41 `_Static_assert`s in `apps/ipodui/*.h`; `fw-ui` build is ILP32 so asserts fire there | Assert every on-disk struct; `-Wconversion`-style lint on `long` (S) |
| 5 | Timing: CPU speed, no 30/80 MHz clock | Sim is 1-3 GHz out-of-order; device 30 MHz (unboosted) | **High**, jank, missed deadlines pass | `tests/perf` ELF instruction counts (`qemu-harness`), `sim_cmd_clock` | Instruction-budget gate per frame already; add "slow sim" mode (CPU throttle) (M) |
| 6 | Storage: no ATA, no flash/HDD, no power cut mid-write | Sim reads host files; device ATA with write cache, 2048-B virtual sectors, wake costs | **High**, data loss, power | `tests/fatcrash/fsreplay`, devstats disk model, `sim_cmd_storpwr` | Fault injection: crash at every write-order point (M) |
| 7 | Audio: mixer frame 4x, PCM clock, underrun | Sim 1024 samples/frame, SDL pulls; device 256, DMA-driven | Medium-High | none until now | `-DSIM_DEVICE_MIXER` (implemented); device-sized frames in an audio gate (S) |
| 8 | Threads and locks: no ISR, cooperative only | Sim: SDL mutex hand-off, no interrupts, tick tasks on a timer thread; device: IRQ/FIQ preempt, COP parallel | Medium-High | `corelock` code paths, `tests/lock` | Run tick tasks only at yield points in sim ("deterministic ISR") (M) |
| 9 | BCM job model | Sim models jobs (`sim_bcm_job.c`, `jmodel`); the port, timing and torn reads are not modelled | Medium-High | `vc2sim`, `tests/bcmjob`, device bench | Shared fixtures between vc2sim and `jmodel`; cross-check (M) |
| 10 | LCD: partial update, host-port frame time | Sim blits instantly; device ~27 ms CPU/frame at 30 MHz, torn reads (quirks #17, #34) | Medium | LCD update-rect budget tests | Sim charges the measured cost per rect to its virtual clock (M) |
| 11 | Clock and PLL | Sim has no clock; boost is a counter | Medium | `cpu_boost` balance gate | Virtual clock for boost duty (M) |
| 12 | Screen arena size | Sim's arena reported 12 KB larger than the device's | Medium | `tests/arena`, floor gate | Verify and remove or assert equality (S) |
| 13 | Memory size 32 / 64 MB | `sim-ui32` / `sim-ui` via `--ram`; the rest of the host RAM is hidden | Medium | floor gate (`tests/perf/floor_gate.sh`) | Cap the sim's heap with `RLIMIT_AS` plus guard pages (S) |
| 14 | USB | Sim has no controller: no dTD stalls, 304 ms hangs, cache/uncached rules | Medium | on-device bench only | Host test of buffer rules + bench row (S) |
| 15 | Buttons and wheel | Sim: instant clean events from key map; device: packet stream, bounce, hold switch, wheel rate | Medium | `wheel`/`hold` script commands | Wheel replay from a recorded device trace (M) |
| 16 | Battery and power | Sim drains 1 %/s or pins; device curve, charging, standby, boot guard | Medium | `battery`, `IPODUI_SIM_BATT_MV` | Replay a logged discharge (`battlog`) (S) |
| 17 | Endianness | Both little-endian; no divergence, but host tests on big-endian are untested | Low | none needed | Note only |
| 18 | Floating point / libm / `printf` | Host libc vs Rockbox's own `snprintf`, no FPU on device | Low-Medium | `-Wdouble-promotion` not on | Warn on `float`/`double` in `apps/ipodui` (S) |
| 19 | Filesystem semantics | Host FS: case sensitivity, path length, rename atomicity differ from FAT32 on 1 TB | Medium | `fatcrash` model | Run shell file ops through the FAT model in tests (M) |
| 20 | Stack and heap size | Host stacks are MB; device thread stacks are KB | Medium | `scripts/risk/stack_usage.sh` | Run sim threads on small stacks (S) |

## Entries

### 1. Cross-core coherence (the COP)
- **Sim.** One core. `PROCESSOR_ID` is CPU; no cache; every store is visible at once. The COP code paths (`ci_cop`, shared flags, engine-2 claim) are exercised only as plain function calls.
- **Device.** Two cores, each with its own cache status; no coherency between them or with DMA; a write-back is per core (`commit_dcache()` writes back the caller's lines only). Shared data must be in IRAM or the uncached alias. [silicon]/[rockbox] `quirks.md` #8, #11, #52; whether each core has its own data array is open (#51 "Open", COP row C2) [hypothesis].
- **Risk.** Stale reads, lost updates, DMA started from the wrong core; invisible in the sim by construction.
- **Guard.** `HAVE_COP_COHERENCE` (S0 placement of shared objects in uncached memory, `ipodvideo.h`), `tests/copjob`, `tests/coplab`; owner decision 2026-10-24 pins decoding to PP1 and the UI move to PP2 is later, so the exposure grows then.
- **Proposed.**
  1. *Lint (S)*: a script that lists every object touched from both a COP-eligible path and the CPU, and asserts it carries the shared/uncached attribute from `xcore.h`.
  2. *Host model (M)*: a two-core cache model in `tests/copjob` (per-core write-back lines, no snoop) so a missing flush/invalidate fails with the address and the core.
  3. *Bench row*: COP rows C1-C3 already exist; do not promote any COP claim to [silicon] before they run.

### 2. IRAM and memory layout
- **Sim.** Flat 64-bit heap from `malloc`; `IRAM_ATTR`/`IBSS_ATTR` expand to nothing; `core_alloc` runs in a host buffer. No uncached alias, so `0x1000_0000 | addr` tricks are untestable. Addresses are not 32-bit, so address arithmetic on integers truncates silently in one build and not the other.
- **Device.** 32 KB IRAM shared with codecs (rule: IRAM is for codecs), cached SDRAM at logical 0, uncached alias at +0x1000_0000, DMA wants physical addresses and 16-byte alignment (ATA reads, quirks #27, #28). [rockbox]; remap rules `memory-map.md`.
- **Risk.** A DMA source/destination in the wrong window, an object moved out of IRAM by mistake, `uintptr_t` truncations.
- **Guard.** Size gate (`_end`, text/bss), `scripts/risk/stack_usage.sh`, `fw-ui` link.
- **Proposed (S).** After the `fw-ui` link, diff the map against a checked-in list of IRAM and uncached symbols: a new entry fails the gate and names the symbol and size. Also, in the sim, allocate the audio and `core_alloc` arenas with `MAP_32BIT`-like low addresses where available or assert `(uintptr_t)p < 2^32` in a debug mode so pointer-to-`int` casts surface. (macOS arm64 has no `MAP_32BIT`; use a `-Wpointer-to-int-cast` error instead, which is free.)

### 3. Alignment, and what is still open
- **Sim.** Strict now: UBSan sim with `-fno-sanitize-recover=alignment` (STRICT scripts for SDK, games, modload), every host test compiled through `scripts/align-shim`, `tests/aligncast` baselines byte-offset casts. [silicon] fault: `quirks.md` #51.
- **Remaining gaps.**
  1. The plain `sim-ui` and non-UBSan builds still accept it; only the UBSan builds refuse. A script not in the UBSan pool (`ipodui-p14a-*`, no video player there) is not checked.
  2. UBSan checks *typed* accesses. `memcpy` into a misaligned field is legal on both and fine; but an ARM-only path (assembly, `memset16`, `memcpy` fast paths that assume 4-byte alignment, codec `.S` files) is checked by neither.
  3. Unaligned *source* to `memcpy`/`memmove`/`memset16` helpers whose ARM versions use `ldm`/`stm` (those also abort on odd addresses; `ldrd` etc.). The host libc hides it.
  4. `__attribute__((packed))` structs: GCC emits byte accesses on ARMv5, so safe on device; the shim gives a false pass either way.
  5. Alignment of *file image offsets* (a table after a variable-length header) is only checked when a test loads that image.
  6. The sim was built non-recovering only for the listed STRICT zones; the rest report-only (see `sim_ubsan.sh`), so a new offender elsewhere is a note, not a failure.
- **Proposed (S).** Make report-only findings in `apps/ipodui` and `firmware/target/arm/ipod` fail the gate too, behind a baseline file of known sites (the same pattern as `tests/aligncast/baseline.txt`). Add the ARM `memcpy`/`memset16` helper preconditions as `assert((uintptr_t)p % 4 == 0)` in the sim-only wrappers (dev-mode only).

### 4. Endianness, type sizes, struct layout
- **Sim.** LP64: `long` and pointers 8 bytes, `size_t` 8, `time_t` 8, struct padding follows 8-byte alignment of `long`/pointers. Both little-endian.
- **Device.** ILP32 ARMv5: `long` 4, pointers 4, `int64_t` aligned 4 in EABI structs padded to 8 only where declared. [rockbox]
- **Risk.** (a) An on-disk or on-wire struct containing `long`/pointer/`size_t` has different size per build; the sim writes a file the device cannot read (or the reverse for the sync tool). (b) Arithmetic that overflows 32 bits on device but not on host (`ticks * 1000`, byte counts above 4 GB on a 1 TB card). (c) `printf("%ld")` with `int` arguments passes on one ABI and not the other.
- **Guard.** 41 `_Static_assert(sizeof(...))` in `apps/ipodui/*.h` (alarm, stopwatch, xindex, xcontact...). The firmware build is ILP32, so these fire in `fw-ui`. Host tests and the sim cannot see an ILP32 mismatch, so the asserts are the only line.
- **Proposed.**
  1. *(S)* A grep gate listing every struct written with `write()`/`fwrite()` in `apps/ipodui` against those with a `_Static_assert`; names the file and struct.
  2. *(S)* `-Wformat=2 -Wconversion` over `apps/ipodui` with `arm-none-eabi-gcc` (already in the gate environment): the ILP32 compiler is the check; no new sim needed.
  3. *(M)* Overflow audit of tick and byte arithmetic with UBSan `-fsanitize=signed-integer-overflow` plus `-m32`-like narrowing: build the host tests with `-DLONG_IS_32` style typedef overrides is not safe; prefer rule 1 and 2.

### 5. Timing and CPU speed
- **Sim.** Host CPU is ~50-100x faster than the PP5022 at 30 MHz; `HZ` is 100 on both (`firmware/kernel/include/tick.h`); the sim tick comes from an SDL timer with `SDL_Delay` granularity (~1-10 ms); `sleep(HZ/10)` style waits are exact in wall time.
- **Device.** 30 MHz unboosted, 80 MHz boosted; instruction cost, cache misses, host-port copies (27 ms per full frame at 30 MHz, quirks #17), IDE wake times. [silicon] S8, [rockbox].
- **Risk.** Anything CPU-bound passes: frame budgets, marquee cadence, boost durations, sort times at 50k, race windows that only open when slow.
- **Guard.** `qemu-harness` instruction counts (`scripts/risk/arm_insn_count.sh`), budget "300k instructions per 33 fps frame" (process rule 8), the perf floor gate, `sim_clock_cpu_us()` for the UI thread's CPU figure.
- **Proposed.**
  1. *(M)* A sim `--slow N` mode: a per-thread CPU-time governor (spin or `nanosleep` after each tick-quantum proportional to measured CPU) so races and frame overruns open up. Pairs with #8.
  2. *(S)* Make `tests/perf` fail when any path lacks an instruction count and relies on wall time.
  3. Mark wall-time-dependent scripts `# golden: skip` (already the rule).

### 6. Storage
- **Sim.** `filesystem-sim.c` over host files: no ATA, no sector size (the 5.5G's 2048-byte units on a 512-byte drive, quirks #29, #50), no write cache, no spin-up, instant seeks, a case-sensitive-ish FS (APFS is case-insensitive by default, FAT32 is case-insensitive with 8.3/LFN rules), host rename is atomic. A devstats disk *model* counts wakes but does not delay.
- **Device.** ATA with write cache flushed only at standby (quirk #41, [hypothesis] on write ordering after a power cut), flash adapter negotiates UDMA 2 [silicon], 16-byte DMA alignment, flash vs HDD runtime-detected (`docs/storage-choices.md`), FAT32 on 1 TB with large clusters.
- **Risk.** Partial files, ordering bugs at power-off, assumptions about sector size or file-name rules, behaviours tuned only on the flash unit.
- **Guard.** `tests/fatcrash/fsreplay` (replays a session through the FAT layer), `tests/dbjournal`, `sim_cmd_storpwr`, `storage-choices.md`.
- **Proposed.**
  1. *(M)* Crash-injection: for each write-order point the file layer flushes (R11, #91), a host test cuts the model at every point and reboots; the check names the point and the file left half-written.
  2. *(S)* Sim option to run on a case-sensitive, 8.3-restricted view (reject names FAT32 would reject: reserved characters, trailing dots, >255 UTF-16 units).
  3. *(bench)* One row for HDD: nothing in the sim distinguishes it; the owner has only a flash unit.

### 7. Audio (mixer frame 4x)
- **Sim.** `MIX_FRAME_SAMPLES` is 1024 on hosted targets (`firmware/export/pcm_mixer.h`: "hosted targets need larger buffers"); the PCM sink is SDL's pull callback (`pcm-sdl.c`). Unlimited CPU, no DMA half-buffer interrupt.
- **Device.** 256-sample frames (`#else` branch for PP502x), DMA-driven I2S, a mixer step in the DMA interrupt; PCM chunk boundaries, underrun, and crossfade/volume-ramp granularity differ 4x. Visualizer agent report, 2026-10-25. [rockbox]
- **Risk.** Latency-sensitive features (click, fades, speed change, gapless, visualizer, `mixer_channel` callbacks) have their step size wrong; callbacks that assume >=1024 samples per call overrun on device.
- **Guard.** Bit-perfect hash test (`Audio path test`, P15), `tests/fade`, `tests/bitperfect`.
- **Implemented.** `-DSIM_DEVICE_MIXER` gives device-sized frames in the sim (below). Not on by default: SDL's callback may under-run at 256 on a loaded host.
- **Proposed.** (S) Run `tests/fade`, `tests/bitperfect` and one playback UI script once per gate with the define; compare the hashes to the 1024 build (they must match for steady-state output; a difference is the finding).

### 8. Threads, locks, interrupts
- **Sim.** SDL threads hand off through one mutex (`thread-sdl.c`: "enables us to simulate a cooperative environment even if the host is preemptive"). No interrupts: tick tasks run from the timer thread; `disable_irq()` is a no-op or mutex; `corelock` degenerates.
- **Device.** Cooperative threads on each core, but IRQ/FIQ preempt anywhere (tick, DMA, USB, wheel), the COP runs in parallel, `disable_irq` really matters. [rockbox] `cores-and-threads.md`.
- **Risk.** Data shared between a thread and a tick task or ISR with no lock; held-lock-across-block; priority-inversion; `yield()` polling that keeps a core awake (quirk #53 [hypothesis]).
- **Guard.** `tests/lock`, S0 lock placement, UBSan/ThreadSanitizer not run.
- **Proposed.** (M) A sim "ISR at yield" mode: tick tasks delivered only at `switch_thread()`/`yield()` points, plus a randomised-order option; run a script pool under it. (S) Grep gate: any `tick_add_task` callback may call only an allow-listed API.

### 9. BCM job model
- **Sim.** `sim_bcm_job.c` and `jmodel` execute jobs functionally; ideal bus; reads of 32 bits are never torn; ordering is as submitted.
- **Device.** Host port: 32-bit read is two 16-bit reads and can tear [silicon] S11, S12 (quirk #34); write address bits 1:0 are dropped; waits can hang when the BCM is unpowered (#35); DMA into the port has never delivered a byte (#36). See `docs/bcm2722-reference/memory-access.md` §11 for the BCM-side strict rules and `docs/data-paths.md`.
- **Risk.** A job layout that works in `jmodel` and corrupts on the port.
- **Guard.** `vc2sim`, `tests/bcmjob`, `tests/jpeg`, device bench. **vc2sim's computation is silicon-exact for scalar C at 150 MHz under load** [silicon, stage 3d 2026-10-26: ten 3D check frames CRC-equal at 27 and 150 MHz]; its timing needs the per-workload factor (3D: × 1.14–1.64; the decoder's FEED fit).
- **Proposed.** (M) The PP-side job packer calls the same port-access model as vc2sim strict mode (shared header), so one set of rules refuses both ends. Owner: BCM session.

### 9a. BCM job model: lessons from sittings 3 to 4b (added 2026-10-26)

The sitting 4b first-use check is the case study: a host-side model that was more permissive than the image hid a real ordering bug until the device ran it.

- **The job model must not be more permissive than the image.** The host-test model of the chip (`vbcm_model.c`) let the core see a host write the moment it was made, so a `FEED` sent right after its packet passed on the Mac and gave no picture on the chip at any band (`bcm2722-reference/quirks.md` #47; mechanism a [hypothesis], BCM #73). The model now refuses a `FEED` of bytes the host has not read back or let settle (`VBM_LAND_US`: 2 ms, or 4 KB written after them), and the old check sequence fails 14 rows against it (`tests/video/test_vcheck.c`, `t_order`). vc2sim's port model reproduces the late write the same way (`bcmfw/tests/vcheck_image_test.py`). Rule: every behaviour the device showed that a model skipped becomes a refusal in the model, with the silicon evidence in its header comment. Proposed: a model hole found on the device is closed in the same change as the driver fix.
- **`sim_bcm_job` takes its capabilities from the image, as silicon does.** DECODE is offered only once `dec-ovl.bin` has been loaded in the current epoch, and SPECTRUM, BARS and the line ops only once the visualizer's overlay is loaded. The simulator models the shared overlay region's eviction (a load replaces the other overlay) and the `BUSY` refusal of a visualizer load meeting a decode in flight, and `TONE` is refused while an overlay is live. It cannot model the chip's posted writes or its timing of a swap.
- **New script commands** (`uisimulator/common/sim_cmd_bcm.c`):
  - `bcm_suspend on|off`: the job driver suspended, as after a device stage play that left the chip on Apple's program (`quirks.md` #48): `bcmjob_have()` false, `FENCE` too, until it resumes. Use it to test the shell's loud CPU fallbacks.
  - `bcm_viz ovl on|off|refuse`: the visualizer overlay's gate (on by default, as on the device); `refuse` makes a load fail.
  - `bcm_decode ovl on|off|refuse`: the same for DECODE's overlay (a separate form from `bcm_decode on|panel|off [pace N]`, which sets whether the model decodes at all).
  Scripts: `ipodui-p19-relviz` (the BCM path through the overlay, a cut and its reload, no CPU fallback) and `ipodui-p19-relviz-refuse` (the missing file gives the loud CPU net).
- **The simulator cannot produce the silicon freezes.** Sitting 4's silent freezes (a branch to address 0 that spun with IRQs masked, the confirmed cause, `pp5022-reference/quirks.md` #56; and the unbounded chip waits found beside it) have no counterpart in the sim: its chip never wedges, and a NULL call faults in the host. What guards them instead: a bound and a FAULT line on every chip wait (`bcm2722-reference/host-driver.md` §2a), the PP's hang record (the sim has `hang_ui MS` and `ipodui-hang-record.script`, which stall the UI pass to exercise the watcher, not the real failure), and, at build time, `scripts/febe_link_check.py` over the linked `rockbox.elf`, run by the gate: it refuses a wrapped name that links as a data object (the stub's code read as a struct, p19-bssfix), the family of wrapping mistakes that jump to 0 comes from. It does not replace a device run.

### 10. LCD and backlight
- **Sim.** `lcd-sdl.c` copies to a texture immediately; `lcd_update_rect` has no cost; backlight is an overlay; no DMA frame time; no tearing; no reads from the panel.
- **Device.** The panel is behind the BCM host port: ~10.5 PP cycles per halfword, ~27 ms CPU per full frame at 30 MHz [silicon] S8 (quirk #17); partial updates matter; backlight has no absolute register (quirk #33).
- **Risk.** Full-screen redraws and per-pixel work look smooth in the sim.
- **Guard.** Lean rules 1-4, ELF instruction harness; `sim_cmd_video`, devtest metrics.
- **Proposed.** (M) The sim accumulates "device milliseconds" per update rect from the measured model (cost = pixels x 350 ns + setup) in a virtual clock and logs the worst frame; the gate fails on a frame above a budget. Cheap form (S): log pixels pushed per user action and compare with a baseline.

### 11. Clock, PLL, cpu_boost
- **Sim.** No clock; `cpu_boost(true/false)` toggles a counter; frequency scaling does not exist. The sim never stalls for a PLL change.
- **Device.** 30/80 MHz, stalls both cores during a switch, `DEV_TIMING1` coupling, UDMA mode >2 needs 80 MHz (quirks #12-#16). [rockbox]/[silicon]
- **Risk.** Unbalanced boost passes unless the gate counts; a blocking wait done boosted wastes battery; code that runs inside a clock switch.
- **Guard.** `cpu_boost` balance gate, `sim_cmd_clock` (header phase only), battery tests.
- **Proposed.** (M) A virtual clock: boosted time accumulates in the sim log and the gate reports boosted-ms per scenario, failing a budget (ties to #5 and #16).

### 12. Screen arena
- **Sim.** Reported by the visualizer agent as 12 KB larger than the device's. Not verified in this pass: `UI_ARENA_SIZE` is 9,216 B for the screen arena (`apps/ipodui/screen_stack.h`); the 12,288 B figure matches the codec-slot reservation (changes-from-rockbox, 2026-10-24). One of these may be the difference; the audit could not tell which.
- **Device.** The numbers in `docs/hardware.md` (arena rows).
- **Risk.** A screen that fits only in the sim's larger arena passes every script and fails on the device with an allocation refusal that is logged, not shown (rule 1).
- **Guard.** `tests/arena` (refusal case at `UI_ARENA_SIZE + 1`), floor gate.
- **Proposed.** (S) Find the sim-only `#ifdef SIMULATOR` that enlarges it (the 248 `SIMULATOR` sites in `apps/ipodui` are the search space; `grep -n "ARENA" apps/ipodui/*.c | grep -B2 SIMULATOR`) and delete it, or assert at boot that the sim's size equals the device's. Highest priority of the cheap fixes: it directly hides a device failure.

### 13. Memory size (32 / 64 MB)
- **Sim.** `--ram` (`MEMORYSIZE`) sets the audio buffer; the process itself uses host RAM for everything else: no 32 MB ceiling, no fragmentation of `core_alloc` caused by a smaller heap, resident usage unmeasured.
- **Device.** 32 or 64 MB shared by code, `.bss` (~1.1 MB), audio buffer; floor 7.5 MB / 39 MB [ours].
- **Risk.** A transient allocation that fits the sim's heap but not the device's fragmented one.
- **Guard.** Floor gate at 50k tracks on `sim-ui32`/`sim-ui`, the budget table in `hardware.md`, `core_alloc` receipts.
- **Proposed.** (S) Run the floor scripts with the `core_alloc` heap the exact device size (the buffer is already sized by `MEMORYSIZE`; confirm host allocations outside `core_alloc` are listed in a gate receipt) and `RLIMIT_AS` set generously above the model so a stray `malloc` growth shows up.

### 14. USB
- **Sim.** No USB device controller. `usbscreen.c` (14 `SIMULATOR` sites) simulates entering and leaving; the storage thread is not exercised through a real host.
- **Device.** ARC/ChipIdea controller, PHY sequence, INQUIRY buffer in uncached page-aligned memory, bulk-IN dTD can stay ACTIVE ~304 ms, ATA DMA concurrent. [silicon] `usb.md` §6, `data-paths.md` D12, quirks #43-#47.
- **Risk.** The sim USB screen is a stage set.
- **Guard.** Device bench, `usb_storage` counters.
- **Proposed.** (S) Host test for the buffer *rules* (alignment, uncached alias, no stack buffers) as a static check on `firmware/usbstack`; no sim controller is worth building.

### 15. Buttons and wheel
- **Sim.** Key map to button events; the wheel is a scripted delta; no bounce, no packet loss, no hold-switch latency.
- **Device.** Serial packet stream through the wheel controller, rate-dependent acceleration, the I2C interrupt shared with the wheel (quirk #19), the hold switch, the piezo click (`DEV_PIEZO == DEV_OPTO` quirk #38).
- **Risk.** Scroll feel, key-repeat thresholds, chords (`press_hold`), long-press timing.
- **Guard.** `wheel`, `hold`, `key` script commands; `tests/click`, `tests/nav`.
- **Proposed.** (M) A `wheel_trace <file>` replay of a recorded device packet log (the logger already exists in the bench) so scroll tests run on real data.

### 16. Battery and power
- **Sim.** `powermgmt-sim.c`: linear drain (1 %/s) or pinned; charging flag; `IPODUI_SIM_BATT_MV`.
- **Device.** Discharge curve, 3,000 mAh pack, PMU charge states, standby/wake rules (quirks #39, #40, #49), bootloader never reads the battery.
- **Risk.** Low-battery guard timing, power-off decisions, cell-voltage hysteresis.
- **Guard.** `tests/batcal`, `tests/battlog`, `tests/bootbatt`.
- **Proposed.** (S) Feed a logged real discharge into the sim and assert level transitions against the `battlog` record.

### 17-20. Smaller items
- **17 Endianness.** Both little-endian; nothing to fix. Keep byte-explicit readers for on-disk formats so a future big-endian host (a CI box) is not a surprise.
- **18 Floating point.** The device has no FPU: floats are software and slow; the sim runs them free. Proposed (S): `-Wdouble-promotion -Wfloat-conversion` and a grep gate on `float`/`double` in `apps/ipodui` (allow-list).
- **19 Filesystem semantics.** See #6.2; also long paths (PATH_MAX 4096 vs Rockbox `MAX_PATH` 260) and non-UTF-8 names. Proposed (S): the sim rejects paths over `MAX_PATH`.
- **20 Stack sizes.** Host thread stacks are MBs; device threads have KB (`DEFAULT_STACK_SIZE`, per-thread constants). Guard: `scripts/risk/stack_usage.sh` (static). Proposed (S): create SDL threads with the device stack size plus a canary in the sim so deep recursion or big locals fail there.

## Implemented in this pass

1. **`-DSIM_DEVICE_MIXER`** (`firmware/export/pcm_mixer.h`, guarded by `SIMULATOR`, off by default): the sim mixes in the device's 256-sample frames instead of 1024. Recorded in `docs/changes-from-rockbox.md`. To use it, add the define to the sim build's compiler flags, by appending it to `GCCOPTS` in the generated build Makefile (the way `--with-ubsan` does in `tools/configure`). Untested: not built or run in this pass, and not run through the gate.
2. This document.

Nothing else was implemented. The ILP32 struct asserts already exist (41 sites); the static layout check for on-disk structs (#4.1) is proposed.

## Rules this audit suggests adding to `docs/process.md`

- A new sim-only behaviour (`#ifdef SIMULATOR`, a script command that makes something easier than on the device) goes in this file with its device counterpart and a proposed check. The sim has 248 such sites in `apps/ipodui` today.
- A bench row confirms any [hypothesis] this file relies on (cache arrays #1, `yield()` keeping the COP awake #8) before a design depends on it.
- A model of device behaviour is [hypothesis] until a bench row checks it; say so in its header comment (the BCM side already does).

## Only the device can check

COP cache behaviour (rows C1-C3), write-order after a power cut, USB stalls, host-port torn reads under load, the real discharge curve, wheel noise, HDD behaviour, and everything about 30 MHz timing at 50k tracks.

## Device surprises (bench sittings 3, 4, 4b and 4c, 2026-10-26)

Every behaviour the device showed in these sittings that the simulator or a host model did not, ordered by how likely it is to bite a game or app developer. Each entry gives what the sim did against silicon, the model change that now reproduces it, and the regression test that fails on the old behaviour. The rule they share: **a model is never more permissive than the silicon**; a hole found on the device is closed in the model in the same change as the driver fix, with the evidence in the model's header comment.

**1. A host write into the chip's SDRAM is not seen before a command that follows it closely** (sitting 4b; `bcm2722-reference/quirks.md` #47, open question #73; `data-paths.md` D16).
- *Sim against silicon.* The host model let the core see every port write the moment it was made, so the first-use check, which sent each `FEED` right after writing its packet, passed on the Mac; on silicon the image took the packet and decoded no picture at all seven bands.
- *Model.* Already changed (branch `p19-vcheck-silicon`): `apps/video/vbcm_model.{c,h}` holds the host's SDRAM writes back until a host read of SDRAM, 2 ms (`VBM_LAND_US`) or 4 KB more writes (`VBM_LAND_BYTES`); a `FEED` of bytes still in flight is consumed with no picture and no error, as on silicon. vc2sim's port model does the same.
- *Test.* `tests/video/test_vcheck.c` `t_order` (the old sequence fails 14 rows); `bcmfw/tests/vcheck_image_test.py` against the merged image in vc2sim.
- *For developers.* Write data ahead of the command that uses it, or read its last word back through the port first (`host-driver.md` R1 step 6).

**2. A port read costs 8.9 us, not nothing** (sitting 4c; open question #76).
- *Sim against silicon.* The whole-plane first-use check took 887 ms in the simulator and 7.99 s on the device: about 920,000 word reads in about 7.8 s. The check now sums one sampled row per macroblock row and takes about 0.9 s on silicon. The simulator charged nothing per read; the host test charged an [estimate] of 0.5–1 us.
- *Model.* `VBM_RD_NS` (8,900 ns a 32-bit read, the calibration and its source in `vbcm_model.h`). The simulator's `bcm_video_rd()` (`uisimulator/common/sim_video.c`) holds the reading thread that long, paid in 1 ms steps against a running deadline so host oversleeps don't accumulate; `SIM_PORT_RD_NS` overrides it (0, the old free port, for A/B only). The sampled check takes about 0.9 s in the simulator (832 ms measured).
- *Test.* `tests/video/test_vcheck.c` `t_time`: see that file for the time window the sampled check must meet at `VBM_RD_NS`. `tests/ui/ipodui-p19-vcheck.script`: `# logrange: 600 1500` on the passing band's time.
- *Finding it exposed (resolved).* Under the old 8 s check a unit that decoded wrong never reached the error screen (budget ran out after three bands: INCONCLUSIVE, film plays). With the sampled check each band fails at picture 5 in 0.2–0.3 s, all nine bands fail in about 2.1 s, inside the 4 s budget, and the styled error screen comes; the vcheck script pins this.
- *For developers.* Anything that reads back through the port (pictures, results, polls in a loop) costs about 8.9 us a word on an idle chip and far more under a decode (35–85 us, `data-paths.md` R2): 1,000 reads are 8.9 ms of a 33 ms frame.

**3. A stage play's stop hands the chip to Apple's vmcs: the jobs stay off** (sitting 4b; `quirks.md` #48, `data-paths.md` D17, `host-driver.md` "Restart, reload, hand back").
- *Sim against silicon.* The simulator's stop always went back to a chip with jobs; on the device a bench or stage play (or a lab, a probe) boots the image from its file, and its stop takes the stock wake path, so the visualizer ran on the CPU (`CPU FALLBACK why=nojobs`) and the album panel decoded on the PP until our image started again. `bcm_suspend on|off` existed but only by hand.
- *Model.* `bcm_video_release()` (`sim_video.c`) now does the hand-back itself: after a play that was not the resident image's own (`sim_video_resident()`, set by `vplayer.c`'s `load_image_from()` from the play's kind: a bench play, stage 6 to 8, a lab or a probe) it suspends the job model ("the chip on Apple's vmcs"); a later resident play's stop resumes it (a new epoch, as our image's start). The script command `video_start <path> stage` plays a clip as a bench step does.
- *Test.* `tests/ui/ipodui-p19-stage-handback.script`: the visualizer on the BCM, a stage play's stop, the visualizer on the CPU with `why=nojobs` and no BARS job, then a normal play's stop and BARS on the BCM again. Fails on the old sim (no hand-back line, no fallback).
- *For developers.* `bcmjob_have()` can turn false between two runs of your code with nothing you did; every chip job needs its loud CPU path, and a bench orders the steps that need jobs before the first program swap.

**4. One overlay region: the art pass's DECODE evicts the visualizer's overlay** (sittings 4b/4c; `quirks.md` #49, open question #74).
- *Sim against silicon.* Owned by branch `p19-vizown`, which fixes the thrash and adds its script; not duplicated here. The job model already evicts one overlay when the other loads and refuses a visualizer load meeting a decode in flight (`sim_bcm_job.c`, `bcm_viz ovl` / `bcm_decode ovl`).
- *For developers.* A game's overlay shares `0x12B000–0x133FFF` with DECODE and the visualizer: expect to be evicted, reload on the epoch, never assume it stays.

**5. DECODE and overlay loads block the UI pass (the counter stall)** (sitting 4c).
- *Sim against silicon.* The simulator's loads return at once; on the device the UI's pass waited on them. Owned by branch `p19-vizown` (model and script); referenced, not duplicated.
- *For developers.* A load or decode on the chip is not free to the thread that waits for it; never wait in the UI pass.

**6. A cache file cut mid-write: size kept, cluster chain short** (sitting 4c: panels.pak).
- *Sim against silicon.* The device's read-only fsck found panels.pak's chain ending after 2 clusters (32 KB) while its directory entry said 5,407,744 B, the rest of the chain free, 61 orphaned clusters, FSInfo's free count off by one: a write cut after the size was updated. Its header passed, so the builder wrote slots in place past the cut and every PP panel build failed (`art panel cpu ms=… ok=0`) pass after pass, and the panel decode fell back to the PP. The simulator's host files can't be in that state.
- *Model.* `fs_chaincut <path> <bytes>` (`uisimulator/common/filesystem-sim.c`, an upstream file: ledger row): the file keeps its size and `lseek`, a read or write starting past the chain's end fails with EIO, a read across it stops short, a write across it extends the chain (as `fat_readwrite` allocates); keyed by the host inode, so a rename keeps the damage and a new file does not. `fs_chaincut <path> check` logs whether it is still cut.
- *Fix.* `apps/ipodui/artcache.c` `panel_load()` reads the file's last byte (one seek and one read): a cut chain is recovered from `panels.pak.prev` (itself checked the same way) or the set starts over in a new file published over the cut one (`panel_fresh`: the private copy starts empty, never copied from the cut file, never written in place).
- *Test.* `tests/ui/ipodui-p19-panels-chaincut.script`: panels built, the chain cut at 32 KB, a pass: "chain cut: tail unreadable", then "set rebuilt", the published file whole, the panels valid, no `art retain panels`. Fails on the old builder (no detection: the cut file stays published).
- *For developers.* A file's size is not proof its data is on disk. Check the tail of any cache file you write in place, and publish rebuilt files through a temporary and a rename.

**7. The warm swap's flush: bit 0 still set, and a CPU upload's words not landing** (sittings 3 and 4c, B13; `quirks.md` #41).
- *Sim against silicon.* The host model has no warm swap (`PARK`, the cache control word at `0x1000_0C00`, the upload over a parked image); the simulator's `bcm_video_flush()` returns 0. On silicon the cold-start form of the flush took bit 0, still set by the park's own flush, as "done", and one swap in ten into the display-only image never became ready (the instruction side still held the parked image's lines).
- *Model.* Not added to `vbcm_model.c`: it would need a cache model of the core, not cheap. vc2sim models both: `bcmfw/tests/jobs_image_test.py` `t_park_unflushed` (a park without Apple's flush is refused: the unflushed stub ran) and `t_park_sticky` (bit 0 stuck at 1: our flush gives up after 15 re-requests, still parks). The host side's guard is `vplayer.c` `warm_flush()` (@25a's running-core form, `WARM_E_CACHE` after 15 stale reads).
- *Proposed.* (M) A `WARM_CACHE` word in `vbcm_model` with a sticky bit 0 and a stale-instruction flag on a swap whose flush was taken from a stale bit, so the warm lab runs in the host tests.

**8. Freezes and lost file writes** (sittings 4 and 4c).
- *Jump to 0* (`pp5022-reference/quirks.md` #56, BCM open question #75). The simulator cannot fault the same way: a NULL call faults in the host, it never spins with IRQs masked. Guard: `scripts/febe_link_check.py` over the linked `rockbox.elf` in the gate (a wrapped name with no real body is refused at build time), the hang record (`hang_ui MS`, `tests/ui/ipodui-hang-record.script`), and a bound and a FAULT line on every chip wait (`host-driver.md` §2a).
- *File-open failures* (sitting 4c: results-file lines lost). Owned by branches `p19-4c-fixes` and `p19-bench-sim`; referenced here, not duplicated.
