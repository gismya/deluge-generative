# Implementation notes: Perform UI (`gui/views/perform_view.*`)

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

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
- GENRE ▸ is built twice, in PERFORM FROM (`perform_from.cpp`) and the Perform menu (`perform_menu.cpp`), from the
  same rows and items (`gui/menu_item/generative/genre_rows.h`: `GenreMenuItems<Item>`, each menu with its own genre
  item; laid out by the portable `perform/genre_menu.h`). Stock
  `Submenu`s have fixed item lists and a row can't be a leaf one time and a submenu the next (`selectButtonPress()` is
  final in `Submenu`), so each row slot holds both a genre item and a `GenreGroup`, and `isRelevant()` shows the one
  that fits. All groups share one list of genre items, which read the open parent (set in
  `GenreGroup::beginSession()`). Both menus open on the Current Recipe's genre (`GenreMenu::beginSession()`), so a
  probe script that picks a genre counts rows from there, not from the top.
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
  KEYBOARD FILL pads and the CLIP page's NEW SOUND and SOUND EDITOR; a Perform menu (`editorOpen_` false) closes on a
  press. A SHIFT + pad that doesn't move an open editor (a param the target lacks) puts the current Clip back.
- With no current Clip, `getCurrentOutputType()` is NONE and the other current-Clip getters return null (`song.cpp`),
  so song-level editor paths never read null. Test builds fault on any null access: run the probe scripts after
  touching what the editor reaches.
- The preset browser (`gui/ui/browser/perform_preset_browser.*`, §13.5) is its own `SlotBrowser`, not stock's
  `LoadInstrumentPresetUI`, whose `currentFileChanged()` loads and swaps the instrument on every scroll step. Its
  `outputTypeToLoad` is NONE, as the sample and DX browsers' are: stock otherwise adds the current song's Instruments
  to the list, and in Perform those are the parts (the default synth among them, `SYNTHS/0`, which isn't on the card).
  A QWERTY UI covers the main pads and can't see the view underneath, so `UI::graphicsRoutine()` wouldn't run
  Perform's frame: the browser's `graphicsRoutine()` calls `performView.graphicsRoutine()` itself, which keeps the
  pump running (`pump.h`), and hands sidebar presses and SYNC-SCALING to Perform. End still can't arm under it:
  `checkEndHold()` drops the hold whenever Perform isn't on top, as under every overlay (§13.5). Its choice goes to
  `PerformView::chooseSound()` after it has closed, so Perform is on top again when the press's popup shows.
  SYNTH's sound rule (a synth part: `PartSpec::synth()`) is the one `buildPartOutput()` uses.
- TAP (F4): stock `tapTempoButtonPress()` keeps the tap count, the first tap, the LED and its reset timer; under
  Perform it hands `PerformView::tapTempo()` only the taps' mean gap in samples, where stock would set the song's
  tempo, and returns at once under an external clock.
- Stock's `BACK_MENU_EXIT` timer starts on any BACK press (`buttons.cpp`) and fires on whichever UI is current
  then: when the press closed an overlay (the preset browser in SYNTHS/, a Perform menu), it fires on Perform. So
  `PerformView::exitUI()` opens LEAVE PERFORM? only when `backDown_` says the press reached Perform;
  `focusRegained()` clears it. End is the same: a hold arms it and a tap cancels it only while Perform is the current
  UI, though the browser and the sound editor hand sidebar presses on (`sidebarPadAction()`, `checkEndHold()`).
- `refreshScreen()` redraws the OLED only while Perform is the current UI. Perform's status line changes every beat,
  and each `renderUIsForOled()` under an overlay restarted that overlay's side scroller (the browser's long file
  names, a menu's long rows), so they never scrolled to their end. `focusRegained()` redraws when it closes.
- Opening a UI can read the card (the browser lists its folder in `opened()`), and `routineForSD()` delivers pad
  presses and releases meanwhile, to the new UI. Let go of Perform's holds that a release would act on (End)
  before `openUI()`, not after.
