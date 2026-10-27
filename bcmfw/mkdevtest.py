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
"""Build the bcmfw images the BCM device test embeds and write
firmware/target/arm/ipod/video/bcm-devtest-images.c.

  probe     BCMFW_PROBE=1 (stage 1; no panel traffic)
  hello     default build (stage 2)
  hello_rb  BCMFW_RESET_BRANCH=1 (stage 2 when the probe saw the core
            start at address 0)
  hello_dma     BCMFW_LCD_DMA=1 (stage 3)
  hello_dma_rb  BCMFW_LCD_DMA=1 BCMFW_RESET_BRANCH=1 (stage 3, entry at 0)
  bench     BCMFW_BENCH=1 (stage 4: the timing bench; a probe build, so
            either entry point works)
  bench_pll BCMFW_BENCH=1 BCMFW_BENCH_PLL=1 (stage 4b: the same, then the
            150 MHz switch and a second table; optional, owner's go)
  hello_dma_clk BCMFW_LCD_DMA=1 BCMFW_CLOCK_CMD=1 (stage 4c, dev9: the DMA
            image plus CMD_CLOCK, the 150 MHz switch with the panel)
  drift     BCMFW_DRIFT=1 (stage 5b: the DMA image plus the drift ops,
            the main menu's panel drift on the BCM; both modes run it)
  hello_dma_idle BCMFW_LCD_DMA=1 BCMFW_IDLE_SLEEP=1 BCMFW_IDLE_DOORBELL=1
            (stage 5i: the DMA image asleep between commands, woken by the
            1 ms tick and, if it fires, the host doorbell)
  hello_dma_sby BCMFW_LCD_DMA=1 BCMFW_IDLE_SLEEP=1 BCMFW_IDLE_DOORBELL=1
            BCMFW_IDLE_GATED=1 BCMFW_LCD_STANDBY=1 (stage 5s, the sleep
            spikes: the idle image plus panel standby; it sleeps only
            when the host turns sleep on, which stage 5s does only after
            a stage 5i pass)
  hello_3d  BCMFW_R3D=1 BCMFW_CLOCK_CMD=1 (stage 3d: the 3D render test,
            a PS1-style renderer on the core with four built-in scenes,
            plus CMD_CLOCK so a run can go at 150 MHz)
  jobs_display  BCMFW_HOSTPROTO=1 BCMFW_JOBS=1 (bench row B13, the warm
            swap: the display-only image, swapped with the video image
            and back; docs/research/bcm2722/warm-swap.md)
  viz-ovl   not an image: the visualizer's overlay (#126; build.py
            build_overlay), linked against jobs_display's map and written
            to bcmfw/video/viz-ovl.bin (make zip ships it beside the
            firmware: tools/buildzip.pl), with its record bcmdt_viz_ovl
            (size, FNV-1a, ABI word, the image's own copy of the word and
            the image's FNV-1a) for the job driver's checks and
            bcmfw/ovlpair.py's
  hibstub   not an image: the hibernate park's stub (bcmfw/hibstub.c,
            linked at HW_STUB; bcmfw/hibwake.h, power-contract.md section
            6.6), which the PP uploads beside either jobs image.  Not for
            silicon until PW5 passes on the unit

The HAVE_BCMFW boot path uses the same images: a stage's pass flag names
the image that passed and its FNV-1a, and the boot path takes only that
image, only if the embedded copy still matches.

bcmfw/verified.txt lists the images that passed stage 5 on a device; it
is compiled in as bcmdt_verified_fnv[] for the default boot path.

A build without the device test (the release boot path: --bcmfw without
--bcmfw-devtest) needs only the images in RELEASE; the others are
emitted under #ifdef HAVE_BCMFW_DEVTEST.

The generated file is committed, so a firmware build needs no VideoCore
toolchain.  --check rebuilds and fails if the committed file differs
(the bench runs it).

Usage: bcmfw/mkdevtest.py [--check]
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build as B  # noqa: E402

VERIFIED = os.path.join(HERE, 'verified.txt')
OUT_C = os.path.join(ROOT, 'firmware', 'target', 'arm', 'ipod', 'video',
                     'bcm-devtest-images.c')
OUT_OVL = os.path.join(HERE, 'video', 'viz-ovl.bin')
OVL_IMAGE = 'jobs_display'      # the image the overlay is linked against
IMAGES = [('probe', ['BCMFW_PROBE=1']),
          ('hello', []),
          ('hello_rb', ['BCMFW_RESET_BRANCH=1']),
          ('hello_dma', ['BCMFW_LCD_DMA=1']),
          ('hello_dma_rb', ['BCMFW_LCD_DMA=1', 'BCMFW_RESET_BRANCH=1']),
          ('bench', ['BCMFW_BENCH=1']),
          ('bench_pll', ['BCMFW_BENCH=1', 'BCMFW_BENCH_PLL=1']),
          ('hello_dma_clk', ['BCMFW_LCD_DMA=1', 'BCMFW_CLOCK_CMD=1']),
          ('drift', ['BCMFW_DRIFT=1']),
          ('hello_dma_idle', ['BCMFW_LCD_DMA=1', 'BCMFW_IDLE_SLEEP=1',
                              'BCMFW_IDLE_DOORBELL=1']),
          ('hello_dma_sby', ['BCMFW_LCD_DMA=1', 'BCMFW_IDLE_SLEEP=1',
                             'BCMFW_IDLE_DOORBELL=1', 'BCMFW_IDLE_GATED=1',
                             'BCMFW_LCD_STANDBY=1']),
          ('hello_3d', ['BCMFW_R3D=1', 'BCMFW_CLOCK_CMD=1']),
          ('jobs_display', ['BCMFW_HOSTPROTO=1', 'BCMFW_JOBS=1'])]
# The images a build without the device test links: the default boot
# path's DMA rung (bcmfw-boot.c; the one verified.txt lists)
RELEASE = {'hello_dma'}

HEADER = '''/***************************************************************************
 *             __________               __   ___.
 *   Open      \\______   \\ ____   ____ |  | _\\_ |__   _______  ___
 *   Source     |       _//  _ \\_/ ___\\|  |/ /| __ \\ /  _ \\  \\/  /
 *   Jukebox    |    |   (  <_> )  \\___|    < | \\_\\ (  <_> > <  <
 *   Firmware   |____|_  /\\____/ \\___  >__|_ \\|___  /\\____/__/\\_ \\
 *                     \\/            \\/     \\/    \\/            \\/
 *
 * GENERATED by bcmfw/mkdevtest.py from bcmfw/ (our own VideoCore II
 * source, GPL).  Do not edit.  The BCM device test's images.
 *
 * Copyright (C) 2026 ProkBox contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "bcm-devtest.h"
'''


def fnv(data):
    v = 2166136261
    for b in data:
        v = ((v ^ b) * 16777619) & 0xffffffff
    return v


def abi_at(image, abi):
    """the byte offset of the image's one copy of the ABI word (the
    overlay stub's 48-bit immediate: jobs_bcm.c jref_ext)"""
    w = abi.to_bytes(4, 'little')
    at = [i for i in range(len(image) - 3) if image[i:i + 4] == w]
    if len(at) != 1:
        raise SystemExit('mkdevtest: the ABI word %08x is at %d places in %s, want 1'
                         % (abi, len(at), OVL_IMAGE))
    return at[0]


