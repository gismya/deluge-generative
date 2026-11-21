# Implementation notes: Extension points left open

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- **LOAD as a transition (#139, built):** `Transitions::load()` beside `change()`; the preload type is
  `Transitions::Incoming` (a genre, or a file and its name, copied into the job's own `LoadSlot`). The macro rule is
  `PendingRecipe::macros` (`HandoverMacros::Pending` for a LOAD): `handOver()` reads it and restarts `macroSteering_`
  from the file's macros, and the lifecycle passes it to `PerformDriver::queueIncoming(keepsMacros)` through the
  adapter's `incomingMacros()`, so `carryOver()` skips the live macros at the start step. A new kind of transition
  sets those two fields in its preload. Incoming Outputs don't get unique names yet (§9.7): Perform's song is never
  saved.
- **Chord-playing flavours (Sustain, Stab, Arp, Motif):** read `Generator::chord()`, the voiced chord on the last
  decided step in steps from the tonic, and resolve each note with `noteFor`; `chordRoot()` is its degree. A genre
  walks once its Harmony slot points at a `ChordGraph` (`harmonyValid()` in `genre_data_spec.cpp` checks it).
- **New flavours:** a `decideX` beside `decideLoop` and `decideSustain` in `core/generator_flavours.cpp`, marked
  `GEN_STEP_CODE` (`decidePart()` calls it from another file, and LTO may not inline it there: unmarked, it lands
  outside the step's section), a branch in `Generator::decidePart()` (`core/generator.cpp`), and the flavour's roles in
  `genre_data_spec`'s role/flavour table.
  **New interplay rules:** a `RuleKind`, a case in `Generator::against()` (`core/generator_patterns.cpp`), and its
  name in `ruleKindName()` (`tests/spec/generator/render/names.cpp`, which `theory_review.py` reads). `against()` is
  const: a rule with state keeps it in the Generator and updates it before the source decides, as Respond does.
  Avoid (#318): its anchor check is `avoidRulesValid()` (`core/genre.h`, in `genreValid()`), and
  `Generator::endAvoided()` ends a pitched source's notes on an avoided step (`avoided()`, called from
  `decidePart()`) and keeps late entries off it (`lateEntries()`). Respond (#321): `Generator::countHalves()` counts
  the target's halves on each half-bar's first step into `busyHalves_` (one `PartsAndLanes` per half, by target:
  `respondMask()`), called just before the source decides (top of `decidePart()` for a pitched part, before each
  lane's pick in `decideKit()`), after its targets have picked. `interplayRulesValid()` rejects a Respond rule whose
  target lists one or reads one at any depth. A source's notes sounding into its against half aren't the rule's
  business (§5.5), so `avoided()` stays Avoid's.
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
  pack Risers that fit its shortest Build at its fastest tempo, which `riser_fit.py` in `check_all.py` holds over
  `pack/` (#131), from `generator_render --list-risers` (each genre's shortest Build in ms and its lane's folders and
  tags). A new genre or a new pack Riser needs no edit there.
- **Siren (built in #131):** `LaneSpec::heldBars` (its note's bars, `sirensValid()`), `fxNoteTicks()`'s case (a
  Break's first step, a Drop), and the release Throw: `sirenReleases()` (`core/generator_controls.cpp`) watches the
  FX parts' sounding notes, and `updateThrow()` spikes the Siren row on the step its note has ended by, before every
  other rule and with no switch. A second held FX lane would need its own `sirenSounding_`-style flag.
- **Fills:** a `FillKind` and a case in `fillRules()`'s switch, which writes the step's mask for it (`Step::rest`,
  `swap` or `roll`); `decideKit()` reads the mask. The Roll (#318) sets `Step::roll` with its stage and velocity
  (`heldRollAt()` and `autoRollAt()`, a `RollStep` like the Build roll's, both climbing by `climbVelocity()`), and
  `decideKit()` plays it through `playRoll()` like the Build roll, gated by `gateOpen()`.
  Drop-outs beat a fill on a part or lane they share: `fillRules()` clears them from `Step::rest`/`swap`/`roll`.
- **Drop-out (spec §8.8, built in #130):** `Generator::dropOutRules()` (`core/generator_sections.cpp`), after
  `sectionRules()` and before `fillRules()`, sets `Step::dropOut`; `silenced()` reads it, so `keptOut()`,
  `decideKit()`, `lateEntries()` and the dim see it with no line of their own, and `fillRules()` clears it from
  `Step::rest`, `Step::swap` and `Step::roll`, so any fill kind added there gives way to it too. State is `dropOut_`
  (the entry last drawn), `dropOutFrom_`/`dropOutTo_` and `dropOutAfter_` (the gap). The Arc plan never reads it. The
  Throw: `updateControls()` fills `ThrowLine::dropOut*` and `updateThrow()` spikes the target in the window, holds
  `ControlSet::thrown` at `throwHoldSteps` while it plays and falls over its last beat (or a beat from a Section line
  that ended it). The delay Throw reads the same `thrown` counter (`throwLevel()`), so it holds and falls with it. A
  Drums target throws the set the part's Throw acts on (the lane's row if aimed at it, else kit-wide).
- **Delay setups and the delay Throw (spec §10.6, §11.4, built in #320):** a genre gives a part, a lane's row or a
  Kit (the Drums or FX `PartSpec`'s own) a `DelaySetup`; `PartSpec::throwDelayDepth` (12) is the lift in menu units.
  The core sets `ControlSet::delaySetup` in `begin()` and sends the Throw's level on Space (`Event::throwDelay()`,
  the Control event's `velocity` byte; `ControlSet::sentThrowDelay` sends it when only the level moves). The driver binds
  the set's delay feedback among Space's params (`SoundControls::bindThrowDelay()`, 0..0 range where Space doesn't
  move it) and plays `perform::throwDelayValue()`. Loads write the four fields through `applySoundSettings()` (synth
  and row, `laneSoundSettings(part, lane)`) and `applyDelaySetup()` (the Kit, in `buildKit()`). Dub (#131) sets
  `.delay` on its Skank, Melodica, Rim row and Siren row, and aims its Drums Throw at the Rim; its session script
  `perform_dub.txt` covers them. `perform_delay_throws.txt` and its `hold delay_setups` test roster stay: they throw a
  kit-wide set and a Bass, which Dub doesn't.
- **Roll fill (built in #318):** `fillRules()`, `heldRollAt()`, `autoRollAt()`, `decideKit()` and `playRoll()`
  (given the `RollStep` to play). It obeys gates, mute and keeps as a Swap does (decided in #127's grilling), so `mayPlay()` doesn't change:
  the dim and the Arc plan never count it. A held Roll counts its steps in `heldSteps_` (reset by the SetFill press).
  A drop-out on its lane clears it in `fillRules()`, and `silenced()` would stop it in `decideKit()` anyway.
- **Capture's builds (M4):** the runner has one next slot ([runner.md](runner.md)), and the leave job can wait in it
  (`Runner::cancelAllThen()`). A Capture build queued behind `LeaveJob` would replace it there: chain it from the leave
  job's `finished()`, or give the runner a larger next slot.
- **Capture's clips (M4):** `ClipBuilder::take()` drops Section, KeyScale and Sound events today (spec §15.2, §15.5).
  Capture cuts clips on Sound and KeyScale and splits songs on Section (§17.5, §17.6): those cases are where it starts
  a new clip or song. Its replay feeds the steering log to `ComposeDriver` instead of the LENGTH rule's End
  (`endsHere()`), and its muted notes are already dropped there.
- The Generative menu's lists (#140): a new source row is a `MenuItem` in `SourceMenu`'s list
  (`gui/menu_item/generative/source_rows.h`), in PERFORM FROM (`perform_from.cpp`), COMPOSE FROM and the screen's
  FROM (`compose_screen.cpp`). Perform entry from a new kind of source is an `EntrySource`
  (`generative/firmware/entry_source.h`) and a branch at the start of `EntryJob::run()`. SAVE and LOAD on the screen
  are the sound editor's SAVE and LOAD branches (F19, `composeScreenShowing()`).
- What a file chosen in `LoadRecipeUI` does is `loadChosen()` (`gui/ui/load/load_recipe_ui.cpp`): PERFORM FROM under
  it (`performFromShowing()`) asks DISCARD SONG?; Perform as root UI makes it a transition; otherwise the Compose
  load (`loadComposeRecipe()`), which shows the screen. A new place to open the browser from adds a branch there.
