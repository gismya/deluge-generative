# Rendering a listening session

How to render a session's clips, and the traps the first session (Psytrance, round 1) hit. Read it before the first capture.

Start from that session's scripts in `~/.cache/deluge-generative-m2/tuning/psytrance/round-1/tools/`:
- `content.py`: the cards, clips and held questions;
- `cap2.py` and `check_wav.py`: the capture runner, and the check it runs on each WAV;
- `variants.py`: the table edits;
- `song.py` and `normal_cap.py`: a normal-mode song, and its capture;
- `build_session.py` with `page.html`: it cuts the clips and writes `index.html` and `variants.md`.

Swap in the new genre's content, and keep the rest, with two fixes the later sessions needed (#403 will move a tested version into the repo):
- **A job line's comment is a whole line.** Don't cut a line at `#`: keys are spelled with it. Ambient rules 1's runner cut `--key D#` to `--key D` and captured three clips in the wrong key, and `gen_emu.py check` passed them, since it rendered the same wrong arguments. Split the args with `shlex`.
- **Check each job's Recipe before capturing.** Render its harness MIDI and compare the THEORY track's `recipe` and `key` lines with what the job asked for (key, scale, tempo, macros). It takes 30 ms.

## Before any capture: the harness

Check every question in the harness first: a capture takes a minute, and the harness takes a second.

- Build `generator_render` with `./dbt test --generator -n`. For each candidate Recipe, render with `--end-after-drop --midi-only`, and read the MIDI file with `theory_review.piece_from_midi(open(path, "rb").read())`. A piece holds its sections (bar, label), the tempo and key, and each part's notes (`velocity`, `end`, drum lanes by note).
- **Pick seeds and macros where the ticket's behaviour shows**: the section the question is about, the pattern entry it names, the Bass velocities it quotes. `--motion 50` brings the Break, Build and Drop sooner (Psytrance: the Drop at bar 80 instead of 160).
- **Prove each table variant changes what the ticket says**, before you build it into firmware. Apply the edit, rebuild the harness, copy `generator_render` aside per variant, revert, and compare the variant's render with the base render (`cmp` on the MIDI files over 20+ seeds, or count the notes). Tickets from a theory review can misread the table. Psytrance round 1 found three such cases:
  - an option that changes nothing at the Complexity the ticket names: `ByComplexity::at()` interpolates, so `{2, 6}` and `{4, 6}` both give a leap of 5 at Complexity 25 or less;
  - a pattern entry that sounds the same as the ticket's fix, because its `7`s already play at the default Density, while the behaviour came from another entry;
  - a "sparse" option that took the part out entirely.

  When the question as written can't be heard, render the nearest audible version, and say why on the card.
- **Rough-render clips** (a card about the notes only, as `SKILL.md` step 3 sorts them): render the variant's own `generator_render` without `--midi-only`, at the card's Recipe, and cut the clip from its WAV as from a capture (it is 16-bit already; the bars start at 0 too). 205 s of psytrance renders in 6 s. Kit rows play their real samples; pitched parts play one stand-in voice, every sound at home: no presets, sound controls or Drift sweeps. It can't mute a part yet (#402), so give these cards full-mix clips, and a delugemu isolated clip only where the part is lost in the mix.
- **Palette picks per entropy:** a WAV render (not `--midi-only`) prints `warning: PART: SYNTHS/… not modelled` for each preset it loads. A part only loads once it plays, so add `--density 50 --bars 20` to see the Lead's. Scan entropies 0–30 to find one per pick. Each entropy also rolls its own key and tempo.

## Captures

