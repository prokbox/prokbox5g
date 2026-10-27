# Brief: automatic battery calibration (owner, 2026-10-12; tracker #70)

**Rev 1** (brief review 2026-10-12: H1–H3, M1–M6, L1–L3 folded in;
Rev 0's write-at-shutoff, ±200 mV bound and charge-done test were
unworkable). Owner request: "lets set up auto battery calibration".

**Rev 2 note** (implementation, 2026-10-12; branch `batcal`): where the
code disagreed with Rev 1, the safe option was taken:
1. **No `powermgmt.c` edit.** The `battery_levels.cfg` error splash
   already goes through the shell's splash funnel (`apps/gui/splash.c`,
   `HAVE_IPOD_UI`: logged as `splash-suppressed`, never drawn), so the
   guarded one-line edit and its ledger row were not needed.
2. **Write trigger.** `DISK_EVENT_SPINUP` fires on the storage thread
   just before `Q_STORAGE_SLEEPNOW` (`firmware/storage.c`), so a pending
   flag serviced later by the UI thread would always find the disk
   asleep. Instead the 1 s sampler posts one shell wake
   (`np_pb_ab_wake()`) when it sees the disk come up with something to
   write; `batcal_service()` writes while `storage_disk_is_active()`, at
   most once per 5 min unless a run opened, closed or ended. The event
   handler still writes when it runs on the UI thread (`system_flush()`:
   USB connect, clean shutdown). Still UI thread only, zero wakes,
   nothing below disk-safe.
3. **Plug-in at the very end.** A charger plug-in while the run's last
   sample is already within `BATCAL_END_MV` of disk-safe ends the run
   (fitted at once on the UI thread) instead of discarding it, the same
   rule the boot decision uses; an ended run cannot be replaced by the
   next full-charge unplug before its fit.
4. **100 % point.** The first 10 min are skipped, so the curve at Q = 0
   cannot be observed; point 10 is extended back from the first fit
   sample along the slope of the next 2 % of Q. Host test: points 0–9
   within ±15 mV of the true curve; point 10 between the curve at the
   first fit sample and at Q = 0, within 15 mV.
5. **Skip samples** carry the mV value 0xFFF (a sentinel; a merge with
   one stays a skip). The header keeps `last_mv`, the last period's mean
   voltage, for the end rule.
6. **`batcal.dat`** ends with a line `end <crc32 hex>`, so a truncated or
   corrupted file is ignored whole (the built-in table stays).
