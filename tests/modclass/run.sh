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

# tests/modclass: the module class accountant against devstats (p090-notescjk)
# The real firmware/export/devstats.h, its config.h and thread.h includes
# taken out (the stubs give the simulator's switches), copied fresh each
# run so the test reads the struct the firmware builds with.
set -e
cd "$(dirname "$0")/../.."
OUT=build/tests/modclass
mkdir -p "$OUT/inc"
sed -e '/^#include "config.h"/d' -e '/^#include "thread.h"/d' \
    firmware/export/devstats.h >"$OUT/inc/devstats.h"
grep -q 'font_cmds' "$OUT/inc/devstats.h" || { echo "FAIL: devstats.h has no font_cmds"; exit 1; }
${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
    -include tests/modclass/stubs/config.h -include tests/modclass/stubs/thread.h \
    -I"$OUT/inc" -Itests/modclass/stubs -Iapps \
    tests/modclass/test_modclass.c apps/ipodui/modclass.c -o "$OUT/test_modclass"
"$OUT/test_modclass"
