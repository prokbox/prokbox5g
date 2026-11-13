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

/* PCM DMA chunk lengths and addresses on the PP502x (issue #191).
 *
 * Runs the REAL playback DMA arithmetic of firmware/target/arm/pp/pcm-pp.c
 * (struct dma_data, dma_play_data, max_dma_chunk_size, DMA_PLAY_CONFIG,
 * dma_tx_buf_prepare, dma_tx_start, the PP502x fiq_playback, sink_set_freq,
 * sink_start_pcm, sink_dma_start: run.sh extracts them into pcmpp_parts.c)
 * against a register model, at every rate of the real hw_freq_sampr
 * (firmware/pcm_sampr.c, built with the iPod Video's HW_SAMPR_CAPS), for
 * every buffer size 4..8192 in whole words and every start offset 0..3,
 * each passed through the real ALIGN_AUDIOBUF (pcm-internal.h) as
 * pcm_get_more_int() and pcm_play_data() do.
 *
 * Asserts, for every transfer programmed into DMA0:
 *   - its length ((DMA0_CMD & 0xffff) + 4) is a multiple of 4 (DMA0_INCR is
 *     32 bits wide; what silicon does with a part word is unknown);
 *   - DMA0_RAM_ADDR is a multiple of 4;
 *   - the transfers tile each buffer exactly (no byte skipped or repeated).
 * Prints the chunk size per rate and the number of transfers checked;
 * fails when nothing was checked.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcm_sampr.h"

/* ---- the environment pcm-pp.c expects ---- */
#define ICODE_ATTR
#define IBSS_ATTR
#define NUM_CORES 1
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
static unsigned long uncached_base;          /* 0: host pointers pass as-is */
#define UNCACHED_BASE_ADDR uncached_base
#define UNCACHED_ADDR(a) (a)
static void commit_dcache(void) {}
#define FIQ_TARGET_SET(core, fn) ((void)(fn))
#define IF_LAYOUT_A_CORE 0

/* DMA0 register model: plain cells */
static volatile unsigned long r_cmd, r_status, r_ram;
#define DMA0_CMD      r_cmd
#define DMA0_STATUS   r_status
#define DMA0_RAM_ADDR r_ram
#include "dma_defs.h"                        /* DMA_CMD_*, DMA_REQ_IIS */

enum pcm_dma_status { PCM_DMAST_ERR_DMA = -1, PCM_DMAST_OK = 0,
                      PCM_DMAST_STARTED = 1 };
bool pcm_play_dma_complete_callback(enum pcm_dma_status status,
                                    const void **addr, size_t *size);
static enum pcm_dma_status pcm_play_dma_status_callback(enum pcm_dma_status s)
{
    return s;
}
static void audiohw_set_frequency(int f) { (void)f; }
static inline void dma_tx_setup(void) {}
static void sink_dma_stop(void);
/* p083-audiophile: the high-rate stall watch's piece counter (shell builds) */
#define PCM_PP_PIECE_DONE() do { } while (0)
/* 0.8.3 fifo96: the lateness model's start (shell builds) */
#define PCM_PP_ARMED(size) do { (void)(size); } while (0)

#include "align_audiobuf.h"                  /* the real ALIGN_AUDIOBUF */
#include "pcmpp_parts.c"

static void sink_dma_stop(void)
{
    dma_play_data.addr = 0;
    dma_play_data.size = 0;
}

/* ---- the PCM layer: hands out NBUF buffers, through ALIGN_AUDIOBUF ---- */
#define NBUF    6
#define MAXSZ   8192
static uint32_t pool[2][MAXSZ / 4 + 4];
static int handed;
static size_t cur_size, cur_off;
static uintptr_t buf_base, buf_end, cursor;
static long checked, failures;
static int cur_rate;
static long rate_checked, rate_failures;
static char frame_seq[160];   /* the 1 KB mixer frame's transfers */

static void begin_buffer(const void *p, size_t size)
{
    buf_base = cursor = (uintptr_t)p;
    buf_end = buf_base + size;
}

