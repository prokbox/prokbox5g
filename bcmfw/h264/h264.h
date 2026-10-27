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
 * Our H.264 Baseline decoder (ITU-T H.264, CAVLC, progressive, 4:2:0,
 * 8-bit), written from the standard for the BCM2722's VideoCore (#60).
 * Portable, freestanding C: no malloc (one caller-provided arena), integer
 * only, no libc beyond memcpy/memset, and every error is a return code.
 * Design and results: docs/research/bcm2722/decoder.md.
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
#ifndef H264_H
#define H264_H

#include <stdint.h>
#include <stddef.h>

/* Return codes.  Negative values are errors; the decoder stays usable
 * after every one of them (the next IDR picture resynchronises). */
#define H264_OK              0   /* NAL consumed, no picture finished */
#define H264_FRAME           1   /* NAL consumed and a picture finished:
                                    h264_frame() until the next call */
#define H264_FRAME_AGAIN     2   /* an unfinished picture was closed by
                                    this NAL, which was NOT consumed:
                                    take h264_frame(), then pass the
                                    same NAL again */
#define H264_E_BITSTREAM   (-1)  /* malformed syntax (slice dropped) */
#define H264_E_UNSUPPORTED (-2)  /* valid H.264 outside our subset */
/* the last H264_E_UNSUPPORTED's check: file << 16 | line (h264_int.h) */
extern uint32_t h264_refused_at;
/* the last other error's: file << 16 | line (h264_int.h H264_ERR) */
extern uint32_t h264_error_at;
#define H264_E_NOMEM       (-3)  /* the arena is too small for the SPS */
#define H264_E_STATE       (-4)  /* no SPS/PPS, missing reference, ... */

/* Limits of this decoder (the SPS is refused beyond them). */
#define H264_MAX_MB_W        40  /* 640 pixels */
#define H264_MAX_MB_H        30  /* 480 pixels */
#define H264_MAX_REF_FRAMES  16

struct h264_dec;

/* A decoded picture, cropped as the SPS says.  The planes stay valid
 * until the next call into the decoder. */
struct h264_picture {
    const uint8_t *y, *u, *v;
    int stride_y, stride_c;      /* bytes per row */
    int width, height;           /* luma, after cropping */
    int frame_num;
    int idr;
    int concealed;               /* macroblocks that were not decoded */
};

struct h264_stats {
    uint32_t pictures;           /* finished (output) */
    uint32_t slices;
    uint32_t mbs;                /* macroblocks decoded (skipped included) */
    uint32_t skipped;            /* P_Skip macroblocks */
    uint32_t concealed;          /* macroblocks concealed */
    uint32_t errors;             /* error returns */
};

/* Bytes of arena needed for a stream of mb_w x mb_h macroblocks with
 * num_ref_frames references (the SPS's max_num_ref_frames). */
size_t h264_arena_size(int mb_w, int mb_h, int num_ref_frames);

/* Start a decoder in arena (any alignment; at least h264_arena_size()
 * of the largest stream it will see).  NULL if size can't even hold the
 * context.  Buffers are laid out when the first SPS is activated. */
struct h264_dec *h264_open(void *arena, size_t size);

/* The split layout (bcmfw: context and macroblock info in internal SRAM,
 * pictures in SDRAM).  h264_ctx_size() is the arena h264_open() needs
 * for a stream of mb_w x mb_h macroblocks when the frames live apart,
 * h264_frames_size() the frame buffer region (32-byte aligned planes,
 * plus the vector kernels' slack).  Call h264_set_frames() once, right
 * after h264_open(); without it the frames follow the context in the
 * arena (h264_arena_size).  Returns 0, or H264_E_NOMEM if frames is
 * NULL or not 32-byte aligned. */
size_t h264_ctx_size(int mb_w, int mb_h);
size_t h264_frames_size(int mb_w, int mb_h, int num_ref_frames);
int h264_set_frames(struct h264_dec *d, void *frames, size_t size);

/* With on: the frames go in the arena after the context whenever the
 * stream fits there (bcmfw: internal SRAM), in the h264_set_frames()
 * region only when it doesn't.  Off (the default): the region always. */
void h264_frames_arena_first(struct h264_dec *d, int on);
/* Where the frames went: frame 0's luma plane, NULL before a layout */
const void *h264_frame_base(const struct h264_dec *d);

/* Decode one NAL unit (no start code).  The buffer is modified in place
 * (emulation-prevention bytes are removed).  Returns a code above. */
int h264_decode_nal(struct h264_dec *d, uint8_t *nal, int len);

/* End of stream: finish a picture still in progress.  H264_FRAME if one
 * was finished, else H264_OK. */
int h264_flush(struct h264_dec *d);

/* The picture after H264_FRAME / H264_FRAME_AGAIN. */
const struct h264_picture *h264_frame(const struct h264_dec *d);
/* The parameter sets held: bit n set for a valid PPS (or SPS) id n
   (session 37's diagnostic) */
uint32_t h264_ps_valid(const struct h264_dec *d, uint32_t *sps);

const struct h264_stats *h264_stats(const struct h264_dec *d);

/* Annex B: find the next NAL unit in buf[pos..len).  On success returns 1
 * and sets *nal and *nal_len (start code and trailing zero bytes
 * excluded) and *next (where to continue); returns 0 at the end. */
int h264_annexb_next(const uint8_t *buf, int len, int pos,
                     int *nal, int *nal_len, int *next);

#endif /* H264_H */
