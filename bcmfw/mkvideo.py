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
"""Build the two video images and write:

  bcmfw/video/bcmvideo.bin       the plain image (BCMFW_VIDEO=1 and the
                                 shipped flags): the decoder plus the
                                 brief probes (HP_OP_PEEK ... HP_OP_HIB)
  bcmfw/video/bcmvideo-jobs.bin  the merged image: the same flags plus
                                 BCMFW_JOBS=1 (the display jobs; no room
                                 for the probes, which answer HP_E_UNKNOWN)
  bcmfw/video/dec-ovl.bin        DECODE's overlay for the merged image
                                 (video/dec_ovl.c, linked at its region
                                 against its map; album-render.md 5.3);
                                 make zip ships it beside the image
  bcmfw/video/viz-ovl-m.bin      the visualizer's overlay for the merged
                                 image (video/viz_ovl.c, the same region
                                 and exports as DECODE's: one resident at
                                 a time; visualizer.md 8.5); make zip
                                 ships it beside the image too
  apps/video/bcmprobe_image.h    the plain image's size and FNV-1a
  apps/video/bcmvideo_image.h    the merged image's, the player's choice
                                 between the two, and dec-ovl.bin's
                                 record (BCMVIDEO_DECOVL_*: size, FNV-1a,
                                 ABI word, the image's copy of it, the
                                 region's base; bcmfw/ovlpair.py --dec),
                                 and viz-ovl-m.bin's (BCMVIDEO_VIZOVL_*;
                                 ovlpair.py --viz-m)
  apps/video/bcmvideo_clock.h    verified.txt's clock150 tokens: the
                                 video images a unit played at 150 MHz
                                 (vclock_gate.h, the release's clock)

Each image is padded to a whole word.  On the device:

  /.rockbox/bcmprobe.bin  always the plain image: the developer probe
                          rows (vplayer_probe) and the E2 lab boot it
  /.rockbox/bcmvideo.bin  the player's image (playback, the resident
                          rung, the stage and bench labs): the plain image
                          by default; the merged one in a build configured
                          with --bcmfw-merged (HAVE_BCMFW_MERGED_VIDEO),
                          the bench build that verifies it.  The default
                          moves to the merged image once bcmfw/verified.txt
                          lists its FNV (memory-contract.md section 6).

vplayer.c and devbcm.c refuse a file whose size or FNV-1a differs from
the header's.  Everything here is committed, so a firmware build and
`make zip` need no VideoCore toolchain; buildzip.pl copies the images into
.rockbox when the build has HAVE_BCMFW_VIDEO.  --check rebuilds both and
fails if any committed file differs (as mkdevtest.py --check does for the
devtest images).

Usage: bcmfw/mkvideo.py [--check]
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build as B  # noqa: E402
from mkdevtest import fnv, tokens  # noqa: E402

# p14-resident: the image may also be the display driver between videos
# (the boot path's resident rung), so it can sleep its core while idle,
# but only when the host turns it on (BCMFW_IDLE_GATED: never during a
# session, and off until the developer setting says so).  DEFS is the
# plain image's (devtest_test.py builds the resident rung's image from it)
DEFS = ['BCMFW_VIDEO=1', 'BCMFW_IDLE_SLEEP=1', 'BCMFW_IDLE_GATED=1',
        'BCMFW_IDLE_DOORBELL=1']
JOBS_DEFS = DEFS + ['BCMFW_JOBS=1']

PLAIN_BIN = os.path.join(HERE, 'video', 'bcmvideo.bin')
JOBS_BIN = os.path.join(HERE, 'video', 'bcmvideo-jobs.bin')
DEC_BIN = os.path.join(HERE, 'video', 'dec-ovl.bin')
VIZ_BIN = os.path.join(HERE, 'video', 'viz-ovl-m.bin')
PROBE_H = os.path.join(ROOT, 'apps', 'video', 'bcmprobe_image.h')
VIDEO_H = os.path.join(ROOT, 'apps', 'video', 'bcmvideo_image.h')
CLOCK_H = os.path.join(ROOT, 'apps', 'video', 'bcmvideo_clock.h')

PROBE_HEADER = """/* Generated by bcmfw/mkvideo.py: do not edit.  The plain video image
 * (%s): the decoder and the brief
 * probes.  The developer probe rows load it from /.rockbox/bcmprobe.bin,
 * and the player from /.rockbox/bcmvideo.bin unless the build has
 * HAVE_BCMFW_MERGED_VIDEO (bcmvideo_image.h); a file whose size or
 * FNV-1a differs is refused.
 * sha256 %s */
