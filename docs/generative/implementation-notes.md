# Generative mode: implementation notes

What building Generative mode has taught, by area: gotchas, firmware traps and the extension points left for later
tickets. The [spec](spec.md) says what to build; read the sections here for the code you touch before working on it.
Paths are under `src/deluge/` unless they say otherwise. When you learn something the next agent would trip on, add
it here; when a note stops being true, fix or delete it.

Once this file passes about 200 lines, split it into one file per area (core, timing, runner, firmware adapter,
Perform UI, build and tools) behind a short index here.

## Portable code (`generative/core/`, `perform/`, `runner/`)

- The harness builds these three trees and nothing else (`tests/spec/generator/CMakeLists.txt`); `generative/firmware/`
  is the firmware side. The `generative-rules` hooks (`scripts/lint/generative_rules.py`) keep float, firmware
  includes and libc stdio out.
- `snprintf` and friends in portable code break the firmware link: newlib's stdio pulls in `_close`, `_sbrk` and the
  rest. Build text with `Text` (`perform/text.h`). Firmware code may use `snprintf`: it gets the
  non-allocating `lib/printf.h` through `io/debug/log.h`.
- `Purpose` (`core/draw.h`) and `CommandKind` (`core/steering.h`) are append-only: a purpose's value is part of every
  draw key, and logs store command kinds.
- Every land-time effect of a command goes in `Generator::apply()`. Replay feeds logged commands to `apply()`
  (ADR 0005), so an effect done anywhere else is lost on replay.
- Bar- and phrase-quantised commands wait in `PendingList` (`core/steering.h`); a `static_assert` in
  `core/generator.h` keeps its capacity above every slot kind. `Generator::stepsToLine()` is the one line rule: both
  `landPending()` and `landsAt()` (QUEUE's countdowns) use it.
- Capacity is sized by construction: `kMaxEventsPerStep` and `worstCaseStepEvents()` in `core/generator.h`, and
  `static_assert(sizeof(Generator) <= 16 * 1024)`. When `sizeof(Generator)` changes, measure it on ARM and update
  §20: compile a probe file with the firmware's `arm-none-eabi-g++` containing
  `template <int N> struct Size; Size<sizeof(Generator)> s;` and read the size from the error.
- `Event` is 16 B, pinned by a `static_assert`: the event buffer holds hundreds per block. Kind-specific fields share
  the layout (the NoteOn flags and Control's `snap` are bit-fields, KeyScale's root lives in `pitch`, read it with
  `keyRoot()`). A new field has to fit, or the block outgrows H18. The buffer's keep priority lives in `Event::keep`
  while an event is buffered (0 once it plays), so `operator==` names every field but `keep`: add a new field there.
- Sound controls (spec §11.3–§11.5) are worked out in `Generator::updateControls()`, after the step's notes are
  decided, so Throw sees the step's hits. Each `ControlSet` has a `ControlSetKind` (`core/genre.h`), and
  `setMoves(kind, control)` is the one rule for which layers reach which control: `updateControls` skips the rest,
  and `kMaxControlEventsPerStep` is counted from it. A new layer or gesture that moves another control changes
  `setMoves`, and with it the buffer. `controlSetsOf`/`controlSetKind`/`controlSetLane` lay sets out; the firmware's
  `SoundControls::makeSet` uses the same helpers, so the two always agree.
- Genre data that would hang the device is checked at compile time: a bad interplay rule makes `planned()` recurse
  forever, so each genre file has `static_assert(interplayRulesValid(...))` (`core/genres/techno.cpp`). Other data
  checks are in `tests/spec/generator/genre_data_spec.cpp`, which only runs in the harness.
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
- The FX part's lanes have no pools: section rules place their notes. Specs that walk pools skip pool-less lanes.
- Fills (§8.7) are worked out per step in `Generator::fillRules()`, right after `sectionRules()`, so they see the Gap
  and the roll; they set `Step::rest` and `Step::swap`. State is four bytes: the phrase's automatic fill (drawn on its
  first step), the held one, the pitched parts rested on the last step (a Rest ends their notes only as it starts)
  and whether a fill covered the last step (the Phrase accent reads it). A lane's fill patterns are its own
  `LaneSpec::fillPool`; a Swap picks from it with `pickEntry()` and the fill purposes, keyed on the lane's pick (a
  lane with only a fill pool still moves its pick at Drops and re-picks, through `placePick()`), and
  `fillPick()` gives the fill pattern an entry index after the pool's, so its draws never match a normal pattern's.
  Specs that walk pools at Density 0 turn fills off (a Rest removes anchors).
