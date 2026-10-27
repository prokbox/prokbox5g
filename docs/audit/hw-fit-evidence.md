# Hardware-fit audit: evidence extracts

Supporting numbers for `docs/hardware-fit-audit.md`, pulled read-only on 2026-10-13 from the device backups in the backup folder and from the owner-variant build `build/fw-ui` (`flash-64-3000-usbc-haptic`, `p9-unified` lineage). Each line says where it came from, so it can be re-run.

## E1. Power log (`power.csv`), battery on (charger = 0), periodic rows only

`awk -F, 'NR>1 && $17=="" && $4==0'` grouped by `play` (column 6); means of `boost_pct`, `bl_pct`, `lcd_pct`, `disk_pct`.

| File | play | rows | boost % | backlight % | LCD % | disk % |
|---|---|---|---|---|---|---|
| `battery-day-20261012-172546/power.csv` | playing | 569 | 5.7 | 8.2 | 27.9 | 11.8 |
| | paused | 42 | 5.9 | 11.4 | 28.6 | 5.7 |
| | stopped | 92 | 16.4 | 32.7 | 42.3 | **95.7** |
| `20261013-211900-pre-s13a/rockbox/power.csv` | playing | 726 | 11.0 | 15.9 | 29.2 | 16.7 |
| | paused | 79 | 10.4 | 18.1 | 22.3 | 16.8 |
| | stopped | 1,535 | 3.1 | 5.1 | 8.2 | **96.2** |

- Second file, stopped rows: `disk_pct` = 100 in **1,466 of 1,535** rows; 0 in 47.
- Second file, playing with the backlight off (562 rows): `boost_pct` = 0 in 530 rows (94 %); mean `disk_pct` 7.6.
- `disk_pct` samples `storage_disk_is_active()` = `ata_state >= ATA_SPINUP` (`firmware/drivers/ata.c:683`), which stays true until `sleep_timeout` (5 s, `ata.c:99`; `storage_spindown()` is only called under `HAVE_DISK_STORAGE`, `apps/settings.c:968`) after the last access.

## E2. Scheduling under video (`bcmtest.log`, all backups, de-duplicated: 31,081 lines)

- `player: clock at …; codec priority P, longest loop gap G ms` (519 rows): longest gap by codec priority: P=10 948 ms, P=12 980 ms, P=13 125 ms, P=14 1,189 ms, P=16 1,379 ms (rows before and after the dev33/dev39 follow and floor fixes are mixed).
- Session 34 A/B matrix (stage 7 steps 9–17, `stage 6 summary` lines): unboosted + CPU copy + floor **PASS, drift 16 ms**; boosted + CPU + floor PASS 17 ms; unboosted + CPU + no floor **FAIL 566 ms**; boosted + CPU + no floor FAIL 70 ms; every DMA row showed 0–53 pictures (DMA into SDRAM landed nothing).
- `video: thread active permille, session s`: 326–825 ‰ in the last eight sessions, while `video: port CPU ms, blocks` was 64–174 ms per 9–12 s session (0.6–1.7 % of wall time). "Active" is wall time between the thread's waits (`apps/video/vplayer.c`, dev41, `busy_us`), so it includes time the thread was runnable but pre-empted: it is not CPU time.

## E3. Start-up and UI latencies (`ipodui.log`, all backups, de-duplicated: 1,148 lines)

- `np art read slow ms=` (a Now Playing cover read over 300 ms): 848, 884, 888 ms at 5–14 s after boot; 439 ms once (`apps/ipodui/screens/nowplaying.c:888`).
- `lib window … total_ms=`: median 5 ms, p90 360 ms, max 410 ms (19 rows).
- `video: first picture (ms after the start, boot ms)`: 327–919 ms (separate-image builds, boot included).
- `audio shrink want=132656` while playing or paused (status 1 and 3): the 128 KB allocation that stops playback to shrink the buffer, seen in 2 boots.

## E4. Build composition (`scripts/sizemap.py build/fw-ui/rockbox.map --elf build/fw-ui/rockbox.elf`)

Image 709,856 B, bss 350,588 B, `_end` 1,000,560 B. By subsystem (image / bss): shell 283,185 / 4,501; rbcodec 80,603 / 6,507; firmware 74,938 / 12,151; devmode-stock-ui 58,440 / 4,673; playback 41,607 / 33,994; storage 34,904 / 67,744; apps-core 28,188 / 2,832; tagcache 25,207 / 19,563; lcd-fonts 22,079 / 177,082; usb-stack 17,321 / 8,102; lang 16,217 / 4,385; unicode-nfc 11,645.

Largest statics (`arm-none-eabi-nm -S --size-sort`): `lcd_static_framebuffer` 153,600; disk cache `cache_buffer` 32,768 + `cache_map_entry` 8,192; `tagcache_stack` 17,408; `lcd_scroll` 13,312; `open_streams` 11,520 + 2,480; `codec_stack` 9,216 (IRAM); `buffering_stack` 9,216; `static_id3_entries` 8,328; `stackbegin` 8,192 (IRAM); `audio_stack` 5,120; `usb_stack` 4,736.

The developer video build `build/fw-bcmfw`: text 811,108, data 8,268, bss 351,324.

## E5. Threads in the shipped shell build (all `IF_COP(, CPU)`)

| Thread | Priority | File |
|---|---|---|
| piezo (haptic) | 4 (`PRIORITY_REALTIME`) | `firmware/target/arm/ipod/piezo.c:190` |
| codec | 16, boosted to 5–14 by pcmbuf | `apps/codec_thread.c:662`, `apps/pcmbuf.c:395` |
| buffering | 15 | `apps/buffering.c:1725` |
| audio | 15 | `apps/audio_thread.c:181` |
| main/UI, storage, backlight, scroll | 16 | `firmware/storage.c:281`, `firmware/backlight.c:803`, `firmware/scroll_engine.c:200` |
| video (dev builds) | 16, follows the codec ≥ 8 for 3 s; floor `PRIORITY_BUFFERING − 1` from dev39; **dev43:** fixed `PRIORITY_VIDEO_FEED` (14), no follow | `apps/video/vplayer.c` (`create_thread`) |
| usb, power | 18 | `firmware/usb.c:753`, `firmware/powermgmt.c:1020` |
| tagcache, artcache | 20 | `apps/tagcache.c:7210`, `apps/ipodui/artcache.c:4504` |

The COP runs only the MP3 decoder's synthesis filter (`lib/rbcodec/codecs/mpa.c:29`, `MPA_SYNTH_ON_COP`) and is woken every tick by the CPU's Timer 1 handler (`firmware/target/arm/pp/kernel-pp.c:36`).
