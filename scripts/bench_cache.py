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
# Content-hash cache for the long bcmfw benches (scripts/bcmfw_benches.sh).
#
# A bench whose expected run is over THRESHOLD_MIN minutes is skipped when
# the hash of everything it reads matches a recorded PASS
# (CACHE/<name>/<hash>).  The hash covers the test script, the
# sources it builds or simulates, the tools that run them (vc2sim, vc2cc),
# the corpus manifests and a listing of the built corpus, the images it
# uses (their bytes, so their FNVs), the toolchain's version and the
# VC2SIM_* settings.  When unsure a file is in: a spurious re-run costs
# minutes, a stale "cached" costs trust.
#
#   bench_cache.py hash NAME           print NAME's input hash (exit 2: not cached)
#   bench_cache.py check NAME          exit 0 and print the hash if a PASS holds it
#   bench_cache.py record NAME HASH    record a PASS of NAME at HASH
#   bench_cache.py drop NAME HASH      forget a PASS of NAME at HASH
#   bench_cache.py list                the cached benches and their minutes
#   bench_cache.py limit NAME FLOOR    NAME's timeout in seconds: twice its
#                                      expected minutes, never under FLOOR
#
# One cache for every worktree and agent (2026-10-26: each quick gate ran
# all 30 benches from scratch): a PASS is a file CACHE/NAME/HASH, so
# worktrees at different inputs never evict each other; the hash covers
# every input's bytes, so sharing is safe.  At most KEEP passes a bench
# (the oldest go).  Writes are a unique temporary renamed into place.
#
# Environment: BENCH_CACHE_DIR (default ~/.cache/prokbox-bench-cache), BENCH_CACHE_ROOT
# (the tree hashed; default the repo), BENCH_CACHE_TABLE (a JSON file
# {name: [minutes, [paths...]]} replacing the table; the tests use it).
#
# Copyright (C) 2026 prokopto-dev
# Copyright (C) 2026 prokopto-dev
#
# Portions derived from Rockbox (rockbox.org).
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

import hashlib
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.environ.get('BENCH_CACHE_ROOT', REPO)
CACHE = os.environ.get('BENCH_CACHE_DIR',
                       os.path.expanduser('~/.cache/prokbox-bench-cache'))
KEEP = 16           # passes kept a bench, across every worktree

# Benches longer than this (minutes) are cached; shorter ones always run.
THRESHOLD_MIN = 5

# Shared by every bcmfw bench: the image sources and their tests, the
# simulator and compiler, the port model's source, the gate script itself.
COMMON = [
    'bcmfw/',
    'tools/bcmre/',
    'firmware/target/arm/ipod/video/',
    'firmware/export/config/ipodvideo.h',
    'scripts/bcmfw_benches.sh',
    'scripts/bench_cache.py',
]
# The embedded and shipped images (bytes, so their FNVs): the merged,
# plain and display-only video images, the overlays, the devtest set.
IMAGES = [
    'bcmfw/video/bcmvideo.bin',
    'bcmfw/video/bcmvideo-jobs.bin',
    'bcmfw/video/viz-ovl.bin',
    'bcmfw/video/dec-ovl.bin',
    'apps/video/',
    'firmware/target/arm/ipod/video/bcm-devtest-images.c',
    'bcmfw/verified.txt',
]

# name: (expected minutes, inputs beyond COMMON and IMAGES).  A directory
# is every file under it; build/<x>/corpus is listed (names and sizes).
TABLE = {
    'jpeg': (38, [
        'bcmfw/tests/jpeg_test.py',
        'apps/recorder/jpeg_load.c', 'apps/recorder/jpeg_load.h',
        'apps/recorder/resize.c', 'apps/recorder/resize.h',
        'apps/recorder/bmp.c', 'apps/recorder/bmp.h',
        'apps/recorder/jpeg_common.h', 'apps/recorder/jpeg_idct_arm.S',
        'bcmfw/jpeg/', 'bcmfw/jobs/',
        'tests/jpeg/',
        'build/jpeg/corpus', 'build/jpeg-prog',
    ]),
    'video': (22, [
        'bcmfw/tests/video_test.py',
        'bcmfw/h264/', 'bcmfw/video/', 'bcmfw/aac/',
        'tests/video/corpus.sha256', 'tests/video/make_corpus.sh',
        'build/video/corpus',
    ]),
    'vcheck_image': (7, [
        'bcmfw/tests/vcheck_image_test.py', 'bcmfw/tests/vcheck_shim.c',
        'bcmfw/tests/video_test.py',
    ]),
    'devtest': (12, [
        'bcmfw/tests/devtest_test.py',
        'bcmfw/bench/',
        'apps/ipodui/devbench.c', 'apps/ipodui/devbench.h',
        'apps/ipodui/devbench_power.c',
        'docs/research/bcm2722/bench-p19.md',
    ]),
}


