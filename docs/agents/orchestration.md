# Orchestrating a milestone

For the agent running a milestone spec with subagents (the `mattpocock-skills:implement-spec` skill). These are the
user's standing rules: follow them without asking again. Where this differs from the skill, this wins.

## Setup

- **Shared notes dir**, outside the repo: `~/.cache/deluge-generative-<milestone>/`. `notes/` holds exploration notes
  (`explore-*.md`) and one handoff per ticket (`<ticket>.md`). Keep your own state in `orchestrator-state.md` there
  (per PR: review rounds, CI runs, agent threads; plus open questions for the user), so a fresh session can take
  over. Its first line reads: "On resume (a restart or a compaction), re-read `docs/agents/orchestration.md`, and
  list the scheduled tasks: delete an old heartbeat bound to a dead thread and start your own."
  A summary of the session keeps the work but drops these rules. Notes are for the milestone. A gotcha later
  agents need goes into the matching file under `docs/generative/implementation-notes/`, in the ticket's PR.
- **Milestone README**: `README.md` in the notes dir, short, for every subagent to read after the repo docs. Only
  facts about this milestone; a rule that holds for every milestone goes into the repo docs instead, and the repo
  doc wins where the two differ. It names:

  ```markdown
  # <milestone>: what's specific to this milestone
  Read AGENTS.md, this file's briefs, CODING_STANDARDS.md and (for hardware) the deluge-hardware-test skill first.
  - Spec issue: #<spec>. Tickets are its sub-issues.
  - Integration branch: `<integration>`, its draft PR into `generative`: #<PR>. Use it as `--base`.
  - Your branch: `<milestone>/<ticket>-<slug>`. PR body: `Part of #<spec>`, a `Closes #<ticket>` line per ticket.
  - The orchestrator's worktrees (never touch): <paths>.
  - The Deluge: <what's on the card, the build it was flashed with, anything odd about it now>.
  - Size and budget rules: <limits this milestone may move, and how (e.g. H18 in spec §20)>.
  ```
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

1. Read `AGENTS.md`, the tickets and the spec sections they link. For the code you'll touch, read the files
   `docs/generative/implementation-notes.md` lists for it and its handoffs in `notes/`.
2. Check `git merge-base --is-ancestor origin/<integration> HEAD`; if it fails, `git fetch origin && git switch -C
   <branch> origin/<integration>` (`git reset` is blocked for agents).
3. Build test-first with the `tdd` skill. It asks you to confirm the test seams with the user, which a subagent can't
   do: choose the seams yourself (prefer existing ones) and list them in your report.
4. Before reporting: merge `origin/<integration>`, run `python3 scripts/generative/check_all.py --base <integration>`
   until it passes, push with `git delugepush -u <branch>`, and open a PR into the integration branch
   (`gh pr create --base <integration>`; body from the `mattpocock-skills:pr` skill, with `Part of #<spec>` and a
   `Closes #<ticket>` line per ticket). It runs what the change needs: the probe scripts whose `# covers:` paths
   changed, with smoke and `test_port_smoke.txt`, and no firmware build or delugemu when no firmware input, emulator
   check code or probe script changed. A `src/` change takes 2 to 8 minutes, by the scripts it picks, which can pass Bash's 600 s limit: run it in the
   background as `check_all.py ... > LOG 2>&1; echo "check_all exit=$?" >> LOG` and wait for the `check_all exit=`
   line as item 7 says (each step prints a `[check-all]` line as it starts and ends). Use `--quick` while iterating.
   A new probe script gets a `# covers:` line of the paths it checks (`scripts/generative/covers.py`).
5. Hardware runs, when the ticket needs them: the `deluge-hardware-test` skill.
6. Write `notes/<ticket>.md`, short: where the code lives, seams later tickets extend, what's deferred, choices the
   spec left open, human checks.