- The sounding-note and late-entry tables are one pool each (`SoundingNotes`), laid out by role in
  `Generator::start()`: a part gets `maxSounding(role)` slots, and the pool holds a roster's worth with at most one
  Drums part (51). A roster past it (bad data) leaves a part no slots, so its notes don't play.
- Portable text (`sectionName()` in `core/types.h`, `perform/queue_text.cpp`, `perform/arc_map.cpp`,
  `perform/oled_page.cpp`) is plain ASCII built without l10n, which the harness doesn't have. It is built with
  `perform::Text` (`perform/text.h`), not printf; its adds are out of line, since inlined at every use they cost
  about 2 KB of image.

## Timing

- The Generator decides each step 12 ticks ahead (`kLookaheadTicks`, ADR 0003, §12.2). Everything the UI asks
  (`partMuted()`, `sectionForced()`, `planArc()`) describes the last decided step, about 45 ms before it is heard: a
  mute pad changes colour that early.
- The driver plays as it decides, so events due before each decision play first (`perform/perform_driver.cpp`). Every
  jump calls `NoteSink::jumped()` once, after its note-offs.
- Read the tick magnitude from `currentSong->getInputTickMagnitude()` on every tick: an external clock changes it
  mid-play. The Generator's own is `kGeneratorTickMagnitude` (`core/types.h`).
- At a transition's start step the driver's clock and the playing instance's steps start again from 0 (the new
  roster's step 0), and the outgoing instance's events play at their tick less the start step's. Anything the
  adapter counts in steps of the playing instance restarts there (`PerformAdapter::restartStepCounters()`). Anything
  the driver latches as "already sent" to an instance that a new one clears (a held FILL) goes in
  `PerformAdapter::instanceStarts()`, which a start and a transition's start step both call.
- A sample Riser's timing lives in the Perform driver, not the core (§9.5, §10.5): the core places the note from the
  Build's first step to the Drop, and `PerformDriver::playDue()` holds its note-on back (`holdRiser()`) when the sink
  says it's a sample (`NoteSink::sampleMicros()`, the adapter's `OutputSink`: the row's first range at the system
  rate, for a ONCE or CUT row). `riserStart()` (`perform/riser_timing.h`) walks back from the Drop a beat at a time at
  `plannedTempo()`: a running glide, then a transition's planned glide across its Build. Each change to that plan
  (`setTempo`, `glideTempo`, the transition's window, a cancel) times it again; a note-off that comes first (a forced
  Section, a jump) drops it. Under an external clock (`NoteSink::tempoFollowsMaster()`, read each `advance()`) the
  plan is the tempo last set, the master's as `sampleExternalTempo()` samples it, and `glideIntoTransition()` plans
  no window: nothing glides (§9.7). A mute landing while it waits doesn't stop it (the core counts the note as
  sounding, §12.5). Tempo is driver-side (§2.2), so only the driver knows the plan, and the Generator block
  doesn't grow. The event stream is unchanged: the hold is at the output, like a mute.

## Runner and jobs

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
- `TransitionLifecycle::reset()` leaves the request counter alone. A preload the leave cancelled ends after
  `forget()`, and must still reach `Transitions::preloadEnded()`: it removes the working animation and commits the
  rolled Recipe for the leave.
- The lifecycle hears from the tick through the driver's sink: `transitionLanded()` (Queued to Committed, never for
  a cut), `transitionDropped()` (a BACK tap took the command: only a flag, the pump ends the transition),
  `transitionStarts()` and `outgoingDrained()`. Nothing polls the core.
- A stop's commit (`Transitions::commitStopped()`) usually runs in task context, but in the tick when a piece ends
  by itself while a transition is Queued. That happens only if the preload ends in the 12 ticks between the end
  line's decision tick and the line, so the command is still in the steering queue. It calls
  `changeSwingInterval()`, which is safe there only because `endPlayback()` has cleared `playbackState` first: the
  call then sets the interval and reschedules nothing. Keep anything added to that commit safe for the tick.
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
  `nm -n`. Mark one that does not. Not pinned: the Generator's constant tables (`curves::`, `kLevelVelocity` and
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

## Firmware adapter and the private song (`generative/firmware/`)

