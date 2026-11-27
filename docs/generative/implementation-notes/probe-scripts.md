# Implementation notes: Probe scripts and the test port

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- Probe scripts: start every script with `start_state`, then Perform scripts with `include perform_open.txt` and
  `include perform_confirm.txt`. An expect's timeout needs the keyword, last on the line: `expect event gen_left
  within=15`. A bare trailing number is part of the match text (`expect event gen_section INTRO 0` matches section
  0). Over delugemu's DIN link test-port events can arrive seconds late: give `expect event` a long timeout before an
  `expect no_event`. SHIFT+SONG right after reaching session view needs about 1 s (the zoom animation holds a UI
  mode).
- A probe expect's `within=` counts from the expect line, the wait for the firmware's queue included (`no_event`
  alone watches its whole window after that wait). An expect after queued `wait`s and turns needs a window that covers
  them too (`perform_transport.txt`'s swing: 4 s of queue, `within=10`).
- `within=` is host seconds; an event's timestamp is the firmware's. Under host load delugemu runs well below real
  time (#381: 13.5 firmware s outran a 20 s wait at load 13-16), so a wait for something bars away needs about 3x its
  musical length. The H16 report comes every 4 bars on the phrase grid: the one on a Section's line goes out just
  before that Section's event, so an h16 expected after a Section event is the next one, 4 bars later
  (`perform_genre_change_dub.txt`, `_ambient.txt`: `within=40` at 70 BPM). A `since=mark` there would take the
  previous Section's report instead.
- Trap (#561): Techno's `BUILD` variant set (`#531`) includes `long` (16 or 32 bars, 1 in 24 runs draws 32 bars), so an
  unpinned DROP (`style=0`) out of any Techno subgenre can legitimately play a 32-bar Build: 58.4–62.2 s real time into
  Melodic techno (125→122 BPM), up to 81.0 s into Roots Dub (125→70 BPM), plus up to a phrase (15.4 s) waiting for the
  Build to start. Size `within=` on unpinned Techno DROP transitions for 32 bars (and ~3× under delugemu load).
- A fast gold turn (`encoder 4 N fast`) takes no time, so presses a slow turn used to space out can land in a
  double-tap window (`perform_homes.txt` waits 400 ms before pressing DRUMS again). Its 4-step edits don't always
  act as 4 one-step ones: stock deletes a mod cable inside the one-step edit that reaches 0, not after fast turns.
- Swapping a state expect for an event expect moves the event search: `exitUI()` lets go of a held row before
  `gen_leave_menu`, so the focus event after it needs `since=mark` (`perform_homes.txt`).