7. **Developer rows:** the status is two read-only rows ("Built-in
   table" / "Calibrated: N runs, last <date>", then "Run: X h logged" /
   "Run: waiting for a full charge"); both on one line don't fit the
   stock list's 320 px without a marquee. The reset confirms with a
   second SELECT within 5 s ("Select again within 5 s"): the stock yes/no
   linked about 1.5 KB. Reset restores the table in use before the
   learned one was applied (built-in, or `battery_levels.cfg`'s).
8. **A carried-on run** (clean shutdown, next boot) is rewritten "open"
   at boot while the disk is still up, so a crash early in that boot
   cannot leave a stale "clean" file.
9. **Code size** is over the < 3 KB estimate: +7,536 B `.text` on the
   ipodui firmware (core about 3.5 KB, glue about 3.4 KB, developer rows
   about 300 B), +12 B data, +1,256 B bss. Trims for the owner to choose:
   a binary `batcal.dat` (about −0.9 KB, loses the readable file), the
   cold code under `--thumb-cold` (#45, about −1.5 to −2 KB).
10. **End review fixes** (budget review, 2026-10-12): near the end of a
   run (last sample within `BATCAL_END_MV` of disk-safe) every disk spin
   writes, not one per 5 min (`bc_write_due()`), so the last write above
   disk-safe is not held back; `system_flush()`'s write always marks the
   run "clean" (some screens reach the shutdown without `input.c`); the
   table swap runs with IRQs masked; the reset restores `isdefault` too.
   Left for the owner: on the flash variant `system_flush()` runs below
   disk-safe at the shutoff, so the "ending" mark could be written there
   (relaxes "nothing below disk-safe"); Rev 1 behaviour kept.

## Why
The battery percentage is a fixed voltage-to-percent table
(`percent_to_volt_discharge[11]`, `powermgmt-ipod-pcf.c:62`, read
directly by `voltage_to_percent`, `powermgmt.c:374`). The PP5022/PCF50605
has no fuel gauge: voltage is all we can measure. The large-cell build
(`IPOD_BATTERY_MAH > 600`) borrows the stock 400 mAh table, marked
uncalibrated. The curve depends on chemistry, load sag and maker, not
size. The firmware can learn it from each full discharge, so every unit,
stock or modded, calibrates itself with no lab step (keeps #43 open).

## What the owner sees
Nothing new by default (the 7G has no such setting). The percentage, the
battery icon and the low-battery warnings get more accurate after the
first full discharge. Developer mode gets a read-only status row and a
reset row.

## Design

### 1. Recording a run
- **Own sampler** (the powerlog is devmode-only and idle while off, so
  it can't be shared): a `timeout_register` callback every 1 s that only
  writes RAM: the busy share of the second (backlight lit, boosted, disk
  active, charger present). Every sample period (start 60 s) it stores
  one sample.
- **Sample = 2 B:** 12 bits of mV offset from 3000 mV (1 mV steps,
  3000–7095) plus 4 bits of busy share (0–15 sixteenths). The time index
  is implicit (periodic sampling). Samples while the disk was active in
  the period count as busy (the voltage filter pauses during disk I/O,
  `powermgmt.c:742`). **Ring: 512 samples = 1 KB static** (the process
  rule's limit). When it fills, pairs are merged (mV averaged, busy
  averaged) and the period doubles, so any run length fits.
- **Start ("full" latch):** a run opens at charger unplug only if,
  before the unplug, the charger was present, USB charging was enabled,
  and `charging_state()` had read false for at least 2 continuous
  minutes with voltage at least `BATCAL_FULL_MV` (fixed constant,
  4100 mV, not table-relative). Anything else opens nothing.
- **Skip:** the first 10 min of a run (surface charge) and the first
  5 min after each boot (rest recovery after off-time) are recorded but
  flagged and excluded from the fit.
- **Off-time** adds nothing (charge is used only while on;
  self-discharge while off is ignored, documented).
- **Partial:** a charger plug-in while a run is open, or USB power
  present at any time, closes the run as partial: discarded (v1 learns
  only from full runs).

### 2. Persistence (UI thread only, crash-safe)
- File `/.rockbox/batcal.run`: header (magic, version, sequence number,
  period, sample count, flags: open/ending/clean, the boot count), the
  samples, then a CRC-32 over both. Written as a temp file plus rename,
  so a power cut leaves the old or the new file, never a mix.
- **When:** only on the UI thread, while the disk is already spinning
  (a pending flag set from the spin-up event, serviced from the shell's
  main loop as `powerlog_service()` does), at USB connect and at clean
  shutdown (`system_flush()`), and **never below the disk-safe level**
  (`battery_level_safe()` false: no write at all).
- **Clean vs crash:** each boot sets the "open" flag in RAM; a clean
  shutdown writes "clean". A boot that finds a run file without "clean"
  and with a minutes gap (the RAM since the last write was lost: hard
  reset, crash) discards the run, **unless** the last stored sample is
  within `BATCAL_END_MV` (50 mV) of the disk-safe level: that is the
  normal end of a run (below disk-safe nothing writes, so the last write
  is near it, and the power-off at shutoff is not a clean shutdown,
  `misc.c:518,584-605`).
- **End mark:** in the UI thread's `SYS_POWEROFF` handling
  (`apps/ipodui/input.c:155`, beside `ui_log_flush`), when
  `query_force_shutdown()` is true, set "ending" in RAM (written only if
  the level still allows it). No upstream edit in `powermgmt.c`.
- **Complete run** (decided at the next boot, never at shutdown): the
  file is "ending", or "open" with its last sample within `BATCAL_END_MV`
  of disk-safe; at least 2 h of on-time and 120 fit samples; busy
  minutes at most 30% of the run. The fit runs then, on the UI thread,
  and the run file is deleted after the result is stored.

### 3. Fit (pure, host-tested)
- **Charge axis:** each sample's charge weight = period × (1 +
  (`BATCAL_BUSY_W` − 1) × busy share). `BATCAL_BUSY_W` starts at 1.5, an
  estimate documented in `docs/hardware.md` (to be replaced by measured
  class currents once a USB-C meter or a rundown gives them). Cumulative
  charge Q(t) normalised to 0..1 over the run (to the last sample,
  where the cell sat near disk-safe).
- **Curve:** a moving median of the fit samples' voltage against Q
  (window 4% of Q, narrowing to 1.5% for Q > 0.85, near the knee),
  linearly interpolated.
- **Reserve:** the run ends near disk-safe, so `Q = 1` is "disk-safe
  reached", which already leaves the reserve the shell needs (3500 mV,
  `powermgmt-ipod-pcf.c:30`; the shutoff 3300 is below it). The learned
  0% point is the curve at Q = 1 − `BATCAL_RESERVE` (3%), never below
  disk-safe + 20 mV. So 0% still means "about to shut down", and the 5%
  and 15% warnings (`powermgmt.c:316`), the low-battery path
  (`misc.c:552`) and the `percent_now < 8` disk sampling gate (`:752`)
  keep their meaning.
- **Table point k** (k = 0..10, k×10%): the curve at
  Q = (1 − k/10) × (1 − `BATCAL_RESERVE`).
- **Sanity** (fixed lithium-ion limits, not the uncalibrated table):
  strictly decreasing k = 10 → 0; point 10 within 3950–4250 mV; point 0
  within 3400–3750 mV; every step 5–250 mV. A failed fit is logged to
  ipodui.log, never shown.
- **Blend:** `/.rockbox/batcal.dat` holds the last 3 accepted fits
  (text: a version line, then one line per fit: sequence number, date
  or "-" if the clock wasn't set, on-time hours, 11 voltages), written
  as a temp file plus rename. The table in use is the per-point median
  of the stored fits, ordered by sequence number (not date).

### 4. Applying it
- At boot, after settings load, a valid `batcal.dat` overwrites
  `percent_to_volt_discharge` (11 shorts, a correct `memcpy` of
  `sizeof`, not the upstream `PWRELEMS`-byte copy). **Precedence:** if
  `battery_levels.cfg` exists, `init_battery_tables()` (`main.c:834`)
  has loaded it first; the learned table then wins and the cfg file is
  ignored (logged). In `HAVE_IPOD_UI` builds the cfg parser's error
  splash (`powermgmt.c:1012`) becomes a log line (guarded, one-line
  upstream edit, ledger row).
- **Charge table** (L2): point 10 of `percent_to_volt_charge` stays as
  built. At unplug the display can step a few percent; documented, not
  blended in v1.
- `battery_level_dangerous` / `battery_level_shutoff` and the capacity
  setting stay as built. v1 doesn't estimate mAh.

### 5. Developer rows (devmode.c)
- **Battery calibration**: one status line: "Built-in table" or
  "Calibrated: N runs, last <date>" (no date if the clock wasn't set),
  plus "Run: X h logged" or "Run: waiting for a full charge". Hard-coded
  English like the other developer rows (devmode.c:249,264): a
  deliberate, documented exception to rule 8, developer mode only. No
  file names on screen ("batcal", "battery_levels.cfg", the hidden
  folder).
- **Reset battery calibration**: confirm, then delete both files and
  restore the built-in table.

### 6. Code layout and flags
- `HAVE_BATTERY_LEARN`, on in `--ipodui` iPod Video builds (flag row in
  `docs/hardware.md`). A stock build compiles unchanged.
- `apps/ipodui/batcal_core.{c,h}`: pure logic (sample packing,
  decimation, the state machine as a function of events, the fit,
  sanity, the blend, file parse and format with CRC). Host-buildable.
- `apps/ipodui/batcal.c`: glue (the timeout, charger and USB events, the
  spin-up pending flag, the `SYS_POWEROFF` mark, file I/O with temp plus
  rename, applying the table).

### 7. Ownership

| File | Owner |
|---|---|
| `apps/ipodui/batcal*.{c,h}`, `tests/batcal/` | new, implementer |
| `apps/ipodui/input.c` (one call), `apps/ipodui/devmode.c` (two rows) | implementer (no other open branch edits them) |
| `firmware/powermgmt.c` (splash to log, guarded) | implementer, ledger row |
| `apps/SOURCES`, `tools/configure` flag | implementer |
| `docs/hardware.md` (flag row, BUSY_W note, fix the "low-battery shutoff included" claim at :189), ledgers | implementer |

### 8. Device reality
- Below disk-safe nothing is written: the run's end is inferred at the
  next boot (§2).
- A brown-out under load before the shutoff is an unclean end: handled
  by the last-sample rule.
- Hard reset or crash mid-run loses minutes: the run is discarded, not
  fitted short.
- Flash disk: a write costs about 1–2 KB, only while the disk spins.
- The clock may be unset: sequence numbers order everything.

## Tests and acceptance
- **Host (`tests/batcal/`):**
  - the fit on synthetic discharges (a known curve plus noise, busy
    stretches, filter lag at the knee): every point within ±15 mV;
  - the sanity rejections and the reserve policy;
  - decimation with implicit indices (a 60 h run);
  - the blend and its ordering by sequence;
  - a state-machine table: full latch (and its misses: USB charging off,
    a 100 mA host, unplug at 4.15 V while charging), USB without
    charging, plug-in mid-run, reboot, hard reset, unset clock, normal
    end, brown-out end;
  - file round trip, plus a file truncated at every byte offset and
    corrupted (ignored, the old file or the built-in table kept);
  - writes refused below disk-safe.
- **Replay:** a small committed synthetic power.csv-style trace through
  the recorder: runs open and close where expected (the owner's real
  log is used locally only, not committed).
- **UI script** for the two developer rows with goldens.
- **Full gate**, then the budget-reviewer.
- **Device procedure** (for the owner, later): one full run-down with
  the power log on; the learned table against the power.csv curve, and
  the percentage at known hours afterwards.

**Accept when:** the gate passes; the host tests above pass; the budget
review passes (≤ 1 KB per static, code < 3 KB); nothing new is visible
outside developer mode.

## Budget
- RAM: 1 KB ring + about 64 B of state.
- CPU: one 1 s timeout (a few comparisons), one 2 B store per period;
  the fit once per complete run at boot.
- Disk: about 1 KB written only while the disk already spins; zero disk
  wakes; nothing below disk-safe.
- Code: under 3 KB.

## Open questions (defaults frozen, owner can override)
- **O1:** only full runs teach (default).
- **O2:** developer-mode rows only (default).
- **O3:** `BATCAL_BUSY_W` 1.5 until measured.
