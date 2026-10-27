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

# Hardware Sound Check fixtures (docs/phase15-brief.md section 3.5, wave
# 2b; tests/ui/ipodui-p15-soundcheck.script): three 4 s MP3s with
# ReplayGain tags (ID3v2 TXXX REPLAYGAIN_*), committed; rerun to remake.
#   sc_a.mp3  track -6.50 dB              -> gain_db10=-65
#   sc_b.mp3  track +3.20 dB              -> gain_db10=32
#   sc_d.mp3  album -4.00 dB, no track    -> gain_db10=-40 (the fallback)
# Requires ffmpeg (with libmp3lame).
set -e
cd "$(dirname "$0")"
FF="ffmpeg -nostdin -v error -y"
MP3="-c:a libmp3lame -b:a 32k -ac 1 -ar 22050 -id3v2_version 3 -fflags +bitexact -flags:a +bitexact"
mk() {
    out=$1 freq=$2; shift 2
    $FF -f lavfi -i "sine=frequency=$freq:sample_rate=22050" -t 4 $MP3 \
        -metadata title="$out" -metadata artist="SC Test" \
        -metadata album="Sound Check" "$@" "$out"
}
mk sc_a.mp3 330 -metadata REPLAYGAIN_TRACK_GAIN="-6.50 dB"
mk sc_b.mp3 440 -metadata REPLAYGAIN_TRACK_GAIN="+3.20 dB"
mk sc_d.mp3 660 -metadata REPLAYGAIN_ALBUM_GAIN="-4.00 dB"
