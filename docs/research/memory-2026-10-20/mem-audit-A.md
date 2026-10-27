> Research record, 2026-10-20 (read-only audit; inputs to docs/memory-audit-2026-10-20.md, docs/design/module-loader.md and docs/hardware-fit-audit.md). Paths to `scratchpad/` refer to the session's scratch area and are not kept. Where it conflicts with the synthesis docs (notably the floor framing: the owner chose option 1, the audio file buffer itself), the synthesis wins.

# Memory audit A: static RAM (fw-ui, `_end` = 1,092,208 B, cap 1,100,000)

Source: `.claude/worktrees/headers/build/fw-ui/rockbox.{elf,map}` (read only), worktree sources, `fw-rel` ELF for comparison, `scripts/risk/reach.py`. No build was run, so `stack_depth.py` (needs `.ci` files, none present) was NOT run; stack figures come from `docs/risk-tooling.md` (2026-10-06) and `docs/hardware.md` (2026-10-19, 8b-T). Raw nm dumps: `nm.txt`, `nmn.txt` in this folder.

## 0. Headlines

1. **`_end` is the whole resident image, not just RAM statics**: `.text` 645,696 + `.rodata` 133,516 + exidx 8 (= 779,224 up to `.data` at 0xbe3d8), `.data` 8,472 + `.ncdata` 16, `.bss` 299,644 + `.ncbss` 4,852 (= 304,496). Sum = 1,092,208 = 0x10aa70.
2. **The 4 KiB step is a one-way ratchet for everything before `inquiry_static` (0xff000).** Pad is 524 B (0xfedf4 -> 0xff000). Shrinking anything *before* the barrier by X moves `_end` by **0** until X >= 3,572 B, then by exactly 4,096 (and again at X >= 7,668, 11,764, ...). Growth: the first 524 B are free, the 525th costs 4,096. Anything *after* the barrier (cache_buffer, bidi buffers, USB stats, `.ncbss`) is byte-exact both ways. This is why "after page alignment" savings below are given as raw bytes plus side (P = pre-barrier, A = after).
3. Cap headroom: 7,792 B. One more step (+4,096 -> 1,096,304) is still under the cap; two steps (1,100,400) break it.
4. **The cap measures the developer build.** `build/fw-rel` (no `HAVE_IPOD_UI_DEVMODE`) has `_end` = 0x100820 = **1,050,656 B (-41,552 B)**: text -32.8 KB, rodata -6.4 KB, bss -2.98 KB (simplelist_buffer 1,024, devstats 584, current_subitems 256, m0_*/f3_base/line/dev_menu items, ...). If the device ships fw-rel, the 1.1 MB cap is already 49 KB loose for shipping; the real lever is deciding which build the cap applies to (a policy call, not a code change).
5. `inquiry_static` is **not** an experiment to drop: ledger row `docs/changes-from-rockbox.md:198` says it fixes the macOS INQUIRY Bulk-IN timeout and is on by default (`sw_inqstatic = true`). The only recoverable waste is the 524 B pad (candidate 1). Moving it to the end of bss would cost more (+1,460 B), computed from the layout.

## 1. Layout

### Sections (arm-none-eabi-size -A)
| Section | Size | Where | In `_end`? |
|---|---:|---|---|
| .text / .rodata / exidx | 645,696 / 133,516 / 8 | DRAM 0 | yes |
| .data / .ncdata | 8,472 / 16 | DRAM 0xbe3d8 | yes |
| .bss | 299,644 | DRAM 0xc0500..0x10977c | yes |
| .ncbss | 4,852 | uncached alias of 0x10977c..0x10aa70 | yes |
| .ibss | 25,256 | IRAM 0x40000000 | no |
| .iram | 9,320 | IRAM | no |
| .idle_stacks / .stack | 256 / 8,192 | IRAM | no |
| .init | 19,520 | codec slot, boot only | no |

IRAM statics (not in `_end`): codec_stack 9,216, qh_array 384, td_array 3,072, `__thread_entries` 2,788, resample_out_bufs 2,304, downmix_buf 2,048, sample_bufs 1,536, eq_data 576, dir_fatent 688, batch_td_array 512, beep_buf 384, comp_curve 264, usb_control_data 256 (IRAM 49,152 B total, ~46 KB used).

