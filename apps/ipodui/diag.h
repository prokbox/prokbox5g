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
 * iPod UI shell: Settings > About > Save Diagnostics (release-plan 6.2)
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
#ifndef _IPODUI_DIAG_H_
#define _IPODUI_DIAG_H_

#include <stdbool.h>

/* The report: one file at the top of the data volume, where a computer
 * shows it; the same name every time, so a new report replaces the last */
#define DIAG_REPORT_NAME  "ProkBox Report.txt"
#define DIAG_REPORT_FILE  "/" DIAG_REPORT_NAME

/* UI thread, on the user's SELECT only (never at USB entry): flushes the
 * log, builds the report in one core_alloc'd block (freed before it
 * returns) and writes it in one write. true: saved */
bool diag_save_report(void);

#endif /* _IPODUI_DIAG_H_ */
