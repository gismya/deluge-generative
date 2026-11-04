# Deluge UI conventions and free controls

Research for [#33](https://github.com/gismya/deluge-generative/issues/33), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1). It feeds [Perform control layout (#38)](https://github.com/gismya/deluge-generative/issues/38) and [Entering Generative mode and the Compose screen (#39)](https://github.com/gismya/deluge-generative/issues/39). It builds on [Firmware architecture (#2)](https://github.com/gismya/deluge-generative/issues/2), [Runtime budget (#3)](https://github.com/gismya/deluge-generative/issues/3) and [Perform interaction model (#15)](https://github.com/gismya/deluge-generative/issues/15).

**Question.** What does a new `RootUI` in v1.3.0-rc3 have to work with on an OLED Deluge, and how do the existing views use it? Controls a root UI can own versus global handling, precedents in the existing views, how views are entered and which combos are free, every root-UI identity check, OLED and pad drawing (with UART cost), and text entry.

**Sources.** Firmware source on the `generative` branch at commit `487f7d9e` (upstream `v1.3.0-rc3` plus docs). Paths are relative to `src/deluge/`; `../` means `src/`. The shortcut tables in `website/src/components/deluge_companion/data/shortcuts/` were used to check which combos are documented. Nothing was run on hardware. Values marked **est.** are my own arithmetic.

---

## Summary

- **Input always goes to the top of the UI stack, `getCurrentUI()`, not the root.** Pads, every button, the scroll and select encoders and both gold knobs reach the UI. Only the **tempo encoder turn** is hard-wired past the UI to `playbackHandler.tempoEncoderAction` (`hid/encoder_input.cpp:133-147`).
- **`View` is not a base class.** It is a global helper object, `view`. TAP, LEARN, SAVE, LOAD, SYNC-SCALING, BACK (undo/redo) and SHIFT+SELECT (settings) only work if the root UI forwards to `view.buttonAction` (`gui/views/view.cpp:137-469`). `TimelineView` does (`gui/views/timeline_view.cpp:119`). A bare `RootUI` gets none of it.
- **The 8 mod buttons never reach `buttonAction`.** They go straight to `modButtonAction` (`hid/buttons.cpp:100-107`). PLAY, SHIFT, RECORD, the tempo push and the gold-knob pushes are global **fallbacks** that only run if the UI returns `NOT_DEALT_WITH` (`hid/buttons.cpp:150-239`). A UI must let SHIFT through, or `isShiftButtonPressed()` breaks.
- **Free entry combos in session view:** SHIFT+SONG, SCALE, SHIFT+SCALE, CROSS-SCREEN and SHIFT+CROSS-SCREEN have no handler. KEYBOARD and SHIFT+KEYBOARD are both taken by Performance view.
- **23 `== / != &performanceView` checks in 9 files**, plus `getUIType()` switches. The ones that would mistreat a new root UI: undo casts the current UI to `TimelineView*` (`model/action/action_logger.cpp:565,580`); PLAY picks arrangement playback unless `lastClipInstanceEnteredStartPos == -1` (`playback/playback_handler.cpp:494-503`); SELECT opens Settings instead of a song menu (`gui/ui/sound_editor.cpp:2080-2088`); the sound editor greys the whole grid and closes on any pad (`:502-507`, `:1783-1810`); MIDI follow falls back to the current clip (`io/midi/midi_follow.cpp:429-451`).
- **OLED:** 128×43 visible pixels, 4 text rows of the 6-px font (21 characters), or a title plus 3 rows. UI rendering runs at most every 10 ms and only with > 882 bytes free in the PIC UART buffer (`gui/ui/ui.cpp:400`). Frames are deduped before sending.
- **Pads:** a full 18×8 redraw is 441 bytes at 200 000 baud, **est. 22 ms**. A single column pair is 49 bytes, **est. 2.45 ms**. Sending only changed column pairs is the cheap path.
- **Text entry:** `QwertyUI` / `RenameUI` gives a pad keyboard (A–Z, 0–9, `- ' , .`, space). A subclass implements `trySetName` and opens with `openUI`. There is no number-entry UI; numbers are menu items in `soundEditor`.

---

## 1. Controls: what a root UI owns

### 1.1 Dispatch

- **Hardware read.** `readButtonsAndPads()` reads one byte from the PIC UART (`deluge.cpp:249`). Pads go to `matrixDriver.padAction` (`deluge.cpp:289`), buttons to `Buttons::buttonAction` (`deluge.cpp:296`). A `REMIND_ME_OUTSIDE_CARD_ROUTINE` result puts the byte back for later (`deluge.cpp:299-305`).
- **Pads.** `getCurrentUI()->padAction(x, y, velocity)` with no global interception (`hid/matrix/matrix_driver.cpp:75-83`). Pads are ignored during stem export (`:71-73`). The sidebar arrives as x=16 and x=17 (`kDisplayWidth` = 16, `../definitions_cxx.hpp:91`); each view decides what they mean.
- **Buttons** (`hid/buttons.cpp:75-243`), in order:
  1. `buttonStates` is updated first, so `Buttons::isButtonPressed()` is right even for consumed buttons (`:80-81`).
  2. The 8 mod buttons go to `getCurrentUI()->modButtonAction(i, on)` and stop (`:100-107`).
  3. Pre-hooks that don't stop the UI seeing the button: AFFECT ENTIRE cancels any popup (`:109-112`); SHIFT+LEARN+AFFECT ENTIRE swaps the display type and is consumed (`:113-118`); BACK arms the `BACK_MENU_EXIT` long-press timer (`:121-127`); CROSS-SCREEN and SELECT push set flags (`:129-138`).
  4. `getCurrentUI()->buttonAction(b, on, inCardRoutine)` (`:141`).
  5. Only if that returned `NOT_DEALT_WITH`: PLAY (`:151-174`), SHIFT → `commandToggleShift` (`:176-178`), RECORD (`:180-210`), TEMPO push (`:213-231`), gold-knob push → `modEncoderButtonAction` (`:234-239`). Anything else is dropped.
- **Encoders** (`hid/encoders.cpp:38-43` declares scrollY, scrollX, tempo, select, mod0, mod1; routing in `hid/encoder_input.cpp`):
  - scroll X → `horizontalEncoderAction` (`:111-121`).
  - scroll Y → SHIFT+LEARN+turn sets the pad dimmer globally (`:124-125`); otherwise `verticalEncoderAction` (`:128`).
  - **tempo → `playbackHandler.tempoEncoderAction`** (`:143-145`). The only exception is a hard-coded `getCurrentUI() == &instrumentClipView` (or automation note editor) with Quantize on (`:134-140`).
  - select → CLIP held + turn sets pad refresh time (`:150-151`); RECORD held + turn sets threshold recording (`:153-156`); otherwise `selectEncoderAction` (`:159`).
  - gold knobs → `modEncoderAction`, with a first-wiggle filter (`:168-190`).
- **Defaults** in `UI`: pads, scroll encoders return `DEALT_WITH`, select does nothing (`gui/ui/ui.h:97-103`). The `mod*` methods forward to `view` (`gui/ui/ui.cpp:33-43`).

### 1.2 Per control

| Control | Where handled | Can a root UI own it? |
|---|---|---|
| 16×8 main pads | `padAction`, x 0–15 | **Fully.** |
| Sidebar x=16, x=17 | `padAction`. Session rows: x=16 mute/launch, x=17 section (`gui/views/session_view.cpp:1040-1078`). Arranger: x=16 mute/solo, x=17 audition (`gui/views/arranger_view.cpp:976-989`). Keyboard: two configurable control columns (`gui/ui/keyboard/layout/column_controls.cpp:33-34`). | **Fully.** The names "audition" and "mute" are per-view conventions. |
| Gold knob turn | `modEncoderAction` → `View::modEncoderAction` acting on `activeModControllableModelStack` (`view.cpp:817-840`) | **Fully,** by overriding. |
| Gold knob push | `buttonAction(MOD_ENCODER_x)`, then fallback `modEncoderButtonAction` (`buttons.cpp:234-239`); default is SHIFT+push deletes automation (`view.cpp:1349-1371`) | **Fully.** |
| 8 mod buttons | `modButtonAction` only (`buttons.cpp:100-107`); default `View::modButtonAction` (`view.cpp:1519-1568`) | **Fully,** by overriding `modButtonAction`. Never via `buttonAction`. |
| Select turn / push | `selectEncoderAction` / `buttonAction(SELECT_ENC)`. SHIFT+push = settings, only through `view.buttonAction` (`view.cpp:447-462`) | Yes, except CLIP+turn and RECORD+turn (`encoder_input.cpp:150-156`). |
| Scroll X / Y | `horizontalEncoderAction` / `verticalEncoderAction`. X held is read globally for PLAY restart (`playback/playback_handler.cpp:213-215`) and clock nudge (`:2293`). | Yes, except SHIFT+LEARN+Y (`encoder_input.cpp:124`). |
| PLAY | Fallback `buttons.cpp:151-174` → `playButtonPressed` | Yes, by returning `DEALT_WITH`. Modal UIs return `NOT_DEALT_WITH` on purpose so PLAY keeps working (`gui/views/instrument_clip_view.cpp:3844-3845`). |
| RECORD | Fallback `buttons.cpp:180-210`; SHIFT+RECORD resamples (`:188-190`) | Yes. |
| SHIFT | Fallback `buttons.cpp:176-178`; state in `Buttons::isShiftButtonPressed()` (`:302-304`), sticky shift in `commandToggleShift` (`:245-286`) | **Must pass through.** Consuming it breaks the global shift state. |
| BACK | Long-press timer always armed (`buttons.cpp:121-127`); on expiry `getCurrentUI()->exitUI()` (`gui/ui_timer_manager.cpp:162-163`). Undo/redo only via `view.buttonAction` (`view.cpp:402-418`). | Press: yes. The 400 ms long-press always calls `exitUI()`; override it. |
| SAVE / LOAD | Only `View::buttonAction` (`view.cpp:229-271`, `:274-329`) | **Fully.** |
| TAP | Only `View::buttonAction` (`view.cpp:144-180`); SHIFT+TAP metronome (`:168-169`) | Press: yes. TAP held still changes the tempo encoder's meaning (swing interval, `playback_handler.cpp:2280-2287`). |
| Tempo turn | Always `playbackHandler.tempoEncoderAction` (`encoder_input.cpp:143`): TAP held = swing interval (`playback_handler.cpp:2280`), X held = nudge (`:2293`), SHIFT = swing amount (`:2298`), LEARN = clock-out scale (`:2308`), else tempo, with push+turn coarse/fine (`:2338-2345`). Tempo is edited in place only for session/arranger on OLED, else via a popup (`:2321-2326`). | **No.** Needs a firmware hook in `encoder_input.cpp:133-147`, like the Quantize branch. |
| Tempo push | Fallback `buttons.cpp:213-231` | Yes. |
| SYNC-SCALING | `view.cpp:332-399` | Yes. |
| TRIPLETS | `TimelineView::buttonAction` (`timeline_view.cpp:92-99`) | Yes; a non-`TimelineView` root gets nothing. |
| CROSS-SCREEN, SCALE, KEYBOARD, SONG, CLIP, SYNTH/KIT/MIDI/CV | No global handler; each view does its own (e.g. `session_view.cpp:227`, `:245`) | **Fully.** They do nothing unless implemented. |
| AFFECT ENTIRE | Popup cancel always (`buttons.cpp:109-112`), then the UI | Mostly. |

Other global combos that fire whatever the root UI: SHIFT+LEARN+AFFECT ENTIRE (`buttons.cpp:113-117`), SHIFT+LEARN+Y turn (`encoder_input.cpp:124`), CLIP+select turn, RECORD+select turn (`:150-156`), all tempo-turn combos, X held + PLAY restart (`playback_handler.cpp:213-215`). MIDI can toggle SHIFT (`playback_handler.cpp:2944-2946`). The MIDI-mapped TAP only fires when `getCurrentUI() == getRootUI()` (`:2951-2953`).

### 1.3 Hold and shift gestures, general mechanics

- **Press timing.** Store `AudioEngine::audioSampleTimer` on press, compare on release against `kShortPressTime` (`../definitions_cxx.hpp:1054`, half a second) or `FlashStorage::holdTime`. RECORD does this at `buttons.cpp:183,200-201`; SAVE at `view.cpp:239,253`.
- **UI modes.** `currentUIMode` (`gui/ui/ui.cpp:411`) with the `UI_MODE_*` values (`gui/ui/ui.h:34-89`). Low byte is exclusive; a few bit flags combine. Helpers `isUIModeActive`, `enterUIMode`, `exitUIMode` (`ui.cpp:413-474`).
- **Timers.** `uiTimerManager.setTimer(TimerName::UI_SPECIFIC, ms)` calls `getCurrentUI()->timerCallback()` (`gui/ui_timer_manager.cpp:155-160`). `LONG_PRESS_DURATION` is 400 ms (`ui.h:91`).
- **Synthetic releases.** When the PIC reports no presses, release events are sent for anything still held (`buttons.cpp:323-331`, `matrix_driver.cpp:56-67`).

---

## 2. Precedents

### 2.1 Shared framework

- **UI stack.** `uiNavigationHierarchy[]`, index 0 is the root (`ui.cpp:62`, `:149-154`). `changeRootUI` resets the stack to depth 1 and calls `opened()` (`ui.cpp:107-121`). `openUI` pushes, cancels `UI_SPECIFIC` and calls `opened()` (`ui.cpp:233-253`). `closeUI` pops, cancels `UI_SPECIFIC`, calls `focusRegained()` on the new top and redraws (`ui.cpp:181-231`). None of them touch playback.
- **Pad rendering.** Rows are rendered top-down; the first UI whose `renderMainPads` / `renderSidebar` returns true owns those rows (`ui.cpp:340-368`). Called with `image == nullptr` they must only report opacity (`ui.h:140-150`).
- **OLED rendering.** Walks down while `oledShowsUIUnderneath` is set, clears `OLED::main`, then calls `renderOLED` bottom-up (`ui.cpp:371-391`). The flag defaults false (`ui.cpp:30`).
- **Greyout.** The topmost UI whose `getGreyoutColsAndRows` returns true wins (`ui.cpp:73-83`). Column bit i means x = 17 − i (`hid/led/pad_leds.cpp:299`), so `0x03` is the sidebar.
- **Gold-knob binding.** `view.setActiveModControllableTimelineCounter(tc)` calls `tc->getActiveModControllable`, then sets the mod LEDs and knob rings (`view.cpp:1889-1920`). For the song, `Song::getActiveModControllable` binds `globalEffectable` (a `GlobalEffectableForSong`, `model/song/song.h:188`) **only if `affectEntire` is set**, else null (`model/song/song.cpp:5794-5803`).
- **Obligations.** `renderOLED` and `getUIType` are pure virtual (`ui.h:157`, `:164`). `TimelineView` also forces `getMaxZoom` and `getMaxLength` (`timeline_view.h:35-38`).

### 2.2 PerformanceView (`gui/views/performance_view.cpp`)

- **Class.** `final : ClipNavigationTimelineView` (`performance_view.h:58`). `getUIType` = PERFORMANCE (`.h:69`). `getUIContextType` returns ARRANGER or SESSION from `lastClipInstanceEnteredStartPos` (`.cpp:299-307`).
- **Pads.** Each column x 0–15 is one song FX param (`layoutForPerformance[x]`, defaults `:102-120`, 20 assignable params `:77-100`). Each row y is a preset value from `defaultFXValues[x][y]` (`:175-176`, `:1502-1521`). Drawn by `renderRow` (`:373-428`). The sidebar is delegated to session or arranger (`:442-460`, `:1036-1077`).
- **Pad behaviour** (`normalPadAction`, `:1082-1159`). Press sets the value through `setParameterValue` → `setValuePossiblyForRegion` (`:1410-1463`). A long press (≥ `holdTime`) reverts on release (`:1124-1145`). A quick tap latches (`:1148-1152`).
- **Gold knobs.** `focusRegained` forces `currentSong->affectEntire = true` and binds the song (`:277-281`), so the knobs always drive song FX. `modEncoderAction` acts only when it is the current UI (`:1601-1618`). `View` skips its knob popup when the perf display already shows that param (`view.cpp:869-873`).
- **OLED.** `renderOLED` → `renderViewDisplay()` plus `sessionView.renderOLED` (`:683-686`). The two display functions draw straight to `OLED::main` and return unless `getCurrentUI() == this` (`:465-469`, `:546-550`). Shows "Performance View", the edit mode, or param name and value (`:471-657`).
- **Gestures.**
  - KEYBOARD from session/arranger enters it (`session_view.cpp:665-669`, `arranger_view.cpp:420-424`). Releasing KEYBOARD after ≥ `holdTime` returns, so a hold is a **peek** (`performance_view.cpp:952-968`). A press while in it leaves (`:937-950`).
  - SHIFT+KEYBOARD opens value-edit mode by `openUI(&performanceView)` on top of itself; again → param-edit; again → close (`:904-936`). BACK in edit mode closes (`:838-846`).
  - Value edit: hold pad + select turn (`:1524-1540`). Param edit: hold shortcut pad + press a column (`:1214-1274`).
  - X held + BACK resets held FX (`:830-834`). SAVE/LOAD held + KEYBOARD saves/loads the layout (`:849-865`).
  - SHIFT + main pad → `soundEditor.potentialShortcutPadAction` (`:1012-1018`).
- **Sound editor.** SELECT → `soundEditor.setup(nullptr, &soundEditorRootMenuPerformanceView)` + `openUI(&soundEditor)` (`:868-886`).

### 2.3 SessionView (`gui/views/session_view.cpp`)

- **Class.** `final : ClipNavigationTimelineView` (`session_view.h:44`). Type SESSION.
- **Rows layout.** y = clip `songViewYScroll + y` (`:1894-1900`), x = time (`:2765-2771`). x=16 mute/launch (`:1547-1558`, `:1040-1046`), x=17 section (`:1561`, `:1050-1078`).
- **Grid layout.** x = track, y = section (`:4785-4810`). x=16 section launcher (`:3289`), x=17 mode pads Launch/Edit/loop/layer (`:3323-3366`). Holding a mode pad switches mode temporarily (`:3960-4003`).
- **Gold knobs.** Song bound in `focusRegained` (`:182-184`). Holding a clip rebinds to it (`:833`, `:4125`); release rebinds the song (`:1117`). AFFECT ENTIRE toggles (`:402-406`).
- **OLED.** `renderOLED` (`:1924-1963`) → `renderViewDisplay` (`:2064-2111`): tempo with metronome icon, song name (side-scrolls), root and scale. Held clip shows the output name.
- **Gestures.** Hold clip + other row = clone (`:893-909`). Tap = enter (`:952-969`). Hold clip + SAVE = delete (`:482-503`). Hold clip + SONG = copy to arranger (`:294-379`). SHIFT + section pad = cycle (`:1142-1183`). Hold section pad 300 ms = repeats edit (`:1186-1222`). Hold RECORD 500 ms = arm view (`:413`, `:1224-1233`). Hold SONG + select = change layout (`:1373-1378`).
- **Sound editor.** SELECT → `soundEditor.setup()` + `openUI` (`:557-560`), giving the song menu.

### 2.4 ArrangerView (`gui/views/arranger_view.cpp`)

- **Class.** `final : TimelineView` (`arranger_view.h:37`). Type ARRANGER.
- **Pads.** y = output, x = arrangement time (`:568-583`, `:2217-2219`). x=16 mute/solo/arm (`:612-656`, `:1018-1148`), x=17 audition (`:658`, `:1150-1175`).
- **Gold knobs.** Song in `focusRegained` (`:550-552`). Held clip instance binds that clip (`:1247`); held audition binds its output (`:926-931`).
- **OLED.** Shows the output name while auditioning, otherwise `sessionView.renderOLED` (`:105-121`).
- **Gestures.** SHIFT + pad = clone (`:1262-1265`). Hold + press right = length (`:1286-1291`). Audition + SYNTH/KIT/MIDI/CV = change type (`:339-408`). Audition + SAVE = delete output (`:311-319`). X + BACK = clear arrangement (`:411-418`). CROSS release = auto-scroll (`:244-259`).

### 2.5 KeyboardScreen (`gui/ui/keyboard/keyboard_screen.cpp`)

- **Class.** `final : RootUI, InstrumentClipMinder` (`keyboard_screen.h:35`), not a `TimelineView`. It calls `view.buttonAction` itself (`keyboard_screen.cpp:614`).
- **Pads.** Delegated to the current layout (`:808-849`). Isomorphic: note = `scrollOffset + x + y·rowInterval` (`layout/isomorphic.h:43-44`). Sidebar is two configurable control columns (`layout/column_controls.cpp:33-34`).
- **Gold knobs.** No override; `InstrumentClipMinder::focusRegained` binds the clip (`model/clip/instrument_clip_minder.cpp:324-326`).
- **Gestures.** Hold KEYBOARD + select = change layout, and release only exits if unused (`keyboard_screen.cpp:488-507`, `:728-732`). Hold column pad y=7 + X turn = change that column's function (`column_controls.cpp:322-339`).
- **Sound editor.** SHIFT + pad goes to `potentialShortcutPadAction` first (`keyboard_screen.cpp:104-107`).

### 2.6 Sound editor as an overlay

- **Open.** `soundEditor.setup(Clip* clip = nullptr, const MenuItem* item = nullptr, int32_t sourceIndex = 0)` (`gui/ui/sound_editor.h:135`), then `openUI(&soundEditor)`. For a root that isn't a ClipMinder, it always edits the **song's** `globalEffectable` (`sound_editor.cpp:1951-1959`). With a clip it edits that clip's sound, kit row or audio clip (`:1960-2026`).
- **Default menu** when `item` is null and no clip: PerformanceView → its menu; session/arranger/automation → `soundEditorRootMenuSongView`; **anything else → `settingsRootMenu`** (`:2079-2090`).
- **SHIFT+pad shortcuts.** `potentialShortcutPadAction` (`:1477-1687`) uses the song shortcut table for non-ClipMinder roots (`:1419-1421`), then `setup` + `enterOrUpdateSoundEditor` (`:1689-1719`).
- **While open.** Main pads are transparent, so the root keeps drawing them (`:248-276`). Greyout by root type (`:462-511`): none for keyboard and performance, **everything for the default case**. `padAction` forwards to keyboard, automation or performance roots (`:1737-1780`); for any other root a pad press **closes the menu** (`:1783-1810`). Only the menu is drawn on the OLED (`:2400-2414`). The root's `graphicsRoutine` still runs (`ui.cpp:45-48`, `sound_editor.h:134`).
- **Return.** BACK → `goUpOneLevel`, at depth 0 `exitCompletely` (`:681-710`, `:939-947`). Holding BACK → `exitUI()` = `exitCompletely` (`sound_editor.h:143`). `exitCompletely` calls `close()` and **sets `currentUIMode = UI_MODE_NONE`** (`:962-1007`). `closeUI` then calls the root's `focusRegained()` (`ui.cpp:203`). SONG inside the menu calls `changeRootUI` to session or arranger (`:830-839`).
- **Playback** keeps running throughout.

**Traps for a new root.** Pass an explicit menu item, or SELECT opens Settings. Add the new `UIType` to the greyout switch and the pad-forwarding branch, or the grid greys out and pads close the menu. Keep `focusRegained()` idempotent and rebind the knobs there. Re-set any hold mode after the editor closes. Guard OLED drawing in `graphicsRoutine` with `getCurrentUI() == this`. Song-level `setup` with a null clip reads `clip->type` (`sound_editor.cpp:2159`) when there is no sound; it works today but is a latent null read.

---

## 3. Entering a view

### 3.1 How each view is reached

`changeRootUI` is at `gui/ui/ui.cpp:107-121`.

| View | Entry |
|---|---|
| Session | SONG from arranger (`arranger_view.cpp:204-211`); SONG release from a clip view → `ClipMinder::transitionToArrangerOrSession` (`model/clip/clip_minder.cpp:28-39`); KEYBOARD from Performance (`performance_view.cpp:948,966`); SONG in the sound editor (`sound_editor.cpp:830-838`); undo (`model/action/action_logger.cpp:675`). |
| Arranger | SONG release in session (`session_view.cpp:382-394`, `:692`); clip pad + SONG (`:294-378`); back from a clip when `lastClipInstanceEnteredStartPos != -1` (`arranger_view.cpp:1936-1944`). |
| Instrument / audio clip | Tap a clip pad (`session_view.cpp:952-968`); CLIP in session (`:227-236`); end of the zoom animation (`hid/led/pad_leds.cpp:929`). |
| Keyboard | KEYBOARD in instrument clip view (`instrument_clip_view.cpp:314-320`). |
| Automation | CLIP in a clip view (`instrument_clip_view.cpp:306`, `audio_clip_view.cpp:374`) or arranger (`arranger_view.cpp:225-226`); a menu item via `swapOutRootUILowLevel` (`gui/menu_item/automation/automation.cpp:77-83`). |
| Performance | KEYBOARD in session (`session_view.cpp:665-669`) or arranger (`arranger_view.cpp:420-424`) when `currentUIMode == UI_MODE_NONE`. No SHIFT check, so SHIFT+KEYBOARD enters it too. |

- **Startup and song load.** `setUIForLoadedSong` picks a clip view, arranger or session only (`deluge.cpp:342-379`). There is no "start in view X" setting; `RuntimeFeatureSettingType` has nothing view-related (`model/settings/runtime_feature_settings.h:48-74`). A song saved while in a new mode reopens in session or arranger (`model/song/song.cpp:1179`).
- **Menu route.** A menu item can change the root: `soundEditor.exitCompletely()` and then act, as `gui/menu_item/stem_export/start.h:31-33` does. `editing_mode.h:49-74` opens a UI from a menu item.

### 3.2 Free combos in rc3

All buttons are listed in `hid/button.h:24-54`. Checked against session, arranger, `TimelineView`, `View` and the global path:

- **SHIFT+SONG in session view.** Session requires `!isShiftButtonPressed()` (`session_view.cpp:245`) and nothing later handles it. Also free in Performance view (`performance_view.cpp:762`). **Taken** in arranger, which ignores SHIFT (`arranger_view.cpp:203`).
- **SCALE and SHIFT+SCALE** in session, arranger and Performance. No handler; session just turns the LED off (`session_view.cpp:155`).
- **CROSS-SCREEN and SHIFT+CROSS-SCREEN** in session. Taken in arranger (`arranger_view.cpp:244-259`). CROSS+PLAY is restart when accessibility shortcuts are on (`playback_handler.cpp:214`).
- **Weaker options:** SYNTH/KIT/MIDI/CV with no clip held in session (`session_view.cpp:570`), SHIFT+SAVE (SHIFT is ignored, `view.cpp:237`).
- **Taken:** KEYBOARD and SHIFT+KEYBOARD (Performance), SONG (arranger), hold SONG + select (layout), CLIP, SHIFT+SELECT (settings), SHIFT+LOAD (clear song), SHIFT+BACK (redo), SHIFT+TAP (metronome).
- **Watch out:** for a "hold SONG + X" combo, the SONG release still goes to arranger unless `sessionButtonUsed` is set (`session_view.cpp:383`).

None of the free combos appear in the website shortcut tables (`website/src/components/deluge_companion/data/shortcuts/`).

### 3.3 Root-UI identity checks

`grep -rnE "(==|!=) ?&performanceView" src/deluge` finds 23 checks in 9 files. `getUIType()` is switched on at `view.cpp:1615-1648`, `sound_editor.cpp:477-507`, `io/midi/midi_follow.cpp:429-451` and `io/midi/midi_transpose.cpp:63-75`; `getUIContextType()` at `view.cpp:1662-1672`.

**Would mistreat a new root UI:**

| Where | What happens |
|---|---|
| `model/action/action_logger.cpp:414-481`, `:565`, `:580` | Undo picks an animation from the current UI and, for an unknown UI, casts it to `TimelineView*`. Undefined behaviour unless the new UI is a `TimelineView`. Post-undo refresh (`:681-721`) does nothing for it. |
| `playback/playback_handler.cpp:494-503` | For a non-ClipMinder root, PLAY uses **arrangement** playback if `lastClipInstanceEnteredStartPos != -1`. The new UI must set it to -1, as session does (`session_view.cpp:197`). |
| `gui/ui/sound_editor.cpp:2080-2088` | SELECT opens Settings, not song FX. |
| `gui/ui/sound_editor.cpp:502-507`, `:1783-1810` | Whole grid greyed; any pad closes the menu. |
| `gui/ui/sound_editor.cpp:830-838` | SONG inside the menu leaves for session or arranger. |
| `io/midi/midi_follow.cpp:429-451` | Unknown UI type falls to `getCurrentClip()`, as if in clip view. |
| `gui/views/view.cpp:1662-1672` | SONG LED only set for ARRANGER or SESSION context. |
| `clip_minder.cpp:28-39`, `performance_view.cpp:937-968`, `pad_leds.cpp:941-955` | Return paths hard-code session or arranger, so a clip or Performance view entered from the new mode can't come back to it. |
| `gui/menu_item/stem_export/start.h:34-48` | Start Export does nothing for an unknown root. |

**Treat it as "not Performance view"** (decide case by case): `view.cpp:234,279` (SAVE/LOAD+KEYBOARD), `:870` (knob popup), `:1534` (mod buttons outside UI modes), `:2762,2843,2864` (redraws); `model/global_effectable/global_effectable.cpp:103` (stutter ends on mod button); `model/mod_controllable/mod_controllable_audio.cpp:1224` and `midi_follow.cpp:1119` (learned-CC refresh); `playback/mode/session.cpp:810,1064,1301,2417` (launch countdown and redraws); `playback_handler.cpp:210,345` (`isArrangerView`); `session_view.cpp:1934,1972,2049,2491` and `arranger_view.cpp:3423` (OLED text and `requestRendering`).

**OLED tempo checks:** `buttons.cpp:224`, `playback_handler.cpp:2324` and others edit tempo in place only for session and arranger. A new UI gets the tempo popup instead.

**`UIType`** is at `../definitions_cxx.hpp:187-215`. A new value also needs an entry in `uiTypeMap` under `ENABLE_MATRIX_DEBUG` (`ui.cpp:476-501`).

---

## 4. OLED and pads

### 4.1 OLED geometry and fonts

- 128 × 48 buffer, top 5 rows hidden, **43 visible** (`../RZA1/cpu_specific.h:137-149`). Buffer is 6 pages × 128 bytes = 768 B (`hid/display/oled_canvas/canvas.h:294-304`).
- Fonts (`../definitions_cxx.hpp:993-1009`, chosen by height in `canvas.cpp:501-539`):

| Size | Spacing / height | Monospaced | Characters across (est.) |
|---|---|---|---|
| small | 4 / 5 | no | about 32 |
| normal | 6 / 9 | yes | 21 |
| title | 9 / 10 | no | about 14 (`gui/menu_item/menu_item.h:228`) |
| big | 11 / 13 | no | about 11 |
| huge | 18 / 20 | no | about 7 |

- **Rows (est.).** 4 rows of the normal font, or a title (rule at y=17, `canvas.cpp:913-921`) plus 3 list rows at y = 19, 28, 37 (`menu_item.cpp:52-59`; `kOLEDMenuNumOptionsVisible` = 3, `../definitions_cxx.hpp:983`).

### 4.2 Drawing helpers

- **Canvas** (`canvas.h`): `drawString` (:145), `drawStringCentred` (:157), `drawStringCentredShrinkIfNecessary` (:188), `drawStringAlignRight` (:197), `getStringWidthInPixels` (:226), lines and rectangles (:95-133), `drawGraphicMultiLine` (:238), `drawIcon` (:246), `drawScreenTitle` (:260), `invertArea` (:268). Canvases: `OLED::main`, `OLED::popup`, `OLED::console` (`hid/display/oled.h:103-105`).
- **Popups.** `display->displayPopup(text, numFlashes)`: `numFlashes == 0` is persistent, otherwise an 800 ms box, up to 19 characters per line (`oled.h:169-172`, `oled.cpp:680-719`). `cancelPopup` (`oled.cpp:293-300`).
- **Notification bar.** `displayNotification(title, value)` draws an inverted bar at the top for 2 s (`oled.cpp:819-870`). The gold knobs use it via `View::displayModEncoderValuePopup` (`view.cpp:1093`, `:1259`), throttled to about 45 ms (`view.h:182`, `view.cpp:1272-1295`).
- **Working animation.** `displayLoadingAnimationText` → small icon top right (`oled.cpp:722-817`).
- **Console.** `consoleText`, a line sliding up from the bottom, about 1.2 s (`oled.cpp:912-936`).
- **Side scroller.** `OLED::setupSideScroller(index 0..1, text, startX, endX, startY, endY, spacingX, sizeY, highlight)` scrolls text that doesn't fit, with 400 ms pauses at each end (`oled.cpp:994-1029`, `:1127-1144`). **It does not scroll while playback runs** (`:1065`).
- **Blink.** `OLED::setupBlink` inverts an area every 110 ms (`oled.cpp:948-964`).
- **Values.** Menu items draw a title then `drawPixelsForOled` (`menu_item.cpp:37-41`). `Integer` draws a centred huge number (`integer.cpp:64-73`). `Number::drawHorizontalBar` draws a zero-anchored bar (`number.cpp:33-73`). Horizontal menus have KNOB/BAR/SLIDER/PAN slot styles (`number.h:25-38`).

### 4.3 OLED redraw rate

- `renderUIsForOled()` only sets a flag (`ui.cpp:271-273`). Direct drawing outside the pipeline calls `OLED::markChanged()` (`oled.h:89-96`).
- `doAnyPendingUIRendering` runs as a task every 10 ms, max 30 ms (`deluge.cpp:582-583`). It **skips pad and OLED rendering while ≤ 882 bytes are free** in the PIC TX buffer (`ui.cpp:400-402`).
- `sendMainImage` waits while the SPI bus is busy, **skips frames identical to the last one** (`oled.cpp:390`, `:437-442`), and queues the rest (32 slots, `drivers/oled/oled.h:22`).
- The frame goes by DMA over RSPI at 4 MHz (`deluge.cpp:701-706`), **not** over the PIC UART. The PIC only sends SELECT/DESELECT handshakes, and SELECT waits for the UART buffer to drain (`../RZA1/oled/oled_low_level.c:150`). Each SELECT and DESELECT also waits a 3 ms settle (`:104`, `:326-343`).
- **Est.** 768 B at 4 MHz ≈ 1.5 ms on the wire; a lone frame is about 8 ms end to end with settles and handshakes. Back-to-back frames chain without a DESELECT.

### 4.4 Pad colours and blink

- **Colour.** `RGB{r,g,b}` (`gui/colour/rgb.h:14-34`) with `fromHue` (:55), `dim(level)` (:123), `forTail` (:84), `forBlur` (:96), `blend` (:166), `greyOut` (:148). Named colours in `deluge::gui::colours::` (`gui/colour/palette.h:8-40`).
- **Frame buffer.** `PadLEDs::image[8][18]` and `occupancyMask[8][18]` (`hid/led/pad_leds.h:36-37`). Request redraws with `uiNeedsRendering(ui, mainRowMask, sideRowMask)` (`ui.h:201`, `ui.cpp:293-317`); only visible UIs get rendered.
- **Pad flash.** `PadLEDs::flashMainPad(x, y, colour)` uses the PIC's own flash, 1–2 bytes (`pad_leds.h:132-139`, `drivers/pic/pic.h:150-161`). `setTickSquares` draws one cursor per row and re-sends only the touched column pairs (`pad_leds.cpp:169-236`).
- **Button LEDs.** `indicator_leds::setLedState` (1 byte) and `blinkLed(led, numBlinks, type)` with 6 blink slots (`hid/led/indicator_leds.h:23`, `:81-82`). Flash times 250 / 110 / 60 ms (`../definitions_cxx.hpp:158-160`).
- **Knob rings.** `setKnobIndicatorLevel(knob, 0–128, bipolar)` (`indicator_leds.h:86`, `.cpp:183-188`).
- **Greyout** fades over about 300 ms (`pad_leds.cpp:748-776`, `../definitions_cxx.hpp:156`).

### 4.5 UART cost of pad redraws

- A column-pair update is 1 + 16 × 3 = **49 bytes** (`drivers/pic/pic.h:107-112`, `../definitions_cxx.hpp:94`). Main grid = 8 pairs = **392 B**, sidebar = **49 B**, full 18×8 = **441 B** (`../definitions_cxx.hpp:97-99`).
- UART runs at **200 000 baud** after boot (`drivers/pic/pic.h:26`, `:97`; `deluge.cpp:750`), 8N1. TX ring is 1024 B, shared with LEDs, knob rings and OLED handshakes (`../RZA1/cpu_specific.h:49-55`).

| Update | Bytes | Time (est., 50 µs/byte) |
|---|---|---|
| One column pair | 49 | 2.45 ms |
| Main grid | 392 | 19.6 ms |
| Full 18×8 | 441 | 22 ms (about 45 per second at most) |

- **Throttles.** `sendOutMainPadColours` defers if ≤ 392 B free and retries after 20 ms (`pad_leds.cpp:1035-1053`, `:1079-1083`). UI rendering needs > 882 B free (`ui.cpp:400`). `GRAPHICS_ROUTINE` runs every 15 ms only with > 49 B free (`ui_timer_manager.cpp:226-231`).
- **Consequence.** A full redraw per 16th at 120 BPM (125 ms) uses about 18 % of the UART (est.). Redrawing only changed rows (`uiNeedsRendering` masks) and using `flashMainPad` or `setTickSquares` for playheads keeps it small. The UART is DMA-driven, so the cost is latency and contention with OLED handshakes, not CPU.

---

## 5. Text entry

- **`QwertyUI`** (`gui/ui/qwerty_ui.cpp`). Holds `static String enteredText` and a cursor (`qwerty_ui.h:38`, `:63`). `opened()` clears it and blinks BACK (`.cpp:52-74`).
- **Pad keyboard** (`.cpp:232-252`, home row `kQwertyHomeRow = 3`, `../definitions_cxx.hpp:858`; column = x − 3):

| y | x 3–13 | Extra |
|---|---|---|
| 5 | `1 2 3 4 5 6 7 8 9 0 -` | x 14–15 Backspace |
| 4 | `Q W E R T Y U I O P` | |
| 3 | `A S D F G H J K L` and `'` | x 14–15 Enter |
| 2 | `Z X C V B N M , .` | Shift at x 1–2 and x 13–14 |
| 1 | space at x 5–10 | |

  AZERTY and QWERTZ variants follow `FlashStorage::keyboardLayout` (`../definitions_cxx.hpp:901-905`).
- **Characters.** A–Z, 0–9, `- ' , .`, space, plus shifted `_ ! # ^` (`:335-355`). The first character must be a letter or digit (`:326-332`). A letter after a letter is lower-cased (`:387-395`). No length limit. Typing mid-string cuts off the rest (`util/d_string.cpp:291-292`).
- **Encoders.** X moves the cursor (`:476-506`). The select encoder does **not** scroll through letters.
- **Display.** `drawTextForOLEDEditing(x, xMax, y, maxChars, canvas)` draws a scrolling field with a blinking cursor (`:128-172`).
- **`RenameUI`** (`gui/ui/rename/rename_ui.h:22-41`) is the pattern to copy: implement `trySetName(string_view)` and `getCurrentName()`, optionally `allowEmpty()`. Enter (pad or select push) calls `trySetName`; true closes (`rename_ui.cpp:45-52`). BACK or a sidebar pad cancels (`:103-109`, `:136-142`). It sets `oledShowsUIUnderneath` (`:26`) and draws a 120×40 window with 17 visible characters (`:68-91`). Open it by setting the target on the global instance and calling `openUI` (e.g. `gui/context_menu/clip_settings/clip_settings.cpp:83-84`).
- **Numbers.** No number-entry UI. `menu_item::Integer` / `IntegerWithOff` / `Decimal` edit with the select encoder inside `soundEditor` (`gui/menu_item/integer.cpp:25-39`, `decimal.h:24-44`). A seed could be typed with the qwerty digit row and validated in `trySetName`.

---

## Open

- The real UART line rate: the SCIF is in double-speed mode (`../RZA1/uart/sio_char.c:142-146`), and I did not check the divisor against the RZ/A1 manual. Timings assume a nominal 200 000 baud.
- OLED and pad redraw costs are estimates; measure on hardware.
- Whether the latent null read in `SoundEditor::setup` (`sound_editor.cpp:2159`) matters for a new root that opens the song menu with a null clip.
