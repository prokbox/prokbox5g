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

"""The video chip's per-stream clock: the FEED cost model and each clip's
settle point (P19 PW4; docs/research/bcm2722/battery-levers.md section 4).

The data are the silicon FEED times (STATUS word 13, the trace's V6 lines)
of the bench clips at 27 and 150 MHz.  Run with --logs FILE... to derive
them again from bcmtest logs (each "stage 6 clip N:" block's V6 lines);
without, the table below (from the logs named beside each row) is used.

Prints:
  - the cycles a frame at each clock (cycles = FEED us x MHz): whether the
    decode scales with the core clock;
  - the model, mean cycles a frame = a x pixels + b x bits a frame, least
    squares over the 150 MHz means (the intercept per pixel only: every
    clip is 320 x 240), and its residuals;
  - per clip and point: the floor (the model's mean, less its fit error,
    at the frame rate) and the settle rule (p95 at 150 x 150 / f under 75 %
    of the budget, the I frames (p99) under 90 %): where race-then-settle
    would stop.

No dependencies.  --json for the numbers."""

import argparse
import json
import re
import sys

POINTS = (27, 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150)   # the bands
MARGIN = 0.25           # apps/video/vclk.h VCLK_MARGIN_PCT
SURE = 0.80             # VCLK_MODEL_SURE_PCT

# [silicon] per clip: pixels, fps, kbit/s; FEED us mean, p95, p99 (the
# worst steady frames: the I frames; the max is the first frame's one-time
# set-up), max at each clock, with the log they come from (V6 lines; p95 over the samples the
# trace kept, about one STATUS a picture)
CLIPS = {
    "clip1": dict(
        file="stage6-clip.mp4 (bcmtest/clip.mp4): 320x240 30 fps 169 kbit/s "
             "Constrained Baseline, 240 frames",
        w=320, h=240, fps=30.0, kbps=169,
        at={150: (7552, 8391, 12646, 31242, "session 27 bcmtest.log clip 1"),
            27: (42706, 56152, 79610, 164126,
                 "bench-20261025-1840/bcmtest.1.log clip 1 (69 skipped)")}),
    "clip2": dict(
        file="stage6-real.mp4 (bcmtest/clip2.mp4): 320x240 24 fps 414 kbit/s "
             "Constrained Baseline, 312 frames",
        w=320, h=240, fps=24.0, kbps=414,
        at={150: (18352, 27234, 28783, 39810, "session 27 bcmtest.log clip 3"),
            27: (95371, 158152, 228999, 228999,
                 "bench-20261025-1840/bcmtest.1.log clip 3 (203 skipped)")}),
    "stage7": dict(
        file="stage7.m4v: prokbox-convert's output, 320x240, 600 kbit/s "
             "(768 max), 3 s GOP; fps not logged (30 assumed)",
        w=320, h=240, fps=30.0, kbps=600,
        at={150: (14750, 22467, 31912, 35617, "session37c-1440/bcmtest.log stage 7")}),
}


def read_logs(paths):
    """V6 work times per (log, clip) block: {name: {mhz: [us...]}}"""
    out = {}
    for path in paths:
        clip = None
        for line in open(path, errors="replace"):
            m = re.search(r"stage 6 clip (\d):", line)
            if m:
                clip = "%s#%s" % (path, m.group(1))
            m = re.search(r"V6 (\d+) (-?\d+) (\d+) (\d+) (\d+)", line)
            if m and int(m.group(3)) > 0:
                key = clip or path
                out.setdefault(key, {}).setdefault(int(m.group(4)), []) \
                   .append(int(m.group(3)))
    return out


def stats(xs):
    xs = sorted(xs)
    n = len(xs)
    return sum(xs) // n, xs[int(n * 0.95)], xs[-1]


