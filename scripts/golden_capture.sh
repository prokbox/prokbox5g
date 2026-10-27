#!/bin/sh
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************

# scripts/golden_capture.sh [--verify3] [--dry] [-j N] <script>... [shot ...]
#
# Captures or refreshes the golden images for one or more
# tests/ui/*.script. Per docs/process.md "Goldens": agents only *propose*
# a look; this is the only thing allowed to write
# tests/ui/golden/<script>/*.png, and it writes one only after proving it
# is reproducible.
#
# Every script is run TWICE (three times with --verify3) with
# scripts/shots.sh, each run on its OWN clone of a primed simdisk (the same
# rule as the gate's parallel workers: the shared disk is primed once, then
# cp -c cloned), so runs are independent and can go in parallel. Runs of
# the same script, and of different scripts, are all spread over JOBS
# parallel workers (env JOBS, default min(16, 2*ncpu); -j N overrides).
# Pinned state (clock, battery, resume -- tests/ui/README.md) is already
# pinned by default in shots.sh.
#
# Per script, run 1 is compared with run 2 (and run 3 with --verify3) by
# scripts/pngdiff.py --batch at threshold 0 (pixel-identical, not just
# under GOLDEN_THRESHOLD). Only if EVERY compared shot matches does it copy
# run 1's images into tests/ui/golden/<script>/ (creating or overwriting).
# If even one shot differs it prints the differing shot(s), writes nothing
# for that script and the exit status is non-zero -- a flaky or
# still-settling script must not get a golden. Scripts are judged
# independently: one refused script does not stop the others from being
# written.
#
# Shots named in the script's own "# golden: skip <shot>..." line
# (tests/ui/README.md: timing-sensitive, e.g. mid-animation) are run like
# any other shot but are never compared or captured, same as validate.sh.
#
# Arguments ending in .script are scripts; anything else is a shot name,
# limiting the capture to those shots (meant for a single script; every
# other shot is still run, just not compared or written). Default: every
# shot, minus the golden:skip ones.
#
# SIM_BUILD picks the simulator: build/sim-ui-video for an ipodui-p14a-*
# script (Phase 14a, the video player), build/sim-ui-mod for a "# needs:
# modules" one (validate.sh builds it), build/sim-ui for another ipodui-*
# one, build/sim otherwise (as validate.sh's script_sim(); a given
# SIM_BUILD applies to all scripts).
#
# A script that leaves residue a clone-fresh second run doesn't expect is
# exactly the kind of non-determinism this exists to catch -- fix the
# script's own cleanup/directives, don't retry until it happens to pass.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
. scripts/simpool.sh

work=build/.golden-capture

# internal: one run of one script on its own disk clone (called via xargs)
if [ "${1:-}" = --job ]; then
    script=$2; run=$3; sim=$4
    name=$(basename "$script" .script)
    d=$work/$name
    disk=$d/disk$run
    simpool_clone "$sim/simdisk" "$disk" || exit 1
    SIM_BUILD=$sim SIMDISK=$disk scripts/shots.sh "$script" "$d/run$run" \
        >"$d/run$run.log" 2>&1
    rc=$?
    rm -rf "$disk"
    echo $rc >"$d/status$run"
    exit 0
fi

VERIFY3=0; DRY=0; JOBS=${JOBS:-$(simpool_jobs)}
scripts_list=""; want_shots=""
while [ $# -gt 0 ]; do
    case "$1" in
        --verify3) VERIFY3=1 ;;
        --dry) DRY=1 ;;
        -j) shift; JOBS=${1:-} ;;
        -j*) JOBS=${1#-j} ;;
        *.script) scripts_list="$scripts_list $1" ;;
        *) want_shots="$want_shots $1" ;;
    esac
    shift
done
[ -n "$scripts_list" ] || { echo "usage: $0 [--verify3] [-j N] <script>... [shot ...]" >&2; exit 2; }
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1
NRUNS=2; [ $VERIFY3 = 1 ] && NRUNS=3
simpool_machine_lock   # one gate or capture at a time on this machine
for s in $scripts_list; do
    [ -f "$s" ] || { echo "no such script: $s" >&2; exit 2; }
done

sim_for() {
    if [ -n "${SIM_BUILD:-}" ]; then echo "$SIM_BUILD"; return; fi
    # the same rule as validate.sh's script_sim()
    case "$1" in
        ipodui-p14a-*) echo build/sim-ui-video ;;
        ipodui-*) if grep -qE '^#[[:space:]]*needs:[[:space:]]*modules' "tests/ui/$1.script" 2>/dev/null
                  then echo build/sim-ui-mod; else echo build/sim-ui; fi ;;
        docs-*) echo build/sim-ui ;;
        *) echo build/sim ;;
    esac
}

rm -rf "$work"
mkdir -p "$work"
: >"$work/fails.tsv"

# 1. prime each distinct sim's shared disk once, serially
sims=""
for s in $scripts_list; do
    b=$(sim_for "$(basename "$s" .script)")
    case " $sims " in *" $b "*) ;; *) sims="$sims $b" ;; esac
done
for b in $sims; do
    group=""
    for s in $scripts_list; do
        [ "$(sim_for "$(basename "$s" .script)")" = "$b" ] && group="$group $s"
    done
    echo "== golden_capture: priming $b/simdisk once"
    simpool_prime "$b" "$work/prime-$(basename "$b")" $group || exit 1
done

