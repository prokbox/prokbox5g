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
# p090-osr128: the 88.2/96 kHz EQ guard and M128ENB user (test_hr.c), six
# red runs.
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
sed 's/dac_osr64 = dac_osr64_for(fsel);/dac_osr64 = false;/' \
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

# red run 2 (0.9.0 Hi-Res 128x): the switch ignored (64x stays at 88.2/96)
sed 's/dac_osr128_rates = rates & (DAC_OSR128_88 | DAC_OSR128_96);/dac_osr128_rates = 0;/' \
    firmware/drivers/audio/wm8758.c > "$T/wm8758_red128.c"
if cmp -s firmware/drivers/audio/wm8758.c "$T/wm8758_red128.c"; then
    echo "FAIL: red run 2: the 128x mask line was not found in the driver"
    exit 1
fi
if ! build "$T/test_red128" "$T/wm8758_red128.c"; then
    echo "FAIL: compiling red run 2"; cat "$T/test_red128.cc.log"; exit 1
fi
if red=$("$T/test_red128"); then
    echo "FAIL: red run 2 passed with the 128x switch ignored"; exit 1
fi
if ! echo "$red" | grep -q "^FAIL 128x: on at 96k, stopped: R10 0x000, want 0x008"; then
    echo "FAIL: red run 2 failed for another reason:"; echo "$red"; exit 1
fi
echo "ok   red run 2 (Hi-Res 128x ignored) fails: $(echo "$red" | grep -c '^FAIL') checks"

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
m128_red 'bool on = dac_m128_eq || dac_m128_3d || dac_m128_hr;' 'bool on = dac_m128_eq || dac_m128_hr;' \
    'FAIL 3D on: R7 0x001, want 0x101' 'the 3D stage not counted'

# p090-osr128: the high-rate EQ guard and the M128ENB user (test_hr.c)
H=tests/dacosr/test_hr.c
if ! build "$T/test_hr" firmware/drivers/audio/wm8758.c "$H"; then
    echo "FAIL: compiling the high-rate switch test"; cat "$T/test_hr.cc.log"
    exit 1
fi
if out=$("$T/test_hr"); then
    echo "ok   88.2/96 kHz EQ guard and M128ENB user: $(echo "$out" | tail -1)"
else
    echo "$out"; echo "FAIL high-rate switches"; exit 1
fi
hr_red() {
    sed "s/$1/$2/" firmware/drivers/audio/wm8758.c > "$T/wm8758_hrred.c"
    if cmp -s firmware/drivers/audio/wm8758.c "$T/wm8758_hrred.c"; then
        echo "FAIL: red run ($4): the line was not found in the driver"
        exit 1
    fi
    if ! build "$T/test_hrred" "$T/wm8758_hrred.c" "$H"; then
        echo "FAIL: compiling the red run ($4)"
        cat "$T/test_hrred.cc.log"; exit 1
    fi
    if red=$("$T/test_hrred"); then
        echo "FAIL: red run ($4) passed"; exit 1
    fi
    if ! echo "$red" | grep -q "^$3"; then
        echo "FAIL: red run ($4) failed for another reason:"; echo "$red" |
            head -5
        exit 1
    fi
    echo "ok   red run ($4) fails: $(echo "$red" | grep -c '^FAIL') checks"
}
hr_red 'bool bypass = dac_hr_guard \&\& hi;' 'bool bypass = false;' \
    'FAIL 44.1 -> 96 with the EQ on: EQ flat' 'no EQ guard'
hr_red '        band = NULL;' ';' 'FAIL preset at 96: EQ flat' \
    'a preset at 96 not guarded'
hr_red '!dac_state.hw_eq \&\& !DAC_HR_BYPASS() \&\& ' '!dac_state.hw_eq \&\& ' \
    'FAIL bass\/treble at 96: EQ flat' 'bass and treble not guarded'
hr_red 'bool on = dac_m128_eq || dac_m128_3d || dac_m128_hr;' \
    'bool on = dac_m128_eq || dac_m128_3d;' \
    'FAIL switch on live, 96 at 128x' 'the high-rate user not counted'
