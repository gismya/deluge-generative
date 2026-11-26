# Probe scripts

Script syntax: the `run_script()` docstring in `scripts/generative/deluge_probe.py`. Event lines: `parseLine()` in
`src/deluge/io/midi/test_port.cpp`. Copy `scripts/generative/test_port_smoke.txt` to start a new script, and add
`TEST_EVENT(...)` calls to firmware code you need to observe.

- Start every script with `start_state`, once (include files have none; after a `load song`, a second one; `perform_start_state.txt` tests `start_state` itself and has two). It brings the device from wherever the last
  script left it (Perform, a menu, session view, playing) to a stopped instrument clip, so scripts run in any order on
  one load. Last, outside Perform, it sends `reset recipe`: the Current Recipe goes back to none, as after boot, so
  `perform_open.txt` lands on ACID and entry rolls a fresh Acid techno Recipe, whatever genre the last script left
  playing. Then `reset settings`: GLIDE and BROWSER PREVIEW (Perform settings kept until power-off) at their defaults,
  and stock's sample browser's last file cleared, so a file browser opens in `SAMPLES` (#500: a lane preview left it
  in a kick's folder, and BROWSER PREVIEW left OFF stopped the next script's previews). Anything else a script
  changes that lasts until power-off outlives `start_state`: put it back at the script's end, or add it to
  `resetSettings()` in `test_port.cpp`. `--events` counts a script's events from after its `start_state`, so what `start_state` itself causes,
  such as leaving Perform, prints for no script: a script that measures a leave leaves Perform itself (as
  `perform_mem_cycle.txt` does).
- `hold NAME` and `release NAME` set a firmware test hook on or off. The names, and what each does, are the rows of
  `kHolds` in `src/deluge/io/midi/test_port.cpp`. `start_state` first sends `release all`, so a script that stopped
  mid-hold leaves nothing held for the next one.
- Pick a genre by name, never by counting rows: `pick genre NAME` (queued like a press; NAME is the genre's `name`,
  `techno-dub`) focuses its row in the GENRE menu the sound editor shows, as turning to it would, and sends
  `gen_genre_pick NAME row=LABEL from=LABEL` (`from=`: the row focused before, where the menu opened) or
  `gen_genre_pick NAME refused` (no GENRE menu open, or the genre isn't in it). In GENRE ▸ a subgenre's row is its
  parent's: SELECT, `pick genre`, SELECT, `pick genre` again, SELECT. `perform_pick_acid.txt` (the end of
  `perform_open.txt`) enters on ACID that way, so entry doesn't depend on where GENRE opens or on the rows other
  genres add. Expect the event after each pick (`expect event gen_genre_pick techno-dub row=DUB`): a refusal moves
  nothing.
- `keep KEY...` keeps state values (`keep bpm undo_log`) and `expect KEY=kept` compares with them later: for "the song
  is untouched" when the value isn't known ahead (`perform_runner.txt`).
- `reset recipe` (queued like a press) sends `gen_recipe_reset done`, or `gen_recipe_reset refused why=perform` (in
  Perform: nothing changes) or `why=busy` (a runner job hasn't ended; `start_state` retries that for 10 s). A refusal
  or no event fails `start_state`. `reset settings` sends `gen_settings_reset done`, or `refused why=perform`.
- Bisecting with old builds: a build older than `release all` rejects it, and `start_state` then releases the holds
  of that time by name. For each one the build doesn't know either it prints `WARNING ...: start_state: the build
  doesn't know release entry` and goes on, so today's scripts still run there. A build older than `reset recipe`
  or `reset settings` gets one such warning for each. A build older than `pick genre` rejects it, which fails every Perform script's
  entry: bisect those with a copy of the scripts from that build's commit. Any other rejected line still fails the
  script.
- A timeout needs the keyword: `expect event gen_left within=15`. A bare trailing number is part of the text to match.
- A failed `expect event` prints the script's last events of the same kind (the same first word after the
  timestamp) as `got ...` lines, whatever `--events` says, so one run shows what came instead. Start TEXT with the
  event's kind (`gen_left delete_us=`); otherwise it lists the events containing TEXT's first word. The event the
  previous `expect event` matched says `(line N's match)`, and earlier ones `(before line N's match)`.
