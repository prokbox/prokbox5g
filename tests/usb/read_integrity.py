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

"""Read-only macOS USB integrity probe.

Use only after a volume is mounted.  Each pass opens the file again, asks
Darwin not to retain its file-cache pages, then hashes it at a chosen read
size.  It never writes, mounts, unmounts, or ejects anything.

    tests/usb/read_integrity.py /private/tmp/ipod_mount/path/to/file \
        --sha256 <known-source-sha256>

The different read sizes distinguish a media/layout problem from a USB
double-buffer or DMA handoff bug.  ``--sha256`` is required: a successful
read without an independent source digest is not an integrity result.
"""
import argparse
import fcntl
import hashlib
import os
import sys


# Darwin <fcntl.h>: request no caching of this descriptor's file data.
F_NOCACHE = 48
SIZES = (4 * 1024, 64 * 1024, 128 * 1024, 1024 * 1024)


def digest_uncached(path, size):
    fd = os.open(path, os.O_RDONLY)
    try:
        if sys.platform != "darwin":
            raise RuntimeError("this probe needs macOS F_NOCACHE")
        fcntl.fcntl(fd, F_NOCACHE, 1)
        h = hashlib.sha256()
        total = 0
        while True:
            block = os.read(fd, size)
            if not block:
                break
            h.update(block)
            total += len(block)
        return h.hexdigest(), total
    finally:
        os.close(fd)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("path", help="mounted iPod file to read; never modified")
    p.add_argument("--sha256", required=True,
                   help="known SHA-256 of the independent source file")
    args = p.parse_args()
    expected = args.sha256.lower()
    if len(expected) != 64 or any(c not in "0123456789abcdef" for c in expected):
        p.error("--sha256 must be a 64-character hexadecimal digest")

    failed = False
    for size in SIZES:
        got, total = digest_uncached(args.path, size)
        ok = got == expected
        print("%7d KiB  %10d bytes  %s  %s" %
              (size // 1024, total, "OK  " if ok else "FAIL", got))
        failed |= not ok
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
