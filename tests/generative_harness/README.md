# Generative harness

A desktop driver for the portable Generator core in `src/deluge/generative/core/`. It runs the core for N bars,
prints the event stream as text and can write a Standard MIDI File to listen to. It does basic checks only:
genre tables are tuned by ear on real firmware in delugemu (issue #11).

## Build

The harness builds with the rest of `tests/` (`./dbt test` builds it along with the unit tests), or on its own:

```sh
cmake -S tests/generative_harness -B build/generative_harness
cmake --build build/generative_harness
```

Any C++20 host compiler works. If CMake or Ninja isn't on your `PATH`, use the ones in `toolchain/current/`.

## Run

```sh
build/generative_harness/generative_harness --bars 8 --key A --scale phrygian --density 35
build/generative_harness/generative_harness --bars 64 --midi techno.mid --bpm 130 --quiet
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--seed N` | 1 | Seed (decimal or `0x` hex) |
| `--bars N` | 16 | Bars to run |
| `--key K` | C | Root, as 0–11 or a name such as `F#` or `Bb` |
| `--scale S` | minor | `minor`, `phrygian` or `dorian` |
| `--energy N` | 25 | Energy macro, 0–50 |
| `--density N` | 25 | Density macro, 0–50 |
| `--motion N` | 25 | Motion macro, 0–50 |
| `--bpm N` | 130 | Tempo written to the MIDI file |
| `--midi PATH` | | Also write a MIDI file |
| `--offs` | | Print note-off lines too |
| `--quiet` | | Print only the summary |

Each note-on prints one line:

```
  1.2.4  tick    166  -2  Drums ClosedHat  note -       vel  61  len 4
```

That's bar.beat.16th (1-based) of the nearest grid line, the swung tick, the Lean from that grid line, the part,
the drum lane, the MIDI note (Bass only), the velocity and the length in ticks. The last line counts note-ons per
lane and shows the bass range.

The MIDI file is format 0 at 96 PPQN. Drums are on channel 10 with General MIDI notes (kick 36, clap 39, closed
hat 42, open hat 46). Bass is on channel 1 with GM program 39 (Synth Bass 1).

The harness drives the core the way Perform does when it honours the returned waits: `tick(now)`, then
`now += wait`.
