# Steering is logged where it lands, from the tick

Capture replays a piece's steering log through the Compose driver, so the log has to put every action where it changed the music. Steering doesn't write into Generator state from the UI. UI handlers push **steering commands** into a small fixed queue, and the core drains it at each step decision, inside the tick. A command is applied, and appended to the log, at the step where it **lands**: the next undecided step for a macro, or the line for a bar-, phrase- or Drop-quantised action, which waits in the core's own pending list until then. Replay applies the logged entries at their steps through the same apply path, without quantisation. This reverses the earlier rule that "nothing is recorded from the audio routine". Some actions are only seen in the tick (a MIDI FILL toggle, the per-bar external-clock tempo), and some land at a moment set by wall-clock time (a transition's Build starting when its preload ends). Logging at the press can't place either of those correctly. Decision: [Interface between the Generator core and its drivers](https://github.com/gismya/deluge-generative/issues/75), from technical review findings B3 and W4.

## Considered options

- **Stamp at the press, in task context, and replay through quantisation.** The log would hold the player's intent, and replay would re-run the quantisation. But actions seen only in the tick still need a tick-side writer. A press stamped before a yield and applied after it can land a step apart from its stamp. And a transition's commit step depends on preload time, which a replay can't reproduce.
- **UI writes straight into Generator state, plus a separate log.** This has no single path, so live play and replay could drift apart, and Capture's correctness would rest on two code paths agreeing.

## Consequences

- Cancelled actions never reach the log, and the log needs no cancel entries.
- "Now" actions (SHIFT+mute, hold) take effect at the next undecided step rather than at tick grain, so the log matches what was heard.
- Driver-side steering (homes, other params, tempo and swing glides) acts in the Perform driver at once but goes through the same queue to be logged at the next undecided step.
- The queue and log are a portable module outside the Generator instance blocks, so the harness can run the "Capture replay matches the live run" spec.
