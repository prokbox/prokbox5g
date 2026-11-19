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

# 10,000 CJK-tagged songs on their own sim disk (#235, package G):
#   tests/perf/cjk10k.sh [tracks]
# build/sim-cjk10k (the build/sim-ui binary, its installed files without
# the database, tests/fixtures/cjk/make_cjk_library.py's timing library in
# /Music/CJK), then tests/perf/cjk-10k.script; prints the build and list
# lines from out/shots/cjk-10k/sim.log.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-10000}
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-cjk$((N / 1000))k
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null
mkdir -p "$B/simdisk"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a --exclude 'database*' "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
echo "max files in playlist: 2000" >>"$B/simdisk/.rockbox/config.cfg"
python3 "$ROOT/tests/fixtures/cjk/make_cjk_library.py" "$B/simdisk/Music/CJK" "$N"
OUT=$ROOT/out/shots/cjk-$((N / 1000))k
SIM_BUILD=$B SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-1800} \
    "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/cjk-10k.script" "$OUT" >/dev/null
grep -E "wait_db|lib open|RAM cache|cjk tier|commit" "$OUT/sim.log" | head -40
