> Research record, 2026-10-20 (read-only audit; inputs to docs/memory-audit-2026-10-20.md, docs/design/module-loader.md and docs/hardware-fit-audit.md). Paths to `scratchpad/` refer to the session's scratch area and are not kept. Where it conflicts with the synthesis docs (notably the floor framing: the owner chose option 1, the audio file buffer itself), the synthesis wins.

# Memory audit B: code size (.text / .rodata), fw-ui at eb2f96dea5

Inputs: build/fw-ui/rockbox.elf + rockbox.map in the `headers` worktree (read only), scripts/sizemap.py, scripts/risk/reach.py, tools/thumb-cold.txt, docs/phase8b-brief.md 4.T. Thumb figures were measured by compiling each source twice (ARM and `-mthumb`) with the build's exact flags in the scratchpad (nothing built in the worktree). Scratch data: `scratchpad/thumb.txt`, `thumb1.txt`, `sm.txt`, `nm.txt`, `strs.txt`.

## 0. Owner decision (2026-10-20) and what it changes
Cap is now 1,500,000 B `_end`; `_end` is 1,092,208 B, so there are ~408 KB of headroom and size is no longer the binding constraint (the audio buffer floor is). Consequences for this audit:
- Thumb churn with device-timing risk drops to "only if free" (below).
- Nothing in section 2 is needed for the cap. The only residual reason to do any of it is audio buffer (every resident byte is one fewer buffer byte; on 32 MB units that matters more).
- Large cold rodata that can be loaded from disk on demand is the interesting class: it costs nothing at runtime when unused and needs no timing proof (section 3).
- The page-step note (524 B before the next 4 KiB step) is moot for the cap. For the record: `cbw_buffer` ends 0xfedf4, `inquiry_static` is at 0xff000 (524 B gap going up). Going down, the first 3,572 B saved frees a whole 4,096 B, then 1:1 per 4 KiB.

## 1. Where the bytes are
`_end` = .text 645,696 + .rodata 133,516 + .data 8,472 (= 787,712) + .bss 299,644 + .ncbss 4,852. The sizemap "image" total of 816,612 also counts .init 19,520 (boot logo etc., copied to the audio buffer and freed) and IRAM sections; those are NOT in `_end`. So `bootscreen_data` (11,184) is not a resident cost.

Sizemap subsystems (image / bss): shell 347,059 / 7,749; rbcodec 82,212 / 6,508; firmware 81,581 / 13,029; devmode-stock-ui 57,632 / 1,777 (see caveat); tagcache 45,894 / 20,176; playback 42,212 / 34,043; storage 36,624 / 60,615; apps-core 30,697 / 3,053; usb-stack 27,541 / 11,553; lcd-fonts 22,760 / 165,459 (lcd framebuffers); lang 17,230 / 4,569; unicode-nfc 11,645; linker (veneers) 4,888; stock-menus 2,303; skin 1,299; rest under 1.3 KB each.

**Attribution caveat (new finding):** the linker merges every `.rodata.*.str1.1` section into one pool of 21,605 B (0xa23b5..0xa781a) and the map charges all of it to `apps/debug_menu.o`. So "devmode-stock-ui 57,632" is overstated by ~21.6 KB and the real shell/other objects are understated. Per-object string contributions (pre-dedup, 24.5 KB in total): settings_list 5,267; debug_menu 2,265; id3tags 1,567; usbscreen 1,544; devmode 1,382; tagcache 1,297; artcache 886; metadata 524; powerlog 433; settings 388; ui_main 388; batcal 372; udmatest 353; debug-pp 349; buflib_mempool 283; fat 276; nowplaying 253 (full list in `strs.txt`). sizemap.py should special-case merged string sections (or split by per-object `.str` size); worth a small tooling fix.

Real developer-only stock UI (not counting the pool and not counting recorder/jpeg which the art builder uses): debug_menu 5,072 text + 2,265 own strings + gui/list 2,192 + gui/line 2,033 + option_select 1,704 + bitmap/list 1,544 + menu.o 2,303 + splash 1,147 + scrollbar 876 + icon 836 + viewport 616 + usb_screen 184 = ~20.8 KB (already mostly Thumb). Not developer-only: jpeg_load 11,808, resize 3,404, bmp 2,952, jpeg_idct_arm 1,440 (art builder).

