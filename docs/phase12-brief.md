# Brief: Phase 12, Extras

> **Closed (2026-10-24).** P12 closing (branch `p12-close`, gate running on `p12`); see `docs/ROADMAP.md` and `docs/p12-close-notes.md`. **Decisions taken by default overnight that the owner should confirm:**
> - The sync-time index build block of **520 KB** (532,480 B, `XIDX_BUILD_BLK`).
> - The SDK resident budget of **15 KB** (≤ 8 KB ARM, ≤ 7 KB Thumb; 14,514 B measured) in place of the 6 KB target.
> - **Games are hidden in resident (non-module) builds**, and the Games row appears only where a game can run.
> - **`J_BUDGET_APP` of 51.5 KB** for the Class B docs (`docs/design/sdk.md` still says 56 KB in places).
> - **Parachute's name and look** may be a Game & Watch lookalike (owner question: keep, rename or redraw?).
>
> **Follow-up:** a paused-window open keeps the tagcache 1 Hz poll running for the whole pause (P19 19-1a L5).


> **DRAFT, revision 2 (2026-10-23, `budget-reviewer` brief review: 1 Critical, 6 High, 12 Medium and 6 Low findings, every one applied; table at the end).** Not approved: the owner pre-flight (§0) comes next. Documents only; no code exists for any item below.

