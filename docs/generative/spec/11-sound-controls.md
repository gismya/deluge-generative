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
- What the driver binds, as built: a target takes part only if the preset uses it as a piece starts (the filter isn't off, env1's sustain is below 50 for its decay, env2 or an LFO is patched to something, delay feedback or a modulator level is above 0, the mod FX type isn't none, a DX7's HPF is above 0). Wave index moves each wavetable oscillator, pulse width both oscillators, feedback each modulator in use and both carriers, LFO rate each LFO patched to something, and Wobble's cable scaling every cable from an LFO to a param other than a level or pan ([§11.8](#118-what-the-generator-never-automates)). A cable scaled to ×0 is left at the smallest amount on its side, because the firmware deletes a cable set to 0. A cable is found by its source and destination each time it moves, since an edit to the patching reorders the cables. A param that holds automation is left alone. Kit-row Decay lowers env1's sustain and decay, each −6..0 (*tuning*). Kit-row Drive (bitcrush and SRR) waits for the first genre that opts in.

### 11.3 Three layers

A control's offset from home is **macro layer + gesture layer + drift layer**, summed and clamped to the control's range.

**Macro layer.** One Generator-wide sign table. A genre weights each entry ×0..×2 and can never flip a sign. It reads **effective** Energy, so Section changes shade the sound with no extra rules.

- A macro's entry is measured from the genre's default for that macro, so a fresh Recipe in a Groove plays every sound at home. At weight ×1, a macro 25 points from its default moves a control across its whole range on that side (*tuning*): a Techno Break, 10 points of Energy below the centre, is 40% darker, longer and wetter. Drive's ranges start at home, so its negative macro entry moves nothing: Drive only rises above home.
- Every part gets the same macro offset. On Drums it moves the kit-wide controls, and on FX each lane's own ([§11.4](#114-gestures)).

