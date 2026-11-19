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

# #203/#82: the EQ level plan (precut, K-weighted loudness, make-up) and the
# custom slot text format, device code (apps/ipodui/eqlab_core.c) against a
# double-precision RBJ + BS.1770 reference (tests/eqlevel/test_eqlevel.c).
# Red run: the pre-#203 plan (precut, no make-up) must fail the loudness
# check on the boost presets.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=build/eqlevel
rm -rf "$T"
mkdir -p "$T"
CC=${HOSTCC:-cc}
SRC="tests/eqlevel/test_eqlevel.c apps/ipodui/eqlab_core.c apps/ipodui/eq_hw_presets.c"

$CC -O1 -std=c99 -Wall -Wextra -Werror -Iapps -Ifirmware/export $SRC -lm \
    -o "$T/green" || { echo "FAIL: eqlevel build"; exit 1; }
$CC -O1 -std=c99 -DLEVEL_RED=1 -Iapps -Ifirmware/export $SRC -lm \
    -o "$T/red" || { echo "FAIL: eqlevel red build"; exit 1; }

"$T/green" >"$T/green.log"
rc=$?
tail -2 "$T/green.log"
if [ $rc -ne 0 ] || ! grep -q '^PASS: ' "$T/green.log"; then
    grep FAIL "$T/green.log" | head -20
    echo "FAIL: eqlevel"
    exit 1
fi
"$T/red" >"$T/red.log"
rc=$?
n=$(grep -c "from EQ off's loudness" "$T/red.log")
if [ $rc -eq 0 ] || [ "$n" -lt 12 ]; then
    echo "FAIL: eqlevel red run (no make-up) passed or caught only $n presets"
    exit 1
fi
echo "ok   eqlevel red run (no make-up): $n presets quieter than EQ off"
echo "PASS: eqlevel"
