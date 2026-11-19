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

"""tests/upfix: crafted variants of the committed fixtures, made at test
time (deterministic byte surgery, no encoder needed), for the upstream
fixes taken in on branch fix-upstream-batch.

    gen.py <fixtures dir> <out dir> [name...]   (default: all)
    gen.py tree <file>                          (print the MP4 atom tree)
    gen.py artpos                               (mp3_apic16*'s picture)
    gen.py newpos <fixtures dir>                (picback/picpng/apicunsync pictures)
"""
import os
import struct
import sys

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'udta',
              b'dinf', b'edts', b'ilst'}


class Atom:
    def __init__(self, typ, data=b'', kids=None, pre=b''):
        self.typ = typ
        self.data = data      # leaf payload
        self.kids = kids      # list of Atom for containers
        self.pre = pre        # bytes before the children (meta: 4)

    def find(self, path):
        a = self
        for t in path.split('/'):
            a = next(k for k in a.kids if k.typ == t.encode())
        return a

    def body(self):
        if self.kids is None:
            return self.data
        return self.pre + b''.join(k.raw() for k in self.kids)

    def raw(self):
        b = self.body()
        return struct.pack('>I4s', len(b) + 8, self.typ) + b


def parse(d, o=0, e=None):
    e = len(d) if e is None else e
    out = []
    while o + 8 <= e:
        s, t = struct.unpack('>I4s', d[o:o + 8])
        if s == 0:
            s = e - o
        body = d[o + 8:o + s]
        if t in CONTAINERS:
            out.append(Atom(t, kids=parse(body)))
        elif t == b'meta':
            out.append(Atom(t, kids=parse(body, 4), pre=body[:4]))
        else:
            out.append(Atom(t, data=body))
        o += s
    return out


def tree(atoms, dep=0):
    for a in atoms:
        print('  ' * dep + a.typ.decode('latin1'), len(a.raw()))
        if a.kids is not None:
            tree(a.kids, dep + 1)


def top(atoms, t):
    return next(a for a in atoms if a.typ == t)


def audio_stbl(moov):
    return moov.find('trak/mdia/minf/stbl')


def flat(atoms):
    return b''.join(a.raw() for a in atoms)


# ---- m4a variants (from base.m4a: ftyp free mdat moov) ----

def m4a_mdat64(base):
    """free(8)+mdat as one mdat with the 64-bit size form: the payload
    stays where it was, so the chunk offsets stay valid"""
    atoms = parse(base)
    out = b''
    i = 0
    while i < len(atoms):
        a = atoms[i]
        if a.typ == b'free' and len(a.data) == 0 and atoms[i + 1].typ == b'mdat':
            m = atoms[i + 1]
            out += struct.pack('>I4sQ', 1, b'mdat', len(m.data) + 16) + m.data
            i += 2
            continue
        out += a.raw()
        i += 1
    assert len(out) == len(base)
    return out


