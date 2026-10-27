# Brief: Phase 8a, native system screens before device testing (#7, #16, #51, #54, #56)

*Orchestrator brief, 2026-10-09, branch `p8a` (from `main`). **Revision 1 (brief review, 2026-10-09) is applied inline to §3–§6 and listed at the end; where an older sentence disagrees, Revision 1 wins.***

* Written while the owner was asleep: open points are frozen as ⚠️ with a default (§10) and go to the owner round. They are not revised mid-phase. Process: `docs/process.md` (brief template, device-reality checklist 1–13).*

**Goal.** Before the device sees this firmware (P10), nothing Rockbox-looking can show up on any path a user can reach. That covers the boot (bootloader, then firmware), USB, splashes, and the dark screen after the backlight times out. The System debug screens stay reachable only through a hidden combo.

## 1. Owner pre-flight

**Decided (recorded in the design docs, not reopened here):**

| Item | Decision | Source |
|---|---|---|
| Boot glyph | "wheel" in 3-D spun metal, **MEDIUM**, on the blue-grey 5-stop gradient, 68 px ring centred at (160, 117). Pre-composited RGB565 block plus a 240 × 2 dithered row table | owner 2026-10-07; `docs/design/boot-logo.md` §wheel3d |
| Unlit face | the **"stock"** variant: 30 × 68 cells, stroke 9, r 3, ink 56 with a 150 rim; play/pause triangle; battery 54 × 32 + nub; status row at y 179 | owner 2026-10-08; `docs/design/unlit.md` |
| LCD sleep | about 90 s after the backlight goes off | owner 2026-10-07 ("a minute or two") |
| Devmode scope | **MINIMAL**: the debug screens plus a USB and recovery path. No stock root menu, file browser, settings UI, themes or plugins | owner 2026-10-06; `docs/design/devmode.md` (top note) |
| USB screen | dark header, slate-to-navy gradient, glossy orange disc, a connector glyph when idle, sync arrows spinning during I/O, "Connected" / "Eject before disconnecting." | owner 2026-10-06/08; `docs/design/spec/system-screens.md` |
| Connector glyph | `HAVE_IPOD_DOCK30` set: the 30-pin pictogram. Unset (this unit): a USB-C plug pictogram | owner 2026-10-08; #54 |

**Captures used (local only, never committed):** `XCShMisR6KMNUYmB.webp` (7G boot), `unlit-clock-2.webp` (7G unlit), `usb-connected.jpg` / `usb-connected-b.jpg` (photo 1165 / photo 1226). **Wanted, not blocking:** a video of a 7G going from backlight timeout to the unlit face to a button wake (it would settle ⚠️ U3 and U4).

**Still open (frozen in §10):** POL-4, whether the shipped build keeps devmode (default: yes). Plus the per-item ⚠️ below.

## 2. Scope