Top objects by image: tagcache 45,894; library 39,650; artcache 35,042; panels 29,546 (23,114 rodata); debug_menu 26,812 (21.7 KB of it is the string pool); np_draw 21,521 (15,397 rodata); nowplaying 20,515; lang_core 16,390; usb_storage 15,202; playback 14,569; search 12,486 (6,834 rodata); settings_list 12,203; mp4 11,864; jpeg_load 11,808; fat 11,311; preview 10,897; playlist 9,665; nfc 9,563; artfmt 9,333; lcd-16bit 9,032; list 8,744; dbjournal 7,380; otg 7,200; ui_main 6,741.

Top 40 functions (bytes): lib_precompute_ctx 7,004; handle_scsi 5,028; clip_jpeg_fd 4,704; read_mp4_container 4,392; art_thread 4,180; run_books 3,996; np_tick 3,972; commit_timed 3,968; audio_playback_handler 3,672; run_late 3,640; build_index 3,428; build 3,260; setid3v2title 2,980; ui_usb_screen_loop 2,846; ipodui_main 2,772; tagcache_thread 2,752; get_asf_metadata 2,668; ui_list_tick 2,628; usb_drv_int 2,388; store_row_jpeg 2,068; vuprintf 2,060; format_double_radix 2,020; usb_storage_transfer_complete 1,996; slide_frame 1,980; chap_walk 1,960; add_dir_entry 1,904; decode_pinned 1,896; ui_qscroll_draw 1,852; np_enter 1,784; rows_build 1,744; run_panels 1,732; mp4_read_chapters_buf 1,720; read_bmp_fd 1,716; playlist_resume 1,716; add_tagcache 1,668; album_try 1,600; ui_journal_compact_worker 1,588. (`bootscreen_block` 10,224 is in .init, not resident.) Full list in `nm.txt`. Notable: `format_double_radix` (2,020) is floating-point printf support inside vuprintf; check whether any caller prints a double (probably only the Rockbox `%f` path).

Top 25 rodata objects (B, access pattern):
1. merged string pool 21,605 (read on log/format/UI text; settings_list names 5.3 KB are config-file keys, only read at boot/save)
2. core_language_builtin 16,390 (string table, random access via 1,141-pointer array, hot enough that it must stay RAM-addressable)
3. npph_book_data 5,778 and npph_note_data 5,483 (Now Playing placeholders, PackBits, decoded once per placeholder draw, only when a track has no art)
4. nfc_pairs16 5,568 + nfc_ccc_rng 1,612 + nfc_pairs32 396 + nfc_ccc_val 403 (utf8 normalisation, lookups at tag/scan time)
5. settings (settings_list table) 5,448
6. panelart_wheel_rgb_tu 4,568 + panelart_wheel_a_tu 505 (About / Panels: read on screen entry)
7. panelart_map 3,753 (panels, on entry)
8. srch_glyph 3,570 (search glyph coverage)
9. srl_alpha 2,954 (search letters, search screen)
10. glyph_eq 2,327; glyph_clicker 1,199; glyph_timer 1,094; glyph_volume 1,014; glyph_shuffle_on 957; glyph_book 923; glyph_contacts 844; glyph_sun 829; glyph_repeat 804 (row/menu glyph coverage, read when the row is drawn)
11. rdigit_cov 2,160 (roller digits)
12. caldig_rle 1,767 (calendar digits RLE)
13. panelart_podcast 1,677; panelart_clock_g 1,571; panelart_clock_a 1,227
14. _font_bits 1,146 (sysfont)
15. jointable 1,104 (Arabic joining; only for RTL text)
16. audio_formats 912 (metadata)
17. luma_table + chroma_table 1,656 (JPEG, art builder)
18. tz_cities 582; genres 592; taglist 560; diac_ranges 572; keymap-ipod 1,231; arabjoin 1,144
Sum of all non-string, non-lang rodata objects above ~85 KB; remainder is small tables.

