# Silent incoming Kits, retiring Outputs and tail checks

Ticket: [Measure silent incoming Kits and retiring Outputs on a Deluge](https://github.com/gismya/deluge-generative/issues/83),
backing [§12.9](../generative/spec/12-runtime.md#129-instances-and-the-transition).

## What ran

- **Hardware:** OLED Deluge, RelWithDebInfo test build (Perform POC, MEAS probes, test port, plus the probes below)
  loaded with `loadfw`. No hands on the device during runs.
- **Load:** the user's heavy song **Ttt** (5 synths, 4 kits) plays under the Perform POC at Energy 50 / Density 50 and
  **175 BPM**. The Build roll is stood in for by a Clap-lane hit on the POC kit every 6 Generator ticks (four a 16th),
  velocity climbing over each bar (`probe roll 6`). The POC has no Build section.
- **Probes** (`src/deluge/generative/debug/hw_probe.cpp`, section "#83"):
  - `tlink seed synth`: builds the largest Drums kit (11 lanes rolled from their folders), the largest FX kit (2 sample
    lanes, 2 `loadSynthToDrum` rows) and, with `synth`, the card's largest pad, unlinked. Then links each with
    `perform::linkWithClip` (empty, always-active 1-bar clip, NoteRows for a Kit), the end of Preloading.
  - `rwin 0|1`: times every `renderAudio()` call and each probe Output's `renderOutput()` (hooks in
    `AudioEngine::routine_()` and `Song::renderAudio()`), with late renders (gap between audio routine entries > 2.9 ms)
    and the longest gap.
  - `tretire mask`: frees probe Outputs as stock `SessionView::removeClip` does, with `InstrumentRemoval::DELETE`:
    `stopAllNotesPlaying`, `midiFollow.removeClip`, `removeSessionClipLowLevel`, `deleteClipObject`. Counts audio
    routine entries during the free (the yields), how many of them came while the Output was still in
    `song->firstOutput`, the longest gap and late renders.
  - `ttail kind fb hold`: one hit on the Synth (kind 0), or on the Drums kit's Snare row with the kit's delay (1) or the
    row's own delay (2). Delay feedback starts at 0, goes to `fb` (menu units) just before the hit (the Throw), holds
    `hold` ms, then falls to 0 over 1 beat in 16 steps (`hold` -1: never falls). Every render it evaluates §12.9's
    silence check (`skippingRendering` for the Synth; for the Kit `drumsWithRenderingActive` empty, kit delay
    `repeatsUntilAbandon` 0, no arp input notes, not stuttering) and measures the largest sample the Output added to
    the mix, in 20 ms buckets. Reverb isn't counted: it's the song's, not the Output's.
- **Driver:** `scripts/generative/spec_hw_tests.py 83 --song Ttt --parts silent,retire|tail`.
- **Logs:** [`t83-silent-retire.log`](logs/t83-silent-retire.log), [`t83-tail.log`](logs/t83-tail.log) (its first two
  runs stopped on driver bugs after one tail; the third is complete), [`t83-tail-natural.log`](logs/t83-tail-natural.log).

## 1. Silent incoming Kits

Two silent Kits linked beside the playing roster, 10 s windows, three seeds (11, 23, 37), each as off / on / on / off.

| | Without | With 2 silent Kits | After retiring them |
|---|---|---|---|
| `renderAudio()` per 128 samples | 1,745–1,751 µs | 1,803–1,815 µs | 1,745–1,750 µs |
| Drums kit `renderOutput()` (11 lanes) | – | 11.2 µs a render, **59 µs per 128 samples** | – |
| FX kit `renderOutput()` (4 rows) | – | 8.0 µs a render, **42 µs per 128 samples** | – |
| Longest render | 586–613 µs | 640–688 µs | |
| Late renders | 0 | **0** | 0 |

- The two silent Kits cost **about 100 µs per 128-sample buffer** (2,902 µs of real time), about 3.5% of the CPU.
  The whole render rose by 55–66 µs (3.6%), less than the Kits' own time, because with them renders come fewer and
  larger, so every other Output's per-render overhead shrinks.
- The cost is per render, not per sample: 11.2 and 8.0 µs whatever the render's length. With a light load renders are
  short (about 13 samples), and the same Kits cost 139 µs per 128 samples (no user song: 960 → 1,090 µs).
- Linking both took 6–7 ms, the longest single link (no yield) 0.8–1.6 ms. 0 late renders.
- **Pass** against no late renders. The fallback (link Kits late) isn't needed for CPU.

## 2. Retiring while playing

Each Output freed while Ttt and the POC play. "Played" means every drum and the synth note got a hit 3 s before.

| Output | Total | Yields | Longest gap | Late | Renders while still listed | Freed |
|---|---|---|---|---|---|---|
| Drums kit, 11 lanes (6 runs) | 18–24 ms | 67 | 0.64–0.84 ms | 0 | 12 | 93 K fast |
| FX kit, 2 samples + 2 synth rows (6 runs) | 6–10.5 ms | 21 | 0.34–0.93 ms | 0 | 5 | 37–38 K fast |
| Synth, largest pad, still ringing (3 runs) | 1.7–3.6 ms | 5 | 0.48–0.92 ms | 0 | 1 | 17 K fast, 198 K SDRAM |

- No more than 3 ms between yields: **pass** (longest 0.93 ms, render time included).
- No late renders: **pass**.
- **Finding: stock `removeClip` doesn't unlink the Output before its first yield.** `Clip::prepareForDestruction`
  detaches the clip first, and `InstrumentClip::unassignAllNoteRowsFromDrums` (`instrument_clip.cpp:2145`) yields once
  per NoteRow while the Kit is still in `song->firstOutput`. Only then does `deleteOutputThatIsInMainList` take it out,
  and `~Kit` yields once per drum after that. The Output is rendered, with no active clip, through 12 yields for an
  11-row Kit (5 for 4 rows, 1 for a Synth). Nothing went wrong here, but §12.9's "unlinked from rendering before the
  first yield, as stock `SessionView::removeClip` does" is wrong. To unlink first, Perform would call
  `removeOutputFromMainList` before `deleteClipObject`.

## 3. Tail checks

Silence check against the Output's own signal. "−80 dB" is the end of the last 20 ms bucket within 80 dB of the run's
loudest bucket. Hits peaked at −32 dBFS (Snare) and −40 dBFS (pad), so −80 dB is far below hearing.

**After a Throw** (feedback spikes, holds 1 bar = 1,371 ms, falls to 0 over 1 beat, ending at about 1,710 ms):

| Throw depth | Synth: silent / −80 dB | Kit delay | Row delay | `repeatsUntilAbandon` while held |
|---|---|---|---|---|
| 20 | 1,759 / 1,720 ms | 1,688 / 1,680 ms | 1,709 / 1,680 ms | 40 |
| 30 | 1,759 / 1,720 ms | 1,688 / 1,700 ms | 1,709 / 1,720 ms | **255** |
| 40 | 1,760 / 1,720 ms | 1,689 / 1,700 ms | 1,709 / 1,720 ms | **255** |

**Natural decay** (feedback fixed, no fall):

| Feedback | Synth: silent / −80 dB | Kit delay | Row delay | repeats |
|---|---|---|---|---|
| 0 | 1,116 / 1,060 ms | 252 / 260 ms | 252 / 260 ms | 0 |
| 10 | 4,035 / 4,000 ms | 684 / 700 ms | 683 / 700 ms | 6 |
| 20 | 23,374 / 23,340 ms | 3,590 / 3,580 ms | 3,590 / 3,580 ms | 40 |
| 50 (max) | never in 30 s | never in 30 s | never in 30 s | 255 |

- The check reports silence **within one 20 ms bucket of the last sample within 80 dB of the peak**, in every case
  that ends: **pass**. It never flipped back to sounding.
- At maximum feedback it never reports silence (30 s, `repeatsUntilAbandon` stuck at 255), so the one-phrase cap is
  needed: **confirmed**. The pad's echo climbed to 0 dBFS.
- **A natural tail can outlast a phrase without runaway feedback.** The pad at feedback 20 (40 repeats of its own
  delay time) was audible for 23 s. At 175 BPM an 8-bar phrase is 11 s, so the cap also ends ordinary long tails.
- **Finding for the Throw depth ([§11.4](../generative/spec/11-sound-controls.md#114-gestures)):** at depth 30, the
  spec's cap, `repeatsUntilAbandon` is already 255, the never-abandon value. The echo also regenerates while held:
  the Kit's rose 18 dB above the hit over the 1-bar hold (to −12 dBFS), the row's 26 dB (to −6 dBFS), the pad's 5 dB.
  What ends a Throw's tail is the fall back to home, which drops feedback below the delay's threshold and cuts the
  echo. "Capped at +30, below the firmware's own feedback clamp" doesn't keep a held Throw from growing. The
  never-abandon line lies between depth 20 (40 repeats) and 30.

## Other notes

- **Link order.** §12.9 says linking follows stock `SessionView::createNewInstrumentClip` with "`setActiveClip` before
  `addOutput`". Stock `createNewTrackForInstrumentClip` does `addOutput` first, then `setActiveClip`, with no yield
  between, so nothing renders half-patched either way. The POC's `linkWithClip` does the same.
- **Loading this build.** `loadfw` straight over the SD-flashed build crashed twice (undefined instruction at a
  valid `smmulr` in `LpLadderFilter::doFilterStereo`, sidebar commit `f30c`). Loading the base build
  (`ab85726f2`) first and this one on top worked every time. The same build boots and passes the smoke script in
  delugemu. The cause wasn't found.

## Left out

- The Build roll is a Clap roll on the POC kit, not a real Build. The playing roster is the POC's plus Ttt, not a
  Generative maximum roster.
- The Throw's reverb spike isn't applied: reverb is the song's, so it doesn't decide when an Output can be freed.
- No `NonAudioDrum`s or arp were in the Kits, so those parts of the check were never false.
