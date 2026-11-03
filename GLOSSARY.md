# Deluge Generative

A generative mode for the Synthstrom Deluge, built on the community firmware. One set of musical parameters drives music that is either played live or rendered into an editable song.

## Language

**Generative mode**:
The umbrella feature: everything this firmware adds on top of the stock Deluge workflow.
_Avoid_: Gen mode, auto mode

**Recipe**:
What the Generator should make: a genre, the musical facts (tempo, key, scale, swing) and the macro values. It holds no notes, and the seed sits beside it, not inside it.
_Avoid_: Preset, patch, settings, seed

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

**Generator**:
The single engine that turns a Recipe into notes, sounds, and automation. Both Perform and Compose use it.
_Avoid_: Engine, sequencer, AI

**Perform**:
The mode where the Generator plays live and non-destructively, steered in real time through the Recipe.
_Avoid_: Performance mode, live mode, auto-play

**Compose**:
The mode where the Generator renders a Recipe into a regular Deluge song, to be refined in the normal views.
_Avoid_: Song generation mode, sketch mode, render mode
