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

#ifndef BCMFW_JSTAGE_H
#define BCMFW_JSTAGE_H

/* The staged upload's bookkeeping (jobs.h rev 0.5, candidate; jobs.md
 * section 3, "The staged upload"): one definition for the BCM image
 * (video/jobs_bcm.c) and the host test's chip (tests/bcmjob), as jref is
 * for the jobs themselves.  Plain C over struct jref; the window, its
 * copy and the SDRAM bring-up are the caller's.
 *
 * The window holds one upload at a time: the last staged ALLOC's slot
 * (*owner, JSTAGE_NONE for none).  The caller clears the owner whenever
 * the epoch moves (RESET, a mode change) and at the slot's FREE. */

#include <stdint.h>
#include "hostproto.h"
#include "jref.h"

#define JSTAGE_NONE     0xFF

/* After jref_exec ran an ALLOC: 1 if its reply should point at the
 * window (the PP asked, it succeeded with a reply, a miss, and the slot
 * fits).  The caller then brings the window up and calls jstage_give. */
static inline int jstage_wanted(const struct jref *j, const uint32_t *a,
                                uint32_t n, uint32_t st, const uint32_t *rep,
                                uint32_t nrep, uint32_t win_bytes)
{
    /* one test at a time: smaller code on the BCM than one && chain */
    if (st != HP_OK || n < 6 || nrep < 5)
        return 0;
    if (!(a[5] & J_ALLOC_F_STAGE) || (rep[4] & J_ALLOC_HIT))
        return 0;
    return j->slot[rep[1] & (JREF_SLOTS - 1)].bytes <= win_bytes;
}

/* The reply then names the window (host_addr) and the slot owns it */
static inline void jstage_give(uint32_t *owner, uint32_t *rep, uint32_t host_addr)
{
    rep[2] = host_addr;
    rep[4] |= J_ALLOC_STAGED;
    *owner = rep[1];
}

/* Before jref_exec runs a COMMIT of slot: the slot's pixels if its
 * upload is in the window (the owner, still ALLOCATED or READY in this
 * epoch), and the window is free again; NULL if the slot holds its own
 * pixels (J_HOW_DIRECT).  *bytes: what to move, whole 32-byte units
 * (the pools hand slots out in them) */
static inline uint16_t *jstage_take(struct jref *j, uint32_t *owner,
                                    uint32_t slot, uint32_t *bytes)
{
    uint16_t *px;

    if (slot != *owner)
        return 0;
    *owner = JSTAGE_NONE;
    px = jref_slot_px(j, slot);
    if (px)
        *bytes = (j->slot[slot].bytes + 31) & ~31u;
    return px;
}

/* After jref_exec ran a COMMIT of slot: its reply's rev 0.5 words.  A
 * reply on HP_OK gets how after A and B; J_E_SUM gets J_SUMREP_* (the
 * slot's sums, its first word, how, and win's B when the pixels came
 * through the window win: the same w x h at the slot's pitch).  A slot
 * whose sum failed stays ALLOCATED, so its surface is still the slot's */
static inline void jstage_reply(struct jref *j, uint32_t slot, uint32_t st,
                                uint32_t how, const uint16_t *win,
                                uint32_t *rep, uint32_t *nrep)
{
    const struct jref_surf *sp;

    if (st == HP_OK && *nrep >= 3)
    {
        rep[3] = how;
        *nrep = 4;
        return;
    }
    if (st != J_E_SUM || slot >= JREF_SLOTS)
        return;
    sp = &j->slot[slot].surf;
    jref_f16w(sp, &rep[J_SUMREP_A], &rep[J_SUMREP_B]);
    rep[J_SUMREP_W0] = *(const uint32_t *)sp->px;  /* slots: 32-byte aligned */
    rep[J_SUMREP_HOW] = how;
    rep[J_SUMREP_WIN_B] = 0;
    if (how != J_HOW_DIRECT && win)
    {
        struct jref_surf s = *sp;
        uint32_t a;

        s.px = (uint16_t *)win;
        jref_f16w(&s, &a, &rep[J_SUMREP_WIN_B]);
    }
    *nrep = J_SUMREP_N;
}

#endif /* BCMFW_JSTAGE_H */
