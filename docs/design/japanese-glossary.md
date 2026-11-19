# Japanese UI glossary (#266)

*The terms of ProkBox5g's Japanese shell (`apps/lang/japanese.lang`, the `ipodui:` lines), set against the Japanese words of Apple's iPod classic. Owner rules of 2026-11-10: short labels follow Apple's Japanese (they may match it exactly); full sentences are our own words with the same meaning and tone; strings that read the same as the English (names, units, format names, numbers) stay as they are; the legal texts stay in English. The owner does not read Japanese, so the quality comes from process: this glossary, then two independent reviews (§5).*

## 1. Source

| | |
|---|---|
| **A** | Apple, *iPod classic ユーザガイド* (Japanese iPod classic user guide, the 120 GB edition, 2008): <https://cdsassets.apple.com/live/6GJYWVAV/user/locale/ja-jp/ma630_ipod_classic_120gb_en.pdf> (the file name says "en"; the text is Japanese). Read 2026-11-10 with `pdftotext`. Page numbers below are the printed ones. |

Limits of the source:
- It is the guide for the 120 GB classic (6.5G), not the 160 GB 7G's; its menu words are those of the classic line, and 2.0.5 is assumed to use the same words [not confirmed: no Japanese 2.0.5 screenshot was found].
- A user guide quotes menu items in 「」 inside prose; it does not show every screen. Where it is silent, the term is ours (marked **ours**) in the style of the words it does show.
- Searched and not reached: manuals.info.apple.com's older guide addresses (they redirect to support.apple.com/docs). No Japanese review screenshots were used.

## 2. Style (from A)

