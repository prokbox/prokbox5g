# Static RAM and binary-size audit (iPod Video, target 22)

Date: 2026-10-06. Baseline is `build/fw` (stock, `--target=22 --type=n`). `build/fw-ui`
(`--ipodui`) is within 8 bytes of it (text +8, bss +8), so the shell does not trim or
add anything yet. Every number below applies to both builds.

Method: `arm-none-eabi-size -A`, `arm-none-eabi-nm --size-sort -S`, and a parser over
`build/fw/rockbox.map` that attributes every input section to its object file. The merged
`.rodata.str1.1` string pool (25.7 KB) is listed by the linker under `bookmark.o`. It has been
moved into its own row. Trim estimates marked **measured** come from throw-away builds of
patched source copies in the session scratchpad. No tracked file was edited for them.

## 1. Where the bytes are

| Section | Bytes | Lives in | Notes |
|---|---:|---|---|
| `.text` | 603,984 | DRAM | |
| `.rodata` | 446,252 | DRAM | 341 KB of this is utf8proc tables |
| `.data` (+`.ncdata`) | 10,892 | DRAM | 4.9 KB `settings_list` |
| `.bss` + `.ncbss` | 590,840 | DRAM | overlays the load images of `.vectors`/`.iram`/`.init` |
| `.ibss` | 27,748 | core IRAM | |
| `.iram` (icode/idata) | 9,612 | core IRAM | |
| `.idle_stacks` + `.stack` | 8,448 | core IRAM | |
| `.init` | 72,704 | **codec slot** at boot, then discarded | 62.7 KB is `rockboxlogo.320x98x16` |
| **Resident core DRAM** (`_end` = `audiobuffer`) | **1,651,968** (1.575 MiB) | | this is the number that matters for RAM |
| `rockbox.ipod` file | 1,143,516 | | text+rodata+data+iram+init images |

`size` Berkeley: text 1,132,560 / data 10,948 / bss 627,044 (its "bss" includes the IRAM NOLOAD sections).

### DRAM map (64 MiB)

```
0x0000000  core: text/rodata/data/bss ............ 1.575 MiB (resident)
0x0193500  audiobuffer -> buflib "core_ctx" ....... 60.93 MiB (63,884,032 B)  <- every core_alloc()
0x3E80000  codec slot (CODEC_SIZE) ................ 1 MiB   (.init lives here during boot)
0x3F80000  plugin slot (PLUGIN_BUFFER_SIZE) ....... 512 KiB
0x4000000  end
```
On a 32 MB unit, `core_allocator_init()` subtracts 32 MiB from `audiobufend` at runtime.

## 2. Top 40 static consumers (DRAM `.bss`/`.data`, plus IRAM marked [I])