def m4a_gap(base, wide=False, per=15, gap=64):
    """the audio cut into chunks of `per` frames with `gap` bytes of junk
    between them, so only the chunk offset table finds the frames; wide:
    that table as co64 instead of stco (moov sits after mdat, so growing
    it moves no sample)"""
    atoms = parse(base)
    pos = 0
    for a in atoms:
        if a.typ == b'mdat':
            break
        pos += len(a.raw())
    mdat = next(a for a in atoms if a.typ == b'mdat')
    moov = top(atoms, b'moov')
    stbl = audio_stbl(moov)
    stsz = next(k for k in stbl.kids if k.typ == b'stsz')
    n = struct.unpack('>I', stsz.data[8:12])[0]
    sizes = struct.unpack('>%dI' % n, stsz.data[12:12 + 4 * n])
    st = next(k for k in stbl.kids if k.typ == b'stco')
    assert struct.unpack('>I', st.data[4:8])[0] == 1
    first = struct.unpack('>I', st.data[8:12])[0]
    assert first == pos + 8 and n % per == 0
    data, offs, o = b'', [], 0
    for c in range(n // per):
        if c:
            data += b'\xa5' * gap
        offs.append(pos + 8 + len(data))
        k = sum(sizes[c * per:(c + 1) * per])
        data += mdat.data[o:o + k]
        o += k
    data += mdat.data[o:]
    mdat.data = data
    stsc = next(k for k in stbl.kids if k.typ == b'stsc')
    stsc.data = stsc.data[:4] + struct.pack('>IIII', 1, 1, per, 1)
    if wide:
        st.typ = b'co64'
        st.data = st.data[:4] + struct.pack('>I%dQ' % len(offs), len(offs), *offs)
    else:
        st.data = st.data[:4] + struct.pack('>I%dI' % len(offs), len(offs), *offs)
    i = [a.typ for a in atoms].index(b'moov')
    assert i > [a.typ for a in atoms].index(b'mdat')
    return flat(atoms)


def m4a_co64(base):
    return m4a_gap(base, wide=True)


def m4a_long(base, dur=1 << 27):
    """every stts entry's duration set to dur: 45 frames then last
    about 38 h at 44.1 kHz, past the 32-bit sample count (27 h)"""
    atoms = parse(base)
    moov = top(atoms, b'moov')
    stbl = audio_stbl(moov)
    st = next(k for k in stbl.kids if k.typ == b'stts')
    n = struct.unpack('>I', st.data[4:8])[0]
    ent = list(struct.unpack('>%dI' % (2 * n), st.data[8:8 + 8 * n]))
    for j in range(1, 2 * n, 2):
        ent[j] = dur
    st.data = st.data[:8] + struct.pack('>%dI' % (2 * n), *ent)
    return flat(atoms)


def m4a_clamp(base):
    """udta (title, then an atom claiming more than its parent holds)
    moved before the audio trak: the walk of moov must still reach it"""
    atoms = parse(base)
    moov = top(atoms, b'moov')
    udta = next(k for k in moov.kids if k.typ == b'udta')
    ilst = udta.find('meta/ilst')
    # a well-formed album, then the oversized one last in ilst
    alb = Atom(b'\xa9alb', kids=None,
               data=Atom(b'data', data=b'\0\0\0\1\0\0\0\0Clamp').raw())
    ilst.kids.append(alb)
    moov.kids.remove(udta)
    moov.kids.insert(1, udta)          # after mvhd, before trak
    raw_moov = bytearray(moov.raw())
    # make the last ilst child claim 4000 bytes
    pos = raw_moov.find(b'\xa9alb') - 4
    struct.pack_into('>I', raw_moov, pos, 4000)
    i = [a.typ for a in atoms].index(b'moov')
    return flat(atoms[:i]) + bytes(raw_moov) + flat(atoms[i + 1:])


def m4a_sttsbig(base):
    """the stts entry count 0x7fffffff in a two-entry atom: the parser
    must stop at the atom's end, not loop for billions of entries"""
    atoms = parse(base)
    moov = top(atoms, b'moov')
    st = next(k for k in audio_stbl(moov).kids if k.typ == b'stts')
    st.data = st.data[:4] + struct.pack('>I', 0x7fffffff) + st.data[8:]
    return flat(atoms)


# ---- other formats ----

def flac_blocks(d):
    """(type, body) of each metadata block, and the frames after them"""
    assert d[:4] == b'fLaC'
    o, out = 4, []
    while True:
        h = d[o]
        n = int.from_bytes(d[o + 1:o + 4], 'big')
        out.append((h & 0x7f, d[o + 4:o + 4 + n]))
        o += 4 + n
        if h & 0x80:
            return out, d[o:]


def flac_join(blocks, frames):
    out = b'fLaC'
    for i, (t, b) in enumerate(blocks):
        last = 0x80 if i == len(blocks) - 1 else 0
        out += bytes([t | last]) + len(b).to_bytes(3, 'big') + b
    return out + frames


JPEG = b'\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00' \
    + bytes(range(64)) + b'\xff\xd9'


def picture(mime_len=None, mime=b'image/jpeg'):
    ml = len(mime) if mime_len is None else mime_len
    return struct.pack('>II', 3, ml) + mime + struct.pack('>I', 0) + \
        struct.pack('>IIIII', 1, 1, 24, 0, len(JPEG)) + JPEG


def flac_pic(base, mime_len=None):
    """a PICTURE block after STREAMINFO (mime_len: the length field as
    written; 0x80000000 is negative as an int)"""
    blocks, frames = flac_blocks(base)
    blocks.insert(1, (6, picture(mime_len)))
    return flac_join(blocks, frames)


def flac_picneg(base):
    return flac_pic(base, 0x80000000)


PNG = b'\x89PNG\r\n\x1a\n' + bytes(32)


def picture_of(ptype, mime, data):
    """a PICTURE block body: picture type, MIME, no description"""
    return struct.pack('>II', ptype, len(mime)) + mime + \
        struct.pack('>I', 0) + \
        struct.pack('>IIIII', 1, 1, 24, 0, len(data)) + data


def flac_pics(base, pics):
    """PICTURE blocks after STREAMINFO, in this order"""
    blocks, frames = flac_blocks(base)
    for i, p in enumerate(pics):
        blocks.insert(1 + i, (6, p))
    return flac_join(blocks, frames)


def flac_picback(base):
    """upstream cb36cae2bc: a back cover (type 4), then the front (3)"""
    return flac_pics(base, [picture_of(4, b'image/jpeg', JPEG),
                            picture_of(3, b'image/jpeg', JPEG)])


def flac_picpng(base):
    """upstream cb36cae2bc: a PNG front cover, then a JPEG front cover"""
    return flac_pics(base, [picture_of(3, b'image/png', PNG),
                            picture_of(3, b'image/jpeg', JPEG)])


def flac_pic_pos(base, pics, k):
    """where the JPEG of picture k starts in flac_pics(base, pics)"""
    blocks, _ = flac_blocks(base)
    o = 4 + 4 + len(blocks[0][1])
    for p in pics[:k]:
        o += 4 + len(p)
    return o + 4 + len(pics[k]) - len(JPEG)


def flac_short_block(base, fake_len, fake=b''):
    """a block whose length (4) is too short, so what follows it is read
    as one more block header, here a second STREAMINFO of fake_len bytes
    (fake, then zeros: a sample rate of 0). Upstream's case was a short
    PICTURE; this uses PADDING, since a short PICTURE is refused by the
    picture check (10857759e6) before the second STREAMINFO is reached,
    and the codec walks the blocks the same way whatever their type."""
    blocks, frames = flac_blocks(base)
    first = bytes([0]) + len(blocks[0][1]).to_bytes(3, 'big') + blocks[0][1]
    pic = bytes([1]) + (4).to_bytes(3, 'big') + b'\0\0\0\3'
    second = bytes([0]) + fake_len.to_bytes(3, 'big') + \
        (fake + bytes(fake_len))[:fake_len]
    rest = flac_join(blocks[1:], frames)[4:]
    return b'fLaC' + first + pic + second + rest


def flac_sirate0(base):
    return flac_short_block(base, 34)


def flac_sibig(base):
    """the second STREAMINFO 600 bytes long: the codec read it into its
    255-byte stack buffer"""
    return flac_short_block(base, 600, b'\x10\x00' * 300)


def crc8(b):
    c = 0
    for x in b:
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xff if c & 0x80 else (c << 1) & 0xff
    return c


def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c


def utf8num(v):
    if v < 0x80:
        return bytes([v])
    n = 2
    while v >= 1 << (5 * n + 1):
        n += 1
    out = []
    for _ in range(n - 1):
        out.insert(0, 0x80 | (v & 0x3f))
        v >>= 6
    return bytes([((0xff00 >> n) & 0xff) | v]) + bytes(out)


def flac_frame_hdr(d, o):
    """the header at o if a valid one starts there: (header length,
    blocksize code, number, number's byte length)"""
    if d[o] != 0xff or (d[o + 1] & 0xfe) != 0xf8:
        return None
    bs, sr = d[o + 2] >> 4, d[o + 2] & 15
    p = o + 4
    lead = d[p]
    n = 1
    while n < 8 and lead & (0x80 >> (n - 1)) and n > 0:
        n += 1
    n = 1 if lead < 0x80 else n - 1
    if n > 7:
        return None
    num = lead & (0x7f >> n) if n > 1 else lead
    for i in range(1, n):
        num = (num << 6) | (d[p + i] & 0x3f)
    q = p + n
    q += {6: 1, 7: 2}.get(bs, 0)
    q += {12: 1, 13: 2, 14: 2}.get(sr, 0)
    if crc8(d[o:q]) != d[q]:
        return None
    return q + 1 - o, bs, num, n


def flac_varblock(base):
    """every frame header rewritten to the variable blocking strategy
    (bit set, the first sample's number in place of the frame number),
    CRC-8 and CRC-16 made again; STREAMINFO keeps min = max block size.
    The decoder took the sample number for a frame number and multiplied
    it by the block size."""
    blocks, frames = flac_blocks(base)
    si = blocks[0][1]
    maxbs = (si[2] << 8) | si[3]
    starts = [o for o in range(len(frames) - 1) if flac_frame_hdr(frames, o)]
    # keep only starts whose frame number follows the one before
    good, want = [], 0
    for o in starts:
        if flac_frame_hdr(frames, o)[2] == want:
            good.append(o)
            want += 1
    out = b''
    for i, o in enumerate(good):
        end = good[i + 1] if i + 1 < len(good) else len(frames)
        hl, bs, num, nl = flac_frame_hdr(frames, o)
        h = bytes([frames[o], frames[o + 1] | 1, frames[o + 2], frames[o + 3]])
        h += utf8num(num * maxbs) + frames[o + 4 + nl:o + hl - 1]
        h += bytes([crc8(h)])
        body = frames[o + hl:end - 2]
        f = h + body
        out += f + struct.pack('>H', crc16(f))
    return flac_join(blocks, out)


def flac_badframe(base):
    """one damaged byte in the 8th of 16 frame headers (its CRC-8 then
    fails): one frame lost, the track goes on"""
    blocks, frames = flac_blocks(base)
    starts = [o for o in range(len(frames) - 1) if flac_frame_hdr(frames, o)]
    o = [s for s in starts if flac_frame_hdr(frames, s)[2] == 7][0]
    b = bytearray(frames)
    b[o + 3] ^= 0x10
    return flac_join(blocks, bytes(b))


def le32(v):
    return struct.pack('<I', v & 0xffffffff)


def wav(fmt_bits=16, info=b'', data=None, fmt_tag=1):
    """a mono 8 kHz WAV: fmt, an optional LIST INFO chunk, data"""
    if data is None:
        data = bytes(1600)
    ba = max(1, fmt_bits // 8)
    fmt = struct.pack('<HHIIHH', fmt_tag, 1, 8000, 8000 * ba, ba, fmt_bits)
    body = b'WAVE' + b'fmt ' + le32(len(fmt)) + fmt
    if info:
        body += b'LIST' + le32(len(info) + 4) + b'INFO' + info
        if len(info) & 1:
            body += b'\0'
    body += b'data' + le32(len(data)) + data
    return b'RIFF' + le32(len(body)) + body


def info_item(tag, text, size=None):
    n = len(text) if size is None else size
    out = tag + le32(n) + text
    return out + (b'\0' if len(text) & 1 else b'')


def wav_infoneg(_):
    """INAM, then an ICMT whose size reads as negative"""
    return wav(info=info_item(b'INAM', b'Wave Title\0') +
               info_item(b'ICMT', b'x' * 8, 0x80000000))


def wav_infolong(_):
    """INAM, then a 1700-character Latin-1 ICMT (3400 bytes as UTF-8,
    past the 1800-byte tag buffer) and an IART after it"""
    return wav(info=info_item(b'INAM', b'Wave Title\0') +
               info_item(b'ICMT', b'\xe9' * 1700 + b'\0') +
               info_item(b'IART', b'Wave Artist\0'))


def wav_pcm4(_):
    """linear PCM of 4 bits a sample"""
    return wav(fmt_bits=4)


def id3v23(frames):
    body = b''.join(frames)
    sz = len(body)
    syncsafe = bytes([(sz >> 21) & 0x7f, (sz >> 14) & 0x7f,
                      (sz >> 7) & 0x7f, sz & 0x7f])
    return b'ID3\x03\x00\x00' + syncsafe + body


def id3frame(fid, body):
    return fid + struct.pack('>I', len(body)) + b'\0\0' + body


def mp3_audio(base):
    assert base[:3] == b'ID3'
    sz = (base[6] << 21) | (base[7] << 14) | (base[8] << 7) | base[9]
    return base[10 + sz:]


def apic_tag(desc_utf16):
    """TIT2, then APIC: UTF-16 (encoding 1) description with its BOM"""
    tit = id3frame(b'TIT2', b'\0Apic Song')
    apic = id3frame(b'APIC', b'\x01image/jpeg\0\x03' + desc_utf16 + JPEG)
    return id3v23([tit, apic])


def mp3_apic16(base):
    """the picture is where the generator put it: art_pos below"""
    return apic_tag(b'\xff\xfe' + 'Cover'.encode('utf-16-le') + b'\0\0') + \
        mp3_audio(base)


def mp3_apic16e(base):
    return apic_tag(b'\xff\xfe\0\0') + mp3_audio(base)


def art_pos(desc_len):
    """where mp3_apic16*'s JPEG starts: tag header, TIT2, APIC header,
    encoding, MIME and nil, picture type, description"""
    return 10 + (10 + 10) + 10 + 1 + 11 + 1 + desc_len


def unsync(b):
    """ID3v2 unsynchronisation: a 0x00 after each 0xFF that is followed
    by 0x00 or by 0xE0 and above (and after a final 0xFF)"""
    out = bytearray()
    for i, c in enumerate(b):
        out.append(c)
        if c == 0xff and (i + 1 == len(b) or b[i + 1] == 0
                          or b[i + 1] >= 0xe0):
            out.append(0)
    return bytes(out)


def apic_unsync_parts():
    """TIT2, then an APIC with a UTF-16 description (BOM 0xFF 0xFE)"""
    tit = id3frame(b'TIT2', b'\0Apic Song')
    head = b'\x01image/jpeg\0\x03' + b'\xff\xfe' + \
        'Cover'.encode('utf-16-le') + b'\0\0'
    return tit, id3frame(b'APIC', head + JPEG), len(head)


def mp3_apicunsync(base):
    """upstream 619144481a: an ID3v2.3 tag unsynchronised as a whole
    (header flag 0x80): the picture must be found where it starts"""
    tit, apic, _ = apic_unsync_parts()
    body = unsync(tit + apic)
    sz = len(body)
    syncsafe = bytes([(sz >> 21) & 0x7f, (sz >> 14) & 0x7f,
                      (sz >> 7) & 0x7f, sz & 0x7f])
    return b'ID3\x03\x00\x80' + syncsafe + body + mp3_audio(base)


def apicunsync_pos():
    """the JPEG's first byte in mp3_apicunsync()'s file"""
    tit, apic, hl = apic_unsync_parts()
    return 10 + len(unsync(tit + apic[:10 + hl]))


def ieee80(rate):
    import math
    m, e = math.frexp(rate)       # rate = m * 2**e, 0.5 <= m < 1
    return struct.pack('>HQ', 16383 + e - 1, int(m * (1 << 64)))


def aiff(chunks, frames=800):
    comm = struct.pack('>hIh', 1, frames, 16) + ieee80(8000)
    ssnd = struct.pack('>II', 0, 0) + bytes(frames * 2)
    body = b'AIFF'
    for cid, data in [(b'COMM', comm)] + chunks + [(b'SSND', ssnd)]:
        body += cid + struct.pack('>I', len(data)) + data
        if len(data) & 1:
            body += b'\0'
    return b'FORM' + struct.pack('>I', len(body)) + body


def aiff_id3first(_):
    """an ID3 chunk before SSND (the old walk never read one there)"""
    tag = id3v23([id3frame(b'TIT2', b'\0Aiff Id3 Title')])
    return aiff([(b'ID3 ', tag)])


def aiff_textlong(_):
    """AUTH, then NAME and ANNO of 1500 bytes each: the old code read at
    most 512 of each and moved its pointer on by the chunk's size, so
    ANNO went 1512 bytes into the 1800-byte buffer and past its end"""
    return aiff([(b'AUTH', b'Aiff Author'), (b'NAME', b'N' * 1500),
                 (b'ANNO', b'A' * 1500)])


def vgm_gd3(strings):
    gd3 = b''.join(s.encode('utf-16-le') + b'\0\0' for s in strings)
    gd3 = b'Gd3 ' + le32(0x100) + le32(len(gd3)) + gd3
    hdr = bytearray(0x40)
    hdr[0:4] = b'Vgm '
    data = b'\x66\0\0\0'                        # end of sound data
    total = 0x40 + len(data) + len(gd3)
    struct.pack_into('<I', hdr, 0x04, total - 4)
    struct.pack_into('<I', hdr, 0x08, 0x150)
    struct.pack_into('<I', hdr, 0x0c, 3579545)  # SN76489 clock
    struct.pack_into('<I', hdr, 0x14, 0x40 + len(data) - 0x14)
    struct.pack_into('<I', hdr, 0x18, 44100)    # 1 s
    struct.pack_into('<I', hdr, 0x34, 0x40 - 0x34)
    return bytes(hdr) + data + gd3


VGM_TITLE = '音楽' * 10                 # 20 CJK characters


def vgm_cjk(_):
    """a 20-character CJK title (40 bytes of UTF-16, 60 of UTF-8), the
    game and author after it"""
    return vgm_gd3([VGM_TITLE, '', 'Game', '', 'Sys', '', 'Author', '',
                    '2026', 'Dumper', 'Note'])


def vgm_pcmseek(_):
    """YM2612 song: a 4-byte PCM data block, a PCM seek 256 MB past it,
    then DAC writes from there (the emulator read wherever the seek
    pointed), and a write command cut short by the end of the data"""
    hdr = bytearray(0x40)
    hdr[0:4] = b'Vgm '
    data = (b'\x67\x66\x00' + le32(4) + b'\x80\x90\xa0\xb0' +
            b'\xe0' + le32(0x10000000) + b'\x81' * 64 +
            b'\x61' + struct.pack('<H', 44100) + b'\x52')
    struct.pack_into('<I', hdr, 0x04, 0x40 + len(data) - 4)
    struct.pack_into('<I', hdr, 0x08, 0x150)
    struct.pack_into('<I', hdr, 0x18, 44100 + 64)
    struct.pack_into('<I', hdr, 0x2c, 7670453)    # YM2612 clock
    struct.pack_into('<I', hdr, 0x34, 0x40 - 0x34)
    return bytes(hdr) + data


# made file: (source under tests/fixtures, maker)
MAKERS = {
    'picok.flac': ('f1/sort.flac', flac_pic),
    'picneg.flac': ('f1/sort.flac', flac_picneg),
    'sirate0.flac': ('f1/sort.flac', flac_sirate0),
    'sibig.flac': ('f1/sort.flac', flac_sibig),
    'varblock.flac': ('f1/sort.flac', flac_varblock),
    'badframe.flac': ('f1/sort.flac', flac_badframe),
    'picback.flac': ('f1/sort.flac', flac_picback),
    'picpng.flac': ('f1/sort.flac', flac_picpng),
    'apicunsync.mp3': ('plain.mp3', mp3_apicunsync),
    'infoneg.wav': (None, wav_infoneg),
    'infolong.wav': (None, wav_infolong),
    'pcm4.wav': (None, wav_pcm4),
    'apic16.mp3': ('plain.mp3', mp3_apic16),
    'apic16e.mp3': ('plain.mp3', mp3_apic16e),
    'id3first.aiff': (None, aiff_id3first),
    'textlong.aiff': (None, aiff_textlong),
    'cjk.vgm': (None, vgm_cjk),
    'pcmseek.vgm': (None, vgm_pcmseek),
    'm4a64.m4a': ('upfix/base.m4a', m4a_mdat64),
    'apple64.m4b': ('apple_chapters.m4b', m4a_mdat64),
    'nero64.m4b': ('nero_chapters.m4b', m4a_mdat64),
    'gap.m4a': ('upfix/base.m4a', m4a_gap),
    'co64.m4a': ('upfix/base.m4a', m4a_co64),
    'long.m4a': ('upfix/base.m4a', m4a_long),
    'clamp.m4a': ('upfix/base.m4a', m4a_clamp),
    'sttsbig.m4a': ('upfix/base.m4a', m4a_sttsbig),
}


def main(argv):
    if len(argv) == 3 and argv[1] == 'tree':
        tree(parse(open(argv[2], 'rb').read()))
        return 0
    if len(argv) == 2 and argv[1] == 'artpos':
        print(art_pos(14), art_pos(4), len(JPEG))
        return 0
    if len(argv) == 3 and argv[1] == 'newpos':
        base = open(os.path.join(argv[2], 'f1/sort.flac'), 'rb').read()
        back = [picture_of(4, b'image/jpeg', JPEG),
                picture_of(3, b'image/jpeg', JPEG)]
        png = [picture_of(3, b'image/png', PNG),
               picture_of(3, b'image/jpeg', JPEG)]
        print(flac_pic_pos(base, back, 1), flac_pic_pos(base, png, 1),
              apicunsync_pos())
        return 0
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, out = argv[1], argv[2]
    names = argv[3:] or sorted(MAKERS)
    os.makedirs(out, exist_ok=True)
    for n in names:
        base, fn = MAKERS[n]
        if base is None:
            data = fn(None)
        else:
            with open(os.path.join(src, base), 'rb') as f:
                data = fn(f.read())
        with open(os.path.join(out, n), 'wb') as f:
            f.write(data)
    print('made %d' % len(names))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
