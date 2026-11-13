# 4. Genres

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 4.1 What a genre is

A **genre** is a built-in, read-only style definition. It gives a new Recipe its starting values and sets the rules the Generator plays by. A Recipe refers to it by name and never copies its tables. The player can't edit a genre.

- Genres are constant tables in the Generator core, shared by firmware and harness. A new genre needs a firmware rebuild.
- Genres are **pure data** read by one generic Generator. New behaviour becomes a shared mechanism with a flag (for example "the Build rolls on snare or perc"), never code for one genre.
- Every field has a Generator-wide default, and a genre lists only the fields it changes. A genre may be written as overrides of another genre, as an authoring shortcut. Slots are optional: a genre with no drums leaves that slot empty and isn't a special case.
- Adding a field means adding it once with a default that keeps today's behaviour. It never means editing existing genres.
- A genre has **no pieces** to pick from. Variety comes from rolled facts, sound picks and the seed.
- **Subgenres.** A genre may name a **parent genre**, a table that is never played itself. The subgenre is written as overrides of its parent and inherits every field it doesn't list. In every other way a subgenre is a genre: it has its own name, version, roster, Harmony slot and palette, Regenerate stays inside it, and moving to a sibling is a genre change ([§9.7](09-arc.md#97-transitions)). The parent also groups its subgenres in the GENRE menu ([§14.1](14-entry-and-compose-screen.md#141-the-generative-menu)). A parent has no parent of its own.
- A part offers no alternatives inside a genre. Another kind of bassline, or another lead, is another subgenre.

### 4.2 Slots

