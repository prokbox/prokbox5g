---
title: "Translations"
description: "Deutsch, Nederlands and 日本語: first drafts, and how to send a better word."
---

ProkBox5g speaks **English**, **Deutsch**, **Nederlands** and **日本語** (Japanese). Choose one in **Settings › Language**. The new language shows straight away and you return to the main menu.

> [!NOTE]
> The German, Dutch and Japanese texts are **first drafts**. They have not yet been checked by native speakers. Some words may be stiff or wrong, and a few might differ from what you remember from your old iPod. The texts for the screens that are new in 0.9.0 (Audiophile, the custom equaliser, the alarm panel, the library page, songs that can't play) are the newest drafts of all.

## Short labels and full sentences

- **Menu items and buttons** use the everyday words people know from music players: *Musik*, *Wiedergabelisten*, *Einstellungen*; *Muziek*, *Afspeellijsten*, *Instellingen*.
- **Longer messages** are written fresh in each language, not translated word for word.
- **Some labels are shorter** than the usual phrase, so that they fit next to a value on the narrow left half of the screen. For example, the German Sound Check row and several equaliser presets.

## Japanese

- Menu words follow the Japanese words of Apple's iPod classic where we know them: *ミュージック*, *プレイリスト*, *設定*, *曲をシャッフル*.
- **Some texts stay in English on purpose:** the legal texts (Settings › Legal and the legal part of the start-up notice), and, as on Apple's Japanese iPod, *Reset Settings*, *Reset* and the equaliser preset names.
- Japanese needs the Japanese fonts that come with ProkBox5g. They are part of every download, so 日本語 is offered straight away. If they are missing, 日本語 is left out of Settings › Language rather than showing empty text.

## For translators: dates, numbers and shared words

- **Dates and times are patterns, not sentences.** `LANG_IPODUI_DATE_*` and `LANG_IPODUI_TIME_*` hold a pattern: `$j` day, `$n` month, `$Y` year, `$M` month name, `$D` weekday, `$l` weekday in full, `$t` the time, and so on (the full list is in each string's description). Put the fields in your language's order and add your own words and marks around them: German `$j. $M $Y`, Dutch `$j $_M $Y`, Japanese `$Y年$n月$j日`. A `_` after the `$` writes the name with a small first letter, for languages that write month and day names that way mid-sentence (Dutch: *13 nov 2026*).
- **The short numeric date** (`LANG_IPODUI_DATE_SHORT`) also sets the order of the date wheels in Settings › Date & Time and in the alarm editor.
- **The 12-hour time** (`LANG_IPODUI_TIME_12H`) decides where AM/PM goes: Japanese puts 午前/午後 first, and so does the AM/PM wheel.
- **The decimal mark** (`LANG_IPODUI_DECIMAL_SEP`) is used for every number the device prints itself: 44,1 kHz, 28,1 GB, 2,2 kHz.
- **The clock** (`LANG_IPODUI_CLOCK_DEFAULT`, "12" or "24") is the language's choice until the owner sets *24 Hour Clock*; after that, the owner's choice stays.
- **Words shared with Rockbox** (for example *Repeat*, *Shuffle*, *Always On*, the month and weekday abbreviations) come from the language file's `*:` lines, which the stock Rockbox build shares. A different word for the shell alone isn't possible yet: change the `*:` line, and check that the stock build still reads well. The tour scripts `tests/ui/ipodui-p20-lang-*.script` fail on any text that is cut with "...".

## Suggest a fix

You don't need to know anything about software. For each fix, tell us:

1. the language;
2. where you saw the text (for example *Settings › EQ*, or the message shown while the device is connected to a computer);
3. what it says now, and what it should say;
4. if it is cut off with "...", say so. Each row has a fixed width, so a shorter word is often the best fix.

Send it to **prokbox.project@gmail.com**, or open an issue at <https://github.com/prokbox/prokbox5g/issues>. A photo of the screen helps.

Want ProkBox5g in another language? Write to the same address. A new language needs about 600 short texts, plus a check that every one fits on the screen.
