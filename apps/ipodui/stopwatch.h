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
 * iPod UI shell: the resident half of the Stopwatch (P12, docs/phase12-brief.md
 * 3.4): the running timing, stopwatch.dat, the panel; owner W1-S
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
#ifndef IPODUI_STOPWATCH_H
#define IPODUI_STOPWATCH_H

/* The Stopwatch screens are a cold module (screens/stopwatch.c); what
 * must outlive them is here, resident: one struct sw_state (a timing runs
 * while its screen is closed, Q18: it never keeps the unit awake, the RTC
 * carries it through sleep and power-off) and the stopwatch.dat writer. UI thread only, except ui_sw_poweroff(). */

#include <stdbool.h>
#include <stdint.h>
#include "ipodui/stopwatch_core.h"

struct ui_preview_ops;
/* The Stopwatch panel (the stopwatch icon, screens/panels.c): Extras row
 * and the Stopwatch screen's own split list. */
extern const struct ui_preview_ops ui_preview_x_stopwatch;

/* A timing exists: running, paused, or with laps (the list then offers
 * Resume) */
bool ui_sw_active(void);
bool ui_sw_running(void);
/* Elapsed ms now. 'fresh' reads the RTC (a redraw after a wake); else the
 * RTC is read at most once a second and the ticks carry the rest. A tick
 * count that fell behind the RTC (a sleep) is re-anchored here, so the
 * tenths keep running. */
uint32_t ui_sw_now_ms(bool fresh);
const struct sw_state *ui_sw_state(void);

/* A log's date: y, m 1..12, d, h, min from the seconds a log holds */
void ui_sw_split(uint32_t rtc, int *year, int *mon, int *mday, int *hour,
                 int *min);

/* PLAY: start or resume / stop (the stop writes the log into 'f' and
 * stopwatch.dat). 'f' may be NULL: the log is then not written. */
void ui_sw_toggle(struct sw_logs_file *f);
/* SELECT while running: a lap */
void ui_sw_lap(void);
/* New Timer: a running timing is stopped and logged, then all zero */
void ui_sw_new(struct sw_logs_file *f);

/* stopwatch.dat into f (empty on none, corrupt or short); returns the
 * count */
int  ui_sw_logs_load(struct sw_logs_file *f);
/* Clear Logs: f emptied and written */
void ui_sw_logs_clear(struct sw_logs_file *f);

/* SYS_POWEROFF: a running timing is stopped and logged (one write inside
 * clean_shutdown()'s window). Needs the hook line in input.c. */
void ui_sw_poweroff(void);

#endif /* IPODUI_STOPWATCH_H */
