# 12. Generator runtime

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 12.1 The tick hook

- One hook in `Session::doTickForward`, after the per-Output arp loop (`playback/mode/session.cpp`, F1), calls the Perform driver. The driver calls `generator.tick(...)`, which returns the ticks until its next event and lowers `playbackHandler.swungTicksTilNextEvent`, as the arp does.
- The Generator sends notes with `sendNote` on Synths and `noteOnPreKitArp` on Kits, the same entry points clip playback uses from the tick. Notes never go into Perform's clips (ADR 0002).
- Perform must stay in session playback (`lastClipInstanceEnteredStartPos = -1`).
- The Generator works in **swung ticks**, so song swing applies for free. Its unit is fixed at **24 ticks per 16th**, whatever the song's magnitude. Every offset in the event stream is in that unit, Lean, flam gaps and ratchet spacing included, and each driver converts its song's ticks to it ([§12.3](#123-the-drivers-clock)).
- The hook loops over running instances: one, or two around a transition ([§12.9](#129-instances-and-the-transition)).

### 12.2 When steps are decided

- The Generator decides each 16th step **12 ticks before its grid line**, in every genre (ADR 0003). Ticks between decisions only read the event buffer.
- The fixed event buffer holds the step being decided as well as the step playing.
- Steering reaches the core through the steering queue, drained at each decision ([§2.6](02-architecture.md#26-steering-into-the-core)). Continuous steering takes effect from the next undecided step: about 63 ms more latency at 120 BPM (625/BPM ms per tick) on top of the wait of up to one step. So do "now" actions such as SHIFT+mute and hold.
- Bar- and phrase-quantised actions (mute, solo, force, take, key change) wait in the core's pending list. A press reaches a line only if it arrives before that line's decision tick, so 13 or more ticks ahead of it. One 12 ticks ahead or later lands on the following boundary.
- If step cost turns out too high on hardware, the fallback is to plan Arc and Section decisions at phrase lines in task context and keep only note decisions in the tick. Timing never changes the result: the plan is a pure function of core state at the line's decision. Task context may precompute it from a snapshot, but the tick uses that result only if it is ready and no steering has been drained since the snapshot. Otherwise the tick computes it inline and takes the cost once. The mechanism waits until H1 says to adopt the fallback ([§20](20-budgets.md)).

### 12.3 The driver's clock

- The driver keeps its own clock. On every hook call it adds the swung-tick delta, shifted from the current magnitude to the Generator's 24 ticks per 16th, with the remainder carried. It never divides (the POC's 64-bit divide per call must not return) and never works out time from an origin, so a magnitude change only alters the clock's rate.
- A delta that is negative or longer than a bar counts as a **jump**, handled like a mid-play SPP ([§12.4](#124-external-clock)).
- Tempo magnitude matching stays as the player set it, as in a stock song. It settles within the first beat after an external Start.
- Perform, Compose and Capture songs are created at **magnitude 2** (24 song ticks per 16th), whatever the player's Resolution default, so Compose and Capture place every event at its exact tick. `Song()` takes the player's default, so the driver sets it explicitly. The Recipe's swing interval is a note value (16th, 8th…), mapped to a sync level at magnitude 2.
- An external clock's magnitude matching can still change Perform's magnitude mid-play, so the driver never assumes 2 at runtime. When the magnitude is lower, an event plays at the first hook call whose driver clock has reached it: less than one song tick late, never early. The event stream doesn't change, only when Perform plays it.

### 12.4 External clock

- **Start:** step 0 lands at tick 0, the normal Perform start.
- **Continue and SPP:** the driver tracks the SPP offset itself, because the firmware's song counter doesn't follow SPP. It stays silent until the next master bar line and puts step 0 there.
- **Mid-play SPP or a jump:** note-offs at the jump, then resume at the next master bar line. The Arc carries on where it was and is never rewound by SPP. The Generator skips the rest of its own bar, deciding nothing, and its next bar line plays on the master's. The hook applies an SPP at its next call; only the song ticks since the SPP arrived count after the jump, not those before it. A clock scale change mid-piece is a jump too, once magnitude matching after an external Start has settled.
- Tempo under an external clock is [§13.7](13-perform.md#137-sound-editor-midi-and-external-tempo).

### 12.5 Mute gate and note-offs

- A muted part is computed every step exactly like an audible one. The core applies mute and solo as a **muted flag** on its note-ons, so the event stream still holds them ([§2.2](02-architecture.md#22-drivers-and-the-event-stream)). Perform drops flagged note-ons at the output, and Compose and Capture leave them out. Notes already sounding play to their note-off.
- A Drop-out works like the mute gate: no note-ons, and sounding notes play to their note-off, so a thrown hit rings ([§8.8](08-rhythm-and-expression.md#88-drop-out)).
- A Rest is different: when it starts, the Generator sends note-offs for the notes sounding on its pitched parts, and leaves drum one-shots to play out ([§8.7](08-rhythm-and-expression.md#87-fill)).
- Firmware clip mute and solo are never used in Perform. The firmware has no session-level Output mute, and an inactive clip stops param ramps.
- **Section lines.** At every Section line the core cuts every sounding note: a note-off at offset 0 of the Section's first step, before that step's note-ons. Notes still meant to hold are re-struck by the late-entry rule ([§5.6](05-parts-and-lanes.md#56-gates)): a note whose remainder reaches the end of the first bar starts again on the line with the Energy velocity at that moment, and a shorter remainder is dropped. Because the core does it, every driver gets the same stream and Compose clips stay self-contained. Perform re-attacks a held pad or drone at the line, and a LEGATO slide across it becomes a fresh note with no glide. Drum and FX one-shots under 2 s ignore the note-off and play out.
- Direct `sendNote` calls aren't tracked by the firmware, so the core keeps a fixed table of sounding notes per part and sends every note-off as an event. Notes in the stream carry no length ([§2.7](02-architecture.md#27-buffer-sizes-and-overflow)).
- On stop, Perform exit, a jump, or retiring an Output, the driver asks the core to flush, which sends note-offs for everything in its table. A key change or Section start the core has decided but nobody has heard yet (the 12-tick lookahead) still plays at a jump, because the piece carries on in it. At a stop it is dropped, cancelled with the rest of the pending list ([§2.6](02-architecture.md#26-steering-into-the-core)). The driver then sends all notes off on each part's Output as a safety net. There's no Output-level `allNotesOff`: Kits need `Sound::allNotesOff` per drum, as the POC does. Tails ring out.

### 12.6 Voice starts

- The firmware starts at most 4 voices per render window (1 under heavy load). Extra starts aren't dropped: they wait for the next render, at most 2.9 ms later (the routine targets 1.45 ms). At 130 BPM a render is shorter than a tick, so the budget is per tick.
- Each step sends note-offs first, then note-ons in role priority: Drums (Kick, then Snare and Clap, then the rest), Bass, Lead, Chords, Pad, FX. Ornament hits start last, so under load a flam's grace hit is the one that waits.
- Compose has no voice cap and writes every hit. That stays within the contract, which covers the event stream, not audio.

### 12.7 Tempo and swing glide

- While a glide runs, the driver follows a linear curve, asking for a tick every tick, and writes song tempo on each tick where the value changes. `Song::setTempoFromNumSamples` is public (call it with `shouldLogAction = false`, or it logs an undo action each tick); Perform calls it through `Song::setBPMInner`, made public for it (F14). Stock already changes tempo inside the tick.
- Swing changes only on step boundaries. An Instant glide lands on the next one. The swing interval changes at once, as stock does.
- External tempo is first sampled on the first bar line after step 0, once the firmware's measure of it has settled.
- Tempo glide is off while following an external clock. Swing still glides, because the master doesn't own swing.

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
- The incoming instance decides from the start step on, so its first decision is 12 ticks before it ([§12.2](#122-when-steps-are-decided)). The outgoing instance decides no step at or after the start step (`stopDecidingAt`, [§2.8](02-architecture.md#28-queries-from-task-context)). The hook keeps draining its event buffer (note-offs, Lean-late hits, the Throw ramp) until `isDrained()`, then that instance goes idle.
- The two overlap only between the incoming instance's first decision and the outgoing buffer running dry. Running two for a crossfade is out of scope ([§1.2](01-scope.md#12-out-of-scope-for-v1)).

**Incoming Outputs.**

- At the end of Preloading, in task context, each incoming Output is linked into Perform's song with its empty, always-active, 1-bar clip ([§12.8](#128-performs-song-at-runtime)) and, for a Kit, its NoteRows. This follows stock `SessionView::createNewInstrumentClip`: `addOutput`, then `setActiveClip`.
- Each Output is linked in one runner chunk ([§12.10](#1210-long-running-work-and-input)): its clip, its NoteRows, `addOutput` and `setActiveClip`, with no yield inside, so an incoming Output never renders half-built. The runner yields between Outputs. The largest link measured 1.6 ms ([Measure silent incoming Kits and retiring Outputs on a Deluge][t83]). Linking allocates, so it never happens in a tick.
- No notes go to them until the start step. Perform never uses firmware solo or section launch, so their clips can't be muted or launched by accident.
- A silent Synth skips rendering. A silent Kit still runs its FX chain every buffer (`global_effectable_for_clip.cpp:111-161`), so up to two silent incoming Kits cost CPU through Queued and Committed. Measured, that's about 3.5% CPU with no late renders ([§20](20-budgets.md#20-budgets-and-measured-facts)), so all Kits are linked at the end of Preloading.

**Retiring the outgoing Outputs.**

- An outgoing Output is freed once it is silent: `skippingRendering` for a Synth, and for a Kit no active drums (`drumsWithRenderingActive` empty), its delay's `repeatsUntilAbandon` at 0, no arp or stutter, and no `NonAudioDrum` voices. On hardware this check reported silence within 20 ms of the tail's end and never flipped back ([§20](20-budgets.md#20-budgets-and-measured-facts)).
- The cap is one phrase after the start step. At the highest delay feedback an echo never dies (`delay.cpp:187`), so an Output still sounding at the cap fades its volume over 1 beat, gets note-offs ([§12.5](#125-mute-gate-and-note-offs)), and is freed.
- Freeing runs in task context, outside the card routine, through the runner ([§12.10](#1210-long-running-work-and-input)), because deleting a Kit yields once per drum. It removes the clip with `Song::deleteClipObject` and `InstrumentRemoval::DELETE`, because the default only hibernates an Output the user has edited.
- Freeing keeps the stock order, the one `SessionView::removeClip` uses when a user deletes a clip while the song plays. The clip detaches from the Output first: for a Kit, `unassignAllNoteRowsFromDrums` yields once per NoteRow through `AudioEngine::routineWithClusterLoading()`, which runs only audio. Only then does `deleteOutputThatIsInMainList` unlink the Output. So the Output renders with no active clip through about one yield per NoteRow (12 for an 11-lane Kit, 1 for a Synth). That's safe because it is already silent, or faded to 0 at the cap, and nothing but the render runs in those yields. On hardware: 0 late renders and at most 0.93 ms between yields ([§20](20-budgets.md#20-budgets-and-measured-facts)). Linking is atomic and freeing isn't, on purpose. Unlinking first would need a removal path that no stock code takes, and it gains nothing audible.
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

### 12.10 Long-running work and input

**The runner.** All long work in Generative mode goes through one **runner**, which runs one job at a time. The jobs:

- Perform entry build ([§14.3](14-entry-and-compose-screen.md#143-replacing-the-song))
- transition preload, ending with linking the incoming Outputs ([§12.9](#129-instances-and-the-transition))
- sound re-roll preload ([§10.7](10-sounds.md#107-when-sounds-load))
- freeing retired Outputs ([§12.9](#129-instances-and-the-transition))
- Compose build ([§15.7](15-compose-output.md#157-build-and-handover))
- Capture build and save ([§17.7](17-capture.md#177-building-on-leave))
- tearing down a cancelled private song, or Perform's song on leave

The runner has one active job and one **next** slot, which holds at most one waiting job. A job starts only once the one before it has finished or been torn down. Only one card load can run at a time anyway ([§10.7](10-sounds.md#107-when-sounds-load)).

**How a job runs.**

- Each job runs as its own one-off scheduler task (`addOnceTask`), as ordinary blocking code. It is never run inside the button handler that started it, so the buttons task isn't on its stack and can't be re-entered from inside its own handler.
- It calls `yieldToIdle()` between chunks on a time budget of about 2 ms; the scheduler logs a run over 3 ms as too long. `yieldToIdle()` runs every task that is due, then returns: audio, the tick, buttons, encoders, MIDI and the PIC run between chunks, as they do while stem export runs. Not `yield()`, which runs only one task: in a build of a few chunks the buttons task may get no turn at all, so BACK would wait for the build to end (seen in delugemu on the M1 runner).
- Card reads use the stock loaders, which wait through `routineForSD()` with `sdRoutineLock` set. A preset or Kit load is never split into steps.
- No job calls `AudioEngine::routineWithClusterLoading()` to stay alive: it runs only audio, so a job that yields only through it never sees BACK.

**Busy flag.** While a job runs or one waits in the next slot, Generative mode is **busy**. Generative handlers never return `REMIND_ME_OUTSIDE_CARD_ROUTINE`, because one deferred press puts its edge back at the head of the queue and blocks every later press until the whole build ends (H8, [Measure input during card loads on a Deluge][t61]). They check the busy flag instead:

- **Transition requests** (genre change, Regenerate, Recipe LOAD confirm) during Preloading or Queued cancel the current transition and go into the next slot. If several arrive, the latest wins ([§12.9](#129-instances-and-the-transition)).
- **PLAY while a preload runs**, after Stop has committed the transition, sets a "start when loaded" flag. Playback starts when the preload ends.
- **Every other action that needs the card or the runner** is refused with a popup and never queued: Recipe SAVE, opening the LOAD browser, a sound re-roll. A refused press is never held back and re-run later.
- **Act-at-once actions** run as soon as their handler is called, inside a card wait too. These are actions that change only Recipe or steering state: macros, force, hold, Fill, mute and solo, part focus, Capture mark, and BACK.

**Cancel.**

- Cancel is a flag. The job checks it at each chunk boundary and after each card read returns.
- The UI reacts at once: the popup goes, or the screen goes back. The job then unwinds through return codes (the firmware has no exceptions) and frees what it built inside the same job, also in chunks. The runner counts as free only once that's done, and then the next-slot job starts.
- The worst-case delay before teardown starts is one card read plus one chunk.

**Modal and live jobs.**

- Perform entry, Compose builds and Capture builds are **modal**. Only BACK acts. The current song keeps playing untouched, with its pads and knobs ignored.
- Perform preloads and freeing retired Outputs are **live**. The Perform page works as usual, under the busy-flag rules above. While a preload runs, the Perform page shows the stock working animation (the 17×9 icon, top right), so a refused SAVE or re-roll makes sense. Freeing retired Outputs shows nothing.

**Leaving Perform** cancels the active job and empties the next slot. Tearing down Perform's song (both rosters and both Generator blocks) is a job of its own. Capture's builds from the leave menu ([§17.7](17-capture.md#177-building-on-leave)) wait in line behind it, so a Capture build never shares memory with Perform's song.

**Gold knobs.** Between card reads, the gold knobs act at once. During one card wait the firmware skips the mod knobs while `sdRoutineLock` is set (`interpretEncoders()`). The hardware counts the edges, so a turn lands late but isn't lost. The lag is up to one card wait, about 6–12 ms per sample in H8. This is accepted, rather than changing the firmware to run the knobs inside `routineForSD()`, where a knob could touch a sound that is only half loaded.

**Acceptance test.** Re-run H8 against the real runner during a transition preload while Perform plays: act-at-once inputs and BACK within 20 ms, gold-knob lag no longer than one card wait, no late renders, and a card action pressed mid-preload refused or replaced, never blocking later input.

Sources: [Generator scheduling][t19], [Transition lifecycle][t73], [Long-running work and input during loads][t74], [Measure input during card loads on a Deluge][t61], [Pushed Lean][t30], [POC gaps][t52], [Hardware tests of the Perform POC][t45], [Pitch model][t21], [Sub-step expression][t20], [Firmware changes the design needs][t43], [Firmware architecture of v1.3.0-rc3][t2], [Runtime budget on the Deluge][t3], [Interface between the Generator core and its drivers][t75], [Determinism details: tick unit, tempo, held notes and ramps][t77].

[t2]: https://github.com/gismya/deluge-generative/issues/2
[t3]: https://github.com/gismya/deluge-generative/issues/3
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t30]: https://github.com/gismya/deluge-generative/issues/30
[t43]: https://github.com/gismya/deluge-generative/issues/43
[t45]: https://github.com/gismya/deluge-generative/issues/45
[t52]: https://github.com/gismya/deluge-generative/issues/52
[t61]: https://github.com/gismya/deluge-generative/issues/61
[t73]: https://github.com/gismya/deluge-generative/issues/73
[t74]: https://github.com/gismya/deluge-generative/issues/74
[t75]: https://github.com/gismya/deluge-generative/issues/75
[t77]: https://github.com/gismya/deluge-generative/issues/77
[t83]: https://github.com/gismya/deluge-generative/issues/83
