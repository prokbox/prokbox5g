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

# tests/fixtures/upfix: the committed sources tests/upfix/gen.py crafts its
# variants from. Made once with ffmpeg 9.0.2 (ffprobe 9.0.2: aac, 44100 Hz,
# 1.000000 s, 45 frames); re-make only on purpose, then re-run
# tests/upfix/run.sh. Layout ffmpeg writes: ftyp, free (8 bytes), mdat,
# moov at the end; the variants depend on it.
set -e
cd "$(dirname "$0")/upfix"
ffmpeg -nostdin -loglevel error -f lavfi \
    -i sine=frequency=440:duration=1:sample_rate=44100 \
    -c:a aac -b:a 48k -ac 1 -fflags +bitexact -flags:a +bitexact \
    -map_metadata -1 -metadata title=Upfix -metadata artist=Tester \
    -y base.m4a
