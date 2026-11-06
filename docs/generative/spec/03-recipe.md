# 3. The Recipe

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 3.1 Contents

A **Recipe** holds no notes. It is:

- a **genre**, by stable name ([§4.4](04-genres.md#44-names-and-versions));
- the **musical facts** ([§3.2](#32-musical-facts));
- the four **macros** ([§3.3](#33-macros));
- the **sound picks**: one card path per synth part, and one sample path per lane of the Drums and FX parts ([§10](10-sounds.md#10-sounds)).

The **seed** and each part's **take** sit beside the Recipe, not in it ([§6](06-randomness.md#6-randomness-and-seeding)). Generative mode works on one **Current Recipe** with its seed and takes, kept in RAM until power-off ([§14.2](14-entry-and-compose-screen.md#142-the-current-recipe)).

### 3.2 Musical facts

| Fact | Range |
|---|---|
| Tempo | any tempo the Deluge supports. A genre's range is where rolls come from, not a clamp |
| Key root | C..B |
| Scale | one of the 12 seven-note preset scales: Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Melodic Minor, Harmonic Minor, Hungarian Minor, Marva, Arabian |
| Swing | song swing amount −49..49, plus swing interval |

For each fact, the genre gives either a fixed default or a range or weighted list to roll from. Facts are rolled once, when a Recipe is created from a genre, from fresh entropy (not from the seed), and stored as concrete values. Rolling again is always an explicit action: picking a genre, Regenerate, or a genre change. A genre change or Regenerate leans the key roll towards the playing key ([§9.7](09-arc.md#97-transitions)).

### 3.3 Macros

There are four macros: **Energy**, **Density**, **Complexity** and **Motion**. Each is 0–50 on the OLED and stored at full resolution. They are global, with no per-part overrides.

- A macro value is the **centre** the Arc moves around, not a level. The Arc offsets effective Energy and effective Density only ([§9.4](09-arc.md#94-what-the-arc-moves)). It never moves Complexity or Motion.
- A macro moves its features the same way in every genre and part. A genre only weights how much each feature moves, through a per-genre macro weight table. Weights run ×0..×2 and can never flip a sign.
- Macros always start from the genre's defaults. They are never rolled. A genre change keeps them.

What each macro moves in v1:

| Macro | Moves |
|---|---|
| Energy | intensity: note velocity ([§8.4](08-rhythm-and-expression.md#84-velocity)), and the macro layer of the sound controls: brighter, shorter, more driven and drier as it rises ([§11.3](11-sound-controls.md#113-three-layers)) |
| Density | how many notes play: onsets thinned or filled by step importance ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)), and the part and lane **Density gates** ([§5.6](05-parts-and-lanes.md#56-gates)) |
| Complexity | how intricate each beat is: the Complexity tier of each pattern, within a genre-set band, and how Loop, Motif and Arp lines leap ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)); how often Ornaments happen ([§8.2](08-rhythm-and-expression.md#82-ornaments)); interplay rule strength ([§5.5](05-parts-and-lanes.md#55-interplay-rules)); the Extension level: how many in-scale Extensions (7, 9, sus4, 6) each chord gets, within the genre's floor and ceiling; how far the chord walk strays from home at phrase and Section starts ([§7.4](07-pitch.md#74-chords)); and when the Build roll starts ([§8.3](08-rhythm-and-expression.md#83-the-build-roll)). It never moves sound |
| Motion | how much and how fast the music changes: mutation probability (anchors never move); how often patterns change at phrase lines ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)); the Motif re-roll rate; drift depth and speed ([§11.5](11-sound-controls.md#115-drift)); how often fills and answers happen; and how eager the Arc is to move ([§9.3](09-arc.md#93-section-lengths)) |

Energy no longer decides which notes play. That is Density's job. No macro moves Lean, Velocity spread or a part's Degree set.

### 3.4 Steering grain

In Perform every Recipe value can be steered live. Each kind of value takes effect at its own grain:

- Macros: note-level effects from the next undecided step ([§12.2](12-runtime.md#122-when-steps-are-decided)), pattern-level effects from the next bar, harmonic effects from the next chord.
- Tempo and swing **glide** to the new value over the Perform glide setting ([§13.3](13-perform.md#133-perform-settings)). The OLED shows the target at once.
- Key and scale are queued to the next phrase line ([§9.7](09-arc.md#97-transitions)). A genre change, Regenerate or Recipe LOAD plays a transition ([§9.7](09-arc.md#97-transitions)), where tempo glides across the Build or Outro and swing switches where the new genre starts.

Compose v1 renders one Recipe that is fixed up front ([§14.4](14-entry-and-compose-screen.md#144-the-compose-screen)).

Sources: [Recipe parameters][t10], [Density as a fourth macro][t24], [Automation][t14], [Generator randomness and seeding][t17], [Mapping steps onto 5- and 6-note scales][t29], [Chord graph across a genre's scales][t31], [Sub-step expression][t20], [Harmony for static genres and chord extensions][t64], [Pattern choice under Complexity, and protected hits][t66].

[t10]: https://github.com/gismya/deluge-generative/issues/10
[t14]: https://github.com/gismya/deluge-generative/issues/14
[t17]: https://github.com/gismya/deluge-generative/issues/17
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t29]: https://github.com/gismya/deluge-generative/issues/29
[t31]: https://github.com/gismya/deluge-generative/issues/31
[t64]: https://github.com/gismya/deluge-generative/issues/64
[t66]: https://github.com/gismya/deluge-generative/issues/66
