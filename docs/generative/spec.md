# Generative mode spec

This is the consolidated v1 design for **Generative mode** on this fork of the Deluge community firmware. It folds in every decision ticket on the [wayfinder map][map], with each amendment applied, so an implementer shouldn't need the ticket history. Each section ends with the tickets it comes from. Read those for the reasoning, not for the rules. Where a ticket and this spec disagree, this spec wins, and §23 lists every place where writing it settled something.

## 0. How to read this spec

- **Base.** Upstream community firmware tag `v1.3.0`, on this fork's `generative` branch. OLED hardware only. Firmware paths are relative to `src/deluge/` and are checked against `v1.3.0` unless a line says rc3. Older tickets cite `v1.3.0-rc3`, and `src/` differs between the two (22 commits, 50 files), so check any rc3 line number before using it.
- **Words.** The terms in bold are defined in [`GLOSSARY.md`](../../GLOSSARY.md), and this spec uses them exactly. A part is never a "track", a Section is never a "scene", and the Deluge's own numbered clip groups are **session sections**.
- **ADRs.** Four records in [`docs/adr/`](../adr/) hold decisions whose reasoning matters for later changes:
  - [ADR 0001 Porting policy](../adr/0001-porting-policy.md): reimplement by default.
  - [ADR 0002 Perform plays notes through a Generator hook](../adr/0002-perform-plays-through-a-generator-hook.md), not through clips.
  - [ADR 0003 The Generator decides each step 12 ticks ahead](../adr/0003-generator-decides-steps-12-ticks-ahead.md), so Lean can push.
  - [ADR 0004 Generator decisions use integer math only](../adr/0004-generator-decisions-use-integer-math-only.md).
- **Tuning data.** Many values here are marked *tuning*. They are starting data, tuned by ear later (§21.5). An implementation slice puts them in genre or Generator-wide tables with the stated starting value. It doesn't need to get them right first time.
- **Fixed rules.** Anything not marked *tuning* is a rule. Changing it means amending this spec.
- **Section numbers** are stable. Implementation tickets cite them as `spec §9.4`.

## 1. Scope

### 1.1 What v1 is

Generative mode adds two modes that share one **Generator**:

- **Perform** plays live from the **Current Recipe**, steered in real time, in a song of its own.
- **Compose** renders the Current Recipe into a regular, editable Deluge song: notes, sounds and automation in session and arranger view.

Perform can also keep what it played through **Capture**, which turns played **pieces** into songs. Recipes can be saved and loaded as **Recipe files**.

v1 ships five built-in genres: Techno, House, Dub, Psytrance and Ambient (§4.5).

### 1.2 Out of scope for v1

Each of these was ruled out on purpose. Later features among them are also listed in [`docs/future-features.md`](../future-features.md), with a link to the ticket that holds the worked-out design.

- MIDI/CV output to external gear. The output seam (§2.3) keeps it cheap to add.
- Generating or resampling samples. Sounds come only from the synth engines and from samples on the card.
- 7-segment display support. Upstreaming to the community firmware.
- A Recipe timeline in Compose: Recipe values that change over the song. Compose v1 renders one fixed Recipe. Capture replays steering (§17), which is a different thing.
- Genres loaded from the card or written by users. Corpus-derived genre tables. Meters other than 4/4.
- Crossfading a genre change through parallel Outputs.
- Player clips playing alongside Perform. A player override of section lengths.
- Variation when a sound is picked: a seeded offset on the sound controls at load.
- Whole card kits as a sound pick.
- Recipes on 5- and 6-note scales (Whole Tone, Blues, Pentatonic Minor, Hirajoshi).
- Settings for the chord guard and colour moves, and settings that change a part's Degree set. The data must leave room for both (§7.4, §7.5).
- Saving Capture logs to the card during a set.
- Playing along over Perform from a MIDI keyboard. Perform drops every external note.
- Polymeter (§23.2). Energy moving a part's Register (§23.4).
- Typing a seed in. The seed is shown, never entered (§6.5).
- Measuring the Generator's own per-step cost on hardware. That needs Generator code, so it is implementation work against the budgets in §20.