def images():
    """(images, overlay): the images as (name, defines, bytes), and the
    overlay as (bytes, its record's words)"""
    out = []
    ovl = None
    for name, defs in IMAGES:
        d = os.path.join(ROOT, 'build', 'bcmfw', 'devtest-' + name)
        prog, rep = B.build(d, defs, listing=True)
        image = prog.load_image()
        out.append((name, defs, image))
        if name == OVL_IMAGE:
            oprog, orep = B.build_overlay(prog, d, defs)
            data = open(os.path.join(d, 'viz-ovl.bin'), 'rb').read()
            if oprog.abi != prog.abi:
                raise SystemExit('mkdevtest: overlay ABI %08x, image %08x' % (
                    oprog.abi, prog.abi))
            ovl = (data, (len(data), fnv(data), oprog.abi, abi_at(image, prog.abi),
                          fnv(image), B.config_value('BCMFW_JOBS_OVL', defs)))
    # the hibernate park's stub (bcmfw/hibwake.h): not an image, the PP
    # uploads it beside either jobs image (bcm-hibwake.h bhw_prepare)
    prog, rep = B.build_hibstub(os.path.join(ROOT, 'build', 'bcmfw', 'devtest-hibstub'))
    out.append(('hibstub', None, rep['image']))
    return out, ovl


def verified():
    """(name, fnv, note) from verified.txt: images passed on a device"""
    out = []
    if os.path.exists(VERIFIED):
        for line in open(VERIFIED):
            line = line.split('#', 1)[0].strip()
            if line:
                f = line.split(None, 2)
                if f[0] in TOKENS:
                    continue        # a clock token, not a boot image
                out.append((f[0], int(f[1], 16), f[2] if len(f) > 2 else ''))
    return out


