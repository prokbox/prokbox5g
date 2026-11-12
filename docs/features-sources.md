# Sources for the Features page

`docs/user/features.md` compares the iPod Video's original software (1.3), Rockbox 4.0 and ProkBox5g. It is public, so every cell must be checkable. This file lists where each cell comes from. Re-check it when a row changes, and at each release (the ProkBox5g column moves with the code).

Planned ProkBox5g cells are `{{< plan ISSUE >}}` tags. They read `site/data/roadmap.json`, which `scripts/roadmap.py generate` writes from the tracker together with `docs/user/roadmap.md`. A tag whose item has left What's coming (shipped, moved or hidden) fails the site build, so a finished item can't keep a "Planned" tag. The reverse needs a person: when an item ships, write what is true now in its cell.

## How each column was checked (2026-11-12)

- **Original software (1.3).** Apple's published specifications: support.apple.com/111923 (iPod with video, 30 and 60 GB) and support.apple.com/112454 (Late 2006, 30 and 80 GB). Apple's *iPod Features Guide (Late 2006)*, the guide for software 1.2; a private copy is kept with the 5G reference material and is not in the repo. `docs/reference/5g/SOURCES.md`, `docs/design/5g-to-7g-delta.md`. No Apple release notes exist for 1.3 (March 2008, "bug fixes"), so 1.3 is taken to behave like 1.2.
- **Rockbox 4.0.** The `v4.0-final` tag in this repository (tagged 2025-03-31), read with `git show v4.0-final:<path>`: the manual sources (`manual/`), `lib/rbcodec/codecs/SOURCES`, `lib/rbcodec/metadata/metadata.c`, `firmware/export/config/ipodvideo.h`, `apps/plugins/`, `apps/lang/`. Not run on a player.
- **ProkBox5g.** The code on this branch, the release notes (`docs/user/releases/`), the user guide (`docs/user/guide/`) and What's coming. Only one hardware variant (flash, 64 MB, modded) has been tested on a player.

## Row by row

