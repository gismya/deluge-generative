# H8: input during card loads

Test H8 of the spec review's [hardware tests](https://github.com/gismya/deluge-generative/blob/research/spec-review/docs/research/spec-review-technical.md#performance-assumptions-and-hardware-tests),
for [Measure input during card loads on a Deluge](https://github.com/gismya/deluge-generative/issues/61). Run on
2026-11-05 on the OLED Deluge over the sysex test port, with no hands on the device.

## Setup

- **Build:** this branch (the H7 and H14 builds plus the H8 probe), RelWithDebInfo with the test port, loaded with
  `loadfw`.
- **The load:** a cold 16-lane kit built on Perform's kit code (`perform::buildSampleKit`, as the #37 trigger does)
  while the Perform POC plays at 130 BPM. To keep every build cold, each run takes 16 samples nobody has loaded since
  boot from the factory card's `SAMPLES/DRUMS` folders (`probe kitprep` picks them beforehand, `probe kitbuild` builds
  and then deletes the kit). Like the #37 trigger, the build runs from the buttons-and-pads task, so input reaches the
  firmware only through `routineForSD()` while the build waits on the card.
- **The inputs:** sent by the host 10–60 ms after the build command, without waiting for replies. In H8 mode the
  Perform view treats them as follows:
  - pad (15, 7) is a stand-in "Wait" action: during a card routine it returns `REMIND_ME_OUTSIDE_CARD_ROUTINE`, as
    §12.10 says genre change, Regenerate and the like will;
  - pads (3, 2), (4, 2), (5, 2) stand for "act at once" pads such as macro pads;
  - BACK acts at once and doesn't leave Perform (§12.10: BACK still cancels);
  - one detent of the upper gold knob (Energy), sent last.
- **Timing:** each edge from when the test port queued it to when its handler returned. The knob from when its turn
  was queued to the first `modEncoderAction()`. The build's start and end are logged too.
- Two variants, three runs each: **no wait** (only act-at-once inputs) and **wait first** (the Wait pad, then the
  same inputs).

## Results

Times in ms from arrival. Every input arrived while the build ran.

| Run | Build | Wait pad | Pad (3,2) | BACK | Pad (4,2) | Pad (5,2) | Gold knob |
|---|---|---|---|---|---|---|---|
| no wait #1 | 126.6 | – | 0.5 | 3.4 | 0.8 | 1.2 | 76.3 |
| no wait #2 | 118.1 | – | 0.3 | 3.3 | 1.8 | 3.5 | 61.2 |
| no wait #3 | 103.0 | – | 0.0 | 4.4 | 0.4 | 0.0 | 51.3 |
| wait first #1 | 90.0 | 83.1 (after the build) | 88.4 | 98.0 | 96.7 | 97.1 | 100.2 |
| wait first #2 | 195.2 | 187.9 (after the build) | 195.2 | 205.8 | 206.3 | 203.8 | 208.3 |
| wait first #3 | 167.6 | 162.4 (after the build) | 165.3 | 177.2 | 178.3 | 172.0 | 177.3 |

In "no wait" every press ran inside the card routine (`sdRoutineLock` set). In "wait first" the Wait pad was put back
once, and nothing after it ran until the build had ended. No late audio renders. The longest render gap was 0.81 ms.
Log: [`logs/h8.log`](logs/h8.log).

## Against the thresholds

| Threshold | Result | |
|---|---|---|
| Act-at-once inputs ≤ 20 ms | With no Wait press: 0–4.4 ms. After one Wait press: 88–206 ms, every one of them, BACK too | **Fail**: one Wait press blocks everything behind it |
| Knob lag ≤ 10 ms | 51–76 ms with no Wait press, 100–208 ms after one: the knob waits for the build to end | **Fail** |
| Wait-class actions don't run mid-build | The Wait pad ran 2.4–4.7 ms after each build ended | **Pass** |

As the review predicted (B2), the "Wait" rule in §12.10 doesn't work with the stock deferral.

## What it shows

- **Pads and buttons do get through during a card load**, as long as nothing has been deferred: their handlers run
  inside `routineForSD()` within 5 ms.
- **One deferred press blocks every later one.** `REMIND_ME_OUTSIDE_CARD_ROUTINE` puts the edge back at the head of the
  queue and sets `waitingForSDRoutineToEnd`. Inside `routineForSD()` `sdRoutineLock` is always set, so input stops
  until the whole build returns to the buttons task, not just the current card wait. Macro pads and BACK all waited the
  full build (up to 206 ms here, for one 16-lane kit).
- **Gold knobs never act during a build.** `interpretEncoders()` skips the mod knobs while `sdRoutineLock` is set, and
  the encoder task doesn't run while the build holds the buttons task. So the knob lag is the rest of the build, not
  "a few ms" as §12.10 says. On real hardware the encoder IRQ still counts the edges meanwhile, so the turn isn't
  lost, only late.
- So §12.10 needs what the review proposes: a Generative busy flag that queues or refuses Wait-class actions instead
  of returning REMIND, and builds that yield to the scheduler so the encoder and button tasks run. That's for
  [Long-running work and input during loads](https://github.com/gismya/deluge-generative/issues/74).

## Notes on the stand-in

- The injected knob turn only gets its edges once the test port's encoder feed runs (from the encoder task), which
  also doesn't run during the build. The lag measured is the same either way: the firmware wouldn't act on the knob
  before the build ends.
- The test port holds back every entry after an encoder turn until it has fed the turn, so the knob was sent last.
- A first try picked samples without checking them. Some files in `SAMPLES/DRUMS` aren't WAV or AIFF and fail with
  `FILE_UNSUPPORTED`, so `kitprep` now checks each file's header with a plain FatFS read, which leaves the sample cache
  cold.
- The Perform POC played a new, almost empty song, not a heavy user song.
