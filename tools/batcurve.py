#!/usr/bin/env python3
# ProkBox battery discharge-table fit from a developer power log (power.csv).
# Copyright (C) 2026 the ProkBox owner. GPL v2 or later.
"""Rebuild percent_to_volt_discharge[] (11 points) from a power.csv rundown.

    tools/batcurve.py POWER.CSV [--busy-w 1.5] [--disksafe 3400]
                                [--plateau MV] [--skip 300] [--json]

Method (docs/hardware.md, "Measured discharge curve"):
  * `s` resets at each boot: rows are split into boot segments there. Off
    time between boots adds nothing (self-discharge is ignored).
  * The rundown is every boot segment after the last segment that held a
    charger row at 4150 mV or more (the last real charge).
  * Charge is counted by time at a constant load, weighted like batcal:
    w = 1 + (BC_BUSY_W - 1) * busy, busy = max(bl_pct, boost_pct,
    disk_pct) / 100 (the three overlap; the max is the lower bound of the
    union). Charge is measured back from the last row (the cut-off).
  * Voltage of the first `skip` seconds after each boot is left out (rest
    recovery); their charge still counts.
  * The part above the rundown's first voltage comes from the earlier,
    partial discharges: per 40 mV band, weighted seconds per mV over every
    playing, charger-off pass, summed to the plateau.
  * 100 % = the post-surface-charge plateau (median voltage 10 to 16 min
    after the charger comes off a full charge, or --plateau). 0 % = disk-safe
    + 20 mV ("about to switch off"). The points between are equal charge steps.
  * Sanity limits as in batcal: strictly decreasing, 100 % in 3950..4250,
    0 % in 3400..3750, steps 5..250 mV.
"""
import argparse
import csv
import json
import statistics
import sys

BAND = 40


def load(path):
    rows = []
    seg = -1
    prev = None
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                s = int(r['s'])
                row = dict(s=s, mv=int(r['mv']), chg=int(r['charger']),
                           play=int(r['play']),
                           busy=max(int(r['bl_pct']), int(r['boost_pct']),
                                    int(r['disk_pct'])) / 100.0)
            except (ValueError, KeyError):
                continue
            if prev is None or s < prev:
                seg += 1
            prev = s
            row['seg'] = seg
            rows.append(row)
    if not rows:
        sys.exit('no rows')
    # row duration: gap to the previous row in the segment, capped
    gaps = [b['s'] - a['s'] for a, b in zip(rows, rows[1:])
            if b['seg'] == a['seg'] and b['s'] > a['s']]
    step = statistics.median(gaps) if gaps else 30
    for i, r in enumerate(rows):
        if i and rows[i - 1]['seg'] == r['seg']:
            r['dt'] = min(r['s'] - rows[i - 1]['s'], 2 * step)
        else:
            r['dt'] = step
        r['first'] = (i == 0 or rows[i - 1]['seg'] != r['seg'])
    return rows, step


def med_smooth(vals, half):
    out = []
    n = len(vals)
    for i in range(n):
        out.append(statistics.median(vals[max(0, i - half):i + half + 1]))
    return out


