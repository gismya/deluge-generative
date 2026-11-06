# Generative mode spec

This is the consolidated v1 design for **Generative mode** on this fork of the Deluge community firmware. It folds in every decision ticket on the [wayfinder map][map], with each amendment applied, so an implementer shouldn't need the ticket history. Each section ends with the tickets it comes from. Read those for the reasoning, not for the rules. Where a ticket and this spec disagree, this spec wins, and [§23](spec/23-decisions-while-writing.md#23-decisions-made-while-writing-this-spec) lists every place where writing it settled something.

The spec is split into one file per section under [`spec/`](spec/), so read only what your task needs. Start with this page, which has the reading rules (§0, below), what each file covers, and where to start for common kinds of work. Section numbers are shared across all files, and every `§` reference links to its file.

## Files

| § | File | Covers | Read it when |
|---|---|---|---|
| 1 | [Scope](spec/01-scope.md) | what v1 is, everything ruled out | checking whether something is in v1 |
| 2 | [Architecture](spec/02-architecture.md) | the Generator core, drivers, event stream, output seam, code layout | starting any implementation work |
| 3 | [The Recipe](spec/03-recipe.md) | Recipe contents, musical facts, the four macros and what each moves, steering grain | touching anything a macro or Recipe value drives |
| 4 | [Genres](spec/04-genres.md) | genre data model, the ten slots, versions, the five v1 genre briefs | writing or tuning genre tables |
| 5 | [Parts, roles, flavours and lanes](spec/05-parts-and-lanes.md) | roster, roles, flavours, Outputs, lanes, interplay rules, gates, Replace pairs, voice modes | building a part or a flavour |
| 6 | [Randomness and seeding](spec/06-randomness.md) | keyed hash draws, the event-stream contract, seed, takes, freeze | any code that makes a random choice |
| 7 | [Pitch](spec/07-pitch.md) | scale steps, scales, Register, Chord graph, guard, colour moves, Degree sets, key changes | pitched parts and harmony |
| 8 | [Rhythm and expression](spec/08-rhythm-and-expression.md) | Density thinning, Ornaments, the Build roll, velocity and Velocity spread, Lean, slide, Fill, Drop-out | onsets, timing feel and velocity |
| 9 | [The Arc](spec/09-arc.md) | Section kinds, the cycle, lengths, Energy and Density offsets, section rules, force and hold, transitions | song form and genre changes |
| 10 | [Sounds](spec/10-sounds.md) | palettes, sound picks, kit assembly, tags, FX lanes, load-time settings, when sounds load | loading or picking sounds |
| 11 | [Sound controls and automation](spec/11-sound-controls.md) | the six controls, per-engine mapping, macro, gesture and drift layers, homes, ramps | moving sound params live or in Compose |
| 12 | [Generator runtime](spec/12-runtime.md) | the tick hook, 12-tick lookahead, driver clock, external clock, mute gate, note-offs, voice starts, glides | the Perform driver and anything in the audio routine |
| 13 | [Perform](spec/13-perform.md) | Perform's song, transport, live actions, the full control layout, menu, MIDI, leaving | Perform UI and input |
| 14 | [Generative menu, entry and the Compose screen](spec/14-entry-and-compose-screen.md) | SHIFT+SONG menu, source lists, the Current Recipe, DISCARD SONG?, the Compose screen, building | entering the modes and the Compose UI |
| 15 | [Compose output shape](spec/15-compose-output.md) | what Compose writes: clips, session sections, arranger, notes, automation, the private-song build | the Compose driver |
| 16 | [Recipe files](spec/16-recipe-files.md) | file contents, versions, names, SAVE, LOAD, missing sounds | saving and loading Recipes |
| 17 | [Capture](spec/17-capture.md) | the steering log, pieces, marking, song shape, mid-piece changes, building on leave | Capture |
| 18 | [Generative pack](spec/18-generative-pack.md) | card layout, pack contents, built-in palettes | adding sounds or palette data |
| 19 | [Firmware changes](spec/19-firmware-changes.md) | F1–F13: every change to existing firmware code, with `file:line` | touching code outside the Generator |
| 20 | [Budgets and measured facts](spec/20-budgets.md) | CPU, memory, load times, redraw cost, measured on hardware | sizing anything or checking a cost |
| 21 | [Testing and CI](spec/21-testing.md) | CI, harness specs and goldens, delugemu checks, when agents run what, the tuning loop | writing tests or setting up checks |
| 22 | [Build order and milestones](spec/22-build-order.md) | the starting point, genre and mode order, M0–M4, beta builds, handoff | planning or splitting work |
| 23 | [Decisions made while writing this spec](spec/23-decisions-while-writing.md) | the 14 conflicts between tickets and how the spec settled them | a ticket seems to disagree with the spec |
| 24 | [Source tickets](spec/24-source-tickets.md) | every decision ticket on the map, by type | looking for a decision's reasoning |

