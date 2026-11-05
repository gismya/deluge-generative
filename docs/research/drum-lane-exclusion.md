# Research: drum lanes that exclude or replace each other on a step

Question: in the genres Generative mode has or may get, which pairs of drum
voices should not sound on the same 16th, or replace one another, beyond Closed
hat and Open hat? Which of these can the Deluge's single CHOKE group express,
and which need the Generator to resolve them? This feeds the pending choice
between (b) independent lanes plus one fixed hat rule and (c) a per-genre table
of lane pairs.

Vocabulary follows `GLOSSARY.md` (Lane, Part, Genre, Density).

## Summary

- Outside the hats, the pairs that sources describe as "not together" all come
  from **one drummer having one hand per stroke**: a crash replaces the hi-hat
  (or ride) stroke on the beat it lands on, the snare stroke replaces the hat
  stroke in two-handed 16th patterns, and ride and hi-hat are alternative
  timekeepers for the same hand. They matter in genres built on a played kit or
  sampled breaks (Dub, breakbeat, jungle/DnB, lo-fi, synthwave), and not in
  machine-programmed genres, where sources layer voices freely.
- Snare, clap and rim are **layered** in house, trap, dubstep and reggaeton.
  The only snare-family exclusion found is reggae's **sidestick _or_ snare** on
  the backbeat, which is a role choice (which lane carries the backbeat) more
  than a per-step clash.
- Kick and snare **apart** (DnB two-step, dubstep and trap half-time,
  breakbeats) is a property of the patterns, not an exclusion rule: one drop
  _requires_ kick and sidestick together on beat 3. Step-importance tables
  already express it.
