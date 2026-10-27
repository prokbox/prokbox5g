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
# tests/bench_cache: the long benches' content-hash cache
# (scripts/bench_cache.py, wired in scripts/bcmfw_benches.sh).  A fake slow
# bench under a test table: a PASS records and the next run is cached; a
# changed input busts it; GATE_FORCE=1 always runs; a FAIL never records
# and drops an earlier PASS of the same inputs; a short bench never caches.
#
# Copyright (C) 2026 prokopto-dev
# Copyright (C) 2026 prokopto-dev
#
# Portions derived from Rockbox (rockbox.org).
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

cd "$(dirname "$0")/../.." || exit 1
REPO=$(pwd)
W="$REPO/build/tmp/bench_cache_test"
rm -rf "$W"
mkdir -p "$W/tree/src" "$W/tests" "$W/cache" "$W/tmp"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$W"' EXIT

cat >"$W/tests/slow_test.py" <<'PY'
import os, sys
W = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
open(os.path.join(W, 'ran'), 'a').write('slow\n')
f = os.path.join(W, 'rc')
rc = int(open(f).read()) if os.path.exists(f) else 0
print('1 rows, 1 cases' if rc == 0 else 'FAIL: told to')
sys.exit(rc)
PY
cat >"$W/tests/quick_test.py" <<'PY'
import os
W = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
open(os.path.join(W, 'ran'), 'a').write('quick\n')
print('1 rows, 1 cases')
PY
echo one >"$W/tree/src/input.c"
echo m1 >"$W/tree/corpus.sha256"
cat >"$W/table.json" <<JS
{"slow": [10, ["src/", "corpus.sha256"]],
 "quick": [3, ["src/"]]}
JS

export BENCH_CACHE_TABLE="$W/table.json" BENCH_CACHE_ROOT="$W/tree" \
       BENCH_CACHE_DIR="$W/cache" BCMFW_TESTS="$W/tests" TMPDIR="$W/tmp" \
       BCMFW_ONLY="slow quick" BCM_JOBS=2
unset GATE_FORCE

pass=0 fail=0
runs() { grep -c "^$1\$" "$W/ran" 2>/dev/null || echo 0; }
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
bench() { sh scripts/bcmfw_benches.sh >"$W/out" 2>&1; echo $? >"$W/rc_out"; }
# a PASS is held for slow's inputs as they are now
# (the hash sees the strict mode bcmfw_benches.sh sets)
held() { VC2SIM_STRICT_ALIGN=${VC2SIM_STRICT_ALIGN:-trap} \
         python3 -I scripts/bench_cache.py check slow >/dev/null 2>&1; }
notheld() { ! held; }

# 1. first run: runs, passes, records
bench
check "first run passes" grep -q '^ok   bcmfw slow: 1 rows' "$W/out"
check "first run ran" [ "$(runs slow)" = 1 ]
check "a PASS records" held
check "a short bench never records" [ ! -e "$W/cache/quick" ]

# 2. same inputs: cached, not run; the short one runs again
bench
check "same inputs: cached" grep -qE '^ok   bcmfw slow \(cached [0-9a-f]{12}\)$' "$W/out"
check "cached: not run" [ "$(runs slow)" = 1 ]
check "short bench runs every time" [ "$(runs quick)" = 2 ]
check "cached run exits 0" [ "$(cat "$W/rc_out")" = 0 ]

# 3. a changed source busts it; so do a changed manifest and an added file
echo two >"$W/tree/src/input.c"
bench
check "changed source: runs" [ "$(runs slow)" = 2 ]
echo m2 >"$W/tree/corpus.sha256"
bench
check "changed manifest: runs" [ "$(runs slow)" = 3 ]
echo new >"$W/tree/src/added.h"
bench
check "added file: runs" [ "$(runs slow)" = 4 ]
bench
check "then cached again" [ "$(runs slow)" = 4 ]
(VC2SIM_STRICT_ALIGN=off; export VC2SIM_STRICT_ALIGN; bench)
check "a changed VC2SIM_ setting: runs" [ "$(runs slow)" = 5 ]

# 4. GATE_FORCE=1 always runs
(GATE_FORCE=1; export GATE_FORCE; bench)
check "GATE_FORCE runs" [ "$(runs slow)" = 6 ]

# 5. a FAIL never records, and drops the last PASS of the same inputs
echo 1 >"$W/rc"
(GATE_FORCE=1; export GATE_FORCE; bench)
check "forced fail reported" grep -q '^FAIL bcmfw slow' "$W/out"
check "forced fail exits 1" [ "$(cat "$W/rc_out")" = 1 ]
check "a FAIL drops the record" notheld
bench
check "after a FAIL: runs again" [ "$(runs slow)" = 8 ]
check "still no record after a FAIL" notheld
echo three >"$W/tree/src/input.c"
bench
check "changed input that fails: no record" notheld
rm -f "$W/rc"
bench
check "a pass after fails records" held
bench
check "and is cached" [ "$(runs slow)" = 10 ]

# 5b. one cache for every worktree: a PASS at other inputs stays
echo four >"$W/tree/src/input.c"
bench
check "other inputs: run" [ "$(runs slow)" = 11 ]
echo three >"$W/tree/src/input.c"
bench
check "back to earlier inputs: still cached" [ "$(runs slow)" = 11 ]
check "several passes held" [ "$(ls "$W/cache/slow" | wc -l | tr -d ' ')" -ge 2 ]

# 6. the real table: the long benches hash, the short ones do not
unset BENCH_CACHE_TABLE BENCH_CACHE_ROOT
for b in jpeg video devtest; do
    check "$b is cached" python3 -I scripts/bench_cache.py hash "$b" >/dev/null
done
python3 -I scripts/bench_cache.py hash hello >/dev/null 2>&1
check "hello is not cached" [ $? = 2 ]

echo "bench_cache: $pass passed, $fail failed"
[ "$fail" = 0 ]
