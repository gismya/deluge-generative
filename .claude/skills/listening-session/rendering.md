# Rendering a listening session

How to render a session's clips, and the traps the first session (Psytrance, round 1) hit. Read it before the first capture.

Use the toolkit in the repo, `scripts/generative/listening/` (`python3 scripts/generative/listen.py --help`). A session writes only its content: a content file (cards), a variants file and job files, kept in the session folder's `tools/`. Its unit tests run in `check_all.py`; don't copy an earlier session's `tools/` scripts. The steps:

1. `listen.py card --scratch X pack`: `X/card_pack.img`, the stock card with `pack/` copied over it (`--no-pack`: stock only). `--palette FILE` also writes a palette override onto it (see "Preset candidates" below).
2. `listen.py variants make --session S --scratch X --content S/tools/variants_def.py`: each variant's diff to `S/diffs/<name>.diff`, made in a scratch worktree of the repo's HEAD (`X/wt/make`); commit what the variants build on first. A `--worktree` you give must be clean under `src/` and `pack/`: `make` and `prove` revert those around each variant. The file defines `VARIANTS = {name: Variant(patches=[...], edits=[(file, old, new)], why="...")}`; each `old` must occur once.
3. `listen.py variants prove --session S --scratch X --genre G NAME...`: builds each variant's `generator_render` in its own worktree (`X/wt/prove`), keeps it as `X/bin/<name>`, and prints the seeds (entropy = seed, default 20) where the variant changes no note.
4. `listen.py run --genre G --session S --scratch X --runners N JOBS...` captures in delugemu. `--dry-run` stops after the Recipe check and prints the build plan: per runner its captures, links and seconds, and the totals. Then `listen.py clips --genre G --session S --scratch X JOBS...` rough-renders the note-only cards' jobs with each variant's `X/bin/<name>`, all at once, in seconds. It refuses a `bin/<name>` built from another commit or diff than now: prove that variant again.
5. `listen.py page --content S/tools/content.py --session S --scratch X`: cuts the clips into `S/audio/` and writes `S/index.html` and `S/variants.md`. The content file's schema is in `listening/page.py`'s docstring; the build fails on a clip whose source doesn't exist and warns on a card past 60 words.

