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

# Scan cost of an AAC library and the boost of each dedicated build (p081).
#
#   tests/perf/m4a_scan.sh [mp3_tracks] [m4a_tracks]
#
# Makes build/sim-m4a: the build/sim-ui simulator with its own simdisk:
# <mp3_tracks> synthetic MP3s (tests/perf/synthetic_library.py, default
# 1000), <m4a_tracks> tagged 3 s AAC files (default 400, half with 'moov'
# at the end as ffmpeg writes them, half moved first), every m4b chapter
# fixture three times in each layout, and a 30-minute chaptered book with
# 'moov' at the end. Then it runs the first build, a reboot, a USB plug with
# one new file (the post-USB scan finds a change: a candidate), and a
# playing update whose commit waits for the stop (the delayed commit), and
# one USB plug in the middle of a slowed scan (a cancel).
#
# Prints the devmode "scantrace" lines (apps/tagcache.c: per stage ms and
# storage commands; in the simulator a command is a file open, read or
# write, filesystem-sim.c) and the "dedicated on/off" lines, then checks
# them with tests/perf/boost_balance.py (every "on" has its "off", the
# depth is 0 after a cut scan and at the end, each waited-on build held
# it). Exit 1 on a failed check, 2 when the run inspected nothing.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NMP3=${1:-1000}
NM4A=${2:-400}
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-m4a
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
command -v ffmpeg >/dev/null || { echo "needs ffmpeg" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null
rm -rf "$B"
mkdir -p "$B/simdisk"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a --exclude 'database*' "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
[ "$NMP3" -gt 0 ] \
    && python3 "$ROOT/tests/perf/synthetic_library.py" "$B/simdisk" "$NMP3" >/dev/null

# the AAC files (generated once per run; 3 s of a tone, tagged)
T=$B/gen
mkdir -p "$T"
ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=3" -c:a aac -b:a 64k \
    -metadata title=Tone -metadata artist=Gen -metadata album=Gen \
    "$T/tone.m4a"
python3 "$ROOT/tests/audiobook/faststart.py" "$T/tone.m4a" "$T/tone_fs.m4a"
# a 30-minute book, 12 chapters, 'moov' at the end
{
    echo ";FFMETADATA1"
    i=0
    while [ $i -lt 12 ]; do
        echo "[CHAPTER]"; echo "TIMEBASE=1/1000"
        echo "START=$((i * 150000))"; echo "END=$(((i + 1) * 150000))"
        echo "title=Part $((i + 1))"
        i=$((i + 1))
    done
} >"$T/chapters.txt"
ffmpeg -v error -f lavfi -i "sine=frequency=220:duration=1800" \
    -i "$T/chapters.txt" -map_metadata 1 -map_chapters 1 \
    -c:a aac -b:a 32k -ac 1 -metadata title=LongBook -metadata artist=Gen \
    "$T/longbook.m4b"

D=$B/simdisk/AAC
mkdir -p "$D/Album0" "$D/Album1" "$D/Album2"    # the script's new files
i=0
while [ $i -lt "$NM4A" ]; do
    a=$((i / 10))
    mkdir -p "$D/Album$a"
    if [ $((i % 2)) -eq 0 ]; then src=$T/tone.m4a; else src=$T/tone_fs.m4a; fi
    cp "$src" "$D/Album$a/Track$i.m4a"
    i=$((i + 1))
done
mkdir -p "$B/simdisk/Books"
for f in "$ROOT"/tests/fixtures/*.m4b; do
    n=$(basename "$f" .m4b)
    for k in 1 2 3; do
        cp "$f" "$B/simdisk/Books/${n}_$k.m4b"
        python3 "$ROOT/tests/audiobook/faststart.py" "$f" \
            "$B/simdisk/Books/${n}_fs$k.m4b"
    done
done
cp "$T/longbook.m4b" "$B/simdisk/Books/LongBook.m4b"
cp "$T/tone.m4a" "$B/simdisk/new_track.m4a.in"

S=$B/m4a_scan.script
cat >"$S" <<'EOF2'
# timeout: 900
wait 3000
wait_db 600000
wait 20000
reboot
wait 3000
wait_db 600000
wait 20000
usb_connect
wait 2500
copy_file /new_track.m4a.in /AAC/Album0/NewTrack.m4a
usb_disconnect
wait_commit 600000
wait 20000
play_files /AAC/Album1/Track10.m4a
wait 2000
copy_file /new_track.m4a.in /AAC/Album1/NewTrack2.m4a
update_db
wait 15000
stop
wait_commit 600000
wait 20000
scan_slow 20
usb_connect
wait 2500
copy_file /new_track.m4a.in /AAC/Album2/NewTrack3.m4a
usb_disconnect
wait 1500
usb_connect
wait 3000
usb_disconnect
scan_slow 0
wait_commit 600000
wait 20000
shot end
quit
EOF2
rc=0
SIM_BUILD=$B SHOTS_TIMEOUT=900 "$ROOT/scripts/shots.sh" "$S" >"$B/shots.out" 2>&1 || rc=$?
LOG=$ROOT/out/shots/m4a_scan/sim.log
echo "== m4a scan: $NMP3 mp3, $NM4A m4a, books (shots rc=$rc; log $LOG)"
grep -E "scantrace|db scan ms|db commit ms|dedicated" "$LOG" \
    | sed 's/^ipodui: //' || true
python3 "$ROOT/tests/perf/boost_balance.py" "$LOG"
