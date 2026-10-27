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

# Search at 50k tracks (docs/phase11-brief.md section 6 item 3, section 7
# criterion 3): host CPU time of lib_search() per keystroke over a
# synthetic 50,000-track library (tests/search/bench50k.c: library.c and
# search_core.c over the fake tagcache of tests/ipodui), then the ARM7
# instruction count of srch_match() per byte (tests/search/insn.sh),
# which is the device gate (the host time is not; the simulator drives
# the same code once U1's Search screen exists). A measurement, not a
# gate; not run by scripts/test.sh.
#
#   tests/perf/search50k.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SRC="apps/ipodui/library.c apps/ipodui/sortkey.c apps/ipodui/artkey.c"
SRC="$SRC apps/ipodui/artfmt.c apps/ipodui/search_core.c"
# shellcheck disable=SC2086
"$HOSTCC" -std=gnu99 -O2 -Wall -Itests/search/stub -Itests/ipodui/libstub \
    -Itests/ipodui -DARTFMT_HOST -DLIB_P11_HOST -Iapps -Iapps/ipodui \
    -o "$TMP/bench" tests/search/bench50k.c tests/ipodui/fake_tagcache.c \
    $SRC || exit 0
(cd "$TMP" && mkdir -p db && ./bench db)
echo
tests/search/insn.sh
