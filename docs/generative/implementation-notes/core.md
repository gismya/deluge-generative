# Implementation notes: Portable code (`generative/core/`, `perform/`, `compose/`, `runner/`): build, steering, capacity, genre data

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- The harness builds these four trees and nothing else (`tests/spec/generator/CMakeLists.txt`); `generative/firmware/`
  is the firmware side. The `generative-rules` hooks (`scripts/lint/generative_rules.py`) keep float, firmware
  includes and libc stdio out.
- The Generator (`core/generator.h`) is one class whose code is split by area: `generator.cpp` (start, the step's
  loop `decideNextStep()` and `decidePart()`, the chord walk, note bookkeeping and flush), `generator_steering.cpp`
  (`apply()`, waiting and landing commands, `planArc()`), `generator_controls.cpp` (sound controls, §11.3 to §11.5),
  `generator_patterns.cpp` (picks, planned onsets and interplay rules, gates, the Kit, expression),
  `generator_sections.cpp` (Section rules, the Build roll, fills, silences) and `generator_flavours.cpp` (Loop lines
  and Answers, Sustain, Stab, Arp, Motif). What more than one area uses lives with its data: the draw keys
  (`partKey()`, `onPhrase()`, `withField()`) in `core/draw.h`, `onPick()` beside `PatternState`, the Arc's rules
  (`effectiveOn()`, `rollStageOn()`, `rollHits()`) in `core/arc.h` and the keeps in `ArcSpec::keepsIn()`; a helper
  only one area uses stays file-local in that area's file. A step callee defined in another file than its caller is
  marked `GEN_STEP_CODE` even when it is inlined today: LTO's choice can change with any edit. Trap: GCC LTO drops
  `GEN_STEP_CODE` (`[[gnu::section(".text.generative_step")]]`) on a header `constexpr`/`inline` (COMDAT) function when
  it emits an out-of-line copy — define any step callee LTO may outline out-of-line in a `.cpp` file (`draw()` lives in
  `core/draw.cpp` beside `partKey()`, `onPhrase()` and `withField()`).
- `snprintf` and friends in portable code break the firmware link: newlib's stdio pulls in `_close`, `_sbrk` and the
  rest. Build text with `Text` (`perform/text.h`). Firmware code may use `snprintf`: it gets the
  non-allocating `lib/printf.h` through `io/debug/log.h`.
