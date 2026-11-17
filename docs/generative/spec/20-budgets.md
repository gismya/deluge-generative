# 20. Budgets and measured facts

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

Measured on a real OLED Deluge unless marked.

| Fact | Value |
|---|---|
| CPU | one Cortex-A9 at 400 MHz, cooperative scheduler. Ticks run inside the audio routine, which has about 2.9 ms of buffer (128 samples at 44.1 kHz) |
| Per-tick work | keep it in the tens of µs, with no card access and no allocation (estimate) |
| Task runs | keep each well under 3 ms, or split and yield. State read by the tick path must be consistent across yields |
| Voice starts | 4 per render window, 1 under heavy load. Extras wait one render: the audio routine targets 1.45 ms and is at most 2.9 ms. Above about 215 BPM a tick is shorter than a render, so the per-tick budget no longer holds. No fixed voice cap; culling is driven by CPU load |
| Fast heap free | 1002 K with no song; 749 K with a typical song; 694 K with that song plus a Perform roster playing (RelWithDebInfo, Release has about 46 K more) |
| Two songs in RAM | 591 K free at the swap with two typical songs, 273 K with two heavy ones (largest block 148 K). No late renders |
| Perform roster | about 162 K for the largest roster: 11 drum and 4 FX lanes at about 8.1 K plus 4 synths at about 10 K. Two rosters are about 323 K, which fits next to a heavy song (273 K free with two heavy songs is the closest measurement). Perform's song and the Generator blocks aren't in this figure yet |
| Steering log | one fixed 128 KB buffer in SDRAM, allocated on Perform entry ([§17.2](17-capture.md#172-the-steering-log)), not fast heap. A piece header is about 5–6 KB for the largest roster |
| Steering queue | 64 commands, fixed, in the Perform driver ([§2.6](02-architecture.md#26-steering-into-the-core)) |
| Kit lane | about 8.1 KB fast heap plus the sample in SDRAM. 10–15 ms per cold lane while playing; a 12-lane kit under 0.2 s |
| Synth preset load | 5.3 ms, 10 K fast heap |
| Entering Perform | 129–172 ms with Acid techno's roster, every sample read from the card (H9, below). Detroit techno's six parts are not measured yet (#251) |
| Silent incoming Kits | linked next to a playing heavy song, an 11-lane Kit costs 59 µs and a 4-row FX Kit 42 µs per 128-sample buffer: about 3.5% CPU together, with no late renders ([Measure silent incoming Kits and retiring Outputs on a Deluge][t83]). The cost is per render, so it rises when renders are short |
| Linking an incoming roster | 6–7 ms for three Outputs. The largest single link (clip, NoteRows and Output, no yield) took 1.6 ms. No late renders |
| Freeing a retired Output | `deleteClipObject` with `InstrumentRemoval::DELETE` while playing: 11-lane Kit 18–24 ms in 67 yields, FX Kit 6–10.5 ms in 21, Synth 1.7–3.6 ms in 5. At most 0.93 ms between yields, no late renders. The Output stays in the render list for about one yield per NoteRow first ([§12.9](12-runtime.md#129-instances-and-the-transition)) |
| Compose song | measured with a clip per part per Section occurrence: 1.7 MB for 4:00, 4.7 MB for 20:00, 15.7 MB for 60:00. Clip setup is 67–75% of it, about 130 K per occurrence (about 6 K per kit row, 11 K per synth clip). Notes are about 30 B each. So [§15](15-compose-output.md#15-compose-output-shape) uses one clip per part: about 0.7 MB for 4:00 and 1.8 MB for 20:00 (estimate). Freeing the current song doesn't help the fast heap: song structures are `allocMaxSpeed` and fill it first. XML parsing is 2–5× slower while audio runs |
| Saving and loading songs | a 1.0 MB file saves in 0.36 s and loads in 2.0 s. A 10.1 MB file saves in 3.4–7.55 s but fails to load on an empty Deluge with `FILE_CORRUPTED`, so Capture splits long pieces ([§17.5](17-capture.md#175-song-shape)) |
| Out of memory | once SDRAM is full, any `operator new` throws `BAD_ALLOC`, nothing catches it, and the device freezes with `TERM`. Compose and Capture stop at an SDRAM reserve, 512 K to start ([§15.9](15-compose-output.md#159-memory-and-failures)) |
| Pad redraw | 21–22 ms of UART per full redraw, shared with the OLED. Pad sends defer behind OLED updates. Any main-pad change sends all of them (`sendOutMainPadColours`), so a row mask saves render time, not UART. The SONG page in delugemu: 100 main redraws in an 8-bar Techno phrase with nothing queued (under one per 16th), 139 with queued actions blinking (about 9.4 a second; the ceiling is about 45) |
| Arc plan | `Generator::planArc` (16 phrases, 2,048 steps for Techno) takes about 0.55 ms in delugemu, 1.3 ms the first time; not yet recorded here from hardware (#251) |
| OLED | 128×43 visible, 4 rows of 21 characters or a title plus 3 rows. Redraws at most every 10 ms. `OLED::setupSideScroller` doesn't scroll while playing; the Compose title scrolls the way the stock session view scrolls a long song name |
| Tempo | the hardware plays exactly the set BPM (130.0000 by the sample clock). delugemu runs about 0.5% slow |
| Stability | the POC's hook ran 31 minutes with no leak, drift, stuck notes or late renders, and followed swing and tempo changes exactly |
| `new` | global `operator new` and std containers use only a 2 MiB SDRAM pool and throw when full. The Generator doesn't use them ([§2.1](02-architecture.md#21-the-generator-core)) |
| Song loads | the "buttons and pads" task can run up to 20 ms without yielding during a song load |
| System clock | `getSystemTime()` reads about 0.8% short against real time |

The M1 core is 13,308 B of state per Generator block on ARM (`sizeof(Generator)`), 3,076 B under the 16 KB `static_assert` (H18). `scripts/generative/generator_size.py` measures it with the firmware's compiler, and `check_all.py` fails until this paragraph states it. Perform's two blocks take about 26 K of fast heap. The capacities are sized by construction for the largest roster, Detroit techno's (Drums, Bass, Pad, Chords, Lead, FX), from one roster table: a 4-note voicing on Chords and Pad, one note on Bass and Lead. The parts:

- **Event buffer**, 10,072 B: 629 events of 16 B, with the buffer's keep priorities in spare bits of each event. It holds a step plus the step still playing, each with every note's hits, a note-off for each hit inside the step, and a note-off for every note already sounding. Only one of the two can be a bar line, so the re-strikes of late entry and the Section-line cut are counted once, and only the step being decided can hold Control events (each control each control set can move, 108 a step), since they play on its grid line. A Sound event per part fits in each of the two steps.
- **Part state**, 1,440 B: 6 parts of 240 B. A part holds its Loop or Motif line (2 B a step), each lane's pick with the bar it began on and room for a pick that waits out a freeze, its Answer and freeze state, its lane gates as a bitmask, and its held voicing (4 pitches and the size, with the Density state of each note place and the mute state in one byte).
- **Control sets**, 440 B: 20 sets of 22 B. A Drums part has one per lane as well as its kit-wide one. Each set keeps its Throw state and its drift walk's points, so a step draws only when the walk passes a point.
- **Sounding-note and late-entry tables**, 428 B each: one pool of 35 notes of 12 B, each part given what the buffer counts for its role.
- **Pending list**, 328 B: 27 commands of 12 B, one slot each for a key change, a force or End, and a transition, and a take, a mute, a solo and a sound slot per part (a `static_assert` in `generator.h` holds it).
- **Arc**, 76 B, with a hold that waits for a new roster's Groove. The other 96 B are the Generator's own fields: the Recipe without its sound picks (20 B), the takes, the voiced chord and the walk's state, the fill state, the instance's start step and each part's last landed sound.

Outside the block, all in SDRAM (`PLACE_SDRAM_BSS`), so the sound picks add no internal SRAM:
- The Current Recipe, 13,192 B: the Recipe, its sound picks and the copy a genre pick stages (2.7 K each), and two palette overrides (3.9 K each), one in use and one staged.
- The sound controls' bound params for each piece (where each sits, its home and range), 14,180 B: 12.7 K for 20 control sets and 1.4 K for the outgoing roster's Space params during a transition's Throws. It is sized from the [§11.2](11-sound-controls.md#112-mapping-per-engine) table for every control set moving every param it could, and finds a patch cable by its source and destination, not a pointer.
- Sound re-rolls' three sets of sound picks, 8 K: the picks a preload rolls, those of each part's sound waiting for its line, and each part's previous sound.
- Transitions: the pending Recipe and its sound picks (2.7 K), the roster a preload builds, and up to 24 retiring Outputs' handles twice over (the list and the free job's, 1.4 K and 1.3 K).

**H1, measured** on hardware over the four Techno subgenres (Acid, Melodic, Dub and Detroit techno; Release test-port builds, the Perform scripts of each subgenre's session; a 33.33 MHz OSTM count around each step decision in the tick, reported every 64 steps as `gen_h1`): a step decision costs 60–69 µs on average per session, at most 212–246 µs in a session. That is the "tens of µs" assumed, on average, so the phrase-line fallback of [§12.2](12-runtime.md#122-when-steps-are-decided) isn't needed. Re-measure as genres add parts and layers. Where the step's code lies across cache lines moves the figure by about 2 %: `docs/generative/implementation-notes/runner.md` says how it is pinned.

**H2, measured** on hardware with Techno's first sound-controls roster (Release test-port build, `scripts/generative/perform_sound_controls_h2.txt`, reported every 4 bars as `gen_h2`), over four sessions on one Deluge; a piece binds 27–28 params over 5 control sets (the Drums Kit's affect-entire params, the acid Bass, three FX rows). In the Intro, where Energy rises and Brightness, Decay, Drive and Space move together on 15–16 steps in every 4 bars, applying a step's Control events costs 25–27 µs on average and at most 32 µs in the tick, for about 20 params. The Cortex-A9 divides 64-bit values in software: with two such divisions per param, the same apply cost 33 µs. The ramps' render cost was found by playing the same piece twice, with the test port's `hold ramps` dropping every Control event the second time: per 128 samples, the Intro renders 80–119 µs slower with ramps (10–14%). A Break, where the sound stays shaded with nothing ramping, renders 62 µs slower (9%), so most of that is the shaded sound (longer releases), and the ramps themselves cost about 20–60 µs per 128 samples, 1–2% of the CPU. A Groove at home renders the same within 1 µs, so a param left flagged as interpolating after its ramp ends costs nothing measurable. The slowest render was 0.41 ms either way. The worst case, every bound param rewritten on every step (the test port's `hold stress`: all 28 params ramping halfway up and down their ranges in turn), costs 22 µs a step in the tick and about 280 µs per 128 samples in the render against the same piece with no ramps, in the Intro and the Groove alike: about 10 µs per 128 samples, 0.35% of the CPU, for each param ramping all the time, with the slowest render at 0.59 ms. So at the macro layer's pace the ramps cost little, and no phrase-line or coarser grain is needed. Budget about 0.35% of the CPU per param a layer keeps ramping.

**Drift and gestures, measured** on hardware with Techno's roster before the subgenres (Release test-port build, one load, `perform_sound_controls_h2.txt`, `perform_skeleton.txt`, `perform_transition.txt`). Techno bound 12 control sets and 60–63 params. In a Groove at the default macros only drift moves: 6.3–7.5 params armed a step, and the render costs 65–90 µs more per 128 samples than the same piece under `hold ramps` (about 2.2–3.1% CPU, 0.35–0.41% per param that keeps ramping). The Intro costs 127–130 µs more and a Break's first windows 109–131 µs. Applying a step's Control events takes 13–20 µs on average, 92 µs at most on the Intro's first step. No render culled a voice (`gen_h16 culls=0`), and the slowest render in any window was 617 µs.

**H9, measured** on hardware (Release, the `gen_entry done` event; entry as a modal runner job, timed from the OK press to Perform's song swapped in) with Acid techno's roster, over the blank song leaving lands on: 129–172 ms over two runs. Every entry reads its samples from the card: leaving empties the sample cache (below). Detroit techno's entry, with six parts, and an entry while a heavy song plays are not measured: no script enters Perform with Detroit selected yet (#251). BACK 5 ms into a cold build cancels it, and the job has unwound 12 ms after the OK. Leaving takes 16–17 ms, 0.7–1.2 ms of it (`gen_left`'s `delete_us`) deleting Perform's song and every unused cached sample. With a large sample cache it is no slower: after loading Ttt (`perform_leave_heavy.txt`: 3.4 MB of Ttt's samples still cached when Perform's entry deletes the song), leaving took 16 ms, 1.5 ms of it deleting.

**H16 and the transition, measured** on hardware with Techno's roster before the subgenres (Release test-port build, one load, `perform_transition.txt` and `perform_transition_input.txt`). No render culled a voice in any 4-bar window of the session (`gen_h16 culls=0`), around each transition's Drop, seam and cut line included, so neither Kick nor Bass was culled; the slowest render in a window was 193–422 µs (no late renders). A Regenerate's preload (roll, every sound re-rolled away from the playing one, three Outputs built and linked) took 178–224 ms; a BACK tap cancelled one 115 ms in. During preloads every press was taken within 9 ms of being due (`input_lag`), and gold-knob edges kept their 160 ms pace (no card wait held one back). A retiring Output still sounding at its cap faded over a beat and was freed. Over each enter, Regenerate and leave cycle both heaps came back to the byte. The start step's work in the tick (the roster swap, the Current Recipe, binding the sound controls; `gen_transition start ... us=`, Release test-port build) takes 125–130 µs on average per session, single start steps 96–142 µs. Most of it is cache misses in two copies of cold SDRAM data; `docs/generative/implementation-notes/runner.md` says why and what keeps it from growing.

**H16 on the Techno subgenres**, measured on hardware with H1 above: Acid, Melodic and Dub techno culled no voice. Detroit techno culls 1–3 voices per session: one with 12 voices sounding (render 1.34 ms, the Low-priority Strings first, never Kick or Bass), three at a genre change into Detroit (render 1.5 ms; that these spare Kick and Bass too is expected from the voice priorities, not yet seen: confirm with `gen_h16 culled=` on hardware, #251). `gen_h16 culled=` names the last culled voice's part in test builds, to see which part goes.

**H17, measured** on hardware (Release test-port build, b395ec5d0; Techno, `perform_midi.txt`, MIDI follow's default mappings, feedback at the HIGH rate, 40 ms). Feedback sends a value only when it moved since it was last sent (§13.7), so while a piece played with ACID focused the steady windows sent 361–412 B a second on DIN's 3 B a CC, 12–13 % of the 3125 B/s link, at most 7 CCs a pass; later windows sent 14–109 B/s. A context change (a focus, entry) sends every mapped param and the macros in one pass, 84 CCs (252 B): its window sent 600 B/s, as stock sends one. delugemu gave 431–568 B/s, at most 8 CCs a pass, the same with every sound-control param re-ramped each step (`hold stress`). Stock's own automation feedback re-sends every automated param each pass, unfiltered. The cost of sending only moved values: a controller that drops a message, or is plugged in mid-set, shows a stale value until the next context change.

**Leaving frees what entry built.** After an enter/leave cycle the internal and external heaps are exactly where they were before entry. The first cycle after boot is the exception: it costs 320 B of internal heap once (the sample cache's bookkeeping), and on hardware it leaves 296 B more external heap free than before, since that leave also deletes the samples cached before entry. Two rules give this:
- Leaving deletes every audio file no song uses, before it builds the blank song: the samples Perform loaded, and the previous song's cached samples too. Stock firmware keeps unused samples as a stealable cache, which still holds heap, and each entry picks new sounds. The cost is card reads: entering again, or loading the old song back, reads its samples from the card.
- Building the blank song leaks nothing: a stock Browser leak lost 96–104 B of external heap on every SHIFT+LOAD → NEW (`docs/upstream-candidates.md`).

**Assumptions measured during implementation.** These rest on assumptions that needed Generator code before they could be measured. Each has a test design with a pass threshold in the [technical review][tech-review]. The last column says where each was measured, or the milestone that measures it:

| Test | Assumption | When |
|---|---|---|
| H1 | a step decision costs tens of µs in the tick (the headroom knee can be found earlier, see the map) | M1: measured, above |
| H2 | live ramps on every targeted param, rewritten each step, cost little | M1: measured, above |
| H9 | entering Perform with a full roster while a heavy song plays takes under 1 s | M1: measured with Acid techno's roster (above); still open for Detroit's six parts and with a heavy song playing (#251) |
| H13 | 128 KB of steering log holds about 10 marked pieces with busy steering | M4. Perform has no steering log or counter yet: the Perform driver's log argument is `nullptr` (`perform_adapter.cpp`, `driver_.advance()`) |
| H15 | 12 ticks of extra steering latency is acceptable | M1 |
| H16 | CPU and voices around a Drop don't cull Kick or Bass | M1: measured, above. Detroit culls 1–3 voices a session, not Kick or Bass (at a genre change: expected, to confirm with `culled=`, #251) |
| H17 | MIDI feedback of live values fits the link | M1: measured on hardware, above |
| H18 | the Generator state block stays small (a `static_assert`) | M1: the `static_assert` in `generative/core/generator.h`, 16 KB |
| H19 | the largest saved song that loads back on an empty Deluge, and the [§15.9](15-compose-output.md#159-memory-and-failures) size estimate against a real Compose build. Sets the SDRAM reserve and the Capture split budget | M3, M4 |

**Raising H18's limit** (the user's rule). The 16 KB `static_assert` is a tripwire, not a hardware limit: Perform
allocates two Generator blocks from the fast heap. A change that doesn't fit first audits the block for wasted space
and fixes what it finds: field sizes, padding, capacities above their derivation, and stored state that could be
derived. Only then does it raise the limit, by 4 KB at a time, updating the H18 row and the size breakdown above, and
its PR reports the audit.

Sources: [Runtime budget on the Deluge][t3], [Measure memory and load times on a Deluge][t37], [Hardware tests of the Perform POC][t45], [Deluge UI conventions and free controls][t33], POC findings on the [map][poc], [Interface between the Generator core and its drivers][t75], [Compose and Capture memory, limits and failures][t78], [Measure building and saving Compose- and Capture-sized songs on a Deluge][t62].

[t3]: https://github.com/gismya/deluge-generative/issues/3
[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t33]: https://github.com/gismya/deluge-generative/issues/33
[t37]: https://github.com/gismya/deluge-generative/issues/37
[t45]: https://github.com/gismya/deluge-generative/issues/45
[tech-review]: https://github.com/gismya/deluge-generative/blob/research/spec-review/docs/research/spec-review-technical.md
[t75]: https://github.com/gismya/deluge-generative/issues/75

[t78]: https://github.com/gismya/deluge-generative/issues/78
[t62]: https://github.com/gismya/deluge-generative/issues/62
[t83]: https://github.com/gismya/deluge-generative/issues/83