| # | Symbol | Bytes | Object | Subsystem |
|---:|---|---:|---|---|
| 1 | `lcd_static_framebuffer` | 153,600 | lcd-16bit.o | LCD framebuffer, required (320x240x2) |
| 2 | `serbuf` | 65,672 | apps/iap/iap-core.o | **iAP serial** (static because `IAP_MALLOC_DYNAMIC` is unset) |
| 3 | `codepage_table` | 65,538 | unicode.o | codepages (used only for non-default codepages) |
| 4 | `uniqbuf` | 65,536 | apps/tagtree.o | tagcache browse dedupe list |
| 5 | `language_buffer` | 34,216 | apps/language.o | lang (sized for the largest language; English is 15.4 KB) |
| 6 | `cache_buffer` | 32,768 | disk_cache.o | FS sector cache (32 x 1 KB) |
| 7 | `tagcache_stack` | 17,408 | apps/tagcache.o | thread stack |
| 8 | `lcd_scroll` | 13,312 | scroll_engine.o | 32 scroll lines x 416 B |
| 9 | `open_streams` | 11,776 | dir.o | 32 open-dir slots x 368 B |
| 10 | `codec_stack` [I] | 9,216 | codec_thread.o | thread stack in IRAM |
| 11 | `buffering_stack` | 9,216 | buffering.o | thread stack |
| 12 | `static_id3_entries` | 8,304 | playback.o | playback id3 slots |
| 13 | `stackbegin` (main) [I] | 8,192 | linker | main stack in IRAM |
| 14 | `normbuf` | 8,192 | unicode.o | utf8proc scratch |
| 15 | `cache_map_entry` | 8,192 | disk_cache.o | FS cache |
| 16 | `usb_stack` | 6,784 | usb.o | USB thread stack |
| 17 | `thread_stack` | 6,144 | apps/iap/iap-core.o | **iAP thread stack** |
| 18 | `audio_stack` | 5,120 | audio_thread.o | thread stack |
| 19 | `fobindings` | 4,824 | fileobj_mgr.o | FS |
| 20 | `language_strings` | 3,732 | lang_core.o | lang |
| 21 | `viewer` | 3,268 | playlist_viewer.o | stock UI |
| 22 | `scroll_stack` | 3,072 | scroll_engine.o | thread stack |
| 23 | `playlist_stack` | 3,072 | playlist.o | thread stack |
| 24 | `utf_buf` | 2,980 | bidi.o | bidi |
| 25 | `bidi_buf` | 2,980 | bidi.o | bidi |
| 26 | `__thread_entries` [I] | 2,720 | thread-common.o | kernel |
| 27 | `state` | 2,496 | random.o | PRNG |
| 28 | `open_streams` | 2,480 | file.o | FS (31 files) |
| 29 | `resample_out_bufs` [I] | 2,304 | resample.o | DSP |
| 30 | `filetypes` | 2,304 | filetypes.o | file browser |
| 31 | `kbd_param` | 2,088 | recorder/keyboard.o | on-screen keyboard |
| 32 | `storage_thread_stack` | 2,048 | storage.o | thread stack |
| 33 | `presets` | 2,048 | radio/presets.o | **radio** |
| 34 | `downmix_buf` [I] | 2,048 | pcm_mixer.o | PCM |
| 35 | `dircache_stack` | 2,048 | dircache.o | thread stack |
| 36 | `commit_buffer` | 2,048 | talk.o | **voice** |
| 37 | `voice_stack` [I] | 1,984 | voice_thread.o | **voice** |
| 38 | `global_settings` | 1,976 | settings.o | settings |
| 39 | `stream_mutexes` | 1,608 | fileobj_mgr.o | FS (uncached) |
| 40 | `global_decstate` [I] | 1,572 | libspeex-voice nb_celp.o | **voice** |

Thread stacks total 57.5 KB in DRAM and 19.6 KB in IRAM.

### Per-subsystem resident footprint (code+rodata+data, bss, IRAM)

| Subsystem | code+ro | bss | IRAM | Resident DRAM |
|---|---:|---:|---:|---:|
| **Unicode normalisation (utf8proc)** | 341,318 | 0 | 0 | **341,318** |
| LCD core + framebuffer + scroll | 13,203 | 171,981 | 3,304 | 185,184 |
| Tagcache + tagtree (DB) | 36,400 | 86,188 | 0 | 122,588 |
| Storage: FS/ATA/disk cache | 36,658 | 68,085 | 1,736 | 104,743 |
| **iAP (dock serial)** | 32,362 | 72,198 | 0 | **104,560** |
| Playlists/bookmarks/file browser/plugin API | 71,199 | 15,303 | 0 | 86,502 |
| Codepages/bidi/unicode | 5,705 | 79,763 | 0 | 85,468 |
| Playback engine (playback/buffering/pcm) | 37,995 | 24,365 | 13,104 | 62,360 |
| Generic GUI (lists, screens, keymaps, misc) | 52,167 | 2,946 | 8 | 55,113 |
| **USB stack (all classes + usblogo)** | 45,138 | 8,282 | 2,660 | 53,420 |
| Skin engine / WPS | 51,047 | 1,568 | 0 | 52,615 |
| Language strings | 14,221 | 37,949 | 0 | 52,170 |
| Metadata parsers (rbcodec) | 49,589 | 34 | 0 | 49,623 |
| Settings + menus | 44,207 | 3,054 | 0 | 47,261 |
| Kernel/threads/system | 36,698 | 5,485 | 4,158 | 42,183 |
| **Voice / talk (speex)** | 37,801 | 4,054 | 4,464 | 41,855 |
| Merged string literals | 25,677 | 0 | 0 | 25,677 |
| DSP | 21,420 | 972 | 6,000 | 22,392 |
| Image decode (JPEG/BMP/resize/albumart) | 19,764 | 0 | 0 | 19,764 |
| libc/libgcc | 18,710 | 76 | 1,692 | 18,786 |
| Debug menu / unwinder / panic | 17,964 | 164 | 0 | 18,128 |
| **Recording + encoders** | 15,708 | 458 | 116 | 16,166 |
| **Radio / RDS** | 11,224 | 2,843 | 0 | 14,067 |
| dircache (code) | 9,296 | 2,404 | 0 | 11,700 |
| Keyboard / morse | 5,942 | 2,212 | 0 | 8,154 |
| Fonts (code) | 7,014 | 316 | 0 | 7,330 |

