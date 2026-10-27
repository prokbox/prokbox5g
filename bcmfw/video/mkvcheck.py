#!/usr/bin/env python3
"""The first-use video check's test stream (docs/research/bcm2722/
first-use-check.md): about 30 pictures of ffmpeg's moving test pattern
(testsrc2), 320x240, H.264 Constrained Baseline (x264, one thread, the
bit-exact flags: the same bytes for the same x264), packed one picture a
packet for the player, with each picture's sum.

Entirely synthetic: never a frame of anyone's media.

Outputs (both committed):
  bcmfw/video/vcheck.bin        the packed stream; buildzip.pl puts it in
                                the hidden folder as vcheck.bin in every
                                build with the video player
  apps/video/vcheck_stream.h    its size and FNV-1a, its geometry, and
                                each picture's sum (vcheck.h
                                vcheck_sample_row: FNV-1a over a sample of
                                the Y, U and V rows, one of each plane a
                                macroblock row, as little-endian 32-bit
                                words)

The sums come from ffmpeg's decode here; tests/video/test_vcheck.c decodes
the packed file with our own decoder (bcmfw/h264) and checks every one, so
two independent decoders agree on the table.

The file (little-endian): "VCK1", width (16 bits), height (16 bits),
pictures, the frame time in us, then one length a picture, then the
packets, each Annex B with 4-byte start codes and padded to a multiple of
4 bytes.  The first packet carries the SPS and PPS; SEI and AUD units are
left out.

Usage: bcmfw/video/mkvcheck.py [--check]
  --check   regenerate into a temporary place and compare with the
            committed files (exit 1 if they differ: another x264 gives
            other bytes; then regenerate and commit both together)
"""
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
# The first-use video check's test stream and its sums.
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
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BIN = os.path.join(HERE, 'vcheck.bin')
HDR = os.path.join(ROOT, 'apps', 'video', 'vcheck_stream.h')

W, H, PICS, FPS, QP = 320, 240, 30, 30, 38
MAGIC = 0x314B4356          # "VCK1"


def encode(out):
    subprocess.check_call([
        'ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        'testsrc2=size=%dx%d:rate=%d:duration=2' % (W, H, FPS),
        '-frames:v', str(PICS), '-c:v', 'libx264', '-profile:v', 'baseline',
        '-preset', 'medium', '-threads', '1', '-qp', str(QP),
        '-g', str(PICS), '-bf', '0', '-refs', '1', '-pix_fmt', 'yuv420p',
        '-fflags', '+bitexact', '-flags:v', '+bitexact', '-f', 'h264', out])


def nals(data):
    """(type, bytes without start code) for each NAL unit"""
    starts = []
    i = 0
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    out = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        while e > s and data[e - 1] == 0:      # trailing zeros, 4-byte codes
            e -= 1
        out.append((data[s] & 31, data[s:e]))
    return out


def packets(data):
    pics, pending = [], b''
    for t, n in nals(data):
        code = b'\x00\x00\x00\x01' + n
        if t in (7, 8):
            pending += code
        elif t in (1, 5):
            # one slice a picture (x264's default): first_mb_in_slice 0
            if not (n[1] & 0x80):
                raise SystemExit('mkvcheck: more than one slice a picture')
            pics.append(pending + code)
            pending = b''
        # SEI (6), AUD (9) and anything else: left out
    if len(pics) != PICS:
        raise SystemExit('mkvcheck: %d pictures, want %d' % (len(pics), PICS))
    return pics


def fnv_words(h, b):
    for (w,) in struct.iter_unpack('<I', b):
        h = ((h ^ w) * 0x01000193) & 0xFFFFFFFF
    return h


def sample_row(plane, mbrow, pic):
    """vcheck.h vcheck_sample_row: the row of a plane a picture's sum
    takes in a macroblock row"""
    if plane:
        return mbrow * 8 + ((3 * mbrow + 5 * pic + 4 * (plane - 1)) & 7)
    return mbrow * 16 + ((7 * mbrow + 5 * pic) & 15)


