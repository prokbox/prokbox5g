# 5G/5.5G firmware 1.3 → iPod classic 2.0.5: delta

This lists what Apple changed between the 5.5G's final firmware (1.3) and the iPod classic's final software (2.0.5). That gap is exactly what the North star asks us to rebuild.
- Source keys (G5, G07, G08, G12, MM7, IMG, SPEC) are defined in `7g-catalogue.md` §0.
- The 5G column describes fw 1.2 per G5. 1.3 is believed to have the same UI (`docs/reference/5g/SOURCES.md`) ⚠️.
- ⚠️ marks anything unverified.

**Rebuild?**
- **yes**: match stock.
- **adapt**: same idiom, different mechanism or content.
- **skip**: leave out.

**Phase** numbers are from `docs/ROADMAP.md`. **new** means the roadmap does not cover it yet.

## Look and feel
| # | Area | 5G fw 1.3 | Classic 2.0.5 | Source | Rebuild? | Phase |
|---|---|---|---|---|---|---|
| 1 | Font | Podium Sans (bitmap) | Helvetica Bold (anti-aliased) | SPEC, `fonts.md` | adapt (TeX Gyre Heros, decided) | 3 ✅ |
| 2 | Main menu layout | Full-width list | **Split screen**: list on the left half, slow pan/zoom art panel on the right; Settings panel shows capacity | IMG `main*.png`, `photo-main-6g-vs-5g.jpg` | yes | 3/4 (in SPEC) |
| 3 | Chevrons | On every row that leads to a submenu ("An arrow next to a menu item indicates…", G5 p.9) | Same wording in G12 p.8, but drawn **only on the selected row** | IMG, SPEC | yes | 3 ✅ |
| 4 | Selection bar, header, battery, gloss | Flat blue bar, 5G header | Glossy gradient bar, silver header, new glyphs. Charging shown with a bolt/plug glyph; the 5G animated the battery icon while charging (G5 p.9). | IMG, SPEC | yes | 3 ✅ / 8 |
| 5 | Music menu | Full list | **Split screen with art** | IMG `photo-music.jpg` | yes | 4 |
| 6 | Album/podcast lists | Single-line | **Two-line rows with thumbnails**, "n Songs" subtitle | IMG `photo-artist-albums.jpg`, `photo-podcasts.jpg` | yes | 4 |
| 7 | Now Playing art | Small, flat art (and on the 5G a separate full-art screen: "press the Center button twice", G5 p.29) | Large **slanted 3D art with reflection** | IMG `nowplaying*.png`, G5 p.29 | yes | 5 |
| 8 | Transitions | Slide | Slide (classic speed) ⚠️ | SPEC | yes | 3 |
| 9 | USB / charging screens | "Do not disconnect" screen with a 5G icon (G5 p.12) | Dark-header full screens: Connected / Synchronizing / Ejecting / OK to Disconnect (progress bar) / Charging / Charged / Charging, Please Wait... / Connect to Power | IMG pdf160, G12 p.10–12 | yes (own glyphs) | **new** (8: "native polish") |
| 10 | Quick-scroll letter | Letter overlay. Symbols/numbers **before A**. Only for lists of more than 100 items (G5 p.7). | Letter overlay, also in Cover Flow. Symbols/numbers **after Z** in G12 (before A in G08) | G5 p.7, G08 p.7, G12 p.9 | yes. Check `sortkey.c` ordering. | 3 (done?) / 7 |

## Navigation and menus
| # | Area | 5G fw 1.3 | Classic 2.0.5 | Source | Rebuild? | Phase |
|---|---|---|---|---|---|---|
| 11 | Main menu items | Music, Photos, Videos, Extras, **Settings (5th)**, Shuffle Songs (+Now Playing) ⚠️ | Music, **Videos, Photos**, **Podcasts**, Extras, **Settings (6th)**, Shuffle Songs, Now Playing | G5 p.58 ("fifth menu item"), G12 p.51 ("sixth"), IMG | yes. Videos/Photos hidden (SPEC). | 4 |
| 12 | Podcasts location | **Music › Podcasts** (G5 p.29); video podcasts under Videos | **Main menu › Podcasts** (audio and video) | G5 p.29, G12 p.32 | yes | 4 |
| 13 | Main Menu customisation | Settings › Main Menu, each item toggled **Off → On** | Settings › Main Menu with **checkmarks** | G5 p.9, G12 p.8 | yes | **new** |
| 14 | Music Menu customisation | None. Only **Settings › Compilations On/Off** | **Settings › Music Menu** (checkmarks + "Reset Menu"). Compilations moved here. | G5 p.28, G12 p.30 | yes | **new** |
| 15 | Cover Flow | — | Music › Cover Flow | G12 p.26 | yes | 7 |
| 16 | Search | Music › Search. On-screen alphabet with back arrow and **DONE**. "Click DONE to display the found list". Menu returns to Search. Songs have no icon. | Wheel alphabet. **Prev = delete, Next = space, Menu = results list**. Every result has a type icon. Center returns to Search. | G5 p.7, G12 p.30 | yes | **new** |
| 17 | Hold-Center action | Hold "until the song title flashes" = add to/remove from On-The-Go | Hold → **popup menu**: Start Genius / Add to On-The-Go / Remove from On-The-Go / Browse Artist / Browse Album | G5 p.25–26, G12 p.6, 28–29 | yes (no Genius) | **new** |
| 18 | On-The-Go save/clear | "Save Playlist > Save Playlist"; Clear Playlist | "Save Playlist"; "Clear Playlist" → **Clear** confirm; saved as "New Playlist 1" | G5 p.26, G12 p.28 | yes | **new** (4 lists OTG only) |
| 19 | Browse Artist / Album from Now Playing | — | Via the hold menu | G12 p.29 | yes | **new** (5) |
| 20 | Genius playlists | — | Start Genius / Save Playlist / Refresh. Needs iTunes data. | G12 p.27 | skip | — |
| 21 | Genius Mixes | — | Music › Genius Mixes, dark paged UI (2.0.3+) | G12 p.27, IMG | skip, or adapt as local mixes (owner) | — |
| 22 | Shuffle Songs | Same (skips audiobooks and podcasts) | Same | G5 p.25, G12 p.29 | yes | 4 |
| 23 | iTunes U | — | Main-menu item when present | G12 p.32 | skip | — |