### Core vs slots
- **Core** is everything above. Codecs, encoders (`*_enc.codec`) and plugins (`*.rock`) are **not** in the core image. Each one is loaded on demand into its slot.
- **Codec slot (1 MiB).** The DRAM footprint is text+rodata+data+bss plus the IRAM load image:
  - MP3 75 KB, AAC 158 KB, FLAC 128 KB, Opus 144 KB, Vorbis 109 KB, APE 254 KB, WMA 227 KB.
  - Only the chiptune codecs are larger: kss 444 KB, vgm 356 KB, spc 336 KB, sid 325 KB.
- **Plugin slot (512 KiB).** The largest plugins are clock 481 KB, doom 383 KB and pdbox 343 KB. Most plugins are under 200 KB.
- The encoders are codecs, so `HAVE_RECORDING` costs only about 16 KB of core.

## 3. IRAM

PP5022 has **128 KB** of IRAM. The core uses `0x40000000–0x4000BFFF` (48 KB, `app-pp.lds`). Codecs and plugins get their own **80 KB** window at `0x4000C000` (`plugin.lds`, which codecs also use). The two don't compete for the same bytes.

**Core IRAM: 45,808 / 49,152 B used (93.2%), 3,344 B free.**

| User | Bytes | Type |
|---|---:|---|
| codec thread stack | 9,216 | ibss |
| main stack + idle stacks | 8,448 | stack |
| DSP (resample 2456, sample_input 1536, eq 576, dsp_core 424, compressor 316, crossfeed 152, dsp_arm icode 504, misc) | 6,000 | ibss+icode |
| **voice** (voice_stack 1984, speex state 2096, speex icode 384) | 4,464 | ibss+icode |
| PCM (pcm_mixer 2180+948, pcm-pp 376) | 3,504 | ibss+icode |
| LCD drawing (lcd-16bit 2600, lcd-as-video 672, lcd-video 32) | 3,304 | icode |
| thread table (thread-common) | 2,916 | ibss |
| USB (ARC qh/td 1664, hid 352, iap 256, core 256, audio 132) | 2,660 | ibss |
| ATA/FS (ata-as-arm 844, ata 172, dir_fatent 688, disk_cache 32) | 1,736 | icode+ibss |
| memcpy/memmove/memset/strlen | 1,692 | icode |
| system-pp502x 880, beep 384, rolo 310, pcm_record 116, other | ~1,870 | |

**Codec IRAM window (80 KB):** FLAC uses 81,008 B and APE 81,104 B of 81,920, so both are already at the limit. Opus uses 39 KB, MP3 56 KB, AAC 71 KB.

The lean variant below frees only 1.1 KB of core IRAM. Removing voice would free 4.4 KB.

## 4. Trim candidates (ranked by value/effort)

Δ = reduction in resident core DRAM (`_end`), which is RAM given straight back to the audio buffer. File = `rockbox.ipod` size.

