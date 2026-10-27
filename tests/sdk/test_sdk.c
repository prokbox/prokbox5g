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

/* Host test of the SDK's pure rules (apps/ipodui/sdk/sdk_core.c) */
#include <stdio.h>
#include <string.h>
#include "ipodui/sdk/sdk_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Feeds the chord every 10 ms from t0 to t1 with fixed buttons; returns
 * the OR of the results, counts BEGIN and EXIT */
static int run(struct sdk_chord *ch, bool s, bool p, uint32_t t0,
               uint32_t t1, int *begins, int *exits, bool *all_active)
{
    int acc = 0;
    uint32_t t;

    for (t = t0; t <= t1; t += 10)
    {
        int r = sdk_chord_feed(ch, s, p, t, 2000);

        acc |= r;
        if (r & SDK_CHR_BEGIN)
            (*begins)++;
        if (r & SDK_CHR_EXIT)
            (*exits)++;
        if (all_active && !(r & SDK_CHR_ACTIVE))
            *all_active = false;
    }
    return acc;
}

static void test_chord_overlap(uint32_t ms, bool sel_first)
{
    struct sdk_chord ch;
    int b = 0, e = 0, r;
    bool act = true;

    memset(&ch, 0, sizeof(ch));
    /* the first button alone: not active */
    r = run(&ch, sel_first, !sel_first, 0, 500, &b, &e, NULL);
    CHECK(r == 0);
    /* both down for ms */
    run(&ch, true, true, 510, 510 + ms, &b, &e, &act);
    CHECK(b == 1);
    CHECK(e == 0);
    CHECK(act);
    /* the second button released, the first still held: still swallowed */
    run(&ch, sel_first, !sel_first, 520 + ms, 3000 + ms, &b, &e, &act);
    CHECK(act);
    CHECK(e == 0);
    /* both up: idle again */
    r = sdk_chord_feed(&ch, false, false, 4000 + ms, 2000);
    CHECK(r & SDK_CHR_ACTIVE);  /* this release event itself is withheld */
    CHECK(ch.state == SDK_CH_IDLE);
    r = sdk_chord_feed(&ch, true, false, 4100 + ms, 2000);
    CHECK(r == 0);
}

static void test_chord(void)
{
    struct sdk_chord ch;
    int b = 0, e = 0;
    bool act = true;

    test_chord_overlap(300, true);
    test_chord_overlap(300, false);
    test_chord_overlap(1900, true);
    test_chord_overlap(1900, false);

    /* 2.0 s: exits exactly once, then swallows to both up, then re-arms */
    memset(&ch, 0, sizeof(ch));
    CHECK(sdk_chord_due(&ch, 0, 2000) == -1);
    run(&ch, true, true, 1000, 3000, &b, &e, &act);
    CHECK(b == 1 && e == 1 && act);
    run(&ch, true, true, 3010, 9000, &b, &e, &act);
    CHECK(b == 1 && e == 1 && act);
    sdk_chord_feed(&ch, false, false, 9100, 2000);
    CHECK(ch.state == SDK_CH_IDLE);
    run(&ch, true, true, 10000, 12000, &b, &e, &act);
    CHECK(b == 2 && e == 2);

    /* due */
    memset(&ch, 0, sizeof(ch));
    sdk_chord_feed(&ch, true, true, 100, 2000);
    CHECK(sdk_chord_due(&ch, 600, 2000) == 1500);
    CHECK(sdk_chord_due(&ch, 2100, 2000) == 0);
    CHECK(sdk_chord_due(&ch, 5000, 2000) == 0);
    /* both released at once before 2 s: straight back to idle */
    CHECK(sdk_chord_feed(&ch, false, false, 700, 2000) == SDK_CHR_ACTIVE);
    CHECK(ch.state == SDK_CH_IDLE);
}

