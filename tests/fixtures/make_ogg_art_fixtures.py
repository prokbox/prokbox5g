#!/usr/bin/env python3
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

# Regenerate the Ogg embedded-art fixtures used by tests/oggart/run.sh
# (issue #120: a METADATA_BLOCK_PICTURE comment that spans Ogg pages).
# Output: tests/fixtures/oggart/ (or DIR).
#
#   art_1page.ogg    Vorbis, a small cover: the whole comment header on one
#                    page (the case that always worked; must not change).
#   art_2page.ogg    Vorbis, a ~60 KB cover: the comment header spans two
#                    full (255-segment, libogg-sized) pages, the page break
#                    falls inside the picture's base64 data.
#   art_2page.opus   Opus, the same cover and page layout.
#   art_hdrsplit.opus  Opus, a ~20 KB cover on short (16-segment) pages: a
#                    filler comment puts the first page break inside the
#                    picture block's header (before the image data), and the
#                    image data then crosses several more pages.
#
# The audio is 0.3 s of silence from ffmpeg (native vorbis / libopus); this
# script rebuilds the comment packet and re-paginates the headers (page
# sequence numbers and CRCs recomputed). The covers are baseline JPEGs drawn
# with PIL from a fixed seed (the test takes the original back out of the
# comment with Python's own base64 decoder).
#
# Needs python3 with PIL, and ffmpeg. Usage: make_ogg_art_fixtures.py [DIR]
import base64
import os
import random
import struct
import subprocess
import sys
import tempfile

from PIL import Image

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), 'oggart')
os.makedirs(OUT, exist_ok=True)

# --- Ogg pages ------------------------------------------------------------

def _crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        t.append(r & 0xFFFFFFFF)
    return t

CRC = _crc_table()

