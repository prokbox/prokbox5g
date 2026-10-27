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
 * Host test of the Screen Lock combination rules, apps/ipodui/lock_core.c
 * (P12, brief 3.4, Q10). W0 skeleton: the cases are the contract; W1-L adds
 * more
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
#include <string.h>
#include "ipodui/lock_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    struct lock_state st, saved;
    struct lock_setter s;

    lock_clear(&st);
    CHECK(st.code == LOCK_CODE_NONE && !st.locked);
    /* no combination: Lock does nothing */
    CHECK(!lock_engage(&st) && !st.locked);

    /* new, then a confirm mismatch: start again at New */
    lock_set_begin(&s);
    CHECK(s.step == LOCK_SET_NEW);
    CHECK(lock_set_enter(&s, 1234, &st) == LOCK_SET_CONFIRM);
    CHECK(lock_set_enter(&s, 1243, &st) == LOCK_SET_MISMATCH);
    CHECK(st.code == LOCK_CODE_NONE);
    CHECK(lock_set_enter(&s, 4321, &st) == LOCK_SET_CONFIRM);
    CHECK(lock_set_enter(&s, 4321, &st) == LOCK_SET_DONE);
    CHECK(st.code == 4321 && !st.locked);
    /* 0000 is a valid combination */
    lock_set_begin(&s);
    lock_set_enter(&s, 0, &st);
    CHECK(lock_set_enter(&s, 0, &st) == LOCK_SET_DONE && st.code == 0);
    lock_set_begin(&s);
    lock_set_enter(&s, 4321, &st);
    lock_set_enter(&s, 4321, &st);

    /* lock, a wrong code, the right code */
    CHECK(lock_engage(&st) && st.locked);
    CHECK(!lock_try(&st, 1111) && st.locked);
    CHECK(lock_try(&st, 4321) && !st.locked);

    /* a data host unlocks; the combination stays */
    lock_engage(&st);
    lock_usb_host(&st);
    CHECK(!st.locked && st.code == 4321);

    /* persisted state: what the two settings hold comes back as it was */
    lock_engage(&st);
    saved = st;
    memset(&st, 0, sizeof(st));
    st = saved;
    CHECK(st.locked && !lock_try(&st, 1234) && lock_try(&st, 4321));

    /* Reset forgets it */
    lock_clear(&st);
    CHECK(st.code == LOCK_CODE_NONE && !st.locked && !lock_engage(&st));

    /* W1-L: codes outside 0..9999 are never a combination */
    lock_set_begin(&s);
    CHECK(lock_set_enter(&s, 10000, &st) == LOCK_SET_NEW);
    CHECK(lock_set_enter(&s, -1, &st) == LOCK_SET_NEW);
    CHECK(lock_set_enter(&s, 9999, &st) == LOCK_SET_CONFIRM);
    CHECK(lock_set_enter(&s, 9999, &st) == LOCK_SET_DONE && st.code == 9999);
    lock_engage(&st);
    CHECK(!lock_try(&st, -1) && !lock_try(&st, 19999) && st.locked);
    /* every wrong code leaves it locked; the right one still opens */
    {
        int c, opened = 0;
        for (c = 0; c < 9999; c++)
            opened += lock_try(&st, c);
        CHECK(opened == 0 && st.locked && lock_try(&st, 9999));
    }
    /* a mismatch forgets the first entry: confirming it later fails */
    lock_clear(&st);
    lock_set_begin(&s);
    lock_set_enter(&s, 1111, &st);
    CHECK(lock_set_enter(&s, 2222, &st) == LOCK_SET_MISMATCH);
    CHECK(lock_set_enter(&s, 2222, &st) == LOCK_SET_CONFIRM);
    CHECK(lock_set_enter(&s, 1111, &st) == LOCK_SET_MISMATCH);
    CHECK(st.code == LOCK_CODE_NONE);
    /* a data host on an unlocked unit changes nothing */
    st.code = 1234; st.locked = 0;
    lock_usb_host(&st);
    CHECK(st.code == 1234 && !st.locked);

    /* damaged settings never trap the owner (lock_state_fix) */
    st.code = LOCK_CODE_NONE; st.locked = 1;
    CHECK(lock_state_fix(&st) && !st.locked);
    st.code = 12345; st.locked = 1;
    CHECK(lock_state_fix(&st) && st.code == LOCK_CODE_NONE && !st.locked);
    st.code = -7; st.locked = 0;
    CHECK(lock_state_fix(&st) && st.code == LOCK_CODE_NONE);
    st.code = 42; st.locked = 1;
    CHECK(!lock_state_fix(&st) && st.code == 42 && st.locked);
    st.code = 42; st.locked = 3;
    CHECK(lock_state_fix(&st) && !st.locked);
    if (fails)
        return 1;
    printf("ok   screen lock rules (new, confirm mismatch, lock, wrong, "
           "right, USB unlock, persisted, reset, range, damaged settings)\n");
    return 0;
}
