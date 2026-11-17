# Implementation notes: Build and tools

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
  `python3 scripts/generative/arm_types.py` compiles `generative/core/`, `perform/` and `runner/` (each `.cpp`, and
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
  `./dbt exec`, and the build is multi-config: without `-C Debug` every test shows "Not Run".
- Harness specs: one `describe` per `*_spec.cpp`, named after the file stem, and the file ends with
  `CPPSPEC_SPEC(<the same name>)`: a different name fails the link with an undefined `<name>_spec`. Put
  `// clang-format off` above the `describe`, as every spec does: clang-format mangles CppSpec's nested lambdas, and
  the commit hook's reformat then lands in the next `--amend`. Don't name a local `it` (CppSpec's macro). Specs reach
  `pack/` and fixtures through `GENERATOR_REPO_DIR`.
- Don't edit a harness header while `./dbt test --generator` builds. An object that started compiling before the edit
  and finished after it is newer than the header, so ninja keeps it, with the old layout: the specs then segfault at
  once (`generator_specs transitions_spec` printing nothing). `touch` the header and build again.
- Stepping a piece in a spec: use `StepwisePiece` (`tests/spec/generator/harness.h`), not a hand-wired driver or a
  bare `Generator::decideNextStep`. `decideBefore(step)` stops at step - 1's decision tick, so a push then reaches
  `step`. `runToStepLine(step)` stops on `step`'s grid line, so a push then reaches step + 1. `runToStepLine` counts
  from the last call: after a jump, or once a transition's incoming instance plays, run in song ticks (`runToTick`).
  `decideBefore` and `pushAndDecide` count steps on the instance that plays when they are called, and return false
  if it stopped deciding first (the piece ended or stopped, or a transition's incoming instance took over). For a
  transition use `TransitionPiece` (`transition_piece.h`): it runs the real lifecycle, so `confirm()` is a request
  and a preload that ends well. `TransitionPiece(playing, pending)` takes two Recipes, each at its own genre, tempo,
  swing and key. `genre_change_matrix_spec` changes every ordered pair of `allGenres()` in all three styles with it,
  well inside spec §21.2's 15 s budget: a genre joins it by being in `allGenres()`. Each case runs once and its faults print as
  `<from> -> <to> (<style>): <what>` lines, so a failure names its pairs.
  Shared stream helpers are in `stream_helpers.h`. A helper there shares its name with every spec that includes it:
  delete the spec's own copy, or the call is ambiguous.
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
- A probe script that needs its own delugemu card names its overlay folder in `# needs: card fixtures/<script>`
  (relative to `scripts/generative/`); `test_port_emu.py` reads the line and builds that card, and refuses scripts
  that need different cards in one run (`--card-overlay` overrides the line). A `remove` in
  the overlay's `card.txt` of a path the factory card doesn't have fails the run: the card changed under the overlay.
  Reuse fixtures from the repo with `copy <repo path> -> TO` rather than copying them into the overlay. A `#` in
  `card.txt` starts a comment, so no path there may hold one. `fat_image.py` handles FAT32 on the whole image only
  (delugemu's card); its unit tests format their own images and run `fsck_msdos` or `fsck.fat` on each where the
  host has one.
- Probe scripts: start every script with `start_state`, then Perform scripts with `include perform_open.txt` and
  `include perform_confirm.txt`. An expect's timeout needs the keyword, last on the line: `expect event gen_left
  within=15`. A bare trailing number is part of the match text (`expect event gen_section INTRO 0` matches section
  0). Over delugemu's DIN link test-port events can arrive seconds late: give `expect event` a long timeout before an
  `expect no_event`. SHIFT+SONG right after reaching session view needs about 1 s (the zoom animation holds a UI
  mode).
- A probe expect's `within=` counts from the expect line, the wait for the firmware's queue included (`no_event`
  alone watches its whole window after that wait). An expect after queued `wait`s and turns needs a window that covers
  them too (`perform_transport.txt`'s swing: 4 s of queue, `within=10`).
- A fast gold turn (`encoder 4 N fast`) takes no time, so presses a slow turn used to space out can land in a
  double-tap window (`perform_homes.txt` waits 400 ms before pressing DRUMS again). Its 4-step edits don't always
  act as 4 one-step ones: stock deletes a mod cable inside the one-step edit that reaches 0, not after fast turns.
- Swapping a state expect for an event expect moves the event search: `exitUI()` lets go of a held row before
  `gen_leave_menu`, so the focus event after it needs `since=mark` (`perform_homes.txt`).
- Hold the BACK button or the End pad with `include perform_hold_back.txt` / `include perform_hold_end.txt`, not a copy
  of their three lines: their `held_ms=` bounds go up to 999 ms, as a card access can delay the menu or the arm by a
  few hundred ms (a 100 ms margin over delugemu's numbers would be flaky on hardware).
- run_script lets go of every hold when a script ends, also on an error (`hold entropy` left on pins every entry until
  a reboot). To compare events between two runs of something whose events depend on the card, `mark`, then `keep
  events TEXT`, `mark` again and `expect events kept` (`perform_entropy.txt`). Every Perform entry rolls a fresh
  Recipe, the Current Recipe or not.
- `./dbt exec "..."` runs its string through `sh`: a file list with newlines in it (`$(git diff --name-only)`) runs
  each path as a command, and an executable one (`scripts/tasks/task-test.py`) really runs. Join the list with spaces
  (`$(git diff --name-only | tr '\n' ' ')`).
- A `git commit` runs pre-commit, which stashes the unstaged files for a few seconds and puts them back: a build
  running in the background meanwhile (a test-port build for delugemu, `check_all.py`) can compile the committed
  version of a file you changed but haven't staged yet. Commit before starting a long build, or stage everything first.
- A test-port hold (`hold NAME`, `release NAME`) is a row of `kHolds` in `io/midi/test_port.cpp` plus the hook that
  reads it. Nothing else needs a new hold added (the probe's `HOLDS_BEFORE_RELEASE_ALL` is frozen, for older builds): `start_state` sends `release all`, which sets every row off, so a row's
  `set(false)` must be safe when the hold was never on.
- An `expect event` or `expect no_event` TEXT matches as one piece of the event's text: `gen_focus locked=-1`
  never matches `gen_focus part=-1 locked=-1`, so an `expect no_event` written that way passes whatever happens. Use
  the `/regex/` form (`/gen_focus part=-?[0-9]+ locked=-1/`) for fields that aren't adjacent, and run a new no_event
  on a build without the fix once, to see it fail.
- `mark` keeps the place of the last expect's match, not the moment of the `mark` line, and an `expect no_event`
  doesn't move it. After a long `expect no_event ... within=20`, `mark` then `expect event gen_phrase ... since=mark`
  matches a phrase that started inside that window, so "just after a phrase starts" was up to 20 s late
  (`perform_preset_browser.txt`'s cancel cases swapped before their BACK tap). Use `mark now` there: it keeps the end
  of the events received so far, once the firmware has sent what it queued.
- A Y turn (`encoder 0 N` on a focused part) steps from the take queued, or else the one playing
  (`PerformView::verticalEncoderAction()`). Right after one turn, a turn back acts differently before and after the
  queued take lands on its phrase line, so its `gen_press take` depends on timing (#298 dropped such an expect). Expect
  a second turn's press only once the first take has landed.
- `hold preload` parks only the first preload job that starts after it: `TestPort::waitWhilePreloadHeld()` turns the
  hold off when that job's wait ends, by a release or a cancel. A confirm that cancels a held sound preload therefore
  lets its transition's preload run straight through (`perform_transition.txt`). Each `expect event` searches after
  the previous expect's match, so two expects can't both match one event: put every field you check in one expect (a
  `/regex/` when they aren't adjacent).
- `start_state` ends with `reset recipe` (`resetRecipe()` in `io/midi/test_port.cpp`), which zero-fills the Current
  Recipe as boot does (`forgetCurrentRecipe()`): with `exists` false, GENRE ▸ opens on the default genre and entry
  rolls a fresh Recipe. Entry picks ACID by name anyway (`pick genre`, in `perform_pick_acid.txt`), so a session
  stopped partway through a genre change can't move the next script onto another genre. It resets only the Current
  Recipe: other Generative state a script leaves outside Perform
  outlives `start_state`, so a probe script that depends on more than the genre after boot must set it itself.
- Test events from `PerformAdapter::tick()` come when the Generator decides a step; those from `OutputSink::play()`
  (`gen_section`, `gen_key`, notes) come when the step plays, a little later. So `gen_phrase bar=8` (decided) arrives
  before bar 8's `gen_section` (played): an `expect no_event gen_section ...` right after it sees that line's Section.
- Pick genres by name in probe scripts (`pick genre techno-dub`, test-port `pickGenre()` with
  `pickGenreForTest()` in `gui/menu_item/generative/genre_rows.cpp`), never by encoder turns from where GENRE opens:
  new genres add rows. The menu side only works on the GENRE ▸ or parent submenu whose session began last.
- `gen_outputs_freed outputs=N total=M`: `total=` counts a transition's Outputs freed since none was left retiring
  (a sound re-roll's, retiring alongside, don't count), so a script pins a roster's whole retirement
  (`/^gen_outputs_freed outputs=\d+ total=5$/`) whether it came in one batch or several. A batch can reach the total
  with another still to come: `mark` after it and `expect no_event gen_outputs_freed since=mark` later
  (`perform_genre_change.txt`).
- Spec temp files: name them with `tempPath()` (`tests/spec/generator/temp_path.h`), which adds the pid, so harness
  runs in parallel worktrees never share one; a folder per spec file goes in a `RemovedAtExit`.
- Seeded harness counts are exact: pin them (`drift_spec`'s depths and turns). `--update-goldens` doesn't refresh
  such pins: a retune that moves them re-pins them by hand. A floor that only shows a run reached its
  worst case (`roster_room_spec`, `stab_spec`) says so in a comment, so nobody tightens it into a pin a retune breaks.
- The firmware queues test events (`TestPort::event()`, 64 lines) and sends one a pass of the main loop, each after
  the last has left, so a burst can fill the queue. With 16 lines a transition's start step (about 25 events in 160
  ms) dropped a few in every delugemu run, now and then one a script waited for: #272's "phase stays retiring" was a
  dropped `gen_transition phase=idle`. A dropped line leaves `test_events_dropped n=N` where it was, and
  `run_script()` fails a script whose run moved state's `events_dropped` (one more expect, `FAIL end: N test events
  dropped`). It reads `events_dropped` only once state's `event_queue` (lines not yet sent) is 0 (`wait_sent()`): a
  line dropped while start_state's own burst drains isn't the script's, and one dropped after the last line is. When
  that fails, the script's other failures may be missing events, not firmware: read the log around the
  mark before debugging. A script that sends more in a burst needs a bigger queue, not fewer expects.
