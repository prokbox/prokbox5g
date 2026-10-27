# Gate speed-up: brief and implementation plan

Status: **Phase 1 (analysis and plan).** No tooling has changed yet.
Branch `gate-speed` (from `p12`). Owner of the work: the `tooling` agent. Written 2026-10-23.

## 1. Goal

Bring the **full gate** (`scripts/gate-run --full`, i.e. plain `validate.sh`) from
about 2.5 to 4 hours to **about 1 to 1.5 hours**, with:

- the **same coverage**: every `tests/ui/*.script` still runs in a full gate, same checks, same goldens;
- **less flakiness**, so the extra parallelism is safe;
- `GATE PASSED` printed only on a real pass. Any reduced mode is **named** (a flag, printed in the log header) and never becomes the default.

"One full gate per integration" (docs/process.md) stays the process; this makes that one gate cheap enough to re-run after a fix round.

## 2. Where the time goes (measured)

Logs carry no timestamps, so the breakdown comes from file birth/mtime times in the worktrees (read-only). Two full gates were reconstructed.

### 2.1 `gate-p18-2` (worktree `p18-114`, 355 UI scripts, 4 workers; a full gate, but see the build note)

| Phase | Start | End | Minutes | Source of the time |
|---|---|---|---|---|
| builds | 11:04:26 | 11:04:37 | 0.2 | **the 5 firmware builds had failed**; a healthy run costs about 3 min warm (see 2.2) |
| fonts, warnings, host tests, trace self-tests, bcmfw benches, priming, clones | 11:04:37 | 11:17:05 | 12.5 | host tests about 5 min serial, priming about 5 min on a cache miss |
| **UI scripts**, 355, 4 workers | 11:17:05 | 12:09:47 | **52.7** | `status-*` file mtimes |
| size, boot identity, fw-rel, string check | 12:09:47 | about 12:10:50 | about 1 | |
| **UBSan pass** (all ipodui scripts on `sim-ui-ubsan`, 4 workers, then ubsan host tests) | 12:10:52 | 13:22:56 | **72** | `out/shots-ubsan/*/sim.log` mtimes; 357 script dirs, so it runs **every** script, not about 140 |
| **floor gate** (32 + 64 MB, 50k library) | 13:22:56 | 13:36:49 | **13.9** | `build/floor-gate.log` mtime; includes the `sim-ui32` build |
| upstream dry-run, end | | about 13:37 | about 1 | |
| **Total** | 11:04:26 | about 13:37 | **about 153** | |

That gate's builds failed in 11 seconds (and the gate **kept going for 2.5 more hours**; see section 3, item 8), so a real gate is 153 min plus build time. A run that
includes a cold ccache, a retry round or a re-run lands in the owner's 3.5 to 4 h; those extras are not in these files and must be captured by the Wave 0 timing log
(5.1) before the plan's numbers are trusted.

### 2.2 `gate-p12` (the one running now, worktree `p12`, 392 UI scripts, 4 workers; all times 2026-10-23)

| Phase | Start | End | Minutes |
|---|---|---|---|
| builds (8 dirs concurrently, `-j2` each, warm ccache) | 19:23:48 | 19:25:58 | 2.2 |
| compile-only `fw-ui-video`, `fw-alt`, `fw-bcmfw` (serial, after) | 19:25:58 | 19:26:38 | 0.7 |
| host tests (`tests/*/run.sh`, serial; `fatcrash` alone is about 3 min: 19:27:40 to 19:30:32) | about 19:27 | about 19:32 | about 5 |
| bcmfw benches | about 19:32 | about 19:33 | about 1 |
| priming (cache miss, inputs changed) | about 19:33 | about 19:38 | about 5 |
| UI pass | about 19:38 | running | modelled 55 to 60 (below) |

Per-script wall times of the first 45 finished scripts (consecutive `status-*` mtimes on the same worker): **mean overhead 10.9 s over the scripted time, median 7 s**.
Scripts with a `fresh-db` boot or a library rebuild cost 25 to 65 s over their script (for example `ipodui-idle-charger` 64 s, `ipodui-h1-pre-dirty` 31 s).

### 2.3 What the scripts themselves contain (parsed from `tests/ui/*.script`)

