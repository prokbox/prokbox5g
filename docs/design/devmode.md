> **Scope decision (owner, 2026-10-06): MINIMAL.** Developer mode exposes only Rockbox's debug screens (hardware info, RAM and buffers, battery, database stats, logs) plus a USB and recovery path. The stock root menu, menus, file browser, settings UI, themes and plugins are **not** kept. Wherever this doc says otherwise, this note wins.

# Hidden developer mode

**Purpose:** reach the stock Rockbox screens from the `HAVE_IPOD_UI` build without ever showing them to a normal user. The stock screens are the root menu, Settings, the debug menu, the file browser, the plugin browser and System Info. CLAUDE.md rule 1 says Rockbox menus are reachable *only* through this combo. Every number in `docs/hardware.md` §"How to measure" (buffer size, boost ratio, thread stacks) lives in the debug menu, so we need this from Phase 2 on.

## 1. What the hardware can detect

`firmware/target/arm/ipod/button-clickwheel.c`:

- **Buttons are a true bitmask.**
  - `ipod_4g_button_read()` decodes the clickwheel packet (`status & 0x800000ff == 0x8000001a`) into independent bits: SELECT `0x100`, RIGHT `0x200`, LEFT `0x400`, PLAY `0x800`, MENU `0x1000`.
  - It ORs them into one value (`btn |= …`), so any combination of the five buttons can be detected.
  - The generic button driver then posts combined codes with `BUTTON_REPEAT` while they are held. The stock keymap already relies on this: `keymap-ipod.c` maps `BUTTON_SELECT|BUTTON_LEFT|BUTTON_REPEAT` and `BUTTON_SELECT|BUTTON_MENU|BUTTON_REPEAT` in its USB-HID contexts.
- **A chord always arrives in steps.** Two fingers never land in the same 10 ms tick, so the first event is the single button (for example `BUTTON_SELECT`), followed by the combined code. A screen that hosts a chord must therefore act on the single button's **release**, never on its press.
- **The wheel posts separately.** Wheel movement (`BUTTON_SCROLL_FWD`/`BACK`, with velocity in `button_get_data()`) is posted only when the queue is empty. Touching the wheel during a chord does not break the chord bits.
- **Hold disables input.** When the Hold switch is on, `button_read_device()` turns the wheel sensor off (`DEV_EN &= ~DEV_OPTO`), so no buttons are read at all. The Hold *transition* is visible (`button_hold()`), but Hold cannot be part of a button chord.

**Combos to avoid:**

| Combo | Why not |
|---|---|
| MENU + SELECT held for about 6–8 s | This is the iPod's well-known hard-reset gesture. Users will do it, and a long hold reboots the device. |
| SELECT + PLAY or SELECT + PREV **at reset** | Apple's ROM reads these as Disk Mode and Diagnostics before our bootloader runs. |
| MENU, PLAY or the Hold switch **at boot** | Our bootloader (`bootloader/ipod.c`) uses them: MENU or Hold boots Apple's firmware (the recovery path), and PLAY loads `linux.bin`. |
| Any chord on a screen where SELECT acts on press | It would trigger the screen's own action first. |

## 2. The combo

**Primary: on Settings → About, hold SELECT + PREV (LEFT) together for 3 seconds.**

- **It is plausible on an iPod.** It mirrors Apple's own hidden chord (SELECT+PREV is the 5G diagnostics chord, but we use it at runtime, not at reset). It sits on the screen where the 7G hides device details.
- **It is hard to trigger by accident.** It only works on one screen, needs two buttons at once, and takes 3 s.
- **It doesn't conflict.** On About, SELECT pages through the About screens **on release** (7G behavior), and PREV does nothing. The chord cancels the pending release action.
- **There is subtle feedback** that matches the shell's style:
  - after 1 s the About title gains a small ellipsis
  - at 3 s there is one click tick and a short brightness flash of the header
  - no text says "developer"

**Boot-time escape (recovery): hold NEXT (RIGHT) from power-on until the main menu would appear.**

- `main()` checks `button_read_device() & BUTTON_RIGHT` just before `ipodui_main()`.
- If NEXT is held, it enters the stock root menu instead of the shell.
- This is the way out when a broken shell build crashes or hangs before About can be reached.
- NEXT is unused by Apple's ROM and by our bootloader at boot.
- It is guarded by the same compile-time switch as everything else here.

**Rejected alternative:** a sequence on About such as PREV, NEXT, PREV, NEXT, SELECT within 2 s. It is fine for the simulator, but a user flicking around the About screen can hit it. We keep the chord only. For simulator tests, `sim_script.c` gets a `chord` command (see §6).

## 3. What devmode opens

The combo opens a plain stock `do_menu()` list, the **Developer** menu. It uses the stock list widget on purpose, so it is obviously not part of the product UI.

**Layout (owner, 2026-10-26):** what is on is one screen; what the owner presses is up front; the technical rows are a couple of levels down. A release build (`--ipodui-release`) has no Developer menu at all.

