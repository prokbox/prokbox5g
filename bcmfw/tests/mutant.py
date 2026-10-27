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

"""A generated kernel's mutant for this process's builds only.

The mutation tests (jidct_test, jobs_image_test t_variants, viz_test) build
deliberately broken kernels (gen_jdec.py, gen_jobs.py, gen_viz.py
--mutate NAME).  They used to write them over the tracked kernels in
bcmfw/video and regenerate after; scripts/bcmfw_benches.sh runs BCM_JOBS
benches at once, and any of them that built an image in that window got
the mutant: decode_image's merged overlay built during jidct_test's puty
and trapped at strict alignment (V8 D +r y 8 at 0x200), 2026-10-26.  Now
the generator writes the mutant into a private directory (--out) and
vc2cc.SUBST builds it in place of the tracked file, in this process only.

    with mutant.use(GEN, 'puty'):
        ...builds here take the mutant...
"""
import contextlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import vc2cc as C       # noqa: E402

VIDEO = os.path.join(ROOT, 'bcmfw', 'video')


@contextlib.contextmanager
def use(gen, mutate):
    """gen (a bcmfw/video/gen_*.py) --mutate mutate into a private
    directory; within the block, every vc2cc build of this process takes
    its .S in place of bcmfw/video's.  mutate None: the tracked kernels
    (nothing substituted)."""
    if not mutate:
        yield
        return
    tmp = tempfile.mkdtemp(prefix='mutant-%s-' % mutate,
                           dir=os.path.join(ROOT, 'build') if os.path.isdir(
                               os.path.join(ROOT, 'build')) else None)
    saved = dict(C.SUBST)
    try:
        subprocess.run([sys.executable, gen, '--mutate', mutate, '--out', tmp],
                       check=True, capture_output=True)
        for f in os.listdir(tmp):
            if f.endswith('.S'):
                C.SUBST[os.path.realpath(os.path.join(VIDEO, f))] = os.path.join(tmp, f)
        yield
    finally:
        C.SUBST.clear()
        C.SUBST.update(saved)
        shutil.rmtree(tmp, ignore_errors=True)


def switcher(gen):
    """A regen(mutate=None) for the tests' loops: from the call on, this
    process's builds take gen's mutant (None: the tracked kernels again),
    until the next call; the tree is never written"""
    cur = []

    def regen(mutate=None):
        while cur:
            cur.pop().__exit__(None, None, None)
        if mutate:
            cm = use(gen, mutate)
            cm.__enter__()
            cur.append(cm)
    return regen
