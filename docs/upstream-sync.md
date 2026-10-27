# Keeping up with upstream Rockbox

Our fork sits on `upstream/master` plus a small stack of topical commits. This note records the most recent check, and gives the procedure for repeating it.

## Status (synced 2026-10-06)

**Rebased** onto `2890d4a40d`; old history kept as `backup/pre-sync-20261006`. Resolutions followed the table below, plus: Darwin uses sigaltstack threads again (our `_setjmp` fix), and the gcc version probe is kept. The gate passed and the screenshots were unchanged apart from the clock.


| Item | Value |
|---|---|
| Our base | `51abd937d5` (2026-05-12, "playlist viewer: retrieve track name id3 from db") |
| `upstream/master` | `2890d4a40d` (2026-10-05, "vorbis: don't buffer the comment packet") |
| Upstream commits since our base | **609** (mostly from Solomon Peachy, William Wilgus, Christian Soffke, Marcin Bukat and Michael Giacomelli) |
| Our commits on top | 17, of which 16 are non-merge (1 is the merge `117a6102af`, which a rebase flattens) |
| Dry-run rebase | It stops on **6 of 16 commits**. That is **9 conflicting files with 10 hunks**, all small. There is also **1 semantic break** that git does not flag (see below). |

### Upstream commits that touch areas we care about

Most of the 609 commits are new ports (Samsung YP-CP3, ipodnano3g, ap80max, 3ds, hibyr1, echoplayer) and areas we don't build. The ones that matter to us:

