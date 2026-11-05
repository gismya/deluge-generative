# H1, H3, H4: decision headroom, voice starts and tempo glides

Tests H1 (the headroom knee only), H3 and H4 of the spec review's [hardware tests](https://github.com/gismya/deluge-generative/blob/research/spec-review/docs/research/spec-review-technical.md#performance-assumptions-and-hardware-tests),
for [Measure step-decision headroom, voice starts and tempo glides on a Deluge](https://github.com/gismya/deluge-generative/issues/58).
Run on 2026-11-06 on the OLED Deluge over the sysex test port, with no hands on the device.

## Setup

- **Build:** this branch: the earlier stand-in builds (Perform POC, MEAS probes, test port) plus probe hooks in
  `PerformDriver::doTickForward()` (after `Generator::tick()`), `Sound::noteOnPostArpeggiator()` and the voice culling.
  RelWithDebInfo, loaded with `loadfw`.
- **Load:** the user's song **Ttt** loaded through the LOAD browser, its first 8 session rows switched on (it was saved
  for arrangement playback), then the Perform POC entered on top at Energy 50, Density 50, 130 BPM. About 11 voices,
  and render gaps up to 1.3 ms with nothing added. For a heavier CPU the probe can also spend a fixed time in every
  audio render (`renderburn`): 500 µs gives `cpu_direness` 9–14 and 1000 µs holds it at 14.
- **Driver:** `scripts/generative/spec_hw_tests.py h134` (H1 sweep, H3, soak; log `logs/h1-h3-h4.log`, printed
  summary `logs/h1-h3-h4-summary.txt`) and `h1fine` (a finer H1 sweep and H4 again; `logs/h1-fine-h4.log`).
- The probe file in this commit also holds the next tickets' stand-ins (lane rolls, roster preload, Compose-shaped
  songs). The `h134` run used a build without them; the `h1fine` run used this commit's code.

## H1: the headroom knee

The probe spins for a calibrated time on every hook call that decided a step (`Generator::stepsDecided()` went up),
which is once per 16th: 174 decision ticks per 20 s window. It runs where the real decision would, in the playback
routine task. A render is late when it comes more than 128 samples (2.9 ms) after the one before.

| Render burn | Busy per decision | Late renders / 20 s | Longest render gap | Max direness | Culled |
|---|---|---|---|---|---|
| 0 | 0 µs | 0 | 1.28 ms | 0 | 0 |
| 0 | 100 µs | 0 | 1.30 ms | 0 | 0 |
| 0 | 200 µs | 0 | 1.35 ms | 0 | 0 |
| 0 | 300 µs | 0 | 1.50 ms | 0 | 0 |
| 0 | 500 µs | 0 | 1.82 ms | 0 | 0 |
| 0 | 750 µs | 0 | 2.19 ms | 4 | 0 |
| 0 | 1000 µs | 0 | 2.60 ms | 0 | 0 |
| 0 | 1500 µs | 53 | 3.27 ms | 14 | 0 |
| 500 µs | 0 µs | 0 | 1.88 ms | 9 | 0 |
| 500 µs | 300 µs | 0 | 2.14 ms | 14 | 0 |
| 500 µs | 500 µs | 0 | 2.29 ms | 14 | 0 |
| 500 µs | 750 µs | 0 | 2.86 ms | 14 | 0 |
| 500 µs | 1000 µs | 11 | 3.26 ms | 14 | 0 |
| 500 µs | 1500 µs | 144 | 3.87 ms | 14 | 0 |

A finer sweep around the knee (`h1fine`, a fresh boot, the same load):

| Render burn | Busy per decision | Late renders / 20 s | Longest render gap |
|---|---|---|---|
| 0 | 1000 µs | 0 | 2.62 ms |
| 0 | 1100 µs | 0 | 2.79 ms |
| 0 | 1200 µs | 3 | 2.94 ms |
| 0 | 1300 µs | 11 | 3.09 ms |
| 0 | 1400 µs | 14 | 3.20 ms |
| 500 µs | 700 µs | 0 | 2.68 ms |
| 500 µs | 800 µs | 0 | 2.83 ms |
| 500 µs | 900 µs | 5 | 3.04 ms |

The render gap grows about one for one with the busy time: the playback task holds the CPU and the audio routine
waits. **The knee is about 1.1–1.2 ms per decision tick** with Ttt and Perform playing, and **about 0.8–0.9 ms** with a 500 µs render burn on top.

**Soak:** 30 minutes at 100 µs per decision tick (the H1 pass line for the real decision), with Ttt and Perform
playing: **0 late renders**, longest render gap 1.28 ms, direness 0, 0 culled voices.

| Threshold | Result | |
|---|---|---|
| Worst decision tick ≤ 100 µs and ≤ ⅓ of the knee | ⅓ of the knee is 370–400 µs with no burn and 270–300 µs at the heavier load, so the 100 µs line is the binding one. The real tick's cost isn't measured yet | Knee found; the real cost waits for M1 |
| 0 late renders over 30 min | 0, with a 100 µs stand-in decision | **Pass** for the stand-in |

## H3: voice starts under overflow

On the first tick after each downbeat the probe sends 8 or 12 note-ons in §12.6 role order: the roster's Kick, Clap,
closed and open hat, then 2 or 4 of Ttt's kit rows, the roster's Bass, then 1 or 3 of Ttt's synths. It notes how many
renders passed before each note's voice started (`Sound::noteOnPostArpeggiator`) and in what order they started.
About 10 bursts per setting.

