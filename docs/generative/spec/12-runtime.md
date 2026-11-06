# 12. Generator runtime

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 12.1 The tick hook

- One hook in `Session::doTickForward`, after the per-Output arp loop (`playback/mode/session.cpp`, F1), calls the Perform driver. The driver calls `generator.tick(...)`, which returns the ticks until its next event and lowers `playbackHandler.swungTicksTilNextEvent`, as the arp does.
- The Generator sends notes with `sendNote` on Synths and `noteOnPreKitArp` on Kits, the same entry points clip playback uses from the tick. Notes never go into Perform's clips (ADR 0002).
- Perform must stay in session playback (`lastClipInstanceEnteredStartPos = -1`).
- The Generator works in **swung ticks**, so song swing applies for free. A 16th is 24 ticks at the default magnitude.
- The hook loops over running instances. v1 runs one.

### 12.2 When steps are decided

- The Generator decides each 16th step **12 ticks before its grid line**, in every genre (ADR 0003). Ticks between decisions only read the event buffer.
- The fixed event buffer holds the step being decided as well as the step playing.
- Continuous steering takes effect from the next undecided step: about 63 ms more latency at 120 BPM (625/BPM ms per tick) on top of the wait of up to one step.
- Bar- and phrase-quantised actions (mute, solo, force, take, key change) must arrive 12 ticks before the line. A later press lands on the following boundary.
- If step cost turns out too high on hardware, the fallback is to plan Arc and Section decisions at phrase lines in task context and keep only note decisions in the tick.

### 12.3 The driver's clock

- The driver keeps its own clock. On every hook call it adds the swung-tick delta, shifted by the current magnitude, with the remainder carried. It never divides (the POC's 64-bit divide per call must not return) and never works out time from an origin, so a magnitude change only alters the clock's rate.
- A delta that is negative or longer than a bar counts as a **jump**, handled like a mid-play SPP ([§12.4](#124-external-clock)).
- Tempo magnitude matching stays as the player set it, as in a stock song. It settles within the first beat after an external Start.
- `Song()` takes a user-set default magnitude, and an external clock can change it mid-play. Never assume magnitude 2.

### 12.4 External clock

- **Start:** step 0 lands at tick 0, the normal Perform start.
- **Continue and SPP:** the driver tracks the SPP offset itself, because the firmware's song counter doesn't follow SPP. It stays silent until the next master bar line and puts step 0 there.
- **Mid-play SPP or a jump:** note-offs at the jump, then resume at the next master bar line. The Arc carries on where it was and is never rewound by SPP.
- Tempo under an external clock is [§13.7](13-perform.md#137-sound-editor-midi-and-external-tempo).

### 12.5 Mute gate and note-offs

- A muted part is computed every step exactly like an audible one. The gate sits at the output, where note-ons are dropped. Notes already sounding finish their length.
- A Drop-out works like the mute gate: no note-ons, and sounding notes finish their length, so a thrown hit rings ([§8.8](08-rhythm-and-expression.md#88-drop-out)).
- A Rest is different: when it starts, the Generator sends note-offs for the notes sounding on its pitched parts, and leaves drum one-shots to play out ([§8.7](08-rhythm-and-expression.md#87-fill)).
- Firmware clip mute and solo are never used in Perform. The firmware has no session-level Output mute, and an inactive clip stops param ramps.
- Direct `sendNote` calls aren't tracked by the firmware, so the Generator keeps a fixed table of sounding notes per part and schedules its own note-offs as events.
- On stop, Perform exit, a jump, or retiring an Output, it sends note-offs for everything in its table, then all notes off on each part's Output as a safety net. There's no Output-level `allNotesOff`: Kits need `Sound::allNotesOff` per drum, as the POC does. Tails ring out.

### 12.6 Voice starts

- The firmware starts at most 4 voices per render window (1 under heavy load). Extra starts aren't dropped: they wait for the next render, at most 2.9 ms later (the routine targets 1.45 ms). At 130 BPM a render is shorter than a tick, so the budget is per tick.
- Each step sends note-offs first, then note-ons in role priority: Drums (Kick, then Snare and Clap, then the rest), Bass, Lead, Chords, Pad, FX. Ornament hits start last, so under load a flam's grace hit is the one that waits.
- Compose has no voice cap and writes every hit. That stays within the contract, which covers the event stream, not audio.

### 12.7 Tempo and swing glide

- While a glide runs, the driver writes song tempo once per swung tick on a linear curve, asking for a tick every tick. `Song::setTempoFromNumSamples` is public (call it with `shouldLogAction = false`, or it logs an undo action each tick), and stock already changes tempo inside the tick.
- Swing changes only on step boundaries.
- Tempo glide is off while following an external clock.

### 12.8 Perform's song at runtime

- Each part's Output has one empty, always-active, looping 1-bar session clip. An Output with no active clip renders nothing.
- A Kit clip carries one NoteRow per lane, in the fixed lane order, because `noteOnPreKitArp` and per-lane sound controls go through those rows.
- Perform writes the Recipe's key and scale into its own song's `key` at every resolved change, by plain assignment. Its clips are empty, so nothing is remapped. Firmware that reads the song key, such as the arpeggiator's chord mode, then agrees with what plays.

### 12.9 Instances and the transition

- During a genre change, Regenerate or Recipe LOAD, the incoming roster is held ready: loaded Outputs plus its Recipe, seed, takes and genre.
- It starts generating on the Drop downbeat, with walking state at its initial values as on Start. Its state swaps in atomically on that step boundary.
- Running two instances at once, for a crossfade, is out of scope, but the no-globals rule keeps it cheap.

### 12.10 Input during card loads

Audio and the tick hook keep running during card loads, and pad and button handlers are still called, with `sdRoutineLock` set.

- **Act at once:** actions that change only Recipe or steering state: macros, force, hold, Fill, mute and solo, part focus, Capture mark.
- **Wait:** actions that need the card or another build: genre change, Regenerate, Recipe LOAD or SAVE, a sound re-roll. They wait through the stock `REMIND_ME_OUTSIDE_CARD_ROUTINE` deferral.
- BACK still cancels a queued genre change. Gold knobs are deferred by the firmware to just after each card routine, so they steer with a few ms of extra lag.

Sources: [Generator scheduling][t19], [Pushed Lean][t30], [POC gaps][t52], [Hardware tests of the Perform POC][t45], [Pitch model][t21], [Sub-step expression][t20], [Firmware changes the design needs][t43], [Firmware architecture of v1.3.0-rc3][t2], [Runtime budget on the Deluge][t3].

[t2]: https://github.com/gismya/deluge-generative/issues/2
[t3]: https://github.com/gismya/deluge-generative/issues/3
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t30]: https://github.com/gismya/deluge-generative/issues/30
[t43]: https://github.com/gismya/deluge-generative/issues/43
[t45]: https://github.com/gismya/deluge-generative/issues/45
[t52]: https://github.com/gismya/deluge-generative/issues/52
