#!/usr/bin/env python3
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************

"""Generate the Time Zone city table (stdlib only).

    apps/ipodui/art/tz/tzcities.py        > apps/ipodui/tzcities.h
    apps/ipodui/art/tz/tzcities.py --lang > (phrases for apps/lang/english.lang)

Settings > Date & Time > Time Zone picks a region, then a city
(docs/phase11-brief.md section 3.6, frozen default D4). The table is
APPEND-ONLY (Rev 1 L-5, tzcity.h): a city's row index is its stable id,
which the ipodui_tz_city setting stores. Display order (by region, then
by name) comes from a separate generated order array, so adding cities
never changes what a saved setting means.

To add a city, append a row to CITIES. The generator refuses to run if a
row that apps/ipodui/tzcities.h already has was moved, renamed or
deleted. Row 0 is New York, the default (tzcity.h UI_TZ_CITY_DEFAULT).

Offsets are each city's STANDARD offset from UTC in minutes (no DST row
in Date & Time, as on the 7G's guide). Names are LANG phrases (rule 8),
one per city.

P12 (Q27, owner 2026-10-23): each city also carries one byte, its
daylight-saving rule (tzpick.h enum tz_dst): DST below, by name, so the
append-only rows above stay as they are. A city not listed keeps standard
time all year. Rules as observed in 2026; where a city's own rule differs
from the four in the table, the nearest one stands in (noted per line).
"""
import os
import re
import sys

REGIONS = [
    # (LANG suffix, English name); the region list shows them in this order
    ('AFRICA', 'Africa'),
    ('ASIA', 'Asia'),
    ('ATLANTIC', 'Atlantic'),
    ('AUSTRALIA', 'Australia'),
    ('EUROPE', 'Europe'),
    ('NORTH_AMERICA', 'North America'),
    ('PACIFIC', 'Pacific'),
    ('SOUTH_AMERICA', 'South America'),
]

