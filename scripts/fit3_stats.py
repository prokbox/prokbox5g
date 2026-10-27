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

# Fit-3 step 0 (docs/fit3-brief.md section 3): turns the developer build's
# once-a-minute counter lines in ipodui.log into a table of per-minute
# rates, one row per logged window, then the whole run's rates.
#
#   scripts/fit3_stats.py ipodui.log [ipodui.log.1 ...]
#
# A window is an "f3 lcd s=S ..." line and the "f3 port/art/pcm" lines
# that follow it (each only when its counts moved; apps/ipodui/devmode.c,
# ipodui_devmode_f3_service()). S is the window in seconds: a window runs
# longer than a minute while the disk sleeps, so every rate is the count
# times 60 / S. Also prints the boot's "storage id" lines and the first
# wake ("storage wake1"). The log's lines start with a sequence number,
# which is ignored; a "boot" line (each file's first) starts a new run.
import re
import sys

KV = re.compile(r'(\w+)=(-?\w+)')
# (line kind, key) -> column; per-minute rate columns, then the max
COLS = [
    ('lcd', 'n', 'lcd/min'), ('lcd', 'kb', 'lcdKB/min'),
    ('lcd', 'ms', 'lcd ms/min'),
    ('lcde2', 'kb', 'e2KB/min'), ('lcde2', 'ms', 'e2 ms/min'),
    ('lcde2', 'fail', 'e2 fail/min'),
    ('port', 'ovl', 'ovl/min'), ('port', 'kb', 'ovlKB/min'),
    ('port', 'tick', 'rdtick/min'), ('port', 'poll', 'rdpoll/min'),
    ('art', 'n', 'art/min'), ('art', 'kb', 'artKB/min'),
    ('art', 'r', 'artrd/min'), ('art', 'rms', 'art rd ms/min'),
    ('art', 'dms', 'art dec ms/min'), ('art', 'bo', 'boost/min'),
    ('pcm', 'wb', 'wb/min'), ('pcm', 'us', 'wb us/min'),
]


def parse(lines):
    """Yields ('window', dict) and ('note', text) in log order."""
    win = None
    for raw in lines:
        parts = raw.split()
        if parts and parts[0].isdigit():
            parts = parts[1:]           # the ring's sequence number
        if not parts:
            continue
        if parts[0] == 'boot' and len(parts) > 1 and \
                parts[1].startswith('build='):
            if win:
                yield 'window', win
                win = None
            yield 'note', 'boot ' + parts[1]
            continue
        if parts[0] == 'storage':
            yield 'note', ' '.join(parts)
            continue
        if parts[0] != 'f3' or len(parts) < 2:
            continue
        kind = parts[1]
        vals = {k: int(v) for k, v in KV.findall(' '.join(parts[2:]))
                if re.fullmatch(r'-?\d+', v)}
        if kind == 'lcd':
            if win:
                yield 'window', win
            win = {'s': vals.get('s', 0), 'max': vals.get('max', 0)}
        if win is None:
            continue                    # a window's tail without its head
        for k, v in vals.items():
            win[(kind, k)] = v
    if win:
        yield 'window', win


def rate(v, s):
    return v * 60.0 / s if s > 0 else 0.0


def main(paths):
    lines = []
    for p in paths:
        with open(p, errors='replace') as f:
            lines.extend(f.read().splitlines())
    rows, total = [], {'s': 0, 'max': 0}
    heads = ['s'] + [c[2] for c in COLS] + ['lcd max us']
    for what, x in parse(lines):
        if what == 'note':
            print('#', x)
            continue
        s = x['s']
        total['s'] += s
        total['max'] = max(total['max'], x['max'])
        row = [str(s)]
        for kind, key, _ in COLS:
            v = x.get((kind, key), 0)
            total[(kind, key)] = total.get((kind, key), 0) + v
            row.append('%.0f' % rate(v, s))
        row.append(str(x['max']))
        rows.append(row)
    if not rows:
        print('no f3 lines (a developer build, the disk awake once a '
              'minute)')
        return 1
    trow = [str(total['s'])] + ['%.0f' % rate(total.get((k, c), 0),
                                              total['s'])
                                for k, c, _ in COLS] + [str(total['max'])]
    w = [max(len(h), *(len(r[i]) for r in rows + [trow]))
         for i, h in enumerate(heads)]
    fmt = '  '.join('%%%ds' % n for n in w)
    print(fmt % tuple(heads))
    for r in rows:
        print(fmt % tuple(r))
    print(fmt % tuple(trow) + '  (all windows)')
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__ or 'usage: fit3_stats.py ipodui.log ...',
              file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
