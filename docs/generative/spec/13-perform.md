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
- **Start.** PLAY from stop plays the genre's **Intro**, one phrase by default ([§9.3](09-arc.md#93-section-lengths)), with Rise across it, then Groove at the next phrase line (Reset on its downbeat). SHIFT+PLAY skips the Intro and starts in Groove. Every start is fresh: the Arc resets and a new piece begins.
- **Stop.** PLAY stops at once, the Deluge way: notes released, tails ring out. Every queued action is dropped, a key change included, so the next start plays the key the last piece played ([§2.6](02-architecture.md#26-steering-into-the-core)).
- **End** queues the genre's **Outro** from the next phrase line, one phrase by default ([§9.3](09-arc.md#93-section-lengths)), with Close and Wash across it. Energy and Density fall and parts leave through their gates. The transport stops on the phrase line where the Outro ends. End is armed by holding its pad 0.6 s, and a tap cancels it while queued. Once the Outro plays, End does nothing. A Section forced to start on the line where the Outro ends cancels the ending: the transport keeps going in the forced Section. *(Implementation choice in #108, for the user to confirm.)*
- X+PLAY (stock restart) is blocked.

### 13.3 Perform settings

- **Glide**: how long tempo and swing glides take. Instant, 1 beat, **1 bar** (default), 2 bars, 4 bars or 1 phrase. It is a Perform setting, not a Recipe value, kept until power-off, set in the Perform menu's GLIDE ([§13.6](#136-perform-menu)). A transition's Build or Outro sets its own glide length, and a hard cut uses this one ([§9.7](09-arc.md#97-transitions)).
- **Transition style** is chosen per change in the confirm ([§9.7](09-arc.md#97-transitions)), not stored.

### 13.4 Live actions

| Action | Grain |
|---|---|
| Macros | next undecided step ([§3.4](03-recipe.md#34-steering-grain)) |
| Tempo, swing | glide from now |
| Key, scale | next phrase line, or an Arc Drop if one lands first ([§9.7](09-arc.md#97-transitions)) |
| Genre, Regenerate, Recipe LOAD | transition ([§9.7](09-arc.md#97-transitions)) |
| Force a Section, hold | next phrase line / next undecided step |
| End | next phrase line |
| Part mute and solo | next bar. SHIFT = next undecided step |
| Sound re-roll; back to previous sound | preload, then next phrase line |
| Pattern re-roll (new take); step back or forward through takes | next phrase line |
| Freeze / unfreeze | next undecided step |
| Fill | while held |
| Capture mark | now |
| Sound-control home edits | now, logged at the next undecided step |

- "Now" in this table means the next undecided step, because every action reaches the core at a decision ([§2.6](02-architecture.md#26-steering-into-the-core)). Only Capture marks, home edits and tempo and swing glides, which never enter the core, act at once.
- A muted part keeps generating ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)). Unmuting a part a Break has silenced brings it back only when the Arc does.
- "Back to previous sound" remembers one previous pick per part, as performance state.
- While a transition is pending, fact edits, arming End, take and sound actions are refused, and the new roster starts clean ([§12.9](12-runtime.md#129-instances-and-the-transition)).

### 13.5 Control layout

Stock habits come first: a control does what its printed label or stock behaviour suggests wherever Perform has a matching action. The prototype on [`prototype/perform-control-layout`](https://github.com/gismya/deluge-generative/tree/prototype/perform-control-layout) at `111e4968` is the visual reference.

**Grid pages.** SONG, CLIP and KEYBOARD switch the grid page, and their LEDs show which is on. The sidebar is the same on every page.

- **SONG, the Arc map.** One column per phrase. Column 4 is now; columns to its left have played and columns to its right are the Arc's plan.
  - Rows 1–6, parts: bright = sounded, the now column flashes live, dim = planned to sound. Holding a part's row focuses it. "Planned to sound" means the Arc lets the part play on some step of the phrase (its Density gate is open and Break or Build keeps don't stop it) and it is audible there (not muted or soloed out), the same test as the MUTE pad's dim. In a planned column, a part muted there shows faint red. The now column flashes a part that played a note in the previous 16th or in this one so far, so a flash can span two 16ths. It is sampled once a 16th, so the flash redraws the pads at most once per 16th.
  - Row 7: effective Energy against the Recipe centre, averaged over the phrase. Blue below, green at (less than 2 points from it), orange above.
  - Row 8: the Section of each phrase, the one playing its last step, so a Build shorter than a phrase shows in the phrase it ends.
  - A queued action blinks in the column where it lands: a mute, solo, take or sound on its part's row, a force, End or key change on row 8. The plan redraws to show its result. Columns after the line where the Outro ends are dark.
  - Turning X scrolls, back over the last 15 phrases the history keeps and ahead to the end of the 16-phrase plan. Tapping X returns to now; a turn or a solo while X is down isn't a tap.
- **CLIP, one part in depth.** It shows the last part held or locked, and choosing this page doesn't move the gold knobs.
  - Row 1: takes 1–16. Bright = playing, dim = an earlier take, grey = a new take, blinking = queued. Tapping a take queues it. Only the lit pads act: the takes made so far (a queued one counted) and the next new one, which is grey. Tapping the queued take again, or the take that plays, cancels the one queued. While stopped a take acts at once: it is the Recipe's take, and the next start plays it.
  - Row 2: FREEZE, NEW SOUND (SHIFT = previous sound), SOUND EDITOR, on pads 1–3. FREEZE is bright while the part is frozen. NEW SOUND blinks while its sound preloads or waits for the phrase line ([§10.7](10-sounds.md#107-when-sounds-load)). SOUND EDITOR opens the part's sound editor at its root menu ([§13.7](#137-sound-editor-midi-and-external-tempo)).
  - Rows 3–8: Brightness, Thin, Decay, Space, Drive and Wobble as horizontal faders. Tapping a pad sets home. The white pad is the live value, drift included.
  - Each fader shows and sets its control's **lead param** on the part: the first param the control moves there ([§11.2](11-sound-controls.md#112-mapping-per-engine)'s order, among those the preset uses now), such as the LPF for Brightness on a subtractive synth. Its 16 pads span the param's knob range: lit up to home, and the live value's pad white, on home's pad too. A tap sets the param's home ([§11.6](11-sound-controls.md#116-home-and-player-edits-perform)). The control's other params keep their homes; the sound editor reaches them. A row stays dark where the preset doesn't use the control. A Kit part's faders are its kit-wide controls. An FX part has no kit-wide controls (each lane has its own), so its faders stay dark. The faders are read about ten times a second (*tuning*), so a ramp moves the white pad without redrawing the grid every frame.
- **KEYBOARD, performance pads** (stock KEYBOARD opens PerformanceView, so this keeps the habit).
  - Columns 1–8: Energy, Density, Complexity and Motion as faders, two columns each. Tap = new centre. Hold = a temporary value that springs back on release. White = where the Arc has the value now.
  - A fader's 8 rows span 0–50, the bottom row 0 and the top 50: row *n* from the bottom (0–7) is *n* × 50 / 7 points, to the nearest. A press steers the core with its row's value at once (the next undecided step, [§13.4](#134-live-actions)), whether it turns out a tap or a hold, as stock's PerformanceView pads do. Let go within 300 ms it is a tap, and the value stays as the Recipe's centre. Held longer, the release steers the core back to the centre. Either way each value is logged where it lands, so a hold logs two entries. The 300 ms is fixed, the user's choice after trying stock's Hold Press Time (100 ms by default) on the Deluge and finding it too short; the faders never read that setting, so stock's own pads keep it.
  - The row values, steering at the press, and a second pad on a held fader moving the held value (the hold ends with its last pad) are implementation choices. *(Implementation choice in #113, for the user to confirm.)*
  - A piece or a new instance that starts while a fader is held (PLAY, or a Regenerate or genre change landing) plays the held value from its start, as a held FILL does ([§8.7](08-rhythm-and-expression.md#87-fill)); the release springs back to the centre. A tap that spans a start leaves its value as the centre in the Recipe and in the new piece. A gold-knob turn on a held macro moves only its centre (the rings and popup show it): the core keeps the held value, and the release springs back to the new centre. A tap's value wins over a knob turn made during it. *(Implementation choice in #113, for the user to confirm.)*
  - The pads are lit up to the value in effect (the held value while held, brighter), and while playing the pad where the Arc has the value is white, on a lit pad too. Complexity and Motion's white pad is their value, since the Arc never moves them ([§3.3](03-recipe.md#33-macros)). *(Implementation choice in #113, for the user to confirm.)*
  - Column 14, rows 1–4: phrase position, a quarter of the phrase per pad from the top, in the playing Section's colour: played quarters dim, the one playing bright, the rest faint. Dark while stopped. *(Implementation choice in #113, for the user to confirm.)*
  - Columns 14–16, rows 5–8: FILL, held. SHIFT = a fresh pick ([§8.7](08-rhythm-and-expression.md#87-fill)). Any of the 12 pads and SYNC-SCALING hold FILL together: it starts with the first held, with SHIFT as that one went down, and ends with the last let go. The pads are white while FILL is on, from any source (MIDI FILL too), and grey otherwise. *(Implementation choice in #113, for the user to confirm.)*
  - A fader or FILL pad whose release an overlay swallowed lets go when Perform is back on top; a fader held past 300 ms springs back then. *(Implementation choice in #113, for the user to confirm.)*

**Sidebar**, the same on every page:

- **MUTE/LAUNCH, rows 1–6:** part mute at the next bar; SHIFT = at the next undecided step. X held + MUTE pad = solo, as stock. Colours are stock: green active, red muted, blue solo, dim = silenced by the Arc or soloed out. While any part is soloed, only soloed parts sound, muted or not; a muted part's mute takes effect again when the last solo goes, and its pad stays red meanwhile. "Silenced by the Arc" means its Density gate is closed or section rules (Break keeps, the cut, the Gap) stop all its notes.
- **AUDITION/SECTION:** rows 1–3 force Groove, Break or Build (again cancels). Row 5 holds the Arc. Row 8 is End: hold 0.6 s to arm (the pad brightens while held); a tap cancels. Each force pad and End show their Section's colour, bright while it plays; the hold pad is white while held.
- The sidebar does nothing while stopped: mutes, solos, holds and forces are performance state, and a start is fresh.

**OLED.** Only the stock 7, 9, 13 and 20 px fonts, no 5 px text. Popups are stock style.

- **Status line, always shown.** Left: Section and count (the phrase within it, `n/m` when its length is planned, `n` for a Groove), and HELD (H when the right side leaves no room for it, nothing when it leaves none for H either). Right: the next queued action as an inverted tag with its bar count (`>BR3`, plus `+` when more are queued), or bar.beat when nothing is queued. EXT when following an external clock. A Capture dot: filled = marked, blinking = log full. A phrase bar underneath. STOPPED when stopped.
- **The page follows the grid page.** SONG → ARC: now and the next 7 phrases as boxes (queued ones blink, Drop marked) and the Arc's next change. CLIP → PART: the part's name and a synth part's preset (its file name; a Kit part has no one pick, each lane having its own sample, so it shows its name only), take n/m on the right (`T2/3`: the take playing of the takes made, a queued one counted, with `*` while frozen), and the six controls as bars (home plus a live tick), in two columns in the CLIP page's order, each labelled with its name's first three letters. KEYBOARD → MACROS: four bars, fill = centre, tick = where the Arc has it (while playing). Each row starts with the macro's initial and its centre in points (`E25`). *(Implementation choice in #113, for the user to confirm.)*
- **SELECT turn** shows QUEUE (queued actions with their countdowns, soonest first: three lines, so with four or more it shows two and "+N MORE"; a label is cut short to fit beside its countdown in 21 characters), RECIPE or CAPTURE for 4 s. **SELECT push** opens the Perform menu ([§13.6](#136-perform-menu)).

**Gold knobs.** Pages are stock everywhere, and the F page is remembered per target, as stock does. Perform owns the mod buttons, so pressing F1 again never turns the VU meter on over the mute column. Knob LEDs show the value; for macros, that's the centre.

- **Recipe level** (nothing focused): F8 = Energy / Density. Pressing F8 again switches to Complexity / Motion, and the F8 LED blinks while that set is on. This displaces stock's song-level decimation and bitcrush. F1–F7 stay stock song FX, pushes included (F2 filter type, F7 stutter).
- **Part focused:** the part's stock Sound pages. Turning a param a control targets sets that control's home: CUTOFF = Brightness, RELEASE = Decay, REVERB and DELAY AMOUNT = Space, MOD DEPTH = Wobble (and MOD RATE where an LFO is patched). DELAY AMOUNT counts only where the preset uses the delay, and on a synth MOD DEPTH (its LFO1 → pitch cable) only where the preset already has that cable ([§11.2](11-sound-controls.md#112-mapping-per-engine)); turning it on a preset without one makes a new cable, which no control moves until the next piece. Pressing F7 again gives Thin / Drive: the upper knob turns Thin's lead param and the lower Drive's (as the CLIP faders do), with the stock popup and knob rings (which stay on Thin and Drive when stock refreshes them, F15 in [§19](19-firmware-changes.md#19-firmware-changes)), and the F7 LED blinks while the set is on. F7 pressed a third time, or any other F button, goes back to stock pages, and focusing another part starts it on stock pages. A sound swap on the part keeps Thin / Drive: the new Output comes up on F7, with its rings and blinking LED. Plain F7 stays stutter / CUSTOM 1, and the upper knob's push stays stutter on either set. Under an overlay Perform opens (the leave menu, the sound editor), the gold knobs and F buttons stay Perform's (F16 in [§19](19-firmware-changes.md#19-firmware-changes)): the macros on F8, Thin / Drive with its rings and blinking LED, and every F press as above, so turning F8 never reaches stock's song-level decimation. Knob pushes stay stock.
- **Focus:** holding a part's row on the SONG page puts it on the knobs while held, like holding a clip. Double-tap (two presses within 0.35 s) locks it, and double-tapping the locked part unlocks it. AFFECT ENTIRE unlocks, and its LED is lit at Recipe level. A Kit part focuses its whole Kit (its kit-level pages). Focus is performance state: a stop or leaving Perform drops it, so a new entry starts with nothing focused. An overlay opening over a held row lets go of the hold (a lock stays). The SONG LED stays lit and CLIP dark while a part is focused. The focused part's pad in the now column is white, which hides its flash.

**Buttons.**

| Control | In Perform |
|---|---|
| PLAY / SHIFT+PLAY | Start with the Intro, or stop / start without the Intro. X+PLAY is blocked |
| RECORD | Capture mark (LED on = marked, blinks when the log is full). SHIFT+RECORD stays RESAMPLE |
| SYNC-SCALING / SHIFT+SYNC-SCALING | Fill while held, whatever the community setting: the genre's held-fill entry / a fresh pick ([§8.7](08-rhythm-and-expression.md#87-fill)) |
| TEMPO turn / SHIFT+turn | Recipe tempo / swing, gliding (needs the tempo hook, F4). TAP taps the Recipe tempo. SHIFT+TAP = metronome |
| SCALE | key and scale editor (below). The LED blinks while a key change is queued |
| SYNTH or KIT | focused part: new sound at the phrase line. SHIFT = back to the previous sound |
| CROSS-SCREEN | focused part: freeze (LED = frozen) |
| Y turn / Y push | focused part: take back / forward from the queued take, or else the one playing (forward past the newest makes a new take) / show key, scale and seed code as a popup (`A MINOR 1A2B3C4`) |
| SHIFT + main pad | sound editor at the printed param: the focused part's Sound, or song FX with nothing focused ([§13.7](#137-sound-editor-midi-and-external-tempo)). Not on the KEYBOARD page's FILL pads or the CLIP page's NEW SOUND and SOUND EDITOR, whose SHIFT means something already. Pads, PLAY and knobs stay live. BACK closes |
| BACK tap / hold | cancel the newest queued action / open the leave menu ([§13.8](#138-leaving)) |
| SAVE / SHIFT+SAVE | quick save while playing, full save UI when stopped / always the full save UI ([§16.4](16-recipe-files.md#164-save)) |
| LOAD | Recipe load browser ([§16.5](16-recipe-files.md#165-load)) |
| MIDI, CV | reserved for later outputs |
| TRIPLETS | unused. LEARN stays stock |

**Key and scale editor.** A stock submenu, KEY / SCALE, with two rows, KEY (C to B) and SCALE (the 12 scales, in the short names QUEUE uses), each with its value right-aligned and edited in the stock list. SCALE opens it, and so does KEY / SCALE in the Perform menu.

- It starts from the key ahead: the queued change's key, or else the key that plays. Each turn queues the key and scale it shows to the next phrase line, or an Arc Drop if one lands first ([§13.4](#134-live-actions)), replacing a change already queued. Turning back to the key that plays cancels the queued change.
- While stopped, each turn sets the Recipe's key and scale at once, as a macro turn does, and the next start plays them.
- The SCALE LED blinks from the step decision that takes the change in to the one that lands it, 12 ticks before it is heard ([§12.2](12-runtime.md#122-when-steps-are-decided)).

**Queued actions.** A queued action blinks on its pad and shows in the status line, and QUEUE lists every one. Pressing the action again cancels it, and so does a BACK tap, newest first. Quantised actions close 12 ticks early ([§12.2](12-runtime.md#122-when-steps-are-decided)). A Queued transition shows as `CHG` in the status line and as CHANGE DROP, CHANGE FADE or CHANGE CUT in QUEUE, counting down to where its Build or Outro starts, or to its cut line; it lands on the Section row of the SONG page. While its preload runs it isn't queued yet: the working animation shows it, and a BACK tap cancels it first ([§12.9](12-runtime.md#129-instances-and-the-transition)).

**Pad redraws.** A full pad redraw costs 21–22 ms of UART, shared with the OLED. Redraw only changed rows. Once per 16th uses about 19% of the link at 130 BPM, and the ceiling is about 45 full redraws a second.

### 13.6 Perform menu

Stock look, opened by SELECT push (SHIFT+SELECT stays stock). Rows with a value show it right-aligned, as the Compose screen's do ([§14.4](14-entry-and-compose-screen.md#144-the-compose-screen)):

- CANCEL CHANGE (shown only while a genre change, Regenerate or LOAD can still be cancelled: preloading, or queued before its line)
- GENRE ▸: the genres, grouped as in [§14.1](14-entry-and-compose-screen.md#141-the-generative-menu), the current one labelled `= REGEN` and picking it a Regenerate. Picking another genre is a genre change ([§9.7](09-arc.md#97-transitions)). In M1 the rows are the Techno subgenres
- REGENERATE
- KEY / SCALE ▸: the key and scale editor ([§13.5](#135-control-layout))
- GLIDE: Instant, 1 beat, 1 bar, 2 bars, 4 bars or 1 phrase ([§13.3](#133-perform-settings)), at once
- SEED + TAKES ▸: titled with the seed code (`SEED 1A2B3C4`), one row per part with its take (1–16), the queued one or else the one playing. A turn steps from the take it shows as a Y turn does ([§13.5](#135-control-layout)): forward past the newest makes a new take, and back onto the take that plays cancels the queued one. NEW SEED is the Compose screen's only: a new seed mid-piece would also move the Arc's plan *(implementation choice in #114, for the user to confirm)*.
- CAPTURE

Under the menu the playback and the gold knobs stay Perform's (F16), with their rings and blinking F7 and F8 LEDs. A pad press closes it. SHIFT + a main pad opens the sound editor there instead, as from Perform ([§13.7](#137-sound-editor-midi-and-external-tempo)).

Genre, Regenerate and a mid-set LOAD confirm with a choice: BUILD INTO A DROP, FADE THROUGH or HARD CUT AT PHRASE ([§9.7](09-arc.md#97-transitions)), in a stock context menu titled REGENERATE? for a Regenerate. The incoming genre's transition entry is highlighted. A choice closes the menu back to the Perform page, which shows the working animation while the incoming roster loads; one refused (Committed or Retiring, [§12.9](12-runtime.md#129-instances-and-the-transition)) shows CHANGE PENDING. CANCEL CHANGE also closes the menu.

### 13.7 Sound editor, MIDI and external tempo

**Sound editor overlay.** A part's normal sound editor opens inside Perform while it keeps playing. It needs F6 and F7.

- SHIFT + a main pad opens it at the printed param: on the focused part's Sound (a Kit part's kit-level FX, as on the gold knobs), or on the song FX with nothing focused. SOUND EDITOR on the CLIP page opens the shown part's root menu. The pads whose SHIFT already means something keep it: the KEYBOARD page's FILL pads (a fresh pick), and the CLIP page's NEW SOUND (the previous sound) and SOUND EDITOR.
- Pads, PLAY and knobs stay live with no grey-out. Every pad press reaches Perform: holding a part's row focuses it as it does without the editor, and SHIFT + a main pad moves the editor to that param, on whatever is focused then. The gold knobs stay where focus puts them (F16), so they can be on another part than the editor.
- Knob moves and edits in the editor set homes and are logged ([§11.6](11-sound-controls.md#116-home-and-player-edits-perform)). MIDI feedback goes out only for what the gold knobs point at, as below.
- Perform stays the root UI and never becomes a ClipMinder (F6): SAVE preset, KEYBOARD, CLIP (automation) and AFFECT ENTIRE do nothing in the editor, and PLAY is Perform's. SONG closes the editor back to the SONG page, and BACK closes it.
- A sound swap or a new roster that replaces the part closes the editor, back to the Perform page, before the old Output is freed. Under a screen the editor opened (OSC 1's sample browser, say), the old Output waits until that screen closes back to the editor, which then closes. SOUND EDITOR does nothing while the CLIP page shows no part.
- *(Implementation choices in #116, for the user to confirm: the FILL, NEW SOUND and SOUND EDITOR exceptions, SONG showing the SONG page, and SHIFT + a pad under the editor following focus.)*

**MIDI in.**

- **MIDI-follow CCs** go where the gold knobs point: the focused part's Sound, or song-level params with nothing focused (F8). Only the follow channels A, B and C are followed: the per-track channels (Track 1–16) are off in Perform, so no part can be played, muted or soloed past the Generator. With nothing focused, a CC mapped to the song's tempo does nothing (no tempo CC, below).
- **External notes are dropped**, with their pitch bend, mod wheel, aftertouch and MPE, whether from MIDI follow or an Output's learned input (F18). They would be missing from the steering log and would bypass the note-off table and mute gate. A pitch bend or CC learned to a param still edits it.
- **MIDI-learned and MIDI-follow param edits** move homes like a knob turn and are logged. An incoming CC is read as a live value: home = incoming − current layer offset. LEARN stays stock, so a param learned inside Perform lives on that part's Output and is lost when the Output is replaced. MIDI follow is the route that lasts.
- **Macros over MIDI.** CCs 114–117 on the MIDI-follow channels steer Energy, Density, Complexity and Motion. 0–127 scales to 0–50. They set the Recipe centre, are logged, and get feedback. They win over a follow mapping of the same CC. While a KEYBOARD fader holds the macro, only the centre moves, as for a gold knob. With MIDI follow's DISPLAY PARAM on, they show the macro's popup.
- **Feedback** sends each followed param's live value (home plus all layers) and each macro's centre on the follow feedback channel. While playing, a pass at the stock automation-feedback rate sends each value that moved since it was last sent. "Disabled" means no passes, so only changes go out: a player edit of a param on what the gold knobs point at, as stock sends one, and a macro centre moved on the panel. A new context (focus, entry) sends every value again. A follow or macro CC's value isn't sent back on that CC; an edit through a learned CC goes out on its follow CC, which isn't a loop. Feedback goes out only on the follow channels A, B and C: set to a Track, it has no channel in Perform, which has no tracks. Stock re-sends only `isAutomated()` params, so F8 sends Perform's feedback from `View::sendMidiFollowFeedback` instead.

**Global MIDI commands.**

| Command | In Perform |
|---|---|
| PLAY, FILL, SHIFT | as the panel button. PLAY starts with the Intro |
| RECORD | Capture mark (a no-op until Capture) |
| TAP | Recipe tempo, as below |
| TRANSPOSE | queues a Recipe key change to the next phrase line: the played note's pitch class becomes the key, the scale is kept. Stopped, it sets the key at once, as the key editor does. Note-offs do nothing |
| PLAYBACK_RESTART | blocked |
| LOOP, LOOP_CONTINUOUS_LAYERING, UNDO, REDO, NEXT_SONG | ignored. Perform has no undo |

While a Generative job's modal screen (entry, leave) is up, every global command's note-on is swallowed, so none reaches the song the job replaces.

**Tempo.**

- TAP sets the Recipe tempo through the tempo hook and glides at the Glide setting.
- No tempo CC. MIDI clock out stays stock and follows the gliding song tempo tick by tick.
- **Under an external clock** the master owns tempo and the glide is off. The Recipe tempo tracks the clock: sampled once per bar, rounded to a whole BPM, logged only when the rounded value changes. A tempo turn shows the stock tempo popup marked EXT and changes nothing. TAP is ignored. SHIFT+turn still steers swing, which still glides. X+turn nudges sync as stock. LEARN+turn is swallowed (F4), so the clock-out scale can't change inside Perform.
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
- If nothing is kept, the player lands on a new blank song in session view, the song SHIFT+LOAD → NEW makes. Leaving also empties the sample cache, the previous song's samples included ([§20](20-budgets.md)).

Sources: [Perform interaction model][t15], [Perform control layout][t38], [Entering Generative mode and the Compose screen][t39], [Compose screen and Generative menu prototype][t50], [External tempo and MIDI in Perform][t51], [Generator randomness and seeding][t17], [Deluge UI conventions and free controls][t33], [Measure memory and load times on a Deluge][t37], [Recipe files][t40], [Capture][t35], [Firmware changes the design needs][t43], [Interface between the Generator core and its drivers][t75].

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
[t75]: https://github.com/gismya/deluge-generative/issues/75
