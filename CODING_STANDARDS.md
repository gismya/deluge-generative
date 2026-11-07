# Coding standards

For reviewing Generative mode changes. The mechanical rules (no float, firmware includes or libc stdio in the
Generator's portable code, no OLED string literals in Perform's UI) are the `generative-rules` hooks in
`.pre-commit-config.yaml`, which CI runs: leave them to the hooks. What follows are the judgement calls.

## Stock firmware edits

- A stock edit stays small and gated. It acts only when `rootUIIsGenerativePerform()` holds or the Generative modal
  (`generativeModalUI`) is the current UI, and leaves stock behaviour unchanged otherwise. The logic lives in
  Generative code; the stock file holds the gate and a call.
- Every stock edit has a row in spec §19 giving its `v1.3.0` line, and its comment in the stock file starts
  `Generative mode (F<n>, spec §…):`. A new edit adds or extends a row.
- A fix to stock behaviour that isn't specific to Generative mode gets an entry in `docs/upstream-candidates.md`.

## Constants and capacity

- No speculative constants: no constant, field, enum value or capacity for behaviour that isn't built yet. A value
  picked by ear says `(tuning)` in its comment.
- Capacity is sized by construction and pinned by a `static_assert`. A change to a size the spec budgets updates §20.

## Spec and code agree

- When the spec leaves a choice open, the change makes it, writes it into the spec where it is a rule, and names it
  in the PR. Spec text and code change in the same PR.
- One rule, one place: two sites working out the same rule (where a command lands, a name table, an interpolation)
  share one helper or table.
- Comments say what the code does now.

## Determinism and the tick

- Anything that can change the event stream replays: draws are keyed hashes (§6.1) with no history counters, and a
  command's land-time effects happen in `Generator::apply()` (ADR 0005).
- Tick-side work is bounded and small (H1, §20). Long work, card scans included, runs in a runner job that yields at
  chunk boundaries (§12.10).

## Tests

- Every harness spec and probe expectation can fail: it pins the value this change produces. A value any build
  satisfies (a trailing number the probe reads as its timeout) checks nothing.
- New firmware behaviour a probe script should see gets a `TEST_EVENT`.

See `docs/generative/implementation-notes.md` for the code's known traps.
