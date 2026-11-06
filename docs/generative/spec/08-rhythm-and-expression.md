# 8. Rhythm and expression

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 8.1 Onsets and Density

- Each part's pattern material carries a **step importance** per step. Effective Density sets a threshold, with seeded jitter around it, and a step plays only if its importance clears the threshold. Patterns are never edited, only thinned or filled.
- At Density 0, only the most important hits play (Techno: the kick alone). The threshold curve and the jitter are *tuning*. The POC's Techno curve let nearly every step through by Density 40, which was too fast.
- Syncopation under Complexity picks pattern material and ghost masks aimed at a genre-set band, not a maximum.

### 8.2 Ornaments

All expression is plain note events ([§2.2](02-architecture.md#22-drivers-and-the-event-stream)). A part's flavour decides which steps play, at what pitch, velocity and length. One **expression stage**, shared by every role, then adds Ornaments and applies Lean to the notes that survived Density, using per-part and per-lane genre settings.

- **Ratchet.** One note becomes 2, 3 or 4 evenly spaced hits inside its 16th, every 12, 8 or 6 ticks. Each hit has the same pitch and lasts the spacing, or the original note if that's shorter. The first hit keeps the note's velocity, and each later hit is slightly softer. The genre lists which counts each lane or part may use. Pitched parts may ratchet if the genre allows it. A ratchet never spans more than one step.
- **Flam.** The main hit stays at the note's Lean position, and a soft grace hit plays a few ticks before it. Each part and lane has one **flam gap** in genre data, 2 to 6 ticks (*tuning*). The gap is in ticks, not ms, so it widens in ms as the tempo drops. When the grace hit has no room, the gap shrinks and the main hit stays put ([§8.5](#85-lean)).
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

- A part or lane has four **Lean** offsets in genre data, one for each 16th position in the beat: the 1, the e, the & and the a. A lane can move its & without moving its 1, so 8th hats can shuffle while the hat on the 1 stays with the kick. Per-part swing is Lean on the e and the a, added on top of song swing. With song swing at 0, a genre's leaning hats still lean.
- Lean is signed, **−12 to +12 ticks**, half a 16th either side of the grid. The Generator decides each step 12 ticks ahead so pushed hits are possible ([§12.2](12-runtime.md#122-when-steps-are-decided), ADR 0003).
- **First-step clamp.** Negative Lean clamps to 0 on the first step of every Section, in every driver. Compose clips stay self-contained, and downbeats such as the Drop land together.
- **How offsets add up.** Lean moves the whole note, Ornaments included. An event's total offset from its grid line is clamped to −12..+12. A ratchet's hits keep their spacing from the shifted start, and the spacing shrinks so the last hit still falls inside the step.
- **A flam at the edges.** The grace hit may not land earlier than −12, or earlier than the line on a Section's first step. When it would, it moves to that limit and the gap shrinks. The main hit never moves. With no gap left, the note plays plain, so a flammed hit on a Section's first step with Lean 0 is a single hit and downbeats still land together.
- No macro moves Lean, and the player can't set it. There's no humanize jitter: one tick is about 5 ms at 120 BPM, too coarse for jitter.

### 8.6 Slide

- Slide is a flavour decision, written as note overlap. A Loop part's sound is set to LEGATO with portamento on at load ([§10.6](10-sounds.md#106-load-time-settings)).
- In LEGATO, a note that overlaps the previous one changes pitch on the same voice and glides when portamento is on (`sound.cpp`, `voice.cpp`). A note after a gap starts fresh with no glide. That is the 303 slide, and it needs no firmware change.
- No pitch bends or trills in v1.

### 8.7 Fill

A **Fill** is a short change to the music that ends on a phrase line. It never covers a phrase's first step, so downbeats and Drops land intact.

**Fill kinds.** A fixed Generator-wide set of three. A genre's fill entries (slot 6) each give a kind, a lane mask, a length of 1, 2 or 4 beats, and a weight.

| Kind | What it does | What it may touch |
|---|---|---|
| **Rest** | the masked lanes and parts play nothing for its length. The kick drop | Drums lanes, and whole Bass, Chords, Pad or Lead parts |
| **Swap** | the masked lanes play a fill pattern from their pattern pool instead of the normal one. Snare fills, tom runs and Dub's return fill | Drums lanes |
| **Roll** | a short Build-style ratchet ramp on one lane ([§8.3](#83-the-build-roll)) | one Drums lane |

FX lanes are never touched, because section rules own them ([§5.4](05-parts-and-lanes.md#54-lanes)). A tom run is a Swap whose fill pattern is on the Tom lane.

**Where fills sit in a step.**
- A Swap replaces the lane's pattern source *before* Density. Fill patterns carry step importance like any pattern, so Density thins them, the expression stage adds Ornaments and Lean, and the lane's Density gate still applies. A Swap picks its fill pattern the same way normal patterns are picked.
- A Roll works like the Build roll: it ignores Density, forces ratchets upwards and climbs in velocity.
- A Rest is a mask applied after everything else, so no note is made and Compose writes none.
- Interplay rules read the normal planned pattern, never the fill ([§5.5](05-parts-and-lanes.md#55-interplay-rules)). Nothing reacts to a fill.
- **Precedence: Gap > Build roll > fill.** A Gap silences fills too. The roll lane keeps rolling under a fill, and the fill only changes the other lanes.

**Automatic fills.**
- At most one per phrase. The draw (whether a fill plays, and which entry) is made at the **phrase's first step**, keyed like every other draw ([§6](06-randomness.md)). The fill is placed so it ends on the phrase line.
- Automatic fills play only in **Groove phrases and the Intro's last phrase**. Not in Breaks, Builds or the Outro.
- The genre sets a **fill chance** at Motion 25. Motion scales it the same way it scales Groove length ([§9.3](09-arc.md#93-section-lengths)): 0 at Motion 0, double at Motion 50, capped at 100%.
- An optional **section-end chance** applies instead to a Groove's last phrase before a Break or Build. It defaults to the fill chance and is scaled the same way. A forced Section counts only if it was queued before the phrase's first step.
- An automatic fill counts as a **drum change** for Answers ([§5.2](05-parts-and-lanes.md#52-flavours)), since it's known a bar or more ahead.

**Held FILL.**
- FILL is momentary, like the Deluge FILL button. A held fill overrides any automatic fill. It is never a drum change for Answers, since it can't be foreseen.
- Fill state is read from `currentSong->isFillModeActive()`, so the MIDI-learnable FILL command works too.
- Open, settled by [Held FILL and how a Rest ends sounding notes][t79]: whether a held FILL plays one held-fill entry the genre names, or a seeded pick from the fill weights on each press, and whether a Rest cuts notes already sounding at its start or lets them ring. Either way the part comes back on the phrase line.

A **Phrase accent** follows a fill whenever any fill, automatic or held, covered the last step before a phrase line ([§9.5](09-arc.md#95-section-rules)).

Sources: [Sub-step expression][t20], [Pushed Lean][t30], [Lean per 16th position and the flam][t70], [Density as a fourth macro][t24], [POC gaps][t52], [Sound generation][t13], [Perform interaction model][t15], [Prior art in generative music engines][t6], [Perform control layout][t38], [Fill kinds and phrase-end events][t65], POC findings on the [map][poc].

[t6]: https://github.com/gismya/deluge-generative/issues/6
[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t13]: https://github.com/gismya/deluge-generative/issues/13
[t15]: https://github.com/gismya/deluge-generative/issues/15
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t30]: https://github.com/gismya/deluge-generative/issues/30
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t52]: https://github.com/gismya/deluge-generative/issues/52
[t65]: https://github.com/gismya/deluge-generative/issues/65
[t70]: https://github.com/gismya/deluge-generative/issues/70
[t79]: https://github.com/gismya/deluge-generative/issues/79
