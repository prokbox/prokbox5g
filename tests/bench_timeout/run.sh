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

# Self-test of scripts/bcmfw_benches.sh's per-bench timeout: a planted bench
# that never ends (a child shell running a sleeping python, both deaf to
# TERM) must fail as a TIMEOUT, its whole process group must be gone
# afterwards (TERM, then KILL after BENCH_GRACE), and the bench loop's wait
# must return.  A bench that ends by itself leaves no stragglers either.

cd "$(dirname "$0")/../.." || exit 1
REPO=$(pwd)
W="$REPO/build/tmp/bench_timeout_test"
rm -rf "$W"
mkdir -p "$W/tests" "$W/cache" "$W/tmp"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$W"' EXIT

# the hanging bench: records its process group, then a child shell runs a
# python that sleeps forever; both ignore TERM, so only the KILL ends them
cat >"$W/tests/hang_test.py" <<'PY'
import os
W = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
open(os.path.join(W, 'pgid'), 'w').write('%d\n' % os.getpgrp())
os.execvp('sh', ['sh', '-c', "trap '' TERM; "
                 "python3 -c 'import time; [time.sleep(1) for _ in iter(int,1)]'; echo"])
PY
# a bench that ends at once but leaves a background sleeper behind
cat >"$W/tests/leave_test.py" <<'PY'
import os, subprocess
W = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
open(os.path.join(W, 'pgid2'), 'w').write('%d\n' % os.getpgrp())
subprocess.Popen(['sh', '-c', 'sleep 600'])
print('1 rows, 1 cases')
PY
echo '{}' >"$W/table.json"

# expected time 0 (not in the table): the limit is BENCH_TIMEOUT
export BENCH_CACHE_TABLE="$W/table.json" BENCH_CACHE_ROOT="$W" \
       BENCH_CACHE_DIR="$W/cache" BCMFW_TESTS="$W/tests" TMPDIR="$W/tmp" \
       BCMFW_ONLY="hang leave" BCM_JOBS=2 BENCH_TIMEOUT=5 BENCH_GRACE=2
unset GATE_FORCE

pass=0 fail=0
check() { # what, condition...
    what=$1; shift
    if "$@"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $what"
        sed 's/^/    /' "$W/out"
    fi
}
gone() { [ -s "$W/$1" ] && ! pgrep -g "$(cat "$W/$1")" >/dev/null 2>&1; }

t0=$(date +%s)
sh scripts/bcmfw_benches.sh >"$W/out" 2>&1
rc=$?
secs=$(( $(date +%s) - t0 ))
check "the planted bench recorded its group" [ -s "$W/pgid" ]
check "the wait returned within limit and grace (${secs} s)" [ "$secs" -lt 30 ]
check "the run fails" [ "$rc" = 1 ]
check "FAIL ... TIMEOUT printed" grep -q '^FAIL bcmfw hang: TIMEOUT after' "$W/out"
check "the hung bench's whole group is gone" gone pgid
check "the ended bench passes" grep -q '^ok   bcmfw leave' "$W/out"
check "the ended bench's stragglers are gone" gone pgid2
# leave nothing behind whatever failed (only the groups this test started)
for f in pgid pgid2; do
    [ -s "$W/$f" ] && kill -KILL -"$(cat "$W/$f")" 2>/dev/null
done
rm -f "$REPO/build/bench-logs/hang.log"
echo "bench_timeout: $pass passed, $fail failed (${secs} s)"
[ "$fail" = 0 ]
