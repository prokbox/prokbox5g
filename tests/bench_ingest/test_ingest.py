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

"""scripts/bench_ingest.py over the synthesized folders in fixtures/
(gen_fixtures.py): each case runs the tool isolated (python3 -I), as the
orchestrator does, and checks what it prints; the passing re-verify run's
patch must apply to this tree (git apply --check)."""
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(ROOT, 'scripts', 'bench_ingest.py')
FX = os.path.join(HERE, 'fixtures')

failed = 0


def run(name, *extra):
    r = subprocess.run([sys.executable, '-I', TOOL, os.path.join(FX, name),
                        '--mkdevtest', 'none'] + list(extra),
                       capture_output=True, text=True, cwd=tempfile.gettempdir())
    return r.returncode, r.stdout, r.stderr


def check(name, cond, what):
    global failed
    print('%-4s %s: %s' % ('ok' if cond else 'FAIL', name, what))
    if not cond:
        failed += 1


def top(out):
    """The failures-and-anomalies block"""
    return out.split('\n== session', 1)[0].split('== FAILURES AND ANOMALIES', 1)[-1]


def case(name, want_rc, musts=(), nots=(), tops=(), extra=()):
    rc, out, err = run(name, *extra)
    check(name, not err.strip() or 'Warning' not in err, 'no Python warnings or tracebacks'
          + ('' if not err.strip() else ': ' + err.strip().splitlines()[-1]))
    check(name, 'Traceback' not in err, 'no traceback')
    check(name, rc == want_rc, 'exit %d (got %d)' % (want_rc, rc))
    for m in musts:
        check(name, m in out, 'prints %r' % m)
    for m in nots:
        check(name, m not in out, 'does not print %r' % m)
    for m in tops:
        check(name, m in top(out), 'flags %r first' % m)
    return out


LINE = ("bcmvideo_jobs cd8e5d96  stage 6 PASS (merged, 218660 bytes), B5 PASS, "
        "jobs self-test PASS, J_VPU 3 PASS, drift B3 PASS on the owner's 5.5G: ")

case('full_pass', 0,
     musts=('mode: full; build p19-a1b2c3d', 'image line: image=218660 fnv=cd8e5d96 kind=merged',
            'verified ' + LINE + 'full session 2026-10-25, build p19-a1b2c3d',
            'end line: end passed=29 failed=0 skipped=11',
            'replayed: 29 passed, 0 failed, 11 skipped',
            '  X15M pass  x15 / vmulhd 32-bit sum: exact', 'not in this build\'s records: BARV',
            'video: port KB, MB/s (busy time) (812, 11)', 'jvpu2: jvpu: J_VPU 3, expected 3',
            'lab5: J_VPU 7 (display-only image)', 'floor dev audio=39845888 bcm_res=218112',
            'drift jobs mode=on', 'no slp or lit24ui counter lines', 'bcmpass5.txt   5 hello_dma 00001f50 4326f8dc',
            'mark=ab_A1', 'patch: not made (--mkdevtest none)',
            'rows above y=20 are refreshed only by the "clock" rect'),
     tops=('none',))
case('reverify_earns', 0,
     musts=('mode: reverify; build 4e57b3b9a1',
            'verified ' + LINE + 're-verify session 2026-10-25, build 4e57b3b9a1',
            '  s05     skip',
            'jvpu: jvpu: forms BSL1 ok BSF1 ok BSM1 differs',
            'jvpu: jvpu: forms: first differing BSM1 lane 7 got 00ff exp 0047',
            'jvpu: jvpu: sdram byte 41.2 ns word 38.0 ns; sram byte 7.4 ns'))
# sitting 3 (2026-10-26): a must-pass step that skipped is flagged first,
# with the rc 1 cause and the restart that never came; the later session
# (the reason named, the restart re-offered, jobs passed) flags only the
# owner going on without the restart, and the replay counts it once
_o = case('need_skip', 1,
          tops=('20261026-094717-7e8c140e40-261026.txt jobs: a must-pass step SKIPPED '
                '(job self-test rc 1)',
                'note: (empty: the step gave no reason)',
                'cause: the job self-test answered rc 1, "unavailable"',
                'the restart step boot was recorded done with no restart check',
                'no new start between "auto: boot" and "auto: jobs": the restart never '
                'happened',
                'p19-s3.txt boot: the owner went on without the restart it asked for'),
          musts=('mode: reverify; build p19-s3', '  pre     done', '  boot    done'))
