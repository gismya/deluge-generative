# Implementation notes: Tools (capture, oscillator octaves, the theory review)

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- `make_pack.py`'s `knob(50)` writes `0x7FFFFFFF`, as the sound editor saves its top
  (`computeFinalValueForStandardMenuItem`), not `50 * 85899345 - 2^31` (`0x7FFFFFD2`): `SoundControls::uses()`'s
  `SustainBelowFull` compares with `INT32_MAX`, so the formula's value bound Decay to env1's decay on full-sustain
  presets (#483). A sample preset (`sample_preset()`) leaves its samples on the factory card: only the XML is in the
  pack, unless it names a `DERIVED` sample (`LeadTrim`, Dub's `GEN DDD1 Snare wet`). Trap: `main()` runs `to_wav()`
  (mono, normalised) only over `SAMPLES`' synthesized lists; a `DERIVED` WAV is written as its edits leave it. A factory
  drum sample can start late: `DDD1 Snare wet` sits near silent for 33 ms, a flam on a backbeat; check a new drum file's
  attack against the others on its lane. Past the volume knob's top: `clippingAmount` (the voice's saturation) is
  `tanh(x << (5+c)) >> (6+c) << (c-2)`, so its ceiling is the same at every amount. On a hot sample (the 808 boom) it
  lowered the peak and added 0-2 dB; on the noise wash, 4 added 6 dB. A sample on both oscillators adds 6.0 dB; a
  velocity->volume cable adds gain for velocities over 64 (the source is bipolar), about 5.5 dB at 50 for an Impact's
  80-94 (#472).
- `osc_octaves.py` (in `check_all.py` after the harness specs, local only) reads the presets the built-in pitched parts
  can pick from `generator_render --list-palettes`, which prints them from the genre tables, so a new genre's palette is
  covered with no edit to the script. It looks each up in pack/ and then the factory card, and fails on a preset whose
  sounding oscillators are all 12 semitones or more off, on a missing one, and on an `ALLOWED` entry whose genre and
  part no longer list the preset. The factory presets all write `<osc1><transpose>` as elements; the parser takes
  attributes too.
- `generator_render --mute PART` mutes only the copy of the rendering the WAV is made from (`withMutes()` in
  `render/command.cpp`): the MIDI file and piano roll must stay byte-identical, so `theory_review` and `gen_emu.py
  check` read the same notes with or without it. A row past the genre's own parts fails (Acid techno has 4), where
  `gen_emu.py capture` takes 1-6 for any genre.
- `gen_emu.py capture` names its files after the WAV's stem, which holds the whole Recipe. Keep that stem out of
  the QMP socket's path: a UNIX socket path past 104 bytes (macOS) makes QEMU fail every boot. `qmp_socket()` hashes
  it. The capture's check must read a `generator_render` built from the source the firmware was built from: capture
  builds that target in `build/tests/` under a lock right after the firmware, and renders there. A render left from
  before a table edit fails good captures (and can pass a real dropout). `./dbt test` and `check_all.py` build the
  same tree without that lock: don't run them beside a checked capture.
- The capture's check counts a Riser lane's note on the beat before its note-off (the Drop), not on its onset
  (`active_steps`): the firmware starts a sample Riser late so it ends on the Drop (`generative/perform/riser_timing.h`),
  from the length of the sample it picks off the card, which the render can't know. Synth Risers count there too: the
  render doesn't say which kind a lane plays. A capture with no harness step to check fails if silent throughout.
  The default `--card`, the factory image, has none of the pack's Riser samples: a Riser there is silent and its
  capture fails the check. Capture on a pack card (listening-session `rendering.md`, **Pack card**).
- `gen_emu.py capture`'s default card (delugemu's `deluge_sd.img`) has no pack, and a pitched part's capture passes
  its check either way (a Riser's doesn't, above): a pack pick quietly plays its fallback chain's factory preset. To hear a pack preset, pass `--card` a copy
  of the image with pack/ put on it: `test_port_emu.apply_overlay(copy, folder)` with a folder holding only a
  `card.txt` of `copy pack -> /`.
- `gen_emu.py capture` boots the test-port build (`-DENABLE_SYSEX_TEST=ON`, in `--build-dir`) and sends the Recipe
  and seed as the test port's `next recipe` lines (seam S7, spec §21.3) before it enters Perform, so a new Recipe
  builds nothing (#523). delugemu's DIN MIDI goes to a UDP socket (run.sh's `--midi`, which
  `DelugeSession.launch()` has no argument for, so `launch_with_midi()` starts run.sh itself). The capture checks the
  entry's `gen_next_recipe taken` event, then stops the test events before PLAY. A `--variant` capture still takes
  the older route, since S7 has no variant lines yet (#548): a `-DGENERATIVE_CAPTURE=ON` build in
  `<--build-dir>-variants/` whose `CaptureRecipe` (GENCAP2, with the variant pairs) gen_emu patches into a copy of
  the ELF (#401, `capture_recipe()`, `write_capture_elf()`). The two routes build in separate dirs, so a session
  mixing them never reconfigures one; each dir builds once. Keep `CAPTURE_LAYOUT` and `CaptureRecipe` in step until
  #548 retires the route.
- Under icount the VM moves only in `run_to()`, and any wait whose length the host decides makes captures differ run
  to run (#523 saw three WAVs from one build): `TestPort.wait_idle()` and `pump()` run for wall-clock time, so a VM
  stepped inside them moves a host-dependent distance. So `send_recipe()` sends each line while the VM is stopped,
  then runs it in 10 ms virtual steps until that line's reply (`TestPort.inject_stepped()`), and `settle()` ends each
  exchange at a fixed virtual time. The one wall-clock dependency left is `LINK_DELIVER_S`: `IcountLink.send()` gives
  QEMU 0.1 s to hand a request's bytes to the UART's receive DMA, which takes them at once even with the VM stopped.
  Each reply then takes one step (`REPLY_STEPS`); a capture warns when one takes more, as a host too loaded to
  deliver in time would make it.
- A capture's WAV is a function of its arguments *and the firmware's code*: delugemu's audio follows the instruction
  count, so any code change gives different bytes for the same Recipe (a no-op 100-iteration loop in
  `rollFromGenre` did). Compare captures from one build only. To check that two builds or routes play the same
  Recipe, read `currentRecipe` in RAM after entering Perform and compare all but its pointers (the genre's): QMP `xp`
  (#401), or `pmemsave` of its first 4 KB, which holds the Recipe, the sound picks and the seed (#523). Quote
  `pmemsave`'s path: HMP reads a bare `/` as division.
- What still relinks a capture build dir: a source change, a commit (the version string; `-dirty` too), and a build
  in a new build dir of the same checkout (`./dbt sizediff`, a fresh `check_all`): it rewrites the l10n
  `g_english.cpp` and `g_seven_segment.cpp` in the source tree, so every other build dir recompiles those and
  relinks once.

## The firmware check (`gen_emu.py golden`, #141)

- `golden_compose.txt` runs under `gen_emu.py golden`, not as a probe script: check_all only picks scripts named
  `perform_*.txt` or `compose_*.txt`, so check_all doesn't list it as a probe script it can't run (its `# needs:`
  line); `gen_emu.py golden` runs it. It has no covers lines;
  check_all runs it whenever it runs delugemu. `# needs: gen_emu.py` keeps it off hardware sessions.
- The golden ends at its last line, not at 16 bars: Perform's stop ends what still sounds at the driver's clock (6132
  or 6134), so a Drone note is shorter in the golden than in the song. Both sides cut there and drop notes within
  `kMaxLean` of it. Muted notes are in the golden and never in a Compose song.
- The song is read with the Recipe file: Kits are named after their part, synths after their preset, so a synth clip
  maps to its part through the Recipe file's `sound` path. Instruments come in roster order.
- To prove it fails, change the stream in `src/` (a velocity in the genre table), rebuild build-testport/ only and run
  `gen_emu.py golden`: the goldens are the harness's, so they stay as committed.

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

## Listening-session toolkit

- `scripts/generative/listening/` (`listen.py`) builds listening sessions; its tests are `test_listening.py`. A job
  line's comment is a whole line: never cut a line at `#`, since keys are spelled with it (`--key D#`). The Recipe
  check (`listening/recipe.py`) compares each job's harness THEORY `recipe` and `key` lines with the job's own flags,
  because `gen_emu.py check` renders the same arguments the capture got and so passes a mangled job.
- Job args go through `gen_emu.add_recipe_args`, so a new gen_emu Recipe flag reaches jobs with no edit here; add it
  to `jobs.RECIPE_FIELDS` and `recipe.mismatches` so the check and the merge see it.
- The runner applies a variant once per claimed group and reverts after; ninja goes by mtime, so a revert followed by
  the same apply still relinks. Merged captures are hard links in `raw/`: the cutter reads `raw/<name>.json` for the
  tempo, so write one for any WAV you put there by hand.
- A gen_emu flag that only steers the check (`--quiet-bars`) stays out of `Job.capture_flags()`: that list is the
  stamp's `args`, so putting it there would make every WAV captured before stale. The runner adds it to the command
  line itself (`Run.command`), and the `.failed` marker records it (`Run.failed_stamp`), so naming quiet bars after a
  failure retries.
- A runner holding two chunks of one variant (`base#1`, `base#2`) skips the revert and apply between them: it would
  relink for nothing (ninja goes by mtime).
- A job's genre is its own `--genre`: the job parser's genre default is None and `jobs.parse_args` fills in the
  command's `--genre` after parsing (#426). Never put the command's genre on the job's argument list: it only won
  by coming first (last wins), so a job's `--genre` beat it by accident. A variant's genre lives in
  `diffs/genres.json` (`variants.genre_of`), a page card's in its `genre` field or its midi entry's args.
- A WAV kept with its check failed (`info["check"]`) is current only for the quiet bars it was checked with
  (`info["quiet"]`, `Run.done`); a clean WAV stays current whatever its job's `--quiet-bars` (#426).
- `variants.build_renders` reuses `bin/<name>` when `bin/<name>.json` equals `render_stamp` now: the stamp is the
  commit and the diff only, so a change to `build_render` itself (flags, target) needs the stamp extended.
- The page's script is tested in node on a stub DOM (`test_listening.page_export`): keep `exportText()` a top-level
  function and the page free of anything a Proxy stub can't stand in for at load.
- The pack holds few drum samples (the GEN rides, claps and brushes, Dub's snare, Psytrance's `GEN PSYKICK-` kicks):
  most of a kit's lanes come from the card folder, so `generator_render --list-sounds` over `pack/` alone prints `-`
  for most drum lanes.
- A clip's timeline strip (`listening/timeline.py`, #513) names each part's sound from `generator_render
  --list-sounds`: the harness's palette roll over `pack/` and the stock card, not what the capture loaded. A capture
  on a palette-override card (`listen.py card --palette`) plays other presets: give the clip's timeline `sounds`
  ({PART: name}, merged over the listed names). A sample lane's file can differ too (the card's file order). Every
  render the page runs goes through `variants.current_render`, which refuses a `bin/<name>` whose stamp is stale.
- `gen_emu.py capture --variant`: the firmware's capture field is 64 B ("set=name,…" with its NUL), so Psytrance's
  five sets don't all fit: leave out one the clip doesn't reach (`break` for a piece with no Break) (#497).
- `gen_emu.py capture --elf` wants a test-port build (`-DENABLE_SYSEX_TEST=ON`), and with `--variant` a
  `GENERATIVE_CAPTURE` build (its `generative_capture_recipe`), not `./dbt build release`'s ELF: give `--build-dir`
  and let it build one. To compare two firmwares, run each capture from its own tree (a scratch
  worktree for the base) with its own `--build-dir`, on the same card, with one `--render` (when the events match).
- A power difference between a Bass+Drums and a Bass-alone capture misreads the kick's share once the bass is the
  louder: phase and the bass's ducking move the sum by more than a faded kick's remainder. Capture the Drums alone
  for the kick's level per 16th (#497).
