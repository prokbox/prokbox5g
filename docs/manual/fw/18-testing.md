---
title: "FW-18 Testing"
weight: 180
description: "How to test a change to the ProkBox5g firmware: the gate, the quick loops, reading a failure, and the rules that keep the gate honest."
---

This chapter covers testing a change to the ProkBox5g firmware with the project's own harness: the one command that decides whether a change is ready, the quick loops you use while working, how the gate is put together, how to read a failure, and the rules the gate runs under. It ends with what the harness cannot tell you. All commands run from the root of your checkout, on macOS or Linux, and need the toolchain from [FW-3](03-toolchain.md) and the builds from [FW-4](04-building.md).

## 18.1 The one command that matters

```
$ scripts/validate.sh
```

This is the phase gate. A change is ready to merge when the last line of its output is:

```
GATE PASSED
```

If anything failed, the last line is `GATE FAILED` and the exit status is 1. The gate prints `GATE FAILED` and stops early if a build fails, unless you set `GATE_KEEP_GOING=1`.

The full gate does the following, in order:

- refuses a branch with a commit whose subject starts with `TEMP-` (override with `GATE_ALLOW_TEMP=1`);
- builds every configuration at once (firmware, bootloader, the simulators, the release shapes);
- runs the host tests, the font check and the trace-rule self-tests;
- runs every `tests/ui/*.script` against the right simulator, comparing screenshots with committed golden images and checking the trace log;
- runs the UI scripts again on a sanitiser build that fails on misaligned memory access, and the audio-buffer floor check;
- checks size, the stock bootloader bytes, the sealed release build, user-visible strings, licensing, and a dry-run merge against upstream.

A full gate takes hours. A watchdog aborts it after `GATE_MAX_MINUTES` (default 240; 75 for `--quick`).

For work in progress, use the wrapper:

```
$ scripts/gate-run              # same as: --quick --changed
$ scripts/gate-run --quick
$ scripts/gate-run --full       # the plain full gate
```

`--quick` skips the bootloader and the stock simulator builds and the sanitiser pass. `--quick --changed` also narrows the `ipodui-*` scripts to those that `scripts/gate_map.txt` maps to your changed files. A changed file that no rule matches, or that a rule marks `ALL`, runs every `ipodui-*` script, because an unmapped file counts as an unknown risk. `--changed` is ignored without `--quick`.

> **Note.** Run one gate at a time. `gate-run` holds a lock under `build/` and refuses a second gate in the same worktree. `validate.sh` also takes a machine-wide lock (`~/.cache/rockbox-gate.lock`), so a gate in another worktree waits and prints `waiting for gate lock`. Each gate sizes its worker pool to the whole machine.

`gate-run` also refuses a third `--quick` run for one task (`GATE_TASK`, default the worktree name). Do not use `--quick` as a retry loop.

## 18.2 Quick loops while you work

Run the host tests:

```
$ scripts/test.sh
```

It builds `build/warble` if needed, then runs every `tests/*/run.sh` and ends with `ALL TESTS PASSED` or `TESTS FAILED`. A misaligned access aborts a host test (`scripts/align-shim`; `ALIGN_SHIM=0` disables it, for comparison only). `TEST_JOBS=4` runs four suites at a time. Do not run it while a gate runs in the same worktree: they share `build/warble`.

Run one UI script and look at the screens:

```
$ scripts/shots.sh tests/ui/<name>.script
```

This runs the headless simulator with the script and writes PNGs to `out/shots/<name>/`, together with `sim.log`. It prints the paths and exits 0 on success, 1 on a failed expectation, 2 on a usage error such as a missing simulator build. Useful settings:

- `SIM_BUILD=<dir>` picks the simulator build (default `build/sim`; the new shell is `build/sim-ui`);
- `SHOTS_TIMEOUT=<seconds>` kills a simulator that runs too long (default 60);
- `SHOTS_WINDOW=1` shows the window.

`shots.sh` applies the hang rule (section 18.5) and the script's own `expect_*` commands. It does not compare goldens or run `check_trace.py`; only the gate does that. Each UI change should come with a script and screenshots, which you then compare by eye. The script format is in `tests/ui/README.md`.

## 18.3 How the gate is built

The gate works in stages. `validate.sh` prints a `== <stage>` heading for each, and `build/gate-timing/` keeps a timing table afterwards.