static void test_bands(void)
{
    struct sdk_bands b;

    memset(&b, 0, sizeof(b));
    sdk_bands_add(&b, 30, 10, 20, 240);
    sdk_bands_add(&b, 35, 10, 20, 240);         /* overlapping */
    CHECK(b.n == 1 && b.y[0] == 30 && b.h[0] == 15);
    sdk_bands_add(&b, 45, 5, 20, 240);          /* touching */
    CHECK(b.n == 1 && b.y[0] == 30 && b.h[0] == 20);
    CHECK(sdk_bands_rows(&b) == 20);

    /* clipping */
    memset(&b, 0, sizeof(b));
    sdk_bands_add(&b, 0, 30, 20, 240);
    CHECK(b.n == 1 && b.y[0] == 20 && b.h[0] == 10);
    sdk_bands_add(&b, 230, 50, 20, 240);
    CHECK(b.n == 2 && b.y[1] == 230 && b.h[1] == 10);
    sdk_bands_add(&b, 0, 10, 20, 240);          /* fully off: nothing */
    sdk_bands_add(&b, 100, 0, 20, 240);
    CHECK(b.n == 2);

    /* four separate, then a fifth joins its nearest */
    memset(&b, 0, sizeof(b));
    sdk_bands_add(&b, 20, 5, 20, 240);
    sdk_bands_add(&b, 60, 5, 20, 240);
    sdk_bands_add(&b, 120, 5, 20, 240);
    sdk_bands_add(&b, 200, 5, 20, 240);
    CHECK(b.n == 4 && sdk_bands_rows(&b) == 20);
    sdk_bands_add(&b, 130, 5, 20, 240);         /* nearest: 120..125 */
    CHECK(b.n == 4);
    CHECK(b.y[2] == 120 && b.h[2] == 15);
    CHECK(sdk_bands_rows(&b) == 30);
    /* a mark spanning two bands joins them */
    sdk_bands_add(&b, 50, 80, 20, 240);         /* 50..130 */
    CHECK(b.n == 3);
    CHECK(b.y[1] == 50 && b.h[1] == 85);
    CHECK(sdk_bands_rows(&b) == 95);
}

/* The record in RAM, as sdk_cold.c keeps it: one 16-aligned block holds
 * the file image (read in place) and the record at SDK_REC_OFF(set_n),
 * which must be 8-aligned for every settings count (a game casts it to its
 * struct; the ARM7TDMI aborts a misaligned access: P12 device bench,
 * Chute at state_img + 17). Unpack moves the record up in place; the
 * write packs it down and moves it back. */
static void test_state_ram(void)
{
    static uint8_t blk[512] __attribute__((aligned(16)));
    uint8_t file[256], set[16], lim[16],
            def[16], sv[16], r[37];
    int set_n, n, i, f;
    uint16_t len;

    for (i = 0; i < (int)sizeof(r); i++)
        r[i] = (uint8_t)(i * 13 + 5);
    for (i = 0; i < 16; i++)
    {
        lim[i] = 4; def[i] = 0; sv[i] = (uint8_t)(i & 3);
    }
    for (set_n = 0; set_n <= 16; set_n++)
    {
        uint8_t *rec = blk + SDK_REC_OFF(set_n);

        CHECK(((uintptr_t)rec & (SDK_REC_ALIGN - 1)) == 0);
        CHECK((int)SDK_REC_OFF(set_n) >= SDK_STATE_HDR + set_n);
        n = sdk_state_pack(file, sizeof(file), 3, sv, set_n, r, sizeof(r));
        CHECK(n == SDK_STATE_HDR + set_n + (int)sizeof(r));
        memset(blk, 0xee, sizeof(blk));
        memcpy(blk, file, n);                   /* the one range read */
        f = sdk_state_unpack(blk, n, 3, set, set_n, lim, def, rec,
                             sizeof(r), &len);
        CHECK(f == 0 && len == sizeof(r) && !memcmp(rec, r, sizeof(r)));
        /* sdki_state_write: pack in place, write, the record back */
        n = sdk_state_pack(blk, SDK_STATE_HDR + set_n + sizeof(r), 3, set,
                           set_n, rec, len);
        CHECK(n > 0 && !memcmp(blk, file, n));
        memmove(rec, blk + SDK_STATE_HDR + set_n, len);
        CHECK(!memcmp(rec, r, sizeof(r)));
    }
}

