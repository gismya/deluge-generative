# Implementation notes: Build and the harness

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- `set_source_files_properties` in `src/deluge/CMakeLists.txt` needs `TARGET_DIRECTORY deluge` to reach the target.
- `./dbt sizediff` noise: under LTO a different git SHA in `version.h` alone moves `handle_cpu_fault`,
  `fault_handler_print_freeze_pointers` and an `OrderedResizeableArrayWith32bitKey` constructor (about +20 B text).
  Don't chase it. The base also builds under `.cache/sizediff/base/`, so its `__FILE__` strings are longer: #283's
  run showed text −108 B (`.text` −84, `.sdram_rodata` −24) with only those two functions resized. To show a change
  to a file is a no-op, A/B it instead: build HEAD with the base's copy of the file (`git show BASE:PATH > PATH`,
  `./dbt build release`) and diff `arm-none-eabi-size -A` and `arm-none-eabi-nm -S` with HEAD's build.
- A base class only test builds need (`GenreSubmenu` in `gui/menu_item/generative/genre_rows.h`) is an alias of the
  stock class outside `ENABLE_SYSEX_TEST`: even an empty base between `Submenu` and its subclasses changed how LTO laid
  out the default build (−80 B text, other functions resized), which `./dbt sizediff` then shows.
- CI builds the harness with GCC, which rejects designated initializers out of declaration order
  (`PartSpec{.reg = ..., .densityGate = ...}` with the fields swapped); local clang accepts them, so the error shows
  only in CI. Write designators in the struct's order. To check before pushing, compile the changed spec files with
  the firmware toolchain's `arm-none-eabi-g++ -std=gnu++23 -fsyntax-only` (after `./dbt test --generator`, for the
  test deps' includes); on ARM `int32_t` is `long`, so ignore errors on lines the branch didn't change.
