# 3. The Recipe

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 3.1 Contents

A **Recipe** holds no notes. It is:

- a **genre**, by stable name ([§4.4](04-genres.md#44-names-and-versions));
- the **musical facts** ([§3.2](#32-musical-facts));
- the four **macros** ([§3.3](#33-macros));
- each pitched part's **octave shift**, −2 to +2 octaves as far as its notes stay MIDI notes, 0 in a new Recipe: it moves the part's Register ([§7.3](07-pitch.md#73-register)). Drums and FX have none;
- the **sound picks**: one card path per synth part, and one sample path per lane of the Drums and FX parts ([§10](10-sounds.md#10-sounds));
- each **variant set**'s pick and allowed variants ([§3.6](#36-variant-sets)).

The **seed** and each part's **take** sit beside the Recipe, not in it ([§6](06-randomness.md#6-randomness-and-seeding)). Generative mode works on one **Current Recipe** with its seed and takes, kept in RAM until power-off ([§14.2](14-entry-and-compose-screen.md#142-the-current-recipe)).

### 3.2 Musical facts

| Fact | Range |
|---|---|
| Tempo | any tempo the Deluge supports, held as an integer in hundredths of a BPM, as `UNPATCHED_TEMPO` holds it. Float BPM appears only at the firmware edge. A genre's range is where rolls come from, not a clamp |
| Key root | C..B |
| Scale | one of the 12 seven-note preset scales: Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Melodic Minor, Harmonic Minor, Hungarian Minor, Marva, Arabian |
| Swing | song swing amount −49..49, plus swing interval. A genre's amount is a range, `swingLow`..`swingHigh` ([ADR 0008](../../adr/0008-a-piece-draws-its-genres-variants-into-the-recipe.md)), straight (0..0) by default; the interval is always the 16th in a new Recipe |

For each fact, the genre gives either a fixed default or a range or weighted list to roll from. Facts are rolled once, when a Recipe is created from a genre, from fresh entropy (not from the seed), and stored as concrete values. They roll in a fixed order from one stream of that entropy: tempo, root, scale, then swing, so a fact added later appends and leaves what any entropy gave before. Rolling again is always an explicit action: picking a genre, Regenerate, or a genre change. A genre change or Regenerate leans the key roll towards the playing key ([§9.7](09-arc.md#97-transitions)).

### 3.3 Macros

There are four macros: **Energy**, **Density**, **Complexity** and **Motion**. Each is 0–50 on the OLED and stored at full resolution. They are global: one centre every part follows. A part's **Part bias** moves where that part sits around it (below).

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

**Part bias.** Each part has a signed bias per macro, −50 to +50 (stored in macro units, 0 by default). The part plays every macro at its biased value: the shared value after the Arc (effective Energy and Density, the centre for Complexity and Motion), moved by the bias through one curve, the **50/50 blend**: the mean of two curves.

- The linear one, `v + bias`, clamped to 0–50.
- The headroom-proportional one, which moves `v` by `bias / 50` of the room towards the end the bias heads for (50 for a bias up, 0 for one down): `v + bias · (50 − v) / 50` up, `v + bias · v / 50` down.
- So through the middle a part moves about three quarters of its bias, and it reaches each end together with the shared value, so it keeps following the Arc there.
- Integer only ([ADR 0004](../../adr/0004-generator-decisions-use-integer-math-only.md)); the identity at bias 0, monotonic, within 0–50. The owner picked it by ear from a prototype (#414; the knee hybrid before it) and may swap it: it is one function (`partBiased()`, `core/part_bias.h`).

A part reads its biased value everywhere it reads a macro for its own notes and sound: velocity, thinning, its Density gate and late entries, the Complexity tier, lines, Answers, mutation, Ornament rate, the strength of its interplay rules, and its sound-control macro layer ([§11.3](11-sound-controls.md#113-three-layers)). An interplay rule plans its target at the target's own biased Density. What the parts share stays unbiased: the Arc, the chord walk and its Extensions, the Build roll's start, the automatic fill chance, Phrase accents, Groove length and drift ([§11.5](11-sound-controls.md#115-drift)). The SONG page's Arc plan and the MUTE pad's dim read each part's biased Density, so what they show is what plays ([§13.5](13-perform.md#135-control-layout)). Kit lanes follow their part: there is no bias per lane.

A bias is steering, logged where it lands ([§17](17-capture.md)), saved with the Recipe and in a Recipe file ([§16.1](16-recipe-files.md)). Regenerate keeps each part's bias, as it keeps the macros; a genre change resets every bias to 0, because the parts change; a LOAD plays the file's.

### 3.4 Steering grain

In Perform every Recipe value can be steered live. Each kind of value takes effect at its own grain:

- Macros: note-level effects from the next undecided step ([§12.2](12-runtime.md#122-when-steps-are-decided)), pattern-level effects from the next bar, harmonic effects from the next chord.
- Tempo and swing **glide** to the new value over the Perform glide setting ([§13.3](13-perform.md#133-perform-settings)). The OLED shows the target at once.
- A part's octave shift lands at the next bar line, as a mute does ([§13.4](13-perform.md#134-live-actions)). A shift past the part's octave range is refused ([§7.3](07-pitch.md#73-register)).
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

### 3.6 Variant sets

A genre may list up to eight **variant sets** ([§4.2](04-genres.md#42-slots), slot 11; [ADR 0008](../../adr/0008-a-piece-draws-its-genres-variants-into-the-recipe.md)). Each is one rule a piece draws a **variant** of, as it rolls key and tempo: Dub's skank plays 7th chords or plain triads. A set has 2–8 weighted variants. Its first is the genre as it played before the set was added, so adding a set whose first variant is today's moves no golden ([§21.2](21-testing.md#212-harness)). A set has a stable name and an OLED name of at most 8 characters; a variant has a stable name, an OLED name of at most 9 characters and a weight above 0. Each is unique in its list (`variantSetsValid()`, checked in `genreValid()`).

- **The Recipe's fields.** Per set, the variant playing (`pick`, by index in the genre's list) and the variants a draw may land on (`allowed`, a bit per variant): 16 bytes for eight sets. Every bit set is RANDOM, one bit a fixed variant, several a subset. They are stored, not derived from the seed, so a Recipe file ([§16.1](16-recipe-files.md#161-contents)), a Capture header ([§17.2](17-capture.md#172-the-steering-log)) and AS IT BEGAN carry them as they carry the key.
- **Drawing.** A new Recipe draws each set by weight within its allowed variants, from the same fresh entropy as the facts, after them ([§3.2](#32-musical-facts)), so a set never moves a fact any entropy gave. The draw order is tempo, root, scale, swing, then the sets, one value a set; a later draw appends after them.
- **When they're drawn.** Picking a genre draws the new genre's sets within every variant. A genre change and a Regenerate in Perform draw within what the Regenerate page allows: its VARIANTS rows open on RANDOM every time and may narrow a set for that one change ([§13.6](13-perform.md#136-perform-menu)). The Compose screen's REGENERATE draws within the Recipe's allowed variants ([§14.4](14-entry-and-compose-screen.md#144-the-compose-screen)). NEW SEED keeps every variant: a variant is a Recipe value, not a seeded draw ([§6.4](06-randomness.md#64-what-the-seed-covers)). A Recipe LOAD plays the file's picks and allowed variants; a set the file leaves out, or a pick it names that the genre doesn't have, is drawn within what the file allows ([§16.1](16-recipe-files.md#161-contents)).
- **Playing.** A variant is a patch: a function that writes fields of a RAM copy of the genre and its parts, the instance's **genre copy** (`ResolvedGenre`). An instance builds it when it begins (`Generator::begin()`: Perform's start and a transition's incoming instance, a restart and AS IT BEGAN, the Compose build, Capture's replay, the harness), applying each picked variant's patch in set order. A transition's incoming instance begins in task context; Perform's start, a restart and AS IT BEGAN begin in the tick, where the copy (1,396 B on ARM) sits next to the larger reset `begin()` already does (accepted in #503 R1; [ADR 0008](../../adr/0008-a-piece-draws-its-genres-variants-into-the-recipe.md) has the hardware figures). The step reads it as it reads a genre table: the step path does no new work. When no picked variant patches anything (a genre with no sets, or every set on a variant with no patch), the instance plays the constant table and nothing is copied. The copy sits outside the Generator block, owned beside it ([§20](20-budgets.md#20-budgets-and-measured-facts)); a Generator with no copy to write plays the table as written.
- **Changing at a Drop** (#495). A set with `changeAt = Drop` and a `changeChance` (1–1000 per mille) may change at each Drop: a keyed draw `(seed, kArcPart, VariantChange, the Drop's Groove occurrence, the set)` against the chance decides whether, and a second (its other field) picks the new variant by weight within the set's allowed variants, leaving out the one playing (`dropChange()`). A set that allows only the variant playing never changes. Being keyed on the seed, Capture replays it with nothing logged, and NEW SEED moves it. A **live** set (`live = true`) can be switched from Perform's VARIANTS ▸ ([§13.6](13-perform.md#136-perform-menu)): the pick is a `SetVariant` steering command (target the set, value the variant) that waits in the pending list, one slot a set, for the next Drop and lands there, logged on the Drop's step (ADR 0005), so Capture replays it. Picking the variant that plays cancels the one waiting, and BACK does too. Only a live set takes one; a variant the set didn't allow joins its allowed ones. A switch landing on a Drop replaces that Drop's draw for its set. Both happen at the decision of the Drop's first step, after the Arc enters it and before anything reads the genre there (the chord walk's loop draw and voicing, the pattern picks): when a pick moved, the instance applies its genre copy again from the constant genre with the new picks (`Generator::dropVariants()`), a 1.4 KB copy and the patches once at such a Drop. A transition's new roster opening on a Drop plays the picks it was drawn with. The instance keeps the picks playing in its Recipe (`Generator::variants()`); Perform's Current Recipe follows them each tick, as it follows the takes, so SAVE, RESTART ▸ AS IT IS and the Compose screen have the variant playing now. A Generator with no copy to write moves its picks and plays the table as written.
- **What a live or Drop-changing set may patch.** Only what a Drop reads afresh (pattern pools, fills, drop-out entries, a part's flavour data), never what the Arc's look-ahead reads across the line (Arc lengths, the Build, the Break, Registers), and a pool it patches keeps its entry count and each entry's length, with each part's lanes in their order, because a frozen part holds its pool entry across the Drop ([§6.7](06-randomness.md#67-freeze)) and plays that entry of the new pool. `variant_sets_spec` checks it for every genre by diffing each variant's copy against the genre's (the patch-diff spec).

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
