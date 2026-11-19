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

# make_lng_fixtures.sh BUILD [OUT]: .lng files that BUILD's firmware must
# refuse (apps/language.c, the string-table check), under OUT (default
# build/lng-fixtures), made fresh every run with BUILD's own target id,
# features and tools:
#   deutsch-shifted.lng  German, numbered against an english.lang with one
#                        phrase more at the top (every ID one higher): what
#                        a .lng left over from another build looks like
#   deutsch-v6.lng       German in the old header (no table hash): genlang
#                        run without the "ipodui" feature
# The build's own apps/lang/deutsch.lng is the matching file.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD=${1:?usage: make_lng_fixtures.sh BUILD [OUT]}
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
OUT=${2:-"$ROOT/build/lng-fixtures"}
MODEL=$(sed -n 's/^export MODELNAME=//p' "$BUILD/Makefile")
TID=$(sed -n 's/^export TARGET_ID=//p' "$BUILD/Makefile")
FEAT=$(cat "$BUILD/apps/genlang-features")
[ -n "$MODEL" ] && [ -n "$TID" ] && [ -n "$FEAT" ] \
    || { echo "make_lng_fixtures: $BUILD is not a configured build" >&2; exit 2; }
case ":$FEAT" in *:ipodui:*) ;; *)
    echo "make_lng_fixtures: $BUILD is not a shell build" >&2; exit 2 ;; esac

mkdir -p "$OUT"
work=$(mktemp -d "$OUT/.work.XXXXXX")
trap 'rm -rf "$work"' EXIT
LANG_DIR="$ROOT/apps/lang"

# genlang -b caches english.lang's numbering in english.list beside the
# output: each run below gets its own directory
mkdir "$work/shift" "$work/v6"
python3 "$ROOT/tests/fixtures/lng/lang_shift.py" "$LANG_DIR/english.lang" \
    "$work/english-shifted.lang"
"$ROOT/tools/updatelang" "$work/english-shifted.lang" \
    "$LANG_DIR/deutsch.lang" "$work/deutsch-shifted.lang" >/dev/null 2>&1
"$ROOT/tools/genlang" -e="$work/english-shifted.lang" -t="$MODEL:$FEAT" \
    -i="$TID" -b="$work/shift/deutsch-shifted.lng" "$work/deutsch-shifted.lang"

FEAT_V6=$(printf '%s' "$FEAT" | tr : '\n' | grep -v '^ipodui$' | tr '\n' :)
"$ROOT/tools/updatelang" "$LANG_DIR/english.lang" "$LANG_DIR/deutsch.lang" \
    "$work/deutsch.lang" >/dev/null 2>&1
"$ROOT/tools/genlang" -e="$LANG_DIR/english.lang" -t="$MODEL:$FEAT_V6" \
    -i="$TID" -b="$work/v6/deutsch-v6.lng" "$work/deutsch.lang"

for f in shift/deutsch-shifted.lng v6/deutsch-v6.lng; do
    [ "$(wc -c < "$work/$f")" -gt 1000 ] \
        || { echo "make_lng_fixtures: $f is empty" >&2; exit 2; }
    mv "$work/$f" "$OUT/$(basename "$f")"
done
echo "lng fixtures: $OUT (target $TID)"
