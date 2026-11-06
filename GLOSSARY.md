# Deluge Generative

A generative mode for the Synthstrom Deluge, built on the community firmware. One set of musical parameters drives music that is either played live or rendered into an editable song.

## Language

**Generative mode**:
The umbrella feature: everything this firmware adds on top of the stock Deluge workflow.
_Avoid_: Gen mode, auto mode

**Recipe**:
What the Generator should make: a genre, the musical facts (tempo, key, scale, swing), the macro values and the sound picks. It holds no notes, and the seed sits beside it, not inside it.
_Avoid_: Preset, patch, settings, seed

**Current Recipe**:
The one Recipe, with its seed and takes, that Generative mode is working on. Perform steers it and Compose edits it, so both modes start from where the player left off. Picking a genre or a Recipe file replaces it, and it is lost when the Deluge is switched off unless saved.
_Avoid_: Last Recipe, active Recipe, draft, working copy

**Recipe file**:
A Recipe saved on the SD card with its seed and takes, from which the Generator can start the same music again. It holds no steering, so it doesn't replay a piece; a Capture song does that.
_Avoid_: Piece, preset, song, patch

**Seed**:
The number beside a Recipe that fixes every random choice the Generator makes while playing it. The same Recipe and seed, steered the same way at the same moments, play the same notes.
_Avoid_: Random state, take, variation

**Take**:
A part's re-roll count, starting at 0. Re-rolling a part gives it the next take: new patterns for that part only, with the rest of the piece unchanged, and any earlier take can be played again exactly. A new seed puts every part back to take 0.
_Avoid_: Variation, version, seed

**Genre**:
A built-in style definition. It gives a new Recipe its starting values and sets the rules the Generator plays by. The player picks a genre but can't edit it.
_Avoid_: Style, preset, piece, template

**Macro**:
A single 0–50 Recipe control that moves many musical features at once, in the same direction for every genre and part. There are four: Energy, Density, Complexity and Motion.
_Avoid_: Mood, knob, parameter

**Energy**:
The macro for intensity: how hard, bright and driven the notes are. How many notes play is Density's job.
_Avoid_: Mood, intensity, loudness

**Density**:
The macro for how many notes play: which steps sound, by their importance, and which parts and lanes are let in at all.
_Avoid_: Busyness, energy

**Complexity**:
The macro for how intricate each beat is: which Complexity tier of pattern plays, sub-step tricks, layer interplay, and how many Extensions a chord carries.
_Avoid_: Busyness, density

**Motion**:
The macro for how much and how fast the music changes over time.
_Avoid_: Evolve, drift, variation

**Arc**:
The Generator's movement through sections (such as break, build and drop) around the Recipe's macro values, which stay the centre it returns to.
_Avoid_: Auto-DJ, tide, structure

**Section**:
A stretch of the Arc, whole phrases long, of one kind: Intro, Groove, Break, Build or Outro. Groove is home, where the macros sit at the Recipe's values. Not the Deluge's own session-view "section", a numbered group of clips launched together. Call that a **session section**.
_Avoid_: Scene, part, segment, state

**Drop**:
The moment a Build lands back in Groove, with everything playing and the Energy and Density lifted for a while.
_Avoid_: Return, peak, section

**Gap**:
A short silence at the end of a Build, just before the Drop.
_Avoid_: Cut, pause, break

**Regenerate**:
Starting a fresh piece in the same genre: new musical facts where the genre rolls them, new sound picks and a new seed, with the macros kept.
_Avoid_: Reset, re-roll, new song, shuffle

**Piece**:
What Perform plays from a Start, a Regenerate, a genre change or a Recipe file loaded mid-set, until the transport stops or the next of those. Starting again after a stop begins a new piece.
_Avoid_: Song, set, take, session

**Part**:
One voice the Generator plays, such as the drums or the acid line. A genre lists its parts, and each part has exactly one role.
_Avoid_: Track, voice, instrument, layer, channel

