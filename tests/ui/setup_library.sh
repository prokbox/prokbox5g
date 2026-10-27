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

# Generate the Phase 4 test library (~300 tagged tracks) for simulator UI
# tests of the database-backed Music menu (docs/phase4-brief.md, Agent C).
#
#   tests/ui/setup_library.sh <simdisk-dir>
#
# The contents are defined in tests/ui/setup_library.py (which documents the
# layout); this wrapper only guards the destination and keeps it idempotent.
# Writes into <simdisk-dir> (normally $SIM_BUILD/simdisk, under the
# gitignored build/; anything else is refused):
#
#   Library/...      music (47 artists, 59 albums + 1 compilation)
#   Podcasts/...     2 shows x 2 episodes, genre Podcast
#   Audiobooks/...   3 .m4b books with chapters
#   Playlists/...    4 playlists (.m3u/.m3u8, one empty)
#   .rockbox/config.cfg   tagcache_ram: on, tagcache_autoupdate: on
#
# Idempotent: Library/.test-library holds "<VERSION> <file count>"; when it
# matches and the files are there the script exits at once. Regenerating
# deletes the tagcache files (.rockbox/database_*.tcd) so the database is
# rebuilt from the new library. Bump VERSION in setup_library.py whenever
# the contents change (and refresh tests/ui/library_manifest.txt with
# "python3 tests/ui/setup_library.py --manifest > tests/ui/library_manifest.txt",
# and tests/ui/art_manifest.txt (cover-art variety, Phase 4b) with
# "python3 tests/ui/setup_library.py --art-manifest > tests/ui/art_manifest.txt").
#
# Requires ffmpeg (with libmp3lame) and python3. The progressive-JPEG art
# fixture additionally wants jpegtran or cjpeg (libjpeg-turbo, commonly
# installed alongside ffmpeg); without either, generate() falls back to a
# plain copy and prints a warning (see setup_library.py's docstring).
set -eu

[ -n "${1:-}" ] || { echo "usage: $0 <simdisk-dir>" >&2; exit 2; }
DISK=$1
HERE=$(cd "$(dirname "$0")" && pwd -P)
# Only ever write inside the repo's gitignored build/ tree.
ROOT=$(cd "$HERE/../.." && pwd -P)
PARENT=$(cd "$(dirname "$DISK")" 2>/dev/null && pwd -P) \
    || { echo "setup_library: no such directory: $(dirname "$DISK")" >&2; exit 2; }
case "$PARENT/$(basename "$DISK")/" in
    "$ROOT/build/"*) ;;
    *) echo "setup_library: refusing to write outside $ROOT/build/: $DISK" >&2; exit 2;;
esac
mkdir -p "$DISK"
DISK=$(cd "$DISK" && pwd -P)

STAMP="$DISK/Library/.test-library"
want=$(python3 "$HERE/setup_library.py" --stamp)
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ]; then
    have=$(find "$DISK/Library" "$DISK/Podcasts" "$DISK/Audiobooks" \
               -type f \( -name '*.mp3' -o -name '*.m4b' \) 2>/dev/null | wc -l)
    if [ "${want#* }" -eq "$have" ]; then
        python3 "$HERE/setup_library.py" --config "$DISK"
        exit 0
    fi
fi

command -v ffmpeg >/dev/null || { echo "setup_library: ffmpeg not found" >&2; exit 1; }
TMP=$(mktemp -d "$ROOT/build/.setup_library.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
python3 "$HERE/setup_library.py" --generate "$DISK" "$TMP"
