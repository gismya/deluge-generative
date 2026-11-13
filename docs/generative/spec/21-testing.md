# 21. Testing and CI

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 21.1 CI

- Only `tests.yml` runs, on every PR (milestone branches too): x86 unit tests, ARMv7 under QEMU user mode and the `generative-rules` hooks, about 2.5 min, each job with a timeout. `test-results.yml` publishes its results.
- Every other workflow file is deleted, not disabled: `backport-from-issue`, `backport`, `beta-build`, `build`, `nightly-build`, `pr-build`, `release-build`, `publish`, `clang-tidy`, `format-check`, `deploy-website`, `label-issues`, `label-prs` and `sync-labels`, plus the `.github/scripts` and labeler config files only they use. autofix.ci has no file in the repo and stays. Upstream merge conflicts on deleted files are solved as they come.
- No branch protection. `AGENTS.md` gets a rule: don't merge a PR while **Run tests** is red.
- Hardware is never in CI.

### 21.2 Harness

- The Generator's tests are another target in the `tests/` CMake tree. CI runs them in the x86 job only, not `tests/qemu`. Locally they build natively on the Mac with dbt's cmake and Apple clang. The target builds with `-fsigned-char` like the firmware, and CI runs it with `-fsanitize=signed-integer-overflow` (ADR 0004). It also defines `GENERATOR_TEST_SEAMS`, which the firmware never does, for harness-only hooks in the core: one plays a flavour the core has no code for yet, so a spec can fill the largest roster before every flavour exists.
- **Specs** hold invariants and equivalences. The invariants come from the first slice. Each equivalence comes with its driver: Perform against Compose in M3, Capture replay in M4:
  - every pitched note is in scale and in its Register;
  - every note-on gets a note-off;
  - voice starts per step, reported against the 4-per-render budget rather than failed, since the firmware defers extra starts (§23.14);
  - interplay rules only target earlier parts and earlier Drums lanes ([§5.5](05-parts-and-lanes.md#55-interplay-rules));
  - every Motion curve table matches its formula, R and start ([§3.5](03-recipe.md#35-motion-curves));
  - every Register span is at least 12 semitones wide, and an Arp part's span holds its widest pool entry's octave range ([§7.3](07-pitch.md#73-register));
  - stepping back to a take within one pick replays that take's line as it unfolded before, under the same macros ([§6.6](06-randomness.md#66-takes)), and an Arp's notes don't shift when Density thins it ([§8.1](08-rhythm-and-expression.md#81-onsets-and-density));
  - an untouched Perform driver run matches a Compose run ([§6.8](06-randomness.md#68-perform-and-compose-agree));
  - a Capture replay matches the live steered run ([§17.1](17-capture.md#171-what-it-is));
  - a note held across a Section line gets a note-off on the line and is re-struck by late entry ([§12.5](12-runtime.md#125-mute-gate-and-note-offs));
  - no step overflows the event buffer or the sounding-note table ([§2.7](02-architecture.md#27-buffer-sizes-and-overflow)), and a Bass or Lead note overlaps only the next onset's ([§8.5](08-rhythm-and-expression.md#85-lean), [§8.6](08-rhythm-and-expression.md#86-slide)).

  Equivalence checks compare two live runs, so they store no file. They run the portable Perform and Compose drivers with recording sinks ([§2.5](02-architecture.md#25-portable-drivers-and-firmware-adapters)). The Capture check runs the portable Perform driver with a steering script, takes the log it wrote, and replays that log through the portable Compose driver.
- **Goldens** catch change: Recipe + seed + takes + steering script → a text dump. Before v1 there is **one golden per Techno subgenre**: its default Recipe at a fixed seed, short. The fuller set (a default Recipe per genre for a full Arc cycle, plus steering cases such as macro sweeps, force and hold, Fill, take re-roll and step back, Regenerate, genre change, key change and freeze) comes after v1, or earlier when a bug escapes.
- **Retuning on purpose:** one local command regenerates the goldens (such as `--update-goldens`). The new dumps are committed with the retune, which also bumps the genre version, and the commit says which genre was retuned. Review is the text diff.
- **Rough render:** an offline, deterministic render of a Recipe to a WAV, a Standard MIDI File with one track per part, and a text piano roll. It uses our own small voice code (one subtractive voice plus a sample player for kit rows) and reads samples from `pack/` and a local card-shaped folder. Parameters it doesn't model are ignored with a warning. Renders are made locally and never committed.

### 21.3 delugemu (local only)

- Use [SynthstromAudible/delugemu](https://github.com/SynthstromAudible/delugemu). It runs headless, scripted and deterministic in icount mode (`-icount shift=2,sleep=off -S` with `run_until`). The factory card is a prebuilt `mksd.sh` image at `~/source/delugemu-sd`.
- A script in the repo runs **smoke checks** against a local `./dbt build release`: boot; Perform starts and plays N bars (non-silent, no freeze); Compose builds a song, saves it and reloads it; Capture writes its songs; a Recipe file round-trips. The checks are functional only, with no audio hashes.
- **Firmware matches the harness:** render the golden case through Compose in delugemu, read the saved song's notes back into the same text dump, and diff it against the harness golden. This is the only end-to-end proof that the firmware plays what the harness tested.
- **Pinning:** the repo records the delugemu commit the checks last ran with. The script warns when the installed bundle's `VERSION` differs, and bumping the pin is a deliberate commit.
- Gotchas: one boot in three can come up blank, so the script needs a boot check and a retry. Under icount the virtual clock runs faster than real time, so waits count virtual time. pylib's `press` and `load_song` use wall-clock sleeps, so icount scripts use `run_until` plus `button()`. Check timing in ticks or MIDI, not audio.

- **Null guard (test builds):** a build with the sysex test port (`ENABLE_SYSEX_TEST`, in delugemu and on the Deluge)
  makes the first MB of the address space a translation fault at boot (`guardNullPage()` in `src/resetprg.c`). A read
  or write through a null pointer then data-aborts at once. Without it the Deluge reads whatever its empty CS0 bus floats
  to, so a null dereference crashes only sometimes, and delugemu never. To see where a fault happened in delugemu, run
  `test_port_emu.py --gdb PORT`, attach `arm-none-eabi-gdb` with `target remote :PORT` and `hbreak handle_cpu_fault`:
  r0 and r2 at the break are the faulting PC and the LR. Add `--gdb-wait` for a fault during boot: the CPU waits at
  reset until gdb continues it.

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

Sources: [Testing and CI][t41], [Simulation approach][t8], [Time a scripted capture in delugemu][t36], [Build and run v1.3.0-rc3 in delugemu][t9], [delugemu on v1.3.0-rc3][t4], [Generator randomness and seeding][t17], POC findings on the [map][poc], [Interface between the Generator core and its drivers][t75], [Determinism details: tick unit, tempo, held notes and ramps][t77].

[t4]: https://github.com/gismya/deluge-generative/issues/4
[t8]: https://github.com/gismya/deluge-generative/issues/8
[t9]: https://github.com/gismya/deluge-generative/issues/9
[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t17]: https://github.com/gismya/deluge-generative/issues/17
[t36]: https://github.com/gismya/deluge-generative/issues/36
[t41]: https://github.com/gismya/deluge-generative/issues/41
[t75]: https://github.com/gismya/deluge-generative/issues/75
[t77]: https://github.com/gismya/deluge-generative/issues/77
