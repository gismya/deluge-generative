# 15. Compose output shape

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 15.1 Input and length

- Compose renders the Current Recipe, seed and takes, re-rolled takes included, so "Compose matches Perform" holds for a Recipe that was steered and then rendered.
- It runs the Arc exactly as an untouched Perform from Start would: the Intro, then the automatic cycle under Motion and the seed.
- Once the song passes the target length, End is queued at the first point that doesn't cut a Build or a Break, normally the end of a Groove, and the Outro plays from there. Sections are never cut short to hit the target. At Motion 0 the song is Intro, Groove up to the target, then Outro.

### 15.2 Session view and arranger

- Compose writes both. Session view has one clip per part per **Section occurrence** (Intro, Groove 1, Break 1, Build 1, Groove 2, …), and each occurrence gets its own **session section**, numbered in Arc order.
- The arranger places those same clips in Arc order, from Intro to Outro.
- The Deluge has 24 session sections (`kMaxNumSections`). If a song reaches 23 occurrences, Compose queues End and the Outro takes the 24th. The OLED says the song was ended early.

### 15.3 Clips

- One clip per part per Section occurrence, as long as the whole Section. A 6-phrase Groove of 8 bars is one 48-bar clip.
- The Drop's lift is written into the start of the following Groove clip, and the Gap into the end of the Build clip. Gestures never cross a clip boundary: Rise runs across the Build clip, and Reset lands on the first step of the next Groove clip.
- **No reuse.** Every occurrence gets its own clips, even when identical to an earlier one. Launching a session section stops every clip outside it, so a reused clip would leave later session sections incomplete.
- A part that doesn't sound for a whole occurrence (left out of Break keeps, or below its Density gate the whole time) gets **no clip** there. A part that sounds for part of a Section gets a clip with rests.
- Every clip of a Drums or FX part has a row for every lane the part has, in the fixed lane order, even when silent.

### 15.4 Notes

- Fixed notes: probability 100, no iterance, no fill flags, exactly as Perform plays them. Variety comes from the Arc. The player can add firmware chance afterwards.
- Notes go at exact tick positions, so Lean, swing differences and Ornaments show as nudged notes, and the player can edit ratchets as notes.
- Synth clips are in scale mode. Kit clips aren't.

### 15.5 Song settings

- Tempo, swing, key and scale are set once on the song from the Recipe. `key.rootNote` and `key.modeNotes` are assigned directly.
- After rendering it is a plain Deluge song for pitch: stock key transpose and mode remap work, and stock refuses a scale smaller than the pitch classes the song uses. Compose adds no guard.
- Nothing goes into the arranger's song-level automation.

### 15.6 Automation

- Each part's automation lives in its own clip, per occurrence. Each clip carries its own copy of the sound's parameters (stock behaviour), so a base-value edit in one clip doesn't spread.
- **Set the base, automate only movement.** Each param's stored value is home + the macro layer at the Recipe centre. A param that doesn't move gets no automation.
- A sound control lands as the firmware params it maps to on that engine ([§11.2](11-sound-controls.md#112-mapping-per-engine)). FM Brightness becomes the modulator 1 and 2 volumes. The song has no "Brightness" param.
- **Ramps** (Rise, Close, Wash, effective-Energy changes) become interpolated nodes at their start and end, plus enough points between to follow the curve. **Snaps and Throws** become step-exact nodes.
- **Drift** is thinned to at most one interpolated node per bar per param, so the result stays editable.
- `setNodeAtPos` doesn't flag the param as automated, so Compose must flag it.

### 15.7 Build and handover

- Compose builds into a **private `Song*`**, not `preLoadedSong`: the next Session launch event swaps in a non-null `preLoadedSong` even when half-built. The swap sets `preLoadedSong` only immediately before `doSongSwap`, which reads it, with no yield between (as `clear_song.cpp` does).
- It steps the core in UI/task context and yields every N steps to stay well under 3 ms per run. Builds must yield so BACK can cancel. In-memory construction must call `AudioEngine::routineWithClusterLoading()` periodically, and those calls run sequencer ticks too.
- The finished song is swapped in **stopped**, which keeps `deleteSoundsWhichWontSound` away from a playing song. `clear_song.cpp` is the template for the swap.
- The song opens in arranger view, **unsaved but named** `GEN <GENRE> <seed code>` (for example `GEN TECHNO 1A2B3C4`). Nothing is written to the card until the player saves.
- The song keeps no link to its Recipe beyond its name. Song XML drops unknown attributes on re-save, so an embedded link wouldn't survive.

### 15.8 Song construction facts

- `Song::writeToFile()` only works for `currentSong`: it writes reverb from the live `AudioEngine`, the pad preview from `PadLEDs` and uses the root UI. Saving happens only after the swap.
- `Clip` doesn't initialize `loopLength`. Set it before attaching an instrument.
- `getOrCreateNoteRowForYNote` can change the song scale. Write the song key first and use rows that are in scale.
- Some out-of-memory paths freeze the firmware instead of returning an error.
- A song needs each instrument's params in its clips (`soundParams`, `kitParams`, and per-drum `soundParams` in each note row). With params left only on the instrument, synth songs freeze with E412 on load and kit songs report FILE CORRUPTED.

Sources: [Compose output shape][t16], [Compose song construction path][t18], [Automation][t14], [Pitch model][t21], [Generator scheduling][t19], [Firmware changes the design needs][t43], [Time a scripted capture in delugemu][t36], [Firmware architecture of v1.3.0-rc3][t2], [Entering Generative mode and the Compose screen][t39].

[t2]: https://github.com/gismya/deluge-generative/issues/2
[t14]: https://github.com/gismya/deluge-generative/issues/14
[t16]: https://github.com/gismya/deluge-generative/issues/16
[t18]: https://github.com/gismya/deluge-generative/issues/18
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t36]: https://github.com/gismya/deluge-generative/issues/36
[t39]: https://github.com/gismya/deluge-generative/issues/39
[t43]: https://github.com/gismya/deluge-generative/issues/43
