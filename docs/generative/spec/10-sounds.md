# 10. Sounds

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 10.1 Palettes and sound picks

- Synth sounds are card presets, chosen through a **palette**. There are no built-in sound templates and no preset mutation. The firmware bundles no sounds.
- Each genre has a built-in **default palette** (slot 8), constant data in the core, so the harness reads it with no file parsing. Entries are keyed by part, with a role-level section as the default for parts that aren't listed. Each entry is a weighted list of preset paths, which can point anywhere on the card.
- A card file `GENERATIVE/PALETTES/<GENRE NAME>.XML` is an optional **override**. Each entry it lists (a part, a role default or a lane) replaces the built-in entry, and everything else falls back. v1 ships no override files, but the firmware reads one if a player writes it.
- **Override format.** One `<palette>` element holding `<part name="ACID">`, `<role name="bass">` and `<lane name="kick">` sections. A part or role lists `<preset path="SYNTHS/…" weight="3"/>`. A lane lists `<folder path="…"/>`, `<tag text="…" weight="…"/>`, `<file path="…"/>` (one exact file) and, for an FX lane, `<preset …/>`. A weight is 0–255 and defaults to 1. Role names are lowercase (`bass`, `fx`) and lane names camel case (`closedHat`, `perc1`). Unknown elements and names are skipped. **Limits:** the file is at most 8 KB; its attribute text (names, paths, tags) at most 2 KB in all; at most 64 presets and 64 tags across the file, and 32 folders; at most 8 presets and 8 tags in one part, role or lane; each part, role and lane listed once. A file past a limit, or malformed, is ignored and the built-in palette plays.
- A palette changes only what the music sounds like, never how it plays.
- A **sound pick** is a concrete path rolled (from fresh entropy, by weight) when a Recipe is created and stored in the Recipe. It is kept until the player re-rolls it or changes it. The player can point any part at any card preset, and any single lane at any sample file.
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

- Riser, Fall, Impact and Siren look for samples in `SAMPLES/FX/<lane>` first. If none, they load a Generative pack synth preset into the kit row through `StorageManager::loadSynthToDrum`, which takes a `Song*` and is safe on a private song. Its only caller does the follow-up on `currentSong`, so the Generator needs its own follow-up on `song`.
- Riser lanes list only pack risers. Fall and Impact use factory files plus pack fallbacks. Siren is always a pack synth preset.
- FX lanes always use their own row controls, so a synth Riser sweeps its own row ([§11.4](11-sound-controls.md#114-gestures)).

### 10.6 Load-time settings

When the Generator loads a part's sound, it does these once, before playback. None is a sound control, and nothing changes them live.

- Sets the voice mode (`Sound::polyphonic`) to the part's mode ([§5.8](05-parts-and-lanes.md#58-voice-modes)).
- **Portamento.** Parts that slide (Loop) need portamento: if the preset's is off, set a role default; if it's on, keep it. Every other part gets portamento 0. Portamento is `UNPATCHED_PORTAMENTO`, an `AutoParam`, so Compose writes it as a normal clip value.
- Switches the clip's arp off. Old presets store `<arpeggiator><mode>`, which the clip copies on load (`instrument_clip.cpp`). This makes the 15 factory presets with a saved arp usable, 165 Acid Arp included.
- Sets `Sound::voicePriority` by role: High for Drums and Bass, Medium for Lead and Chords, Low for Pad and FX. CPU-load culling then thins the pad before the kick or bass drops a note.
- **Delay setup.** A genre may give a part, a kit row or a whole kit a delay setup (slot 10): sync level and type (even, dotted or triplet, such as a dotted 8th), ping-pong on or off, and analog on or off. These are plain fields on the `Delay` (`syncLevel`, `syncType`, `pingPong`, `analog`), not params, and Compose saves them with the song. Feedback home stays as the preset saved it, often 0. A delay setup is what lets Throw raise delay feedback ([§11.4](11-sound-controls.md#114-gestures)). The stock render allocates the buffer itself once feedback is above its threshold and sound is coming in, from SDRAM (`allocLowSpeed`, up to 88,200 stereo samples, about 690 KB), and frees it when the repeats run out (`dsp/delay/delay.cpp`). Every delay preset already works this way, so no non-zero home is needed. Kit rows are Sounds with their own delay, and the kit has one at kit level.
- Sets CHOKE on the Open hat and Closed hat rows, and on no other row. A CHOKE drum cuts every other CHOKE drum in its Kit (`kit->choke()`), and there's only one group per Kit.

The Generator never changes oscillator type, synth mode, tuning or mod routing. All of the above are plain field writes and need no firmware change.

### 10.7 When sounds load

- Loading runs in task context as a runner job ([§12.10](12-runtime.md#1210-long-running-work-and-input)), never in a tick. Card loads can't run inside the audio routine.
- Entering Perform or loading a Recipe loads every sound before playback.
- A genre change, Regenerate or Recipe LOAD preloads the incoming roster into fresh Outputs while the old one plays ([§9.7](09-arc.md#97-transitions)).
- **Card errors.** If a preload fails because the card errors or was pulled, the transition or re-roll is cancelled as BACK would cancel it, the playing roster carries on, and the OLED shows the stock card error. Missing-sound re-rolls ([§16.6](16-recipe-files.md#166-missing-sounds-and-sharing)) cover files that are absent, not card errors.
- A sound re-roll in Perform preloads into a fresh Output, then swaps at the next phrase line. A transition confirm cancels one still preloading ([§12.9](12-runtime.md#129-instances-and-the-transition)).
  - NEW SOUND rolls the part's picks again from its palette and fresh entropy, each pick moving away from the one it replaces where the palette offers another (another entry for a synth part; for a lane, the next file of the tag it rolled, or another tag when that tag has only the old file), so it's heard as new. A Kit part re-rolls every lane it plays. Back to the previous sound loads the part's previous picks.
  - The preload is a live runner job ([§12.10](12-runtime.md#1210-long-running-work-and-input)). It builds the part's new Output beside the old one and links it into Perform's song in one chunk at its end, silent until notes go to it, then queues SetSound for the part. The Sound event at the line swaps it in: the sound controls bind to its params with its preset's homes, at the offsets the old sound had ([§11.6](11-sound-controls.md#116-home-and-player-edits-perform)), and the gold knobs move onto it if the part is focused. The old Output retires as a transition's outgoing Outputs do ([§12.9](12-runtime.md#129-instances-and-the-transition)): freed by a live job once silent by §12.9's test, or a phrase after the swap at the latest, counted at the tempo then so it runs on while stopped. A stop leaves its tail ringing too. The cap's 1-beat fade comes with #115's retiring; until then an Output still sounding at the cap is cut. The free also deletes every audio file no song uses, so memory comes back with the old sound. A re-roll is refused with the busy popup while the Outputs waiting to be freed could fill their list.
  - Pressing the part's NEW SOUND (or SHIFT, or SYNTH or KIT) again cancels its sound still preloading or waiting for the line. A BACK tap cancels a preload still running first, as the newest action, and otherwise drops the newest queued action, which may be a waiting sound. A stop cancels a waiting sound like every pending action. While stopped there's no line: a re-roll swaps in as soon as its preload ends, and the next start plays it.
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
