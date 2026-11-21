# Implementation notes: Portable code: chords, fills, sounding notes, flavours

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- The Chord-graph walk (spec §7.4): the rules are pure functions in `core/chord_walk.h` (`chordWeights()`,
  `nextChord()`, the guard and the colour rows), and `Generator::walkChord()` works out where a chord falls from the
  Arc and draws it. It runs after `arc_.enterStep()` and after a key change lands, so the guard sees the new scale,
  and before the parts decide, so a Loop resolves against the new root on the chord line itself. A Section start is a
  chord line too, so a Drop always starts a chord, except that a harmonic rhythm longer than the phrase only changes
  on phrase lines: there a Build shorter than a phrase (spec §9.3) starts mid-chord, and its start only restarts the
  cadence count. The trap: a Section starts off the grid more often than it looks (short Builds, forced Builds,
  a Drop transition's Build), so test every rhythm with a short Build.
- A chord's place needs the next chord line, not just the grid: a Section start ahead ends the chord early, so the
  phrase rules and the Drop cadence come from `Arc::ahead(motion, move)`. It derives the Arc's own next start from
  the helpers `enterStep()` uses (`stepsToStart()` for the lines, `playedOut()` for whether the Section is done, the
  same check `endsBefore()` makes), so a prediction can't drift from the real move; `arc_ahead_spec` checks it against
  stepping the Arc. `Generator::waitingMove()` is the one place that finds which waiting force, End or transition
  moves the Arc first (`throwLine()` and the walk both read it); `ahead()` merges it in, replacing the Arc's own move
  where it lands first, and the Drop where it lands between a Build's start and its Drop. Traps it fixed: a Groove's
  end in a later phrase matters on a rhythm longer than the phrase (the chord before a short Build is the last before
  the Drop), and a Section held past its length and released still ends on its next line, never "already ended".
  It is a prediction: a Motion turn, a hold or a force pressed after the chord is drawn can still move the Section
  start, and the chord keeps the rules it was drawn with. The rules are written into spec §7.4.
  Only the first waiting move counts, which is exact today (#269): on a rhythm up to a phrase long its Section start
  ends the chord; on a longer one chords start on phrase lines, where every move pressed earlier has landed, except a
  Build shorter than the phrase, and every such Build ends on the next phrase line (a forced Build and a Drop
  transition's draw one length, keyed on the Builds started, so two waiting ones land together). `walkChord()`
  asserts it in the harness on every phrase line of every genre. A new command that moves the Arc off a phrase line, or a Build length that doesn't end
  on the next one, trips the assert: `ahead()` then needs every waiting move, not the first.
  The state sits with the other byte fields: the chord before and the cadence count, and the playing chord itself as
  `voiced_` (below), whose root is the walk's degree. Chords change only
  on bar lines and a jump's `skipTo()` crosses none, so `skipTo()` doesn't walk. A genre with no graph returns at
  once, so its stream is unchanged. The guard, the colour row and the walk rules are passed in with their
  Generator-wide defaults: an override (a genre field, a Recipe value) is a new argument there.
- A looping graph (`ChordGraph::loopChance` ≥ 0, House) takes `Generator::walkLoop()` instead of the walk's place
  rules: the loop playing is a stored byte (`chordLoop_`), moved only on a Drop's chord line and keyed on the
  Groove's occurrence there, so a Break, a Build and the Outro hold the Groove's loop with no recount. Each chord is
  keyed on the loop's index and the bar in it (`Purpose::ChordLoop`), so every pass replays it chord for chord: the
  walk's other inputs (the chord before, A-B-A) agree because a loop always starts on home. It skips `Arc::ahead()` (no Drop cadence, no Section home bonus), and a Section start off the
  rhythm's grid draws no chord there, so the loop stays on the phrase grid.
- The FX part's lanes have no pools: section rules place their notes. Specs that walk pools skip pool-less lanes.
- Fills (§8.7) are worked out per step in `Generator::fillRules()`, right after `sectionRules()`, so they see the Gap
  and the roll; they set `Step::rest` and `Step::swap`. State is four bytes: the phrase's automatic fill (drawn on its
  first step), the held one, the pitched parts rested on the last step (a Rest ends their notes only as it starts)
  and whether a fill covered the last step (the Phrase accent reads it). A lane's fill patterns are its own
  `LaneSpec::fillPool`; a Swap picks from it with `pickEntry()` and the fill purposes, keyed on the lane's pick (a
  lane with only a fill pool still moves its pick at Drops and re-picks, through `placePick()`), and
  `fillPick()` gives the fill pattern an entry index after the pool's, so its draws never match a normal pattern's.
  Specs that walk pools at Density 0 turn fills off (a Rest removes anchors).
- Avoid (§5.5) works out "avoided" on the avoided step itself (`Generator::avoided()`, called by `endAvoided()` after
  the part's pick in `decidePart()`), not on the step before: a phrase line's re-pick or a Density move lands between
  the two, so a check made a step early reads the wrong pick. Only a rule at effective strength 100 counts: below it
  the rule only thins. A Loop note is cut back to its plain gate measured from `PartState::loopOn`, where the part's
  last note started (`express()` returns it). Not from the sounding note's `onTick`: a tie (`SoundingNotes::extend()`)
  keeps the first segment's, and a Section line may already have cut the note to the line (so its `offTick` no longer
  tells how it was played either). The cut can't reach back past the step's decision tick (`decisionTick()`: a
  note-off already due has gone out), so a gate under the 12-tick lookahead ends there instead. Only the note that
  started at `loopOn` takes the plain gate: a ratchet's later hits and a late entry's re-strike start after it, so a
  plain gate measured from `loopOn` would end them before they start (a note-off before its note-on); they end on
  the line at most. No cut may end a note before `onTick + 1`. The other cut paths (`cutSectionLine()`,
  `updateGates()`, and the Rest, Gap, Build cut, key change and new sound through `endPitchedBy()`) run before the
  step's parts decide and cut on a line at or after every note they reach, so only this one, which runs after the
  part's flavour, can reach back before a note's start. Other flavours' notes end on the line. `avoided()` needs no
  second walk of the target's plan: at full strength only an anchor survives, so it reads the source's pattern for
  an anchor. Trap for specs: a slide onto the same pitch ties, with no second note-on, so a higher strength can break
  a tie and *add* a note-on. Count planned onsets with slides off (`avoid_spec` does), or the "only ever removes
  more" check fails on note-ons.
- Respond (§5.5) counts the target's halves at the half-bar's first step, not on every step: a Density turn
  mid-half would otherwise flip the half under the source. The count waits for the source's own turn in the step
  (`countHalves()` at the top of `decidePart()`, or before a lane's pick in `decideKit()`), not the step's start, so
  the target's phrase-line re-pick is counted (`respond_spec` has a case a count at the step's start fails). The
  second half keeps the last bar's verdict until step 8 (§5.5). `interplayRulesValid()` follows a Respond target's own
  rules to any depth (`targetResponds()`), since a Lock or Avoid between them reads the Respond source's plan in the
  other half too. Specs check Respond against their own count through `testTargetPlanned()` (`respond_spec`).
- The Roll fill (§8.7) is a third `Step` mask, `Step::roll`, with its own stage and velocity beside the Build roll's
  (`heldRollAt()`, `autoRollAt()`); `decideKit()` keeps a stage per lane, so the Build roll and a Roll on another
  lane play in one step, and Replace pairs read each lane's own `rollHits()`. A held Roll's beat counts in
  `heldSteps_`.
- The sounding-note and late-entry tables are one pool each (`SoundingNotes`), laid out by role in
  `Generator::start()`: a part gets `maxSounding(role)` slots, and the pool holds the largest roster's worth (35). A
  roster past it (bad data) leaves a part no slots, so its notes don't play.
- The harness builds the core with `GENERATOR_TEST_SEAMS` (the firmware never does): `Generator::testFlavour_` plays
  a part whose flavour has no code yet, in that flavour's place in `decidePart()`. `roster_room_spec` sets it (its
  `TestFlavour` is a friend) to play a worst-case Stab, Pad and Motif with no flavour, worse than any real one
  (ratcheted 4-note voicings every step, a Motif note lasting two bars). A new flavour's branch goes before the seam's.