check('need_skip', [l for l in top(_o).splitlines() if l.startswith('* ')][:1] ==
      ['* bench/20261026-094717-7e8c140e40-261026.txt jobs: a must-pass step SKIPPED '
       '(job self-test rc 1)'], 'the skipped must-pass step is the first anomaly')
check('need_skip', 'p19-s3.txt jobs: a must-pass' not in _o,
      'a passed must-pass step is not flagged')
# sitting 4c's runner: the steps that skipped by themselves, listed by the
# step they needed; no "did not fail" disagreement; s6's failure costs the
# line, and no must-pass step skipped
_o = case('autoskip', 1,
          musts=('mode: reverify; build p19-s4c',
                 'auto-skipped after s6 fail: vfeed, s7, vbv, vgov, vplay',
                 'auto-skipped after ucop fail: uck1, uck2, ufeel, upanel, ucop2',
                 '  vfeed   skip', 'auto-skip: needs s6 (fail)',
                 'no line: step s6 fail'),
          tops=('p19-s4c.txt s6: FAIL',),
          nots=('which did not fail', 'a must-pass step SKIPPED',
                'verified bcmvideo_jobs'))
case('portcheck', 1,
     musts=('mode: portcheck; build p19-a1b2c3d', '  cr1     FAIL',
            'no line: the port write check earns no line'),
     tops=('cr1: FAIL: region not writable after start',),
     nots=('verified bcmvideo_jobs',))
case('failed_step', 1,
     musts=('  b5      FAIL   Sheared *', '  scroll  FAIL   Stale pixels *',
            'bcmjob resync x=0 y=20 w=320 h=220', 'no line: step b5 fail'),
     tops=('b5: FAIL', 'question: Was the white frame straight and whole?',
           'answer: Sheared (the good one is "Yes")', 'lab5: PASS (91 pushes',
           'scroll: FAIL', 'answer: Stale pixels', 'auto: scroll fail answer=Stale pixels',
           'bench check ASHG (stage 4b): FAIL', 'lane offset is not as modelled (ASHG'),
     nots=('verified bcmvideo_jobs',))
# sitting 2 (2026-10-25): a rotated log's older build is labelled and not
# counted; a FAIL flag over a reading that is today's model is 'stale'
# (the image's expected value), never "FAIL ... (as modelled)"
_o = case('stale_image', 1,
          musts=('-- stage 4 (bcmtest.1.log, bench fnv 5c3c242d, build caaa9890b0, an older '
                 'build than a1b2c3d: not counted)',
                 '  X15S stale ', '  REPA stale ',
                 '-- stage 4b (bcmtest.log, bench fnv 9c8fedf6, no build stamp)'),
          tops=("bench check X15S (stage 4b): the image's expected value is older than "
                'the model', 'rebuild the bench image'),
          nots=('X15S FAIL', 'REPA FAIL'))
check('stale_image', 'bench check REPA' not in top(_o) and 'caaa9890b0' not in top(_o),
      "the older build's checks stay out of the failures")
case('battery_rows', 1,
     musts=('battery rows: slp doorbells, lit24ui missed frames',
            'slp B1 rings=41 bells=41 lost=0 via=fence',
            'slp B2 rings=40 bells=38 lost=2 via=fence', 'slp B2: FAIL: a doorbell was lost',
            'lit24ui A30 drift_miss=2 scroll_miss=1 frames=790/140',
            'lit24ui B24 drift_miss=31 scroll_miss=9 frames=780/150',
            '  slp     FAIL', '  lit24ui FAIL'))
case('resumed', 1,
     musts=('resumed 2 times: after port, boot',
            '  b3      pending', 'end line: none (the session is still open',
            'no line: the session has not ended'),
     tops=('b3: started, never ended', 'a resume records it when the owner finishes it'))
case('hang', 1,
     musts=('  c2      HANG', '  c3      skip   -                             0  the COP '
            'worker is stuck'),
     tops=('c2: HANG', 'note: restarted during the step', 'auto: c2 hang (a restart)',
           'c2: shared cache data array: start'))
case('truncated', 1,
     tops=('b13: started, never ended', 'a resume records it as hung',
           'not a results-file line (corrupt or cut)', '"step=b13 result=pa"',
           'bcmtest.log: 512 NUL bytes', 'bcmtest.log: bytes that are not text',
           'power.csv: the last line has no end'))
case('oldformat', 0, extra=('--commit', '4e57b3b9a1'),
     musts=('step table: 4e57b3b9a1', 'image line: none (a file from before the image fields',
            'this build\'s full session earns no line (the re-verify run does)'),
     nots=('verified bcmvideo_jobs',))
