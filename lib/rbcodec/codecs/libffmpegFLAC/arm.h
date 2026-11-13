/* Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md. */
#ifndef _FLAC_ARM_H
#define _FLAC_ARM_H

#include "bitstream.h"

void lpc_decode_arm(int blocksize, int qlevel, int pred_order, int32_t* data, int* coeffs);

/* The 64-bit LPC filter, the wide path of decode_subframe_lpc.  Both have the
   same contract as flac_lpc_32_c in decoder.c.  The _narrow form uses the
   ARMv5E packed 16-bit multiplies and is only valid when every history sample
   fits a signed halfword, which bps <= 16 guarantees. */
void flac_lpc_32_arm(int32_t *decoded, int coeffs[], int pred_order,
                     int qlevel, int len);
void flac_lpc_32_arm_narrow(int32_t *decoded, int coeffs[], int pred_order,
                            int qlevel, int len);

/* Rice residuals of one partition with parameter k: decodes up to count of
   them into out[], advances gb, and returns how many it decoded.  It stops
   in front of a code it leaves to C (see arm.S), so the caller decodes one
   with get_sr_golomb_flac() and calls again for the rest. */
int flac_rice_arm(GetBitContext *gb, int32_t *out, int count, int k);

/* flac_decode_frame()'s stereo decorrelation and scaling for the first
   n & ~3 samples; returns that count (the caller does the rest). */
/* OR of (p[i] + bias) over n samples: lpc_analyze_remodulate()'s range
   check (any sample outside bps bits sets a bit at or above bit bps). */
unsigned flac_range_or_arm(const int32_t *p, int n, int bias);

int flac_decorrelate_arm(int32_t *ch0, int32_t *ch1, int n, int mode,
                         int scale);

#endif