- Perform's song is built while another song is current. Drum, arp and sidechain constructors take sync defaults
  from `preLoadedSong`, else `currentSong`, so `applySyncDefaults()` (`perform_song.cpp`) resets them. `preLoadedSong`
  is set only at the swap: a pending launch event could otherwise swap in a half-built song.
- `doSongSwap()` leaves `currentUIMode` and the working animation set. Clear both after it, as `perform_adapter.cpp`
  and stock Clear song do.
- `lastClipInstanceEnteredStartPos = -1` keeps PLAY on session playback. `Arrangement::doTickForward` has no Generator
  hook.
- Stop reaches Perform through `RootUI::playbackEnded()`, not the tick hook.
- Write tempo with `Song::setBPMInner()` (public since F14). `Song::setBPM()` goes through the live-input path and
  can log undo.
- Don't write Generator values through `AutoParam::setCurrentValueInResponseToUserInput()`. It resets the
  interpolation increment, and with RECORD armed it writes automation nodes. It is the player-edit point (F3).
- Sound controls (`generative/firmware/sound_controls.cpp`): homes are read when a piece starts, and a stop puts
  every bound param back. Ramp with `AutoParam::setLiveRamp()` (F2) and set the param's bit in its summary's
  `whichParamsAreInterpolating`; nothing clears that bit until playback ends, so an idle flagged param still costs a
  check per render window. The live target shares the word of the float-only interpolation remainder (odd while
  live), so never arm a ramp on a float-interpolated param (tempo) or on one with automation.
- Player edits (F3, `PerformAdapter::playerEdited`): every route that edits a param ends in
  `AutoParam::setCurrentValueInResponseToUserInput()`, which calls Perform after the value lands. Bound params are
  matched by collection and param id (`idOf()`; for a cable, its source and destination), never by the AutoParam pointer
  (stock may delete the edited cable inside the edit), within the set whose ParamManager the model stack carries. A
  stock edit to the patching (`setupPatching`, `deletePatchCable`, a knob creating a cable) reorders `patchCables[]`, so
  a bound cable is kept as its source and destination and looked up with `getPatchCableIndex()` each use; 255 means the
  player deleted it.
- A cable set to exactly 0 is deleted by `PatchCableSet::notifyParamModifiedInSomeWay`, so Wobble's ×0 leaves ±1.
- Only disk-level FatFs errors are card errors (`card_sounds.cpp`). A missing or bad folder lists nothing and falls
  back (§10.7).
- A test hook can fault the card: "hold card_error" makes `CardFolders` and Perform's loaders fail with `SD_CARD`
  (`cardErrorInjected()`, `card_sounds.h`). delugemu can't pull or fault its card.
- An unused Sample isn't freed: stock firmware keeps it as a stealable cache, and it still holds internal and
  external heap (its cluster list, its path). Each entry picks new sounds, so leaving deletes every unused audio file
  (`LeaveJob::deleteUnusedAudioFiles()` in `perform_adapter.cpp`) before it builds the blank song. Free it there,
  not after: a blank song built around cached Samples lands elsewhere on the heap each time, and free memory then
  drifts by 8 B. Temp-recorded Samples are kept for `deleteAnyTempRecordedSamplesFromMemory()`, which also resets
  the recording number.
- MIDI in (#117, `perform_midi.cpp`): the stock MIDI routes call it under `rootUIIsGenerativePerform()` (F8, F10,
  F18). MIDI is read in `PlaybackHandler::midiRoutine()`, which `AudioEngine::routine()` also runs, so these calls can
  land wherever the audio routine runs (inside a card load too), as MIDI TAP and F3's learned edits already do. Stock's
  note route into a Perform part's Output hung delugemu, so F18 drops notes before any Output sees them. Follow with
  nothing focused builds a song model stack on the caller's (`followedSongParam()`), since stock follow has no song
  context. Feedback: `View::sendMidiFollowFeedback` hands every call to `sendFeedback()`; while playing,
  `PerformView::graphicsRoutine()` keeps stock's `SEND_MIDI_FEEDBACK_FOR_AUTOMATION` timer armed, and its rate check
  runs the pass. `perform::FeedbackFilter` keeps each CC's last value so a pass sends only what moved (H17).
