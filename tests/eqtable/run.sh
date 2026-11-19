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

# #352: the per-family EQ preset table parser (apps/ipodui/eqtable_core.c,
# tests/eqtable/test_eqtable.c): good, bad, short, long and version or
# family mismatches, against the sim's hand-made hi table fixture too.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=build/eqtable
rm -rf "$T"
mkdir -p "$T"
CC=${HOSTCC:-cc}
SRC="tests/eqtable/test_eqtable.c apps/ipodui/eqtable_core.c apps/ipodui/eqlab_core.c apps/ipodui/eq_hw_presets.c"
FIX=tests/ui/fixtures/eqtable-hi.txt

[ -s "$FIX" ] || { echo "FAIL: eqtable fixture $FIX missing"; exit 1; }
$CC -O1 -std=c99 -Wall -Wextra -Werror -Iapps -Ifirmware/export $SRC -lm \
    -o "$T/test" || { echo "FAIL: eqtable build"; exit 1; }
"$T/test" "$FIX" >"$T/test.log"
rc=$?
grep FAIL "$T/test.log" | head -20
tail -1 "$T/test.log"
if [ $rc -ne 0 ] || ! grep -q '^PASS: eqtable [0-9][0-9]* checks' "$T/test.log"; then
    echo "FAIL: eqtable"
    exit 1
fi
n=$(sed -n 's/^PASS: eqtable \([0-9]*\) checks/\1/p' "$T/test.log")
if [ "${n:-0}" -lt 40 ]; then
    echo "FAIL: eqtable ran only ${n:-0} checks (expected 40 or more)"
    exit 1
fi
