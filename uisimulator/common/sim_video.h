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
 * The simulator's BCM for the video player (HAVE_BCMFW_VIDEO): the same
 * calls lcd-video.c gives apps/video/vplayer.c on the device, answered by
 * the model of our image (apps/video/vbcm_model.c), whose pictures are
 * decoded with our own decoder (bcmfw/h264, built for the host) and put
 * on the simulator's panel.
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
#ifndef SIM_VIDEO_H
#define SIM_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_BCMFW_VIDEO
int bcm_video_take(void);
int bcm_video_boot(const uint8_t *img, uint32_t bytes);
void bcm_video_off(void);
void bcm_video_release(void);
/* p14-resident: the simulator has no resident image: always < 0 */
int bcm_video_hand_back(void);
int bcm_video_rd(uint32_t addr, uint32_t *val);
int bcm_video_wr(uint32_t addr, uint32_t val);
int bcm_video_wr_block(uint32_t addr, const uint16_t *src, uint32_t n);
/* the device's PP DMA path (bcm-devtest.h): the simulator copies, and
   counts as the CPU */
struct bcm_video_port_load
{
    uint32_t bytes, cpu_us, cpu_blocks, dma_us, dma_blocks, dma_fail;
};
void bcm_video_set_dma(bool on);
void bcm_video_set_dma_e2(bool on);
void bcm_video_port_load(struct bcm_video_port_load *out, bool reset);
int bcm_video_flush(void);
void bcm_video_set_dma_fixup(bool on);
uint32_t bcm_video_now_us(void);
void bcm_video_port_stats(uint32_t out[8], bool reset);

/* Pictures put on the panel since the last take, and decode errors */
uint32_t sim_video_frames(void);
uint32_t sim_video_errors(void);
/* The panel as the model last showed it, to <--shotdir>/<name>.bmp as
   the shot command writes the framebuffer (which the player doesn't use) */
bool sim_video_shot(const char *name);
/* The first-use check's faults, on the model until the next take or the
   next call (vbcm_model.h): bands whose switch falls back (bit b), the
   picture of a session from which every one decodes wrong (0 never),
   and a packet's decode time (0: the model's own), slow enough for a
   script to see the check's screen */
/* The play about to boot is the resident image's own (a normal play),
   or not (a bench or stage play, a lab, a probe: its image from the
   file).  bcm_video_release() after a play that was not hands the chip
   to Apple's vmcs, as the device's stock wake path does (quirks #48):
   the shell's chip jobs are off (the job driver suspended) until a
   resident play's stop hands our image back */
void sim_video_resident(bool res);
void sim_video_vcheck_faults(uint32_t vclk_fail_mask, uint32_t pic_bad,
                             uint32_t decode_us);
/* The first-use check's inconclusive path (the model's no_pic knob: each
   packet taken, no picture); off again before the film */
void sim_video_vcheck_nopic(bool on);
/* day 4c: the model refuses the FEED n FEEDs from now (HP_E_BUSY, as a
   full queue), its error reply lost too if quiet (vbcm_model.h busy_at,
   feed_err_quiet); n 0 clears.  The FIFO errors that follow count in
   sim_video_feed_errors(), not sim_video_errors() */
void sim_video_feed_fault(uint32_t n, bool quiet);
uint32_t sim_video_feed_errors(void);
uint32_t sim_video_feed_refusals(void);   /* the full-queue refusals */
/* a CLOSE came after the refusal (a restart overtook it, #192), and the
   last video_fault feed armed again */
bool sim_video_feed_overtaken(void);
void sim_video_feed_rearm(void);
/* day 4c: from this file offset on the audio's buffer never holds the
   video's bytes (vplayer.c src_read answers "not buffered" unless the
   play reads the file directly); 0 none */
void sim_video_set_hole(uint64_t off);
uint64_t sim_video_hole(void);
/* #175: the next play's index window in records (vpc_source's
   window_records, so a short clip slides it); 0 the build's */
void sim_video_set_window(uint32_t records);
uint32_t sim_video_window(void);
/* #340: from the next take on, every packet is taken with no picture
   and STATUS reports this many decode errors (the model's no_pic and
   st_errors knobs: b12's first OPEN after a boot, whose decode failed and
   showed nothing); 0 clears.  Kept over takes until cleared */
void sim_video_set_decode_fault(uint32_t errors);
#endif

#endif /* SIM_VIDEO_H */
