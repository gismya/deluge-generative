# 23. Decisions made while writing this spec

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

Places where the tickets disagreed or left a gap, and what this spec says.

1. **Compose song name.** [Compose output shape][t16] names the song `GEN <GENRE> <seed code>`, and the [prototype resolution][t50] said it stays unnamed. The user chose **named, unsaved** ([§15.7](15-compose-output.md#157-build-and-handover)). Capture's `… LIVE` name and Recipe file naming build on it.
2. **Polymeter.** [Recipe parameters][t10] listed it under Complexity, but nothing designed it. The user chose to **drop it from v1** ([§1.2](01-scope.md#12-out-of-scope-for-v1)). It is in `docs/future-features.md`.
3. **Ambient never drops.** The user chose **gentle section rules**, with no new mechanism: a slow swell with no Gap, roll or Throw, and a Drop lift near 0 ([§4.5](04-genres.md#45-v1-genres), [§9.5](09-arc.md#95-section-rules)).
4. **Energy and Register.** [Track roles][t12] and [Density as a fourth macro][t24] said Energy shifts the Register, and [Pitch model][t21] never said how. The user chose to **drop it for v1**: Registers are fixed ([§7.3](07-pitch.md#73-register)). It is in `docs/future-features.md`.
5. **Takes per part.** The CLIP page shows 16 take pads, so v1 has 16 takes per part, 0–15 shown as 1–16 ([§6.6](06-randomness.md#66-takes)). Making a new take past the 16th is refused.
6. **Genre versions.** [Genre model][t11] said genres have no version and a missing genre falls back to a default. The later [Recipe files][t40] gave genres a version and refuses an unknown genre. This spec follows Recipe files ([§4.4](04-genres.md#44-names-and-versions), [§16.2](16-recipe-files.md#162-genre-versions)).
7. **Part cap.** [Track roles][t12] set no hard cap on parts. [Generator scheduling][t19] sizes state for 6 parts and the sidebar has 6 MUTE pads, so the cap is 6 ([§5.1](05-parts-and-lanes.md#51-parts-and-roles)).
8. **A flam's grace hit under load.** [Sub-step expression][t20] said it may be dropped. [Hardware tests of the Perform POC][t45] showed the firmware delays extra voice starts by one render instead. The spec says it waits ([§12.6](12-runtime.md#126-voice-starts)).
9. **Generative menu in M1.** Build order puts the Generative menu in M3, but entering Perform needs it. M1 builds the menu with PERFORM and GENRE ▸ only ([§22.3](22-build-order.md#223-milestones)).
10. **MIDI-learned edits.** [Generator scheduling][t19] said edits from MIDI learn are overwritten at the next step. The later [External tempo and MIDI in Perform][t51] makes them move homes like a knob turn. The spec follows the later ticket ([§11.6](11-sound-controls.md#116-home-and-player-edits-perform)).
11. **Genre change timing.** [Sound generation][t13] swaps at the first phrase line after loading, and [Perform interaction model][t15] plays a Build first. The spec places the transition's Build like a forced Build, ending on a phrase line once loading is done ([§9.7](09-arc.md#97-transitions)).
12. **Sound-control homes on stop.** No ticket said whether a home survives a stop. A home is the param's value on the Output, so it lasts until the Output is replaced ([§13.1](13-perform.md#131-its-own-song)).
13. **Mid-set LOAD and pieces.** [Capture][t35] defined piece boundaries before [Recipe files][t40] added mid-set LOAD. A LOAD transition ends and starts a piece like a genre change ([§17.3](17-capture.md#173-pieces)).
14. **The voice-start spec.** [Testing and CI][t41] listed "at most 4 voice starts per step" as an invariant. A busy downbeat with Ornaments can legitimately exceed it, and the firmware delays the extra starts rather than dropping them ([Hardware tests of the Perform POC][t45]). The spec reports the count instead of failing on it ([§21.2](21-testing.md#212-harness)).

15. **Melodic Techno.** [Genre model][t11] briefed Techno as drums, an acid line and one chord, with Pads, Chords and Leads arriving in M2 with Ambient. After M1's skeleton played, the user wanted pads, chords, melodies and basslines other than acid in Techno. The user chose **subgenres**, genres written as overrides of a parent, over alternatives inside one genre's parts, and moved the flavours they need into M1 ([§4.1](04-genres.md#41-what-a-genre-is), [§4.5](04-genres.md#45-v1-genres), [§22.3](22-build-order.md#223-milestones)). ADR 0006 has the reasoning.

[t10]: https://github.com/gismya/deluge-generative/issues/10
[t11]: https://github.com/gismya/deluge-generative/issues/11
[t12]: https://github.com/gismya/deluge-generative/issues/12
[t13]: https://github.com/gismya/deluge-generative/issues/13
[t15]: https://github.com/gismya/deluge-generative/issues/15
[t16]: https://github.com/gismya/deluge-generative/issues/16
[t19]: https://github.com/gismya/deluge-generative/issues/19
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t21]: https://github.com/gismya/deluge-generative/issues/21
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t35]: https://github.com/gismya/deluge-generative/issues/35
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t41]: https://github.com/gismya/deluge-generative/issues/41
[t45]: https://github.com/gismya/deluge-generative/issues/45
[t50]: https://github.com/gismya/deluge-generative/issues/50
[t51]: https://github.com/gismya/deluge-generative/issues/51