**Role**:
The musical job a part does, from a fixed list: Drums, Bass, Chords, Pad, Lead or FX. It decides how the part's notes are made and how the Arc and other parts treat it.
_Avoid_: Track type, instrument type

**Flavour**:
The method a part uses to make notes within its role, such as a mutating loop or an arpeggio. The genre picks it per part.
_Avoid_: Style, mode, type

**Register**:
A part's pitch home: the note its music centres on and the span it stays within. A key change keeps every part in its register rather than moving it by the full interval.
_Avoid_: Range, octave, tessitura

**Degree set**:
The scale degrees a bass or lead part may land on, named for the sound it wants, such as Full or Pentatonic, and worked out for whichever scale is playing. It stays fixed to the key's home, but the current chord's root is always allowed, and a note that lands outside moves to the nearest allowed degree. A genre sets it per part.
_Avoid_: Note set, mask, sub-scale, pool, mode

**Chord graph**:
A genre's chord habits, written over scale degrees so that one graph serves every scale: how strongly home pulls, which moves it favours, how often it stays put, and how often a phrase cadences. The Generator walks it to pick each chord, and steers away from any chord that isn't plain major or minor in the current scale. A genre with no Chord graph stays on its home chord.
_Avoid_: Progression, chord table, chord map

**Colour move**:
A chord move that gives a scale its character, such as Dorian's i–IV or Phrygian's i–♭II. The Generator holds one row of them per scale and adds them to whichever chord graph is playing, so a genre gets the scale's colour without listing it.
_Avoid_: Modal move, signature move, characteristic progression

**Extension**:
An in-scale tone added to a chord's triad for colour: a 7th, a 9th, a sus4 (in place of the 3rd) or a 6th. A tone a semitone above a chord tone is never added.
_Avoid_: Colour, colour tone, tension, added note

**Extension level**:
How many Extensions a chord carries: none, one or two. Complexity raises it, and a genre can set a floor and a ceiling, so House stabs always carry a 7th.
_Avoid_: Colour level, colour floor, chord kind, chord complexity

**Pattern**:
One bar or two of rhythm for one lane or one part, taken from the genre's pool. Each step says how important it is and how hard it plays (ghost, normal or accent). Density thins a pattern by importance but never edits it.
_Avoid_: Groove, beat, sequence, ghost mask

**Anchor**:
A step in a pattern that always plays when its lane or part plays at all, such as a four-on-the-floor kick or House's claps on 2 and 4. Density, mutation and interplay never remove it. Only gates, Break keeps, the Gap, fills, Drop-outs, Replace pairs and mute do.
_Avoid_: Backbeat, protected step, downbeat

**Complexity tier**:
How intricate a pattern is, from 0 (plain) to 3 (busiest), tagged by hand. Complexity picks the tier, within a range the genre sets for each part and lane.
_Avoid_: Syncopation level, difficulty, variation

**Ornament**:
Extra hits that decorate a single note inside its 16th: a ratchet (the note repeated two to four times) or a flam (a soft grace hit a few ticks before the main hit, which stays where the note belongs). Complexity decides how often they happen. A Build's snare roll is a Build driving ornaments, not an ornament of its own.
_Avoid_: Fill, roll, flourish, trick

**Fill**:
A short change to the music that ends on a phrase line, such as a kick drop or a snare run into the next phrase. It is one of three kinds: a Rest (some drum lanes, and maybe whole parts such as the bass, stop for a few beats), a Swap (drum lanes play a fill pattern instead of their usual one) or a Roll (a short climbing roll on one drum lane). The Generator plays fills on its own at phrase ends, more often with more Motion. The player can also hold FILL, which plays the genre's held-fill entry, or SHIFT + FILL for a fresh pick.
_Avoid_: Break, turnaround, mute, drop-out, Ornament

**Drop-out**:
Whole parts or drum lanes falling silent for one to four bars in a Groove, often with the last hit thrown into an echo, such as the skank dropping out of a dub mix. A genre lists the drop-outs it may play, and the Generator plays them more often with more Motion. Unlike a fill, the notes already sounding ring on.
_Avoid_: Rest, mute, Break, Fill, cut

