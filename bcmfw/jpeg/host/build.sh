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

# Build the S1 reference (jpegref) against the simulator's own objects of
# the shell's decoder and resizer (build/sim-ui must be built), with the
# simulator's compile flags.  Usage: bcmfw/jpeg/host/build.sh [OUTDIR]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SIM=$ROOT/build/sim-ui
OUT=${1:-$ROOT/build/jpeg-host}
mkdir -p "$OUT"
for o in jpeg_load resize bmp; do
    test -f "$SIM/apps/recorder/$o.o" || { echo "build/sim-ui first"; exit 1; }
done
# the simulator's command line for one of its own files, re-aimed at ours
touch -c "$ROOT/apps/recorder/bmp.c"
CMD=$(make -C "$SIM" -n 2>/dev/null | grep -m1 "gcc.* -c .*apps/recorder/bmp.c")
test -n "$CMD" || { echo "no compile line from build/sim-ui"; exit 1; }
CMD=$(echo "$CMD" | sed -e "s| -o [^ ]*bmp.o| -o $OUT/jpegref.o|" \
                        -e "s| [^ ]*apps/recorder/bmp.c| $HERE/jpegref.c|" \
                        -e "s| -MMD -MP||; s| -MF [^ ]*||; s| -MT [^ ]*||")
eval "$CMD"
CC=$(echo "$CMD" | awk '{for (i = 1; i <= NF; i++) if ($i ~ /gcc/) { print $i; exit }}')
$CC -O2 -c "$HERE/stubs.c" -o "$OUT/stubs.o"
$CC -o "$OUT/jpegref" "$OUT/jpegref.o" "$OUT/stubs.o" \
    "$SIM/apps/recorder/jpeg_load.o" "$SIM/apps/recorder/resize.o" "$SIM/apps/recorder/bmp.o"
echo "built $OUT/jpegref"
