# 2. Architecture

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 2.1 The Generator core

- One portable C++ **Generator core** lives in the firmware tree. It has no firmware globals: no `currentSong`, no `display`, no root UI and no firmware allocator. The same sources build into the firmware and into the `tests/` harness ([§21](21-testing.md#21-testing-and-ci)).
- Genres are constant data in the core, read by one generic Generator ([§4](04-genres.md#4-genres)).
- The core has no globals of its own. All state of one Generator instance lives in one fixed-size block, sized for the largest roster (6 parts, every lane). The block is allocated once from the internal heap when Perform is entered or a Compose or Capture build starts. Perform allocates two, because a transition overlaps the outgoing and incoming instances ([§12.9](12-runtime.md#129-instances-and-the-transition)). The core never allocates, never touches the card, and uses no `new` and no std containers.
- The core uses only its own integer pseudo-random hash ([§6](06-randomness.md#6-randomness-and-seeding)), and integer math only for any decision that can change the event stream (ADR 0004). Probabilities and weights are integers on a fixed scale (per-mille or 0–255), never floats in 0–1.

### 2.2 Drivers and the event stream

The core exposes one **step function**. Three **drivers** call it:

| Driver | Where it runs | What it does with events |
|---|---|---|
| Perform | the tick hook, inside the audio routine ([§12](12-runtime.md#12-generator-runtime)) | sends notes to each part's Output, ramps sound controls |
| Compose | a yielding task loop in UI context ([§15](15-compose-output.md#15-compose-output-shape)) | writes clip notes and automation nodes into a private `Song*` |
| Harness | the `tests/` executable ([§21](21-testing.md#21-testing-and-ci)) | records them for specs, goldens and rough renders |

Capture is the Compose driver replaying a steering log ([§17](17-capture.md#17-capture)).

The step function produces an **event stream**. Every driver gets the same stream for the same input ([§6.3](06-randomness.md#63-the-contract)). Song swing comes from the clock, because the Generator works in swung ticks ([§12.1](12-runtime.md#121-the-tick-hook)). Lean, Ornaments and swing differences in Capture are plain events in that stream ([§8](08-rhythm-and-expression.md#8-rhythm-and-expression)). Offsets are in the Generator's fixed unit of 24 ticks per 16th ([§12.1](12-runtime.md#121-the-tick-hook)). It has six event kinds:

| Event | Carries | Notes |
|---|---|---|
| **NoteOn** | part, lane (Kits), absolute pitch (pitched parts), velocity, tick offset, **muted** flag | a muted or soloed-out part's notes stay in the stream, flagged ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)) |
| **NoteOff** | part, lane, pitch, tick offset | the core sends every note-off, from its sounding-note table. Notes carry no length |
| **Control** | part, lane or kit-wide, sound control, offset, ramp or snap | the offset from home in the control's own units, −100 to +100 (percent of its range on that side), already clamped to the control's offset range ([§11.3](11-sound-controls.md#113-three-layers)). Sent only when a target changes, on its step's grid line |
| **Section** | Section kind and occurrence index | Capture splits a long piece into songs on it, and the SONG page keeps its history from it |
| **KeyScale** | the resolved key and scale | sent when they change. Perform mirrors it into its song ([§12.8](12-runtime.md#128-performs-song-at-runtime)), and Capture cuts pitched clips on it and uses it to find chromatic clips ([§17.6](17-capture.md#176-changes-partway-through-a-piece)) |
| **Sound** | part, the driver's token for the sound it preloaded | where a sound re-roll lands, on a phrase line ([§10.7](10-sounds.md#107-when-sounds-load)). The part's notes end there first, and it plays after the line's note-offs and before its note-ons, so the old sound gets the note-offs and the new one the note-ons. Perform swaps the part's Output on it; the seed doesn't cover sounds, so no note changes ([§6.4](06-randomness.md#64-what-the-seed-covers)) |

Not in the stream, because the core never needs them:

- **Homes**, the per-engine mapping of a control onto params ([§11.2](11-sound-controls.md#112-mapping-per-engine)), and the clamp to the param's own range. These are driver-side: Perform reads homes from the part's Output, and Compose starts from the preset values.
- **Tempo and swing.** The core works in swung ticks. The Perform driver glides the song tempo ([§12.7](12-runtime.md#127-tempo-and-swing-glide)), and Capture writes tempo automation and builds swing differences into note ticks ([§17.6](17-capture.md#176-changes-partway-through-a-piece)).

### 2.3 The output seam

The Generator writes each part to an **output**. In v1 an output is an internal Synth (`SoundInstrument`) or Kit. MIDI/CV out must be addable later as another output kind without touching roles or the core.

### 2.4 Code and data layout

- Generator core and genre tables: a new directory under `src/deluge/`, chosen by the first slice. The POC used a `generator.h` seam, which is a fine starting point.
- Perform, the Generative menu, the Compose screen and the Recipe browsers: new UI classes under `src/deluge/gui/`.
- Harness: a new target in the `tests/` CMake tree, with specs as CppSpec `*_spec.cpp` files in `tests/spec`.
- Generative pack: `pack/` at the repo root, laid out exactly as on the card ([§18](18-generative-pack.md#18-generative-pack)).

### 2.5 Portable drivers and firmware adapters

Each driver is a **portable driver** next to the core, plus a thin firmware **adapter**, so the harness runs the real driver logic ([§21.2](21-testing.md#212-harness)):

- **Perform**: the portable part keeps the clock ([§12.3](12-runtime.md#123-the-drivers-clock)), decides 12 ticks ahead, drains the event buffer and emits to a `NoteSink`. The adapter calls `sendNote` / `noteOnPreKitArp`, applies homes and ramps params.
- **Compose**: the portable part writes one clip per part for the whole song (Capture cuts it at sound re-rolls and key changes, and splits songs at Section events), pairs NoteOn with NoteOff into note lengths, drops muted notes and turns Control events into node values, writing to a `SongWriter`. The adapter builds the `Song`.
- A transition's **phase** ([§12.9](12-runtime.md#129-instances-and-the-transition)) is portable too, beside the Perform driver. It hears from the driver's `NoteSink` when the transition's command lands and when the core drops it (two calls, each once per transition), beside the start step and the outgoing drain. Its adapter does the preload, the Outputs and the Current Recipe.
- The harness plugs in recording sinks.

### 2.6 Steering into the core

**Steering** is everything the player does in Perform that changes the music or the sound. It reaches the core one way, live and in replay alike:

- UI handlers, in task context, push **steering commands** `{kind, part or lane, payload}` into a fixed queue of 64 entries, one producer and one consumer. Continuous values (macros, homes, params) **coalesce** by key, keeping the latest value, so a control gets at most one value per step. A discrete command (force, mute, take…) that finds the queue full is refused with the stock popup.
- Commands the tick sees first go into the same queue from the driver: a MIDI FILL toggle, and the external-clock tempo sampled per bar ([§13.7](13-perform.md#137-sound-editor-midi-and-external-tempo)). These are the tick-side writers ADR 0005 allows: in Perform, the adapter's `sampleExternalTempo` pushes the tempo into the queue, and the driver's `Generator::takeQueued` moves line-bound commands into the pending list on the Outro's end line, which is never decided, and pushes every other command back onto the queue in order ([§13.2](13-perform.md#132-transport-start-and-end)).
- At each step decision the core drains the queue. A command for the next undecided step applies there. A bar-, phrase- or Drop-quantised one waits in a **pending list** in the instance state until its line ([§12.2](12-runtime.md#122-when-steps-are-decided)). Quantisation is in the core, so the harness can test force, hold, End and take steps.
- Every command is **logged when it lands**, stamped with the step where it takes effect, from inside that decision ([§17.2](17-capture.md#172-the-steering-log)). Cancelled actions never reach the log. An action whose landing depends on wall-clock time, such as a transition's Build starting when its preload ends, is logged where it actually landed. Stamping and applying happen in one decision, so they can't straddle a yield. See ADR 0005.
- Replay feeds logged entries back at their steps through the same apply path, with no pending list. So applying a command does everything its landing does (a key change's note-offs, a forced Section's start), wherever it comes from.
- A stop cancels the pending list, because every start is fresh ([§13.2](13-perform.md#132-transport-start-and-end)).
- End and a forced Section share one pending slot: the later of the two replaces the other. A tap on End cancels only End.
- A key change, a transition and a force or End have one slot each in the pending list, and each part has one for its take, its mute, its solo and its sound. A press that replaces the command in its slot counts as the newest. A BACK tap sends a cancel through the queue, which drops the newest waiting command, so it can't race the tick ([§13.5](13-perform.md#135-control-layout)).
- Toggles are resolved in the core, where the state is: a mute, solo or hold pad sends a press, and the core turns it into the part's or the Arc's other state, or, for a mute or solo still waiting for its bar line, cancels that one. The log holds the resolved state (0 or 1), so replay needs no press.
- **Driver-side steering** (homes, other params, tempo and swing glides) acts in the Perform driver at once and goes through the queue only to be logged, at the next undecided step. The core passes it through unchanged. Capture's Compose driver applies it from the log.
- The queue and the log are a portable module outside the instance blocks, because a log spans pieces and transitions. The driver hands the module its buffers.

### 2.7 Buffer sizes and overflow

- The event buffer and the sounding-note table are sized **by construction** from core constants: max parts, max lanes, max ratchet hits, max chord notes, flam grace hits. The buffer holds a worst-case step plus the step still playing, worked out part by part for the largest roster, and a `static_assert` checks that the sounding-note table holds a hit per lane or two overlapping chords.
- If a step overflows anyway (a bug or bad genre data), the core drops Ornament hits first, then parts in reverse role priority (FX, Pad, Chords, Lead, Bass, Drums), so every driver gets the same stream. A harness spec fails on any drop.
- A part's sounding-note table holds what the event buffer counts for its role: one note per lane for a Kit part, two chords for a pitched part. When it is full, its oldest note gets a NoteOff before the new NoteOn. The parts' tables share one pool per instance, laid out by role when a piece starts and sized for a roster with at most one Drums part ([§5.1](05-parts-and-lanes.md#51-parts-and-roles)).
- In the worst-case step, each of a note's hits can also end inside the step: a ratchet's earlier hits end at the next one, and its last can end before the next decision (four hits 6 ticks apart at Lean −12).
- On stop, a jump, Perform exit or retiring an Output, the driver asks the core to **flush**: NoteOffs for everything in its table ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)).

### 2.8 Queries from task context

The tick and the UI never pre-empt each other: the audio routine is a cooperative task, so it runs only where UI work yields. Task-context code may read an instance with `const` queries, as long as it doesn't yield mid-read:

- **Arc plan**: a few phrases ahead, computed on demand from the Arc counters and keyed draws, with no cache and no second copy of state. It includes the pending list, so a queued action shows its result. The SONG page calls it only when a phrase line passes or the queue changes ([§13.5](13-perform.md#135-control-layout)). Its "played" columns come from a small per-phrase history ring the Perform driver keeps from the events it sent.
  - The Generator keeps no copy of the plan; the SONG page keeps the last result until a phrase line passes or the queue changes.
  - `Generator::planArc` steps a copy of the Arc, the gates and the mute and solo masks through 16 phrases from the one the last decided step is in, landing each pending command on the step its line rule names, with the Recipe as it is now. So a Motion turn shows in the plan from the next phrase line or queue change. "The queue changes" covers the pending list, the hold and the mute and solo state. A transition waiting or landed plays its Build or Outro in the plan, and the phrases from its start step are the new roster's, which the plan can't know: the SONG page shows them as nothing known.
  - The history ring starts again at a transition's start step, as at a start: the new roster's phrases count from its own step 0.
  - The history ring keeps 16 phrases: each phrase's Section (the last Section event in it), whether a Drop started it, the parts with a note sent, and Energy against the centre averaged over its decided steps. A start clears it.
- **Transition**: `transitionWaiting()`, `transitionStart()` (the start step once its command has landed), `stoppedDeciding()` and `isDrained()` on an instance ([§12.9](12-runtime.md#129-instances-and-the-transition)).

Sources: [Simulation approach][t8], [Genre model][t11], [Track roles][t12], [Generator scheduling][t19], [Testing and CI][t41], [Build order][t44], [Interface between the Generator core and its drivers][t75], [Determinism details: tick unit, tempo, held notes and ramps][t77].

[t8]: https://github.com/gismya/deluge-generative/issues/8
[t11]: https://github.com/gismya/deluge-generative/issues/11
[t12]: https://github.com/gismya/deluge-generative/issues/12
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t41]: https://github.com/gismya/deluge-generative/issues/41
[t44]: https://github.com/gismya/deluge-generative/issues/44
[t75]: https://github.com/gismya/deluge-generative/issues/75
[t77]: https://github.com/gismya/deluge-generative/issues/77
