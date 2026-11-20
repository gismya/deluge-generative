---
name: listening-session
description: Listening sessions - a browser page of rendered clips where the owner judges a genre's open tuning questions and sounds by ear, then exports answers. Use when building a session for a genre's open questions, or when the owner pastes back a listening-session export.
---

# Listening sessions

A listening session is one round of ear checks for one genre, or for one set of questions that belong to no genre. It is a static page of audio clips with a card per question. The owner answers on the page and pastes the exported Markdown back to the orchestrator. Sessions are how §21.5's tuning loop (`docs/generative/spec/21-testing.md`) reaches the owner without a Deluge in hand.

Each session lives outside the repo, in `<notes dir>/tuning/<genre>/round-<n>/`. The notes dir is the milestone's, e.g. `~/.cache/deluge-generative-m2/`. Nothing in it is committed.

## Building a session

Delegate the build to one agent, with this section as its brief plus the genre and round.

1. **Collect the questions.** Gather the genre's open questions:
   - its open tickets titled `<Genre>: …` (the theory review's judgement calls);
   - the genre's comment on the tuning ticket (#133 for M2), its "Sound choices to judge" list;
   - its items on the hand-found sounds ticket (#313).

   Read §4.5's Tables paragraph for the genre and `notes/<ticket>.md` ("Levels", "Capture commands", "For #313"). In round 2 and later, start from the previous round's "Later rounds" list and its "can't tell" answers.
2. **Split into rounds.** A question whose answer depends on another answer waits for a later round. For example, a Lead's register depends on which Lead preset is kept, and a level depends on which sound is chosen. This round holds only independent questions. Each held question goes under "Later rounds", with what it waits on.
3. **Render the clips.** Each question gets one clip per option (A, B, C …), 20–60 s each, at fixed seeds so A/B is fair. Give a full-mix clip, plus an isolated one (the other parts muted with `--mute`) where the part is hard to hear. Build it with the repo's toolkit, `scripts/generative/listen.py` (`scripts/generative/listening/`): a session writes only its content file, variants and job files. Before the first capture, read [`rendering.md`](rendering.md): it covers proving each option in the harness, rough-render clips, keeping captures and links few, checking each capture, cutting clips, writing a normal-mode song, and testing the page.

   First pick each card's path. A delugemu capture costs a minute or more; the harness rough render takes seconds.
   - **delugemu, the real sounds:** a preset choice, a level, how a part sounds (shrill, ringing on, its register as timbre), FX and delay, what sound controls or the Drift sweeps do, the pack-less fallback, and the context clips (today, all proposed).
   - **Harness rough render, stand-in synths and the real kit samples:** questions about the notes only, such as arrangement (what enters when, Break and Build shapes, rests, fills), rhythm, harmony, voicing, density and a figure's shape. Mark these cards "rough render (stand-in sounds)". Give each one delugemu clip of today at the same Recipe, so the owner hears what the stand-in stands for. A "can't tell" on such a card moves it to delugemu next round.

   How each kind of option is rendered:
   - **Macro or Recipe options** (Density, Complexity, Motion, key, tempo): `gen_emu.py capture` flags, against a local `./dbt build release`, on a card image with `pack/` overlaid (`listen.py card`), so the pack sounds play. A new Recipe builds nothing: gen_emu patches it into a copy of the built firmware. On a rough-render card, the same flags to `generator_render`.
   - **Preset options** (which sound a part or lane plays): a palette override on the card (`listen.py card --palette`), one card per candidate set. No table change and no rebuild; see `rendering.md`.
   - **Table options:**
     1. Make one change in the genre's table.
     2. Prove it in the harness, and keep that variant's `generator_render`: a rough-render card's clips come from it.
     3. For a delugemu card, capture all of the variant's jobs in a row, from one runner's `--build-dir`.
     4. Revert the change.

     Record each variant's exact diff in `variants.md` beside the page.
   - **Scenarios the Generator can't produce on demand** (a chord voicing, a given note set, a line without ghosts): write the notes into a song in the firmware's normal mode, with the same presets and kit rows, key and tempo. Capture that in delugemu, and mark the card "rendered in normal mode".
   - **A question about statistics over many pieces:** give it the closest audible example, or mark it "not renderable", with the reason.

   Raw WAVs, card images, build dirs and temporary cuts go in a scratch folder outside the session folder; delete it once the page passes its browser test. The session folder keeps only the page, the m4a clips, the MIDI files, `variants.md` with its diffs, and `tools/` (the session's content, variants and job files).

   Run at most six delugemu on the host at once, across all sessions (`ListAgents` lists them). The toolkit gives each runner its own worktree, `--build-dir` and `--out` folder: a build dir follows its worktree's source, so only a variant costs a build there, not a Recipe. In #377, the same 85 s capture run 1, 2, 4 and 6 at a time gave the same sha256 every time, and every WAV passed `gen_emu.py check`. Six at once captured 13.2 s of audio per wall second, against 8.0 at four and 2.7 alone. An earlier run of six gave short, corrupt and partly silent WAVs because its captures shared an `--out` folder and read each other's audio (#376), not because of the load. Each capture checks its WAV (`gen_emu.py check`: its length, sound in the beat after each 16th where the harness starts a note, no corrupt run) and exits 1 if it is bad; capture again any that fails. Drop to one while another agent runs timing measurements (probe scripts, hardware-style sessions, `check_all --full`, the leak check): that one is theirs, since host load skews their H1 timings and culls. To find out, list the sessions (`ListAgents`) and ask the one running them to message you when they're done.
4. **Build the page.** Make one self-contained `index.html`: inline JS and CSS, no server, no external fetches, playable from `file://`.
   - **Instrument checks first:** one card per part or lane whose sound itself is in question. Each card holds short isolated clips across its palette picks. Each clip takes a verdict, keep/unsure/drop by default; a card asking something else per clip names its own answers (`verdicts` in `listening/page.py`'s schema, as `{"level": ["louder", "fine", "quieter"]}`), so the owner doesn't put them in the note (Ambient round 2's level and weight cards).
   - **Then one card per question:** the ticket number, the current behaviour, the options, a "what to listen for" line, and the clips with loop toggles.
   - **Card text is short and plain.** The owner reads it while listening, so each card must say in a few seconds what changes and what to listen for:
     - **What changes:** one sentence per option, in musical terms ("the bells play half as many notes, with empty bars"), not table names, variant ids or code.
     - **Listen for:** one line, naming the part and where in the clip ("the Pad at each chord change, from 0:08").
     - At most about 60 words of text per card, besides the clip labels. Measurements, harness results, diffs and caveats go in `variants.md`, not on the card; link it once at the top of the page.
     - Clip labels name the option and what's soloed ("Proposed, Pad alone"), not file names.
   - **Answer controls on each card:** a pick per option, "keep as is", "can't tell", and a free-text note. Answers are kept in `localStorage`.
   - **An Export button:** it produces one Markdown block with a Copy button. The block opens with the line `Listening-session answers (<genre>, round <n>, <session folder>): handle them with the listening-session skill.` That line makes whoever reads the pasted block load this skill. The block then lists, per card: the ticket, the pick, the note and the clips compared. It also carries the "Later rounds" list.
5. **Report.** The page's path, the questions and clips, the total size, the variants built, what couldn't be rendered and why, and the held questions. Leave the worktree clean.

**Done when:** every open question is either a card on this page, held under "Later rounds", or marked "not renderable"; every clip plays from `file://`; the Export block covers every card.

## Handling the answers

When the owner pastes an export back:

1. **Save it.** Save the block verbatim as `answers.md` in the round's folder.
2. **Act on each card:**
   - **An option picked:** apply its `variants.md` diff. All the round's changes for one genre go in one retune PR, reviewed like any other.
     - A genre with goldens follows **A retune** in §21.2: a version bump and new goldens.
     - An M2 genre has no golden until the owner signs it off on #133.

     Comment the decision on the option's ticket, with the PR, and close the ticket when the PR merges.
   - **"Keep as is":** comment "Owner: keep as is (listening session, round n)" on the ticket, and close it.
   - **"Can't tell":** leave the ticket open. Re-render the question next round with clearer clips: longer, isolated, or a starker contrast.
   - **A note asking for something no option offered:** check it against the ticket's scope. If it changes a spec beyond the ticket, ask the owner before building it.
3. **Plan the next round:** the held questions whose dependency is now answered, the "can't tell" ones, and any new questions the notes raised. Build it as above, as `round-<n+1>`.
4. **Report to the owner:** the changes made, with their PR, the tickets closed, and what the next round holds.

An export is not the owner's sign-off of the genre. Sign-off is said on the tuning ticket.
