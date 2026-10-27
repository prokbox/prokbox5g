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

# USB session reader (#81): turns the iPod's ipodui.log "usb ..." lines into
# one row per USB session, with a verdict, so a device test round reads in
# seconds.
#
#   scripts/usb_sessions.py ipodui.log [ipodui.log.1 ...]   (oldest last is fine)
#   scripts/usb_sessions.py --mac [minutes]   the Mac's own view: every
#        diskarbitration mount attempt of an IPOD volume in the last N
#        minutes (default 60), and the helper's log
#
# A session starts at "usb enter n=N" and its counters come at USB exit
# (apps/ipodui/usbscreen.c): exit, cmd, ms, ata, pf, mnt, bot, rst, rlog,
# blk, hand (the storage handover: when it was asked for and granted, in ms
# from the USB screen's entry, and which threads acknowledged it when). Verdicts:
#   DEAF   entered USB mode, but the host sent no command at all (the
#          connected screen with no arrows: the enumeration bug)
#   RESET  the host reset the device during the session (bulk or bus
#          resets); survived if the session still moved data afterwards
#   STUCK  the reset flag or exclusive storage was still set at exit
#          (pend=1 / excl=1 / the next session's pendexit=1)
#   SLOW   a single USB-thread block over 1 s (blk busy)
#   OK     none of the above
# Lines start with a sequence number, which is ignored.
import re
import subprocess
import sys

KV = re.compile(r'(\w+)=([^\s]+)')


