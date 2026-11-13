/***************************************************************************
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

/* tests/codecplace: what apps/codec_cop.c uses of lib/rbcodec/dsp/dsp_core.h
 * (#282 step 4, the DSP on the COP); the same layout as the real one */
#ifndef STUB_DSP_CORE_H
#define STUB_DSP_CORE_H
#include <stdbool.h>
#include <stdint.h>
struct dsp_config;
struct dsp_buffer {
    int32_t remcount;
    union {
        const void *pin[2];
        int16_t *p16out;
    };
    union {
        uint32_t proc_mask;
        int bufcount;
    };
};
void dsp_process(struct dsp_config *dsp, struct dsp_buffer *src,
                 struct dsp_buffer *dst, bool thread_yield);
void dsp_xcore_lock(void);
void dsp_xcore_unlock(void);
#endif
