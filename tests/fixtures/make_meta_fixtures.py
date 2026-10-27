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

# Regenerate the metadata-extras fixtures used by tests/audiobook/run.sh
# (issues #18 UTF-16 chapter titles, #35 lyrics, #39 per-chapter art).
#
#   id3v24_chapters.mp3  ID3v2.4: CHAP frames whose TIT2 sub-frames use every
#                        ID3 text encoding (Latin-1, UTF-16 with BOM, UTF-16
#                        without BOM, UTF-16BE, UTF-8), stored out of
#                        chronological order; APIC (JPEG, PNG) inside two
#                        CHAP frames; a UTF-16 USLT with a non-BMP character.
#   id3v23_unsync.mp3    ID3v2.3 with tag-wide unsynchronisation: UTF-16
#                        (BOM FF FE gets unsync-stuffed) chapter titles and
#                        lyrics; an APIC inside a CHAP, which must be ignored
#                        (its file offsets are not usable under unsync).
#   rich_chapters.m4b    MP4 with a QuickTime chapter text track whose samples
#                        are UTF-16 (BOM BE and LE) and UTF-8, a chapter image
#                        track (hdlr 'vide', 'jpeg', referenced by tref 'chap')
#                        with fewer images than chapters, and a ©lyr atom.
#
# Requires ffmpeg (libmp3lame, aac, mjpeg, png) and python3. The audio and the
# images come from ffmpeg; the tags and the MP4 chapter tracks are written
# here, byte by byte, so every encoding case is exactly what it says.
# Run from anywhere; writes next to this script. The expected values printed
# at the end are what tests/audiobook/golden/*.txt must show.
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FF = ['ffmpeg', '-nostdin', '-v', 'error', '-y']
BITEXACT = ['-fflags', '+bitexact', '-flags:a', '+bitexact']


def ff(*args):
    subprocess.run(FF + list(args), check=True)


def image(tmp, name, color, codec):
    path = os.path.join(tmp, name)
    ff('-f', 'lavfi', '-i', 'color=%s:s=16x16' % color, '-frames:v', '1',
       '-c:v', codec, '-fflags', '+bitexact', '-flags:v', '+bitexact',
       '-f', 'image2', path)
    return open(path, 'rb').read()


# ---------------------------------------------------------------- ID3 -----

def synchsafe(n):
    return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f,
                  n & 0x7f])


def id3_frame(fid, body, v24):
    size = synchsafe(len(body)) if v24 else struct.pack('>I', len(body))
    return fid.encode() + size + b'\0\0' + body


def text_body(enc, text, bom=True, le=True):
    """ID3 text frame body: encoding byte + text (no terminator)."""
    if enc == 0:
        data = text.encode('latin-1')
    elif enc == 1:
        data = text.encode('utf-16-le' if le else 'utf-16-be')
        if bom:
            data = (b'\xff\xfe' if le else b'\xfe\xff') + data
    elif enc == 2:
        data = text.encode('utf-16-be')
    else:
        data = text.encode('utf-8')
    return bytes([enc]) + data


def apic_body(mime, data):
    # Latin-1 description; picture type 0x00 ("other")
    return b'\x00' + mime.encode() + b'\0' + b'\x00' + b'chapter art\0' + data


def chap(eid, start, end, subframes, v24):
    body = eid.encode() + b'\0' + struct.pack('>IIII', start, end,
                                               0xffffffff, 0xffffffff)
    for fid, sub in subframes:
        body += id3_frame(fid, sub, v24)
    return id3_frame('CHAP', body, v24)


def ctoc(ids, v24):
    body = b'toc\0' + bytes([0x03, len(ids)])     # top level, ordered
    for i in ids:
        body += i.encode() + b'\0'
    return id3_frame('CTOC', body, v24)


