# Extras and Games SDK: developer reference (rev 0)

**Status:** design, revision 0 (P12 package W4-0, 2026-10-23), for review by `budget-reviewer` before any game code. It seeds the release **Game Development Guide** (P20, ROADMAP row 23).

**Sources:**
- binding decisions: `docs/phase12-brief.md` §2 (the resource classes) and §2A (the SDK outline), with every owner decision recorded there, and the owner's 2026-10-23 additions (per-game settings; a falling-block game);
- the wave-1 framework this SDK sits on: `apps/ipodui/modload*.{c,h}`, `modclass.{c,h}`, `extras*.{c,h}`, `tools/modpak.py`, `apps/ipodui/modules/class_*.deny`, `modules.make`, `module.lds`;
- the BCM2722 job interface for Classes B and C (branch `p19-bcm-prep`): `bcmfw/jobs.h`, `bcmfw/jobs/bcmjob.h`, `docs/research/bcm2722/jobs.md`, `memory-contract.md`, `power-contract.md`; the drawing model in `docs/bcm2722-reference/graphics.md` (branch `p14-sleep-spikes`);
- the games research: `docs/design/games.md`.

**Names:** PP5022 (the main SoC; its two ARM7TDMI cores are the **CPU** and the **COP**), **BCM2722** (the video co-processor, "the BCM" below), **WM8758** (the audio codec). Evidence tags as in the brief: [built] in the tree on `p12`, [ours] our design, [rockbox-derived], [silicon], [est], [hypothesis].

**Who reads this:** anyone writing a game for ProkBox, starting with our own: **Solitaire**, **Bricks**, **Chute** and **Quadrus** (the falling-block game, §13.2). A long-term rhythm game (§13.4) is the Class C example the SDK is shaped to support. A game is written against `sdk.h` alone. If a game needs something the SDK lacks, that is an SDK change, never a direct call into the core (brief §6, W4-1..3).

---

## 0. Status of every part

| Part | State | Lands with |
|---|---|---|
| Module pack v2, `struct mod_desc` at export slot 0, `mod_runnable()` and hiding, the manifest parser, the size and deny-list checks for shell modules, `mod_work_alloc()`, `ui_boost_burst()`, the Class A accountant (`modclass.c`) | **[built]** wave 1 | P12 W1-F |
| `sdk.h`, `sdk.sym` (the link allow list), the resident SDK table, the loop, the input layer (chord, `SDK_IN_CANCEL`), the Class A drawing calls, the state record and settings, the file confinement, the watchdog prototype, `sdk_badgame`, `modpak.py` SDK-kind checks, the per-game string tables | **planned, P12 W4-0 (code)** | this phase, after this document's review |
| Classes A games: Solitaire, Bricks, Chute, and Quadrus in its Class A form (§13.2) | planned, P12 W4-1.. | this phase |
| **Class B**: the borrow from the audio buffer | planned | **#108** (shrink without a stop) **and P19** |
| **Class B/C drawing** on the BCM2722 (scene/command API, texture slots), music pause/resume (B), library views (B), the absolute wheel mode | planned | **P19** (the BCM job driver, `J_CLS_APP`), and #108 for anything that borrows |
| **Class C**: the handover session, 150 MHz BCM2722, `J_MODE_GAME`, the game's own audio | planned | **P14** (video, the handover) **and P19** |
| Cutscene video (`sdk_video_play`) | planned | **P14 and P19** |
| Wheel regions with timestamps (`sdk_wheel_regions`), the game's own songs through our codecs with a sample-accurate position (`sdk_song_*`), the global audio/input offset, 60 fps with TE sync, tile and sprite layers, the read-only game media folder | planned (for the rhythm game, §13.4, a long-term idea) | **P19** (layers, TE), **P14** (the session), plus the device checks D-W1, D-W2, D-A1 (§15) |
| The COP's own watchdog (when games move to the COP front end) | planned | **P19** |
| Signing and the install allow list for third-party games | planned | **P20** (Q25) |

Today `MOD_CLASSES_BUILT` is `1 << MOD_CLASS_A` (`modload_core.h`), `mod_borrow()` always refuses and `mod_session_enter()` always fails (`modload.c` :140–162) [built]. A Class B or C game is therefore **hidden** on every unit until its phase lands (`mod hide <name> why=class`), never shown as an error.

---

## 1. Concepts

### 1.1 A game is a module

A game is one **SDK module** (`kind = sdk`) in `modules.pak`, loaded into the module arena when its row in Extras › Games is highlighted (intent preload) and run when it is selected. Nothing of it stays resident except its row and its play count, which the firmware owns (§8.5).

