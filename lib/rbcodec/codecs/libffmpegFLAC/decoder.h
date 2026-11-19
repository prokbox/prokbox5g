/* Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md. */
#ifndef _FLAC_DECODER_H
#define _FLAC_DECODER_H

#include "bitstream.h"

#define MAX_CHANNELS   7      /* Maximum supported channels, only left/right will be played back */
#ifndef MAX_BLOCKSIZE
#define MAX_BLOCKSIZE  4608   /* Samples per channel of flac.c's static buffers;
                                 larger blocks (to 65535) get buffers from
                                 codec_malloc() */
#endif
#define MAX_FRAMESIZE  65536  /* Maxsize in bytes of one compressed frame */
#define MIN_FRAME_SIZE 11     /* smallest valid FLAC frame possible */

#include "flac_limits.h" /* FLAC_OUTPUT_DEPTH (29): samples left-shifted to 28 bits+sign */
/* #318: streams of FLAC_OUTPUT_DEPTH + 1 .. 32 bits decode at their full
   width (a 32-bit stereo stream's side channel has 33 bits: FLACContext's
   wide buffer) and are then reduced to FLAC_OUTPUT_DEPTH bits with TPDF
   dither (dither.h).  A multichannel one, downmixed, is reduced to
   FLAC_DOWNMIX_DEPTH first, which the downmix sums hold without overflow. */
#define FLAC_MAX_BPS       32
#define FLAC_DOWNMIX_DEPTH 24

enum decorrelation_type {
    INDEPENDENT,
    LEFT_SIDE,
    RIGHT_SIDE,
    MID_SIDE,
};

typedef struct FLACContext {
    GetBitContext gb;

    int min_blocksize, max_blocksize;
    int min_framesize, max_framesize;
    int samplerate, channels;
    int blocksize/*, last_blocksize*/;
    int bps;
    unsigned long samplenumber;
    unsigned long totalsamples;
    enum decorrelation_type ch_mode;

    int filesize;
    int length;
    int bitrate;
    int metadatalength;

    int bitstream_size;
    int bitstream_index;

    int sample_skip;
    int framesize;

    int32_t *decoded[MAX_CHANNELS];

    /* #318: max_blocksize 33-bit samples for a 32-bit stereo stream's side
       channel (the caller allocates it; NULL for any other stream: a frame
       that needs it without one is an error), which channel of the current
       frame went there (-1: none), and the dither generator's state */
    int64_t *wide;
    int wide_ch;
    uint32_t dither;
} FLACContext;

int flac_decode_frame(FLACContext *s,
                      uint8_t *buf, int buf_size,
                      void (*yield)(void)) ICODE_ATTR_FLAC;

/* #318: after flac_decode_frame(), a stereo or mono frame wider than
   FLAC_OUTPUT_DEPTH is still at its own width (what a reference decoder
   gives); this reduces it to FLAC_OUTPUT_DEPTH bits with TPDF dither.  It
   does nothing to any other frame.  Call it once a frame, on every frame
   that is played, in order (the dither's sequence is the stream's). */
void flac_reduce_frame(FLACContext *s);

#endif
