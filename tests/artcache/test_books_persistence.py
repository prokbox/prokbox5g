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

"""Compile actual book persistence functions with real-file fault injection.

This deliberately does not model firmware scheduling or replace simulator/
device tests. Extraction fails loudly if the implementation boundaries move.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
art = (root / "apps/ipodui/artcache.c").read_text()
runtime = (root / "apps/ipodui/ab_book.c").read_text()
functions = art[art.index("static enum artidx_open_result bk_read_exact("):
                art.index("/* -- a book's files,")]
start = art.index("    if (rc == 0)\n    {\n        uint32_t off = 0;")
end = art.index("    core_put_data_pinned(blk);", start)
append = art[start:end]
reader = runtime[runtime.index("static bool idx_hdr("):
                 runtime.index("/* The whole blob into buf")]
loops = art[art.index("/* The chapter ledger (bwork.chskip)"):
            art.index("/* ======================================================================= */\n/* builder: books.pak")]
loops += art[art.index("static int run_books("):
             art.index("/* ======================================================================= */\n/* builder: what the user sees first")]
meta_start = art.index("        memset(&w->id3, 0, sizeof(w->id3));", art.index("static int book_build("))
meta_end = art.index("        m.starts[q] = len;", meta_start)
metadata_gate = art[meta_start:meta_end]
metadata_wrapper = """
static bool test_metadata_gate(struct bwork *w)
{
    struct { struct audiobook_chapters *parse; void *scratch;
             size_t scratch_size; } m;
    uint32_t table[AB_CH_TABLE_SIZE / 4];
    int fd, rc = 0, c = 0;
    m.parse = (struct audiobook_chapters *)table;
    m.scratch = NULL; m.scratch_size = 0;
    do {
""" + metadata_gate + "    } while (0);\n    return rc == 0;\n}\n"
wrapper = """
static int test_append(struct bwork *w, uint64_t key)
{
    unsigned char blob[32] = {0};
    struct { void *blob; } m = { blob };
    struct artalb_rec row = { .key = key }, *t = &row;
    struct ab_idx_rec rec, *old = NULL;
    size_t size = sizeof(blob);
    int fd, rc = 0, n = 1;
""" + append + "    return rc;\n}\n"
with tempfile.TemporaryDirectory(prefix="books-persistence-") as tmp:
    out = Path(tmp)
    (out / "books_actual.inc").write_text(functions + reader + wrapper)
    (out / "books_loops.inc").write_text(loops)
    (out / "books_metadata.inc").write_text(metadata_wrapper)
    cc = os.environ.get("HOSTCC", "cc")
    subprocess.run([cc, "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-DARTFMT_HOST", "-DWARBLE", "-I" + tmp,
                    "-Iapps/ipodui", "-Ilib/rbcodec/metadata",
                    "-Itests/nowplaying", "tests/artcache/test_books_persistence.c",
                    "apps/ipodui/artfmt.c", "apps/ipodui/artkey.c",
                    "apps/ipodui/sortkey.c",
                    "apps/ipodui/ab_book_core.c",
                    "lib/rbcodec/metadata/abpack.c", "-o", str(out / "test")],
                   cwd=root, check=True)
    subprocess.run([str(out / "test"), tmp], check=True)
