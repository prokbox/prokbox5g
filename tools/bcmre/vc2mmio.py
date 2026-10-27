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
"""Modelled peripheral bus for vc2sim (BCM2722 hardware recovery, #60).

Bus replaces the simulator's flat Memory.  RAM below the simulator's
32 MB behaves as before; every other address is a device access: it is
logged with the pc that made it (and a sequence number), writes are remembered, and
reads return the last value written unless a register model (Rule)
says otherwise.  A read that the same instruction repeats more than
`poll_limit` times with an unchanged result is a stalled polling loop
and stops the run (PollStall) naming the register, so the model can be
told which bit the code is waiting for.

Rules (also from the command line, --reg):
    ADDR=V        reads return V
    ADDR|=M       reads return (last write | M)
    ADDR&=M       reads return (last write & M)
    ADDR^=M       reads return (last write ^ M)   (e.g. a self-clearing bit)
    ADDR+=N       reads return a counter that advances N per read
                  (free-running timers)
ADDR is hex or decimal; a rule may cover a range: LO-HI (inclusive).
A suffix @N applies the rule only from the Nth read after the last
write to that register.  With |= the bits of M then become status bits:
read as 0 for the first N reads after a write, then 1, whatever was
written ("busy, then done": 0x10000c00|=1@1).

Aliases (--alias LO-HI:BASE) map an address window onto RAM at BASE,
for memory the image reaches through a cache-alias or SDRAM address.

Nothing here is Apple's: the register meanings are recovered facts
written up in docs/research/bcm2722/hardware.md, and the rules are
the minimum each recovered sequence needs to run.
"""
import collections
import re

MASK = 0xffffffff


class Access(collections.namedtuple('Access', 'seq pc rw addr size value us')):
    def __str__(self):
        return '%8d  %08x  %s%d %08x %s %08x' % (
            self.seq, self.pc, self.rw, self.size * 8, self.addr,
            '->' if self.rw == 'R' else '<-', self.value)


class Rule:
    """Read model for one register (or a range of them)."""
    def __init__(self, kind, v, delay=0):
        self.kind, self.v, self.delay = kind, v & MASK, delay
        self.count = 0

    def read(self, last, since_write=1 << 30):
        k = self.kind
        if self.delay:
            # status bits: computed, never the written value
            if k == '|=':
                return (last & ~self.v) | (self.v if since_write >= self.delay else 0)
            if since_write < self.delay:
                return last
        if k == '=':
            return self.v
        if k == '|=':
            return last | self.v
        if k == '&=':
            return last & self.v
        if k == '^=':
            return last ^ self.v
        if k == '+=':
            self.count = (self.count + self.v) & MASK
            return self.count
        raise ValueError(k)

    def __repr__(self):
        return 'Rule(%s0x%x)' % (self.kind, self.v)


RULE_RE = re.compile(r'^(0x[0-9a-fA-F]+|\d+)(?:-(0x[0-9a-fA-F]+|\d+))?'
                     r'(=|\|=|&=|\^=|\+=)(0x[0-9a-fA-F]+|-?\d+)(?:@(\d+))?$')


def parse_rule(text):
    """'0x10000c00|=1' -> (lo, hi, Rule)."""
    m = RULE_RE.match(text.replace(' ', ''))
    if not m:
        raise ValueError('bad register rule %r' % text)
    lo = int(m.group(1), 0)
    hi = int(m.group(2), 0) if m.group(2) else lo
    return lo, hi, Rule(m.group(3), int(m.group(4), 0), int(m.group(5) or 0))


class PollStall(Exception):
    """A polling loop that cannot end under the current model."""
    def __init__(self, acc, count):
        self.acc, self.count = acc, count
        super().__init__('polling loop: pc %08x reads %08x = %08x %d times '
                         'unchanged (add a --reg rule)' % (acc.pc, acc.addr,
                                                           acc.value, count))


