# ProkBox5g

**ProkBox5g: a device-tuned fork of Rockbox for the iPod Video (5G/5.5G).**

ProkBox5g is a community-driven, open-source firmware from the ProkBox project. It brings the look, feel and behaviour of the iPod Classic's final software (2.0.5) to the iPod Video 5.5G. It is built on the [Rockbox](https://www.rockbox.org) open-source firmware, which supplies the kernel, drivers, playback engine, codecs and music database. On top of that sits a native user interface designed to feel like the iPod Classic 7G: split-screen menus with live previews, smooth lists, Cover Flow, a 7G-style Now Playing screen, and first-class audiobooks with chapters, per-chapter art, resume and speed control.

It is a community-driven project. Development started on one modified iPod Video 5.5G (1 TB flash, 3000 mAh battery, USB-C, haptic clicker), and the same build is meant for stock 5G and 5.5G units. The *5g* in the name marks this build line; other hardware, if it ever comes, gets its own.

## Credits
Full list in `CREDITS`.
- **ProkBox** is by prokopto-dev, released under the **GNU General Public License v2 or later** (`docs/COPYING`). Every file the project adds carries the ProkBox header (`docs/templates/prokbox-header.txt`).
- **Rockbox**: this firmware is a fork of Rockbox (<https://www.rockbox.org>, <https://github.com/Rockbox/rockbox>), the work of many contributors over two decades, also GPL v2 or later. Files taken from upstream keep their original headers and credits. The upstream history is preserved in this repository.
- **Fonts:** TeX Gyre Heros (GUST Font License), shipped as "ShellSans". Licence texts are in `apps/ipodui/fonts/`.
- **Codec and support libraries** (under `lib/`) keep their own licences and notices.
- **Artwork:** all glyphs, gradients and images are original to this project.
- **Test footage:** the device test's second video clip (`bcmfw/video/stage6-real.mp4`, developer builds only) is a 13-second excerpt of the *Sintel* trailer, (c) Blender Foundation | durian.blender.org, licensed under [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/). It is cropped, scaled and re-encoded by `bcmfw/video/mkclip-real.sh`. The converted test clip (`bcmfw/video/stage7-conv.m4v`, developer builds only) is a 40-second excerpt of the same trailer, under the same licence, cropped and converted by `bcmfw/video/mkclip-conv.sh`.

## Not affiliated with Apple
**ProkBox5g is not Apple software and not an Apple product.** It is free, open-source, community-driven firmware: a fork of Rockbox, tuned for the iPod Video (5th generation and 5.5). Apple Inc. did not make it, has not approved or endorsed it, does not support it, and has no connection with this project. If something goes wrong with ProkBox5g, ask the ProkBox community, not Apple. iPod, iPod classic, iTunes, Click Wheel, Cover Flow and Apple are trademarks of Apple Inc., registered in the U.S. and other countries. We use those names only to say which hardware ProkBox5g runs on. ProkBox5g contains no Apple software, fonts, logos or artwork. Reference material used during development stays private and is never committed.

## Contact
Questions, bug reports that don't fit the issue tracker, and trademark or takedown requests: **prokbox.project@gmail.com**. We answer complaints within a few days.

## Licence
- **Code:** GNU General Public License, version 2 or (at your option) any later version (`docs/COPYING`). Rockbox's own files keep their original copyright headers; files we changed are listed, with what changed, in `docs/changes-from-rockbox.md`, and from v1.0 each also carries a change line in its header.
- **Third-party code and fonts:** their own licences, kept in `docs/LICENSES`, under `lib/` and in `apps/ipodui/fonts/`.
- **Docs and site prose:** CC BY-SA 4.0.
- Every release ships its complete corresponding source next to the binaries. How each obligation is met is recorded in `docs/legal.md`.

## Status
In active development and **not yet tested on hardware**. Do not install it on a device whose contents you haven't backed up. See `docs/ROADMAP.md` for phases and status.

## For contributors and agents
Start with `CLAUDE.md`, then `docs/process.md`, `docs/hardware.md` and `docs/design/spec/index.md`. Build instructions are in `CLAUDE.md`. Rockbox's own build notes are in `docs/README`.