- 408 `ipodui-*` plus 3 stock scripts; 18 are skipped by `PENDING` / `# pending:` / `# needs: modules`, 1 is `fonts` (silently skipped), so **392 run**.
- 4,682 `wait` lines totalling **8,642 s = 144 min** (matches the owner's figure). Plus `key` 467 s and `wheel` 744 s of built-in press time. **Scripted time: 157 min** across the 392 running scripts.
- Waits of 5 s or more total **52 min** (of the 157). The other 92 min of waiting is 4,600 short "settle" waits (`wait 400` to `wait 3000` after a key or wheel).
- 211 scripts declare `# timeout:` (sum 39,760 s, a ceiling not a cost).
- Heaviest: `p18a-wakes` 400 s, `p16-panels-stopped` 198 s, `p18a-wakes-dark` 187 s, `p5-refill-dark` 145 s, `p16-bootbatt-*` 115 to 129 s, `hold-np` 101 s, `art-chap-retry` 101 s.
- Implicit event waits: 370 `wait_db`, 203 `art_wait`, 104 `update_db_wait_change`, 90 `wait_pos`.

### 2.4 Model check

Static round-robin (what `run_worker` does), 4 workers, per-script cost = scripted time + 6.5 s: **52 min**. Measured on `gate-p18-2` (355 scripts): 52.7 min. The model is good; use it for the numbers below.

Machine facts that matter (read from `ps`, `top`, `sysctl`; 8 cores, 8 GB):

- Each simulator is **about 17 MB resident and 0.1 to 0.4 % CPU** while waiting. Four workers leave the machine **61 % idle**. The 4-worker limit is a flake limit, not a CPU or memory limit.
- `validate.sh` already defaults to `min(16, 2*ncpu, mem GiB)` = 8 workers; the logs show `GATE_JOBS=4` because the handoff notes recommend turning it down (flakes).
- Background load that is not ours: a Virtualization.framework VM at about 105 % CPU for 5 days, `fseventsd` at about 80 % (we create and clone thousands of files under `build/` and `out/`), `mds_stores` at about 23 % (Spotlight indexing them). Load average is about 6 with the sims nearly idle.
- Disk: 9.5 GB free on `/` (96 % used). RAM: 7.4 GB used, 2.5 GB compressed.
- ccache (`~/.cache/rockbox-ccache`): **full** (2.2 of 2.2 GB, 2,234 cleanups), 84 % hit rate, 63 % of hits only in slower "preprocessed" mode. `base_dir` is unset and `hash_dir` is on, so objects built in one worktree path miss in another when debug info is on.
- `GATE_MAX_MINUTES` defaults to **75**; every full gate is longer than that, so the orchestrator must already be overriding it. Fix the default (Q10).

### 2.5 Share of the 153 minutes

UBSan 47 %, UI scripts 34 %, pre-UI work 8 %, floor 9 %, rest 2 %. **81 % of the gate is simulator scripts that are almost all idle waiting**, run at 4-way on an 8-core machine.

## 3. Findings that shape the plan

1. **Waiting, not computing.** Scripted time is 157 min of the 200 worker-minutes in a UI pass. The sims use about 0.2 % CPU. More workers is the biggest lever.
2. **Static partitioning.** `run_worker` assigns job `i` to worker `((i-1) % N) + 1` in alphabetical order. A worker that draws several 100 to 400 s scripts finishes minutes after the others; the modelled 4-worker makespan is 52 min static vs 50 min LPT, but at 8 workers it is 30 vs 25 and at 12 it is 23 vs 17. The tail grows as workers are added.
3. **UBSan is serial after the UI pass, a report only ("never fails the gate"), and re-runs every script's full waits** at 1.36x the UI pass's cost (72 vs 52.7 min). Its report is long-standing noise (293 findings in `gate-p18-2`, including a host-test compile failure), so a new finding is easy to miss.
4. **The floor gate** is 14 min, of which the sims use very little CPU; it runs 32 MB and 64 MB one after the other, and builds `sim-ui32` late. It is deterministic given the sources.
5. **The `sim-ui-ubsan` and `sim-ui32` builds happen late and serially** (inside `sim_ubsan.sh` and the floor step), instead of with the other builds.
6. **Host tests run serially** (`scripts/test.sh` loops `tests/*/run.sh`); `fatcrash` is 3 of the 5 minutes and is first in the list only by luck of ordering.
7. **Flake retries are serial and late.** After the parallel phase, each failed script is re-run alone, one by one (p16-final: 10 scripts). 5 to 6 "flaky-retry ok" per gate in P16; the cause was mostly the frame-interval rule and wall-clock windows under load.
8. **A red build does not stop the gate** (`gate-p18-2`: five builds failed, then 2.5 h of simulator work ran for a result that could only be `GATE FAILED`).
9. **No timing data is kept**, so the breakdown above had to be reconstructed from file times.

## 4. How the frame-interval rule works today (idea 2)

`apps/ipodui/anim.c` `ui_anim_frame()` logs `anim frame <ms>` where `<ms>` is `(current_tick - last_frame) * 10`. In the **simulator, `current_tick` is derived from the wall clock**:
`firmware/target/hosted/sdl/kernel-sdl.c:165`, `new_tick = (SDL_GetTicks() - start_tick) / (1000/HZ)`, run from an SDL timer thread. So the "tick" interval **is** a wall-clock interval, 10 ms resolution, and any host
scheduling stall (VM, Spotlight, other sims, a build) lengthens it. `scripts/check_trace.py` (the `frame` rule, lines 3018 to 3025; `frame_intervals()` at 2274) takes the mean over all frames after the first of each animation segment and fails above `--max-frame-ms 50` ("sim sanity only"; the frame period is 40 ms).
Examples in `gate-p16-final`: `mean frame interval 82.9 ms > 50 ms (28 frames) [ipodui-p6-book-end]`, 62.7, 60.5, 57.1 ms in three other scripts.

So "measure in ticks, not wall-clock" does not help by itself: in the sim they are the same clock. The options that keep the smoothness signal:

- **A. CPU time per frame (preferred).** Add a sim-only field: `anim frame <ms> cpu=<us>` where `cpu` is the UI thread's CPU time (`CLOCK_THREAD_CPUTIME_ID`, wrapper already in `apps/ipodui/simclock.c`) between the previous frame and this one. The rule becomes two checks: (1) **mean CPU per frame** stays under a budget (calibrated from today's passing runs, then multiplied by the existing `SIM_FACTOR` logic used for `libopen`), which fails if frames get more expensive, load or not; (2) the wall mean stays a hard FAIL **only when the CPU check is also near its budget**; otherwise it is reported as `starved` (a note plus a flake-ledger entry), because a frame that costs little CPU but arrives late was held up by the host, not by our code. `UPDATE_TRACE` compiles away on target, so this costs nothing on the device. **Needs an `apps/` hook: not ours; the orchestrator assigns it (6.3).**
- **B. Host jitter watcher (no `apps/` change).** New `uisimulator/common/sim_cmd_load.c`: a host thread sleeps 10 ms in a loop and logs the overshoot (`sim_load: jitter mean=.. p95=.. max=.. ms`) every second and at exit. `check_trace.py` then classifies a frame-rule failure in a run with high p95 jitter as `starved`, re-runs it at once on a fresh clone, and records it. Deterministic classification, no semantic change to the rule, but the failing run still has to be repeated. Do B first (tooling-only), A when the hook is assigned.
- **C. Lock-step simulated time** (the sim advances `current_tick` only when all threads are blocked, so frames cost zero virtual time) would be fully deterministic but replaces the simulator's timing model; far too invasive and it hides real cost. Not recommended.

