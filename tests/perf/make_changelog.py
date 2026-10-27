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

"""Write a synthetic tagcache changelog for every track of a database.

    tests/perf/make_changelog.py <simdisk> [lines] [--shuffle | --realistic]

Reads <simdisk>/.rockbox/database_4.tcd (the filename tag file) and writes
<simdisk>/.rockbox/database_changelog.txt with one line per entry (or the
first 'lines' of them), in master index order (idx_id) like the shell's
export (ui_export_changelog() in apps/tagcache.c), each with a play count
and a resume position: what the shell's import reads after a rebuild.
--shuffle writes the lines in random order instead (the worst case for
the import's sequential lookup). --realistic models a changelog exported
from an older database and imported into a rebuilt one: 5% of the lines
(in album-sized blocks of 10 tracks) are moved to the end, as files
re-added or retagged later sit at the end of the old database, and 2%
more lines name files that no longer exist (deleted since the export),
spread through the file; each of those is a lookup that finds nothing.
Used by tests/perf/import50k.sh to time the changelog import on the 50k
synthetic library.
"""
import os
import random
import struct
import sys

TAG_FILENAME = 4


def entries(path):
    with open(path, 'rb') as f:
        data = f.read()
    _magic, datasize, count = struct.unpack_from('<iii', data, 0)
    pos, end = 12, 12 + datasize
    out = []
    while pos < end and len(out) < count:
        length, idx_id = struct.unpack_from('<ii', data, pos)
        name = data[pos + 8:pos + 8 + length].split(b'\0', 1)[0]
        out.append((idx_id, name.decode('utf-8', 'replace')))
        pos += 8 + length
    return out


def quote(s):
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        sys.exit(__doc__)
    db = os.path.join(args[0], '.rockbox')
    rows = sorted(entries(os.path.join(db, 'database_%d.tcd'
                                       % TAG_FILENAME)))
    if len(args) > 1:
        rows = rows[:int(args[1])]
    if '--shuffle' in sys.argv:
        random.Random(1).shuffle(rows)
    elif '--realistic' in sys.argv:
        rng = random.Random(1)
        block = 10
        nblocks = max(1, len(rows) * 5 // 100 // block)
        moved_blocks = set(rng.sample(range(len(rows) // block), nblocks))
        kept, moved = [], []
        for i, row in enumerate(rows):
            (moved if i // block in moved_blocks else kept).append(row)
        rows = kept + moved
        for k in range(len(rows) * 2 // 100):
            rows.insert(rng.randrange(len(rows) + 1),
                        (-1, '/Synth/Deleted/Gone %05d.mp3' % k))
    with open(os.path.join(db, 'database_changelog.txt'), 'w') as f:
        f.write('## Changelog version 1\n')
        for i, (_idx, name) in enumerate(rows):
            f.write('filename="%s" playcount="%d" rating="0" playtime="%d" '
                    'lastplayed="%d" commitid="1" lastelapsed="%d" '
                    'lastoffset="0" \n'
                    % (quote(name), 1 + i % 7, 60000, i + 1, 1000 + i))
    print('changelog: %d lines in %s' % (len(rows), db))


if __name__ == '__main__':
    main()
