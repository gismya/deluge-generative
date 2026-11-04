# Deluge Generative

A generative mode for the Synthstrom Deluge, built on the community firmware. One set of musical parameters drives music that is either played live or rendered into an editable song.

## Language

**Generative mode**:
The umbrella feature: everything this firmware adds on top of the stock Deluge workflow.
_Avoid_: Gen mode, auto mode

**Recipe**:
What the Generator should make: a genre, the musical facts (tempo, key, scale, swing), the macro values and the sound picks. It holds no notes, and the seed sits beside it, not inside it.
_Avoid_: Preset, patch, settings, seed

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
A single 0–50 Recipe control that moves many musical features at once, in the same direction for every genre and part. There are three: Energy, Complexity and Motion.
_Avoid_: Mood, knob, parameter

**Energy**:
The macro for intensity: how many notes play, how hard and how bright.
_Avoid_: Mood, density, intensity

**Complexity**:
The macro for how intricate each beat is: syncopation, sub-step tricks, polymeter, layer interplay and harmonic colour.
_Avoid_: Busyness, density

**Motion**:
The macro for how much and how fast the music changes over time.
_Avoid_: Evolve, drift, variation

**Arc**:
The Generator's movement through sections (such as break, build and drop) around the Recipe's macro values, which stay the centre it returns to.
_Avoid_: Auto-DJ, tide, structure

**Section**:
A stretch of the Arc, whole phrases long, of one kind: Intro, Groove, Break, Build or Outro. Groove is home, where the macros sit at the Recipe's values.
_Avoid_: Scene, part, segment, state

**Drop**:
The moment a Build lands back in Groove, with everything playing and the Energy lifted for a while.
_Avoid_: Return, peak, section

**Gap**:
A short silence at the end of a Build, just before the Drop.
_Avoid_: Cut, pause, break

**Regenerate**:
Starting a fresh piece in the same genre: new musical facts where the genre rolls them, new sound picks and a new seed, with the macros kept.
_Avoid_: Reset, re-roll, new song, shuffle

**Part**:
One voice the Generator plays, such as the drums or the acid line. A genre lists its parts, and each part has exactly one role.
_Avoid_: Track, voice, instrument, layer, channel

**Role**:
The musical job a part does, from a fixed list: Drums, Bass, Chords, Pad, Lead or FX. It decides how the part's notes are made and how the Arc and other parts treat it.
_Avoid_: Track type, instrument type

**Flavour**:
The method a part uses to make notes within its role, such as a mutating loop or an arpeggio. The genre picks it per part.
_Avoid_: Style, mode, type

**Lane**:
One drum or FX kind inside a Drums or FX part, such as Kick, Open hat or Riser. Lanes are fixed kinds, so the Generator knows what each one is for.
_Avoid_: Row, drum, slot

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

**Drift**:
The slow, seeded wander of a part's sound controls around where they would otherwise sit. Motion sets how far and how fast it moves.
_Avoid_: Tide, LFO, wobble, modulation

**Generative pack**:
The files Generative mode needs on the SD card beyond the factory content, such as fallback FX sounds and our own presets. The firmware bundles no sounds of its own.
_Avoid_: Preset pack, content pack, firmware assets

**Generator**:
The single engine that turns a Recipe into notes, sounds, and automation. Both Perform and Compose use it.
_Avoid_: Engine, sequencer, AI

**Perform**:
The mode where the Generator plays live and non-destructively, steered in real time through the Recipe.
_Avoid_: Performance mode, live mode, auto-play

**Compose**:
The mode where the Generator renders a Recipe into a regular Deluge song, to be refined in the normal views.
_Avoid_: Song generation mode, sketch mode, render mode
