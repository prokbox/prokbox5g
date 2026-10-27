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

"""The video chip's load governor in simulation: Apple's against ours, one
improvement at a time (P19 PW4; docs/research/bcm2722/battery-levers.md
section 4).

Workloads (core cycles, each with its release and deadline):
  clip1, clip2  the stage 6 clips' FEED times at 150 MHz [silicon, session
                27's V6 lines, tests/vclk/traces/], as cycles; a FEED goes
                out 35 ms before its picture (vpcore.h VPC_LEAD_US) and is
                late past half a frame after it (vtrace.c's hitch rule);
                played 4 times over
  ui            a minute of the lit shell on the merged image: the drift's
                steps (7.4 a second: FADE, AVG, the pan, SHADE; COMMIT a
                cover a cycle), the marquee (20 a second, 15 % of the time),
                a retained slide every 15 s (9 frames of 33 ms), and one
                album-art DECODE (8.6 M instructions, 150 ms cap) [sim
                costs, jobs.md section 10 and album-render.md]
  undeclared    ui plus 10 s of load no client declares (25 frames a
                second of 1.2 M cycles: a game or visualizer on the chip)

Governors (every 10,000 us on the 1 MHz counter, busy time measured):
  fixed150, fixed27   today's video session and today's UI
  apple       pm_set_policy min (@fe00): +6 % when busy (util >= 0.9), -6 %
              when it idled, whole MHz, 5-150, any clock at its table level
              [apple; the busy threshold is our hypothesis]
  +prop       f_next = f x util / 0.8 (schedutil's target)
  +fast       util >= 0.95: straight to the top; down one band at most a
              window
  +bands      targets snapped up to the top clock of each of Apple's level
              bands (27 on the crystal at level 5, then 46, 59, 74, 87,
              102, 116, 123, 130, 140, 147, 150)
  +dwell      a band held at least 100 ms before it goes down; no relock
              inside a band
  +floors     the clients' floors: the video session's (a miss raises it a
              band above the clock that missed; 3 s without one lowers it a
              band; it starts at the cost model's mean, vclk_fit.py) and a
              DECODE burst's full speed for its run
  floors only the clients' floors, banded, and the crystal otherwise
  floors+net  floors only, with a safety net for undeclared load: a
              sustained overload (the busy share's EWMA >= 0.9) goes to the
              top for 500 ms at a time
  floors|gov (v1)  a client's floor when declared, else 1-4 + EWMA (this
              branch's first design)
  2-layer     ours (apps/video/vclk.c, read every 100 ms): the band
              selector (up at 95 % of the band's top, down when the band
              below carries it at 80 %, 100 ms dwell, the floors a minimum
              band) and the in-band policy, top (the band's top clock, the
              default) or prop (load / 0.8 inside the band)

Every change costs what the image's clock_set does: 1 ms of PLL lock on
the crystal, and 10 ms of regulator settle (busy-waiting) for each level
change, before a clock goes up and after it came down [apple @5134].

The energy proxy (scripts/bcm_power_model.py's figures, [estimate]):
P = k_bcm x f x (V / 1.2)^2 x (1 busy | sleep_factor asleep) + leak x
(V / 1.04)^2, V = 0.80 + 0.04 x (level - 5) [hypothesis: levels 5-15 span
the brief's 0.8-1.2 V evenly], as mA at the battery.  Idle is asleep (the
gated idle sleep) or polling (--poll: idle burns like busy).

Usage: scripts/vclk_gov_sim.py [--poll] [--xtal5] [--json] [--trace NAME]
Prints, per workload and governor: mean MHz, mean level, the proxy (mA),
misses, clock and level changes.  No dependencies."""

import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import bcm_power_model as PM  # noqa: E402

WIN = 10000                     # us, Apple's window
LEAD = 35000                    # vpcore.h VPC_LEAD_US
PLL_US = 1000
REG_US = 10000

