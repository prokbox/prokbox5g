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

/* C reference for the polyphase FIR kernels in resample_fir.h (#271). The
 * ARM build uses resample_fir_arm.S instead; tests/resample/arm.sh checks the
 * two agree bit for bit. */
#ifndef RESAMPLE_HB_STANDALONE
#include "rbcodecconfig.h"
#include "platform.h"
#endif
#include "resample_fir.h"
#include "resample_fir_coefs.h"

#if defined(RESAMPLE_HB_STANDALONE) || !defined(CPU_ARM) || \
    defined(CPU_ARM_MICRO)

#ifdef RESAMPLE_HB_STANDALONE
#define RS_FIR_ROW   rs_fir_row_c
#define RS_FIR_BATCH rs_fir_batch_c
#else
#define RS_FIR_ROW   rs_fir_row
#define RS_FIR_BATCH rs_fir_batch
#endif

void RS_FIR_ROW(int32_t *c, uint32_t frac)
{
    const int32_t *r0 = rs_fir_proto[frac >> 26];
    const int32_t *r1 = r0 + RS_FIR_TAPS;
    int32_t eta = (int32_t)((frac & 0x3ffffff) << 5);

    for (int i = 0; i < RS_FIR_TAPS; i++)
    {
        int32_t d = (int32_t)((uint32_t)r1[i] - (uint32_t)r0[i]);
        c[i] = (int32_t)((uint32_t)r0[i] +
                         (uint32_t)(int32_t)(((int64_t)d * eta) >> 31));
    }
}

void RS_FIR_BATCH(int32_t *dst, const int32_t *x, const uintptr_t *tag,
                  int n)
{
    for (int k = 0; k < n; k++)
    {
        const int32_t *c = (const int32_t *)(tag[k] & ~(uintptr_t)3);
        int64_t acc = 0;
        x += tag[k] & 3;
        for (int i = 0; i < RS_FIR_TAPS; i++)
            acc += (int64_t)x[i] * c[i];
        dst[k] = (int32_t)(acc >> 31);
    }
}

#endif /* C reference */
