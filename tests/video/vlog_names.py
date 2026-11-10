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

"""Every video player log line's text (#224), for test_vlogline: the string
literals in apps/video/vpcore.c and vplayer.c that start with "video: ",
"sdram: " or "watch: " (say(), be_log(), chip_report()'s table), adjacent
literals joined.  One per line, "x" (register words, sdram:) or "d" and a
tab first.  Refuses to print fewer than MIN_NAMES: a parse that found
nothing must not let the test pass."""
import re
import sys

FILES = ('apps/video/vpcore.c', 'apps/video/vplayer.c')
MIN_NAMES = 100

names = set()
for f in FILES:
    src = open(f).read()
    src = re.sub(r'"\s*\n\s*"', '', src)            # "a "\n  "b" -> "a b"
    for lit in re.findall(r'"((?:video|sdram|watch): [^"\\]*)"', src):
        names.add(lit)
if len(names) < MIN_NAMES:
    sys.exit('vlog_names: %d names in %s, expected %d or more'
             % (len(names), ', '.join(FILES), MIN_NAMES))
for n in sorted(names):
    print('%s\t%s' % ('x' if n.startswith('sdram:') else 'd', n))
