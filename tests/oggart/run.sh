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

# Ogg embedded art (#120): a METADATA_BLOCK_PICTURE comment whose comment
# header spans Ogg pages, in Vorbis and Opus (tests/fixtures/oggart/, from
# tests/fixtures/make_ogg_art_fixtures.py). Builds tests/oggart/oggart.c
# against build/warble's librbcodec.a (the real metadata parser) and the
# firmware's JPEG decoder (apps/recorder/jpeg_load.c, compiled twice: the
# stock 16-byte reads and the shell's 64 KiB bulk reads), then per fixture:
#  1. the art record: JPEG, base64, and albumart.pos/size equal to what an
#     independent Python walk of the Ogg pages gives (pos: the file offset
#     of the image data's first base64 group, counted through the pages;
#     size: the file span from there to the comment value's end);
#  2. the picture streamed back through ogg_file_open_art()/ogg_file_read()
#     and base64_decode() (4096- and 12-character reads) starts with
#     exactly the original JPEG's bytes (taken out of the comment by Python);
#  3. clip_jpeg_fd() at albumart.pos with the art's flags (what the art
#     builder and buffering.c call) decodes to the same pixels as the plain
#     original JPEG.
#   WARBLEDIR  a built warble (default build/warble; scripts/test.sh builds it)
#   HOSTCC     default gcc-16
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WARBLEDIR=${WARBLEDIR:-$ROOT/build/warble}
HOSTCC=${HOSTCC:-gcc-16}
FIX=$ROOT/tests/fixtures/oggart
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ -f "$WARBLEDIR/lib/librbcodec.a" ] || { echo "FAIL: no warble build at $WARBLEDIR"; exit 1; }
printf 'printcf:\n\t@echo CF: $(CFLAGS)\n' > "$TMP/print.mk"
FL=$(make --no-print-directory -C "$WARBLEDIR" -f Makefile -f "$TMP/print.mk" printcf \
     | sed -n 's/^CF: //p' | sed -E -e 's#-DTARGET_NAME=[^ ]*##' -e 's#-DCODECDIR=[^ ]*##')
[ -n "$FL" ] || { echo "FAIL: no CFLAGS from $WARBLEDIR"; exit 1; }
echo 'void yield(void);' > "$TMP/yield.h"
W=$WARBLEDIR
# shellcheck disable=SC2086
{
    eval "$HOSTCC" $FL -I"$ROOT/apps/recorder" -Wall -Werror -c "$ROOT/tests/oggart/oggart.c" -o "$TMP/oggart.o"
    for f in resize bmp; do
        eval "$HOSTCC" $FL -I"$ROOT/apps/recorder" -w -c "$ROOT/apps/recorder/$f.c" -o "$TMP/$f.o"
    done
    for v in small bulk; do
        X=; [ $v = bulk ] && X=-DHAVE_IPOD_UI
        eval "$HOSTCC" $FL $X -I"$ROOT/apps/recorder" -w -include "$TMP/yield.h" \
            -c "$ROOT/apps/recorder/jpeg_load.c" -o "$TMP/jpeg-$v.o"
        "$HOSTCC" -o "$TMP/oggart-$v" "$TMP/oggart.o" "$TMP/jpeg-$v.o" "$TMP/resize.o" \
            "$TMP/bmp.o" "$W/firmware/common/strmemccpy.o" "$W/firmware/common/strlcpy.o" \
            "$W/firmware/common/unicode.o" "$W/firmware/common/pathfuncs.o" \
            "$W/lib/librbcodec.a" "$W/lib/libfixedpoint.a" 2>"$TMP/ld.log" \
            || { cat "$TMP/ld.log"; exit 1; }
    done
}

# The reference: the original JPEG and the expected art record, from an
# independent walk of the Ogg pages (writes the JPEG to $2, prints "pos size")
ref() {
    python3 - "$1" "$2" <<'EOF'
import base64, struct, sys
data = open(sys.argv[1], 'rb').read()
# the page bodies as one stream, with each byte's file offset
offs, body, p = [], bytearray(), 0
while p < len(data):
    assert data[p:p + 4] == b'OggS'
    n = data[p + 26]
    size = sum(data[p + 27:p + 27 + n])
    b = p + 27 + n
    offs.extend(range(b, b + size))
    body += data[b:b + size]
    p = b + size
key = b'METADATA_BLOCK_PICTURE='
k = body.index(key)
v = k + len(key)
vlen = struct.unpack('<I', body[k - 4:k])[0] - len(key)
blk = base64.b64decode(bytes(body[v:v + vlen]))
mlen = struct.unpack('>I', blk[4:8])[0]
dlen = struct.unpack('>I', blk[8 + mlen:12 + mlen])[0]
d = 12 + mlen + dlen + 16            # the image length field
jpg = blk[d + 4:d + 4 + struct.unpack('>I', blk[d:d + 4])[0]]
open(sys.argv[2], 'wb').write(jpg)
skip = ((d + 4 + 2) // 3) * 4        # base64_encoded_size(): whole groups
# skipping through the pages stops right after the last skipped byte (at
# a page end that is the next page header, read with the next byte)
pos = offs[v + skip - 1] + 1
end = offs[v + vlen - 1] + 1
print(pos, end - pos)
EOF
}

fail=0
for f in "$FIX"/*.ogg "$FIX"/*.opus; do
    n=$(basename "$f")
    set -- $(ref "$f" "$TMP/$n.jpg")
    epos=$1 esize=$2
    jl=$(wc -c < "$TMP/$n.jpg" | tr -d ' ')
    rec=$("$TMP/oggart-small" "$f" "$TMP/$n.b64" 4096 | head -1)
    want="art: embedded=1 type=3 base64=1 pos=$epos size=$esize"
    if [ "$rec" = "$want" ]; then
        echo "ok   $n: $rec"
    else
        echo "FAIL: $n: got '$rec', want '$want'"; fail=1
    fi
    for c in 4096 12; do
        "$TMP/oggart-small" "$f" "$TMP/$n.$c.bin" $c >/dev/null || { echo "FAIL: $n: stream ($c)"; fail=1; continue; }
        if head -c "$jl" "$TMP/$n.$c.bin" | cmp -s - "$TMP/$n.jpg"; then
            echo "ok   $n: picture bytes identical ($jl B, $c-char reads)"
        else
            echo "FAIL: $n: picture bytes differ ($c-char reads)"; fail=1
        fi
    done
    for v in small bulk; do
        if "$TMP/oggart-$v" -j "$f" "$TMP/$n.$v.raw" >/dev/null \
           && "$TMP/oggart-$v" -j x "$TMP/$n.ref.raw" "$TMP/$n.jpg" >/dev/null \
           && cmp -s "$TMP/$n.$v.raw" "$TMP/$n.ref.raw"; then
            echo "ok   $n: JPEG decode ($v reads) = the original's pixels"
        else
            echo "FAIL: $n: JPEG decode ($v reads)"; fail=1
        fi
    done
done
exit $fail
