# Probe scripts

Script syntax: the `run_script()` docstring in `scripts/generative/deluge_probe.py`. Event lines: `parseLine()` in
`src/deluge/io/midi/test_port.cpp`. Copy `scripts/generative/test_port_smoke.txt` to start a new script, and add
`TEST_EVENT(...)` calls to firmware code you need to observe.

- Start every script with `start_state`, once (include files have none; after a `load song`, a second one; `perform_start_state.txt` tests `start_state` itself and has two). It brings the device from wherever the last
  script left it (Perform, a menu, session view, playing) to a stopped instrument clip, so scripts run in any order on
  one load. Last, outside Perform, it sends `reset recipe`: the Current Recipe goes back to none, as after boot, so
  `perform_open.txt` lands on ACID and entry rolls a fresh Acid techno Recipe, whatever genre the last script left
  playing. `--events` counts a script's events from after its `start_state`, so what `start_state` itself causes,
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
  or no event fails `start_state`.
- Bisecting with old builds: a build older than `release all` rejects it, and `start_state` then releases the holds
  of that time by name. For each one the build doesn't know either it prints `WARNING ...: start_state: the build
  doesn't know release entry` and goes on, so today's scripts still run there. A build older than `reset recipe`
  gets one such warning for it too. A build older than `pick genre` rejects it, which fails every Perform script's
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
  skips it in delugemu (whose card has no songs); put the song there with [card-files.md](card-files.md).
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
- Time a press against the music with the firmware's clock, not the host's. Lines are queued and run in order on the
  firmware, waits included, so `button PLAY ...`, `wait 300`, `pad 16 6 ...` lands 300 ms into bar 1 whatever the
  host's load. An `expect event` on a status event can match one the host read late, and a press sent after it can
  land in a bar's last step, which decides at once (`perform_sidebar.txt`'s stop section). Likewise a hold: let go
  after the event that says it took (`expect event gen_end armed within=10`), not after a fixed wait, since delugemu
  under load runs its audio clock slower than the host's.
- Wait on the music, not a `within=` window, for "by bar N": `gen_phrase bar=N` (test builds) comes once the
  Generator has decided each phrase line's step, just before that line's `gen_section` (about 70 ms in delugemu).
  `gen_section KIND N clip_pos= bar=` gives the bar a Section starts on, so check where it landed, not when
  (`perform_transport.txt`: no Outro after `gen_phrase bar=8`, then the Outro on `bar=16`).
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
- Leaving: hold BACK until the leave menu is up, then let go (`button BACK down`, `expect ui=context_menu
  within=10`, `button BACK up`). The menu opens while BACK is held, after the stock hold time on the audio clock, so
  a fixed `wait 700` can read as a tap in delugemu under load (`perform_homes.txt:34` in tooling batch 4's run).
