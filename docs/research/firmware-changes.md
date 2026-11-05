# Firmware changes the design needs

Research for [#43](https://github.com/gismya/deluge-generative/issues/43), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1). It feeds [Build order (#44)](https://github.com/gismya/deluge-generative/issues/44).

**Question.** Which changes does the design need in *existing* firmware code? For each one: `file:line`, rough size, whether it can go upstream on its own, and which decision needs it. New Generator code (the core, drivers, the Perform view, new menus and browsers) is not on the list, except where saying so settles an ambiguous item.

**Sources.** Every decision on the map and the resolution comments of #2, #7, #12–#21, #24–#26, #28, #30, #33–#35, #37–#40, #42 and #50; the three context comments on #43; ADRs 0001–0004 and `GLOSSARY.md`; the research docs on `origin/research/*` (firmware-architecture, reusable-firmware-features, generator-kits, deluge-ui-conventions, compose-song-construction, runtime-budget, sound-controls-per-engine); the POC findings comment on #1; the diffs of `origin/poc/perform-techno` and `poc/core` against their merge base; and the firmware source.

**Tree.** All `file:line` references are to `generative` at `996759313`, relative to `src/deluge/` unless they start with `src/`. Nothing was run; every claim comes from reading the source.

**Base: rc3 vs v1.3.0.** The map says the base is `v1.3.0-rc3`, but `generative` is now upstream **`v1.3.0`** (`156500e7c`) plus docs plus the sysex test port. The two tags differ in `src/`: 22 commits, 50 files, +1108/−290 lines. The ones that matter here:

- `storage/storage_manager.cpp`: `loadSynthToDrum`'s error paths were fixed upstream (#28's second refactor is done), and `loadInstrumentFromFile` was cleaned up. Line numbers moved; `:406` is still `:406`.
- `gui/ui/sound_editor.cpp`, `gui/views/session_view.cpp`, `model/song/song.cpp`, `model/action/action_logger.cpp`, `gui/context_menu/clear_song.cpp`: small fixes, and line numbers shift by a few lines.

On top of `v1.3.0`, the test port touches 11 files in `src/` (`deluge.cpp`, `gui/ui/ui.cpp/.h`, `hid/encoder_input.cpp`, `playback/playback_handler.cpp`, `io/midi/sysex.*`, `io/midi/test_port.*`, CMake). It adds 4 lines to `encoder_input.cpp` (so the tempo branch is at `:137`, not `:133`) and compiles `uiTypeMap` when `ENABLE_SYSEX_TEST` is set (`gui/ui/ui.cpp:476`). The POC branches are still on rc3 (merge base `c0725ce56`).

---

## Summary

- **The design needs about a dozen small firmware changes, and no large one.** Two are new seams for the Generator: the tick hook and the `AutoParam` live ramp. One is the tempo encoder hook. The rest are guards and cases for a new root UI, in the sound editor, in the record/restart paths, in global MIDI commands and in undo.
- **Only one item from the ticket's starting list is a real standalone upstream fix**, `storage_manager.cpp:406`, and v1 doesn't need it, because whole-kit picks are out of scope. `loadSynthToDrum`'s error paths are already fixed in `v1.3.0`. A second small upstream fix turned up, a null read in `SoundEditor::setup`.
- **Three items on the ticket's list need no firmware change:** loading a song from a memory buffer (Compose and Capture build in memory through the model API), moving Euclidean and pattern logic into the model (the Generator has its own patterns and RNG), and the load-time sound settings (plain field writes).
- **Five things no ticket anticipated:** overlays and MIDI commands reach RECORD and restart past Perform; the MIDI global commands NEXT SONG, LOOP and UNDO act under Perform; undo logs Perform's knob turns and can cast Perform to `TimelineView*`; with the sound editor open, gold-knob turns skip Perform, so homes need a hook; and MIDI follow plays the "current clip" in Perform. There are also four traps for Generator code: `Song()` takes a user-set tick magnitude, sync defaults read the current song, `Song::writeToFile` reads UI state, and `setNodeAtPos` doesn't flag automation.
- **The POC touched 4 firmware files and 20 lines** (`src/definitions_cxx.hpp`, `gui/ui/ui.cpp`, `gui/views/session_view.cpp`, `playback/mode/session.cpp`). Everything else went through public APIs from new files.

---

## Summary table

Sizes: **S** under ~30 lines in existing code, **M** ~30–150 lines or several files, **L** larger or risky.

