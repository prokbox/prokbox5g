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

/* Host test: the awake battery rows' pure logic (tests/pwrawake/run.sh) */
#include <stdio.h>
#include <string.h>
#include "pwr_awake.h"
#include "usbpark_core.h"
#include "pcf_adc_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

#define HZ 100

/* the gate: a token beats the developer mask; a failure beats both */
static void test_gate(void)
{
    uint32_t lg = PWRAW_BIT(PWRAW_LINGER), us = PWRAW_BIT(PWRAW_USBOFF);

    CHECK(pwraw_gate(PWRAW_LINGER, 0, 0, 0) == PWRAW_W_NONE);
    CHECK(pwraw_gate(PWRAW_LINGER, 0, lg, 0) == PWRAW_W_DEV);
    CHECK(pwraw_gate(PWRAW_LINGER, lg, 0, 0) == PWRAW_W_TOKEN);
    CHECK(pwraw_gate(PWRAW_LINGER, lg, lg, 0) == PWRAW_W_TOKEN);
    CHECK(pwraw_gate(PWRAW_LINGER, lg, lg, lg) == PWRAW_W_FAILED);
    CHECK(pwraw_gate(PWRAW_USBOFF, lg, lg, lg) == PWRAW_W_NONE);
    CHECK(pwraw_gate(PWRAW_USBOFF, 0, us, lg) == PWRAW_W_DEV);
    CHECK(pwraw_why_on(PWRAW_W_TOKEN) && pwraw_why_on(PWRAW_W_DEV));
    CHECK(!pwraw_why_on(PWRAW_W_FAILED) && !pwraw_why_on(PWRAW_W_NONE));
    /* developer default: the two rows with no hang risk */
    CHECK(PWRAW_DEV_DEFAULT == (lg | us));
    CHECK(!(PWRAW_DEV_DEFAULT & PWRAW_BIT(PWRAW_LCDLDO)));
    CHECK(!(PWRAW_DEV_DEFAULT & PWRAW_BIT(PWRAW_I2SGATE)));
    CHECK(PWRAW_N <= 8);            /* the force masks are 8 bits */
}

/* AW-2: the charger park */
static void test_park(void)
{
    struct usbpark p;
    long W = USBPARK_WINDOW_MS * HZ / 1000, M = USBPARK_WINDOW_MAX_MS * HZ / 1000;

    CHECK(W > 10 * HZ);             /* past usb_core's 10 s no-host call */
    usbpark_init(&p, W, M);
    CHECK(!usbpark_due(&p, 0));
    /* a charger: VBUS, no host, the window runs out */
    usbpark_vbus(&p, true, 1000);
    CHECK(!usbpark_due(&p, 1000 + W));
    CHECK(usbpark_due(&p, 1000 + W + 1));
    usbpark_parked(&p);
    CHECK(p.state == USBPARK_PARKED && p.parks == 1);
    CHECK(!usbpark_due(&p, 1000 + 2 * W));      /* parked: nothing more */
    /* the cable out, a host in: a host inside the window is never parked */
    usbpark_vbus(&p, false, 5000);
    CHECK(p.state == USBPARK_OFF && !usbpark_reprobe(&p, 5001));
    usbpark_vbus(&p, true, 6000);
    CHECK(!usbpark_host(&p));                   /* not late */
    CHECK(p.state == USBPARK_HOST && !usbpark_due(&p, 6000 + 10 * W));
    /* a re-probe after a park with nobody: parked again, no late host */
    usbpark_vbus(&p, false, 9000);
    usbpark_vbus(&p, true, 10000);
    usbpark_parked(&p);
    CHECK(usbpark_reprobe(&p, 20000));          /* the controller back */
    CHECK(!usbpark_reprobe(&p, 20001));         /* not twice */
    CHECK(p.state == USBPARK_WINDOW && p.reprobes == 1);
    CHECK(usbpark_due(&p, 20000 + W + 1));
    usbpark_parked(&p);
    /* a host that was there all along (the screen woke it into view):
       late, the window doubles; twice in a row gives the park up */
    CHECK(usbpark_reprobe(&p, 30000));
    CHECK(usbpark_host(&p));
    CHECK(p.late_hosts == 1 && p.window == 2 * W && !p.gave_up);
    usbpark_vbus(&p, false, 31000);
    usbpark_vbus(&p, true, 32000);
    CHECK(!usbpark_due(&p, 32000 + W + 1));     /* the wider window */
    CHECK(usbpark_due(&p, 32000 + 2 * W + 1));
    usbpark_parked(&p);
    CHECK(usbpark_reprobe(&p, 50000));
    CHECK(usbpark_host(&p) && p.gave_up);
    CHECK(p.window == M || p.window == 4 * W);
    CHECK(p.window <= M);
    usbpark_vbus(&p, false, 60000);
    usbpark_vbus(&p, true, 61000);
    CHECK(!usbpark_due(&p, 61000 + 100 * W));   /* given up: never */
    /* a host on a VBUS edge's window clears the late run */
    usbpark_init(&p, W, M);
    usbpark_vbus(&p, true, 0);
    usbpark_parked(&p);
    usbpark_reprobe(&p, 10);
    usbpark_host(&p);
    CHECK(p.late_run == 1);
    usbpark_vbus(&p, false, 20);
    usbpark_vbus(&p, true, 30);
    usbpark_host(&p);
    CHECK(p.late_run == 0 && !p.gave_up);
    /* the window is capped */
    usbpark_init(&p, M, M);
    usbpark_vbus(&p, true, 0);
    usbpark_parked(&p);
    usbpark_reprobe(&p, 1);
    usbpark_host(&p);
    CHECK(p.window == M);
}

/* AW-9: the done bit */
struct adc_sim { int calls, ready_at, err_at; };

static int sim_read2(void *ctx, unsigned char *d)
{
    struct adc_sim *s = ctx;

    s->calls++;
    if (s->calls == s->err_at)
        return -1;
    d[0] = 0xAB;
    d[1] = (unsigned char)(0x02 | (s->calls >= s->ready_at ? 0x80 : 0));
    return 0;
}

static void test_adc(void)
{
    struct adc_sim s = { 0, 1, 0 };
    unsigned char d[2];

    CHECK(pcf_adc_wait(sim_read2, &s, d, PCF_ADC_POLLS) == 1);
    CHECK(pcf_adc_value(d) == ((0xAB << 2) | 2));
    s = (struct adc_sim){ 0, 3, 0 };
    CHECK(pcf_adc_wait(sim_read2, &s, d, PCF_ADC_POLLS) == 3 && s.calls == 3);
    /* never ready: bounded, the last read kept (the old behaviour) */
    s = (struct adc_sim){ 0, 99, 0 };
    CHECK(pcf_adc_wait(sim_read2, &s, d, PCF_ADC_POLLS) == -1);
    CHECK(s.calls == PCF_ADC_POLLS && pcf_adc_value(d) == ((0xAB << 2) | 2));
    /* an I2C error is a miss for that read, not a result */
    s = (struct adc_sim){ 0, 1, 1 };
    CHECK(pcf_adc_wait(sim_read2, &s, d, PCF_ADC_POLLS) == 2);
}

int main(void)
{
    test_gate();
    test_park();
    test_adc();
    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    return fails != 0;
}
