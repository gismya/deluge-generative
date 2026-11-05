# 21. Testing and CI

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 21.1 CI

- Only `tests.yml` runs, on PRs into `generative`: x86 unit tests and ARMv7 under QEMU user mode, about 2.5 min. `test-results.yml` publishes its results.
- Every other workflow file is deleted, not disabled: `backport-from-issue`, `backport`, `beta-build`, `build`, `nightly-build`, `pr-build`, `release-build`, `publish`, `clang-tidy`, `format-check`, `deploy-website`, `label-issues`, `label-prs` and `sync-labels`, plus the `.github/scripts` and labeler config files only they use. autofix.ci has no file in the repo and stays. Upstream merge conflicts on deleted files are solved as they come.
- No branch protection. `AGENTS.md` gets a rule: don't merge a PR into `generative` while **Run tests** is red.
- Hardware is never in CI.

### 21.2 Harness

- The Generator's tests are another target in the `tests/` CMake tree. CI runs them in the x86 job only, not `tests/qemu`. Locally they build natively on the Mac with dbt's cmake and Apple clang.
- **Specs** hold invariants and equivalences. The invariants come from the first slice. Each equivalence comes with its driver: Perform against Compose in M3, Capture replay in M4:
  - every pitched note is in scale and in its Register;
  - every note-on gets a note-off;
  - voice starts per step, reported against the 4-per-render budget rather than failed, since the firmware defers extra starts (§23.14);
  - interplay rules only target earlier parts ([§5.5](05-parts-and-lanes.md#55-interplay-rules));
  - an untouched Perform driver run matches a Compose run ([§6.8](06-randomness.md#68-perform-and-compose-agree));
  - a Capture replay matches the live steered run ([§17.1](17-capture.md#171-what-it-is)).

  Equivalence checks compare two live runs, so they store no file.
- **Goldens** catch change: Recipe + seed + takes + steering script → a text dump. Before v1 there is **one golden**: Techno's default Recipe at a fixed seed, short. The fuller set (a default Recipe per genre for a full Arc cycle, plus steering cases such as macro sweeps, force and hold, Fill, take re-roll and step back, Regenerate, genre change, key change and freeze) comes after v1, or earlier when a bug escapes.
- **Retuning on purpose:** one local command regenerates the goldens (such as `--update-goldens`). The new dumps are committed with the retune, which also bumps the genre version, and the commit says which genre was retuned. Review is the text diff.
- **Rough render:** an offline, deterministic render of a Recipe to a WAV, a Standard MIDI File with one track per part, and a text piano roll. It uses our own small voice code (one subtractive voice plus a sample player for kit rows) and reads samples from `pack/` and a local card-shaped folder. Parameters it doesn't model are ignored with a warning. Renders are made locally and never committed.

### 21.3 delugemu (local only)

- Use [SynthstromAudible/delugemu](https://github.com/SynthstromAudible/delugemu). It runs headless, scripted and deterministic in icount mode (`-icount shift=2,sleep=off -S` with `run_until`). The factory card is a prebuilt `mksd.sh` image at `~/source/delugemu-sd`.
- A script in the repo runs **smoke checks** against a local `./dbt build release`: boot; Perform starts and plays N bars (non-silent, no freeze); Compose builds a song, saves it and reloads it; Capture writes its songs; a Recipe file round-trips. The checks are functional only, with no audio hashes.
- **Firmware matches the harness:** render the golden case through Compose in delugemu, read the saved song's notes back into the same text dump, and diff it against the harness golden. This is the only end-to-end proof that the firmware plays what the harness tested.
- **Pinning:** the repo records the delugemu commit the checks last ran with. The script warns when the installed bundle's `VERSION` differs, and bumping the pin is a deliberate commit.
- Gotchas: one boot in three can come up blank, so the script needs a boot check and a retry. Under icount the virtual clock runs faster than real time, so waits count virtual time. pylib's `press` and `load_song` use wall-clock sleeps, so icount scripts use `run_until` plus `button()`. Check timing in ticks or MIDI, not audio.

### 21.4 When agents run checks

These rules go into `AGENTS.md` in M0:

- Harness tests and specs on every Generator change.
- delugemu smoke checks and the firmware check before pushing any change under `src/` that touches Generative mode.
- Hardware runs (the `deluge-hardware-test` skill, over the sysex test port) in the test plan for changes to UI, timing, memory or CPU.

### 21.5 Genre tuning loop

1. **Harness rough render**: rhythm, harmony and interplay in stand-in sounds, with no firmware build.
2. **delugemu capture with the real sounds**, the main loop: a script takes genre + Recipe overrides + seed + length, then builds, captures and plays the WAV. A 60 s capture takes about 18 s on an M4 Pro, and the same seed before and after gives a fair A/B.
3. **The real Deluge**: sign-off, and how steering feels.

Genre tables stay compiled in, so a tweak is an incremental rebuild. There's no dev-only card override.

Sources: [Testing and CI][t41], [Simulation approach][t8], [Time a scripted capture in delugemu][t36], [Build and run v1.3.0-rc3 in delugemu][t9], [delugemu on v1.3.0-rc3][t4], [Generator randomness and seeding][t17], POC findings on the [map][poc].

[t4]: https://github.com/gismya/deluge-generative/issues/4
[t8]: https://github.com/gismya/deluge-generative/issues/8
[t9]: https://github.com/gismya/deluge-generative/issues/9
[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t17]: https://github.com/gismya/deluge-generative/issues/17
[t36]: https://github.com/gismya/deluge-generative/issues/36
[t41]: https://github.com/gismya/deluge-generative/issues/41
