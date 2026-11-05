# H14: the driver's clock under an external MIDI clock

Test H14 of the spec review's [hardware tests](https://github.com/gismya/deluge-generative/blob/research/spec-review/docs/research/spec-review-technical.md#performance-assumptions-and-hardware-tests),
for [Measure the driver clock under an external MIDI clock](https://github.com/gismya/deluge-generative/issues/63).
Run on 2026-11-05 on the OLED Deluge, with no hands on the device.

## Setup

- **Build:** this branch (the H7 build plus the H14 probe), RelWithDebInfo with the test port, loaded with `loadfw`.
- **Clock source:** the host. `scripts/generative/spec_hw_tests.py h14` runs a thread that sends MIDI real-time
  messages to the Deluge's first USB MIDI port (`Deluge Port 1`) with python-rtmidi, timed with `perf_counter`. The
  test port runs on `Deluge Port 3` at the same time. Clocks went out up to 6–8 ms late at worst, which is rougher
  than a DAW.
- **Settings for the run:** tempo magnitude matching on and MIDI clock in on, set in RAM through the probe
  (`probe clockcfg 1 1`) and put back afterwards. Nothing was saved to flash. The user's setting had matching off.
- **The probe** (`hw_probe.cpp`, `driverHook()`) runs at the top of `PerformDriver::doTickForward()` on every hook
  call. It logs the swung-tick position, the delta since the previous call and the input tick magnitude. A delta that
  is negative or longer than a bar counts as a jump (§12.3). If the jump is on the first hook call after a Start,
  Continue or SPP, it is expected. Anywhere else it is false. `PlaybackHandler` reports each Start, Continue, SPP and
  Stop it acts on.
- **Each scenario:** a new song in Perform, stopped. Then Start and 8 bars of clock, a mid-play SPP to bar 3 and 4
  more bars, Stop, a 1 s pause, SPP to bar 5, Continue and 4 bars, Stop. External 70, 120 and 180 BPM against the
  Perform tempo of 130 BPM, plus 180 BPM against 90 BPM, which makes matching lower the magnitude.

## Results

| External / internal BPM | Hook calls | False jumps | Expected jumps | Magnitude | Changed at | Largest delta | Played at |
|---|---|---|---|---|---|---|---|
| 70 / 130 | 761 | **0** | 1 (Continue) | 2 → 3 | 2nd hook call, 42 ms after Start | 48 (a 16th at magnitude 3) | 70.00 BPM |
| 120 / 130 | 759 | **0** | 1 (Continue) | 3, unchanged | – | 48 | 119.97 BPM |
| 180 / 130 | 754 | **0** | 1 (Continue) | 3 → 2 | 2nd hook call, 37 ms after Start | 24 | 180.13 BPM |
| 180 / 90 | 754 | **0** | 1 (Continue) | 2 → 1 | 2nd hook call, 36 ms after Start | 12 | 179.97 BPM |

The magnitude carries over from one scenario to the next, which is why 120 / 130 starts at 3. Matching stayed active
for the first 12 hook calls after Start, about one beat, as the firmware's `kNumInputTicksToAllowTempoTargeting`
says. 0 late renders, and the longest render gap was 0.52 ms. Log: [`logs/h14.log`](logs/h14.log), with every
recorded hook call.

**Pass:** no false jumps while magnitude matching settles, at all three tempos and the extra one.

## What the deltas show

- **A magnitude change doesn't look like a jump.** On the call where the magnitude changes, the delta is small
  (6 or 12 ticks) and positive. From then on, every delta is in the new magnitude's units (a 16th is 48 ticks at
  magnitude 3, 12 at magnitude 1). It isn't clear which magnitude the straddling delta belongs to. At worst that
  misplaces the driver's clock by a few ticks, less than an eighth of a 16th, once, in the first 16th after Start.
- **A mid-play SPP doesn't show in the deltas at all.** The first hook call after the SPP had a delta of +15 or +16,
  and the swung position kept counting on as if nothing happened. The firmware moves the clips' play position
  (`resetPlayPos`) but not the swung tick counter the hook sees. So §12.3's rule ("a negative delta or one longer than
  a bar counts as a jump, handled like a mid-play SPP") never catches a real mid-play SPP. To do §12.4's note-offs at
  the jump and resume at the next master bar line, the driver needs to be told about the SPP itself, for example from
  `PlaybackHandler::positionPointerReceived()`.
- **Continue restarts the swung counter at 0**, whatever the SPP before it said: the first hook call after Continue
  saw position 0 and a delta of minus the whole previous position (-9,200 ticks at magnitude 3). The jump rule does
  catch that one. This confirms §12.4: the firmware's counter doesn't follow SPP, so the driver must keep the SPP
  offset itself.

## Left out

- The POC's driver still works out time from an origin with a divide, not the §12.3 delta clock. The probe records
  what that clock would be fed (the deltas and the magnitude), not a running §12.3 implementation.
- The M1 parts of H14 (first note on the master bar ± 1 tick, 0 stuck notes) need the real driver.
- DIN MIDI and a hardware clock source weren't tried: the clock came from the host over USB.
