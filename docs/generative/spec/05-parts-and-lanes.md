# 5. Parts, roles, flavours and lanes

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

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
- **Answers** are read-time transforms of the same loop, played every Nth bar or loop, or in the bar before a drum change (an automatic fill, [§8.7](08-rhythm-and-expression.md#87-fill)). PERF-1's set is octave, fifth, diatonic third, mirror, reverse, rotate, sparse and accents-only. Which a genre uses, and how often, is data. Motion raises the rate.
- **Motif** slots replay with a probability (PERF-1's "deja vu"). Motion sets the re-roll rate.

### 5.3 Outputs

- Each part maps to exactly one Output for its lifetime. No part spans two Outputs, and no two parts share one.
- Drums map to one **Kit** with one row per lane. FX map to a Kit of their own. Bass, Chords, Pad and Lead map to one Synth each.
- Every Kit clip carries one NoteRow per lane, in the fixed lane order of [§5.4](#54-lanes), even when the lane is silent.

### 5.4 Lanes

**Lanes** are fixed kinds inside a Drums or FX part. The Generator addresses a lane by kind, never by row order.

| Part | Lanes, in fixed order |
|---|---|
| Drums | Kick, Snare, Clap, Rim, Closed hat, Open hat, Ride, Crash, Tom, Perc 1, Perc 2 |
| FX | Riser, Fall, Impact, Siren |

- A genre picks a subset. FX lanes are played only by section rules ([§9.5](09-arc.md#95-section-rules)), never by patterns.
- Lane kinds drive protected mutation (Kick and backbeat never move), interplay rules and sample lookup ([§10.3](10-sounds.md#103-tags)).
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
- Gates are separate from step-importance thinning inside a lane ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)).
- A part sounds only when the Arc, its gate and the player all allow it ([§13.4](13-perform.md#134-live-actions)).

### 5.7 Replace pairs

- Lanes of one part play independently, apart from **Replace pairs**: one-way rules "A replaces B". When both land on the same step, B's hit is dropped before any voice starts.
- Replace pairs are applied in the Generator, so every driver agrees.
- Genre data holds the list. The Generator-wide default is **Open hat replaces Closed hat**, and every v1 genre uses the default.
- The firmware's one CHOKE group per Kit still cuts the hat tails ([§10.6](10-sounds.md#106-load-time-settings)). It can't express a same-step rule, which is why Replace pairs exist.

### 5.8 Voice modes

Role defaults, which flavour and genre may override, applied when the sound loads ([§10.6](10-sounds.md#106-load-time-settings)):

| Role | Voice mode |
|---|---|
| Bass | LEGATO |
| Lead | LEGATO with Loop. POLY with Arp or Motif |
| Chords, Pad | POLY, at most 4 notes |
| Drums, FX | per lane. Open hat and Closed hat use CHOKE |

Sources: [Track roles][t12], [Kit lane mapping][t25], [Generative pack contents][t42], [POC gaps][t52], [Density as a fourth macro][t24], [Generator scheduling][t19], [PERF-1 internals][t5], [Perform control layout][t38], [Fill kinds and phrase-end events][t65].

[t5]: https://github.com/gismya/deluge-generative/issues/5
[t12]: https://github.com/gismya/deluge-generative/issues/12
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t25]: https://github.com/gismya/deluge-generative/issues/25
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t42]: https://github.com/gismya/deluge-generative/issues/42
[t52]: https://github.com/gismya/deluge-generative/issues/52
[t65]: https://github.com/gismya/deluge-generative/issues/65
