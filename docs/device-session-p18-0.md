# Device session P18-0: runbook, revision 2

For the owner's modded iPod Video 5.5G. Three parts, in this order:

1. **Now, on the current install, with no new build.** Do not interrupt it.
2. **The attended session (about 1 hour)** on a new session build. It centres on **D2, the Sleep lab**.
3. **The unattended runs:** #114 H3 run B, and the D3 24 h sleep drops once soft sleep passes D2.

Rules that hold throughout (CLAUDE.md rule 5):
- **Each install needs your go and a fresh backup.** The agent only backs up, installs and pulls files when you say so.
- **fsck is only `fsck_msdos -n`, and you run it.**
- **Nothing is formatted.**
- Apple's firmware stays as the recovery path (hold **MENU** at boot). It is tested once, at the end of part 2.
- **No USB-C meter** (owner, 2026-10-22; #111 deferred). Sleep current comes from repeated 24 h voltage drops through the learned battery table.

What changed since revision 1: 18-1 is merged, so soft sleep exists and D1 to D4 can run. The old build `2ef1befcb9` is gone. #114's drive power-off, the boot battery guard, the crash fixes (a panic now restarts the unit, a close on a damaged directory fails softly) and the measured battery table are all in.

## Builds

| | Build | Where it comes from |
|---|---|---|
| Installed now | `c08ccbebacM` | `p18-114` at `c08ccbebac` plus local changes. H3 run A is running on it |
| Session build | *(the agent fills in the id and SHA-256 when it exists)* | `p18-114` after its gate. Configure with the BCM device-test set: `--ipodui --bcmfw-devtest --bcmfw --bcmfw-video --bcmfw-l1` (the "dev47 set"). It carries the Sleep lab, **Soft sleep** (default Off), the stage 5i/5s/5b entries and everything from `c08ccbebac` on |

The session build is **one install**. The Sleep lab and the video-chip entries are in the same image. The bootloader and Apple's firmware are never touched.

## What to prepare
- A plain **wall charger** (not a computer) and a USB-C cable.
- The Mac with a USB-C cable, for the USB wake. Terminal for `sudo fsck_msdos -n`.
- A **phone** for photos. For D4 (optional), one 240 fps video.
- **A dark room** for the deep-sleep row (it checks that no flash or image shows).
- A watch or the phone clock: several rows need "wait 30 s", "wait 2 minutes" or "wait 11 minutes".
- **Battery above 50 %** for the attended session (the Sleep lab sleeps for up to 30 minutes per row). Full is better.
- A 2,000+ track playlist or folder on the unit for the queue check (P16 acceptance).

---

# Part 1. On the current install (`c08ccbebacM`), no new build

## 1.1 Right now: H3 run A is running. Leave it alone.

Run A is the #114 rundown with **Drive power: off when idle**, FLAC on repeat, **Hold on**, **Power log: on, every 30 s**, playing until it shuts off (about a day).

Do not:
- install anything;
- plug in USB (the `stor pwr ... reason=usb` line and the power log would be spoiled, and it ends the rundown);
- touch the wheel or buttons. Leave Hold on.

Nothing else is needed from you until it dies. Make a note of the clock time it was started.

## 1.2 When run A ends (the unit shuts off by itself)

This is also the check of the boot battery guard and of the learned battery table. Do it in this order:
1. **Leave it off for 30 minutes**, unplugged. It rests.
2. **Press a button on battery.** Expected: the brief boot glyph, then the low-battery image for about 2 s, then off. No menu. (A drained cell is refused.) Note what you see; photograph the image if you can. Do this a second time.
3. **Plug it into the wall charger.** It boots to charging. This clears the drained latch.
4. When it has charged a few minutes, the agent (with your go) copies the logs off over USB. The agent reads, in `ipodui.log`:
   - `bootbatt drained latch set (zero) mv=` (written when the display hit 0 %);
   - `bootbatt drained latch cleared` (at the charger boot);
   - `batcal fit seq=` at the first boot that passed the guard (and no `run dropped`);
   - the `stor pwr on ms=` and `stor pwr off ms=` lines, the `stor pwr hour span_s= cycles= wasted= on_pct=` lines, any `ata` error or soft reset near a power-on, any `pcm underrun`;
   - `power.csv`: the `stor_on_pct`, `stor_ons` and `stor_ons_total` columns, and the hours to shut-off.
5. **Keep the unit charging** to full. The logs of run A are pulled and then cleared before part 2 (the agent does this with your go).

Run A's result goes to the agent as: hours to shut-off, mAh per hour through the learned table, `stor_on_pct`, and the volume you used.

## 1.3 Optional on this install: the crash fixes (5 minutes, after 1.2)

Developer list, last two rows. This install has the panic-to-reboot change.
1. **Crash test: restart.** It restarts by itself. At the next boot `ipodui.log` holds `panic ... msg dev test restart`.
2. **Crash test: freeze.** The unit should now restart by itself within a few seconds. If the screen stays frozen for more than 10 s, hold **MENU + SELECT** for 6 to 8 s and tell the agent. `ipodui.log` should hold `panic kind=0 count=1 thread=<UI thread>` and `panic msg dev test freeze`.

Do not run the freeze test twice in a row. The restart has a loop guard, and a second panic right after a panic is meant to stop.

---

# Part 2. The attended session (about 1 hour) on the session build

Order of work: install (agent, with your go) -> **D2 Sleep lab** (the centre, about 40 min) -> video-chip stages (about 10 min) -> P16 acceptance checks -> recovery test. Items marked **(turns soft sleep on)** are the pass criteria in section 2.9.

## 2.0 Pre-flight and install (agent, with your go; about 15 min, not counted in the hour)

1. Plug the iPod in over USB. The agent finds the data volume (`diskutil list external physical`) and stops if there is more than one candidate.
2. **Back up** the hidden system folder to the backup folder with rsync, as the `deploy-ipod` skill says. The agent checks the copy and tells you its path. Nothing is installed until you say so.
3. **You:** run `sudo fsck_msdos -n /dev/rdiskNs2` (the agent gives you the exact disk). `-n` only reads. Paste or photograph the result. This is the orphaned-cluster count "before".
4. **Before the first boot of the new build,** copy a few new tracks onto the iPod. That forces one library update, so the #47 name re-sort is exercised on this install.
5. The agent pulls and then clears `power.csv`, `batcal.run`, `ipodui.log` and any `bcmtest*.log` (this gives clean logs for the session). It also keeps `batcal.dat`.
6. With your go, the agent unpacks the session zip onto the volume (config, database and bookmarks are kept), checks the version string, and ejects.
7. Unplug. If the unit does not restart by itself, hold **MENU + SELECT** for 6 to 8 s. Watch the first boot: the busy glyph while the library updates (P16 check, 2.7).

## 2.1 Settings for the session

All under **Settings > About, hold SELECT + Previous (|<<) for 3 s**, which opens the Developer list.
- **Power log: on**, **Power log every: 5 s**.
- **Soft sleep: Off** (leave it; the Sleep lab calls the sleep directly and does not need it). It is turned on in 2.6, not before.
- **Drive power: off when idle** (the default). Leave it.

Before every Sleep lab row:
- Music **stopped**. (A playing track makes the lab refuse: see 2.5.)
- Charger and USB **unplugged**, unless the row says otherwise.
- **Hold off**, unless the row says otherwise.
- Take your hand off the wheel when you press the row's SELECT. The lab waits for every button to read released.

## 2.2 How a Sleep lab row works (read once)

**Developer > Sleep lab** has a result line on top and seven test rows below it. Each test row sleeps the unit once, on a fixed request, and logs the result. The screen goes dark at once.
- **Start when quiet:** wait about a minute after USB, a sync or a library change, and after pausing, before you press the row. A post-USB art pass or a database update keeps the CPU boosted and the drive busy; the lab then waits and gives up (below).
- You press **SELECT** on the row. The screen and backlight go dark. The lab waits **up to 30 s** for everything a sleep needs to be quiet: no CPU boost, no PLL hold (the lit screen's included), PCM stopped, the drive idle, the art builder and maintenance held. It logs what is still busy while it waits. Still busy after 30 s, it **does not sleep** and comes back lit.
- **Wait 30 s after the screen goes dark before you do the wake action**, so that the entry itself is not mixed up with the wake. A button pressed *during* the entry's wait cancels the run ("key pressed"); that press is swallowed and the next one works normally.
- The row ends at the **first wake**. A wake returns you to the menu with the screen lit (**with Hold on too**). **The press that woke it must be swallowed:** the list must not move and nothing must open.
- **Read the result line on top after every run.** It says whether the run counted: `Slept 612 s, woke: button` (it counted), `Didn't sleep: busy (boost/disk/art)` (the quiet never came: run it again once things settle), `Didn't sleep: key pressed` or `Didn't sleep: USB/charger` (cancelled in the wait), or `Refused: PCM` (the platform refused at the last check; the name says why). Only a "Slept" line is a result for the row.
- Every row needs its own run. If a row says "wake by X", do it once per run, then read the screen.
- A row has a time cap (the poll test 10 min, the wake test 30 min). If you do nothing, it wakes by itself at the cap, which is also a result.

**Log lines the agent reads after every row** (`ipodui.log`; each line fits the log's 61 characters):
```
sleep lab test=N start
sleep lab t=S boost= holds= disk= pcm= art= maint=     (while waiting)
sleep lab busy boost= holds= disk= pcm= art= maint=    (gave up: no sleep)
sleep lab abandoned why=key|event t=S                  (cancelled)
sleep lab test=N boost= holds= mhz= disk=
sleep lab dac= keys= waited=S
sleep wake why=N refused=N NAME s=
sleep wake polls= spurious= resyncs= buttons=0x..
sleep wake hold_edges= mv= crc=
sleep poll i= asked= rtc= us=
sleep stack hw= before=
sleep lab end lit= hold=
```
`why` values: 1 button, 2 USB or charger, 3 alarm, 4 deep timer (or a refusal), 5 low battery, 6 Hold slid off. `polls=0` means woken by interrupt rather than by a poll timer. `refused=0 NONE` for a real sleep; otherwise the name (BOOST, CLOCK, PCM, KEYS, COP, TIMER2, RTC). `crc=2` always in this build (see D1, 2.8).

## 2.3 D2 first: the poll timer (must pass before anything else counts)

Brief: 18-1 does not merge on trust, it rests on this. **Hold off, USB cable at hand.**

| Step | Row (exact label) | What to do | Look for |
|---|---|---|---|
| T1 | **Poll timer test (10 min)** | SELECT. Leave the iPod alone for 10 minutes. Keep the phone clock. | It wakes by itself near 10 min with the screen lit. Photograph the clock when it wakes. If it never wakes, hold **MENU + SELECT** for 6 to 8 s and note how long it was |

Log: `sleep poll i= asked=30 rtc=` lines (about 20 of them, one per poll). **Pass:** each `rtc` is 30 (plus or minus 1) and the total `s=` is close to 600; `us` is close to 30,000,000 per poll if the microsecond timer counts asleep (either answer is recorded, neither fails). A timer that wakes late or not at all fails D2 and stops the session here: tell the agent.

Later, as the last lab row: **Countdown poll test (2 min)**. It tries the other timer (the seconds-unit countdown). Same log lines with `asked=5`. Record the result either way.

## 2.4 D2: every wake source (the centre of the session)

For each row below: open **Developer > Sleep lab**, select the row, press SELECT, hands off, wait 30 s, do the action, **read the screen** and write down what you saw.

Use the row **Wake test** unless a row says otherwise. Each source is its own run.

### Buttons, one per run (5 runs)

| # | Action 30 s after entry | Pass criteria |
|---|---|---|
| W1 | Press and release **PLAY** | Wakes within 0.5 s. `why=1`, `polls=0`, `buttons=` shows the PLAY bit. The menu does not move (swallowed) |
| W2 | Press and release **SELECT** | Same, SELECT bit. **The row you were on does not reopen** |
| W3 | Press and release **MENU** | Same, MENU bit. The menu does not go back a level |
| W4 | Press and release **Next (>>|)** | Same, Right bit. The highlight does not move |
| W5 | Press and release **Previous (|<<)** | Same, Left bit. The highlight does not move |

For each: note whether the wake felt instant, and **photograph the first lit screen** (it should be the same menu, same highlight, as before the row).

### The press and the release, separately (the swallow and the release rules)

| # | Run | Action | Pass criteria |
|---|---|---|---|
| S1 | **Wake test** | Wake with SELECT and **hold it for 3 s**, then release | Nothing happens on the screen while held or on release. No list opens, no value changes |
| S2 | **Wake test** | Wake with PLAY, **hold it 1 to 3 s**, release | Same. PLAY does not start or stop anything (the lab has nothing playing) |
| S3 | **Wake test** | After 30 s: **touch the wheel** (no turn, no button) for 5 s, then **turn it** for 5 s, then take your finger off and wait 10 s | **No wake** for touches or turns. Then press **SELECT** to end the run. The log shows `spurious=` counting the touches and `why=1` for the SELECT only. A wheel touch that wakes the unit is a fail |

### Charger and USB

| # | Run | Action | Pass criteria |
|---|---|---|---|
| C1 | **Wake test** | After 30 s plug the **wall charger** in | Wakes: `why=2`, within 1 s. Screen lit. Nothing else happens in the lab; unplug it again |
| C2 | **Wake test** | After 30 s plug **USB into the Mac** | Wakes: `why=2`. The USB screen may appear. Eject from the Mac, unplug. Nothing is written to the disk while the screen is up (no disk writes at entry) |
| C3 | **Wake test** | With Hold **on** (slide it on during the first 5 s after SELECT), after 30 s plug **USB** | Wakes to the USB screen (brief N8) even with Hold on. `why=2` |

### The Hold switch and the 10 minute window (Q12)

`SLEEP_HOLD_WAKE_S` is 600 s: sliding Hold off wakes the unit **only within 10 minutes of sleep entry**.

How to enter with Hold on: press SELECT on the row (Hold off, so the button works), then **slide Hold on within the first 5 s**, while the screen goes dark.

| # | Run | Action | Pass criteria |
|---|---|---|---|
| H1 | **Wake test** | Hold on during the first 5 s. At **2 minutes**, slide Hold **off** | Wakes: `why=6`, `hold_edges=1` or more, `polls=0`, within 0.5 s. Screen lit |
| H2 | **Wake test** | Hold on during the first 5 s. At **5 minutes**, slide Hold off | Same. Note the exact time |
| H3 | **Wake test** | Hold on during the first 5 s. At **about 9 minutes**, slide Hold off | Wakes (still in the window). If it does not, note the time |
| H4 | **Wake test** | Hold on during the first 5 s. At **about 11 minutes**, slide Hold off. Wait 20 s: the screen must stay dark | **No wake** (outside the window). `hold_edges` counts the edge, `why` is not 6. Then press **SELECT** to end the run. This is the 7G behaviour the owner described (Q0, Q12) |
| H5 | **Wake test** | Hold on during the first 5 s. At 30 s **press every button, one at a time, and turn the wheel**. Wait 30 s. Then slide Hold off | No wake from any button while Hold is on. `spurious=` counts them (if the controller is kept powered) or stays 0 (if it is off). The Hold-off at the end wakes as in H1 |
| H6 | **Wake test, wheel kept on** | Once, with a button press (SELECT) 30 s after entry; and once more with Hold on, Hold off at 2 minutes | Only needed to compare. It keeps the wheel controller powered; the agent uses it if H1 to H5 fail. Note whether a press wakes as fast as W2 |

### The poll timer during a press, and the alarm

| # | Run | Action | Pass criteria |
|---|---|---|---|
| P1 | **Wake test, fast polls** | The row polls every 2 s (cap 10 min). Press **SELECT** at a different moment each time. Do **five runs**, at 5 s, 7 s, 13 s, 19 s and 31 s after entry | Every press wakes within 0.5 s. Note any press that felt late or was missed. `polls=` is the number of poll wakes before the press |
| A1 | **Alarm test (2 min)** | Hands off for 2 minutes | Wakes by itself near 2:00: `why=3`, `s=120` (plus or minus 2) |
| Z1 | *(do this one last, see 2.4.1)* **MENU + SELECT while asleep** | In **Wake test**, after 30 s hold **MENU + SELECT** for 6 to 8 s | Record what happens: a reset to ProkBox, a wake, or nothing (quirk 48: the mechanism is unknown). Neither answer is a fail |

### 2.4.1 Deep sleep, in the dark

| # | Row | What to do | Pass criteria |
|---|---|---|---|
| Y1 | **Deep sleep test (powers off)** | **Dark room.** Hands off for 2 minutes | After about 2 minutes the unit **powers off with no visible flash, image or glow** (the video chip is forced awake in the dark). Note anything you saw. Then press **PLAY** to boot: it must boot normally and `ipodui.log` has `sleep deep s=` |

This row ends in a power-off by design; it is the last row because of that.

## 2.5 D2: refusals never become a power-off

A refusal means "do not sleep now". It must leave the unit **awake and dark**, never switch it off. The Sleep lab shows this directly.

| # | Setup | What to do | Pass criteria |
|---|---|---|---|
| R1 | Start a track playing (anything), **do not pause** | Developer > Sleep lab > **Wake test** | Refused: `sleep lab test=1 refused why=playing` in the log; no sleep; the unit does not switch off and playback is not interrupted |
| R2 | Wall charger **plugged in** | **Wake test** | Either an immediate wake (`why=2`, `s=0`) or `refused=N` with N non-zero. Either way the unit **stays on and returns to the menu**. No power-off, no reset |
| R3 | USB cable to the Mac **plugged in** | **Wake test** | Same as R2 |
| R4 | Wall charger plugged in | **Deep sleep test (powers off)** | A refusal must not run the deep power-off: the unit stays on |

If any refusal ends in a power-off or a reset, that is a **fail**: it is the most important rule of this phase (a refusal is "awake, dark", never "off"). Record exactly which row did it.

## 2.6 The real sleep (soft sleep on)

Only once 2.3 to 2.5 pass. Developer > **Soft sleep: Off** toggles to **Soft sleep: On** when you press SELECT on it.

1. Music stopped, Hold off. **Hold PLAY** until the screen goes dark, then **release it slowly** (1 to 3 s after the screen goes dark). The unit stays asleep (the release must not wake it). Wait a minute. Press **SELECT** on a list: the unit wakes, the list does **not** move or open (swallowed). The first access after the wake logs `stor pwr on` (drive power-off, H4 of #114: `sleep drive off rc=0`).
2. **Hold on first, then sleep:** with Hold on, leave the unit idle until its idle timer sleeps it (or press PLAY, hold it, then slide Hold on within the first seconds). Sleep for about 2 minutes, then slide Hold **off** within the window: it wakes by the Hold switch.
3. Playing: start a track, press and hold PLAY. It pauses first, then sleeps. Wake with PLAY: the track resumes only if a normal PLAY press would resume it (the press that wakes must not also act).
4. Charger plugged in: hold PLAY. `sleep ignored why=` in the log, the unit stays on. Same with USB.
5. Leave Soft sleep **On** from here on only if you intend to run part 3 right away; otherwise set it back to **Off** (the default) before you leave.

Log lines: `sleep enter why= batt=`, `sleep quiet why=`, `sleep drive off rc=`, `sleep wake why= s= polls= spurious= crc=`, `sleep lit ms=` (a key in the entry window), `sleep refused why= try=` and `sleep refused stay=awake` (stay awake and dark), `sleep ignored`, `sleep deep s= why=`.

## 2.7 P16 acceptance checks (same install, about 15 min, woven in)

1. **Boot digest:** the first reboot logs the boot digest ms; reboot a second time with no change: the digest should be skipped (a line saying so, near-zero ms).
2. **Flush support:** read the `usb ata w85/w86 flush n/fail/nosup` line after a USB connect and eject (C2 above does this). n, fail and nosup: fail and nosup must be 0.
3. **Post-USB busy glyph (#84):** copy a few albums over USB, eject and unplug. The busy glyph appears while the library updates and then goes away by itself. Photograph it.
4. **Battery colours, bolt and plug:** photograph the battery icon at full, about half and low as you reach them, the **bolt** while charging and the **plug** when charged on the charger. (The log's `pct` helps.)
5. **Queue by index:** (a) pick the 2,000+ list, play all: it starts at once; note how long. (b) Reboot while playing: the same queue and position resume. (c) Start a library update (copy a file over USB and eject): check the queue still resumes.
6. **Orphaned clusters:** the `fsck_msdos -n` count from 2.0 is the "before"; run it again at the end (2.10).

## 2.8 D1 retention, D4 wake latency and D5 lock time

**D1 SDRAM retention.** Honest limit: the Sleep lab has **no guard-region fill**. The `crc=` field reads 2 ("unchecked") on every row, so there is no explicit check. What the lab does give is an **implicit check**: the whole firmware lives in SDRAM, so if the memory did not hold through a sleep, the unit would crash or reboot at the wake, and the resident video image (checked at wake) would be wrong. So:
- Every row that wakes the unit back into a working menu is a pass of retention at that length. Rows reach 2 min, 5 min, 10 min and (Wake test) up to 30 min.
- For 1 h and 8 h, leave the unit asleep from the real soft sleep (2.6 with Soft sleep On): 1 hour, then wake; and overnight, then wake. The unit must wake into a working UI with the library intact and no reboot (look at the log: no `boot` line, a `sleep wake` line, and the next `sleep lit` or normal menu).
- The agent asks the owner before adding an explicit guard-fill CRC row: it is a code change, not part of this session.

**D4 wake latency (optional).** Film the screen at 240 fps with the phone while you press SELECT to wake from a **Wake test** run (hold the phone steady, finger visible). Do 3 takes. The agent reads the frame count from the press to the first lit frame: target **0.5 s or less**; today's backlight wake is 0.37 s. The log's `sleep lit ms=` is the software part only. The agent reads the video; you do not need to measure.

**D5 PLL lock time.** Nothing to do. After the session the agent reads the `clk lock` lines in `ipodui.log`. Note any boot where they are absent.

## 2.9 The video-chip part (about 10 minutes; stages 5i, 5s, 5b)

Getting there: Developer > **Debug**; the BCM entries are at the top. Each entry first shows a confirm screen; scroll to its last line ("Run stage ... now (SELECT here)") and press **SELECT** to start; **MENU** cancels. Music stopped, Hold off, hands off during each run (a press lights the screen and spoils a look). When a run ends the summary appears: **photograph it**, then press MENU. Any hang: hold **MENU + SELECT** for 6 to 8 s (nothing the tests do persists).

| # | Entry | Time | Photograph or note |
|---|---|---|---|
| V0 | *(only if V1 says "Not available")* **BCM test: stages 0-5 (all but 4b)** | 5 min | Must end with stage 5 PASS (it writes the flag the next steps need) |
| V1 | **BCM test: stage 5i (idle sleep, optional)** | 3 min | The summary, including **"Framebuffer words changed: N"** (0 means the chip's memory held while its core slept). Say **5i PASS or FAIL**; it decides V3 |
| V2 | **BCM test: stage 5s (sleep spikes, optional)** | 3 min | The confirm screen: does it say **"Chip sleep: on (5i passed)"** or **"off"**? Then four looks (below). Photograph the summary: Wake light/deep, Reset/cold, Refused, Framebuffer changed, Upload CPU/E2, Read back wrong |
| V3 | *(only if V1 passed)* stage 5s again | 3 min | Same; this time the chip sleeps, so the power-log phases are for a sleeping core |
| V4 | *(optional, about 14 min, hands off)* **BCM test: stage 5b (drift A/B, optional)** | 14 min | Needs the stage 5 pass flag. Runs the main-menu cover drift twice: A = the PP pushes pixels (today), B = the video chip pans and fades the cover. Note any visible difference between A and B. Power log on, music stopped, Hold off |

**Stage 5s, in order (hands off):** look 1 (colour bars, 15 s, the reference) -> dark 20 s (panel on, backlight off) -> dark 20 s (light standby) -> **look 2** (bars after the light wake) -> two dark blinks -> dark 20 s (deep standby) -> **look 3** -> two dark blinks -> dark 20 s (panel off, chip powered) -> **look 4** (bars after a full panel power-on) -> dark 20 s (chip power cut) -> four brief bar flashes (the boot uploads: CPU, engine 2, CPU, engine 2) -> the normal screen, then the summary.

At looks 2, 3 and 4, compare with look 1: same colours and brightness, nothing shifted, no stripes or flicker, not washed out or inverted. During each wake, note any white flash, garbage or slow fade-in. If a look never comes (screen dark more than 5 s past its phase), note which; the stage recovers by itself.

With the power log on, the marks `s5s_on`, `s5s_light`, `s5s_deep`, `s5s_reset`, `s5s_cut` and `s5s_end` give one voltage trace per phase. They are too short for mAh; the agent only checks them for anything odd. For 5b the marks are `s5_A1`, `s5_B1` ... `s5_end`. The arm decision (BCM cut vs panel standby) waits for D3's 24 h drops.

## 2.10 Recovery test and close (about 5 minutes)

1. **Hold Soft sleep at Off** (the default) unless the owner has decided otherwise after 2.11.
2. **Apple's recovery:** reset (MENU + SELECT), hold **MENU** at boot, **photograph Apple's menu**, then reset again back to ProkBox. This must work whatever happened above, deep-sleep power-off included.
3. Plug in. **You:** run `sudo fsck_msdos -n` again; the orphaned-cluster count should match 2.0. Paste or photograph it.
4. With your go, the agent pulls the final files and (if you want a clean slate) turns the power log off and clears it.

## 2.11 Pass criteria that turn soft sleep on by default (the owner's decision)

Soft sleep stays **Off by default** (today's power-off on every sleep request) until **all** of these hold. The agent reports each one as met or not met, with the log lines.

1. **Poll timer (2.3):** T1 passes (30 s polls land on time, 10 min total).
2. **Every button wakes (W1 to W5):** within 0.5 s, `why=1`, `polls=0`.
3. **Swallowed press (W2 to W5, S1, S2, 2.6 step 1):** no list moves, opens or acts on the waking press or its release.
4. **No false wakes (S3, H5):** wheel touch and turn never wake; with Hold on no button wakes.
5. **Hold wake window (H1 to H4):** Hold-off wakes at 2, 5 and 9 minutes and does not at 11 minutes.
6. **Charger and USB wake (C1 to C3):** both wake, USB reaches the USB screen even with Hold on.
7. **Alarm (A1):** wakes at 2 minutes plus or minus 2 s.
8. **Refusals (R1 to R4, 2.6 step 4):** none ends in a power-off or reset; each leaves the unit awake.
9. **Deep sleep (Y1):** powers off in the dark with no visible flash and boots normally; MENU at boot still reaches Apple's firmware (2.10).
10. **Retention (D1 implicit):** every sleep through 30 minutes, and the 1 h and overnight sleeps, wake into a working UI with no reboot.
11. **Wake latency (D4):** 0.5 s or less to the first lit frame (optional; if not measured, the criterion is open, not failed).
12. **Battery (D3, part 3):** see 3.2: a mean 24 h drop at or under **1 %/day** passes; **3 %/day or more** fails (18-4, the deeper tier, runs); between 1 and 3 %/day, 18-4 stays off and the numbers come back to you.

If 12 is undecided, soft sleep may still be turned on for daily use by your decision; D3 then keeps running.

---

# Part 3. The unattended runs

## 3.1 H3: Drive power A/B, one short automatic run (#114)

**This replaces the two full rundowns** (run A and run B): the owner decided full runs are too long. The firmware alternates the two modes itself, so both are measured in one sitting on the same charge, same volume and same playlist. Needs the build with the **Drive power A/B** rows (this branch) installed, with your go and a fresh backup as always.

What the test does: it switches **Drive power** between **off when idle** (A) and **standby only** (B) every 30 minutes (or 60), for 2, 4, 6, 8 or 10 hours (A B A B ...; an even count, so A and B get equal time and it ends on B). Each switch is a mark in `power.csv`: `ab_A1`, `ab_B1`, `ab_A2`, ... `ab_end`. At the end, or if you stop it, the mode goes back to what it was.

Steps (the default is 4 hours, 30 minutes per switch):
1. Charge to full. Unplug. **Rest 10 minutes.**
2. **Settings, Repeat: All.** The test refuses to start with Repeat off ("set Repeat first"), because a playlist that ends ends the run.
3. Start a **long, mostly-FLAC playlist** and **watch it play for a few seconds** to confirm it is playing (not paused). The test refuses to start otherwise ("start music first").
4. **Note the volume** (the agent needs it) and leave it alone from here.
5. Developer: **Drive power A/B length** picks 2, 4, 6, 8 hours or Overnight (10 h); **Drive power A/B switch** picks 30 or 60 minutes. Leave them at 4 h and 30 min for the standard run.
6. Developer: select **Drive power A/B (4 h)**. The row changes to **Drive power A/B: running 0:00**. If it says "start music first" or "set Repeat first" instead, fix that and select it again.
7. Leave the Developer list (the music keeps playing), **switch Hold on**, and **leave it alone**. Do not plug in USB, do not touch the wheel.
8. After the chosen time (about 4 hours for the default), **plug it in**. The row would then read **done**. Do not unplug before the agent has copied the logs.

If the music stops for more than about 2 minutes, the test halts and puts the mode back (`ab paused` in the log), so a run that stopped is obvious. Restarting or switching the unit off aborts it (the mode returns to its saved setting on the next boot, and the log shows the restart without an `ab_end`).

The agent reads `power.csv` between each pair of marks (battery mV and percent, `stor_on_pct`, `stor_ons`) and `ipodui.log` (`stor pwr` lines, `pcm underrun`, `ata` errors), and compares A against B over equal time.

**Expected resolution:** a difference of **about 10 %** in drain between A and B can be resolved by a 4 h run; a smaller difference is reported as **"not resolved"**. [model: the drive is on about 4 % of the time in A and 100 % in B at 1.1 Mbit/s FLAC; a longer run or the Overnight length narrows the error]. The power log at 30 s rows is about 120 B each, 144 KB for 10 h, far inside its 4 MB file, so nothing rolls and the start is kept.

## 3.2 D3: sleep current by 24 h voltage drops (only once soft sleep passes D2)

Run these only when 2.11 items 1 to 10 are met, or the agent asks you to.

**Each run:**
1. Charge to full. Unplug. Rest 30 minutes. Power log on, every 30 s, then **Power log: off** just before you start (the sleep must not run the log).
2. **Soft sleep: On.** Hold PLAY: the unit sleeps. **Leave it for the time the agent gives you (20 hours by default).** Hold off, nothing connected, no touches.
3. After the sleep (20 or 24 hours, as the agent says) press a button. The agent reads the `sleep wake ... s= mv=` line and the battery at wake through the learned table, and the percent change.

Repeat 3 times (3 days), or once for 72 h. The method's resolution is about plus or minus 1 %/day for one reading, plus or minus 0.6 %/day for the mean of three.

**Arms (for the BCM and drive questions), one 24 h each, same start state:**
- drive off at sleep vs standby only (the **Drive power** row);
- the video-chip cut vs the panel in standby (the lab's BCM cut tier, once the agent has a build with the switch);
- wheel on vs off with Hold on (the Wake test, wheel kept on build).

Rules of thumb from the brief (Q5): budget is 2 % of the capacity a day (60 mAh, 2.5 mA average). **At or under 1 %/day passes. 3 %/day or more fails and 18-4 runs. Between 1 and 3 %/day, 18-4 stays off and the numbers come to you.** `SLEEP_DEEP_AFTER` is 24 h, so a sleep that long ends in a deep power-off, not a wake. Read the drops at **20 hours** (press a button then) and scale to a day, or use the build the agent prepares with a longer deep timer for these runs.

Optional later: if a serious question remains, the deferred USB-C meter session S-M1 (#111) comes back. Not now.

---

## Files the agent pulls afterwards

- `ipodui.log`, `power.csv` (and `power.csv.1` if present); for #114 the `stor pwr` lines and the `stor_*` columns.
- `bcmtest.log` (the lines `5i ...`, `5s ...`; each block ends with a `(bcmtest.log this boot: ...)` line) and `bcmtest-results.log`.
- `batcal.run`, `batcal.dat`.
- The `sleep` lines: `sleep lab`, `sleep wake`, `sleep poll`, `sleep stack`, `sleep enter`, `sleep refused`, `sleep deep`, and the `bootbatt` lines.
- Your photographs (video-chip summaries, the busy glyph, battery states, the first lit frame after a wake, Apple's menu), the fsck output before and after, your notes of times for the H rows, and any 240 fps video.

Results go to `docs/bcm2722-reference` (open questions 3, 12, 49 to 51, tagged [silicon]), `docs/data-paths.md`, `docs/hardware.md` and the #70 comment (#111 gets a note that the meter session is deferred).

## If something goes wrong

- **Hang:** hold **MENU + SELECT** for 6 to 8 s. The BCM tests write only their two logs.
- **A Sleep lab row that does not wake:** wait for its cap (10 or 30 minutes) before you reset; a late wake is itself a result. Note the time.
- **Dark screen after a restart:** wait out the backlight timeout and press a button, or hold **Play/Pause** to switch off and on again.
- **A refusal that powers the unit off:** stop the sleep rows, note the row and tell the agent.
- **Anything that looks like data damage** (fsck complaints beyond the usual orphans, missing files): stop, do not copy anything further, and tell the agent. The backup from 2.0 is the way back.
