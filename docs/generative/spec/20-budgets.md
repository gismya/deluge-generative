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
| Entering Perform | 67 ms while playing with 4 lanes and a Bass |
| Silent incoming Kits | linked next to a playing heavy song, an 11-lane Kit costs 59 µs and a 4-row FX Kit 42 µs per 128-sample buffer: about 3.5% CPU together, with no late renders ([Measure silent incoming Kits and retiring Outputs on a Deluge][t83]). The cost is per render, so it rises when renders are short |
| Linking an incoming roster | 6–7 ms for three Outputs. The largest single link (clip, NoteRows and Output, no yield) took 1.6 ms. No late renders |
| Freeing a retired Output | `deleteClipObject` with `InstrumentRemoval::DELETE` while playing: 11-lane Kit 18–24 ms in 67 yields, FX Kit 6–10.5 ms in 21, Synth 1.7–3.6 ms in 5. At most 0.93 ms between yields, no late renders. The Output stays in the render list for about one yield per NoteRow first ([§12.9](12-runtime.md#129-instances-and-the-transition)) |
| Compose song | measured with a clip per part per Section occurrence: 1.7 MB for 4:00, 4.7 MB for 20:00, 15.7 MB for 60:00. Clip setup is 67–75% of it, about 130 K per occurrence (about 6 K per kit row, 11 K per synth clip). Notes are about 30 B each. So [§15](15-compose-output.md#15-compose-output-shape) uses one clip per part: about 0.7 MB for 4:00 and 1.8 MB for 20:00 (estimate). Freeing the current song doesn't help the fast heap: song structures are `allocMaxSpeed` and fill it first. XML parsing is 2–5× slower while audio runs |
| Saving and loading songs | a 1.0 MB file saves in 0.36 s and loads in 2.0 s. A 10.1 MB file saves in 3.4–7.55 s but fails to load on an empty Deluge with `FILE_CORRUPTED`, so Capture splits long pieces ([§17.5](17-capture.md#175-song-shape)) |
| Out of memory | once SDRAM is full, any `operator new` throws `BAD_ALLOC`, nothing catches it, and the device freezes with `TERM`. Compose and Capture stop at an SDRAM reserve, 512 K to start ([§15.9](15-compose-output.md#159-memory-and-failures)) |
| Pad redraw | 21–22 ms of UART per full redraw, shared with the OLED. Pad sends defer behind OLED updates |
| OLED | 128×43 visible, 4 rows of 21 characters or a title plus 3 rows. Redraws at most every 10 ms. `OLED::setupSideScroller` doesn't scroll while playing; the Compose title scrolls the way the stock session view scrolls a long song name |
| Tempo | the hardware plays exactly the set BPM (130.0000 by the sample clock). delugemu runs about 0.5% slow |
| Stability | the POC's hook ran 31 minutes with no leak, drift, stuck notes or late renders, and followed swing and tempo changes exactly |
| `new` | global `operator new` and std containers use only a 2 MiB SDRAM pool and throw when full. The Generator doesn't use them ([§2.1](02-architecture.md#21-the-generator-core)) |
| Song loads | the "buttons and pads" task can run up to 20 ms without yielding during a song load |
| System clock | `getSystemTime()` reads about 0.8% short against real time |

The POC's core was about 1.65 KB of state (`sizeof(Generator)` 1664 B) with no allocation.

The M1 core is 12,260 B of state per Generator block on ARM (`sizeof(Generator)`, measured after the interplay and expression slices, #100 and #101; it was 8480 B after the sounds slice, #104, moved the Recipe's sound picks out of the Generator's copy). The event buffer, sized for the largest roster, is most of it: 8780 B, grown by a re-strike per sounding note for the Section-line cut. The sounding-note table and the late-entry table are 1056 B each. Perform's two blocks take about 24.5 K of fast heap. The Current Recipe is 13,192 B in SDRAM (`PLACE_SDRAM_BSS`): the Recipe, its sound picks and the copy a genre pick stages (2.7 K each), and two palette overrides (3.9 K each), one in use and one staged. The sounds slice adds no internal SRAM. A `static_assert` bounds it at 16 KB (H18).

**H1, measured** on the M1 skeleton (Techno, Drums with 4 lanes and a Loop Bass, RelWithDebInfo, 33.33 MHz OSTM count around each step decision in the tick, reported every 4 bars): at the default macros a decision costs 10.5–11.4 µs on average, at most 22–48 µs; with every macro at 50, 14–15.4 µs on average, at most 26–31 µs, with one 4-bar window at 53.6 µs. That is the "tens of µs" assumed, so the phrase-line fallback of [§12.2](12-runtime.md#122-when-steps-are-decided) isn't needed yet. With keyed pattern picks (#98): 12.7–13.1 µs on average at the default macros, at most 27–33 µs; with every macro at 50, 15.7–19.6 µs on average, at most 30–39 µs, and 76 µs in the 4-bar window holding a Drop, where every lane picks afresh and the Loop line is rolled. Re-measure as later slices add parts and layers.

**H9, measured** on the M1 runner (RelWithDebInfo, entry as a modal runner job, timed from the OK press to Perform's song swapped in): 25 ms with the user's heavy song Ttt playing (531 K of fast heap free before entry), from a fresh boot; 23–35 ms cold and 12–13 ms warm over the boot song. The roster was M1's Techno at the time (an 11-row Kit with 4 sampled lanes, and a synth Bass), so re-measure once Techno has its full roster. BACK 5 ms into a cold build cancels it, and the job has unwound 12 ms after the OK. Leaving takes 16 ms, 0.6–0.8 ms of it deleting Perform's song.

**Leaving frees what entry built.** On hardware, five enter/leave cycles from a blank song leave the internal heap exactly where it was before entry (the first cycle after boot also fills the sample cache's bookkeeping, about 0.8 K once). The external heap loses 96 B a cycle, the same 96 B a stock SHIFT+LOAD → NEW loses (delugemu: 104 B for both), so the loss is in the stock new-song path that leaving follows, not in Perform.

**Still to measure during implementation.** These rest on assumptions that need Generator code before they can be measured. Each has a test design with a pass threshold in the [technical review][tech-review]:

| Test | Assumption | When |
|---|---|---|
| H1 | a step decision costs tens of µs in the tick (the headroom knee can be found earlier, see the map) | M1 skeleton: measured, above |
| H2 | live ramps on every targeted param, rewritten each step, cost little | after F2, early M1 |
| H9 | entering Perform with a full roster while a heavy song plays takes under 1 s | M1: measured with a partial roster (above); still open for the full roster |
| H13 | 128 KB of steering log holds about 10 marked pieces with busy steering | M1 counter, M4 |
| H15 | 12 ticks of extra steering latency is acceptable | M1 |
| H16 | CPU and voices around a Drop don't cull Kick or Bass | M1 transitions |
| H17 | MIDI feedback of live values fits the link | M1 MIDI |
| H18 | the Generator state block stays small (a `static_assert`) | M1 skeleton: the `static_assert` in `generative/core/generator.h`, 16 KB |
| H19 | the largest saved song that loads back on an empty Deluge, and the [§15.9](15-compose-output.md#159-memory-and-failures) size estimate against a real Compose build. Sets the SDRAM reserve and the Capture split budget | M3, M4 |

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
