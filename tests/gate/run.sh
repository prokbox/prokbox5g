#!/bin/bash
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

# Unit tests of the gate machinery (scripts/gatelib.sh, gate_tools.py):
# phase timing, the build abort, the job pool (cap, longest-first order),
# the duration table, the flake ledger. Fake jobs only: no simulator, no build.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
T=build/gate-unit
rm -rf "$T"; mkdir -p "$T"
fails=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }

export GATE_TMP=$T/tmp; mkdir -p "$GATE_TMP"
export GATE_LEDGER=$T/flakes.tsv GATE_DURATIONS=$T/durations.tsv

# --- phase timing ------------------------------------------------------
( . scripts/gatelib.sh
  gl_phase one; sleep 1; gl_phase two; gl_phase_end ) >"$T/phases.out" 2>&1
if [ "$(wc -l <"$GATE_TMP/phases.tsv" | tr -d ' ')" = 2 ] \
   && awk -F'\t' '$1=="one"&&$4>=1{f=1} END{exit !f}' "$GATE_TMP/phases.tsv" \
   && grep -q 'min total' "$T/phases.out"; then
    ok "phase timing rows and table"
else bad "phase timing ($(cat "$T/phases.out"))"; fi

# --- build abort ---------------------------------------------------------
abort_case() { # bfail nbad nbad_builds keepgoing -> exit code
    ( . scripts/gatelib.sh
      bfail=$1 nbad=$2 nbad_builds=$3 GATE_KEEP_GOING=$4
      gl_abort_on_build_failure; exit 0 ) >"$T/abort.out" 2>&1
    echo $?
}
[ "$(abort_case 0 0 0 0)" = 0 ] && ok "no build failure: continues" || bad "abort: clean case"
[ "$(abort_case 1 0 0 0)" = 1 ] && grep -q '^GATE FAILED' "$T/abort.out" \
    && ok "build status failure aborts with GATE FAILED" || bad "abort: status failure"
[ "$(abort_case 0 3 2 0)" = 1 ] && ok "compile-only build failure aborts" || bad "abort: nbad"
[ "$(abort_case 0 2 2 0)" = 0 ] && ok "earlier failure (TEMP-) does not abort" || bad "abort: earlier nbad"
[ "$(abort_case 1 1 0 1)" = 0 ] && ok "GATE_KEEP_GOING=1 keeps going" || bad "abort: keep going"

# --- pool: cap and longest-first order -----------------------------------
pool_case() { # N -> max concurrency seen
    ( . scripts/gatelib.sh
      rm -f "$T/run.cur" "$T/run.max"; echo 0 >"$T/run.cur"; echo 0 >"$T/run.max"
      job() { # holds a slot for 0.3 s and tracks the number of live jobs
          mkdir "$T/lock.$1"
          c=$(ls -d "$T"/lock.* 2>/dev/null | wc -l); m=$(cat "$T/run.max"); m=${m:-0}
          [ "$c" -gt "$m" ] && echo "$c" >"$T/run.max"
          sleep 0.3; rmdir "$T/lock.$1"; }
      gl_pool_init "$1"
      for i in 1 2 3 4 5 6 7 8 9 10; do gl_pool_submit job "$i"; done
      gl_pool_wait )
    cat "$T/run.max"
}
m=$(pool_case 3)
[ "$m" -le 3 ] && [ "$m" -ge 2 ] && ok "pool honours its cap (3 workers, saw $m)" || bad "pool cap: saw $m"
start=$(date +%s)
pool_case 5 >/dev/null
el=$(( $(date +%s) - start ))
[ "$el" -le 2 ] && ok "pool runs 10 x 0.3 s jobs on 5 slots in ${el}s" || bad "pool too slow: ${el}s"

printf 'ui\tipodui-a\nubsan\tipodui-a\nfloor\tfloor\nui\tipodui-b\n' >"$T/jobs.tsv"
printf 'ipodui-a\t100\nipodui-b\t500\n' >"$T/seed.tsv"
python3 scripts/gate_tools.py order "$T/jobs.tsv" --table "$T/seed.tsv" >"$T/order.out"
# floor 480 < ui-b 500; ubsan 136 > ui 100
want=$'ui\tipodui-b\t500\nfloor\tfloor\t480\nubsan\tipodui-a\t136\nui\tipodui-a\t100'
[ "$(cat "$T/order.out")" = "$want" ] && ok "jobs ordered longest first (ubsan x1.36, floor 480)" \
    || bad "order: $(cat "$T/order.out")"

