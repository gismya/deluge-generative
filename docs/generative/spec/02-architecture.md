# 2. Architecture

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 2.1 The Generator core

- One portable C++ **Generator core** lives in the firmware tree. It has no firmware globals: no `currentSong`, no `display`, no root UI and no firmware allocator. The same sources build into the firmware and into the `tests/` harness ([§21](21-testing.md#21-testing-and-ci)).
- Genres are constant data in the core, read by one generic Generator ([§4](04-genres.md#4-genres)).
- The core has no globals of its own. All state of one Generator instance lives in one fixed-size block, sized for the largest roster (6 parts, every lane). The block is allocated once from the internal heap when Perform is entered or a Compose or Capture build starts. The core never allocates, never touches the card, and uses no `new` and no std containers.
- The core uses only its own integer pseudo-random hash ([§6](06-randomness.md#6-randomness-and-seeding)), and integer math only for any decision that can change the event stream (ADR 0004). Probabilities and weights are integers on a fixed scale (per-mille or 0–255), never floats in 0–1.

### 2.2 Drivers and the event stream

The core exposes one **step function**. Three **drivers** call it:

| Driver | Where it runs | What it does with events |
|---|---|---|
| Perform | the tick hook, inside the audio routine ([§12](12-runtime.md#12-generator-runtime)) | sends notes to each part's Output, ramps sound controls |
| Compose | a yielding task loop in UI context ([§15](15-compose-output.md#15-compose-output-shape)) | writes clip notes and automation nodes into a private `Song*` |
| Harness | the `tests/` executable ([§21](21-testing.md#21-testing-and-ci)) | records them for specs, goldens and rough renders |

Capture is the Compose driver replaying a steering log ([§17](17-capture.md#17-capture)).

The step function produces an **event stream**: absolute-pitch notes with tick offsets, lengths and velocities, sound-control targets, and Arc section changes. Every driver gets the same stream for the same input ([§6.3](06-randomness.md#63-the-contract)). Swing, Lean and Ornaments are all plain events in that stream ([§8](08-rhythm-and-expression.md#8-rhythm-and-expression)).

### 2.3 The output seam

The Generator writes each part to an **output**. In v1 an output is an internal Synth (`SoundInstrument`) or Kit. MIDI/CV out must be addable later as another output kind without touching roles or the core.

### 2.4 Code and data layout

- Generator core and genre tables: a new directory under `src/deluge/`, chosen by the first slice. The POC used a `generator.h` seam, which is a fine starting point.
- Perform, the Generative menu, the Compose screen and the Recipe browsers: new UI classes under `src/deluge/gui/`.
- Harness: a new target in the `tests/` CMake tree, with specs as CppSpec `*_spec.cpp` files in `tests/spec`.
- Generative pack: `pack/` at the repo root, laid out exactly as on the card ([§18](18-generative-pack.md#18-generative-pack)).

Sources: [Simulation approach][t8], [Genre model][t11], [Track roles][t12], [Generator scheduling][t19], [Testing and CI][t41], [Build order][t44].

[t8]: https://github.com/gismya/deluge-generative/issues/8
[t11]: https://github.com/gismya/deluge-generative/issues/11
[t12]: https://github.com/gismya/deluge-generative/issues/12
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t41]: https://github.com/gismya/deluge-generative/issues/41
[t44]: https://github.com/gismya/deluge-generative/issues/44
