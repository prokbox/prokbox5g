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

Reads database_4.tcd (the filename tag file) of the active database
generation (<simdisk>/.rockbox/database_gen<N>/, the newest intact
manifest's; the flat <simdisk>/.rockbox/ on an older disk) and writes
database_changelog.txt beside it, with one line per entry (or the
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
Used by tests/perf/import65k.sh to time the changelog import on the 65k
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


DBGEN_MAGIC = 0x47445049      # apps/ipodui/dbgen.c: "IPDG", native endian


def active_db_dir(rockbox):
    """The active database generation's folder (#230): the intact
    database_manifest<0|1>.dat with the higher seq names database_gen<N>
    (apps/ipodui/dbgen.c, struct manifest: magic, version, seq, gen,
    little-endian). The manifest's checksum isn't checked here: a torn
    manifest makes the import fail loudly in the sim. Without manifests,
    the old flat layout."""
    best = None
    for slot in (0, 1):
        try:
            with open(os.path.join(rockbox, 'database_manifest%d.dat'
                                   % slot), 'rb') as f:
                head = f.read(20)
        except OSError:
            continue
        if len(head) < 20:
            continue
        magic, _version, seq, gen = struct.unpack('<IIQI', head)
        if magic != DBGEN_MAGIC:
            continue
        d = os.path.join(rockbox, 'database_gen%d' % gen)
        if os.path.isdir(d) and (best is None or seq > best[0]):
            best = (seq, d)
    if best:
        return best[1]
    if os.path.exists(os.path.join(rockbox, 'database_%d.tcd'
                                   % TAG_FILENAME)):
        return rockbox
    sys.exit('make_changelog: no database in %s (no intact manifest, no '
             'flat database_%d.tcd)' % (rockbox, TAG_FILENAME))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        sys.exit(__doc__)
    db = active_db_dir(os.path.join(args[0], '.rockbox'))
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
