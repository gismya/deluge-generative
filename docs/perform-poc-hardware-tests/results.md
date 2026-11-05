# Hardware tests of the Perform POC (#45)

Run on 2026-11-05 on the OLED Deluge on the desk. An agent drove it over USB with the sysex test port. A human power cycled it twice (see [The load hang](#the-load-hang)) and did nothing else. Logs are in [`logs/`](logs/), one per test, with every probe event stamped with host time.

## How

- **Build**: `task/perform-poc-hardware-tests` (`30682728b`, which differs from the loaded build only in clang-format and ruff fixes). That's `poc/perform-techno` with `generative` (the test port) merged in, plus the hat CHOKE fix. RelWithDebInfo (`-O2`, LTO), with `-DENABLE_SYSEX_TEST=ON -DENABLE_SYSEX_LOAD=ON`, loaded into RAM with `./dbt loadfw relwithdebinfo`.
- **CHOKE fix**: `poc/perform-techno` now puts Closed hat and Open hat in CHOKE mode (`337fa0f91`), as #12 requires. Test 6 needed it.
- **Probe**: [`src/deluge/generative/debug/poc_probe.cpp`](../../src/deluge/generative/debug/poc_probe.cpp) is compiled in only with `ENABLE_SYSEX_TEST`. Each bar it sends three test events:
  - `downbeat`: the audio sample timer at the bar's first grid line, and the bar's length in samples.
  - `bar`: the average 16th interval in samples, for intervals that start on an even step (`long`, the long ones under 16th swing) and on an odd step (`short`), with min and max.
  - `barv`: note-ons (`n`); voice starts that `AudioEngine::allowedToStartVoice()` refused, so they started in a later render (`def`, with `dk` for the kick and `db` for the Bass); open-hat note-ons on the same tick as a closed hat (`hat2`); closed-hat note-ons while the open hat still had a voice (`ohr`); the most voices sounding (`vmax`); all refusals and late starts in the firmware (`ref`, `late`); skipped and backward Generator steps (`skip`, `back`); the longest gap between audio renders in µs (`gap`); and renders late enough to run the 128-sample output buffer dry (`lr`).
  
  It also reports `gen_start` (hook position and the tick the Generator starts on), `gen_stop` (Generator note-ons left without a note-off), `perform_enter` (time spent in `PerformDriver::enter()`) and free memory every 15 s.
- **Driver**: [`scripts/generative/poc_hw_tests.py`](../../scripts/generative/poc_hw_tests.py), one subcommand per test.
- **Song**: a new, empty song (SHIFT+LOAD, OK), then SHIFT+SONG in session view.
- **Knobs**: one test-port detent is 4 encoder edges, and the Perform view moves a macro one step per edge. So a detent is 4 macro steps. The driver reads the macro values back from a `recipe` event after every change.

## Results

| # | Test | Result | Pass |
|---|---|---|---|
| 1 | Energy 50, Density 50: dropped kicks or bass notes | 33 bars, 1285 note-ons. **0 voice starts refused** by the 4-per-render limit, so none was deferred (kick 0, Bass 0). At most 5 voices at once, 0 late renders, longest render gap 0.57 ms. No held notes at stop, and voices were 0 within 5 s. | Pass, by measurement. Listening still open (below). |
| 2 | Change song swing while playing | Swing applies with no extra work. Steady state at 50 / 60 / 70 / 40%: long/short 16ths of 5088/5088, 6106/4070, 7123/3053 and 4070/6106 samples, which is exactly 50.0 / 60.0 / 70.0 / 40.0%. A change takes effect within the bar, with no stray intervals, and nothing was deferred or late. | Pass |
| 3 | Turn the tempo encoder while playing, slowly and fast | Slowly 130 → 140 → 130 (one detent every 0.7 s), fast 130 → 160 → 130 (back-to-back detents). Bar lengths followed exactly (75,600 samples at 140, 66,150 at 160), with **0 skipped, repeated or backward steps**, 0 deferrals and 0 late renders. The tempo encoder moves at most 1 BPM per action, so a 40-detent "burst" moved 1 BPM. | Pass |
| 4 | Play 30 min or more, sweeping both knobs | 31 min (1008 bars, 24,048 note-ons). Every ~2 min both macros jumped to a new pair: 0/0, 50/50, 10/40, 40/10, 24/26, 50/30, 6/50, 30/14, twice round. **No leak**: free fast heap read 866,704 B in all 123 readings (every 15 s), and external heap 1,930,592 B. **No drift**: every bar was 81,415–81,416 samples. 0 skipped or backward steps, 0 voice starts refused, at most 6 voices, CPU direness 0 throughout, render gaps 0.33–0.57 ms, 0 late renders, 0 test events dropped. At stop: 0 held notes, voices 0, and the device kept answering. | Pass. Listening open. |
| 5 | Quick start/stop; enter Perform while the transport runs | 8 start/stops with 0.08–3 s between them: 8 `gen_start`, 8 `gen_stop`, all with 0 held notes, and voices back to 0. Entered Perform three times while a 120 BPM session played: the Generator started on the next bar line each time (hook at tick 241 → started at 384; 387 → 768; 511 → 768), then ran at 130 BPM with 0 deferrals and 0 late renders. | Pass |
| 6 | Open hat at Density 30 and up (after the CHOKE fix) | See [The hats on one tick](#the-hats-on-one-tick). In 9 bars: Density 25 has no open hats; at 30, **every open hat (24) lands on the same tick as a closed hat**; at 40 and 50, 33 of them. Each of those closed hats found the open hat from the last offbeat still sounding. | Measured. Listening open. |
| 7 | Load time on entering Perform | `PerformDriver::enter()` takes **27 ms the first time** (cold samples) and **5 ms** after that, stopped, empty song. The view swap and the first OLED frame add nothing you'd notice. The press-to-UI-change time the host saw, 125 ms, is mostly the driver's own injected button waits. Free fast heap was the same after each of three enter/exit cycles. #37 measured 67 ms entering while a heavy song plays. | Measured |
| 8 | SAVE in Perform | **Nothing happens**, stopped or playing: no save screen, no popup, no LED, and the device stays in Perform. `GenerativePerformView::buttonAction` returns `NOT_DEALT_WITH` for SAVE, and the global handler in `buttons.cpp` doesn't handle SAVE. Only `View::buttonAction` does, and the POC view never calls it. Safe, but the player gets no feedback. | Known (no crash) |
| 9 | Is the tempo exactly 130 BPM? | **Yes.** Over test 4's 1007 bars, the downbeats spanned 1859.076 s by the host clock and 1859.077 s by the audio sample clock: **130.0001 BPM** and 130.0000 BPM. The emulator's 129.3 BPM is an emulator artefact. On hardware, timing checks can expect exact tempo in ticks and samples. | Pass |

### The hats on one tick

Techno's hat tables put the closed hat on the offbeat 8ths first and let the open hat in on the same offbeat 8ths at higher Density. With both hats in CHOKE mode and note-ons in Lane order (Closed hat before Open hat), a shared offbeat plays:

1. The closed hat starts and chokes the open hat still ringing from the previous offbeat.
2. The open hat starts on the same tick and chokes the closed hat, microseconds after it started.

So on a shared offbeat **the open hat wins**, and the closed hat can't be heard there. The closed hat cuts the open hat only from a later step. #12's "Open hat and Closed hat share a CHOKE group" doesn't decide which one wins when they land together. This is the POC findings' gap "Two lanes on one step". Whether it sounds right is a listening check.

### The load hang

Twice, a `loadfw` froze the device in the last ~3% of the upload. The OLED froze and the pads showed a crash. Loading over the SD-flashed firmware always worked. The cause is not POC code. The sysex load progress bar (`sysex.cpp:183-186`) writes past `PadLEDs::image`, and in this build `numUIsOpen` comes right after the image. The next audio render's `getCurrentUI()` takes a data abort, and the pads show exactly that crash. See [The sysex load progress bar writes past PadLEDs::image, which can hang loadfw](https://github.com/gismya/deluge-generative/issues/55).

### Side finding: the firmware's system timer reads 0.8% short

`getSystemTime()` (the task scheduler's clock, which stamps test events) runs about **0.8% slow** in seconds: test 4's 1007 bars took 1844.21 s by it, against 1859.08 s by the host clock and the audio sample clock (ratio 1.00806). So durations measured with it, or with `getSystemTicks()` divided by `DELUGE_CLOCKS_PER` (the same timer and constant), read about 0.8% short. That includes the ms figures here and in #37. Too small to change any decision. The audio sample clock and the host clock agree to under 1 ppm.

## Listening checks left for a human

The agent can't hear. These need ears, ideally with the logs' numbers in mind:

- **Test 1**: at Energy 50 / Density 50, are kicks and bass notes all there? The probe says nothing was deferred or dropped. A miss would have to come from somewhere it doesn't watch.
- **Test 6**: how do the hats sound from Density 30 up, now that the open hat wins on shared offbeats?
- **Test 4**: anything odd during the long run: clicks, a stuck bass note, the sound changing over time.
