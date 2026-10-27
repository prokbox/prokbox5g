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

"""Print the module arena size the floor gate must count: the largest
MOD_ARENA_SIZE defined in apps/ipodui/modload.h (release or developer
variant; the arithmetic of a plain integer expression is evaluated), or
65536, the 16-6 placeholder, when none can be read. tools/modpak.py is the
enforcer of the same define (docs/phase12-brief.md 3.1).

    python3 tests/perf/arena_size.py apps/ipodui/modload.h
"""
import re
import sys

best = 0
text = open(sys.argv[1]).read()
for m in re.finditer(r'^\s*#\s*define\s+MOD_ARENA_SIZE\s+(.+?)\s*(?:/\*.*)?$',
                     text, re.M):
    expr = m.group(1)
    if not re.fullmatch(r'[0-9*+()\s-]+', expr):
        continue
    try:
        best = max(best, int(eval(expr, {'__builtins__': {}})))
    except Exception:
        pass
print(best or 65536)