def make_bus(Memory, SimError, Misaligned=None):
    """Build the Bus class on top of vc2sim's Memory (passed in so that
    this module does not import vc2sim and create a second copy).
    Misaligned (a SimError subclass) is raised for unaligned accesses."""
    Misaligned = Misaligned or SimError

    class Bus(Memory):
        def __init__(self, sim, size, poll_limit=2000, log_limit=1 << 20):
            super().__init__(size)
            self.sim = sim
            self.regs = {}                  # last value written, per address
            self.rules = []                 # (lo, hi, Rule), first match wins
            self.aliases = []               # (lo, hi, ram base)
            self.log = []
            self.log_limit = log_limit
            self.poll_limit = poll_limit
            self._last = None
            self._repeat = 0
            self.on_write = {}              # addr -> fn(bus, addr, value)
            self.seq = 0                    # device accesses so far
            self.since_write = {}           # addr -> reads since last write
            self.devices = []               # (lo, hi, device model)
            self.timer = None               # SysTimer, for time stamps

        # ---- configuration
        def rule(self, text_or_lo, hi=None, rule=None):
            if isinstance(text_or_lo, str):
                self.rules.insert(0, parse_rule(text_or_lo))
            else:
                self.rules.insert(0, (text_or_lo, hi, rule))

        def alias(self, lo, hi, base):
            self.aliases.append((lo, hi, base))

        def attach(self, dev):
            self.devices.append((dev.base, dev.base + dev.size - 1, dev))
            dev.bus = self
            if isinstance(dev, SysTimer):
                self.timer = dev
            return dev

        def device(self, cls):
            for _, _, d in self.devices:
                if isinstance(d, cls):
                    return d
            return None

        # ---- device side
        def _ram(self, a, n):
            for lo, hi, base in self.aliases:
                if lo <= a and a + n - 1 <= hi:
                    return base + (a - lo)
            return None

        def _pc(self):
            return self.sim.r[31]

        def _record(self, rw, a, n, v):
            self.seq += 1
            acc = Access(self.seq, self._pc(), rw, a, n, v,
                         self.timer.clo if self.timer else 0)
            if len(self.log) < self.log_limit:
                self.log.append(acc)
            return acc

        def dev_read(self, a, n):
            if a & (n - 1):
                raise Misaligned('unaligned %d-byte device read at 0x%08x' % (n, a))
            last = self.regs.get(a, 0)
            since = self.since_write.get(a, 1 << 30)
            self.since_write[a] = since + 1
            v = last
            for lo, hi, r in self.rules:
                if lo <= a <= hi:
                    v = r.read(last, since)
                    break
            else:
                for lo, hi, d in self.devices:
                    if lo <= a <= hi:
                        dv = d.read(a - lo)
                        if dv is not None:
                            v = dv
                        break
            v &= (1 << (8 * n)) - 1
            acc = self._record('R', a, n, v)
            key = (acc.pc, a, v)
            if key == self._last:
                self._repeat += 1
                if self._repeat >= self.poll_limit:
                    self._repeat = 0
                    raise PollStall(acc, self.poll_limit)
            else:
                self._last, self._repeat = key, 0
            return v

        def dev_write(self, a, n, v):
            if a & (n - 1):
                raise Misaligned('unaligned %d-byte device write at 0x%08x' % (n, a))
            v &= (1 << (8 * n)) - 1
            self._record('W', a, n, v)
            self.regs[a] = v
            self.since_write[a] = 0
            self._last = None
            fn = self.on_write.get(a)
            if fn:
                fn(self, a, v)
            for lo, hi, d in self.devices:
                if lo <= a <= hi:
                    d.write(a - lo, v)
                    break

        # ---- Memory interface (RAM fast path first)
        def r8(self, a):
            if 0 <= a < self.size:
                return self.b[a]
            ra = self._ram(a, 1)
            return self.b[ra] if ra is not None else self.dev_read(a, 1)

        def r16(self, a):
            if 0 <= a < self.size - 1:
                if a & 1:
                    raise Misaligned('unaligned 2-byte access at 0x%08x' % a)
                return self.b[a] | self.b[a + 1] << 8
            ra = self._ram(a, 2)
            return Memory.r16(self, ra) if ra is not None else self.dev_read(a, 2)

        def r32(self, a):
            if 0 <= a < self.size - 3:
                return Memory.r32(self, a)
            ra = self._ram(a, 4)
            return Memory.r32(self, ra) if ra is not None else self.dev_read(a, 4)

        def w8(self, a, v):
            if 0 <= a < self.size:
                self.b[a] = v & 0xff
                return
            ra = self._ram(a, 1)
            if ra is not None:
                self.b[ra] = v & 0xff
            else:
                self.dev_write(a, 1, v)

        def w16(self, a, v):
            if 0 <= a < self.size - 1:
                return Memory.w16(self, a, v)
            ra = self._ram(a, 2)
            if ra is not None:
                return Memory.w16(self, ra, v)
            self.dev_write(a, 2, v)

        def w32(self, a, v):
            if 0 <= a < self.size - 3:
                return Memory.w32(self, a, v)
            ra = self._ram(a, 4)
            if ra is not None:
                return Memory.w32(self, ra, v)
            self.dev_write(a, 4, v)

        # block access (the vector unit's loads and stores): RAM or an
        # alias window (SDRAM); a block never reaches a device register
        def read(self, a, n):
            if 0 <= a and a + n <= self.size:
                return bytes(self.b[a:a + n])
            ra = self._ram(a, n)
            if ra is None:
                raise SimError('block read of device space 0x%08x+%d' % (a, n))
            return bytes(self.b[ra:ra + n])

        def write(self, a, data):
            n = len(data)
            if 0 <= a and a + n <= self.size:
                self.b[a:a + n] = data
                return
            ra = self._ram(a, n)
            if ra is None:
                raise SimError('block write to device space 0x%08x+%d' % (a, n))
            self.b[ra:ra + n] = data

        # ---- reporting
        def compact(self):
            """The log with runs of accesses by the same instruction folded
            (same register, or a constant address stride):
            [first Access, n, last value, stride]."""
            out = []
            for acc in self.log:
                if out:
                    first, n, _, stride = out[-1]
                    same = (first.pc, first.rw, first.size) == (acc.pc, acc.rw, acc.size)
                    d = acc.addr - first.addr
                    if same and (d == stride * n if n > 1 else abs(d) <= 64):
                        out[-1][1] += 1
                        out[-1][2] = acc.value
                        out[-1][3] = d // n if n == 1 else stride
                        continue
                out.append([acc, 1, acc.value, 0])
            return out

        def summary(self, lo=0, hi=MASK):
            """Per register: reads, writes, distinct values written."""
            st = collections.OrderedDict()
            for acc in self.log:
                if not lo <= acc.addr <= hi:
                    continue
                s = st.setdefault(acc.addr, [0, 0, []])
                if acc.rw == 'R':
                    s[0] += 1
                else:
                    s[1] += 1
                    if acc.value not in s[2] and len(s[2]) < 8:
                        s[2].append(acc.value)
            return st

    return Bus