static void get_buffer(const void **addr, size_t *size)
{
    const void *p = (const char *)pool[handed & 1] + cur_off;
    size_t s = cur_size;
    ALIGN_AUDIOBUF(p, s);
    *addr = p;
    *size = s;
}

bool pcm_play_dma_complete_callback(enum pcm_dma_status status,
                                    const void **addr, size_t *size)
{
    (void)status;
    if (cursor != buf_end && (rate_failures++, failures++ < 3))
        printf("FAIL: %lu Hz size %zu off %zu: buffer done after %ld of "
               "%ld bytes\n", hw_freq_sampr[cur_rate], cur_size, cur_off,
               (long)(cursor - buf_base), (long)(buf_end - buf_base));
    if (++handed >= NBUF)
        return false;
    get_buffer(addr, size);
    begin_buffer(*addr, *size);
    return true;
}

/* One programming of DMA0 happened (RAM_ADDR moved off the sentinel) */
static void check_transfer(void)
{
    unsigned long len = (r_cmd & 0xffff) + 4;

    checked++;
    rate_checked++;
    if (cur_size == 1024 && cur_off == 0 && handed == 0 &&
        strlen(frame_seq) < sizeof(frame_seq) - 12)
        snprintf(frame_seq + strlen(frame_seq),
                 sizeof(frame_seq) - strlen(frame_seq), "%s%lu",
                 frame_seq[0] ? "+" : "", len);
    if (((len & 3) || (r_ram & 3) || r_ram != cursor ||
         cursor + len > buf_end) && (rate_failures++, failures++ < 3))
        printf("FAIL: %lu Hz size %zu off %zu: programmed length %lu at "
               "RAM_ADDR offset %ld (expected offset %ld, buffer %ld, "
               "address %% 4 = %lu)\n",
               hw_freq_sampr[cur_rate], cur_size, cur_off, len,
               (long)(r_ram - buf_base), (long)(cursor - buf_base),
               (long)(buf_end - buf_base), r_ram & 3);
    cursor = r_ram + len;   /* what the engine moves */
}

#define SENTINEL (~0UL)

int main(void)
{
    int nrates = 0;

    for (int f = 0; f < HW_NUM_FREQ; f++) {
        cur_rate = f;
        sink_set_freq(f);
        rate_checked = rate_failures = 0;
        frame_seq[0] = '\0';
        nrates++;
        for (cur_off = 0; cur_off < 4; cur_off++) {
            for (cur_size = 4; cur_size <= MAXSZ; cur_size += 4) {
                const void *p;
                size_t s;

                handed = 0;
                get_buffer(&p, &s);
                begin_buffer(p, s);
                r_ram = SENTINEL;
                sink_dma_start(p, s);
                if (r_ram == SENTINEL) {
                    printf("FAIL: sink_dma_start programmed nothing\n");
                    return 1;
                }
                check_transfer();
                for (int guard = 0; ; guard++) {
                    if (guard > 100000) {
                        printf("FAIL: the FIQ loop never ended\n");
                        return 1;
                    }
                    r_ram = SENTINEL;
                    r_status = 1ul << 30;       /* DMA_STATUS_INTR */
                    fiq_playback();             /* the transfer completed */
                    if (r_ram == SENTINEL)
                        break;                  /* no more data */
                    check_transfer();
                }
                if (handed != NBUF) {
                    printf("FAIL: %d of %d buffers played\n", handed, NBUF);
                    return 1;
                }
            }
        }
        printf("rate %6lu Hz: chunk %3u bytes%s; 1 KB frame -> %s; "
               "%ld transfers, %ld bad\n", hw_freq_sampr[f],
               (unsigned)max_dma_chunk_size,
               (max_dma_chunk_size & 3) ? " (NOT whole words)" : "",
               frame_seq, rate_checked, rate_failures);
    }
    printf("pcmdma: %d rates, %ld DMA transfers checked, %ld failures\n",
           nrates, checked, failures);
    if (nrates == 0 || checked == 0) {
        printf("FAIL: nothing checked\n");
        return 1;
    }
    return failures ? 1 : 0;
}