## 2. Thumb reserve (T1b and beyond), measured now
Method: same flags, ARM vs `-mthumb`, `size` text+rodata; "net" subtracts 8 B per distinct called external symbol and per exported function (veneer upper bound; ARMv4T has no BLX, per docs the stubs are one per callee). Real linked deltas will be a bit better where stubs are shared.

Files on the T1b list still ARM (raw / net saving, proof needed):
| file | arm to thumb | net | what runs | proof needed |
|---|---|---|---|---|
| artfmt.c | 9,601 to 6,267 | ~2,750 | art format decode/encode on the builder path | per-album time in `ipodui.log` (< 5 %); it feeds the art builder, so scan-time sensitive |
| dbjournal.c | 7,607 to 4,793 | ~2,480 | journal replay at boot, compaction worker | boot journal replay time + one compaction (bulk loop; CRC stays ARM) |
| otg.c | 7,465 to 4,961 | ~1,940 | list resolve with pass over the filename tag file | OTG list resolve time on the owner library |
| screens/search.c | 12,623 to 10,883 | ~1,190 | per-keystroke screen + draw | keystroke-to-paint on device, plus sim row trace |
| batcal_core.c | 3,571 to 2,291 | ~1,060 | `bc_tick()` in tick IRQ | cycles per tick in sim; 1 s cadence, trivially safe in practice; no asm in the core file |
| lib_shuffle.c | 3,354 to 2,236 | ~640 | Shuffle Songs start (library pass) | Shuffle Songs start time on the library |
| search_core.c | 1,865 to 1,187 | ~600 | matching loop per keystroke over the library (on the "hot list") | device keystroke time; this is the only one with real risk |
| shuffle_core.c | 1,780 to 1,116 | ~540 | shuffle permutation | same as lib_shuffle |
| powerlog_core.c | 1,523 to 1,099 | ~350 | `pl_ring_push()` per HZ/4 in tick | cycles per tick |
Total T1b remainder ~11.6 KB net (about 9.5 KB if the three bulk-loop files are left out).

Blocked files: `batcal.c`, `powerlog.c`, `ab_store.c`, `uilog.c`, `artcache.c`, `tagcache.c`, `main.c`, `beep.c` fail under `-mthumb` because of inline ARM asm (`mrs cpsr` for IRQ disable, `mov r,r,lsr#7`). They could join only if the asm is moved to a tiny ARM helper (ab_store ~1,900 raw, powerlog ~1,300, batcal ~1,000; tagcache and artcache are hot).

Other cold ARM objects not on any list (raw / net where computed):
- apps/gui/line.c 663, scrollbar.c 490, icon.c 300, viewport.c 136: reach.py shows their only callers are T2 thumb stubs and gui/list.c, so they are developer-only like the rest of T2. ~1.1 KB net. Lowest-risk Thumb candidates there are.
- apps/playlist.c 4,368 raw (~3,200 net): bulk per-track loops on playlist build; needs a "start playback of a big album" timing.
- apps/misc.c 2,295 (~960), apps/settings.c 1,636 (~620), apps/playlist_viewer.c/playlist_catalog.c if linked (not in this image).
- ui_main.c 2,012 (~990), unlit_core.c 1,566 (~1,390; but it is per-minute pixel work, kept ARM deliberately), ab_book.c 1,134 (~770), np_playback.c 1,358 (~650), vclass/vixpass/devbcm not linked in this build.
- Hot, not recommended: library.c 13,615 (~11.5 KB net, scan speed is an owner priority), nowplaying.c 6,282 (~5 KB), playback.c 4,626, buffering.c 2,117, pcmbuf.c 1,863, panels.c 2,052, list.c 2,786, preview.c 2,395.
With the new cap none of the hot ones should be touched.