# --------------------------------------------------------------------------
# Device models.  Each covers one register block; read() returns None to
# fall back to "last value written".  The layouts are the recovered ones
# (hardware.md): confidence there, not here.

class SysTimer:
    """Free-running 1 MHz counter with four compare channels.
    +0 status: bit k = channel k matched (write 1 to clear); +4 counter;
    +8/+0c/+10/+14 compare 0-3.  Channel k raises interrupt source 16+k."""
    base, size = 0x10001000 + 0x800, 0x20

    def __init__(self, us_per_read=1):
        self.clo = 0                # simulated time, also the host's clock
        self.count = 0              # what +4 reads when a gate is attached
        self.gate = None            # callable: False = counter clock off
        self.cmp = [0] * 4
        self.armed = [False] * 4
        self.cs = 0
        self.us_per_read = us_per_read

    def _update(self):
        for k in range(4):
            if self.armed[k] and (self.clo - self.cmp[k]) & MASK < 0x80000000:
                self.cs |= 1 << k
                self.armed[k] = False

    def advance(self, us):
        self.clo = (self.clo + us) & MASK
        if self.gate is not None and self.gate():
            self.count = (self.count + us) & MASK
        self._update()

    def read(self, off):
        if off == 4:
            self.advance(self.us_per_read)
            return self.clo if self.gate is None else self.count
        self._update()
        if off == 0:
            return self.cs
        if 8 <= off <= 0x14:
            return self.cmp[(off - 8) >> 2]
        return None

    def write(self, off, v):
        if off == 0:
            self.cs &= ~v
        elif 8 <= off <= 0x14:
            k = (off - 8) >> 2
            self.cmp[k] = v
            self.armed[k] = True

    def next_event(self):
        """Microseconds to the nearest armed compare, or None."""
        d = [(self.cmp[k] - self.clo) & MASK for k in range(4) if self.armed[k]]
        return min(d) if d else None

    def sources(self):
        return [16 + k for k in range(4) if self.cs >> k & 1]


class Stall(Exception):
    """An access to a block whose clock is off: on the chip the core
    would wait on the bus for ever, with no exception.  Harnesses treat
    it as a hung core."""


class ClockBlock:
    """The clock block at 0x10001C00, only as far as bcmfw's tests need
    it: a gate for the 1 MHz counter and one for the LCD interface.
    Writes need the 0xA5A5 password in bits 31:16 and read back with it.
    Which register opens which gate on silicon is not known (device
    sessions 2-3 found the counter stopped after a host start); each gate
    names the hypothesis under test:
      timer_on: 'reset' (runs from power-up), '4c' (0x4C written 0x51B,
                Apple's write that looks like a 27 MHz / 27 prescaler),
                '58' (0x58 written 0x101), None (never runs)
      lcd_on:   'reset', '4c', '58' as above, None (every LCD access
                stalls the core)"""
    base, size = 0x10001C00, 0x60
    OPENS = {'4c': (0x4C, 0x51B), '58': (0x58, 0x101)}

    def __init__(self, timer_on='reset', lcd_on='reset'):
        self.timer_on, self.lcd_on = timer_on, lcd_on
        self.reset()

    def reset(self):
        self.r = {}

    def _open(self, how):
        if how == 'reset':
            return True
        if how is None:
            return False
        off, v = self.OPENS[how]
        return self.r.get(off) == v

    def timer_runs(self):
        return self._open(self.timer_on)

    def lcd_runs(self):
        return self._open(self.lcd_on)

    def read(self, off):
        return 0xA5A50000 | self.r[off] if off in self.r else 0

    def write(self, off, v):
        if v >> 16 == 0xA5A5:
            self.r[off] = v & 0xFFFF


