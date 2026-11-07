# Generative mode: implementation notes

What building Generative mode has taught, by area: gotchas, firmware traps and the extension points left for later
tickets. The [spec](spec.md) says what to build; read this before working in an area. Paths are under `src/deluge/`
unless they say otherwise. When you learn something the next agent would trip on, add it here; when a note stops
being true, fix or delete it.

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
- Genre data that would hang the device is checked at compile time: a bad interplay rule makes `planned()` recurse
  forever, so each genre file has `static_assert(interplayRulesValid(...))` (`core/genres/techno.cpp`). Other data
  checks are in `tests/spec/generator/genre_data_spec.cpp`, which only runs in the harness.
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
- Folder scans (`generative/firmware/card_sounds.cpp`, the genre roll in `current_recipe.cpp`) yield through
  `AudioEngine::routineWithClusterLoading()`, not `Chunks`, so BACK isn't seen during a long scan. Pass the `Chunks`
  down when that matters.
- A live job (preloads, re-rolls) subclasses `Job` with `Kind::Live`, calls `chunks.boundary()` between Outputs and
  after each card read, and frees what it built when that returns true.

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
- Only disk-level FatFs errors are card errors (`card_sounds.cpp`). A missing or bad folder lists nothing and falls
  back (§10.7).
- Stock Clear song (SHIFT+LOAD → NEW) loses 96–104 B of external heap a cycle, and leaving Perform mirrors it. That
  is the baseline for enter/leave leak checks (`gen_mem` test events): `gen_emu.py leak` allows 104 B a cycle
  (`EXTERNAL_LOSS_PER_CYCLE`).

## Perform UI (`gui/views/perform_view.*`)

- Firmware pad `y` 0 is the **bottom** row. The spec and the control-layout prototype count rows from the top:
  convert with `rowFromY()`.
- Stock files guard Generative behaviour with `rootUIIsGenerativePerform()` (`gui/ui/ui.h`), which holds under
  overlays, and never include Perform's headers.
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
