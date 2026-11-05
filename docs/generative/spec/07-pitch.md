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

At a resolved key or scale change, every held pitched note gets a note-off and the new notes start fresh. Tails ring out. When it happens is [§9.7](09-arc.md#97-transitions).

Sources: [Pitch model][t21], [Mapping steps onto 5- and 6-note scales][t29], [Chord graph across a genre's scales][t31], [Parts that play some scale degrees][t32], [PERF-1 internals][t5].

[t5]: https://github.com/gismya/deluge-generative/issues/5
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t29]: https://github.com/gismya/deluge-generative/issues/29
[t31]: https://github.com/gismya/deluge-generative/issues/31
[t32]: https://github.com/gismya/deluge-generative/issues/32
