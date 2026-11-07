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

1. Read `AGENTS.md`, the tickets, the spec sections they link, `docs/generative/implementation-notes.md` and the
   handoffs in `notes/` for the code you'll touch.
2. Check `git merge-base --is-ancestor origin/<integration> HEAD`; if it fails, reset onto `origin/<integration>`.
3. Build test-first with the `tdd` skill.
4. Before reporting: merge `origin/<integration>`, run `python3 scripts/generative/check_all.py --base <integration>`
   until it passes, push with `git delugepush -u <branch>`, and open a PR into the integration branch
   (`gh pr create --base <integration>`; body from the `mattpocock-skills:pr` skill, with `Part of #<spec>` and a
   `Closes #<ticket>` line per ticket).
5. Hardware runs, when the ticket needs them: the `deluge-hardware-test` skill.
6. Write `notes/<ticket>.md`, short: where the code lives, seams later tickets extend, what's deferred, choices the
   spec left open, human checks.
7. Report: PR URL, branch and head SHA, tickets done, the CI run (URL and state), checks run with results, hardware
   runs, human checks left, anything deferred. Don't end the turn just to wait for CI: report the run and let the
   orchestrator check it.
8. On review findings: fix on the same branch, re-run `check_all.py`, push, and report what you changed and what you
   declined, with the reason.

## Reviews and merging

- Review each PR with `mattpocock-skills:code-review` and the built-in `code-review`, **one at a time**, alternating,
  with fixes between rounds and a fresh agent per round. Every PR gets at least one round from each. Send fixes to
  the implementer's thread; make small ones yourself.
- Merge (yourself, into the integration branch) only when each reviewer's latest round is happy and CI is green. A
  run stuck in the queue may be ignored; a failing one is fixed.
- **NITs never hold up a merge.** Hand each to the next implementer working in that code, or append it to
  `nit-backlog.md` (source PR and round, file, problem, fix). A separate implementer fixes the backlog in batches, in
  its own reviewed PR.
- When every ticket is merged, run both reviews on the integration PR.

**Reviewer brief** (include in each review prompt):

- The diff is `git diff $(git merge-base origin/<base> HEAD)...HEAD`. Drop findings outside it.
- Build and run the harness only with `./dbt test --generator [--sanitize]`; run no cmake directly.
- With `mattpocock-skills:code-review`, give the verdict only after both halves (the Standards and Spec sub-agents)
  have reported.
- Raise a MUST only for a behaviour bug against the spec or the ticket. Everything else is a NIT, with file and fix.
- End with a verdict: HAPPY, or CHANGES with the MUSTs.

## While waiting

- Keep a heartbeat while subagents run, and while paused: a one-shot `CronCreate` 55 minutes ahead, re-armed each
  time it fires (a recurring cron can jitter by up to 30 minutes).
- Never block on a human. Record checks only a human can do (listening, feel, power cycles) in the handoffs and the
  draft PR body, and questions for the user in `orchestrator-state.md`.