| Row | Original software (1.3) | Rockbox 4.0 | ProkBox5g |
|---|---|---|---|
| MP3, AAC, WAV, AIFF; ALAC | Spec pages 111923, 112454 (audio formats) | `metadata.c` audio_formats; `codecs/SOURCES`; manual `appendix/file_formats.tex` | Same codecs as Rockbox: `lib/rbcodec/codecs/SOURCES` (none removed) |
| FLAC, Vorbis, Opus; WMA, APE, WavPack, Musepack; chip formats | Absent from both spec pages; Features Guide p.19 (WMA must be converted) | `codecs/SOURCES` (no PP5022 exclusion except `vtx.c`); `metadata.c` (Opus) | as Rockbox; 32-bit FLAC note: overrides.tsv #285, 0.8.2 notes |
| APE footnote (Fast level) | | `file_formats.tex` (APE -c1000 only) | same decoder |
| Audible (.aa) | Spec pages ("Audible formats 2, 3 and 4") | no codec or extension (`git grep -i audible` empty) | as Rockbox |
| Copy-protected iTunes Store songs | Spec pages ("Protected AAC") | no DRM handling in `libm4a` or `metadata/mp4.c`, no `.m4p` extension (inferred, not tested) | as Rockbox |
| DSD | Absent from spec pages | no codec, no extension | What's coming #274 |
| Gapless | Daring Fireball, "Fifth generation iPod features" (Sept 2006): gapless with 1.2; formats not stated | `file_formats.tex` codec feature table (gapless column) | same playback core (`apps/playback.c`, `apps/pcmbuf.c`); Opus check #292 |
| EQ presets | Features Guide p.28 (Settings › EQ) | no `.cfg` presets in the `v4.0-final` tree (removed upstream in 2013, commit d475dd36a3); "Browse EQ Presets" lists your own | `apps/ipodui/screens/eq.c` (Off plus 22 named presets, the later model's list: `docs/design/7g-catalogue.md`); band fix #265, volume #203 |
| Own EQ settings | Features Guide p.28 (presets only) | `lib/rbcodec/dsp/eq.h` `EQ_NUM_BANDS 10`; `sound_settings.tex` | not in the shell's settings (`eq.c`); #82 |
| Even volume | Features Guide p.28 (Sound Check) | `playback_options.tex` (ReplayGain) | `apps/ipodui/screens/settings.c` (Sound Check = ReplayGain track mode); no iTunNORM reader in `lib/` or `apps/` |
| Crossfade; other effects | absent from the Features Guide | `config.h` `HAVE_CROSSFADE`; `sound_settings.tex`; `apps/menus/sound_menu.c` | no setting in `apps/ipodui/` |
| Volume limit | Features Guide pp.26-27 (with a combination) | `sound_settings.tex`; `settings_list.c` (no code) | `apps/ipodui/vlimit.c` (limit and lock code) |
| Bit depth | not published | `firmware/drivers/audio/wm8758.c` (16-bit interface); `sound_settings.tex` (dithering to 16 bits) | `firmware/target/arm/pp/pcm-pp.c` (`SAMPLE_SIZE 16`); 24-bit #268 |
| Highest rate; files above 48 kHz | not published | `ipodvideo.h` `HW_SAMPR_CAPS` (8-48 kHz); `config_caps.h` `HAVE_PLAY_FREQ 48`; `apps/playback.c` `audio_guess_frequency` | `ipodvideo.h` (same caps); 0.8.2 notes (full speed above 48 kHz); #270, #278, #271 |
| Putting music on; copying back | Features Guide pp.21-24, p.45 (iTunes sync, disk use); copy-back limited to iTunes Store purchases (well known iTunes behaviour, "Transfer Purchases") | `manual/getting_started/installation.tex` (USB mass storage, FAT32) | `docs/user/guide/usb-and-syncing.md` |
| Reads an iTunes-synced library | n/a (its own library) | `apps/tagcache.c` `check_dir` at `v4.0-final` (walks every folder, no hidden-attribute filter, so it indexes the song files); `git grep -i itunesdb` and `iPod_Control`: no library reader | 0.8.1 notes; changelog #225 (read only, never written) |
| Adds to the original's library | | | #227 |
| Browse by artist etc.; search; folders | Features Guide pp.27-28; Search added by 1.2 (`docs/reference/5g/SOURCES.md`, citing Wikipedia), 1.3 final for all three builds (same file); Daring Fireball (Sept 2006) said 5.5 only: footnoted as unconfirmed | `apps/tagnavi.config`; `manual/main_menu/main.tex` (File Browser) | `docs/user/guide/music.md`; `IPODUI_TRIM_BROWSER` in `firmware/export/config/ipodvideo-variant.h` (no file browser) |
| On-The-Go; smart playlists; .m3u | Features Guide pp.25-26 (On-The-Go); smart playlists made in iTunes | `manual/working_with_playlists/`; `tagnavi.config` (most/recently played); `apps/filetypes.c` (.m3u, .m3u8) | `docs/user/guide/music.md`; `apps/ipodui/otg.c`; `apps/ipodui/screens/playlists.c` (.m3u/.m3u8 under /Playlists); #139 |
| Album art; cover browser | Features Guide p.28; no cover browser on the 5G (`5g-to-7g-delta.md` #15) | `ipodvideo.h` `HAVE_ALBUMART`; `manual/plugins/pictureflow.tex` (no embedded art) | 0.8.1 notes (art sources); #6 |
| Audiobooks menu; chapters; resume; speed | Features Guide p.30 (chapters, resume, Slower/Normal/Faster for Store and Audible books) | no chapter reader (`metadata/mp4.c` "ADDME: add support for real chapters"); `playback_options.tex` (cue sheets); `bookmarking.tex`, `autoresume.tex`; `tdspeed.h` (35-250 %), `wps.tex`, `sound_settings.tex` (timestretch) | `docs/user/guide/audiobooks.md`; `tests/audiobook/golden/` (Nero, Apple and ID3 CHAP chapters); `docs/design/changes-from-7g.md` |
| Podcasts | Features Guide pp.29, 31 | no podcast menu; `settings_list.c` (auto-resume directories) | `docs/user/guide/audiobooks.md`; #164 |
| H.264, MPEG-4 | Spec pages (5G: H.264 768 kbit/s 320 × 240, MPEG-4 2.5 Mbit/s 480 × 480; 5.5: also 640 × 480) | `manual/plugins/mpegplayer.tex` (MPEG-1/2 only) | `docs/user/guide/video.md`, `docs/user/guide/video-converter.md` (320 × 240 H.264 after conversion); #185, #169 |
| MPEG-1/2; AVI, MKV, WebM | absent from spec pages | `mpegplayer.tex` | #187, #188, #189 |
| TV | Features Guide pp.31-36 | no TV-out code in `apps/` or `firmware/export` | TV Out hidden: `apps/ipodui/screens/setvideos.c` |
| Photos | Spec pages; Features Guide pp.37-44 | `manual/plugins/imageviewer.tex` | hidden: 0.8.0 notes; `docs/design/changes-from-7g.md`; no tracker item, so "may return (no date)" |
| Games; extras | Features Guide p.48 (games bought from the iTunes Store); 1.2 added games (`docs/reference/5g/SOURCES.md`); built-in games on older iPods, unconfirmed for 1.3; pp.46-52 (Clock, Stopwatch, Screen Lock, Contacts, Calendar, Notes) | `apps/plugins/SOURCES`, `SUBDIRS`, `manual/plugins/main.tex`; `ipodvideo.h` `HAVE_RTC_ALARM` | Extras today: the owner's check on a player running the 0.8.2 build, 2026-11-12 (the menu works); the list from `apps/ipodui/extras.c` and the modules in `apps/ipodui/modules/modules.make` (clocks, alarms, stopwatch, screen lock, notes, contacts, calendars). Fuller set and games: #32; game kits: #304 |
| Languages; CJK | Spec pages (21 languages; CJK listed) | `apps/lang/` at `v4.0-final`: 48 `.lang` files, English and US English among them; the page says "more than 40"; `fonts/` (CJK fonts) | `apps/ipodui/screens/language.c` (English only); #273, #266, #235 |
| Accessories | Features Guide pp.46, 52-53 | `ipodvideo.h` `IPOD_ACCESSORY_PROTOCOL`; `apps/iap/`; `system_options.tex` ("partially implemented") | `ipodvideo-variant.h` (`IPODUI_TRIM_IAP` unless `HAVE_IPOD_DOCK30`); `scripts/release_flags.sh` (stock variants built with `--dock30`, modded without); untested on a player; #205, #236 |
| Sleep | well known 5G behaviour (sleep, instant wake); footnoted as unconfirmed | `firmware/target/arm/ipod/power-ipod.c` (power off; bootloader reloads each start) | 0.8.0 notes (no sleep yet); #86 |
| Processor speed | not published | `system-target.h`, `ipodvideo.h` `HAVE_ADJUSTABLE_CPU_FREQ` | `docs/hardware.md` (24/30/80 MHz) |
| Look; settings | `5g-to-7g-delta.md` | `manual/configure_rockbox/theme_settings.tex`; `apps/settings_list.c` at `v4.0-final` (about 250 setting entries: "well over two hundred") | `docs/design/changes-from-7g.md`; CLAUDE.md rule 1 |
| Installing; going back | Features Guide (updating through iTunes) | `installation.tex` (Rockbox Utility); `bootloader/ipod.c` at `v4.0-final`, the comment at line 252 (Hold on, or Menu held, boots the original) | `docs/user/guide/getting-started.md`; `docs/user/guide/going-back.md`; #128 |
| Licence | proprietary | `apps/main.c` header (GPL v2 or later) | `docs/legal.md` |

## Open questions for the owner

- **Extras today:** settled by the owner on 2026-11-12. The menu works on the 0.8.2 build, so the page says "Yes" for what ships, and keeps #32 for the fuller set and the games.
- **The name "Sound Check".** Settled (owner, 2026-11-12): keep it. It is a short functional label, matching the 7G's menu wording as rule 2 allows for short labels, and is not in docs/legal.md's list of marks kept out of names.

## Disclaimer

The short non-affiliation text from `docs/legal.md` section 2 is in the site footer (`site/layouts/partials/footer.html`), on every page including this one.

## Left out on purpose

- **Which built-in games on 1.3.** Brick, Music Quiz, Parachute and Solitaire came on older iPods; no primary source confirms them for 1.3, so the page says only that a few may have come on the player.
- **Battery life.** Apple's figures (up to 14-20 hours of music by model) are for new batteries and its own tests; Rockbox and ProkBox5g figures vary by unit. No fair comparison.
- **Exact original EQ preset count, maximum sample rate, bit depth and gapless formats.** Apple did not publish them.
- **Digital audio over USB.** Not checked for Rockbox 4.0.
- **Voice recording.** Rockbox 4.0 enables line-in and radio recording on this player, but it was not checked on hardware.
