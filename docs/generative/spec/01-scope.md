# 1. Scope

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 1.1 What v1 is

Generative mode adds two modes that share one **Generator**:

- **Perform** plays live from the **Current Recipe**, steered in real time, in a song of its own.
- **Compose** renders the Current Recipe into a regular, editable Deluge song: notes, sounds and automation in session and arranger view.

Perform can also keep what it played through **Capture**, which turns played **pieces** into songs. Recipes can be saved and loaded as **Recipe files**.

v1 ships eight built-in genres: four Techno subgenres (Acid, Melodic, Detroit and Dub techno), House, Dub, Psytrance and Ambient ([§4.5](04-genres.md#45-v1-genres)).

### 1.2 Out of scope for v1

Each of these was ruled out on purpose. Later features among them are also listed in [`docs/future-features.md`](../../future-features.md), with a link to the ticket that holds the worked-out design.

- MIDI/CV output to external gear. The output seam ([§2.3](02-architecture.md#23-the-output-seam)) keeps it cheap to add.
- Generating or resampling samples. Sounds come only from the synth engines and from samples on the card.
- 7-segment display support. Upstreaming to the community firmware.
- A Recipe timeline in Compose: Recipe values that change over the song. Compose v1 renders one fixed Recipe. Capture replays steering ([§17](17-capture.md#17-capture)), which is a different thing.
- Genres loaded from the card or written by users. Corpus-derived genre tables. Meters other than 4/4.
- Crossfading a genre change through parallel Outputs.
- Player clips playing alongside Perform. A player override of section lengths.
- Variation when a sound is picked: a seeded offset on the sound controls at load.
- Whole card kits as a sound pick.
- Recipes on 5- and 6-note scales (Whole Tone, Blues, Pentatonic Minor, Hirajoshi).
- Settings for the chord guard and colour moves, and settings that change a part's Degree set. The data must leave room for both ([§7.4](07-pitch.md#74-chords), [§7.5](07-pitch.md#75-degree-sets)).
- Saving Capture logs to the card during a set.
- Playing along over Perform from a MIDI keyboard. Perform drops every external note.
- Polymeter (§23.2). Energy moving a part's Register (§23.4).
- Typing a seed in. The seed is shown, never entered ([§6.5](06-randomness.md#65-seed-source-and-display)).
- Measuring the Generator's own per-step cost on hardware. That needs Generator code, so it is implementation work against the budgets in [§20](20-budgets.md#20-budgets-and-measured-facts).

Sources: [map][map], [Recipe parameters][t10], [Genre model][t11], [Variation when a sound is picked][t27], [Kit lane mapping][t25], [Mapping steps onto 5- and 6-note scales][t29], [Chord graph across a genre's scales][t31], [Parts that play some scale degrees][t32], [Capture][t35], [External tempo and MIDI in Perform][t51].

[map]: https://github.com/gismya/deluge-generative/issues/1
[t10]: https://github.com/gismya/deluge-generative/issues/10
[t11]: https://github.com/gismya/deluge-generative/issues/11
[t25]: https://github.com/gismya/deluge-generative/issues/25
[t27]: https://github.com/gismya/deluge-generative/issues/27
[t29]: https://github.com/gismya/deluge-generative/issues/29
[t31]: https://github.com/gismya/deluge-generative/issues/31
[t32]: https://github.com/gismya/deluge-generative/issues/32
[t35]: https://github.com/gismya/deluge-generative/issues/35
[t51]: https://github.com/gismya/deluge-generative/issues/51