| Rank | Candidate | Δ resident | Δ file | Effort | Evidence / what vanishes |
|---:|---|---:|---:|---|---|
| 1 | **Drop utf8proc** (NFC normalisation of tags in `metadata.c`) | **−350,080** measured | −341,920 | Low (make flag) or Med (replace with a ~4–8 KB Latin composition table) | `utf8proc_properties` 201 KB, `stage2table` 93 KB, `sequences` 26 KB, `normbuf` 8 KB. The loss is NFD tags (common from macOS rips) sorting and deduping separately. **Alternative:** normalise at sync time on the host. |
| 2 | **iAP serial + Apple remote tuner + `HAVE_SERIAL`** | −113,952 measured (iAP alone ≈ −93,456) | −37,120 | Low (config) | `serbuf` 64 KB, iAP `thread_stack` 6 KB, `iap.o` 12 KB, `iap-lingo*` 13 KB. The USB-C mod removes the 30-pin dock, so nothing can talk iAP serial and the Radio Remote cannot attach. **Confirm with the user.** The tuner and `serial_setup()` must go too, or the link fails. |
| 3 | **USB HID + USB audio (UAC) + USB-iAP** (keep mass storage) | −46,400 measured | −43,088 | Low (config) | `usb_hid`, `hid.o`, `usb_keymaps`, `usb_audio`, `usb_iap`, 2.2 KB data, 0.8 KB IRAM. USB-iAP is the only one that might matter (car head units over USB). |
| 4 | **Recording + encoders** | −27,232 measured | −25,128 | Low (config) | `pcm_record`, `recording.o`, `enc_config`, `recording_menu`, `iap-lingo1`. The line-in source is gone with the dock. |
| 5 | **Radio / RDS** (`CONFIG_TUNER`, `HAVE_RDS_CAP`) | −20,496 measured | −15,976 | Low (config) | `radio*.o`, `presets` 2 KB, `rds.o`, `ipod_remote_tuner.o`, `iap-lingo7` |
| 6 | Statics → `core_alloc` on demand | ≈ −170,000 est. | 0 | Med (small `#ifdef` edits upstream) | `codepage_table` 64 KB (only for non-UTF-8/ISO-8859-1 codepages), `uniqbuf` 64 KB (only while browsing the DB), `language_buffer` 34 KB → about 16 KB for English only, `lcd_scroll` 13 KB (32 lines → 8 saves about 10 KB), dir `open_streams` (32 → 12 saves 7.4 KB) |
| 7 | **Voice / talk** (speex voice decoder, talk clips) | ≈ −41,900 est. + 4.4 KB IRAM | ≈ −38,000 | Med–High (needs `talk_*` stubs; voice is not behind a config flag) | `talk.o`, `voice_thread.o`, `libspeex-voice.a`. The only build where this pays off is one that needs the IRAM. |
| 8 | Stock file browser / playlist viewer / bookmarks / plugin API | ≈ −50,000 to −85,000 est. | similar | High (Phase 9; the dev-combo fallback needs some of it) | tree, filetree, filetypes, onplay, shortcuts, playlist_viewer/catalog, bookmark, plugin, open_plugin |
| 9 | `HAVE_MORSE_INPUT` + `AB_REPEAT_ENABLE` + `HAVE_USB_HID_MOUSE` | ≈ −3,100 measured (all − rec − radio) | ≈ −3,000 | Low | `abrepeat.o` and the morse tables. Too small to matter on their own. |
| 10 | `usblogo` (16.9 KB rodata) | −16,920 est. | −16,920 | Low (shell draws its own USB screen) | `usblogo.176x48x16.o` |
| – | `rockboxlogo` (62.7 KB) | 0 (it is in `.init`) | −62,744 | Low | The native-illusion boot glyph replaces it anyway |
| – | dircache code | −11,700 | −9,300 | Low (`HAVE_DIRCACHE` off) | The runtime saving is far larger; see §5 |
| – | LCD remote / remote backdrops | 0 | 0 | – | `HAVE_REMOTE_LCD` is not defined, so this is already absent |
| – | Second font cache / `FONT_UI_REMOTE` | ~0 static | | – | Fonts are buflib allocations at runtime (§5); static font code is only 7 KB |