```
Developer
  Status                     what is on and off, in plain words
  Bench session              guided; sets and restores its own switches (target, --bcmfw-devtest --bcmfw-video)
  Power log: off             on / off (kept across restarts)
  Power log every: 30 s      5 / 30 / 60 s
  <battery table>            the calibration table in use (HAVE_BATTERY_LEARN)
  Reset battery calibration  second SELECT within 5 s
  Advanced ›
    Experiments ›            every switch, with its state and its bench step's verdict
      Speed governor: off / stage 1 / log only / apply   (HAVE_PP_GOV; apply needs a second SELECT)
      UI thread: inline / cpu / cop                       (HAVE_UI_THREAD_SPLIT; next start)
      Soft sleep: Off / On
      Drive power: off when idle / standby only           (HAVE_STORAGE_POWEROFF)
      DAC standby: on / off                               (not saved)
      24 MHz idle, Lit screen: 30 / 24 MHz                (HAVE_PLL_IDLE; not saved)
      Resident video image, Video chip idle sleep,
        Video chip core level 5                           (HAVE_BCMFW_RESIDENT; next start)
      UDMA4 experiment, UDMA4 test                        (HAVE_ATA_UDMA_FORCE)
      USB prefetch next, USB ARC attach init              (target)
    Diagnostics ›            Audio path test, Crash test: freeze / restart, the data-rate rows
                             (HAVE_DEVSTATS), Loader & storage (modules), the battery run,
                             FSInfo (free-space recount, target), USB INQUIRY buffer (target)
    Labs ›                   Sleep lab ›, Alarm lab ›, Drive power A/B (length, switch, start)
    Video chip ›             (--bcmfw-devtest) Boot path ›, Fault injection › (--bcmfw-l1),
                             Probes › (the optional stages and labs, PB/PW probes, C1-C3),
                             BCM jobs: driver self-test, BCM jobs: drift on video chip,
                             Video: full log on every play
    System debug ›          debug_menu(), the stock debug screens
  Back                       returns to the shell's About screen
```

