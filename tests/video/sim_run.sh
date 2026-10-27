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

# The video player end to end in the simulator (#64): builds the
# --bcmfw-video sim in build/sim-video when needed, puts a 12 s test clip
# (our corpus, never anyone's media) on its disk and runs
# tests/video/sim_play.script through scripts/shots.sh.  Screenshots of
# the panel land in out/shots/sim_play/.  Not part of the gate (it needs
# a build of its own); run it after touching apps/video/ or the sim's BCM
# (uisimulator/common/sim_video.c).
set -e
cd "$(dirname "$0")/../.."
SIM=build/sim-video
if [ ! -x "$SIM/rockboxui" ]; then
    mkdir -p "$SIM"
    (cd "$SIM" && ../../tools/configure --target=22 --type=s --ipodui \
        --bcmfw-video </dev/null >/dev/null)
fi
(cd "$SIM" && make -j8 >/dev/null)
CLIP=build/video/mp4/long_ipod.mp4
if [ ! -f "$CLIP" ]; then
    echo "making the test clips (tests/video/run.sh)"
    sh tests/video/run.sh >/dev/null
fi
mkdir -p "$SIM/simdisk/Videos"
cp "$CLIP" "$SIM/simdisk/Videos/clip.mp4"
SIM_BUILD=$SIM scripts/shots.sh tests/video/sim_play.script
cp bcmfw/video/stage6-clip.mp4 "$SIM/simdisk/Videos/stage6.mp4"
cp bcmfw/video/stage6-real.mp4 "$SIM/simdisk/Videos/stage6-real.mp4"
SIM_BUILD=$SIM scripts/shots.sh tests/video/sim_stage6.script
