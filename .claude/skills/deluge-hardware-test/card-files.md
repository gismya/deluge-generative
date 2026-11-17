# Files on the card

`scripts/generative/deluge_files.py` lists, copies and deletes files on the Deluge's SD card over USB, with no one at
the device. Use it to install `pack/` or a test song before a hardware run, or to fetch a song or preset to read.

```shell
./dbt exec "python3 scripts/generative/deluge_files.py ls /SONGS"
./dbt exec "python3 scripts/generative/deluge_files.py put pack/SAMPLES /SAMPLES"   # skips files whose size matches
./dbt exec "python3 scripts/generative/deluge_files.py get '/SONGS/Ttt.XML' /path/to/scratch/"
./dbt exec "python3 scripts/generative/deluge_files.py rm /GENTEST/old.txt"         # rm -r for a directory
./dbt exec "python3 scripts/generative/deluge_files.py mv /GENTEST/deluge.bin /deluge.bin"  # renames; never replaces
```

- It takes the same lock as `deluge_probe.py session`, so never run it inside a session's command, and it refuses
  while the hung marker exists (exit 3). Any build answers it, the SD-flashed one too: no load needed.
- The card is the user's and is backed up, but write only under `/GENTEST/` or the pack's own paths
  (`SAMPLES/FX/Riser`, `SAMPLES/DRUMS/Clap`, `SAMPLES/DRUMS/Ride`, `SYNTHS/GENERATIVE/FX`), and delete only what you
  wrote. So put `pack/SAMPLES` and `pack/SYNTHS` separately: `put pack/ /` would also write `/LICENSES.md`.
- `put` skips a file whose size on the card matches, also when its content differs: `--force` writes it anyway.
- Speed: about 75 KB/s reading and 40 KB/s writing.
- Over delugemu's UDP bridge (`--udp LISTEN:SEND`) it uses smaller blocks, for the DIN port's buffers.
- Over USB it writes in 512-byte blocks: on the desk Deluge a write of 757 bytes or more wrote one byte short (749
  wrote whole). The firmware's 7-to-8 unpack (`util/pack.c`) gives 757 bytes for a 757-byte message, so the byte goes
  missing before it, on the USB receive path or the host's; the cause isn't known (`USB_BLOCKS` in `deluge_files.py`).

## Swapping the root `.bin` for an SD-flash test

A test that needs the user to flash a build from the card (boot, startup or flash storage, see
[troubleshooting.md](troubleshooting.md)) puts that build at the card's root, the one place outside `/GENTEST/` you
may write. Keep exactly one `.bin` at the root, so there's no doubt which build the user flashes:

1. `ls /` and note the `.bin` there. Back it up twice: `get` it to a local scratch folder, and `mv` it to
   `/GENTEST/` on the card.
2. `put` the new build to `/GENTEST/`, then `mv` it to the root. `mv` never replaces, so a name clash fails rather
   than overwriting.
3. `ls /` again: exactly one `.bin`, the new one. Tell the user its name.

To go back, move the new `.bin` to `/GENTEST/` and the old one from `/GENTEST/` to the root.
