# Implementation notes: Runner and jobs

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- `Chunks::commit()` (`runner/runner.h`) is a job's point of no return: after it the runner refuses to cancel that
  run. A job that can never stop (a teardown) returns `false` from `cancellable()` instead.
- `generative/firmware/runner.cpp` yields with `yieldToIdle()`, not `yield()`. `yield()` runs one task, so buttons
  (and BACK) can miss a whole build.
- Each job is an `addOnceTask` holding `RESOURCE_SD | RESOURCE_SD_ROUTINE`, so it can't start during a card wait.
- The tick runs inside every card and parser yield. Tick code never starts a load or touches a half-built Kit: loads
  belong in a job.
- A live job (preloads, re-rolls) subclasses `Job` with `Kind::Live`, calls `chunks.boundary()` between Outputs and
  after each card read, and frees what it built when that returns true. Give `CardFolders` the job's `Chunks`
  (`CardFolders folders(&chunks)`): each folder read is then a boundary, and `folders.cancelled()` tells a cancel from
  a card error when a listing returns false. Entry's genre roll and sound re-rolls do this.
- The runner's next slot only ever holds a transition's preload or the leave job: the free job and a sound re-roll
  are submitted only when the runner is free. Keep it so: a submit replaces the next slot, and a free job dropped
  from it leaves its Outputs linked until Perform is left.
- Transitions: the phase and its rules are portable, in `generative/perform/transition_lifecycle.h`
  (`TransitionLifecycle<Adapter>`), and nothing else sets the phase. The firmware's `Transitions`
  (`generative/firmware/transitions.cpp`) derives from it, privately, as its adapter: the preload job, the rosters'
  Outputs, the Current Recipe, popups and the `gen_transition` events. The header lists the adapter's members; the
  harness's adapter is `TransitionAdapter` in `tests/spec/generator/transition_piece.h`. A new rule goes in the
  lifecycle with a spec in `transition_lifecycle_spec.cpp`; an adapter member only does what needs the firmware. The
  lifecycle is a template so that no adapter call is virtual, and it holds no pointer, since `transitions` sits in
  SDRAM that is only cleared. The `gen_transition` texts come from `traceText()` in the header, for both adapters.
- A genre change's preload (`TransitionJob::run()`) reads the new genre's palette override into the Current Recipe's
  spare override (`CurrentRecipe::stagePalette()`) and rolls and loads from it; `commitRecipe()` adopts it only when
  the genre changed. Two preloads never run at once (one cancels and unwinds before the next starts), so one spare is
  enough. `Transitions::phraseLength()` reads the Current Recipe's genre: the new one by the time the outgoing roster
  has drained, the old one at a stop's commit. Every Techno subgenre has an 8-bar phrase, so the choice only matters
  once genres of other phrase lengths can change into each other.
- `TransitionLifecycle::reset()` leaves the request counter alone. A preload the leave cancelled ends after
  `forget()`, and must still reach `Transitions::preloadEnded()`: it removes the working animation and commits the
  rolled Recipe for the leave.
- The lifecycle hears from the tick through the driver's sink: `transitionLanded()` (Queued to Committed, never for
  a cut), `transitionDropped()` (a BACK tap took the command: a flag; the pump, a stop or a cancel ends the transition),
  `transitionStarts()` and `outgoingDrained()`. Nothing polls the core.
- A stop's commit (`Transitions::commitStopped()`) usually runs in task context, but in the tick when a piece ends
  by itself while a transition is Queued. That happens only if the preload ends in the 12 ticks between the end
  line's decision tick and the line, so the command is still in the steering queue. It calls
  `changeSwingInterval()`, which is safe there only because `endPlayback()` has cleared `playbackState` first: the
  call then sets the interval and reschedules nothing. Keep anything added to that commit safe for the tick.
- When a piece ends by itself after a BACK tap dropped the command, the stop in the tick runs `endDropped()` instead:
  `cancelIncoming()` (nothing to glide back, the generator is gone), `releaseIncoming()` (adds the incoming Outputs
  to the retirement list; the free job frees them later), a trace and `phaseChanged()`. Keep anything added there
  safe for the tick too.
- The start step's cost in the tick (`gen_transition start ... us=`, about 125 to 130 µs) is mostly cache misses in
  two copies of cold SDRAM data: the sound picks (2.7 KB, `commitRecipe()`) and the incoming roster (312 bytes).
  newlib's `memcpy` prefetches with `pld` only when source and destination differ in their address modulo 8; with
  the same remainder it waits for each line, which costs 7 to 8 µs over the two copies. So `currentRecipe`,
  `pendingRecipe`, `transitions` and `performAdapter` are aligned to 8, the members are ordered so that each copy's
  offsets differ modulo 8, and a `static_assert` beside each holds it. Without that the link order decides: a change
  that only moved `.sdram_bss` objects made the start step 5 % slower. When a tick figure moves and the code in the
  timed window disassembles the same, compare the addresses of the data it copies.
