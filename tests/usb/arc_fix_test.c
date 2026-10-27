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
 * Host test for the ARC driver's pure decisions (usb-arc-reset.h): the
 * ENDPTCOMPLETE bits cleared, dTD error bits, the dTD count a transfer
 * needs.
 *
 * What would break, and how it shows:
 * - a bit cleared for a pipe whose dTD is still active: that completion
 *   is lost and the class driver waits for ever (CSW never sent);
 * - an error bit missed: a halted endpoint reports success;
 * - a length past the endpoint's dTDs accepted: the chain runs into the
 *   next endpoint's dTDs (stale data on the wire).
 * (The TXFILLTUNING/TXFIFOTHRES A/B and its check went with 2905708714.)
 *
 *   cc -Wall -Wextra -I. tests/usb/arc_fix_test.c -o arc_fix_test
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
#include <stdio.h>
#include "firmware/target/arm/usb-arc-reset.h"

static int fails;

#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

int main(void)
{
    uint32_t seen, unf;

    /* every subset of the 10 pipes: only finished pipes are cleared, and
     * the unfinished keep their bit */
    for (seen = 0; seen < 0x400; seen++)
        for (unf = 0; unf < 0x400; unf += 7) {
            uint32_t clr = arc_complete_clear(seen, unf & seen);
            CHECK((clr & (unf & seen)) == 0, "unfinished bit cleared");
            CHECK((seen & ~clr) == (unf & seen), "finished bit kept");
        }
    CHECK(arc_complete_clear(0x30001, 0x10000) == 0x20001, "one skipped");
    CHECK(arc_complete_clear(0x3, 0) == 0x3, "none skipped");

    CHECK(arc_td_error(0x80) == 0, "active is no error");
    CHECK(arc_td_error(0x00) == 0, "idle is no error");
    CHECK(arc_td_error(0x40) == ARC_TD_HALTED, "halted");
    CHECK(arc_td_error(0x20) == ARC_TD_BUF_ERR, "data buffer");
    CHECK(arc_td_error(0x08) == ARC_TD_TXN_ERR, "transaction");
    CHECK(arc_td_error(0x68 | 0x10) == 0x68, "all three, other bits off");
    CHECK(arc_td_error(0x8000 | 0x1f0000) == 0, "ioc and length no error");

    CHECK(arc_tds_needed(0, 16384, 4) == 1, "zero length is one dTD");
    CHECK(arc_tds_needed(1, 16384, 4) == 1, "1 byte");
    CHECK(arc_tds_needed(16384, 16384, 4) == 1, "16 KB");
    CHECK(arc_tds_needed(16385, 16384, 4) == 2, "16 KB + 1");
    CHECK(arc_tds_needed(65536, 16384, 4) == 4, "64 KB fits");
    CHECK(arc_tds_needed(65537, 16384, 4) == -1, "64 KB + 1 rejected");
    CHECK(arc_tds_needed(1 << 20, 16384, 4) == -1, "1 MB rejected");
    CHECK(arc_tds_needed(-1, 16384, 4) == -1, "negative rejected");

    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("arc_fix_test: ok\n");
    return 0;
}