H = 60
# APPEND ONLY: (English name, region suffix, standard offset in minutes)
CITIES = [
    ('New York', 'NORTH_AMERICA', -5 * H),          # id 0: the default
    ('Anchorage', 'NORTH_AMERICA', -9 * H),
    ('Atlanta', 'NORTH_AMERICA', -5 * H),
    ('Boston', 'NORTH_AMERICA', -5 * H),
    ('Chicago', 'NORTH_AMERICA', -6 * H),
    ('Dallas', 'NORTH_AMERICA', -6 * H),
    ('Denver', 'NORTH_AMERICA', -7 * H),
    ('Detroit', 'NORTH_AMERICA', -5 * H),
    ('Edmonton', 'NORTH_AMERICA', -7 * H),
    ('Halifax', 'NORTH_AMERICA', -4 * H),
    ('Houston', 'NORTH_AMERICA', -6 * H),
    ('Los Angeles', 'NORTH_AMERICA', -8 * H),
    ('Mexico City', 'NORTH_AMERICA', -6 * H),
    ('Miami', 'NORTH_AMERICA', -5 * H),
    ('Montreal', 'NORTH_AMERICA', -5 * H),
    ('Phoenix', 'NORTH_AMERICA', -7 * H),
    ('Regina', 'NORTH_AMERICA', -6 * H),
    ('San Francisco', 'NORTH_AMERICA', -8 * H),
    ('Seattle', 'NORTH_AMERICA', -8 * H),
    ("St. John's", 'NORTH_AMERICA', -3 * H - 30),
    ('Toronto', 'NORTH_AMERICA', -5 * H),
    ('Vancouver', 'NORTH_AMERICA', -8 * H),
    ('Washington, D.C.', 'NORTH_AMERICA', -5 * H),
    ('Winnipeg', 'NORTH_AMERICA', -6 * H),
    ('Bogotá', 'SOUTH_AMERICA', -5 * H),
    ('Buenos Aires', 'SOUTH_AMERICA', -3 * H),
    ('Caracas', 'SOUTH_AMERICA', -4 * H),
    ('Lima', 'SOUTH_AMERICA', -5 * H),
    ('Montevideo', 'SOUTH_AMERICA', -3 * H),
    ('Rio de Janeiro', 'SOUTH_AMERICA', -3 * H),
    ('Santiago', 'SOUTH_AMERICA', -4 * H),
    ('São Paulo', 'SOUTH_AMERICA', -3 * H),
    ('Amsterdam', 'EUROPE', 1 * H),
    ('Athens', 'EUROPE', 2 * H),
    ('Barcelona', 'EUROPE', 1 * H),
    ('Berlin', 'EUROPE', 1 * H),
    ('Brussels', 'EUROPE', 1 * H),
    ('Dublin', 'EUROPE', 0),
    ('Helsinki', 'EUROPE', 2 * H),
    ('Istanbul', 'EUROPE', 3 * H),
    ('Kyiv', 'EUROPE', 2 * H),
    ('Lisbon', 'EUROPE', 0),
    ('London', 'EUROPE', 0),
    ('Madrid', 'EUROPE', 1 * H),
    ('Moscow', 'EUROPE', 3 * H),
    ('Oslo', 'EUROPE', 1 * H),
    ('Paris', 'EUROPE', 1 * H),
    ('Prague', 'EUROPE', 1 * H),
    ('Rome', 'EUROPE', 1 * H),
    ('Stockholm', 'EUROPE', 1 * H),
    ('Vienna', 'EUROPE', 1 * H),
    ('Warsaw', 'EUROPE', 1 * H),
    ('Zürich', 'EUROPE', 1 * H),
    ('Accra', 'AFRICA', 0),
    ('Cairo', 'AFRICA', 2 * H),
    ('Cape Town', 'AFRICA', 2 * H),
    ('Casablanca', 'AFRICA', 1 * H),
    ('Johannesburg', 'AFRICA', 2 * H),
    ('Lagos', 'AFRICA', 1 * H),
    ('Nairobi', 'AFRICA', 3 * H),
    ('Bangkok', 'ASIA', 7 * H),
    ('Beijing', 'ASIA', 8 * H),
    ('Delhi', 'ASIA', 5 * H + 30),
    ('Dhaka', 'ASIA', 6 * H),
    ('Dubai', 'ASIA', 4 * H),
    ('Hong Kong', 'ASIA', 8 * H),
    ('Jakarta', 'ASIA', 7 * H),
    ('Jerusalem', 'ASIA', 2 * H),
    ('Kabul', 'ASIA', 4 * H + 30),
    ('Karachi', 'ASIA', 5 * H),
    ('Kathmandu', 'ASIA', 5 * H + 45),
    ('Manila', 'ASIA', 8 * H),
    ('Mumbai', 'ASIA', 5 * H + 30),
    ('Riyadh', 'ASIA', 3 * H),
    ('Seoul', 'ASIA', 9 * H),
    ('Shanghai', 'ASIA', 8 * H),
    ('Singapore', 'ASIA', 8 * H),
    ('Taipei', 'ASIA', 8 * H),
    ('Tehran', 'ASIA', 3 * H + 30),
    ('Tokyo', 'ASIA', 9 * H),
    ('Adelaide', 'AUSTRALIA', 9 * H + 30),
    ('Brisbane', 'AUSTRALIA', 10 * H),
    ('Darwin', 'AUSTRALIA', 9 * H + 30),
    ('Melbourne', 'AUSTRALIA', 10 * H),
    ('Perth', 'AUSTRALIA', 8 * H),
    ('Sydney', 'AUSTRALIA', 10 * H),
    ('Apia', 'PACIFIC', 13 * H),
    ('Auckland', 'PACIFIC', 12 * H),
    ('Fiji', 'PACIFIC', 12 * H),
    ('Guam', 'PACIFIC', 10 * H),
    ('Honolulu', 'PACIFIC', -10 * H),
    ('Midway', 'PACIFIC', -11 * H),
    ('Azores', 'ATLANTIC', -1 * H),
    ('Bermuda', 'ATLANTIC', -4 * H),
    ('Cape Verde', 'ATLANTIC', -1 * H),
    ('Reykjavík', 'ATLANTIC', 0),
    ('South Georgia', 'ATLANTIC', -2 * H),
]

