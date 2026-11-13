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

# tests/walkcache/run.sh: the walk lookup cache (#243) on the firmware's
# file layer over a RAM FAT32 image (walkcache.c, with tests/fatcrash's
# volume model dirburst.c and its shim).
#   tests/walkcache/run.sh [tracks...]     (default: 2000 10000)
# Controls (WALKCACHE_BREAK mask): 1 creates, 2 removes, 4 mounts don't
# invalidate; 8 never a hit; 16 no LRU; 32 the name ignored; 64 a listed
# folder kept with the wrong position.
# 1. the unit cases with the cache, on the 5.5G's geometry (16 KiB
#    clusters, 2048-byte BPB sectors) and on 512-byte sectors: all ok;
# 2. each negative control (WALKCACHE_BREAK) fails the case that guards it,
#    and a cache that ignores names (32) changes the library's results;
# 3. per library size: the scan's results and the opens with the cache
#    and without it (-DFILE_NO_WALK_CACHE) are identical, line for line;
#    the directory entries decoded and the host time of each are printed.
# 4. the art builder's cover lookups (#332): the real
#    search_albumart_files() (apps/recorder/albumart.c with misc.c's
#    fix_path_part() and strip_extension(), against artshim/) for every
#    album of a library volume with every candidate kind, the cache held
#    across the pass: each album's expected file found, the results with
#    and without the cache identical, and fewer entries decoded with it;
#    a cache that ignores names (32) must change them.
# Needs only cc. No device is touched.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${WALKCACHE_OUT:-$top/build/walkcache}
mkdir -p "$out/inc"
for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$top/$d"/*.h "$out/inc/"
done
cp "$top"/tests/fatcrash/shim/*.h "$out/inc/"
fs=$top/firmware/common
CFLAGS="-std=gnu99 -O2 -w -I$out/inc -I$top/tests/fatcrash"
# art mode: apps/misc.c's two path helpers, as they are
{
    echo '#include <string.h>'
    echo '#include <sys/types.h>'
    echo '#include "misc.h"'
    awk '/^void fix_path_part\(/,/^}/' "$top/apps/misc.c"
    awk '/^char \*strip_extension\(/,/^}/' "$top/apps/misc.c"
} >"$out/artmisc.c"
grep -q fix_path_part "$out/artmisc.c" && grep -q strip_extension \
    "$out/artmisc.c" || { echo "artmisc.c: helpers not found"; exit 1; }
ARTFLAGS="-I$here/artshim -DHAVE_ALBUMART -DHAVE_JPEG"

# build <name> <extra cflags>: file_internal.c counts every entry decoded
build() {
    name=$1; shift
    o=$out/obj-$name
    rm -rf "$o"
    mkdir -p "$o"
    for f in file.c fileobj_mgr.c dir.c pathfuncs.c rb_namespace.c fat.c \
             disk_cache.c linked_list.c; do
        # shellcheck disable=SC2086
        cc $CFLAGS "$@" -c -o "$o/${f%.c}.o" "$fs/$f"
    done
    # shellcheck disable=SC2086
    cc $CFLAGS "$@" -DWALK_CACHE_STATS -Dfat_readdir=wc_fat_readdir -c \
       -o "$o/file_internal.o" "$fs/file_internal.c"
    # shellcheck disable=SC2086
    cc $CFLAGS $ARTFLAGS "$@" -c -o "$o/walkcache.o" "$here/walkcache.c"
    # shellcheck disable=SC2086
    cc $CFLAGS $ARTFLAGS "$@" -c -o "$o/albumart.o" \
       "$top/apps/recorder/albumart.c"
    # shellcheck disable=SC2086
    cc $CFLAGS $ARTFLAGS "$@" -c -o "$o/artmisc.o" "$out/artmisc.c"
    # shellcheck disable=SC2086
    cc $CFLAGS "$@" -c -o "$o/strmemccpy.o" "$fs/strmemccpy.c"
    cc -o "$out/walkcache_$name" "$o"/*.o
}

build on
build off -DFILE_NO_WALK_CACHE
for m in 1 2 3 4 8 16 32 64; do
    build "break$m" -DWALKCACHE_BREAK=$m
done

rc=0
echo "== unit"
for geo in "32 2048" "8 512"; do
    # shellcheck disable=SC2086
    "$out/walkcache_on" unit $geo || rc=1
done

echo "== negative controls (each must fail its case)"
for pair in 8:hits 2:rmdir 3:rename 3:create 4:usb 16:evict 64:listed; do
    m=${pair%%:*}
    c=${pair#*:}
    "$out/walkcache_break$m" unit 32 2048 >"$out/break$m.txt" 2>&1 || true
    if grep -q "^$c: FAIL" "$out/break$m.txt"; then
        echo "WALKCACHE_BREAK=$m: $c FAILS (red, as it must):" \
             "$(grep "^$c: FAIL" "$out/break$m.txt" | head -1)"
    else
        echo "WALKCACHE_BREAK=$m: $c did NOT fail"
        rc=1
    fi
done
# misses are never kept, so a create alone can't stale a slot: the
# create bump (mask 1) by itself breaks no case
if "$out/walkcache_break1" unit 32 2048 >"$out/break1.txt" 2>&1; then
    echo "WALKCACHE_BREAK=1 alone: all ok (no case depends on it alone)"
else
    echo "WALKCACHE_BREAK=1 alone: $(grep -c FAIL "$out/break1.txt") checks fail"
fi

# the library comparison below must see a wrong cache
"$out/walkcache_break32" lib 300 32 2048 2>/dev/null | grep -v "^stat" \
    >"$out/a.txt" || true
"$out/walkcache_off" lib 300 32 2048 2>/dev/null | grep -v "^stat" \
    >"$out/b.txt" || true
if cmp -s "$out/a.txt" "$out/b.txt"; then
    echo "WALKCACHE_BREAK=32: library results did NOT change"
    rc=1
else
    echo "WALKCACHE_BREAK=32: library results differ (red, as they must)"
fi

[ $# -gt 0 ] || set -- 2000 10000
for n in "$@"; do
    echo "== lib $n (16 KiB clusters, 2048-byte sectors)"
    "$out/walkcache_on" lib "$n" 32 2048 >"$out/lib-on-$n.txt" \
        2>"$out/lib-on-$n.err" || rc=1
    "$out/walkcache_off" lib "$n" 32 2048 >"$out/lib-off-$n.txt" \
        2>"$out/lib-off-$n.err" || rc=1
    grep -v "^stat" "$out/lib-on-$n.txt" >"$out/a.txt" || true
    grep -v "^stat" "$out/lib-off-$n.txt" >"$out/b.txt" || true
    if [ "$(grep -c "^scan:" "$out/a.txt")" != 1 ] \
       || ! cmp -s "$out/a.txt" "$out/b.txt"; then
        echo "lib $n: results DIFFER with the cache (or no scan ran)"
        diff "$out/a.txt" "$out/b.txt" | head
        rc=1
    else
        echo "lib $n: identical, $(grep -c . "$out/a.txt") lines" \
             "($(grep "^scan:" "$out/a.txt"))"
    fi
    echo "  cache:    $(grep "^stat" "$out/lib-on-$n.txt" | tr '\n' ' ')"
    echo "  no cache: $(grep "^stat" "$out/lib-off-$n.txt" | tr '\n' ' ')"
    sed 's/^/  cache: /' "$out/lib-on-$n.err"
done
# the art comparison below must see a wrong cache
"$out/walkcache_break32" art 400 32 2048 2>/dev/null | grep -v "^stat" \
    >"$out/a.txt" || true
"$out/walkcache_off" art 400 32 2048 2>/dev/null | grep -v "^stat" \
    >"$out/b.txt" || true
if cmp -s "$out/a.txt" "$out/b.txt"; then
    echo "WALKCACHE_BREAK=32: art results did NOT change"
    rc=1
else
    echo "WALKCACHE_BREAK=32: art results differ (red, as they must)"
fi

[ -n "$WALKCACHE_ART" ] || WALKCACHE_ART="1000 6500"
for n in $WALKCACHE_ART; do
    echo "== art $n albums (16 KiB clusters, 2048-byte sectors)"
    "$out/walkcache_on" art "$n" 32 2048 >"$out/art-on-$n.txt" \
        2>"$out/art-on-$n.err" || rc=1
    "$out/walkcache_off" art "$n" 32 2048 >"$out/art-off-$n.txt" \
        2>"$out/art-off-$n.err" || rc=1
    grep "FAIL" "$out/art-on-$n.txt" "$out/art-off-$n.txt" | head -5
    grep -v "^stat" "$out/art-on-$n.txt" >"$out/a.txt" || true
    grep -v "^stat" "$out/art-off-$n.txt" >"$out/b.txt" || true
    if ! grep -q "^art: $n albums [1-9][0-9]* found 0 wrong" "$out/a.txt" \
       || [ "$(grep -c "^art kind .*: [1-9][0-9]* of" "$out/a.txt")" != 12 ] \
       || ! cmp -s "$out/a.txt" "$out/b.txt"; then
        echo "art $n: wrong answers, a kind never found, or results DIFFER"
        diff "$out/a.txt" "$out/b.txt" | head
        rc=1
    else
        echo "art $n: identical, every kind found ($(grep "^art:" \
             "$out/a.txt"))"
    fi
    on=$(sed -n 's/^stat art_decoded \([0-9]*\).*/\1/p' "$out/art-on-$n.txt")
    off=$(sed -n 's/^stat art_decoded \([0-9]*\).*/\1/p' "$out/art-off-$n.txt")
    if [ -z "$on" ] || [ -z "$off" ] || [ "$on" -ge "$off" ]; then
        echo "art $n: the cache decoded no fewer entries ($on vs $off)"
        rc=1
    fi
    echo "  cache:    $(grep "^stat" "$out/art-on-$n.txt")"
    echo "  no cache: $(grep "^stat" "$out/art-off-$n.txt")"
    sed 's/^/  cache: /' "$out/art-on-$n.err"
done
if [ $rc = 0 ]; then echo "walkcache: PASS"; else echo "walkcache: FAIL"; fi
exit $rc
