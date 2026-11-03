# Compose song construction path

Research for [#18](https://github.com/gismya/deluge-generative/issues/18), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1). Follows on from [#2](https://github.com/gismya/deluge-generative/issues/2) (`docs/research/firmware-architecture.md` on branch `research/firmware-architecture`).

**Question:** How can Compose build a Song in v1.3.0-rc3? Should it assemble Song, InstrumentClip, NoteRow and AutoParam in memory, or write song XML and load it through the normal loader? Can a non-current Song (`preLoadedSong`) be built safely while another song is playing? Which XML elements and attributes does the loader need for a minimal valid song with Synths, Kits, notes and automation? What does each path cost in time and memory?

**Source:** the firmware source on the `generative` branch (base `v1.3.0-rc3`, commit `3ca05672`). Paths are relative to the repo root and line numbers are for this commit. The only real song file in the repo is `contrib/audio_export/multi_sample/Multisample_Template_Full_Bar.XML` (`firmwareVersion="c1.3.0"`), which is used below as a format example.

This file records facts only. It does not design the feature. Nothing here was run on hardware, so the costs in section 7 are structural, not measured.

---

## Summary

- **Both paths already exist in the firmware, and both build into `preLoadedSong`.** The SD loader builds a whole Song in memory into `preLoadedSong` while the old song keeps playing, then swaps (`gui/ui/load/load_song_ui.cpp:310-570`). "Clear song" and the boot blank song build a Song in code, also through `preLoadedSong` (`gui/context_menu/clear_song.cpp:62-115`, `deluge.cpp:381-405`). So "build a non-current Song while another plays" has precedent on both paths.
- **The model layer mostly takes its Song from the ModelStack, not from `currentSong`.** Note, NoteRow, clip, drum and automation edits work on a non-current Song. The `currentSong` reads that matter are a short list (section 3). The worst one for Compose is `StorageManager::loadInstrumentFromFile`, which checks kit drums against `currentSong` (`storage/storage_manager.cpp:406`).
- **A non-null `preLoadedSong` changes global behaviour.** The next Session launch event swaps it in, even if it is half-built (`playback/mode/session.cpp:2365-2370`). Stopping playback while a launch is scheduled swaps it in instantly (`playback/playback_handler.cpp:600-611`). Recording cannot start while it is set (`playback_handler.cpp:288`). Building into a private `Song*` and assigning `preLoadedSong` only when the song is complete avoids all three.
- **The stock "load while playing" path damages the playing song.** `AudioEngine::songSwapAboutToHappen()` calls `currentSong->deleteSoundsWhichWontSound()`, which deletes every clip that is not active on its output and every hibernating instrument (`processing/engines/audio_engine.cpp:261-268`, `model/song/song.cpp:2438-2478`). The old song can then no longer be saved intact. The step frees RAM. Building the new song does not depend on it.
- **XML route, minimum the loader needs:** a `firmwareVersion` attribute (otherwise every legacy-conversion branch runs), `timePerTimerTick` (otherwise tempo becomes 1 sample per tick), the outputs under `<instruments>` (`<sound>` / `<kit>`, matched to clips by name and folder), and per clip an `<instrumentClip>` with `instrumentPresetName`, `length`, and `soundParams` or `kitParams`. For kits, every sound drum needs a ParamManager either from the drum's `defaultParams` or from the row's `soundParams`. Details are in section 4.
- **The XML route always goes through the SD card as the code stands.** `XMLDeserializer` has no memory-buffer mode. `JsonDeserializer` does, but nothing loads a song from memory today. The memory-mode `FileWriter` is capped at one 32 KB buffer, and that is smaller than a song with a single kit.
- **`Song::writeToFile()` cannot serialize a non-current Song correctly.** It writes reverb from the live `AudioEngine`, writes the pad preview from `PadLEDs::imageStore`, reads the root UI, and `Sound::writeToFile` converts sync levels using `currentSong`. Saving only ever writes `currentSong` (`gui/ui/save/save_song_ui.cpp:476`).
- **Cost:** both routes end up with the same in-RAM objects, allocated from the same heap. The file route adds serialize, SD write, SD read and a character-by-character parse. The parser yields to the audio engine every 64 characters. If either route runs while a song plays, both songs are in RAM at once. The stock loader avoids that when stopped by deleting the old song first (`load_song_ui.cpp:341-347`).

---

## 1. How a Song comes into being today

### 1a. Loading from SD (`LoadSongUI::performLoad`, `gui/ui/load/load_song_ui.cpp:310-570`)

In order:

1. `actionLogger.deleteAllLogs()` (`:323`). Loading always wipes undo history. Arrangement playback is switched to Session first (`:325-327`).
2. `StorageManager::openDelugeFile(currentFileItem, "song")` (`:330`). This picks `smJsonDeserializer` for `.Json` names and `smDeserializer` (XML) otherwise (`storage/storage_manager.cpp:743-755`).
3. **If no clock is active**, the old song is deleted first: `deleteOldSongBeforeLoadingNew()` (`:341-347`, body at `deluge.cpp:1046-1061`). The comment says this is to avoid "RAM overfilling, fragmentation etc.". **If a clock is active**, the old song stays and `AudioEngine::songSwapAboutToHappen()` runs instead (`:348-355`). See section 2.
4. `allocMaxSpeed(sizeof(Song))`, `preLoadedSong = new (songMemory) Song()`, `paramManager.setupUnpatched()`, `GlobalEffectable::initParams` (`:357-404`).
5. `preLoadedSong->readFromFile(*activeDeserializer)` (`:410`), then `closeWriter()` (`:416`), then set `dirPath` (`:422`).
6. Samples: `loadAllSamples(false)` claims samples already in RAM (`:441`). Then `loadCrucialSamplesOnly()` if playing, else `loadAllSamples(true)` (`:444-449`).
7. Swap. If playing, `session.armForSongSwap()` (`:470`) schedules the swap at the next launch quantization (`playback/mode/session.cpp:1573-1602`), or does it at once if there is nothing to sync to. If stopped, `playbackHandler.doSongSwap()` runs directly (`:521`).
8. Delete the old Song (`:534-539`), `currentSong->loadAllSamples()` (`:544`), `markAllInstrumentsAsEdited()` (`:546`), `setUIForLoadedSong(currentSong)` (`:551`).

`doSongSwap` (`playback/playback_handler.cpp:1442-1552`):
- stops auditioning and MIDI/gate notes, and can carry the old tempo over;
- `AudioEngine::killAllVoices(true)`;
- `currentSong = preLoadedSong; preLoadedSong = nullptr` (`:1494-1496`);
- sends MIDI PGMs, reads reverb params from the new song;
- if playing, resets or resyncs the play position, calls `resyncLFOs(true)`, and enters arrangement if the song was saved there;
- sets `currentUIMode = UI_MODE_LOADING_SONG_NEW_SONG_PLAYING`.

It does not change the root UI. The caller does that with `setUIForLoadedSong` (`deluge.cpp:342-380`).

### 1b. Building in code

- **`setupBlankSong()`** (`deluge.cpp:381-405`): `new Song` into `preLoadedSong`, `setupUnpatched`, `initParams`, `setupDefault()`, `ensureAtLeastOneSessionClip()`, `currentSong = preLoadedSong`, `setUIForLoadedSong`. It is only used when there is no current song.
- **`ClearSong::acceptCurrentOption()`** (`gui/context_menu/clear_song.cpp:62-115`). This is the closest in-code precedent for Compose:
  - it ends playback if the internal clock or arrangement is running (`:63-67`);
  - it deletes the old song, or calls `songSwapAboutToHappen()` if a clock (external) is still active (`:71-77`);
  - it builds the new song into `preLoadedSong` with `ensureAtLeastOneSessionClip()` (`:79-88`);
  - it then calls `doSongSwap(isEitherClockActive())`, deletes the old song, loads samples and calls `setUIForLoadedSong` (`:90-110`).
- **`Song::ensureAtLeastOneSessionClip()`** (`model/song/song.cpp:380-471`) is the only in-firmware code that assembles clip, instrument and output by hand. Its order is:
  1. `new InstrumentClip(this)`, then `sessionClips.insertClipAtIndex`, then set `loopLength` (`:391-394`)
  2. `loadInstrumentFromFile(this, firstClip, SYNTH, ...)` or `createNewInstrument` + `setupAsDefaultSynth` (`:411-448`)
  3. `loadAllAudioFiles(true)`
  4. `setAudioInstrument(newInstrument, this, true, &newParamManager)`
  5. `addOutput`
  6. `currentClip = firstClip`
  7. `session.reSyncClip(...)` if a Session clock is active
  8. `newInstrument->setActiveClip(...)` (`:450-468`)

  On allocation failure it hangs in `while (1) {}` (`:431-445`).

### Constructors: what they do and do not set

- **`Song::Song()`** (`song.cpp:137-207`):
  - sets `insideWorldTickMagnitude` from `FlashStorage::defaultMagnitude`, major-scale `modeNotes`, swing 0, `firstOutput = nullptr`, `lastClipInstanceEnteredStartPos = -1` (session), reverb defaults and `dirPath = "SONGS"`.
  - It does **not** set the tempo (`timePerTimerTickBig`). It does not set `inClipMinderViewOnLoad` (`song.h:253`), which only `setupDefault()` (`song.cpp:256`) and the clip tags `beingEdited` / `selected` set (`model/clip/clip.cpp:732-744`). It does not set up `paramManager`; the caller must call `setupUnpatched()` + `GlobalEffectable::initParams()`.
- **`Song::setupDefault()`** (`song.cpp:255-291`): random-seeded BPM, swing, key and scale from the user's default menus. `setBPM` writes the `UNPATCHED_TEMPO` song param through the live-input path (`song.cpp:3112-3135`).
- **`Clip::Clip()`** (`model/clip/clip.cpp:42-73`):
  - `activeIfNoSolo = true`, which means a new clip counts as "playing" in Session;
  - `armedForRecording = true`, `section = 0`, `launchStyle = DEFAULT`;
  - **`loopLength` is not initialized** (`clip.h:184`).
- **`InstrumentClip(Song* song = nullptr)`** (`model/clip/instrument_clip.cpp:67-104`, default argument at `instrument_clip.h:50`):
  - `inScaleMode` comes from the user's default scale setting (`:88`);
  - `yScroll` is computed from `song->key` only if a song is passed;
  - `outputTypeWhileLoading = SYNTH` (`:103`).

  The loader constructs clips with no song (`song.cpp:2365`).
- **`Clip::~Clip()`** only touches `currentSong` behind a null check (`clip.cpp:78-82`). So a clip of a non-current song can be destroyed safely. `Song::~Song()` deletes clips, backed-up ParamManagers and outputs, and calls the audio routine every 32 clips (`song.cpp:209-240`).

---

## 2. `preLoadedSong`: what a non-null value does

`Song* preLoadedSong` is a global (`deluge.cpp:95`, `song.h:486`). Readers outside the load/clear/blank code:

| Where | Effect while `preLoadedSong != nullptr` |
|---|---|
| `playback/mode/session.cpp:2365-2370` (`considerLaunchEvent`) | **Any** launch event that fires (armed clips, sections, the song-swap arm) calls `playbackHandler.doSongSwap()`. A half-built song would be swapped in. |
| `session.cpp:2153-2161` (`endPlayback`) + `playback_handler.cpp:600-611` | Stopping playback while a launch is scheduled, or in `UI_MODE_LOADING_SONG_UNESSENTIAL_SAMPLES_UNARMED`, swaps instantly. |
| `session.cpp:802-803` | Launch scheduling is not cancelled even when no clips are armed. |
| `playback_handler.cpp:287-290` | The RECORD button cannot start recording. |
| `modulation/arpeggiator.cpp:46`, `modulation/sidechain/sidechain.cpp:37`, `model/mod_controllable/mod_controllable_audio.cpp:72` | Constructors of `ArpeggiatorSettings`, `SideChain` and `ModControllableAudio` (every Sound, Kit and drum) take their default sync level from `preLoadedSong`'s tick magnitude, or from `currentSong` if it is null. |
| `playback_handler.cpp:1442-1496` (`doSongSwap`) | Assumes it is non-null and swaps it in. |

During `readFromFile` the audio routine runs many times, from `FileReader::readDone` every 64 characters (`storage/storage_manager.cpp:900-916`) and from explicit `AudioEngine::routineWithClusterLoading()` calls in `song.cpp:2142-2335`. Each of those calls can action sequencer ticks (#2 §4). So the stock loader already runs Session ticks while `preLoadedSong` is half-built. Nothing guards this except that the UI is in a loading mode.

`AudioEngine::songSwapAboutToHappen()` (`processing/engines/audio_engine.cpp:261-268`) is called by both the loader and ClearSong when a clock is active. It runs:
- `uiTimerManager.unsetTimer(PLAY_ENABLE_FLASH)`
- `currentSong->deleteSoundsWhichWontSound()`, which deletes every clip not active on its Output (except the clip under the gold knobs), any clip whose sounds will not sound, all arrangement clip instances and all hibernating instruments (`song.cpp:2438-2478`)
- `playbackHandler.stopAnyRecording()`

---

## 3. Building a non-current Song in memory: what still reads globals

Calls on the in-memory path that take the Song from the ModelStack or from an argument, and do not read `currentSong`:

- `InstrumentClip::setAudioInstrument(inst, song, ...)` (`instrument_clip.cpp:2298-2325`)
- `setupAsNewKitClipIfNecessary` / `assignDrumsToNoteRows` (`:4356-4362`, `:1999+`)
- `NoteRow::setDrum`, which uses `modelStack->song` for backed-up ParamManagers (`model/note/note_row.cpp:3700-3760`)
- `getOrCreateNoteRowForYNote`, which calls `modelStack->song->setRootNote` (`instrument_clip.cpp:1080-1130`, `song.cpp:586-626`)
- `NoteRow::attemptNoteAdd`
- `AutoParam::setNodeAtPos` / `setValueForRegion`
- `Output::setActiveClip`, `SoundInstrument::setActiveClip`, `Kit::setActiveClip` (`model/output.cpp:70-82`, `processing/sound/sound_instrument.cpp:256-282`, `model/instrument/kit.cpp:1048-1080`)
- `Song::backUpParamManager`, `setupPatchingForAllParamManagers`, `sortOutWhichClipsAreActiveWithoutSendingPGMs`

The instrument and drum readers (`SoundInstrument::readFromFile`, `SoundDrum::readFromFile`, `Kit::readFromFile`) also build their ModelStack from the `song` argument (`sound_instrument.cpp:79-86`, `sound_drum.cpp:144-150`, `kit.cpp:263-340`).

Places on or near the construction path that read `currentSong`, UI state or other globals:

| Call | Global read | Consequence for a non-current Song |
|---|---|---|
| `StorageManager::loadInstrumentFromFile` | `currentSong->getBackedUpParamManagerPreferablyWithClip(soundDrum, NULL)` for each kit drum (`storage/storage_manager.cpp:400-410`) | Loading a **KIT** preset into a non-current song checks the wrong song's backups. It returns `FILE_CORRUPTED`, or dereferences null if there is no current song. Synth presets are not affected. `ensureAtLeastOneSessionClip` only loads synths, which is why the existing path works. |
| `loadInstrumentFromFile` / `loadSynthToDrum` | They use the global `smDeserializer` (`storage_manager.cpp:320, 349, 577`) | They cannot run while a song file is being parsed through the same global reader. |
| `Song::addOutput` | `if (this == currentSong) output->resyncLFOs()` (`song.cpp:3329-3332`) | Harmless. `doSongSwap` calls `resyncLFOs(true)` when playing (`playback_handler.cpp:1532`). |
| `Song::setTimePerTimerTick` | Reschedules timer ticks only `if (currentSong == this)` (`song.cpp:2573, 2599`) | Safe on a non-current song. |
| `Song::setBPM` | Reads `currentUIMode`, `arrangerView`, `currentSong->xScroll` when the user is holding an arranger pad (`song.cpp:3121-3133`). Then writes the tempo param through `setValuePossiblyForRegion(..., pos -1, length 0)`. | The normal loader calls this too (`song.cpp:2123`). |
| `AutoParam::setCurrentValueInResponseToUserInput` (reached from `setValuePossiblyForRegion` with length 0) | "Is playing" = global clock state AND `TimelineCounter::isPlayingAutomationNow()` (`auto_param.cpp:110-112`). For a clip this is `currentSong->isClipActive(this)` (`clip.cpp:603-606`), which is true for any fresh clip (`activeIfNoSolo = true`). For the Song it is `currentPlaybackMode == &arrangement` (`song.cpp:5782-5784`). | If recording were on and the target is armed (both default true), a length-0 write into the new song would be recorded at the old song's live position. Writes with `length > 0`, and `setNodeAtPos`, do not take this path. |
| `AutoParam::setValueForRegion` | Interpolation flags from `automationView` only if it is the root UI (#2 §3) | Non-deterministic interpolation unless set explicitly. |
| `actionLogger.getNewAction` | Returns null when there is no current song, when the UI is not the root, and so on (`model/action/action_logger.cpp:88-161`) | No undo. `nullptr` actions are supported. |
| `ensureAtLeastOneSessionClip` | `session.reSyncClip(...)` if a Session clock is active (`song.cpp:463-465`) | Untested here for a non-current song. ClearSong only reaches it under external clock. |
| `InstrumentClip::transpose`, `Output::getClipFromName`, `Output::isEmpty`, `Session::reSyncClipToSongTicks`, `SessionView::createNewClip` etc. | `currentSong` (`instrument_clip.cpp:1320`, `output.cpp:57,193`, `session.cpp:834-846`, #2 §2) | Not usable for a non-current song. |
| Allocation failure | `FREEZE_WITH_ERROR` in `NoteRow::setDrum` (`note_row.cpp:3745-3756`), `getOrCreateNoteRowForYNote` (`instrument_clip.cpp:1108`), `claimOutput` (`:4024`); `while (1)` in `ensureAtLeastOneSessionClip` | Not recoverable. |

Steps that `readFromFile` and the loader perform after parsing, which an in-memory build would have to do itself (`song.cpp:2110-2336`, `load_song_ui.cpp:422-551`):

1. `setTimePerTimerTick` / `setBPM`.
2. Set `section = 255` on arrangement-only clips.
3. Per clip `claimOutput`, which matches kit rows to drums, calls `compensateVolumeForResonance` and `possiblySetupDefaultExpressionPatching` (`instrument_clip.cpp:3902-4140`).
4. Resolve clip instances.
5. `reassessWhetherAnyClipsSoloing`.
6. `setupPatchingForAllParamManagers`.
7. `sortOutWhichClipsAreActiveWithoutSendingPGMs`.
8. Set `dirPath` and `name`, then `loadAllSamples`.
9. Swap, then `markAllInstrumentsAsEdited` and `setUIForLoadedSong`.

`setUIForLoadedSong` needs `currentClip` and `inClipMinderViewOnLoad` to be set, otherwise it falls back to session or arranger view (`deluge.cpp:342-380`).

---

## 4. The XML route: what the loader requires

The entry point is `Song::readFromFile(Deserializer&)` (`song.cpp:1415-2338`). The deserializer is format-agnostic: `match('{')` / `match('[')` are no-ops in XML and real tokens in JSON. Unknown tags are skipped (`song.cpp:2091-2106`). Attributes and child tags are read the same way, so any song-level value can be either.

### 4a. `<song>` element

| Item | Status | Source |
|---|---|---|
| `firmwareVersion` | **Effectively required.** `XMLDeserializer::reset()` sets `song_firmware_version` to an empty official version (`storage/Deserializer.cpp:62`). Every `< x.y.z` legacy branch then fires: pre-1.2 ParamManager purge + resonance compensation (`song.cpp:2303-2307`), pre-2.1 linear interpolation, pre-1.5 arp rate shift, pre-3.1 swingInterval not converted (`:2111-2116`), pre-3.2 bend range forced to 12 (`instrument_clip.cpp:2990-2998`), pre-4.0 default expression patching (`sound.cpp:268-280`). The template uses `c1.3.0`. | `Deserializer.cpp:858-862` |
| `earliestCompatibleFirmware` | Optional. Song.cpp ignores the result (`song.cpp:1618-1621`). | |
| `timePerTimerTick` + `timerTickFraction` | **Required.** The default is `1 << 32`, i.e. 1 sample per tick (`song.cpp:1432`). The value is 32.32 fixed point. 120 BPM at magnitude 2 is `229` / `-1342177280` in the template. | `song.cpp:1663-1673, 2119-2123` |
| `inputTickMagnitude` | Optional. The default is `FlashStorage::defaultMagnitude` (`song.cpp:139`). Note lengths and positions in the file are in these ticks. | `:1675-1678` |
| `rootNote`, `<modeNotes><modeNote>…` | Optional. The defaults are C (`rootNote` member default) and major (`song.cpp:157`). | `:1680-1683, 1779-1793` |
| `swingAmount`, `swingInterval` | Optional. swingInterval is converted from the file encoding only if `firmwareVersion >= 3.1.0-alpha2`. | `:1685-1700, 2111-2117` |
| `<songParams>` | Optional. If absent, the `initParams` defaults set by the caller are used. | `:2068-2072` |
| `<reverb>`, `<delay>`, `<sidechain>`, `<audioCompressor>`, etc. | Optional (globalEffectable tags). | `:1444-1532, 2091-2096` |
| `currentTrackInstanceArrangementPos` | If present and not -1, the song opens and plays in Arranger. | `:1653-1656`, `playback_handler.cpp:1508, 1536-1540` |
| `preview`, `previewNumPads`, scroll/zoom attributes | Optional and cosmetic. The preview is skipped on load. | `:1622-1625` |

### 4b. `<instruments>` (outputs)

- `<sound …>` creates a `SoundInstrument` with `dirPath = "SYNTHS"`. `<kit …>` creates a `Kit` with `dirPath = "KITS"`. Also accepted: `<audioTrack>`, `<midi>` / `<midiChannel>` / `<mpeZone>`, `<cvChannel>` (`song.cpp:1979-2066`). Each output is marked `mightExistOnCard = true` (`:2024`).
- Name: the `presetName` attribute (`model/instrument/instrument.h:83`, read at `model/output.cpp:417-419`). Folder: `presetFolder` (`instrument.cpp:118-120`). These are the keys that clips match on (4c).
- Sound body: `Sound::readTagFromFileOrError` (`processing/sound/sound.cpp:412-1300`), e.g. `mode`, `polyphonic`, `osc1` / `osc2` (with `type`, and `fileName` for samples, `:668-691`), `lfo1..4`, `unison`, `modKnobs`, `defaultParams` (`:1273-1277`). Anything omitted keeps the `Sound()` constructor default. `osc` type defaults to `SQUARE` (`processing/source.cpp:42`). That is not the same as `setupAsDefaultSynth` (`sound.cpp:224-266`).
- `defaultParams` creates a ParamManager and backs it up in the Song (`sound.cpp:3266-3278`). Instruments under `<instruments>` are read with `readAutomationUpToPos = 0` (`song.cpp:2018-2020`), so **automation in an instrument's `defaultParams` is discarded**. Automation belongs to the clip.
- Kit body: `<soundSources>` holds the drums, `<sound>` (also `sample` / `synth`), `<midiOutput>` or `<gateOutput>` (`kit.cpp:268-302`). Drum order defines `drumIndex`. A drum's name is its `name` attribute (`model/drum/drum.cpp:58-60`). A drum's `defaultParams` becomes that drum's backed-up ParamManager.

### 4c. `<sessionClips>` / `<instrumentClip>`

Clips are read by `Song::readClipsFromFile` (`song.cpp:2341-2394`) and `InstrumentClip::readFromFile` (`instrument_clip.cpp:2462-3028`), with shared tags in `Clip::readTagFromFile` (`clip.cpp:696-776`). An `<instrumentClip>` needs these:

| Item | Status | Source |
|---|---|---|
| `length` (or `trackLength`) | **Required.** `loopLength` is not initialized otherwise. It is clamped to ≥ 1, and it also caps how far automation is read (`readAutomationUpToPos`). | `clip.cpp:721-726` |
| `instrumentPresetName` (+ `instrumentPresetFolder`) | **Required for Synth/Kit.** After the whole file is read, `claimOutput` looks for an output with the same type, name and folder (`strcasecmp`), searching only non-hibernating outputs. No match returns `FILE_CORRUPTED`. If the folder is omitted, it defaults to `SYNTHS` / `KITS`, the same default the output gets. | `instrument_clip.cpp:2525-2531, 3000-3016, 3902-3919`; `instrument.h:56-65`; `song.cpp:3723-3746` |
| `soundParams` (synth) / `kitParams` (kit) | **Required.** Besides holding the clip's params, this tag is how the reader learns the clip's output type: `soundParams` sets SYNTH and `kitParams` sets KIT, and the default is SYNTH. A kit clip without `kitParams` is matched against synths and fails. A synth clip without `soundParams` gets no ParamManager, because the post-2.0 path never creates one (`instrument_clip.cpp:2915-2988` only runs for pre-2.0 files). | `instrument_clip.cpp:2781-2821, 103` |
| `isPlaying` | Optional, default 1 (`activeIfNoSolo`). If several clips on one output have `isPlaying=1`, only the first one is kept active (`song.cpp:4290-4313`). | `clip.cpp:698-700` |
| `inKeyMode` | Optional. The default comes from the user's default-scale setting (`instrument_clip.cpp:88`). | `:2500-2502` |
| `beingEdited` / `selected` | Optional. These set `currentClip` and the opening view. | `clip.cpp:732-744` |
| `section`, `colourOffset`, `launchStyle`, `<arpeggiator>`, `<columnControls>` | Optional. | |

Automation in clip params (`soundParams` / `kitParams`) is read up to `length`. Expression (MPE) params are separate: `pitchBend`, `yExpression`, `channelPressure` or `expressionData` (`instrument_clip.cpp:2856-2903`).

### 4d. `<noteRows>` / `<noteRow>`

`NoteRow::readFromFile` is at `note_row.cpp:3206-3561`.

- **Melodic rows:** `y` is the MIDI note. Rows must be in ascending `y`. The reader forces `y = max(y, previous y + 1)` (`:3551-3552`).
- **Kit rows:** `drumIndex` is the index into the kit's drum list. Rows with no `drumIndex` get "no drum". A bad index falls back to the first drum, and a second row for the same drum loses its drum (`instrument_clip.cpp:3976-3992`, `kit.cpp:488-498`).
- **Kit rows need a ParamManager for every sound drum:** either the row's own `<soundParams>`, or a backed-up one from the drum's `defaultParams`. If neither exists, `claimOutput` returns `Error::UNSPECIFIED` and the song fails to load (`instrument_clip.cpp:3996-4029`). The template puts full `soundParams` on every kit row.
- **Notes:** `noteDataWithSplitProb="0x…"` with 28 hex characters per note: pos (8), length (8), velocity (2), lift (2), probability (2), iterance (4), fill (2) (`:3405-3410`, writer `:3587-3620`). Notes must be in ascending `pos` and must not overlap. A note with `pos < previous end` is dropped silently (`:3488`). Velocity 0 or > 127 becomes 64, and lift 0 or > 127 becomes the default. `noteData` (20 characters) is also accepted. `noteDataWithLift` and `noteDataWithIteranceAndFill` are only read for files with `firmwareVersion < c1.3.0` (`:3521-3533`).
- Optional: `muted`, `length` (independent row length, which also sets the row's automation read limit), `sequenceDirection`, `bendRange`, `expressionData`.

### 4e. Automation encoding

Every param value is `"0x"` + 8 hex characters for the current value, followed by 16 hex characters per node: value (8) + pos (8), where bit 31 of pos means "interpolated" (`modulation/automation/auto_param.cpp:2005-2031` writer, `:2035-2140` reader).

- Nodes must be in strictly ascending `pos`; out-of-order nodes are dropped (`:2112-2115`). Reading stops at `readAutomationUpToPos`. A node exactly at the end is moved to pos 0 (`:2118-2134`).
- Values are raw int32, the same units the in-memory route uses (#2 §3).
- `ParamSet::readParam` sets the "automated" summary bit (`modulation/params/param_set.cpp:200-206`). The raw in-memory `setNodeAtPos` leaves that bit to the caller (#2 §3).
- Param names are the ones `Sound::writeParamsToFile` writes (`sound.cpp:4056+`), e.g. `lpfFrequency`, `volume`, `<envelope1 attack=…>`. Kit and song params use the GlobalEffectable names (`model/global_effectable/global_effectable.cpp:811-1030`).

### 4f. Arrangement

- Arrangement-only clips go in `<arrangementOnlyTracks>` / `<arrangementOnlyClips>` (`song.cpp:2082-2088`).
- Placement is an output attribute, `clipInstances="0x…"`, with 24 hex characters per instance: pos (8), length (8), clip code (8) (`model/output.cpp:352-412`, writer `:274-305`).
- The clip code is the index into session clips, or with bit 31 set the index into arrangement-only clips. `0xFFFFFFFF` means an empty instance.
- An arrangement-only clip with no instance is deleted on load (`song.cpp:2283-2300`).

---

## 5. Writing a song: `Song::writeToFile()` and the save path

`Song::writeToFile()` (`song.cpp:1147-1413`) writes to the global serializer from `GetSerializer()`. XML is always used, because `writeJsonFlag` is `const false` (`storage/storage_manager.cpp:71-79`). It reads live global state:

- the pad preview comes from `PadLEDs::imageStore` (`:1166-1176`);
- `inArrangementView` is written from `getRootUI() == &arrangerView` (`:1179`);
- `beingEdited` / `selected` use the global `getCurrentClip()` and `getRootUI()` (`clip.cpp:676-682`);
- **reverb is written from `AudioEngine::reverb` / `AudioEngine::reverbSidechain`, not from the Song's own fields** (`song.cpp:1238-1270`);
- `Sound::writeToFile` writes LFO sync levels and arp params relative to `currentSong` (`sound.cpp:4200-4223, 4257`).

So `writeToFile` is only correct for `currentSong`, and `SaveSongUI` only ever calls it on `currentSong` (`gui/ui/save/save_song_ui.cpp:459-480`). `Song::writeTemplateSong` (`song.cpp:1136-1145`) goes through the same code.

`StorageManager::createXMLFile(path, smSerializer, mayOverwrite, ...)` (`storage_manager.cpp:178`) opens a file for writing. `StorageManager::fileExists(path, FilePointer*)` (`:224`) and `openXMLFile(FilePointer*, XMLDeserializer&, "song")` (`:711`) let a song be loaded from a known path without the browser. `load_song_ui.cpp:559-566` uses `fileExists` the same way for MIDI device files.

---

## 6. Reading or writing from memory instead of SD

- `FileReader(char* memBuffer, uint32_t len)` sets `memoryBased = true` and `callRoutines = false` (`storage/storage_manager.cpp:797-803`). `FileDeserializer(uint8_t*, size_t)` forwards to it (`storage_manager.h:215-219`).
- **`JsonDeserializer(uint8_t*, size_t)` exists** and its `reset()` respects `memoryBased` (`storage/JsonDeserializer.cpp:46-57`).
- **`XMLDeserializer` has only a default constructor, and its `reset()` forces cluster-read state unconditionally** (`Deserializer.cpp:51-67`). So XML cannot be parsed from a memory buffer without changing the deserializer.
- `Song::readFromFile` takes the abstract `Deserializer&`. JSON song files are loaded when the file name contains `.Json` (`storage_manager.cpp:746-749`). One reader branch is marked "NOT CONVERTED TO ALSO WORK WITH JSON": legacy `<notes>` (`note_row.cpp:3295`). Whether a full JSON song round-trips was not checked.
- In memory mode `readDone()` does **not** call the audio routine (`storage_manager.cpp:900-916`). A long in-memory parse yields only at the explicit `routineWithClusterLoading()` calls in `Song::readFromFile` / `claimOutput`.
- Memory-mode `FileWriter` has one buffer of `bufferSize = 32768` bytes (`storage_manager.cpp:926-930`). When it is full it sets `fileAccessFailedDuringWrite` and drops further bytes (`:974-985`). The only current user is SysEx JSON replies (`storage/smsysex.cpp:116-127, 471`).

---

## 7. Cost: time and memory

Nothing here was measured on hardware. The points below come from the code structure.

### Shared by both routes

- **Final RAM.** Both routes end with the same objects: Song, Outputs, Clips, NoteRows, a `ParamManager` per clip and per kit row, notes and nodes.
  - A `Note` is pos + `int32 length` + four bytes + a 2-byte `Iterance` (`model/note/note.h:24-62`), so about 12-16 bytes.
  - A ParamNode is pos, value and an interpolated flag.
  - `allocMaxSpeed` tries on-chip RAM first, then the 2 MiB general external region plus the 128 KiB small-object region, and only then the stealable sample-cache area (`memory/general_memory_allocator.cpp:172-221`; sizes at `general_memory_allocator.h:31-33`).
- **Two songs at once.** If the build happens while another song plays, both songs are in RAM. The stock loader avoids that only when stopped (`load_song_ui.cpp:341-347`). When playing, it frees RAM by gutting the old song (section 2).
- **Samples.** Sample-based kit drums load their audio in `loadAllSamples` / `loadAllAudioFiles` after construction, on either route. Samples already in RAM are claimed first (`load_song_ui.cpp:438-441`).
- **Audio starvation.** The firmware is cooperative: audio renders only when something calls `AudioEngine::routine` (`processing/engines/audio_engine.cpp:403-428`), and the SSI TX buffer is 128 samples, about 2.9 ms at 44.1 kHz (`src/definitions.h:54`). Long construction must call `AudioEngine::routineWithClusterLoading()` periodically, as `Song::readFromFile`, `~Song` and `Kit::setActiveClip` do. Those calls also action sequencer ticks (section 2).

### File route, extra cost

- **Serialize.** A generator-side writer is needed, because `Song::writeToFile` cannot write a non-current Song (section 5). The writer pushes bytes one at a time and yields every 256 bytes (`storage_manager.cpp:971-994`).
- **SD write and SD read.** The reader pulls one 32 KB cluster buffer at a time via `f_read` (`storage_manager.cpp:849-868`).
- **Parse.** Characters are read one at a time (`readChar`). Tag dispatch is `strcmp` chains, with a 4-byte switch at song level (`song.cpp:1442`). Notes cost 28 hex characters each and automation nodes 16 each. A Sound with full params is several KB: in the template, one kit drum `<sound>` block is 3,368 bytes and one kit `<noteRow>` with `soundParams` is 3,303 bytes. The 128-drum template song is 865,275 bytes.
- **Not available on this route:** no partial result (it is all or nothing), and errors come back as generic `FILE_CORRUPTED` / `UNSPECIFIED`.

### In-memory route, extra cost

- No serialize, I/O or parse step.
- The caller must reproduce the post-parse fix-ups from section 3, and must avoid the `currentSong`-reading helpers listed there.
- Allocation failure in several helpers freezes the device instead of returning an error (section 3).

---

## Open points (not verified here)

- Whether a full song round-trips through `JsonDeserializer` from a memory buffer. That is the only existing in-memory parse path.
- Whether `session.reSyncClip` (`ensureAtLeastOneSessionClip`, `song.cpp:463-465`) is safe on a clip of a non-current song while Session is playing.
- What happens at runtime to a synth clip that has no `soundParams` (no ParamManager): crash, or a later fallback.
- Real timings and RAM use for either route on hardware, e.g. for a 4-track, 8-section song.
- Whether `deleteSoundsWhichWontSound` can be skipped when swapping in a generated song while playing, and what RAM pressure then results.
