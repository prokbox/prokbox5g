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

# Device defaults (#142): backlight 10 s and Clicker on, read from the
# header as the device build sees it (no SIMULATOR); the shell's backlight
# list must hold the default; settings_list.c must use the macros.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/defaults
cc -std=c99 -Wall -Werror -Iapps -DHAVE_IPOD_UI -DHAVE_BACKLIGHT -DEXPECT_DEVICE \
   tests/defaults/defaults_test.c -o build/defaults/dev
cc -std=c99 -Wall -Werror -Iapps -DHAVE_IPOD_UI -DHAVE_BACKLIGHT -DSIMULATOR \
   tests/defaults/defaults_test.c -o build/defaults/sim
build/defaults/dev
build/defaults/sim
grep -q 'bl_values\[\] = { -1, 2, 5, 10, 20, 30, 0 }' apps/ipodui/screens/settings.c \
    || { echo "FAIL: shell backlight list lost the 10 s entry"; exit 1; }
n=$(grep -c 'DEFAULT_KEYCLICK' apps/settings_list.c)
[ "$n" -ge 4 ] || { echo "FAIL: settings_list.c does not use the keyclick macros"; exit 1; }
grep -q 'DEFAULT_BACKLIGHT_TIMEOUT, "backlight timeout"' apps/settings_list.c \
    || { echo "FAIL: backlight entry not on the macro"; exit 1; }
echo "defaults: PASS"
