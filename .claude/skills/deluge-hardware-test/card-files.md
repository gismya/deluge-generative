# Files on the card

`scripts/generative/deluge_files.py` lists, copies and deletes files on the Deluge's SD card over USB, with no one at
the device. Use it to install `pack/` or a test song before a hardware run, or to fetch a song or preset to read.

```shell
./dbt exec "python3 scripts/generative/deluge_files.py ls /SONGS"
./dbt exec "python3 scripts/generative/deluge_files.py put pack/SAMPLES /SAMPLES"   # skips files whose size matches
./dbt exec "python3 scripts/generative/deluge_files.py get '/SONGS/Ttt.XML' /path/to/scratch/"
./dbt exec "python3 scripts/generative/deluge_files.py rm /GENTEST/old.txt"         # rm -r for a directory
```

- It takes the same lock as `deluge_probe.py session`, so never run it inside a session's command, and it refuses
  while the hung marker exists (exit 3). Any build answers it, the SD-flashed one too: no load needed.
- The card is the user's and is backed up, but write only under `/GENTEST/` or the pack's own paths
  (`SAMPLES/FX/Riser`, `SAMPLES/DRUMS/Clap`, `SAMPLES/DRUMS/Ride`, `SYNTHS/GENERATIVE/FX`), and delete only what you
  wrote. So put `pack/SAMPLES` and `pack/SYNTHS` separately: `put pack/ /` would also write `/LICENSES.md`.
- `put` skips a file whose size on the card matches, also when its content differs: `--force` writes it anyway.
- Speed: about 75 KB/s reading and 40 KB/s writing.
- Over delugemu's UDP bridge (`--udp LISTEN:SEND`) it uses smaller blocks, for the DIN port's buffers.