case('oldformat', 0,
     musts=('no line: the image is not the merged one (kind=absent)',))
# P19 PW4: the video clock section
case('vclk', 1,
     musts=('== video clock: the chip\'s bands and governor',
            'band moves by why: fail 1, floor 2',
            'play (in-band policy top): mean 112 MHz, level 11.6, idle 0 permille, '
            '3 band moves, 3 clock changes (3 of level), 1 fallbacks, 0 misses, 20 s',
            'vclk time 27=100 87=4100 150=15800 ms', 'vgov clip1: PASS'),
     tops=('video clock: the chip fell back to the crystal',
           'video clock: a clock row failed or a governed play missed'))
case('drift_a', 1, musts=('no vclk, vbv or vgov lines',))
# the visualizer: its lines listed, a CPU fallback flagged first
case('viz_fallback', 1, musts=('== visualizer: on the chip, the swap, the CPU',
                               'starts: 1 (bcm); overlay loads: 1; cpu samples: 1'),
     tops=('visualizer: off the chip',))
# the driver's per-start line, one row a power-up
case('bcm_starts', 1, musts=('== BCM starts: vpu, vpu2, load timings',
                             'start 1: vpu=0x3 expect=0x3 vpu2=0xff',
                             'start 2: vpu=0x3 expect=0x3 vpu2=0x7b fail=3 lane=5 got=0x12 '
                             'exp=0x34 lat=286/136@150 latw=210/98'))
# the febe DATA-wrap garbage lines: left out for an affected or unknown build
case('febe_noise', 1, musts=('== ignored log lines', '3 "fat empty slot / fat slot / '
                             'ata barrier flush / ata flush" lines left out'),
     nots=('empty slot total=3818963462',))
# vfeed: a run with one clip of its two is flagged, the missing one named
case('vfeed', 1,
     musts=('== video pace: vfeed', 'run 1: 1 of 2 clips played; missing: converted (stage7.m4v)',
            'run 1 played 1 of 2 clips; untested this sitting: converted (stage7.m4v)'),
     tops=('video pace: vfeed played fewer than its 2 clips',))
case('vclk', 1, musts=('no vfeed lines (the pace row did not run)',))
# RESPANEL answered with the panel on the PP (sitting 3) is flagged; with
# the overlay loaded and the panel on the chip it is not
RESP_PP = 'respanel: DECODE on the chip untested: the panel was decoded on the PP'
case('respanel_pp', 1, musts=('art panel cpu ms=20320',), tops=(RESP_PP,))
case('respanel_chip', 1, nots=(RESP_PP,),
     musts=('bcmjob overlay load k=1 bytes=23884',))

for name, verdict in (('drift_a', '(a) fits: a job marked failed, so D-1\'s still drift; '
                       'op AVG (0x27)'),
                      ('drift_b', '(b) fits: a frame\'s PRESENT/FENCE never completed'),
                      ('drift_c', '(c) fits: the job owner\'s feed waited on frame_due'),
                      ('drift_d', '(d) fits: jobs wanted (bcmjobs.on) but the merged image '
                       'is not the resident display driver'),
                      ('drift_e', '(e) fits: drill 2 injected FADE failures'),
                      ('drift_f', '(f) fits: the slot upload was refused: COMMIT (0x22) '
                       'failed st=21 (J_E_SLOT) before the first still (panel drift still '
                       'why=failed); then BLIT (0x24) on the uncommitted slot')):
    out = case(name, 1, musts=('verdict: ' + verdict,), tops=('drift still', verdict))
check('drift_e', '[first drift still:' in out or 'lines from the first drift still' in out,
      'drill 2\'s inject placed against the first drift still')

case('flat_root', 0,
     musts=('== session 20261025-141650-4e57b3b9a1-261025.txt',
            'mode: full; build 4e57b3b9a1-261025', '  s05     pass   Yes'),
     nots=('no results file', 'notes.txt'))
out = case('drift_f', 1, musts=('[st=21 (J_E_SLOT)]', 'bcmjob init allowed=1 verified=0'),
           nots=('(e) fits',))

