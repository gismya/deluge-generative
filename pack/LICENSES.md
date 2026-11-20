# Generative pack: origins and licences

`pack/` is laid out as on the SD card (spec §18.1). Unzip it onto the card root. Its licences don't affect the
firmware's. Every file is CC0 1.0 (**Generated files**) except those derived from Synthstrom Audible's factory presets
(**Derived from the factory presets**), which carry no CC0 claim.

## Generated files

Every file below is written by `scripts/generative/make_pack.py` from code in that script: presets field by field, and
samples by its own small DSP routines with fixed random seeds. No third-party audio or preset goes into them. They are
released under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/): use them in anything, no credit needed.

Re-run the script after changing a sound and commit what changed. `make_pack.py --check` reports any file that
differs from what it would write, and any file here it doesn't write.

| File | What it is |
|---|---|
| `SYNTHS/GENERATIVE/FX/GEN Riser.XML` | Riser lane synth fallback: two saws and noise swelling over seconds, the filter and pitch lifting |
| `SYNTHS/GENERATIVE/FX/GEN Fall.XML` | Fall lane synth fallback: a bright hit whose pitch and filter sink as it decays |
| `SYNTHS/GENERATIVE/FX/GEN Impact.XML` | Impact lane synth fallback: a sine boom with a fast pitch drop and a noise crack |
| `SYNTHS/GENERATIVE/FX/GEN Siren.XML` | Dub's Siren: a square and a saw a fifth apart, a triangle LFO sweeping the pitch, a long release, velocity to volume |
| `SAMPLES/FX/Riser/GEN Riser 1.wav` | 8 s noise riser, a resonant band-pass sweeping up, peaking at the file end |
| `SAMPLES/FX/Riser/GEN Riser 2.wav` | 6 s riser, three detuned saws climbing three octaves |
| `SAMPLES/FX/Riser/GEN Riser 3.wav` | 4 s riser, high-passed noise and a rising sine under a speeding tremolo |
| `SAMPLES/FX/Riser/Short/GEN Riser Short.wav` | 2.0 s riser for Dub's 1-bar Build, three detuned saws climbing three octaves |
| `SAMPLES/DRUMS/Ride/GEN Ride 1.wav` | 909-style ride, metallic partials and noise |
| `SAMPLES/DRUMS/Ride/GEN Ride 2.wav` | 909-style ride with a bell |
| `SAMPLES/DRUMS/Clap/GEN Clap 1.wav` | Drum-machine clap, three bursts and a short tail |
| `SAMPLES/DRUMS/Clap/GEN Clap 2.wav` | Drum-machine clap, wider spacing and a longer tail |
| `SYNTHS/GENERATIVE/GEN Sine Sub.XML` | Ambient's Bass: one sine, an 80 ms attack, full sustain, a 1.5 s release, velocity to volume |
| `SYNTHS/GENERATIVE/GEN Psy Bass.XML` | Psytrance's Bass: a saw and a square an octave down into a resonant low-pass, a filter envelope, sustain 0, velocity to volume |
| `SYNTHS/GENERATIVE/GEN Dub Sub.XML` | Dub's Bass: a sine with a quiet triangle, a soft attack, a quick release, velocity to volume |
| `SYNTHS/GENERATIVE/GEN Melodica.XML` | Dub's Melodica: a square reed and a detuned saw, a breath of noise, a soft attack, velocity to volume and filter |
| `SYNTHS/GENERATIVE/GEN House Stab.XML` | House's Stab: a plain saw and square at one pitch, no portamento, sustain 0, a filter pluck, velocity to volume and filter |
| `SAMPLES/DRUMS/Snare/GEN Brush 1.wav` | Brush tap: band-passed noise, a fast attack and a short decay |
| `SAMPLES/DRUMS/Snare/GEN Brush 2.wav` | Brush stroke: band-passed noise swelling in over 60 ms |
| `SAMPLES/DRUMS/Snare/GEN Brush 3.wav` | Brush sweep: brighter noise swelling in over 180 ms and dying slowly |
| `SAMPLES/DRUMS/Other/GEN Mallet 1.wav` | Felt mallet thud: a sine falling from 210 to 90 Hz and a noise thump |
| `SAMPLES/DRUMS/Other/GEN Mallet 2.wav` | Higher mallet thud, falling from 320 to 150 Hz |
| `SAMPLES/DRUMS/Other/GEN Mallet 3.wav` | Deeper mallet thud, falling from 150 to 60 Hz |
| `SAMPLES/DRUMS/Block/GEN Wood 1.wav` | Wood hit: a click and four inharmonic modes, short |
| `SAMPLES/DRUMS/Block/GEN Wood 2.wav` | Lower, longer wood hit |
| `SAMPLES/DRUMS/Other/GEN Ping 1.wav` | Inharmonic metal ping: five non-integer partials ringing for about a second |
| `SAMPLES/DRUMS/Other/GEN Ping 2.wav` | Higher, longer metal ping |

## Derived from the factory presets

Each file below is a copy of a preset from the Deluge's factory card content by Synthstrom Audible, with a few edits.
They are derived from that factory content and are distributed on its terms, not under CC0. `make_pack.py` writes them
from the factory file (read from the card's files, checked by its SHA-256) and the edits recorded in its `DERIVED`
table, never by hand; `make_pack.py --check` fails while a `DERIVED` file has no row here naming its source. To add
one: a `DERIVED` entry, a row here, then run the script.

| File | Source (factory card) | Edits | What it is for |
|---|---|---|---|
| `SYNTHS/GENERATIVE/GEN Echo Chord Dry.XML` | `SYNTHS/037 Echo Chord.XML` | delay feedback 0, volume 50 | Dub techno's Stab, its echo from the part's delay setup alone (#322) |
| `SYNTHS/GENERATIVE/GEN Short Sharp Dry.XML` | `SYNTHS/082 Short Sharp Delay.XML` | delay feedback 0, volume 40 | Dub techno's Stab (#322) |
| `SYNTHS/GENERATIVE/GEN Kyoto Phono Dry.XML` | `SYNTHS/072 Kyoto Phono.XML` | delay feedback 0, volume 28 | Dub techno's Stab (#322) |

Every pack synth preset must respond to velocity (spec §10.1). Check with
`python3 scripts/generative/check_pack_velocity.py`.

## Reserved

`GENERATIVE/` on the card is reserved for palette override files. The pack ships none.
