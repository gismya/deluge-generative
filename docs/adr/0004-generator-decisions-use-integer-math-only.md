# Generator decisions use integer math only

Every Generator decision that can change the event stream uses integer math only. That covers table lookups, the Arc, the seeded hash draws, onset thinning, gates, the chord walk and Lean. Float is allowed only after events leave the Generator, for example in the harness's rough render and the firmware's sound-control ramps. The reason is the local check that renders a golden case through Compose in delugemu and diffs it against the harness golden. The firmware builds with `-ffast-math` and the harness doesn't (it runs natively on the Mac and on x86 in CI). A float threshold could round differently in each build, flip a note, and fail that check with no real bug behind it. Integer math costs little here: the macros are 0–50 integers, PERF-1's logic is integer and table work, and the draws are integer hashes. Decision: [Testing and CI](https://github.com/gismya/deluge-generative/issues/41).

## Considered options

- **Float with matching compiler flags in both builds** (`-ffp-contract=off`, no fast-math in the Generator's files). Possible, but it depends on keeping flags in step across two toolchains (arm-none-eabi gcc and Apple clang or x86 gcc), and a mismatch fails silently until a golden diff turns up.
- **No check of the firmware against the harness.** Then nothing proves the firmware plays what the harness tested.

## Consequences

Probabilities and weights are integers on a fixed scale, such as per-mille or 0–255, not floats in 0–1. We may need to reconsider this rule if integer math turns out to get in the way, for example if a curve or blend can't be written cleanly in fixed point, or if rounding makes something sound wrong. If that happens, the likely fallback is the first option above, scoped to the Generator's own files, and this ADR would be superseded.