class HostDoorbell:
    """The host-port doorbell, interrupt source 23 [M] (hardware.md, "Host
    port"): Apple's handler acknowledges it by writing 1 to 0x10003800
    (@d56a); Rockbox writes BCM_CONTROL = 0x31 after each command word.
    Which host write raises it is not known, so the harness chooses:
    raises=True models "BCM_CONTROL = 0x31 raises it" (ring() sets it
    pending until acknowledged), raises=False models "it never fires"."""
    base, size = 0x10003800, 4
    SOURCE = 23

    def __init__(self, raises=True):
        self.raises = raises
        self.pending = False
        self.rings = self.acks = 0

    def ring(self):
        self.rings += 1
        if self.raises:
            self.pending = True

    def read(self, off):
        return 0

    def write(self, off, v):
        if off == 0 and v & 1:
            self.acks += 1
            self.pending = False

    def sources(self):
        return [self.SOURCE] if self.pending else []


class IntCtl:
    """Interrupt controller.  +0 bits 2:0: current level (sources at or
    below it are held off); +4: (priority << 8) | vector of the highest
    pending source, 0 if none (vector = 32 + source, handler address at
    4 * vector); +10..+1c: 4-bit priority per source (0 = disabled),
    eight sources per word.  Pending state comes from the device models
    (level-triggered)."""
    base, size = 0x10001000, 0x20

    def __init__(self, *sources):
        self.srcs = list(sources)     # callables returning pending source numbers
        self.level = 0
        self.pri = [0] * 32

    def pending(self):
        best = None
        for f in self.srcs:
            for s in f():
                p = self.pri[s]
                if p > self.level and (best is None or p > best[0]):
                    best = (p, s)
        return best

    def read(self, off):
        if off == 0:
            return None
        if off == 4:
            b = self.pending()
            return 0 if b is None else (b[0] << 8) | (32 + b[1])
        return None

    def write(self, off, v):
        if off == 0:
            self.level = v & 7
        elif 0x10 <= off <= 0x1c:
            k = (off - 0x10) >> 2
            for i in range(8):
                self.pri[8 * k + i] = (v >> (4 * i)) & 15


# --------------------------------------------------------------------------
# Reading a log as LCD traffic (hardware.md, "LCD").

LCD_CMD, LCD_DATA = 0x10004178, 0x1000417c
GPIO_SET = {0x10002810: 0, 0x10002814: 32}
GPIO_CLR = {0x10002818: 0, 0x1000281c: 32}


def describe_lcd(log, gap_us=500, max_data=24):
    """Device writes as panel commands: 'cmd XX data[n] ...', GPIO
    set/clear by pin, other writes raw, and a 'delay' line wherever
    gap_us or more passed between two writes.  Returns lines."""
    out = []
    cmd, data = [None], []
    last = [None]

    def flush():
        if cmd[0] is not None:
            d = ''
            if data:
                d = ' data[%d] %s%s' % (len(data), ' '.join('%x' % v for v in data[:max_data]),
                                        ' ...' if len(data) > max_data else '')
            out.append('cmd %04x%s' % (cmd[0], d))
        cmd[0] = None
        del data[:]

    for acc in log:
        if acc.rw != 'W':
            continue
        if last[0] is not None and acc.us - last[0] >= gap_us:
            flush()
            out.append('delay %d us' % (acc.us - last[0]))
        last[0] = acc.us
        a, v = acc.addr, acc.value
        if a == LCD_CMD:
            flush()
            cmd[0] = v
        elif a == LCD_DATA and cmd[0] is not None:
            data.append(v)
        else:
            flush()
            if a in GPIO_SET or a in GPIO_CLR:
                base = GPIO_SET.get(a, GPIO_CLR.get(a))
                pins = [base + i for i in range(32) if v >> i & 1]
                out.append('gpio %s %s' % ('set' if a in GPIO_SET else 'clear',
                                           ','.join(map(str, pins))))
            else:
                out.append('write %08x <- %08x' % (a, v))
    flush()
    return out


# --------------------------------------------------------------------------
# LCD panel model (bcmfw tests, #60): the panel behind the LCD interface.

