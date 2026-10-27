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

# The JPEG spike's corpus (docs/research/bcm2722/jpeg-spike.md section 5):
# generated pictures only, never anyone's covers.  ffmpeg draws the
# sources (testsrc2, smptebars, noise) as PPM; libjpeg-turbo's cjpeg
# encodes them baseline at 4:2:0, 4:2:2, 4:4:4 and grey, qualities 50, 85
# and 95, with and without restart intervals, at the sizes the art builder
# meets; plus a progressive file (refused) and two corrupt ones (a
# truncated file, a bad Huffman code).  Into build/jpeg/corpus; the
# names and sha256s are in corpus.sha256 (cjpeg's output is stable for a
# version: a different libjpeg may differ, and then the goldens are
# remade, not the decoder doubted).
# Usage: tests/jpeg/make_corpus.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/build/jpeg/corpus
SRC=$ROOT/build/jpeg/src
mkdir -p "$OUT" "$SRC"
ff() { ffmpeg -loglevel error -y "$@"; }

src() {     # name w h lavfi-source
    ff -f lavfi -i "$4=size=$2x$3" -frames:v 1 -pix_fmt rgb24 "$SRC/$1.ppm"
}
src t55 55 55 testsrc2
src t142 142 142 testsrc2
src t299 299 299 testsrc2
src t300 300 300 smptebars
src t301 301 301 testsrc2
src t600 600 600 testsrc2
src t1000 1000 1000 smptebars
src t1500 1500 1500 testsrc2
src t3000 3000 3000 testsrc2
src w640x360 640 360 testsrc2
src h333x500 333 500 smptebars
# photo-like: a picture with fine grain, the Huffman load of a real
# photograph somewhere between the smooth patterns and pure noise
ff -f lavfi -i "testsrc2=size=3000x3000,noise=alls=14:allf=t" -frames:v 1 \
   -pix_fmt rgb24 "$SRC/grain3000.ppm"
ff -f lavfi -i "nullsrc=size=500x500,geq=random(1)*255:128+64*sin(X/7):random(2)*255" \
   -frames:v 1 -pix_fmt rgb24 "$SRC/noise500.ppm"

enc() {     # out in args...
    o=$1; i=$2; shift 2
    cjpeg -baseline "$@" -outfile "$OUT/$o.jpg" "$SRC/$i.ppm"
}
for s in t55 t142 t299 t300 t301 t600 t1000 t1500 t3000 w640x360 h333x500 noise500 grain3000; do
    enc "${s}_420_q85" $s -quality 85 -sample 2x2
done
for s in t600 h333x500; do
    enc "${s}_422_q85" $s -quality 85 -sample 2x1
    enc "${s}_444_q85" $s -quality 85 -sample 1x1
    enc "${s}_grey_q85" $s -quality 85 -grayscale
    enc "${s}_420_q50" $s -quality 50 -sample 2x2
    enc "${s}_420_q95" $s -quality 95 -sample 2x2
    enc "${s}_420_rst" $s -quality 85 -sample 2x2 -restart 2
done
enc noise500_444_rst noise500 -quality 95 -sample 1x1 -restart 1
cjpeg -progressive -quality 85 -outfile "$OUT/t600_prog.jpg" "$SRC/t600.ppm"
# corrupt: cut at two thirds; a scan byte flipped (a code no table has)
n=$(wc -c < "$OUT/t600_420_q85.jpg")
head -c $((n * 2 / 3)) "$OUT/t600_420_q85.jpg" > "$OUT/t600_trunc.jpg"
python3 -I - "$OUT/t600_420_q85.jpg" "$OUT/t600_badhuff.jpg" <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
sos = d.index(b'\xff\xda')
ln = d[sos + 2] << 8 | d[sos + 3]
p = sos + 2 + ln + 400              # inside the entropy-coded data
for k in range(p, p + 8):
    if d[k] != 0xFF and d[k - 1] != 0xFF:
        d[k] = 0xFF if d[k + 1] != 0x00 else 0xFE
open(sys.argv[2], 'wb').write(bytes(d))
PY
# an EXIF-style thumbnail (a whole JPEG with its own SOS and EOI) in an
# APP1 segment before the picture: whole, and with the picture truncated
# (the thumbnail's EOI must not count as the picture's)
python3 -I - "$OUT/t55_420_q85.jpg" "$OUT/t600_420_q85.jpg" "$OUT" <<'PY'
import sys
thumb = open(sys.argv[1], 'rb').read()
pic = open(sys.argv[2], 'rb').read()
body = b'Exif\x00\x00' + thumb
app1 = b'\xff\xe1' + (len(body) + 2).to_bytes(2, 'big') + body
whole = pic[:2] + app1 + pic[2:]
open(sys.argv[3] + '/t600_exif.jpg', 'wb').write(whole)
open(sys.argv[3] + '/t600_exif_trunc.jpg', 'wb').write(whole[:len(whole) * 2 // 3])
PY
(cd "$OUT" && shasum -a 256 *.jpg) > "$ROOT/build/jpeg/corpus.sha256.new"
echo "corpus: $(ls "$OUT" | wc -l | tr -d ' ') files in $OUT"
if [ -f "$HERE/corpus.sha256" ] && ! cmp -s "$HERE/corpus.sha256" "$ROOT/build/jpeg/corpus.sha256.new"; then
    echo "corpus differs from tests/jpeg/corpus.sha256 (another cjpeg?): goldens to remake"
fi
