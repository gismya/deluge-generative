# 22. Build order and milestones

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 22.1 Starting point

- Work starts fresh on `generative`. The POC branches (`poc/core`, `poc/perform-techno`) are not merged. They are a reference for ideas and scaffolding: the `generator.h` seam, the harness CMake skeleton and the delugemu script. Several of their choices break decisions (the POC layers onto the player's song, never sets LEGATO or CHOKE, divides in the driver, and gives Techno House's clap). Delete the POC branches once v1's first Perform slice plays.
- **First slice:** a **walking skeleton** through every layer: Perform playing **Techno** on a Deluge, with drums and bass, the Arc cycling and the four macros on the knobs. It lands with harness specs from day one. After that, each layer (chords, interplay, sound controls, Ornaments and so on) is its own slice that ends in sound.

### 22.2 Order

- **Genres:** Techno → Ambient → Psytrance → Dub → House. Techno fixes the one pre-v1 golden. Ambient comes second because it is the far end of the model (Pad, Chords, Sustain, a slow Arc), so it finds gaps early.
- **Modes:** Perform (one genre) → Compose and the Compose screen → Recipe files → Capture. Capture needs the Compose driver, and the firmware-matches-harness check needs Compose.

### 22.3 Milestones

| Milestone | Delivers | Release |
|---|---|---|
| M0 Housekeeping | delete the workflow files, scripts and configs ([§21.1](21-testing.md#211-ci)); add the `AGENTS.md` rules ([§21.1](21-testing.md#211-ci), [§21.4](21-testing.md#214-when-agents-run-checks)); add the harness target with an empty spec suite; add the beta release script | none |
| M1 Perform, Techno | every Generator layer, the Perform control layout, sound controls, firmware changes F1–F11 and F14, Techno's share of the pack. F9–F11 land before the beta | Beta 1 |
| M2 All genres + Generative pack | Ambient, Psytrance, Dub and House in that order, with their palettes and pack content | Beta 2 |
| M3 Compose + Recipe files + Generative menu | Compose driver, Compose screen, the Generative menu and source lists, Recipe files, the firmware-matches-harness check | Beta 3 |
| M4 Capture | steering log, marking, the leave menu builds | v1 |

- The Generative menu ([§14.1](14-entry-and-compose-screen.md#141-the-generative-menu)) is needed in M1 to enter Perform. M1 builds PERFORM with GENRE ▸ only; CURRENT RECIPE, FILE... and COMPOSE arrive in M3 (§23.9).
- Every beta ships the pack zip as it stands.

### 22.4 Beta builds

Built locally and published by a script in the repo: `./dbt build release` plus the pack zip, then `gh release create --prerelease`. No new CI workflow. Revisit a `workflow_dispatch` workflow only if testers ever need nightlies.

### 22.5 Upstream fixes

No upstream PRs are part of the plan. The `sound_editor.cpp:2161` null read is fixed in the fork with F6. `storage_manager.cpp:406` is left alone. Both are recorded in [`docs/upstream-candidates.md`](../../upstream-candidates.md).

### 22.6 Handoff

This spec is split into implementation tickets: one parent issue per milestone (M0–M4), with `ready-for-agent` slices under it, each citing spec sections and wired with blocking edges. The slice order inside M1 starts from the walking skeleton.

Sources: [Build order][t44], [Testing and CI][t41], [Firmware changes the design needs][t43], POC findings on the [map][poc].

[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t41]: https://github.com/gismya/deluge-generative/issues/41
[t43]: https://github.com/gismya/deluge-generative/issues/43
[t44]: https://github.com/gismya/deluge-generative/issues/44
