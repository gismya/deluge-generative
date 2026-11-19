# 10. Sounds

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 10.1 Palettes and sound picks

- Synth sounds are card presets, chosen through a **palette**. There are no built-in sound templates and no preset mutation. The firmware bundles no sounds.
- Each genre has a built-in **default palette** (slot 8), constant data in the core, so the harness reads it with no file parsing. Entries are keyed by part, with a role-level section as the default for parts that aren't listed. Each entry is a weighted list of preset paths, which can point anywhere on the card, each with its own sound-control ranges ([§11.1](11-sound-controls.md#111-the-six-sound-controls)).
- A card file `GENERATIVE/PALETTES/<GENRE NAME>.XML` is an optional **override**. Each entry it lists (a part, a role default or a lane) replaces the built-in entry, and everything else falls back. v1 ships no override files, but the firmware reads one if a player writes it.
- **Override format.** One `<palette>` element holding `<part name="ACID">`, `<role name="bass">` and `<lane name="kick">` sections. A part or role lists `<preset path="SYNTHS/…" weight="3"/>`, which may also give each sound control's range on that preset as a percent of its default ranges, 0–200: `brightness`, `decay`, `drive`, `space`, `wobble` and `thin` ([§11.1](11-sound-controls.md#111-the-six-sound-controls)). A lane lists `<folder path="…"/>`, `<tag text="…" weight="…"/>`, `<file path="…"/>` (one exact file) and, for an FX lane, `<preset …/>` with the same range attributes as a part's. A weight is 0–255 and defaults to 1; a range is a whole number, 0–200, and defaults to 100. Role names are lowercase (`bass`, `fx`) and lane names camel case (`closedHat`, `perc1`). Unknown elements and names are skipped, a part name the genre doesn't have included; a skipped section uses none of the limits below but the file's size. **Limits:** the file is at most 8 KB; its attribute text (names, paths, tags) at most 2 KB in all; at most 64 presets and 64 tags across the file, and 32 folders; at most 8 presets and 8 tags in one part, role or lane; at most 12 part entries, counting the built-in palette's part entries the file doesn't replace; each part, role and lane listed once. A file past a limit, or malformed, is ignored and the built-in palette plays.
- A palette changes only what the music sounds like, never how it plays.
- A **sound pick** is a concrete path rolled (from fresh entropy, by weight) when a Recipe is created and stored in the Recipe. In memory the picks sit beside the Recipe, not in it: the core never reads them, so the Generator's copy of the Recipe leaves them out. A Recipe file stores them with it ([§16.1](16-recipe-files.md#161-contents)). A pick is kept until the player re-rolls it or changes it. The player can point any synth part at any card preset through Perform's preset browser ([§13.5](13-perform.md#135-control-layout)), and any single lane at any sample file (not built yet).
- **Path length.** A sound pick holds a card path of at most 127 characters (128 bytes with its terminator). Files with longer paths are skipped when picks are rolled, and a longer palette path counts as missing.
- **Fallback chain.** If a pick doesn't load, try the next palette entry. If nothing loads, use the Deluge default synth patch and show one OLED warning. Generative mode still starts.
- **Missing path on Recipe load:** re-roll from this card's palette and warn ([§16.6](16-recipe-files.md#166-missing-sounds-and-sharing)).
- **Minimum length.** Each part has at least 3 palette entries, and each lane a genre always plays has at least 3 files after tag filtering. Gated lanes may have fewer.
- **Velocity.** Every palette Synth must respond to velocity, through a velocity cable or DX7 operator velocity sensitivity. A script over `pack/` checks pack presets, run locally. The Generator never adds cables.
- **Unison.** Chords palettes avoid presets with more than 4 unison voices. Pads may use them.

### 10.2 Drums: assembled per lane

The Generator builds its own Drums Kit, one sample per lane. Each palette lane entry carries a **folder list** and optional **tags**. The default folder list comes from this built-in table:

| Lane | Folders under `SAMPLES/DRUMS` |
|---|---|
| Kick | Kick |
| Snare | Snare |
| Clap | Clap |
| Rim | Rim |
| Closed hat | HatC |
| Open hat | HatO |
| Ride | Ride |
| Crash | Crash |
| Tom | TomL, TomM, TomH (one row, one sample drawn from any of the three) |
| Perc 1, Perc 2 | Shaker, ConL, ConM, ConH, Cowbell, Tambourine, Claves, Block, Guiro, Other |

- China and Timpany are in no lane by default. A palette can still name them.
- A card override can point any lane at any folder (for example Kick at `SAMPLES/DRUMS/MyKicks`).
- No folder-name aliases and no recursion into subfolders. Case doesn't matter, because FatFs looks paths up case-insensitively. The scan skips non-audio files such as the `.reapeaks` files in `Clap`.
- A lane with nothing loadable stays silent and shows one warning. There's no synth fallback for drums.

### 10.3 Tags

- A tag matches case-insensitively anywhere in the filename, without the extension.
- A lane rolls a tag by weight, then picks one matching file at random.
- When a lane lists tags, only files matching one of them can be picked. With no tags, any file in the lane's folders can be picked.
- A lane entry can name an exact file path instead. That's how a palette picks the plain `808 Kick` alone, since that name is contained in longer ones.
- **Hat pair.** The Open hat's roll first tries the tag the Closed hat rolled. It falls back to its own weights when that tag isn't in its list or matches no file. This is a fixed Generator rule for the hat pair only.

### 10.4 Building a Kit in code

No firmware API builds a Kit from sample paths, but every call underneath takes a `Song*` or a ModelStack, so this is Generator code that works on the playing song (Perform) and a private one (Compose, Capture) alike:

- `createNewInstrument(KIT, &pm)`, `createNewDrum`, `setupWithPatching` + `Sound::initParams` + `setupAsSample`, `song->backUpParamManager`, `getOrCreateFirstRange` + `filePath` + `loadFile` (or `kit->loadAllAudioFiles(true)`), `kit->addDrum`, `setAudioInstrument`, `setupAsNewKitClipIfNecessary` → `NoteRow::setDrum`.
- Pass `manuallySelected = false` to `loadFile` (true writes `sampleBrowser.lastFilePathLoaded`). Don't call `Source::setOscType`, which writes `soundEditor`.
- Set what the browser normally sets: repeat mode (it defaults to `CUT`; the browser picks `ONCE` for samples under 2 s), the Kick sidechain send, and a unique drum name.
- Keep a new Kit off the song's output list until it is complete.
- `ModControllableAudio`, `ArpeggiatorSettings` and `SideChain` constructors read their default sync level from `preLoadedSong` or `currentSong`. Set it explicitly when building a private song.

### 10.5 FX lanes

- Riser, Fall, Impact and Siren look for samples in their palette entry's folders, `SAMPLES/FX/<lane>` when it lists none, as every lane does ([§10.2](#102-drums-assembled-per-lane)). Techno's Riser lists none; its Fall and Impact list factory folders and pick by tag (`Downer` files for the Fall, descending and sustained kicks for the Impact), so they never scan `SAMPLES/FX/Fall` or `SAMPLES/FX/Impact`. If none, they load a Generative pack synth preset into the kit row through `StorageManager::loadSynthToDrum`, which takes a `Song*` and is safe on a private song. Its only caller does the follow-up on `currentSong`, so the Generator needs its own follow-up on `song`.
- Riser lanes list only pack risers. Fall and Impact use factory files plus pack fallbacks. Siren is always a pack synth preset: its lane lists no sample folder that holds files, only `GEN Siren`, so it loads through the synth fallback above.
- **Riser fit.** A genre's Riser lane lists only pack Risers no longer than its shortest Build at the top of its tempo range, so a sample Riser never overshoots the Drop. A `check_all.py` check over `pack/` holds it. Dub's 1-bar Build takes `GEN Riser Short` (about 2.5 s); House leaves out the 8 s Riser.
- FX lanes always use their own row controls, so a synth Riser sweeps its own row ([§11.4](11-sound-controls.md#114-gestures)).
- **A sample Riser's timing** ([§9.5](09-arc.md#95-section-rules)) lives in the Perform driver, not the core. Only the firmware knows a sample's length, and tempo, with its glides, is driver-side ([§2.2](02-architecture.md#22-drivers-and-the-event-stream)). So the core still places the Riser's note on the Build's first step, and the event stream doesn't change ([§6.3](06-randomness.md#63-the-contract)). The adapter tells the driver the sample's length: the row's first range at the system sample rate, for a ONCE or CUT row (a synth row has none). The driver holds the note-on back until the time left to the Drop, at the tempo it plans for each beat, equals that length, in integer math. Compose and Capture write the note at the same place from their tempo automation.

### 10.6 Load-time settings

When the Generator loads a part's sound, it does these once, before playback. None is a sound control, and nothing changes them live.

- **Transpose.** On a pitched part (Bass, Lead, Chords, Pad), sets the preset's whole-sound transpose (`Sound::transpose`) to 0 and leaves each oscillator's transpose alone, so a Register is the pitch heard whatever the pick (ADR 0007). FX synths and Kit rows keep theirs. A local check against the card image flags a palette preset that puts an octave on every oscillator, which this doesn't reach.
- Sets the voice mode (`Sound::polyphonic`) to the part's mode ([§5.8](05-parts-and-lanes.md#58-voice-modes)).
- **Portamento.** Parts that slide (Loop) need portamento: if the preset's is off, set a role default; if it's on, keep it. Every other part gets portamento 0. Portamento is `UNPATCHED_PORTAMENTO`, an `AutoParam`, so Compose writes it as a normal clip value.
- Switches the clip's arp off. Old presets store `<arpeggiator><mode>`, which the clip copies on load (`instrument_clip.cpp`). This makes the 15 factory presets with a saved arp usable, 165 Acid Arp included.
- Sets `Sound::voicePriority` by role: High for Drums and Bass, Medium for Lead and Chords, Low for Pad and FX. CPU-load culling then thins the pad before the kick or bass drops a note.
- **Delay setup.** A genre may give a part, a kit row or a whole kit a delay setup (slot 10): sync level and type (even, dotted or triplet, such as a dotted 8th), ping-pong on or off, and analog on or off. These are plain fields on the `Delay` (`syncLevel`, `syncType`, `pingPong`, `analog`), not params, and Compose saves them with the song. Feedback home stays as the preset saved it, often 0. A delay setup is what lets Throw raise delay feedback ([§11.4](11-sound-controls.md#114-gestures)). The stock render allocates the buffer itself once feedback is above its threshold and sound is coming in, from SDRAM (`allocLowSpeed`, up to 88,200 stereo samples, about 690 KB), and frees it when the repeats run out (`dsp/delay/delay.cpp`). Every delay preset already works this way, so no non-zero home is needed. Kit rows are Sounds with their own delay, and the kit has one at kit level. A setup applies on every load of the part's sound, a chosen one included, and writes only those four fields. The data is a `DelaySetup` (a note from a whole to a 32nd, even, dotted or triplet, ping-pong, analog) on the `PartSpec` (a synth part's sound, or on Drums and FX the Kit's own at kit level) or a `LaneSpec` (its row). FX lanes always use their own row controls, so an FX Kit's kit-level setup is written on load but never thrown. The stock sync level counts the note from the song's tick magnitude (a 16th is 8 − magnitude, as a Kit's sync defaults are set), so the setup is written after them. Dub's parts have setups (its Tables in [§4.5](04-genres.md#45-v1-genres)). A test build's `hold delay_setups` gives Perform entry a test copy of the chosen genre with one on its Bass, its Drums kit and the Snare row, with notes long enough that at 60 BPM the render allocates two of the largest buffers at once ([§20](20-budgets.md)) (`perform_delay_throws.txt`).
- **Volume.** A genre may start a synth part below its preset's level: it scales the post-FX volume knob to a percent of the preset's position (tuning data on the part). Volume 50 (of the knob) is about −12 dB as played: the knob isn't linear in level (on hardware `gen_sound_level` read Acid techno's Drone at 134,193,152 against its preset's 536,821,760). Melodic techno's Pad starts at volume 50, since a 4-note chord at the preset's level sits over the bass and lead, Acid techno's Drone at 50, since its held root and fifth at the preset's level sat about 5 dB over the rest of the mix, and Dub techno's pedal Pad at 50, since at the preset's level it sat about 2 dB over the rest of the mix together. Dub techno's Stab keeps its preset's level: soft by itself, it sits about 5 dB under the drums and sub. Volume isn't a sound control ([§11.6](11-sound-controls.md#116-home-and-player-edits-perform)), so the mix stays the player's from there.
- Sets CHOKE on the Open hat and Closed hat rows, and on no other row. A CHOKE drum cuts every other CHOKE drum in its Kit (`kit->choke()`), and there's only one group per Kit.

The Generator never changes oscillator type, synth mode, tuning or mod routing, apart from the whole-sound transpose above. All of the above are plain field writes and need no firmware change.

### 10.7 When sounds load

- Loading runs in task context as a runner job ([§12.10](12-runtime.md#1210-long-running-work-and-input)), never in a tick. Card loads can't run inside the audio routine.
- Entering Perform or loading a Recipe loads every sound before playback.
- A genre change, Regenerate or Recipe LOAD preloads the incoming roster into fresh Outputs while the old one plays ([§9.7](09-arc.md#97-transitions)).
- **Card errors.** If a preload fails because the card errors or was pulled, the transition or re-roll is cancelled as BACK would cancel it, the playing roster carries on, and the OLED shows the stock card error. Missing-sound re-rolls ([§16.6](16-recipe-files.md#166-missing-sounds-and-sharing)) cover files that are absent, not card errors.
- A sound re-roll in Perform preloads into a fresh Output, then swaps at the next phrase line. A transition confirm cancels one still preloading ([§12.9](12-runtime.md#129-instances-and-the-transition)).
  - NEW SOUND rolls the part's picks again from its palette and fresh entropy, each pick moving away from the one it replaces where the palette offers another (another entry for a synth part; for a lane, the next file of the tag it rolled, or another tag when that tag has only the old file), so it's heard as new. A Kit part re-rolls every lane it plays. Back to the previous sound loads the part's previous picks.
  - A **chosen sound**: SYNTH on a synth part opens the preset browser ([§13.5](13-perform.md#135-control-layout)), and the preset confirmed there becomes the part's pick and preloads the same way, palette or not. It is an ordinary pick once chosen: the same load-time settings ([§10.6](#106-load-time-settings)) and fallback chain ([§10.1](#101-palettes-and-sound-picks)) when it doesn't load, logged with its path as a re-roll is ([§17.2](17-capture.md#172-the-steering-log)), kept until NEW SOUND, a Regenerate or a genre change, and saved with the Recipe. Back to the previous sound returns to the pick before it. A preset its palette lists plays with that entry's control ranges, as a rolled pick of the same path does ([§11.1](11-sound-controls.md#111-the-six-sound-controls)); any other with the defaults.
  - The preload is a live runner job ([§12.10](12-runtime.md#1210-long-running-work-and-input)). It builds the part's new Output beside the old one and links it into Perform's song in one chunk at its end, silent until notes go to it, then queues SetSound for the part. The Sound event at the line swaps it in: the sound controls bind to its params with its preset's homes, at the offsets the old sound had ([§11.6](11-sound-controls.md#116-home-and-player-edits-perform)), and the gold knobs move onto it if the part is focused. The old Output retires as a transition's outgoing Outputs do ([§12.9](12-runtime.md#129-instances-and-the-transition)): freed by a live job once silent by §12.9's test, or a phrase after the swap at the latest, counted at the tempo then so it runs on while stopped. A stop leaves its tail ringing too. An Output still sounding at the cap fades over a beat first, as §12.9's do. The free also deletes every audio file no song uses, so memory comes back with the old sound. A re-roll is refused with the busy popup while the Outputs waiting to be freed could fill the sound share of their list ([§12.9](12-runtime.md#129-instances-and-the-transition)).
  - The old Output retires as an outgoing one does ([§12.9](12-runtime.md#129-instances-and-the-transition)), within a phrase, except while a sound editor open on it sits under a screen it opened: then it outlives the cap until that screen closes and the editor with it ([§13.7](13-perform.md#137-sound-editor-midi-and-external-tempo)). Stopped, with no fade, its tail rings until then.
  - Pressing the part's NEW SOUND (or SHIFT, or SYNTH or KIT) again cancels its sound still preloading or waiting for the line, a chosen one too: SYNTH then cancels and doesn't open the browser. A cancel of a waiting sound that finds the steering queue full is refused with QUEUE FULL ([§2.6](02-architecture.md#26-steering-into-the-core)), and the sound still swaps on its line. A BACK tap cancels a preload still running first, as the newest action, and otherwise drops the newest queued action, which may be a waiting sound. A stop cancels a waiting sound like every pending action. While stopped there's no line: a re-roll swaps in as soon as its preload ends, and the next start plays it.
  - A card error, or a pulled card, during the preload ends it the same way, with the stock error popup. Card folder scans are chunk boundaries of the job, so BACK is seen during a long scan.
- Only one card load runs at a time: the deserializer, the sample `FIL` and the alternate-folder state are global. Ticks run inside every yield, so tick code must never touch a half-built Kit or start a load.
- Factory FM presets load as they are. The firmware forces their filters off (files older than community 1.2.0), which is what their designers heard. FM Brightness is modulator level, so nothing is lost.

Sources: [Sound generation][t13], [Kit lane mapping][t25], [Generative pack contents][t42], [Building Generator Kits in code][t28], [Factory card content per role][t34], [POC gaps][t52], [Track roles][t12], [Firmware changes the design needs][t43], [Dub's drop-outs and delay throws][t67], [Compose and Capture memory, limits and failures][t78].

[t12]: https://github.com/gismya/deluge-generative/issues/12
[t13]: https://github.com/gismya/deluge-generative/issues/13
[t25]: https://github.com/gismya/deluge-generative/issues/25
[t28]: https://github.com/gismya/deluge-generative/issues/28
[t34]: https://github.com/gismya/deluge-generative/issues/34
[t42]: https://github.com/gismya/deluge-generative/issues/42
[t43]: https://github.com/gismya/deluge-generative/issues/43
[t52]: https://github.com/gismya/deluge-generative/issues/52
[t67]: https://github.com/gismya/deluge-generative/issues/67

[t78]: https://github.com/gismya/deluge-generative/issues/78
