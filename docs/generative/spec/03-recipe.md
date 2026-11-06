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
| Tempo | any tempo the Deluge supports, held as an integer in hundredths of a BPM, as `UNPATCHED_TEMPO` holds it. Float BPM appears only at the firmware edge. A genre's range is where rolls come from, not a clamp |
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
| Complexity | how intricate each beat is: the Complexity tier of each pattern, within a genre-set band, and how Loop, Motif and Arp lines leap ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)); which Answer transform a Loop plays, plain to intricate ([§5.2](05-parts-and-lanes.md#52-flavours)); how often Ornaments happen ([§8.2](08-rhythm-and-expression.md#82-ornaments)); interplay rule strength ([§5.5](05-parts-and-lanes.md#55-interplay-rules)); the Extension level: how many in-scale Extensions (7, 9, sus4, 6) each chord gets, within the genre's floor and ceiling; how far the chord walk strays from home at phrase and Section starts ([§7.4](07-pitch.md#74-chords)); and when the Build roll starts ([§8.3](08-rhythm-and-expression.md#83-the-build-roll)). It never moves sound |
| Motion | how much and how fast the music changes: mutation probability (anchors never move); how often patterns change at phrase lines ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)); the Motif re-roll rate; drift depth and speed ([§11.5](11-sound-controls.md#115-drift)); how often fills and answers happen; and how eager the Arc is to move ([§9.3](09-arc.md#93-section-lengths)). Each rate follows its own Motion curve ([§3.5](#35-motion-curves)) |

Energy no longer decides which notes play. That is Density's job. No macro moves Lean, Velocity spread or a part's Degree set.

### 3.4 Steering grain

In Perform every Recipe value can be steered live. Each kind of value takes effect at its own grain:

- Macros: note-level effects from the next undecided step ([§12.2](12-runtime.md#122-when-steps-are-decided)), pattern-level effects from the next bar, harmonic effects from the next chord.
- Tempo and swing **glide** to the new value over the Perform glide setting ([§13.3](13-perform.md#133-perform-settings)). The OLED shows the target at once.
- Key and scale are queued to the next phrase line ([§9.7](09-arc.md#97-transitions)). A genre change, Regenerate or Recipe LOAD plays a transition ([§9.7](09-arc.md#97-transitions)), where tempo glides across the Build or Outro and swing switches where the new genre starts.

Compose v1 renders one Recipe that is fixed up front ([§14.4](14-entry-and-compose-screen.md#144-the-compose-screen)).

### 3.5 Motion curves

Each rate Motion moves has a value at Motion 25, which the genre sets, and a **Motion curve** that scales it.

- A curve is two Generator-wide numbers: **R**, the multiple at Motion 50, and the **start** `z`, the multiple just above Motion 0. In between, the multiple is `z + (1 − z)(c^(M/25) − 1)/(c − 1)` with `c = (R − 1)/(1 − z)`, so it is `z` just above 0, 1 at Motion 25 and R at 50. R 2 with start 0 is a straight line through 0, and start 1/R is a plain exponential.
- **Motion 0 is "never"** on every curve: the multiple is 0, whatever the start.
- A chance is its Motion 25 value times the multiple, clamped at 100%. A length or a gap is divided by it.
- **Integer tables.** Each curve is a table of multiples in fixed point, indexed by Motion and interpolated between entries. It is generated at build time from R and the start, so Perform, Compose and the harness read the same integers and every keyed draw matches on every platform. A harness spec checks each table against the formula.
- Drift ([§11.5](11-sound-controls.md#115-drift)) isn't a rate and keeps its own rule.

| Rate | R | Start | Gives | Where |
|---|---|---|---|---|
| Groove length | 3 | 1/3 | a Groove of `b` ÷ 3 to 3 × `b` phrases | [§9.3](09-arc.md#93-section-lengths) |
| Fill chance and section-end chance | 2.5 | 0.05 | Techno's 35%: 3% at Motion 1, 88% at 50 | [§8.7](08-rhythm-and-expression.md#87-fill) |
| Phrase accent chance | 2.5 | 0.05 | the fill's curve, so accents fade with the fills | [§9.5](09-arc.md#95-section-rules) |
| Loop mutation | 3 | 0 | | [§5.2](05-parts-and-lanes.md#52-flavours) |
| Answers | 2 | 0.25 | every N bars at 25: about every 4N bars just above 0, N/2 at 50 | [§5.2](05-parts-and-lanes.md#52-flavours) |
| Drop-out chance | 2 | 0 | a straight line | [§8.8](08-rhythm-and-expression.md#88-drop-out) |
| Pattern re-pick chance | 2 | 0 | a straight line | [§8.1](08-rhythm-and-expression.md#81-onsets-and-density) |
| Motif re-roll | 2 | 0 | a straight line | [§5.2](05-parts-and-lanes.md#52-flavours) |

The numbers were picked by ear in a prototype and are *tuning*.

Sources: [Recipe parameters][t10], [Density as a fourth macro][t24], [Automation][t14], [Generator randomness and seeding][t17], [Mapping steps onto 5- and 6-note scales][t29], [Chord graph across a genre's scales][t31], [Sub-step expression][t20], [Harmony for static genres and chord extensions][t64], [Pattern choice under Complexity, and protected hits][t66], [How Motion scales Groove length, fills and drop-outs][t81], [Determinism details: tick unit, tempo, held notes and ramps][t77], [Should Complexity weight which Answer transform plays?][t82].

[t10]: https://github.com/gismya/deluge-generative/issues/10
[t14]: https://github.com/gismya/deluge-generative/issues/14
[t17]: https://github.com/gismya/deluge-generative/issues/17
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t29]: https://github.com/gismya/deluge-generative/issues/29
[t31]: https://github.com/gismya/deluge-generative/issues/31
[t64]: https://github.com/gismya/deluge-generative/issues/64
[t66]: https://github.com/gismya/deluge-generative/issues/66
[t81]: https://github.com/gismya/deluge-generative/issues/81
[t82]: https://github.com/gismya/deluge-generative/issues/82
[t77]: https://github.com/gismya/deluge-generative/issues/77
