# Deluge Generative

A generative mode for the Synthstrom Deluge, built on the community firmware. One set of musical parameters drives music that is either played live or rendered into an editable song.

## Language

**Generative mode**:
The umbrella feature: everything this firmware adds on top of the stock Deluge workflow.
_Avoid_: Gen mode, auto mode

**Recipe**:
The set of musical parameters (genre, BPM, energy, motion, and so on) that describes what the Generator should make.
_Avoid_: Preset, patch, settings, seed

**Generator**:
The single engine that turns a Recipe into notes, sounds, and automation. Both Perform and Compose use it.
_Avoid_: Engine, sequencer, AI

**Perform**:
The mode where the Generator plays live and non-destructively, steered in real time through the Recipe.
_Avoid_: Performance mode, live mode, auto-play

**Compose**:
The mode where the Generator renders a Recipe into a regular Deluge song, to be refined in the normal views.
_Avoid_: Song generation mode, sketch mode, render mode