### Every symbol >= 256 B in DRAM .data/.bss/.ncbss (sorted by size)
Side: P = before `inquiry_static` (4 KiB-quantised), A = after (byte-exact). Phase: always / playback / USB / scan+commit / devmode / boot.

**Display and shell (apps/ipodui, gui)**
| Symbol | B | Side | What / phase |
|---|---:|---|---|
| lcd_static_framebuffer (lcd-16bit.o) | 153,600 | P | 320x240x16 LCD fb; always; mandatory |
| ring (batcal.o) | 1,024 | P | battery-calibration sample ring (#70); always |
| ring (uilog.o) | 768 | P | 12-line ui log ring; always |
| click_buf (click.o) | 768 | P | rendered wheel-click PCM; always (rendered once) |
| sbuf (ab_store.o) | 512 | P | cached A-B store sector; playback/NP |
| items 476, main_items 420, music_items 252, pv 200, theme 180, dt_items 140 | data | P | menu/theme tables; always |
| menu_slot 376, grp_bits 256, slots 256 (library.o), stop 256 (search_core.o), value_label 240, foot 232, stack 224 | | P | shell state; always |
| simplelist_buffer 1,024 (+text 128) (gui/list.o) | 1,152 | P | simple lists; devmode-only users (absent in fw-rel) |
| current_subitems (menu.o) | 256 | P | devmode menus; absent in fw-rel |
| all ipodui .data+.bss | 9,960 | | `docs/hardware.md` says 9,201 B (bss 6,750); includes small items not listed |

**Tagcache / library / playlist**
| Symbol | B | Side | What / phase |
|---|---:|---|---|
| tagcache_stack | 17,408 | P | thread stack (0x4000+0x400); thread is permanent; used scan/commit/journal drains |
| build_idx_buf 552, curpath 552, command_queue 512 (+ctx a/b 192 each) | ~2,000 | P | tagcache build + queue; always resident, used scan/commit |
| current_playlist / on_disk_playlist | 380 each | P | playlist control; playback |
| tagcache object total | 20,057 | P | |

**Playback / codec / buffering**
| Symbol | B | Side | What / phase |
|---|---:|---|---|
| buffering_stack | 9,216 | P | buffering thread; playback + metadata parse (always created) |
| static_id3_entries | 8,352 | P | 4 x 2,088 B mp3entry (PLAYING, BUFFERING, NEXTTRACK, PEEK); playback |
| audio_stack | 5,120 | P | audio thread; 95.3% static peak (ROADMAP H1) |
| last_folder_aa_path | 260 | P | playback art path |
| pbe_filter 280, afr_filters 224 | | P | DSP, always |
| codec_stack 9,216 | | IRAM | not in `_end` |

**Firmware / drivers / kernel**
| Symbol | B | Side | What / phase |
|---|---:|---|---|
| open_streams (dir.o) | 11,520 | P | 32 dir slots x 360 B; always |
| fobindings (fileobj_mgr.o) | 4,224 | P | 66 x 64 B (31 files + 32 dirs + 3 aux); always |
| stream_mutexes (ncbss) | 1,584 | A | 66 x 24 B; always |
| open_streams (file.o) | 2,480 | P | 31 file slots x 80 B; always |
| state (random.o) | 2,496 | P | Mersenne Twister state (624 words); used by shuffles, srand(seed) |
| scroll_stack 3,072, lcd_scroll 1,664, line_buf 745 (scroll_engine.o) | 5,481 | P | stock scroll thread + 4 scroll lines; shell does not use them (own scroller); live always, used by devmode/Rockbox lists |
| bidi_buf 2,980, utf_buf 2,980 (bidi.o) | 5,960 | A | char->UCS buffers, 745 chars each; every `lcd_putsxy`; always |
| language_strings (lang) | 4,560 | P | 1,140 string pointers; always |
| usb_stack | 4,736 | P | USB thread; always created, busy in USB mode |
| global_settings 1,836, global_status 320, settings_list (.data) 4,548 | | P | settings; always |
| storage_thread_stack | 2,048 | P | storage thread; tight (2 KiB) |
| sector_cache 1,040, identify_info 512 (ata.o) | | P | ATA; always |
| piezo_stack 1,024, backlight_stack 1,024, power_stack 512 | | P | threads; always |
| events 336, fat_bpbs 352, panic_buf 128 | | P | |
| buf.5 (usb_storage) | 512 | P | USB; USB only |
| ustats 352, dstats 384 | 736 | A | USB session counters (`HAVE_USB_STATS`, ipodvideo.h:295); USB only, tiny always-resident |
| devstats (ncbss) | 584 | A | `HAVE_DEVSTATS`, devmode build only |
| inquiry_static | 36 | -- | 4096-aligned INQUIRY reply; USB only |

**Disk cache (FS)**
| Symbol | B | Side | What |
|---|---:|---|---|
| cache_buffer | 32,768 | A | 64 x 512 B sector buffers; always |
| cache_entry 1,536, cache_map_entry 1,024 | 2,560 | A | 64 entries x 24 B; 1 map x 32 buckets x 4 vols x 8 B; always |

Remaining ~2.5 KB is a long tail of items < 256 B (queues ~156 B each x 14 = 2.2 KB in `.ncbss`, mutexes, flags).

### Subsystem totals (bss+data, DRAM, from the map)
LCD fb+scroll 159.1 KB (fb 153.6) | disk cache 35.3 | tagcache 20.1 | dir/file/fileobj tables 18.3 | buffering 9.3 | playback 8.7 | bidi 6.0 | audio thread 5.1 | USB (usb.o 4.9, usb_storage 1.6, drv 0.4, core 0.4) 7.3 | settings (list+runtime) 6.7 | ipodui 10.0 | PRNG 2.5 | storage/ata 3.7 | threads misc (piezo, backlight, power) 3.1.

## 2. Alignment and padding waste (exact)
| Where | Bytes | Detail |
|---|---:|---|
| .bss `*fill*` total | 910 | |
| - before `inquiry_static` | **524** | 0xfedf4 (end of `cbw_buffer`) -> 0xff000, `aligned(4096)` (usb_storage.c:646) |
| - small 2/4/8/16-byte alignment | 386 | scattered, not reclaimable |
| .data / .ncdata / .ncbss fills | 59 / 12 / 14 | |
| .text / .rodata fills | 98 / 96 | |
| `.idle_stacks` | 256 | two 128 B idle stacks (IRAM, not `_end`) |
| `.stack` | 8,192 | main/UI stack (IRAM, not `_end`) |
| fw-rel pad before `inquiry_static` | 1,196 | for reference |

`reach.py` "unreachable bss 940" is therefore essentially the padding (910) plus ~30 B; it itemises no real dead statics. Unreachable code: `lcd_write_yuv420_lines` 584 B (icode, IRAM) and libgcc exidx 8 B (listed); summary total 1,157 B. Nothing material is compiled in but unreached: the shell's reach graph says the stock scroll thread, bidi, random, playlist control etc. are all reachable (even if only via devmode or boot init).

## 3. Candidates to remove or defer (after-alignment accounting)
"Realised" = `_end` movement. All P-side items combine: realised = 4,096 x floor((sum + 524) / 4096). A-side items are exact. The page rule is a cap-planning aid, not a reason to reject an item: a P-side item that alone doesn't reach a step still banks pad for future growth (the 524 B ratchet).

### Combination examples
- dirs 32->16 (7,168) + tagcache stack -3 KiB (3,072) = 10,240 P -> realised 8,192 (and 2,572 B banked pad).
- Add A-side bidi (2,888): total `_end` -11,080.

### Candidates (details)
1. **Fill the 524 B pad (link order).** Pin ~500 B of A-side small data (e.g. `ustats` 352 + `xlate` 136, or `dstats` 384 + 136) ahead of `inquiry_static` via an `app-pp.lds` rule/section name so the pad holds real data. Saves up to **524 B exact** (A items move into the pad). Risk none functionally; touches the linker script / section attributes. Other users: none (data placement only).
2. **bidi_buf/utf_buf 745 -> 384 ucschars.** New `BIDI_LINE_SIZE` instead of `SCROLL_LINE_SIZE` in `bidi.c`. Saves **2,888 B, A-side exact**. Risk low: scroll linebuffer is 369 B anyway, ID3 items cap at 500, 240 px shows ~35 chars; text beyond 383 chars is clipped before drawing. Callers: `lcd_putsxy*` (`lcd-bitmap-common.c:426,567`) plus plugin API `bidi_l2v` (plugins not shipped). Needs ledger row (firmware/bidi.c).
3. **MAX_OPEN_DIRS 32 -> 16 (or 20).** `fs_defines.h:45` under `HAVE_IPOD_UI`. Saves 360 + 64 (fobindings) + 24 (mutex) = **448 B per slot: 7,168 B at 16, 5,376 at 20**. P-side (quantised). Risk low-medium: dir users are tagcache check_dir recursion (+1 per level, ~8 deep), filetree, fileop, vplayer, vixpass, otg, playlists screen, folder_select, talk, misc; today's peak unmeasured (8b-T4 asked for a high-water mark; none exists in `firmware/common`). Add an open-dir HWM to the dev log first, then pick peak + 8.
4. **tagcache_stack 17,408 -> 14,336 (or 12,288).** Saves **3,072 (or 5,120) B**, P. Static peaks: `build_index` chain 8,720 B (hardware.md:172), 9,504 B historical (risk-tooling, 54.6%), drains 6,888 B, scan 7,272 B + 56 B per dir level. At 12 KiB that is 71-78% plus recursion; 14 KiB gives 61-68%. Risk medium (owner/handoff: "blindly shrinking is unsafe"); needs the device stack HWM (Developer > OS stacks) after a full library update + journal drain first. Moving it to `core_alloc` saves nothing (handoff 2026-10-18). Others sharing: tagcache thread only; also the journal drain 4 KiB buffer lives on it.
5. **Mersenne Twister -> small PRNG** (`firmware/libc/random.c`, `state` 2,496 B). Saves **~2,480 B**, P. Risk medium: seeded reproducible shuffles (`playlist.c:1599 srand(seed)`) would produce a different order for stored seeds once; callers: playlist, tagtree, onplay, `ipodui/library.c:3262`, plugin API. Need to check what the seed persistence promises (resume of a shuffled queue).
6. **ID3V2_BUF_SIZE 1800 -> 1200** (lib/rbcodec/metadata/metadata.h:191). 4 x 600 = **2,400 B**, P, and shrinks every on-stack `struct mp3entry` (tagcache `build_index` frame 6.4 KB). Risk medium-high for fidelity: later tags (album artist, comment, composer; audiobook author/narrator) are dropped when 12 items x 500 don't fit; `struct mp3entry` is also in the codec ABI. Needs a tag-corpus test (m4b/ID3 samples). Owner's "burned before" rule applies.
7. **Drop PLAYING_PEEK_ID3** (AUDIO_FAST_SKIP_PREVIEW undefined for the shell). **2,088 B**, P. Risk medium-high: it is the fast-skip preview metadata (next track on rapid skips); check `audio_peek_track` callers in the shell (NP title during scrub) before touching.
8. **Disk cache 64 -> 48 entries** (`fs_defines.h:46-47`). Saves 16 x (512+24) = **8,576 B... actually 8,192 + 384 = 8,576**, A exact. Constraint: entries must stay above `MAX_OPEN_HANDLES` + reserved (63 + 4 aux today), so it requires candidates 3 (and likely a file-table cut) first. Risk medium-high: scan/commit and USB read throughput (scan speed, FAT ordering work was just tuned); needs the scan benchmark on flash and HDD profile.
9. **Lazy stock scroll engine** (scroll_stack 3,072 + lcd_scroll 1,664 + line_buf 745 = 5,481 B, P). The shell never uses it; only devmode/Rockbox lists/splash do. Start the thread and allocate lines on first `lcd_scroll_*` add. Risk medium-high: thread-stack alloc on demand must survive audio-buffer takeover (pinned core_alloc after `audio_reset_buffer`), `scroll_init` is called from `lcd_init_device` (lcd-color-common.c:96). Realised only for users who never enter devmode.
10. **buffering_stack 9,216 -> 7,168** (-2,048, P). Static peak 1,872 B is blind to indirect metadata parser calls (`parse_func` pointers); stock +0x2000 was added for them. Risk high without a device HWM. Do only after measuring.
11. **simplelist_buffer 1,024 + simplelist_text 128** to `core_alloc` on show: 1,152 B, P. Already absent in fw-rel. Risk low-medium: exported in the plugin API (`plugin.c`), users are devmode/debug_menu/onplay/screens/mask_select/folder_select.
12. **USB stats (ustats 352 + dstats 384 + xlate/ra locals) to the USB transfer allocation** (~736 B, A). Counters are zeroed per session anyway (`memset(&dstats)` at usb-drv-arc.c:584). Risk low-medium: written from IRQ context, used by the device log/dev USB screen.
13. **piezo_stack 1,024 -> 768** (256 B, P). Static peak 592 B (57.8%). Risk low but haptics is one of the allowed config dimensions; savings tiny.
14. **click_buf 768** to a lazily rendered `core_alloc` (768 B, P). Risk low-medium: click latency on first wheel tick, rendered at boot today.
15. **language_strings 4,560**: trim `LANG_*` table to IDs the shell uses. Large effort, high risk (rule 8, never strip the backend). Mention only; do not do.

Not recommended (considered): framebuffer (needed), codec/audio/storage stacks (tight or IRAM), `static_id3_entries` count (4 needed for gapless/next-track), `global_settings`/settings_list (hidden-setting rewrite, high effort), batcal ring (live feature, #70), uilog ring, `inquiry_static` removal (macOS fix).

### Top 15 by bytes saved per unit of risk
| Rank | Candidate | Bytes | Side | Risk | Notes |
|---:|---|---:|---|---|---|
| 1 | Fill the 524 B pad with real data (cand. 1) | 524 | A | none | linker-only |
| 2 | bidi buffers 745 -> 384 (cand. 2) | 2,888 | A exact | low | firmware edit, ledger row |
| 3 | MAX_OPEN_DIRS 32 -> 16 after HWM (cand. 3) | 7,168 | P | low-med | needs HWM first |
| 4 | tagcache stack -> 14 KiB after device HWM (cand. 4a) | 3,072 | P | low-med | steps 1-3 combine into a 4,096 step |
| 5 | Mersenne Twister -> xorshift (cand. 5) | 2,480 | P | med | shuffle seed compat |
| 6 | simplelist buffers on demand (cand. 11) | 1,152 | P | low-med | devmode only |
| 7 | USB stats on demand (cand. 12) | 736 | A | low-med | |
| 8 | tagcache stack -> 12 KiB (cand. 4b, +2,048) | 2,048 | P | med | needs HWM, 71-78% |
| 9 | Lazy scroll engine (cand. 9) | 5,481 | P | med-high | effort high |
| 10 | click_buf lazy (cand. 14) | 768 | P | low-med | |
| 11 | Disk cache 64 -> 48 (cand. 8) | 8,576 | A | med-high | after #3, bench needed |
| 12 | ID3V2_BUF_SIZE -> 1200 (cand. 6) | 2,400 | P | med-high | fidelity of tags |
| 13 | Drop PEEK_ID3 (cand. 7) | 2,088 | P | med-high | fast-skip preview |
| 14 | buffering_stack -2 KiB (cand. 10) | 2,048 | P | high | unmeasured |
| 15 | piezo_stack -256 (cand. 13) | 256 | P | low | trivial gain |

Policy lever (outside the ranking): apply the 1.1 MB cap to `fw-rel` (1,050,656 B) and track `fw-ui` separately, or fold the 2,979 B of devmode-only bss plus 39 KB of devmode code out of the gated number.

## 4. Thread stacks vs peaks
Static worst-case peaks include the 400 B panic tail (`thread_stkov -> thread_panicf -> snprintf`). They follow direct calls only (indirect calls, e.g. metadata `parse_func`, event handlers, are missed), so treat them as lower bounds. The "stack_depth.py" tool could not be run (needs a `-fcallgraph-info` build; none present).

| Thread | Size | In `_end`? | Static peak (source) | % | Reduction? |
|---|---:|---|---|---:|---|
| main/UI | 8,192 | IRAM | 6,004 (risk-tooling); deepest through splash 4,144+400 / overall 5,112 (hardware.md:93) | 62-73 | no |
| tagcache | 17,408 | yes | 9,504 (build_index chain 8,720; +56/dir) | 50-55 | 14 KiB plausible, 12 KiB needs device HWM |
| codec | 9,216 | IRAM | 2,064 static | 22 | IRAM, not DRAM; no gain to `_end` |
| buffering | 9,216 | yes | 1,872 static (blind to parsers) | 20 | -2 KiB at most, only after HWM |
| audio | 5,120 | yes | ~95% (ROADMAP H1; read_bmp_fd frame 2,592) | 95 | none |
| usb | 4,736 | yes | not measured; trimmed from 6,784 earlier | ? | needs HWM |
| scroll | 3,072 | yes | not measured | ? | thread unused by shell (cand. 9) |
| storage | 2,048 | yes | small; journal work deliberately kept off it | tight | none |
| piezo | 1,024 | yes | 592 (57.8%) | 58 | -256 possible |
| backlight / power | 1,024 / 512 | yes | not measured | ? | leave |
| art builder | 9,216 + work 35,728 | no (`core_alloc` pinned block, shell start) | | | already dynamic |
| `playlist`, `voice`, `iap` stacks | 0 | | symbols are gone from this build | | |

Thread stacks in `_end`: 17,408 + 9,216 + 5,120 + 4,736 + 3,072 + 2,048 + 1,024 + 1,024 + 512 = **44,160 B** (4.0% of `_end`).

## 5. Prior art to reuse
`docs/static-ram-audit.md` (2026-10-06) already moved most of the big items (iAP, codepage_table, uniqbuf, language_buffer, voice, radio, recording are not in this ELF). Disk-cache map 256 -> 32 buckets was done (changes-from-rockbox.md:307). Remaining multi-KB items are exactly the ones listed above.

## 6. Runtime reservations from the audio buffer vs the floors (owner decision 2026-10-20: cap 1,500,000, floors 7.5 MB on 32 MB, 39 MB on 64 MB)

Pool before reservations = `_audiobufend` 0x3f00000 (66,060,288) - `_end` - 256 (panic record). At `_end` 1,092,208: **64 MB 64,967,824 B; 32 MB (minus 32 MiB) 31,413,392 B**. Plugin slot is 0 (IPODUI_SCRATCH_SIZE trimmed). Sizes come from `docs/hardware.md` (4b/8b tables, 2026-10-19/20) and the source constants; "est" = not measured here. The 1.5 MB cap would at most cost another 407,792 B on top of these figures.

| Reservation | Bytes | When | Source |
|---|---:|---|---|
| List strip | 184,000 (180,000 + 4,000) | shell start, pinned while drawing | `anim.c:131` |
| Row art LRU (16 x 55^2) | 109,428 | shell start | `rowart.c:205` |
| Art builder pinned block (work 35,728 + stack 9,216 ...) | 54,007 | shell start, pinned | `artcache.c:5725` |
| Art builder decode block (incl. 64 KiB JPEG refill) | ~164,120 | shell start, pinned per decode | `artcache.c:5720` |
| Drift slots 2 x 180,000 | 360,000 (180,000 if the second can't be had; 32 MB fallback) | shell start only | `preview.c:1915,1932` |
| Quick-scroll tables | ~2,900 | shell start | `qscroll.c:128` |
| Fonts (4 files) | ~102,100 | shell start | `font.c:565` |
| Now Playing block | 58,368 | NP screen open | `nowplaying.c:314` (`NP_BLK_SIZE`) |
| Playlist indices + refs | ~65,536 | boot / playlist load | `playlist.c:2031` (hardware.md: 64 KB) |
| Audiobook book block | 11,344 | on use | `ab_book.c:69` |
| Tagcache UI generation | 16,384 | shell start | `tagcache.c:4841` |
| Playback log | 2,600 | first playback | `playback.c:1329` |
| Chapter table (1024 x 68 + 8) | 69,640 | playback init, carved from audio buffer | `playback.c`, scratch mem |
| Playback `AUDIO_BUFFER_RESERVE` | 262,144 | held back by playback | `playback.c:910` |
| PCM buffer (chunks x 8 KiB) | ~500,000 est | playback init | `pcmbuf_init` |
| Library precompute | 25 B x N + 6,688: 1,006,688 @40k; 1,256,688 @50k | shell start, re-reserved while stopped if outgrown | `library.c:3888` |
| Library view pool | 12 B x N + 64 KB: 545,536 @40k; 665,932 @50k | on library load | `library.c:1023,1062` |
| Tagcache RAM cache (if on; shell setup per static-ram-audit) | ~5.5 MB @40k est; ~6.9 MB @50k est | boot, reloaded after commit | `tagcache.c allocate_tagcache` |
| Commit buffer (transient) | 5.86 MB first build @50k; 8.06 MB 1-track update in the 64-bit sim; steals the RAM cache first, waits while playing | scan/commit, stopped | `tagcache.c:4560` |
| Panel transient | 785,844 | stopped only, freed after each panel | hardware.md |
| USB prefetch ring | 8 MB (4/2 MB fallback) | USB mode only, taken from idle audio buffer | `usb_storage.c:1036` |
| Power log ring | 6,348 | devmode, when on | `powerlog.c:566` |
| Library-less dev screens (devbcm, udmatest) | small, dev only | devmode | |

Totals: shell-start fixed 1,131,043 B (951,043 on 32 MB with one drift slot); playback 831,784 B; library 1,552,224 B @40k (1,922,620 @50k).

| Case | 64 MB audio buffer | floor 39,000,000 | 32 MB audio buffer | floor 7,500,000 |
|---|---:|---|---:|---|
| Steady, 40k tracks, RAM cache on (reservations 9.02 MB / 8.84 MB) | ~55.9 MB | +16.9 MB | ~22.6 MB | +15.1 MB |
| Steady, 50k stress (10.79 MB / 10.61 MB) | ~54.2 MB | +15.2 MB | ~20.8 MB | +13.3 MB |
| 50k + USB ring 8 MB + panel transient 0.79 MB (worst stack) | ~45.4 MB | +6.4 MB | ~12.0 MB | +4.5 MB |
| Same at cap 1,500,000 (extra 407,792 B) | ~45.0 MB | +6.0 MB | ~11.6 MB | +4.1 MB |

Caveats: the RAM cache, PCM buffer and per-track sizes are estimates (hardware.md figures for the cache are not re-measured; confirm via the devmode buffer screen, P10). The commit buffer is not additive with the RAM cache (it takes it over), and the USB ring is only taken when audio is idle, so the last two rows are conservative. The binding case is the 32 MB unit at 50k with the USB ring; it holds with ~4 MB spare. Nothing here threatens the floors; the larger risk is the 8 MB ring fallback on a 32 MB unit with a 50k+ library (spare would be ~4.1 MB, ring falls back to 4/2 MB by design).

### Re-ranking by value (cap is no longer the constraint)
Value-first order of the static candidates: (1) bidi buffers 745 -> 384 (2,888 B, low risk, harmless cleanup), (2) MAX_OPEN_DIRS cut after a high-water log (7,168 B), (3) pad fill (524 B), (4) lazy scroll engine only if the thread/stack becomes a problem, (5) everything marked medium-high risk (cache_buffer, ID3 buffer, PEEK id3, buffering stack, Mersenne Twister replacement, tagcache stack cut) is now **not worth the fidelity or stability risk**: the saved bytes (2-9 KB) are 0.01-0.04% of the 32 MB floor margin. With the cap at 1.5 MB, the 4 KiB `inquiry_static` step stops mattering. Keep the stack and tag-buffer sizes as they are.
