# Building Generator Kits in code

Research for [#28](https://github.com/gismya/deluge-generative/issues/28), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1). It follows [Compose song construction path (#18)](https://github.com/gismya/deluge-generative/issues/18) and [Sound generation (#13)](https://github.com/gismya/deluge-generative/issues/13).

**Question.** How does the Generator get its Kits into a Song in v1.3.0-rc3? It needs three kinds, both in the playing song (Perform) and in a private, non-current `Song*` that Compose publishes when it is complete:

1. an assembled Drums kit, one sample file per lane from `SAMPLES/DRUMS/<folder>`;
2. a whole card kit picked by the player;
3. a synth preset in an FX kit row, through `StorageManager::loadSynthToDrum`.

For each one, what does it cost (SD reads, sample loading while another song plays, allocations, yielding), and which firmware refactors does it need?

**Sources.** Firmware source on the `generative` branch at commit `1f948250` (upstream `v1.3.0-rc3` plus docs). Paths are relative to `src/deluge/`. Object sizes come from symbol sizes in `build/Release/deluge.elf` in the main checkout, built 2026-11-03 (`arm-none-eabi-nm -S`). The factory card at `~/source/delugemu-sd/sdcard` was used for file sizes. Nothing was run on hardware. Values marked **est.** are my own estimates.

---

## Summary

- **Every call needed to build a Kit already takes a `Song*` or a ModelStack.** The `currentSong` reads sit in the UI wrappers (`SampleBrowser`, `InstrumentClipView::enterDrumCreator`, `LoadInstrumentPresetUI::performLoadSynthToKit`) and in one line of `loadInstrumentFromFile`. The Generator can build an assembled kit in either song by calling the lower-level functions itself, with no firmware change.
- **Whole card kit: change one token.** `storage_manager.cpp:406` reads `currentSong->getBackedUpParamManagerPreferablyWithClip(soundDrum, NULL)`. It should read `song->`. The drum ParamManagers were filed into `song` a few lines earlier by `Sound::readFromFile` (`sound.cpp:3276`). In Perform `song == currentSong`, so the change does nothing there. The current code already works for Perform.
- **`loadSynthToDrum` is safe on a non-current song.** It reads no `currentSong`. Its error handling has three bugs (a leak, dead code, and a path that returns success with no drum). They only show on SD errors, and they bite the same in both songs.
- **Global state, not `currentSong`, is the real limit.** All XML loads share one global reader (`smDeserializer`). Sample header loads `f_open` into that same reader's file object. The alternate-folder state in `audioFileManager` is global too. So the Generator can run one load at a time and must not start one from tick code.
- **Cost.** A synth preset is about 5 KB of XML, a factory kit about 62 KB. The parser yields to audio every 64 characters. Each new sample costs a directory lookup, a FAT chain walk, one cluster read now and two start clusters queued. Half the factory drum samples fit in one 32 KB cluster. SD waits yield. Building objects in code does not yield, and it is cheap.

---

## 1. An assembled Drums kit

### 1.1 What exists today

No firmware function builds a Kit from a list of sample paths. Three UI paths each do part of it, and all three are tied to the UI:

| Path | What it does | Ties to UI / `currentSong` |
|---|---|---|
| `InstrumentClipView::enterDrumCreator` (`gui/views/instrument_clip_view.cpp:5653-5725`) | New empty kit row: `allocMaxSpeed(sizeof(SoundDrum))`, `ParamManagerForTimeline::setupWithPatching()`, `Sound::initParams`, `new SoundDrum`, `setupAsSample`, `song->backUpParamManager(newDrum, song->getCurrentClip(), …)`, `kit->addDrum`, `noteRow->setDrum`. Then opens the sample browser. | `getCurrentKit()` reads `currentSong` (`model/song/song.cpp:104-107`). `getCurrentInstrumentClip()`, `soundEditor.setup`, `openUI`. |
| `SampleBrowser::claimCurrentFile`, kit branch (`gui/ui/browser/sample_browser.cpp:848-1175`) | "Load sample into a kit row". `claimAudioFileForInstrument` sets `holder->filePath` and calls `holder->loadFile(…, manuallySelected = true, …, CLUSTER_ENQUEUE)` (`:811-823`). Sets repeat mode from length: under 2,002 ms `ONCE`, otherwise `CUT` (`:1056-1059`). Auto sidechain send for kicks (`:1177-1183`). Renames the drum and makes the name unique in the kit (`:1064-1094`). Resets osc volume (`audioFileIsNowSet`, `:1185-1204`). | Everything goes through `soundEditor.currentSound` / `currentSource` / `currentMultiRange`, plus `getCurrentKit()`, `view`, `display`. |
| `SampleBrowser::importFolderAsKit` (`sample_browser.cpp:1954-2113`) | The closest thing to a batch API. For each sample in a folder: alloc a `SoundDrum`, `setupWithPatching`, `initParams`, `getOrCreateFirstRange`, `kit->addDrum`, `setupAsSample`, `currentSong->backUpParamManager` (`:2055`), set the file path and `setAudioFile`, sidechain detection, name, repeat mode (`:2087-2088`). Then `resetDrumTempValues` and `assignDrumsToNoteRows` (`:2102-2105`). | `currentSong` at `:2055`, `soundEditor` for the first drum, `getCurrentKit()`, `getCurrentInstrumentClip()`. |

### 1.2 The calls the Generator would use

Below is what `importFolderAsKit`, `ensureAtLeastOneSessionClip` and the song loader do between them, minus the UI. Every call takes its Song from an argument or the ModelStack.

| Step | Call | Reads globals? |
|---|---|---|
| Kit and its ParamManager | `StorageManager::createNewInstrument(OutputType::KIT, &paramManager)`: `allocMaxSpeed(sizeof(Kit))`, `setupUnpatched()`, `GlobalEffectableForClip::initParams` (`storage/storage_manager.cpp:600-657`). Set `name` and `dirPath` yourself (comment at `:599`). | No. |
| Drum | `StorageManager::createNewDrum(DrumType::SOUND)`, `allocMaxSpeed(sizeof(SoundDrum))` (`storage_manager.cpp:683-708`). `sizeof(SoundDrum)` is 1,864 B in the release build. | The constructors of `ModControllableAudio` and `ArpeggiatorSettings` take their default delay and arp sync level from `preLoadedSong`, or from `currentSong` if that is null (`model/mod_controllable/mod_controllable_audio.cpp:72-82`, `modulation/arpeggiator.cpp:46-52`). For a private Song with a different tick magnitude the defaults are off. Set them after construction if it matters. |
| Drum ParamManager | `ParamManagerForTimeline pm; pm.setupWithPatching(); Sound::initParams(&pm); drum->setupAsSample(&pm);` (`processing/sound/sound.cpp:131-222`). `sizeof(ParamManagerForTimeline)` is 116 B. The param collections it allocates come on top of that, and I did not measure them. | No. |
| File it with the Song | `song->backUpParamManager(drum, clip, &pm, true)` (`model/song/song.cpp:3890`). `clip` may be null. | No. |
| Sample source | `drum->sources[0].getOrCreateFirstRange()` (`processing/source.cpp:173-188`). The default range type is `MultisampleRange` (`storage/multi_range/multi_range_array.cpp:28-29`). Set `range->getAudioFileHolder()->filePath`. Don't call `Source::setOscType`. It writes `soundEditor.currentMultiRange` (`source.cpp:271-288`), and `setupAsSample` already sets `oscType` directly (`sound.cpp:194-195`). | No. |
| Load the sample | Either `holder->loadFile(reversed, false, true, CLUSTER_ENQUEUE)` per drum (`storage/audio/audio_file_holder.cpp:34-58`), or `kit->loadAllAudioFiles(true)` once at the end (`model/instrument/kit.cpp:366-397`), which calls it for every range. Pass `manuallySelected = false`. With `true`, `SampleHolder::setAudioFile` writes `sampleBrowser.lastFilePathLoaded` (`model/sample/sample_holder.cpp:119-121`). | `Kit::loadAllAudioFiles` sets up the global alternate audio folder from the kit's `dirPath`/`name` (`model/instrument/instrument.cpp:173-184`). `shouldAbortLoading()` reads `currentUIMode` (`util/functions.cpp:1736-1739`). |
| Repeat mode, sidechain, name | `Source::repeatMode` defaults to `CUT` (`source.cpp:39`). `setupAsSample` doesn't change it. Set it after the header has loaded, from `Sample::getLengthInMSec()`, the way the browser does. Set `sideChainSendLevel` on the Kick lane yourself (`sample_browser.cpp:1177-1183`). `drumName` is a `std::string`, so it allocates from the 2 MiB external heap. | No. |
| Into the Kit | `kit->addDrum(drum)` (`kit.cpp:423-431`) just appends to the linked list. | No. |
| Clip and rows | `clip->setAudioInstrument(kit, song, true, &kitParamManager)` (`model/clip/instrument_clip.cpp:2298-2321`, through `Clip::solicitParamManager`, `model/clip/clip.cpp:917+`). Then `clip->setupAsNewKitClipIfNecessary(modelStack)` (`instrument_clip.cpp:4356-4362`) calls `assignDrumsToNoteRows`. That calls `NoteRow::setDrum`, which takes each drum's ParamManager from `modelStack->song`'s backups (`model/note/note_row.cpp:3681-3770`). | Only `display->displayPopup("E073")` in the fallback where no ParamManager is found (`note_row.cpp:3761`). |
| Link and activate | `song->addOutput(kit)` calls `resyncLFOs()` only when `this == currentSong` (`song.cpp:3328-3331`). `kit->setActiveClip(modelStack)`. | No. |

Nothing on this path reads the root UI. `display` is only touched on error paths: the `E073` popup, and `Sound::readTagFromFileOrError` showing `INSUFFICIENT_RAM` when a mod FX can't allocate (`sound.cpp:839, 851`). That second one only runs when a file is read.

### 1.3 Perform versus Compose

- **Compose (private `Song*`).** Pass the private Song to every call above and nothing touches the playing song. Samples are shared. `audioFiles` is one global, de-duplicated list, so a sample both songs use is in RAM once. Each holder adds a reason to keep it (`audio_file_holder.cpp:62-77`).
- **Perform (playing song).** Same calls with `currentSong`. The stock UI already does all of this live. `Song::renderAudio` walks `firstOutput` and skips outputs whose `inValidState` is false (`song.cpp:2501-2512`). That flag is only set by `setActiveClip` / `setupWithoutActiveClip` (`model/output.cpp:32-86`). So a new Kit in the output list isn't rendered until it has an active clip. A Kit kept off the list until it's complete is safer still. Its backed-up ParamManagers sit in `currentSong`. If a preload is abandoned, call `kit->deleteBackedUpParamManagers(song)` before freeing it, as `loadInstrumentFromFile` does (`storage/storage_manager.cpp:362-365`, `kit.cpp:1124-1133`).

### 1.4 Cost per lane

- **Allocations.** One `SoundDrum` (1,864 B, `allocMaxSpeed`), one set of patched/unpatched/patch-cable param collections (not measured), one `MultisampleRange` element, a `std::string` name. Per new sample there's a `Sample` object plus a cluster-pointer array, both from `allocStealable` (`storage/audio/audio_file_manager.cpp:786-800`). Then the 32 KB data clusters, in the stealable region too. When RAM is tight, stealable allocations evict cached clusters, possibly the playing song's.
- **SD reads, first load only.** `getAudioFileFromFilename` (`audio_file_manager.cpp:481-907`): `f_open` on the path (directory reads), a walk of the FAT chain for every cluster of the file (`:829-846`), then the WAV/AIFF header, read through `SampleReader` with `CLUSTER_LOAD_IMMEDIATELY` (`model/sample/sample_reader.cpp:41-53`). `setAudioFile` then claims the two start clusters with `CLUSTER_ENQUEUE`, and the load-clusters task reads them (`sample_holder.cpp:156-189`). On the factory card, 109 of 213 drum samples are ≤ 32 KB and 160 are ≤ 64 KB (median 31 KB, max 352 KB). For most drums, loading the start clusters reads the whole file. A sample already in memory costs only a search of `audioFiles`.
- **Yielding.** SD waits yield to the scheduler, so audio and ticks keep running (#3). `Source::loadAllSamples` calls `AudioEngine::routineWithClusterLoading()` before the first range of every source (`source.cpp:112-125`). So `kit->loadAllAudioFiles(true)` gives audio a turn at least once per drum. Building objects in code doesn't yield. It is a few allocations and param writes per lane, **est.** well under 1 ms each. If the Generator calls `holder->loadFile` itself, it should call `routineWithClusterLoading()` between lanes.
- **Est. for a 12-lane kit with cold samples:** 12 small allocations of about 2 KB plus param sets, 12 file opens and FAT walks, 12 to 24 cluster reads. Not measured.

## 2. A whole card kit

### 2.1 The bug

`loadInstrumentFromFile(song, …, OutputType::KIT, …)` (`storage_manager.cpp:326-420`) parses the kit with `Kit::readFromFile(reader, song, clip)` (`kit.cpp:263-340`). Every drum goes through `readDrumFromFile` (`kit.cpp:343-364`), then `SoundDrum::readFromFile`, which builds its ModelStack from `song` (`processing/sound/sound_drum.cpp:144-150`), then `Sound::readFromFile`. That files the drum's ParamManager with `modelStack->song` (`sound.cpp:3276`). The kit's own ParamManager is checked against `song` (`storage_manager.cpp:371`). The drum check then asks `currentSong` instead (`:406`).

- `song == currentSong` (Perform, and every caller today): works.
- `song` is a private Song: no drum ParamManager is found in `currentSong`, so it jumps to `paramManagersMissing`, deletes the kit and returns `FILE_CORRUPTED`.
- `currentSong == nullptr`: null dereference.

### 2.2 Smallest fix

Change `currentSong->` to `song->` at `storage_manager.cpp:406`. One token. Every existing caller passes a song that is current at the time (`load_instrument_preset_ui.cpp:636, 941`, `session_view.cpp:1606`, `arranger_view.cpp:842`, `instrument_clip.cpp:3808`, `song.cpp:416, 5081`), so it changes nothing for them, and it works for Perform as is. It's a plain upstream bug fix.

Nearby, and not needed for the fix: at `:356`, `if (!fileSuccess)` tests an `FRESULT` where `FR_OK == 0`. It reports `SD_CARD` when the file closed fine and a read error happened, the opposite of what it means. It only changes which error code comes back.

### 2.3 What the Generator does after the load

`loadInstrumentFromFile` returns a Kit that isn't in any output list. Its ParamManagers are filed with `song`. `loadAllAudioFiles(mayReadSamplesFromFiles)` has run, and every caller passes `false`, so only samples already in RAM are claimed (`:416`). After that it's the same as steps 1.2 "Clip and rows" and "Link and activate", plus `kit->loadAllAudioFiles(true)` for the samples. That's what the preset browser does (`load_instrument_preset_ui.cpp:959`). To get a `FilePointer` from a path, use `StorageManager::fileExists(path, &fp)` (`storage_manager.cpp:224-241`). It opens its own `FIL`.

### 2.4 Cost

- **XML.** The 43 factory kits average 62 KB (max 95 KB), so two 32 KB clusters each, with 5 to 19 drums (median 12). The reader allocates a one-cluster buffer (#3). Parsing reads one character at a time. Every 64 reads, `FileReader::readDone` runs `routineWithClusterLoading()`, `uiTimerManager.routine()` and the OLED routine (`storage_manager.cpp:898-913`). A kit parse yields about 1,000 times (**est.**, 62 KB / 64).
- **Objects.** One Kit, one `SoundDrum` per drum, a full patched param set per drum, built from about 3.4 KB of XML per drum (#18).
- **Samples.** Same as 1.4, for every drum.

## 3. A synth preset in an FX kit row

### 3.1 Is `loadSynthToDrum` safe on a non-current song?

Yes. `StorageManager::loadSynthToDrum(song, clip, …)` (`storage_manager.cpp:559-598`) allocates a `SoundDrum`, opens the file as a synth (`openInstrumentFile`, `:300-322`) and calls `SoundDrum::readFromFile(smDeserializer, song, clip, 0)`. The read path files the ParamManager with `song` and converts sync levels with `song` (`sound.cpp:412+`, which receives `modelStack->song`). It reads no `currentSong`. The only global reads are the same constructor defaults as in 1.2 and the shared reader.

The function only creates the drum. It doesn't load samples, set the name or path, or check that a ParamManager was filed. Its one caller, `performLoadSynthToKit` (`gui/ui/load/load_instrument_preset_ui.cpp:1056-1146`), does those steps, all on `currentSong` (`:1077, 1093-1094, 1106, 1114, 1125, 1138`). The Generator needs its own version of those steps on `song`: `newDrum->loadAllSamples(true)`, set `drumName` and `path`, check `song->getBackedUpParamManagerPreferablyWithClip(newDrum, clip)`, `kit->addDrum`, then rows as in 1.2. Replacing a drum in a kit that is already playing needs the old-drum handover from `:1110-1142`. Building a fresh FX kit does not.

### 3.2 Error-path bugs (both songs)

- `:570-573`: if `openInstrumentFile` fails, `newDrum` leaks.
- `:582-592`: on failure it frees the drum and returns `error`. The `SD_CARD` branch after the `return` is dead. If parsing succeeds but `closeWriter()` fails, `error` is `NONE`. The function then returns success with `*getInstrument` unset, and the ParamManager that was filed with `song` now points at a freed drum. `performLoadSynthToKit` would then dereference a null `newDrum` (`load_instrument_preset_ui.cpp:1085`).

These only show on SD errors. A Generator caller should still initialise its out-pointer to null and check it.

### 3.3 Cost

Factory synth presets are 5 KB on average (max 11 KB, 171 files): one cluster, about 80 parser yields (**est.**). One `SoundDrum` plus a param set. 3 of 171 factory synths use samples. For those, the cost in 1.4 applies.

## 4. Constraints that apply to all three

- **One load at a time.** `smDeserializer` is a single global (`storage_manager.cpp:66`), shared by preset, kit, song and favourites loads. `getAudioFileFromFilename` opens sample files into `smDeserializer.readFIL` (`audio_file_manager.cpp:737`) and reads from `activeDeserializer` (`:763`). The loaders close the XML before loading samples, so a sequence of loads is fine. Two that overlap are not, and neither is a load during a user song or preset load. The alternate audio folder (`alternateLoadDirStatus`, `thingTypeBeingLoaded`) is global too.
- **Ticks run inside loads.** Every yield above runs the audio routine, and with it sequencer ticks (#3). Tick code must not touch a kit that is half built, and must not start a load, because the reader isn't re-entrant. UI timer callbacks also run in the middle of a parse (`storage_manager.cpp:909`).
- **Allocation failures.** `NoteRow::setDrum` freezes (`E101`, `E010`) if it can't allocate a ParamManager (`note_row.cpp:3745-3757`). The other calls return errors.
- **Names.** Clips find their output by name and folder when a song is loaded (#18). An assembled kit needs a name that is unique among the song's kits. So does a card kit loaded a second time into the same song, for example a Perform preload of the kit that is already playing. The preset browser handles this with a cloned name (`findUnusedSlotVariation`, `load_instrument_preset_ui.cpp:930-935`).

## 5. Firmware refactors

Needed:

1. **`loadInstrumentFromFile` drum check uses `song`, not `currentSong`** (`storage_manager.cpp:406`). One token. Upstreamable bug fix. Needed for card kits in Compose.

Recommended, small:

2. **Fix `loadSynthToDrum` error handling** (`storage_manager.cpp:570-592`): free the drum when open fails, return `SD_CARD` when close fails, and delete the ParamManager already filed with `song` on any failure. Upstreamable.

Not firmware changes, but code the Generator must own:

3. A song-agnostic "sample drum from path" helper built from the calls in 1.2. It could be lifted out of `importFolderAsKit` upstream. Nothing requires that.
4. A load queue that runs one card load at a time, in task context, never from tick code.

Optional: default sync levels in the `ModControllableAudio` / `ArpeggiatorSettings` constructors come from `preLoadedSong`/`currentSong`. A private Song with a different tick magnitude gets wrong defaults. Overwrite them after construction rather than changing the constructors.

## 6. Open points

- Real timings on hardware: kit parse time, cold sample open and FAT walk, and how much a 12-lane kit load delays a Perform genre swap.
- Size of a `ParamManagerForTimeline`'s param collections, so RAM per lane can be budgeted.
- Whether a Perform preload of the kit that is already playing needs a cloned name, or should reuse the playing Kit. That belongs to the Perform interaction model.
