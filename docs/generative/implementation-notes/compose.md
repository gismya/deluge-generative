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
  (`beginCompose()`, which opens COMPOSING itself after its refusal checks, `composeProgress()`, `composeTooLong()`). The job reuses `buildPerformSong()` for the Outputs (the
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
- `ComposeJob::finished()` closes COMPOSING itself, before the caller's `ComposeEnded`: a caller that forgot to left
  the modal up over the new song.
- Every swap goes through `swapInSong()` (`blank_song.h`), which takes down the "Loading" animation `doSongSwap()`
  puts up. Without it the animation covered COMPOSING after the alone path's blank song, and stayed up for good after
  a cancel there. A check at the job's end can't see it: stock instrument loads in `buildPerformSong()` take it down
  as a side effect, so the probe reads it at `gen_compose_alone working=`. `gen_compose ... working=` proves
  little when the player's song wasn't freed (`alone=0`), for the same reason. The blank song's swap keeps "Loading"
  up until its `loadAllSamples()` is done, then takes it down (`detail::endSwapLoading()`).
- The blank song is in `preLoadedSong` from its `new`, before `ensureAtLeastOneSessionClip()`, as Clear song: the
  preset load reads `preLoadedSong`'s tempo for its default sync levels (`ArpeggiatorSettings`, sidechain), with no
  current song to fall back on. A built song (Compose, Perform) goes in only at the swap.
- A cancel check sits right after the genre roll, before `fitMemory()`: BACK during the roll's last card read must
  not reach `freePlayerSong()`. Work that can't be undone (the player's song freed) needs a check right before it.
- Memory figures (`gen_compose ... used_kb= setup_kb=`) count the general heaps only (internal and SDRAM's general
  regions): the sample cache lands in the stealable region and swung `used_kb` by 800 K between sound rolls when it
  was counted. The reserve check counts SDRAM's general regions and the stealable region's free space, stopping as
  soon as it has the reserve, so it walks a few records a bar.
- Stock traps found here: arranger playback read `output->recordingInArrangement` on rows below the last Output (a
  song with under 8 tracks faulted under the test builds' null guard), and the save UI offered `SONGn` to a song with
  a name but no file. Both fixed; rows in `docs/upstream-candidates.md`.
- Test-port lines for scripts: `compose <seconds> [<genre>]` (opens COMPOSING and starts the job, until the Compose
  screen has BUILD), `save song <name>` (SONGS/<name>.XML through the stock save UI), and the holds `compose` (waits
  after the first bar), `compose_alone`, `compose_reserve`, `compose_roll` (waits after a genre roll), and `golden`
  (#141: a genre roll makes the golden case and the build starts in the Groove, for `gen_emu.py golden`).

## The Compose screen (`gui/menu_item/generative/compose_screen.*`)

- One stock `Submenu` (`composeScreen`) opened with `soundEditor.setup(nullptr, &composeScreen)`; `openComposeScreen()`
  does it (COMPOSE FROM's rows, and the test port's `compose screen`, the only way to it with no Recipe).
  `showComposeScreen()` is how a source row shows it: from the screen's FROM it goes back up to the screen, from
  COMPOSE FROM it closes the sound editor and opens it again on the screen, so BACK on the screen leaves rather than
  going back to COMPOSE FROM. COMPOSE FROM and FROM share their GENRE ▸ (`composeGenres`) and FILE... items: a
  stock `Submenu` keeps no parent, so one item can sit in two lists. Its text (title, SCALE's short names, LENGTH's
  `m:ss`, a pick's file name, `DRUMS: 8 LANES`) is portable, `generative/compose/compose_screen.*`, with
  `compose_screen_spec`. Lane names are `generative/core/lane_names.h`, shared with the Kit rows (`gen_sound` builds
  from it, so the scripts that expect `gen_sound` cover it).
- The title draws itself (`ComposeScreen::renderOLED()`) and side-scrolls on scroller 1: scroller 0 is the selected
  row's, which `Submenu` sets up.
- KEY and SCALE edit in place: the row is an item the sound editor enters (`InPlaceRow`), which draws the screen
  (`composeScreen.renderOLED()`, still focused on that row) and hides its own value on alternate `UI_SPECIFIC` timer
  ticks. Its `selectButtonPress()` returns nullptr, so a push goes up a level as BACK does. `beginSession()` sets the
  transition direction to 0, or the stock slide plays.
- A timer that blinks something must not re-render the screen: `renderUIsForOled()` clears the image, which stops the
  side-scrollers, and `setupSideScroller()` starts each one again from its start after a 400 ms pause, so at a 300 ms
  blink the title never moves. `InPlaceRow` keeps the y its row was drawn at and redraws only the value's box (clear,
  draw, invert as the highlight does, `markChanged()`).
- Stock `Submenu`s have fixed item lists, so SOUNDS holds a synth row and a Kit submenu per part slot, one relevant
  (as GENRE ▸ does). The Kit submenus share one list of lane rows, which read the part whose submenu began last
  (`openKitPart`). Sound rows show `NAME PICK` as their name, with no right-aligned value, so a long one is cut and
  side-scrolls when selected, as a stock row does.
- Card-reading edits (a genre pick, REGENERATE, a sound re-roll) are `RecipeRoll`s (`firmware/compose_edits.*`): a modal
  job under `generativeModalUI.open()`, over the sound editor. Their `ended` runs in the job's task and may move the
  sound editor (a genre pick goes back up to the screen). The previous picks and the re-roll's `before` copy live in
  SDRAM (2.7 KB each); `CurrentRecipe::rerollPart()`/`rerollLane()`/`regenerate()` stage into its own `stagedSounds_`.
- State kept outside the Current Recipe about it (the previous picks) goes stale when the Recipe is replaced anywhere,
  Perform's genre change and REGENERATE included. It records `CurrentRecipe::generation` and is void once that moved
  on. Anything that replaces the whole Recipe (a Recipe file's load) calls `currentRecipe.replaced()`;
  `forgetCurrentRecipe()` keeps counting across its memset, or a later Recipe would match the forgotten one's.
- A context menu or the working modal over the screen begins the screen's session again when it closes, with no
  `navigatedBackwardFrom`. `openComposeScreen()` marks a new session, so `gen_compose_screen opened` (and
  `gen_compose_recipe opened`) come once; a re-focus reports `gen_compose_recipe shown`. `endSession()` can't mark the
  end: the sound editor also ends the screen's session when it enters a child row.
- BUILD's DISCARD SONG? leaves the sound editor open under COMPOSING, so a cancel lands back on the screen. On Done
  and on the fits-alone path the job's `setUIForLoadedSong()` replaces the UI stack. On Done it runs before
  `generativeModalUI.jobEnded()`: closing COMPOSING first re-focuses the sound editor over the swapped-in song, its
  items still pointing into the deleted one.
- Test events: one line would pass the test port's 120-character event (it is cut, not dropped): `gen_compose_recipe`
  and `gen_compose_seed` split the Recipe between them.

## Capture's replay (#145)

- `ComposeDriver::beginReplay(generator, piece, writer, splitBudget)` replays one piece's span (to `endStep()`, or
  `cutAt()` if earlier); `run()` applies each step's records through `Generator::decideReplayed()` (a template taking a
  `next(LogRecord&)`), reading them with `SteeringLog::Piece::Reader`. A hard cut's Transition sits on `endStep()`
  itself: the replay applies it there after the span (`endsAtTransition_`). It never stops on `ended()`.
- Records stamped on step n but written after its decision (TransitionTempo, SoundDropped) are read before n's
  decision in replay: harmless, apply() keeps nothing for them, and a drop then reaches `dropped_` before the Sound event
  at stepTick(n).
- Driver-side commands go to the writer from `ComposeDriver::replayed()`; Sound/KeyScale/Section from `shape()` in
  `writeDue()`. The SongWriter's Capture calls (`cutClip`, `soundPick`, `newTrack`, `tempo`, `paramEdit`, `stutter`,
  `nextSong`) default to nothing for Compose; `ComposeSong` implements them for Capture (below).