- Leaving returns both heaps exactly (#167; `gen_mem` test events). `gen_emu.py leak` allows no change at all, so a
  few bytes either way is a finding. Find the owner with a heap walk: diff the allocated blocks of two `after_leave`s,
  then search RAM for pointers to the new ones.

## Perform UI (`gui/views/perform_view.*`)

- Firmware pad `y` 0 is the **bottom** row. The spec and the control-layout prototype count rows from the top:
  convert with `rowFromY()`.
- Stock files guard Generative behaviour with `rootUIIsGenerativePerform()` (`gui/ui/ui.h`), which holds under
  overlays. A stock file that calls into Perform includes only the headers it needs (`gui/views/perform_view.h` in
  `view.cpp` for F15, `perform_adapter.h` in `encoder_input.cpp`, `buttons.cpp` and `playback_handler.cpp`,
  `perform_midi.h` in `view.cpp`, `midi_follow.cpp` and `playback_handler.cpp` for F8, F10 and F18), and holds
  nothing but the gate and the call.
- `View::setModLedStates` overwrites the SONG, CLIP and AFFECT ENTIRE LEDs. Call `assertPageLeds()` after every stock
  call that can run it; `assertLeds()` also runs it every frame, writing only the LEDs whose state differs
  (`indicator_leds::ledStates`, F17), which covers stock calls Perform doesn't see (a menu over it).
- `graphicsRoutine()` must not call `UI::graphicsRoutine()`: that calls the root UI's, which is Perform's own.
- `TimerName::UI_SPECIFIC` fires on whatever UI is current, so Perform polls holds (the End pad) in
  `graphicsRoutine()`.
- Redraw only changed rows (`uiNeedsRendering` masks). Any main-pad change still sends all 8 column pairs to the PIC.
- The sidebar, the CLIP page and the KEYBOARD page are page models in `generative/perform/` (`sidebar.h`,
  `clip_page.h`, `keyboard_page.h`), as the SONG page's `composeArcMap()` is: a `compose…()` takes a plain input and
  returns cells, and a `describe…()` makes the page's test-port text (`gen_sidebar`, `gen_clip`, `gen_faders`,
  `gen_keyboard`) from those cells: the text and the pads are made from the same cells. The view fills the input, turns
  cells into RGB with the blink phase and diffs against its pad caches. A new look is a cell field and a spec case,
  not a branch in the view. Test builds compose the CLIP page on every page, since the scripts read its text from
  any page; Release composes it only when it shows.
- What the SONG page keeps between frames is `perform::ArcMapState` (`generative/perform/arc_map.h`): the Arc plan
  and the key it is worked out again from, the queue, the X scroll with its clamp, and the now column's flash window
  (the step before and this one, sampled once per step, so a hit flashes for two 16ths). Every frame the view calls
  its `refreshPlan()`, `sampleFlash()` and `clampScroll()` and reports what they return. It composes the map only when something reads it:
  on the SONG page, and in test builds on every page for `gen_arcmap` and `gen_arcrow`.