def uslt(enc, lang, desc, text, v24, le=True):
    if enc == 1:
        tenc = 'utf-16-le' if le else 'utf-16-be'
        bom = b'\xff\xfe' if le else b'\xfe\xff'
        d = bom + desc.encode(tenc) + b'\0\0'
        t = bom + text.encode(tenc)
    elif enc == 0:
        d = desc.encode('latin-1') + b'\0'
        t = text.encode('latin-1')
    else:
        d = desc.encode('utf-8') + b'\0'
        t = text.encode('utf-8')
    return id3_frame('USLT', bytes([enc]) + lang.encode() + d + t, v24)


def unsynchronise(data):
    out = bytearray()
    for i, b in enumerate(data):
        out.append(b)
        if b == 0xff and (i + 1 == len(data) or data[i + 1] == 0
                          or data[i + 1] >= 0xe0):
            out.append(0)
    return bytes(out)


def id3_tag(frames, v24, unsync=False, padding=64):
    body = b''.join(frames)
    flags = 0
    if unsync:
        body = unsynchronise(body)
        flags |= 0x80
    body += b'\0' * padding
    return b'ID3' + bytes([4 if v24 else 3, 0, flags]) + synchsafe(len(body)) \
        + body


# ---------------------------------------------------------------- MP4 -----

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'udta', b'tref',
              b'dinf', b'edts'}


def parse(data, off=0, end=None):
    end = len(data) if end is None else end
    out = []
    while off + 8 <= end:
        size, typ = struct.unpack('>I4s', data[off:off + 8])
        assert size >= 8, 'unsupported atom size'
        if typ in CONTAINERS:
            out.append([typ, parse(data, off + 8, off + size)])
        else:
            out.append([typ, data[off + 8:off + size]])
        off += size
    return out


def build(atoms):
    out = b''
    for typ, body in atoms:
        if isinstance(body, list):
            body = build(body)
        out += struct.pack('>I4s', len(body) + 8, typ) + body
    return out


def child(atoms, typ):
    return [a for a in atoms if a[0] == typ]


def full(version_flags, payload):
    return struct.pack('>I', version_flags) + payload


def track_tables(sizes, durations, offsets):
    stts = full(0, struct.pack('>I', len(durations))
                + b''.join(struct.pack('>II', 1, d) for d in durations))
    stsz = full(0, struct.pack('>II', 0, len(sizes))
                + b''.join(struct.pack('>I', s) for s in sizes))
    stsc = full(0, struct.pack('>I', 1) + struct.pack('>III', 1, 1, 1))
    stco = full(0, struct.pack('>I', len(offsets))
                + b''.join(struct.pack('>I', o) for o in offsets))
    return stts, stsz, stsc, stco


def set_tables(stbl, tables):
    names = (b'stts', b'stsz', b'stsc', b'stco')
    for name, body in zip(names, tables):
        for a in stbl:
            if a[0] == name:
                a[1] = body
                break
        else:
            stbl.append([name, body])
    stbl[:] = [a for a in stbl if a[0] not in (b'co64', b'stss', b'ctts')]


def handler(trak):
    mdia = child(trak[1], b'mdia')[0][1]
    hdlr = child(mdia, b'hdlr')[0][1]
    return hdlr[8:12]