7. Report: PR URL, branch and head SHA, tickets done, the CI run (URL and state), checks run with results (the
   `check_all.py` table), hardware runs, human checks left, anything deferred. Under **Tooling problems**, list any
   tool, skill, script or repo doc that got in your way (wrong, missing, flaky) and what you did instead; don't work
   around it quietly. The user's rule: tooling gets fixed during the build. Don't end the turn just to wait for CI: report the run and let the
   orchestrator check it. Your own commands (a hardware session, `check_all.py`, a long build) are the opposite:
   finish them in the same turn. Prefer the foreground with a long Bash timeout (up to 600000 ms, repeated). For a
   background one (Bash `run_in_background` with `timeout: 7200000`), wait on its log with Monitor and an
   until-loop on its own marker (`until grep -q 'check_all exit=' LOG; do sleep 30; done`; a foreground `sleep`
   is blocked), with `timeout_ms: 1800000`, re-armed on expiry until the line appears. A background task that
   ends without its `exit=` line (killed) has ended too: report it with the log's tail. `t3_background_wait` is
   only a quick check: it can return at once ("matched") while the job still runs, and it fails when called
   without `taskIds`. To read a failed CI job's log
   when `gh run view --log` prints nothing: `gh api --allow-escape-sequences
   repos/gismya/deluge-generative/actions/jobs/<id>/logs`.
   Never make a status message ("waiting for …") your last output: the turn ends there.
8. On review findings: fix on the same branch, push, and report what you changed and what you declined, with the
   reason. Before fixing a MUST, look for other cases of the same bug (the same route, state or class of input) and
   fix those too; list what you checked. In a fix round run `check_all.py --quick` plus the probe scripts the fix
   touches. Run `check_all.py --full` and any hardware run once more only when the orchestrator says the PR is
   ready to merge. Add each round's changes and choices to `notes/<ticket>.md`: a later round may start in a fresh
   session that has only the notes, the PR and the findings. The final runs go to a runner (see below), unless the
   orchestrator asks you for them.
9. Keep your context small: every later call re-reads all of it. Send long command output (builds, `check_all.py`,
   probe and hardware logs) to a file and read only its tail or its failures. Grep first, then read only the line
   ranges you need: don't `cat` or read whole docs or source files.

## Reviews and merging

- Review each PR with one full round from each reviewer, **one at a time**, with a fresh agent per round:
  `mattpocock-skills:code-review` first, then the built-in `code-review`. Send fixes to the implementer's thread;
  make small ones yourself.
