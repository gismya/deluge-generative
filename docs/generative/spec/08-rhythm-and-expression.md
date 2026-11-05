# 8. Rhythm and expression

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 8.1 Onsets and Density

- Each part's pattern material carries a **step importance** per step. Effective Density sets a threshold, with seeded jitter around it, and a step plays only if its importance clears the threshold. Patterns are never edited, only thinned or filled.
- At Density 0, only the most important hits play (Techno: the kick alone). The threshold curve and the jitter are *tuning*. The POC's Techno curve let nearly every step through by Density 40, which was too fast.
- Syncopation under Complexity picks pattern material and ghost masks aimed at a genre-set band, not a maximum.

### 8.2 Ornaments

All expression is plain note events ([§2.2](02-architecture.md#22-drivers-and-the-event-stream)). A part's flavour decides which steps play, at what pitch, velocity and length. One **expression stage**, shared by every role, then adds Ornaments and applies Lean to the notes that survived Density, using per-part and per-lane genre settings.

- **Ratchet.** One note becomes 2, 3 or 4 evenly spaced hits inside its 16th, every 12, 8 or 6 ticks. Each hit has the same pitch and lasts the spacing, or the original note if that's shorter. The first hit keeps the note's velocity, and each later hit is slightly softer. The genre lists which counts each lane or part may use. Pitched parts may ratchet if the genre allows it. A ratchet never spans more than one step.
- **Flam.** A soft grace hit at the note's Lean position, then the main hit 1–3 ticks later.
- **Rate.** Effective Complexity scales how likely Ornaments are, between per-lane floors and ceilings the genre sets. Density doesn't thin Ornament hits, since an Ornament decorates a note that already survived. Energy affects only their velocities.
- Ornament draws are keyed like every other draw, so takes re-roll them and freeze holds them.

### 8.3 The Build roll

The Build snare roll is a section rule that drives ratchets, not an Ornament of its own. Across a Build, a switched-on lane's ratchet count is forced upwards (plain hits, then 2, then 4), with velocity climbing towards the Gap, however low Complexity is. The roll rule decides which steps it plays on (every beat, then every 8th, then every 16th), not Density. Complexity only sets how early in the Build the roll starts. The roll lane and rate are section rules ([§9.5](09-arc.md#95-section-rules)).

### 8.4 Velocity

- Flavours set a note's base velocity (accents included). Energy's velocity layer applies on top, scaled by the genre's weight.
- **Velocity spread.** Each part and lane has a spread amount in genre data, default 0. It is a seeded nudge from the Generator's hash, applied after Energy's layer and before clamping. Ornament hits take their main note's spread.
- **Spread mode**, per part and lane, default **Repeat**. Repeat keys the draw on the step's place in the part's pattern, so the nudge repeats with the pattern. Fresh keys it on the absolute Arc step, so every pass differs. There's no player control in v1.
- Accents are mostly volume accents. Every factory synth preset routes velocity to volume, and only three Techno presets (000, 005, 015) also open the filter on velocity. Brightness belongs to the sound controls.

### 8.5 Lean

- A part or lane has two **Lean** offsets in genre data: one for on-beat 16ths and one for off-beat 16ths. Per-part swing is off-beat Lean, added on top of song swing. With song swing at 0, a genre's leaning hats still lean.
- Lean is signed, **−12 to +12 ticks**, half a 16th either side of the grid. The Generator decides each step 12 ticks ahead so pushed hits are possible ([§12.2](12-runtime.md#122-when-steps-are-decided), ADR 0003).
- **First-step clamp.** Negative Lean clamps to 0 on the first step of every Section, in every driver. Compose clips stay self-contained, and downbeats such as the Drop land together.
- **How offsets add up.** Lean moves the whole note, Ornaments included. An event's total offset from its grid line is clamped to −12..+12. A flam's main hit clamps to +12. A ratchet's hits keep their spacing from the shifted start, and the spacing shrinks so the last hit still falls inside the step.
- No macro moves Lean, and the player can't set it. There's no humanize jitter: one tick is about 5 ms at 120 BPM, too coarse for jitter.

### 8.6 Slide

- Slide is a flavour decision, written as note overlap. A Loop part's sound is set to LEGATO with portamento on at load ([§10.6](10-sounds.md#106-load-time-settings)).
- In LEGATO, a note that overlaps the previous one changes pitch on the same voice and glides when portamento is on (`sound.cpp`, `voice.cpp`). A note after a gap starts fresh with no glide. That is the 303 slide, and it needs no firmware change.
- No pitch bends or trills in v1.

### 8.7 Fill

- Fill is momentary, like the Deluge FILL button. While it is held the Generator plays the genre's fill kinds (genre slot 6) in place of the normal patterns.
- Motion also sets how often the Generator plays fills of its own, normally in the last bar of a phrase.
- Fill state is read from `currentSong->isFillModeActive()`, so the MIDI-learnable FILL command works too.

Sources: [Sub-step expression][t20], [Pushed Lean][t30], [Density as a fourth macro][t24], [POC gaps][t52], [Sound generation][t13], [Perform interaction model][t15], [Prior art in generative music engines][t6], [Perform control layout][t38], POC findings on the [map][poc].

[t6]: https://github.com/gismya/deluge-generative/issues/6
[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t13]: https://github.com/gismya/deluge-generative/issues/13
[t15]: https://github.com/gismya/deluge-generative/issues/15
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t30]: https://github.com/gismya/deluge-generative/issues/30
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t52]: https://github.com/gismya/deluge-generative/issues/52