- **Script.** Katakana loanwords for most nouns (ミュージック, プレイリスト, アーティスト, アルバム, バックライト, イコライザ); kanji for 設定, 情報, 写真, 曲, 言語, 音量制限, 日付, 時刻. A few words stay in Latin letters: **Podcast**, **Genius**, **Cover Flow**, **On-The-Go**, **Reset Settings**, **Reset**, the EQ preset names (A p. 33, "Rock") and **To Do** (p. 53).
- **Labels** are plain nouns or noun + を + verb stem (曲をシャッフル, プレイリストを保存). No polite ending on a label.
- **Sentences** are polite ます form, with 、 and 。 (full-width). Questions end in **？** (full-width). Menu paths are written 「設定」＞「情報」 (A p. 11).
- **Numbers** are half-width, with a space between a number and its counter, as A writes them (10 秒, 24 時間, 1 曲). A space also separates a Latin word from Japanese (On-The-Go に追加, Genius を開始).
- **Parentheses** in Japanese text are full-width （）.
- "..." after a progress label stays three ASCII dots, as in the English strings (the shell's fonts draw them; the ideographic ellipsis is not in ShellSans).

## 3. Terms

**Apple** = the word in A (page). **Ours** = A has no word for it; chosen to match A's style.

### Main menu and Music

| English (7G) | Apple Japanese (A) | ProkBox5g | Note |
|---|---|---|---|
| Music | ミュージック (p. 6) | ミュージック | |
| Videos | ビデオ (p. 6) | ビデオ | |
| Photos | 写真 | 写真 | |
| Podcasts | Podcast (p. 33) | Podcast | Latin, as Apple |
| Extras | エクストラ (p. 49) | エクストラ | |
| Settings | 設定 (p. 7) | 設定 | Rockbox's `LANG_SETTINGS` already says 設定 |
| Shuffle Songs | 曲をシャッフル (p. 6) | 曲をシャッフル | |
| Now Playing | 再生中 (p. 6, 27) | 再生中 | |
| Cover Flow | Cover Flow (p. 7) | **Album Flow** | our feature name (docs/legal.md: no Apple marks); Latin like Apple's |
| Playlists | プレイリスト (p. 29) | プレイリスト | |
| Artists | アーティスト (prose) | アーティスト | |
| Albums | アルバム (prose) | アルバム | |
| Songs | 曲 (p. 10) | 曲 | |
| Genres | – | ジャンル | ours (the iTunes word) |
| Composers | – | 作曲者 | ours (the iTunes word) |
| Audiobooks | オーディオブック (prose, p. 34) | オーディオブック | |
| Search | 検索 (p. 245 of the text: 「ミュージック」メニューから「検索」) | 検索 | |
| Compilations | コンピレーション (p. 28) | コンピレーション | |
| On-The-Go | On-The-Go (p. 29) | On-The-Go | Latin, as Apple |
| Add to On-The-Go | On-The-Go に追加 (p. 7, 29) | On-The-Go に追加 | |
| Remove from On-The-Go | On-The-Go から取り除く (p. 29) | On-The-Go から取り除く | |
| Clear Playlist / Clear | プレイリストを削除 / 消去 (p. 30) | プレイリストを削除 / 消去 | Apple's label kept; pass 3 preferred 消去 (the list is emptied, not deleted): open |
| Save Playlist | プレイリストを保存 (p. 29) | プレイリストを保存 | |
| Browse Album / Artist | アルバムをブラウズ / アーティストをブラウズ (p. 27–28) | same | |
| All Songs / All Albums | – | すべての曲 / すべてのアルバム | ours; Rockbox's `LANG_ALL` = すべて |
| Unknown Artist / Album | – | 不明なアーティスト / 不明なアルバム | ours |
| *n* songs | 1 曲 (p. 28) | %d 曲 | counter 曲 |
| *n* of *m* (Now Playing) | not found | %d / %d | ours, neutral; keeps the argument order |

### Now Playing

| English | Apple Japanese (A) | ProkBox5g |
|---|---|---|
| Shuffle: Off / Songs / Albums | シャッフル: オフ / 曲 / アルバム (p. 28) | same (`LANG_OFF` set to オフ) |
| Repeat: Off / One / All | リピート: オフ / 1 曲 / すべて (p. 28) | same |
| Chapter | チャプタ (p. 183 of the text) | チャプタ %d |
| Visualiser | – | ビジュアライザ (ours, like イコライザ) |

### Settings

| English | Apple Japanese (A) | ProkBox5g | Note |
|---|---|---|---|
| About | 情報 (p. 11) | 情報 | |
| Shuffle / Repeat | シャッフル / リピート | same | Rockbox's strings already say so |
| Main Menu | メインメニュー (p. 10) | メインメニュー | |
| Music Menu | ミュージック (p. 29) | ミュージック | as Apple; its caption is メニュー項目を編集 ("ミュージックメニューを編集" is 182 px on a 148 px caption) |
| Volume Limit | 音量制限 (p. 31) | 音量制限 | |
| Backlight / Always On | バックライト / 常にオン (p. 10) | same | Rockbox's 「バックライト点灯時間 (バッテリー使用時)」 is replaced |
| Brightness | 画面の明るさ (p. 10) | 画面の明るさ | |
| Audiobooks: Slower / Normal / Faster | – | 遅く / 標準 / 速く | ours |
| EQ | イコライザ (p. 33) | イコライザ | |
| EQ presets | English ("Rock", p. 33) | English | all 22 preset names unchanged |
| Sound Check | 音量の自動調整 (p. 32) | 音量の自動調整 | |
| Clicker | クリック音 (p. 8) | クリック音 | |
| Date & Time | 日付と時刻 (p. 49) | 日付と時刻 | |
| Date / Time / Time Zone | 日付 / 時刻 / 時間帯 | same | |
| 24 Hour Clock | 24 時間時計 | 24 時間時計 | |
| Time in Title | 時刻表示 | 時刻表示 | |
| Sort By: First / Last | 並べ替え: 名 / 姓 (p. 53) | same | |
| Language | 言語 (p. 10) | 言語 | Rockbox's 表示言語 is replaced |
| Legal | not found | 法的情報 | ours |
| Reset Settings / Reset | **Reset Settings** / **Reset** (p. 11, English on the Japanese iPod) | English | Apple keeps these two in English so a reader who picked a language they can't read can still find them; we do the same. The confirm heading and body are Japanese |
| On / Off | オン / オフ (p. 8) | オン / オフ | Rockbox's オンにする / オフにする are replaced |

### About

| English | Apple Japanese (A) | ProkBox5g |
|---|---|---|
| Serial Number, Model, Version | シリアル番号, モデル, (ソフトウェアの)バージョン (p. 11, prose) | シリアル番号 / モデル / バージョン |
| Free / Used | 空き容量 (p. 11, prose) | 空き %s GB / 使用済み %s GB |
| Audio / Video / Photos / Other | – | オーディオ / ビデオ / 写真 / その他 (ours) |

### Extras

| English | Apple Japanese (A) | ProkBox5g |
|---|---|---|
| Clocks | 世界時計 (p. 49) | 世界時計 |
| Alarms / Create Alarm | アラーム / アラームを作成 (p. 50) | same |
| Alarm rows: Date, Time, Sound, Beep, Repeat, Weekdays, Label, Delete | 日付, 時刻, 音色, ビープ音, リピート, 平日, ラベル, 削除 (p. 50) | same |
| Sleep Timer | スリープタイマー (p. 50) | スリープタイマ (fits the 86 px split header; owner may choose Apple's スリープタイマー with 2 px more header room) |
| Stopwatch | タイム計測 (p. 51) | タイム計測 |
| Stopwatch: Resume / New Timer | 再開 / 新たな計測 (p. 51) | same |
| Games | ゲーム (p. 51) | ゲーム |
| Contacts | 連絡先 (p. 53) | 連絡先 |
| Calendars / To Do | カレンダー / To Do (p. 53) | same |
| Notes | メモ (p. 53) | メモ |
| Screen Lock | 画面のロック (p. 52) | 画面のロック |
| Lock / Reset | ロック / リセット (p. 52) | same |
| New / Enter / Confirm Combination | 新しい番号 / 番号を入力 / 番号を確認 (p. 52) | same (also Volume Limit's combination) |
| Add (a clock) | 追加 (p. 49) | 追加 |

### Device and buttons

| English | Apple Japanese (A) | ProkBox5g |
|---|---|---|
| Menu button | 「メニュー」ボタン (p. 5) | メニューボタン |
| Select (centre) button | 「センター」ボタン (p. 5) | センターボタン |
| Connected | 接続されています (p. 12) | 接続中 (ours, shorter) |
| Charging | 充電中 (prose) | 充電中 |
| the device | – | never named (docs/legal.md: messages need no device name); "ProkBox" stays as is |

### Our own features (no Apple word)

| English | ProkBox5g |
|---|---|
| Artwork (updating, preparing ...) | アートワーク (the iTunes word, A p. 822) |
| Album Art | アルバムアート |
| Save Diagnostics | 診断情報を保存 |
| Movies / TV Shows / Season | ムービー / テレビ番組 / シーズン (A p. 606) |
| Subtitles | 字幕 |
| Playback Speed | 再生速度 |
| Game names Quadrus, Bricks, Chute | unchanged (our names); Solitaire ソリティア, Chess チェス (generic words) |
| Chess terms | チェック, チェックメイト, ステイルメイト, 引き分け, クイーン, ルーク, ビショップ, ナイト |

### 0.9.0 additions (first drafts, H0, 2026-11-13; not yet reviewed)

Marked `# draft-0.9.0` in `japanese.lang`. A has no word for any of these (none existed on the classic); all are **ours**, chosen in A's style.

| English | ProkBox5g | Note |
|---|---|---|
| Audiophile (Settings row) | 高音質 | "オーディオファイル" also reads as "audio file"; 高音質 is the everyday word for the idea. Review: is a label like オーディオ設定 clearer? |
| Hi-Res / Hi-Res 128x | ハイレゾ / ハイレゾ 128x | the common Japanese word for high-resolution audio |
| Stereo Width: Low / Medium / High | ステレオ幅: 弱 / 中 / 強 | an effect strength, not a pitch or level |
| Sample rate | サンプリングレート | |
| the audio chip | オーディオチップ | |
| Test finished / Testing... | テスト完了 / テスト中... | |
| Custom EQ (Extras) / Custom 1-3 (slots) | イコライザ編集 / カスタム 1-3 | EQ is イコライザ (A p. 33); カスタムイコライザ overflowed the header |
| EQ editor: Band, Frequency, Level | バンド, 周波数, レベル | |
| Bands: Bass, Low mid, Mid, Presence, Treble | 低音, 中低音, 中音, 中高音, 高音 | the standard five-band names; プレゼンス is studio jargon |
| narrow / wide | 狭い / 広い | |
| Button names in one-line hints | センター, メニュー, 再生, ホイール; "Hold Select" = センター長押し | short forms of A's 「センター」ボタン etc. A hint line has no room for 「」 or ボタン |
| Can't Play Song / Audiobook / Episode / Video | 曲を / オーディオブックを / エピソードを / ビデオを再生できません | the page header is full width, so the item kind fits |
| Protected file | 保護されたファイルです | a short statement, like 対応していない形式です |
| Preparing Your Library | ライブラリを準備中 | |
| Battery (first start): Stock | 純正 | |
| Alarm panel: In 5 minutes / In 18 hours 42 minutes / In less than a minute | あと 5 分 / あと 18 時間 42 分 / 1 分以内 | |
| Alarm panel: Fri 13 Nov, 7:00 | 金曜 13日 11月 7:00 | the code passes weekday, day, month, time in that order and Rockbox's printf can't reorder them, so the natural 11月13日(金) is not possible without a code change. Review |

## 4. Kept in English

- **Legal** (owner, 2026-11-10; `docs/legal.md` is normative and has no Japanese text): Settings › Legal (`LANG_IPODUI_LEGAL_1..6`) and the legal part of the first-boot notice: the heading "This is not Apple software" and paragraphs 1, 3, 4 and 5 (independence, trademark, copyright and licence, the source address). The notice's help line (`NOTICE_2`) and its "press the centre button" lines (`NOTICE_6`, `NOTICE_WAIT`) are Japanese, so a Japanese reader knows how to go on.
- **As Apple does:** Reset Settings, Reset, the EQ presets, Podcast, On-The-Go, To Do.
- **Same in every language:** ProkBox, ProkBox5g and the version string, AM/PM (after the time, as the shell draws them), OK, CPU, %d%%, the game numbers, the game names above.
- **Developer mode** (`ipodui_devmode`): English; never in a release.

## 5. How this was checked

- **Pass 1** (author): this glossary, then every string.
- **Pass 2** (independent reviewer, top model, given only the strings, their contexts and this glossary): grammar, naturalness, politeness and terminology.
- **Pass 3** (second independent reviewer): back-translation of every sentence to English, compared with the English source for meaning drift.
- **Machine checks:** `scripts/check_shell_langs.py` (every string translated, printf conversions kept, every character drawable: ShellSans or the ShellCJK cut and its fonts); the fit trace of `tests/ui/ipodui-p20-lang-ja.script` (no text cut with "...").
- **Results (2026-11-10):** pass 2 changed 15 strings (plus one from its notes: "Requested Covers" → 優先アートワークを処理中...); pass 3 back-translated 55 sentence-level strings with 0 drift and 0 tone findings, and raised 2 label findings: one taken (the Music Menu caption), one left open (プレイリストを削除, Apple's word, against 消去); two of its optional notes taken. The fit run cut 3 strings with "..." at the 7G widths; all three were shortened (Customise Main/Music Menu captions, the Music Menu row).
- **The date under the Extras clock** reads 「2026年11月10日」 (0.9.0, p090-locale): every shell date is a pattern string (`LANG_IPODUI_DATE_*`, fields such as `$Y` `$n` `$j`; `apps/ipodui/locale_core.h`), so each language sets its own order. Before that, printf's fixed argument order put the year last (「11月10日 2026年」).
- **Not checked:** a native speaker's reading. Suggestions go to prokbox.project@gmail.com (`docs/user/translations.md`).
