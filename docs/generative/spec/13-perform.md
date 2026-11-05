# 13. Perform

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 13.1 Its own song

- Perform builds its own `Song` from the Current Recipe, with one Output per part. It never layers onto the player's song.
- Entry ([§14.3](14-entry-and-compose-screen.md#143-replacing-the-song)) builds it privately while the current song keeps playing, swaps it in **stopped**, and opens on the SONG page, waiting for PLAY.
- Steering edits the Current Recipe itself, so the Recipe is always the current centre. Arc state, mutes, solos, holds, freezes and focus are **performance state**: never stored, and lost on stop or exit.
- A sound-control home is the param's value on the part's Output, so it lasts until that Output is replaced (genre change, Regenerate, LOAD, sound re-roll). Homes are never saved in a Recipe file.
- The normal song, clip and arranger views can't be reached from Perform.

### 13.2 Transport, Start and End

- PLAY is the only start and stop. There's no separate "Generator on".
- **Start.** PLAY from stop plays a one-phrase **Intro** (Rise across it), then Groove at the next phrase line (Reset on its downbeat). SHIFT+PLAY skips the Intro and starts in Groove. Every start is fresh: the Arc resets and a new piece begins.
- **Stop.** PLAY stops at once, the Deluge way: notes released, tails ring out.
- **End** queues a one-phrase **Outro** from the next phrase line (Close and Wash across it). Energy and Density fall and parts leave through their gates. The transport stops on the following phrase line. End is armed by holding its pad 0.6 s, and a tap cancels it while queued.
- X+PLAY (stock restart) is blocked.

### 13.3 Perform settings

- **Glide**: how long tempo and swing glides take. Instant, 1 beat, **1 bar** (default), 2 bars, 4 bars or 1 phrase. It is a Perform setting, not a Recipe value, kept until power-off.
- **Transition style** is chosen per change in the confirm ([§9.7](09-arc.md#97-transitions)), not stored.

### 13.4 Live actions

| Action | Grain |
|---|---|
| Macros | next undecided step ([§3.4](03-recipe.md#34-steering-grain)) |
| Tempo, swing | glide from now |
| Key, scale | next phrase line, or an Arc Drop if one lands first ([§9.7](09-arc.md#97-transitions)) |
| Genre, Regenerate, Recipe LOAD | transition ([§9.7](09-arc.md#97-transitions)) |
| Force a Section, hold | next phrase line / now |
| End | next phrase line |
| Part mute and solo | next bar. SHIFT = now |
| Sound re-roll; back to previous sound | preload, then next phrase line |
| Pattern re-roll (new take); step back or forward through takes | next phrase line |
| Freeze / unfreeze | next undecided step |
| Fill | while held |
| Capture mark | now |
| Sound-control home edits | now |

- A muted part keeps generating ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)). Unmuting a part a Break has silenced brings it back only when the Arc does.
- "Back to previous sound" remembers one previous pick per part, as performance state.

### 13.5 Control layout

Stock habits come first: a control does what its printed label or stock behaviour suggests wherever Perform has a matching action. The prototype on [`prototype/perform-control-layout`](https://github.com/gismya/deluge-generative/tree/prototype/perform-control-layout) at `111e4968` is the visual reference.

**Grid pages.** SONG, CLIP and KEYBOARD switch the grid page, and their LEDs show which is on. The sidebar is the same on every page.

- **SONG, the Arc map.** One column per phrase. Column 4 is now; columns to its left have played and columns to its right are the Arc's plan.
  - Rows 1–6, parts: bright = sounded, the now column flashes live, dim = planned to sound. Holding a part's row focuses it.
  - Row 7: effective Energy against the Recipe centre. Blue below, green at, orange above.
  - Row 8: the Section of each phrase.
  - A queued action blinks in the column where it lands, and the plan redraws to show its result.
  - Turning X scrolls. Tapping X returns to now.
- **CLIP, one part in depth.** It shows the last part held or locked, and choosing this page doesn't move the gold knobs.
  - Row 1: takes 1–16. Bright = playing, dim = an earlier take, grey = a new take, blinking = queued. Tapping a take queues it.
  - Row 2: FREEZE, NEW SOUND (SHIFT = previous sound), SOUND EDITOR.
  - Rows 3–8: Brightness, Thin, Decay, Space, Drive and Wobble as horizontal faders. Tapping a pad sets home. The white pad is the live value, drift included.
- **KEYBOARD, performance pads** (stock KEYBOARD opens PerformanceView, so this keeps the habit).
  - Columns 1–8: Energy, Density, Complexity and Motion as faders, two columns each. Tap = new centre. Hold = a temporary value that springs back on release. White = where the Arc has the value now.
  - Column 14, rows 1–4: phrase position.
  - Columns 14–16, rows 5–8: FILL, held.

**Sidebar**, the same on every page:

- **MUTE/LAUNCH, rows 1–6:** part mute at the next bar; SHIFT = now. X held + MUTE pad = solo, as stock. Colours are stock: green active, red muted, blue solo, dim = silenced by the Arc or soloed out.
- **AUDITION/SECTION:** rows 1–3 force Groove, Break or Build (again cancels). Row 5 holds the Arc. Row 8 is End: hold 0.6 s to arm (the pad brightens while held); a tap cancels.

**OLED.** Only the stock 7, 9, 13 and 20 px fonts, no 5 px text. Popups are stock style.

- **Status line, always shown.** Left: Section and count, and HELD. Right: the next queued action as an inverted tag with its bar count (`>BR3`, plus `+` when more are queued), or bar.beat when nothing is queued. EXT when following an external clock. A Capture dot: filled = marked, blinking = log full. A phrase bar underneath. STOPPED when stopped.
- **The page follows the grid page.** SONG → ARC: now and the next 7 phrases as boxes (queued ones blink, Drop marked) and the Arc's next change. CLIP → PART: sound pick, take n/m, frozen, and the six controls as bars (home plus a live tick). KEYBOARD → MACROS: four bars, fill = centre, tick = where the Arc has it.
- **SELECT turn** shows QUEUE (every queued action with its countdown), RECIPE or CAPTURE for 4 s. **SELECT push** opens the Perform menu ([§13.6](#136-perform-menu)).

**Gold knobs.** Pages are stock everywhere, and the F page is remembered per target, as stock does. Perform owns the mod buttons, so pressing F1 again never turns the VU meter on over the mute column. Knob LEDs show the value; for macros, that's the centre.

- **Recipe level** (nothing focused): F8 = Energy / Density. Pressing F8 again switches to Complexity / Motion, and the F8 LED blinks while that set is on. This displaces stock's song-level decimation and bitcrush. F1–F7 stay stock song FX, pushes included (F2 filter type, F7 stutter).
- **Part focused:** the part's stock Sound pages. Turning a param a control targets sets that control's home: CUTOFF = Brightness, RELEASE = Decay, REVERB and DELAY AMOUNT = Space, MOD DEPTH = Wobble. Pressing F7 again gives Thin / Drive. Plain F7 stays stutter / CUSTOM 1.
- **Focus:** holding a part's row on the SONG page puts it on the knobs while held, like holding a clip. Double-tap locks it. AFFECT ENTIRE unlocks, and its LED is lit at Recipe level.

**Buttons.**

| Control | In Perform |
|---|---|
| PLAY / SHIFT+PLAY | Start with the Intro, or stop / start without the Intro. X+PLAY is blocked |
| RECORD | Capture mark (LED on = marked, blinks when the log is full). SHIFT+RECORD stays RESAMPLE |
| SYNC-SCALING | Fill while held, whatever the community setting |
| TEMPO turn / SHIFT+turn | Recipe tempo / swing, gliding (needs the tempo hook, F4). TAP taps the Recipe tempo. SHIFT+TAP = metronome |
| SCALE | key and scale editor. The LED blinks while a key change is queued |
| SYNTH or KIT | focused part: new sound at the phrase line. SHIFT = back to the previous sound |
| CROSS-SCREEN | focused part: freeze (LED = frozen) |
| Y turn / Y push | focused part: take back / forward (forward past the newest makes a new take) / show key, scale and seed |
| SHIFT + main pad | sound editor at the printed param: the focused part's Sound, or song FX with nothing focused. Pads, PLAY and knobs stay live. BACK closes |
| BACK tap / hold | cancel the newest queued action / open the leave menu ([§13.8](#138-leaving)) |
| SAVE / SHIFT+SAVE | quick save while playing, full save UI when stopped / always the full save UI ([§16.4](16-recipe-files.md#164-save)) |
| LOAD | Recipe load browser ([§16.5](16-recipe-files.md#165-load)) |
| MIDI, CV | reserved for later outputs |
| TRIPLETS | unused. LEARN stays stock |

**Queued actions.** A queued action blinks on its pad and shows in the status line, and QUEUE lists every one. Pressing the action again cancels it, and so does a BACK tap, newest first. Quantised actions close 12 ticks early ([§12.2](12-runtime.md#122-when-steps-are-decided)).

**Pad redraws.** A full pad redraw costs 21–22 ms of UART, shared with the OLED. Redraw only changed rows. Once per 16th uses about 19% of the link at 130 BPM, and the ceiling is about 45 full redraws a second.

### 13.6 Perform menu

Stock look, opened by SELECT push:

- CANCEL CHANGE (shown only while a genre change, Regenerate or LOAD is queued)
- GENRE (the current genre is labelled `= REGEN`)
- REGENERATE
- KEY / SCALE
- GLIDE
- SEED + TAKES
- CAPTURE

Genre and Regenerate confirm with a choice: BUILD INTO A DROP or HARD CUT AT PHRASE.

### 13.7 Sound editor, MIDI and external tempo

**Sound editor overlay.** A part's normal sound editor opens inside Perform while it keeps playing (SHIFT + main pad, or SOUND EDITOR on the CLIP page). Pads, PLAY and knobs stay live with no grey-out. Knob moves set homes ([§11.6](11-sound-controls.md#116-home-and-player-edits-perform)). It needs F6 and F7.

**MIDI in.**

- **MIDI-follow CCs** go where the gold knobs point: the focused part's Sound, or song-level params with nothing focused (F8).
- **External notes are dropped**, with their pitch bend, mod wheel and MPE, whether from MIDI follow or an Output's learned input. They would be missing from the steering log and would bypass the note-off table and mute gate.
- **MIDI-learned and MIDI-follow param edits** move homes like a knob turn and are logged. An incoming CC is read as a live value: home = incoming − current layer offset. LEARN stays stock, so a param learned inside Perform lives on that part's Output and is lost when the Output is replaced. MIDI follow is the route that lasts.
- **Macros over MIDI.** CCs 114–117 on the MIDI-follow channels steer Energy, Density, Complexity and Motion. 0–127 scales to 0–50. They set the Recipe centre, are logged, and get feedback.
- **Feedback** sends each sound control's live value (home plus all layers) at the stock automation-feedback rate; "disabled" means only on change. Stock re-sends only `isAutomated()` params, so Perform needs its own case.

**Global MIDI commands.**

| Command | In Perform |
|---|---|
| PLAY, FILL, SHIFT | as the panel button. PLAY starts with the Intro |
| RECORD | Capture mark |
| TAP | Recipe tempo, as below |
| TRANSPOSE | queues a Recipe key change to the next phrase line: the played note's pitch class becomes the key, the scale is kept |
| PLAYBACK_RESTART | blocked |
| LOOP, LOOP_CONTINUOUS_LAYERING, UNDO, REDO, NEXT_SONG | ignored. Perform has no undo |

**Tempo.**

- TAP sets the Recipe tempo through the tempo hook and glides at the Glide setting.
- No tempo CC. MIDI clock out stays stock and follows the gliding song tempo tick by tick.
- **Under an external clock** the master owns tempo and the glide is off. The Recipe tempo tracks the clock: sampled once per bar, rounded to a whole BPM, logged only when the rounded value changes. A tempo turn shows the stock tempo popup marked EXT and changes nothing. TAP is ignored. SHIFT+turn still steers swing. X+turn nudges sync as stock. LEARN+turn is swallowed (F4), so the clock-out scale can't change inside Perform.
- When the clock stops being followed, the transport stops (stock), and the next PLAY starts at the last tracked tempo.

### 13.8 Leaving

Holding BACK opens the leave menu, a stock context menu, while playback continues. The verb is in the title, so every option fits 13 characters. It starts on the first option.

| Situation | Title | Options |
|---|---|---|
| Marked pieces, last one unmarked | `KEEP: 2 MARKED` | MARKED + LAST, MARKED ONLY, NOTHING, CANCEL |
| The last piece is itself marked | `KEEP: 2 MARKED` | MARKED, NOTHING, CANCEL |
| No marks | `LEAVE, KEEP:` | LAST, NOTHING, CANCEL |
| No piece was played | `LEAVE PERFORM?` | LEAVE, CANCEL |

- Any choice but CANCEL stops the transport.
- If pieces are kept, Capture builds and saves them ([§17.7](17-capture.md#177-building-on-leave)). The player lands on the last saved song in arranger view.
- If nothing is kept, the player lands on a new blank song in session view, the same as SHIFT+LOAD → NEW.

Sources: [Perform interaction model][t15], [Perform control layout][t38], [Entering Generative mode and the Compose screen][t39], [Compose screen and Generative menu prototype][t50], [External tempo and MIDI in Perform][t51], [Generator randomness and seeding][t17], [Deluge UI conventions and free controls][t33], [Measure memory and load times on a Deluge][t37], [Recipe files][t40], [Capture][t35], [Firmware changes the design needs][t43].

[t15]: https://github.com/gismya/deluge-generative/issues/15
[t17]: https://github.com/gismya/deluge-generative/issues/17
[t33]: https://github.com/gismya/deluge-generative/issues/33
[t35]: https://github.com/gismya/deluge-generative/issues/35
[t37]: https://github.com/gismya/deluge-generative/issues/37
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t39]: https://github.com/gismya/deluge-generative/issues/39
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t43]: https://github.com/gismya/deluge-generative/issues/43
[t50]: https://github.com/gismya/deluge-generative/issues/50
[t51]: https://github.com/gismya/deluge-generative/issues/51
