# USB session catalog (all saved device logs, to 2026-10-18)

Generated from the backup folder (read only) with `scripts/usb_sessions.py`, deduplicated by build, boot and exit counters. Companion to [`usb-speed.md`](usb-speed.md). Dates are the boot RTC, not the session start. Inferences are marked.


Source: 439 `ipodui.log*` files in the backup folder  (read only), deduplicated to 104 unique USB sessions by (log sequence number of the session's `usb cmd` or `usb exit` line, its counters). Verdicts come from `scripts/usb_sessions.py` (`verdict()`), run on each session block. Facts are from the logs; anything marked **(inference)** is not.

## Reading notes and caveats
- **Date/time is the boot rtc** (when the firmware booted), not the USB session time. Several sessions share one boot; the session is some time after. Sessions from older logs without a `boot build=` line show the backup directory name instead and have no build sha (all of them pre-2026-10-15, and no USB counters beyond secs/rd/wr/READs).
- **rd/wr MB** are from `usb exit`. From build `ad29aae06d` (second session) on, the `usb enter/release/exit` lines are often dropped (`log dropped=N` in the log); those rows show `-` for secs and rd/wr, and the verdict adds `NO-EXIT-LINES`. They still have cmd, ms, mnt, rst, rbits, hand lines.
- **"rd30/30s MB/s"** = `mnt rd30` (KB sent in the first 30 s after SET_CONFIGURATION/first READ) divided by 30 and 1024. It is the best available mount-phase rate, but it also counts any bulk copy the host started in that window, so a high value can mean the mount finished fast, not that the mount scan itself ran fast. **(inference)**. `t18` (MB/s over the first 18 s, from the `mnt t18xxx` snapshot) exists only for builds from `ad29aae06d` onward.
- `rd MB / secs` is NOT used as a rate (idle time dominates, e.g. 4069 s and 5101 s sessions).
- **hand ack ms** = `usb hand ... ack=` (storage handover granted, ms from USB-screen entry; n = acks). The log line truncates after ~64 chars, so held-CBW fields (`held`, `holdmax`) are mostly cut off. A handover above ~45 s is the tagcache-scan hold (fault "54 s handover", fixed in `0ded0466a6`/`248ee66c81`).
- **cfg2rd** = ms from SET_CONFIGURATION to first READ. **rd2fat** = ms from first READ to first READ past the first FAT. Values of ~5-14 s in cfg2rd mean the host's first INQUIRY/TUR failed or timed out and macOS retried (**inference**); 1-3 s is a normal first command.
- Symptoms: `stalls N` = count of the 8 logged reset events (`rbits`) with `e20 t80` (and `ov80`, i.e. the bulk-IN dTD primed/active, ~304 ms silent, then host reset: "fault A"); `idle-rst N` = events with `e00` and a long idle (> 5000 in `i`); `cmdstat` = `op:fail/stall/rst` (ff = reset at idle/unknown opcode, 28 = READ(10) interrupted, 00 = TUR fail, 03 = REQUEST SENSE). Only the first 8 reset events per session are logged, so "stalls 3-4, idle-rst 3-4" means all 8 slots were used and the true count is larger (see rst bulk/bus).
- Confound on every row: the macOS helper (`mount_msdos` via launchd, polls every 3 s, sleeps 27 s after one failure) can mount manually while the native mount is retrying; the logs do not show who mounted. No row can be called a native success from device logs alone.

## 1. Per-session table (chronological)

| # | boot rtc (file if none) | build | secs | rd/wr MB | READs | rd30/30s MB/s (pf ring KB) | rst bulk/bus | hand ack ms | cfg2rd ms | rd2fat | rd30 KB; t18 MB/s | wrfail | verdict | symptoms |
| 1 | file 20261010-225157 | ? | 153 | -/- | - | - (ring -) | -/- | - | - | - | -; t18 - | - | DEAF | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 2 | file 20261011-091214 | ? | 485 | -/- | - | - (ring -) | -/- | - | - | - | -; t18 - | - | DEAF | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 3 | file 20261011-091214 | ? | 245 | -/- | - | - (ring -) | -/- | - | - | - | -; t18 - | - | DEAF | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 4 | file 20261011-091214 | ? | 621 | -/- | - | - (ring -) | -/- | - | - | - | -; t18 - | - | DEAF | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 5 | file 20261011-114507 | ? | 322 | 17/2 | 293 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 6 | file 20261011-214314 | ? | 199 | 826/0 | 32310 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 7 | file 20261011-214314 | ? | 300 | 827/292 | 32344 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 8 | file 20261012-063916 | ? | 329 | 174/0 | 6913 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 9 | file 20261012-063916 | ? | 1433 | 555/443 | 15044 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 10 | file 20261012-233540-pre-s10 | ? | 2448 | 1034/13 | 23075 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=1 |
| 11 | file 20261013-080112-pre-s11 | ? | 1 | 0/0 | 0 | - (ring -) | -/- | - | - | - | -; t18 - | - | DEAF | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 12 | file 20261013-080112-pre-s11 | ? | 232 | 569/0 | 19651 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=1 |
| 13 | file 20261013-090325-pre-s12d | ? | 207 | 326/0 | 11339 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=1 |
| 14 | file 20261013-093546-pre-s12e | ? | 331 | 518/0 | 27232 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 15 | file 20261013-135927-pre-s12o | ? | 398 | 811/0 | 32214 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 16 | file 20261013-183354-pre-s12v | ? | 394 | 0/0 | 0 | - (ring -) | -/- | - | - | - | -; t18 - | - | DEAF | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 17 | file 20261013-194500-pre-s12x | ? | - | -/- | 28436 | - (ring -) | -/- | - | - | - | -; t18 - | - | NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 18 | file 20261013-194500-pre-s12x | ? | 145 | 81/0 | 5505 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 19 | file 20261013-205454-pre-s12z | ? | 170 | 1045/15 | 23073 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 20 | file 20261014-073813-pre-morning | ? | 6787 | 768/998 | 31541 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 21 | file 20261015-105759-pre-strip | ? | 1401 | 1356/18 | 41091 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 22 | 2026-10-15 14:01:18 | 1cb5e0ae3a | 338 | 617/13 | 16466 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=56490 n=741; ejected=1 |
| 23 | 2026-10-15 14:07:03 | a045339424 | 514 | 599/12 | 15924 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=1 |
| 24 | 2026-10-15 14:30:36 | acf129664c | 287 | 336/0 | 11445 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 25 | 2026-10-15 14:30:36 | acf129664c | 315 | 997/0 | 22180 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 26 | 2026-10-15 15:07:26 | acf129664c | 152 | 1070/13 | 36339 | - (ring -) | -/- | - | - | - | -; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 27 | 2026-10-15 15:10:05 | ad2950d141 | 215 | 179/0 | 5510 | 3.0 (ring 8192) | -/- | - | 2233 | 101 | 93252; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=56870 n=741; ejected=0 |
| 28 | 2026-10-15 15:14:45 | ad2950d141 | 259 | 610/0 | 12776 | 2.8 (ring 8192) | -/- | - | 2031 | 253 | 84920; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=60600 n=741; ejected=1 |
| 29 | 2026-10-15 15:14:45 | ad2950d141 | 182 | 18/0 | 1629 | 0.6 (ring 8192) | -/- | - | 8247 | 145 | 19075; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=61880 n=741; ejected=0 |
| 30 | 2026-10-15 15:22:36 | ad2950d141 | 170 | 90/0 | 4607 | 2.4 (ring 8192) | -/- | - | 2029 | 67 | 73112; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=56960 n=741; ejected=0 |
| 31 | 2026-10-15 15:28:50 | 2c99312a17 | 184 | 512/0 | 11238 | 2.2 (ring 8192) | -/- | - | 2027 | 57 | 67132; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=97120 n=741; ejected=0 |
| 32 | 2026-10-15 15:28:50 | 2c99312a17 | 94 | 46/0 | 3325 | 1.5 (ring 8192) | -/- | - | 9113 | 57 | 45978; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=60630 n=741; ejected=0 |
| 33 | 2026-10-15 15:28:50 | 2c99312a17 | 160 | 7/0 | 596 | 0.2 (ring 8192) | -/- | - | 9359 | 45 | 7665; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=59490 n=741; ejected=0 |
| 34 | 2026-10-15 17:19:54 | 2c99312a17 | 317 | 277/0 | 18656 | 1.7 (ring 8192) | -/- | - | 2287 | 57 | 52074; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=56340 n=741; ejected=0 |
| 35 | 2026-10-15 17:19:54 | 2c99312a17 | 64 | 0/0 | 0 | - (ring 8192) | -/- | - | 0 | 0 | 0; t18 - | - | DEAF | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=59130 n=741; ejected=0 |
| 36 | 2026-10-15 17:26:43 | 2c99312a17 | 58 | 0/0 | 22 | 0.0 (ring 8192) | -/- | - | 11959 | 57 | 62; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=64450 n=741; ejected=0 |
| 37 | 2026-10-16 08:58:36 | 2c99312a17 | 161 | 479/0 | 10884 | 2.3 (ring 8192) | -/- | - | 2025 | 89 | 69856; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=58070 n=741; ejected=0 |
| 38 | 2026-10-16 09:01:37 | 2c99312a17 | 126 | 331/0 | 10283 | 3.1 (ring 8192) | -/- | - | 2227 | 67 | 94608; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=56570 n=741; ejected=0 |
| 39 | 2026-10-16 09:17:02 | 11e7c83bed | - | -/- | 5077 | 2.8 (ring 8192) | 1/52 | - | 2029 | 77 | 85208; t18 - | 0 | RESET(53) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 40 | 2026-10-16 09:17:02 | 11e7c83bed | 850 | 533/0 | 8844 | 4.6 (ring 8192) | 1/219 | - | 10773 | 99 | 140098; t18 - | 0 | RESET(220) | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 41 | 2026-10-16 09:17:02 | 11e7c83bed | 402 | 532/0 | 12645 | 1.9 (ring 8192) | 1/108 | - | 8349 | 67 | 58879; t18 - | 0 | RESET(109) | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 42 | 2026-10-16 09:17:02 | 11e7c83bed | 25 | 0/0 | 0 | - (ring 8192) | 1/3 | - | 0 | 0 | 0; t18 - | 0 | DEAF RESET(4) | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=88990 n=741; ejected=0 |
| 43 | 2026-10-16 11:47:02 | 11e7c83bed | 305 | 588/0 | 24680 | 1.9 (ring 8192) | 0/314 | - | 1587 | 139 | 56922; t18 - | 0 | RESET(314) | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 44 | 2026-10-16 11:47:02 | 11e7c83bed | 274 | 1065/13 | 19841 | 1.7 (ring 8192) | 0/384 | - | 5469 | 67 | 53324; t18 - | 0 | RESET(384) | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 45 | 2026-10-16 12:12:00 | be80fc40cc | - | -/- | 0 | - (ring 8192) | 1/6 | - | 0 | 0 | 0; t18 - | 0 | RESET(7) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 46 | 2026-10-16 12:12:00 | be80fc40cc | - | -/- | 8207 | 3.5 (ring 8192) | 0/34 | - | 10617 | 67 | 106938; t18 - | 0 | RESET(34) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 47 | 2026-10-16 12:48:10 | be80fc40cc | - | -/- | 3540 | 1.7 (ring 8192) | 0/17 | - | 1657 | 77 | 51338; t18 - | 0 | RESET(17) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 48 | 2026-10-16 12:48:10 | be80fc40cc | - | -/- | 31617 | 2.6 (ring 8192) | 0/222 | - | 7907 | 57 | 79466; t18 - | 0 | RESET(222) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 49 | 2026-10-16 13:55:11 | 7167f5ddc7 | 342 | 250/0 | 10379 | 3.9 (ring 8192) | 1/30 | - | 2033 | 77 | 119040; t18 - | 0 | RESET(31) | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 50 | 2026-10-16 13:55:11 | 7167f5ddc7 | 226 | 624/12 | 15109 | 3.7 (ring 8192) | 1/118 | - | 8621 | 67 | 113260; t18 - | 0 | RESET(119) | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=1 |
| 51 | 2026-10-16 14:04:55 | 26e13c10fd | 480 | 1848/0 | 25200 | 2.3 (ring 8192) | -/- | - | 5799 | 67 | 69656; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=70330 n=741; ejected=0 |
| 52 | 2026-10-16 14:04:55 | 26e13c10fd | 303 | 65/0 | 4249 | 2.2 (ring 8192) | 1/39 | - | 10247 | 57 | 67180; t18 - | 0 | RESET(40) | stalls 3, idle-rst 4; cmdstat ; scan:db scan ms=60640 n=741; ejected=0 |
| 53 | 2026-10-16 14:23:05 | 26e13c10fd | 170 | 303/0 | 8162 | 2.7 (ring 8192) | 1/84 | - | 2031 | 57 | 81764; t18 - | 0 | RESET(85) | stalls 3, idle-rst 4; cmdstat ; scan:db scan ms=58210 n=741; ejected=0 |
| 54 | 2026-10-16 14:43:08 | ea36e78e09 | 106 | 43/0 | 3416 | 1.5 (ring 8192) | -/- | - | 2689 | 45 | 44728; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=62550 n=741; ejected=0 |
| 55 | 2026-10-16 14:43:08 | ea36e78e09 | 36 | 0/0 | 0 | - (ring 8192) | 1/4 | - | 0 | 0 | 0; t18 - | 0 | DEAF RESET(5) | stalls 0, idle-rst 3; cmdstat ff:0/0/4; scan:db scan ms=61930 n=741; ejected=0 |
| 56 | 2026-10-16 14:43:08 | ea36e78e09 | 108 | 0/0 | 0 | - (ring 8192) | 1/6 | - | 0 | 0 | 0; t18 - | 0 | RESET(7) | stalls 1, idle-rst 4; cmdstat ff:0/0/5 12:0/0/1; scan:db scan ms=61930 n=741; db scan ms=51650 n=741; ejected=0 |
| 57 | 2026-10-16 14:43:08 | ea36e78e09 | 446 | 366/0 | 9571 | 4.2 (ring 8192) | 1/93 | - | 5227 | 241 | 128364; t18 - | 0 | RESET(94) | stalls 4, idle-rst 3; cmdstat ; scan:db scan ms=108000 n=741; ejected=0 |
| 58 | 2026-10-16 14:43:08 | ea36e78e09 | 60 | 3/0 | 318 | 0.1 (ring 8192) | 1/9 | - | 2027 | 57 | 3535; t18 - | 0 | RESET(10) | stalls 3, idle-rst 4; cmdstat ff:0/0/5 28:0/0/4; scan:db scan ms=92010 n=741; ejected=0 |
| 59 | 2026-10-16 14:43:08 | ea36e78e09 | 136 | 466/0 | 12056 | 4.5 (ring 8192) | 1/96 | - | 7217 | 57 | 137576; t18 - | 0 | RESET(97) | stalls 3, idle-rst 4; cmdstat ff:0/0/5 28:0/0/91; scan:db scan ms=57630 n=741; ejected=0 |
| 60 | 2026-10-16 15:21:14 | f4efa21c2a | 106 | 113/0 | 5155 | 3.6 (ring 8192) | -/- | - | 10465 | 67 | 111620; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=60190 n=741; ejected=0 |
| 61 | 2026-10-16 15:21:14 | f4efa21c2a | 306 | 1661/0 | 20627 | 4.7 (ring 8192) | 1/223 | - | 8235 | 57 | 143156; t18 - | 0 | RESET(224) | stalls 3, idle-rst 4; cmdstat ff:0/0/5 28:0/0/219; scan:db scan ms=58790 n=741; ejected=0 |
| 62 | 2026-10-16 15:21:14 | f4efa21c2a | 621 | 775/0 | 28213 | 5.0 (ring 8192) | 1/128 | - | 7515 | 111 | 154751; t18 - | 0 | RESET(129) | stalls 3, idle-rst 4; cmdstat ff:0/0/5 28:0/0/123; scan:db scan ms=59010 n=741; ejected=0 |
| 63 | 2026-10-16 15:44:30 | 70aa7ef1e4 | 529 | 105/0 | 4968 | 3.2 (ring 8192) | -/- | - | 7731 | 57 | 97956; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=145960 n=741; ejected=0 |
| 64 | 2026-10-16 16:12:03 | 0ded0466a6 | 416 | 1218/1 | 17027 | 5.5 (ring 8192) | -/- | - | 4157 | 57 | 167976; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=67650 n=741; ejected=1 |
| 65 | 2026-10-16 16:12:03 | 0ded0466a6 | 481 | 26/0 | 2311 | 0.9 (ring 8192) | 1/9 | 61640 ms (n=7) | 6421 | 57 | 27439; t18 - | 0 | RESET(10) | stalls 3, idle-rst 4; cmdstat ff:0/0/5 28:0/0/5; scan:db scan ms=62850 n=741; ejected=0 |
| 66 | 2026-10-16 16:27:46 | 0ded0466a6 | 131 | 481/0 | 12078 | 3.5 (ring 8192) | -/- | - | 1551 | 57 | 107832; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 67 | 2026-10-16 16:36:01 | 0ded0466a6 | 296 | 699/0 | 14442 | 3.1 (ring 8192) | -/- | - | 10243 | 57 | 96368; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=76000 n=741; ejected=0 |
| 68 | 2026-10-16 16:46:59 | 0ded0466a6 | 117 | 161/0 | 6069 | 3.2 (ring 8192) | -/- | - | 11515 | 99 | 97144; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=57790 n=741; ejected=0 |
| 69 | 2026-10-16 16:59:52 | 0ded0466a6 | 122 | 372/0 | 11222 | 4.1 (ring 8192) | -/- | - | 3041 | 67 | 125416; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=69570 n=741; ejected=0 |
| 70 | 2026-10-16 16:59:52 | 0ded0466a6 | 608 | 412/1 | 6930 | 5.6 (ring 8192) | 1/73 | - | 6827 | 57 | 173156; t18 - | 0 | RESET(74) | stalls 3, idle-rst 4; cmdstat ff:0/0/5 28:0/0/67 00:7/0/0 03:; scan:db scan ms=59300 n=741; ejected=1 |
| 71 | 2026-10-16 17:12:39 | 073c8e4366 | 376 | 149/0 | 3387 | 5.0 (ring 8192) | 1/25 | 53950 ms (n=7) | 10135 | 67 | 153140; t18 - | 0 | RESET(26) | stalls 4, idle-rst 3; cmdstat ff:0/0/4 28:0/0/22; scan:db scan ms=59450 n=741; ejected=0 |
| 72 | 2026-10-16 17:19:19 | 073c8e4366 | 48 | 0/0 | 0 | - (ring 8192) | -/- | - | 0 | 0 | 0; t18 - | - | DEAF | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=0 |
| 73 | 2026-10-16 17:26:03 | 073c8e4366 | 351 | 1293/0 | 17245 | 7.6 (ring 8192) | -/- | - | 11093 | 67 | 231988; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=60280 n=741; ejected=1 |
| 74 | 2026-10-16 17:26:03 | 073c8e4366 | 141 | 409/0 | 6700 | 9.5 (ring 8192) | 1/48 | 60770 ms (n=7) | 5657 | 67 | 291774; t18 - | 0 | RESET(49) | stalls 3, idle-rst 4; cmdstat ff:0/0/5 28:0/0/44; scan:db scan ms=59690 n=741; ejected=0 |
| 75 | 2026-10-16 18:46:12 | 073c8e4366 | 457 | 2214/1 | 22374 | 10.1 (ring 8192) | 2/46 | - | 11777 | 65 | 311350; t18 - | 0 | RESET(48) | stalls 4, idle-rst 3; cmdstat ff:0/0/4 28:0/0/42 00:19/0/0 03; scan:db scan ms=56380 n=741; ejected=1 |
| 76 | 2026-10-16 18:53:58 | 050d0d9cf9 | 137 | 348/0 | 10644 | 7.1 (ring 8192) | 1/8 | 54740 ms (n=7) | 10943 | 43 | 218016; t18 - | 0 | RESET(9) | stalls 3, idle-rst 3; cmdstat ff:0/0/4 28:0/0/4; scan:db scan ms=58790 n=741; ejected=0 |
| 77 | 2026-10-16 18:53:58 | 050d0d9cf9 | 383 | 1151/0 | 34537 | 5.9 (ring 8192) | 3/14 | 62340 ms (n=7) | 7297 | 39 | 180522; t18 - | 0 | RESET(17) | stalls 3, idle-rst 4; cmdstat ff:0/0/5 28:0/0/12; scan:db scan ms=65600 n=741; ejected=0 |
| 78 | 2026-10-16 21:31:39 | 050d0d9cf9 | 34 | 0/0 | 0 | - (ring 8192) | 1/4 | - | 0 | 0 | 0; t18 - | 0 | DEAF RESET(5) | stalls 0, idle-rst 3; cmdstat ff:0/0/4; scan:; ejected=0 |
| 79 | 2026-10-16 21:35:58 | 953be4b67a | 1371 | 2478/12 | 30490 | 9.3 (ring 8192) | 1/5 | - | 11999 | 33 | 285814; t18 - | 0 | RESET(6) | stalls 0, idle-rst 3; cmdstat ff:0/0/4 00:6/0/0 03:0/0/1; scan:db scan ms=56080 n=741; ejected=1 |
| 80 | 2026-10-16 21:59:00 | bcb3e8d326 | 551 | 554/11 | 15180 | 9.3 (ring 8192) | 1/6 | - | 10901 | 29 | 285730; t18 - | 0 | RESET(7) | stalls 0, idle-rst 4; cmdstat ff:0/0/5 00:40/0/0 03:0/0/1; scan:db scan ms=69120 n=741; ejected=1 |
| 81 | 2026-10-16 22:08:20 | ea10f38c9f | 128 | 422/10 | 14066 | 9.1 (ring 8192) | 1/6 | 56260 ms (n=7) | 2029 | 31 | 280570; t18 - | 0 | RESET(7) | stalls 0, idle-rst 4; cmdstat ff:0/0/5 00:5/0/0 03:0/0/1; scan:db scan ms=58750 n=741; ejected=1 |
| 82 | 2026-10-16 22:10:38 | 5a1d396d87 | 376 | 471/0 | 13590 | 8.5 (ring 8192) | 1/7 | - | 3137 | 33 | 261382; t18 - | 0 | RESET(8) | stalls 0, idle-rst 4; cmdstat ff:0/0/5 00:2/0/0 03:0/0/1; scan:db scan ms=62680 n=741; ejected=1 |
| 83 | 2026-10-16 22:10:38 | 5a1d396d87 | 123 | 357/10 | 8487 | 7.7 (ring 8192) | 1/5 | - | 9303 | 39 | 236628; t18 - | 0 | RESET(6) | stalls 0, idle-rst 3; cmdstat ff:0/0/4 00:6/0/0 03:0/0/1; scan:db scan ms=114950 n=741; ejected=1 |
| 84 | 2026-10-16 22:20:19 | 2cefd681ce | 244 | 470/0 | 13484 | 3.3 (ring 8192) | 1/6 | - | 10683 | 39 | 102883; t18 - | 0 | RESET(7) | stalls 0, idle-rst 3; cmdstat ff:0/0/4 00:3/0/0 03:0/0/1; scan:db scan ms=58180 n=741; ejected=1 |
| 85 | 2026-10-16 22:20:19 | 2cefd681ce | 327 | 422/14 | 14763 | 8.8 (ring 8192) | 1/6 | - | 4503 | 77 | 270442; t18 - | 0 | RESET(7) | stalls 0, idle-rst 3; cmdstat ; scan:db scan ms=60600 n=741; ejected=1 |
| 86 | 2026-10-16 22:30:11 | 2cefd681ce | 123 | 238/0 | 3857 | 8.0 (ring 8192) | 1/5 | 55130 ms (n=7) | 11273 | 37 | 244649; t18 - | 0 | RESET(6) | stalls 0, idle-rst 3; cmdstat ff:0/0/4 2a:0/0/1; scan:db scan ms=57100 n=741; ejected=0 |
| 87 | 2026-10-16 22:30:11 | 2cefd681ce | 140 | 100/0 | 8553 | 3.3 (ring 8192) | 1/6 | 60970 ms (n=7) | 5943 | 39 | 102403; t18 - | 0 | RESET(7) | stalls 0, idle-rst 4; cmdstat ff:0/0/5 28:0/0/1; scan:db scan ms=59660 n=741; ejected=0 |
| 88 | 2026-10-16 22:34:58 | 2cefd681ce | 437 | 424/14 | 14851 | 3.4 (ring 8192) | -/- | - | 11619 | 37 | 103207; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=57650 n=741; ejected=1 |
| 89 | 2026-10-16 22:42:25 | b9d19f7166 | 288 | 425/14 | 14927 | 3.4 (ring 8192) | -/- | - | 11017 | 37 | 103879; t18 - | - | OK | stalls 0, idle-rst 0; cmdstat ; scan:db scan ms=59160 n=741; ejected=1 |
| 90 | 2026-10-16 22:47:26 | cc16c920ec | 5101 | 392/5989 | 14795 | 3.4 (ring 8192) | 1/7 | - | 9947 | 37 | 103327; t18 - | 0 | RESET(8) | stalls 0, idle-rst 3; cmdstat ff:0/0/4 2a:0/0/1; scan:db scan ms=56930 n=741; ejected=0 |
| 91 | 2026-10-17 00:14:54 | cc16c920ec | 1297 | 635/4231 | 30929 | 4.4 (ring 8192) | 0/5 | - | 8627 | 77 | 133642; t18 - | 0 | RESET(5) | stalls 1, idle-rst 0; cmdstat 28:0/0/1 00:1/0/0 03:0/0/1; scan:; ejected=1 |
| 92 | 2026-10-17 08:10:48 | cc16c920ec | 327 | 408/13 | 13952 | 5.8 (ring 8192) | 0/5 | - | 1595 | 45 | 179528; t18 - | 0 | RESET(5) | stalls 1, idle-rst 0; cmdstat 28:0/0/1 00:11/0/0 03:0/0/1; scan:; ejected=1 |
| 93 | 2026-10-17 08:51:24 | 4f03579b41 | 176 | 0/0 | 0 | - (ring 8192) | 1/8 | 145990 ms (n=7) | 0 | 0 | 0; t18 - | 0 | RESET(9) | stalls 1, idle-rst 4; cmdstat ff:0/0/5 12:0/0/1; scan:db scan ms=154000 n=1125; ejected=0 |
| 94 | 2026-10-17 12:34:22 | 2465b3d092 | - | -/- | 11769 | 3.1 (ring 8192) | -/- | - | 1797 | 57 | 94099; t18 - | - | NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 95 | 2026-10-17 12:41:29 | 2465b3d092 | - | -/- | 11107 | 2.8 (ring 8192) | -/- | - | 1433 | 45 | 86599; t18 - | - | NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 96 | 2026-10-17 12:49:05 | ad29aae06d | - | -/- | 11340 | 2.9 (ring 8192) | -/- | - | 1865 | 45 | 89443; t18 - | - | NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ; scan:; ejected=- |
| 97 | 2026-10-17 12:49:05 | ad29aae06d | 4069 | 324/0 | 11149 | 2.8 (ring 8192) | 0/4 | 650 ms (n=7) | 1839 | 33 | 86875; t18 - | 0 | RESET(4) | stalls 0, idle-rst 0; cmdstat 00:6/0/0 03:0/0/1; scan:; ejected=1 |
| 98 | 2026-10-17 14:03:56 | ad29aae06d | - | -/- | 9952 | 2.3 (ring 8192) | 0/5 | - | 4179 | 67 | 70471; t18 3.4 | 0 | RESET(5) NO-EXIT-LINES | stalls 1, idle-rst 0; cmdstat 28:0/0/1 00:4/0/0 03:0/0/1; scan:; ejected=- |
| 99 | 2026-10-17 14:03:56 | ad29aae06d | - | -/- | 55186 | 2.3 (ring 8192) | 0/4 | 670 ms (n=7) | 1793 | 51 | 71323; t18 2.5 | 0 | RESET(4) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat 28:0/0/1; scan:; ejected=- |
| 100 | 2026-10-17 14:36:48 | ad29aae06d | - | -/- | 22373 | 4.4 (ring 8192) | 0/5 | - | 3251 | 69 | 134875; t18 4.2 | 0 | RESET(5) NO-EXIT-LINES | stalls 1, idle-rst 0; cmdstat 28:0/0/1 00:4/0/0 03:0/0/1; scan:; ejected=- |
| 101 | 2026-10-17 14:48:06 | b669d2e493 | - | -/- | 10048 | 2.4 (ring 0) | 0/4 | 2800 ms (n=7) | 3921 | 39 | 73087; t18 2.5 | 0 | RESET(4) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat 00:6/0/0 03:0/0/1; scan:; ejected=- |
| 102 | 2026-10-17 14:48:06 | b669d2e493 | - | -/- | 11778 | 2.3 (ring 0) | 0/4 | 890 ms (n=7) | 2435 | 51 | 71767; t18 2.5 | 0 | RESET(4) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat 00:1/0/0 03:0/0/1; scan:; ejected=- |
| 103 | 2026-10-17 15:15:50 | 7de657f382 | - | -/- | 11220 | 4.5 (ring 0) | 0/4 | 0 ms (n=7) | 1139 | 43 | 138728; t18 2.6 | 0 | RESET(4) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat 00:5/0/0 03:0/0/1; scan:db scan ms=134150 n=1449; ejected=- |
| 104 | 2026-10-17 21:23:19 | 2da4d5b355 | - | -/- | 6264 | 8.0 (ring 0) | 1/4 | - | 13013 | 47 | 244649; t18 7.8 | 0 | RESET(5) NO-EXIT-LINES | stalls 0, idle-rst 0; cmdstat ff:0/0/1 2a:0/0/1; scan:; ejected=- |

(scan: = a `db scan` completion line near the session in the same log, i.e. the library scan ran around USB entry; ms is the scan duration. `ejected=1` means the host ejected cleanly; `0` means the cable was pulled or the session ended without eject, or exit info is missing.)

## 2. Per-build summary

Counts are of unique logged sessions per build (small samples). Median rd30 is in MB per 30 s.

| build | first seen (boot rtc) | n sess | OK | DEAF | RESET | median cfg2rd ms | median rd30 MB (30 s) | max rd30 MB | commit subject |
|---|---|---|---|---|---|---|---|---|---|
| ? | file 20261010-225157 | 21 | 14 | 6 | 0 | - | - | - | (no build tag in these older logs) |
| 1cb5e0ae3a | 2026-10-15 14:01:18 | 1 | 1 | 0 | 0 | - | - | - | ipodui: the video strip slides up on start, pause, skips, held seeks and a wheel touch (owner) |
| a045339424 | 2026-10-15 14:07:03 | 1 | 1 | 0 | 0 | - | - | - | docs: storage-choices: #73's set-change read, same on both, the hdd stall noted |
| acf129664c | 2026-10-15 14:30:36 | 3 | 3 | 0 | 0 | - | - | - | goldens: the developer list's Fit-3 rows; the see-through scroll bar (#74) |
| ad2950d141 | 2026-10-15 15:10:05 | 4 | 4 | 0 | 0 | 2132 | 77 | 91 | Merge branch 'usb-prefetch' into next3 |
| 2c99312a17 | 2026-10-15 15:28:50 | 8 | 7 | 1 | 0 | 2287 | 51 | 92 | playback: the audio thread frees its buffer before parking for USB, so the prefetch ring never waits on it (#81) |
| 11e7c83bed | 2026-10-16 09:17:02 | 6 | 0 | 1 | 6 | 5469 | 57 | 137 | usb: a held SETUP never replays after a new bus reset; the reset log on its own line (review #1, #2) |
| be80fc40cc | 2026-10-16 12:12:00 | 4 | 0 | 0 | 4 | 7907 | 78 | 104 | fat: log the FSInfo free count at boot and USB connect; a developer row counts free space and writes FSInfo back (#81) |
| 7167f5ddc7 | 2026-10-16 13:55:11 | 2 | 0 | 0 | 2 | 5327 | 113 | 116 | fat: log the volume's dirty flags (boot sector 0x41, FAT[1] clean/hard-error bits) at boot and USB connect, read only ( |
| 26e13c10fd | 2026-10-16 14:04:55 | 3 | 1 | 0 | 2 | 5799 | 68 | 80 | usb: a URI with the port out of reset and our address intact is acknowledged, not torn down (HAVE_USB_RESET_GATE) |
| ea36e78e09 | 2026-10-16 14:43:08 | 6 | 1 | 1 | 5 | 3958 | 85 | 134 | usb_storage: SYNCHRONIZE CACHE and eject flush the drive's write cache (ata_flush), FAIL if it fails (review blocker) |
| f4efa21c2a | 2026-10-16 15:21:14 | 3 | 1 | 0 | 2 | 8235 | 140 | 151 | usb: TXFILLTUNING left alone by default; stale completion cleared at prime; handover timing and NAK/frame/OUT fields at |
| 70aa7ef1e4 | 2026-10-16 15:44:30 | 1 | 1 | 0 | 0 | 7731 | 96 | 96 | docs: scope USB evidence by connector path |
| 0ded0466a6 | 2026-10-16 16:12:03 | 7 | 5 | 0 | 2 | 6421 | 105 | 169 | usb: defer tagcache export until disconnect |
| 073c8e4366 | 2026-10-16 17:12:39 | 5 | 1 | 1 | 3 | 10614 | 256 | 304 | docs: preserve E2 display path in USB plan |
| 050d0d9cf9 | 2026-10-16 18:53:58 | 3 | 0 | 1 | 3 | 9120 | 195 | 213 | docs: record PP5022 stream-disable no-op |
| 953be4b67a | 2026-10-16 21:35:58 | 1 | 0 | 0 | 1 | 11999 | 279 | 279 | usb: keep cache flush available without prefetch |
| bcb3e8d326 | 2026-10-16 21:59:00 | 1 | 0 | 0 | 1 | 10901 | 279 | 279 | artcache: quiesce before queued USB handover |
| ea10f38c9f | 2026-10-16 22:08:20 | 1 | 0 | 0 | 1 | 2029 | 274 | 274 | artcache: stop I/O at USB physical attach |
| 5a1d396d87 | 2026-10-16 22:10:38 | 2 | 0 | 0 | 2 | 6220 | 243 | 255 | tagcache: cancel and resume commits for USB handover |
| 2cefd681ce | 2026-10-16 22:20:19 | 5 | 1 | 0 | 4 | 10683 | 101 | 264 | usb: split PP5022 bulk transfers into 4K dTDs |
| b9d19f7166 | 2026-10-16 22:42:25 | 1 | 1 | 0 | 0 | 11017 | 101 | 101 | usb: retry dropped ARC endpoint primes |
| cc16c920ec | 2026-10-16 22:47:26 | 3 | 0 | 0 | 3 | 8627 | 131 | 175 | usb: reset ARC endpoint overlays on configuration |
| 4f03579b41 | 2026-10-17 08:51:24 | 1 | 0 | 0 | 1 | - | - | - | usb: log native mount command timing |
| 2465b3d092 | 2026-10-17 12:34:22 | 2 | 0 | 0 | 0 | 1615 | 88 | 92 | usb: default compatible inquiry buffer and trace mount reads |
| ad29aae06d | 2026-10-17 12:49:05 | 5 | 0 | 0 | 4 | 1865 | 85 | 132 | usb: publish BOT send state before priming IN |
| b669d2e493 | 2026-10-17 14:48:06 | 2 | 0 | 0 | 2 | 3178 | 71 | 71 | usb: preserve mount policy and deadline telemetry |
| 7de657f382 | 2026-10-17 15:15:50 | 1 | 0 | 0 | 1 | 1139 | 135 | 135 | ipodui: add Music hold rebuild |
| 2da4d5b355 | 2026-10-17 21:23:19 | 1 | 0 | 0 | 1 | 13013 | 239 | 239 | ipodui: show rebuild progress |

What each build changed (from commit subjects and docs/usb-history.md) and what the logs show:

- **No sha (pre 10-15):** early firmware with USB stats up to `usb exit s= rd= wr=` only. 6 of 21 sessions DEAF (entered USB, host sent nothing: 10-10 to 10-11, 10-13 08:01, 18:33). No resets/telemetry logged, so these older sessions cannot be graded beyond DEAF vs moved data. Largest transfer 1.36 GB (10-15, `pre-strip`) and 2.5 GB class writes work (e.g. 1034 MB read + 13 MB written in 2448 s). Writes worked.
- **1cb5e0ae3a, a045339424, acf129664c (10-15 morning):** UI/doc commits; same older USB code. All OK, 0 resets in data, no reset telemetry. Writes up to 18 MB, 1.07 GB read in 152 s.
- **ad2950d141 (10-15 15:10, merge of usb-prefetch):** first build with the prefetch ring (`pf ring=8192`) and `mnt`/`rst` counters but resets not yet counted separately (rst fields show none). 4 sessions OK; rd30 ~2.4-3.0 MB/s; cfg2rd 2.0-8.2 s; the scan runs ~57-62 s near each entry.
- **2c99312a17 (10-15 15:28, audio thread frees buffer before USB park):** 8 sessions: 7 OK, 1 DEAF (64 s, 17:19). cfg2rd up to 11.9 s, rd30 0.2-3.1 MB/s (very slow ones: sessions with 596-3325 READs, short).
- **11e7c83bed (10-16 09:17, held SETUP never replays after bus reset; first build that logs reset counts):** every session shows resets: 220, 109, 4 (DEAF), 314, 384 resets. Reset counts of 100+ per session: this is the first time the reset storm (host retrying every ~11.5 s) is visible; it was probably already present before (**inference**).
- **be80fc40cc (FSInfo logging), 7167f5ddc7 (dirty-flag logging):** logging-only changes. Resets 30-384 per session; rd30 2.6-3.9 MB/s.
- **26e13c10fd (URI with port out of reset acknowledged, `HAVE_USB_RESET_GATE`):** 3 sessions: 1 OK (1848 MB read in 480 s), 2 RESET (40, 85). The first logged fault-A signature (`e20 t80 ov80`) appears in the rbits here. Rolled back to this build at 15:05 after the next build's DEAF.
- **ea36e78e09 (SYNCHRONIZE CACHE / eject flush):** 6 sessions, 1 DEAF+RESET, 1 with only a TUR+INQUIRY and no READ ("fault B", dead at enumeration), others RESET 7-97. Highest-variance build. The doc suspects the TXFILLTUNING write in the neighbouring commit.
- **f4efa21c2a (TXFILLTUNING untouched; handover timing field):** first with `usb hand`: handover 54.7-62.3 s (tagcache scanning), 2 of 3 sessions with 128-224 resets; rd30 up to 5.0 MB/s.
- **70aa7ef1e4 (docs only; same firmware as f4efa21c2a family):** 1 OK session, 0 resets, but cfg2rd 7.7 s and a 146 s scan nearby.
- **0ded0466a6 (tagcache export deferred until disconnect):** 7 sessions, 5 OK (0 resets), 2 RESET. Handover on the clean runs is no longer logged as slow (one session at 61.6 s, first run after install, 16:12). Best long-read OK sessions: 1218 MB read in 416 s. Resets only when the owner ran the large reads (73 resets, 608 s).
- **073c8e4366 / 050d0d9cf9 (diagnostic/doc builds, dirty `M`):** 8 sessions, largest rd30 so far (256-304 MB per 30 s, 8-10 MB/s), 2214 MB read in one session (10.1 MB/s over rd30) but 46-49 resets and 1 DEAF (48 s, 17:19) and 1 DEAF+RESET(5) at 21:31. Handover again 54-62 s on these (before the 22:xx tagcache/artcache fixes).
- **953be4b67a, bcb3e8d326, ea10f38c9f, 5a1d396d87 (21:35 to 22:10, flush without prefetch; artcache quiesce; tagcache cancel/resume):** 5 sessions, all with only 5-8 resets (down from dozens), rd30 236-286 MB (7.7-9.3 MB/s), cfg2rd 2-12 s, and no fault-A stalls in the 8 logged reset events (stalls 0; idle e00 resets ~11.5 s remain). Handover ack where logged: 56.3 s (ea10f38c9f, only that one), the others not logged. These are the cleanest-by-reset-count sessions of the 10-16 evening. All with `ring=8192`.
- **2cefd681ce (4 KiB dTDs):** 5 sessions, resets 5-7 each; rd30 fell to ~100 MB (3.3 MB/s) in 3 of 5 sessions and stayed 244-270 in 2. 1 OK. Handover 55-61 s again in 2 of 5 (the ones where a scan was running).
- **b9d19f7166 (prime retry):** 1 session, OK, 0 resets, 425 MB read, 14 MB written; cfg2rd 11 s, rd30 3.4 MB/s.
- **cc16c920ec (overlay reset at configuration):** 3 sessions: RESET 8/5/5, including the 5101 s session with 5989 MB written (largest write on record, wrfail=0) and the 1297 s session 4231 MB written. Writes work. rd30 3.4-5.8 MB/s; `stalls` fell to 0-1.
- **4f03579b41 (native mount command timing logged):** one session: `cfg2inq=89793 inq2data=0`, `inq=1` only, no READ at all, hand ack 145.99 s, scan 154 s on 1125 tracks: INQUIRY reply never retired ("fault B"), 8 bus resets in 176 s.
- **2465b3d092 (static compatible INQUIRY buffer, mount read trace):** 2 sessions, both 0 bulk resets, 4 bus resets (all `d2`/`d0` pattern), `inq2data=1`, cfg2rd 1.4-1.8 s, rd30 86-94 MB (2.8-3.1 MB/s). No fault-A stalls except one `e20 t80`. Handover where logged: 160 ms.
- **ad29aae06d (BOT send state published before priming IN):** 5 sessions, 4 bus resets typical (1 session 5), 0 or 1 bulk reset, handover 530-670 ms or 650 ms, cfg2rd 1.8-4.2 s, rd30 70-135 MB. First clearly clean-handover family; the 4069 s session (host 4035 s) moved 324 MB read and ejected cleanly.
- **b669d2e493 (preserve mount policy and deadline telemetry; `pf ring=0`, serial read-ahead):** 2 sessions, 4 bus resets, no bulk reset, cfg2rd 2.4-3.9 s, rd30 72 MB (~2.4 MB/s; t18 2.5 MB/s), real music writes (19173 and 54564 write commands, `wrfail=0`), handover 890 ms and 2800 ms (artcache last acker at 2.8 s).
- **7de657f382 (Music hold rebuild):** 1 session; `ring=0`, handover 0 ms (main acked), cfg2rd 1.1 s, rd30 138 MB (4.5 MB/s) and t18 2.6 MB/s; scan 134 s of 1449 tracks ran before USB; 4 bus resets.
- **2da4d5b355 (rebuild progress):** 1 session, `ring=0`, **different pattern**: 1 bulk reset (`b1,d0`), cfg2rd 13.0 s (first command late), then rd30 244 MB (8.0 MB/s) and t18 144 MB (7.8 MB/s) with READs of 64 KB (`mnt` at `.../128`) in place of the 12-16 KB mount reads (`/24`, `/32`) seen in all ring=0 sessions at 2.5 MB/s; 28.8 s idle gap (`i28788`) in the last reset event; handover ack 11.45 s (artcache). **(inference)** the 13 s start plus 64 KB reads is consistent with a manual/helper `mount_msdos` winning after a failed native attempt, not a native fast mount.

## 3. Confounding factors per session

- **Library scan during USB entry (db scan lines):** nearly every session from `ad2950d141` to `cc16c920ec` has a `db scan ms=~56000-77000 n=741` near it (scan restarts at boot / on USB entry; tagcache holds the storage handover, the 54-62 s acks above). Extreme: 108 s, 114.9 s, 146 s, 154 s (n=1125, `4f03579b41` session with no READ), 134 s (n=1449 on `7de657f382`). Sessions without a scan nearby are often the OK or low-reset ones (rows with `scan:` empty, e.g. ea36e78e09-era row `2465b3d092`), but the handover fix (`0ded0466a6`) removed the scan from the critical path, so later scans overlap USB only for the art pass.
- **Art passes:** `art pass start total=172..235` follow almost every session (artcache re-runs after disconnect; `art repeat=1`). Artcache was the last handover acker in b669d2e493 (2.8 s) and 2da4d5b355 (11.45 s), so it can still delay the first command.
- **Short or aborted sessions (<=60 s):** row `2c99312a17` 64 s DEAF, `ea36e78e09` 36 s (DEAF+reset), `050d0d9cf9` 34 s (DEAF+reset 5), `073c8e4366` 48 s DEAF, `11e7c83bed` 25 s DEAF+reset. DEAF means no command at all; `rbits` shows `e00` idle resets only (3-4 logged).
- **Very long sessions:** 4069 s, 5101 s, 6787 s (pre-tag), 2478 MB / 1371 s; their idle time dominates `secs`.
- **Early unplug:** `ejected=0` rows are either unplugs or sessions lacking exit lines; 10-17 rows without exit lines cannot be classified (**inference** only from `rst bus=4-5`, all rows `d2,d0` reset pattern: the host reset twice at enumeration and then ran).
- **Helper / manual mount:** not recorded in the iPod logs. Rows with cfg2rd of 8-14 s and tens of READs before the first FAT pass are candidates (e.g. `953be4b67a`, 12.0 s; `2da4d5b355`, 13.0 s). Rows with cfg2rd 1-3 s and `inq2data=1` (2465b3d092, ad29aae06d, b669d2e493, 7de657f382) are consistent with the host's first INQUIRY completing on time. The Mac-side log (`usb_sessions.py --mac`) would be needed to confirm.
- **Dirty builds:** `M` suffix on 073c8e4366M, 050d0d9cf9M, b9d19f7166M, 2465b3d092M, ad29aae06dM, b669d2e493M, 7de657f382M, 2da4d5b355M: working-tree experiments (A/B markers like usb_boost, usb_sdis may have been active; see usb-history). The 16:29 boost A/B and 17:34 sdis A/B correspond to the 073c8e4366M/050d0d9cf9M sessions.

## 4. Best observed results, regressions

- **Highest mount-phase rate (rd30):** `073c8e4366M` 10.1 MB/s (row 2214 MB read, 457 s, 46 resets, session 18:46 boot), 9.5 MB/s (row 17:26), and the 21:35-22:10 builds 953be4b67a / bcb3e8d326 / ea10f38c9f / 5a1d396d87 at 7.7-9.3 MB/s with only 5-8 resets. All with the 8192 KB prefetch ring and overlapped ATA/IN. These do not prove a fast native mount: rd30 includes the host's first big read burst (**inference**).
- **Cleanest sessions (0 resets, OK verdict, good data):** `0ded0466a6` 1218 MB read / 1 MB written in 416 s, 0 resets, ring prefetch, 5.5 MB/s rd30; also its siblings (481 MB, 699 MB, 372 MB sessions). `26e13c10fd` 1848 MB in 480 s, 0 resets. Pre-prefetch `acf129664c`/older builds had 0 logged resets but no telemetry to judge.
- **Fewest resets with real transfer on the final handover/INQUIRY fixes:** `ad29aae06d` family (bus=4, bulk=0, handover 0.5-0.7 s, inq2data=1, cfg2rd 1.8 s) and `b669d2e493` (also real writes 19173 and 54564 commands, wrfail=0), but at 2.4-4.5 MB/s rd30 (ring=0 sessions at ~2.5 MB/s t18, matching the ~2.5 MiB/s number in hw-experiments.md).
- **Cleanest firmware-side start (fastest first command):** `7de657f382` cfg2rd 1139 ms, handover 0 ms; `2465b3d092` cfg2rd 1.43 s.
- **Regressions:**
  - **Fault A (e20/t80/ov80 ~304 ms stall then host reset)** is present in every ring-prefetch build from `26e13c10fd` through `4f03579b41` and `ad29aae06d`/`b669d2e493` (one `e20 t80 ov80` slot per session even in the best ones). It is the stall that costs the 4-5 reset floor.
  - **Fault B (dead at enumeration; inq=1, no READ)** appears in `ea36e78e09` (2 of 6) and `4f03579b41` (1 of 1, inq2data=0): fixed by the static INQUIRY buffer in `2465b3d092` (all 7 sessions from that build onward have inq2data=1).
  - **Reset storm** (dozens to hundreds of resets per session): `11e7c83bed` to `050d0d9cf9`. It collapses to 5-8 resets from `953be4b67a` (21:35), then 4 (bus) from `2465b3d092`.
  - **Handover hold 54-146 s** (tagcache scan): `ad2950d141` to `050d0d9cf9`, and again in `4f03579b41` (146 s); removed (<= 0.9 s typical, 11 s worst) from `ad29aae06d` on.
  - **Throughput regression with serial read-ahead (`pf ring=0`):** `b669d2e493`, `7de657f382` (rd30 2.4-4.5 MB/s, t18 2.5-2.6 MB/s) vs ring builds 7.7-10 MB/s; `2da4d5b355` rd30 8.0 MB/s is the exception noted above.
  - **4 KiB dTDs (`2cefd681ce`)**: rd30 fell back to ~3.3 MB/s in 3 of 5 sessions (vs 7.7-9.3 for the three builds before it), no benefit to reset count.
- **DEAF sessions per build:** none since `2465b3d092` (0 of 10 unique sessions: all have commands). DEAF counts: unknown-pre-tag 6, 2c99312a17 1, 11e7c83bed 1, ea36e78e09 1, 073c8e4366 1, 050d0d9cf9 1.
