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
"""Gate bookkeeping helpers for scripts/validate.sh (docs/gate-speed-brief.md).

  gate_tools.py seed                       write the duration seed (stdout)
  gate_tools.py order JOBS [--table F]     JOBS lines "kind<TAB>name"; prints
                                           them longest estimated first
  gate_tools.py update SCRIPTS_TSV         fold measured durations into the
                                           duration cache
  gate_tools.py ledger-add ...             append a first-attempt failure
  gate_tools.py ledger-report ...          this gate's flakes + repeat offenders
  gate_tools.py phases PHASES_TSV          print the phase timing table
  gate_tools.py slowest SCRIPTS_TSV [N]    the N slowest scripts + pool use

Files:
  tests/ui/durations.tsv                   committed seed: name<TAB>seconds
  $GATE_DURATIONS (~/.cache/rockbox-gate/durations.tsv)  learned overlay
  $GATE_LEDGER    (~/.cache/rockbox-gate/flakes.tsv)     flake ledger
"""
import argparse
import os
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SEED = os.path.join(ROOT, 'tests/ui/durations.tsv')
CACHE_DIR = os.path.expanduser('~/.cache/rockbox-gate')
OVERHEAD_S = 8          # measured: mean per-script cost over scripted time
UBSAN_FACTOR = 1.36     # measured: 72 min UBSan pass vs 52.7 min UI pass
FLOOR_S = 480           # measured: ~14 min serial, two halves in parallel ~8
DEFAULT_S = 30


def durations_path():
    return os.environ.get('GATE_DURATIONS') or os.path.join(CACHE_DIR, 'durations.tsv')


def ledger_path():
    return os.environ.get('GATE_LEDGER') or os.path.join(CACHE_DIR, 'flakes.tsv')


def scripted_seconds(path):
    """Time a script spends in its own waits and presses (the sim sleeps in
    whole ticks): wait + key/hold/chord ms + wheel clicks."""
    t = 0
    try:
        f = open(path, errors='replace')
    except OSError:
        return None
    for line in f:
        s = line.split('#')[0].strip()
        if not s:
            continue
        w = s.split()
        c = w[0]
        try:
            if c == 'wait':
                t += int(w[1])
            elif c == 'key':
                t += int(w[2]) if len(w) > 2 else 100
            elif c in ('hold', 'chord'):
                t += int(w[2])
            elif c == 'wheel':
                t += abs(int(w[1])) * max(int(w[2]) if len(w) > 2 else 40, 10)
        except (ValueError, IndexError):
            pass
    return t / 1000.0


def read_table(path):
    tab = {}
    try:
        for line in open(path):
            p = line.rstrip('\n').split('\t')
            if len(p) >= 2 and not line.startswith('#'):
                try:
                    tab[p[0]] = float(p[1])
                except ValueError:
                    pass
    except OSError:
        pass
    return tab


def table(seed=None):
    tab = read_table(seed or SEED)
    tab.update(read_table(durations_path()))
    return tab


def estimate(kind, name, tab):
    if kind == 'floor':
        return FLOOR_S
    base = tab.get(name)
    if base is None:
        sc = scripted_seconds(os.path.join(ROOT, 'tests/ui', name + '.script'))
        base = (sc + OVERHEAD_S) if sc is not None else DEFAULT_S
    return base * (UBSAN_FACTOR if kind.startswith('ubsan') else 1.0)


def cmd_seed(a):
    names = sorted(os.path.basename(p)[:-7] for p in
                   (os.path.join(ROOT, 'tests/ui', f) for f in os.listdir(os.path.join(ROOT, 'tests/ui')))
                   if p.endswith('.script'))
    print('# name<TAB>seconds: scripted time + %d s; seed for the gate pool order.' % OVERHEAD_S)
    print('# The pool learns from real runs: ~/.cache/rockbox-gate/durations.tsv')
    for n in names:
        sc = scripted_seconds(os.path.join(ROOT, 'tests/ui', n + '.script')) or 0
        print('%s\t%d' % (n, round(sc + OVERHEAD_S)))


def cmd_order(a):
    tab = table(a.table)
    rows = []
    for i, line in enumerate(open(a.jobs)):
        p = line.rstrip('\n').split('\t')
        if len(p) < 2:
            continue
        rows.append((-estimate(p[0], p[1], tab), i, p[0], p[1]))
    rows.sort()
    for neg, _i, kind, name in rows:
        print('%s\t%s\t%d' % (kind, name, -neg))