def pic_sum(pic, k):
    """vcheck.c pic_sum: FNV-1a over the sampled rows' 32-bit words, Y's
    rows, then U's, then V's"""
    cw, ch = W // 2, H // 2
    planes = [(pic[:W * H], W),
              (pic[W * H:W * H + cw * ch], cw),
              (pic[W * H + cw * ch:], cw)]
    h = 0x811C9DC5
    for pl, (data, width) in enumerate(planes):
        for m in range(H // 16):
            r = sample_row(pl, m, k)
            h = fnv_words(h, data[r * width:(r + 1) * width])
    return h


def pic_sums(yuv):
    size = W * H * 3 // 2
    if len(yuv) != size * PICS:
        raise SystemExit('mkvcheck: decoded %d bytes, want %d' %
                         (len(yuv), size * PICS))
    return [pic_sum(yuv[k * size:(k + 1) * size], k) for k in range(PICS)]


def fnv_bytes(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def build(tmp):
    raw = os.path.join(tmp, 'vcheck.264')
    encode(raw)
    with open(raw, 'rb') as f:
        pics = packets(f.read())
    # the sums from ffmpeg's decode of exactly the bytes we ship
    annexb = os.path.join(tmp, 'packed.264')
    with open(annexb, 'wb') as f:
        f.write(b''.join(pics))
    yuv = subprocess.check_output([
        'ffmpeg', '-v', 'error', '-i', annexb, '-f', 'rawvideo',
        '-pix_fmt', 'yuv420p', '-'])
    sums = pic_sums(yuv)
    body = struct.pack('<IHHII', MAGIC, W, H, PICS, 1000000 // FPS)
    body += b''.join(struct.pack('<I', len(p)) for p in pics)
    for p in pics:
        body += p + b'\0' * (-len(p) % 4)
    return body, sums


def header_text(body, sums):
    import prokbox_header  # noqa: E402
    lines = [
        '/* Generated by bcmfw/video/mkvcheck.py: do not edit.  The first-use video',
        ' * check\'s test stream (bcmfw/video/vcheck.bin, shipped as',
        ' * vcheck.bin in the hidden folder): its size and FNV-1a, which the',
        ' * player checks before it trusts the file, and each picture\'s sum',
        ' * over the rows vcheck.h vcheck_sample_row names). */',
        '#ifndef VCHECK_STREAM_H',
        '#define VCHECK_STREAM_H',
        '#include <stdint.h>',
        '#define VCHECK_FILE_BYTES %du' % len(body),
        '#define VCHECK_FILE_FNV 0x%08xu' % fnv_bytes(body),
        '#define VCHECK_WIDTH %d' % W,
        '#define VCHECK_HEIGHT %d' % H,
        '#define VCHECK_PICS %d' % PICS,
        'static const uint32_t vcheck_sums[VCHECK_PICS] =',
        '{',
    ]
    for k in range(0, len(sums), 4):
        lines.append('    ' + ' '.join('0x%08xu,' % s for s in sums[k:k + 4]))
    lines += ['};', '#endif', '']
    return prokbox_header.convert('\n'.join(lines), '.h')[0]


def main(argv):
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    with tempfile.TemporaryDirectory() as tmp:
        body, sums = build(tmp)
    text = header_text(body, sums)
    if '--check' in argv:
        same = (open(BIN, 'rb').read() == body and
                open(HDR).read() == text)
        print('mkvcheck: %s' % ('same' if same else
                                'DIFFERS (another x264? regenerate both)'))
        return 0 if same else 1
    with open(BIN, 'wb') as f:
        f.write(body)
    with open(HDR, 'w') as f:
        f.write(text)
    print('mkvcheck: %s %d bytes, %d pictures, fnv %08x' %
          (os.path.relpath(BIN, ROOT), len(body), PICS, fnv_bytes(body)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
