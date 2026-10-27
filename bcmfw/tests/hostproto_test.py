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
"""bcmfw's video host protocol skeleton (BCMFW_HOSTPROTO=1) in the
simulator, driven by a model of the PP-side driver.

The PP side (class Vp) follows docs/research/bcm2722/hostproto.md: it
reads the header at 0x100, writes command records into the command ring
and publishes its write index, reads replies and events from the event
ring, and never waits without a deadline.  It also counts host-port
transfers (16-bit bus cycles, address setups included), the PP's cost.
The LCD side is hello_test.py's lcd-video.c model, unchanged: both
contracts run on the same image at once.

Usage:
  bcmfw/tests/hostproto_test.py [-v] [--seed N] [--vmcs PATH] [--only NAME]

Skips cleanly (exit 0, message) when vc4-elf-gcc is not installed.
"""
import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hello_test as T   # noqa: E402

B = T.B
DEFS = ('BCMFW_HOSTPROTO=1',)


def hp_constants():
    """The protocol numbers, read from bcmfw/hostproto.h (one source)."""
    text = open(os.path.join(T.FW, 'hostproto.h')).read()
    out = {}
    for name, val in re.findall(r'^#define\s+(HP_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b', text, re.M):
        out[name] = int(val, 0)
    out['HP_VERSION_WORD'] = out['HP_MAGIC'] << 16 | out['HP_MAJOR'] << 8 | out['HP_MINOR']
    return out


K = hp_constants()


def enc(v):
    return ((~v & 0xffff) << 16) | (v & 0xffff)


def valid(w):
    return ((w >> 16) ^ (w & 0xffff)) == 0xffff


class Timeout(Exception):
    pass


class Vp:
    """The PP-side driver model."""

    def __init__(self, bench):
        self.b = bench
        self.xfers = 0          # 16-bit host-port cycles
        self.tag = 0
        self.events = []        # unsolicited events seen
        self.replies = {}       # tag -> (op, status, payload)

    # host port, as lcd-video.c does it: an address setup, then data
    def rd(self, a):
        self.xfers += 4
        return self.b.r32(a)

    def wr(self, a, v):
        self.xfers += 4
        self.b.w32(a, v)

    def wr_block(self, a, words):
        self.xfers += 2 + 2 * len(words)
        for i, v in enumerate(words):
            self.b.w32(a + 4 * i, v)

    def index(self, a):
        w = self.rd(a)
        return (w & 0xffff) if valid(w) else None

    def attach(self):
        self.version = self.rd(K['HP_VERSION'])
        self.caps = self.rd(K['HP_CAPS'])
        self.cmd_base, self.cmd_size = self.rd(K['HP_CMD_BASE']), self.rd(K['HP_CMD_SIZE'])
        self.evt_base, self.evt_size = self.rd(K['HP_EVT_BASE']), self.rd(K['HP_EVT_SIZE'])
        self.fifo_base, self.fifo_size = self.rd(K['HP_FIFO_BASE']), self.rd(K['HP_FIFO_SIZE'])
        self.cmd_wr = self.index(K['HP_CMD_WR'])
        self.evt_rd = self.index(K['HP_EVT_RD'])
        self.fifo_wr = 0
        return self.version

    def send(self, op, payload=(), tag=None):
        """Queue one command; returns its tag, or None when the ring has
        no room (the caller retries later: no waiting here)."""
        if tag is None:
            self.tag = (self.tag + 1) & 0xffff or 1
            tag = self.tag
        rd = self.index(K['HP_CMD_RD'])
        if rd is None:
            return None
        n = len(payload)
        if self.cmd_size - ((self.cmd_wr - rd) & 0xffff) < n + 1:
            return None
        words = [op | n << 8 | tag << 16] + list(payload)
        # contiguous pieces (the ring may wrap)
        k = 0
        while k < len(words):
            pos = (self.cmd_wr + k) % self.cmd_size
            m = min(len(words) - k, self.cmd_size - pos)
            self.wr_block(self.cmd_base + 4 * pos, words[k:k + m])
            k += m
        self.cmd_wr = (self.cmd_wr + n + 1) & 0xffff
        self.wr(K['HP_CMD_WR'], enc(self.cmd_wr))
        return tag

    def drain(self):
        wr = self.index(K['HP_EVT_WR'])
        if wr is None:
            return 0
        got = 0
        while self.evt_rd != wr:
            hdr = self.rd(self.evt_base + 4 * (self.evt_rd % self.evt_size))
            n = hdr >> 8 & 0xff
            body = [self.b.r32(self.evt_base + 4 * ((self.evt_rd + 1 + i) % self.evt_size))
                    for i in range(n)]
            self.xfers += 2 * n
            op, tag = hdr & 0xff, hdr >> 16
            if op & K['HP_REPLY']:
                self.replies[tag] = (op & 0x7f, body[0], body[1:])
            else:
                self.events.append((op, body[0], body[1:]))
            self.evt_rd = (self.evt_rd + 1 + n) & 0xffff
            got += 1
        self.wr(K['HP_EVT_RD'], enc(self.evt_rd))
        return got

    def wait(self, tag, timeout_us=50_000):
        t0 = self.b.now()
        while tag not in self.replies:
            if self.b.now() - t0 > timeout_us:
                raise Timeout('no reply to tag %d in %d us' % (tag, timeout_us))
            self.b.run(500)
            self.drain()
        return self.replies.pop(tag)

    def call(self, op, payload=(), timeout_us=50_000):
        tag = self.send(op, payload)
        if tag is None:
            raise Timeout('command ring full')
        return self.wait(tag, timeout_us)


