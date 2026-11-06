# 17. Capture

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 17.1 What it is

**Capture** keeps **pieces** played in Perform as regular Deluge songs. It replays a **steering log** through the Compose driver. It never records notes. The log is written from the tick, where each action lands ([§2.6](02-architecture.md#26-steering-into-the-core), ADR 0005).

This rests on the contract in [§6.3](06-randomness.md#63-the-contract). A harness spec checks it directly: a Perform run with a steering log must produce the same event stream as a replay of that log ([§21.2](21-testing.md#212-harness)). A determinism bug would make the song differ from what was heard.

### 17.2 The steering log

- Perform logs every steering action at the step where it **lands**, from inside that step's decision ([§2.6](02-architecture.md#26-steering-into-the-core)). A quantised action is logged at its line, not at the press. A cancelled action is never logged. Replay applies each entry at its step through the same path, with no quantisation.
- **Core entries:** Recipe edits (macros, facts, key, tempo tracked from an external clock, rounded to hundredths of a BPM when logged), Arc forces and holds, End, pattern and sound re-rolls, take steps, freezes, mutes and solos, and Fill (and whether SHIFT made it a fresh pick). A Fill toggled by MIDI FILL and the per-bar external-clock tempo are queued by the driver from the tick.
- **Driver entries**, which the core passes through:
  - **Param edits**, as `{output, param, value}`, from knobs, faders, the sound editor and MIDI. An edit to a param a sound control targets moves its home. Any other becomes plain automation in the Capture song: song params (song FX F1–F7 among them) on the song, sound params on the part's clip.
  - **Glides**, as `{target, length in ticks}` when a tempo or swing glide starts, not the Glide setting.
- A **sound re-roll** logs the new pick's path, because picks are rolled from fresh entropy ([§6.4](06-randomness.md#64-what-the-seed-covers)).
- **Transitions** log a **confirm** entry in the outgoing piece and a **start** entry at the start step, where the outgoing piece ends and the incoming one begins ([§12.9](12-runtime.md#129-instances-and-the-transition)). The transition's Build or Outro is logged as a force landing at its commit step, so a replay of the outgoing piece plays it without the preload.
- **Piece header.** Each piece's log opens with the Recipe (facts, macros, picks as paths, seed, takes) and a snapshot of every param value on every Output: song params, each Synth's params and each kit row's params, about 300 B per Sound and 5–6 KB for the largest roster. Capture applies the snapshot on top of the loaded presets, so edits made before the piece started (a Start after a stop keeps the Outputs) carry over.
- Continuous values coalesce in the steering queue, so the log holds at most one value per control per step.
- The log is one fixed **128 KB buffer in SDRAM**, allocated on Perform entry ([§20](20-budgets.md#20-budgets-and-measured-facts)). That holds about 10 marked pieces with busy steering. A discarded piece's header and entries are freed with its log.
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
- **Mutes.** What a muted part generated while muted is left out, as rests, or as no clip for an occurrence muted throughout: the Compose driver drops note-ons with the muted flag ([§12.5](12-runtime.md#125-mute-gate-and-note-offs)). Capture records what was heard.
- **Param edits** that aren't homes become automation on the song or the part's clip ([§17.2](#172-the-steering-log)).
- **Not captured in v1:** sound-editor edits that aren't params, such as oscillator type, synth mode or filter mode. The song loads each preset as picked, with the param snapshot and param edits on top. Cloning the live Outputs instead would work only for the last piece.

### 17.7 Building on leave

- The leave menu ([§13.8](13-perform.md#138-leaving)) offers the marked pieces plus the last piece, marked or not, so forgetting to press costs nothing.
- On a keep choice the pieces are built one at a time in set order, as modal runner jobs queued behind the teardown of Perform's song ([§12.10](12-runtime.md#1210-long-running-work-and-input)). Each is replayed privately the way Compose builds, swapped in stopped, and **saved to the card** through the normal save path for the current song. This breaks Compose's "nothing goes to the card until the player saves" rule on purpose: saving a song that isn't current is unverified.
- The OLED shows the COMPOSING layout titled `BUILDING 2 OF 3` or `SAVING 2 OF 3`, with **BACK = STOP** under the bar.
- **BACK stops Capture at once**, without asking. The song being built is thrown away, a card save that has started finishes, and songs already saved stay.
- The player lands on the last saved song in arranger view, or a blank session if none was saved.
- **Name:** Compose's name plus ` LIVE` (`GEN TECHNO 1A2B3C4 LIVE`), within the OLED name limit. Name clashes on the card use the firmware's normal rule.
- Facts: a stored `Note` is 24 bytes, and notes allocate internal RAM first, then SDRAM.

Sources: [Capture][t35], [Compose screen and Generative menu prototype][t50], [Entering Generative mode and the Compose screen][t39], [Recipe files][t40], [External tempo and MIDI in Perform][t51], [Interface between the Generator core and its drivers][t75], [Determinism details: tick unit, tempo, held notes and ramps][t77].

[t35]: https://github.com/gismya/deluge-generative/issues/35
[t39]: https://github.com/gismya/deluge-generative/issues/39
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t50]: https://github.com/gismya/deluge-generative/issues/50
[t51]: https://github.com/gismya/deluge-generative/issues/51
[t75]: https://github.com/gismya/deluge-generative/issues/75
[t77]: https://github.com/gismya/deluge-generative/issues/77
