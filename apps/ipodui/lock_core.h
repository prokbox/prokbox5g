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
 * iPod UI shell: Screen Lock combination rules, pure (P12 contract, brief
 * 3.4, Q10) Host-tested in tests/lock; owner W1-L
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
#ifndef IPODUI_LOCK_CORE_H
#define IPODUI_LOCK_CORE_H

/* The combination state machine, with no settings, screen or input in
 * it: lock.c keeps a struct lock_state in step with the two settings. */

#include <stdint.h>
#include <stdbool.h>

#define LOCK_CODE_NONE  (-1)
#define LOCK_CODE_MAX   9999            /* 4 digits, roller.h */

struct lock_state {
    int16_t code;                       /* ipodui_lock_code */
    uint8_t locked;                     /* ipodui_locked */
};

/* New Combination, then Confirm Combination */
enum lock_set_step { LOCK_SET_NEW, LOCK_SET_CONFIRM, LOCK_SET_DONE,
                     LOCK_SET_MISMATCH };
struct lock_setter {
    int16_t first;                      /* the code entered at NEW */
    uint8_t step;                       /* enum lock_set_step */
};
void lock_set_begin(struct lock_setter *s);
/* One entered code: NEW -> CONFIRM; CONFIRM with the same code -> DONE
 * (st->code set, not locked); a different code -> MISMATCH, and the next
 * entry starts again at NEW. Returns the new step. */
int  lock_set_enter(struct lock_setter *s, int code, struct lock_state *st);
/* Lock: false (nothing changes) without a combination */
bool lock_engage(struct lock_state *st);
/* The locked screen's entry: true and unlocked on the right code */
bool lock_try(struct lock_state *st, int code);
/* A computer connected for data unlocks (Q10: any computer) */
void lock_usb_host(struct lock_state *st);
/* Reset (Screen Lock's Reset row while unlocked, Reset Settings): no
 * combination, unlocked */
void lock_clear(struct lock_state *st);
/* The two settings as loaded (a hand-edited or damaged config.cfg): a
 * code outside 0..9999 becomes none, and "locked" with no combination
 * becomes unlocked. True if anything changed. (W1-L addition.) */
bool lock_state_fix(struct lock_state *st);

#endif /* IPODUI_LOCK_CORE_H */
