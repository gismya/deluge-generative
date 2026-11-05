# Musical review of the Generative mode spec

A review of the musical sections of [`docs/generative/spec.md`](https://github.com/gismya/deluge-generative/blob/generative/docs/generative/spec.md) at `9e83e480b`: 03 recipe, 04 genres, 05 parts and lanes, 07 pitch, 08 rhythm and expression, 09 the Arc, 11.3–11.5, 18 and the musical items in 23. A subagent did the review, reading as a producer and arranger of the five v1 genres who also knows generative and algorithmic composition. Nothing was listened to. The report is kept as written. Tickets on the [wayfinder map](https://github.com/gismya/deluge-generative/issues/1) link to its findings by number (B1–B7, W1–W17).

## Blocking for the split

**B1. The chord-walk rules break the static-harmony genres.** §7.4, with §4.5.
- **What's wrong:** four walk rules collide whenever a phrase holds only one chord: every phrase starts on home, home is excluded on a phrase's last chord, the walk can't repeat the current chord, and there's a cadence period. Ambient (8+ bars per chord in an 8-bar phrase) has one chord per phrase, so it either sits on I forever or breaks a rule. Techno ("a drone or one chord") and Psy ("almost no chord changes") can't be written as a graph at all. The "can't repeat" rule forces a move on every chord. Loop basses are written as offsets from the chord, so any forced move drags the psy pedal bass with it. The spec also never says how the cadence period relates to the phrase-end rules.
- **Why it matters:** three of the five genres can't follow their briefs.
- **Fix:**
  - Add a genre `repeat weight`, default 0, so the walk can stay put.
  - Apply start-on-home and exclude-home only when a phrase holds 2 or more chords.
  - Let harmonic rhythm be longer than a phrase.
  - Count the cadence period across phrases.
  - State that an empty Harmony slot means "always the home chord".

**B2. Fill kinds are never defined.** §8.7, §4.2 slot 6, §5.2.
- **What's wrong:** the fill kinds in slot 6 are referenced but there's no list. The same goes for the "drum change" that Answers key on. Nothing says whether Fill affects the Drums only or every part, or how a fill ranks against the Build roll and the Gap in a phrase's last bar.
- **Why it matters:** the Techno kick drop at phrase ends depends entirely on this, and so do Dub's return fills and every phrase-end turnaround.
- **Fix:** a fixed Generator-wide set, each kind a lane mask plus a length in beats:
  - **Mute** (lanes silent for N beats, which gives the kick drop);
  - **Swap** (play the pool's fill pattern for that lane);
  - **Roll** (a short ratchet ramp);
  - **Tom run**.

  Precedence: Gap > Build roll > fill.

**B3. How Complexity picks patterns, and when patterns change, are left to the implementer.** §8.1, §5.2.
- **What's wrong:** the spec says Complexity picks material "aimed at a genre-set band", but there's no syncopation metric and no selection rule. It doesn't say when a drum part changes pattern (per Section, per phrase, under Motion). It doesn't say how Complexity shapes the generated lines (Loop, Arp, Motif) at all.
- **Why it matters:** two implementers would build two different instruments. There's also a combination that produces nonsense: high Complexity with low Density. Syncopated hits normally carry low importance, so they're thinned first and Complexity goes silent.
- **Fix:**
  - Hand-tag each pool entry with an integer complexity tier (0–3). The genre's band maps effective Complexity linearly onto a tier, and the pick is keyed on (part, take, Section index).
  - Pattern changes happen only at Section starts, plus a Motion-rated draw at phrase lines.
  - In Loop generation, Complexity sets the chance of off-beat steps, rests, slides and octave jumps.
  - Authoring rule plus a harness check: a tier's syncopating hits must rank in the top half of that pattern's importance.

**B4. Dub's main motion, delay throws and drop-outs, can't be expressed.** §9.5, §11.2, §11.4, §10.6.
- **What's wrong:**
  - Throw fires only before a Break or Gap, during a transition, or on a Siren release.
  - Space on kit rows is reverb send only.
  - Synth delay feedback moves ±4, and only if the preset already uses delay.
  - Load-time settings never set up a delay.
  - Drop-outs exist only as Breaks, which last whole phrases (8 bars at 75 BPM is about 25 s).
- **Why it matters:** a roots dub mix is mostly 1–4-bar mutes of the skank, melodica or drums, with the last skank or rim hit thrown into a synced echo. Without that, Dub is just a slow one-drop loop.
- **Fix:**
  - Add a **Drop-out** event: Motion-rated, drawn at bar lines, mutes a seeded subset of non-anchor parts and lanes for 1–4 bars, with a Throw on the last hit before it.
  - Add a genre load-time **delay setup** per part and kit (synced rate such as dotted 8th, a nonzero home so the buffer is allocated before playback).
  - Give Throw a delay-amount target with genre depth, rather than the ±4 clamp.

**B5. Build and Break granularity and contents.** §9.1, §9.5, glossary "Section".
- **What's wrong:**
  - The minimum Build is half a phrase. Dub needs a 1-bar fill-in return, and House often wants 4 bars.
  - "Whether the kick comes back" is ambiguous (when, and from what?).
  - There's no **Build keeps**. Gate-0 parts that a Break silenced reappear on the Build's first step, so the psy bass is back for the whole build.
  - Nothing says where the ramp starts after Groove → Build, when there's no Break level.
  - The glossary says every Section is "whole phrases long", which contradicts the half-phrase Build.
- **Fix:**
  - Build length in bars (1, 2, 4, 8, 16), dividing the phrase.
  - A Build keeps mask (default: same as Break keeps).
  - A kick schedule as an integer: "kick out for the last N bars", where 0 means it stays in.
  - Groove → Build ramps from 0.
  - Fix the glossary.

**B6. Transitions ignore what the incoming genre is.** §9.7.
- **What's wrong:**
  - The tempo glide is stretched over the Build. Psy into Dub (142 to 75 BPM) is then a build that slows down as it rises, which sounds like a turntable stopping.
  - The transition always plays the outgoing genre's Build. Techno into Ambient plays a roll and a Gap, then lands on Ambient's near-zero Drop, which is an anticlimax.
- **Fix:**
  - If the tempo moves by more than about 8% (tuning), don't glide. Jump at the Drop, inside the Gap if there is one.
  - When the genre change rolls the new tempo, prefer the value nearest the old tempo ×½, ×1 or ×2 (142 → 71 is clean).
  - If the incoming genre is flagged no-drop (lift below a threshold), the outgoing genre plays a Break-style fade (Close and Wash, Density ramping down) instead of a Build.

**B7. Chord extensions have no rule, and the plain in-scale derivation sounds wrong.** §7.4, slot 7 "chord kinds".
- **What's wrong:**
  - Stacking in-scale 9ths gives ♭9s on Phrygian i (every Techno and Psy Phrygian roll), Minor ii and v, and Major iii and vii.
  - 11ths clash with the 3rd on major chords.
  - With at most 4 notes, nothing says which tone a 9th replaces.
  - The House brief requires 7th and 9th stabs, but colour comes only from Complexity, so House at low Complexity plays bare triads.
- **Fix:**
  - An integer extension guard: reject any extension a semitone above a chord tone.
  - A genre colour floor: a minimum extension tier at Complexity 0.
  - A fixed priority (7 > 9 > sus4 > add6), with 9ths replacing the 5th.

## Worth fixing

**W1. Lean can't move the "&" on its own.** §8.5, glossary.
- **What's wrong:** "off-beat 16ths" is never defined, though swing implies the odd 16ths. Under that reading, the 8th-offbeat open hat in Techno and House is an on-beat 16th, so pushing or dragging it also moves the downbeats.
- **Fix:** four Lean values per part or lane, one per 16th position within the beat.

**W2. Anchors aren't defined.** §5.2, §8.1.
- **What's wrong:** "backbeat" is undefined. Dub's is step 8, House's is steps 4 and 12. The jitter around the Density threshold can drop max-importance hits, and a four-on-the-floor kick with uneven importance turns half-time at low Density.
- **Fix:** a per-lane protected-step mask that is exempt from mutation, thinning and jitter. Only gates, keeps and fills can remove those hits.

**W3. Density jitter keying and gate flicker.** §8.1, §5.6.
- **What's wrong:** nothing says what the jitter is keyed on. If it's keyed per bar, hats flicker every bar instead of looping. Gates on a moving effective Density can also cut parts mid-bar.
- **Fix:** key jitter on (lane, step in pattern, phrase). Evaluate gates at bar lines with a ±2 hysteresis.

**W4. Arc offsets vanish at the extremes.** §9.4.
- **What's wrong:** with the centre at 50 there's no room for the Drop lift, and at 0 the Break dip does nothing.
- **Fix:** scale each offset into the headroom available on its side (integer).

**W5. Fixed lengths contradict Ambient's long decay.** §9.1, §9.2.
- **What's wrong:** the Drop "decays over a phrase" is fixed, which contradicts Ambient's "long decay". Intro and Outro are fixed at one phrase, while DJ-style Techno intros are 2–4 phrases and Ambient's are longer.
- **Fix:** make the Drop decay and the Intro and Outro lengths genre data, in phrases.

**W6. The roll rate contradicts itself.** §8.3 vs §9.5.
- **What's wrong:** §8.3 forces the ratchet up to 4 per 16th (64ths), but §9.5 says the rate tops out at 32nds or triplets. Triplets can't be built from ratchets that stay inside one 16th.
- **Fix:** pick 16ths → 32nds, drop triplets, and make the stage boundaries genre data (default 1/2, 1/4, 1/8 of the Build remaining).

**W7. The flam is backwards now that Lean can go negative.** §8.2.
- **What's wrong:** the main hit lands 1–3 ticks late, so it moves off the grid, while the grace hit sits where the note should be. 1–3 ticks at 142 BPM is 4–13 ms, too tight to hear as a flam.
- **Fix:** put the main hit at the Lean position and the grace hit 2–6 ticks earlier, clamped to the grid on a Section's first step.

**W8. No way to mark a phrase position in a drum lane.** §5.4, §9.5.
- **What's wrong:** there's no Crash on the Drop or on phrase downbeats. Drum patterns can't see where they are in the phrase.
- **Fix:** a phrase-accent section rule that places a one-shot on a named drum lane at the Drop and at Groove phrase starts, with a Motion-rated chance.

**W9. No deliberate filter sweep.** §11.4, §11.5.
- **What's wrong:** Psy's swept leads and Techno's acid cutoff movement are deliberate ramps over 8–32 bars. Drift is a random walk.
- **Fix:** a **Sweep** gesture: a phrase-locked triangle on Brightness, with genre-set period, depth and seeded phase.

**W10. The interplay rules are ambiguous.** §5.5.
- Is Lock a filter or a generator?
- What counts as a "rest" when the target plays all the time (a 4/4 kick, an offbeat bass)? House Lead Answer Bass then never plays.
- Is the "planned" pattern taken before or after Density thinning?
- What's the strength formula?
- **Suggested:** Lock filters; a rest is a half-bar with at most N target onsets; the rule reads the pattern after thinning and before mutes; strength = strength + sensitivity × (C − C_default) / 50.

**W11. Register ambiguities.** §7.3.
- **What's wrong:** if the span is relative to the anchor, a tonic sitting 5 semitones below the anchor folds up an octave. The 6-semitone tie isn't resolved. A span of 0..+14 stops dub and house basses from dropping to the 5th or ♭7 below the root.
- **Fix:** make the span relative to the tonic, break the tie downwards, and use a starting Bass span of −5..+12.

**W12. Gestures are switched per part, not per Section kind.** §11.4.
- **What's wrong:** Rise is shared between the Intro and the Build, and Close and Wash between the Break and the Outro. A Techno Intro then gets a high-passed kick, when DJs want it full.
- **Fix:** key gesture switches on (gesture, Section kind).

**W13. One knob for two jobs, and an ambiguous length formula.** §9.3.
- **What's wrong:** Motion couples how eager the Arc is with the acid mutation rate, so "busy acid, long sections" isn't reachable. Separately, "linear between" is ambiguous for Motion 0..25. It's unclear whether length or rate is linear.
- **Fix:** state the length formula (e.g. length = base × 25 / Motion, capped). Consider a separate genre weight for Arc eagerness.

**W14. Polymeter: dropped from v1, still in the glossary.** §23.2.
- **What's wrong:** §23.2 drops it, but the Complexity entry in `GLOSSARY.md` still lists it. Ambient's most natural generative device (loops of unrelated lengths) and Techno's 3-against-4 percussion go with it.
- **Fix:** fix the glossary. A cheap version is a per-lane or per-Motif loop length in steps, reset at phrase lines.

**W15. Ambient's swell is Brightness alone.** §5.2 Sustain, §8.4.
- **What's wrong:** pads held for 8+ bars don't respond to velocity after the note starts, and level isn't automated. Nothing says when Sustain re-articulates or what Density does to a part that plays one note per chord.
- **Fix:** specify re-articulation at the harmonic rhythm plus an optional bar-rate retrigger, and let Density add voicing notes.

**W16. Genre changes roll a random key.** §9.7.
- **Fix:** weight the new key's roll towards the same root, the relative key, or a fifth either way of the playing key.

**W17. The Dub pack is missing sounds.** §18.2.
- No skank or bubble organ preset (Chords is the thinnest role on the card).
- No presets carrying a synced delay for throws.

## Per-genre check

- **Techno.** The kick, offbeat hats and acid Loop are well covered. Riskiest gaps: the "one chord" harmony the walk can't hold (B1) and the kick drop with nowhere to live (B2). Acid filter motion is drift only (W9).
- **House.** It has the best fit: Chord graph, Lock(off) bass, claps and swing. Riskiest gaps: plain-triad stabs at low Complexity and ♭9 colour (B7), and Lean that can't move the "&" (W1). Answer Bass may also mute the lead (W10).
- **Dub.** It's the weakest. The one-drop, skank and Loop bass work as data, but its main motion (B4) and its short Builds and drop-outs (B5) don't exist, and its brief gives no harmony, phrase or Groove data. As specified it will sound like slow reggae-ish loops, not dub.
- **Psytrance.** The rolling bass through Avoid Kick 100% with a 16-bar phrase is right. Riskiest gaps: chord moves pulling the pedal bass (B1), and Build-keeps and kick-out timing in long builds (B5). There's also no deliberate lead sweep (W9).
- **Ambient.** The gates, slow swell and no Gap are fine. Riskiest gaps: the chord walk can't move (B1), and the fixed one-phrase Drop decay contradicts its brief (W5). Its swell is Brightness only (W15), and a transition into Ambient builds into nothing (B6).

## Fine as tuning data

- Groove, Break and Build ranges, and the Groove → Build weight.
- Threshold curve and jitter depth.
- Mutation curve (Mutate/100)².
- Guard factor 0.15 and the colour-row weights.
- Pentatonic rows.
- Gate thresholds (Ambient 35, shaker 30).
- Energy velocity weight per lane (set the kick near ×0 so the kick level stays the mix's reference).
- Energy's sign on Space and Decay for Ambient and Dub: weight ×0, not flipped.
- The answer-transform set and its rates.
- Velocity spread amounts and modes.
- Ratchet counts and floors and ceilings per lane (the kick's ceiling at 0).
- Drift depth and period, with per-lane weights (keep kick Brightness drift at 0).
- Gesture depths.
- Siren length.
- Gap length per genre.
- Harmonic rhythm values, and the 2–4 bar House range.
- Bass anchors.
- Swing amounts.
- Palettes and the 165 Acid Arp check.