def num(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def parse(paths):
    sessions, cur = [], None
    for path in paths:
        try:
            lines = open(path, errors='replace').read().splitlines()
        except OSError as e:
            print('skip %s: %s' % (path, e), file=sys.stderr)
            continue
        for line in lines:
            parts = line.split(None, 2)
            if len(parts) < 2:
                continue
            # "<seq> usb <kind> k=v ..." (the tag is "usb")
            if parts[0].isdigit():
                parts = parts[1:]
            if not parts or parts[0] != 'usb':
                continue
            rest = line.split('usb', 1)[1].strip()
            kind = rest.split()[0] if rest else ''
            kv = dict(KV.findall(rest))
            if kind == 'enter':
                cur = {'file': path, 'n': kv.get('n')}
                sessions.append(cur)
                continue
            if cur is None or (kind == 'exit' and 'exit.s' in cur):
                # no "enter" line (older builds): a second exit block
                # starts the next session
                cur = {'file': path, 'n': '?'}
                sessions.append(cur)
            # The mount-deadline snapshot is deliberately rendered as
            # "mnt t18003 ..." so it stays compact on the device. Preserve
            # its timestamp and keep its keys separate from normal mnt data.
            if kind == 'mnt':
                at = re.match(r'mnt\s+t(\d+)\b', rest)
                if at:
                    cur['mnt.t18'] = at.group(1)
                    kind = 'mnt18'
            if kind == 'rlog':
                cur['rlog'] = rest.split(None, 1)[1] if ' ' in rest else '-'
            elif kind == 'rbx':
                cur.setdefault('rbx', []).append(rest.split(None, 1)[1]
                                                 if ' ' in rest else '')
            elif kind == 'rbits':
                cur.setdefault('rbits', []).append(rest.split(None, 1)[1]
                                                    if ' ' in rest else '')
            elif kind == 'irq':
                for k, v in kv.items():
                    cur['irq.%s' % k] = v
            elif kind == 'hand' and rest.split()[1:2] == ['by']:
                cur['hand_by'] = rest.split(None, 2)[2] if rest.count(' ') > 1 else '-'
            elif kind in ('prev',):
                cur['prev_pendexit'] = kv.get('pendexit')
            else:
                for k, v in kv.items():
                    cur['%s.%s' % (kind, k)] = v
    return sessions


def verdict(s):
    v = []
    cmds = sum(num(s.get(k)) or 0 for k in
               ('cmd.rd', 'cmd.wr', 'bot.tur', 'bot.inq', 'bot.oth'))
    if 'exit.s' in s and cmds == 0:
        v.append('DEAF')
    resets = (num(s.get('rst.bulk')) or 0) + (num(s.get('rst.bus')) or 0)
    if resets:
        v.append('RESET(%d)' % resets)
    if s.get('rst.pend') == '1' or s.get('rst.excl') == '1':
        v.append('STUCK')
    busy = num(s.get('blk.busy'))
    if busy is not None and busy > 1000:
        v.append('SLOW(%dms)' % busy)
    if 'exit.s' not in s:
        v.append('NO-EXIT-LINES')
    return ' '.join(v) or 'OK'


def report(sessions):
    hdr = ('#', 'secs', 'rdMB', 'wrMB', 'TUR', 'INQ', 'inqdone', 'READs',
           'pf', 'test', 'ring', 'hit%', 'stall', 'cfg2rd', '18sKB', 'rd30KB', 'bulk', 'bus',
           'defer', 'bad', 'wrfail', 'busy', 'verdict')
    rows = []
    for i, s in enumerate(sessions):
        hit = num(s.get('pf.hit')) or 0
        pend = num(s.get('pf.pend')) or 0
        miss = num(s.get('pf.miss')) or 0
        tot = hit + pend + miss
        stall = s.get('pf.stall', '-')
        rows.append((
            str(i + 1), s.get('exit.s', '-'), s.get('exit.rd', '-'),
            s.get('exit.wr', '-'), s.get('mnt.cfg2tur', '-'),
            s.get('mnt.cfg2inq', '-'), s.get('mnt.inq2data', '-'),
            s.get('cmd.rd', '-'),
            s.get('sw.pf', '-'), s.get('sw.test', '-'),
            s.get('pf.ring', '-'),
            ('%d' % (100 * (hit + pend) // tot)) if tot else '-',
            stall.split('/')[0] if stall != '-' else '-',
            s.get('mnt.cfg2rd', '-'), s.get('mnt18.rd', '-'), s.get('mnt.rd30', '-'),
            s.get('rst.bulk', '-'), s.get('rst.bus', '-'),
            s.get('rst.defer', '-'), s.get('bot.bad', '-'),
            s.get('bot.wrfail', '-'), s.get('blk.busy', '-'), verdict(s)))
    w = [max(len(h), *(len(r[c]) for r in rows)) if rows else len(h)
         for c, h in enumerate(hdr)]
    print('  '.join(h.rjust(w[c]) for c, h in enumerate(hdr)))
    for r in rows:
        print('  '.join(x.rjust(w[c]) for c, x in enumerate(r)))
    for i, s in enumerate(sessions):
        extra = []
        if s.get('prev_pendexit') == '1':
            extra.append('previous session exited with the reset flag set')
        if s.get('rlog') and s.get('rlog') != '-':
            extra.append('reset log ' + s['rlog'])
        irq = ['%s=%s' % (k[4:], s[k]) for k in
               ('irq.uri', 'irq.pci', 'irq.uei', 'irq.rstreal', 'irq.rstfake',
                'irq.addr', 'irq.td', 'irq.mode') if k in s]
        if irq:
            extra.append('controller ' + ' '.join(irq))
        if 'stor.small' in s:
            extra.append('storage reads <=16K n/mean_us %s, larger n/mean_us/KB '
                         '%s, cpu MHz %s' % (s.get('stor.small'),
                                            s.get('stor.big'), s.get('stor.mhz')))
        if 'hand.ok' in s:
            extra.append('handover req=%s ok=%s ms (acks %s, CBWs held %s, '
                         'longest %s ms) by %s' % (
                             s.get('hand.req'), s.get('hand.ok'),
                             s.get('hand.acks'), s.get('hand.held'),
                             s.get('hand.holdmax'), s.get('hand_by', '-')))
        rbx = s.get('rbx', [])
        for i, b in enumerate(s.get('rbits', [])[:8]):
            extra.append('raw ' + b + (' | ' + rbx[i].split(None, 1)[-1]
                                       if i < len(rbx) else ''))
        if extra:
            print('  #%d: %s' % (i + 1, '; '.join(extra)))
    bad = [verdict(s) for s in sessions if verdict(s) != 'OK']
    print('\n%d sessions, %d not OK' % (len(sessions), len(bad)))


def mac(minutes):
    pred = ('process == "diskarbitrationd" AND (eventMessage CONTAINS '
            '"mounted disk" OR eventMessage CONTAINS "unable to mount" OR '
            'eventMessage CONTAINS "created disk, id = /dev/disk" OR '
            'eventMessage CONTAINS "removed disk")')
    out = subprocess.run(['/usr/bin/log', 'show', '--last', '%dm' % minutes,
                          '--style', 'compact', '--predicate', pred],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if re.search(r'disk\d+s2|created disk, id = /dev/disk\d+\.|removed disk',
                     line):
            m = re.match(r'(\S+ \S+)', line)
            msg = line.split(']', 1)[-1].strip()
            print('%s  %s' % (m.group(1)[11:19] if m else '?', msg))
    try:
        tail = open('/var/log/ipod-automount.log').read().splitlines()[-6:]
        print('\nhelper:')
        for t in tail:
            print('  ' + t)
    except OSError:
        pass


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--mac':
        mac(int(sys.argv[2]) if len(sys.argv) > 2 else 60)
    elif len(sys.argv) > 1:
        report(parse(sys.argv[1:]))
    else:
        print(__doc__ if __doc__ else 'usage: usb_sessions.py ipodui.log ...')
        sys.exit(2)
