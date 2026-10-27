# P19 bench entries: B5, B6, B7, B13, the brief probes and the COP rows

## Session 2026-10-25 (build A)

Results of the first bench run on silicon, from `bench-20261025-1430/` (`bench-20261025-1430/bcmtest.log`, `bcmtest-results.log`, `bench-20261025-1430/ipodui.log`; local, never committed). Build A is p19 `4e57b3b9a1`, merged image fnv `cd8e5d96`, display-only image fnv `f7772048`, probe image `b51bab40`. Folded into the manual: `bcm2722-reference/quirks.md` #30 to #32, `isa-vector.md` ("Silicon check of the modelled forms"), `brief-map-power.md` (PW1, PW2, PW7, PW8), `brief-map-peripherals.md` (PB-2, PB-3) and `open-questions.md` #67.

| Row | Result |
|---|---|
| Stages 2, 3, 4, 4b, 5 | PASS (stage 4: vector 24/24, scalar 9/9, probe checks 9/9, kernels 69/69; stage 4b: 150 MHz switch measured) |
| Vector checks | X15M, X15Z, MHDU, VCLP, R48E, D16R, RNDT, RNDU, MLSC and the REP-16 `vst16` layout as modelled; **ASHF, ASHG, X15S and REPA not** (fitted: quirks #30–#32; rows ASHX, ACSA, GTHR below) |
| C1, C2, C3, PB-1, PB-4, PW8 | PASS (C2: separate, no coherency; C3: COP awake under yield) |
| PW1, PW2, PW7, PB-2 | FAIL (details in `brief-map-power.md` and `brief-map-peripherals.md`) |
| PB-3 | done: no change with the cable |
| Stage 6 | **Not a merged-image result.** It never reached the image: "the BCM is busy", the player's take was refused, so every clip logged 0 frames. |
| Stage 7 | not available (no test video) |
| B13, the warm swap | FAIL at round 0, **step 2: "never parked"** |
| B5, the window push | FAIL at **step 5** (-5), the `win_slot` `COMMIT` |
| Merged image verification | `COMMIT` has not passed on silicon (`jobs.md` §10), so the image's job path is not yet device-verified |

The lane-shift self-check fell back to C as designed (`bcmjob vpu=0x1 expect=0x3 why=degraded`). The next session needs the status word logged for `COMMIT` and the port write-check probe (open question #67); PW5 stays locked behind a passing PW1.

## Session plan: merged image verification + P19 rows

**What this session is for:**
- earning the merged image (video plus the display jobs) its `bcmfw/verified.txt` entry (`memory-contract.md` §6, steps 2 and 3);
- running the P19 developer and hardware rows that are built and not yet run on silicon.

Main-UI features are not part of it: ordinary use tests them.

**How it runs:** one Developer row, **Bench session** (`apps/ipodui/devbench.c`; Developer menu layout of 2026-10-26, `docs/design/devmode.md` §3), does the whole session on the iPod:
- Part A runs on its own.
- Part B then guides the owner: one short instruction a step, and a question with wheel answers where a step needs a human judgement.
- Every result, answer, timing and key log line goes into one results file, so nobody writes anything down.

The cards further down this page stay the reference for what each step checks.

**The build:**
- It is `p19` with 19-2 merged, plus `p19-bcm-prep` (with `p19-session-img`), configured as `--ipodui --bcmfw-devtest --bcmfw --bcmfw-video --bcmfw-l1 --bcmfw-merged`.
- `--bcmfw-merged` makes the merged image the player's `bcmvideo.bin`.
- The probe rows always use the plain image, `bcmprobe.bin`.
- The zip carries both. Before asking for the go, the agent checks each file's size and FNV-1a against `apps/video/bcmvideo_image.h` and `bcmprobe_image.h`, then writes them here:

| File | Image | Size | FNV-1a |
|---|---|---|---|
| `bcmvideo.bin` | merged | 218,660 B | `cd8e5d96` |
| `bcmprobe.bin` | plain | 220,900 B | `b51bab40` |

These are the values at `p19-retained` (19-4's rev 0.4, over 19-5's rev 0.3 at `45097ba6`; the merged image was `9af095b8` at `p19-session-img`). Any change to the image changes them, so the agent re-reads them on the day. **The staged session build keeps verifying `9af095b8`**, the image it was built with; a full session on `p19` ends with the `verified` line for the image it carries (`cd8e5d96` today) when it passes; a merged image built after that goes through the Bench session's "Re-verify the video image" mode before its FNV joins `verified.txt`.

**Rules:**
- Each install needs the owner's go and a fresh backup.
- Apple's firmware stays as the way back (hold MENU at boot).
- If the unit hangs, hold MENU + SELECT for 6 to 8 seconds, then open the row again: it offers **Resume after <step>**.
- Nothing a step does persists, except the boot path step in Part B (on purpose).

### Before: backup and install (the agent, with the owner's go)

1. Back up the hidden system folder to the backup folder and check the copy.
2. Have the owner run `sudo fsck_msdos -n` (the agent gives the exact disk).
3. Pull and clear `ipodui.log`, `power.csv` and `bcmtest*.log`.
4. Unpack the build.
5. Nothing to place by hand: the session sets the developer marker `bcmjobs.on` itself when it is missing (the job driver needs it for an image that `verified.txt` doesn't list yet) and removes it at its end; Developer › Status shows it while it is held. A marker placed by hand is left alone. The re-verify run, when it starts with the stage flags present and the resident image unverified, first asks for one restart (the gate is read at boot), then Resume.
6. Check that both image files are there with the sizes above, then eject.

- **Pass:** the backup checks out and the unit boots to the menu.
- **On failure:** no session. The backup is the way back.

### Part A: one watched step, then walk away (5 + about 15 minutes)

1. Settings › About, hold SELECT + Previous 3 s, then Developer › **Bench session**.
2. Read the one confirm screen and select its last line.
   - It lists everything Part A runs and says all of it is read-only or reversible.
   - The rows' own confirm screens are skipped by the session's batch flag; their defaults don't change.
3. **Stages 0–5 come first, and they are watched** (`s05`, about 5 minutes, no watchdog). The stages' pass verdicts are automatic, but they don't cover the picture:
   - Stages 0, 1, 4 and 5 pass on reads, the probe's self-check, the bench's records, and counts and times (`bcm-devtest-core.c` `stage0`, `stage1`, `stage5`, and stage 4's `bench_wait`).
   - Stages 2 and 3 pass on the frame count, the mailboxes and the panel state (`stage2` ends `n != 6 || !(info & INFO_PANEL_ON)`; `stage3` "frame count / DMA counts / panel state"). Nothing reads the panel back.
   - What the panel showed was always judged by eye: their `PHOTO 3` to `PHOTO 6` lines (pattern with box, no black band, magenta box over the bars, speckles at a bus timing, the CPU fallback's band).
   - So `s05` ends with **"Did the test pictures look right?" Yes / Wrong colours / Garbled / Blank / Skip**.
4. A "Hands off now" cue follows. Select it and walk away.

**What it runs, in order, after `s05`.** Each step has a watchdog (the rule is below), and the file keeps the record.

| Id | Step | Watchdog | Expected on this build |
|---|---|---|---|
| `s4b` | Stage 4b (150 MHz and the top core voltage, undone by its power cut): the second clock for ASHF/ASHG, X15M–D16R, REPA | 180 s | pass (the rows themselves are read by readlog afterwards) |
| `s4c` | **Watched, not hands off** (about 1 min, one question; the "Automatic checks done" screen comes before it and "Hands off now" after): stage 4c, the 150 MHz clock with the screen on, three 8 s pictures. Only on a 4b flag (else skipped, no question). A pass writes `bcmpass4c.txt`; with 4b's flag it lets the video player run the chip at 150 MHz (`bcm_devtest_fast_clock_allowed`). Any answer but "Yes" removes the flag again (`bcm_devtest_forget_4c`). Added after sitting 1: the unit had a 4b flag but no 4c flag, so stage 6 decoded at 27 MHz (42.7 ms a frame against 33 ms) | — | pass |
| `c1` `c2` `c3` | COP rows C1–C3 | 90/60/60 s | pass, or a finding |
| `pb1` `pb4` `pw1` `pw2` `pw7` `pw8` | Wave 1 probes that need nobody (the probe image) | 90 s each | pass |
| `jobs` | The job driver's self-test | 30 s | **skip** ("jobs off"): the resident image isn't the driver yet; it runs again in Part B |
| `jvpu` | `J_VPU` from the running image's job block, against that image's own value (merged 3, display-only 7) | 30 s | **skip** (no job block yet), likewise |
| `port` | The video port's MB/s | — | **skip**: it comes from stage 6's play, which needs eyes on the clip (Part B, `s6`) |

**Expected summary** (the "Automatic checks done" screen, `s05` and `s4c` included): 12 passed, 0 failed, 3 skipped (a row with no verdict line of its own counts as passed, "done").

**The watchdog never restarts the iPod mid-write** (`devbench.h` `bench_dog_decide`, tested in `tests/autochecks`):
- The bench writes nothing while a step runs: the results file, its `auto:` lines in `bcmtest.log` and `ui_log`'s flush (held in RAM by `ui_log_hold`) all wait for the step's end, with the watchdog disarmed.
- The rows' own `bcmtest.log` lines are the only writes during a step.
- Past a step's limit, the watchdog restarts the iPod only while the disk is quiet: off, or no transfer stamped for 0.5 s (`storage_disk_is_active()`, `storage_last_disk_activity()`; `ata.c` stamps every transfer's start, chunk and end).
- If the disk is busy, it looks again each tick for up to 10 s.
- If it's still busy after that, it doesn't restart: it records "bench hang, disk busy: <id>" in the RAM panic record (the next boot's `ipodui.log` shows it), and a small watchdog thread shows **"A step hung while the disk was busy. Hold MENU and SELECT to restart."**
- The screen is best effort. If the hung step never yields, the screen stays as it was. Then the rule is: **a screen frozen for more than the step's limit plus 10 s means hold MENU + SELECT**.

**If something fails:**
- `s05` failing (its stages, or an answer other than "Yes") stops nothing else, but every BCM step after it then says "Not available" and skips. Report the stage's line. It's a regression against session 17.
- A COP step that hangs restarts the iPod. On resume, the other COP steps are skipped (the worker is stuck).
- A probe's failed canary voids that run only. The file has the line.

### Part B: the steps that need the owner

Each step's first screen is a short script: what happens, for how long, what is normal, and what MENU and SELECT do (the exact screens: **Owner's script**, below, generated from the code). SELECT on its last line starts the step; MENU stops the session there, and the next press resumes at that step. At the end of a step the question appears with each answer's meaning; the first answer is the good one, "Skip" is always there, and MENU counts as Skip. A step that could not run (not available, or MENU in a battery row's phase) asks nothing. The instruction column below is a summary.

| Id | On-screen instruction | Question (answers) | Notes |
|---|---|---|---|
| `pb2` | "Pin levels: follow the cues." | — (the row's own seven cues) | PB-2 card |
| `pb3` | "USB status: plug in and pull out USB-C on cue." | — (the row's cues) | PB-3 card |
| `s6` | "Two short clips play. Watch and listen." | How did the clips play? (Smooth, in sync / Stutter / Sound off picture / No picture) | The merged image's decode on silicon. Its first log line must say `video image (merged) 218660 bytes fnv cd8e5d96`; `(plain)` means the build lacks `--bcmfw-merged`. The port lock's cost is read from this run's port lines |
| `s7` | "The iPod works the test video's controls by itself (about 1 min); hands off." (It is scripted: the old "follow the steps on screen" was wrong.) | Did every control work? (Yes / Some failed / It hung) | stage 7 |
| `b13` | "Watch the screen while the chip swaps programs (last restart dark)." | Did it blink, blank or flicker? (No / Yes, briefly / Yes, badly) | B13, into the merged image |
| `b5` | "Pictures in windows; a white frame stays 5 s at the end." | Was the frame straight and whole? (Yes / Sheared / Broken) | B5 |
| `cr1` | (hands off; the "Hands off now" cue comes first) | — | CR1, the COMMIT readback (below): the port's writes read back, then `COMMIT`, on the display-only image |
| `boot` | "Our video image becomes the display driver; then restart (MENU+SELECT)." | — | Sets the boot path to DMA. The owner restarts, then opens the row again: **Go on: Display driver** |
| `rstat` `jobs2` `jvpu2` | (hands off) | — | The boot status, the job self-test, and `J_VPU`. Each image is judged against its own value (`bench_vpu_expect`, from the block's `J_OPS`): **the merged image 3** (checked, shift; no DECODE, so no `J_VPU_MAC`), **the display-only image 7**. B5 reads the display-only image's word (`lab5: J_VPU …`), and the file pins it under `b5`, so a session records both. `J_VPU_SHIFT` clear (1, or 5) means the shift check disagreed: compare it with ASHF/ASHG |
| `slp` `lvl5` `lit24` `lit24ui` | "About 80 s on battery, 4 phases of 20 s, A B A B; leave it alone until the row ends." `lit24ui`: the menus at 24 MHz, judged by eye | Did the screen stay right? (`slp`, `lvl5`); Did the menus stay smooth at 24 MHz? (`lit24ui`) | **The battery rows**, viability only (no meter, no power claims): card "The battery rows" below. `lvl5` asks for a go first |
| `b3` | "Debug: drift on video chip: on; watch the menu's covers 2 min." | Did the covers drift smoothly? (Yes / Stutter / Frozen / Blank) | **19-2 only** |
| `b3ab` | "Drift on video chip on, 5 min; then off, 5 min (power log on)." | Did both halves run to the end? (Yes / On half failed / Off half failed) | 19-2 only. The `drift jobs mode=` lines time the switches against `power.csv` |
| `drill1` | "Debug: next wake never answers; then wake holding MENU through it." | Did the screen recover? (Yes / Still cover / Hang / Reboot) | 19-2 only |
| `drill2` | "Drift on: the next two fades fail on purpose; watch 1 min." | What showed after it? (Drift or still cover / Frozen / Blank / It restarted) | 19-2 only. **Arms, then leaves the session** (`BENCH_F_LEAVES`): the owner watches the menu, then picks "Go on: Drill: job failure" and answers. The row arms `bcmjob_dev_inject_fail(J_OP_FADE, 2)`: the second counted failure marks FADE for the boot, and the drift falls back. The CPU's drift is pixel-identical to the chip's, so "no visible change" is the pass; the log's `bcmjob fault op=0x.. why=failed` lines and `drill2: inject` show it happened |
| `drill3` | "Let the backlight go off during a cross-fade, then wake." | Did the drift resume cleanly? (Yes / Stuck frame / Hang) | 19-2 only |
| `scroll` | "Drift on: scroll a long list fast, top to bottom and back." | Was the scroll smooth? (Yes / Tearing / Stale pixels / Stutter) | 19-2 only |
| `wave2` | — | Run wave 2 (TV out, voltage, hibernate)? (Not now / Yes, run it) | The separate go. "Not now" skips everything below |
| `pb6` `pb7` `pb8` `pb9` `pw3` `pw4` `pw5` `pw6` | One line each (AV cable and TV for PB-6, PB-8, PB-9) | — (the rows' own confirms, locks and questions stay) | Wave 2 cards |

**Manual steps:**
- The 19-2 steps (`b3` to `scroll`) are manual until 19-2 wires its functions in. The instruction says what to do in Developer › Advanced › Video chip or the menus.
- SELECT leaves the session. The owner does it, then opens the row again, and the question comes up first.
- Two checks need no question: the boot log's floor line (`mem floor dev audio= bcm_res= free=`) and the `drift ...` log lines. The agent reads both from `ipodui.log`.

### After: the summary screen and what to send back

1. The last screen says "N passed, N failed, N skipped. Results saved. Plug into the Mac when done."
2. Optional: reset to Apple's firmware once (hold MENU at boot), then reset back.
3. Plug into the Mac. The owner runs `sudo fsck_msdos -n` again.
4. **Send back:** the fsck output and the go for the file pull. Photographs only of anything odd: everything else is in the file.
5. **The agent pulls:**
   - the session's results file, `bench/<date>-<build>.txt` in the hidden folder;
   - `bcmtest.log` and `bcmtest-results.log`;
   - `ipodui.log`, `power.csv` (and `.1`);
   - the `bcmpass*.txt` flags and `bcmtest/`.
6. Nothing to remove: the session's end took away the `bcmjobs.on` it set (one placed by hand stays).
7. Afterwards, in regular use: the drift on the video chip should simply look right. Anything odd goes in the next session's notes.

### After the session: ingest

Once the pulled copy of the hidden folder is on the Mac (step 5 above), one command reads all of it:

```
python3 -I scripts/bench_ingest.py <copy of the folder> --commit <the build's commit> --patch <file>
```

- Always with `-I`: the pulled files are untrusted data. The tool never runs or evaluates them, skips symlinks, caps each file's size, escapes control characters, and reports a truncated or corrupt file (a restart mid-write) instead of stopping.
- `--commit` picks the build's `devbench.c`/`devbench.h` for the step table and the verdict rules (build A: `4e57b3b9a1`). Without it, this tree's are used.
- **What it prints, failures first:**
  - a "Failures and anomalies" block: failed and hung steps; every answer other than the good one, with the question and the step's `bcmtest.log` lines (between its `auto:` markers); bench checks not as modelled; cut files; a drift-still verdict;
  - each results file's table: one row per step (result, answer, ms, note), the mode, the image line and the end line, replayed as `devbench.h` does;
  - the readlog rows: the bench checks (ASHF/ASHG, X15M/X15S/X15Z, MHDU, VCLP, R48E, D16R, REPA, PUTV, BARV) at each clock, the port's MB/s, `J_VPU` per image, the floor line, the drift-jobs lines, the pass flags and the `power.csv` marks;
  - the stale-band hints (rects below row 0, scroll and pan parameters, engine-2 pieces; stage 5's storm rects leave rows 0–19 to the clock rect by design);
  - the drift-still lines in order, with a verdict: (a) a job marked failed or slow, (b) PRESENT/FENCE never completing, (c) the frame_due hook stalling the feed, (d) jobs on but the merged image not resident, (e) drill 2's injected failures.
- **The verified line:** if the session earned one (the replay's line must match the file's), the tool writes a patch to `--patch` and prints it. The patch is never applied. It holds the `bcmfw/verified.txt` line, the regenerated `bcm-devtest-images.c` (`bcmfw/mkdevtest.py` in a temporary worktree, or its renderer over the committed images when there's no VideoCore toolchain: `--mkdevtest render`), and the reference-doc tag changes (`[silicon] pending (…)` to `[silicon] as modelled (…; bench … session <date>, build <id>)`) for every check that came back as modelled. Checks that came back otherwise are listed for a writer. Review the patch, then `git apply` it.
- Exit status: 0 when nothing needs attention, 1 when the block above has entries, 2 for a usage error.
- The tests: `tests/bench_ingest/run.sh` (synthesized folders from `gen_fixtures.py`; no device logs are committed).

### The results file

One file a session (`bench/<yyyymmdd-hhmmss>-<build>.txt` in the hidden folder, never named on screen), appended and closed a line at a time, so a restart keeps it. It is also the resume record. The format is `devbench.h`'s:

```
session build=<id> date=<yyyy-mm-dd hh:mm> steps=<n>
step=<id> result=start
step=<id> log=<one of the step's last six log or summary lines>
step=<id> result=<pass|fail|skip|done|hang> answer=<token|-> ms=<n> note=<the last verdict line>
resume after=<id>
end passed=<n> failed=<n> skipped=<n>
```

- An answer token is the answer with spaces as `_`.
- "done" means the step ran and has no verdict of its own.
- "hang" means a step started and never ended (found on resume).
- `bcmtest.log` gets one `auto: <id> <result> answer=<a> ms=<n> <note>` line a step, among the rows' usual prefixes.

### Adding steps (for other sessions' code)

- Add an entry to `steps[]` in `apps/ipodui/devbench.c`, in run order, compiled in under the feature's own flag (as the 19-2 entries are, under `HAVE_BCM_JOBS`). Each entry has:
  - an id (short, unique);
  - a title;
  - one or two instruction lines;
  - the function (`enum bench_res`; a Developer row's `bool` function goes through the `ROW()` wrapper, and its log and summary lines give the verdict);
  - the question and up to four answers (the good one first);
  - flags: `BENCH_F_AUTO` for no owner and the watchdog, `BENCH_F_COP`, `BENCH_F_REBOOTS`, `BENCH_F_GO` with `BENCH_F_OPTIONAL`, and `BENCH_F_OWN` to keep the row's own confirms;
  - a watchdog limit.
- An entry with no function is manual. Wiring the function in later is the only change needed: the id, the question and the file's lines stay the same.
- The pure parts (the format, the replay, the verdicts) are tested by `tests/autochecks`. A new flag or format change goes there too.

### The `verified.txt` entry this session can earn

- **When:** the full session ends with it when it passes every check the re-verify run makes (`devbench.h` `bench_full_ok`, over `bench_reverify_picks`): `s05` (the full session has no `pre`, so the watched stages themselves must pass), `jobs` and `jvpu` (or their reruns `jobs2` and `jvpu2`, after the `boot` step's restart onto the display driver, which stand in when part A's skipped), `s6`, `b13` and `b5`; `b3` and `drill2` (19-2 builds) pass or skip, never fail. The image must be the merged one. Other steps' results don't affect it. No second run is needed.
- **What:** the same line as the re-verify run's (below), naming the full session, shown on the end screen and written to the results file after `verified `, before the `end` line:

```
bcmvideo_jobs cd8e5d96  stage 6 PASS (merged, 218660 bytes), B5 PASS, jobs self-test PASS, J_VPU 3 PASS, drift B3 and drill 2 PASS on the owner's 5.5G: full session 2026-10-25, build <id>
```

- **The results file:** the full session's line also records `image=<bytes> fnv=<hex> kind=<merged|plain>` (no `mode=`), so a resumed session still earns the line for the image it started with. A file from before this change has no `kind=`, so it earns none.
- **If a check failed or skipped,** the end screen says "Not every video check passed: no verified line this time."
- **The FNV:** take it from `s6`'s log line, not from this page.
- **Then:**
  1. Rerun `bcmfw/mkdevtest.py` to compile the list in.
  2. Make the merged image the default: the choice `--bcmfw-merged` makes today becomes the build's default, and the flag retires (`memory-contract.md` §6, "Built").
- **What doesn't get a line:** the plain image needs none (only the default path's rungs are checked). `hello_dma` is already listed.

### Re-verify a new merged image (about 10 minutes)

**When:** for later images: the merged image changed after it earned its line (a new FNV-1a in `apps/video/bcmvideo_image.h`), and the full session has already run on this unit (the full session earns the line for the image it runs). Each new FNV needs its own `verified.txt` line; this run earns it without the whole session. A new unit, or new P19 rows to run, still takes the full session.

**How it runs:** the same row. Press it and pick **Re-verify the video image** (if a session is open, the resume offer comes first; **Start a new session** then asks which). A confirm screen lists the run. Same install and backup steps as the full session (Before, above); the session sets `bcmjobs.on` itself. The unit's boot path should already be the display driver (the full session's `boot` step), so the merged image is the resident one at boot.

**Sitting 4d** holds `b3`, `s4b`, `s4c`, `vfeed`, `s7`, `vbv` and `vgov` (`BENCH_R_HELD`: in the table below, not in the run; drop the `| BENCH_R_HELD` to bring one back).

**What it checks, in run order** (sitting 4c's runner, `devbench.h` `bench_reverify_picks`; the full list, the needs and the gates are in the owner's script below). Basic to advanced, independent groups first, so a failure can't strand a later group; no screen asks "run it or skip", no screen shows a step code; a step that may flash, blank or restart opens with a one-line heads-up:

| Group | Ids | Must | When it fails |
|---|---|---|---|
| 1. Start-up | `pre`, (`s05`, `boot` only if needed), `jobs` | `jobs` pass (straight after the boot, while the merged image is resident) | `s05` failing ends the session (every later step skips). `jobs` failing skips group 2 only; a restart is offered once when one may fix it ("Restart and retry" / "Carry on") |
| 2. The chip's jobs | `b3`, `respanel`, `uviz` | `b3` may skip, never fail | needs `jobs`. Straight after it: every later step that swaps the chip's program hands it back to Apple's vmcs (stock wake path, `dt_restore(false)`), jobs off until the next start: `c1`'s probe image (`jvpu` only reads the running image's words), the 4b/4c stages, the labs, the stage plays (sitting 4b: the panel decoded on the PP, no overlay load). None of the three swaps the program itself (b3 sets the drift's backend; the panel's DECODE and the visualizer's overlay load into the merged image). `respanel` before `uviz`: it wants an album not yet played on this build |
| 1b. The other self-tests | `jvpu`, `c1`, `s4b`, `s4c` | `jvpu` pass (`J_VPU` **3**, read from the still resident merged image) | `jvpu` failing costs the line and skips nothing. `s4b` failing skips `s4c` |
| 3. The chip's programs | `b13`, `b5` | `b5` pass | independent: run whatever failed before |
| 4. Video | `s6`, `vfeed`, `s7`, `vbv`, `vgov` | `s6` pass | `s6` failing skips the rest of 4 and 5 |
| 5. One normal play from Videos | `vplay` (manual, outside the bench: the first-use check's "Preparing Video..." then the play) | — | needs `s6` (sitting 4d: the image's clock150 token opens the fast clock, so `s4c` no longer gates it; the step drops the unit's check result first so the check runs again) |
| 6. The UI on the second core, last | `ucop` (restart), `uck1`, `uck2`, `ufeel`, `upanel`, `ucop2` | — | needs `c1`; `ucop` is recorded failed when the restart leaves the UI off the COP, and the rest skip |

Why video comes before the second core: `ucop`'s restart moves the UI to the COP for the rest of the session, and the bench runs its steps on the UI thread, while the devtest's stage plays and labs are CPU-only code (no remote calls; `dt_restore` edits the tick list, BCM must-fix 3; engine 2 and the devtest's flags are driven directly; `p19-ui-on-cop.md`). The restart also brings the merged image back after the stage plays.

**The clean finish:** a must-pass step that fails never stops the session by itself. The steps that need it skip by themselves (results file: `result=skip`, `note=auto-skip: needs <id> (<result>)`; one plain sentence on screen, once per failure); the groups that don't need it still run; the end screen lists what didn't pass by title and says "That's all for today. Plug the iPod into the Mac." `bench_ingest.py` lists the auto-skips by the step they needed ("auto-skipped after s6 fail: ...").

**The results file:** the same format. The session line adds `mode=reverify image=<bytes> fnv=<hex> kind=<merged|plain>`, from the build's `BCMVIDEO_IMAGE_*`, so a resume carries on in the re-verify run.

**The verified line:** if every step passed (and the image is the merged one), the end screen shows the line to add, and the file has it too, after `verified `, before the `end` line:

```
bcmvideo_jobs 9af095b8  stage 6 PASS (merged, 218756 bytes), B5 PASS, jobs self-test PASS, J_VPU 3 PASS on the owner's 5.5G: re-verify session 2026-10-24, build <id>
```

(with `, drift B3 and drill 2 PASS` before ` on the owner's` when both ran and passed). The agent pulls the results file, copies the line from it into `bcmfw/verified.txt` (adding the backup directory and `bcmtest.log` line numbers if wanted), and reruns `bcmfw/mkdevtest.py`, which compiles the list into `bcm-devtest-images.c`. If a step failed or skipped, the end screen says there is no verified line this time.

**The resident rung and `verified.txt`:** loading the unverified merged image as the resident driver trips nothing. The resident rung needs the setting, the player's offer (the header's size and FNV) and no failure at an earlier boot. `bcmfw_res_verified()` only shows in the status row and opens the job gate, which `bcmjobs.on` opens meanwhile.

### Port write check (about 2 minutes)

**When:** to settle open question #67 (`COMMIT` failing on silicon, 2026-10-25) without the full session, and again after any change to the port's write paths.

**How it runs:** the same row; pick **Port write check**. A confirm screen, then `pre` (the stage 5 flag; if it is missing, `s05` is offered, the one question this mode can ask), the "Hands off now" cue, and `cr1`. No other wheel question. The session line says `mode=portcheck`; a resume carries on in it. It earns no verified line. It writes only the results file between steps and `bcmtest.log` during `cr1`, under the bench watchdog (120 s) like the part A rows.

---


**What this is:** the BCM session's device-test entries for the P18+P19 bench (`docs/device-bench-p18-p19.md`, Part B), built on `p19-bench-splus`.

**How to run them:**
- Each is a Developer › Advanced › Video chip › Probes row (B5, B13 and CR1 are Bench session steps now, not rows).
- Each needs a stage 5 pass flag and the video image file.
- Each writes its lines to `bcmtest.log` and shows a summary.

**Tags:** [sim] vc2sim only, [ours] our design. None of these has run on silicon yet.

| Card | Entry (exact Debug row) | Time | Log prefix | Decides |
|---|---|---|---|---|
| B5 | **BCM test: window push (B5, optional)** | < 1 min | `lab5` | #53: a panel push straight from a texture window at any pitch (19-2, 19-5) |
| B6 | **BCM test: texel fetch (B6, optional)** | ~1 min | `lab6` | #54: the cost of one fetch at a computed address (19-7, the affine and 3D budgets) |
| B7 | **BCM test: memory bandwidth (B7, optional)** | ~2 min | `lab7` | #7 (the core's SRAM and SDRAM bandwidth) and #44 (whether port traffic slows the core) |
| B13 | **BCM test: warm swap (B13, optional)** | ~1 min | `warm` | Q2: the display-only resident image (`warm-swap.md`) |
| CR1 | **BCM test: COMMIT readback (CR1)** | < 1 min | `cr1` | #67: why `COMMIT` fails on silicon: the write path, SRAM the host can't write after the start, or the BCM's own sum. **Answered 2026-10-25:** the BCM's sum (`quirks.md` #33), plus odd-width rows at 2 mod 4 (#34) |

## B5: window push

**What runs:**
- The display-only image (`jobs_display`) boots cold.
- It fills two slots through the job interface:
  - 300 × 300 (pitch 300);
  - 400 × 160 (pitch 400).
- It presents a 160-wide window of each, starting at an odd row (31 and 41), with `J_PRES_WAIT`.
- Beside them, the same window of the framebuffer (pitch 320).
- Ten pushes each, each timed by the chip's own DMA counter (`BCMFW_MB_DMA_US`, arm to end).
- The pitch-300 window stays up for 5 s.

**What the owner sees:** a white frame with a white diagonal over blue and dark-red stripes. It must be straight and whole: a wrong pitch shears it, and a lost row breaks the frame.

**Lines:**
- `lab5 pitch P: N px from row R, A-B us, C ns/px`, once per pitch.
- `lab5: PASS|FAIL (N pushes by DMA of 31, M timed out); …`

**Passes when:**
- all 31 pushes went by DMA, none timed out;
- the photograph shows the frame straight and whole.

**[sim]:** `jobs_image_test.py`, row "B5". The panel equals the slots' windows pixel for pixel at both pitches, through the DMA model's 2-D mode.

**A failed slot (from p19-bench-fix):** `lab5: step 5.k st=N (what, size)`, k 1 alloc, 2 write, 3 flush, 4 commit; `st` is the reply's status (`HP_E_*`, `J_E_*`: 21 `J_E_SLOT`, 23 `J_E_SUM`, 24 `J_E_MODE`), a `BVP_E_*` (-2 no reply in time) or the port call's -1. The `COMMIT` now waits twice `J_T_COMMIT` for the slot's bytes (190 ms for the 300 × 300 drift slot); before, it waited 50 ms, under the 95 ms budget, so a slow but sound `COMMIT` could fail as a timeout.

## CR1: COMMIT readback

**Why:** on 2026-10-25 `COMMIT` failed for the UI drift (engine 2) and for B5 (CPU), on both images, with no status logged (open question #67). memory-access.md §8 proves host writes after the start only for the rings, the host frame and SDRAM; every slot pool is SRAM below `0xD0000`.

**What runs** (`vplayer_commit_lab`, on the display-only image, booted cold as B5 boots it; nothing goes to disk but `bcmtest.log`):
1. **Regions.** 64 words of a unique pattern, written by our CPU path (`bcm_video_wr_block`), read back by the port, then the old words put back, at: the control `0x105700` (the host frame's last 256 bytes, proven writable), each pool base of the display-only image (drift `0x46000`, strip `0x9F200`, app `0xBF200`, thumb `0x12B000`), the drift pool's middle `0x72880` and last 256 bytes `0x9F000`, and the scratch pool `0xD4000`. Line: `cr1 region 0x%06lx wrong=N/64 (what) [first got G want W]` (N -1: the port gave up).
2. **Apple's write forms** [apple-RE], at the first pool address that read wrong (the drift base if none did), each its own write, readback and restore (`bcm_video_wr_variant`, a probe-only helper; the shared paths are unchanged): `addr16` (both address halves to `WR_ADDR` `0x30010000`, low then high), `data16` (halfword data stores), `nopoll` (no `CONTROL` poll after the address), and `all` three. Lines: `cr1 variant ours|addr16|data16|nopoll|all at 0x%06lx wrong=N/64`; then, if any form landed where ours didn't, `cr1: variants landing where ours doesn't: <names>`: a PP-side-only fix, no image change.
3. **Slots**, by the CPU, then by engine 2 (rows of 256 bytes and up; shorter rows go by the CPU, the port's rule): `J_CLS_DRIFT` 300 × 300 (pitch 608), `J_CLS_STRIP` 400 × 160, 301 × 7 (odd width; pitch 602 before 2026-10-25, 608 since: every class's pitch is rounded to 16 pixels) and 8 × 1. Each: `ALLOC`, rows written as `bcmjob_upload` writes them (since 2026-10-25 `write_row`: an odd first or last pixel merged into its word by read-modify-write; before, the even part as a block and an odd last pixel as a word, which put odd rows of an odd pitch one pixel early), flush, **every pixel read back** by the port, `COMMIT` with the PP's `j_f16w`; on `J_E_SUM`, a second `COMMIT` with `J_COMMIT_NOCHECK` returns the BCM's own sums. Lines:
   - `cr1 <class> WxH CPU|E2: slot S at A pitch P`
   - up to four `cr1 bad @A got G want W (x X, y Y)`
   - `cr1 <class> WxH CPU|E2: bad px N (words M); commit st=S n=N r=R0 R1, ours A B`
   - on `J_E_SUM`: `cr1 <class> WxH CPU|E2: unchecked st=S, the BCM's sums A B, readback's A B`
   - `cr1 <class> WxH CPU|E2: PASS`, or `FAIL (write path: readback wrong)`, `FAIL (BCM-side sum: readback right, J_E_SUM)`, `FAIL (COMMIT refused, readback right)`, `FAIL at alloc|write|flush|readback st=S`.

**The verdict** (the last line, `cr1: … (control C wrong, pools N of 7 wrong; slots P pass, W write, S sum, O other)`):

| Verdict | Means | Next |
|---|---|---|
| `cr1: PASS` | every readback right and every `COMMIT` accepted | the failures were elsewhere (the merged image's job path, the drift's own upload): compare with the UI log's `bcmjob fault op=0x22 st=…` |
| `cr1: FAIL: write path` | the control (the host frame) reads back wrong too, or a slot's pixels do while every region reads right | the PP's write path itself: compare the variants; any that land are the fix |
| `cr1: FAIL: region not writable after start` | the control is right but a pool reads wrong | SRAM below `0xD0000` is not host-writable after the start (or not as we write it): the variants say whether Apple's form reaches it; otherwise the pools move (an image change, a later decision) |
| `cr1: FAIL: BCM-side sum` | every readback right, `COMMIT` answers `J_E_SUM` (23) | the BCM's F16w differs from `j_f16w`: the unchecked `COMMIT`'s sums against ours and the readback's locate it |
| `cr1: FAIL: other` | another status or step | the line names it |

**[sim]:** not modelled (the question is silicon's). The bench session logic (`portcheck` mode, the `cr1` step) is in `tests/autochecks`.

**Sitting 1 (2026-10-25, `bench-20261025-1819`)** [silicon]: control and all 7 pools 0 wrong, every Apple form 0 wrong; slots 1 pass, 2 write (301×7 by CPU and E2: rows at 2 mod 4, `quirks.md` #34), 5 sum (every readback right, the BCM's F16w wrong: #33). **Expected in sitting 2** (the batch): `cr1: PASS (control 0 wrong, pools 0 of 7 wrong; slots 8 pass, 0 write, 0 sum, 0 other)`, the 301×7 slot at pitch 608.

## CR2: staged commit (withdrawn 2026-10-25)

CR2 ran once (sitting 1, `bench-20261025-1819`) and is removed with the staged candidates (`merged-image-batch.md`). Its direct uploads gave the halfword-load fit (`../../bcm2722-reference/quirks.md` #33): `cr2 disp 8x1 direct: pp 0007fff8,0023ffdc bcm 0000fff8,0007ffdc` is `0xFFFF` plus seven −1. Staging has no reason now: the pools take host writes after the start (CR1, 0 of 7 wrong).

## HWLD: the halfword load forms (stages 4 and 4b, the timing bench)

**What runs:** eight informational checks in the bench image's vector-check records (tags `HL16`, `HS16`, `H28O`, `H29O`, `H48H`, `H48S`, `HSP0`, `HSP1`; `gen_bench.py` `HWLD_FORMS`). Each loads the halfwords `0x8001`, `0xFFFF`, `0xFFFF`, `0x8001` from `+0`, `+2`, `+4`, `+6` through one form (encoding pinned as raw halfwords) and stores the 32-bit register: the 16-bit `ldh (rs)` and `ldsh (rs)`, the 32-bit `0x28`/o1 and `0x29`/o1, the 48-bit op 2 and op 6, and the sp-relative `2d4x`/o1 and `2d5x`/o1.

**Lines:** `B4V k TAG flags got0 got1 got2 got3 bad want` and `B4VL k ...`; `readlog.py` names each: `H29O 0x29/o1 (rs+off) sign-extends (as modelled)`, or `... zero-extends: NOT as modelled (the model sign-extends)`, or `neither reading: ...`. `bench_ingest` lists them among its CHECKS.

**Expected (the model)** [silicon for the first four]:

| Tag | Form | got0..3 | Reading |
|---|---|---|---|
| `HL16` | 16-bit `ldh (rs)` | `00008001 0000ffff 0000ffff 00008001` | zero-extends |
| `HS16` | 16-bit `ldsh (rs)` | `ffff8001 ffffffff ffffffff ffff8001` | sign-extends |
| `H28O` | `0x28`/o1 | `00008001 0000ffff 0000ffff 00008001` | zero-extends |
| `H29O` | `0x29`/o1 | `ffff8001 ffffffff ffffffff ffff8001` | sign-extends |
| `H48H` | 48-bit op 2 ("ldh") | `00008001 0000ffff 0000ffff 00008001` | zero-extends [hypothesis] |
| `H48S` | 48-bit op 6 ("ldsh") | `ffff8001 ffffffff ffffffff ffff8001` | sign-extends [hypothesis] |
| `HSP0` | `2d4x`/o1, sp-relative | `00008001 0000ffff 0000ffff 00008001` | zero-extends [hypothesis] |
| `HSP1` | `2d5x`/o1, sp-relative | `ffff8001 ffffffff ffffffff ffff8001` | sign-extends [hypothesis] |

Informational: a surprise names itself in readlog and can't fail stage 4. vc2cc emits neither the 48-bit nor the sp-relative halfword loads until these come back.

**[sim]:** `bench_test.py` (the records pass under the model); `halfload_test.py` (the model and compiler).

## B6: texel fetch

**What runs:**
- The video image, with a 320 × 240 session open and no packets fed.
- `HP_OP_LAB` (`bcmfw/hostproto.c`) runs 262,144 iterations of a random-address halfword fetch over 64 KB of SRAM (`0x12B000`) and over 1 MB of SDRAM (`0xFE10_0000`, the DPB).
- It also runs the same arithmetic without the load. The difference is the fetch.
- At 27 MHz, then at 150 MHz when the 4b and 4c flags allow.

**Lines:**
- `lab6 <MHz> SRAM|SDRAM: arithmetic N us, with fetches M us for 262144: X.Y ns a fetch (Z cycles)`
- `lab6: PASS|FAIL (K clocks)`

## B7: memory bandwidth and contention

**What runs** (the same image and session):
- Word loads, stores and copies in order, 262,144 each, over the same regions.
- A run of 1,048,576 loads, first alone, then while the PP writes a full 153,600-byte frame through the port into the same memory: the host framebuffer (`0xE0000`) for SRAM, the bitstream FIFO for SDRAM.

**Lines:**
- `lab7 <MHz> SRAM|SDRAM: load N KB/s, store N KB/s, copy N KB/s (each way)`
- `lab7 <MHz> SRAM|SDRAM contention: loads A us alone, B us under the host's 153600 bytes (C us): X.Y % slower`
- `lab7: PASS|FAIL (K clocks)`

**Caveats:**
- The loops are C, so the per-word figures include the loop's own instructions. They are the core's achievable rate, not the memory's peak.
- SDRAM refresh isn't isolated. It shows up only as part of the SDRAM figures.

**[sim]:** `lab_test.py`. The op's refusals, every kind's checksum against a Python model, and stores and copies read back, in both memories.

## The UI on the COP (19-9, branch `p19-ui-cop`): `c1`, `ucop`, `ucop2`

One viability row, in three steps, run only after MF-4 has merged (`HAVE_FE_FB_HANDOFF`; before that the switch's `cop` is refused at boot and `ucop2` fails at once, saying so).
- **`c1` (B10, part A, automatic):** engine 2 started and polled from the COP (`devbcm_probes.c`, "COP row C1"). The UI's pushes from the COP depend on it; if it fails, stop here.
- **`ucop`:** sets the developer switch "UI thread" to `cop` for the next start; the owner restarts (MENU + SELECT).
- **MF-4 on silicon (during `ucop2`, next sitting):** the pixel hand-off is host-tested only. Watch for: the wake frame after the display goes dark and comes back (a stale or torn first frame means the COP's write-back did not land), `fe fb fence TIMEOUT` and `fb fence timeout: pushed uncached (MF-4)` lines (a few in the USB screen or a prompt are expected; any while browsing is a fault), and `fbto=` on the `ucop2` line. A video overlay, if the video build is on the unit. Engine 2 started from the COP is `c1` (B10).
- **`ucop2` (2 min soak):** the owner browses, plays an MP3, opens Now Playing and scrolls Songs fast; the question comes first on return. It **fails** on any watchdog stall, any coherence canary (`fe coh fail`), any pixel fence timeout (`fe fb fence TIMEOUT`), a call to the back end over 250 ms, or under 100 s of use; the line `ucop2 s= calls= rcalls= max_us= stalls= coh= fbto=` records the counters. Latency: the `uilat np/cpu/fr` lines (np-lag's probe; `cpu ... ui=cop cop_idle=`) for the same minutes.
- **Safety:** the first start on the COP sets the next start back to the CPU until the attempt has run 120 s with no stall (`fe cop attempt`, `fe cop held`); a stall sets it back (`fe FALLBACK`); a failed soak, or any answer but "Normal", sets the switch to `cpu`. A pass leaves the switch on `cop`: the owner keeps using it, and the logs carry `uilat` and `fe` lines.

## B13: warm swap

See `warm-swap.md` §7.

**Sessions 2026-10-25 (builds `4e57b3b9a1`, `caaa9890b0`, sitting 2's `799ff0f8d7`): FAIL, never parked** [silicon]. `PARK` answered `HP_OK` with flags 3; the stub's "PRK" never reached `0x13C` (10 ms, then 60 ms on sitting 2), and the image then ignored the stock power-down (`power-down: no answer, cutting power`: the black flash the owner saw; no squares). **Root cause** [hypothesis, strong; `warm-swap.md` §8, `quirks.md` #40]: the park called its stub straight after copying it with data stores, with no cache flush; Apple never runs code it has written without its whole-cache flush (`vmcs.bin` @25a, called at @88a0 and @91a4). Every other step was checked against the code, the listing and vc2sim and ruled out. **Fixed** on `p19-b13` in every image (`hp_code_flush`), with diagnostics. The merged image changes (`80bc8611` → `88e710cb`), so the re-verify runs on the new one.

**Sitting 3 (2026-10-26, build `7e8c140e40`): it parks; FAIL at round 9, "never ready"** [silicon]: rounds 0–8 passed both ways; round 9 (to display by CPU) got no ready word in 100 ms, with no diagnostics. Reading and fix: `quirks.md` #41, `warm-swap.md` §9 (the host's flush in Apple's running-core form; `warm N: diag`, `diag park` and `diag flush … upload read back` lines at every failure). The next sitting: 20 of 20, or the failing round's diag lines.

**What the next sitting must show** (B13 as it is, with `jobs_display` `05c1e914` and the merged image `88e710cb`):
- **Round 0** (the display-only image parks, with the flush): parks, `warm 0: … PASS`, and `warm 0: diag parked, prog 5706xxxx` (step 6; the detail is the flush's re-requests << 12 | polls: `0000` or a few polls says the flush ran normally, `f000` says bit 0 never read clear and the park went on).
- **Round 1** (the merged image parks, also flushed) and every later round: park and pass; `warm 1: diag parked, prog 5706xxxx`. `warm: PASS` (20 of 20, the fault handled), no flicker, the squares stepping along.
- **If round 0 still fails**, read its two `warm 0: diag` lines:

| `prog` step | stub words | state then … | Reading |
|---|---|---|---|
| 6 (`5706…`) | `013c2e61 2f62b910 0057474f ce03b810` (the stub) | unchanged | the core called the stub and its first store never landed: the fetch from `0xDE900` (move the stub low, below `0x40000`) |
| 4 or 5 | the stub | unchanged | stopped in the flush: bit 0's behaviour (compare `cache`) |
| 1 or 2 | anything | unchanged | stopped in `lcd_blit_wait` (compare `lcd dma cs/int`) |
| not `57…` | not the stub | moving | `park()` never ran: the image is still in its loop |
| any | any | any, with `ready` = `fa17…` and a `fault pc` | an exception: the pc names the instruction |

## If an entry fails

**The summary shows:**
- the step that failed;
- the first failing round, for B13.

**The log has the line.** Report both. A failure in B13 or B5 is a silicon finding for `quirks.md`.

## The job driver (P19 W0-5/6, branch `p19-w0-56`): device checks

**Needs:**
- a bench build that merges `p19-w0-56`;
- a Developer row that calls `bcmjob_selftest()` (none exists in W0; the staged build adds it);
- the jobs gate open, by either:
  - the display image's FNV in `verified.txt`, or
  - the developer marker file `bcmjobs.on` in the hidden folder (a file, never an on-screen string; the Bench session sets and removes it).

**What to check:**
- `HELLO` through the ring and the doorbell;
- the port lock against the tick's reads;
- `lcd_e2_push` slot uploads;
- G2's restart blank time;
- the heartbeat within 3 ms;
- suspend and resume keeping the rings consistent;
- a dark want against the backlight thread;
- the self-test's readback at `0x105800`.

**Log lines** (`ui_log`; `bcmjob-core.c`, `bcmjob-dev.c` at `a5316a7e17`):

| Line | Notes |
|---|---|
| `bcmjob init allowed=… verified=…` | |
| `bcmjob up epoch=… ops=…` | |
| `bcmjob cpu why=hello\|job block\|no jobs caps=…\|protocol\|rings` | |
| `bcmjob submit op=…` and `bcmjob fence seq=…` | verbose only |
| `bcmjob fault op=… why=busy\|dead\|failed err=…` | |
| `bcmjob off why=second failure` | |
| `bcmjob resync x= y= w= h=` | |
| `bcmjob selftest ok\|failed rc=…` | |
| `bcm power state=C0 why=dark\|fault\|…` | |
| `bcm power state=C1 why=want-…` | |
| `bcm power want why=… hold=…` | |
| `bcm power ups dark= decode= game= video=` | hourly, on the next API call |

**Passes when:**
- `bcmjob selftest ok`;
- `bcmjob up` with this image's ops;
- no `bcmjob fault` and no `bcmjob off` over the row's run;
- `bcm power ups dark=0` in dark play.

## The brief probes, wave 1 (branch `p19-pb-probes`)

**What this is:** the read-only and reversible stages of `../../bcm2722-reference/brief-map-peripherals.md` §11 (PB-1 to PB-5) and `../../bcm2722-reference/brief-map-power.md` §9 (PW1, PW2, PW7, PW8), as Developer › Advanced › Video chip › Probes rows. **Not built:** PW5 and PW6 (hibernate, bypass); they need the owner's go. PB-6 (Apple's own TV pattern, host side only) is built (branch `p19-pb6`, card below) and not run. PB-7 to PB-9 (the TV) are built on both sides (branch `p19-tv-rows`, cards below); they switch a clock on and drive the TV output, so each has its own confirm and is locked behind the row before it. PW3 and PW4 (the core's voltage and clock, at Apple's values) are built on both sides (branch `p19-pw34`, cards below), with the same confirm and lock; running them needs the owner's go for wave 2.

**How they run:**
- Each row needs a stage 5 pass flag and the probe image file (`bcmprobe.bin`, the plain video image; p19-session-img), and asks for SELECT on a confirm screen that says what it writes.
- Each boot is `vplayer_probe()` (`apps/video/vplayer.c`): the BCM taken from the display driver, the panel ID read through the port, the probe image booted, the stage's steps, then a **power cut** (`bcm_video_cut()`, ≥ 50 ms off) and the stock wake path. A row with owner steps boots once per step; the prompt is on the lit screen between boots.
- **Canary**, first in every boot: `0x1000_1804` read twice 100 µs apart (90–110), and the panel ID (`0x1000_2824` bits 19:18) equal to what the port read before the boot. A failed canary voids the run (`<prefix> canary …: FAIL (run void)`).
- Every register is read twice; two different values log as `volatile`.
- A request with no reply in 2 s is a hang: `<prefix>: hang at step …`, then the power cut.
- During PB-2 and PB-3 the USB mode is charge-only, so a cable in never takes the disk; it goes back to the default at the end.
- The image side: `HP_OP_PEEK` (0x13), `HP_OP_PULL` (0x15), `HP_OP_CACHE` (0x16), `HP_OP_DMACPY` (0x17) and `HP_OP_PINS`' four-word form (`bcmfw/hostproto.h`), only in the plain video image, not the merged one. The rows always load it from `bcmprobe.bin`, whichever image the build gives the player as `bcmvideo.bin`.

| Stage | Row | Time | Prefix | What the owner does |
|---|---|---|---|---|
| PB-1 | **BCM probe PB-1: register census** | < 1 min | `pb1:` | nothing |
| PB-2 | **BCM probe PB-2: pin levels (on cue)** | ~5 min | `pb2:` | seven states, SELECT after each (below) |
| PB-3 | **BCM probe PB-3: USB status (cable on cue)** | ~1 min | `pb3:` | USB-C in, SELECT; out, SELECT |
| PB-4 | **BCM probe PB-4: pin pulls** | < 1 min | `pb4:` | nothing |
| PB-5 | **BCM test: stage 8a-1** (existing row) | as 8a-1 | `8a1:` | as 8a-1 |
| PW1 | **BCM probe PW1: power census** | < 1 min | `pw1` | nothing |
| PW2 | **BCM probe PW2: caches** | ~1 min | `pw2` | nothing |
| PW7 | **BCM probe PW7: second port bank** | < 1 min | `pw7` | nothing |
| PW8 | **BCM probe PW8: DMA copies** | < 1 min | `pw8` | nothing |
| PB-6 | **BCM probe PB-6: TV with Apple's own pattern** | < 1 min | `pb6:` | AV cable and TV; pick NTSC or PAL, watch 10 s, say what showed (no lock; no stage 5 flag or video image needed) |
| PB-7 | **BCM probe PB-7: TV timing (no output)** | < 1 min | `pb7:` | SELECT on the confirm; nothing plugged in |
| PB-8 | **BCM probe PB-8: TV output (AV cable and TV)** | ~1 min an arm | `pb8:` | AV cable and TV; pick an arm, watch, say what showed (locked until PB-7 passes) |
| PB-9 | **BCM probe PB-9: TV width 640 vs 704** | ~2 min | `pb9:` | as PB-8; say how each width looked (locked until a PB-8 arm showed bars) |
| PW3 | **BCM probe PW3: core voltage ladder** | ~90 s | `pw3:` | SELECT on the confirm |
| PW4 | **BCM probe PW4: clock and voltage ladder** | ~1 min | `pw4:` | SELECT on the confirm (locked until PW3 passes; needs stage 4b's pass flag) |

### PB-7 to PB-9, image side (branch `p19-probes2`): built, not run

`HP_OP_TV` (0x14) in the plain video image only (`bcmfw/hostproto.c` `cmd_tv`; the merged image still answers `HP_E_UNKNOWN`, byte-identical). The PP rows that send it are the cards PB-7, PB-8 and PB-9 below.
- **Off by default:** `HP_TV_UNLOCK` returns a one-time code; the next TIMING or OUTPUT request uses it up whether it matches or not. Unarmed: `HP_E_UNSUPPORTED`; wrong code or malformed: `HP_E_INVAL`; a session open: `HP_E_BUSY`; nothing written in any refusal.
- **TIMING (PB-7):** pixel clock, `0x2C08`–`0x2C18`, `0x2C00`; then a window of 1–1000 ms counting `0x2C18` bit 31 changes. Never the DAC, GPIO 19 or DMA (checked per write).
- **OUTPUT (PB-8, PB-9):** adds the Macrovision off set (only with `0x5404` bit 0 set), GPIO 19 (function to output if needed, then high), the DAC (`0xA0`/`0xE0`), the start waits on `0x2C00` bits 31 then 30 (10 ms each), and DMA channel 6 feeding one self-linked line from SRAM `0x12B000` (format: UYVY, YUYV, RGB565 or a Y word; pattern: 8 bars or a line every 32 pixels; input width 640 or 704) for 1–8000 ms.
- **Every exit** (done, a start-wait timeout, the chain found stopped): the off order, then each saved register written back and read back (`HP_TV_R_MISMATCH`).
- **Progress:** `0x138` = `'T' << 24 | phase << 16 | ms`; the image keeps its heartbeat during the window.
- vc2sim: `bcmfw/tests/peek_test.py --only tv`.

### PB-1: census

**Reads** (PEEK, one address a request, so a hang names its address): pass 1a `0x1000_2C18`, `5404`, `6400`, `1C3C`, `4800`, `2820`, `2824`; pass 1b `2C00`, `2C08`–`2C14`, `5484`–`54DC` (23), `1C28`–`1C58` (13), `4810`, `4814`, `2800`–`280C`, `2860`. At 27 MHz, then at 150 MHz in an `OPEN` with `HP_OPEN_FAST_CLOCK` when the 4b/4c flags allow.

**Lines:** `pb1: <clock> <addr> <v1> <v2|volatile>`; `pb1: done <clock> 1a=<n> 1b=<n>`; `pb1: PASS|FAIL`.

**Passes when:** all 7 + 48 words are logged at each clock run, canaries good. The summary shows the signatures: `5404` bit 0, `2C18` bit 31 static or toggling, `1C30`, `6400` bit 24.

### PB-2: pin census

**States, in order** (a prompt each; SELECT when done): idle (nothing plugged in, Hold off); music (the PP plays silence through its codec, so its clocks run; no headphones needed); USB-C in; USB-C out; AV cable in the jack; AV out, headphones in; Hold on (after SELECT, slide Hold on: the sample runs 5 s later; then Hold off and SELECT). Backlight off with the BCM up is skipped: this build cuts the BCM with the backlight.

**Each state:** `HP_OP_PINS` with four words (masks 0–31 and 32–51, 500 ms, flags 0): AND and OR of all 52 levels, edges per pin.

**Lines:** `pb2: <state> and=<w0>,<w1> or=<w0>,<w1> edges=<pin>:<n>,…` (non-zero pins only); `pb2: wired gpio<p> by <state>`; `pb2: PASS|FAIL`.

**Passes when:** all seven states sampled, and the known pins read as expected in each: 2/3 edges only in "music", 49 toggling, 50/51 steady and equal to the panel ID, 48 high. A pin whose level or edges differ from idle in exactly one state is logged as wired to it.

**Fixed before any run** (`p19-tv-rows`) [ours]: the PP's port driver kept at most 16 words of a reply (`BVP_REPLY_WORDS`), so the 19-word census came back short and every state would have logged `pb2: census refused`. It now keeps the whole payload (31 words). A PB-2 run from a build before that fix is void.

### PB-3: USB status

**Reads:** `0x1000_1C3C` and `0x1000_6400` twice, with the cable in, then out; the PP's own `usb_inserted()` beside them.

**Lines:** `pb3: usb=<in|out> step=1 1c3c=<v> 6400=<v> (pp sees in|out)`; `pb3: PASS|FAIL: bit 24 follows the cable|unchanged`.

**Passes when:** both reads came. Bit 24 following the cable means the BCM's USB block sees the connector; unchanged (expected) means not wired. **Step 2 (R1, the clock-gate toggle) is not built:** it writes `0x1000_1C3C`.

### PB-4: pull test

**What runs:** `HP_OP_PULL`, 8 pins a request (50 ms bound), pins 0–47. Per allowlisted input (0, 4–18, 20–22, 24–47; a pin whose function isn't input is skipped): pull up (mode 2, Apple's clock-word order), 1 ms, two reads; pull down (mode 1), 1 ms, two reads. The down pull is also the restore. Never pulled: 1, 2, 3, 19, 23, 48–51. Then 50/51 read without a pull, as the control.

**Lines:** `pb4: gpio<p> up=<l,l> down=<l,l> -> float|high|low|volatile`; `pb4: gpio<p> skipped (never pulled|not an input)`; `pb4: control gpio50=… gpio51=…`; `pb4: PASS|FAIL: float …, high …`.

**Passes when:** all six requests answered and the 50/51 control steady and equal to the panel ID. (The spec asks 50/51 to read high/low against the pulls, but also forbids pulling them; the build keeps the prohibition and reads them unpulled.)

### PB-5: audio clock words in 8a-1

`HP_OP_TONE`'s reply now ends with `0x1000_1C34` and `0x1000_1C38` read before the rate, while playing and after the close. One line per layout: `8a1: layout <n> clocks 1c34/1c38 before the rate …, playing …, after close …`. No new writes.

### PW1: power census

**Contexts:** A, whatever the take found (Apple's `vmcs` after stage 0, logged `A`; our resident image, logged `R`); B, the video image idle at 27 MHz; C, the same in an `OPEN` at 150 MHz when the 4b/4c flags allow (the spec names stage 4b's image; the video image's 150 MHz session is used instead).

**Reads** (through the port, each bounded): `0x1000_1400`; `0x1000_1C00`–`1C58` (23); `0x1000_0C00`, `0400`, `0408`; `0x1000_3410`, `3414`, `3420`, `3424`, `3428`, `3430`; `0x1000_1804` twice, 1,000 µs apart on the PP's timer; the PP's `GPO32_VAL`, `GPO32_ENABLE`, `0x7000_0088`, `STRAP_OPT_A`, `0x7000_0040`. Context A only: 200 samples 10 ms apart of `1400`, `1C28`, `1C14`, the UI idle (the scripted scroll isn't possible while the BCM is held). In B and C the same words are read again by PEEK, the core's view, and any word the port reads differently is logged.

**Lines:** `pw1 <ctx> reg 10001400 = … (level N, hi H)`; `pw1 <ctx> clk +34 = … src S (name) div D (off)`; `pw1 <ctx> pll +00 (A) m=… n=… on|off f=… MHz`; `pw1 <ctx> ctr delta N us`; `pw1 <ctx> gpo32 val=… en=… rb=… strap=… 40=…`; `pw1 A samples (idle, 2 s): …`; `pw1 <ctx> port/peek differ …`; `pw1: PASS|FAIL (N contexts, …)`.

**Passes when:** every read completed and the counter delta is 1,000 ± 5 µs in B and C.

### PW2: caches

**What runs:** an `OPEN` at 27 MHz (SDRAM up); the PP fills 16 KB at `0x12_B000`, 16 KB at `0xFE3F_6000` and 1 MB at `0xFE10_0000` with a seeded pattern and computes the checksums. For each cache word 0, `0x8000_0000`, `0x8001_0000` (Apple's values only): `HP_OP_CACHE` TIME per region (Apple's flush, the word, 262,144 loads cold then warm, flush, ours back), then the coherence handshake in SRAM (`0x12_BFF0`) and SDRAM (`0xFE3F_FFF0`) on the step word `0x12_B800`, 100 ms a step. The op refuses unless the word reads `0x0001_0000` first.

**Lines:** `pw2 cc=<w> SRAM16K|SDRAM16K|SDRAM1M cold N us warm M us, sums ok|WRONG, flush polls P`; `pw2 coh cc=<w> SRAM|SDRAM core-after-host fresh|stale, host-after-core fresh|stale, after-flush fresh|stale (timeouts T)`; `pw2: PASS|FAIL`.

**Passes when:** every checksum matches (cold and warm) under all three words. A wrong sum stops the stage at once (data corrupted). Timing and coherence are results. A step timeout counts as stale.

### PW7: the ALT bank

**What runs** (`bcm_video_alt_lab()`, the video image idle): 256 halfwords by the main bank to `0x12_C000`; 256 by the ALT bank to `0x12_C400`, the address by halves (the spec's `0xC000` low half would land on the main bank's area; `0xC400` matches its area list); interleaved main `0x12_C800` and ALT `0x12_CC00`; all four areas read back by both banks; a 153,600-byte frame into `0x0E_0000` by each bank with the 2-register `stm` loop. Every handshake 1 ms; no `CONTROL` write.

**Lines:** `pw7 … by the main bank: a b c d; by the ALT bank: a b c d; alt ready seen yes|no, frame main X us alt Y us`; `pw7: PASS|FAIL (rc, step)`.

**Passes when:** all four areas read 256/256 by both banks.

### PW8: DMA copies

**What runs:** an `OPEN` (SDRAM up); `HP_OP_DMACPY` three times: SRAM `0x12_B000` → SRAM `0x12_F000`, SRAM → SDRAM `0xFE3F_6000`, SDRAM → SRAM. Channel 7 sub-channel 1, Apple's words (config `0x0000_E802`, block `{0, src, dst, 512, 0x0102_5151}`), done on status bit 14, 100 ms bound; then the core's word loop over the same copy. **16 KB, not the spec's 64 KB:** 64 KB from `0x12_B000` would overlap its own destination and the parked AAC tables at `0x13_4000`, and 64 KB at `0xFE3F_6000` runs past the 4 MB of SDRAM.

**Lines:** `pw8 SRAM>SRAM|SRAM>SDRAM|SDRAM>SRAM dma N us core M us, wrong W (core C), status S[, DMA TIMED OUT]`; `pw8: PASS|FAIL`.

**Passes when:** 0 wrong words in every copy and no timeout. Apple's config word leaves bit 0 (the enable in our reading, `dma.md`) clear; a timeout here is a finding about that bit, not a fault.

**[sim]:** `bcmfw/tests/peek_test.py`: PEEK's allowlist against every address the specs name and the never-read list, refusals reading nothing, the bus's values, the access list equal to the request; TV refusing every step with no TV, DAC or GPIO 19 write; the census's edges and levels with no GPIO write; PULL on allowlisted inputs only, Apple's register order, each pin left pulled down; CACHE's checksums under all three words, only Apple's values written and ours put back, the handshake (vc2sim has no cache: always fresh); DMACPY's three copies with 0 wrong words. `video_test.py`'s tone row checks PB-5's six words and that the clock words are never written.

## The TV-out probes: PB-7, PB-8, PB-9 (branch `p19-tv-rows`)

**What this is:** the PP rows for `HP_OP_TV` (above; spec in `../../bcm2722-reference/brief-map-peripherals.md` §11). TV out is a 5G feature Rockbox never had; these three rows answer whether our own image can bring it back on the owner's unit, and how. **Built, not run on silicon.**

**How each run goes** (`apps/ipodui/devbcm_probes.c`; the pure logic in `apps/ipodui/devbcm_tv.h`, tested by `tests/tvprobe/`) [ours]:
- One `vplayer_probe()` boot of the plain video image per run (PB-7 runs both standards in one boot): the canary, then `HP_TV_UNLOCK` for a one-time code (2 s bound), then the step with that code.
- **Timeouts:** the PP waits for the requested window plus 2 × 10 ms (the start waits) plus 2 s, not the probes' usual 2 s. It reads the progress word (`0x138`) and the heartbeat (`HP_STATE`) every 10 ms; neither moving for 2 s is a hang (`<prefix> … hang at phase P, M ms`).
- **The screen:** every prompt is on the lit screen before the boot or after the power cut. Nothing draws while the BCM holds the display, so the window is never touched.
- **Clean-up:** the image writes back every register it touched and reads them back (the mismatch mask); then the PP cuts the BCM's power, frees the image and brings the display back by the stock wake path, on every path, error or not. Nothing of the video image stays resident.
- **Locks, this boot only:** PB-8 opens once PB-7 passed for both NTSC and PAL; PB-9 opens once a PB-8 arm whose window ran was marked "Colour bars". A locked row says what to run first.
- **Lines:** every reply word, as `<prefix> <what> st=… words=29 exit=… phase=…: <check>`, `<prefix> <what> dac0=… mode0=… fields=… us=… clk=… flags=… mismatch=…` and two `ctl/dma` lines of five samples each.

### PB-6: TV with Apple's own pattern (branch `p19-pb6`)

**What this is:** brief-map-peripherals.md's PB-6, the one TV probe that runs **only Apple's code**: does the owner's headphone jack still carry composite video? Host side only; no image of ours, no stage 5 flag, no video image file. **Built, not run on silicon.**

**Owner:** the AV cable in the headphone jack (**no headphones**), a TV on its AV input. SELECT on the confirm, pick **NTSC** or **PAL** (the TV's own), watch the TV for 10 s (SELECT ends it early); after the screen comes back, answer: **Colour bars**, **A grey or blank picture**, **No signal** or **Snow or a rolling picture**.

**What runs** (`devbcm_pb6()` in `apps/ipodui/devbcm_probes.c`, `bcm_devtest_tv_pattern()` in `bcm-devtest-hal.c`; the pure logic in `apps/ipodui/devbcm_pb6.h`, tested by `tests/pb6probe/`) [ours]:
1. If our image drives the display, it steps down to Apple's NOR `vmcs` through the stock wake path, for the rest of this boot (the device test's fallback; log `pb6: our image drove the display: …`). Still ours after that: the row refuses (rc −3), nothing touched.
2. A 230,456-byte `core_alloc` block (pinned for the run, freed on every path) holds the BMP; the BCM is taken from the display driver (`dt_take`; refused: rc −1, nothing touched).
3. The BMP to host `0xC020_0000` through the port; its first two words read back (`head`, checked against the header).
4. `TV_NTSCBMP` (`0xFFFC_0003`) or `TV_PALBMP` (`0xFFFD_0002`) to `0x1F8`, the doorbell, then `0x1F8` polled until it is neither the command nor `0xFFFF` (`bcm_command()`'s rule), bounded at 2 s; then `TV_MVOFF` (`0xFFF1_000E`) the same way.
5. Hold 10 s.
6. **Every path once taken:** power cut (≥ 50 ms off), then the stock wake (`dt_restore(false)`: `bcm_init` from NOR, Apple's `vmcs`, the screen repainted). The owner answers after it.

**The BMP** [hypothesis: the format Apple's command expects is unknown]: `BITMAPFILEHEADER` (14 bytes: `BM`, size 230,454, offset 54) + `BITMAPINFOHEADER` (40 bytes: 320 × 240, 1 plane, 24 bits, `BI_RGB`, 230,400 bytes of pixels, 2835 px/m), **bottom-up** (positive height), **B, G, R** per pixel, 960-byte rows (a multiple of 4: no padding), padded with two zero bytes to a whole word for the port. The plainest Windows BMP, the one a minimal decoder is most likely to take. The picture: eight vertical 40-pixel bars at 75 % (white, yellow, cyan, green, magenta, red, blue, black), the same on every row, so an upside-down reading still shows bars.

**Lines:** `pb6: head <w0> <w1> (ok|MISMATCH); bmp 0x1F8=<v> in <us> us; mvoff <ack|timeout|not sent> 0x1F8=<v> in <us> us; held <ms> ms`, then the spec's `pb6: std=<ntsc|pal> cmd=<ack|timeout> owner=<bars|grey|nothing|noise|none>`, then the verdict line. A run that never sent the command: `pb6: std=… not run: <why> (rc, upload, head)`.

**What it means:**
- **Bars:** the jack carries video on this unit, and Apple's TV path works: **PB-8 is worth running** (PB-8 itself still opens only after PB-7).
- **Grey or noise:** a signal reaches the TV, so the jack is wired; the BMP guess or the standard is wrong. Try the other standard once, then PB-8.
- **Nothing** with `cmd=ack`: unclear (the jack, the BMP or the standard). Try the other standard, then PB-8.
- **`cmd=timeout`** and nothing: Apple's `vmcs` didn't take the command (or the port failed); nothing learnt about the jack.
- **`head` MISMATCH:** the upload didn't land at `0xC020_0000`; a finding for `host-driver.md`.

**Safety:** only Apple's code drives the pins, as on a stock iPod. The hold is 10 s; every exit after the take is a power cut and the stock wake.

### PB-7: TV timing (no output)

**Owner:** SELECT on the confirm. Nothing plugged in; nothing to watch.

**What runs:** TIMING for NTSC, then for PAL, 500 ms each: the pixel clock, the timing words and the control word; never the DAC, GPIO 19 or the DMA. The spec's window is 1 s; 500 ms still separates a field rate from a frame rate and NTSC from PAL (tolerance 2 % plus one count).

**Lines:** the reply words above, then `pb7: std=<ntsc|pal>: PASS|FAIL, N changes in U us (<rate>/s, field rate|frame rate|neither rate|no changes)`, then `pb7: PASS|FAIL (rc R); PB-8 unlocked|stays locked`.

**Passes when**, for both standards: the window ran to its end (`exit=0`, `st=0`), the mismatch mask is 0, no write was denied, and bit 31 of `0x2C18` changed at the standard's field rate (59.94 or 50 a second) or half of it.

**What it means for TV out:**
- **Field rate:** the generator runs at BT.601 timing from our image, and bit 31 is the field bit (vsync for the TV path's feeder). Go to PB-8.
- **Frame rate:** the same, with bit 31 a frame bit. Go to PB-8; the feeder counts frames.
- **No changes** with a clean restore: the generator didn't start from these words. Compare `ctl` in the samples with Apple's run (`brief-map-peripherals.md` §2.3); the missing piece is a clock or an enable outside the table.
- **Neither rate:** it runs, at another clock. The pixel clock word (`clk=`) is the first suspect.
- **Mismatch or denied write:** a finding for `quirks.md` (a register that won't take its old value back), and PB-8 stays locked.

**Stack:** the probe rows' `struct bvp` (~1.9 KB) comes from a pinned `core_alloc` block for the run (p19-bvp-heap), not the UI stack; their frames are 24–544 B.

### PB-8: TV output (AV cable and TV)

**Owner:** the AV cable in the headphone jack (no headphones), a TV on its AV input. SELECT on the confirm, pick one arm (NTSC or PAL × UYVY, YUYV, RGB565, Y word), watch the TV for 4 s, then answer: **Colour bars**, **A garbled picture** or **Nothing**. One arm a press; run it again for the next.

**What runs:** OUTPUT with eight colour bars for 4 s: Apple's on order (the Macrovision off set if `0x5404` bit 0 is set, the clock, GPIO 19 high, the DAC, the timing words, the control word), the two start waits, then DMA channel 6 feeding one line from SRAM.

**Lines:** the reply words, then `pb8: std=… arm=… run=<check> owner=<bars|garbled|nothing|no answer> (rc R); PB-9 unlocked|stays locked`.

**What it means for TV out:**
- **Bars, right colours:** that arm's FIFO format and standard are the ones to build on: TV out works on this unit from our image. PB-9 opens.
- **Garbled** (shapes or wrong colours): the signal reaches the TV, the format is wrong. Try the other arms; a byte order mix-up shows as wrong colours in one of UYVY/YUYV.
- **Nothing in every arm**, with `exit=0` and the samples showing channel 6 moving: the jack may not carry video on this unit (PB-6, Apple's own pattern, settles that), or the DMA isn't paced to the FIFO. Read Apple's band writer next (@7C9A's callers).
- **`exit=1` or `2`** (a start wait timed out): the generator didn't report ready; compare with PB-7's samples.
- **`exit=3`** (the chain stopped): channel 6 stopped in the window; a finding for `dma.md`.

### PB-9: TV width 640 vs 704

**Owner:** as PB-8. SELECT on the confirm; then, for each width, SELECT on the cue and watch the TV for 4 s, then say how it looked: **Lines reach both edges**, **A band or border at a side**, **A garbled picture** or **Nothing**. Last, pick which looked right: **640**, **704**, **Both looked the same** or **Neither**.

**What runs:** OUTPUT in the arm that showed bars, with a 1-pixel line every 32 pixels, the input 640 wide (output 704), then 704 wide. Only the 640/704 pair is built (the spec's 352 and 320 are left out).

**Lines:** the reply words for each run, `pb9: in=<640|704> run=<check> owner=<full|band|garbled|nothing>`, then `pb9: pick=… verdict=<scaler|window|unclear>`.

**What it means for TV out:**
- **640 reaches both edges, 704 too** (`scaler`): the TV path stretches 640 to 704 itself. Compose 640-wide bands, as Apple does; the scaling is free.
- **640 leaves a band, 704 fills** (`window`): no scaler. Compose 704-wide bands (the vector unit scales), or accept the border.
- **Anything else** (`unclear`): repeat once; then photograph both and log it as a finding.

## The power probes: PW3, PW4 (branch `p19-pw34`)

**What this is:** stages PW3 and PW4 of `../../bcm2722-reference/brief-map-power.md` §9, both sides. Our images run the BCM's core regulator at **level 11** at 27 MHz, where Apple's table and Apple's own running chip use **level 5** (`quirks.md` #26), and they never step the voltage with the clock as Apple does. Under the brief's 0.8–1.2 V, spread evenly over levels 5–15 [hypothesis], level 5 is about 40 % less core power than 11 [estimate]. PW3 asks whether our image holds at Apple's levels; PW4 whether it runs Apple's whole clock ladder at Apple's levels. **Built, not run on silicon.**

**The image side** (`HP_OP_PW`, `0x18`, plain video image only; `bcmfw/hostproto.c` `cmd_pw`, `bcmfw/pwsum.S`) [ours]:
- **Off by default**, as `HP_OP_TV`: `HP_PW_UNLOCK` hands out a one-time code; the next request uses it up, right or wrong. Unarmed `HP_E_UNSUPPORTED`; a wrong code or a malformed request `HP_E_INVAL`; a session open `HP_E_BUSY`; the counter stopped, the core off the crystal or the regulator outside levels 5–15 `HP_E_STATE`. Nothing is written in any refusal. The merged image answers `HP_E_UNKNOWN` and is byte-identical.
- **One request is one step**, and nothing outlives it: `LEVEL` sets one regulator level (5–15) on the crystal; `POINT` takes the core to one ladder point and its level. Request: `HP_PW_MAGIC`, step, code, level or point, hold ms (0–20,000).
- **Values: Apple's only**, compiled in, and each write is checked against the step's own value: the regulator `0xA5A5_0000 | (bits 7:4 as found) | level` (@141f0); PLL A `M = 108` (@4cb8), `N = VCO / 250 kHz` then `N | 0x400` after 1 ms; the core word `0x100 | divider`; the SDRAM refresh `f / 128,000` in bits 31:16 (@4d18). The ladder: 46 MHz (VCO 92, ÷2, level 5, refresh 359), 74 MHz (VCO 148, ÷2, level 7, 578), 102 MHz (÷1, level 9, 796), 150 MHz (÷1, level 15, 1171 = `0x493`).
- **Allowlist per step:** `LEVEL` writes `0x1000_1400` only; `POINT` adds PLL A (`0x1000_1C00`, `0x1000_1C14`), the core word `0x1000_1C28` and, with SDRAM up, the refresh `0x1000_3424`. A denied write is counted in the reply.
- **Order** (@5134's): a higher level before the clock goes up, a lower one after it; 10 ms after every regulator change. The refresh goes up after the clock.
- **Restore on every exit** (done, a register that didn't take, a wrong self-check, a stopped counter): the refresh back first, a higher start level before the clock comes down, the core word back, PLL A's two words back, a lower start level after; then every register written is read back (clock words and regulator without the password, `quirks.md` #25) into the mismatch mask.
- **The self-check:** 32 KB of SRAM at `0x12_B000` (64 KB would reach the parked AAC tables) filled by the core with one of two patterns, by turns, then summed by the core (every word) and by the vector unit (the low byte of each halfword, through bench row SUMR's three instructions, checked on silicon in session 4). References are taken at the start level, both patterns, and the vector sum must equal the core's sum of the same bytes before anything is written. At the point it runs over and over for the hold; the first wrong run ends it.
- **The 150 MHz point** pushes one frame with Apple's 150 MHz bus words (the image can't recompute timing words for other clocks, so the lower points push nothing), and puts our words back after the clock is down.
- **Waits:** every wait is on the 1 MHz counter and ends if the counter reads the same 1,024 times in a row (stopped). **Progress:** `0x138` = `'P' << 24 | phase << 16 | ms`; the heartbeat runs through the hold.
- **Reply:** 19 words (`HP_PW_R_*`): exit, last phase, the regulator at entry and at the point, the core word, PLL A, the refresh at entry and at the point, the reference and point sums, runs and wrong runs, one run's µs at the start and at the point, the frame's µs, flags with the denied count, the mismatch mask, the time held.

**The PP side** (`apps/ipodui/devbcm_probes.c`; logic in `devbcm_pw.h`, host tests in `tests/pwprobe/`) [ours]:
- One `vplayer_probe()` boot of the plain video image per row: the canary, then one `HP_PW_UNLOCK` (2 s bound) and one request per step. The steps stop at the first that isn't clean; the image has already put everything back; then the power cut and the stock wake path.
- **Timeouts:** the hold plus 2 s plus four settles and the PLL wait. The TV rows' watchdog: the progress word and the heartbeat read every 10 ms; neither moving for 2 s is a hang (`pw3: step K: hang at phase P, M ms`).
- **Lock, this boot only:** PW4 opens once PW3 passed. PW4 also needs stage 4b's pass flag (PLL A proven on this unit) and says so if it's missing.
- **Lines**, per step: `<prefix> <L=…|f=… level …> st=… exit=… phase=…: <check>`; `… reg … (was …) core … plla … refresh … (was …) flags … mismatch …`; `… runs N wrong W sums …/… ref …/… us U (ref U0) hold H us frame F us`; PW4 adds `pw4: f=… speed xS (clock alone xC): with the clock|slower than the clock|faster than the clock`.

**vc2sim** (`bcmfw/tests/peek_test.py --only pw`, 395 cases) [sim]: the refusals write nothing; each PW3 level and each PW4 point writes exactly its allowlist, in the expected order, with the level at or above Apple's table for the running clock and the refresh never too slow **after every single write**; everything back after a normal end, an injected read-back error (the regulator stuck at 11), a wrong vector sum at the point, a reference that disagrees (nothing written) and the counter stopping in the PLL wait and in a settle; the reference sums equal a Python model of both patterns; the 150 MHz point's frame and the bus words coming back; a hold runs both patterns. Two mutant images, one raising the level after the clock and one skipping the restore, fail those same checks.

### PW3: core voltage ladder

**Owner:** SELECT on the confirm; hands off for about 90 s (levels 11, 9, 7, 5, 20 s each). The screen shows the last frame throughout. No meter (owner, 2026-10-25): the row passes or fails on its own checks.

**What runs:** at 27 MHz, one `LEVEL` request for each of 11, 9, 7 and 5, each held 20 s with the self-check running throughout, each back to the start level before the reply.

**Lines:** the three lines above per level, then `pw3: PASS|FAIL at L, lowest good level G (rc R); PW4 unlocked|stays locked`.

**Passes when:** level 5 came back `ok`: every run right, nothing denied, the mismatch mask 0.

**What it means for battery life** (the voltages are an even spread of the brief's range [hypothesis], so these are estimates; the meter readings replace them):
- **PASS at 5:** our image holds where Apple's does. Running the BCM's UI image at level 5 instead of 11 cuts its core power by about 40 % (0.80 V against 1.04 V) for every second the screen is lit, with nothing the user sees changing. Next: a small change that sets level 5 when our image starts (and 15 only for a 150 MHz session, as today), then the meter's figures in `power.md`.
- **Lowest good 7:** about 28 % less than today; **9:** about 15 %. Use that level, and log the first failing one in `quirks.md`.
- **FAIL at 11** (today's level): not a voltage finding. The probe or the self-check is wrong; read the reply words before anything else.
- **`restore mismatch`:** the regulator didn't take its old value back: a `quirks.md` finding. The power cut clears it.
- **A hang** at a level: that level is too low for our image (or the write stalls the core). The cut clears it; the lowest good level stands.

### PW4: clock and voltage ladder

**Owner:** run PW3 first, in the same boot. SELECT on the confirm; hands off for about a minute.

**What runs:** an `OPEN` and `CLOSE` to bring SDRAM up (so the refresh follows the clock), then one `POINT` request for each of 46, 74, 102 and 150 MHz at levels 5, 7, 9 and 15, each held 10 s with the self-check running, each back to 27 MHz and the start level before the reply. At 150 MHz one frame goes to the panel with Apple's bus words.

**Lines:** as above, then `pw4: PASS|FAIL (N of 4 points, rc R[, no 4b flag][, SDRAM down])`.

**Passes when:** all four points came back `ok`. The speed (one self-check's time at 27 MHz over its time at the point) is a reading, not a condition: the self-check is memory-bound, unlike stage 4's kernels, so it may not scale with the clock.

**What it means for battery life:**
- **All four points pass:** our images can run any clock of Apple's ladder at Apple's level. A video session can then pick the lowest clock that keeps decoding inside the frame time, with its level: a stream that fits in 102 MHz at level 9 needs about 56 % less core power than 150 MHz at level 15 (power ∝ f·V²) [estimate]. Next: a per-session clock choice in the video image, measured per stream.
- **Speed "with the clock":** SRAM runs on the core clock, so decode time scales with f; frame budgets per clock follow from stage 4's numbers. **"Slower than the clock":** memory or the bus doesn't scale; budget each clock from its own measured speed.
- **The frame's time at 150 MHz** against a 27 MHz push says whether the LCD bus follows the core (open question #4).
- **A point fails:** the highest good point caps the per-session choice; video stays at 150 MHz and level 15 (today's, which is Apple's) until the failure is understood.
- **No 4b flag:** run stage 4b first; PW4 never touches PLL A on a unit where it isn't proven.

## The hibernate probes: PW5, PW6 (branch `p19-pw56`)

**What this is:** stages PW5 and PW6 of `../../bcm2722-reference/brief-map-power.md` §9. Today every dark spell ends in a cold start of the BCM: 0.37–0.62 s to the first frame (`power.md` §2). Apple's firmware carries a hibernate path (`videocore=1`, §3) that keeps SRAM and would wake in about 10 ms. PW5 asks whether our image can use it; PW6 asks what the port and the panel do with the chip cut, as a first, read-only step towards the brief's "bypass mode". **Built, not run on silicon. PW5 drives `GPO32` bit 15, a pin Rockbox has never driven: it needs the owner's explicit go.**

**The image side** (`HP_OP_HIB`, `0x19`, plain video image only; `bcmfw/hostproto.c` `cmd_hib`, `bcmfw/video/video.c` `video_sdram_idle`/`_rewake`) [ours]:
- **Off by default**, as `HP_OP_TV` and `HP_OP_PW`: `HP_HIB_UNLOCK` hands out a one-time code. Unarmed `HP_E_UNSUPPORTED`; a wrong code or a malformed request `HP_E_INVAL`; a session open `HP_E_BUSY`; the counter stopped, or the core off the crystal, `HP_E_STATE`. Nothing is written in any refusal. The merged image answers `HP_E_UNKNOWN` and is byte-identical. Request: `HP_HIB_MAGIC`, `HP_HIB_PARK`, code, the image's own bound in ms (1–90,000).
- **Apple's park, in Apple's order** (@14288): PLLs B, C, D, then A, divider and control each `pw 0` (the core must already be on the crystal, so its word is never written); the 14 GPIO event words `0x1000_2828`–`285C` saved and cleared; SDRAM idled as @14892 (cache flushed, bits 30 and 16 cleared, command `0x1000_3420 = 0`, bit 15 polled up to 8,000 times), only when an `OPEN` brought it up; then `0x1F0 = 0xA5A5_0000` and a busy poll for `0xA5A5_0001`. Interrupts are already off there (our image enables them only inside its idle sleep).
- **Ours on top:** before the park, 32 KB of SRAM at `0x12_B000` and 32 KB of SDRAM at `0xFE3F_6000` (the spec's 84 KB and 64 KB would reach the parked AAC tables and the dumps) filled with seeded patterns, and eight colour bars drawn into the host frame and pushed (look 1). The poll ends at the request's bound on the 1 MHz counter (90 s from the row), or when the counter reads the same 1,024 times (stopped).
- **Allowlist:** the eight PLL words and the 14 event words only, each write checked against the park's value or the one saved; the SDRAM controller and the cache only through video.c's own bring-up code; the panel through the image's normal push. A denied write is counted.
- **The wake, and every other exit** (no wake word in the bound, a stopped counter, SDRAM's bit 15 stuck): PLLs back (each divider, its control without the enable, 1 ms, then the enables: Apple's rule @4cb8), SDRAM brought up again with Apple's two probe words kept (@147ca), the event words back; then every saved word read back into the mismatch mask (PLL words without the password, `quirks.md` #25; the enables, not the write-1-to-clear status words; the cache's enables). Then `0x1F0 = 0xA5A5_0002` (or `0xA5A5_00FF`, "gave up", ours), the patterns and the frame checked, and the bars **mirrored** and pushed (look 2), so a picture the panel merely kept is told from one the image pushed.
- **Progress:** `0x138` = `'H' << 24 | phase << 16 | ms`. **Reply:** 19 words (`HP_HIB_R_*`): exit, last phase, µs to park, the counter's µs parked, polls while parked, µs to restore, SRAM and SDRAM wrong words with the first index, the frame's sum before and after, SDRAM's bring-up result, the regulator and core words at entry and after the wake, flags with the denied count, the mismatch mask, the counter's raw values at the park and the wake.

**The PP side** (`firmware/target/arm/ipod/video/bcm-devtest-core.c` `bcmdt_hib_cycle`; the rows in `apps/ipodui/devbcm_probes.c`; their logic in `devbcm_hib.h`, host tests in `tests/hibprobe/`) [ours]:
- **The pins, Apple's order only** (`osos` 1.3's state machine, host `0x1028_92D4`, and its three-mode helper, host `0x1026_668C`): retain is bit 15 high, 5,000 µs, bit 14 low; on is bit 14 high, 5,000 µs, bit 15 low. Read-modify-write with interrupts off, bits 14 and 15 only; `GPO32_ENABLE` is never written.
- **Refusals before anything moves:** bits 14 and 15 not both outputs, or the pins not as Rockbox leaves a running chip (14 high, 15 low). Bit 15 is never set from a state with bit 14 low.
- **One cycle:** `0x1F0 = 0`, the park sent, `0xA5A5_0000` awaited for 300 ms (the spec's 100 ms didn't count the patterns and the bars), the retain pins, the hold with **no port access at all**, the on pins, then `0x1F0` read (200 ms bound): `0xA5A5_0000` means SRAM and the parked core held; all ones means the chip restarted from its boot ROM; anything else means SRAM was lost. Held: `0xA5A5_0001` written, `0xA5A5_0002` awaited (200 ms); the wake time is from bit 14 high to that answer.
- **Recovery:** any failure after the first pin is cut on the spot: bit 15 low, bit 14 low, 100 ms. Every run, pass or fail, then ends as every probe does: `vplayer_probe()` cuts the power and the stock wake path cold-starts the display.
- **Timeouts:** the image's own 90 s bound covers the 60 s hold; the PP's bounds are 300 ms to park, 200 ms each for the word and the answer, then the reply under the TV rows' watchdog (progress word and heartbeat every 10 ms, 2 s without either moving is a hang), at most 4 s.
- **Not built** (no evidence for it, or no need): the spec's switch-down from 150 MHz (the row never opens 150 MHz; the image refuses off the crystal); a read of `ALT_CONTROL` to recognise the boot ROM's state (all ones on `0x1F0` is taken as the restart); Apple's write of the core word (already on the crystal).

**vc2sim** [sim]:
- `bcmfw/tests/peek_test.py --only hib` (243 cases): the refusals write nothing; a park writes Apple's order exactly (PLLs B, C, D, A; the 14 event words; SDRAM idled last when up, untouched when down); while parked the core touches nothing but the counter; the restore order (PLL A first, N without the enable, the enables after 1 ms) and every word back; the bars on the panel at the park and mirrored after; a word spoiled in each window while parked is reported with its index; no wake word (the image's bound), a counter stopped while parked, and SDRAM's bit 15 stuck each end with everything back.
- `bcmfw/tests/devtest_test.py --only t_hib` (57 cases): `bcmdt_hib_cycle` through ctypes against the shipped image, with a model of the two pins: Apple's order; both pins high only for the 5 ms windows; no port access while bit 15 is high; the image's reply with every pattern kept. A chip that cuts anyway, comes back scrambled, never answers or loses its port each ends in the standard cut, with no port access after it. The refusals and a park that never comes leave the pins alone.
- The times in vc2sim (park about 17 ms, wake about 5.3 ms of which 5 ms is Apple's pin wait) are a flow check only: vc2sim's clock is not the chip's.

### PW5: hibernate and wake

**Owner:** run PW1 first, in the same boot (it must pass); PW5 is locked until then. Give the explicit go for bit 15. No meter (owner, 2026-10-25). SELECT on the confirm, then watch the screen for about 2 minutes:
1. Colour bars appear (look 1), then the chip parks and the pins go to retain for **1 s**.
2. The bars come back **mirrored** (look 2) if the chip woke. Then the same for **10 s**, then **60 s**, each only if the one before passed.
3. During each hold, note: are the bars still on the panel, faded, or gone?
4. The run ends with the screen back as it was (a cold start, as after any probe).

**Lines** (per hold): `pw5: hold H ms: <end>; park N us, held T us, both pins high A/B us, pins end P[, cut]`; `pw5: hold H ms: 1F0 after = W (held (parked)|all ones (restarted)|lost), wake M us (answer A us): xS a cold start's 370000 us`; `pw5: hold H ms: image exit X phase P: <check>; sram bad N (first I), sdram kept|LOST|not laid (bad N), frame pre/post …/…`; `pw5: hold H ms: counter C us parked (ran|STOPPED or reset), polls N, restore R us, sd …, reg … core … flags … mismatch …`; at the end `pw5: PASS|FAIL (N of 3 holds, rc R); gpo32 now P; PW6 unlocked|stays locked`. Power-log marks `pb5_hold` and `pb5_wake` around each hold.

**Passes when:** the 1 s hold came back with `0x1F0` still `0xA5A5_0000`, the image answered the wake within 200 ms, nothing was denied, everything read back, and the SRAM pattern was intact. SDRAM, the frame and the panel's picture are reported, not required. The 10 s and 60 s holds are readings: how long the chip keeps.

**What each outcome means:**
- **PASS, SRAM kept** (`held (parked)`, the image answered): Apple's hibernate works for our image. Bit 15 is the retention request (the reading in §3). A dark spell can end in the logged wake time (vc2sim's flow: 5 ms of Apple's pin wait plus the restore) instead of 0.37–0.62 s, with the image, its slots and the retained frames still in SRAM: no upload. Next: a hibernate state in the power contract (`power-contract.md`), C3/C4 replaced, and the meter's hold current against today's cut (`power.md`).
- **`counter … STOPPED or reset`** with a pass: the chip's clocks really stopped in the dark (true retention, not a powered park). Expected if bit 14 removes the run supply. **`ran`:** the core kept running through the hold; the hold's current tells whether that state saves anything over a parked, powered chip.
- **SDRAM `kept`:** pools survive too (the controller's command 0 self-refreshes, or retention covers it). **`LOST`:** SDRAM must be refilled after a wake; SRAM-only retention is still the big win.
- **Frame kept** and the bars still on the panel during the hold: the panel's supply and reset (BCM GPIO 48) held. The unlit clock face could then sit in hibernate between minute updates (`brief-map-power.md` §3, "Combined with the panel"). **Bars gone:** the panel needs its 265 ms power-on after a wake; the wake still saves the upload.
- **`all ones (restarted)`:** bit 15 doesn't retain; the chip cold-starts as after Rockbox's cut. Hibernate is not available this way (bypass or something else may be what bit 15 does: PW6). Nothing lost: today's path stands.
- **`lost`:** the chip stayed powered enough to answer but SRAM wasn't kept. As above; note the value read in `quirks.md`.
- **`no answer to the wake`:** the word held but the core didn't resume: SRAM kept, the core's state not (registers or clocks lost). A wake could still skip the upload if the host restarts the core at our entry point (a warm start like B13's): a follow-up probe.
- **`never parked`:** the image refused or was slow; read its reply (logged), nothing was driven.
- **A restore mismatch:** a register that didn't take its saved value back after the wake: a `quirks.md` finding; the final cut clears it.

### PW6: LCD with the BCM off (read only)

**Owner:** run PW5 first, in the same boot (its pins must have run); PW6 is locked until then. Photograph the screen just before SELECT. SELECT on the confirm. When the screen stops changing (the chip is cut), photograph it at about 1 s and again at about 10 s. The run ends with the screen back as it was.

**What runs:** B1, the control: with the image idle, the port's four registers read raw, with no address written first (`DATA` four times, `CONTROL`, `ALT_DATA` four times, `ALT_CONTROL`), then the canary again to show the port still works. B2: bit 14 low alone (Rockbox's own cut, bit 15 low as found), 100 ms, the same reads, a 10 s look, the same reads again. Power-log mark `pb6_cut`.

**Lines:** `pw6: B1 (chip on) data … ctl … alt … altctl … (all ones|all zeros|mixed)`; the canaries; `pw6: B2 (cut, 100 ms) …`; `pw6: B2 (cut, after the look) …`; `pw6: ran|FAIL (rc R): B1 …, B2 … / …; owner looks recorded`.

**Not built, and why:** the brief's bypass has no register evidence on this board (`brief-map-power.md` §8: Apple never uses it, `vmcs.bin` writes nothing that could enable it, and the only candidate is bit 15, which PW5 settles). So PW6 writes nothing into the port of a powered-down chip: the spec's 256-word writes to `DATA`/`ALT_DATA` in B2 and B3, and its B3 (Apple's retain state) with reads, are left out. B3's look (the panel in the retain state) is PW5's hold. The 16 × 16 window those writes would need can come from the image's own `lcd_update_rect` (an `UPDATERECT` of 16 × 16 sets the window and the RAM-write index; that the panel then stays in RAM-write mode is [hypothesis]), so `lcd.c` needs no change if the writes are approved later.

**What each outcome means:**
- **Picture kept with the chip cut** (photos at 1 s and 10 s match the one before): the panel holds its memory with the BCM powered down, so a lit static screen needs only a way to *write* the panel without the BCM. That is the bypass question proper, for a write probe with its own go.
- **Picture fades or goes:** the panel's supply or reset comes through the BCM (GPIO 48, `lcd.md`): a lit screen with the BCM off is not possible on this board, whatever bypass does. The bypass row in the brief map becomes "unverifiable".
- **B2 `all ones`:** nothing drives the bus with the chip cut (what Rockbox's dead-port check expects). **`mixed`** or **`all zeros`:** something answers with the chip unpowered: a pad state worth a `quirks.md` entry, and a hint for how bypass might present.
- **B1 canary fails after the raw reads:** reading `DATA` without an address upsets the port: a `host-interface.md` finding (the run still ends in the cut).

## The COP rows for 19-9 (branch `p19-cop-rows`)

**What this is:** three questions that only silicon can answer before package 19-9 moves the UI, and with it the BCM job ring, to the COP. Each one is a Developer › Advanced › Video chip › Probes row with a confirm screen (SELECT on the last line, MENU cancels). Each row stops the audio, starts its own worker on the COP (`firmware/export/cop_job.h`, posted with no cache flags, so the only cache operations are the row's own), and stops it again at the end. Every wait has a bound: a COP job that does not end within 1 s marks the worker stuck, the row says **REBOOT**, and every later COP row refuses to run. The pure logic is in `firmware/target/arm/ipod/video/bcm-coplab.h` and is tested by `tests/coplab/` [ours]. The device half is in `bcm-devtest-hal.c`. **None of these rows has run on silicon yet.**

| Card | Entry (exact Debug row) | Time | Log prefix | Decides for 19-9 |
|---|---|---|---|---|
| C1 | **COP row C1: engine 2 from the COP** | < 1 min | `c1:` | whether the ring's engine-2 slot pieces can be started and polled from the COP |
| C2 | **COP row C2: shared cache data array** | a few s | `c2:` | whether the cores' caches share one data array; what crossing data needs |
| C3 | **COP row C3: COP sleep under yield** | ~5 s | `c3:` | whether a COP that polls with `yield()` can still sleep |

### C1: engine 2 from the COP

**Needs:** a stage 5 pass flag and the probe image (`bcmprobe.bin`), like the probe rows.

**What runs:**
- `vplayer_probe()` boots the probe image (the plain video image), which then idles.
- The target is `HP_LAB_SRAM` (`0x12_B000`, 32 KB: free SRAM in the image, below the AAC tables at `0x13_4000`). Inside it are 16 guard words at each end, with a 32,640-byte piece between them.
- Two runs follow, each with a fresh seed (the patterns are `bcm-e2lab.h`'s G, T and S):
  1. The CPU writes G | T | G over the whole window through the port and reads back every 64th word. Any mismatch voids the run.
  2. Engine 2 moves S over T as one piece (`e2port_start`, then `e2port_busy` polled until the piece ends, 100 ms bound, then `e2port_stop` on a timeout; the port's write-ready check follows).
  3. The CPU reads back every word of the window and tallies it.
- **Run 1 (the control):** the piece is started and polled on the CPU (the row's own thread).
- **Run 2:** the same piece is started and polled **on the COP**, in a job.
- The CPU's thread claims the engine (`e2port_claim`) and holds the port lock around each run. It writes back its own dirty lines of S first, because `e2port_start`'s `commit_dcache()` writes back only the cache of the core that runs it.

**Lines:**
- Per run: `c1: CPU|COP run: rc R on core K, start S us, run U us (P polls), wall W us, 32640 B`
- Per run: `c1: … ok N untouched … shifted … other … of 8160, first bad …, guard hits G, seed …`
- At the end: `c1: PASS|FAIL (rc …): CPU pass|fail, COP pass|fail`

**Reading it:**
- **PASS:** the COP run reports core 1 and rc 0, all 8,160 words are correct, and no guard word changed.
  - 19-9 may start and poll the ring's engine-2 pieces on the COP.
  - Compare `run` with the CPU's run. Similar times mean the engine does not care which core started it.
- **The CPU's run passes and the COP's fails:**
  - all words untouched: the engine never started from the COP;
  - rc 1: the piece never ended (stopped);
  - rc 2: the port was not ready after it.

  In every one of these cases, 19-9 keeps the engine's start and end on the CPU and posts the pieces to it.
- **Both runs fail:** the setup is at fault (the engine or the image), not the core. Rerun after the E2 lab.
- **REBOOT:** the COP job never finished. Reboot before playing anything.

**Two findings already, from the code [ours]:**
1. `e2port_start()` writes back only the running core's cache. On the COP, the CPU's dirty lines of the source would never reach memory, so the core that produced the source must write it back.
2. e2-port.c's owner flag (`owner`) and its claim are core-local: the flag sits in cached `.bss` and is guarded by `disable_irq_save()`. A claim made on the COP is not seen by the CPU. The row claims the engine on the CPU's side for this reason. This belongs to the step-0 coherence package (`e2-port.c` is not ours to edit).

### C2: shared cache data array

**Needs:** nothing but a COP running. The video chip is not involved.

**What runs:** 8 trials in each direction, the writer W being the CPU and then the COP. The trials rotate over 4 lines, 16 bytes apart, of the row's own 80-byte allocation (cached SDRAM). Each trial (`bcm-coplab.h`):
1. Both cores clean and drop their caches. The word gets v0 through the uncached alias. The reader R reads it cached, so it holds the line (`pre`).
2. W reads it cached, writes v1 cached, and reads the uncached alias (`u1`: v0 while the write sits in W's cache). It then writes v9 to memory through the uncached alias.
3. R reads it cached (`r`).
4. The uncached alias is read again (`p`: v9 unless W's dirty line went to memory in between).
5. Both cores clean and drop their caches again. This leaves the cache state as it was, with no line of the buffer left in either cache.

**Lines:**
- Per trial: `c2: CPU>COP|COP>CPU trial k at <addr>: <class> (pre … u1 … r … p …; v0 … v1 … v9 …)`
- Per direction: `c2: <dir>: <verdict>; separate … shared … miss … setup … through … evict … odd …`
- Overall: `c2: <verdict> (rc …)`

**Classes:**
- `separate` (r = v0): R answered from its own copy.
- `shared` (r = v1): R's lookup found W's data.
- `miss` (r = v9): R's line was gone.

The void classes come first and prove nothing:
- `setup`: pre ≠ v0;
- `through`: W's write reached memory at once;
- `evict`: W's line was written back before R read;
- `odd`: r is none of the three values.

**Verdict:** decided by the valid trials only (at least 4):
- `SEPARATE (no coherency)`: only separate;
- `SHARED data array`: only shared;
- `MIXED: rerun`: both, or any odd;
- `INCONCLUSIVE`: fewer than 4 valid trials.

**Reading it:**
- **SEPARATE in both directions (expected):** each core has its own data, as `cores-and-threads.md` §4 assumes.
  - 19-9's shared state (the ring's indices, the port lock's corelock, `bcm_devtest_owned`, `bcmfw_active`) must live in IRAM or `SHAREDBSS`.
  - Buffers handed over by pointer need a write-back on the producer core and a clean and drop on the consumer core. That is the step-0 package's plan.
- **SHARED:** the two status arrays front one data array. A core's stale tag can then return the other core's data.
  - Today's rules stay safe.
  - Stop and look before 19-9 relies on any cached hand-off: a whole-cache drop on one core would also hide the other core's dirty data.
- **`through` on every trial:** this core's cached writes do not stay in the cache (write-through or no write-allocate). Record it as a [silicon] finding for the reference; the question stays open.
- **Mostly `miss` or `evict`:** the scheduler's code between the steps evicted the line. Rerun with the screen untouched.

### C3: COP sleep under yield polling

**Needs:** nothing but a COP running.

**What runs:** three windows of 1 s each. The COP's job waits in a different way in each window:
1. `while (!stop) yield();`
2. `while (!stop) sleep(1);`, the way `lcd_e2_push` waits;
3. a blocking `semaphore_wait()` that the CPU releases.

Throughout each window, the CPU samples the COP's control register (`COP_CTL` bit 31, `PROC_SLEEP`) every 20 µs. It yields to no other thread, so the screen freezes during the count. The asleep samples give the sleep share. The awake-to-asleep edges are a lower bound on the sleep entries. Beside them is the kernel's own idle time for the COP (`devstats.idle_us[COP]`, in a developer-mode build; otherwise `n/a`).

**Lines:**
- Per window: `c3: yield|sleep(1)|blocking: rc …, asleep X % of N samples, E sleep entries, W waits, kernel idle Y %, window … us`
- At the end: `c3: yield keeps the COP awake|the COP sleeps under yield|unclear (rc …)`

**Expected [hypothesis]:**
- **yield:** about 0 % asleep and about 0 entries. The poller is the COP's only runnable thread, so `switch_thread()` never reaches `core_sleep()`.
- **sleep(1):** mostly asleep, with about 100 entries a second (one per tick wake).
- **blocking:** about 99 % asleep, with about 100 entries a second (the CPU's tick wakes the COP).

**Reading it:**
- **"yield keeps the COP awake":** under yield the COP sleeps less than 5 %, and under the blocking wait more than 50 %. 19-9's COP side must not poll the BCM, the engine or the ring with `yield()` alone: each poll then costs a whole core. It should block instead (a semaphore, or `sleep(1)` when a piece is longer than a tick).
- **"the COP sleeps under yield":** the kernel idles the core even under yield. Polling is then cheap; record how.
- **"unclear":** the blocking control did not sleep either. Something else kept the COP busy (the codec worker?). Rerun with the audio stopped and nothing playing.

## The video port's lock, a piece at a time (jobs.md §6)

The player's port calls (`bcm_video_rd`, `_wr`, `_wr_block`, `_flush`) and the stages' HAL port calls now take the port lock for each piece. A DMA block keeps the lock until its end is seen, and the feed thread flushes before it blocks and before it exits. **Owner check, the next time a video plays:** compare the `video: port KB, MB/s (busy time)` and `port CPU ms, blocks` lines in `bcmtest.log` with the last session's for the same clip. They should agree within noise, because a lock and an unlock (both uncontended) cost a few µs against a piece's 35–85 µs read or a block's milliseconds [hypothesis].

## Planned silicon row: the REP-16 vector store

**Why:** the job kernels' single-row stores are about half the cycles of every vector copy (the cost audit, `jobs.md` §10). The REP-16 `vst16` with a stride would roughly halve that. On silicon it has only been checked informationally, in session 8's store check.

**The row:**
1. Build each copy kernel both ways (single stores and REP-16 stores).
2. Run both on the device over a set of rects.
3. Read every destination row back, and compare it with the C copy.

**Passes when:** every row matches, every pitch tried, both clocks.

**Decision:** the REP-16 form is adopted in `gen_jobs.py` only after this passes. Until then the single stores stay (the orchestrator, 2026-10-24).

**Status:** to build in the next probe wave.

## Planned silicon row: the lane shift (ASHF, ASHG)

**Why:** the job kernels fetch a source that is out of phase with its destination (the drift's pan at x 70, a FADE toward a cover out of phase, AVG's second framing, the marquee strip) by two aligned loads and a lane shift (`gen_jobs.py`'s `jv_gather16`, `jobs.md` §10). The shift reads an H16 **A operand at a `+r` lane offset** (x = p, then 16 + p). Silicon aligns a horizontal **B** operand's `+r` x down to 16 (`quirks.md` #17); A operands took any constant x on silicon (S22, S27), but never a `+r` one. vc2sim applies A's `+r` as given [sim].

**The row:** two informational vector checks in the stage 4 timing bench (stage 4b too), read-only like every bench check (`bench/gen_bench.py`, `shift_case`):
- **ASHF:** two 32-byte rows side by side ([lo0 hi0 lo1 hi1]) in VRF rows 0–1; `vbitplanes #0xF800 setf`; `vadd D16 A16+r2 #0 rep2` with r2 = 5, then the same at +r3 = 21 under `ifnz`. Rows 8–9 must hold pixels 5–20 of the 32.
- **ASHG:** the same at p = 13 (+r 13 and 29, mask `0xFFF8`).

Nothing else changes: no new Debug row, no write outside the bench's own buffers.

**Passes when:** both records match (readlog: no line about the lane offset). Both clocks.

**If it fails:** readlog says "an A operand's +r lane offset is not as modelled (ASHF/ASHG)". The record's words show what x silicon used: if it reads x aligned down (pixels 0–15 or 16–31), A's `+r` follows B's rule. The image already protects itself: every jobs image runs a self-check of the same shift at start (below), so a failing chip keeps out-of-phase sources in C without a rebuild. The fix is then to rebuild the shift from constant-x A operands. It is a `quirks.md` finding either way.

**The image's own check (2026-10-24):** `jobs_init` (`jobs_bcm.c`, every image with the job hooks) gathers through the real `jv_group16` at the same phases, 5 and 13, and compares against the pattern in C. If they agree it sets `J_VPU_SHIFT` in the job block's `J_VPU` word (`jobs.h`); if not, only `J_VPU_CHECKED`, and `span()` sends out-of-phase sources to the C path, as before the lane shift. So ASHF/ASHG now also **confirm which path the self-check chose**: on a bench pass, read the job block's `J_VPU` (0x44) alongside the two records. ASHF/ASHG passing with `J_VPU` = 3, or both failing with `J_VPU` = 1, is consistent. Any other pairing is a finding: the check and the bench row disagree about the same form.

**Status:** built with this change; runs with the next stage 4 bench pass. The out-of-phase kernels are bit-exact in vc2sim [sim] and unproven on silicon until then; the in-phase path (every aligned rest) doesn't use them.

## Planned silicon row: the 32-bit accumulate (X15M, X15S, X15Z, MHDU, VCLP, R48E, D16R)

**Why:** hw5 `[15]` (`x15`) and ops 18, 19, 51 and 37 are decoded from Apple's code and modelled in vc2sim (`isa.md` "The 32-bit accumulator" [apple-RE, sim]), but silicon has never run them. They give the vector unit an exact 16 × 16 → 32 multiply-accumulate: the exact islow IDCT candidate (`algorithms.md` §1), Apple's H.264 centre half-pel, and fixed-point (16.16) math for the 3D engine all need it. Two operand rules came with them: where a 48-bit op's +r goes, and a 16-bit D's +r x (which also bears on ASHF/ASHG).

**The row:** seven informational vector checks in the stage 4 timing bench (stage 4b too), read-only like every bench check (`bench/gen_bench.py`, `x15_cases`; their own random stream, so no other check's data moves):
- **X15M:** X · −320 + Y · 1280 per lane as `vmul … x15` / `vmulhd … acc` pairs (`jhoz`'s idiom); rows 8 and 9 read back the high half (`vmov #0 acc`) and the low half (`vmov #0 x15`), as Apple's VPU context save does. Lanes 0–7 carry or borrow out of the low half.
- **X15S / X15Z:** the high half from row 0 (`vmov … clra acc`), then row 1 into the low half with `acc x15` (X15S: signed, so the high half drops by 1 where row 1 is negative) or with `x15` alone (X15Z: zero-extended, the high half unchanged; Apple's restore idiom). Lanes 0–7 are negative.
- **MHDU:** `vmulhdsu`, `vmulhduu`, `vmulhd` on the same rows (B negative in lanes 0–7).
- **VCLP:** `vclip #255` on values below 0 and above 255.
- **R48E:** `vadd.1 d08.08+r2, a00, b01.2` with r2 = one row: row 8 = row 1 + row 2, row 9 untouched.
- **D16R:** `vadd D16+r3, A16, #0` with r3 = 3 (x only): row 8 = row 0 if the x aligns down to 16.

Nothing else changes: no new Debug row, no write outside the bench's own buffers. The image grows by 1,888 bytes (30,992 → 32,880; the PLL build 31,952 → 34,000).

**Passes when:** all seven records match (readlog prints one line each: "… (as modelled)"). Both clocks.

**If it fails:** readlog names the reading silicon took for each check, from the record's first four words (lanes 0–7 of row 8):
- X15M "high halves only, NO carry": the low half does not carry. The exact IDCT and Apple's j would then need the hi/lo readout and a scalar carry fix; the candidate is off until understood.
- X15S/X15Z "NOT as modelled": swap the sign rule in vc2sim (`HYP['v_x15']`) and the readout recipes in `isa.md`.
- MHDU "reads B signed": op 19 is the signed one; the 16.16 recipes change.
- VCLP "min only": the IDCT's `vclip` needs a `vmax #0` first.
- R48E or D16R not as modelled: the operand rule is wrong; `vc_interp16xN_jhoz`/`jvert` need another look. D16R failing (x applied as given) also means ASHF/ASHG's A operands probably take x as given.
Each is a `quirks.md` finding either way (#27–#28).

**Status:** built with this change; runs with the next stage 4 bench pass. Until then the semantics are [apple-RE, sim].

**Depends on it (2026-10-24, branches `p19-vpu-idct`, `p19-idct416`):** the display-only image's JPEG IDCTs (4, 8 and 16 points), `jv_idct4n` (`jpeg-spike.md` §13-14), uses `vmul … x15` (zero-extended, with `clra`: X15Z's reading), `vmulhd … acc` (X15M, MHDU), the carry out of the low half (X15M) and `vclip #255` (VCLP). It also uses REP 8 accumulating across repetitions, with A and B stepping (REPA, below), and forms no row checks yet: a stepping V16 B; D's `+r` on V16 and H8. The image's start-up check (`J_VPU_MAC`) compares the kernel with the C on four blocks and keeps the C if anything differs, so a failure here costs speed, never pixels.

## Planned silicon row: REP accumulation (REPA)

**Why:** `jv_idct8x4` (`jpeg-spike.md` §13) makes each output's eight products with one REP 8 pair, and the pair is only exact if every repetition adds into the same accumulator. vc2sim and `isa-vector.md` model it that way [sim]; nothing on silicon has shown it. The X15 rows check one product at a time (REP 1). REPA checks the pair itself.

**The row:** one informational vector check in the stage 4 bench (4b too), read-only like the others (`bench/gen_bench.py`, `repa_case`, drawing from `x15_cases`' random stream after the X15 rows, so no other check's data moves). It is the kernel's exact form:
- `vmov Ddiscard, #514 clra acc` (pass 2's seed, 514 << 16);
- `vmul d48, A, B rep8 x15`, then `vmulhd d49, A, B rep8 acc`;
- A = H16 row 16 + j through `+r4` (r4 = 1,024, pass 2's value), stepping a row; B = the V16 column j of rows 32–47, stepping a column (row 32 + i holds lane i's eight constants).
Rows 49 (the high half) and 48 (the low half) are stored, in that order.

**The readings** (lanes 0–7 of row 49, the record's four words; readlog prints which one silicon followed):

| Reading | Lanes 0–7 of row 49 |
|---|---|
| **accumulates** (as modelled): 514 << 16 + Σ p_j, mod 2^32 | 514, 521, 511, −15,868, 529, 272, 23,481, 509 |
| **restarts each repetition** (each from a cleared accumulator: only p_7, the seed lost) | 16,384, 0, −1, −1, 0, −1, 16,384, −3 |
| **last only** (only the last repetition adds, onto the seed) | 16,898, 514, 513, 513, 514, 513, 16,898, 511 |

Lane 0 is eight (−32,768)² products (the sum wraps at 32 bits), lane 1 eight 255 · 255 low halves that carry 7 into the high half, lanes 2–7 mixed signs. The generator asserts that the three readings differ in every one of lanes 0–7; `bench_test` checks that readlog's table is the generator's and that readlog names vc2sim's record "accumulates".

The image grows by 1,152 bytes (32,880 → 34,032; the PLL build 34,000 → 35,152): 26 input rows (832 B) and the expected rows.

**Passes when:** readlog prints "REP 8 with acc accumulates across repetitions (as modelled; …) (REPA)". Both clocks.

**If it fails:**
- "restarts each repetition" or "last only": the IDCT's REP 8 pair isn't exact on silicon. `J_VPU_MAC` stays clear (the start-up check catches it) and jdec keeps the C IDCT. Rebuild with `gen_jdec.py --unrolled` (sixteen REP-1 ops an output; about 130 B more, so it needs room in the display-only image), fix vc2sim's REP rule, and record it in `quirks.md`.
- "neither reading": readlog prints the eight values; compare them with the per-lane products (`gen_bench.py` `REPA_A`, `REPA_B`), for example a REP that doesn't step A or B.

**Status:** ran 2026-10-25: lanes 1–7 accumulate as modelled, lane 0 saturated (32767; quirks #32). The model now saturates (`sat32`) and REPA expects 32767 there; readlog names that reading "saturating at 32 bits (as modelled …)".

## Planned silicon rows: the variants of 2026-10-25 (ASHX, ACSA, GTHR)

**Why:** ASHF/ASHG, X15S and REPA failed on 2026-10-25. Two rules for an H16 A operand's +r x and three for the accumulator's saturation fit every word that session logged (`bcmfw/tests/vpu_fit_test.py`, quirks #30–#32). These rows tell them apart in one sitting; the kernels are already exact under every one of them, so the rows only settle the model.

**ASHX** (`gen_bench.py` `vpu_fit_cases`): row 8's lane pairs 0–1, 2–3, 4–5, 6–7 each from an H16 A at +r x 37, 53, 16 and 48 (rows 0–1 x 0 and x 32 hold four different columns), each pair under its own `vbitplanes … setf`, `ifnz`. First record by rule (readlog `VARIANT_WORDS`):

| Rule | Record (lanes 0–7 of row 8) |
|---|---|
| `dbl16` (modelled) | `1e85f481 23592002 a5ea4233 6655f25a` |
| `near32` | `46034350 8d6eaadf a5ea4233 68512e4a` |
| as given (refuted) | `f25aa5ea 9b085166 ea6e3321 51664af2` |
| aligned to 16 bytes (refuted) | `46034350 6e23df20 ea6e3321 51664af2` |
| x dropped (refuted) | `1e85f481 8d6eaadf 6eac2191 68512e4a` |

**ACSA:** four sums in lanes 0–3 and again in 4–7: `clra acc` with row 0, then `acc` with rows 1 and 2 (32767 + 1 − 1, −32768 − 1 + 1, 20000 + 20000 + 0, 1000 + 2000 − 3); the high half read into row 8 (`vmov …, #0 acc`), then the low half (`#0 x15`) on lanes 4–7.

| Rule | Record |
|---|---|
| `sat32` (modelled) | `80017ffe 0bb57fff 0000ffff 0000ffff` |
| `read16` | `80007fff 0bb57fff 00000000 00000000` |
| `hi16` | `80017ffe 0bb57fff 00000000 00000000` |
| wrapping (refuted) | `80007fff 0bb59c40 00000000 00000000` |

**GTHR:** `jv_gather16`'s own four ops (two V16-B transposes, an H16 A at a +r y of 7 rows, the transpose back) on 32 distinct rows: F = pixels 7–22 of each. Uses only forms the 2026-10-25 run proved (a stepping V16 B, REPA; an A's +r y, REPA and R48E), so it should pass under every rule.

**Passes when:** readlog names `dbl16` (ASHX), `sat32` (ACSA) and "the jobs' lane shift … as modelled" (GTHR). If ASHX names `near32` or ACSA `read16`/`hi16`, set that rule as vc2sim's default (`tools/bcmre/vc2vpu.py` `v_a16_reg_x`, `v_acc_sat`) and the bench model with it; no kernel changes. "Neither variant": readlog prints the words; fit them as `vpu_fit_test.py` does.

**Status:** built; runs with the next stage 4 bench pass.

## Planned silicon row: alignment (ALGN; branch `p19-vc-align`)

**Why:** the owner's question, "are we also checking alignment?". The PP side aborts on a misaligned access and the gate's UBSan checks catch it; the BCM side gives wrong data instead (`../../bcm2722-reference/memory-access.md` §11). CR1's 301 × 7 slot showed one such case (a host write at 2 mod 4 lands 2 bytes low, quirk #34). ALGN checks every unknown of §11 once.

**Where it runs:** at the end of stage 4 (the timing bench), after the vector checks, the store-locating records and the timing tables are logged: wherever stage 4 runs (the bench session's stage 4 step, the Debug menu's device test), ALGN follows. It never fails stage 4. Stage 4b does not run it.

**Part 1, the BCM's own accesses** (`bcmfw/bench/algn.S`, `algn.c`, `algn.h`): the host sends `CMD_ALGN` (`0x45`) to the bench image once the bench is read; the image runs a table of 41 small checks and answers. Each check is a leaf routine; a fault in it is caught by `crt0.S`'s hook (bench builds only), which records the vector, the saved pc and r0–r5 and resumes in the runner, so no check can stop the image. A check that hangs the core leaves its index in the block's step word, and the host's 2 s command timeout logs `ALGN stopped at check k (TAG)`; the checks that might fault on silicon all come after the controls.

| Tags | What | Readings told apart |
|---|---|---|
| LW1C | `ld (rs)` at +1: the control (S2/S3) | faults after reading rounded down (as modelled) |
| LWR2, LWR3, LWO2, LWL2, LWP2, LWX2 | `ld` at +2/+3 through `(rs)`, `(rs + 64)`, the 48-bit `(rs + 65536)`, `(rs)++`, `(rs + ri)` | fault or not; rounded down, exact (unaligned) or rotated; how many of 4 later adds ran; writeback |
| LHR1, LHR3, LHO1, LSH1, LHP1 | `ldh`/`ldsh` at odd addresses, every form | the same, for halfwords |
| SWR1, SWR2, SWR3, SWO2, SWP2, SHR1, SHR3, SHO1 | `st`/`sth` at +1/+2/+3, every form, into a filled buffer | fault (precise: saved pc = the store?), and nothing stored, stored rounded down, or stored at the exact address |
| VL68 | `vld16` at +8: the control (quirk #17a) | reads rounded down to 32 (as modelled) |
| VL64, VL62, VL88, VL84, VL82 | `vld16` at +4/+2, `vld8` at +8/+4/+2 | rounded down to 32, 16 or 8, or exact |
| VLS4 | `vld16` REP 2 at a stride of 40 | row 1 at +40 (the base rounded once, as modelled) or rounded per row |
| VPG4, VPGX | `jv_yuv16`'s gather: a lone `vldp8` REP 16 at a stride of 4 from +12 (row phases 12, 0, 4, 8); a lone `vldp8` from +14, lanes crossing the line | each row's first-line lanes loaded (as modelled) or not; the crossing lanes dropped (a `vldq` needed, as modelled) or fetched (`open-questions.md` #21) |
| VPQS | a `vldp8`/`vldq8` pair, REP 4 at a stride of 20 from +4 (row phases 4, 8, 12, 0): our display tile and decoder MC run pairs at strides of 88 or 120 for widths that are not multiples of 16 | each row split at its own line (as modelled) or at the first row's |
| VS68, VS64, VS62, VS80, VS88, VS84 | `vst16` at +8/+4/+2; `vst8` at +0 (its layout), +8, +4 | stored at the exact address (as modelled), rounded down to 32/16/8, or nothing |
| D32C | DMA channel 7, 64 bytes in 32-byte units, SRAM to SRAM: the control (PW8's form) | copies exactly |
| D4W9, D2H3, D1B5, D32M | 4-byte units × 9; 2-byte units at 2 mod 4; 1-byte units, 5 at odd ends; one 32-byte unit to +4 | copies exactly, rounded to 4, whole words a unit, nothing, or times out (20 ms bound, then the channel is stopped) |

**Part 2, the host port** (`bcm-devtest-core.c` `algn_port`, the HAL's `port_algn`): writes in chosen forms into SRAM the bench image never touches (`0xF0000` on, 8 KB a check), each region filled first by aligned words, then every halfword read back against a shadow of what an exact port would hold.

| Tags | The coordinator's row | What | Expected (the rules of §11.4) |
|---|---|---|---|
| PWD0, PWD2, PWD4, PHD2, PAD2 | ALGN1 | 64 words at +0, +2, +4 by 32-bit data; at +2 by halfword data; at +2 in Apple's forms (halfword address writes, halfword data, no poll) | +0 and +4 exact; +2 two bytes low; PHD2 and PAD2 are the open question |
| P164, P168, P16C | ALGN6 | 16 bytes at +4, +8, +12 | exact: word alignment suffices |
| PLH1, PLH3, PLH5, PLW3 | ALGN2 | 1, 3, 5 halfwords at an aligned address by halfword stores, then a word elsewhere and a 16-byte block; 3 halfwords by our word tail | exact, nothing later shifted (a lone halfword commits); PLW3 exact with its zero high half |
| R602, R604, R608, R64W | ALGN3 | 301 × 7 by engine 2 (600 bytes a row) and a halfword tail, at pitches 602, 604, 608; 604 with our word tail | 604 and 608 exact, 602 two bytes low on rows 1, 3, 5 |
| E2ND, E2DR | ALGN4 | engine 2 (256 bytes), then at once a new address and a word with no poll: after the engine's end only, and after the port's write-ready too | no difference: both exact |
| RX13, RX1W | ALGN5 | rectangles at x 1, w 3 and w 301, three rows at pitch 640, written by the rule writer (read-modify-write of the end words, aligned words between) | exact |

**Lines:**
- `ALGN: alignment checks (bench row ALGN)`
- `ALGN n 41 faults F src S buf B in U us`
- `ALGN k TAG status pc at r2 r3 r4 r5` (status: 1 ran, 3 fault, 4 timeout; vector `<< 8`; faults `<< 16`) and `ALGNB k w0 … w7` (the target buffer's window);
- `ALGN stopped at check k (TAG): …` if a check hung;
- `ALGNP k TAG bad N low2 M of T [+off got want]…` (low2: wrong halfwords holding the shadow's next one, a write 2 bytes low).

`bcmfw/bench/readlog.py` (through `algnlog.py`) names the reading each check fits, with "NOT as modelled" where it differs from vc2sim's model or §11's expectation; `scripts/bench_ingest.py` lists them under "bench checks" and as anomalies.

**Passes when:** it is informational: every check reports, none hangs. The readings, not a pass, are the result; each settles one unknown of `memory-access.md` §11 (open questions #5, #43, #68–#70).

**[sim]:** `bench_test.py`, row "ALGN": the 36 BCM-side scalar and vector checks run with every fault caught and read as modelled; the five DMA checks only report there (that bus has no DMA model; with vc2mmio's model they time out, since it takes config bit 0 as the channel's enable and PW8's word `0x0000_E802` leaves it clear: a model gap, PW8 ran on silicon). The port checks are not modelled (the question is silicon's).

**Status:** built with this change. Only the bench images change: bench 37,168 → 40,544 bytes, bench_pll 38,256 → 41,664 (`bcm-devtest-images.c` is regenerated by the image batch, on its own commit; until then `devtest_test`'s "embedded images match a fresh build" row reports it stale). The display-only, merged, staged and video images are byte-identical (`mkvideo.py --check`).

## Later: np-sharp's panel, BCM against CPU (PANELAB)

**Why:** the Now Playing box decodes on the BCM (box-size read-back, about 10 k port words), but the 300 x 300 panel's read-back is about 45 k words: at 55-85 us a word under load [silicon, `host-driver.md` §9] that is 2.5-3.8 s of PIO, likely more PP time and battery than the CPU decoding the panel itself. So the panel's BCM path is on in developer (devtest) builds, to collect both timings, and off in release builds, where the CPU decodes it (`art_bcm_panel_set()`, branch `p19-6shell`).

**The row:** the same covers rendered both ways on the device, from `ipodui.log`: every panel logs `art panel cpu ms=N ok=1` (the CPU's decode and write) and, with the switch on, `art panel bcm dec_ms=N read_ms=M ok=1` (the BCM's decode, then the read-back and write). A small cover (about 30 KB), a 600 px one and a 3000 px one, lit and in the unlit window, on a devtest build (switch on). The CPU's own figure for the same cover comes from a release build, or from a devtest panel that took the CPU path (dark, or no DECODE).

**Passes when** (the release default may turn on): `dec_ms + read_ms` < the CPU's `ms` for the covers np-sharp sees, at no more PP time (the read-back's owner passes). Otherwise the panel stays on the CPU until a design keeps it resident on the BCM (a BLIT, no read-back: `album-render.md` §14).

**Status:** the log lines are in `p19-6shell`; not run.

## Later: the resident panel on silicon (RESPANEL)

**Why:** the panel's end state (`album-render.md` §13.8 as built, §5.3): np-sharp's panel stays in a BCM slot and the Now Playing row and the drift BLIT it; the PP reads it back only for `npcover.pak`, in the background. The sim proves the paths and the pixels; silicon must show the times and that nothing is read back on the UI's path.

**Needs:** an image that offers DECODE: the display-only image (devtest build, B5/B13), or the merged image with its DECODE overlay (the next re-verify batch, `BCMFW_JOBS_MERGED_OVL`, after the driver loads `dec-ovl.bin`).

**The row** (from `ipodui.log`; a devtest build, the drift's jobs on): play an album with no built panel, rest on the main menu's Now Playing row, pause, wait 10 s; Music; wait 10 s; back to the row; backlight off and on (a cut on battery).
- `art panel bcm dec_ms=N read=later`: the decode while the row shows, its read-back deferred;
- `panel resident set key=… slot=… epoch=…`, then `panel nowplaying resident … how=fade|swap` and `resident show us=N rc=0`: **the BLIT's time** (still frame: submit to FENCE, BLIT + SHADE + PRESENT);
- off the row: `art panel bcm dec_ms=0 read_ms=M ok=1`: **the background read-back's time** (a hit, no decode), and `npcover build … state=1`;
- after the cut: `nowplaying resident lost`, `art npcover queue … why=resident`, `art panel resident dec_ms=N`;
- never: `FAULT art bcm read off thread`, `FAULT art bcm read while shown` (no read-back on the UI's path), `drift cpu why=`, `bcmjob fault op=0x30`.

**Passes when:** the lines above appear in order, `resident show us` is under one frame (33 ms), and no FAULT line appears. Record `dec_ms`, `read_ms` and `us` per cover size (as PANELAB's covers).

**Status:** built in `p19-panel-resident` (sim: `ipodui-p19-panel-resident.script`); not run on silicon.

## Later (not in the current session build): the visualizer tap's uncached costs (VTAP)

**Why:** the PP-side PCM tap (`apps/ipodui/viz_tap*.c`, branch `p19-pcm-tap`, merging with the visualizer shell package #126) writes its ring from the PCM FIQ and reads it on the UI side through the **uncached alias**, so a COP reader after 19-9 needs no cache operation per frame. Its costs were measured as ARM instruction counts under qemu (FIQ copy 182 instructions a 256-frame mixer frame; a 512-sample read 8,795), but uncached stores and loads cost more per access on the PP5022 than the cached ones the count assumes; the CPU share (0.15-1 % FIQ, 0.5-2.2 % read) is an estimate until measured.

**The row (developer, read-only, about 1 min):** with a track playing and the tap open on a 4 KB arena block, the USEC_TIMER around (a) 1,000 FIQ tap copies (instrumented in a developer build) and (b) 300 reads of 512 samples, at 80 and 30 MHz; then the same through the cached alias for comparison. Log `vtap: fiq_us= read_us= mhz= alias=uncached|cached`.

**Passes when:** both shares stay under the hardware.md budget row (FIQ ≤ 1 %, read ≤ 2.5 % at 30 MHz). **If it fails:** keep the ring cached on the CPU-only UI (one clean per read is cheaper than uncached per word) and move the uncached path to 19-9.

**Status:** planned with #126; not in `out/deploy-session-1024`.

## Planned silicon row: jdec's put and fill (PUTV)

**Why:** `jv_put` and `jv_fill` (`jpeg-spike.md` §16) rely on two forms silicon has never run: a **V8 D at +r** whose y is not a multiple of 16 (the copy of an 8-point block sits in rows 8–15), and the fill's **masked H8 `vmov` at +r**, REP 16 stepping a row, `ifnz` after `vbitplanes`.

**The row:** one informational check in the stage 4 bench (`gen_bench.py` `putv_case`): `vadd` into the V8 column at +r2 = 8 · 64 + 17 from row 48's H8 at +r5 = 8 (rows 8–15 of column 17 get row 48's bytes 8–15), then `vbitplanes #0x5555 setf` and `vmov D8+r3 (x 16), r4 = 0xA5, rep16 ifnz` (columns 16 + even lanes of rows 0–15 = 0xA5, column 17 kept). Rows 0–15 stored. No other check's data moves.

**Passes when:** the record matches; readlog prints "jdec's put and fill forms … as modelled (PUTV)".

**If it fails:** the image's start-up check already keeps the C put, fill and IDCT; readlog says so. The V8 D's +r y or the predicated REP is then a `quirks.md` finding, and the put needs another layout (the copy in rows 0–15).

**Status:** built with this change; runs with the next stage 4 bench pass.

## Planned silicon row: the visualizer's vector bars (BARV)

**Why:** `jv_bars16` (`visualizer.md` §5.1, `gen_viz.py`) draws `BARS` 16 × 16 blocks at a time. It uses forms silicon has never run:
- `vadd` D16 by a register (ADRG's form, its first silicon run still pending), and the same at REP 16;
- `vmul` D16 by an immediate, and `vasr #8` and `#15`;
- the broadcast: a `vadd` into a **V16 D** stepping a column per REP, from an unstepped H16 A;
- `vmov` D16 from a register at REP 16 under `ifnz` (after `vbitplanes`);
- `vsub` into a discarded D with `setf`, then `vadd D16 #0 rep16 ifn` (the N flags of `first − (rr + 1)`);
- `vsub`/`vrsub` with an unstepped H16 B row at REP 16;
- `vmul`, `vrsub` and `vmin` with a stepped 16-bit B at REP 16.

**The row:** one informational check in the stage 4 bench (`gen_bench.py` `barv_case`). It drives one block through every one of those forms in the kernel's order, on its VRF layout:
- **Inputs:** the block in rows 0–15; the first bar rows, the cap rows, f and the row indices (rr = 32) in rows 48–52.
- **The gradient:** three channels into row 53, broadcast to GC (rows 16–31 x 32); the indices broadcast to RI (rows 32–47).
- **The background:** on lanes 3–14 (`vbitplanes r5`, `vmov … ifnz`).
- **The covered lanes:** `setf` on r2 = 33, then `ifn`.
- **The bar select and the cap select.**
- **Output:** rows 0–15 stored. The expected rows come from `gen_bench.py`'s model of the semantics, which vc2sim matches (`bench_test.py`).
- **Cost:** the bench images grow by 1,824 B (35,344 → 37,168; the PLL build 36,464 → 38,256). The devtest images are regenerated (the bench images only).

**Passes when:** the record matches; readlog prints "the vector bars' forms (jv_bars16's block) as modelled (BARV)".

**If it fails:** the image's start-up check (`viz_bars_init`) already keeps the C bars (`J_VPU_BARS` clear), so the cost is speed, never pixels; readlog says so. To find the form, rerun with the block's ops one at a time: the gradient, the broadcast, the predicated paths, then the selects. Each form is a `quirks.md` finding.

**Status:** built with this change (branch `p19-viz-bars`); runs with the next stage 4 bench pass.

<!-- owner-script: generated by scripts/bench_script.py; do not edit -->

## Owner's script

Generated from `apps/ipodui/devbench.c`, `devbench_power.c` and `devbench.h` by `scripts/bench_script.py` (`--write`); `tests/bench_script/run.sh` fails when this section and the code differ, or when a screen would not fit. Each block is the exact screen: the title in brackets, the text as the iPod wraps it (30 characters), then the buttons; `( )` marks an answer. Times are the steps' budgets (`budget_s`, or the watchdog limit for a hands-off step). Build A configuration: every entry is in.

**Buttons everywhere:** a SELECT line is always the last line: scroll down to it with the wheel, then press SELECT (SELECT elsewhere does nothing). On a question, scroll to an answer and press SELECT; MENU there skips the question (recorded as "Skip"). No screen offers "run it or skip": the session decides by itself (the dependency table below). MENU on a start screen still stops the session quietly; opening Bench session again offers Resume, and that step comes back. A frozen screen: hold MENU and SELECT 6 to 8 s.

### Opening the row

```
[Bench session]
Which session?

Full session: every check,
about 87 min. The first time
on a build.
Re-verify the video image:
about 18 min, for a new video
image after a full session.
Port write check: about 3 min.

Pick with SELECT. MENU:
cancel.
( ) Full session
( ) Re-verify the video image
( ) Port write check
```

If a session is open, this comes first instead:

```
[Bench session]
A session is open: it stopped,
or the iPod restarted.

Resume or Go on: carries on
where it stopped (a step you
left asks its question first).
Start a new session: begins
again; the old results stay
saved.
Cancel (or MENU): leave.
( ) Resume after <last step>
( ) Start a new session
( ) Cancel
```

("Go on: <step>" replaces the first answer when a step you left, or the restart, is waiting for its question.)

### Full session: 48 steps, about 87 min

```
[Bench session]
48 steps, about 87 min.
Wave 2 at the end only if you
say yes there: about 15 min
more.

Have ready: battery charged, a
USB-C cable, the AV cable,
headphones. The battery rows
run unplugged.

You watch two sets of test
pictures first; hands-off
checks run between and after.
All read-only or put back, but
the display program step.

Each step's first screen says
what to do, how long it takes
and what is normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.
MENU: cancel
Scroll here, SELECT: start
```

| # | Step | Kind | Time | Question |
|---|---|---|---|---|
| 1 | `s05` Test pictures | watched | ~5 min | Did the test pictures look right? |
| 2 | `s4b` Fast clock check | hands off | ~3 min | — |
| 3 | `s4c` Fast clock pictures | watched | ~1 min | Did the 3 pictures look right? |
| 4 | `c1` Second core check | hands off | ~2 min | — |
| 5 | `c2` COP row C2 | hands off | ~1 min | — |
| 6 | `c3` COP row C3 | hands off | ~1 min | — |
| 7 | `pb1` Probe PB-1 | hands off | ~2 min | — |
| 8 | `pb4` Probe PB-4 | hands off | ~2 min | — |
| 9 | `pw1` Probe PW1 | hands off | ~2 min | — |
| 10 | `pw2` Probe PW2 | hands off | ~2 min | — |
| 11 | `pw7` Probe PW7 | hands off | ~2 min | — |
| 12 | `pw8` Probe PW8 | hands off | ~2 min | — |
| 13 | `jobs` Chip self-test | hands off | ~30 s | — |
| 14 | `jvpu` Chip maths check | hands off | ~30 s | — |
| 15 | `port` Port speed | hands off | ~10 s | — |
| 16 | `pb2` Probe PB-2 | watched | ~5 min | — |
| 17 | `pb3` Probe PB-3 | watched | ~1 min | — |
| 18 | `s6` Test clips | watched | ~1 min | How did the clips play? |
| 19 | `s7` Video controls | watched | ~3 min | The test closes and reopens the video a few times. Did it come back and keep playing each time? |
| 20 | `vfeed` Video pace | watched | ~2 min | How did the two videos play? |
| 21 | `vbv` Clock bands | watched | ~2 min | Did any play show a garbled picture? |
| 22 | `vgov` Clock governor | watched | ~4 min | Did the clips keep playing smoothly, after each jump and pause too? |
| 23 | `b13` Program swap | watched | ~1 min | Did it blink, blank or flicker before the last dark moment? |
| 24 | `b5` Window test | watched | ~45 s | Was the white frame straight and whole? |
| 25 | `cr1` COMMIT readback | hands off | ~2 min | — |
| 26 | `boot` Display program | watched | ~1 min | — |
| 27 | `rstat` Driver status | hands off | ~30 s | — |
| 28 | `jobs2` Chip self-test | hands off | ~30 s | — |
| 29 | `jvpu2` Chip maths check | hands off | ~30 s | — |
| 30 | `slp` Chip idle sleep | watched | ~2 min | Did the screen stay right? |
| 31 | `lvl5` Chip core level 5 | watched | ~2 min | Did the screen stay right? |
| 32 | `lit24` Lit clock 24 MHz | watched | ~2 min | — |
| 33 | `lit24ui` Menus at 24 MHz | watched | ~3 min | Did the menus stay smooth at 24 MHz? |
| 34 | `ucop` Screens on second core | watched | ~30 s | — |
| 35 | `uck1` Clicks, music stopped | done outside the session | ~1 min | Besides the clicks, did you hear a tone or hum? |
| 36 | `uck2` Clicks with music | done outside the session | ~1 min | Did the music glitch, pop or crackle while you scrolled? |
| 37 | `ufeel` Scrolling feel | done outside the session | ~1 min | Compared with the normal UI, scrolling felt: |
| 38 | `upanel` Panel keeping up | done outside the session | ~45 s | Did the right-hand panel keep up with the highlighted row? |
| 39 | `ucop2` Second core, everyday use | watched | ~3 min | How did the iPod feel on the second core? |
| 40 | `b3` Drift on the chip | done outside the session | ~3 min | Did the covers drift smoothly? |
| 41 | `uviz` Visualiser on the chip | done outside the session | ~2 min | How did the visualiser look? |
| 42 | `respanel` Cover on the chip | done outside the session | ~2 min | How did the album cover look? |
| 43 | `b3ab` Drift A/B | done outside the session | ~12 min | Did both halves run to the end? |
| 44 | `drill1` Drill: restart | done outside the session | ~2 min | Did the screen recover? |
| 45 | `drill2` Drill: job failure | watched | ~2 min | What showed after it? |
| 46 | `drill3` Drill: backlight | done outside the session | ~2 min | Did the drift resume cleanly? |
| 47 | `scroll` Scroll over drift | done outside the session | ~1 min | Was the scroll smooth? |
| 48 | `wave2` Wave 2 | go question | ~10 s | Run wave 2: TV out, voltage, hibernate? Some need the AV cable and a TV; each has its own screens. |
| 49 | `pb6` Probe PB-6 | wave 2 (its own screens) | ~1 min | — |
| 50 | `pb7` Probe PB-7 | wave 2 (its own screens) | ~30 s | — |
| 51 | `pb8` Probe PB-8 | wave 2 (its own screens) | ~2 min | — |
| 52 | `pb9` Probe PB-9 | wave 2 (its own screens) | ~2 min | — |
| 53 | `pw3` Probe PW3 | wave 2 (its own screens) | ~2 min | — |
| 54 | `pw4` Probe PW4 | wave 2 (its own screens) | ~3 min | — |
| 55 | `pw5` Probe PW5 | wave 2 (its own screens) | ~3 min | — |
| 56 | `pw6` Probe PW6 | wave 2 (its own screens) | ~1 min | — |

The screens between steps, in this run:

Before `s4b`:

```
[Hands off now]
Automatic checks next, about 3
min. Put the iPod down: no
buttons are needed.

The screen will flash, show
test pictures or lists, and
may go dark: that's normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.

Screen frozen for over 4 min:
hold MENU and SELECT 6 to 8 s,
then the same.
Scroll here, SELECT: start
```

Before `s4c`:

```
[Automatic checks done]
Automatic checks done: 0
passed, 0 failed, 0 skipped so
far.

Next: a step that needs you.
Scroll here, SELECT: go on
```

(the counts are the session's so far)

Before `c1`:

```
[Hands off now]
Automatic checks next, about
14 min. Put the iPod down: no
buttons are needed.

The screen will flash, show
test pictures or lists, and
may go dark: that's normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.

Screen frozen for over 2 min:
hold MENU and SELECT 6 to 8 s,
then the same.
Scroll here, SELECT: start
```

Before `pb2`:

```
[Automatic checks done]
Automatic checks done: 0
passed, 0 failed, 0 skipped so
far.

Next: a step that needs you.
Scroll here, SELECT: go on
```

(the counts are the session's so far)

Before `cr1`:

```
[Hands off now]
Automatic checks next, about 2
min. Put the iPod down: no
buttons are needed.

The screen will flash, show
test pictures or lists, and
may go dark: that's normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.

Screen frozen for over 3 min:
hold MENU and SELECT 6 to 8 s,
then the same.
Scroll here, SELECT: start
```

Before `boot`:

```
[Automatic checks done]
Automatic checks done: 0
passed, 0 failed, 0 skipped so
far.

Next: a step that needs you.
Scroll here, SELECT: go on
```

(the counts are the session's so far)

Before `rstat`:

```
[Hands off now]
Automatic checks next, about 2
min. Put the iPod down: no
buttons are needed.

The screen will flash, show
test pictures or lists, and
may go dark: that's normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.

Screen frozen for over 1 min:
hold MENU and SELECT 6 to 8 s,
then the same.
Scroll here, SELECT: start
```

Before `slp`:

```
[Automatic checks done]
Automatic checks done: 0
passed, 0 failed, 0 skipped so
far.

Next: a step that needs you.
Scroll here, SELECT: go on
```

(the counts are the session's so far)

### Re-verify the video image: 16 steps, about 18 min

```
[Bench session]
16 steps, about 18 min.
Plus about 6 min if this iPod
needs its test pictures and a
restart first.

Re-verifies a new video image
on this iPod (a full session
ran before). Have ready:
battery charged; nothing to
plug in.

All read-only or put back. The
session picks what to run by
itself: if a step fails, the
steps that need it are
skipped, with one line saying
why.

Each step's first screen says
what to do, how long it takes
and what is normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.
MENU: cancel
Scroll here, SELECT: start
```

| # | Step | Kind | Time | Needs | Question |
|---|---|---|---|---|---|
| 1 | `pre` Start-up check | automatic, no screen, a moment | ~5 s | — | — |
| 2 | `s05` Test pictures | watched; only without the stage 5 flag | ~5 min | — | Did the test pictures look right? |
| 3 | `boot` Display program | watched; only without the stage 5 flag or without the display driver in this start | ~1 min | — | — |
| 4 | `jobs` Chip self-test | hands off; must pass (a restart offered if it cannot run) | ~30 s | — | — |
| 5 | `respanel` Cover on the chip | done outside the session; not needed for the verified line | ~2 min | `jobs` | How did the album cover look? |
| 6 | `uviz` Visualiser on the chip | done outside the session; not needed for the verified line | ~2 min | `jobs` | How did the visualiser look? |
| 7 | `jvpu` Chip maths check | hands off; must pass (a restart offered if it cannot run) | ~30 s | — | — |
| 8 | `c1` Second core check | hands off; not needed for the verified line | ~2 min | — | — |
| 9 | `b13` Program swap | watched; not needed for the verified line | ~1 min | — | Did it blink, blank or flicker before the last dark moment? |
| 10 | `b5` Window test | watched; must pass | ~45 s | — | Was the white frame straight and whole? |
| 11 | `s6` Test clips | watched; must pass | ~1 min | — | How did the clips play? |
| 12 | `vplay` Play a video | watched; not needed for the verified line | ~1 min | `s6` | Did "Preparing Video..." show briefly, then the video play? |
| 13 | `ucop` Screens on second core | watched; not needed for the verified line | ~30 s | `c1` | — |
| 14 | `uck1` Clicks, music stopped | done outside the session; not needed for the verified line | ~1 min | `ucop` | Besides the clicks, did you hear a tone or hum? |
| 15 | `uck2` Clicks with music | done outside the session; not needed for the verified line | ~1 min | `ucop` | Did the music glitch, pop or crackle while you scrolled? |
| 16 | `ufeel` Scrolling feel | done outside the session; not needed for the verified line | ~1 min | `ucop` | Compared with the normal UI, scrolling felt: |
| 17 | `upanel` Panel keeping up | done outside the session; not needed for the verified line | ~45 s | `ucop` | Did the right-hand panel keep up with the highlighted row? |
| 18 | `ucop2` Second core, everyday use | watched; not needed for the verified line | ~3 min | `ucop` | How did the iPod feel on the second core? |

The screens between steps, in this run:

Before `jobs`:

```
[Hands off now]
Automatic checks next, about 1
min. Put the iPod down: no
buttons are needed.

The screen will flash, show
test pictures or lists, and
may go dark: that's normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.

Screen frozen for over 1 min:
hold MENU and SELECT 6 to 8 s,
then the same.
Scroll here, SELECT: start
```

Before `respanel`:

```
[Automatic checks done]
Automatic checks done: 0
passed, 0 failed, 0 skipped so
far.

Next: a step that needs you.
Scroll here, SELECT: go on
```

(the counts are the session's so far)

Before `jvpu`:

```
[Hands off now]
Automatic checks next, about 2
min. Put the iPod down: no
buttons are needed.

The screen will flash, show
test pictures or lists, and
may go dark: that's normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.

Screen frozen for over 2 min:
hold MENU and SELECT 6 to 8 s,
then the same.
Scroll here, SELECT: start
```

Before `b13`:

```
[Automatic checks done]
Automatic checks done: 0
passed, 0 failed, 0 skipped so
far.

Next: a step that needs you.
Scroll here, SELECT: go on
```

(the counts are the session's so far)

**When a step fails** (the gates, `bench_gates[]`): the steps that need it skip by themselves, recorded with `note=auto-skip: needs <id> (<result>)`, and this screen shows once a gate. A gate that ends the session skips every later step. A must-pass step that fails never stops the session by itself: what does not need it still runs, and the summary closes it.

| Gate | Ends the session | Skipped after it |
|---|---|---|
| `s05` | yes | every later step |
| `jobs` | no | `respanel`, `uviz` |
| `s4b` | no | — |
| `s4c` | no | — |
| `s6` | no | `vplay` |
| `c1` | no | `ucop` |
| `ucop` | no | `uck1`, `uck2`, `ufeel`, `upanel`, `ucop2` |

```
[Skipping ahead]
That's all for today: the test
pictures didn't pass, so the
other checks can't run. The
log has what we need.
Scroll here, SELECT: go on
```

```
[Skipping ahead]
Skipping the steps that use
the video chip's jobs: its
self-test didn't pass. The log
has what we need.
Scroll here, SELECT: go on
```

```
[Skipping ahead]
Skipping the fast clock
pictures: the fast clock check
didn't pass.
Scroll here, SELECT: go on
```

```
[Skipping ahead]
Skipping the video start-up
question: without the fast
clock pictures passing, that
check doesn't run.
Scroll here, SELECT: go on
```

```
[Skipping ahead]
Skipping the other video
steps: the test clips didn't
play properly. The log has
what we need.
Scroll here, SELECT: go on
```

```
[Skipping ahead]
Skipping the second-core
steps: the second core check
didn't pass. The log has what
we need.
Scroll here, SELECT: go on
```

```
[Skipping ahead]
Skipping the second-core
steps: the screens didn't move
to the second core. The log
has what we need.
Scroll here, SELECT: go on
```

A must-pass step that could not run for a reason a restart fixes (no display driver in this start; e.g. Chip self-test), offered once:

```
[Chip self-test]
Chip self-test couldn't run in
this start. A restart usually
fixes that, and the step then
runs again.

Restart and retry: sets it up,
then you restart.
Carry on: the session goes on
without it; the steps that
need it are skipped.
( ) Restart and retry
( ) Carry on
```

The end of the session (the counts, what did not pass by title, the verified line when earned):

```
[Bench session]
8 passed, 2 failed, 9 skipped
Didn't pass:
- Test clips
- Screens on second core
Not every step passed:
no verified line this time.
That's all for today. Plug the
iPod into the Mac.
MENU: leave this screen
```

Stopped before the end (MENU on a screen, or left for a step outside the session):

```
[Bench session]
3 passed, 0 failed, 1 skipped
Results so far are saved. To
carry on: open Bench session
again and pick Go on or
Resume.
MENU: leave this screen
```

### Port write check: 2 steps, about 3 min

```
[Bench session]
2 steps, about 3 min.
Plus about 5 min if this iPod
needs its test pictures and a
restart first.

Checks the video chip's port
writes: patterns written, read
back and checked. Hands off
after the start; nothing to
plug in.

Read-only for your music.

If the iPod restarts by
itself: open Bench session
again and pick Resume.
MENU: cancel
Scroll here, SELECT: start
```

| # | Step | Kind | Time | Question |
|---|---|---|---|---|
| 1 | `pre` Start-up check | automatic, no screen, a moment | ~5 s | — |
| 2 | `s05` Test pictures | watched; only without the stage 5 flag | ~5 min | Did the test pictures look right? |
| 3 | `cr1` COMMIT readback | hands off; must pass | ~2 min | — |

The screens between steps, in this run:

Before `cr1`:

```
[Hands off now]
Automatic checks next, about 2
min. Put the iPod down: no
buttons are needed.

The screen will flash, show
test pictures or lists, and
may go dark: that's normal.

If the iPod restarts by
itself: open Bench session
again and pick Resume.

Screen frozen for over 3 min:
hold MENU and SELECT 6 to 8 s,
then the same.
Scroll here, SELECT: start
```

### Every step

#### `pre` Start-up check

- **Kind:** automatic, no screen, a moment. **Time:** ~5 s.
- **MENU and SELECT:** none: no screen.

No screen of its own.

#### `s05` Test pictures

- **Kind:** watched. **Time:** ~5 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Test pictures]
The screen will flash and show
test pictures for about 5 min:
that's normal.

Colour bars, red, green and
blue ramps, a letter F at the
bottom left, a magenta box.
One picture scrolls: its top
bar stays still, except the
clock box. The screen may go
dark between pictures.

Buttons do nothing until the
question. Normal: steady,
clean colours; nothing torn,
shifted or speckled.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Test pictures]
Did the test pictures look
right?
Yes: clean and steady.
Wrong colours: a colour is
off.
Garbled: torn, shifted or
speckled.
Blank: nothing showed.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Wrong colours
( ) Garbled
( ) Blank
```

#### `s4b` Fast clock check

- **Kind:** hands off. **Time:** ~3 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `s4c` Fast clock pictures

- **Kind:** watched. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Fast clock pictures]
The screen will flash and show
test pictures for about 1 min:
that's normal.

The video chip runs at its
fast clock with the screen on.
Three pictures, 8 s each:
1. colour bars, ramps, an F;
2. the same with a magenta box
at the top left;
3. back at the normal clock,
no box.
Then the normal screen
returns.

Buttons do nothing until the
question. Normal: all three as
clean as the earlier test
pictures.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Fast clock pictures]
Did the 3 pictures look right?
Yes: clean. Videos may then
use the fast clock.
Wrong colours: a colour is
off.
Garbled: speckles, streaks or
shifted lines.
Blank: nothing showed.
Any answer but Yes keeps
videos at the normal clock.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Wrong colours
( ) Garbled
( ) Blank
```

#### `c1` Second core check

- **Kind:** hands off. **Time:** ~2 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `c2` COP row C2

- **Kind:** hands off. **Time:** ~1 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `c3` COP row C3

- **Kind:** hands off. **Time:** ~1 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `pb1` Probe PB-1

- **Kind:** hands off. **Time:** ~2 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `pb4` Probe PB-4

- **Kind:** hands off. **Time:** ~2 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `pw1` Probe PW1

- **Kind:** hands off. **Time:** ~2 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `pw2` Probe PW2

- **Kind:** hands off. **Time:** ~2 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `pw7` Probe PW7

- **Kind:** hands off. **Time:** ~2 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `pw8` Probe PW8

- **Kind:** hands off. **Time:** ~2 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `jobs` Chip self-test

- **Kind:** hands off. **Time:** ~30 s.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `jvpu` Chip maths check

- **Kind:** hands off. **Time:** ~30 s.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `port` Port speed

- **Kind:** hands off. **Time:** ~10 s.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `pb2` Probe PB-2

- **Kind:** watched. **Time:** ~5 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PB-2]
About 5 min. Reads the video
chip's pins in 7 states. Have
the USB-C cable, the AV cable
and headphones at hand.

7 prompts follow, one per
state (cables, music, Hold).
Do what each says, then scroll
to its last line and press
SELECT. MENU on a prompt ends
this probe early; the session
goes on. No question at the
end.
Scroll here, SELECT: start
```

#### `pb3` Probe PB-3

- **Kind:** watched. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PB-3]
About 1 min. Reads the video
chip's USB status bit with the
USB-C cable in, then out (a
charger or a computer; the
iPod only charges).

2 prompts follow: plug in,
then unplug. Do it, then
scroll to the prompt's last
line and press SELECT. MENU on
a prompt ends this probe
early; the session goes on. No
question.
Scroll here, SELECT: start
```

#### `s6` Test clips

- **Kind:** watched. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Test clips]
The screen will go dark, then
test clips play for about 1
min: that's normal. Music
stops.

Clip 1 plays twice: colour
bars, a counter 0 to 7 and a
steady tone (8 s). Then clip
2: 13 s of real film with fast
pans and cuts.

No buttons: it plays to the
end by itself. Normal: smooth
motion with no skips, sound in
step with the picture.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Test clips]
How did the clips play?
Smooth, in sync: normal.
Stutter: jerky or frozen
moments.
Sound off picture: out of
step.
No picture: nothing showed.
Pick an answer with SELECT.
MENU: no answer.
( ) Smooth, in sync
( ) Stutter
( ) Sound off picture
( ) No picture
```

#### `s7` Video controls

- **Kind:** watched. **Time:** ~3 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Video controls]
About 3 min, hands off. The
iPod plays the test video and
works its own controls: pause,
jump ahead, back to the start,
the screen dark for 3 s and
back, then it closes the video
and opens it again where it
was. That's the test.

Then the video restarts six
times, 8 s each, to measure
settings; two of those run on
purpose at a slow clock and
may stutter. Normal: after
each step the picture and
sound come back within a
moment.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Video controls]
The test closes and reopens
the video a few times. Did it
come back and keep playing
each time?
Yes: every time.
Froze or error: it stalled or
showed an error.
Didn't come back: it stayed
closed.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Froze or error
( ) Didn't come back
```

#### `vfeed` Video pace

- **Kind:** watched. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Video pace]
The screen will go dark, then
two videos play for about 1
min: that's normal. Music
stops.

The 13 s film clip from the
test clips, then a 40 s
converted clip, each to the
end by itself.

No buttons. Normal: smooth
motion, no jumps or frozen
moments, sound in step with
the picture in both.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Video pace]
How did the two videos play?
Smooth: steady motion, in
sync.
Stutter: jerky, jumps or
freezes.
Sound off picture: out of
step.
No picture: nothing showed.
Pick an answer with SELECT.
MENU: no answer.
( ) Smooth
( ) Stutter
( ) Sound off picture
( ) No picture
```

#### `vbv` Clock bands

- **Kind:** watched. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Clock bands]
The screen will go dark, then
a clip plays 11 times over
about 2 min: that's normal.
Music stops.

Clip 1 plays 8 s at each of
the video chip's clock bands,
slowest first. At the slow
ones the picture may stutter
or freeze: expected.

No buttons. Normal: every play
shows the bars and the counter
moving, no garbled picture.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Clock bands]
Did any play show a garbled
picture?
No: normal.
Yes, garbled: broken or wrong
picture.
No picture at all: a play
stayed blank.
Pick an answer with SELECT.
MENU: no answer.
( ) No
( ) Yes, garbled
( ) No picture at all
```

#### `vgov` Clock governor

- **Kind:** watched. **Time:** ~4 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Clock governor]
The screen will go dark, then
clips play for about 3 min:
that's normal. Music stops.

Each clip plays three times
for 20 s: at full clock, then
with the clock following the
load two ways. Every play
jumps back once at 5 s and
pauses for 1 s at 10 s.

No buttons. Normal: all plays
of a clip look and sound the
same: smooth, in step, a short
jump and a short pause.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Clock governor]
Did the clips keep playing
smoothly, after each jump and
pause too?
Yes: smooth throughout.
Stutter: jerky or skipping at
times.
Sound off picture: out of
step.
No picture: a play stayed
blank.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Stutter
( ) Sound off picture
( ) No picture
```

#### `vplay` Play a video

- **Kind:** watched. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); after arming, MENU or SELECT closes the screen; question: SELECT picks; MENU skips it.

```
[Play a video]
Done outside the session,
under 1 min. From the main
menu open Videos and play any
video. "Preparing Video..."
shows for about a second
first: that's normal (a
one-off check, run again for
this test).

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Play a video; the question
comes first.
Scroll here, SELECT: start
```

After it (the session leaves):

```
[Play a video]
Set. Do the rest outside the
session, as the first screen
said.

Then: Developer > Bench
session, and pick Go on: Play
a video. The question comes
first.
MENU: close
Scroll here, SELECT: close
```

The question (when you come back and pick Go on):

```
[Play a video]
Did "Preparing Video..." show
briefly, then the video play?
Yes: briefly, then it played.
No message: it just played.
Stuck: it stayed on that
screen.
No video: it didn't play.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) No message
( ) Stuck
( ) No video
```

#### `b13` Program swap

- **Kind:** watched. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Program swap]
The screen will go dark once
at the end, under 1 min from
now: that's normal.

The video chip swaps between
its video and display programs
up to 20 times (10 each way)
with the screen on, and stops
at the first swap that fails.
Each swap back adds one small
square, green then red in
turn, a step to the right near
the bottom. No video plays.

No buttons. Normal: no blink,
blank or flicker before that
last dark moment.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Program swap]
Did it blink, blank or flicker
before the last dark moment?
No: normal.
Yes, briefly: once, short.
Yes, badly: often or long.
Pick an answer with SELECT.
MENU: no answer.
( ) No
( ) Yes, briefly
( ) Yes, badly
```

#### `b5` Window test

- **Kind:** watched. **Time:** ~45 s.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Window test]
The screen will flash for
under 1 min: that's normal.

Windows flash by in the same
spot (one picture sent 31
times from three differently
laid-out copies), then the
last stays 5 s: a white frame
and a white diagonal over red
and blue stripes.

No buttons. Normal: the
flashes, then that last
picture, the frame straight
and whole, the diagonal
unbroken.
Scroll here, SELECT: start
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Window test]
Was the white frame straight
and whole?
Yes: normal.
Sheared: slanted or shifted
rows.
Broken: gaps or garbage.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Sheared
( ) Broken
```

#### `cr1` COMMIT readback

- **Kind:** hands off. **Time:** ~2 min.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `boot` Display program

- **Kind:** watched. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); then MENU or SELECT closes the restart screen.

```
[Display program]
The iPod will restart once:
that's normal.

A few seconds: our video
program drives the screen from
the next start on (it stays,
on purpose). Then you restart
the iPod.

After it: hold MENU and SELECT
together 6 to 8 s. After the
restart: Developer > Bench
session, then pick Go on:
Display program.
Scroll here, SELECT: start
```

After it:

```
[Display program]
Set for the next start.

Now restart: hold MENU and
SELECT together 6 to 8 s,
until the screen goes dark.

After the restart: Developer >
Bench session, then pick Go
on: Display program.
MENU: close
Scroll here, SELECT: close
```

At Go on, if this is still the start that asked for the restart (nothing is recorded; the step stays open):

```
[No restart seen]
No restart seen: this is still
the start the step asked you
to leave.

Hold MENU and SELECT together
6 to 8 s until the screen goes
dark.

After the restart: Developer >
Bench session, then pick Go
on: Display program.
MENU: close
Scroll here, SELECT: close
```

#### `rstat` Driver status

- **Kind:** hands off. **Time:** ~30 s.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `jobs2` Chip self-test

- **Kind:** hands off. **Time:** ~30 s.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `jvpu2` Chip maths check

- **Kind:** hands off. **Time:** ~30 s.
- **MENU and SELECT:** none of its own: the "Hands off now" screen comes first (SELECT starts the checks); buttons do nothing while it runs.

No screen of its own.

#### `slp` Chip idle sleep

- **Kind:** watched. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); in a phase: MENU ends the row early (skipped, no question; the session goes on); SELECT and the wheel do nothing; question: SELECT picks; MENU skips it.

```
[Chip idle sleep]
About 80 s, on battery (unplug
the iPod). 4 phases of 20 s, A
B A B. A: the video chip polls
(today). B: it sleeps between
ticks. The screen stays still
and shows the phase. Leave it
alone until the question
appears.

It checks itself: in B the
chip wakes on every tick,
sleeps, and still answers.
Through B the iPod also rings
the chip's doorbell at random
moments and counts them; a
lost one fails the row.
MENU in a phase: ends the row
early (skipped, no question).
Scroll here, SELECT: start
```

Each of the 4 phases (the first shown; the screen is drawn once a phase and stays still; B phases show "B: chip sleeps between ticks"):

```
Chip idle sleep

Phase 1 of 4 (A), 20 s:
A: chip polls (today)

Leave it alone until the
row ends. Keep the iPod
unplugged (on battery).

MENU: ends this row early
(skipped; the session goes
on). Other buttons: nothing.
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Chip idle sleep]
Did the screen stay right?
Yes: still and clean.
Garbled: noise or wrong
pixels.
Froze: stuck after the row.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Garbled
( ) Froze
```

#### `lvl5` Chip core level 5

- **Kind:** watched. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); in a phase: MENU ends the row early (skipped, no question; the session goes on); SELECT and the wheel do nothing; question: SELECT picks; MENU skips it.

```
[Chip core level 5]
About 80 s after a go
question, on battery. 4 phases
of 20 s, A B A B. A: the video
chip's core at level 11
(today). B: level 5. The
screen stays still. Leave it
alone until the question
appears.

It checks itself: level 5
holds and the job self-test
passes at it.
MENU in a phase: ends the row
early (skipped, no question).
Scroll here, SELECT: start
```

First, inside the step, the go question:

```
[Chip core level 5]
Run the core voltage test? For
half of its 80 s the video
chip's core runs at level 5
(Apple's value), lower than
today's 11.

Risk: the screen may garble or
freeze; holding MENU and
SELECT restarts the iPod and
undoes it.

Yes, run it: starts it. Not
now (or MENU): skips this row;
the session goes on.
( ) Yes, run it
( ) Not now
```

Each of the 4 phases (the first shown; the screen is drawn once a phase and stays still; B phases show "B: core at level 5"):

```
Chip core level 5

Phase 1 of 4 (A), 20 s:
A: core at level 11 (today)

Leave it alone until the
row ends. Keep the iPod
unplugged (on battery).

MENU: ends this row early
(skipped; the session goes
on). Other buttons: nothing.
```

The question (asked only if the step ran; a skipped step asks nothing):

```
[Chip core level 5]
Did the screen stay right?
Yes: still and clean.
Garbled: noise or wrong
pixels.
Froze: stuck after the row.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Garbled
( ) Froze
```

#### `lit24` Lit clock 24 MHz

- **Kind:** watched. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); in a phase: MENU ends the row early (skipped, no question; the session goes on); SELECT and the wheel do nothing.

```
[Lit clock 24 MHz]
Logs only, about 80 s, on
battery (USB keeps the clock
at 30 MHz). 4 phases of 20 s,
A B A B: the iPod's clock at
30 MHz (today), then 24 MHz.
The screen stays still; leave
it alone. No question.

MENU in a phase: ends the row
early (skipped).
Scroll here, SELECT: start
```

Each of the 4 phases (the first shown; the screen is drawn once a phase and stays still; B phases show "B: clock at 24 MHz"):

```
Lit clock 24 MHz

Phase 1 of 4 (A), 20 s:
A: clock at 30 MHz (today)

Leave it alone until the
row ends. Keep the iPod
unplugged (on battery).

MENU: ends this row early
(skipped; the session goes
on). Other buttons: nothing.
```

#### `lit24ui` Menus at 24 MHz

- **Kind:** watched. **Time:** ~3 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); after arming, MENU or SELECT closes the screen; question: SELECT picks; MENU skips it.

```
[Menus at 24 MHz]
About 3 min. SELECT leaves the
session and starts counting
missed frames: 1 min at 30
MHz, then 1 min at 24 (the
clock changes by itself;
nothing shows). In each minute
watch the covers drift in the
main menu for 30 s, then open
Songs and turn the wheel fast
to the bottom and back.

Then open Bench session again
and pick Go on: Menus at 24
MHz; the question comes first
and puts the clock back to 30.
It fails if 24 MHz misses more
frames than 30. Normal: as
smooth as ever.
Scroll here, SELECT: start
```

After it (the session leaves):

```
[Menus at 24 MHz]
Set. Do the rest outside the
session, as the first screen
said.

Then: Developer > Bench
session, and pick Go on: Menus
at 24 MHz. The question comes
first.
MENU: close
Scroll here, SELECT: close
```

The question (when you come back and pick Go on):

```
[Menus at 24 MHz]
Did the menus stay smooth at
24 MHz?
Yes: as smooth as ever.
Drift stuttered: jerky covers.
Scroll lagged: the list fell
behind the wheel.
Hung: stuck; you restarted it.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Drift stuttered
( ) Scroll lagged
( ) Hung
```

#### `ucop` Screens on second core

- **Kind:** watched. **Time:** ~30 s.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); then MENU or SELECT closes the restart screen.

```
[Screens on second core]
The iPod will restart once:
that's normal.

About 4 min in all. The iPod's
screens and menus will run on
its second core from the next
start. SELECT sets that; then
you restart.

After it: hold MENU and SELECT
together 6 to 8 s. After the
restart: Developer > Bench
session, then pick Go on:
Clicks, music stopped.
Scroll here, SELECT: start
```

After it:

```
[Screens on second core]
Set for the next start.

Now restart: hold MENU and
SELECT together 6 to 8 s,
until the screen goes dark.

After the restart: Developer >
Bench session, then pick Go
on: Screens on second core.
MENU: close
Scroll here, SELECT: close
```

At Go on, if this is still the start that asked for the restart (nothing is recorded; the step stays open):

```
[No restart seen]
No restart seen: this is still
the start the step asked you
to leave.

Hold MENU and SELECT together
6 to 8 s until the screen goes
dark.

After the restart: Developer >
Bench session, then pick Go
on: Screens on second core.
MENU: close
Scroll here, SELECT: close
```

#### `uck1` Clicks, music stopped

- **Kind:** done outside the session. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Clicks, music stopped]
Done outside the session,
under 1 min. With Settings >
Clicker on and no music
playing: in Music > Songs
scroll up and down for 20 s,
then stop and listen for 5 s.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Clicks, music stopped; the
question comes first. Normal:
only the clicks, then silence.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Clicks, music stopped]
Besides the clicks, did you
hear a tone or hum?
No: only clicks, then quiet.
Faint: a quiet tone or hum.
Clear: an easy-to-hear tone or
hum.
Pick an answer with SELECT.
MENU: no answer.
( ) No
( ) Faint
( ) Clear
```

#### `uck2` Clicks with music

- **Kind:** done outside the session. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Clicks with music]
Done outside the session,
about 1 min. Play a song from
Music > Songs, press MENU back
to the main menu, and scroll
up and down for 20 s while it
plays.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Clicks with music; the
question comes first. Normal:
the music plays on cleanly
under the clicks.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Clicks with music]
Did the music glitch, pop or
crackle while you scrolled?
Never: clean the whole time.
Once or twice: a rare glitch.
Often: glitches again and
again.
Pick an answer with SELECT.
MENU: no answer.
( ) Never
( ) Once or twice
( ) Often
```

#### `ufeel` Scrolling feel

- **Kind:** done outside the session. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Scrolling feel]
Done outside the session,
under 1 min. Scroll Music >
Songs fast, up and down, then
open the Developer menu and
scroll it.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Scrolling feel; the question
comes first. Compare it with
how the iPod normally feels.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Scrolling feel]
Compared with the normal UI,
scrolling felt:
Same or faster: as quick, or
quicker.
A little slower: a slight lag.
Much slower: clearly laggy.
Pick an answer with SELECT.
MENU: no answer.
( ) Same or faster
( ) A little slower
( ) Much slower
```

#### `upanel` Panel keeping up

- **Kind:** done outside the session. **Time:** ~45 s.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Panel keeping up]
Done outside the session,
under 1 min. With music
playing, go to the main menu
and spin the wheel fast up and
down for 10 s.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Panel keeping up; the question
comes first. Normal: the
right-hand panel follows the
highlighted row.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Panel keeping up]
Did the right-hand panel keep
up with the highlighted row?
Kept up: always the right
picture.
A little behind: caught up
within a moment.
Far behind: lagged well behind
or stuck.
Pick an answer with SELECT.
MENU: no answer.
( ) Kept up
( ) A little behind
( ) Far behind
```

#### `ucop2` Second core, everyday use

- **Kind:** watched. **Time:** ~3 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); after arming, MENU or SELECT closes the screen; question: SELECT picks; MENU skips it.

```
[Second core, everyday use]
About 3 min. SELECT leaves the
session. For 2 min use the
iPod: browse Music, play an
MP3, open Now Playing, turn
the wheel fast through Songs
and back.

Then open Bench session again
and pick Go on: Second core,
everyday use; the question
comes first. It fails on a
stall, a coherence error or a
slow call. Normal: as quick as
ever. A hang: hold MENU and
SELECT; the next start runs
the screens on the main core.
Scroll here, SELECT: start
```

After it (the session leaves):

```
[Second core, everyday use]
Set. Do the rest outside the
session, as the first screen
said.

Then: Developer > Bench
session, and pick Go on:
Second core, everyday use. The
question comes first.
MENU: close
Scroll here, SELECT: close
```

The question (when you come back and pick Go on):

```
[Second core, everyday use]
How did the iPod feel on the
second core?
Normal: quick, nothing odd.
Slow: lag or stutter.
Glitches: wrong pixels or
text.
Hung: stuck; you restarted it.
Pick an answer with SELECT.
MENU: no answer.
( ) Normal
( ) Slow
( ) Glitches
( ) Hung
```

#### `b3` Drift on the chip

- **Kind:** done outside the session. **Time:** ~3 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Drift on the chip]
Done outside the session,
about 3 min. In Developer >
Advanced > Video chip, press
BCM jobs: drift on video chip
until its screen says on. Then
go to the main menu and watch
the covers drift for 2 min.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Drift on the chip; the
question comes first. Normal:
a slow, smooth drift and
cross-fades that never stop (a
still panel was the fault this
build fixes).
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Drift on the chip]
Did the covers drift smoothly?
Yes: smooth.
Stutter: jerky.
Frozen: they stopped.
Blank: no covers.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Stutter
( ) Frozen
( ) Blank
```

#### `uviz` Visualiser on the chip

- **Kind:** done outside the session. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Visualiser on the chip]
Done outside the session,
about 2 min. Play a song (not
an audiobook), open Now
Playing and press SELECT until
the visualiser fills the
screen. Watch it 1 min, then
press Next once and watch 20 s
more.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Visualiser on the chip; the
question comes first. Normal:
smooth motion, a short pause
at the new song, then moving
again.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Visualiser on the chip]
How did the visualiser look?
Smooth: moving all along, back
after the new song.
Stuttered: jerky or frozen at
times.
Stuck after Next: did not move
again.
Never showed: no visualiser.
Pick an answer with SELECT.
MENU: no answer.
( ) Smooth
( ) Stuttered
( ) Stuck after Next
( ) Never showed
```

#### `respanel` Cover on the chip

- **Kind:** done outside the session. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Cover on the chip]
Done outside the session,
about 2 min. Play an album you
have not played on this build.
Rest on the main menu's Now
Playing row 20 s, go to Music
10 s, come back 10 s. Then let
the screen go dark and wake it
with MENU.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Cover on the chip; the
question comes first. Normal:
the cover appears sharp, stays
put, and comes back after the
wake.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Cover on the chip]
How did the album cover look?
Sharp, steady: normal.
Blurry or late: soft, or long
to come.
Wrong picture: garbled or
another cover.
Missing: no cover.
Pick an answer with SELECT.
MENU: no answer.
( ) Sharp, steady
( ) Blurry or late
( ) Wrong picture
( ) Missing
```

#### `b3ab` Drift A/B

- **Kind:** done outside the session. **Time:** ~12 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Drift A/B]
Done outside the session,
about 12 min. Developer >
Power log: on. Drift on video
chip: on (as before); watch
the main menu 5 min. Then
press it until it says off;
watch 5 min more.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Drift A/B; the question comes
first. Normal: the menu drifts
the whole time.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Drift A/B]
Did both halves run to the
end?
Yes: both.
On half failed: it stopped
while on.
Off half failed: it stopped
while off.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) On half failed
( ) Off half failed
```

#### `drill1` Drill: restart

- **Kind:** done outside the session. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Drill: restart]
Done outside the session,
about 2 min. In Developer >
Advanced > Video chip > Fault
injection, press BCM boot
path: next wake never answers.
Set the drift on, go to the
main menu, let the screen go
dark, then press MENU once to
wake it.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Drill: restart. Normal: no
freeze, a dark blink at most.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Drill: restart]
Did the screen recover?
Yes: the menu came back.
Still cover: a still picture,
no drift.
Hang: stuck; you restarted it.
Reboot: it restarted by
itself.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Still cover
( ) Hang
( ) Reboot
```

#### `drill2` Drill: job failure

- **Kind:** watched. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); after arming, MENU or SELECT closes the screen; question: SELECT picks; MENU skips it.

```
[Drill: job failure]
About 2 min. SELECT arms it:
the next two cross-fades fail
on purpose. The session then
leaves. With the drift on,
watch the main menu 1 min.

Then open Bench session again
and pick Go on: Drill: job
failure; the question comes
first. Normal: no visible
change, or a still cover.
Scroll here, SELECT: start
```

After it (the session leaves):

```
[Drill: job failure]
Set. Do the rest outside the
session, as the first screen
said.

Then: Developer > Bench
session, and pick Go on:
Drill: job failure. The
question comes first.
MENU: close
Scroll here, SELECT: close
```

The question (when you come back and pick Go on):

```
[Drill: job failure]
What showed after it?
Drift or still cover: normal.
Frozen: the menu stuck.
Blank: nothing showed.
It restarted: by itself.
Pick an answer with SELECT.
MENU: no answer.
( ) Drift or still cover
( ) Frozen
( ) Blank
( ) It restarted
```

#### `drill3` Drill: backlight

- **Kind:** done outside the session. **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Drill: backlight]
Done outside the session,
about 2 min. With the drift
on, in the main menu, let the
screen go dark during a
cross-fade (as the cover
changes), then press MENU once
to wake it.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Drill: backlight. Normal: the
drift carries on.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Drill: backlight]
Did the drift resume cleanly?
Yes: it carried on.
Stuck frame: a frozen picture.
Hang: stuck; you restarted it.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Stuck frame
( ) Hang
```

#### `scroll` Scroll over drift

- **Kind:** done outside the session. **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line leaves the session to do it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume); question: SELECT picks; MENU skips it.

```
[Scroll over drift]
Done outside the session, 1
min. With the drift on, open a
long list (Songs) and turn the
wheel fast to the bottom and
back.

SELECT here leaves the
session. Then open Bench
session again and pick Go on:
Scroll over drift. Normal:
smooth, nothing left behind.
Scroll here, SELECT: leave
```

The question (when you come back and pick Go on):

```
[Scroll over drift]
Was the scroll smooth?
Yes: smooth.
Tearing: split pictures.
Stale pixels: bits left
behind.
Stutter: jerky.
Pick an answer with SELECT.
MENU: no answer.
( ) Yes
( ) Tearing
( ) Stale pixels
( ) Stutter
```

#### `wave2` Wave 2

- **Kind:** go question. **Time:** ~10 s.
- **MENU and SELECT:** SELECT picks an answer; MENU skips it (= Not now).

The question:

```
[Wave 2]
Run wave 2: TV out, voltage,
hibernate? Some need the AV
cable and a TV; each has its
own screens.
Not now: skips all of it.
Yes, run it: runs them, one by
one.
About 15 min, 8 checks.
Pick an answer with SELECT.
MENU: no answer.
( ) Not now
( ) Yes, run it
```

#### `pb6` Probe PB-6

- **Kind:** wave 2 (its own screens). **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PB-6]
Under 1 min. Needs the AV
cable and a TV. Shows Apple's
own TV test pattern; you pick
NTSC or PAL first.

Its own screens follow: MENU
there cancels just this check.
At the end it asks what the TV
showed.
Scroll here, SELECT: start
```

#### `pb7` Probe PB-7

- **Kind:** wave 2 (its own screens). **Time:** ~30 s.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PB-7]
Under 1 min. TV timing with
nothing plugged in; nothing to
watch.

Its own screens follow: MENU
there cancels just this check.
Scroll here, SELECT: start
```

#### `pb8` Probe PB-8

- **Kind:** wave 2 (its own screens). **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PB-8]
About 2 min. Needs the AV
cable and a TV: the chip's TV
output in a few formats.

Its own screens follow: MENU
there cancels just this check.
It asks what the TV showed.
Locked until PB-7 passed.
Scroll here, SELECT: start
```

#### `pb9` Probe PB-9

- **Kind:** wave 2 (its own screens). **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PB-9]
About 2 min. Needs the AV
cable and a TV: the picture at
two widths.

Its own screens follow: MENU
there cancels just this check.
Locked until PB-8 passed.
Scroll here, SELECT: start
```

#### `pw3` Probe PW3

- **Kind:** wave 2 (its own screens). **Time:** ~2 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PW3]
About 2 min, hands off: the
chip's core voltage steps
through Apple's levels and
back.

Its own screens follow: MENU
there cancels just this check.
Scroll here, SELECT: start
```

#### `pw4` Probe PW4

- **Kind:** wave 2 (its own screens). **Time:** ~3 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PW4]
About 3 min, hands off: the
chip's clock and voltage step
up and back. Locked until PW3
passed.

Its own screens follow: MENU
there cancels just this check.
Scroll here, SELECT: start
```

#### `pw5` Probe PW5

- **Kind:** wave 2 (its own screens). **Time:** ~3 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PW5]
About 3 min, hands off: the
chip hibernates and wakes; the
screen may go dark. Locked
until PW1 passed.

Its own screens follow: MENU
there cancels just this check.
Scroll here, SELECT: start
```

#### `pw6` Probe PW6

- **Kind:** wave 2 (its own screens). **Time:** ~1 min.
- **MENU and SELECT:** start screen: SELECT on the last line starts it (no MENU line: MENU still stops the session quietly, and this step comes back at the next Resume).

```
[Probe PW6]
Under 1 min, hands off: reads
the screen's state with the
chip off.

Its own screens follow: MENU
there cancels just this check.
Scroll here, SELECT: start
```

**Fit:** the longest screen has 27 rows (one screen shows 28); every line is at most 30 characters.

<!-- owner-script: end -->

## The battery rows: slp, lvl5, lit24 (branch `p19-battery`)

**What:** the three lit-power levers of `power-contract.md` §7, as **viability checks** in part B after `jvpu2`: does the lever work, and does it keep things smooth. **No meter and no power claims** (owner, 2026-10-25: "We won't be doing meter-based tests for a long time"; "just do performance tests for viability"). Rows in `apps/ipodui/devbench_power.c`; the reasoning and the model's savings stay in `battery-levers.md`, unmeasured.

**Setup (owner):** nothing. The rows run **on battery** (unplugged), so `power.csv`'s voltage trend is the iPod's own draw; it is logged, never a verdict. USB would also hold the PP at 30 MHz.

**Each row:** its start screen, then four phases of 20 s, A B A B (`BPW_PHASES`, `BPW_PHASE_S`). The screen shows the phase and stays still. MENU in a phase ends the row early: put back as found, recorded skipped, no question. The rows pass or fail on their own read-backs:

| Row | A | B | Passes when | Lines |
|---|---|---|---|---|
| `slp` | idle gate off | gate on | the chip sleeps and wakes on time: in each B phase its tick count is within 10 % of the milliseconds, it slept, no sleep disturbed r0–r5, it still answers every read (a read that fails is a FAIL), and no doorbell was lost (the PP rings at random 0.1 to 0.8 s intervals through B; the image's `bells` delta must be at least the rings); then "Did the screen stay right?" | `slp A1 ms= ticks= bells= sleeps= bad=` per phase; `slp B1 rings= bells= lost= via=fence` |
| `lvl5` | level found (11) | level 5 | level 5 reads back in both B phases and the job self-test passes at it; a go question first (the risk); then "Did the screen stay right?" | `lvl5 B1 reg 000000B5 (level 5) jobs self-test 0` |
| `lit24` | 30 MHz | 24 MHz | logs only: B reaches 24 MHz with no hold, A 30 or more | `lit24 B1 MHz=24 holds=0 policy=on` |
| `lit24ui` | — | 24 MHz | the UI stays responsive at 24 MHz: the step sets the lit clock to 24, leaves the session, the first minute stays at 30 MHz (phase A), the first frame after it sets 24 (phase B); in each minute the owner watches the drift 30 s and scrolls Songs fast, comes back, and the step judges the counters before it asks; the answer puts the clock back to 30 (a restart does too: not saved). Passes when B's misses per 1000 frames are at most A's plus 20, for the drift and for the scroll, and each phase had at least 30 drift and 10 scroll frames | `lit24ui A30 drift_miss= scroll_miss= frames=<drift>/<scroll>` and `lit24ui B24 ...`; `clk lit` lines in `ipodui.log` |

`slp`'s doorbell count: in each B phase the PP rings a FENCE (the job driver's own doorbell path; a bare `BCM_CONTROL = 0x31` with no ring record when the driver is down, which makes the image wake, find nothing new and sleep) at random intervals, and counts it; the image's `bells` delta may exceed the rings (the UI's own traffic rings too) but never fall short, and `bad` must not move. `lit24ui`'s missed frames (`apps/ipodui/litmiss.h`, developer builds only): a drift step is missed per half step skipped and per step posted over 2 ticks late; a scroll or slide frame is missed when posted a whole 3-tick frame slot or more past its due time. Each hook is a flag test and an increment. Both are viability checks, never power measurements.

**power.csv marks:** `slp A1` … `slp B2`, then `slp` (the end); likewise `lvl5 …`, `lit24 …` (and the switch's own `lit 24`/`lit 30`).

**What it decides:** #3 (the tick wakes the core: `slp`'s ticks) and #11/#12 for level 5, as works / doesn't. The defaults stay off until a measurement shows a saving.

### Sitting 3 run sheet

Image batch 3 (`docs/research/bcm2722/merged-image-batch.md` §7) on a developer build (build A's config). **Re-verify mode, one sitting**: 14 steps, **about 19 min** from the steps' budgets (the confirm screen shows the same sum), of which about 6 min hands-off (`jobs`, `jvpu`, `c1`, `s4b`), so **about 13 min of the owner's time**. Trimmed from 17 (2026-10-26): `drill2`, `s7` and `vbv` wait for a later sitting; the full session still runs them. If `pre` finds the stage 4 and 5 flags void, `s05` and `boot` add 6 min. The exact screens are in **Owner's script** above.

1. **Have ready:** battery charged, a USB-C cable for the pull afterwards; the zip's `bcmvideo.bin` (merged), `dec-ovl.bin`, `bcmprobe.bin`, `bcmtest/clip.mp4`, `clip2.mp4` and the test video for `vfeed`. The session sets `bcmjobs.on` itself. An album never played on this build (for `respanel`).
2. Settings › About, hold SELECT + Previous 3 s, then Developer › **Bench session**, pick **Re-verify the video image**, read the confirm screen, scroll to its last line, SELECT.
3. Hands off: `pre` (and `s05`, `boot` only if it fails), `jobs`, `jvpu`, `c1`, `s4b`; then `s4c` (three 150 MHz pictures; answer).
4. Outside the session, each followed by **Go on**: `b3`, `respanel`.
5. Watched, one answer each: `s6`, `vfeed`, `b13`, `b5`.
6. Last: `ucop` (a restart), then `ucop2` (2 min of normal use, outside; then Go on).
7. The summary screen; plug into the Mac; the agent pulls the results and logs (`scripts/bench_ingest.py --commit <build>`).

**What each step should show** (the merged image `2c6dc03a`, 214,772 B; `dec-ovl.bin` `8be7f8d2`; the display-only image `f45ecffc`):

| Step | Role | Budget | Expected | Gates the merged line |
|---|---|---|---|---|
| `pre` | pre-flight | 5 s | PASS: the stage 4/5 flags still name this build's unchanged bench and DMA images (batch 3 changed none of them); otherwise `s05` + `boot` run | — |
| `jobs` | NEED | 30 s | `jobs: self-test rc 0` | yes |
| `jvpu` | NEED | 30 s | `J_VPU 3, expected 3` (CHECKED, SHIFT; MAC and YUV come only with the DECODE overlay, which loads later, while Now Playing shows the panel) | yes |
| `c1` | ALSO | 90 s | engine 2 started from the COP: PASS (19-9's C1) | no |
| `s4b`, `s4c` | ALSO | 3 + 1 min | both PASS, flags written; `s4c` "Yes" (the clock flag `s6` plays at) | no |
| `b3` | OPT | 3 min | the drift on the chip: `bcmjob up … vpu=0x3 expect=0x3`, COMMITs accepted, no `FALLBACK` | no (must not fail) |
| `respanel` | ALSO | 2 min | RESPANEL: `bcmjob overlay load k=1 bytes=23884`, `bcmjob overlay ok k=1 … mac=vector yuv=vector lent=1`; `drift cover park`; `art panel bcm dec_ms=…`; `panel resident set key=…`; `resident show us=N rc=0` with N under 33,333; off the row `art panel bcm dec_ms=0 read_ms=M ok=1`; after the wake the panel decoded again; **never** `FAULT art bcm read`, `bcmjob fault op=0x30`, `why=nowork`; answer "Sharp, steady" | no |
| `s6` | NEED | 1 min | "Smooth, in sync"; `chip MHz (150`, 0 skipped, A/V in bounds; **its line earns the clock150 token for `2c6dc03a`** | yes |
| `vfeed` | ALSO | 1 min | the pace rule (p95 FEED < 75 % of the frame, 0 skips, A/V ≤ 50 ms) on the film clip and the test video; the film clip's p95 near 25 ms or less (sitting 2: 27.4 ms on `80bc8611`; vc2sim predicts about 10 % less on batch 3's decoder) | no |
| `b13` | ALSO | 1 min | the warm swap 20 of 20: PARK within 60 ms each round (`PRK`), no blink before the last dark restart, "No" | no (gates only the display-only image's swap path) |
| `b5` | NEED | 45 s | 31 pushes by DMA of 31, the white frame whole ("Yes"); `lab5` J_VPU `0x27` on the display-only image `f45ecffc` (sitting 2 read `0x23`: open question #72) | yes |
| `ucop` | ALSO | 30 s + a restart | sets the UI on the second core from the next start | no |
| `ucop2` | ALSO | 3 min | 2 min of use: no stall, no coherence failure, calls under 250 ms; "Normal" | no |

The verified line for `2c6dc03a` is earned when `jobs`, `jvpu`, `s6` and `b5` pass (and `s05` if it ran), with `b3` not failed (the line then reads "drift B3 PASS"); the clock150 token for `2c6dc03a` comes from `s6` at 150 MHz after `s4b` and `s4c`. `verified.txt` is not edited before the sitting: the ingest's patch carries both.

### Sitting 2 results (2026-10-25, 22:33–23:00, build 799ff0f8d7, merged image `80bc8611`)

Pulled to `session log, bench-20261025` (session `20261025-224345-799ff0f8d7-261026.txt`; the first attempt at 22:33 stopped after `s05`, whose flags the second used). **9 passed, 1 failed, 1 skipped**; no verified line (B13 is a NEED step). All [silicon]:

| Step | Result | Readings |
|---|---|---|
| `pre`, `s05` | pass; s05 skipped (flags present) | stage 4 and 5 flags of this build |
| `jobs` | pass | `jobs: self-test rc 0` |
| `jvpu` | pass | `J_VPU 3, expected 3 (merged image; checked 1, shift 1, MAC 0)`: the lane shift by transposes (GTHR) passes its start-up check on silicon. MAC 0 is by design |
| `b3`, `drill2` | pass ("Yes"; "Drift or still cover") | the drift on the BCM: `ipodui.log` `bcmjob up epoch=1`, `2`, `3`, each `vpu=0x3 expect=0x3`; every `panel drift upload … rc=0` (about 59–60 ms a cover, 21.7 ms a smaller one); `inject op=0x26 n=2` ridden out; no `fault` and no `FALLBACK` line |
| `s4b` | pass | the bench at 27 MHz (start-up clock 26,997 kHz), the 150 MHz switch (regulator `0xbb` → `0xbf`, PLL A `0x6c`/`0x658`, select `0x101`), ALU1 at 150,019 kHz; vector 24/24, scalar 9/9, kernels 69/69 |
| `s4c` | pass ("Yes") | a full frame 14,263 µs on the crystal, **8,256 µs on the PLL** (the LCD bus runs at the core's clock), 14,261 µs back on the crystal; switch up 11,018 µs, down 10,002 µs; a 2-D rect 586 µs on the PLL |
| `s6` | **pass, "Smooth, in sync"** | **the chip at 150 MHz** for all three plays (`chip MHz (now, was) (150, 0)`). Clip 1: 239 shown, 0 skipped, A/V max 25.8 ms (mean 5.7), FEED 7,591 µs mean, 29,813 µs max. Clip 1 again: 239/0, A/V max 28.4 ms, FEED 7,581 µs mean. Clip 2: **303 shown, 0 skipped**, A/V max 33.5 ms (mean 19.6), 48 hitches, FEED 18,531 µs mean, 40,220 µs max. SDRAM up, readback 0 of 64 bad, packet readback 0 bad, port waits ≤ 58 µs, 0 timeouts, port 14–16 MB/s |
| `b13` | **FAIL** ("Yes, briefly") | never parked: `PARK st=0 n=1 f=3, warm word 00000000 after 60008 us`, then the image did not answer the stock power-down and its power was cut (`../../bcm2722-reference/open-questions.md` #71; root cause in another session) |
| `b5` | pass ("Yes") | `31 pushes by DMA of 31, 0 timed out`; pitch 320 and 300 at 5,947 µs (32,000 px), pitch 400 at 2,976 µs (16,000 px), about 186 ns a pixel. Display-only image `J_VPU` **0x23**, not the expected 0x27: `J_VPU_MAC` clear (open questions #72) |

**Sitting 1's stage 6 skips were the clock.** There the chip ran at 27 MHz (no 4c flag, so stage 6 stayed on the crystal): FEED 42.7–95.4 ms a picture, 69–203 skipped, A/V out to seconds. With the 4b and 4c flags earned, the same clips ran at 150 MHz with FEED 7.6 ms (clip 1) and 18.5 ms (clip 2) and nothing skipped. The clock gate's explanation holds.

**The rows** (stage 4 `bcmtest.1.log` and 4b `bcmtest.log`, the same readings at both clocks):

- **HWLD:** all eight halfword load forms as the corrected model (`../../bcm2722-reference/quirks.md` #33, `isa-scalar.md`).
- **ASHX: `near32`**, not the modelled `dbl16` (quirks #30). **ACSA: `sat32`**, as modelled (#31). **GTHR** passes. ASHF, ASHG, X15M, X15S, X15Z, MHDU, VCLP, R48E, D16R, REPA, BARV pass; **PUTV fails** (informational; the C put stays).
- **ALGN:** 54 of 61 checks ran (the port part stopped at R602, port error −1). Loads as modelled; misaligned stores land rounded down and fault imprecisely; `vst16`/`vst8` off their size and each REP row round down; DMA sub-word units exact, a misaligned 32-byte destination rounds down; the port at +2 lands 2 bytes low in every form; a lone trailing halfword is dropped (`../../bcm2722-reference/memory-access.md` §11; quirks #34–#38).

**Ingest note:** the folder also held sitting 1's rotated logs (`bcmtest.3.log`, build caaa9890b0). The first ingest listed that build's stage 4 X15S/REPA as this sitting's "FAIL … (as modelled)": the old image's flags compared against the wrapping model, while the readings matched today's. `bench_ingest.py` now labels each stage-4 run with its log, bench image and build, leaves an older build's checks out of the failures, and calls a failed flag over a modelled reading `stale`.

## The clock rows: vbv, vgov (P19 PW4, branch `p19-video-dvfs`)

**What:** the video chip's level bands and the PP's clock governor (`battery-levers.md` §4, `power-contract.md` §8). **Performance and viability only: no meter, no power pass condition.** Rows in `apps/ipodui/devbench_vclk.c`, in part B after `s7`; both skip at once (`Not available: … HP_CAP_VCLK`) with an image without the bands, so they wait for **the next merged-image batch** (the image change is its own commit).

**Setup:** the bench clips in `bcmtest/` (`clip.mp4`, `clip2.mp4`, as stage 6). Nothing else.

| Row | What runs (the screen's script) | Pass (automatic) | Lines |
|---|---|---|---|
| `vbv` Clock bands | About 2 min. Clip 1 plays 8 s at each band, 46 MHz first, 150 last, the band held. At the slow bands the picture may stutter or freeze: expected. No buttons. Question: "Did any play show a garbled picture?" | every band: the chip ran the band asked for (PLL A locked, its speed checked, no fallback), ≥ 20 pictures shown, no decode error, no concealed block, no image loss. Passing bands (but 150, the 4b/4c flags') go into `bcmpassvc.txt` | `vbv 87 MHz level 8: PASS (ran 87 MHz, fallbacks 0, shown 240, errors 0, concealed 0, misses 0)`, `vbv: PASS: 11 of 11 bands hold; flags written` |
| `vgov` Clock governor | About 3 min. Each clip plays three times for 20 s: held at 150 MHz, then governed with the top-of-band policy (the default), then with the proportional one (the A/B); every play jumps back at 5 s and pauses 1 s at 10 s. No buttons. Question: "Did a later play of a clip look worse than its first?" | governed (top-of-band; the proportional play's verdict is logged, informational): no skipped picture, no dropped GOP, A/V within 50 ms after settling, no more hitches than at 150 (+1), at most 20 band changes a minute, the top band again within 1 s of the seek and of the resume | `vgov clip1 150|top|prop: shown … skipped … A/V max … hitches …; mhz= lvl= idle= bands= clocks= est=… mA at the bands' tops [estimate]`, `vgov clip1 prop: pass|fail (…; informational)`, `vgov clip1: PASS (…)`, `vgov: PASS: 2 of 2 clips`; the player's own `vclk band …`, `vclk sum policy=…`, `vclk time …` |

**The proxies it logs** (informational): the time-weighted MHz and level, the chip's idle share (its busy meter), the band and level changes, and an estimate from `bcm_power_model.py`'s per-level figures (`bvc_est_ma100`, the core polling between frames as a session does today), each against the same clip held at 150 MHz.

**What it decides:** which bands the governor may use on this unit (`vbv`'s flags), and whether the governor goes on for regular use (`vgov` PASS); then the long-run battery log (`batt session` lines) confirms or not over the following weeks (`power-contract.md` §8).

**Ingest:** `scripts/bench_ingest.py` prints a "video clock" section (every `vclk`/`vbv`/`vgov` line, the changes by why, each play's means) and flags a fallback, a failed row or a governed play with misses.

## Planned silicon row: the release hibernate (HW1)

**After PW5 passes on the unit, with its own owner go; not in a session build yet.** PW5 tests Apple's sequence with the probe's code in the plain video image; HW1 runs the release pieces (`power-contract.md` §6.6): the PP-uploaded stub (`bcmfw/hibstub.c`, `bcmdt_hibstub_image`), the display-only image's `HP_PARK_HIB` hook (the merged image's after its own re-verify), and `bcm-hibwake.h`'s `bhw_prepare`, `bhw_park`, `bhw_wake`.

**The row:** a committed slot and a frame in BACK; then park, a 1 s hold, wake; then a 10 s hold. Each wake must answer `AWAKE` (the stub's sums equal), keep `J_EPOCH`, hit the slot on `ALLOC`, and show the screen left from BACK. Log `hw1: hold H ms: end E word W wake U us sums A/B epoch kept|moved slot hit|miss`.

**Passes when** both holds come back `AWAKE` with the epoch kept and the slot hit. **Then** the stub's FNV goes into the record `bhw_gate` reads (the `verified.txt` discipline); before that, `HAVE_BCM_HIBWAKE` stays off and dark means cut.