- The Sustain flavour (spec §5.2) is `Generator::decideSustain()`, with the pure voicing in `core/voicing.h`
  (`leadVoicing()`, `pedalVoicing()`, `voicePlace()`), shared with the Stab. It keeps no note longer than a bar: each note lasts to the next
  bar line, and on that line `SoundingNotes::extend()` holds it on (matching notes whose `offTick` is the line itself,
  so one a Rest, the Gap or a cut ended earlier comes back as a new note-on). The trap: a note cut *on* the line
  looks the same as one ending there naturally by its `offTick`, and a held voicing would ring past the cut (a new
  sound's swap would leave the Pad's notes on the retired Output). So every cut marks its notes `SoundingNote::cut`,
  even those already ending on its line, and `extend()` refuses marked notes: `SoundingNotes::cutAt()`, called by
  `endPartBy()` (a new sound), `endPitchedBy()` (a key change, a Rest, the Gap, a Build cut) and `cutSectionLine()`.
  A closing gate in `updateGates()` marks only the notes sounding past its line (a gated-out part plays nothing
  there, so nothing extends). A new cut path calls `cutAt()` too. Only the mute state, which cuts
  nothing, has its own re-strike rule in `PartState::sustain`; a gated-out or kept-out Sustain plays nothing. Its notes never last past a bar line, so
  `cutSectionLine()` and `updateGates()` never hold them back and `lateEntries()` never sees them: the flavour does
  its own late entry. The harness driver drops muted note-ons as Perform does: a spec of mute reads silence, not
  flagged events.