# 2. all runs of all scripts, JOBS at a time, each on its own disk clone
jobfile=$work/jobs
: >"$jobfile"
for s in $scripts_list; do
    name=$(basename "$s" .script)
    mkdir -p "$work/$name"
    r=1
    while [ $r -le $NRUNS ]; do
        printf '%s\n%s\n%s\n' "$s" "$r" "$(sim_for "$name")" >>"$jobfile"
        r=$((r + 1))
    done
done
echo "== golden_capture: $(echo $scripts_list | wc -w | tr -d ' ') script(s) x $NRUNS runs, $JOBS parallel job(s)"
xargs -n 3 -P "$JOBS" sh "$ROOT/scripts/golden_capture.sh" --job <"$jobfile"

# 3. per-script verdict (pixel-identical or refuse), in argument order
overall=0
okn=0
for script in $scripts_list; do
    name=$(basename "$script" .script)
    d=$work/$name
    fail=0

    r=1
    while [ $r -le $NRUNS ]; do
        if [ "$(cat "$d/status$r" 2>/dev/null || echo 1)" != 0 ]; then
            tail -20 "$d/run$r.log" 2>/dev/null
            echo "FAIL: golden_capture: $name: run $r failed (log: $d/run$r.log)" >&2
            fail=1
        fi
        r=$((r + 1))
    done
    [ $fail = 0 ] || { overall=1; continue; }

    skips=$(sed -n 's/^#[[:space:]]*golden:[[:space:]]*skip[[:space:]]*//p' "$script")
    out1=$d/run1
    pairs=$d/pairs
    : >"$pairs"
    shots=""
    for p in "$out1"/*.png; do
        [ -f "$p" ] || continue
        s=$(basename "$p" .png)
        if printf '%s\n' "$skips" | tr ' ' '\n' | grep -qx "$s"; then
            echo "skip $name/$s (golden: skip)"
            continue
        fi
        if [ -n "$want_shots" ]; then
            case " $want_shots " in
                *" $s "*) ;;
                *) continue ;;
            esac
        fi
        regs=$(sed -n "s/^#[[:space:]]*golden-region:[[:space:]]*$s[[:space:]]\{1,\}\([0-9]*\)[[:space:]]\{1,\}\([0-9]*\)[[:space:]]\{1,\}\([0-9]*\)[[:space:]]\{1,\}\([0-9]*\).*/\1,\2,\3,\4/p" "$script")
        r=2
        while [ $r -le $NRUNS ]; do
            if [ ! -f "$d/run$r/$s.png" ]; then
                echo "FAIL: golden_capture: $name/$s: run 1 took it, run $r didn't" >&2
                fail=1
            elif [ -z "$regs" ]; then
                printf '%s (run1 vs run%s)\t%s\t%s\n' "$name/$s" "$r" "$out1/$s.png" "$d/run$r/$s.png" >>"$pairs"
            else
                for reg in $regs; do
                    printf '%s [%s] (run1 vs run%s)\t%s\t%s\t%s\n' "$name/$s" "$reg" "$r" "$out1/$s.png" "$d/run$r/$s.png" "$reg" >>"$pairs"
                done
            fi
            r=$((r + 1))
        done
        shots="$shots $s"
    done
    [ $fail = 0 ] || { overall=1; continue; }
    if [ -z "$shots" ]; then
        echo "FAIL: golden_capture: $name: nothing to capture (all shots skipped, or none match [shot ...])" >&2
        overall=1; continue
    fi

    echo "== golden_capture: $name: comparing $NRUNS runs (threshold 0: pixel-identical)"
    if ! gout=$(scripts/pngdiff.py --batch "$pairs" --threshold 0); then
        echo "$gout"
        diffnames=$(printf '%s\n' "$gout" | sed -n 's/^FAIL: \([^:]*\):.*/\1/p')
        echo "FAIL: golden_capture: $name is not reproducible between $NRUNS runs; differing shot(s):$(printf ' %s' $diffnames)" >&2
        echo "FAIL: golden_capture: nothing written to tests/ui/golden/$name/" >&2
        printf '%s\n' "$gout" | sed -n 's/^FAIL: \(.*\): [0-9.]*% pixels.*/\1/p' >"$d/faillabels"
        awk -F'\t' 'NR==FNR{l[$1]=1;next} ($1 in l){print $1 "\t" $3 "\t" $2}' \
            "$d/faillabels" "$pairs" >>"$work/fails.tsv"
        overall=1
        continue
    fi
    echo "$gout"

    if [ $DRY = 1 ]; then
        echo "GOLDEN CAPTURE OK (--dry, nothing written): $name ($(echo $shots | wc -w | tr -d ' ') shot(s), $NRUNS runs)"
        okn=$((okn + 1))
        continue
    fi
    gdir="tests/ui/golden/$name"
    mkdir -p "$gdir"
    for s in $shots; do
        cp "$out1/$s.png" "$gdir/$s.png"
        echo "ok   wrote $gdir/$s.png"
    done
    echo "GOLDEN CAPTURE OK: $name ($(echo $shots | wc -w | tr -d ' ') shot(s), $NRUNS runs)"
    okn=$((okn + 1))
done

if [ -s "$work/fails.tsv" ]; then
    scripts/gate_report.py "$work/fails.tsv" --out out/gate-report --left run1 --right run-n >&2 \
        && echo "note: [run1 | run-n | diff] composites of the differing shots: out/gate-report/index.md" >&2
fi
echo
if [ $overall = 0 ]; then
    echo "GOLDEN CAPTURE OK: $okn script(s)"
else
    echo "GOLDEN CAPTURE FAILED: $okn of $(echo $scripts_list | wc -w | tr -d ' ') script(s) written; see FAIL lines above" >&2
    exit 1
fi
