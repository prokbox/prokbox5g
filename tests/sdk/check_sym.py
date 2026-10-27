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

# The SDK table is append only within an ABI major (docs/design/sdk.md
# section 11.1): an entry's index is its ABI, so a game built against an
# older table must find every entry where it was. Fails if an entry of
# the frozen list (sdk_sym.base, one name a line, the table as shipped
# or last reviewed) is gone, renamed or moved in apps/ipodui/sdk/sdk.sym.
# New entries may follow the frozen ones; append them to sdk_sym.base
# when they are reviewed.
import re
import sys

def names(path, pat):
    out = []
    with open(path) as fh:
        for line in fh:
            m = re.match(pat, line)
            if m:
                out.append(m.group(1))
    return out

base = names(sys.argv[1], r"\s*([A-Za-z_]\w*)\s*$")
cur = names(sys.argv[2], r"\s*SDK_SYM\(\s*(\w+)\s*\)")
bad = 0
for i, n in enumerate(base):
    if i >= len(cur):
        print("FAIL sdk.sym: entry %d (%s) removed" % (i, n))
        bad = 1
        break
    if cur[i] != n:
        print("FAIL sdk.sym: entry %d is %s, was %s (removed, renamed or "
              "reordered: append only)" % (i, cur[i], n))
        bad = 1
        break
dup = sorted(set(n for n in cur if cur.count(n) > 1))
if dup:
    print("FAIL sdk.sym: duplicate entries: " + ", ".join(dup))
    bad = 1
if not bad:
    print("PASS sdk.sym append only: %d frozen, %d now" % (len(base),
                                                          len(cur)))
sys.exit(bad)