- An `int32_t`/`int` mix in the portable code (`std::clamp(x, 0, kMacroFull)` with an `int32_t` x) compiles in the
  harness, where `int32_t` is `int`, and fails the Release build, where it is `long`. CI never builds Release:
  `python3 scripts/generative/arm_types.py` compiles `generative/core/`, `perform/`, `compose/` and `runner/` (each `.cpp`, and
  each `.h` on its own: some headers only the firmware includes) with the firmware's compiler and its
  `-Werror=switch`, syntax only, in about a second (check_all.py's first step). It doesn't cover the spec files,
  which never build on ARM.
- `./dbt sizediff --base NAME`, `check_all.py --base NAME` and `goldens.py check` (without --base) all go through
  `pr_base.merge_base()`: a branch name means `origin/NAME`, fetched first, never the local branch. Each prints
  `base: REF (why)`. Offline it uses the copy fetched last and says so.
- CppSpec is pinned (v1.1.0) to a commit and hash in `tests/spec/CMakeLists.txt` because `tests/CMakeLists.txt`'s
  clang `-Werror` flags reach it. To move it, change the commit and the SHA256 together and run both harness builds.
- One harness spec on its own: `./dbt test --generator -n` builds them, then
  `./dbt exec "ctest -C Debug --test-dir build/tests -R <name>_spec --output-on-failure"`. ctest is only inside
  `./dbt exec`, and the build is multi-config: without `-C Debug` every test shows "Not Run". `-n` prints the binary it
  built; run that one (`<binary> <name>_spec --verbose`). `check_all.py` builds only `build/tests-sanitize`, so after a
  plain `-n` its binary there is stale (`--sanitize -n` rebuilds it): the "`--no-run` didn't rebuild" of #436.
- Harness specs: one `describe` per `*_spec.cpp`, named after the file stem, and the file ends with
  `CPPSPEC_SPEC(<the same name>)`: a different name fails the link with an undefined `<name>_spec`. Put
  `// clang-format off` above the `describe`, as every spec does: clang-format mangles CppSpec's nested lambdas, and
  the commit hook's reformat then lands in the next `--amend`. Don't name a local `it` (CppSpec's macro). Specs reach
  `pack/` and fixtures through `GENERATOR_REPO_DIR`. Group `it`s inside the `describe` with `context(...)`: a nested
  `describe(...)` compiles but its `it`s never run, and the run still passes (#143). The
  `generative-spec-no-nested-describe` pre-commit hook (`scripts/lint/generative_rules.py`) refuses one in any
  `tests/spec/**/*_spec.cpp`.
- Don't edit a harness header while `./dbt test --generator` builds. An object that started compiling before the edit
  and finished after it is newer than the header, so ninja keeps it, with the old layout: the specs then segfault at
  once (`generator_specs transitions_spec` printing nothing). `touch` the header and build again.
- Stepping a piece in a spec: use `StepwisePiece` (`tests/spec/generator/harness.h`), not a hand-wired driver or a bare
  `Generator::decideNextStep`. `decideBefore(step)` stops at step - 1's decision tick, so a push then reaches `step`.
  `runToStepLine(step)` stops on `step`'s grid line, so a push then reaches step + 1. `runToStepLine` counts from the
  last call: after a jump, or once a transition's incoming instance plays, run in song ticks (`runToTick`).
  `decideBefore` and `pushAndDecide` count steps on the instance that plays when they are called, and return false if it
  stopped deciding first (the piece ended or stopped, or a transition's incoming instance took over). For a transition
  use `TransitionPiece` (`transition_piece.h`): it runs the real lifecycle, so `confirm()` is a request and a preload
  that ends well. `TransitionPiece(playing, pending)` takes two Recipes, each at its own genre, tempo, swing and key.
  `genre_change_matrix_spec` changes every ordered pair of `allGenres()` in all three styles with it, well inside spec
  §21.2's 15 s budget: a genre joins it by being in `allGenres()`. Each case runs once and its faults print as `<from>
  -> <to> (<style>): <what>` lines, so a failure names its pairs. Shared stream helpers are in `stream_helpers.h`. A
  helper there shares its name with every spec that includes it: delete the spec's own copy, or the call is ambiguous.
- `./dbt exec "..."` runs its string through `sh`: a file list with newlines in it (`$(git diff --name-only)`) runs
  each path as a command, and an executable one (`scripts/tasks/task-test.py`) really runs. Join the list with spaces
  (`$(git diff --name-only | tr '\n' ' ')`).
- A `git commit` runs pre-commit, which stashes the unstaged files for a few seconds and puts them back: a build
  running in the background meanwhile (a test-port build for delugemu, `check_all.py`) can compile the committed
  version of a file you changed but haven't staged yet. Commit before starting a long build, or stage everything first.
- Spec temp files: name them with `tempPath()` (`tests/spec/generator/temp_path.h`), which adds the pid, so harness
  runs in parallel worktrees never share one; a folder per spec file goes in a `RemovedAtExit`.
- Seeded harness counts are exact: pin them (`drift_spec`'s depths and turns). `--update-goldens` doesn't refresh
  such pins: a retune that moves them re-pins them by hand. A floor that only shows a run reached its
  worst case (`roster_room_spec`, `stab_spec`) says so in a comment, so nobody tightens it into a pin a retune breaks.
- Trap: editing a header while a background `./dbt test --generator` builds can leave an object built from the old
  header with a newer mtime, so the next incremental build skips it. A layout change (a field added to `ControlSet`)
  then shows up as a `__stack_chk_fail` abort or a segfault before a spec prints anything. `touch` the edited headers
  and build again.
- Goldens (spec §21.2): `tests/spec/generator/goldens/<genre>.txt`, one per Techno subgenre, written and checked by
  `golden_spec` (`golden.h`). Each is the default Recipe at seed 1, 16 bars from SHIFT+PLAY, one event per line, under a
  header with the genre's version and its stream fingerprint (60 pieces): the per-subgenre fingerprint specs live there.
  A retune that's meant follows **A retune** in spec §21.2. The command refuses (exit 1) a golden that changed without a
  bump against the merge base with the PR's base (`scripts/generative/goldens.py`, the same base as `check_all.py`; CI
  checks it too), so one bump covers a branch's retunes. It runs whenever the update wrote a golden, even if a spec
  failed. A golden the base doesn't have counts from the commit that added it, so a golden a branch adds is still
  checked against its first version. CI's depth-2 checkout can't find that commit (its shallow oldest commit looks as if
  it added every file), so there such a golden is new and goes unchecked. A golden without a `format` line is format 1.
  A change to the dump itself, its seed, length or fingerprint size bumps `kFormat` in `golden.h` instead, and needs no
  version bump.
  - `golden_spec` pins no subgenre's table: its format specs use a made-up genre and Recipe, and its changed-stream
    spec computes what it expects, so a retune passes after the update alone (`--update-goldens` runs only
    `golden_spec`). The folder check runs after the goldens' own specs, so an update that adds a subgenre passes the
    first time. An update never writes a dump that lost events.
  - A retune that moves a fact spec §4.5 states (tempo range, scales, roster) also fails the subgenre's identity spec
    (`acid_techno_spec` and the like), which mirrors §4.5: edit both in the retune's commit.
  - A version bump of techno-acid fails three delugemu scripts, not any harness spec: the Recipe-load fixtures
    (`scripts/generative/fixtures/recipe_load/RECIPES/LOAD OK`, `MISSING`, `BROKEN`) hold its current version, and
    `compose_recipe_load.txt`, `perform_recipe_load.txt` and `perform_generative_menu.txt` expect it
    (`version=N`, `genre=N`). Bump them with it (#423). The identity specs of House, Dub, Psytrance and Ambient pin their versions.
  - A `kFormat` bump doesn't excuse a changed `fingerprint` line whose parenthesized text stayed the same: the
    fingerprint doesn't depend on the dump. A change to how `stream_fingerprint.h` hashes or picks its pieces edits
    that text in `goldenText()` along with the `kFormat` bump.
  - A retune of Acid's drums, acid line, Arc or FX also fails `acid_fingerprint_spec`'s first check, which pins Techno's
    stream from before Techno split into subgenres: delete that check in the retune's commit. A retune of its facts
    (tempo, scales) or of the Drone alone leaves it passing: the fingerprint's pieces roll their own macros, scales and
    keys, and the check leaves the Drone out. Specs don't pin a genre's version; the golden does.
  - `stream_fingerprint.h` starts from 1469598103934665603, a digit short of FNV-1a's offset basis
    (14695981039346656037). Harmless, but fixing it changes every fingerprint: do it with `--update-goldens`, a
    `kFormat` bump and a new text in the fingerprint line's parentheses, and re-measure `acid_fingerprint_spec`'s split
    pin on the same commit.
  - CppSpec keeps the `const char*` an `it` is given: a title built at run time (one per genre) must outlive the
    spec, as `kGoldenTitles` does. A name in a spec's namespace that matches its `describe` (`golden`) or CppSpec's
    own (`describe`) is ambiguous.

## The harness's card (#448)

- `log_store.h`: `MemoryLogStore` stands in for the card-log task. Hook and `play()` drain the driver's log after
  every call, as the task runs between ticks; a spec that drives a `SteeringLog` itself drains (`store.drain()`) or
  the ring fills (a stall) and a kept piece's header is never taken (the next piece doesn't open). Read pieces with
  `harness::image(log, i)` (whole, the playing one too) or `store.stored(i)` (the file alone, through `readImage()`,
  as Capture reads it). A `LogRecord`'s path points into its image: keep the `PieceImage` alive while reading it.
- `regenerate_page_spec` reads the stock fonts' metrics: `src/deluge/gui/fonts/fonts.c` (plain C data) is linked into
  `generator_specs`. The menu rows' font is monospaced (6 px, `kTextSpacingX`); titles use the bold 9 px Metric font
  (`Canvas::drawScreenTitle()`, height 10), proportional, so a title's fit is a sum of glyph widths plus 2 px a gap
  (5 px after a space, which has no glyph). Kerning only narrows, so the spec leaves it out.