Sources: [map][map], [Recipe parameters][t10], [Genre model][t11], [Variation when a sound is picked][t27], [Kit lane mapping][t25], [Mapping steps onto 5- and 6-note scales][t29], [Chord graph across a genre's scales][t31], [Parts that play some scale degrees][t32], [Capture][t35], [External tempo and MIDI in Perform][t51].

## 2. Architecture

### 2.1 The Generator core

- One portable C++ **Generator core** lives in the firmware tree. It has no firmware globals: no `currentSong`, no `display`, no root UI and no firmware allocator. The same sources build into the firmware and into the `tests/` harness (§21).
- Genres are constant data in the core, read by one generic Generator (§4).
- The core has no globals of its own. All state of one Generator instance lives in one fixed-size block, sized for the largest roster (6 parts, every lane). The block is allocated once from the internal heap when Perform is entered or a Compose or Capture build starts. The core never allocates, never touches the card, and uses no `new` and no std containers.
- The core uses only its own integer pseudo-random hash (§6), and integer math only for any decision that can change the event stream (ADR 0004). Probabilities and weights are integers on a fixed scale (per-mille or 0–255), never floats in 0–1.

### 2.2 Drivers and the event stream

The core exposes one **step function**. Three **drivers** call it:

| Driver | Where it runs | What it does with events |
|---|---|---|
| Perform | the tick hook, inside the audio routine (§12) | sends notes to each part's Output, ramps sound controls |
| Compose | a yielding task loop in UI context (§15) | writes clip notes and automation nodes into a private `Song*` |
| Harness | the `tests/` executable (§21) | records them for specs, goldens and rough renders |

Capture is the Compose driver replaying a steering log (§17).

The step function produces an **event stream**: absolute-pitch notes with tick offsets, lengths and velocities, sound-control targets, and Arc section changes. Every driver gets the same stream for the same input (§6.3). Swing, Lean and Ornaments are all plain events in that stream (§8).

### 2.3 The output seam

The Generator writes each part to an **output**. In v1 an output is an internal Synth (`SoundInstrument`) or Kit. MIDI/CV out must be addable later as another output kind without touching roles or the core.

### 2.4 Code and data layout

- Generator core and genre tables: a new directory under `src/deluge/`, chosen by the first slice. The POC used a `generator.h` seam, which is a fine starting point.
- Perform, the Generative menu, the Compose screen and the Recipe browsers: new UI classes under `src/deluge/gui/`.
- Harness: a new target in the `tests/` CMake tree, with specs as CppSpec `*_spec.cpp` files in `tests/spec`.
- Generative pack: `pack/` at the repo root, laid out exactly as on the card (§18).

Sources: [Simulation approach][t8], [Genre model][t11], [Track roles][t12], [Generator scheduling][t19], [Testing and CI][t41], [Build order][t44].

## 3. The Recipe

### 3.1 Contents

A **Recipe** holds no notes. It is:

- a **genre**, by stable name (§4.4);
- the **musical facts** (§3.2);
- the four **macros** (§3.3);
- the **sound picks**: one card path per synth part, and one sample path per lane of the Drums and FX parts (§10).

The **seed** and each part's **take** sit beside the Recipe, not in it (§6). Generative mode works on one **Current Recipe** with its seed and takes, kept in RAM until power-off (§14.2).

### 3.2 Musical facts

| Fact | Range |
|---|---|
| Tempo | any tempo the Deluge supports. A genre's range is where rolls come from, not a clamp |
| Key root | C..B |
| Scale | one of the 12 seven-note preset scales: Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Melodic Minor, Harmonic Minor, Hungarian Minor, Marva, Arabian |
| Swing | song swing amount −49..49, plus swing interval |

For each fact, the genre gives either a fixed default or a range or weighted list to roll from. Facts are rolled once, when a Recipe is created from a genre, from fresh entropy (not from the seed), and stored as concrete values. Rolling again is always an explicit action: picking a genre, Regenerate, or a genre change.

### 3.3 Macros

There are four macros: **Energy**, **Density**, **Complexity** and **Motion**. Each is 0–50 on the OLED and stored at full resolution. They are global, with no per-part overrides.

- A macro value is the **centre** the Arc moves around, not a level. The Arc offsets effective Energy and effective Density only (§9.4). It never moves Complexity or Motion.
- A macro moves its features the same way in every genre and part. A genre only weights how much each feature moves, through a per-genre macro weight table. Weights run ×0..×2 and can never flip a sign.
- Macros always start from the genre's defaults. They are never rolled. A genre change keeps them.

What each macro moves in v1:

| Macro | Moves |
|---|---|
| Energy | intensity: note velocity (§8.4), and the macro layer of the sound controls: brighter, shorter, more driven and drier as it rises (§11.3) |
| Density | how many notes play: onsets thinned or filled by step importance (§8.1), and the part and lane **Density gates** (§5.6) |
| Complexity | how intricate each beat is: syncopation, aimed at a band rather than a maximum; how often Ornaments happen (§8.2); interplay rule strength (§5.5); harmonic colour through in-scale extensions such as 7ths, 9ths, sus and added notes (§7.4); and when the Build roll starts (§8.3). It never moves sound |
| Motion | how much and how fast the music changes: mutation probability (Kick and backbeat never move); the Motif re-roll rate; drift depth and speed (§11.5); how often fills and answers happen; and how eager the Arc is to move (§9.3) |

Energy no longer decides which notes play. That is Density's job. No macro moves Lean, Velocity spread or a part's Degree set.

### 3.4 Steering grain

In Perform every Recipe value can be steered live. Each kind of value takes effect at its own grain:

- Macros: note-level effects from the next undecided step (§12.2), pattern-level effects from the next bar, harmonic effects from the next chord.
- Tempo and swing **glide** to the new value over the Perform glide setting (§13.3). The OLED shows the target at once.
- Key and scale are queued to the next phrase line (§9.7). A genre change, Regenerate or Recipe LOAD plays a transition (§9.6).

Compose v1 renders one Recipe that is fixed up front (§14.4).

Sources: [Recipe parameters][t10], [Density as a fourth macro][t24], [Automation][t14], [Generator randomness and seeding][t17], [Mapping steps onto 5- and 6-note scales][t29], [Chord graph across a genre's scales][t31], [Sub-step expression][t20].

## 4. Genres

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
| 1 | Identity | stable name, OLED name, genre version | §4.4 |
| 2 | Musical facts | default or roll per fact | §3.2 |
| 3 | Macros | default values, macro weight table | §3.3 |
| 4 | Meter and phrase | phrase length in bars, 8 by default. Always 4/4 on a 16th grid | §4.3 |
| 5 | Part roster | 4–6 parts with role, flavour, gates, Register, Degree set, Lean, Velocity spread, ornament settings | §5 |
| 6 | Rhythm | pattern pool per part, step importance, ghost masks, fill kinds, interplay rules, Replace pairs, part order | §5, §8 |
| 7 | Harmony | Chord graph (home pull, weighted moves, cadence period), harmonic rhythm, chord kinds and colour | §7.4 |
| 8 | Default palette | sound sources per part and lane | §10 |
| 9 | Section rules | Arc offsets, lengths, Break keeps, Build rules, Gap, Intro and Outro curves | §9 |
| 10 | Sound-control data | macro weights, gesture switches and depths per part, drift weights | §11 |

### 4.3 Meter

Every v1 genre is 4/4 on a 16th-note grid. Phrase length is a genre value (Psytrance uses 16 bars). Triplet and shuffle feel come from swing and Lean (§8.5).

### 4.4 Names and versions

- A genre has a stable lowercase name (such as `techno`), used in Recipe files. Names are never reused.
- A genre has a **version number** that goes up whenever a retune changes its event stream. Recipe files store it. Old genre tables are not kept (§16.2). Golden files change in the same commit (§21.3).

### 4.5 v1 genres

The briefs below are what implementation fills tables against. Tables are hand-written, with PERF-1's genre tables as the main reference, but every value is our own, re-derived and tuned by ear. Copying PERF-1's tables row for row counts as porting (ADR 0001).

- **Techno.** 125–135 BPM, Minor or Phrygian, straight. Four-on-the-floor kick, offbeat hats and a mutating acid line. Sparse harmony: a drone or one chord, slow harmonic rhythm. Long builds and a kick drop at phrase ends. No claps on 2 and 4: that's House. Groove 4–8 phrases (*tuning*).
- **House.** 120–126 BPM, moderate 16th swing, Minor, Dorian or Major. Claps on 2 and 4, an offbeat bass that answers the kick, chord stabs with 7th and 9th colour, a Chord graph that matters, and 2–4 bar harmonic rhythm. Hats lean late on off-beats (for example on 0, off +3).
- **Dub** (roots dub, not dub techno). 70–80 BPM, Minor, light swing. One-drop drums, an offbeat skank, a heavy bass that carries the tune, and delay and reverb throws and drop-outs as the main motion. It has a **Siren** FX lane (§10.5). Its step tables give the beat-3 backbeat to only one of Rim and Snare, and keep the Crash off strong hat steps. A lazy snare leans late (for example +2 everywhere).
- **Psytrance.** 138–146 BPM, Minor, Phrygian or Harmonic Minor, straight. Kick plus a rolling 16th bass that skips the kick step (Bass Avoid Kick at 100%, sensitivity 0). A pedal-tone root with almost no chord changes, filter-swept leads and arps, 16-bar phrases, and long builds into hard drops. Groove 3–6 phrases (*tuning*).
- **Ambient.** 60–90 BPM, Major, Lydian or Dorian. No drums, or soft sparse percussion behind a high Density gate (about 35). Pads and drones with 8+ bar harmonic rhythm. The Arc swells and recedes and never drops: its section rules make the Build a slow swell with no Gap, no roll and no Throw, and its Drop lift is near 0 with a long decay (§9.5). It has no Impact lane. Groove 6–12 phrases (*tuning*).

Sources: [Genre model][t11], [Recipe parameters][t10], [Perform interaction model][t15], [Porting policy][t23], [Recipe files][t40], [Generative pack contents][t42], [POC gaps][t52], [Sub-step expression][t20], POC findings on the [map][poc].

## 5. Parts, roles, flavours and lanes

### 5.1 Parts and roles

- A genre's roster lists 4–6 **parts**. The roster belongs to the genre, and the Recipe holds no part list. The hard limit is 6 parts, which the fixed state block and the six MUTE pads both assume.
- Each part has exactly one **role** from a fixed Generator-wide set: **Drums, Bass, Chords, Pad, Lead, FX**. The role decides how notes are made, which interplay rules can target the part, how the Arc treats it and which palette section it draws from.
- Roles may repeat, such as an acid Lead and an arp Lead. A genre has at most one Drums part.
- In Compose, the player deletes unwanted parts afterwards in the normal views.

### 5.2 Flavours

A **flavour** is the note-making method within a role. The genre picks one per part. New flavours are shared mechanisms, never per genre.

| Flavour | Roles | How it makes notes |
|---|---|---|
| Loop | Bass, Lead | a stored N-step line. Each step holds a scale offset from the chord, an octave, a gate, an accent and a slide. It mutates under Motion and plays answers. It covers the acid line, the House offbeat bass, the rolling psy bass and the Dub bass, which differ only in data |
| Arp | Lead | walks the current chord's notes on a rhythm |
| Motif | Lead | a ring of slots storing chord notes by their position in the chord, not as pitches, so remembered phrases follow chord changes. Covers plucks and the Dub melodica |
| Stab | Chords | voiced chords on a rhythm pattern. House stabs and the Dub skank differ only in pattern |
| Sustain | Pad | held voicings that change with the chord. A **pedal** flag makes it a drone: root and fifth only, ignoring chord changes |

Drums and FX have no flavour.

- **Loop mutation.** At the end of a loop pass, steps re-roll with a probability that rises with Motion. PERF-1 uses (Mutate/100)², a fine starting curve (*tuning*). The Kick and the backbeat never mutate.
- **Answers** are read-time transforms of the same loop, played every Nth bar or loop, or in the bar before a drum change. PERF-1's set is octave, fifth, diatonic third, mirror, reverse, rotate, sparse and accents-only. Which a genre uses, and how often, is data. Motion raises the rate.
- **Motif** slots replay with a probability (PERF-1's "deja vu"). Motion sets the re-roll rate.

### 5.3 Outputs

- Each part maps to exactly one Output for its lifetime. No part spans two Outputs, and no two parts share one.
- Drums map to one **Kit** with one row per lane. FX map to a Kit of their own. Bass, Chords, Pad and Lead map to one Synth each.
- Every Kit clip carries one NoteRow per lane, in the fixed lane order of §5.4, even when the lane is silent.

### 5.4 Lanes

**Lanes** are fixed kinds inside a Drums or FX part. The Generator addresses a lane by kind, never by row order.

| Part | Lanes, in fixed order |
|---|---|
| Drums | Kick, Snare, Clap, Rim, Closed hat, Open hat, Ride, Crash, Tom, Perc 1, Perc 2 |
| FX | Riser, Fall, Impact, Siren |

- A genre picks a subset. FX lanes are played only by section rules (§9.5), never by patterns.
- Lane kinds drive protected mutation (Kick and backbeat never move), interplay rules and sample lookup (§10.3).
- Drums and FX lanes are unpitched in v1. Key changes never retune them.

### 5.5 Interplay rules

Interplay rules (genre slot 6) shape a part's rhythm against another part:

| Rule | Meaning |
|---|---|
| Avoid X | thin my onsets where X has one |
| Lock X | put my onsets on X's onsets. With the `off` option, on X's offbeats |
| Answer X | play mainly in X's rests, at half-bar grain |

- Each rule has a **strength**, 0–100% at the genre's default Complexity, and a signed **Complexity sensitivity**, so Complexity can tighten or loosen it. Examples: psy Bass Avoid Kick at 100% with sensitivity 0; House Lead Answer Bass at 60% with positive sensitivity; Techno hats Lock(off) Kick at 90% with negative sensitivity.
- A rule reads the target's **planned** pattern, not what sounds. A muted or Break-silenced kick doesn't change the bass line.
- **Order.** Harmony is planned before any part. Parts are then generated in role order, **Drums → Bass → Chords → Pad → Lead → FX**, unless the genre overrides it (for example Bass after Chords when the bass locks to the stabs). A rule may only target a part earlier in the effective order. A harness spec rejects any genre that breaks this.

### 5.6 Gates

- Every part and every lane has an optional **Density gate**: a threshold, default 0. It plays only while effective Density (Recipe centre plus Arc offset) is at or above the threshold. Examples: Ambient drums at about 35, a shaker Perc lane at 30 (*tuning*).
- Gates are separate from step-importance thinning inside a lane (§8.1).
- A part sounds only when the Arc, its gate and the player all allow it (§13.4).

### 5.7 Replace pairs

- Lanes of one part play independently, apart from **Replace pairs**: one-way rules "A replaces B". When both land on the same step, B's hit is dropped before any voice starts.
- Replace pairs are applied in the Generator, so every driver agrees.
- Genre data holds the list. The Generator-wide default is **Open hat replaces Closed hat**, and every v1 genre uses the default.
- The firmware's one CHOKE group per Kit still cuts the hat tails (§10.6). It can't express a same-step rule, which is why Replace pairs exist.

### 5.8 Voice modes

Role defaults, which flavour and genre may override, applied when the sound loads (§10.6):

| Role | Voice mode |
|---|---|
| Bass | LEGATO |
| Lead | LEGATO with Loop. POLY with Arp or Motif |
| Chords, Pad | POLY, at most 4 notes |
| Drums, FX | per lane. Open hat and Closed hat use CHOKE |

Sources: [Track roles][t12], [Kit lane mapping][t25], [Generative pack contents][t42], [POC gaps][t52], [Density as a fourth macro][t24], [Generator scheduling][t19], [PERF-1 internals][t5], [Perform control layout][t38].

## 6. Randomness and seeding

### 6.1 Own randomness

The core uses only its own integer hash. It never calls the firmware's `getRandom255`, `getNoise` or `CONG`, and never reseeds `jcong`. That generator is one global LCG (`util/waves.h`) seeded from a timer, and probability, the arp and about a dozen DSP paths all draw from it, so its sequence depends on what is sounding.

### 6.2 Keyed draws

- Every draw is a pure hash of `(seed, part, take, purpose, position)`. Draws don't affect each other. A macro change moves the threshold over the *same* underlying choices, so the piece stays recognisably itself. Muted parts and skipped work cost nothing.
- Things that walk, such as drift and the chord walk, keep small state, but each step's draw is keyed.
- The hash is a cheap 32-bit integer mix, chosen at implementation.
- **Position means Arc counters**: the Section index (such as the 3rd Groove), the phrase within the Section, the bar in the phrase and the step. Each purpose keys on the counters it chooses, so repetition is designed in. A Loop keyed on the phrase repeats bars within a phrase and mutates between phrases. Forcing a Section changes *when* things happen, not *what* they are.

### 6.3 The contract

The same Recipe, seed and takes, with the same steering at the same positions, give an identical **event stream**: notes, velocities, sound-control values and Arc sections. This holds in the firmware and the harness, whatever the CPU load, voice culling, muting or wall-clock time. Audio is not promised bit-identical, because firmware DSP randomness and culling are outside the Generator.

### 6.4 What the seed covers

- The seed covers only the music the Generator makes: patterns, notes, chords, Arc section lengths, fills, Ornaments, Velocity spread and drift.
- Musical facts and sound picks are rolled from fresh entropy and stored as concrete values. A Recipe and seed reproduce exactly only on a card with the same files and on the same genre version.
- **No firmware chance.** Perform and Compose write notes at probability 100 with no iterance and no fill flags, and use no arp random modes and no firmware randomizer.

### 6.5 Seed source and display

- A fresh 32-bit seed is drawn from a hardware timer on Recipe creation, Regenerate and any genre change.
- The player sees it read-only as a **seed code** of 7 base-32 characters (for example `1A2B3C4`), used in names and on the SEED + TAKES title. The alphabet is chosen at implementation, from characters `QwertyUI` accepts (A–Z, 0–9).
- The player can draw a new seed (NEW SEED) but can't type one in v1.

### 6.6 Takes

- Each part has a **take**, starting at 0, that is part of every key for that part's notes. Re-rolling a part's pattern gives it the next take: new patterns for that part only, with the rest of the piece unchanged.
- Earlier takes replay exactly, so stepping back is free. A new take or a step back starts at the next phrase line.
- A take covers the part's notes and Ornaments, not its drift. Re-rolling the pattern and re-rolling the sound are separate actions.
- v1 has 16 takes per part, 0–15, shown as 1–16 (§23.5).
- A new seed, Regenerate and a genre change reset every take to 0.

### 6.7 Freeze

- Freezing a part holds the counters its pattern keys on, so it replays its current phrase's draws.
- Macros and section rules still act on top: a frozen hat line still thins with Density and still drops out in a Break.
- Drift keeps wandering, because freeze holds notes, not sound.
- Unfreeze jumps to where the counters would be now, not back to where it froze, which keeps the timeline reproducible.

### 6.8 Perform and Compose agree

A Compose of a Recipe, seed and takes plays the same notes at the same Arc positions as an untouched Perform of it from Start (with the Intro). Compose may differ only where its song shape forces it: length and Outro placement. A harness spec checks this (§21.2).

Sources: [Generator randomness and seeding][t17], [Compose output shape][t16], [Sub-step expression][t20], [POC gaps][t52], [Perform control layout][t38], [Compose screen and Generative menu prototype][t50].

## 7. Pitch

### 7.1 Steps inside, absolute notes out

- The Generator works in **scale steps** (step plus octave) against its own copy of the Recipe's key and scale. It resolves each note to an absolute MIDI note when it emits it. The event stream carries absolute notes.
- A key or scale change reaches every part on its next note, with nothing rewritten.
- The Generator never calls `Song::setScale` or `Song::setRootNote`. Both remap or re-infer from clip notes.

### 7.2 Scales

- Steps always sit on a 7-degree grid, which is every v1 scale unchanged (§3.2). No mapping onto smaller scales is needed.
- **In-scale only.** Every pitched note is in the Recipe's scale. A genre that wants a raised leading tone on V rolls Harmonic Minor. Chromatic colour is a later effort.

### 7.3 Register

- Each role has a **Register**: an anchor note plus a span (for example Bass anchor 36, span 0..+14). A genre may override both per part.
- A part's tonic is the instance of the key root's pitch class nearest its anchor, so a key change moves any part by at most 6 semitones.
- Notes outside the span, from inversions, interplay transforms or Degree-set snapping, fold back by octaves.
- Registers are fixed in v1. Energy doesn't move them (§23.4).

### 7.4 Chords

- Each genre has one degree-based **Chord graph**: home pull, up to about 8 weighted favourite moves, whether A-B-A is allowed, and a cadence period in chords. The Generator walks it, one keyed draw per chord, to pick each chord. The same graph is used unchanged in every scale.
- **Harmonic rhythm** is a genre value in bars per chord. Chord changes always land on a bar line.
- **Strict guard.** A degree whose triad in the current scale isn't plain major or minor (diminished, augmented, major ♭5 or non-tertian) has its weight multiplied by 0.15 (150 per-mille). A cadence target that is guarded in the current scale swaps to V, else IV.
- **Colour moves.** A Generator-wide table, one row per scale, adds weight to the moves that give the scale its character. It is added to whichever graph is playing. Starting rows (*tuning*):

  | Scale | Colour moves |
  |---|---|
  | Dorian | i–IV |
  | Phrygian | i–♭II |
  | Lydian | I–II |
  | Mixolydian | I–♭VII |
  | Harmonic Minor | V–i, iv–V |
  | others | none |

- The guard factor, the guard rule (strict or tritone-fifth only) and each colour row's weights are **data with Generator-wide defaults**, not constants in the walk, so a later genre field, Recipe value or player setting can override them.
- All 12 scales work with no per-genre fallback. Known costs, accepted: Locrian's home chord is diminished and Arabian's is major ♭5, and the guard can't move home, so their phrases resolve onto an unstable chord.
- Walk details from the prototype that sounded right, as starting values (*tuning*): every phrase starts on home, the cadence target gets +20, home is excluded on a phrase's last chord, and the walk can't repeat the current chord.
- **Harmonic colour** comes from in-scale extensions (7ths, 9ths, sus, added notes), more of them as Complexity rises. Chords and Pad play at most 4 notes. Voicing and voice leading are flavour implementation.
- At a key or scale change the walk keeps its place (steps are relative) and its cadences pull it home as usual. It doesn't reset to the tonic.

### 7.5 Degree sets

- A Bass or Lead part (flavours Loop, Arp and Motif) may name a **Degree set**. The default is **Full**, all 7 degrees. v1 also has **Pentatonic**. Chords and Pad always use the whole scale.
- The name is resolved per scale through a hand-written Generator-wide table (*tuning*):

  | Scale | Pentatonic degrees | Semitones in the set |
  |---|---|---|
  | Major, Lydian, Mixolydian | 1 2 3 5 6 | 0 |
  | Minor, Dorian, Phrygian | 1 3 4 5 7 | 0 |
  | Melodic Minor | 1 3 4 5 6 | 0 |
  | Locrian | 1 3 5 6 7 | 0 |
  | Arabian | 1 2 3 5 6 | 0 |
  | Harmonic Minor | 1 3 4 5 7 | 1 (keeps the leading tone) |
  | Marva | 1 3 4 5 6 | 1 |
  | Hungarian Minor | 1 3 4 5 7 | 2 (no better set exists) |

- The set is fixed to the key's home, so a part stays in its box while chords move. The current chord's root is always allowed.
- **Snap.** A step outside the set moves to the nearest allowed degree, and a tie goes down. Order of resolution: flavour step → Answer and interplay transforms → snap → Register fold → absolute note. An Ornament repeats the snapped pitch. Two neighbouring steps may snap onto one note, and a slide onto the same note just ties.
- Nothing moves a Degree set in v1. It is a part field plus the table, so a later rule can be added without reshaping anything.

### 7.6 Key changes

At a resolved key or scale change, every held pitched note gets a note-off and the new notes start fresh. Tails ring out. When it happens is §9.7.

Sources: [Pitch model][t21], [Mapping steps onto 5- and 6-note scales][t29], [Chord graph across a genre's scales][t31], [Parts that play some scale degrees][t32], [PERF-1 internals][t5].

## 8. Rhythm and expression

### 8.1 Onsets and Density

- Each part's pattern material carries a **step importance** per step. Effective Density sets a threshold, with seeded jitter around it, and a step plays only if its importance clears the threshold. Patterns are never edited, only thinned or filled.
- At Density 0, only the most important hits play (Techno: the kick alone). The threshold curve and the jitter are *tuning*. The POC's Techno curve let nearly every step through by Density 40, which was too fast.
- Syncopation under Complexity picks pattern material and ghost masks aimed at a genre-set band, not a maximum.

### 8.2 Ornaments

All expression is plain note events (§2.2). A part's flavour decides which steps play, at what pitch, velocity and length. One **expression stage**, shared by every role, then adds Ornaments and applies Lean to the notes that survived Density, using per-part and per-lane genre settings.

- **Ratchet.** One note becomes 2, 3 or 4 evenly spaced hits inside its 16th, every 12, 8 or 6 ticks. Each hit has the same pitch and lasts the spacing, or the original note if that's shorter. The first hit keeps the note's velocity, and each later hit is slightly softer. The genre lists which counts each lane or part may use. Pitched parts may ratchet if the genre allows it. A ratchet never spans more than one step.
- **Flam.** A soft grace hit at the note's Lean position, then the main hit 1–3 ticks later.
- **Rate.** Effective Complexity scales how likely Ornaments are, between per-lane floors and ceilings the genre sets. Density doesn't thin Ornament hits, since an Ornament decorates a note that already survived. Energy affects only their velocities.
- Ornament draws are keyed like every other draw, so takes re-roll them and freeze holds them.

### 8.3 The Build roll

The Build snare roll is a section rule that drives ratchets, not an Ornament of its own. Across a Build, a switched-on lane's ratchet count is forced upwards (plain hits, then 2, then 4), with velocity climbing towards the Gap, however low Complexity is. The roll rule decides which steps it plays on (every beat, then every 8th, then every 16th), not Density. Complexity only sets how early in the Build the roll starts. The roll lane and rate are section rules (§9.5).

### 8.4 Velocity

- Flavours set a note's base velocity (accents included). Energy's velocity layer applies on top, scaled by the genre's weight.
- **Velocity spread.** Each part and lane has a spread amount in genre data, default 0. It is a seeded nudge from the Generator's hash, applied after Energy's layer and before clamping. Ornament hits take their main note's spread.
- **Spread mode**, per part and lane, default **Repeat**. Repeat keys the draw on the step's place in the part's pattern, so the nudge repeats with the pattern. Fresh keys it on the absolute Arc step, so every pass differs. There's no player control in v1.
- Accents are mostly volume accents. Every factory synth preset routes velocity to volume, and only three Techno presets (000, 005, 015) also open the filter on velocity. Brightness belongs to the sound controls.

### 8.5 Lean

- A part or lane has two **Lean** offsets in genre data: one for on-beat 16ths and one for off-beat 16ths. Per-part swing is off-beat Lean, added on top of song swing. With song swing at 0, a genre's leaning hats still lean.
- Lean is signed, **−12 to +12 ticks**, half a 16th either side of the grid. The Generator decides each step 12 ticks ahead so pushed hits are possible (§12.2, ADR 0003).
- **First-step clamp.** Negative Lean clamps to 0 on the first step of every Section, in every driver. Compose clips stay self-contained, and downbeats such as the Drop land together.
- **How offsets add up.** Lean moves the whole note, Ornaments included. An event's total offset from its grid line is clamped to −12..+12. A flam's main hit clamps to +12. A ratchet's hits keep their spacing from the shifted start, and the spacing shrinks so the last hit still falls inside the step.
- No macro moves Lean, and the player can't set it. There's no humanize jitter: one tick is about 5 ms at 120 BPM, too coarse for jitter.

### 8.6 Slide

- Slide is a flavour decision, written as note overlap. A Loop part's sound is set to LEGATO with portamento on at load (§10.6).
- In LEGATO, a note that overlaps the previous one changes pitch on the same voice and glides when portamento is on (`sound.cpp`, `voice.cpp`). A note after a gap starts fresh with no glide. That is the 303 slide, and it needs no firmware change.
- No pitch bends or trills in v1.

### 8.7 Fill

- Fill is momentary, like the Deluge FILL button. While it is held the Generator plays the genre's fill kinds (genre slot 6) in place of the normal patterns.
- Motion also sets how often the Generator plays fills of its own, normally in the last bar of a phrase.
- Fill state is read from `currentSong->isFillModeActive()`, so the MIDI-learnable FILL command works too.

Sources: [Sub-step expression][t20], [Pushed Lean][t30], [Density as a fourth macro][t24], [POC gaps][t52], [Sound generation][t13], [Perform interaction model][t15], [Prior art in generative music engines][t6], [Perform control layout][t38], POC findings on the [map][poc].

## 9. The Arc

### 9.1 Sections

The **Arc** is the Generator's movement through **Sections** around the Recipe's macro centre. There are five kinds: **Intro, Groove, Break, Build, Outro**. There are no others in v1, and genres differ through section rules, not new kinds.

- Sections are whole phrases long, using the genre's phrase length. A Build may be half a phrase.
- **Groove** is home, where the macros sit at the Recipe centre.
- A **Gap** isn't a Section. It is an optional Build ending (none, 1 beat or 1 bar, set by the genre) where the mix cuts out.
- A **Drop** isn't a Section. It is the event where a Build lands back in Groove: everything plays, and effective Energy and Density lift, then decay back to the centre over a phrase.

### 9.2 Automatic cycle

- The Arc runs on its own: **Groove → Break → Build → Drop → Groove**. A genre may also allow Groove → Build, weighted.
- A Start plays a one-phrase Intro first (§13.2). End plays a one-phrase Outro (§13.2).

### 9.3 Section lengths

- **Groove length** is drawn (seeded) from a per-genre phrase range given at Motion 25. Motion 50 halves it, Motion 0 means the Arc never moves on its own, with a linear scale between.
- **Break length** is drawn from a per-genre range. Motion doesn't scale it.
- **Build length** is a genre value in phrases or half a phrase.
- Example Groove ranges (*tuning*): Techno 4–8, Psytrance 3–6, Ambient 6–12 phrases.

### 9.4 What the Arc moves

The Arc moves **only effective Energy and effective Density**, as offsets from the Recipe centre per Section kind. Each kind has its own Energy offset and its own Density offset. A genre that doesn't set the Density offset uses its Energy offset.

| Kind | Offset shape |
|---|---|
| Intro | rising curve from low |
| Groove | 0 |
| Break | a dip |
| Build | a ramp from the Break level to above the centre |
| Drop (event) | a lift that decays back to 0 over a phrase |
| Outro | falling curve |

So a genre can have a Break that thins but stays loud, or a Build that fills in before it brightens. In the Intro, parts enter through their Density gates as effective Density rises, and in the Outro they leave as it falls.

### 9.5 Section rules

A genre's section rules (slot 9) hold everything else:

- Energy and Density offsets per kind, and the Intro and Outro curves.
- **Break keeps**: which parts and lanes still play in a Break.
- **Build**: length, roll lane and rate (16ths up to 32nds or triplets), whether the kick comes back, and Gap length.
- Groove and Break length ranges, and the Groove → Build weight.
- **FX lanes.** Riser plays across a Build. A sample Riser starts early enough to end exactly on the Drop, timed by the file end (pack risers are trimmed to land on it). A synth Riser holds one note over the Build and sweeps through its own row's sound controls (§11.4). Fall and Impact are one-shots that section rules place at a moment, such as the Drop, so they need no special timing. **Siren** is one held note starting on a Break's first step or on a Drop, lasting a genre-set number of bars, with its pitch sweep from the preset's own LFO. Density gates FX lanes like any lane, and Complexity adds nothing to them.
- **Ambient** uses these rules to swell and recede without a heard drop: a slow, long Build with no Gap, no roll and no Throw, and a Drop lift near 0 with a long decay. No Impact lane.

### 9.6 Force and hold

- **Force** a Section: it starts at the next phrase line. A forced Build starts at whichever boundary lets it end on a phrase line, so the Drop lands on one. Pressing the force again cancels it while queued.
- **Hold** keeps the current Section until released.
- Forcing works during a hold: the forced Section plays, then the hold applies wherever the Arc lands.

### 9.7 Transitions

**Genre change, Regenerate and Recipe LOAD** are all played the same way:

1. The incoming roster is preloaded into fresh Outputs in task context while the old genre keeps playing (§10.7).
2. Once loading finishes, the outgoing genre plays a Build, placed like a forced Build so that it ends on a phrase line. A slow load delays it by a phrase.
3. The new roster lands on a **Drop**. The tempo and swing glide is stretched over the Build, so the new tempo arrives on the Drop. The new key and scale take effect on the Drop.
4. Outgoing parts get **Throw** on their last hit, then release. Their Outputs are deleted once their tails finish.

Further rules:

- The player picks **BUILD INTO A DROP** or **HARD CUT AT PHRASE** in the confirm. A hard cut plays no Build and swaps at the next phrase line.
- A queued change can be cancelled until its Build starts. After that, cancelling is refused.
- If the Arc is held, the change still plays its Build, and the hold resumes in the new genre's Groove.
- **Regenerate** is a genre change to the same genre: rolled facts, every sound pick and the seed are rolled again, every take resets to 0, and the macros are kept. It never picks a sound a part or lane already has, as long as its list has another entry.
- A **genre change** resets the facts from the new genre (default or roll), rolls the sound picks for the new roster, draws a new seed and resets every take. It keeps the macros.
- A **Recipe LOAD** mid-set uses the file's seed, takes, facts and macros instead of rolling. The file therefore starts at a Drop, not an Intro (§16.5).
- The incoming roster always gets **fresh Outputs with unique names**, even when a pick matches a playing one, because two Outputs with the same preset name and folder break clip matching when the song is saved and loaded again. Two rosters fit in memory (§20).

**Key or scale change on its own** takes effect at the next phrase line, with no Build or Drop. If one is pending when an Arc Drop lands, it takes effect on the Drop.

Sources: [Perform interaction model][t15], [Density as a fourth macro][t24], [Automation][t14], [Sound generation][t13], [Perform control layout][t38], [Recipe files][t40], [Generative pack contents][t42], [Building Generator Kits in code][t28], [Measure memory and load times on a Deluge][t37], [Genre model][t11].

## 10. Sounds

### 10.1 Palettes and sound picks

- Synth sounds are card presets, chosen through a **palette**. There are no built-in sound templates and no preset mutation. The firmware bundles no sounds.
- Each genre has a built-in **default palette** (slot 8), constant data in the core, so the harness reads it with no file parsing. Entries are keyed by part, with a role-level section as the default for parts that aren't listed. Each entry is a weighted list of preset paths, which can point anywhere on the card.
- A card file `GENERATIVE/PALETTES/<GENRE NAME>.XML` is an optional **override**. Each entry it lists (a part, a role default or a lane) replaces the built-in entry, and everything else falls back. v1 ships no override files, but the firmware reads one if a player writes it.
- A palette changes only what the music sounds like, never how it plays.
- A **sound pick** is a concrete path rolled (from fresh entropy, by weight) when a Recipe is created and stored in the Recipe. It is kept until the player re-rolls it or changes it. The player can point any part at any card preset, and any single lane at any sample file.
- **Fallback chain.** If a pick doesn't load, try the next palette entry. If nothing loads, use the Deluge default synth patch and show one OLED warning. Generative mode still starts.
- **Missing path on Recipe load:** re-roll from this card's palette and warn (§16.6).
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
- `ModControllableAudio` and `ArpeggiatorSettings` constructors read their default sync level from `preLoadedSong` or `currentSong`. Set it explicitly when building a private song.

### 10.5 FX lanes

- Riser, Fall, Impact and Siren look for samples in `SAMPLES/FX/<lane>` first. If none, they load a Generative pack synth preset into the kit row through `StorageManager::loadSynthToDrum`, which takes a `Song*` and is safe on a private song. Its only caller does the follow-up on `currentSong`, so the Generator needs its own follow-up on `song`.
- Riser lanes list only pack risers. Fall and Impact use factory files plus pack fallbacks. Siren is always a pack synth preset.
- FX lanes always use their own row controls, so a synth Riser sweeps its own row (§11.4).

### 10.6 Load-time settings

When the Generator loads a part's sound, it does these once, before playback. None is a sound control, and nothing changes them live.

- Sets the voice mode (`Sound::polyphonic`) to the part's mode (§5.8).
- **Portamento.** Parts that slide (Loop) need portamento: if the preset's is off, set a role default; if it's on, keep it. Every other part gets portamento 0. Portamento is `UNPATCHED_PORTAMENTO`, an `AutoParam`, so Compose writes it as a normal clip value.
- Switches the clip's arp off. Old presets store `<arpeggiator><mode>`, which the clip copies on load (`instrument_clip.cpp`). This makes the 15 factory presets with a saved arp usable, 165 Acid Arp included.
- Sets `Sound::voicePriority` by role: High for Drums and Bass, Medium for Lead and Chords, Low for Pad and FX. CPU-load culling then thins the pad before the kick or bass drops a note.
- Sets CHOKE on the Open hat and Closed hat rows, and on no other row. A CHOKE drum cuts every other CHOKE drum in its Kit (`kit->choke()`), and there's only one group per Kit.

The Generator never changes oscillator type, synth mode, tuning or mod routing. All of the above are plain field writes and need no firmware change.

### 10.7 When sounds load

- Loading runs in task context, never in a tick. Card loads can't run inside the audio routine.
- Entering Perform or loading a Recipe loads every sound before playback.
- A genre change, Regenerate or Recipe LOAD preloads the incoming roster into fresh Outputs while the old one plays (§9.7).
- A sound re-roll in Perform preloads into a fresh Output, then swaps at the next phrase line.
- Only one card load runs at a time: the deserializer, the sample `FIL` and the alternate-folder state are global. Ticks run inside every yield, so tick code must never touch a half-built Kit or start a load.
- Factory FM presets load as they are. The firmware forces their filters off (files older than community 1.2.0), which is what their designers heard. FM Brightness is modulator level, so nothing is lost.

Sources: [Sound generation][t13], [Kit lane mapping][t25], [Generative pack contents][t42], [Building Generator Kits in code][t28], [Factory card content per role][t34], [POC gaps][t52], [Track roles][t12], [Firmware changes the design needs][t43].

## 11. Sound controls and automation

### 11.1 The six sound controls

The Generator changes sounds live only through six **sound controls**: **Brightness, Decay, Drive, Space, Wobble, Thin**. Each moves a list of concrete params within a range around its **home**, which is how the preset was saved, or wherever the player has since set it (§11.6).

- On Bass, Chords, Pad and Lead, each control exists once per part.
- On Drums and FX, each control exists per lane (row params) and once kit-wide (kit-level params).
- A target only takes part if the loaded preset already uses it, so on some presets a control does nothing.
- Each control has a default range relative to home, and a palette entry may override it for one preset.

### 11.2 Mapping per engine

Offsets are in menu units (0–50), relative to the stored value. Every range is a starting estimate (*tuning*).

| Control | Subtractive / wavetable | FM | Ringmod | DX7 osc | Kit row (sample) |
|---|---|---|---|---|---|
| Brightness | LPF ±8; wave index ±5 | modulator 1/2 volume ±6 (modulators in use only) | LPF ±8; pulse width ±6 | LPF −10..0 | LPF −15..0 (kit level −20..0) |
| Decay | env1 release ±6; env1 decay ±6 if sustain < 50; env2 decay ±6 | same | same | cut only | cut only (lower sustain and decay) |
| Drive | ladder `lpfMorph` 0..+15; fold 0..+10 | modulator and carrier feedback 0..+8; fold | fold; ladder morph | fold | bitcrush / SRR, genre opt-in |
| Space | reverb send 0..+10; delay feedback ±4 only if already > 0 | same | same | same | reverb send |
| Wobble | mod FX depth ±8 if type ≠ none; LFO rate ±4 if cabled; existing cable amounts ×0..×2 | same | same | same | kit-level mod FX |
| Thin | HPF 0..+20 | none on factory FM | HPF 0..+20 | HPF if on | HPF (kit level 0..+25) |

Engine notes:

- In FM, brightness is modulator level, and Drive is feedback.
- `lpfMorph` is ladder drive on 12/24/24-drive LPFs, but an LP→BP→HP morph on SVFs. Wavefold does nothing on squares.
- DX7 operator levels and envelopes are `DxPatch` bytes, not params, so they can't move. The factory card has no DX7 or wavetable presets.
- All kit rows on the factory card are one-shots with sustain 50 and the LPF open, so Brightness can only darken and Decay can only shorten. Whole-drum gestures use the kit's affect-entire filter, reverb and mod FX.
- Wobble scales existing mod cables (lfo1 → pitch is in 69 presets) but never adds cables and never switches a mod FX on. A mod FX type change allocates memory.
- Space moves delay feedback only where the preset already uses it, because raising it from 0 allocates a buffer. Thin rising from 0 switches on per-voice HPF processing, but only during Builds.
- Not params, so never moved and never automated: saturation, unison count and detune, filter and mod FX types, sample start, every DX7 operator setting.

### 11.3 Three layers

A control's offset from home is **macro layer + gesture layer + drift layer**, summed and clamped to the control's range.

**Macro layer.** One Generator-wide sign table. A genre weights each entry ×0..×2 and can never flip a sign. It reads **effective** Energy, so Section changes shade the sound with no extra rules.

| | Brightness | Decay | Drive | Space | Wobble | Thin |
|---|---|---|---|---|---|---|
| Energy | + | − | + | − (drier at peak) | 0 | 0 |
| Density | 0 | 0 | 0 | 0 | 0 | 0 |
| Complexity | 0 | 0 | 0 | 0 | 0 | 0 |
| Motion | no static offset. It sets drift depth and speed (§11.5) |||||

Thin is left free for Builds.

### 11.4 Gestures

A fixed Generator-wide set. A genre switches each gesture on per part (or aims it at one lane, such as Thin on the snare in a Build roll) and sets its depth ×0..×2.

| Gesture | When | What | Shape |
|---|---|---|---|
| Close | entering a Break; across an Outro | Brightness down | eases in over 1 bar |
| Wash | during a Break; across an Outro | Space up | eases in over 1 bar |
| Rise | across a Build; across an Intro | Thin and Brightness up. A synth Riser also gets Space up | ease-in over the whole Section, peaking on its last step |
| Throw | last hit before a Break or Gap; outgoing parts' last hit in a transition; a Siren's release | Space spike | decays over 1 beat |
| Reset | Drop downbeat; first Groove downbeat after an Intro | every gesture offset snaps to 0 | one step-exact jump |

- A Gap is silence, so it is made of notes, not automation.
- On Drums and FX, the macro layer and gestures act on the **kit-wide** controls, unless a gesture is aimed at a lane. FX lanes always use their own row controls.

### 11.5 Drift

- A seeded, smoothed random walk per part on **Brightness, Decay, Space and Wobble**. Drive and Thin never drift.
- Depth is Motion × genre weight, up to about half the control's range at Motion 50.
- The period is about one phrase at Motion 25, half that at 50, and close to frozen at 0.
- On Drums and FX, drift acts on **per-lane** controls (hat Decay wandering, for example).
- Drift is not covered by takes and keeps running under freeze (§6.6, §6.7).

### 11.6 Home and player edits (Perform)

- When the player moves a param that a control targets, the **home moves by the amount turned**: home = new value − current layer offset. The Generator keeps offsetting around the new home.
- This applies to gold-knob turns, the sound editor overlay, the CLIP page faders, MIDI-learned params and MIDI follow (§13.7). All of them arrive through `AutoParam::setCurrentValueInResponseToUserInput` (F3 in §19).
- Every home change is logged as steering (§17.2).
- The home rule is provisional. Check it in beta testing.
- The Generator's own writes are never recorded as clip automation, even with a clip armed (F9 guards RECORD).

### 11.7 Grain

- The Generator works out each control's target once per 16th step and ramps linearly between steps. Only snaps (Reset) jump.
- **Perform** ramps through a new `AutoParam` live ramp (F2): target plus length in ticks, no nodes. The firmware then moves the param once per render window through its existing `tickSamples` path. The Generator rewrites the target every step, because the ramp has no stop. Snaps use `setCurrentValueWithNoReversionOrRecording`. Perform's clips hold no automation, so nothing competes with the ramp. This avoids zipper on params the single `paramLPF` smoothing slot can't cover.
- **Compose** writes automation nodes (§15.6).

### 11.8 What the Generator never automates

- Level and pan. Volume automation would override the player's fader in every Compose clip. Swells use velocity and Brightness, so the mix stays the player's.
- Arp and randomizer params. The Generator writes its own notes.
- Tempo and swing. Those are Recipe steering (§3.4). In Capture, tempo changes become song tempo automation (§17.6).

Sources: [Automation][t14], [Sound controls per synth engine][t26], [Sound generation][t13], [Generator scheduling][t19], [External tempo and MIDI in Perform][t51], [Perform interaction model][t15], [Perform control layout][t38], [Generative pack contents][t42].

## 12. Generator runtime

### 12.1 The tick hook

- One hook in `Session::doTickForward`, after the per-Output arp loop (`playback/mode/session.cpp`, F1), calls the Perform driver. The driver calls `generator.tick(...)`, which returns the ticks until its next event and lowers `playbackHandler.swungTicksTilNextEvent`, as the arp does.
- The Generator sends notes with `sendNote` on Synths and `noteOnPreKitArp` on Kits, the same entry points clip playback uses from the tick. Notes never go into Perform's clips (ADR 0002).
- Perform must stay in session playback (`lastClipInstanceEnteredStartPos = -1`).
- The Generator works in **swung ticks**, so song swing applies for free. A 16th is 24 ticks at the default magnitude.
- The hook loops over running instances. v1 runs one.

### 12.2 When steps are decided

- The Generator decides each 16th step **12 ticks before its grid line**, in every genre (ADR 0003). Ticks between decisions only read the event buffer.
- The fixed event buffer holds the step being decided as well as the step playing.
- Continuous steering takes effect from the next undecided step: about 63 ms more latency at 120 BPM (625/BPM ms per tick) on top of the wait of up to one step.
- Bar- and phrase-quantised actions (mute, solo, force, take, key change) must arrive 12 ticks before the line. A later press lands on the following boundary.
- If step cost turns out too high on hardware, the fallback is to plan Arc and Section decisions at phrase lines in task context and keep only note decisions in the tick.

### 12.3 The driver's clock

- The driver keeps its own clock. On every hook call it adds the swung-tick delta, shifted by the current magnitude, with the remainder carried. It never divides (the POC's 64-bit divide per call must not return) and never works out time from an origin, so a magnitude change only alters the clock's rate.
- A delta that is negative or longer than a bar counts as a **jump**, handled like a mid-play SPP (§12.4).
- Tempo magnitude matching stays as the player set it, as in a stock song. It settles within the first beat after an external Start.
- `Song()` takes a user-set default magnitude, and an external clock can change it mid-play. Never assume magnitude 2.

### 12.4 External clock

- **Start:** step 0 lands at tick 0, the normal Perform start.
- **Continue and SPP:** the driver tracks the SPP offset itself, because the firmware's song counter doesn't follow SPP. It stays silent until the next master bar line and puts step 0 there.
- **Mid-play SPP or a jump:** note-offs at the jump, then resume at the next master bar line. The Arc carries on where it was and is never rewound by SPP.
- Tempo under an external clock is §13.7.

### 12.5 Mute gate and note-offs

- A muted part is computed every step exactly like an audible one. The gate sits at the output, where note-ons are dropped. Notes already sounding finish their length.
- Firmware clip mute and solo are never used in Perform. The firmware has no session-level Output mute, and an inactive clip stops param ramps.
- Direct `sendNote` calls aren't tracked by the firmware, so the Generator keeps a fixed table of sounding notes per part and schedules its own note-offs as events.
- On stop, Perform exit, a jump, or retiring an Output, it sends note-offs for everything in its table, then `allNotesOff` on each part's Output as a safety net. Tails ring out.

### 12.6 Voice starts

- The firmware starts at most 4 voices per render window (1 under heavy load). Extra starts aren't dropped: they wait for the next render, about 2.9 ms later. At 130 BPM a render is shorter than a tick, so the budget is per tick.
- Each step sends note-offs first, then note-ons in role priority: Drums (Kick, then Snare and Clap, then the rest), Bass, Lead, Chords, Pad, FX. Ornament hits start last, so under load a flam's grace hit is the one that waits.
- Compose has no voice cap and writes every hit. That stays within the contract, which covers the event stream, not audio.

### 12.7 Tempo and swing glide

- While a glide runs, the driver writes song tempo once per swung tick on a linear curve, asking for a tick every tick. `Song::setTempoFromNumSamples` is public, and stock already changes tempo inside the tick.
- Swing changes only on step boundaries.
- Tempo glide is off while following an external clock.

### 12.8 Perform's song at runtime

- Each part's Output has one empty, always-active, looping 1-bar session clip. An Output with no active clip renders nothing.
- A Kit clip carries one NoteRow per lane, in the fixed lane order, because `noteOnPreKitArp` and per-lane sound controls go through those rows.
- Perform writes the Recipe's key and scale into its own song's `key` at every resolved change, by plain assignment. Its clips are empty, so nothing is remapped. Firmware that reads the song key, such as the arpeggiator's chord mode, then agrees with what plays.

### 12.9 Instances and the transition

- During a genre change, Regenerate or Recipe LOAD, the incoming roster is held ready: loaded Outputs plus its Recipe, seed, takes and genre.
- It starts generating on the Drop downbeat, with walking state at its initial values as on Start. Its state swaps in atomically on that step boundary.
- Running two instances at once, for a crossfade, is out of scope, but the no-globals rule keeps it cheap.

### 12.10 Input during card loads

Audio and the tick hook keep running during card loads, and pad and button handlers are still called, with `sdRoutineLock` set.

- **Act at once:** actions that change only Recipe or steering state: macros, force, hold, Fill, mute and solo, part focus, Capture mark.
- **Wait:** actions that need the card or another build: genre change, Regenerate, Recipe LOAD or SAVE, a sound re-roll. They wait through the stock `REMIND_ME_OUTSIDE_CARD_ROUTINE` deferral.
- BACK still cancels a queued genre change. Gold knobs are deferred by the firmware to just after each card routine, so they steer with a few ms of extra lag.

Sources: [Generator scheduling][t19], [Pushed Lean][t30], [POC gaps][t52], [Hardware tests of the Perform POC][t45], [Pitch model][t21], [Sub-step expression][t20], [Firmware changes the design needs][t43], [Firmware architecture of v1.3.0-rc3][t2], [Runtime budget on the Deluge][t3].

## 13. Perform

### 13.1 Its own song

- Perform builds its own `Song` from the Current Recipe, with one Output per part. It never layers onto the player's song.
- Entry (§14.3) builds it privately while the current song keeps playing, swaps it in **stopped**, and opens on the SONG page, waiting for PLAY.
- Steering edits the Current Recipe itself, so the Recipe is always the current centre. Arc state, mutes, solos, holds, freezes and focus are **performance state**: never stored, and lost on stop or exit.
- A sound-control home is the param's value on the part's Output, so it lasts until that Output is replaced (genre change, Regenerate, LOAD, sound re-roll). Homes are never saved in a Recipe file.
- The normal song, clip and arranger views can't be reached from Perform.

### 13.2 Transport, Start and End

- PLAY is the only start and stop. There's no separate "Generator on".
- **Start.** PLAY from stop plays a one-phrase **Intro** (Rise across it), then Groove at the next phrase line (Reset on its downbeat). SHIFT+PLAY skips the Intro and starts in Groove. Every start is fresh: the Arc resets and a new piece begins.
- **Stop.** PLAY stops at once, the Deluge way: notes released, tails ring out.
- **End** queues a one-phrase **Outro** from the next phrase line (Close and Wash across it). Energy and Density fall and parts leave through their gates. The transport stops on the following phrase line. End is armed by holding its pad 0.6 s, and a tap cancels it while queued.
- X+PLAY (stock restart) is blocked.

### 13.3 Perform settings

- **Glide**: how long tempo and swing glides take. Instant, 1 beat, **1 bar** (default), 2 bars, 4 bars or 1 phrase. It is a Perform setting, not a Recipe value, kept until power-off.
- **Transition style** is chosen per change in the confirm (§9.7), not stored.

### 13.4 Live actions

| Action | Grain |
|---|---|
| Macros | next undecided step (§3.4) |
| Tempo, swing | glide from now |
| Key, scale | next phrase line, or the next Drop if one is pending |
| Genre, Regenerate, Recipe LOAD | transition (§9.7) |
| Force a Section, hold | next phrase line / now |
| End | next phrase line |
| Part mute and solo | next bar. SHIFT = now |
| Sound re-roll; back to previous sound | preload, then next phrase line |
| Pattern re-roll (new take); step back or forward through takes | next phrase line |
| Freeze / unfreeze | next undecided step |
| Fill | while held |
| Capture mark | now |
| Sound-control home edits | now |

- A muted part keeps generating (§12.5). Unmuting a part a Break has silenced brings it back only when the Arc does.
- "Back to previous sound" remembers one previous pick per part, as performance state.

### 13.5 Control layout

Stock habits come first: a control does what its printed label or stock behaviour suggests wherever Perform has a matching action. The prototype on [`prototype/perform-control-layout`](https://github.com/gismya/deluge-generative/tree/prototype/perform-control-layout) at `111e4968` is the visual reference.

**Grid pages.** SONG, CLIP and KEYBOARD switch the grid page, and their LEDs show which is on. The sidebar is the same on every page.

- **SONG, the Arc map.** One column per phrase. Column 4 is now; columns to its left have played and columns to its right are the Arc's plan.
  - Rows 1–6, parts: bright = sounded, the now column flashes live, dim = planned to sound. Holding a part's row focuses it.
  - Row 7: effective Energy against the Recipe centre. Blue below, green at, orange above.
  - Row 8: the Section of each phrase.
  - A queued action blinks in the column where it lands, and the plan redraws to show its result.
  - Turning X scrolls. Tapping X returns to now.
- **CLIP, one part in depth.** It shows the last part held or locked, and choosing this page doesn't move the gold knobs.
  - Row 1: takes 1–16. Bright = playing, dim = an earlier take, grey = a new take, blinking = queued. Tapping a take queues it.
  - Row 2: FREEZE, NEW SOUND (SHIFT = previous sound), SOUND EDITOR.
  - Rows 3–8: Brightness, Thin, Decay, Space, Drive and Wobble as horizontal faders. Tapping a pad sets home. The white pad is the live value, drift included.
- **KEYBOARD, performance pads** (stock KEYBOARD opens PerformanceView, so this keeps the habit).
  - Columns 1–8: Energy, Density, Complexity and Motion as faders, two columns each. Tap = new centre. Hold = a temporary value that springs back on release. White = where the Arc has the value now.
  - Column 14, rows 1–4: phrase position.
  - Columns 14–16, rows 5–8: FILL, held.

**Sidebar**, the same on every page:

- **MUTE/LAUNCH, rows 1–6:** part mute at the next bar; SHIFT = now. X held + MUTE pad = solo, as stock. Colours are stock: green active, red muted, blue solo, dim = silenced by the Arc or soloed out.
- **AUDITION/SECTION:** rows 1–3 force Groove, Break or Build (again cancels). Row 5 holds the Arc. Row 8 is End: hold 0.6 s to arm (the pad brightens while held); a tap cancels.

**OLED.** Only the stock 7, 9, 13 and 20 px fonts, no 5 px text. Popups are stock style.

- **Status line, always shown.** Left: Section and count, and HELD. Right: the next queued action as an inverted tag with its bar count (`>BR3`, plus `+` when more are queued), or bar.beat when nothing is queued. EXT when following an external clock. A Capture dot: filled = marked, blinking = log full. A phrase bar underneath. STOPPED when stopped.
- **The page follows the grid page.** SONG → ARC: now and the next 7 phrases as boxes (queued ones blink, Drop marked) and the Arc's next change. CLIP → PART: sound pick, take n/m, frozen, and the six controls as bars (home plus a live tick). KEYBOARD → MACROS: four bars, fill = centre, tick = where the Arc has it.
- **SELECT turn** shows QUEUE (every queued action with its countdown), RECIPE or CAPTURE for 4 s. **SELECT push** opens the Perform menu (§13.6).

**Gold knobs.** Pages are stock everywhere, and the F page is remembered per target, as stock does. Perform owns the mod buttons, so pressing F1 again never turns the VU meter on over the mute column. Knob LEDs show the value; for macros, that's the centre.

- **Recipe level** (nothing focused): F8 = Energy / Density. Pressing F8 again switches to Complexity / Motion, and the F8 LED blinks while that set is on. This displaces stock's song-level decimation and bitcrush. F1–F7 stay stock song FX, pushes included (F2 filter type, F7 stutter).
- **Part focused:** the part's stock Sound pages. Turning a param a control targets sets that control's home: CUTOFF = Brightness, RELEASE = Decay, REVERB and DELAY AMOUNT = Space, MOD DEPTH = Wobble. Pressing F7 again gives Thin / Drive. Plain F7 stays stutter / CUSTOM 1.
- **Focus:** holding a part's row on the SONG page puts it on the knobs while held, like holding a clip. Double-tap locks it. AFFECT ENTIRE unlocks, and its LED is lit at Recipe level.

**Buttons.**

| Control | In Perform |
|---|---|
| PLAY / SHIFT+PLAY | Start with the Intro, or stop / start without the Intro. X+PLAY is blocked |
| RECORD | Capture mark (LED on = marked, blinks when the log is full). SHIFT+RECORD stays RESAMPLE |
| SYNC-SCALING | Fill while held, whatever the community setting |
| TEMPO turn / SHIFT+turn | Recipe tempo / swing, gliding (needs the tempo hook, F4). TAP taps the Recipe tempo. SHIFT+TAP = metronome |
| SCALE | key and scale editor. The LED blinks while a key change is queued |
| SYNTH or KIT | focused part: new sound at the phrase line. SHIFT = back to the previous sound |
| CROSS-SCREEN | focused part: freeze (LED = frozen) |
| Y turn / Y push | focused part: take back / forward (forward past the newest makes a new take) / show key, scale and seed |
| SHIFT + main pad | sound editor at the printed param: the focused part's Sound, or song FX with nothing focused. Pads, PLAY and knobs stay live. BACK closes |
| BACK tap / hold | cancel the newest queued action / open the leave menu (§13.8) |
| SAVE / SHIFT+SAVE | quick save while playing, full save UI when stopped / always the full save UI (§16.4) |
| LOAD | Recipe load browser (§16.5) |
| MIDI, CV | reserved for later outputs |
| TRIPLETS | unused. LEARN stays stock |

**Queued actions.** A queued action blinks on its pad and shows in the status line, and QUEUE lists every one. Pressing the action again cancels it, and so does a BACK tap, newest first. Quantised actions close 12 ticks early (§12.2).

**Pad redraws.** A full pad redraw costs 21–22 ms of UART, shared with the OLED. Redraw only changed rows. Once per 16th uses about 19% of the link at 130 BPM, and the ceiling is about 45 full redraws a second.

### 13.6 Perform menu

Stock look, opened by SELECT push:

- CANCEL CHANGE (shown only while a genre change, Regenerate or LOAD is queued)
- GENRE (the current genre is labelled `= REGEN`)
- REGENERATE
- KEY / SCALE
- GLIDE
- SEED + TAKES
- CAPTURE

Genre and Regenerate confirm with a choice: BUILD INTO A DROP or HARD CUT AT PHRASE.

### 13.7 Sound editor, MIDI and external tempo

**Sound editor overlay.** A part's normal sound editor opens inside Perform while it keeps playing (SHIFT + main pad, or SOUND EDITOR on the CLIP page). Pads, PLAY and knobs stay live with no grey-out. Knob moves set homes (§11.6). It needs F6 and F7.

**MIDI in.**

- **MIDI-follow CCs** go where the gold knobs point: the focused part's Sound, or song-level params with nothing focused (F8).
- **External notes are dropped**, with their pitch bend, mod wheel and MPE, whether from MIDI follow or an Output's learned input. They would be missing from the steering log and would bypass the note-off table and mute gate.
- **MIDI-learned and MIDI-follow param edits** move homes like a knob turn and are logged. An incoming CC is read as a live value: home = incoming − current layer offset. LEARN stays stock, so a param learned inside Perform lives on that part's Output and is lost when the Output is replaced. MIDI follow is the route that lasts.
- **Macros over MIDI.** CCs 114–117 on the MIDI-follow channels steer Energy, Density, Complexity and Motion. 0–127 scales to 0–50. They set the Recipe centre, are logged, and get feedback.
- **Feedback** sends each sound control's live value (home plus all layers) at the stock automation-feedback rate; "disabled" means only on change. Stock re-sends only `isAutomated()` params, so Perform needs its own case.

**Global MIDI commands.**

| Command | In Perform |
|---|---|
| PLAY, FILL, SHIFT | as the panel button. PLAY starts with the Intro |
| RECORD | Capture mark |
| TAP | Recipe tempo, as below |
| TRANSPOSE | queues a Recipe key change to the next phrase line: the played note's pitch class becomes the key, the scale is kept |
| PLAYBACK_RESTART | blocked |
| LOOP, LOOP_CONTINUOUS_LAYERING, UNDO, REDO, NEXT_SONG | ignored. Perform has no undo |

**Tempo.**

- TAP sets the Recipe tempo through the tempo hook and glides at the Glide setting.
- No tempo CC. MIDI clock out stays stock and follows the gliding song tempo tick by tick.
- **Under an external clock** the master owns tempo and the glide is off. The Recipe tempo tracks the clock: sampled once per bar, rounded to a whole BPM, logged only when the rounded value changes. A tempo turn shows the stock tempo popup marked EXT and changes nothing. TAP is ignored. SHIFT+turn still steers swing. X+turn nudges sync as stock. LEARN+turn is swallowed (F4), so the clock-out scale can't change inside Perform.
- When the clock stops being followed, the transport stops (stock), and the next PLAY starts at the last tracked tempo.

### 13.8 Leaving

Holding BACK opens the leave menu, a stock context menu, while playback continues. The verb is in the title, so every option fits 13 characters. It starts on the first option.

| Situation | Title | Options |
|---|---|---|
| Marked pieces, last one unmarked | `KEEP: 2 MARKED` | MARKED + LAST, MARKED ONLY, NOTHING, CANCEL |
| The last piece is itself marked | `KEEP: 2 MARKED` | MARKED, NOTHING, CANCEL |
| No marks | `LEAVE, KEEP:` | LAST, NOTHING, CANCEL |
| No piece was played | `LEAVE PERFORM?` | LEAVE, CANCEL |

- Any choice but CANCEL stops the transport.
- If pieces are kept, Capture builds and saves them (§17.7). The player lands on the last saved song in arranger view.
- If nothing is kept, the player lands on a new blank song in session view, the same as SHIFT+LOAD → NEW.

Sources: [Perform interaction model][t15], [Perform control layout][t38], [Entering Generative mode and the Compose screen][t39], [Compose screen and Generative menu prototype][t50], [External tempo and MIDI in Perform][t51], [Generator randomness and seeding][t17], [Deluge UI conventions and free controls][t33], [Measure memory and load times on a Deluge][t37], [Recipe files][t40], [Capture][t35], [Firmware changes the design needs][t43].

## 14. Generative menu, entry and the Compose screen

### 14.1 The Generative menu

- **SHIFT+SONG in session view** opens a stock context menu titled GENERATIVE over the session view, with **PERFORM** and **COMPOSE**. The view underneath keeps drawing and the song keeps playing. BACK closes it with nothing changed.
- Arranger view keeps its stock SHIFT+SONG, so from arranger the player presses SONG first.
- Picking one opens a full-screen stock submenu, **PERFORM FROM** or **COMPOSE FROM**, with three rows:
  - **CURRENT RECIPE**, only once one exists;
  - **GENRE** ▸, a submenu of the five genres that opens on the Current Recipe's genre. Picking one rolls a fresh Recipe: rolled facts, rolled sound picks, a new seed, takes at 0 and the genre's default macros;
  - **FILE...**, the Recipe load browser (§16.5).
- The fonts have no "…" or "▸": write FILE..., and use the stock submenu arrow icon.

### 14.2 The Current Recipe

- Generative mode holds one **Current Recipe** (Recipe, seed and takes) in RAM until power-off. Perform steers it and the Compose screen edits it, so both modes start where the player left off. Rolls that were never built carry over too.
- Picking a genre or a file replaces it, without asking.
- It remembers the name of the file it was loaded from or last saved as (§16.3). Picking a genre or Regenerate clears that name.

### 14.3 Replacing the song

- The firmware has no unsaved-changes prompt: it doesn't track edits, and LOAD replaces the song without asking.
- Generative mode asks every time before replacing the song, with a stock one-option context menu **DISCARD SONG?** → OK, modelled on "Clear song?". BACK backs out with nothing changed.
- It is asked after the source pick for PERFORM, and on BUILD in Compose.

**Perform entry.** After DISCARD SONG?, Perform's song (Outputs and sound loads) is built privately while the current song keeps playing, with the stock working animation (the 17×9 icon, top right). BACK cancels the build and leaves the old song untouched. Perform's song then swaps in stopped on the SONG page, with STOPPED on the status line.

### 14.4 The Compose screen

A stock-style menu that edits the Current Recipe before building. The Recipe stays fixed for the whole song. There's no audition: hearing a Recipe means playing it in Perform.

- Three rows on screen, each row's value right-aligned. Pushing a value row edits it in place: the value blinks, SELECT turns it, push or BACK finishes. Rows with ▸ open a submenu.
- **Title**: genre, tempo, key and scale, such as `PSYTRANCE 146 A# HUNGARIAN MINOR`, in the bold title font. It side-scrolls when wider than 128 px, like a long song name in the stock session view.

| Row | Value | Behaviour |
|---|---|---|
| FROM | genre ▸ | the source list of §14.1 |
| REGENERATE | | asks REGENERATE? → OK |
| SEED + TAKES | ▸ | submenu titled `SEED 0Y8438B`: NEW SEED, then one row per part with its take (number editor) |
| TEMPO | BPM | stock big-number editor |
| SWING | amount | stock big-number editor |
| KEY | root | |
| SCALE | name | short name (HARM MIN, HUNG MIN) when the full one doesn't fit |
| ENERGY, DENSITY, COMPLEXITY, MOTION | 0–50 | stock continuous number editor (number and bar) |
| SOUNDS | n PARTS ▸ | one row per part with its pick (`BASS 017 ACID LINE`). Push re-rolls from the palette, SHIFT+push goes back to the previous pick. Drums and FX rows (`DRUMS: 8 LANES` ▸) open a lane list that works the same way |
| LENGTH | m:ss | target length, 1:00–20:00 in 30 s steps, default 4:00, stock big-number editor. Kept until power-off, never saved |
| BUILD | | §14.5 |

- Sound-control homes, freeze, mutes, and Arc force and hold are left out. They are steering or performance state, not Recipe values. After BUILD the result is a normal song, shaped in the stock views.
- SAVE saves the Current Recipe (full save UI) and LOAD is the same as FILE... (§16).

### 14.5 Building

1. BUILD → DISCARD SONG? → OK.
2. The OLED shows **COMPOSING** with a progress bar of bars rendered against the bars expected at the target length plus one Outro phrase, so it may jump at the end. Below it: **BACK = CANCEL**. The current song keeps playing or stays stopped.
3. BACK cancels at once and throws the private song away. Nothing changes.
4. The finished song swaps in **stopped** and opens in **arranger view**.

Sources: [Entering Generative mode and the Compose screen][t39], [Compose screen and Generative menu prototype][t50], [Compose output shape][t16], [Recipe files][t40], [Deluge UI conventions and free controls][t33].

## 15. Compose output shape

### 15.1 Input and length

- Compose renders the Current Recipe, seed and takes, re-rolled takes included, so "Compose matches Perform" holds for a Recipe that was steered and then rendered.
- It runs the Arc exactly as an untouched Perform from Start would: the Intro, then the automatic cycle under Motion and the seed.
- Once the song passes the target length, End is queued at the first point that doesn't cut a Build or a Break, normally the end of a Groove, and the Outro plays from there. Sections are never cut short to hit the target. At Motion 0 the song is Intro, Groove up to the target, then Outro.

### 15.2 Session view and arranger

- Compose writes both. Session view has one clip per part per **Section occurrence** (Intro, Groove 1, Break 1, Build 1, Groove 2, …), and each occurrence gets its own **session section**, numbered in Arc order.
- The arranger places those same clips in Arc order, from Intro to Outro.
- The Deluge has 24 session sections (`kMaxNumSections`). If a song reaches 23 occurrences, Compose queues End and the Outro takes the 24th. The OLED says the song was ended early.

### 15.3 Clips

- One clip per part per Section occurrence, as long as the whole Section. A 6-phrase Groove of 8 bars is one 48-bar clip.
- The Drop's lift is written into the start of the following Groove clip, and the Gap into the end of the Build clip. Gestures never cross a clip boundary: Rise runs across the Build clip, and Reset lands on the first step of the next Groove clip.
- **No reuse.** Every occurrence gets its own clips, even when identical to an earlier one. Launching a session section stops every clip outside it, so a reused clip would leave later session sections incomplete.
- A part that doesn't sound for a whole occurrence (left out of Break keeps, or below its Density gate the whole time) gets **no clip** there. A part that sounds for part of a Section gets a clip with rests.
- Every clip of a Drums or FX part has a row for every lane the part has, in the fixed lane order, even when silent.

### 15.4 Notes

- Fixed notes: probability 100, no iterance, no fill flags, exactly as Perform plays them. Variety comes from the Arc. The player can add firmware chance afterwards.
- Notes go at exact tick positions, so Lean, swing differences and Ornaments show as nudged notes, and the player can edit ratchets as notes.
- Synth clips are in scale mode. Kit clips aren't.

### 15.5 Song settings

- Tempo, swing, key and scale are set once on the song from the Recipe. `key.rootNote` and `key.modeNotes` are assigned directly.
- After rendering it is a plain Deluge song for pitch: stock key transpose and mode remap work, and stock refuses a scale smaller than the pitch classes the song uses. Compose adds no guard.
- Nothing goes into the arranger's song-level automation.

### 15.6 Automation

- Each part's automation lives in its own clip, per occurrence. Each clip carries its own copy of the sound's parameters (stock behaviour), so a base-value edit in one clip doesn't spread.
- **Set the base, automate only movement.** Each param's stored value is home + the macro layer at the Recipe centre. A param that doesn't move gets no automation.
- A sound control lands as the firmware params it maps to on that engine (§11.2). FM Brightness becomes the modulator 1 and 2 volumes. The song has no "Brightness" param.
- **Ramps** (Rise, Close, Wash, effective-Energy changes) become interpolated nodes at their start and end, plus enough points between to follow the curve. **Snaps and Throws** become step-exact nodes.
- **Drift** is thinned to at most one interpolated node per bar per param, so the result stays editable.
- `setNodeAtPos` doesn't flag the param as automated, so Compose must flag it.

### 15.7 Build and handover

- Compose builds into a **private `Song*`**, never `preLoadedSong`: the next Session launch event swaps in a non-null `preLoadedSong` even when half-built.
- It steps the core in UI/task context and yields every N steps to stay well under 3 ms per run. Builds must yield so BACK can cancel. In-memory construction must call `AudioEngine::routineWithClusterLoading()` periodically, and those calls run sequencer ticks too.
- The finished song is swapped in **stopped**, which keeps `deleteSoundsWhichWontSound` away from a playing song. `clear_song.cpp` is the template for the swap.
- The song opens in arranger view, **unsaved but named** `GEN <GENRE> <seed code>` (for example `GEN TECHNO 1A2B3C4`). Nothing is written to the card until the player saves.
- The song keeps no link to its Recipe beyond its name. Song XML drops unknown attributes on re-save, so an embedded link wouldn't survive.

### 15.8 Song construction facts

- `Song::writeToFile()` only works for `currentSong`: it writes reverb from the live `AudioEngine`, the pad preview from `PadLEDs` and uses the root UI. Saving happens only after the swap.
- `Clip` doesn't initialize `loopLength`. Set it before attaching an instrument.
- `getOrCreateNoteRowForYNote` can change the song scale. Write the song key first and use rows that are in scale.
- Some out-of-memory paths freeze the firmware instead of returning an error.
- A song needs each instrument's params in its clips (`soundParams`, `kitParams`, and per-drum `soundParams` in each note row). With params left only on the instrument, synth songs freeze with E412 on load and kit songs report FILE CORRUPTED.

Sources: [Compose output shape][t16], [Compose song construction path][t18], [Automation][t14], [Pitch model][t21], [Generator scheduling][t19], [Firmware changes the design needs][t43], [Time a scripted capture in delugemu][t36], [Firmware architecture of v1.3.0-rc3][t2], [Entering Generative mode and the Compose screen][t39].

## 16. Recipe files

### 16.1 Contents

A **Recipe file** holds the Current Recipe and nothing more:

- the genre's stable name and its genre version;
- tempo and swing (the glide targets if a glide is running), key and scale;
- the four macros;
- every sound pick as a card path;
- the seed and every part's take.

Freezes, mutes, sound-control homes, Arc position, Compose LENGTH and the Glide setting stay out. A Recipe file holds no steering, so it doesn't replay a piece. A Capture song does that.

### 16.2 Genre versions

- On a version mismatch the file still loads and the OLED shows a short notice (`OLDER TECHNO`). Saving again writes the current version. Old genre tables are not kept.
- A file naming a genre this firmware doesn't have is refused with `UNKNOWN GENRE`.
- A file fails to load only if it is unreadable or its genre is unknown.

### 16.3 Format, folder and names

- XML through the stock serializer, in a top-level `RECIPES/` folder, created on first save. A folder of its own keeps Recipe files out of the song browser.
- A never-saved Recipe gets the default name `<GENRE> <seed code>` (`TECHNO 1A2B3C4`). If that name is taken, the stock letter variation applies.
- Every save suggests the next letter variation of the remembered name (`SONG001` → `SONG001A`, stock). Overwriting needs an explicit pick and a confirm, like a stock song save.
- After any save, the Current Recipe takes the saved name.

### 16.4 SAVE

- **In Perform while playing**, SAVE is a **quick save**: it saves at once under the suggested name, a popup shows the name, and the grid stays put. A quick save never overwrites. If no letter variation is free, it refuses with `NO FREE NAME`.
- **In Perform while stopped**, and **on the Compose screen**, SAVE opens the full save UI: QWERTY on the pads, then the browser on `RECIPES/`.
- SHIFT+SAVE in Perform always opens the full save UI.
- Recipe files are written only by SAVE. Capture saves songs only.

### 16.5 LOAD

- LOAD opens a stock-style load browser on `RECIPES/`, the same as FILE... in the source lists. The QWERTY appears on entry and hides once the player scrolls, and the root UI's pads show again. BACK leaves with nothing changed.
- **In Perform while playing**, the music keeps playing in the browser. Choosing a file is queued like a genre change (§9.7): Build into a Drop on fresh Outputs, tempo and swing glide over the Build, a new piece at the Drop, cancellable until the Build starts. It uses the file's seed, takes, facts and macros. To hear it from the top, the player stops and starts again.
- **In Perform while stopped**, it rebuilds directly, like entering Perform.
- **On the Compose screen**, it replaces the Current Recipe, ready for BUILD.
- No unsaved-changes prompt anywhere. Loading a file, picking a genre and Regenerate replace the Current Recipe without asking.

### 16.6 Missing sounds and sharing

- On load each sound pick's path is checked. A missing one is re-rolled from this card's palette for that part or lane, from fresh entropy, and the OLED shows `2 SOUNDS REPLACED`. The Current Recipe keeps the replacements. The notes don't change, because the seed doesn't cover sounds.
- A path that still exists but points at a different sample isn't detected. That's accepted.
- A file isn't self-contained. It plays the same for another player with the same firmware, factory card and Generative pack.

Sources: [Recipe files][t40], [Entering Generative mode and the Compose screen][t39], [Generator randomness and seeding][t17].

## 17. Capture

### 17.1 What it is

**Capture** keeps **pieces** played in Perform as regular Deluge songs. It replays a **steering log** through the Compose driver. It never records notes, and nothing is recorded from the audio routine.

This rests on the contract in §6.3. A harness spec checks it directly: a Perform run with a steering log must produce the same event stream as a replay of that log (§21.2). A determinism bug would make the song differ from what was heard.

### 17.2 The steering log

- Perform logs every steering action at its step position: Recipe edits (macros, facts, key, tempo tracked from an external clock), Arc forces and holds, End, pattern and sound re-rolls, take steps, freezes, mutes and solos, Fill, and home changes from knobs, faders and MIDI.
- Knob moves are merged to at most one value per control per step.
- The log is one fixed, preallocated buffer, sized at implementation. A few hours of busy steering is tens of KB.
- When it fills, new marks are refused with an OLED message, and the current piece stops logging at the last step that fit. The leave menu then says its span was cut there.

### 17.3 Pieces

- A **piece** runs from a Start, Regenerate, genre change or mid-set LOAD until the transport stops or the next Regenerate, genre change or LOAD.
- If End was pressed, the piece includes the Outro up to where it finishes. A plain stop cuts the piece at the stop step. A Start after a stop begins a new piece.
- At a transition, the old piece ends on the last step before the Drop, Build included, and the new piece starts at the Drop. A captured piece that began that way opens on a Groove with the Drop lift. Every piece has one set of Outputs.
- When a piece ends, its log is thrown away unless the piece is marked.

### 17.4 Marking

- RECORD (or MIDI RECORD) marks the current piece to keep as a whole. The log keeps running until the piece ends, so music after the press is included.
- Pressing again unmarks it. The status line dot and the RECORD LED show whether the current piece is marked.
- Several pieces can be marked in one set, and their logs share the buffer.
- Marks and logs live in RAM only. A power-off or crash before leaving loses them.

### 17.5 Song shape

- The Compose shape (§15): one clip per part per Section occurrence, each occurrence in its own session section, arranger in Arc order.
- Occurrences past the 23rd become **arrangement-only clips** (`section = 255`, in `Song::arrangementOnlyClips`), because a piece can't be ended early. The arranger has no clip or instance cap apart from RAM.
- A Capture of an untouched Perform comes out identical to a Compose of the same length.
- One song per piece. Pieces are never merged, because they differ in genre, key and swing.

### 17.6 Changes partway through a piece

The song gets the piece's first tempo, swing, key and scale, which are what a Compose of the starting Recipe would write.

- **Tempo** changes and glides become song-level tempo automation (`params::UNPATCHED_TEMPO` in the song's `paramManager`). It plays back only in arrangement mode.
- **Swing** can't be automated (`Song::swingAmount`). Notes played at another swing get the difference built into their tick position, the way Lean is.
- **Key and scale** are song-wide only. A clip with any note outside the song's scale is written with scale mode off, because a scale-mode clip with an outside note widens the whole song's scale. A key change partway through a Section doesn't split the clip; that clip is just chromatic.
- **Sound re-rolls** start a **new track** from the re-roll step, and that occurrence's clip is cut in two there, so every track has one sound for its whole life.
- **Mutes.** What a muted part generated while muted is left out, as rests, or as no clip for an occurrence muted throughout. Capture records what was heard.

### 17.7 Building on leave

- The leave menu (§13.8) offers the marked pieces plus the last piece, marked or not, so forgetting to press costs nothing.
- On a keep choice the pieces are built one at a time in set order. Each is replayed privately the way Compose builds, swapped in stopped, and **saved to the card** through the normal save path for the current song. This breaks Compose's "nothing goes to the card until the player saves" rule on purpose: saving a song that isn't current is unverified.
- The OLED shows the COMPOSING layout titled `BUILDING 2 OF 3` or `SAVING 2 OF 3`, with **BACK = STOP** under the bar.
- **BACK stops Capture at once**, without asking. The song being built is thrown away, a card save that has started finishes, and songs already saved stay.
- The player lands on the last saved song in arranger view, or a blank session if none was saved.
- **Name:** Compose's name plus ` LIVE` (`GEN TECHNO 1A2B3C4 LIVE`), within the OLED name limit. Name clashes on the card use the firmware's normal rule.
- Facts: a stored `Note` is 24 bytes, and notes allocate internal RAM first, then SDRAM.

Sources: [Capture][t35], [Compose screen and Generative menu prototype][t50], [Entering Generative mode and the Compose screen][t39], [Recipe files][t40], [External tempo and MIDI in Perform][t51].

## 18. Generative pack

### 18.1 Shape

| On the card | Holds |
|---|---|
| `SYNTHS/GENERATIVE/` | our presets |
| `SAMPLES/FX/<lane>` | FX samples |
| `SAMPLES/DRUMS/<folder>` | `GEN`-prefixed one-shots in the factory folders, so the lane→folder table finds them |
| `GENERATIVE/` | reserved for optional palette overrides |
| `RECIPES/` | created by the firmware |

- In the repo: `pack/`, laid out exactly as on the card. The harness reads it from there. WAVs go straight into git, with no LFS.
- `pack/LICENSES.md` lists each file's origin and licence. The pack's licences don't affect the firmware, so it may ship GPL-3.0-only data such as PERF-1's DX and DY voices (ADR 0001).
- Distribution: one zip attached to each firmware release, unzipped onto the card root.
- Version skew between firmware and pack is accepted for now.

### 18.2 Contents

- **Presets in `SYNTHS/GENERATIVE/`:**
  - `FX/`: Riser, Fall, Impact and Siren presets.
  - A plain house chord stab with no built-in interval and no saved portamento.
  - A dub sub bass, a melodica, a tight psy bass (sustain 0) and a soft sine sub for Ambient.
  - `DX/`: only the PERF-1 DX/DY voices a palette uses, as synth presets with the patch inside (`dx7patch`). They are picked during tuning: likely EP stabs, basses and Ambient bells. They play even with the DX7 runtime setting off.
- **Samples**, all rendered from our own Deluge presets, so their licence is ours:
  - At least 3 risers in `SAMPLES/FX/Riser`, trimmed to land on the file end.
  - Two or more 909-style rides and claps, and a handful of soft mallet, brush and bell one-shots for Ambient, named `GEN <kind>.wav`.
- **Not in the pack:** a 303 acid preset (165 Acid Arp works with its arp cleared, and the preset comes back only if 165 fails its ear check), a psy drum set (tags reach `SAMPLES/Artists/Dean Daughters/Psy`), pack falls and impacts, whole `.syx` banks, re-saved factory FM presets, and palette override files.

### 18.3 Built-in palettes

- The v1 palettes start from the [draft palettes](https://github.com/gismya/deluge-generative/blob/research/factory-card-content/docs/research/factory-card-content.md#6-draft-palettes) in [Factory card content per role][t34], with these changes: pack entries where the drafts say "Pack"; Riser lanes list only pack risers; Dub gets a Siren lane and a factory boom added to its Impact lane; Ambient drops its Impact lane.
- Factory card facts: 103 subtractive, 54 FM and 14 ringmod synth presets, no wavetable or DX7. Bass, Pad and Lead are well covered and Chords thinly. Every drum lane has at least 3 samples. There's no `SAMPLES/FX`.
- Avoid presets with off-key tuning (040, 164 and 060 among 8). Presets with a built-in interval (30 of them) suit Bass, Lead and drones but not Chords.
- Every palette is starting data. A wrong pick is a data edit, and no ear check blocks implementation. The first check is 165 Acid Arp with its arp cleared, when Techno is first built.
- The pack grows genre by genre: each milestone ships the share its genres name (§22).

Sources: [Generative pack contents][t42], [Factory card content per role][t34], [Porting policy][t23], [POC gaps][t52], [Sound generation][t13].

## 19. Firmware changes

About a dozen small changes to existing firmware, none large. Sizes: S is under ~30 lines, M up to ~150. Paths are relative to `src/deluge/` at `v1.3.0`. The full findings are on [`research/firmware-changes`](https://github.com/gismya/deluge-generative/blob/research/firmware-changes/docs/research/firmware-changes.md).

| # | Change | Where | Size | Needed by |
|---|---|---|---|---|
| F1 | Generator hook after the arp loop. Perform must keep session playback | `playback/mode/session.cpp:2541-2553` | S | §12.1 |
| F2 | `AutoParam` live ramp: set the interpolation increment and flag the param as interpolating. Render pre-checks already tick clip, kit-row and kit-level params | `auto_param.cpp:859-868`, `param_set.cpp:105-109`, `patch_cable_set.cpp:345` | S–M | §11.7 |
| F3 | Player-edit notification for homes. With the sound editor open, gold knobs go `UI::modEncoderAction` → `view`, past Perform | `AutoParam::setCurrentValueInResponseToUserInput` (`auto_param.cpp:101`) | S | §11.6 |
| F4 | Tempo encoder hook. Test `getRootUI()` so it works under overlays, and swallow LEARN+turn | `hid/encoder_input.cpp:137-151` | S | §13.5, §13.7 |
| F5 | `UIType` value, `uiTypeMap` entry (compiled into test-port builds too), SHIFT+SONG in session view | `definitions_cxx.hpp:187-213`, `gui/ui/ui.cpp:476-502`, `session_view.cpp:238-245` | S | §14.1 |
| F6 | The sound editor's song-or-clip choice uses `rootUIIsClipMinderScreen()`. Replace it with a root-UI predicate. Don't make Perform a ClipMinder (that turns on SAVE preset, the KEYBOARD swap, automation entry and arrangement PLAY). Fix the null `clip->type` read in `setup()` here too | `sound_editor.cpp:1106, 1421, 1953, 2161, 2311, 2387` | M | §13.7 |
| F7 | Sound editor cases for Perform: no grey-out, pads forwarded, SONG must not `changeRootUI` | `sound_editor.cpp:477-509, 1777-1780, 829-841` | S | §13.7 |
| F8 | MIDI follow: a Perform case in the fallback that otherwise picks the current clip | `io/midi/midi_follow.cpp:420-454` | S | §13.7 |
| F9 | Guard RECORD and X+PLAY under an overlay, so RECORD can't write automation into Perform's clips | `playback_handler.cpp:213-255, 280-292` | S | §13.5 |
| F10 | Global MIDI commands under Perform: NEXT_SONG, LOOP, PLAYBACK_RESTART, UNDO, TRANSPOSE | `playback_handler.cpp:162-191, 2893-2965` | S | §13.7 |
| F11 | Undo: guard `getNewAction` and `allowedToDoReversion`, because reverting casts the current UI to `TimelineView*` | `model/action/action_logger.cpp:86-100, 565, 580, 867-869` | S | §13.7 |
| F12 | `currentSong` → `song` in `loadInstrumentFromFile`. **Not needed in v1** (whole-kit picks only) | `storage/storage_manager.cpp:406` | S | none |
| F13 | Optional: Capture's save without the save UI. New code can call `createXMLFile` → `writeToFile` → `closeFileAfterWriting` unchanged, or `SaveSongUI::performSave` can be refactored (M). Left to implementation | `save_song_ui.cpp:117-528`, `song.cpp:1177-1191` | S or M | §17.7 |

Also:

- Optional: `View::setModLedStates` overwrites Perform's SONG, CLIP and AFFECT ENTIRE LEDs. Perform can re-assert them instead.
- MIDI-follow feedback needs a Perform case next to stock's `isAutomated()` check (`midi_follow.cpp:1336`).
- F9–F11 must land before any beta reaches testers.
- Fixes that aren't specific to Generative mode get an entry in [`docs/upstream-candidates.md`](../upstream-candidates.md) (rule in `AGENTS.md`). F6's null read and F12 are already there.

**No firmware change needed** for: loading a song from memory (Compose and Capture build through the model API); Euclidean and pattern logic (the Generator has its own); `loadSynthToDrum`'s error paths (fixed upstream before `v1.3.0`); the private-song swap; the load-time settings; the Siren (a held note on an ordinary Sound, check by ear); the drum scan and tags; Recipe files and their browsers (`nextDefaultName`, `fileExists`, new `SaveUI`/`LoadUI` subclasses); BACK hold (`exitUI` is virtual); Fill; and mute and solo.

**Root-UI checks.** About two dozen `== &performanceView` checks and `getUIType()` switches exist in about nine files. The ones that would mistreat a new root are covered by F6–F11. The SONG LED and `clip_minder.cpp`'s return-to-view logic also assume session or arranger: check them when wiring Perform.

Sources: [Firmware changes the design needs][t43], [External tempo and MIDI in Perform][t51], [Deluge UI conventions and free controls][t33], [Build order][t44], [Building Generator Kits in code][t28].

## 20. Budgets and measured facts

Measured on a real OLED Deluge unless marked.

| Fact | Value |
|---|---|
| CPU | one Cortex-A9 at 400 MHz, cooperative scheduler. Ticks run inside the audio routine, which has about 2.9 ms of buffer (128 samples at 44.1 kHz) |
| Per-tick work | keep it in the tens of µs, with no card access and no allocation (estimate) |
| Task runs | keep each well under 3 ms, or split and yield. State read by the tick path must be consistent across yields |
| Voice starts | 4 per render window, 1 under heavy load. Extras wait one render, about 2.9 ms. No fixed voice cap; culling is driven by CPU load |
| Fast heap free | 1002 K with no song; 749 K with a typical song; 694 K with that song plus a Perform roster playing (RelWithDebInfo, Release has about 46 K more) |
| Two songs in RAM | 591 K free at the swap with two typical songs, 273 K with two heavy ones (largest block 148 K). No late renders |
| Perform roster | about 240 K for two rosters (2 × 12 lanes × 8 K + 4 synths × 10 K), so two rosters fit |
| Kit lane | about 8.1 KB fast heap plus the sample in SDRAM. 10–15 ms per cold lane while playing; a 12-lane kit under 0.2 s |
| Synth preset load | 5.3 ms, 10 K fast heap |
| Entering Perform | 67 ms while playing with 4 lanes and a Bass |
| Compose song | about 230–250 K fast heap for a heavy-song-sized render. XML parsing is 2–5× slower while audio runs |
| Pad redraw | 21–22 ms of UART per full redraw, shared with the OLED. Pad sends defer behind OLED updates |
| OLED | 128×43 visible, 4 rows of 21 characters or a title plus 3 rows. Redraws at most every 10 ms. `OLED::setupSideScroller` doesn't scroll while playing; the Compose title scrolls the way the stock session view scrolls a long song name |
| Tempo | the hardware plays exactly the set BPM (130.0000 by the sample clock). delugemu runs about 0.5% slow |
| Stability | the POC's hook ran 31 minutes with no leak, drift, stuck notes or late renders, and followed swing and tempo changes exactly |
| `new` | global `operator new` and std containers use only a 2 MiB SDRAM pool and throw when full. The Generator doesn't use them (§2.1) |
| Song loads | the "buttons and pads" task can run up to 20 ms without yielding during a song load |
| System clock | `getSystemTime()` reads about 0.8% short against real time |

The POC's core was about 1.65 KB of state (`sizeof(Generator)` 1664 B) with no allocation.

Sources: [Runtime budget on the Deluge][t3], [Measure memory and load times on a Deluge][t37], [Hardware tests of the Perform POC][t45], [Deluge UI conventions and free controls][t33], POC findings on the [map][poc].

## 21. Testing and CI

### 21.1 CI

- Only `tests.yml` runs, on PRs into `generative`: x86 unit tests and ARMv7 under QEMU user mode, about 2.5 min. `test-results.yml` publishes its results.
- Every other workflow file is deleted, not disabled: `backport-from-issue`, `backport`, `beta-build`, `build`, `nightly-build`, `pr-build`, `release-build`, `publish`, `clang-tidy`, `format-check`, `deploy-website`, `label-issues`, `label-prs` and `sync-labels`, plus the `.github/scripts` and labeler config files only they use. autofix.ci has no file in the repo and stays. Upstream merge conflicts on deleted files are solved as they come.
- No branch protection. `AGENTS.md` gets a rule: don't merge a PR into `generative` while **Run tests** is red.
- Hardware is never in CI.

### 21.2 Harness

- The Generator's tests are another target in the `tests/` CMake tree. CI runs them in the x86 job only, not `tests/qemu`. Locally they build natively on the Mac with dbt's cmake and Apple clang.
- **Specs** hold invariants and equivalences, from day one:
  - every pitched note is in scale and in its Register;
  - every note-on gets a note-off;
  - voice starts per step, reported against the 4-per-render budget rather than failed, since the firmware defers extra starts (§23.14);
  - interplay rules only target earlier parts (§5.5);
  - an untouched Perform driver run matches a Compose run (§6.8);
  - a Capture replay matches the live steered run (§17.1).

  Equivalence checks compare two live runs, so they store no file.
- **Goldens** catch change: Recipe + seed + takes + steering script → a text dump. Before v1 there is **one golden**: Techno's default Recipe at a fixed seed, short. The fuller set (a default Recipe per genre for a full Arc cycle, plus steering cases such as macro sweeps, force and hold, Fill, take re-roll and step back, Regenerate, genre change, key change and freeze) comes after v1, or earlier when a bug escapes.
- **Retuning on purpose:** one local command regenerates the goldens (such as `--update-goldens`). The new dumps are committed with the retune, which also bumps the genre version, and the commit says which genre was retuned. Review is the text diff.
- **Rough render:** an offline, deterministic render of a Recipe to a WAV, a Standard MIDI File with one track per part, and a text piano roll. It uses our own small voice code (one subtractive voice plus a sample player for kit rows) and reads samples from `pack/` and a local card-shaped folder. Parameters it doesn't model are ignored with a warning. Renders are made locally and never committed.

### 21.3 delugemu (local only)

- Use [SynthstromAudible/delugemu](https://github.com/SynthstromAudible/delugemu). It runs headless, scripted and deterministic in icount mode (`-icount shift=2,sleep=off -S` with `run_until`). The factory card is a prebuilt `mksd.sh` image at `~/source/delugemu-sd`.
- A script in the repo runs **smoke checks** against a local `./dbt build release`: boot; Perform starts and plays N bars (non-silent, no freeze); Compose builds a song, saves it and reloads it; Capture writes its songs; a Recipe file round-trips. The checks are functional only, with no audio hashes.
- **Firmware matches the harness:** render the golden case through Compose in delugemu, read the saved song's notes back into the same text dump, and diff it against the harness golden. This is the only end-to-end proof that the firmware plays what the harness tested.
- **Pinning:** the repo records the delugemu commit the checks last ran with. The script warns when the installed bundle's `VERSION` differs, and bumping the pin is a deliberate commit.
- Gotchas: one boot in three can come up blank, so the script needs a boot check and a retry. Under icount the virtual clock runs faster than real time, so waits count virtual time. pylib's `press` and `load_song` use wall-clock sleeps, so icount scripts use `run_until` plus `button()`. Check timing in ticks or MIDI, not audio.

### 21.4 When agents run checks

These rules go into `AGENTS.md` in M0:

- Harness tests and specs on every Generator change.
- delugemu smoke checks and the firmware check before pushing any change under `src/` that touches Generative mode.
- Hardware runs (the `deluge-hardware-test` skill, over the sysex test port) in the test plan for changes to UI, timing, memory or CPU.

### 21.5 Genre tuning loop

1. **Harness rough render**: rhythm, harmony and interplay in stand-in sounds, with no firmware build.
2. **delugemu capture with the real sounds**, the main loop: a script takes genre + Recipe overrides + seed + length, then builds, captures and plays the WAV. A 60 s capture takes about 18 s on an M4 Pro, and the same seed before and after gives a fair A/B.
3. **The real Deluge**: sign-off, and how steering feels.

Genre tables stay compiled in, so a tweak is an incremental rebuild. There's no dev-only card override.

Sources: [Testing and CI][t41], [Simulation approach][t8], [Time a scripted capture in delugemu][t36], [Build and run v1.3.0-rc3 in delugemu][t9], [delugemu on v1.3.0-rc3][t4], [Generator randomness and seeding][t17], POC findings on the [map][poc].

## 22. Build order and milestones

### 22.1 Starting point

- Work starts fresh on `generative`. The POC branches (`poc/core`, `poc/perform-techno`) are not merged. They are a reference for ideas and scaffolding: the `generator.h` seam, the harness CMake skeleton and the delugemu script. Several of their choices break decisions (the POC layers onto the player's song, never sets LEGATO or CHOKE, divides in the driver, and gives Techno House's clap). Delete the POC branches once v1's first Perform slice plays.
- **First slice:** a **walking skeleton** through every layer: Perform playing **Techno** on a Deluge, with drums and bass, the Arc cycling and the four macros on the knobs. It lands with harness specs from day one. After that, each layer (chords, interplay, sound controls, Ornaments and so on) is its own slice that ends in sound.

### 22.2 Order

- **Genres:** Techno → Ambient → Psytrance → Dub → House. Techno fixes the one pre-v1 golden. Ambient comes second because it is the far end of the model (Pad, Chords, Sustain, a slow Arc), so it finds gaps early.
- **Modes:** Perform (one genre) → Compose and the Compose screen → Recipe files → Capture. Capture needs the Compose driver, and the firmware-matches-harness check needs Compose.

### 22.3 Milestones

| Milestone | Delivers | Release |
|---|---|---|
| M0 Housekeeping | delete the workflow files, scripts and configs (§21.1); add the `AGENTS.md` rules (§21.1, §21.4); add the harness target with an empty spec suite; add the beta release script | none |
| M1 Perform, Techno | every Generator layer, the Perform control layout, sound controls, firmware changes F1–F11, Techno's share of the pack. F9–F11 land before the beta | Beta 1 |
| M2 All genres + Generative pack | Ambient, Psytrance, Dub and House in that order, with their palettes and pack content | Beta 2 |
| M3 Compose + Recipe files + Generative menu | Compose driver, Compose screen, the Generative menu and source lists, Recipe files, the firmware-matches-harness check | Beta 3 |
| M4 Capture | steering log, marking, the leave menu builds | v1 |

- The Generative menu (§14.1) is needed in M1 to enter Perform. M1 builds PERFORM with GENRE ▸ only; CURRENT RECIPE, FILE... and COMPOSE arrive in M3 (§23.9).
- Every beta ships the pack zip as it stands.

### 22.4 Beta builds

Built locally and published by a script in the repo: `./dbt build release` plus the pack zip, then `gh release create --prerelease`. No new CI workflow. Revisit a `workflow_dispatch` workflow only if testers ever need nightlies.

### 22.5 Upstream fixes

No upstream PRs are part of the plan. The `sound_editor.cpp:2161` null read is fixed in the fork with F6. `storage_manager.cpp:406` is left alone. Both are recorded in [`docs/upstream-candidates.md`](../upstream-candidates.md).

### 22.6 Handoff

This spec is split into implementation tickets: one parent issue per milestone (M0–M4), with `ready-for-agent` slices under it, each citing spec sections and wired with blocking edges. The slice order inside M1 starts from the walking skeleton.

Sources: [Build order][t44], [Testing and CI][t41], [Firmware changes the design needs][t43], POC findings on the [map][poc].

## 23. Decisions made while writing this spec

Places where the tickets disagreed or left a gap, and what this spec says.

1. **Compose song name.** [Compose output shape][t16] names the song `GEN <GENRE> <seed code>`, and the [prototype resolution][t50] said it stays unnamed. The user chose **named, unsaved** (§15.7). Capture's `… LIVE` name and Recipe file naming build on it.
2. **Polymeter.** [Recipe parameters][t10] listed it under Complexity, but nothing designed it. The user chose to **drop it from v1** (§1.2). It is in `docs/future-features.md`.
3. **Ambient never drops.** The user chose **gentle section rules**, with no new mechanism: a slow swell with no Gap, roll or Throw, and a Drop lift near 0 (§4.5, §9.5).
4. **Energy and Register.** [Track roles][t12] and [Density as a fourth macro][t24] said Energy shifts the Register, and [Pitch model][t21] never said how. The user chose to **drop it for v1**: Registers are fixed (§7.3). It is in `docs/future-features.md`.
5. **Takes per part.** The CLIP page shows 16 take pads, so v1 has 16 takes per part, 0–15 shown as 1–16 (§6.6). Making a new take past the 16th is refused.
6. **Genre versions.** [Genre model][t11] said genres have no version and a missing genre falls back to a default. The later [Recipe files][t40] gave genres a version and refuses an unknown genre. This spec follows Recipe files (§4.4, §16.2).
7. **Part cap.** [Track roles][t12] set no hard cap on parts. [Generator scheduling][t19] sizes state for 6 parts and the sidebar has 6 MUTE pads, so the cap is 6 (§5.1).
8. **A flam's grace hit under load.** [Sub-step expression][t20] said it may be dropped. [Hardware tests of the Perform POC][t45] showed the firmware delays extra voice starts by one render instead. The spec says it waits (§12.6).
9. **Generative menu in M1.** Build order puts the Generative menu in M3, but entering Perform needs it. M1 builds the menu with PERFORM and GENRE ▸ only (§22.3).
10. **MIDI-learned edits.** [Generator scheduling][t19] said edits from MIDI learn are overwritten at the next step. The later [External tempo and MIDI in Perform][t51] makes them move homes like a knob turn. The spec follows the later ticket (§11.6).
11. **Genre change timing.** [Sound generation][t13] swaps at the first phrase line after loading, and [Perform interaction model][t15] plays a Build first. The spec places the transition's Build like a forced Build, ending on a phrase line once loading is done (§9.7).
12. **Sound-control homes on stop.** No ticket said whether a home survives a stop. A home is the param's value on the Output, so it lasts until the Output is replaced (§13.1).
13. **Mid-set LOAD and pieces.** [Capture][t35] defined piece boundaries before [Recipe files][t40] added mid-set LOAD. A LOAD transition ends and starts a piece like a genre change (§17.3).
14. **The voice-start spec.** [Testing and CI][t41] listed "at most 4 voice starts per step" as an invariant. A busy downbeat with Ornaments can legitimately exceed it, and the firmware delays the extra starts rather than dropping them ([Hardware tests of the Perform POC][t45]). The spec reports the count instead of failing on it (§21.2).

## 24. Source tickets

Every decision ticket on the map, in the order the spec uses them:

- Research: [Firmware architecture of v1.3.0-rc3][t2], [Runtime budget on the Deluge][t3], [delugemu on v1.3.0-rc3][t4], [PERF-1 internals][t5], [Prior art in generative music engines][t6], [Existing firmware features the Generator can reuse][t7], [Compose song construction path][t18], [Licence compatibility for porting][t22], [Sound controls per synth engine][t26], [Building Generator Kits in code][t28], [Deluge UI conventions and free controls][t33], [Factory card content per role][t34], [Firmware changes the design needs][t43].
- Tasks: [Build and run v1.3.0-rc3 in delugemu][t9], [Time a scripted capture in delugemu][t36], [Measure memory and load times on a Deluge][t37], [Hardware tests of the Perform POC][t45].
- Grilling: [Simulation approach][t8], [Recipe parameters][t10], [Genre model][t11], [Track roles][t12], [Sound generation][t13], [Automation][t14], [Perform interaction model][t15], [Compose output shape][t16], [Generator randomness and seeding][t17], [Generator scheduling][t19], [Sub-step expression][t20], [Pitch model][t21], [Porting policy][t23], [Kit lane mapping][t25], [Variation when a sound is picked][t27], [Parts that play some scale degrees][t32], [Capture][t35], [Entering Generative mode and the Compose screen][t39], [Recipe files][t40], [Testing and CI][t41], [Generative pack contents][t42], [Build order][t44], [External tempo and MIDI in Perform][t51], [POC gaps][t52].
- Prototypes: [Density as a fourth macro][t24], [Mapping steps onto 5- and 6-note scales][t29], [Pushed Lean][t30], [Chord graph across a genre's scales][t31], [Perform control layout][t38], [Compose screen and Generative menu prototype][t50].

[map]: https://github.com/gismya/deluge-generative/issues/1
[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t2]: https://github.com/gismya/deluge-generative/issues/2
[t3]: https://github.com/gismya/deluge-generative/issues/3
[t4]: https://github.com/gismya/deluge-generative/issues/4
[t5]: https://github.com/gismya/deluge-generative/issues/5
[t6]: https://github.com/gismya/deluge-generative/issues/6
[t7]: https://github.com/gismya/deluge-generative/issues/7
[t8]: https://github.com/gismya/deluge-generative/issues/8
[t9]: https://github.com/gismya/deluge-generative/issues/9
[t10]: https://github.com/gismya/deluge-generative/issues/10
[t11]: https://github.com/gismya/deluge-generative/issues/11
[t12]: https://github.com/gismya/deluge-generative/issues/12
[t13]: https://github.com/gismya/deluge-generative/issues/13
[t14]: https://github.com/gismya/deluge-generative/issues/14
[t15]: https://github.com/gismya/deluge-generative/issues/15
[t16]: https://github.com/gismya/deluge-generative/issues/16
[t17]: https://github.com/gismya/deluge-generative/issues/17
[t18]: https://github.com/gismya/deluge-generative/issues/18
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t22]: https://github.com/gismya/deluge-generative/issues/22
[t23]: https://github.com/gismya/deluge-generative/issues/23
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t25]: https://github.com/gismya/deluge-generative/issues/25
[t26]: https://github.com/gismya/deluge-generative/issues/26
[t27]: https://github.com/gismya/deluge-generative/issues/27
[t28]: https://github.com/gismya/deluge-generative/issues/28
[t29]: https://github.com/gismya/deluge-generative/issues/29
[t30]: https://github.com/gismya/deluge-generative/issues/30
[t31]: https://github.com/gismya/deluge-generative/issues/31
[t32]: https://github.com/gismya/deluge-generative/issues/32
[t33]: https://github.com/gismya/deluge-generative/issues/33
[t34]: https://github.com/gismya/deluge-generative/issues/34
[t35]: https://github.com/gismya/deluge-generative/issues/35
[t36]: https://github.com/gismya/deluge-generative/issues/36
[t37]: https://github.com/gismya/deluge-generative/issues/37
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t39]: https://github.com/gismya/deluge-generative/issues/39
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t41]: https://github.com/gismya/deluge-generative/issues/41
[t42]: https://github.com/gismya/deluge-generative/issues/42
[t43]: https://github.com/gismya/deluge-generative/issues/43
[t44]: https://github.com/gismya/deluge-generative/issues/44
[t45]: https://github.com/gismya/deluge-generative/issues/45
[t50]: https://github.com/gismya/deluge-generative/issues/50
[t51]: https://github.com/gismya/deluge-generative/issues/51
[t52]: https://github.com/gismya/deluge-generative/issues/52