- `SustainSpec::openVoicing` (Ambient's Pad) swaps `leadVoicing()` for `openVoicing()` (`core/voicing.h`) in
  `decideSustain()`. The voices are the previous voicing's pitches, `PartState::voicing`, so it needs no state of its
  own: the trap is that `leadVoicing()`'s nearest-any rule is per pitch class, not per voice, so a wide first voicing
  collapses into a cluster within a few chords (the rules review's sketch kept 67 % open). `openVoicing()` pairs notes
  with voices by the least total move, over a table of the 24 orders of 4 slots (`kPairings`): no
  `std::next_permutation`, whose out-of-line copy would land outside the step section, and no codes decoded with `%`
  and `/` by a runtime divisor, each an `__aeabi_idivmod` call on the Cortex-A9 (about 1,000 a bar line). With slots
  fixed at 4, a pairing counts only when min(notes, voices) notes take a voice: a leftover note costs nothing, so it
  would otherwise stand in for a free voice. `leadVoicing()` itself is untouched, so no other genre moves.
- The Extension order is `HarmonySpec::extensionOrder` (`ExtensionOrder`, `core/pitch.h`), passed to `voiceChord()` by
  `voiceChordAt()` only: specs that call `voiceChord()` to check a genre's chords must pass the genre's order
  (`ambient_spec` passes `kNinthFirst`). `harmonyValid()` checks it with `extensionOrderValid()`.
- `SustainSpec::holdsAcrossSections` takes two places: `cutSectionLine()` leaves the part's notes ending on the line
  uncut (a cut note never extends), and `decideSustain()` skips the chord-change restrike on a Section line, so
  `extend()` holds each pitch the new voicing keeps. Every other cut (key, sound, Gap, Build cut) still marks them.
  `sustainValid()` rejects the flag on any other flavour: only `decideSustain()` ends or extends the uncut notes.
  The first Groove's first line (`arc_.occurrence() == 0`) re-strikes a holding part once (#341), so its level
  doesn't depend on how the piece began. A walk or run with `skipIntro` starts there, so its step 0 is that line and
  a spec of the re-strike needs an Intro. `SustainSpec::holdsCommonTones` is the same hold on every other bar line,
  so a chord change strikes only the changed pitches; `cutSectionLine()` doesn't read it (no cut on those lines).
  It composes with `openVoicing` by pitch: the voicing is chosen first and `extend()` holds only exact pitches it
  kept, so the two never disagree on what sounds, but the open voicing's least-movement pairing moves about a
  quarter of Ambient's common tones by an octave (they strike there).
- Trap: a spec that counts a held Pad's note-ons per chord line, or reads "a note ending where the same pitch
  starts" as a re-strike, must allow the first Groove line's re-strike; a walk that starts in the Intro has one.
- The Stab flavour (spec §5.2) is `Generator::decideStab()`: the pick and Density thinning are the Loop's
  (`updatePick()`, `planned()`), and the voicing is the Sustain's `leadVoicing()` without a doubled root, held in
  `PartState::voicing` (a `HeldVoicing`, shared with the Sustain: a part plays one flavour) and worked out on every
  bar line, before the pick, whether or not an onset plays. Each note goes through `express()` with the same pick
  and step, so the chord's notes share their draws. Its length is capped at the pattern's next onset (before
  thinning) and the bar line, which is what keeps it inside the Chords table (two onsets' notes) without a data
  check like `loopNotesValid()`; `stabNotesValid()` still runs the onset-gap bound, as for the Arp and the Motif. Dub techno and Detroit techno
  play one (`dub_techno_spec`, `detroit_techno_spec`; `stab_spec`'s Techno-like roster stays for the walk);
  `stabNotesValid()` (`core/genre.h`) holds each Stab part to a pool and a gate, and each genre `static_assert`s it. `roster_room_spec`
  keeps its worst-case Stab on the test seam (flavour None), and runs a real one on every step too.
- A Loop note longer than a step (Dub techno's sub) is a data rule, not a runtime one: `decideLoop()` plays the
  gate as it always did, and `loopNotesValid()` accepts a gate past a step only when every note ends before the
  pattern's next onset, the pattern's end counted (`detail::longLoopNotesFit()`). Don't cap Loop notes at the bar
  line or the next onset at runtime, as Stab/Arp/Motif notes are: `section_lines_spec`, `gates_spec` and
  `invariants_spec` play Loop notes lasting one or two bars (`LongBassParts`) to reach late entry, the Section-line
  cut, gates holding notes back and a full table, and a cap leaves those paths with no note to test them.
- A Register is the pitch heard (ADR 0007): pitched parts load at whole-sound transpose 0 (spec §10.6), so a factory
  bass saved at −24 (002, 007, 012, 047, 050) plays where its Register says. Set Registers for the sound you want,
  never to make up for a preset's saved transpose.
- Trap: an octave on the preset's oscillators (165 Acid Arp's −36 and −24, 068 FM Bells 1's +12) isn't reached by
  the Register: `osc_octaves.py` (in `check_all.py`) flags such palette presets.
- Trap: `patterns_spec`'s "play exactly the anchors at Density 0" counts every note-on on a lane, so a flam on an
  anchored lane fails it (the grace hit lands a step early). Techno's rim flams but has no anchors; Dub techno's rim
  has an anchor, so it has no flam.
- Trap: `patterns_spec`'s "play exactly the anchors at Density 0" can't hold for a part gated above Density 0
  (Detroit techno's Motif): it plays nothing there, so the spec expects silence from a gated part.
- Detroit techno takes the parent's Drums and FX, so its root-rhythm bass shares the beats with the kick (the
  brief's "root on most beats"). Its Chord graph plays each of the brief's moves both ways, the way back to i by the
  home pull: read one way only, ♭VI is unreachable, and a phrase's last chord on v or ♭VII has nothing but home to go
  to, which the phrase rule excludes, so the walk stalls on a repeat the brief's repeat weight 0 rules out. Its Break
  keeps the closed hat, which has no anchors: like Melodic techno it dips Density by 5 there, not 20.
- Trap: a pedal Sustain re-strikes only on a cut line, so in a Break its only note-ons are on the Break's first
  step. A spec that counts a Break's notes but skips its first step (to leave out what leans in from before) reads
  a kept pedal as silent (`dub_techno_spec` counts that step for the kept parts).
- The Arp flavour (spec §5.2) is `Generator::decideArp()`: the pick and Density thinning are the Loop's and the
  Stab's (`updatePick()`, `planned()`), and the note length is the Stab's (`onsetLength()`: the gate, cut at the
  pattern's next onset before thinning and at the bar line). It keeps no state: the walk position is counted from
  the pattern's onsets before the step, every step, and the pitch is the pure `arpNote()` (`core/voicing.h`) over
  `chord()`. A pool entry's order and octave span live beside the pool in `ArpSpec::walks` (`walks[i]` goes with
  `pool[i]`, which `arpNotesValid()` checks), so `Pattern` didn't grow for every pool. Its length is `onsetTicks()`, so
  the bound holds whatever the gate and the Lean (see core.md). A Random walk draws with `Purpose::ArpWalk`, keyed like the
  Density jitter.
- The Motif flavour (spec §5.2) is `Generator::decideMotif()`. Its slots are the Loop's line
  (`PartState::line`, a `LineStep` whose `degree` holds a chord position; `slide` stays false), so
  `rollLine()`/`mutateLine()`/`updateLine()` serve both flavours: a new pick or take rolls it, a pass end re-rolls
  slots on the Motif curve keeping their accent, freeze holds it, all with the Loop's keys. One roll helper,
  `rollLineStep()`, draws a step for either from its values (`LineWeight`) and its `LineDraws` (leap, octave jumps,
  accent), the Loop adding its slide; the draw fields are the Loop's, so Acid's and Melodic's goldens pin them.
  The pitch is the pure `motifNote()` (`core/voicing.h`) over `chord()` at play time, so a chord change moves the
  line with nothing re-rolled. `motifNote()` counts positions over the chord's notes taken into one octave, not the
  voiced order: `voiceChord()` voices a 9th above the octave (steps `{0, 2, 6, 8}`), and counting through those with
  `steps[p mod n] + 7·(p div n)` would make position -1 a 2nd above the root and position 4 lower than 3.
  Detroit techno is the first built-in genre to play one, gated at 30 so it comes in on Drops; `kTechnoMotif`
  (`tests/spec/generator/techno_motif.h`) stays as a harsher Motif (Lean on the a, ratchets, flams) for the bound's
  edges.
- The patterned pitched flavours but the Loop open with `Generator::playingStep()`: the pick (`updatePick()`, its
  change handed back for a line), then Density thinning, the interplay rules and the section rules, giving the step
  in the pattern or -1.
- The playing chord is voiced once a bar line (`Generator::voiced_`, set by `voiceChordAt()` in `decideNextStep()`
  after the walk, and by `begin()` at `extensionLevel()` of the Recipe's Complexity within the genre's floor and
  ceiling; a transition's `carryOver()` may then change the macros, which step 0's bar line picks up):
  `chord()` returns it, so the Pad, the Stab,
  the Arp and the Motif all hear a mid-bar Complexity turn on the next bar line (spec §7.4), and an Arp or Motif onset
  doesn't re-voice the chord. It holds because chords, Extensions and keys change only on bar lines: a key change
  lands on a phrase line, before the walk. A command that changes the scale or the chord off a bar line would have to
  re-voice it. `chordRoot()` is its `steps[0]`.
- A cut that ends a part's notes on a line must also stop its next notes leaning before the line: the cut runs when
  the command lands, before the step is decided, so a note leaning early starts before it, on the old Output or
  in the old key, and isn't cut. `Generator::cutParts_` holds the parts a SetSound or SetKey cut on the step being
  decided (reset at the start of each `decideNextStep()`), and `express()` treats them as a Section's first step:
  Lean and a flam's grace hit clamp to the line. A new command that cuts notes on its line sets it too.
- Portable text (`sectionName()` in `core/types.h`, `perform/queue_text.cpp`, `perform/arc_map.cpp`,
  `perform/oled_page.cpp`) is plain ASCII built without l10n, which the harness doesn't have. It is built with
  `perform::Text` (`perform/text.h`), not printf; its adds are out of line, since inlined at every use they cost
  about 2 KB of image.
- Drop-outs (spec §8.8, `Generator::dropOutRules()`) cut nothing: they only stop note-ons through `silenced()`, so
  sounding notes ring and a Sustain's notes end on its own bar line. A dropped Sustain needs no re-strike rule: its
  notes ended a bar or more before the drop-out's end, so `extend()` finds none and the voicing strikes again there.
  The draw reads `Arc::ahead()` with `waitingMove()`, as the chord walk does, so a drop-out never runs past a Section
  start known when it is drawn; one a later press moves is cut on the line instead.
- Octave shift (§7.3, #397): `Generator::partRegister()` is the one Register a flavour reads, the genre's
  `registerOrDefault()` moved by `Recipe::octave`. A new flavour that reads `spec.registerOrDefault()` itself ignores
  the shift, and `octave_spec` fails. A shift that lands moves the part's held voicing (`PartState::voicing`) with it
  in `apply()`, so voice leading goes on as a translation: without that, `leadVoicing()` places the new voicing near
  the old one's pitches inside the moved Register, not 12 apart. A part takes only the shifts that keep its reach
  in MIDI's range (`octaveRange()`, `pitchReach()` in `core/genre.h`): the reach is worked out from the Register
  alone, so a new flavour that plays outside the span (a note above `spanHigh`, a grace note an octave down) must
  widen `pitchReach()`, or `octave_spec`'s every-key reach case fails. A shift up alone doesn't show a voicing left
  behind in a Stab: its voicings sit low in their window, and voice-led from below the moved window takes the same
  notes. The re-voice spec runs each Sustain and Stab part down an octave too, which does.
