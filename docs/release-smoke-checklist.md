# Release smoke checklist (0.8.x)

The owner's roughly 10-minute check of the **exact zip downloaded from GitHub**, run before the release is published (or, for a draft release, before it is made public). It is a quick check, not the full gate: `scripts/validate.sh` and the device rounds come first.

**Rules for this run**
- Use the zip as downloaded from the draft release, not a local build. Check its SHA-256 against `SHA256SUMS` first.
- Pick the variant that matches the unit under test (battery, storage, memory).
- Use a unit that has Apple's firmware as its recovery path (CLAUDE.md rule 5). Take a fresh backup first (step 1). Never run this on a unit without a go from the owner.
- Use a small test library (a few songs, one audiobook with chapters, one converted video), never the owner's full library.

Fill in the results line at the end. Tick or mark each step P (pass), F (fail) or S (skipped, with a reason).

## Steps

1. **Backup and install from the zip.**
   - Do: take a fresh backup of the unit's firmware partition and of anything that matters on its disk. Download the zip, check its checksum, unzip it, and install it following `docs/install-and-restore.md`.
   - Pass: the backup file exists and is non-empty; the checksum matches; the install finishes with no error; the unit restarts.

2. **First boot and the first-run notice.**
   - Do: let it boot. Read the notice. Press **Select** straight away, then again after 3 seconds.
   - Pass: the logo shows, then the notice with the heading "This is not Apple software". The first press does nothing; the press after 3 seconds continues to the main menu. The text shows the correct version (the one on the release). Settings › Legal shows the same text. Nothing on screen says "Rockbox" apart from Settings › Legal.

3. **Browse and play.**
   - Do: open Music, then Artists, then an album, and play a song. Turn the wheel for volume; skip forward and back.
   - Pass: lists scroll smoothly; the song plays with cover art; volume and skipping respond without a gap or stutter. Now Playing shows the right title, artist and time.

4. **An audiobook: resume and speed.**
   - Do: open Music › Audiobooks and start the test book. Skip one chapter. Stop partway through, play a song, return to the book and choose **Resume**. Then set Settings › Audiobooks to Faster and play.
   - Pass: chapter skipping lands on a chapter start; Resume returns within a few seconds of where you stopped; Faster is clearly quicker and the voice pitch is unchanged.

5. **Plug in while paused, copy a file, eject, check the library.**
   - Do: pause the music. Connect to the computer. Copy one new song onto the drive. Eject it on the computer and unplug.
   - Pass: the Connected screen shows without a crash or reboot; the copy completes; after unplugging, "Updating Library..." appears and finishes, and the new song is listed under Songs and plays.

6. **Play a converted video.**
   - Do: convert a short clip with `prokbox-convert.py`, copy it over as in step 5, then open Videos and play it. Pause it and press **Select** for the progress bar. Also try one unconverted file.
   - Pass: the clip plays smoothly with sound; pause and the progress bar work; **Menu** returns to the list. The unconverted file shows Can't Play Video with a reason and goes back to the list without a crash.

7. **The visualizer.**
   - Do: play a song. On Now Playing, press **Select** until the visualizer appears. Hold **Select** to change the style, then press **Select** to leave.
   - Pass: the screen fills with moving graphics that follow the music; the style changes; leaving returns to Now Playing; audio does not stutter at any point.

8. **Save Diagnostics, then check the file.**
   - Do: Settings › About, go to the last page, and choose **Save Diagnostics**. Connect to the computer and open `ProkBox Report.txt` at the top of the drive.
   - Pass: the row goes Saving... then Saved. The file exists and shows the right version, model, memory and storage. It holds no file names from the library and no personal paths.

9. **Power off and on.**
   - Do: hold **Play/Pause** to switch it off, wait a few seconds, switch it on. Then play the audiobook again.
   - Pass: it powers off and boots back to the menu without the first-run notice (the notice shows once per version); settings (speed, backlight) are kept; the audiobook still resumes.

10. **Apple firmware recovery and back.**
    - Do: restart (hold **Menu** and **Select** about 8 seconds), and as the screen goes dark hold **Menu**. Apple's software should start. Then restart normally.
    - Pass: Apple's software starts from the hold. A normal restart brings ProkBox back with the library and settings intact.

## Results line

Copy this into the release issue:

```
0.8.0 smoke | date: ____ | zip: ______________ | sha256 ok: [ ] | unit: ____ (battery __ / storage __ / RAM __)
[ ]1 install  [ ]2 first boot  [ ]3 browse+play  [ ]4 audiobook  [ ]5 usb copy  [ ]6 video  [ ]7 visualizer  [ ]8 diagnostics  [ ]9 power cycle  [ ]10 recovery
result: PASS / FAIL | failed step(s): ____ | notes: ____________________
```

A single failed step blocks the release until it is understood. Anything cosmetic goes in the release's Known issues.
