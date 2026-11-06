# 17. Capture

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 17.1 What it is

**Capture** keeps **pieces** played in Perform as regular Deluge songs. It replays a **steering log** through the Compose driver. It never records notes, and nothing is recorded from the audio routine.

This rests on the contract in [§6.3](06-randomness.md#63-the-contract). A harness spec checks it directly: a Perform run with a steering log must produce the same event stream as a replay of that log ([§21.2](21-testing.md#212-harness)). A determinism bug would make the song differ from what was heard.

### 17.2 The steering log

- Perform logs every steering action at its step position: Recipe edits (macros, facts, key, tempo tracked from an external clock), Arc forces and holds, End, pattern and sound re-rolls, take steps, freezes, mutes and solos, Fill (and whether SHIFT made it a fresh pick), and home changes from knobs, faders and MIDI.
- Knob moves are merged to at most one value per control per step.
- The log is one fixed, preallocated buffer, sized at implementation. A few hours of busy steering is tens of KB.
- When it fills, new marks are refused with an OLED message, and the current piece stops logging at the last step that fit. The leave menu then says its span was cut there.

### 17.3 Pieces

- A **piece** runs from a Start, Regenerate, genre change or mid-set LOAD until the transport stops or the next Regenerate, genre change or LOAD.
- If End was pressed, the piece includes the Outro up to where it finishes. A plain stop cuts the piece at the stop step. A Start after a stop begins a new piece.
- At a transition, the old piece ends on the last step before the new roster starts, its Build or Outro included, and the new piece starts there ([§9.7](09-arc.md#97-transitions)). A captured piece that began on a Drop opens on a Groove with the Drop lift, one that began at a seam opens on its Intro, and one that began on a hard cut opens on a Groove. Every piece has one set of Outputs.
- When a piece ends, its log is thrown away unless the piece is marked.

### 17.4 Marking

- RECORD (or MIDI RECORD) marks the current piece to keep as a whole. The log keeps running until the piece ends, so music after the press is included.
- Pressing again unmarks it. The status line dot and the RECORD LED show whether the current piece is marked.
- Several pieces can be marked in one set, and their logs share the buffer.
- Marks and logs live in RAM only. A power-off or crash before leaving loses them.

### 17.5 Song shape

- The Compose shape ([§15](15-compose-output.md#15-compose-output-shape)): one clip per part per Section occurrence, each occurrence in its own session section, arranger in Arc order.
- Occurrences past the 23rd become **arrangement-only clips** (`section = 255`, in `Song::arrangementOnlyClips`), because a piece can't be ended early. The arranger has no clip or instance cap apart from RAM.
- A Capture of an untouched Perform comes out identical to a Compose of the same length.
- One song per piece. Pieces are never merged, because they differ in genre, key and swing.

### 17.6 Changes partway through a piece

The song gets the piece's first tempo, swing, key and scale, which are what a Compose of the starting Recipe would write.

- **Tempo** changes and glides become song-level tempo automation (`params::UNPATCHED_TEMPO` in the song's `paramManager`). It plays back only in arrangement mode.
- **Swing** can't be automated (`Song::swingAmount`). Notes played at another swing get the difference built into their tick position, the way Lean is.
- **Key and scale** are song-wide only. A clip with any note outside the song's scale is written with scale mode off, because a scale-mode clip with an outside note widens the whole song's scale. A key change partway through a Section doesn't split the clip; that clip is just chromatic.
- **Sound re-rolls** start a **new track** from the re-roll step, and that occurrence's clip is cut in two there, so every track has one sound for its whole life.
- **Mutes.** What a muted part generated while muted is left out, as rests, or as no clip for an occurrence muted throughout. Capture records what was heard.

### 17.7 Building on leave

- The leave menu ([§13.8](13-perform.md#138-leaving)) offers the marked pieces plus the last piece, marked or not, so forgetting to press costs nothing.
- On a keep choice the pieces are built one at a time in set order. Each is replayed privately the way Compose builds, swapped in stopped, and **saved to the card** through the normal save path for the current song. This breaks Compose's "nothing goes to the card until the player saves" rule on purpose: saving a song that isn't current is unverified.
- The OLED shows the COMPOSING layout titled `BUILDING 2 OF 3` or `SAVING 2 OF 3`, with **BACK = STOP** under the bar.
- **BACK stops Capture at once**, without asking. The song being built is thrown away, a card save that has started finishes, and songs already saved stay.
- The player lands on the last saved song in arranger view, or a blank session if none was saved.
- **Name:** Compose's name plus ` LIVE` (`GEN TECHNO 1A2B3C4 LIVE`), within the OLED name limit. Name clashes on the card use the firmware's normal rule.
- Facts: a stored `Note` is 24 bytes, and notes allocate internal RAM first, then SDRAM.

Sources: [Capture][t35], [Compose screen and Generative menu prototype][t50], [Entering Generative mode and the Compose screen][t39], [Recipe files][t40], [External tempo and MIDI in Perform][t51].

[t35]: https://github.com/gismya/deluge-generative/issues/35
[t39]: https://github.com/gismya/deluge-generative/issues/39
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t50]: https://github.com/gismya/deluge-generative/issues/50
[t51]: https://github.com/gismya/deluge-generative/issues/51
