# 8. Rhythm and expression

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 8.1 Onsets and Density

**Patterns.**
- A **pattern** is one entry in a genre's pool (slot 6), for one Drums lane or one Bass, Chords or Lead part. It is 16 or 32 steps (1 or 2 bars) long. Each step holds a **step importance** (0 means never) and a velocity level: ghost, normal or accent. Ghost notes are low-importance ghost steps, so they come in as Density rises. There are no separate ghost masks.
- Each Drums lane picks from its own pool, independently of the other lanes. A genre that wants lanes to agree (Dub's one-drop kick and snare) gives each lane one entry per tier.
- Patterns are never edited, only thinned. Drum patterns never mutate step by step. They change only by a new pick.

**Anchors.**
- An **anchor** is a step a pattern marks as always playing (the top importance value). Anchors are part of the pattern, not a per-lane mask, because Dub's backbeat is step 8 and House's are 4 and 12.
- Density thinning, the jitter, Loop mutation and interplay rules never remove an anchor. Only part and lane gates, Break keeps, the Gap, fills (Rest and Swap), Drop-outs, Replace pairs and mute do. `genreValid()` rejects a genre that anchors a step an Avoid rule targets: for each Avoid rule, no anchor in any of the source's pool patterns may fall on a step where any of the target's pool patterns has an onset, at any tier, comparing patterns of different lengths by their step in the bar.
- At Density 0 exactly the anchors play (Acid techno: the kick and the Drone's root).

**Density thinning.**
- Effective Density sets an importance threshold. A non-anchor step plays only if its importance plus a seeded jitter clears it. The Density the notes read is latched on each bar line (#451): the effective Density there, the Arc's ramp included, and each part's Density bias ([§3.3](03-recipe.md#33-macros)). So an Intro, a Build, a Drop's decay or an Outro thins parts in and out only on bar lines, and a live Density turn or bias reaches the notes at the next bar line. Everything that decides from Density reads the latched value: thinning, a Swap's fill pattern, the planned onsets interplay rules read (Respond's half-bar counts among them), Sustain voicings and gates (both read only on bar lines anyway). Energy and the sound controls' macro layer, Density's included, still move every step. Within a bar the jitter can still split hits of one importance tier, so a lane may come in on its luckiest hit a bar or two before the rest: that is the pattern's thinning, not a Density change. The threshold curve and the jitter amount are *tuning*: one Generator-wide value each, which a genre may override. The POC's Techno curve let nearly every step through by Density 40, which was too fast.
- The jitter is keyed on part, lane, take, the current pick and the step within the pattern. While a pattern holds, its thinned shape loops exactly. Each step's jittered importance is fixed, so raising Density only ever adds hits and never swaps one for another.

**Complexity tiers.**
- Every pool entry has a hand-tagged **Complexity tier**, 0 (plain) to 3 (busiest). Each part and lane has a tier band in genre data (lowest and highest tier). Effective Complexity maps linearly onto the band in integer math, and the remainder is a keyed chance of the next tier up, so a slow turn shifts lanes one at a time. Within a tier the pick is weighted and keyed. An empty tier falls back to the nearest lower one.
- The tier is checked each time a pattern wraps (every 1 or 2 bars), with the same keyed draws, so a Complexity turn is heard within 2 bars. If the tier doesn't change, neither does the pick.
- **Authoring rule.** In every pattern of tier 1 or above, at least one hit off the beat (an e, & or a) ranks in the top half of that pattern's importances, so low Density keeps the pattern's defining syncopation. Anchors always play, so they are left out of the ranking, an off-beat anchor meets the rule by itself, and the hit must play at the genre's default Density whatever its jitter. A harness spec checks it. Ornaments show Complexity at any Density ([§8.2](#82-ornaments)).

**When patterns change.**
- **Every Drop** opens a fresh pick for every part and lane, keyed on the Groove's index. The Intro and the first Groove share the first pick. Breaks and Builds keep what was playing.
- **At each phrase line in a Groove**, each Drums lane and each other patterned part re-picks on its own, with a genre chance at Motion 25 that the re-pick curve scales ([§3.5](03-recipe.md#35-motion-curves)), 0 at Motion 0 (*tuning*). A Groove that starts without a Drop (forced from a Break or the Outro) has this chance on its first step too, keyed on its occurrence and phrase 0 as a Drop would be. The first Groove keeps the Intro's pick.
- The draw for *whether* a change happens leaves out the take, and the pick of *which* pattern includes it. A new take gives a new pattern at the next phrase line, and stepping back replays exactly the pick that take would have now, with its Loop or Motif line rolled fresh ([§6.6](06-randomness.md#66-takes)).

**Complexity in each flavour** ([§5.2](05-parts-and-lanes.md#52-flavours)).
- **Loop** and **Motif**: onsets come from the part's tiered pattern, and Density thins them. Pitch content, the part's **line**, is rolled onto that rhythm at each pick and then mutates pass by pass ([§5.2](05-parts-and-lanes.md#52-flavours)). Complexity sets how far a step may leap (scale steps in a Loop, chord positions in a Motif), the chance of an octave jump and, in a Loop, the chance of a slide (*tuning*). The genre gives each as a value at Complexity 0 and one at 50, linear between. The leap range is measured from the chord root: it is the largest offset a step may draw, not the interval from the step before, so each step's draw stays independent of its neighbours and mutation can re-roll one step alone. Complexity never adds or removes notes.
- **Arp**: a pool entry is a tiered pattern plus an order and an octave span of 1 or 2 octaves. The orders are a fixed Generator-wide set: Up, Down, Up-down, a keyed Random, and Pulse, the chord's root and its octave in turn whatever its other notes (#531; its entry spans 2 octaves). The walk position is the count of the pattern's onsets since the pass start, before Density thinning, and the Random order draws once per onset. Thinning removes notes without shifting the rest, and the Arp keeps no state across passes ([§6.6](06-randomness.md#66-takes)).
- **Stab**: tiered rhythm patterns, like drums.
- **Sustain** has no pattern. Complexity reaches it only through Extensions ([§7.4](07-pitch.md#74-chords)). Density thins its voicing instead. Each voicing note has an importance: the root is an anchor (and the first Extension, where a genre says so, [§5.2](05-parts-and-lanes.md#52-flavours)), then the fifth, the third and the Extensions, and lowest of all an optional octave doubling of the root (a per-part flag). The usual threshold applies, and a pedal is just root and fifth. The voicing is read again at bar lines with the gate hysteresis: a note that comes in starts on the bar line by late entry ([§5.6](05-parts-and-lanes.md#56-gates)), and a note thinned out gets a note-off there. During an Intro the voices come in one by one, so a pad swells louder without level automation ([§11.8](11-sound-controls.md#118-what-the-generator-never-automates)).

### 8.2 Ornaments

All expression is plain note events ([§2.2](02-architecture.md#22-drivers-and-the-event-stream)). A part's flavour decides which steps play, at what pitch, velocity and length. One **expression stage**, shared by every role, then adds Ornaments and applies Lean to the notes that survived Density, using per-part and per-lane genre settings.

- **Ratchet.** One note becomes 2, 3 or 4 evenly spaced hits inside its 16th, every 12, 8 or 6 ticks. Each hit has the same pitch and lasts the spacing, or the original note if that's shorter. The first hit keeps the note's velocity, and each later hit is slightly softer. The genre lists which counts each lane or part may use. Pitched parts may ratchet if the genre allows it. A ratchet never spans more than one step.
- **Flam.** The main hit stays at the note's Lean position, and a soft grace hit plays a few ticks before it. Each part and lane has one **flam gap** in genre data, 2 to 6 ticks (*tuning*). The gap is in ticks, not ms, so it widens in ms as the tempo drops. When the grace hit has no room, the gap shrinks and the main hit stays put ([§8.5](#85-lean)).
- **Rate.** Effective Complexity scales how likely Ornaments are, between per-lane floors and ceilings the genre sets. Density doesn't thin Ornament hits, since an Ornament decorates a note that already survived. Energy affects only their velocities.
- Ornament draws are keyed like every other draw, so takes re-roll them and freeze holds them.

### 8.3 The Build roll

The Build snare roll is a section rule that drives ratchets, not an Ornament of its own. It climbs through **stages**: a hit every beat, then every 8th, then every 16th, then 32nds (ratchet 2 in each 16th), then 64ths (ratchet 4). The genre sets the **top stage**, 16ths, 32nds or 64ths, and 32nds by default. The roll stops climbing there.

- Each stage after the first starts when a genre-set fraction of the Build is left before the Drop: by default 1/2, 1/4, 1/8 and 1/16 (*tuning*). In a 16-bar Build with the default top stage, the roll plays 8ths from bar 9, 16ths from bar 13 and 32nds from bar 15, with a Gap or without. Build lengths are powers of two, so every stage change lands on the grid, and the Gap only cuts off the roll's end.
- The roll rule decides which steps it plays on, not Density, and it plays however low Complexity is. Neither the roll lane's Density gate nor its part's ever stops it. Velocity climbs towards the Gap.
- Complexity only sets how early in the Build the roll starts: on the Build's first step at Complexity 50, and with a quarter of the time before the Drop left at Complexity 0, linear between (*tuning*). It starts no later than its last grid step before the Gap, so a short Build with a Gap still rolls. A roll that starts late joins at the stage already reached, so a Build's last bars sound the same at any Complexity.
- There are no triplet rolls: ratchets stay inside one 16th, and a triplet roll would need hits off the 16th grid (see [`docs/future-features.md`](../../future-features.md)).
- The roll lane, top stage and stage fractions are section rules ([§9.5](09-arc.md#95-section-rules)).

### 8.4 Velocity

- Flavours set a note's base velocity (accents included). Energy's velocity layer applies on top, scaled by the genre's weight.
- **Velocity spread.** Each part and lane has a spread amount in genre data, default 0. It is a seeded nudge from the Generator's hash, applied after Energy's layer and before clamping. Ornament hits take their main note's spread.
- **Spread mode**, per part and lane, default **Repeat**. Repeat keys the draw on the step's place in the part's pattern, so the nudge repeats with the pattern. Fresh keys it on the absolute Arc step, so every pass differs. There's no player control in v1.
- Accents are mostly volume accents. Every factory synth preset routes velocity to volume, and only two Techno presets (000, 005) also open the filter on velocity. Brightness belongs to the sound controls.

### 8.5 Lean

- A part or lane has four **Lean** offsets in genre data, one for each 16th position in the beat: the 1, the e, the & and the a. A lane can move its & without moving its 1, so 8th hats can shuffle while the hat on the 1 stays with the kick. Per-part swing is Lean on the e and the a, added on top of song swing. With song swing at 0, a genre's leaning hats still lean.
- Lean is signed, **−12 to +12 ticks**, half a 16th either side of the grid. The Generator decides each step 12 ticks ahead so pushed hits are possible ([§12.2](12-runtime.md#122-when-steps-are-decided), ADR 0003).
- **First-step clamp.** Negative Lean clamps to 0 on the first step of every Section, in every driver. Compose clips stay self-contained, and downbeats such as the Drop land together. It clamps a part's Lean to 0 on the phrase line a new sound ([§10.7](10-sounds.md#107-when-sounds-load)) or a key change ([§7.6](07-pitch.md#76-key-changes)) lands on for it too, since that line ends its notes: its notes there start on the line, on the new sound and in the new key, and nothing ties across the cut.
- **How offsets add up.** Lean moves the whole note, Ornaments included. One exception: a Stab, Arp or Motif note that leans late still ends on the bar line, its Lean coming off its length ([§5.2](05-parts-and-lanes.md#52-flavours)). An event's total offset from its grid line is clamped to −12..+12. A ratchet's hits keep their spacing from the shifted start, and the spacing shrinks so the last hit still falls inside the step.
- **A flam at the edges.** The grace hit may not land earlier than −12, or earlier than the line where the first-step clamp holds. When it would, it moves to that limit and the gap shrinks. The main hit never moves for its grace hit. With no gap left, the note plays plain, so a flammed hit on a Section's first step with Lean 0 is a single hit and downbeats still land together.
- **Hits on one key never cross.** Neighbouring 16ths can lean into each other (an & 12 ticks late and the a after it 12 early land on the same tick), and a ratchet fills its step to the end. So a hit never starts before, or on the same tick as, a hit already started on its part's key (lane or pitch): it starts one tick after that hit instead. This is the one exception to "the main hit never moves": a flam's main hit moves only when an earlier hit on its key lands at or after its Lean position, and its grace hit has the same floor, so the gap shrinks or the note plays plain. Only Lean data that crosses 16ths, or a ratchet followed by an early hit, reaches it.
- **Lean on a Loop part.** A Bass or Lead note may overlap only the next onset's ([§2.7](02-architecture.md#27-buffer-sizes-and-overflow)), so a slid note, which lasts its step plus the slide overlap from where it starts ([§8.6](#86-slide)), must end by the time the onset two 16ths later starts. It starts at its 16th's Lean, moved later in three cases: to the line when the 1 leans early (the first-step clamp), to the line on every 16th when the part may ratchet (the last hit of a ratchet just before may start a tick before the line, and a ratchet's last hit ends before the onset two 16ths on starts, which a `static_assert` holds), and to a tick after the 16th before when that one leans past it (hits on one key never cross). The onset two 16ths later starts at its Lean, its flam gap earlier when the part may flam, never earlier than −12. With no Lean the overlap may reach a whole step; with the 1 leaned +12 it may reach 12 ticks. A part that never slides has no limit on its Lean. A harness spec rejects a genre that breaks it.
- No macro moves Lean, and the player can't set it. There's no humanize jitter: one tick is about 5 ms at 120 BPM, too coarse for jitter.

### 8.6 Slide

- Slide is a flavour decision, written as note overlap. A Loop part's sound is set to LEGATO with portamento on at load ([§10.6](10-sounds.md#106-load-time-settings)).
- In LEGATO, a note that overlaps the previous one changes pitch on the same voice and glides when portamento is on (`sound.cpp`, `voice.cpp`). A note after a gap starts fresh with no glide. That is the 303 slide, and it needs no firmware change.
- A slid step's note lasts its step plus a few ticks of overlap (genre data, *tuning*), so it reaches into the next step's note only when that step plays. A plain note lasts at most a step, and the overlap ends by the time the onset after the next starts, Lean included ([§8.5](#85-lean)), so a Bass or Lead part has at most two notes sounding ([§2.7](02-architecture.md#27-buffer-sizes-and-overflow)). A harness spec rejects a genre whose gate or overlap reaches further. A slide onto the same note ties ([§7.5](07-pitch.md#75-degree-sets)): the stream has no second note-on, and the first note lasts to the end of the second. An Ornament on the second note re-strikes it instead. A plain note may tie the same way on a part's tie chance ([§5.2](05-parts-and-lanes.md#52-flavours)), its note ending on the next onset.
- No pitch bends or trills in v1.

### 8.7 Fill

A **Fill** is a short change to the music that ends on a phrase line. It never covers a phrase's first step, so downbeats and Drops land intact.

**Fill kinds.** A fixed Generator-wide set of three. A genre's fill entries (slot 6) each give a kind, a lane mask, a length of 1, 2 or 4 beats, and a weight.

| Kind | What it does | What it may touch |
|---|---|---|
| **Rest** | the masked lanes and parts play nothing for its length. The kick drop | Drums lanes, and whole Bass, Chords, Pad or Lead parts |
| **Swap** | the masked lanes play a fill pattern from their fill pool instead of the normal one. Snare fills, tom runs and Dub's return fill | Drums lanes |
| **Roll** | a short Build-style ratchet ramp on one lane ([§8.3](#83-the-build-roll)) | one Drums lane |

FX lanes are never touched, because section rules own them ([§5.4](05-parts-and-lanes.md#54-lanes)). A tom run is a Swap whose fill pattern is on the Tom lane.

A lane's fill patterns are a list of their own beside its pool, its **fill pool**, tiered and weighted like the pool's entries and read with the lane's tier band. Only a Swap plays them. A lane may have a fill pool and no pool (Techno's Tom).


**Where fills sit in a step.**
- A Swap replaces the lane's pattern source *before* Density. Fill patterns carry step importance like any pattern, so Density thins them, the expression stage adds Ornaments and Lean, and the lane's Density gate still applies. A Swap picks its fill pattern the same way normal patterns are picked, at the live Complexity, keyed on the lane's current pick: while a pick holds, every Swap on the lane plays the same fill pattern, and a new pick or take changes it. The lane's own interplay rules don't filter a fill pattern.
- A Roll works like the Build roll and shares its code: it ignores Density thinning and Complexity, plays no Lean, spread or Ornament, forces ratchets upwards and climbs in velocity. Unlike the Build roll, it obeys its lane's and part's Density gates, mute and keeps, as a Swap does, so a Roll on a lane that isn't playing is silent and nothing that plans ahead (the Arc plan, the MUTE pad's dim) has to count it. It plays 16ths from its first step and the genre's top roll stage ([§8.3](#83-the-build-roll)) for its last quarter, so a 4-beat Roll plays 16ths for 3 beats and then 32nds, and a 1-beat Roll 16ths for 3 steps and 32nds on its last; velocity climbs from normal to accent across it. A held Roll (SHIFT + FILL's fresh pick can land on one) plays 16ths for its first beat, then the top stage until release, reaching accent at the end of that beat; its beat counts from the step FILL takes effect, every step held counting, a phrase's first step it pauses on and the Gap's steps included. A new instance's start (Start, or a transition's start step) clears fill state, and the driver sends a FILL still held again, so a held Roll then starts its 16ths and its climb again: the new piece's Roll starts with it. On the Build roll's own lane the Build roll wins. Its hits are the lane's own, so a Replace pair replaces them as it does a Swap's ([§5.7](05-parts-and-lanes.md#57-replace-pairs-and-alternate-lanes)).
- A Rest is a mask applied after everything else, so no note is made and Compose writes none. A rested lane replaces nothing, like a lane a section rule silences ([§5.7](05-parts-and-lanes.md#57-replace-pairs-and-alternate-lanes)).
- Interplay rules read the normal planned pattern, never the fill ([§5.5](05-parts-and-lanes.md#55-interplay-rules)). Nothing reacts to a fill.
- **Precedence: Gap > Build roll > fill.** A Gap silences fills too, and a fill doesn't cover a step in the Gap. The roll lane keeps rolling under a fill, and the fill only changes the other lanes. A Drop-out beats a fill on any lane or part they share ([§8.8](#88-drop-out)).

**Automatic fills.**
- At most one per phrase. The draw (whether a fill plays, and which entry) is made at the **phrase's first step**, keyed like every other draw ([§6](06-randomness.md)) on the Section's kind and occurrence and the phrase in it, never on a take. The fill is placed so it ends on the phrase line. It plays only on steps still in a Groove or the Intro, so before a Build shorter than a phrase, which takes the phrase's end, it isn't heard.
- Automatic fills play only in **Groove phrases and the Intro's last phrase**. Not in Breaks, Builds or the Outro.
- The genre sets a **fill chance** at Motion 25. Motion scales it along the fill curve ([§3.5](03-recipe.md#35-motion-curves)): 0 at Motion 0, 5% of the Motion 25 chance just above 0, 2.5 times it at Motion 50, clamped at 100%.
- An optional **section-end chance** applies instead to a Groove's last phrase before a Break or Build. It defaults to the fill chance and is scaled the same way. A forced Section counts only if it was queued before the phrase's first step.
  - The phrase is the Groove's last when, at its first step, a forced Break or Build waits in the pending list, or, with no force or End waiting, the Groove ends within the phrase by Motion as it is then, with no hold ([§9.3](09-arc.md#93-section-lengths)). A force queued during the phrase lands at its end, after the draw, so it doesn't count; only a short Build's force can wait across a phrase line.
- An automatic fill counts as a **drum change** for Answers ([§5.2](05-parts-and-lanes.md#52-flavours)), since it's known a bar or more ahead.

**Held FILL.**
- FILL is momentary, like the Deluge FILL button. A held fill overrides any automatic fill. It is never a drum change for Answers, since it can't be foreseen.
- Fill state is read from `currentSong->isFillModeActive()`, so the MIDI-learnable FILL command works too. The Perform driver sends each change to the core through the steering queue ([§2.6](02-architecture.md#26-steering-into-the-core)). A MIDI FILL has no SHIFT latched, so it plays the held-fill entry. A FILL held while stopped plays from the next piece's start.
- A held fill plays in any Section, with the same precedence as an automatic one.
- **What it plays.** FILL plays the genre's **held-fill entry**, one of its fill entries named in slot 6, so a plain press is always the same move. SHIFT + FILL plays a **fresh pick** instead: a weighted draw from the genre's fill entries, keyed like every other draw ([§6](06-randomness.md)) at the step the press takes effect. Perform latches whether SHIFT was held when FILL went down, and logs it with the Fill for Capture.
- **How long.** A held fill starts on the next undecided step and lasts while FILL is held. The entry's length is ignored. On release, the parts come back on the next undecided step. A held fill still never covers a phrase's first step: across a phrase line it pauses for that step and carries on from the next.
- A genre with no fill entries (Ambient) has no held-fill entry, and FILL does nothing.

**Rests and notes already sounding.** When a Rest starts, the Generator sends note-offs through its note-off table ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)) for the notes still sounding on the Rest's pitched parts (Bass, Chords, Pad, Lead). Their release plays, so the drop is clean but not clipped. Drum lanes are left alone: a kit sample under 2 s is a one-shot that ignores note-off, so it plays out, and the hat choke works as usual. The rule is the same for automatic and held Rests.
- A held Rest that spans a phrase line starts again after the line's first step, so it sends note-offs again, and a pitched note started on that first step is cut one step later.
- Compose shortens a note that runs into a Rest so that it ends where the Rest starts, which keeps Compose and Capture matching Perform.
- This differs from the mute gate, where sounding notes finish their length. Muting is a mixer action, and a Rest is part of the music.
- The prototype didn't settle note-off versus ring, or coming back at once versus on the phrase line. These rules are the recommendation, to be checked on the Deluge during building (*tuning*).

A **Phrase accent** follows a fill whenever any fill, automatic or held, covered the last step before a phrase line ([§9.5](09-arc.md#95-section-rules)).

### 8.8 Drop-out

A **Drop-out** is whole parts or Drums lanes falling silent for 1–16 bars in a Groove, usually with the last hit thrown into an echo. It is the main motion of a roots dub mix, and gives genres with longer phrases (Ambient, Psytrance) layer rests inside a Groove. It isn't a fill: it is longer, sits on bar lines, and lets its notes ring instead of cutting them.

**Entries.** The mechanism is Generator-wide. A genre lists **drop-out entries** (slot 6). Each entry gives:
- a mask of whole pitched parts (Bass, Chords, Pad, Lead), Drums lanes, and optionally FX lanes listed in the roster,
- a length of 1, 2, 4, 8 or 16 bars,
- a weight,
- a **throw target**: one pitched part or Drums lane in the mask that gets a Throw ([§11.4](11-sound-controls.md#114-gestures)), or none (never an FX lane).

An entry may not silence every audible part, because that would be a Gap: `genreValid()` refuses an entry whose mask covers every pitched part and every Drums lane that has a pool (FX lanes don't count as keeping a part playing). It also refuses an entry whose length isn't 1, 2, 4, 8 or 16 bars or is longer than the phrase, whose mask is empty or holds a lane not in the roster or a whole Kit, or whose throw target is an FX lane, outside the mask or more than one part or lane, and a genre whose gap is under a bar, whose start grid isn't a power of two dividing the phrase, or whose chance is outside 0 to 1000 per-mille. Dub, Dub techno, Ambient and Psytrance carry entries in v1 (*tuning*): Ambient's rest its Bass and bells for 4 or 8 bars; Psytrance's rest its Arp for 8 or 16 bars or everything but Kick and Bass for 8 on half-phrase lines. Other genres may add some during tuning.

**When.**
- Drop-outs play only in **Groove** phrases, which include the phrases a Drop decays over. Never in the Intro, a Break, a Build or the Outro.
- A drop-out starts on a bar line of the genre's **drop-out grid** (`dropOutGridBars`, a power of two dividing `phraseBars`, default 1 = any bar line; Psytrance uses 8, half-phrase and phrase lines) and ends on or before the next phrase line. Unlike a fill, it may cover a phrase's first bar, or a whole phrase when its length equals `phraseBars`. An entry too long to end by the phrase line, or by the start of a Build that takes the end of the phrase ([§9.3](09-arc.md#93-section-lengths)), isn't drawn.
- The draw is made a bar ahead: at each bar line before a grid line (`(barInPhrase + 1) % dropOutGridBars == 0`) where no drop-out is playing or waiting and the gap since the last one has passed, it decides whether one starts on the **next** bar line and which entry, so the bar in between can throw its last hit (below). It is keyed like every other draw ([§6](06-randomness.md)). The key leaves out every take, so re-rolling or freezing a part never moves drop-outs. A frozen part can still be dropped out.
- **What ends one early.** Any Section line ends a drop-out on that line: a forced Section, a Build shorter than a phrase starting mid-phrase, a transition's Build, Outro or cut line, and End's Outro. Its parts come back there, and a Throw falls over one beat from it. A Groove starts only on a phrase line, which no drop-out runs past, so a Groove's Reset never ends one early: the Throw has always fallen by then. A drop-out drawn to start after such a line doesn't play, and a part already spiked for it comes home on its next hit, as for any plan that changes. A stop clears both, and the next Start begins with none. A drop-out is never drawn when it couldn't end before a Build that is already known.
- The genre sets a **drop-out chance per bar** (per grid line) at Motion 25. Motion scales it along the drop-out curve ([§3.5](03-recipe.md#35-motion-curves)), a straight line: 0 at Motion 0, double at Motion 50, clamped at 100%.
- One drop-out plays at a time. After one ends, the next can't start for a genre **drop-out gap** of at least 1 bar (the default).

**How it plays.**
- The mask is applied after everything else, like a Rest, so no note-on is made and Compose writes none. It removes anchors.
- **Notes already sounding finish their length**, as under the mute gate ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)). This is the opposite of a Rest, so the thrown hit rings into its echo. Compose keeps their lengths. The one exception is a held **FX lane** in the mask (such as Dub's 2-bar Siren sounding from a Drop into bar 1): on the drop-out's first bar line it gets a note-off there (`SoundingNotes::cutAt()`), so the FX drops out with the rest and a Siren's release Throw plays into its echo ([§11.4](11-sound-controls.md#114-gestures)); when every FX lane is masked, the FX MUTE pad dims too.
- **Throw.** The throw target gets a Throw on its last hit in the bar before the drop-out: every hit in that bar's last beat spikes, as before a Break (`kThrowWindowSteps`, [§11.4](11-sound-controls.md#114-gestures)). If Density left it no hit there, there is no Throw. The spike holds while the part is silent and falls back to home over the last beat before it returns, so it comes back dry ([§11.4](11-sound-controls.md#114-gestures)). On Drums it moves the set the part's Throw acts on: the target lane's row when the Throw is aimed at that lane, otherwise the kit-wide set, which then holds while the drop-out lasts even if other lanes play. A Throw aimed at another lane leaves a Drums target unthrown. The drop-out's Throw has no switch: a part's Throw moments that are off don't stop it ([§11.4](11-sound-controls.md#114-gestures)). In the last beat before a Break, Gap or transition line that the drop-out ends on, that line's Throw comes first: a hit there that spikes for the line (a kit-wide Throw whose other lanes still play, say) holds into the line as any Throw before it does. If the line goes before it comes (a force or transition cancelled, a Motion turn that takes the Break away), a Throw the drop-out held goes back to the drop-out, and its hold and fall resume. If a different Section starts on the line's step instead, that Section ends the drop-out and the Throw plays by the plain rule from there: the part's next hit brings it home, with no fall. A target with a delay setup ([§10.6](10-sounds.md#106-load-time-settings)) holds and falls its delay Throw the same way.
- **Sustain and lines.** A Sustain's notes last only to the next bar line, so a dropped Pad's notes end on the drop-out's first bar line and their release rings; it re-strikes on the bar line the drop-out ends, as after an unmute. Loop and Motif lines keep advancing, passing and mutating while silent.
- **The Build roll, the Arc plan and the dim.** A drop-out never meets the Build roll: drop-outs play only in Groove, and none is drawn across a Build's start. So a drop-out never stops the roll lane. The Arc plan steps a copy of the same drop-out state (`DropOutState`), so a rest covering a whole phrase (or the Groove half of a phrase a short Build finishes) clears the part on the SONG page and agrees with the notes; a partial-phrase rest still lets the part sound in the rest of the phrase. A drop-out dims a part's MUTE pad while it lasts ([§13.5](13-perform.md#135-control-layout)).
- **Fills.** A fill and a drop-out may play in the same phrase. On a lane or part they share, the drop-out wins: it stays silent, and a Swap there doesn't play. The fill still plays on its other lanes, and the Phrase accent after a fill still applies ([§9.5](09-arc.md#95-section-rules)).
- **Nothing reacts to a drop-out.** Interplay rules read the normal planned pattern, and a drop-out is never a drum change for Answers ([§5.2](05-parts-and-lanes.md#52-flavours)). Drop-outs are pure subtraction.
- The player's MUTE is separate and unchanged: a mixer action with no Throw. A player-thrown mute is a later feature.

### 8.9 Punch-ins

A **punch-in** is a player's effect on the KEYBOARD page's punch-in pads ([§13.5](13-perform.md#135-control-layout)): on while its pad is held or latched, at a fixed amount, with no depth. It is steering (`SetPunch`: the punch-in, a Drums lane for a lane mute, on or off), so it lands on the next undecided step, and so does its release, and the steering log holds both ([§17.2](17-capture.md#172-the-steering-log)). It draws nothing: the piece around it, and after it, plays as it would have. Every punch-in but Pump acts on the Drums part. Several can be on at once.

- **Lane mute**, one per Drums lane: the lane's notes are flagged muted, as a part mute's are ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)), so none sounds and the ones already sounding ring out.
- **Hat roll**: the Closed hat plays 32nds (2 hits a 16th) at normal velocity, in place of its own hits. **Snare roll**: the Snare, or the Clap in a kit with no Snare, plays 16ths the same way. A roll plays whatever the lane's gates, section rules (Break keeps, the cut, the Gap), fills, Replace pairs and the Build roll say: it is the player's. A lane mute still silences it. Neither shows in the Arc plan or the MUTE pads' dim.
- **Flam**: the backbeat lanes' hits (Snare, Clap and Rim) play a flam ([§8.2](#82-ornaments)), at their lane's flam gap, in place of the Ornament the hit drew; in a kit with none of the three, every hit but the Kick's does (the snare roll's fallback, the owner's choice). In Techno that is the Rim and the Snare's fills and roll only, so at the default macros a Flam is often silent there. **Double kick**: every Kick hit plays a ratchet of 2 (a 32nd apart). The hits' Ornament draws stay as they were, so letting go plays the piece's own again. A flam can't fit before a Section's first step, where the hit plays plain, as any flam there does.
- **Pump**: a Pump event at the landing step's line tells the driver to duck every part but the Drums under the Kick, which sends to the sidechain at full ([§10.4](10-sounds.md#104-building-a-kit-in-code)): a synth part through its sidechain → volume cable (stock's VOLUME DUCKING, made if the preset has none), a Kit part through its kit-wide sidechain level, at a fixed depth (*tuning*). Off, each goes back to what it had. A part whose sound is swapped while it pumps ducks on its new sound too. A stop lets every part go, and so does each instance's start (a restart, a transition's start step), on the Outputs that ducked, the outgoing roster's included; the driver's step-0 re-send ducks the new instance again if PUMP is still on, so a release made after the last instance's final decision is never lost. A cable the pump made is deleted when it lets go, so the song keeps none it never had. An on and an off landing on the same step cancel: that step sends no Pump.
- Punch-ins are performance state, as mutes are: a start begins with none in the core, and the driver sends each one still on again at every instance's step 0, as it does a held FILL. A new Perform entry starts with none on.
- Compose plays none: they live in Perform only, and reach a song through Capture's replay.

Sources: [Sub-step expression][t20], [Pushed Lean][t30], [Lean per 16th position and the flam][t70], [Density as a fourth macro][t24], [POC gaps][t52], [Sound generation][t13], [Perform interaction model][t15], [Prior art in generative music engines][t6], [Perform control layout][t38], [Fill kinds and phrase-end events][t65], [Pattern choice under Complexity, and protected hits][t66], [Held FILL and how a Rest ends sounding notes][t79], [Dub's drop-outs and delay throws][t67], [Build shape and Section lengths][t68], [Arc headroom, gestures per Section, and Motion][t72], [How Motion scales Groove length, fills and drop-outs][t81], [Takes and freeze for stateful flavours][t76], POC findings on the [map][poc].

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
[t66]: https://github.com/gismya/deluge-generative/issues/66
[t67]: https://github.com/gismya/deluge-generative/issues/67
[t70]: https://github.com/gismya/deluge-generative/issues/70
[t79]: https://github.com/gismya/deluge-generative/issues/79
[t72]: https://github.com/gismya/deluge-generative/issues/72
[t81]: https://github.com/gismya/deluge-generative/issues/81
[t76]: https://github.com/gismya/deluge-generative/issues/76