## 3. Rodata on disk or compressed
Nothing here is needed for the cap; ranked by value per risk under the new rules.
- **Search glyph/letter tables (srch_glyph 3,570 + srl_alpha 2,954 + glyph_* ~9 KB):** read only when a screen draws. A single `art.bin` on disk, loaded with one bulk read into a `core_alloc` block on screen entry (freed on exit), is the same pattern as the disk font. Cost: one read (~tens of ms on flash) at screen entry. ~10 KB.
- **Now Playing placeholders (npph_book_data + npph_note_data, 11.3 KB):** PackBits already, shown only for tracks without art; load from disk on first use and keep (or decode into the existing art cache). ~11 KB. Needs the art decode path to read from a file instead of memory (np_draw.c 400-410).
- **panelart_* (wheel 5 KB, map 3.7 KB, podcast/clock 4.5 KB), caldig_rle 1.8 KB, rdigit 2.2 KB, tz_cities 0.6 KB:** entered on a press; same mechanism. ~18 KB.
- **nfc tables (~8 KB in nfc.o):** utf8 normalisation at scan/tag time; load as one bulk file at scan start and free after; or keep (scan speed). Medium value.
- **Language table (16.4 KB, plus 4.5 KB bss pointers):** English built-in is needed; but many entries are Rockbox-only. My crude reference scan (LANG_ ids by text use; mapping of IDs to strings is approximate, 1,141 ids vs 1,140 strings so treat as upper bound): ~267 entries (~3.9 KB) have no textual use in compiled sources (some are used through a base+index macro, e.g. LANG_IPODUI_LEGAL_*, so the real number is smaller), ~327 are used only by stock apps files (4.6 KB), 238 only by settings_list/headers (3.8 KB). Pruning needs a use list from the language generator; skip until a real need.
- **settings_list names (5.3 KB strings + 5.4 KB table + 12.2 KB code/relocs):** most Rockbox settings are hidden (developer only) but the `.cfg` key names live resident. Could be cut by dropping settings the shell never exposes, but the developer UI still lists them. Medium risk, ~6 KB.
- **Debug/log strings in release:** usbscreen 1.5 KB (USB trace formats), devmode 1.4 KB, udmatest 353 B, powerlog 433, batcal 372, artcache 886. Diagnostic text the owner uses on device; not worth removing.
- **Unused fonts:** none; ShellSans loads from disk and only sysfont (1.8 KB) is resident. BCM memory as a rodata home: not pursued here; any table the video chip needs could live there, but it is write-only from the host for this purpose and costs a bus transfer, so disk wins.

## 4. Dead / duplicate / unused-by-shell
- reach.py: unreachable 1,157 B image = `lcd_write_yuv420_lines` 584 (.icode, kept for the Videos door) + libgcc `_udivmoddi4` exidx 8 + ~565 B of alignment/unattributed. Essentially nothing dead.
- Linker veneers: 3,864 B ("linker stubs" in `textobj.txt`; 327 x 8 + 46 x 12 per hardware.md). Some callees are duplicated (e.g. `__memcpy_veneer`, `__memset_veneer`, `__create_thread_veneer`, `__system_reboot_veneer` appear twice, ARM-side and Thumb-side). More Thumb adds more veneers.
- Rockbox features compiled but not exposed by the shell, candidates for `#ifndef HAVE_IPOD_UI` or `--ipodui` config: DSP compressor 2,949, surround 2,459, PBE 1,782, crossfeed 991, AFR 784 (~9 KB; shell has no UI, though the stock EQ/sound path may reference them; check `dsp_core` registration); tdspeed 2,783 is needed for audiobook speed. `format_double_radix` 2,020 in vuprintf if no `%f` user; cuesheet 1,928 (cue chapters: keep, audiobooks); beep 388 + piezo 741 (piezo exists on 5.5G hardware: keep or drop with haptics flag); debug-pp 988, screendump 362, rolo 602 (developer only); arabjoin/bidi/hangul ~2.3 KB (RTL/Hangul text; keep for tags).
- Duplicated helpers: nm shows no duplicated data tables; static functions repeat by name across objects only for small helpers (`core_get_data` x3, `file_internal_lock_WRITER` x3, `ops` x4), under 200 B in total.
- Unwarminder/tlsf/skin_parser libraries are built but not linked (not in the map). Nothing to drop there.