# --corrections: the owner's corrected answer beside the device's, never
# rewriting it; a malformed line stops the run
with tempfile.TemporaryDirectory() as d:
    c = os.path.join(d, 'corr.txt')
    with open(c, 'w') as f:
        f.write('# sitting 1\n* b5 Yes owner: the by-design case (test)\n')
    r = subprocess.run([sys.executable, '-I', TOOL, os.path.join(FX, 'failed_step'),
                        '--mkdevtest', 'none', '--corrections', c],
                       capture_output=True, text=True)
    check('corrections', '== OWNER CORRECTIONS (1)' in r.stdout and
          'b5: device answer Sheared, corrected to Yes: owner: the by-design case' in r.stdout,
          'listed in their own section')
    check('corrections', 'corrected answer: Yes (device: Sheared)' in r.stdout,
          'shown beside the device answer in the table')
    check('corrections', '(the good one is "Yes")' not in r.stdout.split('b5: FAIL', 1)[-1]
          .split('\n* ', 1)[0] and 'owner-corrected to "Yes"' in r.stdout,
          'no bad answer flagged; the device FAIL marked')
    with open(c, 'w') as f:
        f.write('* b5 Yes\n')
    r = subprocess.run([sys.executable, '-I', TOOL, os.path.join(FX, 'failed_step'),
                        '--mkdevtest', 'none', '--corrections', c],
                       capture_output=True, text=True)
    check('corrections', r.returncode != 0 and 'corr.txt:1:' in (r.stderr + r.stdout),
          'a malformed line stops the run')

# a FALLBACK line heads the anomalies, whatever else the folder holds
with tempfile.TemporaryDirectory() as d:
    with open(os.path.join(d, 'ipodui.log'), 'w') as f:
        f.write('1 panel drift upload us=58605 rc=0\n'
                '2 panel FALLBACK drift left the video chip (commit): last resort, '
                'a chip bug to fix\n')
    r = subprocess.run([sys.executable, '-I', TOOL, d, '--mkdevtest', 'none'],
                       capture_output=True, text=True)
    blk = r.stdout.split('== FAILURES AND ANOMALIES', 1)[-1]
    first = [l for l in blk.splitlines()[1:] if l.startswith('* ')][:1]
    check('fallback', r.returncode == 1 and first and first[0].startswith('* FALLBACK: 1 line')
          and 'ipodui.log:2:' in blk, 'a FALLBACK line is the first anomaly')

# untrusted input: a control sequence in a log line is escaped, never printed raw
with tempfile.TemporaryDirectory() as d:
    with open(os.path.join(d, 'ipodui.log'), 'wb') as f:
        f.write(b'1 panel drift still why=\x1b[2Jcleared\n')
    r = subprocess.run([sys.executable, '-I', TOOL, d, '--mkdevtest', 'none'],
                       capture_output=True, text=True)
    check('escape', '\x1b' not in r.stdout and '\\x1b[2Jcleared' in r.stdout,
          'control characters escaped')
    check('escape', r.returncode == 1 and 'Traceback' not in r.stderr, 'no results file: '
          'reported, exit 1')

# the patch: made in a temporary worktree, applies to this tree
with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, 'v.patch')
    r = subprocess.run([sys.executable, '-I', TOOL, os.path.join(FX, 'reverify_earns'),
                        '--mkdevtest', 'render', '--patch', p], capture_output=True, text=True)
    check('patch', r.returncode == 0 and os.path.exists(p), 'written (rc %d) %s' % (
        r.returncode, r.stderr.strip()[-200:]))
    text = open(p).read() if os.path.exists(p) else ''
    check('patch', '+' + LINE + 're-verify session 2026-10-25, build 4e57b3b9a1' in text,
          'adds the line to bcmfw/verified.txt')
    # the patch adds one line to however many verified.txt lists today
    # (its token lines, clock150, stay out of the boot path's list:
    # mkdevtest.py TOKENS)
    n_ver = sum(1 for l in open(os.path.join(ROOT, 'bcmfw', 'verified.txt'))
                if l.split('#', 1)[0].strip()
                and l.split()[0] not in ('clock150',)) + 1
    check('patch', '+    0xcd8e5d96,   /* bcmvideo_jobs' in text and
          '+const int bcmdt_n_verified = %d;' % n_ver in text,
          'regenerates bcm-devtest-images.c')
    # the retag reaches the live docs only while they still carry a pending
    # row (a session's docs commit retags them by hand); doc_edits itself is
    # checked below on a fixed text, whatever the docs say today
    pending = any('[silicon] pending (' in open(os.path.join(ROOT, f)).read() or
                  '**Silicon pending**: bench' in open(os.path.join(ROOT, f)).read()
                  for f in ('docs/bcm2722-reference/isa-vector.md',
                            'docs/bcm2722-reference/quirks.md'))
    if pending:
        check('patch', '[silicon] as modelled (' in text and
              'bench re-verify session 2026-10-25' in text,
              'turns the docs\' [silicon] pending into the session\'s evidence')
    a = subprocess.run(['git', '-C', ROOT, 'apply', '--check', p], capture_output=True,
                       text=True)
    check('patch', a.returncode == 0, 'applies cleanly to this tree %s' % a.stderr.strip())
    w = subprocess.run(['git', '-C', ROOT, 'worktree', 'list'], capture_output=True, text=True)
    check('patch', 'bench-ingest-' not in w.stdout, 'its temporary worktree is gone')

