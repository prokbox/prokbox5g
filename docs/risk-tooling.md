# Pre-device risk-reduction tooling

Three scripts under `scripts/risk/` that reduce specific pre-device risks
without needing the iPod: a corrected CPU-time estimate, a sanitizer pass
for the alignment/UB bugs the simulator's x86/arm64 host hides, and a
stack-depth audit of the shell's threads. None of them touch `apps/`;
each reports findings for the orchestrator (or the relevant owner agent)
to act on.

## 1. ARM instruction counts (`scripts/risk/arm_insn_count.sh`)

Cross-compiles the UI shell's pure, host-testable modules
(`apps/ipodui/sortkey.c`, `scroll.c`, `library.c`'s sort/precompute core)
for ARM, measures real dynamic instruction counts, and converts to a
device-time range with a stated CPI/clock basis -- replacing the
project's single uncalibrated x400 CPU factor (`docs/process.md`,
`scripts/check_trace.py` `SIM_FACTOR`) with a per-workload, measured
estimate.

**Not covered (scope boundary):** `apps/ipodui/artkey.c`, `artfmt.c`, and
an image-downscale path do not exist on `main` yet (Phase 4b, in
progress on other branches as of 2026-10-06) -- `tests/artcache/` is
also not present. The tool is written so a new module needs only one
more driver file under `scripts/risk/drivers/` (host) and, optionally,
one more bare-metal harness under `scripts/risk/qemu-harness/` (ARM
dynamic count), following the pattern below; nothing else changes.

### Method

1. **Static sanity (regression signal).** `arm-none-eabi-gcc -mcpu=arm7tdmi
   -Os -ffreestanding` compiles the real `apps/ipodui/{sortkey,scroll}.c`
   unmodified (`-Ifirmware/{,export,drivers,include,kernel/include,libc/include}`
   resolve `<stdint.h>`/`<string.h>` from Rockbox's own minimal libc
   headers, `firmware/libc/include/`, rather than needing a newlib
   install -- Homebrew's `arm-none-eabi-gcc` formula ships the compiler
   only, no target libc). `library.c` additionally needs
   `tests/ipodui/libstub` (the same stub headers the host tests use) and
   one extra shim, `strings.h` (`strcasecmp`/`strlcpy`/etc., written by
   the script into `build/risk/arm-shim/`: `library.c`'s real
   `string-extra.h` pulls those in and the stub set doesn't declare
   them). `scripts/risk/lib/objdump_count.py` then disassembles each
   `.o` and reports each function's live static instruction count and
   its `bl`/`blx` call targets -- a regression signal: if
   `ui_sortkey`/`ui_scroll_step`/`cmp_rec`/`key_prefix`'s total has
   drifted far from the baseline recorded below, the per-call constants
   in `scripts/risk/lib/device_time.py` need re-deriving.

   Baseline (2026-10-06): `ui_sortkey` 226 insns (calls `memcpy`,
   `memmove`, has a loop), `ui_scroll_step` 36 insns (loop), `cmp_rec`
   104 insns (loop, calls `cmp_labels`/`cmp_track_no`/`eff_artist`/
   `label_of`/`tagcache_ui_seeks`), `key_prefix` 33 insns (calls
   `memset`, `ui_sortkey`).

