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
3. **Render the clips.** Each question gets one clip per option (A, B, C …), 20–60 s each, at fixed seeds so A/B is fair. Give a full-mix clip, plus an isolated one (the other parts muted with `--mute`) where the part is hard to hear. Before the first capture, read [`rendering.md`](rendering.md): it covers proving each option in the harness, checking each capture, cutting clips, writing a normal-mode song, and testing the page. How each kind of option is rendered:
   - **Macro or Recipe options** (Density, Complexity, Motion, key, tempo): `gen_emu.py capture` flags, against a local `./dbt build release`, on a card image with `pack/` overlaid, so the pack sounds play.
   - **Table options:**
     1. Make one change in the genre's table.
     2. Build it into its own `--build-dir`.
     3. Capture.
     4. Revert the change.

     Record each variant's exact diff in `variants.md` beside the page.
   - **Scenarios the Generator can't produce on demand** (a chord voicing, a given note set, a line without ghosts): write the notes into a song in the firmware's normal mode, with the same presets and kit rows, key and tempo. Capture that in delugemu, and mark the card "rendered in normal mode".
   - **A question about statistics over many pieces:** give it the closest audible example, or mark it "not renderable", with the reason.

   Run at most two delugemu on the host at once, one per session, so one capture at a time. Check each WAV's length and silence after its capture. Six emulators on the host at once (two sessions at three each) gave short, corrupt and partly silent WAVs, and gen_emu passed them. If a WAV is bad, wait for the other session's emulator to finish, then capture it again. Drop to one while another agent runs timing measurements (probe scripts, hardware-style sessions, `check_all --full`, the leak check): that one is theirs, since host load skews their H1 timings and culls. To find out, list the sessions (`ListAgents`) and ask the one running them to message you when they're done.
4. **Build the page.** Make one self-contained `index.html`: inline JS and CSS, no server, no external fetches, playable from `file://`.
   - **Instrument checks first:** one card per part or lane whose sound itself is in question. Each card holds short isolated clips across its palette picks.
   - **Then one card per question:** the ticket number, the current behaviour, the options, a "what to listen for" line, and the clips with loop toggles.
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
