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

# #146 fixtures for tests/ui/ipodui-146-audiobook-classify.script: three
# books found by one rule each and a decoy music album. Regenerate with
#   sh tests/ui/fixtures/p146/make.sh
# (committed; small: 8 kHz mono, low bit rate, 12 s a file).
#   type.m4b          "Classify Type Book", genre Fiction: the m4b type rule
#   genre.mp3         "Classify Genre Book", genre "spoken word": genre rule
#   folder_cd1.mp3,   "Classify Folder Book" discs 1 and 2, genre Fiction:
#   folder_cd2.mp3    the folder rule (copied under /Audiobooks/.../CD1, CD2)
#   decoy.mp3         "Classify Decoy Album", genre Rock: stays music
set -e
cd "$(dirname "$0")"
gen() {     # out freq title album disc genre
    out=$1; hz=$2; title=$3; album=$4; disc=$5; genre=$6
    case "$out" in
        *.m4b) codec="-c:a aac -b:a 16k -f ipod" ;;
        *)     codec="-c:a libmp3lame -b:a 8k -id3v2_version 3" ;;
    esac
    # shellcheck disable=SC2086
    ffmpeg -nostdin -loglevel error -y -f lavfi \
        -i "sine=frequency=$hz:duration=12:sample_rate=8000" -ac 1 $codec \
        -metadata title="$title" -metadata album="$album" \
        -metadata artist="Classify Author" \
        -metadata album_artist="Classify Author" \
        -metadata genre="$genre" -metadata disc="$disc" \
        -metadata track=1/1 -metadata date=2026 "$out"
}
gen type.m4b       330 "Classify Type Book"   "Classify Type Book"   1/1 Fiction
gen genre.mp3      392 "Classify Genre Book"  "Classify Genre Book"  1/1 "spoken word"
gen folder_cd1.mp3 440 "Part One"             "Classify Folder Book" 1/2 Fiction
gen folder_cd2.mp3 494 "Part Two"             "Classify Folder Book" 2/2 Fiction
gen decoy.mp3      523 "Decoy Song"           "Classify Decoy Album" 1/1 Rock
