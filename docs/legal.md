# Legal and licensing: the compliance record

This file is the single record of what ProkBox5g owes, legally, and how each obligation is met and checked. The release gate (`scripts/validate.sh --release`) runs `scripts/legal_audit.py`. Every public release is checked against this list. It is our own working record, not legal advice.

**Owner decisions, 2026-10-24 (later):**
- **No IP-lawyer consult** (Q18 reversed). This is a small, free, volunteer project without the resources for one. We rely on the measures in this file: no Apple assets or code, no Apple marks in any name, explicit disclaimers everywhere, our own artwork and fonts, and a fast takedown response (§3 L14). The remaining risk is the 7G *look* (trade dress), which wording alone doesn't address; L14 is how we would handle a complaint.
- **The licence stays GPL-2.0-or-later.** We considered GPL-3.0 and kept v2-or-later for three reasons. Our code can still go back into Rockbox (`docs/upstream-backports.md`). About 200 upstream files say only "subject to the GNU GPL, see COPYING" (the v2 text), so a v3-only combined work would rest on a reading of v2 §9 we would rather not depend on. And "or later" already lets anyone take the whole work under v3. Revisiting is one-way: v2+ can become v3 later, but not back.

Owner decisions (2026-10-24):
- ProkBox5g is a community-driven project, not an Apple product, and not endorsed by Apple.
- Apple's marks are named only to identify the hardware.
- We comply with GPL v2 for the Rockbox base.
- We never remove a licence or legal notice.
- Our own code is GPL-2.0-or-later.
- We use no Apple trade dress.
- Code that is never compiled is stripped from the public tree only.
- A notice is shown on first boot after every new build.

## 1. Name

| | |
|---|---|
| Brand | **ProkBox** |
| This build line | **ProkBox5g**: *a device-tuned fork of Rockbox for the iPod Video (5G/5.5G)* |
| On screen | The device title, header and default name stay **ProkBox**. About and the version string show **ProkBox5g &lt;ver&gt;**. |
| Files and places | Public repo `prokbox5g`; assets `ProkBox5g-<ver>.zip`, `ProkBox5g-<ver>-source.tar.gz`, `ProkBox5g-Installer-<ver>-<os>` |
| GitHub organization | **`prokbox`** (owner, 2026-10-26): the single home for every ProkBox project, not just this build line: `prokbox5g` now, builds for other models later (6G/7G), and the hardware references (PP5022, BCM2722, WM8758) as repos of their own. Org-wide files (profile, `SECURITY.md`, `SUPPORT.md`) live in `prokbox/.github` and stay model-neutral; each project repo carries its own issue forms. Org texts say **ProkBox**, project texts say **ProkBox5g** |
| Contact | **prokbox.project@gmail.com** (owner, 2026-10-26): the one public project address for questions, takedown and trademark complaints, and the Reddit/forum account sign-ups. Never the owner's personal address |

The "5g" suffix leaves room for builds for other hardware later. Apple's marks appear only after "for the", to say which hardware the build runs on (nominative use). They never appear in a product, feature, repo, file, asset or release name. So we write "ProkBox5g for the iPod Video", never "ProkBox iPod firmware". Feature names avoid Apple's marks: Cover Flow ships as *Album Flow*. On-screen text never calls the device "iPod": messages are worded so they need no device name (owner, 2026-10-24); the only on-screen Apple marks are in Settings › Legal and the first-boot notice. The *Parachute* game became *Chute*: *Airdrop* was rejected because *AirDrop* is an Apple mark, and *Parachute* was a Nintendo Game & Watch title.

