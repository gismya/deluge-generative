# Existing firmware features the Generator can reuse

Research for [#7](https://github.com/gismya/deluge-generative/issues/7), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1).

**Question:** Which generative or variation features already exist in community firmware v1.3.0-rc3? How are they implemented and exposed? Which could the Generator drive or reuse directly instead of reimplementing them?

**Sources:** the firmware source on the `generative` branch (`3ca05672`, tag `v1.3.0-rc3` plus one docs commit) and the user docs that ship with it. `docs/community_features.md` and `CHANGELOG.md` are now stubs that point to delugecommunity.com. The same content lives in this repo under `website/src/content/docs/`, which is what is cited below as "community features doc" (`website/src/content/docs/features/community_features.mdx`) and "arp doc" (`website/src/content/docs/features/arpeggiator.mdx`). All `path:line` references are relative to the repo root at that commit.

This note describes what exists. It does not propose a design.

## Summary

| Feature | Stored | Model-level API the Generator could call | Notes |
|---|---|---|---|
| Note probability (incl. latching, sum-to-100) | per note (`Note::probability`) | yes, plain field / `NoteRow::attemptNoteAdd` | evaluated at playback in `InstrumentClip::processCurrentPos` |
| Iterance (1st, last, x-of-y, custom bitmask) | per note (`Note::iterance`) | yes, `Iterance` struct | uses clip or row `repeatCount` |
| Fill / Not-fill notes | per note (`Note::fill`) + song-wide `fillModeActive` | yes, field + `Song::changeFillMode` | |
| Row "default" probability/iterance/fill | per note row, runtime only (not saved) | field | UI copies the value onto every note in the row |
| Euclidean fill | writes notes into a row | no, UI-only (`InstrumentClipView`) | algorithm is ~10 lines; easy to copy |
| Note repeat | writes notes into a row | `NoteRow::editNoteRepeatAcrossAllScreens` | UI-oriented signature |
| Quantize / humanize | rewrites note positions | yes, `NoteRow::quantize` | destructive |
| Sequence direction (fwd/rev/ping-pong) | per clip and per note row | yes, `Clip::setSequenceDirectionMode`, `NoteRow::setSequenceDirectionMode` | saved |
| Independent row length (polymeter) | per note row (`loopLengthIfIndependent`) | yes, `NoteRow::setLength` | gives each row its own `repeatCount` |
| Arpeggiator (modes, octave modes, rhythm, step repeat, ratchet, chord) | per instrument clip, per drum, plus kit-level | settings struct + unpatched params | params are automatable |
| Randomizer (note prob, velocity spread, reverse prob, lock) | unpatched params in the clip's or kit row's ParamManager | yes, via params / automation | applies to sequenced notes too, not only arp |
| Scales, root, user scale, master transpose | per song (`Song::key`), scale mode flag per clip | yes, `Song::setScale`, `setRootNote`, `transposeAllScaleModeClips` | notes store absolute pitch, not degrees |
| Chord tables | constant data | yes, read the tables | lives in keyboard UI namespace |
| Swing | per song | yes, `Song::swingAmount`, `swingInterval` | global only |
| Random LFO shapes (S&H, random walk) | per sound (`LFOConfig`) | yes, set LFO type | |
| Fill clips / Once clips | per clip (`Clip::launchStyle`) | yes, field + `Session` launch calls | session-view only |
| Sections, launch exclusively, song macros | per clip / per song | `Session::armSection`, `View::activateMacro` | |
| Param automation | per clip ParamManager, per note row for kit rows and MPE | yes, `AutoParam::setNodeAtPos` / `setValueForRegion` | |
| Pattern files | SD card `PATTERNS/` XML | no, UI-coupled (`InstrumentClipView::pasteNotesFromFile`) | a ready-made note-data format |
| New-song randomization of tempo, swing, key, scale | flash defaults | `Song::setupDefault` | only on new song |
| Drum randomizer | loads random samples into kit rows | UI-only | file browsing, not sequencing |
| RNG | global `jcong` LCG | `getRandom255`, `random`, `CONG` | shared with audio and playback |

## 1. Per-note variation: probability, iterance, fill

### Storage

Each `Note` carries its own variation data (`src/deluge/model/note/note.h:55-61`):

- `int32_t length`
- `uint8_t velocity`
- `uint8_t probability`
- `Iterance iterance`
- `uint8_t fill`
- `uint8_t lift` (note-off velocity)

Notes are kept in a sorted `NoteVector` per `NoteRow` (`src/deluge/model/note/note_row.h:146`). All of this is saved with the song. `NoteRow::writeToFile` writes each note as a fixed-width hex record of pos, length, velocity, lift, probability, iterance (16 bits) and fill under the tag `noteDataWithSplitProb` (`src/deluge/model/note/note_row.cpp:3586-3619`).

### Probability

- Range 1..20, where 20 (`kNumProbabilityValues`) means 100% and each step is 5% (`src/definitions_cxx.hpp:709`). The UI clamps to 1..20 (`src/deluge/gui/views/instrument_clip_view.cpp:2959`).
- Bit 7 (`| 128`) marks a "latching" note: it reuses the last random outcome rolled for that probability value instead of rolling again (`src/deluge/model/clip/instrument_clip.cpp:821-831`, UI label " latching" at `instrument_clip_view.cpp:3482-3490`).
- When a probability is rolled, the opposite value is stored as well: rolling p also sets the outcome for `20 - p` to the inverse (`instrument_clip.cpp:855-859`). A latched note at the complementary value therefore plays exactly when the first note did not. This gives you either/or alternation between two notes.
- If all pending note-ons at one tick have probabilities that sum to exactly 100%, exactly one of them is chosen ("sum to 100") (`instrument_clip.cpp:743-803`).
- Notes at the same tick with the same probability value share one roll (`instrument_clip.cpp:844-862`).

All of this is evaluated in `InstrumentClip::processCurrentPos` (`src/deluge/model/clip/instrument_clip.cpp:696`, decision loop at 806-907). The order is probability, then iterance, then fill (`instrument_clip.cpp:868-899`).

### Iterance

`struct Iterance { uint8_t divisor; std::bitset<8> iteranceStep; }` (`src/deluge/model/iterance/iterance.h:26-68`).

- `passesCheck(repeatCount, ending)` returns `iteranceStep[repeatCount % divisor]` (`iterance.h:46-57`). With divisor 1..8 and any bitmask, this gives "plays on loops 1 and 3 of 4" and similar patterns, which is the "CUSTOM" iterance in the UI (community features doc 4.3.11).
- `divisor == 0` encodes FIRST (step bit 1) and LAST (step bit 2), checked against `repeatCount == 0` and the `ending` flag.
- `{0,0}` is OFF (`src/definitions_cxx.hpp:713`). There are 37 presets (`definitions_cxx.hpp:710`, table at `src/deluge/util/lookuptables/lookuptables.cpp:506`). Conversion helpers are `toInt`/`fromInt`/`toPresetIndex`/`fromPresetIndex` (`src/deluge/model/iterance/iterance.cpp:58-106`).
- The repeat count comes from `ModelStackWithNoteRow::getRepeatCount()`. It is the row's own `repeatCountIfIndependent` if the row has an independent length or direction, otherwise the clip's `repeatCount` (`src/deluge/model/model_stack.cpp:100-107`).

### Fill

`enum FillMode { OFF, NOT_FILL, FILL }` (`src/definitions_cxx.hpp:727-731`). FILL notes play only while fill mode is active and NOT_FILL notes only while it is inactive (`instrument_clip.cpp:883-894`). Fill mode is one song-wide flag, `Song::fillModeActive`, read with `isFillModeActive()` and set with `Song::changeFillMode(bool)` (`src/deluge/model/song/song.h:272`, `src/deluge/model/song/song.cpp:942-955`). The user toggles it with a MIDI command or the SYNC-SCALING button when the community setting is enabled (community features doc 4.3.4).

### Row-level defaults

`NoteRow::probabilityValue`, `iteranceValue` and `fillValue` (`note_row.h:148-150`, initialised at `note_row.cpp:61-63`) are the defaults for new notes, read by `getDefaultProbability/Iterance/Fill` (`note_row.cpp:702-717`). The UI's "set probability by row" writes the row default and then overwrites every note in the row (`instrument_clip_view.cpp:4243-4284`). These defaults are **not** written to the song file. Only the per-note values persist.

### Generator access

All fields are public. The model-level entry point for adding a note with every attribute is `NoteRow::attemptNoteAdd(pos, length, velocity, probability, iterance, fill, modelStack, action)` (`note_row.cpp:722-791`). It refuses overlapping notes, clamps length to the next note, and passing `action = nullptr` skips undo logging. `NoteRow::changeNotesAcrossAllScreens` with `CORRESPONDING_NOTES_SET_PROBABILITY/ITERANCE/FILL/VELOCITY` (`note_row.h:36-40`, `note_row.cpp:1757-1830`) edits existing notes. A Compose render could write notes carrying probability, iterance and fill, and the stock sequencer would then produce the variation on every play. Perform could set the same fields on notes it owns, or flip `changeFillMode`.

## 2. Euclidean sequencing

- **What:** hold an audition pad and turn the vertical encoder. The row is replaced with N notes spread as evenly as possible over the visible steps (community features doc 4.3.6 mentions using it with row probability).
- **Where:** `InstrumentClipView::commandEuclidean` (`src/deluge/gui/views/instrument_clip_view.cpp:7700-7729`) calls `editNumEuclideanEvents` (`instrument_clip_view.cpp:7732` onward). The encoder hookup is at `instrument_clip_view.cpp:6386-6391`.
- **Algorithm:** `note->pos = (n * numStepsAvailable) / newNumNotes * squareWidth` for n in 0..N-1, every note one step long, with the row's default probability, iterance and fill and the instrument's default velocity (`instrument_clip_view.cpp:7811-7821`). The step count comes from the current zoom (`getSquareWidth`) and the clip loop length (`instrument_clip_view.cpp:7736-7739`). There is no rotation parameter and no separate steps parameter.
- **Storage:** none of its own. The result is ordinary notes.
- **Generator access:** not as an API. It is a UI method tied to the current clip, the view's zoom and the action logger. The distribution formula is trivial, so reusing it means copying the formula, not calling the method.

## 3. Note repeat, quantize and humanize

- **Note repeat:** hold a note and turn the vertical encoder. The note's square is filled with N evenly spaced repeats. UI: `InstrumentClipView::editNoteRepeat` (`instrument_clip_view.cpp:6973`). Model: `NoteRow::editNoteRepeatAcrossAllScreens(editPos, squareWidth, modelStack, action, wrapEditLevel, newNumNotes)` (`note_row.h:229`, `note_row.cpp:1147`). It is callable from outside the view but expects a view-style "square" and wrap level.
- **Quantize / humanize:** hold an audition pad and turn the tempo encoder (community features doc 4.3.3). Model: `NoteRow::quantize(modelStack, increment, amount)` (`note_row.h:222-228`, `note_row.cpp:1636`). A positive amount pulls notes toward the `increment` grid. A negative amount jitters positions randomly ("humanize"). It is destructive and rewrites `Note::pos`. The caller must call `Clip::expectEvent()` afterwards. This is a clean model-level function the Generator could call on rows it has written.

## 4. Sequence direction and independent row length

- `enum class SequenceDirection { FORWARD, REVERSE, PINGPONG, OBEY_PARENT }` (`src/definitions_cxx.hpp:935-940`).
- **Per clip:** `sequenceDirectionMode` lives on `TimelineCounter` (`src/deluge/model/timeline_counter.h:55`), set via `Clip::setSequenceDirectionMode(modelStack, mode)` (`src/deluge/model/clip/clip.cpp:1041`).
- **Per note row:** `NoteRow::sequenceDirectionMode` (`note_row.h:139`), default OBEY_PARENT (`note_row.cpp:65`), set via `NoteRow::setSequenceDirectionMode` (`note_row.cpp:4453`), saved as the `sequenceDirection` attribute (`note_row.cpp:3583-3585`). The UI exposes it per row for synth/MIDI/CV clips (community features doc 4.5.6).
- **Independent row length (polymeter):** `NoteRow::loopLengthIfIndependent` (`note_row.h:132`, 0 means "follow the clip"), set with `NoteRow::setLength(modelStack, newLength, action, oldPos, hadIndependentPlayPosBefore)` (`note_row.cpp:2643`), saved as the `length` attribute (`note_row.cpp:3580-3582`). A row with its own length or direction keeps its own play position and `repeatCountIfIndependent` (`note_row.h:132-137`, `NoteRow::hasIndependentPlayPos` at `note_row.cpp:4335`), so iterance on that row counts that row's loops.
- **Generator access:** yes for both. These are model setters that take a model stack.

## 5. Arpeggiator

### What it does

Since c1.1 and c1.3 the arpeggiator goes well beyond up/down (arp doc, "Arpeggiator Modes" and "Pattern"):

- Presets: OFF, UP, DOWN, BOTH, RANDOM, WALK, CUSTOM (`src/definitions_cxx.hpp:523-531`).
- Note modes: UP, DOWN, UP_DOWN, RANDOM, WALK1/2/3 (weighted random walks), AS_PLAYED, PATTERN (`definitions_cxx.hpp:533-543`).
- Octave modes: UP, DOWN, UP_DOWN, ALTERNATE, RANDOM (`definitions_cxx.hpp:545-551`), plus the number of octaves.
- Step repeat, sequence length, and 51 fixed rhythm masks such as `0-00` (`src/deluge/modulation/arpeggiator_rhythms.h:82`, listed in the arp doc).
- Ratchets (amount and probability), chord probability and polyphony, bass, swap and glide probabilities, gate, velocity and octave spread (arp doc, "Randomizer").
- PATTERN note mode uses a 16-entry random permutation generated by `ArpeggiatorSettings::generateNewNotePattern()` (`src/deluge/modulation/arpeggiator.cpp:1943-1947`) and saved with the song.
- A kit arpeggiator layer chooses which kit rows get note-ons (community features doc 4.3.8, arp doc "Kit Arpeggiator").
- "Chord simulator" for kit rows uses `chordTypeIndex` (`src/deluge/modulation/arpeggiator.h:89`, used at `arpeggiator.cpp:805-864`).

### Where and how it is stored

- `class ArpeggiatorSettings` (`src/deluge/modulation/arpeggiator.h:48-147`) holds preset, mode, note mode, octave mode, numOctaves, numStepRepeats, chordTypeIndex, sync, randomizer lock state and the 16-step locked random tables.
- Owners: `InstrumentClip::arpSettings`, one per synth/MIDI/CV clip and also the kit-level arp for kit clips (`src/deluge/model/clip/instrument_clip.h:103`, `src/deluge/model/instrument/kit.cpp:789-792`), and `Drum::arpSettings`, one per kit row (`src/deluge/model/drum/drum.h:68`).
- The continuous values (gate, rhythm, sequence length, chord polyphony, ratchet amount, all probabilities, spreads) are **unpatched params** `UNPATCHED_ARP_GATE` … `UNPATCHED_SPREAD_VELOCITY` (`src/deluge/modulation/params/param.h:190-206`). Each render they are copied into the settings struct by `ArpeggiatorSettings::updateParamsFromUnpatchedParamSet` (`arpeggiator.cpp:1973-1988`). Because they are params, they live in the clip's ParamManager (synth) or the row's ParamManager (kit row), so they can be automated, learned to gold knobs and driven by MIDI follow (arp doc; `src/deluge/io/midi/midi_follow.cpp:213-232`). The arp doc notes they are not automatable in MIDI clips.

### Generator access

The enum fields can be set directly on the settings struct, then `updatePresetFromCurrentSettings()` (`arpeggiator.cpp:1949`). The numeric params can be set or automated through the ParamManager like any other param (section 9). The arp only runs on notes that reach the instrument (sequenced, auditioned or MIDI in), so a Generator that emits held chords could let it do the figuration.

## 6. Randomizer for sequenced notes (non-arp)

- **What:** a `Randomizer` submenu in the sound menu. When the arp is OFF it still applies `Note probability`, `Velocity spread`, `Reverse probability` and `Lock` to every sequenced note (community features doc 4.3.9, arp doc "Randomizer"). Lock rolls 16 values once and replays them in a cycle, which makes the randomness repeatable until a parameter changes.
- **Where:** `Arpeggiator::noteOn` takes the arp-off branch, calls `calculateRandomizerAmounts` and gates the note on `isPlayNoteForCurrentStep`, with velocity spread applied (`src/deluge/modulation/arpeggiator.cpp:343-387`). The kit-row equivalent is `ArpeggiatorForDrum::noteOn` (`arpeggiator.cpp:157-224`). Rolling and locking happen in `ArpeggiatorBase::calculateRandomizerAmounts` (`arpeggiator.cpp:925-1044`). Defaults are set at `src/deluge/model/mod_controllable/mod_controllable_audio.cpp:114-125`.
- **Stored:** as unpatched params (same ParamManager as the arp params), so per clip for synths and per kit row for kits. The lock flag is in `ArpeggiatorSettings::randomizerLock` (`arpeggiator.h:96`).
- **Generator access:** yes, via param values or automation. This sits on top of note-level probability, so a note can be filtered by both.

## 7. Scales, key and transposition

- **Scale table:** 7-note (Major … Arabian), 6-note (Whole tone, Blues), 5-note (Pentatonic minor, Hirajoshi) scales are defined by `DEF_SCALES` in `src/deluge/model/scale/preset_scales.h`. The enum `Scale` has `NUM_PRESET_SCALES`, `USER_SCALE` and `RANDOM_SCALE` (`preset_scales.h:73-85`). Notes per scale are in `presetScaleNotes[]` (`preset_scales.h:96`). `NoteSet` is a 12-bit pitch-class set with `degreeOf`, `operator[]`, `scaleSize`, `majorness`, `toImpliedScale` and `modulateByOffset` (`src/deluge/model/scale/note_set.h`).
- **Stored:** per song. `Song::key` is a `MusicalKey { NoteSet modeNotes; int16_t rootNote; }` (`src/deluge/model/song/song.h:225`, `src/deluge/model/scale/musical_key.h`). The song also stores `disabledPresetScales` ("active scales", `song.h:226`), `userScaleNotes` (one learned user scale, `song.h:469`) and `masterTransposeInterval` (`song.h:425`). Each instrument clip only has an `inScaleMode` flag (`src/deluge/model/clip/instrument_clip.h:107`).
- **Notes are absolute.** `NoteRow::y` is a MIDI note number (`note_row.h:117,127`), not a scale degree. Changing scale moves existing notes: `Song::setScale` → `setScaleNotes` computes a reversible mapping with `ScaleMapper::computeChangeFrom` and applies it to every scale-mode clip via `replaceMusicalMode` (`src/deluge/model/song/song.cpp:3032-3074`, `song.cpp:659`, `src/deluge/model/scale/scale_mapper.h`). It fails if the song already uses more pitch classes than the new scale has.
- **Other setters:** `Song::setRootNote` (`song.cpp:586`), `Song::transposeAllScaleModeClips(offset, chromatic)` / `(interval)` for master transpose (`song.cpp:477`, `song.cpp:547`), `Song::learnScaleFromCurrentNotes`, `Song::notesInScaleModeClips` (`song.cpp:576`), and `Song::isYNoteAllowed` (`song.cpp:689`).
- **Generator access:** yes. These are Song methods. Perform could call them for key or scale moves, and Compose can pick the song key and write absolute note numbers.

## 8. Chord tables

- `struct Chord { name; NoteSet intervalSet; Voicing voicings[4]; }`, 33 named chords (`kMajor` … `kMinor13`) and quality-grouped arrays `majorChords`, `minorChords`, `dominantChords` and so on (`src/deluge/gui/ui/keyboard/chords.h:85-137`), plus `getChordQuality(NoteSet&)` (`chords.h:44`).
- These back the chord keyboard and chord library layouts (`src/deluge/gui/ui/keyboard/layout/chord_keyboard.h`, `chord_library.h`) and the chord memory column (`src/deluge/gui/ui/keyboard/column_controls/chord_mem.h`).
- **Stored:** constant data. Chord memory is per keyboard UI state.
- **Generator access:** the tables are plain `const` data and can be read directly. They live in the `deluge::gui::ui::keyboard` namespace, so using them from a non-UI module adds a dependency on a UI header.

## 9. Parameter automation

- Every param (patched, unpatched, expression) is an `AutoParam` in a `ParamManagerForTimeline`. That is one per clip, plus one per note row for kit rows and for MPE expression on melodic rows (`note_row.h:143`).
- Write API: `AutoParam::setNodeAtPos(pos, value, interpolate)`, `setValueForRegion(pos, length, value, modelStack, action)`, `setValuePossiblyForRegion`, `setCurrentValueWithNoReversionOrRecording` and `deleteAutomation` (`src/deluge/modulation/automation/auto_param.h:49-121`).
- **Generator access:** yes. Compose could write automation lanes. Perform could set current values without recording, which is how gold-knob tweaks behave.

## 10. Swing

- `Song::swingAmount` (int8, -49..49) and `Song::swingInterval` (sync level) (`src/deluge/model/song/song.h:220-221`), saved in the song (`song.cpp:1204-1205`). Applied globally by the playback handler's tick timing (`src/deluge/playback/playback_handler.cpp:1116-1171`, `1243-1252`, `1786-1793`).
- **Generator access:** yes, plain fields. Swing is song-wide only. There is no per-clip or per-row swing.

## 11. Random LFO shapes

- `LFOType::SAMPLE_AND_HOLD` and `LFOType::RANDOM_WALK` (`src/definitions_cxx.hpp:397-405`), implemented in `src/deluge/modulation/lfo.h:66-95`. Random walk has a bias back toward zero.
- **Stored:** per sound in `Sound::lfoConfig[LFO_COUNT]` (`src/deluge/processing/sound/sound.h:99`). Four LFOs since c1.3 (community features doc 4.5.9).
- **Generator access:** set the LFO type and patch it to a destination for timbral movement that needs no extra CPU from the Generator.

## 12. Clip launch behaviour: Fill clips, Once clips, sections, macros

- **Launch style:** `enum class LaunchStyle { DEFAULT, FILL, ONCE }` (`src/definitions_cxx.hpp:1120`), stored per clip as `Clip::launchStyle` (`src/deluge/model/clip/clip.h:197`) along with `fillEventAtTickCount` (`clip.h:198`). A FILL clip schedules itself to *end* at the next loop boundary. A ONCE clip plays one loop and stops (community features doc 4.1.3). Logic is in `src/deluge/playback/mode/session.cpp` (e.g. 325, 516, 648, 1011) and `Session::scheduleFillClip` (`src/deluge/playback/mode/session.h:114`).
- **Sections:** `Clip::section` (`clip.h:168`). Launch via `Session::armSection(section, latency)`, `armNextSection`, `userWantsToArmClipsToStartOrSolo` and `toggleClipStatus` (`session.h:40-76`). "Launch exclusively" sections stay independent of other launches (community features doc 4.2.10).
- **Song macros:** `Song::sessionMacros` (`std::array<SessionMacro, 8>`, `song.h:259`), with kinds CLIP_LAUNCH, OUTPUT_CYCLE and SECTION (`song.h:88-99`), fired by `View::activateMacro(y)` (`src/deluge/gui/views/view.cpp:2928`).
- **Generator access:** these are the existing live-arrangement tools. Perform could use the `Session` arming calls to switch prepared clips on bar boundaries, and Compose could set `launchStyle`, `section` and macros in the rendered song. The FILL/ONCE limitation of one clip per output still applies (community features doc 4.1.3).

## 13. Pattern files

- Save or load all notes on screen, with velocity, probability, lift, iterance and fill, as XML under `PATTERNS/MELODIC` or `PATTERNS/RHYTHMIC/{KIT,DRUM}`. Supports stretch and "paste gently" (merge). External tools convert MIDI files to this format (`website/src/content/docs/features/save_load_patterns.mdx`, community features doc 4.3.14).
- **Where:** `StorageManager::loadPatternFile` (`src/deluge/storage/storage_manager.cpp:499`) calls `InstrumentClipView::pasteNotesFromFile` (`src/deluge/gui/views/instrument_clip_view.cpp:1538`). Writing goes through `copyNotesToFile` (`instrument_clip_view.cpp:1300`).
- **Generator access:** not without refactoring. Load and save are methods on the clip view and act on the current clip and screen. The file format could still be a source of seed material.

## 14. Things that randomize but are not sequencing

- **New-song defaults:** `Song::setupDefault` seeds the RNG and picks tempo, swing and root note from user-set ranges, plus a random scale from the active scales if the default scale is RANDOM (`src/deluge/model/song/song.cpp:255-275`, community features doc 4.4.2). This only runs when a song is created.
- **Drum randomizer:** loads a random sample into one, several or all active kit rows (`instrument_clip_view.cpp:1868`, community features doc 4.6.2). It is UI and file-browser code, gated by a runtime setting.
- **Stutter:** per-sound and song stutter config with reverse, ping-pong and quantize (`StutterConfig stutterConfig` at `src/deluge/model/mod_controllable/mod_controllable_audio.h:104`, community features doc 4.2.8). It is a performance effect, not a note generator.
- **Performance view:** a 16-column grid of momentary or latched FX values over song-level params (`src/deluge/gui/views/performance_view.h:41-58`, community features doc 4.1.6). It is a UI over param values, so it gives no generation logic to reuse, but it is the stock example of live, non-destructive param steering.

## 15. Random number source

All of the above use one global linear congruential generator: `#define CONG (jcong = 69069 * jcong + 1234567)` (`src/deluge/util/waves.h:5-7`), wrapped by `getRandom255()`, `getNoise()` (`src/deluge/util/functions.h:314-319`) and `random(upperLimit)` (`src/deluge/util/functions.cpp:1430`). `seedRandom()` seeds it from a hardware timer (`functions.cpp:1637`). The state is shared with audio-rate code (S&H LFO, humanize, probability rolls), so a Generator that reads it cannot get a reproducible sequence from a seed unless it keeps its own RNG state.

## Observations relevant to the Generator (facts, not design)

1. The richest reusable layer is the note data. Per-note probability (with latching and sum-to-100), iterance with an 8-step custom mask, fill and NOT_FILL, velocity and lift are all evaluated by the stock sequencer. Notes written by Compose would keep varying after hand-off with no Generator code running.
2. Arp and randomizer settings are ordinary params, so they are automatable and saved per clip or per kit row. Driving them costs a param write.
3. Euclidean and pattern load/save exist only as UI methods. Quantize/humanize, note add, note editing, sequence direction, row length and scale changes have model-level entry points that take a `ModelStack`.
4. Pitch is stored as absolute MIDI notes. Scale changes rewrite notes song-wide through `ScaleMapper`, and only for clips in scale mode.
5. Swing and fill mode are song-global. There is no per-clip swing.
6. The RNG is one global LCG with no per-feature state.