- The OLED is a page model too (`generative/perform/oled_page.h`): `composeOled()` takes the status line's, the
  peeks' and the pages' input and returns an `OledScreen` of text and numbers, which `renderOLED()` draws. The view
  fills the input, applies the blink phase to the ARC page's queued boxes and redraws on a change. The words on it
  (`STOPPED`, `STARTING`, `HELD`, the macros' initials) are the model's own, in capitals as the status line shows
  them. `gen_arcpage` and `gen_macropage` are made from the screen by `describeArcPage()` and
  `describeMacrosPage()`.
- Other shown text (popups, menus, the PART page's control labels) comes from l10n (`gui/l10n/`); the
  `generative-perform-l10n` hook catches literals passed straight to OLED calls.
- Perform's menus (`gui/menu_item/generative/perform_menu.*`) open with `soundEditor.setup(nullptr, &menu)`, as stock
  song-level menus do; `PerformView::openMenu()` lets go of held pads first. A stock `Submenu` remembers its last row
  across openings, so probe scripts choose a row from the top (`encoder 3 -5`). The sound editor's `setLedStates()` and
  its SELECT/BACK handling run `View::setModLedStates()`, which sets F7 and F8 steady, SCALE off and the page LEDs
  the stock way: `PerformView::assertLeds()` puts them back every frame (Perform's `graphicsRoutine()` runs under the
  sound editor, which can see the view underneath). A menu value held while it's open (the edited key, a take row)
  records what it was read under (`Basis` in `perform_menu.cpp`: playing, pieces started, and the key presses or that
  part's take presses, BACK taps counting as both) and is read again when that moves, or when its submenu opens
  afresh; `refreshPerformMenu()` redraws the open item then.
- FILL has several holders: SYNC-SCALING and the KEYBOARD page's 12 FILL pads (`perform::FillHolds`).
  `PerformView::holdFill()` moves the song's fill mode only on the first down and the last up, latching SHIFT on the
  first; `PerformAdapter::sampleFill()` sends the song's fill mode to the core each tick, so MIDI FILL works too.
- A KEYBOARD fader press steers the core at once (`PerformAdapter::holdMacro()`, the Current Recipe untouched). The
  release keeps it as the centre after a tap (`keepMacroCentre()`) or steers back after a hold (`springMacroBack()`);
  `perform::MacroFaderHolds` tells them apart at `kMacroTapMs` (300, fixed: stock's Hold Press Time is left to stock's
  pads). Every SetMacro goes through `pushMacro()`, which `perform::MacroSteering` records: a start drops the queue,
  so `keepMacroCentre()` steers again when the last value sent differs (a start during the tap), and
  `instanceStarts()` sends held faders again, as it does FILL. While a fader holds a macro, `setMacro()` (the gold
  knobs, and any later MIDI path through it) moves only the centre (`MacroSteering::centreSteers()`). A pad held
  when the page changes, or whose release an overlay swallowed (`releaseLostHolds()` in `focusRegained()`, from
  `matrixDriver.isPadPressed()`), still lets go.
- The sound editor over Perform (§13.7, `PerformView::openSoundEditor()`): Perform never has a current Clip, but the
  stock editor reads it throughout, so while the editor is open on a part the song's current Clip is that part's clip,
  and F6's `editingClip()` reads that as a clip context. `focusRegained()` clears it. Test `isUIOpen(&soundEditor)`,
  not the current UI: a shortcut can open a UI over the editor (the sample browser), and the current Clip must stay
  its part's there. `closeEditorOnRetiredPart()` runs every frame and closes the editor once a sound swap or a new
  roster has taken its clip out of the roster; while a UI sits over the editor it can't, so
  `PerformView::releaseOutput()` answers Held for that Output (its other retirees go on) until the editor is closed. The UIs the
  editor opens over Perform (sample browser, audio recorder, sample marker editor, DX browser, rename) all read the
  part through the current Clip or the editor's pointers, so they stay valid that way. LEAVE can't start under the
  editor: BACK hold calls the current UI's `exitUI()`.
  Don't set the current Clip anywhere else: MIDI follow, the knobs and the popups use the Clip on the gold knobs.
- The F page (stock's `modKnobMode`) lives on each Output, but Thin / Drive (`thinDrivePart_`) is on a part. A sound
  swap's new Output comes up on its own page, so `bindKnobs()` sets the page from `perform::knobPage()`
  (`generative/perform/knob_page.h`) each time it binds a part: F7 while Thin / Drive is on that part. Anything that
  replaces a part's Output must end in `bindKnobs()` (the swap reaction and `releaseOutput()` both do).
- Every pad press under the sound editor and Perform's menus reaches `PerformView::padAction()` (F7):
  `shiftOpensSoundEditor()` (`generative/perform/grid_page.h`) picks the SHIFT + main pad shortcuts, leaving out the
  KEYBOARD FILL pads and the CLIP page's NEW SOUND and SOUND EDITOR; a Perform menu (`editorOpen_` false) closes on a
  press. A SHIFT + pad that doesn't move an open editor (a param the target lacks) puts the current Clip back.
- With no current Clip, `getCurrentOutputType()` is NONE and the other current-Clip getters return null (`song.cpp`),
  so song-level editor paths never read null. Test builds fault on any null access: run the probe scripts after
  touching what the editor reaches.

## Extension points left open

- **Genre change (M2) and LOAD (M3):** both run `TransitionLifecycle` unchanged, through
  `TransitionLifecycle::request()`: add a call beside `Transitions::regenerate()` that says what to preload, and have
  `Transitions::submitPreload()` hand it to the job. `TransitionJob::run()` (`generative/firmware/transitions.cpp`)
  rolls a Regenerate's pending Recipe; a genre change rolls the new genre's facts and sounds (with its palette override,
  which `CurrentRecipe` reads only for itself today), a LOAD takes the file's. The lifecycle never reads the pending
  Recipe: `startIncoming()`, `rosterStarts()` and `commitStopped()` hand it over in the adapter. With a genre change
  `Transitions::phraseLength()` reads the Current Recipe's genre, which is the old one at a stop's commit and the new
  one once the outgoing roster has drained: decide which phrase the cap means. `TransitionGenreItem` shows only the
  genre playing; the others' rows open the same confirm once a genre change exists. The confirm always starts on BUILD
  INTO A DROP: a genre whose transition entry is Intro (Ambient) needs a section-rule field and
  `TransitionConfirm::setupAndCheckAvailability()` to read it. Incoming Outputs don't get unique names yet (§9.7):
  Perform's song is never saved in M1.
- **New flavours:** a `decideX` beside `decideLoop` in `core/generator.cpp`. **New interplay rules:** a `RuleKind`
  and a case in `Generator::against()`.
- **Replay (M4):** `Generator::ended()` reads the pending list, which replay doesn't have (comment on `ended()`).
  Glides log their length in `Command::length`. Jumps aren't logged.
- **Capture (M4):** the early return under Perform in `PlaybackHandler::recordButtonPressed()` becomes the Capture
  mark.
- **Long sample Risers (M2):** a Riser longer than its Build starts with it (`riserStart()` returns the note's own
  tick), so its end overshoots the Drop and the Drop's note-off cuts it. House's 4-bar and Dub's 1-bar Builds make it
  real: start partway into the file (the adapter's `play()` would need a start offset, `PerformDriver::holdRiser()`
  knows the time left). Compose and Capture (M3, M4) write the Riser's note late the same way, from their tempo
  automation, with `riserStart()`.
- **Fills:** the Roll kind (Psytrance) is a `FillKind` and a case in `fillRules()`, playing like `playRoll()`.
  Drop-outs (Dub) beat a fill on a part or lane they share: clear it from `Step::rest`/`Step::swap`.

## Build and tools

- `set_source_files_properties` in `src/deluge/CMakeLists.txt` needs `TARGET_DIRECTORY deluge` to reach the target.
- `./dbt sizediff` noise: under LTO a different git SHA in `version.h` alone moves `handle_cpu_fault`,
  `fault_handler_print_freeze_pointers` and an `OrderedResizeableArrayWith32bitKey` constructor (about +20 B text).
  Don't chase it.
- Harness specs: one `describe` per `*_spec.cpp`, named after the file stem. Don't name a local `it` (CppSpec's
  macro). Specs reach `pack/` and fixtures through `GENERATOR_REPO_DIR`.
- Stepping a piece in a spec: use `StepwisePiece` (`tests/spec/generator/harness.h`), not a hand-wired driver or a
  bare `Generator::decideNextStep`. `decideBefore(step)` stops at step - 1's decision tick, so a push then reaches
  `step`. `runToStepLine(step)` stops on `step`'s grid line, so a push then reaches step + 1. `runToStepLine` counts
  from the last call: after a jump, or once a transition's incoming instance plays, run in song ticks (`runToTick`).
  `decideBefore` and `pushAndDecide` count steps on the instance that plays when they are called, and return false
  if it stopped deciding first (the piece ended or stopped, or a transition's incoming instance took over). For a
  transition use `TransitionPiece` (`transition_piece.h`): it runs the real lifecycle, so `confirm()` is a request
  and a preload that ends well.
  Shared stream helpers are in `stream_helpers.h`. A helper there shares its name with every spec that includes it:
  delete the spec's own copy, or the call is ambiguous.
- Probe scripts: start every script with `start_state`, then Perform scripts with `include perform_open.txt` and
  `include perform_confirm.txt`. An expect's timeout needs the keyword, last on the line: `expect event gen_left
  within=15`. A bare trailing number is part of the match text (`expect event gen_section INTRO 0` matches section
  0). Over delugemu's DIN link test-port events can arrive seconds late: give `expect event` a long timeout before an
  `expect no_event`. SHIFT+SONG right after reaching session view needs about 1 s (the zoom animation holds a UI
  mode).
- A test-port hold (`hold NAME`, `release NAME`) is a row of `kHolds` in `io/midi/test_port.cpp` plus the hook that
  reads it. Nothing else needs a new hold added (the probe's `HOLDS_BEFORE_RELEASE_ALL` is frozen, for older builds): `start_state` sends `release all`, which sets every row off, so a row's
  `set(false)` must be safe when the hold was never on.