| | Brightness | Decay | Drive | Space | Wobble | Thin |
|---|---|---|---|---|---|---|
| Energy | + | − | + | − (drier at peak) | 0 | 0 |
| Density | 0 | 0 | 0 | 0 | 0 | 0 |
| Complexity | 0 | 0 | 0 | 0 | 0 | 0 |
| Motion | no static offset. It sets drift depth and speed ([§11.5](#115-drift)) |||||

Thin is left free for Builds.

### 11.4 Gestures

A fixed Generator-wide set. A genre switches each gesture on or off **per part and per moment**: each entry in a row's "When" column has its own switch, so a Techno Intro can stay unfiltered while its Builds still Rise. Every switch defaults to on, so a genre lists only the ones it turns off. A gesture may also be aimed at one lane, such as Thin on the snare in a Build roll. Depth is set per gesture and part, ×0..×2.

| Gesture | When | What | Shape |
|---|---|---|---|
| Close | entering a Break; across an Outro | Brightness down | eases in over 1 bar |
| Wash | during a Break; across an Outro | Space up | eases in over 1 bar |
| Rise | across a Build; across an Intro | Thin and Brightness up. A synth Riser also gets Space up | ease-in over the whole Section, peaking on its last step |
| Throw | last hit before a Break or Gap; outgoing parts' last hit in a transition; a Siren's release; a Drop-out's throw target ([§8.8](08-rhythm-and-expression.md#88-drop-out)) | Space spike: reverb send, plus delay feedback where there's a delay setup | holds while its part is silent, then decays over 1 beat |
| Reset | Drop downbeat; first Groove downbeat after an Intro | every gesture offset snaps to 0 | one step-exact jump |

- **Moments with switches.** Close: entering a Break, across an Outro. Wash: during a Break, across an Outro. Rise: across a Build, across an Intro. Throw: before a Break, before a Gap, outgoing in a transition. Reset has no switch, because it only clears.
- A Drop-out's Throw has no switch. The drop-out entry's throw target decides it ([§8.8](08-rhythm-and-expression.md#88-drop-out)), and the part's Throw depth applies. A Siren's release Throw belongs to the Siren lane and is always on.
- A Gap is silence, so it is made of notes, not automation.
- **Throw and delay.** On a part, kit row or kit with a delay setup ([§10.6](10-sounds.md#106-load-time-settings)), Throw also spikes delay feedback. Its genre depth is in menu units, separate from Space's ±4. On the Deluge the delay amount is the feedback, so a high spike on a part that then goes silent echoes only its last hit.
- **Throw feedback cap.** While a Throw holds, delay feedback is `max(home, min(home + depth, 23))` in menu units. The cap is on that result, not on the depth, because the spike adds to home, and a Throw never lowers feedback. Each repeat's gain is the menu value / 25 (`dsp/delay/delay.cpp`), so 23 is a gain of 0.92 (about −0.7 dB a repeat), just under the firmware's 15/16 clamp. That clamp only applies to clips that aren't active, so it never covers Perform. At 25 and above an echo grows with every repeat and the delay never abandons (`repeatsUntilAbandon` 255). Under the cap a held Throw always fades, and its fall back to home cuts the tail. If home is already 23 or more, the Throw adds nothing to the delay, its reverb spike still plays, and the one-phrase cap ends any tail ([§12.9](12-runtime.md#129-instances-and-the-transition)). Depth and throw hold don't depend on delay time: every delay setup is tempo-synced, so a genre tunes depth with its own setup. A growing echo is a later feature.
- **Throw hold.** A Throw holds while its part is silent, up to the genre's **throw hold** (1 bar by default, *tuning*), then decays over 1 beat. A part that plays again sooner comes back at home. In a Drop-out, the hold lasts the whole drop-out and the fall takes its last beat, so the part returns dry ([§8.8](08-rhythm-and-expression.md#88-drop-out)).
- On Drums and FX, the macro layer and gestures act on the **kit-wide** controls, unless a gesture is aimed at a lane. FX lanes always use their own row controls.
- **As built** (the Generator, from #107):
  - Peak offsets at depth ×1, in control units (*tuning*): Close Brightness −50, Wash Space +50, Rise Thin +100 and Brightness +40 (a Riser lane's Space +50 more), Throw Space +100. Every layer sums before the clamp, so a Throw on a wet Break can end at +100.
  - Close and Wash start on the first step of a Break or an Outro, ease in linearly over its first bar, and hold until a Reset, so a Build after a Break starts closed and Rise opens it again. The moment that switches them is the latest Section that started them: an Outro after a Break uses its Outro switches.
  - Rise follows `x²`, where `x` is the share of the Build or Intro played, so it reaches its peak on the last step. A held Intro stays at the peak.
  - Reset happens on every Groove downbeat after a piece's first step: the Drop, the first Groove after an Intro, and a Groove forced from a Break. Every Control event on that step snaps, its macro and drift share included.
  - **Throw's last hit.** The Generator decides one step at a time, so it can't know which hit will be a part's last. Every hit in the last beat before the line spikes Space (*tuning*, `kThrowWindowSteps`), and the hold counts from the latest. The line is the Gap, or a Break that the Groove ends into on its own or that a waiting force lands. A force, End or transition that lands first replaces the Break. A Break silences a part its keeps don't keep. On Drums the kit-wide set throws only if the Break keeps no lane, and a lane's set only when the Throw is aimed at it. FX lanes always play in a Break, so they throw only before a Gap. Muted notes count as hits, as they're in the stream. The spike snaps on the hit, and a hit after it outside the window snaps back home. A plan can change after a spike, such as with HOLD or a Motion turn, and then the part's next hit brings it home.
  - Not built yet: Throw on the outgoing parts in a transition (the outgoing instance stops deciding at the start step, so it can't send the hold and the fall), a Siren's release (no genre has a Siren lane yet), and with Dub, Throw on delay feedback with its cap, and Drop-out holds. `GestureMoment::ThrowTransition` is already a switch.

### 11.5 Drift

- A seeded, smoothed random walk per part on **Brightness, Decay, Space and Wobble**. Drive and Thin never drift.
- Depth is Motion × genre weight, up to about half the control's range at Motion 50.
- The period is about one phrase at Motion 25, half that at 50, and close to frozen at 0.
- On Drums and FX, drift acts on **per-lane** controls (hat Decay wandering, for example).
- Drift is not covered by takes and keeps running under freeze ([§6.6](06-randomness.md#66-takes), [§6.7](06-randomness.md#67-freeze)).
- **As built:** the walk passes a seeded point every phrase at Motion 25 and eases between points (smoothstep). Each step moves it on by Motion, so the time between points scales as 25 / Motion. Each set and control sits at its own seeded place between the points. Depth is ±50 control units at Motion 50 and weight ×1, linear in Motion. A part's one drift weight covers its four controls. On Drums the macro layer stays on the kit-wide set, which never drifts. A Sweep counts from the piece's first step and, on a Kit part, moves every lane's Brightness.
- **Sweep.** A genre may give a part's Brightness drift the **Sweep** shape instead of the random walk. A Sweep is a triangle locked to phrase lines, with a period in bars set by the genre that must divide the phrase or be a whole multiple of it (a harness spec checks this). Its starting phase is one of four quarter positions, drawn from the seed and the part, so like the rest of drift it ignores takes. Depth is Motion × genre weight, as for drift, so it stops at Motion 0. The period doesn't scale with Motion. A Sweep replaces the walk on that part's Brightness and sums with the macro and gesture layers. Examples (*tuning*): the Psytrance Lead at 16 bars and the Techno acid Loop at 8.

### 11.6 Home and player edits (Perform)

- When the player moves a param that a control targets, the **home moves by the amount turned**: home = new value − current layer offset. The Generator keeps offsetting around the new home.
- The current layer offset is the control's offset as last applied (the target of a ramp still running), taken in full even where the param's own range clipped it, so the param stays where it was turned until the control's next change. The new home is clamped to the param's own range. A cable Wobble scales has home = new amount ÷ its scale; at ×0, where every home plays the same, the new amount is the home.
- Homes are driver-side. The core sends only offsets ([§2.2](02-architecture.md#22-drivers-and-the-event-stream)), and a home edit never enters it.
- This applies to gold-knob turns, the sound editor overlay, the CLIP page faders, MIDI-learned params and MIDI follow ([§13.7](13-perform.md#137-sound-editor-midi-and-external-tempo)). All of them arrive through `AutoParam::setCurrentValueInResponseToUserInput` (F3 in [§19](19-firmware-changes.md#19-firmware-changes)). A CLIP fader tap sets the home itself, so the param plays home plus the current offset.
- A param a control moves is bound as a piece starts ([§11.2](#112-mapping-per-engine)), so an edit moves a home only while a piece plays. Edits made while stopped are on the Output, and the next piece reads them as homes ([§11.7](#117-grain)).
- Every home change acts at once in the Perform driver and is logged as driver-side steering at the next undecided step ([§2.6](02-architecture.md#26-steering-into-the-core), [§17.2](17-capture.md#172-the-steering-log)), as a `SetHome` entry with the new home.
- The home rule is provisional. Check it in beta testing.
- The Generator's own writes are never recorded as clip automation, even with a clip armed (F9 guards RECORD).

### 11.7 Grain

- The Generator works out each control's target once per 16th step and ramps linearly between steps. Only snaps (Reset, a piece's first step) jump.
- **Perform** ramps through a new `AutoParam` live ramp (F2): a target plus a length in internal (unswung) ticks, no nodes, and the ramp stops at its target. The firmware then moves the param once per render window through its existing `tickSamples` path, which advances in internal-tick time. The core sends a Control event only when a target changes, and the adapter arms a ramp only then, one unswung 16th long at the current magnitude. On a long swung 16th the ramp arrives early and holds. On a short one, the next ramp starts from wherever the value is. Snaps use `setCurrentValueWithNoReversionOrRecording`. Perform's clips hold no automation, so nothing competes with the ramp. This avoids zipper on params the single `paramLPF` smoothing slot can't cover.
- A Control event plays on its step's grid line and ramps across that step. A piece's first step snaps its offsets, so its first notes already sound shaded. A jump keeps a decided Control event and plays it at once, as it does a Section start.
- The Perform driver reads homes from the parts' Outputs as each piece starts, and a stop (or the piece's end, or leaving) puts every param it moved back at its home at once. Player edits made while stopped are the next piece's homes; edits while playing come with [§11.6](#116-home-and-player-edits-perform).
- **Compose** writes automation nodes ([§15.6](15-compose-output.md#156-automation)).

### 11.8 What the Generator never automates

- Level and pan. Volume automation would override the player's fader in every Compose clip. Swells use velocity and Brightness, so the mix stays the player's.
- Arp and randomizer params. The Generator writes its own notes.
- Tempo and swing. Those are Recipe steering ([§3.4](03-recipe.md#34-steering-grain)). In Capture, tempo changes become song tempo automation ([§17.6](17-capture.md#176-changes-partway-through-a-piece)).

Sources: [Automation][t14], [Sound controls per synth engine][t26], [Sound generation][t13], [Generator scheduling][t19], [External tempo and MIDI in Perform][t51], [Perform interaction model][t15], [Perform control layout][t38], [Generative pack contents][t42], [Dub's drop-outs and delay throws][t67], [Arc headroom, gestures per Section, and Motion][t72], [Interface between the Generator core and its drivers][t75], [Determinism details: tick unit, tempo, held notes and ramps][t77], [Measure silent incoming Kits and retiring Outputs on a Deluge][t83], [Throw depth against the delay's never-abandon line][t85].

[t13]: https://github.com/gismya/deluge-generative/issues/13
[t14]: https://github.com/gismya/deluge-generative/issues/14
[t15]: https://github.com/gismya/deluge-generative/issues/15
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t26]: https://github.com/gismya/deluge-generative/issues/26
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t42]: https://github.com/gismya/deluge-generative/issues/42
[t51]: https://github.com/gismya/deluge-generative/issues/51
[t67]: https://github.com/gismya/deluge-generative/issues/67
[t72]: https://github.com/gismya/deluge-generative/issues/72
[t75]: https://github.com/gismya/deluge-generative/issues/75
[t77]: https://github.com/gismya/deluge-generative/issues/77
[t83]: https://github.com/gismya/deluge-generative/issues/83
[t85]: https://github.com/gismya/deluge-generative/issues/85
