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
  - every ordered pair of built-in genres changes in each of the three transition styles: every outgoing note ends, the incoming roster's notes are in its scale and Registers, nothing overflows, the tempo glide arrives where the new roster starts and swing switches on its start step. Each case plays only until the new roster's first phrase ends, so the whole matrix adds under 15 s to the harness run; past that, each genre changes in and out against two neighbours instead of all;
  - no step overflows the event buffer or the sounding-note table ([§2.7](02-architecture.md#27-buffer-sizes-and-overflow)), and a Bass or Lead note overlaps only the next onset's ([§8.5](08-rhythm-and-expression.md#85-lean), [§8.6](08-rhythm-and-expression.md#86-slide)).

  Equivalence checks compare two live runs, so they store no file. They run the portable Perform and Compose drivers with recording sinks ([§2.5](02-architecture.md#25-portable-drivers-and-firmware-adapters)). The Capture check runs the portable Perform driver with a steering script, takes the log it wrote, and replays that log through the portable Compose driver.
- **Goldens** catch change: Recipe + seed + takes + steering script → a text dump. Before v1 there is **one golden per signed-off genre**: its default Recipe at a fixed seed, short. Each Techno subgenre has one. An M2 genre (Ambient, Psytrance, Dub, House) gets its golden and its `kPinned` steering fingerprint when the player signs it off on the Deluge ([#133](https://github.com/gismya/deluge-generative/issues/133)), not when its slice lands, so tuning it needs no version bumps. Until then nothing pins its stream, and a change to shared code can move it unseen: that is accepted while it is tuned. The fuller set (a default Recipe per genre for a full Arc cycle, plus steering cases such as macro sweeps, force and hold, Fill, take re-roll and step back, Regenerate, genre change, key change and freeze) comes after v1, or earlier when a bug escapes.
- **Retuning on purpose:** one local command regenerates the goldens, committed with the retune (**A retune** below). Review is the text diff.
  - **The goldens.** `tests/spec/generator/goldens/<genre>.txt`, one per signed-off genre, checked by `golden_spec`: the default Recipe (the genre's new Recipe from entropy 0) at seed 1 for 16 bars from SHIFT+PLAY, so the Groove at its default macros; one event per line (tick, bar.beat.16th, part, then the lane or note name and what it does, Control events included). Its header holds the genre's version and its stream fingerprint over 60 pieces with random macros, scales, keys and seeds, two thirds of them with an Intro: the golden pins the whole stream, the dump shows the default Recipe as text. **A retune:** in the genre's table (`src/deluge/generative/core/genres/`, `techno.cpp` for a Techno subgenre), raise its `genre.version = N;` line (after `subgenreOf(kTechnoParent)` in a subgenre) to N + 1, run `./dbt test --generator --update-goldens`, copy the steered fingerprint `steering_fingerprint_spec` prints for the genre into that spec's `kPinned` (below), commit the table, the goldens and `kPinned` together, and name the genre in the commit (`Retune techno-acid (version 2): …`). Nothing checks the commit message: the command only prints the retuned genres for it. The rule is `scripts/generative/goldens.py`: a golden whose stream changed needs its genre's version above the one at the merge base with the branch's base (the open PR's base when `gh` knows it, else the branch's upstream on origin when that's another branch, else `generative`: `scripts/generative/pr_base.py`), fetched from origin first so a stale local branch or an old fetch never sets the merge base, so one bump covers every retune of that genre on a branch. A version below the base's fails whether or not the golden changed: versions only go up. A golden the base hasn't yet counts from the commit that added it. The command, `check_all.py` and CI (the generative-rules job, against the PR's base) all check the rule, the command whenever it wrote a golden. CI's checkout is shallow and can't find the commit that added a golden, so there a golden the PR's base hasn't is new and unchecked; the command and `check_all.py` check it. An update never writes a dump that lost events. A golden whose `format` line went up (a change to the dump itself, its seed or its length; none is format 1) needs no bump, unless its `fingerprint` line changed while the text in its parentheses didn't: the fingerprint doesn't depend on the dump, so that is a retune. A new format is refused while the genre tables (the `.cpp` and `.h` files in `src/deluge/generative/core/genres/`) differ from the base: a retune the fingerprint doesn't see would pass as the new format, so the format bump lands on its own. A golden that is gone since the base is reported, and `golden_spec` checks that every signed-off genre has one. `golden_spec` pins no subgenre's table, so a retune needs no edit to it. `steering_fingerprint_spec` pins each subgenre's stream under steering (mutes, solos, held fills, a force, End), which the goldens play without: a retune fails it too, and its failure prints the new fingerprint, which the retune copies into the spec's `kPinned` (the command reminds it). A retune that moves a fact §4.5 states (tempo range, scales, roster) also edits §4.5 and the subgenre's identity spec, which mirrors it.
- **Rough render:** an offline, deterministic render of a Recipe to a WAV, a Standard MIDI File with one track per part, and a text piano roll. It uses our own small voice code (one subtractive voice plus a sample player for kit rows) and reads samples from `pack/` and a local card-shaped folder. Parameters it doesn't model are ignored with a warning. Renders are made locally and never committed. The MIDI file's last track, THEORY, holds as text what the notes don't show: the Recipe, each part's and lane's Density gate and interplay rules, and the key, chord and effective macros as they change. For the theory review (§21.5) the render can play one Arc cycle (`--end-after-drop`: End armed on the Drop, so the Outro follows its phrase) and write the MIDI file alone (`--midi-only`).

### 21.3 delugemu (local only)

- Use [SynthstromAudible/delugemu](https://github.com/SynthstromAudible/delugemu). It runs headless, scripted and deterministic in icount mode (`-icount shift=2,sleep=off -S` with `run_until`). The factory card is a prebuilt `mksd.sh` image at `~/source/delugemu-sd`.
- A probe script that needs another card names an overlay folder in its `# needs: card fixtures/<script>` line (`perform_sounds_override.txt`, `perform_sounds_fallback.txt`). `test_port_emu.py` reads that line and puts the folder on each boot's clone of the factory card: the steps in its `card.txt` (`remove PATH`, `copy FROM -> TO` from the repo or the card), then its own files. `fat_image.py` writes the FAT32 image directly, with no mount, sudo or `._*` files.
- A script in the repo runs **smoke checks** against a local `./dbt build release`: boot; Perform starts and plays N bars (non-silent, no freeze); Compose builds a song, saves it and reloads it; Capture writes its songs; a Recipe file round-trips. The checks are functional only, with no audio hashes.
- **Firmware matches the harness:** render the golden case through Compose in delugemu, read the saved song's notes back into the same text dump, and diff it against the harness golden. This is the only end-to-end proof that the firmware plays what the harness tested.
- **Genre changes:** each genre has one probe script that changes into it and back out in all three transition styles in one boot: an M2 genre from the M2 genre before it (Ambient from Acid techno), and Psytrance into Dub for the long tempo glide.
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

These rules are in `AGENTS.md`:

- Harness tests and specs on every Generator change.
- delugemu smoke checks and the firmware check before pushing any change under `src/` that touches Generative mode.
- Hardware runs (the `deluge-hardware-test` skill, over the sysex test port) in the test plan for changes to UI, timing, memory or CPU.

### 21.5 Genre tuning loop

1. **Harness rough render**: rhythm, harmony and interplay in stand-in sounds, with no firmware build.
   - **Theory review**, in each genre's slice: a script (`scripts/generative/theory_review.py`) summarises `generator_render`'s MIDI for the genre's default Recipe over a full Arc cycle at a few entropies (onsets by 16th position per lane and part, pitch classes against the scale, chords per bar, each part's range and its overlap with the others, notes per bar per Section, notes against each Density gate, each interplay rule's effect, velocity ranges). A fresh agent reads it against §4.5's brief and the genre's conventions. A clear fault is fixed in the slice; a judgement call becomes its own ticket for the player. It never blocks a merge.
2. **delugemu capture with the real sounds**, the main loop: a script takes genre + Recipe overrides + seed + length, then builds, captures and plays the WAV. A 60 s capture takes about 18 s on an M4 Pro, and the same seed before and after gives a fair A/B.
3. **The real Deluge**: sign-off, and how steering feels.

A tweak that changes a genre's event stream fails its golden ([§21.2](#212-harness)). Once it sounds right, retune it as [§21.2](#212-harness) says: one version bump, new goldens, the genre named in the commit.

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
