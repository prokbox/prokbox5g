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

"""Compare two release zips file by file (docs/release-plan.md section 3.3).

usage: scripts/release_compare.py A.zip B.zip
       scripts/release_compare.py DIR_A DIR_B     (every ProkBox5g-*.zip by name)

Zip containers differ between builds (timestamps, order), so this compares
what is inside, opening nested zips too, and ignores the two "Host" lines
of rockbox-info.txt: the same file names with the same SHA-256 each. Both sides
must come from scripts/release_build.sh in the release toolchain container
(ci/toolchain/Dockerfile): GitHub's draft, and the local build on the
owner's Linux host. Exit 0 when
every pair matches, 1 on any difference, 2 on a usage error.
"""
import hashlib, io, os, re, sys, zipfile

# rockbox-info.txt names the machine that built it; nothing on the device
# reads these lines, and a Linux and a Mac build differ only there.
HOST_LINE = re.compile(rb'^Host (gcc|system):.*$', re.M)


def digests(src, prefix=''):
    """name -> SHA-256 of every file; nested zips are opened, not hashed."""
    out = {}
    with zipfile.ZipFile(src) as z:
        for i in z.infolist():
            if i.is_dir():
                continue
            data = z.read(i)
            name = prefix + i.filename
            if name.endswith('.zip'):
                out.update(digests(io.BytesIO(data), name + '!'))
                continue
            if name.endswith('rockbox-info.txt'):
                data = HOST_LINE.sub(b'', data)
            out[name] = hashlib.sha256(data).hexdigest()
    return out


def compare(a, b):
    da, db = digests(a), digests(b)
    bad = 0
    for name in sorted(set(da) | set(db)):
        if name not in da or name not in db:
            print(f"  only in {'B' if name in db else 'A'}: {name}")
            bad += 1
        elif da[name] != db[name]:
            print(f"  differs: {name}")
            bad += 1
    print(f"{'MATCH' if not bad else 'DIFFER'}: {os.path.basename(a)} "
          f"({len(da)} files) vs {os.path.basename(b)} ({len(db)} files)"
          + (f", {bad} difference(s)" if bad else ""))
    return bad == 0


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    a, b = argv[1], argv[2]
    if os.path.isdir(a) and os.path.isdir(b):
        names = sorted(n for n in os.listdir(a) if n.startswith('ProkBox5g-') and n.endswith('.zip'))
        if not names:
            print(f"no ProkBox5g-*.zip in {a}", file=sys.stderr)
            return 2
        ok = True
        for n in names:
            if not os.path.exists(os.path.join(b, n)):
                print(f"DIFFER: {n} missing from {b}")
                ok = False
            else:
                ok = compare(os.path.join(a, n), os.path.join(b, n)) and ok
        return 0 if ok else 1
    return 0 if compare(a, b) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
