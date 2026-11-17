# Implementation notes: Extension points left open

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- **LOAD (M3):** runs `TransitionLifecycle` unchanged, as a genre change does: add a call beside
  `Transitions::change()` that says what to preload, and have `TransitionJob::run()`
  (`generative/firmware/transitions.cpp`) take the file's Recipe, seed and takes instead of rolling them. The lifecycle
  never reads the pending Recipe. The handover is the steering's (`PerformSteering::handOver()`, `takeOverStopped()`),
  and the handover's macro rule lives there: each caller passes `HandoverMacros::Live` today
  (`PerformAdapter::incomingStarts()`, `incomingCommits()`, `pendingCommitsOnLeave()` and the harness's
  `TransitionAdapter`); a LOAD passes `Pending`, the file's. `Pending` isn't the whole of a LOAD's macro rule: at the
  start step the incoming instance's `carryOver()` (`PerformDriver::switchToIncoming()`) copies the live macros into
  it, which a LOAD has to skip, and the steering's `macroSteering_` has to restart from the file's macros
  (`MacroSteering::startFrom()`, as `PerformSteering::pieceStarts()` does), or it steers from the live ones. The confirm
  always starts on BUILD INTO A DROP today. M2 (#125) adds `ArcSpec::transitionEntry` (Drop or Intro) and the confirm
  (`gui/context_menu/generative.cpp`, `TransitionConfirm::setupAndCheckAvailability()`) highlights the incoming genre's;
  a LOAD reads the file genre's. Incoming Outputs don't get unique names yet (§9.7):
  Perform's song is never saved.
- **Chord-playing flavours (Sustain, Stab, Arp, Motif):** read `Generator::chord()`, the voiced chord on the last
  decided step in steps from the tonic, and resolve each note with `noteFor`; `chordRoot()` is its degree. A genre
  walks once its Harmony slot points at a `ChordGraph` (`harmonyValid()` in `genre_data_spec.cpp` checks it).
- **New flavours:** a `decideX` beside `decideLoop` and `decideSustain` in `core/generator_flavours.cpp`, marked
  `GEN_STEP_CODE` (`decidePart()` calls it from another file, and LTO may not inline it there: unmarked, it lands
  outside the step's section), a branch in `Generator::decidePart()` (`core/generator.cpp`), and the flavour's roles in
  `genre_data_spec`'s role/flavour table.
  **New interplay rules:** a `RuleKind` and a case in `Generator::against()` (`core/generator_patterns.cpp`): Avoid
  (#318), with its anchor check in `genreValid()`, and Respond (#321), whose half-bar count is new step state.
- **Replay (M4):** `Generator::ended()` reads the pending list, which replay doesn't have (comment on `ended()`).
  Glides log their length in `Command::length`. Jumps aren't logged. At a transition's start step the incoming
  instance's `carryOver()` (the live macros and the hold, `PerformDriver::switchToIncoming()`) isn't a logged
  command: the start entry (§17) has to carry both, or replay starts the new piece on the pending Recipe's macros.
  A transition's confirm in the 12 ticks between a sound's SetSound landing and its Sound event drops the swap in
  firmware (`SoundSwapLifecycle::cancelAll()`, then `swap()` finds no token), but the core has logged the SetSound:
  the log says a sound changed that never played (#261 decision 2, pinned in `sound_swap_spec`). M4 settles what the
  steering log says there. It also needs the new pick's path for each landed token: tokens wrap at 255.
- **Capture (M4):** the early return under Perform in `PlaybackHandler::recordButtonPressed()` becomes the Capture
  mark. Before it saves Perform's song, its Outputs need unique names (§9.7): an incoming roster's, and a sound
  re-roll's new Output, can share a preset name and folder with one still playing or retiring.
- **Long sample Risers:** a Riser longer than its Build starts with it (`riserStart()` returns the note's own tick), so
  its end overshoots the Drop and the Drop's note-off cuts it. M2 doesn't build a start offset (a Compose note can't
  start a sample partway, and the offset would differ per Build length and tempo): each genre's Riser lane lists only
  pack Risers that fit its shortest Build at its fastest tempo, which a `check_all.py` check over `pack/` holds (#131).
- **Fills:** the Roll kind (#127a) is a `FillKind` and a case in `fillRules()`, playing through `playRoll()` with its
  own stage and velocity (spec §8.7).
  Drop-outs (Dub) beat a fill on a part or lane they share: clear it from `Step::rest`/`Step::swap`.
- **Drop-out (M2, spec §8.8, #88), what it touches:** a new `Step` mask and its rule beside the fills; one line in
  `Generator::silenced()` (so `keptOut()`, `lateEntries()` and the dim see it) and one in `decideKit()` beside the
  Rest. A part a Drop-out silences dims its MUTE pad, as the cut and the Gap do (triage of #259): putting it in
  `silenced()` does that through `updateSilent()`'s `mayPlay()` test. A Drop-out never meets the Build roll (Groove
  only, never drawn across a Build's start; decided in #130's grilling), so `mayPlay()`'s roll branch doesn't change.
  Like the cut and the Gap it stays out of the Arc plan, which reads only the keeps of the section rules. It is drawn
  a bar ahead, so the throw target's last beat before it can spike (spec §8.8).
- **Roll fill (M2, #127a), what it touches:** note-source work in `fillRules()`, `decideKit()` and `playRoll()`. It
  obeys gates, mute and keeps as a Swap does (decided in #127's grilling), so `mayPlay()` doesn't change: the dim and
  the Arc plan never count it.
- **Capture's builds (M4):** the runner has one next slot ([runner.md](runner.md)), and the leave job can wait in it
  (`Runner::cancelAllThen()`). A Capture build queued behind `LeaveJob` would replace it there: chain it from the leave
  job's `finished()`, or give the runner a larger next slot.
