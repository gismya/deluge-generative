# Implementation notes: Compose driver (`generative/compose/`)

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- The portable Compose driver (spec §2.5, §15) is two classes. `ComposeDriver` (`compose/compose_driver.h`) starts the
  Generator from Start, steps it (`run(steps)`, a chunk a call, for the runner job's yields), queues End and feeds
  every event, in play order, to a `ClipBuilder` (`compose/clip_builder.h`), which pairs notes and thins the Control
  events into nodes for a `SongWriter`. The harness's `RecordingWriter` (`compose_driver_spec`) and the firmware's
  Song adapter (#136) implement `SongWriter`; its comment gives the call order.
- Compose plays as Perform plays: before each decision it writes what falls due before that step's decision tick
  (`decisionTick(next) - 1`), as `PerformDriver::decideDue()` does, and ends as `advance()` does at the Outro's end:
  write due to the end line, `flush(Stop)`, write the rest. A change to how Perform drains or ends changes Compose's
  stream too: `compose_driver_spec`'s equivalence fails if the two part.
- End goes on the queue just before deciding the phrase line where a Groove ends by itself (`Arc::ahead().start == 1`
  from the last decided step), so it lands there and the Groove's last phrase is decided exactly as in Perform: End
  waiting earlier would change it (`lastGroovePhrase()` reads the pending End, and so do the fills). A Groove that
  never ends (Motion 0, `kNotAhead`) ends on its first phrase line past the target. A Groove that ends at a bar line
  into a short Build plays on; the third such Groove past the target ends anyway (`kMaxShortBuildGrooves`). A fixed
  overrun in phrases didn't work: Ambient's Grooves run to 12 phrases.
- The stream doesn't tag which layer moved a control, so the thinning can't treat drift apart from a Rise or a Throw.
  It works a bar at a time: a bar within `kBarTolerance` (16) of the line across it keeps only its end, else every step
  line; then a cone (`kNodeTolerance`, 2) merges what lines up. The drift spec turns gestures, Section offsets, the
  Drop lift and Drop-outs off, and skips a lane's bar with a snap and the bar after it: Dub's Siren release Throw is
  always on. Tolerances below 16 left two nodes in some bar of Ambient's Wobble at Motion 100.
- A lane starts at home (0) and is made only when a value leaves it, so a set with no delay setup gets no Throw-lift
  lane. A lane's first value on the piece's first step is its base, not a move. A moving lane's base is
  `Generator::centreLayer()` (the macro layer at the Recipe's macros), 0 on a Drums lane's row and on the lift.
- A sample Riser's note-on moves later in `ComposeDriver::timeRiser()` (`perform::riserStart()` at the Recipe's tempo),
  asked through `SongWriter::sampleMicros()` as Perform asks its `NoteSink`.
- `ClipBuilder` holds a sounding-note table (`SoundingNotes::kTotal`) and `kMaxTracks` lanes, each with a bar of
  buffered points (2 B each): it lives in the Compose job, not the Generator block, so it isn't under H18.
- Harness traps: CppSpec's `it` is a macro, so a local named `it` doesn't compile in a spec; `expect()` works only
  inside an `it` (helpers return values); a group inside a `describe` is `context(...)`: a nested `describe(...)`
  compiles, prints its title and runs nothing, so the spec passes empty.