1. **Builds.** All configurations build concurrently; `JOBS=N` sets the `make -j` per directory.
2. **Static and host checks.** Link map, misaligned static data, fonts, new warnings, host tests, and the trace-rule self-tests in `tests/trace/`.
3. **Selecting.** The scripts to run are listed, honouring `--changed`. Names in `tests/ui/PENDING` are skipped.
4. **Priming.** Each simulator build gets one primed disk image: the test music, the test library and a built database. It is made once, serially, and then cloned for every worker, so a script that rebuilds the database or reboots only changes its own copy. Primed disks are cached under `$ROCKBOX_SIMDISK_CACHE` (default `~/.cache/rockbox-simdisk`) and shared by every worktree with the same inputs. The cache key is described in section 18.6.
5. **Running.** Scripts run on `GATE_JOBS` parallel workers. The default is the smallest of 16, twice the CPU count, and the memory in GiB, because each simulator takes about a gigabyte. Each worker gets its own clone of the primed disk. A `ipodui-*` script passes only when the simulator exits cleanly, `check_trace.py` accepts its log and every committed golden matches. Results print in one block sorted by name.
6. **Flake guard.** Failed scripts are run again, as rule 1 in section 18.6 describes.
7. **Reports.** Size, bootloader identity, release build, strings, sanitiser, floor gate, legal and upstream checks.

`GATE_POOL=1` (full gate only) is an opt-in variant: one pool of `GATE_JOBS` slots (default at most 8) runs the UI scripts, the sanitiser scripts and the floor gate together, longest first, using the durations in `tests/ui/durations.tsv`. A failing script is retried at once inside its slot, on a fresh clone of the primed disk.

Goldens are compared pixel by pixel; more than `GOLDEN_THRESHOLD` percent of differing pixels (default 0.5) fails. A script can exempt a shot with a `# golden: skip <shot>` line.

## 18.4 Reading a failure

Start at the end of the log. For real failures the gate prints a one-page summary: each failing script with its first `FAIL` line. For every golden mismatch it also writes a composite image, golden on the left, actual in the middle, diff on the right, under `out/gate-report/`. Open `out/gate-report/index.md` to find them (`scripts/gate_report.py` makes these).

Then look at the per-script detail:

- `ok   <name> (N shots, <sim>)` means the script ran. Lines after it ending in `[<name>]` are the trace rules' verdicts.
- `FAIL: golden <name>/<shot>` is a pixel mismatch, or a shot the script never took.
- `FAIL: same ...` and `FAIL: ref ...` are the script's own shot-to-shot comparisons.
- The simulator's log for the last run of a script is `out/shots/<name>/sim.log`. The gate's scratch files for the current run, including each script's captured output, are in `build/.gate/` and are wiped at the next start.
- To look again at one failure, re-run that script with `scripts/shots.sh` (section 18.2); do not start the whole gate again.

`scripts/gate_verdicts.py show <log>` turns a saved gate log into one line per script (`ok`, `flaky` or `FAIL`).

### Trace rules

`scripts/check_trace.py <sim.log> <script>` reads the shell's trace lines in `sim.log` and prints one `ok` or `FAIL` line per rule; it exits 1 on any failure. Besides the geometry and frame-timing rules it has library, art and native-illusion rules. The script itself opts into extra rules with comment directives in its header. Two matter often:

- `# logafter: <mark> <regex>` requires a log line matching the regex after the script's `ab_mark <mark>` line.
- `# logbetween: <from> <to> [no] <regex>` looks at the log between the script's `ab_mark <from>` and `ab_mark <to>` lines. `start` and `end` stand for the ends of the log. Without `no`, at least one line in that span must match the regex. With `no`, none may match, and the failure names the first matching line. If either mark is missing from the log, the rule fails.

A trace rule that finds none of its lines fails instead of skipping; this is deliberate. The self-tests for the rules are `tests/trace/test_*_rules.py`, and the gate runs them before the UI scripts.

A failure marked `starved` means the host was too busy to keep time (see rule 4 in section 18.6).

## 18.5 The hang rule

The device has a watcher that reports a thread which sleeps or blocks without saying it is idle. The simulator logs those reports as hang records, and `scripts/check_hang_rule.py` fails a script that has any it did not expect. `shots.sh` runs it, so a single run and the gate apply the same rule.

As the script implements it:

