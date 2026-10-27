# Simulator UI scripts

Run with `scripts/shots.sh tests/ui/<name>.script [outdir]` (PNGs default to
`out/shots/<name>/`). Format: see `uisimulator/common/sim_script.c`.

To run every script as part of the phase gate, use `scripts/gate-run`
(see `docs/process.md` "Gate"), not `scripts/validate.sh` directly: it
holds a per-worktree lock (a second gate in the same worktree refuses
at once) and a two-run `--quick` budget per task (a third refuses;
`GATE_FORCE=1` overrides it for the orchestrator). No args defaults to
`--quick --changed`; `--full` runs the plain full gate.

| Script name   | iPod button      | SDL key   |
|---------------|------------------|-----------|
| `MENU`        | Menu             | Escape    |
| `PLAY`        | Play/Pause       | Space     |
| `LEFT`/`PREV` | Previous         | Left      |
| `RIGHT`/`NEXT`| Next             | Right     |
| `SELECT`      | Center button    | Return    |
| `SCROLL_FWD`  | Wheel clockwise  | Down      |
| `SCROLL_BACK` | Wheel counter-cw | Up        |
| `HOLD`        | Hold switch (toggles) | h    |

Any SDL key name (`F5`, `Keypad 5`, ...) also works. `wheel +n` sends n
`SCROLL_FWD` clicks, `wheel -n` sends n `SCROLL_BACK` clicks.

## Commands

See the header of `uisimulator/common/sim_script.c` for the full list:
`wait`, `key`, `hold`, `wheel`, `shot`, `quit`, `wait_db`, `build_db`, and

    chord <A>+<B>[+...] <ms>

which presses the keys one after another, 50 ms apart (the way fingers land on
a real iPod, so the app first sees the single button, then the combined
bits), holds them all for `ms`, then releases them in reverse order. Example:
`chord SELECT+PREV 3200` (the devmode combo), `chord MENU+SELECT 6000`.
Up to 4 keys; names as in the table above.

    key <K> [ms]      (and hold <K> <ms>)

presses `K` for `ms` (default 100). The release is a tick task's at the
press's tick + `ms`, not the script thread's: the driver sees the key down
for the same number of ticks on any host, so a loaded host cannot turn a
short press into a long one (button.c repeats after 300 ms, the shell's
long press at 500 ms; the gate's pool once opened the main menu's
long-press menu that way). The next command runs after the release.
While the sim is asleep (Phase 18) the press goes to the sleep as before.

    wheel <+n|-n> [ms]

sends one click every `ms` (default 40, 25 clicks/s; minimum 10). Use
`wheel +5 300` for slow single-row steps that wheel acceleration can't
change, `wheel +120 10` for a fast spin (quick-scroll letter).

    wait_db [timeout_ms]
    wait_commit [timeout_ms]   (a commit ran and ended since the last wait_commit or
                                the boot: a latch on tagcache.c's commit counter, so
                                none is missed; lands before its publication)
    build_db

    update_db
    update_db_wait [timeout_ms]
    update_db_wait_change [timeout_ms]
    db_mark
    expect_db_grew [timeout_ms]
    expect_db_same <ms>
    import_changelog
    expect_resume <path> [min_ms] [wait_ms]
    expect_playing <ms>
    rebuild_db_start
    scan_slow <ticks>
    dirty_db
    expect_lib_updating <ms>
    copy_file <src> <dst>
    copy <hostsrc> <dst>
    rm_file <path>
    remove <path>

`update_db` starts an incremental database update (`tagcache_update()`:
scan, commit, RAM cache reload, what the background update does) and
returns at once. `update_db_wait` does the same and then blocks until the
reloaded database is usable, logging `update_db_wait ready after N ms`; a
timeout logs `update_db_wait timed out` and fails the run.
`update_db_wait_change` starts an update and waits until the database is
usable with a new generation (a commit and reload really happened), logging
`update_db_wait_change ready after N ms`, else `update_db_wait_change
failed` (fails the run). `db_mark` and `expect_db_grew` check that a commit
landed; `expect_db_same <ms>` checks that the entry count stays at the
`db_mark` value for `ms` (no commit landed), logging `expect_db_same ok`
or `failed`. `import_changelog` imports the runtime-stats changelog
(`sim_db_import()`, what the stock "Import Modifications" does) and logs
`import_changelog done|nothing in N ms`. `expect_resume <path> [min_ms]
[wait_ms]` waits up to `wait_ms` (default 5000) for the track's stored
resume position (`lastelapsed`) to reach `min_ms` (default 1) and logs
`expect_resume ok`, else `failed` (-1: none, -2: not in the database); `expect_playing` samples the
audio status every 10 ms and fails on any stop; `expect_paused <ms>` the
same for "paused" (play + pause bits). `copy_file` copies a file
inside the sim disk (fresh mtime); `copy` copies a host file (path relative
to the script's directory, e.g. `fixtures/refresh.mp3`) to a sim path (its
directory must exist); `rm_file` (alias `remove`) deletes a sim file. Sim
paths write a space as `%20`. Together they add or drop a track while the
UI runs (`ipodui-p4-refresh.script`, `ipodui-p4-commit-playing.script`). A
failed `expect_*` fails the run in `scripts/shots.sh`. `rebuild_db_start`
starts a rebuild and returns at once (`rebuild_db` waits for it);
`scan_slow <ticks>` makes every later scan sleep `<ticks>` at each
directory it walks (0: off), so the test library's update lasts seconds
(the header's busy glyph, 3.20: `ipodui-busyglyph-*.script`);
`dirty_db` marks the database on disk dirty, as a power cut in the middle
of a commit leaves it (follow it with `reboot`; "dirty_db failed" fails
the run); `expect_lib_updating <ms>` checks that the shell's Music menu
says "Updating Library..." (`lib_state()` is `LIB_UPDATING`) all through
`ms`.

`wait_db` blocks until the database is usable (`tagcache_is_usable()` and no
commit running; default timeout 30000 ms) and logs `wait_db ready after N
ms`. A timeout, or a scan whose commit was delayed to the next boot, logs
`wait_db timed out` / `wait_db commit delayed`, and `scripts/shots.sh`
then fails the run. `build_db` starts a full build when there is no
database (the stock "Initialize Now", without its prompt) and does nothing
when one is usable or a build is running. Both read the state through the
sim-only hook `uisimulator/common/sim_db.c` (in the stock and the shell
sim).

    usb_connect
    usb_disconnect
    art_wait [ms]
    power_off_now

Phase 4b (docs/phase4b-brief.md) commands, in
`uisimulator/common/sim_cmd_art.c` (registered from `sim_script.c`, kept
apart as its own file per the shared-file ownership rule above).
`usb_connect`/`usb_disconnect` simulate `SYS_USB_CONNECTED`/
`SYS_USB_DISCONNECTED` by calling `sim_trigger_usb()`
(`uisimulator/common/sim_tasks.c`), the same path the SDL window's own
USB key toggle uses; `logf`/`DEBUGF` in affected threads show the
ack/broadcast handling. `art_wait` blocks until the art cache builder is
idle (default 10000 ms) through a weak hook, `sim_art_builder_idle()`,
so this links before Agent B's builder (`apps/ipodui/artcache.c`) exists;
until then (or in a build without it) it logs `art_wait unavailable` and
returns at once. `power_off_now` calls `sys_poweroff()` on the script
thread directly: the same `SYS_POWEROFF` broadcast a real power-button
press makes (`firmware/powermgmt.c`), reaching `apps/misc.c`'s
`clean_shutdown()` mid-operation rather than only at `quit`/EOF (which
already goes through the same path, via `sdl_sys_quit()`). The shutdown
runs on other threads asynchronously; follow it with a `wait` and, to
check what survives, a `reboot` so `scripts/shots.sh` starts a fresh boot
for the rest of the script.

    art_request
    disk_active <ms>
    expect_art <manifest>

Phase 4b-1 integration, same file. `art_wait` now waits for the builder
to have finished a pass over the current database (it no longer returns
before the first request). `art_request` asks the builder for a pass, as
a database load or the precompute does. `disk_active <ms>` makes the
builder see storage awake for that long (the hostfs sim has no disk
state), as an audio refill would on the device: during playback it reads
and writes only then. `expect_art <manifest>` reads every album of the
builder's table and its state from `index.dat` and logs `expect_art ok`,
or `expect_art failed` (a failure for `scripts/shots.sh`) when an album
of the manifest (`tests/ui/art_manifest.txt`, relative to the script)
has another state or any album is still pending. The builder logs
`art io read|write key=.. playing=P active=A` before its source reads
and slot writes, for check_trace.py's `artio` rule. The 4b-1 cases
(ready, fresh, dirty, playing, USB, power-off, torn, deletion while
playing) are `ipodui-p4b1-*.script`. check_trace.py allows one full
update per boot (`reboot`) and per `usb_disconnect`.

    audio_reload
    art_poweroff_abort

Phase 4b-1 end-review fixes, same file. `audio_reload` makes a real
audio refill without a key press (a key pauses the art builder): it calls
`audio_flush_and_reload_tracks()`, as a playlist change does, and
`buffering.c` ends the fill with `BUFFER_EVENT_FILL_DONE` and
`storage_sleep()`; during playback the builder works only in the window
that opens there (`ipodui-p4b1-art-playing.script`, which uses no
`disk_active`). `art_poweroff_abort` posts `SYS_POWEROFF` to the builder's
queue only (nothing else shuts down), as a power-off that
`clean_shutdown()` then cancels: the builder stops and, still alive
`STOP_HOLD` (25 s) later, builds again (`ipodui-p4b1-art-offabort.script`).

    title_phase title|clock|auto
    sim_clock HH:MM|rtc
    clock_format 12|24

Phase 4b-2 Time in Title header, in `uisimulator/common/sim_cmd_clock.c`
(test-only hooks the shell reads in the simulator). A scripted run starts
**frozen on the title**, so no golden depends on where a shot lands in
the 2 s title / 4 s clock cycle; `title_phase clock` with `sim_clock
15:04` gives a deterministic clock header, `title_phase auto` the device
behaviour (host clock unless `sim_clock` is set). `clock_format` sets the
24 Hour Clock setting. The sim run by hand alternates from the start.
`ipodui-p4b2-header-clock.script` uses all three.

    battery <0-100>|drain

Same file. A scripted run (stock or shell sim) starts with the simulated
battery **pinned at 100 %** (`uisimulator/common/powermgmt-sim.c`): the
sim otherwise drains 1 % a second, so the header's battery fill (x
146-149 on the main menu, 300-309 full width, y 5-12) depended on how long
a script had run. `battery <pct>` pins another level, `battery drain`
restores the sim's own drain and recharge. The sim run by hand drains as
before.

    drift_phase <ticks>

\#38 (`docs/phase-nav-brief.md`), in `uisimulator/common/sim_cmd_nav.c`.
Pins `apps/ipodui/preview.h`'s panel phase (lit ticks since shell start:
drift framing, the Extras clock) to an exact value, for deterministic
drift/Extras-clock goldens and review videos. Calls `ui_panel_phase_set()`,
declared weak in `sim_cmd_nav.c` with that function's exact signature so
this file (and the sim it links into) builds in a stock sim, or an
`--ipodui` sim before agent A's `preview.c` provides the real (strong)
definition — same pattern as `sim_cmd_art.c`'s `sim_art_builder_idle()`.
`preview.c` now provides it strongly (`#ifdef SIMULATOR`) in both the
stock and the `--ipodui` sim, the weak one winning only when `preview.c`
isn't linked in at all (the stock sim, which never builds any
`apps/ipodui` file).

    audio_stop

\#38 integration fix-up, in `uisimulator/common/sim_cmd_audio.c`. Calls
`audio_stop()` directly on the script thread: the same stop a real
"Stop" action makes, without holding any button. Revision 3's global
buttons (`apps/ipodui/ui_main.c`) made a long-held PLAY mean sleep from
every screen; the stock WPS's own long-press-to-stop exits well inside a
scripted `hold PLAY 1500` (it watches `audio_status()` every `HZ/5` and
returns as soon as playback stops), so the rest of that hold is read by
the shell's own input loop, which sees the still-held PLAY and calls
`sys_poweroff()`, quitting the sim. Every `ipodui-*` script that used
`hold PLAY <ms> # stop` to stop playback uses `audio_stop` instead.

    record <ms> <fps> <name>

Revision 2 (owner request), in `uisimulator/common/sim_cmd_record.c`.
Dumps the LCD at a steady `fps` (clamped 1..30) for `ms`, into
`<shotdir>/<name>_frames/frame%05d.bmp` — `scripts/make_video.sh` stitches
them into a small H.264 MP4 for the owner round. Frame pacing uses an
accumulator (each frame sleeps only its own delta from an ideal elapsed-ms
target), so an `fps` that doesn't divide 1000 (24, e.g.) doesn't drift
over a long recording. A second `record` with the **same** `name` in one
script continues the frame sequence from where the previous one left off
(a video built from several non-contiguous bursts, e.g. a push and, later,
its pop); a new `name` starts a fresh sequence in its own directory.
Nothing about `record` drives the backlight, input or any other sim
state — pin the clock, battery and (for `#38` scripts) the drift phase
with their own commands first, for a deterministic recording. `record`
does not block the UI thread (unlike `wait`), so calling it right after a
`key`/`wheel`/`hold` command captures whatever that command set in motion
(a push/pop slide, a glide) as it actually plays out.

    np_pin <ms|off> [<vol%>|off]
    np_seq_bump
    charger on|off
    repeat off|all|one
    dark on|off
    play_files <path>...
    mkdir <path>
    expect_rating <path> <0-10> [wait_ms]
    expect_pos <min_ms> <max_ms>
    wait_pos <ms> [timeout_ms [name]]
    wait_index <n> [timeout_ms]
    expect_albums <n>

Phase 5 (Now Playing, `docs/phase5-brief.md`), in
`uisimulator/common/sim_cmd_np.c`. `np_pin` pins Now Playing's elapsed time
(`np_pb_sim_pin()`) and the volume bar's percentage, for deterministic
shots and videos. `play_files` starts a playlist of sim paths straight
away (no database lookup, no screen pushed): the shell's main menu only
re-reads its "Now Playing" row after a push/pop, so a script does
`key SELECT; key MENU` (Music and back), then `wheel +10; key SELECT`
(the GONP block every `ipodui-p5-*` script starts with). Library tracks
are only 2-5 s long; scripts that need a steady track play the mini
library (`/Music/...`, 20-60 s) or a fixture. `mkdir` makes a sim
directory (the target of `copy`). `expect_rating` reads the file's
`tag_rating` (stars x 2) from the database, `expect_pos` checks the
current track's elapsed time, `wait_pos` blocks until it reaches `<ms>`
(only once the playing path contains `<name>`, if given: a new queue starts
with the old track still current) and `wait_index` until the queue's
display index is `<n>`: playback progress instead of a wall-clock `wait`,
because the sim's audio runs below real time on a loaded host (0.1-0.8x
seen with coreaudio; shots.sh defaults to `SDL_AUDIODRIVER=dummy`, a steady
~0.65x). A timeout fails the run (default 60 s).
`expect_albums <n>` checks that the next n
queue entries form one contiguous run per album (the Albums shelf).
`np_seq_bump` makes one seqlock write (`np_pb_ab_write_begin/end`) with no
track change, as a chapter-table rewrite under the same track does (the
script thread stands in for the audio thread; a real rewrite needs a buffer
re-carve the sim cannot trigger on demand). `charger` sets the charger state (the sim pins the battery under a script,
so nothing else moves it; a pinned battery below 100 % reports charging while
the charger is on), `repeat` the Repeat setting, `dark` switches the
backlight off or on (the shell is then "dark": deferred work waits).