- The step's cost in the tick (H1, `gen_h1 mean_ns`) depends on where its code lies across cache lines. PR #208
  changed no code in the timed window and no address modulo 32 of the data it reads, but its smaller image moved
  the step's functions by 16 to 28 bytes modulo 32, each group by its own amount, and the step was 1 to 1.5 µs
  (2 %) slower on the Deluge. So `Generator::decideNextStep()` is `GEN_STEP_ENTRY` and the functions it calls are
  `GEN_STEP_CODE` (`core/types.h`): they lie packed in the section `.text.generative_step`, which starts on a
  cache line, and `decideNextStep()` is not inlined into `Session::doTickForward()`. Code elsewhere can no longer
  move them against each other. To list the step's functions, disassemble the hardware build and follow the calls
  from `decideNextStep()`; each Generator function found must lie between the section's first and last symbol in
  `nm -n`. Mark one that does not. The marks are added by hand and no build step or test checks them, so a change
  that gives `decideNextStep()` a new callee, or moves work into a new helper, must run this check on the hardware
  build. Growing `decideNextStep()` also moves LTO's inlining: with the Sustain flavour (#123), `sectionRules()` and
  `drawAutoFill()`, inlined before, came out as functions in plain `.text`, and `noteFor()`, `voiceChord()`,
  `chord()` and `extensionLevel()` became step callees; `pickEntry()` and `updatePick()` had been outside since earlier slices. With the Arp (#126), `updateAnswer()` came out as a function and `snap()` gained a step caller (`arpNote()`). With the Motif (#129), `playingStep()`, `updateLine()`, `onsetLength()`, `fillRules()`, `chord()` and the line roll helpers `drawLineStep()` and `rollLineStep()` came out as functions, while `rollLine()`, `mutateLine()`, `motifNote()` and `updateAnswer()` are inlined. All are marked now. The check:
  take the section's range from `build/Release/deluge.map` (`.text.generative_step`), disassemble it, and list the
  `bl` targets in the `deluge::generative` namespace outside it; there should be none. Not pinned: the Generator's constant tables (`curves::`, `kLevelVelocity` and
  the like, in the image's one `.rodata` block), the division helpers and `memset`, and which cache sets the
  section falls in. A change to the step's own code changes the layout inside the section, so measure H1 on the
  Deluge then, in several sessions per build: each session rolls its own piece, and the mean moves by about 1 µs
  with the piece. Two other layouts of the same code read slower in one or two short sessions each: every function
  on its own cache line (`perform_sidebar.txt` 56.6 µs against 54.4 to 55.5 packed), and packed with fifteen more
  functions marked (55.6 and 55.7 µs).
- Two `TransitionJob`s alternate, so a request can cancel the one running and wait in the next slot while it
  unwinds. A job ending for an older request changes nothing (`TransitionLifecycle::loaded()` returns false and the
  job's Outputs are freed). Retired Outputs, a sound re-roll's and a transition's, go through `retirements`
  (`retirements.cpp`): one list, one free job, and the cap's 1-beat fade.
- Sound re-rolls (`generative/firmware/sound_swaps.cpp`): the preload links the new Output into Perform's song while
  it plays (`buildPartOutput`, which re-syncs the clip to the session), and `chunks.commit()`s once linked, so a cancel
  after that is a free, not an unwind. The swap happens in the tick, on the core's Sound event; freeing (stock
  `removeSessionClipLowLevel` + `deleteClipObject(DELETE)`, `freePartOutput`) only in a job.
- Sound swaps, transitions and retiring Outputs are serviced in one place, `Pump::service()`
  (`generative/firmware/pump.h`), which `PerformView::graphicsRoutine()` calls once; the three `service()` functions
  are private to it. What keeps the UI off a freed Output is in the pump, not in the order of the view's frame:
  before an Output goes to the free job, `Retirements::service()` asks the view to let go of it
  (`PerformView::releaseOutput()`), and one the view still holds waits. A UI reference to a part's clip or Output
  that lasts across frames needs a line there. The view's rebind on a swap and its focus drop on a new roster are
  only what the player sees at once. Test builds check by identity at the top of `freePartOutput()` (the gold knobs,
  the song's current Clip, every `SoundControls` set), so on both routes to a free: the free job, and
  `TransitionJob::unwind()`, which frees a cancelled preload's Outputs itself without asking the UI. A hit sends
  `gen_retire_referenced by=...`, that Output isn't freed, and nothing more is handed to the free job until Perform
  is left. `pump.h` also says when the pump doesn't run (under the sample browser's preview or QWERTY keyboard, the
  Slicer and the sample marker editor).
