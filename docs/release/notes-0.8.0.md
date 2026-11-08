# ProkBox5g 0.8.0 release notes (draft)

*The first public pre-release. It builds on 0.7.0-alpha. For which players have been tried, see `known-hardware-0.8.0.md`.*

> ProkBox5g is not Apple software. It is an independent, community-driven fork of Rockbox, and Apple Inc. has not made, approved, endorsed or supported it in any way. It contains no Apple software. iPod is a trademark of Apple Inc.

## The short version

Compared with 0.7.0-alpha, 0.8.0 switches video on, makes saving settings and logs safe against power cuts, fixes a crash when plugging in USB while music is paused, and fixes a start-up fault on some storage. Items marked [pending bench] depend on checks that are still running.

## Music and audiobooks

- Music, Now Playing, Search, Shuffle Songs, On-The-Go playlists and every audio format carry over from the alpha, with the same chapters, per-book resume point and pitch-preserving speed for audiobooks.
- The visualiser on Now Playing is drawn by the video chip from a more efficient routine. [pending bench] The final check of its speed-up on the player.
- The guide now explains that Resume appears once a book has a saved place.
- A video's sound stops when its screen closes, so it never lingers in the music queue (#154).

## Video

- **Videos (basic)** is on. It plays files made with the included converter, and shows a clear message when a file can't be played. The converter can burn subtitles into the picture.
- **A first-use check** runs once on each unit to confirm the video chip can hold its full speed. Until it has passed, video plays at Apple's stock clock, and a decode fault steps the clock down. The check is much quicker than in earlier builds and its result appears in Save Diagnostics.
- Stalled or refused video data now restarts playback instead of freezing, and a restart restores the video chip's clock setting.
- Photos stay hidden until a later 0.8.x.

## Battery life

- The processor speed is managed more closely while you listen, and the video chip's memory idles when nothing is drawn.
- Playback with the screen off runs on one core, with the second held in reserve.
- [pending bench] Battery figures for 0.8.0. We will publish real hours only from long-run logs, not estimates.

## USB

- Plugging the player in while music is paused no longer runs it out of memory. Playback stops first, then the drive appears.
- The player refuses a USB request addressed to a drive it does not have, rather than answering badly.
- Drag-and-drop in and out works as in the alpha: copy files, eject, and the library updates itself.

## Reliability

- **Settings are saved safely.** A changed setting is written within about two seconds, and a power cut can't leave the file half written.
- **Logs rotate safely.** A log is never cut short in place; the oldest is removed last.
- **Start-up fix.** Reading the drive's identity could fault at boot on some storage. That is fixed in every build.
- Hang and fault records survive a restart, so Save Diagnostics shows what happened.
- A release now refuses to build if its language file comes out empty.

## Language support

- The interface is **English only** in 0.8.0. The language system is in place, and accented Latin letters display correctly.
- Dutch and German are promised for 1.0, with their language files in the release zip. [pending owner]

## What is not in it yet

Photos, Album Flow, sleep and hibernation, soft subtitles you can switch on and off, and the games SDK. They are hidden, not removed.

## Known issues

- **First-use video check.** A video played before the unit's one-off check has passed (the test file missing, no memory for it, or an inconclusive result) runs the video chip at 150 MHz unchecked, which is the stock clock. If the video decoder faults during that play, the clock steps down and the next play checks again.
- **Videos not made with the converter** may play less smoothly. A 320×240 H.264 stream with four reference frames keeps its frames in the video chip's slower memory instead of its fast memory; the converter uses three. A fix is planned for the next video-chip update.
- **Big flash cards on a Mac (#81).** A very large flash card (the 1 TB class) may not mount on macOS without a helper. A one-time reformat of the card is the documented way round it for now, and a proper fix is planned for later. This is the most likely place for drag-and-drop to fail on a Mac.
- **30-pin dock builds.** Crash fixes for players that keep the dock connector are still open (#135). Use a dock build only if you are willing to report what happens. [pending decision]
- **Untested hardware.** Original hard drives, 32 MB players, original batteries and the original piezo have not been tried. See `known-hardware-0.8.0.md`.

## Install, report, go back

The guide covers install, Save Diagnostics and returning to Apple's software (hold Menu at start-up). Back up before you install.
