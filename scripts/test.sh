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

# Run all host-side tests. Builds build/warble if needed.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
if [ ! -f build/warble/Makefile ]; then
    mkdir -p build/warble
    (cd build/warble && ../../tools/configure --target=22 --type=w </dev/null >/dev/null)
fi
make -C build/warble -j10 >build/warble/make.log 2>&1 || {
    grep -nE "error" build/warble/make.log | head -20; exit 1; }
fail=0
# Misaligned access is a hard failure in every host test (the PP5022 aborts
# on it, quirks.md #51): host compilers called as cc/gcc/gcc-NN go through
# scripts/align-shim (ALIGN_SHIM=0 disables). Set after warble is built.
[ "${ALIGN_SHIM:-1}" = 0 ] || PATH="$ROOT/scripts/align-shim:$PATH"
export PATH
# TEST_JOBS=N (N > 1): the suites run N at a time, the slow fatcrash first;
# each suite's output is kept and printed in name order, so the log reads
# as the serial run does. Suites keep to their own build/<name> directories
# and only read build/warble (made above). Default: one after the other.
if [ "${TEST_JOBS:-1}" -gt 1 ] 2>/dev/null; then
    PD=build/.test-par
    rm -rf "$PD"; mkdir -p "$PD"
    { echo tests/fatcrash/run.sh; for t in tests/*/run.sh; do
          [ "$t" = tests/fatcrash/run.sh ] || echo "$t"; done; } |
        xargs -P "$TEST_JOBS" -I{} sh -c '
            t=$1; n=$(basename "$(dirname "$t")")
            [ -x "$t" ] || { echo 0 >"$2/$n.rc"; exit 0; }
            "$t" >"$2/$n.out" 2>&1; echo $? >"$2/$n.rc"' _ {} "$PD"
    for t in tests/*/run.sh; do
        [ -x "$t" ] || continue
        n=$(basename "$(dirname "$t")")
        echo "== $t"
        cat "$PD/$n.out" 2>/dev/null
        [ "$(cat "$PD/$n.rc" 2>/dev/null || echo 1)" = 0 ] || { echo "FAIL: $t exited non-zero"; fail=1; }
    done
else
for t in tests/*/run.sh; do
    [ -x "$t" ] || continue
    echo "== $t"
    "$t" || fail=1
done
fi
[ $fail -eq 0 ] && echo "ALL TESTS PASSED" || { echo "TESTS FAILED"; exit 1; }
