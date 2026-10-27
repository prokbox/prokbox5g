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

/* The SDK's pure rules for its files (sdk_core.h): the .state image and
 * the games file's records. Cold (once per entry, exit or save):
 * compiled Thumb (tools/thumb-cold.txt); sdk_core.c keeps the per-frame
 * rules. */
#include <string.h>
#include "ipodui/sdk/sdk_core.h"

/* -- the .state file ---------------------------------------------------- */
uint32_t sdk_crc32(const void *p, size_t n, uint32_t crc)
{
    const uint8_t *s = p;
    int k;

    crc = ~crc;
    while (n--)
    {
        crc ^= *s++;
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int sdk_state_pack(uint8_t *out, size_t cap, uint8_t set_ver,
                   const uint8_t *set, int set_n, const void *rec,
                   uint16_t rec_len)
{
    struct sdk_state_hdr h;
    size_t total = SDK_STATE_HDR + (size_t)set_n + rec_len;

    if (set_n < 0 || set_n > 255 || total > cap)
        return -1;
    memset(&h, 0, sizeof(h));
    h.magic = SDK_STATE_MAGIC;
    h.layout = SDK_STATE_LAYOUT;
    h.set_ver = set_ver;
    h.set_n = (uint8_t)set_n;
    h.rec_len = rec_len;
    if (set_n)
        memcpy(out + SDK_STATE_HDR, set, set_n);
    if (rec_len)
        memmove(out + SDK_STATE_HDR + set_n, rec, rec_len);
    h.crc = sdk_crc32(out + SDK_STATE_HDR, set_n + rec_len, 0);
    memcpy(out, &h, sizeof(h));
    return (int)total;
}

int sdk_state_unpack(const uint8_t *in, size_t len, uint8_t set_ver,
                     uint8_t *set, int set_n, const uint8_t *limits,
                     const uint8_t *defs, void *rec, uint16_t rec_cap,
                     uint16_t *rec_len)
{
    struct sdk_state_hdr h;
    int i, flags = SDK_SU_OK;
    bool reset = false;

    *rec_len = 0;
    if (set_n > 0)
        memcpy(set, defs, set_n);
    if (!in || len < SDK_STATE_HDR)
        return SDK_SU_NONE;
    memcpy(&h, in, sizeof(h));
    if (h.magic != SDK_STATE_MAGIC || h.layout != SDK_STATE_LAYOUT
        || (size_t)SDK_STATE_HDR + h.set_n + h.rec_len > len)
        return SDK_SU_NONE;
    if (sdk_crc32(in + SDK_STATE_HDR, h.set_n + h.rec_len, 0) != h.crc)
        return SDK_SU_CRC;
    /* the settings: the same table version and every value in range */
    if (h.set_ver != set_ver || h.set_n != set_n)
        reset = set_n > 0 || h.set_n > 0;
    for (i = 0; !reset && i < set_n; i++)
        if (in[SDK_STATE_HDR + i] >= limits[i])
            reset = true;
    if (reset)
        flags |= SDK_SU_SET_RESET;
    else if (set_n > 0)
        memcpy(set, in + SDK_STATE_HDR, set_n);
    if (h.rec_len <= rec_cap)
    {
        memmove(rec, in + SDK_STATE_HDR + h.set_n, h.rec_len); /* in
                                           place: rec may overlap, above */
        *rec_len = h.rec_len;
    }
    return flags;
}

/* -- the games file ------------------------------------------------------- */
void sdk_rec_sync(struct sdk_gamerec *r, uint32_t key)
{
    if (r->key != key)
    {
        r->key = key;               /* an update: kills start again */
        r->kills = 0;
    }
}

void sdk_rec_launch(struct sdk_gamerec *r)
{
    if (r->plays < 0xffff)
        r->plays++;
}

void sdk_rec_end(struct sdk_gamerec *r, bool killed)
{
    if (!killed)
        r->kills = 0;
    else if (r->kills < 0xff)
        r->kills++;
}

bool sdk_rec_hidden(const struct sdk_gamerec *r, uint32_t key)
{
    return r->key == key && r->kills >= SDK_KILLS_HIDE;
}
