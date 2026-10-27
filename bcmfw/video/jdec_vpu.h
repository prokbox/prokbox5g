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

#ifndef BCMFW_JDEC_VPU_H
#define BCMFW_JDEC_VPU_H
/* jdec's vector kernels (jdec_vpu.c): sets the decoder's hooks, the
 * colour stage always, the 4-, 8- and 16-point IDCTs with their put and
 * DC fill if idct and the start-up check passes at every size
 * (run once; fail: tests only, the check made to fail).  1 if the IDCT is
 * on the vector unit (jobs.h J_VPU_MAC), else 0 (the C) */
int jdec_vpu_init(int idct, int fail);
/* after jdec_vpu_init: 1 if the colour stage runs on the vector unit (its
 * start-up check agreed with the C; jobs.h J_VPU_YUV), else 0 (the C) */
int jdec_vpu_yuv(void);
#endif