Neither A nor B weakens the check: frames that are too expensive still fail.

## 5. Ideas evaluated

Savings are wall-clock minutes on a full gate, computed from the model in 2.4 (200 worker-min UI pass, about 290 worker-min UBSan, about 20 floor) unless noted. "Cost" is tooling effort.

### Idea 1. Simulated-time fast-forward

What exists: `sleep_warp <s>` and `rtc_set`/`rtc_at` (`sim_cmd_sleep.c`) move **only the RTC offset and the pinned sim clock** (`sim_rtc_offset_s`, `sim_clock_advance`); they do **not** advance `current_tick`, so tick-based timers (backlight, idle power-off, poll threads, battery drain, art retry timers) do not fire. 20 `sleep_warp` uses and a handful of `rtc_*` uses exist, all in the P18 sleep and P12 alarm scripts.
`wait` is `sleep_ms()` = kernel `sleep()` in ticks, so it is real time.

Inventory of the 52 min of waits of 5 s or more (read from the script headers of the 40 heaviest):

| Class | Examples | Minutes | Can fast-forward? |
|---|---|---|---|
| **A. Tick/thread-timer-bound, nothing playing** | `idle-poweroff` 80 s, `p5-export-charger-on/off` 2 x 69 s, `p16-panels-stopped` 195 s, `p16-bootbatt-*` 115 to 124 s, `art-chap-retry` 100 s, `p18a-wakes` stopped and paused phases, `p5-refill-dark` idle part | about 25 | yes, with a tick skew (below) |
| **B. RTC-bound** | `p8a-unlit-minute` 62 s (waits for the host's next minute), `p12-alarm-*`, `p12-notes-owed` | about 3 | yes today: `rtc_at 12:00:57` puts the minute boundary 3 s away |
| **C. Audio-bound** (position follows the PCM callback, real time on the dummy driver) | `p5-refill-dark` 2 to 5 s tracks, `p5-book-alone`, `p18-stor-refills`, `idle-done` playing part, `fit3a-sets`, `p17-usb-nowrite`, anything with `wait_pos` | about 20 | **no**: keep real time |
| **D. Event waits** (`wait_db`, `art_wait`, `update_db_wait_change`) | all | n/a | already return early; nothing to gain |
| **E. Short settle waits** (92 min) | `wait 800` after a key | 92 | not by warping (animations are frame-paced); see idea 9 |

Proposal (not just `sleep_warp`):

- **`time_scale <K>` script command** (new `uisimulator/common/sim_cmd_time.c`), a virtual-time multiplier applied at the **single** sim time source (`kernel-sdl.c:165` and the sub-tick read at `thread-sdl.c:237`) and to the RTC (`time(NULL)` in `sim_cmd_sleep.c`). `wait 130000` then means 130 s of *virtual* time. The timer thread keeps firing at 10 ms real and the tick catches up in steps, so every tick-waiting thread still sees its sleeps expire in order and **wake counts per virtual minute are preserved** (this matters for `p18a-wakes`, which asserts "no polling": a one-shot jump of 130 s would make it pass vacuously, which is why `tick_skip` as a single jump is rejected). Use K up to 10, scoped: `time_scale 10 / wait 130000 / time_scale 1`, only around class A waits with no playback.
- Pre-flight audit done: host-clock reads in the sim are few (`sim_cmd_scan.c`, `sim_cmd_ab.c`, `sim_video.c`, `sim_cmd_floor.c`: measurements only; `sim_cmd_sleep.c`: RTC). `apps/ipodui/simclock.c` reads the host clock for dev timing; the `art retry in=60s` and bootbatt timers need checking for `clock_gettime` vs tick (Wave 3 task).
- Savings: class A at K=10 saves about 22 worker-min per pass; **about 6 min wall for the UI and UBSan passes together at 8 workers** (and the 130 s wake phases stop being the critical path). Class B: about 3 worker-min, free today.
- Risk: medium. It touches `firmware/target/hosted/sdl/` (not tooling's tree) and changes timing semantics for the scripts converted. Each converted script must run with K=1 and K=10 and give identical `log:`/`nolog:` verdicts and identical goldens (verification 8.3). Never applied to a script with playback.
- Cost: kernel hook small (about 20 lines, orchestrator-assigned); tooling: command, 15 to 25 script conversions, A/B harness.
- **Verdict: worth doing, but late (Wave 3).** It is not the main lever (6 of about 80 saved minutes); do it only if Waves 1 and 2 leave the gate above 1.5 h.

### Idea 2. Frame-interval checks that survive load

See section 4. Savings: indirect, it is what makes 8 to 12 workers safe. Cost: B small (new `sim_cmd_load.c`, `check_trace.py` classification, `tests/trace` cases), A small plus an `apps/` hook. Risk: low if the CPU budget is calibrated from existing passes and a negative-control trace (an artificially slow render) proves the rule still fails.

### Idea 3. UBSan pass

Facts: report only, never fails the gate; serial after the UI pass; all 355 to 392 scripts (not about 140; to be checked on `build/risk/ubsan/gate-findings.txt` after the running gate); 72 min; its build happens inside it.
Options:

- **3a. (default) Keep it in the full gate with full coverage, but run it concurrently** in the same worker pool as the UI scripts and the floor jobs (Wave 1). Savings come from the larger shared pool, not from skipping. Same coverage, same report.
- **3b. Named reduced mode** `--ubsan=mapped`: scripts that `gate_map.txt` maps from changed files, plus a full run scheduled weekly or at pre-release (`scripts/risk/sim_ubsan.sh` already takes `UBSAN_LIST`; `--quick --changed` already does this). Saves about 60 min of pool time per gate. **Never the default of the full gate**; offered because the pass cannot fail the gate.
- **3c. Baseline diff.** `gate-findings.txt` is compared with a committed `tests/risk/ubsan.baseline` and the gate prints only *new* findings (still not failing, unless the owner chooses). Makes the report readable; removes the 293-line noise and the standing `test_library` compile finding (needs the `library.c:184` fix, an `apps/` item).
- **3d. Single-pass sanitizer** (run each script once on the UBSan sim and use that run for goldens, trace and UBSan): would delete the whole separate UI pass (200 worker-min), but then the shipped-flags simulator is no longer what the goldens and timing rules run against. Named option only; recommend **no** (Q5).

### Idea 4. Floor gate

Inputs that can change the result: `apps/`, `firmware/`, `lib/`, `uisimulator/`, `tools/configure`, `tests/perf/`, `tests/ui/setup_*` (the 50k generators). Proposed:

- Key = hash of `git ls-files -s` plus the working-tree diff over those paths, plus the two configure arg lines. Store the receipts (`floor ...`, `floor gate: ok ...`, `floor_watch all_cases`) in `~/.cache/rockbox-gate/floor-<key>/`.
- **`FLOOR_REUSE=1` reuses a receipt for an identical key** and prints `floor: reused <key> measured <date> in <worktree>`; `FLOOR_FRESH=1` forces a run. Default for `--quick --changed` (already skips unless those paths changed) and for the full gate: **measure** (Q4). Reuse pays only in re-gates after tests/docs-only fixes.
- Independent of reuse: run the 32 MB and 64 MB halves **in parallel**, move the `sim-ui32` build into the build phase, and schedule the floor jobs in the shared pool. Wall cost goes from 14 min to about 3 min of the critical path (about 7 worker-min). Saves about 11 min.
- Risk: none for the parallel part (each half uses its own build and disposable clone of `build/sim-50k`); reuse has the usual stale-key risk, mitigated by hashing the sources not the binary (the binary embeds a build date).

### Idea 5. Build parallelism and caching

- Builds already run concurrently (8 dirs, `JOBS` = (ncpu+2)/n, min 2 each), then three compile-only builds run serially. Warm: 2.9 min total. No build is redundant: `fw` (stock byte-identity), `fw-ui`, `fw-rel` (sealed release, string check), `boot`/`boot-ui` (stock boot identity), `sim` (stock smoke), `sim-ui`, `sim-ui-video` (p14a), `fw-ui-video`/`fw-alt`/`fw-bcmfw` (variant matrix). Dropping any loses a check.
- Fold the three compile-only builds, `sim-ui-ubsan` and `sim-ui32` **into the one concurrent build phase** with a fair share of jobs (they are now built at the end, so a stale build of either lands on the critical path). Savings: about 0.7 min warm, **unmeasured but several minutes cold** for the UBSan and 32 MB sims (the first measurement is a Wave 0 task). Also fixes the memory concern in the comment ("so the memory peak doesn't grow"): 11 dirs at `-j2` is 22 compilers, versus 16 today plus 8 jobs of `-j8` after.
- ccache (owner/system setting, outside the repo): the shared cache is full and evicting. Set `base_dir` to the repo root and `hash_dir = false` so worktrees share objects; keep `max_size` but prune old worktrees (only 9.5 GB free). Expected effect on a cold-header gate: large (the 12 build trees are rebuilt in each worktree). Not measurable here without a build; Wave 0 logs per-build wall times and ccache hit/miss deltas (`ccache -s` before and after, read-only).
- `fail-fast`: abort when any build fails (see section 3 item 8 and Q3). Saves 100+ min on a red build.

### Idea 6. Host tests

`scripts/test.sh` runs 55 `tests/*/run.sh` one after the other; about 5 min, `fatcrash` about 3 min. Plan:

- Run suites in parallel (`xargs -P4`), longest first (`fatcrash` first), each with its output captured and printed in name order, and the same `ALL TESTS PASSED` / `TESTS FAILED` contract. Suites already use their own `build/<name>` dirs; those that share `build/warble` only read it after the single `make`. An audit step in Wave 1 checks each `run.sh` for shared writable paths.
- Cache the fatcrash images: it rebuilds `fsreplay` each run (a few seconds) and replays hundreds of crash points per seed (compute-bound, the 3 min). The images are deterministic given the FAT code, so key a `build/fatcrash/.stamp` on the hash of `firmware/common/{fat,file,dir,disk_cache,...}.c`, the shims and `fsreplay.c`; skip the *positive* replays when the key is unchanged **but always run the negative controls** (they prove the test has teeth). Do not skip anything in a full gate by default; offer `FATCRASH_CACHE=1` (a named mode), and instead split the seeds across 4 processes for the default (about 3 min to about 1 min).
- Run host tests **during** the build phase and priming rather than after (they need only `warble`, built first). Savings: about 3 min (parallel) plus overlap with priming, together **about 7 min** of the 12.5 pre-UI minutes.

### Idea 7. Smarter scheduling

- Replace `run_worker`'s static `i % N` with a **dynamic queue**: a job list ordered **longest first** (estimated from a committed `tests/ui/durations.tsv`, regenerated by Wave 0's timing log; initial estimate = scripted time plus 8 s) fed to `xargs -P N`. Each job already gets its own fresh clone (`clone_disk` per script), so nothing is tied to a worker number; the per-worker `simdisk.w$N` dirs go away. Savings: the gap between static and LPT in 2.4: 2 min at 4 workers, 5 min at 8, 6 min at 12.
- Priming is already done once and cached across worktrees (`simdisk_cache_key`); clones are `cp -c` of a 24 MB tree. Keep. Measure the per-script clone cost in Wave 0; if it exceeds 1 s, clone once per worker and reset only the state files.
- One shared pool for **UI, UBSan and floor jobs** with a global limit, so idle cores are always used and UBSan's tail overlaps the UI scripts.
- `fseventsd`/Spotlight: have the gate create `.metadata_never_index` markers (harmless) and ask the owner to add `build/` and `out/` to Spotlight's privacy list and Time Machine's exclusions (Q6). Expected: frees about one core and reduces jitter.

### Idea 8. Flake handling

- **Cheaper:** retry a failing script **immediately, in the pool, on a fresh clone**, not after the whole parallel phase, and run the retries concurrently instead of one by one. A failure that repeats is a real FAIL as today.
- **Clearer:** write every first-attempt failure to a persistent ledger `~/.cache/rockbox-gate/flakes.tsv` (date, commit, script, rule, value, jitter p95, attempt result). The end-of-gate summary prints `GATE PASSED (N flaky retries: names)` in the same line, so a pass with flakes is never silent; a script that flakes in 2 of the last 5 gates is listed under "needs a fix".
- **Rarer:** the frame-rule change (section 4), then convert chronic offenders from fixed waits to condition waits (new `wait_log <regex> <ms>`, a sim command in `sim_cmd_*.c` that blocks until the sim logs a matching line, with a timeout that fails loudly "wait_log failed: pattern <p> after <n> ms [script, line]"). Fewer fixed windows means fewer load-dependent failures and shorter waits. Do the top 20 by ledger count, not all 4,682.
- Cost: moderate; mostly script edits plus the ledger.

### Idea 9 (extra). Event-based settling for the 92 min of short waits

Most settle waits (`wait 800` after a key) are generous guesses. A `settle <max_ms>` command (return when the UI thread is idle: no animation, no pending update, no event in the queue for 100 ms) would cut about 30 % of them (about 28 worker-min per pass), but it needs an idle probe from the shell (`apps/` hook) and a per-script A/B check that shots and logs are identical. Wave 3 candidate only.

### Idea 10 (extra). Per-script result cache for re-gates

Key (sim binary hash, script, fixtures, golden hashes, simdisk cache key) to a stored PASS. Useful only for test-only fix rounds; the binary hash needs a reproducible build (build date is embedded). Not the default; named `--reuse-passes`; Wave 3 and only if the owner wants it (Q9).

## 6. Ranked plan

Expected wall-minute savings are on the measured 153 min (add the builds of a healthy run, about 3 min warm). They overlap, so the total is the combined schedule at the end of the table, not the sum.

### 6.1 Waves

| Wave | Item | Saves | Risk to coverage | Cost |
|---|---|---|---|---|
| **0. Measure and fail early** (first, 1 day) | Phase timer in `validate.sh` (`step` prints elapsed; `build/.gate/timing.tsv`: step, start, end), per-script timing (sim, trace, golden seconds, worker), per-build wall and ccache delta, `scripts/gate_timeline.py` that prints the table in 2.1 from a finished gate; flake ledger (first cut); fail-fast on any build failure (`GATE_KEEP_GOING=1` to continue); raise the watchdog default for the full gate; print the gate header with every named reduced mode in force | 0 on a green run; **100+ on a red build** | none | S |
| **1. Pool and schedule** (2 days) | One dynamic LPT pool (UI + UBSan + floor jobs), `GATE_JOBS` default 8 after a soak; builds incl. ubsan, ui32 and the three compile-only dirs in one phase; host tests parallel and overlapped with priming; floor halves in parallel; immediate concurrent retries; the Spotlight markers | **about 55 to 70** (4 to 8 workers: the pool section drops from 125 to about 68 min; builds/pre-UI/floor give about 15 more) | none (same scripts, same checks) | M |
| **2. Flake-proofing** (2 days, in parallel with 1's soak) | `sim_cmd_load.c` jitter watcher; `starved` classification in `check_trace.py`; `wait_log`; conversion of the 20 worst ledger offenders; `apps/` CPU-per-frame hook (orchestrator) and the CPU rule; trace self-tests incl. a negative control | enables 8 to 12 workers: another **10 to 20** (8 to 12 workers: 68 to about 45 min) and 5 to 10 per flaky gate | none if the negative controls fail as they should | M |
| **3. Time and cadence** (only if still above 1.5 h) | `time_scale` + 15 to 25 script conversions; `rtc_at` for `p8a-unlit-minute` and the alarm waits; floor receipt reuse (`FLOOR_REUSE`); UBSan baseline diff; named `--ubsan=mapped`; optional `settle`; optional result cache | about 6 (time scale) plus 3 to 14 on re-gates | low to medium; each conversion A/B tested | M to L |
| **4. Not recommended** | Single-pass sanitizer (3d); lock-step sim time (C) | | | |

### 6.2 Resulting schedule (modelled)

Healthy full gate on this 8 core, 8 GB Mac:

| | Now (measured, builds healthy) | After Wave 1 (8 workers) | After Wave 2 (12 workers) |
|---|---|---|---|
| builds | 3 | 4 | 4 |
| host tests + priming + benches | 12.5 | 5 | 5 |
| UI + UBSan + floor | 125 + 14 | 68 | 45 |
| size, strings, upstream | 3 | 3 | 3 |
| **total** | **about 157** | **about 80** | **about 57** |

The target of 1 to 1.5 h is reached by Wave 1 alone provided 8 workers are stable. UBSan's 288 worker-min is the largest block; if the owner accepts 3b as a named mode it drops by about 35 min more, but that is a coverage decision (Q1), not a given.

### 6.3 File ownership

- **Tooling (this agent):** `scripts/validate.sh`, `scripts/simpool.sh`, `scripts/gate-run`, `scripts/shots.sh`, `scripts/check_trace.py`, `scripts/gate_map.txt`, `scripts/risk/sim_ubsan.sh`, `scripts/test.sh`, new `scripts/gate_timeline.py`, `tests/perf/floor_gate.sh`, `tests/ui/*` (scripts, `README.md`, `durations.tsv`), `tests/trace/*`, `tests/*/run.sh` for the parallel-safety audit, and new `uisimulator/common/sim_cmd_load.c`, `sim_cmd_time.c`, `wait_log` (own `sim_cmd_*.c`, registered from `sim_script.c`). Directives documented in `tests/ui/README.md`.
- **Not ours; the orchestrator assigns each as a described hook:**
  1. `apps/ipodui/anim.c`, `ui_anim_frame()`: append `cpu=<us>` (UI-thread CPU time since the previous frame, `simclock.c`) to `anim frame`, sim only (`UPDATE_TRACE` compiles away on target).
  2. `firmware/target/hosted/sdl/kernel-sdl.c:165` and `thread-sdl.c:237`: read the time through `sim_now_ms()` = `(SDL_GetTicks() - base) * scale + offset` (default scale 1: behaviour unchanged).
  3. `apps/ipodui/library.c:184` (the `clock_gettime_nsec_np` extern) so `sim-ui-ubsan` host tests compile (the standing `test_library` UBSan finding).
  4. Optional idle probe for `settle`.
- **Docs:** the gate rules in `docs/process.md` and the budget table in `docs/hardware.md` belong to the orchestrator; tooling supplies the text. Tooling updates `tests/ui/README.md` itself.

## 7. Risks and guard rails

- **More workers raise load.** Soak: 3 consecutive full gates at the new default with zero "starved" or flaky-retry events attributable to load before it becomes the default; until then `GATE_JOBS=4` stays the documented fallback.
- **Memory.** Sims are 17 MB; the 2026-10-09 freeze was builds at `-j4` x 5 plus 16 sims plus a VM. The pool starts only after all builds exit (unchanged); the machine lock stays. The pool cap stays `min(16, 2*ncpu, mem GiB)`.
- **Hidden coverage loss.** Every reduced mode (`--ubsan=mapped`, `FLOOR_REUSE`, `FATCRASH_CACHE`, `--reuse-passes`, `GATE_KEEP_GOING`) is printed in the first lines of the log and in the final verdict line, and none is the default of the plain full gate. `GATE PASSED` is withheld if a script did not run or ran fewer times than listed (the pool prints `ran N of M`).
- **Retry that hides a bug.** The retry rule is unchanged (a failure that repeats on a fresh clone is a real FAIL); the ledger makes repeat flakers visible.
- **Time scale.** Never on a script that plays audio; A/B equivalence per converted script.
- **Cleanup.** Gate scratch stays under `build/` and `out/`; nothing outside the repo; no device access.

## 8. Verification: proving the faster gate catches the same failures

1. **Same-tree A/B verdict diff.** Keep the old gate as `scripts/validate-classic.sh` (a copy, until Wave 3 closes). On one tree, run both and write a verdict table (`step`, script, status, first `FAIL` line) from each; the tables must match exactly except for entries in the flake ledger. Use `gate-p18-final`'s tree: it has **12 known real FAILs** (`batcal-dev`, `fb-np-entry`, `p10-powerlog`, `p10-powerlog-rotate`, `p11-sys-charging`, `p15-apath`, `p18a-lcd-sleep-timing`, `p18a-wakes`, `p18a-wakes-dark`, `p8a-devmode`, `p8a-devmode-boot`, `p9-lang-alloc`) and `gate-p16-final`'s tree (5 real FAILs, 5 flaky). The new gate must report the same real set.
2. **Mutation corpus** (`tests/gate/mutants/*.patch`, applied to a scratch worktree only, never to the working tree): about 15 small patches, each targeting one rule: a golden pixel change; a missing `# log:` line; `nolog` violation; an unbalanced `cpu_boost` (`boost` rule); a slow render (frame CPU rule); a stray "Rockbox" string (native-illusion check); a new warning; a size regression past the backstop; a floor breach (shrunk buffer); a `fatcrash` weakening (the negative control must fail); a UBSan finding (new vs baseline); a hung wait (`wait_pos timed out`); a dropped script (the `ran N of M` guard). For each: old gate and new gate must both FAIL with the same rule name and script. Run once per wave and whenever `validate.sh` changes (it is the only expensive part: use `--quick --changed` style narrowing on each mutant).
3. **Time-scale equivalence:** for each converted script, K=1 and K=10 runs give identical `log:`/`nolog:`/`logbetween:` outcomes, identical shots at golden level, and wake counts within the script's stated tolerance. A script failing this keeps its fixed wait.
4. **Schedule equivalence:** the set of scripts executed is compared with the old selection (the same `job_names`); the pool prints the list and a count, and the gate fails if a name in `job_names` has no result file.
5. **Trace rule self-tests** (`tests/trace/test_*_rules.py`) get the `starved` and CPU-rule cases, including a negative control for the new frame rule.
6. **Soak:** 3 consecutive full gates on one tree at the new default; report wall time per phase from `timing.tsv`, flake ledger rows, and the final verdict.

## 9. Device-reality checklist (tooling change)

Nothing in this plan changes firmware behaviour or any device build. The only code that would reach the shell is the optional sim-only `cpu=` trace field (`UPDATE_TRACE` is a no-op on target) and the optional sim time hook in `firmware/target/hosted/sdl/` (hosted simulator only, not compiled for the iPod). No new splashes, "Rockbox" strings, statics, `cpu_boost` calls, storage wake-ups, persistence or locks on the device. Nothing in the plan touches the iPod.

## 10. Owner questions (each with a default)

| # | Question | Default if no answer |
|---|---|---|
| Q1 | Keep UBSan on **every** script in the full gate? (It is report-only and costs about 290 worker-min.) | **Yes**, but run concurrently in the shared pool. `--ubsan=mapped` exists as a named mode and is never the default. |
| Q2 | Default worker count after the soak? | 8 (the existing formula already gives 8); 12 only if three soaks show no load flakes. |
| Q3 | Abort the gate as soon as any build fails? | **Yes** (print which, exit `GATE FAILED`); `GATE_KEEP_GOING=1` restores the old run-to-the-end behaviour. |
| Q4 | Allow reusing a floor measurement for an identical source hash? | Only for `--quick`; **the full gate always measures** (`FLOOR_REUSE=1` opts in). |
| Q5 | Allow a single-pass sanitizer mode that replaces the separate UI pass? | **No.** |
| Q6 | Will you exclude `build/` and `out/` of each worktree from Spotlight and Time Machine, and keep the VM idle during a gate? | Tooling adds `.metadata_never_index` markers only; the rest is yours. |
| Q7 | ccache: set `base_dir` to the repo root and `hash_dir = false`, keep 2.2 GB, and prune old worktrees (9.5 GB free)? | Yes to the two settings (they do not grow the cache); no size increase. |
| Q8 | Approve the two non-tooling hooks (`anim.c` CPU field; `kernel-sdl.c` time scale) and the `library.c:184` fix? | CPU field: **yes, assign now**. Time scale: **defer** to Wave 3 and decide on the post-Wave-2 timing. `library.c:184`: yes, as a separate small change. |
| Q9 | Want a per-script PASS cache for re-gates (`--reuse-passes`)? | **No.** |
| Q10 | Default `GATE_MAX_MINUTES`: 75 now (every full gate exceeds it). | 240 for a full gate, 75 for `--quick`, printed in the header. |
| Q11 | May tooling rewrite fixed waits in existing scripts (top 20 flakers to `wait_log`; class A/B time waits) under A/B equivalence, with no golden changes? | Yes. |
| Q12 | The brief's "about 140 UBSan scripts" vs the code (every `ipodui-*` script): confirm from `build/risk/ubsan/gate-findings.txt` after the running gate. | Treated as all scripts until shown otherwise. |

## 11. Method notes (reproducing the numbers)

- Gate phase times: birth time of `build/.gate` (the gate's start, `stat -c %W`), mtimes of `build/.gate/build-*.status`, `build/.gate/status-*` (first and last), `out/shots-ubsan/*/sim.log`, `build/risk/ubsan/gate-findings.txt`, `build/floor-gate.log`.
- Per-script wall: consecutive `status-<name>` mtimes on one worker; worker = (index in the sorted, non-skipped job list) mod N (the static partition in `run_worker`). Wave 0's `timing.tsv` replaces this.
- Scripted time: `wait` + `key`/`hold`/`chord` ms (default key 100 ms) + `wheel` clicks x 40 ms, summed from `tests/ui/*.script` (the sim sleeps in whole ticks).
- Model: cost = scripted time + 6.5 s; static round-robin vs longest-first on N workers. Reproduces the measured 52.7 min at N=4 (model 52).
- ccache: `CCACHE_DIR=$HOME/.cache/rockbox-ccache ccache -s` (read-only).
- Unmeasured and to be captured in Wave 0: cold build times (including `sim-ui-ubsan` and `sim-ui32`), the exact UBSan script count, the share of the per-script overhead that is boot versus shots conversion versus `check_trace`/`pngdiff` startup, retry-round cost, and any extra gate re-runs behind the owner's 3.5 to 4 h figure.

## 12. Results of the verification (2026-10-24, branch `gate-speed`)

Machine: 8 cores, 8 GB, other agents building and testing at nice during every run (load average 5 to 22), so these are loaded numbers. Builds were warm (ccache, `base_dir`/`hash_dir=false` set this day), so build time is small in both columns.

### 12.1 Old against new, wall clock

| | Old gate (`gate-p12`, plain full gate, 4 workers, commit c00496f6fa) | New gate (`GATE_POOL=1`, 8 slots, same tree plus the tooling) |
|---|---|---|
| builds | 3 | 0.2 to 4 (warm) |
| host tests, benches, priming, self-tests | 12.6 | 6 to 9 (overlapped) |
| ui scripts | 70 | the pool below |
| UBSan (all scripts) | 80 | in the pool |
| floor (32 + 64 MB) | 11 | in the pool (748 s job, both halves at once) |
| ui + UBSan + floor | 161 | 57 to 61 |
| **total** | **178 min** | **67.9 (soak 2b), 70.2 (soak 3), 76.5 (soak 1)** |

Per-script mean overhead over scripted time was 9 s, as before. The pool ran 392 ui + 375 UBSan + 1 floor job in 57 min at 8 slots (459 to 503 worker-minutes). The model in section 6.2 predicted about 80 min for Wave 1; the measurement is better because the floor and UBSan tails overlap the ui scripts and the flake rounds got short.

### 12.2 Verdict equivalence (`scripts/gate_verdicts.py compare`)

| Run | scripts | real FAIL | differences from the old gate's per-script verdicts | note |
|---|---|---|---|---|
| old `gate-p12` | 392 | 6 | | batcal-dev, nav-reveal, p16-usb-busy-play, p8a-devmode, p8a-devmode-boot, p9-lang-alloc; 10 flaky retries |
| new soak 2b | 392 (ran 392 of 392) | 6 | **0** | **EQUIVALENT**; 2 flaky retries |
| new soak 3 | 392 (ran 392 of 392) | 7 | 1 (`shuf-chunks`) | a timing-bound script (`# log: shuf: mode albums ok`) that fails under pool load and passes alone; in the ledger |
| new soak 1 | 392 | 7 | 3 | `nav-transitions` (a D7 wall-time rule under load), `p14a-classify` (ran into its 60 s watchdog), `p16-usb-busy-play` (flaky); all three fixed or classed by the pcpu rule and a `# timeout: 180` afterwards |
| new soak 2 | 392 | 20 | 16 | **cache poisoning**, see 12.4; not a pool defect, fixed |
| mutants (10 script mutants, `scripts/gate_mutants.sh`) | 11 | 10 + control ok | 0 | classic and new gate fail the same mutants for the same rule: golden, golden region, `# log`, `# nolog`, `# same`, `# expect-file`, shot not taken, `expect_playing`, `wait_pos` timeout, watchdog timeout |

Non-script `FAIL:` lines (builds, size, strings, floor, host tests) were identical (none) in every pair. The ledger and `ran N of M` guard behaved as designed.

### 12.3 Frame rule

The simulator's UI-thread CPU per animation frame measured 0.4 to 1.4 ms (budget `--max-frame-cpu-us` set to 8,000). In soak 1 about 20 scripts had a wall mean of 50 to 96 ms with 0.5 to 1.2 ms of UI CPU and a quiet host (jitter p95 2 to 5 ms): the delay was our own cooperative simulator threads, not the host, so the jitter watcher alone could not class them. The whole-process figure `pcpu=` now decides: starved only when the process was under half busy over the frames; busier fails with "our own threads delayed the UI". Soak 3 reported 23 such "starved by the host" passes and no frame-rule failure. Old gates failed 4 to 8 scripts per run on this rule at 4 workers.

### 12.4 Findings that are not about the pool

1. **The shared simdisk cache can poison a gate.** Its key hashes the generators, not the code. A disk primed by another worktree's firmware under the same key (soak 2: sim-ui "simdisk cache hit") made 14 boot and sleep scripts fail deterministically (`expect_boot_walk ... why=nostamp`), and they passed again on a freshly built disk. The key now also covers the tree ids and the uncommitted diff of `apps/ firmware/ lib/ uisimulator/` (salt 4). Separately, a sim-ui or sim-ui-video cache hit regularly fails its own verification boot ("had to rescan") and is rebuilt, so the cache saves nothing for those two flavours (about 2 min per gate); the stock `sim` hit works.
2. **`shuf-chunks` and `panic-record` fail twice under pool load and pass alone**, repeatedly. They are the first candidates for `wait_log` (Wave 2 part 2, not written).
3. `scripts/validate.sh --quick` runs the 50k floor gate whenever `--changed` is not given (about 11 min); `GATE_ONLY` runs in this repository therefore set `FLOOR_GATE=0`.
4. A gate waiting for the machine lock behind another worktree's gate waits indefinitely; the p12 gate held it for hours while the p18-tree replay queued.

### 12.5 Recommendations

- **Watchdog.** The plain `gate-run --full` on p12 died at the old 75-minute default because that tree predates this change. In this branch the defaults are 240 min (full), 75 (`--quick`), set in `validate.sh`; with `GATE_POOL=1` the full gate takes about 70 min, so a pool default of 120 would still leave margin; keep 240 for the classic path (178 min measured). Merge the tooling to p12 (and any worktree gating soon) before its next full gate, or pass `GATE_MAX_MINUTES=240`.
- **Default.** Keep `GATE_POOL` opt-in for one more integration, then make it the default for the full gate (`GATE_POOL=0` stays as the classic path; `scripts/validate-classic.sh` goes when that is done). The soak shows 67.9 to 76.5 min and an equivalent verdict set once the cache key is fixed, with two chronic timing scripts to convert.
- **Worker count.** 8 held for three full runs on a loaded machine; 12 was not tried.
- Not done: `time_scale` (Wave 3), `wait_log`, floor receipt reuse, the UBSan baseline diff. The known-failure-tree replays are queued, see below.

### 12.6 Not yet verified

- **The `gate-p18-final` and `gate-p16-final` tree replays.** `p18-final` is replayed on worktree `verify-p18` (commit fc7714add5, my tooling copied over, `GATE_ONLY` of its 12 real FAILs plus 6 controls, classic then new). It queued behind another gate's machine lock and had not started when this was written; logs: `gate-p18tree-old.log` and `gate-p18tree-new.log` in the session scratchpad, finished when `p18tree.done` exists. Compare with `scripts/gate_verdicts.py compare`. The p16-final tree was not replayed (its failures were the load-dependent frame rule, which cannot be reproduced on demand). Remove `verify-p18` afterwards (`git worktree remove`).
