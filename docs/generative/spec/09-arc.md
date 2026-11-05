# 9. The Arc

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 9.1 Sections

The **Arc** is the Generator's movement through **Sections** around the Recipe's macro centre. There are five kinds: **Intro, Groove, Break, Build, Outro**. There are no others in v1, and genres differ through section rules, not new kinds.

- Sections are whole phrases long, using the genre's phrase length. A Build may be half a phrase.
- **Groove** is home, where the macros sit at the Recipe centre.
- A **Gap** isn't a Section. It is an optional Build ending (none, 1 beat or 1 bar, set by the genre) where the mix cuts out.
- A **Drop** isn't a Section. It is the event where a Build lands back in Groove: everything plays, and effective Energy and Density lift, then decay back to the centre over a phrase.

### 9.2 Automatic cycle

- The Arc runs on its own: **Groove → Break → Build → Drop → Groove**. A genre may also allow Groove → Build, weighted.
- A Start plays a one-phrase Intro first ([§13.2](13-perform.md#132-transport-start-and-end)). End plays a one-phrase Outro ([§13.2](13-perform.md#132-transport-start-and-end)).

### 9.3 Section lengths

- **Groove length** is drawn (seeded) from a per-genre phrase range given at Motion 25. Motion 50 halves it, Motion 0 means the Arc never moves on its own, with a linear scale between.
- **Break length** is drawn from a per-genre range. Motion doesn't scale it.
- **Build length** is a genre value in phrases or half a phrase.
- Example Groove ranges (*tuning*): Techno 4–8, Psytrance 3–6, Ambient 6–12 phrases.

### 9.4 What the Arc moves

The Arc moves **only effective Energy and effective Density**, as offsets from the Recipe centre per Section kind. Each kind has its own Energy offset and its own Density offset. A genre that doesn't set the Density offset uses its Energy offset.

| Kind | Offset shape |
|---|---|
| Intro | rising curve from low |
| Groove | 0 |
| Break | a dip |
| Build | a ramp from the Break level to above the centre |
| Drop (event) | a lift that decays back to 0 over a phrase |
| Outro | falling curve |

So a genre can have a Break that thins but stays loud, or a Build that fills in before it brightens. In the Intro, parts enter through their Density gates as effective Density rises, and in the Outro they leave as it falls.

### 9.5 Section rules

A genre's section rules (slot 9) hold everything else:

- Energy and Density offsets per kind, and the Intro and Outro curves.
- **Break keeps**: which parts and lanes still play in a Break.
- **Build**: length, roll lane and rate (16ths up to 32nds or triplets), whether the kick comes back, and Gap length.
- Groove and Break length ranges, and the Groove → Build weight.
- **FX lanes.** Riser plays across a Build. A sample Riser starts early enough to end exactly on the Drop, timed by the file end (pack risers are trimmed to land on it). A synth Riser holds one note over the Build and sweeps through its own row's sound controls ([§11.4](11-sound-controls.md#114-gestures)). Fall and Impact are one-shots that section rules place at a moment, such as the Drop, so they need no special timing. **Siren** is one held note starting on a Break's first step or on a Drop, lasting a genre-set number of bars, with its pitch sweep from the preset's own LFO. Density gates FX lanes like any lane, and Complexity adds nothing to them.
- **Ambient** uses these rules to swell and recede without a heard drop: a slow, long Build with no Gap, no roll and no Throw, and a Drop lift near 0 with a long decay. No Impact lane.

### 9.6 Force and hold

- **Force** a Section: it starts at the next phrase line. A forced Build starts at whichever boundary lets it end on a phrase line, so the Drop lands on one. Pressing the force again cancels it while queued.
- **Hold** keeps the current Section until released.
- Forcing works during a hold: the forced Section plays, then the hold applies wherever the Arc lands.

### 9.7 Transitions

**Genre change, Regenerate and Recipe LOAD** are all played the same way:

1. The incoming roster is preloaded into fresh Outputs in task context while the old genre keeps playing ([§10.7](10-sounds.md#107-when-sounds-load)).
2. Once loading finishes, the outgoing genre plays a Build, placed like a forced Build so that it ends on a phrase line. A slow load delays it by a phrase.
3. The new roster lands on a **Drop**. The tempo and swing glide is stretched over the Build, so the new tempo arrives on the Drop. The new key and scale take effect on the Drop.
4. Outgoing parts get **Throw** on their last hit, then release. Their Outputs are deleted once their tails finish.

Further rules:

- The player picks **BUILD INTO A DROP** or **HARD CUT AT PHRASE** in the confirm. A hard cut plays no Build and swaps at the next phrase line.
- A queued change can be cancelled until its Build starts. After that, cancelling is refused.
- If the Arc is held, the change still plays its Build, and the hold resumes in the new genre's Groove.
- **Regenerate** is a genre change to the same genre: rolled facts, every sound pick and the seed are rolled again, every take resets to 0, and the macros are kept. It never picks a sound a part or lane already has, as long as its list has another entry.
- A **genre change** resets the facts from the new genre (default or roll), rolls the sound picks for the new roster, draws a new seed and resets every take. It keeps the macros.
- A **Recipe LOAD** mid-set uses the file's seed, takes, facts and macros instead of rolling. The file therefore starts at a Drop, not an Intro ([§16.5](16-recipe-files.md#165-load)).
- The incoming roster always gets **fresh Outputs with unique names**, even when a pick matches a playing one, because two Outputs with the same preset name and folder break clip matching when the song is saved and loaded again. Two rosters fit in memory ([§20](20-budgets.md#20-budgets-and-measured-facts)).

**Key or scale change on its own** takes effect at the next phrase line, with no Build or Drop. If one is pending when an Arc Drop lands, it takes effect on the Drop.

Sources: [Perform interaction model][t15], [Density as a fourth macro][t24], [Automation][t14], [Sound generation][t13], [Perform control layout][t38], [Recipe files][t40], [Generative pack contents][t42], [Building Generator Kits in code][t28], [Measure memory and load times on a Deluge][t37], [Genre model][t11].

[t11]: https://github.com/gismya/deluge-generative/issues/11
[t13]: https://github.com/gismya/deluge-generative/issues/13
[t14]: https://github.com/gismya/deluge-generative/issues/14
[t15]: https://github.com/gismya/deluge-generative/issues/15
[t24]: https://github.com/gismya/deluge-generative/issues/24
[t28]: https://github.com/gismya/deluge-generative/issues/28
[t37]: https://github.com/gismya/deluge-generative/issues/37
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t40]: https://github.com/gismya/deluge-generative/issues/40
[t42]: https://github.com/gismya/deluge-generative/issues/42
