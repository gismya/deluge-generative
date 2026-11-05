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
| Perform roster | about 162 K for the largest roster: 11 drum and 4 FX lanes at about 8.1 K plus 4 synths at about 10 K. Two rosters are about 323 K, which fits next to a heavy song (273 K free with two heavy songs is the closest measurement). Perform's song, the Generator blocks and the steering log aren't in this figure yet |
| Kit lane | about 8.1 KB fast heap plus the sample in SDRAM. 10–15 ms per cold lane while playing; a 12-lane kit under 0.2 s |
| Synth preset load | 5.3 ms, 10 K fast heap |
| Entering Perform | 67 ms while playing with 4 lanes and a Bass |
| Compose song | **not measured.** #37 extrapolated 230–250 K from a heavy user song, but a Compose song has one clip per part per occurrence, each with its own param copies, and the technical review estimates about 600 K for 4:00 before drift nodes. XML parsing is 2–5× slower while audio runs |
| Pad redraw | 21–22 ms of UART per full redraw, shared with the OLED. Pad sends defer behind OLED updates |
| OLED | 128×43 visible, 4 rows of 21 characters or a title plus 3 rows. Redraws at most every 10 ms. `OLED::setupSideScroller` doesn't scroll while playing; the Compose title scrolls the way the stock session view scrolls a long song name |
| Tempo | the hardware plays exactly the set BPM (130.0000 by the sample clock). delugemu runs about 0.5% slow |
| Stability | the POC's hook ran 31 minutes with no leak, drift, stuck notes or late renders, and followed swing and tempo changes exactly |
| `new` | global `operator new` and std containers use only a 2 MiB SDRAM pool and throw when full. The Generator doesn't use them ([§2.1](02-architecture.md#21-the-generator-core)) |
| Song loads | the "buttons and pads" task can run up to 20 ms without yielding during a song load |
| System clock | `getSystemTime()` reads about 0.8% short against real time |

The POC's core was about 1.65 KB of state (`sizeof(Generator)` 1664 B) with no allocation.

**Still to measure during implementation.** These rest on assumptions that need Generator code before they can be measured. Each has a test design with a pass threshold in the [technical review][tech-review]:

| Test | Assumption | When |
|---|---|---|
| H1 | a step decision costs tens of µs in the tick (the headroom knee can be found earlier, see the map) | M1 skeleton |
| H2 | live ramps on every targeted param, rewritten each step, cost little | after F2, early M1 |
| H9 | entering Perform with a full roster while a heavy song plays takes under 1 s | M1 |
| H13 | the steering log stays in tens of KB over hours | M1 counter, M4 |
| H15 | 12 ticks of extra steering latency is acceptable | M1 |
| H16 | CPU and voices around a Drop don't cull Kick or Bass | M1 transitions |
| H17 | MIDI feedback of live values fits the link | M1 MIDI |
| H18 | the Generator state block stays small (a `static_assert`) | M1 |

Sources: [Runtime budget on the Deluge][t3], [Measure memory and load times on a Deluge][t37], [Hardware tests of the Perform POC][t45], [Deluge UI conventions and free controls][t33], POC findings on the [map][poc].

[t3]: https://github.com/gismya/deluge-generative/issues/3
[poc]: https://github.com/gismya/deluge-generative/issues/1#issuecomment-6640449431
[t33]: https://github.com/gismya/deluge-generative/issues/33
[t37]: https://github.com/gismya/deluge-generative/issues/37
[t45]: https://github.com/gismya/deluge-generative/issues/45
[tech-review]: https://github.com/gismya/deluge-generative/blob/research/spec-review/docs/research/spec-review-technical.md
