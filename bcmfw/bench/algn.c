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
 * Bench row ALGN (bench builds): the runner and the DMA checks (algn.h).
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
#include "bcmfw.h"
#include "hw.h"
#include "dma.h"
#include "bench/algn.h"

#if BCMFW_BENCH

#define AB ((volatile uint32_t *)ALGN_RESULTS)

/* ---- DMA checks: channel 7, sub-channel 1, as Apple's dma_memcpy and
 * PW8 drive it (dma.md; data-paths.md cell 25), from algn_src into
 * algn_buf at chosen widths, offsets and unit counts.  r2-r5 of the
 * record: status after, state word after, microseconds, the unit count.
 * A wait that runs out stops the channel and says so. */
#define ALGN_DMA_CH     7
#define ALGN_DMA_SUB    1
#define ALGN_DMA_US     20000
#define ALGN_DMA_DONE   (1u << (13 + ALGN_DMA_SUB))

static uint32_t algn_cb[8] __attribute__((aligned(32)));

static uint32_t now(void)
{
    return REG(TIMER_CLO);
}

/* width: log2 bytes on both sides; units: the length field */
static uint32_t algn_dma(uint32_t src_off, uint32_t dst_off, uint32_t width,
                         uint32_t units)
{
    uint32_t t0, st, k;
    int timeout = 0;

    algn_cb[0] = 0;
    algn_cb[1] = (uint32_t)algn_src + src_off;
    algn_cb[2] = (uint32_t)algn_buf + dst_off;
    algn_cb[3] = units;
    /* Apple's memcpy word with the widths changed: bit 0, source
     * increment (8), destination increment (17), interrupt (24) */
    algn_cb[4] = 0x01020101u | TI_SRC_WIDTH(width) | TI_DST_WIDTH(width);
    algn_cb[5] = algn_cb[6] = algn_cb[7] = 0;
    REG(DMA_CS(ALGN_DMA_CH)) = 0x0000E802;
    REG(DMA_INT(ALGN_DMA_CH)) = DMA_INT_ALL;
    t0 = now();
    REG(DMA_CBADDR(ALGN_DMA_CH, ALGN_DMA_SUB)) = (uint32_t)algn_cb;
    for (k = 0; !(REG(DMA_INT(ALGN_DMA_CH)) & ALGN_DMA_DONE); k++)
        if (now() - t0 > ALGN_DMA_US || k > 0x100000)
        {
            timeout = 1;
            break;
        }
    bcmfw_algn_regs[2] = now() - t0;
    st = REG(DMA_INT(ALGN_DMA_CH));
    bcmfw_algn_regs[0] = st;
    bcmfw_algn_regs[1] = REG(DMA_CS(ALGN_DMA_CH));
    bcmfw_algn_regs[3] = units;
    REG(DMA_INT(ALGN_DMA_CH)) = DMA_INT_ALL;
    REG(DMA_CS(ALGN_DMA_CH)) = 0;
    return timeout ? ALGN_ST_TIMEOUT : ALGN_ST_RAN;
}

/* D32C: the control, PW8's form in SRAM: 64 bytes, 32-byte units */
uint32_t algn_dma_d32c(void)
{
    return algn_dma(0, 0, 5, 2);
}

/* D4W9: 4-byte units, nine of them (36 bytes, an odd count), from +4 to +4 */
uint32_t algn_dma_d4w9(void)
{
    return algn_dma(4, 4, 2, 9);
}

/* D2H3: 2-byte units, three (6 bytes), from +2 to +2: both ends 2 mod 4 */
uint32_t algn_dma_d2h3(void)
{
    return algn_dma(2, 2, 1, 3);
}

/* D1B5: byte units, five, from +1 to +1: an odd length at odd ends */
uint32_t algn_dma_d1b5(void)
{
    return algn_dma(1, 1, 0, 5);
}

/* D32M: one 32-byte unit to +4, off its width: rounded, split or refused? */
uint32_t algn_dma_d32m(void)
{
    return algn_dma(0, 4, 5, 1);
}

/* ---- the runner (CMD_ALGN) ------------------------------------------ */
void algn_run(void)
{
    uint32_t n, k, i, total = 0;

    for (n = 0; n < ALGN_MAX && algn_tab[ALGN_E_WORDS * n]; n++)
        ;
    AB[AH_MAGIC] = 0;
    AB[AH_STATE] = 1;
    AB[AH_FAULTS] = 0;
    AB[AH_SRC] = (uint32_t)algn_src;
    AB[AH_BUF] = (uint32_t)algn_buf;
    AB[AH_FORMAT] = n | ALGN_REC_WORDS << 8 | ALGN_VERSION << 16;
    AB[AH_MAGIC] = ALGN_MAGIC;
    for (k = 0; k < n; k++)
    {
        const uint32_t *e = algn_tab + ALGN_E_WORDS * k;
        volatile uint32_t *r = AB + ALGN_HDR_WORDS + ALGN_REC_WORDS * k;
        uint32_t st, win = ALGN_WIN(e[3]);

        for (i = 0; i < ALGN_REC_WORDS; i++)
            r[i] = 0;
        for (i = 0; i < ALGN_BUF_WORDS; i++)
            algn_buf[i] = ALGN_FILL(i);
        for (i = 0; i < 4; i++)
            bcmfw_algn_regs[i] = 0;
        bcmfw_algn_fault[8] = 0;
        AB[AH_STEP] = k;
        r[AR_TAG] = e[0];
        if (e[3] & ALGN_F_C)
        {
            st = ((uint32_t (*)(void))e[1])();
            r[AR_AT] = (uint32_t)algn_cb;
        }
        else
        {
            st = bcmfw_algn_call(e[1]) ? ALGN_ST_FAULT : ALGN_ST_RAN;
            r[AR_AT] = e[2];
        }
        if (st == ALGN_ST_FAULT)
        {
            st |= (bcmfw_algn_fault[0] & 0xFF) << 8;
            r[AR_PC] = bcmfw_algn_fault[1];
        }
        st |= (bcmfw_algn_fault[8] & 0xFF) << 16;
        total += bcmfw_algn_fault[8];
        for (i = 0; i < 4; i++)
            r[AR_REGS + i] = bcmfw_algn_regs[i];
        for (i = 0; i < ALGN_WIN_WORDS && win + i < ALGN_BUF_WORDS; i++)
            r[AR_BUF + i] = algn_buf[win + i];
        r[AR_STATUS] = st;
    }
    AB[AH_FAULTS] = total;
    AB[AH_STEP] = n;
    AB[AH_STATE] = 2;
}

#endif /* BCMFW_BENCH */
