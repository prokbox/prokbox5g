# BCM2722 host protocol: working notes

**Status: early static findings (2026-10-09, issue #60).** Everything here comes from Apple's 1.3 updater files (`artefacts.md`) using strings, ELF headers and a first disassembly pass. Nothing has been confirmed on hardware or in an emulator. Each claim carries a confidence level: **[H]** seen directly in the binaries, **[M]** a strong inference, **[L]** a guess.

## 1. The big picture: the video stack is loaded from disk

- [H] Apple's video playback does **not** run from the NOR-flash `vmcs` image that Rockbox uploads today (`flash_get_section(ROM_ID('v','m','c','s'))`, `firmware/target/arm/ipod/video/lcd-video.c`). The firmware partition's `rsrc` volume carries a separate, larger VideoCore stack:
  - `/Resources/VideoCore/Boot/vmcs.bin` (197 KB): the VMCS run-time OS.
  - `/Resources/VideoCore/Boot/RenderServer.bin` (102 KB).
  - `/Resources/VideoCore/Library/*.vll`: dynamically loaded libraries for `h264dec`, `mpg4dec`, `aacdec`, `mplayer` (the media-player engine), `passthruhandler` and `slideshow` (Photos).
- [H] `osos` contains the strings `vmcs.bin`, `RenderServer.bin` and `set_vll_dir %s` next to each other (around `0x101afc80`), plus a "Channel VideoCoreBoot" disk channel. [M] So the ARM reads these files from disk at video start, uploads `vmcs.bin` and RenderServer, and points the BCM at its library directory.
- [H] The chip identifies as **VideoCore II**: the C++ classes `DisplayDriverVC02`, `LcdDriverVC02`, `DtvDriverVC02`, and `VC02LOCL`.
- [H] The `.vll` files are **ELF32 little-endian, `e_machine` = 95 (EM_VIDEOCORE)**, relocatable shared objects linked by "MetaWare Universal Linker v5.3h", with `.dynsym` intact. That's 199 named symbols in `mplayer.vll` and about 390 each in `h264dec`/`mpg4dec`, for example `mp_register_gencmds`, `mp_disptask_*`, `H264InitDecoder`, `H264DecodeFrame`, `H264ReleaseDecoder` and `h264_writestripe_*`.
- [H] `h264dec.vll` and `vmcs.bin` changed between 1.2.x and 1.3. Everything else in `/Resources/VideoCore` is identical, so diffing these narrows 1.3's video changes.

**Superseded (owner, 2026-10-10):** we write our own VideoCore firmware; Apple's stack is reference only. See `approach.md`.

## 2. The control channel is text ("gencmd")

[H] The ARM drives the media player with **text commands**, the same "gencmd" design Broadcom later used (and documented) on VideoCore IV (`vc_gencmd`). Strings in `osos`, with addresses for 1.3:

| Group | Command format strings (osos) |
|---|---|
| boot / power | `set_vll_dir %s`, `pm_set_policy min`, `pm_show_stats 0 10 10 90 16` |
| media player | `mp_selectplay passthru:%s %d`, `mp_play`, `mp_pause`, `mp_stop`, `mp_seek %d`, `mp_step %d`, `mp_get_status`, `mp_get_stats video`, `mp_get_stats audio` |
| layout | `mp_region display=%d dest=fullscreen mode=%s`, `mp_region display=%d dest="%d %d %d %d" mode=%s`. Modes seen: `letterbox`, `fill`, `letterbox pixel="1 1"` |
| display / TV | `display_control %d model=%d power=%d`, `display_control %d dac=0`, `display_control %d dac=1 encoding=%d`, `display_control %d freeze=%d` |
| slideshow | `ss_selectplay i:black?width=%d&height=%d`, `ss_selectplay i:next:%s?width=%d&height=%d`, `ss_region …`, `ss_stop`, `ss_get_status`, `ss_trantype %s`, `ss_tranprop %s %s`, `ss_trantime %ld`. Transitions: push, alphawipe, horiz, curtains, cube, pageflip, spiral, radial, dissolve. Pixel formats: rgb565, yuv422i |

[H] Replies are text too (from `mplayer.vll`): `state=%s mode=%s error=%d paused=%s …`, `duration=%lu elapsed=%lu`, `error=%d error_msg="…"`, plus statistics such as `lost_frame_assert_starvation`, `min_time_ahead` and `num_frame_buffers`.

[M] `mp_selectplay passthru:<name> <n>` plus `passthruhandler.vll` means that **the ARM demuxes the file and streams the elementary streams (packets) to the BCM**, and the BCM decodes, schedules and displays them itself. The `mplayer.vll` stats name the failure modes of that path: packets out of order, too big, late (starvation). [L] It's unknown whether audio goes through `aacdec.vll` on the BCM or stays on the PP. For ProkBox, the plan is to keep audio on the Rockbox codec path and make the BCM video-only, if A/V sync allows it (the stats include `min/max_time_ahead`, so the BCM schedules frames by timestamp).

## 3. The bus transport (where the next work is)

- [H] The register map matches Rockbox's (`lcd-video.c`): DATA, WR_ADDR, RD_ADDR and CONTROL at `0x3000_0000/1/2/3 0000`, and a second **ALT** bank at `0x3004..7 0000`. Rockbox only uses the first bank.
- [H] Every literal for all eight registers sits in one small driver in `osos`, at about **`0x102878f0–0x10287dd0`** (1.3). Several of its functions pick the bank with a flag (for example `ldrne r5,=0x30070000 / ldreq r5,=0x30030000`). [M] So Apple uses **two host ports**, probably one for control and messages and one for bulk data.
- [L] Literals `0x1F8` (12×), `0x1FC` (19×) and `0xE0000` (11×) appear elsewhere in `osos`. They may be Rockbox's known command and status mailbox, or simply common constants. Each one still needs checking.
- **Open:** how a gencmd string and a stream packet are framed on the bus (message-queue layout in BCM memory, doorbell, reply polling or IRQ); the `vmcs.bin` upload and start sequence (versus Rockbox's NOR `vmcs` upload); and where the BCM keeps its stream FIFO.

## 4. Next steps
1. Ghidra project for `osos.code` (ARM v4T, base `0x10000000`): name the bus driver at `0x10287xxx`, follow callers upward to the message layer and to the `mp_*` gencmd senders, and recover the message framing.
2. Ghidra or rizin, VideoCore side: the ISA is VideoCore II, so check whether VC4 processor modules decode it at all. Even without a disassembler, the `.vll` `.dynsym` names plus `.rela` give structure, and `mp_register_gencmds` should list the full command table.
3. The unicorn harness (`tools/bcmre/`) to trace the OF's bus traffic for the start, select and play sequence.
