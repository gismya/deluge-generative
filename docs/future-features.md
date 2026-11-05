# Future features

Features for Generative mode that v1 leaves out on purpose. Each entry links the ticket that holds its reasoning or worked-out design.

- **Playing along over Perform**: the player plays a part, such as a lead, from a MIDI keyboard over the generated music. In v1, Perform drops every external note. See [External tempo and MIDI in Perform](https://github.com/gismya/deluge-generative/issues/51).
- **Recipe timeline in Compose**: Recipe values that change over the song, such as a key change at bar 64. v1 renders one fixed Recipe. See [Recipe parameters](https://github.com/gismya/deluge-generative/issues/10).
- **Crossfading a genre change**: new sounds on parallel Outputs, faded in. v1 swaps at a phrase boundary. See [Sound generation](https://github.com/gismya/deluge-generative/issues/13).
- **Player clips alongside Perform**: the player's own clips playing next to the Generator. See [Perform interaction model](https://github.com/gismya/deluge-generative/issues/15).
- **Player override of section lengths**: genres set Groove, Break and Build lengths in v1. See [Perform interaction model](https://github.com/gismya/deluge-generative/issues/15).
- **Variation when a sound is picked**: a seeded offset on the sound controls when a preset loads. See [Variation when a sound is picked](https://github.com/gismya/deluge-generative/issues/27).
- **Whole card kits as a sound pick**: the player points Drums or FX at a card KIT, and its rows are mapped to lanes. See [Kit lane mapping](https://github.com/gismya/deluge-generative/issues/25).
- **Recipes on 5- and 6-note scales**: Whole Tone, Blues, Pentatonic Minor and Hirajoshi. See [Mapping steps onto 5- and 6-note scales](https://github.com/gismya/deluge-generative/issues/29).
- **Settings for the chord guard and colour moves**: a player, genre or Recipe switching the guard or changing colour-move weights. See [Chord graph across a genre's scales](https://github.com/gismya/deluge-generative/issues/31).
- **Settings that change a part's Degree set**: a macro letting passing tones through, or a player or Recipe choosing the set. See [Parts that play some scale degrees](https://github.com/gismya/deluge-generative/issues/32).
- **Saving Capture logs during a set**: marked pieces that survive a power-off or crash. See [Capture](https://github.com/gismya/deluge-generative/issues/35).
