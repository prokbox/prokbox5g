# Settings

*Split from `7g-spec.md` "Global chrome" and "Menu trees" (2026-10-07).*

## Split screen

- **Settings split screen (photos `settings-01-about.jpg` … `settings-17-reset-settings.jpg`):** list at the left (x 0–159, header above it, 25 px rows, scrollbar at x≈155). Right panel x 160–319, full height, dark navy/slate gradient, no divider, updating as the selection moves:
  - **Title:** the selected item's name, white bold (list size), centred at x≈240, centre y≈25 *(≈)*.
  - **Icon tile:** a dark rounded square ≈76×76 px at y≈80–156, centred at x≈240, holding a light glyph, with a faint **mirror reflection** ≈20 px below it *(≈)*. About shows the capacity panel; Language shows drifting multilingual "Welcome" words instead; Legal a large logo (ours, not Apple's).
  - **Caption:** centred white regular-weight text at y≈210 *(≈)*, wrapping to 2–3 lines: a description, or the current value for Backlight ("10 Seconds"), Brightness ("0%"), EQ (preset name), Date & Time (weekday). The Audiobooks row describes its submenu: "Set Speed and Sort Order" (owner, 2026-10-10; the 7G shows the speed, which now sits on the submenu's Playback Speed row). Full per-row list in `7g-catalogue.md` §7.1. All glyphs are our own drawings.

## Clicker

- **Clicker:** one tick per list step (not per raw wheel click) and on button presses. On this device the piezo ticks are **haptic**: the haptic mod is driven from the piezo clicker line (issue #23). **Values (owner, 2026-10-20): Off or On.** On is both the click mixed into the audio (heard in headphones: a soft 4 ms damped 1.8 kHz tick, `apps/ipodui/clicktone.h`, following the volume at half a dB per dB; it replaced upstream's 4 kHz square keyclick after owner device feedback 2026-10-20; tune by ear there) and the piezo/haptic tick; never one without the other. Stock offers Speaker / Headphones / Both / Off; we keep only Off and Both (deliberate difference). Rate-limit during fast spins and tune the pulse by feel. Headphone clicks wake the DAC from paused standby through the mixer (`pcm_mixer.c` `dac_wake()`).

## Menu trees — Settings

- **Settings: stock only.** Show exactly the 7G's Settings tree, with its wording, order, value names and sub-screens. No Rockbox settings appear unless developer mode is on. Each stock item maps to the underlying Rockbox setting(s) internally, and anything with no stock equivalent is fixed at the value that reproduces 7G behaviour.

- **Settings tree** (17 rows, order proven by photos `settings-01-about.jpg` … `settings-17-reset-settings.jpg`): About, Shuffle, Repeat, Main Menu (edit items), Music Menu (edit items), Volume Limit, Backlight, Brightness, Audiobooks (speed: Slower / Normal / Faster), EQ, Sound Check, Clicker, Date & Time, Sort By, Language, **Legal**, Reset Settings.
  - **Inline values** (blue) only on Shuffle, Repeat, Sound Check, Clicker, Sort By. Backlight, Brightness, Audiobooks and EQ show their value only as the panel caption.
  - **Shuffle and Repeat tiles (owner, 2026-10-08):**
    - **Shuffle** has three values, **Off / Songs / Albums**, cycled by SELECT. The tile shows **two parallel right arrows when Off**, and **crossed (criss-cross) shuffle arrows when Songs or Albums**.
    - **Repeat**'s loop arrows have a **small gap** where each arrowhead meets the other arrow's tail; they are not a closed loop (photo `settings-03-repeat.jpg`).
    - Match the Shuffle arrows' proportions to `settings-02-shuffle.jpg`.
  - **Date & Time panel (owner, 2026-10-08; polish #52):** stock shows a **large calendar page** (red month band, big day digits, the weekday below) **with no dark tile** behind it. Ours is currently a small page on a tile; a closer approximation comes in polish.
  - **Backlight:** a value list, still in the split screen: **Off, 2 Seconds, 5 Seconds, 10 Seconds, 20 Seconds, 30 Seconds, Always On** (no 15 s). No checkmark; the current value is the highlighted row on entry. Default 10 Seconds.
  - **Brightness:** dark full-screen screen, header "Brightness" centred. A dark rounded slider box (≈x 23–297, y 114–155) with a crescent moon at the left, a thin capsule track and a sun at the right; the percentage ("0%") centred below at ≈y 208 *(≈, photo `brightness.jpg`)*. The wheel adjusts; MENU returns.
  - **Audiobooks:** a light full-screen prompt, not a list: bold heading "How fast should audiobooks play?", regular body "Playback speed will not affect the pitch of the voice in audiobook playback.", and near the bottom a light-gray rounded shelf with a dark segmented control **Slower | Normal | Faster** (selected segment blue). Settings panel caption "Normal Playback" (photo `audiobooks-speed.jpg`).
  - **About:** dark full-screen, **3 pages**, header = device name centred. Each page: "X GB Used" left, logo centre (ours), "Y GB Free" right, page dots at the bottom. P1: capacity capsule bar + legend Audio / Video / Photos / Other. P2: Songs, Videos, Podcasts, iTunes U | Photos, Games, Contacts with counts. P3: Serial Number, Model, Version (our own version string, "ProkBox5g <ver>"). **NEXT / PREV page between them; the wheel does nothing; MENU returns to Settings (owner, 2026-10-06).** Photos `about-1.jpg` … `about-3.jpg`.
    - **P4 (ours, not on the 7G; release-plan §6.2):** a fourth page and dot. One row in P3's style, "Save Diagnostics" (value at the right: "Saving...", "Saved" or "Not Saved"), and under it a centred wrapped line in the 12 px title font, at most 3 lines, the last baseline on P3's fourth row (y 212). SELECT on P4 only (on the release) writes the report; the line then reads "Saved as ProkBox Report.txt. Connect to a computer and attach that file to your problem report." No device name on screen (`docs/legal.md`). Dots: centred for any page count, 12 px apart.
    - **FAQ: what is in "ProkBox Report.txt"?** One small text file (a few KB) at the top of the iPod's disk, replaced by each new save and never written unless you ask. It holds: the version and build, the hardware variant (storage, RAM, battery, connector, haptics), the hardware revision and RAM, the drive's kind, size and free space, the battery's level, voltage and calibration, the uptime, how many songs, podcasts and audiobooks there are (counts only), the video chip's image state, the last crash record if there is one, and the last lines of the firmware's log from this start and the one before. Song, album, artist and book names and file and folder names are replaced by `<title>` and `<path>` (the scrubber: `apps/ipodui/diag_core.c`, proven by `tests/diag`); no serial number and no device name are included. Built by `apps/ipodui/diag.c`; checked by `tests/ui/ipodui-diag-report.script`.
  - **Legal:** sits between Language and Reset Settings; shows our licence text: the notice heading "This is not Apple software" (Bold 15) and its first paragraph, then the GPL credit, the type licences and the long disclaimer (`docs/legal.md` section 2; the first-boot notice is the same screen in its notice mode, index.md "Attribution on the device").
  - Hidden: a developer combo opens the stock Rockbox menus.

## Panel tiles: owner direction (2026-10-08, for the polish pass #52)
- **A reflective surface:** every tile stands on a glossy surface and shows a mirror reflection below it, like the Now Playing art's reflection but **head-on** (no slant).
- **Smoother gradation:** the tile's gradient is smoother and reads as a **metallic button standing on the surface**: a rounded, 3D, glossy tile.
- **Metallic glyphs:** the white glyph on each tile is metallic. Its white bits are shaded and **fade off toward a corner**, giving a 3D-object look, not flat white.
- **Caption placement:** ours is centred at the bottom of the panel. The originals sit **higher**, with less dead space above and below, so the title, tile and caption feel more evenly spread down the panel.
- **Date & Time:** a nicer calendar page, **not inside a tile box** (see the Date & Time note above).
- **Capacity bar (About and the main-menu Settings panel):** the free-space bar in the original is **3D and metallic**: a glossy, rounded capsule with metallic shading and a fill, not a flat white bar.
- **The same direction applies to the main-menu panels:** the **podcast glyph** (broadcast icon) and every other panel glyph get the metallic, 3D shading, standing on a reflective surface with a head-on reflection.
- **Rendering approach (owner decision, 2026-10-08):**
  - **Plan: option 2.** Store each glyph as a 4-bit metallic shading mask. Apply the tile colour and gloss from precomputed per-row colour tables at draw time, and generate the head-on reflection on the fly. Expected panel art is ~40–60 KB (today 21 KB), and the draw cost is ~2–3 ms per panel arrival, with no storage reads.
  - **Fallbacks, if option 2 can't reach the look:**
    1. fully pre-rendered RGB565 tiles in the firmware (~15 KB each, ~300 KB total; too large to keep resident, so only for a subset);
    2. full-colour tiles in a file in `.rockbox`, loaded on demand into the panel scratch and cached. This needs storage-awake gating during playback, per the device-reality checklist, items 3 and 4.