def fit(clips):
    """least squares, y = a x px + b x bits; px is the same for every clip,
    so a is the intercept over the pixels"""
    rows = [(c["w"] * c["h"], c["kbps"] * 1000.0 / c["fps"],
             c["at"][150][0] * 150.0) for c in clips.values()]
    n = len(rows)
    mx = sum(r[1] for r in rows) / n
    my = sum(r[2] for r in rows) / n
    sxx = sum((r[1] - mx) ** 2 for r in rows)
    sxy = sum((r[1] - mx) * (r[2] - my) for r in rows)
    b = sxy / sxx
    a = (my - b * mx) / rows[0][0]
    return a, b


SPIKE = 0.90            # VCLK_SPIKE_PCT


def settle(c, a, b, margin=MARGIN):
    budget = 1e6 / c["fps"]
    p95 = c["at"][150][1]
    p99 = c["at"][150][2]
    model = a * c["w"] * c["h"] + b * c["kbps"] * 1000.0 / c["fps"]
    floor = model * SURE * c["fps"] / 1e6
    rows = []
    for f in POINTS:
        scaled = p95 * 150.0 / f
        ok = (scaled <= budget * (1 - margin) and f >= floor and
              p99 * 150.0 / f <= budget * SPIKE)
        rows.append((f, round(scaled), ok))
    land = 150
    for f in sorted(POINTS, reverse=True):
        if f == 150:
            continue
        r = [x for x in rows if x[0] == f][0]
        if not r[2]:
            break
        land = f
    return dict(budget_us=round(budget), model_mcyc=round(model / 1e6, 3),
                floor_mhz=round(floor, 1), points=rows, settles_at=land)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--logs", nargs="*", help="bcmtest logs to read V6 from")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    if args.logs:
        for k, by in read_logs(args.logs).items():
            for mhz, xs in sorted(by.items()):
                m, p, x = stats(xs)
                print("%s: %d MHz n %d mean %d p95 %d max %d us; "
                      "Mcyc mean %.2f p95 %.2f max %.2f" %
                      (k, mhz, len(xs), m, p, x, m * mhz / 1e6,
                       p * mhz / 1e6, x * mhz / 1e6))
        return 0
    a, b = fit(CLIPS)
    out = dict(a_cyc_per_px=a, b_cyc_per_bit=b, clips={})
    for k, c in CLIPS.items():
        r = dict(file=c["file"], scaling={})
        for mhz, (m, p, p99, x, src) in sorted(c["at"].items()):
            r["scaling"][mhz] = dict(mean_mcyc=round(m * mhz / 1e6, 3),
                                     p95_mcyc=round(p * mhz / 1e6, 3),
                                     max_mcyc=round(x * mhz / 1e6, 3),
                                     source=src)
        model = a * c["w"] * c["h"] + b * c["kbps"] * 1000.0 / c["fps"]
        r["model_resid_pct"] = round(
            100.0 * (model - c["at"][150][0] * 150.0) / (c["at"][150][0] * 150.0), 1)
        r.update(settle(c, a, b))
        out["clips"][k] = r
    if args.json:
        json.dump(out, sys.stdout, indent=1)
        print()
        return 0
    print("model: mean cycles a frame = %.2f x pixels + %.1f x bits a frame "
          "[silicon fit, 3 clips]" % (a, b))
    for k, r in out["clips"].items():
        print("\n%s: %s" % (k, r["file"]))
        for mhz, s in sorted(r["scaling"].items()):
            print("  %3d MHz: %.2f M cycles mean, %.2f p95, %.2f max  (%s)" %
                  (mhz, s["mean_mcyc"], s["p95_mcyc"], s["max_mcyc"],
                   s["source"]))
        print("  model %.2f M (residual %+.1f %%); floor %.1f MHz; "
              "budget %d us" % (r["model_mcyc"], r["model_resid_pct"],
                                r["floor_mhz"], r["budget_us"]))
        print("  p95 scaled: " + ", ".join(
            "%d MHz %d us%s" % (f, s, "" if ok else " x")
            for f, s, ok in r["points"]))
        print("  settles at %d MHz" % r["settles_at"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