static void test_state(void)
{
    uint8_t buf[256], set[3], rec[64];
    const uint8_t lim[3] = { 2, 3, 10 }, def[3] = { 1, 0, 5 };
    const uint8_t sv[3] = { 0, 2, 9 };
    const char *r = "hello, record";
    uint16_t len;
    int n, f;

    n = sdk_state_pack(buf, sizeof(buf), 7, sv, 3, r, (uint16_t)strlen(r));
    CHECK(n == SDK_STATE_HDR + 3 + (int)strlen(r));
    f = sdk_state_unpack(buf, n, 7, set, 3, lim, def, rec, sizeof(rec), &len);
    CHECK(f == SDK_SU_OK);
    CHECK(!memcmp(set, sv, 3));
    CHECK(len == strlen(r) && !memcmp(rec, r, len));

    /* too small a buffer */
    CHECK(sdk_state_pack(buf, 10, 7, sv, 3, r, 5) == -1);

    /* no file */
    f = sdk_state_unpack(NULL, 0, 7, set, 3, lim, def, rec, sizeof(rec), &len);
    CHECK(f == SDK_SU_NONE && len == 0 && !memcmp(set, def, 3));

    /* CRC failure */
    n = sdk_state_pack(buf, sizeof(buf), 7, sv, 3, r, (uint16_t)strlen(r));
    buf[n - 1] ^= 1;
    f = sdk_state_unpack(buf, n, 7, set, 3, lim, def, rec, sizeof(rec), &len);
    CHECK(f == SDK_SU_CRC && len == 0 && !memcmp(set, def, 3));

    /* bad magic */
    n = sdk_state_pack(buf, sizeof(buf), 7, sv, 3, r, (uint16_t)strlen(r));
    buf[0] ^= 0xff;
    f = sdk_state_unpack(buf, n, 7, set, 3, lim, def, rec, sizeof(rec), &len);
    CHECK(f == SDK_SU_NONE && len == 0);

    /* truncated */
    n = sdk_state_pack(buf, sizeof(buf), 7, sv, 3, r, (uint16_t)strlen(r));
    f = sdk_state_unpack(buf, n - 1, 7, set, 3, lim, def, rec, sizeof(rec),
                         &len);
    CHECK(f == SDK_SU_NONE);

    /* a settings version change resets them, keeps the record */
    n = sdk_state_pack(buf, sizeof(buf), 6, sv, 3, r, (uint16_t)strlen(r));
    f = sdk_state_unpack(buf, n, 7, set, 3, lim, def, rec, sizeof(rec), &len);
    CHECK(f == SDK_SU_SET_RESET && !memcmp(set, def, 3));
    CHECK(len == strlen(r) && !memcmp(rec, r, len));

    /* a value out of range resets all of them */
    {
        const uint8_t bad[3] = { 1, 3, 2 };
        n = sdk_state_pack(buf, sizeof(buf), 7, bad, 3, r, 4);
        f = sdk_state_unpack(buf, n, 7, set, 3, lim, def, rec, sizeof(rec),
                             &len);
        CHECK(f == SDK_SU_SET_RESET && !memcmp(set, def, 3) && len == 4);
    }

    /* a record bigger than the cap: settings kept, no record */
    n = sdk_state_pack(buf, sizeof(buf), 7, sv, 3, r, (uint16_t)strlen(r));
    f = sdk_state_unpack(buf, n, 7, set, 3, lim, def, rec, 4, &len);
    CHECK(f == SDK_SU_OK && len == 0 && !memcmp(set, sv, 3));

    /* no settings, an empty record */
    n = sdk_state_pack(buf, sizeof(buf), 1, NULL, 0, NULL, 0);
    CHECK(n == SDK_STATE_HDR);
    f = sdk_state_unpack(buf, n, 1, set, 0, lim, def, rec, sizeof(rec), &len);
    CHECK(f == SDK_SU_OK && len == 0);

    /* CRC-32 check value */
    CHECK(sdk_crc32("123456789", 9, 0) == 0xCBF43926u);
}

