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

"""Synthesized pulled folders for scripts/bench_ingest.py (no owner media).

Every line is written in the device's own formats: the results file by
devbench.h's bench_fmt_* (mirrored below), bcmtest.log by devbcm.c's
s6_line ("%6ld text") and bcm-devtest-core.c's bench records (B4V, read
by bcmfw/bench/readlog.py), ipodui.log by uilog.c ("<seq> <tag> <text>",
a "boot build=" line first), the pass flags by bcmdt_flag_format, power.csv
by powerlog_core.c.  The step ids, answers and flags come from
apps/ipodui/devbench.c through the ingest tool's own table parser.

Usage: gen_fixtures.py OUTDIR    (run.sh compares it with fixtures/)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
sys.path.insert(0, os.path.join(ROOT, 'bcmfw', 'bench'))
import bench_ingest as BI  # noqa: E402
import readlog as RL  # noqa: E402

RULES = BI.Rules()
STEPS = {s['id']: s for s in RULES.all_steps}
FNV, BYTES = 0xcd8e5d96, 218660


# ---- devbench.h's formatters ------------------------------------------------
def fmt_session(build, dt, steps, mode, image=True, kind='merged'):
    if not image:                       # build A's full session (no image fields)
        return 'session build=%s date=%s steps=%d\n' % (build, dt, steps)
    if mode == 'full':
        return 'session build=%s date=%s steps=%d image=%d fnv=%08x kind=%s\n' % (
            build, dt, steps, BYTES, FNV, kind)
    return 'session build=%s date=%s steps=%d mode=%s image=%d fnv=%08x kind=%s\n' % (
        build, dt, steps, mode, BYTES, FNV, kind)


def fmt_start(sid):
    return 'step=%s result=start\n' % sid


def fmt_log(sid, text):
    return 'step=%s log=%s\n' % (sid, text)


def fmt_result(sid, r, answer, ms, note):
    return 'step=%s result=%s answer=%s ms=%d note=%s\n' % (
        sid, r, BI.token(answer), ms, note)


class Bench:
    """A session as the device writes it: the results file and the
    bcmtest.log lines (the row's lines, then the auto: line)"""

    def __init__(self, t0=1000):
        self.res = []
        self.log = []
        self.t = t0

    def bl(self, text):
        self.t += 37
        self.log.append('%6d %s' % (self.t, text))

    def step(self, sid, r='pass', answer=None, ms=1200, note='', logs=(), rows=(),
             start=True):
        if start:
            self.res.append(fmt_start(sid))
        for x in rows:
            self.bl(x)
        for x in logs:
            self.bl(x)
            self.res.append(fmt_log(sid, x))
        self.res.append(fmt_result(sid, r, answer, ms, note))
        self.bl('auto: %s %s answer=%s ms=%d %s' % (sid, r, answer or '-', ms, note))

    def manual(self, sid, r, answer):
        """fn NULL: start, the owner leaves, the next press asks and records,
        then the resume line"""
        self.res.append(fmt_start(sid))
        self.res.append(fmt_result(sid, r, answer, 0, ''))
        self.bl('auto: %s %s answer=%s ms=0 ' % (sid, r, answer))
        self.res.append('resume after=%s\n' % sid)


def vrec_lines(t, fail=()):
    """stage 4 / 4b bench records with every P19 check as modelled (the
    values readlog's verdicts name as modelled), except those in fail"""
    def w(v):
        return v & 0xFFFF

    def words(lanes):
        lanes = [w(x) for x in lanes] + [0] * (8 - len(lanes))
        return [lanes[2 * k] | lanes[2 * k + 1] << 16 for k in range(4)]
    x, y = RL.X15M_IN
    h, lo = RL.X15S_IN
    a, b = RL.MHDU_IN
    got = {
        'COP8': [0x11, 0x22, 0x33, 0x44], 'SUMR': [0x55, 0, 0, 0],
        'ASHF': [1, 2, 3, 4], 'ASHG': [5, 6, 7, 8],
        'X15M': words([(p * -320 + q * 1280) >> 16 for p, q in zip(x, y)]),
        'X15S': words([max(-32768, p + (-1 if q < 0 else 0)) for p, q in zip(h, lo)]),
        'X15Z': words(list(h)),
        'MHDU': words([(p * (q & 0xFFFF)) >> 16 for p, q in zip(a, b)]),
        'VCLP': words([max(0, min(v, 255)) for v in RL.VCLP_IN]),
        'R48E': [9, 9, 9, 9], 'D16R': [7, 7, 7, 7],
        'REPA': words(list(RL.REPA_TH[0][1])),
        'PUTV': [3, 3, 3, 3],
    }
    out = []
    for k, (tag, g) in enumerate(got.items()):
        ok = tag not in fail
        flags = (1 if ok else 0) | 8 << 8 | (0xFFFF if ok else 2) << 16
        out.append('%6d B4V %d %s %08x %s %08x %08x' % (
            t + k, k, tag, flags, ' '.join('%08x' % v for v in g),
            0 if ok else 0xdeadbeef, 0 if ok else 0x12345678))
    return out


def stage4(b, fail4b=()):
    b.bl('stage 4: timing bench (no panel I/O)')
    b.log += vrec_lines(b.t, ())
    b.bl('stage 4b: timing bench at 150 MHz (no panel I/O)')
    b.log += vrec_lines(b.t, fail4b)
    b.t += 100


def ui_log(lines, build='4e57b3b9a1'):
    out = ['boot build=%s rtc=2026-10-25 13:58:02' % build]
    for k, (tag, text) in enumerate(lines):
        out.append('%d %s %s' % (k + 1, tag, text))
    return '\n'.join(out) + '\n'


def power_csv(marks):
    hdr = BI_POWER_HEADER
    rows = []
    for k, m in enumerate(marks):
        rows.append('%d,4012,88,0,0,0,10,100,100,0,1,0,0,0,-20,0,%s,90,95,ui,3,-,0,0,0,0,-,0,0,0'
                    % (60 * k, m))
    return hdr + '\n'.join(rows) + '\n'


BI_POWER_HEADER = ("s,mv,pct,charger,charging,play,boost_pct,bl_pct,lcd_pct,disk_pct,"
                   "dac_allow,dac_sb,dac_sleeps,dac_wakes,vol_db,dropped,mark,"
                   "idle_pct,cop_idle_pct,cpu1,cpu1_pct,cpu2,cpu2_pct,"
                   "disk_cmds,disk_kb,disk_wakes,disk_by,"
                   "stor_on_pct,stor_ons,stor_ons_total\n")

S6_LOGS = ('video image (merged) %d bytes fnv %08x' % (BYTES, FNV),
           'stage 6: 2 clips, 0 late, A/V within 12 ms')
PORT = ('video: port KB, MB/s (busy time) (812, 11)', 'video: port CPU ms, blocks (74, 212)')


def full_session(b, answers=None, verified=True, image=True, build='p19-a1b2c3d',
                 fail4b=()):
    """The full run on a job build, all good unless answers overrides"""
    ans = dict(s05='Yes', s6='Smooth, in sync', s7='Yes', b13='No', b5='Yes', b3='Yes',
               b3ab='Yes', drill1='Yes', drill2='Drift or still cover', drill3='Yes',
               scroll='Yes')
    ans.update(answers or {})
    n = len([s for s in RULES.all_steps if not s['flags'] & BI.F['BENCH_F_PICKED']])
    b.res.append(fmt_session(build, '2026-10-25 14:02', n, 'full', image=image))

    def verdict(sid, auto='pass'):
        s = STEPS[sid]
        return 'pass' if BI.answer_index(s, BI.token(ans[sid])) == 0 else 'fail'
    b.step('s05', verdict('s05'), ans['s05'], 301000, 'stage 5 PASS',
           rows=('stage 0: Apple vmcs running, read-only', 'frame 1 done in 27100 us: PHOTO 1 '
                 '(pattern)', 'kick LCD_UPDATERECT 200,150 100x80',
                 'scroll storm: 200 UPDATERECTs (the pattern moves up)',
                 'storm list area 320x220 : 50, upload 9100 us (drawing 2100 us), push 7000 '
                 'us avg, 7400 max'), logs=('stage 5 PASS',))
    stage4(b, fail4b)
    b.step('s4b', 'pass', None, 92000, 'stage 4b PASS', logs=('stage 4b PASS',))
    for sid in ('c1', 'c2', 'c3', 'pb1', 'pb4', 'pw1', 'pw2', 'pw7', 'pw8'):
        b.step(sid, 'pass', None, 4000, '%s: PASS' % sid, logs=('%s: PASS' % sid,))
    b.step('jobs', 'skip', None, 10, 'jobs: self-test rc 1', logs=('jobs: self-test rc 1',))
    b.step('jvpu', 'skip', None, 300, 'jvpu: no job block: the resident image isn\'t running '
           '(block 00000000)', logs=('jvpu: no job block: the resident image isn\'t running '
                                     '(block 00000000)',))
    b.step('port', 'skip', None, 0, 'port: measured by stage 6\'s play (part B): it needs '
           'eyes on the clip')
    b.step('pb2', 'pass', None, 30000, 'pb2: PASS')
    b.step('pb3', 'pass', None, 30000, 'pb3: PASS')
    b.step('s6', verdict('s6'), ans['s6'], 26000, 'stage 6 PASS', rows=PORT, logs=S6_LOGS)
    b.step('s7', verdict('s7'), ans['s7'], 60000, 'stage 7 PASS')
    b.step('b13', verdict('b13'), ans['b13'], 9000, 'Warm swap: PASS')
    b.step('b5', verdict('b5'), ans['b5'], 15000, 'Window push: PASS',
           logs=('lab5: J_VPU 7 (display-only image)', 'lab5: PASS (91 pushes by DMA of 91, '
                 '0 timed out)'))
    b.step('cr1', 'pass', None, 21000, CR1_PASS, logs=(CR1_PASS,))
    # boot: a planned restart, finished at the next press, then resume
    b.res.append(fmt_start('boot'))
    b.bl('boot path toggle: DMA (attempt marker and strikes cleared)')
    b.res.append(fmt_result('boot', 'done', None, 0, ''))
    b.bl('auto: boot done answer=- ms=0 ')
    b.res.append('resume after=boot\n')
    b.bl('boot path: active: bcmvideo (toggle), first frame 30100 us; marker cleared')
    b.step('rstat', 'pass', None, 200, 'Driver status: active bcmvideo')
    b.step('jobs2', 'pass', None, 40, 'jobs: self-test rc 0', logs=('jobs: self-test rc 0',))
    jv = ('jvpu: J_VPU 3, expected 3 (merged image; checked 1, shift 1, MAC 0) at block '
          '000de000')
    b.step('jvpu2', 'pass', None, 400, jv, logs=(jv,))
    for sid in ('b3', 'b3ab', 'drill1'):
        b.manual(sid, verdict(sid), ans[sid])
    b.step('drill2', verdict('drill2'), ans['drill2'], 60000, 'drill2: inject op=0x26 n=2 rc=0',
           logs=('drill2: inject op=0x26 n=2 rc=0',))
    for sid in ('drill3', 'scroll'):
        b.manual(sid, verdict(sid), ans[sid])
    b.res.append(fmt_start('wave2'))
    b.res.append(fmt_result('wave2', 'done', 'Not now', 0, ''))
    b.bl('auto: wave2 done answer=Not now ms=0 ')
    for sid in ('pb6', 'pb7', 'pb8', 'pb9', 'pw3', 'pw4', 'pw5', 'pw6'):
        b.step(sid, 'skip', None, 0, 'not chosen', start=False)


CR1_PASS = ('cr1: PASS (control 0 wrong, pools 0 of 7 wrong; slots 8 pass, 0 write, 0 sum, '
            '0 other)')
CR1_REGION = ('cr1: FAIL: region not writable after start (control 0 wrong, pools 7 of 7 '
              'wrong; slots 0 pass, 8 write, 0 sum, 0 other)')


def finish(b, d, build='p19-a1b2c3d', mode='full', want_verified=True):
    rules = BI.Rules()
    st = BI.State()
    sess_lines = ''.join(b.res).splitlines()
    ids = {BI.parse_line(x)[1] for x in sess_lines} - {None}
    rules.choose(ids, -1, BI.parse_mode(mode))
    for x in sess_lines:
        BI.replay(st, rules.steps, x)
    ok, _, picks = BI.earned(rules, st)
    if ok and want_verified:
        b.res.append('verified %s\n' % BI.fmt_verified(st, build, BI.opt_passed(picks, st)))
    b.res.append('end passed=%d failed=%d skipped=%d\n' % (st.passed, st.failed, st.skipped))


def write(d, rel, text, mode='w'):
    p = os.path.join(d, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, mode) as f:
        f.write(text)


UI_GOOD = [('boot', 'variant 5.5G'), ('mem', 'floor dev audio=39845888 bcm_res=218112 '
                                             'free=1048576'),
           ('bcmjob', 'up epoch=1 ops=00003fff'), ('bcmjob', 'vpu=0x3 expect=0x3'),
           ('panel', 'drift jobs mode=on')]


def common(d, b, ui=UI_GOOD, marks=('boot 2026-10-25T13:58', 'ab_A1', 'ab_B1')):
    write(d, 'bcmtest.log', '\n'.join(b.log) + '\n')
    write(d, 'ipodui.log', ui_log(ui))
    write(d, 'bcmpass5.txt', '5 hello_dma 00001f50 4326f8dc\n')
    write(d, 'bcmpass4b.txt', '4b bench_pll 0000a2c4 1b2c3d4e\n')
    write(d, 'power.csv', power_csv(marks))
    write(d, 'bcmjobs.on', '')


def gen(out):
    # 1. a passing full session on today's format: earns its line
    d = os.path.join(out, 'full_pass')
    b = Bench()
    full_session(b)
    finish(b, d)
    write(d, 'bench/20261025-140200-p19-a1b2c3d.txt', ''.join(b.res))
    common(d, b)

    # 1b. the port write check: CR1 alone, its verdict, no line
    d = os.path.join(out, 'portcheck')
    b = Bench()
    b.res.append(fmt_session('p19-a1b2c3d', '2026-10-25 18:00', 3, 'portcheck'))
    b.step('pre', 'pass', None, 20, 'pre: stage 5 pass flag: PASS',
           logs=('pre: stage 5 pass flag: PASS',))
    b.step('s05', 'skip', None, 0, 'not needed: the stage 5 flag is there', start=False)
    b.step('cr1', 'fail', None, 21000, CR1_REGION,
           logs=('cr1 region 0x105700 wrong=0/64 (control, host frame)',
                 'cr1 region 0x046000 wrong=64/64 (drift) first got 00000000 want 3c6ef372',
                 'cr1 variant addr16 at 0x046000 wrong=0/64',
                 'cr1: variants landing where ours doesn\'t: addr16', CR1_REGION))
    finish(b, d, mode='portcheck')
    write(d, 'bench/20261025-180000-p19-a1b2c3d.txt', ''.join(b.res))
    common(d, b)

    # 2. a re-verify run on build A that earns its line
    d = os.path.join(out, 'reverify_earns')
    b = Bench()
    b.res.append(fmt_session('4e57b3b9a1', '2026-10-25 15:10', 9, 'reverify'))
    b.step('pre', 'pass', None, 20, 'pre: stage 5 pass flag: PASS',
           logs=('pre: stage 5 pass flag: PASS',))
    b.step('s05', 'skip', None, 0, 'not needed: the stage 5 flag is there', start=False)
    b.step('boot', 'skip', None, 0, 'not needed: the stage 5 flag is there', start=False)
    b.step('jobs', 'pass', None, 40, 'jobs: self-test rc 0', logs=('jobs: self-test rc 0',))
    jv = ('jvpu: J_VPU 3, expected 3 (merged image; checked 1, shift 1, MAC 0) at block '
          '000de000')
    b.step('jvpu', 'pass', None, 400, jv, logs=(
        jv, 'jvpu: forms BSL1 ok BSF1 ok BSM1 differs BSR1 ok BSX1 ok BSS1 ok BSZ1 ok '
            '(vpu2 f7)',
        'jvpu: forms: first differing BSM1 lane 7 got 00ff exp 0047',
        'jvpu: sdram byte 41.2 ns word 38.0 ns; sram byte 7.4 ns word 7.3 ns at 150 MHz'))
    b.manual('b3', 'pass', 'Yes')
    b.step('drill2', 'pass', 'Drift or still cover', 60000, 'drill2: inject op=0x26 n=2 rc=0',
           logs=('drill2: inject op=0x26 n=2 rc=0',))
    b.step('s6', 'pass', 'Smooth, in sync', 26000, 'stage 6 PASS', rows=PORT, logs=S6_LOGS)
    b.step('b13', 'pass', 'No', 9000, 'Warm swap: PASS')
    b.step('b5', 'pass', 'Yes', 15000, 'Window push: PASS',
           logs=('lab5: J_VPU 7 (display-only image)',))
    finish(b, d, build='4e57b3b9a1', mode='reverify')
    write(d, 'bench/20261025-151000-4e57b3b9a1.txt', ''.join(b.res))
    b.log.insert(0, '%6d boot path: active: bcmvideo (default), first frame 30100 us; '
                    'marker cleared' % 10)
    stage4(b)
    common(d, b)

    # 2b. sitting 3 (2026-10-26): a re-verify run whose restart step was
    #     recorded done with no restart (bcmtest.log shows no new start),
    #     so jobs answered "self-test rc 1" with an empty note: a NEED step
    #     skipped, flagged first; then a later session on a build that
    #     names the reason, retries after a restart and passes
    d = os.path.join(out, 'need_skip')
    b = Bench()
    b.res.append(fmt_session('7e8c140e40-261026', '2026-10-26 09:47', 16, 'reverify'))
    b.step('pre', 'skip', None, 20, 'pre: Not available: void: the embedded image differs '
           'from the one that passed', logs=('pre: Not available: void: the embedded image '
                                             'differs from the one that passed',))
    b.step('s05', 'pass', 'Yes', 182330, 'Stage 5: PASS', logs=('Stage 5: PASS',),
           rows=('restore: Apple vmcs (stock wake path)',))
    b.res.append('resume after=s05\n')
    b.res.append(fmt_start('boot'))
    b.bl('boot path toggle: DMA (attempt marker and strikes cleared)')
    b.res.append(fmt_result('boot', 'done', None, 0, ''))
    b.bl('auto: boot done answer=- ms=0 ')
    b.res.append('resume after=boot\n')
    b.step('jobs', 'skip', None, 10, '', logs=('jobs: self-test rc 1',))
    write(d, 'bench/20261026-094717-7e8c140e40-261026.txt', ''.join(b.res))
    b2 = Bench(t0=100)
    b2.log = ['boot: BCM powered (the ROM\'s vmcs) at entry',
              '     0 === boot path: bcmvideo ===']
    b2.res.append(fmt_session('p19-s3', '2026-10-26 11:00', 16, 'reverify'))
    why = ('pre: stage 4 and 5 pass flags there; a restart comes first: the display '
           'program isn\'t running in this start')
    b2.step('pre', 'done', None, 20, why, logs=(why,))
    b2.step('s05', 'skip', None, 0, 'not needed: the stage 5 flag is there', start=False)
    b2.res.append(fmt_start('boot'))
    b2.res.append(fmt_log('boot', 'restart asked: wall=1793012345'))
    b2.res.append(fmt_log('boot', 'No restart seen: still the start that asked for it'))
    b2.bl('auto: boot no restart seen (asked at 1793012345, this start began at '
          '1793012000)')
    b2.res.append(fmt_result('boot', 'done', None, 0, 'restart seen; the display driver runs'))
    b2.res.append('resume after=boot\n')
    b2.step('jobs', 'pass', None, 40, 'jobs: self-test rc 0', logs=('jobs: self-test rc 0',))
    write(d, 'bench/20261026-110000-p19-s3.txt', ''.join(b2.res))
    common(d, b)
    write(d, 'bcmtest.log', '\n'.join(b.log + b2.log) + '\n')

    # 2c. sitting 4c's order (devbench.h): basic to advanced, the steps
    #     that need a failed one skipped by themselves (the note says
    #     which, bench_fmt_autoskip), the rest still run.  s6 shows no
    #     picture: the other video steps and the Videos play skip; ucop's
    #     restart leaves the UI on the CPU: the second-core rows skip
    d = os.path.join(out, 'autoskip')
    b = Bench()
    b.res.append(fmt_session('p19-s4c', '2026-10-27 10:00', 25, 'reverify'))
    b.step('pre', 'pass', None, 20, 'pre: stage 4 and 5 pass flags and the display '
           'driver: PASS', logs=('pre: stage 4 and 5 pass flags and the display driver: '
                                 'PASS',))
    b.step('s05', 'skip', None, 0, 'not needed: the stage 5 flag is there', start=False)
    b.step('boot', 'skip', None, 0, 'not needed: the display driver runs', start=False)
    b.step('jobs', 'pass', None, 40, 'jobs: self-test rc 0', logs=('jobs: self-test rc 0',))
    jv = ('jvpu: J_VPU 3, expected 3 (merged image; checked 1, shift 1, MAC 0) at block '
          '000de000')
    b.step('jvpu', 'pass', None, 400, jv, logs=(jv,))
    b.step('c1', 'pass', None, 3000, 'C1: PASS')
    b.step('s4b', 'pass', None, 60000, '')
    b.step('s4c', 'pass', 'Yes', 30000, '')
    b.manual('b3', 'pass', 'Yes')
    b.manual('respanel', 'pass', 'Sharp, steady')
    b.manual('uviz', 'pass', 'Smooth')
    b.step('b13', 'pass', 'No', 9000, 'Warm swap: PASS')
    b.step('b5', 'pass', 'Yes', 15000, 'Window push: PASS')
    b.step('s6', 'fail', 'No picture', 26000, 'stage 6 FAIL')
    for sid in ('vfeed', 's7', 'vbv', 'vgov', 'vplay'):
        b.step(sid, 'skip', None, 0, 'auto-skip: needs s6 (fail)', start=False)
    b.res.append(fmt_start('ucop'))
    b.res.append(fmt_log('ucop', 'restart asked: wall=1793100000'))
    note = ('ucop: the UI runs cpu after the restart, not on the COP (refused or fell '
            'back: fe lines)')
    b.res.append(fmt_log('ucop', note))
    b.res.append(fmt_result('ucop', 'fail', None, 0, note))
    b.bl('auto: ucop fail answer=- ms=0 %s' % note)
    b.res.append('resume after=ucop\n')
    for sid in ('uck1', 'uck2', 'ufeel', 'upanel', 'ucop2'):
        b.step(sid, 'skip', None, 0, 'auto-skip: needs ucop (fail)', start=False)
    finish(b, d, build='p19-s4c', mode='reverify')
    write(d, 'bench/20261027-100000-p19-s4c.txt', ''.join(b.res))
    common(d, b)

    # 3. a failed step, with answers that are not the good one (the stale
    #    band report: the scroll's top rows), and a check not as modelled
    d = os.path.join(out, 'failed_step')
    b = Bench()
    full_session(b, answers=dict(b5='Sheared', scroll='Stale pixels'), fail4b=('ASHG',))
    finish(b, d)
    write(d, 'bench/20261025-140200-p19-a1b2c3d.txt', ''.join(b.res))
    common(d, b, ui=UI_GOOD + [('bcmjob', 'resync x=0 y=20 w=320 h=220'),
                               ('bcmjob', 'upload_rect deferred rows=12/240')])

    # 3b. the battery rows' counters: slp loses a doorbell in B2, lit24ui
    #     misses more frames at 24 MHz than at 30
    d = os.path.join(out, 'battery_rows')
    b = Bench()
    b.res.append(fmt_session('p19-a1b2c3d', '2026-10-25 19:00', 2, 'full'))
    slp = ('slp A1 ms=20000 ticks=20001 bells=0 sleeps=1 bad=0',
           'slp B1 ms=20000 ticks=20001 bells=41 sleeps=20990 bad=0',
           'slp B1 rings=41 bells=41 lost=0 via=fence',
           'slp B2 ms=20000 ticks=19999 bells=38 sleeps=20800 bad=0',
           'slp B2 rings=40 bells=38 lost=2 via=fence',
           'slp B2: FAIL: a doorbell was lost',
           'slp: FAIL (B phases woken on the 1 ms tick, no doorbell lost: 1 of 2; gate back off)')
    b.step('slp', 'fail', 'Yes', 82000, slp[-1], logs=slp)
    lit = ('lit24ui A30 drift_miss=2 scroll_miss=1 frames=790/140',
           'lit24ui B24 drift_miss=31 scroll_miss=9 frames=780/150',
           'lit24ui: FAIL: more frames missed at 24 MHz than at 30')
    b.step('lit24ui', 'fail', 'Yes', 150000, lit[-1], logs=lit)
    finish(b, d)
    write(d, 'bench/20261025-190000-p19-a1b2c3d.txt', ''.join(b.res))
    common(d, b)

    # 4. a resumed session, still open: a MENU stop, then a manual step
    #    left for the owner (b3 started, no result)
    d = os.path.join(out, 'resumed')
    b = Bench()
    full_session(b)
    cut = next(i for i, x in enumerate(b.res) if x.startswith('step=b3 result=start'))
    b.res = b.res[:cut + 1]
    b.res.insert(next(i for i, x in enumerate(b.res) if x.startswith('step=pb2 result=start')),
                 'resume after=port\n')
    write(d, 'bench/20261025-140200-p19-a1b2c3d.txt', ''.join(b.res))
    common(d, b)

    # 5. a hang: c2 started and never ended; the resume recorded it, and
    #    the COP steps after it skipped
    d = os.path.join(out, 'hang')
    b = Bench()
    full_session(b)
    i = next(i for i, x in enumerate(b.res) if x.startswith('step=c2 result=pass'))
    b.res[i:i + 1] = [fmt_result('c2', 'hang', None, 0, 'restarted during the step'),
                      'resume after=c2\n']
    j = next(i for i, x in enumerate(b.res) if x.startswith('step=c3 result=pass'))
    b.res[j - 2:j + 1] = [fmt_result('c3', 'skip', None, 0,
                                     'the COP worker is stuck (a COP step hung)')]
    b.res = [x for x in b.res if not x.startswith('step=c2 log=')]
    b.log = [x.split(' auto: ')[0] + ' auto: c2 hang (a restart)' if ' auto: c2 ' in x
             else x for x in b.log]
    b.log = [x.replace(' c2: PASS', ' c2: shared cache data array: start') for x in b.log]
    finish(b, d)
    write(d, 'bench/20261025-140200-p19-a1b2c3d.txt', ''.join(b.res))
    common(d, b)

    # 6. truncated files: a restart mid-write
    d = os.path.join(out, 'truncated')
    b = Bench()
    full_session(b)
    text = ''.join(b.res)
    cut = text.index('step=b13 result=pass') + len('step=b13 result=pa')
    write(d, 'bench/20261025-140200-p19-a1b2c3d.txt', '')
    write(d, 'bench/20261025-140200-p19-a1b2c3d.txt',
          text[:cut].encode() + b'\0' * 64, mode='wb')
    common(d, b)
    log = '\n'.join(b.log)
    write(d, 'bcmtest.log', (log[:len(log) * 2 // 3]).encode() + b'\0' * 512 + b'\xff\xfe',
          mode='wb')
    write(d, 'power.csv', power_csv(('ab_A1',)) + '120,40', mode='w')

    # 6b. a flat copy: the results file at the top of the folder, no bench/
    d = os.path.join(out, 'flat_root')
    b = Bench()
    b.res.append(fmt_session('4e57b3b9a1-261025', '2026-10-25 14:16', 39, 'full',
                             image=False))
    b.step('s05', 'pass', 'Yes', 301000, 'stage 5 PASS', logs=('stage 5 PASS',))
    b.step('s4b', 'pass', None, 92000, 'stage 4b PASS')
    write(d, '20261025-141650-4e57b3b9a1-261025.txt', ''.join(b.res))
    write(d, 'notes.txt', 'not a session file\n')
    common(d, b)

    # 6c. sitting 2's folder (2026-10-25): a rotated bcmtest.1.log from an
    #     older build (its own lines stamp caaa9890b0) whose stage 4 image
    #     still expected the wrapping accumulate, so X15S and REPA failed
    #     their own flags with readings that are today's model; then this
    #     build's stage 4b with the same readings, where X15S's flag fails
    #     too (an image built before the model changed)
    d = os.path.join(out, 'stale_image')
    b = Bench()
    full_session(b)
    finish(b, d)
    write(d, 'bench/20261025-224345-p19-a1b2c3d.txt', ''.join(b.res))
    common(d, b)
    old = ['boot: BCM powered (the ROM\'s vmcs) at entry',
           '     0 === BCM test, warm swap (B13): build caaa9890b0-261025 ===',
           ' 33847 stage 4: timing bench (no panel I/O)',
           ' 33861 boot bench: 37168 bytes, fnv 5c3c242d'] + \
        vrec_lines(33900, ('X15S', 'REPA'))
    write(d, 'bcmtest.1.log', '\n'.join(old) + '\n')
    cur = ['%6d stage 4b: timing bench at 150 MHz (no panel I/O)' % 100,
           '   120 boot bench_pll: 44992 bytes, fnv 9c8fedf6'] + vrec_lines(200, ('X15S',))
    write(d, 'bcmtest.log', '\n'.join(b.log + cur) + '\n')

    # 7. build A's full session: the session line without image= or kind=
    d = os.path.join(out, 'oldformat')
    b = Bench()
    full_session(b, image=False, build='4e57b3b9a1')
    finish(b, d, build='4e57b3b9a1')
    write(d, 'bench/20261025-140200-4e57b3b9a1.txt', ''.join(b.res))
    common(d, b)

    # 8-12. the drift went still: one folder per candidate
    boot_ok = '%6d boot path: active: bcmvideo (default), first frame 30100 us; marker cleared'
    drift = {
        'drift_a': ([boot_ok % 10], [
            ('bcmjob', 'up epoch=1 ops=00003fff'), ('bcmjob', 'vpu=0x3 expect=0x3'),
            ('panel', 'drift jobs mode=on'), ('bcmjob', 'fault op=0x27 why=failed'),
            ('panel', 'drift job fault at=wait rc=-5'), ('panel', 'drift still why=failed')]),
        'drift_b': ([boot_ok % 10], [
            ('bcmjob', 'up epoch=1 ops=00003fff'), ('panel', 'drift jobs mode=on'),
            ('panel', 'drift job late at=fade'), ('bcmjob', 'fault op=0x20 why=timeout'),
            ('panel', 'drift job fault at=wait rc=-4'), ('panel', 'drift still why=slow')]),
        'drift_c': ([boot_ok % 10], [
            ('bcmjob', 'up epoch=1 ops=00003fff'), ('panel', 'drift jobs mode=on')] +
            [('bcmjob', 'upload_rect deferred rows=%d/240' % (12 * k)) for k in range(1, 6)] +
            [('bcmjob', 'fault op=0x30 why=timeout')]),
        'drift_d': (['%6d boot path: fell back to Apple\'s vmcs: our DMA image failed' % 10], [
            ('panel', 'drift jobs mode=on'), ('bcmjob', 'cpu why=no jobs caps=0000'),
            ('panel', 'drift still why=off')]),
        'drift_e': ([boot_ok % 10, '%6d drill2: inject op=0x26 n=2 rc=0' % 900], [
            ('bcmjob', 'up epoch=1 ops=00003fff'), ('panel', 'drift jobs mode=on'),
            ('bcmjob', 'inject op=0x26 n=2'), ('bcmjob', 'fault op=0x26 why=failed'),
            ('bcmjob', 'fault op=0x26 why=failed'), ('panel', 'drift still why=noop')]),
        # (f): COMMIT refused before the first still; drill 2's inject comes
        # after it, so it explains nothing (the next build's st= field too)
        'drift_f': ([boot_ok % 10, '%6d drill2: inject op=0x26 n=2 rc=0' % 900], [
            ('bcmjob', 'init allowed=1 verified=0'), ('bcmjob', 'up epoch=1 ops=0000bfff'),
            ('bcmjob', 'vpu=0x1 expect=0x3 why=degraded'), ('panel', 'drift jobs mode=on'),
            ('panel', 'drift upload us=58605 rc=0'),
            ('bcmjob', 'fault op=0x22 st=21 why=failed'),
            ('bcmjob', 'resync x=160 y=0 w=160 h=240'), ('bcmjob', 'fault op=0x24 why=failed'),
            ('panel', 'drift job fault at=blit errors=1'), ('panel', 'drift still why=failed'),
            ('bcmjob', 'resync x=160 y=0 w=160 h=240'), ('bcmjob', 'inject op=0x26 n=2')]),
    }
    for name, (bl, ul) in drift.items():
        d = os.path.join(out, name)
        write(d, 'bcmtest.log', '\n'.join(bl) + '\n')
        write(d, 'ipodui.log', ui_log(ul))
        write(d, 'bcmjobs.on', '')

    # 13. P19 PW4: the clock rows and a governed play with a fallback
    d = os.path.join(out, 'vclk')
    write(d, 'bcmtest.log', '\n'.join([
        boot_ok % 10,
        '  2000 vbv 46 MHz level 5: PASS (ran 46 MHz, fallbacks 0, shown 120, '
        'errors 0, concealed 0, misses 40)',
        '  3000 vbv 140 MHz level 13: FAIL (ran 27 MHz, fallbacks 1, shown 0, '
        'errors 0, concealed 0, misses 0)',
        '  4000 vbv: FAIL: 10 of 11 bands hold; flags written',
        'player: +900 vclk band from=150 to=87 lvl=8 why=floor util=420 floor=87',
        'player: +5000 vclk band from=87 to=150 lvl=15 why=floor util=430 floor=150',
        'player: +5100 vclk band from=150 to=27 lvl=11 why=fail util=400 floor=150',
        'player: +20000 vclk sum policy=top mhz=112 lvl=11.6 idle=0 bands=3 clocks=3 '
        'lvl_changes=3 fails=1 misses=0 s=20',
        'player: +20001 vclk time 27=100 87=4100 150=15800 ms',
        '  9000 vgov clip1: PASS (misses 0, A/V max 21000 us, hitches 0 vs 0 at '
        '150, 3 changes in 20 s, top after the seek yes and the resume yes)',
        '  9001 vgov: PASS: 1 of 1 clips']) + '\n')
    write(d, 'ipodui.log', ui_log([('video', 'clock bands verified: fff')]))

    # 16. the visualizer: on the chip, then a refusal and the loud fallback
    d = os.path.join(out, 'viz_fallback')
    write(d, 'bcmtest.log', boot_ok % 10 + '\n')
    write(d, 'ipodui.log', ui_log([
        ('viz', 'start style=0 path=bcm n=512 owed'),
        ('bcmjob', 'overlay load k=0 bytes=19380 at=0012b000'),
        ('uilat', 'cpu codec=30% main=4% idle=60% s=60 ui=inline cop_idle=0%'),
        ('bcmjob', 'overlay refused why=abi'),
        ('viz', 'CPU FALLBACK why=lost n=256 fps=20')]))

    # 17. the driver's per-start line: two power-ups, the second with a
    # vector form that disagreed and the load timings measured
    d = os.path.join(out, 'bcm_starts')
    write(d, 'bcmtest.log', boot_ok % 10 + '\n')
    write(d, 'ipodui.log', ui_log([
        ('bcmjob', 'up epoch=1 ops=0020bfff'),
        ('bcmjob', 'vpu=0x3 expect=0x3'),
        ('bcmjob', 'start vpu=0x3 expect=0x3 vpu2=0xff'),
        ('bcmjob', 'up epoch=2 ops=0020bfff'),
        ('bcmjob', 'start vpu=0x3 expect=0x3 vpu2=0x7b fail=3 lane=5 got=0x12 exp=0x34 '
                   'lat=286/136@150 latw=210/98')]))

    # 15. the febe DATA-wrap garbage (builds 69945379dd..9d747741e3; an
    # unknown build counts as one): left out and counted
    d = os.path.join(out, 'febe_noise')
    write(d, 'bcmtest.log', boot_ok % 10 + '\n')
    write(d, 'ipodui.log', ui_log([('fat', 'empty slot total=3818963462'),
                                   ('ata', 'barrier flush fail=3852451844 nosup=19922716'),
                                   ('fat', 'slot prev a c09fe51cff2f0178fdffea44770f0006'),
                                   ('video', 'clock bands verified: fff')]))

    # 14. vfeed with one clip of two (sitting 3: the zip had no stage7.m4v)
    d = os.path.join(out, 'vfeed')
    write(d, 'bcmtest.log', '\n'.join([
        boot_ok % 10,
        '  2000 === video pace (vfeed) ===',
        '  2001 vfeed: 1 of 2 clips missing',
        '  2002 vfeed demanding (clip2.mp4): PASS; FEED p95 24400 us, max 39000 us, '
        'mean 17000 us; frame 41666 us (p95 58%); 0 skipped, 0 GOPs dropped; '
        'A/V max 30000 us; 150-150 MHz',
        '  2003 vfeed converted (stage7.m4v): Not available (no clip)',
        '  2004 === end: video pace ===']) + '\n')
    write(d, 'ipodui.log', ui_log([('video', 'clock bands verified: fff')]))

    # 15. RESPANEL answered with the panel on the PP (sitting 3: no overlay
    # load, no decode on the chip), and its twin where the chip decoded it
    resp = {
        'respanel_pp': [('bcmjob', 'up epoch=1 ops=0020bfff'),
                        ('art', 'panel cpu ms=20320 ok=1')],
        'respanel_chip': [('bcmjob', 'up epoch=1 ops=0020bfff'),
                          ('bcmjob', 'overlay load k=1 bytes=23884 at=0012b000 epoch=1'),
                          ('art', 'panel bcm dec_ms=900 read=later'),
                          ('panel', 'resident set key=6a1b2f700f283f50 slot=2 epoch=1')],
    }
    for name, ul in resp.items():
        d = os.path.join(out, name)
        write(d, 'bcmtest.log', '\n'.join([
            boot_ok % 10, '323706 auto: respanel pass answer=Sharp, steady ms=0 ']) + '\n')
        write(d, 'ipodui.log', ui_log(ul))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        sys.exit(2)
    gen(sys.argv[1])