`# needs: np` (a header line) makes `scripts/shots.sh` run
`tests/fixtures/np/make_np_fixtures.py`, which writes `build/np-fixtures/`
(a 74-character title, lyrics in ID3v2.3, ID3v2.4 and MP4, 16:9 / 2:3 /
4:1 covers, a 90 s book with chapter images). A script copies what it needs
into the sim disk (`copy ../../build/np-fixtures/<f> /NPTest/<f>`) and
removes it at the end; scripts that scan them into the database
(`update_db_wait_change`) rescan after the removal.

check_trace.py header directives (Phase 5, `check_np()`):

| Line | Meaning |
|---|---|
| `# log: <text>` | the sim log must contain `<text>` |
| `# nolog: <text>` | the sim log must not contain it |
| `# seeks: N` | exactly N `np seek ms= id= pre=` lines (dropped seeks are not counted); fails when none are found and N > 0 |
| `# fulls-extra: N <why>` | allows N more `update full` lines than boots + SELECT/MENU presses (a menu redraw the script's events cause) |
| `# trace: np` | the log must have `np snap` lines (fails if none) |
| `# trace: shuf` | the log must have `shuf:` lines (fails if none) |
| `# np-wake` | after `unlit exit` the log must have an `np draw id=<a> playing=<b>` line (Now Playing's full paint) before any LCD update, with a = b: the wake paints the playing track (#58). Checked on every log that has one; the directive makes it required |
| `# samtrack-nobody` | two `np snap` lines share a track id with a moved seq (`np_seq_bump`), and no `update np:body` follows the second |
| `# logrange: <lo> <hi> <regex>` | the regex's one `(\d+)` group is within lo..hi on every matching line (at least one) |
| `# shuf-fail-ok` | allows `shuf: start|mode .. fail` lines (they fail the run otherwise) |

Rules: `np:<band>` updates stay inside the body; bands are `body` (220 rows),
`info` (178) and `title`, `row`, `rowfull`, `stars`, `glyphs`, `chapter`,
`counter` (34 each), any other band name fails; the strip is never claimed
by PANEL or DRIFT while Now Playing is on top; no `np alloc` line, and no
`np io ... playing=1 ... lit=0` line (storage read while dark; both are
proposed log lines, skipped while absent). `art get ... size=2` (Now
Playing's own bundle read at enter) is allowed during the push slide.

`tests/review/np-review.sh` is the owner-only review video (it plays the
owner's own FLAC, never committed, and is not part of the gate).

    hold_switch on|off
    backlight_expire
    usb_glyph usbc|dock30|auto
    splash_probe <text>
    yesno_probe <text>
    capture <name>
    storage_idle

Phase 8a (native system screens, `docs/phase8a-brief.md` section 8), in
`uisimulator/common/sim_cmd_sys.c` (`sim_cmd_sys.h`). `hold_switch` sets the
Hold switch to a state (the `HOLD` key toggles): `button-sdl.c` drops every
key while Hold is on and calls `backlight_hold_changed()` on the change, as
the clickwheel driver does, which is the call that posts
`SYS_IPODUI_HOLD_CHANGED` on a shell build (the command waits 3 ticks for
that poll). `backlight_expire` calls `backlight_off()`: the same
`BACKLIGHT_OFF` path as the idle timeout, so the unlit face's
`SYS_IPODUI_BACKLIGHT_OFF` fires. That event is posted only for a positive
timeout (brief Rev 1 M2), so the command first sets a timeout of 0 (Always
On, the sim default) or Off to 15 s, battery and plugged alike. `usb_glyph` pins which idle connector glyph the USB screen
draws, through `int sim_usb_glyph_pin` (-1 auto, 0 `UI_USB_GLYPH_DOCK30`,
1 `UI_USB_GLYPH_USBC`; declared in `sim_cmd_sys.h`, read by
`usbscreen.c` under `SIMULATOR`). `splash_probe <text>` calls
`splashf(0, "%s", text)` from the script thread (`%20` is a space): in a
shell build the funnel logs `splash-suppressed <text>` and draws nothing.
`yesno_probe <text>` calls `gui_syncyesno_run()` with that one line from the
script thread (shell builds only): the funnel logs `yesno-suppressed <text>`,
draws nothing and answers No (D2); the command prints
`yesno_probe <text> -> no`.
`capture <name>` calls `sim_capture_screen(name)` (the call
`show_logo_boot()` makes at the boot hand-off, `sim_capture.h`): it writes
`<outdir>/<name>.png` when the script has `# boot_shot`, else does nothing.
`storage_idle` calls `call_storage_idle_notifys(true)`. `chord` is the
existing `sim_script.c` command (above), already what devmode scripts use.

Script headers added in Phase 8a (`scripts/shots.sh` unless noted):

| Line | Meaning |
|---|---|
| `# boot_keys: NAME[+NAME] <ms>` | holds those buttons (`MENU`, `PLAY`, `LEFT`, `RIGHT`, `SELECT`) for `ms` from the sim's first button poll, on every boot (`IPODUI_SIM_BOOT_KEYS=RIGHT:2500`, `button-sdl.c`; logs `sim: boot keys ...`). The boot-escape finger: `# boot_keys: RIGHT 2500`. A malformed line fails the run |
| `# boot_shot` | exports `IPODUI_SHOT_DIR=<outdir>`, so `sim_capture_screen()` writes `boot-fw.png` at the hand-off. Without it the call is a no-op and no other script gets an extra PNG |
| `# expect-file: <sim path> <text>` | after the run, the sim file must exist and contain the text (e.g. `/.rockbox/ipodui.log splash-suppressed probe`). Several lines allowed |
| `# expect-file-lacks: <sim path> <text>` | after the run, the sim file must exist and must not contain the text, in any case (the diagnostics report's privacy check). For either directive a path with spaces goes in double quotes: `"/ProkBox Report.txt" Velvet` |
| `# same: <shotA> <shotB>` | `validate.sh`: the two shots are pixel-identical at threshold 0 (the screen after a USB session, a face or devmode equals the one before) |
| `# ref: <shot> <repo-path>` | `validate.sh`: compare a shot with a reference image at threshold 0, at RGB565 precision (`pngdiff.py --rgb565`: references are host renders, whose 565-to-8-bit expansion differs from sips' by up to 1). A report (`note ref ...`): `GATE_REF_STRICT=1` makes it a FAIL. For brief 7.4's "pixel-identical to `stock-*-lit.png`, deviations signed off" |
| `# trace: boot\|usb\|unlit\|devmode\|splash\|log` | arms a `check_trace.py` 8a rule (below); a script named `ipodui-p8a-<kind>*` arms it by name |
| `# unlit-digits: N [M]` | exactly N (or N..M) `update unlit:digits` lines (one per minute step) |
| `# devmode: none` | the script expects no `devmode enter` (wrong screen, short chord); any enter fails |

`check_trace.py` Phase 8a rules (`check_p8a()`; every rule FAILS when its
anchor lines are absent, never skips; `tests/trace/test_p8a_rules.py` is the
self-test, run by the gate):

| Rule | Checks |
|---|---|
| `p8a-boot` | exactly one `boot screen gen=<hex>` per boot (1 + `reboot` lines), equal to `BOOTSCREEN_GEN` in `firmware/export/bootscreen_data.h` (header or define missing is a FAIL) |
| `p8a-usb` | `usb enter glyph=<usbc\|dock30> shell=<0\|1>` / `usb exit frames=<n> boost=<n>` paired, every exit `boost=0`; inside a pair only `update usb:static`, `usb:frame y=68 h=80` (needs the latest `usb io=1`), `usb:header y=0 h=22`; none outside |
| `p8a-unlit` | `unlit enter` / `unlit exit <key\|holdoff\|usb>` paired; inside only `update unlit:*` (`full` at most once, `digits y=62 h=68`, `status y=161 h=37`); none outside; the first update after an exit key/holdoff is `update full`; `unlit skip` and `swallow-end` values checked; `# unlit-digits` |
| `p8a-devmode` | `devmode arm` / `enter` / `exit` in order; an enter needs an arm (a `# boot_keys:` script is the boot escape and needs none); armed enters <= script `chord` lines of >= 3000 ms (the log has no timestamps); `# devmode: none` requires zero enters |
| `p8a-splash` | `# trace: splash`: at least one `splash-suppressed` / `yesno-suppressed`; on every script: no `splash-pass` unless the script is a devmode script |
| `p8a-log` | `log flush lines=<n> seq=<a>-<b>`: n >= 1, a <= b (16-bit wrap allowed); `# trace: log` requires one |

Review sheets: `tests/review/p8a-sheets.sh` (after running the scripts) composes
`out/review/p8a-{boot,usb,unlit,unlit-seq,devmode,splash}.png` with
`scripts/pngsheet.py` (N-column sheets, `diff:` and `unlit:` tone-mapped
cells), plus `boot.mp4` and `usb-io.mp4` from the `record` frames.

### Phase 6 (Audiobooks, `docs/phase6-brief.md`)

Commands in `uisimulator/common/sim_cmd_ab.c` (registered from
`sim_script.c`; they read the files as the host sees them, so the stock sim
links too). Paths are `/.rockbox/artcache/books.dat|books.tmp|books.pak|
books.idx|books.pak.tmp`.

    ab_power_cut now|exit <kind> [arg]
    ab_copy_at_exit <hostsrc> <simdst>
    ab_touch <simpath>
    ab_mark <text>
    ab_mem_mark
    expect_ab_mem_same
    expect_ab_store <started|-1|geN> <finished|-1|geN> [wait_ms]
    expect_ab_store_own <started|-1|geN> <finished|-1|geN> [wait_ms]
    expect_ab_file <store|tmp|pak|idx|pak_tmp|idx_tmp> present|absent [wait_ms]
    expect_ab_pak <books> [wait_ms]
    ab_pak_stat
    ab_pak_mark
    expect_ab_pak_same <ms>
    expect_ab_pak_changed <ms>

`ab_power_cut` damages a file the way a power cut leaves it. `exit` applies
the damage **after** the clean shutdown's own flush (at process exit, so a
`reboot` boots on it); `now` applies it at once. Kinds: `store_tmp`
(books.dat renamed to books.tmp), `store_hdr0` (first header sector zeroed),
`store_hdr_both` (both headers), `store_tear [sector]` (half of a record
sector zeroed; default the middle one), `store_trunc` (half the file),
`store_junk` (every byte garbage), `store_drop`, and the same `pak_*` /
`idx_*` kinds (`pak_tear`, `pak_drop`, `idx_drop`, `idx_flip` zeroes the
generation word, `pak_tmp_left` leaves a torn compaction file).
`ab_copy_at_exit` installs a host file (path relative to the script, like
`copy`) over a sim file at exit, e.g. the full `store_full.dat` fixture.
`ab_touch` moves a file's mtime an hour ahead (a stale book). `ab_mark`
only logs `sim_script: ab_mark <text>` (check_trace brackets).
`ab_mem_mark` records `core_available()`; `expect_ab_mem_same` fails if
it is now smaller (a core_alloc survived: pool growth while playing).
`expect_ab_store` counts the **valid** records of books.dat (the check byte
is non-zero) by state, retrying up to `wait_ms` (default 2000); `-1` = any,
`geN` = at least N. `expect_ab_store_own` counts the same way but leaves
out the records seeded from the database's play stats (a store that was
missing at boot is rebuilt from them), so a script that starts without
books.dat (`# reset: books`) counts only the books it played. `expect_ab_pak <n>` waits for books.idx to list at least
n books. `ab_pak_mark` remembers the index generation;
`expect_ab_pak_same <ms>` fails if it moves within `ms`,
`expect_ab_pak_changed <ms>` fails if it does not.
Every `expect_ab_*` failure fails the run (`expect_* failed`).

`# needs: ab` (a header line) runs `tests/fixtures/ab/make_ab_fixtures.py`,
which writes `build/ab-fixtures/` (atomic swap, so parallel workers are
safe): `pref.m4b` (Preface + Chapter 1-3), `nochap.m4b`, `short.m4b` (40 s,
2 chapters), `multi/01..04.mp3` (album "AB Multi", CHAP in files 1 and 3),
`ffctoc.mp3` (ffmpeg ID3v2.4 with non-synchsafe CHAP/CTOC sizes, #50),
`many.m4b` (1,100 chapters), `utf8.m4b` (titles that need a UTF-8 cut) and
`store_full.dat` (2,048 sealed records, made by `mkstore.c` over the real
`ab_store_core.c`). A script copies what it needs into `/Audiobooks/...`
and runs `update_db_wait_change`. Audiobooks rows with the library's four
books: `A Field Guide` row 0, then the `AB ...` books alphabetically
(`AB FFmpeg CTOC`, `AB Many Chapters`, `AB Multi`, `AB No Chapters`,
`AB Preface Book`, `AB Short Book`, `AB Utf8 Cut`), then Lighthouse, Mini,
Winter. `hold MENU 1000` returns to the main menu from anywhere.
`tests/fixtures/ab/gen_p6_scripts.py` writes the repetitive multi-boot
scripts (`powercut`, `powercut-torn`, `store-evict`, `pak-build*`,
`pool-cap`); edit it, not the output.

check_trace.py header directives (Phase 6, `check_ab()`; the lines are the
`AB_TR_*` macros of `apps/ipodui/ab_book.h`):

| Line | Meaning |
|---|---|
| `# trace: ab` | the log must have `ab ...` trace lines (fails on a build without B/M) |
| `# ab-idle-max: N` | at most N `ab store w=idle` lines (default 4; the log has no clock, so this bounds the forced 30-minute checkpoint wake) |
| `# ab-slots: strict` | consecutive `ab slot w=S` lines name different slots |
| `# ab-speed: a b c` | the `ab speed N` percents contain a, b, c in order (75/100/125 only) |

Always on once `ab` lines exist: no `ab alloc|core_alloc` line; every
`ab open ... reads=` is at most 2; `ab store w=now` never falls between
`ab_mark track_change_begin` and `ab_mark track_change_end` (a track change
inside a book is a pending save). Plus the generic `# log:`/`# nolog:`/
`# logrange:` lines of Phase 5.

| Script | Shots / checks |
|---|---|
| `ipodui-p6-booklist` (`# fresh-db`) | list, list_row2, page\*, list_back: two-line rows, no dots |
| `-open` | page_nospot, page_ch1, np_chapter1\*, page_resume, np_resume\*: Resume only with a saved spot; entry 2 is "Chapter 1" |
| `-nochap` | page, np_start\*: a book without chapters opens its page (AB-1) |
| `-resume`, `-resume-rebuild`, `-resume-dirty` | resume per book across a reboot (DB state ready / rebuilt / dirty) |
| `-start-at-zero` | old tagcache stats never move a shell-started book (Rev 1 item 1) |
| `-finished` | within 30 s of the end, or the queue's end, clears the spot |
| `-speed` | prompt, Slower/Normal/Faster, `ab-speed: 125 100 75 100`, music unaffected |
| `-multifile` | whole-book bar and counter across files; fallback then pak map |
| `-slots` | two-book queue, no next-file chapters, alternating slots |
| `-powercut`, `-powercut-torn` | books.dat recovery (books.tmp, header copies, torn, truncated, junk) |
| `-store-evict` | past 2,048 books (`00a00000`, `00a00003`, `00a00006`) |
| `-pak-build`, `-pak-build-fresh`, `-pak-build-dirty` | a blob for every book, stale rebuild, stopped only, lost/torn index |
| `-pool-cap` | opening a 1,100-chapter book's page while playing does not grow the pool |
| `-titles` | the pages of `ffctoc.mp3` (#50), `many.m4b` (1,100 chapters) and `utf8.m4b` (UTF-8 cuts); reviewer shots, no goldens |

\* timing-sensitive, `# golden: skip`.

`ab-review.mp4` (owner review, not a gate script): `tests/review/ab-review.sh`
runs `tests/review/ab-review.script` and writes `out/review/ab-review.mp4`.
Steps: Music > Audiobooks (book list); "AB Preface Book" without a spot (no
Resume); Chapter 1 in Now Playing (chapter title + counter); next/previous
chapter; pause; the page again with Resume; Resume; "AB Multi" (whole-book
bar, counter across the file boundary, Scrubber); Settings > Audiobooks
(caption, the prompt, Faster/Slower/Normal).

### Phase 11 (`docs/phase11-brief.md`)

Sim commands (`uisimulator/common/sim_cmd_p11.c`, header `sim_cmd_p11.h`):
`usb_eject [off]`, `type <chars>` (`<` deletes, `%20` a space),
`expect_queue <n> <first-path>`, `otg_dump`, `db_rebuild_keep_otg [ms]`, and

- **`time_in_title on|off`**: sets `global_settings.ipodui_time_in_title`
  and calls `ui_header_set_time_in_title()`. Time in Title is **Off** by
  default since Phase 11 (brief 1.2, Rev 1 M-7), so a script that needs the
  header clock (`title_phase clock|auto`) runs `time_in_title on` first. We
  picked the command over a `# setting:` header: it works mid-script and
  next to `title_phase`. After a `reboot` the saved setting applies, so
  re-issue it. **Explicit-clock-on list:** `ipodui-p4b2-header-clock`,
  `ipodui-nav-drift-review`, `ipodui-nav-transitions-review`, and every
  `ipodui-p11-datetime*` script. A new script that shows the header clock
  without it is a bug (the header shows the title).

Trace directives read by `check_trace.py`'s p11 rules (section "Trace
assertions"): `# trace: search otg hold vlimit about sys qw2` (or the name
`ipodui-p11-<kind>*` / `ipodui-qw2-*`) selects a kind, which then FAILS if
the screen emitted none of its lines; `# arena: full-ok <why>` allows
`arena full` lines; `# otg-resolves: N` allows N more `otg resolve` lines;
`# vlimit: locked` starts the vlimit rule locked; `# sys: dark` requires
the backlight-off path after the Charging screen; `# qw2: bundle` requires
`np art src=bundle` after the first bundle line.

### Phase 15 (`docs/phase15-brief.md`)

No new sim commands. Trace rules in `scripts/check_trace.py` (`check_p15`);
a script selects one with `# trace: p15eq p15dac p15vol p15fade` or by its
name (`ipodui-p15-eq*`, `-standby*`, `-volume*`, `-fade*`), and a selected
rule that finds none of its lines FAILS. With the lines present a rule
always runs.

| Rule | Lines | Passes when |
|---|---|---|
| p15eq | `eq engine=hw\|sw preset= maxerr= precut=` | `maxerr`/`precut` equal `apps/ipodui/eq_hw_presets.c`; hw iff `maxerr <= 15`; Off is `sw 0 0`; the `eq preset= enabled= precut=` line before it shows one engine (hw: `enabled=0 precut=0`; sw: `enabled=1`; Off: `enabled=0`). `# eq-engines: 2=hw 5=sw` pins presets |
| p15dac | `dac standby= sleeps= wakes= wake_us=` | standby 0/1; counts never decrease and change on every line; `wakes <= sleeps+1`; `standby=1` needs `sleeps >= 1`. `# dac: sleeps>=N wakes>=N` bounds the final counts |
| p15vol | `vol step= db= bar=` | step 0..47; `bar == step*1024/47`; `db` equals `apps/ipodui/volcurve.c`'s table. `# vol-max: N` caps the top step and requires reaching it; `# vol-walk: N` bounds the jump between neighbouring lines |
| p15fade | `ipodui: fade dir=in\|out steps=` | steps 0..25. `# fade: reverse` needs an opposite-direction line with `0 < steps < 25` |

Timing of the standby windows uses `ab_mark` + `# logbetween:` (wall-clock
`wait`s; nothing plays, so no position to wait on). The `dac` line comes
from the shell's main loop (`ui_main.c`), which blocks in Now Playing
while paused, so the standby scripts pause from the main menu.
`ipodui-p15-standby` cannot show "stopped by the user" (the shell has no
stop) or a headphone click in standby (the sim's click callback replaces
the sound; device check only).

### Phase 9 (`docs/phase9-brief.md`)

Sim commands (`uisimulator/common/sim_cmd_p9.c`, header `sim_cmd_p9.h`):

- **`alloc_dump`**: one `p9 alloc name=<ops> size=<b> handle=<h> pins=<n>
  move=<cb>` line per live `core_alloc` (buflib) block, then `p9 alloc
  total=<b> count=<n> free=<b>`. buflib keeps no names, so `name=` is the
  block's `buflib_callbacks` symbol and `move=` its move callback (dladdr;
  `none` for NULL ops). Criterion 3 compares the dump at boot with the trims
  off and on (no browser cache, skin/backdrop, iAP or dircache block).
- **`open_count`**: `p9 opens=<n>`, the successful `sim_open()` calls since
  boot, any thread (a `HAVE_IPOD_UI` counter in `filesystem-sim.c`).
- **`run_rock <path> [param]`**: `--ipodui-plugins` sims only:
  `plugin_load()` on the script thread, then `p9 rock rc=<status>`. The
  script blocks until the rock returns, so only a rock that finishes
  without input fits; elsewhere it logs `p9 rock unsupported` and
  `expect_rock failed` (the script fails).

Agent A's database commands live in `sim_cmd_p9a.c` (`stat_mark`,
`expect_stat`, `expect_stat_delta`, `expect_in_db`, `expect_not_in_db`; see its header).

Scripts: `ipodui-p9-smoke-{boot,browse,play,usb,devmode}` are the
`sim-ui --ram=32` smoke set (criterion 8), run like any `ipodui-*` script;
`smoke-boot` carries the boot `alloc_dump`/`open_count`. A's
`ipodui-p9-stats-playcount`, `-usb-rescan`, `-rating-persist` and
`-devmode-debug` cover H1, H2 and M1. None has goldens: they assert log
lines (`# log:`/`# nolog: splash-pass`) and keep shots for review.

Host-side Phase 9 tools (not scripts, listed here because the gate's size
step uses them):

- `scripts/sizemap.py MAP [--base MAP] [--by object]`: image and bss per
  subsystem from the linker map (the waterfall, brief section 8).
- `scripts/risk/reach.py ELF [--callers SYM] [--paths SYM] [--cut GLOB]
  [--reachable|--unreachable]`: reachability over the linked core
  (contract in its docstring). The size step checks `utf8_normalize` keeps
  a caller in `metadata.o`. `scripts/risk/reach_selftest.sh` builds an ARM
  fixture under `build/` and checks 30 verdicts plus exact agreement with
  linker GC; `scripts/risk/reach_gc_agree.py OUT` checks that every section
  the linker keeps in a real build holds a reachable node (the other
  direction from B's `gc_check.sh`).
- `scripts/trim-revert.sh --list|--check|--dry|[build]`: trim closures;
  the build mode is orchestrator-only (P9 batch lock, outside the gate).

## Naming: which simulator runs a script

`scripts/validate.sh` runs every `tests/ui/*.script`:

- `ipodui-*.script` against `build/sim-ui` (`--ipodui` shell build),
- everything else against stock `build/sim` (`fonts*` is skipped; it is a
  manual comparison script).

To run one by hand against the shell build:
`SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/ipodui-p2-menu.script`.

| Script | Shots |
|---|---|
| `smoke.script` (stock) | boot, menu, item, back |
| `ipodui-p2-menu.script` | boot, menu_down, music, music_down, back |
| `ipodui-p2-wps.script` | nowplaying, wps_back |
| `ipodui-p3-scroll.script` | music_mid\*, music_end, songs_top, slow_bar\*, slow_rest, fast_mid\*, fast_bar\*, fast_end, fast_top, back: `#38` retargets the long-list half from the Settings stub (now split) to Music > Songs (~297 rows, two-line) |
| `ipodui-p3-transition.script` | boot, push_mid\*, push_done, pop_mid\*, pop_done, push2_mid\*, push2_done, pop2_mid\*, pop2_done: still targets Settings as its "full-width list" case, which `#38` will make split too — not retargeted this phase (out of `#38`'s C scope; flagged to the orchestrator) |
| `ipodui-p3-marquee.script` | songs_top, marquee_0, marquee_2s\*, marquee_left: `#38` retargets from the Settings stub's row 17 to Music > Songs' one deliberately long title (`tests/ui/setup_library.py` SPECIAL_TITLES' "Ölfus ...", rank 161 of 297, reached by `wheel +165 100`) |
| `ipodui-p3-motion-dev.script` | developer walk-through of Phase 3 motion, retargeted like `-scroll`/`-marquee`: boot, push_mid\*, music, push2_mid\*, songs, slow, glide_mid\*, slow_rest, fast_mid\*, fast_end, fast_rest, marquee_0, marquee_2s\*, marquee_lap\*, pop_mid\*, music_back, pop2_mid\*, back |
| `ipodui-nav-panels.script` | `#38`: the main menu's per-item preview panel (D1/D2/D10) — music_panel, podcasts_panel, extras_panel, settings_panel, shuffle_panel, nowplaying_panel, shuffle_panel_again. Needs agent A's preview.c (and B's providers for the icon panels): the stub draws plain panel_bg regardless of the item, so every shot looks the same on `nav` alone |
| `ipodui-nav-drift.script` | `#38`: the drift panel (D4/D5) with a pinned phase (`drift_phase`) and the storage-asleep re-dwell case (`disk_active`) — fade_done, mid_pan, near_cycle_end, cycle_change_asleep, cycle_change_awake. Needs agent A's preview.c |
| `ipodui-nav-settings.script` | `#38`: the Settings split screen and the Backlight leaf (D8/D11/D13) — settings_push_mid\*, settings_top, backlight_push_mid\*, backlight_top, backlight_changed, back_to_settings. Needs agent B's screens/settings.c: the stub is still the flat 30-row list (same "Backlight" position, row 6) |
| `ipodui-nav-reveal.script` | `#38` D6: the reveal/split stub (the kind round-trip and D7 durations are a host test, `tests/nav/test_nav.c`; nothing reachable this phase ever pushes with `UI_T_REVEAL`/`UI_T_SPLIT`, Extras having no real sub-items yet) — boot, push_mid\*, extras, pop_mid\*, back. `# transitions: 250 290`: **pending** until agent A wires `nav_transition_ticks()` into `ui_main.c` (`tests/ui/PENDING`) |
| `ipodui-nav-transitions.script` | `#38` D7 push/pop timing assertions, on screens already reachable without A's or B's work — boot, push1_mid\*, music, push2_mid\*, songs, pop1_mid\*, music_back, pop2_mid\*, back. `# transitions: 190 250 290 190`: **pending** until A wires the durations (`tests/ui/PENDING`) |
| `ipodui-nav-globalkeys.script` | Revision 3: NEXT/PREV from any screen, hold MENU -> main menu, hold PLAY -> sleep — artists_before, artists_after, three_deep, after_hold_menu, after_hold_play. Needs agent A's input.c/ui_main.c work; today none of the three has any effect (documented in the script), so every pair of shots is identical |
| `ipodui-nav-drift-review.script` | Revision 2: `drift-review.mp4` source (`record`, see "Review videos" above) |
| `ipodui-nav-transitions-review.script` | Revision 2: `transitions-review.mp4` source |
| `ipodui-p3-nostrip.script` (`# env: IPODUI_NO_STRIP=1`) | settings, past_bar\*, past_view, fast_end, top, back: the no-strip fallback (snap, full redraws) |
| `library-db.script` (stock) | boot, database, album_artists: the test library in the stock database browser |
| `ipodui-p4-browse.script` | music, artists, artists_b, artist, all_songs, album, wps\*, back |
| `ipodui-p4-sort.script` | artists_top, artists_b, artists_end, artists_t, albums_top, songs_top, songs_end, genres |
| `ipodui-p4-qscroll.script` | songs_top, slow, spin_mid\*, spin_mid2\*, spin_stop\*, spin_rest\*, spin_back\*, top_rest |
| `ipodui-p4-audiobooks.script` (`# fresh-db`) | books, book_wps\*, books_played\*, podcasts, episodes |
| `ipodui-p4-playlists.script` | playlists, otg_added\*, otg_gone, otg\*, pl_wps\*, pl_back |
| `ipodui-p4-update.script` (`# fresh-db`) | updating\*, music_updating\*, music_ready, artists |
| `ipodui-p4-commit-playing.script` | playing\*, stopped\*, reload_waits\*, reloaded\*: a library update while music plays must not stop it (the commit waits for the stop); after a `reboot`, music resumed before the boot-time RAM cache load makes it wait too, with "Updating Library..." (round-3 M1); fails on `expect_playing` / `expect_db_grew` / `expect_lib_updating`. DB state *ready* (docs/process.md item 10) |
| `ipodui-p4-rebuild-playing.script` | updating\*, restored\*: a rebuild while music plays: the new database's commit waits, "Updating Library...", and lands in the same boot once playback stops (round-3 H1). DB state *fresh* |
| `ipodui-p4-dirty-playing.script` | updating\*, restored\*: `dirty_db` + `reboot` (a power cut mid-commit); music resumed at boot; the background rebuild's commit waits for the stop and lands in the same boot. DB state *dirty* |
| `ipodui-p4b-rows.script` | artist_albums, albums, albums_glide\*, albums_rest, podcasts, episodes: two-line rows with 55 px mini art or the glyph; `expect_art` against `art_manifest.txt` |
| `ipodui-p4b-artbuild.script` (`# fresh-db`) | pending\*, built: thumbnails fill in after a fresh build, no layout jump |
| `ipodui-p4b2-header-clock.script` | main_title, main_clock, music_clock, main_clock_12h (frozen `sim_clock`), auto_title\*, auto_clock\*, auto_back\*: Time in Title |
| `ipodui-p4b1-art-*.script`, `ipodui-p4b-art-*.script`, `ipodui-p4b1-delete-playing.script` | the art builder in each DB state (ready, fresh, dirty), while playing (fill windows), across USB, power-off, an aborted power-off (`-offabort`) and a torn index; no goldens (state, not pixels): asserted by `expect_art`, `art_wait` and check_trace.py |
| `ipodui-p4-refresh.script` | before, during_update\*, after, after_top, reopened, restored: Songs stays filled and reopens with a new track after a background commit (adds and removes `fixtures/refresh.mp3`) |
| `ipodui-p8a-boot.script` | `# boot_shot`: boot-fw (the sim's hand-off capture), first; `boot.mp4` from `record` |
| `ipodui-p8a-usb.script` | before, idle_usbc, after, idle_dock30, io0\*/io3\*/io6\*/io9\*, idle_after_io, after_io; `usb-io.mp4`; `# same: before after` |
| `ipodui-p8a-unlit.script` | before_face, scene_12h_playing, after_dismiss, scene_24h_paused, scene_24h_stopped, scene_12h_hold_charging, after_hold; `# same: before_face after_dismiss`, `# ref:` the four stock lit PNGs |
| `ipodui-p8a-unlit-minute.script` | face_a\*, face_b\*: one digits band per minute step (real clock, waits 62 s) |
| `ipodui-p8a-unlit-usb.script` | before, usb_over_face, after_usb: USB over the face (the PLAY-hold sleep path is device-only: the sim defines HAVE_POWEROFF_WHILE_CHARGING) |
| `ipodui-p8a-devmode.script` | about_before, developer, debug\*, about_after: the chord on About (`# same:`) |
| `ipodui-p8a-devmode-neg.script` | main_before/after, about_before/after: wrong screen and a short chord do nothing (`# devmode: none`) |
| `ipodui-p8a-devmode-boot.script` | `# boot_keys: RIGHT 2500`: developer, main: the boot escape |
| `ipodui-p8a-splash.script` | before, after: `splash_probe` is suppressed; `# expect-file:` the log line |
| `ipodui-p8a-splash-yesno.script` | before, after: `yesno_probe` is suppressed, logged and answers No |
| `ipodui-p8a-unlit-npstop.script` | woken: playback ends under the face; the wake press dismisses it and Now Playing pops by itself (fix M2) |

\* timing-sensitive (`# golden: skip`), never compared with a golden.

`validate.sh` skips every script named in `tests/ui/PENDING` (one name per
line, `#` comments) with a note, for scripts written ahead of the code
they test. Two are pending now (`#38`): `ipodui-nav-reveal` and
`ipodui-nav-transitions` declare `# transitions:` (check_trace.py), which
hard-fails today — push/pop timing isn't wired to `nav_transition_ticks()`
until agent A's work lands; see `tests/ui/PENDING` for the detail.
`ipodui-p4b-rows`, `-artbuild` and `-art-playing` check every album's art
state with `expect_art`; `# trace: art` in a script makes check_trace.py
require "art get" lines (thumbnails read, and only at rest).

Settings has 30 stub rows (apps/ipodui/menu.c); the long-titled row
("Remember Playback Position ...") is row 17 (+17 clicks from the top),
and "Backlight" is row 6 (`#38`'s `ipodui-nav-settings.script`). Both
positions are specific to `nav_stub.c` SECTION B's unchanged Phase 3 list
(D11) and need rechecking once agent B's `screens/settings.c` replaces it
— B may reorder items to match `docs/design/7g-catalogue.md` section 7.1.

## Environment (`# env: VAR=value`)

`scripts/shots.sh` exports every `# env: VAR=value` line of a script to the
simulator (so `validate.sh` gets it too). Sim-only knobs of the shell:

| Variable | Effect |
|---|---|
| `IPODUI_NO_STRIP=1` | the offscreen strip allocation fails (apps/ipodui/anim.c), so lists snap and push/pop redraw in full, as on a device out of memory |
| `IPODUI_SIM_BATT_MV=<mV>` | pins the battery at that raw voltage from power-on (uisimulator/common/powermgmt-sim.c), until a `battery` command: the boot battery guard's scripts (`ipodui-p16-bootbatt-*`) |
| `IPODUI_SIM_CHARGER=1` | boots with the charger plugged (powermgmt-sim.c) |
| `IPODUI_SIM_CHARGER_AT_MS=<ms>` | the charger goes in that many ms after the boot guard's first reading (powermgmt-sim.c): `ipodui-p16-bootbatt-plug` |
| `IPODUI_SIM_DRAINED=0\|1\|missing\|empty` | the boot guard's drained latch record (or none, or a 0-byte one), written at boot before the guard reads it (apps/ipodui/bootbatt.c); unset, the sim has no latch (a record one script leaves never decides another's boot): `ipodui-p16-bootbatt-*` |
| `IPODUI_SIM_DRAIN_AT_MS=<ms>` | the cell reads 1,000 mV (or `IPODUI_SIM_DRAIN_MV=<mV>`) that many ms after power-on: the low-battery shutdown within seconds, or a slow fall to 0 % (powermgmt-sim.c): `ipodui-p16-bootbatt-latch-{set,shutdown}` |

## Resume/playback state (`# resume: keep`/`# resume: playing`)

Resume/playback state (`apps/settings.c` `RESUMEFILE`,
`.rockbox/.resume.cfg`) survives on the simdisk across boots. Left alone,
a script's "Now Playing" visibility (`apps/ipodui/ui_main.c`
`ui_now_playing_visible()`: audio playing, or `resume_index != -1` and something played and not stopped this boot; Phase 6 Revision 3, #58) would
depend on whatever ran earlier on that disk — fine when every script
always runs in the same order against one shared disk, not fine once a
script can run on a freshly cloned disk, in any order, next to any other
script (the parallel gate; see "Naming" above). So `scripts/shots.sh`
deletes `.resume.cfg` (and its `.new`/`.old` rename-dance files) before
every script's first boot, unless the script says otherwise:

    # resume: keep

leaves the file as is — inherit whatever is there. It never re-applies
before a script's own internal `reboot` parts (those already share one
disk within a single `shots.sh` run).

    # resume: playing

seeds a minimal `.resume.cfg` (`IDX: 0`, the rest left at their defaults)
and `.rockbox/.sim_np_row` (the sim build counts that queue as played this
boot; on the device the row waits for playback after a boot, owner #58)
so the main menu's "Now Playing" row is deterministically visible without
the script having to start playback itself first. This is **not** a real,
resumable playlist position (no valid playlist crc) — it only satisfies
the visibility check above; a script that resumes it into the WPS needs
the real thing (play a track, as `ipodui-p2-wps.script` does).

A golden that shows/hides "Now Playing" only because of this reset (not
because of anything the script's own commands do) is the tell that it
relied on another script's leftover state; add the directive that
reproduces its approved look instead of changing the golden.

## Mini music library (`# needs: music`)

`tests/ui/setup_music.sh <simdisk>` generates a small tagged library with
ffmpeg into `<simdisk>/Music/` (all under the gitignored `build/`):

| Path | Contents |
|---|---|
| `Tone Collective/Sine Studies/` | 3 MP3s (45/30/25 s), track numbers, embedded cover art |
| `Square Wave Quartet/Low Frequencies/` | 2 MP3s (60/20 s), no art |
| `Fixture Author/Mini Audiobook/Mini Audiobook.m4b` | 48 s AAC, 4 chapters (Nero + QuickTime), genre Audiobook, cover art |

The cover is an original abstract gradient drawn by ffmpeg. The script is
idempotent: a version stamp (`Music/.mini-library`) makes reruns instant;
bump `VERSION` in the script when changing the library.

`scripts/shots.sh` runs it automatically on `$SIM_BUILD/simdisk` before any
`ipodui-*` script, and before any script containing a line

    # needs: music

(`# needs: library` selects the larger Phase 4 library below.)

## Test library (`# needs: library`)

`tests/ui/setup_library.sh <simdisk>` (contents defined and documented in
`tests/ui/setup_library.py`) generates the Phase 4 library in about 3 s
(8 base clips, every track a 2-5 s stream copy retagged by ffmpeg, in
parallel). Under `build/` only, idempotent (`Library/.test-library` holds
`<VERSION> <file count>`; regenerating also deletes the database files):

| Path | Contents |
|---|---|
| `Library/<Artist>/<Album>/` | 295 MP3s: 47 artists, 59 albums, 8 genres, 6 composers; cover-art variety across specific albums (below) |
| `Library/Compilations/Summer Tones Vol. 1/` | 12 tracks, album artist "Various Artists", compilation flag (TCMP) |
| `Library/Untagged Session/` | 2 MP3s with no ID3 tags at all (no album/artist/title): a directory-grouped "album" |
| `Podcasts/<Show>/` | Signal Hour (embedded show art), Tone Talk Weekly (none): 2 episodes each, genre Podcast |
| `Audiobooks/<Author>/<Book>.m4b` | 3 books, brand M4B, 3-5 chapters; "Winter Stories" has genre "Spoken Word", not Audiobook; MP4 `covr` art, PNG on "A Field Guide to Tones" (unsupported in 4b), baseline JPEG on the other two |
| `Playlists/` | Road Trip.m3u (8), Focus.m3u8 (6), Accents & Symbols.m3u (7), Empty.m3u |
| `.rockbox/config.cfg` | `tagcache_ram: on`, `tagcache_autoupdate: on` (other lines kept) |

**Cover-art variety** (Phase 4b, docs/phase4b-brief.md): most albums get
the default embedded baseline JPEG (ID3 APIC); four have no art anywhere
(NOART); specific others get folder art (`cover.jpg`/`folder.jpg`,
baseline JPEG), an embedded progressive JPEG, a folder-only PNG (both
unsupported in 4b, FAILED), or a non-square 600x400 embedded cover (OK).
"The Bellwethers" shows all three outcomes across its own three albums:
Flock Together (embedded, OK), Shepherd's Delight (NOART), Wool & Wire
(`cover.jpg`, OK). Per-chapter audiobook images are not generated: ffmpeg
has no simple way to author a QuickTime chapter image track, so this is
skipped (noted, not attempted). `tests/ui/art_manifest.txt` (from
`setup_library.py --art-manifest`) lists the expected art state (`OK`,
`NOART` or `FAILED`) for every album/show/book above with variety worth
asserting on.

Sorting edge cases: "!!!", "10dd", "2Gem"; "The Bellwethers", "The The",
"The Zephyrs" next to "Theory of Tides"; Björvik, Silfur Rós, Émile Ardent,
Ångström Lane, Zoë Marchetti; lower-case "aurora drift"; titles like
"#9 Dream Machine", "(Untitled)", "¿Dónde Estás?". The library stays out of
`/Music`, so the Phase 2 folder walk and its goldens see only the mini
library. No resume position is seeded (the tagcache keeps it in the
database, written only at runtime); `ipodui-p4-audiobooks` plays a book
instead. `tests/ui/library_manifest.txt` (from `setup_library.py
--manifest`) lists the audiobook/podcast titles, authors and genres that
must never appear in a music list.

**Database in the sim.** Stock Rockbox never builds a database on its own:
`tagcache_autoupdate` only updates an existing one, and the first build is
"Initialize Now" behind a yes/no prompt. In the sim the first build also
can't commit in the same boot: there is no dircache whose buffer `commit()`
could borrow, so the scan is left in `database_tmp.tcd` ("commit delayed",
"Please reboot"); the next boot commits it at startup (with no prompt in the
macOS sim). So for a `# needs: library` script without a database,
`shots.sh` first runs a priming boot (`build_db`, `wait_db`) and the
script's own boot commits the scan; `wait_db` at the top of the script
then returns within a fraction of a second.

`# fresh-db` makes `shots.sh` delete `simdisk/.rockbox/database_*.tcd`
before boot and skip the priming boot: the shell must build the database
itself (`ipodui-p4-update`).

`# stale-import` is `# fresh-db` plus an empty `database_import.pending`
and an empty `database_tmp.tcd`, as an old build left them: the first boot
has no manifest and no database but an import marker
(`ipodui-library-import-marker`). It needs no `drop-db`: the script ends
with a published library.

## Golden images (ipodui-* only)

`# golden-region: <shot> <x0> <y0> <x1> <y1>` (end exclusive; several lines
per shot) limits the comparison, in `validate.sh` and in `golden_capture.sh`'s
run-to-run check, to those rectangles (`pngdiff.py --batch` takes them as a
4th tab field `x0,y0,x1,y1`). For a shot whose rest is timing-dependent:
`ipodui-p5-rating-slide` compares only the art and its reflection under a
sliding row.

Goldens must not depend on the state earlier scripts left on the disk: pin
the elapsed time (`np_pin`), wait for the art (`art_wait`), and leave
persistent state as found (the rating scripts reset Concert A to 0 stars).

If `tests/ui/golden/<script>/<shot>.png` exists, `scripts/validate.sh`
compares the new shot with it using `scripts/pngdiff.py` (stdlib-only PNG
decoder) and fails when more than `GOLDEN_THRESHOLD` percent of pixels differ
(default 0.5). Only `ipodui-*` scripts are compared, because the stock
status bar shows a clock. Check one by hand with `scripts/pngdiff.py
<shot.png> <golden.png>`.

To accept a new look, run

    scripts/golden_capture.sh [--verify3] [--dry] [-j N] tests/ui/<script>.script... [shot...]

rather than copying a shot out of `out/shots/<script>/` by hand: per
docs/process.md "Goldens", an agent only *proposes* a look, and this is
the only thing allowed to write `tests/ui/golden/<script>/`. Several
scripts can be given at once. The shared sim disk is primed once, then
every run gets its own `cp -c` clone and the runs go `JOBS` at a time
(env `JOBS` or `-j N`; default `min(16, 2*ncpu)`), so run 1 and run 2 of
the same script also run in parallel. Each script runs **twice** (pinned
state: clock/battery/resume are already pinned by default, above; three
times with `--verify3`, the third independent run) and, only if every
compared shot comes out pixel-identical between the runs (threshold 0),
its shots are copied in; otherwise it prints the differing shot(s), writes
`[run1 | run-n | diff]` composites to `out/gate-report/`, writes nothing
for that script and exits non-zero (other scripts in the same call are
judged on their own). `--dry` does everything except the write.
`[shot...]` (any argument not ending in `.script`) narrows which shots get
written (every shot the script takes still runs either way); shots named
in the script's own "# golden: skip ..." line (below) are skipped the same
as in `validate.sh`. Because runs use clones of the primed disk (like the
gate's workers), a capture never leaves residue on `build/sim-ui/simdisk`
-- the old sequential tool ran on that disk itself and could pollute it
for later gate runs (e.g. the `NPArt` copies of the Now Playing scripts).

**Hermetic gate disks.** On a simdisk-cache hit the gate wipes `<build>/simdisk`, reinstalls it (`make install`) and only then restores the cached fixtures, so a stray file left in a build's simdisk (an ad-hoc episode, a test song) is never indexed or counted; a disk is always install + cache, never leftovers.

When a gate fails a golden, `out/gate-report/index.md` links one
`[golden | actual | diff]` composite per differing shot.

Shots that depend on timing (taken mid-animation, or while the scrollbar is
showing) must never get a golden. List them in the script:

    # golden: skip push_mid pop_mid

(one or more shot names, nothing else on the line; several such lines are
fine). `validate.sh` prints `skip golden ...` for them even if a golden
file exists.

## Review videos (`record`, Revision 2 of `docs/phase-nav-brief.md`)

For the owner round, a script can record a small H.264 MP4 instead of (or
alongside) still shots, with the `record <ms> <fps> <name>` sim command
(above) and `scripts/make_video.sh`:

    SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/<script>.script
    scripts/make_video.sh out/shots/<script>/<name>_frames <fps> out/shots/<name>.mp4

320x240 (the LCD's native size, no scaling), a few hundred KB to a few MB
for a short clip. `ipodui-nav-drift-review.script` and
`ipodui-nav-transitions-review.script` are the two `#38` review videos
(`drift-review.mp4`: the main menu resting on Music for three drift
cycles, ~21 s; `transitions-review.mp4`: push/pop split -> split and
split -> full-screen, and the Clocks reveal stub) — pin the clock and
battery first (`sim_clock`, `clock_format`, `battery`, above) for a
deterministic recording, and (drift) `drift_phase`. `record` does not
block the UI thread, so a script calls it right after the `key`/`wheel`
that starts an animation, to capture it as it plays; repeating `record`
with the same `name` later in the same script (e.g. after the push, for
its pop) continues one clip instead of starting a new one.

## Trace assertions (ipodui-* only)

The shell logs every LCD push and animation event through `UPDATE_TRACE`
(stderr in the simulator, so it lands in `out/shots/<script>/sim.log`).
`scripts/check_trace.py <sim.log> <script>` checks them; `validate.sh` runs
it for every `ipodui-*` script:

| Rule | Lines | Passes when |
|---|---|---|
| band | `update band y= h=` | h <= 2 rows, or a whole-list shift (h a multiple of the row pitch, inside the body) |
| full | `update full` | count <= 1 + `key`/`hold` SELECT/MENU lines (boot + push/pop; animated slides need none) |
| boost | `anim start boost=N`, `anim end boost=N` | starts == ends, never an end without a start, last end has boost=0 |
| frame | `anim frame <ms> [cpu=<us>]` | mean CPU per frame <= 8,000 us when logged (the sim measures 0.4 to 1 ms) (`--max-frame-cpu-us`, provisional until calibrated); mean interval <= 50 ms (sim sanity only), the first frame of each animation left out; a late run whose frames cost under half the CPU budget passes as "starved by the host", and a late run without CPU figures on a host the `sim_load` jitter watcher saw stalled (p95 >= 20 ms) fails with "starved" in the line. `<ms>` is the interval since the previous frame; a non-decreasing run starting above 200 is read as timestamps and differenced |
| anim | `update anim:scroll/anim:slide y= h=` | inside the list body: y >= header (20), y + h <= 240; scroll h <= 200 (view), slide h <= 220 |
| marquee | `update marquee y= h=` (scripts named *marquee*) | at least one, each h <= 25, no `anim start` while it runs |
| panelsync (every script) | `panel want key= arg=`, `panel show key= arg= how=` | each show equals the last want on key and arg; a show before any want fails; `# trace: panelsync` also requires at least one show. The old 0.4 s / 1.1 s `rest` minimum is gone (rest delay 0) |
| p11-arena | `arena push s= n= free=`, `arena full need=`, `mem core_alloc s=` | every script: no `arena full` (unless `# arena: full-ok`), no screen-state `core_alloc` |
| p11-search | `search q= kind_n= n= us= yields= refine= cap=` | `refine=1` on each appended letter after the first, `refine=0` on q=1, `yields >= n_entries/4096` |
| p11-otg | `otg add/resolve/play/save/clear` | add `reads <= 1 + n`; resolve `reads == ceil(bytes/32 KB)` and one per open; no `playlist_create` of `otg.m3u8` after `otg play`; no write inside an animation |
| p11-hold | `hold open ctx= items=` | the mask is `ui_hold_items()` for the context |
| p11-vlimit | `vlimit set= code= applied=`, `vlimit unlock ok=` | `applied <= set`; a locked change needs `unlock ok=1` first |
| p11-about | `about page=` | page 1..4 (the 4th: Save Diagnostics); a >= 3 s chord shows `devmode enter` |
| p11-sys | `sys charging state=/pop=`, `sleep: charging` | follows `sleep: charging`; `# sys: dark` needs the backlight-off path, no nested loop |
| p11-qw2 | `np art src= reads= us=` | `reads <= 1`; `# qw2: bundle` keeps `src=bundle`; `np_art_render` only after legacy/ph-render |

A log without any `anim` lines (a Phase 2 build) skips the last three rules
with a note.

Library rules (Phase 4); each skips with a note when its lines are absent,
except that a script with a `# trace: lib` line fails without `lib open`
and `lib fetch` lines:

| Rule | Lines | Passes when |
|---|---|---|
| libopen | `lib open <cat> n=<count> ms=<t>` | every ms <= 300 (`--max-open-ms`, sim sanity) |
| libfetch | `lib fetch n=<rows>` | inside an animation (`anim start`..`anim end`), rows summed per frame (between `anim frame` lines) <= visible rows (8, `--visible-rows`) + 2 |
| books | `lib row <cat> <label>` (optional) | no row of artists/albums/songs/genres/composers/compilations/shuffle (`lib_` prefix and case ignored) has a label from `tests/ui/library_manifest.txt` |

Art rules (Phase 4b, docs/phase4b-brief.md); each is skipped with a note
when its lines are absent, so a log from before Agent B's builder or
Agent C's rows land passes silently. None of this code exists yet, so
these lines aren't emitted by anything today; they are the contract
those agents log to once they do:

    art get key=<hex> row=<n>                 row <n>'s thumbnail
                                                fetched from the cache
    art build key=<hex> boost=<n>              the builder processed
                                                one album; n = boost
                                                count after (like
                                                "anim start/end boost=")
    art io <read|write> key=<hex> playing=<0|1> active=<0|1>
                                                a builder disk read or
                                                write; active = whether
                                                storage was already
                                                active for an audio
                                                refill at that moment

| Rule | Lines | Passes when |
|---|---|---|
| artbuild | `art build key= boost=` | every line has boost=0 (the builder never holds boost, docs/phase4b-brief.md Revision 1 Decision 2) |
| artio | `art io <op> key= playing= active=` | no line has playing=1 and active=0 (the builder never wakes storage on its own while playing, Revision 2 D1) |
| artglide | `art get`, `anim start`/`anim end` | no `art get` line falls between an `anim start` and its `anim end` (thumbnails are fetched only at rest, never mid-glide) |
| artonce | `art get key= row=` | inside any one rest window (between the end of one glide and the start of the next), no row number repeats: at most one `art get` per newly visible row |

Navigation rules (`#38`, `docs/phase-nav-brief.md`); each is skipped with
a note when its lines are absent, so a log from before agent A's
`preview.c` lands passes silently. `panel strip claim owner=<PANEL|DRIFT>
top=<screen name>` is already in the contract (`apps/ipodui/preview.h`,
the strip-borrowing comment); the other two are `check_trace.py`'s own
proposal for `preview.c` to log, the same `UPDATE_TRACE` way as every
other line here (plain `logf()`, as `apps/ipodui/screen_stack.c`'s
push/pop lines use, never reaches `sim.log` in this tree — checked
empirically — since nothing in `apps/ipodui/` sets `#define LOGF_ENABLE`;
`UPDATE_TRACE` is the one mechanism that does):

    panel arrive owner=<name> wait=<ms>        a highlight cross-fade
                                                began (the selection
                                                rested); wait = ms since
                                                it last moved or the
                                                screen was pushed/popped
                                                onto
    panel drift step=<kind> boost=<0|1> lit=<0|1>
                                                one drift tick
                                                (nav_drift_act); boost
                                                must always be 0 (D2:
                                                "unboosted"); a correct
                                                build only ever logs this
                                                with lit=1 (drift is idle
                                                while dark), so the rule
                                                checks "no line has
                                                lit=0" rather than
                                                relying on no lines
                                                appearing — it catches a
                                                regression that still
                                                ticks in the dark instead
                                                of passing it silently

| Rule | Lines | Passes when |
|---|---|---|
| rest | `panel arrive owner= wait=` | every wait >= 400 ms (`NAV_REST_ICON`, the smaller of the two rest delays) minus one frame |
| driftboost | `panel drift step= boost= lit=` | every line has boost=0 |
| driftdark | `panel drift step= boost= lit=` | every line has lit=1 |
| stripclaim | `panel strip claim owner= top=` | no line's `top=` is a screen `#38` knows is full-width (a conservative, name-based allow-list: it only ever flags a name it can positively confirm is not split, never one it doesn't recognize) — D15 |

`update anim:slide`/`update full` lines are also checked for width, from
`ui_lcd_push()`'s own trace (`apps/ipodui/anim.c`): full width omits
`x=..w=..`, anything narrower logs them.

| Rule | Lines | Passes when |
|---|---|---|
| fullwidth | `update anim:slide`/`update full` | neither has `x=..w=..` (panel-only, 160 px pushes are fine for `panel arrive`/`panel drift`; only the screen-level slide/full redraw must be full width) |

D7 transition durations (`nav_transition_ticks()`,
`apps/ipodui/nav_core.c`): a script declares what it expects, in the
order its push/pop transitions occur:

    # transitions: <ms> <ms> ...

| Rule | Lines | Passes when |
|---|---|---|
| transitions | `anim start`/`anim frame`/`anim end` | the Nth declared ms matches the Nth observed boosted animation's total duration within one frame (30 ms, `UI_ANIM_FRAME_TICKS`); skipped with a note without the directive, fails on a count mismatch |

`transitions` assumes nothing else animates between the script's declared
push/pop transitions (no concurrent list glide) — a push/pop's own slide
is always the very next boosted animation after the key that triggers it,
so a dedicated timing script like `ipodui-nav-transitions.script` needs
no screen-name bookkeeping to match them up in order.

## Fonts

`scripts/check_fonts.sh build/sim-ui` (run by `validate.sh` after the
sim-ui build, which is reinstalled first) reads the shell's font list from
`apps/ipodui/fonts/MANIFEST` (one `.fnt` file name per line, first field;
`#` comments) and fails if one is missing from
`build/sim-ui/simdisk/.rockbox/fonts/`, is not a valid RB12 font, or differs
from the committed copy next to the MANIFEST. Without a MANIFEST it skips
with a note. `scripts/fntinfo.py <fnt>...` dumps a font header (height,
ascent, maxwidth, depth, glyph count).

## Host tests for the shell (`tests/ipodui/`)

`tests/ipodui/run.sh` (run by `scripts/test.sh`) compiles
`apps/ipodui/scroll.c` with the host gcc and runs `test_scroll.c` on the
`scroll.h` contract (docs/phase3-brief.md): convergence within 200 steps
(max <= 1500 px, dt 1..4), bounds, zero overshoot, monotonic approach from
rest, `set_max` clamping, target changes mid-flight (converges, at most one
direction reversal), and `ui_wheel_steps` (>= 1 for clicks >= 1,
non-decreasing in velocity, 1 step for a slow click), plus
`ui_scroll_limit_lag` and `ui_scroll_snap`. `test_bands.c` compiles
`apps/ipodui/screen_stack.c` with stubbed `config.h`/`system.h`/`logf.h`
(`tests/ipodui/stub/`) and checks the dirty-band coalescing. Until
`apps/ipodui/scroll.c` exists it tests the throwaway
`tests/ipodui/ref_scroll.c` instead and says so.

`test_sortkey.c` checks the `sortkey.h` contract (docs/phase4-brief.md):
"The Beckons" keys as `b…` and sorts between Basalt and Björvik, "Theory"
keeps its "The"; case and accents fold (Björvik = Bjorvik, Émile under E);
"10dd", "!!!", "2Gem", "(Untitled)", "Ωmega" ... get '#' and sort after
every letter (the 2.0.5 software lists numbers and symbols after Z, #34);
50 shuffles of a 33-name list sort to the same expected order with
non-decreasing quick-scroll letters ('#' last), and equal keys keep input
order; NULL/empty input, truncation at `keylen` (nothing written past it),
malformed UTF-8, and return value == key length.

## Framebuffer versus panel (#57)

Screenshots read the framebuffer, so a screen drawn but never pushed to the
LCD (the device-only stock-list bug) still looks right. `# env:
SIM_TRACE_LCD_PUSH=1` makes the simulator log every panel push
(`sim lcd push x= y= w= h=`). When the log has those lines, `check_trace.py`
applies the `lcdpush` rule to every `shot`: the pixels that changed since the
previous shot (first shot: any non-black pixel) must lie inside the union box
of the pushes logged in between; an unchanged screen needs none, so it never
false-positives. For one transition, also assert a full push explicitly:

    # env: SIM_TRACE_LCD_PUSH=1
    # logbetween: dev_in dev_shown \bsim lcd push x=0 y=0 w=320 h=240\b
    ab_mark dev_in
    ...
    ab_mark dev_shown

Use both on every script that shows a stock (non-shell) screen
(`ipodui-p8a-devmode*`, `ipodui-p9-*devmode*`, `ipodui-p11-about-chord`,
`ipodui-p9-lang-alloc`).

## Phase 8b-0 tooling (`docs/phase8b-brief.md` 4.0)

### Perf traces (`apps/ipodui/ptrace.h`)

Tiny hooks, `PT_VAR`/`PT_LAP`/`PT_GAP`/`PT_TRACE`, compiled in only for the
simulator (or a firmware built with `-DUI_PTRACE`, where they need
`USEC_TIMER`; the trace itself still needs a sink there, `logf` is a no-op).
Otherwise each is an empty statement: no code, no data, no timer read.
Simulator microseconds are **host CPU time**: relative cost and the shape of
the work, never a device figure. The device run needs a `UI_PTRACE` firmware
whose `UPDATE_TRACE` goes to `ui_log`; what it must measure is listed below.

| Line | Emitted by | Fields |
|---|---|---|
| `rowart fetch key= res= lookup_us= read_us= decode_us=` | `rowart.c` `fetch()` | art-cache index lookup, then the pak read; thumbnails are stored ready to blit, so `decode_us` is always 0 (a non-zero value fails) |
| `rowart draw key= thumb= draw_us=` | `rowart.c` `ui_rowart_draw()` | the blit of thumbnail (1) or glyph (0) into the framebuffer |
| `rowart fill row= fill_us= push_us= px= since_glide_ms=` | `list.c` `fill_art()` | an at-rest fill of a row that got a stand-in during the glide: the row redraw, the LCD push of its art box, and the wait from the last glide frame |
| `panel cost kind=pan\|fade step= blit_us= push_us= gap_us= px=` | `preview.c` `pan_to()`, `drift_fade_step()` | one drift pan step (window blit + panel push) or one cross-fade step (`xfade_rect` + push); `gap_us` is the wall time since the previous step of that kind (frame pacing; 0 for the first, large after a pause) |

Rules in `scripts/check_trace.py` (`check_p8b`; a script selects one with
`# trace: p8bperf p8bpan`, and a selected rule that finds none of its lines
fails; with the lines present it always runs):

* `p8b-rowart`: `decode_us` is 0; no fetch, draw, fill or push stage over 20 ms (sim
  sanity); every fill starts within 1000 ms of the last glide frame. Prints
  min/mean/max per stage and the worst fill, so the slow stage is named.
* `p8b-pan`: no compose or push over 20 ms; a fade step is 1..25. Prints compose,
  push and gap stats, an **estimated** instruction count per step (0.25 per
  pixel for the window blit, 14 per pixel for a fade step; the method of the
  `panels.c` comment, not a measurement) and how many pan steps skipped a
  pixel. `# pan-max-jump: N` turns the skipped-step count into a failure.

Scripts: `ipodui-p8b-rowart-latency.script` (Albums and Audiobooks, a quick
turn out and back) and `ipodui-p8b-pan-cost.script` (the main menu's drift,
unfrozen from tick 0, so the same steps occur on every run; re-run it after
any shadow or compose change to compare). First numbers in the sim:
fetch lookup mean 12 us / read 42 us, draw 0 us, fill 75 us + push 202 us,
fill 20-60 ms after the glide; pan compose 9 us + push 544 us per step,
a step every 162 ms, fade compose 64 us (max 93) + push 506 us, one per
50 ms. On the device measure the same four stages for the row art (the pak
read is the storage wake: flash vs HDD) and, for the pan, compose and push
microseconds plus the step-to-step gap, with the shadow on and off.

### Side-by-side harness (`tests/review/p8b-sxs.sh`)

Owner-only, never in the gate. `tests/review/p8b-sxs.txt` lists rows
`<name> <reference> <script> <shot>`; the harness runs each existing script
once, composes the owner's photo (`docs/reference/7g/owner/labeled/<reference>.jpg`,
local-only, found in this checkout or the main one) at 640x480 next to our shot, and
writes `out/review/p8b/<name>.png`, a `<name>.notes.md` template (geometry,
colour after cast correction, weight and softness, icon shapes, text) and
`index.md`. `-n` reuses existing shots, `-d` picks the photo directory, `-t`
another table, trailing arguments name rows, `P8B_SXS_OPEN=1` opens the
results. It **skips cleanly** (message, exit 0) when the photo directory,
ffmpeg, sips or the sim build is missing, and per row when the photo, script
or shot is missing (the 8b screen scripts that do not exist yet).

### Style gallery (`art/gallery.py`, `ipodui-p8b-gallery.script`)

Host-side only, stdlib only, no firmware bytes. `art/gallery.py` draws every
`ui_mat*` colour table and the `ui_shadow_a` alphas of the generated
`apps/ipodui/material_data.h` (skipped with a note until 8b-1 lands it) above
the shots of `ipodui-p8b-gallery.script` (battery 0/9/50/100 %, charging,
hold lock, play, pause, main menu, Settings, About x 3), into
`out/review/p8b/gallery.png` plus `gallery.txt`, the cell legend. Expected
table format: C arrays named `ui_mat*` of RGB565 literals (a row per entry)
and `*shadow*` arrays of 8-bit alphas; `material_data.h`'s author keeps this
in step or edits `parse_tables()`.

### Split-shadow probe (`expect_shadow`, `ipodui-p8b-shadow.script`, 8b close)

`uisimulator/common/sim_cmd_shadow.c`: `expect_shadow <label> [tol_per_mille
[min_sum]]` reads the panel (x 160..319) of the LCD framebuffer and
checks the split shadow at the seam (`ui_shadow_a` = 92 75 59 45 33 23 15 8
4 1 of 256, 10 px, 36 %), with the spec's numbers copied into the sim, not
read from the shell. The true picture under the shadow is unknown, so it
uses the rows where columns 12..27 are flat and at least `min_sum` bright
(r+g+b of 0..765; default 500, the cover's cream strip; 110 for the dark
glyph panels): the median ratio of column c to column 12 must be within
`tol` (default 80 = 8 %) of `(256 - a[c]) / 256`, aggregated over columns
0..4, and columns 0..2 each within 2x. Twice applied reads 0.57, not applied
1.00 (both verified against a patched `ui_shadow_apply`). Fewer than 8 such
rows is a failure (a label starting with `~` is soft: only a note, for mid-fade frames whose blend may
have none). Failures print the label, the measured and the expected
per-mille and which way it is off, and fail the run in `shots.sh`.
`ipodui-p8b-shadow.script` probes a drift whole step (phase 190) and half
step (phase 200), a highlight fade (Music to Podcasts) and a cover fade
(back), plus the settled frames. The fade probes (labels with `~`) are
timing-dependent (the sim fades in about 200 ms and a script wait cannot be
pinned to the frame), so a run may catch none of them; the whole-step,
half-step and settled probes are exact. Dark panels measure about 704 of an
expected 762 per mille (rgb565 quantisation of dark values), a 58 point
bias inside the 80 tolerance, twice-applied (about 520) well outside it.

## Phase 14a: video scripts, fixtures and the `sim-ui-video` build (`docs/phase14a-brief.md` 7 A2)

**Build.** `ipodui-p14a-*` scripts run on `build/sim-ui-video`
(`--target=22 --type=s --ipodui --bcmfw-video`), not `build/sim-ui`;
`scripts/validate.sh` picks the build by name (`script_sim()`), and
`scripts/shots.sh` defaults `SIM_BUILD` to it for those names. The same gate
also builds `build/fw-ui-video` (`--ipodui --bcmfw-devtest --bcmfw
--bcmfw-video --bcmfw-l1`, compile-only `make bin`, after the parallel builds):
its `.text` is the baseline for the brief's 24 KB budget (the size step prints
it next to fw-ui's). `--quick` builds `sim-ui-video` only when `apps/`,
`uisimulator/`, `firmware/`, `lib/`, `bcmfw/` or a p14a test file changed
(`GATE_SIM_VIDEO=0` drops it), and `fw-ui-video` only when `apps/`, `firmware/`,
`lib/`, `bcmfw/` or `tools/configure` changed (`GATE_FW_VIDEO=1` forces it).
A p14a script is skipped with a note when its sim was not built. The gate primes
two base disks for that build (`script_disk()`): `simdisk` (the mini music
and the video fixtures) for the p14a scripts, whose counts and covers are the
fixtures', and `simdisk.lib` (the same plus the Phase 4 test library) for the
`ipodui-p19-*` job-model scripts, whose drift needs the library's albums. The UBSan pass
skips them (`sim-ui-ubsan` has no video player).

**Fixtures** (`tests/fixtures/videos/mkvideos.sh [--install <simdisk>] [<outdir>]`).
Generated under `build/video-fixtures/` (never committed, about 9.5 MB), from
the committed CC BY clip `bcmfw/video/stage6-real.mp4`, byte-reproducible
(`SHA256SUMS` lists every file; a stamp of this script's and the clip's hash
makes a rerun instant). `--install` copies the tree (mtimes pinned) into a sim
disk. Layout: `Movies/{Short Film (poster, chapters 0/4/8 s), Late Chapter
(sidecar jpg, first chapter at 2 s), High Profile (B-frames: audio fallback)}`,
`TV Shows/Show A/Season 1|2/` (tagged, 2 x 2, S01E01 has a poster), `TV
Shows/Show B/Season 1/` (untagged), `Misc/{Show.C.S01E02.m4v, S01E03.m4v,
Audio First.m4v (audio trak first)}`, `Podcasts/Pod One/{Video Episode.m4v,
Audio Episode.m4a}`, `Music/{Fixture Singer/Audio Only/Audio Only.mp4 (a song),
Fixture Author/Image Book/Image Book.m4b (an mjpeg chapter-image trak: a
book)}`. Posters are
synthetic. The gate's prime step runs it for the `sim-ui-video` disk and caches
the result with the database (the cache key gains the generator, the clip, the
classifier and the mp4 reader for that flavour only). `# needs: video` makes
`shots.sh` do the same for a standalone run (all `ipodui-p14a-*` names imply
it), including a priming `build_db` boot.

**`# needs: modules`** (header line, `validate.sh`): the script needs a simulator built with the module loader (`-DIPODUI_WITH_MODULES`, off by default since #106). Since the P12 close the gate runs it on **`build/sim-ui-mod`** (`--target=22 --type=s --ipodui`, then `-DIPODUI_WITH_MODULES` appended to the Makefile's `EXTRA_DEFINES` right after configure; a dir configured any other way is wiped, `.gate-mod-args`), in the full gate and the pool, never skipped. They also run on the UBSan pass's module twin, **`build/sim-ui-mod-ubsan`** (`UBSAN_VARIANT=mod scripts/risk/sim_ubsan.sh`; the default UBSan sim has no loader and leaves them out), in the full gate, the pool (`ubsanmod` jobs) and every `--quick` that built `sim-ui-mod`. **A misaligned access in `apps/ipodui/sdk/`, `apps/ipodui/games/` or `apps/ipodui/modload*` is a `STRICT:` finding and fails the gate** (the ARM7TDMI aborts a misaligned halfword or word access; the host does not: the P12 games crash, `quirks.md` #51). Every other UBSan finding stays report-only; `UBSAN_STRICT_RE` overrides the path pattern. `scripts/golden_capture.sh` picks the same sim. Standalone: `SIM_BUILD=build/sim-ui-mod scripts/shots.sh ...`.

**Module builds in the gate (P12 close).** The simulator links modules statically, so the pack is only exercised on the firmware build. `validate.sh` also builds **`build/fw-ui-mod`** (`--target=22 --type=n --ipodui`, the same define, then `make zip`), removing `modules/check.ok` first so `modpak.py check`, `modules.pak` and the patched `rockbox.bin` are this run's. The **"module pack"** step then runs `tests/modpak/run.sh` strictly (`GATE_BUILD_STAMP=build/.gate/build.stamp`, `GATE_MOD_DIR`: `test_modpak.py` FAILs on a missing check.ok, one older than the run's build marker, or one older than its ELFs, `modpak.py`, manifests or deny lists; without the stamp, as in a plain `scripts/test.sh`, those are notes) and `tests/modload/run.sh`, which, when `tests/modload/test_modload_pak.c` is present (p12-modfix), must print both its `[developer]` and `[release]` runs on the pack. `--quick` builds `sim-ui-mod` and `fw-ui-mod` only when `apps/ipodui/modload*`, `apps/ipodui/modules/`, `apps/ipodui/sdk*`, `apps/ipodui/games/`, a top-level `modules/`, `sdk/` or `games/`, `tools/modpak.py`, `tests/modpak/`, `tests/modload/`, `scripts/validate.sh` or a `# needs: modules` script changed; `GATE_MOD=1` forces them, `GATE_MOD=0` drops them.

**Runtime native illusion (`check_trace.py` rule "illusion", P12 close).** In a simulator shell build every string the LCD draws passes `sim_text_drawn()` (one call in `firmware/drivers/lcd-bitmap-common.c`'s `putsxyofs`, where `lcd_putsxy`, list rows, header titles, splashes and scrolling lines all end) and `str()` passes `sim_lang_str()` (the generated `lang.h`, `tools/genlang`). Both live in `uisimulator/common/sim_cmd_text.c` and log only problems: `sim text: forbidden x= y= text="..."` once per distinct text containing "rockbox" in any case (".rockbox" too; x/y are screen coordinates for direct draws, strip coordinates for list rows drawn off screen), and `lang: str id=<n> zero|out-of-range at apps/ipodui/<file>:<line>` once per call site in the shell's own files (an unlabeled LANG id reads id 0, "Rockbox"; an out-of-range id returns "" instead of reading past the array). Every `ipodui-*` script FAILS on either. Exceptions: `tests/ui/illusion_allow.txt` lists sentences that may name Rockbox (the Legal credit), matched as fragments of four words or more; `# illusion: devmenu <why>` makes drawn text a note for a script that opens stock developer menus (str() warnings still fail). The first draw logs `sim text: watch on`; the gate fails a run in which an `ipodui-*` log lacks it. Self-test: `tests/trace/test_illusion_rules.py`. Device developer builds have no such hook yet (it needs an `apps/` edit; see the P12 close notes).

**`# reset: mainmenu`** (header line, `shots.sh`): the script starts from the default Main and Music Menus (the `ipodui main menu:` and `ipodui music menu:` lines leave `config.cfg` before boot). Every script that counts wheel steps from the Main Menu needs it: a checklist script that ticked a row moves every row below it (P12 integration). `# reset: clocks` and `# reset: books` work the same way for the Clocks list and the per-book resume store.

**`# keep: alarms`** (header line, `shots.sh`): every run otherwise starts with no alarms (`.rockbox/ipodui/alarms.dat`, `.tmp`, `.old` deleted before boot, as the sim PMU state is), so an alarm one script made never rings in the next.

**`# pending: <reason>`** (header line, `validate.sh`): the script was written
ahead of the code it tests; the gate skips it with its reason (and
`GATE_PENDING=run` runs it anyway). Delete the line to un-pend. Unlike
`tests/ui/PENDING` it lives in the script, so it needs no shared file.

| Script | Checks |
|---|---|
| `ipodui-p14a-menu` | main panel with posters, Videos rows, then (all videos removed) the empty panel and rows |
| `ipodui-p14a-movies` | Movies list, thumbnails, no wrap |
| `ipodui-p14a-tv` | show > seasons > episodes: tagged, folder-only (Seasons skipped), filename and nameless shows |
| `ipodui-p14a-podcasts-mixed` | glyph; video episode opens the video screen (`expect_video`); audio opens Now Playing; audio plays on past a video episode without playing it |
| `ipodui-p14a-play` | `expect_video 30 playing`, pause, overlay, `video_shot`, MENU, reopen resumes within 1 s |
| `ipodui-p14a-chapters` | Next lands on chapter 2 +-0.5 s, ticks on the overlay, speed stays 1x with the audiobook speed on Faster (`# nolog: ab speed 125`) |
| `ipodui-p14a-fallback` | a video refused at open fails entirely (owner 2026-10-13): nothing plays, the light failure prompt (High Profile: format; a non-MP4 `.m4v`: damaged), SELECT and MENU back to Movies, no Now Playing row (`# logafter: home np row visible=0`) |
| `ipodui-p14a-error-runtime` | the player failing mid-play (faked by `/.rockbox/.sim_vfail<why>`, sim only, read in `ui_video_play()`): 6 too large, 8 damaged, -2 didn't start, 5 format; audio stopped (`page=... audio=0`), PLAY swallowed, SELECT/MENU back to the list |
| `ipodui-p19-vcheck` | the first-use video check (`docs/research/bcm2722/first-use-check.md`; `# env: SIM_VCHECK=1` opens the clock gate on the sim's model and gives it `HP_OP_VCLK`; the script copies `bcmfw/video/vcheck.bin` onto the disk, which the sim's install leaves out): "Preparing Video..." and a PASS at 150 MHz on the first play, none on the second (the token), a step down to 147 MHz with `/.rockbox/.sim_vcheck_band` (the top band's switch falls back), and the "Video check failed" prompt with `/.rockbox/.sim_vcheck_bad` (every picture from the sixth decodes wrong); run on `build/sim-ui-video` |
| `ipodui-p14a-settings` | Videos > Settings rows and persistence across a reboot |
| `ipodui-p14a-about` | capacity legend and the Videos count (`# log: about videos=12`: movies + TV, the video podcast under Podcasts) |
| `ipodui-p14a-classify` | audio `.mp4` in Songs, the m4b in Audiobooks, no video in any music list; Search "audio" finds the song, not the movie (`search screen q=5 n=4`); Shuffle Songs queues the 6 songs only (`shuf: start songs ok queued=6`), Shuffle Albums too, and no track change starts the player |

Video pictures move, so shots taken while playing are `# golden: skip`;
`video_shot` writes the panel picture beside the shots. Row positions were
confirmed against the merged screens at integration (main menu: Music,
Videos, Podcasts, Extras, Settings, Shuffle Songs; Videos: Movies, TV Shows,
Settings; Movies: Audio First, High Profile, Late Chapter, Short Film).
`expect_video 0 none` checks that the player is not running (`idle` is a
running player's state before its first picture). The music fixture song is
by Fixture Singer: Fixture Author is an audiobook author in
`library_manifest.txt`, which `check_trace.py` bans from music lists.

**Shell video commands** (`sim_cmd_video.c`, builds with `HAVE_BCMFW_VIDEO` and
`HAVE_IPOD_UI`; run on the script thread, declared in `apps/ipodui/screens/video.h`):

    video_screen <path> [tc]
    setvideos

`video_screen` calls `ui_video_play(path, tc)`: the shell's video screen on a
sim path, without the Movies list (`tc` is the 0-based tagcache master index,
default -1). `setvideos` calls `ui_setvideos_push()`: Videos > Settings. They are
the way V6's screens are reached directly; `ipodui-p14a-screen` and
`-setvideos` use them (moved from V6's `tests/video/ui_video_screen.script` and
`ui_video_settings.script`; the clip is the fixture `/Misc/S01E03.m4v`, no
`/Videos/clip.mp4`, which the scanner would list as a movie). V5's
`ipodui-p14a-v5-videos.script` is folded into `ipodui-p14a-menu` (its empty
Movies / TV Shows lists run after the fixtures are removed); do not merge both.

**Library boot recovery (#85)** (`sim_cmd_dbrec.c`, `--ipodui` sims; full
list in `sim_cmd_dbrec.h`): `dbrec <sub>` damages the test disk's database
generations, manifests or statistics journal (`corrupt_newest`,
`corrupt_referenced`, `truncate_newest_manifest`, `foreign_manifests`,
`partial_legacy`, `corrupt_journal`) and checks the next boot against a
mark kept in `/.rockbox/dbrec.mark` (`mark`, `mark_files`,
`expect_selected`, `expect_active`, `expect_unchanged`,
`expect_slots_changed`, `expect_published`, `expect_not_updating`,
`expect_retries`, `expect_prepares`, `expect_journal_bad`). A failed check or
setup logs `dbrec expect_<x> failed`, which fails the run.
`ipodui-library-boot-recovery` runs scenarios A-G of the review (newest
generation corrupt, torn manifest, foreign manifests, both generations bad,
partial legacy root, damaged journal, bounded retries) across 12 boots.

## Audio-buffer floor and measurement commands (Phase 16-A / 16-0)

Defined in `uisimulator/common/sim_cmd_floor.{c,h}`; used by
`tests/perf/floor-50k.script` through `tests/perf/floor_gate.sh [32|64]`
(also run by `scripts/validate.sh`, step "audio buffer floor"; `FLOOR_GATE=0`
skips it). The floor is 7,500,000 B (32 MB unit, `build/sim-ui32`) and
39,000,000 B (64 MB unit, `build/sim-ui`) for `audio_buffer_size()` at 50k
tracks (`docs/hardware.md`, budget rules).

    floor <label>                  log the buffer now, fail below the floor
    floor_watch begin [timeline]   a host thread keeps the minimum buffer
                                   (and free heap) seen; "timeline" logs a
                                   "floor_tl ms= free= audio=" line per
                                   256 KB move of the free heap
    floor_watch end <label> [free] log the minimum, fail under the floor
                                   ("free": the free-heap minimum only)
    floor_reserve <slot 0-3> <B>   a real core_alloc (a shell reservation)
    floor_release <slot>           free it

Failure text: `expect_floor failed: <where> <label>: audio file buffer N B <
floor F B (M MB unit, S B short; docs/hardware.md budget rules)`, which
`scripts/shots.sh` turns into a script failure. The #88 hook: a script that
runs the paused window adds `floor after_shrink` where
`tests/perf/floor-50k.script` has `floor after_shrink_hook`. The USB prefetch
ring is not measured (it is taken only while audio is idle).

Report scripts (not gates): `tests/perf/scan_trace.sh` (where a no-change
scan goes; needs the devmode `scantrace` lines, `docs/research/p16-measure-0.md`),
`tests/perf/commit_peak.sh` (free heap per commit phase) and the resume
stand-in `tests/perf/resume-shrink-50k.script` (run by `floor_gate.sh`).

## The paused maintenance window (Phase 16-1, #88)

`uisimulator/common/sim_cmd_maint.c`:

    pause_age <s>     the current pause began <s> seconds ago
    maint_try <B>     maint_take(commit, B), then core_alloc(B): "maint try
                      ok= alloc= shrink=" (shrink must be 0)
    give_hold <ms>    the next window restart waits <ms> after the buffer
                      change (events queue up behind it)
    expect_title      the current track has a title and a length (what Now
                      Playing draws), else "expect_title failed"
    maint_hold <job> <B>  job (commit, chapters, dbq, lists) takes the window
                      for B and holds a core_alloc(B) across commands
    maint_drop <job> [ok] frees it and ends that tenancy only
    audio_mark        remember the audio file buffer's size
    expect_audio_back [ms]  it is back within 64 KB of the mark (the regrow)

`sim_cmd_dbrec.c`: `db_opens_mark` / `expect_db_opens <max>` (master index
opens since the mark). `sim_cmd_sys.c`: `commit_hold 100` holds a
candidate's publication before its view pool (PLAY during publication).

The window (`apps/ipodui/maint.c`) opens after a 3-minute pause with the
user idle (`backlight_expire`); `pause_age 200` stands in for the wait.
Its lines are `ipodui: maint window open ...`, `maint take job=<commit|
chapters> ok= need= audio=<before>-><after> ms=`, `maint job=<j> wait
floor need= audio= floor=` (refused: Q11) and `maint job=<j> ok= ms=
play_during=` (done). `ipodui: stats finish idx= elapsed=` is every
statistics event (tagtree.c); the window's silent restart sends none.

| Script | Checks |
|---|---|
| `ipodui-p16-paused-window` | dark but young: closed; old but lit: closed; old and dark: open |
| `ipodui-p16-paused-restart-state` | a paused shrink restarts the song at the same spot, still paused: no `stats finish`, no `ab store w=`, no DAC wake (`dac standby=0`) between the marks; PLAY resumes |
| `ipodui-p16-chapters-paused` | the chapter-image book that waited (`rc=-6`) builds in the window, paused; a later update lands in the next window |
| `ipodui-p16-play-mid-commit` | PLAY while the window's commit is held (`commit_hold 1`): playback resumes at once, the commit publishes, `play_during=1` |
| `ipodui-p16-restart-{stop,play,pick,usb}` | an event during a held restart (STOP, PLAY, a song picked, USB): handled after it, the restart ends (`audio give done`), PLAY resumes (status paused throughout), events alive afterwards (`stats finish` on a later stop) |
| `ipodui-p16-restart-wake` | a wake and PLAY during a held restart: the title stays (`expect_title`), the UI thread is not blocked (the list moves before `audio give done`), the song resumes after |
| `ipodui-p16-regrow-churn` | after the pause's one regrow, allocation churn goes through the one-event restart: no second regrow, no `stats finish`, still paused at the spot |
| `ipodui-p16-two-takers` | a second taker while the first's restart is held: `take queued`, serialised, both ok |
| `ipodui-p16-take-boundary` | `maint_try` B then `core_alloc(B)`: no second shrink; the buffer grows back after the jobs (`take job=regrow ok=1`) |
| `ipodui-p16-window-lit` | charging, lit, no key: closed; dark: open |
| `ipodui-p16-tenant-ends` | a queue's transient (`dbq`) and a commit take one window; the commit ends unfit: no regrow while the queue holds; after it, the regrow gives every byte back (`expect_audio_back ok`) |
| `tests/perf/resort-play-50k.script` | 50k, both units (`resort_play_check.sh`): PLAY right after a stopped commit, and during its publication (`commit_hold 100`): the candidate yields its RAM (and its prepared lookup, `stats=1`), publishes at the stop |
| `tests/perf/resort-pause-50k.script` | 32 MB, 50k: a yielded candidate whose reload does not fit in a paused window: no master index open (`expect_db_opens 1`), no take, the regrow not held off; published at the stop |
| `tests/perf/floor-50k.script` (hook) | 50k, both units: the window's commit lands paused; `floor after_shrink` / `after_shrink_play` and the watch stay >= the floor |
| `tests/perf/paused-floor-wait-50k.script` | 32 MB, 50k, buffer 6 MB smaller: the window refuses (`wait floor`), takes nothing; the update commits at the stop (`floor_gate.sh` checks it) |

Phase 16-2 (#89/#87, `sim_cmd_maint.c`): the no-change boot.

    boot_unclean                    this session ends without the boot stamp
    expect_boot_walk <0|1> [ms] [why]  this boot's decision (0: skipped, no scan ran)
    expect_walks_ge <n> [ms]        at least n scans ran this boot

`# env: SIM_BOOT_NOHOLD=1` skips the boot window's hold and its early start
(the check at the tagcache thread's own timeout, as before #87): for scripts
that need the device's order, where the boot check outlasts the hold and
music resumes before it ends (`ipodui-p4-commit-playing`, `-dirty-playing`,
`ipodui-p8b-footer`). `ipodui-p16-boot-dirty-held` is `-dirty-playing` with the hold.
Every script starts with no boot stamp (`shots.sh` deletes
`.rockbox/boot_stamp.dat`, so its first boot walks, "nostamp") unless it
says `# boot-stamp: keep`. `boot_unclean` stands in for a crash or a power cut before the library
settled (the clean shutdown runs, its stamp write is refused, logged `boot stamp write=0
why=sim_unclean`; since npfix it also removes a stamp the settled library wrote this session,
`boot stamp removed=1 (sim_unclean)`, so the next boot has none). `boot_crash` (npfix) is a crash
after the library settled: later writes are refused, the settled stamp (`boot stamp settle=1`)
stays.
`expect_boot_walk` waits up to ms (default 15000) for the tagcache thread's
decision (`boot stamp walk=<0|1> why=<reason>`; reasons in `bootstamp_core.c`
`boot_why_name()`) and fails on another one, or on a scan in a skipped boot.

| Script | Checks |
|---|---|
| `ipodui-p16-boot-clean` | an unclean end walks ("nostamp"); the next two boots trust the stamp, no scan, skipped 0, 1, 2; the boot window ends "done" |
| `ipodui-p16-boot-changed` | a track added after the shutdown, deleted, renamed, renamed back: each next boot walks ("digest") and picks it up (the deleted one is gone from the library: `expect_not_in_db`); unchanged: trusted |
| `ipodui-p16-boot-usb` | a USB session in a trusted boot forces the post-USB walk; the stamp then counts from it (never skipped=1) |
| `ipodui-p16-boot-unclean` | clean: skip; unclean: walk; clean again: skip |
| `ipodui-p16-boot-tenth` | a walk, nine skips (skipped 0..8), the tenth boot walks ("tenth") |
| `ipodui-p16-boot-rebuild` | Rebuild Cache in a trusted boot walks and restarts the count |
| `ipodui-boot-settle` | npfix: the stamp is written once a completed walk settles (`boot stamp settle=1 skipped=0`); a crash after it (`boot_crash`) trusts it at the next boot |
| `ipodui-p16-boot-dirty-held` | `-dirty-playing`'s cut commit with the boot hold: the boot walks, the window ends in ≤ 1.5 s (never the 3 s cap), music resumes after |

## Phase 12: the PMU alarm stub, the piezo trace and the Extras rules (`docs/phase12-brief.md` 3.6, 9)

Commands in `uisimulator/common/sim_cmd_extras.{c,h}` (registered from
`sim_script.c`). They reuse P18's `rtc_set` / `sleep_warp`
(`sim_cmd_sleep.c`) and the `usb_connect` / `usb_disconnect` of
`sim_cmd_art.c`; there is no `rtc_warp`. Shell builds only (a stock sim logs
`<cmd> unavailable`).

    rtc_at YYYY-MM-DD HH:MM:SS     rtc_set by calendar time (local, as the RTC)
    pmu_arm HH:MM:SS [YYYY-MM-DD]  what the firmware writes: sec/min/hour,
                                   the date at its ignore values (daily) or
                                   the exact date, and RTCWAK
    pmu_disarm                     year 0x99, RTCWAK cleared
    pmu_fire                       latch INT1 bit 7 now
    pmu_dump                       log "sim_pmu: regs= int1= rtcwak= bootwake= rtc="
    expect_pmu on|off|any [hms=HH:MM:SS] [date=daily|YYYY-MM-DD] [rtcwak=0|1]
               [bootwake=0|1] [int1=0|1] [next=<rtc s>]
                                   "expect_pmu failed: <what>" names the
                                   value (hms, date, rtcwak, bootwake,
                                   int1, next = the next match after the
                                   RTC, seconds since 2000)
    alarm_boot <s>                 the NEXT boot is a power-on by the RTC
                                   alarm <s> s after the armed time
    piezo_mark / expect_piezo <min> [max]   "piezo play" lines since the mark
    piezo_test <hz> <ms>           one piezo_play_for_tick (tooling tests)
    press_hold <A> <B> <ms> [lead_ms]   A down, B down lead_ms later (20),
                                   both held ms, then both up: the exit
                                   chord (SELECT+PREV) in either order;
                                   `chord A+B ms` stays the simultaneous form

**The PMU model.** `pcf50605_read/write[_multiple]` and
`pcf50605_wakeup_flags` exist in the sim (`sim_cmd_extras.c`); `stubs.c`'s
`rtc_set_alarm`, `rtc_enable_alarm`, `rtc_get_alarm` and
`rtc_check_alarm_flag` go through them, mirroring `rtc_pcf50605.c`.
Registers `0x11..0x17` hold the image of `alarm_core.h`
(`alarm_pmu_regs()`, `ALARM_REG_IGNORE`, year `0x99` = disabled). INT1
(`0x02`) bit 7 is read-to-clear and is set lazily when the sim RTC passes
the armed time between two looks (the power thread's
`rtc_check_alarm_flag()` looks about once a second; the log line is
`sim_pmu: int1 alarm armed= now=`). So the unfixed `power_thread_rtc_process()`
really does disarm the PMU (the H2 race) and `-alarm-disarm-race` fails
until W1-A1's hunk lands. The tests' reads use `pcf50605_read(0x11..)`
and the `rtc_*` calls; an app that writes the image with
`pcf50605_write_multiple(0x11, regs, 7)` and sets `pcf50605_wakeup_flags |=
0x10` is modelled the same.

**Power-off, reboot, alarm boot.** `shots.sh` exports `SIM_PMU_STATE`
(`<out>/.pmu-state`, removed at the start of a run); the sim saves the
registers, the wake flags, the RTC offset and a pending `alarm_boot` there at
every change and at exit, and restores them in a constructor before `main`.
`pcf50605_wakeup_flags` starts at 0 each boot, as on the device;
`bootwake` is what it held at the last power-off. Put `alarm_boot <s>` in
the part before the power-off (the part ends with `power_off_now` then
`reboot`); at the next boot, if RTCWAK was set and the registers are
enabled, the RTC reads `armed + s` and the sim logs `sim_pmu: alarm_boot
armed=N +Ss`, else `sim_pmu: alarm_boot: no wake (...)` and the boot is plain
(this is how `-alarm-early-poweroff` and `-alarm-disarm-race` see a lost
RTCWAK). The RTC offset of `rtc_set`/`rtc_at`/`sleep_warp` now also
survives `reboot` parts, once the PMU has been used (the file exists only
then; other scripts are unchanged).

**The piezo.** The sim defines `piezo_init/play/play_for_tick/
play_for_usec/stop/clear/busy/hz/button_beep` (the target's `piezo.h`
prototypes). They only log: `ipodui: piezo play hz=<Hz> ms=<ms>` (`hz` is
`91225 / inv_freq`, so `piezo_hz(2000)` logs 2027; `ms` 0 for an
open-ended `piezo_play`), `piezo stop`, `piezo click`.

**The class accountant hook.** `filesystem-sim.c` calls `modclass_io(write,
bytes)` (`apps/ipodui/modclass.h`) for every sim file open (bytes 0), read
and write. A weak no-op in `sim_cmd_extras.c` keeps the sim linking until
`modclass.c` (W1-F) defines the counter.

**Trace rules** (`scripts/check_trace.py` `check_p12`, self-test
`tests/trace/test_p12_rules.py`, run by `validate.sh`). A rule armed by a
`# trace:` token (or by the script name) that finds none of its lines FAILS.

| Rule | Lines | Passes when |
|---|---|---|
| p12modclass | `modclass <name> class=A reads= writes= allocs= boost_pct= fps=`, `modclass viol <name> <rule>` | armed by `# trace: modclass`, by the names `ipodui-p12-{extras,clocks,stopwatch,lock,notes,contacts,calendars,sdk,modclass,mod-swap}*` and `*-while-playing*` (`# modclass: none <why>` opts out): at least one line; and in ANY script no `modclass viol`. `# modclass: steady`: every line after a module's first has `reads=0` |
| p12alarm | `alarm ring i=` | the count equals `# alarm-rings: N` (N may be 0); `ipodui-p12-alarm-*` must declare it; a ring in any other script needs the header |
| p12alarmarm | `alarm arm rtc= ok=` | every line has `ok=1`; `# alarm-arms: N` needs N lines; `ipodui-p12-alarm-*` needs at least one unless `# alarm-arm: none <why>` |
| p12piezo | `piezo play hz= ms=` | only between `alarm ring` and the next `alarm snooze\|dismiss\|missed\|stop` (a `boot screen` ends it); `# trace: piezo` or the name `ipodui-p12-alarm-ring*` needs at least one; `# piezo: free <why>` lifts the condition (the tooling test) |
| p12xidx | `xidx build kind= n= ms=` | a sync moment earlier in the log (`sim_script: usb_disconnect`, `boot screen`, `maint take\|job`, `xidx owed\|window`, `post-usb`), ms <= 60000, count equal to `# xidx-builds: N` when given; `# trace: xidx` needs a line |
| p12jobopen | `mod job_open stk=<n>` | n > 0 (target logs, D-M3); `# trace: jobopen` needs a line |

`ipodui-p12-tooling-pmu` is the self-contained test of the stub (no shell
code needed): arm, disarm, the exact date, INT1 at the clock jump, the piezo
counters, `alarm_boot` across a `reboot` part.

**Floor gate.** `tests/perf/floor_gate.sh` counts the module arena at the
real `MOD_ARENA_SIZE` (largest value in `apps/ipodui/modload.h`, read by
`tests/perf/arena_size.py`; override `FLOOR_ARENA`) instead of the 64 KB
placeholder, and `floor-50k.script` gains `p12_arena_tenant`,
`p12_arena_extra_open` (plus the resident build's Extra-open transient,
`FLOOR_EXTRA_WORK`, default 32768 B) and `worst_stack_p12` (all the
reservations at once). Receipts: `floor p12_...` lines in
`out/shots/floor-{32,64}/sim.log`.

## Now Playing stalls (npfix)

The owner's clock skips (log run-1137). `ipodui-np-stall` runs three cases
with Now Playing lit and `# trace: npstall`: C, a shuffle refill forced
during a buffering fill (`shuf_refill_force`): 5-entry chunks; A, the drive
powered off (`stor_tune`, `stor_expect off`) and the power-up stretched to
600 ms (`stor_pwrup`), then NEXT twice: the covers come from the art
thread's copy (`art npf take`), no UI-thread file call (`stor_watch_ui`);
B, an art pass whose decode is slowed (`art_decode_spin`) caught by the user
coming back: the builder parks (`art park ms=`), the clock steps on.

| Command | Effect |
|---|---|
| `stor_pwrup <ms>` | the sim drive's modelled power-up wait (default 300, `filesystem-sim.c`) |
| `stor_watch_ui on\|off` | while on, every UI-thread file call logs `sim io ui pwr=<0\|1>` (pwr: the drive was on) |
| `art_decode_spin <us>` | the art builder busy-waits that long at each decoder yield point (the ARM's decode cost; holds the cooperative sim kernel as the device's CPU would); 0 off |
| `shuf_refill_force` | the next UI pass begins a library shuffle refill as if the queue were low and storage up |
| `boot_crash` | see the boot stamp section above |

check_trace.py `check_npstall()`: on every log, no `art slice ms= lit=1`
(the builder over 100 ms between yield points while lit and playing), no
`sim io ui` line, every `shuf: chunk +n` with n <= 5; with
`# trace: npstall`, no `np jump` and consecutive `np secdt <s>` lines step
+1 (a track change, a full paint, an unlit exit or a seek starts a new run;
it fails with no step at all).

## Gate machinery: timing, ledger, pool (docs/gate-speed-brief.md)

Always on:

- **Timing.** `build/gate-timing/phases.tsv` (step, start, end, seconds) and `scripts.tsv`
  (name, worker, end, sim s, trace s, golden s, total s, attempt, kind) after each gate;
  the end of the log prints the phase table and the slowest scripts. First-run totals
  of UI scripts update the learned duration table `~/.cache/rockbox-gate/durations.tsv`
  (`GATE_DURATIONS` overrides), over the committed seed `tests/ui/durations.tsv`
  (`scripts/gate_tools.py seed` regenerates it; a script missing from it is estimated from its waits).
- **Flake ledger.** Every first-attempt failure that a retry then settles is appended to
  `~/.cache/rockbox-gate/flakes.tsv` (`GATE_LEDGER`): date, gate id, commit, script, kind
  (`flaky`, `starved` when the failure line says so, `real`), retry result, first FAIL line.
  The log ends with this gate's rows and names scripts that flaked in 2 of the last 5 gates.
- **Builds abort the gate.** A failed build ends the run at once with `GATE FAILED`
  (before fonts, host tests and simulators). `GATE_KEEP_GOING=1` runs on as before.
- **`ran N of M`.** GATE PASSED is withheld unless every listed ui script produced a result.
- **Watchdog.** `GATE_MAX_MINUTES` defaults to 240 for the full gate, 75 for `--quick`.
- **Spotlight.** `build/` and `out/` get `.metadata_never_index`.

Opt-in, `GATE_POOL=1` (full gate; stays opt-in until the soak in docs/gate-speed-brief.md section 8 passes):

- One dynamic pool (`GATE_JOBS` slots, default 8 or the machine formula if lower) runs the
  ui scripts, the UBSan scripts (`scripts/risk/sim_ubsan.sh --job`) and the floor gate
  (`FLOOR_PARALLEL=1 tests/perf/floor_gate.sh`, both RAM halves at once), longest first
  by estimated duration. Each job gets its own clone of a primed disk.
- `sim-ui32`, the UBSan sim (`sim_ubsan.sh --build`) and the three compile-only builds
  join the concurrent build phase.
- Host tests (`TEST_JOBS=4 scripts/test.sh`, fatcrash first) run in the background
  alongside the trace self-tests, benches and priming; output is printed before the pool starts.
- A failing ui script is retried at once on a fresh clone inside its slot; one that fails
  twice runs once more alone after the pool before it counts as a real failure.
- `sim_ubsan.sh` also takes `--build`, `--prep`, `--job NAME`, `--report` (no argument: all, as before).
- Unit tests of this machinery (fake jobs, no simulator): `tests/gate/run.sh`.


## The bench session in the simulator (`ipodui-p19-bench-*`)

`build/sim-ui-video` compiles the device's bench runner (`apps/ipodui/devbench.c`:
the step table, the run order, the results file, resume, the auto-skips, the
end summary) with each step's device call replaced by a stub
(`apps/ipodui/devbench_sim.c`; `devbench_sim.h` says what is modelled and
what is real, `devbench_sim_calls.h` maps the calls; guard `DEVBENCH_SIM`).
Developer > Bench session is then in the sim's Developer menu. The model's
state is marker files under `/.rockbox/.sim_bench/`, so it survives the
scripts' `reboot` parts: the stage flags (there by default), the display
driver (the boot step's restart brings it; the first start has none, so pre
is done and boot runs), the UI on the COP (ucop's restart).

    bench_sim reset                 no results files, the model's defaults
    bench_sim set <switch> [text]   noflags | lowmem | putfail <n> | putfails <k> | fail_<step id>
    bench_sim clear <switch>
    bench_sim expect <checks...>    order ended verified|noverified token|notoken <id>=<result|autoskip>
    bench_sim dump                  the newest results file, to the log

`putfail <n>`: the n-th results-file append open from now fails, once (the
device's lost lines); `putfails <k>`: the next k opens all fail (put()'s
three tries each: k = 9 refuses three writes in a row, which wait in the RAM
queue). `lowmem`: the probe image's block (the real
`core_alloc` of its size) is refused while music plays. `fail_<id>`: that
row or stage reports FAIL. The step log lines go to `/.rockbox/bcmtest.log`
as the device's `devbcm_log()` writes them (the resume reads its `auto:`
lines). jvpu reads through `vplayer_peek()` (no memory), so `lowmem` now
only bites a `vplayer_probe()` caller. `expect order`: every re-verify pick has exactly
one result line (again only after a `retry` line), in
`bench_reverify_picks` order, and no other step; a failure logs
`expect_bench failed: ...`, which fails the run. The stubs log every
screen (`bench_sim: screen "<title>" -> go|menu`, `ask ... -> <n>`) and
every SELECT that missed its line, so a desynced script shows where.

| Script | Checks |
|---|---|
| `ipodui-p19-bench-reverify` | sitting 4d's re-verify run (the held picks, `BENCH_R_HELD`, never show), normal answers, both restarts, the outside steps (an album, Now Playing, a video, the wheel), resume by Go on; each step once, in order, the verified line, no clock150 token; vplay drops the unit's check result (`SIM_VCHECK`, a play before the session stored one) and its play runs the first-use check: `vck PASS` within 0.6-1.5 s, the verdict on vplay's result line |
| `ipodui-p19-bench-s6fail` | `fail_s6`: vplay auto-skips (one "Skipping ahead"), the second-core group still runs, a clean end with no verified or token line |
| `ipodui-p19-bench-faults` | `putfail 1` and `lowmem` set while uviz is outside: uviz's result line is lost and jvpu runs with music playing; `putfails 9` while uck1 is outside: three writes refused in a row, held in the RAM queue; still each step once, in order, jvpu passes, the verified line (sitting 4c's bugs a, b, c) |

The scripts are written by `scripts/bench_sim_script.py` (the screen order
is the runner's; a step or screen added to the run needs a line there).
About 4-5 minutes each.