### Internal names (package F, owner 2026-10-24)
The shell's internal names still say "ipod": `HAVE_IPOD_UI`, `--ipodui`, `apps/ipodui/`, `tests/ipodui/`, `ipodui-*.script`, the `ipodui:` feature tag and `LANG_IPODUI_*` in `apps/lang/`, and "iPod UI shell" in comments. That reads as hardware description, like upstream's own `firmware/target/arm/ipod/`. But they are our own names and they will be public, so they move to the brand: **`HAVE_PROKBOX_UI`, `--prokbox-ui`, `apps/prokbox/`, `LANG_PB_*`** (confirmed by the owner, 2026-10-24; brand-level rather than `5G`, so a future build line can reuse the shell). In 2026-10-24 terms that is about 938 tracked files and 7,600 occurrences, with 318 LANG IDs. It runs as one scripted, mechanical commit (`scripts/rename_shell.py`, `git mv` plus a word-boundary rewrite, then the full gate) in the same quiet window as package D, after P12 and P19 merge. Upstream's hardware names (`ipod` targets, `IPOD_VIDEO`, `ipodpatcher`) stay as they are. Names stored on a unit change too: the data folder under the hidden folder, the log file and 16 settings' config names. The owner chose to move them on the **first boot after the renamed build is installed**: rename the folder once, read the old config names once as aliases and save under the new ones, and start the log under its new name. `scripts/rename_shell.py`'s report lists the exact old → new pairs; `scripts/mark_modified.py` is package D's tool. Both have a dry run (the default) and `--apply`.

## 2. Disclaimer texts

These are written in our own words. Use them verbatim, and change them only here.

Every text opens with the same plain statement, **"ProkBox5g is not Apple software."** (owner, 2026-10-24: disavow any suggestion that Apple made, approved or supports it). Nothing comes before it except the name.

**Short** (release notes, site footer, announcements):
> ProkBox5g is not Apple software. It is an independent, community-driven fork of Rockbox, and Apple Inc. has not made, approved, endorsed or supported it in any way. It contains no Apple software. iPod is a trademark of Apple Inc.

