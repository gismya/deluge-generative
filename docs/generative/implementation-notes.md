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
  rest. Build text with `Text` in `perform/queue_text.cpp`. Firmware code may use `snprintf`: it gets the
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
  `keyRoot()`). A new field has to fit, or the block outgrows H18.
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
- Portable text (`sectionName()` in `core/types.h`, `perform/queue_text.cpp`, `perform/arc_map.cpp`) is plain ASCII
  built without l10n, which the harness doesn't have.

## Timing

- The Generator decides each step 12 ticks ahead (`kLookaheadTicks`, ADR 0003, §12.2). Everything the UI asks
  (`partMuted()`, `sectionForced()`, `planArc()`) describes the last decided step, about 45 ms before it is heard: a
  mute pad changes colour that early.
- The driver plays as it decides, so events due before each decision play first (`perform/perform_driver.cpp`). Every
  jump calls `NoteSink::jumped()` once, after its note-offs.
- Read the tick magnitude from `currentSong->getInputTickMagnitude()` on every tick: an external clock changes it
  mid-play. The Generator's own is `kGeneratorTickMagnitude` (`core/types.h`).

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
- Sound re-rolls (`generative/firmware/sound_swaps.cpp`): the preload links the new Output into Perform's song while
  it plays (`buildPartOutput`, which re-syncs the clip to the session), and `chunks.commit()`s once linked, so a cancel
  after that is a free, not an unwind. The swap happens in the tick, on the core's Sound event; freeing (stock
  `removeSessionClipLowLevel` + `deleteClipObject(DELETE)`, `freePartOutput`) only in a job. The view binds the gold
  knobs again when `soundSwaps.swaps()` changes, before it calls `service()`, which is what submits the free: keep
  that order, or the knobs point at a freed clip.

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
- Leaving returns both heaps exactly (#167; `gen_mem` test events). `gen_emu.py leak` allows no change at all, so a
  few bytes either way is a finding. Find the owner with a heap walk: diff the allocated blocks of two `after_leave`s,
  then search RAM for pointers to the new ones.

## Perform UI (`gui/views/perform_view.*`)

- Firmware pad `y` 0 is the **bottom** row. The spec and the control-layout prototype count rows from the top:
  convert with `rowFromY()`.
- Stock files guard Generative behaviour with `rootUIIsGenerativePerform()` (`gui/ui/ui.h`), which holds under
  overlays. A stock file that calls into Perform includes the one header it needs (`gui/views/perform_view.h` in
  `view.cpp` for F15, `perform_adapter.h` in `encoder_input.cpp`, `buttons.cpp` and `playback_handler.cpp`), and
  holds nothing but the gate and the call.
- `View::setModLedStates` overwrites the SONG, CLIP and AFFECT ENTIRE LEDs. Call `assertPageLeds()` after every stock
  call that can run it.
- `graphicsRoutine()` must not call `UI::graphicsRoutine()`: that calls the root UI's, which is Perform's own.
- `TimerName::UI_SPECIFIC` fires on whatever UI is current, so Perform polls holds (the End pad) in
  `graphicsRoutine()`.
- Redraw only changed rows (`uiNeedsRendering` masks). Any main-pad change still sends all 8 column pairs to the PIC.
- Shown text comes from l10n (`gui/l10n/`); the `generative-perform-l10n` hook catches literals passed straight to
  OLED calls.

## Extension points left open

- **Transitions (#115):** `PerformAdapter` allocates both Generator blocks at entry but uses only `blocks_[0]`.
  `planArc()` doesn't know about a pending transition.
- **New flavours:** a `decideX` beside `decideLoop` in `core/generator.cpp`. **New interplay rules:** a `RuleKind`
  and a case in `Generator::against()`.
- **Replay (M4):** `Generator::ended()` reads the pending list, which replay doesn't have (comment on `ended()`).
  Glides log their length in `Command::length`. Jumps aren't logged.
- **Capture (M4):** the early return under Perform in `PlaybackHandler::recordButtonPressed()` becomes the Capture
  mark.
- **Sample Riser timed by its file end (§9.5):** not built (#160).

## Build and tools

- `set_source_files_properties` in `src/deluge/CMakeLists.txt` needs `TARGET_DIRECTORY deluge` to reach the target.
- `./dbt sizediff` noise: under LTO a different git SHA in `version.h` alone moves `handle_cpu_fault`,
  `fault_handler_print_freeze_pointers` and an `OrderedResizeableArrayWith32bitKey` constructor (about +20 B text).
  Don't chase it.
- Harness specs: one `describe` per `*_spec.cpp`, named after the file stem. Don't name a local `it` (CppSpec's
  macro). Specs reach `pack/` and fixtures through `GENERATOR_REPO_DIR`.
- Probe scripts: start every script with `start_state`, then Perform scripts with `include perform_open.txt` and
  `include perform_confirm.txt`. An expect's timeout needs the keyword, last on the line: `expect event gen_left
  within=15`. A bare trailing number is part of the match text (`expect event gen_section INTRO 0` matches section
  0). Over delugemu's DIN link test-port events can arrive seconds late: give `expect event` a long timeout before an
  `expect no_event`. SHIFT+SONG right after reaching session view needs about 1 s (the zoom animation holds a UI
  mode).