- It reads the script file and `sim.log`.
- A hang record is a line matching `hang recovered <slot> after <N>s` or `panic hang record thread=<name>`. The slots are `ui`, `be`, `art` and `sto`. A panic record's thread name maps to a slot: `main` to `ui`, `be_svc` to `be`, `artcache` to `art`, `storage` to `sto`.
- A record is allowed only if the script has a line `# expect: hang <slot>` for that slot. Any other record is a failure, with the message "unexpected hang record on slot ...".
- The fix for an unexpected record is to mark the sleeping thread idle (`hang_idle_begin`/`hang_idle_end`, or `hang_pause`). Add `# expect: hang <slot>` only when the hang is the point of the test.
- The rule must have something to inspect. If `sim.log` is missing or empty, it fails with exit status 2 rather than passing.
- Exit codes: 0 ok, 1 unexpected record, 2 nothing inspected.

## 18.6 Rules for the gate (decided 2026-11-06)

**1. A failure that persists is a code defect.** A failed script is retried at once in its slot, on a fresh clone of the primed disk. If it fails again, it runs once more in an isolated parallel batch, without the rest of the pool's load. It runs serially only when you ask for that. A failure that survives that isolation is presumed to be a defect in the code: trace it with the log and the trace rules, and never loosen the test, its threshold, its timeout or its golden to make it pass.

In 0.8.0 this policy needs two settings on the full gate, `GATE_POOL=1 GATE_ALONE_JOBS=N`, where N is how many isolated runs your machine can take at once. Release 0.8.1 makes it the default. Without them, the gate still re-runs a failed script alone, one at a time, once the parallel phase ends, with no in-slot retry first. A passing retry prints `flaky-retry ok`. The gate also keeps a flake ledger (`~/.cache/rockbox-gate/flakes.tsv`) and names any script that flaked in two or more of the last five gates as needing a fix.

**2. Missing tools fail loudly; disk images are keyed on tool versions.** The fixture scripts exit with an error when a tool they need is absent: `setup_music.sh` and `setup_library.sh` print `ffmpeg not found` and exit 1, and `mkvideos.sh` fails if `ffmpeg` or `ffprobe` is missing. The gate reports the priming step as a failure and fails the scripts on that disk. The primed-disk cache key hashes the first line of `ffmpeg -version`, or `ffmpeg none` if it is missing. For the video simulator builds it also hashes the `ffprobe` version line and whether `libx264` is present. The key also covers the generator scripts, the committed fixture checksums, the code directories (committed and uncommitted) and a salt. If you upgrade `ffmpeg`, the next gate builds a new disk instead of reusing a stale one, and a disk whose database had to rescan on a verification boot is rebuilt.

**3. The hang rule** is section 18.5, exactly as `scripts/check_hang_rule.py` implements it.

**4. Keep desktop indexers out of the build tree, and size the jobs to the machine.** This is advice from experience rather than something the gate checks. The test libraries hold tens of thousands of files, and the gate clones them for every worker, which swamps a desktop indexer and slows everything else down. Exclude your build tree from indexing:

- on Linux with GNOME, exclude the checkout (or at least `build/` and `out/`) from LocalSearch (Tracker), or turn it off while you test;
- on macOS, exclude `build/` and `out/` from Spotlight. The gate already creates `.metadata_never_index` markers there, which helps but does not cover every case.

Size parallel jobs to the machine's real headroom, not its core count: `GATE_JOBS`, `JOBS`, `TEST_JOBS`, `UBSAN_JOBS` and `GATE_ALONE_JOBS` all take a number. The default `GATE_JOBS` assumes about 1 GiB per simulator; two gates at once once froze an 8 GB machine, hence the machine-wide lock. If a trace rule reports `starved`, lower the numbers.

## 18.7 Setting up a machine for testing

You need:

- the toolchain from [FW-3](03-toolchain.md), including `arm-none-eabi-gcc`;
- `python3`;
- `ffmpeg` with MP3 support (`libmp3lame`) for the music and library fixtures, and `ffprobe` with `libx264` for the video fixtures;
- `git`, with a local `main` branch: `--changed` and the `TEMP-` check compare against it (without one the gate prints a note instead);
- free disk under `build/` and for the cache in `~/.cache/rockbox-simdisk`, and the indexer settings from rule 4.

Run `scripts/test.sh` first; it is the cheapest proof that your toolchain works. Leave the machine quiet during a gate: a busy desktop makes timing rules fail.

## 18.8 What the gate does not cover

Everything above runs on host builds and the simulator. The gate cannot tell you how the firmware behaves on a real iPod: the click wheel, the display driver, the audio hardware, power and battery behaviour, USB timing, and the real disk. The simulator accepts things the player's CPU would fault on, which is why the alignment checks exist, but they are a safeguard rather than a proof.

Device testing is done only by maintainers, with the project owner's go and a fresh backup of the device. If your change touches hardware-facing code, say so in your pull request and a maintainer will arrange a device run.
