# Probe scripts

Script syntax: the `run_script()` docstring in `scripts/generative/deluge_probe.py`. Event lines: `parseLine()` in
`src/deluge/io/midi/test_port.cpp`. Copy `scripts/generative/test_port_smoke.txt` to start a new script, and add
`TEST_EVENT(...)` calls to firmware code you need to observe.

- Start every script with `start_state`, once (include files have none; after a `load song`, a second one). It brings the device from wherever the last
  script left it (Perform, a menu, session view, playing) to a stopped instrument clip, so scripts run in any order on
  one load. `--events` counts a script's events from after its `start_state`, so what `start_state` itself causes,
  such as leaving Perform, prints for no script: a script that measures a leave leaves Perform itself (as
  `perform_mem_cycle.txt` does).
- `hold NAME` and `release NAME` set a firmware test hook on or off. The names, and what each does, are the rows of
  `kHolds` in `src/deluge/io/midi/test_port.cpp`. `start_state` first sends `release all`, so a script that stopped
  mid-hold leaves nothing held for the next one.
- Bisecting with old builds: a build older than `release all` rejects it, and `start_state` then releases the holds
  of that time by name. For each one the build doesn't know either it prints `WARNING ...: start_state: the build
  doesn't know release entry` and goes on, so today's scripts still run there. Any other rejected line still fails
  the script.
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
