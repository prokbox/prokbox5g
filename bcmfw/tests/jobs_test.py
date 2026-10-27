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

"""The display job interface's reference executor and ring codec
(bcmfw/jobs, contract bcmfw/jobs.h and docs/research/bcm2722/jobs.md):

- jtest (built for the Mac): unit cases, among them FADE against the
  shell's xfade_rect() at every weight and AVG against AVG2, the slot
  rules, the reply rules, the ring's wrap-around word for word;
- jmtest: the host model (jmodel.c, the bcmjob.h API the simulator
  wraps): results, order, slots and epochs, injected faults, power
  states and the dark power-up count, the trace lines;
- the scripted scene (jscene.c) on the Mac and in vc2sim, compiled by
  vc2cc as the chip would build it: both checksums must equal the golden
  one.  That is the check that the executor means the same on both
  sides, and that the toolchain builds it right (compiler.md, "Known GCC
  bugs").

Usage: bcmfw/tests/jobs_test.py
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
JOBS = os.path.join(FW, 'jobs')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import vc2cc as C     # noqa: E402
import vc2sim as S    # noqa: E402

GOLDEN = 0x73428E8F     # jscene_run(); update only with a contract change
                        # (0x82E48043 until rev 0.4: the slide now draws into
                        # the front frame, out of it in place, each presented)
CFLAGS = ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
          '-fno-tree-loop-distribute-patterns']
MEM = 0x400000


def host():
    exe = os.path.join(ROOT, 'build', 'jobs', 'jtest')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    r = subprocess.run(['cc', '-O2', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-I' + FW, '-I' + JOBS, '-o', exe,
                        os.path.join(JOBS, 'jtest.c'), os.path.join(JOBS, 'jref.c'),
                        os.path.join(JOBS, 'jscene.c')], capture_output=True, text=True)
    if r.returncode:
        print('FAIL host build:\n' + r.stderr)
        return None, 1
    out = subprocess.run([exe], capture_output=True, text=True).stdout
    print(out.strip())
    mexe = os.path.join(ROOT, 'build', 'jobs', 'jmtest')
    # the model offers DECODE (jref.h JREF_DECODE) and the visualizer's
    # proposed SPECTRUM and BARS (JREF_VIZ, proposed); jtest above builds
    # the executor as the PP's CPU path does, without them
    r = subprocess.run(['cc', '-O2', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-DJREF_DECODE', '-DJREF_VIZ', '-I' + FW,
                        '-I' + JOBS, '-o', mexe,
                        os.path.join(JOBS, 'jmtest.c'), os.path.join(JOBS, 'jmodel.c'),
                        os.path.join(JOBS, 'jref.c'), os.path.join(JOBS, 'jref_viz.c'),
                        os.path.join(JOBS, 'jviz.c')],
                       capture_output=True, text=True)
    if r.returncode:
        print('FAIL model build:\n' + r.stderr)
        return None, 1
    mout = subprocess.run([mexe], capture_output=True, text=True).stdout
    print(mout.strip())
    if not mout.rstrip().endswith(' 0 failed'):
        return None, 1
    scene = next((int(x.split()[1], 16) for x in out.split('\n') if x.startswith('jscene ')), None)
    return scene, 0 if out.rstrip().endswith(' 0 failed') else 1


def sim():
    prog = C.build([os.path.join(JOBS, 'jref.c'), os.path.join(JOBS, 'jscene.c')], '-O2',
                   extra=CFLAGS + ['-I' + FW, '-I' + JOBS], libs=C.DEFAULT_LIBS,
                   no_float=True, conservative=True)
    s = S.Sim()
    C.load_into_sim(prog, s)
    s0 = s.steps
    h = s.call('jscene_run', MEM, check_abi=True, max_steps=400_000_000, regs={15: 0, 22: 0})
    return h, s.steps - s0, prog


def main():
    if not os.path.exists(C.GCC):
        print('skip: vc4-elf-gcc not found (%s)' % C.GCC)
        return 0
    t0 = time.time()
    bad = 0
    hs, b = host()
    bad += b
    vs, steps, prog = sim()
    text = prog.ends['text'] - prog.symbols['__text_start']
    print('vc2sim: jscene %08x, %.2f M instructions; executor and scene %d bytes of code'
          % (vs, steps / 1e6, text))
    for name, v in (('host', hs), ('vc2sim', vs)):
        if v != GOLDEN:
            print('FAIL %s scene %s, golden %08x' % (name, 'none' if v is None else '%08x' % v,
                                                     GOLDEN))
            bad += 1
    print('jobs_test: %d failed (%.0f s)' % (bad, time.time() - t0))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
