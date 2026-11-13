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

/* The fitted cell: the stored answer applied, the first-start question
 * (battcell.h) */
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include "system.h"
#include "settings.h"
#include "powermgmt.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "ipodui/anim.h"
#include "ipodui/uilog.h"
#include "ipodui/alarm.h"
#include "ipodui/batcal.h"
#include "ipodui/battcurve.h"
#include "ipodui/battcell.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/screens.h"

#ifdef SIMULATOR
extern const char *sim_script_file;     /* sim_script.c */
#endif

extern struct battery_tables_t device_battery_tables;  /* powermgmt.c */

static uint16_t base_row[BATTCURVE_POINTS];  /* the storage's row */

static const char *const cell_name[BATTCELL_COUNT] = {
    "unset", "stock", "2000", "3000"
};

/* the unit's RAM, read at start-up (crt0-pp.S; the simulator's
 * IPODUI_SIM_RAM=32|64, system-sdl.c): one image for both units */
static int ram_mb(void)
{
    return probed_ramsize;
}

/* The answer this boot works with. A scripted simulator run counts as
 * answered "3,000 mAh" unless its script asks for the question
 * (battcell.h). */
static int stored_cell(void)
{
    int c = global_settings.ipodui_battery_cell;

#ifdef SIMULATOR
    if (sim_script_file && !getenv("IPODUI_SIM_BATTQ"))
        return BATTCELL_3000;
#endif
    return c > BATTCELL_UNSET && c < BATTCELL_COUNT ? c : BATTCELL_UNSET;
}

/* capacity and curve for 'cell'; discard: batcal drops its learned
 * table (the answer changed) */
static void apply(int cell, bool discard)
{
    int ram = ram_mb();
    int mah = battcurve_mah(cell, ram);
    uint16_t t[BATTCURVE_POINTS];

    global_settings.battery_capacity = mah;     /* settings_apply() keeps it */
    set_battery_capacity(mah);
    battcurve_scale(base_row, battcurve_sag(cell, ram), t);
    batcal_seed(t, discard);
}

void battcell_boot_apply(void)
{
#ifdef SIMULATOR
    const char *e = getenv("IPODUI_SIM_BATTQ");

    /* "clear": the run's first boot only (shots.sh names the parts after
     * a "reboot" .part-1.script, .part-2.script, ...) */
    if (sim_script_file && e && !strcmp(e, "clear")
        && (!strstr(sim_script_file, "/.part-")
            || strstr(sim_script_file, "/.part-0.")))
    {
        global_settings.ipodui_battery_cell = BATTCELL_UNSET;
        fe_settings_save();
    }
#endif
    /* the row powermgmt_storage_init() set; the levels file is not
     * loaded yet (init_battery_tables() comes next) */
    memcpy(base_row, device_battery_tables.discharge, sizeof(base_row));
    apply(stored_cell(), false);
}

/* the cell and the table in use, read back (0, 10, 50, 90, 100 % and
 * disk-safe); tag "boot" at shell start, "battq" after an answer */
static void log_cell(const char *tag)
{
    const unsigned short *d = device_battery_tables.discharge;

    ui_log(tag, "unit battery cell=%s mah=%d ram=%d",
           cell_name[stored_cell()], get_battery_capacity(), ram_mb());
    ui_log(tag, "battery curve %u %u %u %u %u %u", d[0], d[1], d[5],
           d[9], d[10], *device_battery_tables.disksafe);
}

void battcell_log(void)
{
    log_cell("boot");
}

void battcell_ask(void)
{
    if (stored_cell() != BATTCELL_UNSET)
        return;
#ifdef SIMULATOR
    if (sim_script_file && !getenv("IPODUI_SIM_BATTQ"))
        return;
#endif
    if (alarm_ringing())
        return;                         /* the ring first; next boot then */
    if (ui_push_t(&ui_battq_screen, NULL, UI_T_SYSTEM) < 0)
        return;
    (void)ui_take_transition();         /* up at once, no slide */
    ui_invalidate(UI_DIRTY_ALL);
    UPDATE_TRACE("battq shown%s", "");
}

bool battcell_question_up(void)
{
    struct ui_screen *top = ui_top();

    return top && top->ops == &ui_battq_screen;
}

void battcell_answer(int cell)
{
    int old = global_settings.ipodui_battery_cell;

    if (cell <= BATTCELL_UNSET || cell >= BATTCELL_COUNT)
        return;
    global_settings.ipodui_battery_cell = cell;
    /* the first answer keeps a learned table (it is this unit's own);
     * a changed one means a different cell: learn again */
    apply(cell, old > BATTCELL_UNSET && old != cell);
    fe_settings_save();                 /* with the next idle write */
    UPDATE_TRACE("battq answered cell=%s", cell_name[cell]);
    log_cell("battq");
}
