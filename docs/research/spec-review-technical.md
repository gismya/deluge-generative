# Technical review of the Generative mode spec

A review of the technical sections of [`docs/generative/spec.md`](https://github.com/gismya/deluge-generative/blob/generative/docs/generative/spec.md) at `9e83e480b`: 02, 06, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21 and 22, checked against `src/deluge/` at `v1.3.0`. A subagent did the review. Nothing was run on hardware and no files were changed. The report is kept as written. Tickets on the [wayfinder map](https://github.com/gismya/deluge-generative/issues/1) link to its findings by number: blocking B1–B4, worth fixing W1–W15, and hardware tests H1–H18.

Summary: four blocking gaps, each a design decision an implementer would otherwise make alone, and three of them also open a hole in the determinism contract. Most firmware `file:line` references hold. The weak points are performance and memory assumptions. The biggest is the Compose song's memory: it is projected from a user song, not measured, and is likely 2–3× the figure in §20.

## Blocking for the split

**B1. Genre change, Regenerate and LOAD transitions have no lifecycle** (§12.9, §9.7, §10.7, §2.1, §16.1, §17.3)
- **Where incoming Outputs live before the Drop.** An Output renders only once it has an active clip (`song.cpp:2516-2527`, `sound_instrument.cpp:93-96`). Linking Outputs and clips into the song allocates (ClipArray insert), so it can't happen in the tick at the Drop. The Outputs must already be in `currentSong`, silent, with active clips. The spec doesn't say this.
- **Two Generator instances overlap.** The incoming instance must decide the Drop step 12 ticks early (ADR 0003). The outgoing one must still send Throw ramps and note-offs after the Drop. That contradicts "v1 runs one" (§12.9) and the single state block in §2.1.
- **Retiring Outputs mid-play is not specified.** The open references are: gold-knob focus (`view.activeModControllableModelStack`), a sound editor open on an outgoing part, MIDI-follow stored clips and MIDI-learned params. `session.cpp:2489` already guards against outputs being freed mid-render.
- **Missing state rules:**
  - What the Current Recipe is between the confirm and the Drop. SAVE during the Build would write the old genre with the new glide target (§16.1).
  - Whether macro turns during the Build carry over.
  - What happens on stop during a preload or Build.
  - What a second genre, LOAD or Regenerate request does while one is preloading or queued.
  - What End does during the transition Build.
  - What happens to a sound re-roll or take queued across the Drop.
  - Whether mute, solo, freeze and focus carry over to a roster with different parts.
  - Which Section a hard cut lands in.
- **Fix:** add a §12.9 subsection covering each point, with a state diagram.

**B2. There is no runner for long operations, and the "Wait" input rule doesn't work** (§12.10, §14.3, §14.5, §15.7, §17.7)
- `REMIND_ME_OUTSIDE_CARD_ROUTINE` only defers until `sdRoutineLock` clears, which is the end of the current card wait, not the end of the preload. It also puts the byte back at the head of the PIC queue (`deluge.cpp:285-313`). One "Wait"-class press therefore blocks every later pad and button press, BACK and macro pads included. That breaks "act at once" and "BACK still cancels".
- `routineWithClusterLoading()` runs only the audio routine, not the buttons, MIDI or PIC tasks (`audio_engine.cpp:403-416`). A build that yields only through it never sees BACK.
- The spec never says whether builds are scheduler tasks or blocking calls that use `yield()`, nor how cancellation unwinds a build that is on the stack.
- **Fix:** one shared runner for Perform entry, preload, Compose and Capture:
  - it yields to the scheduler on a time budget;
  - a Generative "busy" flag makes Wait-class actions queue once or refuse with a popup, instead of relying on REMIND;
  - cancel is a flag checked at chunk boundaries;
  - teardown is chunked too.

**B3. The interface between the core and its drivers is undefined** (§2.2, §6.3, §11, §17.2, §21.2)
- **Sound-control payload.** §6.3 promises identical "sound-control values", but homes are preset param values (§13.1) and the harness has no presets. The stream must carry per-control offsets in menu units, with homes and the §11.1–11.2 per-preset target choices on the driver side. This isn't stated.
- **Steering input.** Nothing says how steering enters the core: UI writes straight into state, or step-stamped commands. Capture's correctness depends on live steering and replay taking the same path. Stamp-then-apply must not straddle a yield.
- **Equivalence specs.** The §21.2 specs ("an untouched Perform driver run matches a Compose run", "Capture replay matches") need Perform and Compose driver logic (lookahead, mute gate, Section splitting) built into the harness. Today that logic sits behind `sendNote` and `Song*`.
- **Arc plan query.** The SONG page's Arc plan (§13.5) needs a read-only query that is safe to call from task context.
- **Fix:**
  - state the tick unit (24 per 16th);
  - define the event schema: `{part, lane|kit, control, offset, ramp|snap}`;
  - make one steering queue `{step, kind, payload}` that is also the log;
  - give each driver a portable scheduler plus a thin firmware adapter;
  - add a const plan query.

**B4. Stateful flavours don't fit takes, freeze and unfreeze** (§6.2, §6.6, §6.7, §5.2)
- Loop mutation and Motif re-rolls build up over passes and depend on steered Motion.
- "Earlier takes replay exactly", "stepping back is free" and "unfreeze jumps to where counters would be now" all need a take's line at an arbitrary position. The only ways to get it are state ×16 takes, an unbounded recompute in the tick, or mutation defined as a pure function of position.
- Ticket #17 deferred this to "the role and flavour work".
- **Fix:** choose one. Either mutation becomes a pure function of position with bounded look-back, or define that "an earlier take restarts from its base line at the phrase line".

## Worth fixing

**W1. Compose memory is unmeasured and probably 2–3× too low** (§20, §15.6–15.8)
- The "230–250 K" is #37's extrapolation from a heavy user song.
- A Compose song has one clip per part per occurrence, and each clip carries its own param copies (per row for Kits). At about 20 B per AutoParam that is roughly 3–4 KB per ParamManager × 21 ParamManagers per occurrence. That comes to about 600 K for a 4:00 song, before drift nodes (one per bar per param).
- The current song is still in RAM during the build, and there is no out-of-memory policy even though §15.8 notes freeze paths.
- **Fix:** measure it (H10). Add a headroom check before every build and a clean abort path. Consider freeing the current song before building.

**W2. Held notes across Section and clip boundaries** (§15.3)
- Pad Sustain, pedal drones, a Siren, a synth Riser or a LEGATO slide across a Section line gets cut at the clip end and not re-struck, so Compose differs from Perform.
- **Fix:** a rule in every driver, like the Lean clamp, that cuts held notes at the boundary and re-strikes them on the Section's first step.

**W3. F10 is incomplete** (§19, §13.7)
- MIDI RECORD calls `recordButtonPressed()` directly (`playback_handler.cpp:2918`), which starts recording instead of making a Capture mark.
- MIDI TAP calls `tapTempoButtonPress` (`:2952-2958`), which bypasses the Recipe tempo and glide. The TAP button under an overlay falls through the same way.
- **Fix:** add both to F10, and TAP under overlays to F4.

**W4. §17.1 "nothing is recorded from the audio routine" can't hold**
- Fill (MIDI FILL toggles `fillModeActive` outside the UI) and the external-clock tempo sampled per bar are only seen in the tick.
- SHIFT ("now") mutes and home edits act at tick grain but are logged at step grain, so Capture can differ from what was heard by up to a step.
- **Fix:** allow the tick to append to the fixed log buffer, and make "now" actions take effect at the next undecided step.

**W5. The swap template contradicts the wording** (§15.7)
- `clear_song.cpp:96-105` sets `preLoadedSong` and then calls `doSongSwap`, which reads it (`playback_handler.cpp:1497`). So "never `preLoadedSong`" should read "set it only immediately before `doSongSwap`, with no yield between".
- `clear_song.cpp:73-87` stops playback only on internal clock or arrangement. Under an external clock it swaps live and runs `deleteSoundsWhichWontSound`.
- **Fix:** state the external-clock case for Perform entry, Compose, Capture and a stopped LOAD.

**W6. Tick resolution vs magnitude** (§12.3, §8.5)
- Bar length is `96 << inputTickMagnitude` (`song.cpp:5791`), so at magnitude 1 a 16th is 12 ticks and at 0 it is 6.
- Flams 1–3 ticks apart, ±12 Lean and 6-tick ratchets then can't be placed, which breaks the firmware-matches-harness diff.
- **Fix:** fix the Generator's unit at 24 per 16th, set the magnitude explicitly on Perform, Compose and Capture songs, and define rounding when external magnitude matching lowers it.

**W7. Tempo has no integer representation** (§3.2, §14.4, ADR 0004)
- Compose's End placement comes from LENGTH and tempo. A float BPM under `-ffast-math` could round differently from the harness.
- **Fix:** integer centi-BPM, which is what `UNPATCHED_TEMPO` already uses (`song.cpp:3133`).

**W8. F2 ramp unit and missing stop** (§11.7)
- `tickSamples` advances in internal-tick time (`auto_param.cpp:948-972`), but steps are 24 swung ticks. With no stop, a ramp overshoots by the swing fraction on long 16ths and leaves the control's clamp.
- **Fix:** state the unit, and add a target stop in `applyValueIncrement`, which is a few lines.

**W9. Roster arithmetic in §20 is wrong**
- "2 × 12 lanes × 8 K + 4 synths × 10 K" counts the synths once and uses 12 lanes.
- The maximum is 11 drum lanes plus 4 FX lanes, two of them synth-sized through `loadSynthToDrum`, plus 4 synths: 2 × (15 × 8.1 + 4 × 10) ≈ 323 K. It still fits.
- Perform's song, the Generator blocks and the steering log are missing from the budget.

**W10. The event buffer and the sounding-note table have no worst-case size or overflow rule** (§12.2, §12.5)
- A busy step can hold about 100 sound-control targets plus ratchets and flams.
- Overflow must behave the same in every driver.

**W11. Capture misses some of what was heard** (§17.2, §17.6)
- Not logged: song FX F1–F7, Sound-page params no control targets, sound-editor edits that aren't params, and the Glide setting needed to replay a glide.
- Re-rolled picks use fresh entropy, so the log must store their paths, and each piece needs a starting snapshot of its picks (about 2 KB). That is missing from the "tens of KB" estimate.

**W12. The 23-occurrence cap contradicts the End rule** (§15.2 vs §15.1)
- If the 23rd occurrence is a Break or Build, End can't be queued without cutting it.

**W13. Failure handling is missing**
- Card full or write error during a Capture save.
- Card removed during a preload.
- Recipe save failure.

**W14. The equivalence specs can't exist "from day one"** (§21.2 vs §22.3)
- The Compose driver arrives in M3 and the log in M4.
- Nothing checks the firmware's Perform path against the harness.
- **Fix:** at M1, emit Perform's event stream through `TEST_EVENT` in delugemu and diff it against the harness.

**W15. Contradictions**
- The glossary's **Piece** omits mid-set LOAD (§17.3, §23.13).
- §13.4 says a key change waits for a pending Drop; §9.7 says it waits only if the Drop comes first.
- §2.2 says swing is "plain events", but song swing comes from the clock (§12.1).
- §6.2 says muted parts "cost nothing"; §12.5 says they are computed every step.

## Nits

- F5: `definitions_cxx.hpp` is at `src/`, not `src/deluge/`.
- F6 omits `sound_editor.cpp:597`, the ClipMinder check in `buttonAction()`.
- §10.4: `SideChain`'s constructor also reads `preLoadedSong`/`currentSong` (`sidechain.cpp:37`).
- §12.5: there is no Output-level `allNotesOff`. Kits need a per-drum `Sound::allNotesOff`, as the POC does.
- §12.6: the audio routine targets 1.45 ms, so "next render ~2.9 ms" is the worst case. Above about 215 BPM a tick is shorter than a render, so the per-tick voice budget assumption fails, and tempo is unbounded.
- §12.7: call `setTempoFromNumSamples` with `shouldLogAction=false` (it logs at `song.cpp:2583`). Glide linearly in BPM to match `UNPATCHED_TEMPO` interpolation in Capture.
- §15.7: make "yield every N steps" time-based.
- ADR 0004: also ban `long`, `size_t` and signed overflow in decision math. `long` is 32-bit on ARM and 64-bit on the Mac. `char` is fine because of `-fsigned-char` (`CMakeLists.txt:169`).
- §13.7: say whether feedback covers only the focused part.
- §17.7: peak memory is the previous Capture song plus the current build. Swap to a blank song between builds.
- §12.2: the fallback of planning in task context needs a deterministic rule for when the plan is late, since stock song loads run 20 ms without yielding.

## Firmware claims checked

| Claim | Result |
|---|---|
| F1 `session.cpp:2541-2553` arp loop | OK |
| F2 `auto_param.cpp:859-868`, `param_set.cpp:105-109`, `patch_cable_set.cpp:345` (flags automated cables only); render pre-checks in `sound_instrument`, `kit.cpp:589-660`, `global_effectable_for_clip.cpp:162-171`, all gated on clock active and clip active; "no stop" | OK |
| F3 `auto_param.cpp:101`, `ui.cpp:33-35`; knobs and menus reach it through `setValuePossiblyForRegion` (`:997-1005`) | OK |
| F4 `encoder_input.cpp:137-151` | OK |
| F5 UIType `:187-213`, uiTypeMap `ui.cpp:477` under `ENABLE_SYSEX_TEST`, `session_view.cpp:245` | OK (path nit) |
| F6 lines 1106, 1421, 1953, 2311, 2387 and the null read at 2161 | OK (597 missing) |
| F7 `:477-509`, `:1777-1780`, `:829-841` | OK |
| F8 `midi_follow.cpp:420-454`, `:1336` | OK |
| F9 `playback_handler.cpp:213-255`, `:280-292` | OK |
| F10 `:162-191`, `:2893-2965` | OK, but RECORD and TAP are missing |
| F11 `action_logger.cpp:86-100, 565, 580, 867-869` | OK |
| F12 `storage_manager.cpp:406` | OK |
| F13 `save_song_ui.cpp:467-478`, `song.cpp:1177-1191` | OK |
| §10.4 APIs (every name exists, `loadFile(reversed, manuallySelected, …)`, preLoadedSong constructors); §10.5 `loadSynthToDrum(Song*)` and its `currentSong` follow-up | OK |
| §10.6 `polyphonic`, `voicePriority`, CHOKE through `SoundDrum::noteOn` → `Kit::choke()` | OK |
| §12.1 `SoundInstrument::sendNote`, `Kit::noteOnPreKitArp` | OK |
| §12.6 `allowedToStartVoice` 4/1 (`audio_engine.cpp:1624`), pending notes through arp status | OK |
| §12.7 `setTempoFromNumSamples` is public (`song.cpp:3111`) | OK |
| §12.8 an Output with no active clip renders nothing | OK |
| §12.10 handlers get `sdRoutineLock` | OK, but deferral semantics are as in B2 |
| §15.2 `kMaxNumSections` = 24 | OK |
| §15.7 `session.cpp:2367` swaps `preLoadedSong` | OK |
| §15.8 `loopLength` not set in the constructor; `getOrCreateNoteRowForYNote` changes the scale | OK |
| §17.5 `section = 255` | OK |
| §17.6 `UNPATCHED_TEMPO` plays only in arrangement or arrangement recording | OK |
| §8.7 `isFillModeActive` | OK |
| §12.10 gold-knob deferral; §11.2 delay-buffer allocation from 0 | Not checked |

## Performance assumptions and hardware tests

Already backed by hardware: voice starts are delayed, not dropped, and a 31-minute run showed no leak or drift (#45); tempo is exact (#45); kit-lane load times, two songs in RAM and full pad redraw cost (#37); POC Perform entry (#37, #45).

Everything below is still assumed. The "stand-in" tests use `poc/perform-techno` (the hook) plus the `task/hardware-measurements` MEAS build, driven through the test port, and time with the PMU cycle counter or the build's timer.

| # | Decision (§) | Backing | Proposed test | Runs at |
|---|---|---|---|---|
| H1 | Every step decided in one tick, 12 ticks ahead, "tens of µs" (§12.2, §20, ADR 0003) | Assumed (§1.2 defers it) | **Stand-in:** inject a calibrated busy loop on decision ticks in the POC hook. Sweep 0–1000 µs and record late renders, longest render gap, `cpu_direness` and culled voices to find the knee. **Then** time `generator.tick` at every macro on 50 with a Build roll and Ornaments, at phrase lines too. **Pass:** worst decision tick ≤ 100 µs and ≤ ⅓ of the knee; 0 late renders over 30 min. Fail → adopt the §12.2 fallback. | Knee now; real cost at the M1 skeleton |
| H2 | F2 live ramps on every targeted param, rewritten each step, ticked every render; drift per lane (§11.7, §11.5) | Assumed | **Stand-in:** once F2 lands, ramp N params (LPF, release, reverb send) on every POC kit row and the bass each 16th, N = 0/30/90/150. Measure audio routine time per render, culling and tick cost per rewrite. **Pass:** ≤ 10% routine time added at full N; no extra culling; no zipper by ear. | After F2 (early M1) |
| H3 | Voice-start priority under overflow (§12.6) | Partly: the limit was never hit (#45 test 1) | **Stand-in:** emit 8–12 note-ons on one tick in role order. Force `cpuDireness` > 12 with dense pads. Probe tick→voice-start samples per note. **Pass:** starts follow send order; Kick and Bass never delayed; any other note waits at most one render each. | Now |
| H4 | Tempo glide writes every tick (§12.7) | Assumed: #45 used encoder steps only | **Stand-in:** glide 130→170→100 over 1 bar and over 1 phrase. Log step sample times against the analytic curve; capture MIDI clock out on the host. **Pass:** 0 skipped or repeated steps; each step within ±1 tick; clock-out intervals monotonic; 0 late renders. | Now |
| H5 | Two full rosters in RAM, preload while playing (§9.7, §10.7, §20) | Partly (12-lane sample kit, one bass preset) | **Stand-in:** build a maximum roster (11 drum lanes from folder scans, an FX kit with 2 `loadSynthToDrum` rows, 4 synths including the heaviest pad) while a maximum roster plays. **Pass:** ≥ 150 K fast free and largest block ≥ 64 K at the peak; preload ≤ 1 bar at 175 BPM; 0 late renders. | Now |
| H6 | Folder scans with tags; Perc scans 10 folders (§10.2–10.3) | Assumed | **Now:** probe the Perc and Tom lane rolls on the factory card, cold and warm, playing and stopped. **Pass:** full-roster roll ≤ 300 ms while playing; no run over 3 ms without a yield. | Now |
| H7 | Pad rows redrawn each 16th plus OLED status line (§13.5) | Pad cost measured; combined with OLED not measured | **Now:** redraw k changed rows each 16th at 175 BPM with an OLED update each beat and each step. Measure UART busy time, row and OLED latency, and the latency of an injected press. **Pass:** ≤ 50% of the link; rows sent within their 16th; OLED ≤ 20 ms; press latency ≤ 10 ms. | Now |
| H8 | "Act at once" vs "Wait" input during card loads; knob lag (§12.10) | Assumed; the code suggests head-of-line blocking | **Now:** during the #37 kit-build-while-playing trigger, inject a Wait-class press, then macro pads, BACK and knob turns, and timestamp each handler. **Pass:** act-at-once inputs ≤ 20 ms; knob lag ≤ 10 ms; Wait-class actions don't run mid-build. | Now (expected to fail) |
| H9 | Perform entry with a full roster while a heavy song plays (§14.3) | Partly (67 ms POC roster; 273 K for two heavy songs) | Enter Perform with the full Techno roster while Ttt plays; cancel with BACK too. **Pass:** < 1 s; ≥ 100 K free; 0 late renders; heap back to baseline after cancel. | M1 |
| H10 | Compose song ≈ 230–250 K (§20, §15) | Assumed (extrapolated) | **Stand-in:** build a Compose-shaped song through the model API (9 and 23 occurrences × 6 parts, per-row kit params, about 40 notes per bar, one drift node per bar on about 70 params) into a private `Song*` while Ttt plays. Measure fast heap, SDRAM and sample-cache evictions. **Pass:** 4:00 fits with ≥ 100 K fast free; 20:00 doesn't evict the playing song's samples. | Now |
| H11 | Compose chunking, build time, cancel (§15.7, §14.5) | Assumed | Same builder with time-budget yields. Measure the longest run between yields, total time, cancel latency and teardown time. **Pass:** longest run ≤ 2 ms; 0 late renders; cancel ≤ 100 ms; 4:00 build ≤ 10 s. Repeat with the real driver. | Now; again at M3 |
| H12 | Capture of long pieces, build and save (§17.5, §17.7) | Assumed | **Stand-in:** a 60-minute piece shape (arrangement-only clips past the 23rd) saved through `createXMLFile` → `writeToFile` → `closeFileAfterWriting`, then reloaded. **Pass:** fits in memory; save ≤ 30 s per hour of piece; reload OK. | Now; again at M4 |
| H13 | Steering log "tens of KB for hours" (§17.2) | Assumed | 1-hour soak with scripted busy steering (two knobs swept continuously, macro pads every bar, mutes) plus a 100 Hz jittering MIDI-follow CC stream; count entries and bytes. **Pass:** 3 h projected ≤ the chosen buffer (for example 64 KB). | M1 counter stand-in; M4 |
| H14 | Driver clock under external clock: magnitude changes, jump rule (§12.3–12.4) | Assumed | **Stand-in:** the host sends MIDI clock at 70, 120 and 180 BPM with Start, Continue + SPP, and a mid-play SPP. Log hook deltas and magnitude. **Pass:** no false jumps while magnitude matching settles. At M1, also first note on the master bar ± 1 tick and 0 stuck notes. | Now; M1 |
| H15 | 12-tick latency (≈63 ms at 120 BPM) is acceptable (ADR 0003) | Judged in a browser prototype only | Inject a macro change and measure samples to the first affected note; then a listening session. **Pass:** ≤ one 16th + 12 ticks + 10 ms; testers don't report lag. | M1 |
| H16 | CPU and voices at the Drop (old tails + new roster + Throw); Thin turns on per-voice HPF in Builds (§9.7, §11.2) | Assumed | 20 genre changes and Regenerates at maximum macros; log voices, `cpu_direness`, culls by role and late renders around the Drop. **Pass:** 0 late renders; no Kick or Bass culled. | M1 (transitions) |
| H17 | MIDI feedback of live values at the stock rate (§13.7) | Assumed | Count CCs per second on DIN and USB with the focused part's 6 controls drifting at Motion 50. **Pass:** ≤ 30% of DIN bandwidth; no clock-out jitter change. | M1 (MIDI) |
| H18 | Size of the Generator state block (§2.1) | POC was 1.6 KB | Add a `static_assert` on its size in the build. No hardware needed. | M1 |
