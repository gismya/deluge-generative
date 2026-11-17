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
  Outputs, the sound picks and palette, popups and the `gen_transition` events. The header lists the adapter's members; the
  harness's adapter is `TransitionAdapter` in `tests/spec/generator/transition_piece.h`. A new rule goes in the
  lifecycle with a spec in `transition_lifecycle_spec.cpp`; an adapter member only does what needs the firmware. The
  lifecycle is a template so that no adapter call is virtual, and it holds no pointer, since `transitions` sits in
  SDRAM that is only cleared. The `gen_transition` texts come from `traceText()` in the header, for both adapters.
- The handover (the pending Recipe becomes the Current Recipe) is the steering's, not the lifecycle adapter's:
  `PerformSteering::handOver()`, and `takeOverStopped()` at a stop's commit, on `perform::PendingRecipe`
  (`perform/pending_recipe.h`). The firmware reaches them through `PerformAdapter::incomingStarts()` (the handover,
  then `instanceStarts()`, the start's own sequence), `incomingCommits()` and, leaving, `pendingCommitsOnLeave()`;
  the harness's `TransitionAdapter` calls the same ones in the same order, its leave's commit included. What only the
  firmware has stays in `Transitions::commitPending()`, around the handover: the staged palette before it (so the
  menus `newRecipe()` refreshes read the new genre's), the sound picks' copy and `soundSwaps.forgetPrevious()` after
  it. A new step of the handover that the harness can
  run goes in the steering, with a spec in `transition_lifecycle_spec.cpp`; one hook per firmware step on the
  lifecycle's adapter was rejected (#260): it widens that contract and splits the start step's timed window.
- A genre change's preload (`TransitionJob::run()`) reads the new genre's palette override into the Current Recipe's
  spare override (`CurrentRecipe::stagePalette()`) and rolls and loads from it. It works out once whether this is a
  genre change and records it beside the pending Recipe (`PendingRecipe::genreChange`); `commitPending()` adopts the
  staged palette by that flag, not by comparing genres. Two preloads never run at once (one cancels and unwinds
  before the next starts), so one spare is enough. `Transitions::phraseLength()` reads the Current Recipe's genre,
  so the lifecycle reads it for the outgoing roster's cap before the pending Recipe replaces it: at the start step
  (kept until the drain) and at a stop's commit. No shipped genre has another phrase length yet: the harness's
  `kFourBarPhrases` and `kOneBarPhrases` (`transition_piece.h`, Acid with 4- and 1-bar phrases) cover changes
  between phrase lengths, in `transitions_spec` and `transition_lifecycle_spec`.
- The rosters live in the Perform adapter's sink; `Transitions` changes them only through `PerformAdapter`'s
  transition calls (`incomingStarts()`, `incomingCommits()`, `retireOutgoing()`), and every roster it drains goes
  through `Retirements::retireRoster()`. The start step's mod-8 `static_assert` on the roster copy sits in
  `incomingStarts()`, where the copy is, and reads `incoming_`'s offset from `Transitions::incomingOffset()`.
- `Retirements` has a share per owner (`room(Owner)`), and nothing retires without checking its share first: a sound
  press against `SoundSwaps::retirementsOwed()`, a transition request against `Transitions::retirementRoom()` (the
  share less pending, less a roster for a `TransitionJob` still active, which `preloaded()` retires if it ends for an
  older request). The lifecycle asks for one roster's room, two when it replaces a Queued one. `gen_retire_overflow`
  never fires; if a new path retires Outputs (M3's LOAD, Capture), give it a share or count it in an owner's check.
  The harness models it (`TransitionAdapter::jobs`, `retirementRoom()`): the newest preload ends last, so its
  `load()` leaves the runner idle.
- A transition request (`TransitionLifecycle::request()`) presses a cancel, the playing take and a sound's cancel into
  the steering queue. It counts the places first (`cancelPlaces()`, `PerformSteering::waitingTakes()`, the adapter's
  `waitingSounds()`) and is refused with QUEUE FULL before anything is dropped, then submits the preload before it
  drops or cancels anything: a refused or failed request changes nothing. Keep both orders when the confirm drops
  something new. Each count and its drop walk one visitor (`forEachWaitingTake()`, `SoundSwaps::forEachWaitingSound()`),
  so they can't disagree. What to preload (the genre) goes through `request()` to `submitPreload()` and on to the job;
  the lifecycle never reads it, and nothing keeps it for a later submit.
- `Runner::submit()` on a free runner makes the job active at once, so `runner.active()` read after a submit is the
  new job: cancelling the old preload by `active()` after the submit cancelled the new one (every delugemu transition
  script failed; the harness can't see it). `Runner::submitReplacing()` reads the job to cancel first.
- A job whose task can't start (`RunnerHost::startTask()` fails) ends Failed inside `submit()`, which still returns
  true. A caller that changes state after a successful submit has to allow for it. The transition's preload ends with
  `loadEnded(request_ + 1)` before `request()` has taken that number: the lifecycle reads an end for the next request
  as one inside its own submit, and `request()` then gives up with nothing changed. The other submitters (a sound's
  preload, Perform entry and leave, the free job) change nothing after the submit that the job's `finished()` doesn't
  already undo.
- The working animation is up while a preload runs, a sound's or a transition's (spec §12.10), and one file holds
  that rule: `generative/firmware/working_animation.h`. A press that may start a preload runs inside
  `pressShowingWork()`, which puts the animation up first unless a preload runs (a job whose task can't start ends
  inside the press) and takes it down after only if `preloadRunning()` is false then: a refused or ignored press can
  sit beside a running preload (QUEUE FULL while Preloading, a sound press refused while a transition preloads), whose
  animation must stay. A sound preload's end calls `soundPreloadEnded()`, which leaves the animation up while a
  transition is Preloading: a confirm cancels a running sound preload and its own preload waits for it to unwind.
  `preloadRunning()` counts a transition by its phase, not by `runner.active()`, since its job may still wait in the
  next slot. Two hand-written copies of this rule would each miss the other kind of preload: add a new preload
  kind to `preloadRunning()`, and a new press that can start one goes through `pressShowingWork()`. A transition's
  ends call `transitionPreloadEnded()`, never `removeWorkingAnimation()` directly. While Perform is being left no
  preload's end takes the animation down: `runner.cancelAllThen(teardown)` lets a cancelled preload's `finished()`
  run after the leave has put up its own animation, and only `LeaveJob::finished()` takes that down
  (`gen_leave_teardown working=1`, in `perform_sounds.txt` and `perform_transition.txt`).
- The modal screen over entry and leave (`GenerativeModalUI`, `gui/ui/generative_modal.*`) is opened with the job it
  covers (`performAdapter.entryJob()` or `leaveJob()`), and BACK cancels only while that job is `runner.active()`. The
  leave runs `cancelAllThen(teardown)`: a preload it cancelled stays the active job until it unwinds, and a bare
  `runner.cancel()` there would find that preload, return true (it is cancelled) and dismiss the leave's modal while
  the teardown still waits. The job
  must outlive the modal (both are file-scope objects in `entry_leave.cpp`).
- `TransitionLifecycle::reset()` leaves the request counter alone. A preload the leave cancelled ends before
  `LeaveJob::run()` (the runner ends cancelled jobs first; `forget()` runs inside the leave), and must still reach
  `Transitions::preloadEnded()`: while leaving it only commits the rolled Recipe for the leave, and the working
  animation stays up for `LeaveJob::finished()`. A preload that ends Done while leaving reaches it too, through
  `loaded()` (#292): the commit hangs on the leave, not on the job's outcome. The firmware's `TransitionJob` can't
  end Done once the leave has cancelled it (`run()` ends on a `chunks.boundary()`, which reads the cancel after its
  yield, and `Runner::runActive()` hands the outcome to `finished()` with no yield between), so only the harness
  reaches that path today; a job that commits (`Chunks::commit()`) would reach it on the Deluge.
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
  two copies of cold SDRAM data: the sound picks (2.7 KB, `commitPending()`) and the incoming roster (456 bytes).
  newlib's `memcpy` prefetches with `pld` only when source and destination differ in their address modulo 8; with
  the same remainder it waits for each line, which costs 7 to 8 µs over the two copies. So `currentRecipe`,
  `pendingRecipe`, `transitions` and `performAdapter` are aligned to 8, the members are ordered so that each copy's
  offsets differ modulo 8, and a `static_assert` beside each holds it. Without that the link order decides: a change
  that only moved `.sdram_bss` objects made the start step 5 % slower. When a tick figure moves and the code in the
  timed window disassembles the same, compare the addresses of the data it copies.
- The step's cost in the tick (H1, `gen_h1 mean_ns`) depends on where its code lies across cache lines. A change
  that touched no code in the timed window and no address modulo 32 of the data it reads, but shrank the image, moved
  the step's functions by 16 to 28 bytes modulo 32, each group by its own amount, and the step was 1 to 1.5 µs
  (2 %) slower on the Deluge. So `Generator::decideNextStep()` is `GEN_STEP_ENTRY` and the functions it calls are
  `GEN_STEP_CODE` (`core/types.h`): they lie packed in the section `.text.generative_step`, which starts on a
  cache line, and `decideNextStep()` is not inlined into `Session::doTickForward()`. Code elsewhere can't
  move them against each other. To list the step's functions, disassemble the hardware build and follow the calls
  from `decideNextStep()`; each Generator function found must lie between the section's first and last symbol in
  `nm -n`. Mark one that does not. The marks are added by hand, so a change
  that gives `decideNextStep()` a new callee, or moves work into a new helper, must mark it. Growing
  `decideNextStep()` also moves LTO's inlining: a function inlined before can come out as a function in plain
  `.text`, or become a step callee (the Sustain, Arp and Motif flavours each moved several). Moving the Generator's code
  between files does too, though LTO sees one program either way: the order the functions reach it in, and an
  `inline` on a shared helper, change its choices. Splitting `generator.cpp` by area (#244) put `updateControls()`
  and a clone of `onPick()` out of line (both marked since) and changed which flavours inline into the step; every
  callee one file calls in another is marked now (#309). The check,
  `scripts/generative/step_section.py` (`check_all.py` runs it on `build/Release`): take the section's range from
  `build/Release/deluge.map` (`.text.generative_step`), disassemble it, and list the branch targets in the
  `deluge::generative` namespace outside it, by address; there should be none. Not pinned: the Generator's constant tables (`curves::`, `kLevelVelocity` and
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
