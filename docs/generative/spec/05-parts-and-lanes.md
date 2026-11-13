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
| Arp | Lead | walks the current chord's notes on a rhythm. The walk position counts the pattern's onsets since the pass start, before thinning, so it keeps no state |
| Motif | Lead | a ring of slots storing chord notes by their position in the chord, not as pitches, so remembered phrases follow chord changes. Covers plucks and the Dub melodica |
| Stab | Chords | voiced chords on a rhythm pattern. House stabs and the Dub skank differ only in pattern |
| Sustain | Pad | held voicings that change with the chord. A **pedal** flag makes it a drone: root and fifth only, ignoring chord changes |

Drums and FX have no flavour.

- **Note lengths.** Loop, Arp and Motif play one note at a time, and each note overlaps at most the next onset's, as a slide does ([§2.7](02-architecture.md#27-buffer-sizes-and-overflow)). An Arp or Motif note ends by its next onset plus the slide overlap, and the Loop's limits on Lean and overlap ([§8.5](08-rhythm-and-expression.md#85-lean)) hold for it too. A Motif note is long only where its onsets are sparse.

- **Lines.** A Loop line and a Motif ring are both a **line**: per-step pitch content over the pattern's rhythm, kept as per-part state. It is rolled fresh at each pick ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)), a Complexity tier change included, and at each take change ([§6.6](06-randomness.md#66-takes)). After that it changes only by mutation. The two flavours share this mechanism and differ only in what a step stores (a scale offset, octave, accent and slide in a Loop; a chord position, octave and accent in a Motif) and in which Motion curve applies. Mutation draws are keyed on take, pick, the number of passes since the line was last rolled, and step. Freeze holds a line ([§6.7](06-randomness.md#67-freeze)).
- **Loop mutation.** At the end of a loop pass, each step that isn't an anchor re-rolls with a chance the genre sets at Motion 25, scaled by the mutation curve ([§3.5](03-recipe.md#35-motion-curves)). Motion sets only how many steps re-roll. A re-rolled step is drawn the way a fresh pick rolls pitch content, from the live Complexity: its leap range, octave-jump chance and slide chance ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)). There's no separate depth setting. PERF-1's curve gives 25% at the equivalent of Motion 25, which re-rolls three quarters of a line every pass at Motion 50, so the genre value is *tuning*. Mutation re-rolls only the pitch, octave, slide and accent of steps that aren't anchors. It never moves onsets, so the rhythm stays the pattern's ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)).
- **Answers** are read-time transforms of the same loop, played every N bars, or in the bar before a drum change (an automatic fill, [§8.7](08-rhythm-and-expression.md#87-fill)). A Drop-out is never a drum change ([§8.8](08-rhythm-and-expression.md#88-drop-out)). The bar before a fill is the bar before the one it starts in, when that bar is in the fill's phrase. Its Answer is drawn as any other, keyed on its bars since the pick. A frozen part answers there too: the fill is the Arc's, acting on top of the freeze like a section rule. Each Loop part lists its Answer transforms with weights, and N at Motion 25 (slot 5). An empty list means the part plays no Answers. Motion divides N by the Answers curve's multiple ([§3.5](03-recipe.md#35-motion-curves)), so Answers come about every 4N bars just above Motion 0, every N/2 at Motion 50, and never at Motion 0. Motion sets how often Answers come, and Complexity sets which transform plays.
  - **Where they fall.** An Answer lasts one bar. Counting bars from the pick's first bar, the last bar of every N answers: the bar where bars since the pick, plus one, is a multiple of N. N divided by the curve's multiple rounds to the nearest bar, and is at least 1. So with N = 4 at Motion 25, bars 4, 8, 12… of a pick answer, and a phrase of 8 bars ends on one.
  - **Transforms and tiers.** Each transform kind has a fixed Generator-wide **Answer tier**, from plain to intricate:

    | Tier | Transforms |
    |---|---|
    | 0 | sparse, accents-only (the call, thinned) |
    | 1 | octave, fifth (the same shape, moved) |
    | 2 | diatonic third, reverse |
    | 3 | mirror, rotate (the shape rebuilt) |

  - **Choosing one.** The part's band runs from the lowest to the highest tier in its list, with no separate band field. Effective Complexity maps onto the band the way it maps onto a pattern tier band ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)), with the remainder as a keyed chance of the next tier up. Within the tier, the list's weights pick. An empty tier falls back to the nearest lower one, so a part listing only octave and mirror plays octave up to about Complexity 25 and mirror above it.
  - **The draw.** There is one draw per Answer, at its first step, from the live Complexity. It is keyed on part, take, pick and bars since the pick, so a take you return to replays its Answers and a frozen part ([§6.7](06-randomness.md#67-freeze)) replays them too. A Complexity turn takes effect at the next Answer, never in the middle of one.
  - **Pitch shifts** work in scale steps and octaves, before snap and the Register fold ([§7.5](07-pitch.md#75-degree-sets)): octave moves each step's octave by 1, fifth by 4 scale steps (a diminished fifth on the 7th degree stands), and diatonic third by 2 scale steps. Up or down is a keyed even draw per Answer, and the size is always 1. A doubled size would gain nothing: two octaves fold back to the call, two fifths are a 9th, and two thirds are a fifth. Mirror reflects each step's pitch, counted in scale steps (offset plus octave), about the middle of the line's own range (`min + max − pitch`), with no extra shift. The range is taken over the line steps the pattern has onsets on, before Density thinning.
  - **Reverse, rotate and mirror reorder pitch only.** On each onset they play another step's pitch content (scale offset, octave, accent and slide). Reverse reorders the onsets: counting the pattern's onsets before Density thinning, the *k*-th of *n* plays the content of onset `n − 1 − k`. Counted before thinning, the pairing holds at every Density, so at full Density the call comes back backwards; at lower Density some onsets take content from onsets thinned out of the call. Rotate moves by steps: step *i* plays the content of step `i − 4`, wrapping (one beat late). The rhythm stays the pattern's, so anchors, interplay rules and Density thinning are untouched.
  - **Sparse and accents-only** are the only transforms that remove onsets: sparse silences odd steps of the pattern, and accents-only silences unaccented ones (a step is accented when its line step or its pattern marks it). Neither removes an anchor. Like a Drop-out, the removal is pure subtraction: interplay rules read the normal planned line, and nothing reacts to it.
  - **Narrow Registers.** On a −5..+12 span (Bass, Chords), an octave Answer moves only the notes whose pitch class fits twice in the span, which are the root and the 5th to the 7th ([§7.3](07-pitch.md#73-register)). The other notes fold back to where they were, so an octave-up Bass Answer sounds like its roots and fifths jumping up. This is the Register fold itself, with no rule of its own: a note already on the upper of its two places stays there on an octave-up Answer. A genre that wants a stronger Bass Answer lists other transforms.
- **Complexity** picks each patterned part's Complexity tier and shapes Loop, Motif and Arp lines ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)).
- **Motif re-roll** works like Loop mutation, with the Motif curve ([§3.5](03-recipe.md#35-motion-curves)): at the end of each pass, each slot that isn't an anchor re-rolls its chord position and octave from the live Complexity with the curve's chance. Otherwise it replays, which is PERF-1's "deja vu".
- **Sustain re-strikes** its whole voicing at every chord change. When the walk stays on the same chord, or the part is a pedal, the notes keep holding. A genre may give a Sustain part a **retrigger period** in bars (default none). The voicing then also re-strikes on every Nth bar line counted from the phrase line, picking up the current Energy velocity. N must divide the phrase or be a whole multiple of it. Density thins the voicing ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density)).
  - **As built** (#123). A Sustain decides on bar lines only. Each note it plays lasts to the next bar line, and on that line it either holds on (the same pitch, nothing re-striking) or ends there, so the table never holds more than the voicing and the next one's (§2.7). The whole voicing re-strikes at a chord change (the voiced chord's notes differ: a repeat or a Complexity turn that leaves them alike holds), a retrigger line, a Section line, a key change, and the bar line a mute or unmute lands on, so a muted Pad goes quiet there and an unmuted one comes back. A Rest, the Gap or a Build cut that ended its notes mid-bar brings the voicing back on the next bar line its keeps allow.
  - **Voice leading.** Each note goes to the instance of its pitch class inside the Pad's Register nearest any note of the voicing before (the tonic for the first), a tie going down. So a common tone stays where it was and every other note moves by the smallest step. The voicing before is the whole one, before Density thinning, so thinning never moves the next chord. A pedal has none: its root and the 5th above sit where the Register puts them.
  - **Places.** A note's importance comes from its place in the chord: the root (anchor), the 5th (8), the 3rd or the sus4 in its place (7), the first and second Extension (6 each) and the doubled root (4) (*tuning*). The jitter is keyed on part, take and place, never on the chord, so raising Density only ever adds notes. A part may double the root an octave up (down when up leaves the Register), only while the voicing has fewer than 4 notes.

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
- **Strength.** Each rule has a **strength**, 0–100% at the genre's default Complexity, and a signed **Complexity sensitivity** in percentage points over the whole 0–50 range. Effective strength = clamp(strength + sensitivity × (C − C_genre) / 50, 0, 100), in integer math, where C is the Recipe's Complexity (the Arc doesn't move it) and C_genre the playing genre's default. Examples: psy Bass Avoid Kick at 100% with sensitivity 0; House Lead Respond Bass at 60% with positive sensitivity; Techno hats Lock(off) Kick at 90% with sensitivity −40, so 70% at Complexity 50 when the default is 25.
- **What a rule reads.** A rule reads the target's **planned** onsets: its pattern after Density thinning (jitter included) and after the target's own interplay rules, before Ornaments, Lean, gates, mute, Break keeps, the Gap, fills, Drop-outs and Replace pairs. A muted or Break-silenced kick doesn't change the bass line, and a bass that avoids the kick fills the holes a low Density leaves in it.
- **Sources and targets.** A rule's source and its target are each a part or a Drums lane. A rule that targets the Drums part reads the union of its lanes.
- **Order.** Harmony is planned before any part. Parts are then generated in role order, **Drums → Bass → Chords → Pad → Lead → FX**, unless the genre overrides it (for example Bass after Chords when the bass locks to the stabs). A rule may only target a part earlier in the effective order, and a rule between two Drums lanes may only target a lane earlier in the fixed lane order ([§5.4](#54-lanes)). A harness spec rejects any genre that breaks this.

### 5.6 Gates

- Every part and every lane has an optional **Density gate**: a threshold, default 0. It plays only while effective Density (Recipe centre plus Arc offset) is at or above the threshold. Gates are checked only at bar lines, with a hysteresis of ±2 (*tuning*): a part comes in once Density reaches its threshold and leaves only when Density falls 2 below it, so a Build ramp or Drop decay never cuts a part mid-bar. Examples: Ambient drums at about 35, a shaker Perc lane at 30 (*tuning*).
- **Late entry.** When a gate opens at a bar line while the part or lane has a note that started earlier and lasts at least to the end of that bar, that note starts on the bar line for the rest of its length, with the velocity from the Energy at that moment. A shorter remainder is skipped. This matters for Sustain and for long Motif notes: an Ambient pad comes in on the first bar line its gate allows, not at its next chord. A Motif note ends by its next onset plus the slide overlap ([§5.2](#52-flavours)), so it is long only where its onsets are sparse. A Loop note lasts at most a step and its slide overlap ([§8.6](08-rhythm-and-expression.md#86-slide)), so it never enters late. When a gate closes, its sounding notes get a note-off on the bar line. The same rule re-strikes held notes at every Section line ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)).
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

Sources: [Track roles][t12], [Kit lane mapping][t25], [Generative pack contents][t42], [POC gaps][t52], [Density as a fourth macro][t24], [Generator scheduling][t19], [PERF-1 internals][t5], [Perform control layout][t38], [Fill kinds and phrase-end events][t65], [Pattern choice under Complexity, and protected hits][t66], [Interplay rule details and Register spans][t71], [Arc headroom, gestures per Section, and Motion][t72], [How Motion scales Groove length, fills and drop-outs][t81], [Takes and freeze for stateful flavours][t76], [Determinism details: tick unit, tempo, held notes and ramps][t77], [Should Complexity weight which Answer transform plays?][t82].

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
[t72]: https://github.com/gismya/deluge-generative/issues/72
[t76]: https://github.com/gismya/deluge-generative/issues/76
[t81]: https://github.com/gismya/deluge-generative/issues/81
[t77]: https://github.com/gismya/deluge-generative/issues/77
[t82]: https://github.com/gismya/deluge-generative/issues/82
