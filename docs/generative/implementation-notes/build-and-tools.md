# Implementation notes: Build and tools

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- `set_source_files_properties` in `src/deluge/CMakeLists.txt` needs `TARGET_DIRECTORY deluge` to reach the target.
- `./dbt sizediff` noise: under LTO a different git SHA in `version.h` alone moves `handle_cpu_fault`,
  `fault_handler_print_freeze_pointers` and an `OrderedResizeableArrayWith32bitKey` constructor (about +20 B text).
  Don't chase it.
- CI builds the harness with GCC, which rejects designated initializers out of declaration order
  (`PartSpec{.reg = ..., .densityGate = ...}` with the fields swapped); local clang accepts them, so the error shows
  only in CI. Write designators in the struct's order. To check before pushing, compile the changed spec files with
  the firmware toolchain's `arm-none-eabi-g++ -std=gnu++23 -fsyntax-only` (after `./dbt test --generator`, for the
  test deps' includes); on ARM `int32_t` is `long`, so ignore errors on lines the branch didn't change.
- Harness specs: one `describe` per `*_spec.cpp`, named after the file stem, and the file ends with
  `CPPSPEC_SPEC(<the same name>)`: a different name fails the link with an undefined `<name>_spec`. Put
  `// clang-format off` above the `describe`, as every spec does: clang-format mangles CppSpec's nested lambdas, and
  the commit hook's reformat then lands in the next `--amend`. Don't name a local `it` (CppSpec's macro). Specs reach
  `pack/` and fixtures through `GENERATOR_REPO_DIR`.
- Stepping a piece in a spec: use `StepwisePiece` (`tests/spec/generator/harness.h`), not a hand-wired driver or a
  bare `Generator::decideNextStep`. `decideBefore(step)` stops at step - 1's decision tick, so a push then reaches
  `step`. `runToStepLine(step)` stops on `step`'s grid line, so a push then reaches step + 1. `runToStepLine` counts
  from the last call: after a jump, or once a transition's incoming instance plays, run in song ticks (`runToTick`).
  `decideBefore` and `pushAndDecide` count steps on the instance that plays when they are called, and return false
  if it stopped deciding first (the piece ended or stopped, or a transition's incoming instance took over). For a
  transition use `TransitionPiece` (`transition_piece.h`): it runs the real lifecycle, so `confirm()` is a request
  and a preload that ends well.
  Shared stream helpers are in `stream_helpers.h`. A helper there shares its name with every spec that includes it:
  delete the spec's own copy, or the call is ambiguous.
- Goldens (spec §21.2): `tests/spec/generator/goldens/<genre>.txt`, one per Techno subgenre, written and
  checked by `golden_spec` (`golden.h`). Each is the default Recipe at seed 1, 16 bars from SHIFT+PLAY, one event per
  line, under a header with the genre's version and its stream fingerprint (60 pieces): the per-subgenre fingerprint
  specs live there. A retune that's meant follows the procedure in AGENTS.md and spec §21.2 (nothing checks the
  commit message). The command refuses (exit 1) a golden that changed without a bump against the merge base with the
  PR's base (`scripts/generative/goldens.py`, the same base as `check_all.py`; CI checks it too), so one bump covers a
  branch's retunes. It runs whenever the update wrote a golden, even if a spec failed. A golden the base doesn't have
  counts from the commit that added it, so a golden a branch adds is still checked against its first version. CI's
  depth-2 checkout can't find that commit (its shallow oldest commit looks as if it added every file), so there such a
  golden is new and goes unchecked. A golden without a `format` line is format 1. A change to the dump itself, its
  seed, length or fingerprint size bumps `kFormat` in `golden.h` instead, and needs no version bump.
  - `golden_spec` pins no subgenre's table: its format specs use a made-up genre and Recipe, and its changed-stream
    spec computes what it expects, so a retune passes after the update alone (`--update-goldens` runs only
    `golden_spec`). The folder check runs after the goldens' own specs, so an update that adds a subgenre passes the
    first time. An update never writes a dump that lost events.
  - A retune that moves a fact spec §4.5 states (tempo range, scales, roster) also fails the subgenre's identity spec
    (`acid_techno_spec` and the like), which mirrors §4.5: edit both in the retune's commit.
  - A `kFormat` bump doesn't excuse a changed `fingerprint` line whose parenthesized text stayed the same: the
    fingerprint doesn't depend on the dump. A change to how `stream_fingerprint.h` hashes or picks its pieces edits
    that text in `goldenText()` along with the `kFormat` bump.
  - A retune of Acid's drums, acid line, Arc or FX also fails `acid_fingerprint_spec`'s first check, which pins
    Techno's stream from before Techno split into subgenres: delete that check in the retune's commit. A retune of its facts
    (tempo, scales) or of the Drone alone leaves it passing: the fingerprint's pieces roll their own macros, scales
    and keys, and the check leaves the Drone out. Specs don't pin a genre's version; the golden does.
  - `stream_fingerprint.h` starts from 1469598103934665603, a digit short of FNV-1a's offset basis
    (14695981039346656037). Harmless, but fixing it changes every fingerprint: do it with `--update-goldens`, a
    `kFormat` bump and a new text in the fingerprint line's parentheses, and re-measure `acid_fingerprint_spec`'s split pin on the same commit.
  - CppSpec keeps the `const char*` an `it` is given: a title built at run time (one per genre) must outlive the
    spec, as `kGoldenTitles` does. A name in a spec's namespace that matches its `describe` (`golden`) or CppSpec's
    own (`describe`) is ambiguous.
- Probe scripts: start every script with `start_state`, then Perform scripts with `include perform_open.txt` and
  `include perform_confirm.txt`. An expect's timeout needs the keyword, last on the line: `expect event gen_left
  within=15`. A bare trailing number is part of the match text (`expect event gen_section INTRO 0` matches section
  0). Over delugemu's DIN link test-port events can arrive seconds late: give `expect event` a long timeout before an
  `expect no_event`. SHIFT+SONG right after reaching session view needs about 1 s (the zoom animation holds a UI
  mode).
- A test-port hold (`hold NAME`, `release NAME`) is a row of `kHolds` in `io/midi/test_port.cpp` plus the hook that
  reads it. Nothing else needs a new hold added (the probe's `HOLDS_BEFORE_RELEASE_ALL` is frozen, for older builds): `start_state` sends `release all`, which sets every row off, so a row's
  `set(false)` must be safe when the hold was never on.
- `hold preload` parks only the first preload job that starts after it: `TestPort::waitWhilePreloadHeld()` turns the
  hold off when that job's wait ends, by a release or a cancel. A confirm that cancels a held sound preload therefore
  lets its transition's preload run straight through (`perform_transition.txt`). Each `expect event` searches after
  the previous expect's match, so two expects can't both match one event: put every field you check in one expect (a
  `/regex/` when they aren't adjacent).
- `start_state` ends with `reset recipe` (`resetRecipe()` in `io/midi/test_port.cpp`), which zero-fills the Current
  Recipe as boot does (`forgetCurrentRecipe()`): with `exists` false, GENRE ▸ opens on the default genre and entry
  rolls a fresh Recipe. It resets only the Current Recipe: other Generative state a script leaves outside Perform
  outlives `start_state`, so a probe script that depends on more than the genre after boot must set it itself.
- Test events from `PerformAdapter::tick()` come when the Generator decides a step; those from `OutputSink::play()`
  (`gen_section`, `gen_key`, notes) come when the step plays, a little later. So `gen_phrase bar=8` (decided) arrives
  before bar 8's `gen_section` (played): an `expect no_event gen_section ...` right after it sees that line's Section.
