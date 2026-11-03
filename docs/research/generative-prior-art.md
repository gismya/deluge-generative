# Prior art in generative music engines

Research for [#6](https://github.com/gismya/deluge-generative/issues/6), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1). Researched 2026-11-03.

**Question.** How do existing genre-aware generative systems model musical intent (energy, motion or variation over time, density, complexity)? How do they map it onto notes, drums, sounds and automation, and how do they build song-scale structure? Which approaches fit a small rule-based Generator on the Deluge (Cortex-A9, bare metal, no ML runtime)?

This is a survey. It does not design the Generator. PERF-1 gets only a light pass here, because [#5](https://github.com/gismya/deluge-generative/issues/5) covers it in depth.

Sources are primary unless marked. "Unverified" means I could only reach an abstract, a search snippet or a secondary write-up. Source-code line numbers refer to these pinned commits:

- PERF-1 `9a916c61eed7491d4e94d15b6d1e711f54e62fab`
- AMB-1 (`charlesvestal/fm1-amb`) `62a588ab102b5f1cf5e4aa72553694eaa7ddaaa5`
- Mutable Instruments eurorack `f4cfa7697095a28d292bff758beb51deb49b5a86`

## Contents

1. [Summary](#summary)
2. [Open-source firmware: the AMB-1, HYP-1, PERF-1 family](#1-open-source-firmware-the-amb-1-hyp-1-perf-1-family)
3. [Mutable Instruments Grids and Marbles](#2-mutable-instruments-grids-and-marbles)
4. [Hardware sequencers: Torso T-1, OXI One, Elektron](#3-hardware-sequencers)
5. [Other products, briefly](#4-other-products-briefly)
6. [What the Deluge already has](#5-what-the-deluge-already-has)
7. [Academic and algorithmic composition](#6-academic-and-algorithmic-composition)
8. [Adaptive game music](#7-adaptive-game-music)
9. [Cross-cutting patterns](#8-cross-cutting-patterns)
10. [Fit for an embedded rule-based Generator](#9-fit-for-an-embedded-rule-based-generator)
11. [Licences](#10-licences)
12. [Not covered or unverified](#11-not-covered-or-unverified)

## Summary

- **Intent knobs are almost never modelled directly.** Systems store a flat parameter row per piece or genre and put a few macro controls on top. Each macro is a fixed table of (target, coefficient) pairs or an interpolation between stored states. PERF-1's Mood, AMB-1's ARC, Octatrack scenes and FMOD automation all work this way. None of them uses a learned model.
- **Density means thresholding importance.** Grids stores a 0..255 importance level per step and fires when `level > ~density`. PERF-1 uses one fixed 16-step importance order. Marbles' STEPS knob does the same for scale degrees. Euclidean k/n is the evenly spread fallback.
- **Complexity is less settled.** Products express it as Euclidean layers combined with boolean ops, ratchets/repeats, lanes of different lengths, and voicing or harmony depth. The research measure is syncopation: a metric-weight table, with listener preference peaking at medium syncopation (Witek 2014).
- **Motion and variation over time come from four cheap mechanisms.**
  1. Seeded random sequences that loop or freeze (Marbles deja vu, T-1 random, OXI Stochastic at 0%).
  2. Counters on loop passes (Elektron A:B, 1ST and LST, T-1 Cycles).
  3. Slow shapes over 1 to 64 bars (T-1 Phrase and Groove, AMB-1 ARC).
  4. Protected mutation, where the kick and on-beat snare never move (PERF-1 drum drift).
- **Song structure is phrase-quantised everywhere it exists.** EDM phrases are 8 bars in over 98% of cases in the GESMI Breaks corpus. Parts enter or leave only at phrase boundaries, fills go in the last bar of a phrase, and a drop is bass plus full beat returning after a build. PERF-1 runs a groove, break, build, drop state machine with per-genre rows. GESMI generates section sequences from a tiny Markov table.
- **Fit.** Everything above except the ML and search-based systems is table lookups and O(steps) loops. It fits the Cortex-A9 easily. GrooVAE, MorpheuS's search and Vogl's RBM are context, not candidates.

## 1. Open-source firmware: the AMB-1, HYP-1, PERF-1 family

Lineage: PERF-1 is "a fork of HYP-1 by RedtailWorks, itself a fork of Charles Vestal's AMB-1" ([PERF-1 LICENSING.md L3](https://github.com/deamonSLINKY/PERF-1/blob/9a916c61eed7491d4e94d15b6d1e711f54e62fab/LICENSING.md)). AMB-1 is at [charlesvestal/fm1-amb](https://github.com/charlesvestal/fm1-amb). I found no public HYP-1 repo. PERF-1's engine header calls itself "HYP-1, hypnotic techno and acid for the FM-1 (a fork of AMB-1)" (`firmware/src/dsp/amb.h` L2), and HYP-1's model appears to survive as PERF-1's genre 0, "HYPNO". Anything about a standalone HYP-1 is unverified.

### AMB-1 (ambient, the root)

- **Recipe-like representation.** `PIECES[8]` holds 8 named pieces. Each piece is an `int8_t v[P_NPARAMS]` row with 28 parameters in five groups ([amb.c L37-63](https://github.com/charlesvestal/fm1-amb/blob/62a588ab102b5f1cf5e4aa72553694eaa7ddaaa5/app/dsp/amb.c#L37-L63); `amb.h` L49-59):
  - Layer levels: Pad, Bells, Bass, Pulse.
  - Motion: Density, Evolve, Drift, Hits.
  - Space: Echo, Echo time, Reverb, Size.
  - Tone: Bright, Swell, Bell tone, Motion.
  - Setup: Tempo, Root, Mode.
- **Harmony.** `chord_change()` (L621-646) is a weighted random walk over scale degrees with `W7 = {4,2,1,3,3,3,0.4}`. The current degree gets weight 0. Drift sets the time between chord changes, 20 s to 5 min, randomised ×0.7 to 1.3.
- **Melody.** Four "tape loops" have unequal lengths `{13,17,23,29}` quarter notes.
  - Each loop holds a note count target of `Density*0.05*(1+0.8*arc)`.
  - At each pass, `loop_pass()` grows or prunes the loop toward that target. It rests the loop with probability `evolve*0.15 + 0.15*max(-arc,0)`, and re-pitches or moves a note with probability `evolve*0.7` (L665-733).
- **Drums.** Three Euclidean lanes of 16, 12 and 7 steps use Bjorklund's algorithm, with `k = Hits*KMAX/100` and `KMAX={5,7,3}`.
  - Each bar, Evolve may rotate a lane or nudge its k by ±1 (`drums_evolve()`, L796-819).
  - Evolve can also rest the whole pulse layer for 2 to 4 bars (L884-918).
- **The ARC is a one-scalar energy envelope.** It is a slow random target in −1..+1, pushed away from the middle, renewed every 50 to 150 s and slewed with a ~35 s time constant. It is then scaled by Motion (`control()`, L1716-1731).
  - The ARC fans out with fixed coefficients to loop density, rest chance, pulse velocity (`clamp(1+1.3*arcm, 0.3, 1.15)`), pad brightness, FM index and reverb send.
- **No song structure.** There are no sections, only the ARC and the chord drift.
- **Cost.** The engine is about 1.8k lines of C. Pattern state is `int8_t loop_note[4][29]` plus three `uint32_t` drum masks.

### PERF-1 (light pass; see #5)

- **Parameters.** About 150 engine parameters ([amb.h L72-133](https://github.com/deamonSLINKY/PERF-1/blob/9a916c61eed7491d4e94d15b6d1e711f54e62fab/firmware/src/dsp/amb.h)). Genre is a bank selector, `EN_NGENRE 19`, with 10 pieces per genre.
- **Mood is the energy macro.** `MOOD[]` (`firmware/src/app/ui.c` L801-828) is a table of 15 (param, delta) pairs, for example `{P_AC_DENS,30}`, `{P_AC_CUT,25}`, `{P_REVERB,-15}`, `{P_EVOLVE,20}` and `{P_DRIVE,20}`.
  - The engine receives `v + d*(mood-50)/50`, clamped, where v is the piece's stored value. 50 is neutral.
  - The stored piece never changes. Mood is a linear one-to-many offset over a recipe.
- **Drum density by step importance.** `en_lane_mask()` (`amb.c` L2427-2453) uses a fixed ranking, `IMP[16] = {0,8,4,12,2,10,6,14,1,3,5,7,9,11,13,15}`.
  - Density below 50 removes the least important existing hits first.
  - Density above 50 adds hits on the most important empty steps, up to half of them.
  - The written pattern is never edited.
- **Pattern data.** Factory bitmask tables `EN_PIECE_PATS[pieces][6][6]` of `uint16_t` are generated offline by `tools/gen_pats.py`. Per-genre bitmask tables also cover bass rhythm, pad comping and ghost notes (`amb.c` L3079-3254).
- **Harmony per genre.** `CHT[EN_NGENRE]` (`amb.c` L1880-1900) is a sparse Markov chain. Every move has weight 1, plus up to 8 weighted genre "edges" (for example lofi ii→V weight 12).
  - Each genre also sets a tonic pull, a cadence target every N chords, an A-B-A flag, a harmonic rhythm in bars and a raised-7th flag.
- **Sections.** There are three, `EN_SEC_GROOVE`, `EN_SEC_BREAK` and `EN_SEC_BUILD`, and three commands, break, build and drop. All are quantised to the next bar (`section_cmd()`, `amb.c` L2535-2558). The drop lands on the tonic 70% of the time.
  - The per-genre `SECT[19]` row (L2370-2390) sets:
    - which parts the breakdown keeps;
    - build length (2, 4 or 8 bars);
    - pad rise;
    - whether the kick returns in the last build bar;
    - flags for roll, wobble, a gap on the last beat, and kick drop.
  - Fills are a 5×16 velocity table plus a per-genre allowed-fills mask (`FILLS`, `FILL_OK`, L2391-2408).
- **Auto-DJ** (`dj_bar()`, `ui.c` L1653-1756). It plays groove for 8, 16, 32 or 64 bars, or syncs to chord changes. Then it fills, breaks, builds and drops.
  - Break length is rounded so that break plus build ends on an 8-bar line (`dj_break_len`, L727-733).
  - It can bring in the next piece on the fill bar and ramp tempo across rounds.
- **Variation.** Several mechanisms run alongside each other:
  - Drum drift changes pattern every 4 to 64 bars, with a fill the bar before.
  - Mutate shifts hits in the snare, hat and perc lanes. The kick never moves and on-beat snare hits are protected.
  - The acid "answer" transforms its line every N loops (octave, fifth, mirror, reverse).
- **Cost.** The engine is about 7.7k lines. A piece packs into 376 bytes. It runs on a 2×240 MHz JieLi AC79 with 512 KB RAM ([README](https://github.com/deamonSLINKY/PERF-1)).

## 2. Mutable Instruments Grids and Marbles

### Grids ([source](https://github.com/pichenettes/eurorack/tree/f4cfa7697095a28d292bff758beb51deb49b5a86/grids), [manual](https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/manual/))

- **Intent parameters.** Six `uint8_t` values (`grids.cc` L282-293): X, Y, Chaos, and a density ("fill") per instrument for BD, SD and HH.
- **Representation.** `drum_map[5][5]` points to 25 nodes ([pattern_generator.cc L69-75](https://github.com/pichenettes/eurorack/blob/f4cfa7697095a28d292bff758beb51deb49b5a86/grids/pattern_generator.cc#L69-L75)). Each node is 96 bytes: 3 instruments × 32 steps of a 0..255 importance level, not on/off.
  - The source data is in `grids/resources/lookup_tables.py`.
  - Steps are 32nds (`kPulsesPerStep = 3` at 24 ppqn), so one node is one 4/4 bar. Almost all content is on even steps. The step size is inferred from the constants, not stated in the manual.
- **Interpolation.** `ReadDrumMap` (L78-95) picks the cell `x>>6, y>>6` and mixes the four corner nodes bilinearly with 8-bit crossfades. Any (X, Y) gives a blended level map, computed per step with no RAM buffer.
- **Density.** `EvaluateDrums` (L98-136) fires a trigger when `level > ~density`. Turning density up admits hits in order of importance.
  - The manual says: "The position of the added/removed notes depends on the rhythmic backbone specified by the X and Y parameters."
- **Accent.** A level above 192 sets the instrument's accent bit. The manual calls accents "the structurally important steps".
- **Chaos.** Once per pattern cycle, each part draws a random boost of `rand * (chaos>>2) >> 8`, which is 0..63, and adds it to every step's level (L100-106).
  - The effect is a random per-cycle density bump: weak ghost hits appear. Positions are never shuffled.
  - With swing enabled, the same knob becomes swing amount instead.
- **Euclidean mode** (`EvaluateEuclidean`, L139-172). It is a lookup in `lut_res_euclidean[(length-1)*32 + density]`, a table of precomputed Bjorklund bitmasks.
- **No structure.** Variation over time comes only from Chaos and CV.
- **Cost.** 2,400 B of pattern nodes plus a 4 KB Euclidean table, a few bytes of RAM, and 4 table reads and 3 mixes per instrument per step.

### Marbles ([source](https://github.com/pichenettes/eurorack/tree/f4cfa7697095a28d292bff758beb51deb49b5a86/marbles))

- **Deja vu** (`random_sequence.h` L40-41, L160-190). A 16-value loop buffer mutates with probability `p = (2*deja_vu - 1)^2`.
  - At deja vu ≤ 0.5, a mutation writes a fresh random value.
  - At 0.5 the loop repeats exactly.
  - Above 0.5, a mutation jumps to a random position in the loop.
  - The result is one knob that runs from random, through locked, to shuffled.
- **Spread and bias.** `BetaDistributionSample` interpolates a precomputed inverse-CDF table on a 5×9 grid (`distributions.h` L42-63). Spread sets the width and bias sets the skew.
- **Steps.** `Quantizer::Init` (`quantizer.cc` L40-89) builds 7 nested scale subsets from per-degree weights with thresholds `{0,16,32,64,128,192,255}`. Turning Steps up restricts pitch to progressively more important degrees, ending at root and fifth.
  - This is Grids' importance threshold applied to pitch.
- **t-generator.** Its models are complementary Bernoulli, clusters, a small drum table, independent Bernoulli, divider, three-states and Markov (`t_generator.h` L44-53). Bias acts as probability and jitter as timing randomness.

## 3. Hardware sequencers

### Torso T-1 ([user guide, OS 2.1](https://docs.torsoelectronics.com/t1/introduction))

Each of the 16 tracks has Shape, Groove and Tonal sections. CTRL gives each knob a second parameter.

- **Density.** Pulses is a Euclidean count over Steps, and Rotate shifts it ([pulses-rotate](https://docs.torsoelectronics.com/t1/parameter-reference/shape/pulses-rotate/)).
  - Probability (0 to 100%) applies to every note, or to pulses only when turned counter-clockwise ([velocity-probability](https://docs.torsoelectronics.com/t1/parameter-reference/groove/velocity-probability/)).
- **Complexity.** Repeats adds 0 to 48 extra triggers per pulse, with a velocity Ramp. Time sets their spacing and Pace accelerates or decelerates them ([repeats-ramp](https://docs.torsoelectronics.com/t1/parameter-reference/shape/repeats-ramp/), [time-pace](https://docs.torsoelectronics.com/t1/parameter-reference/shape/time-pace/)).
  - Voicing and Style move chord shapes ([voicing-style](https://docs.torsoelectronics.com/t1/parameter-reference/shape/voicing-style/)).
  - Harmony shifts one chord tone at a time and is hysteretic: its result depends on earlier changes.
- **Motion.** Range plus Phrase move pitch through the note pool on a shape lasting 1 to 64 bars. The shapes are Cadence 1-4, Saw, Triangle, Sine, Pulse or CV ([range-phrase](https://docs.torsoelectronics.com/t1/parameter-reference/shape/range-phrase/)).
  - Accent plus Groove move velocity on a template (Agogo, Congas and others) over 1 to 64 bars ([accent-groove](https://docs.torsoelectronics.com/t1/parameter-reference/groove/accent-groove/)).
- **Random.** Each assigned parameter gets a looping 16-step random sequence. Random sets the depth, Rate sets the speed, and a gesture reseeds it ([random-rate](https://docs.torsoelectronics.com/t1/parameter-reference/setup/random-rate/)).
  - A table says which parameters re-randomise per cycle and which at the random rate ([random behavior](https://docs.torsoelectronics.com/t1/appendix/random-behavior/)).
- **Cycles.** Up to 16 stored versions of a track are played on successive loop passes ([cycles](https://docs.torsoelectronics.com/t1/core-concepts/cycles/)).
  - The guide's suggested arc: minimal, then add pulses, then add movement, then simplify for release. "Patterns define structure… Cycles define development inside that structure."
- **Structure.** 16 patterns per bank, with quantised launch and chaining ([banks-patterns](https://docs.torsoelectronics.com/t1/tracks-patterns-banks/banks-patterns/)). There is no arranger or fill condition. The T-1 drives notes only; it does not modulate sound parameters.

### OXI One MKII ([manual, fw 2.2.10](https://docs.oxiinstruments.com/oxi-one-mkii-manual/))

- **Step conditions** ([sequencer basics](https://docs.oxiinstruments.com/oxi-one-mkii-manual/sequencer-basics/)):
  - Trig %, A:B (and NOT), PRE, 1st.
  - Flow, a step that plays only while held, like a fill.
  - Fixed, which makes a step immune to random and Euclidean generators.
  - Ratchets, plus repeat ramp and acceleration.
- **Generators** ([creating patterns](https://docs.oxiinstruments.com/oxi-one-mkii-manual/creating-patterns/)):
  - A two-layer Euclidean generator combines its layers with AND, XOR or Accent, and adds Density, note-randomisation range and probability, octave range, tie % and note order.
  - A Random generator has Bias, Range and Density. Its sub-zero density modes (Human, Order, Pitch, Flip) transform existing notes instead of adding new ones.
- **Multitrack drum generator** ([multitrack generators](https://docs.oxiinstruments.com/oxi-one-mkii-manual/multitrack-generators/)). Kick, snare and hat engines take X/Y ("define the source model"), Chaos and Density per track over 32 steps. It works like Grids, and is the closest commercial analogue to a genre map × density × chaos design.
- **Stochastic mode** ([stochastic mode](https://docs.oxiinstruments.com/oxi-one-mkii-manual/stochastic-mode/)). Each of 12 scale-note columns has a play probability.
  - Random Pitch and Random Rhythm (0 to 100%) set the share of steps regenerated each cycle. At 0% the phrase is frozen.
- **Matriceal mode** ([matriceal mode](https://docs.oxiinstruments.com/oxi-one-mkii-manual/matriceal-mode/)). Ten lanes per track (trigger, note, interval, velocity, octave, gate, probability and others), each with its own length and direction.
  - "The length of the parameter lanes is what brings the surprise." An Interval lane drifts pitch on every cycle.
- **Modulation** ([modulation](https://docs.oxiinstruments.com/oxi-one-mkii-manual/modulation/)). Eight mod lanes per sequencer, up to 128 steps each, with random amount and slew. Two LFOs per sequencer can target trig probability, velocity, pitch or gate, and lanes can also drive MIDI CC.
- **Arranger** ([arranger](https://docs.oxiinstruments.com/oxi-one-mkii-manual/arranger/)). An 8×8 grid of lanes and slots. Each slot has a repeat count and a Next rule: next, previous, first, random, or random-other.

### Elektron

- **Trig conditions.** Digitakt II manual §10.7.3, p.48-49 ([PDF](https://www.elektron.se/wp-content/uploads/2026/10/Digitakt-2-User-Manual_ENG_OS1.17_260930.pdf)):
  - PRE / NOT PRE follows or inverts the last evaluated condition on the same track.
  - NEI / NOT NEI does the same for the neighbour track.
  - 1ST and LST fire on the first loop, or the last loop before a pattern change.
  - A:B fires on loop A of every B. For example, 2:4 plays on loops 2, 6, 10 and so on.
  - PROB is re-rolled every time a trig is set to play.
  - FILL is a separate on/off flag that combines with COND (§11.2, p.53-54).
  - The manual's own framing is a "grid of mutually exclusive or conclusive note trigs" with probability locks to make minimal patterns "come alive with randomness". A PRE chain gives correlated randomness: one dice roll decides a whole phrase variant.
- **Fill mode** (§10.8.4, p.49). It arms fill for exactly one pattern cycle, starting at the next loop.
- **Euclidean mode** (§10.3, p.44-45). Two generators with their own rotations, combined by OR, XOR, AND or SUB. Euclidean trigs can still be parameter-locked or converted to normal trigs. The Digitone II has the same ([PDF](https://www.elektron.se/wp-content/uploads/2026/10/Digitone-2-User-Manual_ENG_OS1.12_261008.pdf), §10.6).
- **Locks** (§10.8.1-2). Per-step parameter locks (up to 80 parameters per pattern) and preset locks. Lock trigs change sound without playing a note.
- **Song mode** (§10.9, p.50). Up to 99 rows, each with a label (Verse, Chorus, Fill), pattern, play count, length, tempo and mutes.
- **Octatrack scenes** ([MKII manual §10.3, p.53-54](https://www.elektron.se/wp-content/uploads/2026/08/Octatrack-MKII-User-Manual_ENG_OS1.40C_260827.pdf)). Each scene is a set of locked values. The crossfader interpolates between scene A and scene B, and a parameter locked in only one scene interpolates against the base value.
  - This is the clearest primary-source model of one control moving many parameters: two target vectors and a linear blend.
- **Analog Rytm scenes and Performance mode** ([MKI manual §10.3-10.4, p.27-29](https://www.elektron.se/wp-content/uploads/2026/10/Analog-Rytm-User-Manual_ENG_OS1.74_260930.pdf)). Scenes are instant lock sets. Performance pads sweep lock ranges by pressure. The two modes share a budget of 48 locks per kit.

## 4. Other products, briefly

- **Ableton Live 12 MIDI Tools** ([manual](https://www.ableton.com/en/live-manual/12/midi-tools/)):
  - Rhythm: steps, density, split chance, accent frequency.
  - Seed: pitch, duration and velocity ranges plus density.
  - Shape: a drawn contour with density and jitter.
  - Stacks: scale-aware chord progressions.
  - Euclidean.
  - Transformations: Connect, Ornament, Recombine, and others.
  - Per-note Chance, Probability Groups ("Play All" or "Play One") and velocity deviation ([editing MIDI §10.5.12-13](https://www.ableton.com/en/live-manual/12/editing-midi/)).
- **Polyend Play** ([manual](https://polyend.com/manuals/play/)):
  - Per-step Chance paired with an Action that randomises sample, note, octave, cutoff, repeat or humanise.
  - A Fill tool with "128 pre-programmed rhythms in different musical genres", Euclidean and Random density.
  - Punch-in perform FX.
- **Novation Circuit Tracks** ([guide](https://userguides.novationmusic.com/hc/en-gb/articles/25494476280850-Circuit-Tracks-hardware-overview)). Per-step probability, Mutate, pattern chains and scenes.
- **Teenage Engineering OP-Z.** Step components (pulse, multiply, random velocity, ramp, jump; [guide](https://teenage.engineering/guides/op-z/step-components)) and per-track polymeter.
  - Punch-in effects (ramp up, fills, random) are recorded on a performance track, so performance turns into editable automation ([guide](https://teenage.engineering/guides/op-z/punch-in-effects)).
- **Koan / Noatikl / Wotja (Intermorphic)** ([Noatikl NME 2 docs](https://intermorphic.com/archive/tiklapp/engine/nme/2/)). A commercial rule-based generator built the Recipe → Generator way.
  - Four weight tables per voice: Scale (12 weights), Harmony, Next Note, and Rhythm (9 durations). Each weight runs 0 to 100%, and "an element with a value of 0 will not be chosen."
  - Voice types: Rhythmic, Ambient, Follows, Repeat Bar, Fixed Pattern.
  - Mutation Factor and Note Rest % ("useful for making any Voice sound sparser").
  - Per-parameter envelopes of up to 100 points across the piece.

## 5. What the Deluge already has

From the [community features docs](https://delugecommunity.com/features/community_features/) (the repo's `docs/community_features.md` now points there) and [arpeggiator docs](https://delugecommunity.com/features/arpeggiator/):

- Per-note probability and iteration. Since c1.3, probability, iterance and fill are independent (§4.3.11). Custom iterance has a divisor of 1 to 8 with a toggle per iteration, so arbitrary A-of-B masks are possible.
- Fill / Not Fill conditions (§4.3.4), row-level probability (§4.3.6), and Fill and Once clips (§4.1.3).
- Euclidean rows and per-row play direction.
- A sequence randomizer (§4.3.9) and an advanced arp with random walk, ratchet, chord and other probabilities.
- Random Walk and Sample & Hold LFO shapes (§4.3.1), Performance View FX columns (§4.1.6), Song Macros (§4.1.9), and scales and chord memory (§4.4).
- I found no PRE, NEI, 1ST or LST conditions, no scene morph, and no note generator driven by intent parameters. That is "not found", not "proven absent". [#7](https://github.com/gismya/deluge-generative/issues/7) covers reuse in detail.

Rendered output from Compose can carry these features as data (probability, iterance, fill flags, row length) instead of baking every variation into notes. Whether it should is a design question for later tickets.

## 6. Academic and algorithmic composition

### Rhythm density, complexity, syncopation

- **Toussaint 2005.** [The Euclidean Algorithm Generates Traditional Musical Rhythms](https://cgm.cs.mcgill.ca/~godfried/publications/banff.pdf) (Bridges, pp. 47-56).
  - Bjorklund's algorithm spreads k onsets over n steps as evenly as possible. E(3,8) is the tresillo, and rotations give style variants.
  - Cost is O(n) with no data. Even spacing is the opposite of syncopation, so it needs a complexity measure alongside.
- **Longuet-Higgins & Lee 1984** (Music Perception 2(4); unverified, from abstracts and restatements). Each metrical position gets a weight, falling at each lower level. A syncopation is a note on a weak position followed by a rest on a stronger one, scored by the weight difference.
  - Cost is a 16-entry table and a linear scan.
- **Witek et al. 2014.** [Syncopation, Body-Movement and Pleasure in Groove Music](https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0094446) (PLoS ONE 9(4)).
  - It extends Longuet-Higgins & Lee to polyphonic drums with instrument weights.
  - The finding is "an inverted U-shaped relationship" between syncopation and both wanting to move and pleasure. Medium syncopation rates best.
  - So a complexity parameter should target a syncopation band, not ramp without limit.
- **Sioros, Holzapfel & Guedes 2012** (ISMIR, abstract only). Syncopation used to drive real-time generation. Measure it per instrument layer, not on the summed mix.
- **Vogl & Knees 2017.** [An Intelligent Drum Machine for EDM](https://nime.org/proceedings/2017/nime2017_paper0047.pdf) (NIME).
  - A 4×16 grid feeds an RBM with 500 hidden units, about 32k weights, trained on 2,752 one-bar patterns. Gibbs sampling steps away from a seed give "variation amount".
  - Its training filter is itself a usable density rule: 2 to 6 kicks, 1 to 5 snares and at least 2 hi-hats per bar.
  - Inference would fit on an A9 (estimate: about 32 KB as int8 weights), but it needs offline training.
- **GrooveToolbox.** [Bruford et al., ISMIR 2020](https://github.com/fredbru/GrooveToolbox) is a set of Python rhythm features (density, syncopation, complexity, swing). A good reference for formulas to port.
- **GrooVAE** ([Gillick et al. 2019](https://arxiv.org/abs/1905.06118)) is an LSTM VAE trained on 13.6 h of drumming. ML adds micro-timing, velocity feel and infilling. It does not address intent or structure. A rule system gets partway with per-groove offset and velocity tables, which is what OXI and T-1 groove templates are.
- **MTG rhythm spaces.** The MTG group built 2D drum maps with interpolation and generators with density and syncopation controls ([MTG](https://mtg.upf.edu/node/3865); details unverified).

### Tension and energy

- **Farbood 2012.** [A Parametric, Temporal Model of Musical Tension](https://bpb-us-e1.wpmucdn.com/wp.nyu.edu/dist/f/11865/files/2020/08/Farbood_2012_Musical_Tension.pdf) (Music Perception 29(4)).
  - Perceived tension is a weighted sum of feature *slopes* over an attentional window of about 3 s.
  - The weights are 3 for dynamics and melodic pitch height, 2 for tempo and onset frequency, and 1 for harmony and bass and inner-voice pitch.
  - A momentum factor β = 5 rewards trends that continue in the same direction.
  - Run in reverse, it says: to build, raise loudness and lead register first, then add onsets, then harmonic change, and keep the direction steady. That is an EDM riser.
- **Herremans & Chew, MorpheuS** ([arXiv 1812.04832](https://arxiv.org/abs/1812.04832)).
  - Three spiral-array tension measures: cloud diameter, cloud momentum and tensile strain. Generation matches a target tension profile while keeping repeated patterns as constraints.
  - The search takes minutes per piece, so it is not usable live. The three measures are cheap (3D point maths over 12 pitch classes) and could score chord or bass candidates greedily.
- **Lerdahl & Krumhansl 2007** (abstract only) needs a prolongational tree. It is too heavy to run; at most a source for chord-distance tables.

### EDM structure

- **Eigenfeldt & Pasquier, GESMI.** [Evolving Structures for Electronic Dance Music](http://www.sfu.ca/~eigenfel/Eigenfeldt%20GECCO2013.pdf) (GECCO 2013). The most reusable source for song form.
  - The corpus is 100 hand-transcribed tracks (Breaks, House, Dubstep, Drum and Bass). "Less than 2% of the 621 phrases in the Breaks corpus are something other than 8 bars", and phrases are mostly 2-bar patterns.
  - There are five sections: A lead-in, B intro, C verse ("all instruments are present"), D breakdown ("the beat may drop out, or a filter may remove all mid- and high-frequencies. It will tend to build tension") and E outro.
  - The representation is a form-state matrix: 8-bar phrases × 16 part types, each cell on/off plus a pattern number. "Parts enter or drop out at the start of any phrase."
  - Section order comes from a first-order Markov table run backwards from E. For Breaks, D is preceded by C 0.31 and D 0.57.
  - Candidates are filtered so the lead-in is about 10% of the length.
  - Part on/off comes from a GA scored on corpus mean ± SD: parts per section, parts entering or leaving per boundary, and paired-part rules. Constructive rules could replace the GA.
- **Eigenfeldt & Pasquier.** [Considering Vertical and Horizontal Context in Corpus-based Generative EDM](http://www.sfu.ca/~eigenfel/Eigenfeldt%20ICCC%2013.pdf) (ICCC 2013).
  - Generation order: form, drums, aux percussion conditioned on drums, bass conditioned on drums, synths conditioned on bass, fills, one-hits.
  - Drums are first-order chains over a 4-bit (kick, snare, closed hat, open hat) nibble per 16th, with tables per beat position and section. "Not found it necessary to move beyond first-order."
  - Bass is steered by intent targets: density, straightness, dryness and jaggedness. Candidates are ranked by distance to the targets, then picked at random from the top 20%. This is a cheap way to steer Markov output with intent knobs.
  - Fills fall "at the end of eight measure phrases". Shuffling a one-bar pattern after seven repeats "will produce a perfectly acceptable fill".
- **GEDMAS** ([Anderson, Eigenfeldt & Pasquier 2013](https://ojs.aaai.org/index.php/AIIDE/article/view/12649), abstract). Markov models for form, chords, melody and rhythm, rendering a 16-part piece into Ableton Live. The same idea as Compose.
- **Yadati et al. 2014.** [Detecting Drops in EDM](https://archives.ismir.net/ismir2014/paper/000297.pdf) (ISMIR). A drop "occurs after a build, a building up of tension, and is followed by the re-introduction of the full bassline". The build shows as rising spectral content. Analysis only.
- **Vande Veire & De Bie 2018.** [An automated DJ system for Drum and Bass](https://link.springer.com/article/10.1186/s13636-018-0134-8) (EURASIP JASMP).
  - "Phrases are a multiple of 8 measures long, and musical segments are an integer number of phrases long."
  - Segments are labelled high or low energy by RMS. This two-level energy model is directly usable.

### Mapping one high-level parameter onto many features

- **Wallis et al. 2011.** [A rule-based generative music system controlled by desired valence and arousal](https://zenodo.org/records/849861/files/smc_2011_004.pdf) (SMC). Fully rule-based, no corpus.
  - Arousal drives rhythmic roughness (9 joins down to 0), tempo (60 to 100 BPM), articulation, velocity (50 to 70) and voicing size (2 to 8 notes).
  - Valence drives mode (six modes from Phrygian to Lydian), extensions, register and voice-leading step.
  - The density algorithm is simple. It starts from 16 equal notes per bar, and "as roughness increases, pairs of notes are selected at random and joined."
  - With tempo fixed, "the rhythmic roughness parameter alone was sufficient" for arousal. Intended versus perceived arousal correlated at r = 0.679, valence at r = 0.475.
- **AffectMachine-Pop** ([arXiv 2506.08200](https://arxiv.org/html/2506.08200v1)). A hand-tuned expert system.
  - Tempo is logarithmic in arousal, 36 to 130 BPM, and velocity is `60 + 15·arousal`.
  - Patterns switch at arousal thresholds, and layers are added or removed at thresholds.
  - Valence drives a probabilistic chord graph.
  - Sections are 8 bars.
- **Livingstone et al. 2010, CMERS** (Computer Music Journal 34(1); abstract only). Changing valence needs score changes such as mode. Arousal can be moved with performance features alone (density, tempo, loudness).

### Classic cheap rule systems

- **Markov chains.** Eigenfeldt's results are evidence that first-order tables are enough for EDM drums once they are conditioned on beat position and section. PERF-1's `CHT` is a sparse first-order chord chain.
- **Grammars.**
  - Steedman's jazz chord grammar ([Music Perception 1984](https://homepages.inf.ed.ac.uk/steedman/papers/music/40285282.pdf); rules not read) expands a 12-bar blues with a handful of rewrite rules. Recursion depth works as a harmonic-complexity knob.
  - Quick & Hudak's probabilistic temporal graph grammars ([Kulitta PTGG](https://hackage.haskell.org/package/Kulitta/docs/Kulitta-PTGG.html)) rewrite timed chord symbols with probabilistic rules. `Let`/`Var` express "same 8 bars again" as shared subtrees, giving phrase repetition and variation in a few hundred lines.

## 7. Adaptive game music

- **FMOD Studio** ([glossary 2.03](https://www.fmod.com/docs/2.03/studio/glossary.html)).
  - Automation maps "the possible values of a parameter to specific values of properties", with a curve per target.
  - Transition regions jump to markers. Quantization limits triggers to "certain beats or bars".
  - Probability trigger conditions are "checked once each time the playback position reaches a quantization point".
- **Wwise** ([Wwise-201 course](https://www.audiokinetic.com/en/courses/wwise201/); [Audiokinetic blog](https://www.audiokinetic.com/en/community/blog/about-dynamic-music-design-part-1-design-classification/)).
  - Michael Sweet's split into "Vertical Remixing (Layering) and Horizontal Re-Sequencing".
  - Discrete remixing switches sub-tracks; continuous remixing has game parameters drive track volume.
  - Transitions sync to the next bar, beat or cue, with transition segments and stingers.
- **Spore** ([Kent Jolly, PdCon 2011](https://www.uni-weimar.de/kunst-und-gestaltung/wiki/images/Usage_of_Pd_in_Spore_and_Darkspore.pdf)).
  - Pure Data was used "only for its event logic", not DSP, with game parameters fed into patches. `play` objects pick samples "random, norepeat, or inorder".
  - Seeded random drum loops repeating every 8 beats come from a press write-up of the GDC 2008 talk and are unverified.
  - The relevant lesson is a seeded RNG reseeded per section: generated, but repeatable.

The vertical/horizontal split maps onto energy (which layers play, and how loud) and structure (which section comes next, quantised to a bar or phrase).

## 8. Cross-cutting patterns

| Intent | Mechanisms found | Where |
|---|---|---|
| Energy | Linear offset table over a stored recipe; slow scalar fanned out with coefficients; A/B scene interpolation; layer count; velocity base and accent; filter, drive and reverb send; Farbood ordering (loudness and register before onsets before harmony) | PERF-1 Mood, AMB-1 ARC, Octatrack, FMOD/Wwise, Wallis, AffectMachine, Farbood |
| Density | Importance level per step against a threshold; fixed metric importance order; Euclidean k; probability floor; roughness joins; layer on/off | Grids, PERF-1 `IMP`, Marbles Steps, T-1, Elektron, Wallis, GESMI |
| Complexity | Syncopation score against a target band (medium); two Euclidean layers with boolean ops; ratchets and repeats; lanes of different lengths; harmony and voicing depth; grammar recursion depth; Markov steered by feature targets | Witek, L-H&L, Elektron, OXI, T-1, Steedman, Eigenfeldt |
| Motion and variation | Looping or freezable seeded random; loop-pass counters (A:B, 1ST, LST, Cycles); correlated dice (PRE/NEI); 1-64 bar shapes on pitch and velocity; protected mutation; evolve probabilities per bar or pass | Marbles, T-1, OXI Stochastic, Elektron, AMB-1, PERF-1 |
| Structure | 8-bar phrase grid; section Markov table (A-E); part on/off matrix per phrase; per-genre section rows (what the break keeps, build length, kick return); fill in the last bar of the phrase; drop = bass and kick back on a boundary; bar-quantised transitions | GESMI, PERF-1 `SECT`/auto-DJ, Vande Veire, Yadati, FMOD/Wwise, Elektron song mode, OXI arranger |
| Genre | Bank of recipe rows; per-genre tables for patterns, chord graph, section behaviour and allowed fills; X/Y position on a pattern map | PERF-1, AMB-1, Grids, OXI drum generator |

Two things stand out. First, every system keeps the stored recipe or pattern intact and applies intent as a non-destructive transform at play time (PERF-1 Mood and density, Grids threshold, Octatrack scenes). Second, every product that randomises gives a way to freeze or reseed (T-1, Marbles, OXI Stochastic at 0%, Deluge arp Lock). That is relevant to the map's open question on seeding and reproducibility.

## 9. Fit for an embedded rule-based Generator

The target is a Cortex-A9, bare metal, C++, no ML runtime, sharing the CPU with the Deluge's audio engine. #3 measures the actual budget.

**Fits well.** Each of these is a table lookup or an O(steps) loop, needs no training, and has shipped on far smaller chips:

- Recipe rows plus macro offset tables (PERF-1 Mood, about 15 pairs) and slow-scalar fan-out (AMB-1 ARC). They run on a 240 MHz AC79.
- Importance-level pattern maps with density threshold, X/Y interpolation and per-cycle chaos (Grids). They run on an 8-bit AVR from 2.4 KB of data.
- A fixed metric importance order (PERF-1 `IMP[16]`) and nested scale-degree subsets (Marbles Steps).
- Euclidean generation, Bjorklund or a precomputed 4 KB table, including two-layer boolean combination.
- First-order Markov tables for chords (PERF-1 `CHT`), drums per beat position and section (Eigenfeldt), and section order (GESMI A-E).
- Candidate scoring against feature targets: density, syncopation via a metric-weight table, straightness. Pick from the top fraction (Eigenfeldt bass) and cap syncopation at medium (Witek).
- An 8-bar phrase form-state matrix plus a section state machine with per-genre rows (GESMI, PERF-1 `SECT` and auto-DJ). This maps onto Compose's rendered arrangement and onto Perform's bar-quantised transitions.
- Loop-pass counters, PRE-style correlated dice, freezable seeded random loops (Marbles deja vu) and protected mutation.
- Wallis/AffectMachine-style linear or threshold maps from one intent value to tempo, velocity, articulation, voicing size and layer count.
- Farbood's slope weighting, used offline as a design rule for builds rather than as a runtime model.
- Weight-table voices with rest % and per-parameter envelopes over time (Noatikl).
- Probabilistic grammars (PTGG-style) for phrase-level repetition, if the harmony needs more than a Markov chain. This is the heaviest item here at a few hundred lines, but it is still cheap.

**Fits with care:**

- GESMI's GA for part on/off is cheap in absolute terms, but it is search, and its output is hard to steer live. Constructive rules from the same fitness terms are simpler and deterministic.
- Spiral-array tension measures are cheap to evaluate, but only greedy use makes sense. MorpheuS-style search is too slow for Perform, and only plausible in Compose if a render can take seconds.
- An RBM pattern-variation model (Vogl & Knees) would run at about 32k weights. It needs offline training, a weights blob and a training corpus, which goes against "small rule-based".

**Does not fit:**

- GrooVAE and other LSTM or transformer models: large weights, float-heavy inference, an ML runtime, and corpus licensing.
- MorpheuS's variable neighbourhood search in Perform.
- Lerdahl's tonal tension, which needs prolongational analysis.
- Corpus-trained Markov tables as a runtime dependency are fine, but building them needs a transcribed corpus. Hand-authored tables (PERF-1, Noatikl) avoid that.

## 10. Licences

The Deluge community firmware is GPL-3.0.

| Project | Licence | Port code? |
|---|---|---|
| PERF-1 | GPL-3.0-only ([LICENSE](https://github.com/deamonSLINKY/PERF-1)) | Yes |
| AMB-1 | MPL-2.0 since 0.9; 0.9 and earlier GPL-3.0-only (`LICENSING.md` L1-10) | Yes |
| Grids (AVR code) | GPL-3.0 ([eurorack README](https://github.com/pichenettes/eurorack) L36) | Yes |
| Marbles (STM32 code) | MIT (eurorack README L38) | Yes |
| GESMI and the other papers | Ideas and published tables only | Not applicable |

PERF-1's generator ideas are credited to Tuna by delirioux (GPL-3.0), "written afresh: no code taken". Whether "GPL-3.0-only" sources combine cleanly with the firmware's exact GPL variant is a question for #5 or a later ticket. I did not check the firmware's licence header wording.

## 11. Not covered or unverified

- HYP-1 has no public source. Everything about it is inferred from PERF-1.
- Not researched: Ornament & Crime / Hemisphere, Teletype, Tuna ([source](https://github.com/delirioux/tuna)), Elias, Eno/Chilvers Bloom (no technical primary source found), Nierhaus's book, Polyend Tracker, and Arturia's randomizers.
- Abstract only: Longuet-Higgins & Lee, Lerdahl & Krumhansl, CMERS, Collins 2009, Williams 2015, the MTG density and syncopation generators, and PAD/SAD.
- I did not read the Analog Rytm MKII manual (too large to fetch). The MKI manual describes the same scene and performance features.
- The Deluge feature list comes from the community docs site, not the code. #7 should confirm it against `v1.3.0-rc3`.
