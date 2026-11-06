# 15. Compose output shape

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 15.1 Input and length

- Compose renders the Current Recipe, seed and takes, re-rolled takes included, so "Compose matches Perform" holds for a Recipe that was steered and then rendered.
- It runs the Arc exactly as an untouched Perform from Start would: the Intro, then the automatic cycle under Motion and the seed.
- LENGTH becomes a target step count once, before the render: `floor(LENGTH in seconds × tempo in hundredths of a BPM / 1500)`, in integer math ([§3.2](03-recipe.md#32-musical-facts), ADR 0004).
- Once the step index reaches that count, End is queued at the first point that doesn't cut a Build or a Break, normally the end of a Groove, and the Outro plays from there. Sections are never cut short to hit the target. At Motion 0 the song is Intro, Groove up to the target, then Outro.

### 15.2 Session view and arranger

- Compose writes both. Each part gets **one clip for the whole song**, from the Intro to the end of the Outro. Session view holds those clips in a single **session section**, and launching it loops the whole song.
- The arranger places one instance of each part's clip, from the start of the Intro to the end of the Outro. The Deluge arranger has no markers, so Section lines don't show.
- Why not a clip per Section occurrence: each occurrence costs about 130 K of clip setup before any note, mostly each kit row's param copy, so a 4:00 song came to 1.7 MB and LENGTH would have been capped near 6 minutes ([§20](20-budgets.md#20-budgets-and-measured-facts)). With one clip per part there's no session-section cap either, so nothing ends a song early.

### 15.3 Clips

- One clip per part, as long as the whole song. Compose never cuts a clip; Capture does, at sound re-rolls and key changes ([§17.6](17-capture.md#176-changes-partway-through-a-piece)). The firmware's longest clip (`kMaxSequenceLength`, about 1.6 billion ticks) is far beyond 20 minutes.
- Sections, the Drop lift, the Gap and gestures are notes and nodes along that one clip.
- The core still cuts sounding notes at every Section line and re-strikes those still holding on its first step ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)), so a Capture split ([§17.5](17-capture.md#175-song-shape)) never cuts a note.
- A part that never sounds in the whole song (below its Density gate throughout) gets **no clip** and no track. A part that sounds for only some of it gets rests.
- Every Drums or FX clip has a row for every lane the part has, in the fixed lane order, even when silent.

### 15.4 Notes

- Fixed notes: probability 100, no iterance, no fill flags, exactly as Perform plays them. Variety comes from the Arc. The player can add firmware chance afterwards.
- Notes go at exact tick positions, so Lean, swing differences and Ornaments show as nudged notes, and the player can edit ratchets as notes.
- Synth clips are in scale mode. Kit clips aren't.

### 15.5 Song settings

- The song is created at magnitude 2 ([§12.3](12-runtime.md#123-the-drivers-clock)). Tempo, swing, key and scale are set once on the song from the Recipe. `key.rootNote` and `key.modeNotes` are assigned directly.
- After rendering it is a plain Deluge song for pitch: stock key transpose and mode remap work, and stock refuses a scale smaller than the pitch classes the song uses. Compose adds no guard.
- Nothing goes into the arranger's song-level automation.

### 15.6 Automation

- Each part's automation lives in its clip, which carries its own copy of the sound's parameters (stock behaviour).
- **Set the base, automate only movement.** Each param's stored value is home + the macro layer at the Recipe centre. A param that doesn't move gets no automation.
- A sound control lands as the firmware params it maps to on that engine ([§11.2](11-sound-controls.md#112-mapping-per-engine)). FM Brightness becomes the modulator 1 and 2 volumes. The song has no "Brightness" param.
- **Ramps** (Rise, Close, Wash, effective-Energy changes) become interpolated nodes at their start and end, plus enough points between to follow the curve. **Snaps and Throws** become step-exact nodes. A Throw's hold is a flat run and its fall is an interpolated ramp.
- **Drift** is thinned to at most one interpolated node per bar per param, so the result stays editable.
- `setNodeAtPos` doesn't flag the param as automated, so Compose must flag it.

### 15.7 Build and handover

- Compose builds into a **private `Song*`**, not `preLoadedSong`: the next Session launch event swaps in a non-null `preLoadedSong` even when half-built. The swap sets `preLoadedSong` only immediately before `doSongSwap`, which reads it, with no yield between (as `clear_song.cpp` does).
- It runs as a runner job ([§12.10](12-runtime.md#1210-long-running-work-and-input)): it steps the core in task context and yields to the scheduler about every 2 ms, so BACK can cancel. It never relies on `AudioEngine::routineWithClusterLoading()`, which runs only audio.
- The finished song is swapped in **stopped**, which keeps `deleteSoundsWhichWontSound` away from a playing song. `clear_song.cpp` is the template for the swap, with one change: stock ends playback first only on the internal clock and swaps live under an external one. Every Generative swap (Perform entry, Compose, Capture, a Recipe LOAD while stopped) ends playback before `doSongSwap`, whatever the clock. Under an external clock the new song then waits for the master's Start or Continue ([§12.4](12-runtime.md#124-external-clock)). A stopped Deluge ignores clock ticks, and PLAY on the Deluge starts on the internal clock, out of sync.
- The song opens in arranger view, **unsaved but named** `GEN <GENRE> <seed code>` (for example `GEN TECHNO 1A2B3C4`). Nothing is written to the card until the player saves.
- The song keeps no link to its Recipe beyond its name. Song XML drops unknown attributes on re-save, so an embedded link wouldn't survive.

### 15.8 Song construction facts

- `Song::writeToFile()` only works for `currentSong`: it writes reverb from the live `AudioEngine`, the pad preview from `PadLEDs` and uses the root UI. Saving happens only after the swap.
- `Clip` doesn't initialize `loopLength`. Set it before attaching an instrument.
- `getOrCreateNoteRowForYNote` can change the song scale. Write the song key first and use rows that are in scale.
- Some out-of-memory paths freeze the firmware instead of returning an error.
- A song needs each instrument's params in its clips (`soundParams`, `kitParams`, and per-drum `soundParams` in each note row). With params left only on the instrument, synth songs freeze with E412 on load and kit songs report FILE CORRUPTED.

### 15.9 Memory and failures

- **Size estimate.** Before building, Compose estimates the song's size from the Arc plan: about 370 K, plus clip setup (about 6 K per kit row, 11 K per synth clip), plus an upper bound per bar on notes (about 30 B each) and drift nodes. If it doesn't fit, BUILD is refused with the longest length that does (`TOO LONG: MAX 12:30`). A 4:00 song comes to about 0.7 MB and 20:00 to about 1.8 MB (*tuning*: checked against a real build, see [§20](20-budgets.md#20-budgets-and-measured-facts)).
- **The current song.** It keeps playing during the build when the estimate fits next to it. When the new song fits only alone, Compose ends playback and frees the current song before building, with no extra prompt, since the player has already answered DISCARD SONG?. A cancel or failure then lands on a blank song.
- **SDRAM reserve.** Every runner chunk checks that a reserve of SDRAM is still free, 512 K to start (*tuning*). If the build reaches it, the build stops: the private song is torn down, nothing else changes, and the OLED says there wasn't enough memory. The reserve has to be a stop, not a recovery: once SDRAM is full, the next ordinary `operator new` (a UI string, say) throws, nothing catches it, and the device freezes with `TERM`.
- The fast heap may fill. Stock song loads do the same, since song structures are allocated with `allocMaxSpeed` and spill into SDRAM.
- **Card errors.** If a sound can't load because the card errors or was pulled, the build stops with the stock card error and nothing changes. Missing-sound re-rolls ([§16.6](16-recipe-files.md#166-missing-sounds-and-sharing)) cover files that are absent, not card errors.

Sources: [Compose and Capture memory, limits and failures][t78], [Measure building and saving Compose- and Capture-sized songs on a Deluge][t62], [Compose output shape][t16], [Compose song construction path][t18], [Automation][t14], [Pitch model][t21], [Generator scheduling][t19], [Firmware changes the design needs][t43], [Time a scripted capture in delugemu][t36], [Firmware architecture of v1.3.0-rc3][t2], [Entering Generative mode and the Compose screen][t39], [Determinism details: tick unit, tempo, held notes and ramps][t77].

[t2]: https://github.com/gismya/deluge-generative/issues/2
[t14]: https://github.com/gismya/deluge-generative/issues/14
[t16]: https://github.com/gismya/deluge-generative/issues/16
[t18]: https://github.com/gismya/deluge-generative/issues/18
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t36]: https://github.com/gismya/deluge-generative/issues/36
[t39]: https://github.com/gismya/deluge-generative/issues/39
[t43]: https://github.com/gismya/deluge-generative/issues/43
[t77]: https://github.com/gismya/deluge-generative/issues/77
[t62]: https://github.com/gismya/deluge-generative/issues/62
[t78]: https://github.com/gismya/deluge-generative/issues/78
