# AGENTS.md

This repo is gismya's fork of the [Deluge community firmware](https://github.com/SynthstromAudible/DelugeFirmware). It adds **Generative mode** (see `GLOSSARY.md`).

- Generative work lives on the `generative` branch (the default), based on upstream tag `v1.3.0`.
- `main` tracks upstream. Fetch upstream with `git fetch upstream`.
- To push, use `git delugepush <args>` (e.g. `git delugepush -u generative`), never `git push`. Plain `git push` is blocked for agents. The alias runs `git push origin <args>` and refuses unless `origin` is gismya/deluge-generative, so it can never push to upstream.
- To download files, use Node's built-in `fetch`, e.g. `node -e 'fetch(process.argv[1]).then(r=>r.ok?r.arrayBuffer():Promise.reject(new Error(r.status))).then(b=>require("fs").writeFileSync(process.argv[2],Buffer.from(b)))' <url> <out>`. An org-level policy blocks `curl` and `wget` (even mentioning them in a command), and it can't be changed per project.
- Before each commit, run the hooks on the files you changed: `./dbt exec "pre-commit run --files <paths>"`. ruff and clang-format exist only inside `./dbt exec`.
- To show a change leaves the default build unchanged, run `./dbt sizediff`: it compares the Release build's sizes and symbols with the merge base on `generative`.
- When a task is done (its PR merged, or its commits pushed to `generative`), clean up after it: remove the worktrees it used, including subagents' worktrees, and delete its local branches and its merged branches on `origin` (`git delugepush --delete <branch>`). Check each worktree for uncommitted changes first. Leave worktrees and branches that belong to other work alone.
- Target hardware is OLED only. The design should stay upstreamable where that is cheap, but upstreaming is not a goal.

## Agent skills

### Issue tracker

Issues live in GitHub Issues on **gismya/deluge-generative** (via the `gh` CLI), never on the upstream SynthstromAudible repo. See `docs/agents/issue-tracker.md`.

### Triage labels

Default vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
