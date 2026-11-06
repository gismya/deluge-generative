# 14. Generative menu, entry and the Compose screen

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 14.1 The Generative menu

- **SHIFT+SONG in session view** opens a stock context menu titled GENERATIVE over the session view, with **PERFORM** and **COMPOSE**. The view underneath keeps drawing and the song keeps playing. BACK closes it with nothing changed.
- Arranger view keeps its stock SHIFT+SONG, so from arranger the player presses SONG first.
- Picking one opens a full-screen stock submenu, **PERFORM FROM** or **COMPOSE FROM**, with three rows:
  - **CURRENT RECIPE**, only once one exists;
  - **GENRE** ▸, a submenu of the five genres that opens on the Current Recipe's genre. Picking one rolls a fresh Recipe: rolled facts, rolled sound picks, a new seed, takes at 0 and the genre's default macros;
  - **FILE...**, the Recipe load browser ([§16.5](16-recipe-files.md#165-load)).
- The fonts have no "…" or "▸": write FILE..., and use the stock submenu arrow icon.

### 14.2 The Current Recipe

- Generative mode holds one **Current Recipe** (Recipe, seed and takes) in RAM until power-off. Perform steers it and the Compose screen edits it, so both modes start where the player left off. Rolls that were never built carry over too.
- Picking a genre or a file replaces it, without asking.
- It remembers the name of the file it was loaded from or last saved as ([§16.3](16-recipe-files.md#163-format-folder-and-names)). Picking a genre or Regenerate clears that name.

### 14.3 Replacing the song

- The firmware has no unsaved-changes prompt: it doesn't track edits, and LOAD replaces the song without asking.
- Generative mode asks every time before replacing the song, with a stock one-option context menu **DISCARD SONG?** → OK, modelled on "Clear song?". BACK backs out with nothing changed.
- It is asked after the source pick for PERFORM, and on BUILD in Compose.

**Perform entry.** After DISCARD SONG?, Perform's song (Outputs and sound loads) is built privately as a modal runner job ([§12.10](12-runtime.md#1210-long-running-work-and-input)) while the current song keeps playing, with the stock working animation (the 17×9 icon, top right). BACK cancels the build and leaves the old song untouched. Perform's song then swaps in stopped on the SONG page, with STOPPED on the status line. The swap ends playback under an external clock too ([§15.7](15-compose-output.md#157-build-and-handover)). Perform then waits for the master's Start or Continue, and the status line shows STOPPED and EXT. If a sound can't load because the card errors or was pulled, the build stops with the stock card error and the old song is untouched.

### 14.4 The Compose screen

A stock-style menu that edits the Current Recipe before building. The Recipe stays fixed for the whole song. There's no audition: hearing a Recipe means playing it in Perform.

- Three rows on screen, each row's value right-aligned. Pushing a value row edits it in place: the value blinks, SELECT turns it, push or BACK finishes. Rows with ▸ open a submenu.
- **Title**: genre, tempo, key and scale, such as `PSYTRANCE 146 A# HUNGARIAN MINOR`, in the bold title font. It side-scrolls when wider than 128 px, like a long song name in the stock session view.

| Row | Value | Behaviour |
|---|---|---|
| FROM | genre ▸ | the source list of [§14.1](#141-the-generative-menu) |
| REGENERATE | | asks REGENERATE? → OK |
| SEED + TAKES | ▸ | submenu titled `SEED 0Y8438B`: NEW SEED, then one row per part with its take (number editor) |
| TEMPO | BPM | stock big-number editor |
| SWING | amount | stock big-number editor |
| KEY | root | |
| SCALE | name | short name (HARM MIN, HUNG MIN) when the full one doesn't fit |
| ENERGY, DENSITY, COMPLEXITY, MOTION | 0–50 | stock continuous number editor (number and bar) |
| SOUNDS | n PARTS ▸ | one row per part with its pick (`BASS 017 ACID LINE`). Push re-rolls from the palette, SHIFT+push goes back to the previous pick. Drums and FX rows (`DRUMS: 8 LANES` ▸) open a lane list that works the same way |
| LENGTH | m:ss | target length, 1:00–20:00 in 30 s steps, default 4:00, stock big-number editor. Kept until power-off, never saved |
| BUILD | | [§14.5](#145-building) |

- Sound-control homes, freeze, mutes, and Arc force and hold are left out. They are steering or performance state, not Recipe values. After BUILD the result is a normal song, shaped in the stock views.
- SAVE saves the Current Recipe (full save UI) and LOAD is the same as FILE... ([§16](16-recipe-files.md#16-recipe-files)).

### 14.5 Building

1. BUILD → DISCARD SONG? → OK. If the size estimate doesn't fit even alone, BUILD is refused first with the longest length that fits (`TOO LONG: MAX 12:30`, [§15.9](15-compose-output.md#159-memory-and-failures)).
2. The OLED shows **COMPOSING** with a progress bar of bars rendered against the bars expected at the target length plus one Outro phrase, so it may jump at the end. Below it: **BACK = CANCEL**. The current song keeps playing or stays stopped, unless the new song fits only alone: then playback ends and the current song is freed before the build.
3. BACK cancels at once and throws the private song away. Nothing else changes, but if the current song was freed, the player lands on a blank song. Running out of memory (the SDRAM reserve) or a card error ends the build the same way, with a message.
4. The finished song swaps in **stopped** and opens in **arranger view**.

Sources: [Entering Generative mode and the Compose screen][t39], [Compose screen and Generative menu prototype][t50], [Compose output shape][t16], [Recipe files][t40], [Deluge UI conventions and free controls][t33], [Compose and Capture memory, limits and failures][t78].

[t16]: https://github.com/gismya/deluge-generative/issues/16
[t33]: https://github.com/gismya/deluge-generative/issues/33
[t39]: https://github.com/gismya/deluge-generative/issues/39
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t50]: https://github.com/gismya/deluge-generative/issues/50

[t78]: https://github.com/gismya/deluge-generative/issues/78