class LcdPanel:
    """The LCD controller block at 0x10004000 plus the panel behind it.

    Records the traffic that matters for a power-on sequence as events
    (time-stamped with the system timer):
        ('ctl', offset, value)   interface writes (+0x00, +0x10, +0x14 ...)
        ('cmd', index)           a write to the index register +0x178
        ('data', value)          a parameter / register value (+0x17C),
                                 except pixel data (counted, not listed)
        ('fsel3', value)         GPIO function select for pins 48-63
        ('reset', level)         GPIO 48 (the panel's reset, active low)
    and decodes windows and RAM writes into a 320x240 RGB565 frame.

    kind picks the controller family (hardware.md "Panel identification"):
      'dcs'  panel 0: 0x2A/0x2B take 4 bytes (start hi, lo, end hi, lo),
             0x2C starts a RAM write at (x0, y0)
      'r2xx' panels 1, 3: 0x210/0x211 x range, 0x212/0x213 y range,
             0x200 x address, 0x201 y address, 0x202 RAM write
      'r4x'  panel 2: 0x45/0x44 x start/end, 0x46 = y end << 8 | y start,
             0x20 y address, 0x21 x address, 0x22 RAM write
    The meanings beyond the full-frame values are the same readings
    bcmfw's lcd.c uses ([M]/[L]), so a sub-rectangle test checks
    consistency with them, not the real panel.  Pixels advance x first,
    wrapping inside the window."""
    base, size = 0x10004000, 0x200
    KINDS = {0: 'dcs', 1: 'r2xx', 2: 'r4x', 3: 'r2xx'}
    RAMWR = {'dcs': 0x2C, 'r2xx': 0x202, 'r4x': 0x22}

    def __init__(self, kind, width=320, height=240):
        self.kind = kind
        self.w, self.h = width, height
        self.events = []
        self.fb = [None] * (width * height)
        self.pixels = 0             # pixel writes since the last RAM write
        self.pixels_total = 0
        self.high_bits = 0          # pixel writes with bits 31:16 set
        self.stray = 0              # data writes outside any command
        self.outside = 0            # pixel writes before a reset/init
        self.reset_level = None
        self.pulses = 0             # reset low->high edges seen
        self.cmd = None
        self.params = []
        self.ram = False
        self.win = [0, 0, width - 1, height - 1]
        self.cur = [0, 0]
        self.regs = {}
        self.bus = None
        # automatic pixel runs fed through the FIFO at +0x0C (the DMA path)
        self.ctl = {}               # last value per interface register
        self.run_left = 0           # bus transfers still expected in the run
        self.runs = []              # (length, format, control) per run started
        self.fifo_words = 0
        self.fifo_stray = 0         # FIFO words with no run armed
        self.dreq_enabled = True    # False: a dead request line (tests)
        self.gate = None            # callable: False = interface clock off

    def _now(self):
        t = self.bus.timer if self.bus is not None else None
        return t.clo if t is not None else 0

    def _ev(self, *e):
        self.events.append(e + (self._now(),))

    # ---- GPIO side (wired by the harness through Bus.on_write)
    def gpio_write(self, bus, addr, v):
        if addr == 0x1000280c:
            self._ev('fsel3', v)
        elif addr in (0x10002814, 0x1000281c) and v >> 16 & 1:
            level = 1 if addr == 0x10002814 else 0
            if self.reset_level == 0 and level == 1:
                self.pulses += 1
            self.reset_level = level
            self.ram = False
            self._ev('reset', level)

    def power_cut(self):
        self.reset_level = None
        self.pulses = 0
        self.ram = False
        self.run_left = 0
        self.ctl = {}

    def dreq(self):
        """The interface's DMA request line (DREQ 3): raised while an
        armed run still expects data."""
        return self.dreq_enabled and self.run_left > 0

    # ---- interface registers
    def read(self, off):
        if self.gate is not None and not self.gate():
            raise Stall('LCD interface read +0x%x with its clock off' % off)
        if off == 0x04 and 4 in self.ctl:
            # [H] device session 5: +0x04 counts the run's transfers UP as
            # they go out, from 0 to the length written (0x12c00 after a
            # full frame, 0xa5 one 64 us poll in)
            return self.ctl[4] - self.run_left
        if off == 0x00 and 0 in self.ctl and self.ctl[0] & 9 == 9:
            # [M] device session 5: a run started with 0x329 reads 0x325
            # while it runs and 0x323 once its transfers are out: bit 3
            # (go) self-clears, bit 2 = busy, bit 1 = done
            return (self.ctl[0] & ~0xE) | (4 if self.run_left > 0 else 2)
        return None

    def write(self, off, v):
        if self.gate is not None and not self.gate():
            raise Stall('LCD interface write +0x%x with its clock off' % off)
        if off == 0x178:
            self._index(v & 0xffff)
        elif off == 0x17c:
            self._data(v)
        elif off == 0x0c:
            self._fifo(v)
        else:
            self._ev('ctl', off, v)
            self.ctl[off] = v
            if off == 0:
                # Apple's run start: 0x231|b<<8, +4 = length, +8 = format,
                # then 0x229|b<<8 [H values].  Our reading: bit 3 starts
                # the run, a write without it (e.g. 1) ends any run [L].
                if v & 9 == 9:
                    self.run_left = self.ctl.get(4, 0)
                    self.runs.append((self.run_left, self.ctl.get(8), v))
                else:
                    self.run_left = 0

    def _fifo(self, v):
        """A 32-bit FIFO word: two 16-bit bus transfers, the low half
        first (little-endian memory order, as the CPU path sends) [M]."""
        self.fifo_words += 1
        if self.run_left < 2:
            self.fifo_stray += 1
            return
        self.run_left -= 2
        self._data(v & 0xffff)
        self._data(v >> 16 & 0xffff)

    def _index(self, c):
        self.cmd, self.params = c, []
        self.ram = c == self.RAMWR[self.kind] and c != 0
        if self.ram:
            x0, y0 = self.win[0], self.win[1]
            if self.kind == 'dcs':
                self.cur = [x0, y0]
            self.pixels = 0
        self._ev('cmd', c)

    def _data(self, v):
        if self.ram:
            if v >> 16:
                self.high_bits += 1
            if self.reset_level != 1 or self.pulses < 2:
                self.outside += 1
            x, y = self.cur
            if 0 <= x < self.w and 0 <= y < self.h:
                self.fb[y * self.w + x] = v & 0xffff
            self.pixels += 1
            self.pixels_total += 1
            x += 1
            if x > self.win[2]:
                x = self.win[0]
                y += 1
                if y > self.win[3]:
                    y = self.win[1]
            self.cur = [x, y]
            return
        if self.cmd is None:
            self.stray += 1
        v &= 0xffff
        self._ev('data', v)
        self.params.append(v)
        c, p = self.cmd, self.params
        if self.kind == 'dcs':
            if c in (0x2a, 0x2b) and len(p) == 4:
                a, b = p[0] << 8 | p[1], p[2] << 8 | p[3]
                if c == 0x2a:
                    self.win[0], self.win[2] = a, b
                else:
                    self.win[1], self.win[3] = a, b
            return
        self.regs[c] = v
        if self.kind == 'r2xx':
            idx = {0x210: 0, 0x212: 1, 0x211: 2, 0x213: 3}
            if c in idx:
                self.win[idx[c]] = v
            elif c == 0x200:
                self.cur[0] = v
            elif c == 0x201:
                self.cur[1] = v
        else:
            if c == 0x45:
                self.win[0] = v
            elif c == 0x44:
                self.win[2] = v
            elif c == 0x46:
                self.win[1], self.win[3] = v & 0xff, v >> 8
            elif c == 0x20:
                self.cur[1] = v
            elif c == 0x21:
                self.cur[0] = v

    def sequence(self):
        """Events without time stamps, interface writes to the window and
        pixel registers only (what a power-on sequence is made of)."""
        return [e[:-1] for e in self.events]