def table():
    path = os.environ.get('BENCH_CACHE_TABLE')
    if path:
        return {k: (v[0], v[1]) for k, v in json.load(open(path)).items()}
    return {k: (m, COMMON + IMAGES + list(p)) for k, (m, p) in TABLE.items()}


def walk(rel):
    """[(relpath, kind, data)] for rel: a file's bytes, or a corpus
    listing (names and sizes) under build/, or every file of a directory"""
    full = os.path.join(ROOT, rel)
    if not os.path.exists(full):
        return [(rel, 'missing', b'')]
    if os.path.isfile(full):
        return [(rel, 'file', open(full, 'rb').read())]
    out = []
    listing = rel.startswith('build/')
    for d, dirs, files in os.walk(full):
        dirs[:] = sorted(x for x in dirs if x not in ('__pycache__', '.git'))
        for f in sorted(files):
            if f.endswith('.pyc') or f == '.DS_Store':
                continue
            p = os.path.join(d, f)
            r = os.path.relpath(p, ROOT)
            if listing:
                out.append((r, 'size', str(os.path.getsize(p)).encode()))
            else:
                out.append((r, 'file', open(p, 'rb').read()))
    return out or [(rel, 'empty', b'')]


def toolchain():
    try:
        r = subprocess.run(['vc4-elf-gcc', '--version'], capture_output=True,
                           text=True, timeout=30)
        return r.stdout.split('\n')[0]
    except (OSError, subprocess.SubprocessError):
        return 'none'


def digest(name):
    t = table()
    if name not in t or t[name][0] <= THRESHOLD_MIN:
        return None
    h = hashlib.sha256()
    h.update(('bench %s\n' % name).encode())
    seen = set()
    for rel in t[name][1]:
        for r, kind, data in walk(rel.rstrip('/')):
            if r in seen:
                continue
            seen.add(r)
            h.update(('%s %s %d\n' % (kind, r, len(data))).encode())
            h.update(data)
    env = sorted((k, v) for k, v in os.environ.items()
                 if k.startswith('VC2SIM_') or k in ('BCM_QUICK', 'GATE_LONG',
                                                     'JPEG_CORPUS', 'JPEG_PROG'))
    h.update(repr(env).encode())
    h.update(('python %s\n' % sys.version.split()[0]).encode())
    if not os.environ.get('BENCH_CACHE_TABLE'):
        h.update(('vc4 %s\n' % toolchain()).encode())
    return h.hexdigest()


def main(argv):
    if len(argv) >= 2 and argv[0] == 'hash':
        d = digest(argv[1])
        if d is None:
            return 2
        print(d)
        return 0
    if len(argv) >= 2 and argv[0] == 'check':
        d = digest(argv[1])
        if d is None:
            return 2
        print(d)
        return 0 if os.path.isfile(os.path.join(CACHE, argv[1], d)) else 1
    if len(argv) == 3 and argv[0] == 'record':
        d = os.path.join(CACHE, argv[1])
        os.makedirs(d, exist_ok=True)
        tmp = os.path.join(d, '.%s.%d.tmp' % (argv[2], os.getpid()))
        with open(tmp, 'w') as f:
            f.write(argv[2] + '\n')
        os.replace(tmp, os.path.join(d, argv[2]))
        held = sorted((e for e in os.listdir(d) if not e.startswith('.')),
                      key=lambda e: os.path.getmtime(os.path.join(d, e)))
        for e in held[:-KEEP]:
            try:
                os.remove(os.path.join(d, e))
            except OSError:
                pass
        return 0
    if len(argv) == 3 and argv[0] == 'drop':
        try:
            os.remove(os.path.join(CACHE, argv[1], argv[2]))
        except OSError:
            pass
        return 0
    if len(argv) == 3 and argv[0] == 'limit':
        # a loaded host (several gates at once) stretches the long benches:
        # jpeg timed out at the flat 2,400 s under a load of 15-30
        t = table()
        floor = int(argv[2]) if argv[2].isdigit() else 2400
        print(max(floor, 2 * 60 * t[argv[1]][0]) if argv[1] in t else floor)
        return 0
    if argv[:1] == ['list']:
        for k, (m, _) in sorted(table().items()):
            print('%-10s %3d min %s' % (k, m, 'cached' if m > THRESHOLD_MIN else 'always run'))
        return 0
    sys.stderr.write(__doc__ if __doc__ else 'usage: bench_cache.py hash|check|record|list\n')
    return 64


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
