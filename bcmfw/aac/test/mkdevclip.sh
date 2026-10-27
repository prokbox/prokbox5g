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
# The files for the device's stage 9 (AAC on the chip, audio-spike.md M4):
# the first AUS access units of one of bcmfw/aac/test's synthetic clips
# (mkclips.sh; no owner media), and the host build's answer for them.
#
#   aac9.au   'AACU', u32 ASC length, the ASC, then per AU: u32 length,
#             the bytes (adts2au.py's format, cut to AUS)
#   aac9.ref  'AACR', u32 AUS, then per AU: u32 frames out, u32 FNV-1a of
#             that AU's 16-bit PCM; then u32 FNV-1a of all of it
#
# The owner copies both into the iPod's system folder, bcmtest/.
# Generated, never committed.
#
#   bcmfw/aac/test/mkdevclip.sh [CLIP] [AUS] [OUTDIR]
#   (default chirp_at128, 200 AUs (4.6 s), build/aac/device)
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
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
TEST="$ROOT/bcmfw/aac/test"
OUT="$ROOT/build/aac"
CLIP=${1:-chirp_at128}
AUS=${2:-200}
DEST=${3:-$OUT/device}
[ -f "$OUT/clips/$CLIP.au" ] || "$TEST/mkclips.sh"
[ -f "$OUT/$CLIP.raw" ] || "$TEST/run.sh" --host >/dev/null
mkdir -p "$DEST"
python3 -I - "$OUT/clips/$CLIP.au" "$OUT/$CLIP.raw" "$OUT/$CLIP.s16" "$AUS" \
    "$DEST/aac9.au" "$DEST/aac9.ref" <<'EOF'
import struct
import sys

au_in, raw_in, pcm_in, aus, au_out, ref_out = sys.argv[1:]
aus = int(aus)
data = open(au_in, 'rb').read()
assert data[:4] == b'AACU'
pos = 8 + struct.unpack_from('<I', data, 4)[0]
cut = pos
for k in range(aus):
    if cut >= len(data):
        sys.exit('mkdevclip: %s has only %d AUs' % (au_in, k))
    cut += 4 + struct.unpack_from('<I', data, cut)[0]
raw = open(raw_in, 'rb').read()
pcm = open(pcm_in, 'rb').read()


def fnv(h, b):
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


rec, p, total, out = 4 + 8192, 0, 2166136261, [b'AACR', struct.pack('<I', aus)]
for k in range(aus):
    n = struct.unpack_from('<I', raw, k * rec)[0]
    b = pcm[p:p + 4 * n]
    p += 4 * n
    out.append(struct.pack('<II', n, fnv(2166136261, b)))
    total = fnv(total, b)
out.append(struct.pack('<I', total))
open(au_out, 'wb').write(data[:cut])
open(ref_out, 'wb').write(b''.join(out))
print('%s: %d AUs (%d bytes), %d frames; PCM fnv %08x' % (
    au_out, aus, cut, p // 4, total))
print('%s: the host reference' % ref_out)
EOF
echo "copy $DEST/aac9.au and $DEST/aac9.ref to the iPod's system folder, bcmtest/"
