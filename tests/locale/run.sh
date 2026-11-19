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

# The shell's date, time and number formats per language (P20, 0.9.0):
# apps/ipodui/locale_core.c against the patterns and names the four
# shell .lang files ship (gen_patterns.py reads them).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=build/locale
rm -rf "$T"
mkdir -p "$T"
CC=${HOSTCC:-cc}

python3 tests/locale/gen_patterns.py "$ROOT" > "$T/patterns.h" || {
    cat "$T/patterns.h"; echo "FAIL: locale patterns"; exit 1; }
$CC -O1 -std=c99 -Wall -Wextra -Werror -Iapps -I"$T" \
    tests/locale/test_locale.c apps/ipodui/locale_core.c -o "$T/test" || {
    echo "FAIL: locale build"; exit 1; }
"$T/test" > "$T/test.log"
rc=$?
grep FAIL "$T/test.log" | head -20
tail -1 "$T/test.log"
if [ $rc -ne 0 ] || ! grep -q '^PASS: locale [0-9][0-9]* checks' "$T/test.log"; then
    echo "FAIL: locale"
    exit 1
fi
n=$(sed -n 's/^PASS: locale \([0-9]*\) checks/\1/p' "$T/test.log")
if [ "${n:-0}" -lt 40 ]; then
    echo "FAIL: locale ran only ${n:-0} checks (expected 40 or more)"
    exit 1
fi
