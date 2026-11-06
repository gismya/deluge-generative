# 9. The Arc

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 9.1 Sections

The **Arc** is the Generator's movement through **Sections** around the Recipe's macro centre. There are five kinds: **Intro, Groove, Break, Build, Outro**. There are no others in v1, and genres differ through section rules, not new kinds.

- Intro, Groove, Break and Outro are whole phrases long, using the genre's phrase length. A Build is a set number of bars that ends on a phrase line ([§9.3](#93-section-lengths)). A Build shorter than a phrase takes the end of the phrase before it, so the phrase grid never moves.
- **Groove** is home, where the macros sit at the Recipe centre.
- A **Gap** isn't a Section. It is an optional Build ending (none, 1 beat or 1 bar, set by the genre) where the mix cuts out.
- A **Drop** isn't a Section. It is the event where a Build lands back in Groove: everything plays, and effective Energy and Density lift, then decay back to the centre over the genre's Drop decay ([§9.3](#93-section-lengths)).

### 9.2 Automatic cycle

- The Arc runs on its own: **Groove → Break → Build → Drop → Groove**. A genre may also allow Groove → Build, weighted.
- A Start plays the genre's Intro first ([§13.2](13-perform.md#132-transport-start-and-end)). End plays its Outro ([§13.2](13-perform.md#132-transport-start-and-end)).

### 9.3 Section lengths

- **Groove length** is drawn (seeded) from a per-genre phrase range given at Motion 25. Motion 50 halves it, Motion 0 means the Arc never moves on its own, with a linear scale between.
- **Break length** is drawn from a per-genre range. Motion doesn't scale it.
- **Build length** is in bars: 1, 2, 4, 8, 16 or 32. It must divide the phrase or be a whole multiple of it, so the Drop lands on a phrase line. It is drawn (seeded) from a weighted list in the genre when the Build is scheduled or forced. A list with one entry is a fixed length. Motion doesn't scale it. A harness spec checks every length against the genre's phrase length.
- **Build placement.** A Build of a phrase or longer starts on a phrase line. A shorter one takes the last bars of the phrase before the Drop, so it shortens the Section before it: a 1-phrase Break followed by a 4-bar Build in an 8-bar phrase plays 4 bars of Break.
- **Drop decay** is a genre value in phrases, 1 by default. If the next Section starts before the decay ends, the lift stops there and that Section's offsets take over. A harness spec checks that the decay fits in the genre's shortest Groove at Motion 50.
- **Intro and Outro lengths** are fixed genre values in phrases, 1 by default.
- Example Groove ranges (*tuning*): Techno 4–8, Psytrance 3–6, Ambient 6–12 phrases.
- Example Build and length values (*tuning*). Build keeps is the same as Break keeps in every genre:

| Genre | Build lengths (bars ×weight) | Build cut | Roll top stage | Drop decay (phrases) | Intro / Outro (phrases) |
|---|---|---|---|---|---|
| Techno | 8 ×2, 16 ×1 | Kick, last 1 bar | 32nds | 1 | 2 / 1 |
| House | 4 ×1, 8 ×2 | none | 32nds | 1 | 1 / 1 |
| Dub | 1 | none | 16ths | 1 | 1 / 1 |
| Psytrance (16-bar phrase) | 16 ×2, 32 ×1 | Kick and Bass, last 2 bars | 32nds | 1 | 1 / 1 |
| Ambient | 16 | none | no roll | 3 | 2 / 2 |

### 9.4 What the Arc moves

The Arc moves **only effective Energy and effective Density**, as offsets from the Recipe centre per Section kind. Each kind has its own Energy offset and its own Density offset. A genre that doesn't set the Density offset uses its Energy offset.

| Kind | Offset shape |
|---|---|
| Intro | rising curve from low |
| Groove | 0 |
| Break | a dip |
| Build | a ramp from the offset at the Build's start (the Break dip, or 0 after Groove → Build) to above the centre |
| Drop (event) | a lift that decays back to 0 over the Drop decay |
| Outro | falling curve |

Energy and Density each ramp from their own offset at the Build's start, so no Build jumps on its first step, whether it follows a Break or a Groove, or is forced during the Intro. So a genre can have a Break that thins but stays loud, or a Build that fills in before it brightens. In the Intro, parts enter through their Density gates as effective Density rises, and in the Outro they leave as it falls.

### 9.5 Section rules

A genre's section rules (slot 9) hold everything else:

- Energy and Density offsets per kind, the Intro and Outro curves and lengths, and the Drop decay ([§9.3](#93-section-lengths)).
- **Break keeps**: which parts and lanes still play in a Break.
- **Build keeps**: which parts and lanes play in a Build, in the same form as Break keeps and by default the same set. It applies to every Build: after a Break, after Groove → Build, forced, and in a transition. The roll lane and the Riser lane always play. Parts in the set still need their Density gate, so the ramp brings them in one at a time.
- **Build cut**: parts and lanes that stop for the last N bars before the Drop. The default set is Kick, and N = 0 means no cut. The Gap still silences everything at the very end, inside those bars. The roll lane can't be in the cut, which a harness spec checks.
- **Build**: the length list ([§9.3](#93-section-lengths)), the roll lane, the roll's top stage and stage fractions ([§8.3](08-rhythm-and-expression.md#83-the-build-roll)), and Gap length.
- Groove and Break length ranges, and the Groove → Build weight.
- **FX lanes.** Riser plays across a Build. A sample Riser starts early enough to end exactly on the Drop, timed by the file end (pack risers are trimmed to land on it). A synth Riser holds one note over the Build and sweeps through its own row's sound controls ([§11.4](11-sound-controls.md#114-gestures)). Fall and Impact are one-shots that section rules place at a moment, such as the Drop, so they need no special timing. **Siren** is one held note starting on a Break's first step or on a Drop, lasting a genre-set number of bars, with its pitch sweep from the preset's own LFO. Density gates FX lanes like any lane, and Complexity adds nothing to them.
- **Phrase accent.** The genre names one accent lane (usually Crash, maybe Ride or Open hat, or none). The accent is a single hit on that lane on a phrase's first step: always on a Drop, always after a fill that covered the phrase's last step ([§8.7](08-rhythm-and-expression.md#87-fill)), and on other Groove phrase starts with a seeded chance that Motion raises. It follows the lane's Density gate and Replace pairs like any hit, so a genre may add "Crash replaces Closed hat". The Impact lane on a Drop is a separate rule.
- **Fills.** Fills play only in Groove phrases and the Intro's last phrase, and give way to the Gap and the Build roll ([§8.7](08-rhythm-and-expression.md#87-fill)).
- **Drop-outs.** Drop-outs play only in Groove phrases ([§8.8](08-rhythm-and-expression.md#88-drop-out)). Breaks remain the long thinning.
- **Ambient** uses these rules to swell and recede without a heard drop: a slow, long Build with no Gap, no roll and no Throw, and a Drop lift near 0 with a long decay. No Impact lane.

### 9.6 Force and hold

- **Force** a Section: it starts at the next phrase line. A forced Build draws its length when forced and starts at the earliest bar line still ahead that lets it end on a phrase line, so the Drop lands on one. A 4-bar Build forced in bar 6 of an 8-bar phrase starts at bar 13. Pressing the force again cancels it while queued.
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

Sources: [Perform interaction model][t15], [Density as a fourth macro][t24], [Automation][t14], [Sound generation][t13], [Perform control layout][t38], [Recipe files][t40], [Generative pack contents][t42], [Building Generator Kits in code][t28], [Measure memory and load times on a Deluge][t37], [Genre model][t11], [Fill kinds and phrase-end events][t65], [Dub's drop-outs and delay throws][t67], [Build shape and Section lengths][t68].

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
[t65]: https://github.com/gismya/deluge-generative/issues/65
[t67]: https://github.com/gismya/deluge-generative/issues/67
[t68]: https://github.com/gismya/deluge-generative/issues/68
