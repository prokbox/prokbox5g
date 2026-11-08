---
title: "Making videos that play well"
weight: 900
description: "Appendix: convert movies, TV episodes and podcasts so they play well on the player."
---

Your player has a small 320 x 240 screen and a modest video chip. It plays one kind of video very well, so the easy way to get a good result is to convert your videos first. This page shows how, using the converter that comes with ProkBox5g: `prokbox-convert.py`.

## What the converter does

For each video you give it, the converter makes a new copy that is:

- 320 x 240 pixels (the screen's size),
- H.264 video, Baseline profile,
- AAC sound,
- saved as a `.m4v` file with a title, and a poster picture if you provide one.

It then files the copy into a tidy folder: `Movies`, `TV Shows/<Show>/Season N`, or `Podcasts/<Show>`. Your original files are never changed or deleted. Nothing is uploaded anywhere; everything stays on your computer.

If you run it twice on the same files, it skips the ones it already finished, so it is safe to re-run after adding more.

## What you need

- **Python 3**
- **ffmpeg** (this also installs `ffprobe`, which the converter uses)

How to get them:

- **macOS:** install Homebrew (brew.sh), then in Terminal run `brew install python ffmpeg`.
- **Linux:** use your package manager, for example `sudo apt install python3 ffmpeg` (Debian, Ubuntu) or `sudo dnf install python3 ffmpeg` (Fedora).
- **Windows:** install Python 3 from python.org (tick "Add Python to PATH") and ffmpeg (for example `winget install ffmpeg`). Open a new Command Prompt afterwards. Use `python` where the examples below say `python3`.

To check, run `ffmpeg -version` and `python3 --version`. Both should print a version number.

The script is `prokbox-convert.py`, in the `scripts` folder of the ProkBox5g download. The examples assume your terminal is inside that folder; otherwise put the full path in front of the script name.

## Common jobs

Converted files go to a folder called `ProkBox Videos` on your Desktop unless you choose another with `-o`.

**A movie**

```
python3 prokbox-convert.py --movie "My Movie.mkv"
```

Add `--title "A Better Title"` if the file name makes a poor title (this works only when you give one file). Add `--poster cover.jpg` to give it a poster.

**A TV episode**

```
python3 prokbox-convert.py --tv --show "My Show" --season 1 --episode 3 "episode3.mkv"
```

The result is filed under `TV Shows/My Show/Season 1/`. If your file names already look like `My Show S01E03.mkv`, the converter can often work out the show, season and episode itself, so `--tv` alone may be enough.

**A podcast**

```
python3 prokbox-convert.py --podcast --show "Show Name" "episode.mp4"
```

**Black bars, or fill the screen**

Most films are wider than the 4:3 screen. By default the converter keeps the whole picture and adds black bars above and below ("letterbox"). To fill the screen instead and lose the edges, add `--fill`:

```
python3 prokbox-convert.py --movie --fill "My Movie.mkv"
```

`--fill` crops from the centre to 4:3, so try one file first.

**Subtitles you can see**

Subtitles you can switch on and off during playback are not supported yet. Instead you can "burn" them into the picture so they are always shown:

```
python3 prokbox-convert.py --movie --burn-subs auto "My Movie.mkv"
```

`auto` uses the first subtitle track in the file, or a matching `.srt` / `.ass` file with the same name sitting next to it. To use a specific subtitle file:

```
python3 prokbox-convert.py --movie --burn-subs "My Movie.en.srt" "My Movie.mkv"
```

> [!NOTE]
> Burning in text subtitles needs an ffmpeg built with "libass" (the Homebrew one has it). If yours does not, the converter says so and converts nothing.

**A whole folder, or many files at once**

Give it a folder and it converts every video inside, sub-folders included:

```
python3 prokbox-convert.py --tv --show "My Show" --season 1 "Season 1 Folder"
```

It works on 2 videos at a time; `-j 4` raises that if your computer is fast. `-o "/path/to/folder"` chooses where the results go.

**See the plan first**

```
python3 prokbox-convert.py --dry-run --movie "My Movie.mkv"
```

This prints what would happen and writes nothing. `--force` redoes files that were already finished.

**Check a file you already have**

```
python3 prokbox-convert.py --check "some video.m4v"
```

This tells you whether the file will show pictures on the player and, if not, why.

## Where to copy the files

1. Connect the player to your computer with USB and wait for it to appear as a drive.
2. Open the converter's output folder (`ProkBox Videos` on your Desktop, or the one you chose).
3. Drag the folders inside it (`Movies`, `TV Shows`, `Podcasts`) onto the player's drive. Merging them with folders that are already there is fine.
4. Eject the player from your computer, then unplug it.

The player updates its library after you disconnect, and your videos then appear under Videos.

## If a video won't play

When a video can't be played, the player shows a **Can't Play Video** screen with a short reason:

- **Format not supported:** convert the file with the converter and copy it again.
- **Video too large:** the video is too big or too long. Convert a smaller or shorter version.
- **File damaged:** the copy may have been interrupted. Copy it again, and eject properly next time.
- **Video didn't start:** try once more, then convert the file again.

Still stuck? Please tell us, so we can fix it:

1. On the player, go to **Settings › About**, move to the last page and choose **Save Diagnostics**. This writes a small text file, `ProkBox Report.txt`, to the top level of the player's drive.
2. Connect the player to your computer and find that file.
3. Email it to **prokbox.project@gmail.com** (or attach it to an issue on the project's tracker). Tell us the video's name, where it came from, and the command you used to convert it. If you can, run `--check` on the converted file and include what it says.

The report holds the version, hardware details, and a short log. It does not contain your music or videos.