- The genre must be on the branch you capture from. M2 genres were on `m2-all-genres`, not `generative`.
- **Pack card:** clone the stock image, `cp -c ~/source/delugemu-sd/deluge_sd.img card.img` (an APFS clone, instant), then `fat_image.copy_tree(FatImage("card.img"), "pack")`.
- A capture's audio starts at PLAY, which is the harness's bar 0. Cut clips by bar, at the capture's tempo (`240 / bpm` s a bar). Check it once: the Build's Gap is a silent beat right before the Drop.
- `--mute` rows follow the genre's part order (Psytrance: 1 DRUMS, 2 BASS, 3 DRONE, 4 LEAD, 5 ARP, 6 FX). A drum lane can't be soloed. An isolated capture logs `WARNING silent from …` for the stretches the muted parts would fill. That's expected.
- Costs on an M4 Pro: a capture takes about 19 s (boot, enter Perform) plus a quarter of its length, so an 85 s capture about 35–50 s. A fresh `--build-dir` adds a 2-minute full build.
- **A new Recipe relinks the firmware.** gen_emu compiles the Recipe (seed, entropy, macros, key, scale, tempo, swing) into the capture build, so a capture whose Recipe differs from the last one in its `--build-dir` relinks under LTO first: 47–60 s, one core, 1 GB. So does a table variant applied or reverted, even a revert to the same text (ninja goes by file times). Mutes and `--seconds` don't. Psytrance round 2 spent about as long linking as emulating. Until #401 lands:
  - sort each runner's jobs by variant, then by Recipe, so equal ones run back to back;
  - apply a variant once before its group and revert it once after, not around each capture;
  - keep the base jobs on a runner whose worktree never takes a variant.
- **A shorter capture is the start of a longer one, bit for bit**, for the same build, card, Recipe and mutes. Give each such combination one capture, as long as its latest card needs, and cut every clip from it.
- gen_emu writes 32-bit WAVs, about 60 MB per 170 s. Keep them, the card images and the build dirs (735 MB each) in a scratch folder, not the session folder, and delete it when the page is done. Ambient rules 1 left 2.6 GB in its session folder for a 48 MB page.
- **At most six captures at once on the host**, across all sessions, as `SKILL.md` step 3 says. Each needs its own `--out` folder and its own `--build-dir`, built beforehand. Measured in #377 (85 s psytrance capture, M4 Pro, 12 cores): identical sha256 and a passing `check` at 1, 2, 4 and 6 at once; wall time per capture 31 s alone, 33 s at two, 42 s at four, 38 s at six; throughput 2.7, 5.0, 8.0 and 13.2 captured seconds per wall second. Queue the jobs in a file and run up to six at a time.
- The first capture into a freshly built `--build-dir` can rebuild the git-state version and relink, about 40 s, before its emulator starts (#377 saw it once per dir). Count it when you time captures, or run one throwaway capture per dir first.
- **Check each WAV after its capture**, and capture again any that fails:
  - the right length (gen_emu exits 0 even when its log says "only N s of audio rendered");
  - sound in every bar where the harness render has a note in a part the capture didn't mute (isolated captures are silent elsewhere, so check by bar);
  - no run of full-scale samples;
  - on a forced key, the right key. `gen_emu.py check` only checks that something sounds, so it passes a capture in the wrong key (#405 adds a pitch check). Until then, the Recipe check above catches a mangled job, and `tuning/ambient/rules-1/tools/pitchcheck.py` measures the notes of a bar range.

  With six emulators on the host (two sessions at three each), captures came back short, corrupt and partly silent, and none failed. Before capturing a bad one again, wait for the other session's emulator to finish. Round 1's `cap2.py` and `check_wav.py` do all this.
- A capture rebuilds from its worktree's source before it boots. Keep a table variant applied through its group of captures in that worktree, and revert it right after the group.
- Give two captures that might ever run at once separate `--out` folders and `--build-dir`s. gen_emu keeps its raw dump as `capture-boot1.s32` in the WAV's folder, deletes it when a capture starts, and reads it back by path at the end, so captures sharing a folder read each other's audio. It also writes the capture's Recipe header into the build dir. Round 1 lost 38 captures to a shared folder.
- **Clips:** cut with Python's `wave`, shift the 32-bit samples to 16-bit (`>> 16`), fade 10 ms at each end so a loop doesn't click, and encode with `afconvert -f m4af -d aac -b 160000`. Keep the captured level: a level is part of what the owner judges.

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
