# Videos: the shell's contract with the player (Phase 14)

Issue #64. This is what the Phase 14 brief needs from the video side: the calls the Videos screens make, the library-update hook, the classification rules, and the test hooks. The player itself is built and host-tested on `p14-player` (`player.md`, "As built"); the Videos screens, the tagcache hook and the classifier are the shell's pieces 4 and 7 (`player.md` §10).

Everything here is behind `HAVE_BCMFW_VIDEO` (`tools/configure --bcmfw-video`, which implies `--bcmfw`). Without it, the shell builds and behaves as today.

## 1. The calls (`apps/video/vplayer.h`)

```c
bool vplayer_start(const char *path);
void vplayer_seek(unsigned long ms);
void vplayer_stop(void);
bool vplayer_info(struct vplayer_info *info);
void vplayer_set_frame_hook(void (*fn)(const struct vpc_frame *f)); /* device test only */
```

**Playing a video, in this order:**
1. **`vplayer_start(path)` first.** Its memory comes from `core_alloc`, taken on the CPU (PP1): buflib has no lock, so from 19-9 the COP UI asks a CPU service to allocate. Taken after the audio starts, it would shrink the audio buffer (`player.md` §5).
   - It returns false if the video can't play: no index and none can be built, no `/.rockbox/bcmvideo.bin` or a wrong hash, or no memory. The failure is logged.
   - **On false, still start the audio.** The file plays as sound alone, as today. The screen shows nothing extra (rule 1: errors are logged, not shown).
2. **Then start the audio on the same file,** through the normal playback path, at the resume position if there is one. The audio is the master clock; the player follows it and never owns playback.

**While it plays:**

| Shell action | What to call | Notes |
|---|---|---|
| Pause / resume | `audio_pause()` / `audio_resume()` only | The player reads the audio status every tick. Paused, it holds the picture and lets the backlight timeout run (the 7G keeps a *playing* video lit). |
| Seek (scrubber) | Seek the audio, then `vplayer_seek(ms)` | The player also follows an audio jump by itself (more than 0.3 s back or 1.5 s forward), so a missed `vplayer_seek` costs a restart, not a wrong picture. Calling it is still the contract: it's immediate. |
| Fast forward / rewind | The audio's own ff/rw | The core skips to key frames above 2× (`vpc_set_speed` exists in `vpcore.h`; `vplayer` doesn't expose it yet: **gap**, add `vplayer_speed(int percent)` if the screen needs speed control beyond the audio's). |
| Volume | The audio | Nothing on the video side. |
| Exit (MENU) | `vplayer_stop()`, then stop the audio, or leave it playing | `vplayer_stop()` gives the BCM back and the normal screen repaints. The 7G stops a video on MENU and keeps the position for resume. |

