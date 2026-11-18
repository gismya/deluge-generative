# Generative mode: implementation notes

What building Generative mode has taught, by area: gotchas, firmware traps and the extension points left for later
tickets. The [spec](spec.md) says what to build; before working on code, read the files here for the code you touch.
Paths are under `src/deluge/` unless they say otherwise. When you learn something the next agent would trip on, add
it to its area's file; when a note stops being true, fix or delete it. Keep each file under about 200 lines: past
that, split it by topic and list the parts here.

- [Portable code: build, steering, capacity, genre data](implementation-notes/core.md): `generative/core/`,
  `perform/`, `runner/` as the harness builds them; steering commands, capacity, events, sound controls, genre
  data checks, subgenres and their tables.
- [Portable code: chords, fills, sounding notes, flavours](implementation-notes/core-music.md): the Chord-graph
  walk, fills, the sounding-note pools, the Sustain, Stab, Arp and Motif flavours, cut lines, portable text.
- [Compose driver](implementation-notes/compose.md): `generative/compose/`, the `SongWriter` seam, where End goes,
  the node thinning, the Perform-matches-Compose spec.
- [Timing](implementation-notes/timing.md): the tick, step lookahead, tempo, the driver's clock.
- [Runner and jobs](implementation-notes/runner.md): the Runner's jobs, transitions, the start step's cost,
  the step's code section (`GEN_STEP_CODE`, `.text.generative_step`) and H1.
- [Firmware adapter and the private song](implementation-notes/firmware.md): `generative/firmware/`, Outputs,
  clips, sounds and their settings.
- [Perform UI](implementation-notes/perform-ui.md): `gui/views/perform_view.*`, pads, pages, menus.
- [Extension points left open](implementation-notes/extension-points.md): seams later tickets extend.
- [Build and tools](implementation-notes/build-and-tools.md): the harness, goldens, probe scripts, test-port holds,
  delugemu, hardware runs, the theory review.