- **Status** (`apps/ipodui/devstatus.c`) is one read-only stock list. Each row is made when the list draws it: the build variant; the video image (resident and running, verified, 150 MHz token), the video chip's idle sleep and level 5, the job switch; the speed governor (with "refused: UI on 2nd core" when apply would be refused); the UI thread (now and next start); soft sleep; drive power; DAC standby; the power log; the lit clock and 24 MHz idle; UDMA4; the USB switches; and this build's last bench session (date, ended or open, pass/fail/skip counts). A row away from its default ends in "(changed)", and the first row counts them ("Not at default: N"). The results file is read once on entry, never at boot or while browsing.
- **Experiments' verdicts:** the governor (`vgov`), UI thread (`ucop2`), idle sleep (`slp`), level 5 (`lvl5`) and lit clock (`lit24ui`) rows end in `[bench: pass|fail|skipped]` from this build's newest results file, read when Experiments opens.
- **Speed governor** writes the existing markers `pgov_on`, `pgov2_on`, `pgov2_apply` (`clockpol.h`), which `clockpol_init()` reads at the next start, unchanged. Developer builds only: a release never reads them.
- **The Bench session's switches** (`devswitch_*`, `devstatus.h`): a new session creates `bcmjobs.on` if it is missing and lists it in `bench/switches.txt`; the list survives restarts and resumes (never re-recorded, never re-prompted); the session's end removes what the list names; at boot, a list left by a session that is no longer open (ended, or another build's) is undone, so the switch never leaks into normal use. A marker the owner placed is never listed and never removed. When the re-verify run starts with the stage flags present and the resident image up but unverified, one screen asks for a restart first (the job gate is read at boot), then Resume.
- **Strings:** the new rows and Status go through `LANG_IPODUI_DEV_*` (feature `ipodui_devmode`); the older switch rows keep their developer literals. Nothing names the hidden folder; "System debug" is the one row that says Rockbox (devmode only).
- **Removed (2026-10-26):** EQ test; Codec core and Codec full wait with their markers (`codec_place_cop`, `codec_place_screen`, `codec_fullpoll`; decode is pinned to the main core, owner 2026-10-24); the `lcd_e2off` reader; the variant row (now in Status); the Debug menu's BCM rows and its stage rows that the Bench session runs (0, 0-1 … 0-5, 0/1/4, 4b, 4c, 6, 7, B5, B13, CR1; their functions stay where the Bench session calls them).
- **Release leaks closed:** `soft_sleep` and the governor markers are read only in developer builds.

## 4. Getting into `root_menu`, and back out

**The problem:** `root_menu()` is `NORETURN` (`apps/root_menu.h:27`). Its `while (true)` loop never exits.
- MENU at the top-level `do_menu()` returns `GO_TO_PREVIOUS` (`apps/menu.c`, about line 642).
- `root_menu` then jumps to `last_screen` (often the WPS) instead of returning.

**The change:** one `#ifdef HAVE_IPOD_UI_DEVMODE` block in `apps/root_menu.c`, about 20 lines.

```c
#ifdef HAVE_IPOD_UI_DEVMODE
/* Run the stock root menu loop starting at 'start' until the user presses
 * MENU at the top level, then return to the caller (the iPod UI shell). */
void root_menu_dev(int start);
#endif
```

- **Implementation:**
  - Split the body of `root_menu()` into `static void root_menu_loop(int start, bool returnable)`.
  - `root_menu()` calls `root_menu_loop(root_menu_setup_screens(), false)`.
  - `root_menu_dev()` calls `root_menu_loop(start, true)`. It skips `root_menu_setup_screens()`, so it does not register the playback events or check RTC alarms twice, and it doesn't resume `global_status.last_screen`.
  - In the `GO_TO_ROOT` case: `if (returnable && next_screen == GO_TO_PREVIOUS) break-out`.
  - Return also on `MENU_ATTACHED_USB`, so the shell's own USB screen handles USB.
- **Undo the stock side effects before returning:**
  - Restore `global_status.last_screen`, which `load_screen()` writes and `status_save()`s. If we don't, the next stock-path boot would resume into a stock screen. The shell keeps its own resume state anyway.
  - Pop to the activity depth the shell had (`push_current_activity` / `pop_current_activity`).
- **On return the shell:**
  1. re-applies its theme state: `viewportmanager_theme_enable(SCREEN_MAIN, false, NULL)` and its own fonts and viewport
  2. invalidates its header cache
  3. does **one** full `lcd_update()`, then goes back to partial updates
- **Stock settings changed in devmode still apply:** EQ, crossfeed, buffer settings, dircache and so on. That is intended; devmode is where those knobs live.
- **Plugins:** a plugin may take the audio buffer (`plugin_get_audio_buffer`), which stops playback. That is acceptable in devmode. On return the shell's Now Playing finds nothing playing and handles it as usual.
- **Fallback:** if a stock screen ever leaves things in a state the shell can't recover from, the Developer menu has no "reboot" item; holding MENU+SELECT (hardware reset) is always available.

## 5. Compile-time removal

- `HAVE_IPOD_UI_DEVMODE` is defined by `tools/configure --ipodui`. It is on by default during development.
- `--ipodui --ipodui-release` (a new flag) leaves it undefined.
- When it is undefined:
  - the About chord handler, the boot-time NEXT check, the Developer menu and `root_menu_dev()` are not compiled in
  - the strings for the Developer menu are absent, so a binary scan for "Developer", "Debug" or "Rockbox main menu" finds nothing
  - `root_menu()` itself remains (the stock build needs it), but the shell never calls it
- **Add a check to the budget-reviewer:**
  - `strings rockbox.ipod | grep -i rockbox` on a release build must contain only file paths, such as `/.rockbox/`.
  - The `--ipodui-release` build must compile with `HAVE_IPOD_UI_DEVMODE` undefined.
- **Decision for the user:** should the release build that goes on the device keep devmode? The recommendation is **yes** for this personal device (the debug menu is how we measure battery and RAM), and `--ipodui-release` exists for a fully sealed build.

## 6. Files and tests (for the implementer, Phase 2 or 8)

| File | Change |
|---|---|
| `apps/ipodui/devmode.{c,h}` | The chord detector (a small state machine fed by the About screen's input handler), the Developer menu, and the theme save and restore around `root_menu_dev()`. |
| `apps/ipodui/devstatus.{c,h}` | Status, the Experiments verdicts, the speed governor switch, the Bench session's own switches (§3). |
| `apps/ipodui/screens/about.c` | Calls `devmode_feed(button)`. SELECT pages on release. |
| `apps/root_menu.{c,h}` | Adds `root_menu_dev()` under `#ifdef HAVE_IPOD_UI_DEVMODE` (the loop refactor above). |
| `apps/main.c` | Adds the boot-time NEXT check inside the existing `#ifdef HAVE_IPOD_UI` block. |
| `tools/configure` | Defines `HAVE_IPOD_UI_DEVMODE` with `--ipodui`, and adds `--ipodui-release`. |
| `uisimulator/common/sim_script.c` | Adds `chord <A>+<B> <ms>`, which presses both keys (A first, B 50 ms later), holds them, then releases. |
| `tests/ui/devmode.script` | Settings → About → `chord SELECT+LEFT 3200` → shot → Debug → shot → MENU, MENU → shot. The last shot must show the shell's About screen. |
| `tests/ui/ipodui-devmenu-layout.script` | Each level of the 2026-10-26 layout and the Status screen, at defaults and with one switch changed. |
| `tests/ui/devmode_boot.script` | Boots with NEXT held (`hold RIGHT 5000` as the first line). Expect the stock root menu. |
