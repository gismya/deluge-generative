# 4. Genres

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 4.1 What a genre is

A **genre** is a built-in, read-only style definition. It gives a new Recipe its starting values and sets the rules the Generator plays by. A Recipe refers to it by name and never copies its tables. The player can't edit a genre.

- Genres are constant tables in the Generator core, shared by firmware and harness. A new genre needs a firmware rebuild.
- Genres are **pure data** read by one generic Generator. New behaviour becomes a shared mechanism with a flag (for example "the Build rolls on snare or perc"), never code for one genre.
- Every field has a Generator-wide default, and a genre lists only the fields it changes. A genre may be written as overrides of another genre, as an authoring shortcut. Slots are optional: a genre with no drums leaves that slot empty and isn't a special case.
- Adding a field means adding it once with a default that keeps today's behaviour. It never means editing existing genres.
- A genre has **no pieces** to pick from. Variety comes from rolled facts, sound picks and the seed.

### 4.2 Slots

| # | Slot | Contents | Spec |
|---|---|---|---|
| 1 | Identity | stable name, OLED name, genre version | [§4.4](#44-names-and-versions) |
| 2 | Musical facts | default or roll per fact | [§3.2](03-recipe.md#32-musical-facts) |
| 3 | Macros | default values, macro weight table | [§3.3](03-recipe.md#33-macros) |
| 4 | Meter and phrase | phrase length in bars, 8 by default. Always 4/4 on a 16th grid | [§4.3](#43-meter) |
| 5 | Part roster | 4–6 parts with role, flavour, gates, Register, Degree set, Lean, Velocity spread, ornament settings, Sustain retrigger period and octave doubling | [§5](05-parts-and-lanes.md#5-parts-roles-flavours-and-lanes) |
| 6 | Rhythm | pattern pool per Drums lane and per patterned part (fill patterns included), each pattern with step importance, velocity levels, anchors and a Complexity tier; a tier band per part and lane; fill entries (kind, lane mask, length, weight), held-fill entry, fill chance and section-end chance, drop-out entries (mask, length, weight, throw target), drop-out chance and gap, interplay rules, Replace pairs, part order | [§5](05-parts-and-lanes.md#5-parts-roles-flavours-and-lanes), [§8](08-rhythm-and-expression.md#8-rhythm-and-expression) |
| 7 | Harmony | Chord graph (home pull, weighted moves, repeat weight, cadence target and period), harmonic rhythm, Extension floor and ceiling. Empty means the home chord only | [§7.4](07-pitch.md#74-chords) |
| 8 | Default palette | sound sources per part and lane | [§10](10-sounds.md#10-sounds) |
| 9 | Section rules | Arc offsets, Section lengths, Break and Build keeps, Build cut, roll, Gap, Drop decay, Intro and Outro curves and lengths, Phrase accent | [§9](09-arc.md#9-the-arc) |
| 10 | Sound-control data | macro weights, gesture switches per part and moment, gesture depths per part, Throw delay depth and throw hold, drift weights, Sweep period per part, delay setup per part, kit row and kit | [§11](11-sound-controls.md#11-sound-controls-and-automation) |

### 4.3 Meter

Every v1 genre is 4/4 on a 16th-note grid. Phrase length is a genre value (Psytrance uses 16 bars). Triplet and shuffle feel come from swing and Lean ([§8.5](08-rhythm-and-expression.md#85-lean)).

### 4.4 Names and versions

- A genre has a stable lowercase name (such as `techno`), used in Recipe files. Names are never reused.
- A genre has a **version number** that goes up whenever a retune changes its event stream. Recipe files store it. Old genre tables are not kept ([§16.2](16-recipe-files.md#162-genre-versions)). Golden files change in the same commit ([§21.3](21-testing.md#213-delugemu-local-only)).

### 4.5 v1 genres

The briefs below are what implementation fills tables against. Tables are hand-written, with PERF-1's genre tables as the main reference, but every value is our own, re-derived and tuned by ear. Copying PERF-1's tables row for row counts as porting (ADR 0001).

- **Techno.** 125–135 BPM, Minor or Phrygian, straight. Four-on-the-floor kick, offbeat hats and a mutating acid line. Sparse harmony: an empty Harmony slot, so one chord throughout. Long builds and a kick drop at phrase ends. No claps on 2 and 4: that's House. Groove 4–8 phrases (*tuning*). Fills: a Rest on Kick + Bass (1–4 beats) as the main entry and the held-fill entry, plus a Swap on Snare and Tom. Crash accent on Drops only (*tuning*).
- **House.** 120–126 BPM, moderate 16th swing, Minor, Dorian or Major. Claps on 2 and 4, an offbeat bass between the kicks, chord stabs with 7th and 9th Extensions (Extension floor 1, so never a bare triad), a Chord graph that matters, and 2–4 bar harmonic rhythm. Hats lean late on the e and the a (for example 0, +3, 0, +3). Fills: a Swap on Clap and Snare (the held-fill entry), plus a Roll on Snare. Crash accent on Drops and some phrase starts (*tuning*).
- **Dub** (roots dub, not dub techno). 70–80 BPM, Minor, light swing. One-drop drums, an offbeat skank, a heavy bass that carries the tune, and delay and reverb throws and drop-outs as the main motion. It has a **Siren** FX lane ([§10.5](10-sounds.md#105-fx-lanes)). Its step tables give the beat-3 backbeat to only one of Rim and Snare, and keep the Crash off strong hat steps. A lazy snare leans late (for example +2 everywhere). Fills: a Swap on Snare and Tom (the return fill, and the held-fill entry), and one that moves the Rim's backbeat to the Snare. Crash accent rare (*tuning*). Its drop-outs are not fills: they are drop-out entries ([§8.8](08-rhythm-and-expression.md#88-drop-out)), such as the skank for 2 bars thrown, the drums but the kick for 1 bar with the Rim thrown, or everything but the bass for 4 bars with the melodica thrown. The skank, melodica and Rim have a dotted-8th delay setup ([§10.6](10-sounds.md#106-load-time-settings)) so their Throws echo (*tuning*).
- **Psytrance.** 138–146 BPM, Minor, Phrygian or Harmonic Minor, straight. Kick plus a rolling 16th bass that skips the kick step (Bass Avoid Kick at 100%, sensitivity 0). A pedal-tone root with no chord changes (an empty Harmony slot), filter-swept leads and arps, 16-bar phrases, and long builds into hard drops. Groove 3–6 phrases (*tuning*). Fills: a Rest on Kick + Bass (the held-fill entry), plus a Roll on Snare. Crash accent on Drops (*tuning*).
- **Ambient.** 60–90 BPM, Major, Lydian or Dorian. No drums, or soft sparse percussion behind a high Density gate (about 35). Pads and drones with 8+ bar harmonic rhythm, which may span phrases, and a repeat weight so a chord can hold longer still. The Arc swells and recedes and never drops: its section rules make the Build a slow swell with no Gap, no roll and no Throw, and its Drop lift is near 0 with a long decay ([§9.5](09-arc.md#95-section-rules)). It has no Impact lane, no fills and no accent lane. Groove 6–12 phrases (*tuning*).

Sources: [Genre model][t11], [Recipe parameters][t10], [Perform interaction model][t15], [Porting policy][t23], [Recipe files][t40], [Generative pack contents][t42], [POC gaps][t52], [Sub-step expression][t20], [Harmony for static genres and chord extensions][t64], [Fill kinds and phrase-end events][t65], [Pattern choice under Complexity, and protected hits][t66], [Held FILL and how a Rest ends sounding notes][t79], POC findings on the [map][poc].

[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t10]: https://github.com/gismya/deluge-generative/issues/10
[t11]: https://github.com/gismya/deluge-generative/issues/11
[t15]: https://github.com/gismya/deluge-generative/issues/15
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t23]: https://github.com/gismya/deluge-generative/issues/23
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t42]: https://github.com/gismya/deluge-generative/issues/42
[t52]: https://github.com/gismya/deluge-generative/issues/52
[t64]: https://github.com/gismya/deluge-generative/issues/64
[t65]: https://github.com/gismya/deluge-generative/issues/65
[t66]: https://github.com/gismya/deluge-generative/issues/66
[t79]: https://github.com/gismya/deluge-generative/issues/79
