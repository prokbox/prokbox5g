/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * Phase 9 sim commands (docs/phase9-brief.md section 4.4; owner T):
 * alloc_dump, open_count, run_rock, and the language-allocation
 * read-backs lang_set, lang_load, lang_str, lang_alloc_check (Phase 9 end
 * review). See sim_cmd_p9.h.
 *
 * Copyright (C) 2026 prokopto-dev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE            /* dladdr */
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#define P9_HAVE_DLADDR
#endif
#include "config.h"
#include "system.h"
#include "core_alloc.h"
#include "buflib.h"
#ifdef HAVE_IPOD_UI_PLUGINS
#include "plugin.h"
#endif
#include "file.h"
#include "settings.h"
#include "language.h"
#include "lang.h"
#include "string-extra.h"
#include "sim_cmd_p9.h"

extern struct buflib_context core_ctx;

/* uisimulator/common/filesystem-sim.c (HAVE_IPOD_UI); weak so this file
 * also links where that counter isn't built */
int sim_open_count(void) __attribute__((weak));
int sim_open_count(void)
{
    return -1;
}

static const char *sym_name(const void *p, char *buf, size_t n)
{
    if (!p)
        return "none";
#ifdef P9_HAVE_DLADDR
    Dl_info info;
    if (dladdr(p, &info) && info.dli_sname && info.dli_saddr == p)
    {
        snprintf(buf, n, "%s", info.dli_sname);
        return buf;
    }
#endif
    snprintf(buf, n, "%p", p);
    return buf;
}

#if CONFIG_BUFLIB_BACKEND == BUFLIB_BACKEND_MEMPOOL
static void alloc_dump(void)
{
    char name[96], move[96];
    long total = 0, freeb = 0;
    int count = 0;
    /* the walk doesn't yield (sim threads run one at a time), so no other
     * thread can move or free a block under it */
    for (union buflib_data *b = core_ctx.buf_start; b < core_ctx.alloc_end;
         b += labs(b->val))
    {
        long bytes = labs(b->val) * (long)sizeof(union buflib_data);
        if (b->val == 0)
        {
            fprintf(stderr, "p9 alloc corrupt block at %p\n", (void *)b);
            break;
        }
        if (b->val < 0)
        {
            freeb += bytes;
            continue;
        }
        long size = bytes - (long)BUFLIB_ALLOC_OVERHEAD;
        int handle = (int)(core_ctx.handle_table - b[BUFLIB_IDX_HANDLE].handle);
        struct buflib_callbacks *ops = b[BUFLIB_IDX_OPS].ops;
        fprintf(stderr, "p9 alloc name=%s size=%ld handle=%d pins=%u "
                "move=%s\n", sym_name(ops, name, sizeof(name)), size, handle,
                (unsigned)b[BUFLIB_IDX_PIN].pincount,
                sym_name(ops ? (const void *)ops->move_callback : NULL,
                         move, sizeof(move)));
        total += size;
        count++;
    }
    fprintf(stderr, "p9 alloc total=%ld count=%d free=%ld\n", total, count,
            freeb + (long)core_available());
}
#else
static void alloc_dump(void)
{
    fprintf(stderr, "p9 alloc unsupported (buflib backend)\n");
}
#endif


/* the core lang_size of a .lng (subheader 0), as apps/language.c reads it */
static int lng_core_size(const char *path)
{
    unsigned char hdr[10];
    int size = -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (read(fd, hdr, sizeof hdr) == (ssize_t)sizeof hdr)
        size = (hdr[6] << 8) | hdr[7];
    close(fd);
    return size;
}

#if CONFIG_BUFLIB_BACKEND == BUFLIB_BACKEND_MEMPOOL
/* exactly one pinned, callback-less block whose usable size is the file's
 * core size rounded up to buflib's unit: the pinned language block */