hr_red 'if (hi \&\& !(dacctrl_reg \& DACCTRL_SOFTMUTE)' 'if (hi \&\& false' \
    'FAIL switch on live: R7 written muted' 'a live switch without the mute'
hr_red 'dac_hr_sync();      \/\* p090-osr128: M128ENB, before the unmute \*\/' ';' \
    'FAIL switch on, 88.2 at 64x' 'Hi-Res 128x off live leaves M128ENB'
# b18's release defaults (the owner's answers, 2026-11-19), each against
# its old or wrong value
hr_red '#define DAC_M128_HIRATE_DEFAULT 1' '#define DAC_M128_HIRATE_DEFAULT 0' \
    'FAIL default: M128ENB user on' 'the M128ENB user off by default'
hr_red '#define DAC_HR_GUARD_DEFAULT 0' '#define DAC_HR_GUARD_DEFAULT 1' \
    'FAIL default: EQ guard off' 'the EQ guard on by default'

# #203/#327: the EQ's precut, make-up, write order, limiter and AMUTE
# (test_eqlev.c, with the shell's level plan from eqlab_core.c)
E=tests/dacosr/test_eqlev.c
EXTRA="apps/ipodui/eqlab_core.c apps/ipodui/eq_hw_presets.c"
build_eq() {
    "$CC" -std=gnu99 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
        -Itests/dacosr/stub -Ifirmware/export -Iapps \
        -o "$1" "$E" "$2" $EXTRA 2>"$1.cc.log"
}
if ! build_eq "$T/test_eqlev" firmware/drivers/audio/wm8758.c; then
    echo "FAIL: compiling the EQ level test"; cat "$T/test_eqlev.cc.log"
    exit 1
fi
if out=$("$T/test_eqlev"); then
    echo "ok   EQ precut, make-up, limiter, AMUTE: $(echo "$out" | tail -1)"
else
    echo "$out" | head -20; echo "FAIL EQ level writes"; exit 1
fi
eq_red() {
    sed "s/$1/$2/" firmware/drivers/audio/wm8758.c > "$T/wm8758_eqred.c"
    if cmp -s firmware/drivers/audio/wm8758.c "$T/wm8758_eqred.c"; then
        echo "FAIL: red run ($4): the line was not found in the driver"
        exit 1
    fi
    if ! build_eq "$T/test_eqred" "$T/wm8758_eqred.c"; then
        echo "FAIL: compiling the red run ($4)"
        cat "$T/test_eqred.cc.log"; exit 1
    fi
    if red=$("$T/test_eqred"); then
        echo "FAIL: red run ($4) passed"; exit 1
    fi
    if ! echo "$red" | grep -q "^$3"; then
        echo "FAIL: red run ($4) failed for another reason:"; echo "$red" |
            head -5
        exit 1
    fi
    echo "ok   red run ($4) fails: $(echo "$red" | grep -c '^FAIL') checks"
}
eq_red 'dac_level_apply(MAX(pc_new, dac_precut), MIN(makeup_db10, dac_eq_makeup));' \
    ';' 'FAIL write [0-9]*: R11 cut' 'the EQ written before its cut'
eq_red 'int trim = dac_state.gain_db10 + dac_eq_makeup;' \
    'int trim = dac_state.gain_db10;' 'FAIL preset [0-9]*: amp' \
    'no make-up (the pre-#203 level)'
eq_red 'lim = band != NULL \&\& dac_lim_allow;' 'lim = dac_lim_allow;' \
    'FAIL EQ off: limiter off' 'the limiter on with the EQ off'
eq_red 'if (dac_amute)' 'if (true)' 'FAIL AMUTE off by default' \
    'AMUTE always on'
eq_red '#define DAC_LIMITER_DEFAULT 1' '#define DAC_LIMITER_DEFAULT 0' \
    'FAIL limiter off by default with the EQ on' \
    'the limiter off by default (pre-b18)'
eq_red '#define DAC_AMUTE_DEFAULT 0' '#define DAC_AMUTE_DEFAULT 1' \
    'FAIL AMUTE off by default' 'AMUTE on by default'
exit 0
