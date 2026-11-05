# delugemu capture (throwaway, task branch only)

Scripts for the "Time a scripted capture in delugemu" ticket. They boot the firmware in delugemu, load a test song, play 60 s of it and save the audio to WAV. This branch is never merged.

- `make_card.py <factory card> <out>` writes `SONGS/CAPTURE.XML` and copies the 37 samples it uses. The factory card has no songs, so this builds one from three factory presets: the `000 TR-808` kit (sample drums), `118 Small Bridge Pad` (oscillator synth) and `169 Double Bass` (multisampled synth). The song holds each preset's params in its clips, the way a saved song does. Leaving them in the instrument freezes the firmware with E412.
- `capture.py` runs the capture and writes `run<N>.wav`, `run<N>.png` (the panel at the end) and `report.json`. Use `--mode icount` for a repeatable run on a virtual clock, or `--mode realtime` to run against the host clock.
- `summarise.py <out>` prints the reports as a Markdown table.
- `.github/workflows/delugemu-capture.yml` runs all of it on `ubuntu-latest` on each push to this branch.

## Run it locally (macOS)

```sh
python3 -I tools/delugemu-capture/make_card.py ~/source/delugemu-sd/sdcard /tmp/cap/card
~/source/delugemu/scripts/mksd.sh /tmp/cap/card /tmp/cap/card.img          # about 10 s
export DELUGEMU_QEMU_BIN=~/source/delugemu-nightly/DelugEmu-macos-arm64/DelugEmu.app/Contents/MacOS/qemu-system-arm
python3 -I tools/delugemu-capture/capture.py --firmware build/Release/deluge.elf \
  --card /tmp/cap/card.img --delugemu ~/source/delugemu --mode icount --shift 2 --runs 2 --out /tmp/cap/out
```

## How it captures

- **Audio**: `DELUGEMU_SSIF_DUMP`, the raw S32LE stereo frames the firmware renders, taken before the emulator's output resampler. v1.3.0 has no USB audio class, so `--usb-audio` gets nothing. `--audio wav` comes after the host-timed resampler and is 16-bit.
- **icount mode**: `-icount shift=N,sleep=off -S`. The VM starts paused and the script steps it forward in 100 ms of virtual time with `run_until`. It sends each button press at a fixed virtual time: LOAD, then the SELECT push, then PLAY. Waits are fixed virtual spans, or "the SD card has been quiet for 1 virtual second". pylib's `load_song` and `press` time their holds with wall-clock sleeps, so this mode doesn't use them.
- **realtime mode**: pylib's `load_song("CAPTURE")` and `press("PLAY")` on the host clock, then wait until the dump holds 60 s of frames.
- **Dropouts**: the SSIF stats stream (`DELUGEMU_SSIF_STATS`) reports underruns per virtual second. The report also gives the longest run of digital silence and RMS per second. In icount mode it compares frames rendered after Play with the virtual time that passed.