**Combined lean build, measured:**
- Removed: rec, radio/RDS, morse, AB repeat, HID mouse, iAP serial, `HAVE_SERIAL`, utf8proc, and USB HID/UAC/iAP.
- Resident: 1,651,968 → **1,111,296 B** (−540,672, −32.7%).
- `rockbox.ipod`: 1,143,516 → **693,332 B** (−39.4%).
- This meets the "≤ stock −20%" budget (≤ 914,813 B) using config changes plus one makefile change.

### Codec and plugin slots (whole-MiB levers)
| Change | Gain to the audio buffer | Condition |
|---|---:|---|
| `CODEC_SIZE` 1 MiB → 512 KiB | +512 KiB | Drop the chiptune codecs (kss, vgm, spc, sid, nsf, sgc, ay, gbs, hes, asap). Every mainstream codec fits in under 260 KB, and `.init` (72 KB) still fits. |
| `PLUGIN_BUFFER_SIZE` 512 → 256 KiB | +256 KiB | Ship only small plugins (battery_bench, etc.); clock, doom, pdbox and the puzzles do not fit |

## 5. Runtime DRAM: how the 60.93 MiB pool is split

Everything after `_end` is one buflib pool (`core_ctx`, `firmware/core_alloc.c`). Allocations happen in this order at boot. The audio buffer goes last and takes whatever is left (`playback.c audio_reset_buffer`, `core_alloc_maximum`).

| Consumer | Size | Source |
|---|---|---|
| Fonts | per loaded font: min(file, `MAX_FONT_SIZE` 60,000) or glyph-cache size, plus header | `firmware/font.c` |
| Skin buffers | exactly the parse usage per skin (`skin_buffer_usage()`), typically 10–60 KB | `skin_parser.c` |
| Backdrops | 153,600 B (`LCD_BACKDROP_BYTES`) **each** when a theme uses one | `skin_backdrops.c` |
| File browser cache | `max_files_in_dir` × (40 + 20) = **~293 KB** at the default of 5000, allocated at boot even if the browser is never opened | `tree.c tree_mem_init` |
| Playlist | `max_files_in_playlist` × (4 + 12 dircache ref) = 32 KB at the PP default of 2000 (512 KB at 32000) + 32 KB load buffer | `playlist.c` |
| Tagtree | parse buffer, shrunk to fit (a few KB) | `tagtree.c` |
| Voice | 0 unless a `.voice` file exists | `talk.c` |
| DSP extras | compressor/surround/PBE buffers, allocated only when enabled | rbcodec/dsp |
| USB storage | transfer buffer, allocated only in USB mode | `usb_storage.c` |
| **Tagcache RAM cache** | see formula; **off by default** (`tagcache_ram` = 0) | `tagcache.c allocate_tagcache` |
| **dircache** | see formula; **on by default** | `dircache.c` |
| **Audio buffer** | everything else, minus a 256 KB reserve; the PCM buffer and album-art bitmaps are carved from it | `playback.c`, `buffering.c` |

### Tagcache RAM cache formula
```
alloc = datasize(master hdr + 96·N index + Σ all tag files incl. filenames)
        + 12·N (dircache refs, only if HAVE_DIRCACHE) + 32 KB reserve + ~0.3 KB
string entry = 8 B header + (len+1 rounded up to 8)
```
- The index costs 96 B per track: 23 tag slots plus a flag word.
- **Bug/waste:** the filename tag file (about 8 + 72 B per track) is counted in `alloc` but never stored. `load_tagcache()` reads the paths and discards them. Nothing calls `core_shrink()` after the load (only `ramcache_used` is recorded), so that allocation is wasted for the whole session.

| Library | Used | Allocated today | Wasted |
|---|---:|---:|---:|
| 50k tracks, 4k albums, 1.5k artists, light comments | ~7.5 MB (≈150 B/track) | **~11.5 MB** (≈230 B/track) | ~4.0 MB |
| 1 TB MP3 (~125k tracks @ 8 MB) | ~19 MB | ~29 MB | ~10 MB |
| 1 TB FLAC (~33k tracks @ 30 MB) | ~5 MB | ~7.6 MB | ~2.6 MB |

