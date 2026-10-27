# Brief: Phase 8b, visual polish on the real screen

> **DRAFT, Revision 3 (2026-10-19).** Based on **`main` `11b8a83945`** (the deploy-1019 merge: library generations and the statistics journal, #85; USB batching, #81; P15 and the BCM dev work came in with `usbfix`). Every file:line below is on that base. Branch `p8b`. **Part of 8b already shipped on `main`** (§0.0): the tooling (8b-0), #67 and the library footer with its producer stamps (8b-1), the half-pixel drift, and the translucent scrollbar (#74). What is left: a **trim first (8b-T, new)**, then the material kit (8b-2), the metallic panels and shadow (8b-3), and motion, Hold and the device passes (8b-4). It becomes the real brief after the owner round on the open ⚠️ questions (§0.1) and this revision's `budget-reviewer` review (findings at the end).

*Process: `docs/process.md` (brief template, checklist 1–13, lean rules 1–11, one gate per machine). Open owner questions are frozen as ⚠️ with a default (§0.1); the brief is not revised mid-phase for them. Tracker: #52 (polish), #67 (done), #74 (done), #45 (Thumb, used by 8b-T), with #58/#65/#23 notes folded in. ROADMAP order 13.*

*Revision 3 (2026-10-19). Re-based on `main` `11b8a83945`; every reference re-checked (`tagcache.c` moved by ~3,000 lines with #85, `library.c` by ~400, `preview.c` by ~400). Sub-phases already merged are recorded as done with their commits (§0.0) and their sections shortened to what still matters. **New 8b-T**: `_end` is 1,099,376 B against `P9_END_MAX` 1,100,000 B (624 B left) and the owner's rule is "trim, don't raise", so 8b's +21–30 KB of resident art and code needs room first; measured candidates in §4.T, gates on both `_end` and `rockbox.ipod` in §6. **#85 changes the library story**: an update now builds into a second context and the published one stays readable, so the lists no longer go empty after a USB session (§1, fact with file:line); the footer's remaining cases and the owner's newer direction (paused is normal; hide the walk at boot) become question **O12**. The panel writer list gains `half_window()` (the half-pixel drift). The old 752,000 B image gate is retired (`fw-ui` is 814,136 B today).*

*Revision 2 (2026-10-11, confirming review: 8b-0 approved, 8b-1 after these fixes). Re-based on `p15-dac`; the tagcache stamps belong to B, at named sites; the scan-path book key comes from the RAM cache via `artcache_key()`, never from disk; the browse empty row is specified as new code; Goal 6 reworded; size re-based on `fw-ui` 720,148 B; F is numbers-only with `lcd-video.c` on the BCM track; the owner's thumbnail and pan reports join 8b-0.*

*Revision 1 (2026-10-11, brief review: 3 High, 7 Medium, 6 Low). The silent library states what the user sees in every database state; the drift's cover fade becomes shadow-aware and one compose function is the contract (§3.1, §4.3); #67's rule ranks the playing book, then the pending record, then unseeded stamps; the pin has its own generation; a size gate per sub-phase.*

**Goal.** Make the shell look and move like the 7G **on the 5.5G's own LCD**, not just in the simulator:
1. **One shared material kit** (blue gel, green/red gel, grey gloss, metal, groove, reflection, drop shadow), defined once, generated as tables at build time, used by every surface; plus the **style gallery** (exists, 8b-0) showing every material on one sheet.
2. **Metallic panels:** Settings tiles and main-menu panel glyphs as metallic objects on a reflective surface, the big **Date & Time calendar page** (no tile), the 3D capacity capsule, About's gel legend squares.
3. **Gel selection bar, header glyphs and battery** on the kit; the **split-screen depth shadow**. (The trackless translucent scrollbar is done, #74.)
4. **Motion tuning** on the device (slides, panel cross-fade, drift, header swap, frame pacing) and the **Hold face** timing.
5. ~~#67: the last-played audiobook pinned at the top~~ **done** (§0.0).
6. **Library building:** the footer (done, §0.0) now shows only when the lists really are empty (§1 "Library generations"). Whether it stays for those cases is **O12**.
7. **Side-by-side passes** against the owner's 7G photos until each screen passes owner review.

**Split by risk** (§4): the trim (8b-T) lands and is gated before any byte of art; the material kit's contract is committed before any visual fan-out; motion and the Hold face need the device and come last.

---

## 0. Owner pre-flight

### 0.0 Already on `main` (do not redo)

| Item | Commit (merge) | What exists | Where |
|---|---|---|---|
| **8b-0 tooling** | `edfd49f541` (2026-10-12) | side-by-side harness, style gallery, row-art fill and panel-step traces | `tests/review/p8b-sxs.sh` (+ `.txt`), `art/gallery.py`, `tests/ui/ipodui-p8b-{gallery,rowart-latency,pan-cost}.script` |
| **#67 pin (8b-1a)** | `5b31d28c63` (2026-10-12) | the last-played book on top of Audiobooks, ranked playing → pending → highest unseeded stamp; books-only pin generation | `ab_store_pin_key()` `ab_store.c` :601 (seeds skipped :579); `lib_pin_find()` `library.c` :1915, pin pass :1977–2012, `lib_pin_generation()` :2246; `browse.c` reopen :606; `tests/ui/ipodui-p8b-pin.script`, host cases (`1cd0fdecdc`) |
| **Library footer + producer stamps (8b-1b)** | `1078bc279e` (2026-10-12) | the "Updating Library…" footer on Music screens and Search while `lib_state() == LIB_UPDATING`; the main menu's strip removed; `lib window` stamps and `usb release` log | contract `menu.h` :53–79, show rule `menu.c` :519–530, users `menu.c` :300, `browse.c` :461, `search.c` :506; stamps `tagcache.c` :670–686, :5390, :9393, :9422, :10117; log `ui_main.c` :785–800; `usbscreen.c` :1097; `tests/ui/ipodui-p8b-{footer,silent}.script` |
| **Half-pixel drift, NP clock skip (p8b-smooth)** | `5286f614a2` (2026-10-12) | each drift pixel shown in two half steps (75 ms cadence at the same speed) | `half_window()` `preview.c` :631–656, `NAV_DRIFT_HALF` `nav_core.h` :113 |
| **Translucent scrollbar (#74)** | `b18fd7d870` (2026-10-15) | a see-through (~60 %) thumb that never moves text, blended only over freshly drawn pixels: settles O10 and Revision 1's M3 | `list.c` :40–55 (`SBAR_ALPHA32` :55), `draw_scrollbar()` :606, callers :688, :774, :952, :1057 |

The unmerged branch `p8b-refs` (`07ad31f83a`, +60 lines to `docs/reference/7g/MEASUREMENTS.md` and `SOURCES.md`) is merged with the 8b-2 contract commit.

### 0.1 Open questions (frozen ⚠️ with a default until the owner round)

| # | Question | ⚠️ Default | Why it matters |
|---|---|---|---|
| **O1** (RESOLVED 2026-10-20: owner chose look B, "midway", `UI_MAT_SEL_VARIANT` 2; A and C stay selectable) | Selection bar: the **blue gel** (owner 2026-10-08: lighter top line, deepening blue, brightening to cyan in the lower third, darker bottom edge) or the older **sky-blue gloss** (2026-10-06: white-blended top half, today's `sel_gloss`)? | **Gel** (the later direction, and the spec says every blue element shares it), tuned toward the lighter sky blue by side-by-side | Every list golden changes either way: one recapture, after approval |
| **O2** | The **5-pixel charging-bolt notch** on the unlit face (#52 comment 2026-10-10): our anti-aliased knockout corners are dark grey 58, the approved mockup's are light 148 | **Match the mockup** (fix the knockout edge pixels; the reference stays) | The gate prints "needs sign-off" on every run |
| **O3** | **Hold face timing** (#65 owner note 2026-10-11, "may need timing tweaks"): what feels off? (a) the instant cut into the face, (b) the face flashing on a brief accidental Hold flick, (c) the way back on Hold off | **Collect specifics first; no change until then.** Candidate fixes, each one constant: a ~150 ms fade into the face; a ~0.5 s debounce before the face on Hold on; on Hold off, today's full redraw | Unknown until the owner says; stock behaviour on Hold is unconfirmed (ledger 2.25) |
| O4, O5 | *History (Revision 2): what may hold the USB screen through the empty window; what the first-ever build shows.* | **Superseded** by the owner's footer answer (2026-10-11, quoted below) and then by #85 (O12). Release on unplug stays as today | — |
| O6 | *#67 details (pinned row is the same row, ranking, no mark, Audiobooks only, no letter, no pin from seeds only)* | **Implemented with these defaults** (§0.0). The owner may still revise in the round | — |
| **O7** | **Motion** (#52 2026-10-08: "a little hurried and fast, rather than smooth and calm"). The durations are measured from the owner's videos (190/250/290 ms, drift 7 s and 400 ms fade). Keep them and soften the curve, or lengthen them? | **Keep the measured durations; change the slide curve from cubic to quadratic ease-out** (`ui_main.c` :489–492), fix frame pacing on the device, and re-measure from a 240 fps capture (C1) before touching any duration | A 24 fps review video looks jerkier than the device; the LCD's response may be the real cause |
| **O8** | Panel arrival: keep our **immediate 200 ms cross-fade** (ledger 3.5) or match stock's rest delay (0.4 s / 1.1 s) with its varying slide/fade? | **Keep the immediate cross-fade** | Ledger 3.5 "to confirm" |
| **O9** | **Split-screen shadow** width and strength (spec `main-menu.md`, "measure at polish") | **6 px, 30 % black at the seam, quadratic falloff**, re-measured from C4. **Owner 2026-10-20: 10 px, 36 %** ("a little more intense, but not by much") | One 10-entry table |
| O10 | *Scrollbar translucency* | **Done** (#74, §0.0) | — |
| **O11** | Text "looks blocky" in sim shots (#52 2026-10-09) | **Judge on the LCD; no font change in 8b** unless the device check says so | A font change would recapture every golden again |
| **O12** (new) | **The "Updating Library…" footer after #85.** With library generations an update builds a second context and the published library stays readable (§1), so the footer no longer shows after a normal USB session. It still shows when the lists are truly empty: the **first-ever build** (15–45 min at 50k), the ~0.6 s at **every boot** before the library's RAM copy loads (device, 2026-10-19: `lib window empty_ms=580…610` on four boots; only visible if a Music list is open, and the shell never starts on one), a **reload waiting for memory** (`ui_reload_pending`), and a library whose generation is unresolved. The owner's later direction is that maintenance should be rare, fast and hidden (paused is the normal state; the walk could hide in the boot screen like stock; sleep and instant resume #86 later) | **Keep the footer only for those empty-list cases, show nothing otherwise: no code change**, since that is what `lib_state()` already does on `main`. Re-check after #86 and any boot-time maintenance window, which may remove the boot case entirely | An unexplained empty Music menu during a first build reads as "my library is gone"; anything more visible contradicts the "hardly noticed" direction |

> **Owner answer, 2026-10-11 (history; supersedes the Revision 2 O4/O5 defaults):** "have an updating Library indicator at the bottom of the Music menu… or an overlay thing on the bottom… pick something elegant and in the style of our approach". Chosen (orchestrator): the footer bar, built in 8b-1b (§0.0). Release on USB unplug stays as today.

### 0.2 Captures from the owner's stock 7G (#26 rules: **read-only**: photos and videos only, never mounted writable, no sync; any setting touched is put back)
- **C1, 240 fps slow motion:** Main → Music push and MENU pop; Music → Artists (split → full) and back; the panel arriving on three Main items; Songs spun slow, then fast (also settles ledger 2.19, the stock fast-scroll).
- **C2, Hold:** slide Hold on while lit on the main menu, then in Now Playing; press a button while held; slide Hold off. 2–3 s each (settles ledger 2.25 and O3).
- **C3, charging:** a straight-on macro of the header battery while charging, and of the battery on a dark screen (USB or About) while charging.
- **C4, materials, straight-on macro, screen filling the frame:** a list's selection bar; the header play glyph, pause glyph and battery; every Settings tile; the Date & Time calendar; the main-menu Settings capacity panel and the Podcasts panel; About page 1 (legend squares, capsule); the main-menu seam at x = 160 with a bright cover on the panel (the shadow).
- ~~C5, Audiobooks reorder~~: still useful for the ledger row of #67 (changes-from-7g 2.26), not blocking.

---

## 1. What already exists (read it; reuse it, do not rebuild)

| Item | Fact, with evidence (`main` `11b8a83945`) | 8b does |
|---|---|---|
| **Gloss primitive** | `struct ui_gloss` (`paint.h` :30–38); `ui_gloss_color()` computes one row's colour per call with two blends (`paint.c` :80–104); `ui_paint_gloss()` loops it per row (:106–126). **16 private `struct ui_gloss` descriptors** across 7 files (theme, rowart, qscroll, roller, vlimit, setwidgets, search; `grep 'struct ui_gloss [a-z_]* *='`) | Becomes the kit's runtime fallback; hot surfaces read generated tables instead (lean rule 2) |
| **Selection bar** | `sel_gloss` (`theme.c` :54–63): the white-blended top-half recipe, not the gel. Drawn per row by `ui_paint_selection()` (`paint.c` :133), re-derived per row for anti-aliasing by `ui_paint_selection_row()` (`paint.c` :128–131), used by the chevron (`paint.c` :148), `rowart.c` :448, `screens/checklist.c` :180, `screens/browse.c` :263 and `screens/search.c` :433 | Moves to the gel (O1) through one table |
| **Blue gel (exists, private)** | Now Playing has it: `GEL_TOP`/`GEL_EDGE` (`screens/nowplaying.c` :453–454) and `gel_rows()` (:485–494), used for the bar fill (:514, `NP_BAR_H` = 14), the diamond (:536, 15 rows) and the stars (:553, `STAR_H` = 19, :179), pre-blended once per boot | **Lifted into the kit unchanged**; NP goldens must stay pixel-identical (the proof) |
| **Header glyphs** | Battery: green/red/glass row tables (`header.c` :75–96) and `draw_battery()` (:320–391); play: `play_rows` (:108) and the coverage map `play_map` (:122, from `apps/ipodui/art/play.py`), **drawn pixel by pixel** through `glyph_px()` → `lcd_drawpixel()` (:210–222) in `draw_play()` (:224–268); lock (`draw_lock()` :270–303); bolt `bolt_rows` (:163) with a light halo (`draw_bolt()` :304–318); dark-screen battery `draw_dark_battery()` (:730–790, bolt knocked out at :768–776) | Pre-rendered tiles on the kit's gel (lean rule 1); battery fill composed from two column tiles |
| **Four bolts** | Light header (`header.c` :163), dark header (:768–776), Charging screen (`sysscreen.c` :63, `apps/ipodui/art/sys/`), unlit face (`unlit_glyphs.h` :79–80, knocked out of the fill in `unlit_core.c` :329–333) | One bolt shape in the art kit, four renderings; O2 fixes the unlit knockout edge |
| **Panel tiles** | `screens/panels.c`: 76 px tile (`TILE` :97), `cv_tile()` gradient + rim (:217–236), `cv_reflect()` (:247), `draw_calendar()` = white page with a red band **on the tile** (:298–315), `ui_panel_draw_tile()` (:317–378, cost estimate in the comment at :379–383). Glyphs are flat 4-bit RLE coverage (`panelart.h`, generated by `apps/ipodui/art/panelart.py`): **21,601 B rodata today** (`panelart.o` 5,081 + `panels.o` 16,520, `build/fw-ui/rockbox.map`) | Owner-decided **option 2** (`spec/settings.md` "Rendering approach"): 4-bit metallic shading masks + per-row colour tables + head-on reflection at draw time |
| **Panel engine** | `apps/ipodui/preview.c`: one push per panel (`push_panel()` :177), in-place cross-fade over what the LCD holds (`xfade_rect()` :189–), keys drawn into the LCD or the strip (`draw_key()` :249–), drift blits from the slot (`blit_window()` :568–576) **and half-pixel blends** (`half_window()` :631–656, writing `FBADDR(UI_PANEL_X, 0)` directly at :635; called from `pan_to()` :938), books drift (`drift_draw_books()` :812), static first cover at boot (`DRIFT_HOLD` :66). **Every writer into the panel** (review H2, M4, plus Revision 3): `draw_key(…, NULL)` straight into the LCD framebuffer (:1212, :1463, :1501); `draw_key(…, strip)` for a highlight fade's new image (:1274); `xfade_rect()` toward the strip (:1239, :1339) and **toward the raw cover slot** in the drift's cover fade (`drift_fade_step()` :852–888, the call at :868), which bypasses `draw_key()`; `blit_window()` for whole-pixel pan steps (:796, :957); `half_window()` for half steps (:938). **No shadow anywhere** | One compose function and a shadow-aware fade and half step (§3.1, §4.3) |
| **Scrollbar** | **Done** (#74): translucent, no track, blended only over fresh pixels (§0.0) | Re-coloured from the kit if O1 changes its grey (`ui_theme->sbar_gloss`, `list.c` :608) |
| **Motion** | Durations `nav_transition_ticks()` (`nav_core.c` :239–255); **cubic ease-out** for every slide (`ui_main.c` :489–492); 30 fps cap `UI_ANIM_FRAME_TICKS` (`anim.h` :45); panel fade 200 ms / 6 steps (`nav_core.h` :40–41), rest delays (:35–36), drift 7 s cycle, 400 ms / 8-step fade, 15-tick steps shown as two half steps (:109–113); row jumps `UI_LIST_JUMP_*` (`list.h` :116–). **`UI_T_REVEAL` and `UI_T_SPLIT` run as plain pushes in v1** (`ui_main.c` :347–350) | Curve and pacing (O7). The measured reveal and split stay with P12 (Clocks) and P7 (Cover Flow) |
| **Unlit face / Hold** | `UE_HOLD_ON` enters the face at once (`unlit_core.c` :145, #65), entry is one full `lcd_update()` (`unlit.c` header comment :16); swallow windows `UNLIT_SWALLOW_MAX`, `UNLIT_WHEEL_SWALLOW` (`unlit.h` :72–73) | O3 constants only |
| **Library generations (#85)** | Two contexts: `ui_active` (published, read by every list) and `ui_build` (`tagcache.c` :545–546); `tc_stat` and the RAM cache are `ui_build`'s (:607, :609). An update starts a candidate in the other context (`ui_candidate_begin()` :4852–4962, `ui_build = next` :4959); the commit drops **`ui_build`'s** RAM (:5388) and the reload loads it (:9391–9419); publication swaps `ui_active = ui_build` with no yield (:5076). The lists read `tagcache_ui_count()` = `ui_active`'s count (:9797–9800, :9720–9726), so **during an update the old library stays listed**. `lib_state()` (`library.c` :2230–2238) is `LIB_UPDATING` only when `ui_active` has no count and the database is initialising, committing in the active context, or `tagcache_ui_busy()` (:9663–9673: a reload waiting, a fresh database's commit) | O12. **One stale log semantic** (Revision 3): `ui_win_drop()` at :5390 and :9393 stamps "lists go empty" whenever `ui_build`'s cache drops, also when `ui_build != ui_active` and nothing went empty, so a post-update `lib window empty_ms=` overstates the window. Fixed in 8b-T as a log-only change (§4.T) |
| **Boot hold** | The shell waits behind the boot logo only while an interrupted commit finishes (`apps/main.c` :409–415); a pending commit otherwise runs in the background | Unchanged |
| **USB screen** | Native screen loop `ui_usb_screen_loop()` (`usbscreen.c` :831), leaves on unplug (:1087); upstream `gui_usb_screen_run()` calls it (`apps/gui/usb_screen.c` :312) then `settings_apply()` and `playlist_resume()` (:348–350); `misc.c` :848 | Unchanged |
| **Review tooling** | `scripts/pngcompose.py` (reference left, ours right), `tests/review/p8b-sxs.sh` (generalised, 8b-0), `tests/review/{np,ab}-sidebyside.sh`, `scripts/make_video.sh`, `tests/ui/ipodui-nav-*-review.script`; owner photos under `docs/reference/7g/owner/labeled/` (git-ignored, main checkout only) | Used per screen (§4.4) |
| **The bolt notch** | `tests/ui/ipodui-p8a-unlit.script` :13 references `docs/design/unlit/stock-12h-hold-charging-lit.png`; non-strict refs print "needs sign-off" (`scripts/validate.sh` :852–882) | O2 |

**Existing upstream and shell paths doing the same job (checklist 12)**, and which wins:
- **Materials:** upstream's skin/theme engine and backdrops are trimmed (P9); nothing upstream draws shell surfaces. The kit replaces the 16 private descriptors one surface at a time; `ui_paint_gloss()` stays as the fallback for one-off sizes.
- **Battery:** upstream's statusbar battery isn't used by the shell (its own header, `header.c`). `battery_level()` (voltage) stays the source; calibration is #10/#70, not 8b.
- **Library status:** upstream's boot `splash_progress(LANG_TAGCACHE_INIT)` is compiled out in the shell (`main.c` :414 onward is the `#else`); upstream "Updating in background" strings (`english.lang` :2214) are unreachable. The footer (and the Music hold menu's own progress step, `screens/holdmenu.c` :290, a user-started rebuild) are the only visible ones.
- **Hold:** upstream's "backlight on button hold" setting (`settings_list.c` :2291, hidden) stays at its default; the shell's `UE_HOLD_ON` path wins (#65).
- **Code size (8b-T):** upstream's whole-firmware `--thumb` option exists (`tools/configure`, measured −162,496 B .text, `docs/code-audit-1.md` :182–186) but slows hot paths; 8b-T uses per-file Thumb for cold shell code only (#45's plan).

---

## 2. Out of scope
- The measured **Clocks reveal** (shared-backdrop push) and the **Cover Flow split**: P12 and P7 (they run as pushes today).
- Battery **calibration** for 3000 mAh (#10, #70).
- Font changes (O11), new languages, any Rockbox setting.
- **Haptics (#23):** the clicker cadence is unchanged; 8b adds no click to transitions.
- **BCM2722 2D offload (#60):** 8b renders on the CPU. §4.5 only says where an offload could later plug in.
- Library update speed, the ~18 s no-change boot walk, boot-time maintenance, sleep and instant resume (#86): ROADMAP "In flight" / "Later", not 8b. O12 is re-checked when they land.
- Thumb for **hot** code, codecs or IRAM (#45 stays open for a device-measured pass in 9b).

---

## 3. Contracts (committed to `p8b` before fan-out, with host tests and one shared stub)

### 3.1 `apps/ipodui/material.h` (new) and `art/materials.py` (new)
```c
enum ui_mat {                       /* one entry per material, spec index.md */
    UI_MAT_GEL_BLUE, UI_MAT_GEL_GREEN, UI_MAT_GEL_RED,
    UI_MAT_GLOSS_GREY,              /* scrollbar rod (horizontal) */
    UI_MAT_METAL,                   /* tiles, capsule, glyph shading */
    UI_MAT_GROOVE,                  /* NP track, capsule channel */
    UI_MAT_COUNT
};
/* Row colours of material m for an object h rows tall: a generated const
 * table for every (m, h) pair the shell uses, else NULL (the caller falls
 * back to ui_paint_gloss()). Never computed per pixel. */
const fb_data *ui_mat_rows(enum ui_mat m, int h);
/* Head-on reflection alpha (/256) of row d under an object h tall, n rows
 * (np_flat_refl_a() moved here, same values). */
unsigned ui_mat_refl_a(int d, int h, int n);
/* Split-screen shadow: alpha (/256) of panel column c (0 = the seam). */
#define UI_SHADOW_W 6                                   /* ⚠️ O9 */
extern const uint8_t ui_shadow_a[UI_SHADOW_W];
/* Applies the shadow to a composed 160 x h panel image in place. */
void ui_shadow_apply(fb_data *px, int stride, int h);
```

**Panel compose and fade (in `preview.h`, review H2/M4, Revision 3).** Every panel image reaches the LCD through exactly one of these, and nothing else writes the panel rectangle:
```c
/* Draws key k into dst (the LCD framebuffer at UI_PANEL_X, or the strip)
 * and applies the shadow once. Replaces the bare draw_key() calls at
 * preview.c :1212, :1274, :1463, :1501; blit_window() calls it too. */
int  ui_panel_compose(const struct nav_panel_key *k, bool leaf,
                      fb_data *dst, int stride);
/* In-place fade of the LCD panel toward src (weight /256). src is a raw
 * (unshadowed) image when 'raw' is set, e.g. the drift's cover slot: for
 * columns 0..UI_SHADOW_W-1 the target is src darkened by ui_shadow_a[c],
 * so the shadow neither fades out nor snaps back across a cover change.
 * Replaces every xfade_rect() call (:868, :1239, :1339). */
void ui_panel_fade(const fb_data *src, int sstride, int weight, bool raw);
```
**The half step** (`half_window()` :631–656) is a third writer: it averages two slot rows into the LCD (`AVG2`, macro :590, used :626). It keeps its O2/unrolled loop for columns `UI_SHADOW_W..159` and calls `ui_shadow_apply()` on columns 0–5 of the rows it wrote, so a half step is shadowed exactly like a whole step. `ipodui-p8b-shadow` checks a half step too (§7).

**No `ICODE_ATTR`** on compose, shadow, fade or the half step (review L4): IRAM is for codecs (CLAUDE.md rule 3; `.iram` + `.ibss` = 34,512 B today).
- **One source of truth:** `art/materials.py` holds the key colours and writes `apps/ipodui/material_data.h` (generated, committed; header says how to regenerate). Every art generator (`apps/ipodui/art/*.py`, `art/*/*.py`, `art/gallery.py`) imports it, so a glyph pre-rendered at build time and a surface drawn at run time use the same colours.
- **Host test** `tests/material/`: the committed `material_data.h` equals a fresh run of the generator; every table monotonic where the spec says so; and **NP's gel against a frozen golden** (review M7): `tests/material/np_gel_golden.h`, committed **with the contract, before any move**, captured on `main` (`NP_TICK_TOP` 0, `np_draw.h` :198), holds today's `gel_rows()` output as `uint16_t` (`np_px`, `np_draw.h` :32) for the three heights NP uses (14, 15, 19, §1), plus today's `np_flat_refl_a()` values (`np_draw.c` :629, declared `np_draw.h` :214) for the NP bar and art reflections. The test compares the kit's tables with the golden, so it still fails after `gel_rows()` is deleted.
- **Stub:** the header plus `material.c` returning today's colours (sky-blue gloss for the selection bar), so agents compile against it before the gel lands.
- **Tile colouring** (review L5): a mask pixel is a 4-bit level; each tile row gets a 16-entry colour palette built once per row from the metal rows (16 entries × 98 rows ≈ 1.6k blends per tile, not per pixel), and the pixel loop is a table lookup and a store, **no blend per pixel**. Counted against the 300k-instruction frame budget (lean rule 8): target ≤ 60k for the glyph and ≤ 140k for the whole tile (today's estimate, `panels.c` :379–383).

### 3.2 #67 and 3.3 the device log lines: **done** (§0.0)
Implemented as Revision 2 specified. Two consequences carry into the remaining work:
- The panel and list changes must keep `ipodui-p8b-pin` and `ipodui-p8b-silent`/`-footer` passing (they are in the gate).
- The `lib window` stamps need the Revision 3 correction in §4.T (stamp only when the active context empties).

---

## 4. Work items, by sub-phase

### 4.0 8b-0 tooling, 4.1 8b-1 library and state: **done** (§0.0)
Left from 4.1: the device measurement of the empty window after a USB session is **moot** under #85 (the lists stay populated); the boot window (~0.6 s) is recorded by the device logs of 2026-10-19 and goes to `hardware.md` with 8b-T's numbers.

### 4.T 8b-T, trim first (new in Revision 3; agent G, firmware-platform, opus; own gate before 8b-2 merges)
**Why:** `_end` = **1,099,376 B** against `P9_END_MAX` = **1,100,000 B** (`validate.sh`, gate log 2026-10-19), 624 B left. Every byte 8b adds is resident. The owner's rule: **trim, don't raise**. 8b's estimate is +21 to +30 KB (§6).

**`_end` moves in 4 KiB steps** (budget review M1): `inquiry_static` is page-aligned (`0x000ff000`; `cbw_buffer` ends at `0x000fe764`), so the gap before it absorbs savings until a whole page is freed. `_end` ≤ 1,072,000 B is reached only when the **linked** saving is ≥ 26,468 B (→ ≈ 1,070,704 B); at 26,467 B or less `_end` lands near 1,074,800 B and fails. So the 8b-T gate is: **linked text and data saved ≥ 26.5 KB, or `_end` ≤ 1,072,000 B**. Measured at `.o` level, T1a + T2 + T3 give 22,903 B, so 8b-T needs part of the T1b reserve (each file with its timing proof) to pass. After 8b-T, 8b-2 and 8b-3 have **20–24 KB** together, depending on where in the page `_end` sits (the close gate, §6); if the estimate's top (+30 KB) is reached, more T1b files join, else E's mask cap drops from 36 to 30 KB.

**Measured candidates** (`build/fw-ui` at `241cf99559`, the image now on the device and equal to `main`'s firmware):

| # | Candidate | Measured | Risk and proof needed |
|---|---|---|---|
| **T1a** | **Thumb for safe-cold shell code** (#45's plan, cold half only): per-file `-mthumb -mthumb-interwork` for screens and services that run on a press or once, never per frame, never in an interrupt and never in a bulk loop: about, apath, brightness, clockpol(+core), datetime, devmode, eq, eq_hw_presets, holdmenu, language, legal, menucfg, newsw, reset, sc_hw, settings, setwidgets, stub, sysscreen, udmatest, usbscreen, vlimit | **−11,394 B** (`.o` text 48,603 → 37,209, −23.4 %) | Interwork costs nothing at `-Os` (`code-audit-1.md` :178–179). `.o` sizes include functions the link may drop: G reports the **linked** delta. One reversible switch (`IPODUI_THUMB_COLD`): a per-object flag list in `tools/root.make`, built like the existing per-object `core_secflags` filter (:521–526), default on in `fw-ui`, off in the sim (no effect there). `usbscreen.c` stays in T1a with its own check: its `usb_compose_all()` blends 12 frames per pixel on every plug (`usbscreen.c` :188–219, :916–938), so device check 8 times the first USB frame before and after; if it is > 10 % slower it moves to T1b. Device check 8 |
| **T1b** | **Thumb for the second tier, the ready reserve, only with a timing proof per file:** screens whose row callbacks run per frame while scrolling (audiobooks, playlists, otglist, checklist, search); services with loops over the library or a file (search_core, lib_shuffle(+rec), shuffle_core, dbgen, dbjournal, artfmt, and `otg.c`, which resolves a list with a pass over the filename tag file, `otg.c` :895–950, :988); and code that runs in the tick interrupt (`powerlog_core.c`: `pl_ring_push()` every HZ/4, `powerlog.c` :246, :550; `batcal_core.c`: `bc_tick()` every second, `batcal.c` :98–120, :366) | **−18,396 B** (`.o` text 58,559 → 40,163) | Each joins only if its own cost moves < 5 %: sim cycle counts for the per-frame callbacks (the 8b-0 `panel cost`/row traces) and the interrupt paths (cycles per tick), and on the device the search keystroke time over the owner's library, OTG list resolve, Shuffle Songs start, the boot's journal replay and the art builder's per-album time (`ipodui.log`). These files are the **named reserve** for the 8b-T gate below. **Never Thumb:** `sortkey.c` (the collation key inside the commit's sort: scan speed is an owner priority), and every file on the hot list |
| **T2** | **The stock developer UI's own objects**, reachable only through the hidden developer combo: `apps/debug_menu.c`, `apps/screens.c`, and the `apps/gui/` files the shell never calls at run time (`splash.c`, `option_select.c`, `yesno.c`, `statusbar.c`, `list.c`, `bitmap/list.c`, `menu.c`) | **−9,388 B** (`.o` text 32,090 → 22,702) | G proves each is cold in the shell before it joins: no caller outside the developer UI in the linked map, except `splash.c`, whose callers in playback, playlist and tagcache all exit at the shell's suppression funnel (`splash.c` :382–392) before any drawing; `apps/recorder/*` (JPEG decode and resize, used by the art builder) is **not** in it. Under `HAVE_IPOD_UI` only, so stock objects stay ARM |
| **T3** | `splash_buf` (`apps/gui/splash.c` :94, `static char splash_buf[MAXBUFFER]`): **2,121 B bss**. In the shell only the developer UI splashes | Under `HAVE_IPOD_UI`, take the bootloader's stack form (:108) only if every thread that can reach it has 2.2 KB spare at its measured stack peak: the UI thread, and also the tagcache and playback threads, which splash when the developer mode is on (devmode stack listing after a library update and a playback session with developer mode on); else leave it |
| **T4** | **Open-file tables** sized for 31 files (`fs_defines.h` :44): `open_streams` 11,520 B (31 × 372 B, `file.c` :52), plus per handle a 64 B binding (`fobindings`, `fileobj_mgr.c` :50) and a 24 B mutex: **460 B per file handle** | Needs the peak first: G adds a high-water mark of open handles to the developer log, and the sim scripts plus one device day (playback, art builder, USB, a library update) report it. Only then a shell-only `MAX_OPEN_FILES` (peak + 8); never below 16. Not counted on for 8b's 32 KB |
| **T5** | The 4 KB-aligned INQUIRY buffer leaves a **1,644 B** gap before it (`inquiry_static` at `0x000ff000`, `cbw_buffer` ends at `0x000fe994`) | **Not taken:** the owner's rule is not to change the `inquiry_static` definition. A linker-side `SORT_BY_ALIGNMENT` would recover ~540 B at most and moves every `.bss` object; not worth it |
| — | Rejected: the bidi line buffers (`bidi.c` :163–166, 5,960 B) and the built-in English strings (`core_language_builtin`, 16,381 B rodata) | The language backend stays (CLAUDE.md rule 8); the built-in strings are the fallback when no `.lng` loads |
| **T6** | **Log correctness, not size:** `ui_win_drop()` (`tagcache.c` :5390, :9393) only when the lists really go empty: `ui_build == ui_active`, or the active context has no count (`tagcache_ui_count_ctx(ui_active) < 0`, the first build, whose candidate is a different context while nothing is published). So `lib window empty_ms=` means what §3.3 says under generations | Log only; `ipodui-p8b-silent`'s expectation updated with it (its scan with a candidate must not log an empty window) |

**Precedent:** the panic screens' removal from the shell freed ~4 KB the same way (`docs/changes-from-rockbox.md` §2.14).

**8b-T output:** the gate's size checks extended by G (budget review L3): `scripts/validate.sh` :1006–1029 enforces only `P9_END_MAX` and the 914,813 B ceiling and prints "target 760,000" (:1024), so G adds `P8B_T_END_MAX` (1,072,000), `P8B_END_MAX` (1,096,000) and `P8B_IPOD_MAX` (820,000) and updates the budget row at `docs/hardware.md` :89, which still says 760,000; the linked `_end` and `rockbox.ipod` before and after each candidate taken, in the `hardware.md` budget table; a ledger row for the Thumb switch (`changes-from-rockbox.md`, build system) and for T3/T4 if taken; the gate.

### 4.2 8b-2, the material kit and its first users (agent D, ui-fidelity)
- `material.c`, the generated tables (§3.1), and the migration of: the **selection bar** (O1), the chevron and every row that anti-aliases over it (via `ui_mat_rows()`; the callers listed in §1); the **header** play, pause, lock, bolt and battery as **pre-rendered tiles** (the header background rows are fixed, so the glyphs are pre-blended over them at build time and drawn with one `lcd_bitmap_part()` each; the battery is two column tiles, fill and glass, cut at the fill width; green/red gel from the kit); **Now Playing's gel** moved into the kit with no visual change; About's legend squares (gel) and the capsule channel (groove) if D has budget, else E.
- **Bolt notch (O2):** the unlit bolt's knockout edge pixels match the mockup; the p8a reference stops printing "needs sign-off".
- **Goldens:** the selection bar touches nearly every list golden. **One** recapture at the end of 8b-2 after the owner approves the gallery and the side-by-sides (`golden_capture.sh`, pixel-identical twice); none before.

### 4.3 8b-3, metallic panels and the shadow (agent E, ui-fidelity, opus: visual judgment)
- **Tiles, option 2:** `apps/ipodui/art/panelart.py` emits 4-bit metallic shading masks (white bits shaded and fading toward a corner; rounded stroke ends, #52 2026-10-08), RLE as today. At draw time: the tile from `UI_MAT_METAL` rows, the glyph mask coloured through a per-row table, the head-on reflection from `ui_mat_refl_a()`. Captions placed higher (spec `settings.md` "Panel tiles"). Per-icon alignment against the photos (centre, baseline, size). EQ tile wide (#52).
- **Date & Time:** a large calendar page, red month band, big day digits, weekday below, **no tile** (`draw_calendar()` `panels.c` :298 today). **Size against the 16,384 B panel scratch** (`preview.h` :116, review L3): page ≤ 80 × 84 px plus an 18-row reflection = 80 × 102 × 2 = **16,320 B** (fits). If the side-by-side wants it larger, E draws it straight into the compose destination row by row and says so; the scratch is not raised (that is a budget line).
- **Capacity capsule** (main-menu Settings panel and About page 1): glossy rounded metal capsule with a fill. Panel capsule ≤ 120 × 14 px plus a 6-row reflection = **4,800 B**. About's capsule is drawn in About's own full screen (`screens/about.c`), not through the panel scratch. The tile canvas stays **76 × 98** (`panels.c` :381) = 14,896 B.
- **Main-menu panel glyphs:** Podcasts broadcast icon, Extras clock and map, Videos/Photos (kept compiling), on the reflective surface.
- **Split shadow:** through the §3.1 contract only. `ui_panel_compose()` applies `ui_shadow_apply()` once to every composed image (the four `draw_key()` sites and `blit_window()`); `ui_panel_fade()` replaces all three `xfade_rect()` calls: the highlight fades (:1239, :1339) fade toward a composed (already shadowed) strip image, and the **drift's cover fade** (:868, review H2) fades toward the raw slot with `raw` set; `half_window()` shadows columns 0–5 of what it wrote. No other code writes the panel rectangle (T adds a source grep to the gate that fails on any `xfade_rect(` or `FBADDR(UI_PANEL_X` outside those functions and `half_window()`). Settings tiles, drift (whole and half steps), fades and the books drift all get it; no extra push (inside the existing 160 × 240 panel push).
- **The remaining #52 backlog**, each a side-by-side item: book glyph at final size; Volume Limit roller visuals (frosted background, metallic padlock, lighter strip); Search letter cap height and magnifier handle; speed prompt text size; USB 30-pin glyph and arrows; clock-face digits; "Ej" spacing at Regular 12; NP reflection strength judged on the device with dark covers.
- **Budget guard:** panel art grows from **21,601 B** to the owner's estimate of 40–60 KB. If the masks pass **36 KB**, E stops and reports (fallback 1 in `settings.md` is ~300 KB and not resident, so it is not an automatic way out).

### 4.4 8b-4, motion, Hold and the device passes (agents D and F)
- **D (ui-fidelity):** the slide curve (O7) as one function used by every slide and band slide; header title/clock swap (≤ 40 ms, spec `header.md`); drift and fade constants only if C1 says so; Hold face constants (O3) once the owner gives specifics.
- **F (firmware-platform, the LCD/BCM path):** on the device, measure the full-width push rate and a 190 ms slide's delivered frames, check for tearing or smear in slides and drift (slow-motion phone video of the 5.5G), and report whether frame pacing is CPU-bound (compose) or bus-bound (BCM push). **F is numbers-only in 8b**: any `lcd-video.c` change goes through #60. Use 8b-0's traces (`ipodui-p8b-pan-cost`, `-rowart-latency`) and their devmode `ipodui.log` lines for the owner's thumbnail and panning reports, before and after the shadow.
- **Owner round:** side-by-sides of every screen, the gallery, before/after transition videos, the device impressions; the batched questions of §0.1 that remain.

### 4.5 Where the BCM2722 2D offload (#60) could take over later
8b stays on the CPU. Candidates for a later move, if a device measurement shows a battery or speed win at the same fidelity: the panel compose (tile, mask colouring, reflection, shadow), the drift blit, half step and cover cross-fade, the art reflections, and slide frames. To keep that door open, **all panel images go through the §3.1 contract** (`ui_panel_compose()`, `ui_panel_fade()`, the shadowed half step). **Read-back, stated honestly** (review M4): compose reads back pixels from its destination, which is the LCD framebuffer when no strip is free (the `draw_key(…, NULL)` sites :1212, :1463, :1501): the shadow is a read-modify-write and the reflection reads rows just drawn. The fade reads what the LCD holds; the translucent scrollbar reads the rows under it (`list.c` :606). All of it is confined to those functions, so a BCM backend would replace each whole function rather than chase read-backs through the shell. Composing always into the strip was considered and rejected for 8b: the strip is often held by the drift prefetch or a slide, and the extra 76.8 KB copy per panel buys nothing on the CPU path.

---

## 5. Ownership and read lists

| Files | Owner (model) |
|---|---|
| `tools/root.make` (the cold-Thumb flag list, beside `core_secflags`), `apps/gui/splash.c` (T3, under `HAVE_IPOD_UI`), `firmware/include/fs_defines.h` (T4, only after the peak is measured), the open-handle high-water log, `apps/tagcache.c` (T6, the two `ui_win_drop()` guards, each inside its existing `#ifdef UI_COMMIT`) | **G: firmware-platform (opus)** |
| `material.{c,h}`, `material_data.h` (generated), `art/materials.py`, `paint.{c,h}`, `theme.c`, `header.c`, `list.c` (selection colours only), `screens/nowplaying.c` (gel move only), `apps/ipodui/np_draw.{c,h}` (`np_flat_refl_a()` moved to the kit, review M7), `unlit_glyphs.h`/`art/unlit/unlit.py` (bolt), `ui_main.c` (slide curve) and `unlit.h` constants in 8b-4 | **D: ui-fidelity (opus for the gel and glyph judgment, sonnet for the migration)** |
| `screens/panels.{c,h}`, `panelart.{c,h}`, `apps/ipodui/art/panelart.py`, `apps/ipodui/preview.{c,h}` (`ui_panel_compose()`, `ui_panel_fade()`, the shadow, `half_window()`), `screens/about.c` (capsule, legend), `rowart.c`/`roller.c`/`vlimit.c`/`setwidgets.c` (backlog items) | **E: ui-fidelity (opus)** |
| `scripts/` (gate grep, gate_map, check_trace rules), `art/gallery.py` (kit import), `tests/review/p8b-*`, `tests/ui/ipodui-p8b-*.script` (new: shadow; updated: silent for T6), `tests/material/` | **T: tooling (sonnet)** |
| device measurement notes only (no file in 8b; `lcd-video.c` changes go through #60) | **F: firmware-platform (opus)** |
| `apps/SOURCES`, `docs/hardware.md` budget table, both ledgers, ROADMAP, `english.lang` | orchestrator |

Shared-file rules: `ui_main.c` is D's in 8b-4 only. `preview.c` is E's alone. `apps/tagcache.c` is G's for the two T6 lines only, in 8b-T, before anyone else touches it. `material.h` changes go through the orchestrator after the contract commit.

**Read lists (≤ 3 each):** G: §4.T, §6 (size); `docs/code-audit-1.md` :170–190. D: §3.1, §4.2, §4.4; `spec/index.md` "Visual language"; `spec/header.md`. E: §3.1, §4.3; `spec/settings.md` (Panel tiles); `spec/main-menu.md`. T: §3.1, §4.T (T6), §7; `tests/ui/README.md`. F: §4.4; `docs/hardware.md` (LCD); `lcd-video.c`.

---

## 6. Device-reality checklist

1. **Compiled features:** all shell code under `HAVE_IPOD_UI`, the same in the sim. 8b-T's Thumb switch exists only in target builds (the sim compiles natively), so the sim can't show its speed: that is device check 8. The T6 guard is inside `UI_COMMIT` (defined only under `HAVE_IPOD_UI`, `tagcache.c` :93–111). Device-only: the LCD's response (smear, tearing), real push rates, real compose cost, Thumb's speed on cold screens. The generated tables are identical on both (rodata).
2. **Memory:** **no new `core_alloc`, no new reservation.** Tables and masks are rodata. Tile composition and its reflection use the existing panel scratch (16,384 B): tile canvas 14,896 B, calendar ≤ 16,320 B, panel capsule 4,800 B; or the strip as today. **As built (8b-3):** every panel composes in place in its own pixels (LCD or strip), so the scratch lost its last user and is no longer reserved (−16,384 B of shell-start reservation; `UI_PANEL_SCRATCH_SIZE` 0 in `preview.h`, `ui_panel_scratch()` returns NULL; one define brings it back). The shadow is applied in the buffer that already holds the image. Nothing allocates while audio plays; panel draws keep the D9/D15 "no alloc, no lib_open" trace rule (`preview.c` :276–325). 8b-T only removes bytes.
   - **As built (2026-10-20):** one owner-approved exception: the **second drift slot** (8b-4a), +180,000 B taken from the audio buffer at shell start only, holding the other set's last cover so Music ↔ Audiobooks never needs a read. The panel scratch was freed (−16,384 B; 8b-3 composes in place). Net audio-buffer effect of Phase 8b: −163,616 B (`hardware.md`). Under the owner's 2026-10-20 budget rule the audio-buffer floors bind, not `_end`.
3. **CPU:** per panel arrival ~2–3 ms on the device (owner estimate, option 2), boosted as today (a response to input); tile glyph colouring by per-row 16-entry palettes, no blend per pixel. Shadow: 6 columns × 240 rows = 1,440 blends per composed image, ~15–25k instructions; the shadow-aware drift fade and each half step add the same (8 fade steps per 7 s cycle, and now two steps per pixel of pan: 88 shadow applications per cycle instead of 44); on the drift's unboosted 75 ms step that is ≲ 3 % of 30 MHz (measured by T/E with 8b-0's `panel cost` trace, lean rule 8). Header tiles replace ~300 `lcd_drawpixel()` calls per header draw with ~5 blits. Thumb on cold files: more instructions per function, run from cache; T1a has no per-frame or bulk-loop file by construction, T1b files (including the tick-interrupt code of `powerlog_core.c` and `batcal_core.c`) join only with a measured cost under 5 % (§4.T), `sortkey.c` never. **No `ICODE_ATTR`** anywhere in 8b.
4. **Storage:** zero new reads. Masks compiled in, not streamed.
5. **LCD:** no new pushes. The shadow sits inside the existing 160 × 240 panel push; header tiles push the same header rects; the selection bar the same row bands. A full 320 × 240 push takes ~53 ms over the BCM bridge, so a full-width slide runs at **~19 fps** (≈ 3–4 frames in 190 ms) whatever the 30 fps cap; half-width panel pushes and list bands are faster. 8b does not change the push path.
6. **Threads and locks:** all 8b drawing is on the UI thread. T6 reads `ui_build`/`ui_active` on the tagcache thread where it already writes them.
7. **Upstream splashes and prompts:** none added. T3 changes only where `splash_buf` lives.
8. **Persistence:** no new persisted state.
9. **Scale:** no per-entry cost added; the panel work is per panel, the header per draw.
10. **Deferred or retried work, and what the user sees:** unchanged from 8b-1 (its scripts stay in the gate). O12 records which empty-list cases still show the footer.
11. **Stock firmware unchanged:** T6's lines are inside `#ifdef UI_COMMIT`; T3 inside `#ifdef HAVE_IPOD_UI`; the Thumb list applies only to `apps/ipodui/` objects (shell-only); T4 (if ever) only under `HAVE_IPOD_UI`. Stock `.text` byte-identical to `build/fw/.size-baseline`; the stock bootloader stays byte-identical (the gate checks it).
12. **Existing paths doing the same job, and every writer into a shared surface:** see §1. The panel rectangle's writers are all listed (four `draw_key()` sites, three `xfade_rect()` sites including the drift's cover fade, `blit_window()`, `half_window()`) and all go through §3.1. The shell's own: the 16 private gloss descriptors (migrated or left as fallbacks, listed in D's report), NP's private gel (moved, frozen golden), the four bolts (one shape).
13. **Builder iteration predicates:** `apps/ipodui/art/panelart.py` iterates the tile enum the C side uses (a static assert ties the counts). `art/materials.py` emits a table for every (material, height) pair listed in one place; any other pair falls back to `ui_paint_gloss()` (slower, never wrong; traced once per pair in the sim). The cold-Thumb list is one list in `tools/root.make`; the gate prints it with the size report so a hot file can't join it silently.

**Lean rules in practice:** 1 (header glyphs become pre-rendered tiles; tile glyph masks pre-rendered at build time), 2 (every hot gradient from a generated per-row table: today `ui_gloss_color()` runs two blends per row per draw, and `rowart.c` :134–136 calls it inside a per-pixel loop), 4 (shadow and tiles stay inside existing rects), 6 (compose boosted as today, drift never), 8 (counts reported by T/E/D against the 300k frame budget), 9 (no new block), 10–11 (no new reads).

### Size and RAM budget (estimate; measured per sub-phase)

Base (`build/fw-ui`, `main` firmware = `241cf99559`): **`rockbox.ipod` 814,136 B**, **`_end` 1,099,376 B** (`P9_END_MAX` 1,100,000 B; image ceiling 914,813 B; the Phase 9 target of 760,000 B was passed by #81/#85/P15 and is no longer a gate).

| Item | Binary (rodata + text), all resident |
|---|---|
| **8b-T** (T1a + T2 + T3 measured at `.o` level, plus T1b reserve files with timing proof) | **≥ −26.5 KB linked** (T1a + T2 + T3 = −22.9 KB; −41.3 KB with all of T1b; linked figures reported by G) |
| Panel art, option 2 (masks replace today's 21,601 B) | **+15 KB at most** (masks capped at **36 KB**) |
| Material tables (`material_data.h`, ~40 (m, h) pairs) | +1.5 to 2 KB |
| Header glyph tiles (play, pause, lock, bolt, battery columns × 3 palettes) | +3 KB, minus the drawing code they replace |
| Calendar page, capsule, legend | +3 KB |
| Kit, shadow, compose, fade code | +4 to 6 KB |
| **Net** | **≈ −6 to +4 KB** against today, in 4 KiB `_end` steps |

- **Gates, checked at each sub-phase's integration from `validate.sh`'s size report (a sub-phase that crosses one stops and reports before merging):**
  - **`_end` ≤ 1,100,000 B always** (the existing `P9_END_MAX`; never raised).
  - **After 8b-T: linked saving ≥ 26.5 KB, or `_end` ≤ 1,072,000 B**, before 8b-2 may merge. `_end` moves in 4 KiB steps (the page-aligned `inquiry_static`, §4.T), so 8b-2 and 8b-3 together have 20–24 KB.
  - **At 8b close: `_end` ≤ 1,096,000 B** (≥ 4 KB left for P10 device fixes) and **`rockbox.ipod` ≤ 820,000 B** (`fw-ui`; today + 6 KB).
  - `fw-alt` and the largest pairwise variant (`scripts/variants.sh`, rerun at 8b-T and at close) are reported, under the 914,813 B ceiling.
- **Headroom for later phases** (a block in the `hardware.md` budget table at 8b close, each figure set by that phase's brief): P10 device fixes (~4 KB, the margin above), P12 Extras (~60–100 KB: Clocks, Calendars, Alarms, games), P13 mixes (~15 KB), P7 Cover Flow (~30 KB code; its ≈512 KB of textures are runtime). Those do not fit under `P9_END_MAX` without more trimming: the T2 developer-UI Thumb pass and #45's measured hot/cold split (9b) are the known levers, recorded there.
- Every byte is resident (`_end` grows by the same), so it comes out of the audio buffer (~65 MB). No new runtime reservation; statics < 100 B.

---

## 7. Acceptance, review media and device checks

**Gate:** `GATE PASSED` on `p8b` after each sub-phase's integration (8b-T has its own gate and `budget-reviewer` pass before 8b-2 merges); the §6 size gates at each; budget-reviewer end review clean of High findings.

**Host tests:** `tests/material/` (§3.1: regenerated tables equal; the kit's NP gel and reflection tables equal the **frozen golden** committed with the contract). The 8b-1 host cases (`tests/abbook/` pin ranking) stay in the gate unchanged.

**Sim scripts (T):**
- `ipodui-p8b-silent` updated for T6: a USB session whose update builds a candidate logs **no** empty window while the old library stays listed; a first build still logs one.
- `ipodui-p8b-gallery` (exists) regenerated from the kit.
- `ipodui-p8b-shadow` (new): pixel checks at x = 160–165 for a whole drift pan step, **a half step**, **the drift cover fade at mid-fade and at its final step**, a highlight fade mid and final, and a Settings tile; the seam's colour equals `cover × ui_shadow_a[c]` in every case (constant, never doubled, never faded out).
  - **Decision (8b end review, 2026-10-20):** the script is added at 8b close by the tooling agent (not waived), with the source grep for `xfade_rect(` / `FBADDR(UI_PANEL_X` outside the contract functions in the gate; its checks use the settled 10 px / 36 % shadow (x = 160–169), and `tests/ui/ipodui-p8b-main.script`'s comment is corrected from 160–165.
- Unchanged and still required: `ipodui-p8b-pin`, `ipodui-p8b-footer`, `ipodui-p8b-rowart-latency`, `ipodui-p8b-pan-cost`.
- The p8a unlit reference without "needs sign-off".

**Goldens:** one recapture after the owner approves 8b-2 (selection bar, header), one after 8b-3 (panels). Proposed by agents, written by `golden_capture.sh` only.

**Review media (opened for the owner):** per-screen side-by-sides under `out/review/p8b/` against `docs/reference/7g/owner/labeled/` (main menu, Music, Artists, Albums, Songs, Audiobooks, Settings × 17 tiles, Date & Time, About × 3, Now Playing, USB, unlit face) with diff notes; the style gallery sheet; before/after transition videos (`make_video.sh`) next to the C1 slow-motion reference.

**Device checks** (added to ROADMAP → Device checklist; one owner session after 8b-3, plus check 8 right after 8b-T):
1. Slides, panel fades and drift (including a cover change) on the 5.5G: no smear or tearing, and the seam shadow steady through whole and half steps, in a slow-motion video; F's frames-per-slide figure.
2. Selection bar, header glyphs, battery (charging and not), tiles and calendar under room light next to the 7G (C4 photos).
3. Hold on/off in a list and in Now Playing with the O3 constants.
4. `panel compose us=` figures from a devmode build (tile, drift, fade, half step).
5. **Thumbnails and panning** (owner reports 2026-10-11): spin Albums and Audiobooks at slow, medium and fast speeds; read the row-art read/decode/draw/upload split and the per-step drift timing and jitter from `ipodui.log`, before and after the shadow. Numbers to D/F and #60.
6. The footer: never seen after an ordinary USB session with music added (O12).
7. The open-handle high-water mark after a normal day (T4's input).
8. **After 8b-T:** Settings, Search, About, the USB screen and the Music hold menu feel the same as before; the **time to the first USB-screen frame** after plugging in within 10 % of today (`usbscreen.c` in T1a); for each T1b file taken, its timing from §4.T within 5 %.

**Numbers reported at close:** binary and `_end` deltas per sub-phase (8b-T's per candidate) and the largest pairwise variant; compose, shadow, fade and half-step instruction counts; device push rate and frames per slide; stock `.text` identity.

---

## Revision 3: changes and review findings

| Change or finding | Resolution | § |
|---|---|---|
| R3-1 the brief said "no code exists"; 8b-0, 8b-1a/1b, p8b-smooth and #74 are merged | §0.0 records each with its commit and files; §3.2/3.3/4.0/4.1 shortened to what remains; O6, O10 marked done | 0.0, 0.1, 3.2, 4.0 |
| R3-2 every file:line re-checked on `main` `11b8a83945` | `tagcache.c`, `library.c`, `preview.c`, `usbscreen.c`, `nowplaying.c`, `header.c`, `panels.c`, `ui_main.c`, `nav_core.*`, `main.c`, `usb_screen.c`, `misc.c`, `settings_list.c`, `validate.sh` updated; `checklist.c` path is `screens/`; 16 gloss descriptors, not 20 | all |
| R3-3 `_end` has 624 B left; "trim, don't raise" | New 8b-T with measured candidates (T1a safe-cold Thumb −11,394 B after 3a, T1b timing-gated −18,396 B after 3a, T2 developer-UI Thumb −9,388 B, T3 2,121 B, T4 measure-first, T5 not taken by owner rule) and gates on `_end` (≤ 1,072,000 after 8b-T, ≤ 1,096,000 at close) and `rockbox.ipod` (≤ 820,000 at close); the 752,000 B gate retired | 4.T, 6, 7 |
| R3-4 #85: lists no longer go empty during an update | §1 fact with file:line (two contexts, publication swap, `lib_state()`); O12 with the default "footer only where lists are truly empty, no code change" | 0.1, 1 |
| R3-5 the empty-window stamp misreports under generations | T6: stamp only when the active context empties or has nothing published; `ipodui-p8b-silent` updated | 1, 4.T, 7 |
| R3-6 `half_window()` writes the panel directly (p8b-smooth) | Added to the writer list, shadowed on columns 0–5, in the gate grep's allow list and in `ipodui-p8b-shadow`; CPU line doubled for half steps | 1, 3.1, 4.3, 6, 7 |
| R3-7 the scrollbar items are done (#74) | Removed from 8b-2; `list.c` only for selection colours | 0.0, 1, 4.2, 5 |
| R3-8 owner's newer direction (paused is normal, hidden maintenance, #86) | Out of scope for 8b, recorded in §2 and as O12's re-check trigger | 0.1, 2 |

### Revision 3 brief review (budget-reviewer checklist, 2026-10-19): 1 High, 3 Medium, 3 Low, all applied

| Finding | Resolution | § |
|---|---|---|
| **H1** the first cold-Thumb list held per-frame row callbacks (audiobooks, playlists, otglist, checklist, search) and bulk loops (search matching, shuffle, journal replay, art writes) and `sortkey.c`, which runs inside the commit's sort (scan speed is an owner priority) | Split: T1a safe-cold (−15,602 B) on by default; T1b (−14,188 B) per file only with a < 5 % timing proof; `sortkey.c` never; the list printed by the gate | 4.T, 6.3, 6.13 |
| **M1** T6's first guard (`ui_build == ui_active`) would hide the first build's real window, whose candidate is a different context | Also stamp when the active context has no count | 4.T |
| **M2** the size map's `devmode-stock-ui` subsystem includes `apps/recorder/*` (JPEG decode and resize, the art builder's bulk path) | T2 limited to `debug_menu.c`, `screens.c` and the developer-only `apps/gui/` files, each proven unreachable from the shell; measured −9,388 B | 4.T |
| **M3** the ≥ 32 KB target was not reachable from safe candidates alone | Target 27 KB (T1a + T2 + T3, measured); 8b-2 + 8b-3 limited to 24 KB by the close gate, with T1b or a 30 KB mask cap as the fallback | 4.T, 6 |
| L1 the boot window's visibility claim | Reworded: the shell never starts on a Music list | 0.1 |
| L2 `apps/ipodui/ipodui.make` does not exist | The flag list goes in `tools/root.make` beside `core_secflags` (:521–526) | 4.T, 5, 6.13 |
| L3 the panic precedent's ledger section | §2.14 | 4.T |

### Revision 3a, budget-reviewer review of 58a87d2b1c (2026-10-19): no High, 2 Medium, 3 Low, all applied

| Finding | Resolution | § |
|---|---|---|
| **M1** `_end` moves in 4 KiB steps (`inquiry_static` page-aligned at `0x000ff000`); 1,072,000 B needs a **linked** saving ≥ 26,468 B, and 27,111 B was a `.o` figure; 8b-2/3 room is 20–24 KB, not 24 | Step stated in §4.T and §6; gate = linked saving ≥ 26.5 KB or `_end` ≤ 1,072,000 B; T1b named as the ready reserve | 4.T, 6 |
| **M2** T1a broke its own rule: `powerlog_core.c` and `batcal_core.c` run in the tick interrupt, `otg.c` passes over the filename tag file, `usbscreen.c` blends 12 frames per pixel on every plug | `otg.c`, `powerlog_core.c`, `batcal_core.c` moved to T1b; `usbscreen.c` kept with a "time to first USB frame" device check; T1a −11,394 B, T1b −18,396 B | 4.T, 6.3, 7 |
| L3 `validate.sh` enforces only `P9_END_MAX` and 914,813 B and prints "target 760,000"; `hardware.md` :89 keeps 760,000 | G's 8b-T work items: `P8B_T_END_MAX`, `P8B_END_MAX`, `P8B_IPOD_MAX` in `validate.sh`; `hardware.md` :89 updated | 4.T |
| L4 `splash.c` does have callers outside the developer UI (playback, playlist, tagcache) | T2's proof reworded: every non-developer caller exits at the suppression funnel (`splash.c` :382–392) | 4.T |
| L5 T3 puts 2.1 KB on the stack of every thread that splashes | Measured stack peaks required for the UI, tagcache and playback threads | 4.T |

## Revisions 1–2: review findings → resolution (history)

| Finding | Resolution | § (Revision 2 numbering) |
|---|---|---|
| H1 silent library leaves Music empty and unexplained; assertion can't fail | Per-state table of what the user sees; strip removed; Search never claims "No Search Results" when it couldn't search; window measured at the producer; scripts assert list contents | 0.1, 1, 3.3, 4.1, 6.10, 7 |
| H2 drift cover fade bypasses compose, shadow fades and snaps | `ui_panel_compose()` + shadow-aware `ui_panel_fade()` as the contract, replacing every `draw_key`/`xfade_rect` site; drift fade mid and final in `ipodui-p8b-shadow` | 1, 3.1, 4.3, 7 |
| H3 "newest stamp" picks the wrong book (seeds, 30 s lag) | Ranking: playing book → pending record → highest unseeded `ab_rec_ok()` stamp; else no pin | 0.1, 1, 3.2, 6.13, 7 |
| M1 scan path zeroes keys | Keys filled on the scan path for unfiltered books views | 1, 4.1, 7 |
| M2 global generation reopens every view | `lib_pin_generation()`, books-only | 3.2, 4.1, 5, 7 |
| M3 translucent scrollbar double-blends | `draw_scrollbar()` takes the repainted band (done differently by #74: blended only over fresh pixels) | 1, 4.2, 7 |
| M4 "no read-back" untrue | §4.5 restated: read-back confined to compose/fade/scrollbar | 4.5 |
| M5 size over 760,000 B | Gate ≤ 752,000 B per sub-phase (retired in Revision 3, replaced by the `_end` gates) | 4.3, 6, 7 |
| M6 `lib busy end` unreliable | Producer tick stamps in `tagcache.c` (`UI_COMMIT`) | 3.3, 4.1 |
| M7 NP gel test can't fail; `np_flat_refl_a` unowned; heights | Frozen golden committed with the contract; `np_draw.{c,h}` to D; heights 14 / 15 / 19 | 3.1, 5 |
| L1–L6 | qscroll letter via `lib_letter()`; key stride; scratch sizes; no `ICODE_ATTR`; tile colouring cost; paths | 1, 3.1, 3.2, 4.1, 4.3, 5, 6.3 |
| R2-1 … R2-9 | Stamp owner and sites; scan-path key from RAM; browse empty row as new code; Goal 6 wording; size base; `lcd-video.c` via #60 and the ~19 fps fact; owner reports as 8b-0 traces; line drift | 3.3, 4.0, 4.1, 4.3, 4.4, 6 |
