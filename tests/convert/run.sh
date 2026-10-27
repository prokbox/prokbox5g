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

# Converter tests (Phase 14a V1): scripts/prokbox-convert.py against
# synthetic sources and the committed CC BY clip bcmfw/video/stage6-real.mp4.
# Skips cleanly (exit 0) when ffmpeg/ffprobe are not installed.  Scratch
# lives in build/convert-test/ (removed and rebuilt each run).  Chapter forms
# are parsed with warble (build/warble, see scripts/test.sh); without it
# those checks are skipped with a notice.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

FFMPEG=$(command -v ffmpeg || echo /opt/homebrew/bin/ffmpeg)
FFPROBE=$(command -v ffprobe || echo /opt/homebrew/bin/ffprobe)
if [ ! -x "$FFMPEG" ] || [ ! -x "$FFPROBE" ]; then
    echo "SKIP tests/convert: ffmpeg/ffprobe not found"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP tests/convert: python3 not found"
    exit 0
fi
if [ -z "${WARBLE:-}" ]; then
    for w in build/warble/warble.*; do
        [ -x "$w" ] && WARBLE=$w && break
    done
fi
export WARBLE="${WARBLE:-}" FFMPEG FFPROBE
exec python3 tests/convert/test_convert.py