**In:**
1. **Boot screen.** One `bootscreen_draw()` plus generated data, shared by `bootloader/ipod.c` and `apps/main.c` `show_logo_boot()`, so the hand-off is pixel-identical. A new `HAVE_IPOD_BOOTSCREEN` flag. Bootloader text only with the verbose key (RIGHT) held, except errors. The Rockbox logo, the version string and the USB logo drop out of the shell build. The black flash between the logo and the shell goes away. `art/boot/bootlogo.py` gains `--c`.
2. **Native USB screen** in place of the stock one. `HAVE_IPOD_DOCK30` picks the idle glyph. A new generator, `art/usb/usbscreen.py`.
3. **No stock splashes or prompts** on any shell path. One funnel at `splashf()` / `splash_progress()` / `gui_syncyesno_run_w_tmo()` routes them to a log file (`/.rockbox/ipodui.log`). Devmode lets them through.
4. **Developer mode (MINIMAL).** On Settings → About, hold SELECT + PREV for 3 s, which opens a stock `do_menu()` "Developer" list (Debug, Back). Holding RIGHT at boot opens the same list before the shell starts (the recovery path). `--ipodui-release` compiles it all out.
5. **Unlit clock face, plus LCD sleep at 90 s** (#51). This needs a backlight-off event, a Hold-on lights-the-backlight hook, wake-press swallowing in `input.c`, and procedural drawing.
6. **#56.** Every hard-coded user-visible shell string moves to `english.lang` (`ipodui` feature, appended).

**Out of scope (named so nobody builds it):**
- the Ejecting / OK to Disconnect, Charging and Very-low-battery native screens (8b; in 8a their splashes are logged);
- the panic screen (`panicf`): a crash is the only diagnostic left on a unit with no serial port, so it stays ⚠️ D5;
- a real About screen (8b);
- hidden FAT attributes on `.rockbox` (separate item);
- compiling iAP and dock code out under `!HAVE_IPOD_DOCK30` (#54 part 2, ⚠️ D4);
- the 8b material kit;
- the Apple ROM logo that may show before our bootloader (we can't change it; it's a P10 observation);
- stock `root_menu()` changes: devmode.md §4's `root_menu_dev()` is **superseded** by MINIMAL and not built.

## 3. Design

### 3.1 Boot screen (`firmware-platform`, P)

- **Data.** `art/boot/bootlogo.py --3d --c OUT` writes `bootscreen_data.c/.h` for the medium strength (§4.1):
  - `bootscreen_bg[240][2]`: even-x and odd-x colours per row, 960 B;
  - `bootscreen_block[71 × 72]`: 10,224 B;
  - `BOOTSCREEN_X/Y/W/H`;
  - `BOOTSCREEN_GEN`, an FNV-1a hash over both arrays.

  The generated files are **committed**, so the build never needs Python.
- **Draw.** For each row, 160 32-bit stores of `bg[y][0] | bg[y][1] << 16` into `FBADDR(0, y)`. Then one `memcpy` per block row. No `lcd_*` per-pixel calls, no fonts, no allocation, no `lcd_update()`. Lean rules 1, 2 and 7.
- **Bootloader** (`bootloader/ipod.c`, under `HAVE_IPOD_BOOTSCREEN`):
  - Order: `lcd_init()` → `bootscreen_draw(); lcd_update();` → `backlight_init()`, so the backlight comes on over the image. The `lcd_clear_display()` goes.
  - The banner `printf`s stay. `bootloader/common.c` `printf()` already pushes only when `verbose`; in silent mode the lines land in the framebuffer unpushed.
  - Errors (`fatal_error()`, load failures) call `lcd_update()` and so show their text over the gradient, which is the recovery information. That's ⚠️ D6.
  - Verbose mode also prints `bootscreen gen <hex>`.
- **Firmware** (`apps/main.c`):
  - Under `HAVE_IPOD_BOOTSCREEN`, `show_logo_boot()` becomes `bootscreen_draw(); lcd_update();` plus one `DEBUGF("ipodui: boot screen gen=%08lx\n", ...)`. No `lcd_clear_display()` and no version text. `sleep(HZ)` stays in the sim.
  - `lcd_init()` only clears the framebuffer, and `lcd_init_device()` finds the BCM powered and doesn't re-init it (`lcd-video.c`). The panel therefore holds the bootloader's push until this identical push.
  - In `main()`, the `screens[i].clear_display(); update();` after `init()` gets `#ifndef HAVE_IPOD_UI`, because the shell's first frame is a full draw. This removes a black flash.
- **Drops.**
  - `bm_rockboxlogo` loses its last shell reference. `libbitmaps.a` members are pulled in only when referenced, so its 62,720 B leave `.initdata`.
  - `bm_usblogo` goes too (§3.2), which is **16,896 B of resident rodata**.
  - P confirms both with `arm-none-eabi-nm build/fw-ui/rockbox.elf | grep -c 'rockboxlogo\|usblogo'` = 0. If that fails, P adds an `apps/bitmaps/native/SOURCES` guard.
- **Config.** `tools/configure` (§4.6):
  - `--ipodui` with `--type=b` defines **only** `HAVE_IPOD_BOOTSCREEN`. The bootloader never sees `HAVE_IPOD_UI`, so `firmware/font.c` and `core_alloc.c`'s existing guards stay out of it.
  - Every other type gets `HAVE_IPOD_UI HAVE_IPOD_BOOTSCREEN HAVE_IPOD_UI_DEVMODE`.
  - `--ipodui-release` drops `HAVE_IPOD_UI_DEVMODE`. `--dock30` adds `HAVE_IPOD_DOCK30`.
- **Identity vs validity.** The *identity* of the art is `BOOTSCREEN_GEN`. The bootloader on the device and the firmware are built and installed separately, so the hand-off is identical only when their GENs match. Both stages print or log it, and P10 compares them. *Validity* (whether the bootloader actually drew it) can't be known by the firmware, and nothing depends on it.

### 3.2 Native USB screen (`ui-fidelity`, U1)

- **Hook.**
  - `apps/gui/usb_screen.c` `gui_usb_screen_run()`, under `HAVE_IPOD_UI`: after `usb_acknowledge()`, call `ui_usb_screen_loop(early_usb)` in place of `usb_screens_draw()` + `handle_usb_events()`.
  - Activity push and pop, `font_disable_all()` / `font_enable_all()`, `settings_apply(true)`, `playlist_resume()`, `backlight_on()` and the theme undo stay stock.
  - **Viewport stack** (Rev 1 H1): the stock `usb_screen_fix_viewports()` call (`usb_screen.c:264`) pushes the viewportmanager stack (`theme_enable(true)`, `:143`), and the undo at `:308` pops it.
    - Under `HAVE_IPOD_UI` that call becomes `viewportmanager_theme_enable(i, false, NULL)`, so the stack stays balanced (§4.2).
    - Guarded `#ifndef HAVE_IPOD_UI`: `usb_screens_draw()`, `handle_usb_events()`, `draw_usb_keypad_mode()`, plus `usb_screen_fix_viewports()` once it is unreferenced. That means no unused-function warnings.
  - `ui_usb_screen_loop()` calls `backlight_on()` at entry.
  - P makes this edit; U1 owns the callee.
- **Fonts.** The shell's 4 fonts are loaded whole (`font_load_ex(path, 0, 0)` leaves `fd < 0`), so `font_disable_all()` doesn't touch them. U1 asserts `fd < 0` at entry and logs it otherwise.
- **Layout** (⚠️ U1: the on-device owner photo wins over Apple's `usb.png` asset for geometry; colours come from `MEASUREMENTS.md`, high confidence):
  - **Header:** dark, 22 px (y 0–21), #333739 → #191D1F. The title is centred white: the clock when `ui_header_time_in_title()` is on, else `LANG_IPODUI_DEVICE_NAME`. The battery has the charging glyph.
  - **Body:** gradient #889AA5 (y 24) → #464F56 (y 239), 2 × 2 dithered like the boot gradient.
  - **Disc:** 80 px diameter, centred at (160, 108), orange, lighter at the top, with a gloss cap.
  - **Glyph:** black, centred on the disc.
  - **Text:** "Connected" in Bold 15 at a baseline of y ≈ 190, "Eject before disconnecting." in Regular 12 at y ≈ 205. White, centred, `DRMODE_FG` over the gradient.
  - All of these are generator constants, cheap to retune at the owner round.
- **Art** (`art/usb/usbscreen.py`, stdlib only, the visual source of truth; it writes reference PNGs to `docs/design/usb/` and `apps/ipodui/usbscreen_data.h`):
  - per-row colour stops for the header and body;
  - the disc's per-row base and gloss tables plus its radius and centre (the disc is drawn **procedurally** in C with integer edge coverage, not stored);
  - the two connector glyphs as 4-bit alpha, ≤ 48 × 28 each. Only one is compiled into the target (`UI_USB_IDLE_GLYPH`); the sim compiles both, and `usb_glyph` pins one;
  - the arrow geometry (two arcs plus two heads, as parameters).

  Total rodata ≤ 1.8 KB.
- **Compose once, then blit** (lean rules 1 and 3):
  - At entry, U1 claims the strip (`UI_STRIP_OWNER_USB`) and renders into it:
    - the clean disc (80 × 80 × 2 = 12,800 B);
    - the idle frame;
    - **12 arrow frames** at 30° steps (12 × 80 × 80 × 2 = 153,600 B; with the disc and the idle frame that's 179,200 B of the 180,000 B strip, a tight fit). ⚠️ U2: if the strip is short, use 6 frames over 180°, since the arrows have two-fold symmetry.
  - It draws the static layer (header, body, text) straight into the framebuffer, then makes one full push.
  - That's one boosted compose of about 1.9 M instructions (≈ 24 ms at 80 MHz). `cpu_boost(false)` comes before the first `button_get_w_tmo()`, so it's balanced.
  - **Deliberate deviation** (Rev 1 L3): `usb_storage` already boosts the CPU during transfers (`needs_cpu_boost`), so the compose boost overlaps the stack's own. It's kept so the first frame is ready quickly even when no transfer is running.
- **Loop:**
  - **I/O active and lit:** 10 fps. Each frame `memcpy`s the next frame's 80 rows × 160 B (the disc's width) into the framebuffer and pushes **one full-width band over the disc rows** (y 68–147, 80 rows).
  - **Idle and lit:** the idle glyph shows. The loop blocks with a `HZ/2` timeout to notice I/O.
  - **Dark** (`!is_backlight_on(true)`): no drawing, `TIMEOUT_BLOCK`. A button or event wakes it, and it re-evaluates on wake.
  - **Header clock** (only when Time in Title is on): one 22-row band push at the minute.
  - **Exit** on `SYS_USB_DISCONNECTED`, plus `SYS_CHARGER_DISCONNECTED` → `reset_runtime()` as stock.
  - **HID** (Rev 1 L4): the shell's loop **ignores HID**. It never calls `get_hid_usb_action()`. `usb_hid` defaults to off under `HAVE_IPOD_UI` (orchestrator hook H1b). A developer who turns it on gets an enumerated HID device whose keys do nothing.
- **I/O signal.**
  - On the device, `ui_usb_io_active()` is `TIME_BEFORE(current_tick, storage_last_disk_activity() + HZ/2)`. `ata.c` stamps `last_disk_activity` on every sector read and write, including the USB storage driver's.
  - In the sim it reads the existing `sim_disk_active_until` (set by the `disk_active <ms>` script command, `sim_cmd_art.c`). U1 declares the `extern` itself; `artcache.c` is a P6 file and isn't touched.
  - The timing matches the device: a transfer burst keeps it active for 0.5 s after the last sector.
- **Shell not started** (USB during the boot-time devmode, or the "No partition found" path): `ui_shell_started()` is false. The loop then draws the gradient, disc and idle glyph straight into the framebuffer: no strip, no text, no animation.
- **Identity vs validity:** see `struct ui_usb_state` (§4.3).

### 3.3 No stock splashes (P for the funnel, U2 for the log)

- **Funnel** (P, `apps/gui/splash.c`, `apps/gui/yesno.c`, guarded by `HAVE_IPOD_UI`):
  - `splashf()`: if `!ipodui_stock_ui_allowed()`, format into the log (`ui_log_va("splash-suppressed", ...)`) and return. No draw and no `sleep(ticks)` ⚠️ D1.
  - `splash_progress()`: log only when `current == 0` or `current >= total`.
  - `gui_syncyesno_run_w_tmo()`: log the first message line as `yesno-suppressed` and return **`YESNO_NO`**, the non-destructive answer ⚠️ D2.
  - Inside devmode (or before the shell, on the boot escape), they draw as stock and log `splash-pass`.
- **Inventory** (P's report, with method):
  - P lists every call site of `splashf`, `splash_progress` and `gui_syncyesno_run*` linked into `build/fw-ui/rockbox.elf` (`arm-none-eabi-objdump -d` → `bl` targets → containing function), and classifies each as reachable from a shell path or not.
  - The known reachable ones:
    - `misc.c`: shutdown, low and empty battery (already DEBUGF-guarded, kept as is), `charging_splash()`, playlist search;
    - `playlist.c` error splashes (`notify_access_error` and others);
    - `main.c` "key remap failed" and `LANG_RESET_DONE_CLEAR`;
    - `bookmark.c` and `tree.c` if any shell path reaches them;
    - `tagcache` progress (already suppressed in `main.c`).
  - The funnel covers all of them, including future ones. The inventory documents which ones deserve a native screen in 8b.
  - Non-splash stock UI kept on purpose: the "No partition found" text screen in `main.c` `init()`, and the bootloader error text. Both are recovery information ⚠️ D6.
- **Log** (U2, `apps/ipodui/uilog.c`):
  - a 768 B static ring (12 × 64 B lines, under the 1 KB rule);
  - each line is also emitted as `DEBUGF("ipodui: <tag> <text>\n")`, so the gate sees it in `sim.log`. On the device DEBUGF is a no-op;
  - **flush** from a persistent `add_event_ex(DISK_EVENT_SPINUP, false, ...)` handler, registered once (Rev 1 H3; `register_storage_idle_func()` is a oneshot). The disk is already awake, so this costs **0 extra wakes**. Also on `SYS_POWEROFF` in the shell's event path. There's no `usb_inserted()` test: `storage.c` skips the idle notifies in USB mode by itself. Writes go line by line through a 64 B local copy with sequence numbers, under 128 B of the storage thread's 2 KB stack;
  - append-only to `/.rockbox/ipodui.log`, rotated once to `ipodui.log.1` above 16 KB;
  - snapshot the ring, then write; no lock is held across I/O. A ring overflow drops the oldest line and counts it (`dropped=<n>` on the next flushed line). A call from the COP is dropped and counted (`CURRENT_CORE != CPU`).

### 3.4 Developer mode (U2; P for `main.c` and `configure`)

- **Chord** (`apps/ipodui/devmode.c`):
  - **Armed** only when `top->ops == &ui_about_screen` (Rev 1 M6), with no title `strcmp`.
    - `ui_about_screen` is an exported stub (`.name "about"`) in `screens/stub.c` (U2), declared in `screens/screens.h` by the contract commit.
    - Orchestrator hook **H3** (after P6 merges) changes the About row's target in `screens/settings.c:471–472` from `&ui_stub_screen` to `&ui_about_screen`.
    - The real About screen (8b) replaces the stub under the same symbol.
  - **Fed** from `ui_main.c`'s global input layer with `in->raw`, before the top screen. Chords map to `UI_KEY_NONE` in `input.c`, so the screen never acts on them.
  - **Trigger:** both bits (`BUTTON_SELECT | BUTTON_LEFT`) held for `DEVMODE_HOLD_TICKS` (3 s) gives one clicker tick, then `ipodui_devmode_run()`. Everything up to the release is swallowed.
  - There is no hint at 1 s ⚠️ D3, and no visible text.
- **Menu:** a stock `do_menu()` list titled `LANG_IPODUI_DEVELOPER`, with the items `LANG_DEBUG` (`debug_menu()`) and `LANG_BACK` (Rev 1 L1).
  - The new phrase sits under the `ipodui_devmode` feature, so it is absent from the release build.
  - `ipodui_stock_ui_allowed()` is true only while `ipodui_devmode_run()` is on the stack (M3).
  - **Viewport stack** (Rev 1 H1): `do_menu()` is wrapped in exactly one `viewportmanager_theme_enable(i, true, NULL)` / `viewportmanager_theme_undo(i, false)` pair, and nothing is pushed on return.
  - **On return:** `scroll_stop()`, then the shell's fonts and viewport, the header cache invalidated, and **one** full redraw.
- **Boot escape** (P, `apps/main.c`, guarded by `HAVE_IPOD_UI_DEVMODE`): just before `ipodui_main()`, `if (ipodui_devmode_boot_requested()) ipodui_devmode_run();`, which checks `button_status() & BUTTON_RIGHT` (Rev 1 L7).
  - It opens the same list before the shell has reserved anything, so a shell that crashes at start can still be inspected.
  - USB still works there: the native screen in its shell-not-started form, §3.2. That's the recovery path for replacing a broken build.
  - RIGHT is also the bootloader's verbose key, so a "developer boot" shows the boot text too.
- **Removal.**
  - Without `HAVE_IPOD_UI_DEVMODE`: no chord handler, no boot check, no menu, no literals, and `ipodui_stock_ui_allowed()` is constant `false`.
  - `strings rockbox.ipod | grep -ci developer` = 0 on a `--ipodui-release` build. The `budget-reviewer` check is in devmode.md §5.

### 3.5 Unlit clock face and LCD sleep (U2; P for the firmware hooks)

- **Firmware hooks** (P, `firmware/backlight.c`, guarded by `#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)`, §4.2):
  1. In `do_backlight_off()`: `button_queue_post(SYS_IPODUI_BACKLIGHT_OFF, 0)`, **only when `backlight_get_current_timeout() > 0`** (Rev 1 M2). It's posted from the backlight thread as the fade starts. `queue_post` is safe across threads.
  2. In `backlight_hold_changed()`:
     - Hold going **on** also posts `BACKLIGHT_ON`, so sliding Hold on over a dark face lights it.
     - Every change also posts `SYS_IPODUI_HOLD_CHANGED` (data 1 or 0, MISC 7, Rev 1 H4). `UE_HOLD_ON`/`UE_HOLD_OFF` come only from that event.
     - The sim's `hold_switch on` also drops keys while Hold is on, as the device does.
- **LCD sleep 90 s:** the `lcd_sleep_after_backlight_off` default goes from 5 to 90 under `HAVE_IPOD_UI` (orchestrator hook H1a; `settings_list.c` is P6's). `timeout_sec_common` already contains 90. The firmware's own countdown (`backlight_lcd_sleep_countdown` → `lcd_sleep()`) does the BCM power-down; the shell does nothing. The setting stays hidden.
- **Shell** (U2: `unlit_core.c` is pure and host-tested; `unlit.c` draws):
  - **States:** NORMAL and FACE. FACE_ASLEEP is the firmware's `!lcd_active()`, not tracked.
  - **Transitions:** unlit.md's table, with these frozen choices:
    - `SYS_IPODUI_BACKLIGHT_OFF` → FACE, **only if `!is_backlight_on(true)`** when it's handled (Rev 1 M1), unless skipped:
      - `unlit skip sleep`: the shell's PLAY-hold sleep path calls `ui_unlit_suppress_next()` just before its `backlight_off()` (⚠️ U5). The flag carries a tick stamp and expires after `UNLIT_SKIP_TICKS` (≈ HZ; `unlit skip-expired`, M2);
      - `unlit skip usb`: only while `ui_usb_screen_active()`, a flag set inside `ui_usb_screen_loop()`, never `usb_inserted()` (M4);
      - `unlit skip devmode`.

      The "Off" setting never posts at all (M2).
    - Any key → NORMAL (⚠️ U3), then `ui_invalidate(UI_DIRTY_ALL)`. One full redraw; the face covered the whole screen, which is the written reason lean rule 4 asks for.
      - A lone release (no press seen while FACE) also dismisses.
      - Swallowing ends on the dismissing key's release, or when `button_status() == 0`, with a 1 s fallback. Wheel events are swallowed for a fixed `HZ/4` after the dismiss (M1).
      - `tests/unlit` has a row for each case.
    - Hold off → NORMAL.
    - USB → NORMAL first, then the USB screen.
    - Backlight back on without a key (Hold on, charger) → stays FACE, now lit.
  - **While FACE, drawing is suspended, never logic** (Rev 1 H2):
    - `ui_main.c` skips the top screen's `tick()`/`draw()`, header polling and panels, and calls `scroll_stop()` on enter (L6).
    - It **still calls `np_pb_idle()`** (the shuffle queue refill and the other playback housekeeping) and `min_wait`s its wake with `ui_unlit_timeout()`. That's the ticks to the next minute boundary from `get_time()`, one timer source (lean rule 5). Lit and dark rates are the same: one wake a minute, plus whatever `np_pb_idle()` asks for.
    - Only Now Playing's `auto_return()` (a screen push) is deferred until dismiss. FACE calls `np_pb_idle()` directly instead of `ui_nowplaying_idle()`, so `nowplaying.c` (P6) isn't edited.
    - **After P6 merges**, U2 re-checks P6's `np_*` idle logic for any further draw-free work that must keep running in FACE.
  - **Drawing** (procedural, no bitmaps, 0 B heap):
    - **First show:** clear to paper (white) and draw about 40 `lcd_fillrect`s (each digit bar in the rim grey 150, then inset by 1 px in ink 56, with rounded corners from a 3-entry span table). The triangle and pause bars use per-row spans; the battery is rects. One full push.
    - **Minute:** redraw the changed cells, then push the band `320 × 68` at y 62. ⚠️ U9: try the narrow `30 × 68` rect on the device; whichever is cheaper on the BCM wins at P10.
    - **Status change** (play state, Hold, charging, battery fill width): push the band `320 × 37` at y 161.
    - **12 h** shows no AM/PM and no leading zero. **24 h** zero-pads.
  - **Asleep:** `lcd_update_rect()` returns at once when the display is off. The minute tick keeps the framebuffer current (≈ 0.2 ms/min), so `lcd_awake()`'s push shows the right time on wake.
- **Identity vs validity:** see `struct ui_unlit_state` (§4.4).
  - *Identity* is the minute and the status fields that are on screen. Redraws are decided by comparing them with now.
  - *Validity* is `fb_valid`. It's cleared by anything else that draws: the USB screen, devmode, a dismiss. A minute tick with `!fb_valid` does a full first-show draw, never a digits-only patch over someone else's pixels.

### 3.6 #56: shell strings to `english.lang` (S; the orchestrator for P6 files)

- **Mechanism:** every `ipodui` phrase is `*: none` / `ipodui: "<text>"`, **appended** after the last phrase (P6's block lands first at integration, then 8a's). Existing IDs reuse identical text (`LANG_SETTINGS`, `LANG_IPODUI_SONGS`, `LANG_IPODUI_ALBUMS`, `LANG_IPODUI_AUDIOBOOKS`, `LANG_IPODUI_DEVICE_NAME`).
  - Menu tables with literal labels switch to the `settings.c` pattern: a `short item_lang[]` array, with `label = str(id)` at init.
- **Part A** (S, now; non-P6 files):
  - `menu.c`: the main and music menu labels, the "iPod" title, "Updating Library...";
  - `screens/playlists.c`: "On-The-Go";
  - `screens/stub.c`, `screens/panels.c`: any literal found.
  - `header.c` AM/PM belongs to U1 (U1 owns `header.c`).
  - Plus an **audit**: a `grep` of `apps/ipodui/**/*.c` string literals reaching `lcd_puts*`, `font_getstringsize`, a menu label or a screen title. Traces, paths and format-only strings ("%d", "...") are excluded. The audit table goes in S's report.
- **Part B** (after P6 merges to `main` and `p8a` merges `main`): S, resumed with SendMessage, patches the P6-owned sites:
  - `library.c:380/387/1762` ("Unknown Artist", "Unknown Album");
  - `screens/browse.c:97` (`all_labels[]`: "All Songs", "All Albums");
  - `screens/audiobooks.c:38/41` ("Audiobooks", "Podcasts");
  - any others the audit finds in `settings.c`, `nowplaying.c` and `np_*`.

  Line numbers are as of `main` 89dd11a492; S re-greps.
- **Goldens:** the English text is unchanged, so **no golden may change** because of #56.

## 4. Contracts (real code; the orchestrator commits these on `p8a` before fan-out)

Committed in one `p8a: contracts` commit:
- the headers below;
- the `queue.h` event and the `anim.h` strip owner;
- the 8a block in `english.lang` (§4.7);
- `apps/ipodui/p8a_stub.c`, the **single shared stub**. It has three marked sections (USB / UNLIT+DEVMODE+LOG / BOOT-SIM). Each agent **deletes only its own section** in the commit that adds the real file;
- its `apps/SOURCES` line inside `#ifdef HAVE_IPOD_UI`.

P's **step 0** (its first commit, merged to `p8a` before T starts the boot script): `bootlogo.py --c`, the generated data, `bootscreen.c`, `firmware/SOURCES`, and the **round-trip host test** `tests/bootscreen/run.sh`. The test runs the generator, compiles `bootscreen.c` + data on the host into a 320 × 240 `uint16_t` framebuffer, writes a PNG, and requires it to be **pixel-identical** to `docs/design/logo/wheel3d-medium-screen.png`. A missing PNG or generator **fails**; it never skips.

### 4.1 `firmware/export/bootscreen.h` and the generated `bootscreen_data.h`
```c
/* bootscreen.h: the boot screen shared by the bootloader and the firmware
 * (Phase 8a, docs/design/boot-logo.md "wheel3d"). GPL v2+. */
#ifndef BOOTSCREEN_H
#define BOOTSCREEN_H
#ifdef HAVE_IPOD_BOOTSCREEN
#include "bootscreen_data.h"            /* generated by art/boot/bootlogo.py --c */

#if defined(BOOTLOADER)
#define BOOTSCREEN_ATTR                 /* whole bootloader is IRAM-resident */
#define BOOTSCREEN_DATA_ATTR
#else
#define BOOTSCREEN_ATTR      INIT_ATTR  /* firmware: reclaimed after boot */
#define BOOTSCREEN_DATA_ATTR INITDATA_ATTR
#endif

/* Draws the full 320x240 boot screen into the main framebuffer (FBADDR,
 * default viewport): dithered gradient rows, then the pre-composited glyph
 * block. No lcd_update(), no font, no allocation, no boost change.
 * Same code + same data in both stages = pixel-identical hand-off. */
void bootscreen_draw(void) BOOTSCREEN_ATTR;
#endif /* HAVE_IPOD_BOOTSCREEN */
#endif

/* bootscreen_data.h (GENERATED, committed; do not edit) */
#define BOOTSCREEN_W   72               /* medium: 72 x 71 */
#define BOOTSCREEN_H   71
#define BOOTSCREEN_X   /* generated */  /* block origin; ring centre (160, 117) */
#define BOOTSCREEN_Y   /* generated */
#define BOOTSCREEN_GEN /* generated */  /* FNV-1a of bg + block: art identity */
extern const unsigned short bootscreen_bg[240][2];           /* even x, odd x */
extern const unsigned short bootscreen_block[BOOTSCREEN_W * BOOTSCREEN_H];
```

### 4.2 Firmware hooks (P; exact shape)
```c
/* firmware/kernel/include/queue.h, after SYS_VOLUME_CHANGED (contract commit) */
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
/* the backlight began to turn off by its timer (Phase 8a, unlit face) */
#define SYS_IPODUI_BACKLIGHT_OFF  MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 6)
/* the Hold switch changed; data = 1 on, 0 off (Rev 1 H4) */
#define SYS_IPODUI_HOLD_CHANGED   MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 7)
#endif

/* firmware/backlight.c, do_backlight_off(), first statement after
 * backlight_timer = 0;  Rev 1 M2: only when a timeout is configured, so
 * "Off" (timeout < 0, re-entered on every BACKLIGHT_ON) and "Always On"
 * never post */
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    if (backlight_get_current_timeout() > 0)
        button_queue_post(SYS_IPODUI_BACKLIGHT_OFF, 0);
#endif

/* firmware/backlight.c, backlight_hold_changed() */
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    button_queue_post(SYS_IPODUI_HOLD_CHANGED, hold_button ? 1 : 0);  /* H4 */
    if (true)   /* unlit face: Hold on lights it too (unlit.md) */
#else
    if (!hold_button || (backlight_on_button_hold > 0))
#endif

/* apps/gui/usb_screen.c, gui_usb_screen_run() (Rev 1 H1). Each screen keeps
 * exactly one viewportmanager push, so the stock
 * viewportmanager_theme_undo(i, false) at the end (:308) still pops it: */
    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];
        screen->set_viewport(NULL);
        screen->scroll_stop();
#ifdef HAVE_IPOD_UI
        viewportmanager_theme_enable(i, false, NULL);  /* the push the undo pops */
#else
        usb_screen_fix_viewports(screen, &usb_screen_vps_ar[i]);  /* pushes (:143) */
#endif
    }
    ...
    usb_acknowledge(SYS_USB_CONNECTED_ACK, seqnum);
#ifdef HAVE_IPOD_UI
    ui_usb_screen_loop(early_usb);
#else
    usb_screens_draw(usb_screen_vps_ar);
    handle_usb_events(title);
#endif
/* #ifndef HAVE_IPOD_UI around exactly: usb_screens_draw(),
 * handle_usb_events(), draw_usb_keypad_mode(). usb_screen_fix_viewports()
 * becomes unused too under the shell, so it joins the guard (no new
 * warnings); stock code is unchanged. */

/* apps/gui/splash.c, splashf(), right after fmt = P2STR(...) */
#ifdef HAVE_IPOD_UI
    if (!ipodui_stock_ui_allowed())
    {
        va_start(ap, fmt);
        ui_log_va("splash-suppressed", fmt, ap);
        va_end(ap);
        return;                         /* no draw, no sleep (D1) */
    }
    ui_log("splash-pass", "%s", fmt);
#endif
```
P checks that `SYS_EVENT_CLS_MISC` values 6 and 7 are unused tree-wide (`grep -rn "CLS_MISC" firmware apps lib`) and picks the next free values if not. The values are part of the contract commit.

Rev 1 M3 refines the splash hook:
- `splash-pass` happens only while `ipodui_devmode_run()` is on the stack; `ipodui_stock_ui_allowed()` is exactly that flag, so nothing else ever passes.
- A repeat with the same `fmt` pointer within 1 s is counted instead of logged again. The next different line, or the flush, carries `repeat=<n>`. This is needed because `show_search_progress()` splashes at 10 Hz.

### 4.3 `apps/ipodui/usbscreen.h`
```c
#ifndef IPODUI_USBSCREEN_H
#define IPODUI_USBSCREEN_H
#include <stdbool.h>

#define UI_USB_FPS      10              /* arrows, while I/O and lit */
#define UI_USB_FRAMES   12              /* 30-degree steps (U2: 6 over 180) */
#define UI_USB_IO_HOLD  (HZ/2)          /* I/O counts as active this long */
#define UI_USB_DISC_D   80              /* generator constants, mirrored */
#define UI_USB_DISC_CX  160
#define UI_USB_DISC_CY  108
#define UI_USB_BAND_Y   (UI_USB_DISC_CY - UI_USB_DISC_D / 2)   /* 68 */
#define UI_USB_BAND_H   UI_USB_DISC_D                         /* 80 rows */

enum ui_usb_glyph { UI_USB_GLYPH_DOCK30, UI_USB_GLYPH_USBC };
#ifdef HAVE_IPOD_DOCK30
#define UI_USB_IDLE_GLYPH UI_USB_GLYPH_DOCK30
#else
#define UI_USB_IDLE_GLYPH UI_USB_GLYPH_USBC
#endif

struct ui_usb_state {
    signed char frame;      /* identity: arrows frame on screen, -1 = idle glyph */
    bool io;                /* identity: I/O state last drawn */
    bool static_valid;      /* validity: header/body/text are in the framebuffer */
    bool frames_valid;      /* validity: the strip holds this entry's frames
                               (false if the claim was lost or never made) */
};

/* Called by apps/gui/usb_screen.c after usb_acknowledge(); UI thread.
 * Returns on SYS_USB_DISCONNECTED. Boosts only for the entry compose and
 * releases before blocking. 'early' is informational; the loop itself
 * checks ui_shell_started() for fonts/strip (static art, no text if not). */
void ui_usb_screen_loop(bool early);  /* calls backlight_on() at entry (H1) */
bool ui_usb_io_active(void);    /* device: ATA activity; sim: sim_disk_active_until */
bool ui_usb_screen_active(void);/* true only while ui_usb_screen_loop() runs:
                                   the unlit "skip usb" test (Rev 1 M4),
                                   never usb_inserted() */
/* HID (Rev 1 L4): the loop never calls get_hid_usb_action(). usb_hid is
 * off by default (H1b); a developer who turns it on gets an enumerated
 * HID device whose keys the shell ignores. */
bool ui_shell_started(void);    /* ui_main.c: true once fonts + strip are reserved */
#endif
```
`apps/ipodui/anim.h` (contract commit): `#define UI_STRIP_OWNER_USB (-6) /* the USB screen's disc and arrow frames (8a) */`.

### 4.4 `apps/ipodui/unlit.h`
```c
#ifndef IPODUI_UNLIT_H
#define IPODUI_UNLIT_H
#include <stdbool.h>
#include "ipodui/input.h"

/* "stock" variant geometry (docs/design/unlit.md), frozen */
#define UNLIT_DIGIT_Y   62
#define UNLIT_DIGIT_H   68
#define UNLIT_CELL_W    30
#define UNLIT_STROKE    9
#define UNLIT_RADIUS    3
#define UNLIT_INK       56              /* grey levels, quantised to RGB565 */
#define UNLIT_RIM       150
#define UNLIT_STATUS_Y  161             /* status band y 161..197 */
#define UNLIT_STATUS_H  37

/* pure core: unlit_core.c, host-tested (tests/unlit/) */
enum ui_unlit_ev {
    UE_BL_OFF,                          /* SYS_IPODUI_BACKLIGHT_OFF */
    UE_BL_ON_NOKEY, UE_KEY, UE_KEY_RELEASE, UE_WHEEL, UE_BUTTONS_UP,
    UE_HOLD_ON, UE_HOLD_OFF,            /* from SYS_IPODUI_HOLD_CHANGED (H4) */
    UE_USB, UE_SLEEP_NEXT, UE_DEVMODE, UE_MINUTE, UE_STATUS, UE_TICK,
};
enum ui_unlit_act {                     /* bitmask */
    UA_NONE = 0, UA_DRAW_FULL = 1, UA_DRAW_DIGITS = 2, UA_DRAW_STATUS = 4,
    UA_DISMISS = 8, UA_SWALLOW = 16, UA_SKIP = 32,
};
struct ui_unlit_status {                /* identity of the status row */
    unsigned char play;                 /* 0 stopped, 1 playing, 2 paused */
    unsigned char hold, charging;
    unsigned char fill_px;              /* battery fill width drawn, not % */
};
struct ui_unlit_state {
    bool face;                          /* NORMAL / FACE */
    bool swallowing;                    /* drop keys until the dismissing release */
    long swallow_since;                 /* tick of the dismiss (M1 fallbacks) */
    bool skip_next;                     /* the next BL_OFF is the shell's own sleep */
    long skip_stamp;                    /* tick skip_next was set; expires (M2) */
    short shown_min;                    /* identity: minute of day drawn, -1 none */
    bool shown_12h;                     /* identity: format drawn */
    struct ui_unlit_status shown;       /* identity: status drawn */
    bool fb_valid;                      /* validity: framebuffer still holds the face */
};
/* now = current_tick; lit = is_backlight_on(true) when ev arrived.
 * Rev 1 M1: UE_BL_OFF enters FACE only if !lit. In FACE a lone release
 * (UE_KEY_RELEASE with no press seen) also dismisses. Swallowing ends on the
 * dismissing key's UE_KEY_RELEASE, on UE_BUTTONS_UP (button_status() == 0),
 * or UNLIT_SWALLOW_MAX after the dismiss; UE_WHEEL is swallowed for
 * UNLIT_WHEEL_SWALLOW after the dismiss.
 * Rev 1 M2: skip_next expires UNLIT_SKIP_TICKS after it was set. */
#define UNLIT_SWALLOW_MAX    HZ          /* fallback end of swallowing */
#define UNLIT_WHEEL_SWALLOW  (HZ/4)      /* wheel quiet window after dismiss */
#define UNLIT_SKIP_TICKS     HZ          /* skip_next lifetime */
unsigned ui_unlit_step(struct ui_unlit_state *s, enum ui_unlit_ev ev,
                       long now, bool lit);
int ui_unlit_ticks_to_minute(int sec_of_minute);    /* (60 - s) * HZ, min 1 */

/* shell side: unlit.c (UI thread only) */
bool ui_unlit_active(void);
void ui_unlit_sysevent(long ev, intptr_t data); /* input.c: SYS_IPODUI_BACKLIGHT_OFF,
                                                   SYS_IPODUI_HOLD_CHANGED */
bool ui_unlit_filter(const struct ui_input *in);    /* true = consumed */
int  ui_unlit_timeout(void);            /* ticks to the next minute boundary */
void ui_unlit_tick(void);               /* minute / status redraw + band push */
void ui_unlit_suppress_next(void);      /* call right before the sleep path's backlight_off() */
void ui_unlit_foreign_draw(void);       /* someone else drew: fb_valid = false */
/* Drawing contract (Rev 1 L6): digits, colon, play, pause, battery outline
 * and fill, the padlock (Hold) and the charging bolt (XOR over the fill),
 * all procedural. scroll_stop() on FACE enter (no marquee writes under the
 * face). */
#endif
```

### 4.5 `apps/ipodui/devmode.h` and `apps/ipodui/uilog.h`
```c
/* devmode.h */
#ifndef IPODUI_DEVMODE_H
#define IPODUI_DEVMODE_H
#include <stdbool.h>
struct ui_screen;
#ifdef HAVE_IPOD_UI_DEVMODE
#define DEVMODE_CHORD       (BUTTON_SELECT | BUTTON_LEFT)
#define DEVMODE_HOLD_TICKS  (3 * HZ)
bool ipodui_devmode_feed(const struct ui_screen *top, long raw_btn); /* true = consumed */
void ipodui_devmode_run(void);              /* the Developer list; returns */
bool ipodui_devmode_boot_requested(void);   /* RIGHT held (main.c, before the shell) */
bool ipodui_stock_ui_allowed(void);         /* true only while ipodui_devmode_run()
                                               is on the stack (Rev 1 M3) */
/* Rev 1 H1: ipodui_devmode_run() wraps do_menu() in exactly one
 * viewportmanager_theme_enable(i, true, NULL) / viewportmanager_theme_undo(i,
 * false) pair per screen and never pushes on return. VPSTACK_DEPTH is 16, so
 * one unbalanced push per visit would panic after 16 visits. On return:
 * scroll_stop(), the shell's fonts and viewport, header cache invalidated,
 * one full redraw (L6). */
extern const struct ui_screen_ops ui_about_screen;  /* Rev 1 M6: the About
                                stub, .name "about" (screens/stub.c, U2);
                                devmode arms on top->ops == &ui_about_screen */
#else
static inline bool ipodui_stock_ui_allowed(void) { return false; }
#endif
#endif

/* uilog.h */
#ifndef IPODUI_UILOG_H
#define IPODUI_UILOG_H
#include <stdarg.h>
#include "gcc_extensions.h"
#define UI_LOG_RING  768                    /* static; 12 x 64 B lines */
#define UI_LOG_FILE  ROCKBOX_DIR "/ipodui.log"
#define UI_LOG_MAX   16384                  /* then rotate once to .log.1 */
/* Ring + DEBUGF("ipodui: <tag> <text>\n"). Any CPU-core thread; never
 * yields; COP callers are dropped and counted. */
void ui_log(const char *tag, const char *fmt, ...) ATTRIBUTE_PRINTF(2, 3);
void ui_log_va(const char *tag, const char *fmt, va_list ap);
/* Rev 1 H3: register_storage_idle_func() is a oneshot
 * (add_event_ex(DISK_EVENT_SPINUP, true, ...), ata_idle_notify.c:40), so
 * ui_log_init() calls add_event_ex(DISK_EVENT_SPINUP, false, ui_log_idle_cb,
 * NULL) once (idempotent). The callback returns at once when nothing is
 * pending. It runs on the storage thread (2 KB stack): it copies one line at
 * a time into a 64 B local buffer, checks the seq against the writer, and
 * writes it, using under 128 B of stack in all. A line logged while a flush
 * runs gets a later seq and goes out on the next flush; it is never torn.
 * No usb_inserted() test: storage.c:180 already skips the idle notifies in
 * USB mode, and USB_POWERED (charging) must still flush.
 * Device timing: the notify fires as the disk goes to sleep, so at most once
 * per spin-down cycle (about every 30 s while the disk is in use), never on
 * its own. */
#define UI_LOG_LINE  64
struct ui_log_line { unsigned short seq; char text[UI_LOG_LINE - 2]; };
void ui_log_init(void);
void ui_log_flush(void);                    /* also from SYS_POWEROFF */
#endif
```

### 4.6 Configure flags (P, `tools/configure`)

| Flag | Defines | Default |
|---|---|---|
| `--ipodui` | n/s types: `HAVE_IPOD_UI HAVE_IPOD_BOOTSCREEN HAVE_IPOD_UI_DEVMODE`; type b: `HAVE_IPOD_BOOTSCREEN` only | – |
| `--ipodui-release` (with `--ipodui`) | drops `HAVE_IPOD_UI_DEVMODE` | off (⚠️ D7 = POL-4) |
| `--dock30` (with `--ipodui`) | `HAVE_IPOD_DOCK30` | off (this unit is a USB-C mod) |

### 4.7 `english.lang`: the 8a block (contract commit; appended, `*: none` / `ipodui:`)

| ID | Text |
|---|---|
| `LANG_IPODUI_USB_CONNECTED` | Connected |
| `LANG_IPODUI_USB_EJECT_FIRST` | Eject before disconnecting. |
| `LANG_IPODUI_UPDATING_LIBRARY` | Updating Library... |
| `LANG_IPODUI_MUSIC` / `_PODCASTS` / `_EXTRAS` / `_SHUFFLE_SONGS` / `_NOW_PLAYING` | Music / Podcasts / Extras / Shuffle Songs / Now Playing |
| `LANG_IPODUI_COVER_FLOW` / `_PLAYLISTS` / `_ARTISTS` / `_COMPILATIONS` / `_GENRES` / `_COMPOSERS` / `_SEARCH` | Cover Flow / Playlists / Artists / Compilations / Genres / Composers / Search |
| `LANG_IPODUI_ON_THE_GO` | On-The-Go |
| `LANG_IPODUI_ALL_SONGS` / `_ALL_ALBUMS` | All Songs / All Albums |
| `LANG_IPODUI_UNKNOWN_ARTIST` / `_UNKNOWN_ALBUM` | Unknown Artist / Unknown Album |
| `LANG_IPODUI_AM` / `_PM` | AM / PM |

Rev 1 L1: the devmode list's title comes from a new `LANG_IPODUI_DEVELOPER` ("Developer"), with the feature tag **`ipodui_devmode`** (`*: none` / `ipodui_devmode: "Developer"`).
- `apps/features.txt` gets `#if defined(HAVE_IPOD_UI_DEVMODE)` / `ipodui_devmode` / `#endif`, in the contract commit.
- The items use the stock `LANG_DEBUG` and `LANG_BACK`; there are no literals.
- A `--ipodui-release` build must give `strings rockbox.ipod | grep -ci developer` = 0. No stock phrase contains "developer" today.

S may append more from the audit. The audit **may not change** an existing ID's text.

### 4.8 Trace lines (sim; `ipodui: ` prefix)

Shell code uses `UPDATE_TRACE` or `ui_log`. `apps/main.c` and `apps/gui/*` use `DEBUGF`. **`logf` is a no-op in `apps/ipodui` and is never the only trace.**

| Line | From | Rule (check_trace.py, T) |
|---|---|---|
| `boot screen gen=<hex>` | `main.c` | p8a-boot: exactly 1, gen = the data header's |
| `usb enter glyph=<usbc\|dock30> shell=<0\|1>`, `usb io=<0\|1>`, `usb exit frames=<n> boost=<n>` | usbscreen.c | p8a-usb: enter/exit paired, `boost=0` at exit; between them only `update usb:*` pushes |
| `update usb:static`, `update usb:frame y=68 h=80`, `update usb:header y=0 h=22` | `ui_lcd_push` | `usb:frame` only while `io=1` and lit |
| `unlit enter`, `unlit exit <key\|holdoff\|usb>`, `unlit skip <sleep\|usb\|devmode>`, `unlit swallow <key>` | unlit.c | p8a-unlit: between enter and exit only `update unlit:*`; one `unlit:digits` per minute step |
| `update unlit:full`, `update unlit:digits y=62 h=68`, `update unlit:status y=161 h=37` | `ui_lcd_push` | – |
| `devmode arm`, `devmode enter`, `devmode exit` | devmode.c | p8a-devmode: enter needs ≥ 3 s after arm |
| `splash-suppressed <text>`, `yesno-suppressed <text>`, `splash-pass <text>` | uilog.c | every `ipodui-*` script: 0 `splash-pass` outside devmode scripts; p8a-splash: ≥ 1 `splash-suppressed` |

Rev 1 adds these trace lines:
- `unlit swallow-end <release|buttons-up|timeout>`;
- `unlit skip-expired`;
- `hold <on|off>` (input.c, from `SYS_IPODUI_HOLD_CHANGED`);
- `log flush lines=<n> seq=<a>-<b>`;
- `splash-suppressed ... repeat=<n>`.

**Every rule fails when its anchor lines are absent.** A p8a script with no `unlit enter` fails the unlit rule; it never skips.

## 5. Ownership (worktrees; one owner per file)

| Agent (tier) | Owns | Must not edit |
|---|---|---|
| **P** firmware-platform (opus) | `tools/configure` (§4.6); `bootloader/ipod.c`; `firmware/common/bootscreen.c`, `bootscreen_data.c` and `firmware/export/bootscreen_data.h` (generated); `firmware/SOURCES` (2 guarded lines); `art/boot/bootlogo.py` (`--c`); `tests/bootscreen/*`; `apps/main.c` (show_logo_boot, clear guard, boot escape); `firmware/backlight.c` (2 hooks); `apps/gui/usb_screen.c`, `splash.c`, `yesno.c` (guarded hooks); the splash inventory (report) | `apps/ipodui/*`, P6 files, contract headers |
| **U1** ui-fidelity (opus) | `apps/ipodui/usbscreen.c`, `usbscreen_data.h` (generated); `art/usb/usbscreen.py`; `docs/design/usb/*` (reference PNGs + a short README); `apps/ipodui/header.c` (a dark-header entry point for the USB screen, AM/PM → LANG); deletes the USB section of `p8a_stub.c` | `ui_main.c`, `input.c`, P6 files, contract headers |
| **U2** ui-fidelity (opus) | `apps/ipodui/unlit.c`, `unlit_core.c`, `devmode.c`, `uilog.c`; `apps/ipodui/ui_main.c` (FACE suppression, devmode feed, `ui_shell_started()`, sleep-path `ui_unlit_suppress_next()`, POWEROFF flush, `ui_log_init()`); `apps/ipodui/input.c` (SYS_IPODUI event routing, `ui_unlit_filter`); `tests/unlit/*`, `tests/uilog/*`; deletes the UNLIT+DEVMODE+LOG section of `p8a_stub.c` | `header.c`, `usbscreen.c`, P6 files, contract headers |
| **S** ui-fidelity (sonnet) | #56 part A: `apps/ipodui/menu.c`, `screens/playlists.c`, `screens/stub.c`, `screens/panels.c` (literals only); `apps/lang/english.lang` (appends after the contract block); part B later (§3.6) | everything else |
| **T** tooling (sonnet) | `uisimulator/common/sim_cmd_sys.c` (new) + its one registration line in `sim_script.c`; the sim boot-capture and boot-keys hooks (§8); `scripts/validate.sh` (a `boot-ui` build: `--type=b --ipodui`, built when `bootloader/` or `firmware/` changed and in the full gate, plus the stock `boot` `.text` identity check against `build/boot/.size-baseline`); `scripts/check_trace.py` (8a rules); `scripts/gate_map.txt` (8a entries); `tests/ui/ipodui-p8a-*.script`; compose sheets; deletes the BOOT-SIM section of `p8a_stub.c` | `apps/`, `firmware/`, `bootloader/`, `lib/` |

- **Orchestrator.**
  - The contract commit (§4).
  - `apps/SOURCES` additions (requested by U1 and U2, inside `#ifdef HAVE_IPOD_UI`).
  - The P6-file hooks below, applied after P6 merges.
  - Merging the additive conflicts with P6 in `english.lang` (P6 block first, then 8a's), `sim_script.c` registration lines, `check_trace.py` and `gate_map.txt`.
  - The `docs/hardware.md` budget table and flag rows, and the ROADMAP P10 list, at close.
- **Hooks in P6-owned files** (the orchestrator applies them after P6's merge; each is a guarded macro, not an `#ifdef` inside a macro argument):
  - **H1a** `apps/settings_list.c`: above the backlight block, `#ifdef HAVE_IPOD_UI` / `#define LCD_SLEEP_DEFAULT 90` / `#else` / `#define LCD_SLEEP_DEFAULT 5` / `#endif`. In the `lcd_sleep_after_backlight_off` `TABLE_SETTING_LIST`, `5,` → `LCD_SLEEP_DEFAULT,`.
  - **H1b** `apps/settings_list.c`: the same pattern, `USB_HID_DEFAULT` (`false` under `HAVE_IPOD_UI`, else `true`), in `OFFON_SETTING(0, usb_hid, LANG_USB_HID, USB_HID_DEFAULT, ...)`.
  - **H2** `library.c`, `screens/browse.c`, `screens/audiobooks.c`, plus any other P6 file the audit names: #56 part B (S, resumed), reviewed by the orchestrator.
  - **H1c** (Rev 1 L5) `apps/settings_list.c`: `WARNON_ERASE_DYNPL_DEFAULT` (`false` under `HAVE_IPOD_UI`, else `true`) in `OFFON_SETTING(0, warnon_erase_dynplaylist, ...)`. Its default is true, and with D2 the erase prompt would otherwise cancel playback silently.
  - **H3** (Rev 1 M6) `apps/ipodui/screens/settings.c:471–472`: the About row's target `&ui_stub_screen` → `&ui_about_screen` (one line).
- **Rev 1 ownership changes:**
  - `apps/ipodui/screens/stub.c` moves from S to **U2** (the `ui_about_screen` stub). S has no known literal in it; any S finds is reported to U2.
  - **T** is explicitly granted `firmware/target/hosted/sdl/button-sdl.c` (`IPODUI_SIM_BOOT_KEYS`, and `hold_switch` dropping keys while Hold is on).
  - `scripts/shots.sh`, `uisimulator/common/SOURCES` and `tests/ui/README.md` are also edited by P6, so they join the orchestrator's merge list. T edits them on `p8a` as usual (additive) and doesn't wait for P6.
  - The contract commit also adds `uisimulator/common/sim_capture.h` (`void sim_capture_screen(const char *name);`) and its no-op stub in `p8a_stub.c` (BOOT-SIM section). T replaces the stub with the real implementation.
  - The contract commit adds `apps/features.txt` `ipodui_devmode` (L1) and the `ui_about_screen` declaration in `screens/screens.h` (M6).
  - T adds sim command `storage_idle`, which calls `call_storage_idle_notifys(true)` (H3).
- **Read lists** (≤ 3 each; contract headers are read as code and don't count):
  - **P:** this brief §3.1, §3.3, §4.2, §4.6; `docs/design/boot-logo.md` §"Format decision" and §"Integration plan"; `bootloader/ipod.c` + `bootloader/common.c` `printf()`.
  - **U1:** this brief §3.2; `docs/design/spec/system-screens.md` (USB, dark screens); `docs/reference/7g/MEASUREMENTS.md` rows "USB/charge header" to "USB text", plus `docs/design/fidelity-audit-1.md` §USB.
  - **U2:** this brief §3.3 (log), §3.4, §3.5; `docs/design/unlit.md` (stock variant, cost plan, state machine); `docs/design/devmode.md` §1–3 (with its MINIMAL note).
  - **S:** this brief §3.6, §4.7; issue #56 (`scripts/forgejo.py show 56`); one existing `LANG_IPODUI_*` phrase in `english.lang` as the format example.
  - **T:** this brief §4.8, §7, §8; `tests/ui/README.md`; `docs/phase-nav-brief.md` Revision 2 (`record`, `make_video.sh`).

## 6. Device-reality checklist (numbers; the builds are `build/boot`, `build/fw`, `build/fw-ui` as of 2026-10-07; agents re-measure on their branch)

1. **Compiled features.**
   - `HAVE_IPOD_BOOTSCREEN`: bootloader and firmware.
   - `HAVE_LCD_SLEEP` / `HAVE_LCD_SLEEP_SETTING` / `HAS_BUTTON_HOLD` / `HAVE_BACKLIGHT` are all on ipodvideo, sim and device alike. `CONFIG_BACKLIGHT_FADING` is `BACKLIGHT_FADING_PWM` on the device, and `BACKLIGHT_FADE_IN_THREAD` **includes** `BACKLIGHT_FADING_PWM` (`backlight.c:68–71`). So `do_backlight_off()` calls `backlight_setup_fade_down()`, and the LCD-sleep countdown starts in one of two places:
     - immediately, there, if the fade-out is off;
     - otherwise in `backlight_isr()` when the PWM fade reaches 0. The `backlight_fade_out` default is 2000 ms (`settings_list.c:1541`), so this is about 2 s after the event.

     The face therefore appears about 2 s before the 90 s countdown starts (Rev 1 L2: not accepted as written, see Revision 1). P checks whether the sim runs the PWM ISR path; if not, the 2 s offset is a P10 item.
   - `HAVE_USBSTACK`: USB arrives through `default_event_handler()` → `gui_usb_screen_run(false)`. The early-USB loop in `main.c` `init()` is `#ifndef HAVE_USBSTACK`, so it isn't compiled on the device; only "No partition found" calls `gui_usb_screen_run(true)`.
   - `USB_ENABLE_HID` is compiled (`HAVE_USB_HID_MOUSE`); its default is off under the shell (H1b).
   - `storage_last_disk_activity()` is ATA on the device and `-1` in the sim (hostfs), hence the `sim_disk_active_until` stand-in.
   - The bootloader is built without `HAVE_IPOD_UI`.
   - The `boot-ui` gate build proves the bootloader path compiles. The bootloader can't run in the sim; the host round-trip test and the device test (§9) cover it.
2. **Memory.**
   - **No `core_alloc` anywhere in 8a.** The USB screen uses the existing strip (claimed at entry; audio is already stopped by the USB stack). Unlit draws straight into the framebuffer. Devmode's stock list uses stock statics. The log is a 768 B static.
   - Shell static RAM: 1,168 → ≈ 1,980 B (ring 768, unlit state ≈ 16, USB state 4, devmode ≈ 12). Under the 64 KB budget; every static < 1 KB.
   - The bootscreen block (10,224 B) and gradient table (960 B) are `INITDATA_ATTR` in the firmware, **0 B resident**. That's the stated exemption from the "> 1 KB static" rule, and the gate's static check must not count `.initdata`.
   - **Indirect allocations:** none. The USB screen draws with the already-loaded shell fonts (no `font_load`). The devmode menu may call stock code that allocates, which is accepted inside devmode.
   - **Bootloader IRAM:** 52,940 B used of 122,880 (128 KB − 8 KB stack) → **≈ 64,330 B** (+10,224 block + 960 table + ≈ 200 code): ≈ 58.5 KB stays free.
3. **CPU** (nothing scales with the library):
   - **boot draw:** 76,800 framebuffer stores + 71 row copies, < 5 ms at 30 MHz, in each stage;
   - **USB entry compose:** ≈ 1.9 M instructions boosted (≈ 24 ms at 80 MHz);
   - **USB frame:** 80 × 160 B `memcpy` (the disc's width; Rev 1 L3) ≈ 3.5k instructions + the 80-row band push;
   - **unlit first show:** ≈ 40 fillrects ≈ 0.2 ms;
   - **unlit minute:** ≈ 0.2 ms;
   - **devmode feed:** O(1) per event.

   No boost outside the USB compose (a deliberate deviation: `usb_storage` boosts anyway, Rev 1 L3) and the boot's existing `init()` boost.
4. **Storage.**
   - **Boot / USB / unlit / devmode chord:** 0 reads and 0 writes of their own. USB I/O is the host's.
   - **Log flush:** 1 append, ≤ 768 B, from the persistent `DISK_EVENT_SPINUP` handler, so it rides an already-awake disk: **0 extra wakes**. It fires as the disk goes to sleep, at most once per spin-down cycle (about every 30 s while the disk is in use). Rotation: 1 rename when the file passes 16 KB. Sim: `storage_idle` triggers it.
   - **Sim stand-ins with device timing:** USB I/O holds 0.5 s after the last sector, as on the device (`disk_active <ms>`). The backlight-off event is posted by the same `firmware/backlight.c` code in the sim.
5. **LCD** (measured: a 24-row band ≈ 2 ms, the 220-row body 13–20 ms at 30 MHz, `code-audit-1.md`; full screen ≈ 16–20 ms):
   - **Boot:** 1 full push per stage.
   - **USB:** 1 full push at entry. While I/O and lit, 10 fps × one 80-row band (≈ 6–7 ms) ≈ 6.5 % bus time. Idle: no pushes except the clock band at the minute (≈ 2 ms). Dark: none.
   - **Unlit:** 1 full push per backlight-off, then 1 × 68-row band a minute (≈ 4–6 ms) for at most 2 minutes before LCD sleep, plus a 37-row status band (≈ 3 ms) on a change. 0 pushes while asleep.
   - **Dismiss:** 1 full redraw and push.
6. **Threads and locks.**
   - The backlight thread only posts to the button queue (`button_queue_post`, IRQ-safe).
   - Everything else draws on the UI thread. The USB loop runs inside `default_event_handler()` on the UI thread, and the main loop doesn't run meanwhile.
   - `ui_log` may be called from any CPU-core thread. The kernel is cooperative per core and `ui_log` never yields, so no lock is needed. COP calls are dropped.
   - The flush copies one line at a time into a 64 B local buffer (seq-checked, < 128 B of the storage thread's 2 KB stack), then does file I/O with nothing held. A line logged during a flush goes out on the next one (`tests/uilog` covers it).
   - The backlight thread also posts `SYS_IPODUI_HOLD_CHANGED` (H4). Everything else in `devmode`/USB keeps one viewportmanager push per entry and one pop per exit (H1).
   - No mutex is taken from the UI. No pointer is held across a yield: the strip is pinned only while composing or blitting a frame, and re-claimed per frame through `ui_strip_claim` (`kept` false → `frames_valid = false` → recompose).
7. **Upstream splashes and prompts:** covered at the funnel (§3.3). P's inventory lists each linked call site and whether it's reachable. Not funnelled: `panicf` (⚠️ D5), the `main.c` "No partition found" screen and the bootloader error text (recovery, ⚠️ D6).
8. **Persistence.**
   - The face, USB screen and boot screen keep no state.
   - The log survives a power-off as of its last flush. A hard reset or empty-battery cut loses the ring (≤ 12 lines), which is accepted. Power-off mid-append leaves at worst a truncated last line.
   - USB connected mid-flush: `storage.c` doesn't run the idle notifies in USB mode, and the USB stack waits for the storage thread before it takes the disk. A charging-only connection (`USB_POWERED`) still flushes.
   - Devmode leaves stock settings changes in place (intended) and nothing else.
9. **Scale:** no 8a code walks the library. At 50k tracks and 5k albums every cost above is unchanged. The post-USB reverse scan is unchanged upstream behaviour.
10. **Deferred or retried work:** the log flush is the only deferred work.
    - Retried in: disk idle (normal), after USB (the next idle callback), after a write error (the line is dropped and counted), at `SYS_POWEROFF`.
    - Database states (ready / fresh / dirty) are **N/A**: 8a never touches the database, and devmode's rebuild items are out of scope.
    - Test scripts: `ipodui-p8a-splash` (normal flush via `storage_idle`), plus `tests/uilog/` on the host: overflow, rotation, a line logged during a flush, and repeat collapse.
11. **Stock firmware unchanged:**
    - Every upstream edit is guarded (`HAVE_IPOD_UI`, `HAVE_IPOD_BOOTSCREEN`, `HAVE_IPOD_UI_DEVMODE`, `!defined(BOOTLOADER)`).
    - Stock `fw` `.text` is byte-identical to `build/fw/.size-baseline` (1,142,204) **with `english.lang` held equal** (the accepted exception).
    - Stock `boot` `.text` (52,940) is byte-identical too (T adds the check).
12. **Existing upstream paths doing the same job:**

    | Job | Upstream path | Wins | How the other is disabled |
    |---|---|---|---|
    | Firmware boot screen | `show_logo_boot()`: `bm_rockboxlogo` + "Ver." | `bootscreen_draw()` | `#ifdef HAVE_IPOD_BOOTSCREEN` body swap; the bitmap is unreferenced |
    | Bootloader banner | `printf` lines (`verbose` gates the push) | the boot screen | already push-gated; no change beyond drawing first |
    | Other targets' bootloader logo | `bootloader/show_logo.c` | n/a | not linked by `ipod.c` |
    | Clear between boot and UI | `main()` `clear_display(); update()` | the shell's first full frame | `#ifndef HAVE_IPOD_UI` |
    | USB screen | `usb_screens_draw()` + `handle_usb_events()` (+ HID keypad title) | `ui_usb_screen_loop()` | `#ifdef HAVE_IPOD_UI` at the call; statics guarded; `usb_hid` default off |
    | Splash suppression | `main.c` dircache/tagcache and `misc.c` shutdown/battery `DEBUGF` guards | the funnel (they stay, now redundant and harmless; left alone to keep the diff small) | – |
    | First-keypress filter | `bl_filter_first_keypress` (`button.c`) | the shell's swallow in `input.c` (it also works while the face is lit) | the setting stays default off |
    | LCD sleep | `backlight_lcd_sleep_countdown` → `lcd_sleep()` | upstream (the shell does nothing) | default 90 via H1a |
    | Debug access | stock `root_menu()` → `debug_menu()` | the devmode list → `debug_menu()` | `root_menu()` is never called in the shell build; devmode.md's `root_menu_dev()` is not built |
    | Disk-activity sim signal | `sim_disk_active_until` (`sim_cmd_art.c`, read by `artcache.c`) | reused as is by `ui_usb_io_active()` | no second stand-in |
    | Clock / battery sim pins | `sim_clock`, `clock_format`, `battery`, `charger` | reused | – |

13. **Builder iteration predicate:** 8a has no background builder. The only deferred worker, the log flusher, selects **"ring holds ≥ 1 line with seq > last written seq"** at each `DISK_EVENT_SPINUP` notify (`storage.c` already suppresses those in USB mode) and at `SYS_POWEROFF`. That covers lines logged during USB (flushed at the first idle after), lines logged before the shell started (devmode boot escape: the ring is static, `ui_log_init()` registers the callback at shell start, and `ipodui_devmode_run()` calls the idempotent `ui_log_init()` too), and overflow (counted).

## 7. Acceptance criteria

1. `scripts/gate-run --quick` passes on each branch.
   - The integration full gate passes, with the `boot-ui` build, and stock `fw` and `boot` identity (#11).
   - No new warnings, and no static > 1 KB outside `.initdata`.
   - No new user-visible literal: S's audit table is empty after part B.
   - No "Rockbox" string newly visible in the shell paths.
2. **Boot:**
   - The host round-trip test passes (generator → C → PNG = `wheel3d-medium-screen.png`, threshold 0).
   - The sim `boot-fw.png` is pixel-identical to the host `boot-bl.png` and the reference.
   - The `bm_rockboxlogo` and `usblogo` symbols are absent from `fw-ui`.
   - `fw-ui` `rockbox.ipod` shrinks by ≈ 59 KB (est.: −62,720 − 16,896 + 11,184 + ≈ 9,300 of new code and data). Resident (non-init) ≈ −6.8 KB. Reported as measured.
3. **USB:**
   - Idle shots for both glyphs, plus ≥ 3 distinct I/O frames.
   - After `usb_disconnect`, the previous screen is pixel-identical to the shot before `usb_connect`.
   - Trace rules pass (`boost=0` at exit, only `usb:*` pushes inside).
   - A side-by-side with photo 1226 and photo 1165 for the owner round.
4. **Unlit:**
   - The 4 scenes (12 h playing 4:01 55 %, 24 h paused 09:41 100 %, 24 h stopped 15:03 8 %, 12 h Hold charging 12:58 45 %) are pixel-identical to `docs/design/unlit/stock-*-lit.png`. U2 lists any deviation pixel by pixel with its reason; this needs orchestrator sign-off.
   - A minute step pushes exactly one digits band.
   - The dismissing SELECT is swallowed: no push of any screen and no selection change.
   - The screen after dismiss is pixel-identical to the one before the face.
   - Charging sleep shows no face. USB over the face gives the USB screen, then NORMAL.
   - `lcd_sleep_after_backlight_off` = 90 on a fresh settings reset (after H1a).
   - `tests/unlit` covers every row of the §3.5 table.
5. **Devmode:**
   - The chord on About for 3.2 s gives the Developer list. Debug opens. Back returns to the About stub, pixel-identical to before.
   - The chord on any other screen does nothing.
   - The boot escape gives the Developer list before the main menu.
   - `--ipodui-release` builds: P builds it once and reports `strings | grep -ci developer` = 0 and the size delta.
6. **Splashes:** `ipodui-p8a-splash` (`splash_probe "probe"`) logs `splash-suppressed probe`, the screen is unchanged, and `/.rockbox/ipodui.log` contains the line after the next flush. 0 `splash-pass` in every non-devmode script.
7. **#56:** every pre-existing golden is unchanged.

## 8. Review artifacts (T scripts them, the agents propose goldens, `golden_capture.sh` writes them)

Sim commands T adds in `sim_cmd_sys.c`:
- `hold_switch on|off` (the sim's `button_hold()` plus `backlight_hold_changed()`);
- `backlight_expire` (calls `backlight_off()`, the real event path);
- `usb_glyph usbc|dock30` (sim only);
- `splash_probe <text>`;
- `chord <A>+<B> <ms>` (devmode.md §6).

Boot hooks:
- a `# boot_keys: RIGHT <ms>` script header that `shots.sh` passes as env `IPODUI_SIM_BOOT_KEYS`, read by the sim button driver;
- `sim_capture_screen(const char *name)`, declared in a new `uisimulator/common/sim_capture.h` and called by P from `show_logo_boot()` under `SIMULATOR`. It writes `out/shots/<script>/<name>.png` when `IPODUI_SHOT_DIR` is set, and is a no-op otherwise.

| Artifact | Script | Content |
|---|---|---|
| Boot hand-off sheet | `ipodui-p8a-boot` | `[boot-bl (host) \| boot-fw (sim) \| diff (must be empty) \| first shell frame]`, plus `boot.mp4` (the sim boot recorded from power-on) |
| USB sheet | `ipodui-p8a-usb` | idle USB-C, idle 30-pin, I/O frames 0/3/6/9, after disconnect; `usb-io.mp4` (2 s at 10 fps); side-by-side with photo 1226 / photo 1165 |
| Unlit sheet | `ipodui-p8a-unlit`, `-unlit-sleep` | 4 scenes lit; the same tone-mapped to "unlit" (paper → (182,177,165), ink → (60,58,52), as `unlit.py`), next to `stock-*-unlit.png`; the Hold scene; before-face / after-dismiss pair |
| Devmode sheet | `ipodui-p8a-devmode`, `-devmode-boot` | About stub, Developer list, Debug, back on About; boot escape |
| Splash pair | `ipodui-p8a-splash` | before and after the probe (identical) plus the log excerpt |

## 9. Device-only checks (go to `docs/ROADMAP.md` → Device checklist (P10) at close)

1. **Boot hand-off.** Install the bootloader and firmware built from the same commit. Power on and film at 60 fps.
   - Expect: the Apple ROM logo (if any), then our gradient and wheel, with no flash or jump when the firmware takes over, and no black frame before the main menu.
   - Hold RIGHT: the boot text shows, with `bootscreen gen` equal to the firmware's (`System Info`/log).
   - Time from power-on to the first frame.
2. **Gradient and dither on the panel.** Is the 2 × 2 dither invisible at arm's length? Compare brightness and hue next to a 7G.
3. **Backlight vs image.** Does `backlight_init()` come on over the image, with no white flash?
4. **Silent errors.** Rename `rockbox.ipod`. Expect the error text over the gradient, then the stock instructions. MENU at boot still reaches Apple's firmware.
5. **USB.**
   - Connect to a Mac. Idle: the USB-C glyph. Copying 1 GB: arrows spin at about 10 fps and stop about 0.5 s after the copy.
   - Time a 1 GB copy with the arrows running vs a build with the animation disabled (`usb_glyph` frozen), to show the frames don't slow transfers (Rev 1 L3).
   - Eject, then disconnect: back to the previous screen.
   - The current draw with the arrows spinning vs idle.
   - The macOS volume mounts, and no HID device enumerates.
6. **Unlit.**
   - Readability unlit in daylight and indoors.
   - The one-frame wake flash (⚠️ U4).
   - Whether charger insertion lights the face.
   - Hold on over a dark face lights it.
   - The band vs narrow-rect push cost (⚠️ U9).
   - Current: backlight off with the BCM on (face) vs the LCD asleep. That's the price of 85 s more BCM time per timeout than today's 5 s default. Estimate ≤ 10 mA × 85 s ≈ 0.24 mAh per timeout, about 24 mAh/day at 100 timeouts, < 1 % of 3000 mAh. To be measured.
7. **Devmode.** The chord feel (3 s) on the real wheel. Debug → battery and buffer screens work. Boot escape with RIGHT.
8. **Log.** After a day of use, `/.rockbox/ipodui.log` exists, stays ≤ 16 KB plus one rotation, and holds no line from during USB.
9. **(fix round) Log rotation stack.** Pad `/.rockbox/ipodui.log` past 16 KB over USB, eject, use the device until the disk spins down twice (the storage-thread flush marks the rotation due; `ui_log_service()` rotates on the UI thread while the disk spins). Expect `ipodui.log.1` holding the old file and a fresh `ipodui.log`. Then Debug → OS stacks: the storage thread's stack use stays below its 2 KB with margin (it now runs open/write/close only, never remove/rename).
10. **(fix round) USB entered dark.** Let the backlight time out on a menu, then connect USB (the screen comes up with the backlight off or fading): within 0.5 s of the backlight coming back by any path (a key, the charger, the host), the arrows and clock run again; nothing freezes until a keypress.
11. **(fix round) Now Playing's stop pop.** With Now Playing up, let the last track of the queue end naturally: Now Playing pops to the previous screen. Again with the face up (backlight timed out) and a SELECT wake: the face goes, the press is swallowed, Now Playing pops by itself, and the next key acts normally. ⚠️ On `p8a-int` Now Playing re-snapshots only when its playback seq moves (`PLAYBACK_EVENT_TRACK_CHANGE`); a plain stop sends none, so a natural end of queue may never be noticed (P6 to check after its merge; the sim script uses `np_seq_bump` as the stand-in).

## 10. ⚠️ Frozen defaults (the owner round confirms; not revised mid-phase)

| # | Default | Alternative |
|---|---|---|
| D1 | A suppressed splash also drops its `sleep(ticks)` (no invisible pause) | keep the delay |
| D2 | A suppressed yes/no prompt answers **No** | answer the prompt's timeout default |
| D3 | Devmode: chord only, no 1 s hint, one click tick at 3 s; menu = Debug + Back, using `LANG_IPODUI_DEVELOPER` (feature `ipodui_devmode`), `LANG_DEBUG` and `LANG_BACK` (Rev 1 L1), never "Rockbox" | devmode.md's ellipsis hint; more items |
| D4 | `HAVE_IPOD_DOCK30` picks only the glyph in 8a; iAP / dock compile-out is #54 part 2 later | do it in 8a |
| D5 | `panicf` keeps its stock text screen (the only crash diagnostic without a serial port) | a native crash screen that logs |
| D6 | Bootloader and "No partition found" errors show text over the gradient (recovery) | a native "Use iTunes to restore"-style screen (8b) |
| D7 | POL-4: the device build keeps devmode; `--ipodui-release` exists for a sealed build | ship sealed only |
| D8 | (fix round, from D2) "Boot changed, reboot now?" (`apps/misc.c:883`, `check_bootfile()` after a new `rockbox.ipod` is copied over USB) is a suppressed yes/no, so it answers **No**: the new firmware runs only from the next power-on, never by an immediate rolo. Already an owner question | answer Yes (reboot into the new build at once), or a native prompt |
| D9 | (fix round) The RIGHT boot escape is also the bootloader's verbose key (`bootloader/ipod.c`), so a developer boot always shows the boot text over the gradient too, and a RIGHT held only for the boot text also opens the Developer list | separate keys |
| U1 | USB geometry from the on-device owner photo: disc 80 px at (160, 108), text at y 190/205; colours from `MEASUREMENTS.md` | `usb.png` asset: disc 94 px at (160, 100), text at y 176/195 |
| U2 | 12 arrow frames at 30° (6 over 180° if the strip is short), 10 fps, only while lit | other counts and rates |
| U3 | The press that dismisses the face is swallowed | the press also acts |
| U4 | Accept a possible one-frame face flash on wake from LCD sleep | repaint before wake (racy) |
| U5 | PLAY-hold sleep shows no face (dark, then LCD sleep) | show the face |
| U6 | USB header: the clock centred when Time in Title is on, else "iPod"; no title/clock alternation | alternate as on lists |
| U7 | The face shows nothing for "stopped" except the battery (+ padlock) | a stop glyph |
| U8 | The face is never shown while the backlight setting is "Off" (Rev 1 M2: the event isn't even posted) | always |
| U9 | Unlit minute push: the full-width 320 × 68 band (hardware.md: bands burst fastest) | the 30 × 68 cell rect, if P10 finds it cheaper on the BCM |

## Revisions
*(Deltas go to running agents by SendMessage; they are recorded here.)*

## Revision 1 (brief review, 2026-10-09). This overrides anything above.

Every finding is accepted except **L2**, which the code contradicts (see its row). The other code checks confirmed the reviewer's evidence. The inline text and contracts in §3–§9 are updated. This list records where each fix went.

| Id | Resolution | Where |
|---|---|---|
| **H1** | Confirmed: `usb_screen_fix_viewports()` (`usb_screen.c:264`) pushes via `viewportmanager_theme_enable(..., true, ...)` (`:143`), and `:308` undoes it. Under `HAVE_IPOD_UI` the call becomes `viewportmanager_theme_enable(i, false, NULL)`, so there's one push and one pop per screen. Guarded statics: `usb_screens_draw`, `handle_usb_events`, `draw_usb_keypad_mode` (+ `usb_screen_fix_viewports` once unreferenced). `ui_usb_screen_loop()` calls `backlight_on()` at entry. Devmode wraps `do_menu()` in one enable/undo pair per screen and never pushes on return (`VPSTACK_DEPTH` 16, `viewport.c:61`) | §3.2, §3.4, §4.2, §4.3, §4.5 |
| **H2** | FACE suspends drawing, not logic. `np_pb_idle()` still runs and its wake is `min_wait`ed with the minute timeout; only `auto_return()` is deferred until dismiss. FACE calls `np_pb_idle()` directly instead of `ui_nowplaying_idle()` (`nowplaying.c:2388`), so P6's file stays untouched. U2 re-checks the `np_*` idle logic after P6 merges | §3.5 |
| **H3** | Confirmed `register_storage_idle_func()` is a oneshot (`ata_idle_notify.c:40`, `add_event_ex(..., true, ...)`). Now a single `add_event_ex(DISK_EVENT_SPINUP, false, ...)` that returns at once when idle; line-by-line writes through a 64 B local with seq numbers, < 128 B of stack; no `usb_inserted()` test (`storage.c:180` skips notifies in USB mode; USB_POWERED must flush); device timing documented; T adds sim `storage_idle` → `call_storage_idle_notifys(true)`; `tests/uilog` covers lines logged during a flush | §3.3, §4.5, §5, §6.4, §6.6, §6.8, §6.10, §6.13 |
| **H4** | `backlight_hold_changed()` also posts `SYS_IPODUI_HOLD_CHANGED` (MISC 7, data 1/0) under the same guard. `UE_HOLD_ON/OFF` come from it. The sim `hold_switch on` drops keys | §3.5, §4.2, §4.4, §5 |
| **M1** | FACE is entered only if `!is_backlight_on(true)`. A lone release dismisses. Swallowing ends on release or `button_status() == 0`, with a 1 s fallback (`UNLIT_SWALLOW_MAX`). Wheel is swallowed for `UNLIT_WHEEL_SWALLOW` (HZ/4). New events `UE_WHEEL`, `UE_BUTTONS_UP`, `UE_TICK`; `ui_unlit_step()` takes `now` and `lit`; a `tests/unlit` row for each case | §3.5, §4.4 |
| **M2** | The event is posted only when `backlight_get_current_timeout() > 0` (public, `backlight.h:41`). `skip_next` carries `skip_stamp` and expires after `UNLIT_SKIP_TICKS` (HZ). `UE_BL_SETTING_OFF` and the `bl-off` skip are removed | §3.5, §4.2, §4.4, §4.8 |
| **M3** | `ipodui_stock_ui_allowed()` is true only while `ipodui_devmode_run()` is on the stack. Repeats with the same `fmt` pointer within 1 s collapse to `repeat=<n>` | §3.4, §4.2, §4.5, §4.8 |
| **M4** | "skip usb" is `ui_usb_screen_active()`, a flag set only inside `ui_usb_screen_loop()` | §3.5, §4.3 |
| **M5** | T is explicitly granted `firmware/target/hosted/sdl/button-sdl.c`. `scripts/shots.sh`, `uisimulator/common/SOURCES` and `tests/ui/README.md` join the orchestrator merge list, and T doesn't wait for P6. The `sim_capture_screen()` prototype (`sim_capture.h`) and its no-op stub are in the contract commit | §5 |
| **M6** | Devmode arms on `top->ops == &ui_about_screen`, an exported stub (`.name "about"`) in `screens/stub.c`, which moves to U2. No title `strcmp`. Orchestrator hook **H3** changes the About row's target in `settings.c:471–472` after P6 merges | §3.4, §4.5, §5 |
| **L1** | Labels: stock `LANG_DEBUG` (`english.lang:6105`), `LANG_BACK` (`:15264`) plus a new `LANG_IPODUI_DEVELOPER` under a new `ipodui_devmode` feature (`apps/features.txt`, gated by `HAVE_IPOD_UI_DEVMODE`). No stock phrase contains "developer", so the release check `strings \| grep -ci developer` = 0 is achievable. This supersedes D3's literals | §3.4, §4.7 |
| **L2** | **Not accepted as written; corrected differently.** `BACKLIGHT_FADE_IN_THREAD` is defined as `CONFIG_BACKLIGHT_FADING & (SW_SETTING \| SW_HW_REG \| PWM)` (`backlight.c:68–71`), so it is non-zero on ipodvideo. `do_backlight_off()` takes the `backlight_setup_fade_down()` branch. The countdown starts immediately only when the fade-out is off; with the default 2000 ms fade-out (`settings_list.c:1541`) it starts in `backlight_isr()` when the fade reaches 0, about 2 s later. My original text named the wrong function (`backlight_switch()`) and the wrong offset (0.5 s), so §6.1 is fixed on both counts | §6.1 |
| **L3** | The compose boost is noted as a deliberate deviation (`usb_storage` boosts anyway). The frame copy is 80 × 160 B. A P10 check times a 1 GB copy with the arrows running vs idle | §3.2, §6.3, §9 |
| **L4** | Stated: the shell's USB loop ignores HID | §3.2, §4.3 |
| **L5** | Confirmed default true (`settings_list.c:2162`). Added to P's inventory; orchestrator hook **H1c** sets it false under the shell | §4.7, §5 |
| **L6** | The drawing contract names the padlock and charging bolt. `scroll_stop()` on devmode return and on FACE enter | §3.4, §3.5, §4.4, §4.5 |
| **L7** | The boot escape uses `button_status() & BUTTON_RIGHT` | §3.4 |

**Contract-commit additions (Rev 1):**
- `SYS_IPODUI_HOLD_CHANGED` (MISC 7);
- the revised `unlit.h`, `usbscreen.h` (`ui_usb_screen_active()`), `devmode.h` (`ui_about_screen`) and `uilog.h` (`ui_log_line`, the persistent flush);
- `uisimulator/common/sim_capture.h` + its stub;
- the `apps/features.txt` `ipodui_devmode` block;
- `LANG_IPODUI_DEVELOPER`;
- the `ui_about_screen` declaration in `screens/screens.h`.

**Orchestrator hooks after P6 merges:** H1a (LCD sleep 90), H1b (`usb_hid` off), H1c (`warnon_erase_dynplaylist` off), H2 (#56 part B), H3 (About row target).

## Revision 2 (fix round after the end review, 2026-10-09)

| Finding | Resolution |
|---|---|
| **M2** Now Playing's stop pop swallowed after a wake | `ui_pop_request()` / `ui_take_pop_request()` (screen_stack); the main loop pops a requester right after its tick, outside the input path. `nowplaying.c` changes one line (P6 merge note). The unlit core never treats a key-less release as a key (`unlit_release_route()`); `tests/unlit` rows; `ipodui-p8a-unlit-npstop.script` |
| L1 USB entered dark | dark wait is `UI_USB_IO_HOLD`, not `TIMEOUT_BLOCK` (P10 item 10) |
| L2 `usb_check_fonts` | tests `pf->disabled` |
| L3 uilog repeat collapse | also compares a hash of the formatted text |
| L4 uilog rotation on the storage stack | deferred to `ui_log_service()` (UI thread, disk spinning) and the power-off flush (P10 item 9) |
| L5 devmode `armed` stuck | disarms when About is not on top |
| L6, L7 | ⚠️ D8 and D9 |
| L8 `paint_table` 1,104 B frame | rows expanded one at a time: 1,136 → 72 B (gcc `-fstack-usage`), pixels identical |
| L9 gate tooling | stock `bootloader.bin` sha256 vs `scripts/stock_boot.sha256` (version string pinned); `fw-rel` (`--ipodui-release`) in the full gate with 0 "developer" strings |
| L10 golden region | devmode scripts compare from row 8 (the title's top row) |
| L11 hygiene | menu.c comment; sim boot keys under `HAVE_IPOD_UI` |
| L12 | sim `yesno_probe`, `ipodui-p8a-splash-yesno.script` |
