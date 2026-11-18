# Implementation notes: Portable code (`generative/core/`, `perform/`, `runner/`): build, steering, capacity, genre data

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- The harness builds these three trees and nothing else (`tests/spec/generator/CMakeLists.txt`); `generative/firmware/`
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
  marked `GEN_STEP_CODE` even when it is inlined today: LTO's choice can change with any edit.
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
- `Purpose` (`core/draw.h`) and `CommandKind` (`core/steering.h`) are append-only: a purpose's value is part of every
  draw key, and logs store command kinds.
- Every land-time effect of a command goes in `Generator::apply()`. Replay feeds logged commands to `apply()`
  (ADR 0005), so an effect done anywhere else is lost on replay.
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
  `sizeof(Generator)` changes, update §20's "The M1 core is N B" with the ARM size:
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
- Shared helpers for draws and card names, so a new genre's palette or roll code doesn't grow a fifth copy:
  `EntropyStream` (fresh-entropy rolls: a Recipe's facts, sound picks), `scaleBelow()` and `rollWeighted()` (keyed and
  fresh weighted rolls) in `core/draw.h`; `equalsIgnoringCase()`, `lowerChar()` and `isExactPathTag()` in
  `core/palette.h`; `pitchClass()` in `core/types.h` (specs had their own copies: a helper moved into a header can
  clash with a spec's file-local one of the same name). The Arc plan (`Generator::planArc()`) shares the play path's
  rules: `effectiveOn()`, `Gates::crossBar()`, `MuteSolo` (`audible()`, `land()`), `mayPlay()` with its lane test (the
  dim's is `!silenced()`, the plan's `Keeps::lets()`; it adds `rollsIn()` to `maySound()`), `rollStageOn()`, and
  `outroEndsAfter()` (with `ended()`). A change to how a gate, a mute, the roll or the Outro's end works goes there, not
  into a copy. The plan leaves out the cut and the Gap, which the dim counts (spec §13.5): `cutAndGapUnderBuild()` in
  `genreValid()` keeps each shorter than a phrase and than the shortest Build (Dub's is 1 bar).
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
- A Chord graph holds at most `kMaxChordMoves` (8) moves (`harmonyValid()`): "each move both ways" over five moves is
  ten, so let the home pull carry the moves back to i, as Detroit's and House's graphs do.
- A Respond source whose patterns plan nothing in the half its target makes against shows no effect at all: a spec
  checking "no note in the against half" then passes with the rule's strength at 0. House's Lead has lead-ins in each
  bar's first half for Respond to thin; check a spec like that bites by running it once with the strength at 0.
- Registers of the role defaults overlap (Chords and Pad share anchor C4; the Lead's span reaches down into the
  Chords'). A genre whose parts must stay apart in every key sets each Register with anchors a whole octave apart, so
  the tonics are octaves apart in every key, and spans that don't meet (House's, §4.5).
- A spec that mutates a genre table rebuilds `generator_render` too: rebuild (`./dbt test --generator`) after
  restoring the table, or `theory_review.py` and later probes read the mutant.
