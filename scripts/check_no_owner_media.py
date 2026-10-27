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

"""Commit guard: no data copied from the owner's iPod may be tracked.

Owner rules, 2026-10-26: no copyrighted content or private data from the
owner's iPod in the repo, even privately; art caches and database files are
never committed unless synthetic.

Fails if git tracks any of
  *.tcd, database_*          tag-cache / database files
  *.pak, art cache files     (artcache*.dat etc.), outside the whitelist below
  *.log, *.log.N, bench/*.txt   device logs and bench results, outside
                             tests/bench_ingest/fixtures/
  config.cfg                 outside packaging/ and tests/ui/fixtures/

usage: scripts/check_no_owner_media.py [--list FILE]
  --list FILE  check the newline-separated paths in FILE instead of `git ls-files`
"""
import os, re, subprocess, sys

# Synthetic files, listed one by one.  Generators: none is committed; each was
# written by hand for its test script and is plainly synthetic.
ALLOWED = {
    # header "IAPN" with the 0xdeadbeef marker and zero fill: a torn art-panel
    # pack for tests/ui/ipodui-p17-panels-inplace.script
    'tests/ui/fixtures/panels-torn.pak',
    # round-number voltage table "batcal 1" (3600, 3700 ...) and a smooth
    # synthetic discharge curve for tests/ui/ipodui-batcal-dev.script
    'tests/ui/fixtures/batcal/batcal.dat',
    'tests/ui/fixtures/batcal/batcal.run',
    # a real bench trace, reviewed 2026-10-26: chip timing and register words
    # only (no file names, tags, paths or media).  Read by
    # bcmfw/tests/silicon_test.py; written by the BCM bench (devbench.c).
    'tests/silicon/20261025-799ff0f8d7/stage4.log',
    # hand-written synthetic previous-boot log (invented titles and paths)
    # for tests/ui/ipodui-diag-report.script: proves the scrubber hides them
    'tests/ui/fixtures/diag/prev.log',
}
SYNTH_DIRS = ('tests/bench_ingest/fixtures/',)            # synthetic bench logs (test_ingest.py)
CFG_DIRS = ('packaging/', 'tests/ui/fixtures/')
ART = re.compile(r'^(artcache|artalb|artidx|artbooks|artpages)[\w.-]*\.(dat|bin|idx|db|tmp|art|pak)$', re.I)


def violations(paths):
    bad = []
    for p in paths:
        if p in ALLOWED:
            continue
        base = os.path.basename(p)
        why = None
        if base.endswith('.tcd') or base.startswith('database_'):
            why = 'tag-cache / database file'
        elif base.endswith('.pak') or ART.match(base):
            why = 'art cache file'
        elif (re.search(r'\.log(\.\d+)?$', base) or re.search(r'(^|/)bench/[^/]*\.txt$', p)) \
                and not p.startswith(SYNTH_DIRS):
            why = 'device log or bench result'
        elif base == 'config.cfg' and not p.startswith(CFG_DIRS):
            why = 'settings file'
        if why:
            bad.append((p, why))
    return bad


def main(argv):
    if len(argv) == 3 and argv[1] == '--list':
        paths = [l.strip() for l in open(argv[2]) if l.strip()]
    else:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        paths = [f for f in subprocess.run(['git', '-C', root, 'ls-files', '-z'],
                                           capture_output=True, text=True).stdout.split('\0') if f]
    bad = violations(paths)
    for p, why in bad:
        print('OWNER MEDIA: %s (%s)' % (p, why))
    if bad:
        print('check_no_owner_media: %d tracked file(s) may come from a device; '
              'remove them, or whitelist a synthetic one in ALLOWED with its generator' % len(bad))
        return 1
    print('check_no_owner_media: %d tracked files, none from a device' % len(paths))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
