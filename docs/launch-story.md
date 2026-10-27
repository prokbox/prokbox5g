# Launch story, version ladder and finish line

*Owner, 2026-10-26.* How ProkBox5g is introduced in public, what each version brings, and what qualifies us for 1.0. The release mechanics (tags, zips, gate, staging) are in `docs/release-plan.md` §3; the 0.8.x gate items are §1.3a there. When this file and the release plan disagree about scope, the owner decides and both are updated.

## 1. The story we tell

- **How it started:** the owner wanted better audiobook support in Rockbox (chapters, per-book resume, speed). One fix led to the next and became a whole firmware.
- **What it is:** free, open-source firmware for the iPod Video (5G/5.5G), built on Rockbox, that feels like the last classic iPods: their menus and look, plus drag-and-drop music, every format Rockbox plays, and proper audiobooks.
- **Why it's different:** free, with nothing to pay or unlock. GPL with the full source, and honest credit to Rockbox. Apple's firmware stays on the iPod as the way back (hold MENU at start-up).
- **Honest notes:** developed with the help of AI agents (the repo description says so). Pre-release: tested on one flash-modded 5.5G with 64 MB so far; hard-drive and 32 MB testers wanted.
- Every post carries the short disclaimer from `docs/legal.md` §2. Full sentences are in our own words, never Apple's.

## 2. Version ladder

| Version | What it brings | Docs that ship with it |
|---|---|---|
| **0.8.0 (first pre-release)** | Music, audiobooks, USB drag-and-drop, basic video playback (release-plan §1.3a), and the Now Playing visualizer (#126; owner, 2026-10-26) | User guide **v0.8**: install and restore, drag-and-drop, audiobooks, video converter guide, known issues |
| **Later 0.8.x** | Photos, the stock hard-drive run, video polish (release-plan §1.3a) | User guide 0.8.x updates |
| **0.9** *(proposed; owner to confirm)* | The finish-line features arrive for testing: Album Flow, sleep and hibernation, the games SDK (class B and C) | User guide v0.9; SDK guide draft |
| **1.0** | Every finish-line item below done and verified on hardware, after the pre-release feedback | Full user and technical docs, public hardware references |

Every release ships release builds only (developer mode compiled out). Developer builds are not published; anyone can build one from the source (owner, 2026-10-26). The UI moving to the second core (19-9) lands **before 0.8.0** (owner, 2026-10-26).

## 3. The finish line (what qualifies 1.0)

1. **Album Flow**: the cover-flow-style album browser.
2. **Sleep and hibernation**: the iPod sleeps and wakes instantly, so a full shut-off is rarely needed (ROADMAP P18).
3. **Games SDK**: public docs and tools for class B (borrows RAM) and class C (exclusive, like video) games, with the signing and allow-list policy.
4. **Hardware references, tested and public**: the PP5022 platform manual and the BCM2722 video-processor manual, every fact tagged by evidence, in repositories of their own under the `prokbox` org.
5. **ProkBox5g documentation**: the user guide (with screenshots) and the technical docs (build, architecture, development).

## 4. After 1.0 (future roadmap)

Listed in no particular order: this is not a priority ranking, and nothing here has a date (owner, 2026-10-26).

- **Smart playlists**, from the play statistics the library already keeps:
  - *Top 25 Most Played*;
  - *Favorites*: add the current song from the Select-hold menu in Now Playing.
- **Smart Mixes** (P13, #33): playlists of songs that go well together, built on the iPod itself from your library. Our own name (owner, 2026-10-26): "Genius" is an Apple mark and never names a feature (`docs/legal.md` §1).
- **Dark mode**: an Appearance setting; Light stays the default.
- **Custom EQ** (#82): three user slots, built in Extras and saved across restarts, chosen in Settings › EQ next to the stock presets, each with a small frequency-response curve. The engine follows the curve: one that fits the DAC's own 5-band EQ (WM8758: a low shelf, three peaks, a high shelf) runs in hardware at no CPU cost; one that needs more bands or shapes the DAC can't match runs as the software EQ.

- **More languages** (P22): 1.0 is English only; accented Latin already displays.
- **Warm screen filter**: a lower-blue tint for night use (off, on, or by the clock), next to dark mode. Needs our own name: "Night Shift" is an Apple mark.
- **Faster library updates after drag-and-drop**: rescan only changed folders; on a Mac, read macOS's own change log on the volume.
- **Companion app** (#53, P21): optional syncing for music, podcasts and audiobooks. Drag-and-drop stays first-class.
- **Soft subtitles for video**: until then, the converter burns subtitles into the picture.
- **Big flash mods on Macs** (#81): mounting very large cards without a helper, via a one-time reformat.
- **One download for flash and hard drive** (#79): the iPod detects its storage itself, halving the release zips.
- **USB extras, maybe**: the click wheel as a remote for the computer, or the iPod as a USB sound card.
- **Our own install and recovery** (#62, #122; v2): no Apple firmware needed at any stage.
- **More games** once the SDK is out, e.g. a rhythm game (#124).

**Giving back to Rockbox (owner, 2026-10-26):** once ProkBox5g is stable at **1.2**, we start sending fixes and improvements back to upstream Rockbox, from the list we keep as we go (`docs/upstream-backports.md`).

## 5. Launch post kit (r/IpodClassic, then r/ipod)

Media to capture, with notes:

| Item | Notes |
|---|---|
| Short video: navigating the menus | Real device, steady shot; covers Music, Now Playing, Settings |
| Short video: a video clip playing | Silent is fine (audio goes to headphones). An openly licensed clip (e.g. *Sintel*, *Big Buck Bunny*) avoids takedown trouble on a public post |
| The VC 3D rendering sample | The existing BCM2722 3D demo capture: shows the video chip doing real work for games |
| The music visualizer | Short capture during playback |
| A few screenshots | Simulator or device: main menu, Now Playing, an audiobook with chapters |

Text: the story in §1, the version ladder in §2, the finish line in §3, links to the repo, the guide and the tester issue form, and the short disclaimer.
