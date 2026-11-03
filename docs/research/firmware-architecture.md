# Firmware architecture of v1.3.0-rc3

Research for [#2](https://github.com/gismya/deluge-generative/issues/2), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1).

**Question:** How is the v1.3.0-rc3 community firmware structured where Generative mode would plug in? This covers the view/UI stack, the Song / Clip / Output / Instrument model, automation storage and playback, playback tick scheduling, and the candidate seams for (a) feeding notes and parameter changes in live and (b) writing clips, notes, sounds and automation into a Song.

**Source:** the firmware source on the `generative` branch (base `v1.3.0-rc3`, commit `3ca05672`). Every path below is relative to the repo root. `docs/dev/*.md` are stubs that point to delugecommunity.com, so all of this comes from the code. Line numbers are for this commit.

This file records facts only. It does not design the feature.

---

## Summary

- **UI:** a new top-level mode is a `RootUI` subclass. It has a new `UIType` enum value and is installed with `changeRootUI()`. Entry and exit are hard-wired into other views' `buttonAction` handlers. `PerformanceView` is the closest recent precedent.
- **Sequencer:** all sequencing runs inside the audio task. It is event-driven: each subsystem reports "ticks until I next need attention", and the audio routine cuts its render window at the exact sample of the next tick.
- **Live seam:** the arpeggiator is an existing in-firmware note generator. It is driven by a per-Output virtual hook, `Output::doTickForwardForArp`, that Session and Arrangement call on every swung tick. Live notes reach instruments through `MelodicInstrument::sendNote`, `beginAuditioningForNote`, and `Kit::noteOnPreKitArp` / `beginAuditioningforDrum`.
- **Live params:** live parameter changes go through `AutoParam::setValuePossiblyForRegion` / `setCurrentValueInResponseToUserInput`. While record is on, these write automation as a side effect.
- **Song writing:** there is no "build a song" API. The building blocks exist: `Song` setup, `InstrumentClip` + `setAudioInstrument`, `StorageManager::createNewInstrument` / `loadInstrumentFromFile`, `NoteRow::attemptNoteAdd`, and `AutoParam::setValueForRegion` / `setNodeAtPos`. Many of them assume `currentSong`, the current root UI, or `display`. Writing song XML and loading it through the normal loader is a second route.

---

## 1. View / UI stack

### Base classes

- `class UI` is defined at `src/deluge/gui/ui/ui.h:92-181`.
  - Input handlers: `padAction(x, y, velocity)`, `buttonAction(Button, on, inCardRoutine)` (default `NOT_DEALT_WITH`), `horizontalEncoderAction`, `verticalEncoderAction`, `selectEncoderAction`, `modEncoderAction`, `modButtonAction`, `modEncoderButtonAction`.
  - Lifecycle and timing: `graphicsRoutine`, `timerCallback`, `opened` (calls `focusRegained`), `focusRegained`, `exitUI` (long BACK press), `close`.
  - Rendering: `renderMainPads` and `renderSidebar`. Returning false means "transparent", and both must accept a null image.
  - Pure virtual: `renderOLED(Canvas&)` (`ui.h:157`) and `getUIType()` (`ui.h:164`).
  - Mod-related virtuals default to forwarding to the global `view` (`src/deluge/gui/ui/ui.cpp:33-43`).
- `class RootUI : public UI` (`src/deluge/gui/ui/root_ui.h`) adds playback and clip notifications: `notifyPlaybackBegun`, `playbackEnded`, `clipNeedsReRendering`, `noteRowChanged`, and others. Its `canSeeViewUnderneath()` is final and returns true.
- Hierarchy:
  - `TimelineView : RootUI` (`gui/views/timeline_view.h:27`)
    - `ClipNavigationTimelineView : TimelineView` (`gui/views/clip_navigation_timeline_view.h:23`), parent of `SessionView`, `PerformanceView` and `ClipView`
    - `ArrangerView : TimelineView`
  - `KeyboardScreen : RootUI, InstrumentClipMinder` (`gui/ui/keyboard/keyboard_screen.h:35`) is a root view that is not a timeline view.
  - `SoundEditor`, `QwertyUI` and `ContextMenu` are plain `UI` overlays.
- View singletons are globals placed with `PLACE_SDRAM_BSS`, e.g. `gui/views/performance_view.cpp:179`.

### Navigation stack (`src/deluge/gui/ui/ui.cpp`)

- The stack is `static std::array<UI*, 16> uiNavigationHierarchy` with `numUIsOpen` (`ui.cpp:61-64`). Index 0 is the root view.
- `changeRootUI(UI*)` (`ui.cpp:107-121`):
  - replaces the whole stack with one root;
  - unsets the `UI_SPECIFIC` timer and reassesses greyout;
  - calls `opened()` (its return value is ignored) and requests an OLED redraw.
- `openUI(UI*)` pushes an overlay (`ui.cpp:233-253`). If `opened()` returns false, the overlay is popped again.
- `closeUI` / `UI::close()` pops the overlay and calls `focusRegained()` on the UI below (`ui.cpp:181-231`).
- Accessors: `getCurrentUI()` returns the top of the stack (`ui.cpp:140`). `getRootUI()` returns index 0 (`ui.cpp:149`) and can be null during a song swap.
- UI modes are a separate global, `currentUIMode`, holding exclusive values plus bitflags (`ui.h:33-89`). Helpers are `enterUIMode`, `exitUIMode` and `isUIModeActive` (`ui.cpp:411-474`).

### Adding a top-level view: what must be touched

- **`UIType`:** add a value to the alphabetical `enum class UIType` in `src/definitions_cxx.hpp:187-215`, before `UI_TYPE_COUNT`. A debug-only `uiTypeMap` is sized by `UI_TYPE_COUNT` (`gui/ui/ui.cpp:476-505`, `ENABLE_MATRIX_DEBUG`). Code that switches on `UIType`:
  - `gui/views/view.cpp:1573-1640` (mod LEDs)
  - `gui/ui/sound_editor.cpp:474-510` (greyout)
  - `io/midi/midi_follow.cpp:425-445`
  - `io/midi/midi_transpose.cpp:62-96`
  - `gui/ui_timer_manager.cpp:83`
  - `processing/stem_export/stem_export.cpp:1190`
  - `model/instrument/kit.cpp:1624`
- **Entry and exit:** there is no central mode registry. Each existing view decides in its own `buttonAction` where SONG, CLIP and KEYBOARD take you. The PerformanceView precedent:
  - Entry is the KEYBOARD button in Session (`gui/views/session_view.cpp:665-670`), Arranger (`gui/views/arranger_view.cpp:419-425`) and Automation-arranger (`gui/views/automation_view.cpp:1320-1322`).
  - Exit is KEYBOARD again, or releasing KEYBOARD after a hold, which acts as a "peek" (`gui/views/performance_view.cpp:904-969`).
  - Shift+KEYBOARD pushes the same object as an overlay for editing mode (`performance_view.cpp:928`). BACK closes it (`:838-846`).
- **Pointer checks for `rootUI == &performanceView`** appear in about 20 places: `view.cpp`, `session_view.cpp`, `arranger_view.cpp`, `playback/playback_handler.cpp:210,345`, `playback/mode/session.cpp:810,1064,1301,2417`, `sound_editor.cpp:1778,2080`, `midi_follow.cpp:1119-1131`, `mod_controllable_audio.cpp:1224-1232` and `global_effectable.cpp:103`. A new top-level view that co-exists with Session playback may need similar special cases.
- **Song load:** `setUIForLoadedSong` (`src/deluge/deluge.cpp:342-380`) only ever picks the automation, keyboard, instrument clip, audio clip, arranger or session view.
- **Build:** new `.cpp` files under `src/deluge` are picked up by `file(GLOB_RECURSE ...)` (`src/deluge/CMakeLists.txt:8`).
- **Optional feature flag:** `RuntimeFeatureSettings` (`model/settings/runtime_feature_settings.h:53-131`) is persisted to `SETTINGS/CommunityFeatures.XML`. The recipe for adding one is in `docs/CONTRIBUTING.md:91-114`. `OledOnlySettingToggle` exists for OLED-only toggles (`gui/menu_item/runtime_feature/settings.cpp:32-36`).

### Input dispatch

- **Pads:** `readButtonsAndPads()` (`src/deluge/deluge.cpp:249-336`) calls `matrixDriver.padAction`, which calls `getCurrentUI()->padAction(x, y, velocity)` (`hid/matrix/matrix_driver.cpp:69-88`). The grid is 16×8 main pads plus 2 sidebar columns (x ≥ 16).
- **Buttons:** `Buttons::buttonAction` (`hid/buttons.cpp:75-243`) handles them in this order:
  1. The 8 mod buttons go to `getCurrentUI()->modButtonAction`.
  2. Every button is offered to `getCurrentUI()->buttonAction`.
  3. Only if that returns `NOT_DEALT_WITH` does the global handling run: PLAY, SHIFT, RECORD, tempo encoder press, and mod encoder presses.
- **Encoders:** `interpretEncoders` (`hid/encoder_input.cpp:52-200`):
  - X and Y scroll go to the current UI.
  - The tempo encoder goes to `playbackHandler.tempoEncoderAction` and is **not** routed to the UI.
  - The select encoder goes to `selectEncoderAction`.
  - The two gold knobs go to `getCurrentUI()->modEncoderAction(0|1, offset)`.

### Gold knobs

- The default path is `View::modEncoderAction` (`gui/views/view.cpp:817-840`). It edits the param mapped to the current mod-button "knob mode" on `view.activeModControllableModelStack`.
- The target is set with `view.setActiveModControllableTimelineCounter(...)` (`view.cpp:1889-1920`) or `setActiveModControllableWithoutTimelineCounter(...)` (`view.cpp:1922-1943`).
- A view takes the knobs over completely by overriding `modEncoderAction`, `modButtonAction` and `modEncoderButtonAction` and not calling the base. It then sets the knob indicator LEDs itself with `indicator_leds::setKnobIndicatorLevel` (`hid/led/indicator_leds.h:81-88`).

### Rendering

- **OLED:**
  - `renderUIsForOled()` sets a dirty flag.
  - `doAnyPendingOLEDRendering()` (`gui/ui/ui.cpp:371-391`) clears `OLED::main`, then calls `renderOLED(canvas)` up the stack and sends the image.
  - The canvas API is in `hid/display/oled_canvas/canvas.h:52-296`: lines, rectangles, `drawString`, `drawStringCentred`, `drawScreenTitle`, `invertArea`, icons.
  - The screen is 128×48 (`src/RZA1/cpu_specific.h:137-149`).
  - Popups: `display->displayPopup` / `popupText` (`hid/display/display.h:58-90`).
- **Pads:**
  - `uiNeedsRendering(ui, rows...)` queues rows (`ui.cpp:293-317`).
  - `doAnyPendingGridRendering()` (`ui.cpp:319-369`) calls `renderMainPads` / `renderSidebar` down the stack until a UI returns true (opaque). Views fill `PadLEDs::image[8][18]` and `occupancyMask` (`hid/led/pad_leds.h:36-37`).
  - A minimal example is `KeyboardScreen::renderMainPads` (`gui/ui/keyboard/keyboard_screen.cpp:808-828`).
- **Timers:**
  - `uiTimerManager.setTimer(TimerName::UI_SPECIFIC, ms)` calls back `getCurrentUI()->timerCallback()` (`gui/ui_timer_manager.cpp:155-161`). Any stack change unsets it.
  - `graphicsRoutine()` runs every 15 ms (`ui_timer_manager.cpp:226-231`).

---

## 2. Song / Clip / Output / Instrument model

### Song (`src/deluge/model/song/song.h`, `song.cpp`)

- **Globals:** `Song* currentSong` and `Song* preLoadedSong` (`src/deluge/deluge.cpp:94-95`).
- **Clips:** `ClipArray sessionClips` and `ClipArray arrangementOnlyClips` (`song.h:190-191`). These are pointer arrays; insert with `insertClipAtIndex`.
- **Outputs:** a singly linked list. `Output* firstOutput` (`song.h:193`) chains through `Output::next` (`model/output.h:94`). `firstHibernatingInstrument` (`song.h:194`) holds unused instruments that are kept.
- **Song-level params:** in `TimelineCounter::paramManager` (`model/timeline_counter.h:51`) plus `GlobalEffectableForSong globalEffectable` (`song.h:188`).
- **Key and scale:** `MusicalKey key` (`song.h:225`) holds `modeNotes` + `rootNote`. Setters are `setRootNote` (`song.cpp:586`), `setScale` (`:3032`) and `setScaleNotes` (`:3045`).
- **Swing:** `swingAmount` (clamped to -49..49) and `swingInterval` (`song.h:220-221`).
- **Sections:** `sections[kMaxNumSections=24]` (`song.h:223`, `src/definitions_cxx.hpp:450`).
- **Tempo:** stored as samples per timer tick, `uint64_t timePerTimerTickBig` in 32.32 fixed point (`song.h:206`).
  - `setBPM(float, bool)` (`song.cpp:3112`) also writes the `UNPATCHED_TEMPO` song automation param, and it reads `currentUIMode` and `arrangerView`.
  - `setBPMInner` (`song.cpp:3151`) computes `110250 / BPM` adjusted by magnitude and calls `setTempoFromNumSamples`.
- **Blank song:** `setupBlankSong()` (`src/deluge/deluge.cpp:381-405`) runs these steps:
  1. `new Song()`
  2. `paramManager.setupUnpatched()`
  3. `GlobalEffectable::initParams`
  4. `setupDefault()` (`song.cpp:255-291`), which picks BPM, key and scale from the user defaults, possibly randomized
  5. `ensureAtLeastOneSessionClip()` (`song.cpp:380-471`)
  6. `currentSong = preLoadedSong`
  7. `setUIForLoadedSong`

  "Clear song" (`gui/context_menu/clear_song.cpp:72-115`) does the same and then calls `playbackHandler.doSongSwap()`.

### Clips

- **`Clip`** (`model/clip/clip.h:46`) is a `TimelineCounter`. Fields:
  - `output`, `type` (`INSTRUMENT` / `AUDIO`), `section`, `armState`
  - `activeIfNoSolo`, which means "playing" in session
  - `loopLength`, `launchStyle` (`DEFAULT` / `FILL` / `ONCE`), `name`

  The constructor does **not** initialize `loopLength` (`clip.cpp:43-75`).
- **`InstrumentClip`** (`model/clip/instrument_clip.h:48`) holds `NoteRowVector noteRows`, `inScaleMode` and `arpSettings`.
  - `setAudioInstrument(Instrument*, Song*, bool setupPatching, ParamManager*, ...)` (`instrument_clip.cpp:2298`) attaches a Synth or Kit. `setNonAudioInstrument` (`:1612`) attaches MIDI or CV.
  - `Clip::solicitParamManager` trims automation to `loopLength`, so `loopLength` must be set first. `ensureAtLeastOneSessionClip` does it in that order (`song.cpp:391` then `:454`).
  - For kits, `setupAsNewKitClipIfNecessary` (`instrument_clip.cpp:4356`) calls `assignDrumsToNoteRows` to create one NoteRow per Drum.
- **`AudioClip`** (`model/clip/audio_clip.h:35`) is for recorded audio. It is not relevant to generation.
- **No `Song::addClip` exists.** Clip creation lives in UI code, and every one of these helpers uses `currentSong`, `display` and `actionLogger.deleteAllLogs()`:
  - `SessionView::createNewClip` / `createNewInstrumentClip` (`gui/views/session_view.cpp:1672, 1731`)
  - `createNewTrackForInstrumentClip` (`:3586`)
  - `setupNewClip` (`:3674`)
  - `insertAndResyncNewClip` (`:1764`)
  - `ArrangerView::createNewClipForClipInstance` (`gui/views/arranger_view.cpp:1653-1721`) for arrangement-only clips

### Outputs and instruments

- **`Output`** (`model/output.h:81`) has `type` (`SYNTH`, `KIT`, `MIDI_OUT`, `CV`, `AUDIO`), `name`, `next`, `activeClip` and arrangement `clipInstances`.
- **Instrument classes:**
  - `Instrument` (`model/instrument/instrument.h:45`) has a `dirPath` field that must be set to "SYNTHS" or "KITS".
  - `MelodicInstrument` (`melodic_instrument.h:30`)
  - `SoundInstrument : Sound, MelodicInstrument` is the Synth (`processing/sound/sound_instrument.h:30`).
  - `Kit : Instrument, GlobalEffectableForClip` (`model/instrument/kit.h:34`) holds a `Drum* firstDrum` linked list.
  - `NonAudioInstrument` is the base of `MIDIInstrument` and `CVInstrument`.
- **Drums:** `Drum` (`model/drum/drum.h:44`). `SoundDrum : Sound, Drum` (`processing/sound/sound_drum.h:28`) is a full synth voice that can be sample-based.
- **Creating instruments:**
  - `StorageManager::createNewInstrument(OutputType, ParamManager*)` (`storage/storage_manager.cpp:601-659`) allocates a Synth or Kit and initializes its ParamManager. It does not set `dirPath`.
  - Default sounds: `Sound::setupAsDefaultSynth` (`processing/sound/sound.cpp:224`), `setupAsBlankSynth` (`:298`) and `setupAsSample` (`:189`).
  - `Song::addOutput(Output*, bool atStart)` (`song.cpp:3307`) links the output into the song.
  - The UI path for "new blank synth/kit" is `InstrumentClipMinder::createNewInstrument` (`model/clip/instrument_clip_minder.cpp:155-286`).
- **Presets from SD:**
  - `StorageManager::loadInstrumentFromFile(Song*, InstrumentClip*, OutputType, bool mayReadSamples, Instrument**, FilePointer*, String* name, String* dirPath)` (`storage_manager.cpp:326-420`) reads a synth or kit XML preset. At `:406` it checks `currentSong`, not the `song` argument.
  - `loadSynthToDrum` (`:559`) loads a synth preset into a kit drum.
  - `ensureAtLeastOneSessionClip` shows the "next unused SYNTHS preset, else default synth" fallback (`song.cpp:405-448`).
- **Kit drum with an SD sample:**
  - Build the drum (`gui/views/instrument_clip_view.cpp:5653-5730`):
    1. Allocate a `SoundDrum`.
    2. Call `setupWithPatching` + `initParams` and `setupAsSample`.
    3. Call `song->backUpParamManager`.
    4. Call `kit->addDrum` (`kit.cpp:423`).
    5. Call `noteRow->setDrum`.
  - Set the sample on `source->getOrCreateFirstRange()->getAudioFileHolder()`: set `filePath`, then call `loadFile` (`gui/ui/browser/sample_browser.cpp:1985-2061`, `storage/audio/audio_file_holder.h:30-47`).
  - `Sound` has `kNumSources = 2` sources.

### Notes

- **`Note`** (`model/note/note.h:24`) has `pos`, `length`, `velocity`, `probability` (0..20, where 20 = 100%), `iterance`, `fill` and `lift`. The default constructor leaves the fields uninitialized.
- **`NoteRow`** (`model/note/note_row.h:99`) has:
  - `y` (the MIDI note code for melodic clips)
  - `Drum* drum` (kits)
  - `NoteVector notes`, ordered by `pos`
  - its own `ParamManagerForTimeline paramManager`: full Sound params for kit SoundDrums, expression (MPE) params only for melodic rows (`note_row.cpp:3540, 4410`)
  - per-row `probabilityValue`, `iteranceValue` and `fillValue`, an independent loop length, and `muted`
- **NoteRow ids:** for kits the id is the row index, otherwise it is `y` (`instrument_clip.cpp:3428`).
- **Adding notes:**
  - `InstrumentClip::getOrCreateNoteRowForYNote(yNote, ModelStackWithTimelineCounter*, Action*, bool* scaleAltered)` (`instrument_clip.cpp:1080-1124`). If the note is out of scale and the clip is in scale mode, this **can change the song's scale** through `setRootNote`.
  - `NoteRow::attemptNoteAdd(pos, length, velocity, probability, Iterance, fill, ModelStackWithNoteRow*, Action*)` (`note_row.cpp:722-791`). It returns 0 on overlap or allocation failure, clamps `length` to the next note, sets lift to 64 and calls `expectEvent()`. `Action` may be null.
  - Bulk pattern: build a `NoteVector` and call `notes.swapStateWith(&newNotes)`. This is how Euclidean editing does it (`instrument_clip_view.cpp:7732+`, `note_row.cpp:684-697`).
- **Undo:** `actionLogger.getNewAction(...)` returns null when there is no `currentSong`, when the current UI is not the root UI, and in other cases (`model/action/action_logger.cpp:88-161`). Passing `nullptr` for the action is supported throughout.

### ModelStack

`src/deluge/model/model_stack.h:44-121` explains the design. It is a stack-allocated context chain:

`ModelStack{song}` → `+TimelineCounter{clip}` → `+NoteRow` → `+ModControllable` → `+ThreeMainThings{paramManager}` → `+ParamCollection` → `+ParamId` → `+AutoParam`

Lower-level code reads the song, the clip and the loop length from it instead of following parent pointers. Usage is `char mem[MODEL_STACK_MAX_SIZE]; setupModelStackWithSong(mem, song)` / `setupModelStackWithTimelineCounter(...)` (`model_stack.h:290-301`). Every edit API above takes one.

### Serialization

- **Formats:** `XMLSerializer` / `XMLDeserializer` and `JsonSerializer` / `JsonDeserializer` (`storage/storage_manager.h:119-305`).
- **Writing:** `Song::writeToFile()` (`song.cpp:1147`) reads UI state for the pad preview and the root UI. It writes, in order: song attributes, `instruments`, `sections`, `sessionClips`, `arrangementOnlyTracks`. NoteRows write `noteDataWithSplitProb`, a hex blob of pos, length, velocity, lift, probability, iterance and fill (`note_row.cpp:3589-3619`). `Song::writeTemplateSong(path)` (`song.cpp:1135`) is a self-contained writer.
- **Loading:** `gui/ui/load/load_song_ui.cpp:356-541` runs `new Song` → `readFromFile` (`song.cpp:1415`) → `loadAllSamples` → `doSongSwap`.

---

## 3. Automation (AutoParam)

- **Storage:** an `AutoParam` (`modulation/automation/auto_param.h:43-166`) holds:
  - a lazily allocated `ParamNodeVector`, where each node is `{pos, int32_t value, bool interpolated}` (`modulation/params/param_node.h:23-43`)
  - `currentValue`
  - interpolation state
  - an override timestamp

  `isAutomated()` is true when the param has any nodes.
- **Value range:** values use the full int32 range. The knob mapping is `knobPos(-64..64) << 25` (`modulation/params/param_collection.cpp:55-68`). Patch cables use ±2^30 (`modulation/patch/patch_cable.h:72`).
- **Containers:**
  - A `ParamSet` is a fixed array of AutoParams:
    - `UnpatchedParamSet` (Kind `UNPATCHED_SOUND` / `UNPATCHED_GLOBAL`)
    - `PatchedParamSet` (Kind `PATCHED`)
    - `ExpressionParamSet` (3 MPE dimensions)

    These are in `modulation/params/param_set.h:36-158`.
  - `PatchCableSet` (`modulation/patch/patch_cable_set.h:41-122`).
  - A `ParamManager` holds up to 5 collection summaries, with bitfields of which params are automated or interpolating (`param_manager.h:45-153`, `param_collection_summary.h:24-60`). The layout differs by owner:
    - Sound: Unpatched, Patched, PatchCables, Expression (`param_manager.cpp:82-107`)
    - Song, Kit and audio clip: Unpatched, Expression
    - MIDI: MIDIParamCollection, Expression
- **Who owns params:**
  - each `Clip` (and the Song) through `TimelineCounter::paramManager`, as a `ParamManagerForTimeline`
  - each `NoteRow` (per-drum params for kits, MPE for melodic rows)
- **Param IDs** are in `modulation/params/param.h`:
  - `Kind` (`:42-59`)
  - patched `LOCAL_*` / `GLOBAL_*` (`:68-158`)
  - unpatched shared, sound and global, including arp params and `UNPATCHED_TEMPO` (`:177-237`)
  - patch cables at 190 (`:250`)
- **Writing automation:**
  - `setNodeAtPos(pos, value, interpolate)` (`auto_param.cpp:1070-1095`) is a raw insert. It sends no notification, updates no summary bit and creates no undo record. The caller must flag the param as automated, as `InstrumentClip::recordNoteOn` does (`instrument_clip.cpp:4651-4665`).
  - `setValueForRegion(pos, length, value, ModelStackWithAutoParam*, ActionType)` (`auto_param.cpp:1097-1182`):
    - creates an undo action;
    - homogenizes the region to the value;
    - takes interpolation flags from `automationView` only when that view is the root UI, otherwise both are false;
    - notifies, which sets the automated bit.

    A full-loop region deletes the automation and sets a constant value.
  - `setValuePossiblyForRegion(value, ms, pos, length)` (`auto_param.cpp:997-1005`) calls `setValueForRegion` when `length > 0`. Otherwise it behaves like a live user move.
  - The automation view writes steps this way (`gui/views/automation/editor_layout/mod_controllable.cpp:972-992`).
- **Getting an AutoParam:** use `Output::getModelStackWithParam(ms, clip, paramID, Kind, affectEntire, useMenuStack)` (`model/output.h:197`) or `Song::getModelStackWithParam` (`song.h:387`).
- **Playback:**
  - `AutoParam::processCurrentPos` (`auto_param.cpp:402-774`) returns ticks until the next node. On reaching a node it jumps to the node's value, or ramps toward it if the node is interpolated.
  - `tickSamples` advances the ramp during render for internal synths (`auto_param.cpp:948-972`). MIDI and CV use `tickTicks`.
  - `ParamManagerForTimeline::processCurrentPos` (`param_manager.cpp:374-412`) only visits automated params.
  - `Clip::processCurrentPos` folds the next automation event into `playbackHandler.swungTicksTilNextEvent` (`model/clip/clip.cpp:303-322`).
- **Live override:** while playing and not recording, a user move on an automated param overrides the automation for about 200 ms (`auto_param.cpp:46-53, 283-288`).

---

## 4. Playback and tick scheduling

- **Resolution:** a quarter note is `24 << inputTickMagnitude` ticks, a bar `96 << …` and a 16th `6 << …` (`song.cpp:5768-5778`). The magnitude defaults to `FlashStorage::defaultMagnitude = 2` (`storage/flash_storage.cpp:350`), which gives **96 ticks per quarter note**. It is stored per song and can change, for example under sync scaling.
- **Tempo math:** `timePerTimerTick = 110250 / (BPM · 2^mag)` samples, with `kSampleRate = 44100` (`song.cpp:3151-3160`). At 120 BPM that is about 229.7 samples per tick.
- **Execution context:** sequencing runs inside the audio task.
  - `AudioEngine::routine_()` (`processing/engines/audio_engine.cpp:735-813`) renders at most 128 samples (about 2.9 ms) per window.
  - Before rendering, `tickSongFinalizeWindows` (`audio_engine.cpp:926-1008`) actions every timer tick or swung tick that is due through `playbackHandler.actionTimerTick()` / `actionSwungTick()`.
  - It then **shortens the window so it ends exactly at the next tick** (`:967-970`). Ticks are therefore sample-accurate and synchronous with render.
- **Event-driven ticks:** `actionSwungTick()` (`playback/playback_handler.cpp:968-1080`) only runs when something is due. Each subsystem lowers `playbackHandler.swungTicksTilNextEvent`. `PlaybackHandler::expectEvent()` (`:3116-3124`) and `Clip::expectEvent()` pull the next tick to "now + 1".
- **Swing** is applied in `scheduleSwungTickFromInternalClock()` (`playback_handler.cpp:1092-1133`).
- **Per-tick flow (Session):**
  1. `Session::considerLaunchEvent` (`playback/mode/session.cpp:2307-2443`) calls `clip->incrementPos` on active clips, then handles launches, fills and song swaps.
  2. `Session::doTickForward` (`:2445-2561`) calls `clip->processCurrentPos` for each active clip, early-playback outputs first.
  3. It then calls **`output->doTickForwardForArp(modelStack, pos)` for every Output in the song**, taking the minimum of the returned "ticks until next" values (`:2538-2552`).
  4. `Arrangement::doTickForward` does the equivalent for arrangement mode (`playback/mode/arrangement.cpp:111-345`).
- **How sequenced notes sound:**
  - `InstrumentClip::processCurrentPos` (`model/clip/instrument_clip.cpp:696-913`) calls `NoteRow::processCurrentPos` (`note_row.cpp:2032`) for each row.
  - Note-offs are sent immediately. Note-ons go into a `PendingNoteOnList` (at most `kMaxNumNoteOnsPending = 64`), which is then filtered by probability, iterance and fill.
  - `sendPendingNoteOn` (`:915-939`) then calls:
    - `MelodicInstrument::sendNote(ms3, true, noteCode, mpeValues, ch, velocity, sampleSyncLength, ticksLate)` (`model/instrument/melodic_instrument.h:36-38`) for melodic clips
    - `Kit::noteOnPreKitArp(ms3, drum, velocity, ...)` (`kit.h:166-169`) for kits
  - `SoundInstrument::sendNote` calls `Sound::noteOn`, which goes through the arpeggiator and then `noteOnPostArpeggiator` (`processing/sound/sound_instrument.cpp:372-387`, `sound.cpp:1509-1604`).
- **Arpeggiator precedent:**
  - `ArpeggiatorBase::doTickForward(settings, instr, clipCurrentPos, reversed)` (`modulation/arpeggiator.cpp:1555-1595`) fires on `clipCurrentPos % ticksPerPeriod == 0` and returns ticks until the next period.
  - Its output is an `ArpReturnInstruction` (`arpeggiator.h:192-220`).
  - `SoundInstrument::doTickForwardForArp` (`sound_instrument.cpp:414-440`) applies the result with `noteOnPostArpeggiator` / `noteOffPostArpeggiator`. Gate-offs and unsynced timing run sample-driven in `render()` (`arpeggiator.cpp:1485-1551`).
  - The hook is a virtual on Output: `virtual int32_t doTickForwardForArp(ModelStack*, int32_t currentPos)`, with a default that returns `INT32_MAX` (`model/output.h:151`).
- **Song swap:** `doSongSwap` (`playback_handler.cpp:1442-1552`) kills all voices and stops notes, and resets tick counters unless it is preserving position.

---

## 5. Candidate seams

### (a) Feeding notes and parameter changes in live

These are candidates only. Choosing between them is a design decision.

| Seam | Where | Notes |
|---|---|---|
| Per-tick hook on every Output | `Output::doTickForwardForArp` (`model/output.h:151`), called from `Session::doTickForward` (`session.cpp:2538-2552`) and `Arrangement::doTickForward` | The arpeggiator already uses it as a tick-synced note generator inside the audio task. Its return value schedules the next wake-up. It only runs while playback is running. |
| Playback-mode tick | `Session::doTickForward` / `considerLaunchEvent` | Global, once per swung tick. This is where clip launches are decided. |
| Instrument note entry (sequencer-style) | `MelodicInstrument::sendNote(ModelStackWithThreeMainThings*, isOn, noteCode, mpeValues, ...)`; `Kit::noteOnPreKitArp` / `noteOffPreKitArp` | This is what the sequencer calls. It needs a ModelStack with the instrument's ParamManager. Note-on needs non-null `mpeValues`. |
| Instrument note entry (audition-style) | `MelodicInstrument::beginAuditioningForNote` / `endAuditioningForNote` (`melodic_instrument.cpp:528-593`); `Kit::beginAuditioningforDrum` | Used by pads and live MIDI. Requires `activeClip`. Tracks `notesAuditioned`. |
| Live MIDI path | `PlaybackHandler::noteMessageReceived` → `Output::offerReceivedNote` → `MelodicInstrument::receivedNote` (`melodic_instrument.cpp:95-284`) | Records into the clip when recording is on. |
| Live param change | `AutoParam::setValuePossiblyForRegion(value, ms, 0, 0)` → `setCurrentValueInResponseToUserInput` (`auto_param.cpp:101-304`) | This is the knob, MIDI CC and MIDI follow path (`view.cpp:919-922`, `mod_controllable_audio.cpp:1127`). **It writes automation when recording is on and the clip is armed.** It overrides existing automation for about 200 ms when not recording. |
| Direct value set | `autoParam->currentValue = v` plus a notify | Used inside `recordNoteOn` for MPE. It bypasses recording logic. |

Constraints on any live seam:
- The tick and sequencer path runs in the audio task, so heavy work there delays audio.
- `AudioEngine::allowedToStartVoice()` allows 4 new voices per render, or 1 when the CPU is under pressure (`audio_engine.cpp:1622-1629`). Surplus note-ons stay pending in the arpeggiator.
- The sequencer comments that 8-10 note-ons per render cause culling (`note_row.cpp:2562-2563`).
- Voices are culled when CPU "direness" rises (`audio_engine.cpp:516-560`).

### (b) Writing clips, notes, sounds and automation into a Song

- **In-memory route**, using existing calls in roughly this order:
  1. Song: `new Song` → `setupUnpatched` → `initParams` → `setupDefault` / `setBPM` / `setScale` / `setRootNote`.
  2. Instruments: `createNewInstrument` + `setupAsDefaultSynth`, or `loadInstrumentFromFile`; set `dirPath`; `Song::addOutput`.
  3. Clip: allocate an `InstrumentClip(song)`, set `loopLength`, call `setAudioInstrument`, call `setupAsNewKitClipIfNecessary` for kits, then `sessionClips.insertClipAtIndex`.
  4. Notes: `getOrCreateNoteRowForYNote` / `attemptNoteAdd`, or the bulk `swapStateWith`.
  5. Automation: `getModelStackWithParam`, then `setValueForRegion` / `setNodeAtPos`.
  6. Kit samples: `filePath` + `loadFile`.
  7. Song swap: `doSongSwap` (or `setUIForLoadedSong` for a fresh song).
- **Hazards on the in-memory route:**
  - Many helpers read `currentSong` rather than a passed Song: `loadInstrumentFromFile:406`, `getNewAction`, the SessionView helpers, and `Song::addOutput` (which resyncs LFOs only for `currentSong`).
  - `setBPM` and `writeToFile` read UI state.
  - `getOrCreateNoteRowForYNote` can rewrite the song scale.
  - Some allocation failures freeze instead of returning an error: `while(1)` in `ensureAtLeastOneSessionClip` (`song.cpp:431-445`) and `FREEZE_WITH_ERROR` in `NoteRow::setDrum`.
  - There is no fixed cap on clips or notes. RAM is the only limit (`memory/general_memory_allocator.h`).
- **File route:** emit song XML in the `Song::writeToFile` format and load it through `load_song_ui.cpp:356-541`. This decouples the Generator from firmware internals but goes through the SD card. The minimum required set of XML attributes was not checked.

---

## Open points (not verified here)

- Whether a non-current Song (`preLoadedSong`) can be fully built with these APIs, given how many of them read `currentSong`.
- Whether `doTickForwardForArp` or the Session tick runs when transport is stopped. From the code it only runs while a clock is active. A Perform mode that plays "by itself" would need playback started.
- Where swing is applied under external clock (only `getActualSwungTickCount`, `playback_handler.cpp:1217-1256`, was located).
- `MIDIParamCollection` internals (CC automation for MIDI clips), which matter for the later MIDI/CV output option.
- The gold knob 0/1 to lower/upper mapping is taken from a code comment (`hid/encoder_input.cpp:169`), not checked on hardware.