# The DST rule of every city that observes one (tzpick.h enum tz_dst);
# any other city: TZ_DST_NONE. Name -> rule suffix.
DST = {
    # North America: 2nd Sunday of March -> 1st Sunday of November
    # (Mexico City, Phoenix and Regina keep standard time)
    'New York': 'US', 'Anchorage': 'US', 'Atlanta': 'US', 'Boston': 'US',
    'Chicago': 'US', 'Dallas': 'US', 'Denver': 'US', 'Detroit': 'US',
    'Edmonton': 'US', 'Halifax': 'US', 'Houston': 'US',
    'Los Angeles': 'US', 'Miami': 'US', 'Montreal': 'US',
    'San Francisco': 'US', 'Seattle': 'US', "St. John's": 'US',
    'Toronto': 'US', 'Vancouver': 'US', 'Washington, D.C.': 'US',
    'Winnipeg': 'US', 'Bermuda': 'US',
    # Europe: last Sunday of March -> last Sunday of October, 01:00 UTC
    # (Istanbul and Moscow keep standard time)
    'Amsterdam': 'EU', 'Athens': 'EU', 'Barcelona': 'EU', 'Berlin': 'EU',
    'Brussels': 'EU', 'Dublin': 'EU', 'Helsinki': 'EU', 'Kyiv': 'EU',
    'Lisbon': 'EU', 'London': 'EU', 'Madrid': 'EU', 'Oslo': 'EU',
    'Paris': 'EU', 'Prague': 'EU', 'Rome': 'EU', 'Stockholm': 'EU',
    'Vienna': 'EU', 'Warsaw': 'EU', 'Zürich': 'EU', 'Azores': 'EU',
    # nearest rule: Israel switches the Friday before the EU's Sunday
    'Jerusalem': 'EU',
    # southern hemisphere: 1st Sunday of October -> 1st Sunday of April
    'Adelaide': 'SOUTH', 'Melbourne': 'SOUTH', 'Sydney': 'SOUTH',
    # nearest rule: New Zealand starts on the last Sunday of September,
    # Chile on the first Sunday of September
    'Auckland': 'SOUTH', 'Santiago': 'SOUTH',
}
DST_RULES = ('NONE', 'US', 'EU', 'SOUTH')     # tzpick.h enum tz_dst order

MAX_CITIES = 256                # tzcity.h UI_TZ_CITIES_MAX (uint8_t order)


def lang_id(name):
    ascii_name = (name.replace('á', 'a').replace('ã', 'a').replace('ü', 'u')
                  .replace('í', 'i'))
    return 'LANG_IPODUI_TZ_CITY_' + re.sub(r'[^A-Z0-9]+', '_',
                                           ascii_name.upper()).strip('_')


def check_append_only(path):
    """Refuse a table that moved, renamed or dropped an existing row."""
    if not os.path.exists(path):
        return
    with open(path, encoding='utf-8') as f:
        old = re.findall(r'/\* id (\d+) \*/ \{ (LANG_IPODUI_TZ_CITY_\w+),', f.read())
    for idx, lid in old:
        i = int(idx)
        if i >= len(CITIES) or lang_id(CITIES[i][0]) != lid:
            sys.exit('tzcities.py: row %d (%s) moved, renamed or deleted: '
                     'the table is append-only' % (i, lid))


