# H7: pad rows and the OLED redrawn together

Test H7 of the spec review's [hardware tests](https://github.com/gismya/deluge-generative/blob/research/spec-review/docs/research/spec-review-technical.md#performance-assumptions-and-hardware-tests),
for [Measure pad and OLED redraws together on a Deluge](https://github.com/gismya/deluge-generative/issues/60).
Run on 2026-11-05 on the OLED Deluge over the sysex test port, with no hands on the device.

## Setup

- **Build:** this branch, RelWithDebInfo, `-DENABLE_SYSEX_TEST=ON -DENABLE_SYSEX_LOAD=ON`, loaded with `loadfw`. It
  is `task/perform-poc-hardware-tests` (the Perform POC and its probe) with `generative` (test port, #55 fix) and
  `task/hardware-measurements` (MEAS probes) merged in, plus the stand-in probe
  `src/deluge/generative/debug/hw_probe.cpp`.
- **Driver:** `scripts/generative/spec_hw_tests.py h7`. A new empty song, Perform entered, 175 BPM set through the
  probe, transport running. Each configuration runs 30 s.
- **What the probe does.** It finds each 16th grid line from the playback position in the audio routine (at most one
  128-sample render late). The Perform view polls that every 15 ms (its `graphicsRoutine`) and on a new 16th asks for
  the test rows with `uiNeedsRendering()`, and for an OLED frame each beat or each 16th. The test rows show a moving
  pad, the sidebar a moving pad, and the OLED status line `Bar b.beat` or `Bar b.beat.16th`, so every frame really
  changes. The OLED skips frames whose pixels didn't change.
- **Timing points:**
  - row latency: grid line → the PIC UART TX DMA has sent the rows (`tx_interrupt` with nothing left);
  - OLED latency: grid line → the OLED frame's SPI DMA completed (`oledTransferComplete`);
  - both also from the request (the view's poll), which leaves out the POC's 15 ms polling;
  - UART busy: time the PIC UART TX DMA was running, over the 30 s;
  - press latency: an injected pad edge arriving over USB (the test port queues it in the MIDI routine) → its
    handler returned in `readButtonsAndPads()`. A press and a release 40 ms apart, 2–5 times a second.
- Audio: the POC probe's late-render count (a render later than the 128-sample buffer).

## Results

Times in ms, average / maximum. 175 BPM: a 16th is 85.0 ms.

| Rows changed | Sidebar | OLED each | 16ths | UART busy | Rows from grid | Rows from request | Rows late | OLED from grid | OLED from request | Press | Late renders |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | no | beat | 356 | 3.6% | – | – | – | 20.3 / 31.0 | 12.8 / 16.8 | 2.3 / 6.9 | 0 |
| 0 | no | 16th | 352 | 3.6% | – | – | – | 19.9 / 46.4 | 12.0 / 16.9 | 2.5 / 5.0 | 0 |
| 1 | no | beat | 357 | 25.4% | 31.7 / 50.2 | 23.9 / 30.4 | 0 | 38.7 / 48.4 | 31.4 / 35.4 | 2.3 / 4.9 | 0 |
| 4 | no | beat | 356 | 25.3% | 31.6 / 42.5 | 24.1 / 28.5 | 0 | 38.2 / 48.3 | 31.2 / 35.2 | 2.6 / 4.9 | 0 |
| 8 | no | beat | 355 | 25.4% | 31.6 / 45.6 | 24.0 / 28.5 | 0 | 38.8 / 48.3 | 31.0 / 35.0 | 2.4 / 5.0 | 0 |
| 1 | no | 16th | 355 | 25.3% | 31.7 / 42.9 | 23.9 / 28.4 | 0 | 38.5 / 49.8 | 30.6 / 35.4 | 2.3 / 5.0 | 0 |
| 8 | no | 16th | 354 | 25.4% | 31.9 / 85.5 | 24.0 / 28.4 | 1 | 38.6 / 49.8 | 30.8 / 35.4 | 2.3 / 5.0 | 0 |
| 8 | yes | beat | 353 | 28.2% | 34.1 / 45.1 | 26.4 / 36.3 | 0 | 41.1 / 50.4 | 33.4 / 37.7 | 2.2 / 5.0 | 0 |
| 8 | yes | 16th | 356 | 28.2% | 34.1 / 63.5 | 26.4 / 30.8 | 0 | 40.8 / 51.9 | 33.1 / 37.9 | 2.7 / 5.0 | 0 |
| 0 | no | beat | 352 | 3.6% | – | – | – | 19.8 / 31.5 | 12.7 / 16.8 | 2.5 / 5.0 | 0 |

The longest gap between audio renders was 0.53 ms. Log: [`logs/h7.log`](logs/h7.log).

## Against the thresholds

| Threshold | Result | |
|---|---|---|
| UART ≤ 50% of the link | 25% with the main grid each 16th, 28% with the sidebar too | **Pass** |
| Rows sent within their 16th | 2,482 of 2,483 redraws. The one late redraw (85.5 ms) waited 57 ms for the view's 15 ms poll; from the request no redraw took over 36.3 ms | **Pass**, with one late redraw caused by the POC's polling |
| OLED ≤ 20 ms | 12–13 ms average, 17 ms max from the request when no pad rows are sent. With pad rows each 16th, 31 ms average and 38 ms max from the request, 39–41 ms average and 52 ms max from the grid line | **Fail** whenever pad rows go out each 16th |
| Press latency ≤ 10 ms | 2.2–2.7 ms average, 6.9 ms max | **Pass** |
| (0 late renders) | 0 | Pass |

## What it shows

- **The number of changed rows doesn't matter.** Any main-row change sends the whole main grid:
  `PadLEDs::sendOutMainPadColours()` sends all 8 two-column messages (8 × 49 = 392 bytes, about 18.5 ms of UART at
  200 kbaud). One row costs the same as eight. The sidebar adds one more message (49 bytes). "Redraw only changed
  rows" (§13.5) saves render time, not UART time, unless Perform adds a per-row or per-column-pair send.
- **The OLED waits behind the pads.** An OLED frame starts with a SELECT message that waits until the PIC UART has
  drained, so a frame asked for just after a pad redraw waits for its 18.5 ms. Without pad traffic a frame takes
  12 ms from the request (up to 10 ms for the pending-UI task, then the PIC handshake and its 3 ms settle). With pad
  rows each 16th it takes 31 ms. To keep the OLED under 20 ms Perform would have to send the OLED frame before the
  pad rows on the same 16th, or skip pad rows on 16ths that also change the OLED.
- **Input isn't starved.** Pad sends don't delay injected presses: 2.5 ms on average, at most 6.9 ms, the same with
  no redraws as with the busiest one.
- **Measured from the grid line, the POC adds up to 15 ms** of polling before anything is asked for. A real Perform
  that asks for its redraw from the tick would save that.

## Notes on the stand-in

- The MEAS build shows a summary popup for 30 s after Perform entry. While it is up it covers the status line, so
  OLED frames don't change and none are sent. The driver waits 35 s after entry before measuring. An earlier run that
  didn't wait showed no OLED frames in its first configuration.
- The POC view follows the step the Generator has decided, up to 12 ticks before it sounds, and the Generator's hook
  runs only on the ticks it asks for (10–12 of 16 grid lines a bar at Density 25). So the probe finds grid lines from
  the playback position instead.
- Press latency starts when the test port has the edge, so it leaves out USB and, for a real press, the PIC's scan.
  Both are the same with or without redraws.
- No one looked at the panel during the run.