def cmd_update(a):
    cur = read_table(durations_path())
    seed = read_table(SEED)
    n = 0
    for line in open(a.tsv):
        p = line.rstrip('\n').split('\t')
        # name worker start sim trace golden total attempt kind
        if len(p) < 9 or p[8] != 'ui' or p[7] != '1':
            continue
        try:
            meas = float(p[6])
        except ValueError:
            continue
        old = cur.get(p[0], seed.get(p[0]))
        cur[p[0]] = meas if old is None else 0.5 * old + 0.5 * meas
        n += 1
    os.makedirs(os.path.dirname(durations_path()), exist_ok=True)
    tmp = durations_path() + '.tmp%d' % os.getpid()
    with open(tmp, 'w') as f:
        for k in sorted(cur):
            f.write('%s\t%.1f\n' % (k, cur[k]))
    os.replace(tmp, durations_path())
    print('durations: %d script times folded into %s' % (n, durations_path()))


def cmd_ledger_add(a):
    p = ledger_path()
    os.makedirs(os.path.dirname(p), exist_ok=True)
    reason = ' '.join(a.reason.split())[:200]
    with open(p, 'a') as f:
        f.write('\t'.join([time.strftime('%Y-%m-%dT%H:%M:%S'), a.gate_id, a.commit,
                           a.script, a.kind, a.retry, reason]) + '\n')


def cmd_ledger_report(a):
    rows = []
    try:
        for line in open(ledger_path()):
            p = line.rstrip('\n').split('\t')
            if len(p) >= 7:
                rows.append(p)
    except OSError:
        pass
    mine = [r for r in rows if r[1] == a.gate_id]
    print('flake ledger: %d first-attempt failure(s) this gate (%s)'
          % (len(mine), ledger_path()))
    for r in mine:
        print('  %-8s %-8s %s: %s' % (r[4], 'retry-' + r[5], r[3], r[6][:110]))
    ids = []
    for r in rows:
        if r[1] not in ids:
            ids.append(r[1])
    last = ids[-a.last:]
    cnt = {}
    for r in rows:
        if r[1] in last and r[4] in ('flaky', 'starved'):
            cnt.setdefault(r[3], set()).add(r[1])
    bad = sorted(k for k, v in cnt.items() if len(v) >= 2)
    if bad:
        print('flake ledger: needs a fix (flaked in 2+ of the last %d gates): %s'
              % (a.last, ' '.join(bad)))


def cmd_phases(a):
    rows = []
    for line in open(a.tsv):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 4:
            rows.append((p[0], float(p[3])))
    tot = sum(s for _n, s in rows) or 1
    print('gate timing (build/gate-timing/phases.tsv):')
    for n, s in rows:
        if s >= 1:
            print('  %6.1f min %4.0f%%  %s' % (s / 60, 100 * s / tot, n))
    print('  %6.1f min total' % (tot / 60))


def cmd_slowest(a):
    rows = []
    for line in open(a.tsv):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 9:
            try:
                rows.append((float(p[6]), p[0], p[8], p[7]))
            except ValueError:
                pass
    rows.sort(reverse=True)
    tot = sum(r[0] for r in rows)
    print('gate scripts: %d runs, %.1f worker-minutes; slowest:' % (len(rows), tot / 60))
    for s, n, k, att in rows[:a.n]:
        print('  %6.0f s  %-6s %s%s' % (s, k, n, '' if att == '1' else ' (attempt %s)' % att))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('seed').set_defaults(f=cmd_seed)
    s = sub.add_parser('order'); s.add_argument('jobs'); s.add_argument('--table')
    s.set_defaults(f=cmd_order)
    s = sub.add_parser('update'); s.add_argument('tsv'); s.set_defaults(f=cmd_update)
    s = sub.add_parser('ledger-add')
    for k in ('gate-id', 'commit', 'script', 'kind', 'retry', 'reason'):
        s.add_argument('--' + k, required=True)
    s.set_defaults(f=cmd_ledger_add)
    s = sub.add_parser('ledger-report'); s.add_argument('--gate-id', required=True)
    s.add_argument('--last', type=int, default=5); s.set_defaults(f=cmd_ledger_report)
    s = sub.add_parser('phases'); s.add_argument('tsv'); s.set_defaults(f=cmd_phases)
    s = sub.add_parser('slowest'); s.add_argument('tsv')
    s.add_argument('n', nargs='?', type=int, default=10); s.set_defaults(f=cmd_slowest)
    a = ap.parse_args()
    a.f(a)


if __name__ == '__main__':
    main()
