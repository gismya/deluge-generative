# Generative pack: origins and licences

`pack/` is laid out as on the SD card (spec §18.1). Unzip it onto the card root. Its licences don't affect the
firmware's.

## Generated files

Every file below is written by `scripts/generative/make_pack.py` from code in that script: presets field by field, and
samples by its own small DSP routines with fixed random seeds. No third-party audio or preset goes into them. They are
released under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/): use them in anything, no credit needed.

Re-run the script after changing a sound and commit what changed. `make_pack.py --check` reports any file that
differs from what it would write.

| File | What it is |
|---|---|
| `SYNTHS/GENERATIVE/FX/GEN Riser.XML` | Riser lane synth fallback: two saws and noise swelling over seconds, the filter and pitch lifting |
| `SYNTHS/GENERATIVE/FX/GEN Fall.XML` | Fall lane synth fallback: a bright hit whose pitch and filter sink as it decays |
| `SYNTHS/GENERATIVE/FX/GEN Impact.XML` | Impact lane synth fallback: a sine boom with a fast pitch drop and a noise crack |
| `SAMPLES/FX/Riser/GEN Riser 1.wav` | 8 s noise riser, a resonant band-pass sweeping up, peaking at the file end |
| `SAMPLES/FX/Riser/GEN Riser 2.wav` | 6 s riser, three detuned saws climbing three octaves |
| `SAMPLES/FX/Riser/GEN Riser 3.wav` | 4 s riser, high-passed noise and a rising sine under a speeding tremolo |
| `SAMPLES/DRUMS/Ride/GEN Ride 1.wav` | 909-style ride, metallic partials and noise |
| `SAMPLES/DRUMS/Ride/GEN Ride 2.wav` | 909-style ride with a bell |
| `SAMPLES/DRUMS/Clap/GEN Clap 1.wav` | Drum-machine clap, three bursts and a short tail |
| `SAMPLES/DRUMS/Clap/GEN Clap 2.wav` | Drum-machine clap, wider spacing and a longer tail |

Every pack synth preset must respond to velocity (spec §10.1). Check with
`python3 scripts/generative/check_pack_velocity.py`.

## Reserved

`GENERATIVE/` on the card is reserved for palette override files. The pack ships none.