- `load song PATH` loads a song from the card (`load song SONGS/Ttt.XML`, path relative to the card's root, up to
  53 characters) as the boot song is loaded, then sends `song_loaded path=... ms=...`, or `song_load_failed path=...
  why=busy|missing|load` (busy: in Perform, or something open over the root UI, so load after `start_state`). Wait
  for it with `within=` (a big song takes a while; the expect also waits that long for the load), then `start_state`
  again: the song opens
  in the view it was saved in. A script that needs a song on the card says so in a `# needs:` line, so check_all.py
  skips it in delugemu (whose card has no songs); put the song there with [card-files.md](card-files.md). A script
  that needs other files on delugemu's card, or fewer, says `# needs: card fixtures/<script>`: test_port_emu.py
  (and so check_all.py) runs it on the factory card with that overlay folder put on it (the folder's `card.txt` and
  files, see `apply_overlay()`). On the Deluge, put the same files on its card by hand.
- A script names the paths it checks in `# covers:` lines (files, folders or globs, repo-relative):
  `scripts/generative/covers.py` picks the scripts a change needs from them, for `check_all.py` and for a session
  (`covers.py --hardware`). A new script gets them, naming at least every `src/` file
  that sends an event it expects (`covers.py --check` lists the ones missing); one with none runs with every change.
- A script that leaves files in `SONGS/` or `RECIPES/` (a `save song`, a Capture, a Recipe SAVE) declares them in
  `# writes: GLOB` lines, one glob each, from the card's root, matched without regard to case
  (`# writes: SONGS/GEN ACID TECHNO 3E9D8DZ LIVE*`, `# writes: RECIPES/ACID TECHNO *`). A session fails on a new file
  there that no script in it declares, and prints every new file's path so the run's files can be deleted after.
  When a script ends, the session deletes the new files its own `# writes:` lines declare (`session: deleted
  /PATH`), so the next script can save under the same name (#500: `perform_capture_leave.txt`'s Capture song gave
  `perform_lane_capture.txt` `... LIVE D`); `--keep-writes` keeps them.
  `test_deluge_probe.py` fails on a script with a save action and no `# writes:` line.
- Scripts that send MIDI clock or transport (`clock`, `midi fa`) depend on the device's clock-in settings:
  [external-clock.md](external-clock.md).
- Scripts that need MIDI follow, its feedback or the global MIDI commands use `hold midi_setup` (see `MidiSetup` in
  `src/deluge/io/midi/test_port.cpp` for the channels and notes): it sets them in RAM only, so the player's own MIDI
  settings in flash stay. Never learn them through SETTINGS > MIDI on the Deluge, which saves to flash. `start_state`
  releases it, as every hold.
- `expect event TEXT since=mark` searches from where the search stood at the script's last `mark` line, not after
  the previous match: for events one frame reports in either order (the Arc map's rows in `perform_song.txt`, the
  sound loads of a transition's preload in `perform_genre_change.txt`). The next expect searches after the later of
  the two matches. `expect event /REGEX/` matches the event's text after its timestamp with a regular expression,
  for a value that may wobble: `/^gen_ext_tempo bpm=(119|120|121)$/`.
- An `include`d script's `expect event` lines move the search too: an event its steps caused before its last match
  is behind the search when your next `expect event` runs, so that expect waits for a later one and fails. Put
  `mark now` before the `include` and give the expect `since=mark`:
  `mark now`, `include perform_transition_menu.txt`, `expect event gen_phrase bar= since=mark`.
- Time a press against the music with the firmware's clock, not the host's. Lines are queued and run in order on the
  firmware, waits included, so `button PLAY ...`, `wait 300`, `pad 16 6 ...` lands 300 ms into bar 1 whatever the
  host's load. An `expect event` on a status event can match one the host read late, and a press sent after it can
  land in a bar's last step, which decides at once (`perform_sidebar.txt`'s stop section). Likewise a hold: let go
  after the event that says it took (`include perform_hold_end.txt`: `expect event /^gen_end armed held_ms=[6-9]\d\d$/
  within=10`), not after a fixed wait, since delugemu under load runs its audio clock slower than the host's.
- Wait on the music, not a `within=` window, for "by bar N": `gen_phrase bar=N` (test builds) comes once the
  Generator has decided each phrase line's step, just before that line's `gen_section` (about 70 ms in delugemu).
  `gen_section KIND N clip_pos= bar=` gives the bar a Section starts on, so check where it landed, not when
  (`perform_transport.txt`: no Outro after `gen_phrase bar=8`, then the Outro on `bar=16`).
- `expect no_event TEXT since=mark` counts from the last `mark`, so it also fails on a matching event that came before
  the previous expect's match: for an event that may arrive on either side of the one you wait for
  (`perform_transport.txt`: a bar-8 Outro sent before `gen_phrase bar=8`).
- Many events are reported only when their text changes (`gen_sidebar`, `gen_arcrow`, `gen_rings` and the like), so
  `expect no_event X` passes without proving anything if the state was already wrong before the script looked.
  Check it right after an event that shows a known-good state.
