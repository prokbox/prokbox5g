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

# Font sanity check for the iPod UI shell.
#
#   scripts/check_fonts.sh <sim_build>      e.g. build/sim-ui
#
# Reads the expected font files from apps/ipodui/fonts/MANIFEST (one .fnt
# file name per line, first whitespace-separated field; '#' starts a
# comment; written by the font build, see docs/design/fonts.md) and checks
# that each one is installed in <sim_build>/simdisk/.rockbox/fonts/, is a
# valid RB12 font, and (when the committed copy apps/ipodui/fonts/<name>
# exists) is byte-identical to it. Prints the installed fonts' metrics
# with scripts/fntinfo.py. Without a MANIFEST (before the Phase 3 fonts)
# it prints a note and succeeds.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
[ -n "${1:-}" ] || { echo "usage: $0 <sim_build>" >&2; exit 2; }
B=$1
MAN=${FONT_MANIFEST:-apps/ipodui/fonts/MANIFEST}   # override for testing
SRCDIR=$(dirname "$MAN")
DIR=$B/simdisk/.rockbox/fonts

if [ ! -f "$MAN" ]; then
    echo "note font check skipped: no $MAN yet"
    exit 0
fi

fail=0
found=""
for name in $(sed -e 's/#.*//' "$MAN" | awk 'NF { print $1 }'); do
    f=$DIR/$name
    if [ ! -f "$f" ]; then
        echo "FAIL: font $name missing from $DIR"
        fail=1
        continue
    fi
    if [ -f "$SRCDIR/$name" ] && ! cmp -s "$f" "$SRCDIR/$name"; then
        echo "FAIL: installed $name differs from $SRCDIR/$name (stale install?)"
        fail=1
    fi
    found="$found $f"
done

if [ -z "$found" ] && [ $fail = 0 ]; then
    echo "FAIL: $MAN lists no fonts"
    exit 1
fi
# shellcheck disable=SC2086
if [ -n "$found" ]; then
    scripts/fntinfo.py $found || fail=1
fi
[ $fail = 0 ] && echo "ok   fonts ($(echo $found | wc -w | tr -d ' ') installed in $DIR)"
exit $fail
