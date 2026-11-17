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
  always starts on BUILD INTO A DROP: a genre whose transition entry is Intro (Ambient) needs a section-rule field and
  `TransitionConfirm::setupAndCheckAvailability()` to read it. Incoming Outputs don't get unique names yet (§9.7):
  Perform's song is never saved.
- **Chord-playing flavours (Sustain, Stab, Arp, Motif):** read `Generator::chord()`, the voiced chord on the last
  decided step in steps from the tonic, and resolve each note with `noteFor`; `chordRoot()` is its degree. A genre
  walks once its Harmony slot points at a `ChordGraph` (`harmonyValid()` in `genre_data_spec.cpp` checks it).
- **New flavours:** a `decideX` beside `decideLoop` and `decideSustain` in `core/generator_flavours.cpp`, marked
  `GEN_STEP_CODE` (`decidePart()` calls it from another file, and LTO may not inline it there: unmarked, it lands
  outside the step's section), a branch in `Generator::decidePart()` (`core/generator.cpp`), and the flavour's roles in
  `genre_data_spec`'s role/flavour table.
  **New interplay rules:** a `RuleKind` and a case in `Generator::against()` (`core/generator_patterns.cpp`).
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
- **Long sample Risers (M2):** a Riser longer than its Build starts with it (`riserStart()` returns the note's own
  tick), so its end overshoots the Drop and the Drop's note-off cuts it. House's 4-bar and Dub's 1-bar Builds make it
  real: start partway into the file (the adapter's `play()` would need a start offset, `PerformDriver::holdRiser()`
  knows the time left). Compose and Capture (M3, M4) write the Riser's note late the same way, from their tempo
  automation, with `riserStart()`.
- **Fills:** the Roll kind (Psytrance) is a `FillKind` and a case in `fillRules()`, playing like `playRoll()`.
  Drop-outs (Dub) beat a fill on a part or lane they share: clear it from `Step::rest`/`Step::swap`.
- **Drop-out (M2, spec §8.8, #88), what it touches:** a new `Step` mask and its rule beside the fills; one line in
  `Generator::silenced()` (so `keptOut()`, `lateEntries()` and the dim see it) and one in `decideKit()` beside the
  Rest. A part a Drop-out silences dims its MUTE pad, as the cut and the Gap do (triage of #259): putting it in
  `silenced()` does that through `updateSilent()`'s `mayPlay()` test, but not for a part's roll lane while the Build
  rolls: `mayPlay()` counts a rolling lane as sounding whatever the test says. A Drop-out that can stop the roll lane
  must change `mayPlay()`'s roll branch too. Like the cut and the Gap it stays out of the Arc plan, which reads only the
  keeps of the section rules.
- **Roll fill (M2, Psytrance), what it touches:** note-source work in `fillRules()`, `decideKit()` and
  `playRoll()`. Whether it obeys gates is the Psytrance slice's call. If it plays whatever the gates, as the Build
  roll does, it goes in `mayPlay()` (`core/generator.h`) beside `rollsIn()`: the dim and the Arc plan both ask it, so
  a part only it plays neither dims nor drops out of the plan.
- **Capture's builds (M4):** the runner has one next slot ([runner.md](runner.md)), and the leave job can wait in it
  (`Runner::cancelAllThen()`). A Capture build queued behind `LeaveJob` would replace it there: chain it from the leave
  job's `finished()`, or give the runner a larger next slot.
