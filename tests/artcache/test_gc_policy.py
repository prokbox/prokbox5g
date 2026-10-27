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

"""#85 B-M4: the artwork GC policy, compiled from apps/ipodui/artcache.c.

Records of albums gone from the table are retained until the index or the
pack passes 3/4 of ARTIDX_MAX_RECS; the GC callback only reads the live
bitmap (no deletion inside the merge, so index.prev keeps working). The
merge and GC marking themselves are covered by test_artfmt.c.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
art = (root / "apps/ipodui/artcache.c").read_text()
start = art.index("#define ART_GC_MARK")
end = art.index("static uint64_t *gc_keys(void *ctx)", start)
policy = art[start:end]
keep = art[art.index("static bool keep_live("):]
keep = keep[:keep.index("\n}\n") + 3]
assert "remove" not in keep and "unlink" not in keep, "GC callback deletes"
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "artfmt.h"
struct bwork { uint32_t live[ARTIDX_MAX_RECS / 32]; };
''' + policy + keep + r'''
int main(void)
{
    static struct bwork w;
    struct artcache_rec r = { 0 };
    uint32_t mark = ARTIDX_MAX_RECS / 4 * 3;
    assert(!art_gc_pressure(0, 0));
    assert(!art_gc_pressure(mark - 1, mark - 1));
    assert(art_gc_pressure(mark, 0) && art_gc_pressure(0, mark));
    assert(art_gc_pressure(ARTIDX_MAX_RECS, ARTIDX_MAX_RECS));
    w.live[1] = 1u << 3;
    assert(keep_live(&w, &r, 35) && !keep_live(&w, &r, 34));
    printf("ok   art GC policy: retained below %u of %u records\n",
           (unsigned)mark, (unsigned)ARTIDX_MAX_RECS);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="art-gc-") as tmp:
    c = Path(tmp) / "gc.c"
    c.write_text(harness)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu99", "-Wall",
                    "-Wextra", "-Werror", "-DARTFMT_HOST", "-Iapps/ipodui",
                    str(c), "-o", str(Path(tmp) / "gc")], cwd=root, check=True)
    subprocess.run([str(Path(tmp) / "gc")], check=True)
