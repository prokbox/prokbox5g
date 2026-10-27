# P12 close notes (2026-10-24)

For the orchestrator to carry into `docs/upstream-backports.md` (on `main`, not on this branch). Candidates are bounded; none is a plain bug fix except where noted.

## New P12 upstream candidates
1. `apps/recorder/jpeg_load.c` / `.h`: `jpeg_set_abort_hook()` (asked before each MCU row; true makes `store_row_jpeg()` return NULL and `clip_jpeg_fd()` return 0). Generic cancel point for a long decode; also fixes the unscaled path returning on a NULL row. Hook-shaped, not a bug fix. Pairs with the already-listed `jpeg_set_pause_hook()`.
2. `firmware/common/timefuncs.c` / `timefuncs.h`: `get_time_stale()`. Upstream's `get_time()` keeps its RTC copy up to ~1 s by the *tick*; after a sleep where the tick stood still the copy is as old as the sleep. A target that stops the tick (soft sleep) needs it. Only the `!HAVE_RTC_IRQ` body changes. Hook-shaped, small.
3. `firmware/powermgmt.c`: the Sleep Timer's end can post a registered sleep request instead of `sys_poweroff()` (`powermgmt_set_sleep_handler()` moved above its first use). Upstreamable as "let a target intercept the timer's end"; low value for stock Rockbox.
4. `firmware/powermgmt.c`: `power_thread_rtc_process()` calling `rtc_enable_alarm(false)` on the alarm flag disarms an alarm the moment it fires and races a re-arm (a snooze or the next repeat armed before a power-off is lost). Upstream behaviour for the PCF50605 targets; a real upstream bug on any target that re-arms from the alarm handler. Our fix is shell-guarded; the unguarded form is a one-line removal plus the INT1 read. Candidate bug fix.
5. `rtc_pcf50605.c`: `rtc_set_alarm_full()` / `rtc_get_alarm_full()` with read-back, and the RTCWAK rule (`rtc_alarm_keep.h`, host-tested in `tests/alarm`): a valid alarm still ahead keeps RTCWAK; a just-passed exact date becomes the daily match. Candidate for upstream's iPod 4G/Video wake-from-off alarm.
6. `tools/modpak.py` is ours (no upstream counterpart); nothing to send.

## Open ledger gaps
- None unresolved. The strip-tail byte count for the Clocks reveal is not in any commit message, so `docs/hardware.md` records "0 B added, spare tail" without a number; the owner of the reveal code (W2-W) can add the exact figure.
- `docs/design/sdk.md` still says `J_BUDGET_APP` 56 KB (lines ~350, ~528) while the overnight default is 51.5 KB; reconcile when the owner confirms.
