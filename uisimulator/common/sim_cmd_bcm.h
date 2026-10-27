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


#ifndef SIM_CMD_BCM_H
#define SIM_CMD_BCM_H

#include <stdbool.h>

/* P19 W0-5: the BCM2722 job model's script commands (brief section 3.4).
 * Handles these sim_script.c commands:
 *
 *   bcm_fault clear|slow|fail <op>
 *                      op (0xNN or decimal) is missing from the image,
 *                      takes 40 x its deadline, or fails
 *   bcm_fault ringfull [<op>] <n>
 *                      the next <n> submits (of any op: the model counts
 *                      submits; an <op> before <n> is accepted and
 *                      ignored) answer BUSY
 *   bcm_fault dead|cut the heartbeat stops / the BCM loses power now
 *   bcm_fault none     every fault cleared
 *   bcm_charger 0|1    the charger input of the model's power rules
 *   bcm_advance <ms>   model time moves on by <ms> (it also follows the
 *                      sim's clock, synced at every call into the API)
 *   bcm_selftest       bcmjob_selftest(); its line is in the log
 *   bcm_stats          prints (stdout)
 *                      "bcm stats c0_ms=.. c1_ms=.. c2_ms=.. cold=.. done=..
 *                      epoch=.. dark_ups=.. decode=.. game=.. video=.."
 *   expect_bcm_dark_ups <n>
 *                      "expect_bcm failed" when the power-ups while dark
 *                      (any reason) differ from <n>
 *
 * The model exists only in a build with HAVE_BCM_JOBS_MODEL (the
 * --bcmfw-video simulator); every command there reports "unavailable"
 * in any other.  Returns false for any other command. */
bool sim_cmd_bcm(const char *cmd, char *argv[], int argc, int lineno);

#endif
