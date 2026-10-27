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
 * Host test for the ARC USB reset decisions (ProkBox, USB resets,
 * firmware/target/arm/usb-arc-reset.h): which reset interrupts tear the
 * session down, and the compact register bytes the device log keeps.
 *
 * What would break, and how it shows:
 * - a reset with the port in reset, or with our address gone, kept as
 *   "spurious": the device would answer the old address while the host
 *   enumerates at 0, so the host loses the device ("real reset kept");
 * - an unaddressed device's reset kept: nothing to lose, but the first
 *   enumeration would run on stale endpoint state;
 * - a URI with PR clear and our address intact torn down: the case this
 *   exists for ("spurious reset torn down");
 * - wrong log bytes: the device log would misreport speed, suspend or
 *   the line state.
 *
 *   cc -I. tests/usb/reset_test.c -o reset_test && ./reset_test
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

#define HS      (2u << ARC_PS_SPD_SHIFT)
#define ADDR(a) ((uint32_t)(a) << ARC_ADDR_SHIFT)

int main(void)
{
    unsigned a;

    /* every address, every PR/address combination */
    for (a = 0; a < 128; a++) {
        /* in reset: always real, whatever the address */
        CHECK(arc_reset_classify(ARC_PS_PR | HS, ADDR(a), a) == ARC_RST_PR,
              "PR set is real");
        CHECK(arc_reset_classify(ARC_PS_PR, 0, a) == ARC_RST_PR,
              "PR set, address cleared, is real");
        if (a == 0) {
            CHECK(arc_reset_classify(HS, 0, 0) == ARC_RST_UNADDR,
                  "unaddressed reset is real");
            continue;
        }
        /* out of reset, the controller cleared our address: real */
        CHECK(arc_reset_classify(HS | ARC_PS_PE, 0, a) == ARC_RST_ADDR0,
              "address cleared is real");
        /* out of reset, a different address than ours: real */
        CHECK(arc_reset_classify(HS, ADDR((a % 127) + 1), a) ==
              ((a % 127) + 1 == a ? ARC_RST_SPURIOUS : ARC_RST_ADDR0),
              "address changed is real");
        /* out of reset, our address intact: the only spurious case */
        CHECK(arc_reset_classify(HS | ARC_PS_PE | ARC_PS_CCS, ADDR(a), a) ==
              ARC_RST_SPURIOUS, "spurious reset torn down");
        CHECK(arc_reset_classify(HS | ARC_PS_SUSP, ADDR(a), a) ==
              ARC_RST_SPURIOUS, "suspend with address intact kept");
        /* other DEVICEADDR bits (24: ADRA) do not count as the address */
        CHECK(arc_reset_classify(HS, ADDR(a) | (1u << 24), a) ==
              ARC_RST_SPURIOUS, "ADRA bit ignored");
    }
    CHECK(arc_reset_is_real(ARC_RST_PR) && arc_reset_is_real(ARC_RST_ADDR0) &&
          arc_reset_is_real(ARC_RST_UNADDR) &&
          !arc_reset_is_real(ARC_RST_SPURIOUS), "is_real");

    /* log bytes */
    CHECK(arc_portsc_byte(0) == 0x00, "portsc zero");
    CHECK(arc_portsc_byte(ARC_PS_PR) == 0x01, "portsc PR");
    CHECK(arc_portsc_byte(ARC_PS_SUSP) == 0x02, "portsc SUSP");
    CHECK(arc_portsc_byte(ARC_PS_PE) == 0x04, "portsc PE");
    CHECK(arc_portsc_byte(ARC_PS_CCS) == 0x08, "portsc CCS");
    CHECK(arc_portsc_byte(1u << ARC_PS_LS_SHIFT) == 0x10, "portsc J");
    CHECK(arc_portsc_byte(2u << ARC_PS_LS_SHIFT) == 0x20, "portsc K");
    CHECK(arc_portsc_byte(HS) == 0x80, "portsc high speed");
    CHECK(arc_portsc_byte(0x0c001d8du) == 0xff, "portsc all");
    CHECK(arc_ep_byte(0x00020003u) == 0x23, "ep byte");
    CHECK(arc_ep_byte(0xfff0fff0u) == 0x00 && arc_ep_byte(~0u) == 0xff,
          "ep byte: endpoints 4+ dropped");

    CHECK(arc_gap_bucket(0) == 0 && arc_gap_bucket(99) == 0, "gap <100");
    CHECK(arc_gap_bucket(100) == 1 && arc_gap_bucket(999) == 1, "gap <1s");
    CHECK(arc_gap_bucket(1000) == 2 && arc_gap_bucket(9999) == 2, "gap <10s");
    CHECK(arc_gap_bucket(10000) == 3 && arc_gap_bucket(~0u) == 3, "gap more");

    printf("reset_test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
