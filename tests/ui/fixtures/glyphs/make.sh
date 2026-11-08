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

# Regenerate the font-coverage fixtures (tests/ui/ipodui-font-coverage.script):
# two 2 s silent MP3s whose UTF-8 tags carry the characters the shell fonts
# draw beyond ASCII (music notes, curly quotes, dashes, signs, accented
# Latin, ligatures). The committed files are the reference; run this only
# to change the strings. Requires ffmpeg (libmp3lame).
set -eu
D=$(cd "$(dirname "$0")" && pwd)
mk() {
    ffmpeg -loglevel error -y -f lavfi -i anullsrc=r=22050:cl=mono -t 2 \
        -c:a libmp3lame -b:a 32k -id3v2_version 3 -bitexact \
        -metadata artist="♪ Sigrún Ósk & Łódź № 5" \
        -metadata album_artist="♪ Sigrún Ósk & Łódź № 5" \
        -metadata album="„Grüße“ ‘Ærø’ «Ça» – ﬁnal ﬂow … ŉ ș ț" \
        -metadata title="$2" -metadata track="$1" "$D/$3"
}
mk 1 "♪ Harbour Lights ♪ ♫ ♩ ♬ € £ ™ © • † ‡ ′ ″ ½ ×" glyphs1.mp3
mk 2 "[MUSIC ♪] — “Don’t” ‹a› »b« ‰ ¿¡ ° ± ¹²³ Œuvre" glyphs2.mp3