def fit(path, busy_w, disksafe, plateau_arg, skip):
    rows, step = load(path)
    # 1. the rundown: segments after the last real charge
    last_chg = max((r['seg'] for r in rows
                    if r['chg'] and r['mv'] >= 4150), default=-1)
    run = [r for r in rows if r['seg'] > last_chg]
    if len(run) < 120:
        sys.exit('rundown too short (%d rows)' % len(run))
    for r in rows:
        r['w'] = 1.0 + (busy_w - 1.0) * min(1.0, r['busy']) * 1.0
    for r in rows:
        r['t'] = r['s']     # seconds since this boot
    # 2. charge back from the cut-off (all run rows count)
    c = 0.0
    for r in reversed(run):
        r['c'] = c
        c += r['w'] * r['dt']
    q_run = c
    pts = [(r['c'], r['mv']) for r in run
           if r['t'] >= skip and not r['chg']]
    pts.sort()
    half = max(2, len(pts) // 100)
    vs = med_smooth([p[1] for p in pts], half)
    curve = [(p[0], v) for p, v in zip(pts, vs)]     # (charge to go, mV)
    v_top = curve[-1][1]
    # 3. above the rundown: per-band weighted seconds per mV
    plateau = plateau_arg or auto_plateau(rows)
    nb = int((plateau - v_top) // BAND) + 1
    bands = []
    for b in range(nb):
        lo = v_top + b * BAND
        hi = lo + BAND
        sec = 0.0
        drop = 0.0
        for a, bb in zip(rows, rows[1:]):
            if bb['seg'] != a['seg'] or bb['seg'] >= run[0]['seg']:
                continue
            if bb['chg'] or a['chg'] or not bb['play'] or bb['t'] < skip:
                continue
            if lo <= a['mv'] < hi and lo <= bb['mv'] < hi:
                sec += bb['w'] * bb['dt']
                drop += a['mv'] - bb['mv']
        bands.append((lo, hi, sec, drop))
    # rate: pooled, with a floor on the drop so noise cannot blow it up
    rates = []
    for lo, hi, sec, drop in bands:
        rates.append(sec / drop if drop >= 8 else None)
    # fill holes from the neighbours
    known = [r for r in rates if r]
    mean_rate = statistics.median(known) if known else 1.0
    rates = [r if r else mean_rate for r in rates]
    q_up = 0.0
    up_curve = []       # (charge to go, mV) above v_top
    # the band containing v_top is partly in the rundown: take from v_top
    edges = [v_top + i * BAND for i in range(nb)] + [plateau]
    q = q_run
    for i in range(nb):
        seg_mv = max(0.0, min(edges[i + 1], plateau) - edges[i])
        up_curve.append((q, edges[i]))
        q += rates[i] * seg_mv
    up_curve.append((q, plateau))
    q_tot = q
    full = curve + up_curve[1:]
    full.sort()

    def v_at(cc):
        if cc <= full[0][0]:
            return full[0][1]
        for (c0, v0), (c1, v1) in zip(full, full[1:]):
            if c0 <= cc <= c1:
                if c1 == c0:
                    return v0
                return v0 + (v1 - v0) * (cc - c0) / (c1 - c0)
        return full[-1][1]

    # 4. the table
    c0 = 0.03 * q_tot
    v0 = v_at(c0)
    floor = disksafe + 20
    if True:    # 0 % is pinned to the floor: "about to switch off"
        # charge at which the curve crosses the floor
        lo_c, hi_c = 0.0, q_tot
        for _ in range(60):
            mid = (lo_c + hi_c) / 2
            if v_at(mid) < floor:
                lo_c = mid
            else:
                hi_c = mid
        c0 = hi_c
        v0 = floor
    table = []
    for k in range(11):
        cc = c0 + (q_tot - c0) * k / 10
        table.append(v0 if k == 0 else plateau if k == 10 else v_at(cc))
    table = [int(round(v / 5.0)) * 5 for v in table]
    table[0] = int(round(v0))
    # the reference load: the run's own mean weight
    w_ref = sum(r['w'] * r['dt'] for r in run) / sum(r['dt'] for r in run)
    hours = lambda qq: qq / w_ref / 3600.0
    return dict(table=table, plateau=plateau, v_top=v_top, q_run_h=hours(q_run),
                q_tot_h=hours(q_tot), q_use_h=hours(q_tot - c0),
                reserve_h=hours(c0), w_ref=w_ref, run_rows=len(run),
                run_h=sum(r['dt'] for r in run) / 3600.0,
                bands=[(lo, hi, hours(sec), drop, rates[i])
                       for i, (lo, hi, sec, drop) in enumerate(bands)],
                full=[(hours(a), b) for a, b in full[::max(1, len(full) // 40)]],
                step_h=[hours((q_tot - c0) / 10)])


def auto_plateau(rows):
    """Playing voltage 10 to 16 min after the charger comes off a full
    charge: the highest such release (a partial top-up reads lower)."""
    best = []
    for i in range(1, len(rows)):
        a, b = rows[i - 1], rows[i]
        if a['seg'] == b['seg'] and a['chg'] and not b['chg'] \
                and a['mv'] >= 4190:
            v = [r['mv'] for r in rows[i:i + 80]
                 if r['seg'] == b['seg'] and not r['chg']
                 and 600 <= r['t'] - b['t'] <= 960 and r['play']]
            if len(v) >= 3:
                best.append(statistics.median(v))
    return int(round(max(best))) if best else 4170


def check(t):
    errs = []
    if any(a >= b for a, b in zip(t, t[1:])):
        errs.append('not strictly increasing with percent')
    if not 3950 <= t[10] <= 4250:
        errs.append('100%% %d outside 3950..4250' % t[10])
    if not 3400 <= t[0] <= 3750:
        errs.append('0%% %d outside 3400..3750' % t[0])
    for a, b in zip(t, t[1:]):
        if not 5 <= b - a <= 250:
            errs.append('step %d..%d outside 5..250' % (a, b))
    return errs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('csv')
    ap.add_argument('--busy-w', type=float, default=1.5)
    ap.add_argument('--disksafe', type=int, default=3400)
    ap.add_argument('--plateau', type=int, default=0)
    ap.add_argument('--skip', type=int, default=300)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    res = fit(a.csv, a.busy_w, a.disksafe, a.plateau, a.skip)
    errs = check(res['table'])
    if a.json:
        print(json.dumps(res))
    else:
        print('rundown: %d rows, %.1f h on-time, mean load weight %.3f'
              % (res['run_rows'], res['run_h'], res['w_ref']))
        print('run charge %.1f h, from %d mV' % (res['q_run_h'], res['v_top']))
        print('total %.1f h (plateau %d mV to cut-off), usable %.1f h, '
              'reserve %.1f h, %.2f h per 10%%'
              % (res['q_tot_h'], res['plateau'], res['q_use_h'],
                 res['reserve_h'], res['step_h'][0]))
        print('bands above the rundown (mV, h, drop, s/mV):')
        for lo, hi, h, drop, rate in res['bands']:
            print('  %4d-%4d  %5.2f h  drop %4d  %6.1f s/mV'
                  % (lo, hi, h, drop, rate))
        print('percent_to_volt_discharge = { %s }'
              % ', '.join(str(v) for v in res['table']))
    if errs:
        print('SANITY FAILED: ' + '; '.join(errs), file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
