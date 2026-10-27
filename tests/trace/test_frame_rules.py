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

"""Self-test of scripts/check_trace.py's frame rule with CPU figures and the
host-jitter classification (docs/gate-speed-brief.md section 4; stdlib only).

    python3 tests/trace/test_frame_rules.py

A late run (wall mean over 50 ms) passes as "starved" only when its frames
cost little CPU; frames that cost too much CPU fail whatever the wall clock
says (the negative control: a slow render still fails); without CPU figures a
late run on a stalled host fails with the word "starved" for the ledger.
"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TMP = os.path.join(ROOT, 'build', 'trace-selftest')
CT = os.path.join(ROOT, 'scripts', 'check_trace.py')
fails = 0


def run(name, log_lines):
    os.makedirs(TMP, exist_ok=True)
    sp = os.path.join(TMP, name + '.script')
    lp = os.path.join(TMP, name + '.log')
    open(sp, 'w').write('wait 1\n')
    open(lp, 'w').write('\n'.join(log_lines) + '\n')
    return subprocess.run([sys.executable, CT, lp, sp], capture_output=True,
                          text=True).stdout


def anim(ms, cpu=None, n=12, busy=0.1):
    out = ['ipodui: anim start boost=1']
    for i in range(n):
        out.append('ipodui: anim frame %d%s' % (0 if i == 0 else ms,
                   '' if cpu is None else ' cpu=%d pcpu=%d' % (
                       0 if i == 0 else cpu, 0 if i == 0 else int(ms * 1000 * busy))))
    out.append('ipodui: anim end boost=0')
    return out


JIT_HI = 'sim_load: run jitter mean=12 p95=45 p99=80 max=120 n=900'
JIT_LO = 'sim_load: run jitter mean=0 p95=1 p99=2 max=4 n=900'


def expect(label, name, log, want_fail, needle):
    global fails
    out = run(name, log)
    rule = [l for l in out.splitlines() if 'frame' in l]
    bad = [l for l in rule if l.startswith('FAIL')]
    good = (len(bad) > 0) == want_fail and any(needle in l for l in rule)
    print('%s %s' % ('ok  ' if good else 'FAIL', label))
    if not good:
        fails += 1
        print('     frame lines: %r' % rule)


expect('on time, cheap frames pass', 'f1', anim(40, 2000), False, 'frame interval mean 40.0')
expect('late but cheap: starved by the host, passes', 'f2', anim(80, 2000) + [JIT_HI],
       False, 'starved by the host')
expect('slow render still fails (negative control)', 'f3', anim(40, 12000), True,
       'mean frame cpu 12000 us')
expect('late and expensive fails on cpu, load or not', 'f4', anim(90, 12000) + [JIT_HI],
       True, 'mean frame cpu')
expect('late, cpu near budget: wall rule fails', 'f5', anim(80, 6000), True,
       'mean frame interval 80.0')
expect('late, cheap, but the process was busy: our threads, FAIL', 'f9',
       anim(80, 2000, busy=0.8), True, 'our own threads delayed the UI')
expect('late, cheap, process idle: starved, passes', 'f10', anim(80, 2000, busy=0.1),
       False, 'process 10% busy')
expect('no cpu figures, late, stalled host: FAIL says starved', 'f6', anim(80) + [JIT_HI],
       True, '[starved: host jitter p95 45 ms]')
expect('no cpu figures, late, quiet host: plain FAIL', 'f7', anim(80) + [JIT_LO], True,
       'mean frame interval 80.0 ms > 50 ms (11 frames)')
expect('no cpu figures, on time passes', 'f8', anim(40) + [JIT_HI], False, 'frame interval mean 40.0')

print('frame rules: %s' % ('ok' if not fails else '%d failure(s)' % fails))
sys.exit(1 if fails else 0)