## 5. Developer stock UI as a loadable module (T2)
Resident ~20.8 KB (above), reachable only from `ipodui_devmode` through the hidden combo; main() calls `list_init` and `gui_syncstatusbar_init` once (need to be stubbed), and the splash funnel returns early under the shell. Rockbox already has a loader for position-dependent code (`firmware/lc-rock.c` is linked; plugins and codecs use it). A `dev.rock`-style module would need: (a) a link of debug_menu + gui/list + line + option_select + bitmap/list + menu + splash + scrollbar + icon + viewport at a fixed load address (as codecs are), (b) an API table of the ~100 firmware/apps symbols they use, or resolving against rockbox.elf's symbol map at build time and patching (codecs use a table struct), (c) loading it into the audio buffer at combo entry (needs audio stopped or a reserved 24 KB block) and handling state that outlives the call (gui_list globals, bss 1,777 B). Estimated saving ~19 KB resident (+~1 KB loader/API table). Risk: moderate to high (new build target, API drift, crash-in-developer-mode), value now low because the cap is 1.5 MB. Recommendation: do not do it now; if wanted later, a smaller first step is to `#ifdef` the stock UI out of a release build entirely (devmode then keeps only the shell's own developer screens) which gives the same ~20 KB with zero loader work, at the cost of losing the stock debug screens on release builds.

## 6. Re-ranked recommendation (post-decision)
Ranked by value (bytes saved per risk) under the 1.5 MB cap. None is required; do in this order if you want buffer back.
1. Fix sizemap.py to split the merged string pool by per-object `.str` size (0 B saved, removes a 21.6 KB misattribution that misleads every future audit). Zero risk.
2. Load-on-demand art tables from one disk file: Now Playing placeholders 11.3 KB (low risk, one call site).
3. Same for panelart/glyph/search/calendar/roller tables, ~28 KB combined (low risk each; one loader helper shared; no timing issue except a read on screen entry; honours "bulk I/O, not streamed").
4. Drop DSP features the shell never exposes (compressor, surround, PBE, crossfeed, AFR) if the config allows: ~9 KB, low-moderate risk (check dsp_core and the stock EQ).
5. Stock developer UI compiled out of release / split (section 5): ~20 KB, low risk if simply compiled out; moderate if made a loader module.
6. Add gui/line.c, scrollbar.c, icon.c, viewport.c to T2 Thumb: ~1.1 KB net, no hot caller; trivial, low gain.
7. Prune `settings_list` of never-shown settings: ~6 KB, medium (developer UI lists them).
8. nfc tables load-at-scan: ~8 KB, medium (scan speed).
9. Language table trimming of unused strings: up to ~4 KB, medium (needs a proper use analysis).
10. `format_double_radix` removal if no `%f`: 2 KB, low.
11. Cold ARM-asm-blocked files after moving the IRQ asm to an ARM helper (powerlog, batcal, ab_store): ~3 KB, low risk but churn.
12. T1b low-risk: batcal_core (1,060) and powerlog_core (350): need tick-cycle proof; sim-measurable.
13. T1b dbjournal (2.5 KB), artfmt (2.75 KB), otg (1.9 KB), lib_shuffle + shuffle_core (1.2 KB): each needs a device timing proof (scan/boot paths): demote, device-timing churn.
14. screens/search.c (1.2 KB): keystroke proof.
15. search_core.c (0.6 KB): hot list, not worth it.
16. playlist.c (3.2 KB net): big-playlist start timing; demote.
17. ui_main/misc/settings/ab_book/np_playback Thumb (~4 KB total): moderate churn; demote.
18. Hot files (library 11.5 KB, nowplaying 5 KB, playback 4.6 KB, ...): do not touch; scan speed is an owner priority.
19. Remove diagnostic strings (~6 KB): rejected, the owner uses them on device.
20. Remove Videos support (`lcd_write_yuv420_lines` 584 B): rejected (Videos door stays open).