- The steering presses live in the Perform driver's steering (`perform/perform_steering.h`), CRTP like
  `TransitionLifecycle`: its adapter (`firmware::Steering`, or the harness's `HarnessSteering`) hands it the driver,
  the sink and the Current Recipe as plain data, and does what a press changes outside them (the song's key and swing
  interval, MIDI feedback, the tick's wake, test events), with no virtual call. The view, menus and MIDI press
  `performAdapter.steering()`. It owns the steering queue as its first member, and `PerformAdapter::steering_` sits
  at offset 20, which is baked into `Generator::decideNextStep()`'s code: it addresses the queue from a constant
  (`adds #24`), and an offset past the short form needs a longer instruction (`addw`), which grows the step by 4
  bytes. The `static_assert`s in `PerformAdapter::swapIn()` and `PerformSteering::enter()` hold it.
  `PerformDriver`'s size also moves `PerformAdapter::sink_`, and with it the roster copy's mod-8 `static_assert` in
  `incomingStarts()`: #143's `log_` pointer flipped it, and moving `lastSwungTick_` ahead of `sink_` put it back.
- `Purpose` (`core/draw.h`) and `CommandKind` (`core/steering.h`) are append-only: a purpose's value is part of every
  draw key, and logs store command kinds.
- The steering log (`core/steering_log.*`, spec §17.2) holds pieces in one buffer: a `PieceInfo`, header records
  (picks, snapshot blocks), then packed records (`encodeCommand()`). The core only calls `append()`; the Perform driver
  opens a piece at its step 0's decision (`openPiece()`, outside H1's timer), ends it in `finish()` and at a
  transition's start step (`switchToIncoming()`), logs re-roll paths by comparing `soundLanded()` around each decision
  (`logLandedSounds()`: a new token always differs from the last), and `logSoundDropped()`. What only the firmware
  knows comes through `NoteSink::logPieceHeader()` and `logSoundPaths()` (`firmware/log_header.*`). A new steering
  command is logged with no log change: append its `CommandKind`, move `kLastCommandKind`, and have `apply()` return
  true where it lands (`steering_log_spec` round-trips every kind). Recipe state it changes reaches the header through
  the `Recipe` copy in `PieceStart`. An unmarked piece is thrown away only when the next begins. A restart's line ends the piece and sets `pieceDue_`,
  so the restarted run opens its own piece at its step 0 (spec §17.3); the sink's `restartPiece()` runs between the two,
  where the ended piece's header is still the newest (AS IT BEGAN reads it there).
- Every land-time effect of a command goes in `Generator::apply()`. Replay feeds logged commands to `apply()`
  (ADR 0005), so an effect done anywhere else is lost on replay. A decision that reads the pending list breaks replay
  unless the log says when that command waits: only Arc moves do today (`waitingMove()`, `lastGroovePhrase()`), and
  `logWaitingMoves()` logs their slots (RecordKind::Waiting/NotWaiting). A new reader of another slot needs the same.
- Bar- and phrase-quantised commands wait in `PendingList` (`core/steering.h`); a `static_assert` in
  `core/generator.h` keeps its capacity above every slot kind. `Generator::stepsToLine()` is the one line rule: both
  `landPending()` and `landsAt()` (QUEUE's countdowns) use it.
- Capacity is sized by construction from one roster table, `kLargestRoster` (`core/types.h`, Detroit techno's), and
  `onsetNotes(role)`: `stepEventsOf()`, `kEventBufferSize` and `kMaxControlEventsPerStep` in `core/capacity.h`,
  `kMaxControlSets` in `core/genre.h`, `SoundingNotes::kTotal`, and `static_assert(sizeof(Generator) <= 16 * 1024)`.
  `rosterFits()` (`core/capacity.h`) holds a genre's roster to it: each built-in genre `static_assert`s it through
  `genreValid()`, and `genre_data_spec` checks every genre. A genre with a bigger roster (two Pads, say) adds to the table,
  and a flavour that starts more notes per onset than its role's `onsetNotes()`, or overlaps more than the next onset
  (an Arp with notes longer than its steps), evicts notes: `roster_room_spec` plays the table's worst case. When
  `sizeof(Generator)` changes, update §20's "The core is N B" with the ARM size:
  `scripts/generative/generator_size.py` measures it (the firmware's `arm-none-eabi-g++` on a probe naming
  `Size<sizeof(Generator)>`), and fails in `check_all.py` until §20 states it.
- A Bass or Lead part's table is two notes, so it keeps one bound (spec §2.7, §5.2): every note ends by the time the
  onset after its next one starts, counting Lean, the clamps and a flam's grace hit. `detail::latestStart()` and
  `detail::earliestStart()` (`core/genre.h`) are where a note's start can move: later by the first-step clamp (a
  Section's first step, or the line a new sound or key change lands on), a ratchet before it, or crossing Lean; earlier
  by a flam's grace hit. A new way to move a note's start belongs there. Each flavour checks its data against the
  bound over its own onset gaps: `loopNotesValid()` a Loop's 16ths (gate, Lean, slide overlap), `arpNotesValid()` and
  `motifNotesValid()` each pattern's own onsets, through `detail::onsetNotesKeepBound()`; each built-in genre
  `static_assert`s them through `genreValid()`, and `genre_data_spec`,
  `arp_spec` and `motif_spec` cover their edges. The Loop's (k, k+2) pairs don't cover uneven onsets: a Lean of
  {12, -12, 12, -12} with a 23-tick overlap passes the Loop rule but sounds three notes when a note lasts to its next
  onset plus the overlap. If a later genre needs longer Lead notes, raise the sizing table (`onsetNotes()` and
  `maxSounding()`), not the rule, and re-measure.
- Stab, Arp and Motif notes share one length rule, `onsetTicks()` (`core/genre.h`): the gate, cut at the pattern's
  next onset before thinning and at the bar line, less a late Lean there, so no note lasts past a bar line.
  `Generator::onsetLength()` and `detail::onsetNotesKeepBound()` both call it. It keeps the bound above by construction
  (a note ends by its next grid onset, and Lean moves a start 12 ticks at most), so the data checks can't fail with
  today's rule: they walk the same rule, so a change to it (an overlap, a cap past the next onset) fails there first.
- Trap: the core decides a step 12 ticks ahead and sends note-offs once a step (`queueNoteOffsBefore()`), so a note
  that has ended can still sit in its part's table when the onset after next is decided (a slid note with a 6-tick
  overlap leaned +7 or more). `noteOn()` frees the part's notes ended by the new note's start before it evicts: count a table by what
  sounds at once, not by what the last note-off pass left.
- `Event` is 16 B, pinned by a `static_assert`: the event buffer holds hundreds per block. Kind-specific fields share
  the layout (the NoteOn flags and Control's `snap` are bit-fields, KeyScale's root lives in `pitch`, read it with
  `keyRoot()`). A new field has to fit, or the block outgrows H18. The buffer's keep priority lives in `Event::keep`
  while an event is buffered (0 once it plays), so `operator==` names every field but `keep`: add a new field there.
- Sound controls (spec §11.3–§11.5) are worked out in `Generator::updateControls()`, after the step's notes are
  decided, so Throw sees the step's hits. Each `ControlSet` has a `ControlSetKind` (`core/genre.h`), and
  `setMoves(kind, control)` is the one rule for which layers reach which control: `updateControls` skips the rest,
  and `kMaxControlEventsPerStep` is counted from it. A new layer or gesture that moves another control changes
  `setMoves`, and with it the buffer. `controlSetsOf`/`controlSetKind`/`controlSetLane` lay sets out; the firmware's
  `SoundControls::makeSet` uses the same helpers, so the two always agree. Drift's walk moves on at the end of
  `updateControls()`, after the step is decided, so `fallThrows()` (a stopped instance's Throws) steps it back by
  `driftLastMotion_`, the Motion it last moved by, and plays drift at that Motion's depth and the macro layer that
  step had (`lastSpaceMacro_`): Space falls to where the last decided step left it. Don't read the live macros there:
  a macro turn can land on the stop step itself, with the cut, after that step's advance.
- Genre data that would hang the device is checked at compile time: a bad interplay rule makes `planned()` recurse
  forever. Each genre table has `static_assert(genreValid(...))` (`core/capacity.h`: a palette, since `Genre::palette`
  is dereferenced unchecked, the interplay rules, `avoidRulesValid()`, `rollFillsValid()`, `rosterFits()` (at most `kMaxParts` parts: `partCount()` stops
  there, so a 7th part would never play), `sustainValid()`, `loopNotesValid()`, the flavour checks, `harmonyValid()`,
  `sweepsValid()`, `buildBarsValid()`, `rollUncut()`, `cutAndGapUnderBuild()` and `dropOutsValid()`), as each Techno
  subgenre does. A new rule goes into `genreValid()` and gets its own `genre_data_spec` case, which names the rule
  that failed where the `static_assert` can't. The period rules
  (harmonic rhythm, retrigger, Sweep, Build length) share `fitsPhrase()` (`core/genre.h`). Other data checks are in
  `tests/spec/generator/genre_data_spec.cpp`, which only runs in the harness. `subgenreOf()` is `consteval` and
  rejects a subgenre as a parent at compile time.
- `newRecipe()` (`core/recipe.cpp`) rolls its facts from one `EntropyStream` in a fixed order: tempo, root, scale,
  swing (#498), then the variant sets (#493). A new fact appends its draw after these, and draws it before the key's
  early `return` (no lean), or a lean shifts it. `new_recipe_spec` pins the order. Swing is `Facts::swingLow/High`;
  a Recipe file without swing takes `Facts::swingMiddle()`.
- Shared helpers for draws and card names, so a new genre's palette or roll code doesn't grow a fifth copy:
  `EntropyStream` (fresh-entropy rolls: a Recipe's facts, sound picks), `scaleBelow()` and `rollWeighted()` (keyed and
  fresh weighted rolls) in `core/draw.h`; `equalsIgnoringCase()`, `lowerChar()` and `isExactPathTag()` in
  `core/palette.h`; `pitchClass()` in `core/types.h` (specs had their own copies: a helper moved into a header can
  clash with a spec's file-local one of the same name). The Arc plan (`Generator::planArc()`) shares the play path's
  rules: `effectiveOn()`, `Gates::crossBar()`, `DropOutState::step()`, `MuteSolo` (`audible()`, `land()`), `mayPlay()`
  with its lane test (the dim's is `!silenced()`, the plan's `Keeps::lets() && !dropOut.has()`; it adds `rollsIn()` to
  `maySound()`), `rollStageOn()`, and `outroEndsAfter()` (with `ended()`). A change to how a gate, a drop-out, a mute,
  the roll or the Outro's end works goes there, not into a copy. The plan leaves out the cut and the Gap, which the dim
  counts (spec §13.5): `cutAndGapUnderBuild()` in `genreValid()` keeps each shorter than a phrase and than the shortest
  Build (Dub's is 1 bar).
- Trap: a Sustain holds its voicing over bar and phrase lines with no new note-on, so a history that counts note-ons
  shows a phrase a held Drone or Pad plays through as unheard. The Perform driver's `PhraseHistory` hears a part in
  every phrase a note of it rings into: it keeps the keys of the unmuted notes it was sent (`Event::noteKey()`, the
  key `Event::sameKey()` matches on) and ends one only on its own note-off, since a muted note's note-on is never
  sent but its note-off is. So a note-off that never comes (the event buffer drops one only if it is full of events
  other than note-ons, which its size rules out) leaves its part sounded in every later phrase until the next
  `begin()`. A spec reading heard phrases needs no ringing-note fix-up of its own.
- On ARM `int32_t` is `long`, so `std::clamp(x, 0, kMacroFull)` with an `int32_t` x doesn't compile there while the
  harness (Apple clang, `int32_t` is `int`) builds it fine. Write `std::clamp<int32_t>(...)` in portable code.
- Tier picks share one rule (`core/tiers.h`): `bandTier`, `playingTier`, `drawInTier`. Pattern picks and Answers both
  use it; Motif and Arp tiers should too.
- Freeze (§6.7) holds a part's picks in `updatePick`: a Drop or re-pick while frozen waits in the lane's
  `PatternState::waiting`, and `Generator::thaw()` adopts it by setting `entry = -1`, so the next decision re-picks
  it (and a Loop rolls its line). A freeze replays the phrase of the last decided step: on a phrase line, the phrase
  just heard, since that line's pick waits. A draw keyed on an absolute step must go through `heldStep()` so a frozen
  part replays its phrase (Answers and Fresh Velocity spread do).
- A Loop's Answer is drawn at each bar line (`updateAnswer`) and stored in `PartState::answer`, so a Complexity turn
  mid-bar waits for the next Answer. Read-time transforms live in `linePitch()`; `resolvePitch()` takes scale steps
  from the chord root.
- Subgenres (§4.1, ADR 0006) are written `constexpr Genre kX = [] { Genre genre = subgenreOf(kParent); ...; return
  genre; }();` in the parent's file: `subgenreOf()` takes the parent's address, so the parent must be a constexpr table
  with static storage in the same translation unit. It resets the identity (names, version 1), so a subgenre that
  forgets its name fails `subgenre_spec`. A parent is in no list: `allGenres()` and `findGenre()` hold only playable
  genres, and a parent's `version` is 0. `gen_emu.py`'s `genre_names()` reads the genre files with a regex that knows
  both forms (a whole table, unless its version is 0, and `subgenreOf` then `genre.name`): keep new genres in one of
  them.
- The Psytrance parent (`kPsytranceParent`, #319) is the other way round from Techno's: it holds the whole genre, and
  Classic (`kPsytranceClassic`) is `subgenreOf` it with only its identity set, so its event stream is the old
  `psytrance`'s. A subgenre's OLED name is the confirm title and the palette override's file name, capped at 14
  characters (`genre_data_spec`): `CLASSIC PSYTRANCE` doesn't fit, so it is `CLASSIC PSY`. Picking it in a probe
  script takes two picks (`row=PSYTRANCE`, SELECT, `row=CLASSIC`), as a Techno subgenre does.
- The Techno parent (`kTechnoParent`) holds only the Drums and FX parts (`kTechnoDrums`, `kTechnoFx`), the
  fills, the Arc, Break keeps of Kick, Rim and Closed hat (a subgenre that names none breaks down to those, not
  a full Groove) and a palette of drum and FX lanes (`technoPalette`). Each subgenre lists its whole roster, Drums
  first and FX last, and its bass second: the parent's Rest fill drops part 1, and each subgenre `static_assert`s
  `technoRosterValid()` (`core/genre.h`). Its palette shares the parent's lanes (`kLanes` in `techno_palette.cpp`;
  Dub techno copies them with Perc 2 changed, so `kLanes` stays the one list).
- Adding a part before another re-keys the later part's drift: drift phases and points are drawn with the part index
  (`partKey(seed_, set.part, ...)`), so FX's Control events change when a part lands before it, though its notes don't.
  A fingerprint that compares a roster with and without the new part (`acid_fingerprint_spec`) leaves that part out,
  moves the later parts' indices back, and leaves FX's Control events out of both streams.
- A note still sounding when the harness driver stops (a Drone held over the last bar) ends at the stop, which lands
  where the last driver call did: specs that compare streams across driver cadences compare before the last step
  (`perform_driver_spec`).
- A Break plays only what its keeps hold, at the Break's Density dip (the default −20 takes the default Density 25 to
  5, where only anchors and some 9s play). A subgenre that keeps no anchored lane in its Break goes silent there, and
  one whose only anchor is a Sustain's root (Melodic techno keeps Bass, Pad, Lead and Closed hat) plays a bare root:
  give it a shallower Density offset for the Break (`arc.offsets[Break]`), as Melodic techno does, and check its
  Break plays (`melodic_techno_spec`).
- A subgenre that takes the parent's Drums part takes its Kick pool too, pickups off the beats included (Techno's tiers
  1–2 play steps 3, 7, 11, 14 and 15, some at the default Density). A part that must stay off the kick (Melodic
  techno's rolling bass) needs the subgenre's own Drums part, with a kick on the beats only (`kMelodicDrums`), and a
  spec that checks against the kick's note-ons, not against the beats.
- A genre of its own (no parent) is written whole, `constexpr Genre kX{...}` in its own file (`genres/ambient.cpp`),
  its `extern` in `genre.h` and a row in `kGenres` (`genre.cpp`); `gen_emu.py`'s `genre_names()` finds it with no edit.
  Its spec file's `describe` takes the file stem (`ambient_spec.cpp` → `describe ambient`), which clashes with a
  file-local `const Genre& ambient`: name the reference something else.
- `genre_data_spec`'s off-beat authoring rules hold every tier 1+ pattern, a lane gated far above the default Density
  too: its strongest off-beat hit must be 7 or more, not a ghost, and in the top half of its importances (Ambient's
  soft percussion, gated at 35). A tier 1+ pattern with only on-beat hits fails (a clap on 2 and 4 with a ghost
  pickup): give such a lane one tier, as Psytrance's Clap has. `patterns_spec`'s Density-0 check skips a gated lane as
  it skips a gated part.
- A Loop's gate is one value for its part, so notes held to the pattern's end (§5.2's long Loop notes) need every
  pattern of the pool the same length: Ambient's Bass pool is one 2-bar pattern with a 2-bar gate.
- A gate on a part a Break keeps must sit at or under the Break's Density at the default macros, or the Break keeps it
  silent: Ambient's bells are gated at 10, its Break dip being 25 − 15.
- A Sustain part counts toward the Intro floor (`lowestGate()`), though it has no pool: without it Ambient's floor
  was its bells' gate and the bells played from the Intro's first step.
- A genre whose open hat plays every & loses every closed hat there: the default Replace pair (Open hat replaces
  Closed hat, spec §5.7) drops the closed hat's hit on each shared step. Psytrance keeps its open hat sparse.
- The Open hat pairs with the Closed hat by tag name (§10.3): a set whose hat tags differ (the Psy set's `CLHAT-` and
  `OPHAT-`) never pairs that way, so with a second machine in both lists the Open hat rolls its own weights against
  the Closed hat's set. Psytrance's hats take the Psy set alone.
- A part gated over the default Density still plays late in a Build: the Build's Density ramp (+12 for Psytrance)
  passes the gate. Leave it out of `buildKeeps` if it should come in on the Drop (Psytrance's Lead).
- A gated part leaves a Drop phrase early when the Drop's lift decays under its gate (Psytrance's Lead left after bar
  10 at a 1-phrase decay). A longer `dropDecayPhrases` keeps it in, but `genre_data_spec` checks the decay fits the
  shortest Groove at Motion 50; Psytrance is its named exception (spec §9.3), and `psytrance_spec` shows the cut-off.
- A Chord graph holds at most `kMaxChordMoves` (16, 8 before Ambient's round 2 took 12) moves (`harmonyValid()`).
  It bounds only data: the walk loops over the moves once a chord change, and nothing is sized by it.
- A Respond source whose patterns plan nothing in the half its target makes against shows no effect at all: a spec
  checking "no note in the against half" then passes with the rule's strength at 0. House's Lead has lead-ins in each
  bar's first half for Respond to thin; check a spec like that bites by running it once with the strength at 0.
- Respond's (and every interplay rule's) draw is keyed on the pick and the pattern step, so it holds while the pick
  does, and a Lead pick can hold for a whole Groove. A line with one lead-in a bar keeps all of them or none in a
  piece: give the thinned steps several steps of their own (House's tier-1 answers have two or three a bar). Measure
  the kept share per seed against the rule at strength 0 *and sensitivity 0*: with sensitivity left on, a "strength
  0" copy still thins at a Complexity over the default.
- A lane tag matches any part of a file name (`tagMatches()`), and a lane listing two folders matches in both: House's
  Impact tag "909", over the Crash and Kick folders, rolled a 909 kick 6 times in 7, and "909 Kick" took the
  distorted and dry kicks too. Name a single file by its path (`SAMPLES/DRUMS/Crash/909 Crash.wav`), and check a new
  palette's tags against `tests/spec/generator/fixtures/factory_card.txt`.
- Drum kits (spec §10.2): `DrumKits` on the `Palette`, machines in `core/genres/drum_machines.h`, rolled in
  `rollPart()` (`palette.cpp`) from the part's own kit stream (`kKitSlot`), so the lanes' draws don't move. A kit tag
  is rolled as a one-tag copy of the lane (`rollKitTag()`), so it matches the lane's folders whatever its list says:
  a machine whose tag also names a closed hat (the Psy set's `CLHAT-`) must never reach the Open hat, which is why
  the hat pair carries only a Closed hat swap. `drum_kits_spec` checks one machine per piece in every genre; a lane
  rule over rolled picks (3 files, the pack's clap) counts the lane's list with `lanePicks(..., {}, false)`. A swap
  by exact path in a folder no lane reads (Dub's pack snare in `SAMPLES/DRUMS/Snare/Dub`) reaches no other lane, and
  where the file is missing the lane rolls its machine's tag, not the lane list. A preset's own level is
  `PaletteEntry::volume` (on top of `PartSpec::volume`, `partSoundSettings(part, load.volume)`).
- An FX lane whose roll finds no sample rolls one of its `synths` by weight from the lane's own draws
  (`rollLaneOrPreset()`, `palette.cpp`), and that preset path is its pick: the Siren's pick is `GEN Siren` and the
  Impact's one of its presets (#472). A lane listing both folders and presets still rolls a file whenever one
  matches, so its presets stay fallbacks. Give an Impact-style lane no folders and no tags, or a factory file that
  matches would win. Its default folder `SAMPLES/FX/<lane>` is empty in the pack, and lists of a lane's picks
  (`lanePicks`) now hold the preset paths.
- A lane's pick (its path) is not in the event stream: the goldens, `kPinned` and `generator_render`'s dump and MIDI
  don't see a palette change that only swaps what a lane plays (#472 round 3: House's Impact from files to a preset,
  Acid's own boom). It needs no version bump. A subgenre that wants one lane's sound of its own copies Techno's
  `kLanes` and swaps that entry (`kDubLanes`, `kAcidLanes` in `techno_palette.cpp`).
- A pack preset's delay feedback stays under 25 (`test_make_pack.py`): an FX row's saved feedback is what plays (a
  kit row's Space is reverb only), and from 25 the echo never dies in Perform (spec §11.4).
- The Fall and the Impact span their Section's start (the Break, the Drop's phrase), so they never enter late
  (`entersLate()`, `core/types.h`, used where `updateGates()` and `express()` hold notes back). A new FX lane with a
  long note should join it.
- A level mark ('g' or 'a') over a '.' step is a compile-time error (the `Pattern` constructor): it did nothing, and it
  usually meant the mark had slid one step off the hit it was for.
- Registers of the role defaults overlap (Chords and Pad share anchor C4; the Lead's span reaches down into the
  Chords'). A genre whose parts must stay apart in every key sets each Register with anchors a whole octave apart, so
  the tonics are octaves apart in every key, and spans that don't meet (House's, §4.5).
- A spec that mutates a genre table rebuilds `generator_render` too: rebuild (`./dbt test --generator`) after
  restoring the table, or `theory_review.py` and later probes read the mutant.
- Part bias (§3.3, #303): `Recipe::bias` (`PartBiases`), the curve `partBiased()` / its inverse `biasFor()` in
  `core/part_bias.h` (swap the curve there only: `biasFor()` searches whatever monotonic curve it is), and
  `Generator::biasedOn(part, macro, shared)`. A part decides on its own copy of the step (`partStep()`, only when
  `recipe_.bias.any(p)`); the shared step stays what the Arc, fills, accents and drift read. `Step::sharedDensity` is
  the unbiased Density: `planned()` takes the shared one and biases it for whichever part it asks about, so every
  caller passes `step.sharedDensity`, never a part's `step.density` (an interplay target plans at its own bias). A new
  per-part macro read goes through `biasedOn()`; a new shared one reads `recipe_.macros` or the shared step. The
  macro layer is per part (`macroLayer(control, part)`, -1 for the shared one), and so is `lastSpaceMacro_`.
- Punch-ins (§8.9, #301): `PunchIns` (`core/punch_ins.h`) in `Generator::punch_`, set by `SetPunch` in `apply()`.
  Their effects act at output, after the plan: a LaneMute in `noteOn()`'s muted flag, the rolls in `decideKit()` after
  the Replace pairs (`punchRoll()`), Flam and Double kick in `express()` after `ornamentFor()` (`punchOrnament()`),
  so no draw moves and the piece after a release is the one with none. Pump is the core's one event the driver acts
  on for a punch-in (`EventKind::Pump`, appended after NoteOn so the other kinds keep their values, which the goldens'
  fingerprints hash); `stepEventsOf()` counts one a step. The harness driver drops a muted note-on, so a spec of a
  LaneMute reads the lane's notes as gone, not flagged (`punch_in_spec`).
  Flam's lanes are `flams()` (`punch_ins.h`, shared with the spec). Pump changes in `apply()` push at most one Pump a
  step: a second change in the same drain removes the first (they cancel), as `stepEventsOf()` counts one.
- Trap: a `PartsAndLanes` keep set with the Drums part's bit keeps every Drums lane, the kick too: to keep all but a
  lane, list the lanes (the pitched parts kept by bit, the lanes `0x7FFF & ~laneBit(Lane::Kick)`; bit 15 is
  `kSameAsBreakKeeps`' marker). `kEverything.without({.lanes = laneBit(Lane::Kick)})` still keeps the kick.
- Trap: Respond never thins an anchor (§5.5), so a spec of "no note in an against half" holds only while the target is
  never busier where the source anchors. House's #368 Bass line makes the second half busier, where the Lead anchors;
  its specs allow the anchor steps. A gated source (House's Lead at 30) plays little at the default Density: run
  interplay specs above the gate.
- Variant sets (§3.6, #493): `Generator::begin()` sets `genre_` to `resolveGenre()` (`core/variants.h`): the constant
  table when no picked variant patches it, else the instance's `ResolvedGenre`, which its owner holds beside it and
  hands over once with `useResolvedGenre()` (Perform's `allocateBlocks()`, `ComposeWork`, `CaptureWork`,
  `harness::Instance` in `tests/spec/generator/instance.h`). Trap: a bare `Generator` has nowhere to copy and plays
  every genre as written, whatever its picks, so a spec or owner that plays a genre with sets on a patched variant
  uses an `Instance` (harness `play()`, `StepwisePiece`, `walk()`, `TransitionPiece` and the render do). Code that
  needs the patched data reads `genre_`, never `recipe_.genre` (the constant table, kept for identity and files).
  A patch writes `ResolvedGenre::genre` (a scalar, or a span or pointer to another constexpr table) or
  `ResolvedGenre::parts[p]`; `genre.parts` points at the copy's parts.
- Trap: `PerformAdapter`'s members before `steering_` are baked into the step's code (offset 20): #493's
  `resolved_[2]` first went beside `blocks_` and moved `steering_` to 28. New adapter members go after `sink_`.
- **Variant changes at a Drop (#495).** `Generator::dropVariants()` runs in `decideLanded()` right after
  `arc_.enterStep()` on a Drop's first step (`dropStartsHere()`, not step 0: a transition's roster opening on a Drop
  keeps its picks), before `walkChord()` and the pattern picks read the genre. A live `SetVariantMask` waits in the
  pending list but `landPending()` never lands it (it runs before the Arc enters the step, so it can't know the step
  is a Drop): `dropVariants()` lands it, logging it there, which is why `decideLanded()` takes the log. In replay the
  record's `apply()` runs before `decideLanded()`; `switchedSets_` carries "this set was switched" to the Drop's draws
  either way. `stepsToLine()` for a `SetVariantMask` is only QUEUE's countdown (`Arc::ahead().drop`), `kNever` with no
  Drop ahead, which `landsAt()` passes on as `Generator::kNoLine`. Re-resolving writes the copy in place, so a
  patched pool must keep its entry count and lengths: a frozen part's `PatternState::entry` indexes the new pool.
- Trap: once a genre has variant sets, a spec's `newRecipe()` or `movingRecipe()` draws them, so a spec of the
  table as written fixes each set to its first variant (`withVariants()`; Psytrance's `asWritten()`, House's
  `build=full`). Psytrance's rest and Lead specs failed in odd ways (a Lead in the Intro, an Arp resting under the
  Lead) until pinned (#497).
- Trap: a pitched part's Register span counts from its tonic, which sits within 6 semitones of the anchor, not from
  the anchor: Psytrance's Lead (anchor A5, +12) tops out at C7 in C, where its tonic is C6 (#497).
