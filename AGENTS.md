# AGENTS.md

This repo is gismya's fork of the [Deluge community firmware](https://github.com/SynthstromAudible/DelugeFirmware). It adds **Generative mode** (see `GLOSSARY.md`).

- Generative work lives on the `generative` branch (the default), based on upstream tag `v1.3.0`.
- `main` tracks upstream. Fetch upstream with `git fetch upstream`.
- Push the commits you make; don't leave them only in your local checkout. You don't need to ask first.
- To push, use `git delugepush <args>` (e.g. `git delugepush -u generative`), never `git push`. Plain `git push` is blocked for agents. The alias runs `git push origin <args>` and refuses unless `origin` is gismya/deluge-generative, so it can never push to upstream.
- To download files, use Node's built-in `fetch`, e.g. `node -e 'fetch(process.argv[1]).then(r=>r.ok?r.arrayBuffer():Promise.reject(new Error(r.status))).then(b=>require("fs").writeFileSync(process.argv[2],Buffer.from(b)))' <url> <out>`. An org-level policy blocks `curl` and `wget` (even mentioning them in a command), and it can't be changed per project.
- Run the hooks on the files you changed before `git add`: `./dbt exec "pre-commit run --files <paths>"`. The commit hook rejects unformatted files, so a commit without this fails and has to be redone. ruff and clang-format exist only inside `./dbt exec`.
- The org policy that blocks `curl` and `wget` also denies `rm` commands. Delete a file with Python's `os.remove` and a directory with `shutil.rmtree`, or move them to `/tmp`.
- To show a change leaves the default build unchanged, run `./dbt sizediff`: it compares the Release build's sizes and symbols with the merge base on the PR's base branch (before a PR: the branch's upstream, else `generative`; `--base` picks another).
- When a task is done (its PR merged, or its commits pushed to `generative`), clean up after it: remove the worktrees it used, including subagents' worktrees, and delete its local branches and its merged branches on `origin` (`git delugepush --delete <branch>`). Check each worktree for uncommitted changes first. Leave worktrees and branches that belong to other work alone.
- When you fix firmware code that isn't specific to Generative mode, add an entry to `docs/upstream-candidates.md`.
- Target hardware is OLED only. The design should stay upstreamable where that is cheap, but upstreaming is not a goal.
- Before working on Generative mode code, read the files `docs/generative/implementation-notes.md` lists for the code you touch; add the traps you hit to the matching file.
- Orchestrating a milestone with subagents: read `docs/agents/orchestration.md`.

## Checks

CI runs only the **Run tests** workflow (`tests.yml`), on every PR. Hardware is never in CI. Details are in spec §21 (`docs/generative/spec/21-testing.md`).

- `python3 scripts/generative/check_all.py` runs every check below but hardware in one go, with one table.
- Merge a PR (into `generative` or a milestone branch) only when its **Run tests** check is green, never while it is red. No branch protection enforces this.
- On every Generator change, run the harness specs: `./dbt test --generator` (`--sanitize` as in CI).
- A change to a genre's event stream (a retune) fails its golden. When it's meant: in the genre's table (`src/deluge/generative/core/genres/techno.cpp`), raise the `genre.version = N;` line after `subgenreOf(...)` to N + 1 (once per branch: a second retune before the merge keeps it), run `./dbt test --generator --update-goldens`, commit the new goldens with it and name the genre in the commit (spec §21.2).
- Before pushing a change under `src/` that touches Generative mode, run each delugemu check the repo has against a local `./dbt build release` (spec §21.3): the smoke checks (`python3 scripts/generative/gen_emu.py smoke`), and the firmware-matches-harness check once it exists. If the script warns that the installed delugemu differs from the pinned one, say so in the PR.
- For changes to UI, timing, memory or CPU, put a hardware run in the test plan: the `deluge-hardware-test` skill, over the sysex test port.

## Agent skills

### Issue tracker

Issues live in GitHub Issues on **gismya/deluge-generative** (via the `gh` CLI), never on the upstream SynthstromAudible repo. See `docs/agents/issue-tracker.md`.

### Triage labels

Default vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