static void test_slice(void)
{
    uint8_t v = 0;

    CHECK(sdk_slice_judge(20000, 20000, &v) == SDK_J_OK && v == 0);
    CHECK(sdk_slice_judge(25000, 20000, &v) == SDK_J_VIOL && v == 1);
    CHECK(sdk_slice_judge(1000, 20000, &v) == SDK_J_OK);
    CHECK(sdk_slice_judge(25000, 20000, &v) == SDK_J_KILL);
    v = 0;
    CHECK(sdk_slice_judge(200001, 20000, &v) == SDK_J_KILL);
    v = 0;
    CHECK(sdk_slice_judge(200000, 20000, &v) == SDK_J_VIOL);
}

static void test_rec(void)
{
    struct sdk_gamerec r;

    memset(&r, 0, sizeof(r));
    sdk_rec_sync(&r, 42);
    sdk_rec_launch(&r);
    CHECK(r.plays == 1);
    sdk_rec_end(&r, true);
    sdk_rec_end(&r, true);
    CHECK(!sdk_rec_hidden(&r, 42));
    sdk_rec_end(&r, false);                     /* a clean end resets */
    CHECK(r.kills == 0);
    sdk_rec_end(&r, true);
    sdk_rec_end(&r, true);
    sdk_rec_end(&r, true);
    CHECK(sdk_rec_hidden(&r, 42));
    CHECK(!sdk_rec_hidden(&r, 43));
    sdk_rec_sync(&r, 43);                       /* an update */
    CHECK(r.kills == 0 && !sdk_rec_hidden(&r, 43) && r.plays == 1);
    r.plays = 0xffff;
    sdk_rec_launch(&r);
    CHECK(r.plays == 0xffff);
}

static void test_rate(void)
{
    struct sdk_rate r;
    int i, ok = 0;

    memset(&r, 0, sizeof(r));
    for (i = 0; i < 30; i++)
        ok += sdk_rate_take(&r, 5 + i, 1000, 20);
    CHECK(ok == 20);
    CHECK(!sdk_rate_take(&r, 999, 1000, 20));
    CHECK(sdk_rate_take(&r, 1000, 1000, 20));
}

/* the absolute wheel, polled (sdk.md 3.3; #132) */
static void test_wheel(void)
{
    struct sdk_wwatch w;

    CHECK(sdk_wheel_norm(-1) == -1);
    CHECK(sdk_wheel_norm(0) == 0);
    CHECK(sdk_wheel_norm(95) == 95);
    CHECK(sdk_wheel_norm(96) == -1);
    CHECK(sdk_wheel_norm(0x7f) == -1);
    memset(&w, 0, sizeof(w));
    /* lit, nobody on the ring: no wake */
    CHECK(sdk_ww_poll(&w, true, -1, 0) == 0);
    /* a finger lands: woken while it stays, and once after the lift */
    CHECK(sdk_ww_poll(&w, true, 30, 1) == SDK_WW_WAKE);
    CHECK(sdk_ww_poll(&w, true, 31, 1) == SDK_WW_WAKE);
    CHECK(sdk_ww_poll(&w, true, -1, 1) == SDK_WW_WAKE);
    CHECK(sdk_ww_poll(&w, true, -1, 1) == 0);
    /* a tap shorter than a poll: only the touch count saw it */
    CHECK(sdk_ww_poll(&w, true, -1, 2) == SDK_WW_WAKE);
    CHECK(sdk_ww_poll(&w, true, -1, 2) == 0);
    /* dark: a touch wakes nobody (the driver lights the screen) */
    CHECK(sdk_ww_poll(&w, false, -1, 2) == 0);
    CHECK(sdk_ww_poll(&w, false, 12, 3) == 0);
    /* lit again: woken, and told so */
    CHECK(sdk_ww_poll(&w, true, 12, 3) == (SDK_WW_WAKE | SDK_WW_LIT));
    CHECK(sdk_ww_poll(&w, true, -1, 3) == SDK_WW_WAKE);
    CHECK(sdk_ww_poll(&w, true, -1, 3) == 0);
}

int main(void)
{
    test_chord();
    test_bands();
    test_state();
    test_state_ram();
    test_slice();
    test_rec();
    test_rate();
    test_wheel();
    printf("sdk core: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
