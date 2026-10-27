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
# The proven-forms table's reader (tools/bcmre/proven_forms.txt).
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
"""The proven-forms table (tools/bcmre/proven_forms.txt): which
instruction, DMA and port forms silicon has proven, with the evidence.

One line per entry, '|'-separated, '#' comments:

  <domain> | <key> | <class> | <evidence>

  domain   scalar, vector, dma, port, or region (a symbol pattern whose
           code takes the class: every form in it is proven by that
           evidence, guarded by a start-up check, or under test)
  key      scalar: vc2sim.cover_key ('ldst_off16 ldh'); vector:
           vc2vpu.form_text of the operand form key; dma and port: the
           kinds scripts/proven_forms.py names; region: an fnmatch
           pattern over symbol names
  class    silicon  measured on our unit (a bench row or a whole
                    program whose output was checked)
           apple    Apple's self-test executes it (its cases pin the
                    semantics in vc2sim)
           guard    used only behind a start-up check that keeps the C
                    path if it misbehaves (the evidence names the bit)
           bench    used only in a bench check region, as the thing
                    under test
           trap     unproven: vc2sim's strict modes refuse it
  evidence the row and session, or the guard, in a few words

vc2sim's strict forms mode (vc2vpu.STRICT_FORMS) traps any vector
instruction whose operand form has no line here; scripts/proven_forms.py
checks every shipped image's forms against it (exit criterion E2).
"""
import collections
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, 'proven_forms.txt')
DOMAINS = ('scalar', 'vector', 'dma', 'port', 'region')
CLASSES = ('silicon', 'apple', 'guard', 'bench', 'trap')


def load(path=TABLE):
    """{(domain, key): (class, evidence)} in file order; region lines
    keep their order too (the first matching pattern wins)."""
    out = collections.OrderedDict()
    if not os.path.exists(path):
        return out
    for n, ln in enumerate(open(path), 1):
        ln = ln.split('#', 1)[0].strip()
        if not ln:
            continue
        parts = [p.strip() for p in ln.split('|')]
        if len(parts) != 4 or not all(parts):
            raise ValueError('%s:%d: want domain | key | class | evidence: %r' % (path, n, ln))
        dom, key, cls, ev = parts
        if dom not in DOMAINS or cls not in CLASSES:
            raise ValueError('%s:%d: domain %r or class %r unknown' % (path, n, dom, cls))
        if (dom, key) in out:
            raise ValueError('%s:%d: %s %r listed twice' % (path, n, dom, key))
        out[(dom, key)] = (cls, ev)
    return out


def regions(table):
    """[(pattern, class, evidence)] of the region lines, in order"""
    return [(k, c, e) for (d, k), (c, e) in table.items() if d == 'region']
