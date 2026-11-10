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

# A synced library's first run in the simulator (#225):
#
#   tests/perf/ipodctl_sim.sh [tracks] [generator args...]
#
# Makes build/sim-ipc-<tracks>: the build/sim-ui simulator (symlinked)
# with its own simdisk holding only tests/perf/ipodctl_library.py's
# library (iPod_Control/Music/F00..F49, hashed names; default 5,400
# tracks, 30 % of albums with an embedded cover), runs
# tests/perf/ipodctl-first.script and prints the scan, commit and art
# lines. Times are host times (Apple M3); counts transfer to the device.
# Device estimate for CPU-bound parts: x200-400 (scripts/check_trace.py
# SIM_FACTOR, uncalibrated); I/O comes from tests/perf/ipodctl_fat.sh.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-5400}
[ $# -gt 0 ] && shift
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-ipc-$N
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null
rm -rf "$B/simdisk/.rockbox"
mkdir -p "$B/simdisk"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a --exclude 'database*' --exclude artcache "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
echo "max files in playlist: 2000" >>"$B/simdisk/.rockbox/config.cfg"
# a 65k art pass outlasts the 10 min idle power-off (it powered the sim
# off at album 5,252 of 6,475): off for the measurement
echo "idle poweroff: 0" >>"$B/simdisk/.rockbox/config.cfg"
rm -f "$B/simdisk/iPod_Control/Music/F00/ZZZZ.mp3"
# #225 S3: the synthetic cover + song databases (tests/syncart's writer)
MKF=$ROOT/build/tmp/mkfixture
mkdir -p "$ROOT/build/tmp"
cc -std=gnu99 -O1 -I"$ROOT/tests/syncart" -o "$MKF" \
    "$ROOT/tests/syncart/mkfixture.c" "$ROOT/tests/syncart/fixwrite.c"
# a re-sync run (THUMBS_MIRROR) copies the re-sync's databases over the
# disk's: the library is written again, so every run starts from the
# first sync
[ -z "${THUMBS_MIRROR:-}" ] || rm -f "$B/simdisk/iPod_Control/.synthetic"
python3 "$ROOT/tests/perf/ipodctl_library.py" "$B/simdisk" "$N" \
    --mkfixture "$MKF" "$@"
OUT=$ROOT/out/shots/$(basename "${IPODCTL_SCRIPT:-ipodctl-first.script}" .script)-$N
rc=0
SIM_BUILD=$B SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-7200} \
    "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/${IPODCTL_SCRIPT:-ipodctl-first.script}" "$OUT" \
    >"$B/shots.out" 2>&1 || rc=$?
echo "== ipodctl first run, $N tracks (shots rc=$rc; log $OUT/sim.log)"
grep -E "tagcache: (scan|probe) (done|stopped)|scantrace ms=|db scan ms|commit|art build (start|done)|art pass done|wait_db|art_wait|syncdb" "$OUT/sim.log" \
    | sed 's/^ipodui: //'
# #225 S1: tracks under iPod_Control/Music never search for image files;
# every art pass that built albums stays at the pass's other lookups
# (EXISTS_MAX, default 40; image-file lookups only, the art cache's own
# probes left out: 11,166 / 10,281 at 5,400 tracks before S1, 0 / 0 after).
# A pass with no exists= line at all is a failure, not a pass.
EXISTS_MAX=${EXISTS_MAX:-40}
n=$(grep -c "art build done albums=[1-9].* exists=" "$OUT/sim.log" || true)
over=$(grep -o "art build done albums=[1-9][0-9]* .* exists=[0-9]*" \
       "$OUT/sim.log" | sed 's/.*exists=//' \
       | awk -v m="$EXISTS_MAX" '$1 > m' | wc -l | tr -d ' ')
S1_PASSES=${S1_PASSES:-2}
echo "== S1 check: $n art passes checked, $over over exists<=$EXISTS_MAX"
if [ "$n" -lt "$S1_PASSES" ] || [ "$over" -ne 0 ]; then
    echo "FAIL S1: art passes must look up <= $EXISTS_MAX files" \
         "(need >= $S1_PASSES passes)"
    [ $rc -ne 0 ] || rc=1
fi
# #225 S3: the synced covers' thumbnails hold the fixture's pixels (byte
# order, orientation, scale); SYNCED_MIN of them at least (default 1).
# THUMBS_MIRROR=1: the re-sync's mirrored pictures (ipodctl-resync), so
# a thumbnail left from the first sync fails
python3 "$ROOT/tests/syncart/check_thumbs.py" "$B/simdisk" \
    "${SYNCED_MIN:-1}" ${THUMBS_MIRROR:+--mirror} || { [ $rc -ne 0 ] || rc=1; }
# NP_KINDS (ipodctl-np): the source kinds of the Now Playing covers built
# (ok=1), in order: 2 embedded, 3 synced
if [ -n "${NP_KINDS:-}" ]; then
    got=$(grep -o "art npcover src key=[0-9a-f]* kind=[0-9]* ok=1" \
          "$OUT/sim.log" | sed 's/.*kind=\([0-9]*\).*/\1/' | tr '\n' ' ' \
          | sed 's/ $//')
    echo "== NP check: npcover kinds '$got', want '$NP_KINDS'"
    if [ "$got" != "$NP_KINDS" ]; then
        echo "FAIL NP: Now Playing's covers came from the wrong source"
        [ $rc -ne 0 ] || rc=1
    fi
fi
exit $rc