def emit_header():
    assert CITIES[0][0] == 'New York'
    assert len(CITIES) <= MAX_CITIES
    ids = [lang_id(c[0]) for c in CITIES]
    assert len(set(ids)) == len(ids), 'duplicate city'
    names = set(c[0] for c in CITIES)
    assert all(n in names for n in DST), 'DST names an unknown city'
    reg_index = {r[0]: i for i, r in enumerate(REGIONS)}
    order = sorted(range(len(CITIES)),
                   key=lambda i: (reg_index[CITIES[i][1]],
                                  CITIES[i][0].lower()))
    first = []
    for r in range(len(REGIONS)):
        first.append(next(k for k, i in enumerate(order)
                          if reg_index[CITIES[i][1]] == r))
    first.append(len(order))

    out = []
    p = out.append
    p('/* Generated by apps/ipodui/art/tz/tzcities.py -- do not edit.')
    p(' * The Time Zone cities, APPEND-ONLY: a row index is the stable id')
    p(' * stored in the ipodui_tz_city setting (tzcity.h). */')
    p('#ifndef _IPODUI_TZCITIES_H_')
    p('#define _IPODUI_TZCITIES_H_')
    p('')
    p('#include <stdint.h>')
    p('#include "ipodui/tzcity.h"')
    p('')
    p('#define TZ_REGIONS %d' % len(REGIONS))
    p('#define TZ_CITIES  %d' % len(CITIES))
    p('')
    p('struct tz_city {')
    p('    short lang;                 /* LANG_IPODUI_TZ_CITY_* */')
    p('    short offset;               /* standard offset from UTC, minutes */')
    p('    uint8_t region;             /* index into tz_region_lang */')
    p('    uint8_t dst;                /* enum tz_dst (tzpick.h), P12 Q27 */')
    p('};')
    p('')
    p('static const short tz_region_lang[TZ_REGIONS] = {')
    for suffix, _ in REGIONS:
        p('    LANG_IPODUI_TZ_REGION_%s,' % suffix)
    p('};')
    p('')
    p('static const struct tz_city tz_cities[TZ_CITIES] = {')
    for i, (name, reg, off) in enumerate(CITIES):
        p('    /* id %d */ { %s, %d, %d, %d },' % (
            i, ids[i], off, reg_index[reg],
            DST_RULES.index(DST.get(name, 'NONE'))))
    p('};')
    p('')
    p('/* display order: by region, then by name */')
    p('static const uint8_t tz_order[TZ_CITIES] = {')
    for k in range(0, len(order), 12):
        p('    ' + ' '.join('%d,' % i for i in order[k:k + 12]))
    p('};')
    p('')
    p('/* region r lists tz_order[tz_region_first[r] .. tz_region_first[r+1]-1] */')
    p('static const uint8_t tz_region_first[TZ_REGIONS + 1] = {')
    p('    ' + ' '.join('%d,' % f for f in first))
    p('};')
    p('')
    p('_Static_assert(UI_TZ_CITY_DEFAULT == 0 && TZ_CITIES <= UI_TZ_CITIES_MAX,')
    p('               "tzcities: New York is row 0, the order fits a uint8_t");')
    p('')
    p('#endif /* _IPODUI_TZCITIES_H_ */')
    print(prokbox_header.convert('\n'.join(out), '.h')[0])


def phrase(lid, desc, text):
    return '\n'.join([
        '<phrase>',
        '  id: %s' % lid,
        '  desc: %s' % desc,
        '  user: core',
        '  <source>', '    *: none', '    ipodui: "%s"' % text, '  </source>',
        '  <dest>', '    *: none', '    ipodui: "%s"' % text, '  </dest>',
        '  <voice>', '    *: none', '    ipodui: "%s"' % text, '  </voice>',
        '</phrase>'])


def emit_lang():
    out = []
    for suffix, name in REGIONS:
        out.append(phrase('LANG_IPODUI_TZ_REGION_' + suffix,
                          'iPod UI shell, Time Zone region (P11, tzcities.py)',
                          name))
    for name, _, _ in CITIES:
        out.append(phrase(lang_id(name),
                          'iPod UI shell, Time Zone city (P11, tzcities.py)',
                          name))
    print('\n'.join(out))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    check_append_only(os.path.join(here, '..', '..', 'tzcities.h'))
    if len(sys.argv) > 1 and sys.argv[1] == '--lang':
        emit_lang()
    else:
        emit_header()


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    main()