class Suite(T.Suite):
    def setup(self, pid=1, awake=True):
        b, h, prog = self.bench(pid, DEFS)
        h.fb = T.random_frame(self.rng)
        if awake:
            h.lcd_awake()
            h.settle()
        else:
            h.bcm_init()
        h.tick_task = False
        h.idle_ok = False
        vp = Vp(b)
        vp.attach()
        return b, h, vp

    def lcd_frame(self, row, b, h, tag):
        h.tick_task = True
        h.fb = T.random_frame(self.rng)
        h.lcd_update()
        h.settle()
        h.tick_task = False
        self.frame_check(row, b, h, tag)

    def open_null(self, vp, flags=0):
        return vp.call(K['HP_OP_OPEN'], [K['HP_CODEC_NULL'], 320 | 240 << 16,
                                         K['HP_MODE_FIT'] | flags, 0, 320 | 240 << 16])

    # ---- rows ---------------------------------------------------------

    def t_header(self):
        row = T.Row('VP header after boot; LCD contract unchanged on the same image')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(1)
            row.check(vp.version == K["HP_VERSION_WORD"] == 0x56500104,
                      'version %08x' % vp.version)
            row.check(vp.caps == K['HP_CAP_NULL'] | K['HP_CAP_PARK'], 'caps %x' % vp.caps)
            row.check((vp.cmd_base, vp.cmd_size, vp.evt_base, vp.evt_size) ==
                      (K['HP_CMD_RING'], K['HP_CMD_WORDS'], K['HP_EVT_RING'], K['HP_EVT_WORDS']),
                      'ring layout')
            row.check((vp.fifo_base, vp.fifo_size) == (K['HP_SRAM_FIFO'], K['HP_SRAM_FIFO_SIZE']),
                      'fifo %x/%x' % (vp.fifo_base, vp.fifo_size))
            for a in ('HP_CMD_WR', 'HP_CMD_RD', 'HP_EVT_WR', 'HP_EVT_RD', 'HP_FIFO_RD'):
                row.check(b.r32(K[a]) == enc(0), '%s = %08x' % (a, b.r32(K[a])))
            st = b.r32(K['HP_STATE'])
            row.check(valid(st) and st & 0xff == K['HP_ST_IDLE'], 'state %08x' % st)
            ends = [K['HP_CMD_RING'] + 4 * K['HP_CMD_WORDS'], K['HP_EVT_RING'] + 4 * K['HP_EVT_WORDS'],
                    K['HP_SRAM_FIFO'] + K['HP_SRAM_FIFO_SIZE']]
            row.check(ends[0] <= K['HP_EVT_RING'] and ends[1] <= K['HP_SRAM_FIFO'] and
                      ends[2] <= T.FB and K['HP_CMD_RING'] >= 0xD0000,
                      'rings overlap each other, the stack or the framebuffer')
            prog, rep, img = self.image(DEFS)
            row.check(rep['end'] <= 0xCC000, 'image runs into the stack')
            row.check(K['HP_END'] <= T.MB_FAULT_PC, 'header runs into the LCD mailbox')
            self.frame_check(row, b, h, 'vp_boot')
            self.lcd_frame(row, b, h, 'vp_lcd')
            # the CPU-push image without the protocol leaves the header zero
            b2, h2, _ = self.bench(1, ())
            h2.bcm_init()
            row.check(b2.r32(K['HP_VERSION']) == 0, 'default image writes a VP header')
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_hello(self):
        row = T.Row('HELLO: version and caps; host major mismatch; PING 0-32 words')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(0, awake=False)
            op, st, p = vp.call(K['HP_OP_HELLO'], [K['HP_MAJOR'] << 8 | K['HP_MINOR']])
            row.check((op, st) == (K['HP_OP_HELLO'], K['HP_OK']) and
                      p == [K['HP_VERSION_WORD'], K['HP_CAP_NULL'] | K['HP_CAP_PARK'], K['HP_MAX_PAYLOAD']],
                      'hello %s %s %s' % (op, st, p))
            op, st, p = vp.call(K['HP_OP_HELLO'], [(K['HP_MAJOR'] + 1) << 8])
            row.check(st == K['HP_E_VERSION'] and p[0] == K['HP_VERSION_WORD'],
                      'major mismatch: status %d' % st)
            for n in (0, 1, 7, 31, 32):
                data = [self.rng.getrandbits(32) for _ in range(n)]
                op, st, p = vp.call(K['HP_OP_PING'], data)
                row.check(st == K['HP_OK'] and p == data, 'ping %d words' % n)
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_pins(self):
        row = T.Row('PINS: GPIO 1-3 sampled as inputs, edges counted, fsel restored')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(0, awake=False)
            fsel0 = b.r32(0x10002800)
            op, st, p = vp.call(K['HP_OP_PINS'], [0xE, 2000, 1], timeout_us=200_000)
            row.check(st == K['HP_OK'] and len(p) == 8, 'PINS status %d, %d words' % (st, len(p)))
            if st == K['HP_OK']:
                row.check(p[0] > 0 and p[1] >= 2000, 'samples %d, elapsed %d us' % (p[0], p[1]))
                row.check(p[2:5] == [0, 0, 0], 'no edges on idle pins: %s' % p[2:5])
            row.check(b.r32(0x10002800) == fsel0, 'fsel restored')
            for pull in (1, 2):
                op, st, p = vp.call(K['HP_OP_PINS'], [0x2, 1000, 1 | pull << 2],
                                    timeout_us=200_000)
                row.check(st == K['HP_OK'], 'PINS with pull %d: status %d' % (pull, st))
                row.check(b.r32(0x10002860) == 0 and b.r32(0x10002864) == 0,
                          'pull %d: mode and clock back to 0' % pull)
            op, st, p = vp.call(K['HP_OP_PINS'], [0, 1000, 0])
            row.check(st == K['HP_E_INVAL'], 'an empty mask refused: %d' % st)
            op, st, p = vp.call(K['HP_OP_PINS'], [2, 2_000_000, 0])
            row.check(st == K['HP_E_INVAL'], 'over 1 s refused: %d' % st)
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_open_close(self):
        row = T.Row('OPEN/CLOSE: null codec, busy, unsupported, invalid, no session')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(2, awake=False)
            E = K
            cases = [
                ([E['HP_CODEC_H264'], 320 | 240 << 16, 0], E['HP_E_UNSUPPORTED']),
                ([E['HP_CODEC_MPEG4'], 640 | 480 << 16, 0], E['HP_E_UNSUPPORTED']),
                ([E['HP_CODEC_NULL'], 642 | 480 << 16, 0], E['HP_E_INVAL']),
                ([E['HP_CODEC_NULL'], 320 | 0 << 16, 0], E['HP_E_INVAL']),
                ([E['HP_CODEC_NULL'], 321 | 240 << 16, 0], E['HP_E_INVAL']),
                ([E['HP_CODEC_NULL'], 320 | 240 << 16, 3], E['HP_E_INVAL']),
                ([E['HP_CODEC_NULL'], 320 | 240 << 16], E['HP_E_INVAL']),
            ]
            for args, want in cases:
                _, st, _ = vp.call(E['HP_OP_OPEN'], args)
                row.check(st == want, 'open %s: status %d, want %d' % (args, st, want))
            _, st, p = vp.call(E['HP_OP_CLOSE'], [1])
            row.check(st == E['HP_E_NOSESSION'], 'close with no session: %d' % st)
            _, st, p = self.open_null(vp)
            row.check(st == E['HP_OK'] and p == [1, E['HP_SRAM_FIFO'], E['HP_SRAM_FIFO_SIZE']],
                      'open null: %d %s' % (st, p))
            row.check(b.r32(E['HP_STATE']) & 0xff == E['HP_ST_OPEN'], 'state not OPEN')
            _, st, _ = self.open_null(vp)
            row.check(st == E['HP_E_BUSY'], 'second open: %d' % st)
            _, st, p = vp.call(E['HP_OP_STATUS'], [1])
            row.check(st == 0 and p[:2] == [E['HP_ST_OPEN'], 1], 'status %s' % p)
            _, st, _ = vp.call(E['HP_OP_CLOSE'], [2])
            row.check(st == E['HP_E_NOSESSION'], 'close of another session: %d' % st)
            _, st, p = vp.call(E['HP_OP_CLOSE'], [1])
            row.check(st == 0 and p == [0, 0], 'close: %d %s' % (st, p))
            row.check(b.r32(E['HP_STATE']) & 0xff == E['HP_ST_IDLE'], 'state not IDLE')
            _, st, _ = vp.call(E['HP_OP_CLOSE'], [1])
            row.check(st == E['HP_E_NOSESSION'], 'second close: %d' % st)
            _, st, _ = self.open_null(vp)
            row.check(st == 0, 'reopen: %d' % st)
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_feed(self):
        row = T.Row('FEED (null codec): 300 packets wrap the FIFO, order checked')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3, awake=False)
            self.open_null(vp)
            size = vp.fifo_size
            off, units, nbytes = 0, 0, 0
            x0 = vp.xfers
            for k in range(300):
                n = self.rng.choice([1, 63, 64, 65, 700, 3200, 9000, self.rng.randrange(1, 20000)])
                # the bitstream itself goes into the FIFO in pieces (it may wrap)
                vp.xfers += 2 + (n + 1) // 2 + (2 if off + n > size else 0)
                tag = vp.send(K['HP_OP_FEED'], [1, off, n, k * 33367, 0,
                                                K['HP_FEED_KEY'] if k % 30 == 0 else 0])
                if tag is None:
                    row.check(False, 'ring full at packet %d' % k)
                off = ((off + n + 63) & ~63) % size
                units = (units + (n + 63) // 64) & 0xffff
                nbytes += n
                if k % 10 == 9:
                    b.run(3000)
                    vp.drain()
            t0 = b.now()
            while vp.index(K['HP_FIFO_RD']) != units and b.now() - t0 < 50_000:
                b.run(500)
            row.check(vp.index(K['HP_FIFO_RD']) == units, 'FIFO_RD %s, want %d' % (
                vp.index(K['HP_FIFO_RD']), units))
            vp.drain()
            row.check(not vp.replies, 'replies to FEED without ACK: %d' % len(vp.replies))
            self.metrics['vp_feed_xfers_avg'] = (vp.xfers - x0) / 300.0
            self.metrics['vp_feed_bytes_avg'] = nbytes / 300.0
            _, st, p = vp.call(K['HP_OP_STATUS'], [1])
            row.check(p[0] == K['HP_ST_PLAYING'] and p[2] == 300 and p[3] == nbytes and p[4] == 0,
                      'status %s' % p)
            # out of order: wrong offset -> E_FIFO with the expected offset
            _, st, p = vp.call(K['HP_OP_FEED'], [1, (off + 64) % size, 10, 0, 0, 0])
            row.check(st == K['HP_E_FIFO'] and p == [off], 'bad offset: %d %s' % (st, p))
            _, st, p = vp.call(K['HP_OP_FEED'], [1, off, 0, 0, 0, 0])
            row.check(st == K['HP_E_FIFO'], 'empty packet: %d' % st)
            _, st, _ = vp.call(K['HP_OP_FEED'], [1, off, 10, 0, 0])
            row.check(st == K['HP_E_INVAL'], 'short feed record: %d' % st)
            _, st, _ = vp.call(K['HP_OP_FEED'], [7, off, 10, 0, 0, 0])
            row.check(st == K['HP_E_NOSESSION'], 'feed to a wrong session: %d' % st)
            _, st, _ = vp.call(K['HP_OP_FEED'], [1, off, 10, 0, 0, K['HP_FEED_ACK']])
            row.check(st == K['HP_OK'], 'ack feed: %d' % st)
            _, st, p = vp.call(K['HP_OP_STATUS'], [1])
            row.check(p[2] == 301 and p[4] == 4, 'status after errors %s' % p)
            _, st, p = vp.call(K['HP_OP_CLOSE'], [1])
            row.check(p == [301, nbytes + 10], 'close stats %s' % p)
            _, st, _ = vp.call(K['HP_OP_FEED'], [1, 0, 10, 0, 0, 0])
            row.check(st == K['HP_E_NOSESSION'], 'feed after close: %d' % st)
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_unknown_and_wrap(self):
        row = T.Row('unimplemented opcodes, oversize record, 3000 commands wrap the rings')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(0, awake=False)
            for op in (K['HP_OP_CLOCK'], K['HP_OP_PAUSE'], K['HP_OP_SEEK'], K['HP_OP_STATS'],
                       K['HP_OP_SLEEP'], 0x00, 0x55, 0x7F):
                _, st, _ = vp.call(op, [1, 2, 3])
                row.check(st == K['HP_E_UNKNOWN'], 'opcode %02x: status %d' % (op, st))
            _, st, _ = vp.call(K['HP_OP_PING'], list(range(K['HP_MAX_PAYLOAD'] + 1)))
            row.check(st == K['HP_E_TOOBIG'], 'oversize: %d' % st)
            ok, sent = 0, 0
            pending = {}
            for k in range(3000):
                data = [k, self.rng.getrandbits(32)][:k % 3]
                tag = vp.send(K['HP_OP_PING'], data)
                while tag is None:          # ring full: let the BCM run, retry
                    b.run(2000)
                    vp.drain()
                    tag = vp.send(K['HP_OP_PING'], data)
                pending[tag] = data
                sent += 1
                if k % 50 == 49:
                    b.run(4000)
                    vp.drain()
                    for t in [t for t in pending if t in vp.replies]:
                        op, st, p = vp.replies.pop(t)
                        ok += (st == 0 and p == pending.pop(t))
            t0 = b.now()
            while pending and b.now() - t0 < 100_000:
                b.run(2000)
                vp.drain()
                for t in [t for t in pending if t in vp.replies]:
                    op, st, p = vp.replies.pop(t)
                    ok += (st == 0 and p == pending.pop(t))
            row.check(ok == sent == 3000, '%d of %d pings answered correctly' % (ok, sent))
            row.check(vp.cmd_wr > K['HP_CMD_WORDS'] and vp.evt_rd > K['HP_EVT_WORDS'],
                      'rings did not wrap (%d, %d)' % (vp.cmd_wr, vp.evt_rd))
            _, st, p = vp.call(K['HP_OP_STATUS'], [0])
            row.check(p[5] == 0, '%d events lost' % p[5])
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_torn(self):
        row = T.Row('torn index words are ignored until whole')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(1, awake=False)
            tag = vp.send(K['HP_OP_PING'], [0x1234])
            # undo the publish: write only the low half of the new index
            new = vp.cmd_wr
            b.w32(K['HP_CMD_WR'], (enc(new - 2) & 0xffff0000) | new)
            b.run(20000)
            row.check(vp.index(K['HP_CMD_RD']) == 0, 'BCM consumed a torn index')
            row.check(b.r32(K['HP_CMD_RD']) == enc(0), 'CMD_RD changed')
            b.w32(K['HP_CMD_WR'], enc(new))
            op, st, p = vp.wait(tag)
            row.check(st == 0 and p == [0x1234], 'after the whole word: %s %s' % (st, p))
            # a torn EVT_RD from the host: the BCM sees no room, posts nothing,
            # counts the loss, and carries on
            b.w32(K['HP_EVT_RD'], (enc(vp.evt_rd) & 0xffff0000) | ((vp.evt_rd + 5) & 0xffff))
            tag = vp.send(K['HP_OP_PING'], [])
            b.run(20000)
            row.check(vp.index(K['HP_CMD_RD']) == vp.cmd_wr, 'BCM stopped reading commands')
            b.w32(K['HP_EVT_RD'], enc(vp.evt_rd))
            _, st, p = vp.call(K['HP_OP_STATUS'], [0])
            row.check(p[5] == 1, 'lost events %d, want 1' % p[5])
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_event_full(self):
        row = T.Row('host stops reading events: BCM drops, never blocks, LCD still served')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(2)
            sent = 0
            while sent < 700:
                tag = vp.send(K['HP_OP_PING'], [sent])
                if tag is None:
                    b.run(4000)
                    continue
                sent += 1
            t0 = b.now()
            while vp.index(K['HP_CMD_RD']) != vp.cmd_wr and b.now() - t0 < 100_000:
                b.run(2000)
            row.check(vp.index(K['HP_CMD_RD']) == vp.cmd_wr, 'commands not all consumed')
            wr = vp.index(K['HP_EVT_WR'])
            row.check((wr - vp.evt_rd) & 0xffff <= K['HP_EVT_WORDS'], 'event ring overrun')
            self.lcd_frame(row, b, h, 'vp_evfull_lcd')
            vp.drain()
            replies = len(vp.replies)
            vp.replies.clear()
            _, st, p = vp.call(K['HP_OP_STATUS'], [0])
            row.check(replies == K['HP_EVT_WORDS'] // 3 and p[5] == 700 - replies,
                      '%d replies kept, %d lost' % (replies, p[5]))
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_display(self):
        row = T.Row('OWN_DISPLAY drops LCD updates; LCD_SLEEP ends the session')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(0)
            _, st, _ = self.open_null(vp, K['HP_OPEN_OWN_DISPLAY'])
            row.check(st == 0, 'open %d' % st)
            f0, px0 = b.r32(T.MB_FRAMES), b.panel.pixels_total
            h.tick_task = True
            old = list(b.panel.fb)
            h.fb = T.random_frame(self.rng)
            h.lcd_update()
            h.settle()
            b.w32(T.MB_RECT_XY, 0)
            b.w32(T.MB_RECT_WH, 10 | 10 << 16)
            h.bcm_command(T.LCD_UPDATERECT)
            h.tick_task = False
            row.check(b.r32(T.MB_FRAMES) == f0 and b.panel.pixels_total == px0 and
                      b.panel.fb == old, 'panel drawn while the session owns it')
            row.check(b.r32(T.CMD_WORD) == T.DONE, 'LCD status %08x' % b.r32(T.CMD_WORD))
            _, st, _ = vp.call(K['HP_OP_CLOSE'], [1])
            self.lcd_frame(row, b, h, 'vp_after_close')
            # a session (not owning the display) and then LCD_SLEEP
            _, st, _ = self.open_null(vp)
            self.lcd_frame(row, b, h, 'vp_shared_display')
            h.tick_task = True
            h.lcd_sleep()
            row.check(not b.running, 'not powered down')
            # the event was posted before the power cut (read from the
            # model's memory before it was scrambled: the host would have
            # drained it before cutting power)
            row.check(any(e[0] == K['HP_EV_LOST'] for e in self.lost_events(b, vp)),
                      'no HP_EV_LOST')
            h.lcd_awake()
            h.settle()
            h.tick_task = False
            vp2 = Vp(b)
            row.check(vp2.attach() == K['HP_VERSION_WORD'], 'header after re-upload')
            _, st, _ = vp2.call(K['HP_OP_STATUS'], [0])
            row.check(st == 0, 'status after re-upload')
            self.frame_check(row, b, h, 'vp_awake')
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def lost_events(self, b, vp):
        return self._lost

    def t_cost(self):
        row = T.Row('cost: PP host-port cycles and BCM instructions per command')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(1, awake=False)
            self.open_null(vp)
            for name, op, args in (('status', K['HP_OP_STATUS'], [1]),
                                   ('ping', K['HP_OP_PING'], []),
                                   ('feed_ack', K['HP_OP_FEED'], [1, 0, 3200, 0, 0, K['HP_FEED_ACK']])):
                x0 = vp.xfers
                tag = vp.send(op, args)
                xs = vp.xfers - x0
                s0 = b.sim.steps
                b.step_until(lambda: vp.index(K['HP_EVT_WR']) != vp.evt_rd, 100_000)
                self.metrics['vp_%s_bcm_instr' % name] = b.sim.steps - s0
                x1 = vp.xfers
                op_, st, p = vp.wait(tag)
                self.metrics['vp_%s_pp_xfers' % name] = xs + (vp.xfers - x1)
                row.check(st == 0, '%s: status %d' % (name, st))
            # the idle poll: one pass of the main loop with nothing to do
            s0 = b.sim.steps
            beat0 = b.r32(K['HP_STATE']) >> 8 & 0xff
            t0 = b.now()
            while b.now() - t0 < 5000:
                b.run(1000)
            beat1 = b.r32(K['HP_STATE']) >> 8 & 0xff
            row.check(beat1 != beat0, 'heartbeat did not move in 5 ms')
            self.metrics['vp_heartbeat_steps_5ms'] = (beat1 - beat0) & 0xff
        except (T.Fail, Timeout) as e:
            row.check(False, str(e))

    def t_coverage_vp(self):
        row = T.Row('VP image: instruction kinds the self-test exercises')
        self.rows.append(row)
        import collections
        import contextlib
        import io
        ours = collections.Counter(T.S.COVER)
        T.S.COVER = collections.Counter()
        with contextlib.redirect_stdout(io.StringIO()):
            rc = T.vc2sim_tests.main([os.path.dirname(os.path.dirname(self.args.vmcs))])
        apple = set(T.S.COVER)
        T.S.COVER = None
        row.check(rc == 0, 'vc2sim self-test failed')
        extra = sorted(set(ours) - apple)
        self.metrics['vp_kinds_executed'] = len(ours)
        self.metrics['vp_kinds_not_in_selftest'] = ' '.join(k.replace(' ', ':') for k in extra)
        row.check(set(extra) <= self.ACCEPTED_KINDS,
                  'unproven kinds: %s' % ', '.join(sorted(set(extra) - self.ACCEPTED_KINDS)))

    def run(self):
        import collections
        t0 = time.time()
        self._lost = []
        cover = os.path.exists(self.args.vmcs) and not self.args.only
        if cover:
            T.S.COVER = collections.Counter()
        # the HP_EV_LOST check needs the event before the power cut: wrap
        # Host.lcd_sleep's power cut to drain first
        orig = T.Bench.power_cut
        suite = self

        def power_cut(bench, rng):
            if bench.running:
                v = Vp(bench)
                v.attach()
                v.evt_rd = 0
                v.drain()
                suite._lost = v.events
            orig(bench, rng)
        T.Bench.power_cut = power_cut
        try:
            for t in (self.t_header, self.t_pins, self.t_hello, self.t_open_close, self.t_feed,
                      self.t_unknown_and_wrap, self.t_torn, self.t_event_full,
                      self.t_display, self.t_cost) + ((self.t_coverage_vp,) if cover else ()):
                if self.args.only and self.args.only not in t.__name__:
                    continue
                n0 = len(self.rows)
                t()
                for row in self.rows[n0:]:
                    print('%-72s %5d %5d %10d' % (row.name[:72], row.cases,
                                                  row.cases - len(row.fails), row.instr))
                    for f in row.fails[:8]:
                        print('    FAIL: %s' % f)
        finally:
            T.Bench.power_cut = orig
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        prog, rep, img = self.image(DEFS)
        print()
        print(rep['lines'][0])
        for k in sorted(self.metrics):
            v = self.metrics[k]
            print('  %-28s %s' % (k, ('%.1f' % v) if isinstance(v, float) else v))
        print('%d rows, %d cases, %d failed (seed %d, %.0f s)' % (
            len(self.rows), cases, fails, self.args.seed, time.time() - t0))
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-vp'))
    ap.add_argument('--only', default='')
    a = ap.parse_args(argv)
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % T.C.GCC)
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
