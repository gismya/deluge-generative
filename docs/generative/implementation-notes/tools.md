# Implementation notes: Tools (capture, oscillator octaves, the theory review)

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- `osc_octaves.py` (in `check_all.py` after the harness specs, local only) reads the presets the built-in pitched parts
  can pick from `generator_render --list-palettes`, which prints them from the genre tables, so a new genre's palette is
  covered with no edit to the script. It looks each up in pack/ and then the factory card, and fails on a preset whose
  sounding oscillators are all 12 semitones or more off, on a missing one, and on an `ALLOWED` entry whose genre and
  part no longer list the preset. The factory presets all write `<osc1><transpose>` as elements; the parser takes
  attributes too.
- `gen_emu.py capture` names its files after the WAV's stem, which holds the whole Recipe. Keep that stem out of
  the QMP socket's path: a UNIX socket path past 104 bytes (macOS) makes QEMU fail every boot. `qmp_socket()` hashes
  it. The capture's check must read a `generator_render` built from the source the firmware was built from: capture
  builds that target in `build/tests/` under a lock right after the firmware, and renders there. A render left from
  before a table edit fails good captures (and can pass a real dropout). `./dbt test` and `check_all.py` build the
  same tree without that lock: don't run them beside a checked capture.
- A capture build (`-DGENERATIVE_CAPTURE=ON`) holds no Recipe: it plays the `CaptureRecipe` in
  `generative/firmware/current_recipe.cpp`, an `extern "C"` object `generative_capture_recipe` in `.data`, and
  `gen_emu.py capture` writes the Recipe's bytes (`CAPTURE_LAYOUT`) into a copy of the ELF beside the WAV, found by that
  symbol and checked by its magic, before it boots that copy. So a new Recipe builds nothing (#401). The two layouts
  must agree: change both, and the test's `RECIPE_LAYOUT`. The object is `volatile` (else LTO folds the build's
  defaults into the code and the patch does nothing) and `used, externally_visible` (else LTO makes it local and may
  rename it, and gen_emu can't find it). A file on the card image was the other way: the image is shared and booted
  with `-snapshot`, so each capture would need its own 445 MB copy or a FAT write.
- A capture's WAV is a function of its arguments *and the firmware's code*: delugemu's audio follows the instruction
  count, so any code change gives different bytes for the same Recipe (a no-op 100-iteration loop in
  `rollFromGenre` did). Compare captures from one build only. To check two builds play the same Recipe, compare
  `currentRecipe` in RAM after entering Perform (QMP `xp`; pointers resolved by symbol), as #401 did.
- What still relinks a capture build dir: a source change, a commit (the version string; `-dirty` too), and a build
  in a new build dir of the same checkout (`./dbt sizediff`, a fresh `check_all`): it rewrites the l10n
  `g_english.cpp` and `g_seven_segment.cpp` in the source tree, so every other build dir recompiles those and
  relinks once.

## Theory review

Each M2 genre slice runs the theory review (spec §21.5) once its genre plays. Build the harness, then summarise:

    ./dbt test --generator -n
    python3 scripts/generative/theory_review.py --genre <genre>

It renders the default Recipe at entropies 1, 2 and 3 (seed = entropy), each one Arc cycle, in about a second each
(`generator_render --end-after-drop --midi-only`; the MIDI's THEORY track carries the Recipe, rules, key, chords and
macros), and prints the summary, about 150 lines, to stdout. `--entropies`, `-- <generator_render flags>` (`-- --density
40`) and `--midi FILE...` cover other cases; `--help` says the rest. Ranges are the MIDI notes the Generator emits: a
preset's own transpose isn't in them (ADR 0007). Two lines come from the Generator's state, not the notes: "Chords" (its
chord per bar) and the gate buckets (Density sampled on bar lines). "Heard" and everything else come from the notes.
Notes per bar are per Section as it played (the Drop is the Groove after the Build), not per kind. A note counts on its
16th, Lean undone, and an Ornament hit (a ratchet's or roll's later hits, a flam's grace) on its main hit's: THEORY
lists them. The script judges nothing. Hand the summary to a fresh agent with this prompt:

> You are reviewing the music theory of the `<genre>` genre in gismya/deluge-generative. Read spec §4.5's brief and
> Tables paragraph for it (`docs/generative/spec/04-genres.md`) and the GLOSSARY.md terms you need. Below is
> `theory_review.py`'s summary of three renders of its default Recipe. Check it against §4.5 and the genre's
> conventions as a producer of the genre knows them: rhythm placement per lane, the harmonic rhythm and chord
> vocabulary, registers and how the parts share them, density across the Arc (the Break's dip, the Build's ramp, the
> Drop), gates, interplay and velocity. Any out-of-scale note, a "Heard" harmonic rhythm that differs from the
> genre's, a "Heard" set that doesn't match the "Chords" numeral of its bar, or a gated part playing below its gate is
> a bug. List each finding as either **fault** (the data contradicts §4.5 or is a bug: say what to
> change) or **judgement call** (§4.5 allows it, but a producer might choose otherwise: say both sides). Cite the
> summary's lines. Don't propose code. <summary>

Where the findings go: each fault is fixed in the slice's PR (a retune if the genre has a golden already), and the
review runs again; each judgement call becomes its own GitHub issue for the owner (label `needs-triage`), linked from
the slice's PR. The PR's test plan lists the review's run and its findings. The review never blocks a merge.