# @141ba: (up to Hz, level), ascending
APPLE_LEVELS = [(46875000, 5), (59665292, 6), (74840572, 7), (87268380, 8),
                (102016607, 9), (116539140, 10), (123607364, 11),
                (130841121, 12), (140183656, 13), (147075854, 14),
                (154130133, 15), (160919540, 15)]
BANDS = [27, 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150]


# the crystal's level: 11 as a host start leaves it (today), 5 with PW3's
# lever (--xtal5); the bands above it take Apple's
XTAL_LEVEL = [11]


def level_for(mhz):
    if mhz == 27:
        return XTAL_LEVEL[0]
    for hz, lvl in APPLE_LEVELS:
        if mhz * 1e6 <= hz:
            return lvl
    return 15


def volts(level):
    return 0.80 + 0.04 * (level - 5)


def power_mw(f, level, busy_frac, idle_factor):
    k = PM.PARAMS['k_bcm'][0]
    leak = PM.PARAMS['p_leak11'][0]
    v = volts(level)
    dyn = k * f * (v / 1.2) ** 2
    return dyn * (busy_frac + (1 - busy_frac) * idle_factor) + leak * (v / 1.04) ** 2


def to_ma(mw):
    return mw / PM.PARAMS['eta'][0] / PM.PARAMS['v_batt'][0]


# ---- workloads: [(release_us, deadline_us, cycles, kind)] -------------------

def clip(name, fps, repeat=4):
    path = os.path.join(ROOT, 'tests', 'vclk', 'traces', name + '.txt')
    rows = [tuple(map(int, l.split())) for l in open(path) if l[0] != '#']
    frame = 1e6 / fps
    jobs = []
    span = (rows[-1][0] + 1000 / fps) * 1000
    for r in range(repeat):
        for k, (pts_ms, cyc) in enumerate(rows):
            if r and k == 0:
                cyc = rows[1][1] * 2            # the first frame's set-up once
            pts = r * span + pts_ms * 1000 + 500000
            jobs.append((pts - LEAD, pts + frame / 2, cyc, 'video'))
    return jobs, frame


