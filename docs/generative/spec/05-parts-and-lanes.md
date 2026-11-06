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

- **Loop mutation.** At the end of a loop pass, steps re-roll with a probability that rises with Motion. PERF-1 uses (Mutate/100)², a fine starting curve (*tuning*). Mutation re-rolls only the pitch, octave, slide and accent of steps that aren't anchors. It never moves onsets, so the rhythm stays the pattern's ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)).
- **Answers** are read-time transforms of the same loop, played every Nth bar or loop, or in the bar before a drum change (an automatic fill, [§8.7](08-rhythm-and-expression.md#87-fill)). A Drop-out is never a drum change ([§8.8](08-rhythm-and-expression.md#88-drop-out)). PERF-1's set is octave, fifth, diatonic third, mirror, reverse, rotate, sparse and accents-only. Which a genre uses, and how often, is data. Motion raises the rate.
- **Complexity** picks each patterned part's Complexity tier and shapes Loop, Motif and Arp lines ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)).
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
- Lane kinds drive interplay rules and sample lookup ([§10.3](10-sounds.md#103-tags)).
- Drums and FX lanes are unpitched in v1. Key changes never retune them.

### 5.5 Interplay rules

Interplay rules (genre slot 6) shape a part's rhythm against another part. Every rule is a **filter**: it only removes the part's own onsets, never adds one and never changes a pitch. Each rule marks some steps as **against**, and the part's onsets on those steps are thinned:

| Rule | Against steps |
|---|---|
| Avoid X | X's onsets |
| Lock X | every step where X has no onset. With the `off` option, every step except X's onsets shifted one 8th later (+2 steps, wrapping inside the pattern), so a four-on-the-floor kick leaves the & steps free |
| Respond X | the busier half of each bar (below) |

- **Thinning.** On an against step, each non-anchor onset is dropped when a keyed draw falls below the rule's effective strength. The draw is keyed on part or lane, rule, take, the current pick and the step within the pattern, like the Density jitter ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)), so while a pattern holds its filtered shape loops exactly and a higher strength only ever removes more. With several rules, an onset plays only if it survives all of them. Rules compare 16th steps, not ticks, so Lean may still offset two locked notes slightly.
- **Lock can't add notes.** A part that should follow X's rhythm needs a pattern with onsets on X's steps. The usual way is a busy pattern that Lock carves down.
- **Respond's halves.** Each bar is two half-bars. The half where X has more onsets is against. When both halves have the same non-zero count, the first half is against, so the response comes after the call: a House Lead Respond Bass over an offbeat bass plays in each bar's second half. When both halves are empty, neither is against. The count for a half-bar is taken once, at its first step, from X's planned onsets at the Density of that moment, and holds for the whole half-bar.
- **Strength.** Each rule has a **strength**, 0–100% at the genre's default Complexity, and a signed **Complexity sensitivity** in percentage points over the whole 0–50 range. Effective strength = clamp(strength + sensitivity × (C − C_genre) / 50, 0, 100), in integer math, where C is the Recipe's Complexity (the Arc doesn't move it) and C_genre the playing genre's default. Examples: psy Bass Avoid Kick at 100% with sensitivity 0; House Lead Respond Bass at 60% with positive sensitivity; Techno hats Lock(off) Kick at 90% with sensitivity −40, so 80% at Complexity 50 when the default is 25.
- **What a rule reads.** A rule reads the target's **planned** onsets: its pattern after Density thinning (jitter included) and after the target's own interplay rules, before Ornaments, Lean, gates, mute, Break keeps, the Gap, fills, Drop-outs and Replace pairs. A muted or Break-silenced kick doesn't change the bass line, and a bass that avoids the kick fills the holes a low Density leaves in it.
- **Sources and targets.** A rule's source and its target are each a part or a Drums lane. A rule that targets the Drums part reads the union of its lanes.
- **Order.** Harmony is planned before any part. Parts are then generated in role order, **Drums → Bass → Chords → Pad → Lead → FX**, unless the genre overrides it (for example Bass after Chords when the bass locks to the stabs). A rule may only target a part earlier in the effective order, and a rule between two Drums lanes may only target a lane earlier in the fixed lane order ([§5.4](#54-lanes)). A harness spec rejects any genre that breaks this.

### 5.6 Gates

- Every part and every lane has an optional **Density gate**: a threshold, default 0. It plays only while effective Density (Recipe centre plus Arc offset) is at or above the threshold. Gates are checked only at bar lines, with a hysteresis of ±2 (*tuning*): a part comes in once Density reaches its threshold and leaves only when Density falls 2 below it, so a Build ramp or Drop decay never cuts a part mid-bar. Examples: Ambient drums at about 35, a shaker Perc lane at 30 (*tuning*).
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

Sources: [Track roles][t12], [Kit lane mapping][t25], [Generative pack contents][t42], [POC gaps][t52], [Density as a fourth macro][t24], [Generator scheduling][t19], [PERF-1 internals][t5], [Perform control layout][t38], [Fill kinds and phrase-end events][t65], [Pattern choice under Complexity, and protected hits][t66], [Interplay rule details and Register spans][t71].

[t5]: https://github.com/gismya/deluge-generative/issues/5
[t12]: https://github.com/gismya/deluge-generative/issues/12
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t25]: https://github.com/gismya/deluge-generative/issues/25
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t42]: https://github.com/gismya/deluge-generative/issues/42
[t52]: https://github.com/gismya/deluge-generative/issues/52
[t65]: https://github.com/gismya/deluge-generative/issues/65
[t66]: https://github.com/gismya/deluge-generative/issues/66
[t71]: https://github.com/gismya/deluge-generative/issues/71
