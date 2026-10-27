# PP band governor stage 2: proposed bench rows (for sitting 3)

**Status: a proposal.** The BCM session owns the sitting-3 sheet and `apps/ipodui/devbench.c`'s step table; nothing here is built into the bench yet. Design: `docs/design/pp-governor.md` §16 (stage 2: band 0 and band 1 at `DCDC1 0xEB`, band 3 at 100 MHz, the apply layer, the watchdog and the record). Branch `p19-pgov2`.

**Shape.** Practical viability, no meter (owner, D5): each row runs real use, asks at most one question with 2–4 wheel answers (the first is the good one), and takes its automatic verdict from the `gov s2` log lines (`pp-governor.md` §16.7). Rows use the existing `struct bench_step` fields (`id`, `title`, `say`, `say2`, `fn`, `ask`, `answers[]`, `flags`, `limit_s`, `budget_s`). Owner time about 15 minutes.

## Before the sitting: shadow in normal use (no bench time)

Put `pgov_on` and `pgov2_on` in the system folder of a developer build and use the unit normally for a few days. Nothing is applied; the logs answer, at no risk:

| Question | Log line |
|---|---|
| The unit's speed grade: does band 3 exist at all? | `gov boot grade=G max=M`, `gov s2 boot … grade=G b3=0/1` |
| `DCDC1` reads `0xEC` with mode bits `111` | `gov s2 boot … dcdc1=ec` |
| How much time stage 2 would spend low, and in band 3 | `gov s2 sum b0= b1= b2= b3= vlow=` (‰) |
| How many first frames would start at 30 MHz while the rail rises | `gov s2 tr … rse=N` |
| How often it would move the rail | `gov s2 tr vch=N` |

If `b3=0` (grade 2 or 3), rows `pg3`'s band-3 half is skipped by itself.

## The rows

The sitting's first row writes the markers and asks for a restart (the markers are read at boot); each later row checks `gov s2 boot mode=apply` and skips itself otherwise.

| id | Title | Flags | Time | What runs | Question (answers) | Automatic verdict (from the log since the row began) |
|---|---|---|---|---|---|---|
| `pg0` | Clock stage 2 on | `BENCH_F_AUTO \| BENCH_F_REBOOTS` | 30 s | Writes `pgov_on`, `pgov2_on`, `pgov2_apply`; the owner restarts | – | after the restart: `s2 boot mode=apply`, `dcdc1=ec`, `s2 rec … ok=1` |
| `pg1` | Dark play, low voltage | `BENCH_F_AUTO` | 6 min | A mixed playlist (MP3, FLAC, AAC, one 96 kHz FLAC) plays with the screen off for 5 min | "Did the music skip or click?" (No / Once / Often) | `s2 arm=1` within 70 s of boot; `vlow ≥ 500 ‰` over the dark stretch; `fb=0`, `dog=0` (`s2 tr`), no `clk strike` or `lit-strike` line |
| `pg2` | Wake and browse | manual | 3 min | Five times: press Menu, scroll fast through Songs for 20 s, open Now Playing, then leave it dark for 10 s | "Did anything feel slower than usual?" (No / Slightly / Yes) | `rse ≥ 5` (each wake started at 30 MHz and raised); `fb=0`, `dog=0` |
| `pg3` | Library and 100 MHz | `BENCH_F_AUTO` | 3 min | Music plays dark; the row starts a library check (the maintenance build's request) so ATA transfers run at the low voltage and the CPU saturates | "Did the music skip or click?" (No / Once / Often) | `fb=0`, `lockf=0`; where band 3 exists: one `s2 b3 loop_us=380–420 crc=ok` and `b3 > 0 ‰`; where it doesn't: `b3=0` and no `s2 b3` line |
| `pg4` | Fallback drill | `BENCH_F_AUTO` | 1 min | Music plays dark at the low voltage; the row holds the power thread for 400 ms with a raise owed (a developer hook, **not built yet**: `pgov2_drill()`), so the tick's watchdog must raise the rail over the raw path | "Did the music keep playing?" (Yes / Paused / Stopped) | `s2 tr … fb=1/5 dog=1` (5 = the watchdog), stage 1 applied after (`gov band=` lines resume), no `clk strike` line |
| `pg5` | Unclean end | `BENCH_F_REBOOTS` | 1 min | Music plays dark at the low voltage (`pg4` reverted; `pg5` restarts with `pgov2_apply` still present); the owner holds MENU + SELECT until it restarts | "Did it start up normally?" (Yes / Slower / Apple's screen) | after the restart: `s2 rec … strike=1 ok=0` (the next boot skips the low voltage), `arm=` never 1 this boot; the boot after that arms again |

`pg4` needs one small developer hook in `firmware/pgov.c` (block the apply for 400 ms after booking a raise) before the sitting; without it the row is dropped and the watchdog stays host-tested only. `pg5` proves the record on silicon; it is the only row that ends a session uncleanly and runs last.

## Pass, and what each answer means

- **Pass:** every question's first answer and every automatic verdict clean. Then stage 2 runs in normal use (the markers stay) and the `gov s2 sum` lines over the following weeks decide whether it reaches release (`pp-governor.md` §5.3's rule 4).
- **A click or skip in `pg1`/`pg3` with `fb=0`:** the voltage step reaches the audio path (the I2S block on the same die, §6.3); the step is left off (remove `pgov2_apply`) and the finding goes to `docs/pp5022-reference/quirks.md` with [silicon].
- **"Slightly" or "Yes" in `pg2`:** the raise's ~11 ms at 30 MHz shows; candidate fixes are a longer voltage dwell (fewer lows while lit) or an earlier raise on the wheel's first event.
- **Any `fb=` in `pg1`–`pg3`:** the unit keeps running at normal voltage by design; the code names the cause (§16.5); the record keeps that unit off until the developer reset.
- **A hang:** MENU + SELECT; the next boot counts the strike (that is `pg5`'s check, met early).

## For the ingest (tooling)

`scripts/bench_ingest.py` would read, per row, from `ipodui.log`: `gov s2 boot`, `s2 rec`, `s2 arm`, `s2 sum`, `s2 tr`, `s2 b3`, `s2 fb` (formats in `pp-governor.md` §16.7). No new results-file fields beyond each row's answer.
