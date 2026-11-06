# H10, H11, H12: building and saving Compose- and Capture-sized songs

Tests H10–H12 of the spec review's [hardware tests](https://github.com/gismya/deluge-generative/blob/research/spec-review/docs/research/spec-review-technical.md#performance-assumptions-and-hardware-tests),
for [Measure building and saving Compose- and Capture-sized songs on a Deluge](https://github.com/gismya/deluge-generative/issues/62).
Run on 2026-11-06 on the OLED Deluge over the sysex test port, with no hands on the device. **H12 stopped before the
reload: the Deluge stopped answering on USB** (see the end).

## Setup

- **Build:** the probe code of `task/step-timing-measurements` (`a158a3e2a`), RelWithDebInfo, loaded with `loadfw`.
  Its version string is `1.3.0-dev-0ff245aeb-dirty`, so a crash drawn on the pads would show commit `0ff2`.
- **The song builder** (`src/deluge/generative/debug/compose_shape.cpp`, `probe compose` / `probe songsave`) builds
  a song through the model API, shaped like §15:
  - 6 parts: an 11-lane Drums kit, Bass, Lead, Chords and Pad synths, and a 4-lane FX kit, with no sample files;
  - one clip per part per occurrence, as long as the occurrence (16-bar Sections). Kit clips have a row per lane,
    each with its own ParamManager. Each clip is in session view with its own section, and the arranger plays it
    at its place;
  - 40 notes a bar (22 drums, 2 FX, 8 bass, 4 lead, 3 chords, 1 pad);
  - one drift node a bar on 70 params (2 per kit row, 10 per synth).

  It stops cleanly if total free memory (fast, SDRAM and sample region) falls under 1 MB.
- **H10:** into a private `Song*` while Ttt and the Perform POC play (Energy 50, Density 50, 130 BPM), yielding to
  audio every 1 ms as stock builds do (`routineWithClusterLoading()`). It measures the fast heap, SDRAM, the sample
  region and the sample cache (the stealables waiting to be evicted), then deletes the song.
- **H11:** the same with a time budget: one scheduler round (`yield()`) whenever a run reaches 1 or 2 ms. A cancel
  sent over the test port sets a flag that the build checks at every yield point.
- **H12:** a new empty song, stopped, then the 60-minute Capture shape built into it (80 occurrences over 1,920 bars;
  past the 23rd, arrangement-only clips), then saved with `createXMLFile` → `Song::writeToFile` →
  `closeFileAfterWriting` to `SONGS/GENTEST_H12.XML`, then loaded again through the LOAD browser.
- **Driver:** `scripts/generative/spec_hw_tests.py h10` and `h12`. Logs: [`logs/h10-smoke.log`](logs/h10-smoke.log),
  [`logs/h10-h11.log`](logs/h10-h11.log), [`logs/h11-cancel.log`](logs/h11-cancel.log), [`logs/h12.log`](logs/h12.log).

## H10: memory

| Song | Clips | Notes | Drift nodes | Fast heap free before → peak | SDRAM free before → peak | Sample region free before → peak | Sample cache before → peak |
|---|---|---|---|---|---|---|---|
| Smoke: 2 occurrences, 16 bars, stopped, no song | 12 | 648 | 1,120 | 966 K → 587 K | 2,006 K → 2,006 K | unchanged | 0 |
| **4:00**: 9 occurrences, 136 bars | 54 | 5,508 | 9,520 | 437 K → **56 bytes** | 1,922 K → 643 K | unchanged | 128 K → 128 K |
| **20:00**: 23 occurrences, 640 bars | 138 | 25,920 | 44,800 | 437 K → 40 bytes | 1,922 K → **0** | 61.2 MB → 58.8 MB | 128 K → 128 K |

So a 4:00 Compose song takes about **1.7 MB** (all 437 K of free fast heap plus about 1.28 MB of SDRAM). A 20:00 song
takes about 4.8 MB, about 2.4 MB of it spilled into the sample region. The allocator falls back to it
("dire memory") once the fast heap and SDRAM are full. The sample cache (Ttt's evictable sample data) didn't shrink:
the spill went into free sample memory, not cached samples.

| Threshold | Result | |
|---|---|---|
| 4:00 fits with ≥ 100 K fast free | It fits only by using up the fast heap: 56 bytes free at the peak | **Fail** |
| 20:00 doesn't evict the playing song's samples | No eviction (the cache stayed at 128 K), with 58.8 MB of the sample region still free. On a card whose song filled the sample memory, the 2.4 MB spill would evict | **Pass** here |

§20's 230–250 K is far too low, and so is the review's 600 K: per clip it comes to about 32 K, mostly the per-row
ParamManagers of the kit clips (15 rows a occurrence) plus notes and drift nodes. All of it was built while Ttt
played, so the fast heap left for everything else (UI, MIDI, the Perform roster's next preload) was zero.

## H11: chunked build, cancel, teardown

| Build | Yield | Build time | Longest run between yields | Late renders | Longest render gap | Teardown |
|---|---|---|---|---|---|---|
| 4:00 | audio every 1 ms (H10) | 131 ms | 3.4 ms | 8 | 4.6 ms | 80 ms |
| 20:00 | audio every 1 ms (H10) | 758 ms | 7.5 ms | 64 | 8.3 ms | 210 ms |
| 4:00 | scheduler, 1 ms budget | 108 ms | 3.4 ms | 27 | 6.1 ms | 79 ms |
| 4:00 | scheduler, 2 ms budget | 130 ms | 4.6 ms | 32 | 6.1 ms | 83 ms |
| 20:00 | scheduler, 1 ms budget | 648 ms | 7.5 ms | 158 | 9.8 ms | 207 ms |
| 20:00, cancel sent 0.15 / 0.3 / 0.45 s in | scheduler, 1 ms budget | 632–653 ms | 7.5 ms | 154–156 | 9.3–9.8 ms | 188–222 ms |

| Threshold | Result | |
|---|---|---|
| Longest run ≤ 2 ms | 3.4 ms at 4:00, 7.5 ms at 20:00. One clip's setup (`setAudioInstrument` and a kit's row ParamManagers) is a single run, and it grows with the song: cloning ParamManagers looks through the song's clips | **Fail** |
| 0 late renders | 8–158 late renders, gaps up to 9.8 ms: audible | **Fail** |
| Cancel ≤ 100 ms | The cancel was never seen: every build ran to the end, 0.2–0.5 s after the cancel was sent. While the build holds the buttons task, the test port's input didn't get through either, even with a scheduler round every millisecond | **Fail** |
| 4:00 build ≤ 10 s | 108–131 ms | **Pass** |
| Teardown | 79–83 ms at 4:00, 188–222 ms at 20:00, in one piece | (no threshold) |

## H12: a 60-minute Capture

| Step | Result |
|---|---|
| Build (80 occurrences, 1,920 bars) | 1.98 s. 480 clips (138 in session plus the new song's own, 342 arrangement-only), 77,760 notes, 480 arranger instances. Fast heap and SDRAM full (0 and 24 bytes free); 13 MB spilled into the sample region. 134 late renders while building (stopped, so silent) |
| Save to `SONGS/GENTEST_H12.XML` | **7.55 s for 10.1 MB** (create 0.33 s, write 7.2 s) |
| Song count after the save | 139 session clips, 342 arrangement-only, 480 instances, 77,760 notes: as built |
| Reload | **Not done.** The next test port request got no reply, and neither did identity, ping or the OLED mirror afterwards |

| Threshold | Result | |
|---|---|---|
| Fits in memory | Only by taking every byte of fast heap and SDRAM plus 13 MB of sample memory | Fail in practice |
| Save ≤ 30 s per hour of piece | 7.55 s | **Pass** |
| Reload OK | Not reached | Not measured |

## The Deluge stopped answering

After the save and the song count, the next state query (the start of the reload, about 2 s after the count) got no
reply. A state query and a read-only probe a minute later got no identity reply, no ping reply and no OLED dump. The
USB MIDI ports were still listed. Nobody could see the pads or the OLED, so it isn't known whether it froze with an
error, faulted (a crash drawn on the pads) or is still busy. The last state read showed the song
built into the current song, stopped, in session view, with 0 bytes of fast heap and 24 bytes of SDRAM free. A
likely cause is that something needed a small allocation after the build had taken all general memory.
Hardware work stopped there.

## What it means

- [§20](../generative/spec/20-budgets.md)'s Compose figure is off by about 7×: a 4:00 Compose song in this shape is
  about 1.7 MB, not 230–250 K, and it can't sit in the fast heap next to a playing song. Building into a private
  `Song*` while a heavy song plays exhausts general memory. [§15](../generative/spec/15-compose-output.md) needs a
  leaner clip shape (fewer per-row ParamManagers, for example params only on rows that move) or a memory policy for
  [Compose and Capture memory, limits and failures](https://github.com/gismya/deluge-generative/issues/78).
- The H11 chunking assumption fails: per-clip setup is a single run of 3–7.5 ms, and neither audio-only yields nor
  scheduler yields from the buttons task keep audio clean or let a cancel in. For
  [Long-running work and input during loads](https://github.com/gismya/deluge-generative/issues/74), the build has to
  split clip setup itself, and the cancel has to come from somewhere that runs while the build does.
- A 60-minute Capture saves fast enough (7.5 s), but building it in memory at this shape takes every byte of general
  memory and, here, left the Deluge unresponsive.
