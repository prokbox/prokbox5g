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

# Regenerate every hardware diagram: <name>.d2 -> <name>.svg (print, docs) and <name>.png (preview).
# Needs D2 v0.9.0 (`d2`). Layout engine: TALA (bundled with d2) for most; ELK for the data-path set
# (data-paths*), whose layered routing keeps their labels apart. LAYOUT=elk|dagre|tala overrides all.
# Usage: docs/diagrams/render.sh [name ...]     (no names: all diagrams)
set -eu
cd "$(dirname "$0")"

PAD=40

if [ "$#" -eq 0 ]; then
    set -- system audio-bus video-path memory-maps \
        data-paths data-paths-pp data-paths-bcm data-paths-audio
fi

for name in "$@"; do
    case "$name" in
        data-paths*) layout=${LAYOUT:-elk} ;;
        *)           layout=${LAYOUT:-tala} ;;
    esac
    d2 --layout "$layout" --pad "$PAD" "$name.d2" "$name.svg"
    d2 --layout "$layout" --pad "$PAD" --scale 1 "$name.d2" "$name.png"
done
