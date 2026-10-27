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

"""Host test of bcmfw/ovlpair.py, the overlays' packaging check (#126;
visualizer.md section 8.4; album-render.md section 5.3): the committed
pairs pass (bcm-devtest-images.c's jobs_display and bcmdt_viz_ovl with
bcmfw/video/viz-ovl.bin; bcmvideo_image.h's BCMVIDEO_DECOVL_* with
bcmvideo-jobs.bin and dec-ovl.bin; BCMVIDEO_VIZOVL_* with
bcmvideo-jobs.bin and viz-ovl-m.bin), and each kind of mismatch is refused
with its reason: the overlay's ABI word, its bytes, its region, the
embedded image, the image's own ABI word (with the record made to agree,
so only the pairing itself is wrong), a record whose ABI moved with the
file but not the image, a truncated file, and a missing record; and the
two visualizer files never pass for each other (each is linked against
its own image).
Prints FAIL lines, then "ovlpair: N cases, M failed"."""
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'bcmfw'))
import ovlpair as P  # noqa: E402

IMAGES = os.path.join(ROOT, 'firmware', 'target', 'arm', 'ipod', 'video',
                      'bcm-devtest-images.c')
OVL = os.path.join(ROOT, 'bcmfw', 'video', 'viz-ovl.bin')
VIDEO_H = os.path.join(ROOT, 'apps', 'video', 'bcmvideo_image.h')
MERGED = os.path.join(ROOT, 'bcmfw', 'video', 'bcmvideo-jobs.bin')
DEC = os.path.join(ROOT, 'bcmfw', 'video', 'dec-ovl.bin')
VIZM = os.path.join(ROOT, 'bcmfw', 'video', 'viz-ovl-m.bin')

cases = failed = 0


def expect(name, text, ovl, want, image=None, prefix='DECOVL'):
    """want: None (passes) or a fragment of a reason; image: the merged
    image's bytes (its overlays' pairs: text is the video header, prefix
    DECOVL or VIZOVL)"""
    global cases, failed
    cases += 1
    why = P.check(text, ovl) if image is None else P.check_dec(text, image, ovl, prefix)
    ok = not why if want is None else any(want in w for w in why)
    if not ok:
        failed += 1
        print('FAIL %s: %s' % (name, why or 'passed'))


def image_bytes(text):
    return P.parse(text)[0]


def with_image(text, data):
    """the images file with jobs_display's bytes replaced (same length)"""
    m = re.search(r'(static const uint8_t jobs_display_data\[\d+\][^{]*\{)([^}]*)(\};)', text)
    pad = data + b'\0' * (-len(data) % 4)
    body = '\n' + ''.join('    ' + ' '.join('0x%02x,' % b for b in pad[k:k + 16]) + '\n'
                          for k in range(0, len(pad), 16))
    return text[:m.start(2)] + body + text[m.end(2):]


def with_record(text, rec):
    return re.sub(r'(const struct bcmdt_ovl bcmdt_viz_ovl =\s*\{)[^}]*(\};)',
                  lambda m: m.group(1) + '\n    %d, 0x%08x, 0x%08x, %d, 0x%08x, 0x%x\n' % tuple(rec) +
                  m.group(2), text)


