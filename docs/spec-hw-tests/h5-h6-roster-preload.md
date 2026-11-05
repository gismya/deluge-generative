# H5, H6: a full roster preload and lane rolls

Tests H5 and H6 of the spec review's [hardware tests](https://github.com/gismya/deluge-generative/blob/research/spec-review/docs/research/spec-review-technical.md#performance-assumptions-and-hardware-tests),
for [Measure a full roster preload and lane scans on a Deluge](https://github.com/gismya/deluge-generative/issues/59).
Run on 2026-11-06 on the OLED Deluge over the sysex test port, with no hands on the device.

## Setup

- **Build:** this branch (the same probe code as `task/step-timing-measurements`), RelWithDebInfo, loaded with
  `loadfw`. Every `loadfw` reboots, so the first scan after it is cold.
- **Lane roll** (`probe laneroll`, `hw_probe.cpp`): §10.2–10.3 as written. Every file in the lane's folders under
  `SAMPLES/DRUMS` (Perc: the 10 folders Shaker … Other; Tom: TomL, TomM, TomH), audio extensions only, an optional
  tag (`808`) matched anywhere in the name, one picked at random. It yields (`AudioEngine::routineWithClusterLoading()`)
  after every folder and every 32 entries, and times the longest stretch between two yields, the slowest
  `f_opendir()` and the slowest `f_readdir()`.
- **Roster preload** (`probe roster`): the largest roster. 11 drum lanes, each rolled from its folders, built into one
  Kit on Perform's kit code (`buildSampleKit`). An FX kit with 2 sample lanes and 2 synth rows through
  `StorageManager::loadSynthToDrum()`. 4 synths through `loadInstrumentFromFile()`: the largest preset with "pad" in its
  name (`153 FM Modulation Pad`, 6 KB of XML) and 3 others picked by seed. Then it waits for every sample cluster, takes
  the memory at the peak and deletes it all. All of it stays unlinked, as a preload next to the playing roster would.
- **While playing:** the user's song **Ttt** (5 synths, 4 kits, 43 samples, the heavy one from #37) with its session
  rows on, plus the Perform POC at Energy 50 / Density 50 and **175 BPM**: about 10 voices. That stands in for "a
  maximum roster plays".
- **Driver:** `scripts/generative/spec_hw_tests.py h6cold` (right after boot, stopped) and `h5` (Ttt and Perform
  playing). Logs: [`logs/h6-stopped.log`](logs/h6-stopped.log), [`logs/h5-h6-playing.log`](logs/h5-h6-playing.log),
  [`logs/h5-preloads.log`](logs/h5-preloads.log), [`logs/h5-preloads-2.log`](logs/h5-preloads-2.log).

## H6: lane rolls

| Roll | Stopped, cold | Stopped, warm | Playing, cold | Playing, warm | Entries |
|---|---|---|---|---|---|
| Perc (10 folders) | 16.1 ms | 15.6 ms | 59.9 ms | 48.7–51.4 ms | 55 |
| Perc, tag 808 | 15.8 | 15.6 | 54.3 | 50.3–51.0 | 55 (6 match) |
| Tom (3 folders) | 7.4 | 7.4 | 23.0 | 22.5–22.9 | 34 |
| Tom, tag 808 | 7.3 | 7.2 | 22.7 | 21.1–23.3 | 34 (3 match) |
| **All 11 lanes** | 55.2 | 55.3 | **174.4** | 174.8–202.8 | 265 |
| All 11 lanes, tag 808 | 55.4 | 55.7 | **205.8** | 186.0–208.3 | 265 (25 match, 10 lanes picked) |

| Longest stretch between yields | Stopped | Playing |
|---|---|---|
| Any roll | 2.5–3.2 ms | 5.5–10.8 ms |
| Slowest single `f_opendir()` | 1.6–2.0 ms | 3.9–6.5 ms |
| Slowest single `f_readdir()` | 0.27–0.38 ms | 0.87–1.64 ms |

0 late renders in every roll. Cold and warm are the same: FatFs keeps no directory cache, so every roll reads the
card. The second set of playing rolls in `h5-preloads.log` is labelled "cold" but ran after the first, so it's warm.

| Threshold | Result | |
|---|---|---|
| Full-roster roll ≤ 300 ms while playing | 174–208 ms | **Pass** |
| No run over 3 ms without a yield | Stopped up to 3.2 ms; playing 5.5–10.8 ms. One `f_opendir()` alone takes up to 2 ms stopped and 6.5 ms playing | **Fail** |

The long stretches aren't time the CPU is held: while a call waits on the card, `routineForSD()` runs the audio
routine (hence 0 late renders). But no UI task runs in them, and a single `f_opendir()` can't be split. So "3 ms
between yields" can't be met by yielding more often; the scan should run where card waits already yield, and the
budget should be counted in CPU time, not wall time.

## H5: full roster preload while playing

| Preload (seed) | Drums scan | Drum kit | FX kit | 4 synths | Total | Fast free before → peak | Largest block at peak | Late renders |
|---|---|---|---|---|---|---|---|---|
| 11, first after boot | 188 ms | 113 ms | 221 ms | 227 ms | **748 ms** | 437 → 278 K | 162 K | 0 |
| 11 again (warm) | 191 | 5 | 204 | 224 | 624 | 432 → 278 K | 159 K | 0 |
| 23 | 199 | 72 | 219 | 229 | 720 | 432 → 276 K | 158 K | 0 |
| 37 | 197 | 187 | 214 | 258 | 855 | 431 → 275 K | 159 K | 0 |
| 51 | 198 | 71 | 225 | 257 | 751 | 430 → 274 K | 159 K | 0 |
| 67 | 172 | 136 | 222 | 250 | 780 | 429 → 273 K | 159 K | 0 |
| 83 | 179 | 149 | 198 | 220 | 746 | 427 → 271 K | 159 K | 0 |

The roster takes about 157 K of fast heap. Teardown took 18–20 ms. The longest render gap was 1.4 ms. Every preload
built all 11 drum lanes, 2 FX sample lanes, 2 FX synth rows and 4 synths without an error.

| Threshold | Result | |
|---|---|---|
| ≥ 150 K fast free at the peak | 271–278 K | **Pass** |
| Largest block ≥ 64 K at the peak | 158–162 K | **Pass** |
| Preload ≤ 1 bar at 175 BPM (1,371 ms) | 624–855 ms | **Pass** |
| 0 late renders | 0 | **Pass** |

## What it shows

- **A maximum roster preloads next to a heavy song with room to spare**: 271–278 K free at the peak, and a preload
  takes a little over half a bar at 175 BPM. The playing roster here was the POC's (about 52 K, from #37), not a
  second maximum one. Swapping it for a maximum roster would take about 105 K more, leaving about 166–173 K at the
  peak: still over the 150 K line. That backs [§9.7](../generative/spec/09-arc.md#97-transitions),
  [§10.7](../generative/spec/10-sounds.md#107-when-sounds-load) and the roster figure in
  [§20](../generative/spec/20-budgets.md). The measured roster (about 157 K) matches §20's "about 162 K for the largest
  roster".
- **The folder scans are a quarter of the preload.** Scanning the 11 lanes costs 170–200 ms while playing, as much as
  building the kit. A preload that rolls lanes once and keeps the folder listing would save most of that on later
  rolls.
- **Fast free after teardown dropped about 1.5 K per preload** (437 → 426 K over seven). It may be a small leak in
  the unlinked-Output teardown or fragmentation. Worth a look before preloads run every few phrases.

## Left out

- "The heaviest pad": the card's pad presets are synth or FM, so the heaviest is a 6 KB XML with no samples. A
  sample-based preset (the card's largest is `170 Sitar`) wasn't forced into the roster. Which three other synths
  loaded depended on the seed.
- "While a maximum roster plays" is Ttt plus the POC's roster, not a second Generative roster.
- The 4 FX lanes of §20 are 2 sample lanes and 2 synth rows here, as the ticket asks.
