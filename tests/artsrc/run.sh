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

# tests/artsrc/run.sh: the art builder's source choice (#332 follow-up).
# The real source_decode(), source_sig() and source_unchanged() of
# apps/ipodui/artcache.c, with the lookups they call (find_file(),
# synced_track(), syn_find(), png_folder(), poster_file()), extracted as
# they are and run over the real search_albumart_files() (apps/recorder/
# albumart.c, with apps/misc.c's two path helpers, against
# tests/walkcache/artshim) with every file lookup counted (artsrc.c).
# Each track kind under each album_art setting: the sources tried and the
# result follow the stock order (a second statement of it in artsrc.c);
# no image-file lookups where embedded art comes first and the track has
# some (until its decode fails), exactly one search's lookups otherwise.
# Built without and with HAVE_BCMFW_VIDEO (a video's own art first).
#   tests/artsrc/run.sh
#   ARTSRC_FROM=<artcache.c> tests/artsrc/run.sh   (another version's)
# Needs only cc. No device is touched.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${ARTSRC_OUT:-$top/build/artsrc}
src=${ARTSRC_FROM:-$top/apps/ipodui/artcache.c}
fns="synced_track poster_file find_file syn_find source_unchanged
     source_sig png_folder source_decode"
rm -rf "$out"
mkdir -p "$out/inc"
for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$top/$d"/*.h "$out/inc/"
done
cp "$top"/tests/fatcrash/shim/*.h "$out/inc/"
CFLAGS="-std=gnu99 -O2 -w -I$out/inc -I$top/tests/fatcrash"
ARTFLAGS="-I$top/tests/walkcache/artshim -DHAVE_ALBUMART -DHAVE_JPEG"
{
    echo '#include <string.h>'
    echo '#include <sys/types.h>'
    echo '#include "misc.h"'
    awk '/^void fix_path_part\(/,/^}/' "$top/apps/misc.c"
    awk '/^char \*strip_extension\(/,/^}/' "$top/apps/misc.c"
} >"$out/artmisc.c"
# the functions under test, as they are, in the order they need
for f in $fns; do
    [ "$f" = poster_file ] && echo '#ifdef HAVE_BCMFW_VIDEO'
    awk "/^static [a-z]+ $f\\(/,/^}/" "$src"
    [ "$f" = poster_file ] && echo '#endif'
    echo
done >"$out/extracted.c"
for f in $fns; do
    grep -q "^static [a-z]* $f(" "$out/extracted.c" || {
        echo "artsrc: $f not found in $src"; exit 1; }
done
rc=0
for v in plain video; do
    vf=
    [ $v = video ] && vf=-DHAVE_BCMFW_VIDEO
    o=$out/obj-$v
    mkdir -p "$o"
    # shellcheck disable=SC2086
    cc $CFLAGS $ARTFLAGS $vf -c -o "$o/albumart.o" \
       "$top/apps/recorder/albumart.c"
    # shellcheck disable=SC2086
    cc $CFLAGS $ARTFLAGS -c -o "$o/artmisc.o" "$out/artmisc.c"
    # shellcheck disable=SC2086
    cc $CFLAGS -c -o "$o/strmemccpy.o" "$top/firmware/common/strmemccpy.c"
    # shellcheck disable=SC2086
    cc $CFLAGS $ARTFLAGS $vf -I"$out" -c -o "$o/artsrc.o" "$here/artsrc.c"
    cc -o "$out/artsrc_$v" "$o"/*.o
    echo "== artsrc ($v)"
    "$out/artsrc_$v" >"$out/$v.txt" 2>&1 || rc=1
    grep -B1 "FAIL" "$out/$v.txt" | head -20 || true
    lines=$(grep -c "^decode \|^sig \|^unchanged " "$out/$v.txt" || true)
    zero=$(grep -c "^decode .* aa=emb .*probes=0$" "$out/$v.txt" || true)
    tail -1 "$out/$v.txt"
    echo "  $lines results; prefer-embedded decodes with no lookups: $zero"
    # a run that checked nothing must not pass
    grep -q "^artsrc: [1-9][0-9]* track x setting cases, 0 failures" \
        "$out/$v.txt" || rc=1
    [ "$lines" -ge 100 ] || { echo "  too few results"; rc=1; }
done
if [ $rc = 0 ]; then echo "artsrc: PASS"; else echo "artsrc: FAIL"; fi
exit $rc
