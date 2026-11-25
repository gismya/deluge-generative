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
  from `preLoadedSong`, else `currentSong`, so `applySyncDefaults()` (`perform_song.cpp`) resets them. It writes
  `delay.syncLevel` too, so a delay setup (`applyDelaySetup()`) goes after it, or the setup's note is lost (#320 hit
  this on sample rows). The stock sync level counts from the song's tick magnitude (Perform's song plays a 16th at 6,
  not the `SYNC_LEVEL_16TH` 5 its name says): `delaySyncLevel()` turns a note into one. `preLoadedSong`
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
- A Space Control event's offset, under a delay Throw (level > 0), holds the Throw's reverb spike, summed and clamped
  in the core: the driver can't take it back out. Anything that wants Space's share without the Throw (a player edit
  to the delay feedback, `gen_throw_delay`'s `rest=`) reads `Set::restSpace`, Space's offset from its last event at
  level 0 (#343's MUST and #344 were both this bug). `rebindPart()` carries it to the new sound's set.
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
- A sound-control target whose stock stage switches on at "> 0" can step the level the moment a "+"-only control
  (Drive, Thin) leaves home. `LOCAL_FOLD` did (#516): `voice.cpp` runs `foldBufferPolyApproximation` from any fold
  above 0, starting at `FOLD_MIN`, and the folder's gain there is −1 to −34 dB depending on input level. So every
  synth part dropped 1.5–5 dB (about 4 on most) when a Build took Energy past home, and the harness can't see it (it
  doesn't model sound). Another gate of this shape: `Sound` runs `doHPF` once `LOCAL_HPF_FREQ` leaves its minimum
  (`processing/sound/sound.cpp:2537`), and Thin crosses it on subtractive and Ringmod parts (`kThin` has no
  `Needs::HpfInUse`); its level step isn't measured. Before adding a target, measure the level at its first step with
  a delugemu capture pair (`gen_emu.py capture`, the part alone, with and without the target).
- A lane sample of 2 s or more loads in CUT mode (`perform/one_shot.h`, §10.4), so its note-off stops it: a lane's
  note length is what you hear, not the file's. Before #423 the Fall's 6-tick note cut the 8 s Downers to a beat
  while the rough render, which played every sample out, rendered all 8 s. The render now follows the same rule, so a
  short gate on a long sample shows in the render too. The Impact's descending kicks (3-4.3 s) are still cut by its
  6-tick gate. Lane samples near 2 s sit on either side of it by a millisecond (`GEN Ride 2`, `GEN Ping 2`, Dub's
  `GEN Riser Short` are 2000 ms: CUT).
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
- Trap (#332): `gen_mem ... internal=` in the hundreds of millions or billions means something wrote into the
  internal heap's free-space records, not that memory grew, and the next allocation hangs (often a preload's, so the
  test port stops answering). In the Release and test-port links `emptySpacesMemoryInternal` sits right after
  `spareRenderingBuffer` (check with `arm-none-eabi-nm -n -S -C`), so an overrun past the voice's scratch rows lands
  there. #332's was stock DSP: `renderOsc()`'s `renderOscSync()` storage lambdas store 4 samples at a time
  (`vst1q_s32`) and can run 3 past the window, and ringmod rendered oscillator 2 into the last row unpadded. A pulse
  width on a basic oscillator other than a square usually renders through sync (the table saws and the sine; a low
  triangle and the crude low saw have one-sample sync loops), so any ringmod preset with one on oscillator
  2 (125 Evolving Pad as saved, or a saw or sine whose pulse width a sound control moves off 0) wrote audio into the
  records. The wavetable path never overran: `WaveTable::render()` stores one sample at a time. Fixed in
  `Oscillator::renderOsc()` (`takeOscSyncRender()`);
  `perform_ringmod_sync.txt` covers it. A sound control may move a param where stock presets rarely sit, so it reaches
  stock paths nothing else exercises: suspect those before Generative code. To find the writer, give the suspect
  buffer a spare row, zero it, and after each render report (a TEST_EVENT) which call left it non-zero.
- Restart AS IT BEGAN (`firmware/restart_began.*`): the press copies nothing from the log but the piece's index
  (`SteeringLog::newestWithHeader()`); the line reads the header in place (`Piece::start()`, `pick()`,
  `forEachSnapshot()`), before the restarted run's `openPiece()` throws the ended piece away, so `lands()` must run
  inside `NoteSink::restartPiece()`, never later. Its preload builds every changed part in one live job and frees what
  it built itself on a cancel or an error (`freePartOutput()`, task context): a later `retire()` would leave a
  cancelled job's Outputs linked. Swaps go through `SoundSwaps::adoptRestarted()`, after `pieceRestarts()` dropped the
  waiting sounds, so the part's incoming slot is free. The snapshot is written with `setCurrentValueBasicForSetup()`
  only where a value differs, then the Sound is patched again (`performInitialPatching()`), as `SoundControls`
  `writeBase()`/`repatch()` do: fine on the line after the flush, wrong under sounding voices. A swing interval that
  changes in the tick waits for the pump (`Transitions::swingIntervalFollows()`): `changeSwingInterval()` reschedules
  the internal clock while playing.

## The steering log on the card (#448)

- `firmware/capture_files.*` (`cardLog`) is the log's only consumer: a priority-30 repeating task with
  `RESOURCE_SD`, added at Perform's entry (`cardLog.enter()`, in the entry job) and removed by `finish()` (the
  teardown, before the swap). The tick may run inside any of its card calls (`routineForSD()`/`yield()` run the
  playback task), never between them: so a chunk's bytes stay put until `took()`, a slot stays until `released()`,
  and `beginPiece()` refuses to open over a kept piece's header the task hasn't taken yet.
- `snprintf()` with a number (`%lu`) pulls newlib's stdio into the link, whose syscalls (`_write`, `_sbrk`…) the
  firmware doesn't stub: the link fails with `undefined reference to _close` and friends. `%s`-only calls are fine
  (GCC folds them into copies). Build numbers into strings by hand (`pathOf()`).
- `cardLog` lives in `.sdram_bss` (its three `FIL`s are 1.7 KB): zeroed at boot, never constant-initialised, so every
  member must read right as zero until `enter()` resets it (`taskAdded_`, not a `-1` task id).
- A piece's file is its image: `PieceInfo` (the genre as its index in `allGenres()` and its version, the pointer
  can't be stored; `kFormat = 2` since #448 part 3 added `kSettingPathTag = 0x85` and `kSettingsTag = 0x92`), header
  records, records. The task rewrites the info at each flush with `bytes` = what reached the card, always a step's end,
  which `readImage()` trusts after a power-off; past it the last step may be torn. `CardLog::enter()` holds back
  `kAdoptReserve` (4) slots when adopting marked files so a full folder still leaves room to log the new set.
- Non-param sound-editor settings (`core/output_settings.h`, `firmware/settings_tracker.*`): `SettingsTracker` lives in
  `.sdram_bss` and polls at most 2 slots per frame in `PerformView::graphicsRoutine()` (the SoundEditor target or
  focused Output, plus 1 round-robin slot) so frame time stays a few microseconds, with full sweeps at piece start/stop,
  before `logHeader()`, and before `RestartBegan::press()` (plus a single-slot Song sweep in `RestartBegan::restore()`).
  `OutputSetting` orders `SynthMode` and `Osc0Type`/`Osc1Type` first so applying a `0x92` block in ascending ID order
  never lets `setSynthMode()`/`setOscType()` clobber filter modes or sample flags; `enqueueScalar()` moves updated
  entries to the back so same-step edits drain in chronological order. `setOscTypeSafe()` saves/restores
  `soundEditor.currentMultiRangeIndex`/`currentMultiRange` so Capture and AS IT BEGAN never clobber SoundEditor state.
  Traps: (1) `CompThreshold` is Song-only (`Sound::render()` and `GlobalEffectableForClip::renderOutput()` overwrite
  `compressor.setThreshold()` from `UNPATCHED_COMPRESSOR_THRESHOLD` on every render); (2) `Source::setReversed()` casts
  `ranges` to `MultisampleRange`/`Sample`, so call it only when `oscType == SAMPLE`; (3) `applyOutputSettingPath()`
  resets `SampleHolderForVoice` zone/loop (`startPos = 0`, `endPos = 9999999`, `loopStartPos = loopEndPos = 0`) for
  `SAMPLE` before `loadFile(isCurrentlyReversed(), false, true, CLUSTER_ENQUEUE, nullptr, true)` so longer samples
  aren't truncated and chunkless wavetables load; (4) while `AudioEngine::reverbSidechainVolume < 0` (AUTO mode),
  `AudioEngine::updateReverbSidechain()` overwrites `reverbSidechain.{attack,release,syncLevel}` from the loudest
  reverb sound, so `SettingsTracker` reads `song->reverbSidechain*` in AUTO mode (`Song::Song()` now initializes
  `reverbSidechainAttack`/`Release`); (5) `PatchCableSet::setupPatching()` (via `finalizeSoundSettings()`) and
  `Sound::setModFXType()`/`setOscType()`/`setNumUnison()`/`setReversed()` allocate or touch clusters, so
  `partNeedsRebuild()` forces a `BeganJob` task-context rebuild for any part whose live or header mask includes them
  (`SynthMode`, `Osc0Type`, `Osc1Type`, `LpfMode`, `HpfMode`, `ModFxType`, `Unison`, `Osc0/1SampleFlags`,
  `Osc0/1SamplePath`, and `OscSync`, `Lfo0`, `Lfo2`: `Sound::maySourcePatchToParam()` reads them, so they change
  `setupPatching()`'s result), `RestartBegan::restore()` skips `restoreSettings()` for rebuilt parts, and `restoreSettings()`
  with `loadPaths = false` only runs `performInitialPatching()` (never `setupPatchingForAllParamManagers()`).

- The preset preview (#443, `preset_preview.*`, the portable `perform/preset_preview.h`) builds with
  `buildPartOutput()` in a live job, as `SoundJob` does, but the Output goes into no roster: it sits linked in the song
  beside the part's own, its note goes through `OutputSink::playNote()`, and it is freed in task context with
  `freePartOutput()` (from the UI, or in the job's `finished()` for a build the cursor has left). Putting it in the
  roster would have needed a restore before every start step (`soundControls.begin()` reads homes from the roster),
  restart, transition and knob bind. The runner is busy while a preview builds, so the browser's SELECT cancels it and
  chooses once it has unwound (`chooseWhenFree_`); a choice made at once would be refused BUSY.
  `deleteUnusedAudioFiles()` after a free runs only with the runner free (or from the preview job itself): another job
  may be loading samples no Output holds yet. What a preview holds, on the Deluge with the pack (`gen_mem preset_preview`
  against `preset_preview_freed`): a synth part's preset 13.7 KB of internal RAM, an FX lane's (a whole Kit, the
  lane's row a synth) 39 KB, external RAM about 0 for both.
- The browser services the preview every frame, also while its build runs (a build that yields at a card read or a
  chunk boundary lets frames through). `PresetPreviewLifecycle::service()` cancels a running build only when its token
  is no longer the newest want. It used to cancel on any frame mid-build, so a build longer than a frame
  was cancelled and resubmitted forever (`gen_preset_preview cancelled` every ~45 ms). It showed once #488 made the FX
  Kit's Impact pick a pack preset: on a card without the pack, the missing preset's lookup made the build span frames.
- The FX Kit's fallback Riser (spec §9.5, #551) is a row past the lane rows, `perform::kRiserFallbackRow`, loaded by
  `buildFallbackRiser()` when the Riser row plays a sample longer than a pack short Riser (`kPackRisers`), so every
  owner of `buildPerformSong()` gets it: Perform, Compose and Capture. A Riser note-on the driver gave the fallback
  carries `kRiserFallbackPitch`; route a laned note by `kitRowOf(lane, pitch)`, never `laneRow(lane)` alone. The core's
  note-off doesn't know which row played, so `OutputSink::playNote()` ends both; Compose's ClipBuilder pairs by lane
  and writes the note-on's pitch, so its note lands on the fallback's row by itself.
