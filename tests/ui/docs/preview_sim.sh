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

# Build the preview simulator for the site's "not released yet" images
# (tests/ui/docs/images.tsv rows on build/sim-rel-preview): the release-shaped
# sim (scripts/validate.sh's sim-rel-video line) with the 0.9.0 features that
# 0.8.3 compiles out switched on, IPODUI_AUDIOPHILE (#270) and IPODUI_NP_INFO
# (#277; firmware/export/config/ipodvideo.h). The defines go into the
# generated Makefile's EXTRA_DEFINES, the way validate.sh adds its module
# define; nothing in the tree changes. A preview build only: never a release.
#   sh tests/ui/docs/preview_sim.sh && scripts/docs_shots.sh --only preview-
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
D=$ROOT/build/sim-rel-preview
DEFS="-DIPODUI_AUDIOPHILE -DIPODUI_NP_INFO"
ARGS="--target=22 --type=s --ipodui --ipodui-release --bcmfw-video --bcmfw-merged"
mkdir -p "$D"
cd "$D"
if ! grep -qE "^export EXTRA_DEFINES=.* $DEFS\$" Makefile 2>/dev/null \
   || [ Makefile -ot "$ROOT/tools/configure" ]; then
    find . -name '*.o' -delete 2>/dev/null || true   # never mix objects built without the defines
    # shellcheck disable=SC2086
    "$ROOT/tools/configure" $ARGS </dev/null >configure.log 2>&1 \
        || { echo "preview_sim: configure failed (see $D/configure.log)" >&2; exit 1; }
    sed -i.bak "/^export EXTRA_DEFINES=/s/\$/ $DEFS/" Makefile && rm -f Makefile.bak
    grep -qE "^export EXTRA_DEFINES=.* $DEFS\$" Makefile \
        || { echo "preview_sim: could not add $DEFS to the Makefile" >&2; exit 1; }
fi
make -j"${JOBS:-10}" >make.log 2>&1 || { echo "preview_sim: make failed (see $D/make.log)" >&2; exit 1; }
echo "preview_sim: $D/rockboxui with $DEFS"
