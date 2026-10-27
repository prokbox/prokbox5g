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
 * iPod UI shell: the developer power log's ring and CSV formatting
 * (tracker #10). Pure logic; see powerlog_core.h.
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
#include "powerlog_core.h"

typedef char pl_row_size[sizeof(struct pl_row) == 36 ? 1 : -1]
    __attribute__((unused));
typedef char pl_ring_size[sizeof(struct pl_ring) <= 7200 ? 1 : -1]
    __attribute__((unused));

const char pl_csv_header[] =
    "s,mv,pct,charger,charging,play,boost_pct,bl_pct,lcd_pct,disk_pct,"
    "dac_allow,dac_sb,dac_sleeps,dac_wakes,vol_db,dropped,mark,"
    "idle_pct,cop_idle_pct,cpu1,cpu1_pct,cpu2,cpu2_pct,"
    "disk_cmds,disk_kb,disk_wakes,disk_by,"
    "stor_on_pct,stor_ons,stor_ons_total\n";

void pl_ring_reset(struct pl_ring *r)
{
    memset(r, 0, sizeof(*r));
}

void pl_ring_push(struct pl_ring *r, const struct pl_row *row)
{
    struct pl_row *dst;

    if (r->head - r->tail >= PL_ROWS)
    {
        r->tail++;                          /* the oldest is lost */
        if (r->dropped != 0xffff)
            r->dropped++;
    }
    dst = &r->rows[r->head % PL_ROWS];
    *dst = *row;
    dst->dropped = r->dropped;
    r->head++;
}

uint8_t pl_ring_mark(struct pl_ring *r, const char *tag)
{
    struct pl_mark *m = &r->marks[r->mark_no % PL_MARKS];
    size_t i;

    m->no = r->mark_no++;
    for (i = 0; i < PL_TAG_LEN - 1 && tag[i]; i++)
    {
        unsigned char c = tag[i];
        m->tag[i] = (c < 0x20 || c == ',' || c == '"' || c == 0x7f)
                    ? '_' : (char)c;
    }
    m->tag[i] = '\0';
    return (uint8_t)(m->no % 255 + 1);
}

const char *pl_ring_tag(const struct pl_ring *r, uint8_t id)
{
    int k;

    if (id == 0)
        return "";
    /* the newest mark with this id still in a slot */
    for (k = 1; k <= PL_MARKS && k <= r->mark_no; k++)
    {
        const struct pl_mark *m = &r->marks[(uint16_t)(r->mark_no - k)
                                            % PL_MARKS];
        if ((uint8_t)(m->no % 255 + 1) == id)
            return m->tag;
    }
    return "?";
}

/* "-" for none; commas (the CSV's separator) become '_' */
static const char *pl_name(char *out, const char *const names[3], int i)
{
    const char *n = names ? names[i] : NULL;
    size_t k;

    if (!n || !*n)
        return "-";
    for (k = 0; k < PL_NAME_LEN - 1 && n[k]; k++)
        out[k] = (n[k] == ',' || (unsigned char)n[k] < 0x20) ? '_' : n[k];
    out[k] = '\0';
    return out;
}

static int pl_pct(uint8_t v)
{
    return v == PL_UNKNOWN ? -1 : (int)v;
}

int pl_format_row(char *buf, size_t size, const struct pl_row *w,
                  const char *tag, const char *const names[3])
{
    char n1[PL_NAME_LEN], n2[PL_NAME_LEN], n3[PL_NAME_LEN];
    int play = (w->flags & PL_F_PAUSED) ? 2
             : (w->flags & PL_F_PLAYING) ? 1 : 0;
    int n = snprintf(buf, size,
                     "%lu,%u,%d,%d,%d,%d,%u,%u,%u,%u,%d,%d,%u,%u,%d,%u,%s,"
                     "%d,%d,%s,%d,%s,%d,%u,%u,%u,%s,%u,%u,%u\n",
                     (unsigned long)w->secs, (unsigned)w->mv,
                     w->pct == 255 ? -1 : (int)w->pct,
                     !!(w->flags & PL_F_CHARGER), !!(w->flags & PL_F_CHARGING),
                     play, (unsigned)w->boost_pct, (unsigned)w->bl_pct,
                     (unsigned)w->lcd_pct, (unsigned)w->disk_pct,
                     !!(w->flags & PL_F_DAC_ALLOW), !!(w->flags & PL_F_DAC_SB),
                     (unsigned)w->dac_sleeps, (unsigned)w->dac_wakes,
                     (int)w->vol, (unsigned)w->dropped, tag ? tag : "",
                     pl_pct(w->idle_pct), pl_pct(w->cop_idle_pct),
                     w->cpu1_slot == PL_UNKNOWN ? "-" : pl_name(n1, names, 0),
                     pl_pct(w->cpu1_pct),
                     w->cpu2_slot == PL_UNKNOWN ? "-" : pl_name(n2, names, 1),
                     pl_pct(w->cpu2_pct),
                     (unsigned)w->disk_cmds, (unsigned)w->disk_kb,
                     (unsigned)w->disk_wakes,
                     w->disk_by == PL_UNKNOWN ? "-" : pl_name(n3, names, 2),
                     (unsigned)w->stor_on_pct, (unsigned)w->stor_ons,
                     (unsigned)w->stor_ons_total);

    if (n < 0)
        return 0;
    if ((size_t)n >= size)
    {
        n = (int)size - 1;                  /* cut, but keep the newline */
        buf[n - 1] = '\n';
    }
    return n;
}