| Burst | Direness | Kick | Clap | Hats | Ttt rows | Bass | Start order |
|---|---|---|---|---|---|---|---|
| 8 notes | 0 (4 starts a render) | 0 | 0 | 0, 0 | 0–1 each | **0–1** | send order but for the two Ttt rows swapped |
| 12 notes | 0 | 0 | 0 | 0, 0 | 0–2 | **1–2** | the first 4 on the tick, then Bass, then Ttt's rows from last to first |
| 8 notes | 14 (1 start a render) | 0 | 5 | 4, 3 | 2, 1 | **6** | Kick, then the pending notes from **last to first** |
| 12 notes | 14 | 0 | 8 | 7, 6 | 5, 4, 3, 2 | **9** | Kick, then last to first, Bass after all of them |

Renders are 128 samples (2.9 ms) apart, so the Bass waited up to 5.8 ms at no load and 17–26 ms under heavy load.
Ttt's synth notes are left out of the table: their voices started 40–125 renders later or were never seen, which
looks like those presets' own behaviour (an arp or a sustained voice being re-used), not the start limit.

| Threshold | Result | |
|---|---|---|
| Starts follow send order | Only the notes that fit the per-render limit do. Pending notes start in the order the audio engine renders Sounds, which here was the reverse of the send order | **Fail** |
| Kick and Bass never delayed | The Kick never was. The Bass was delayed 1–2 renders at no load and 6–9 renders under heavy load | **Fail** for the Bass |
| Any other note waits at most one render each | Each pending note starts one render after the one before it, so this holds per note | Pass |

## H4: tempo glides

The probe writes song tempo with `Song::setTempoFromNumSamples(…, false)` on every Generator tick while a glide runs
(§12.7), asking for a hook call every tick, and logs each 16th's sample time and tempo. Glides start on a bar line:
130→170, 170→100 and 100→130 over 1 bar, then 130→170 and 170→100 over 1 phrase (8 bars). Ttt and Perform played at
Energy 50 / Density 50. The host listened to the Deluge's MIDI clock out on USB (switched on in RAM for the run, then
put back). The analysis is `scripts/generative/h4_analyse.py`.

| Glide | Steps logged | Skipped / repeated | Each step against its written tempo | Position against the analytic curve, worst | Clock out (6-clock means) |
|---|---|---|---|---|---|
| 130→170, 1 bar | 34 | 0 / 0 | −0.21 to +0.08 ticks | 1.15 ticks early | 533 clocks, 13.7–19.3 ms, 0 reversals |
| 170→100, 1 bar | 33 | 0 / 0 | −0.21 to +0.05 | 0.72 early | 449 clocks, 14.3–25.1 ms, 0 reversals |
| 100→130, 1 bar | 34 | 0 / 0 | −0.16 to +0.06 | 0.90 early | 533 clocks, 18.2–25.1 ms, 0 reversals |
| 130→170, 1 phrase | 146 | 0 / 0 | −0.33 to +0.12 | **11.5 early** | 1330 clocks, 13.2–19.5 ms, 0 reversals |
| 170→100, 1 phrase | 146 | 0 / 0 | −0.18 to +0.05 | **5.1 early** | 1309 clocks, 13.9–25.1 ms, 0 reversals |

0 late renders and 0 skipped or backward steps over all 45 bars of glides (longest render gap 1.44 ms). After each
glide, steps were exact to the sample at the new tempo.

| Threshold | Result | |
|---|---|---|
| 0 skipped or repeated steps | 0 | **Pass** |
| Each step within ±1 tick | Each step's length is within 0.33 ticks of the tempo written for it. But the position drifts ahead of the analytic curve by about 0.08 tick a step, always early: 0.7–1.15 ticks after 1 bar, 5–11.5 ticks after a phrase | **Fail** over a phrase (and 1.15 on one 1-bar glide) |
| Clock-out intervals monotonic | Monotonic in 6-clock means: 0 reversals. Single USB intervals jitter | **Pass** (smoothed) |
| 0 late renders | 0 | **Pass** |

The drift's cause isn't pinned down. The steps alternate between slightly long and slightly short in pairs, which
suggests a tempo write also changes the timing of the firmware tick already in progress. A first H4 run, in the H1 run's log, lost
records to the firmware's event queue (the driver asked for them too fast); its numbers aren't used. The table is
from the `h1fine` run.

## What it means

- **§12.2: one tick can decide a whole step** if the real decision costs well under the knee. With the knee at about
  1.1–1.2 ms (0.8–0.9 ms under heavy load), H1's own pass line (≤ 100 µs and ≤ ⅓ of the knee) leaves room. Whether the real Generator stays under
  100 µs can only be timed once the M1 skeleton exists, so the §12.2 fallback isn't needed yet but isn't ruled out.
- **§12.6: role priority doesn't protect the Bass.** The send order only decides which notes fit into the current
  render (4, or 1 under load). Notes past that start in the audio engine's own order. Under load the Bass, which
  §12.6 sends after every drum, waits for all of them. To protect the Bass, it would have to go right after the Kick
  in the send order, and the firmware would need to start pending voices in the order they were asked for.
- **§12.7:** writing tempo every tick is safe: no skipped steps, no late renders, smooth clock out. It isn't
exact: over a phrase the glide runs up to 11.5 ticks (about 70 ms at 170 BPM) ahead of the linear curve. If a glide
must land exactly on a bar line, the driver should correct against the master clock rather than trust the curve, or
§12.7 should accept that tolerance.

## Left out

- H1 measures the knee only, with a busy loop standing in for the decision. The real `generator.tick` cost at every
  macro on 50 with a Build roll and Ornaments waits for the M1 skeleton.
- H3's "dense pads" load is stood in for by Ttt plus a render burn. The heavy-load bursts ran at direness 14, the
  firmware's 1-start-per-render case.
- Nobody listened.