static void lang_alloc_check(const char *path)
{
    int want = lng_core_size(path);
    long unit = (long)sizeof(union buflib_data);
    long hi = want + unit - 1;
    int hits = 0, handle = 0;
    long got = 0;

    for (union buflib_data *b = core_ctx.buf_start; b < core_ctx.alloc_end;
         b += labs(b->val))
    {
        if (b->val <= 0)
        {
            if (b->val == 0)
                break;
            continue;
        }
        long size = labs(b->val) * unit - (long)BUFLIB_ALLOC_OVERHEAD;
        if (b[BUFLIB_IDX_OPS].ops == NULL && b[BUFLIB_IDX_PIN].pincount > 0
            && size >= want && size <= hi)
        {
            hits++;
            got = size;
            handle = (int)(core_ctx.handle_table
                           - b[BUFLIB_IDX_HANDLE].handle);
        }
    }
    if (want > 0 && hits == 1)
        fprintf(stderr, "p9 lang alloc ok want=%d size=%ld handle=%d\n",
                want, got, handle);
    else
        fprintf(stderr, "p9 lang alloc want=%d hits=%d\n"
                        "expect_lang failed (alloc)\n", want, hits);
}
#else
static void lang_alloc_check(const char *path)
{
    (void)path;
    fprintf(stderr, "p9 lang alloc unsupported\nexpect_lang failed\n");
}
#endif

bool sim_cmd_p9(const char *cmd, char *argv[], int argc, int lineno)
{
    (void)lineno;
    if (!strcmp(cmd, "alloc_dump"))
    {
        alloc_dump();
        return true;
    }
    if (!strcmp(cmd, "open_count"))
    {
        int n = sim_open_count();
        if (n < 0)
            fprintf(stderr, "p9 opens unsupported\n");
        else
            fprintf(stderr, "p9 opens=%d\n", n);
        return true;
    }
    if (!strcmp(cmd, "run_rock"))
    {
#ifdef HAVE_IPOD_UI_PLUGINS
        if (argc < 2)
        {
            fprintf(stderr, "sim_script: run_rock needs a path\n"
                            "expect_rock failed\n");
            return true;
        }
        int rc = plugin_load(argv[1], argc > 2 ? argv[2] : NULL);
        fprintf(stderr, "p9 rock rc=%d\n", rc);
#else
        (void)argv; (void)argc;
        fprintf(stderr, "p9 rock unsupported\nexpect_rock failed\n");
#endif
        return true;
    }
    if (!strcmp(cmd, "lang_set"))
    {
        /* lang_set <name>|-: the boot-time language (lang_file, saved to
         * config.cfg; "-" clears it); a following "reboot" loads it */
        const char *name = (argc > 1 && strcmp(argv[1], "-")) ? argv[1] : "";
        strmemccpy(global_settings.lang_file, name,
                   sizeof(global_settings.lang_file));
        settings_save();
        fprintf(stderr, "p9 lang set '%s'\n", name);
        return true;
    }
    if (!strcmp(cmd, "lang_load") && argc > 1)
    {
        /* lang_load <path> <rc>: lang_core_load() now, as Settings would */
        int rc = lang_core_load(argv[1]);
        int want = argc > 2 ? atoi(argv[2]) : 0;
        fprintf(stderr, "p9 lang load rc=%d size=%d\n", rc,
                lng_core_size(argv[1]));
        if (rc != want)
            fprintf(stderr, "expect_lang failed (load rc %d, want %d)\n",
                    rc, want);
        return true;
    }
    if (!strcmp(cmd, "lang_str") && argc > 2)
    {
        /* lang_str <english> <expected>: the string the UI draws for it;
         * '_' stands for a space in both */
        for (int a = 1; a <= 2; a++)
            for (char *c = argv[a]; *c; c++)
                if (*c == '_')
                    *c = ' ';
        int id = lang_english_to_id(argv[1]);
        const char *t = id >= 0 ? (const char *)str(id) : "";
        fprintf(stderr, "p9 lang str id=%d '%s' = '%s'\n", id, argv[1], t);
        if (id < 0 || strcmp(t, argv[2]))
            fprintf(stderr, "expect_lang failed (str '%s')\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "lang_alloc_check") && argc > 1)
    {
        lang_alloc_check(argv[1]);
        return true;
    }
    return false;
}