**Branch.** `p12-brief`, and `p12` for the phase, are cut from **`p18-114` (d328f1ef86)**, not `main`. The owner's rule is to merge the next phase's work into the finishing phase. `p18-114` holds P18 (18-1, 18-2, 18-A) and the P17 merges (#117, #118, #121). **Every file:line below is on `p18-114` d328f1ef86** unless it names another branch. P16 is closed and merged to `main`; its module loader (#106) is **off by default**. Milestone **P12 Extras**; issues **#32** (Extras), **#99** (games research, done: `docs/design/games.md` on branch `games-research` eac08ddb9d, not merged), **#82** (custom EQ curves); related **#23** (haptic mod on the piezo line), **#86** (P18 sleep, alarm wake), **#104** (predictive spin-up), **#108** (shrink the audio buffer without a stop).

*Process: `docs/process.md` (brief template, checklist 1–13, lean rules 1–11, one gate per machine). Open owner questions are frozen as ⚠️ with a default (§0.1); the brief is not revised mid-phase for them. Evidence tags as in the P18 brief: [silicon] measured on our unit, [rockbox-derived] read from upstream code, [apple]/[external] from manuals or RE notes, [photo] the owner's 7G photos (`docs/reference/7g/owner/INDEX-a.md`, `INDEX-b.md`; the images are local-only), [hypothesis] unproven, [ours] our design, [est] arithmetic.*

**Goal (owner, "Extras: all of them", 2026-10-07; ROADMAP row 16):** the 7G's Extras menu (Clocks, Calendars, Contacts, Alarms, Games, Notes, Screen Lock, Stopwatch, in that order), with the 7G's screens and behaviour, in our own words and art, at a cost the hardware does not notice:

1. **Alarms are the entrance for every alarm feature** (owner, 2026-10-22): Extras › Alarms holds **Create Alarm** ("New Alarm") and **Sleep Timer**. A new alarm's settings include **repeat, the day(s) of the week and snooze**.
2. **Alarms ring through the piezo**, so the haptic mod vibrates too (owner, 2026-10-19, #32), independent of the Clicker setting. They **wake the unit** from P18's soft sleep, and from power-off when soft sleep is off (§5). **An alarm is never silently lost** (§5.5).
3. **The Sleep Timer is the stock 7G's.**
4. **Extras are cold modules** (#106): almost nothing of an Extra stays resident. Small resident *engines* keep what must outlive a screen: an armed alarm, a running stopwatch, a lock.
5. **Extras have resource classes** (owner, 2026-10-23, §2). P12 ships Class A (lightweight, alongside playback) and the baseline Extras. Classes B and C are defined now, in the module ABI, and built later.
6. **The Extras/Games SDK rev 0** (owner, 2026-10-23, §2A) is designed with the first games, and our games are its first clients.
7. **Custom EQ curves (#82)** are in the ROADMAP row; they come last (wave 5) and may split into their own mini-phase (Q14).

---

## 0. Owner pre-flight

### 0.1 Open questions (frozen ⚠️ with a default until the owner round)

Labels **P12-Q*n***. Earlier answers that stand:
- **EXT-1** (best-effort alarms) is superseded upward by the owner's 2026-10-19/22 direction: real wake, proven by the device tests of §5.
- **EXT-2** (⚠️ default: Beep only).
- **EXT-3** (⚠️ default: the guide's values).
- **EXT-4** (our own games; G-1..G-8 in `games.md` §8 still open).
- **EXT-5** (⚠️ default: the full `.ics` scope).
- **NAV-5** (Contacts full-width).

**Changed or new in revision 2:** Q1, Q2, Q5, Q17 changed; Q27 and Q28 are new; Q26 (raised by the review, M5) was decided by the owner the same day, together with the game input model; Q29 and Q30 are small follow-ups to that decision.

| # | Question | ⚠️ Default | Why it matters |
|---|---|---|---|
| **Q1** *(changed, rev 2; **signed off by the owner 2026-10-23** after D-A2, with the W1-B guardrails)* | **Hold on when an alarm powers the unit on.** Our bootloader starts **Apple's firmware** when Hold is on at power-on (`bootloader/ipod.c` :314 `button_was_held = button_hold()`, :399 [rockbox-derived]). In bedside use (sleep timer, Hold on, then a power-off), the alarm would be lost and the Apple logo would show | **Two parts.** (1) **In P12, gated on D-A2:** a bootloader change (W1-B): before the Hold test, the bootloader reads the PMU's alarm registers and RTC over I2C and **ignores Hold when the RTC is within `ALARM_BOOT_WIN_S` after an armed alarm**. Hold MENU recovery stays as it is. This is a bootloader change and needs the owner's sign-off. (2) **Until (1) is on the unit:** with an alarm armed within 24 h, the shell **never powers off deep**. Hold PLAY, the idle timer and the Sleep Timer end in a **dark wait** instead: screen and LCD off, unboosted, the UI thread asleep until the engine's deadline or a button. Only a forced low-battery shutdown still powers off, with RTCWAK armed. A soft sleep (P18) replaces the dark wait once it is on | The one way a set alarm can silently not ring. The dark wait costs battery, measured as D-A8. 7G ledger row |
| **Q2** *(changed, rev 2)* | Create Alarm rows. **Capture C2 is a blocker for W1-A2's labels and order** (the owner has a stock 7G) | Likely stock rows (to be confirmed by C2): **Alarm** On/Off · **Date** · **Time** · **Repeat** (starting at **Once**, then Every Day, Weekdays, Weekends) · **Alerts** (the sound) · **Label**. **Owner additions, recorded in the 7G ledger:** Repeat › **Custom** (a Sun…Sat checklist) and **Snooze** On/Off. Label is a fixed list (no text entry). **Delete Alarm** on an existing one | Owner: "repeat, days of the week and snooze" |
| **Q3** | Alarm sound | **Beep** through the piezo (= haptic) only. The 7G also offers headphone tones and a playlist; both are wave 5, not P12 (EXT-2 a) | A playlist alarm needs playback to start by itself from a cold boot |
| **Q4** | Snooze length and count | **9 minutes**, fixed (`ALARM_SNOOZE_S`); **at most 3 snoozes** in a row (`ALARM_SNOOZE_MAX`), then the alarm rings until dismissed or the cap (rev 2, Low) | Owner listed snooze as a setting |
| **Q5** *(changed, rev 2)* | Ringing screen (capture C3) and keys | Full screen in the dark system look (USB/Screen Lock style), our own alarm-clock art, the label and the time, and two rows: **Snooze** / **Dismiss** (Snooze only while snoozes are left). **SELECT** acts on the row **at its release**. **PLAY at release** snoozes, or dismisses if no snooze is left. **Hold PLAY** (the long press) dismisses, then sleeps or does the dark wait. **MENU** at release dismisses. It rings **5 min**, then snoozes (if one is left) or stops, and is marked missed (§5.5) | Stock not photographed |
| **Q6** | Music playing when an alarm fires | **Pause** (resume point kept), ring, and **stay paused** after Dismiss | A beep under music is inaudible on the piezo |
| **Q7** | An alarm during a USB data session, with the UI in the USB screen loop | **Piezo only, no screen change; any button stops it; logged.** The PMU is re-armed at once; the `alarms.dat` write is deferred to the post-USB path (§3.3). Owner of the ring inside the USB loop: **W1-A1**, through one call in `usbscreen.c` | The USB screen owns the UI thread; no writes at USB entry (owner rule) |
| **Q8** | Sleep Timer values and display (capture C4; EXT-3) | **Off, 15, 30, 60, 90, 120 Minutes**. The Alarms row's subtitle reads **Inactive**, or the minutes left ("23 Minutes Left", our wording). No header glyph | EXT-3 a |
| **Q9** | What the Sleep Timer does at the end | **Pause, then sleep** (P18 `ui_sleep_request(SLEEP_TIMER)`; with soft sleep off, today's power-off, or Q1's dark wait). On a charger or USB: pause and go dark (upstream's `handle_sleep_timer()`, `powermgmt.c` :1336) | Stock: stops and sleeps |
| **Q10** | Screen Lock: what works while locked, and how it unlocks | Locked: only the combination entry (wheel + SELECT), hold PLAY to sleep, alarms ring, the Hold switch. Unlock by the code, or by **connecting any computer for data** (we cannot know the 7G's "primary computer"). The lock **survives sleep, power-off and reboot** | Stock: "unlocks when connected to the primary computer" |
| **Q11** | Screen Lock and Volume Limit combinations: shared or separate? | **Separate** (two settings) | Stock not documented |
| **Q12** | Where Notes, Contacts and Calendars live on the disk | Visible root folders **`/Notes`**, **`/Contacts`**, **`/Calendars`** (stock disk-mode names). `.txt` notes, ≤ 4 KB shown per note (stock); `.vcf`; `.ics`/`.vcs`. **Decided: /Notes is created if missing, owner 2026-10-23** (W1-N's behaviour as built) | Drag-and-drop mission: the user puts files there |
| **Q13** | Clocks with 2+ cities (capture C1), and the limit | Stacked city bands (blue by day, dark by night ⚠️), the world map under the last; **up to 6 clocks**; Add/Edit/Delete from SELECT's popup, as the guide says | Only one clock photographed |
| **Q14** | Custom EQ (#82) inside P12 or in its own mini-phase | **Its own mini-phase, P12b**, after the baseline Extras, same brief format | It touches Settings › EQ and the DSP, not Extras only |
| **Q15** | Games: names, icons, quiz behaviour (`games.md` §8 G-1..G-8) | `games.md` defaults: Solitaire, Bricks, Parachute (Class A); **Music Quiz deferred** (Q16) | Wave 4 only; nothing earlier waits for it |
| **Q16** | Music Quiz's class | **Class B** (it borrows a playback session and a queue, §2), so it waits for #108 and P19; not in P12 | It is the one game that touches playback |
| **Q17** *(changed, rev 2)* | Missed alarms: the time passed while the unit could not ring (Hold on before W1-B, a flat battery, a refused boot) | **Not rung late. Shown inline in the Alarms list:** the alarm's row subtitle reads "Missed at 7:00" (our wording) until the user opens the list or the alarm next rings. It is a list state, never a splash or a popup. Also logged. Repeating alarms re-arm their next occurrence | "Never silently lose an alarm" (review H6). This changes revision 1's "logged only" |
| **Q18** | A running stopwatch and idle sleep | **Does not keep the unit awake**; time keeps counting through sleep from the RTC | Stock unknown; this is the lean choice |
| **Q19** | Graceful-exit time limit (`SDK_EXIT_MS_MAX`, §2A.7) | **1,000 ms** ceiling; a game asks for less in its header | Long enough for a ~1 KB safefile save on an awake HDD (~25 ms) with margin; short enough that USB or a power-off never feels held up |
| **Q20** | Repeated kills | A game killed **3 times in a row** is hidden until the next firmware or module update (logged, never shown) | A crashing game should not keep costing the user |
| **Q21** | ~~Class A wheel as volume~~ **Decided (owner, 2026-10-23)** | **No volume control in Class A.** The wheel is the game's input only; a Class A game never changes the volume | Decided |
| **Q22** | ~~Class B music controls~~ **Decided (owner, 2026-10-23)** | A Class B game **may offer its own pause/resume** through the playback API (`sdk_music_pause()`/`sdk_music_resume()`; no skip, no volume). It **may have its own Exit item** in its menus, which calls `sdk_exit_request()`: the same SDK exit path (its exit policy, the firmware's full reclaim). The firmware's SELECT + PREV exit still always works | Decided |
| **Q23** | Class C exit: resume music playing, or leave it paused? | **Paused at its spot** (as P14's video exit; the 7G returns from a video with music paused) | Matches Videos |
| **Q24** | Private storage quota per game | **64 KB** default, 1 MB ceiling (declared in the header) | Saves and scores are ~1 KB (`games.md` §4) |
| **Q25** | Third-party games at release (P20) | Only **signed** games from an owner-controlled allow list. A developer setting accepts unsigned ones, with a warning in our words. P12 ships our own games only | A native module can write any RAM (§2A.6) |
| **Q26** | ~~Reserved inputs and the game exit~~ **Decided (owner, 2026-10-23, M5 and the input model)** | **Always the firmware's, every class, never delivered to a game:** Hold PLAY = power off (sleep); Hold SELECT + MENU = reboot (the hardware reset combination, works even when the firmware is hung); **hold SELECT + PREV together for 2 s = exit the game** (the 2 s guards against accidental double presses; a shorter overlap does nothing); the Hold switch locks input. **Class A:** short MENU backs out step by step as in the main UI. **Class B/C:** MENU is the game's, fully and instantly (press, release, hold, repeat); PLAY is tap-only, at release. **Wheel:** the game chooses relative or absolute (§2A.4) | Decided |
| **Q29** *(new, small; **decided by the owner 2026-10-23: keep**)* | Class A: keep a long MENU hold (500 ms) as a **second** way out of a game, besides SELECT + PREV? | **Keep:** it is what people will try first, and short MENU already backs out | It costs Class A games MENU's press edge (MENU acts at release) |
| **Q30** *(new, small)* | Class A and the absolute wheel: Class A games get relative wheel input only (owner). Bricks' paddle (`games.md` §2) wants the finger's position | **Allow a read-only position in Class A** (`sdk_wheel_pos()`: the absolute position and touch, polled, with the event stream left relative and the driver untouched); the absolute *mode* (driver events off) stays B/C | Otherwise Bricks steers by step counts, which feels worse on a ring |
| **Q27** *(new, rev 2, M7; **decided by the owner 2026-10-23: handle DST**, the rule table)* | Daylight saving in Clocks | **A small DST rule table** (none, US, EU, southern hemisphere; one byte per city, ~0.4 KB code): other cities' times include their DST. The home clock follows the RTC as today (Date & Time has no DST row, P11 D4). The alternative is a 7G-ledger row "other cities show standard time" | Without it, a city in DST shows an hour off for half the year |
| **Q28** *(new, rev 2, H1; **decided by the owner 2026-10-23: the default.** The owner expects a non-playing dark wait to be a trivial drain; D-A8 confirms)* | The dark wait of Q1(2): accept up to 24 h of dark-awake drain when an alarm is armed and P18's soft sleep is off? | **Yes**, until W1-B (the bootloader change) is on the unit or soft sleep is on by default. D-A8 measures the cost. If D-A8 shows more than 10 % of the cell over 24 h, the owner decides between the drain and Q1's Hold limit | Bedside reliability against battery |

### 0.2 Captures and owner sessions
Read-only on the stock 7G (#26 rules):
- **C1 (Q13):** Extras › Clocks with two or three clocks; the SELECT popup (Add/Edit/Delete); a clock at night.
- **C2 (Q2), a blocker for W1-A2's labels:** Extras › Alarms › Create Alarm, every row and its value list (back out without saving).
- **C3 (Q5):** an alarm ringing (video, with sound), its keys, how long it rings.
- **C4 (Q8):** Alarms › Sleep Timer values, and the Alarms row while a timer runs.
- **C5:** Stopwatch running (laps), stopped (Resume / New Timer), a log's detail.
- **C6:** Screen Lock locked, and the wrong-code response.
- **C7 (NAV-5):** Extras › Contacts and one contact.
- **C8 (EXT-5):** Calendars on a day with an event, and To Do's.
- **C9:** the Extras list's right panel for Contacts, Games, Notes and Screen Lock. Only Clocks, Calendars, Alarms and Stopwatch are photographed.
- **C10 (Q27):** Clocks with a city in DST, if one is set.

Device sessions on the 5.5G (owner's explicit go and a fresh backup, rule 5): **D-A** and **D-M** in §5.4.

---

## 1. What already exists (read it; reuse it, do not rebuild)

| Area | Where | Reuse |
|---|---|---|
| **Extras row** | `menu.c` :130, :192: the main menu's Extras row is visible and pushes `ui_stub_screen`. Its panel `ui_preview_extras` lives in **`screens/panels.c` :832–1031** (our world map, a 3-D clock, the date, drawn live once a minute; `changes-from-7g.md` 3.14) | The row pushes the Extras list instead; the panel is also the Clocks item's panel and the reveal backdrop |
| **Main-menu Extras items** | `menucfg.h` :41–42: `UI_MM_ALARMS`, `UI_MM_CALENDARS`, `UI_MM_CLOCKS`, `UI_MM_CONTACTS`, `UI_MM_GAMES`, `UI_MM_NOTES`, `UI_MM_SCREENLOCK`, `UI_MM_STOPWATCH` already exist in the Main Menu checklist. Alarms takes the fixed slot after Extras [photo 1189] | Each pushes its Extra's screen directly |
| **Reveal transition** | `screen_stack.h` `UI_T_REVEAL` is a **stub** that runs as a push. The motion is measured (spec `index.md` Transitions item 4, `VIDEO-2.md` §A3: a fixed map backdrop, everything else pushed left, 250 ms) | W1-C writes the motion; W2-W wires it into `ui_main.c` |
| **Module loader** | `modload.{c,h}`, `modload_core.{c,h}`, `tools/modpak.py`, `apps/ipodui/modules/`, host test `tests/modload` (`docs/design/module-loader.md`, `hardware.md` "Shell modules"). Thumb modules linked `--just-symbols`, no `.data`, `.bss` zeroed at every load and move. One arena counted in the floor (`MOD_ARENA_SIZE` 8 KB release / 14 KB developer). Stand-in screens, the styled "Not Available" page, snapshots (`snap.pak`), intent preload. `MODPAK_MAX` 8. The pack index is read by `job_open()` on the storage thread into **a `struct modpak_hdr` on that thread's stack** (`modload.c` :133) and then published to the static `hdr` (`modload.c` :109, 288 B). `slots[MOD__N]` holds ~56 B per module (:64–74). `modpak.py keep` already lists every core symbol a module uses. **Device checks still open** (`module-loader.md` :97) | The Class A framework is this loader plus class labels and checks (§3.1). **P12 turns it on by default, after its device checks pass (H5, §5.4 D-M)** |
| **Combination roller** | `roller.{c,h}` (dark 4-digit roller, `dark = true`); `vlimit.c` (Volume Limit's combination, `ipodui_vlimit_code`, recovered only via `config.cfg`) | Screen Lock's New/Confirm Combination and the locked screen |
| **Date & Time, cities** | `screens/datetime.c` holds the region → city picker, **inside the Settings module** (a tenant of `MOD_SETTINGS`). `tzcities.h` (97 cities × 6 B + 8 regions, standard offsets, no DST, P11 D4) and `tzcity.h`. The RTC holds **local** time; a zone change shifts it (`datetime.c` :38, :178) | **Modules cannot link to each other**, so W1-C moves the picker and the table into a resident object, `tzpick.{c,h}`, shared by Date & Time and Clocks (§10 counts it). Other cities: utc = home RTC − home standard offset − home DST; other = utc + their standard offset + their DST (Q27; `tzpick.h`). (W0 fix: revision 2's "local + (their offset − home offset) + DST" left out the home city's own DST, an hour off whenever the home city observes it, since the RTC then holds DST time. The EU rule switches at a UTC instant, so the DST function takes the city's offset too.) |
| **Calendar page and digits** | `caldigits.h`, the Date & Time panel's calendar page (`Oct`/`06`), `material.c` (gloss, metal tiles) | Calendars' panel and month grid; Stopwatch's digits |
| **Piezo** | `firmware/target/arm/ipod/piezo.{c,h}`: `piezo_play()`, `piezo_play_for_tick()`, `piezo_play_for_usec()`, `piezo_stop()`, `piezo_busy()`, a `PRIORITY_REALTIME` piezo thread. `click.c` (Clicker: Off/On = headphone click + piezo, #23) [rockbox-derived; the haptic follows the piezo line, owner 2026-10-06, unverified on the device] | The alarm's beep pattern, independent of Clicker |
| **RTC alarm** | `HAVE_RTC_ALARM` (`ipodvideo.h` :97). `rtc_pcf50605.c`: `rtc_init()` calls `rtc_check_alarm_started(false)` (:33–36; called from `apps/main.c` :660). `rtc_set_alarm(h, m)` writes sec/min/hour (registers `0x11`–`0x13`). `rtc_enable_alarm(true)` writes the "ignore" values to `0x14`–`0x17`, unmasks `INT1M` bit 7 and sets `pcf50605_wakeup_flags \|= RTCWAK`. `rtc_enable_alarm(false)` writes year `0x17` = `0x99` and clears RTCWAK. `rtc_check_alarm_started()` compares the alarm and the clock within 10 s at its first call, because the boot ROM clears the PMU's interrupt flags [rockbox-derived]. `pcf50605_standby_mode()` writes `OOCC1 = GOSTDBY \| CHGWAK \| EXTONWAK \| wakeup_flags` (`pcf50605.c` :73); `pcf50605_wakeup_flags` starts at 0 at every boot (:36) | The deep (power-off) path of §5. **None of it is proven on our unit** (`clocks-power.md` §9 last row: "the deep test with an alarm (P12)") |
| **The power thread's alarm disarm** | **`powermgmt.c` :541–546 `power_thread_rtc_process()`:** every power-thread step, if the PMU's INT1 alarm flag is set, it calls `rtc_enable_alarm(false)`, which writes year `0x99` and clears RTCWAK [rockbox-derived] | **Races `alarm_rearm()`** (review H2). Under `HAVE_IPOD_UI` it is changed to clear the flag only (§3.3, W1-A1) |
| **Upstream alarm UI** | `apps/alarm_menu.c` (it also calls `rtc_init()`, :66), `root_menu.c` :894–921 (the alarm-started boot), `time_menu.c`: Rockbox's wake-up alarm screens | **Not used** (Rockbox UI). Only the firmware calls above are reused; `root_menu.c`'s path is not reached under the shell (checklist 12) |
| **Sleep timer engine** | `powermgmt.c` :1296–1354: `set_sleeptimer_duration()`, `get_sleep_timer()`, `handle_sleep_timer()` (charger/USB → `audio_pause()` + `backlight_off()`, else `sys_poweroff()`). `sleeptimer_endtick` is in **ticks**, so it **freezes in a soft sleep** (P18: `current_tick` does not advance). `settings.c` :980 starts the timer at boot when `sleeptimer_on_startup` is set | The engine as is. W2-T routes its end to P18's sleep under `HAVE_IPOD_UI`; at sleep entry the timer is cancelled (a sleep ends what the timer was for) (§5.6) |
| **P18 sleep hooks** | `firmware/export/power_sleep.h` `struct sleep_req.alarm_rtc` (seconds since 2000, 0 = none) and `WAKE_ALARM`; the poll is programmed for `min(poll, alarm − now, …)` (P18 brief §3.1, M5). `apps/ipodui/sleep.c` :546 passes `rq.alarm_rtc` (always 0 today); :718 "WAKE_ALARM: P12's alarm event goes here". `enum ui_sleep_reason` has `SLEEP_TIMER`; `ui_sleep_rtc_now()`; `SLEEP_EVENT_PREPARE`/`SLEEP_EVENT_WAKE`. Soft sleep is **off by default** (`SOFT_SLEEP_MARKER`) until D2 passes. Sim: `sleep_warp`, `rtc_set` (`uisimulator/common/sim_cmd_sleep.c` :433–523) | W2-W fills `alarm_rtc` and handles `WAKE_ALARM`; the sim commands are reused |
| **Sim clock and USB** | `apps/ipodui/simclock.c` (the pinned scripted clock); `usb_connect`/`usb_disconnect` (`uisimulator/common/sim_cmd_art.c` :323–328) | Reused by every P12 script; no new clock command |
| **Boot battery guard** | `bootbatt.c` `bootbatt_enforce()` runs after `lcd_init()`, before `storage_init()`; a refusal calls `power_off()` (`hardware.md` "Boot battery guard") | A refused boot keeps a valid armed alarm's RTCWAK (§5.5) |
| **Safe writes** | `safefile.h` (#91 replace primitive, remove, intent records) | Every Extras file (`alarms.dat`, `stopwatch.dat`, indexes) |
| **Post-USB moment and boot window** | P16 #84 (busy glyph after USB), #87/#89 (boot window), the boot stamp (`bootstamp.h`), the snapshot batch (`module-loader.md`) | Notes/Contacts/Calendars indexes are built here (sync time), never at browse time. The boot stamp lets a trusted clean boot skip the index signature check |
| **Maintenance takes** | `maint.h`: `maint_take(job, bytes)`, `maint_take_fits()`, tenants `MAINT_COMMIT…MAINT_LISTS`; `audio_buffer_regrow()` (`playback.h` :96). **Paused/stopped only** | The indexer's build block (a take while stopped, M3). Class B's borrow is **not** built on it (M4: it needs #108's shrink without a stop) |
| **Video handover** | `screens/video.c` :68, :297, :578–585, :620, :713: `audio_stop()` with the resume point kept, a refused block regrown, the styled error page on failure (memory: a video failure → the error screen) | Class C's contract is this one, generalised |
| **Games design** | `docs/design/games.md` (branch `games-research`): one module per game, image ≤ 32 KB + working set ≤ 24 KB, no threads, `games.dat` via safefile, owner questions G-1..G-8 | Wave 4; merged into `p12` with the contract commit |

## 2. Extras resource classes (owner direction, 2026-10-23)

Owner: *"some games are really low resource usage, and can probably run trivially while music plays. Other games may ultimately require more resources, and require the full music buffer's worth of RAM, or dedicated core/video work. … the baseline extras should be very lightweight and tend to be easy to jump into and out of while music playback occurs … Some Games may end up effectively being in the same situation as the video playback."*

Every module carries one class. The class is a **contract**: what the module may use, what happens to playback, how fast it must enter and leave, and how a breach is caught.

**P12 ships the Class A framework and the baseline Extras. Classes B and C are defined now and not built.** §3.1's ABI carries them, so it does not change later. B lands with P19 (hardware offload) and #108 (shrink the audio buffer without a stop); C with Videos (P14) and Cover Flow (P7).

### 2.1 The three contracts

| | **Class A, lightweight** | **Class B, heavier, alongside music** | **Class C, exclusive (like video)** |
|---|---|---|---|
| **Who** | Every baseline Extra: Clocks, Alarms + Sleep Timer, Stopwatch, Screen Lock, Notes, Contacts, Calendars; the sync-time indexers; simple games (Solitaire, Bricks, Parachute) | Music Quiz (Q16); later richer games; possibly Cover Flow if its textures are borrowed rather than permanent (§2.4) | Videos (P14, already this contract); a game that needs the whole RAM, a dedicated core or the video chip (a Vortex-like game, `games.md` G-7) |
| **RAM** | **Only the module arena**: its image + `.bss` + one working block from `mod_work_alloc()` (§3.1), all inside `MOD_ARENA_SIZE`, which is reserved at shell start and **counted in the audio-buffer floor**. No `core_alloc*`, no buflib, no `lib_open()` view pool, no tagcache search that grows the RAM cache, no font or skin load (checklist 2's indirect allocations) | **A borrow** from the audio buffer **through #108's shrink-without-stop** (`mod_borrow()`, §3.1), only while the buffer stays ≥ floor + the borrow, returned on exit. Refused → the module runs its reduced mode or shows the styled "Not Available" page; never a stop of playback. (Not `maint_take()`: that mechanism runs only while paused or stopped, M4) | **The audio buffer itself**, after the handover: playback paused (resume point kept) or stopped as Videos does, the buffer lent whole. May hold `cpu_boost`, the COP or the BCM for the session |
| **Cores** | The UI thread only (the **front end**: today the CPU's UI thread, the COP after P19's split, memory `front-end-back-end-cores`). No threads, tick tasks, timers, COP jobs or BCM commands of its own | Front end, plus **COP jobs** and **BCM commands** through P19's job API when it exists | Whatever its session declares (a dedicated core, the BCM), released at exit |
| **Storage** | **Entry only:** at most 2 reads per entry (its image, preloaded on highlight; one prepared blob such as `notes.idx`, or one note file), each one range read (lean rule 11). **Zero reads in steady state while lit.** Writes only at exit, pause or stop, through safefile, queued to the next natural disk wake where the data allows (alarms: at once, they must survive a power cut). Never a directory walk (indexes come from sync time) | Bulk reads at entry sized to the borrow (no streaming while music plays); the same write rule | Its own streaming (as the video player), with its session's drive policy |
| **Boost** | Unboosted steady state at ≤ 30 fps (frames dropped, not queued). Bursts ≤ 100 ms (the transition's first frame, a deal, a parse step), **≤ 10 % boosted time over any 10 s**, one release point per burst | A declared budget in its manifest (e.g. "boosted while a round runs"), balanced on every exit path | Session-long boost allowed (as `dev39` video), released when paused, dark, failed or ended |
| **Playback** | Untouched: it keeps playing. **Shell Extras** (Clocks, Stopwatch, …) keep the shell's key meanings, including the wheel as volume **only on screens that do not use the wheel**, as Now Playing does. **SDK Class A games never change the volume** (the wheel is the game's; owner, Q21). Only exception for both: a ringing alarm pauses (Q6), through the normal pause | Keeps playing at the shrunken buffer; the buffer refills normally | Paused or stopped at entry, **restored paused at the same spot** at exit. A failure stops audio and shows the styled error screen (the video-failure rule) |
| **Latency** | **In ≤ 250 ms** to a live screen (inside the push animation; a snapshot covers a cold HDD); **out instantly** (a pop slide; nothing waits on a write) | In ≤ 0.5 s behind a snapshot; out ≤ 0.3 s, the regrow in the background | In ≤ 1 s behind a snapshot (the handover); out ≤ 1 s to a paused queue Now Playing can resume |
| **Built** | **P12** | Defined in P12; built with #108 and P19 | Defined in P12; Videos already conforms; built for games with P19/P14 |

**Paged lists (W3-K, the one declared exception to "zero reads while lit").** A Class A list too long for its working block (Notes, Contacts; built at sync time as a paged index, `xindex_core.h` "Paged lists") may read while browsing, under these rules:
- The manifest declares the rate: `pages = <page loads a second>` (1..30, Class A only; `mod_desc.pages`, which `modpak.py check` compares). Notes and Contacts declare 8.
- A page load is one open and one range read of one self-contained page (rows, their offsets and labels; ~0.5 to 1 KB), bracketed by `modclass_page(true/false)`. The page directory sits in the index's head, which entry reads in its one read together with the first page(s).
- The accountant books page loads apart from reads (`pages=` on its line) and logs `modclass viol <name> page` for a load of more than 2 storage commands, `pages` for more loads in a second than declared. A module that declares no rate books its page loads as reads (the entry rule applies).
- Nothing else changes: entry is still at most 2 commands, a card or a note is an entry of its own, and Calendars needs no paging (it reads records only when a day or an event opens).

**The fps rule** is the frame clock's ceiling, not a round 30: `UI_ANIM_FRAME_TICKS` is 3 ticks (33.3 frames a second at HZ 100), and a 2 s period whose ends straddle a frame reads one more, so the accountant flags more than `ceil(HZ / UI_ANIM_FRAME_TICKS)` = 34 pushes a second.

### 2.2 How a module is labelled
- **Manifest:** each module has `apps/ipodui/modules/<name>.mod` (key = value lines): `class = A|B|C`, `min_ram = 32|64`, `kind = shell|sdk`, `abi = 0.1` (SDK modules), `exit = graceful <ms>|immediate`, `work = <bytes>` (its working block, Class A), `quota = <KB>`, `borrow = <bytes>` (Class B), `boost = burst|session`, `exports = …`. `modpak.py` reads it.
- **In the pack:** `struct modpak_ent` gains the class, the minimum RAM variant, the ABI version, the kind, the exit policy, `work_kb` and `borrow_kb` (§3.1, `MODPAK_VERSION` 2). The loader reads them **before** it reads the image.
  - A module this unit cannot run (`mod_runnable()` false: a wrong ABI, a 64 MB module on a 32 MB unit, a class this build does not support) is **hidden from every menu**, never shown as an error (native illusion).
  - A Class C module is never loaded outside its handover, and a Class B module never without its borrow.
- **In the image:** export slot 0 of every module is its **module header**, `struct mod_desc`: magic, ABI version, class, minimum RAM variant, kind, exit policy and time, work, quota, borrow, name. The loader compares it with the pack entry; a mismatch is `MODPAK_EHDR` (logged, the module treated as absent).
- **Class A is always `min_ram = 32`.** It lives in the floor-counted arena, the same on both variants; `modpak.py` refuses 64. Only Class B and C modules may declare 64 MB (a borrow or a whole-buffer session that only a 64 MB unit's headroom can give).
- **In the simulator** (modules link statically): `mod_desc` is still slot 0, and the screen registry (`extras.h`) names each screen's module, so the sim accountant (§2.3) knows the class of the top screen.

### 2.3 How a breach is caught
1. **Link-time symbol rules** (target build, `modpak.py check`, run by `validate.sh` whenever modules are on). `modpak.py keep` already lists every core symbol each module uses.
   - **SDK modules (games) get an allow list, not a deny list:** they link only against the SDK's symbol table (§2A.6), so any other undefined symbol fails the link.
   - **Shell modules** (our cold Extras screens, linked `--just-symbols` against the core) get a per-class **deny list** (`apps/ipodui/modules/class_a.deny`, `class_b.deny`), and a hit refuses the build. Class A may not reference: `core_alloc*`, `buflib_*`, `create_thread`, `tick_add_task`, `timeout_register`, `cop_job*`, `audio_stop`, `audio_buffer_*`, the `playlist_*` writers, `tagcache_search*`, `lib_open`, `font_load*`, `opendir`/`readdir`, `storage_*`, `bcm_*`, `cpu_boost`. Bursts go through `ui_boost_burst()`, which caps and logs them.
   - Class B adds `audio_stop`/`opendir` to its deny list but allows `mod_borrow` and the P19 job API.
   - Class C has no deny list; its handover function is the only way in.
   - The indexer module `MOD_XINDEX` is the one shell module allowed `opendir`/`readdir` and the file calls. It runs only off-screen, in the sync windows (§3.5), and its manifest says so (`context = sync`).
2. **Size rules** (`modpak.py check`): Class A image ≤ 32 KB, and image + `.bss` + `work` ≤ 56 KB (`games.md`'s largest game). Any one module ≤ `MOD_ARENA_SIZE` − 2 KB (the index staging, §3.1). The lists that stay held under a child are resident (§3.2), so the "side-by-side" set is one module plus the evictable rest.
3. **Runtime accountant** (sim and developer target builds, `modclass.c`, ≤ 1 KB). While a Class A module's screen is on top, it counts:
   - storage reads and writes (the sim's file layer and the target's storage counters);
   - `core_alloc` calls (a buflib hook in developer builds);
   - boosted ticks (the `CPU_BOOST_LOGGING` lines in the sim, `get_cpu_boost_counter()` on the target);
   - pushes per second.

   It logs `modclass <name> class=A reads=%u writes=%u allocs=%u boost_pct=%u fps=%u`, and `modclass viol <name> <rule>` on a breach. **Trace rule:** every `ipodui-p12-*` script that opens an Extra must log at least one `modclass` line (a rule that finds none fails) and no `modclass viol` line.
4. **Floor gate:** `tests/perf/floor_gate.sh` gains the case "arena at its P12 size, its largest tenant held, while playing". It replaces the 64 KB placeholder with the real `MOD_ARENA_SIZE` from `modpak.py`.
5. **Playback check:** each Extra's `-while-playing` script (§9) asserts that the position advanced (`wait_pos`) and 0 underruns across entry, use and exit.

### 2.4 How this connects to P19 and Cover Flow
- **P19's front end/back end split** (memory `front-end-back-end-cores`): Class A is, by definition, front-end code. The Class A **allow list** is what remains after the deny list: drawing, input, the module arena, the Extras engines of §3, and the playback *control* posts that already go through queues. That is exactly the API the front end must keep when the UI moves to the COP. Writing the deny list now turns P19's "what does the UI call that touches the back end?" audit into a check that already runs.
- **Class B is P19's customer:** COP jobs and BCM composition are what make a heavier game possible alongside music. The manifest's `boost`/`borrow` fields are where P19's per-module job budget will go.
- **Cover Flow's textures (P7):** `module-loader.md` plans to grow the one arena to ~512 KB as Cover Flow's texture home, permanently counted in the floor (also `hardware.md` :119, "Cover Flow texture cache ≤ 512 KB").
  - **This brief recommends instead** (a decision for P7's brief; P12 does not prejudge it): keep the **Class A arena small** (§10: 40 KB, then 72 KB) and give Cover Flow its textures either as a **Class B borrow** (taken at entry, returned at exit) or in **BCM SDRAM** (P19, #115). Either way the always-reserved floor cost stays ~72 KB instead of ~512 KB, and a game started from Extras never has to evict textures.
  - Forward notes pointing here are added to `module-loader.md` "Memory" and to `hardware.md` :119 in this revision.

## 2A. Extras/Games SDK (rev 0)

Owner, 2026-10-23: *"we should design the SDK when we design the games we're adding back in … the agents can use it as reference."*

**P12 delivers `docs/design/sdk.md` (rev 0)**, written before wave 4's first game (package **W4-0**, opus, design only). **Our own games are its first clients**, built against it exactly as a third party would. This section is that document's outline and its binding decisions; `sdk.md` expands it. Nothing here is implemented before W4-0 is reviewed. At release (P20) the same document becomes the public "ProkBox game SDK docs" (ROADMAP row 23).

### 2A.1 Module header
The `struct mod_desc` of §3.1, export slot 0, carries:
- the **resource class (A/B/C)**;
- the **minimum RAM variant**: `MOD_RAM_32` runs on 32 and 64 MB units, `MOD_RAM_64` on 64 MB only;
- the **ABI version**, `SDK_ABI_MAJOR`.`SDK_ABI_MINOR` (rev 0 = 0.1);
- the kind (`sdk`);
- the **exit policy**: graceful with its asked-for time, or immediate;
- the working block, the private storage quota, the borrow and the boost mode;
- its own name table.

A module the unit cannot run is **hidden from the Games list**, with no splash and no error page; the reason is logged (`mod hide … why=`). **Our games' names come from `.lang`-style sources** (`apps/ipodui/sdk/names/*.lang`, rule 8), compiled by `modpak.py` into each module's name table, so they are localisable like every shell string. Third-party games ship their own tables.

### 2A.2 Lifecycle
```c
/* Export slot 1 of an SDK module: its entry points. Every call is made by
 * the SDK on the UI thread and must return within its budget (§2A.7). */
struct sdk_game {
    int  (*init)(struct sdk_ctx *c);           /* allocate (sdk_work), load
                                                  state (sdk_file_*), draw the
                                                  first frame; <0 = refuse */
    int  (*step)(struct sdk_ctx *c, const struct sdk_input *in,
                 uint32_t now_ms);             /* one frame: input, update,
                                                  draw dirty bands; returns
                                                  ms until the next step, or
                                                  SDK_IDLE (input only) */
    bool (*back)(struct sdk_ctx *c);           /* Class A short MENU: go back
                                                  one level inside the game;
                                                  false = at its top level,
                                                  the shell exits the game */
    void (*suspend)(struct sdk_ctx *c, enum sdk_why why); /* Hold, alarm,
                                                  sleep, charger/USB screen:
                                                  stop motion, keep state */
    void (*resume)(struct sdk_ctx *c);         /* redraw in full */
    void (*exit)(struct sdk_ctx *c, enum sdk_why why);    /* graceful
                                                  policy only: save within
                                                  exit_ms, then return */
};
```
- **The SDK owns the loop.** A game never loops on its own and never blocks. The shell calls `step()` at the time it asked for (≤ 30 fps, frames dropped not queued), so the UI thread keeps returning to the scheduler, and the playback threads never starve. Rockbox's scheduler is cooperative [rockbox-derived].
- **A game may end itself** with `sdk_exit_request()` (for example its own Exit item; owner, 2026-10-23, for Class B). The SDK runs the same exit path as the SELECT + PREV exit, after `step()` returns.
- **States:** loaded → `init` → running (`step`/`back`) ⇄ suspended (`suspend`/`resume`) → exiting (`exit`, graceful) → reclaimed.
  - A soft sleep (P18) suspends; the game stays in RAM and resumes at wake.
  - A power-off, a USB data session or a low battery exits.

### 2A.3 API surface (rev 0)
| Group | Calls (rev 0, names indicative) | Rules |
|---|---|---|
| **Drawing** | `sdk_lcd_*` wrappers of the shell's draw path (fill, gradient rows from a precomputed table, `bitmap_part`, text in ShellSans, the material kit's tiles) and `sdk_band_dirty(y, h)`. The SDK pushes the dirty **full-width bands** once per step through `ui_lcd_push` | No `lcd_update`; no full-screen push except a scene change; ≤ 25 KB per frame average for Class A (`games.md` §2) |
| **Input** | `struct sdk_input`: the buttons the class owns (§2A.4) as press/release/repeat/long events, plus `SDK_IN_CANCEL` (a button already delivered became part of the exit chord); the wheel in the mode the game chose: **relative** (`steps`, `velocity`, as the shell's `scroll.c`) or **absolute** (`pos` 0–95, `touched`; B/C only); `sdk_wheel_mode(SDK_WHEEL_REL \| SDK_WHEEL_ABS)` at `init` or any time; `sdk_wheel_pos()` read-only in every class (Q30) | Reserved inputs never arrive (§2A.4) |
| **Timers** | the `step()` return value only; `sdk_now_ms()` (RTC + tick, survives a sleep) | No `tick_add_task`, no timeouts, no threads |
| **Feedback** | `sdk_click()` (the Clicker path, rate-limited 20/s) | Never the alarm pattern |
| **RAM** | `sdk_work(bytes)`: the one working block (Class A, from the arena, ≤ `work_kb`). Class B: `sdk_borrow(bytes)` (≤ `borrow_kb`, may be refused). Class C: the session buffer from the handover | Nothing else allocates; all of it is reclaimed at exit |
| **Storage** | `sdk_file_open(name, mode)`, `read`, `write`, `close`, `remove`, `list`, confined to the game's private directory (§2A.5) | Class A: read at `init`, write at `suspend`/`exit` only. Class B: bulk reads at `init`. Class C: per session |
| **Audio coexistence** | Class A: none (music plays, controlled by the firmware). Class B: `sdk_music_pause()`, `sdk_music_resume()` only (owner, 2026-10-23). Class C: `sdk_audio_*` (its own PCM through the SDK mixer channel; music is paused) | No playback, playlist, codec or buffering calls |
| **Library view** (read-only, optional, Class B) | `sdk_lib_pick(n, filter)` (random track ids from the precompute); `sdk_lib_meta(id, field)` (title/artist/album strings copied out); `sdk_art_thumb(id, size)` (from the art cache); `sdk_preview_play(id, start, ms)` (a borrowed preview session; statistics and resume points untouched) | **No file handles to music.** The library never changes: play counts, ratings, resume points and the queue are restored byte-identical |
| **Strings** | the game's own name/string tables per language | No LANG ids for third-party code |

### 2A.4 Firmware authority: input ownership by class
**Decided by the owner, 2026-10-23** (it replaces revision 1's long-MENU exit for games):
- **Class A** games keep the music controls in the firmware's hands, and own only SELECT, the wheel and their own UI. A **short MENU backs out step by step**, like the main UI.
- **Class B** games take every button except the firmware's reserved inputs below. **MENU is fully the game's and instant** (press, release, hold, repeat; no release delay). **PLAY is tap-only, at release** (its hold is power off).
- **Class C** is B plus the large resources, with no music alongside.

**Always the firmware's, in every class, never delivered to a game:**
1. **Hold PLAY** = power off (sleep), as in the shell. `LONG_PRESS_TICKS` = HZ/2 = 500 ms (`input.c` :45), so PLAY reaches a game only as a tap at its release (`input.c` :244–255 marks a long press's release so its short action is skipped).
2. **Hold SELECT + MENU** = reboot. This is the iPod's **hardware reset combination**: the hardware handles it, not software, so it works even when the firmware or a game is hung. Listed so game designers never assign it.
3. **Hold SELECT + PREV together for 2 s = exit the game** (`SDK_EXIT_CHORD_MS` = 2,000). The 2 s guards against accidental double presses. **The chord never reaches the game:** the moment both are down, the SDK withholds both buttons and, if one of them was already delivered, sends the game `SDK_IN_CANCEL` for it (the game drops that press's action). Released before 2 s: **nothing happens**, no event of either button reaches the game. Held 2 s: the game's **exit policy** runs (graceful `exit()` with its deadline, or immediate), then reclaim and the Games screen.
4. **The Hold switch** locks input: everything is blocked and the game is `suspend`ed.

**Class A only:** a **long MENU hold** (500 ms) is kept as a second way out (Q29, default keep), so MENU acts at its release in Class A, as in the shell.

**Wheel modes** (the game chooses at `init`, and may switch through the API):
- **Relative:** step clicks with velocity and acceleration, exactly as the shell's lists (`scroll.c`). Every class.
- **Absolute:** the finger's position on the ring and touch/untouch, for paddles and dials. **Class B/C** (Class A gets a read-only `sdk_wheel_pos()`, Q30).
- **What the driver gives** (`firmware/target/arm/ipod/button-clickwheel.c`):
  - [rockbox-derived] each valid packet carries the position in bits 16–22 (0–95, "Highest wheel = 0x5F, clockwise increases", :165–166), and bit 30 = finger on the wheel (:161);
  - [rockbox-derived] `HAVE_WHEEL_POSITION` is defined for this target (`ipodvideo.h` :271), and `wheel_status()` (:332) returns the last position, or −1 when the last packet had no touch bit (`whl = -1`, :140; stored at :326), so −1 doubles as "untouched";
  - [rockbox-derived] `wheel_send_events(false)` (:337) stops the driver's scroll events, which absolute mode uses so the relative stream does not also fire;
  - [rockbox-derived] the relative path's untouch is debounced (150 ms, `WHEEL_UNTOUCH_TIMEOUT` :57), but `wheel_position` is not;
  - [device check, **D-W1**] whether the controller sends a packet when the finger lifts (if not, `wheel_status()` keeps the stale position, and the SDK infers untouch from 150 ms without packets); the packet rate while the finger moves and while it rests; position jitter at rest; and P18's `clickwheel_sleep_exit()` resetting `wheel_position` to −1 after a wake.

| Input / event | Class A | Class B | Class C |
|---|---|---|---|
| **SELECT** (press, release, long) | game | game | game |
| **Wheel** | game, **relative** (never volume, Q21); read-only position (Q30) | game, **relative or absolute** (the game chooses) | as B |
| **PLAY/PAUSE** | **firmware** (play/pause music at release, as in the shell) | game, **tap only, at release** | as B |
| **NEXT / PREV** | **firmware** (skip/back at release, seek on hold, as in the shell) | game (press edges) | as B |
| **MENU** | **short: firmware, delegated:** the shell calls `back()`. While it returns true the game handled it; when it returns false, the shell exits the game with the normal pop slide (MENU feels exactly as in the shell). **Long (500 ms): exit** (Q29) | **game, fully and instantly** (press, release, hold, repeat) | as B |
| **SELECT + PREV held 2 s** | **firmware: exit the game** (its exit policy, then the Games screen); a shorter overlap does nothing | same | same |
| **The game's own Exit item** (optional) | – | game calls `sdk_exit_request()`: the same exit path as the chord | as B |
| **Hold PLAY** | **firmware, always:** `suspend`, then sleep (P18) or the dark wait (Q1); before a power-off, `exit` first | **firmware, always:** the exit path, then sleep or power-off | as B |
| **Hold SELECT + MENU** | **hardware reset, always**: never input to anyone; works even if the firmware is hung | same | same |
| **Hold switch** | **firmware, always:** blocks all input and `suspend`s the game (the unlit face, `changes-from-7g` 2.25); Hold off → `resume` | same | same |
| **Volume** | **none: a Class A game never changes the volume** (owner) | none; the game may expose its own | none (its own audio, its own volume UI via the SDK) |
| **Music control** | the firmware's buttons keep it controllable | the user cannot control music from the buttons; the game may offer its own pause and resume through `sdk_music_pause()`/`resume()` (owner) | music **paused at entry and left paused at exit** (the 7G's video behaviour as P14 builds it; Q23) |
| **Alarm ring** (P12) | firmware: `suspend`, ring screen on top, `resume` after Snooze/Dismiss | same | same (its session audio is paused) |
| **USB host** | firmware: `exit` (graceful deadline), then the USB screen | same | same |
| **Charger in/out** | firmware: no effect in play (the header glyph changes); unplugging while asleep follows P18 Q9 | same | same |
| **Low battery** | firmware: `exit`, then the low-battery path | same | same |
| **Sleep Timer end** | firmware: `suspend`, then sleep | same | same |

Rule: **a reserved input never reaches the game**, not even as a release. The input layer swallows it before `step()`.

### 2A.5 Sandbox: what a game can see and change
Owner, 2026-10-23: games must not be able to touch the data ProkBox depends on, and **the library never changes**.
- **Files are capabilities.** A game sees only **its private directory, `/.rockbox/gamedata/<module-id>/`** (named here and in `sdk.md`; never on screen).
  - `sdk_file_open()` takes a **bare name** (`[A-Za-z0-9_.-]{1,31}`, no `/`, no `..`, no leading dot). The SDK builds the path itself, so there is no absolute path, no traversal and no symlink to follow (FAT has none; the check is still made).
  - Each open file is a handle in the SDK's table, closed at exit.
- **Quota:** `quota_kb` is declared in the header (default 64 KB, ceiling 1 MB, Q24); the SDK refuses a write past it. Writes go through safefile's replace (no torn saves), and are refused at USB entry and below disk-safe.
- **No raw access** to the database, the library files, music, art caches, playlists, settings, the resume and statistics files, or any ProkBox state file. None of their APIs is in the SDK table, and the file API cannot name them.
- **Read-only views** only through §2A.3's library calls, which copy strings and pixels out. Previews play through a borrowed session that leaves statistics, resume points and the queue untouched (checked by the Music Quiz test in `games.md` §9).

### 2A.6 Enforcement on a chip with no MMU or MPU
Both cores are ARM7TDMI with **no MMU and no MPU** (`cores-and-threads.md` [rockbox]); a module is native code that is **trusted but checked**. Layers:
1. **Link time:** an SDK module links **only against the SDK's symbol table** (`sdk.sym`, a versioned stub library), not `rockbox.elf`; any other symbol fails the link. Shell modules keep `--just-symbols` + the build ID. This split is new: the module-loader decision's "no API table" stays true for shell modules, and games gain a small table so they survive firmware updates within an ABI major.
2. **Load time:** the loader checks the header and the ABI, and every relocation's target: inside the module's own image, or an entry of the SDK table, nothing else. It refuses `.data`, IRAM and any instruction-memory write outside the image (`module.lds` asserts). A refusal hides the module (§2A.1).
3. **Runtime, in the API:** every SDK call checks its arguments (bands inside the screen, handles in the table, sizes within the work block and the quota) and charges the module's budgets (§2A.7).
4. **Watchdog and budget kill** (§2A.7).

**Limits, honestly:** none of this stops a **malicious** native module from writing any RAM or peripheral register; ARM code can store to any address. What the layers stop is accident and casual misuse. Therefore:
- **P12: only our own games** ship (in `modules.pak`, built with the firmware).
- **Release policy (P20, Q25):** third-party games are installed only if **signed** by a key the unit trusts (an allow list the owner controls; the signature is checked once, at install or the post-USB moment, never at every load). Otherwise the user turns on a developer setting that accepts unsigned games, with a clear warning in our own words.
- **Hardware help [hypothesis]:** the PP5022's cache controller has eight remap windows (`MMAPn`) whose physical half carries **read, write, data and code permission bits** (`memory-map.md` §3: bits 8–11) [rockbox-derived names; behaviour unverified; the field layout itself is **PP open question 2**].
  - If a write through a window without the write bit **aborts or is dropped** on silicon, the firmware's code could be mapped read-only **through that window** (64 KB granularity, for every caller alike).
  - **It is not protection of `.text`** (M8). The same SDRAM stays writable through the **uncached alias** and through any overlapping or other window, so a store to another address of the same byte still lands. At best it catches accidental stores through the main mapping.
  - **ARM user mode** (USR) would stop a game masking interrupts (the watchdog's IRQ then always fires), but needs a mode switch on every SDK call; costed in `sdk.md`, not in rev 0.
  - Device spike **D-S1** (optional, wave 4): map a test page read-only through a spare `MMAP` window, write it through the window and through the uncached alias, and record both.

### 2A.7 Runtime authority: tracking, budgets, kill
"Ultimate authority of what is running on the iPod, and what processes are doing" (owner):
- **The loader tracks every resource a module holds:** its arena range, working block, borrow, open file handles, boosted ticks, COP jobs and BCM sessions (B/C, later), the dirty-band push count, and whether music was paused by its session. The firmware releases each one at reclaim, whatever the game did.
- **Budgets per class, enforced at runtime:**
  - RAM: the block sizes, refused beyond.
  - Boost: Class A ≤ 100 ms bursts, ≤ 10 % over 10 s; B/C as declared.
  - Storage while lit: Class A 0 reads/writes in `step()`; files only in `init`/`suspend`/`exit`.
  - **CPU slice per call:** `step()` ≤ 20 ms unboosted for Class A [est, tuned on the device], ≤ 50 ms for B/C. **The slice clock counts only time inside game code:** it stops while an SDK call runs (a push, a file read) and restarts when the SDK returns into the game, so a slow drive never "kills" a well-behaved game.
  - Pushes ≤ 30 per second.
- **Response to a violation:** the first one logs `sdk viol <name> <rule> <value>`. A repeat within the session, or any slice over **10 ×** its budget, **kills** the game:
  - immediate reclaim;
  - the Games screen with a pop slide, the row unchanged;
  - no splash (native illusion);
  - the kill is logged and counted.

  Three kills in a row hide the module until the next firmware or module update (logged; Q20).
- **Exit policy, enforced not trusted:**
  - Graceful: the SDK calls `exit()` and starts a deadline of `min(exit_ms, SDK_EXIT_MS_MAX)` (⚠️ Q19: 1,000 ms default ceiling). If `exit()` has not returned by then, the firmware takes over (the watchdog below) and reclaims anyway.
  - Immediate: no call; reclaim at once (the game saves as it goes).
  - Both: RAM back to the arena or the audio buffer, COP/BCM released, boosts dropped to the count before entry, open files closed, music state restored per class.
- **The watchdog** (rev 2, H3) [ours, a hypothesis until W4-0's prototype passes]:
  1. **At every SDK→game call** the SDK saves a restore point (a `setjmp`-style context: r4–r11, sp, the return address and the CPSR's T bit, in the SDK's own frame on the UI thread) and records `in_game = (module, entry, start_us)`.
  2. **On the tick** (Timer 1 IRQ, every 10 ms), the check compares the game-time used (excluding SDK calls, above) with the kill limit.
  3. **It unwinds only when both are true:** the interrupted thread **is the UI thread**, and the **interrupted PC is inside the module's image** (between its load address and its end). Then it rewrites the saved return context so the IRQ returns into the SDK's unwind trampoline: ARM state, with the T bit cleared in the saved SPSR, because a Thumb module was interrupted in Thumb state. The trampoline restores the restore point, as `longjmp` would, and reclaims as for a kill.
  4. **Otherwise** (the PC is in core code called by the game, or another thread was running), it only **sets a kill flag**, which the next SDK boundary honours (every SDK entry and return checks it). Core code is never unwound mid-call, so no core lock or buffer is left half-updated.
  5. **Fallback, documented:** if a game masks interrupts or never reaches an SDK boundary after the flag (a tight loop with IRQs off), nothing in software can stop it. The unit then needs the hardware reset (MENU+SELECT), as today. A soft path, a **controlled reboot with the resume point saved**, is taken when the tick sees the flag unanswered for 2 s with IRQs still on: it saves the queue's resume point (one `status_save()`-sized write while the drive is allowed), then `system_reboot()`. It is logged and counted as a kill.
  6. **P19 note:** the CPU's Timer 1 tick watches the CPU only. When the UI front end (and so the games) moves to the COP, the COP needs its own timer check: the COP is woken by the CPU's tick today (quirk 20), so the check moves into the COP's wake handler. P19's brief owns it.
  - P18's soft sleep never runs during a game call: sleep is requested and run by the shell between calls.

### 2A.8 How a misbehaving game is caught
- **Sim and gate:** the accountant of §2.3 counts per call. `tests/ui/ipodui-p12-sdk-*` scripts run a **misbehaving test module** (`sdk_badgame`, built only for tests). By mode, it:
  - overruns its slice;
  - writes outside its block;
  - opens a forbidden name;
  - writes over quota;
  - holds a boost;
  - ignores `exit()`;
  - loops forever in game code;
  - loops forever inside an SDK call (the flag path);
  - takes 3 s to save in `exit()` with a 1 s policy (the deadline enforced).
- **The exit chord** (`-sdk-exit-chord`, with a test game in each exit policy): SELECT + PREV overlapping for 0.3 s and for 1.9 s (in both press orders) does **not** exit, delivers **no** event of either button after the overlap starts, and sends `SDK_IN_CANCEL` for the one already delivered; held 2.0 s it **exits**: the graceful game's `exit()` runs and its save lands (the file in its private folder), the immediate game is reclaimed with no call; both end on the Games screen with the arena free. Also: a long MENU exits a Class A game (Q29), never a B/C game (it reaches the game as a hold); Hold PLAY reaches no game. Needs a sim chord command (`press_hold` of two buttons with a duration), W1-T.

  Each script asserts the `sdk viol`/kill lines, the reclaim (arena free, boost count, handles 0), the music position still advancing, and no splash. `modpak.py check` fails any SDK module with a non-SDK symbol or a stray relocation (fixtures in `tests/modpak/`).
- **Device:** the exit chord's feel (accidental triggers in an hour of Bricks), and **D-W1** (the wheel's untouch packet, packet rate, jitter at rest) from the developer "Wheel lab" row. The same `sdk_badgame` modes from a developer list (developer builds only), with the unwind timed (`sdk kill us=`) and 0 underruns; the per-class budgets are tuned from these.

### 2A.9 What a game may not touch (summary for `sdk.md`)
- Threads, tick tasks, timers, interrupts.
- The COP/BCM outside B/C's API; `cpu_boost` outside the SDK's bursts.
- Any file outside its directory; the database and library, music files, art caches, playlists, settings, ProkBox state files.
- The PMU; the RTC (readable only through `sdk_now_ms()`); the backlight and LCD power; USB; the drive's power.
- Other modules, the wheel driver's event switch outside `sdk_wheel_mode()`, and the reserved inputs of §2A.4 (Hold PLAY, SELECT + MENU, the SELECT + PREV exit chord, the Hold switch, and MENU in Class A).

## 3. Contracts (committed to `p12` before fan-out, with host tests and one shared stub)

Agents do not write their own stubs; the gate refuses `TEMP-` commits. Shell trace lines use `UPDATE_TRACE`/`ui_log` (`logf` is a no-op in `apps/ipodui`).

### 3.1 Module class ABI (`apps/ipodui/modload_core.h`, `modload.h`; owner W1-F)
```c
#define MODPAK_VERSION  2               /* v1 packs are refused (EVERSION) */
#define MODPAK_MAX      16              /* the P12 tenants + 3 games + spare;
                                           index = 32 B hdr + 16 x 44 B */

enum mod_class { MOD_CLASS_A = 0, MOD_CLASS_B = 1, MOD_CLASS_C = 2 };
enum mod_ram   { MOD_RAM_32 = 0,        /* runs on 32 MB and 64 MB units */
                 MOD_RAM_64 = 1 };      /* needs a 64 MB unit */
enum mod_exit  { MOD_EXIT_GRACEFUL = 0, /* exit request, bounded time */
                 MOD_EXIT_IMMEDIATE = 1 };
enum mod_kind  { MOD_KIND_SHELL = 0,    /* our cold screens: --just-symbols
                                           against the core, build ID */
                 MOD_KIND_SDK = 1 };    /* games: linked only against the
                                           SDK table (§2A), ABI-versioned */
#define SDK_ABI_MAJOR   0               /* rev 0; a major change refuses */
#define SDK_ABI_MINOR   1               /* a module's minor <= the core's */

struct modpak_ent {                     /* 44 B on disk (was 32) */
    uint32_t id, offset, image, bss_off, bss, nreloc, nexport, fletcher;
    uint8_t  mclass;                    /* enum mod_class */
    uint8_t  min_ram;                   /* enum mod_ram */
    uint8_t  abi_major, abi_minor;      /* SDK modules; 0/0 for shell ones */
    uint16_t work_kb;                   /* Class A working block, KB */
    uint8_t  kind;                      /* enum mod_kind */
    uint8_t  exit_policy;               /* enum mod_exit */
    uint32_t borrow_kb;                 /* Class B borrow, KB; 0 for A/C */
};

/* C1 (rev 2): the index is NEVER a stack object. job_open() reads the
 * 2 KB index sector in one read into the arena's top 2 KB (MOD_IDX_STAGE,
 * reserved inside MOD_ARENA_SIZE: no module may use it, modpak.py checks
 * image + bss + work <= MOD_ARENA_SIZE - MOD_IDX_STAGE), checks it there
 * and compacts it into mod_rt[]. The storage thread's frame holds only
 * pointers; its stk=% is an acceptance item (§11). */
#define MOD_IDX_STAGE   2048

/* M1 (rev 2): one compact runtime table replaces today's static
 * struct modpak_hdr (288 B, modload.c :109) and the per-module slot
 * statistics (modload.c :64-74, ~56 B each): 40 B x MODPAK_MAX = 640 B
 * (W0 measured: natural alignment makes it 40 B, not 36; W1-F may narrow
 * refs/state/flags),
 * one table (lean rule 9: "can't share because" it is the loader's own
 * state, read on every load). The load counters and timings stay in
 * developer builds only (HAVE_IPOD_UI_DEVMODE), in a separate table. */
struct mod_rt {
    uint32_t id, offset, image, fletcher;
    uint16_t bss, nreloc, nexport;      /* bss <= 56 KB by the size rule */
    uint8_t  mclass, min_ram, kind, exit_policy;
    uint8_t  abi_major, abi_minor;
    uint16_t work_kb;
    uint32_t arena_off;                 /* in the arena, or ~0 */
    uint16_t refs;
    uint8_t  state, flags;              /* enum mod_state; HIDDEN, PINNED */
};

/* The module header: export slot 0 of every module. The loader compares
 * it with the pack entry before running anything (a mismatch is
 * MODPAK_EHDR: logged, the module treated as absent). */
#define MOD_HDR_MAGIC   0x484d4b50u     /* "PKMH" */
struct mod_desc {
    uint32_t magic;
    uint8_t  abi_major, abi_minor;      /* SDK_ABI_* it was built against */
    uint8_t  mclass;                    /* enum mod_class */
    uint8_t  min_ram;                   /* enum mod_ram (Class A: MOD_RAM_32
                                           only; modpak.py refuses 64) */
    uint8_t  kind;                      /* enum mod_kind */
    uint8_t  exit_policy;               /* enum mod_exit */
    uint16_t exit_ms;                   /* graceful: asked-for time, capped
                                           at SDK_EXIT_MS_MAX (Q19) */
    uint16_t work_kb;                   /* Class A working block */
    uint16_t quota_kb;                  /* private storage quota (§2A.5) */
    uint32_t borrow_kb;                 /* Class B */
    uint8_t  boost;                     /* 0 bursts only, 1 session (B/C) */
    uint8_t  pages;                     /* W3-K: Class A page loads a
                                           second, 1..MOD_PAGES_MAX (30);
                                           0 none (mod_desc_check) */
    uint16_t state;                     /* SDK modules: state record bytes
                                           (sdk.md 4.1); 0 for shell */
    uint16_t name_lang;                 /* shell: LANG_* id of its title */
    uint16_t names_off;                 /* SDK: offset of its own per-
                                           language name table (UTF-8),
                                           built from .lang sources */
};
#define MODPAK_EHDR     (-7)
/* Runnable on this unit: ABI major equal and minor <= the core's, min_ram
 * <= the probed RAM (probed_ramsize), a class built in this firmware (P12:
 * A only). A module that fails is HIDDEN: no row in the Games list or the
 * Extras list, no page, no splash; logged "mod hide <name> why=abi|ram|
 * class|hdr" (native illusion). */
bool mod_runnable(const struct mod_rt *e);

/* M3 (rev 2): keep a module resident and unmoved while code outside the
 * UI thread runs it (the indexer on the storage worker). No eviction, no
 * move while pinned; balanced by mod_unpin(). */
void mod_pin(enum mod_id id);
void mod_unpin(enum mod_id id);

/* Class A: the module's one working block, from the arena, freed when
 * its last screen leaves (mod_screen_drop). NULL if it does not fit:
 * the caller shows its reduced view, never allocates elsewhere. */
void *mod_work_alloc(enum mod_id id, size_t bytes);
/* Class B (defined, not built in P12; M4): a borrow from the audio buffer
 * WHILE PLAYING, through #108's shrink-without-stop (not maint_take(),
 * which works only paused or stopped). False = refused (reduced mode or
 * "Not Available"). Returned by mod_borrow_return(). No maintenance
 * tenant is added in P12. */
bool mod_borrow(enum mod_id id, size_t bytes);
void mod_borrow_return(enum mod_id id);
/* Class C (defined, not built in P12): the handover. Pauses or stops
 * playback with the resume point kept and lends the buffer; exit restores
 * the queue paused. A failure stops audio and pushes the styled error
 * screen (the video-failure rule). Videos (P14) moves onto it later. */
struct mod_session;                     /* opaque, P14/P19 */
int  mod_session_enter(enum mod_id id, struct mod_session **s);
void mod_session_exit(struct mod_session *s, int status);
```
- `enum mod_id` grows by the P12 tenants: `MOD_CLOCKS`, `MOD_ALARMS` (Alarms list, Create Alarm, Sleep Timer values), `MOD_STOPWATCH`, `MOD_LOCKSET` (New/Confirm Combination, Lock/Reset), `MOD_NOTES`, `MOD_CONTACTS`, `MOD_CALENDARS`, `MOD_XINDEX` (the sync-time indexers, §3.5), then wave 4's three games: 11 with today's `MOD_SETTINGS`/`MOD_DEVLOADER`, ≤ 16. The Extras and Games **lists are resident** (§3.2, M2). `MOD_SETTINGS`/`MOD_DEVLOADER` gain `.mod` manifests (Class A).
- `modpak.py check` (new subcommand) checks the class deny lists, the size rules, any one module ≤ `MOD_ARENA_SIZE − MOD_IDX_STAGE`, and `mod_desc` equal to the manifest. Python host test `tests/modpak/`, with a fixture per rule. **`tests/modload` (owner W1-F) gains the v2 fixtures** (the v1 fixture at :106 becomes a refused-version case; a v2 pack with 16 entries; a hidden 64 MB module on a 32 MB probe; a header mismatch).
- **`MOD_ARENA_SIZE`** = the largest single tenant (image + `.bss` + work) + `MOD_IDX_STAGE` + 25 %, printed by `modpak.py` into the build (§10).
- **`IPODUI_WITH_MODULES` becomes the default at P12 integration only after the P16 loader's open device checks pass** (`module-loader.md` :97, D-M1..D-M3 in §5.4; H5). Until then P12 develops and gates with `-DIPODUI_WITH_MODULES` in the `fw-ui`/`sim-ui` builds it changes, and the release build stays resident.

### 3.2 Extras registry (`apps/ipodui/extras.h`, new; owner W1-F)
```c
/* The Extras list in stock order (spec extras.md; photos 1191/1197),
 * and the same entries for the Main Menu items (menucfg.h UI_MM_*). */
enum extra_id { XT_CLOCKS, XT_CALENDARS, XT_CONTACTS, XT_ALARMS, XT_GAMES,
                XT_NOTES, XT_SCREENLOCK, XT_STOPWATCH, XT__N };
struct extra_ent {
    uint16_t title_lang;                /* LANG_IPODUI_X_* */
    uint8_t  module;                    /* enum mod_id */
    uint8_t  slot;                      /* export slot of its root screen */
    uint8_t  transition;                /* UI_T_REVEAL for Clocks, else PUSH */
    uint8_t  split;                     /* 1: split list (Calendars, Alarms,
                                           Stopwatch); 0: full width */
    const struct ui_preview_ops *panel; /* resident panel of its row, or
                                           NULL: a pre-rendered tile from
                                           panels.pak */
};
extern const struct extra_ent extras_table[XT__N];
/* Push an Extra (from the Extras list or a Main Menu item) */
int extras_open(enum extra_id x);
```
- **M2 (rev 2): the Extras list and the Games list are resident screens**, drawn by the shared `list.c` (~1.5 KB text each [est]). Their per-item panel art (alarm clock, stopwatch, calendar page, game tiles) is **pre-rendered into `panels.pak`** by the existing art builder, as `module-loader.md` allows for right-side panels, so no list module is held in the arena under a child. Without this, the arena would have to hold a list module plus its child: 37.5 KB in wave 1, ~80 KB with games (review M2).

### 3.3 Alarms (`apps/ipodui/alarm_core.{c,h}` pure, `alarm.{c,h}` resident engine; owner W1-A1)
```c
#define ALARM_MAX        8
#define ALARM_SNOOZE_S   (9*60)         /* Q4 */
#define ALARM_SNOOZE_MAX 3              /* Q4, rev 2 */
#define ALARM_RING_S     (5*60)         /* Q5 */
#define ALARM_BOOT_WIN_S 120            /* an RTC power-on this close after
                                           the armed time is an alarm boot */
#define ALARM_HOLD_OFF_S (24*3600)      /* Q1(2): no deep power-off while an
                                           alarm is due within this */
enum alarm_repeat { AR_ONCE, AR_DAILY, AR_WEEKDAYS, AR_WEEKENDS, AR_DAYS };
struct alarm {                          /* 24 B (W0: alignment; was 20) */
    uint8_t  on;
    uint8_t  repeat;                    /* enum alarm_repeat */
    uint8_t  days;                      /* AR_DAYS: bit 0 = Sunday .. 6 */
    uint8_t  snooze;                    /* 0/1 */
    uint8_t  hour, min;
    uint8_t  label;                     /* index into the fixed label list */
    uint8_t  sound;                     /* 0 = Beep (Q3) */
    uint16_t year; uint8_t mon, mday;   /* AR_ONCE: the date */
    uint8_t  snoozes;                   /* taken in this ring, <= MAX */
    uint8_t  missed;                    /* Q17: shown inline until seen */
    uint32_t snooze_until;              /* RTC s since 2000; 0 = none */
    uint32_t missed_at;                 /* RTC s of the missed ring */
};
/* M9 (rev 2): alarms.dat = header + ALARM_MAX records, one safefile. */
#define ALARMS_MAGIC 0x4d4c4150u        /* "PALM" */
struct alarms_file {
    uint32_t magic; uint16_t version, count;
    uint32_t armed_rtc;                 /* what the PMU was last armed for
                                           (0 = disabled): read at boot to
                                           decide an alarm boot and a miss */
    uint32_t crc;                       /* CRC-32 over the records */
    struct alarm a[ALARM_MAX];
};
/* Pure (host-tested): the next ring time strictly after 'now' (RTC s since
 * 2000, local time) over every enabled alarm and snooze; 0 = none.
 * *which = its index. */
uint32_t alarm_next(const struct alarm *a, int n, uint32_t now, int *which);
/* Pure: the state after a ring (AR_ONCE switches off; snooze sets
 * snooze_until = now + ALARM_SNOOZE_S, up to ALARM_SNOOZE_MAX; dismiss
 * clears both) */
void alarm_after_ring(struct alarm *a, uint32_t now, bool snoozed);
/* Pure: was this start an alarm power-on? (armed time, RTC now) */
bool alarm_is_boot(uint32_t armed, uint32_t now);
/* Pure: the alarms whose first occurrence (or snooze) at or after
 * armed_rtc lies in [armed_rtc, now] without a ring get missed = 1 (Q17);
 * the next occurrence is computed after. (W0 fix: the open interval
 * (armed_rtc .. now] excluded the armed occurrence itself, the one that
 * was missed.) An alarm boot that will ring is not a miss. */
void alarm_mark_missed(struct alarm *a, int n, uint32_t armed,
                       uint32_t now);
/* Pure: the PMU register image for an armed time: sec/min/hour BCD, the
 * weekday register at its ignore value (M9), and either the exact day/
 * month/year (one-shot) or the ignore values (daily match), per mode */
void alarm_pmu_regs(uint32_t armed, bool daily, uint8_t regs[7]);

/* Engine, UI thread. alarm_init() runs right after the settings load: it
 * reads alarms.dat (one read; a bad magic/version/CRC is "no alarms",
 * logged, and the file is rewritten from the empty state at the next
 * edit), decides an alarm boot, marks misses, and re-arms the PMU. */
void     alarm_init(void);
/* After any edit, a Date & Time or Time Zone change, a ring, a wake: the
 * one place that recomputes the next time, arms the PMU (the full
 * register image, then RTCWAK), READS THE REGISTERS BACK and logs
 * "alarm arm rtc= ok=" (a mismatch retries once, then logs), and writes
 * alarms.dat if it changed (safefile, at once: it must survive a power
 * cut). Inside a USB session (usb_inserted() with the host owning the
 * disk) it arms the PMU only and marks the file write owed for the
 * post-USB path (M9). */
void     alarm_rearm(void);
uint32_t alarm_next_rtc(void);          /* for sleep.c (wave 2): 0 = none */
/* Q1(2): true while an alarm is due within ALARM_HOLD_OFF_S and the
 * bootloader fix is not on the unit: the shell's power-off requests (hold
 * PLAY, idle, Sleep Timer) take the dark wait instead */
bool     alarm_blocks_poweroff(void);
/* Main-loop deadline (lean rule 5: one timer source): ticks until the
 * next ring while awake, TIMEOUT_BLOCK if none; the loop calls
 * alarm_poll() when it expires and after every wake. */
long     alarm_timeout(void);
void     alarm_poll(void);              /* rings if due: pushes the ring
                                           screen (resident) */
bool     alarm_ringing(void);
```
- **The ring screen is resident** (like About: it must never depend on a file loading). It pauses playback (Q6), lights the screen, runs the beep pattern (§5.3), handles Q5's keys and the 5 min cap. ≤ 3 KB text.
- **H2 (rev 2): the power thread no longer disarms.** `powermgmt.c` `power_thread_rtc_process()` (:541–546) calls `rtc_enable_alarm(false)` whenever INT1's alarm flag is set: it writes year `0x99` and clears RTCWAK, racing `alarm_rearm()`.
  - Under `HAVE_IPOD_UI` it **reads INT1 only** (which clears the flag, a read-to-clear register [rockbox-derived]) and posts nothing. **`alarm_rearm()` is the only code that arms or disarms.**
  - The guarded edit is W1-A1's, with a ledger row.
  - Sim model: the sim's PMU stub keeps INT1 bit 7 and the alarm registers, sets the flag when the scripted clock passes the armed time, and runs this thread's check on the power-thread cadence. Test script `-alarm-disarm-race`: an alarm fires awake → Snooze → hold PLAY power-off. The PMU must still hold the snoozed time with RTCWAK set (`alarm arm` read-back line).
- **H6/M9: RTCWAK at boot.** `pcf50605_wakeup_flags` is 0 at every boot (`pcf50605.c` :36), so **any power-off before `alarm_init()` would drop RTCWAK**: the boot guard's refusal, an early low-battery shutdown, a crash reboot.
  - A guarded edit in `rtc_init()` (`rtc_pcf50605.c`, W1-A1) reads the alarm registers. If they hold a **valid future time** (year ≠ `0x99`, and the date or the daily match lies ahead), it sets `pcf50605_wakeup_flags |= RTCWAK`. This is disk-free.
  - The boot guard's refusal path (`bootbatt.c`, before storage) calls the same check (`rtc_keep_alarm_wake()`), so a refused boot powers off with the alarm still armed (review H6, "disk-free PMU re-arm").
  - If the armed time has just passed (an alarm boot that cannot ring: a refused battery), the check converts the PMU to the **daily match of the same time**, so a repeating alarm fires again tomorrow instead of being lost. The next boot that reaches `alarm_init()` recomputes it exactly.
- **Writers of `alarms.dat` (checklist 13):** the Alarms module's editor (on leaving an edited alarm), `alarm_rearm()` after a ring/snooze/dismiss/miss, the post-USB path (a deferred write), and **Reset Settings** (`screens/reset.c`, which deletes it and disables the PMU alarm through `alarm_rearm()`; owner W1-A1, one call). Nothing else.
- **Writers of the PMU alarm registers:** `alarm_rearm()`; the `rtc_init()`/boot-guard keep-alive above (RTCWAK, or the daily conversion only). Apple's firmware also writes them; the next boot reads them as they are (§5.5).
- **The ring inside the USB loop (Q7):** owner W1-A1. One call in `usbscreen.c`'s loop to `alarm_timeout()`/`alarm_poll_usb()` (piezo only).
- Host test `tests/alarm/`:
  - `alarm_next()` across midnight, week and month ends, a leap day, every repeat mode and `days` mask, `AR_ONCE` in the past, snooze before and after the base time, the snooze cap, two alarms at the same minute;
  - the clock set backwards and forwards (rearm), a zone change (local shift);
  - `alarm_is_boot()` at −1, 0, 119, 121 s;
  - `alarm_mark_missed()`: a missed `AR_ONCE`, a missed weekday then the next weekday armed;
  - `alarm_pmu_regs()` for both modes, the weekday register at its ignore value;
  - the file round trip, and a corrupt file (bad magic, bad CRC, short).

### 3.4 Stopwatch and Screen Lock engines (resident; owners W1-S, W1-L)
```c
/* stopwatch_core.h: pure; time = RTC seconds + tick fraction, so a
 * running stopwatch survives soft sleep (current_tick freezes, P18).
 * Shown in tenths (the 7G's resolution), redrawn at 10 fps (rev 2, Low). */
#define SW_LAPS_MAX  32
#define SW_LOGS_MAX  20                 /* oldest dropped */
struct sw_state { uint32_t start_rtc; int32_t start_tick; uint32_t acc_ms;
                  uint8_t running, nlaps; uint32_t lap_ms[SW_LAPS_MAX]; };
uint32_t sw_elapsed_ms(const struct sw_state *s, uint32_t now_rtc,
                       long now_tick);  /* tick part only within 1 s */
void sw_lap(struct sw_state *s, uint32_t now_ms);
/* stopwatch.dat: SW_LOGS_MAX records {start_rtc, total_ms, nlaps,
 * shortest, longest, average, last 3 laps}: written at Stop and at New
 * Timer, never per lap. */

/* lock.h: the gate. ui_lock_active() is checked by input.c before any
 * screen's input (W1-L's one hook commit, §6); locked = only the
 * combination entry, hold PLAY, Hold, alarms (Q10). The state persists
 * as two settings, ipodui_lock_code (-1..9999) and ipodui_locked (0/1),
 * added to settings_list.c / settings.h by W1-L (guarded, ledger row). */
bool ui_lock_active(void);
void ui_lock_engage(void);              /* from Screen Lock's Lock row */
bool ui_lock_try(int code);             /* true = unlocked */
void ui_lock_usb_host(void);            /* data host connected: unlock */
```
- `struct sw_state` is 144 B, resident (the stopwatch runs while its screen is closed). (W0: `start_tick` is `int32_t`, not `long`, so the layout is the same on the target and a 64-bit host; 144 B, not the estimated 152 B.)

### 3.5 Sync-time indexes (`apps/ipodui/xindex_core.{c,h}`; owner W1-N defines the format, W3-C/W3-K add their records)
```c
/* One file per source folder (Q12), in the hidden folder. Built by the
 * MOD_XINDEX module on the storage worker, in the post-USB moment (#84)
 * and the boot window (#87), only while playback is stopped, inside one
 * maint_take() block (M3). The module is pinned (mod_pin) for the job.
 * A boot the boot stamp trusts as clean (P16 bootstamp.h) skips the
 * signature check: nothing could have changed the folders. */
#define XIDX_MAGIC 0x58444958u          /* "XIDX" */
#define XIDX_VERSION    2               /* W3-K: paged Notes; another
                                           version fails xidx_check(), so
                                           it is rebuilt (a trusted boot
                                           reads the headers for it) */
#define XIDX_FILE_MAX   (64*1024)       /* Calendars */
#define XIDX_NOTES_FILE_MAX (96*1024)   /* Notes: pages + far names */
#define XIDX_CONTACTS_FILE_MAX (256*1024)  /* Contacts: ~2,000 cards */
#define XIDX_BUILD_BLK  (520*1024)      /* the maint_take() block (W3-K):
                                           Contacts' cards, pool, scratch
                                           and the index being built;
                                           Notes uses its first ~170 KB */
enum xidx_kind { XIDX_NOTES = 1, XIDX_CONTACTS = 2, XIDX_CAL = 3 };
struct xidx_hdr {
    uint32_t magic; uint16_t version, kind;
    uint32_t sig;                       /* FNV-1a over the folder's
                                           (name, size, mtime) list */
    uint32_t count, rec_off, str_off, str_bytes;
    uint32_t crc;                       /* over records + strings */
};
/* Notes and Contacts are paged lists (W3-K, xindex_core.h "Paged
 * lists"): a directory per list, self-contained pages of at least
 * XIDX_PAGE_ROWS_MIN (12) rows, labels of at most XIDX_LABEL_MAX (48)
 * bytes, so a reader's window lies in two pages.
 * Notes: {name_off, path_off, size} (12 B) per page row, sorted by name
 *   (sortkey.c); a name longer than a label keeps its cut label in the
 *   page and the file name in the string block (XNOTE_PATH_FAR).
 * Contacts: {card, label, flags} (8 B) per page row: the card's offset
 *   in the card block (organisation, then items), the "Given Family"
 *   label; two orders (First, Last: Settings > Sort By), each paged.
 * Calendars: events expanded over [today - 1 month, today + 12 months]:
 *   {start_rtc, end_rtc, title_off, loc_off, cal, flags} (20 B), sorted by
 *   start, capped at XCAL_MAX_INSTANCES = 2,048 (40 KB of records + a
 *   ~2 KB per-day bitmap + strings, inside XIDX_FILE_MAX; a cut is logged
 *   "xcal cut n="). Revision 1's 8,192 x 20 B (160 KB) broke the 64 KB
 *   cap (M3). The window rolls forward at each build. */
int xidx_check(const struct xidx_hdr *h, uint32_t file_bytes);
```
- If the stopped-state `maint_take()` is refused (no room), the build stays owed for the next sync moment (logged `xidx owed`). The screen shows the last good index, or its stock empty state.
- Host test `tests/xindex/`: header round trip, CRC, the signature (a renamed, a resized, an added file each change it), the caps, and the wave-3 parsers. Fixtures: vCard 2.1/3.0/4.0 with quoted-printable, folded lines and UTF-8 names; `.ics` with all-day events, time zones as fixed offsets, `RRULE` FREQ=DAILY/WEEKLY/MONTHLY/YEARLY with COUNT/UNTIL/BYDAY, `EXDATE`, `VTODO`. Notes need no parser: the index is names and sizes, and a note is read whole (≤ 4 KB, stock) when opened.

### 3.6 Strings, the shared stub and the resident time-zone picker
- **Strings:** the contract commit **appends every P12 `LANG_IPODUI_X_*` id at once** to `apps/lang/english.lang` (checklist 11's accepted exception), with our own English text, so agents only edit the text of their own ids and never append. **Merge rule:** P12's block stays the last block of the file. If P18 or P17 append after P12's contract commit, the integrator moves P12's block back to the end in the merge (ids are by name; their numbers follow the order, as for every shell phase). Short labels may match the 7G (Clocks, Create Alarm, Sleep Timer, New Timer, Clear Logs, New Combination, Lock, Reset, Snooze, Dismiss…); every sentence is ours.
- **Shared stub** (owner T, W1-T): new commands go in their own `uisimulator/common/sim_cmd_extras.c`. **T alone** adds the one registration line in `sim_script.c`. The commands:
  - `alarm_boot <s>`: start as if the RTC powered the unit on `s` seconds after the armed time;
  - a sim PMU stub (alarm registers, INT1, RTCWAK, the power thread's check; H2);
  - `piezo` trace lines (`piezo play hz= ms=`, from a sim `piezo_play*` that only logs);
  - the `modclass` accountant hook in the sim file layer.

  **Reused, not re-invented:** P18's `rtc_set` and `sleep_warp` (`sim_cmd_sleep.c` :433–523), `usb_connect`/`usb_disconnect` (`sim_cmd_art.c` :323–328), `apps/ipodui/simclock.c`.
- **`tzpick.{c,h}`** (resident; owner W1-C, M7): the region → city picker and the city table, moved out of `screens/datetime.c` (the Settings module) so that Date & Time and Clocks both use one copy. Plus the DST rule table (Q27): one byte per city, rules none/US/EU/south. Size: table ~0.7 KB + picker ~2 KB + DST ~0.4 KB [est], measured at W0.

## 4. Scope, ranking and order

Value (to the owner and the mission) and effort, [est]. Effort counts code, art and tests; "device" marks work that only the unit can close.

| Rank | Extra | Value | Effort | Class / module | Depends on P18's sleep internals? |
|---|---|---|---|---|---|
| 1 | **Alarms + Sleep Timer** (engine, PMU arm and keep-alive, the disarm fix, ring screen, dark wait, Alarms list, Create Alarm, Sleep Timer values) | High: the owner's named priority; daily use; the haptic wake | **M–L**, device | A; resident engine + ring screen, `MOD_ALARMS` UI | **Partly:** everything except the soft-sleep hook (wave 2) |
| 2 | **Bootloader alarm window** (W1-B, Q1) | High for bedside use | S, device, owner sign-off | bootloader | No (gated on D-A2) |
| 3 | **Class A framework + Extras list** (class ABI, compact index, checks, accountant, the resident Extras list and its panels) | High: every other Extra stands on it | M | A | No |
| 4 | **Clocks** (city bands, Add/Edit/Delete, day/night, DST, reveal) | High: the Extras panel already promises it; the 7G's signature screen | M (reveal wired in wave 2) | A; `MOD_CLOCKS` + resident `tzpick` | Reveal wiring touches `ui_main.c` (wave 2) |
| 5 | **Stopwatch** (laps, resume, logs) | Medium | S–M | A; resident state, `MOD_STOPWATCH` | Wake redraw only (wave 2) |
| 6 | **Screen Lock** | Medium (stock completeness, privacy) | S–M (the roller exists) | A; resident gate + locked screen, `MOD_LOCKSET` | No (one `input.c` hook commit, §6) |
| 7 | **Notes** (index, list, viewer) | Medium (drag-and-drop mission: drop `.txt` files) | S | A; `MOD_NOTES` + `MOD_XINDEX` | No |
| 8 | **Contacts** (vCard index, Sort By, detail) | Low–medium | M (parser) | A | No |
| 9 | **Calendars** (month grid, events, To Do's, event alarms) | Medium | **L** (iCal + recurrence; owner flagged costly) | A; event alarms feed `alarm_next()` | Event alarms: same wake as Alarms |
| 10 | **SDK rev 0 + Games** (Solitaire, Bricks, Parachute) | Medium (owner "all of them"; the SDK is the release's developer story) | L (SDK + art + 3 games) | A each; Music Quiz B (Q16) | No |
| 11 | **Custom EQ curves (#82)** | Medium for audiophiles | M–L | A editor; resident apply path | No (Q14: likely P12b) |
| – | Voice Memos | – | – | – | Not built (no mic path, catalogue §6) |

## 5. Alarms: the wake cases

### 5.1 Soft sleep on (P18, when its device test passes)
- At sleep entry (`sleep.c` `sleep_once()`, wave 2): `rq.alarm_rtc = alarm_next_rtc()`. `power_sleep()` programs each poll for `min(poll_s, alarm_rtc − now, …)` (P18 M5), reads the RTC by the raw path, and returns `WAKE_ALARM` on time **without needing the PMU's interrupt** [ours, built on P18's poll; the poll timer at 24 MHz is P18's D2 test].
- **The PMU is also armed**: `alarm_rearm()` has already done it. So the deep tier's power-off (`WAKE_DEEP`, `WAKE_LOWBATT` → `sys_poweroff()`) carries RTCWAK and falls into case 5.2.
- On `WAKE_ALARM` (`sleep.c` :718): `ui_wake_restore()` as for any wake, then `alarm_poll()` pushes the ring screen. The swallow mask is empty (no button woke it). `reset_poweroff_timer()` is already called at wake (P18 M1), so an alarm wake does not sleep again at once.
- **Sleep entry with an alarm due within `SLEEP_MAINT_MS` + 5 s:** the entry is refused; the unit stays awake and dark, so the window's job never delays a ring [ours].

### 5.2 Soft sleep off (the default until P18's D2), or the deep tier
- Every power-off goes through `power_off()` → `pcf50605_standby_mode()`: `OOCC1 = GOSTDBY | CHGWAK | EXTONWAK | RTCWAK` when RTCWAK is in `pcf50605_wakeup_flags` [rockbox-derived]. At the alarm, the PMU leaves standby, the **boot ROM** loads `osos` and runs **our bootloader**, which loads ProkBox (`clocks-power.md` §6) [rockbox-derived; never run on our unit].
- **Until W1-B is on the unit (Q1(2), Q28):** `alarm_blocks_poweroff()` turns the shell's power-off requests (hold PLAY, the idle timer, the Sleep Timer) into the **dark wait** while an alarm is due within 24 h. Only a forced low-battery shutdown (`query_force_shutdown()`) still powers off, with RTCWAK armed. A button press wakes from the dark wait as from LCD sleep. D-A8 measures the drain.
- **W1-B, the bootloader alarm window (gated on D-A2, owner sign-off):** in `bootloader/ipod.c`, before `button_was_held = button_hold()` (:314) is acted on (:399):
  - read the PMU's RTC (`0x0A`–`0x10`) and alarm (`0x11`–`0x17`) registers over I2C (the bootloader already drives the PMU for its own power-off);
  - if the alarm is valid and the RTC is 0–`ALARM_BOOT_WIN_S` after it, ignore Hold and boot ProkBox;
  - hold MENU still boots Apple's firmware.

  It is a guarded edit with a ledger row in `changes-from-rockbox.md` and a 7G-ledger row. The bootloader build is part of the gate (`boot`).

  **Owner sign-off, 2026-10-23, with conditions:** approved after D-A2, on the condition that we are "extra careful" and build "as many guardrails as we can during testing locally to prevent a catastrophic failure". Longer term, the owner intends to retire the Apple loaders and firmware and own the boot chain lower down. Guardrails W1-B must meet (acceptance):
  1. **Fail-safe by construction.** The decision is a pure function `boot_alarm_override(rtc, alarm, flags, hold)` in its own file, with no I/O. Any I2C error, timeout (bounded, no unbounded polls), invalid BCD, or alarm not armed returns "no override", which is today's behaviour exactly. Host-tested exhaustively: every Hold state × wake reason × register garbage × window edge, plus a randomized fuzz.
  2. **Smallest possible diff.** The only change on the existing path is one call before the Hold test. `cmp` and `objdump` diffs against the current bootloader are reviewed. The stock (non-shell) bootloader stays byte-identical (the gate already checks this). Size stays within the bootloader's limit, with margin recorded.
  3. **Staged on the device.**
     - (a) D-A1/D-A2 with today's bootloader prove that the alarm powers the unit on and that the RTC and alarm registers read as expected (from the firmware).
     - (b) A **log-only** bootloader computes the decision and leaves it in a reserved IRAM word, or a PMU scratch byte if Q-open 33 finds one, which the firmware logs at boot. Behaviour is unchanged.
     - (c) Only after (b) matches on real alarm and non-alarm boots, the acting bootloader.
  4. **Recovery proven before any install.** Before the first bootloader write:
     - Confirm on this unit, and record in `docs/pp5022-reference/`, which recovery paths exist and where they live: Apple's Disk Mode, the Hold path to Apple's firmware, and a restore with `ipodpatcher` from the Mac. Rev 2 of this brief assumed Disk Mode lives in ROM; that is [hypothesis] until checked.
     - Rehearse the restore of the current bootloader from a saved image.
     - Keep a verified read-back of the firmware partition (`ipodpatcher` read) with sha256 in the backup folder.
  5. **Install discipline.** The owner's explicit go for each bootloader install, a fresh `.rockbox` and partition backup, a read-back verify after the write, and one boot observed with each of: Hold off, Hold on (must still reach Apple's firmware), and an alarm boot.
  6. **Never two risky changes at once.** No bootloader install in the same session as a firmware change that touches boot, power or the PMU.
- **Boot:** `alarm_init()` runs right after the settings load. `alarm_is_boot(armed_rtc, now)` is true within `ALARM_BOOT_WIN_S` = 120 s after the persisted `armed_rtc` (not upstream's 10 s, which an HDD spin-up can exceed) [ours]. An alarm boot **skips the boot window's optional work** (snapshots, top-ups; P16 #87) until the ring is dismissed, and goes straight to the ring screen after the boot glyph. The boot battery guard still runs first: a cell under the floor stays off, and the alarm is kept alive per §3.3.
- **Daily or one-shot match: recommendation (H6).**

  | | Daily h:m:s match (upstream's) | Exact date (one-shot) |
  |---|---|---|
  | Lost future repeats | **Never**: it fires every day at that time until rewritten | Possible: if the armed occurrence fires but no firmware runs `alarm_rearm()` (Hold → Apple's firmware before W1-B, a refused battery), nothing is armed for later |
  | Nights with no ring | **A boot glyph at the alarm time on every off day** (the bootloader draws it before the backlight, `afef154c64`), then a dark power-off: bad at a bedside | None |
  | Evidence | [rockbox-derived], upstream's only mode | [hypothesis] (D-A3) |

  **Recommended: exact date as the primary mode once D-A3 passes, with the daily match as the safety net.** Every path that powers off without reaching `alarm_init()` (the `rtc_init()` keep-alive, the boot guard) converts a just-passed alarm to the daily match of the same time (§3.3), which removes the exact mode's loss case. Daily-only if D-A3 fails; then an off-day wake re-arms and powers off at once (logged).
- **The year-`0x99` "disabled" value** is upstream's convention [rockbox-derived]. That the PCF50605 compares the year at all (so `0x99` never matches) is a **[hypothesis]**, and D-A3 decides it. `alarm_rearm()` writes the disabled value once per change when no alarm is set. The boot ROM is said to set RTCWAK itself (`rtc_pcf50605.c` :94–96 comment [rockbox-derived]), so a stale alarm left by Apple's firmware could power the unit on. `alarm_init()` disables it at the first boot that finds no `alarms.dat` with that time.

### 5.3 The beep
- Pattern [ours, tuned on the device]: 4 × 100 ms at ~2 kHz (`piezo_play_for_tick(piezo_hz(2000), 0x80, HZ/10)`), 100 ms gaps, then 800 ms of silence; repeat until Snooze/Dismiss or `ALARM_RING_S`. It is driven from the ring screen's `tick()` at its deadlines (one timer source), never a new thread; the piezo thread plays each burst.
- Independent of the Clicker setting (#32 note). The haptic mod follows the piezo line (the owner's working assumption, #23). Whether a 2 kHz tone drives the motor well, or a lower frequency and longer pulses feel better, is **D-A5**.
- The screen stays lit while ringing (the ring screen holds the backlight timer and releases it on exit).

### 5.4 Device questions (an owner session after the go and a backup; developer "Alarm lab" rows in the developer UI, our words, logged to `ipodui.log`)
| ID | Question | How | Evidence today |
|---|---|---|---|
| **D-A1** | Does the PCF50605 RTC alarm power the unit on from `power_off()` standby? The time from the alarm to the ring | Arm 2 min ahead, power off (soft sleep off, the dark wait disabled in the lab), wait; log the boot's RTC minus the armed time | [rockbox-derived] only (`HAVE_RTC_ALARM` on this target; upstream's alarm screen) |
| **D-A2** | With **Hold on**: does the bootloader start Apple's firmware (Q1)? Then the same with W1-B's bootloader | As D-A1 with Hold on; observe; hold MENU recovery as usual | [rockbox-derived] `bootloader/ipod.c` :399 says yes |
| **D-A3** | Does the PMU match the full date (one-shot), and does year `0x99` never match? | Arm for tomorrow's date at a time 2 min ahead today: it must **not** wake today. Arm with year `0x99`: it must never wake | [hypothesis] |
| **D-A4** | Inside soft sleep (once P18's D2 passes): `WAKE_ALARM` on time from the poll; and from the deep tier (developer "deep after 2 min") | Sleep lab + an alarm 5 min ahead | P18 §4.0 D2 row "an RTC alarm 2 min ahead" |
| **D-A5** | The piezo pattern on the haptic: felt, audible, how far; current while ringing (power-log mV slope) | Alarm lab "Beep test" rows at 3 frequencies | #23 open |
| **D-A6** | A charger or USB host present at the alarm time with the unit off (CHGWAK also set): does it ring? | Plugged, powered off, alarm | [hypothesis] |
| **D-A7** *(rev 2)* | **Hold on at a Sleep Timer power-off**: the bedside case end to end (Sleep Timer 15 min, Hold on, an alarm next morning), with and without W1-B | Real night, or a 20 min timer and an alarm 30 min ahead | – |
| **D-A8** *(rev 2)* | The dark wait's drain (Q28): mV slope over 8 h dark-awake with an alarm armed | Power log, as the P18 D3 method | – |
| **D-M1** *(rev 2, H5)* | The P16 loader's open device checks (`module-loader.md` :97): load latency over 50 entries while playing (flash; HDD awake and asleep) with 0 underruns; the stand-in-to-real swap time; the snapshot batch time; the Fletcher cost; the error page with the pack removed. **Precondition for the modules-by-default flip** | Developer "Loader & storage" screen | Open since P16 |
| **D-M2** *(rev 2, H5)* | **Pack missing or stale with an alarm armed:** the resident ring screen still rings on time; Extras › Alarms shows the styled "Not Available" page; nothing else changes | Remove or replace `modules.pak` over USB with an alarm 5 min ahead | – |
| **D-W1** *(rev 2, input model)* | The clickwheel for absolute mode: is a packet sent when the finger lifts (else `wheel_status()` stays stale)? Packet rate while moving and at rest; position jitter at rest; `wheel_position` reset after a P18 wake | Developer "Wheel lab" row logging each packet's position, touch bit and `USEC_TIMER` | [rockbox-derived] positions 0–95 and the touch bit; lift packet unknown |
| **D-M3** *(rev 2, C1)* | **The storage thread's `stk=%` at pack open** with a 16-entry v2 index (the `job_open()` line) | Boot and a USB reconnect, read `ipodui.log` | – |

### 5.5 An alarm is never silently lost (rev 2, H6)
- **Armed at all times it should be:** `alarm_rearm()` after every change; RTCWAK kept by `rtc_init()` and the boot guard before any early power-off; the power thread no longer disarms (H2); the read-back after every arm.
- **Missed is visible:** an alarm whose time passed while it could not ring is marked `missed` by `alarm_init()` (from the persisted `armed_rtc`) and shown **inline in the Alarms list** (its subtitle reads "Missed at 7:00", our wording) until the list is opened or the alarm next rings. The list state is drawn by the module; no splash, no popup (Q17, changed).
- **Missed, then the next repeat:** a missed repeating alarm keeps its schedule; the next occurrence is armed at the same boot.
- **Snooze across a power-off:** `snooze_until` is persisted, and the PMU is armed for it, so a snoozed alarm rings after a power-off too.

### 5.6 Sleep Timer and soft sleep (rev 2, M10)
- The engine counts in ticks (`sleeptimer_endtick`), which freeze in a soft sleep. At **sleep entry** the shell cancels the timer (`set_sleeptimer_duration(0)`): the sleep already did what the timer was for, and a frozen deadline would fire on wake. The Alarms row then reads Inactive.
- `sleeptimer_on_startup` (`settings.c` :980) is upstream's "start the timer at every boot"; it is hidden (developer only), as every Rockbox-only setting is (rule 1), and listed in checklist 12.

## 6. Waves, parallel work and ownership

**W0 (orchestrator, before fan-out, on branch `p12` from `p18-114` d328f1ef86 + `games-research`):** the contract commit and T's sim stub. It contains:
- the §3 headers;
- the `.mod` manifests for today's tenants;
- the reserved LANG block;
- the deny-list files;
- `tests/alarm`, `tests/xindex`, `tests/modpak` skeletons with their round-trip cases, and the `tests/modload` v2 fixtures.

**W0 as landed (2026-10-23).** Pack v2 is live in `modload_core.{c,h}` and `tools/modpak.py` (44 B entries, `MODPAK_MAX` 16, labels from `apps/ipodui/modules/*.mod`, `modpak.py manifest`); `MODPAK_EVERSION` refuses v1. `tests/stopwatch` and `tests/lock` skeletons were added too. Each skeleton compiles its cases against the header and prints `SKIP … not written yet (<owner>)` until its `_core.c` exists. The W1-T sim stub stays with W1-T (§3.6, §6 table): W1-A1's ring and disarm-race scripts wait for it. Contract additions beyond §3, kept (orchestrator, 2026-10-23):
- `alarm.h`: `alarm_count/get/set/delete`, `alarm_seen()` (Q17), `alarm_reset()` (Reset Settings), `alarm_poll_usb()` (Q7): the engine/UI line between W1-A1 and W1-A2;
- `alarm_core.h`: `alarms_file_seal/check`, `alarm_rtc_make/split/wday`, `ALARM_REG_IGNORE`;
- `modclass.h`: `ui_boost_burst(ms)`/`ui_boost_burst_end()` (the only Class A boost; the deny list names it) and the accountant hooks (`modclass_top/io/alloc/push/pass`, no-ops in release builds);
- `lock_core.h`: the pure combination state machine behind `lock.h` (for `tests/lock`);
- `stopwatch_core.h`: `sw_start/stop/reset`, `struct sw_log` (36 B), `sw_log_make/push`, `sw_logs_seal/check`;
- `xindex_core.h`: the record structs, `xidx_rec_size/check_body/seal`, and the FNV-1a byte order of `xidx_sig_add()`;
- `tzpick.h`: the `tzp_*` table accessors, `struct tzpick` + `tzpick_push()`, `tz_dst_minutes(rule, utc, std_offset)`, `tz_city_time()`;
- every new file format seals with the firmware's `crc_32(…, 0xffffffff)`;
- `enum mod_id` grows by the eight P12 tenants now; the P16 loader's tables size by `MOD__LOADER` (2) until W1-F's `mod_rt[]`, so a P12 id reads as absent.

**Left for W1-F (interim in W0):** `job_open()` reads the v2 index straight into the static `hdr` (736 B, modules-on builds only) instead of the storage thread's stack; the `MOD_IDX_STAGE` read into the arena and the compaction into `mod_rt[]` are W1-F's. **Resident (modules-off) build:** `mod_work_alloc()` returns a block from a small `core_alloc` made when an Extra opens, freed when its last screen leaves (W1-F implements; the floor gate counts it as an Extra-open transient).

**Merge order in wave 1:** W1-F lands first (it implements `mod_work_alloc`, `mod_pin`, `extras_open`, `ui_boost_burst` and the accountant, which the other packages call). A package that must merge before it keeps its calls behind its own `static` wrapper, removed at W1-F's merge; never a `TEMP-` stub in the contract files.

`p12` merges P18's later commits as they land (the owner's rule: the next phase's work merges into the finishing phase).

### Wave 1: can start **now**, in parallel with P18's close
No P18 sleep internals. The only P18-owned files touched are the two **hook commits** named below, each one small, guarded, and reviewed by P18's `ui_main.c`/`input.c` owner before it lands. **Recommended over moving the ring and lock scripts to wave 2** (H4): the ring and the lock are wave 1's main value, and their scripts need the hooks.

| Package | Agent (model) | Owns (new unless noted) | Read list (≤ 3) |
|---|---|---|---|
| **W1-F Class A framework + Extras list** | firmware-platform for the loader (**opus**); ui-fidelity for the list (sonnet) | `modload.{c,h}`, `modload_core.{c,h}` (compact index, `mod_rt`, `mod_pin`), `tools/modpak.py`, `apps/ipodui/modules/*` (manifests, deny lists, `modules.make`), `tests/modload`, `tests/modpak`, `modclass.c`, `extras.{c,h}`, `screens/extras.c` (**resident** list), `menu.c` (Extras row → `extras_open`, the Main Menu Extras items), the Extras-list panel tiles (art sources for `panels.pak`) | this brief §2–3.2; `docs/design/module-loader.md`; spec `extras.md` |
| **W1-A1 Alarm engine + PMU** | firmware-platform (**opus**) | `alarm_core.{c,h}`, `alarm.{c,h}`, `screens/alarmring.c` (resident), `tests/alarm/`; guarded edits with ledger rows: `rtc_pcf50605.c` (`rtc_set_alarm_full()`, the `rtc_init()` keep-alive, `rtc_keep_alarm_wake()`), **`powermgmt.c` `power_thread_rtc_process()` (H2)**, the boot guard's one call (`bootbatt.c`); one call each in `usbscreen.c` (Q7) and `screens/reset.c`; **hook commit 1 in `ui_main.c`**: `alarm_timeout()` in the loop's deadline, `alarm_poll()` after every wake and at boot, `alarm_blocks_poweroff()` → the dark wait in `global_hold()`'s power-off branch; the developer "Alarm lab" rows | §3.3, §5; `clocks-power.md` §6 + §9; `rtc_pcf50605.c` |
| **W1-B Bootloader alarm window** | firmware-platform (**opus**), **after D-A2 and the owner's sign-off** | `bootloader/ipod.c` (guarded), ledger rows | §5.2; `bootloader/ipod.c`; `clocks-power.md` §6 |
| **W1-A2 Alarms UI + Sleep Timer screen** | ui-fidelity (sonnet) | `screens/alarms.c` (module: the Alarms list with two-line rows and the missed state, Create Alarm editor, Sleep Timer values via `set_sleeptimer_duration()`), alarm-clock panel art in `apps/ipodui/art/`. **Labels and order wait for capture C2** | §3.3; spec `extras.md`; `INDEX-b.md` photo 1197/1198 |
| **W1-C Clocks** | ui-fidelity (**opus** for the visual match) | `screens/clocks.c` (module), city-band art, the Add/Edit/Delete popup, **`tzpick.{c,h}` (resident) and the move out of `screens/datetime.c`** (a Settings-module file, one commit, with Date & Time's scripts re-run), the DST table; the reveal **motion** as new functions in `anim.c`/`nav_core.c`, wired in wave 2 | spec `index.md` Transitions item 4; `INDEX-a.md` photo 1192/1193; `VIDEO-2.md` §A3 |
| **W1-S Stopwatch** | ui-fidelity (sonnet) | `stopwatch_core.{c,h}`, `stopwatch.c` (resident state), `screens/stopwatch.c` (module), stopwatch panel art, `tests/stopwatch/` | §3.4; spec `extras.md`; `INDEX-b.md` photo 1202 |
| **W1-L Screen Lock** | ui-fidelity (sonnet) | `lock.{c,h}` (resident gate + locked screen), `screens/lockset.c` (module), padlock art, `tests/lock/`; `settings_list.c`/`settings.h` (the two lock settings, guarded, ledger row); **hook commit 2 in `input.c`**: the lock gate before any screen's input, and `ui_lock_usb_host()` at the data-host event | §3.4; `INDEX-b.md` photo 1201; `vlimit.c` + `roller.h` |
| **W1-N Notes + indexer** | ui-fidelity (sonnet); the indexer core by playback-audiobook (sonnet) | `xindex_core.{c,h}`, `screens/xindex.c` (`MOD_XINDEX`: the Notes builder), `screens/notes.c` (module: list, a paged text viewer), `tests/xindex/`; **one commit in `maint.c`** (the stopped-state take for the build and the post-USB/boot-window call; reviewed by P18's `maint.c` owner, agent B) | §3.5; `INDEX-b.md` photo 1200; `docs/phase16-brief.md` #84 section |
| **W1-T Tooling** | tooling (sonnet) | `sim_cmd_extras.c` and its registration line in `sim_script.c`, the sim PMU stub, the `modclass` sim hook, `tests/ui/ipodui-p12-*.script`, `scripts/gate_map.txt` rows, the `floor_gate.sh` arena case, the trace rules | §2.3, §3.6, §9 |

### Wave 2: after P18's 18-1/18-2 are final on `p18` (its `sleep.c` owner hands over)
| Package | Agent | Owns |
|---|---|---|
| **W2-W Wake hooks** | ui-fidelity (**opus**) | `sleep.c`: `alarm_rtc = alarm_next_rtc()`, the `WAKE_ALARM` branch, the entry refusal near an alarm (§5.1), the Sleep Timer cancel at entry (§5.6); `ui_main.c`: the reveal wired for `UI_T_REVEAL`; the dark wait handed to the soft sleep when it is on; Stopwatch and Clocks redrawn from the RTC in P18's fresh-first-frame list |
| **W2-T Sleep Timer to sleep** | firmware-platform (sonnet) | `powermgmt.c` `handle_sleep_timer()`: under `HAVE_IPOD_UI`, pause, then `button_queue_post(SYS_SLEEP_REQUEST, SLEEP_TIMER)` instead of `sys_poweroff()` (the charger/USB branch unchanged; P18's tier rule and `alarm_blocks_poweroff()` decide soft, dark wait or off); ledger row. A second hunk after W1-A1's (H2) |
| **W2-D Device round** | orchestrator + owner | §5.4. D-A1..3, A5..A8 and D-M1..M3 can run before P18 closes if the owner wants: they use today's power-off |

### Wave 3: parallel-safe now, scheduled after wave 1 for capacity
| Package | Agent | Owns |
|---|---|---|
| **W3-K Contacts** | ui-fidelity (sonnet); parser by playback-audiobook (sonnet) | vCard records in `xindex_core.c` (its own functions), `screens/contacts.c` (module), the detail view; Settings › Sort By drives the order |
| **W3-C Calendars** | playback-audiobook (**opus** for the `.ics`/`RRULE` expander); ui-fidelity (sonnet) for the month grid, day list, To Do's | `xical_core.{c,h}`, `screens/calendars.c` (module). Calendar event alarms feed `alarm_next()`: one extra source, an `alarm_core.c` function owned by W1-A1, added via SendMessage |

### Wave 4: the SDK, then Games (after the owner answers G-1..G-8 / Q15, Q19, Q20, Q23–Q25, Q29, Q30)
- **W4-0 SDK rev 0** (opus; design doc + contract headers, reviewed by `budget-reviewer` before any game code):
  - `docs/design/sdk.md` from §2A;
  - `apps/ipodui/sdk/sdk.h`, `sdk.sym`, the resident SDK table and checks;
  - the watchdog prototype (§2A.7: PC and thread test, the restore point, the T bit, the flag path, the controlled-reboot fallback);
  - `sdk_badgame` and its scripts (§2A.8);
  - the `modpak.py` SDK-kind checks and the `.lang` name tables.

  Resident cost target: ≤ 6 KB text, ≤ 256 B statics (§10).
- **W4-1..3:** one package per game (Solitaire, Bricks, Parachute, shipped as **Chute**: owner renames Parachute then Airdrop, 2026-10-24), as SDK clients, Class A, `min_ram = 32`.
  - The game kit of `games.md` §2 becomes the SDK's drawing/input layer, not a library linked into each module.
  - The resident Games list (§3.2) lists `mod_runnable()` modules only.
  - Each game uses **only** `sdk.h`. A need the SDK lacks is an SDK change (W4-0's owner), never a direct core call.
  - `games.dat` becomes each game's private file (§2A.5).

### Wave 5
Custom EQ curves (#82), and alarm sounds through the headphones (Q3). Default: P12b (Q14).

### Shared files
| File | Owner | Rule |
|---|---|---|
| `apps/SOURCES` | orchestrator merges | additions only, inside the `HAVE_IPOD_UI` block; one line per new file, sorted within P12's group |
| `apps/lang/english.lang` | W0 (the reserved block) | agents edit the text of their own ids only; P12's block stays last (§3.6) |
| `menu.c`, `screens/panels.c` (new Extras tiles only), `modload*`, `modpak.py`, `tests/modload` | W1-F | others ask via the orchestrator |
| `ui_main.c` | P18's owner; **W1-A1's hook commit 1** in wave 1; W2-W in wave 2 | each P12 change is one reviewed commit |
| `input.c` | P18's owner; **W1-L's hook commit 2** | as above |
| `sleep.c` | P18's owner; W2-W in wave 2 | – |
| `powermgmt.c` | **W1-A1 (H2 hunk) first**, then W2-T (Sleep Timer hunk) | guarded edits, ledger rows, sequential |
| `rtc_pcf50605.c`, `bootbatt.c` (one call), `usbscreen.c` (one call), `screens/reset.c` (one call) | W1-A1 | guarded where upstream; ledger rows |
| `bootloader/ipod.c` | W1-B | gated on D-A2 + owner sign-off |
| `settings_list.c`, `settings.h` | W1-L | guarded, appended settings, ledger row |
| `screens/datetime.c` | W1-C (the picker move) | one commit; Date & Time scripts re-run |
| `maint.c` | P18's agent B reviews; **W1-N's one commit** | – |
| `sim_script.c` (registration line), `shots.sh`, `check_trace.py`, `tests/ui/README.md` | W1-T | others ask via the orchestrator |
| `docs/hardware.md` budget table, ledgers | orchestrator at close | agents report numbers |

## 7. Out of scope
- Videos and Photos (P14); Voice Memos (no microphone path).
- Class B and C **implementations** (only the ABI and the deny lists ship): #108, P19, P14, P7.
- A playlist or headphone-tone alarm sound (Q3), unless the owner moves it in.
- Text entry of any kind (alarm labels come from a fixed list; Notes are read-only, as stock).
- Calendar editing; CalDAV/CardDAV or any sync service (the companion app is P21).
- Rockbox's alarm menu, and plugins (`--ipodui-plugins` stays off).

## 8. Device-reality checklist

| # | Item | Answer |
|---|---|---|
| 1 | Compiled features | Modules develop and gate with `-DIPODUI_WITH_MODULES`; the release default flips only after D-M1 (H5). The sim links modules statically, so **the pack read, relocations and the class checks run only on the target**: `modpak.py check` in the gate covers the static part, and D-M1..M3 check the load. `HAVE_RTC_ALARM` (target) and `HAVE_POWER_SLEEP` (P18): the sim's `alarm_boot`, the PMU stub, `rtc_set` and `sleep_warp` stand in; the PMU wake and the bootloader window are device-only (D-A1..8). Stock build: nothing (all under `HAVE_IPOD_UI`; the bootloader edit is guarded for the shell's bootloader build) |
| 2 | Memory during playback | Class A allocates **nothing** from buflib: image, `.bss` and working block live in the arena reserved at shell start (in the floor gate). The pack index is read into the arena's reserved top 2 KB, never onto a stack (C1). Resident statics (§10): `mod_rt` 640 B (replacing 288 B `hdr` + 112 B of slots); alarms 8 × 24 + 16 = 208 B; stopwatch 144 B; lock 4 B. The indexer's build block is a `maint_take()` **only while stopped** (post-USB moment, boot window), so nothing is taken while playing; its space goes back through the existing regrow at the take's end (P16's rule). Class B's borrow (later) uses #108 |
| 3 | CPU at 50k | No Extra reads the library. Calendars' expansion runs at sync time: est. 2,000 events over 13 months ≤ 50 ms on the device, bounded by `XCAL_MAX_INSTANCES` 2,048 (logged when cut). Contacts: 2,000 cards ≈ 400 KB of vCard parsed at sync ≤ 1 s [est], yielding every 64 cards. Clocks: one redraw a minute of ≤ 6 bands |
| 4 | Storage | Per user action: opening an Extra = 0 reads (the module preloaded on highlight, flash) or 1; Notes/Contacts/Calendars + 1 range read of the index; a note + 1. Steady state: 0 (the accountant). Writes: `alarms.dat` at each edit/ring (one safefile replace; **deferred during a USB session**, the PMU armed at once); `stopwatch.dat` at Stop/New Timer; the lock settings via `settings_save()`; indexes at sync only. **Never at USB entry.** A trusted clean boot skips the index signature check (no directory read). The PMU arm is I2C, not storage. The sim's file counters model the device's (one `read()` per range) |
| 5 | LCD | Full-width bands only. Stopwatch: the digits band (~320 × 48) at **10 fps** unboosted (tenths, as the 7G; ~31 KB/frame, ~5.6 ms push at 30 MHz [est from `clocks-power.md` §8]). Clocks: one band a minute. Ring screen: the label band blinks at 1 Hz. Games ≤ 25 KB/frame average (`games.md`). Frame budget ~300k instructions boosted per frame (lean rule 8), reported per animated screen from the ELF harness |
| 6 | Threads and locks | No new thread. The alarm engine, ring screen, stopwatch and lock run on the UI thread at its deadlines; the piezo thread exists already. The indexer runs on the existing storage worker **with its module pinned** (`mod_pin`, M3) and inside a stopped-state take; its result is published by a sequence number the UI reads (identity vs validity: the index's `sig` is identity, the `crc` validity). The PMU writes (`pcf50605_write*`) take the I2C mutex from the UI thread, never inside `power_sleep()` (P18's raw path is separate). The power thread now only reads INT1 (H2), so it never writes the alarm registers. The SDK watchdog unwinds only the UI thread inside module code (H3) |
| 7 | Upstream splashes | `apps/alarm_menu.c`, `root_menu.c`'s alarm boot path and `time_menu.c` are not reached by the shell (no call site under `HAVE_IPOD_UI`; checked by `grep` in the review). `handle_sleep_timer()`'s `sys_poweroff()` shows no splash in the shell build (`show_shutdown_message`). Errors are logged; the module failure page is ours; a missed alarm is a list state, not a splash |
| 8 | Persistence | `alarms.dat` (with header, CRC and `armed_rtc`): written by the UI thread at edit exit and in `alarm_rearm()`, so a power cut after an edit keeps it; the PMU is armed in the same call and read back. **A power-off before `alarm_init()`** keeps RTCWAK via `rtc_init()` and the boot guard (H6). A power cut mid-ring: the alarm stays armed (`AR_ONCE` switches off only after Dismiss) and **rings again at boot if still within `ALARM_BOOT_WIN_S`**, else it is marked missed and shown inline. Snooze survives a power-off (`snooze_until` persisted and armed). USB mid-edit: the editor's exit write is deferred to the post-USB path; the PMU is armed at once. `stopwatch.dat` at Stop; a running stopwatch at power-off is stopped and logged by a `SYS_POWEROFF` subscriber (one write in `clean_shutdown()`'s window). Screen Lock: `ipodui_locked` is saved at Lock (a settings write at once), so a power cut keeps the lock. A corrupt `alarms.dat` reads as no alarms, logged; the PMU keeps what it held until the next arm. One script per trigger (§9) |
| 9 | Scale | 50k tracks are irrelevant; the scale is 6 clocks, 8 alarms, 20 stopwatch logs, 1,000 notes, 2,000 contacts, 2,000 events (fixtures in `tests/xindex`; sim timings with `tests/perf/`-style scripts) |
| 10 | Deferred work | The index build waits for the post-USB moment or the boot window, stopped. States: *fresh* (sig equal, or a trusted clean boot: skipped); *dirty* (sig differs: rebuilt); *missing/corrupt* (rebuilt; meanwhile the screen shows the stock empty state, e.g. Notes with no rows, never a spinner); *owed* (the take refused: the last good index stays). An alarm due while the indexer runs: the ring wins (the job stops at its next record boundary). An alarm due during the sleep-entry window: entry refused (§5.1). During USB: Q7, the write deferred. During maintenance (P16's paused window): the ring pushes over the window's screen, and the window's job keeps its own safe points. During video (Class C): the ring suspends the session (§2A.4). During a Class A Extra: the ring pushes on top, the Extra resumes after |
| 11 | Stock firmware unchanged | Guarded edits only (`rtc_pcf50605.c`, `powermgmt.c`, `settings_list.c`, `bootloader/ipod.c`); stock `.text` byte-identical with `english.lang` held equal |
| 12 | Existing paths doing the same job | **Alarms:** upstream's `alarm_menu.c`/`root_menu.c` alarm boot is not reached. `rtc_check_alarm_started()` is still called by `rtc_init()` and is harmless; ours decides with `alarm_is_boot()`. **`power_thread_rtc_process()`** (`powermgmt.c` :541–546) disarmed the alarm on every step; under the shell it only reads INT1 (H2). **Sleep timer:** upstream's engine is kept; its UI (`time_menu.c`, quickscreen) is unreachable; `reset_poweroff_timer()`'s `sleeptimer_key_restarts` stays off; **`sleeptimer_on_startup`** (`settings.c` :980) is hidden (developer only); the timer is cancelled at sleep entry (§5.6). **Idle power-off/sleep:** P18's `handle_auto_poweroff()` reroute, plus the dark wait (Q1). **Combination entry:** `vlimit.c` and Screen Lock share `roller.c`, with separate codes (Q11). **City picker:** one copy in `tzpick`, used by Date & Time and Clocks. **Clock drawing:** the Extras panel (`screens/panels.c`) and Clocks share the map and face renderers (one copy, resident). **Hold PLAY while locked or ringing:** `global_hold()` (P18) still sleeps; the ring screen dismisses first (Q5). **Frozen-kernel state (P18):** stopwatch and ring times come from the RTC, never `current_tick` across a sleep. **Every writer of the PMU alarm registers:** `alarm_rearm()`, and the keep-alive's RTCWAK/daily conversion. Apple's firmware also writes them; `alarm_init()` reconciles with `armed_rtc` |
| 13 | Builder predicate | The indexer rebuilds a folder iff its `sig` (FNV-1a over the sorted (name, size, mtime) of the folder's direct entries with the kind's extensions, case-insensitive) differs from the index's, or the index fails `xidx_check()`. A trusted clean boot (P16 boot stamp) skips the check. Writers of `sig`: the indexer only. Subfolders: Notes and Calendars one level deep (stock), Contacts flat |

Items only the device can verify go to `docs/ROADMAP.md` → Device checklist: D-A1..8, D-M1..3, D-S1 (optional); module load latency for each Extra while playing (flash; HDD awake and asleep, with #104's spin-up) with 0 underruns; the stopwatch band's push time at 30 and 24 MHz; the piezo pattern's feel; the lock surviving a battery pull.

## 9. Test plan

**Host tests (`scripts/test.sh`):**
- `tests/alarm/` (§3.3, including the corrupt file and the missed-then-repeat rows);
- `tests/stopwatch/` (elapsed across a simulated sleep: RTC +3,600 s with the tick frozen; laps; the log ring);
- `tests/xindex/` (§3.5; the parsers in wave 3);
- `tests/lock/` (the code state machine: new, confirm mismatch, lock, wrong code, right code, USB unlock, persisted state);
- `tests/modpak/` (class deny lists and size rules against fixture objects);
- `tests/modload/` (the v2 fixtures, §3.1).

**Sim scripts** (`tests/ui/ipodui-p12-*.script`; time and battery pinned; goldens proposed, not written):
- **Framework:**
  - `-extras-list`: order, split/full per item, each panel;
  - `-extras-mm-items`: each Main Menu Extras item opens its screen;
  - `-mod-swap`: stand-in → real with `mod_delay`;
  - `-mod-missing`: `mod_delay -1` gives "Not Available";
  - `-mod-missing-alarm`: no pack, an alarm 2 min ahead: the resident ring still rings, the editor shows the styled page;
  - `-mod-hidden`: a 64 MB fixture module on a 32 MB sim build is absent from the Games list;
  - `-modclass`: each Extra opened while playing logs `modclass` lines, no `viol`, `reads=0` in steady state.
- **Alarms:**
  - `-alarm-create`: every row of Q2;
  - `-alarm-ring-awake`: `rtc_set` to the minute; ring screen, `piezo play` lines, music paused;
  - `-alarm-snooze`: rings again 9 min later; the fourth snooze is refused;
  - `-alarm-dismiss`;
  - `-alarm-keys`: SELECT/PLAY/MENU act at release, Hold PLAY dismisses and sleeps;
  - `-alarm-cap`: 5 min, then snooze or stop and marked missed;
  - `-alarm-boot`: `alarm_boot 30`; boot glyph then ring; boot window deferred;
  - `-alarm-boot-late`: `alarm_boot 200`; no ring, missed shown inline;
  - `-alarm-missed-repeat`: a missed weekday alarm, then the next weekday rings;
  - `-alarm-weekdays`: Saturday no ring; Monday rings;
  - `-alarm-time-change`: Date & Time moves the clock past it; rearm, no phantom ring;
  - **`-alarm-disarm-race`** (H2): fires awake → Snooze → power-off; the PMU stub still holds the snooze with RTCWAK;
  - **`-alarm-early-poweroff`**: a boot-guard refusal before `alarm_init()` keeps RTCWAK in the stub;
  - **`-alarm-snooze-poweroff`**: snooze, power off, `alarm_boot`: rings;
  - **`-alarm-usb`**: a ring during a USB session; piezo only; no file write until `usb_disconnect`, then one write;
  - **`-alarm-locked`**: rings over the lock screen; after Dismiss, the lock screen again;
  - **`-alarm-in-extra`**: rings during Stopwatch; Stopwatch resumes, still counting;
  - **`-alarm-in-video`**: rings during video playback (`HAVE_BCMFW_VIDEO` sim build); the session suspends;
  - **`-alarm-in-maint`**: rings during P16's paused window; the job reaches a safe point and the ring is not delayed;
  - **`-alarm-corrupt`**: a damaged `alarms.dat` reads as no alarms, logged, no splash;
  - `-alarm-hold`: Hold on; rings, keys blocked, the cap ends it;
  - **`-alarm-darkwait`**: an alarm armed, hold PLAY; dark wait instead of power-off; a key wakes; the alarm rings in the dark wait;
  - `-alarm-persist`: power off and boot; the alarm is still listed and armed.
- **Sleep Timer:**
  - `-sleeptimer-set`: values, the row's subtitle;
  - `-sleeptimer-end`: soft sleep off; today's power-off path, or the dark wait with an alarm armed;
  - `-sleeptimer-charger`.
- **Wave 2 (after P18):**
  - `-alarm-soft-sleep`: `sleep_warp` to an alarm 90 s ahead; `sleep wake why=ALARM` on time; ring screen;
  - `-alarm-deep`: sleep, deep tier, then `alarm_boot`;
  - `-alarm-entry-near`: sleep refused within the window;
  - **`-sleeptimer-soft-sleep`**: a timer set, a soft sleep, a 2 h `sleep_warp`; on wake the timer is Inactive and nothing fires;
  - `-lock-wake`: a locked unit wakes to the locked screen;
  - `-stopwatch-sleep`: running across a 1 h `sleep_warp`: +3,600 s;
  - `-clocks-reveal`: the reveal's frames against the measured motion.
- **Clocks:** `-clocks-one`, `-clocks-add` (region → city), `-clocks-three`, `-clocks-night`, `-clocks-dst` (a city in DST), `-clocks-delete`; the Date & Time scripts re-run after the picker move.
- **Stopwatch:**
  - `-stopwatch-run`: start, laps, pause, resume, stop; the log row shows M/D/YY and the time in blue;
  - `-stopwatch-away`: leave while running, come back: still running;
  - `-stopwatch-clear`.
- **Screen Lock:** `-lock-set` (new, confirm, mismatch), `-lock-engage`, `-lock-wrong`, `-lock-right`, `-lock-usb` (a data host unlocks), `-lock-reboot` (still locked).
- **Notes:**
  - `-notes-empty`;
  - `-notes-list` (a fixture folder);
  - `-notes-view` (paging a 4 KB note);
  - `-notes-sync`: add a note over the sim's USB; rebuilt at the post-USB moment (`xidx build` trace), 0 reads while browsing;
  - `-notes-boot-trusted`: a clean boot skips the signature check (no directory read in the trace).
- **SDK (wave 4):** `-sdk-badgame-*` (§2A.8) and **`-sdk-exit-chord`**: a SELECT + PREV overlap under 2 s (0.3 s, 1.9 s, both press orders) does not exit and leaks no event; 2.0 s exits with the game's exit policy honoured (graceful save lands; immediate reclaimed at once).
- **Every Extra:** `-<extra>-while-playing` (the position advances, 0 underruns, a `floor` receipt above the floor).

**Trace rules** (`check_trace.py`, owner T). A rule that finds none of its lines fails.
- `modclass` present, and `modclass viol` absent;
- `alarm ring` exactly once per scripted ring;
- `alarm arm rtc= ok=1` after every edit;
- `xidx build kind= n= ms=` at sync, never while browsing;
- `piezo play` only while `alarm ringing`;
- `mod job_open stk=` present at pack open, in target logs (D-M3).

**Review images and videos:**
- side-by-sides with the owner photos: Extras list photo 1191/1197, Alarms photo 1198, Stopwatch photo 1202, Screen Lock photo 1201, Notes photo 1200, Games photo 1199, Clocks photo 1193, Calendars photo 1196 and the month grid;
- a sim video of the Clocks reveal against `VIDEO-2.md` §A3;
- a device video of an alarm ringing (D-A5).

## 10. Budgets

| Item | Budget | Basis |
|---|---|---|
| Resident `.text` (release, `fw-rel`, modules on) | **≤ +20 KB** net over P18's close: alarm engine + ring + dark wait + keep-alive ~6.5 KB; lock gate + locked screen ~2.5 KB; stopwatch state ~0.5 KB; resident Extras and Games lists ~3 KB; `tzpick` + DST ~3 KB; registry, class checks, `mod_work_alloc`, `mod_pin`, compact index ~1.5 KB; `rtc_*` additions ~0.5 KB; the Settings tenants' −0.7 KB (measured) | [est]; reported per object at integration |
| Resident statics | **≤ +0.8 KB net**. `mod_rt[16]` 640 B (W0 measured layout; 576 B if W1-F narrows it) **replaces** the 288 B `hdr` and ~112 B of release slots (net +240 B; one table over 256 B, with its "can't share because" in §3.1); alarms 208 B (`struct alarm` is 24 B: kept for clarity, owner/orchestrator 2026-10-23); stopwatch 144 B; lock 4 B (sum +596 B); DST/tz tables are rodata. Developer builds add the load statistics table (16 × 32 B) | M1 (rev 2) |
| `MOD_ARENA_SIZE` (release) | **40 KB** for waves 1–3: the largest single tenant, Calendars ~18 KB image + 4 KB work + `.bss` [est], + 2 KB index stage, + 25 %. **72 KB** when games land (`games.md`: ≤ 56 KB per game + 2 KB, + 25 %). No list is held under a child (§3.2). Developer: + the devloader's 4.5 KB | M2 (rev 2); `modpak.py` prints the real figure, which the floor gate uses |
| Audio-buffer cost | Release: **−32 KB** (8 → 40 KB arena), then **−64 KB** with games; plus resident growth ≤ 21 KB | Against 4.11 MB (32 MB) / 5.44 MB (64 MB) of headroom at the P16 close (`hardware.md`) |
| Storage thread stack | `job_open()` frame ≤ 256 B (no index on the stack); `stk=%` at pack open under the existing limit, reported | C1 (rev 2), D-M3 |
| `_end` | stays ≤ 1,500,000 B backstop; report the `inquiry_static` 4 KiB step if crossed (#97) | |
| `rockbox.ipod` | ≤ 1,250,000 B backstop; modules move code out | |
| Bootloader | W1-B ≤ +0.5 KB; the bootloader's own size limit unchanged | |
| On disk (hidden folder) | `modules.pak` +~120 KB (baseline Extras) +~90 KB (games); snapshots for Clocks and each game ~150 KB each; indexes ≤ 64 KB each; `panels.pak` + the Extras/Games tiles | |
| Per-frame | Stopwatch ≤ 120k instructions unboosted per frame at 10 fps; ring screen ≤ 30k per blink | lean rule 8, ELF harness |
| Wakes (P18 18-A table) | Dark: the alarm engine has 0 periodic wakes (one deadline at the next ring); a running stopwatch while dark: 0 (it redraws only when lit); the dark wait: 0 besides the alarm deadline | |
| SDK (wave 4) | resident ≤ 6 KB text (table, checks, watchdog, file confinement), ≤ 256 B statics; the SDK table ~120 entries × 4 B | [est] |
| Battery | An armed alarm costs nothing awake or in soft sleep beyond the deadline. The dark wait (Q1(2)) costs dark-awake current for up to 24 h (D-A8). A ring is ≤ 5 min of piezo + backlight (D-A5 measures the slope) | |

## 11. Acceptance
- **Every wave-1 package:**
  - its scripts and host tests pass;
  - `modpak.py check` passes with modules on;
  - no `modclass viol`;
  - no new warnings, no splash, no "Rockbox" string, and no hidden-folder name on screen;
  - no static > 1 KB, and `mod_rt` as budgeted;
  - `cpu_boost` balanced;
  - the stock build byte-identical (checklist 11).

  The ring and lock scripts run in wave 1, on the two reviewed hook commits (§6).
- **The storage thread's `stk=%` at pack open** (D-M3) is within its stack on the device, and the sim's frame check shows no index on the stack.
- **The modules-by-default flip** happens only after D-M1 and D-M2 pass.
- **Visual match:** the Extras list, Alarms, Stopwatch, Screen Lock, Notes and the Games list match the owner photos within the 8b tolerances (side-by-sides attached); Clocks' reveal matches the measured motion.
- **Alarms:**
  - they ring awake, after a soft sleep (wave 2), and after a power-off (D-A1);
  - **with Hold on, after W1-B (D-A2, D-A7)**;
  - never on a wrong day (D-A3, or the daily fallback);
  - **never silently lost:** every missed path in §5.5 is scripted and shows inline;
  - the piezo pattern is felt on the haptic (D-A5).
- **Budgets:** the floor gate passes with the real arena size, and the budget rows of §10 are measured and recorded at close.
- **Ledgers:**
  - `changes-from-rockbox.md` rows for `rtc_pcf50605.c`, `powermgmt.c` (both hunks), `bootloader/ipod.c`, `settings_list.c`, and the module pack v2;
  - `changes-from-7g.md` rows for Q1 (Hold at an alarm boot; the dark wait), Q2's owner additions (Custom days, Snooze), Q3 (Beep only), Q7, Q10 (USB unlock by any computer), Q17 (missed shown inline), Q27 (DST in Clocks), our game names, our labels.

## 12. Risks
- **The RTC alarm may not power the unit on** (D-A1). Then the deep case has no wake: alarms work only awake, in the dark wait and inside soft sleep, and soft sleep's default becomes the owner's decision with this in the balance. EXT-1's "best effort" is the documented fallback.
- **Hold on at an alarm boot starts Apple's firmware** (D-A2, Q1). The dark wait covers it until W1-B; W1-B needs the owner's sign-off.
- **The dark wait's drain** (D-A8, Q28) may be too high for a 24 h window; the owner then picks between the drain and the Hold limit.
- **A one-shot PMU match may be unsupported** (D-A3). The daily fallback costs a silent boot-and-off, with the boot glyph, on non-ring days.
- **The watchdog's unwind** (H3) is a prototype risk; its fallbacks (the flag, the controlled reboot) are defined, and third-party games wait for signing anyway.
- **Turning modules on by default** changes the release build for every later phase. It waits for D-M1; the floor gate and the `_end` backstop catch the memory side.
- **Calendars' `RRULE`** is the largest parser. It is bounded by the expansion cap and fixtures, and can ship without recurrence first if the end review finds risk (a reduced scope, logged in the 7G ledger).
- **`ui_main.c`/`input.c` are P18's busiest files**: P12 touches them only through two reviewed hook commits in wave 1 and W2-W's work in wave 2.

## Revisions
- **Revision 1 (2026-10-23):** the first draft. It folded in the owner's direction on Extras resource classes (§2, the class ABI in §3.1 and its checks in §2.3), the module header's class, minimum RAM variant and ABI version with hidden-if-unrunnable (§2.2, §3.1), and the SDK rev 0 (§2A). It recorded the owner's Q21/Q22 decisions, and added the P20 ROADMAP line for the public SDK docs.
- **Revision 2 (2026-10-23):** the `budget-reviewer` brief review, every finding applied (table below). Folded in before commit: the owner's decision on M5 and the game input model (§2A.4: the 2 s SELECT + PREV exit chord, B/C owning MENU instantly, PLAY tap-only, the wheel's relative/absolute modes with the driver's evidence and D-W1; Q26 decided; Q29 and Q30 new) and the exit-chord test (§2A.8).

- **W0 notes (2026-10-23, the contract commit):** measured layouts (`struct alarm` 24 B, `mod_rt` 40 B, `sw_state` 144 B, §3.3/§3.4/§10), two correctness fixes (the missed interval `[armed_rtc, now]`, §3.3; the other-city DST formula, §1/`tzpick.h`), the contract additions, the resident-build `mod_work_alloc()` rule and the wave-1 merge order (§6, W0).

### Revision 2: review findings → resolution
| Finding | Resolution | Where |
|---|---|---|
| **C1** `MODPAK_MAX` 32 puts a 1,440 B header on the storage thread's stack (`modload.c` :133) | `MODPAK_MAX` 16; the index is read in one read into the arena's reserved top 2 KB (`MOD_IDX_STAGE`) and compacted into `mod_rt[]`; `stk=%` is in acceptance and D-M3 | §3.1, §5.4, §10, §11 |
| **H1** Hold on at an alarm power-on boots Apple's firmware | W1-B (the bootloader alarm window) is in P12, gated on D-A2 and the owner's sign-off; until it lands, the dark wait (no deep power-off with an alarm due within 24 h); 7G ledger row; Q1 changed, Q28 new | §0.1, §5.2, §6, §11 |
| **H2** `power_thread_rtc_process()` disarms every step | Under `HAVE_IPOD_UI` it reads INT1 only; `alarm_rearm()` alone arms; ledger row; sim PMU stub; `-alarm-disarm-race`; checklist 12 fixed | §1, §3.3, §3.6, §8, §9 |
| **H3** watchdog unwind | Unwind only on the UI thread with the PC in the module; otherwise a kill flag honoured at the next SDK boundary; the slice clock stops inside SDK calls; a `setjmp`-style restore point; the T bit; the controlled-reboot fallback; the P19 COP note | §2A.7 |
| **H4** branch from `p18-114`; shared files; hooks | `p12` from `p18-114`; refs pinned to d328f1ef86; owners and rules for `english.lang`, `SOURCES`, `maint.c`, `sim_script.c`, `powermgmt.c` and more; **recommended: one reviewed hook commit each** (W1-A1 `ui_main.c`, W1-L `input.c`), so the ring and lock scripts stay in wave 1 | header, §6, §11 |
| **H5** loader device checks before the default flip | D-M1 (the P16 checks) as a precondition; D-M2 (pack missing with an alarm armed); `-mod-missing-alarm` | §3.1, §5.4, §9, §11 |
| **H6** never silently lose an alarm | Missed alarms shown inline (Q17 changed); RTCWAK kept at `rtc_init()`; the boot guard's disk-free keep-alive; daily vs exact weighed, exact recommended with the daily safety net | §3.3, §5.2, §5.5 |
| **M1** statics | One compact `mod_rt` table (576 B) replaces `hdr` and the release slot stats; §10 corrected | §3.1, §10 |
| **M2** arena math | The Extras and Games lists are resident, with panel tiles in `panels.pak`; arena 40 KB, then 72 KB; forward notes in `module-loader.md` and `hardware.md` :119 | §2.4, §3.2, §10 |
| **M3** indexer on the storage worker | `mod_pin`/`mod_unpin`; the build inside a stopped-state `maint_take()`; Calendars capped at 2,048 × 20 B within the 64 KB file cap | §3.1, §3.5, §8 |
| **M4** Class B borrow | Defined on #108's shrink-without-stop; no `MAINT_EXTRA` added | §2.1, §3.1 |
| **M5** Class B and Hold PLAY; release timing | **Decided by the owner (Q26), with the game input model:** Hold PLAY, Hold SELECT + MENU (the hardware reset), the 2 s SELECT + PREV exit chord and the Hold switch are the firmware's in every class; Hold PLAY runs the exit path first. B/C get MENU fully and instantly; PLAY is tap-only at release. Class A keeps short MENU = back and, by default, long MENU = exit (Q29). Wheel relative/absolute by class (Q30, D-W1) | §0.1, §2A.4 |
| **M6** Class A volume wording | Scoped: shell Extras keep the shell's wheel-volume rule; SDK Class A games never change volume | §2.1 |
| **M7** the picker in the Settings module; DST | `tzpick.{c,h}` resident and shared, size counted; a DST rule table (Q27) or a ledger row | §1, §3.6, §10 |
| **M8** MMAP wording | No claim of protecting `.text`; uncached aliases and overlapping windows bypass it; PP open question 2 cited | §2A.6 |
| **M9** `alarms.dat` | A header (magic, version, CRC), a persisted `armed_rtc`, the PMU armed and the file deferred during USB, the ring-in-USB owner (W1-A1), the read-back after arming, the weekday register ignored | §0.1 Q7, §3.3 |
| **M10** Sleep Timer in soft sleep | Cancelled at sleep entry; `sleeptimer_on_startup` in checklist 12 | §1, §5.6, §8 |
| **M11** Q2 rows vs the 7G | Likely stock rows listed; C2 is a blocker for W1-A2's labels; Custom days and Snooze recorded as owner additions | §0.1, §0.2, §6, §11 |
| **M12** factual fixes | The Extras panel is `screens/panels.c` :832–1031; `apps/ipodui/simclock.c`; `usb_connect`/`usb_disconnect`; P18's `rtc_set`/`sleep_warp` reused, no `rtc_warp`; owners for `settings_list.c`/`settings.h`, Reset Settings and `tests/modload` | §1, §3.3, §3.6, §6 |
| **Low** PLAY on release vs hold while ringing | Defined (Q5) | §0.1 |
| **Low** snooze cycles | Capped at 3 (`ALARM_SNOOZE_MAX`) | §0.1, §3.3 |
| **Low** stopwatch rate | 10 fps, tenths | §3.4, §8, §10 |
| **Low** game names | Generated from `.lang` sources | §2A.1 |
| **Low** year `0x99` | Kept as [hypothesis], D-A3 decisive | §5.2, §5.4 |
| **Low** index signature at boot | Skipped on a boot the P16 boot stamp trusts | §3.5, §8 |
| **Tests and D-rows** | Every listed test added (the disarm race, an early power-off, missed then the next repeat, a ring during USB with the write deferred, during a lock, a Class A Extra, video and maintenance, the sleep timer across soft sleep, snooze across a power-off, a corrupt file); D-A7, D-M2, D-M3 added | §5.4, §9 |
