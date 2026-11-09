# Orchestrating a milestone

For the agent running a milestone spec with subagents (the `mattpocock-skills:implement-spec` skill). These are the
user's standing rules: follow them without asking again. Where this differs from the skill, this wins.

## Setup

- **Shared notes dir**, outside the repo: `~/.cache/deluge-generative-<milestone>/`. `notes/` holds exploration notes
  (`explore-*.md`) and one handoff per ticket (`<ticket>.md`). Keep your own state in `orchestrator-state.md` there
  (per PR: review rounds, CI runs, agent threads; plus open questions for the user), so a fresh session can take
  over. Notes are for the milestone. A gotcha later agents need goes into `docs/generative/implementation-notes.md`
  in the ticket's PR.
- **Integration branch** per milestone (e.g. `m1-perform-techno`) on `origin`. One PR per ticket (or group of 1–3
  tickets) into it. After the first merge, open a draft PR from it into `generative`; after each merge, update its
  body (`Closes` lines, what's merged, human checks left).

## Implementers

- A fresh implementer per ticket group, in its own worktree on `<milestone>/<ticket>-<slug>` from the integration
  tip. Run the first wave of unblocked tickets in parallel; after that, **one implementer at a time** (fewer build
  clashes, less risk of hitting the session limit).
- Pass each implementer the NITs earlier reviews left in the code it will touch.
- Implementers don't close, edit or comment on tickets: you do.

**Implementer brief** (include in each prompt):

1. Read `AGENTS.md`, the tickets and the spec sections they link. For the code you'll touch, read its sections of
   `docs/generative/implementation-notes.md` and its handoffs in `notes/`.
2. Check `git merge-base --is-ancestor origin/<integration> HEAD`; if it fails, `git fetch origin && git switch -C
   <branch> origin/<integration>` (`git reset` is blocked for agents).
3. Build test-first with the `tdd` skill. It asks you to confirm the test seams with the user, which a subagent can't
   do: choose the seams yourself (prefer existing ones) and list them in your report.
4. Before reporting: merge `origin/<integration>`, run `python3 scripts/generative/check_all.py --base <integration>`
   until it passes, push with `git delugepush -u <branch>`, and open a PR into the integration branch
   (`gh pr create --base <integration>`; body from the `mattpocock-skills:pr` skill, with `Part of #<spec>` and a
   `Closes #<ticket>` line per ticket). The full `check_all.py` takes over 10 minutes, past Bash's 600 s limit: run it
   in the background with its output in a file, and poll that file (each step prints a `[check-all]` line as it
   starts and ends). Use `--quick` while iterating.
5. Hardware runs, when the ticket needs them: the `deluge-hardware-test` skill.
6. Write `notes/<ticket>.md`, short: where the code lives, seams later tickets extend, what's deferred, choices the
   spec left open, human checks.
7. Report: PR URL, branch and head SHA, tickets done, the CI run (URL and state), checks run with results, hardware
   runs, human checks left, anything deferred. Don't end the turn just to wait for CI: report the run and let the
   orchestrator check it. Your own commands (a hardware session, `check_all.py`, a long build) are the opposite:
   finish them in the same turn. Prefer the foreground with a long Bash timeout (up to 600000 ms, repeated). For a
   background one, poll its log in foreground Bash calls (timeout up to 590 s each) until it ends. `t3_background_wait`
   is only a quick check: it can return at once ("matched") while the job still runs.
   Never make a status message ("waiting for …") your last output: the turn ends there.
8. On review findings: fix on the same branch, push, and report what you changed and what you declined, with the
   reason. Before fixing a MUST, look for other cases of the same bug (the same route, state or class of input) and
   fix those too; list what you checked. In a fix round run `check_all.py --quick` plus the probe scripts the fix
   touches. Run the full `check_all.py` and any hardware run once more only when the orchestrator says the PR is
   ready to merge. Add each round's changes and choices to `notes/<ticket>.md`: a later round may start in a fresh
   session that has only the notes, the PR and the findings. The final runs go to a runner (see below), unless the
   orchestrator asks you for them.
9. Keep your context small: every later call re-reads all of it. Send long command output (builds, `check_all.py`,
   probe and hardware logs) to a file and read only its tail or its failures. Read files in the ranges you need.

## Reviews and merging

- Review each PR with one full round from each reviewer, **one at a time**, with a fresh agent per round:
  `mattpocock-skills:code-review` first, then the built-in `code-review`. Send fixes to the implementer's thread;
  make small ones yourself.
- **Fresh context past 450k tokens.** Before sending a fix round or the final run, check the implementer's context
  (the latest call's input plus cache tokens, in its session transcript). Past 450k tokens, send with a fresh
  context (`freshContext: true`) and a self-contained brief: the PR, branch and head, `notes/<ticket>.md`, the
  implementer brief, and the findings in full.
- **Re-checks are scoped.** After a fix round, only the reviewer whose round raised the MUSTs re-checks, with the
  built-in `code-review` at a lower effort, on `git diff <reviewed head>..HEAD` and those MUSTs. A reviewer whose
  latest round was happy stays happy, unless the fix is big: a new stock edit, a design change, or about 150 lines
  or more. Then it gets a full round again. `mattpocock-skills:code-review` (it starts two sub-agents) runs only
  full rounds.
- **Final runs go to a runner on a small model** (Haiku, medium effort). They are mechanical: the full
  `check_all.py` and the hardware session on the final head. The runner works in the implementer's worktree (clean,
  at the final head), runs the exact commands the brief gives, and reports results verbatim. It never edits, pushes,
  diagnoses, retries a hang or clears a marker. Failures come back to you; send fixes to an implementer.
- Merge (yourself, into the integration branch) when each reviewer is happy by the rules above, the final full
  `check_all.py` passed, and CI is green. A run stuck in the queue may be ignored; a failing one is fixed.
- Right after merging a ticket's PR into the integration branch, close each of its tickets, first commenting "Done in
  #<PR>, merged into the integration branch; reaches the default branch with the draft PR". Closing unblocks the
  tickets blocked by it; the draft PR's `Closes` lines stay only as a record.
- **NITs never hold up a merge.** Hand each to the next implementer working in that code, or append it to
  `nit-backlog.md` (source PR and round, file, problem, fix). A separate implementer fixes the backlog in batches, in
  its own reviewed PR.
- When every ticket is merged, run both reviews on the integration PR.

**Reviewer brief** (include in each review prompt):

- The diff is `git diff $(git merge-base origin/<base> HEAD)...HEAD`. Drop findings outside it.
- Build and run the harness only with `./dbt test --generator [--sanitize]`; run no cmake directly.
- With `mattpocock-skills:code-review`, give the verdict only after both halves (the Standards and Spec sub-agents)
  have reported. Run the two halves with `run_in_background: false`, or wait for both in the same turn. Never make a
  status message ("waiting for Spec…") your last output: the turn ends there, and the orchestrator reads it as the
  verdict.
- Raise a MUST only for a behaviour bug against the spec or the ticket. Everything else is a NIT, with file and fix.
- End with a verdict: HAPPY, or CHANGES with the MUSTs.

## While waiting

- Keep a heartbeat while subagents run, and while paused: a one-shot `CronCreate` 55 minutes ahead, re-armed each
  time it fires (a recurring cron can jitter by up to 30 minutes).
- Never block on a human. Record checks only a human can do (listening, feel, power cycles) in the handoffs and the
  draft PR body, and questions for the user in `orchestrator-state.md`.
