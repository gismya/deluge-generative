# Implementation notes: Timing

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- The Generator decides each step 12 ticks ahead (`kLookaheadTicks`, ADR 0003, §12.2). Everything the UI asks
  (`partMuted()`, `sectionForced()`, `planArc()`) describes the last decided step, about 45 ms before it is heard: a
  mute pad changes colour that early.
- The driver plays as it decides, so events due before each decision play first (`perform/perform_driver.cpp`). Every
  jump calls `NoteSink::jumped()` once, after its note-offs.
- Read the tick magnitude from `currentSong->getInputTickMagnitude()` on every tick: an external clock changes it
  mid-play. The Generator's own is `kGeneratorTickMagnitude` (`core/types.h`).
- At a transition's start step the driver's clock and the playing instance's steps start again from 0 (the new
  roster's step 0), and the outgoing instance's events play at their tick less the start step's. Anything the
  adapter counts in steps of the playing instance restarts there (`PerformAdapter::restartStepCounters()`). Anything
  the steering latches as "already sent" to an instance that a new one clears (a held FILL, a held KEYBOARD fader)
  goes in `PerformSteering::instanceStarts()`, which a start and a transition's start step both call (through
  `PerformAdapter::instanceStarts()`).
- A sample Riser's timing lives in the Perform driver, not the core (§9.5, §10.5): the core places the note from the
  Build's first step to the Drop, and `PerformDriver::playDue()` holds its note-on back (`holdRiser()`) when the sink
  says it's a sample (`NoteSink::sampleMicros()`, the adapter's `OutputSink`: the row's first range at the system
  rate, for a ONCE or CUT row). `riserStart()` (`perform/riser_timing.h`) walks back from the Drop a beat at a time at
  `plannedTempo()`: a running glide, then a transition's planned glide across its Build. Each change to that plan
  (`setTempo`, `glideTempo`, the transition's window, a cancel) times it again; a note-off that comes first (a forced
  Section, a jump) drops it. Under an external clock (`NoteSink::tempoFollowsMaster()`, read each `advance()`) the
  plan is the tempo last set, the master's as `sampleExternalTempo()` samples it, and `glideIntoTransition()` plans
  no window: nothing glides (§9.7). A mute landing while it waits doesn't stop it (the core counts the note as
  sounding, §12.5). Tempo is driver-side (§2.2), so only the driver knows the plan, and the Generator block
  doesn't grow. The event stream is unchanged: the hold is at the output, like a mute.
