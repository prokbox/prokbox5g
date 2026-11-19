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

/* tests/modclass: the module class accountant (apps/ipodui/modclass.c)
 * against the devstats counters it reads.  p090-notescjk: a CJK
 * companion's glyph-record reads (devstats font_cmds, booked by
 * firmware/font.c) are the shell's font work and must not count against
 * a module's entry budget of 2; a module's own third read in the entry
 * window must still breach. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "devstats.h"
#include "ipodui/modload.h"
#include "ipodui/modclass.h"

struct devstats devstats;
long current_tick = 1000;
static unsigned int self = 3;           /* the UI thread's slot */
static char last_log[256];
static int viols;
static int checks, fails;

#define CHECK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, what, last_log); } } while (0)

unsigned int thread_self(void) { return self; }
void cpu_boost(int on) { (void)on; }
void ui_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    char line[200];

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    snprintf(last_log, sizeof(last_log), "%s: %s", tag, line);
    if (!strncmp(line, "viol ", 5))
        viols++;
}
static const struct mod_rt rt_a = { MOD_CLASS_A };
const struct mod_rt *mod_rt_get(enum mod_id id) { (void)id; return &rt_a; }
const char *mod_name(int id) { (void)id; return "notes"; }
const void *mod_export_now(enum mod_id id, int slot) { (void)id; (void)slot; return NULL; }

/* storage commands on the UI thread: n of the module's, g of the font
 * layer's glyph-record reads (devstats_font_book() books those in both
 * cmds and font_cmds) */
static void io(unsigned n, unsigned g)
{
    devstats.t[self].cmds += n + g;
    devstats.t[self].font_cmds += g;
}

static void visit(unsigned own, unsigned glyphs)
{
    viols = 0;
    last_log[0] = 0;
    modclass_top(-1);
    current_tick += 10 * HZ;
    modclass_top(MOD_NOTES);            /* the visit and its entry window */
    io(own, glyphs);                    /* the root's reads, its first page */
    current_tick += HZ / 10;            /* inside the window (1 s) */
    modclass_pass();
    current_tick += 3 * HZ;             /* a period later: the report */
    modclass_pass();
    modclass_top(-1);
}

int main(void)
{
    /* the budget: two reads in the entry window, Class A */
    visit(2, 0);
    CHECK(viols == 0, "two own reads: within the entry budget");
    /* a CJK page: two own reads and six glyph records of the companions
     * (the Unicode note: kanji not yet in the glyph cache) */
    visit(2, 6);
    CHECK(viols == 0, "glyph-record reads leak into the module's count");
    /* the module's own third read in the window still breaches */
    visit(3, 0);
    CHECK(viols == 1 && strstr(last_log, "viol notes reads"),
          "a module's own read over budget does not breach");
    /* and with glyph records beside it, still exactly its own count */
    visit(3, 6);
    CHECK(viols == 1 && strstr(last_log, "entry=3 "),
          "own reads over budget among glyph reads: breach with the module's count");
    /* another thread's commands are not the module's (the accountant's) */
    viols = 0;
    modclass_top(-1);
    current_tick += 10 * HZ;
    modclass_top(MOD_NOTES);
    self = 5;
    io(4, 0);
    self = 3;
    current_tick += HZ / 10;
    modclass_pass();
    modclass_top(-1);
    CHECK(viols == 0, "another thread's commands counted as the module's");
    printf("modclass: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
