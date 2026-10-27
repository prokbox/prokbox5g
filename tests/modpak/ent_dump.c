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
 * tests/modpak helper: reads struct modpak_ent images (as modpak.py writes them)
 * and prints every field, so the Python writer and the C layout round-trip
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
#include <stdio.h>
#include <stddef.h>
#include "ipodui/modload_core.h"

#define F(x) printf("%s %lu %lu\n", #x, (unsigned long)offsetof(struct \
    modpak_ent, x), (unsigned long)sizeof(((struct modpak_ent *)0)->x))

int main(int argc, char **argv)
{
    struct modpak_ent e;
    FILE *f;

    if (argc < 2)
    {
        /* the layout: field offset size */
        printf("size %lu\n", (unsigned long)sizeof(struct modpak_ent));
        F(id); F(offset); F(image); F(bss_off); F(bss); F(nreloc);
        F(nexport); F(fletcher); F(mclass); F(min_ram); F(abi_major);
        F(abi_minor); F(work_kb); F(kind); F(exit_policy); F(borrow_kb);
        return 0;
    }
    f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    while (fread(&e, sizeof(e), 1, f) == 1)
        printf("%lu %lu %lu %lu %lu %lu %lu %lu %u %u %u %u %u %u %u %lu\n",
               (unsigned long)e.id, (unsigned long)e.offset,
               (unsigned long)e.image, (unsigned long)e.bss_off,
               (unsigned long)e.bss, (unsigned long)e.nreloc,
               (unsigned long)e.nexport, (unsigned long)e.fletcher,
               e.mclass, e.min_ram, e.abi_major, e.abi_minor, e.work_kb,
               e.kind, e.exit_policy, (unsigned long)e.borrow_kb);
    fclose(f);
    return 0;
}
