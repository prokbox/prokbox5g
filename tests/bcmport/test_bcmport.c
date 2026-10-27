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

/* The host port's block write (bcm-port-block.h) against a model of the
 * port with the rules silicon showed, and of the PP's word loop.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- the port model (memory-access.md section 11, [silicon] sitting 2,
 * bench rows ALGNP): the write address ignores bits 1:0; data comes as
 * 32-bit words (each lands at the address, which then moves on 4) or as
 * 16-bit halfwords, paired into a word; a halfword left unpaired when a
 * new address is written is lost (PLH1/3/5), and so is one still unpaired
 * at the end (the conservative reading; never relied on) */
#define MEM 256
static uint8_t mem[MEM];
static uint32_t wa;                 /* the write address */
static bool half_pending;
static uint16_t half_v;
static uint32_t lone_lost, mid_word, reads;

static void put32(uint32_t a, uint32_t v)
{
    if (a + 4 <= MEM)
        memcpy(mem + a, &v, 4);
}

static void port_addr(uint32_t a)
{
    if (half_pending)
        lone_lost++;
    half_pending = false;
    if (a & 3)
        mid_word++;
    wa = a & ~3u;
}

static void port_data32(uint32_t v)
{
    if (half_pending)               /* never mixed by our writers */
        lone_lost++;
    half_pending = false;
    put32(wa, v);
    wa += 4;
}

static void port_data16(uint16_t v)
{
    if (!half_pending)
    {
        half_pending = true;
        half_v = v;
        return;
    }
    half_pending = false;
    put32(wa, half_v | (uint32_t)v << 16);
    wa += 4;
}

static void port_end(void)
{
    if (half_pending)
        lone_lost++;
    half_pending = false;
}

/* ---- the PP side: lcd_write_data() (lcd-as-video.S): ldm from the
 * source rounded down to a word (an ARM7TDMI ldm ignores bits 1:0), the
 * count's bits 16, 8, 4 and 2 only (an odd last halfword never goes) */
static uint32_t ldm_unaligned;
static void lcd_write_data(const uint16_t *s, uint32_t n)
{
    const uint32_t *w;
    uint32_t k;

    if ((uintptr_t)s & 3)
        ldm_unaligned++;
    w = (const uint32_t *)((uintptr_t)s & ~(uintptr_t)3);
    for (k = 0; k < n / 2; k++)
        port_data32(w[k]);
}

static int bpb_addr(uint32_t a) { port_addr(a); return 0; }
static void bpb_words(const uint16_t *s, uint32_t n) { lcd_write_data(s, n); }
static void bpb_word(uint32_t v) { port_data32(v); }
static int bpb_rd(uint32_t a, uint32_t *v)
{
    reads++;
    memcpy(v, mem + (a & ~3u), 4);
    return 0;
}
static int bpb_wr(uint32_t a, uint32_t v)
{
    port_addr(a);
    port_data32(v);
    return 0;
}

#include "bcm-port-block.h"

/* the writer before 2026-10-26 (dt_wr_block): the address, then
 * lcd_write_data with the count and source as given */
static int old_block(uint32_t addr, const uint16_t *src, uint32_t n)
{
    if (addr & 3)
        return -1;
    port_addr(addr);
    lcd_write_data(src, n);
    return 0;
}

static int failed, checks;
static void check(bool ok, const char *what)
{
    checks++;
    if (!ok)
    {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

/* One block: addr (a word), n halfwords from src_buf + so; true if the
 * memory reads exactly the fill with the n halfwords at addr */
static bool one(int (*fn)(uint32_t, const uint16_t *, uint32_t),
                uint32_t addr, uint32_t n, uint32_t so)
{
    static uint32_t srcw[32];
    const uint16_t *src = (const uint16_t *)srcw + so;
    uint8_t want[MEM];
    uint32_t i;

    for (i = 0; i < MEM; i++)
        mem[i] = (uint8_t)(0xF0 ^ i);
    for (i = 0; i < 64; i++)
        ((uint16_t *)srcw)[i] = (uint16_t)(0x1000 + i);
    memcpy(want, mem, MEM);
    memcpy(want + addr, src, 2 * n);
    if (fn(addr, src, n) < 0)
        return false;
    port_addr(0x80);                /* the next write: a new address */
    port_end();
    return memcmp(mem, want, MEM) == 0;
}

int main(void)
{
    char what[96];
    uint32_t addr, n, so, old_bad = 0, old_odd = 0, old_unal = 0;

    /* the model against silicon's words: PLH1 (one lone halfword by
     * 16-bit data, then a word elsewhere) keeps the fill at +20, and PLW3
     * (three halfwords as a word plus a zero-padded word) lands */
    memset(mem, 0xF0, MEM);
    lone_lost = 0;
    port_addr(0x20);
    port_data16(0x1000);
    port_addr(0x60);
    port_data32(0x12345678);
    check(mem[0x20] == 0xF0 && mem[0x21] == 0xF0 && lone_lost == 1,
          "model: PLH1's lone halfword lost (silicon: +20 f010 where 1000 was written)");
    port_addr(0x24);
    port_data32(0x1001u << 16 | 0x1000);
    port_data32(0x1002);
    check(mem[0x28] == 0x02 && mem[0x29] == 0x10, "model: PLW3's word tail lands");
    port_addr(0x22);
    check(wa == 0x20 && mid_word, "model: the address's bits 1:0 dropped (PWD2)");

    /* the block writer: every count 0..19, both source phases, three
     * word addresses: exact, nothing else touched, no mid-word address,
     * no lone halfword, no unaligned ldm */
    for (addr = 0x20; addr <= 0x28; addr += 4)
        for (so = 0; so < 2; so++)
            for (n = 0; n < 20; n++)
            {
                lone_lost = mid_word = ldm_unaligned = reads = 0;
                snprintf(what, sizeof(what), "bcm_port_block addr %02x n %u src +%u",
                         (unsigned)addr, (unsigned)n, (unsigned)(2 * so));
                check(one(bcm_port_block, addr, n, so) && !lone_lost && !mid_word &&
                      !ldm_unaligned && reads == (n & 1), what);
                /* the old writer, for the control */
                lone_lost = mid_word = ldm_unaligned = 0;
                if (!one(old_block, addr, n, so))
                {
                    old_bad++;
                    old_odd += n & 1;
                    old_unal += so && n > 1;
                }
            }
    check(bcm_port_block(0x22, (const uint16_t *)mem, 2) < 0,
          "bcm_port_block refuses a mid-word address");
    /* the control: the model catches both of the old writer's faults */
    check(old_odd > 0 && old_unal > 0,
          "control: the old writer's odd counts and unaligned sources fail in the model");
    printf("bcm_port_block: %d checks, %d failed (old writer: %u of 120 blocks wrong)\n",
           checks, failed, (unsigned)old_bad);
    return failed != 0;
}