- **Fresh context past 250k tokens.** Before sending a fix round or the final run, check the implementer's context
  (the latest call's input plus cache tokens, in its session transcript): `python3 scripts/agents/agent_context.py`
  prints it for each recent session, with its worktree to match it to its thread. Under about 250k tokens, send the
  round to the same session. Past 250k, send it with a fresh context (`freshContext: true`) and a self-contained
  brief: the PR, branch and head, `notes/<ticket>.md`, the implementer brief, and the findings in full. Every turn
  re-reads the whole context, so a long session costs more per round than a fresh one costs to start.
- **Re-checks read the fix only.** Each PR gets one full review 1 and one full review 2. After a fix round, one
  delta reviewer re-checks, with the built-in `code-review` at medium effort, on `git diff <reviewed head>..HEAD` and
  the MUSTs the round answered. A full round again only when the fix rewrites the design (or adds a new stock edit),
  not for its size alone. `mattpocock-skills:code-review` (it starts two sub-agents) runs only full rounds.
- **No review for a NIT-only round.** Read its diff yourself, then go to the runner.
- **Final runs go to a runner on a small model** (Haiku, medium effort), with the Runner brief below. They are
  mechanical: `check_all.py --full` and the hardware session on the final head, in the implementer's worktree. The
  runner never edits, pushes, diagnoses, retries a hang or clears a marker. Failures come back to you; send fixes to
  an implementer.
- Merge (yourself, into the integration branch) when each reviewer is happy by the rules above, the final full
  `check_all.py --full` passed, and CI is green. A run stuck in the queue may be ignored; a failing one is fixed.
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
  have reported. Run the two halves with `run_in_background: false`, or wait for both in the same turn.
- Finish every command you start (a probe, a build, a sub-agent) in the same turn. Never make a status message
  ("waiting for Spec…", "the probe is running") your last output: the turn ends there, and the orchestrator reads it
  as the verdict. This holds for every review, whichever skill runs it.
- Raise a MUST only for a behaviour bug against the spec or the ticket. Everything else is a NIT, with file and fix.
- End with a verdict: HAPPY, or CHANGES with the MUSTs.

**Runner brief** (include in each final-run prompt, with the worktree, the expected head and the commands):

- Check that the worktree is clean at the expected head; if not, report what you see and stop.
- For the hardware session, the device's state is the milestone README's `- The Deluge:` line, a list item, not a
  section: read it with
  `grep -n -A3 '^- The Deluge:' <notes dir>/README.md`.
- Start each command in the background (Bash `run_in_background` with `timeout: 7200000`) with its exit code
  appended to its log, since a redirected background run loses `$?`. Exactly:

  ```
  python3 scripts/generative/check_all.py --full ... > LOG 2>&1; echo "check_all exit=$?" >> LOG
  ./dbt exec "python3 scripts/generative/deluge_probe.py session --build hardware ..." > LOG 2>&1; echo "session exit=$?" >> LOG
  ```

  `--full` runs every check and probe script whatever changed: the final run before a merge into `generative` or a
  milestone branch always has it. Keep the `echo` outside the `./dbt exec` quotes, or `$?` is expanded too early.
  Both can pass Bash's 600 s limit: `check_all.py --full` takes about 10 minutes, and a session waits for the lock,
  builds, loads and runs every script.
- The hardware session runs the scripts the brief names: `test_port_smoke.txt` plus those `python3
  scripts/generative/covers.py --hardware --base <integration>` prints for the change, and every hardware script
  (`covers.py --hardware --full`) only on a milestone's final run.
- Wait with Monitor and an until-loop on that command's marker (`until grep -q 'check_all exit=' LOG; do sleep 30;
  done`; a foreground `sleep` is blocked), with `timeout_ms: 1800000`, re-armed on expiry until the line appears.
  The `exit=` line is the only sure end: `check_all: PASS` can carry a suffix (`PASS (1 passed only alone, ...)`),
  and an early exit or a traceback prints no verdict line at all. A background task that ends without its `exit=`
  line was killed: that is an end too; report it with the log's tail.
- Start a hardware session only after `check_all.py` has ended, so the two don't compete for the CPU. Run it with
  `--build hardware`: other builds reconfigure `build/` (`check_all.py`'s docstring says why).
- When the PR touches timing, CPU or memory and the brief asks for a comparison with its base, run the sessions with
  `scripts/generative/hw_pairs.py run` (base and PR alternately, the base in a worktree at the PR's merge base) in
  place of a single session, the same way in the background with `echo "hw_pairs exit=$?" >> LOG`, and report
  `hw_pairs.py summary` on its logs. The skill's "Comparing a PR with its base" has the details.
- Stay in the turn while a run is going: ending it ("still running, I'll report") is read as the result. Your last
  message is the report: the `exit=` line, then the results table and any failing lines verbatim, from the log's
  tail.

## While waiting

- Keep a heartbeat while subagents run, and while paused: a T3 `schedule_task` bound to your thread, every 55
  minutes (`{type: 'interval', everyMs: 3300000}`). It outlives the session, unlike a `CronCreate` job; delete it
  when the milestone ends. Without T3, use a one-shot `CronCreate` 55 minutes ahead, re-armed each time it fires.
- Never block on a human. Record checks only a human can do (listening, feel, power cycles) in the handoffs and the
  draft PR body, and questions for the user in `orchestrator-state.md`.