- **Playback and buffering**
  - `31dfd5da2e` and `c54dddc2ac` add the Playlist Single Mode option and fix its pause behavior.
  - `943b73851e` prevents crossfading into a new track after a pause (FS#13958).
  - `bd615cf125` makes `audio_hard_stop()` kill the PCM path (FS#13996).
  - `ecdeb02dda` uses a 44.1 kHz floor when guessing the playback frequency.
  - `9a7ffac2e4` lets each pcm_sink choose software or hardware volume.
  - Our `playback.c` hunks (chapter handling) rebased cleanly.
- **Metadata**
  - `3cd286d8f8` adds `get_metadata_afmt()` and an `audio_fmt` argument.
  - `22b4cd3232` adds `METADATA_EXCLUDE_NORMALIZE`. This is the source of the `metadata.h` conflict.
  - `24b0254d96` is a cleanup.
  - Two commits touch `mp4.c` (the `$Id$` removal and the rbfs rename). Our 394-line chapter parser merged cleanly.
- **Tagcache and tagtree**
  - `dc37bb1ba0` makes tagtree warn instead of freezing when `tagnavi.config` is missing.
  - `16900c7fee` fixes a skin glitch while rebuilding tagcache.
  - `tagnavi.config` is unchanged upstream, so our Audiobooks menu is safe.
- **Skin engine and WPS** (18 commits)
  - `2a29dedeb6` draws album art first.
  - `c0a8303a9c` simplifies screen updates.
  - `3d44fe94f3` simplifies `skin_wait_for_action`.
  - `a824085057` adds the `%pX` time-based playlist progress tag.
  - `a39e4f2a06` and `74905f4796` remove `skin_unload_all` and `get_skin_filename`.
  - `789d796120`, `8eefac3638` and `cacbd9aad2` are quickscreen changes.
  - `376db9bf5b` is touchscreen flick support.
  - Our `skin_tokens.c`, `skin_display.c` and `wps.c` hunks rebased cleanly.
- **root_menu**
  - `57a91121f6` removes the ACTIVITY_UNKNOWN hack for plugins.
  - `86decaab9e` stops the start screen from resuming a finished playlist.
  - `0a1858b9d1` prevents an infinite loop when returning to the WPS from plugins.
  - These matter for Phase 2, which replicates `root_menu` setup. See `docs/phase2-brief.md`.
- **iPod and PP targets**
  - `4c60fe95fc` reverts "pp5020: ata: do not yield in ata_wait_intrq()". The revert restores yielding, which upstream says fixed bad UI lag while the database is in use. That is good for us.
  - `70fd3e1f1e` drops ipodcolor to UDMA 1. It sits next to our `#ifndef ATA_MAX_UDMA` guard but merges cleanly.
  - `190822f261` limits `system_memory_guard()` to Coldfire.
  - `b217a55059` adds the ipod6g inline remote. It does not affect us.
- **FAT and the filesystem.** `3a57f2f721` adds an "rbfs" prefix to the native filesystem functions and **renames `filesize()` to `ffilesize()`**. `fat.c` itself is unchanged.
- **SDL simulator and macOS**
  - `cd699e5448` ("configure: darwin: update GCC, use SDL threads") updates upstream to gcc-16 and **switches Darwin to SDL threads**, because `sigaltstack` returns EPERM on macOS 26.4 and later.
  - Our `d5640fd41c` (`_setjmp`/`_longjmp` in `thread-unix.c`) is the real fix for that FIXME and is worth sending upstream.
  - `aae46a822a` adds simulator support for the `%bu` tag.
- **Build**
  - `44e8d8226a` changes `-std=gnu99` to `gnu11`.
  - `85c1ff8667` makes builds reproducible.
  - `af9b65485b` moves `-mthumb-interwork` into configure.
  - `b6db69e7ef` and `0b2caf7338` clean up configure's utf8proc and appextra handling.
  - `e62ac0d2c6` makes ovl_offset use ELF symbols.
- **Warble**
  - `5d016a2c04` fixes the warble build when configured as the iPod Video. It is **the same `core_alloc.c` change we made**.
  - `7fef95dc08` fixes warble on HAVE_RECORDING targets. It **supersedes our `codecs/SOURCES` hunk**.
  - `8f38274e90` zeroes the mp3entry before reading metadata.

## Conflict list (dry run)

| Our commit | File | Hunks | Cause and resolution |
|---|---|---|---|
| `6b700088f9` metadata: parse m4b chapters | `lib/rbcodec/metadata/metadata.h` | 1 | Upstream added `METADATA_EXCLUDE_NORMALIZE` next to our `struct audiobook_chapters;` forward declaration. **Keep both.** |
| `65d966baea` audiobooks: chapter skip/browse | `apps/onplay.c` | 1 | Upstream reworked the WPS context menu (`bf0fa29a30` and `4d773a3329`). Re-apply our audiobook entries on top of the new structure. |
| `da3afd7d72` build: GCC 16 fixes | `apps/plugins/sdl/sdl.make` | 1 | The 3ds refactor (`e498c0171a`) rewrote the `ctru-app` block. Keep our unconditional `-Wno-*` flags, or drop them if upstream now builds the SDL plugins with GCC 16. |
| `0a9cefa410` configure: newest gcc + `--ipodui` | `tools/configure` | 1 | Upstream hardcodes gcc-16 in the same Darwin block. Keep our version-probe loop, or simply take upstream's. Our `--ipodui` hunks merged cleanly. Also decide on SDL threads (below). |
| `00bdfd79fa` metadata: thread-safe chapter reading | `apps/SOURCES` | 1 | Upstream added port-specific entries nearby. **Keep both.** |
| `793a8ee33d` warble: build on macOS | `firmware/core_alloc.c` | 1 | Upstream has the same fix. **Take upstream's.** |
| | `lib/rbcodec/codecs/SOURCES` | 1 | Superseded by `7fef95dc08`. **Take upstream's.** |
| | `lib/rbcodec/test/warble.c` | 2 | Upstream reworked warble (`7fef95dc08` and `8f38274e90`). **Take upstream's and re-apply only our `-m` metadata-only and chapter-output mode.** |
| | `tools/functions.make` | 1 | Neighbouring 3ds edit. Keep our warble objcopy branch. |

**Semantic break (no textual conflict):** `3a57f2f721` renames `filesize()` to `ffilesize()`.
- Our new call at `lib/rbcodec/metadata/mp4.c:1226` (`off_t fsize = filesize(fd);`) must become `ffilesize(fd)`.
- Check the `filesize()` stub in `warble.c` (line 94) against the renamed upstream one.
- The rebased tree will not compile until this is fixed, so always build after a rebase.

**Behavioral change to check:** after the rebase, Darwin configures with SDL threads unless we restore `sigaltstack=\`check_sigaltstack\`` in the Darwin block. Pick one setting and confirm that `scripts/shots.sh tests/ui/smoke.script` still works with it.

The `ipod7g/base` branch points at `da3afd7d72`. It is only a marker of the old fork point and needs no rebasing.

## Recommendation: rebase now, before Phase 2

- **The conflicts are small and mechanical.** Ten hunks, and half of them are "take upstream's", because upstream fixed the same warble and macOS issues.
- **The cost grows with time.** Phase 2 adds `apps/ipodui/` and a `main.c` hook, and touches `root_menu.c`. Upstream changed `root_menu.c` 7 times in this window, so doing Phase 2 on the old base means re-resolving all of that later.
- **We pick up fixes we want:**
  - the ATA yield revert (UI responsiveness during database work)
  - the playback and crossfade fixes
  - skin album art drawing first
  - the warble fixes
- **Stay off the new ports' churn for now.** After this rebase, sync again at the end of each roadmap phase rather than continuously.

Do it as its own short task, ahead of the Phase 2 work: rebase, fix the `ffilesize` break, then build and test everything.

## Procedure (repeatable)

Run from the repo root on a clean tree.

```sh
# 0. Note the current base and push a safety ref
git fetch upstream
OLD_BASE=$(git merge-base main upstream/master)
git branch -f backup/pre-sync-$(date +%Y%m%d) main

# 1. Survey what changed (adjust the path list to taste)
git rev-list --count $OLD_BASE..upstream/master
git log --oneline $OLD_BASE..upstream/master -- \
  apps/playback.c apps/buffering.c apps/tagcache.c apps/tagtree.c apps/root_menu.c \
  apps/main.c apps/onplay.c apps/gui/skin_engine apps/gui/wps.c apps/recorder/albumart.c \
  lib/rbcodec/metadata firmware/target/arm/ipod firmware/target/arm/pp \
  firmware/drivers/ata.c firmware/drivers/fat.c firmware/target/hosted/sdl \
  firmware/asm/thread-unix.c uisimulator tools/configure lib/rbcodec/test/warble.c

# 2. Dry run in a throwaway worktree (never touches main)
git worktree add -b tmp-rebase-check out/rebase-check main
cd out/rebase-check
git -c core.editor=true rebase upstream/master
#   at each stop: record the files, then decide:
git diff --name-only --diff-filter=U
#   (for the dry run only, `git checkout --theirs -- <f> && git add <f>` then
#    `git -c core.editor=true rebase --continue` to discover later stops)
git rebase --abort 2>/dev/null
cd ../..
git worktree remove --force out/rebase-check
git branch -D tmp-rebase-check

# 3. Real rebase (on main, or on a sync branch and then fast-forward main)
git checkout -b sync/$(date +%Y%m%d) main
git rebase upstream/master          # resolve properly, using the table above as a guide
grep -rn '\bfilesize(' lib/rbcodec/metadata apps/audiobook*.c apps/ipodui  # API renames

# 4. Verify (all must pass before fast-forwarding main)
#    - build skill: stock sim, --ipodui sim, stock fw, --ipodui fw, bootloader, warble
#    - test skill: scripts/test.sh (chapter golden files)
#    - sim-shot skill: scripts/shots.sh tests/ui/smoke.script, compare against the previous PNGs
#    - compare fw binary size against the budget table in docs/hardware.md
git checkout main && git merge --ff-only sync/$(date +%Y%m%d)
```

Notes:
- Rebasing rewrites our commit IDs. Keep the `backup/pre-sync-*` branch until the device has run the new build.
- If a conflict sits in a file we touch only lightly, check whether upstream has fixed the same problem. That happened this time with warble and the macOS gcc, and in that case drop our hunk.
- After each sync, update the "Status" section above.
