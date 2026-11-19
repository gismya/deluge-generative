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
  into a short Build plays on; it is counted where it ends (`ahead().start == 1` off a phrase line), since a Groove one
  phrase long is never the Section on a phrase line: counting at phrase lines missed it and such a song never ended.
  The third such Groove past the target ends on its phrase line; a third one phrase long plays its Build and End lands
  on the Drop (`kMaxShortBuildGrooves`). A fixed overrun in phrases didn't work: Ambient's Grooves run to 12 phrases.
  `arc.ahead()` takes the Recipe's Motion, as the core's calls do, not the effective one.
- The stream doesn't tag which layer moved a control, so the thinning can't treat drift apart from a Rise or a Throw.
  It works a bar at a time: a bar within `kBarTolerance` (16) of the line across it keeps only its end, else every step
  line; then a cone (`kNodeTolerance`, 2) merges what lines up. The drift spec turns gestures, Section offsets, the
  Drop lift and Drop-outs off, and skips a lane's bar with a snap and the bar after it: Dub's Siren release Throw is
  always on. Tolerances below 16 left two nodes in some bar of Ambient's Wobble at Motion 100.
- A bar is measured from the last point the cone took. After a long hold that point is bars back, and a ramp's start
  lies within 16 of the line to its end: the held bars then climb. So a ramp after a hold of a bar or more sends its
  start straight to the cone (`Track::move()`). Drift is not "every step": it holds a step or several often, and a bar
  or more now and then (all genres, both Motions). Sending every hold's end to the cone gave drift 2–3 nodes a bar;
  holds under a bar stay in the bar's buffer. The drift spec counts a ramp start after a bar's hold as the hold's.
- Lanes are found by an index, not a search: a set slot per (part, lane), given out in order of first use, and a
  track per control plus the lift in the slot. A set past `kMaxControlSets` is counted (`droppedSets()`), not written.
- `finish()` writes a lane's last node a tick before the song's end at the latest (a ramp on the Outro's last step
  ends on it) and counts note-ons still open as `unpaired()`.
- A lane starts at home (0) and is made only when a value leaves it, so a set with no delay setup gets no Throw-lift
  lane. A lane's first snap on the piece's first step is its base, not a move; a ramp there is a move from home, as
  Perform plays it. A moving lane's base is `Generator::centreLayer(part, lane, control)` (the macro layer at the
  Recipe's macros, `takesMacroLayer()`'s rule, the same as the core's), 0 on the lift.
- A sample Riser's note-on moves later in `ComposeDriver::timeRiser()` (`perform::riserStart()` at the Recipe's tempo,
  from `Generator::riserDropTick()`, shared with `PerformDriver::holdRiser()`), asked through `SongWriter::sampleMicros()`
  as Perform asks its `NoteSink`. The harness's `play()` sets Perform's tempo to the Recipe's, or a sample Riser there is
  timed at the driver's default tempo and the equivalence fails.
- `ClipBuilder` holds a sounding-note table (`SoundingNotes::kTotal`) and `kMaxTracks` lanes, each with a bar of
  buffered points (2 B each): it lives in the Compose job, not the Generator block, so it isn't under H18.
- Harness traps: CppSpec's `it` is a macro, so a local named `it` doesn't compile in a spec; `expect()` works only
  inside an `it` (helpers return values); a group inside a `describe` is `context(...)`: a nested `describe(...)`
  compiles, prints its title and runs nothing, so the spec passes empty.

## The firmware adapter and the build (`generative/firmware/compose_*`)

- `ComposeSong` (`compose_song.h`) is the firmware's `SongWriter`; `compose_job.cpp` is the modal runner job around it
  (`beginCompose()`, `composeProgress()`, `composeTooLong()`). The job reuses `buildPerformSong()` for the Outputs (the
  sounds, their settings and a 1-bar clip each), then writes into those clips: one clip per part, `loopLength` set
  only at `end()`. Nothing in the write path reads the clip's length (notes go in by hand, nodes by `setNodeAtPos()`).
- Notes arrive in note-off order, so `attemptNoteAdd()` (which refuses an overlap with the note on its left) doesn't
  fit: `ComposeSong::addNote()` inserts in position order and cuts the earlier of two overlapping notes where the
  later starts. A synth row is made with `insertNoteRowAtY()`, never `getOrCreateNoteRowForYNote()`, which changes the
  song's scale for a note outside it (`off_scale=` counts those; none so far).
- Automation goes through `soundControls`, Perform's own instance, which Compose borrows (Compose never runs while
  Perform is active): `begin()` binds the private roster and reads the homes, `writeNode()`/`writeBase()` write each
  bound param, `repatch()` tells each Sound after the bases. `ComposeSong::end()` calls `forget()` before freeing a
  silent part: test builds' `freePartOutput()` refuses an Output a control set is still bound to.
- A delay setup's feedback param is written only from the Throw's lift lane (Space's share at home); the Space lane
  skips it (`forEachComposed()`). Two lanes writing one param would interleave their nodes.
- The song needs a current Clip (the first part's), or CLIP in its session or arranger does nothing and the probe's
  `start_state` can't reach a clip view.
- A build that fits only alone swaps in a blank song from inside the job (`blank_song.h`, shared with the leave).
  Set the UI for it at once (`setUIForLoadedSong()`), as Clear song does, and open COMPOSING again: left under the
  old root UI (an instrument clip view over the deleted song), delugemu hung at the next BACK.
- Memory figures (`gen_compose ... used_kb= setup_kb=`) count the general heaps only (internal and SDRAM's general
  regions): the sample cache lands in the stealable region and swung `used_kb` by 800 K between sound rolls when it
  was counted. The reserve check counts SDRAM's general regions and the stealable region's free space, stopping as
  soon as it has the reserve, so it walks a few records a bar.
- Stock traps found here: arranger playback read `output->recordingInArrangement` on rows below the last Output (a
  song with under 8 tracks faulted under the test builds' null guard), and the save UI offered `SONGn` to a song with
  a name but no file. Both fixed; rows in `docs/upstream-candidates.md`.
- Test-port lines for scripts: `compose <seconds> [<genre>]` (opens COMPOSING and starts the job, until the Compose
  screen has BUILD), `save song <name>` (SONGS/<name>.XML through the stock save UI), and the holds `compose` (waits
  after the first bar), `compose_alone`, `compose_reserve`.
