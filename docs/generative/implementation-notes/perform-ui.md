# Implementation notes: Perform UI (`gui/views/perform_view.*`)

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- `PerformView` (`gui/views/perform_view.h`) is one class whose code is split by area: `perform_view.cpp` (entry and
  focus regained, buttons, menus and LEDs, the pad dispatch, `graphicsRoutine()`, playback and the pad renders),
  `perform_view_knobs.cpp` (focus: holding and locking a part; the gold knobs, F8's macros, Thin / Drive and their
  rings; letting go of held pads before an overlay), `perform_view_clip.cpp` (the CLIP page and its faders, SOUND
  EDITOR and the sound editor over Perform, NEW SOUND and the preset browser, FREEZE and takes),
  `perform_view_keyboard.cpp` (the KEYBOARD page), `perform_view_song.cpp` (the SONG page's Arc map and the sidebar,
  End included) and `perform_view_screen.cpp` (the OLED, the tempo encoder, TAP and swing). What more than one of them
  uses is in `perform_view_internal.h`, which also holds the view's includes; a constant one area uses stays in its
  file.
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
- Trap (#454): nothing that runs in `graphicsRoutine()` may take more than about 1 ms. The scheduler can't preempt a
  task, and the audio TX ring holds 2.9 ms, of which 2.1-2.4 ms is left when the frame starts. The whole Arc plan
  (`Generator::planArc()`, 2.4-6 ms) ran there after every macro turn: one underrun a turn in Psytrance and Detroit.
  The SONG page now plans `PerformView::kArcPlanStepsPerFrame` steps a frame (`ArcPlanner`, in `generator.h`), and
  `ArcMapState` shows the last plan until the new one is done. A change while one is under way lets it finish and
  then plans again, so turning faster than a plan takes still moves the map. A new roster or a stop empties the plan
  at once. `ArcPlanner::begin()` copies everything the loop reads (macros, biases, the landing steps), so the piece
  can step on or be freed between slices. Read a new per-frame cost with `perform_audio_margin.txt` (`gen_audio`,
  `gen_audio_ui_timer`).
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
  across openings, so probe scripts choose a row from the top (`encoder 3 -20`, as `compose_screen.txt` does: `-5`
  stopped short of the Perform menu's top once a script had left it on RESET PART BIAS, its 7th row, #303). The sound
  editor's `setLedStates()` and its SELECT/BACK handling run `View::setModLedStates()`, which sets F7 and F8 steady,
  SCALE off and the page LEDs the stock way: `PerformView::assertLeds()` puts them back every frame (Perform's
  `graphicsRoutine()` runs under the sound editor, which can see the view underneath). A menu value held while it's open
  (the edited key, a take row) records what it was read under (`Basis` in `perform_menu.cpp`: playing, pieces started,
  and the key presses or that part's take presses, BACK taps counting as both) and is read again when that moves, or
  when its submenu opens afresh; `refreshPerformMenu()` redraws the open item then.
- The Generative menu (§14.1): GENERATIVE is a context menu (`gui/context_menu/generative.*`); PERFORM FROM, COMPOSE
  FROM and the Compose screen's FROM are `SourceMenu`s (`gui/menu_item/generative/source_rows.h`) of CURRENT RECIPE
  (hidden until a Recipe exists), GENRE ▸ and FILE.... PERFORM FROM's rows all end in `DiscardSong::setPerform()` with
  an `EntrySource`; FILE... gets there through the load browser's `loadChosen()` (`performFromShowing()`). A list
  remembers its row (stock), so probe scripts reach GENRE from the bottom: `encoder 3 5`, `encoder 3 -1`
  (`perform_pick_acid.txt`), which holds with or without CURRENT RECIPE. `gen_from_menu perform|compose|screen
  current=0|1` comes each time a list shows afresh (a UI over it closing too).
- GENRE ▸ is built three times, in PERFORM FROM (`perform_from.cpp`), the Compose lists (`compose_screen.cpp`) and the
  Perform menu (`perform_menu.cpp`), from the same rows and items (`gui/menu_item/generative/genre_rows.h`:
  `GenreMenuItems<Item>`, each menu with its own genre item; laid out by the portable `perform/genre_menu.h`). Stock
  `Submenu`s have fixed item lists and a row can't be a leaf one time and a submenu the next (`selectButtonPress()` is
  final in `Submenu`), so each row slot holds both a genre item and a `GenreGroup`, and `isRelevant()` shows the one
  that fits. All groups share one list of genre items, which read the open parent (set in `GenreGroup::beginSession()`).
  All three open on the Current Recipe's genre (`GenreMenu::beginSession()`), so a probe script that picks a genre
  counts rows from there, not from the top.
- The Regenerate page (§13.6, #494) is a stock `Submenu` (`gui/menu_item/generative/regenerate_page.*`) over the
  Perform menu, entered with `soundEditor.enterSubmenu()`: a leaf's `selectButtonPress()` return value is ignored by
  the sound editor (only a submenu's current item is entered), so a genre item can't navigate by returning the page.
  Its state is portable (`perform/regenerate_page.h`, harness-tested); the firmware items read it. The VARIANTS set
  rows and their toggles are shared by the page and the Compose screen's VARIANTS ▸ through a `VariantsTarget` the
  open VARIANTS menu sets in `beginSession()`, as GENRE ▸'s groups share their genre items. A mid-set LOAD opens it with
  `soundEditor.setup(nullptr, &page)` once the browser has closed. `beginSession(nullptr)` also runs when the sound
  editor regains focus (a popup closing): only a fresh open (`freshOpen`) jumps to GO.
- FILL has several holders: SYNC-SCALING and the KEYBOARD page's 12 FILL pads (`perform::FillHolds`). Under the sound
  editor and Perform's menus, SYNC-SCALING reaches Perform only through F7 (`SoundEditor::buttonAction()`), ahead of
  the community setting's fill toggle; in SHIFT+SELECT's settings menu it stays stock.
  `PerformView::holdFill()` moves the song's fill mode only on the first down and the last up, latching SHIFT on the
  first; `PerformSteering::sampleFill()` sends the song's fill mode to the core each tick, so MIDI FILL works too.
- A KEYBOARD fader press steers the core at once (`PerformSteering::holdMacro()`, the Current Recipe untouched). The
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
  editor: BACK hold calls the current UI's `exitUI()`. Under the sample browser's preview or QWERTY keyboard,
  Perform's `graphicsRoutine()` doesn't run, so swaps, transitions and retirements wait for it to close (`pump.h`).
  Don't set the current Clip anywhere else: MIDI follow, the knobs and the popups use the Clip on the gold knobs.
- The F page (stock's `modKnobMode`) lives on each Output, but Thin / Drive (`thinDrivePart_`) is on a part. A sound
  swap's new Output comes up on its own page, so `bindKnobs()` sets the page from `perform::knobPage()`
  (`generative/perform/knob_page.h`) each time it binds a part: F7 while Thin / Drive is on that part. Anything that
  replaces a part's Output must end in `bindKnobs()` (the swap reaction and `releaseOutput()` both do).
- Every pad press under the sound editor and Perform's menus reaches `PerformView::padAction()` (F7):
  `shiftOpensSoundEditor()` (`generative/perform/grid_page.h`) picks the SHIFT + main pad shortcuts, leaving out the
  KEYBOARD FILL pads and the CLIP page's NEW SOUND and SOUND EDITOR, and `shiftEditorPart()` the target (the song FX on
  stock Performance view, whatever is focused); a Perform menu (`editorOpen_` false) closes on a
  press. A SHIFT + pad that doesn't move an open editor (a param the target lacks) puts the current Clip back.
- With no current Clip, `getCurrentOutputType()` is NONE and the other current-Clip getters return null (`song.cpp`),
  so song-level editor paths never read null. Test builds fault on any null access: run the probe scripts after
  touching what the editor reaches.
- The sound browser (`gui/ui/browser/perform_sound_browser.*`, §13.5: the preset browser and the lane browser) is its own `SlotBrowser`, not stock's
  `LoadInstrumentPresetUI`, whose `currentFileChanged()` loads and swaps the instrument on every scroll step. Its
  `outputTypeToLoad` is NONE, as the sample and DX browsers' are: stock otherwise adds the current song's Instruments
  to the list, and in Perform those are the parts (the default synth among them, `SYNTHS/0`, which isn't on the card).
  A QWERTY UI covers the main pads and can't see the view underneath, so `UI::graphicsRoutine()` wouldn't run
  Perform's frame: the browser's `graphicsRoutine()` calls `performView.graphicsRoutine()` itself, which keeps the
  pump running (`pump.h`), and hands sidebar presses and SYNC-SCALING to Perform. End still can't arm under it:
  End's pad acts only while Perform is on top (`endPadActs()`), as under every overlay (§13.5). Its choice goes to
  `PerformView::chooseSound()` after it has closed, so Perform is on top again when the press's popup shows.
  SYNTH's sound rule (a synth part: `PartSpec::synth()`) is the one `buildPartOutput()` uses.
- The lane list (`gui/menu_item/generative/perform_lanes.*`) is a Perform menu opened by `openMenu()`, and a lane's
  browser opens over it with `openUI()` (as stock's sample browser opens over the sound editor), so the browser's
  `close()` lands back on the list. A choice closes the list too (`soundEditor.exitCompletely()`) before
  `chooseSound()`, so Perform is on top for the press's popup. Where a browser opens is `perform::browserStart()`
  (`generative/perform/sound_browser.h`): the pick's folder, the palette's, then SYNTHS or SAMPLES.
- Trap: a leaf row in the sound editor (`shouldEnterSubmenu()` false) has its `selectButtonPress()` called, and what
  it returns is dropped (`SoundEditor::buttonAction()`). A row that leads to a submenu enters it itself with
  `soundEditor.enterSubmenu()` (the lane list's FX rows, `perform_lanes.cpp`).
- Trap: `Browser::arrivedInNewFolder()`'s `defaultDirToAlsoTry` is **created** (`f_mkdir`) when it is missing too.
  Pass only a top folder (SYNTHS, SAMPLES) there: a palette folder (`SYNTHS/GENERATIVE/FX`) would be made on a card
  without the pack. The lane browser tries the palette folder with no default first, then the top folder.
- The lane browser previews through stock's preview voice (`AudioEngine::previewSample()`), at the lane's volume from
  `previewVolumeFor()` (F20, shared with stock's sample browser), reading the row's NoteRow param manager and the
  Kit clip's. `AudioEngine::stopAnyPreviewing()` on every move, folder change and close. Presets preview through
  `firmware::presetPreview` (#443, see [firmware](firmware.md)): the browser's `graphicsRoutine()` services it and stops
  it on a start, and `PerformView::focusRegained()` stops it too, for a close that didn't go through `closeBrowser()`.
- TAP (F4): stock `tapTempoButtonPress()` keeps the tap count, the first tap, the LED and its reset timer; under
  Perform it hands `PerformView::tapTempo()` only the taps' mean gap in samples, where stock would set the song's
  tempo, and returns at once under an external clock.
- Stock's `BACK_MENU_EXIT` timer starts on any BACK press (`buttons.cpp`) and fires on whichever UI is current
  then: when the press closed an overlay (the preset browser in SYNTHS/, a Perform menu), it fires on Perform. So
  `PerformView::exitUI()` opens LEAVE PERFORM? only when `backDown_` says the press reached Perform;
  `focusRegained()` clears it. End is the same, with one check, `endPadActs()` ("Perform on top": the current UI):
  the browser and the sound editor hand sidebar presses on, so under them End's press doesn't hold it, a hold made on
  Perform doesn't arm and a release isn't a tap. A release still lets go of the hold (else the pad stays lit under the
  browser, `end=q+` in `gen_sidebar`), and `focusRegained()` lets go of one whose release the overlay kept. The browser is the current UI from the start of
  its `opened()`, so a release delivered during its card read isn't a tap (`endPadActs()`); it still lets go of the
  hold.
- `refreshScreen()` and `showMacroPopup()` redraw the OLED only while Perform's screen shows (`screenShows()`: Perform
  is current, or every UI over it shows the one underneath, as `doAnyPendingOLEDRendering()` walks them). Perform's
  status line changes every beat, and each `renderUIsForOled()` under an overlay that covers it restarted that
  overlay's side scroller (the browser's long file names, a menu's long rows), so they never scrolled to their end;
  around a context menu (LEAVE PERFORM?) the status line keeps moving. `focusRegained()` redraws when an overlay
  closes.
- Opening a UI can read the card (the browser lists its folder in `opened()`), and `routineForSD()` delivers pad
  presses and releases meanwhile, to the new UI, which `openUI()` has made current before `opened()`. A Perform hold
  that a release would act on checks that Perform is on top (End: `endPadActs()`) rather than letting go before
  `openUI()`.
- SHIFT + Y (`PerformView::octaveTurn()`) and OCTAVES ▸ step from `PerformSteering::octaveAhead()`, which reads the
  newest SetOctave still in the steering queue (`SteeringQueue::newest()`): stepping from the core's value, two turns
  inside one step window would press the same shift twice, and the second cancels the first. The rows read it at each
  draw, so they need no `Basis`. `octavePresses_` is a byte, in padding: the steering's size moves
  `PerformAdapter::sink_`, which the start step's modulo-8 `static_assert` in `incomingStarts()` pins (a `uint32_t`
  there broke it).
- Part bias on the KEYBOARD faders and F8 (#303): `PerformSteering::holdFader()` / `keepFader()` /
  `springFaderBack()` take the part the press steers (-1 for the Recipe level), which `MacroFaderHolds::part()`
  remembers from the first pad down, so a release after the focus moved still lets go of the part it held. A row's
  bias is `faderBias()` (`keyboard_page.h`). A part's fader hold is apart from the Recipe-level one in
  `MacroSteering` (`holdBias()`, `biasHeldOn()`), so a knob turn still moves the centre. `onMacroPage()` no longer
  needs the Recipe level: with a part focused, F8 turns its bias (`modEncoderAction()`), and the rings read
  `biasedCentre()`.
- Trap: `MacroSteering` is inside `PerformSteering`, whose size moves `PerformAdapter::sink_`: growing it by 12 B
  broke `incomingStarts()`'s modulo-8 `static_assert` in the Release build only (the harness doesn't build the
  firmware). Grow it by a multiple of 8 (the bias hold's part is an `int16_t` for that).
- X+PLAY reaches Perform through F14's `PerformAdapter::pressPlay()`, which reads the restart shortcut itself (X, or
  CROSS SCREEN with accessibility shortcuts, as stock's `playButtonPressed()` does) and calls `pressRestart()`;
  `playOrStop()` is PLAY without it. RESTART ▸ comes after RESET PART BIAS, then CAPTURE ▸, and BROWSER PREVIEW is the
  last row (`perform_lane_browser.txt` reaches it with `encoder 3 20`, so keep it last): rows are added before it, and
  scripts that count from the top don't move. A script that leaves the menu on a low row goes back to the top before
  closing it (`perform_restart.txt`), since the menu remembers its row.
- Capture marks (#144, spec §17.4): RECORD and MIDI RECORD both reach `PlaybackHandler::recordButtonPressed()`
  (the panel's on a short press's release), which under Perform calls `PerformAdapter::pressMark()`; SHIFT+RECORD
  resamples before that and uses the press up. What a press does, the dot, the RECORD LED, the CAPTURE peek's lines
  and the CAPTURE menu's rows are the portable `generative/perform/capture.h`, read from the `SteeringLog`
  (`current()`, `markPiece()`, `markedPieces()`); `captureStatus()` feeds `OledInput::capture`. The LED is asserted
  every frame in `assertRecordLed()`, except while resampling or in MIDI learn, which own it. An earlier piece
  unmarked in CAPTURE ▸ stays in the log until the next `beginPiece()`, which only flags it dropped; the steps after
  close its room 4 KB at a time (`compactStep()`, from `NoteSink::compactLog()`), never in one move in the tick (#439
  R1). Offsets are logical while a move runs: read a piece's header or records only once `compacting()` is false.
- The SELECT-turn peeks go round RECIPE, QUEUE, CAPTURE (`selectEncoderAction()`); the first turn is still QUEUE to
  the right and RECIPE to the left, which `perform_sidebar.txt` relies on.
- Punch-ins on the KEYBOARD page (#301, §8.9, §13.5): `perform::PunchHolds` (`keyboard_page.h`) is the pads' 300 ms
  rule, keyed by `punchKey()` (a LaneMute by its Lane, so a latched mute follows the lane across a genre change);
  `PerformView::punchPadAction()` calls `PerformSteering::setPunch()` and, if the queue refuses, `refused()`. The
  steering keeps what is on (stopped too) and re-sends it in `instanceStarts()`. The pads' test text is its own event,
  `gen_punch` (`describePunches()`): probe scripts match `gen_keyboard` exactly (`perform_part_bias.txt` ends one
  with `$`), so a new field there breaks them. The punch-in bytes sit in the padding after `fillShift_`: the
  steering's size is pinned (the trap below); at the struct's end they grew it by 4 and broke the Release build.
- Stock Performance view (KEYBOARD's second press, `Page::Performance`, `perform_view_performance.cpp`) is hosted, not
  copied: Perform stays the root UI and calls stock `performanceView.padAction()` and `renderMainPads()` (into a
  scratch image, diffed through `showMainPads()`), with no stock edit. Stock sets a param and starts stutter only
  through `view.activeModControllableModelStack`, which must be the song (`onSong()` moves the knobs there for the
  call when a part is focused), and its layout load reads stutter's options there too. The layout loads once per entry
  (`enterPerformancePage()`, a card read: `REMIND_ME_OUTSIDE_CARD_ROUTINE` in the card routine), after clearing
  `fxPress`'s held columns, which another song's session could have left. Stock pads pressed there are tracked in
  `stockHeld_`, so their release reaches stock on any page and `releaseLostHolds()` lets go of a swallowed one. Stock's
  own column editing opens `performanceView` as a UI, whose buttons change the root UI: it isn't reachable from
  Perform.
- Logging (§17.2): `SetPunch` lands through `apply()` and logs generically; the stock page's param edits reach the log
  through stock's `setCurrentValueInResponseToUserInput()` → F3's `playerEdited()` → a song-target `SetParam`.
  Stutter isn't a param edit: `graphicsRoutine()` samples `UI_MODE_STUTTERING` each frame and pushes `SetStutter`
  (logged only) on a change, so a gold knob's stutter is logged too.
- `generative/firmware/pump.h` is the swaps' service pump, not the Pump punch-in: that is `sidechain_pump.*`.
- Trap (#437 R1): a driver-side effect the core switches on (Pump's ducking) must be let go at every instance start,
  not only at a stop. A release made after the last instance's final decision (12 ticks before a restart's line or a
  transition's start step) never reaches that instance, and a restart's `pieceStarts()` empties the queue: the off is
  lost. `PerformAdapter::instanceStarts()` calls `sidechainPump.release()` before the steering re-sends what is on, and
  the pump remembers each ducked part's own Output and clip, so it lets go of a transition's outgoing roster too
  (`sink_.roster` is already the incoming one there). A logged-only state (stutter's `SetStutter`) has the same
  window: the steering keeps it (`stutterOn_`) and re-sends it at step 0.
- RESTART ▸ has two rows, AS IT IS then AS IT BEGAN, and the submenu remembers its row too: a script that picks AS IT
  BEGAN puts the submenu back on AS IT IS (`encoder 3 -20` inside it) before leaving, or `perform_restart.txt`'s
  SELECT, SELECT picks AS IT BEGAN (`perform_restart_began.txt` does).
- Trap (#413 part 2, merging #437): the restart's two flags and `stutterOn_` share the last padding byte before
  `fillSent_`; they are 1-bit bitfields there. Another flag in that run grows the steering by 4 and breaks the pinned
  size (Release build only): find padding elsewhere, or make it a bit beside them.
- Perform's VARIANTS ▸ (#495) sits after REGENERATE and shows only for a genre with sets, so in Dub, and in every Techno
  subgenre since #531 (Acid techno, the scripts' default), every Perform menu row below REGENERATE is one further down
  than in a genre without sets, as is every Compose screen row below SCALE, and the Regenerate page has a VARIANTS row
  between TRANSITION and GO: a probe script that counts rows from the top counts it. It is the Regenerate page's
  `VariantsMenu` over the same `VariantSetRow`s and `VariantToggle`s, with a third `VariantsTarget` (`perform`): the
  ticks read `maskAhead()`, a turned tick is `pressVariantMask()`, and a set that isn't live doesn't open (AT REGEN). A
  recipe's `allowed` may be 0 (RANDOM): compare masks through `allowedIn()`. Perform's target has no `allowed` array
  (nullptr): a toggle path that falls through to the page's `target.allowed[...] =` hangs the firmware.
  `Submenu::selectButtonPress()` is `final`, so a row that must not open refuses in `checkPermissionToBeginSession()`
  (the sound editor asks it before entering).
