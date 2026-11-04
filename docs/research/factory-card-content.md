# Factory card content per role

Research for [#34](https://github.com/gismya/deluge-generative/issues/34), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1). It feeds [Generative pack contents (#42)](https://github.com/gismya/deluge-generative/issues/42).

**Question.** What does the factory SD card (OLED V4.1.0) offer each part and lane of the five v1 genres (Techno, House, Dub, Psytrance, Ambient), and where are the gaps the Generative pack has to fill?

**Settled context (not reopened here).** Synth parts load card presets through a palette ([Sound generation, #13](https://github.com/gismya/deluge-generative/issues/13)). On load the Generator sets voice mode, and portamento for parts that slide, and after that it moves only the six sound controls. Factory FM presets load with their filters off. Drum lanes draw samples from `SAMPLES/DRUMS/<folder>` through the lane→folder table in [Kit lane mapping, #25](https://github.com/gismya/deluge-generative/issues/25). FX lanes look in `SAMPLES/FX/<lane>` and fall back to synth presets. Roles and flavours are from [Track roles, #12](https://github.com/gismya/deluge-generative/issues/12), and the genre briefs from [Genre model, #11](https://github.com/gismya/deluge-generative/issues/11).

**Sources.**

- The factory card at `~/source/delugemu-sd/sdcard`: 171 `SYNTHS/*.XML`, 213 drum samples in `SAMPLES/DRUMS`, 392 files in `SAMPLES/Artists`. Read and measured, never executed.
- Firmware source on `generative` @ `487f7d9e` (upstream `v1.3.0-rc3`). Paths are relative to `src/deluge/` unless they start with `website/`.
- The survey script [`factory-card-content/card_survey.py`](factory-card-content/card_survey.py). It parses the XML, converts raw values the way the firmware does, and measures WAV files. Run it with `python3 -I card_survey.py <card root> <synths|counts|fmfilter|drums|fxshape fx_candidates.txt|samples|json>`. Every count below comes from it.

Role picks are my reading of the numbers plus the preset name. Nothing here has been listened to. Tuning by ear in delugemu ([#8](https://github.com/gismya/deluge-generative/issues/8)) may move any pick.

---

## 1. Short answer

- **Synths.** The card covers Bass, Pad and Lead well and Chords thinly. Of 171 presets, 28 Bass, 18 Chords, 37 Pad and 40 Lead candidates carry no flag at all (§2.4). Chords is thin because many stab-like presets stack a fifth or a third on every note.
- **What fights the Generator.** 15 presets have a **saved arp** that is copied into the clip and would replace the Generator's notes. 8 are tuned **off-key**: no audible oscillator sits on the played note. 26 have **portamento saved**, and in POLY or MONO mode it makes every note glide, chords included. 46 have a release of 2 s or more. Heavy FX (reverb or delay ≥ 20, crush, saturation ≥ 5, deep mod FX) are on 32. Section 2.3 has the fixes.
- **FM filters.** No factory FM preset relies on its filter. FM had no filter in the firmware these presets were made for, so forcing filters off on load gives the sound the designer heard. 25 of the 54 FM presets have a closed LPF saved and 7 have cables into it. That is dead data.
- **Drums.** Every lane has at least 3 samples in its default folders (the fewest: Clap 8, Perc pool 55). No lane is below the bar. Thin spots are per genre: one 909 ride, one 909 clap, no psy kick or snare. There is also no soft or brushed kit for Ambient and Dub. `SAMPLES/Artists` holds better genre material, such as a full psy set (`Dean Daughters/Psy`).
- **FX.** There is no `SAMPLES/FX`. `SAMPLES/Artists` has 8 risers and 8 falls that I measured as rising or falling (`Danny Taurus/MoDrama/Upper*`, `Downer*`), one more riser (`frugalriser.wav`), and several impacts. No synth preset is a real riser. A few can stand in (§5).
- **Draft palettes** for all five genres are in §6. The biggest gaps for the pack: a 303-style acid preset, a clean house chord stab with no built-in interval, a dub siren, soft brushed or mallet percussion for Ambient, and trimmed riser, fall and impact samples in `SAMPLES/FX/<lane>`.

## 2. Synth presets

### 2.1 How the presets were read

Each preset was parsed with the firmware's own value rules:

- **Synth mode** is the `<mode>` tag: 103 subtractive, 54 FM, 14 ringmod. There are no wavetable or DX7 presets. 3 subtractive presets play samples (168 Hang Drum, 169 Double Bass, 170 Sitar).
- **Saved voice mode** follows `stringToPolyphonyMode` (`util/functions.cpp:854-876`). Old files store `1` (POLY) or `0` (AUTO). The counts are POLY 130, AUTO 25, MONO 11, LEGATO 5. The Generator overrides this on load anyway.
- **Menu values** (0–50) are `(raw + 2^31) / 85899345`, the inverse of `getParamFromUserValue` (`util/functions.cpp:1359-1389`).
- **Envelope times** in seconds were derived from the render code. `Envelope::render` adds the final param value to `pos` every sample until `pos` reaches 2^23 (`modulation/envelope.cpp:29-118`). For decay and release that final value is `neutral × lookupReleaseRate(raw × 2^30 >> 32) >> 32`, with neutral `140<<9` for release and `70<<9` for decay (`util/functions.cpp:57-163`, `:249-253`, `:1340-1356`, `modulation/patch/patcher.cpp:240-271`). Attack is `getExp(4096, −raw × 1.5 × 2^29 >> 32)` (`util/functions.cpp:503-512`). These are the env1 (amplitude) times with no cables. Velocity cables shorten or lengthen them a little.
- **Pitch** is the sound's `<transpose>` plus osc1's. An oscillator counts as audible if its volume is above 0. In ringmod both oscillators always count, and a hard-synced osc2 never counts, because it sounds at osc1's pitch.

### 2.2 Roles

Rules gave a first guess (`role_fit()` in the script):

- **Bass:** pitch ≤ −12, attack ≤ 30 ms, release < 2 s.
- **Chords:** attack ≤ 60 ms, release < 1.6 s.
- **Pad:** attack ≥ 0.1 s, or sustain ≥ 30 with release ≥ 1 s.
- **Lead:** attack ≤ 0.1 s, pitch ≥ −12, release < 2 s.

Each preset was then reviewed by hand against its oscillators, cables, FX and name (`REVIEW` in the script). The first role listed is the best fit. Riser, Fall and Impact mark FX-lane fallbacks. "–" means avoid.

| Role | Presets that list it | Best fit for |
|---|---|---|
| Bass | 38 | 33 |
| Chords | 29 | 17 |
| Pad | 53 | 47 |
| Lead | 71 | 59 |
| FX fallback (Riser / Fall / Impact) | 7 | 4 |
| – (avoid) | 11 | 11 |

Bass fits well: 29 preset names contain "Bass". Lead is the biggest pool because most plain mono-capable sounds, bells and plucks count. Pad is well covered by the pad block (114–157). **Chords is the weak role.** Stab-shaped presets are mostly organs and electric pianos. Several of them add a fifth or a third on every note, which turns a voiced 7th chord into a cluster.

### 2.3 What fights the Generator

| Flag | Presets | Why it fights | Fix |
|---|---|---|---|
| **Saved arp** (`ARP`) | 15: 029, 108, 111, 112, 113, 121, 159–167 | Old files store `<arpeggiator><mode>`. Any mode but `off` turns the arp on (`modulation/arpeggiator.cpp:1751-1763`, `util/functions.cpp:980-987`). The preset fills `SoundInstrument::defaultArpSettings` (`processing/sound/sound_instrument.cpp:85`), and the clip copies that when it is linked (`model/clip/instrument_clip.cpp:2310-2316`). The arp then plays its own pattern over the Generator's notes. | **Recommend:** the Generator switches the clip's arp off on load, just as it sets voice mode. Then 12 of these become usable and 7 have no other flag (029, 160, 161, 162, 165, 166, 167). One of them is 165 Acid Arp, the closest thing to an acid line on the card. |
| **Off-key** | 8: 040, 060, 084, 102, 128, 146, 154, 164 | No audible oscillator sits on the played pitch class. 040 and 164 sound a semitone sharp, 128 a semitone flat, 060 and 102 a fifth off, 146 a fourth off, 154 a fifth up and 084 eight semitones down. Every note lands outside the key. The Generator never changes tuning. | Leave them out of palettes. 128 is kept only as a Riser stand-in, where pitch matters less. |
| **Adds an interval** | 30 | A second oscillator or carrier stacks a fifth (19 presets), a third (7), a 2nd (3) or a tritone (1) on every note. A fifth is harmless on Bass, Lead and a pedal drone. On Chords it turns triads into clusters. A third clashes whenever the chord's third differs. Quiet high harmonics (≤ 16 volume, ≥ 2 octaves up, as in 071 Rhodes and 073 Piano) are timbre and aren't flagged. | Chords palettes avoid "adds 5th" and "adds 3rd". Pad palettes avoid "adds 3rd". |
| **Portamento saved** | 26 | In any mode except LEGATO, a new voice glides from the last note played whenever portamento isn't at minimum (`model/voice/voice.cpp:181-190`, `processing/sound/sound.cpp:1693`). On a Chords or Pad part every chord tone slides in from the previous note. | **Recommend:** the Generator also sets portamento **off** on load for parts that don't slide. #13 only says what happens to parts that do. |
| **Long release** (`rel`) | 46 with release ≥ 2 s (11 of them ≥ 5 s) | Tails overlap the next note on Bass, Lead and Chords. It's fine on Pad. | Decay control (release ±6) can trim, but only partly. Prefer short-release presets for those roles. |
| **Heavy FX** (`rev`, `dly`, `sat`, `crush`, mod FX) | reverb ≥ 20: 8, delay feedback ≥ 20: 8, saturation ≥ 5: 9, bitcrush/decimation ≥ 5: 12, mod FX depth ≥ 30: 6 | Space and Drive have little headroom left, and a wet preset blurs fast parts. Saturation and mod FX type aren't params, so they can't be moved at all ([#26](https://github.com/gismya/deluge-generative/issues/26)). | Weight them low, or keep them for Pad and Ambient. |
| **Pitch wobble** | 9 | An LFO or random source moves pitch far enough to blur a line (≥ 15% on `pitch`, or ≥ 40% on one oscillator). | Avoid for Bass and Lead lines. |

Unison is high on many pads (4–8 voices). Combined with 4-note POLY chords that is up to 32 voices per part. I didn't check the voice budget. It is on the open list.

### 2.4 Clean candidates per role

A candidate is "clean" if it has no flag that matters for that role. Long release doesn't count against Pad. A fifth counts against Chords, and a third or 2nd counts against Chords and Pad. An arp counts as fixable if the recommendation above is taken.

| Role | Listed | Clean | Clean once the arp is off | Other flag |
|---|---|---|---|---|
| Bass | 38 | 28 | 4 | 6 |
| Chords | 29 | 18 | 0 | 11 |
| Pad | 53 | 37 | 0 | 16 |
| Lead | 71 | 40 | 5 | 26 |

### 2.5 Full classification

Columns: **Engine** (sub = subtractive, ring = ringmod), saved **Voice** mode (overridden on load), **Uni** = unison voices, **Pitch** = transpose of the main oscillator in semitones, env1 **Atk** and **Rel** in seconds, **Sus** on the 0–50 menu scale, **Rule fit** (first guess), **Roles** (hand review, best first), **Flags** (§2.3), **Why**.

| # | Preset | Engine | Voice | Uni | Pitch | Atk s | Sus | Rel s | Rule fit | Roles | Flags | Why |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 000 | Rich Saw Bass | sub | poly | 4 | -12 | 0.00 | 50 | 0.1 | Bass Chords Lead | Bass |  | unison saw + square sub, env2 filter pluck |
| 001 | Sync Bass | sub | auto | 4 | -24 | 0.00 | 50 | 0.0 | Bass | Bass |  | LFO sweeps the synced osc2: moving timbre |
| 002 | Basic Square Bass | sub | auto | 2 | -24 | 0.00 | 44 | 1.3 | Bass Pad | Bass |  | round square, 1.3 s release |
| 003 | Synthwave Bass | sub | auto | 4 | -24 | 0.00 | 50 | 1.4 | Bass Pad | Bass |  | unison square/saw |
| 004 | Dubby Bass | sub | auto | 2 | -12 | 0.00 | 44 | 1.3 | Bass Chords Pad Lead | Bass |  | dub bass by name; env2 on osc A, light chorus |
| 005 | Sweet Mono Bass | sub | mono | 4 | -12 | 0.00 | 50 | 1.4 | Bass Chords Pad Lead | Bass |  | mono 4-voice unison, dark filter |
| 006 | Vaporwave Bass | sub | legato | 2 | -24 | 0.00 | 35 | 0.4 | Bass | Bass |  | saved legato + portamento |
| 007 | Detuned Saw Bass | sub | auto | 1 | -24 | 0.00 | 50 | 0.0 | Bass | Bass |  | plain detuned saw |
| 008 | FM Rich Distorted Bass | FM | auto | 8 | -24 | 0.00 | 29 | 0.3 | Bass | Bass |  | 8-voice FM unison, feedback envelope: gritty |
| 009 | Hoover Bass | sub | legato | 2 | -24 | 0.00 | 50 | 0.1 | Bass | Bass, Lead |  | rave hoover, legato + portamento saved |
| 010 | Gravel Basscamp | sub | poly | 2 | +0 | 0.00 | 50 | 0.0 | Chords Lead | Bass | pitch wobble | rough: LFO on osc2 pitch, random FM feedback |
| 011 | Dubstep Bass | sub | mono | 1 | -36 | 0.00 | 50 | 0.7 | Bass | Bass |  | pitch + noise envelope growl, −36 st |
| 012 | Blunt Sync Bass | sub | auto | 5 | -24 | 0.00 | 40 | 0.2 | Bass | Bass |  | sync sweep stab |
| 013 | Trap Bass 1 | sub | legato | 1 | -24 | 0.00 | 19 | 0.1 | Bass | Bass |  | 808-style long decay, legato |
| 014 | Trap Bass 2 | sub | mono | 1 | -36 | 0.00 | 50 | 3.0 | Pad | Bass, Impact | rel 3s, sat 7 | 808 boom with 3 s tail, saturated |
| 015 | Resonant Filter Bass | ring | auto | 2 | -24 | 0.00 | 45 | 0.1 | Bass | Bass, Lead | ring (inharmonic) | resonant (42) ringmod: acid-capable |
| 016 | Dark Saturated Bass | sub | mono | 5 | -12 | 0.00 | 50 | 3.3 | Pad | Bass | rel 3s | saturated, 3.3 s tail |
| 017 | Impact Saw Lead | sub | poly | 2 | +0 | 0.00 | 39 | 0.1 | Chords Lead | Lead, Chords |  | bright saw |
| 018 | Rich Saw Lead | sub | poly | 3 | +0 | 0.21 | 27 | 0.9 | Pad | Lead |  | 0.2 s soft attack |
| 019 | Fizzy Strings | sub | poly | 3 | +0 | 0.08 | 29 | 2.1 | – | Pad | rel 2s | string pad, 2 s release |
| 020 | Soft Saw Lead | sub | poly | 3 | +0 | 0.08 | 29 | 2.1 | – | Lead, Pad | rel 2s | soft saw, 2 s release |
| 021 | 80s TV Lead | sub | poly | 1 | +0 | 0.00 | 37 | 0.9 | Chords Lead | Lead |  | plain square lead |
| 022 | Rich Filter LFO Lead | sub | poly | 2 | +0 | 0.00 | 50 | 3.2 | Pad | Lead | rel 3s | filter-LFO lead, 3 s release |
| 023 | Analog Mono Wow | sub | mono | 4 | -17 | 0.03 | 50 | 0.1 | Chords | Bass | adds 5th | root + fifth, LFO filter wow |
| 024 | Warble Bass Pluck | sub | poly | 4 | -12 | 0.00 | 50 | 5.8 | Pad | Lead | rel 6s | pluck with a 6 s tail |
| 025 | Soft Synth Organ | sub | poly | 1 | +12 | 0.01 | 11 | 1.3 | Chords Lead | Chords |  | soft organ pluck |
| 026 | PW Organ | sub | poly | 1 | +12 | 0.01 | 45 | 1.0 | Chords Lead | Chords |  | pulse organ: stab or skank |
| 027 | PW Envelope | sub | poly | 1 | +0 | 0.00 | 37 | 0.4 | Chords Lead | Lead, Chords |  | pulse-width envelope |
| 028 | PWM | sub | poly | 1 | +0 | 0.00 | 44 | 0.0 | Chords Lead | Chords, Lead |  | PWM |
| 029 | Chiptune Trill | sub | poly | 1 | +0 | 0.00 | 50 | 0.0 | Chords Lead | Lead | ARP | chip lead under a saved arp |
| 030 | Distant Porta | sub | auto | 2 | +12 | 0.00 | 44 | 1.3 | Chords Pad Lead | Lead |  | portamento lead with delay |
| 031 | Nasal Choir | sub | poly | 4 | +0 | 0.00 | 50 | 0.1 | Chords Lead | Chords, Pad |  | choir-ish, short release |
| 032 | Bandpass Choir | ring | poly | 3 | +0 | 0.00 | 45 | 2.1 | Pad | Pad | rel 2s, ring (inharmonic) | ringmod choir, HPF |
| 033 | Rich Square | ring | poly | 2 | +12 | 0.00 | 40 | 0.2 | Chords Lead | Lead, Chords |  | ringmod square |
| 034 | Square Choir | sub | poly | 1 | +0 | 0.00 | 50 | 1.9 | Pad Lead | Pad, Chords |  | square choir, 1.9 s release |
| 035 | Bell Lead & Bass | sub | mono | 8 | +0 | 0.00 | 30 | 1.3 | Chords Pad Lead | Lead, Bass |  | mono 8-voice unison bell |
| 036 | Analog Ambient Square | sub | poly | 1 | +0 | 0.00 | 35 | 2.2 | Pad | Pad, Lead | rel 2s | ambient square, 2.2 s release |
| 037 | Echo Chord | sub | poly | 1 | -12 | 0.00 | 50 | 1.1 | Bass Chords Pad Lead | Chords | dly 20, adds 5th | fifth on every note + delay: dub-techno chord |
| 038 | Vapor Arp | sub | poly | 1 | +0 | 0.00 | 18 | 2.4 | – | Lead | rel 2s | chorused pluck, 2.4 s tail |
| 039 | Detuned Retriggering Saws | ring | poly | 1 | -12 | 0.01 | 40 | 0.2 | Bass Chords Lead | Lead, Bass |  | ringmod saws |
| 040 | Spacer Leader | sub | legato | 1 | -11 | 0.09 | 50 | 2.9 | Pad | – | rel 3s, pitch wobble, off-key +1 | whole sound a semitone sharp |
| 041 | Zithar - Vibed | sub | poly | 8 | +0 | 0.00 | 50 | 3.2 | Pad | Lead | rel 3s | vibrato pluck, 3 s tail |
| 042 | High Triangle | sub | poly | 1 | +24 | 0.00 | 50 | 0.0 | Chords Lead | Lead |  | pure high triangle |
| 043 | Square Porta | sub | auto | 1 | +0 | 0.00 | 50 | 0.0 | Chords Lead | Lead |  | plain square |
| 044 | 8-Bit Lead | sub | auto | 1 | +0 | 0.00 | 50 | 0.8 | Chords Lead | Lead |  | chip lead, pitch blip |
| 045 | Square Sync | sub | poly | 1 | +0 | 0.00 | 40 | 0.2 | Chords Lead | Lead |  | sync lead |
| 046 | Saw Sync | sub | poly | 1 | -12 | 0.00 | 40 | 0.2 | Bass Chords Lead | Lead, Bass |  | sync lead, an octave down |
| 047 | Basic Dirty Bass | sub | poly | 1 | -24 | 0.00 | 50 | 0.0 | Bass | Bass |  | plain dirty bass |
| 048 | Thin Pulse Bass | sub | auto | 1 | -36 | 0.00 | 50 | 0.7 | Bass | Bass |  | thin pulse, −36 st |
| 049 | Basic FM | FM | poly | 1 | +0 | 0.00 | 40 | 0.1 | Chords Lead | Lead, Chords |  | plain FM |
| 050 | FM Basic Bass | FM | poly | 1 | -24 | 0.00 | 29 | 0.3 | Bass | Bass |  | plain FM bass |
| 051 | FM Rich Brass | FM | poly | 2 | -12 | 0.01 | 10 | 0.9 | Bass Chords Lead | Lead | adds 3rd | brass; carrier 2 adds a major 10th |
| 052 | Soft FM | FM | poly | 2 | +0 | 0.05 | 45 | 4.3 | Pad | Pad | rel 4s | soft FM, 4.3 s release |
| 053 | Detuned FM Horns | FM | poly | 1 | +0 | 0.42 | 10 | 1.0 | Pad | Pad | rev 22 | horn swell, 0.4 s attack, reverb |
| 054 | Ghostly Sines | FM | poly | 1 | +12 | 0.13 | 0 | 1.3 | Pad | Pad, Lead |  | sine swell that fades |
| 055 | FM Theremin | FM | poly | 1 | +0 | 0.00 | 22 | 1.6 | Lead | Lead |  | theremin, portamento saved |
| 056 | FM Bell Modulation | FM | poly | 4 | +0 | 0.01 | 50 | 10.0 | Pad | Pad | rel 10s | FM bell drone, 10 s release |
| 057 | FM Lead | FM | poly | 2 | +0 | 0.01 | 39 | 0.2 | Chords Lead | Lead |  | FM lead |
| 058 | FM Rising Attack | FM | auto | 3 | +0 | 0.05 | 20 | 0.1 | Chords Lead | Lead, Chords |  | FM pluck |
| 059 | Distorted Lead Guitar | FM | auto | 1 | +0 | 0.04 | 21 | 0.2 | Chords Lead | Lead | sat 8 | FM guitar, saturated |
| 060 | Bass Guitar | FM | poly | 1 | -5 | 0.00 | 10 | 0.3 | Chords Lead | – | off-key +7 | whole sound a fifth off (−5 st) |
| 061 | Blown-Staccato-Panpipes | sub | poly | 3 | +0 | 0.08 | 41 | 2.1 | Pad | Lead | rel 2s, sat 5 | breathy panpipe, noise 50 |
| 062 | Trumpet | FM | poly | 1 | +0 | 0.05 | 42 | 0.3 | Chords Lead | Lead |  | trumpet: dub horn |
| 063 | Tuba | FM | poly | 1 | -24 | 0.01 | 29 | 0.3 | Bass | Bass |  | tuba |
| 064 | Reeds-Flute-Oboe | sub | poly | 1 | +0 | 0.25 | 50 | 0.0 | Pad | Lead | rev 29 | reed: melodica stand-in, reverb |
| 065 | Cello | FM | poly | 1 | +24 | 0.33 | 50 | 0.4 | Pad | Pad | adds 2nd | cello; carrier 2 adds a major 2nd |
| 066 | Violin | sub | poly | 1 | +0 | 0.16 | 35 | 0.5 | Pad | Lead, Pad |  | violin |
| 067 | Marimba | FM | poly | 2 | -12 | 0.00 | 9 | 0.1 | Bass Chords Lead | Lead |  | marimba: mallet pluck |
| 068 | FM Bells 1 | FM | poly | 1 | +0 | 0.00 | 15 | 0.9 | Lead | Lead |  | FM bell |
| 069 | FM Bells 2 | FM | poly | 1 | +0 | 0.00 | 15 | 0.9 | Chords Lead | Lead |  | FM bell |
| 070 | Glockenspiel | ring | poly | 1 | +12 | 0.00 | 45 | 2.1 | Pad | Lead | rel 2s, ring (inharmonic) | ringmod glockenspiel, 2 s tail |
| 071 | Rhodes | FM | poly | 3 | +0 | 0.00 | 20 | 0.3 | Chords Lead | Chords, Lead |  | Rhodes |
| 072 | Kyoto Phono | FM | poly | 2 | -12 | 0.00 | 9 | 1.6 | Bass Lead | Lead |  | FM pluck with delay |
| 073 | Piano | FM | poly | 3 | +0 | 0.00 | 24 | 0.2 | Chords Lead | Chords, Lead |  | piano |
| 074 | Electric Piano | FM | poly | 1 | +12 | 0.01 | 13 | 0.5 | Lead | Chords, Lead |  | electric piano, flanger |
| 075 | Electric Piano With Strings | FM | poly | 3 | +12 | 0.01 | 45 | 2.2 | Pad | Pad, Chords | rel 2s | EP + strings, 2.2 s release |
| 076 | Organ | FM | poly | 2 | +24 | 0.00 | 23 | 0.2 | Chords Lead | Chords | adds 5th | drawbar-style organ |
| 077 | FM Perc-Organ | FM | poly | 1 | +0 | 0.00 | 22 | 0.1 | Chords Lead | Chords |  | percussive organ: skank |
| 078 | House | FM | poly | 2 | +0 | 0.00 | 0 | 2.4 | – | Chords | rel 2s | house organ stab (sustain 0) |
| 079 | Phased Arper | sub | mono | 4 | -24 | 0.00 | 50 | 0.1 | Bass | Bass |  | mono −24 st, noise |
| 080 | House | sub | auto | 1 | -12 | 0.00 | 4 | 0.3 | Bass Lead | Bass, Chords | adds 5th | house organ bass (osc2 +19 st) |
| 081 | Xylophone Big Bass | FM | poly | 8 | -24 | 0.02 | 0 | 13.6 | – | Impact | rel 14s | FM boom, 13.6 s release |
| 082 | Short Sharp Delay | sub | poly | 2 | +12 | 0.00 | 2 | 0.2 | Chords Lead | Chords, Lead |  | short stab into delay: skank |
| 083 | Dark Chorus | ring | auto | 2 | +0 | 0.00 | 45 | 0.6 | Chords Lead | Chords, Pad |  | chorused ringmod |
| 084 | FM Narrow Band | FM | poly | 2 | -8 | 0.00 | 50 | 0.6 | Chords Lead | – | off-key +4 | whole sound shifted 8 st down |
| 085 | Deep Fizz | sub | poly | 1 | -12 | 0.00 | 50 | 0.0 | Bass Chords Lead | Bass, Lead | crush | fizzy, bitcrushed |
| 086 | Techno Organ | sub | poly | 1 | +0 | 0.00 | 50 | 0.1 | Chords Lead | Chords |  | organ stab |
| 087 | Define Leader | sub | mono | 1 | +12 | 0.00 | 42 | 0.0 | Chords Lead | Lead | sat 7 | saturated mono lead, flanger |
| 088 | Yelp Chords | ring | poly | 1 | +0 | 0.00 | 50 | 0.2 | Chords Lead | Chords |  | ringmod stab with a pitch yelp |
| 089 | Degraded Retro Lead | sub | auto | 1 | +0 | 0.00 | 50 | 0.0 | Chords Lead | Lead |  | degraded lead |
| 090 | FM Organ | FM | poly | 4 | +24 | 0.00 | 48 | 0.1 | Chords Lead | Chords, Pad | adds 5th | FM organ |
| 091 | FM Ricochet | FM | poly | 2 | -12 | 0.00 | 10 | 0.9 | Bass Chords Lead | Lead | adds 3rd | FM pluck; adds a major 10th |
| 092 | Degraded Tremolo | FM | poly | 2 | -12 | 0.01 | 5 | 0.9 | Bass Chords Lead | Lead | adds 3rd | tremolo FM; adds a major 10th |
| 093 | FM Distorted Bells | FM | poly | 1 | -24 | 0.00 | 15 | 0.9 | Bass | Lead |  | low FM bell |
| 094 | Ambient Occlusion Lead | sub | auto | 1 | +0 | 0.00 | 50 | 3.3 | Pad | Lead | rel 3s, phaser 33, crush, adds 5th | ambient lead: phaser, crush, 3 s tail |
| 095 | Harsh FM Feedback | FM | auto | 1 | -12 | 0.00 | 50 | 0.2 | Bass Chords Lead | Bass, Lead |  | FM feedback grit |
| 096 | FM Guitar Power Chord | FM | poly | 1 | +0 | 0.00 | 50 | 0.2 | Chords Lead | Lead | adds 5th | power chord on every note |
| 097 | Saturated Filter | sub | auto | 1 | +0 | 0.00 | 40 | 0.2 | Chords Lead | Lead | sat 7 | saturated resonant filter: acid-capable |
| 098 | Saturated Sync | sub | auto | 1 | -12 | 0.00 | 50 | 0.0 | Bass Chords Lead | Lead | sat 9 | saturated sync |
| 099 | Overdrive Reese Sync | ring | poly | 2 | +0 | 0.00 | 50 | 0.0 | Chords Lead | Bass | sat 5 | Reese: saturated ringmod + chorus |
| 100 | Noise Lead | sub | poly | 1 | +0 | 0.00 | 50 | 0.1 | Chords Lead | Lead | crush | bitcrushed noise lead |
| 101 | Atebit | sub | poly | 1 | +7 | 0.00 | 50 | 0.0 | Chords Lead | Lead | adds 5th | chip lead, fifth stacked |
| 102 | Harsh 5th | sub | poly | 1 | -5 | 0.00 | 50 | 0.1 | Chords Lead | – | off-key +7 | whole sound a fifth off |
| 103 | Sci-fi Chaos | ring | poly | 3 | +0 | 0.00 | 3 | 5.4 | – | Fall | rel 5s | pitch falls with the amp envelope over ~19 s |
| 104 | Alien Vomit | FM | poly | 4 | -12 | 0.56 | 50 | 1.7 | Pad | – | adds 2nd | semitone cluster (carrier 2 at −1) |
| 105 | Attack Bass | ring | poly | 1 | +7 | 0.01 | 18 | 0.5 | Lead | Bass | ring (inharmonic) | ringmod: difference tone an octave down |
| 106 | Hang Drum | FM | poly | 1 | +5 | 0.01 | 9 | 3.2 | – | Lead | rel 3s, adds 5th | FM hang drum, 3 s tail |
| 107 | FM LPG Percussion | sub | poly | 1 | +6 | 0.00 | 20 | 5.0 | – | – | rel 5s, pitch wobble, adds tritone | tritone carrier, random pitch |
| 108 | Robo Arp | sub | poly | 1 | +0 | 0.00 | 50 | 0.0 | Chords Lead | Lead | ARP, crush | robot lead under a saved arp, crushed |
| 109 | Talking Arp | sub | poly | 4 | -17 | 0.05 | 23 | 0.1 | Chords | Lead | crush, adds 5th | talking filter, fifth stacked |
| 110 | Crystalline Ringmod | ring | poly | 4 | +0 | 0.05 | 22 | 1.4 | Chords Lead | Lead | dly 23, chorus 41, crush, ring (inharmonic) | glassy ringmod, delay + chorus + crush |
| 111 | Satellite Drum | sub | poly | 1 | -12 | 0.00 | 50 | 0.4 | Bass Chords Lead | – | ARP, pitch wobble | siren: LFO on both osc pitches, saved arp |
| 112 | Hard Tech Beat | sub | mono | 1 | -24 | 0.00 | 0 | 14.6 | – | Impact | ARP, rel 15s | noise + pitch-drop hit, saved arp, 15 s release |
| 113 | Bio Lab | FM | poly | 2 | -12 | 0.00 | 50 | 0.0 | Bass Chords Lead | – | ARP, crush, pitch wobble | random pitch chaos, saved arp |
| 114 | Sootheerio | FM | poly | 1 | -5 | 0.34 | 50 | 0.7 | Pad | Pad | adds 5th | FM swell |
| 115 | Sounds After Take-off | sub | poly | 1 | -12 | 0.02 | 50 | 0.9 | Bass Chords Lead | Lead | crush | crushed lead |
| 116 | Evolving Frequencies | sub | poly | 4 | +12 | 0.00 | 42 | 5.8 | Pad | Pad, Riser | rel 6s | noise envelope, 5.8 s release |
| 117 | Belledy | sub | poly | 1 | +12 | 0.00 | 50 | 0.6 | Chords Lead | Lead | adds 3rd | adds a minor 3rd (major 6th below) |
| 118 | Small Bridge Pad | sub | poly | 2 | -12 | 2.97 | 10 | 4.8 | Pad | Pad | rel 5s, dly 25 | 3 s attack, 4.8 s release, delay |
| 119 | Stars Of The Bin Pad | sub | poly | 1 | +0 | 2.97 | 50 | 3.5 | Pad | Pad | rel 3s, rev 39, dly 22, crush, adds 5th | 3 s attack, heavy reverb + delay |
| 120 | High Harsh Pad | sub | poly | 4 | +0 | 0.00 | 50 | 0.1 | Chords Lead | Pad, Chords | crush | sustained harsh unison, short release |
| 121 | Tiny Lights | sub | poly | 4 | +0 | 0.01 | 50 | 0.9 | Chords Lead | Lead | ARP, rev 21 | bell lead under a saved arp, reverb |
| 122 | Majestic Synth Orchestra | FM | poly | 2 | +0 | 0.00 | 50 | 2.1 | Pad | Pad | rel 2s, adds 2nd | carrier 2 adds a 9th |
| 123 | Space Dust | FM | poly | 3 | +12 | 0.01 | 45 | 2.2 | Pad | Pad | rel 2s | FM shimmer, flanger |
| 124 | Filter Modulation Pad | sub | poly | 2 | +0 | 0.00 | 45 | 2.1 | Pad | Pad | rel 2s | LFO filter pad |
| 125 | Evolving Pad | ring | mono | 4 | -24 | 0.00 | 50 | 0.6 | Bass | Pad |  | mono −24 st ringmod drone |
| 126 | Dark FM Pad | FM | poly | 4 | +0 | 0.59 | 50 | 1.7 | Pad | Pad | adds 5th | dark FM pad |
| 127 | Alien Larvae | FM | poly | 3 | +0 | 0.40 | 50 | 3.5 | Pad | Pad | rel 4s, adds 3rd | adds a major 3rd, 3.5 s release |
| 128 | Lunar Landing | sub | poly | 1 | -13 | 0.01 | 40 | 11.2 | Pad | Riser | rel 11s, dly 21, chorus 40, crush, pitch wobble, off-key +11 | semitone flat, warbling; env2 opens filter over 3 s |
| 129 | Sci-fi Scenic | sub | auto | 1 | -12 | 2.97 | 41 | 1.6 | Pad | Pad, Riser | dly 20, chorus 37, crush | 3 s swell, noise 30, closed filter |
| 130 | Dark Strings | FM | poly | 4 | -12 | 0.24 | 23 | 1.6 | Pad | Pad |  | FM strings |
| 131 | Warm Strings | sub | poly | 2 | +0 | 2.97 | 50 | 1.0 | Pad | Pad |  | string pad, 3 s attack |
| 132 | Organ Strings | FM | poly | 4 | +24 | 0.93 | 48 | 0.6 | Pad | Pad | adds 5th | FM organ strings |
| 133 | 80s Strings | sub | poly | 3 | +0 | 0.00 | 27 | 2.0 | – | Pad | rel 2s, rev 50, dly 20 | strings, reverb 50 |
| 134 | Melody String | sub | poly | 4 | +0 | 0.56 | 50 | 2.8 | Pad | Pad, Lead | rel 3s | string pad, 2.8 s release |
| 135 | Soothing Growth Pad | sub | poly | 6 | +0 | 0.00 | 50 | 0.5 | Chords Lead | Pad | rev 35 | unison pad, auto-pan, reverb 35 |
| 136 | Synthwave Pad | sub | poly | 1 | +0 | 0.00 | 40 | 0.9 | Chords Lead | Pad, Chords | phaser 30 | phaser pad |
| 137 | Epic Saw Modulation Pad | sub | poly | 1 | +0 | 0.00 | 40 | 0.9 | Chords Lead | Pad, Chords | rev 26, phaser 30 | phaser pad, reverb |
| 138 | Brassy Pad | sub | poly | 2 | +0 | 1.80 | 40 | 0.6 | Pad | Pad |  | brass pad, 1.8 s attack |
| 139 | Detuned Saw Pad | sub | poly | 1 | +0 | 1.12 | 50 | 1.7 | Pad | Pad |  | detuned saw pad, 1.1 s attack |
| 140 | Slow Aural Swells | sub | poly | 4 | -12 | 0.00 | 50 | 2.4 | Pad | Pad | rel 2s | unison swell, 2.4 s release |
| 141 | Ringmod Pad | ring | poly | 1 | +0 | 1.29 | 40 | 1.4 | Pad | Pad |  | ringmod pad |
| 142 | Phaser | sub | poly | 1 | +0 | 0.59 | 40 | 0.9 | Pad | Pad |  | phaser pad |
| 143 | Chillout Pad | sub | poly | 1 | -12 | 0.56 | 50 | 2.8 | Pad | Pad | rel 3s | noisy pad |
| 144 | Sweep Chords | sub | poly | 1 | +0 | 0.15 | 50 | 1.4 | Pad | Chords, Pad | sat 5 | swept chord, 0.15 s attack |
| 145 | Eerie High Pad | ring | poly | 2 | +24 | 2.97 | 50 | 1.0 | Pad | Pad |  | high ringmod pad |
| 146 | Atmospheric Squares Pad | sub | poly | 4 | +5 | 2.97 | 27 | 2.8 | Pad | – | rel 3s, dly 20, pitch wobble, off-key +5 | whole sound a fourth off |
| 147 | Resonant Filter Pad | sub | poly | 3 | +0 | 2.97 | 27 | 3.7 | Pad | Pad | rel 4s | resonant pad, 3.7 s release |
| 148 | Warm 5th Pad | sub | poly | 1 | -12 | 0.40 | 50 | 0.5 | Pad | Pad | adds 5th | fifth pad: drone |
| 149 | Cold 5th Pad | sub | poly | 4 | -5 | 2.97 | 50 | 3.5 | Pad | Pad | rel 3s, adds 5th | fifth pad: drone, 3.5 s release |
| 150 | Vaporwave Pad | sub | auto | 1 | +0 | 0.00 | 43 | 0.7 | Chords Lead | Pad, Chords | adds 5th | chorused fifth pad |
| 151 | Radiant FM Pad | FM | poly | 2 | -12 | 1.80 | 34 | 1.3 | Pad | Pad |  | FM pad, 1.8 s attack |
| 152 | Small Jet Pad | sub | poly | 2 | -12 | 0.71 | 9 | 5.4 | Pad | Pad | rel 5s, rev 23, pitch wobble | jet pad, 5.4 s release |
| 153 | FM Modulation Pad | FM | poly | 1 | -12 | 0.11 | 20 | 1.0 | Pad | Pad |  | FM pad, chorus |
| 154 | Rich FM Pad 1 | FM | poly | 2 | +7 | 0.67 | 50 | 2.2 | Pad | – | rel 2s, off-key +7 | whole sound a fifth up |
| 155 | Rich FM Pad 2 | FM | poly | 4 | -12 | 1.53 | 50 | 3.7 | Pad | Pad | rel 4s, adds 3rd | adds a major 3rd |
| 156 | Rich FM Pad 3 | FM | poly | 3 | +0 | 0.40 | 50 | 3.5 | Pad | Pad | rel 4s, adds 3rd | adds a major 3rd |
| 157 | Rich FM Pad 4 | FM | poly | 2 | +0 | 0.59 | 50 | 1.7 | Pad | Pad | adds 5th | FM pad |
| 158 | Tempo-Synced LFO | sub | auto | 5 | -12 | 0.00 | 50 | 0.0 | Bass Chords Lead | Bass |  | synced LFO on the filter: wobble |
| 159 | 80s Bass Rhythm | sub | poly | 1 | -12 | 0.00 | 5 | 2.1 | – | Bass | ARP, rel 2s | bass under a saved arp |
| 160 | Synthwave Bass Arp | sub | poly | 4 | -12 | 0.00 | 50 | 0.3 | Bass Chords Lead | Bass | ARP | bass under a saved arp |
| 161 | Synthwave Vibrato Arp | FM | poly | 1 | -24 | 0.01 | 29 | 0.3 | Bass | Bass | ARP | FM bass under a saved arp |
| 162 | Busy Arp | sub | poly | 3 | +0 | 0.00 | 50 | 1.2 | Chords Pad Lead | Lead | ARP | arp lead under a saved arp |
| 163 | Crisp Pop Arp | sub | poly | 3 | +0 | 0.08 | 41 | 1.3 | Pad Lead | Lead | ARP, sat 5 | arp lead under a saved arp |
| 164 | Study Arp | sub | legato | 1 | -11 | 0.00 | 50 | 2.3 | Pad | – | ARP, rel 2s, pitch wobble, off-key +1 | semitone sharp, saved arp |
| 165 | Acid Arp | sub | mono | 1 | -36 | 0.00 | 50 | 0.2 | Bass | Lead, Bass | ARP | acid once the saved arp is off: mono, −36 st, res 33 |
| 166 | Harpsichord Cyborg | FM | poly | 2 | -12 | 0.00 | 50 | 0.7 | Bass Chords Lead | Lead | ARP, adds 5th | harpsichord under a saved arp |
| 167 | FM Metallic Bass Arp | FM | poly | 1 | +0 | 0.00 | 50 | 0.5 | Chords Lead | Lead, Bass | ARP | metallic FM under a saved arp |
| 168 | Hang Drum | sub | poly | 1 | +0 | 0.00 | 0 | 7.3 | – | Lead | rel 7s, sample | sampled hang drum, 7 s tail |
| 169 | Double Bass | sub | auto | 1 | -12 | 0.00 | 50 | 0.3 | Bass Chords Lead | Bass | sample | sampled double bass |
| 170 | Sitar | sub | poly | 1 | +0 | 0.00 | 50 | 8.3 | Pad | Lead | rel 8s, sample | sampled sitar, 8 s tail |

## 3. FM presets and the filter

**Short answer: none rely on the filter.** The firmware forces LPF and HPF off for FM presets saved before community 1.2.0 (`processing/sound/sound.cpp:3261-3265`). A file with no `<firmwareVersion>` tag counts as official with an empty version (`storage/Deserializer.cpp:62`). Every factory preset is official 2.0.0-beta, 2.1.0 or untagged. Filters in FM arrived in community c1.2.0 ("Added filters in FM synth mode", `website/src/content/docs/changelogs/CHANGELOG.mdx:491-493`). The code comment says such filter settings "wouldn't have rendered at the time". So every factory FM preset was designed with no filter in the signal. Forcing it off on load reproduces what the designer heard.

What the 54 FM presets have saved in the dead filter section:

- An `lpfMode` on all 54 (12 dB or 24 dB, never off).
- An LPF below fully open on 25.
- Cables into the LPF on 7: 052, 055, 056, 074, 075, 123, 130. Most are copies of one EP template.
- No HPF anywhere.

If the filters weren't forced off, these 25 would come out much darker. That only matters if someone hand-edits a pack copy to a new firmware version tag without clearing `lpfMode`. A copy re-saved on the device is safe, because the load already set both filters to off. Re-saving alone gains nothing. It only helps if the pack deliberately adds a filter, for example an HPF so Thin works on FM.

| # | Preset | Saved `lpfMode` | Saved filter settings that are now dead |
|---|---|---|---|
| 008 | FM Rich Distorted Bass | 12dB | – |
| 049 | Basic FM | 24dB | LPF 20 |
| 050 | FM Basic Bass | 12dB | – |
| 051 | FM Rich Brass | 12dB | – |
| 052 | Soft FM | 24dB | LPF 34, envelope2→lpfFrequency +30%, velocity→lpfFrequency +18%, note→lpfFrequency +12%, lfo1→lpfFrequency +6% |
| 053 | Detuned FM Horns | 24dB | – |
| 054 | Ghostly Sines | 24dB | LPF 20 |
| 055 | FM Theremin | 12dB | LPF 25, envelope1→lpfFrequency +40% |
| 056 | FM Bell Modulation | 24dB | LPF 28, velocity→lpfFrequency +24%, envelope2→lpfFrequency +44%, note→lpfFrequency +14% |
| 057 | FM Lead | 24dB | LPF 24 |
| 058 | FM Rising Attack | 24dB | LPF 20 |
| 059 | Distorted Lead Guitar | 24dB | LPF 28 |
| 060 | Bass Guitar | 24dB | LPF 6 |
| 062 | Trumpet | 24dB | LPF 28 |
| 063 | Tuba | 12dB | – |
| 065 | Cello | 24dB | LPF 30 |
| 067 | Marimba | 12dB | – |
| 068 | FM Bells 1 | 12dB | – |
| 069 | FM Bells 2 | 12dB | – |
| 071 | Rhodes | 12dB | – |
| 072 | Kyoto Phono | 12dB | – |
| 073 | Piano | 12dB | – |
| 074 | Electric Piano | 24dB | LPF 34, lfo1→lpfFrequency +6%, envelope2→lpfFrequency +30%, note→lpfFrequency +12%, velocity→lpfFrequency +18% |
| 075 | Electric Piano With Strings | 24dB | LPF 34, envelope2→lpfFrequency +30%, note→lpfFrequency +12%, velocity→lpfFrequency +18%, lfo1→lpfFrequency +6% |
| 076 | Organ | 24dB | LPF 30 |
| 077 | FM Perc-Organ | 24dB | – |
| 078 | House | 12dB | – |
| 081 | Xylophone Big Bass | 12dB | – |
| 084 | FM Narrow Band | 12dB | – |
| 090 | FM Organ | 24dB | LPF 30 |
| 091 | FM Ricochet | 12dB | – |
| 092 | Degraded Tremolo | 12dB | – |
| 093 | FM Distorted Bells | 12dB | – |
| 095 | Harsh FM Feedback | 12dB | – |
| 096 | FM Guitar Power Chord | 24dB | LPF 30 |
| 104 | Alien Vomit | 24dB | LPF 30 |
| 106 | Hang Drum | 24dB | – |
| 113 | Bio Lab | 24dB | – |
| 114 | Sootheerio | 24dB | – |
| 122 | Majestic Synth Orchestra | 24dB | – |
| 123 | Space Dust | 24dB | LPF 34, lfo1→lpfFrequency +6%, envelope2→lpfFrequency +30%, note→lpfFrequency +12%, velocity→lpfFrequency +18% |
| 126 | Dark FM Pad | 24dB | LPF 30 |
| 127 | Alien Larvae | 24dB | LPF 30 |
| 130 | Dark Strings | 24dB | LPF 34, envelope2→lpfFrequency +30%, note→lpfFrequency +12%, velocity→lpfFrequency +18%, lfo1→lpfFrequency +6% |
| 132 | Organ Strings | 24dB | LPF 30 |
| 151 | Radiant FM Pad | 12dB | – |
| 153 | FM Modulation Pad | 24dB | LPF 34 |
| 154 | Rich FM Pad 1 | 12dB | – |
| 155 | Rich FM Pad 2 | 12dB | – |
| 156 | Rich FM Pad 3 | 24dB | LPF 30 |
| 157 | Rich FM Pad 4 | 24dB | LPF 30 |
| 161 | Synthwave Vibrato Arp | 12dB | – |
| 166 | Harpsichord Cyborg | 24dB | – |
| 167 | FM Metallic Bass Arp | 24dB | – |

## 4. Drum samples

### 4.1 Count per folder against the lane table

The firmware only treats `.wav`, `.aif` and `.aiff` files as audio and skips dotfiles (`util/functions.cpp:1327-1333`). The `Clap` folder also holds 4 `.reapeaks` files, which don't count and must be filtered out by the Generator's own folder scan.

| Lane (#25 default folders) | Samples | Duration range (s) |
|---|---|---|
| Kick (`Kick`) | 23 | 0.09–4.09 |
| Snare (`Snare`) | 20 | 0.12–0.95 |
| Clap (`Clap`) | 8 | 0.18–2.00 |
| Rim (`Rim`) | 11 | 0.04–0.29 |
| Closed hat (`HatC`) | 16 | 0.04–0.34 |
| Open hat (`HatO`) | 15 | 0.23–2.25 |
| Ride (`Ride`) | 12 | 0.50–2.65 |
| Crash (`Crash`) | 12 | 0.25–3.19 |
| Tom (`TomL`, `TomM`, `TomH`) | 13 + 9 + 12 = 34 | 0.19–1.86 |
| Perc 1, Perc 2 (`Shaker`, `ConL`, `ConM`, `ConH`, `Cowbell`, `Tambourine`, `Claves`, `Block`, `Guiro`, `Other`) | 7+5+4+6+8+7+6+5+2+5 = 55 | 0.04–0.50 |
| *(no lane)* `China`, `Timpany` | 3, 4 | |

**No lane has fewer than three samples.** Within the Perc pool, `Guiro` (2) is the only folder under three. That matters only if a palette points a Perc lane at Guiro alone.

Per folder, with every file:

| Folder | Files | Duration (s) | Files (machine, kind) |
|---|---|---|---|
| Block | 5 | 0.08–0.25 | HR16B Block, R-50 Agog, R100 Woodblock, RX-5 Bongo high, RX-5 Bongo low |
| China | 3 | 0.50–1.65 | R-50 China, R100 Brush, RX-5 China |
| Clap | 8 | 0.18–2.00 | 808 Clap, 909 Clap, DDD1 Clap, HR16B Clap, Linndrum Clap, R-50 Clap, RX-5 Clap, Sequential Circuits Drumtraks Clap |
| Claves | 6 | 0.04–0.26 | 808 Claves, CR78 Claves, KR55 Claves, R-50 Claves, Rhythmace Claves, Rhythmace Percussion |
| ConH | 6 | 0.07–0.50 | 808 Conga high, CR78 Conga high, Linndrum Conga extra high, Linndrum Conga high, R100 Conga high, Rhythmace Conga high |
| ConL | 5 | 0.18–0.50 | 808 Conga low, CR78 Conga low, Linndrum Conga low, R100 Conga low, Rhythmace Conga low |
| ConM | 4 | 0.14–0.50 | 808 Conga medium, CR78 Conga medium, Linndrum Conga medium, R-50 Conga medium |
| Cowbell | 8 | 0.09–0.39 | 808 Cowbell, CR78 Cowbell, DDD1 Cowbell, KR55 Cowbell, R-50 Cowbell, R100 Cowbell, RX-5 Cowbell, Sequential Circuits Drumtraks Cowbell |
| Crash | 12 | 0.25–3.19 | 808 Cymbal, 909 Crash, CR78 Cymbal, DDD1 Crash, HR16B Crash, Hammond Rhythm II Crash, Linndrum Crash, R-50 Crash, R100 Crash, RX-5 Crash, Sequential Circuits Drumtraks Crash, XV5080 Crash |
| Guiro | 2 | 0.19–0.21 | CR78 Guiro, R100 Quijada |
| HatC | 16 | 0.04–0.34 | 808 Closed hihat, 909 Closed hihat, CR78 Closed hihat, DDD1 Closed hihat, HR16B Closed hihat, Hammond Rhythm II Closed hihat, KR55 Closed hihat, Linndrum Closed hihat, R-50 Closed hihat, R100 Closed hihat, RX-5 Closed hihat, Rhythmace Closed hihat, SDS5 Closed hihat, SDS5 Pedal hihat, Sequential Circuits Drumtraks Closed hihat, XV5080 Closed hihat |
| HatO | 15 | 0.23–2.25 | 808 Open hihat, 909 Open hihat, CR78 Open hihat, DDD1 Open hihat, HR16B Open hihat, Hammond Rhythm II Open hihat, KR55 Open hihat, Linndrum Open hihat, R-50 Open hihat, R100 Open hihat, RX-5 Open hihat, Rhythmace Open hihat, SDS5 Open hihat, Sequential Circuits Drumtraks Open hihat, XV5080 Open hihat |
| Kick | 23 | 0.09–4.09 | 808 Kick descending, 808 Kick distorted, 808 Kick sustained, 808 Kick, 909 Kick distorted, 909 Kick dry, 909 Kick, CR78 Kick, DDD1 Kick, Deep Sky Kick HQ9094, Deep Sky Kick descending, Deep Sky Kick distorted, HR16B Kick, Hammond Rhythm II Kick, KR55 Kick, Linndrum Kick, R-50 Kick, R100 Kick, RX-5 Kick, Rhythmace Kick, SDS5 Kick, Sequential Circuits Drumtraks Kick, XV5080 Kick |
| Other | 5 | 0.11–0.50 | CR78 Metal, R-50 Click, R-50 Snap, R100 Chime, R100 Triangle |
| Ride | 12 | 0.50–2.65 | 909 Ride, DDD1 Ride, KR55 Ride, Linndrum Ride, R-50 Ride cup, R-50 Ride, R100 Ride, RX-5 Ride cup, RX-5 Ride, Sequential Circuits Drumtraks Ride, XV5080 Ride cup, XV5080 Ride edge |
| Rim | 11 | 0.04–0.29 | 808 Rim, 909 Rim, CR78 Rim, DDD1 Rim, KR55 Rim, Linndrum Rim, R100 Rim, RX-5 Rim, SDS5 Rim, Sequential Circuits Drumtraks Rim, XV5080 Rim |
| Shaker | 7 | 0.06–0.29 | 808 Maraca, DDD1 Cabasa, Linndrum Cabasa, R-50 Shaker, R100 Percussion, R100 Shaker, Sequential Circuits Drumtraks Cabasa |
| Snare | 20 | 0.12–0.95 | 808 Snare, 909 Snare, CR78 Snare, DDD1 Snare dry, DDD1 Snare wet, HR16B Snare dry, HR16B Snare wet, Hammond Rhythm II Snare, KR55 Snare, Linndrum Snare, R-50 Snare, R100 Snare, RX-5 Snare low, RX-5 Snare, Rhythmace Snare, SDS5 Snare, Sequential Circuits Drumtraks Snare, XV5080 Snare dry, XV5080 Snare piccolo, XV5080 Snare |
| Tambourine | 7 | 0.16–0.50 | CR78 Tambourine, DDD1 Tambourine, HR16B Tambourine, Linndrum Tambourine, R-50 Tambourine, RX-5 Tambourine, Sequential Circuits Drumtraks Tambourine |
| Timpany | 4 | 0.35–0.40 | R-50 Timpany high, R-50 Timpany low, RX-5 Timpany high, RX-5 Timpany low |
| TomH | 12 | 0.19–1.16 | 808 Tom high, 909 Tom high, DDD1 Tom high, KR55 Tom high, Linndrum Tom extra high, Linndrum Tom high, R-50 Tom electronic high, R-50 Tom high, R100 Tom high, SDS5 Tom extra high, SDS5 Tom high, XV5080 Tom high |
| TomL | 13 | 0.32–1.86 | 808 Tom low, 909 Tom low, DDD1 Tom low, KR55 Tom low, Linndrum Tom extra low, Linndrum Tom low, R-50 Tom electronic low, R-50 Tom low, R100 Tom low, SDS5 Tom extra low, SDS5 Tom low, Sequential Circuits Drumtraks Tom low, XV5080 Tom low |
| TomM | 9 | 0.22–1.55 | 808 Tom medium, 909 Tom medium, DDD1 Tom medium, Linndrum Tom medium, R-50 Tom electronic medium, R-50 Tom medium, SDS5 Tom medium, Sequential Circuits Drumtraks Tom medium, XV5080 Tom medium |

### 4.2 Thin spots per genre

The folders are full, but filename tags narrow them. These are the tags the palettes in §6 lean on:

- **909** has Kick ×3 (plain, dry, distorted), Snare, Clap, Closed hat, Open hat, Ride, Crash, Rim and Toms. Ride and Clap have **one** file each, so a 909 techno ride never varies.
- **808** has Kick ×4, Snare, Clap (2 s long), hats, Rim, Toms, Cowbell, Claves, Congas and Maraca. It has **no ride**. Its crash is `808 Cymbal`.
- **Psytrance:** no tight, clicky modern kick or snare in `SAMPLES/DRUMS`. The closest are `Deep Sky Kick HQ9094`, `XV5080 Kick` and `909 Kick dry`. `SAMPLES/Artists/Dean Daughters/Psy` is a matching set: 3 KICK, 1 CLAP, 2 SNARE, 3 CLHAT, 3 OPHAT, 1 RIDE, 1 SHAKER, 2 PERC.
- **Dub:** `SDS5` toms (syndrum-like), `Rim` cross-sticks, the "wet" snares (`DDD1`, `HR16B`) and RX-5 bongos in `Block` all fit. Nothing is brushed or dubbed out beyond that.
- **Ambient:** soft material is short. `Other` has Chime and Triangle (one each), `Claves`, `Shaker` and woodblocks. Brushes are absent (`China` has one `R100 Brush`, but that folder belongs to no lane). Longer, softer one-shots live only in `SAMPLES/Artists`.
- **Mixed folders need tags as filters.** `SAMPLES/Artists/*` folders mix lanes (the Psy folder holds kicks and hats side by side). A lane that points there must list tags, and only files matching a tag should be eligible. #13's wording ("with no tag, any file in the folder can be picked") says nothing about tagged entries. The palette format should state that tags filter.

## 5. FX lanes

### 5.1 Samples

`SAMPLES/FX` doesn't exist on the factory card (confirmed: `SAMPLES` holds only `Artists`, `DRUMS`, `RECORD` and `RESAMPLE`, and the last two are empty). Candidates in `SAMPLES/Artists` were measured with `card_survey.py fxshape`. Each file is split into 8 equal slices. For each slice the script gives RMS loudness in dBFS and the zero-crossing rate per millisecond, a rough brightness measure. A riser should get louder and brighter, and a fall the reverse.

| File | Length (s) | RMS dBFS per eighth | Zero crossings per ms per eighth |
|---|---|---|---|
| Artists/Andrew Stirton/Frugal/frugalriser.wav | 13.7 | -73 -45 -28 -22 -15 -12 -13 -14 | 3.6 0.5 0.6 1.1 2.7 4.3 4.8 4.4 |
| Artists/Andrew Stirton/Frugal/befall.wav | 2.9 | -7 -8 -9 -9 -11 -12 -19 -52 | 0.6 0.8 1.0 0.5 0.2 0.5 0.4 2.0 |
| Artists/Andrew Stirton/Frugal/sprongle.wav | 3.0 | -8 -7 -7 -8 -18 -27 -36 -44 | 0.6 0.8 0.5 0.7 0.3 0.2 0.3 0.2 |
| Artists/Andrew Stirton/Frugal/wurgle.wav | 2.0 | -8 -15 -11 -15 -14 -13 -16 -23 | 0.7 0.2 0.3 0.4 0.3 0.4 0.5 0.7 |
| Artists/Danny Taurus/MoDrama/Upper01.wav | 10.0 | -88 -60 -43 -30 -22 -16 -21 -109 | 0.1 0.3 0.9 2.4 6.0 6.3 5.6 0.1 |
| Artists/Danny Taurus/MoDrama/Upper02.wav | 10.0 | -63 -37 -25 -19 -17 -17 -21 -97 | 0.1 0.3 0.8 2.1 4.5 5.6 4.9 0.3 |
| Artists/Danny Taurus/MoDrama/Upper03.wav | 10.0 | -59 -40 -30 -22 -15 -13 -21 -95 | 0.6 1.1 2.3 4.8 6.7 5.0 4.9 0.4 |
| Artists/Danny Taurus/MoDrama/Upper05.wav | 10.0 | -36 -54 -22 -30 -13 -13 -16 -85 | 0.7 0.5 1.2 0.8 4.5 5.2 5.4 0.5 |
| Artists/Danny Taurus/MoDrama/Upper08.wav | 10.0 | -67 -36 -25 -19 -16 -15 -19 -107 | 0.2 1.1 2.9 5.6 6.4 5.6 5.3 0.1 |
| Artists/Danny Taurus/MoDrama/Upper11.wav | 10.0 | -67 -40 -27 -23 -22 -25 -30 -106 | 2.0 3.6 5.2 6.2 5.6 4.8 5.0 0.2 |
| Artists/Danny Taurus/MoDrama/Upper14.wav | 10.0 | -72 -43 -29 -21 -19 -18 -21 -120 | 0.3 1.9 5.3 6.9 5.6 5.3 5.6 0.0 |
| Artists/Danny Taurus/MoDrama/Upper20.wav | 10.0 | -61 -29 -25 -22 -22 -23 -26 -107 | 0.1 0.4 0.5 0.7 0.9 1.2 1.1 0.0 |
| Artists/Danny Taurus/MoDrama/Downer01.wav | 8.0 | -23 -17 -18 -21 -25 -30 -41 -59 | 6.3 7.3 6.0 3.5 1.8 0.7 0.3 0.1 |
| Artists/Danny Taurus/MoDrama/Downer04.wav | 8.0 | -26 -25 -22 -23 -28 -36 -48 -70 | 5.5 5.0 4.0 2.9 1.9 1.0 0.4 0.1 |
| Artists/Danny Taurus/MoDrama/Downer05.wav | 8.0 | -40 -32 -26 -19 -13 -22 -39 -75 | 6.7 6.0 3.1 1.2 0.6 0.5 0.5 0.2 |
| Artists/Danny Taurus/MoDrama/Downer07.wav | 8.0 | -37 -31 -25 -17 -16 -20 -34 -67 | 6.4 5.1 3.0 1.3 0.7 0.5 0.5 0.1 |
| Artists/Danny Taurus/MoDrama/Downer09.wav | 8.0 | -36 -24 -18 -17 -16 -21 -31 -95 | 7.1 7.1 6.0 4.7 3.0 1.6 0.7 0.0 |
| Artists/Danny Taurus/MoDrama/Downer11.wav | 8.0 | -35 -33 -21 -19 -17 -36 -50 -94 | 5.5 4.1 2.4 0.9 0.8 0.4 0.6 0.1 |
| Artists/Danny Taurus/MoDrama/Downer18.wav | 8.0 | -26 -26 -27 -26 -30 -34 -46 -55 | 1.4 1.2 1.2 0.9 0.9 0.9 0.6 0.3 |
| Artists/Danny Taurus/MoDrama/Downer19.wav | 8.0 | -22 -23 -25 -26 -28 -36 -49 -75 | 4.8 4.2 3.7 3.1 2.5 1.9 1.0 0.2 |
| Artists/James Closs/Kit1/13 NOISE L.wav | 16.0 | -11 -11 -11 -11 -11 -11 -11 -11 | 1.1 1.5 1.0 1.5 1.0 1.5 1.1 1.4 |
| Artists/James Closs/Kit1/14 NOISE M.wav | 16.0 | -18 -18 -18 -19 -18 -18 -18 -18 | 0.6 0.5 0.5 0.5 0.6 0.5 0.5 0.5 |
| Artists/James Closs/Kit1/15 NOISE H.wav | 10.0 | -19 -20 -20 -19 -19 -19 -20 -19 | 4.7 4.7 4.8 4.8 4.7 4.7 4.7 4.7 |
| Artists/Eric Mooney/Kit1/Stinger, Guitar Bowed, Visceral, Key of A.wav | 9.1 | -17 -19 -25 -44 -51 -61 -72 -88 | 1.7 2.4 2.0 2.0 2.3 2.9 3.5 3.8 |
| Artists/Eric Mooney/Kit1/Braams, Guitar Bowed, Dark, Foreboding, Key of C.wav | 11.1 | -20 -15 -24 -36 -45 -55 -66 -81 | 0.9 0.7 0.6 0.7 0.6 0.6 0.6 0.6 |
| Artists/Eric Mooney/Kit1/Transition, Modulation, Movement, Key of D.wav | 3.6 | -11 -11 -13 -12 -12 -12 -12 -12 | 0.1 0.2 0.2 0.3 0.2 0.2 0.3 0.2 |
| Artists/JS-Clipping/CinderBlock-Drop.wav | 1.5 | -20 -31 -34 -44 -64 -70 -73 -74 | 2.7 5.4 5.3 5.0 2.9 2.9 2.3 2.9 |
| Artists/JS-Clipping/Gunshot.wav | 1.9 | -8 -13 -27 -34 -41 -54 -76 -82 | 1.7 1.2 1.0 1.6 1.2 1.5 3.9 5.0 |
| Artists/JS-Clipping/WaterJug-Hit.wav | 2.0 | -8 -13 -17 -24 -33 -42 -51 -60 | 0.6 0.6 0.6 0.6 0.6 0.7 1.1 1.3 |
| Artists/JS-Clipping/TrashCan-KickDownStairs.wav | 5.0 | -10 -17 -14 -23 -29 -29 -41 -47 | 1.0 1.0 0.9 0.6 0.6 0.6 0.4 0.5 |
| Artists/Phil Elverum/Gong Clearmoon.wav | 16.6 | -23 -28 -33 -36 -40 -43 -45 -55 | 1.8 1.2 0.8 0.6 0.6 0.5 0.5 0.8 |
| Artists/Chaz/CB5-Cassette808_BD03.wav | 4.3 | -11 -13 -15 -19 -26 -33 -41 -56 | 0.1 0.1 0.1 0.1 0.1 0.1 0.1 0.1 |
| Artists/Totems/808 Bass C Deep-O.wav | 4.7 | -2 -2 -4 -9 -16 -26 -42 -65 | 0.1 0.1 0.1 0.1 0.1 0.1 0.1 0.1 |
| DRUMS/Kick/808 Kick sustained.wav | 3.0 | -9 -16 -22 -28 -34 -40 -46 -53 | 0.1 0.1 0.1 0.1 0.1 0.1 0.1 0.1 |
| DRUMS/Kick/Deep Sky Kick descending.wav | 4.1 | -3 -4 -4 -4 -5 -5 -6 -8 | 0.2 0.1 0.1 0.1 0.1 0.1 0.0 0.0 |
| Artists/Leonard Ludvigsen/Phat Beatbox/Kick/Bbox big up.wav | 1.8 | -8 -15 -26 -36 -46 -56 -67 -77 | 0.2 0.2 0.2 0.2 0.2 0.2 0.5 2.1 |
| DRUMS/Crash/909 Crash.wav | 1.1 | -13 -14 -18 -21 -24 -28 -32 -41 | 5.9 6.8 6.7 6.8 6.6 6.5 6.6 6.6 |
| Artists/Roman Hatz/Kit 1/Hihat Open Long MetalSwish.wav | 3.6 | -14 -17 -21 -25 -31 -37 -44 -53 | 5.2 5.2 5.1 5.2 5.0 4.9 5.0 4.7 |
| Artists/Leonard Ludvigsen/Phat Beatbox/1BurpStretch Pitch-envelope2.wav | 5.8 | -17 -18 -18 -22 -23 -29 -34 -37 | 2.0 2.1 2.4 2.6 2.2 2.3 1.8 1.8 |

- **Riser:**
  - `Danny Taurus/MoDrama/Upper01, 02, 03, 05, 08, 14` rise in both loudness and brightness. `Upper11` and `Upper20` are weaker.
  - Every `Upper*` file is 10.0 s. The peak is at 6.0–7.25 s, sound ends at about 8.4 s, and the rest is silence. A Riser that must "end exactly on the drop" ([#13](https://github.com/gismya/deluge-generative/issues/13)) has to be timed to the peak, not the file end.
  - `Andrew Stirton/Frugal/frugalriser.wav` (13.7 s) rises and runs to the end of the file.
- **Fall:** `Danny Taurus/MoDrama/Downer01, 04, 05, 07, 09, 11, 19` get darker and quieter over 8.0 s, with sound ending at 6.3–7.1 s. `Downer18` is flat. `Frugal/befall.wav` fades but doesn't darken.
- **Impact:**
  - Hits and booms: `JS-Clipping/Gunshot`, `WaterJug-Hit` and `CinderBlock-Drop` (1.5–2 s hits). `Chaz/CB5-Cassette808_BD03` (4.3 s boom), `Totems/808 Bass C Deep-O` (4.7 s, pitched C). `SAMPLES/DRUMS/Kick/Deep Sky Kick descending` (4.1 s, near-constant level) and `808 Kick sustained` (3 s).
  - Cinematic: `Eric Mooney/Kit1/Braams…` (11 s) and `Stinger…` (9 s).
  - For Ambient: `Phil Elverum/Gong Clearmoon` (16.6 s).
- **Noise beds for a swept riser:** `James Closs/Kit1/13 NOISE L`, `14 NOISE M` and `15 NOISE H` are steady noise for 10–16 s. Under a Thin (HPF up) gesture any of them makes a plain riser.

All of these are outside `SAMPLES/FX/<lane>`, so the default lookup won't find them. A palette lane entry can point at the folder with tags (`Upper`, `Downer`, `frugalriser`) ([#25](https://github.com/gismya/deluge-generative/issues/25) allows any folder).

### 5.2 Synth presets as fallbacks

No factory preset is a true riser: none has a long envelope driving pitch or cutoff upwards over bars. Candidates that a sound-control gesture could finish:

| Lane | Preset | Why |
|---|---|---|
| Riser | 129 Sci-fi Scenic | 3 s amplitude swell, noise 30, LPF nearly closed (2/50), so Brightness and Thin have room to sweep |
| Riser | 128 Lunar Landing | env2 opens the filter and speeds an LFO over 3 s. A semitone flat, which matters less for a riser |
| Riser | 116 Evolving Frequencies | noise on an envelope, 5.8 s release |
| Fall | 103 Sci-fi Chaos | env1 → osc A pitch +82%, with a 19 s decay: the pitch slides down as the note decays |
| Impact | 112 Hard Tech Beat | noise 50, env1 → pitch +50% and → LPF +74%: a pitch-drop hit. Needs the arp off |
| Impact | 081 Xylophone Big Bass | FM boom with 8-voice unison, carrier feedback on an envelope, 13.6 s release |
| Impact | 014 Trap Bass 2 | 808-style boom, saturated, 3 s release |

These are stand-ins. The pack should bring proper riser, fall and impact presets as well as samples ([#42](https://github.com/gismya/deluge-generative/issues/42)).

## 6. Draft palettes

Rosters are assumed from each genre brief (roster design is genre slot 5, not decided here). Weights: 3 = main pick, 2 = common, 1 = occasional variation. Preset entries are `SYNTHS/<NNN Name>.XML`. Drum lanes give the folders (relative to `SAMPLES/`) and filename tags with weights. "Pack" marks a gap for [#42](https://github.com/gismya/deluge-generative/issues/42).

### 6.1 Techno

*Brief: 125–135 BPM, minor or phrygian, straight. Four-on-the-floor kick, offbeat hats, a mutating acid line, sparse harmony (drone or one chord), long builds, a kick drop.*

| Part (role, flavour) | Entries | Why |
|---|---|---|
| Bass (Bass, Loop) | 047 Basic Dirty Bass 3 · 000 Rich Saw Bass 2 · 007 Detuned Saw Bass 1 · 008 FM Rich Distorted Bass 1 | Plain, dark and short, leaving room for the acid. 008 adds grit for harder takes |
| Acid (Lead, Loop, legato + slide) | 165 Acid Arp 3 *(arp off)* · 015 Resonant Filter Bass 2 · 097 Saturated Filter 2 · 005 Sweet Mono Bass 1 | 165 is mono, −36 st, resonance 33. 015 has resonance 42. 097 has a saturated resonant LPF. None has the 303's accent behaviour. **Pack: a real 303-style preset** (saw/square, 24 dB resonant LPF, strong env2 → cutoff, accent via velocity → cutoff) |
| Drone (Pad, Sustain pedal) | 148 Warm 5th Pad 2 · 126 Dark FM Pad 2 · 139 Detuned Saw Pad 2 · 149 Cold 5th Pad 1 | Dark and slow. The built-in fifth suits a root + fifth drone |
| Stab (Chords, Stab) | 086 Techno Organ 3 · 037 Echo Chord 2 · 026 PW Organ 1 · 083 Dark Chorus 1 | One-chord stabs. 037's fifth + delay is the classic dub-techno chord, and with one chord the stacked fifth is no problem |

| Lane | Folders | Tags | Why |
|---|---|---|---|
| Kick | DRUMS/Kick | 909 Kick 3 · Deep Sky Kick HQ 1 · Deep Sky Kick distorted 1 · 808 Kick distorted 1 | 909 is the techno kick. The others give deeper or harder takes. Avoid `Deep Sky Kick descending` (a 4 s boom) |
| Clap | DRUMS/Clap | 909 3 · 808 1 | Only one 909 clap. The 808 clap's 2 s tail suits breaks |
| Snare | DRUMS/Snare | 909 3 · XV5080 Snare dry 1 | For build rolls |
| Rim | DRUMS/Rim | 909 2 · 808 1 | Short ticks |
| Closed hat | DRUMS/HatC | 909 3 · 808 1 · DDD1 1 | Offbeat 909 hats |
| Open hat | DRUMS/HatO | 909 3 · 808 1 | Same machine as the closed hat, so they choke as a pair |
| Ride | DRUMS/Ride | 909 3 · XV5080 Ride edge 1 | Only one 909 ride. **Pack: more techno rides** |
| Crash | DRUMS/Crash | 909 3 · XV5080 1 | Drop crash |
| Tom | DRUMS/TomL, TomM, TomH | 909 2 · 808 1 | Fills |
| Perc 1 | DRUMS/Claves, Cowbell, Other | 808 2 · Metal 1 · Click 1 | Metallic ticks |
| Perc 2 | DRUMS/Shaker, Tambourine | (any) | 16th shaker |
| Riser | Artists/Danny Taurus/MoDrama, Artists/Andrew Stirton/Frugal | Upper 3 · frugalriser 1 | Measured as rising. Synth fallback: 129 |
| Fall | Artists/Danny Taurus/MoDrama | Downer 3 | Measured as falling. Synth fallback: 103 |
| Impact | DRUMS/Kick, Artists/Chaz | Deep Sky Kick descending 2 · Cassette808_BD03 1 | Boom on the drop. Synth fallback: 112 *(arp off)* |

### 6.2 House

*Brief: 120–126 BPM, moderate 16th swing, minor, dorian or major. Claps on 2 and 4, an offbeat bass that answers the kick, chord stabs with 7th/9th colour, a chord graph that matters, 2–4 bar harmonic rhythm.*

| Part (role, flavour) | Entries | Why |
|---|---|---|
| Bass (Bass, Loop) | 080 House 3 · 002 Basic Square Bass 2 · 050 FM Basic Bass 2 · 000 Rich Saw Bass 1 · 012 Blunt Sync Bass 1 | 080 is the card's house organ bass (sine + a square 19 st up: a strong 3rd harmonic, in key). The others are round, short offbeat basses |
| Chords (Chords, Stab) | 078 House 3 · 071 Rhodes 2 · 074 Electric Piano 2 · 073 Piano 2 · 026 PW Organ 1 | 078 is the classic FM house organ stab (sustain 0). EPs and piano carry 7th/9th voicings, and their extra carriers are quiet harmonics. **Gap:** only about 5 clean stab sounds. **Pack: a plain saw/square chord stab** with no built-in interval and no portamento |
| Pad (Pad, Sustain) | 131 Warm Strings 3 · 143 Chillout Pad 2 · 139 Detuned Saw Pad 1 · 136 Synthwave Pad 1 | Strings under the stabs. 136 has portamento 2 saved, so it needs portamento cleared on load |
| Lead (Lead, Motif or Arp) | 067 Marimba 2 · 163 Crisp Pop Arp 2 *(arp off)* · 068 FM Bells 1 1 · 021 80s TV Lead 1 | Short answering motifs |

| Lane | Folders | Tags | Why |
|---|---|---|---|
| Kick | DRUMS/Kick | 909 Kick 3 · 808 Kick 1 · Linndrum 1 | 909 for house. `808 Kick` also matches the descending, distorted and sustained variants. Weight them down if tags are substrings |
| Clap | DRUMS/Clap | 909 3 · 808 2 · Linndrum 1 | Claps on 2 and 4 |
| Closed hat | DRUMS/HatC | 909 3 · 808 2 · Linndrum 1 | Swung 16ths |
| Open hat | DRUMS/HatO | 909 3 · 808 1 | Offbeat open hat |
| Ride | DRUMS/Ride | 909 3 · Linndrum 1 | |
| Rim | DRUMS/Rim | 808 2 · 909 2 | Ghost rims |
| Crash | DRUMS/Crash | 909 2 · 808 1 | |
| Tom | DRUMS/TomL, TomM, TomH | 808 2 · Linndrum 1 | |
| Perc 1 | DRUMS/Shaker, Tambourine | Cabasa 2 · Maraca 1 · Tambourine 1 | Shaker and tambourine layer |
| Perc 2 | DRUMS/ConL, ConM, ConH, Cowbell | 808 2 · Linndrum 1 | Congas and cowbell |
| Riser / Fall / Impact | as Techno | Upper / Downer / Crash `909` 2 | |

### 6.3 Dub

*Brief: roots dub, 70–80 BPM, minor, light swing. One-drop drums, offbeat skank, a heavy bass that carries the tune, delay/reverb throws and drop-outs as the main motion.*

| Part (role, flavour) | Entries | Why |
|---|---|---|
| Bass (Bass, Loop) | 004 Dubby Bass 3 · 002 Basic Square Bass 2 · 169 Double Bass 1 · 006 Vaporwave Bass 1 | 004 is round, with env2 on osc A and light chorus: named for the job. 169 is a sampled upright. 006 has portamento already saved. **Gap:** no deep sine sub with a soft attack. **Pack: a dub sub bass** |
| Skank (Chords, Stab) | 077 FM Perc-Organ 3 · 082 Short Sharp Delay 2 · 026 PW Organ 2 · 073 Piano 1 · 076 Organ 1 | Percussive organ is the skank sound. 082 is a short stab feeding its own delay, which suits throws. 077 has portamento 7 saved, so it needs portamento cleared |
| Melodica (Lead, Motif) | 064 Reeds-Flute-Oboe 3 · 062 Trumpet 2 · 061 Blown-Staccato-Panpipes 1 | 064 is the nearest to a melodica. Trumpet covers dub horns. **Pack: a melodica preset** |

| Lane | Folders | Tags | Why |
|---|---|---|---|
| Kick | DRUMS/Kick | 808 Kick 2 · CR78 1 · Linndrum 1 · R100 1 | Round, short one-drop kick |
| Rim | DRUMS/Rim | 808 2 · CR78 2 · Linndrum 1 · XV5080 1 | Cross-stick backbeat |
| Snare | DRUMS/Snare | wet 2 · XV5080 Snare dry 1 | `DDD1/HR16B Snare wet` already carry room |
| Closed hat | DRUMS/HatC | CR78 2 · 808 2 · Linndrum 1 | |
| Open hat | DRUMS/HatO | CR78 2 · 808 1 | |
| Tom | DRUMS/TomL, TomM, TomH | SDS5 3 · 808 1 | Simmons toms are the nearest to a syndrum |
| Perc 1 | DRUMS/Shaker, Claves | Cabasa 2 · Claves 1 | |
| Perc 2 | DRUMS/Block, ConL, ConM, ConH | Bongo 2 · Conga 1 | RX-5 bongos live in `Block` |
| Impact | Artists/JS-Clipping | Gunshot 2 · WaterJug-Hit 1 | The dub gunshot |
| Fall | Artists/Danny Taurus/MoDrama | Downer 2 | For drop-outs |
| Riser | as Techno | Upper 1 | Dub barely builds |

**Gap: the dub siren.** It is the genre's signature FX and has no lane (Riser, Fall and Impact only). The nearest preset, 111 Satellite Drum (LFO on both oscillator pitches), has a saved arp. Whether the siren is an FX lane kind or a Lead is a roster question. **Pack: a siren preset.**

### 6.4 Psytrance

*Brief: 138–146 BPM, minor, phrygian or harmonic minor, straight. Kick plus a rolling 16th bass that skips the kick step, a pedal-tone root with almost no chord changes, filter-swept leads and arps, 16-bar phrases, long builds into hard drops.*

| Part (role, flavour) | Entries | Why |
|---|---|---|
| Bass (Bass, Loop) | 047 Basic Dirty Bass 3 · 050 FM Basic Bass 2 · 048 Thin Pulse Bass 2 · 008 FM Rich Distorted Bass 1 · 095 Harsh FM Feedback 1 | Short, tight and low. FM basses are typical of psy. All of these hold sustain, so the Generator's short gates do the work. **Pack: a psy bass** with sustain 0 and a fast decay, so the roll stays tight at any gate |
| Lead (Lead, Loop) | 087 Define Leader 3 · 057 FM Lead 2 · 098 Saturated Sync 2 · 045 Square Sync 1 · 059 Distorted Lead Guitar 1 | Mono, saturated, sync and FM leads for filter sweeps. 087 and 098 have saturation saved |
| Arp (Lead, Arp) | 162 Busy Arp 2 *(arp off)* · 165 Acid Arp 2 *(arp off)* · 167 FM Metallic Bass Arp 1 *(arp off)* · 166 Harpsichord Cyborg 1 *(arp off)* | Arp-shaped sounds once the saved arp is off |
| Drone (Pad, Sustain pedal) | 126 Dark FM Pad 2 · 149 Cold 5th Pad 2 · 120 High Harsh Pad 1 | Pedal tone |

| Lane | Folders | Tags | Why |
|---|---|---|---|
| Kick | DRUMS/Kick, Artists/Dean Daughters/Psy | KICK- 3 · Deep Sky Kick HQ 1 · XV5080 Kick 1 | The Psy set's kicks. `SAMPLES/DRUMS` has no real psy kick |
| Clap | DRUMS/Clap, Artists/Dean Daughters/Psy | CLAP- 2 · 909 1 | |
| Snare | DRUMS/Snare, Artists/Dean Daughters/Psy | SNARE- 2 · 909 1 | Build rolls |
| Closed hat | DRUMS/HatC, Artists/Dean Daughters/Psy | CLHAT- 3 · 909 1 | |
| Open hat | DRUMS/HatO, Artists/Dean Daughters/Psy | OPHAT- 3 · 909 1 | |
| Ride | DRUMS/Ride, Artists/Dean Daughters/Psy | RIDE- 2 · 909 1 | |
| Crash | DRUMS/Crash | 909 2 · XV5080 1 | |
| Perc 1 | Artists/Dean Daughters/Psy, DRUMS/Other | PERC- 3 · Click 1 | |
| Perc 2 | Artists/Dean Daughters/Psy, DRUMS/Shaker | SHAKER- 2 · Cabasa 1 | |
| Riser | Artists/Danny Taurus/MoDrama, Artists/Andrew Stirton/Frugal | Upper 3 · frugalriser 2 | Long builds. Synth fallbacks: 129, 116 |
| Fall | Artists/Danny Taurus/MoDrama | Downer 3 | Synth fallback: 103 |
| Impact | DRUMS/Kick, Artists/JS-Clipping, Artists/Eric Mooney/Kit1 | Deep Sky Kick descending 2 · CinderBlock 1 · Braams 1 | Hard drops |

All Psy-set entries depend on tags acting as filters (§4.2). A psy kit in the pack's own `SAMPLES/DRUMS`-style folders would avoid that.

### 6.5 Ambient

*Brief: 60–90 BPM, major, lydian or dorian. No drums, or soft sparse percussion that enters only at high Energy. Pads and drones with 8+ bar harmonic rhythm. The Arc swells and recedes and never drops.*

| Part (role, flavour) | Entries | Why |
|---|---|---|
| Pad (Pad, Sustain) | 131 Warm Strings 2 · 118 Small Bridge Pad 2 · 140 Slow Aural Swells 2 · 151 Radiant FM Pad 2 · 135 Soothing Growth Pad 1 · 157 Rich FM Pad 4 1 · 145 Eerie High Pad 1 · 147 Resonant Filter Pad 1 | Slow attacks and long releases are wanted here. 135 already has reverb 35, so Space has less room. Leave out 127, 155 and 156 (they add a major 3rd on every note) |
| Drone (Pad, Sustain pedal) | 125 Evolving Pad 3 · 148 Warm 5th Pad 2 · 056 FM Bell Modulation 1 | 125 is a mono −24 st ringmod drone. 056 is an FM bell drone with a 10 s release |
| Lead (Lead, Motif) | 070 Glockenspiel 2 · 068 FM Bells 1 2 · 106 Hang Drum 2 · 168 Hang Drum 2 · 069 FM Bells 2 1 · 054 Ghostly Sines 1 · 121 Tiny Lights 1 *(arp off)* | Bells and struck sounds. Their long tails are welcome here |
| Bass (Bass, Loop, slow) | 169 Double Bass 2 · 002 Basic Square Bass 1 | Weak fit. **Pack: a soft sine sub** with a slow attack |

| Lane (all behind Density gates) | Folders | Tags | Why |
|---|---|---|---|
| Kick | DRUMS/Kick | CR78 2 · KR55 1 | Soft, short |
| Rim | DRUMS/Rim | CR78 2 · KR55 1 | |
| Closed hat | DRUMS/HatC | CR78 2 · KR55 1 | |
| Perc 1 | DRUMS/Other, DRUMS/Claves, DRUMS/Shaker | Chime 2 · Triangle 2 · Claves 1 · Cabasa 1 | The only soft tonal hits. Chime and Triangle have one file each. **Pack: soft mallet, brush and bell one-shots** |
| Perc 2 | DRUMS/Block, DRUMS/ConL, DRUMS/ConM, DRUMS/ConH | Woodblock 1 · Conga 1 | |
| Riser | Artists/Danny Taurus/MoDrama, Artists/Andrew Stirton/Frugal | frugalriser 2 · Upper 1 | Swells. Synth fallback: 129 |
| Fall | Artists/Danny Taurus/MoDrama | Downer 2 | Recedes |
| Impact | Artists/Phil Elverum | Gong Clearmoon 1 | Only if the Arc uses Impact at all. Ambient never drops |

`SAMPLES/Artists` also holds long drones: `Eric Mooney/Kit1/Drone…` (95 s), `James Closs/Kit1/16 DRONE` (28 s), `John Atkinson/Wave Drone` (51 s) and `Phil Elverum/HighDrone…` (42 s). No role can use a sample drone today, since Pad loads synth presets only.

## 7. Gaps for the Generative pack (#42)

1. **FX samples in `SAMPLES/FX/<lane>`:** risers, falls and impacts, trimmed so the landing point is the file end. The factory set needs palette paths into `SAMPLES/Artists` and has 1.5–2 s of trailing silence (§5.1).
2. **FX synth presets:** a riser (long env → cutoff and pitch up, noise), a fall (pitch down) and an impact. No factory preset is built for this.
3. **Acid:** a 303-style preset for Techno (and Psytrance arps).
4. **Chord stab:** a plain house stab with no built-in interval and no saved portamento. Chords is the thinnest role.
5. **Dub:** a sub bass, a melodica, and a siren (plus a decision on where the siren lives).
6. **Psy:** a tight bass (sustain 0), and a psy drum set under folder names the lane table finds, or rely on tags into `Artists/Dean Daughters/Psy`.
7. **Ambient:** soft one-shots (mallets, brushes, bells) and a soft sine sub.
8. **Techno:** more 909-style rides and claps (one each today).

## 8. Open

- **Arp off on load.** Should the Generator clear the clip's arp mode on load? Recommended yes. Without it, 15 presets (including the best acid sound) are unusable. This extends #13's voice-mode amendment.
- **Portamento off on load for non-sliding parts.** Recommended yes. 26 presets have portamento saved, and in POLY mode it makes every chord tone glide.
- **Tags as filters.** Palette lane entries that point at mixed folders need tags to restrict which files are eligible. The palette format should say so.
- **Tag matching.** Are tags substring matches on the filename? `808 Kick` matches four kicks, three of which are not plain kicks.
- **Riser timing.** A sample riser's landing point isn't the file end in the factory files. Should the Generator scan for the peak or audible end, or should the pack trim files and the Generator use the end?
- **Hat pairing.** Closed and open hats are rolled separately and may come from different machines. Should they share a machine tag?
- **Voice budget.** Pads with 4–8 unison voices playing 4-note chords. Not checked against the firmware's voice limit.
- **Dub siren.** FX lane or Lead part?
- **Listening.** Every pick here comes from reading files. Check them by ear in delugemu.
