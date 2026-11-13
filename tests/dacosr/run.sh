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

# p083-dacosr64: the WM8758's R10 DACOSR per output rate (64x above 48 kHz,
# 128x at or below), host test of the real firmware/drivers/audio/wm8758.c
# against tests/dacosr/stub.  Then the red run: the same test against a copy
# of the driver that leaves R10 at 128x for every rate must fail.
# #265: R7 M128ENB with EQ band 4 and the 3D stage (test_m128.c), with two
# red runs: the pre-#265 driver (the EQ never sets it) and a driver whose
# bit ignores the 3D stage's request.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=build/dacosr
rm -rf "$T"
mkdir -p "$T"

CC=${HOSTCC:-cc}
build() {
    "$CC" -std=gnu99 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
        -Itests/dacosr/stub -Ifirmware/export \
        -o "$1" "${3:-tests/dacosr/test_dacosr.c}" "$2" 2>"$1.cc.log"
}

if ! build "$T/test_dacosr" firmware/drivers/audio/wm8758.c; then
    echo "FAIL: compiling the driver test"; cat "$T/test_dacosr.cc.log"
    exit 1
fi
if out=$("$T/test_dacosr"); then
    echo "ok   R10 DACOSR per rate: $(echo "$out" | tail -1)"
else
    echo "$out"; echo "FAIL R10 DACOSR per rate"; exit 1
fi

# red run: the rate rule removed (128x always, the upstream setting)
sed 's/dac_osr64 = fsel == HW_FREQ_96 || fsel == HW_FREQ_88;/dac_osr64 = false;/' \
    firmware/drivers/audio/wm8758.c > "$T/wm8758_red.c"
if cmp -s firmware/drivers/audio/wm8758.c "$T/wm8758_red.c"; then
    echo "FAIL: red run: the rate rule line was not found in the driver"
    exit 1
fi
if ! build "$T/test_red" "$T/wm8758_red.c"; then
    echo "FAIL: compiling the red run"; cat "$T/test_red.cc.log"; exit 1
fi
if red=$("$T/test_red"); then
    echo "FAIL: red run passed with R10 left at 128x"; exit 1
fi
if ! echo "$red" | grep -q "^FAIL play 96000: R10 0x008, want 0x000"; then
    echo "FAIL: red run failed for another reason:"; echo "$red"; exit 1
fi
echo "ok   red run (128x at every rate) fails: $(echo "$red" | grep -c '^FAIL') checks"

# #265: R7 M128ENB
M=tests/dacosr/test_m128.c
if ! build "$T/test_m128" firmware/drivers/audio/wm8758.c "$M"; then
    echo "FAIL: compiling the M128ENB test"; cat "$T/test_m128.cc.log"
    exit 1
fi
if out=$("$T/test_m128"); then
    echo "ok   R7 M128ENB with EQ band 4 and 3D: $(echo "$out" | tail -1)"
else
    echo "$out"; echo "FAIL R7 M128ENB"; exit 1
fi

# red runs: $1 the driver line, $2 its replacement, $3 the FAIL line the
# red run must show, $4 its name
m128_red() {
    sed "s/$1/$2/" firmware/drivers/audio/wm8758.c > "$T/wm8758_m128red.c"
    if cmp -s firmware/drivers/audio/wm8758.c "$T/wm8758_m128red.c"; then
        echo "FAIL: red run ($4): the line was not found in the driver"
        exit 1
    fi
    if ! build "$T/test_m128red" "$T/wm8758_m128red.c" "$M"; then
        echo "FAIL: compiling the red run ($4)"
        cat "$T/test_m128red.cc.log"; exit 1
    fi
    if red=$("$T/test_m128red"); then
        echo "FAIL: red run ($4) passed"; exit 1
    fi
    if ! echo "$red" | grep -q "^$3"; then
        echo "FAIL: red run ($4) failed for another reason:"; echo "$red"
        exit 1
    fi
    echo "ok   red run ($4) fails: $(echo "$red" | grep -c '^FAIL') checks"
}
m128_red 'm128 = dac_eq_wants_m128(band);' 'm128 = false;' \
    'FAIL band 4 +6: R7 0x001, want 0x101' 'the EQ never sets M128ENB'
m128_red 'bool on = dac_m128_eq || dac_m128_3d;' 'bool on = dac_m128_eq;' \
    'FAIL 3D on: R7 0x001, want 0x101' 'the 3D stage not counted'
exit 0