2. **Realistic-scale host drivers** (`scripts/risk/drivers/*.c`,
   `scripts/risk/arm_insn_count.sh` step 2). Each compiles the *same*
   `apps/ipodui/*.c` with the host compiler and drives it at the
   planning scale from `docs/hardware.md` ("Planning scale": ~30-40k
   tracks, 3-4k albums -- this tool uses 40,000 / 4,000 / 1,250
   artists), measuring:
   - `sortkey_drv.c`: `ui_sortkey()` over deterministic
     track/album/artist-shaped names (same word-list shape as
     `tests/perf/synthetic_library.py`, reimplemented in C so the driver
     has no Python dependency) -- average input length (title 11.81,
     album 11.78, artist 11.65 chars: close enough across roles that one
     per-call cost covers all three) and the measured HASH_GROUP rate
     (names not starting with a letter: ~2.6-2.9%, not assumed).
   - `scroll_drv.c`: `ui_scroll_step()` over representative glides (a
     jump of 200-3200 px in a ~4,000-row offscreen strip, dt=1/tick --
     one 30 fps frame each).
   - `library_sort_drv.c`: `lib_open()` (the real, unmodified
     `apps/ipodui/library.c` + `apps/ipodui/sortkey.c`, driven exactly
     like `tests/ipodui/test_library.c` over
     `tests/ipodui/fake_tagcache.c`) at 40,000 tracks / 4,000 albums.
     **Finding:** the full, unfiltered Songs view does *no* `qsort` at
     all -- it walks the tagcache's already-title-sorted order
     (`fake_tagcache.c`: "the title file is walked in `ui_sortkey()`
     order"); only grouping views (Artists, Albums, Genres, Composers)
     call `qsort(cmp_rec)`. To measure the *real* comparator call count
     instead of assuming an N log N shape, this driver supplies its own
     `qsort()` (a plain quicksort+insertion-sort hybrid, linked ahead of
     the host libc's -- standard link-time interposition, no `--wrap`
     needed: library.c's call site passes `cmp_rec` as a function
     pointer, so the real, `static`, unmodified comparator is counted
     without ever being named outside `library.c`). Measured at 40k/4k:
     Artists 540 rows / 12,326 comparator calls, Albums 3,923 rows /
     100,859 calls. This replacement sort is a different algorithm from
     whatever the device's libc uses, so its *comparator count* is an
     estimate of the same O(N log N) order, not a bit-exact match.

3. **Dynamic ARM instruction counts** (`scripts/risk/arm_insn_count.sh`
   step 3, `scripts/risk/qemu-harness/`). A tiny bare-metal ARM program
   (`start.S` sets the stack pointer, calls `main()`, then issues ARM
   semihosting `SYS_EXIT`; `sortkey_main.c`/`scroll_main.c` call the
   real `ui_sortkey()`/`ui_scroll_step()` over a representative,
   hand-picked call set -- same length/HASH_GROUP shape as the host
   driver's measurements) is linked at address 0 (`link.ld`) and run to
   completion under two **independent** emulators, counting every
   instruction actually executed:

   - **qemu-system-arm** (`-M versatilepb -cpu arm926 -semihosting`) +
     a from-scratch TCG plugin, `scripts/risk/qemu-harness/libinsn.c`
     (~40 lines against the `qemu-plugin.h` Homebrew's `qemu` ships;
     registers a per-instruction callback in `vcpu_tb_trans` and prints
     the total on exit). No `qemu-arm` (Linux user-mode) exists via
     Homebrew on macOS -- only `qemu-system-arm` -- and no plugin `.so`
     ships prebuilt, so this harness builds its own.
   - **Unicorn** (`scripts/risk/lib/unicorn_count.py`, pip-installed into
     a local venv at `out/risk-venv/` -- gitignored, not system-wide):
     loads the same flat binary at address 0 under `UC_CPU_ARM_926` (no
     ARM7TDMI model exists in Unicorn either; same ARMv5TE substitution
     as the qemu `-cpu arm926`) and counts instructions with a
     `UC_HOOK_CODE` callback, stopping at the `svc` (Unicorn doesn't
     implement ARM semihosting, so emulation is bounded to end just
     before it instead of trapping cleanly).

   **Both agree almost exactly** (2026-10-06): sortkey 1,567,013
   (qemu) vs. 1,567,012 (Unicorn) instructions for 2,000 calls; scroll
   427,052 vs. 427,051 for 500 glides -- a 1-instruction difference in
   each case, exactly the final `svc` instruction itself (qemu's plugin
   counts it as executed before trapping; Unicorn's stop-before-address
   bound does not). This is strong cross-validation of the method with
   two independently-implemented emulators, not two configurations of
   one.

   Per call: **sortkey ≈ 783.5 insns/call** (2000-call representative
   mix, ~13 chars average), **scroll ≈ 854 insns/glide** (500-glide
   mix; this figure includes the test harness's own driving-loop
   overhead -- `scroll_main.c`'s `while (ui_scroll_step(...))` adds a
   handful of instructions per tick that the real animation driver in
   `apps/ipodui/ui_main.c` would not -- so it is a conservative, slightly
   high stand-in for `ui_scroll_step()` alone).

   **Caveats, stated rather than hidden:**
   - `ui_sortkey`'s `memcpy`/`memmove` calls resolve against a plain C
     byte-loop shim (`qemu-harness/libc_shim.c`), not Rockbox's real
     hand-optimized `firmware/asm/arm/{memcpy,memmove}.S`: those only
     assemble inside the full firmware build's macro environment
     (`ldmpc`, the `pull`/`push` shift macros are defined elsewhere in
     that build; a standalone `arm-none-eabi-as` on just the `.S` file
     fails with "bad instruction `ldmpc ...`" / "shift expression
     expected"). For the 1-4 byte copies `ui_sortkey` actually does,
     this is a conservative (not an undercount) substitution: hand-
     tracing the real `memcpy.S`'s short-copy tail (label `0:`-`8:`,
     every instruction counted regardless of its ARM condition code,
     since a false-condition instruction still costs a cycle on
     ARM7TDMI -- it's not a branch) gives ~11 instructions for a ≤3-byte
     copy, close to the shim's cost.
   - `apps/ipodui/library.c`'s `cmp_rec`/`key_prefix` are **not**
     measured this way: they call `tagcache_ui_seeks()`/`label_of()`
     (real tagcache dependencies, out of this tool's scope to stub for
     a bare-metal harness) and are `static` (no external linkage, so a
     driver can't call them directly either -- only through the real
     `lib_open()`, which needs the full `fake_tagcache.c` machinery,
     itself dependent on host `malloc`/`qsort`/etc. not available
     freestanding). These use the **hand-traced** static count from
     step 1 instead (`cmp_rec`'s prefix-mismatch fast path: 9-17
     instructions depending on whether the first 4 or all 8 prefix
     bytes differ, call it 13; `key_prefix`'s own wrapper: 33 instructions
     measured + ~8 assumed for a 16-byte `memset`), combined with the
     *measured* qsort comparator call counts above -- a weaker
     grounding than the qemu/Unicorn numbers, flagged in the report.

4. **Device time** (`scripts/risk/lib/device_time.py`). CPI range
   1.2-2.0: cited basis is the ARM7TDMI's 3-stage pipeline (most
   data-processing ops 1 cycle, loads/stores 2-3, branches ~3, no
   prediction) at the low end, SDRAM wait states and this code's branch
   density pushing it up at the high end; cross-checked against the
   project's own existing anchor (`apps/ipodui/library.c`'s header
   comment: ~70 MIPS at 80 MHz boosted on "cached code" ⇒ CPI ≈ 1.14,
   consistent with, and near the low end of, the stated range). Clock:
   30 MHz normal / 80 MHz boosted (`docs/hardware.md`).

### Results (2026-10-06, `scripts/risk/arm_insn_count.sh`)

| Workload | n | Instructions | Device time (CPI 1.2-2.0, 30-80 MHz) | Sim host time | Corrected factor | Old x400 estimate |
|---|---|---|---|---|---|---|
| `ui_sortkey` precompute, titles | 40,000 | 31.3M | 470 ms - 2.1 s | 4 ms | **133x-590x** | 1.6 s |
| `ui_sortkey` precompute, albums | 4,000 | 3.1M | 47 - 209 ms | 0.36 ms | **130x-580x** | 144 ms |
| `ui_sortkey` precompute, artists | 1,250 | 1.0M | 15 - 65 ms | 0.12 ms | **126x-563x** | 48 ms |
| `ui_scroll_step`, per glide (avg ~17 ticks) | 10,000 glides | 8.5M | 128 - 569 ms | 1 ms | **116x-513x** | 400 ms |
| `lib_open` Artists (sort only) | 540 rows, 12,326 cmp | 605K | 9 - 40 ms | 2 ms | 6x-27x | 800 µs |
| `lib_open` Albums (sort only) | 3,923 rows, 100,859 cmp | 4.5M | 68 - 303 ms | 3 ms | 20x-87x | 1.2 ms |
| `lib_open` Songs (full, unfiltered) | 40,000 rows | — (no `qsort`: walks pre-sorted order) | — | 0.84 ms | — | 336 ms |

The measured per-workload factor (**126x-590x** for the two
qemu/Unicorn-grounded modules) brackets the project's uncalibrated x400
reasonably well, within the stated CPI uncertainty -- x400 is not
wildly wrong for these two workloads, but it is a guess that happens to
fall inside a range this tool can now justify with a cited basis and
two independent measurements, rather than a number with no basis at
all. The `lib_open` sort figures (hand-traced `cmp_rec`, not
qemu-measured) show a lower factor (6x-87x): consistent with `cmp_rec`'s
fast path being only ~13 instructions on a core this much faster
per-instruction, but weaker-grounded -- flagged above, not smoothed over.
**Library list-open time is dominated by the `key_prefix` precompute
pass (one `ui_sortkey` call per row, same cost as the per-track
precompute above), not by the comparator** -- the existing budget-table
entry for "Library list open" should account for both.

### Reproducing / extending

`scripts/risk/arm_insn_count.sh` runs the whole pipeline from a clean
checkout (it creates `build/risk/` and, if missing, `out/risk-venv/`
with `pip install unicorn`; both gitignored). It never touches
`build/fw-ui` or `build/sim-ui`. Missing `arm-none-eabi-gcc`: `brew
install arm-none-eabi-gcc`. Missing `qemu-system-arm`: `brew install
qemu` (ships `qemu-system-arm` on macOS; there is no Homebrew
`qemu-arm` user-mode binary and no prebuilt TCG plugin, both expected by
earlier guidance for this task -- this tool builds around both gaps,
documented above, rather than installing anything further). To cover a
new pure module: add `scripts/risk/drivers/<name>_drv.c` (host, realistic
scale) and, for a real dynamic count, `scripts/risk/qemu-harness/<name>_main.c`
(bare-metal, same `start.S`/`link.ld`); wire both into
`scripts/risk/arm_insn_count.sh` next to the existing ones.

## 2. Alignment / UB sanitizer in the simulator (`scripts/risk/sim_ubsan.sh`)

Configures `build/sim-ui-ubsan` (`--target=22 --type=s --ipodui
--with-ubsan`, the existing configure option; GCCOPTS gets
`-fsanitize=undefined -fPIC`, and build forces SDL threads -- confirmed,
`configure.log`: "Using UBSan" / "Selected SDL threads"). The script also
appends `-fsanitize=alignment -fsanitize-recover=all` to the generated
Makefile (`-fsanitize=undefined`'s default set already includes
alignment checks; added explicitly per the brief, and
`-fsanitize-recover=all` so one run reports every finding in a script
instead of aborting at the first).

**Why this matters:** x86/arm64 hosts (the simulator's host) tolerate
misaligned loads/stores silently; ARM7TDMI does not -- an unaligned
access that works fine in `build/sim-ui` can fault or misbehave on the
real device. This tool exists to catch that class of bug before it
reaches hardware.

### Finding 1 (blocks the sim-script scan): `build/sim-ui-ubsan` does not build

`apps/ipodui/library.c:184`:
```c
extern uint64_t clock_gettime_nsec_np(int clock_id);
```
conflicts with this SDK's own declaration
(`.../usr/include/_time.h:185`, pulled in via `apps/status.h:43`'s
unconditional `#include <time.h>`, reached through `apps/tagcache.h` →
`apps/settings.h` → `apps/gui/statusbar.h` → `apps/status.h`):
```c
__uint64_t clock_gettime_nsec_np(clockid_t __clock_id);
```
`error: conflicting types for 'clock_gettime_nsec_np'` (gcc 16.2.0,
macOS 26 SDK, 2026-10-06).

**Confirmed independent of this tool's own `-fsanitize=alignment`
addition**: removing it and building with only the project's existing
`--with-ubsan` (`-fsanitize=undefined -fPIC`) reproduces the identical
error. **Confirmed specific to `-fsanitize=undefined -fPIC`, not
`-D_THREAD_SAFE`/SDL threads**: a plain (non-ubsan) `--ipodui`
`build/sim-ui`, configured with the *same* `-D_THREAD_SAFE` (sigaltstack
threads, not SDL, but the macro is defined either way on this build),
compiles `apps/ipodui/library.c` cleanly. Only adding
`-fsanitize=undefined -fPIC` reproduces the conflict -- not fully
root-caused (why PIC/sanitizer codegen flags change which declaration of
a *libc* function the preprocessor resolves is not obvious and wasn't
chased further under this task's time budget), but the reproduction is
exact and mechanical (toggle one flag pair, the error appears/disappears).

**Tried and rejected** (would "fix" it but breaks something else, so not
applied): globally defining `-D_POSIX_C_SOURCE=200809L` makes the SDK
skip that declaration (`_time.h:184`: `#if !defined(_POSIX_C_SOURCE) ||
defined(_DARWIN_C_SOURCE)`) -- but then `apps/gui/skin_engine.c:391`
fails with `implicit declaration of function 'strncasecmp'`, a *different*
Darwin/BSD extension hidden by the same feature-test macro. There is no
single global define that satisfies both files.

**This is an `apps/` fix, out of this tool's scope** (CLAUDE.md: never
edit `apps/`). Two options for whoever owns `apps/ipodui/library.c`:
(a) gate the `extern` on a feature-test macro or SDK-version check so it
only appears where the SDK doesn't already declare it, or (b) replace
the hand-written `extern` with `#include <time.h>` directly and resolve
whatever it originally clashed with -- the comment above it
(`library.c:178`) says "SDL.h and `<time.h>` clash with the apps
headers" but doesn't say which symbol, so that original clash should be
re-diagnosed on the current toolchain rather than assumed unchanged.

**Consequence:** the `tests/ui/ipodui-*.script` set (the sim-script scan
this tool is primarily for) cannot run at all until this is fixed --
`apps/ipodui/ui_main.c`, `anim.c`, `paint.c`, `header.c`, `menu.c`,
`qscroll.c`, `theme.c`, `input.c`, and every `apps/ipodui/screens/*.c`
file are **not sanitizer-tested** by this pass. This is the main
limitation of this tool's results, not a clean bill of health for the
shell as a whole.

### Finding 2-6: host tests -- clean

The five existing `tests/ipodui/` host test binaries, rebuilt (by
`scripts/risk/sim_ubsan.sh`, independently of `tests/ipodui/run.sh` --
which `scripts/test.sh` also uses with no sanitizer and must stay
unaffected) with `-fsanitize=alignment,undefined -fsanitize-recover=all`,
all pass with **no sanitizer findings**:

| Binary | Sources | Checks | Result |
|---|---|---|---|
| `test_scroll` | `apps/ipodui/scroll.c` | 86,078 | clean |
| `test_bands` | `apps/ipodui/screen_stack.c` | 20 | clean |
| `test_sortkey` | `apps/ipodui/sortkey.c` | 7,099 | clean |
| `test_sortkey_version` | `apps/ipodui/sortkey.c` | 1 (corpus hash) | clean |
| `test_library` | `apps/ipodui/library.c` + `sortkey.c` + `tests/ipodui/fake_tagcache.c` | (classification/grouping/progress/stale/cap suites) | clean |

`tests/artcache/` does not exist yet (Phase 4b, in progress on other
branches as of 2026-10-06): `scripts/risk/sim_ubsan.sh` checks for
`tests/artcache/run.sh` each run and reports (not silently skips) that
its own build step needs a matching `build_and_run` call added once that
test harness exists.

**Net result:** the pure modules that compile standalone (sortkey,
scroll, screen_stack, library's sort/grouping core) show no
alignment/UB bugs. The integrated shell (everything in Finding 1's list)
is unverified by this pass and should be re-run once `library.c:184` is
fixed.

## 3. Stack usage (`scripts/risk/stack_usage.sh`)

Builds `fw-ui` into a **scratch** directory, `build/risk-stack-fw` (never
`build/fw-ui`: `--target=22 --type=n --ipodui
--compiler-prefix=arm-none-eabi-`), with `-fstack-usage
-fcallgraph-info=su,da` appended to the generated Makefile's GCCOPTS.
gcc then writes one `.ci` file per translation unit: a small VCG graph
with every function's own worst-case stack frame size (`N bytes
(static|dynamic)`) and its direct call edges. `scripts/risk/lib/
stack_depth.py` parses every `.ci` under the build dir into one combined
call graph and, from each thread's entry function, finds the deepest
(highest total stack bytes) call chain by a memoized longest-path search.

**Entry points and their declared stacks** (`DEFAULT_STACK_SIZE` = 0x400
= 1024 B for this target, `firmware/asm/arm/thread-classic.h`'s
`PLATFORM_NATIVE` branch; each `create_thread()` call site grepped
directly, not copied from memory):

| Thread | Entry function | Stack size | Source |
|---|---|---|---|
| main/UI (apps/ipodui runs on it directly -- `apps/main.c` calls `ipodui_main()`, no separate `create_thread()` for the shell) | `ipodui_main` | 8,192 B (0x2000) | `firmware/target/arm/pp/app-pp.lds` `.stack` section (IRAM) |
| tagcache (precompute/import/commit) | `tagcache_thread` | 17,408 B | `apps/tagcache.c:6797` |
| audio | `audio_thread` | 5,120 B | `apps/audio_thread.c:168` |
| codec | `codec_thread` | 9,216 B | `apps/codec_thread.c:662` (`WORKAROUND_FS13060` is 0 on this build) |
| buffering | `buffering_thread` | 9,216 B | `apps/buffering.c:1684` |
| playlist | `dc_thread_playlist` | 3,072 B | `apps/playlist.c:2006` |
| piezo (haptic clicker) | `piezo_thread` | 1,024 B | `firmware/target/arm/ipod/piezo.c:190` |
| art builder | — | — | **not present on `main` yet** (Phase 4b, in progress on other branches as of 2026-10-06); `scripts/risk/stack_usage.sh` has a one-line slot to add it once `apps/ipodui/art/*.c` creates the thread |

### Results (2026-10-06)

Every thread's deepest discovered path ends the same way: a blocking
storage call (`ata_write_sectors`/`wait_for_bsy`) reaches `yield()` ->
`switch_thread()` -> the kernel's own stack-overflow check,
`thread_stkov()` -> `thread_panicf()` -> `format_thread_name()` ->
`snprintf()` -> `vuprintf()` -> `format_double_radix()` (400 bytes,
reachable from essentially any blocking call in the firmware, not
specific to any one thread's own logic). `scripts/risk/lib/
stack_depth.py` reports this once, separately, and also with it folded
into each thread's total (its longest-path search finds it on its own,
since the search isn't told to avoid it):

| Thread | Worst-case path | Stack size | **% used** |
|---|---|---|---|
| **audio** | `audio_thread` → `audio_playback_handler`(528) → ... → `playlist_next` → `create_and_play_dir` → `check_subdir_for_music` → `action_userabort` → **`default_event_handler` → `default_event_handler_ex` → `gui_usb_screen_run`**(168) → `playlist_resume` → ... → `skin_data_load`(848) → `font_load_ex` → `glyph_cache_load`(2088) → file/FAT/ATA chain → `yield` → panic tail | 7,968 B | **155.6%** |
| playlist | `dc_thread_playlist` → `get_track_filename`(576) → `convert_m3u_name` → ... → FAT/ATA chain → `yield` → panic tail | 2,448 B | **79.7%** |
| main/UI | `ipodui_main` → `ui_input_get` → `default_event_handler` → ... → `gui_usb_screen_run` → ... → `skin_get_gwps` → `skin_data_load`(848) → `font_load_ex` → `glyph_cache_load`(2088) → file/FAT/ATA chain → `yield` → panic tail | 6,004 B | **73.3%** |
| piezo | `piezo_thread` → `usb_wait_for_disconnect` → `queue_wait` → `block_thread_` → `switch_thread`(panic tail, 400 of the 592) | 592 B | 57.8% |
| tagcache | `tagcache_thread` → `tagcache_build` → `commit`(344) → `build_index`(**6,424**) → `remove_files` → `open_db_fd` → ... → FAT/ATA chain → `yield` → panic tail | 9,504 B | 54.6% |
| codec | `codec_thread` → `codec_load_file` → `lc_open` → file/FAT/ATA chain → `yield` → panic tail | 2,064 B | 22.4% |
| buffering | `buffering_thread` → `fill_buffer` → `buffer_handle` → `get_metadata_ex`/`afmt` → file/FAT/ATA chain → `yield` → panic tail | 1,872 B | 20.3% |

**Flagged (over ~70%): audio (155.6%), playlist (79.7%), main/UI
(73.3%).**

### Audio thread: needs verification, not a confirmed bug

The 155.6% figure is driven by one specific edge: `check_subdir_for_music`
(building a playlist from a directory, called from the **audio** thread
via `playlist_next` while loading the next track) calls
`action_userabort()`, which the callgraph shows calling
`default_event_handler()` -> `default_event_handler_ex()` ->
`gui_usb_screen_run()` -- the **full USB-connected-screen UI**, including
`skin_data_load()`/`font_load_ex()`/`glyph_cache_load()` (2,088 B alone).
If that edge is real (the audio thread really can end up running the USB
screen's UI, synchronously, while already deep in track-loading code),
this is a genuine, serious stack-overflow risk on the real device, where
`build/risk-stack-fw`'s own `.ci` data is doing the measuring, not a
sim-only artifact. If it is not real in practice, it is exactly the kind
of false edge a *static* callgraph is known to produce by over-
approximating through a generic, widely-shared utility function
(`action_userabort`'s job is "has the user pressed a button to cancel",
called from many contexts; whatever it calls to check that may fan out,
in the whole-program callgraph, to every possible event handler
reachable from *any* caller, not just the paths actually live when it is
called from the audio thread). **This needs the owner or
playback-audiobook to confirm by reading `action_userabort()` and
`default_event_handler()`'s real control flow** -- this tool reports the
exact edge chain (above, and in full via `scripts/risk/stack_usage.sh`'s
output) for that; it does not and cannot resolve the ambiguity itself
from a static graph alone.

### Method notes / limitations (stated, not hidden)

- **Direct calls only.** gcc's `-fcallgraph-info` records direct
  (`bl`-style) calls, not calls through a function pointer (event
  dispatch tables, callback-based codec/plugin APIs). A thread that
  reaches deep code *only* through one is undercounted. This is also
  `scripts/risk/lib/objdump_count.py` (risk tool 1)'s limitation, for
  the same reason.
- **Static-name collisions.** Two unrelated `static` functions sharing a
  name in different files are disambiguated by gcc in the `.ci` data
  (`"<abspath>:<name>"`); `stack_depth.py` resolves a plain entry name
  to that form automatically, and warns if more than one file defines a
  function by that name (seen once: two different `audio_thread`
  functions exist in the tree -- `apps/audio_thread.c`'s is the one
  used here; the result for any *other* colliding name should be
  spot-checked against the printed path before trusting it).
  Mid-chain call edges are matched the same way, so a path through a
  common static helper name could in principle jump to the wrong
  file's version of it; not observed in the paths above (every
  mid-chain static function printed resolved to a plausible,
  domain-matching file), but not exhaustively checked either.
- **gcc's own per-function number is itself a worst case** (every local
  array/spill slot the function *could* use, not the one path taken),
  so this tool's total is a bound on a bound -- conservative, not a
  measured number, unlike tool 1's qemu/Unicorn figures.
- **Recursion** is detected (a cycle in the call graph) and reported as
  "N other recursive function(s) reachable from here" rather than
  silently unrolled or treated as infinite; none of the three flagged
  threads' *own* worst-case paths above pass through a cycle, so the
  numbers in the table are not affected by the recursion-handling choice
  here, but the full `stack_usage.sh` output lists which functions
  recurse for anyone who wants to check further.