def ogg_crc(data):
    c = 0
    for b in data:
        c = ((c << 8) ^ CRC[((c >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    return c

def read_pages(data):
    pages, p = [], 0
    while p < len(data):
        assert data[p:p + 4] == b'OggS'
        nseg = data[p + 26]
        lacing = data[p + 27:p + 27 + nseg]
        body = p + 27 + nseg
        size = sum(lacing)
        pages.append(dict(flags=data[p + 5],
                          granule=struct.unpack('<q', data[p + 6:p + 14])[0],
                          serial=struct.unpack('<I', data[p + 14:p + 18])[0],
                          lacing=bytes(lacing),
                          body=data[body:body + size]))
        p = body + size
    return pages

def packets(pages):
    out, cur = [], b''
    for pg in pages:
        off = 0
        for l in pg['lacing']:
            cur += pg['body'][off:off + l]
            off += l
            if l < 255:
                out.append(cur)
                cur = b''
    return out

def page_bytes(flags, granule, serial, seq, lacing, body):
    h = bytearray(b'OggS' + bytes([0, flags]) + struct.pack('<qIII', granule, serial, seq, 0)
                  + bytes([len(lacing)]) + bytes(lacing))
    crc = ogg_crc(bytes(h) + body)
    h[22:26] = struct.pack('<I', crc)
    return bytes(h) + body

def paginate(pkt, serial, seq, maxseg, first_room=None):
    """Pages for one header packet (granule 0), flushed at its end.
    first_room: payload bytes on the first page (a multiple of 255)."""
    segs = [255] * (len(pkt) // 255) + [len(pkt) % 255]
    pages, off, cont = [], 0, False
    first = True
    while segs:
        n = maxseg
        if first and first_room is not None:
            n = first_room // 255
        take, segs = segs[:n], segs[n:]
        body = pkt[off:off + sum(take)]
        off += sum(take)
        pages.append(page_bytes(1 if cont else 0, 0, serial, seq, take, body))
        seq += 1
        cont = True
        first = False
    return pages, seq

# --- comments -------------------------------------------------------------

def picture_block(jpg, w, h):
    mime, desc = b'image/jpeg', b''
    return (struct.pack('>I', 3) + struct.pack('>I', len(mime)) + mime
            + struct.pack('>I', len(desc)) + desc
            + struct.pack('>IIII', w, h, 24, 0)
            + struct.pack('>I', len(jpg)) + jpg)

def comment_body(vendor, comments):
    b = struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', len(comments))
    for c in comments:
        b += struct.pack('<I', len(c)) + c
    return b

def cover(side, q, seed):
    rnd = random.Random(seed)
    im = Image.new('RGB', (side, side))
    px = im.load()
    for y in range(side):
        for x in range(side):
            px[x, y] = ((x * 255 // side) ^ rnd.randrange(48),
                        (y * 255 // side) ^ rnd.randrange(48),
                        ((x + y) * 127 // side) ^ rnd.randrange(48))
    path = tempfile.mktemp(suffix='.jpg')
    im.save(path, quality=q, progressive=False, optimize=False)
    data = open(path, 'rb').read()
    os.unlink(path)
    return data, side

def silence(codec, ext):
    path = tempfile.mktemp(suffix=ext)
    args = ['ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-t', '0.3',
            '-i', 'anullsrc=r=48000:cl=stereo', '-map_metadata', '-1',
            '-fflags', '+bitexact', '-flags:a', '+bitexact']
    if codec == 'vorbis':
        args += ['-c:a', 'vorbis', '-strict', '-2', '-ar', '44100']
    else:
        args += ['-c:a', 'libopus', '-b:a', '16k']
    subprocess.check_call(args + [path])
    data = open(path, 'rb').read()
    os.unlink(path)
    return data

def build(name, codec, jpg, side, maxseg, filler=0, split_in_header=False):
    src = silence(codec, '.ogg' if codec == 'vorbis' else '.opus')
    pages = read_pages(src)
    pk = packets(pages)
    serial = pages[0]['serial']
    nhdr = 3 if codec == 'vorbis' else 2
    vendor = b'ProkBox fixture'
    pic = b'METADATA_BLOCK_PICTURE=' + base64.b64encode(picture_block(jpg, side, side))
    comments = [b'TITLE=' + name.encode(), b'ARTIST=Fixture', b'ALBUM=Ogg Art']
    prefix = b'\x03vorbis' if codec == 'vorbis' else b'OpusTags'
    first_room = None
    if split_in_header:
        # a filler comment sized so the first page break lands 20 base64
        # characters into the picture value (inside its 42-byte header)
        room = maxseg * 255
        base = len(prefix) + len(comment_body(vendor, comments + [b'C=', b'']))
        # bytes before the picture value: base + filler + 4 (pic length) + name
        name_len = len(b'METADATA_BLOCK_PICTURE=')
        fill = room - (base + 4 + name_len + 20)
        assert fill > 0
        comments.append(b'C=' + b'x' * fill)
        first_room = room
    comments.append(pic)
    body = prefix + comment_body(vendor, comments)
    if codec == 'vorbis':
        body += b'\x01'
    if split_in_header:
        v = body.index(b'METADATA_BLOCK_PICTURE=') + 23
        assert v < first_room < v + 56, (v, first_room)
    seq = 0
    out = page_bytes(pages[0]['flags'], 0, serial, seq, [len(pk[0])], pk[0])
    seq += 1
    hp, seq = paginate(body, serial, seq, maxseg, first_room)
    out += b''.join(hp)
    if codec == 'vorbis':
        sp, seq = paginate(pk[2], serial, seq, 255)
        out += b''.join(sp)
    # the audio pages as they were, renumbered
    hdr_pages = 0
    count = 0
    for i, pg in enumerate(pages):
        count += sum(1 for l in pg['lacing'] if l < 255)
        if count >= nhdr:
            hdr_pages = i + 1
            break
    for pg in pages[hdr_pages:]:
        out += page_bytes(pg['flags'], pg['granule'], serial, seq, pg['lacing'], pg['body'])
        seq += 1
    ext = '.ogg' if codec == 'vorbis' else '.opus'
    open(os.path.join(OUT, name + ext), 'wb').write(out)
    npages = len(hp)
    print(f'{name}{ext}: {len(out)} bytes, cover {len(jpg)} bytes, '
          f'comment header on {npages} page(s)')

small, s_side = cover(96, 85, 1)
big, b_side = cover(400, 92, 2)
mid, m_side = cover(200, 90, 3)
build('art_1page', 'vorbis', small, s_side, 255)
build('art_2page', 'vorbis', big, b_side, 255)
build('art_2page', 'opus', big, b_side, 255)
build('art_hdrsplit', 'opus', mid, m_side, 16, split_in_header=True)
