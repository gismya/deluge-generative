# Rendering a listening session

How to render a session's clips, and the traps the first session (Psytrance, round 1) hit. Read it before the first capture.

Start from that session's scripts in `~/.cache/deluge-generative-m2/tuning/psytrance/round-1/tools/`:
- `content.py`: the cards, clips and held questions;
- `cap2.py`: the capture runner. Its `check_wav.py` is now `gen_emu.py check`, which `capture` runs itself;
- `variants.py`: the table edits;
- `song.py` and `normal_cap.py`: a normal-mode song, and its capture;
- `build_session.py` with `page.html`: it cuts the clips and writes `index.html` and `variants.md`.

Swap in the new genre's content, and keep the rest.

## Before any capture: the harness

Check every question in the harness first: a capture takes a minute, and the harness takes a second.

- Build `generator_render` with `./dbt test --generator -n`. For each candidate Recipe, render with `--end-after-drop --midi-only`, and read the MIDI file with `theory_review.piece_from_midi(open(path, "rb").read())`. A piece holds its sections (bar, label), the tempo and key, and each part's notes (`velocity`, `end`, drum lanes by note).
- **Pick seeds and macros where the ticket's behaviour shows**: the section the question is about, the pattern entry it names, the Bass velocities it quotes. `--motion 50` brings the Break, Build and Drop sooner (Psytrance: the Drop at bar 80 instead of 160).
- **Prove each table variant changes what the ticket says**, before you build it into firmware. Apply the edit, rebuild the harness, copy `generator_render` aside per variant, revert, and compare the variant's render with the base render (`cmp` on the MIDI files over 20+ seeds, or count the notes). Tickets from a theory review can misread the table. Psytrance round 1 found three such cases:
  - an option that changes nothing at the Complexity the ticket names: `ByComplexity::at()` interpolates, so `{2, 6}` and `{4, 6}` both give a leap of 5 at Complexity 25 or less;
  - a pattern entry that sounds the same as the ticket's fix, because its `7`s already play at the default Density, while the behaviour came from another entry;
  - a "sparse" option that took the part out entirely.

  When the question as written can't be heard, render the nearest audible version, and say why on the card.
- **Palette picks per entropy:** a WAV render (not `--midi-only`) prints `warning: PART: SYNTHS/… not modelled` for each preset it loads. A part only loads once it plays, so add `--density 50 --bars 20` to see the Lead's. Scan entropies 0–30 to find one per pick. Each entropy also rolls its own key and tempo.

## Captures

- The genre must be on the branch you capture from. M2 genres were on `m2-all-genres`, not `generative`.
- **Pack card:** clone the stock image, `cp -c ~/source/delugemu-sd/deluge_sd.img card.img` (an APFS clone, instant), then `fat_image.copy_tree(FatImage("card.img"), "pack")`.
- A capture's audio starts at PLAY, which is the harness's bar 0. Cut clips by bar, at the capture's tempo (`240 / bpm` s a bar). Check it once: the Build's Gap is a silent beat right before the Drop.
- `--mute` rows follow the genre's part order (Psytrance: 1 DRUMS, 2 BASS, 3 DRONE, 4 LEAD, 5 ARP, 6 FX). A drum lane can't be soloed. An isolated capture is silent where the muted parts would play; the check below allows for it.
- Costs on an M4 Pro: an 85 s capture takes 35–50 s, a 170 s one about 65–85 s, and a fresh `--build-dir` adds a 2-minute full build. gen_emu writes 32-bit WAVs, about 60 MB per 170 s. Keep them in the scratchpad.
- **One capture at a time** (at most two delugemu on the host, one per session, as `SKILL.md` step 3 says). Queue the jobs in a file and run them in order.
- **Each capture checks its WAV** (`gen_emu.py check`, run by `capture` unless `--no-check`), and exits 1 if it fails. Capture again any that fails. After the firmware, `capture` builds the harness's `generator_render` from the same source (only that target, in `build/tests/`) and renders the same Recipe, so a table variant's capture is checked against the variant's notes. It fails a WAV that:
  - is shorter than `--seconds`;
  - is silent for the beat after a 16th where the harness render of the same Recipe starts a note in a part the capture didn't mute (so isolated captures and sparse sections such as Dub's Intro pass, and a dropout shorter than a bar fails);
  - is at full scale for 50 ms in a row (clipped flat or corrupt). A shorter run is a WARNING: the mix clips.

  To check WAVs made before, or by hand: `gen_emu.py check WAV... ` with the capture's Recipe flags, `--seconds` and `--mute`s. It reads the newest `generator_render` under `build/tests/` (or `--render`), logs which, and warns if the Generator's source changed after it was built. With six emulators on the host (two sessions at three each), captures came back short, corrupt and partly silent. Before capturing a bad one again, wait for the other session's emulator to finish.
- A capture rebuilds from its worktree's source before it boots. Apply a table variant just before its capture, and revert it right after.
- Give two captures that might ever run at once separate `--build-dir`s: gen_emu writes the capture's Recipe header into the build dir before it builds. They can share an `--out` folder: each capture names its raw dump after its WAV and its process, and claims its WAV's name when it starts. (Round 1 lost 38 captures to a shared folder, before that.)
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
