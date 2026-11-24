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
- **Replay (M4, #145):** `ComposeDriver::beginReplay()` doesn't stop on `Generator::ended()`, which reads the pending list; it runs the piece's span.
  Glides log their length in `Command::length`. Jumps aren't logged. At a transition's start step the incoming
  instance's `carryOver()` (the live macros and the hold, `PerformDriver::switchToIncoming()`) isn't a logged
  command: the start entry (§17) has to carry both, or replay starts the new piece on the pending Recipe's macros.
  A transition's confirm in the 12 ticks between a sound's SetSound landing and its Sound event drops the swap in
  firmware (`SoundSwapLifecycle::cancelAll()`, then `swap()` finds no token); the core has logged the SetSound, and
  `swapDropped()` logs a SoundDropped record after it (#143): Capture keeps the part's old track. Each landed token
  has its picks as SoundPath records on its step (tokens wrap at 255).
  The header's opening (`PieceStart`) holds the incoming instance's carried-over macros and hold, read at its step 0.
  Jumps (SPP, a clock scale change) still aren't logged: a replay doesn't skip to the next bar line.
- **The steering log's lifetime (#146):** `PerformAdapter::freeBlocks()` frees the log's buffer with the Generator
  blocks at the teardown. Capture's builds run after it, so #146 keeps the buffer (and `log_`) past the teardown
  until the last build. Marks (#144): `SteeringLog::mark()` marks the newest piece, `full()` refuses a new mark.
- **Capture (M4):** the early return under Perform in `PlaybackHandler::recordButtonPressed()` is the Capture mark.
  `perform_song.cpp`'s `link()` gives every linked Output a name unique in its song (`ensureUniqueInstrumentName()`,
  `DRUMS`, `DRUMS 2`, …), so two Kits or two Synths from the same preset never collide in `InstrumentClip::claimOutput()`
  on reload.
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
- **Capture's clips (#146):** the portable replay (#145) calls the SongWriter's Capture hooks (`cutClip`, `soundPick`,
  `newTrack`, `tempo`, `paramEdit`, `stutter`, `nextSong`); the firmware's `ComposeSong` implements none yet. A split
  song's opening state (picks, snapshot, homes) is the writer's to rebuild from begin(), newTrack() and paramEdit().
- The Generative menu's lists (#140): a new source row is a `MenuItem` in `SourceMenu`'s list
  (`gui/menu_item/generative/source_rows.h`), in PERFORM FROM (`perform_from.cpp`), COMPOSE FROM and the screen's
  FROM (`compose_screen.cpp`). Perform entry from a new kind of source is an `EntrySource`
  (`generative/firmware/entry_source.h`) and a branch at the start of `EntryJob::run()`. SAVE and LOAD on the screen
  are the sound editor's SAVE and LOAD branches (F19, `composeScreenShowing()`).
- What a file chosen in `LoadRecipeUI` does is `loadChosen()` (`gui/ui/load/load_recipe_ui.cpp`): PERFORM FROM under
  it (`performFromShowing()`) asks DISCARD SONG?; Perform as root UI makes it a transition; otherwise the Compose
  load (`loadComposeRecipe()`), which shows the screen. A new place to open the browser from adds a branch there.
- **Restart AS IT BEGAN (#413 part 2).** A restart is the core's `CommandKind::Restart` (lands on the next bar line,
  stops the instance as a hard cut does), then `PerformDriver::restartOnLine()`, which ends the first run and calls
  `NoteSink::restartPiece(generator)`. The firmware's is `PerformAdapter::restartPiece()`, which starts the Generator
  through `PerformSteering::restartAsItIs()` (the Current Recipe). AS IT BEGAN is a second mode chosen at the press
  (beside `pressRestart()`): its preload of the swapped sounds runs first, as a sound re-roll's does, and pushes the
  Restart command once loaded; at the line `restartPiece()` puts the piece header's Recipe, takes and param snapshot
  back before `Generator::start()`. #143's `openPiece()` (set by `begin()` and `switchToIncoming()`) also needs
  setting in `restartOnLine()`, so the new piece gets its header.
- **Variant sets (#493, ADR 0008).** Data: `Variant`/`VariantSet` in `core/genre.h` (slot 11, `Genre::variantSets`),
  the rules in `variantSetsValid()` (`core/variants.h`, in `genreValid()`). A genre's new set lists today's behaviour
  first (apply nullptr) and bumps the genre's version; its spec pins the set's names. The draw is `drawVariants()`,
  called last in `newRecipe()`, after #498's swing roll. The menus (#494) edit `Recipe::variants.allowed`
  and pass it as `newRecipe()`'s `narrowed` (the Compose screen's REGENERATE already does,
  `CurrentRecipe::regenerate()`; Perform's `rollPending()` passes none: its page opens on RANDOM). Drop changes and
  live switches (#495): a set's `changeAt = Drop` + `changeChance`, or `live`, is all a retune adds; the draw is
  `dropChange()`, the landing `Generator::dropVariants()`, the patch-diff rule `variant_sets_spec`'s
  `patchesKeepLookAhead()` (look-ahead fields and pool sizes). The tools' `--variant SET=NAME` is `fixVariant()`.
- **Yield and the jump mask (#497).** `PartSpec::yield` (`YieldSpec`: the part yielded to, a mode in Drops and one
  elsewhere) is worked out in `Generator::updateSilent()`, in a second pass after every part's own silence: Rest
  goes into `yieldRest_` (read by `silenced()`, so the part is silenced and its MUTE dims) and `silent_`, Eighths into
  `yieldEighths_` (read by `plays()`). The Arc plan asks the same `yieldOn()` (`ArcPlanner::advance()`). A new mode
  adds to `YieldMode` and those two readers. `LoopSpec::jumpSteps` is applied in `rollLineStep()` after the draws, so
  a mask never moves a keyed draw; Rotate Answers still move the line a beat.