## Now Playing
| # | Area | 5G fw 1.3 | Classic 2.0.5 | Source | Rebuild? | Phase |
|---|---|---|---|---|---|---|
| 24 | Center cycle | Scrubber (diamond) → album art → rating → lyrics → podcast info / audiobook **Speed** ⚠️ order. Scrubber: "Press the Center button to resume playing from that point" (G5 p.6). | Scrubber → [Genius slider] → **Shuffle slider** → Rating → [Lyrics] (G12 table order). Owner: Scrubber → Rating → Shuffle; audiobooks skip Shuffle. | G5 p.24–25, 29–30, G12 p.25, SPEC | yes (owner order). Add Lyrics (adapt). | 5 |
| 25 | Shuffle in Now Playing | — (Settings only) | **Shuffle slider: Off / Songs / Albums** | G08 p.28, G12 p.25 | yes | 5 |
| 26 | Rating UI | "rating screen (showing bullet points, stars, or both)" | Five bullets → stars on the progress-bar row | G5 p.26, G12 p.26 | yes | 5 |
| 27 | Audiobook speed in Now Playing | "Press the Center button until you see the Speed menu item … set Speed to Slower or Faster" | Not documented; Settings › Audiobooks only ⚠️ | G5 p.30, G12 p.33 | adapt: Settings only, which is stock. Owner may want a hidden in-cycle speed. | 6 |
| 28 | Audiobook chapters | Next/Prev chapter | Next/Prev chapter. **Chapter markers on the bar.** Book → chapter list with **Resume**. | G08 p.34, G12 p.33 | yes | 6 |
| 29 | Volume overlay / limit lock | Lock icon to the right of the volume bar when capped | Triangle marks the limit on the volume bar | G5 p.27, G12 p.30 | yes | 5 |
| 30 | Lyrics | Yes (center cycle) | Yes (center cycle, scrollable) | G5 p.29, G12 p.26 | adapt (embedded/.lrc) | **new** (5) |

## Settings
| # | Area | 5G fw 1.3 | Classic 2.0.5 | Source | Rebuild? | Phase |
|---|---|---|---|---|---|---|
| 31 | Backlight item name | "**Backlight Timer**" | "**Backlight**" (G12; G08 still "Backlight Timer") | G5 p.10, G08 p.10, G12 p.8 | yes | 8 |
| 32 | Reset | "**Reset All Settings**" (last item) → second item "Reset" | "**Reset Settings**" (last) → "Reset" / Cancel → language picker | G5 p.58, G12 p.9, 51 | yes | 8 |
| 33 | Compilations toggle | Settings › Compilations | Removed; now in Music Menu | G5 p.28, G12 p.30 | yes | 8 |
| 34 | Volume Limit flow | Wheel → **Center** sets → "Set Combination" / "Done"; Center accepts changes | "**Set Volume Limit**" sub-item → Center sets → Menu = no code, or "Enter Combination"; **Play/Pause** accepts changes | G5 p.26–27, G12 p.30–31 | yes | **new** |
| 35 | Date & Time | Set Time Zone, Set Date & Time, Time (12/24), Time in Title | Date, Time, Time Zone (city), [DST in 2.0 guide only], 24 Hour Clock, Time in Title | G5 p.46, G08 p.49, G12 p.41 | yes | **new** |
| 36 | Contacts sort | Contacts settings ⚠️ | **Settings › Sort By** First/Last | G12 p.45 | yes if Contacts built | **new** |
| 37 | Clicker | On/Off (speaker) | On/Off (earphones and speaker per G12) | G5 p.8, G12 p.9 | adapt (haptic) | ✅ (SPEC) |
| 38 | About | Settings › About (G5 p.6) | About with **3 pages**, Center cycles (capacity, counts, S/N, model, version) | G12 p.9, MM7 | yes, with our version | **new** (8) |
| 39 | EQ, Sound Check, Brightness, Audiobooks, Shuffle, Repeat, Language, Legal | Present | Present, same names ⚠️ (EQ list unchanged ⚠️) | G5, G12 | yes | 8 |

