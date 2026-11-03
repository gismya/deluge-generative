# Sound controls per synth engine

Research for [#26](https://github.com/gismya/deluge-generative/issues/26), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1). It feeds [Sound generation (#13)](https://github.com/gismya/deluge-generative/issues/13) and blocks [#14](https://github.com/gismya/deluge-generative/issues/14).

**Question.** Which synth params should the Generator move live, and how does each kind of change (brighter, shorter, dirtier, wetter, more movement) map onto each engine in v1.3.0-rc3? Is the working set Brightness, Decay, Drive, Space, Wobble right?

**Settled context (from #13, not reopened here).** Synth sounds are curated card presets. The Generator never changes oscillator type, synth mode, tuning, voice mode or mod routing at runtime. Live change goes only through a few abstract sound controls, each moving concrete params within a bounded range around the preset's stored values.

**Sources.** Firmware source on the `generative` branch (commit `190bdd9a`, upstream `v1.3.0-rc3` plus docs). Paths are relative to `src/deluge/` unless they start with `website/` or `src/`. Bundled docs in `website/src/content/docs/`. The factory SD card at `~/source/delugemu-sd/sdcard` (171 `SYNTHS/*.XML`, 43 `KITS/*.XML`), surveyed with a throwaway Python script that is not in the repo. Values marked **(est.)** are my own derivations, not stated in a source.

---

## 1. Recommendation

Keep the five controls and add one more, **Thin**. Each control is a list of *targets* per engine. A target only takes part if the loaded preset actually uses it (see the "active if" column). If no target is active, the control does nothing for that sound. That is expected: not every preset can wobble.

Offsets are in **menu units** (the 0–50 scale on the OLED; 1 unit = 85,899,345 in the raw int32, see §2.3). They are added to the preset's stored value and clamped to the param's range. The ranges are starting points **(est.)** for tuning by ear, not measured.

| Control | Subtractive (incl. wavetable, sample osc) | FM (2 carriers + 2 modulators, all sine) | Ringmod | DX7 osc | Kit row (sample) | Kit level (affect-entire) |
|---|---|---|---|---|---|---|
| **Brightness** | `lpfFrequency` ±8; `oscAWavetablePosition`/`oscB…` ±5 if wavetable | `modulator1Volume` ±6, `modulator2Volume` ±6 (only modulators that are in use) | `lpfFrequency` ±8; `oscAPhaseWidth`/`oscBPhaseWidth` ±6 if set | `lpfFrequency` −10..0, only if the preset has an LPF on; otherwise none | `lpfFrequency` −15..0 | kit `lpfFrequency` −20..0 (single filter for the whole kit) |
| **Decay** | `env1Release` ±6; `env1Decay` ±6 if `env1Sustain` < 50; `env2Decay` ±6 if env2 is cabled to something | same, and env2 here is usually the FM index envelope, so it also shortens the "pluck" | same as subtractive | cut only: `env1Decay` −10..0 with `env1Sustain` lowered, `env1Release` −10..0 | cut only: lower `env1Sustain`, `env1Decay` −10..0 (one-shot samples otherwise play to the end) | — |
| **Drive** | `lpfMorph` (= ladder drive) 0..+15 if LPF is a ladder mode; `waveFold` 0..+10 if osc is sine/triangle | `modulator1Feedback`, `carrier1Feedback` 0..+8; `waveFold` 0..+10 | `waveFold` 0..+10; `lpfMorph` 0..+15 (ladder) | `waveFold` 0..+10 | `bitCrush`/`sampleRateReduction` 0..+10 (genre opt-in) | kit `bitCrush`/`sampleRateReduction` |
| **Space** | `reverbAmount` 0..+10; `delayFeedback` ±4 only if preset delay > 0 | same | same | same | `reverbAmount` 0..+10 | kit `reverbAmount`, kit `delayFeedback` (if > 0) |
| **Wobble** | `modFXDepth` ±8 if modFX type ≠ none; `lfo1Rate`/`lfo2Rate` ±4 if an LFO is cabled; LFO cable amounts ×0..×2 *(needs a #13 decision, §6)* | same | same | `modFXDepth` (if any); `oscAPhaseWidth` (= DX amp-mod, only if the patch has AMS > 0) | `modFXDepth` (rows have none in factory kits) | kit `modFXDepth` ±8, kit `modFXRate` ±4 |
| **Thin** (new) | `hpfFrequency` 0..+20 | none: factory FM presets have no HPF (§3.2) | `hpfFrequency` 0..+20 | `hpfFrequency` 0..+20 only if HPF is on | `hpfFrequency` 0..+20 | kit `hpfFrequency` 0..+25 |

Rationale in one line each:

- **Brightness** is the control whose param changes most by engine. In FM it is modulation index, in subtractive and ringmod it is cutoff, in DX7 there is almost nothing to move.
- **Decay** works on release more than decay: 55% of subtractive presets hold `env1Sustain` at 50, where `env1Decay` does nothing.
- **Drive** has no single "drive" param. Saturation (`clippingAmount`) is a plain setting, not a param, so Compose can't automate it. The automatable options are ladder drive (`lpfMorph`), wavefold and FM feedback.
- **Space** is the per-sound reverb send. Delay only moves if the preset already uses it, because switching delay on allocates a buffer.
- **Wobble** only has something to move on 66% of synth presets (those with an LFO cable or a mod FX), §5.
- **Thin** (HPF up) is the classic build-up gesture and isn't covered by Brightness, which should mean "more top end", not "less bottom end".

## 2. How params work (the mechanics behind the table)

### 2.1 Kinds

`modulation/params/param.h:41-58` defines the param kinds the Generator can touch:

- **Patched** (`Kind::PATCHED`): the mod-matrix params of a `Sound`. Split into **local** (per voice, `enum Local`, `param.h:68-128`) and **global** (once per sound, `enum Global`, `param.h:131-156`). They can be modulated by patch cables. Volume, filter, envelope, FM operator, pulse width, wave index, fold, pan and pitch are local. Reverb send, delay, mod FX depth/rate, global LFO rates and arp rate are global.
- **Unpatched sound** (`enum UnpatchedShared` + `UnpatchedSound`, `param.h:177-215`): bass/treble, bitcrush, sample-rate reduction, mod FX offset/feedback, sidechain shape, compressor threshold, stutter, the arp/randomizer params and portamento. No cables.
- **Unpatched global** (`enum UnpatchedGlobal`, `param.h:218-237`): the params of a kit's affect-entire level and of audio clips. These include a kit-wide LPF/HPF, reverb send, delay, mod FX, volume, pan and pitch.
- **Patch cable** (`Kind::PATCH_CABLE`): the amount of each existing cable is itself an `AutoParam` (`modulation/patch/patch_cable.h:72`), so it can be automated.

Every patched, unpatched and cable value lives in an `AutoParam`, so **every param in the table can be written as automation by Compose**. Synth clips hold one param manager. Each kit row has its own on its `NoteRow` (`model/note/note_row.h:143`), and the kit has one for its affect-entire level.

**Not params (can't be automated, no smoothing):** synth mode, osc type, filter modes, mod FX type, `clippingAmount` (saturation, 0–15, `gui/menu_item/fx/clipping.h:33-56`, stored as a plain `uint8_t` on `ModControllableAudio`), unison voice count and detune (`processing/sound/sound.h:127-128`), transpose, sample start/end, repeat mode, arp mode, and every DX7 operator setting (§3.4).

### 2.2 How a stored value becomes a sound

`Patcher` treats the stored value as one more "cable" and combines it with the real cables (`modulation/patch/patcher.cpp:268-270`). The result is mapped by the param's group (`param.h:30-37`):

- **Volume** (osc A/B, noise, modulator volumes, master volume, reverb send, mod FX depth): cables multiply, the sum is squared.
- **Linear** (resonance, morph, sustain, feedbacks, delay feedback): cables multiply.
- **Hybrid** (pulse width, wave index, pan): cables add.
- **Exp** (LPF/HPF freq, pitches, LFO rates, env times, delay rate, mod FX rate, arp rate): cables add, then the result is exponentiated.

So a relative offset on the stored value moves the centre that the preset's envelopes, velocity and LFOs then act on. A filter envelope keeps working around the new cutoff, and an FM index envelope keeps working around the new modulator level.

### 2.3 Value scale and what an offset means

- Stored values are int32. The menu shows 0–50 linearly: `value = user × 85,899,345 − 2^31` (`util/functions.cpp:1359-1388`). Pulse width uses half that step.
- For **exp** params, the stored value is first scaled by `getParamRange` (`util/functions.cpp:57-84`). It defaults to 2^30, is 1.4 × 2^29 for LPF freq, 2^29 for pitch and delay rate, and 1.5 × 2^29 for attack. Then `getExp` doubles the value for every 2^26 (`util/functions.cpp:503-512`). That gives about **0.32 octave per menu unit** at the default range, and about **0.22 octave per unit for LPF cutoff** (est.). LPF ±8 units is therefore roughly ±1.8 octaves (est.).
- **Decay/release** go through `lookupReleaseRate` (`util/functions.cpp:1340`, table `util/lookuptables/lookuptables.cpp:297-306`). One menu unit is about 1.28 table steps, each changing the rate by 6–14% depending on position. So ±5 units changes the time by roughly ×1.4 to ×2 (est.).

### 2.4 Zipper noise and smoothing

- **One smoothed param per sound.** A UI-style change goes through `Sound::notifyValueChangeViaLPF` (`processing/sound/sound.cpp:2267-2318`). That function smooths the param with a one-pole filter (`doParamLPF`, `sound.cpp:2320-2338`, about 2 render windows ≈ 6 ms (est.)). But `Sound` has a **single** `paramLPF` slot (`sound.h:85`). Changing a second param snaps the first one to its target (`stopParamLPF`). So a control that moves three params in one go only smooths the last one.
- **Which params smooth** (`params::paramNeedsLPF`, `modulation/params/param.cpp:384-416`). Master/osc volumes, reverb send, pan, LPF/HPF freq and wave index smooth only for user input, not for automation playback. Modulator volumes and feedbacks, mod FX depth and delay feedback always smooth. Nothing else smooths: env times, resonance, morph, fold, pulse width, LFO rates and every unpatched param step.
- **Within a voice**, osc, modulator and overall amplitudes and wave index are ramped linearly across each render window (`model/voice/voice.cpp:1017-1100`). Filter coefficients are set once per window (`voice.cpp:997-1004`), so cutoff steps every ≤128 samples (2.9 ms).
- **Automation playback** interpolates between nodes per render window when the node is marked `interpolated` (`modulation/automation/auto_param.h:60, 105-110`). That smooths every param at once, which the single `paramLPF` slot can't do.
- **Practical rule (est.):** move controls in small steps per tick, or as interpolated automation ramps. Step changes are safe for env times, sustain, LFO rates and send levels. Big instant jumps in fold, morph, resonance, bitcrush or FM feedback will be audible as steps. Steps in `clippingAmount` change gain and are clearly audible.
- **Recording trap.** `AutoParam::setCurrentValueInResponseToUserInput` writes automation if playback is recording and the clip is armed (`modulation/automation/auto_param.cpp:101-160`). If a param already has automation, a live write is overridden at the next node. A Perform write path must account for both.

### 2.5 CPU and memory side effects

- **Filters can bypass themselves.** A sound skips the LPF if the mode isn't 24dB-drive, nothing is cabled to cutoff, cutoff ≥ `0x7FFFFFD2` (menu 50) and morph is at minimum. The HPF is skipped if nothing is cabled to it and freq and morph are at minimum (`processing/sound/sound.cpp:2528-2540`). Filters cost CPU when they run: the DX7 doc says they "increase CPU usage and decrease polyphony" (`website/src/content/docs/features/dx_synth.mdx:35`).
  - **LPF: in practice already running.** Every non-FM sound gets a default Y → LPF cable. New sounds get it from `setupDefaultExpressionPatching` (`sound.cpp:275-296`), and pre-4.0 files get it on load (`sound.cpp:262-273, 3273`). Kit rows load through the same `Sound::readFromFile` (`processing/sound/sound_drum.cpp:144-149`). That cable keeps the LPF running even when it is fully open, so Brightness via LPF costs nothing extra on subtractive, ringmod, DX7 and kit-row sounds, unless a curator deleted the cable.
  - **HPF: switches on.** Raising HPF from 0 **turns on per-voice HPF processing**, since there is no default HPF cable. FM sounds get Y → modulator 1 instead of Y → LPF.
- **Wavefold** is skipped at 0 (`voice.cpp:1518, 1600`). Above 0 it costs a polynomial pass per voice buffer.
- **Delay** allocates its buffer when feedback first rises above the threshold, and frees it when it drops (`dsp/delay/delay.cpp:30-50, 93-124`). This is stock knob behaviour, but it is an allocation in the audio path. Only move delay feedback on presets where it is already > 0.
- **Mod FX type** is a setting, and changing it allocates or frees buffers (`Sound::setModFXType`, `sound.cpp:333`). Depth on a preset with no mod FX does nothing. So Wobble can't add a chorus to a dry preset.
- **Patched local params** are recalculated for every voice when their stored value changes (`patchedParamPresetValueChanged`). Global ones are recalculated once per sound. A few writes per tick are cheap (est., not measured).

## 3. Per engine

### 3.1 Subtractive (103 of 171 factory synths)

- Two oscillators (sine, triangle, square, analog square, saw, analog saw, wavetable, sample, DX7, input; `src/definitions_cxx.hpp:358-371`), noise, LPF → HPF (route selectable), wavefold before the filter, saturation after it (`voice.cpp:1515-1530`).
- **Brightness = LPF cutoff.** 88% of subtractive presets set it away from fully open (median 34 of 50), and the default filter-envelope cable is env2 → LPF (50 presets). Resonance is better left alone: it changes level and character, not just brightness.
- **Wavetable:** wave index (`oscAWavetablePosition`) is the timbre knob. It is smoothed for user input, and patch cables reach twice as far (`patcher.cpp:259-265`). No factory preset uses wavetables.
- **Pulse width** sounds a lot like "brightness" on square oscillators, but it is only set in 17% of presets and is disallowed in FM (`sound.cpp:1364-1369`). Leave it out of subtractive Brightness. Use it for ringmod (§3.3).
- **Osc mix** (`oscBVolume`, set in 82%) is preset-specific timbre. Moving it can cancel a sub or detune layer, so keep it out.
- **Drive:** `lpfMorph` is the "DRIVE" param on ladder LPFs (12dB, 24dB, 24dB-drive). On SVF modes it morphs LP → BP → HP instead (`website/src/content/docs/features/community_features.mdx:724-725`). All factory presets use ladder modes. Wavefold has no effect on squares (`community_features.mdx:764`).

### 3.2 FM (54 factory synths)

- In FM mode the oscillator type is ignored and hidden (`gui/menu_item/osc/type.h:115-118`). Two sine carriers and two sine modulators render with feedback (`voice.cpp:1440-1500`). Pulse width, noise and osc sync are disallowed (`sound.cpp:1364-1415`, `gui/menu_item/patched_param/integer_non_fm.h`).
- **Factory FM presets load with no filters.** Files older than community 1.2.0 have `lpfMode` and `hpfMode` forced to OFF on load (`sound.cpp:3261-3265`). Every official version sorts below every community version (`src/deluge/util/firmware_version.h.in:7-11, 40-45`). All 171 factory synths were saved by official firmware 2.x or have no version tag. So in factory FM presets **LPF/HPF params do nothing**, and Brightness and Thin need FM-specific targets.
- **Brightness = modulator volume (FM index).** 94% of FM presets use modulator 1 and 59% use modulator 2. env2 → modulator volume is the main envelope cable (34 and 31 presets), and `note` → modulator for key tracking (39 across all modes). Modulator volume is a volume-group param (squared), with neutral value 2^25 (`util/functions.cpp:86-170`). Raise only modulators that are already in use (stored > 0 or with a cable), since turning on a silent modulator adds a new tone.
- **Drive = feedback.** Modulator and carrier feedback (`modulator1Feedback`, `carrier1Feedback`) push towards noisy, sawtooth-like spectra. 26% and 17% of FM presets use them. They are always smoothed.
- **Default expression cable.** Loading a pre-4.0 preset adds Y → modulator 1 volume for FM and Y → LPF otherwise (`sound.cpp:262-296, 3273`), if there is room for a cable.

### 3.3 Ringmod (14 factory synths)

- The output is osc A × osc B. Osc volumes are ignored and can't take cables (`sound.cpp:1371-1374`, `voice.cpp:1017`). Pulse width is a big timbre control here: 50% of ringmod presets set osc A PW and 36% osc B PW.
- **Brightness = LPF** (86% set, and the LPF is a gold-knob param in all 14), plus pulse width.
- Osc pitch adjust reshapes the inharmonic spectrum but counts as tuning, so it is excluded.

### 3.4 DX7 oscillator (no factory presets)

- DX7 is an oscillator type inside subtractive mode (`dx_synth.mdx:33-35`). The operator levels, ratios, envelopes, algorithm and feedback live in a `DxPatch` byte array, not in params. The docs list "modulating/automating individual operators" as a missing feature (`dx_synth.mdx:129-132`).
- The only Deluge params that reach the DX engine: osc volume and pitch, pulse width (passed as `ampmod`, `voice.cpp:2421-2422`, which acts only on operators whose amp-mod sensitivity is non-zero, `dsp/dx/dx7note.cpp:364-374`), and the Deluge env1, which only cuts the DX output. Filters, fold, saturation and FX all apply after the DX engine.
- **So for DX7: Brightness can only darken (LPF), Decay can only shorten, and Drive, Space, Wobble and Thin work as for subtractive.** Editing `DxPatch` bytes directly (operator output level is the DX brightness knob) is possible in code, using the `dxPatchChanged` flag (`processing/source.h:48`, `voice.cpp:2424-2426`). But it is not a param, can't be automated and isn't smoothed. Leave it out.
- The factory card has no DX7 or wavetable presets. Curated presets for those engines would have to be made.

### 3.5 Kit rows (515 rows in 42 parsed kits)

- Every factory kit row is a subtractive `Sound` with a **sample** on osc 1 (osc 2 is sample or square, silent). Their envelopes are all the same: attack 0, decay 20, sustain 50, release ≈0. LPF is open, no reverb/delay sends (3% and 1%), no mod FX per row. Velocity → volume on 497 rows, random → pitch on 13, velocity → pitch on 17. Rows play once (`loopMode` 1 = `SampleRepeatMode::ONCE`, `src/definitions_cxx.hpp:493-498`).
- Consequences: **Brightness can only darken** a drum row. The row's LPF already runs because of the default Y → LPF cable (§2.5), but the cost is per row voice. **Decay can only shorten**: it has to lower sustain so env1 decay has an effect. Sample start is not a param.
- **Use the kit level for Drums-part gestures.** The affect-entire level has its own LPF, HPF, reverb, delay, mod FX, bitcrush and SRR as unpatched global params. It is one filter for the whole kit, not one per voice, so a "DJ filter" sweep on the whole kit is cheap and automatable. All 42 factory kits have a flanger as kit-level mod FX.
- FX-lane fallbacks are synth presets loaded into kit rows (`loadSynthToDrum`). They follow the synth columns.
- One factory kit (`041 Jonathan Snipes (Waterfalls).XML`) did not parse as XML (mismatched tag at line 1191), so the numbers above cover 42 of 43 kits.

## 4. Factory preset survey

171 synth presets. Roles were guessed from file names (approximate): bass 25, lead 14, pad 38, arp 13, keys/instrument 36, other/FX 45. By mode: subtractive 103, FM 54, ringmod 14. Pads and keys lean FM, and basses and leads are mostly subtractive.

**Share of presets that set a param away from off/neutral:**

| Param | All | Subtr | FM | Ringmod | Bass | Lead | Pad | Arp | Keys | Other |
|---|---|---|---|---|---|---|---|---|---|---|
| lpfFrequency (< open) | 75% | 88% | 46%* | 86% | 80% | 100% | 76% | 77% | 61% | 73% |
| oscBVolume | 75% | 82% | 61% | 79% | 76% | 79% | 84% | 77% | 72% | 67% |
| modulator1Amount | 47% | 22% | 94% | 50% | 20% | 43% | 61% | 46% | 64% | 40% |
| modulator2Amount | 35% | 21% | 59% | 43% | 12% | 36% | 55% | 31% | 47% | 22% |
| delayFeedback | 37% | 46% | 22% | 36% | 20% | 21% | 50% | 77% | 28% | 38% |
| reverbAmount | 25% | 27% | 20% | 29% | 12% | 7% | 37% | 38% | 19% | 29% |
| oscAPulseWidth | 17% | 21% | 0% | 50% | 24% | 21% | 29% | 23% | 8% | 7% |
| hpfFrequency | 16% | 23% | 0% | 21% | 16% | 14% | 21% | 38% | 11% | 9% |
| portamento | 16% | 18% | 13% | 7% | 16% | 29% | 11% | 15% | 11% | 20% |
| modulator1Feedback | 13% | 6% | 26% | 14% | 8% | 21% | 21% | 23% | 11% | 4% |
| treble (EQ) | 13% | 16% | 9% | 7% | 4% | 21% | 21% | 8% | 6% | 16% |
| noiseVolume | 10% | 14% | 4% | 7% | 4% | 7% | 5% | 15% | 8% | 18% |
| carrier1Feedback | 8% | 4% | 17% | 7% | 8% | 7% | 11% | 8% | 8% | 7% |
| sampleRateReduction | 6% | 10% | 0% | 7% | 0% | 21% | 3% | 15% | 0% | 11% |
| bitCrush | 2% | 3% | 2% | 0% | 0% | 7% | 3% | 0% | 0% | 4% |

\* The FM LPF values are stored but inert, because the filter is forced off on load (§3.2).

**Other settings:**

- Mod FX: none on 142 presets, chorus 16, flanger 8, phaser 5.
- Saturation (`clippingAmount` > 0): 28 presets (22 subtractive), mostly 2–5.
- Unison > 1: 86 presets.
- LPF mode: 24dB 85, 12dB 49, 24dB-drive 37 (all ladder).
- Arp on: 15. Wavefold, morph and SVF: none (they post-date the factory set).

**Medians in menu units (all presets):**

- env1: attack 8, decay 20, sustain 45, release 13.
- env1 sustain is 50 in 57 of 103 subtractive, 17 of 54 FM and 4 of 14 ringmod presets.
- LPF cutoff: 34 (subtractive), 36 (ringmod).
- FM modulator 1: 12 (p10–p90 4–21).
- Delay feedback p90: 18. Reverb p90: 11.

These ranges back the offsets in §1.

**Most common cables:**

| Cable | Presets |
|---|---|
| velocity → volume | 171 |
| lfo1 → pitch (vibrato) | 69 |
| env2 → LPF | 64 |
| note → LPF | 60 |
| env2 → mod1 vol | 40 |
| note → mod1 vol | 39 |
| env2 → mod2 vol | 36 |
| velocity → LPF | 22 |
| env2 → mod1 feedback | 17 |
| lfo1 → LPF | 16 |

**Gold knobs.** The params designers put on the gold knobs show which ones they expect a player to turn live. Subtractive and ringmod: LPF freq and resonance, env1 attack and release, delay feedback and rate, reverb, LFO1 rate, portamento. FM: modulator 1 and 2 volume (53 and 52 of 54), then modulator 1 feedback (32), carrier 1 feedback (25), modulator 2 feedback (16) and carrier 2 feedback (12). Sample-rate reduction appears on 41 subtractive presets. This matches the mapping in §1: in FM the knobs designers chose are the FM index and feedback, not the filter.

## 5. Coverage: which controls have a target

Per mode, how many factory presets give each control something to move:

| | Subtractive (103) | FM (54) | Ringmod (14) |
|---|---|---|---|
| Brightness | 103 (LPF always runs, since the default Y → LPF cable is added on load) | 54 (modulator 1 always in use) | 14 |
| Decay (sustain < 50, so `env1Decay` acts) | 46 | 37 | 10 |
| Wobble (LFO cable or mod FX) | 74 | 32 | 8 |
| Space: reverb already > 0 | 28 | 11 | 4 |
| Space: delay already > 0 | 47 | 12 | 5 |

Release always has an effect, so Decay is never a no-op. Space via reverb send works from 0, so it covers all presets. Delay is only an extra target.

## 6. Candidates beyond the five

| Candidate | Verdict | Why |
|---|---|---|
| HPF | **Own control: Thin** | Build-ups and breaks need "less bottom"; Brightness should mean more top. It's cheap at kit level. Raising it from 0 turns the filter on per voice. Inert on factory FM presets. |
| FM mod index | Folds into **Brightness** (FM) | It is what designers put on the gold knobs. |
| FM feedback | Folds into **Drive** (FM) | Makes the sound noisier, not just brighter. Always smoothed. |
| Filter morph | Folds into **Drive** on ladder LPFs only | On SVF modes it is a filter-shape morph, not drive. |
| Wavefold | Folds into **Drive** | Automatable, pre-filter, no effect on squares. |
| Saturation (`clippingAmount`) | Leave out by default; Perform-only Drive extra | It's a setting, not a param: Compose can't automate it, and every step changes gain audibly. |
| Bitcrush / SRR | Folds into **Drive** as a genre opt-in | Rare in presets (2% / 6%). Lo-fi is a genre choice, not a general "dirtier". |
| Mod FX depth / rate | Fold into **Wobble** | Only when the preset already has a mod FX type. |
| LFO rates | Fold into **Wobble** | Only when an LFO is cabled. |
| LFO cable amounts (e.g. lfo1 → pitch in 69 presets) | Fold into **Wobble**, pending #13 | The amount is an `AutoParam`, but scaling it is close to "mod routing". #13 should say whether scaling an existing cable's amount (never adding one) is allowed. Without it, Wobble covers about half as many presets. |
| Osc mix / balance | Leave out | Too preset-specific; can cancel a sub or detune layer. |
| Pulse width | Into **Brightness** for ringmod only | It is the ringmod timbre knob; too rare elsewhere. Disallowed in FM. |
| Unison detune / spread | Leave out | A setting, not a param: can't be automated. |
| Arp rate / gate / arp params | Leave out (they belong to the part's Flavour) | They change notes and rhythm, not timbre. |
| Sample start / pitch on kit rows | Leave out | Start is not a param, and pitch is tuning. |
| Sidechain amount | Leave out of sound controls | It's a cable amount (pump). It belongs to mix and Arc decisions, not timbre. |
| Pan / width | Leave out | Mix decision, set once per part. |

## 7. Open points for the parent tickets

1. **Cable amounts.** May Wobble (and possibly Brightness) scale the amount of a cable the preset already has? Recommendation: yes, within ×0..×2, never adding or rerouting.
2. **Thin as a sixth control.** Recommendation: add it. The Arc uses it on builds; at kit level it's one cheap filter.
3. **Preset curation rules that make controls work.** Re-save curated presets in this firmware, which gives a community version tag. Otherwise FM presets lose their filters on load, and pre-4.0 presets get extra expression cables. For each preset, record which controls have targets: the Generator reads this from the loaded params, so the curator doesn't need to tag it.
4. **Smoothing path.** The single `paramLPF` slot means a multi-param control can't rely on UI-style smoothing. The Automation ticket should pick between per-tick small steps and interpolated automation nodes for Perform. Compose gets interpolation for free.
5. **Not measured:** the CPU cost of turning on a per-voice filter or wavefold across a full voice load, and how big a step makes audible zipper noise. Both need hardware or DelugeEmu listening tests.
