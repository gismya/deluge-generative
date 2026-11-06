# 7. Pitch

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 7.1 Steps inside, absolute notes out

- The Generator works in **scale steps** (step plus octave) against its own copy of the Recipe's key and scale. It resolves each note to an absolute MIDI note when it emits it. The event stream carries absolute notes.
- A key or scale change reaches every part on its next note, with nothing rewritten.
- The Generator never calls `Song::setScale` or `Song::setRootNote`. Both remap or re-infer from clip notes.

### 7.2 Scales

- Steps always sit on a 7-degree grid, which is every v1 scale unchanged ([§3.2](03-recipe.md#32-musical-facts)). No mapping onto smaller scales is needed.
- **In-scale only.** Every pitched note is in the Recipe's scale. A genre that wants a raised leading tone on V rolls Harmonic Minor. Chromatic colour is a later effort.

### 7.3 Register

- Each role has a **Register**: an anchor note plus a span (for example Bass anchor 36, span 0..+14). A genre may override both per part.
- A part's tonic is the instance of the key root's pitch class nearest its anchor, so a key change moves any part by at most 6 semitones.
- Notes outside the span, from inversions, interplay transforms or Degree-set snapping, fold back by octaves.
- Registers are fixed in v1. Energy doesn't move them (§23.4).

### 7.4 Chords

- Each genre has one degree-based **Chord graph**: home pull, up to about 8 weighted favourite moves, a **repeat weight** (default 0), whether A-B-A is allowed, a cadence target and a cadence period in chords. The Generator walks it, one keyed draw per chord, to pick each chord. The same graph is used unchanged in every scale.
- **An empty Harmony slot means the home chord only.** No walk runs and no Colour moves are added, but the home chord still gets its Extensions. Techno and Psytrance ship with it empty, so the psy pedal bass is never pulled by a chord move.
- **Repeat weight** makes staying on the current chord one candidate in the draw. A repeat counts as a chord for the cadence period.
- **Harmonic rhythm** is a genre value in bars per chord. Chord changes always land on a bar line. It must divide the phrase length or be a whole multiple of it (for example 16 bars over an 8-bar phrase), so a chord never straddles a phrase line. A harness spec rejects other values. Restarting the count at each phrase, which would allow any value, is a later option.
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
- **Walk rules.** The bonuses are Generator-wide values (*tuning*). Home bonuses shrink linearly as Complexity rises, from full at 0 to a quarter at 50, and may end up leaning harder towards home after tuning.
  - **Cadence.** The cadence period is a running count of chords that carries across phrases. Every Nth chord gives the cadence target +20. The count resets at every Section start, whatever chord lands there.
  - **Phrase rules** apply only when a phrase holds 2 or more chords. Its first chord gives home the **phrase home bonus**, and home is excluded on its last chord. A phrase with one chord, or a chord that spans phrases, just draws.
  - **Section starts.** The first chord of the piece (the Intro, or the Drop that a genre change or Recipe LOAD lands on) and the chord on every Drop are always home. Every other Section start gives home the **Section home bonus**, which is smaller than the phrase home bonus.
  - **Drop cadence.** The last chord before a Drop gives the cadence target the **Drop cadence bonus**, heavier than the regular cadence but still a weight, not a guarantee. It adds to the regular cadence when both fall on one chord.
  - Both cadences and both home bonuses do nothing when the Harmony slot is empty.
- **Extensions** give harmonic colour: in-scale tones added to the triad. v1 has four, tried in priority order **7, 9, sus4, 6**. There are no 11ths or 13ths; sus4 gives the 4th without clashing with a major 3rd.
- **Extension level** is how many a chord gets: 0 (plain triad), 1 or 2. It comes from Complexity through Generator-wide thresholds (*tuning*) and is clamped to the genre's **Extension floor** and **Extension ceiling** (defaults 0 and 2). It is deterministic, with no draw. House has floor 1, so its stabs always carry a 7th.
- **Extension guard.** An extension 1 semitone above any chord tone (mod 12) is rejected, and the next one in priority is tried. It catches ♭9 on Phrygian i, Minor ii and v, and Major iii and vii, and ♭6 over the 5th on Aeolian i. Like the chord guard, the rule is data with a Generator-wide default.
- **Voicing at 4 notes.** Chords and Pad play at most 4 notes.
  - sus4 replaces the 3rd.
  - A 7 or 9 that would make a 5th note drops the 5th.
  - 6 is only tried when the 7 was rejected, because a 6 under a ♭7 is a semitone cluster the guard can't see.

  So Dorian i at level 2 is R ♭3 ♭7 9 (m9), and Phrygian i at level 2 is R 4 5 ♭7 (the ♭9 is guarded, so it's i7sus4). Voice leading is flavour implementation.
- **Who hears Extensions.** Stab and Sustain voice the extended chord, and Arp and Motif walk its notes. Loop basses are offsets from the chord root and a pedal Sustain is root and fifth, so Extensions don't touch them.
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

At a resolved key or scale change, every held pitched note gets a note-off and the new notes start fresh. Tails ring out. When it happens is [§9.7](09-arc.md#97-transitions).

Sources: [Pitch model][t21], [Mapping steps onto 5- and 6-note scales][t29], [Chord graph across a genre's scales][t31], [Parts that play some scale degrees][t32], [Harmony for static genres and chord extensions][t64], [PERF-1 internals][t5].

[t5]: https://github.com/gismya/deluge-generative/issues/5
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t29]: https://github.com/gismya/deluge-generative/issues/29
[t31]: https://github.com/gismya/deluge-generative/issues/31
[t32]: https://github.com/gismya/deluge-generative/issues/32
[t64]: https://github.com/gismya/deluge-generative/issues/64