def main():
    text = open(IMAGES).read()
    ovl = open(OVL, 'rb').read()
    image, rec = P.parse(text)
    size, ofnv, abi, abi_at, ifnv, base = rec
    expect('the committed pair', text, ovl, None)
    # the overlay's header ABI word (another image build's)
    bad = ovl[:4] + struct.pack('<I', abi ^ 0x100) + ovl[8:]
    expect('ABI word flipped', text, bad, 'ABI word')
    # one byte of the code: the record's FNV
    k = len(ovl) // 2
    expect('a byte changed', text, ovl[:k] + bytes([ovl[k] ^ 1]) + ovl[k + 1:], 'FNV-1a')
    expect('truncated', text, ovl[:-4], 'bytes')
    expect('no header', text, ovl[:16], 'no header')
    # the embedded image changed: its FNV against the record
    im = bytearray(image)
    im[100] ^= 1
    expect('the image changed', with_image(text, bytes(im)), ovl, 'embedded jobs_display')
    # the image's own ABI word changed and the record made to agree with
    # it: only the pairing is wrong (another image build behind the record)
    im = bytearray(image)
    im[abi_at] ^= 0x10
    t2 = with_record(with_image(text, bytes(im)), [size, ofnv, abi, abi_at, P.fnv(bytes(im)),
                                                   base])
    expect('the image takes another ABI', t2, ovl, 'takes ABI')
    # the file and the record moved together to another ABI; the image not
    bad = ovl[:4] + struct.pack('<I', abi ^ 0x100) + ovl[8:]
    t3 = with_record(text, [size, P.fnv(bad), abi ^ 0x100, abi_at, ifnv, base])
    expect('file and record moved, image not', t3, bad, 'takes ABI')
    expect('no record', re.sub(r'const struct bcmdt_ovl bcmdt_viz_ovl', 'X', text), ovl,
           'no bcmdt_viz_ovl')
    # linked at another region (the merged image's): the record's base
    t4 = with_record(text, [size, ofnv, abi, abi_at, ifnv, base ^ 0x10000])
    expect('another region', t4, ovl, 'region')

    # DECODE's pair: the merged image, its header's record, dec-ovl.bin
    vh = open(VIDEO_H).read()
    merged = open(MERGED, 'rb').read()
    dec = open(DEC, 'rb').read()
    dabi = P.parse_dec(vh)[4]
    expect('dec: the committed pair', vh, dec, None, merged)
    bad = dec[:4] + struct.pack('<I', dabi ^ 0x100) + dec[8:]
    expect('dec: ABI word flipped', vh, bad, 'ABI word', merged)
    k = len(dec) // 2
    expect('dec: a byte changed', vh, dec[:k] + bytes([dec[k] ^ 1]) + dec[k + 1:], 'FNV-1a',
           merged)
    expect('dec: truncated', vh, dec[:-4], 'bytes', merged)
    im = bytearray(merged)
    im[200] ^= 1
    expect('dec: the image changed', vh, dec, 'embedded bcmvideo-jobs.bin', bytes(im))
    expect('dec: the image truncated', vh, dec, 'bcmvideo-jobs.bin is', merged[:-4])
    dat = P.parse_dec(vh)[5]
    im = bytearray(merged)
    im[dat] ^= 0x10
    vh2 = re.sub(r'(#define BCMVIDEO_MERGED_FNV\s+)0x[0-9a-f]+', lambda m: m.group(1) +
                 '0x%08x' % P.fnv(bytes(im)), vh)
    expect('dec: the image takes another ABI', vh2, dec, 'takes ABI', bytes(im))
    vh3 = re.sub(r'(#define BCMVIDEO_DECOVL_BASE\s+)0x[0-9a-f]+', lambda m: m.group(1) +
                 '0x13b000', vh)
    expect('dec: another region', vh3, dec, 'region', merged)
    expect('dec: no record', vh.replace('BCMVIDEO_DECOVL_FNV', 'X'), dec, 'no BCMVIDEO',
           merged)
    # the visualizer's pair for the merged image (visualizer.md 8.5)
    vizm = open(VIZM, 'rb').read()
    vabi = P.parse_dec(vh, 'VIZOVL')[4]
    expect('viz-m: the committed pair', vh, vizm, None, merged, 'VIZOVL')
    expect('viz-m: the same ABI as DECODE\'s', vh, vizm, None if vabi == dabi else 'x',
           merged, 'VIZOVL')
    bad = vizm[:4] + struct.pack('<I', vabi ^ 0x100) + vizm[8:]
    expect('viz-m: ABI word flipped', vh, bad, 'ABI word', merged, 'VIZOVL')
    k = len(vizm) // 2
    expect('viz-m: a byte changed', vh, vizm[:k] + bytes([vizm[k] ^ 1]) + vizm[k + 1:],
           'FNV-1a', merged, 'VIZOVL')
    im = bytearray(merged)
    im[200] ^= 1
    expect('viz-m: the image changed', vh, vizm, 'embedded bcmvideo-jobs.bin', bytes(im),
           'VIZOVL')
    expect('viz-m: no record', vh.replace('BCMVIDEO_VIZOVL_FNV', 'X'), vizm, 'no BCMVIDEO',
           merged, 'VIZOVL')
    # the two visualizer files never pass for each other, nor DECODE's
    expect('viz-m: the display-only file', vh, ovl, 'region', merged, 'VIZOVL')
    expect('viz-m: DECODE\'s file', vh, dec, 'FNV-1a', merged, 'VIZOVL')
    expect('viz: the merged image\'s file', text, vizm, 'region')
    # the command line: 0 for the pair, 1 for a mismatch (make zip's rule)
    tmp = os.path.join(ROOT, 'build', 'ovlpair')
    os.makedirs(tmp, exist_ok=True)
    with open(os.path.join(tmp, 'bad.bin'), 'wb') as f:
        f.write(ovl[:4] + struct.pack('<I', abi ^ 0x100) + ovl[8:])
    for name, path, rc in (('cli pair', OVL, 0), ('cli mismatch', os.path.join(tmp, 'bad.bin'), 1)):
        global cases, failed
        cases += 1
        got = subprocess.run([sys.executable, os.path.join(ROOT, 'bcmfw', 'ovlpair.py'),
                              IMAGES, path], capture_output=True).returncode
        if got != rc:
            failed += 1
            print('FAIL %s: exit %d, want %d' % (name, got, rc))
    for name, path, rc in (('cli dec pair', DEC, 0), ('cli dec mismatch',
                                                       os.path.join(tmp, 'bad.bin'), 1)):
        cases += 1
        got = subprocess.run([sys.executable, os.path.join(ROOT, 'bcmfw', 'ovlpair.py'),
                              '--dec', VIDEO_H, MERGED, path], capture_output=True).returncode
        if got != rc:
            failed += 1
            print('FAIL %s: exit %d, want %d' % (name, got, rc))
    for name, path, rc in (('cli viz-m pair', VIZM, 0), ('cli viz-m mismatch', OVL, 1)):
        cases += 1
        got = subprocess.run([sys.executable, os.path.join(ROOT, 'bcmfw', 'ovlpair.py'),
                              '--viz-m', VIDEO_H, MERGED, path], capture_output=True).returncode
        if got != rc:
            failed += 1
            print('FAIL %s: exit %d, want %d' % (name, got, rc))
    print('ovlpair: %d cases, %d failed' % (cases, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