# doc_edits on a fixed text: pending rows whose checks all came back as
# modelled are retagged, a not-as-modelled one is left for a writer
_spec = importlib.util.spec_from_file_location('bench_ingest', TOOL)
_bi = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_bi)
_doc = ('| vclip | [silicon] pending (VCLP) |\n'
        '| acc x15 | [silicon] pending (X15S) |\n'
        'The rounding. **Silicon pending**: bench MHDU and R48E.\n')
_v = {'VCLP': ('pass', 'ok', True), 'X15S': ('FAIL', 'neither reading', False),
      'MHDU': ('pass', 'ok', True), 'R48E': ('pass', 'ok', True)}
_new, _sugg = _bi.doc_edits(_doc, _v, 'bench re-verify session 2026-10-25')
check('doc_edits', '| vclip | [silicon] as modelled (VCLP; bench re-verify session '
      '2026-10-25) |' in _new, 'a modelled pending row is retagged')
check('doc_edits', '[silicon] pending (X15S)' in _new and any('X15S' in s for s in _sugg),
      'a not-as-modelled row stays pending and is listed for a writer')
check('doc_edits', '**Silicon** (bench re-verify session 2026-10-25): bench MHDU, R48E '
      'as modelled' in _new, 'the bold form is retagged too')

# Bench row ALGN (bcmfw/bench/algnlog.py): a stage 4 run's ALGN lines read
# through readlog into bench_checks' verdicts: the control as modelled, a
# store that lands at its exact address flagged, a port write 2 bytes low
# named, a hung check reported
class _Ln:
    def __init__(self, text, src='bcmtest.log'):
        self.text, self.src = text, src


_algn = ['   100 stage 4: timing bench (no panel I/O)',
         '   900 ALGN n 3 faults 2 src 00009c20 buf 00009cc0 in 812 us',
         '   901 ALGN 0 LW1C 00010103 00005734 0000572a 13121110 00000000 00000003 00000003',
         '   902 ALGNB 0 a5a5a5a0 a5a5a5a1 a5a5a5a2 a5a5a5a3 a5a5a5a4 a5a5a5a5 a5a5a5a6 a5a5a5a7',
         '   903 ALGN 1 SWR2 00000001 00000000 0000585e c3c2c1c0 00000000 00000003 00000003',
         '   904 ALGNB 1 c1c0a5a0 a5a5c3c2 a5a5a5a2 a5a5a5a3 a5a5a5a4 a5a5a5a5 a5a5a5a6 a5a5a5a7',
         '   905 ALGN stopped at check 2 (VS62): no answer in 2 s (hung in it); checks after',
         '   906 ALGNP 1 PWD2 bad 129 low2 128 of 160 +20 0102 f010',
         '   907 ALGNP 13 R604 bad 0 low2 0 of 1224',
         '   908 ALGNP 14 R608: port error -1']
_runs = _bi.bench_checks([_Ln(x) for x in _algn])
_av = _runs[0][1] if _runs else {}
check('algn', _av.get('LW1C', ('', '', False))[2], 'the control reads as modelled: %s'
      % (_av.get('LW1C'),))
check('algn', 'SWR2' in _av and not _av['SWR2'][2] and 'exact address' in _av['SWR2'][1],
      'a store landing unaligned is named and flagged: %s' % (_av.get('SWR2'),))
check('algn', 'PWD2' in _av and _av['PWD2'][2] and '2 bytes low' in _av['PWD2'][1],
      'the port write at +2 lands 2 bytes low, as expected: %s' % (_av.get('PWD2'),))
check('algn', _av.get('R604', ('', '', False))[2] and _av.get('VS62', ('',))[0] == 'hung',
      'R604 exact; the hung check reported: %s %s' % (_av.get('R604'), _av.get('VS62')))

check('algn', _av.get('R608', ('',))[0] == 'error' and not _av['R608'][2] and
      'R64W, E2ND' in _av['R608'][1],
      'a port error is reported, with the checks it kept from running: %s'
      % (_av.get('R608'),))

print('bench_ingest: %s' % ('PASS' if not failed else '%d FAILED' % failed))
sys.exit(1 if failed else 0)
