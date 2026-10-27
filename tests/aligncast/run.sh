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

# tests/aligncast: static alignment check (docs/process.md, "Alignment").
# The PP5022 data-aborts on a misaligned halfword/word access (quirks.md #51)
# and the host accepts it, so the risky shape is flagged here: a cast to a
# 16/32-bit pointer of an expression that adds a byte offset to a base, e.g.
#     (uint32_t *)(buf + off)
# in apps/ipodui and firmware/target/arm/ipod/video. Each such site must keep
# the offset a multiple of the type's size (a layout constant that is a
# multiple, or an aligned allocation) or go through memcpy. A reviewed site
# carries "align-ok: <why>" in a comment on its line; the rest are counted
# per file against tests/aligncast/baseline.txt (sites present on
# 2026-10-25, covered where exercised by the UBSan sims, not individually
# audited). A file over its baseline, or a new file, fails with the lines.
# After reviewing/removing sites, lower the baseline:  run.sh --update
# No device is touched.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=tests/aligncast/baseline.txt
TY='(u?int(16|32)_t|unsigned (short|int|long)|short|int|long|fb_data)'
# '(T *)(... + ...' or '(T *)ident + ...' or '(T *)(ident->m + n'
RE="\\((const |volatile )*$TY( const)? ?\\*\\) ?(\\(+[^;]*[^=!<>]\\+[^+=]|[A-Za-z_][A-Za-z_0-9>.-]* ?\\+[^+=])"
scan() {
    grep -rnE "$RE" apps/ipodui firmware/target/arm/ipod/video \
        --include='*.c' --include='*.h' | grep -v 'apps/ipodui/fonts/' \
        | grep -v 'align-ok:' | sort
}
cur=$(scan)
if [ "${1:-}" = --update ]; then
    echo "$cur" | cut -d: -f1 | sort | uniq -c | awk '{print $2, $1}' >"$B"
    echo "updated $B"; exit 0
fi
fail=0
files=$(echo "$cur" | cut -d: -f1 | sort -u | grep .)
for f in $files; do
    n=$(echo "$cur" | grep -c "^$f:")
    base=$(awk -v f="$f" '$1==f{print $2}' "$B" 2>/dev/null)
    base=${base:-0}
    if [ "$n" -gt "$base" ]; then
        fail=1
        echo "FAIL: alignment cast rule, $f has $n byte-offset casts to 16/32-bit pointers, baseline $base"
        echo "      (PP5022 aborts on a misaligned access, quirks.md #51: keep the offset a multiple of the"
        echo "       type size and say so with 'align-ok: <why>' on the line, or use memcpy)"
        echo "$cur" | grep "^$f:" | cut -c1-170 | sed 's/^/      /'
    fi
done
[ $fail = 0 ] || exit 1
echo "ok   alignment casts: $(echo "$cur" | grep -c .) byte-offset casts to 16/32-bit pointers, none over the per-file baseline"