#ifndef BCMPROBE_IMAGE_H
#define BCMPROBE_IMAGE_H
#define BCMPROBE_IMAGE_BYTES %d
#define BCMPROBE_IMAGE_FNV   0x%08xu
#endif
"""

VIDEO_HEADER = """/* Generated by bcmfw/mkvideo.py: do not edit.  The player's video
 * image, /.rockbox/bcmvideo.bin: the merged image
 * (%s)
 * with HAVE_BCMFW_MERGED_VIDEO (configure --bcmfw-merged), else the plain
 * one (bcmprobe_image.h).  vplayer.c refuses a file whose size or FNV-1a
 * differs.
 * merged sha256 %s */
#ifndef BCMVIDEO_IMAGE_H
#define BCMVIDEO_IMAGE_H
#include "bcmprobe_image.h"
#define BCMVIDEO_MERGED_BYTES %d
#define BCMVIDEO_MERGED_FNV   0x%08xu
/* dec-ovl.bin, DECODE's overlay linked against the merged image
 * (album-render.md 5.3; bcmjob_pp.h bcmjob_dec_ops): its bytes, FNV-1a,
 * ABI word, the word's offset in the image, the region's base.
 * sha256 %s */
#define BCMVIDEO_DECOVL_BYTES  %d
#define BCMVIDEO_DECOVL_FNV    0x%08xu
#define BCMVIDEO_DECOVL_ABI    0x%08xu
#define BCMVIDEO_DECOVL_ABI_AT %d
#define BCMVIDEO_DECOVL_BASE   0x%x
/* viz-ovl-m.bin, the visualizer's overlay linked against the merged
 * image, in the same region (one overlay resident at a time; visualizer.md
 * 8.5; bcmjob_pp.h BCMJOB_OVL_VIZ): the same five words.
 * sha256 %s */
#define BCMVIDEO_VIZOVL_BYTES  %d
#define BCMVIDEO_VIZOVL_FNV    0x%08xu
#define BCMVIDEO_VIZOVL_ABI    0x%08xu
#define BCMVIDEO_VIZOVL_ABI_AT %d
#define BCMVIDEO_VIZOVL_BASE   0x%x
#ifdef HAVE_BCMFW_MERGED_VIDEO
#define BCMVIDEO_IMAGE_BYTES BCMVIDEO_MERGED_BYTES
#define BCMVIDEO_IMAGE_FNV   BCMVIDEO_MERGED_FNV
#define BCMVIDEO_IMAGE_KIND  "merged"
#else
#define BCMVIDEO_IMAGE_BYTES BCMPROBE_IMAGE_BYTES
#define BCMVIDEO_IMAGE_FNV   BCMPROBE_IMAGE_FNV
#define BCMVIDEO_IMAGE_KIND  "plain"
#endif
#endif
"""


CLOCK_HEADER = """/* Generated by bcmfw/mkvideo.py from bcmfw/verified.txt: do not edit.
 * The clock150 tokens: the FNV-1a of each video image that played stage 6
 * at 150 MHz on a real unit after stage 4c passed (vclock_gate.h). */