**End and errors: polls, no callbacks.** The screen calls `vplayer_info()` on its own tick (it's cheap: no I/O, no lock) and reads:
- `ended`: the last picture is up. The audio's own end-of-track tells the screen to leave, as for songs.
- `state == VPC_FAILED` (`vpcore.h`): video gave up, and the audio plays on. `fail_why` holds the reason (a `BVP_E_*` or `HP_E_*`) for the log only. The screen goes back to its normal view; it shows no error (rule 1).
- `state == VPC_ASLEEP`: the display is dark. The BCM is off, and it comes back in sync on wake.
- Statistics (`restarts`, `wakes`, `recoveries`, `posted`, `skipped`, `dropped_gops`, `lead_us`, `direct_reads`) are for the developer log and stage 6. Nothing user-visible.

The player also stops by itself about one second after the audio leaves the file or stops. The screen should still call `vplayer_stop()` when it leaves.

## 2. Power and the panel

- **The backlight:** while a video plays (not paused), the player sets the backlight timeout to "always on" and restores the user's setting afterwards. The screen mustn't fight it: no backlight calls of its own while video owns the panel.
- **Sleep:** when the backlight goes off, the player takes the BCM down (`VPC_ASLEEP`) and resyncs on wake. A paused video wakes on the paused picture, not the UI's last frame.
- **What the screen must not do while video owns the panel:**
  - No `lcd_update()` or `lcd_update_rect()`: the LCD driver's pushes are suspended (in the simulator too, which matches the device). Anything drawn now appears only after `vplayer_stop()`.
  - No `cpu_boost` held for the whole video. The player streams in 2 KB pieces from the audio's buffered copy (`bufpeek_at`) and needs no boost.
  - No disk reads of its own during playback beyond what the audio does. The player reads the `.vix` in 64 KB windows refilled with each audio buffer fill.
- **The overlay (SELECT: progress bar, elapsed and remaining):** until the BCM's 2-D jobs exist, there's no compositing on top of a playing picture. **Interim:** the overlay appears while paused, as a bottom-strip `UPDATERECT` (`player.md` §9 hook 3). The picture-in-play overlay waits for the 2-D job interface (`approach.md`, "Shell contract").

## 3. The `.vix` index at library update

- **What:** `vix_build_file(video, VIX_DIR, size, mtime, scratch, scratch_len)` (`apps/video/vix_file.h`). It writes `/.rockbox/ipodui/video/<hash>.vix` atomically (a `.tmp` file, then a rename). It needs at least 4 KB of scratch from the caller.
- **Where:** the tagcache thread's precompute step, next to `lib_precompute()` in `apps/ipodui/library.c`, which `ui_precompute_run()` in `apps/tagcache.c` drives (after every RAM-cache load and every deleting pass). One call per video that has no current index; the hash covers path, size and mtime, so a changed file gets a new index.
- **Cost and I/O budget:** one bulk read of `moov` per file (`player.md` §1). A 2-hour film's `stsz` alone is 864 KB, about 1–2 s on the PP, estimated. Run it in chunks that yield at any queued event (`ui_precompute_event()`), at the tagcache thread's priority, never at browse time (rule 3: precompute at sync time). Batch the files so the disk spins up once.
- **A missing index:** `vplayer_start()` builds it at open, behind the 7G's normal loading pause, and writes it for next time. Files the demuxer refuses (`MP4_E_*`) get no index and play as audio only.
- **Stale indexes:** a pass after the library update deletes `.vix` files whose hash no longer matches a library entry, in the same precompute run.

## 4. Classification (the shell's piece 4)

A file is a video if it has a `vide` track whose sample entry is `avc1`/`avc3`, or `mp4v` once MPEG-4 Part 2 lands (`mpeg4.md`). Read at library update, from the same `moov` read as the index.

| Source | List |
|---|---|
| `stik` 9 | Movies |
| `stik` 10 | TV Shows (`tvsh`, `tvsn`, `tves`: show, season, episode) |
| `stik` 6 | Music Videos |
| `pcst` (podcast flag) | Video Podcasts |
| no `stik`, in a folder named Movies, TV Shows, Music Videos or Podcasts | that category (the nearest such folder in the path wins) |
| no `stik`, anywhere else | Movies |

**Frozen defaults (#64), until the Phase 14 owner round:**
- A `stik` always beats the folder.
- **Visibility follows stock 2.0.5:** the Videos item follows Settings › Main Menu. With no videos, the panel shows "No Videos" (a short label; any full sentence is in our own words). Every category keeps its row, and an empty one opens to an empty-state row.

**Must fix in the same piece: the `.mp4`-as-song issue.** Today `metadata.c` maps `.mp4` to AAC, so a film with an AAC soundtrack appears in Songs and plays as sound. The classifier must keep video files out of every music list (Songs, Albums, Artists, Genres, Composers, Shuffle Songs). `.m4v` isn't mapped at all yet and must be added for video only.

**Where the lists live:** the Videos lists need precompute sections. Either raise `LIBFILE_MAX_SEC` (now 32, `apps/ipodui/libfile.h`), or keep the video lists in their own small file in `VIX_DIR` (`player.md` §9 hook 2). The About page's Videos count and the capacity legend's Video segment come from the same classification.

## 5. The 7G Videos tree, mapped to the calls

The spec doesn't photograph Videos yet. This tree is from the 7G's documented 2.0.x menus and **needs owner photos** (`docs/reference/7g/owner/`) before the brief freezes it. ⚠️ marks unverified rows.

| 7G (2.0.5) | ProkBox | Calls |
|---|---|---|
| Movies | list from §4 | `vplayer_start` + audio on SELECT |
| ⚠️ Rentals | hidden (no rentals) | — |
| TV Shows › show › episodes | list from §4 (show, season, episode) | as Movies |
| Music Videos | list from §4 | as Movies |
| Video Podcasts › podcast › episodes | list from §4 | as Movies |
| ⚠️ Video Playlists | if playlists hold videos | as Movies |
| Settings | Settings › Videos (below) | — |

**Settings › Videos** (7G 2.0.5, `player.md` §9 hook 4):

| Row | ProkBox |
|---|---|
| Start Playing (Where Left Off / From Beginning / Ask) | the resume path: the audio's resume position |
| Fit to Screen | `OPEN` mode `FIT` or `NATIVE` (`hostproto.md`) |
| ⚠️ Alternate Audio, Captions, Subtitles | hidden until supported |
| TV Out, TV Signal, Widescreen | hidden until TV-out exists (`approach.md` workstream 4) |

**The video screen:** full-screen with no header, the picture owned by the BCM. SELECT shows the overlay (§2), MENU exits, and the wheel controls volume, or the scrubber after SELECT, as on the 7G.

## 6. Rules the screens must keep

- **Native illusion (rule 1):** no visible "Rockbox", "BCM", "VideoCore", "vix" or codec names. A video that can't play just plays as audio, and the reason goes to the log (`ui_log("video", …)`, as the player already does).
- **Strings (rule 8):** every label through `LANG_*`. The 7G short labels ("Movies", "TV Shows", "Music Videos", "Video Podcasts", "No Videos", "Fit to Screen") may match; full sentences are our own.
- **Efficiency (rule 3):** no browse-time file parsing. Lists come from the precompute; the only open-time parse is the missing-index fallback.
- **Hidden files (rule 1):** `.vix` files and `bcmvideo.bin` live under `/.rockbox/`.

## 7. Test hooks (simulator)

The simulator plays real video: `vbcm_model.c` stands in for our image, and the host-built `bcmfw/h264` decodes the pictures. Script commands (`uisimulator/common/sim_cmd_video.c`):

| Command | Does |
|---|---|
| `video_start <path>` | `vplayer_start(path)` only; start the audio next with the existing `play_files <path>`, as the screen will |
| `video_stop` | `vplayer_stop()` |
| `audio_pause on\|off` | pauses or resumes the audio, so the player follows |
| `video_shot <name>` | saves the current picture as a PNG |
| `expect_video <min-pictures> [idle\|playing\|asleep\|done\|failed]` | fails unless at least that many pictures went up with no errors, and the state matches (`done` also once the end was reached and the audio stopped) |
| `video_trace on` / `video_trace expect pass\|fail` | stage 6's per-picture trace and verdict (`vtrace.h`) |

Examples: `tests/video/sim_play.script` and `tests/video/sim_stage6.script`, run by `tests/video/sim_run.sh`. Its wall-clock waits are sensitive to machine load. Each Videos screen should get its own `tests/ui/ipodui-videos-*.script` with screenshots, as for every UI change.
