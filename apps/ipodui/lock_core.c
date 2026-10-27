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
 * iPod UI shell: Screen Lock combination rules, pure (P12, brief 3.4, Q10)
 * Host-tested in tests/lock
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

/* The combination rules, with no settings, screen or input: lock.c keeps
 * a struct lock_state in step with the two settings and calls these. */
#include "ipodui/lock_core.h"

static bool code_ok(int code)
{
    return code >= 0 && code <= LOCK_CODE_MAX;
}

void lock_set_begin(struct lock_setter *s)
{
    s->first = LOCK_CODE_NONE;
    s->step = LOCK_SET_NEW;
}

int lock_set_enter(struct lock_setter *s, int code, struct lock_state *st)
{
    switch (s->step)
    {
        case LOCK_SET_CONFIRM:
            if (code_ok(code) && code == s->first)
            {
                st->code = (int16_t)code;
                st->locked = 0;
                s->step = LOCK_SET_DONE;
            }
            else
            {
                s->first = LOCK_CODE_NONE;
                s->step = LOCK_SET_MISMATCH;
            }
            break;
        case LOCK_SET_DONE:             /* a finished setter: start over */
        case LOCK_SET_NEW:
        case LOCK_SET_MISMATCH:         /* the next entry is a new one */
        default:
            if (!code_ok(code))
            {
                s->first = LOCK_CODE_NONE;
                s->step = LOCK_SET_NEW;
                break;
            }
            s->first = (int16_t)code;
            s->step = LOCK_SET_CONFIRM;
            break;
    }
    return s->step;
}

bool lock_engage(struct lock_state *st)
{
    if (!code_ok(st->code))
        return false;
    st->locked = 1;
    return true;
}

bool lock_try(struct lock_state *st, int code)
{
    if (!st->locked)
        return true;                    /* nothing to open */
    if (!code_ok(code) || code != st->code)
        return false;
    st->locked = 0;
    return true;
}

void lock_usb_host(struct lock_state *st)
{
    st->locked = 0;
}

void lock_clear(struct lock_state *st)
{
    st->code = LOCK_CODE_NONE;
    st->locked = 0;
}

bool lock_state_fix(struct lock_state *st)
{
    bool changed = false;

    if (st->code != LOCK_CODE_NONE && !code_ok(st->code))
    {
        st->code = LOCK_CODE_NONE;
        changed = true;
    }
    if (st->locked > 1 || (st->locked && st->code == LOCK_CODE_NONE))
    {
        /* locked with no combination could never open: never trap the
         * owner behind a code that does not exist */
        st->locked = 0;
        changed = true;
    }
    return changed;
}
