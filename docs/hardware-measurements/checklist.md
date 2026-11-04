# Hardware measurements checklist (#37)

Measurements on a real OLED Deluge for [#37](https://github.com/gismya/deluge-generative/issues/37): free memory, two songs at once, kit build time, RAM per kit lane, load times and pad redraw cost. You run it; it takes about 45 minutes. Each step says what to press, what to read off the OLED, and which row of the [results table](#results-table) to fill in. The same numbers, with more detail, also go to a log file on the Mac.

Branch `task/hardware-measurements`. The instrumentation is in `src/deluge/generative/debug/hardware_measurements.cpp` and is compiled only into Debug and RelWithDebInfo builds; the Release build has none of it.

## What you need

- The OLED Deluge, its SD card with the **factory content**, a USB cable to the Mac.
- These factory files on the card (the measurements load them by path):
  - `KITS/000 TR-808.XML`, `KITS/004 R-50.XML`
  - `SYNTHS/047 Basic Dirty Bass.XML`, `SYNTHS/000 Rich Saw Bass.XML`, `SYNTHS/018 Rich Saw Lead.XML`, `SYNTHS/057 FM Lead.XML`, `SYNTHS/170 Sitar.XML`
  - `SAMPLES/DRUMS/…/909 *.wav` (Perform's kit) and the 12 `SAMPLES/DRUMS/…/Linndrum *.wav` files listed in `hardware_measurements.cpp` (`kKit12Paths`)
- The firmware files, in `/Users/larsjohansson/source/deluge-generative-builds/task-hardware-measurements/`:
  - `debug/deluge.bin`: the measurement build (Debug, `-Og`). Use this one.
  - `relwithdebinfo/deluge.bin`: the same measurements, optimised like Release (`-O2`, LTO). Only for the optional [timing pass](#optional-timing-pass-with-the-optimised-build).
  - `perform-poc-release/deluge.bin`: the Perform POC release build you ran before (`poc/perform-techno` @ `49a13d20e`), to go back to afterwards.
  - `deluge_log.py`: the log capture script.

  To rebuild them from the branch instead: `DBT_NO_SYNC=1 ./dbt build debug` (and `relwithdebinfo`, `release`); the files land in `build/Debug/`, `build/RelWithDebInfo/`, `build/Release/`.

## Before you start: make the typical song (once, on any firmware)

\#37's "typical song" is one kit, four synths and some samples. The factory card has no songs, so make one and save it. Do this on the firmware you have now, before installing the debug build.

1. New song (SHIFT + LOAD/NEW).
2. Make five clips in session view, each with a few notes so they play: a kit clip with **`004 R-50`** (not `000 TR-808` or `006 LD`, which the measurements load cold), and synth clips with **`000 Rich Saw Bass`**, **`018 Rich Saw Lead`**, **`057 FM Lead`** and **`170 Sitar`** (Sitar is sample-based).
3. Check that all five clips play. Save it as **`MEAS TYPICAL`** (SAVE, type the name, SAVE).

## Install the measurement build

1. Put the SD card in the Mac. Copy `debug/deluge.bin` to the **root** of the card. It must be the **only** `.bin` file in the root; delete or move any other.
2. Eject the card and put it in the Deluge.
3. **Hold SHIFT while you switch the Deluge on.** Keep holding until the update starts. It flashes the firmware and then boots it.
4. Optional: delete `deluge.bin` from the card afterwards. It is only installed while SHIFT is held at power-on, but the next update needs its `.bin` to be the only one.

## Capture the log on the Mac

Every measurement writes its full results as `MEAS …` lines to the Deluge's debug log, which goes over USB MIDI as sysex. The OLED popup shows only a summary, so keep the log running for the whole session. You'll attach the file to #37.

1. Connect the Deluge to the Mac with USB. The Deluge shows up as MIDI ports named "Deluge…" (no driver needed).
2. In Terminal:

   ```sh
   ~/source/deluge-generative/toolchain/current/python/bin/python3 \
     ~/source/deluge-generative-builds/task-hardware-measurements/deluge_log.py \
     --out ~/Desktop/deluge-meas.log --only-meas
   ```

   That is the dbt toolchain's Python, which has `python-rtmidi`. The script finds the Deluge's port, turns the log on, prints the `MEAS` lines with the Mac's time, and appends **every** log line to `~/Desktop/deluge-meas.log`. Stop it with Ctrl-C. `--list` shows the MIDI ports if it can't find the Deluge.
3. **Run it again after every Deluge power-on**: the Deluge forgets the log subscription on reboot.

Alternatives, if the script gives trouble: `./dbt sysex-logging` in the repo (prints the same lines, with a NUL byte between characters; pipe through `tr -d '\000'`), or DEx in Chrome (<https://dex.silicak.es>, "Auto Debug" console). With nothing listening, the log goes nowhere; the OLED popups work either way.

## How the measurements work

**Triggers.** Hold **LEARN**, press the second button, then release both. In most views LEARN alone starts MIDI learn (the pads flash); that is harmless and stops when the measurement starts. While "Measuring" shows, don't touch anything.

| Hold LEARN + | Measures |
|---|---|
| **SYNTH** | Loads the synth preset `047 Basic Dirty Bass` into the current song (not linked to a clip, as Perform loads its Bass), reports, deletes it. |
| **KIT** | Loads the factory kit `000 TR-808` with all its samples, reports, deletes it. |
| **MIDI** | Builds a 12-lane kit from 12 single Linndrum samples on Perform's own kit-building code, reports each lane, deletes it. |
| **CV** | Memory report: free RAM per allocator region, the sample cache, audio gaps since the last measurement. |
| **KEYBOARD** | Pad redraw: 16 full redraws of the 16×8 grid plus sidebar, split into render, queue and UART time. |
| **SCALE** | Shows the last summary popup again. |

Two measurements run on their own:

- **Entering Perform** (SHIFT + SONG in session view) times the roster build: each of the 4 drum lanes, the Bass preset, the clip linking.
- **Loading a song** (LOAD) times the load. If a song is playing, it also reports the RAM free at the moment both songs are in memory.

**The popup** stays for 30 s (AFFECT ENTIRE clears it, LEARN + SCALE brings it back). At most four lines:

| Popup line | Meaning |
|---|---|
| `kit12 120.2ms` | name and total time. `ERR n` instead means it failed (error code n). |
| `lane 9.9ms 8168B` | per kit lane: average time and average fast + SDRAM-heap bytes (lane measurements) |
| `parse 21.3ms` | the first phase, for loads |
| `used 133K` | RAM the measured thing took, fast + SDRAM heap, sample data not included |
| `@swap fast 812K` | song load: free fast RAM just before the old song is deleted (both songs in RAM when playing) |
| `Free fast 1008K` / `ext 2006K` / `steal 63117K c0K` | memory report: free fast heap, free SDRAM heap (2 MiB, `new`/containers), free sample memory (SDRAM) and reclaimable sample cache |
| `Pads 441B` / `render` / `uart` / `max all` | pad redraw: bytes per redraw, avg render time, fastest UART send, slowest full redraw |
| `gap 1.5ms late 0` | audio: the longest time between two audio renders during the measurement, and how many gaps exceeded the 2.9 ms output buffer. **late > 0 means an audible dropout.** |

**Audio glitches.** "late" counts them; still listen on headphones during the "while playing" steps and note any click.

**Cold vs warm.** Samples stay cached in RAM after the first load. The first run after power-on is **cold** (reads the card); a repeat is **warm**. The steps say which to record.

**Debug build caveats.** It is compiled with `-Og`, so CPU-bound times (XML parsing, rendering) come out slower than Release; card-bound times (sample reads) less so. The optional pass at the end repeats the timing rows on the `-O2` build. Its code is also bigger, so the fast heap is smaller than Release's: the internal region is **40,312 B smaller** in the Debug build and **37,920 B smaller** in RelWithDebInfo (Release: 1,113,324 B). The log prints each region's `size`, so the Release figure is `free + that difference`.

## Session A: no song (fresh boot)

Power on with the measurement build (the Deluge starts a new empty song), start `deluge_log.py`, go to session view (SONG). Don't play anything yet.

| Step | Do | Read off | Row |
|---|---|---|---|
| A1 | LEARN + CV | `Free fast`, `ext`, `steal` | M1 |
| A2 | LEARN + KIT | total, `parse`, `used` (**cold**) | L3 |
| A3 | LEARN + KIT again | total (**warm**) | L4 |
| A4 | LEARN + SYNTH, three times | total of each | L1 |
| A5 | LEARN + MIDI | total, `lane` (**cold**) | K1, R1 |
| A6 | LEARN + MIDI again | total, `lane` (**warm**) | K2 |
| A7 | LEARN + KEYBOARD | `Pads`, `render`, `uart`, `max all` | P1 |

## Session B: typical song

**Switch the Deluge off and on** (so the sample cache is cold), restart `deluge_log.py`, go to session view.

| Step | Do | Read off | Row |
|---|---|---|---|
| B1 | LOAD, pick `MEAS TYPICAL`, load it (not playing) | `song` total, `parse` | L5 |
| B2 | Play 8 bars, stop. LEARN + CV | `Free fast`, `ext`, `steal` | M2 |
| B3 | LEARN + KEYBOARD | pad redraw with a real session grid | P2 |
| B4 | **Play** the song. While it plays: LEARN + MIDI. Listen. | total, `lane`, `gap`/`late`, clicks? | K3 |
| B5 | Still playing: LEARN + KIT. Listen. | total, `gap`/`late`, clicks? | L6 |
| B6 | Still playing: LOAD, pick `MEAS TYPICAL` again, load it. Let it swap in at the end of the loop. Listen. | `song` total, `@swap fast`, `gap`/`late`, clicks? | S1 |
| B7 | Right after the swap, LEARN + CV | `Free fast`, `ext`, `steal` | M3 |
| B8 | Still playing: SHIFT + SONG (enter Perform). Listen. | `perform` total, `lane`, `gap`/`late`, clicks? | K4 |
| B9 | In Perform: LEARN + KEYBOARD | pad redraw in Perform | P3 |
| B10 | In Perform: LEARN + CV | memory with song + Perform roster | M4 |
| B11 | Stop. BACK to session view. | | |

When done, stop `deluge_log.py` (Ctrl-C).

## Optional: timing pass with the optimised build

Install `relwithdebinfo/deluge.bin` the same way, power on, start `deluge_log.py` with another `--out` file (e.g. `~/Desktop/deluge-meas-o2.log`), then repeat A2, A4, A5, A7, power-cycle, B1, and B4. Fill in the "-O2" column.

## Go back to the Perform POC

Install `perform-poc-release/deluge.bin` the same way: copy it to the card root as the only `.bin`, hold SHIFT while switching on. (Or install a newer release build of `generative` if there is one.)

## Results table

Copy this into a comment on #37, fill it in, and attach `deluge-meas.log` (and `deluge-meas-o2.log` if you did the optional pass). Times in ms, memory in KB as the popup shows it; the log has exact bytes.

| Row | Measurement | Debug | -O2 (optional) | Notes |
|---|---|---|---|---|
| M1 | Free memory, no song: fast / ext / steal (KB) | | — | |
| M2 | Free memory, typical song loaded: fast / ext / steal (KB) | | — | |
| M3 | Free memory, right after the swap (B7): fast / ext / steal | | — | |
| M4 | Free memory, typical song + Perform roster: fast / ext / steal | | — | |
| S1 | Two songs: song load while playing, total ms / `@swap fast` KB / late / clicks? | | — | |
| L1 | Synth preset load (`047 Basic Dirty Bass`), 3 runs (ms) | | | |
| L3 | Factory kit load (`000 TR-808`), cold: total / parse ms, used KB | | | |
| L4 | Factory kit load, warm: total ms | | — | |
| L5 | Factory-content song load (`MEAS TYPICAL`), cold, stopped: total / parse ms | | | |
| L6 | Factory kit load while playing: total ms / late / clicks? | | — | |
| K1 | 12-lane kit build, cold: total ms / lane avg ms | | | |
| K2 | 12-lane kit build, warm: total ms / lane avg ms | | — | |
| K3 | 12-lane kit build while playing: total ms / late / clicks? | | | |
| K4 | Perform roster build (4 lanes + Bass) while playing: total ms / late / clicks? | | — | |
| R1 | RAM per kit lane: `lane … B` from the popup (fast + SDRAM heap) | | — | the log's `lane avg` line also gives `stealable` (Sample object + sample data) |
| P1 | Pad redraw, empty session: bytes / render / uart / max all (ms) | | | |
| P2 | Pad redraw, typical song session view | | — | |
| P3 | Pad redraw, Perform view | | — | |

## Reading the log

All lines start `<seconds since boot>: hardware_measurements.cpp:<line>: MEAS <name> …`. `grep MEAS ~/Desktop/deluge-meas.log` gives just the measurements. Per measurement:

- `begin playing=0|1 error=N`: whether a song was playing, and the error code (0 = none).
- `lane i T us fast +B external +B stealable +B`: one kit lane: time, and RAM taken from the fast heap, the SDRAM heap and the sample memory. `lane avg` averages them.
- `phase NAME T us`: time per phase. Loads: `parse` (XML), `files` (sample headers, first clusters queued), `clusters` (until the card queue is empty). Song load: `parse`, `samples` (until the samples needed to start are loaded), `swap` or `noswap` (until the old song is about to be deleted; while playing this includes waiting for the loop to end), `rest`. Perform: `kit`, `bass`, `link`.
- `total T us`, `used fast +B external +B stealable +B`: whole measurement and the RAM it took.
- `mem REGION size S free F largest L`: each allocator region. `internal` + `internal_small` is the fast heap, `external` + `external_small` the 2 MiB SDRAM heap, `stealable` the rest of SDRAM, which holds sample data. `mem cache` is sample data that's cached but not in use, which the allocator can reclaim.
- `audio maxgap T us late N`: see the popup table.
- `pads render|queue|uart|total min A avg B max C us`: per redraw. `uart` is the time until the PIC UART has sent everything (200 kbaud), so it is the wire time.