The `comment` tag is uniqued but often unique per file (for example store IDs), which can add up to about 50 B per track.

### dircache formula
```
RAM = 32 B × entries + Σ(len+1 for names > 4 chars) + 64 KB reserve,
      provisioned between DIRCACHE_MIN 1 MB and DIRCACHE_LIMIT 6 MB
```
- The build first grabs min(free, 6 MB) and compacts afterwards.
- **50k tracks:** about 62k entries (tracks, about 5.5k dirs, 4k art files, system files) × (32 + ~31) ≈ **3.9 MB**.
- **Above ~95k entries the 6 MB cap is exceeded and dircache cannot build at all.** A 1 TB MP3 library hits this.
- There is no `HAVE_EEPROM_SETTINGS` on this target, so `dircache.dat` is not reused. **The whole disk is rescanned on every boot**, which works against the storage wake-up rule.

## 6. Recommendations

### Quick wins (Phase 9 or sooner, low risk)

1. **Config trims under `HAVE_IPOD_UI`** in `ipodvideo.h`/`config.h`:
   - Remove `HAVE_RECORDING`, `CONFIG_TUNER`/RDS, `IPOD_ACCESSORY_PROTOCOL` + `HAVE_SERIAL`, USB HID/UAC (and USB-iAP if no car use), `HAVE_MORSE_INPUT`, `AB_REPEAT_ENABLE` and `HAVE_USB_HID_MOUSE`.
   - Measured saving: about **−200 KB resident**.
   - First confirm that the dock connector is really gone on this unit.
2. **utf8proc:** exclude it for `HAVE_IPOD_UI` (in `tools/root.make`) and normalise tags on the host at sync time, or swap in a small composition table. Saves **−342 KB**, about 21% of the core.
3. **Tagcache:** add `core_shrink()` to `ramcache_used` after `load_tagcache()`. It is about 5 lines, `#ifdef`-guarded. This recovers about 80 B per track (**~4 MB at 50k tracks**). Then turn the RAM cache **on** by default for the shell, as `hardware.md` asks.
4. **dircache: off by default for the shell.** It saves about 3.9 MB at 50k tracks plus 12 B per track of tagcache refs. It also removes a full-disk scan at every boot and avoids the silent failure above 6 MB. Compile it out later (−11.7 KB code).
5. **File-browser cache:** allocate it lazily, or set `max_files_in_dir` to 1000 for the shell. Saves about 235 KB.

### Phase 9 follow-ups

6. **`CODEC_SIZE` 1 MiB → 512 KiB** after dropping the chiptune codecs (+512 KiB). **`PLUGIN_BUFFER_SIZE` 512 → 256 KiB** with a curated plugin set (+256 KiB).
7. **Move the 64 KB statics to on-demand `core_alloc`:** `codepage_table`, `uniqbuf`, `language_buffer` (English-only sizing), `lcd_scroll` lines, `MAX_OPEN_DIRS`. Saves about 170 KB.
   - `uniqbuf` is also an O(n²) linear dedupe (`add_uniqbuf`) that runs at browse time. That is worth fixing for 4k-album lists.
8. **Voice/talk removal** is worth doing only if core IRAM is needed (it frees 4.4 KB). Otherwise the patch cost outweighs about 42 KB.
9. **Keep:** the debug/unwinder code (needed for the dev combo and for panic-to-log), the disk cache and the framebuffer.

Net effect if items 1–7 are all done:
- Core: about 0.71 MB smaller (0.54 MB measured lean config plus 0.17 MB statics).
- Pool: about 0.75 MB larger from the slots.
- File browser cache: about 0.23 MB back.
- Database setup: at 50k tracks, the shell's setup (RAM cache shrunk, no dircache, no refs) uses about 6.9 MB, against about 15.4 MB for stock with both caches on. That is about 8.5 MB less.

**Total: roughly 10 MB more audio buffer** at 50k tracks.
