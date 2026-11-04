# The Generator decides each step 12 ticks ahead, so Lean can push

**Lean** is signed and runs from −12 to +12 ticks (half a 16th either side of the grid). That lets a part or lane play *ahead* of the grid, such as pushed hats or an urgent snare. For an early hit to be possible, the Generator decides each 16th step 12 ticks before its grid line rather than right on it. That amends [Generator scheduling](https://github.com/gismya/deluge-generative/issues/19). The lookahead is fixed at 12 in every genre, even ones that use no negative Lean, so v1 always runs at the worst-case latency and we find out whether that ever matters. It costs 12 ticks of extra steering latency: 625/BPM ms per tick, so about 63 ms at 120 BPM, on top of today's wait of up to one step. The window for actions quantised to a bar or phrase line also closes 12 ticks early. To keep Compose's clips self-contained, the Generator clamps negative Lean to 0 on every Section's first step, in every driver, so Perform and Compose still agree. Decision: [Pushed Lean](https://github.com/gismya/deluge-generative/issues/30), judged by ear with the [prototype](https://github.com/gismya/deluge-generative/tree/prototype/pushed-lean/prototypes/pushed-lean).

## Considered options

- **Late-only offsets** (0 to +12, no lookahead). Simpler, but it can't push against the clock or against a kick on the grid. Faking a push by shifting every other part late makes the whole band lag the clock (a problem for synced gear) and puts the kick off the grid in Compose.
- **Lookahead derived per genre** (*k* = that genre's deepest push). Genres without push would pay no latency. Deferred: it's a tuning change once we know whether 12 ticks ever hurts.
- **Pushed hits on a Section's first step written into the previous Section's clip**, or clamped in Compose only. The first makes clips depend on each other and loses the hit where that part has no clip. The second breaks "Compose matches Perform".

## Consequences

The step's event buffer holds events from the step being decided as well as the step playing. Changing the lookahead or the Lean range within ±12 is a constant and data change, and it changes only the event streams of genres that use push. Going beyond ±12 would reopen this ADR.
