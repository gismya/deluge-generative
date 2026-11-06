# 16. Recipe files

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 16.1 Contents

A **Recipe file** holds the Current Recipe and nothing more:

- the genre's stable name and its genre version;
- tempo as an integer in hundredths of a BPM, and swing (the glide targets if a glide is running), key and scale;
- the four macros;
- every sound pick as a card path;
- the seed and every part's take.

Freezes, mutes, sound-control homes, Arc position, Compose LENGTH and the Glide setting stay out. A Recipe file holds no steering, so it doesn't replay a piece. A Capture song does that.

### 16.2 Genre versions

- On a version mismatch the file still loads and the OLED shows a short notice (`OLDER TECHNO`). Saving again writes the current version. Old genre tables are not kept.
- A file naming a genre this firmware doesn't have is refused with `UNKNOWN GENRE`.
- A file fails to load only if it is unreadable or its genre is unknown.

### 16.3 Format, folder and names

- XML through the stock serializer, in a top-level `RECIPES/` folder, created on first save. A folder of its own keeps Recipe files out of the song browser.
- A never-saved Recipe gets the default name `<GENRE> <seed code>` (`TECHNO 1A2B3C4`). If that name is taken, the stock letter variation applies.
- Every save suggests the next letter variation of the remembered name (`SONG001` → `SONG001A`, stock). Overwriting needs an explicit pick and a confirm, like a stock song save.
- After any save, the Current Recipe takes the saved name.

### 16.4 SAVE

- **In Perform while playing**, SAVE is a **quick save**: it saves at once under the suggested name, a popup shows the name, and the grid stays put. A quick save never overwrites. If no letter variation is free, it refuses with `NO FREE NAME`.
- **In Perform while stopped**, and **on the Compose screen**, SAVE opens the full save UI: QWERTY on the pads, then the browser on `RECIPES/`.
- SHIFT+SAVE in Perform always opens the full save UI.
- During a transition, SAVE writes the playing Recipe, not the incoming one, until the new roster starts ([§12.9](12-runtime.md#129-instances-and-the-transition)).
- Recipe files are written only by SAVE. Capture saves songs only.
- **A failed save** (card full, a write error, the card pulled) shows the stock error popup and changes nothing else. The Current Recipe is still in RAM. A quick save in Perform keeps the music playing. There's no retry prompt.

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
