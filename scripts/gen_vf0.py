#!/usr/bin/env python3
#***************************************************************************
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
"""Writes apps/ipodui/devbench_vf0.h: bench row VF0's frames and goldens
(docs/design/viz-fft-vc.md section 10; sitting 4g).

usage: scripts/gen_vf0.py            (writes the header)
       scripts/gen_vf0.py --check    (fails if the header is out of date)

Five fixed 512-sample frames (a sine at a band's centre bin, two tones,
white noise from a fixed seed, silence, a full-scale clipped sine), in
SPECTRUM's split order, and 32 band edges.  The goldens are the levels
the BCM's C path answers: bcmfw/jobs/jviz.c's jviz_power_ref and
jviz_bands, compiled and run here on the host (the same C), so the row
compares the chip's reply word for word.  Dev builds only: the header is
included by devbench.c alone.
"""
import math
import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OUT = os.path.join(ROOT, 'apps', 'ipodui', 'devbench_vf0.h')
# the overlay's FFT start-up check's goldens (bcmfw/video/viz_vpu.c)
GOLD = os.path.join(ROOT, 'bcmfw', 'jobs', 'jviz_gold.h')
N = 512
NB = 32


def edges():
    # log-spaced over bins 1..256, never decreasing (an equal pair is an
    # empty band, which SPECTRUM allows)
    e = [min(N // 2, max(1, round(2 ** (8.0 * b / NB)))) for b in range(NB + 1)]
    for b in range(1, NB + 1):
        e[b] = max(e[b], e[b - 1])
    e[NB] = N // 2
    return e


def clip(v):
    return max(-32768, min(32767, int(round(v))))


def frames(e):
    c = (e[20] + e[21]) // 2            # band 20's centre bin
    out = []
    out.append(('sine', [clip(16000 * math.sin(2 * math.pi * c * i / N))
                         for i in range(N)]))
    out.append(('two tones', [clip(9000 * math.sin(2 * math.pi * 20 * i / N) +
                                   9000 * math.sin(2 * math.pi * 101 * i / N))
                              for i in range(N)]))
    x, noise = 2722, []
    for _ in range(N):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        noise.append((x >> 15) % 24001 - 12000)
    out.append(('noise', noise))
    out.append(('silence', [0] * N))
    out.append(('clipped', [clip(3 * 32767 * math.sin(2 * math.pi * 33 * i / N))
                            for i in range(N)]))
    return out


def split(x):
    return x[0::2] + x[1::2]


def goldens(e, fr):
    src = ['#include <stdio.h>', '#include <stdint.h>', '#include "jviz.h"',
           'static const uint16_t e[%d] = { %s };' % (NB + 1, ', '.join(map(str, e)))]
    for k, (_, x) in enumerate(fr):
        src.append('static const int16_t f%d[%d] __attribute__((aligned(4))) '
                   '= { %s };' % (k, N, ', '.join(map(str, split(x)))))
    src.append('int main(void) {')
    src.append('    static uint32_t p[%d]; uint16_t l2[%d]; int b;' % (N // 2, NB))
    for k in range(len(fr)):
        src.append('    jviz_power_ref(p, f%d, %d);' % (k, N))
        src.append('    jviz_bands(l2, p, %d, e, %d);' % (N // 2, NB))
        src.append('    for (b = 0; b < %d; b++) printf("%%u ", l2[b]);' % NB)
        src.append('    printf("\\n");')
    # the start-up check's frames: FNV-1a of jviz_raw_sep512's raw output
    src.append('    { static int16_t f[512] __attribute__((aligned(32))); '
               'uint16_t raw[512]; uint32_t s, k, h;')
    src.append('      for (s = 0; s < JVIZ_CHECK_FRAMES; s++) {')
    src.append('        jviz_check_frame(f, s); jviz_raw_sep512(raw, f); h = 0x811C9DC5u;')
    src.append('        for (k = 0; k < 512; k++) { h = (h ^ (raw[k] & 0xFF)) * 0x01000193u;')
    src.append('          h = (h ^ (raw[k] >> 8)) * 0x01000193u; }')
    src.append('        printf("G %u\\n", h); } }')
    src.append('    return 0; }')
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, 'g.c')
        exe = os.path.join(d, 'g')
        with open(c, 'w') as f:
            f.write('\n'.join(src) + '\n')
        subprocess.check_call([os.environ.get('HOSTCC', 'cc'), '-std=gnu99',
                               '-O1', '-I', os.path.join(ROOT, 'bcmfw', 'jobs'),
                               '-o', exe, c,
                               os.path.join(ROOT, 'bcmfw', 'jobs', 'jviz.c')])
        res = subprocess.check_output([exe]).decode().split('\n')
    return [[int(v) for v in ln.split()] for ln in res if ln.strip() and ln[0] != 'G'], \
        [int(ln.split()[1]) for ln in res if ln.startswith('G ')]


def render():
    e = edges()
    fr = frames(e)
    g, _ = goldens(e, fr)
    L = ['/* Generated by scripts/gen_vf0.py; edit that, not this.',
         ' * Bench row VF0 (sitting 4g; docs/design/viz-fft-vc.md section 10):',
         ' * five 512-sample frames in SPECTRUM\'s split order, the band edges',
         ' * and the levels bcmfw/jobs/jviz.c (jviz_power_ref, jviz_bands)',
         ' * gives on the host.  Dev builds only (devbench.c). */',
         '#ifndef DEVBENCH_VF0_H', '#define DEVBENCH_VF0_H',
         '#define VF0_N       %d' % N, '#define VF0_NB      %d' % NB,
         '#define VF0_FRAMES  %d' % len(fr),
         'static const char *const vf0_name[VF0_FRAMES] = { %s };' %
         ', '.join('"%s"' % n for n, _ in fr),
         'static const uint16_t vf0_edges[VF0_NB + 1] = {']
    L.append('    ' + ', '.join(map(str, e)) + ' };')
    L.append('static const int16_t vf0_pcm[VF0_FRAMES][VF0_N] '
             '__attribute__((aligned(4))) = {')
    for _, x in fr:
        s = split(x)
        L.append('    {')
        for i in range(0, N, 12):
            L.append('        ' + ', '.join(map(str, s[i:i + 12])) + ',')
        L.append('    },')
    L.append('};')
    L.append('static const uint16_t vf0_l2[VF0_FRAMES][VF0_NB] = {')
    for row in g:
        L.append('    { ' + ', '.join(map(str, row)) + ' },')
    L.append('};')
    L.append('#endif')
    return '\n'.join(L) + '\n'


def render_gold():
    _, gold = goldens(edges(), frames(edges()))
    return '\n'.join([
        '/* Generated by scripts/gen_vf0.py; edit that, not this.',
        ' * The visualizer overlay\'s FFT start-up check (bcmfw/video/viz_vpu.c',
        ' * viz_vpu_check; docs/design/viz-fft-vc.md section 9): FNV-1a of',
        ' * jviz_raw_sep512\'s raw output for each jviz_check_frame, computed on',
        ' * the Mac from the same C, so a wrong C on the BCM cannot hide a right',
        ' * kernel (or the reverse). */',
        '#ifndef BCMFW_JVIZ_GOLD_H', '#define BCMFW_JVIZ_GOLD_H',
        'static const uint32_t jviz_check_gold[%d] = { %s };' % (
            len(gold), ', '.join('0x%08xu' % v for v in gold)),
        '#endif', ''])


def main(argv):
    # the generated file carries the ProkBox licence header too (legal check 3)
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    from prokbox_header import convert
    text = convert(render(), '.h')[0]
    gtext = convert(render_gold(), '.h')[0]
    if '--check' in argv:
        ok = os.path.exists(OUT) and open(OUT).read() == text and \
            os.path.exists(GOLD) and open(GOLD).read() == gtext
        print('gen_vf0: %s' % ('the header matches jviz.c' if ok else
                               'OUT OF DATE (run scripts/gen_vf0.py)'))
        return 0 if ok else 1
    with open(OUT, 'w') as f:
        f.write(text)
    with open(GOLD, 'w') as f:
        f.write(gtext)
    print(os.path.relpath(OUT, ROOT), os.path.relpath(GOLD, ROOT))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