# --- duration table learns -------------------------------------------------
printf 'ipodui-a\t0\t0\t10\t1\t1\t100\t1\tui\nipodui-b\t0\t0\t10\t1\t1\t900\t2\tui\n' >"$T/rows.tsv"
GATE_DURATIONS=$T/durations.tsv python3 scripts/gate_tools.py update "$T/rows.tsv" >/dev/null
grep -q $'^ipodui-a\t' "$T/durations.tsv" && ! grep -q 'ipodui-b' "$T/durations.tsv" \
    && ok "duration update uses first attempts only" || bad "duration update"

# --- seed (a note: a script missing from it is estimated from its waits) ----
n_scripts=$(ls tests/ui/*.script | wc -l | tr -d ' ')
n_seed=$(grep -vc '^#' tests/ui/durations.tsv)
ok "tests/ui/durations.tsv has $n_seed rows for $n_scripts scripts (scripts/gate_tools.py seed regenerates it)"

# --- validate.sh's pool_ui_job (extracted) with fake run_script_job ----------
PT=$T/poolui; mkdir -p "$PT"
(   . scripts/gatelib.sh
    GATE_TMP=$PT; GATE_ID=unit-g; GATE_COMMIT=abc
    script_sim() { echo "$PT/fakesim"; }
    script_disk() { echo "$PT/fakesim/simdisk"; }
    disk_base() { echo fakesim; }
    clone_disk() { rm -rf "$2"; mkdir -p "$2"; }
    run_script_job() { # name build disk worker attempt: flakyB fails attempt 1; realC always
        n=$1; att=$5
        case "$n:$att" in
            flakyB:1) echo "FAIL: mean frame interval 70 ms > 50 ms [flakyB]" >"$GATE_TMP/out-$n"; echo 1 >"$GATE_TMP/status-$n" ;;
            realC:*)  echo "FAIL: golden realC" >"$GATE_TMP/out-$n"; echo 1 >"$GATE_TMP/status-$n" ;;
            *)        echo "ok   $n" >"$GATE_TMP/out-$n"; echo 0 >"$GATE_TMP/status-$n" ;;
        esac
    }
    eval "$(awk '/^    pool_ui_job\(\)/,/^    }$/' scripts/validate.sh)"
    gl_pool_init 3
    for n in okA flakyB realC; do gl_pool_submit pool_ui_job "$n"; done
    gl_pool_wait
) >"$PT/out.log" 2>&1
if [ -f "$PT/first-failed-flakyB" ] && [ "$(cat "$PT/retry-flakyB")" = 0 ] \
   && [ -f "$PT/first-failed-realC" ] && [ "$(cat "$PT/retry-realC")" = 1 ] \
   && [ ! -f "$PT/first-failed-okA" ] && [ "$(cat "$PT/status-flakyB")" = 0 ] \
   && grep -q 'FAIL: mean frame' "$PT/out-flakyB.first-attempt"; then
    ok "pool job: immediate retry (flaky passes, real fails twice, ok untouched)"
else bad "pool job logic: $(cat "$PT/out.log"; ls "$PT")"; fi
python3 scripts/gate_tools.py ledger-report --gate-id unit-g >"$T/l2.out"
grep -q 'flaky .*flakyB' "$T/l2.out" && grep -q 'real .*realC' "$T/l2.out" \
    && ok "pool job: ledger rows (flaky, real)" || bad "pool ledger: $(cat "$T/l2.out")"

# --- ledger ------------------------------------------------------------------
for g in g1 g2 g3; do
    python3 scripts/gate_tools.py ledger-add --gate-id $g --commit abc --script ipodui-x \
        --kind flaky --retry ok --reason "FAIL: mean frame interval 60 ms"
done
python3 scripts/gate_tools.py ledger-add --gate-id g3 --commit abc --script ipodui-y \
    --kind real --retry fail --reason "FAIL: golden"
python3 scripts/gate_tools.py ledger-report --gate-id g3 >"$T/ledger.out"
grep -q 'needs a fix.*ipodui-x' "$T/ledger.out" && ! grep -q 'needs a fix.*ipodui-y' "$T/ledger.out" \
    && grep -q '2 first-attempt' "$T/ledger.out" && ok "ledger report: repeat flaker named, real failure not" \
    || bad "ledger: $(cat "$T/ledger.out")"

rm -rf "$T"
[ $fails -eq 0 ] && echo "ok   gate machinery" || { echo "gate machinery: $fails failure(s)"; exit 1; }