Module rules (from `module.lds`, enforced at link time) [built]:
- Thumb code; **no writable `.data`** (make it `const`, or `.bss`); **no IRAM** (IRAM is the codecs');
- `.bss` is zeroed at every load and every move, so nothing survives in RAM between two runs: persistent data goes through the state record (§8);
- the image may be **moved** in the arena between runs (memmove plus relocation). Never keep a pointer into your own image anywhere outside the image.

What is new for SDK modules (W4-0): they link **only against `sdk.sym`**, the SDK's versioned symbol table, not against `rockbox.elf` (§11.1). Shell modules (our Extras screens) keep `--just-symbols` against the core.

### 1.2 The three classes

The class is a contract: what the game may use, what happens to music, and how a breach is caught (brief §2.1).

| | **Class A: lightweight** | **Class B: heavier, beside music** | **Class C: exclusive** |
|---|---|---|---|
| Music | keeps playing; **the firmware keeps PLAY, NEXT, PREV and short MENU** | keeps playing at a smaller buffer; the game may pause and resume it | **paused at entry**, left paused at its spot on exit (Q23) |
| The game's input | SELECT, the wheel (**relative only, never volume**) and its own UI | **every button except the reserved holds**; MENU instant | as B |
| Drawing | the CPU, into the shell's framebuffer, as **small dirty bands** pushed by the shell's partial update | **the BCM2722 only**, through the scene/command API, at **27 MHz** | the BCM2722, at **150 MHz** |
| RAM | the module arena only: image + `.bss` + one working block | + a **borrow** from the audio buffer | the **whole audio buffer**, lent for the session |
| Built | P12 | #108 + P19 | P14 + P19 |

**Video is exclusive of games and of music.** A game may play its own pre-rendered cutscene through the SDK's video call (§10), as Class C, or as Class B escalated to Class C resources for the length of the clip, with music paused.

### 1.3 Firmware authority

The firmware keeps "ultimate authority of what is running and what it is doing" (owner):
- it **tracks every resource** a game holds and **reclaims all of it** at exit, whatever the game did;
- **per-class budgets are enforced at runtime** (§6);
- games get **no threads and no hardware access**;
- a game that overruns is unwound or killed by the **watchdog** (§11.3), with a controlled reboot that saves the resume point as the last resort.

---

## 2. Lifecycle

### 2.1 The entry table

Export slot 0 of every module is its `struct mod_desc` (the header, §4.2). Export slot 1 of an SDK module is its `struct sdk_game`:

```c
#include "sdk.h"

struct sdk_game {
    /* allocate (sdk_work / sdk_borrow / sdk_session_mem), read the state
     * record, declare the wheel mode, draw the first frame.
     * < 0 refuses: back to the Games list, logged, no splash. */
    int  (*init)(struct sdk_ctx *c);

    /* one frame: read input, update, draw. Returns the ms until the next
     * step (clamped to >= SDK_FRAME_MIN_MS, i.e. <= 30 fps), or SDK_IDLE:
     * call me again only when input arrives. */
    int  (*step)(struct sdk_ctx *c, const struct sdk_input *in,
                 uint32_t now_ms);

    /* Class A only: a short MENU. true = handled inside the game (closed
     * an overlay, cancelled a pick); false = the game is at its top
     * level: the SDK exits it (its exit policy, then a pop slide). */
    bool (*back)(struct sdk_ctx *c);

    /* stop motion, keep state: Hold, an alarm ring, a soft sleep, the
     * Sleep Timer's end, the SDK's own settings screen, a cutscene */
    void (*suspend)(struct sdk_ctx *c, enum sdk_why why);

    /* redraw in full (the framebuffer or the BCM frames may have been
     * reused while suspended) */
    void (*resume)(struct sdk_ctx *c);

    /* graceful policy only: finish the state record within exit_ms and
     * return. Never called for an immediate-policy game. */
    void (*exit)(struct sdk_ctx *c, enum sdk_why why);

    /* the game's settings, or NULL (section 9); read by the SDK before
     * init so their values are ready in init */
    const struct sdk_settings *settings;
};

SDK_GAME(quadrus) = { .init = st_init, .step = st_step, .back = st_back,
                     .suspend = st_suspend, .resume = st_resume,
                     .exit = st_exit, .settings = &st_settings };
```

`SDK_GAME(name)` (planned, `sdk.h`) places the table at export slot 1 and, in the simulator (where modules link statically), names it `sdk_game_<name>`. `SDK_DESC_A/B/C(...)` builds slot 0 from the same values as the manifest (`modpak.py check` refuses a mismatch, [built]).

### 2.2 States

```
loaded -> init -> running (step, back) <-> suspended (suspend/resume)
                      |
                      v
          exiting (exit, graceful only) -> reclaimed -> the Games list
```

- **The SDK owns the loop.** A game never loops on its own, never waits and never blocks. All calls are made on the UI thread (the front end), and each returns within its slice (§6). Between calls the UI thread goes back to the scheduler, so playback never starves (Rockbox's scheduler is cooperative [rockbox-derived]).
- **Frames are dropped, not queued.** `now_ms` is the real time. Advance your world by the elapsed time in fixed steps (we use 1/30 s, with at most 2 catch-up steps, then drop the rest), so a late frame never slows the game down.
- **`SDK_IDLE`** is for board games: Solitaire waits for input at no CPU cost. An animation (a card glide) returns 33 until it ends.
- **Suspend and resume** are always paired. While suspended, the game gets no `step()`. A **soft sleep** (P18) suspends: the game stays in RAM and resumes at wake. **Covering screens suspend too** (P12 integration, [built]): the alarm's ring (`SDK_WHY_ALARM`), any other system screen pushed over the game, such as the lock screen (`SDK_WHY_SYSTEM`, appended), and, in Class A, **a USB data session** (`SDK_WHY_USB`): the USB screen acks first, then the SDK marks the game suspended **without calling it** (no game code runs while the host owns the disk, so the watchdog can't reboot with the volume mounted); the game gets its `suspend()` when the session ends, just before its `resume()`. **The record of a suspend is written at the next natural disk wake** (the music's refill, a list's read), never by a spin-up of its own and never at USB entry or during the session. A soft sleep suspends the game before its quiesce and writes the record with the sleep's own saves (`persist()`, beside the status save); so does the charging screen's sleep. A **power-off or a low battery** exits instead.
- **Exit** comes from: the exit chord (SELECT + PREV held 2 s), Class A's `back()` returning false or a long MENU (unless the game took it as its pause, §3.2a), the `SDK_STR_EXIT` row of a pause menu (§5.4), `sdk_exit_request()` (B/C), or the firmware (USB, power-off, low battery). The game sees the reason in `why`.

### 2.3 Reasons

```c
enum sdk_why {
    SDK_WHY_USER,       /* the exit chord, back() == false, a long MENU
                           (A), or the game's own Exit (B/C) */
    SDK_WHY_HOLD,       /* the Hold switch (suspend) */
    SDK_WHY_ALARM,      /* an alarm rings on top (suspend) */
    SDK_WHY_SLEEP,      /* hold PLAY or the idle timer: soft sleep
                           (suspend) or power-off (exit) */
    SDK_WHY_TIMER,      /* the Sleep Timer ended (suspend) */
    SDK_WHY_SETTINGS,   /* the SDK's settings screen is on top (suspend) */
    SDK_WHY_VIDEO,      /* a cutscene plays (suspend) */
    SDK_WHY_USB,        /* a USB data session (exit) */
    SDK_WHY_BATTERY,    /* low battery (exit) */
    SDK_WHY_SYSTEM,     /* another system screen covers the game, e.g.
                           the lock screen (suspend); appended in P12 */
};
#define SDK_IDLE          (-1)
#define SDK_FRAME_MIN_MS  33    /* 30 fps ceiling */
```

### 2.4 The exit policy

Declared in the header (`exit = graceful <ms>` or `exit = immediate`) and **enforced, not trusted**:
- **Graceful:** the SDK calls `exit()` and starts a deadline of `min(exit_ms, SDK_EXIT_MS_MAX)`, where **`SDK_EXIT_MS_MAX` = 1,000 ms** (owner; `modload_core.h` [built], `modpak.py` refuses more [built]). The slice clock counts only time inside game code (§6.2). If `exit()` has not returned by the deadline, the firmware takes over through the watchdog and reclaims anyway.
- **Immediate:** no call. The SDK reclaims at once. The game must keep its state record consistent at every return (it "saves as it goes"); the SDK then writes the last record it holds (§8.2).
- **Both:** RAM back to the arena or the audio buffer, BCM slots freed and the BCM back to UI mode, boosts dropped to their count before entry, files closed, the music state restored for the class, then the Games screen with a pop slide.

### 2.5 Lifecycle and utility calls

```c
/* B/C: leave the game (its own Exit item); the SDK runs the exit path
 * after step() returns. Class A: SDK_E_CLASS (it exits through MENU). */
int      sdk_exit_request(struct sdk_ctx *c);
/* B/C: hide the shell's header bar for a full 320 x 240 scene */
int      sdk_header(struct sdk_ctx *c, bool shown);
/* time: ms since an arbitrary origin, from the RTC and the tick, so it
 * keeps counting through a soft sleep; us: the SDK's microsecond clock
 * (region stamps, song positions), valid within one running stretch */
uint32_t sdk_now_ms(struct sdk_ctx *c);
uint32_t sdk_now_us(struct sdk_ctx *c);
/* randomness (there is no libc rand) */
uint32_t sdk_rand32(struct sdk_ctx *c);
/* Class A: one boost burst of <= 100 ms (ui_boost_burst's rules);
 * false = refused (over 10 % in 10 s). B/C with boost = session are
 * boosted by the SDK while running. */
bool     sdk_boost_burst(struct sdk_ctx *c, int ms);
void     sdk_boost_end(struct sdk_ctx *c);
```

---

## 3. Input

### 3.1 Who owns which input

Owner decisions of 2026-10-23 (brief §2A.4, Q21, Q22, Q26, Q29). **A reserved input never reaches a game, not even as a release**; the input layer swallows it before `step()`.

| Input or event | Class A | Class B | Class C |
|---|---|---|---|
| **SELECT** (press, release, long) | game | game | game |
| **Wheel** | game, **relative only**; never volume | game, **relative or absolute** (the game chooses) | as B |
| **PLAY/PAUSE** | **firmware**: play/pause music at release | game, **tap only, at release** (its hold is power-off) | as B |
| **NEXT / PREV** | **firmware**: skip/back at release, seek on hold | game (press, release, repeat) | as B |
| **MENU** | **short: firmware, delegated**: the SDK calls `back()` (true = handled; false = exit). **Long (500 ms): exit** (Q29), or, if the game opted in with `sdk_long_menu(c, SDK_LONGMENU_PAUSE)`, one `{ SDK_BTN_MENU, SDK_IN_LONG }` event: the game opens its pause menu (§3.2a) | **game, fully and instantly**: press, release, hold, repeat | as B |
| **SELECT + PREV held 2 s** | **firmware: exit the game** (its policy, then the Games screen); a shorter overlap does nothing | same | same |
| The game's own Exit item | an `SDK_STR_EXIT` row in its pause menu: the SDK runs the graceful exit itself (§5.4) | `sdk_exit_request()` (or the same row): the same path as the chord | as B |
| **Hold PLAY** | **firmware**: `suspend`, then sleep (P18) or the dark wait; `exit` first before a power-off | **firmware**: the exit path, then sleep or power-off | as B |
| **Hold SELECT + MENU** | **the hardware reset**: never input; works even when the firmware is hung | same | same |
| **Hold switch** | **firmware**: all input blocked; `suspend`, and `resume` when released | same | same |
| **Volume** | none: a Class A game never changes the volume | none (music volume stays the firmware's) | the game's own audio gain (§7.3) |
| **Music control** | the firmware's buttons keep working | the game may offer pause/resume (§7.2); no skip, no volume | paused at entry, left paused |
| Alarm ring | `suspend`, the ring screen on top, `resume` after Snooze/Dismiss | same | same (its own audio paused) |
| USB data | `exit` (graceful deadline), then the USB screen | same | same |
| Charger in/out | no effect in play | same | same |
| Low battery | `exit`, then the low-battery path | same | same |
| Sleep Timer end | `suspend`, then sleep | same | same |

**The exit chord in detail:**
- The moment SELECT and PREV are **both down**, the SDK withholds both. If one of them was already delivered to the game (Class A: SELECT; B/C: either), the game receives **`SDK_IN_CANCEL`** for that button and must drop that press's action. In Class A, PREV's firmware action (skip back) is withheld the same way.
- **Released before 2 s:** nothing happens. No event of either button reaches anyone after the overlap began.
- **Held 2 s** (`SDK_EXIT_CHORD_MS` = 2,000): the exit policy runs.
- **Design consequence:** never map a frequent action to **PREV while SELECT may be held** (or SELECT while PREV may be held). Fast players overlap them, and every overlap is swallowed. Quadrus (§13.2) leaves PREV out of play for this reason.

### 3.2 Events and wheel state

```c
enum sdk_btn {                 /* bit masks */
    SDK_BTN_SELECT = 1 << 0,
    SDK_BTN_MENU   = 1 << 1,   /* B/C only */
    SDK_BTN_PLAY   = 1 << 2,   /* B/C only, SDK_IN_TAP only */
    SDK_BTN_NEXT   = 1 << 3,   /* B/C only */
    SDK_BTN_PREV   = 1 << 4,   /* B/C only */
};
enum sdk_in_kind {
    SDK_IN_PRESS,              /* press edge (not PLAY) */
    SDK_IN_RELEASE,            /* release edge */
    SDK_IN_REPEAT,             /* held: every SDK_REPEAT_MS after the first
                                  SDK_REPEAT_DELAY_MS */
    SDK_IN_LONG,               /* held 500 ms (LONG_PRESS_TICKS, input.c):
                                  once per hold; the hold's later repeats
                                  are SDK_IN_REPEAT */
    SDK_IN_TAP,                /* PLAY released before its long press */
    SDK_IN_CANCEL,             /* this button became part of the exit
                                  chord: undo its press's action */
};
struct sdk_btn_ev { uint8_t btn, kind; uint16_t at_ms; };

#define SDK_IN_EV_MAX 8
struct sdk_input {
    uint8_t  n;                 /* events since the last step, oldest first */
    struct sdk_btn_ev ev[SDK_IN_EV_MAX];
    uint8_t  held;              /* owned buttons down now (enum sdk_btn) */
    uint8_t  flags;             /* SDK_INF_* */
    /* relative wheel (every class) */
    int16_t  steps;             /* signed clicks since the last step,
                                   clockwise positive */
    uint16_t velocity;          /* clicks/s, as the shell's lists (scroll.c) */
    /* absolute wheel (B/C, SDK_WHEEL_ABS only; else pos = -1) */
    int8_t   pos;               /* 0..95, clockwise increasing; -1 untouched */
    int8_t   dpos;              /* change since the last step, wrapped to
                                   -48..+47 */
    uint8_t  touched;           /* finger on the ring */
    int8_t   menu_pick;         /* section 5.4: the row picked in an open
                                   SDK menu, or -1 */
    /* wheel regions (C, SDK_WHEEL_REGIONS only, section 3.4) */
    uint8_t  rg_n;
    struct sdk_region_ev rg[SDK_RG_EV_MAX];
};
#define SDK_INF_DROPPED   0x01  /* more than SDK_IN_EV_MAX events: the
                                   oldest were dropped */
#define SDK_INF_SETTINGS  0x02  /* a setting changed (section 9) */
#define SDK_INF_RESUMED   0x04  /* first step after resume() */
#define SDK_INF_VIDEO     0x08  /* a cutscene ended (section 10) */
#define SDK_INF_SONG_END  0x10  /* the game's song ended (section 7.5) */
```

In Class A, events of the firmware's buttons never appear: `ev[]` holds only SELECT, and `held` only `SDK_BTN_SELECT`. The one exception is the opted-in long MENU (§3.2a).

### 3.2a The long MENU as a pause (Class A, opt-in) [built, 2026-10-24]

```c
enum sdk_longmenu { SDK_LONGMENU_EXIT, SDK_LONGMENU_PAUSE };
int sdk_long_menu(struct sdk_ctx *c, enum sdk_longmenu m);  /* table entry 43 */
```

- **`SDK_LONGMENU_EXIT`** (the default, every game that doesn't call it): a long MENU leaves the game, as Q29 decided.
- **`SDK_LONGMENU_PAUSE`**: a long MENU no longer leaves. At 500 ms the next step gets one `{ SDK_BTN_MENU, SDK_IN_LONG }` event, and the game opens its pause menu. The release that follows is not a short MENU (no `back()`). With the overlay already open the event still comes, and the game may ignore it.
- Either way short MENU stays `back()`, so a game that pauses on the long MENU is free to give the tap its own action (Quadrus: the hold slot).
- **The firmware keeps its reserved inputs**: SELECT + PREV held 2 s exits, hold PLAY sleeps or powers off, hold SELECT + MENU is the hardware reset. The game must offer a way out: an `SDK_STR_EXIT` row in the pause menu (§5.4).
- Call it at `init()` or any time; Class B/C own MENU outright and get `SDK_E_CLASS` (planned; Class A only is built).
- Trace: `sdk long menu <name> pause`.

### 3.3 Wheel modes

```c
enum sdk_wheel { SDK_WHEEL_REL, SDK_WHEEL_ABS,
                 SDK_WHEEL_REGIONS };   /* C only, section 3.4 */
/* At init or any time. Class A: SDK_WHEEL_ABS returns SDK_E_CLASS. */
int sdk_wheel_mode(struct sdk_ctx *c, enum sdk_wheel m);
/* Read-only absolute position, polled; see Q30 below */
int sdk_wheel_pos(struct sdk_ctx *c, int *pos, bool *touched);
```

- **Relative** (every class, the default): click steps with velocity and acceleration, exactly as the shell's lists, so a menu-mode game feels like the rest of the UI.
- **Relative, raw** (`SDK_WHEEL_REL_RAW`, every class, appended in P12 [built]): one step per driver click, no acceleration; `velocity` is still filled, so a game that wants its own curve (a paddle's speed) scales by it itself. `sdk_wheel_mode()` switches between the two at any time; the owner's rule is that games choose.
- **Absolute** (B/C, planned with P19): the finger's position, 0–95 per turn, and touch/untouch, for paddles and dials. The SDK switches the driver's scroll events off (`wheel_send_events(false)`, `button-clickwheel.c` :337 [rockbox-derived]), so the relative stream doesn't also fire, and restores it at exit. Untouch: `wheel_status()` returns −1 when the last packet had no touch bit [rockbox-derived]; whether the controller sends a packet when the finger lifts is device check **D-W1**. If it doesn't, the SDK reports untouch after 150 ms without packets.
- **`sdk_wheel_pos()` in Class A** [built, #132]: Q30's default, taken by the owner's Chess requirements (2026-10-24: "use the SDK's absolute wheel position"): a polled, read-only position (0-95 clockwise from the top, under MENU [rockbox-derived]; -1 with no finger) in every class; the event stream stays relative and the driver is untouched (no `wheel_send_events()`). No ABI change: the table entry existed. A game that calls it is a **watcher**: the driver posts nothing when a finger lands without turning, so while the game is idle the SDK polls the driver's touch count and the position, every 50 ms while lit and every 500 ms while dark (`sdk_ww_poll()`, `sdk_core.c`, host-tested), and steps the game when a finger is on the ring, has just lifted, landed and lifted between two polls, or when the screen lit again. Only a watcher is polled; other games cost nothing new. Three input flags, appended (ABI 0.1 unchanged): `SDK_INF_PRESSED` (a button, the firmware's too, went down since the last step: a finger on the ring now is pressing it, not tapping), `SDK_INF_LIT` (the screen lit again: the touch that lit it is no tap), `SDK_INF_DARK` (the screen is dark now: a game should not search or animate, and may return `SDK_IDLE`; every game sees it, only Chess uses it). The simulator's position comes from the script command `wheel_at <pos> <ms> [KEY | to_pos]` (`sim_cmd_extras.c`). **[device] D-W1**: whether the controller sends packets while a finger rests still, and a lift packet; `wheel_status()` keeps the last position until one arrives.
- **`SDK_WHEEL_REGIONS`** (Class C) is a third mode built on the absolute one (§3.4).

### 3.4 Wheel regions with timestamps (Class C) [planned P19]

For timing games (the rhythm game, §13.4) the SDK turns the absolute wheel into **touches of named regions, each stamped with the time its packet arrived**, so the game judges against when the finger landed, not when the next `step()` ran.

```c
enum sdk_rg_layout {
    SDK_RG_CARDINAL,    /* 4 regions centred on MENU (up), NEXT (right),
                           PLAY (down), PREV (left) */
    SDK_RG_CORNERS,     /* 4 regions centred between them: up-right,
                           down-right, down-left, up-left */
};
enum sdk_rg_kind { SDK_RG_TOUCH, SDK_RG_SLIDE, SDK_RG_LIFT };
struct sdk_region_ev {
    uint8_t  region;    /* 0..3, clockwise from the top (or top-right) */
    uint8_t  kind;      /* TOUCH: the finger landed in it; SLIDE: it moved
                           into it without lifting; LIFT: it left the ring */
    uint8_t  late;      /* LIFT inferred from silence, not a packet */
    uint32_t at_us;     /* the packet's arrival, on the SDK's microsecond
                           clock (the same clock as sdk_song_pos) */
};
#define SDK_RG_EV_MAX 16
/* Switches to SDK_WHEEL_REGIONS: absolute mode, the 96 positions mapped
 * to 4 regions of 24, minus a dead band of `dead` positions at each
 * border (0..6) so a finger resting on a border doesn't flicker */
int sdk_wheel_regions(struct sdk_ctx *c, enum sdk_rg_layout l, int dead);
```

- **Where the stamp comes from:** the click-wheel driver records the microsecond timer when each packet's interrupt arrives (a small change in `button-clickwheel.c`, the platform owner's; one word per packet). The SDK maps packets to regions at the next step; only the mapping waits, not the stamp.
- **One finger only.** The wheel reports a single position per packet [rockbox-derived], so two regions can never be touched at once. A game must not need simultaneous regions (§13.4 handles "jumps").
- **Lift:** from the controller's own untouch packet if it sends one (D-W1); otherwise 150 ms after the last packet, stamped with that last packet's time and marked `late`.
- **Latency and resolution are device facts:** the packet rate while touching bounds both (a packet every *t* ms quantises stamps to *t*). D-W1 and D-W2 (§15) measure them before any timing window is chosen.
- The buttons under the four cardinal points stay ordinary button events (B/C press edges; PLAY only as a tap). They are not region hits: a click needs more force and lands later than the touch.

---

## 4. Header and manifest

### 4.1 The manifest (`apps/ipodui/games/<name>/<name>.mod`)

`key = value` lines, `#` comments; `modpak.py manifest` parses and checks them, and an unknown key or a bad value refuses the build [built].

| Key | Values | Meaning | State |
|---|---|---|---|
| `class` | `A`, `B`, `C` | the resource class (§1.2) | [built] |
| `min_ram` | `32`, `64` | the smallest unit it runs on. **Class A is always 32** (`modpak.py` refuses 64) | [built] |
| `kind` | `sdk` | an SDK module (shell modules say `shell`) | [built] |
| `abi` | `0.1` | the SDK ABI it was built against; required for `kind = sdk`. A different major, or a newer minor than the core's, refuses the build | [built] |
| `exit` | `graceful <ms>` (≤ 1000), `immediate` | the exit policy (§2.4) | [built] |
| `work` | bytes | Class A: the working block. Image + `.bss` + work ≤ 56 KB, image ≤ 32 KB | [built] |
| `quota` | **KB** (≤ 1024; default 64 for games) | the private storage quota (§8.4). Note: `quota` is in KB while `work` and `borrow` are in bytes, as the parser is built | [built]; default planned W4-0 |
| `borrow` | bytes | Class B: the largest borrow (§5.6) | [built] (parsed); runtime #108 |
| `boost` | `burst`, `session` | Class A is `burst`; `session` holds a boost while running (B/C) | [built] |
| `exports` | names | the export table's name (`MOD_EXPORTS`) | [built] |
| `state` | bytes (≤ 4096) | the state record's size (§8.2), taken from the working block (A) or the borrow (B) | **planned W4-0** |
| `strings` | a `.lang`-style file | the game's own string table, names included (§4.3) | **planned W4-0** |
| `textures` | `<id>:<w>x<h> ...` | B/C texture slots (§5.5). B: all of them ≤ `J_BUDGET_APP` (56 KB) and ≤ `J_SLOTS_APP` (8) | **planned P19** |
| `cutscene` | `yes` | a Class B game may escalate for a clip (§10) | **planned P14 + P19** |
| `fps` | `30`, `60` | Class C only: 60 asks for TE-synced presents at the panel's rate (§5.3); default 30 | **planned P19** |
| `media` | a folder name | Class C only: a read-only **game media folder** the user fills over USB, outside the quota (§8.6) | **planned P14 + P19** |
| `songs` | `yes` | Class C only: the game plays its own songs through our codecs (§7.5) | **planned P14 + P19** |
| `cover` | a PNG, 96 x 96 | the game's **cover** (box art) in the Games list's panel (§4.4) | **planned** |

### 4.2 The header (`struct mod_desc`, export slot 0) [built]

28 bytes (`modload_core.h`): magic `"PKMH"`, `abi_major`/`abi_minor`, class, `min_ram`, kind, exit policy, `exit_ms`, `work_kb`, `quota_kb`, `borrow_kb`, `boost`, and `names_off` (the offset of the game's name table). The loader compares it with the pack entry before running anything (a mismatch is `MODPAK_EHDR`: logged, treated as absent).

**A unit that can't run a game hides it.** `mod_runnable()` is false for a different ABI major or a newer minor, `min_ram = 64` on a 32 MB unit, a class this firmware doesn't build, or a header mismatch. The row is then absent from the Games list: no page, no splash, only the log line `mod hide <name> why=abi|ram|class|hdr` [built].

### 4.3 Strings and the name

Every string a game shows comes from **its own string table**, built by `modpak.py` from `.lang`-style sources into the image (rule 8: localisable from day one). String ids are the game's own small enum (`ST_STR_*`); a game never uses the firmware's `LANG_*` ids, and third-party games ship their own tables. Entry 0 is the game's **name** in the Games list.

The SDK adds a few **standard words** that every game should use, so all games say the same thing (localised by us): `SDK_STR_RESUME`, `SDK_STR_NEW_GAME`, `SDK_STR_SETTINGS`, `SDK_STR_EXIT`, `SDK_STR_ON`, `SDK_STR_OFF`. Ids at or above `SDK_STR_BASE` (0x8000) name these.

Words follow the project rule: short labels may match the 7G's; every full sentence is our own.

### 4.4 The cover in the Games list

The Games list is a **split screen** (owner, 2026-10-24): the highlighted game's cover, 8-bit era box art (68 x 84 px) with the head-on reflection, sits in the panel at the right, the way every Extras row shows its icon. The cover has to show **before the module loads** (the row is highlighted while the intent preload is still reading), so it is never in the module image.

- **Now [built]:** our games' covers are the shell's own art, `apps/ipodui/art/extrasart.py` (`cv_chute`, `cv_bricks`, `cv_quadrus`, `cv_solitaire`; the 'pal' format, ~1.9-2.0 KB each, resident in `extrasart.h`), keyed by the `games[]` table in `sdk/sdk_games.c` (`UI_COVER_*`, `extras.h`); the panel is `ui_preview_x_gamecover` (`screens/panels.c`). A game without a cover (the developer test game) shows the Games icon. The title is lettered into the art; the list's row text carries the (localisable) name.
- **Follow-up [planned]:** a `cover` manifest key, so a third-party game supplies its own. `modpak.py` would convert the PNG to the same 'pal' format and store it in `modules.pak` **beside** the module (its own pack entry, not in the image), and the Games list would read the covers of its rows in one bulk read when it opens (a few KB, at most 32 rows) into its arena block, or take them from the snapshot (`docs/design/module-loader.md`), never one read per highlight. The shell's own covers stay the fallback.

---

## 5. Drawing and memory

### 5.1 The screen

```c
#define SDK_LCD_W 320
#define SDK_LCD_H 240
typedef uint16_t sdk_px;                /* RGB565 */
#define SDK_RGB(r, g, b) ((sdk_px)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3))
/* The play area below the shell's header bar (title = the game's name,
 * the clock and battery as everywhere). sdk_header(c, false) gives a
 * game the full 320 x 240 (B/C only in rev 0). */
void sdk_area(struct sdk_ctx *c, int *x, int *y, int *w, int *h);
```

### 5.2 Class A: CPU drawing in small bands [planned W4-0]

A Class A game draws on the CPU, straight into the shell's framebuffer (resident, 153,600 B; no back buffer), and marks what changed. After `step()` returns, the SDK merges the marks into **full-width bands** and pushes them through the shell's partial update (`ui_lcd_push`). The game never calls `lcd_update` and never pushes the whole screen except on a scene change.

```c
void sdk_fill(struct sdk_ctx *c, int x, int y, int w, int h, sdk_px col);
/* rows y..y+h-1 from a row-colour table: the background as a function
 * of y (gradients, flat fills). Erasing a sprite = refilling its rows. */
void sdk_rows(struct sdk_ctx *c, int x, int y, int w, int h,
              const sdk_px *row_col);
/* a rectangle of an RGB565 image (in your image's rodata or your
 * working block); _key skips pixels equal to key */
void sdk_bitmap(struct sdk_ctx *c, const sdk_px *src, int src_x, int src_y,
                int stride, int x, int y, int w, int h);
void sdk_bitmap_key(struct sdk_ctx *c, const sdk_px *src, int src_x,
                    int src_y, int stride, int x, int y, int w, int h,
                    sdk_px key);
/* a 1-bit mask filled with one of the shell's materials (gel, metal,
 * groove, selection), coloured per row as the shell does */
enum sdk_mat { SDK_MAT_GEL_BLUE, SDK_MAT_GEL_GREEN, SDK_MAT_GEL_RED,
               SDK_MAT_GEL_SKY, SDK_MAT_METAL, SDK_MAT_GROOVE,
               SDK_MAT_SELECT, SDK_MAT__N };
void sdk_mask(struct sdk_ctx *c, const uint8_t *mask, int stride_bytes,
              int x, int y, int w, int h, enum sdk_mat m);
void sdk_material(struct sdk_ctx *c, enum sdk_mat m, int x, int y,
                  int w, int h);
/* text in ShellSans (the shell's typeface) from the string table;
 * returns the width drawn. _num draws an integer; _fmt one %d. */
enum sdk_font { SDK_FONT_LIST, SDK_FONT_SMALL };    /* 15 px, 12 px */
int  sdk_text(struct sdk_ctx *c, int x, int y, int str, enum sdk_font f,
              sdk_px col);
int  sdk_text_num(struct sdk_ctx *c, int x, int y, int32_t n,
                  enum sdk_font f, sdk_px col);
int  sdk_text_fmt(struct sdk_ctx *c, int x, int y, int str, int32_t arg,
                  enum sdk_font f, sdk_px col);
int  sdk_text_width(struct sdk_ctx *c, int str, enum sdk_font f);
/* the width sdk_text_num() would draw n at; nothing drawn (P12, table
 * entry 42) */
int  sdk_num_width(struct sdk_ctx *c, int32_t n, enum sdk_font f);
/* rows y..y+h-1 changed (any x): pushed after step() as a band */
void sdk_dirty(struct sdk_ctx *c, int y, int h);
```

Rules:
- Every call **clips** to the play area and checks its source against the game's image or working block. An out-of-range call draws nothing and counts a violation (§12).
- **≤ 25 KB pushed per frame on average** (about 4.5 ms of CPU at 30 MHz [measured rate, S8]); at most 30 pushes a second.
- **Art is composed, not stored as whole screens:** masks plus materials, small tiles rendered once into the working block at `init` (`games.md` §1).

### 5.3 Class B/C: the scene on the BCM2722 [planned P19]

Class B and C games **draw only through the SDK's scene/command API on the BCM2722**. They have no raw access to the PP5022's framebuffer and push no pixels across the host port per frame. The SDK turns their commands into BCM2722 jobs (`bcmfw/jobs.h`, contract rev 0.1), and **owns the present and the tearing-effect wait**.

Why: a PP-pushed full frame costs 27 ms of CPU at 30 MHz, nearly a whole 30 fps frame, while a command list costs microseconds (`graphics.md` §2). The BCM2722 composes and drives the panel.

| | Class B | Class C |
|---|---|---|
| BCM2722 clock | **27 MHz** (UI mode, `J_MODE_UI`) | **150 MHz** (`J_MODE_GAME`; needs the 4b/4c flags, `power-contract.md` §2) |
| Its pool | `J_CLS_APP`: **56 KB, 8 slots** in SRAM, beside the shell's own pools | `J_CLS_GAME`: the SRAM arena (about 0.9 MB) and SDRAM (about 3.5 MB) [est], the UI's pools evicted (epoch + 1) |
| What it suits | 2D: fills, tiles, sprites with a colour key, fades, scrolling | + full-screen blends and, when the engines exist, affine layers and triangles |
| Target | **30 fps** | **30 fps**; **60 fps** with `fps = 60`, TE-synced (below) |

```c
typedef uint8_t sdk_tex;               /* a manifest texture id */
/* Fill a texture's PP-side copy from a file in the private folder
 * (RGB565, w x h as declared) or from the image's rodata, then upload
 * (DMA engine 2, FNV-1a checked, COMMIT) */
int  sdk_tex_load(struct sdk_ctx *c, sdk_tex t, const char *file);
int  sdk_tex_from(struct sdk_ctx *c, sdk_tex t, const sdk_px *src);
/* Draw into the PP-side copy on the CPU (text, a generated tile), then
 * sdk_tex_commit() uploads it */
int  sdk_tex_pixels(struct sdk_ctx *c, sdk_tex t, sdk_px **px, int *pitch);
int  sdk_tex_text(struct sdk_ctx *c, sdk_tex t, int x, int y, int str,
                  enum sdk_font f, sdk_px col);
int  sdk_tex_commit(struct sdk_ctx *c, sdk_tex t);

/* Frame commands: queued during step(), sent as one job batch after it */
void sdk_g_fill(struct sdk_ctx *c, int x, int y, int w, int h, sdk_px col);
void sdk_g_blit(struct sdk_ctx *c, sdk_tex src, int sx, int sy, int w,
                int h, int dx, int dy);
void sdk_g_sprite(struct sdk_ctx *c, sdk_tex src, int sx, int sy, int w,
                  int h, int dx, int dy, sdk_px key);   /* colour key */
void sdk_g_fade(struct sdk_ctx *c, int x, int y, int w, int h,
                sdk_tex a, int ax, int ay, sdk_tex b, int bx, int by,
                int weight);                            /* 1..256 */
void sdk_g_scroll(struct sdk_ctx *c, int x, int y, int w, int h, int dy);
/* the rect to show this frame; the SDK presents it on the panel's next
 * tearing-effect edge (J_PRES_TE) */
void sdk_g_dirty(struct sdk_ctx *c, int x, int y, int w, int h);
enum sdk_gfx { SDK_GFX_BCM, SDK_GFX_CPU };
enum sdk_gfx sdk_gfx_backend(struct sdk_ctx *c);
```

How the SDK runs a frame:
1. After `step()`, it sends the queued commands as jobs (`BLIT`, `FILL`, `FADE`, `SCROLL`; a sprite is a `BLIT` with `J_BLIT_KEY`) into the back frame, then `PRESENT` with `J_PRES_TE`, then one `FENCE`.
2. It waits on the fence at the start of the next step, never inside the game's slice.
3. **At most `SDK_G_CMDS_MAX` = 150 commands a frame** (the command ring holds about 200 drawing jobs, `jobs.md` §3). A full ring (`BCMJOB_BUSY`) drops that frame; it never queues on the PP5022.

**Textures are slots, and slots are a cache.** Every texture has its **PP-side copy** in the game's memory (the borrow for B, the session buffer for C), as the memory contract requires ("nothing on the BCM is the only copy", `memory-contract.md` §1). When the BCM2722's epoch changes (a power cut, a cutscene, a restart), the SDK re-uploads every texture from those copies before the next frame. The game never sees an epoch.

**The CPU path (contract 1: every BCM2722 job has one).** If the BCM2722 is off, marked slow or failed, or lacks a job (`J_OPS`), the SDK runs the same commands through the reference executor (`jref.c`, bit-exact with the chip) on the PP5022's framebuffer and pushes the dirty rect itself. The picture is identical; only the cost moves to the CPU. `sdk_gfx_backend()` returns `SDK_GFX_CPU`, the SDK lowers the frame target to 15 fps and logs `sdk gfx cpu why=`. A game may simplify its effects then. A Class C game whose scene needs the 150 MHz engines may return to the Games list from `init()` (`< 0`, logged).

**Layers [planned P19, `graphics.md` §6 engine 1].** The calls above cost one job each, so a screen of many sprites hits the 150-command cap. Layers send a whole scene in a few jobs:

```c
/* a tile layer: a map of tile indices over a texture of 16x16 tiles,
 * scrolled by (sx, sy); up to 2 layers, drawn back to front */
int sdk_layer_tiles(struct sdk_ctx *c, int layer, sdk_tex tiles,
                    const uint8_t *map, int map_w, int map_h,
                    int sx, int sy);
/* a sprite list: up to 64 sprites a frame, one job, colour-keyed */
struct sdk_sprite { sdk_tex t; uint8_t frame; int16_t x, y;
                    uint8_t w, h, flags; };
int sdk_sprites(struct sdk_ctx *c, const struct sdk_sprite *s, int n,
                sdk_px key);
```

Until the BCM2722 image has the layer jobs, the SDK expands them into `BLIT`s (the CPU path's executor does the same), so a game written against layers runs either way, more slowly.

**60 fps (Class C, `fps = 60`).** The step may return 16 ms, and every present waits for the panel's tearing-effect edge (`J_PRES_TE`). A full-frame push is 14.26 ms of panel-bus time [silicon S5–S7], so 60 fps full-screen uses about 86 % of the bus and is clean only with TE sync; #52 is answered [silicon, stage 3d 2026-10-26]: TE works on our unit and **the panel refreshes at 67.8 Hz** (14,740 µs), so a present on every edge is 67.8 fps and on every 2nd edge 33.9 fps (the synced 3D walk ran at 33.9 fps with 0 late frames). A full-frame push (14.26 ms) fills 97 % of one TE period, so every-edge pacing is clean only for partial rectangles. **`fps = 60` still runs at 30 fps** (logged `sdk fps 30 why=te`) until the SDK owner lifts that gate when the games SDK work resumes (the 1.0 finish line).

**The decided default for frame rates** (orchestrator, 2026-10-26; the owner may override): the SDK paces presents on the measured TE edges and never promises a nominal rate.
- `fps = 60`: **every edge (67.8 fps) only for a game that declares partial-rectangle presents** whose push fits inside one period (14,740 µs, less the push's own time); otherwise every 2nd edge.
- Full-frame games, and `fps = 30`: **every 2nd edge, 33.9 fps.**
- The SDK reports the real rate to the game (a `get_fps` / `frame_time_us` call, fixed-point, from the measured period), and a game's timing reads that, never a constant 60 or 30. A game should present only the rectangles that change (a rhythm game's note lanes, not the whole screen) so 60 fps stays far below the bus limit. On the CPU path a 60 fps game drops to 15 fps like any other.

**Later still:** an affine layer and triangles (Class C at 150 MHz; opcodes `J_OP_GLYPHS`, `J_OP_TRIS` reserved; `graphics.md` §6 steps 2–3).

### 5.4 Standard overlays and menus (every class) [planned W4-0]

A pause overlay, a "game over" panel or an options list should look like the system. The SDK draws them in the shell's look (the dimmed panel, list rows, the selection bar), on the CPU in Class A and as a texture strip in B/C:

```c
struct sdk_menu {
    uint8_t n;                  /* rows, <= 6 */
    const uint16_t *rows;       /* string ids (yours or SDK_STR_*) */
    uint8_t sel;                /* the row selected at open */
};
int  sdk_menu_open(struct sdk_ctx *c, const struct sdk_menu *m);
void sdk_menu_close(struct sdk_ctx *c);
bool sdk_menu_is_open(struct sdk_ctx *c);
```

While a menu is open, the **wheel and SELECT** drive it, and the picked row arrives in the next step's `in->menu_pick`. **MENU is not consumed by the menu:** in Class A it still calls `back()`, in B/C it is still the game's event. So the game decides what MENU does with its overlay open (close it, or leave the game).

**The Exit row** (owner, 2026-10-24: every game's pause menu ends with Exit). A row whose id is `SDK_STR_EXIT` belongs to the SDK: picking it runs the game's exit with `SDK_WHY_USER` and its exit policy, exactly as the exit chord does (a graceful game's `exit()` saves its record), and the game never sees that pick in `menu_pick`. Rows before it keep their indices. Solitaire, Bricks, Chute and Quadrus all end their pause menus with it.

### 5.5 Texture slots in the manifest (B/C)

`textures = bg:320x240 tiles:128x64 font:256x32` names each slot and its size. `modpak.py check` (planned P19) refuses a Class B set over `J_BUDGET_APP` (56 KB) or `J_SLOTS_APP` (8), and the runtime refuses an `sdk_tex_*` on an undeclared id. Class C's set is checked against the `J_CLS_GAME` pools.

Note the Class B budget: 56 KB is **one 160 × 175 RGB565 picture**. A Class B game is built from tiles and sprites, not full-screen pictures. A full-screen background is a `sdk_g_fill`, a row table, or Class C.

### 5.6 RAM

```c
/* Class A: the one working block, from the module arena, once, at init.
 * <= the manifest's work (minus the state record, section 8.2). NULL if
 * it doesn't fit: show a reduced view, never allocate elsewhere. */
void *sdk_work(struct sdk_ctx *c, size_t bytes);

/* Class B [planned #108 + P19]: a borrow from the audio buffer while
 * music plays, through #108's shrink-without-stop. Once, at init,
 * <= the manifest's borrow, granted only while the buffer stays above
 * its floor (>= 7.5 MB on 32 MB units, >= 39 MB on 64 MB units) plus
 * the borrow. NULL = refused: run the reduced mode, or return < 0 from
 * init (back to the Games list, logged). */
void *sdk_borrow(struct sdk_ctx *c, size_t bytes);

/* Class C [planned P14 + P19]: the audio buffer lent whole for the
 * session (music paused, its resume point kept) */
int sdk_session_mem(struct sdk_ctx *c, void **p, size_t *bytes);
```

Nothing else allocates: no `malloc`, no `core_alloc`, no buflib. All of it is reclaimed at exit, whatever the game left behind.

---

## 6. Budgets per class

### 6.1 The table

| Budget | Class A | Class B | Class C |
|---|---|---|---|
| Image | ≤ 32 KB | ≤ the arena less the index stage | same |
| Image + `.bss` + work | **≤ 56 KB** (the arena is sized to hold the largest game + 2 KB + 25 %: **72 KB** with games, brief §10; 29 KB today) | as A, plus the borrow | as A, plus the session buffer |
| Extra RAM | none | `borrow` (manifest), refusable | the audio buffer |
| BCM2722 | none | `J_CLS_APP` 56 KB / 8 slots, 27 MHz | `J_CLS_GAME`, 150 MHz |
| Minimum unit | 32 MB | 32 or 64 MB | 32 or 64 MB |
| **CPU slice per call** (game time only) | **20 ms** unboosted [est, tuned on the device] | **50 ms** | **50 ms** |
| `init()` slice | 100 ms | 250 ms | 500 ms |
| **Kill** | a repeat violation in the session, or any slice over **10 ×** its budget | same | same |
| Boost | bursts **≤ 100 ms**, **≤ 10 % boosted over any 10 s** (`ui_boost_burst`, [built]) | as declared: `burst`, or `session` (boosted while running, dropped while suspended) | session-long allowed |
| Frame rate | ≤ 30 fps | target 30 fps | target 30 fps; 60 fps with `fps = 60` and TE sync (#52) |
| Pixels per frame | ≤ 25 KB pushed on average; ≤ 30 pushes/s | ≤ 150 commands; presents only (no pixel pushes) | same |
| Storage at entry | **2 reads**: the image and the state record | bulk reads at `init`, sized to the borrow | its own streaming |
| Storage while running | **none** | none while music plays | per its session |
| Writes | the state record, at `suspend`/`exit` only | the same, and its own files at `suspend`/`exit` | per its session |
| Private storage | `quota`, default **64 KB**, ceiling **1 MB** | same | same, plus a read-only media folder outside the quota (§8.6) |
| Exit | graceful ≤ 1,000 ms, or immediate | same | same |
| Into the game | ≤ 250 ms to a live screen (inside the push animation; the snapshot covers a cold disk) | ≤ 0.5 s behind a snapshot | ≤ 1 s behind a snapshot |
| Back out | instant (a pop slide; nothing waits on a write) | ≤ 0.3 s | ≤ 1 s |
| Sound | the click only | the click only (rev 0; Q-SDK-2) | its own PCM and effects |

### 6.1a How the accountant counts (`modclass.c`) [built]

- **The game screen is the game's visit.** The SDK's screen is resident and owned by no module, so the accountant asks the SDK (`sdk_screen_mod()`) whose it is; before P12's integration it closed the visit at once and reported `fps=100` off the header push alone.
- **Frames, not pushes:** a frame's first band counts (`update sdk`); its later bands (`update sdk+`) and the push slide between screens (`anim:slide`) don't.
- **`pages = N`** (planned with the file API, Class A only, `N` in 1..30): page loads are counted apart from reads. A load over 2 storage commands is a violation; more than `N` loads in a second is a violation.

### 6.2 How time is counted

- **The slice clock counts only time inside game code.** It stops while an SDK call runs (a push, a file read, a job submit) and restarts when the SDK returns into the game. A slow disk never "kills" a well-behaved game.
- The first violation logs `sdk viol <name> <rule> <value>`. A repeat in the same session, or any slice over 10 × its budget, kills the game (§12.2).

---

## 7. Feedback, music and audio

### 7.1 The click (every class)

```c
/* The Clicker path: a headphone click plus the piezo (and the haptic mod,
 * #23). Rate-limited to 20 a second (extra clicks dropped); silent when
 * the Clicker setting is Off. Never the alarm pattern. */
void sdk_click(struct sdk_ctx *c);
```

Use it for events, not frames: a card placed, a brick hit, a row cleared.

### 7.2 Music controls

```c
/* Class B only (owner Q22): pause or resume the user's music. No skip,
 * no volume, no queue access. Class A/C: SDK_E_CLASS. */
int  sdk_music_pause(struct sdk_ctx *c);
int  sdk_music_resume(struct sdk_ctx *c);
bool sdk_music_playing(struct sdk_ctx *c);  /* every class, read-only */
```

- **Class A:** the music stays in the firmware's hands (PLAY, NEXT, PREV work as everywhere).
- **Class B:** the game may offer its own pause and resume (for example, items in its pause menu). If the game paused the music, the SDK leaves it paused at exit.
- **Class C:** paused by the firmware at entry and **left paused at its spot** at exit (Q23).

### 7.3 Class C audio [planned P14 + P19]

```c
/* The game's own sound through the SDK's channel (music is paused).
 * PCM is pushed, never pulled: there are no callbacks into a game. */
int  sdk_audio_open(struct sdk_ctx *c, unsigned rate);  /* 44100 */
int  sdk_audio_space(struct sdk_ctx *c);                /* frames free */
int  sdk_audio_write(struct sdk_ctx *c, const int16_t *stereo, int frames);
int  sdk_sfx_load(struct sdk_ctx *c, int slot, const char *file); /* <= 8 */
int  sdk_sfx_play(struct sdk_ctx *c, int slot, int gain);         /* 0..256 */
void sdk_audio_gain(struct sdk_ctx *c, int gain);       /* its own volume UI */
```

The SDK mixes the effects into its ring (in the session buffer) and feeds the WM8758 through the normal PCM path. An empty ring plays silence (counted, logged once). There are no threads, timers or interrupt-time callbacks into the game.

### 7.4 Library views (Class B; read-only) [planned #108 + P19]

```c
/* n random track ids from the library precompute (never a walk) */
int sdk_lib_pick(struct sdk_ctx *c, uint32_t *ids, int n, unsigned filter);
/* copies a field (title, artist, album) into buf */
int sdk_lib_meta(struct sdk_ctx *c, uint32_t id, int field, char *buf,
                 int len);
/* a pre-scaled cover from the art cache, copied into a texture */
int sdk_art_thumb(struct sdk_ctx *c, uint32_t id, sdk_tex t);
/* a clip through a borrowed playback session: statistics, resume points
 * and the queue untouched */
int sdk_preview_play(struct sdk_ctx *c, uint32_t id, uint32_t start_ms,
                     uint32_t ms);
```

**No file handles to music, ever.** Strings and pixels are copied out. After a session the library, play counts, ratings, resume points and the queue are byte-identical to before (Music Quiz's test, `games.md` §9). Music Quiz is the first client (Q16, Class B).

### 7.5 Class C songs through our codecs, with a sample-accurate position [planned P14 + P19]

A Class C game with `songs = yes` plays **its own** music files (from its media folder, §8.6) through the firmware's codecs. The user's library playback is paused by the handover as for every Class C game; the game's song uses the playback engine inside its session, so every format Rockbox decodes works, and the game never reads the audio bytes itself.

```c
/* open a song from the media folder (a relative path, section 8.6);
 * decoding starts and fills ahead; play starts paused */
int  sdk_song_open(struct sdk_ctx *c, const char *path);
int  sdk_song_play(struct sdk_ctx *c, uint32_t from_ms);
int  sdk_song_pause(struct sdk_ctx *c);
void sdk_song_close(struct sdk_ctx *c);
/* where the song is at the DAC: samples since sample 0 of the song, and
 * the microsecond time (the same clock as region stamps, 3.4) at which
 * that sample left the WM8758. Sample-accurate: read from the I2S DMA
 * position, corrected by the output latency measured on the device
 * (D-A1) and by the global offset (below). */
int  sdk_song_pos(struct sdk_ctx *c, uint64_t *sample, uint32_t *at_us,
                  uint32_t *rate);
/* the user's global audio/input offset, ms (positive: the sound is
 * heard later than the SDK's estimate) */
int  sdk_av_offset_ms(struct sdk_ctx *c);
```

- **Judging:** a hit's song time is `(region.at_us − at_us) + sample / rate`, both on one clock, so neither the game's step rate nor the BCM2722's present delay enters the judgement.
- **Effects** (`sdk_sfx_play`, §7.3) mix over the song in the SDK's channel; the song and the effects share one output, so one latency applies to both.
- **Codec placement:** the decode runs where library decoding runs (the COP since Fit-2 [measured, S36]); the CPU stays the game's. A format that needs a boost to decode in real time boosts the session (`boost = session`).
- **The song ends** with `SDK_INF_SONG_END` in the next step's flags.

**The global audio/input offset.** One value for the whole unit (not per game), in ms, −200..+200, default 0, kept by the firmware with the play counts (§8.5), not in any game's folder. A game shows it as a row of its Settings screen with the standard entry `SDK_SET_AV_OFFSET` (§9.1); changing it there changes it for every game. `sdk_av_calibrate(c)` pushes the SDK's calibration screen: the user touches the wheel in time with a steady click, and the SDK sets the offset from the median error over 16 beats. Headphones and a dock line-out differ, so the value is the user's to tune, not a constant.

---

## 8. Storage

### 8.1 The private folder

Each game sees **only its private folder**, `/.rockbox/gamedata/<module-id>/` (named here for developers; never on screen).
- Names are **bare**: `[A-Za-z0-9_.-]{1,31}`, with no `/`, no `..` and **no leading dot**. The SDK builds the path itself, so there is no absolute path, no traversal and no link to follow (FAT has none; the check is still made).
- **Names starting with a dot are the SDK's**: `.state` (§8.2). A game can't open, overwrite or delete them.
- Every write goes through **safefile's replace** (a temporary file, then renames), so a power cut leaves the old file or the new one, never a torn one.
- Writes are **refused at USB entry and below the disk-safe battery level** (`SDK_E_REFUSED`), and never wake a sleeping disk just for a save: the SDK queues the write to the next natural spin-up or to the exit, whichever comes first (alarms excepted, which isn't a game's concern).

### 8.2 The state record (every class)

One record per game, which the SDK keeps for it:

```c
/* The record read before init (len = 0: none yet, or it failed its CRC
 * and was reset). It lives in your working block (A) or borrow (B), at
 * most the manifest's `state` bytes; change it in place. */
void *sdk_state(struct sdk_ctx *c, uint16_t *len);
/* Its new length, and "write it at the next chance": the SDK writes it
 * at suspend/exit (never during step), with your settings (section 9),
 * as one file: /.rockbox/gamedata/<id>/.state */
int   sdk_state_commit(struct sdk_ctx *c, uint16_t len);
```

- **Format** (the SDK's): a 16-byte header (magic, the SDK's layout version, the game's `settings` version, the lengths, a CRC-32), the settings values (§9.3), then the game's record. One file, read in **one range read** before `init`: that is Class A's second (and last) entry read.
- **Keep the record consistent whenever a call returns.** The SDK may write it at any suspend, or at an immediate-policy exit, between two calls.
- **On a kill, the record is not written.** The file on disk stays at its last good version, so a game that crashes on a bad state can't save that state.
- Version your own record (a byte at its start) so a game update can migrate or reset it.

### 8.3 Files (B/C) [planned with B]

```c
enum { SDK_O_READ = 1, SDK_O_WRITE = 2 };   /* WRITE = replace on close */
int  sdk_file_open(struct sdk_ctx *c, const char *name, int mode);
int  sdk_file_read(struct sdk_ctx *c, int fh, void *buf, int n);
int  sdk_file_write(struct sdk_ctx *c, int fh, const void *buf, int n);
int  sdk_file_close(struct sdk_ctx *c, int fh);  /* WRITE: commits */
int  sdk_file_load(struct sdk_ctx *c, const char *name, void *buf, int max);
int  sdk_file_save(struct sdk_ctx *c, const char *name, const void *buf,
                   int n);
int  sdk_file_remove(struct sdk_ctx *c, const char *name);
int  sdk_file_list(struct sdk_ctx *c, int i, char name[32], uint32_t *bytes);
int  sdk_file_usage(struct sdk_ctx *c, uint32_t *used, uint32_t *quota);
```

- **At most 4 open handles**, all closed by the SDK at exit.
- Class B reads in bulk at `init` (whole files, sized to its borrow) and writes at `suspend`/`exit`; it never streams while music plays.
- **Class A uses the state record only.** `sdk_file_*` returns `SDK_E_CLASS` in Class A, because Class A's entry budget is two reads (the image and `.state`) and it may not touch storage while it runs (brief §2.1). This narrows brief §2A.3, which listed `sdk_file_*` for Class A; see Q-SDK-1.

### 8.4 The quota

`quota` (KB, default 64, ceiling 1,024; Q24) counts **every file in the folder, `.state` included**. A write that would pass it is refused (`SDK_E_QUOTA`) before anything is written. Typical figures: a Solitaire save is about 120 B, a high-score table about 64 B (`games.md` §4).

### 8.5 What the firmware keeps, not the game

- **The games file** (`/.rockbox/gamedata/.games`) holds one record per module id. It is read as min(file, `MOD__N`) records with the rest zeroed, so a firmware update that adds module ids keeps every game's counts [built].
- **The play count** ("Never Played", "Played Once", "Played 6 Times") is the firmware's. It's counted at each launch (G-8) and kept in the firmware's own games file, because the Games list shows it without loading any game. A game can't read or change it.
- **The kill count** (§12.2) is the firmware's too.
- **The global audio/input offset** (§7.5), one `int16_t` for the unit.
- `games.md` §4's shared `games.dat` keeps only these counters and the offset; each game's saves move into its own `.state` (brief W4-1..3).

### 8.6 The game media folder (Class C) [planned P14 + P19]

Some games need large data the user brings: the rhythm game's songs and charts are megabytes each. The 1 MB quota is the wrong tool for that, and a hidden folder is the wrong place for files the user copies by hand. The proposal:

- **`media = <Folder>`** in a Class C manifest gives the game a **visible, read-only media folder**: `/Games/<Folder>/` at the disk's root, like `/Notes` (brief Q12). The user drags song folders into it over USB; nothing else is needed. The firmware creates it (empty) the first time the game is installed.
- **Outside the quota.** The user's own files, managed like their music: the quota covers only the game's writable private folder. The media folder's size shows in the developer UI and is logged once per post-USB moment (`sdk media <name> files= bytes=`).
- **Read-only to the game**, through the SDK only:
  ```c
  /* paths relative to the media folder: up to 3 components, each a
   * bare name ([A-Za-z0-9 _.,'()&+-]{1,63}, no leading dot, no "..");
   * user folder names have spaces, so the media rule is wider than
   * section 8.1's */
  int sdk_media_list(struct sdk_ctx *c, const char *dir, int i,
                     char name[64], uint32_t *bytes, bool *is_dir);
  int sdk_media_open(struct sdk_ctx *c, const char *path);  /* read only */
  /* read/close: sdk_file_read / sdk_file_close on the handle */
  ```
  Text and pictures (charts, banners) are read with handles; **audio is opened only through `sdk_song_open()`** (§7.5), so the decode stays the firmware's and no audio bytes reach the game. The game can't write, rename or delete anything there.
- **Never part of the library.** The firmware's database scan and the sync-time indexers skip `/Games/` entirely, so a game's songs never appear in Music, never get play counts and never touch resume points.
- **Indexed at sync time, not at browse time** (rule: pre-compute at sync time). At the post-USB moment the firmware writes a listing of each media folder (names, sizes, modification times) to the game's private folder (`.media`, an SDK file, not counted in the quota). The game reads it at `init`, and parses only the charts that changed since its own cache (a file in its private folder, in its quota). It never walks the folder while a song plays.
- **Why not inside the hidden folder with a big quota:** the user could not find it in the Finder (the hidden attribute), and a quota large enough for songs would let any game fill the disk with writes. Read-only, user-owned media keeps both risks out.

Q-SDK-8 asks the owner to confirm the folder's place and name.

---

## 9. Per-game settings

Owner, 2026-10-23: a game offers **its own Settings screen** in a standard way, and its settings **persist in its private folder, within its quota**. Solitaire's "draw one or three cards" is the first one.

### 9.1 Declaring settings

```c
enum sdk_set_kind {
    SDK_SET_CHOICE,     /* one of n labelled values */
    SDK_SET_TOGGLE,     /* On / Off (SDK_STR_ON / SDK_STR_OFF) */
    SDK_SET_AV_OFFSET,  /* the unit-wide audio/input offset (7.5): its
                           row shows "+12 ms" and opens the calibration
                           screen; the value is the firmware's, not
                           stored in .state; label, n, def, values unused.
                           Class C with songs = yes only [planned] */
};
enum sdk_set_apply {
    SDK_APPLY_NOW,      /* the game picks it up at once */
    SDK_APPLY_NEXT,     /* from the next New Game; the current game keeps
                           its value (store it in your record) */
};
struct sdk_setting {
    uint16_t label;             /* string id: the row's label */
    uint8_t  kind;              /* enum sdk_set_kind */
    uint8_t  n;                 /* CHOICE: values (2..12); TOGGLE: 2 */
    uint8_t  def;               /* the default value's index */
    uint8_t  apply;             /* enum sdk_set_apply */
    uint8_t  off_if, off_val;   /* greyed out, and skipped by the cursor,
                                   while setting off_if - 1 holds value
                                   off_val; off_if 0: always on (Chess's
                                   Opponent: Player turns Level and Play
                                   As off). In what was padding: the
                                   struct keeps its 12 B (ABI 0.1) */
    const uint16_t *values;     /* CHOICE: n string ids; TOGGLE: NULL */
};
struct sdk_settings {
    uint8_t version;            /* bump when you change the table's meaning */
    uint8_t n;                  /* <= SDK_SETTINGS_MAX (16) */
    const struct sdk_setting *s;
};
```

The table is `const` (in the image), and `struct sdk_game.settings` points to it, so the SDK knows the settings **before `init`** and has their values ready.

### 9.2 Reading and showing them

```c
/* The current value's index of setting i (always valid: 0..n-1) */
int  sdk_setting(struct sdk_ctx *c, unsigned i);
/* Push the SDK's Settings screen for this game (from your pause menu's
 * "Settings" row). The game is suspended (SDK_WHY_SETTINGS) while it is
 * on top, and resumed after; the first step after it carries
 * SDK_INF_SETTINGS if anything changed. */
int  sdk_settings_open(struct sdk_ctx *c);
/* Did setting i change since the last call (clears the mark)? */
bool sdk_setting_changed(struct sdk_ctx *c, unsigned i);
```

**The screen** is the system's Settings look (the 7G idiom): a full-width list with its title "Settings" in the header, each row's label on the left and its value on the right.
- **SELECT on a row with 2 or 3 values** steps to the next value in place (as the stock Settings' Shuffle and Repeat rows do).
- **SELECT on a row with more values** pushes a list of the values, titled with the row's label; as the stock value lists (Settings › Backlight) there is no check mark: **the current value is the selected row on entry**. SELECT picks and goes back [built].
- **The first `step()` after the Settings screen carries `SDK_INF_SETTINGS`** when any value changed there [built]. `resume()` runs before that step, so a game may also re-read its settings there (Quadrus does).
- **MENU goes back**, in every class. The Settings screen is the SDK's own screen, so MENU there behaves as in the shell even in a B/C game.
- **A greyed-out row** (`off_if`, §9.1) draws its label and value in the list's disabled grey; the wheel skips it and SELECT does nothing there. Its value is kept, and `sdk_setting()` still returns it: the game decides what it means while the row is off [built].
- **The wheel scrolls** as in every list. The music keeps the class's rules (in Class A, PLAY/NEXT/PREV still control it).

### 9.3 Persistence

- Values are stored **one byte each, in the `.state` file's settings block** (§8.2), so settings cost no extra file, no extra read and only a few bytes of quota.
- They're written with the state record at the next suspend or exit, never from the Settings screen itself and never during `step()`.
- **Validated at load:** a value outside 0..n−1, or a stored table `version` different from the game's, resets that game's settings to their defaults (logged `sdk settings reset <name> why=`).
- A game that has no state record still gets its settings saved: the SDK writes a `.state` with an empty record.

### 9.4 Worked example: Solitaire's draw setting

Classic Klondike (the owner asks for "as classic a Solitaire feel as possible"): the deal, the stock, the waste, the four foundations and the seven tableau piles, with **draw one or three cards** from the stock.

```c
enum { SO_STR_NAME, SO_STR_DRAW, SO_STR_ONE_CARD, SO_STR_THREE_CARDS,
       SO_STR_UNDO, SO_STR__N };            /* solitaire.lang */
enum { SO_SET_DRAW };                       /* setting indices */
enum { SO_DRAW_ONE, SO_DRAW_THREE };        /* its values */

static const uint16_t so_draw_values[] = { SO_STR_ONE_CARD,
                                           SO_STR_THREE_CARDS };
static const struct sdk_setting so_set[] = {
    [SO_SET_DRAW] = { .label = SO_STR_DRAW, .kind = SDK_SET_CHOICE,
                      .n = 2, .def = SO_DRAW_THREE,  /* Q-SDK-3 */
                      .apply = SDK_APPLY_NEXT, .values = so_draw_values },
};
static const struct sdk_settings so_settings = { .version = 1, .n = 1,
                                                 .s = so_set };

struct so_save {                        /* the state record, ~120 B */
    uint8_t  ver;                       /* 1 */
    uint8_t  draw;                      /* this deal's draw: a change
                                           applies from the next deal */
    uint8_t  cards[52];                 /* the deal */
    uint8_t  pile_len[13];
    uint16_t moves;
    uint32_t elapsed_s;
    /* ... */
};

static struct so_save *sv;              /* in the working block */

static void so_new_deal(struct sdk_ctx *c)
{
    sv->draw = (uint8_t)sdk_setting(c, SO_SET_DRAW);
    /* shuffle with sdk_rand32(), deal ... */
    sdk_state_commit(c, sizeof *sv);
}

/* the pause overlay: Resume, New Game, Undo, Settings */
static const uint16_t so_pause_rows[] = { SDK_STR_RESUME, SDK_STR_NEW_GAME,
                                          SO_STR_UNDO, SDK_STR_SETTINGS };
static const struct sdk_menu so_pause = { 4, so_pause_rows, 0 };

/* in step(): */
    if (in->menu_pick == 3)
        sdk_settings_open(c);           /* Settings › Draw: Three Cards */
```

- The Settings screen shows one row: **Draw** with **Three Cards** on the right; SELECT steps it to **One Card**.
- The change applies from the **next deal**. The current deal keeps the draw it was dealt with (`sv->draw`), so a game in progress never changes its rules under the player (the classic behaviour; Q-SDK-3 lists the alternative).
- The value is saved in `.state` with the next suspend or exit: one byte.

---

## 10. Cutscene video [planned P14 + P19]

```c
#define SDK_VIDEO_SKIP  0x1     /* SELECT or MENU ends the clip early */
/* Play a pre-rendered clip from the private folder, full screen, through
 * the video player (P14). The game is suspended (SDK_WHY_VIDEO) and
 * resumed after; the first step after carries SDK_INF_VIDEO, and
 * sdk_video_result() says how it ended. */
int sdk_video_play(struct sdk_ctx *c, const char *file, unsigned flags);
int sdk_video_result(struct sdk_ctx *c);  /* SDK_OK, skipped, or an error */
```

- **Video is exclusive of games and of music.** The clip runs as a Class C-level session: the BCM2722 goes to `J_MODE_VIDEO` (the game's slots are evicted, and the SDK re-uploads them from the PP-side copies after the clip), and the music is paused.
- **Class C:** music is already paused.
- **Class B:** needs `cutscene = yes` in its manifest. Its borrow stays held; the handover pauses music and lends the buffer for the clip, then gives it back to playback. Whether music then resumes is Q-SDK-4.
- **Class A:** refused (`SDK_E_CLASS`).
- **A failure** (a bad file, a decoder error) ends the clip; the game gets the error and carries on (logged). Q-SDK-5 asks whether the video-failure rule (stop audio, styled error screen) should apply here instead.
- The clip's format is P14's: the video player's own container, prepared at sync time like every other video.

---

## 11. Enforcement

The PP5022's ARM7TDMI cores have **no MMU and no MPU**, so a game is native code that is **trusted but checked**. The layers are honest about their limits (brief §2A.6).

### 11.1 Link time: the allow list

An SDK module links **only against `sdk.sym`** (a versioned stub library of the SDK table), never against `rockbox.elf`. Any other undefined symbol fails the link. That makes the API surface in this document the **whole** surface.

What a game may link besides `sdk.sym`:
- **`libgcc`** (division, shifts, soft float), statically into its image;
- `memcpy`, `memset`, `memmove`, `memcmp`, `strlen`, `strcmp`: provided **by the SDK table** (the compiler emits calls to them on its own);
- `sdk_rand32(c)`, `sdk_now_ms(c)` for randomness and time; there is no libc `rand`/`time`.

Prefer fixed point: soft float works but costs image size and CPU.

**The table is append only within an ABI major:** an entry's index is its ABI. `tests/sdk/check_sym.py` (run by `scripts/test.sh`) fails when an entry of the frozen list `tests/sdk/sdk_sym.base` is removed, renamed or moved; new entries go at the end of `sdk.sym` and join the frozen list when reviewed [built].

**Modules and firmware are tied by the pack's build ID** [built]: `modpak.py` hashes the core's image and every module image (games included) into the build ID it writes into `modules.pak` and into the core. A pack from another build is refused at open (`mod pak refused rc=-3`, `MODPAK_EBUILD`; logged, never run), and every game is then absent from the Games list. Until games ship apart from the firmware (P20), this tie also covers the rev 0 string tables' firmware language ids (§4.3).

Shell modules (our Extras screens) differ: they link `--just-symbols` against the core and are held by **class deny lists** (`class_a.deny`, `class_b.deny`) [built].

### 11.2 Load time and runtime

- **Load time:** the loader checks the header and the ABI, and **every relocation's target**: inside the module's own image, or an entry of the SDK table. Nothing else. `.data`, IRAM and writes to code outside the image are refused by `module.lds` [built]. A refusal hides the game (`mod hide`).
- **Runtime, in every call:** the SDK checks its arguments (rectangles inside the screen, sources inside the image or the game's memory, handles in the table, sizes within the work block, the borrow and the quota), and charges the budgets (§6).

### 11.3 The watchdog [planned W4-0 prototype]

1. **At every SDK→game call** the SDK saves a restore point (r4–r11, sp, the return address and the CPSR's T bit, in the SDK's frame on the UI thread) and records `(module, entry, start time)`.
2. **On the tick** (Timer 1, every 10 ms), it compares the game time used (SDK calls excluded) with the kill limit.
3. **It unwinds only when both are true:** the interrupted thread **is the UI thread**, and the **interrupted PC is inside the module's image**. It then rewrites the saved return context so the IRQ returns into the SDK's unwind trampoline (ARM state, T bit cleared in the saved SPSR, since a Thumb game was interrupted in Thumb state), which restores the restore point and reclaims as for a kill.
4. **Otherwise** (the PC is in core code the game called, or another thread was running), it only **sets a kill flag**. Every SDK entry and return checks the flag. Core code is never unwound mid-call, so no core lock or buffer is left half-changed.
5. **Fallback:** if the flag is unanswered for 2 s with interrupts still on, the firmware makes a **controlled reboot that saves the resume point** (one write of the playback resume state while the drive is allowed, then `system_reboot()`), logged and counted as a kill. A game that masks interrupts and spins can't be stopped by software at all. The hardware reset (hold SELECT + MENU) remains, as today.
6. **P19:** when the front end (and so the games) moves to the COP, the COP needs its own watchdog check (the COP is woken by the CPU's tick today, so the check moves into its wake handler). P19's brief owns it.

P18's soft sleep never starts during a game call; the shell requests and runs it between calls.

### 11.4 What no layer stops

A **malicious** native module can still store to any address or register; ARM code can. The layers stop accidents and casual misuse. Therefore:
- **P12 ships only our own games**, built with the firmware into `modules.pak`.
- **At release (P20, Q25):** third-party games install only if **signed** by a key the unit trusts (an allow list the owner controls; checked once at install or the post-USB moment, never at each load). A developer setting accepts unsigned games, with a warning in our own words.
- **[hypothesis]** the PP5022's `MMAPn` windows have permission bits; if a write through a window without the write bit is dropped on silicon (device spike D-S1), the firmware's code could be mapped read-only **through that window**. That catches stray stores through the main mapping only, never stores through the uncached alias. ARM user mode would stop a game masking interrupts, at the cost of a mode switch per SDK call; costed later, not in rev 0.

### 11.5 What a game may not touch

- Threads, tick tasks, timers, interrupts, callbacks that outlive a call.
- The COP and the BCM2722 outside the B/C API; `cpu_boost` outside the SDK's bursts and sessions.
- Any file outside its folder: the database and library, music files, art caches, playlists, settings, resume and statistics files, ProkBox's state files.
- The PMU, the RTC (time only through `sdk_now_ms()`), the backlight and LCD power, USB, the drive's power.
- Other modules; the wheel driver outside `sdk_wheel_mode()`; the reserved inputs (§3.1).

---

## 12. Errors

### 12.1 Return codes

```c
enum sdk_err {
    SDK_OK        =   0,
    SDK_E_ARG     =  -1,  /* a bad argument (also counted, section 12.2) */
    SDK_E_CLASS   =  -2,  /* not available in this class */
    SDK_E_PHASE   =  -3,  /* not in this call (e.g. a write in step) */
    SDK_E_QUOTA   =  -4,
    SDK_E_NOMEM   =  -5,  /* work/borrow exhausted, borrow refused */
    SDK_E_REFUSED =  -6,  /* USB entry, battery, disk-safe, power */
    SDK_E_NOENT   =  -7,
    SDK_E_IO      =  -8,
    SDK_E_BUSY    =  -9,  /* command ring full: this frame dropped */
    SDK_E_UNAVAIL = -10,  /* a planned part this firmware lacks */
};
/* your own trace line, "sdk <name> ...", <= 100 characters, at most
 * 10 lines a second (more are dropped); into ipodui.log */
void sdk_log(struct sdk_ctx *c, const char *fmt, ...);
```

### 12.2 What the firmware does

| Event | What the user sees | What is logged |
|---|---|---|
| A game can't run on this unit (ABI, RAM, class, header) | no row | `mod hide <name> why=` |
| The module fails to load | the Games list again, the row's subtitle unchanged | `mod load fail` |
| `init()` returns < 0 | the Games list (pop slide) | `sdk init refused <name> rc=` |
| A bad argument | the call returns `SDK_E_ARG` and does nothing | `sdk viol <name> arg <call>` |
| A budget passed once | nothing | `sdk viol <name> <rule> <value>` |
| A repeat, or 10 × a slice | the game is **killed**: immediate reclaim, the Games list with a pop slide, its `.state` not written | `sdk kill <name> <rule> us=` |
| Killed 3 times in a row | the row is hidden until the next firmware or module update (Q20) | `sdk hide <name> kills=3` |
| The BCM2722 fails mid-game | nothing: the SDK finishes on the CPU path (§5.3) | `sdk gfx cpu why=` |
| A Class B borrow refused | the game's reduced mode, or the Games list | `sdk borrow refused <name> want=` |

**Never a splash, a panic screen or a Rockbox string** (native illusion). The only full-screen notices the user can meet in a game are the firmware's own (an alarm ring, the USB screen, low battery).

---

## 13. Our games

| Game | Class | Wheel | State | Settings | Phase |
|---|---|---|---|---|---|
| **Solitaire** (Klondike rules) | A | relative: a cursor | the deal, ~120 B | **Draw: One Card / Three Cards** | P12 |
| **Bricks** (brick-breaker) | A | relative (paddle 6 px a step, the shell's acceleration); position if Q30 | best scores, the game in progress | **Speed** Slow/Normal/Fast, **Balls** 3/5 | P12 |
| **Chute** (catch the jumpers, in the style of a handheld LCD game) | A | relative (the boat a lane a step) | best scores, the game in progress | **Speed** Slow/Normal/Fast | P12 |
| **Quadrus** (falling blocks; owner's name, 2026-10-23) | A in P12; **B** after P19 (§13.2) | relative: move | best scores, the game in progress | **Start Level 1–10**, **Ghost Piece On/Off** | P12 (A), P19 (B) |
| Music Quiz | B (Q16) | relative | best score | later | #108 + P19 |
| **WildWheel** (step-chart rhythm game, our working name; long term) | **C** | **regions** (§3.4) | scores per chart, its chart cache | **Regions: Corners / Directions**, **Audio Offset** (global), scroll speed | after P14 + P19; idea only (§13.4) |

Their manifests, for example Solitaire's:

```
# Solitaire: Klondike rules, draw one or three
class   = A
min_ram = 32
kind    = sdk
abi     = 0.1
exit    = graceful 200
work    = 12288
state   = 256
quota   = 64
boost   = burst
strings = solitaire.lang
exports = solitaire
```

### 13.1 Solitaire (Class A, event-driven)

- **Rules:** classic Klondike, as plain and familiar as we can make it: 7 tableau piles, the stock and waste top-left, 4 foundations top-right; draw one or three (the setting); unlimited passes through the stock.
- **Input (Class A):** the wheel moves a cursor along two shelves (the top row of piles, then the tableau, in reading order; `games.md` §5.1); **SELECT** picks up the run under the cursor, and SELECT again drops the part of it that fits there; a **long SELECT** on a card sends it to a foundation if it can go; **short MENU** (`back()`): holding a run → put it back; the pause overlay open → `false` (leave the game, the deal kept in `.state`); otherwise → open the pause overlay (**Resume, New Game, Undo, Settings**). PLAY, NEXT and PREV control the music as everywhere.
- **Loop:** `SDK_IDLE` while nothing moves; 33 ms while a card glides or the win cascades (the one long boost, ≤ 2 s, as bursts).
- **Saved:** the deal on every suspend and exit; "Resume" is the first row when a deal is in progress.

### 13.2 Quadrus: the falling-block game (worked example 2: real time)

**Our own name and art.** "Quadrus" is the owner's name for it (2026-10-23, Q-SDK-6; the working name was "Stacks"). It never uses "Tetris", its logo, or its trade dress. Pieces are the generic four-square shapes of the genre, drawn in our gel materials with our own bevel; the colour scheme and the board are ours. We deliberately use a **12 × 18 well** of 11 px cells (132 × 198 px), not the familiar 10 × 20 well, and a release-time legal look at the whole presentation is on the P20 list (Q-SDK-7).

**Which class, and why.** Drawing doesn't need the BCM2722: a move redraws the band of rows the piece spans (at most about 50 rows, a full-width band of ≤ 32 KB), and gravity moves it at most 20 rows a second. That's about 21 KB a frame averaged at 30 fps [est], inside Class A's 25 KB. **Input is what decides the class.** A falling-block game wants five actions: move left, move right, rotate, soft drop and hard drop, plus an instant pause.

- **In Class A** the game owns only **SELECT and the relative wheel**. PLAY, NEXT, PREV and short MENU are the firmware's. One button must then carry both rotate and drop:
  - wheel = move (one column per wheel step quantum, tuned on the device, no acceleration);
  - **SELECT tap = rotate clockwise, at release** (a tap is ~100 ms, so rotation lags by that much);
  - **SELECT held 500 ms (`SDK_IN_LONG`) = hard drop**; no soft drop; gravity does the rest;
  - **short MENU = pause** (`back()` opens the overlay), acting at release as everywhere in Class A.

  That is playable for a relaxed game, but rotation lags by the tap, drop is deliberate rather than quick, and PLAY pauses the music, not the game.
  - **As built (P12 W4, `apps/ipodui/games/quadrus/`), first version:** exactly this scheme, wheel clockwise = right, one column per delivered step (the shell's wheel acceleration still applies, so a fast turn crosses the well; walls stop it). A **fast wheel flick as a second hard drop was considered and left out**: a flick is the same gesture as a fast slide to a wall, which players make all the time, so it would drop pieces they meant to park. Re-entering a game in progress, and every return from a suspend (Hold, the Settings screen, an alarm), opens the pause menu first, so nothing falls while the player isn't looking. The well redraws only the rows whose cells changed (diffed against what is on screen), as full-width bands, under a row allowance of 1,000 rows a second (~640 KB/s, below the 768 KB/s cap) that drops a frame rather than go over.
  - **The owner's controls (2026-10-24, after playing it on the device), as built:**

    | Input | Action | Timing |
    |---|---|---|
    | **Wheel** | a column per delivered step, clockwise right | unchanged (the shell's accelerated steps) |
    | **SELECT press** | rotate clockwise **at once**, on the press | no added delay |
    | **SELECT double tap** | a second press within **300 ms** of the first one's release: the first press's turn is taken back (through the same kick), then the **hard drop** (2 points a row) | `TAP_DBL_MS` |
    | **SELECT held** | past **250 ms** the press's turn is taken back and the **soft drop** runs until release: gravity's step / 10, never faster than 33 ms a row (level 1: 100 ms a row, 10×; level 10 and up: 33 ms, one row a frame), **1 point a row** | `HOLD_SOFT_MS`, `QD_SOFT_DIV`, `QD_SOFT_MIN_MS` |
    | **MENU tap** | **hold**: an empty slot takes the piece and the next one comes in; a full one swaps (the piece comes back at the top, unturned); once a piece, until it locks. The Hold box sits beside Next | `back()` at release |
    | **Long MENU** | the pause menu (Resume, New Game, Settings, Exit) | `SDK_LONGMENU_PAUSE` (§3.2a) |
    | **MENU, menu open** | leave (the game kept), as before | |

    The cost of rotate-on-press: two quick turns (within 300 ms) are a double tap, so a half turn needs two taps a little apart. The rules (hold, the soft drop, the tap and its undo) are in `quadrus_core.c`, host-tested in `tests/quadrus`; the hold slot, the flags and the undo sit in the state record's old padding, so it stays 284 bytes and a game saved before reads as "nothing held". Scripts: `ipodui-p12-quadrus-{hold,softdrop,dbltap,pause-longmenu}`.
- **In Class B** the game owns every button except the reserved holds. The scheme that fits the wheel's layout (MENU top, PLAY bottom, PREV left, NEXT right):

  | Input | Action | Why |
  |---|---|---|
  | **Wheel** (relative) | move the piece a column per step quantum | the most precise and fastest control on the ring |
  | **SELECT press** | rotate clockwise, **on the press** | the centre button, under the thumb; no lag |
  | **PLAY tap** (bottom) | hard drop | "down" is the bottom of the wheel; at release is fine for a deliberate drop |
  | **NEXT held** (right) | soft drop while held (press/release edges) | the thumb rolls to it from the wheel |
  | **MENU press** (top) | pause at once (the overlay) | instant in Class B |
  | **PREV** (left) | **unused** in play | SELECT + PREV is the exit chord: a fast rotate-and-PREV overlap would be swallowed (§3.1) |

- **Verdict:** **Class B is the right home**, because of input, not drawing: on-press rotation, a real drop and an instant pause. Class B with `borrow = 0` needs no audio-buffer borrow, so it doesn't wait on #108, only on **P19** (Class B draws only through the BCM2722 API, at 27 MHz; its few block tiles fit `J_CLS_APP`). Until then we ship it **as Class A in P12** with the scheme above (the default of Q-SDK-6). The rules, the state record and the settings are the same in both, so the P19 move is a new manifest, the B input map and the scene calls, and the user's scores and settings carry over.

**Settings** (the same pattern as Solitaire):

```c
enum { ST_SET_LEVEL, ST_SET_GHOST };
static const uint16_t st_levels[10] = { ST_STR_L1, /* ... */ ST_STR_L10 };
static const struct sdk_setting st_set[] = {
    [ST_SET_LEVEL] = { .label = ST_STR_START_LEVEL, .kind = SDK_SET_CHOICE,
                       .n = 10, .def = 0, .apply = SDK_APPLY_NEXT,
                       .values = st_levels },   /* 10 values: a sub-list */
    [ST_SET_GHOST] = { .label = ST_STR_GHOST, .kind = SDK_SET_TOGGLE,
                       .n = 2, .def = 1 /* On */, .apply = SDK_APPLY_NOW },
};
static const struct sdk_settings st_settings = { .version = 1, .n = 2,
                                                 .s = st_set };
```

- **Start Level** takes effect at the next New Game. **Ghost Piece** (the outline of where the piece will land) takes effect at once: the first step after the Settings screen sees `SDK_INF_SETTINGS` and redraws the well.
- **Loop:** 33 ms steps while a piece falls; gravity by elapsed time (level 1 = 1 row/s, up to 20 rows/s); `SDK_IDLE` while paused. Unboosted; a burst only for a multi-row clear (≤ 100 ms).
- **Click:** on a lock and a row clear (rate-limited).
- **State:** the best scores (top 5, ~40 B) and the game in progress (the well, 12 × 18 cells at 4 bits = 108 B, the piece, the next piece, the score, the level), so a game survives a suspend, a soft sleep and an exit.

### 13.3 Bricks and Chute (Class A, real time)

Both run 30 fps loops on the relative wheel; short MENU pauses (`back()`); PLAY keeps the music's play/pause (`games.md` §2's "PLAY pauses the game" doesn't apply in Class A). Like Quadrus, both reopen their pause menu after a re-entry or any suspend, and keep the game in progress and the five best scores in their state record.

- **Bricks** (as built, P12 W4, `apps/ipodui/games/bricks/`): the paddle moves 6 px per delivered wheel step; the shell's wheel acceleration already makes a slow turn precise and a fast one cross the field, so the game doesn't scale by `velocity` again. SELECT launches. The ball moves in sub-steps of at most 2 px, one axis at a time, so it bounces off the side of a brick it meets from the side; the paddle has five zones. A frame redraws the ball's old and new rows, the paddle's rows when it moves, a broken brick's rows and the score line: 15-25 rows typically (10-16 KB).
- **Chute** (as built, P12 W4, `apps/ipodui/games/chute/`; owner brief: a Game & Watch-style arcade game with our own theme; named Parachute, then Airdrop, until the owner renamed it, 2026-10-24, look unchanged, module id 14 kept): a catch game rather than the turret shooter first sketched in `games.md` §5.3. A plane crosses the top of a pale LCD; jumpers step down three lanes on one beat; the wheel rows a boat a lane per step to catch them before the lake. Every figure position is printed faintly ("unlit"), the live ones dark, as on an LCD handheld. A catch scores 1; a miss is a splash and a miss mark; the third miss ends the game, and every 50 points forgives one. The beat quickens 2 % per 10 points. At most one jumper is on each step, so two never land on the same beat. A beat redraws only the figures that changed (about 100 rows at most).
- Q30 is answered (§3.3): Bricks can read `sdk_wheel_pos()` for an absolute paddle; Chute stays relative (three lanes need no more).

### 13.5 Chess (Class A, event-driven, a sliced engine) [built, #132]

You against an engine of our own at three levels (Easy, Medium, Hard: search depth 2/4/6, a time cap of 1.5/4/10 s, a little root noise at Easy), from either side (Settings: Level, applied at once; Play As, from the next game). `apps/ipodui/games/chess/`; the design, the state-of-the-art choices (0x88, tapered tables of our own, a resumable alpha-beta) and the checklist are in `games.md` §5.6.

- **Input:** the ring as a four-way pad through `sdk_wheel_pos()` (a tap one square, a still finger repeating; §3.3); the wheel's raw clicks (`SDK_WHEEL_REL_RAW`) step the cursor through the squares that matter; SELECT picks and places (a promotion opens a Queen/Rook/Bishop/Knight menu, MENU there keeps the pawn); a **MENU tap** puts a picked piece back, else takes your last move and the reply back (while the engine thinks: stops it and takes back the move it was answering); a **long MENU** (`SDK_LONGMENU_PAUSE`) opens Resume, New Game, Settings, Exit.
- **Loop:** `SDK_IDLE` on your turn (the SDK's watcher wakes it for the ring); 33 ms steps while the engine thinks, each running the search for at most 10 ms of game time; `SDK_IDLE` while dark (`SDK_INF_DARK`). Unboosted.
- **State:** the move list (up to 1,020 plies, 2,048 B record), committed after every move and written by the SDK at suspend/exit; resume replays it in slices.
- **Class A, by necessity:** Class B is hidden until #108; the transposition table is sized to the arena (`games.md` §5.6).

### 13.4 WildWheel: a step-chart rhythm game (the Class C example; long term)

**An owner idea, filed on the tracker; not built.** It is here so the SDK is shaped to support it; every part it needs is marked planned above.

**What it is:** notes scroll toward a target line in four lanes, in time with a song; the player touches the matching region of the click wheel as each note arrives, and is judged on timing. It plays charts in the **StepMania formats (`.sm`, `.ssc`)** with their songs, so players can bring the large existing body of community charts.

**Ours, not theirs:** our own name ("WildWheel" is a working name, Q-SDK-9), our own art, lanes and judgement words, and **our own chart parser** written from the formats' public descriptions (no StepMania code, no StepMania or DDR names, logos or look). The formats are plain text: header tags (`#TITLE`, `#MUSIC`, `#OFFSET`, `#BPMS`, `#STOPS`, and in `.ssc` per-chart blocks), then measures of note rows. The parser reads only what the game uses: the song file, the offset, BPM changes, stops, and the four-panel ("dance-single") charts; it skips other chart types and ignores what it doesn't know. It runs once per changed chart and caches the result (a compact note list in the game's private folder), never during a song.

**Why Class C:** its own music (library playback paused, the game's songs through our codecs), 60 fps scrolling on the BCM2722 at 150 MHz, every button and the absolute wheel, and megabytes of user-supplied media.

**What it uses from the SDK:**

| Need | SDK part | Section |
|---|---|---|
| Touches of 4 wheel regions, with lift, timestamped at packet arrival | `sdk_wheel_regions()`, `struct sdk_region_ev` | §3.4 |
| A setting for **corners or cardinal directions** | its Settings table (`SDK_SET_CHOICE`) passed to `sdk_wheel_regions()` | §9 |
| Its song through our codecs, judged against a **sample-accurate** position | `sdk_song_open/play`, `sdk_song_pos()` | §7.5 |
| The **global audio/input offset**, and calibration | `SDK_SET_AV_OFFSET` row, `sdk_av_calibrate()` | §7.5, §9.1 |
| A hit sound over the song | `sdk_sfx_play()` | §7.3 |
| 30–60 fps scrolling: note sprites over lane tiles, TE-synced | `sdk_layer_tiles()`, `sdk_sprites()`, `fps = 60` | §5.3 |
| Songs and charts copied over USB, large | `media = WildWheel` → `/Games/WildWheel/`, read-only, outside the quota | §8.6 |
| Scores and its chart cache | `.state` and private files, `quota` up to 1 MB | §8 |

**Input design:**
- **Corners** (default) put each region between two buttons, so a thumb resting on a region never presses a button by accident; **Directions** put them on the buttons' marks (up = MENU's side), which reads naturally against arrow-shaped notes. A `dead` band of 3 positions at each border (§3.4) stops a finger on a border from flickering between lanes.
- **A note is hit by a TOUCH or SLIDE into its region** within the timing window around the note's time. Holds ("freeze" notes) are kept while the finger stays in the region; a LIFT ends them.
- **Jumps** (two arrows at once) can't be played on a one-position wheel. The parser turns each jump into a single note on the first of its lanes and marks the chart "adapted"; Q-SDK-9 lists the alternative (accept either lane).
- **Buttons:** MENU pauses (instant in Class C); SELECT confirms in menus; the exit chord and the reserved holds stay the firmware's.
- **Timing windows** are set only after D-W1/D-W2: if the wheel sends a packet every *t* ms while touched, no window can be tighter than about *t*. The game shows its judgement windows as a few named grades, our own words.

**Budgets:** 60 fps of four scrolling lanes is about 4 × 48 × 240 px of change a frame (≈ 92 KB of panel transfer a frame, about half of the panel bus at 60 fps, against 86 % for full frames, §5.3); its textures (lane tiles, note sprites, judgement words) fit a few hundred KB of `J_CLS_GAME`. The song streams from the media folder under the session's own drive policy, as video does.

**Device checks it depends on** (§15): **D-W1** (packet rate, lift events, jitter at rest), **D-W2** (touch-to-event latency), **D-A1** (audio output latency), and **#52** (the TE signal) for 60 fps.

---

## 14. "Hello game" per class

### 14.1 Class A: a counter

```c
/* hello_a: the wheel changes a number, SELECT resets it; it is kept */
#include "sdk.h"

enum { HA_STR_NAME, HA_STR__N };        /* hello_a.lang: "Hello" */
struct ha_rec { uint8_t ver; int32_t n; };
static struct ha_rec *r;

static void ha_draw(struct sdk_ctx *c)
{
    int x, y, w, h;
    sdk_area(c, &x, &y, &w, &h);
    sdk_fill(c, x, y + h / 2 - 12, w, 24, SDK_RGB(255, 255, 255));
    sdk_text_num(c, x + w / 2 - 10, y + h / 2 - 8, r->n, SDK_FONT_LIST,
                 SDK_RGB(0, 0, 0));
    sdk_dirty(c, y + h / 2 - 12, 24);
}

static int ha_init(struct sdk_ctx *c)
{
    uint16_t len;
    r = sdk_state(c, &len);             /* in the working block */
    if (len != sizeof *r || r->ver != 1)
        *r = (struct ha_rec){ .ver = 1, .n = 0 };
    int x, y, w, h;
    sdk_area(c, &x, &y, &w, &h);
    sdk_material(c, SDK_MAT_METAL, x, y, w, h);
    sdk_dirty(c, y, h);
    ha_draw(c);
    return SDK_OK;
}

static int ha_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    (void)now_ms;
    int before = r->n;
    r->n += in->steps;
    for (int i = 0; i < in->n; i++)
        if (in->ev[i].btn == SDK_BTN_SELECT &&
            in->ev[i].kind == SDK_IN_RELEASE)
            r->n = 0;                   /* a CANCEL never follows a
                                           release we acted on */
    if (r->n != before) {
        sdk_click(c);
        sdk_state_commit(c, sizeof *r);
        ha_draw(c);
    }
    return SDK_IDLE;                    /* wait for input */
}

static bool ha_back(struct sdk_ctx *c) { (void)c; return false; }
static void ha_suspend(struct sdk_ctx *c, enum sdk_why w) { (void)c; (void)w; }
static void ha_resume(struct sdk_ctx *c) { ha_init(c); }

SDK_DESC_A(hello_a, /* work */ 1, /* state */ 64, /* quota KB */ 64,
           MOD_EXIT_IMMEDIATE, 0);
SDK_GAME(hello_a) = { .init = ha_init, .step = ha_step, .back = ha_back,
                      .suspend = ha_suspend, .resume = ha_resume };
```

```
# hello_a.mod
class   = A
min_ram = 32
kind    = sdk
abi     = 0.1
exit    = immediate
work    = 1024
state   = 64
quota   = 64
boost   = burst
strings = hello_a.lang
exports = hello_a
```

Note: when SELECT is pressed and then joins the exit chord, the game gets `SDK_IN_CANCEL` for it. Acting on the **release** (as here) means a cancelled press never acted. A game that acts on the press must undo on `SDK_IN_CANCEL`.

### 14.2 Class B: a paddle on the BCM2722 [planned P19]

```c
/* hello_b: a block follows the finger; MENU opens Resume / Pause Music
 * / Exit */
#include "sdk.h"

enum { HB_TEX_BLOCK };                  /* textures = block:32x8 */
enum { HB_STR_NAME, HB_STR_PAUSE_MUSIC, HB_STR__N };
static int bx = 144, old_bx = 144;
static const uint16_t hb_rows[] = { SDK_STR_RESUME, HB_STR_PAUSE_MUSIC,
                                    SDK_STR_EXIT };
static const struct sdk_menu hb_menu = { 3, hb_rows, 0 };

static int hb_init(struct sdk_ctx *c)
{
    sdk_wheel_mode(c, SDK_WHEEL_ABS);
    sdk_header(c, false);               /* the full 320 x 240 */
    static const sdk_px block[32 * 8] = { /* ... our art ... */ };
    if (sdk_tex_from(c, HB_TEX_BLOCK, block) < 0)
        return -1;
    sdk_g_fill(c, 0, 0, 320, 240, SDK_RGB(0, 0, 0));
    sdk_g_dirty(c, 0, 0, 320, 240);
    return SDK_OK;
}

static int hb_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    (void)now_ms;
    for (int i = 0; i < in->n; i++)
        if (in->ev[i].btn == SDK_BTN_MENU && in->ev[i].kind == SDK_IN_PRESS)
            sdk_menu_is_open(c) ? sdk_menu_close(c)
                                : (void)sdk_menu_open(c, &hb_menu);
    switch (in->menu_pick) {
    case 1: sdk_music_playing(c) ? sdk_music_pause(c)
                                 : sdk_music_resume(c); break;
    case 2: sdk_exit_request(c); break;  /* the same path as the chord */
    }
    if (in->touched)
        bx += in->dpos * 3;             /* a turn = 288 px */
    bx = bx < 0 ? 0 : bx > 288 ? 288 : bx;
    if (bx != old_bx) {
        sdk_g_fill(c, old_bx, 220, 32, 8, SDK_RGB(0, 0, 0));
        sdk_g_blit(c, HB_TEX_BLOCK, 0, 0, 32, 8, bx, 220);
        int lo = bx < old_bx ? bx : old_bx;
        sdk_g_dirty(c, lo, 220, 32 + (bx > old_bx ? bx - old_bx
                                                  : old_bx - bx), 8);
        old_bx = bx;
    }
    return 33;                          /* 30 fps */
}
```

```
# hello_b.mod
class    = B
min_ram  = 32
kind     = sdk
abi      = 0.1
exit     = immediate
borrow   = 0
textures = block:32x8
quota    = 64
boost    = burst
strings  = hello_b.lang
exports  = hello_b
```

### 14.3 Class C: a scrolling field with its own sound [planned P14 + P19]

The smallest Class C game; §14.4 sketches the rhythm game's core on the same calls.

```c
/* hello_c: a full-screen tile field scrolls; SELECT plays an effect;
 * a cutscene at start */
enum { HC_TEX_TILES };                  /* textures = tiles:320x64 */
static int16_t *pcm; static int scroll_y;

static int hc_init(struct sdk_ctx *c)
{
    void *mem; size_t bytes;
    if (sdk_session_mem(c, &mem, &bytes) < 0)
        return -1;
    sdk_header(c, false);
    if (sdk_tex_load(c, HC_TEX_TILES, "tiles.565") < 0 ||
        sdk_sfx_load(c, 0, "zap.pcm") < 0 ||
        sdk_audio_open(c, 44100) < 0)
        return -1;
    sdk_video_play(c, "intro.vid", SDK_VIDEO_SKIP);
    return SDK_OK;
}

static int hc_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    scroll_y = (int)(now_ms / 16) % 64;
    for (int row = -1; row < 4; row++)
        sdk_g_blit(c, HC_TEX_TILES, 0, 0, 320, 64, 0, row * 64 + scroll_y);
    sdk_g_dirty(c, 0, 0, 320, 240);
    for (int i = 0; i < in->n; i++)
        if (in->ev[i].btn == SDK_BTN_SELECT && in->ev[i].kind == SDK_IN_PRESS)
            sdk_sfx_play(c, 0, 256);
    return 33;
}
```

```
# hello_c.mod
class    = C
min_ram  = 32
kind     = sdk
abi      = 0.1
exit     = graceful 100
textures = tiles:320x64
quota    = 1024
boost    = session
strings  = hello_c.lang
exports  = hello_c
```

### 14.4 Class C: the rhythm game's judging core (sketch) [planned]

```c
/* WildWheel's core: judge region touches against the song's position */
enum { BW_SET_REGIONS, BW_SET_SPEED, BW_SET_OFFSET };
enum { BW_CORNERS, BW_DIRECTIONS };
static const uint16_t bw_region_values[] = { BW_STR_CORNERS,
                                             BW_STR_DIRECTIONS };
static const struct sdk_setting bw_set[] = {
    [BW_SET_REGIONS] = { .label = BW_STR_REGIONS, .kind = SDK_SET_CHOICE,
                         .n = 2, .def = BW_CORNERS, .apply = SDK_APPLY_NOW,
                         .values = bw_region_values },
    [BW_SET_SPEED]   = { /* scroll speed, a CHOICE of 1x..4x */ },
    [BW_SET_OFFSET]  = { .kind = SDK_SET_AV_OFFSET },   /* unit-wide */
};

static void bw_apply_regions(struct sdk_ctx *c)
{
    sdk_wheel_regions(c, sdk_setting(c, BW_SET_REGIONS) == BW_CORNERS
                         ? SDK_RG_CORNERS : SDK_RG_CARDINAL, 3);
}

static int bw_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    uint64_t smp; uint32_t at_us, rate;
    (void)now_ms;
    if (in->flags & SDK_INF_SETTINGS)
        bw_apply_regions(c);
    sdk_song_pos(c, &smp, &at_us, &rate);   /* offset already applied */
    for (int i = 0; i < in->rg_n; i++) {
        const struct sdk_region_ev *e = &in->rg[i];
        if (e->kind == SDK_RG_LIFT) {
            bw_hold_end(e->region, e->at_us);
            continue;
        }
        /* the touch's song time, in microseconds */
        int64_t t_us = (int64_t)(smp * 1000000u / rate)
                     + (int32_t)(e->at_us - at_us);
        bw_judge(c, e->region, t_us);   /* nearest note in that lane;
                                           sdk_sfx_play + a grade sprite */
    }
    bw_draw_lanes(c, smp, rate);        /* sdk_layer_tiles + sdk_sprites,
                                           sdk_g_dirty of the lanes only */
    return (in->flags & SDK_INF_SONG_END) ? bw_results(c) : 16;
}
```

```
# beatwheel.mod (long term)
class    = C
min_ram  = 32
kind     = sdk
abi      = 0.1
exit     = graceful 300
fps      = 60
songs    = yes
media    = WildWheel
textures = lanes:192x64 notes:128x128 grades:256x64
quota    = 1024
boost    = session
strings  = beatwheel.lang
exports  = beatwheel
```

---

## 15. Testing a game

- **In the simulator** (modules link statically): `tests/ui/ipodui-p12-games-*` scripts drive each game. Every script that opens a game must log at least one `modclass` line and no `modclass viol`/`sdk viol` line, assert the music position still advances (`wait_pos`, 0 underruns), and show no splash.
- **The SDK's own tests** (W4-0): `sdk_badgame` in each misbehaving mode (an overrun slice, a write outside its block, a forbidden name, over quota, a held boost, an ignored `exit()`, a loop in game code, a loop inside an SDK call, a 3 s save under a 1 s policy), and **`-sdk-exit-chord`** (overlaps of 0.3 s and 1.9 s in both orders don't exit and leak no event; 2.0 s exits with the policy honoured; a long MENU exits Class A and never B/C; hold PLAY reaches no game).
- **`modpak.py check`** refuses an SDK module with a non-SDK symbol or a stray relocation (fixtures in `tests/modpak/`).
- **Goldens** are proposed by the game's package and written by `scripts/golden_capture.sh` with the orchestrator.

### Device checks (only the unit can answer)

| Check | What |
|---|---|
| D-W1 | the wheel's untouch (lift) packet, the packet rate while touching and while resting, and position jitter at rest (the absolute mode, Q30, and the rhythm game's regions) |
| Exit chord feel | accidental triggers over an hour of Bricks and of Quadrus |
| Slices | the Class A 20 ms and B/C 50 ms slices, from `sdk_badgame` on the unit, with the unwind timed (`sdk kill us=`) and 0 underruns |
| Quadrus feel | the wheel's step quantum per column; SELECT-at-release rotation lag in Class A |
| Load latency | a ≤ 56 KB game on flash and on an awake and a sleeping HDD, music playing |
| Battery | 30 min A/B: music dark vs Solitaire vs Quadrus (the `powerlog` method) |
| D-S1 (optional) | `MMAPn` write permission on silicon (§11.4) |
| **D-W2** (the rhythm game) | touch-to-event latency: from a finger landing to its `sdk_region_ev`, and the stamp's error against a reference (a photodiode or a logged GPIO edge) |
| **D-A1** (the rhythm game) | audio output latency: from the I2S DMA position `sdk_song_pos()` reports to sound at the headphone jack, at 44.1 kHz, through the WM8758 |
| #52 / stage 3d | the panel's TE signal and refresh period (60 fps): **answered 2026-10-26**, 67.8 Hz; the 3D reference engine's silicon figures are in `research/bcm2722/render3d.md`, "On silicon" |

---

## 16. Open questions (owner)

| # | Question | Default |
|---|---|---|
| **Q-SDK-1** | Class A file access: brief §2A.3 lists `sdk_file_*` for Class A, but Class A's entry budget is 2 reads (image + one blob) | **Class A keeps everything in its state record**; `sdk_file_*` is B/C |
| **Q-SDK-2** | Sound effects for Class B beside the music (a second mixer channel) | **No** in rev 0: the click only (as `games.md` G-5) |
| **Q-SDK-3** | Solitaire: the default draw, and what a change does mid-deal | **Three Cards** by default (the classic default); a change applies from the **next deal** (the alternative: offer to deal again at once) |
| **Q-SDK-4** | A Class B cutscene: after the clip, resume the music if it was playing? | **Resume** if it was playing before the clip (a Class B game runs beside music) |
| **Q-SDK-5** | A cutscene that fails: the video-failure rule (stop audio, styled error screen) or skip the clip? | **Skip it**, log it, and let the game carry on (a cutscene is decoration, and the music is already paused) |
| **Q-SDK-6** | The falling-block game: its name, and shipping it as Class A in P12 ahead of its Class B form in P19 | **"Quadrus"** (owner, 2026-10-23); **ship Class A in P12** (§13.2's scheme), move to Class B with P19 |
| **Q-SDK-7** | A legal look at Quadrus' presentation before a public release (the genre's trade dress) | yes, on the P20 list; our own name, art, colours and a 12 × 18 well meanwhile |
| **Q-SDK-8** | Game media (§8.6): a visible, read-only `/Games/<Folder>/` outside the quota, skipped by the library scan | **yes**, for Class C games that declare `media` |
| **Q-SDK-9** | The rhythm game: its name, and how to play chart "jumps" on a one-position wheel | **"WildWheel"** (working); a jump becomes one note on its first lane, the chart marked "adapted" (alternative: either lane counts) |
| **Q30** (brief) | Class A read-only `sdk_wheel_pos()` | still open; rev 0 returns `SDK_E_CLASS` (the owner's "relative only"), so either answer needs no ABI change |

---

## Revisions

- **rev 0.3** (2026-10-25, Chess, #132): `sdk_wheel_pos()` built for every class (Q30's default, owner 2026-10-24), with the SDK's idle poll for a game that reads it; `SDK_INF_PRESSED`, `SDK_INF_LIT` and `SDK_INF_DARK` appended (§3.3); Chess (§13.5). No table change; ABI stays 0.1.

- **rev 0.2** (2026-10-24, Quadrus's controls): `sdk_long_menu()` appended (table entry 43, §3.2a): a Class A game may take the long MENU as its pause; the `SDK_STR_EXIT` row is the SDK's graceful exit (§5.4), and every game's pause menu ends with it; Quadrus's new controls (§13.2). ABI stays 0.1.

- **rev 0.1** (2026-10-24, P12 integration): covering screens, USB (Class A) and soft sleep suspend; the deferred record write; `SDK_IN_LONG` once per hold; `SDK_INF_SETTINGS` set; the value list for CHOICE rows over 3 values; `sdk_num_width()` and `SDK_WHEEL_REL_RAW` appended (table entry 42; `tests/sdk/check_sym.py` keeps `sdk.sym` append only); `SDK_WHY_SYSTEM` appended; the accountant's attribution; the games file's min() read. ABI stays 0.1 (nothing shipped).
- **rev 0** (2026-10-23, P12 W4-0): first version, from brief §2 and §2A, with the owner's additions the same day: per-game settings (Solitaire's draw), the falling-block game (Quadrus), and the long-term rhythm game as the Class C example (wheel regions, songs with a sample-accurate position, the global offset, 60 fps layers, the game media folder).
