# Phase 16-0: measure first (sim numbers and the device procedure)

Branch `p16-a`, 2026-10-20. Evidence tags: **[sim]** counted or timed in the
simulator, **[model]** reasoned, **[device]** still to be read on the unit.
The sim has no FAT or ATA layer (host files), so storage-command counts and
real latencies come only from the device; the sim gives the counts of
operations and the memory figures.

## 1. Where the no-change scan goes

**Instrumentation.** `docs/research/p16-measure-0-scantrace.patch` (against
`apps/tagcache.c`; **not applied**: tooling does not edit `apps/`, the
orchestrator assigns it to the tagcache owner). It is devmode-only
(`HAVE_IPOD_UI_DEVMODE` and `UI_COMMIT`; compiled out of `fw-rel`), logs
through `ui_log`, and changes no behaviour. Checked: it applies cleanly,
`fw-ui` links, text +996 B and bss +3,104 B (a 4 KiB step may move `_end`;
`_end` was 1,092,208 B before it), the sim runs it.

Stages (each is the span since the previous mark): `tbl` (the filename-table
build and master open), `open` (`opendir`), `ign` (the two
`database.ignore`/`unignore` lookups per folder), `rdir` (`readdir`), `info`
(`dir_get_info`), `add` (`add_tagcache`: the per-file lookup and its
verification read). Each has milliseconds and, on the device, the tagcache
thread's storage commands (`devstats`, the counter behind `power.csv`'s
`disk_cmds`). Log lines (`ipodui.log`, one scan = six lines):

    db scantrace ms=T dirs=D ent=E files=F
    db scantrace ms tbl= open= ign= rdir=
    db scantrace ms info= add= ignmiss=M
    db scantrace cmds tbl= open= ign=
    db scantrace cmds rdir= info= add=
    db scantrace lookups=L reads=R        (sim only; -1 on the device)

`tests/perf/scan_trace.sh [tracks]` runs it in the sim (synthetic library,
a cover and a `._` sidecar per folder; boot, reboot, no-op update).

**Sim numbers [sim]** (1,451 tracks, 206 folders, with the patch applied
to a scratch build; host milliseconds, so only the counts transfer):

| Scan | ms | folders | entries | files seen | failed ignore lookups | lookups | tag-file reads |
|---|---|---|---|---|---|---|---|
| first build | 220 | 205 | 2,116 | 1,768 | 410 | 1,451 | 0 |
| no-change probe (boot) | 29 (tbl 1, open 5, ign 0, rdir 2, info 15, add 3) | 206 | 2,132 | 1,783 | 412 | 1,451 | 1,451 |
| no-op update | 19 | 206 | 2,133 | 1,784 | 412 | 1,451 | 1,451 |

Per folder the probe costs one `opendir` and two failed ignore-file path
lookups (3 path walks, 412 failed lookups = 2 per folder, none cached); per
entry one `readdir` and one `dir_get_info`; per file one table lookup and
one verification read (R = L). The device volume has 418 folders and 3,801
entries for 1,451 tracks (host scan 2026-10-19).

**Model [model], to be replaced by the device lines.** Boot scan
`db scan ms=18380 n=1451`; at ~0.35-0.41 ms per single-sector command
(handoff 2026-10-19) that is ~45-52k commands, ~31-36 per track. The
verification reads give 1,451 of them. The rest must come from the path
walks: 418 folders x 3 lookups = 1,254 walks, each descending ~4 levels
through directories of several sectors, plus the directory reads
themselves (3,801 entries at 16 slots per sector, more with long names).
The patch's `cmds` columns split exactly this.

**Device procedure (owner's next connect, needs the owner's go and a fresh
backup per the deploy skill).**
1. Orchestrator applies the patch (or merges the commit that does), builds
   `fw-ui` (devmode), and deploys per `deploy-ipod`.
2. Boot with no library change; wait for the boot scan (`db scan ms=` line).
3. Read `ipodui.log` (`.rockbox` folder, not named on screen): the six
   `db scantrace` lines directly before `db scan ms=`.
4. Decide with them: if `cmds ign` + `cmds open` dominate, cache the
   ignore lookups per boot and open by cluster (16-B/16-2); if `cmds rdir`
   dominates, burst reads (16-B); if `cmds add` dominates, the lookup path.
   Compare `ms` per stage against `cmds x 0.4 ms` to see CPU versus wait.
5. Repeat once while playing to see the unboosted case (`power.csv`
   `disk_cmds` gives the cross-check).

## 2. Resume latency after a partial shrink

Sim stand-in `tests/perf/resume-shrink-50k.script` (run by `floor_gate.sh`):
pause, a real 7,000,000 B `core_alloc` beside the paused audio, then PLAY.
[sim] 32 MB, 50k: buffer before the window 20,977,328 B, after it
14,585,552 B (the 7 MB is taken from the buffer), and after PLAY it stays
14,585,552 B (the buffer stays smaller, as the brief says); playback resumes.
The sim's `m0 tap2pcm` is host time (0-1030 ms), meaningless as a latency.
**Device twin [device]:** after #88 exists, pause, wait for a window, press
PLAY; read `m0 tap2pcm ms=M why=resume` against the same line without a
window (190-440 ms, 2026-10-19). Q2's bound is "as fast as a normal resume".

## 3. Commit peak memory per phase [sim]

`tests/perf/commit_peak.sh` (audio stopped, 50k, one new file; free core
heap from the `floor_watch ... timeline` sampler, phases from the tagcache
lines):

| Phase | 32 MB free heap | 64 MB free heap |
|---|---|---|
| after boot probe, steady (audio stopped) | 21,846,072 | 55,400,504 |
| scan done, before the commit | 22,429,688 | 55,984,120 |
| commit allocated ("needs 7,475,008 B") | 14,971,064 | 48,525,496 |
| committed (numeric indices, 1.1 s) | 15,481,736 | 49,036,168 |
| RAM cache reload (6,964,336 B) and view pool | **14,232,768 (minimum)** | **47,787,200 (minimum)** |

The commit takes 7.5 MB of scratch (the dip from 22.4 to 15.0 MB) and the
reload with the two snapshots 0.7 MB more. These free-heap minimums are with
audio stopped (its buffer free); the 8,269,688 / 41,824,120 B in the #85
rows are the playing-beside case. The audio-buffer floor is unaffected by
either (see `docs/hardware.md`).

## 4. Surprises
- The audio buffer at shell start is 21.6 / 55.1 MB, not the 15.08 / 48.63 MB
  of the earlier receipts; after an update commits and playback restarts it
  is 13.99 / 47.54 MB. The post-commit value is the one that binds (margin
  6.49 / 8.54 MB above the floors).
- `cp -R` without `-p` made the cloned 50k disk look changed (mtimes), so
  the database rescanned all 50,000 files; `floor_gate.sh` uses `cp -p`.
  `tests/perf/snapshot-memory.sh` clones with plain `cp -cR`; check it.