- Split: `ClipBuilder::split()` ends notes still open (a Sustain) at the line and re-opens them there, and restarts each
  lane from its value with `Track::origin` as its song start (the base rule `tick == origin`). The size check uses
  `estimateBarsBytes()` plus a cut clip's or new track's setup (`songExtra_`).
- Spec traps: `PieceStart` holds a whole Recipe, so a hand-built log needs `sizeof(PieceStart)` bytes beyond its
  records, or `beginPiece()` opens nothing and `piece(0)` reads garbage (a segfault with no CppSpec output). A crashing
  spec prints nothing: read the backtrace from `~/Library/Logs/DiagnosticReports/generator_specs-*.ips` (lldb hangs
  here).

## Capture's build on leave (#146)

- `firmware/capture_job.cpp`: one modal job builds every kept piece (`perform::nextKept()`), behind the teardown:
  `LeaveJob::finished()` calls `beginCapture()` after its own `ended_` (which closes the teardown's modal; opened
  before, the capture screen was closed by it). The log outlives `freeBlocks()` while `PerformAdapter::keepLog_`;
  the job calls `freeLog()` at its end. `finishCompaction()` first: indices of kept pieces don't move with it.
- A piece's song: `buildPerformSong()` from the header's picks (palette: the Current Recipe's for its genre, else
  `stagePalette()`), `restoreSnapshot()` (#441's) before `ComposeSong::prepare()`, which reads the homes from the
  restored values. Swapped in, saved (`song_file.cpp`, F13), and freed by the next song's blank swap. After every swap
  `setUIForLoadedSong()` and the capture screen opens again, as Compose's fits-alone path does.
- `ComposeSong` keeps per-part clip segments (`Segment`, `kMaxSegments`): a note goes to the segment its start tick
  falls in (notes arrive at their note-off, maybe after a cut); a node goes to the newest, because the driver ends the
  part's lanes before each `cutClip()`/`newTrack()` (`ClipBuilder::cutPart()`): without it the bar buffer delivered
  nodes for the old clip after the cut. A cut clip is `setAudioInstrument(..., favourClip = old)` (params cloned,
  no automation) and `SoundControls::moveToClip()` (homes kept); a new track is `buildPartOutput()` from the
  re-roll's picks (card reads inside the replay, yielding through the job's `Chunks`) and `rebindPart()`.
- SetHome: the driver calls `ClipBuilder::endPart(hold)`, `paramEdit()`, `resumePart(restate)`, so the node after the
  home moves is written from the new home (`SoundControls::moveHome()`). A track started after a cut or split starts
  at `partStart_`, not tick 0 (a #145 split bug this fixed).
- Splits: `ComposeSong::nextSong()` calls `CaptureHost::nextSong()`: the job saves the song, builds the next from
  `playingPicks()`, restores the header snapshot for parts not re-rolled and `restoreEdits()` (each param's newest
  SetParam value or SetHome home), then `restart()`. No probe reaches a split (it needs a 2 MB piece): harness only.
- Probe scripts that leave Perform take NOTHING: `perform_leave.txt` turns to the end and back one (NOTHING or LEAVE
  is always before CANCEL). One detent a line: a stock context menu refuses a turn that would pass its end, so
  `encoder 3 5` left it on LAST and every start_state leave ran a Capture and wrote a song to the card. The first
  option after a piece is a Capture: a script that leaves any other way writes to SONGS/.

