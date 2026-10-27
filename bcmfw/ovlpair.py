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

"""The packaging rule for the overlays (#126; visualizer.md section 8.4;
album-render.md section 5.3): an overlay file ships only beside the image
it was linked against.

The visualizer's (viz-ovl.bin, the display-only image's): read from the
firmware's embedded images (the generated bcm-devtest-images.c, no
VideoCore toolchain needed) and the file, it checks that:

  - the file is the one the record describes (bcmdt_viz_ovl: its size and
    FNV-1a), and its header's magic is 'OVL1';
  - the header's ABI word (vc2cc __vc2_abi over the image's imports) is the
    record's, and its base (word 2) the record's region;
  - the embedded jobs_display image is the record's (its FNV-1a), and its
    own copy of the ABI word (the overlay stub's compare, at the record's
    offset) is the header's: the image being shipped accepts the overlay.

DECODE's (dec-ovl.bin, the merged video image's; --dec): the same checks
against mkvideo.py's record in apps/video/bcmvideo_image.h
(BCMVIDEO_DECOVL_*) and the merged image file it describes
(bcmvideo-jobs.bin: BCMVIDEO_MERGED_BYTES and _FNV).

The visualizer's for the merged image (viz-ovl-m.bin; --viz-m): the same
checks against BCMVIDEO_VIZOVL_* (visualizer.md section 8.5: it loads
into the region DECODE's overlay uses, one resident at a time).

Any mismatch prints why and exits 1, so make zip (tools/buildzip.pl) fails
instead of shipping a pair the image would refuse (HP_E_VERSION) or, worse,
a pair whose record and image disagree.

Usage: bcmfw/ovlpair.py IMAGES_C VIZ_OVL_BIN
       bcmfw/ovlpair.py --dec VIDEO_H MERGED_BIN DEC_OVL_BIN
       bcmfw/ovlpair.py --viz-m VIDEO_H MERGED_BIN VIZ_OVL_M_BIN
"""
import re
import struct
import sys

MAGIC = 0x4F564C31          # jobs.h J_OVL_MAGIC, 'OVL1'
IMAGE = 'jobs_display'      # mkdevtest.py OVL_IMAGE


def fnv(data):
    v = 2166136261
    for b in data:
        v = ((v ^ b) * 16777619) & 0xffffffff
    return v


def parse(text):
    """(the image's bytes, the record's words) from bcm-devtest-images.c"""
    m = re.search(r'static const uint8_t %s_data\[(\d+)\][^{]*\{([^}]*)\};' % IMAGE, text)
    if not m:
        raise ValueError('no %s_data in the images file' % IMAGE)
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(2)))
    if len(data) != int(m.group(1)):
        raise ValueError('%s_data: %d bytes, declared %s' % (IMAGE, len(data), m.group(1)))
    m = re.search(r'const struct bcmdt_image bcmdt_%s_image =\s*\{\s*%s_data,\s*(\d+),' %
                  (IMAGE, IMAGE), text)
    if not m:
        raise ValueError('no bcmdt_%s_image' % IMAGE)
    data = data[:int(m.group(1))]
    m = re.search(r'const struct bcmdt_ovl bcmdt_viz_ovl =\s*\{([^}]*)\};', text)
    if not m:
        raise ValueError('no bcmdt_viz_ovl record')
    rec = [int(x, 0) for x in m.group(1).replace(',', ' ').split()]
    if len(rec) != 6:
        raise ValueError('bcmdt_viz_ovl: %d words, want 6' % len(rec))
    return data, rec