- Kick against bass (psytrance rolling bass, trap's 808) is a strong rule, but
  it is between parts, so out of scope here.
- The Deluge choke group can only express "every CHOKE drum cuts every other
  CHOKE drum, last note-on wins". That fits closed/open/pedal hat tails and
  nothing else on this list. Every same-step rule has to be done by the
  Generator.

## Genre by genre

Strength: **hard** = sources treat breaking it as wrong; **common** = usual
practice, sources describe it as the default; **stylistic** = a choice some
producers make.

| Genre | Pair | Rule | Strength | Source |
|---|---|---|---|---|
| All | Closed hat / Open hat | Closed hat cuts the open hat's ring | hard | [TR-8S guide][tr8s], [SFZ][sfz] |
| Techno, House, Trance, Minimal | Clap / Snare | Layered on 2 and 4, not exclusive | common (layering) | [Attack 909][a909], [SOS dubstep][sosdub] |
| House | Kick / Tom | Tom may land on a kick step (909 tom on 16ths 3, 9, 11 over four-to-the-floor) | no exclusion | [Attack rolling deep house][adh] |
| Techno | Ride / Closed hat | Both programmed in one 909 pattern (ride on offbeats) | no exclusion | [Attack 909][a909] |
| Psytrance | Kick / Bass | Alternate, never together | hard, cross-part | [NI community][nipsy], [Bonedo][bonedo] |
| Dub (one drop, rockers, steppers) | Rim (sidestick) / Snare | Backbeat on 3 is "sidestick or snare"; a fill moves a sidestick hit to the snare | common (role choice) | [MusicRadar one drop][m1d], [Wikipedia][w1d] |
| Dub | Kick / Rim on 3 | Must coincide (one drop's defining hit) | hard (anti-exclusion) | [MusicRadar one drop][m1d], [Wikipedia][w1d] |
| Dub | Crash / Hats | Crash layered with a snare fill at a phrase end | stylistic | [MusicRadar one drop][m1d] |
| Breakbeat, Jungle/DnB (Amen) | Ride / Closed hat | Ride carries the time; hat is a pedal chick on every beat, under the ride | common | [MusicRadar Amen][mamen] |
| Breakbeat, Jungle/DnB (Amen) | Crash / Ride | Crash briefly replaces the ride pattern at the end of the break | common | [MusicRadar Amen][mamen] |
| DnB | Kick / Snare | Two-step and half-time keep them apart | pattern, not rule | [MusicRadar DnB][mdnb] |
| Dubstep | Kick / Snare | Snare "between the kick drums" | pattern, not rule | [SOS dubstep][sosdub] |
| Dubstep | Clap / Snare | Layered, transients aligned | common (layering) | [SOS dubstep][sosdub] |
| Dubstep | Two LF hits | Don't let low kick hits overlap | common (mix) | [SOS dubstep][sosdub] |
| Trap / hip-hop | Clap / Snare | Snare on 3, clap layered on it, a second clap on 2 and 4 | common (layering) | [MusicRadar trap][mtrap] |
| Trap | Hat rolls | Rolls are speed changes inside the closed-hat stream, not a second voice | n/a (one lane) | [MusicRadar trap][mtrap] |
| Trap | 808 kick / Bass | One combined part | hard, cross-part | [MusicRadar trap][mtrap] |
| Reggaeton / dembow | Clap / Snare | Clap doubles a snare hit | common (layering) | [MusicRadar reggaeton][mreg] |
| UK garage / 2-step | Rim / Snare | Rimshots take the upbeats and syncopations | stylistic | [Attack UKG][aukg] |
| Electro (808) | Tom/Conga, Rim/Claves, Clap/Maracas | The 808 can only play one of each pair per pattern | machine limit, stylistic | [Roland 808 spec][r808], [TR-08 manual][tr08] |
| Drummer-feel genres (Dub, breaks, lo-fi, synthwave) | Crash / Hat or Ride | Crash on a downbeat replaces that hat stroke | common | [MusicRadar limbs][mlimbs], [Stereo Stickman][ss] |
| Same | Snare / Hat (16ths) | Snare replaces the hat on the backbeat | common | [MusicRadar basic beat][mbasic] |
| Afro house, Footwork, IDM, Ambient | none found beyond hats | | | [Attack deep afro house][aafro] |

No source checked gave pair rules for footwork, IDM, trance, minimal or
synthwave specifically. Synthwave and lo-fi are listed under "drummer-feel"
by inference (played or played-sounding kits), not from a source.

## Notes on the findings

**Hats.** On classic machines closed-mutes-open is "the standard for every
classic drum machine" ([MusicRadar TR-8S][tr8s]). The TR-808's cowbell, cymbal
and both hats share one bank of six square-wave oscillators ([Werner, Abel and
Smith 2014][werner]). Real hi-hats mute one way: "hits which are more open do
not mute hits which are more closed", and a pedal chick mutes everything
([SFZ cymbal muting][sfz]). A pedal hat lane, if added, belongs in the same
group. The Roland TR-909 spec sheet lists "Closed / Open Hi-Hat" as one
instrument with shared Level and Tune ([Roland][r909]); whether its closed hat
cuts the open hat in hardware was not confirmed from a primary source.

**The drummer rules.** "We only have 4 limbs ... 2 of those limbs can only play
kick and hi hat foot pedal sounds" ([MusicRadar][mlimbs]). Consequences in the
sources: "Most drummers hitting a crash on the downbeat will use their hi hat
hand ... the hi hat doesn't play the downbeat" ([MusicRadar][mlimbs]); in
two-handed 16ths "the snare hit replac[es] the hi-hat on the backbeat rather
than being concurrent with it" ([MusicRadar][mbasic]); drummers keep a pattern
and move it between hi-hat and ride by section ([Wikipedia, Ride
cymbal][wride]). All three are **directional replacements** (A sounds, B drops
out), not symmetric exclusions and not ducking.

**Reggae.** One drop puts "simultaneous kick and sidestick or snare hits" on
beat 3, and a variation moves a sidestick hit "to the snare and layering in a
crash cymbal" ([MusicRadar][m1d]). Wikipedia, citing Bass Player (1996), says
the stroke is "usually a click produced by cross-sticking" ([Wikipedia][w1d]).
Steppers keeps the rim click with kick on all four beats ([Modern
Drummer][mdstep]). So in Dub, Rim and Snare both on the backbeat step would be
wrong, but the fix can be in the step tables (only one of them weights that
step) rather than a pair rule.

**Choke beyond hats.** Machines use mute assignments for other pairs too: guiro
tap cuts the slide, and "assigning the snare to mute the crash" is suggested as
a tip ([MusicRadar TR-8S][tr8s]). These cut a ringing tail on a later step;
they are not same-step exclusions.

## What the Deluge choke group can express

From the firmware: a drum in `PolyphonyMode::CHOKE` calls `Kit::choke()` on
note-on, which fast-releases every CHOKE drum in the kit
(`src/deluge/processing/sound/sound_drum.cpp`, `SoundDrum::noteOn` and
`SoundDrum::choke`; `src/deluge/model/instrument/kit.cpp`, `Kit::choke`).
So the group is symmetric, all-to-all, and the last note-on wins.

| Rule | Expressible by the one choke group? |
|---|---|
| Closed hat cuts open hat's tail (later step) | Yes (open hat also cuts closed, harmless because the closed hat is short) |
| Pedal hat cuts open hat | Yes, if added to the same group |
| Snare mutes crash tail | No: the snare would also be cut by, and cut, the hats |
| Crash replaces hat on a step | No: choke cuts tails, it doesn't drop a note, and the crash would be cut by the next hat |
| Ride or hat as timekeeper | No: a section-level choice |
| Rim or snare on the backbeat | No |
| Any same-step rule, including "Open hat wins" | No: both notes fire on one step and the order of note-on decides which survives, so the Generator must drop one |

## Recommendation

The evidence does not call for a general symmetric exclusion table, and none
of the sources describe **ducking** between drum lanes (ducking appears only
between kick and bass, across parts).

For v1, **(b) is enough**. Techno, House, Psytrance and Ambient are
machine-programmed and layer freely apart from the hats. Dub is the one v1
genre with drummer rules, and its two that matter (Rim or Snare on the
backbeat, crash replacing a hat stroke) are a stylistic nicety the step tables
can cover: give the backbeat step to one of Rim and Snare, and keep the Crash
off steps where the hats are strong, or accept the layering.

The later genres built on breaks or a played kit (breakbeat, jungle/DnB,
lo-fi, synthwave) would want the same two or three **directional "A replaces
B on this step"** pairs: Crash over Closed hat/Open hat/Ride, Snare over
Closed hat in two-handed 16th grooves, and Ride or Closed hat as the
section's timekeeper. If the step-resolution code is one function that
already applies "Open hat over Closed hat", adding a short per-genre list of
directional pairs later is cheap. So: ship (b), shaped so it is the first
entry of a (c)-style directional list rather than a special case, and add the
list when the first drummer-feel genre arrives.

## Sources

[a909]: https://www.attackmagazine.com/technique/tutorials/the-beginners-guide-to-the-tr-909/
[aafro]: https://www.attackmagazine.com/technique/beat-dissected/deep-afro-house/
[adh]: https://www.attackmagazine.com/technique/beat-dissected/rolling-deep-house/
[aukg]: https://www.attackmagazine.com/technique/beat-dissected/uk-garage/
[bonedo]: https://www.bonedo.de/artikel/psytrance-produzieren-workshop-1-kick-bass-percussion/
[m1d]: https://www.musicradar.com/how-to/how-to-program-a-typical-one-drop-reggae-beat-and-add-fills
[mamen]: https://musicradar.com/tuition/tech/how-to-program-an-amen-style-break-637374
[mbasic]: https://www.musicradar.com/tuition/tech/how-to-program-realistic-drum-parts-basic-beat-607639
[mdnb]: https://www.musicradar.com/how-to/how-to-program-6-different-jungle-and-drum-n-bass-grooves
[mdstep]: https://www.moderndrummer.com/article/february-2019-reggae-101-the-steppers-beat/
[mlimbs]: https://www.musicradar.com/tutorials/music-production-tutorials/this-may-sound-pretty-obvious-but-we-only-have-4-limbs-so-as-a-drummer-this-means-the-most-drum-elements-we-can-play-at-any-one-time-is-4-how-to-think-like-a-human-drummer-when-building-software-beats
[mreg]: https://www.musicradar.com/news/beat-building-how-to-produce-a-reggaeton-beat
[mtrap]: https://www.musicradar.com/tuition/tech/10-tricks-every-trap-producer-should-know-638684
[nipsy]: https://community.native-instruments.com/discussion/comment/151033
[r808]: https://support.roland.com/hc/en-us/articles/201963539-TR-808-Technical-Specifications
[r909]: https://support.roland.com/hc/en-us/articles/201921899-TR-909-Technical-Specifications
[sfz]: https://sfzformat.com/tutorials/cymbal_muting
[sosdub]: https://www.soundonsound.com/techniques/dubstep-drums
[ss]: https://stereostickman.com/beginners-guide-to-programming-realistic-drums/
[tr08]: https://www.manualslib.com/manual/3013859/Roland-Tr-08.html
[tr8s]: https://musicradar.com/how-to/how-to-master-the-roland-tr-8s
[w1d]: https://en.wikipedia.org/wiki/One_drop_rhythm
[werner]: https://ccrma.stanford.edu/papers/tr-808-cymbal-physically-informed-circuit-bendable-digital-model
[wride]: https://en.wikipedia.org/wiki/Ride_cymbal

- Attack Magazine, Eric Brünjes, "The Beginners Guide To The TR-909", 2024 ([a909])
- Attack Magazine, "Beat Dissected: Deep Afro House" ([aafro]); "Rolling Deep House", 2012 ([adh]); "UK Garage", 2019 ([aukg])
- Bonedo, "Psytrance produzieren Workshop #1" ([bonedo])
- Computer Music / MusicRadar: one drop ([m1d]), Amen-style break ([mamen]), realistic drums basic beat ([mbasic]), six jungle and DnB grooves ([mdnb]), "4 limbs" ([mlimbs]), reggaeton ([mreg]), trap ([mtrap]), Roland TR-8S ([tr8s])
- Modern Drummer, Tommy Benedetti, "Reggae 101: The Steppers Beat", 2018 ([mdstep])
- Native Instruments community, psytrance kick and bass ([nipsy])
- Roland, TR-808 and TR-909 technical specifications ([r808], [r909]); TR-08 owner's manual ([tr08])
- SFZ Format, "Cymbal muting" ([sfz])
- Sound On Sound, Rob Talbott, "Dubstep Drums", 2014 ([sosdub])
- Stereo Stickman, "Beginner's Guide To Programming Realistic Drums" ([ss])
- Werner, Abel and Smith, "The TR-808 Cymbal: a Physically-Informed, Circuit-Bendable, Digital Model", ICMC/SMC 2014 ([werner])
- Wikipedia, "One drop rhythm" (cites Bass Player 1996, Drums For Dummies 2011) ([w1d]); "Ride cymbal" ([wride])