#ifndef BCMVIDEO_CLOCK_H
#define BCMVIDEO_CLOCK_H
#include <stdint.h>
static const uint32_t bcmvideo_clock150_fnv[%d] =
{
%s};
#define BCMVIDEO_N_CLOCK150 %d
#endif
"""


def clock_header():
    toks = tokens('clock150')
    rows = ''.join('    0x%08xu,   /* %s */\n' % (v, note.split(':', 1)[0])
                   for v, note in toks) or '    0           /* none yet */\n'
    return CLOCK_HEADER % (max(1, len(toks)), rows, len(toks))


def image(name, defs, ovl=False):
    """the image's bytes (padded to a word); ovl: and its two overlays'
    [(bytes, record)], DECODE's then the visualizer's (record: size,
    FNV-1a, ABI word, the image's copy of the word, the region's base)"""
    out = os.path.join(ROOT, 'build', 'bcmfw', name)
    prog, _ = B.build(out, defs, listing=False)
    data = prog.load_image()
    data += b'\0' * (-len(data) % 4)
    if not ovl:
        return data
    w = prog.abi.to_bytes(4, 'little')
    at = [i for i in range(len(data) - 3) if data[i:i + 4] == w]
    if len(at) != 1:
        raise SystemExit('mkvideo: the ABI word %08x is at %d places in the merged '
                         'image, want 1' % (prog.abi, len(at)))
    ovls = []
    for kind, stem in (('decode', 'dec-ovl'), ('viz', 'viz-ovl')):
        oprog, _ = B.build_overlay(prog, out, defs, listing=False, kind=kind)
        odata = open(os.path.join(out, stem + '.bin'), 'rb').read()
        if oprog.abi != prog.abi:
            raise SystemExit('mkvideo: %s ABI %08x, image %08x' % (stem, oprog.abi, prog.abi))
        ovls.append((odata, (len(odata), fnv(odata), prog.abi, at[0],
                             B.config_value('BCMFW_JOBS_OVL', defs))))
    return data, ovls


def defs_text(defs):
    return ' '.join('-D ' + d for d in defs)


def outputs():
    """[(path, bytes)] for every generated file"""
    plain = image('video', DEFS)
    jobs, ((dec, rec), (viz, vrec)) = image('video-jobs', JOBS_DEFS, ovl=True)
    probe_h = PROBE_HEADER % (defs_text(DEFS), hashlib.sha256(plain).hexdigest(),
                              len(plain), fnv(plain))
    video_h = VIDEO_HEADER % ((defs_text(JOBS_DEFS),
                               hashlib.sha256(jobs).hexdigest(),
                               len(jobs), fnv(jobs), hashlib.sha256(dec).hexdigest())
                              + rec + (hashlib.sha256(viz).hexdigest(),) + vrec)
    conv = lambda t: prokbox_header.convert(t, '.h')[0].encode()
    return [(PLAIN_BIN, plain), (JOBS_BIN, jobs), (DEC_BIN, dec), (VIZ_BIN, viz),
            (PROBE_H, conv(probe_h)), (VIDEO_H, conv(video_h)),
            (CLOCK_H, conv(clock_header()))]


def main(argv):
    if not B.have_gcc():
        print('mkvideo: vc4-elf-gcc not found (set VC4_GCC)', file=sys.stderr)
        return 2
    outs = outputs()
    if '--check' in argv:
        stale = [p for p, d in outs
                 if not os.path.exists(p) or open(p, 'rb').read() != d]
        if stale:
            print('mkvideo: stale: %s; run bcmfw/mkvideo.py' % ', '.join(
                os.path.relpath(p, ROOT) for p in stale), file=sys.stderr)
            return 1
        for p, d in outs[:4]:
            print('mkvideo: %s up to date (%d bytes, fnv %08x)' % (
                os.path.basename(p), len(d), fnv(d)))
        return 0
    for p, d in outs:
        # an unchanged file is left alone (a frozen image is never rewritten)
        if os.path.exists(p) and open(p, 'rb').read() == d:
            continue
        with open(p, 'wb') as f:
            f.write(d)
    for p, d in outs[:4]:
        print('mkvideo: %s %d bytes, fnv %08x, sha256 %s' % (
            os.path.basename(p), len(d), fnv(d), hashlib.sha256(d).hexdigest()))
    return 0


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
sys.path.insert(0, os.path.join(HERE, '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
