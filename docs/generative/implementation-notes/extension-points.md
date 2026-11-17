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
- **New flavours:** a `decideX` beside `decideLoop` and `decideSustain` in `core/generator.cpp`, and the flavour's
  roles in `genre_data_spec`'s role/flavour table. **New interplay rules:** a `RuleKind`
  and a case in `Generator::against()`.
- **Replay (M4):** `Generator::ended()` reads the pending list, which replay doesn't have (comment on `ended()`).
  Glides log their length in `Command::length`. Jumps aren't logged. At a transition's start step the incoming
  instance's `carryOver()` (the live macros and the hold, `PerformDriver::switchToIncoming()`) isn't a logged
  command: the start entry (§17) has to carry both, or replay starts the new piece on the pending Recipe's macros.
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
- **Capture's builds (M4):** the runner has one next slot ([runner.md](runner.md)), and the leave job can wait in it
  (`Runner::cancelAllThen()`). A Capture build queued behind `LeaveJob` would replace it there: chain it from the leave
  job's `finished()`, or give the runner a larger next slot.