# verified.txt's token lines (mkvideo.py: apps/video/bcmvideo_clock.h)
TOKENS = ('clock150',)


def tokens(name):
    """[(fnv, note)] of verified.txt's lines named name (a token)"""
    out = []
    if os.path.exists(VERIFIED):
        for line in open(VERIFIED):
            f = line.split('#', 1)[0].strip().split(None, 2)
            if f and f[0] == name:
                out.append((int(f[1], 16), f[2] if len(f) > 2 else ''))
    return out


def render(imgs, ovl):
    s = [HEADER]
    dev = False                 # inside #ifdef HAVE_BCMFW_DEVTEST
    for name, defs, data in imgs:
        if dev != (name not in RELEASE):
            dev = not dev
            s.append('\n#ifdef HAVE_BCMFW_DEVTEST\n' if dev
                     else '#endif /* HAVE_BCMFW_DEVTEST */\n')
        pad = data + b'\0' * (-len(data) % 4)
        s.append('\n/* %s: %s; %d bytes; sha256 %s */\n' % (
            name, 'bcmfw/hibstub.c, linked at HW_STUB (not an image)' if defs is None
            else ' '.join('-D ' + d for d in defs) or 'default build', len(data),
            hashlib.sha256(data).hexdigest()))
        s.append('static const uint8_t %s_data[%d] __attribute__((aligned(4))) =\n{\n'
                 % (name, len(pad)))
        for k in range(0, len(pad), 16):
            s.append('    ' + ' '.join('0x%02x,' % b for b in pad[k:k + 16]) + '\n')
        s.append('};\n')
        s.append('const struct bcmdt_image bcmdt_%s_image =\n{\n'
                 '    %s_data, %d, 0x%08x, "%s"\n};\n' % (
                     name, name, len(data), fnv(data), name))
        if name == OVL_IMAGE:
            s.append('/* bcmfw/video/viz-ovl.bin, the visualizer\'s overlay linked against\n'
                     ' * %s: bytes, FNV-1a, ABI word, the word\'s offset in the\n'
                     ' * image, the image\'s FNV-1a, the region\'s base (bcmfw/ovlpair.py\n'
                     ' * checks the pair) */\n'
                     'const struct bcmdt_ovl bcmdt_viz_ovl =\n{\n'
                     '    %d, 0x%08x, 0x%08x, %d, 0x%08x, 0x%x\n};\n' % ((name,) + ovl[1]))
    if dev:
        s.append('#endif /* HAVE_BCMFW_DEVTEST */\n')
    ver = verified()
    names = {name: fnv(data) for name, _, data in imgs}
    s.append('\n/* bcmfw/verified.txt: images passed on a device (stage 5); the\n'
             ' * default boot path needs the DMA image to be one of them */\n')
    s.append('const uint32_t bcmdt_verified_fnv[%d] =\n{\n' % max(1, len(ver)))
    for name, v, note in ver:
        s.append('    0x%08x,   /* %s%s: %s */\n' % (
            v, name, '' if names.get(name) == v else ' (not embedded)', note))
    if not ver:
        s.append('    0           /* none yet */\n')
    s.append('};\nconst int bcmdt_n_verified = %d;\n' % len(ver))
    return ''.join(s)


def main(argv):
    if not B.have_gcc():
        print('mkdevtest: vc4-elf-gcc not found (set VC4_GCC)', file=sys.stderr)
        return 2
    imgs, ovl = images()
    text = prokbox_header.convert(render(imgs, ovl), '.c')[0]
    if '--check' in argv:
        old = open(OUT_C).read() if os.path.exists(OUT_C) else ''
        if old != text:
            print('mkdevtest: %s is stale; run bcmfw/mkdevtest.py' % OUT_C,
                  file=sys.stderr)
            return 1
        if not os.path.exists(OUT_OVL) or open(OUT_OVL, 'rb').read() != ovl[0]:
            print('mkdevtest: %s is stale; run bcmfw/mkdevtest.py' % OUT_OVL,
                  file=sys.stderr)
            return 1
        print('mkdevtest: embedded images and viz-ovl.bin match a fresh build')
        return 0
    with open(OUT_C, 'w') as f:
        f.write(text)
    with open(OUT_OVL, 'wb') as f:
        f.write(ovl[0])
    print('wrote %s, %s (%d bytes, FNV-1a %08x, ABI %08x)' % (
        OUT_C, OUT_OVL, ovl[1][0], ovl[1][1], ovl[1][2]))
    return 0


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
sys.path.insert(0, os.path.join(HERE, '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
