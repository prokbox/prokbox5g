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

# #276: the library page's time with art in the library, before and after
# "library building takes utter priority".
#
#   tests/perf/libfirst_measure.sh <tracks> <sim build dir> <tag>
#
# Makes build/sim-lf-<tracks>-<tag>: the given simulator with a synthetic
# library of <tracks> (tests/perf/synthetic_library.py) and a real cover
# (tests/ui/fixtures/sig-cover.jpg) as cover.jpg in every album folder, so
# the art pass has decodes to do. The art decodes are made to cost what
# they cost on the ARM (art_decode_spin, LF_SPIN us per yield point,
# default 2000) in both arms. One boot:
#   1. the first build, no library on the disk: the boot page's time
#      ("upd done ms=") and the first build's ("db first build");
#   2. the art made, then a USB session that adds LF_ADD songs (default
#      40) in a new folder with its own cover and rewrites one album's
#      track: the post-USB page's time ("update screen done ms=").
# Times are host times (sim); compare the arms, not the device.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:?tracks}
UI=$(cd "${2:?sim build dir}" && pwd)
TAG=${3:?tag}
SPIN=${LF_SPIN:-2000}
ADD=${LF_ADD:-40}
B=$ROOT/build/sim-lf-$N-$TAG
[ -x "$UI/rockboxui" ] || { echo "no $UI/rockboxui" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null
rm -rf "$B"
mkdir -p "$B/simdisk"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a --exclude 'database*' --exclude artcache "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
echo "idle poweroff: 0" >>"$B/simdisk/.rockbox/config.cfg"
python3 "$ROOT/tests/perf/synthetic_library.py" "$B/simdisk" "$N" >/dev/null
COVER=$ROOT/tests/ui/fixtures/sig-cover.jpg
find "$B/simdisk/Synth" -name '*.mp3' -exec dirname {} \; | sort -u \
    | while read -r d; do cp "$COVER" "$d/cover.jpg"; done
FIRST=$(find "$B/simdisk/Synth" -name '*.mp3' | sort | head -1 | sed "s|$B/simdisk||")
S=$B/libfirst.script
{
    echo "# timeout: 7200"
    echo "art_decode_spin $SPIN"
    echo "wait_db 3600000"
    echo "wait 5000"
    echo "art_wait 3600000"
    echo "wait 3000"
    echo "copy_file $FIRST /lf_one.mp3"
    echo "usb_connect"
    echo "wait 2500"
    echo "mkdir /Synth/LfNew"
    echo "copy $COVER /Synth/LfNew/cover.jpg"
    i=1
    while [ $i -le "$ADD" ]; do
        echo "copy fixtures/refresh.mp3 /Synth/LfNew/t$i.mp3"
        i=$((i + 1))
    done
    echo "copy_file /lf_one.mp3 $FIRST"
    echo "usb_disconnect"
    echo "wait 5000"
    echo "wait_db 3600000"
    echo "wait 5000"
    echo "art_wait 3600000"
    echo "shot end"
    echo "quit"
} >"$S"
# fixtures/ paths are relative to tests/ui (shots.sh)
cp "$S" "$ROOT/tests/ui/.lf-$N-$TAG.script"
rc=0
SIM_BUILD=$B SHOTS_TIMEOUT=7200 "$ROOT/scripts/shots.sh" \
    "$ROOT/tests/ui/.lf-$N-$TAG.script" "$ROOT/out/shots/lf-$N-$TAG" \
    >"$B/shots.out" 2>&1 || rc=$?
rm -f "$ROOT/tests/ui/.lf-$N-$TAG.script"
LOG=$ROOT/out/shots/lf-$N-$TAG/sim.log
echo "== libfirst $N tracks, $TAG (shots rc=$rc; log $LOG)"
grep -E "upd (push|done|stuck)|update screen (done|left)|library page|art build (start|done)|art libwork|art yield lib|art lib wait|db first build|tagcache: (scan|probe) (done|stopped)" "$LOG" \
    | sed 's/^ipodui: //' | head -60
echo "libwork lines: $(grep -c 'art libwork' "$LOG" || true)"
exit $rc