| # | Slot | Contents | Spec |
|---|---|---|---|
| 1 | Identity | stable name, OLED name, genre version | [§4.4](#44-names-and-versions) |
| 2 | Musical facts | default or roll per fact | [§3.2](03-recipe.md#32-musical-facts) |
| 3 | Macros | default values, macro weight table | [§3.3](03-recipe.md#33-macros) |
| 4 | Meter and phrase | phrase length in bars, 8 by default. Always 4/4 on a 16th grid | [§4.3](#43-meter) |
| 5 | Part roster | 4–6 parts with role, flavour, gates, Register, Degree set, Lean, Velocity spread, ornament settings, Loop Answers (transforms with weights, and N at Motion 25), Sustain retrigger period and octave doubling | [§5](05-parts-and-lanes.md#5-parts-roles-flavours-and-lanes) |
| 6 | Rhythm | pattern pool per Drums lane and per patterned part (fill patterns included), each pattern with step importance, velocity levels, anchors and a Complexity tier; a tier band per part and lane; fill entries (kind, lane mask, length, weight), held-fill entry, fill chance and section-end chance, drop-out entries (mask, length, weight, throw target), drop-out chance and gap, interplay rules, Replace pairs, part order | [§5](05-parts-and-lanes.md#5-parts-roles-flavours-and-lanes), [§8](08-rhythm-and-expression.md#8-rhythm-and-expression) |
| 7 | Harmony | Chord graph (home pull, weighted moves, repeat weight, cadence target and period), harmonic rhythm, Extension floor and ceiling. Empty means the home chord only | [§7.4](07-pitch.md#74-chords) |
| 8 | Default palette | sound sources per part and lane | [§10](10-sounds.md#10-sounds) |
| 9 | Section rules | Arc offsets, Section lengths, Break and Build keeps, Build cut, roll, Gap, Drop decay, Intro and Outro curves and lengths, Phrase accent, transition entry | [§9](09-arc.md#9-the-arc) |
| 10 | Sound-control data | macro weights, gesture switches per part and moment, gesture depths per part, Throw delay depth and throw hold, drift weights, Sweep period per part, delay setup per part, kit row and kit | [§11](11-sound-controls.md#11-sound-controls-and-automation) |

### 4.3 Meter

Every v1 genre is 4/4 on a 16th-note grid. Phrase length is a genre value (Psytrance uses 16 bars). Triplet and shuffle feel come from swing and Lean ([§8.5](08-rhythm-and-expression.md#85-lean)).

### 4.4 Names and versions

- A genre has a stable lowercase name (such as `house`), used in Recipe files. Names are never reused.
- A subgenre's stable name is its parent's, a hyphen and its own (`techno-acid`). Its OLED name is the full one (`ACID TECHNO`), and a short name (`ACID`) labels its row under the parent in the GENRE menu.
- A parent's table carries no version. An edit to it that changes a subgenre's event stream bumps that subgenre's version.
- A genre has a **version number** that goes up whenever a retune changes its event stream. Recipe files store it. Old genre tables are not kept ([§16.2](16-recipe-files.md#162-genre-versions)). Golden files change in the same commit ([§21.3](21-testing.md#213-delugemu-local-only)).

### 4.5 v1 genres

The briefs below are what implementation fills tables against. Tables are hand-written, with PERF-1's genre tables as the main reference, but every value is our own, re-derived and tuned by ear. Copying PERF-1's tables row for row counts as porting (ADR 0001).

- **Techno** is a parent genre with four subgenres. The parent holds what they share: straight time, a four-on-the-floor kick, offbeat hats and a rim, and no claps on 2 and 4 (that's House). Long Builds and a kick drop at phrase ends. Groove 4–8 phrases (*tuning*). Fills: a Rest on Kick + Bass (1–4 beats) as the main entry and the held-fill entry, plus a Swap on Snare and Tom. Crash accent on Drops only (*tuning*). An FX part with Riser, Fall and Impact. Each subgenre sets its tempo range, scales, pitched parts, Harmony slot, Break keeps and palette, and may override anything else. Their pitched parts share some habits (*tuning*):
  - Minor chords over major ones. The Chord graphs favour i, iv, v, ♭VI and ♭VII, and an Extension is a 7th or a 9th far more often than a 6th.
  - Slow harmony: a chord lasts 2 bars at the least.
  - Every Pad opens over the phrase with an 8-bar Brightness Sweep ([§11.5](11-sound-controls.md#115-drift)).
  - A part that would crowd a default Groove gets a Density gate above the genre's default Density, so the Arc brings it in on Drops and the player brings it in by raising Density. The MUTE pads take any part in and out by hand ([§13.5](13-perform.md#135-control-layout)).

  The subgenres:
  - **Acid techno** (`techno-acid`, ACID TECHNO). 125–135 BPM, Minor or Phrygian. A mutating acid Loop bass with an 8-bar Sweep on its filter, and a **drone**: a pedal Sustain Pad (root and fifth) under everything, which the Break keeps. An empty Harmony slot, so one chord throughout. Parts: Drums, Acid, Drone, FX. It is the default genre, the first row the GENRE menu opens on before any Recipe exists.
  - **Melodic techno** (`techno-melodic`, MELODIC TECHNO). 122–128 BPM, Minor, Phrygian or Harmonic Minor. A Chord graph with home i and the moves i–♭VI, ♭VI–♭VII, ♭VII–i, i–iv, iv–♭VI and i–♭VII, 2 bars per chord, cadence target ♭VII every 4 chords, and Extension ceiling 1. In Harmonic Minor the 7th degree's chord is diminished, so the guard swaps the cadence to V and its raised leading tone, which is what that scale is there for. A rolling bass Loop on the three 16ths after each beat, mostly root and octave, the 5th now and then, rare slides. Its patterns leave the kick's steps empty, so it needs no Avoid rule. A Pad voicing each chord, and an Arp lead on 16ths, Up or Up-down over one or two octaves, with a 16-bar Sweep. Neither is gated: Density thins the Arp by step importance instead. Break keeps: Pad, Arp and Closed hat, so the breakdown is the pad and the arp alone. Parts: Drums, Bass, Pad, Arp, FX.
  - **Detroit techno** (`techno-detroit`, DETROIT TECHNO). 125–135 BPM, Minor or Dorian. A Chord graph that matters: home i, the moves i–♭VII, i–iv, i–v, iv–v and ♭VI–♭VII, with Dorian's i–IV from the colour moves, 2 bars per chord, repeat weight 0. The walk's phrase rule keeps home off a phrase's last chord, so bars 7–8 always turn the loop round. Extension floor 1 and ceiling 2, so every chord carries an Extension, usually the 7th, and a second comes with Complexity. A Bass Loop on root rhythm: the chord root on most beats and on the & before the next, the octave and 5th with Complexity. A Strings Pad, a Stab (Chords) on syncopated 16ths that keep off the 1, and a Motif lead: a one- or two-bar riff stored as chord positions, so it follows each change. The Motif is gated at 30 (*tuning*), so it comes in on Drops. Break keeps: Strings, Stab and Closed hat. Parts: Drums, Bass, Strings, Stab, Motif, FX.
  - **Dub techno** (`techno-dub`, DUB TECHNO). 115–125 BPM, Minor or Dorian. An empty Harmony slot with Extension floor 1 and ceiling 2: one m7 or m9 chord throughout, repeated by the stab while its sound moves (*tuning*: a slow i–iv walk at 8 bars per chord if one chord is too static). A Stab (Chords) on the & and the 16th after it, soft, with ghosted repeats, from presets that carry their own echo until delay setups and delay Throws arrive with Dub ([§10.6](10-sounds.md#106-load-time-settings), [§11.4](11-sound-controls.md#114-gestures)). A sub Bass Loop: long root notes, the octave below now and then, no slides. A pedal Pad washing under the stab. Lighter drums: kick, rim, hats and a shaker on Perc 2. Break keeps: Stab, Pad and Rim. Parts: Drums, Sub, Stab, Pad, FX. Not to be confused with **Dub** below, which is roots dub at 70–80 BPM.
- **House.** 120–126 BPM, moderate 16th swing, Minor, Dorian or Major. Claps on 2 and 4, an offbeat bass between the kicks, chord stabs with 7th and 9th Extensions (Extension floor 1, so never a bare triad), a Chord graph that matters, and 2–4 bar harmonic rhythm. Hats lean late on the e and the a (for example 0, +3, 0, +3). Fills: a Swap on Clap and Snare (the held-fill entry), plus a Roll on Snare, with a fill chance of 35% at Motion 25, so fills reach 88% of phrases at Motion 50 rather than every one (*tuning*). Crash accent on Drops and some phrase starts (*tuning*).
- **Dub** (roots dub, not dub techno). 70–80 BPM, Minor, light swing. One-drop drums, an offbeat skank, a heavy bass that carries the tune, and delay and reverb throws and drop-outs as the main motion. It has a **Siren** FX lane ([§10.5](10-sounds.md#105-fx-lanes)). Its step tables give the beat-3 backbeat to only one of Rim and Snare, and keep the Crash off strong hat steps. A lazy snare leans late (for example +2 everywhere). Fills: a Swap on Snare and Tom (the return fill, and the held-fill entry), and one that moves the Rim's backbeat to the Snare. Crash accent rare (*tuning*). Its drop-outs are not fills: they are drop-out entries ([§8.8](08-rhythm-and-expression.md#88-drop-out)), such as the skank for 2 bars thrown, the drums but the kick for 1 bar with the Rim thrown, or everything but the bass for 4 bars with the melodica thrown. The skank, melodica and Rim have a dotted-8th delay setup ([§10.6](10-sounds.md#106-load-time-settings)) so their Throws echo (*tuning*).
- **Psytrance.** 138–146 BPM, Minor, Phrygian or Harmonic Minor, straight. Kick plus a rolling 16th bass that skips the kick step (Bass Avoid Kick at 100%, sensitivity 0). A pedal-tone root with no chord changes (an empty Harmony slot), filter-swept leads and arps, 16-bar phrases, and long builds into hard drops. Groove 3–6 phrases (*tuning*). Fills: a Rest on Kick + Bass (the held-fill entry), plus a Roll on Snare. Crash accent on Drops (*tuning*).
- **Ambient.** 60–90 BPM, Major, Lydian or Dorian. No drums, or soft sparse percussion behind a high Density gate (about 35). Pads and drones with 8+ bar harmonic rhythm, which may span phrases, and a repeat weight so a chord can hold longer still. The Arc swells and recedes and never drops: its section rules make the Build a slow swell with no Gap, no roll and no Throw, and its Drop lift is near 0 with a long decay ([§9.5](09-arc.md#95-section-rules)). It has no Impact lane, no fills and no accent lane. Groove 6–12 phrases (*tuning*). A change into Ambient fades through by default (its transition entry is Intro, [§9.7](09-arc.md#97-transitions)).

Sources: [Genre model][t11], [Recipe parameters][t10], [Perform interaction model][t15], [Porting policy][t23], [Recipe files][t40], [Generative pack contents][t42], [POC gaps][t52], [Sub-step expression][t20], [Harmony for static genres and chord extensions][t64], [Fill kinds and phrase-end events][t65], [Pattern choice under Complexity, and protected hits][t66], [Held FILL and how a Rest ends sounding notes][t79], [Should Complexity weight which Answer transform plays?][t82], POC findings on the [map][poc].

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
[t82]: https://github.com/gismya/deluge-generative/issues/82
