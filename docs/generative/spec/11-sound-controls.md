# 11. Sound controls and automation

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 11.1 The six sound controls

The Generator changes sounds live only through six **sound controls**: **Brightness, Decay, Drive, Space, Wobble, Thin**. Each moves a list of concrete params within a range around its **home**, which is how the preset was saved, or wherever the player has since set it ([§11.6](#116-home-and-player-edits-perform)).

- On Bass, Chords, Pad and Lead, each control exists once per part.
- On Drums and FX, each control exists per lane (row params) and once kit-wide (kit-level params).
- A target only takes part if the loaded preset already uses it, so on some presets a control does nothing.
- Each control has a default range relative to home, and a palette entry may override it for one preset.

### 11.2 Mapping per engine

Offsets are in menu units (0–50), relative to the stored value. Every range is a starting estimate (*tuning*).

| Control | Subtractive / wavetable | FM | Ringmod | DX7 osc | Kit row (sample) |
|---|---|---|---|---|---|
| Brightness | LPF ±8; wave index ±5 | modulator 1/2 volume ±6 (modulators in use only) | LPF ±8; pulse width ±6 | LPF −10..0 | LPF −15..0 (kit level −20..0) |
| Decay | env1 release ±6; env1 decay ±6 if sustain < 50; env2 decay ±6 | same | same | cut only | cut only (lower sustain and decay) |
| Drive | ladder `lpfMorph` 0..+15; fold 0..+10 | modulator and carrier feedback 0..+8; fold | fold; ladder morph | fold | bitcrush / SRR, genre opt-in |
| Space | reverb send 0..+10; delay feedback ±4 only if already > 0. Throw has its own delay range ([§11.4](#114-gestures)) | same | same | same | reverb send. Throw also moves the row's or kit's delay feedback where there is a delay setup |
| Wobble | mod FX depth ±8 if type ≠ none; LFO rate ±4 if cabled; existing cable amounts ×0..×2 | same | same | same | kit-level mod FX |
| Thin | HPF 0..+20 | none on factory FM | HPF 0..+20 | HPF if on | HPF (kit level 0..+25) |

Engine notes:

- In FM, brightness is modulator level, and Drive is feedback.
- `lpfMorph` is ladder drive on 12/24/24-drive LPFs, but an LP→BP→HP morph on SVFs. Wavefold does nothing on squares.
- DX7 operator levels and envelopes are `DxPatch` bytes, not params, so they can't move. The factory card has no DX7 or wavetable presets.
- All kit rows on the factory card are one-shots with sustain 50 and the LPF open, so Brightness can only darken and Decay can only shorten. Whole-drum gestures use the kit's affect-entire filter, reverb and mod FX.
- Wobble scales existing mod cables (lfo1 → pitch is in 69 presets) but never adds cables and never switches a mod FX on. A mod FX type change allocates memory.
- The macro and drift layers move delay feedback only where the preset already uses it. Only Throw raises it from 0, and only on a part, row or kit with a delay setup ([§10.6](10-sounds.md#106-load-time-settings)). The stock render allocates the buffer on demand from SDRAM and frees it when the repeats run out. Thin rising from 0 switches on per-voice HPF processing, but only during Builds.
- Not params, so never moved and never automated: saturation, unison count and detune, filter and mod FX types, sample start, every DX7 operator setting.

### 11.3 Three layers

A control's offset from home is **macro layer + gesture layer + drift layer**, summed and clamped to the control's range.

**Macro layer.** One Generator-wide sign table. A genre weights each entry ×0..×2 and can never flip a sign. It reads **effective** Energy, so Section changes shade the sound with no extra rules.

| | Brightness | Decay | Drive | Space | Wobble | Thin |
|---|---|---|---|---|---|---|
| Energy | + | − | + | − (drier at peak) | 0 | 0 |
| Density | 0 | 0 | 0 | 0 | 0 | 0 |
| Complexity | 0 | 0 | 0 | 0 | 0 | 0 |
| Motion | no static offset. It sets drift depth and speed ([§11.5](#115-drift)) |||||

Thin is left free for Builds.

### 11.4 Gestures

A fixed Generator-wide set. A genre switches each gesture on per part (or aims it at one lane, such as Thin on the snare in a Build roll) and sets its depth ×0..×2.

| Gesture | When | What | Shape |
|---|---|---|---|
| Close | entering a Break; across an Outro | Brightness down | eases in over 1 bar |
| Wash | during a Break; across an Outro | Space up | eases in over 1 bar |
| Rise | across a Build; across an Intro | Thin and Brightness up. A synth Riser also gets Space up | ease-in over the whole Section, peaking on its last step |
| Throw | last hit before a Break or Gap; outgoing parts' last hit in a transition; a Siren's release; a Drop-out's throw target ([§8.8](08-rhythm-and-expression.md#88-drop-out)) | Space spike: reverb send, plus delay feedback where there's a delay setup | holds while its part is silent, then decays over 1 beat |
| Reset | Drop downbeat; first Groove downbeat after an Intro | every gesture offset snaps to 0 | one step-exact jump |

- A Gap is silence, so it is made of notes, not automation.
- **Throw and delay.** On a part, kit row or kit with a delay setup ([§10.6](10-sounds.md#106-load-time-settings)), Throw also spikes delay feedback. Its genre depth is in menu units, separate from Space's ±4 and capped at +30 (*tuning*), below the firmware's own feedback clamp. On the Deluge the delay amount is the feedback, so a high spike on a part that then goes silent echoes only its last hit.
- **Throw hold.** A Throw holds while its part is silent, up to the genre's **throw hold** (1 bar by default, *tuning*), then decays over 1 beat. A part that plays again sooner comes back at home. In a Drop-out, the hold lasts the whole drop-out and the fall takes its last beat, so the part returns dry ([§8.8](08-rhythm-and-expression.md#88-drop-out)).
- On Drums and FX, the macro layer and gestures act on the **kit-wide** controls, unless a gesture is aimed at a lane. FX lanes always use their own row controls.

### 11.5 Drift

- A seeded, smoothed random walk per part on **Brightness, Decay, Space and Wobble**. Drive and Thin never drift.
- Depth is Motion × genre weight, up to about half the control's range at Motion 50.
- The period is about one phrase at Motion 25, half that at 50, and close to frozen at 0.
- On Drums and FX, drift acts on **per-lane** controls (hat Decay wandering, for example).
- Drift is not covered by takes and keeps running under freeze ([§6.6](06-randomness.md#66-takes), [§6.7](06-randomness.md#67-freeze)).

### 11.6 Home and player edits (Perform)

- When the player moves a param that a control targets, the **home moves by the amount turned**: home = new value − current layer offset. The Generator keeps offsetting around the new home.
- This applies to gold-knob turns, the sound editor overlay, the CLIP page faders, MIDI-learned params and MIDI follow ([§13.7](13-perform.md#137-sound-editor-midi-and-external-tempo)). All of them arrive through `AutoParam::setCurrentValueInResponseToUserInput` (F3 in [§19](19-firmware-changes.md#19-firmware-changes)).
- Every home change is logged as steering ([§17.2](17-capture.md#172-the-steering-log)).
- The home rule is provisional. Check it in beta testing.
- The Generator's own writes are never recorded as clip automation, even with a clip armed (F9 guards RECORD).

### 11.7 Grain

- The Generator works out each control's target once per 16th step and ramps linearly between steps. Only snaps (Reset) jump.
- **Perform** ramps through a new `AutoParam` live ramp (F2): target plus length in ticks, no nodes. The firmware then moves the param once per render window through its existing `tickSamples` path. The Generator rewrites the target every step, because the ramp has no stop. Snaps use `setCurrentValueWithNoReversionOrRecording`. Perform's clips hold no automation, so nothing competes with the ramp. This avoids zipper on params the single `paramLPF` smoothing slot can't cover.
- **Compose** writes automation nodes ([§15.6](15-compose-output.md#156-automation)).

### 11.8 What the Generator never automates

- Level and pan. Volume automation would override the player's fader in every Compose clip. Swells use velocity and Brightness, so the mix stays the player's.
- Arp and randomizer params. The Generator writes its own notes.
- Tempo and swing. Those are Recipe steering ([§3.4](03-recipe.md#34-steering-grain)). In Capture, tempo changes become song tempo automation ([§17.6](17-capture.md#176-changes-partway-through-a-piece)).

Sources: [Automation][t14], [Sound controls per synth engine][t26], [Sound generation][t13], [Generator scheduling][t19], [External tempo and MIDI in Perform][t51], [Perform interaction model][t15], [Perform control layout][t38], [Generative pack contents][t42], [Dub's drop-outs and delay throws][t67].

[t13]: https://github.com/gismya/deluge-generative/issues/13
[t14]: https://github.com/gismya/deluge-generative/issues/14
[t15]: https://github.com/gismya/deluge-generative/issues/15
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t26]: https://github.com/gismya/deluge-generative/issues/26
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t42]: https://github.com/gismya/deluge-generative/issues/42
[t51]: https://github.com/gismya/deluge-generative/issues/51
[t67]: https://github.com/gismya/deluge-generative/issues/67