- `hold card_error` makes every card read in Perform's loaders and folder scans fail as a bad card does
  (`cardErrorInjected()`, `generative/firmware/card_sounds.h`): delugemu can't pull or fault its card, so this is how
  a script reaches the card-error paths there.
- Test-port lines are parsed in the MIDI routine, which doesn't run while a runner job holds the card: a press sent
  during a load waits until the load ends. To press BACK during a load, `wait` in the script first (or a hold, such as
  `hold entry`, that keeps the job at a boundary).
- `gen_sound_level PART volume=N lower|same|higher played= preset=` (synth parts, test builds) gives the level the
  Sound plays at, the patcher's final post-FX volume, against its preset's: a part whose `.volume` is under 100 must
  play `lower`. `gen_sound ... vol=` is only the knob, which moves even when the patcher wasn't told (#228).
- Leaving: hold BACK until the leave menu is up, then let go: `include perform_hold_back.txt` (`button BACK down`,
  `expect event /^gen_leave_menu open held_ms=(3[5-9]|[4-9]\d)\d$/ within=10`, `button BACK up`). The menu opens while
  BACK is held, after the stock hold time (400 ms, `LONG_PRESS_DURATION`; delugemu reports 399-403), so a
  fixed `wait 700` can read as a tap in delugemu under load (`perform_homes.txt:34` in tooling batch 4's run).
  `held_ms=` is how long the test port's press had been held when the menu opened, on the firmware's audio clock, and
  the End pad's `gen_end armed held_ms=` likewise (`include perform_hold_end.txt`, 600-999): bound it with a regex, as
  `within=10` only waits out a slow host and would pass a hold that regressed to seconds. Leave the top under a
  second, not just over the hold time: a card access can delay the menu or the arm by a few hundred ms. `perform_leave.txt` keeps
  `expect ui=context_menu`: `start_state` runs it on old builds too, which send no `gen_leave_menu`.
- Mod buttons by their panel names: `button F1 down` to `button F8 down` (`button 1 1` is F2; `hid/button.h`'s
  `ZmodButtonX/Y`). `button MOD7` still works.
- Gold knobs: `encoder 4 N` (upper) and `encoder 5 N` (lower) turn N detents, each detent 4 edges 0.16 s apart (slow
  enough that the knob's acceleration stays at 1x), each edge a one-step edit: 4 edits a detent, so a sound control
  sends 4 `gen_edit` events per detent. `encoder 4 N fast` moves the same steps with each detent's 4 edges at once,
  the acceleration reset: one 4-step edit (one `gen_edit`) per detent, 32 detents in about a second instead of 20 s.
  Use it for long turns (a 64-detent slow turn queues 41 s, longer than the next expect's window) and wherever a script checks the edit a turn makes (`perform_homes.txt`). Stock code that
  acts on one step can differ: a mod cable turned down to 0 by fast turns stays, where slow ones delete it inside the
  edit (`perform_homes.txt`'s Wobble cable). A fast turn takes no time, so a press after it that a slow turn kept out
  of a double-tap window may now fall inside it: add a `wait`.
- `expect next event TEXT` is "this event and no other": the next event of TEXT's kind (its first word, or the word a
  pattern starts with, after an optional `^`; a pattern that starts otherwise, `/^\s*gen_edit/`, is a script error) must match TEXT, and a wrong one fails it at once. `expect event gen_edit control=none` would pass on a
  later `control=none` and hide a wrong edit before it. It searches as `expect event` does, after the previous
  match (`since=mark` from the mark), so turns that send several events of the kind (slow gold turns) leave some for
  it: use fast turns before it.
- `hold entropy` pins Perform's rolls: from the hold on, the firmware's fresh entropy comes from a fixed sequence, so
  the next entry (after `perform_open.txt`'s genre pick) rolls the same Recipe, seed and part presets on every run
  from the same card; a sound re-roll after it repeats too. Which preset that is depends on the card (delugemu's and
  the desk Deluge's differ). `start_state` releases it, and so does the end of every script, also one that stopped on
  an error. `perform_entropy.txt` checks two entries load the same sounds: `mark` before the entry, `keep events
  /^gen_sound /` after it, then `mark`, `hold entropy` again (each hold starts the sequence over), the second entry and `expect events kept`. Every
  entry rolls a fresh Recipe, so without the pin the second would load other sounds.
- `since=mark` goes only with `expect event` and `expect no_event`; on another form it's a script error.
- `within=S` counts from the expect line, the wait for the queue included: a failing expect ends about S after it
  starts. `expect no_event` alone watches its whole S after that wait. When the queue takes all of S, the expect still
  looks for 0.2 s, and a failure says `the queue before it took N s of its within=S`: give it a window that covers
  the queued waits and turns before it.