def make_m4b(tmp, jpg, png):
    meta = os.path.join(tmp, 'rich.txt')
    with open(meta, 'w', encoding='utf-8') as f:
        f.write(';FFMETADATA1\ntitle=Rich Test Book\nartist=Fixture Author\n'
                'album=Rich Test Book\n')
        for start, end in ((0, 2000), (2000, 4000), (4000, 5500),
                           (5500, 8000)):
            f.write('[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\n'
                    'title=placeholder\n' % (start, end))
    src = os.path.join(tmp, 'rich_src.m4b')
    ff('-f', 'lavfi', '-i', 'sine=frequency=262:duration=8', '-i', meta,
       '-map', '0:a', '-map_metadata', '1', '-map_chapters', '1',
       '-metadata', 'lyrics=Line one\rLine two – Ünïcode\rLast line',
       '-c:a', 'aac', '-b:a', '16k', '-ac', '1', '-ar', '22050', *BITEXACT,
       '-movflags', 'disable_chpl', '-f', 'ipod', src)
    data = open(src, 'rb').read()
    top = parse(data)
    assert [a[0] for a in top][-1] == b'moov', 'moov must be last'
    if b'\xa9lyr' not in data:
        raise SystemExit('ffmpeg did not write a ©lyr atom')
    moov = child(top, b'moov')[0][1]
    traks = child(moov, b'trak')
    text = [t for t in traks if handler(t) == b'text'][0]
    soun = [t for t in traks if handler(t) == b'soun'][0]

    # new chapter titles (UTF-16 with BOM BE / LE, UTF-8, UTF-16 BOM BE
    # with a surrogate pair); one sample per chapter
    def sample(raw):
        return struct.pack('>H', len(raw)) + raw
    titles = [
        b'\xfe\xff' + 'Prologue (UTF-16BE)'.encode('utf-16-be'),
        b'\xff\xfe' + 'Kapitel Zwei – Überblick'.encode('utf-16-le'),
        'Chapter Three: Café'.encode('utf-8'),
        b'\xfe\xff' + 'Finale 🎧 終わり'.encode('utf-16-be'),
    ]
    text_samples = [sample(t) for t in titles]
    images = [jpg, png, jpg]        # 3 images for 4 chapters
    image_durs = [2000, 3500, 2500]  # ms; chapter 3 (4000 ms) shows image 2

    # the text track keeps its own timing (stts) from ffmpeg
    mdia = child(text[1], b'mdia')[0][1]
    stbl = child(child(mdia, b'minf')[0][1], b'stbl')[0][1]
    text_stts = child(stbl, b'stts')[0][1]

    # video track: clone of the text track, re-typed
    tkhd_text = child(text[1], b'tkhd')[0][1]
    max_id = max(struct.unpack('>I', child(t[1], b'tkhd')[0][1][12:16])[0]
                 for t in traks)
    vid_id = max_id + 1
    tkhd = bytearray(tkhd_text)
    tkhd[12:16] = struct.pack('>I', vid_id)
    tkhd[3] = 0                     # flags: disabled (like iTunes)
    mdhd = full(0, struct.pack('>IIII', 0, 0, 1000, 8000) + b'\x55\xc4\0\0')
    hdlr = full(0, b'\0\0\0\0' + b'vide' + b'\0' * 12 + b'\0')
    vmhd = full(1, b'\0' * 8)
    dref = full(0, struct.pack('>I', 1)) + build([[b'url ', full(1, b'')]])
    jpeg_entry = (b'\0' * 6 + struct.pack('>H', 1) + b'\0' * 16
                  + struct.pack('>HHII', 16, 16, 0x480000, 0x480000)
                  + b'\0' * 4 + struct.pack('>H', 1) + b'\0' * 32
                  + struct.pack('>Hh', 24, -1))
    stsd = full(0, struct.pack('>I', 1)) + build([[b'jpeg', jpeg_entry]])
    vstbl = [[b'stsd', stsd]]
    vtrak = [b'trak', [
        [b'tkhd', bytes(tkhd)],
        [b'mdia', [
            [b'mdhd', mdhd],
            [b'hdlr', hdlr],
            [b'minf', [[b'vmhd', vmhd], [b'dinf', [[b'dref', dref]]],
                       [b'stbl', vstbl]]],
        ]],
    ]]
    # chap reference: audio track -> text track, then the image track
    stref = child(soun[1], b'tref')[0][1]
    chapref = child(stref, b'chap')[0]
    chapref[1] = chapref[1] + struct.pack('>I', vid_id)
    moov.insert(moov.index(traks[-1]) + 1, vtrak)

    # layout: original file up to moov, new moov, then a new mdat with the
    # text samples followed by the images. Table sizes don't depend on the
    # offsets, so build once with dummy offsets to get the moov size.
    def tables(base):
        offs, pos = [], base
        for s in text_samples:
            offs.append(pos)
            pos += len(s)
        ioffs = []
        for im in images:
            ioffs.append(pos)
            pos += len(im)
        return offs, ioffs

    prefix = data[:sum(len(build([a])) for a in top[:-1])]
    for _ in range(2):
        moov_len = len(build([[b'moov', moov]]))
        base = len(prefix) + moov_len + 8
        offs, ioffs = tables(base)
        stts_text = text_stts
        set_tables(stbl, (stts_text,) + track_tables(
            [len(s) for s in text_samples], [0] * 4, offs)[1:])
        set_tables(vstbl, track_tables([len(i) for i in images], image_durs,
                                       ioffs))
    assert len(build([[b'moov', moov]])) == moov_len
    payload = b''.join(text_samples) + b''.join(images)
    out = prefix + build([[b'moov', moov]]) + build([[b'mdat', payload]])
    return out, titles