## Extras
| # | Area | 5G fw 1.3 | Classic 2.0.5 | Source | Rebuild? | Phase |
|---|---|---|---|---|---|---|
| 40 | Clock(s) | "**Clock**": New Clock; "Delete This Clock" + confirm | "**Clocks**": Center → Add / Edit; Delete | G5 p.46, G12 p.41, MM7 | yes (small city table) | **new** |
| 41 | Alarms | Per clock: Clock › [clock] › **Alarm Clock** (Alarm, Time, sound). A bell icon marks a clock with an alarm. | Separate **Extras › Alarms**: Create Alarm (Alarm, Date, Time, Repeat, Sound/Tones, Label), multiple alarms | G5 p.47, G12 p.42 | adapt (⚠️ RTC wake support) | **new** |
| 42 | Sleep Timer | Clock › [clock] › Sleep Timer. Now Playing shows a clock icon and minutes left. | **Extras › Alarms › Sleep Timer** (15/30/60/90/120) | G5 p.47, G12 p.42, MM7 | yes | **new** |
| 43 | Stopwatch | Stopwatch › **Timer**: Start / Lap / Pause / Resume / Done | Play/Pause start/stop, **Center = lap** (3 shown), Resume, New Timer; logs with Delete Log / Clear Logs | G5 p.47–48, G12 p.42–43 | yes | **new** |
| 44 | Screen Lock | Set Combination ("Enter New Code"), "Turn Screen Lock On > Lock", Change Combination ("Enter Old Code"). A wrong code "flashes red". | "New Combination" → "Confirm Combination" → Lock / Reset. Wrong code: "the lock remains". | G5 p.49, G12 p.43–44 | yes | **new** |
| 45 | Games | Purchased iPod games only (G5 p.48). Older iPods had Brick, Music Quiz, Parachute, Solitaire (MM7). | Built-in **iQuiz, Klondike, Vortex** | G5 p.48, G12 p.43, MM7 ch02s18 | owner decision (skip or originals) | — |
| 46 | Calendar(s) | "Calendar" › "To Do" | "Calendars" › "To Do's" | G5 p.51–52, G12 p.45 | owner decision (costly) | — |
| 47 | Contacts, Notes | Same | Same | G5, G12 | adapt (Notes cheap) | **new** |
| 48 | Voice Memos | Mic on the dock connector | Also the Apple Earphones with Remote and Mic | G5 p.52, G12 p.46 | skip | — |

## Video and photo (out of scope, recorded for completeness)
| # | Area | 5G | 2.0.5 | Source | Rebuild? |
|---|---|---|---|---|---|
| 49 | Video settings | Videos › **Video Settings**: TV Out, TV Signal, **Widescreen** | Videos › **Settings**: TV Out, TV Signal, **TV Screen, Fit to Screen, Alternate Audio, Captions, Subtitles** | G5 p.36, G12 p.35 | skip |
| 50 | Photos | **Photo Library**, **Slideshow Settings**, Photo Import (camera connector) | **All Photos**, **Settings** | G5 p.43, G12 p.38 | skip |

## Top 10 deltas not yet in ROADMAP (proposed home)
1. **Search** letter-wheel screen (#16): Phase 4 add-on or new Phase 4b.
2. **Hold-Center popup menu** and **On-The-Go** add/remove/clear/save, Browse Artist/Album (#17–19): new phase after 5.
3. **Main Menu / Music Menu checklists** and Reset Menu (#13–14): Phase 8.
4. **Volume Limit** with combination (#34, #29): Phase 8.
5. **Lyrics** step in Now Playing (#30): Phase 5.
6. **System screens**: USB connected/sync/eject/OK, charging, low-battery (#9). Phase 8 says "native polish" but doesn't list them.
7. **Extras: Clocks, Alarms, Sleep Timer** (#40–42): new phase.
8. **Extras: Stopwatch, Screen Lock, Notes, Contacts + Sort By** (#43–44, 47, 36): new phase.
9. **About pages, Date & Time, Time in Title, Legal, Reset Settings flow** (#31–32, 35, 38): Phase 8 (Apple-style Settings).
10. **Podcast specifics**: unplayed blue dots, reverse-chronological episodes, per-episode resume (catalogue §4). Phase 4 lists Podcasts but not these behaviours.

Owner decisions are pending on Genius Mixes substitute (#21), games (#45), calendars (#46) and language support.
