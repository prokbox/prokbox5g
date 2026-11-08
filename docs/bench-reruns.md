# Bench reruns: what the next sitting still has to answer

After sitting 4gh (2026-11-06, build `56cd4e2096`). Owner direction: the next sitting runs only the rows whose answer is still open, in the shortest order that answers all of them. Results and evidence: `hw-experiments.md` §8, `pp5022-reference/clocks-power.md` §11–12, `pp5022-reference/interrupts-timers.md` §6, `design/core-layout-decision.md` §5.2.

## Open: run these (about 35 min, about 5 of them with the owner's hands on)

Order: basic to advanced; the music-stopped rows first, then the rows that play music. Before the playing rows, start an **album** (audio files, not a video: the #154 bug made 4gh's playing rows play a video's soundtrack).

| # | Row | Why it is open | Owner time |
|---|---|---|---|
| 1 | `pldo` (codec part) | 4gh's screen answer was a slip ("Blank"; the screen was fine), so the row skipped its codec part (`0x24` to mode `010`, #42). The screen part runs again as its gate: answer "Normal" | ~1 min, one look and one listen |
| 2 | `psrf`, music **stopped** | 4gh never ran it: "the disk or the music started again" (#39, the self-refresh request/acknowledge) | ~1 min, hands off after stopping the music |
| 3 | `p1` | 4gh's 0 FIQs were not a route verdict: nothing ever cleared the COP's FIQ mask (quirk 74). FIQ on the COP is our own extension (Apple's COP never takes source 26), so the rebuilt row tests it step by step, one log line each (`interrupts-timers.md` §6.3): start-up F bit, the route, one piece with F masked (where source 26 shows), the COP's own unmask (the pending FIQ taken), 10 s idle (~215 FIQs on the COP) | ~1 min: a short blip, a brief gap, then a steady 10 s tone |
| 4 | `pcop` (only after `p1` passes) | auto-skipped in 4gh (needs `p1`): `pcm-pp.c`'s own driver with the FIQ on the COP | ~1.5 min: a 60 s tone, one question |
| 5 | `awlg`, music **playing** | 4gh's run had nothing playing (`playing=0`, no fill in 893 s), so AW-1's linger never acted; its token stays off until this passes | ~15 min, hands off (Hold on) |
| 6 | `duty`, arm F | arm C was read in 4gh; arm F logged "playback did not start" at its codec load on the COP, so the core layout decision still lacks its key input (D-1). The row runs C F C F; only F's numbers are new | ~15 min, hands off (Hold on) |

## Not on the list

- **`k1`**: its 4gh data settles it. The row printed FAIL only because its old rule wanted 0 pcm rescues in the baseline arm as well; both arms read 44 (decode on the CPU, so not the park's doing). Under the corrected A/B rule (parked no worse than pulsed, no underruns) 4gh reads PASS: COP wakes 100.5 → 0.6/s, 3 tick pulses in 150 s, 0 underruns. The 44 rescues per 150 s are a refill-pacing finding for the codec placement work, not a K-1 question. One caveat: 4gh's audio was a video's soundtrack and the owner heard it end and fall silent around this step; `playing=1` at both windows' ends says it ended after them.
- **`awi2`** (AW-5, 2 late resumes of 6): failed on its merits; rerun only after a fix to the ungate's order.

## Done: don't rerun

`pgrd`, `preg`, `prtc` (S-6: no), `pstl` (S-8: no), `pden` (nothing to cut), `p2` (P-2 pass), `awld` (token set), `awus` (token set), `awd2`/`awd2c` (AW-3 answered for audio), `s1`/`l5`/`ck` (unless the BCM image changes them), `duty` arm C (read).

## Left to the BCM session

`b13` warm swap (the panel off after a swap), `vs` (no idle saving in video, dropped and skipped frames), `sd` (unavailable: the chip's cache is on), `uviz` stutter, `vplay` ("video check invalidated: decode-error (1); no token").

## Where they run (2026-11-06, `p19-4gh-b5-triage`)

In *Chip update checks* (batch 5's session, 24 steps), after its 16 VC steps, by the same drop-proven rule: `preg` early (read-only; psrf's starting words), `pldo` on the resident image before `jvpu`, then after every video row (each puts a clip in the audio playlist, #154): `p1`, `pcop`, `psrf`, `alb` (new: the owner plays an album outside the session; `awlg` and `duty` need its "Yes"), `awlg`, `duty`, with `sd` last (its cut). `k1` is left out.

- **psrf:** a pause kept the playback engine's buffering alive, so its refill could wake the disk inside the row's window (4gh's "the disk or the music started again"). The row now stops the music itself, noting the spot, and starts it there after.
- **duty arm F, "playback did not start":** what 4gh played was a film's soundtrack (#154). Arm F's codec load went to the COP (`codec place cop<-cpu load=1`), and no PCM came within `play_from`'s 5 s; the music came back only when the row put the decode back on the CPU. The log holds nothing more, so why the COP run didn't start is [hypothesis]: an AAC track inside a 10-minute film (a large MP4 index, every read a cross-core call) on the COP's first run. The row now refuses a video's soundtrack ("Not run: a video's sound is playing"), gives arm F 15 s to start (5 s on the CPU), and on a failure logs the audio status, the PCM, the codec's run counts on each core and the place, so a repeat names its cause. `awlg` refuses a video's soundtrack too.

## After sitting 4gh-b5 (2026-11-06, `p19-next-bench`)

*Chip update checks* is 25 steps: `heap` (new) runs after `duty`, before `sd`.

- **sd's panic, buflib "handle unaligned":** `sd`'s first allocation set off a compaction that met a corrupt block header; the damage came earlier, most likely in `duty`'s arm F [hypothesis]. Developer builds now walk the heap (`apps/ipodui/heapchk.h`: every header's length, handle, back-pointer and a CRC word) at every step's start and end, around every audio-buffer shrink and give, and at each codec place, COP run end and unload; a "heap walk at=..." line each, "heap BAD ..." lines for the first failure with the block before it. buflib's own panic now names the block before the bad one. New row `heap` (~7 min, hands off): an album decoding on the COP (arm F) then the CPU (arm C), every 2 s an allocation of a varied size (three live, so holes, compactions and audio-buffer shrinks come), the walk and every live block's bytes checked. Its verdict names the first bad block and its arm.
- **vs, idle=0 in every phase:** the idle share is the chip clock governor's, which starts only with a band between the crystal and 150 MHz verified for this image (`bcmpassvc.txt`, vbv's flags); the new image had none. `vs` now runs the band check itself first when the flags don't name one (about 2 min more, once per image).
- **awlg, no refill in 20 min:** the album fitted the 64 MB buffer. The row now logs the bytes queued from the playing spot against the buffer and, short of three buffers' worth, adds the albums beside the playing one (its folder's parent, then that one's parent, never the root); it also logs how long one buffer lasts at the playing bitrate (a low-bitrate library can still see too few refills in 20 min: [hypothesis] until the line is read).
- **psrf:** the skip names which condition tripped (the disk, the music, or both) with both flags and the audio status.
- **pldo:** the screen's supply found already in mode 010 (the lcdldo token, dark) no longer skips the row: the sound part runs and decides.
- **Budgets** from sitting 4gh-b5's `ms=` (s05 190 s, s4b 5, s4c 32, jobs 1, vf0 1, jvpu 1, s1 95, b13 8, b5 9, s6 43, vs 350 with its band check, p1 17, pcop 67, preg 1, pldo 20, psrf 6, awlg 1204, duty 859); `boot`, `b3`, `sdc` and `alb` run outside the session (ms 0) and keep theirs.
- **Fast rows say what they checked:** `jobs` (1 FILL, 1 FENCE, 1 word read back), `jvpu` (12 words), `preg` (26 PP registers, 7 PMU supplies), and the heap walk's line at each step's edges.