**Phrase accent**:
A single hit on a genre's accent lane, usually the crash, that marks the start of a phrase: always on a Drop and after a fill, and sometimes on other Groove phrase starts.
_Avoid_: Crash, downbeat, accent, Impact

**Lean**:
How far behind or ahead of the grid a part or lane plays, set separately for each 16th position in the beat (the 1, e, & and a), such as hats that swing harder than the song, a lazy snare or a pushed hat. It is the genre's timing feel, added on top of the song's swing, and no macro moves it.
_Avoid_: Feel, groove, swing, nudge, pocket

**Velocity spread**:
A small seeded nudge to each note's velocity in a part or lane. It either repeats with the pattern or is fresh on every pass. It is the genre's touch, and no macro moves it.
_Avoid_: Humanize, velocity jitter, randomness

**Lane**:
One drum or FX kind inside a Drums or FX part, such as Kick, Open hat or Riser. Lanes are fixed kinds, so the Generator knows what each one is for.
_Avoid_: Row, drum, slot

**Replace pair**:
Two lanes of one part where the first one wins when both land on the same step, so the second one's hit is dropped, such as Open hat replacing Closed hat. It works in one direction only. Lanes not in a pair play together freely.
_Avoid_: Exclusion, mute group, choke

**Palette**:
The list, for one genre, of which sounds each part and lane may use and how likely each one is. Every genre has one built in, and a file on the SD card can override single entries. It changes only what the music sounds like, never how it plays.
_Avoid_: Sound bank, preset list, kit list

**Sound pick**:
The concrete sound a Recipe holds for one part or lane: a preset or a sample on the card. It is rolled from the palette and kept until the player rolls it again or changes it.
_Avoid_: Patch, preset choice, voice

**Sound control**:
One of six fixed ways the Generator changes a sound while it plays: Brightness, Decay, Drive, Space, Wobble and Thin. Each one moves the sound within bounds around its home: how it was saved, or wherever the player has since set it.
_Avoid_: Parameter, knob, macro, timbre

**Gesture**:
A named sound-control move tied to a moment in the Arc, such as closing the filter as a break starts or sweeping the high-pass up through a build. The set is fixed; a genre only switches each gesture on per part and sets how deep it goes.
_Avoid_: FX, move, transition, automation

**Throw**:
The Gesture that sends a part's last hit before it falls silent into its reverb and, where the part has a delay set up, its echo. It holds while the part is silent, then falls back.
_Avoid_: Send, dub throw, echo, Space

**Drift**:
The slow, seeded wander of a part's sound controls around where they would otherwise sit. Motion sets how far and how fast it moves.
_Avoid_: Tide, LFO, wobble, modulation

**Generative pack**:
The files Generative mode needs on the SD card beyond the factory content, such as fallback FX sounds and our own presets. The firmware bundles no sounds of its own.
_Avoid_: Preset pack, content pack, firmware assets

**Generator**:
The single engine that turns a Recipe into notes, sounds, and automation. Both Perform and Compose use it.
_Avoid_: Engine, sequencer, AI

**Driver**:
What moves the Generator forward one step at a time and takes the result: Perform's live clock, Compose's render into a song, or the test harness. The Generator makes the same music whichever driver runs it.
_Avoid_: Runner, host, player, loop

**Perform**:
The mode where the Generator plays live and non-destructively, steered in real time through the Recipe.
_Avoid_: Performance mode, live mode, auto-play

**Compose**:
The mode where the Generator renders a Recipe into a regular Deluge song, to be refined in the normal views.
_Avoid_: Song generation mode, sketch mode, render mode

**Capture**:
Keeping pieces played in Perform as regular Deluge songs. The player marks pieces to keep while playing. On leaving Perform, the player can keep each marked piece, and the last piece even if unmarked, as a song of its own with the notes the player heard.
_Avoid_: Record, bounce, export, resample
