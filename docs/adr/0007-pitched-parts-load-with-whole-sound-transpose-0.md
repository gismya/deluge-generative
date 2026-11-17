# Pitched parts load with their whole-sound transpose at 0

A Deluge synth preset can transpose the whole sound (`Sound::transpose`) as well as each oscillator. 73 of the factory card's 171 presets set it: mostly −12 and −24, some +12 and +24, and 8 at odd values (−11, −8, −5, −3, +6, +7). M1's palettes mixed −24 presets with plain ones in one part's list, so the same Register sounded two octaves apart from pick to pick (#252). We decided that when the Generator loads a synth preset into a pitched part (Bass, Lead, Chords, Pad), it sets the whole-sound transpose to 0 and leaves the oscillators' transposes alone. A Register then means the pitch heard, whatever the pick. Decision: the user, in the session that grilled M2's milestone ticket (#88).

## Considered options

- **One transpose per list.** A check would require every preset in one part's list to share its whole-sound transpose, and the Register would be set for it, as Detroit techno's Bass is. Rejected: it narrows what each part can draw from.
- **An octave per palette entry**, which the Generator adds to the note. Rejected: the event stream would then depend on the sound pick, which the core never reads (§10.1).
- **Leave it to the ear** during tuning. Rejected: it would only find the mismatches someone happened to hear.

## Consequences

- §10.6's "the Generator never changes tuning" has one exception, the whole-sound transpose of a pitched part's preset. A preset chosen in the browser and a palette override's preset get it too, and Compose saves the 0 with the song.
- The 8 off-key presets become usable, because their odd transpose is the whole-sound one.
- Registers that make up for a −24 preset (Detroit techno's Bass, Dub techno's Sub and others) move down two octaves. The pitch heard is unchanged, but the note numbers in the event stream change, so each Techno subgenre this touches is retuned and its version bumped.
- A preset that puts its octave on every oscillator, and none on the whole sound, still sounds off. A local check against the card image flags such palette presets.
- FX synths (Riser, Fall, Impact, Siren) and Kit rows keep their transpose.
