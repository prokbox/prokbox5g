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

/* tests/fatcrash/armrun/armrt.c: the smallest runtime a host test needs to
 * run as a bare ARM program under qemu-system-arm with semihosting, so it
 * is compiled by the firmware's own compiler and flags (arm-none-eabi-gcc,
 * -Os, -mcpu=arm7tdmi, ARM state) rather than the host's.  Used by
 * tests/fatcrash/run.sh for dcidx.c (#355), whose bug only the 32-bit
 * target compile shows.  No libc: start-up, mem*, str*, a printf for
 * %d %u %lu %s %c, and exit through semihosting. */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

int main(void);

/* ARM-state semihosting call (qemu: -semihosting) */
static int semihost(int op, const void *arg)
{
    register int r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile ("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory", "lr");
    return r0;
}

void armrt_exit(int status)
{
    /* SYS_EXIT: ApplicationExit -> qemu exits 0, anything else 1 */
    semihost(0x18, (const void *)(status ? 0x20023 : 0x20026));
    for (;;)
        ;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q)
        while (n--)
            *p++ = *q++;
    else
        while (n--)
            p[n] = q[n];
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static char outbuf[256];
static size_t outlen;

static void out_flush(void)
{
    outbuf[outlen] = 0;
    semihost(0x04, outbuf);  /* SYS_WRITE0 */
    outlen = 0;
}

static void out_c(char c)
{
    if (outlen == sizeof outbuf - 1)
        out_flush();
    outbuf[outlen++] = c;
    if (c == '\n')
        out_flush();
}

static void out_u(unsigned long v, int width)
{
    char t[12];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (width-- > n)
        out_c(' ');
    while (n)
        out_c(t[--n]);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            out_c(*fmt);
            continue;
        }
        int width = 0, lng = 0;
        while (fmt[1] >= '0' && fmt[1] <= '9')
            width = width * 10 + (*++fmt - '0');
        if (fmt[1] == 'l')
        {
            lng = 1;
            fmt++;
        }
        switch (*++fmt)
        {
        case 'd': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0)
            {
                out_c('-');
                v = -v;
            }
            out_u((unsigned long)v, width);
            break;
        }
        case 'u':
            out_u(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), width);
            break;
        case 's':
            for (const char *s = va_arg(ap, const char *); *s; s++)
                out_c(*s);
            break;
        case 'c':
            out_c((char)va_arg(ap, int));
            break;
        case '%':
            out_c('%');
            break;
        default:
            out_c('?');
            break;
        }
    }
    va_end(ap);
    return 0;
}

extern char __bss_start__[], __bss_end__[];

void armrt_start(void)
{
    memset(__bss_start__, 0, (size_t)(__bss_end__ - __bss_start__));
    armrt_exit(main());
}

/* entry: a stack, then C */
__asm__(
    "    .pushsection .text.start,\"ax\"\n"
    "    .global _start\n"
    "    .arm\n"
    "_start:\n"
    "    ldr sp, =0x00800000\n"
    "    bl armrt_start\n"
    "1:  b 1b\n"
    "    .ltorg\n"
    "    .popsection\n"
);