- Hold the BACK button or the End pad with `include perform_hold_back.txt` / `include perform_hold_end.txt`, not a copy
  of their three lines: their `held_ms=` bounds go up to 999 ms, as a card access can delay the menu or the arm by a
  few hundred ms (a 100 ms margin over delugemu's numbers would be flaky on hardware).
- run_script lets go of every hold when a script ends, also on an error (`hold entropy` left on pins every entry until
  a reboot). To compare events between two runs of something whose events depend on the card, `mark`, then `keep
  events TEXT`, `mark` again and `expect events kept` (`perform_entropy.txt`). Every Perform entry from GENRE rolls a
  fresh Recipe, the Current Recipe or not; PERFORM FROM's CURRENT RECIPE doesn't.
- A test-port hold (`hold NAME`, `release NAME`) is a row of `kHolds` in `io/midi/test_port.cpp` plus the hook that
  reads it. Nothing else needs a new hold added (the probe's `HOLDS_BEFORE_RELEASE_ALL` is frozen, for older builds):
  `start_state` sends `release all`, which sets every row off, so a row's `set(false)` must be safe when the hold was
  never on.
- A timing or voice-count figure (H16 `voices=`/`culls=`, H1's ns, `gen_ext_tempo bpm=`, the MIDI feedback rate)
  measures delugemu's speed under host load as much as the firmware: such expects failed beside other agents' runs
  and passed alone in about 20 PRs (#417). Write them `hardware_only expect ...`; `covers.py --check` fails one that
  pins a `covers.TIMING_FIELDS` field without it, so a new script gets told. Under delugemu a `hardware_only expect
  event` still waits for its kind (keep it starting with the kind), so a press after it comes no sooner than on the
  Deluge; a `hardware_only` no_event or state expect is skipped. It can't capture, and it moves the event search only
  where it matches: a `mark` right after one keeps a different place under delugemu. A field that any value matches
  (`voices=\d+`, `gen_h16 culls=`) pins nothing and needs no prefix.
- An `expect event` or `expect no_event` TEXT matches as one piece of the event's text: `gen_focus locked=-1`
  never matches `gen_focus part=-1 locked=-1`, so an `expect no_event` written that way passes whatever happens. Use
  the `/regex/` form (`/gen_focus part=-?[0-9]+ locked=-1/`) for fields that aren't adjacent, and run a new no_event
  on a build without the fix once, to see it fail.
- `mark` keeps the place of the last expect's match, not the moment of the `mark` line, and an `expect no_event`
  doesn't move it. After a long `expect no_event ... within=20`, `mark` then `expect event gen_phrase ... since=mark`
  matches a phrase that started inside that window, so "just after a phrase starts" was up to 20 s late
  (`perform_preset_browser.txt`'s cancel cases swapped before their BACK tap). Use `mark now` there: it keeps the end
  of the events received so far, once the firmware has sent what it queued.
- An `include`d script's `expect event` lines move the event search as the script's own do (#379). An event the
  include's steps caused before its last match is behind the search when the including script's next `expect event`
  runs, which waits for a later one and fails (#320's and #125's scripts hit it). To check such an event: `mark now`
  before the `include`, then `expect event TEXT since=mark` after it. `mark now`, not `mark`: it also skips older
  unmatched events. Each such expect needs its own `since=mark`, not only the first: a `since=mark` match doesn't
  move the search back to it, so the next plain expect still starts after the include's last match
  (`gen_generator_heap block=0` then `block=1`, both before `gen_perform_entered`, #388). The runner keeps one
  search on purpose: later expects count from an include's match
  (`perform_confirm.txt`'s `gen_perform_entered`).
- To check that nothing swaps on the line a cancelled sound would have landed on: `mark now`, the expects for the
  cancel's events, then `expect event gen_phrase bar= since=mark within=40` and `expect no_event gen_sound_swap
  since=mark within=1` (a swap comes about 0.06 s after its line's `gen_phrase`). The `mark now` goes right after the
  cancelling input, or for a preload parked by `hold preload` right after `release preload`, the first moment it
  could swap. Placed after the cancel's expects, it misses a swap that landed while one of them waited: with the
  cancel removed, the 5 s of a failing `expect event gen_sound_cancel` let the swap land first and the window passed
  (#314). A fixed `expect no_event gen_sound_swap within=20` had the same hole and cost up to 20 s each. A line that
  comes while the preload still runs is missed (the swap waits for the line after): about 0.4 s of a 15 s phrase.
- Each probe script names the paths it checks in `# covers:` lines (`scripts/generative/covers.py`), and
  `check_all.py` runs it only when one of them, the script or its includes changed. A new script, or one that starts
  checking another file, updates its lines. A path a covers line names must exist, and every `src/` file that sends an
  event the script expects or rules out (a string literal starting with the event's kind) must be on its lines
  (`covers.py --check`, `test_covers.py`): hand-picked lines missed `perform_adapter.cpp` (`gen_phrase`) on the
  scripts whose no-swap windows wait for it (#314). Includes have none: they run with the scripts that include them,
  and their events count for those scripts. A new `TEST_EVENT("gen_x %s", text)` whose text another file builds
  needs a `covers.BUILDERS` entry naming that file (or `()`); `test_covers.py` fails until it has one.
- A Y turn (`encoder 0 N` on a focused part) steps from the take queued, or else the one playing
  (`PerformView::verticalEncoderAction()`). Right after one turn, a turn back acts differently before and after the
  queued take lands on its phrase line, so its `gen_press take` depends on timing (#298 dropped such an expect). Expect
  a second turn's press only once the first take has landed.
- `hold preload` parks only the first preload job that starts after it: `TestPort::waitWhilePreloadHeld()` turns the
  hold off when that job's wait ends, by a release or a cancel. A confirm that cancels a held sound preload therefore
  lets its transition's preload run straight through (`perform_transition.txt`). Each `expect event` searches after
  the previous expect's match, so two expects can't both match one event: put every field you check in one expect (a
  `/regex/` when they aren't adjacent).
- `hold sound_committed` parks a sound job past its `chunks.commit()`, where a cancel no longer stops it, until
  `release sound_committed` (no cancel ends the wait). Any sound job's end turns it off (`SoundJob::finished()`), so a
  hold set for a job that fails before its commit doesn't park the next one. Release it before leaving Perform: the
  leave can't cancel a committed job, so its teardown waits on the parked one.
- `start_state` ends with `reset recipe` (`resetRecipe()` in `io/midi/test_port.cpp`), which zero-fills the Current
  Recipe as boot does (`forgetCurrentRecipe()`): with `exists` false, GENRE ▸ opens on the default genre and entry
  rolls a fresh Recipe. Entry picks ACID by name anyway (`pick genre`, in `perform_pick_acid.txt`), so a session
  stopped partway through a genre change can't move the next script onto another genre. Then `reset settings`
  (`resetSettings()`, #500) puts back GLIDE and BROWSER PREVIEW (Perform settings kept until power-off, spec §13.3)
  and clears stock's sample browser's `lastFilePathLoaded`. Other state a script leaves until power-off outlives `start_state`: add it to
  `resetSettings()` and to `perform_start_state.txt`'s check, or put it back at the script's end. Trap: a lane
  browser's sample preview (`AudioEngine::previewSample()`, which loads with `manuallySelected`) sets
  `lastFilePathLoaded`, so a later OSC 1 file browser opened in that kick's folder, where a BACK tap goes up a folder
  instead of closing it (`perform_retire.txt` after `perform_lane_browser.txt`). To find a leak between scripts, run
  the earlier scripts and the failing one in one delugemu boot (`test_port_emu.py --script A --script B`):
  `check_all.py` boots each script alone, so it never sees one.
- Test events from `PerformAdapter::tick()` come when the Generator decides a step; those from `OutputSink::play()`
  (`gen_section`, `gen_key`, notes) come when the step plays, a little later. So `gen_phrase bar=8` (decided) arrives
  before bar 8's `gen_section` (played): an `expect no_event gen_section ...` right after it sees that line's Section.
- Pick genres by name in probe scripts (`pick genre techno-dub`, test-port `pickGenre()` with
  `pickGenreForTest()` in `gui/menu_item/generative/genre_rows.cpp`), never by encoder turns from where GENRE opens:
  new genres add rows. The menu side only works on the GENRE ▸ or parent submenu whose session began last.
- `gen_outputs_freed outputs=N total=M`: `total=` counts a transition's Outputs freed since none was left retiring
  (a sound re-roll's, retiring alongside, don't count), so a script pins a roster's whole retirement
  (`/^gen_outputs_freed outputs=\d+ total=5$/`) whether it came in one batch or several. A batch can reach the total
  with another still to come: `mark` after it and `expect no_event gen_outputs_freed since=mark` later
  (`perform_genre_change.txt`).
- Trap (#505): after a stop's commit, give `gen_transition phase=idle` the whole cap (a phrase of the genre that
  played), not a few seconds. Stopped, an Output still sounding is freed only at its cap (spec §12.9), and an Impact
  with an echo keeps its FX Kit sounding: `GEN Impact Wash`'s delay (feedback 24) keeps rendering for stock's 110
  repeats, about 38 s. Which Impact a piece draws depends on the run (`perform_transition.txt` after a fresh load draws
  the Wash every time; delugemu doesn't), so a short wait can pass in one run and fail in another. Until idle, a Regenerate is refused (`refused
  phase=retiring`), so the expects after it fail too.
- A script that needs someone at the Deluge (a listener, a power pull) says `# needs: a Deluge and someone ...`:
  `covers.py --hardware` leaves out every needs line with the word `someone`.
- The firmware queues test events (`TestPort::event()`, 64 lines) and sends one a pass of the main loop, each after
  the last has left, so a burst can fill the queue. With 16 lines a transition's start step (about 25 events in 160
  ms) dropped a few in every delugemu run, now and then one a script waited for: #272's "phase stays retiring" was a
  dropped `gen_transition phase=idle`. A dropped line leaves `test_events_dropped n=N` where it was, and
  `run_script()` fails a script whose run moved state's `events_dropped` (one more expect, `FAIL end: N test events
  dropped`). It reads `events_dropped` only once state's `event_queue` (lines not yet sent) is 0 (`wait_sent()`): a
  line dropped while start_state's own burst drains isn't the script's, and one dropped after the last line is. When
  that fails, the script's other failures may be missing events, not firmware: read the log around the
  mark before debugging. A script that sends more in a burst needs a bigger queue, not fewer expects.
- A probe script that needs its own delugemu card names its overlay folder in `# needs: card fixtures/<script>`
  (relative to `scripts/generative/`); `test_port_emu.py` reads the line and builds that card, and refuses scripts
  that need different cards in one run (`--card-overlay` overrides the line). A `remove` in
  the overlay's `card.txt` of a path the factory card doesn't have fails the run: the card changed under the overlay.
  Reuse fixtures from the repo with `copy <repo path> -> TO` rather than copying them into the overlay. A `#` in
  `card.txt` starts a comment, so no path there may hold one. `fat_image.py` handles FAT32 on the whole image only
  (delugemu's card); its unit tests format their own images and run `fsck_msdos` or `fsck.fat` on each where the
  host has one.
- `covers.py --check` (`expect_kinds()`) takes an expect's TEXT up to its first space as the kind, and the files that
  send or build it (BUILDERS) must be on the covers lines. A kind with nothing after it matches as a prefix:
  `expect no_event gen_h1` names every file that sends or builds `gen_h16 ...` too. Write `gen_h1 steps=` to match
  only `gen_h1`.
- A script that needs a song (`# needs: SONGS/...`) can still be tried in delugemu: copy it and its includes to a
  scratch folder without its `load song` and `song_loaded` lines, and run the copy on the blank song. What depends on
  the song differs: the blank song has no arrangement, so PLAY in the arranger stops at once (`playing=false`).
- `hold card_routine` hands injected presses to their handlers with `sdRoutineLock` set (`inCardRoutine` true), as a
  press read inside `routineForSD()` is: the way to put a release inside a card wait without racing a load. A handler
  that answers REMIND_ME_OUTSIDE_CARD_ROUTINE waits, queue blocked, until `release card_routine`: hold it only around
  presses whose handler refuses instead (`perform_recipe_save.txt`).
- A preset preview's build reports none of a build's events (`gen_sound`, `gen_sound_level`, ...: each sits under
  `if (!previewBuild())`, `card_sounds.h`), so `gen_sound ACID` expects see only the part's own loads; its own
  `gen_preset_preview built ... path=` says what it loaded. Keep the event kind literal at the start of a
  `TEST_EVENT` format: a `%s` prefix there hides the kind from `covers.py --check`, which fails check_all. `hold
  preset_preview` parks every preview build at its start until it is cancelled or released.
- Trap: a BACK hold whose press closed a browser at its top folder also closes the lane list under it (the
  `BACK_MENU_EXIT` timer fires on the list); from a subfolder only the browser closes. Which one depends on the card
  (with the pack, an FX lane's SYNTH PRESET browser opens in a subfolder): follow the hold with one BACK tap, which on
  Perform with nothing queued does nothing (`perform_preset_preview.txt`).
- Late audio (#449): the time between render starts and a render's length don't tell whether the audio ran dry. Only
  `AudioEngine::routine()` copies rendered audio into the 2.9 ms TX ring, and a call may copy audio rendered earlier
  without rendering. `gen_audio` measures the ring's margin at each entry, and `drift=` checks it against the DMA's own
  position (`io/midi/output_margin.h`, its unit tests in `tests/unit/output_margin_tests.cpp`). The scheduler doesn't
  preempt: any main-loop task over about 2.9 ms is an underrun. `gen_audio_late` blames the task the routine ran
  inside, or else the one that finished just before it; `TaskManager::currentID` starts at 0, not -1, so it can't tell
  a nested call apart, and the test port keeps its own caller (`TestPort::taskStarted()`).
- A hand-written song on a card fixture: `start_state` after `load song` needs an instrument clip, so give the song
  one (an audio-only song fails it, and the next script's too). Write each clip as the firmware does: an
  `<instrumentClip>` with no `<soundParams>` froze in `ParamManager::getPatchCableSet()` during the load
  (`perform_song_audio_clip_no_params.txt`'s fixture, #448), as an audio clip with no `<params>` froze with E410. Run it in
  delugemu first; a hang there shows as `no reply to test port command`, and `test_port_emu.py --gdb PORT` plus
  `arm-none-eabi-gdb -batch -ex "target remote :PORT" -ex bt` names the freeze.
- Every `scripts/generative/*.txt` must be run by `check_all.py` (a `perform_*.txt` or `compose_*.txt` check, or one
  it lists as not run for its `# needs:` line), included by another script, or carry a `# runs-via: COMMAND` line
  (`covers.orphans()`, in `--check`, #466). A script with another prefix was never run and nothing said so:
  `song_audio_clip_no_params.txt` (#448) became `perform_song_audio_clip_no_params.txt`; the capture measurements,
  the power-pull pair, `golden_compose.txt` and `test_port_boot_dma.txt` got `runs-via` lines.
- `test_port_emu.EmuRun` takes a slot from the host-wide pool of real-time emulators (`emu_slots.py`, #417) before its
  first launch and keeps it across boot retries; qemu inherits the lock's fd (`pass_fds`), so an emulator orphaned by
  a killed runner keeps its slot until it exits. The leak check and the firmware check boot through `EmuRun` in real
  time (no icount), so they take slots too; only `gen_emu.py smoke` and `capture` don't. Waiting by hand for a
  `pgrep -x qemu-system-arm` count is no longer needed: the run waits and prints `waited N s for a real-time emulator
  slot`, which check_all's row shows.
- An injected press is handled wherever the firmware next reads buttons, also inside another task's card wait
  (`routineForSD()`), and Perform's card presses (SAVE, LOAD, the browsers' SELECT) refuse a press made there with
  BUSY. In Perform the card log writes on its own: the next piece's spare file 2 s after entry or a piece's opening,
  the piece's opening, and a flush every 10 s while it plays, so under host load a press can land in one (#485:
  `perform_recipe_save.txt`, `_load`, `_no_free_name`). Waiting for a quiet moment doesn't cover a section longer
  than the 10 s flush: a script whose card presses must not be refused, and that doesn't test the log, holds
  `card_log_off` before Perform's entry (the log takes every byte and touches no card).
- A retune (spec §21.2) reaches the probe scripts too: `fixtures/recipe_load/RECIPES/` holds Acid techno files meant
  to be today's (`read=ok`), and the scripts expect their `version=` and the `version older ... genre=` notice. Bump
  them with the genre; `test_recipe_fixtures.py` fails until they match the golden's version (#488). A palette change
  moves `gen_sound` expects: since #488 an FX lane with no sample picks one of its presets per piece (`pick ...
  SYNTHS/...`, not `fallback`), and on delugemu's factory card, which has no pack, the Impact is `silent` and counts
  in `gen_sound_missing` as the Riser does. A script that runs on both cards matches either.
- The Regenerate page (#494) opens on GO: SELECT, SELECT keeps a one-press confirm's flow. A style other than the
  entry: `include perform_regen_transition.txt`, the `encoder 3 N` turn, `include perform_regen_go.txt`, then SELECT.
  Its multi-selects and FROM's list remember their row across scripts on one boot: choose a variant from the top
  (`encoder 3 -5`). Menu events carry l10n's text (`title=Regenerate`, `Random`), not the capitals the OLED draws.
- Seam S7's `next recipe` lines (spec §21.3) are queued like presses, so they land in order with the script's other
  lines. `NextRecipe` lives in `.sdram_bss`, zero-filled, not constructed: the first field a line sets calls `clear()`
  to put its defaults in (`macroPoints` -1, `swing` kept, `variant` -1, `variantGenre` null). `next recipe variant <set>
  <name>` resolves against the latest queued/armed `next recipe genre` (else the Current Recipe's genre), so `next
  recipe genre` must precede it when no Recipe exists; changing `next recipe genre` clears stale variant picks, and
  `applyNextRecipe()` skips them if a Regenerate keeps another genre. A pick's path is the rest of its line and waits in
  `songPath`, the one slot `load song` uses, so `kLineLength` covers `kMaxSoundPath`. A roll that uses them draws its
  fresh entropy first, in the order it always did, then replaces the draws: `hold entropy` sequences stay the same.
- Seam S6's `load synth|kit` lines share `songPath`'s one slot with `load song` (a second waits as a full queue
  would), and run, like it, outside the card routine and one per buttons pass. `midi` lines run inside it, as DIN
  bytes do (`routineForSD()` reads both), and several with no `wait` between them land in one pass: a chord. Under
  icount a pack synth with no samples loads in about 1 ms, so its `preset_loaded` comes back before the inject's reply
  has been read: look for the event from before the inject (`preset_emu.load()`), not after. `midi` in a
  `deluge_probe.py` script is the host's own line, sent when the queue is idle, so a probe script can't send the test
  port's: `preset_emu.py` and the Engine send their lines themselves.