def ui(seconds=60):
    jobs = []
    step = 135000
    t = 0
    n = 0
    while t < seconds * 1e6:
        # a 7 s cover cycle: 8 fades and a COMMIT, the rest pans and halves
        k = n % 52
        cyc = (115428 if k < 8 else 133641 + 20858) + 6537
        if k == 0:
            cyc += 315979
        jobs.append((t, t + step, cyc, 'drift'))
        t += step
        n += 1
    t = 0
    while t < seconds * 1e6:                    # the marquee, 15 % of time
        if (t // 1000000) % 20 < 3:
            jobs.append((t, t + 50000, 7025, 'marquee'))
        t += 50000
    for s in range(5, seconds, 15):             # a retained slide
        for f in range(9):
            r = s * 1e6 + f * 33333
            jobs.append((r, r + 33333, 211000, 'slide'))
    jobs.append((30e6, 30e6 + 150000, 8600000, 'decode'))
    jobs.sort()
    return jobs, None


# ---- governors -------------------------------------------------------------

def band_of(mhz):
    for k, b in enumerate(BANDS):
        if b >= mhz - 1e-9:
            return k
    return len(BANDS) - 1


class Gov:
    """one governor; each improvement a switch (the module's docstring)"""
    def __init__(self, name, start, apple=False, prop=False, fast=False,
                 bands=False, dwell_ms=0, floors=False, fixed=None,
                 ewma=0.0, thr=0.9, target=0.8, floors_only=False,
                 net=False, split=False):
        self.name, self.f, self.apple, self.prop, self.fast = name, start, apple, prop, fast
        self.bands, self.dwell, self.floors, self.fixed = bands, dwell_ms * 1000, floors, fixed
        self.ewma, self.thr, self.target = ewma, thr, target
        self.u = 1.0
        self.floors_only = floors_only
        self.net = net
        self.split = split
        self.net_until = 0
        self.last_change = -1e18

    def decide(self, now, util, floor, top=150):
        if self.fixed:
            return self.fixed
        if self.split and floor > 0:
            # a client declared its load: its floor, nothing else (the
            # util path would chase the frames' bursts)
            self.u = 1.0
            return BANDS[band_of(floor)]
        if self.floors_only:            # the clients' floors, else 27
            want = BANDS[band_of(max(27, floor))]
            if self.net:
                # the safety net for load no client declared: a sustained
                # overload (the 10 ms busy share's EWMA, 1/4) goes to the
                # top at once; back to the floors after 500 ms of EWMA
                # under one half
                self.u += (util - self.u) * 0.25
                if self.u >= 0.9:
                    self.net_until = now + 500000
                if now < self.net_until or (self.f > want and self.u >= 0.5
                                            and self.net_until > 0):
                    return top if now < self.net_until else self.f
            return want
        f = self.f
        if self.ewma:
            self.u += (util - self.u) * self.ewma
            util = self.u
        if not self.prop:               # Apple's: 6 %, whole MHz
            if util >= self.thr:
                nf = min(150, max(f + 1, round(f * 1.06)))
            else:
                nf = max(5, min(f - 1, round(f / 1.06)))
        else:
            nf = f * util / self.target
        if self.fast and util >= 0.95:
            nf = top
        nf = min(top, max(nf, floor, 5))
        if self.bands:
            nf = BANDS[band_of(nf)]
        else:
            nf = int(math.ceil(nf - 1e-9))
        if nf < f:
            if self.fast:               # down one band (or 6 %) a window
                if self.bands:
                    nf = max(nf, BANDS[max(0, band_of(f) - 1)])
                else:
                    nf = max(nf, int(math.floor(f / 1.06)))
            if now - self.last_change < self.dwell:
                nf = f
        return nf


class VideoFloor:
    """the video session's client floor, the PP's (apps/video/vclk.c): it
    starts at the top; after a steady 3 s it drops to the lowest band
    whose scaled FEED p95 keeps 25 % of the frame and whose scaled longest
    FEED keeps 10 % (no lower than a band with a hold); a miss puts it a band above the clock that
    missed, and that band then waits twice as long (up to 8 x)"""
    def __init__(self, frame_us, top=150):
        self.floor, self.frame, self.top = top, frame_us, top
        self.start = 0.0
        self.works = []
        self.hold = {}

    def miss(self, f, now):
        k = band_of(f)
        self.hold[k] = min(self.hold.get(k, 0) + 1, 3)
        self.floor = BANDS[min(len(BANDS) - 1, band_of(max(self.floor, f)) + 1)]
        self.start, self.works = now, []

    def work(self, us_at_f, f):
        self.works.append(us_at_f * f)          # as cycles

    def tick(self, now):
        k = band_of(self.floor)
        if k == 0 or not self.works:
            return
        nxt = BANDS[k - 1]
        if now - self.start < 3e6 * (1 << self.hold.get(k - 1, 0)):
            return
        w = sorted(self.works)
        p95, mx = w[int(len(w) * 0.95)], w[-1]
        fits = lambda b: p95 / b <= self.frame * 0.75 and mx / b <= self.frame * 0.90
        if fits(nxt):
            # the lowest band that fits and has no hold pending, at once
            j = k - 1
            while j > 0 and fits(BANDS[j - 1]) and not self.hold.get(j - 1):
                j -= 1
            self.floor = BANDS[j]
        self.start, self.works = now, []


class TwoLayer:
    """apps/video/vclk.c's governor: read every 100 ms (the PP's reading of
    the chip's meter).  The load (MHz of work) goes up at once and down by
    an EWMA of 1/4 a reading.
      1. the band selector: up one band when the load reaches 95 % of the
         band's top; down one band when the band below's top carries it at
         80 % or less, after 100 ms at the band; a client's floor forces a
         minimum band; a burst the top;
      2. the in-band policy: 'top' the band's top clock, or 'prop' load /
         0.8 inside the band (above the band below's top), down only by
         10 % or more"""
    READ = 10                   # windows a reading

    def __init__(self, name, start, policy='top', floors=True):
        self.name, self.f, self.policy, self.floors = name, start, policy, floors
        self.fixed = None
        self.load = None
        self.busy = self.n = 0
        self.changed = -1e18
        self.last_change = -1e18

    def band(self):
        return band_of(self.f)

    def decide(self, now, util, floor, top=150):
        self.busy += util
        self.n += 1
        if self.n < self.READ and not (floor and BANDS[band_of(floor)] > BANDS[self.band()]):
            return self.f
        u = self.busy / self.n * self.f if self.n else 0
        self.busy = self.n = 0
        if self.load is None or u >= self.load:
            self.load = u
        else:
            self.load -= (self.load - u) / 4
        b = self.band()
        nb = b
        tb = len(BANDS) - 1
        if self.load >= BANDS[b] * 0.95 and b < tb:
            nb = b + 1
        elif b and now - self.changed >= 100000 and self.load <= BANDS[b - 1] * 0.80:
            nb = b - 1
        if floor:
            nb = max(nb, band_of(floor))
        if nb == 0:
            f = 27
        elif self.policy == 'top' or nb > b or self.load >= BANDS[nb] * 0.95:
            f = BANDS[nb]
        else:
            lo = BANDS[nb - 1] + 1
            f = min(BANDS[nb], max(lo, int(math.ceil(self.load / 0.8))))
            if nb == b and f < self.f and f > self.f * 0.9:
                f = self.f
        if nb != b:
            self.changed = now
        return f


def simulate(gov, jobs, idle_factor, frame_us=None, top=150):
    t = 0.0
    q = []              # [remaining cycles, deadline, kind, cycles]
    ji = 0
    end = max(j[1] for j in jobs) + 100000
    f = gov.f if not gov.fixed else gov.fixed
    level = level_for(f)
    stall = 0.0
    misses = 0
    clock_changes = level_changes = 0
    e_mj = 0.0
    t_f = t_l = 0.0
    vf = VideoFloor(frame_us, top) if gov.floors and frame_us else None
    while t < end:
        wend = t + WIN
        busy = 0.0
        s = min(stall, WIN)             # the switch's waits, busy-waiting
        stall -= s
        busy += s
        tt = t + s
        while tt < wend - 1e-6:
            while ji < len(jobs) and jobs[ji][0] <= tt:
                r, d, c, k = jobs[ji]
                q.append([c, d, k, c])
                ji += 1
            if not q:
                nxt = jobs[ji][0] if ji < len(jobs) else wend
                tt = min(wend, max(tt, nxt))
                continue
            job = q[0]
            run = min(job[0] / f, wend - tt)
            job[0] -= run * f
            tt += run
            busy += run
            if job[0] <= 1e-6:
                q.pop(0)
                if vf and job[2] == 'video':
                    vf.work(job[3] / f, f)
                if tt > job[1]:
                    misses += 1
                    if vf and job[2] == 'video':
                        vf.miss(f, tt)
        e_mj += power_mw(f, level, busy / WIN, idle_factor) * WIN / 1e6
        t_f += f * WIN
        t_l += level * WIN
        t = wend
        # the governor's busy: work only (the switch's waits are its own),
        # over the part of the window that was not a wait
        util = (busy - s) / (WIN - s) if WIN - s > 0 else 0.0
        floor = 0.0
        if vf:
            vf.tick(t)
            floor = vf.floor
        if gov.floors and any(j[2] == 'decode' for j in q):
            floor = top                 # a burst's full-speed request
        nf = gov.decide(t, util, floor, top)
        if nf != f:
            nl = level_for(nf)
            stall += PLL_US + (REG_US if nl != level else 0)
            if isinstance(gov, TwoLayer):
                gov.f = nf
            clock_changes += 1
            level_changes += nl != level
            level, f = nl, nf
            gov.f = nf
            gov.last_change = t
    secs = end / 1e6
    return dict(gov=gov.name, mean_mhz=t_f / end, mean_level=t_l / end,
                ma=to_ma(e_mj / secs), misses=misses,
                clock_changes_per_min=clock_changes * 60 / secs,
                level_changes_per_min=level_changes * 60 / secs)


B = dict(start=150)
GOVS = [
    ('fixed150', dict(B, fixed=150)),
    ('fixed27', dict(B, fixed=27)),
    ('apple', dict(B)),
    ('apple+floors', dict(B, floors=True)),
    ('1 prop', dict(B, prop=True)),
    ('1+2 fast', dict(B, prop=True, fast=True)),
    ('1+2+3 bands', dict(B, prop=True, fast=True, bands=True)),
    ('1-4 dwell', dict(B, prop=True, fast=True, bands=True, dwell_ms=100)),
    ('1-4+ewma', dict(B, prop=True, fast=True, bands=True, dwell_ms=100,
                      ewma=0.25)),
    ('1-5 floors', dict(B, prop=True, fast=True, bands=True, dwell_ms=100,
                        floors=True)),
    ('ours: 1-5+ewma', dict(B, prop=True, fast=True, bands=True, dwell_ms=100,
                            ewma=0.25, floors=True)),
    ('floors only', dict(B, floors=True, floors_only=True)),
    ('floors+net', dict(B, floors=True, floors_only=True, net=True)),
    ('floors|gov (v1)', dict(B, prop=True, fast=True, bands=True, dwell_ms=100,
                             ewma=0.25, floors=True, split=True)),
    ('2-layer: top', None),
    ('2-layer: prop', None),
]


def make(name, kw):
    if name == '2-layer: top':
        return TwoLayer(name, 150, 'top')
    if name == '2-layer: prop':
        return TwoLayer(name, 150, 'prop')
    return Gov(name, **kw)


def workloads():
    c1, fr1 = clip('clip1', 30)
    c2, fr2 = clip('clip2', 24)
    u, _ = ui()
    # load no client declares (a visualizer or a game drawing on the chip,
    # 25 frames a second of 1.2 M cycles for 10 s: 30 MHz of work) [ours]
    x = [(10e6 + k * 40000, 10e6 + (k + 1) * 40000, 1200000, 'other')
         for k in range(250)]
    return [('clip1', c1, fr1), ('clip2', c2, fr2), ('ui', u, None),
            ('undeclared', sorted(u + x), None)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--poll', action='store_true', help='idle polls (no sleep)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--trace', help='one workload only')
    ap.add_argument('--xtal5', action='store_true',
                    help='the crystal at level 5 (PW3), not 11')
    args = ap.parse_args()
    idle = 1.0 if args.poll else PM.PARAMS['sleep_factor'][0]
    if args.xtal5:
        XTAL_LEVEL[0] = 5
    out = {}
    for name, jobs, frame in workloads():
        if args.trace and name != args.trace:
            continue
        rows = []
        for gname, kw in GOVS:
            rows.append(simulate(make(gname, kw), jobs, idle, frame))
        out[name] = rows
    if args.json:
        json.dump(out, sys.stdout, indent=1)
        print()
        return 0
    print('idle: %s (factor %.2f); proxy [estimate], misses and changes [sim]' % (
        'polling' if args.poll else 'asleep', idle))
    for name, rows in out.items():
        print('\n%s' % name)
        print('  %-16s %7s %7s %8s %7s %9s %9s' % ('governor', 'MHz', 'level', 'mA',
                                                  'misses', 'clk/min', 'lvl/min'))
        for r in rows:
            print('  %-16s %7.1f %7.2f %8.3f %7d %9.1f %9.1f' % (
                r['gov'], r['mean_mhz'], r['mean_level'], r['ma'], r['misses'],
                r['clock_changes_per_min'], r['level_changes_per_min']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