A job line is `name | variant | card | bars | args`: `bars` is how far from PLAY (bar 0) the clips need, and the seconds come from the harness render's tempo. `args` are `gen_emu.py capture`'s Recipe flags and `--mute`, parsed by gen_emu's own parser when the file loads. What the toolkit does for you, so a session doesn't redo it:
- **A comment is a whole line.** Keys are spelled with `#`: ambient rules 1's runner cut `--key D#` to `--key D` and captured three clips in the wrong key, and `gen_emu.py check` passed them, since it rendered the same wrong arguments.
- **The Recipe check**: before any capture, each job's harness THEORY `recipe` and `key` lines must match what it asked for (key, scale, tempo, macros, seed, entropy). A mismatch stops the run.
- **Merging**: jobs of the same variant, card, Recipe and mutes share one capture, the longest; the shorter ones are hard links to it (a shorter capture is a bit-exact prefix).
- **Order**: a runner claims a whole variant (costliest first), applies its diff once in its own worktree (`X/wt/r<N>`), takes its captures in Recipe order, and reverts after, so it relinks once per variant. A new Recipe builds nothing (#401), and the dry run counts no link for one.
- **Runners**: each with its own `--build-dir` (`X/build/r<N>`) and `--out` folder; a variant's lock in `X/raw/locks/`; one retry; waits while `X/pause` exists or the host runs `--cap` (default 6) emulators or more. Raw WAVs, logs and each source's sidecar (`X/raw/<name>.json`: its tempo, seconds, and what it was captured from) stay in the scratch folder. A rerun keeps a WAV only if its sidecar matches the job now (flags, variant diff, card image, source commit) and covers its bars; anything else is captured again. Each runner's worktree moves to the repo's HEAD when the run starts. `page` cuts a clip again when its source WAV or its bars change.

## Before any capture: the harness

Check every question in the harness first: a capture takes a minute, and the harness takes a second.

- Build `generator_render` with `./dbt test --generator -n`. For each candidate Recipe, render with `--end-after-drop --midi-only`, and read the MIDI file with `theory_review.piece_from_midi(open(path, "rb").read())`. A piece holds its sections (bar, label), the tempo and key, and each part's notes (`velocity`, `end`, drum lanes by note).
- **Pick seeds and macros where the ticket's behaviour shows**: the section the question is about, the pattern entry it names, the Bass velocities it quotes. `--motion 50` brings the Break, Build and Drop sooner (Psytrance: the Drop at bar 80 instead of 160).
- **Prove each table variant changes what the ticket says**, before you build it into firmware. `listen.py variants prove` builds each variant's `generator_render` and compares its MIDI with the base's over 20 seeds; count the notes yourself where the claim is narrower than "changes something". Tickets from a theory review can misread the table. Psytrance round 1 found three such cases:
  - an option that changes nothing at the Complexity the ticket names: `ByComplexity::at()` interpolates, so `{2, 6}` and `{4, 6}` both give a leap of 5 at Complexity 25 or less;
  - a pattern entry that sounds the same as the ticket's fix, because its `7`s already play at the default Density, while the behaviour came from another entry;
  - a "sparse" option that took the part out entirely.

  When the question as written can't be heard, render the nearest audible version, and say why on the card.
- **Rough-render clips** (a card about the notes only, as `SKILL.md` step 3 sorts them): `listen.py clips` renders them with the variant's own `generator_render` (the one `prove` kept), and the page cuts them like captures (the bars start at 0 too). 205 s of psytrance renders in 6 s. Kit rows play their real samples; pitched parts play one stand-in voice, every sound at home: no presets, sound controls or Drift sweeps. `--mute PART` in a job leaves that part out (generator_render's `--mute`, the same rows as gen_emu's), so a rough-render card can have its isolated clips too.
- **Palette picks per entropy:** a WAV render (not `--midi-only`) prints `warning: PART: SYNTHS/… not modelled` for each preset it loads. A part only loads once it plays, so add `--density 50 --bars 20` to see the Lead's. Scan entropies 0–30 to find one per pick. Each entropy also rolls its own key and tempo.

## Captures

- The genre must be on the branch you capture from. M2 genres were on `m2-all-genres`, not `generative`.
- **Pack card:** `listen.py card --scratch X pack` (an APFS clone of the stock image, `pack/` copied over it). Every capture of a pack sound needs it. `gen_emu.py capture`'s default `--card` is delugemu's stock image, which has no pack: a part whose pick is a pack preset (`SYNTHS/GENERATIVE/…`) then plays the next palette entry's factory preset (the fallback chain, spec §10.1), or the default patch, and `gen_emu.py check` passes it, since the notes are the same. A Dub techno Pad capture without the pack played the factory 126 instead of its pack copy an octave up. By hand, overlay the pack the way `listen.py card` does: clone the stock image, then `fat_image.FatImage(img)` and `fat_image.copy_tree(card, "pack")`.
- A capture's audio starts at PLAY, which is the harness's bar 0. Cut clips by bar, at the capture's tempo (`240 / bpm` s a bar). Check it once: the Build's Gap is a silent beat right before the Drop.
- `--mute` rows follow the genre's part order (Psytrance: 1 DRUMS, 2 BASS, 3 DRONE, 4 LEAD, 5 ARP, 6 FX). A drum lane can't be soloed. An isolated capture is silent where the muted parts would play; the check below allows for it.
- Costs on an M4 Pro: a capture takes about 19 s (boot, enter Perform) plus a quarter of its length, so an 85 s capture about 35–50 s. A fresh `--build-dir` adds a 2-minute full build.
- **A new Recipe builds nothing.** The capture build holds no Recipe: `gen_emu.py capture` writes the Recipe (seed, entropy, macros, key, scale, tempo, swing) into a copy of the built ELF beside the WAV and boots that (#401). A capture of a new Recipe on a built dir took 17 s for 20 s of audio, with `ninja: no work to do`. Mutes, `--seconds` and the card build nothing either.
- **What still costs a build** (a relink under LTO, 47–60 s, one core, 1 GB): a variant's source diff, applied or reverted, even a revert to the same text (ninja goes by file times); a commit (the version string; `-dirty` too); and a build in a new build dir of the same checkout (`./dbt sizediff`, a fresh `check_all`), which rewrites l10n files in the source tree, so every other build dir relinks once. So apply a variant once before its group and revert it once after, as the toolkit does.
- **`--elf`: build once, capture many.** `gen_emu.py capture --elf <build dir>/Release/deluge.elf` captures with that capture firmware and builds nothing (`--build-dir` is ignored). Use it when several captures of one source run at once: they share the ELF and no two ninja runs touch one dir. Its check renders with the current checkout's `generator_render`, so the checkout must hold the same Generator source as the ELF (else `--no-check`, and check by hand).
- **A shorter capture is the start of a longer one, bit for bit**, for the same build, card, Recipe and mutes. Give each such combination one capture, as long as its latest card needs, and cut every clip from it.
- gen_emu writes 32-bit WAVs, about 60 MB per 170 s. Keep them, the card images and the build dirs (735 MB each) in a scratch folder, not the session folder, and delete it when the page is done. Ambient rules 1 left 2.6 GB in its session folder for a 48 MB page.
- **At most six captures at once on the host**, across all sessions, as `SKILL.md` step 3 says. Each needs its own `--build-dir`, built beforehand. Measured in #377 (85 s psytrance capture, M4 Pro, 12 cores): identical sha256 and a passing `check` at 1, 2, 4 and 6 at once; wall time per capture 31 s alone, 33 s at two, 42 s at four, 38 s at six; throughput 2.7, 5.0, 8.0 and 13.2 captured seconds per wall second. Queue the jobs in a file and run up to six at a time.
- The first capture into a freshly built `--build-dir` can rebuild the git-state version and relink, about 40 s, before its emulator starts (#377 saw it once per dir). Count it when you time captures, or run one throwaway capture per dir first.
- **Each capture checks its WAV** (`gen_emu.py check`, run by `capture` unless `--no-check`), and exits 1 if it fails. Capture again any that fails. After the firmware, `capture` builds the harness's `generator_render` from the same source (only that target, in `build/tests/`) and renders the same Recipe, so a table variant's capture is checked against the variant's notes. It fails a WAV that:
  - is shorter than `--seconds`;
  - is silent for the beat after a 16th where the harness render of the same Recipe starts a note in a part the capture didn't mute (so isolated captures and sparse sections such as Dub's Intro pass, and a dropout shorter than a bar fails);
  - is at full scale for 50 ms in a row (clipped flat or corrupt). A shorter run is a WARNING: the mix clips.

  It doesn't check the key: it passes a capture in the wrong key (#405 adds a pitch check). Until then, the Recipe check above catches a mangled job.

  To check WAVs made before, or by hand: `gen_emu.py check WAV... ` with the capture's Recipe flags, `--seconds` and `--mute`s. It reads the newest `generator_render` under `build/tests/` (or `--render`), logs which, and warns if the Generator's source changed after it was built. With six emulators on the host (two sessions at three each), captures came back short, corrupt and partly silent. Before capturing a bad one again, wait for the other session's emulator to finish.
- A capture rebuilds from its worktree's source before it boots. Keep a table variant applied through its group of captures in that worktree, and revert it right after the group.
- Captures of different Recipes can share a `--build-dir` and run at once: each patches its own copy of the ELF (`<out>/<tag>.elf`, deleted after). Two at once from one dir gave the same sha256 as one alone (#401). What a build dir can't share is its source: runners that apply different variants need their own worktree and build dir, as the toolkit gives them. Captures can share an `--out` folder too: each names its raw dump and ELF copy after its WAV and its process, and claims its WAV's name when it starts. (Round 1 lost 38 captures to a shared folder, before that.)
- **Clips:** `listen.py page` cuts them: by bar at the source's tempo, 32-bit to 16-bit (`>> 16`), 10 ms fades so a loop doesn't click, `afconvert -f m4af -d aac -b 160000`. It keeps the captured level: a level is part of what the owner judges.

## Preset candidates: a palette override

To compare preset candidates for a part or lane, force them with a palette override on the card instead of a table variant: no table change, no diff, no rebuild. The firmware reads `GENERATIVE/PALETTES/<GENRE NAME>.XML` (spec §10.1), where the name is the genre's OLED name (`AMBIENT`, `DUB TECHNO`, `ROOTS DUB`), and each part, role or lane it lists replaces the built-in entry; the rest falls back. One entry per part pins it:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<palette>
	<part name="DRONE"><preset path="SYNTHS/GENERATIVE/GEN Ambient Drone 1.XML"/></part>
	<part name="PAD"><preset path="SYNTHS/131 Warm Strings.XML"/></part>
	<lane name="kick"><file path="SAMPLES/KICKS/…"/></lane>
</palette>
```

- Make one card per candidate set: `listen.py card --scratch X --palette S/tools/d1/AMBIENT.XML d1`, then name card `d1` in its jobs. A card is an APFS clone, so cards are cheap; the toolkit still merges and orders the jobs, and a card made again is captured again.
- Pin the other parts whose picks the candidates sit beside too (Ambient sounds round 1 pinned the Pad and the bells), so every clip differs only in the part in question.
- The file must be named exactly `<GENRE NAME>.XML` and stay within the limits (8 KB, 8 presets a part): a bad name, a malformed file or one past a limit is ignored, and the built-in palette plays with no warning. `listen.py card` refuses a name that isn't a genre's OLED name and a file over 8 KB. The harness doesn't read the card, so only a capture shows it: listen to one card's part alone before the batch.
- A candidate that isn't in `pack/` yet (a new preset from `make_pack.py`): regenerate `pack/` in the worktree you make the card from, so the overlay holds it.
- Overrides change only the sound, never the notes, so the capture check holds as it is.

## Normal mode: a song file in delugemu

No repo tool loads a song into an icount capture. delugemu's `DelugeSession.load_song` clicks with mouse events, which QEMU refuses while gen_emu holds the VM stopped ("VM not running"). Drive the browser with gen_emu's `Emu` instead:

1. Boot. A fresh boot sits in a blank song's clip view.
2. `emu.tap("LOAD")` opens the song browser at `SONGS/`.
3. Type the name with `emu.s.pad_event(x, y, …)` on the pads `delugemu.qwerty.key_for(ch)` gives. The browser jumps to the first entry the text prefixes.
4. `emu.tap("SELECT_ENC")`, then `emu.card_quiet()`.

To debug, sample `delugemu.display.oled_text(emu.s.read_oled())` every 150 ms after SELECT. A failed load flashes its error ("FILE CORRUPTED", "E412"), then goes back to the old song.

Write the song from a template: the header of `contrib/audio_export/multi_sample/Multisample_Template_Full_Bar.XML` up to `<instruments>`, then your own instruments, `<sections>` and `<sessionClips>`. Each of these but the layout was a failed load or a silent song once:

- **Tempo:** `timePerTimerTick` and `timerTickFraction` hold `110250 / (bpm × 4)` as a whole part and a 32-bit fraction, with `inputTickMagnitude="2"`. That gives 96 ticks a beat (a 16th is 24 ticks), the same as the harness MIDI.
- **Layout:** set `sessionLayout="0"` (rows), so the mute column shows each clip's play state.
- **A synth:** `<sound presetName="…" presetFolder="SYNTHS/…">`, then the preset file's `<sound>` body. To match what Generative mode loads, take out `<arpeggiator>`, set the top-level `<transpose>` to 0, and scale `<volume>` for a part's `.volume` the way `scaleKnob()` does.
- **Each synth clip needs `<soundParams>`**, holding the preset's `<defaultParams>` body. Without it the load freezes with E412 (no patch-cable set).
- **A kit clip needs `<kitParams>`.** Without it the clip looks for a synth of the kit's name, and the load fails with FILE CORRUPTED. Use an empty `<kitParams></kitParams>`: the template's has the volume at minimum. Each kit `noteRow` needs `<soundParams>` too. Keep only the kit rows that have notes.
- **`noteData` notes are 20 hex characters:** position (8), length (8), velocity (2) and probability (2; `14` is 100 %). Leave out the probability and the notes are misread: the song loads, PLAY lights, and it plays silence.

Mark these clips "rendered in normal mode". They miss what Generative mode adds (the Drift sweeps, sound controls, the other drum lanes), so compare them only with each other. Give a Generator clip of the same Recipe beside them as a reference.

## The page

- On `file://`, `navigator.clipboard` can be missing. Fall back to `document.execCommand('copy')` synchronously inside the click, and keep the export in a visible textarea so it can be copied by hand.
- Test the page in a browser before the report: every card renders, one clip from each card loads, an answer survives a reload, and Export opens with the skill's line. Then clear the test answers from `localStorage`.