## Where to start

- **Any Generator slice:** [§2](spec/02-architecture.md#2-architecture), [§6](spec/06-randomness.md#6-randomness-and-seeding), then the files for the layer. Check [§21.2](spec/21-testing.md#212-harness) for the specs it must add.
- **A new flavour or part behaviour:** [§5](spec/05-parts-and-lanes.md#5-parts-roles-flavours-and-lanes), [§7](spec/07-pitch.md#7-pitch) or [§8](spec/08-rhythm-and-expression.md#8-rhythm-and-expression) for the mechanism, [§4](spec/04-genres.md#4-genres) for the genre data it reads, [§9.5](spec/09-arc.md#95-section-rules) if a section rule drives it.
- **The Perform driver or tick hook:** [§12](spec/12-runtime.md#12-generator-runtime), [§19](spec/19-firmware-changes.md#19-firmware-changes) (F1, F2), [§20](spec/20-budgets.md#20-budgets-and-measured-facts).
- **Perform controls and screens:** [§13](spec/13-perform.md#13-perform), then [§19](spec/19-firmware-changes.md#19-firmware-changes) for the firmware cases each control needs.
- **Compose or Capture:** [§14](spec/14-entry-and-compose-screen.md#14-generative-menu-entry-and-the-compose-screen), [§15](spec/15-compose-output.md#15-compose-output-shape), [§17](spec/17-capture.md#17-capture), and [§6.8](spec/06-randomness.md#68-perform-and-compose-agree) for the equivalence they must keep.
- **Sounds and palettes:** [§10](spec/10-sounds.md#10-sounds), [§18](spec/18-generative-pack.md#18-generative-pack), [§11](spec/11-sound-controls.md#11-sound-controls-and-automation) for what moves live.
- **Genre tables and tuning:** [§4](spec/04-genres.md#4-genres), [§9.5](spec/09-arc.md#95-section-rules), [§21.5](spec/21-testing.md#215-genre-tuning-loop), and ADR 0001 on what may be copied from PERF-1.
- **Planning and the ticket split:** [§22](spec/22-build-order.md#22-build-order-and-milestones), [§19](spec/19-firmware-changes.md#19-firmware-changes), [§21](spec/21-testing.md#21-testing-and-ci).

## 0. How to read this spec

- **Base.** Upstream community firmware tag `v1.3.0`, on this fork's `generative` branch. OLED hardware only. Firmware paths are relative to `src/deluge/` and are checked against `v1.3.0` unless a line says rc3. Older tickets cite `v1.3.0-rc3`, and `src/` differs between the two (22 commits, 50 files), so check any rc3 line number before using it.
- **Words.** The terms in bold are defined in [`GLOSSARY.md`](../../GLOSSARY.md), and this spec uses them exactly. A part is never a "track", a Section is never a "scene", and the Deluge's own numbered clip groups are **session sections**.
- **ADRs.** Four records in [`docs/adr/`](../adr/) hold decisions whose reasoning matters for later changes:
  - [ADR 0001 Porting policy](../adr/0001-porting-policy.md): reimplement by default.
  - [ADR 0002 Perform plays notes through a Generator hook](../adr/0002-perform-plays-through-a-generator-hook.md), not through clips.
  - [ADR 0003 The Generator decides each step 12 ticks ahead](../adr/0003-generator-decides-steps-12-ticks-ahead.md), so Lean can push.
  - [ADR 0004 Generator decisions use integer math only](../adr/0004-generator-decisions-use-integer-math-only.md).
- **Tuning data.** Many values here are marked *tuning*. They are starting data, tuned by ear later ([§21.5](spec/21-testing.md#215-genre-tuning-loop)). An implementation slice puts them in genre or Generator-wide tables with the stated starting value. It doesn't need to get them right first time.
- **Fixed rules.** Anything not marked *tuning* is a rule. Changing it means amending this spec.
- **Section numbers** are stable. Implementation tickets cite them as `spec §9.4`.

[map]: https://github.com/gismya/deluge-generative/issues/1