| ID | Change | Where | Size | Upstreamable alone | Needed by |
|---|---|---|---|---|---|
| **F1** | Generator hook in `Session::doTickForward` | `playback/mode/session.cpp:2541-2553` | S | No | [#19](https://github.com/gismya/deluge-generative/issues/19), ADR 0002 |
| **F2** | `AutoParam` live ramp (target + ticks) | `modulation/automation/auto_param.h:136-144`, `auto_param.cpp:811-868`, `param_set.cpp:105-109`, `patch_cable_set.cpp:345` | S–M | With work | [#19](https://github.com/gismya/deluge-generative/issues/19), [#14](https://github.com/gismya/deluge-generative/issues/14), [#42](https://github.com/gismya/deluge-generative/issues/42) |
| **F3** | Player-edit notification for sound-control homes | `modulation/automation/auto_param.cpp:101` (or the menu items' `writeCurrentValue`) | S | No | [#19](https://github.com/gismya/deluge-generative/issues/19), [#14](https://github.com/gismya/deluge-generative/issues/14), [#38](https://github.com/gismya/deluge-generative/issues/38) |
| **F4** | Tempo encoder hook | `hid/encoder_input.cpp:137-151` | S | With work | [#33](https://github.com/gismya/deluge-generative/issues/33), [#38](https://github.com/gismya/deluge-generative/issues/38), [#15](https://github.com/gismya/deluge-generative/issues/15) |
| **F5** | Register the Perform root UI and SHIFT+SONG entry | `src/definitions_cxx.hpp:187-213`, `gui/ui/ui.cpp:476-502`, `gui/views/session_view.cpp:238-245` | S | No | [#2](https://github.com/gismya/deluge-generative/issues/2), [#39](https://github.com/gismya/deluge-generative/issues/39), [#50](https://github.com/gismya/deluge-generative/issues/50) |
| **F6** | Sound editor edits a Perform part's Sound (song-vs-clip predicate) | `gui/ui/sound_editor.cpp:1106, 1421, 1953, 2311, 2387` | M | With work | [#15](https://github.com/gismya/deluge-generative/issues/15), [#38](https://github.com/gismya/deluge-generative/issues/38) |
| **F7** | Sound editor cases for Perform: no greyout, pads forwarded, SONG doesn't leave | `gui/ui/sound_editor.cpp:477-509, 1777-1780, 829-841` | S | No | [#38](https://github.com/gismya/deluge-generative/issues/38), [#15](https://github.com/gismya/deluge-generative/issues/15) |
| **F8** | MIDI follow target in Perform | `io/midi/midi_follow.cpp:420-454` | S | No | *unanticipated* |
| **F9** | RECORD and restart guarded when Perform is root (overlays, MIDI) | `playback/playback_handler.cpp:213-255, 280-292` | S | No | [#38](https://github.com/gismya/deluge-generative/issues/38), [#35](https://github.com/gismya/deluge-generative/issues/35), [#14](https://github.com/gismya/deluge-generative/issues/14) |
| **F10** | Global MIDI commands guarded in Perform | `playback/playback_handler.cpp:2893-2965, 162-191` | S | No | *unanticipated* |
| **F11** | No undo logging or reversion in Perform | `model/action/action_logger.cpp:86-100, 867-869` (the cast is at `:565, :580`) | S | No | [#38](https://github.com/gismya/deluge-generative/issues/38), POC findings |
| **F12** | `currentSong` → `song` in kit-drum check | `storage/storage_manager.cpp:406` | S | **Yes** (bug fix) | Not needed in v1 ([#25](https://github.com/gismya/deluge-generative/issues/25)); only if whole-kit picks return |
| **F13** | Song save without the save UI (Capture) | `gui/ui/save/save_song_ui.cpp:57, 117-528`; `model/song/song.cpp:1177-1191` | S–M (optional) | With work | [#35](https://github.com/gismya/deluge-generative/issues/35), [#50](https://github.com/gismya/deluge-generative/issues/50) |
| **F14** | Perform keeps its page LEDs (`setModLedStates`) | `gui/views/view.cpp:1570-1700` | S (optional) | No | [#38](https://github.com/gismya/deluge-generative/issues/38) |
| **U1** | Null read of `clip->type` in `SoundEditor::setup` | `gui/ui/sound_editor.cpp:2160` | S | **Yes** (bug fix) | Not required; reached from song menus |
| **U2** | Out-of-memory freezes in kit-row and note-row creation | `model/note/note_row.cpp:3748, 3756`; `model/clip/instrument_clip.cpp:1108` | L (optional) | With work | Risk for [#16](https://github.com/gismya/deluge-generative/issues/16), [#35](https://github.com/gismya/deluge-generative/issues/35), [#15](https://github.com/gismya/deluge-generative/issues/15) |
| — | `loadSynthToDrum` error paths | `storage/storage_manager.cpp:559-597` | — | Done upstream in `v1.3.0` | [#28](https://github.com/gismya/deluge-generative/issues/28) |
| — | Load a song from a memory buffer | — | — | Not needed | [#18](https://github.com/gismya/deluge-generative/issues/18) superseded by [#16](https://github.com/gismya/deluge-generative/issues/16)/[#19](https://github.com/gismya/deluge-generative/issues/19) |
| — | Euclidean and pattern logic into the model | — | — | Not needed | [#7](https://github.com/gismya/deluge-generative/issues/7) |

Items N1–N19 below need no firmware change but carry facts or traps for Generator code.

### Upstreamable fixes that can go first

1. **F12**, `storage_manager.cpp:406`: `currentSong->` → `song->`. A one-token bug fix: loading a KIT preset into a non-current song returns `FILE_CORRUPTED`, or dereferences null if there is no current song. It's still present on upstream `main` (checked against `upstream/main`, 102 commits past `v1.3.0`). v1 doesn't need it.
2. **U1**, `sound_editor.cpp:2160`: `else if (clip->type == ClipType::AUDIO)` runs when `setup()` has neither a sound nor a clip, which is the song menu from session view, and Perform's song FX with nothing focused. Change it to `else if (clip && ...)`. It works today only because address 0 is readable.

`loadSynthToDrum`'s error paths, the other fix #28 recommended, are already in `v1.3.0` (`storage_manager.cpp:569-588`).

F2 (live ramp) and F4 (tempo hook as a `RootUI` virtual) could go upstream as small general APIs, but upstream has no caller for either. The rest are Perform-specific.

---

## A. Playback and scheduling

### F1. Generator hook in `Session::doTickForward`

- **What:** after the per-Output arp loop (`playback/mode/session.cpp:2541-2553`), call the Perform driver with `playbackHandler.lastSwungTickActioned` and lower `playbackHandler.swungTicksTilNextEvent` by its answer, as the arp does.
- **Evidence:** the POC did exactly this, in 7 lines (`poc/perform-techno`, `session.cpp` +7), and it played on hardware.
- **Size:** S. **Upstreamable:** no; it's only meaningful with the Generator. A generic "tick listener" list would be upstreamable with work, but nothing upstream would use it.
- **Needed by:** [Generator scheduling](https://github.com/gismya/deluge-generative/issues/19), ADR 0002, [Pushed Lean](https://github.com/gismya/deluge-generative/issues/30) (the 12-tick lookahead is driver code; the hook just returns the wait).
- **Notes:**
  - `Arrangement::doTickForward` (`playback/mode/arrangement.cpp:110`) has no hook. Perform must stay in session playback by keeping `currentSong->lastClipInstanceEnteredStartPos = -1`, or PLAY starts arrangement playback for a non-ClipMinder root (`playback_handler.cpp:495`). The POC sets it in `focusRegained`, with no firmware change.
  - The hook only runs while a clock runs. Stop arrives through `RootUI::playbackEnded()` (`gui/ui/root_ui.h:36`, called at `playback_handler.cpp:632`), which the POC used.

### F2. `AutoParam` live ramp

- **What:** a new method, for example `AutoParam::setLiveRamp(modelStack, target, lengthInTicks)`. It sets `valueIncrementPerHalfTick` (or the float increment for float params) to `(target/2 − current/2) / ticks`, the formula `calculateInterpolationIncrement` uses (`auto_param.cpp:859-868`). It then flags the param in its collection summary's `whichParamsAreInterpolating`, as `ParamSet::checkWhetherParamHasInterpolationNow` does (`modulation/params/param_set.cpp:105-109`), or with `flagCable` for patch cables (`modulation/patch/patch_cable_set.cpp:345`). It needs a small helper on `ParamCollection` so the caller doesn't touch the summary directly.
- **What's already there:** the firmware advances flagged params once per render window, per tick at the current tempo (`AutoParam::tickSamples`, `auto_param.cpp:948-972`). The render-time pre-checks already cover every collection a sound control can target:
  - clip-level Sound params, patched, unpatched and patch cables (`processing/sound/sound_instrument.cpp:112-170`);
  - kit rows (`model/instrument/kit.cpp:597-660`), which covers per-lane controls, FX rows and the Siren's Throw;
  - kit-level (affect-entire) unpatched params (`model/global_effectable/global_effectable_for_clip.cpp:163-171`).

  They run only while a clock runs and the clip is active, and Perform's clips are always active ([#19](https://github.com/gismya/deluge-generative/issues/19)).
- **Size:** S–M (one method plus a collection helper, about 40 lines). **Upstreamable:** with work. A "glide to value over N ticks" call is a reasonable general API, but upstream has no caller.
- **Needed by:** [Generator scheduling](https://github.com/gismya/deluge-generative/issues/19) (sound-control ramps), [Automation](https://github.com/gismya/deluge-generative/issues/14), [Sound controls per synth engine](https://github.com/gismya/deluge-generative/issues/26) (zipper on params the single `paramLPF` slot doesn't smooth), [Generative pack contents](https://github.com/gismya/deluge-generative/issues/42) (Siren Throw on a kit row).
- **Risks:**
  - **No stop at the target.** `applyValueIncrement` only saturates at the int32 limits. A ramp keeps moving until it is rewritten, so the Generator must rewrite every control at every step (it plans to). A late write overshoots by up to one render window per tick. A target field would grow every `AutoParam` (thousands per song), so don't add one.
  - **Stop clears ramps.** `ParamSet::playbackHasEnded` resets increments on stop (`param_set.cpp:207-215`). That's what we want.
  - **A player knob turn on a param that is ramping.** `setCurrentValueInResponseToUserInput` on an unautomated param sets the value, and the ramp keeps pulling it until the next step. That's harmless, because the Generator re-homes it (F3).
  - Ticking calls `notifyParamModifiedInSomeWay(..., automatedBefore = true, automatedNow = true)` (`param_set.cpp:145`), so no automation flags change. That's the same path stock interpolation takes.

### F3. Player-edit notification for sound-control homes

- **Why it's needed:** [#19](https://github.com/gismya/deluge-generative/issues/19) says "Perform's gold-knob handler and the sound editor overlay tell the Generator a param's new value". Perform's own `modEncoderAction` sees gold-knob turns only while Perform is the current UI. With the sound editor open, knob turns go `UI::modEncoderAction` → `view.modEncoderAction` (`gui/ui/ui.cpp:33-35`, `sound_editor.cpp:1903-1927`), and menu-item edits go through `writeCurrentValue` (`gui/menu_item/patched_param/integer.cpp:38-42`, `unpatched_param.cpp:46`, `patch_cable_strength.cpp:233`). Both skip Perform.
- **What:** the cheapest single point is `AutoParam::setCurrentValueInResponseToUserInput` (`auto_param.cpp:101`), which every user-driven write goes through: gold knobs in any UI, sound editor items, MIDI learn and MIDI follow. Add a notification there, filtered to Perform's param managers. The alternative is three narrower hooks: `View::modEncoderAction` plus the three `writeCurrentValue` methods.
- **Size:** S. **Upstreamable:** no.
- **Needed by:** [Generator scheduling](https://github.com/gismya/deluge-generative/issues/19), [Automation](https://github.com/gismya/deluge-generative/issues/14) (provisional home rule), [Perform control layout](https://github.com/gismya/deluge-generative/issues/38) (a part's stock Sound pages).
- **Open:** the single point also catches MIDI-learned and MIDI-follow edits, which [#19](https://github.com/gismya/deluge-generative/issues/19) says get overwritten. Either filter them out, or let them set home too. The second is arguably better, and it costs nothing.
- **No firmware change for the gold knobs themselves:** Perform wraps `view.modEncoderAction` and asks `ModControllable::getParamFromModEncoder` (`model/mod_controllable/mod_controllable.h:49`, public) which param the knob drives.

### F4. Tempo encoder hook

- **What:** in `hid/encoder_input.cpp:137-151` (case 2, tempo), next to the Quantize branch, send the turn to Perform when **`getRootUI()`** is Perform. Use the root, not the current UI, so tempo steering works under the sound editor, the Perform menu and the load browser. Perform then decides each combination:
  - plain turn = Recipe tempo, gliding;
  - SHIFT = swing;
  - push + turn = coarse/fine, as stock;
  - **swallow LEARN + turn**, which changes the tick magnitude (clock-out scale, `playback_handler.cpp:2198-2226`);
  - X + turn = nudge, which is probably harmless and can pass through;
  - under an external clock, show EXT, as [#38](https://github.com/gismya/deluge-generative/issues/38) says.
- **Size:** S. **Upstreamable:** with work, as a `RootUI::tempoEncoderAction` virtual whose default calls `playbackHandler.tempoEncoderAction`. That would also absorb the hard-coded Quantize branch.
- **Needed by:** [Deluge UI conventions and free controls](https://github.com/gismya/deluge-generative/issues/33), [Perform control layout](https://github.com/gismya/deluge-generative/issues/38) (TEMPO / SHIFT + TEMPO), [Perform interaction model](https://github.com/gismya/deluge-generative/issues/15) (glide).
- **Not covered by the hook:**
  - **TAP and MIDI TAP** set song tempo directly (`view.cpp:144-180`; MIDI at `playback_handler.cpp:2952-2958`, only when the current UI is the root). The Recipe would then hold a stale tempo, and Recipe files and Capture logs would miss the change. The cheapest fix is driver code: each step, if song tempo differs from what the driver last wrote, adopt it as the new Recipe tempo with no glide. That needs no firmware change, but no ticket says what TAP does to the Recipe.
- **No firmware change for the glide itself:** the driver calls `Song::setTempoFromNumSamples(samples, false)` (`model/song/song.cpp:3111`, public) once per tick. Stock already changes tempo from inside the tick: `Song::updateBPMFromAutomation` (`song.cpp:6086-6094`, called from `arrangement.cpp:127`). Don't use `Song::setBPM` (`song.cpp:3127-3164`). It writes `UNPATCHED_TEMPO` through the live-input path, which can create undo actions and reads arranger UI state.

---

## B. Root UI and sound editor

### F5. Register the Perform root UI and the SHIFT+SONG entry

- **What:**
  - a `UIType` value (`src/definitions_cxx.hpp:187-213`);
  - its `uiTypeMap` entry (`gui/ui/ui.cpp:476-502`). On `generative` this map is compiled when `ENABLE_MATRIX_DEBUG || ENABLE_SYSEX_TEST`, so test builds need the entry;
  - a SHIFT+SONG branch in `SessionView::buttonAction` before the unshifted SONG branch (`gui/views/session_view.cpp:238-245`). It opens the Generative menu ([#50](https://github.com/gismya/deluge-generative/issues/50)), not Perform directly as in the POC.
- **Evidence:** the POC made exactly these three changes (+1, +1, +11 lines).
- **Size:** S. **Upstreamable:** no.
- **Needed by:** [Firmware architecture](https://github.com/gismya/deluge-generative/issues/2), [Entering Generative mode](https://github.com/gismya/deluge-generative/issues/39), [Compose screen and Generative menu prototype](https://github.com/gismya/deluge-generative/issues/50).

### F6. Sound editor edits a Perform part's Sound

- **Why:** `SoundEditor::setup` ignores its `clip` argument whenever the root UI isn't a ClipMinder and edits the song's `globalEffectable` instead (`sound_editor.cpp:1953-1961`). That same predicate, `rootUIIsClipMinderScreen()` (`gui/ui/ui.cpp:161-164`), picks the shortcut table for SHIFT + pad (`:1421`, song table for non-ClipMinder roots), the shortcut blink (`:1106`), the model stack (`:2311`) and the horizontal-menu chain (`:2387`).
- **What:** at those five sites, use a predicate the root UI can answer, such as a `RootUI` virtual "edits clip sound", defaulting to `toClipMinder() != nullptr`. Perform returns true while a part is focused. Perform also sets the song's current clip to the part's clip, and for Drums/FX lanes sets `kit->selectedDrum` (`:1971-1990`) or `setupKitGlobalFXMenu`.
- **Don't make Perform a ClipMinder.** `rootUIIsClipMinderScreen()` at `:597` sets `isUIInstrumentClipView`, which turns on SAVE-as-preset (`:713-727`), the KEYBOARD root swap (`:789-826`), and entry to automation view (`gui/menu_item/automation/automation.cpp:64-83`). It also changes PLAY's playback choice (`playback_handler.cpp:495-516`) and stem export (`processing/stem_export/stem_export.cpp:144`). All of those would act on Perform's root.
- **Size:** M. **Upstreamable:** with work. The predicate is marked "this check seems very sketchy" in the source (`:1105`), and a virtual is cleaner, but upstream has no second user.
- **Needed by:** [Perform interaction model](https://github.com/gismya/deluge-generative/issues/15) (sound editor overlay), [Perform control layout](https://github.com/gismya/deluge-generative/issues/38) (SHIFT + main pad for the focused part, CLIP page SOUND EDITOR).
- **With nothing focused,** the song-FX path works as it does for PerformanceView, and needs no change.

### F7. Sound editor cases for Perform

- **What:**
  - **Greyout:** add the Perform type next to `UIType::PERFORMANCE` (`sound_editor.cpp:477-509`, case at `:502-504`). Otherwise the default greys the whole grid.
  - **Pads:** forward pads to Perform as for PerformanceView (`:1777-1780`). Otherwise any pad closes the menu (`:1783-1810`).
  - **SONG button:** in the menu it calls `changeRootUI(&sessionView/&arrangerView)` (`:829-841`). From Perform that leaves without cleanup. Ignore it, or route it to Perform.
  - **Default menu:** Perform always passes an explicit item (its own menu, a part's sound menu or song FX), so the Settings fallback (`:2082-2091`) is never reached and needs no change.
- **Size:** S. **Upstreamable:** no, though it's trivial on top of F6's virtual.
- **Needed by:** [Perform control layout](https://github.com/gismya/deluge-generative/issues/38) ("Pads, PLAY and knobs stay live; BACK closes"), [Perform interaction model](https://github.com/gismya/deluge-generative/issues/15).
- **Also in the sound editor:** the Perform menu (SELECT push) and the Compose screen are new `MenuItem` trees opened with `soundEditor.setup(nullptr, &item)`. Values right-aligned on rows already have a per-item hook (`MenuItem::renderSubmenuItemTypeForOled` / `getSubmenuItemTypeRenderLength`, `gui/menu_item/menu_item.h:286-289`; precedent `Selection`, `selection.cpp:53`). In-place editing is a `Submenu` subclass. All of that is new code. Title side-scrolling doesn't run while playing (`hid/display/oled.cpp:1065`), the same as the stock song name.

### F8. MIDI follow target in Perform (unanticipated)

- **What:** `MidiFollow::getSelectedClip` (`io/midi/midi_follow.cpp:420-454`) falls back to `getCurrentClip()` for unknown root types. In Perform, a MIDI controller's CCs would edit whichever part's clip is current, and its notes would play into that part's Output. Those notes aren't Generator events, so Capture never sees them.
- **Size:** S. **Upstreamable:** no.
- **Open:** route MIDI follow to the focused part (CCs then set homes through F3), or switch it off in Perform. No ticket covers external MIDI input in Perform.

### F14 (optional). Perform keeps its page LEDs

- `View::setModLedStates` (`gui/views/view.cpp:1570-1700`) rewrites the CLIP, SONG and AFFECT ENTIRE LEDs from the knob context. It is called from 26 sites, including `View::setActiveModControllableTimelineCounter` (`:1906`, `:1931`), sound editor exits and `playback_handler.cpp:322, 623, 628`. Perform uses SONG/CLIP/KEYBOARD to show the grid page, and AFFECT ENTIRE for "Recipe level" ([#38](https://github.com/gismya/deluge-generative/issues/38)).
- **No change if** Perform re-asserts its LEDs in `focusRegained` and after each `view` call. Otherwise add an S case: skip those three LEDs when the root is Perform. **Upstreamable:** no.

---

## C. Buttons, global commands and undo

When Perform is the current UI, its `buttonAction` sees every button before the global fallbacks (`hid/buttons.cpp:141-209`). Most of [#38](https://github.com/gismya/deluge-generative/issues/38)'s button list needs no firmware change because of that. Plain RECORD (Capture mark), SHIFT + RECORD (passed through to resample, `:188-190`), SYNC-SCALING as Fill, PLAY and SHIFT + PLAY, X + PLAY blocked, and the BACK tap are all Perform's own code. BACK hold calls `getCurrentUI()->exitUI()` (`gui/ui_timer_manager.cpp:162-167`), a virtual (`gui/ui/ui.h:154`), so the leave prompt is an override too. The gaps are overlays and MIDI.

### F9. RECORD and restart guarded when Perform is root

- **Why:** when an overlay is open (sound editor, Perform menu, Recipe browser), PLAY and RECORD go to the overlay. It returns `NOT_DEALT_WITH`, so the stock fallback runs:
  - **RECORD** calls `playbackHandler.recordButtonPressed()` (`playback_handler.cpp:280-292`), which starts clip recording. While recording, knob turns write automation nodes into Perform's clips (`auto_param.cpp:101-130`). Those nodes then fight the Generator's ramps, which breaks [#14](https://github.com/gismya/deluge-generative/issues/14)'s "never recorded".
  - **X + PLAY** restarts playback (`:213-255`).

  MIDI RECORD and PLAY commands call the same functions (`:2914-2920`).
- **What:** early returns in `recordButtonPressed` and in the restart branch of `playButtonPressed` when the root is Perform. MIDI RECORD could become the Capture mark instead.
- **Size:** S. **Upstreamable:** no.
- **Needed by:** [Perform control layout](https://github.com/gismya/deluge-generative/issues/38) (RECORD = Capture mark, X + PLAY blocked), [Capture](https://github.com/gismya/deluge-generative/issues/35), [Automation](https://github.com/gismya/deluge-generative/issues/14).

### F10. Global MIDI commands guarded in Perform (unanticipated)

- **What:** `PlaybackHandler::tryGlobalMIDICommands` (`playback_handler.cpp:2893-2965`) acts whatever the root UI. In Perform:
  - **NEXT_SONG** (`:2943-2945`) loads another song under Perform's root;
  - **LOOP / LOOP_CONTINUOUS_LAYERING** (`:2922-2925`) overdub;
  - **PLAYBACK_RESTART** (`:2908-2912`) restarts. The POC driver survives it by restarting the Generator;
  - **UNDO/REDO** are pended and run in `slowRoutine` (`:162-191`), see F11.

  Guard them for Perform in the same place as F9. FILL already works (`:2939-2941`).
- **Size:** S. **Upstreamable:** no.
- **Open:** whether MIDI PLAYBACK_RESTART is blocked like X + PLAY, and whether MIDI RECORD marks Capture.

### F11. No undo logging or reversion in Perform

- **Why:** `ActionLogger::getNewAction` logs actions whenever the current UI is the root (`model/action/action_logger.cpp:86-100`). Gold-knob turns on Perform's song and parts, and tempo changes, create actions with `view = Perform`. A MIDI UNDO (F10), or SHIFT + BACK if it's ever forwarded to `view.buttonAction`, reverts them. Then `revertAction` casts the current UI to `TimelineView*` for scroll and zoom animations (`:565`, `:580`), which is undefined behaviour for Perform. The POC wiped logs on entry and exit; the POC findings route "Undo inside Perform" here.
- **What:** `getNewAction` returns null and `allowedToDoReversion` (`:867-869`) returns false while Perform is root.
- **Size:** S. **Upstreamable:** no.
- **Needed by:** [Perform control layout](https://github.com/gismya/deluge-generative/issues/38) (a BACK tap cancels a queued action; there's no undo), POC findings.

---

## D. Song construction and storage

### F12. `storage_manager.cpp:406`

See *Upstreamable fixes that can go first*. **Not needed in v1:** whole-kit picks are out of scope ([Kit lane mapping](https://github.com/gismya/deluge-generative/issues/25), [#13 amendment](https://github.com/gismya/deluge-generative/issues/13)). Drums are assembled lane by lane, and FX presets go through `loadSynthToDrum`, which reads no `currentSong`. Perform builds privately now ([#39](https://github.com/gismya/deluge-generative/issues/39)), so a card KIT load into Perform's private song would hit this bug too, if whole kits came back.

### F13 (optional). Song save without the save UI (Capture)

- **What Capture needs:** each built song is swapped in stopped and saved "through the normal save path for the current song" ([#35](https://github.com/gismya/deluge-generative/issues/35)), with no QWERTY, while the OLED shows `SAVING 2 OF 3` ([#50](https://github.com/gismya/deluge-generative/issues/50)).
- **Why the stock path doesn't fit:**
  - `SaveSongUI::performSave` (`gui/ui/save/save_song_ui.cpp:117-528`) reads the browser statics `enteredText`, `currentDir` and `collectingSamples`, may open the overwrite context menu (`:144-158`), and ends with `close()` on itself.
  - `Song::writeToFile` reads UI state. The pad preview comes from `PadLEDs::imageStore` (`model/song/song.cpp:1177-1187`), which `SaveSongUI::opened` fills (`save_song_ui.cpp:57`). `inArrangementView` comes from `getRootUI() == &arrangerView` (`song.cpp:1189`).
- **No change if** new code does what the core of `performSave` does: `StorageManager::createXMLFile` → `currentSong->writeToFile()` → `closeFileAfterWriting(path, "<?xml…<song\n", "\n</song>\n")` (`save_song_ui.cpp:467-482`), then sets `name` and `dirPath`. It must also fill `imageStore` and keep `lastClipInstanceEnteredStartPos != -1`, so the file reopens in arranger (`song.cpp:1194`). It can pick a free name with `nextDefaultName` (see N9). Generated songs load no samples from alternate paths, so the sample-moving part of `performSave` doesn't apply.
- **Optional refactor:** extract the file-writing part of `performSave` into a UI-free function. M; upstreamable with work, for example for autosave.
- **Needed by:** [Capture](https://github.com/gismya/deluge-generative/issues/35), [Compose screen and Generative menu prototype](https://github.com/gismya/deluge-generative/issues/50).

### U2 (optional). Out-of-memory freezes

`NoteRow::setDrum` freezes on allocation failure (`model/note/note_row.cpp:3748`, `:3756`), and so does `getOrCreateNoteRowForYNote` (`model/clip/instrument_clip.cpp:1108`), along with several other clip paths (`:1631`, `:1741`, `:2183`, `:2277`). A long Compose, a large Capture or a genre change near the memory limit would freeze instead of failing cleanly. [#37](https://github.com/gismya/deluge-generative/issues/37)'s numbers say v1 should fit, so this is a risk to watch, not a change to make. L; upstreamable with work.

---

## E. No firmware change needed

| ID | Item | Why no change | Source |
|---|---|---|---|
| N1 | Load a whole song from a memory buffer | Compose and Capture build into a private `Song*` through the model API ([#16](https://github.com/gismya/deluge-generative/issues/16), [#19](https://github.com/gismya/deluge-generative/issues/19), [#35](https://github.com/gismya/deluge-generative/issues/35)). There's no XML route, so `XMLDeserializer`'s missing memory mode doesn't matter. | `storage/Deserializer.cpp`; [#18](https://github.com/gismya/deluge-generative/issues/18) |
| N2 | Euclidean and pattern logic into the model | The Generator writes its own patterns with its own RNG ([#17](https://github.com/gismya/deluge-generative/issues/17)), and its core must build in the x86 harness, so it can't call `InstrumentClipView` anyway. No ticket uses pattern files. | [#7](https://github.com/gismya/deluge-generative/issues/7) |
| N3 | `loadSynthToDrum` error paths | Fixed in `v1.3.0`: the drum is freed if the open fails, and backed-up ParamManagers are deleted if the read fails. | `storage/storage_manager.cpp:569-588` |
| N4 | Swap a finished private song in, stopped | Every step is public: `playbackHandler.endPlayback()`, `actionLogger.deleteAllLogs()`, set `preLoadedSong`, `playbackHandler.doSongSwap(false)` (`playback/playback_handler.h:77`), delete the old song, `loadAllSamples()`, `setUIForLoadedSong()` (`deluge.cpp:368-406`, opens arranger when `lastClipInstanceEnteredStartPos != -1`). `ClearSong::acceptCurrentOption` (`gui/context_menu/clear_song.cpp:72-121`) is the template, and also the "keep nothing → blank song" exit. A shared `swapInSong()` helper is an optional S refactor. | [#16](https://github.com/gismya/deluge-generative/issues/16), [#39](https://github.com/gismya/deluge-generative/issues/39) |
| N5 | Tempo automation and arrangement-only clips from code (Capture) | `UNPATCHED_TEMPO` in the song's `paramManager` plays in arrangement (`playback/mode/arrangement.cpp:120-128`), and Capture songs open in arranger. Arrangement-only clips: `song->arrangementOnlyClips.insertClipAtIndex` + `section = 255` + `output->clipInstances.insertAtIndex` (precedents `model/output.cpp:461`, `model/clip/clip.cpp:539`). **Trap:** `AutoParam::setNodeAtPos` (`auto_param.cpp:1070-1095`) doesn't set the summary's "automated" bit, so call `notifyParamModifiedInSomeWay(..., automatedBefore = false, automatedNow = true)` or use `setValueForRegion`, or the automation never plays. | [#35](https://github.com/gismya/deluge-generative/issues/35), [#2](https://github.com/gismya/deluge-generative/issues/2) |
| N6 | Load-time settings: arp off, portamento 0, voice priority, voice mode, CHOKE | All are plain writes on a private song: clip `arpSettings` after `setAudioInstrument` (it clones the preset's arp at `model/clip/instrument_clip.cpp:2310-2316`); `UNPATCHED_PORTAMENTO` with `setCurrentValueBasicForSetup` in each clip's `paramManager`; `Sound::voicePriority` (`processing/sound/sound.h:152`, saved at `sound.cpp:4177`); `Sound::polyphonic` = `LEGATO` or `CHOKE` (`sound.h:120`, `src/definitions_cxx.hpp:436-442`). The POC did the arp and portamento ones this way. Compose sets them in **every** clip, because each clip carries its own params. | [#42](https://github.com/gismya/deluge-generative/issues/42), [#13](https://github.com/gismya/deluge-generative/issues/13), [#12](https://github.com/gismya/deluge-generative/issues/12) |
| N7 | Siren: a synth preset held in a kit row | `loadSynthToDrum` on the target song, then the Generator's own version of the follow-up in `LoadInstrumentPresetUI::performLoadSynthToKit` (`gui/ui/load/load_instrument_preset_ui.cpp:1055-1146`, which uses `currentSong`). A held note is `noteOnPreKitArp` … `noteOffPreKitArp` on an ordinary `Sound`. LFO1 is the sound's global LFO (`sound.cpp:2404`), the release is the preset's, and the Throw ramp is ticked per row (F2). Expected to work; check it by ear in delugemu. | [#42](https://github.com/gismya/deluge-generative/issues/42) |
| N8 | Drum scan: tags, exact paths, `GEN` samples in factory folders | Generator code over FatFs `f_opendir`/`f_readdir`, as `Browser::surveyNameVariations` does (`gui/ui/browser/browser.cpp:864`). One card operation at a time, because `smDeserializer` is global (`storage/storage_manager.cpp:66`). | [#42](https://github.com/gismya/deluge-generative/issues/42), [#25](https://github.com/gismya/deluge-generative/issues/25) |
| N9 | Recipe files: quick save, save and load browsers on `RECIPES/`, a file-exists check per pick | XML through `StorageManager::createXMLFile` + `closeFileAfterWriting`. Folder through `Browser::createFoldersRecursiveIfNotExists` (`gui/ui/browser/browser.h:77`). Next letter variation through `deluge::gui::browser::nextDefaultName` (`gui/ui/browser/default_name.h`) over a `FileListView` built on the static `Browser::surveyNameVariations` (it reads `Browser::currentDir`, so set that first). The browsers are new `SaveUI` / `LoadUI` subclasses; copy `LoadSongUI`'s root-pad drawing (`gui/ui/load/load_song_ui.cpp:700, 816-817`), don't change it. `StorageManager::fileExists(path)` (`storage/storage_manager.h:383-384`) for each pick. | [#40](https://github.com/gismya/deluge-generative/issues/40) |
| N10 | Gold knobs and mod buttons | `View` is a global with public members (`gui/views/view.h:69-77, 120, 147`). Perform overrides `modButtonAction` / `modEncoderAction`: F1–F7 go to `view`, and F8 and the F7/F8 repeats are its own. The F1 VU toggle (`view.cpp:1537-1545`) is avoided by handling a repeated F1 press itself, or by clearing `view.displayVUMeter`. Page memory per target is stock `getModKnobMode` (`view.cpp:1553`). The song FX pushes are stock (`model/global_effectable/global_effectable.cpp:222-330`). | [#38](https://github.com/gismya/deluge-generative/issues/38) |
| N11 | BACK hold, RECORD, SYNC-SCALING, PLAY, X + PLAY, X + MUTE solo when Perform is the current UI | Perform's own `buttonAction` / `exitUI` (see C). Solo and mute are the Generator's output gate; firmware clip mute is never used ([#19](https://github.com/gismya/deluge-generative/issues/19)). | [#38](https://github.com/gismya/deluge-generative/issues/38) |
| N12 | Fill | Perform reads `currentSong->isFillModeActive()` (`model/song/song.h:272`), and the MIDI FILL command already sets it (`playback_handler.cpp:2939-2941`). | [#38](https://github.com/gismya/deluge-generative/issues/38) |
| N13 | Tick magnitude | `Song()` takes `FlashStorage::defaultMagnitude` (`model/song/song.cpp:140`), **a user setting**, so Perform's and Compose's private songs must set the magnitude explicitly, or convert as the POC does. External-clock tempo targeting changes it mid-play (`playback_handler.cpp:1890-1901`), so the driver must rebase. LEARN + tempo is blocked by F4. | POC findings ("Tick magnitude can change mid-play") |
| N14 | Sync-level defaults while building a private song | The `ModControllableAudio`, `ArpeggiatorSettings` and `SideChain` constructors take their sync level from `preLoadedSong`, else `currentSong` (`model/mod_controllable/mod_controllable_audio.cpp:72-82`, `modulation/arpeggiator.cpp:46`, `modulation/sidechain/sidechain.cpp:37`). Assembled drums built for a private song get the *playing* song's magnitude. Give the private song the same magnitude, or set the delay and arp sync after building. | [#28](https://github.com/gismya/deluge-generative/issues/28) |
| N15 | Build with progress and BACK = cancel | The stock loader runs inside the "buttons and pads" task ([#37](https://github.com/gismya/deluge-generative/issues/37)), so no input arrives during it. A Compose, Perform or Capture build has to run as its own scheduler task that yields (`addOnceTask`, `yield`, `src/OSLikeStuff/scheduler_api.h:59-86`), so buttons and the OLED keep running. One preset or sample load can't be interrupted (5–155 ms, [#37](https://github.com/gismya/deluge-generative/issues/37)). | [#39](https://github.com/gismya/deluge-generative/issues/39), [#50](https://github.com/gismya/deluge-generative/issues/50) |
| N16 | Writing notes into scale-mode clips | `getOrCreateNoteRowForYNote` calls `song->setRootNote` only for an out-of-scale note (`instrument_clip.cpp:1094-1123`). Compose writes in-scale notes, and Capture writes out-of-scale clips chromatic ([#35](https://github.com/gismya/deluge-generative/issues/35)). | [#21](https://github.com/gismya/deluge-generative/issues/21) |
| N17 | The remaining `== &performanceView` checks (23 in 9 files) | Apart from F7 and the ones covered above, they only need to treat Perform as "not PerformanceView", which is what happens by default: `view.cpp:234, 279, 870, 1534, 2762, 2843, 2864`; `session.cpp:810, 1064, 1301, 2417`; `playback_handler.cpp:211, 346`; `session_view.cpp:1934, 1972, 2049, 2491`; `arranger_view.cpp:3423`; `mod_controllable_audio.cpp:1224`; `midi_follow.cpp:1119`; `global_effectable.cpp:103` (stutter ends on a mod-button press, as stock). | [#33](https://github.com/gismya/deluge-generative/issues/33) |
| N18 | Return paths to session or arranger (`model/clip/clip_minder.cpp:28-39`, `performance_view.cpp:937-968`), stem export from an unknown root | Unreachable: Perform can't open clip views or PerformanceView ([#15](https://github.com/gismya/deluge-generative/issues/15)), and the song menu with stem export isn't offered there. | [#33](https://github.com/gismya/deluge-generative/issues/33) |
| N19 | Hardware measurements | [#37](https://github.com/gismya/deluge-generative/issues/37) found no change needed. Song loads block the button task for up to 20 ms, and pad sends wait behind the OLED. | [#37](https://github.com/gismya/deluge-generative/issues/37) |

---

## F. What the POC touched

`origin/poc/perform-techno` against its merge base `c0725ce56` (`v1.3.0-rc3`): 18 files, +2867 lines, of which 4 are existing firmware files.

| File | Lines | What |
|---|---|---|
| `src/definitions_cxx.hpp` | +1 | `UIType::GENERATIVE_PERFORM` |
| `src/deluge/gui/ui/ui.cpp` | +1 | `uiTypeMap` entry |
| `src/deluge/gui/views/session_view.cpp` | +11 | SHIFT+SONG enters Perform |
| `src/deluge/playback/mode/session.cpp` | +7 | The Generator hook after the arp loop |

`poc/core` touches no firmware file. Everything else went through public APIs from new files (`generative/`, `gui/views/generative_perform_view.*`): kit assembly (`createNewInstrument`, `createNewDrum`, `setupAsSample`, `loadFile(false, …)`), `loadInstrumentFromFile` for the bass, clip linking (`setAudioInstrument`, `setupAsNewKitClipIfNecessary`, `addOutput`, `setActiveClip`, `session.reSyncClip`), arp and portamento clearing, `noteOnPreKitArp` / `sendNote`, `allNotesOff`, and stop through `playbackEnded()`. Its workarounds were all in new code:

- `lastClipInstanceEnteredStartPos = -1` in `focusRegained`, so PLAY starts session playback (`playback_handler.cpp:495`);
- `actionLogger.deleteAllLogs()` on entry and exit, which F11 replaces;
- `editedByUser = false` before `removeSessionClip`, so a removed Output is deleted, not hibernated;
- `graphicsRoutine` must not fall through to `UI::graphicsRoutine`, which calls the root UI's, i.e. itself.

It didn't try a private song, the sound editor, the tempo encoder, sound-control ramps or Capture, so F2, F3, F4, F6–F11 and F13 have no POC evidence yet.

---

## Open questions

1. **Homes from MIDI-learned and MIDI-follow edits** (F3): set home, or get overwritten as [#19](https://github.com/gismya/deluge-generative/issues/19) says? The single hook point catches them either way.
2. **MIDI follow in Perform** (F8): focused part, or off?
3. **MIDI RECORD and PLAYBACK_RESTART in Perform** (F9, F10): Capture mark and blocked, like the buttons?
4. **TAP tempo in Perform** (F4): does a tap set the Recipe tempo (driver adopts song tempo changes it didn't make), and does it glide?
5. **Tempo turn under an external clock** (F4): ignore it, or set the Recipe tempo for when the clock stops?
6. **F13:** new save code, or refactor `SaveSongUI::performSave`? This depends on whether Capture ever needs sample collection.
7. **Page LEDs** (F14): re-assert from Perform, or a firmware case?

---

## Order hints for [Build order](https://github.com/gismya/deluge-generative/issues/44)

- **Standalone upstream PRs, whenever:** F12 and U1. Neither blocks anything.
- **First sound on hardware:** F1 + F5 (already proven by the POC), then F2 (sound controls) and F4 (tempo).
- **Before beta:** F9, F10 and F11 (the safety guards: no accidental recording, song loads or undo under Perform), F6 + F7 (sound editor overlay), F3 (homes), F8.
- **With Capture:** F13 (or the new-code route), and N5's `setNodeAtPos` trap.
