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
#include <stddef.h>
#include "panic_record.h"

/* Runs with interrupts off from a panic, so no library calls: plain loops,
 * and every store goes straight to the (uncached) record. */

static uint32_t rec_sum(const struct panic_record *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t s = 0x9e3779b9u;
    unsigned int n = offsetof(struct panic_record, sum) / 4;

    while (n--)
        s = ((s << 5) | (s >> 27)) + *w++;
    return s;
}

static bool rec_valid(const struct panic_record *r)
{
    return r->magic == PANIC_REC_MAGIC && r->sum == rec_sum(r);
}

/* the sum as it was sealed, whatever the magic says now */
static uint32_t rec_sum_sealed(const struct panic_record *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t s = 0x9e3779b9u;
    unsigned int n = offsetof(struct panic_record, sum) / 4;

    s = ((s << 5) | (s >> 27)) + PANIC_REC_MAGIC;   /* word 0: magic */
    for (w++, n--; n; n--)
        s = ((s << 5) | (s >> 27)) + *w++;
    return s;
}

static void __attribute__((noinline)) copy_str(char *dst, const char *src,
                                               unsigned int size)
{
    unsigned int i = 0;

    if (src)
        for (; i < size - 1 && src[i]; i++)
            dst[i] = src[i];
    for (; i < size; i++)           /* pad: the sum covers every byte */
        dst[i] = '\0';
}

void panic_record_write(struct panic_record *r, uint32_t kind, uint32_t pc,
                        uint32_t lr, uint32_t sp, const char *thread,
                        const char *msg)
{
    uint32_t count = rec_valid(r) ? r->count + 1 : 1;

    r->magic = 0;                   /* invalid until sealed */
    r->kind = kind;
    r->count = count;
    r->pc = pc;
    r->lr = lr;
    r->sp = sp;
    copy_str(r->thread, thread, PANIC_REC_THREAD);
    copy_str(r->msg, msg, PANIC_REC_MSG);
    r->uptime = 0;
    for (unsigned int i = 0; i < PANIC_REC_TRACE; i++)
        r->trace[i] = 0;
    copy_str(r->tail, NULL, PANIC_REC_TAIL);
    copy_str(r->info, NULL, PANIC_REC_INFO);
    r->magic = PANIC_REC_MAGIC;
    r->sum = rec_sum(r);
}

void panic_record_seal(struct panic_record *r)
{
    r->magic = PANIC_REC_MAGIC;
    r->sum = rec_sum(r);
}

const struct panic_record *panic_record_peek(const struct panic_record *r)
{
    if ((r->magic != PANIC_REC_MAGIC && r->magic != PANIC_REC_USED)
        || r->sum != rec_sum_sealed(r))
        return NULL;
    return r;
}

const struct panic_record *panic_record_take(struct panic_record *r)
{
    if (!rec_valid(r))
        return NULL;
    r->magic = PANIC_REC_USED;      /* the sum no longer matches either */
    return r;
}

bool panic_record_pending(const struct panic_record *r)
{
    return rec_valid(r);
}

/* An idle (or peer-waiting, see below) thread is never late: idle restarts
 * the clock, so the limit counts from the moment it went back to work */
bool hang_overdue(struct hang_mon *m, uint32_t beats, unsigned int state,
                  long now, long limit, long grace, long peer_grace)
{
    if (beats != m->seen || state == HANG_ST_IDLE)
    {
        m->seen = beats;
        m->since = now;
        return false;
    }
    if (state == HANG_ST_PEER)
        limit += peer_grace;
    return now - m->since >= limit + grace;
}
