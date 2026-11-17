# Implementation notes: Firmware adapter and the private song (`generative/firmware/`)

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- The Perform adapter (`perform_adapter.h`) is split by area: `perform_adapter.cpp` (the tick, start and stop, the
  transport, the transition calls and the steering's adapter), `output_sink.cpp` (`OutputSink`: events into the
  roster's Outputs), `entry_leave.cpp` (`EntryJob`, `LeaveJob`, the Generator blocks and the song swap) and, in test
  builds, `adapter_timing.cpp` (the H1, H2 and H16 timing and `gen_mem`), which the other three share.
- A Sound's patcher works out its levels when its clip becomes active (`link()` → `setActiveClip()` →
  `performInitialPatching()`), and a later `setCurrentValueBasicForSetup()` on a patched param doesn't tell it: the
  Sound plays on at the old value. `applySoundSettings()` writes after `link()`, so it patches again when it scales the
  volume (without it a part's `.volume` shows in `gen_sound vol=` but never reaches the audio). A new patched
  load-time setting does the same. `gen_sound_level` (test builds) reports the patcher's final volume against the
  preset's, and the probe scripts check a `.volume = 50` part plays `lower`.
- Perform's song is built while another song is current. Drum, arp and sidechain constructors take sync defaults
  from `preLoadedSong`, else `currentSong`, so `applySyncDefaults()` (`perform_song.cpp`) resets them. `preLoadedSong`
  is set only at the swap: a pending launch event could otherwise swap in a half-built song.
- `doSongSwap()` leaves `currentUIMode` and the working animation set. Clear both after it, as `PerformAdapter::swapIn()`
  (`entry_leave.cpp`) and stock Clear song do.
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
- A preset's control ranges (spec §11.1) travel with its Output: the core's loaders hand back the loaded entry's
  `ControlRanges` in `SoundLoad`, `loadSynth()`/`buildRow()` store them in `PartOutput::ranges` (a synth's at 0, an FX
  lane's synth at its row), and `SoundControls::forEachTarget()` scales each target with `scaledRange()` as it binds.
  `bindPart()` and `leadParam()` both go through `forEachTarget()`, so a control at 0 binds nothing and its CLIP fader
  and F7 knob find nothing. `leadParam()` stops it at the first target with a param (its callback returns false).
  `rangesOf(Lane::None)` gives a Kit's kit-wide set the defaults, whatever FX lane row 0 loaded. They are not looked up
  by path at bind time: the palette can change under a playing roster (a genre change adopts the staged override), and
  bind runs on the start step. `PartOutput` is in every roster copy, the start step's included, so keep it small:
  `ControlRanges` stores each percent as its difference from 100, so zeroed SDRAM BSS holds the defaults.
  `ControlRanges::with()` is `consteval` (a built-in table's percent out of 0–200 doesn't compile); runtime code calls
  `set()`. Test builds report a set with ranges of its own as `gen_controls ranges part=1 lane=-1 BRI=-16..16 ... DRI=0`
  (each control's first param's range, 0 if it binds none). An event line is 120 bytes with its timestamp, so keep it
  short: a param count or lane names overflowed it.
- A cable set to exactly 0 is deleted by `PatchCableSet::notifyParamModifiedInSomeWay`, so Wobble's ×0 leaves ±1.
- Only disk-level FatFs errors are card errors (`card_sounds.cpp`). A missing or bad folder lists nothing and falls
  back (§10.7).
- A test hook can fault the card: "hold card_error" makes `CardFolders` and Perform's loaders fail with `SD_CARD`
  (`cardErrorInjected()`, `card_sounds.h`). delugemu can't pull or fault its card.
- An unused Sample isn't freed: stock firmware keeps it as a stealable cache, and it still holds internal and
  external heap (its cluster list, its path). Each entry picks new sounds, so leaving deletes every unused audio file
  (`LeaveJob::deleteUnusedAudioFiles()` in `entry_leave.cpp`) before it builds the blank song. Free it there,
  not after: a blank song built around cached Samples lands elsewhere on the heap each time, and free memory then
  drifts by 8 B. Temp-recorded Samples are kept for `deleteAnyTempRecordedSamplesFromMemory()`, which also resets
  the recording number.
- MIDI in (`perform_midi.cpp`): the stock MIDI routes call it under `rootUIIsGenerativePerform()` (F8, F18); F10's
  `takesGlobalMidiCommand()` under `mayTakeGlobalMidiCommands()`, which also opens under the modal screen. MIDI is read in
  `PlaybackHandler::midiRoutine()`, which `AudioEngine::routine()` also runs, so these calls can land wherever the audio
  routine runs (inside a card load too), as MIDI TAP and F3's learned edits already do. Stock's note route into a
  Perform part's Output hung delugemu, so F18 drops notes before any Output sees them. Follow with nothing focused
  builds a song model stack on the caller's (`followedSongParam()`), since stock follow has no song context. Feedback:
  `View::sendMidiFollowFeedback` hands every call to `sendFeedback()`; while playing, `PerformView::graphicsRoutine()`
  keeps stock's `SEND_MIDI_FEEDBACK_FOR_AUTOMATION` timer armed, and its rate check runs the pass.
  `perform::FeedbackFilter` keeps each CC's last value so a pass sends only what moved (H17).
- Leaving returns both heaps exactly (`gen_mem` test events). `gen_emu.py leak` allows no change at all, so a
  few bytes either way is a finding. Find the owner with a heap walk: diff the allocated blocks of two `after_leave`s,
  then search RAM for pointers to the new ones.
- Test-port render timing and H16's culls live in `io/midi/test_port_render.*`; the audio engine makes one call at
  each site. `lastCulled` is a bare Sound pointer: whatever frees a part's Output forgets it, or a Sound built later at
  the same address takes the freed part's name. Check after the free, not before: the teardown yields to the audio
  routine (once per Kit row), which can cull the part's tail on the way. `freePartOutput()` takes the part's Sounds
  (`soundsOf()`) first and compares them by address after. A piece's start resets the whole record, which covers
  entry and leave.
- Whole-sound transpose (#311, spec §10.6, ADR 0007): `applySoundSettings()` sets `Sound::transpose` to 0 when
  `SoundSettings::zeroTranspose` is set, which `partSoundSettings()` sets for Bass, Lead, Chords and Pad; Kit rows
  and FX lanes' synths (`laneSoundSettings()`) keep theirs. Nothing caches it: `Voice::noteOn()` reads it per note,
  and a fresh Output has no voices, so unlike the sound editor's transpose there is no
  `recalculateAllVoicePhaseIncrements()` to run. The default synth (`setupAsDefaultSynth()`) saves −12, so a
  fallback to it plays at 0 too. An oscillator's own transpose (`Source::transpose`) is left alone, so a preset that
  puts its octave on every oscillator still sounds off: `osc_octaves.py` flags those in the palettes. Test builds
  report `gen_sound_transpose PART LANE saved= played=` (LANE `-` for a synth part) apart from `gen_sound`, which is
  near the 120-byte event line already; `perform_transpose.txt` checks it.
