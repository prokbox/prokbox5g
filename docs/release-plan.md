# P20 release plan: ProkBox v1.0 (brief draft)

*Draft 2026-10-24, for the owner round. Phase 20 (Release & install), milestone "P20 Release & install", umbrella issue #43. Owner, 2026-10-24: "by mid-November we'll have all the salient stuff ironed out", and "I want to get this into the hands of the community that would use it."*

**The test case is the owner's parents.** They should be able to install ProkBox (or have it installed for them), drag music on, play an audiobook, and get back to Apple's firmware if something goes wrong, without reading a forum thread.

**Read with:** `docs/install-and-restore.md` (install methods and restore routes), `docs/storage-choices.md` (one build for flash and HDD), `docs/hardware.md` (configuration dimensions), `docs/upstream-backports.md`, and issues #43, #98, #54, #102, #122, #123.

---

## 1. What v1.0 is

### 1.1 Must-have features (all exist today; v1.0 makes them release quality)

| Area | In v1.0 | Phase |
|---|---|---|
| Shell | 7G-style split-screen menus, smooth lists, header, Settings (stock 7G set only), About, Search, hold-SELECT menu, On-The-Go | 2–5, 11, 8b |
| Music | Browse by the database, rich rows and art, Shuffle Songs, playlists, podcasts | 4, 4b, 11 |
| Now Playing | 7G Now Playing, SELECT cycle, ratings, pre-rendered art and reflection | 5, 7a |
| Audiobooks | Chapters (m4b, MP3, cue), per-book resume, 0.5–2× speed, chapter art, last book pinned | 6, 8b |
| Codecs | Every playback codec Rockbox ships | — |
| USB | Drag-and-drop music on and off over USB mass storage; library updates hidden after eject | 8a, 16, #81 |
| DAC | EQ presets on the WM8758, Volume Limit, fades, the volume curve | 15 |
| Library | Silent updates, safe writes, maintenance while paused | 16 |
| Extras | Clocks, Alarms, Sleep Timer, Stopwatch, Screen Lock, Notes, Contacts, Calendars, Games | 12 |
| Power | Dark playback on one core (19-1), boot battery guard, LCD sleep, the unlit clock face | 19, 16, 8a |
| System | Our boot screen, our USB screen, no visible Rockbox, a first-run notice (#102), a user-saved diagnostics report (§6.2) | 8a, 20 |

### 1.2 Quality bar

| Severity | Meaning | v1.0 allows |
|---|---|---|
| S0 | Data loss on the iPod or the computer, a unit that won't boot to anything, an install that can't be undone | none |
| S1 | Crash or panic in a core flow (browse, play, audiobook, USB), library lost after USB, the iPod won't mount on one of the three OSes, battery shutdown wrong | none open |
| S2 | Wrong but recoverable behaviour; visible fidelity faults in main screens | listed in Known issues |
| S3 | Polish, rare edge cases | listed or tracked |

Plus: `scripts/validate.sh` prints `GATE PASSED` on the release commit; the `budget-reviewer` agent passes the release diff; the release scans of §6.3 pass.

### 1.3 Deferred (hidden, not removed)

| Item | Where it goes | Why |
|---|---|---|
| Videos (basic) and Photos (P14) | Videos in the first 0.8.x, Photos in a later 0.8.x (§1.3a) | H.264 on the BCM2722 still has open file faults (S28); Photos not started. Photos stays hidden until a later 0.8.x |
| Cover Flow (P7) | **1.0 finish line** (0.9 for testing) | Needs 19-7 foundations; hidden in 0.8.x. Ships as **"Album Flow"** in public builds (owner, #43) |
| UI on the COP (19-9) | **first 0.8.x (gate)** (owner, 2026-10-26) | Default on in 0.8.0 once bench 4's 19-9 rows (ucop, uck1, uck2, ufeel, upanel, ucop2 with underruns=0, offcore=0) pass; until then the inline UI stays the default. The release build carries the split since `p19-relsplit` (2026-10-26); the flip is one line, `IPODUI_UI_CORE_DEFAULT` 0 → 2 in `firmware/export/config/ipodvideo-variant.h` (`docs/design/p19-ui-on-cop.md` "The switch") |
| 19-3 to 19-12 (other than 19-9) | v1.1+ | v1.0 needs only 19-1 and 19-2 |
| Now Playing visualizer (#126) | **first 0.8.x** (owner, 2026-10-26) | Done on p19: full-screen, dark, five styles drawn on the VC; 19-2 is already in. Gate: its UI scripts pass in the release build, and it plays cleanly with the UI on the second core (the viz tap is CPU-side since ucop-fix2) |
| **Smart Mixes** (P13) | after 1.0 (`docs/launch-story.md` §4) | Not started. The public name is "Smart Mixes"; "Genius" is Apple's and is never used |
| Languages beyond English (P22) | v1.2+ | Accented Latin already ships; v1.0 is English only (COMP-6) |
| Sleep and instant resume (#86, P18) | **1.0 finish line** (0.9 for testing) | Not started; waits (owner, 2026-10-26) until the awake core and VC topology is locked. 0.8.x powers off and boots as today (see Q15) |
| Native install, our own Disk Mode (#122, #62) | v2 | Needs our own recovery proven first (bootloader guardrails) |
| Cluster reformat, native macOS mount for 1 TB mods (#81, P17) | v1.1 | See risk R4 |
| Final trim (P9b), retiring the Rockbox developer UI (#103) | v1.1 | Developer menus are already reachable only by the hidden combo |
| Companion app (P21) | later | Drag-and-drop stays first-class |

### 1.3a Pre-release stages (0.8.x)

*Owner decisions, 2026-10-26.* The pre-release is `0.8.x` (private, then public); `1.0.0` is GA; "beta" (`1.N.0-beta.M`) is only for new features after 1.0 (Q20). The pre-release comes in two steps. §1.1 stays the full v1.0 list; the 0.8.x releases ship slices of it.

| Stage | Adds | Who it is for |
|---|---|---|
| **First 0.8.x** (first pre-release) | Music, audiobooks, USB drag-and-drop and basic video playback | Anyone with a 5G/5.5G who wants those three things |
| **Later 0.8.x** | Photos, the stock HDD run and video polish | The same people, once more hardware is covered |
| **0.9** *(proposed; owner to confirm)* | The finish-line features arrive for testing: Album Flow, sleep and hibernation, the games SDK (class B and C) | The 0.8.x testers |
| **1.0** | Everything in §1.1 plus the finish line (`docs/launch-story.md` §3), fixed up from pre-release feedback | General release |

#### First 0.8.x: music, audiobooks, USB drag-and-drop, basic video

Cut from, or during, the owner's week-long daily-use trial of the release build.

- **Ships:** everything in §1.1 as the release build (`--ipodui-release`, no developer menu), plus **Videos (basic)**: it plays converter-made files, shows the styled video error screen, and comes with the converter guide.
- **Hidden:** Photos.
- **Gate items** (all must be true before the cut):
  1. The USB-plug-while-paused out-of-memory panic is fixed (S1; branch `p19-usboom`, in progress).
  2. The trademark and legal rename pass is done and `scripts/legal_audit.py` runs clean (§7.4).
  3. The first-run notice (#102) and the user-saved diagnostics report (§6.2) both work in the release build.
  4. `docs/install-and-restore.md` has been checked end to end.
  5. A known-hardware note is published: tested on a flash 5.5G with 64 MB; HDD and 32 MB testers are welcome.
  6. Smooth playback of converter-made files on the verified VC image.
  7. A first-use VC self-check. Without it, units with no `clock150` token play video at 27 MHz, which is laggy (the owner saw this in sitting 1); with it, a chip that can't hold the 150 MHz video clock stays at a safe clock. The token is earned today only on the owner's unit.
  8. The styled video error screen and the converter guide for users.
- A stock HDD unit is not required for the first 0.8.x. Testers with stock 5Gs give that coverage.
- Burned-in subtitles made by the converter cover the gap until soft subtitles exist.

#### Later 0.8.x: Photos, the HDD run, video polish

- **Gate items:**
  1. A simple Photos viewer for JPEGs copied over USB, using the VC's JPEG decode.
  2. At least one stock HDD, 32 MB 5G taken through install, use and restore.
  3. Video polish: more decoder headroom, and on-device fit modes if any.

#### 1.0

Follows feedback from the 0.8.x pre-releases. It meets §1.2's quality bar, plus a one-week daily-use trial of the release build with no S0 or S1 open.

**Finish line (owner, 2026-10-26; `docs/launch-story.md` §3):** 1.0 also requires Album Flow, sleep and hibernation, the games SDK (class B and C, with the signing and allow-list policy), tested public PP5022 and BCM2722 references, and complete ProkBox5g user and technical docs. Sleep and hibernation start only once the awake topology (UI core placement, VC residency) is locked down. These move out of §1.3's deferred list.

#### Excluded from 0.8.x (hidden or off)

- Soft sleep and hibernation (wait for the awake core and VC topology to be locked).
- Soft subtitles.
- Cover Flow ("Album Flow").
- PP governor stage 2 (DVS).

**Also a gate item (owner, 2026-10-26):** the UI on the second core (19-9), default on, once its bench-4 rows pass. Releases ship release builds only; developer builds are never published (anyone can build one from source), so retiring the Rockbox developer UI (#103) is not a release blocker.

#### Known issues in 0.8.0

- A video played before the unit's one-off video check has passed (the check's test file missing, no memory for it, or an inconclusive result) runs the video chip at 150 MHz unchecked, Apple's stock clock; a decode fault during the play steps the clock down and the next play checks again (`docs/research/bcm2722/first-use-check.md`, "Unchecked plays").
- Videos not made with the converter may play less smoothly. (A 320×240 H.264 stream with 4 reference frames keeps its frames in the BCM's SDRAM instead of its SRAM: 622,016 B needed vs a 617,472 B arena. Fixed in the next merged image with batch 5. The converter encodes 3 references.)

#### Open work for the first 0.8.x

Nobody owns these yet:

- **Diagnostics report:** Settings › About › Save Diagnostics (§6.2), built and working in the release build.
- **Legal rename pass:** the trademark and legal sweep (§7.4), ending with a clean `scripts/legal_audit.py`.

### 1.4 Supported hardware

| Model | Apple part numbers | Capacity | RAM | Storage | v1.0 status |
|---|---|---|---|---|---|
| iPod Video 5G | MA002, MA146 | 30 GB | 32 MB | HDD | supported, beta-verified required |
| iPod Video 5G | MA003, MA147 | 60 GB | 64 MB | HDD | supported, beta-verified required |
| iPod Video 5.5G | MA444, MA446 | 30 GB | 32 MB | HDD | supported, beta-verified required |
| iPod Video 5.5G | MA448, MA450 | 80 GB | 64 MB | HDD | supported, beta-verified required |
| Any of the above | — | any | 32/64 MB | flash adapter (iFlash-style, CF, SD, mSATA) | supported; the owner's unit (MA450, 1 TB flash) is the reference |

| Dimension | v1.0 handling |
|---|---|
| RAM 32/64 MB | detected at runtime (`probed_ramsize`); identical behaviour (memory floors in memory) |
| HDD / flash | a build axis: `flash` and `hdd` zips (part of the 8-zip matrix (owner, 2026-10-26): `ProkBox5g-<ver>-{stock,modded}-{flash,hdd}-{32,64}MB.zip`, chosen by the user from the guide's three questions (0.8.0 has no installer app: zip plus written steps)); runtime detection (#79) is after 1.0 |
| Battery stock / large | a build axis: `stock` (stock cell, upstream table) or `modded` (3000 mAh); the stock-cell table must exist (exit E10) |
| Dock connector present / removed (#54) | a build axis: `stock` has the 30-pin dock (`--dock30`), `modded` has USB-C; picks the USB screen glyph |
| Haptic mod / piezo | a build axis: `modded` has haptics, `stock` the piezo |
| Mac-format (HFS+) iPods | must be converted to FAT32 (erases the iPod); the guide's install steps say so before anything is copied |

### 1.5 Exit list: "the salient stuff ironed out"

Each item is checkable. Mid-November (RC1, Nov 13) means every item is ticked or has an owner-approved waiver in Known issues.

| # | Item | Check |
|---|---|---|
| E1 | P12 merged to `main` | gate passed on `main` after the merge |
| E2 | P19 19-1 (dark playback) merged | M-1/M-2 recorded; 0 underruns; `lcd wakes dark=0` |
| E3 | P19 19-2 (L2, the drift on the BCM2722) merged, with the release image in `bcmfw/verified.txt` | stages 0–5 pass on the device; or L2 default off with the CPU path (Q10) |
| E4 | One release build for flash and HDD (#79), or the two-zip decision | the HDD build boots and plays on at least one stock HDD unit |
| E5 | Drag-and-drop works on a stock-layout unit | macOS mounts with no helper; copy on and off on macOS, Windows, Linux; 10 connect/eject cycles each |
| E6 | Native illusion holds in the release zip | string scan: no "Rockbox", no hidden folder name on screen; default title ProkBox; Cover Flow hidden |
| E7 | No Apple assets, licences complete | asset scan; `LICENSES/`, `CREDITS`, font licences, corresponding source attached |
| E8 | Install, update and uninstall rehearsed on a spare unit | macOS required; Windows and Linux before GA |
| E9 | Recovery verified on a spare unit | Hold MENU to Apple's firmware; where Disk Mode lives is recorded in `docs/pp5022-reference/` |
| E10 | Stock-battery behaviour | percent, low-battery shutdown and the boot guard on a stock cell |
| E11 | Diagnostics report | a non-technical user can save one and attach it (§6.2) |
| E12 | First-run notice (#102) | shown once after install and after each update |
| E13 | Docs live | quick-start, install guides for 3 OSes, FAQ, Known issues, compatibility table |
| E14 | Device soak | 7 days of the owner's daily use on the RC, no panic record; overnight dark rundown; 100 USB cycles |
| E15 | Bug bar | no open S0/S1 |

---

## 2. Timeline (2026-10-24 to mid-November, then GA)

**Recommendation:** mid-November is **RC1 plus a public beta**, not GA. GA follows two weeks of beta feedback from HDD and 32 MB units, **target Mon 2026-11-30**. Nobody on the team has run ProkBox on a hard drive yet; shipping 1.0 to the public before that is the biggest avoidable risk.

| Week | Dates | Must land | Release work | Milestone |
|---|---|---|---|---|
| 0 | Sat 10-24 – Sun 10-25 | — | Owner answers §9; order a spare 5G/5.5G HDD unit (Q2) | plan agreed |
| 1 | Mon 10-26 – Sun 11-01 | E1 P12 merge; E2 19-1 merge and M-1/M-2; E3 19-2 device round; E4 #79 or the two-zip call | Release packaging (`make zip` → `ProkBox5g-0.8.N.zip`, version string, checksums); release scans (§6.3); history and docs scrub for the public repo (§3); installer spike on the spare unit; recovery checks E9 | **Feature freeze Mon 11-02, 09:00** |
| 2 | Mon 11-02 – Sun 11-08 | Fixes only; E5, E10, E11, E12 | Beta 1 (private): the owner's unit, the parents' unit, 3–5 invited testers. Quick-start and install guides; screenshots from the sim; public repo created (private until beta 2); trademark search | **Beta 1 Wed 11-04** |
| 3 | Mon 11-09 – Sun 11-15 | Fixes from beta 1; E13, E14 started | Public repo and Pages site go live; soft public beta on r/ipod and the Rockbox forums (§7.1); RC1 cut | **Beta 2 (public) Wed 11-11; RC1 Fri 11-13; exit review Mon 11-16** |
| 4–5 | Mon 11-16 – Sun 11-29 | S0/S1 fixes from public beta; E8 Windows and Linux | Soak finishes; demo video; FAQ from real questions; announcement kit (§7.3) | RC2 as needed |
| 6 | Mon 11-30 | — | **v1.0.0 GA**; wider push (§7.2) | GA |

**Can slip to v1.1 without blocking:** Cover Flow, 19-3 to 19-12 (other than 19-9, the UI on the COP, which is a 0.8.0 gate item), the W1-B alarm window in the bootloader, a graphical installer, the PDF guide, Windows code signing, P17's cluster reformat.

**Cannot slip (blocks RC1):** E1, E2, E4, E5, E6, E7, E9, E15.

### Risks

| # | Risk | Likelihood | Effect | Mitigation |
|---|---|---|---|---|
| R1 | No HDD unit tested before public beta | high today | S1 bugs on most users' units (spin-up, buffer watermarks, power-off) | Buy a spare HDD unit now (Q2); beta 1 includes it; `docs/storage-choices.md` rows marked "open" checked first |
| R2 | P12 and P19 both unmerged; two big merges in week 1 | medium | Gate failures eat week 1 | Merge P12 first (its gate already passed); 19-1 next; 19-2 only if its device round passes, else L2 default off |
| R3 | L2 (our BCM2722 image) verified on one unit only | medium | Blank screen on other units | `bcmfw/verified.txt` gate; fallback to the stock display path on any failure; default off unless a second unit passes (Q10) |
| R4 | macOS mount (#81) only proven with the helper on the 1 TB card | medium | Drag-and-drop fails on the most important OS | Test stock-layout units early (cluster sizes there differ); for large flash mods, a documented reformat step or a known issue |
| R5 | Installer and recovery never run on a non-owner unit | high today | Bricked-looking units, lost trust | Spare unit; side-by-side only (Apple's firmware kept); ipodpatcher's proven path; no bootloader feature changes in v1.0 |
| R6 | Stock 15-to-20-year-old batteries | high | Wrong percent, sudden shutdowns | Stock-cell table, conservative low-battery floor, FAQ entry |
| R7 | Unsigned binaries | high | Gatekeeper and SmartScreen warnings scare non-technical users | Apple notarisation (Q7); illustrated "Open anyway" steps |
| R8 | Owner time: device rounds, owner questions, beta support at once | high | Slips | Batch owner questions once (§9); agents prepare everything up to the device step |
| R9 | Legal: trade dress, trademarks | low–medium | Takedown | Name check, disclaimers, Album Flow, no Apple assets, no Apple marks in names, takedown response (`docs/legal.md` L14); no IP consult (owner, 2026-10-24) |

---

## 3. Public home

### 3.1 Where

**Recommendation: GitHub as the public home, with a Codeberg push mirror.**

| | GitHub | Codeberg |
|---|---|---|
| Audience reach (r/ipod, YouTube modders, HN) | best: everyone has an account | smaller |
| Releases with assets and checksums | yes | yes |
| CI for the build matrix (ARM toolchain) | Actions, free for public repos | Woodpecker, more limited |
| Docs site | GitHub Pages | Codeberg Pages |
| Fit with GPL and Rockbox | fine (Rockbox mirrors on GitHub) | strong (non-profit, FOSS) |

**Created (owner, 2026-10-26):** org `prokbox` (umbrella for every ProkBox project: this build, later models, the hardware-reference repos), repo `prokbox/prokbox5g`, Pages at `https://prokbox.github.io/prokbox5g/` (Actions source). Issue forms ship in this repo's `.github/ISSUE_TEMPLATE/`; workflows in `.github/workflows/`.

The private Forgejo stays the development tracker for now. Public GitHub Issues take user reports, with templates (§6.1). The `forge` remote keeps the full private history.

### 3.2 What gets published

**Recommendation (Q4): upstream Rockbox history intact up to our merge-base, then one labelled strip commit ("strip: remove code ProkBox5g never compiles", deletions only), then ProkBox as one import commit (or a few per-subsystem commits) on top, then normal public development.** The public repo is `prokbox5g`. Only the public tree is stripped; the private repo keeps the full upstream tree (owner, 2026-10-24). `scripts/release_tree.py` builds it, and `docs/legal.md` §4 has the rules.

Reasons:
- About 2,140 commits refer to private tracker numbers (#N) that will mean nothing in public, and to private hosts and paths.
- GPL needs the complete corresponding source, not the history.
- Upstream credit stays visible because Rockbox's own history is kept.
- A full-history rewrite (`git filter-repo`) is possible later if the owner wants it; a squash can't be undone in public, so ask now.

**Scrub before the first public push** (a script, so every later sync repeats it):

| What | Action |
|---|---|
| Private hosts and paths (`the private tracker`, NAS and LAN host names, `/Users/...`, backup paths) | replace with neutral wording |
| The owner's unit serial and family devices | remove |
| Tracker references (#N) in docs | keep, with one note that they refer to the development tracker; or map the open ones to public issues |
| Apple reference material | confirm none is in the tree (`docs/reference/*/` is gitignored); scan for Apple images, fonts, firmware bytes |
| Secrets | run a secret scanner (gitleaks or trufflehog) on the published tree |
| `CLAUDE.md`, `.claude/` | **excluded** from the public tree (DROP, owner rules 2026-10-26: they name the private tracker and local paths); the AI-assistance note (Q5) goes in the README instead |
| Test footage | the Sintel clip stays developer-only and keeps its CC BY 3.0 credit |
| Owner media (owner rules, 2026-10-26) | no copyrighted content or private data from the owner's iPod in the repo, even privately; art caches and database files are never committed unless synthetic; screenshots and goldens use open or synthetic media only. `scripts/check_no_owner_media.py` (a gate step) fails when the repository tracks `*.tcd`, `database_*`, art cache files, `*.pak`, `*.log` or `bench/*.txt` outside the synthetic fixtures, or `config.cfg` outside `packaging/` and `tests/ui/fixtures/` |
| **DROP** (added 2026-10-26; never exported) | `docs/reference/**`, `.claude/**`, `CLAUDE.md`, `scripts/forgejo.py`, `scripts/gel_sheet.py`, `tests/review/**`, `docs/handoff-*.md`, `docs/owner-questions.md`, `bcmfw/**/*.mp4` and `*.m4v`. `.github/`, `site/` and `scripts/release_compare.py` are exported, not dropped |
| **SCRUB, fail-closed** (added 2026-10-26; a hit in the exported tree aborts `release_tree.py` with exit 2) | `beelink`, `MC` + 3 digits + 2 letters (model numbers), `IMG_` + 4 digits (photo file names), `ipod-backups`, the private tracker repo name, `/Users/`, and the owner's surname (case-insensitive; "James Caldwell", an upstream credit, passes). The files that define these patterns are exempt |
| Copyright holder (owner, 2026-10-26) | in the exported copy "prokopto-dev" becomes "prokopto-dev" in every ProkBox header (and anywhere else the exact string occurs); the private tree is unchanged. Self-test: `tests/release_tree/test_scrub.py` |

### 3.3 Releases

- Tag `v0.8.0` (the **pre-release**; fixes `v0.8.1`, …), then `v1.0.0`. Semantic versions. **Owner, 2026-10-26:** 0.8 is called the *pre-release*, not a beta; "beta" is kept for new features after 1.0 (`v1.N.0-beta.M`). Where this plan says beta 1 / public beta / RC before 1.0, read pre-release 0.8.x.
- Assets per release: `ProkBox5g-<ver>.zip` (firmware), `ProkBox5g-Installer-<ver>-<os>` (§4), `ProkBox5g-<ver>-source.tar.gz` (corresponding source, including the BCM2722 image sources and build scripts), `SHA256SUMS`, and a signature (minisign or GPG) if the owner wants one.
- Release notes in plain language: what's new, Known issues, how to update, and the short disclaimer from `docs/legal.md` §2. No Apple mark in any asset name or release title.
- Binaries and the source tarball are published together on the same release (GPL v2 §3(a)), never binaries alone.
- CI builds every release asset from the tag; the full gate runs locally (it needs the simulator pool). **`.github/workflows/release.yml`** (2026-10-26): a `v*` tag builds eight zips, `ProkBox5g-<ver>-{stock,modded}-{flash,hdd}-{64,32}MB.zip` (owner, 2026-10-26: *stock* = stock battery, 30-pin dock, no haptics; *modded* = the owner's mods, 3000 mAh, haptics, USB-C; each for `--storage=flash` and `--storage=hdd`; runtime storage detection, #79, is a later roadmap item, not a 0.8.0 gate), one Linux job per RAM size inside the pinned release toolchain container (**owner 2026-10-26:** `ci/toolchain/Dockerfile`: GCC 16.2.0 and binutils 2.47 built from GNU tarballs with SHA-256 pins and good GNU signatures, configured like Homebrew's arm-none-eabi-gcc 16 that local Mac builds use; host tools from Debian snapshot `20261020T000000Z` on a digest-pinned base. **CI never builds GCC (owner, 2026-10-28):** the image is built once on the owner's Linux host and pushed as a **private** package, `ghcr.io/prokbox/prokbox5g-toolchain` (with `prokbox5g-docs`), linked to `prokbox/prokbox5g`; `release.yml` and `pages.yml` pull it by digest. A private package isn't distribution. Rebuild, re-push and update the digests only when a Dockerfile changes. The zips come from `scripts/release_build.sh`, the same script on GitHub and locally), checks that About says `ProkBox5g <ver>`, that the source-address placeholder is gone and that no developer strings remain, then drafts one release with all eight zips, `ProkBox5g-<ver>-source.tar.gz` and `SHA256SUMS` (0.x tags marked pre-release; notes from `docs/user/releases/<ver>.md`, the same page the site shows, plus the short disclaimer; and `ProkBox5g-<ver>-User-Guide.pdf`, printed from the same source as the site). Proven locally: the job's build loop for 32 MB (all four zips), and modded-64 (`flash-64-3000-usbc-haptic`, the owner's unit) release builds with `VERSION=0.8.0` embed the version and have no developer strings. **Release gate and staging (owner, 2026-10-26).** Code and release go public together, only after the exact zip GitHub built has passed the gate:
  1. `scripts/release_tree.py` exports the scrubbed public tree. It is pushed with the tag to the **private** `prokbox/prokbox5g-staging`, whose `release.yml` drafts the release.
  2. `scripts/release_compare.py <draft-dir> <local-dir>` compares the draft's zips with a local build of the same commit, file by file (zip containers differ in timestamps, so it compares contents; two clean local builds of one commit match). The local side runs `scripts/release_build.sh` in the same toolchain image on the owner's Linux host (`beelink`). **Proven 2026-10-26:** all 8 zips built in the image on beelink; a second container run MATCHes the first (4/4, 64 MB); and the Mac's Homebrew gcc 16.2 build MATCHes the container (4/4): the firmware is byte-identical, only the nested `voicestrings.zip` timestamps and `rockbox-info.txt`'s two Host lines differ, which the compare opens and ignores.
  3. The owner installs the staging zip on their iPod (fresh backup, the owner's go) and runs the ~10 min release smoke checklist.
  4. The same tree and tag are pushed to the public `prokbox/prokbox5g`. Its workflow drafts again; `release_compare.py` must report MATCH against the staging zips before the public draft is published.

  **Later (owner, 2026-10-26):** once the process has earned confidence, retire the staging repo. Test builds then run publicly on branches and pull requests in `prokbox/prokbox5g` (free minutes); their zips and results are pulled down as workflow artifacts for the device gate, and only the tag drafts a release.

  Actions minutes: about 2 × 15 Linux minutes per release (×1), well inside the free plan's 2,000 private minutes a month; a toolchain image rebuild costs ~45 once. Public-repo runs are free.

### 3.4 Docs site

**Built (2026-10-26):** one Markdown source, `docs/user/` (home, `guide/` chapters by weight, `releases/<ver>.md`, `images/`; conventions in `docs/user/README.md`), turned by `scripts/docs_build.sh` into the site (Hugo with our own layouts and stylesheet in `site/`, light and dark, phone layout, Pagefind search) and the A5 PDF guide (the guide's print layout, printed by Chromium from the same HTML and CSS, so they never drift). Everything builds in the pinned docs image `ci/docs/Dockerfile` (Hugo 0.168.0, Pagefind 1.3.0, Chromium from the Debian snapshot; built in the job and cached, never published), in CI (`pages.yml` deploys to `https://prokbox.github.io/prokbox5g/` with the PDF at `downloads/`; `release.yml` attaches the PDF) and on beelink for previews. UK spelling in user-facing text (owner, 2026-10-26). Pages: Home (pitch, screenshots), Install (per OS), Using ProkBox (the user guide), FAQ, Known issues, Compatibility, Recover or uninstall, Report a problem, Credits and licences.

### 3.5 Licensing and credits

The full compliance record (every obligation, how it is met, which check proves it) is `docs/legal.md`. This table is the summary.

| Item | Licence | Action |
|---|---|---|
| ProkBox code | GPL-2.0-or-later | ProkBox header on every new file (`docs/templates/prokbox-header.txt`) |
| Rockbox | GPL-2.0-or-later | upstream headers kept; `docs/CREDITS` kept; credit on the site, the About screen's Legal page and every announcement |
| Codec libraries under `lib/` | their own | notices kept in the release zip |
| TeX Gyre Heros ("ShellSans") | GUST Font License | licence text in the zip and the source |
| Source Sans 3 (fallback, if shipped) | SIL OFL 1.1 | licence text in the zip |
| User guide and site prose | CC BY-SA 4.0 recommended (Q19) | stated on the site |
| Apple | none | no Apple code, fonts, images or firmware in anything we ship; the installer never copies Apple's firmware |

---

## 4. Install and recovery

### 4.1 Install types

| | Apple-firmware-compatible (v1.0) | Native (v2, #122, #62) |
|---|---|---|
| Firmware partition | Apple's firmware kept; the ProkBox bootloader added in front of it (ipodpatcher's method) | ProkBox bootloader only |
| Data volume | FAT32 as it is | FAT32 with 32–64 KB clusters |
| Recovery | Hold MENU at power-on → Apple's firmware; Finder/iTunes Restore | our own Disk Mode (#122), then Finder/iTunes Restore |
| Ready | yes, after E8/E9 on a spare unit | no: needs our recovery proven first |

v1.0 ships **only** the Apple-firmware-compatible type. One firmware build will serve both later; it detects the install type at runtime.

### 4.2 Installer: recommendation for v1.0

**A small command-line installer per OS, launched by double-click, plus the plain zip for experienced users.** A graphical installer is v1.1.

Why not just the zip: the firmware folder starts with a dot, so Finder and Explorer hide it, and copying it by hand fails silently for exactly the users we want. Why not a GUI now: three weeks, three OSes, signing.

What the installer is: `ipodpatcher` (already cross-platform C in `utils/ipodpatcher`, used by Rockbox Utility for years) built with the ProkBox bootloader embedded, plus a file copier and a short question script. Builds: a macOS universal binary in a `.command`-launched app folder, a Windows `.exe` that asks for admin, a Linux static binary.

| Step | What the user sees (our words, drafted in the guide) |
|---|---|
| 1 | Plug in the iPod. The installer finds it, shows the model, capacity, RAM and format. |
| 2 | "Back up anything on this iPod you don't have elsewhere." Continue only after a typed `yes`. |
| 3 | If the iPod is Mac-formatted: "This iPod must be erased and reformatted first." Typed confirmation, then format to FAT32. |
| 4 | Three questions: larger battery fitted? 30-pin connector still there? haptic mod? (defaults: no / yes / no) |
| 5 | Saves a copy of the firmware partition to the computer (needed for uninstall), then adds the ProkBox bootloader and checks it by reading it back. |
| 6 | Copies the firmware files, writes the unit config, ejects. |
| 7 | "Your iPod will restart into ProkBox. To use Apple's software instead, hold MENU while it starts." |

Admin rights: macOS admin password, Windows UAC, Linux `sudo` or a udev rule. Each is explained before it is asked for.

**Bootloader rule.** The v1.0 bootloader is today's ProkBox side-by-side bootloader (Rockbox-derived, silent, Hold MENU → Apple's firmware), unchanged. The W1-B alarm window and any other bootloader change are **not** in v1.0 unless they complete every guardrail (fail-safe pure function, byte-reviewed diff, log-only stage on the device, recovery proven, the owner's explicit go). **Every bootloader install on any device the project touches needs the owner's go, a fresh backup of the firmware partition, and a read-back verify.**

### 4.3 Backups

| What | Who | How |
|---|---|---|
| Music on the iPod | the user | the guide says it first; drag-and-drop works both ways, so copying music off is the same step |
| Firmware partition | the installer | saved automatically next to the installer (`ProkBox-backup-<serial>-<date>.img`) |
| Settings and library | the firmware | kept across updates; nothing to do |

### 4.4 Recovery

| Problem | What to do | Erases music? |
|---|---|---|
| ProkBox misbehaves | Reset (hold MENU+SELECT), then hold MENU while it starts → Apple's firmware for that session | no |
| ProkBox won't start | Same; then connect to a computer, reinstall or update | no |
| Want Apple's firmware back for good | Installer → Uninstall (restores the saved firmware partition) | no |
| Nothing starts | Disk Mode (reset, then hold SELECT+PLAY), then Finder (macOS 10.15+) or iTunes/Apple Devices → Restore | yes |

E9 verifies Hold MENU and Disk Mode on a spare unit before RC1. Until then they are [hypothesis] for units other than the owner's.

### 4.5 Updating

- Installer: "Update" copies the new files; settings, library, play counts and resume points stay; the bootloader is untouched unless the release notes say otherwise.
- By hand: copy the zip's contents over (with hidden files shown).
- After the restart, the update notice (#102) says what changed.
- No on-device update checks in v1.0 (no network).

### 4.6 Uninstalling

Installer → Uninstall: restores the saved firmware partition (or removes the bootloader with ipodpatcher's own method if the backup is missing), deletes the ProkBox files, leaves music in place. Apple's firmware then runs on FAT32 and shows an empty library until it is synced. Finder/iTunes Restore remains the full reset.

---

## 5. Docs for release

### 5.1 v1.0 user docs

| Doc | Content | Source |
|---|---|---|
| Quick-start (1 page) | install, add music, play, audiobooks, recover | new |
| Install guides × 3 | macOS, Windows, Linux, with screenshots of each step | new; `install-and-restore.md` |
| User guide (web) | every menu and setting, Now Playing, audiobooks, Extras, USB | `docs/design/spec/`; sim screenshots from `scripts/shots.sh` (a `tests/ui/manual-*.script` set) |
| FAQ | safe? brick? back to Apple? which formats? battery? Rockbox Utility? | new |
| Known issues | from the tracker at each release | new |
| Compatibility table | §1.4, plus beta reports per model | new |
| Report a problem | the diagnostics report steps (§6.2) | new |

Everything in our own words; our screenshots only; the "not affiliated with Apple" notice on every page.

### 5.2 The seven manuals (#123)

| Manual | v1.0 | Later |
|---|---|---|
| 1. User Manual | web user guide (§5.1) | PDF guide in v1.1 (#98) |
| 2. Development Manual | "Build and contribute" page: build, layout, process, rules (from `CLAUDE.md`, `docs/process.md`) | full manual v1.2 |
| 3. PP5022 Processor Manual | publish `docs/pp5022-reference/` as a draft, rendered | edited manual v1.2 |
| 4. BCM2722 Processor Manual | publish `docs/bcm2722-reference/` as a draft after the provenance audit (#43 item 3) | edited manual v1.2 |
| 5. WM8758 DAC Manual | not in v1.0 (seed `docs/wm8758-reference/`) | v1.2 |
| 6. Game Development Guide | **1.0 finish line** (games SDK, class B and C) | draft in 0.9, once the P12 SDK is stable (`docs/design/sdk.md`) |
| 7. iPod Video 5.5G Technical Breakdown | not in v1.0 | v2 |

---

## 6. Testing for release

### 6.1 Beta programme

| | |
|---|---|
| Size | beta 1: owner, the parents' unit, 3–5 invited; beta 2: 10–15 public sign-ups |
| Intake | a GitHub issue template "Beta tester": model, capacity, storage (HDD or mod), battery, connector, OS |
| Coverage target | every row of §1.4 at least once; at least two HDD units, one 32 MB 5G, two stock batteries, one Windows and one Linux install |
| Ask of testers | back up first; install; one week of normal use; a short report (template) plus a diagnostics report on any fault |
| Templates | Bug, Install report, Hardware report |
| The parents' test | the owner installs for them (with a backup), then watches them use it without help; every question they ask goes into the FAQ or a fix |

### 6.2 Logs and crash reports without technical skill

Today `ipodui.log` and the panic record sit in the hidden firmware folder, which users can't see.

**Plan:** a **Settings › About › Save Diagnostics** item (our wording, through `LANG_*`), developer-free:
1. On select it writes `ProkBox Report.txt` to the top level of the iPod (visible in Finder and Explorer): version, model, RAM, storage, battery, the last panic record, and the tail of the log.
2. A short line tells the user to connect the iPod and attach the file to a report.
3. The report page on the site explains it with pictures. Email is offered as an alternative to a GitHub account.

Rules: written on user action only (never at USB entry, the owner's rule); no hidden-folder name on screen; song titles and paths shortened to a count by default, with a note in the FAQ about what's inside.

### 6.3 The gate for a release

1. `scripts/validate.sh` → `GATE PASSED` on the exact release commit; `budget-reviewer` on the release diff.
2. Release scans (a new `scripts/release_check.sh`):
   - illusion: no "Rockbox" or hidden-folder name in user-visible strings of the zip;
   - no Apple assets (fonts, images, firmware bytes, Apple sentences);
   - licences present; the developer-only clip absent;
   - stock bootloader byte-identity (`scripts/stock_boot.sha256`);
   - checksums written.
3. Clean-room build: the release zip rebuilt from the source tarball matches the published one (or the difference is explained).

### 6.4 Device soak (RC)

| Test | Unit | Pass |
|---|---|---|
| 7 days of daily use | owner's 5.5G (flash) | no panic record, no lost resume points |
| Overnight dark playback rundown | owner's unit; one stock-battery HDD unit | hours recorded; no underrun |
| USB cycles | stock-layout unit: 100 on macOS, 20 on Windows, 20 on Linux | mounts every time; library intact after each eject |
| Library at scale | 50k tracks (simulator plus one large unit) | scan, update and power-off-during-commit checks from the P10 checklist |
| Install, update, uninstall, restore | spare unit | each path twice |

---

## 7. Announcement

### 7.1 Soft beta (Wed 11-11)

| Where | Framing |
|---|---|
| r/ipod | "A free firmware that gives the iPod Video the iPod classic feel. Public beta, looking for testers with stock units." Screenshots, the safety facts, the tester template |
| Rockbox forums | Lead with thanks: ProkBox is a Rockbox fork; we're not asking Rockbox developers for support; issues go to our tracker. Offer the backport list (`docs/upstream-backports.md`) as future contributions |

### 7.2 Wider push (after GA, from Mon 11-30)

Hacker News "Show HN", Lobsters (`hardware`, `retro` tags if available), MacRumors forums (iPod section), Mastodon (retro-computing hashtags), a short note to two or three iPod-mod YouTube creators with a review unit offer if the owner has one, iFixit and Elite Obsolete style communities.

### 7.3 Prepare by RC1

- [ ] Demo video, 60–90 s, filmed on the real unit: boot, menus, Now Playing, an audiobook with chapters, drag-and-drop on a Mac, Hold MENU to Apple's firmware.
- [ ] 8–10 screenshots from the simulator (consistent set) and two photos of the device.
- [ ] One-paragraph pitch (draft, our own words): *ProkBox is free, open-source firmware for the iPod Video (5th generation and 5.5). It gives the Video the feel of the last iPod classic software: split-screen menus, smooth lists and a familiar Now Playing screen. It needs no iTunes: drag music on from any computer, play nearly any audio format, and listen to audiobooks with chapters, resume and speed control. It's built on Rockbox, and Apple's own software stays on the iPod as a fallback.*
- [ ] Safety FAQ: "Is it safe? Can it brick my iPod? How do I go back?" Answer: Apple's firmware stays; hold MENU; Disk Mode and Restore are built into the iPod; we never change the iPod's ROM.
- [ ] Credits block for every post: Rockbox and its contributors, TeX Gyre Heros, the codec libraries.

### 7.4 Name, trademark and legal hygiene

- [ ] USPTO and EUIPO search for "ProkBox" (and "ProkBox5g") before beta 2; pick a domain only if needed (Pages URL is enough for v1.0).
- [ ] Never use "iPod", "Apple", "Cover Flow", "Genius" or "Click Wheel" as our product or feature names; compatibility wording only ("runs on the iPod Video 5G/5.5G"). Cover Flow ships as "Album Flow"; the Parachute game ships as "Chute" (not "Airdrop": AirDrop is an Apple mark).
- [ ] "Not affiliated with Apple" on the site, README, guide, release notes, the About screen's Legal page and the first-boot notice (texts in `docs/legal.md` §2).
- [ ] First-boot notice on device: shown on the first boot after each new build, dismissed with Select (GPL v2 §2(c); `docs/legal.md` L4).
- [ ] `scripts/legal_audit.py` prints `LEGAL PASSED` (part of `validate.sh --release`).
- [ ] Every edited upstream file carries its change line (GPL v2 §2(a); `docs/legal.md` L2).
- [ ] BCM2722 provenance audit (#43 item 3) before the reference is published.
- [ ] Contact and takedown address (Q13): `prokbox.project@gmail.com` created 2026-10-26; in the README and `docs/legal.md` §1, still to go on the site and release notes.
- [ ] Free only; no donations tied to Apple marks.

---

## 8. After release

| Item | Plan |
|---|---|
| Triage | twice a week for the first month, then weekly; labels: `bug`, `install`, `hardware-report`, `question`; S0/S1 get a patch release |
| Cadence | patch releases (1.0.x) as fixes land, at most weekly; minor releases about every 6 weeks |
| v1.1 candidates | sleep and instant resume (#86), graphical installer, PDF guide, Game Development Guide, cluster reformat for large mods |
| v2 candidates | Native install with our own Disk Mode (#122), Videos and Photos (P14), languages (P22) |
| Upstream backports | the owner's audit of `docs/upstream-backports.md` after 1.0 is stable (owner, 2026-10-24); nothing is sent before |
| Contributions | `CONTRIBUTING.md` (DCO sign-off, rules from `CLAUDE.md` in short form); games through the P12 SDK once the guide ships; translations through `apps/lang/` once P22 lands |
| Tracker | public issues on GitHub; the private Forgejo keeps development work until the owner moves it |

---

## 9. Open questions for the owner

Each has a recommended default; silence means the default.

| # | Question | Default |
|---|---|---|
| Q1 | Mid-November = RC1 and public beta, GA Mon 11-30? | **Yes** |
| Q2 | Buy one or two spare units now (a 5G or 5.5G 30 GB HDD with a stock battery) for installer, recovery and HDD testing? | **Yes, one now** |
| Q3 | Public home | **GitHub, plus a Codeberg mirror** |
| Q4 | Publish history | **Upstream history plus a squashed ProkBox import**; full history stays on Forgejo |
| Q5 | Publish `CLAUDE.md`, `.claude/` and the docs (scrubbed), and state openly that the project is AI-assisted? | **Yes to all** |
| Q6 | v1.0 installer form | **Double-click command-line installer per OS plus the zip; GUI in v1.1** |
| Q7 | Apple Developer Program for notarised macOS builds; Windows signing | **Join Apple's programme; Windows unsigned in v1.0 with illustrated steps** |
| Q8 | Bootloader in v1.0 | **Today's ProkBox side-by-side bootloader, unchanged; no W1-B in v1.0** |
| Q9 | Variants | **Superseded (owner, 2026-10-26): the 8-zip matrix (owner, 2026-10-26): `ProkBox5g-<ver>-{stock,modded}-{flash,hdd}-{32,64}MB.zip`, chosen by the user from the guide's three questions (0.8.0 has no installer app: zip plus written steps)** |
| Q10 | Our BCM2722 image (L2) on by default in v1.0? | **On only if a second unit passes the verified stages by RC1; otherwise off, CPU path** |
| Q11 | Leave the BCM2722 video decoder out of the release build while Videos are hidden? | **Yes, leave it out** |
| Q12 | Keep the name ProkBox (subject to the trademark search)? | **Yes** |
| Q13 | Public contact address | **A project alias, not a personal address**: `prokbox.project@gmail.com` (created 2026-10-26) |
| Q14 | Beta size and the parents' install | **3–5 private, then 10–15 public; the owner installs on the parents' unit in week 2** |
| Q15 | Sleep and instant resume (#86) in v1.0? | **No, v1.1** |
| Q16 | Games in v1.0 (P12)? | **Yes, if P12's gate holds after the merge** |
| Q17 | Report channel for users without GitHub | **An email alias that accepts the diagnostics file** |
| Q18 | IP-lawyer consult on the 7G look before GA | ~~Yes~~ **No (owner, 2026-10-24):** no resources for it; mitigations in `docs/legal.md`. Licence stays GPL-2.0-or-later (GPL-3.0 considered, see `docs/legal.md`) |
| Q19 | Licence for the user guide and site prose | **CC BY-SA 4.0** (code stays GPL-2.0-or-later) |
| Q20 | Version naming | **Superseded (owner, 2026-10-26): `0.8.x` = the pre-release (private, then public); `1.0.0` = GA; betas (`1.N.0-beta.M`) only for new features after 1.0** |

---

## Workstreams and issues

| Workstream | Issue | Existing issues it pulls in |
|---|---|---|
| Public mirror and release packaging | #127 | #43 |
| Installer and recovery | #128 | #98, #54, #122 (v2) |
| Release docs | #129 | #98, #123 |
| Beta programme and diagnostics | #130 | #102, #96 |
| Launch | #131 | #43 |
