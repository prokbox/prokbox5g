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

# The floor gate's log rules (tests/perf/floor_gate.sh) on hand-made logs,
# good and bad, so each rule is seen to pass and to fail (p090-floor):
#   tests/perf/window309.awk      the 64 MB paused window's commit order
#   tests/perf/upgrade_order.awk  the upgrade case's first-boot order
# Exit 0 = every case gave its expected verdict; prints the count.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/floorrules.XXXXXX")
trap 'rm -rf "$T"' EXIT
FAIL=0
N=0
# case NAME RULE WANT(0|1) [awk -v args...]: the log is $T/log
case_() {
    name=$1; rule=$2; want=$3; shift 3
    out=$(awk "$@" -f "$ROOT/tests/perf/$rule.awk" "$T/log" 2>&1)
    got=$?
    N=$((N + 1))
    if [ "$got" = "$want" ] && [ -n "$out" ]; then
        echo "ok   $rule $name: $out"
    else
        echo "FAIL $rule $name: exit $got, want $want: ${out:-no verdict line}"
        FAIL=1
    fi
}
B='sim_script: floor before_window ram_mb=64 audio=49016528 filebuflen=49245904'
W='ipodui: maint job=commit wait floor need=11108640 audio=49016528 floor=39000000'
R='ipodui: maint retry after regrow job=commit audio=49985880'
K='ipodui: maint take job=commit ok=1 need=10003216 audio=49985880->39982536 ms=10'
WW='sim_script: floor window_wait ram_mb=64 audio=48259648 filebuflen=48489024'
S='sim_script: audio_stop'
C='ipodui: db commit ms=170 ok=1 n=65001 b=0'
C0='ipodui: db commit ms=0 ok=0 n=65001 b=0'
F='-v floor=39000000'

printf '%s\n' "$B" "$W" "$WW" "$S" "$C" >"$T/log"; case_ waited-committed-at-stop window309 0 $F
printf '%s\n' "$B" "$W" "$R" "$W" "$WW" "$S" "$C" >"$T/log"; case_ waited-retried-at-stop window309 0 $F
printf '%s\n' "$B" "$K" "$WW" "$S" "$C" >"$T/log"; case_ direct-take window309 1 $F
printf '%s\n' "$B" "$W" "$R" "$K" "$WW" "$S" "$C" >"$T/log"; case_ taken-after-retry window309 1 $F
printf '%s\n' "$B" "$WW" "$S" "$C" >"$T/log"; case_ no-wait window309 1 $F
printf '%s\n' "$B" "$W" "$R" "$R" "$WW" "$S" "$C" >"$T/log"; case_ two-retries window309 1 $F
printf '%s\n' "$B" "$W" "$WW" "$S" "$C0" >"$T/log"; case_ no-commit-after-stop window309 1 $F
printf '%s\n' "$B" "$W" "$C" "$WW" "$S" "$C" >"$T/log"; case_ commit-before-stop window309 1 $F
printf '%s\n' "$B" "$W" "$S" "$C" >"$T/log"; case_ no-window-wait-line window309 1 $F
printf '%s\n' "$B" "$W" "$WW" "$S" "$C" >"$T/log"; case_ under-floor window309 1 -v floor=50000000

# the upgrade case: the same format (0.8.x to 0.8.x) keeps the old rule,
# the 0.8.x library's RAM cache retired before the first play
OWE='ipodui: db upgrade owed from=none to=a3/k3/s1 why=missing'
EXP='ipodui: db upgrade export rc=0'
GEN='ipodui: db gen 1 n=65000'
RET='ipodui: db retired ram free gen=0 bytes=8959488'
PLAY='ipodui: lib play us=1234 n=2000'
printf '%s\n' "$GEN" "$RET" "$PLAY" >"$T/log"; case_ same-format-retired upgrade_order 0
printf '%s\n' "$GEN" "$PLAY" "$RET" >"$T/log"; case_ same-format-retired-late upgrade_order 1
printf '%s\n' "$GEN" "$PLAY" >"$T/log"; case_ same-format-never-retired upgrade_order 1
# a format bump (0.8.x to 0.9.0): the first boot rebuilds, nothing to
# retire; the rebuild's statistics export and its publication come before
# the first play, and no old RAM cache is freed after it
printf '%s\n' "$OWE" "$EXP" "$GEN" "$PLAY" >"$T/log"; case_ bump-rebuilt upgrade_order 0
printf '%s\n' "$OWE" "$EXP" "$PLAY" "$GEN" >"$T/log"; case_ bump-published-late upgrade_order 1
printf '%s\n' "$OWE" "$GEN" "$PLAY" >"$T/log"; case_ bump-no-export upgrade_order 1
printf '%s\n' "$OWE" "$EXP" "$GEN" "$PLAY" "$RET" >"$T/log"; case_ bump-retired-late upgrade_order 1
GEN3='ipodui: db gen 3 n=65001'
printf '%s\n' "$OWE" "$EXP" "$GEN" "$PLAY" "$GEN3" "$RET" >"$T/log"; case_ bump-later-update upgrade_order 0
printf '%s\n' "$PLAY" "$OWE" "$EXP" "$GEN" >"$T/log"; case_ bump-seen-late upgrade_order 1

echo "floorrules: $N cases"
[ "$N" -eq 19 ] || { echo "FAIL expected 19 cases, ran $N"; FAIL=1; }
exit $FAIL
