# 16. Recipe files

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 16.1 Contents

A **Recipe file** holds the Current Recipe and nothing more:

- the genre's stable name and its genre version;
- tempo as an integer in hundredths of a BPM, and swing (the glide targets if a glide is running), key and scale;
- the four macros;
- each pitched part's octave shift ([§7.3](07-pitch.md#73-register)); a file without it loads as 0;
- every sound pick as a card path;
- the seed and every part's take.

Freezes, mutes, sound-control homes, Arc position, Compose LENGTH and the Glide setting stay out. A Recipe file holds no steering, so it doesn't replay a piece. A Capture song does that.

No drum-kit tag is stored: a Kit part's lane picks are card paths, so they already hold the machine the piece plays ([§10.2](10-sounds.md#102-drums-assembled-per-lane)).

### 16.2 Genre versions

- On a version mismatch the file still loads and the OLED shows a short notice (`OLDER ACID TECHNO`). Saving again writes the current version. Old genre tables are not kept.
- A file naming a genre this firmware doesn't have is refused with `UNKNOWN GENRE`.
  - Acid techno is `techno-acid`. `techno` is Techno's parent table, which no Recipe can name (§4.4), so a file that names `techno` is refused with `UNKNOWN GENRE`; no alias maps it to Acid techno. `psytrance` is likewise Psytrance's parent table, and Classic psytrance is `psytrance-classic` (#319). Palette override files go by the OLED name (§10.1): Acid techno's is `GENERATIVE/PALETTES/ACID TECHNO.XML`. Nothing else the firmware writes names a genre yet.
- A file fails to load only if it is unreadable or its genre is unknown.

### 16.3 Format, folder and names

- XML through the stock serializer, in a top-level `RECIPES/` folder, created on first save (or when the full save UI first opens, since its browser lists the folder). A folder of its own keeps Recipe files out of the song browser.
- The root tag `generativeRecipe` carries `genre` (the stable name), `genreVersion`, `tempo` (hundredths of a BPM), `swing`, `swingInterval`, `key` (pitch class, 0 = C), `scale` (a stable name: `minor`, `hungarianMinor`) and `seed`. Its children are `macros` (the four in full-resolution units, 64 a point), a `part` per part (its `name` from the genre table, its `take`, its `octave` shift, and `sound` when it has a pick) and a `lane` per lane with a pick (`name` as palette override files name lanes, and `sound`). Card paths are entity-encoded.
- Reading: a value the file leaves out takes the genre's default (macros, swing) or the Recipe's; one out of range is clamped (an octave shift to the ones its part takes, [§7.3](07-pitch.md#73-register)); parts, lanes, elements and attributes the firmware doesn't know are skipped, so a newer file still loads. A file that isn't a Recipe file, is cut short, or holds a value that isn't one (a number that isn't a number, an unknown scale) is unreadable.
- A never-saved Recipe gets the default name `<GENRE> <seed code>` (`ACID TECHNO 1A2B3C4`). If that name is taken, the stock letter variation applies: the first free one, `ACID TECHNO 1A2B3C4A`. The letters always go after the seed code, even one that ends in a letter.
- Every save suggests the next letter variation of the remembered name (`SONG001` → `SONG001A`, stock): the first free letter after the one it has, never the name itself. Overwriting needs an explicit pick and a confirm, like a stock song save.
- After any save, the Current Recipe takes the saved name. Saved as suggested, its letters go on from the same stem (`…A` → `…B`); a name the player typed is a stem of its own (`NIGHT DRIVE` → `NIGHT DRIVEA`). A typed name longer than 96 characters is saved but not remembered.

### 16.4 SAVE

- **In Perform while playing**, SAVE is a **quick save**: it saves at once under the suggested name, a popup shows the name, and the grid stays put. A quick save never overwrites. If no letter variation is free, it refuses with `NO FREE NAME`.
- **In Perform while stopped**, and **on the Compose screen** (or a menu under it), SAVE opens the full save UI: QWERTY on the pads, then the browser on `RECIPES/`, on the suggested name. SAVE there saves, a popup shows the name, and the UI closes; BACK in `RECIPES/` leaves with nothing saved. Over Perform, Perform plays on underneath and the sidebar stays its own, as under the preset browser. Stock's long-press options (collect media, create folder) aren't offered. If no letter variation is free, the UI offers the remembered name, or the default name when none is remembered, and saving under it asks to overwrite. Under that confirm the sidebar still shows Perform's, but its presses go to the confirm, as under any stock context menu.
- SHIFT+SAVE in Perform always opens the full save UI.
- During a transition, SAVE writes the playing Recipe, not the incoming one, until the new roster starts ([§12.9](12-runtime.md#129-instances-and-the-transition)).
- Recipe files are written only by SAVE. Capture saves songs only.
- While the runner is busy, or a card access is under way, SAVE is refused with BUSY ([§12.10](12-runtime.md#1210-long-running-work-and-input)), in the full save UI too. SAVE lists `RECIPES/` and writes its file from the button handler, as stock's save UIs do, not as a runner job: a file of a few KB and one folder listing take a few card reads, and a runner job would hold the save behind a preload.
- A save writes the Current Recipe as it was at the press: a handover or a steering press during the card write doesn't reach the file, and if the Recipe was replaced meanwhile, the new one doesn't take the saved name.
- **A failed save** (card full, a write error, the card pulled) shows the stock error popup and changes nothing else; a file it had just created is removed again. The Current Recipe is still in RAM. A quick save in Perform keeps the music playing. There's no retry prompt.

### 16.5 LOAD

- LOAD opens a stock-style load browser on `RECIPES/`, the same as FILE... in the source lists. The QWERTY appears on entry and hides once the player scrolls, and the root UI's pads show again. BACK leaves with nothing changed.
- **In Perform while playing**, the music keeps playing in the browser. Choosing a file is queued like a genre change ([§9.7](09-arc.md#97-transitions)), with the same confirm: BUILD INTO A DROP, FADE THROUGH or HARD CUT AT PHRASE, the file genre's transition entry highlighted. The file starts at a Drop, at its Intro or on the cut line, on fresh Outputs, as a new piece, and the change is cancellable until its Build or Outro starts. It uses the file's seed, takes, facts and macros. To hear it from the top, the player stops and starts again.
- **In Perform while stopped**, it rebuilds directly, like entering Perform.
- **On the Compose screen**, it replaces the Current Recipe, ready for BUILD.
- No unsaved-changes prompt anywhere. Loading a file, picking a genre and Regenerate replace the Current Recipe without asking.

### 16.6 Missing sounds and sharing

- On load each sound pick's path is checked. A missing one is re-rolled from this card's palette for that part or lane, from fresh entropy, and the OLED shows `2 SOUNDS REPLACED`. The Current Recipe keeps the replacements. The notes don't change, because the seed doesn't cover sounds.
- A path that still exists but points at a different sample isn't detected. That's accepted.
- A file isn't self-contained. It plays the same for another player with the same firmware, factory card and Generative pack.

Sources: [Recipe files][t40], [Entering Generative mode and the Compose screen][t39], [Generator randomness and seeding][t17], [Determinism details: tick unit, tempo, held notes and ramps][t77], [Compose and Capture memory, limits and failures][t78].

[t17]: https://github.com/gismya/deluge-generative/issues/17
[t39]: https://github.com/gismya/deluge-generative/issues/39
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t77]: https://github.com/gismya/deluge-generative/issues/77

[t78]: https://github.com/gismya/deluge-generative/issues/78
