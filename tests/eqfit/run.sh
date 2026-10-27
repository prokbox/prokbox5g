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

# Hardware-EQ fit (docs/phase15-brief.md sections 3.1, 7): the committed
# apps/ipodui/eq_hw_presets.c must equal a fresh run of tools/eqfit (so it
# can't drift from screens/eq.c's presets or the model), every field is in
# range and Flat is all zeros. The fit takes about 7 s.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/eqfit
cc -O2 -std=c99 -w -o build/eqfit/eqfit tools/eqfit/eqfit.c -lm
build/eqfit/eqfit -o build/eqfit/eq_hw_presets.c >build/eqfit/table.txt
if ! cmp -s build/eqfit/eq_hw_presets.c apps/ipodui/eq_hw_presets.c; then
    echo "FAIL: apps/ipodui/eq_hw_presets.c differs from a fresh eqfit run"
    diff apps/ipodui/eq_hw_presets.c build/eqfit/eq_hw_presets.c | head -20
    exit 1
fi
echo "ok   eq_hw_presets.c equals a fresh eqfit run"
flat=$(grep -E '^    /\* [A-Z_]+:' apps/ipodui/eq_hw_presets.c |
       awk '/\/\* FLAT:/ { print NR - 1; exit }')
cc -std=c99 -w -Iapps tests/eqfit/check.c apps/ipodui/eq_hw_presets.c \
   -o build/eqfit/check
build/eqfit/check "${flat:--1}"