# --------------------------------------------------------------------------
# DMA engine model (bcmfw DMA frame push, #60).

class DmaEngine:
    """The DMA engine at 0x10000800: eight channels 0x80 apart, three
    sub-channels each (hardware.md "DMA engine").

    Per channel: +0x00 config/state (bit 0 or bit 1 enables: our LCD push
    sets bit 0, PW8's channel 7 copy 0x0000_E802 [silicon] sets bit 1 and
    runs; bits 20:16 the request
    line, bits 31:29 a state that reads 2 once a chain has finished and
    the channel is idle, 1 while a block runs); +0x04 status, write 1 to
    clear: bit 13 + k = sub-channel k finished its chain, and bits 8 and
    9 = a chain paced by the LCD's request line (DREQ 3) finished;
    +0x38 + 0x20 k: writing a control-block address starts sub-channel
    k, reading gives the block in progress (0 when idle).  Control
    blocks are 32 bytes: next, source, destination, length, transfer
    info, 2-D skips (dst << 16 | src, in units).

    Status bits (hardware.md "DMA engine", completion): Apple's handler
    (@7b7c) acknowledges the whole word and calls each registered user
    whose mask meets it; the generic transfers register 0x800 | 1 << (13
    + k) (@804c) [H mask], the LCD push 0x100 (@145e2) [H mask].  Device
    session 4 read 0x2300 and state 2 after our full-frame push on
    sub-channel 0 [H].  Bit 9's meaning, and whether the LCD bits follow
    the run or the chain, are [L]; state 1 while busy is [L].

    Our readings, as in bcmfw/dma.h: the length unit is the smaller of
    the source and destination widths; 2-D lengths are rows << 16 | units
    per row, and after each row the skips are added (Apple's 2-D setup
    computes them as pitch - width); bit 15 paces the destination by the
    channel's request line; bit 17 increments the destination; a chain
    ends at next = 0 and then sets the status bit.

    Time: transfers progress with the system timer, `paced_wpu` 32-bit
    words per microsecond while the paced target requests data (LCD bus
    speed is a device question: 5 words = 10 pixels per us is a guess),
    `mem_wpu` otherwise.  Progress is computed lazily whenever the core
    or the harness reads a DMA register (or calls run_until_idle).

    Counted for tests: blocks started, words moved, reads or writes
    outside internal SRAM and the peripheral space (`outside`), and starts
    on a disabled channel (`errors`)."""
    base, size = 0x10000800, 0x400
    SRAM_END = 0x140000
    ENABLE = 0x3                # either enable bit (PW8's E802: bit 1)

    def __init__(self, paced_wpu=5, mem_wpu=64):
        self.paced_wpu, self.mem_wpu = paced_wpu, mem_wpu
        self.cs = [0] * 8
        self.status = [0] * 8
        self.finished = [False] * 8     # a chain ended since the last config write
        self.subs = {}              # (chan, sub) -> transfer state dict
        self.dreq = {}              # request line -> callable() -> bool
        self.bus = None
        self.blocks = 0
        self.words = 0
        self.outside = []
        self.errors = []
        self.sources_touched = set()
        self._t = None

    def attach_dreq(self, line, fn):
        self.dreq[line] = fn

    # ---- registers
    def read(self, off):
        self.progress()
        ch, r = off >> 7, off & 0x7f
        if r == 0:
            v = self.cs[ch] & 0x1FFFFFFF
            if any(k[0] == ch for k in self.subs):
                return v | 1 << 29
            if self.finished[ch] and v & self.ENABLE:
                return v | 2 << 29
            return self.cs[ch]
        if r == 4:
            return self.status[ch]
        if r >= 0x38 and (r - 0x38) % 0x20 == 0:
            st = self.subs.get((ch, (r - 0x38) // 0x20))
            return st['cb'] if st else 0
        return None

    def write(self, off, v):
        self.progress()
        ch, r = off >> 7, off & 0x7f
        if r == 0:
            self.cs[ch] = v
            self.finished[ch] = False
            if not v & self.ENABLE:
                for k in [k for k in self.subs if k[0] == ch]:
                    del self.subs[k]
        elif r == 4:
            self.status[ch] &= ~v
        elif r >= 0x38 and (r - 0x38) % 0x20 == 0:
            k = (r - 0x38) // 0x20
            if not self.cs[ch] & self.ENABLE:
                self.errors.append('start on disabled channel %d' % ch)
                return
            if v & 31:
                self.errors.append('control block %08x not 32-byte aligned' % v)
                return
            self._load(ch, k, v)

    def sources(self):
        """Interrupt sources pending (channel n = source n)."""
        return [ch for ch in range(8) if self.status[ch]]

    # ---- transfers
    def _load(self, ch, k, cb):
        m = self.bus
        f = [m.r32(cb + 4 * i) for i in range(6)]
        nxt, src, dst, ln, ti, stride = f
        sw, dw = ti >> 4 & 7, ti >> 12 & 7
        unit = 1 << min(sw, dw)
        if ti & 0x10000:
            rows, per_row = ln >> 16, ln & 0xffff
        else:
            rows, per_row = 1, ln
        sskip, dskip = stride & 0xffff, stride >> 16
        sskip -= (sskip & 0x8000) << 1
        dskip -= (dskip & 0x8000) << 1
        if self.strict:
            self._strict(cb, src, dst, sw, dw, unit, rows, sskip, dskip)
        if unit == 32 and dst & 31 and self.mis32 == 'round':
            # bench row ALGN D32M (sitting 2 [silicon]): a 32-byte unit to
            # 4 mod 32 wrote the source's 32 bytes at the destination
            # rounded down (13121110 17161514 ... from +0, not from +4)
            dst &= ~31
        self.blocks += 1
        self.subs[(ch, k)] = dict(cb=cb, next=nxt, src=src, dst=dst, unit=unit,
                                  rows=rows, per_row=per_row, row=0, col=0,
                                  sskip=sskip * unit, dskip=dskip * unit,
                                  paced=bool(ti & 0x8000), dinc=bool(ti & 0x20000),
                                  line=self.cs[ch] >> 16 & 31)

    # Strict alignment (vc2sim.set_strict_align; memory-access.md
    # "Alignment"): what silicon has run is units of 4 or 32 bytes (LCD
    # channel 1 S5-S7, audio channel 5 S35, channel 7 PW8) and, in one
    # dimension, units of 1, 2 and 4 bytes at addresses that are
    # multiples of their width (bench row ALGN D1B5, D2H3, D4W9, sitting
    # 2: each copies exactly), every address a multiple of its own width.
    # A unit of another size (8, 16, 64, 128), a sub-word unit in 2-D, an
    # address off its width (D32M: a 32-byte unit at 4 mod 32 copies to
    # the address rounded down; modelled, still refused) and a 2-D skip
    # that breaks the widths are refused: 'trap' raises, 'log' records in
    # self.align and goes on.
    strict = ''
    trap = Exception
    align = []
    # A 32-byte unit at a destination off 32: 'round' (silicon, D32M) or
    # 'exact' (the model before 2026-10-26)
    mis32 = 'round'

    def _strict(self, cb, src, dst, sw, dw, unit, rows, sskip, dskip):
        why = []
        if unit not in (1, 2, 4, 32):
            why.append('%d-byte units' % unit)
        elif unit < 4 and rows > 1:
            why.append('%d-byte units in 2-D' % unit)
        if src & ((1 << sw) - 1):
            why.append('source %08x off its %d-byte width' % (src, 1 << sw))
        if dst & ((1 << dw) - 1):
            why.append('destination %08x off its %d-byte width' % (dst, 1 << dw))
        if rows > 1 and ((sskip * unit) % (1 << sw) or (dskip * unit) % (1 << dw)):
            why.append('2-D skips %d/%d units break the widths' % (sskip, dskip))
        if not why:
            return
        msg = 'strict alignment: DMA block %08x: %s' % (cb, '; '.join(why))
        if self.strict == 'log':
            self.align.append(msg)
            return
        raise self.trap(msg)

    def _check(self, a, what):
        if not (a < self.SRAM_END or 0x10000000 <= a < 0x10010000):
            self.outside.append('%s %08x' % (what, a))

    def _dev_or_ram_write(self, a, v):
        for lo, hi, d in self.bus.devices:
            if lo <= a <= hi:
                d.write(a - lo, v)
                return
        self.bus.w32(a, v)

    def _step(self, ch, k, st):
        """Move one unit; returns False when the transfer is stalled."""
        if st['paced'] and not self.dreq.get(st['line'], lambda: True)():
            return False
        u = st['unit']
        if u < 4:
            # a byte or halfword unit moves exactly its bytes (bench row
            # ALGN D1B5, D2H3 [silicon])
            a, d = st['src'], st['dst']
            self._check(a, 'read')
            self._check(d, 'write')
            self.sources_touched.add(a & ~0xfff)
            v = self.bus.r8(a) if u == 1 else self.bus.r16(a)
            if u == 1:
                self.bus.w8(d, v)
            else:
                self.bus.w16(d, v)
            self.words += 1
            u_done = True
        else:
            u_done = False
        for i in range(0, 0 if u_done else max(u, 4), 4):
            a = st['src'] + i
            self._check(a, 'read')
            self.sources_touched.add(a & ~0xfff)
            v = self.bus.r32(a)
            d = st['dst'] + (i if st['dinc'] else 0)
            self._check(d, 'write')
            self._dev_or_ram_write(d, v)
            self.words += 1
        st['src'] += u
        if st['dinc']:
            st['dst'] += u
        st['col'] += 1
        if st['col'] == st['per_row']:
            st['col'] = 0
            st['row'] += 1
            st['src'] += st['sskip']
            if st['dinc']:
                st['dst'] += st['dskip']
            if st['row'] >= st['rows']:
                if st['next']:
                    self._load(ch, k, st['next'])
                else:
                    del self.subs[(ch, k)]
                    self.finished[ch] = True
                    self.status[ch] |= self.done_bits(k, st)
        return True

    LCD_DREQ = 3
    INT_LCD = 0x300             # bits 8 (Apple's LCD mask) and 9 (seen with it)

    @classmethod
    def done_bits(cls, k, st):
        """Status bits a finished chain on sub-channel k sets."""
        bits = 1 << (13 + k)
        if st['paced'] and st['line'] == cls.LCD_DREQ:
            bits |= cls.INT_LCD
        return bits

    def progress(self):
        t = self.bus.timer if self.bus is not None else None
        now = t.clo if t is not None else 0
        if self._t is None or not self.subs:
            self._t = now
            return
        dt = (now - self._t) & MASK
        self._t = now
        for (ch, k), st in list(self.subs.items()):
            budget = dt * (self.paced_wpu if st['paced'] else self.mem_wpu)
            words = max(1, st['unit'] // 4)
            while budget >= words and (ch, k) in self.subs:
                st = self.subs[(ch, k)]
                if not self._step(ch, k, st):
                    break
                budget -= words

    def busy(self):
        return bool(self.subs)
