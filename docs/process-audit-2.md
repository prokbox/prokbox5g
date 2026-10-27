# Process audit #2 (after Phases 4 and 4b)

Date: 2026-10-07. Scope: where wall-clock time and agent tokens went in Phase 4 (old process) and Phase 4b (the `docs/process.md` lifecycle adopted after audit #1), and what to change next. The goal is unchanged: **less time and fewer tokens for the same or higher fidelity.**

Sources: `docs/process.md`, `CLAUDE.md`, `docs/ROADMAP.md`, the Phase 4, 4b and nav briefs, `git log` for `eab82807b1..df20b24cad`, Forgejo #3, #24, #38, #41, #42, `scripts/validate.sh`, `scripts/shots.sh`, `.claude/agents/*`, and orchestrator figures (not in the repo).

## 1. What the new process fixed

| Signal | Phase 4 (old) | Phase 4b (new) |
|---|---|---|
| Design Highs found | 2 failed end reviews (F1–F4, then N1–N2), ~45–60 min and 250–400k tokens each | 3 Highs at brief review (70k tokens), confirmation 18k |
| Review cost | ~150–180k per full review | ~50–100k per scoped review |
| Gate | 16 min serial | 8.6 min parallel |

The brief review and scoped reviews paid off. The remaining waste is in **spec churn**, **contract drift**, **agent loops**, **golden re-captures** and **cold builds**.

## 2. Where 4b still lost time and tokens

1. **The owner's reference data arrived after the brief.** The owner's 61 photos plus video were folded in at 16:40, during Phase 4. The 4b brief followed at 18:49. Then:
   - Revision 3 (18:58) added the white strip and per-list row counts.
   - Revision 4 (19:02) withdrew two-line Songs.
   - Revision 5 (19:11) put two-line Songs back.
   - The thumbnail went from 40 to 55 px *after* B had built the cache. That cost `67d6857125` (cache version 2), `3bb49fdffa` (reservations redone after the fix merge) and the strip budget being recomputed three times (160 → 224 → 246 KB).

   Each revision went to agents that were already running, so they had to re-read the brief and redo work.
2. **TEMP stubs were the contract for a file format.** The album table existed only as prose in Revision 1 D6. A and B each stubbed it, the stubs disagreed, and a reconciliation integration pass was needed (`a40da27720` "reads library.c's album table (libfile.h)"). There were also 5 TEMP commits and 5 reverts (`a2584a42ce` … `47c79c0bf1`, including a TEMP-A that was reverted twice).
3. **Agents looped.**
   - Three process commits were added mid-phase, between 19:34 and 19:42: `aefd805660` hand back once, `1a9d20302a` one gate per worktree, and `75cdd4a102` subshells look like duplicate gates.
   - The tooling agent spent 365k tokens, partly on re-verification.
   - These rules are prose in `process.md` and nothing enforces them.
4. **The sim modelled device timing wrong.** The art builder could never run during playback on the device (the end review's Medium). Fixing it took a fix branch, a re-review and the #41 lows.
5. **Goldens were captured three times.**
   - `0512f8c54a`: the art-variety library changed the existing views.
   - `62ccb44c2b`: the owner-approved 4b goldens.
   - `5824d16832`: re-captured 66 minutes later because of battery drift.
   - The order-dependent resume state was only fixed in the gate-speed branch afterwards.
6. **Every worktree builds from cold.** `ccache` is not installed. Each agent worktree builds its own `build/{fw,fw-ui,sim,sim-ui,boot,warble}`, about 750 MB. The header gotcha in `CLAUDE.md` (`touch apps/ipodui/*.c`) forces full rebuilds of the shell on every header edit.
7. **Prompts are heavy.** A typical specialist reads `CLAUDE.md` + `process.md` + `hardware.md` + `7g-spec.md` (36 KB) + `artcache.md` (23 KB), and sometimes `7g-catalogue.md` (52 KB). That is about 40k tokens before the brief, and agents re-read it after each brief revision or context compaction.
8. **Gate fixed costs repeat.**
   - Every gate (and every `--quick` run) primes the shared simdisk: it generates the library and builds and commits the database in the sim.
   - `shots.sh` polls its watchdog with `sleep 1`.
   - Agents run plain `--quick`, not `--quick --changed`.

## 3. Ranked changes (saving × confidence)

Savings are for one 4b-sized phase (about 4 specialists, ~2.5–3M tokens, ~5 h wall-clock).

| # | Change | Saving / phase | Fidelity | Effort | Enforcement |
|---|---|---|---|---|---|
| 1 | **Owner pre-flight before the brief.** The orchestrator lists every visual or behavioural unknown the phase touches: screens, row counts, pitches, sizes, wording. The owner answers or supplies photos *before* the brief is written, and the measurements go into a per-screen spec file (`docs/design/screens/<screen>.md`: geometry table, photo refs, "(owner, date)"). Anything unresolved is **frozen as ⚠️ with a default** and is not changed until the owner round. | 45–90 min, 300–500k tokens (4b revisions 3–5, cache v2, reservations redo, strip ×3) | **Up**: measured from photos once, not flip-flopped | Low | Brief template section "Owner pre-flight: answered / frozen ⚠️". Brief review FAILs a brief whose UI sizes have no source. Mid-phase owner input goes to the *next* phase unless it's a blocker. |
| 2 | **Contracts as real code before fan-out.** The orchestrator, or the agent that owns the contract, commits the real headers and on-disk format structs (`libfile.h`, `artcache.h`, magic, version, field layout and a host test that round-trips one record) plus **one** shared stub `.c`, on the phase branch. Agents branch from it. No per-agent TEMP stubs. | 30–60 min, 150–250k (the reconciliation pass, 5 TEMP/revert pairs) | **Up**: a format mismatch fails to compile or fails a host test | Low | Brief template: "Contracts: committed at `<sha>`" is required before fan-out. `validate.sh` refuses `TEMP-` commits on a phase branch at integration. |
| 3 | **Hard agent stop conditions, enforced by a script.** `scripts/gate-run` wraps `validate.sh` with a per-worktree `flock` (a second run exits at once with "already running") and counts runs in `.git/gate-runs`. The **third** `--quick` run in a worktree refuses and tells the agent to hand back with the failing scripts. Agent definitions get a "budget: hand back at ~250k tokens or 2 failed fix attempts on the same failure". Agents default to `--quick --changed`. | 150–300k tokens, 15–30 min (loops, duplicate gates, 365k tooling run) | Neutral: the integration gate still runs everything | Low | The wrapper script, plus one line in each `.claude/agents/*.md` (replacing the current prose rules). |
| 4 | **Fix rounds resume the owning agent** (`SendMessage` to the same agent ID) instead of starting a new agent that re-reads everything. The orchestrator does the mechanical merges. Only genuine cross-owner reconciliation goes to an agent, and that agent gets the two diffs, not the whole brief. | 50–150k per fix round (onboarding ~40k + exploration) | Neutral to up: context kept | None | Line in `process.md` step 4/5: "fix round = continue the owner's agent". |
| 5 | **Shared compiler cache across worktrees.** Run `brew install ccache`, configure with `CC="ccache …"` (or a `PATH` shim for `arm-none-eabi-gcc`/`gcc-16`), set `CCACHE_BASEDIR=$HOME/rockbox-for-me` and `CCACHE_NOHASHDIR=1` so worktrees share hits, and point `CCACHE_DIR` at one shared directory. Also fix the stale-dependency gotcha (`-MMD -MP` for `apps/ipodui`) so header edits rebuild only the files that include the header. | 5–10 min cold build per worktree × 4–6 worktrees, plus agent wait tokens; avoids full `touch` rebuilds | Up: stale-dependency bugs go away | Low–Med | `scripts/new-worktree.sh` sets up the builds. `validate.sh` prints the ccache hit rate. |
| 6 | **Golden policy: capture once, deterministically, after owner approval.** Agents commit *proposals* to `out/`, never to `tests/ui/golden/`. `scripts/golden_capture.sh` runs each script twice on fresh disk clones and refuses to write unless both runs are pixel-identical. `shots.sh` pins every time-varying input by default (clock, battery, charging, resume, drift phase) with explicit opt-outs. A test-library generator change bumps `VERSION` and recaptures in the same commit. | 20–40 min, 50–100k (two of 4b's three captures) | **Up**: no flaky goldens | Low | Script, plus a pre-commit or gate check that a golden change comes with a `golden_capture.sh` stamp. |
| 7 | **Smaller agent context.** Split `7g-spec.md` into per-screen files (see #1). Each brief lists **exact sections** to read ("artcache.md §2, §5, §11 only") and pastes the relevant checklist answers inline, so specialists skip whole design docs. Brief revisions are issued as a **delta message** to running agents, not "re-read the brief". | 40–100k tokens per agent-run (~150–300k per phase) | Neutral, if the slices are right | Med (one-off split) | Brief template: "Read list (≤ 3 files / sections)". |
| 8 | **Cache gate fixed costs.** Cache the primed simdisk (library plus built and committed database) keyed by `setup_library.py` VERSION + the hash of `apps/tagcache.c`, `apps/ipodui/library.c` and the sim binary's db code. Reuse it with an APFS `cp -c` clone. Replace the `sleep 1` watchdog poll in `shots.sh` with `wait` plus a background timer. | ~0.5–1.5 min per gate run × 10–20 runs per phase | Neutral (a key mismatch re-primes) | Low | Inside `validate.sh`. |

Also considered and not ranked:
- **Model tiering** looks right. The tooling overrun was a loop, not a tier problem (fixed by #3). Do **not** move C (visual) to Sonnet.
- **Sim device-timing stand-ins** are already in checklist item 4. Brief review should require each stub's timing model as a table row (fold into #2's template).

## 4. Expected effect

Changes 1–3 alone remove the largest 4b waste: about **0.5–1M tokens and 1.5–2.5 h per phase** at no fidelity cost (1 and 2 raise fidelity). All eight together come to an estimated **~0.8–1.4M tokens (≈30–45%) and ~2–3 h (≈35–50%) per 4b-sized phase**. The biggest uncertainty is how often mid-phase owner feedback is truly unavoidable.

## 5. Suggested brief template headers (enforced by brief review)
1. Goal / out of scope
2. **Owner pre-flight**: answered / frozen ⚠️ (with defaults)
3. **Contracts committed at `<sha>`**: headers, format structs, round-trip host test, shared stub
4. Work split and ownership
5. **Read list** per agent (≤ 3 files or sections)
6. Device-reality checklist, including a timing table for sim stand-ins
7. Golden plan: which scripts change, and when to capture (after the owner round)
8. Budgets: tokens and gate runs per agent, and the stop condition
9. Acceptance
