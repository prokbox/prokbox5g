---
title: "Audiobooks"
weight: 30
description: "Chapters, a saved place in every book, and a listening speed you choose."
---

Audiobooks have their own list, and ProkBox remembers where you are in every book separately.

## What counts as a book

The player files a track under **Audiobooks** if any one of these is true:

- the file is an `.m4b`;
- its genre is a book genre, such as Audiobook, Audio Book, Books, Spoken Word, or the Dutch and German equivalents (for example Luisterboek or Hörbuch);
- it sits in a folder called `Audiobooks` or `Audio Books`, at any depth (capital letters don't matter).

Audio podcasts are kept apart, in **Podcasts**. If a file is both, Podcasts wins. A track in an Audiobooks folder is treated as a book even if it is really music, so keep music out of that folder.

## Listening

1. Choose **Music**, then **Audiobooks**. (You can also add Audiobooks to the main menu in **Settings › Main Menu**.)
2. The book you listened to most recently is at the top, so getting back to it never needs scrolling.
3. Choose a book. Its page lists the chapters. Once you've listened to part of it, a **Resume** row appears at the top.
4. Choose **Resume** to carry on where you stopped, or choose a chapter to start there.

![Audiobook chapters](../images/audiobooks-audiobook-chapters.png)
<!-- screenshot: scripts/docs_shots.sh; generator in tests/ui/docs/images.tsv -->

Starting a book plays only that book, not the whole list.

## On Now Playing

![Audiobook Now Playing](../images/audiobooks-audiobook-now-playing.png)
<!-- screenshot: scripts/docs_shots.sh; generator in tests/ui/docs/images.tsv -->

- The bar covers the **whole book**, with small tick marks where chapters start.
- The line under the title shows the current chapter, and a counter such as "3 of 12".
- If a book has pictures for its chapters, you see the one for the current chapter.
- **Next** / **Previous** jump a whole chapter, not a few seconds.
- **Select** steps through the scrubber and the rating (no shuffle for books). Turning the wheel quickly in the scrubber snaps from chapter to chapter, which is handy for crossing a long book.
- Turn the wheel in the normal view for volume.

## Resume

Play and Now Playing are for sound only (music, audiobooks and audio podcasts), so a book resumes just as music does. Videos are separate; see [Video](video.md).

Close the book, play music, switch the player off: your place in each book is kept. Open the book again and choose **Resume**.

## Speed

1. Go to **Settings › Audiobooks**.
2. Choose **Slower**, **Normal** or **Faster**.

The voice speeds up or slows down without changing pitch, so it does not sound squeaky or deep.