**Long** (README, the site's Credits and licences page, the user guide):
> ProkBox5g is not Apple software and not an Apple product. It is free, open-source, community-driven firmware: a fork of Rockbox, tuned for the iPod Video (5th generation and 5.5). Apple Inc. did not make it, has not approved or endorsed it, does not support it, and has no connection with this project. If something goes wrong with ProkBox5g, ask the ProkBox community, not Apple. iPod, iPod classic, iTunes, Click Wheel, Cover Flow and Apple are trademarks of Apple Inc., registered in the U.S. and other countries. We use those names only to say which hardware ProkBox5g runs on. ProkBox5g contains no Apple software, fonts, logos or artwork.

**On device: the first-boot notice** (`LANG_IPODUI_NOTICE_*`; owner, 2026-10-26: the contact address, the way back to Apple's software, a countdown while Select is held off, UK spelling, one screen):
> **This is not Apple software**
> ProkBox5g &lt;ver&gt; is independent firmware, built by the community on Rockbox. Apple Inc. did not make, approve, endorse or support it, and it contains no Apple software.
> For help, write to prokbox.project@gmail.com, not to Apple. To start Apple's software instead, hold Menu while switching on.
> iPod is a trademark of Apple Inc., used here only to name the hardware.
> © the Rockbox and ProkBox contributors. Free software (GNU GPL v2 or later), no warranty.
> Licence and source: github.com/prokbox/prokbox5g
> Press Select to continue. *(for its first 3 s: "Press Select to continue (3)", counting down; `LANG_IPODUI_NOTICE_WAIT`)*

How the notice is shown:
- **When:** on the first boot after each new build is installed, before anything else in the shell. Nothing plays or appears behind it.
- **The heading:** "This is not Apple software" is its own heading line, in the large list face (Bold 15), above everything else. The version string goes in the body, not the heading.
- **Dismissing it:** only Select dismisses it, and Select is ignored for the first 3 seconds, so a habitual click can't skip it unread. The wheel scrolls it if it is longer than the screen; the other buttons do nothing. Select saves the acknowledgement once; nothing is written before then.
- **Reading it again:** the same text is the top of Settings › Legal, so it can be reread at any time.

**On device: Settings › Legal**, which can be read at any time: `LANG_IPODUI_LEGAL_1..6`. It opens with the notice's heading and first paragraph; the independence and trademark paragraphs follow the long text.

### Wording we deliberately avoid
- **"Theme" or "skin":** ProkBox5g is a fork of the whole firmware.
- **"Fair use" or "replication":** a disclaimer shouldn't argue a legal conclusion or describe the work as copying. We say what the names are for: identifying the hardware.
- **"Official", "update", "iPod firmware" as a name,** or anything else that implies Apple made or approved it.
- **Mixing trademark and copyright:** Apple's *names* are trademarks. Apple's *software, art and fonts* are copyrighted works, and we include none of them.

## 3. Obligations

Status values: **met** (in place and checked), **partial**, or **todo**. "Audit n" is item n of `scripts/legal_audit.py`.

| # | Obligation | How we meet it | Check | Status |
|---|---|---|---|---|
| L1 | **GPL v2 §1**: keep the copyright notices, licence references and no-warranty statements intact | Upstream headers are never edited, only added to. `docs/COPYING`, `docs/CREDITS` and `docs/LICENSES` are kept | Audit 1 compares each kept upstream file's header block with the merge-base | todo (audit) |
| L2 | **GPL v2 §2(a)**: modified files carry prominent notices that they were changed, and when | One line inside each edited upstream file's header: `Modified for ProkBox5g (2026–<year>); see docs/changes-from-rockbox.md.` The ledger in `docs/changes-from-rockbox.md` says what changed | Audit 4; `scripts/mark_modified.py` (package D, run after P12/P19 merge) | todo |
| L3 | **GPL v2 §2(b)**: the whole work is licensed under the GPL | Every new file has the ProkBox header (GPL-2.0-or-later, `docs/templates/prokbox-header.txt`) | Audit 3 | met for `apps/ipodui/`; audit pending for the rest |
| L4 | **GPL v2 §2(c)**: an interactive program shows a copyright notice, the no-warranty notice and where to find the licence | The first-boot notice (above) and Settings › Legal (package C: `apps/ipodui/notice.c`, `screens/legal.c`, `LANG_IPODUI_NOTICE_*`) | UI script `tests/ui/ipodui-legal-notice.script` | met (2026-10-26): `LANG_IPODUI_NOTICE_URL` = "github.com/prokbox/prokbox5g"; the notice still fits one screen (sim shot, `ipodui-legal-notice.script`). The release workflow refuses a tag while the placeholder is back |
| L5 | **GPL v2 §3(a)**: binaries come with the complete corresponding source | Every release carries `ProkBox5g-<ver>-source.tar.gz` next to the binaries, built by CI from the same tag. It includes the build scripts, `tools/`, the font sources and converter, the BCM2722 image sources and the installer source | Release-day clean-room rebuild from the tarball matches the published zip | todo (#127) |
| L6 | **Rockbox credit** | README, the site, Settings › Legal, the first-boot notice and every announcement. Upstream history is kept in the public repo | Review | partial (README, Legal) |
| L7 | **Third-party licences** (`lib/`, `docs/LICENSES`) | Kept in the tree and the source tarball; the licence texts the binary zip needs are copied into it | Audit 5 | todo (audit) |
| L8 | **Fonts** | TeX Gyre Heros "ShellSans" under the GUST Font License (`apps/ipodui/fonts/GUST-FONT-LICENSE.txt`). Source Sans 3 under the SIL OFL 1.1 if shipped. No Apple fonts, Podium Sans included | Audit 5, `apps/ipodui/fonts/MANIFEST` | met |
| L9 | **Trademarks** | §1 naming rules; §2 texts | Audit 6 covers names and LANG strings | partial |
| L10 | **Trade dress**: no Apple logo, icons, fonts, artwork, marketing images or firmware | Our own boot glyph and icons, generated from our own geometry. Screenshots come from our simulator or our own devices, framed so Apple's logo isn't shown. `docs/reference/*/` stays private (gitignored) | Audit 7 (image provenance) and 8 (Apple firmware bytes) | partial (no consult, owner 2026-10-24; L14) |
| L11 | **Non-commercial posture** | Free only. No donation link in v1.0 | Review | met |
| L12 | **Docs and site prose** | CC BY-SA 4.0 (Q19), stated on the site | Review | todo (#129) |
| L14 | **Takedown and complaints** | A published contact address (Q13: `prokbox.project@gmail.com`) on the site and README. A trademark or trade-dress complaint gets an answer within a few days, and we change or remove what it names rather than argue: rename, redraw or withdraw. We record it here | Review | todo (#131) |
| L13 | **Test media** | The *Sintel* clips (CC BY 3.0) are developer-only and absent from release zips | Audit 7 | met |
| L13a | **Owner media and data** | Owner rules, 2026-10-26: no copyrighted content or private data from the owner's iPod in the repo, even privately; art caches and database files only if synthetic; screenshots and goldens use open or synthetic media only. Test names are synthetic (no real artists or titles). `scripts/check_no_owner_media.py` guards commits; `scripts/release_tree.py` drops the owner-derived directories and fails closed on private names | Owner | met |
| L13b | **Public copyright holder** | The exported tree shows "prokopto-dev" as holder; the owner's surname is a fail-closed pattern there (the first name and initial are public on the GitHub profile). The private tree keeps the full attribution | Owner | met |

## 4. The public tree

Code that is never compiled is stripped **from the public tree only** (owner, 2026-10-24). The private development repo keeps the full upstream tree, so upstream merges and the backport list (`docs/upstream-backports.md`) stay easy. `scripts/release_tree.py` builds the public tree:

1. **Collect.** Build every release configuration (firmware, codecs, shipped plugins, bootloader if shipped, BCM images, installer). Collect every file read, using the `-MMD` dependency files and a make trace of the Makefiles and host tools.
2. **Keep.** Always keep every licence and notice file, `docs/`, `scripts/`, `tests/` and the simulator sources, so contributors can build and test.
3. **Scrub.** Apply the release-plan §3.2 scrub list: private hosts, paths and serials.
4. **Publish** as a labelled commit sequence: upstream Rockbox history up to our merge-base, then **"strip: remove code ProkBox5g never compiles"**, then the ProkBox import commit, then normal public development. The strip commit only deletes. Every remaining upstream file keeps its header.

Stripping never removes a notice the remaining code needs. If a kept file's licence text sits in a file that would otherwise be stripped, that licence file is kept (audit 2 and 5).

## 5. Asset provenance

Every image, font or binary blob in the public tree is either:
- **generated** by a script in the tree, which is listed below; or
- **listed** here with its source and licence.

Anything else fails audit 7.

| Path | Origin | Licence |
|---|---|---|
| `apps/ipodui/fonts/*.fnt` | `apps/ipodui/fonts/gen.sh` from TeX Gyre Heros | GUST Font License |
| `docs/design/usb/*.png`, `apps/ipodui/usbscreen_data.h` | `art/usb/usbscreen.py` | GPL-2.0-or-later |
| `tests/ui/golden/**` | our simulator renders | GPL-2.0-or-later |
| `bcmfw/video/*.bin` | built from our BCM2722 sources in `bcmfw/` | GPL-2.0-or-later |
| `bcmfw/video/stage6-real.mp4` | *Sintel* trailer excerpt, © Blender Foundation | CC BY 3.0, developer-only |
| `bcmfw/video/stage7-conv.m4v` | *Sintel* trailer excerpt (40 s), © Blender Foundation | CC BY 3.0, developer-only |
| `docs/diagrams/*.png` | rendered from our D2 sources beside them (`*.d2`) | GPL-2.0-or-later |
| `docs/research/bcm2722/*.png` | photos and captures of our own bench sessions (test patterns on our own unit) | GPL-2.0-or-later |
| `bcmfw/video/stage6-clip.mp4` | synthetic clip generated for the stage 6 bench test | GPL-2.0-or-later |
| `tests/ui/fixtures/**` | generated test fixtures (flat test art, leftover-file cases) | GPL-2.0-or-later |
| `tests/gate/mutants/**` | our simulator renders, deliberately altered as gate self-test mutants (`scripts/gate_mutants.sh`) | GPL-2.0-or-later |
| `docs/design/logo/`, `docs/design/unlit/` | our own design work | GPL-2.0-or-later (to confirm generator per file in the audit) |
| `docs/user/images/getting-started-main-menu.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/ipodui-nav-panels.script` | GPL-2.0-or-later |
| `docs/user/images/getting-started-legal-notice.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/ipodui-legal-notice.script` | GPL-2.0-or-later |
| `docs/user/images/music-music-menu.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/docs-music-menu.script` | GPL-2.0-or-later |
| `docs/user/images/music-now-playing.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/ipodui-p5-rating.script` | GPL-2.0-or-later |
| `docs/user/images/music-visualiser.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/ipodui-p19-viz.script` | GPL-2.0-or-later |
| `docs/user/images/audiobooks-audiobook-chapters.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/docs-audiobooks-chapters.script` | GPL-2.0-or-later |
| `docs/user/images/audiobooks-audiobook-now-playing.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/ipodui-p5-audiobook.script` | GPL-2.0-or-later |
| `docs/user/images/settings-settings.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/ipodui-p8b-settings.script` | GPL-2.0-or-later |
| `docs/user/images/usb-and-syncing-usb-connected.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/ipodui-p8a-usb.script` | GPL-2.0-or-later |
| `docs/user/images/troubleshooting-save-diagnostics.png` | our simulator render (`scripts/shots.sh`), of the synthetic test library only (`tests/ui/setup_library.py`): no owner media, `tests/ui/ipodui-diag-report.script` | GPL-2.0-or-later |
| `docs/user/images/video-videos-menu.png` | our simulator render (`scripts/shots.sh`, `tests/ui/ipodui-p14a-menu.script`) of the generated video fixtures (`tests/fixtures/videos/mkvideos.sh`; the poster art is drawn by that script) | GPL-2.0-or-later |
| `docs/user/images/video-video-play.png` | our simulator render (`scripts/shots.sh`, `tests/ui/ipodui-p14a-play.script`) of a frame of the generated video fixtures, whose only picture source is `bcmfw/video/stage6-real.mp4`: *Sintel* trailer, © Blender Foundation, durian.blender.org | CC BY 3.0 (the frame; credit as given here), GPL-2.0-or-later (the rest) |

## 6. Release-day checklist

Each step is also in release-plan §6.3 and §7.4.
- [ ] `scripts/validate.sh --release` prints `GATE PASSED` and `LEGAL PASSED`.
- [ ] Public tree exported. A clean-room rebuild from the source tarball matches the release zip.
- [ ] The short disclaimer is in the release notes; the long one is in the README and on the site.
- [ ] The owner has read the first-boot notice on a real unit.
- [x] `LANG_IPODUI_NOTICE_URL` in `apps/lang/english.lang` holds the real public licence and source address, github.com/prokbox/prokbox5g (L4; 2026-10-26).
- [ ] Binaries and source tarball are attached to the same release, with `SHA256SUMS`.
- [ ] Trademark search for "ProkBox" (and "ProkBox5g") is done. There is no IP consult (owner, 2026-10-24).
- [ ] A contact and takedown address is published (Q13): `prokbox.project@gmail.com` in the README (done), on the site and in the release notes.