def pair(name, image, image_name, ovl, size, ofnv, abi, abi_at, ifnv, base):
    """[] if the overlay ovl pairs with image, else the reasons"""
    why = []
    if len(ovl) != size or fnv(ovl) != ofnv:
        why.append('%s is %d bytes, FNV-1a %08x; the record says %d, %08x' % (
            name, len(ovl), fnv(ovl), size, ofnv))
    if len(ovl) < 28:
        return why + ['%s has no header' % name]
    magic, habi, hbase = struct.unpack('<3I', ovl[:12])
    if magic != MAGIC:
        why.append('%s header magic %08x, want %08x' % (name, magic, MAGIC))
    if habi != abi:
        why.append('%s ABI word %08x, the record\'s %08x' % (name, habi, abi))
    if hbase != base:
        why.append('%s linked at 0x%x, the record\'s region 0x%x' % (name, hbase, base))
    if fnv(image) != ifnv:
        why.append('the embedded %s is FNV-1a %08x, the record\'s %08x' % (
            image_name, fnv(image), ifnv))
    if abi_at + 4 > len(image):
        why.append('the ABI word\'s offset %d is past the image' % abi_at)
    else:
        iabi = struct.unpack('<I', image[abi_at:abi_at + 4])[0]
        if iabi != habi:
            why.append('the shipped %s takes ABI %08x, %s has %08x' % (
                image_name, iabi, name, habi))
    return why


def check(text, ovl):
    """[] if the visualizer's pair may ship, else the reasons"""
    try:
        image, (size, ofnv, abi, abi_at, ifnv, base) = parse(text)
    except ValueError as e:
        return [str(e)]
    return pair('viz-ovl.bin', image, IMAGE, ovl, size, ofnv, abi, abi_at, ifnv, base)


DEC_KEYS = ('BCMVIDEO_MERGED_BYTES', 'BCMVIDEO_MERGED_FNV', 'BCMVIDEO_DECOVL_BYTES',
            'BCMVIDEO_DECOVL_FNV', 'BCMVIDEO_DECOVL_ABI', 'BCMVIDEO_DECOVL_ABI_AT',
            'BCMVIDEO_DECOVL_BASE')


def parse_dec(text, prefix='DECOVL'):
    """the record's values from bcmvideo_image.h, by DEC_KEYS (prefix
    DECOVL, or VIZOVL for the visualizer's)"""
    out = []
    for k in DEC_KEYS:
        k = k.replace('DECOVL', prefix)
        m = re.search(r'^#define\s+%s\s+(0x[0-9a-fA-F]+|\d+)u?\b' % k, text, re.M)
        if not m:
            raise ValueError('no %s in the video header' % k)
        out.append(int(m.group(1), 0))
    return out


def check_dec(text, image, ovl, prefix='DECOVL', name='dec-ovl.bin'):
    """[] if DECODE's overlay (or, prefix VIZOVL, the visualizer's) may
    ship beside this merged image"""
    try:
        ibytes, ifnv, size, ofnv, abi, abi_at, base = parse_dec(text, prefix)
    except ValueError as e:
        return [str(e)]
    why = []
    if len(image) != ibytes:
        why.append('bcmvideo-jobs.bin is %d bytes, the header says %d' % (len(image), ibytes))
    return why + pair(name, image, 'bcmvideo-jobs.bin', ovl, size, ofnv, abi,
                      abi_at, ifnv, base)


def main(argv):
    if argv[:1] in (['--dec'], ['--viz-m']) and len(argv) == 4:
        with open(argv[1]) as f:
            text = f.read()
        with open(argv[2], 'rb') as f:
            image = f.read()
        with open(argv[3], 'rb') as f:
            ovl = f.read()
        name = 'dec-ovl.bin' if argv[0] == '--dec' else 'viz-ovl-m.bin'
        why = check_dec(text, image, ovl, 'DECOVL' if argv[0] == '--dec' else 'VIZOVL', name)
        fix = 'bcmfw/mkvideo.py'
    elif len(argv) == 2:
        with open(argv[0]) as f:
            text = f.read()
        with open(argv[1], 'rb') as f:
            ovl = f.read()
        why = check(text, ovl)
        name, fix = 'viz-ovl.bin', 'bcmfw/mkdevtest.py'
    else:
        print('\n'.join(__doc__.strip().splitlines()[-2:]), file=sys.stderr)
        return 2
    for w in why:
        print('ovlpair: %s' % w, file=sys.stderr)
    if why:
        print('ovlpair: refusing to package %s (run %s)' % (name, fix), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
