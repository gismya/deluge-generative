# 12. Generator runtime

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 12.1 The tick hook

- One hook in `Session::doTickForward`, after the per-Output arp loop (`playback/mode/session.cpp`, F1), calls the Perform driver. The driver calls `generator.tick(...)`, which returns the ticks until its next event and lowers `playbackHandler.swungTicksTilNextEvent`, as the arp does.
- The Generator sends notes with `sendNote` on Synths and `noteOnPreKitArp` on Kits, the same entry points clip playback uses from the tick. Notes never go into Perform's clips (ADR 0002).
- Perform must stay in session playback (`lastClipInstanceEnteredStartPos = -1`).
- The Generator works in **swung ticks**, so song swing applies for free. A 16th is 24 ticks at the default magnitude.
- The hook loops over running instances: one, or two around a transition ([§12.9](#129-instances-and-the-transition)).

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

A genre change, Regenerate or Recipe LOAD in Perform runs one **transition** ([§9.7](09-arc.md#97-transitions)). This section is its lifecycle, from the confirm until the outgoing Outputs are freed.

**Phases.**

```
        confirm             loaded        Build or Outro starts       start step        outgoing freed
Idle ──────────▶ Preloading ──────▶ Queued ───────────────────▶ Committed ──────────▶ Retiring ──────────▶ Idle
                    │                 │  │                                       ▲
                    │                 │  └─── hard cut: the cut line is the start step
                    └── cancel ───────┴──▶ Idle (a new request cancels, then starts its own Preloading)
```

| Phase | What happens | Cancel |
|---|---|---|
| **Preloading** | The incoming roster loads into fresh Outputs in task context ([§10.7](10-sounds.md#107-when-sounds-load)). The old genre plays on. | yes |
| **Queued** | Loaded and linked, waiting for its Build or Outro to start, or for the cut line. | yes |
| **Committed** | The outgoing Build or Outro plays. A hard cut has no Committed phase. | no |
| **Retiring** | The new roster plays from its **start step** (the Drop downbeat, the seam, or the cut line). The outgoing Outputs finish their tails. | no |

The **transition pending** state runs from the confirm until the start step (Preloading, Queued and Committed).

**The pending Recipe.**

- The confirm builds a **pending Recipe** with its seed and takes: rolled for a genre change or Regenerate ([§9.7](09-arc.md#97-transitions)), the file's values for a LOAD. The Current Recipe stays the outgoing one until the start step, where the pending Recipe replaces it whole.
- A genre change or Regenerate copies the live macros into it at the start step (macros are kept). A LOAD keeps the file's macros, so macro turns since the confirm are lost.
- A SAVE while a transition is pending writes the Current Recipe, which is what is playing.
- Cancelling discards the pending Recipe and frees the incoming Outputs.

**Two instances.**

- Perform allocates **two** Generator state blocks on entry ([§2.1](02-architecture.md#21-the-generator-core)) and alternates between them, one per transition, so nothing is allocated mid-set.
- When the preload ends, the incoming block is initialised in task context with the pending Recipe, seed, takes and genre, and walking state at its initial values, as on Start.
- The incoming instance decides from the start step on, so its first decision is 12 ticks before it ([§12.2](#122-when-steps-are-decided)). The outgoing instance decides no step at or after the start step. The hook keeps draining its event buffer (note-offs, Lean-late hits, the Throw ramp) until it is empty, then that instance goes idle.
- The two overlap only between the incoming instance's first decision and the outgoing buffer running dry. Running two for a crossfade is out of scope ([§1.2](01-scope.md#12-out-of-scope-for-v1)).

**Incoming Outputs.**

- At the end of Preloading, in task context, each incoming Output is linked into Perform's song with its empty, always-active, 1-bar clip ([§12.8](#128-performs-song-at-runtime)) and, for a Kit, its NoteRows. This follows stock `SessionView::createNewInstrumentClip`: `setActiveClip` before `addOutput`, so nothing half-patched renders. Linking allocates and yields, so it never happens in a tick.
- No notes go to them until the start step. Perform never uses firmware solo or section launch, so their clips can't be muted or launched by accident.
- A silent Synth skips rendering. A silent Kit still runs its FX chain every buffer (`global_effectable_for_clip.cpp:111-161`), so up to two silent incoming Kits cost CPU through Queued and Committed. If that is too much on hardware, the fallback is to link Kits late, about a bar before the start step.

**Retiring the outgoing Outputs.**

- An outgoing Output is freed once it is silent: `skippingRendering` for a Synth, and for a Kit no active drums (`drumsWithRenderingActive` empty), its delay's `repeatsUntilAbandon` at 0, no arp or stutter, and no `NonAudioDrum` voices.
- The cap is one phrase after the start step. At the highest delay feedback an echo never dies (`delay.cpp:187`), so an Output still sounding at the cap fades its volume over 1 beat, gets note-offs ([§12.5](#125-mute-gate-and-note-offs)), and is freed.
- Freeing runs in task context, outside the card routine, through the long-work runner ([§12.10](#1210-input-during-card-loads)), because deleting a Kit yields once per drum. It removes the clip with `InstrumentRemoval::DELETE`, because the default only hibernates an Output the user has edited. The Output is unlinked from rendering before the first yield, as stock `SessionView::removeClip` does.
- Before freeing, Perform clears every pointer at the Output: gold-knob focus (`view.activeModControllableModelStack`), the MIDI-learn pointers (`view.h:113-118`), `Instrument::midiInput`, MIDI follow's last clip (`midiFollow.removeClip`), `Song::currentClip`, `syncScalingClip` and the pulsing clip. Perform keeps the undo log empty from entry, so no action holds an Output. Params learned on an outgoing part are lost ([§13.7](13-perform.md#137-sound-editor-midi-and-external-tempo)).

**At the start step.**

- The new roster starts **clean**: every part unmuted, unsoloed and unfrozen, every take at 0, and no "previous sound" remembered. Gold-knob focus and a double-tap lock drop to song level.
- A sound editor open on an outgoing part closes, back to the Perform page.
- The new key, scale and swing take effect ([§9.7](09-arc.md#97-transitions)).

**While a transition is pending.** These are refused with a popup: tempo, swing, key and scale edits (TAP and MIDI TRANSPOSE too), End, take re-rolls and steps, sound re-rolls and back to previous sound. At the confirm, any take or sound action still queued is dropped, and a sound re-roll still preloading is cancelled and freed. Macros, force, hold, Fill, mute, solo, freeze, focus and Capture marks work as usual on the outgoing roster.

**Other requests.**

- **A second genre change, Regenerate or LOAD** during Preloading or Queued replaces the first: the first is cancelled (its preload stopped, or its Outputs freed) and the new one starts from its own confirm. During Committed or Retiring it's refused with a popup until the outgoing Outputs are freed, because a third roster doesn't fit ([§20](20-budgets.md#20-budgets-and-measured-facts)).
- **Stop** commits in any phase. The pending Recipe becomes Current, the tempo jumps to its value, and the next PLAY starts the new genre fresh ([§13.2](13-perform.md#132-transport-start-and-end)). A preload still running carries on, and PLAY waits for it. The outgoing Outputs are freed as in Retiring.
- **Leaving Perform** stops the transport, so it commits too: the Current Recipe that Compose sees afterwards is the new one. A preload still running is cancelled, and the incoming Outputs go with Perform's song.
- **Hard cut** lands in Groove ([§9.7](09-arc.md#97-transitions)).

### 12.10 Input during card loads

Audio and the tick hook keep running during card loads, and pad and button handlers are still called, with `sdRoutineLock` set.

- **Act at once:** actions that change only Recipe or steering state: macros, force, hold, Fill, mute and solo, part focus, Capture mark.
- **Wait:** actions that need the card or another build: genre change, Regenerate, Recipe LOAD or SAVE, a sound re-roll. They wait through the stock `REMIND_ME_OUTSIDE_CARD_ROUTINE` deferral.
- BACK still cancels a queued genre change. Gold knobs are deferred by the firmware to just after each card routine, so they steer with a few ms of extra lag.

Sources: [Generator scheduling][t19], [Transition lifecycle][t73], [Pushed Lean][t30], [POC gaps][t52], [Hardware tests of the Perform POC][t45], [Pitch model][t21], [Sub-step expression][t20], [Firmware changes the design needs][t43], [Firmware architecture of v1.3.0-rc3][t2], [Runtime budget on the Deluge][t3].

[t2]: https://github.com/gismya/deluge-generative/issues/2
[t3]: https://github.com/gismya/deluge-generative/issues/3
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t30]: https://github.com/gismya/deluge-generative/issues/30
[t43]: https://github.com/gismya/deluge-generative/issues/43
[t45]: https://github.com/gismya/deluge-generative/issues/45
[t52]: https://github.com/gismya/deluge-generative/issues/52
[t73]: https://github.com/gismya/deluge-generative/issues/73