def main():
    with tempfile.TemporaryDirectory() as tmp:
        jpg = image(tmp, 'a.jpg', 'red', 'mjpeg')
        png = image(tmp, 'b.png', 'blue', 'png')
        mp3 = os.path.join(tmp, 'audio.mp3')
        ff('-f', 'lavfi', '-i', 'sine=frequency=392:duration=8',
           '-map_metadata', '-1', '-c:a', 'libmp3lame', '-b:a', '24k',
           '-ac', '1', '-ar', '22050', *BITEXACT, '-write_xing', '0',
           '-id3v2_version', '0', '-write_id3v1', '0', mp3)
        audio = open(mp3, 'rb').read()

        lyrics = 'First line\r\nZweite Zeile – Über\r\n🎧 third line\r\n'

        # ID3v2.4, chapters out of order (ch4 stored before ch3)
        v = True
        frames = [
            id3_frame('TIT2', text_body(3, 'ID3 Chapter Book'), v),
            id3_frame('TPE1', text_body(3, 'Fixture Author'), v),
            uslt(1, 'eng', 'desc', lyrics, v),
            ctoc(['ch0', 'ch1', 'ch2', 'ch3', 'ch4'], v),
            chap('ch0', 0, 1500, [('TIT2', text_body(0, 'Opening Café'))], v),
            chap('ch1', 1500, 3000,
                 [('TIT2', text_body(1, 'Kapitel Zwei – Über', True, True)),
                  ('APIC', apic_body('image/jpeg', jpg))], v),
            chap('ch2', 3000, 4500,
                 [('TIT2', text_body(2, '日本語の章 🎧'))], v),
            chap('ch4', 6000, 8000,
                 [('TIT2', text_body(3, 'Épilogue')),
                  ('APIC', apic_body('image/png', png))], v),
            chap('ch3', 4500, 6000,
                 [('TIT2', text_body(1, 'Part Four (LE, no BOM)', False,
                                     True))], v),
        ]
        open(os.path.join(HERE, 'id3v24_chapters.mp3'), 'wb').write(
            id3_tag(frames, v) + audio)

        # ID3v2.3, tag-wide unsynchronisation
        v = False
        frames = [
            id3_frame('TIT2', text_body(0, 'Unsync Book'), v),
            uslt(1, 'eng', '', 'ÿ unsync lyrics ÿ\nline 2', v),
            chap('c1', 0, 4000,
                 [('TIT2', text_body(1, 'Erster Teil ÿ', True, True)),
                  ('APIC', apic_body('image/jpeg', jpg))], v),
            chap('c2', 4000, 8000,
                 [('TIT2', text_body(1, 'Zweiter Teil', True, False))], v),
        ]
        open(os.path.join(HERE, 'id3v23_unsync.mp3'), 'wb').write(
            id3_tag(frames, v, unsync=True) + audio)

        m4b, _ = make_m4b(tmp, jpg, png)
        open(os.path.join(HERE, 'rich_chapters.m4b'), 'wb').write(m4b)

        print('jpeg %d bytes crc32 %08x' % (len(jpg), zlib.crc32(jpg)))
        print('png  %d bytes crc32 %08x' % (len(png), zlib.crc32(png)))
    for n in ('id3v24_chapters.mp3', 'id3v23_unsync.mp3',
              'rich_chapters.m4b'):
        print(n, os.path.getsize(os.path.join(HERE, n)))


if __name__ == '__main__':
    sys.exit(main())
