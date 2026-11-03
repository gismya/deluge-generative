# PERF-1 internals

Research for [#5](https://github.com/gismya/deluge-generative/issues/5), under the map [#1](https://github.com/gismya/deluge-generative/issues/1).

**Question.** How does PERF-1 generate its music? This covers genres and pieces, the Mood dial, the chord and harmonic-rhythm engine, the arc (groove, fill, breakdown, build, drop), the drum lanes, the acid line's answers, the pluck's deja vu, and the sound and FX choices. For each piece, how portable is it to the Deluge Generator, and what would a port cost?

This doc describes what PERF-1 does. It does not design the Deluge feature.

## Sources

- **PERF-1** by deamonSLINKY, <https://github.com/deamonSLINKY/PERF-1>, commit `9a916c61eed7491d4e94d15b6d1e711f54e62fab` (2 Nov 2026, version 0.16.x). GPL-3.0-only. I read the source and did not build or run it.
- **AMB-1** by Charles Vestal, <https://github.com/charlesvestal/fm1-amb>, commit `62a588ab102b5f1cf5e4aa72553694eaa7ddaaa5`. This is the grandparent. Its `app/dsp/amb.c` is 1784 lines, and its own files carry an MPL-2.0 header.
- **HYP-1** by RedtailWorks, the parent. Neither repo links any HYP-1 source, and a GitHub search found nothing. PERF-1's `amb.h` header still opens with "HYP-1, hypnotic techno and acid for the FM-1 (a fork of AMB-1)", and `LICENSING.md` credits HYP-1's changes inside `firmware/src/{app,dsp}`. I could not tell HYP-1's share apart from PERF-1's.

File references below are paths in the PERF-1 tree at that commit. `amb.c` means `firmware/src/dsp/amb.c`, `amb.h` means `firmware/src/dsp/amb.h`, and `ui.c` means `firmware/src/app/ui.c`. Line numbers are for that commit. To open one, use `https://github.com/deamonSLINKY/PERF-1/blob/9a916c61eed7491d4e94d15b6d1e711f54e62fab/<path>#L<n>`.

## Summary

- The whole instrument is one C compilation unit, `amb.c` (7711 lines). Generation and DSP are mixed together in it. The generator is a **sixteenth-note callback**, `tick()` at amb.c L3300-3593, that runs inside the audio render. A slower `control()` (L7540) runs once per audio chunk and moves the slow things, such as the arc, the chord timer, LFOs and envelopes.
- **A piece is a flat `int16_t par[P_NPARAMS]` array** (about 175 parameters, amb.h L72-134), a scale bitmask, six drum patterns of six 16-bit lane masks, and a 6x16 byte table of per-step tricks. A genre is a set of ten pieces plus about a dozen per-genre tables that the engine indexes by genre number.
- **Mood is a 15-row table of (parameter, delta)** in ui.c L801-829. The engine gets `stored + delta * (mood - 50) / 50`, clamped. The stored piece never changes.
- **Chords come from a weighted random walk over scale degrees.** Each genre's row in `CHT` (amb.c L1889) adds weight to favourite moves, pulls toward the tonic, forces a cadence every N chords, and sets a harmonic rhythm in bars. On top of that sit an optional repeat-the-last-2/4/8 loop, a raised leading tone on V in minor, and brute-force voice leading.
- **The arc has two layers.** A slow random tide (`arc`, -1..+1, scaled by Motion) shades filters, hat levels and reverb sends. Sections are a three-state machine (groove, break, build) with a drop event, and per-genre rules (`SECT`, amb.c L2379) say what each section keeps and how a build rolls. The auto-DJ lives in the UI layer. It only counts bars and sends section commands.
- **Drum lanes are bitmasks.** Density thins or fills a lane by a fixed step-importance order, without editing the pattern. On top come per-lane chance, bar conditions, polymeter lengths, pattern chains, slice permutations, half or double time, per-step ratchet/push/level bytes, genre ghost masks, humanize, and "drum drift" that mutates the stored patterns.
- **The acid line is a 16-step loop** of (scale offset from the chord, octave, gate, accent, slide). Mutate re-rolls steps at loop end. The "answer" is a transform (octave, fifth, third, mirror, reverse, rotate, sparse, accents) applied to the same steps on every Nth bar or loop, or on the bar before a drum change.
- **The pluck's deja vu** is a ring of up to 32 slots. Each slot stores a rung index into the current chord's arpeggio ladder plus a velocity, not a pitch. A slot replays with probability `Deja vu`, so remembered phrases follow the chord changes. A Tuna-derived motif engine develops bar 1 of each 8-bar phrase.
- **Every sound is synthesized.** That includes the drums (14 parametric kits), the "DJ snips", a 6-op DX7 engine for the pluck, and the effects. None of the DSP is useful to the Deluge, which has its own engines, including a DX7 engine in `src/deluge/dsp/dx`.
- **Almost all of the generator logic ports well.** It is integer bitmask and table work with a few `frand()` calls. The cost lies in pulling it out of the render code and its file-level globals, and in moving the data tables off PERF-1's sound names. The RNG is one global xorshift32, also used by DSP code and seeded from the clock, so PERF-1 is not reproducible from a seed.

## 1. Runtime shape

- **Threads.** "en_render() runs in the audio interrupt. Everything else posts commands, which the render drains at the start of a block" (amb.h L20-23). The UI calls `en_set(param, value)` and friends. These push 32-bit commands into a 1024-entry ring (`post`, amb.c L947-963), and `drain` (L5936) applies them through `apply(p, v)` (L3997).
- **The grid.** `tick()` runs once per sixteenth. It is driven by the internal tempo, or by MIDI clock at 6 ticks a step (`en_clock`, amb.h L295-297). Each step it decides every drum lane, the bass-line note, pad stabs, one acid step and one pluck step. It also takes chord changes on the bar line. Swing and humanize become sample delays (`wait`) that the render serves later (`drums_wait_step`, L2741).
- **Slow control.** `control(n)` (amb.c L7540-7629) runs once per audio chunk (about 5.8 ms). It advances ten slow random walkers (`mods_step`, L1168), the arc, section smoothing (`brk`), the chord timer when the genre counts in seconds, both LFOs and the drone envelope.
- **RNG.** One global xorshift32 `rnd()`/`frand()`/`irand()` (L1039-1058). The pad has its own stream, `pfrand`, because its render may run on the second core. The UI reseeds with `en_seed(plat_ms())` on piece changes (ui.c L1908, L1984, L2538). DSP code such as `lfo_step`'s S&H (L1431) and `mods_step` draws from the same stream, so the generator's random sequence depends on audio-chunk timing.
- **Float everywhere.** "Float only, no libm: fastmath.h" (amb.h L23). Generator code uses floats for probabilities and weights only. Everything it outputs is an integer.

## 2. Genres and pieces

**Parameter model.** `PARAMS[]` (amb.c L23-94) gives each parameter a name, lo, hi and default. A piece is the defaults with some values overridden. Three are the player's own and never change with a piece: Tune, MIDI out and Theme (`en_par_global`, L96). The drum-drift settings are "sticky" across pieces (`en_par_sticky`, L97).

**Recipes as sparse override lists.** A recipe is an `int16_t` array of `param, value` pairs ended by -1, for example `PC_UNDERTOW` (L114). Genre bases are macros or arrays (`LOFI_BASE` L145, `HOUSE_BASE` L162, `DUB_BASE` L180, `X_DNB` L200 and so on).

**Two ways a genre is built** (`en_piece_g`, L443-482):
- Genres 0-3 (HYPNO, LOFI, HOUSE, DUBSTEP) have ten hand-written recipes each (`PL_01..10`, `PH_01..10`, `PD_01..10`, HYPNO's `PIECES` at L378). Their drum patterns come from small procedural builders (`lofi_pats` L323, `house_pats` L342, `dub_pats` L361) or, for HYPNO, from `pats.h`, which the comment says `tools/gen_pats.py` generates.
- Genres 4-18 (DUB through FOREST, 15 of them) use one base list per genre (`XBASE`, L241), then a 7-byte row per piece from `XROW[15][10][7]` (L242): tempo, root, mode, chord kind, pad sound, drone sound and acid sound. They also share six drum patterns per genre (`XPAT[15][6][6]`, L263). `x_pats` (L308) rotates which row comes first by piece index. Pieces 7-10 take kick and perc from a different row, "so no two pieces play the same six".

**Sound variety by stride.** `vary(g, i, par)` (L425-442) picks each sound from a per-genre `PALETTE` (L398) of five pad, drone, acid and pluck sounds and three of each drum kit. The index is a different modular stride per slot, e.g. pad `(i + 2h) % 5`, drone `(2i + g + 2h) % 5`, acid `(3i + 3g + i + 2h) % 5`, kick `(i + g) % 3`. The result is that "the pieces of a genre differ in sound as well as in key and tempo, and no two neighbours share them" (L395-397). It also offsets the pluck and acid pans in opposite directions. Pluck rate comes from `PLUCK_RATE[genre]` (L424).

**Special cases.** `psy_slow` (L290) turns three PSY pieces into psy-ambient. It swaps in downtempo patterns and overrides about 12 parameters (L469-481).

**Per-genre feel.** `en_piece_mods_g` (L507-561) fills `en_mod[lane][step]`, where each byte is `ratchet | push << 3 | level << 6`. Lofi's snare and clap get push 2/12 of a step. House and garage get soft off-beat hats. DnB and breaks accent the backbeat. Trap, industrial and forest get two hat rolls per piece from a five-entry table chosen by piece index.

**Per-genre engine tables, indexed by genre number.** These hold most of the genre identity, more than the piece recipes do:

| Table | Where | What it holds |
|---|---|---|
| `CHT` | amb.c L1889 | Chord habits: tonic pull, cadence period and target, A-B-A allowed, harmonic rhythm in bars, minor dominant on/off, up to 8 weighted edges |
| `SECT` | L2379 | Section rules: what a break keeps (kick, hats, snare, bass), build length, pad rise in the break, kick back on the build's last bar, time plan, flags (roll, wobble, hat roll, gap, perc build, perc fill, kick drop) |
| `FILL_OK` | L2391 | Bitmask of the fill kinds the genre may use |
| `BL_ROOT`, `BL_RIFF` | L3079, L3081 | Bass rhythm mask, and the genre riff (mask + 4 intervals) |
| `COMP_GENRE` | L3244 | Pad stab rhythm mask |
| `GHOST_SN`, `GHOST_KK` | L3253-3254 | Steps where ghost snares and kicks may appear |
| `PLUCK_RATE`, `PALETTE` | L424, L398 | See above |

**Storage.** A piece is a packed 376-byte record, and each genre has its own flash object (README, "Under the hood"). On power-on, a piece that was never saved gets "nudged". `project_start` (`firmware/src/app/project.c` L330-366) shifts 24 levels and tone parameters by -8..+8 and the tempo by -3..+3.

## 3. Mood

Mood is a UI-layer macro. The engine knows nothing about it. `ui.c` L801-829:

```c
} MOOD[] = {{P_AC_DENS, 30}, {P_AC_CUT, 25}, {P_LANE_LVL(3), 25}, {P_LANE_LVL(4), 20}, {P_LANE_LVL(5), 25}, {P_BRIGHT, 20},
            {P_CM_DENS, 25}, {P_CM_LVL, 20}, {P_CM_DEC, -15}, {P_DR_CUT, 20},  {P_DRIVE, 20},  {P_REVERB, -15}, {P_ECHO, -10}, {P_EVOLVE, 20}, {P_PULSE, 10}};
...
v += MOOD[i].d * (ui.mood - 50) / 50;   /* then clamped to the parameter's lo..hi */
```

- Mood runs 0 (chill) to 100 (peak). At 50 nothing changes. Each listed parameter moves linearly by up to ±d, with integer maths and clamping. All the listed parameters use 0..100 ranges.
- Moving toward peak:

| Parameter | Delta at peak (negated at chill) | Meaning |
|---|---|---|
| Acid density | +30 | More acid notes on the next re-roll |
| Acid cutoff | +25 | Brighter acid |
| Closed hat, open hat, perc levels (lanes 3, 4, 5) | +25, +20, +25 | Busier top end |
| Pad Bright | +20 | Pad filter |
| Pluck density, level | +25, +20 | Except a pluck at level 0, which stays at 0 |
| Pluck decay | -15 | Shorter, tighter plucks |
| Drone/bass cutoff | +20 | |
| Drive | +20 | In the code this is drum saturation, `drive_k` used only in `render_drums` (amb.c L4076, L7047). The manual says "the bass's cutoff and drive", which does not match the code. |
| Reverb, echo sends | -15, -10 | Drier at peak |
| Evolve | +20 | See below |
| Drums (Pulse) level | +10 | |

- `mood_apply()` sends every row whenever Mood moves. A knob a player turns while Mood is off 50 also goes through `mood_val`, so the offset stays applied (ui.c L1527). Mood is not stored, and a new piece gets the same offset (MANUAL.md "Mood").
- The live parameter (`en_par_now`) and the stored one (`proj.par`) are separate on purpose. Screens and saves show the stored value.
- Evolve barely matters in PERF-1. It adds `0.05 * evolve` to the acid mutate chance (amb.c L2269). It raises the chance that a drop changes chord (L2555). It scales breakbeat "chop" (L3306) and adds random off-beat pad stabs (L3554). In AMB-1 it was a main control.

**Portability.** Very cheap (S). The mechanism is a static table and one line of maths, and it fits a "Recipe plus live offsets" model. The work is choosing the Deluge targets. Several PERF-1 targets are generator parameters (densities, Evolve), and the rest are mix and tone parameters that would map to Deluge sound params or automation.

## 4. Harmony

### Scales and chords (pure functions)

- **Scale** is a 12-bit pitch-class mask relative to the root (`EN_MODE_POOL`, amb.c L17-19). There are eight modes: the seven church modes minus Locrian, plus major and minor pentatonic padded to 7 entries with repeats. `en_scale` (L830) expands the mask to ascending steps. The player can toggle keys to edit the pool. Deluge's `NoteSet` is the same idea.
- **Chord kinds** (`en_chord_kind`, L842-872). Auto, triad, 7th, 9th (1-3-7-9, no fifth), sus (1-4-5-b7) and all stack scale steps by a per-kind offset order. Dub (m9 shape: 0, 3, 10, 14) and Fourths (0, 5, 10, 15) are fixed semitone shapes on the degree, whatever the scale. Auto gives sevenths on 6-7 note scales and triads otherwise.
- **Dominant in minor** (`dom_apply`, L1924-1931). In a genre with `CHT.dom` set, when the chord is V and the scale's seventh is a minor third above the fifth, the scale's 7th degree goes up a semitone for as long as V lasts. That changes the chord's third, the acid's notes and the bass's notes. `en_prog_chord` (L1932) mirrors this for the UI.
- **Voice leading** (`en_voice_lead`, L887-913). Brute force over all 24 permutations of 4 voices onto the chord tones, with the root doubled for triads. Each voice takes the nearest octave of its tone within a register (C3..F5, moved by octave params). The cost is total semitone movement plus 40 for each pair of voices on the same note. Optional "drop 2" voicing lowers the second-highest voice an octave (`pad_retarget`, L1811-1848).
- `en_chord_name` (L915) names chords from an interval bitmask (m7b5, 7sus4, add9 and so on), for the screen.

### The chord walk (`chord_change`, amb.c L2008-2085)

At each change, every scale degree except the current one gets a weight:

- **HYPNO, or any non-7-note scale**: the fixed AMB-1 weights `W7 = {4, 2, 1, 3, 3, 3, 0.4}` for I..vii. On pentatonics the tonic gets 3 and every other degree 1.5.
- **Genres with a 7-note scale**: weight 1, plus `CHT.tonic` if the degree is home, plus the weight of any `CHT` edge from the current degree to it. Then:
  - When `back == 0`, a move straight back to the previous chord (A-B-A) gets weight 0.
  - A diminished degree (fifth a tritone) is multiplied by 0.15.
  - On the chord before the phrase end (`cad_n + 1 >= cad_every`), the cadence chord `cad_to` gets +10. If that chord is diminished in this mode, V or IV replaces it.
  - At the phrase end (`cad_n >= cad_every`), home gets +30.
- Roulette selection on the weights.
- **Loop** (`P_CH_LOOP`, L2060-2080). With value 1-3, the first 2, 4 or 8 chords chosen become a ring (`dh[]`) that then repeats. Values 4-6 do the same, but each chord has a 25 % chance of being redrawn. The ring always starts from the current chord, so a 2-loop is home plus one other chord.
- **Hand progression** (`prog_take`, L1975; amb.h L321-338). Eight slots, each holding a degree (or empty) and a length of 1, 2, 4 or 8 bars, packed into 6 bytes. When it is on, it replaces the walk. `en_prog_capture` (L1950) turns the chords just heard into slots.
- **Drop**. The drop sends the next chord home with 70 % chance when not already home (`section_cmd`, L2552). Otherwise it changes chord with chance `0.4 + 0.5 * evolve`.
- `chord_to` (L2087-2105) updates the history, applies the dominant, retargets the pad, starts the pad's handoff, moves the drone or bass root, and fires the drone envelope if its trigger is "Chord".

Example rows from `CHT` (fields: tonic, cad_every, cad_to, back, hr, dom, edges):

```c
{2, 8, 4, 0, 2, 1, {{1, 4, 12}, {4, 0, 12}, {0, 1, 4}, {0, 3, 4}, {3, 1, 5}, {5, 1, 6}, {2, 5, 6}, {6, 2, 5}}},   /* lofi: ii-V-I, turnarounds */
{2, 4, 6, 1, 2, 0, {{0, 6, 8}, {6, 5, 6}, {5, 6, 6}, {6, 0, 5}, {5, 0, 3}, {0, 5, 5}, {3, 6, 3}, {0, 0, 0}}},    /* house: i-VII-VI-VII loops */
```

### Harmonic rhythm

- `CHT.hr` is bars per chord: 2 for lofi, house, garage, disco, reggae, synthwave, industrial; 4 for dub, DnB, breaks, electro, trap, funk, amapiano, trip-hop; 8 for dubstep, psy, forest. 0 means HYPNO, which counts in seconds.
- The Drift knob scales it (`hr_bars_v`, L1912-1921). Below 20 it is quartered, below 45 halved, above 80 doubled, above 95 quadrupled.
- Counting happens on the bar line in `tick()` (L3583-3591). When `hr_left` reaches 0, `change_pending` is set and the next bar line changes the chord. HYPNO, or a player choice of "seconds", uses `drift_secs` instead: 20 s to 5 min, exponential in the knob, ±30 % random (L635, L1876). `control()` counts that down and the change still waits for a bar.
- With "Every: Chord", drum drift starts on a chord change: a fill on the next bar, then a pattern change (L3576-3582).

### Pad behaviour

Four voices "breathe" with independent attack, hold, release and rest cycles. Their lengths have unequal ratios `{1, 1.21, 1.43, 1.67}` and ±15 % jitter (`pad_breathe`, L1508; `BREATH`, L1404). Each voice picks up its new target note only at its next breath. After a chord change, `pad_leave` (L1548) fades out voices on notes the new chord lacks within about 1 s, and lets voices on shared notes finish their breath. All of this comes from AMB-1. Pad sounds flagged as "stabs" play chords on a comping mask instead (`COMP_MASK`, `COMP_GENRE`, L3243-3251; fired in `tick` L3552-3567).

### Portability

The scale, chord, voice-leading and naming functions are pure, self-contained and int-only, and could port almost verbatim (S). The chord walk with its `CHT` table, loop ring, hand progression and harmonic-rhythm counter is about 300 lines of logic plus tables. It reads file statics (`deg`, `deg_prev`, `cad_n`, `dh[]`, `pg_*`, `hist[]`) that would become one object's state, and it needs an injected RNG (S-M). The pad's breathing is a sound-design idea, not a generator rule. On the Deluge it would have to become note events with long overlapping lengths and an envelope on the synth, since PERF-1 does it per sample in the render (M, and a design question).

## 5. The arc: tide, sections, fills, auto-DJ

### The slow tide (`arc`, Motion)

In `control()` (L7555-7567), a target in -1..+1 is drawn every 50-150 s, pushed away from 0 by 0.25. `arc` eases toward it with a 35 s time constant, and `arcm = arc * Motion`. What reads it:
- pad filter cutoff (L1614, L6088, L6212)
- hat velocity, ×(1 + 0.4·arcm) clamped to 0.6..1.1 (L3304)
- drone filter and FM index (L6353, L6380)
- reverb send, ×(1 - 0.25·arcm) (L7163)

AMB-1 has the same tide, and there it also thins the bells (AMB-1 `amb.h` header).

### Sections (engine)

There are three states, `SEC_GROOVE`, `SEC_BREAK` and `SEC_BUILD`, and the drop is an event that returns to groove (amb.h L191-199; amb.c L2349-2559). Commands take effect on the next bar.
- **Break.** Each lane is silenced unless `SECT` keeps it (kick, hats, snare). The bass line stops unless `SECT.bass` (L3157). Pluck density ×0.4, and replayed deja vu notes sound only 40 % of the time (L2993-2998). In the bar where a break is requested, perc, open hats and odd closed hats drop out first "so it comes as a release" (L3427-3429). `brk` eases to +1 over about 2 s. That makes the pad louder by `SECT.pad`% (L6170), closes the drone filter (L6381), raises reverb send ×1.7 (L7163), and darkens the master LP (L5363).
- **Build** (`SECT.build` bars: 2, 4 or 8). Kick, clap and perc are muted. The snare rolls in quarters, then eighths (second-last bar), then sixteenths up to step 13 with a one-beat breath. Velocity ramps 0.3→1.0 and snare pitch climbs a fifth (L3409-3426, L3392). Genres flagged `SF_PBUILD` roll on the perc lane instead. `SF_ROLL` adds 32nds and then triplets in the last bar. `kret` brings the kick back on beats 1 and 3 of the last bar. `SF_GAP` mutes the whole mix for the last beat (L3393-3394). The open hat plays eighths in the last bar. When the bars run out, the engine drops by itself (L3345-3346).
- **Drop** (`section_cmd`, L2544-2558). Kick, clap and open hat hit on the one (`drop_imp`), and the open hat rings as a crash. The riser cuts. The bass line forces a note on step 0. The chord-home logic in §4 runs. `grv_bars` restarts, which drives the per-genre "time plan" (dubstep's double-time bars after the drop, `pat_bit` L3266-3283), the dubstep LFO wobble for 8 bars (L3354-3357), and techno's kick drop on the last beat of every 8th bar (`SF_KDROP`, L3405-3406).

### Fills

The fill kind for each bar is drawn from the kinds the genre allows (`FILL_OK`), rotating by bar number (L3332-3338). Kinds:
- 0: ramp (snare and hats every step)
- 1-5: `FILLS[5][16]` snare velocity tables for the bar's second half (rush, drag, roll-and-breath, off-beats, hit-gap-sixteenths, L2403-2408)
- 6: triplet roll
- 7: tom fall on the perc with pitch falling 0.9 semitones a step (L3395)

Three sources trigger a fill. The FX key (`fill_hold`), the DJ asking a bar ahead (`fill_q`), or drum drift's bar before a change (`dd_fill`, second half only).

### Auto-DJ (UI layer, ui.c L1653-1756)

The auto-DJ is a bar counter in the UI that sends the same section commands a player would.
- It runs groove for G bars (8, 16, 32 or 64), with a fill requested on the last groove bar (`en_fill_next`), then a break, then a build. The build drops by itself.
- `dj_break_len` (ui.c L727-733) sizes the break (4 bars, half the groove, or the full groove) so that break plus build lands on a multiple of 8 bars. A comment explains why: "groove 16 + break 8 + build 4 was 28 bars, and a DJ mixing in lost the grid".
- In "SYNC" mode the engine itself fills the bar before a chord change and breaks on the change (`dj_sync`, amb.c L3339-3344).
- After N rounds it queues the next piece on the fill bar, so "the breakdown and the drop belong to it". The tempo glides from the old piece's BPM over 4-32 bars (`mix_start`/`mix_tick`). An optional tempo ramp across rounds is available.
- It adds `FX_FILL` during its fill bar and `FX_RISER` during builds to the effect mask (ui.c L3385-3390).
- An optional key change on the drop (+2/+3/+5/+7 semitones) lasts until the next break (`key_tick`, ui.c L1825-1828).

### Portability

The section machine and the auto-DJ are small integer state machines on bar counts, so the logic is cheap (S-M). The cost is extraction. The section rules are inline `if` chains inside the per-lane loop of `tick()` and are tangled with fills, FX bits and ghosts. On the Deluge the audio side of a section (pad swell, filter close, reverb throw, gap, riser) would have to become parameter automation or live param changes on Deluge sounds. That is a mapping job, not a code port.

## 6. Drum lanes

- **Data.** `en_pats[6][6]` is six patterns of six `uint16_t` lane masks (kick, snare, clap, closed hat, open hat, perc), plus `en_mod[6][16]` step-trick bytes (amb.h L376-377). The UI edits patterns in place. A pattern change waits for the bar line.
- **Per-step pipeline** (`tick()`, L3396-3550, in order):
  1. **Lane length (polymeter).** `P_LLEN + l` from 1 to 16. A shorter lane steps by `sixteenth % len` against the bar.
  2. **Source.** `P_LSRC + l` lets a lane read from a different pattern than the piece's current one.
  3. **Density** (`en_lane_mask`, L2429-2452). Steps have a fixed importance order, `IMP = {0, 8, 4, 12, 2, 10, 6, 14, 1, 3, 5, 7, 9, 11, 13, 15}` (downbeat, half, quarters, eighths, then sixteenths). Below 50, the lane drops `round(pop * (50 - dens) / 50)` hits, least important first. Above 50, it adds `round((16 - pop) * (dens - 50) / 100)` hits on the most important empty steps, so at most half the empty steps fill. The stored pattern is never edited.
  4. **Time and slice** (`pat_bit`, L3266-3283). Half time reads the mask over two bars. Double time reads it twice per bar, OR-ing step pairs. Slice reads the bar through one of 8 permutations (`SLICE`, L3261), e.g. halves swapped, a stuttered beat, or backwards. Option 9 picks a random one every fourth bar.
  5. **Condition** (`cond_ok`, L3285-3298). Always, odd bars, even bars, 1st of 4, 4th of 4, 1st of 8, fill bars only, or not in fills.
  6. Section and fill overrides (§5).
  7. **Breakcore chop** for kits with `chop > 0`. Random ghost snares, off-beat kicks, hat flips and 32nd rolls, scaled by Evolve (L3457-3468).
  8. **Ghosts.** On genre `GHOST_SN` and `GHOST_KK` steps, an empty step sounds with probability `0.6 * Ghosts%` at velocity 0.22-0.42 (L3469-3473).
  9. **Chance.** `P_PROB + l` drops hits with probability 100 - p (L3474-3475).
  10. FX overrides: hat roll, hat triplets, snare roll, flam, double kick (L3487-3500).
  11. **Step tricks** (only in groove, not in fills or on drop hits, L3501-3507). Level scales velocity by ×{1, 0.4, 0.7, 1.12} (normal, ghost, soft, accent), weighted by the Accents knob. Push delays by n/12 of a step. A ratchet (`RATCHET[8]`, L2645) gives x2, x3, x4 or x6 at step/div spacing with velocity ×0.8 per repeat, a flam (grace note 20 ms ahead), or swells (repeats getting louder, ×1.35-1.4).
  12. **Humanize.** Velocity ×(1 - 0.2·h·r), plus up to 0.12 step late (L3508-3512).
  13. **Swing** delays odd sixteenths by `min(swing, 0.5) * step_len` (L3305).
- **Chain** (`CHAIN[9]`, L3260). A per-bar pattern sequence such as "1 1 3 4" (the Amen) overrides the pattern choice at each bar line (L3377-3381).
- **Drum drift** (`dd_bar`, L2490-2523; `dd_mutate`, L2454-2488). Every 4-64 bars, or on a chord change, there is a fill bar and then a pattern change to the next one or a random one within `Pool`. Mutate "Shift" moves one or two hits by ±1 step on snare, hats or perc. It never touches the kick or clap, and never moves a snare on a beat (`keep = 0x1111`). "Morph" also adds or removes one hit, capped at 10 per lane. Mutations write into `en_pats` and so get saved. The acid answer can sync to the bar before a drift change.
- **Links** (`P_LINKS`). Kick ducks the drone, snare and clap pulse the pad, hats accent the acid, and perc can clock the acid line (amb.h L92-94; L3568-3573).

**Portability.** This is the most portable part. It is all `uint16_t` masks, small tables and per-step `frand` (S). The data are generic too: XPAT, the genre ghost masks and the fill tables are plain step grids. For Compose, several features match Deluge note fields (`src/deluge/model/note/note.h`): chance maps to `probability`, bar conditions to `iterance`, "fill only" to `fill`, and the level byte to `velocity`. Polymeter maps to per-row lengths, and a pattern chain to a clip or arrangement sequence. Ratchets, flams, push and swing have no matching Note field, so they would be written as extra short notes and fine-tick offsets.

## 7. The acid line

- **State.** `ac_off[16]` holds scale steps above the current chord degree. `ac_oct[16]` holds octaves. `ac_gate`, `ac_accent` and `ac_slide` are 16-bit masks. Length is 1-16 (`P_AC_LEN`), so odd lengths run against the bar.
- **Pitch** (`acid_raw`, L1739-1750). `note = 36 + root + steps[(deg + off) mod n] + 12 * (octave carry + oct + acid oct + song oct)`, folded into MIDI 28..96. Because each step is relative to the current chord degree, the same line moves with the harmony.
- **Generation** (`ac_roll_w`, L2183-2207). Each step independently:
  - gate with probability Density (step 0 gets +0.3)
  - offset is a chord tone (`2 * irand(chord_nt)`, i.e. every other scale step) with 80 % probability, otherwise any scale step
  - octave is 0 with 55 % probability, otherwise `irand(Range)`
  - accent 30 %
  - slide `Slide% * 0.7`
- **Mutate** (`ac_mutate`, L2262-2276). At each loop end, each step re-rolls with probability `(Mutate/100)^2 + 0.05·evolve`. `P_AC_CHG` limits a re-roll to notes only, rhythm only, or accents and slides only. Freeze (`P_AC_LOCK`) stops mutation. A frozen line is stored with the piece as 18 bytes (`en_acid_pack`).
- **Answer** (L1729-1801; scheduled in `tick` L3359-3376 and `acid_step` L2313-2321). `P_AC_RESP = type * 5 + dir`, where dir is -2..+2:

| Type | Effect |
|---|---|
| 1 Octave | ±12 or ±24 |
| 2 Fifth | ±7 or ±14 |
| 3 Third | the degree shifted 2·dir scale steps, so it stays diatonic |
| 4 Mirror | `2 * centre - n`, where centre is the midpoint of the line's own range, then shifted ±12·dir |
| 5 Reverse | reads step `len - 1 - st` |
| 6 Rotate | reads step `st - 4`, one beat late |
| 7 Sparse | odd steps silenced |
| 8 Accents | only accented steps sound |

  A shift that would leave 28..96 goes the other way. "Every" picks every 2nd to 8th bar or loop (`Sync: Bar` or `Loop`). Its last value means "the bar before the drums change". With drift not changing patterns, that falls back to every 8th bar. The call itself is never rewritten. The answer is a read-time transform.
- **Playback** (`acid_step`/`acid_fire`, L2286-2395). Slide happens only when the previous step was gated and had slide set. Pitch then glides and the envelope does not retrigger. The gate is 55 % of a step, or held into the next step on slide. Accent shortens decay ×0.6 and raises velocity.

**Portability.** S. It is pure index maths on a 16-step loop. The generator logic carries over unchanged. The only Deluge question is slide. A Deluge `Note` has no slide flag, so a slide would have to become overlapping notes with portamento or pitch automation. That is a mapping question for later tickets, not a reason against the port.

## 8. The pluck: arpeggio, deja vu, motif

- **Ladder** (`cm_ladder`, L2884-2898). The current chord's tones over Range octaves (1-3) from C4, sorted, up to 12 rungs. Notes are stored as rung indices, never as pitches.
- **Grid.** Every 4th, 3rd (dotted eighth: steps 0, 3, 6, 9, 12), 2nd or 1st sixteenth, by Rate.
- **New note** (`cm_step`, L2941-3096). It sounds with probability Density (×0.4 in a break; +0.3 on the first note). The rung comes from Style: up, down, up-down without repeated ends, random, skip 2 or skip 3. Then:
  - With Deja vu > 0, a "morph" happens 50 % of the time. Instead of the style's rung, the note is a neighbour (±1 rung) of whatever this slot played last time.
  - A leap of an octave (70 % up) happens with probability Leaps.
  - Velocity is 0.7-1.0.
- **Deja vu** ("after Marbles", L2979-2992). The loop is a ring of `4 << P_CM_LOOP` slots (4, 8, 16 or 32), and `cm_k` advances once per pluck grid point. Each slot keeps `cm_f` (filled), `cm_g` (gate), `cm_ix` (rung + leap bits + "set" bit) and `cm_vl` (velocity). On each grid point, with probability Deja vu %, the slot replays its stored gate, rung, leap and velocity against the current chord's ladder. Otherwise it generates a new note and overwrites the slot. At 100 the phrase locks and follows the chords. At 0 every note is new.
- **Motif** ("after Tuna's motif engine, GPL-3.0", L2843-2853, L2954-2977). Bar 0 of each 8-bar phrase records an idea: a rhythm mask plus rung intervals between notes. With 30 % probability it reuses the kept idea instead. Bars 1-7 develop it with probability Motif %. They replay the rhythm and intervals from the current rung (a sequence), inverted 20 % of the time, with the last interval redrawn 30 % of the time. Bar 4 always restates the idea at its original rung. Deja vu replay takes priority over the motif.
- **Freeze** (`P_CM_LOCK`). The first 16 slots become a step-editable line stored with the piece (`en_pluck_pack`, 12 bytes).
- Credits say the bass-line modes, comping rhythms, ghost notes and deja vu loop "follow Tuna by delirioux (GPL-3.0) ... written afresh for this engine" (README credits; `LICENSING.md`).

**Portability.** S-M. It is a few hundred lines of self-contained state: a 32-slot ring and a 16-entry motif. Storing rungs instead of pitches is the idea worth keeping, because remembered material then survives chord changes for free. The Deluge has its own arpeggiator (`src/deluge/modulation/arpeggiator.h`), but deja vu and the motif engine are generator logic that would sit above any arpeggiator.

## 9. The bass line

- `P_BL_MODE` (`bl_step`, L3152-3200):
  - 0 drone (the root, held under the chord)
  - 1 root on the genre's rhythm mask `BL_ROOT[g]`
  - 2 on every kick
  - 3 eighths
  - 4 off-beats
  - 5 walk: root, then third or fifth, then fifth, then an approach note from below with chance Move + 20
  - 6 the genre riff `BL_RIFF[g]`
  - 7-15 named figures from `BL_FIG`: Octaves, Two-step, One-drop, Log drum, Skippy, Funk, Big beat, Dub, Gallop
- A figure is a 16-step mask plus four intervals, cycled per hit (L3081-3097). With probability Move, a hit becomes the octave (50 %), the chord's fifth (30 %), or the scale note just below the root.
- The root comes from chord tone 0, placed at C2 plus octave params and folded into C1..B5. Length sets the gate at 0.25-5 steps. Glide is portamento. Each note re-triggers the drone filter envelope.

**Portability.** S. It is table data plus about 50 lines.

## 10. Sounds and FX

Everything is synthesized in `amb.c` and the `fm6*` files. None of it is sample playback.
- **Pad**: 24 sounds over 10 oscillator models (Rhodes FM, formant "Voices", strings, glass, organ, supersaw, PWM and others), a resonant 4-pole filter on saw sounds, a sub, and an ensemble chorus (`PAD_SOUND`, L1262; `pad_chorus`, L6118).
- **Drone/bass**: 24 sounds (saws, reese, hoover, FM, fifths, sub and others) through a 24 dB low-pass with its own envelope and LFO. A dubstep "wobble" steps the LFO rate.
- **Acid**: 31 oscillators (saw, square, sync, CZ, mini, supersaw, FM bells and others) into a resonant LP with envelope, accent and slide. The SCREAM page adds drive and resonance.
- **Pluck**: 12 two-operator FM sounds plus a six-operator DX7 engine (Melodee, after Dexed/MSFA). It has 30 built-in public-domain DX7 voices and up to 16 user `.syx` voices.
- **Drums**: 14 parametric kits (`kit_t KITS[]`, L1199). Each sets about 24 numbers for kick pitch drop, click, rumble, snare tone and noise, clap spread, hat and perc tuning, and a "chop" amount. The four drum groups can each take a different kit.
- **Noise/vinyl** bed in 8 wear colours, ducked by the kick.
- **Space**: tempo-synced tape echo, an Echoplex, and Dattorro's plate with Plate, Shimmer and Gated variants (L3595+). The plate can run on the second core.
- **Master bus**: stereo-linked compressor and look-ahead brick-wall limiter, with per-genre settings (`master_dyn_block`, L5560).
- **Performance FX**: a 64-bit hold mask (`en_fx`, amb.h L201-232). Beat repeats including triplets, reverse, tape stop, LP/HP sweeps, three trance gates, echo throw, fill, phaser, flanger, tape, roll, pump, riser, vinyl, scatter, brake, spin, half time, sweep, scratch, granular freeze, reverse swell, ring mod, talk, wah, flex beats, punch-in filter/delay/phaser per track, and drum rolls. Each has a settings page (`EN_FXS_*`). There are also 27 synthesized "DJ snips" (`DJS[27]`, L4815) and four FX knobs (filter, crush, throw, wash).
- **Load shedding**. `en_shed` (amb.h L154) drops DX polyphony, chorus, Echoplex and vinyl as CPU climbs.

**Portability.** The DSP is not worth porting. The Deluge has subtractive, FM, ring-mod and DX7 synth engines, sample kits from the card, reverb, delay, compressor, mod FX and granular (`src/deluge/dsp/`). What does carry over is the musical intent per genre. The `PALETTE` and `SECT` choices tell you which kind of sound and which FX moves each genre wants. Those would need mapping to Deluge presets, card samples and params. PERF-1's public-domain DX7 voices (`fm6_dy.h`, 128-byte packed records with sources listed in `tools/fm6_dy.txt`) are standard DX7 bulk format. They could be loaded as patches by the Deluge's DX7 engine, subject to the licence check below. The FX list is a useful catalogue of "performance moves" for Perform, but each item maps to existing Deluge features or does not port.

## 11. Ancestry

- **From AMB-1.** The parts PERF-1 shares with AMB-1 are close to verbatim: `en_euclid`, `en_scale`, `en_voice_lead`, the `W7` degree walk, the breathing pad with `BREATH` ratios, `mods_step`, the arc, the command-queue threading model and Dattorro's plate (AMB-1 `app/dsp/amb.c` L138, L220, L362, L399, L621-627). AMB-1 had bells on four tape loops of 13/17/23/29 quarter notes, and a three-lane Euclidean pulse of 16/12/7 steps. By PERF-1 the bells are gone and the six-lane sequencer has replaced the pulse. `en_euclid` survives in the UI, for the step editor's Euclidean fill and the gate presets (ui.c L2210, L855).
- **HYP-1** (inferred from the `amb.h` header and comments, since the source is not public) added the techno layer: the acid line with Mutate, the six-lane 16-step sequencer with six patterns, sections and HYPNO's ten pieces. PERF-1 then added the 18 other genres, the `CHT`/`SECT` tables, the answer, deja vu, the motif, bass lines, comping, ghosts, density, conditions, chains and slices, the auto-DJ and Mood. This split rests on version-tagged comments such as "0.15.13", "0.16.2" and "(audit M-13)" next to those features.
- **Licences.** PERF-1's `LICENSE` is GPL-3.0-only. AMB-1's files carry an MPL-2.0 header and AMB-1's `LICENSE` is MPL-2.0, which allows combination with GPL code. The Deluge community firmware is GPL-3.0 (`LICENSE` in this repo). GPL-3.0-only code can go into a GPL-3.0 project. I have not checked whether the Deluge firmware's licence is "only" or "or later". I also have not checked file-level headers for code PERF-1 credits to others (Melodee, Dexed/MSFA under Apache-2.0).

## 12. Portability summary

Costs are rough relative sizes for porting the logic into C++ behind an output seam. They do not include UI or Deluge integration. S is a day or two, M is about a week, L is more.

| Piece | PERF-1 location | Port cost | Notes |
|---|---|---|---|
| Scale, chord kinds, voice leading, chord naming, Euclid | amb.c L786-944 | S | Pure int functions. Port verbatim. |
| Chord walk, genre `CHT`, harmonic rhythm, minor dominant, loop ring, hand progression | L1868-2105 | S-M | File statics become object state. Inject the RNG. |
| Piece and genre data model (sparse overrides, base plus rows, stride palettes) | L23-561 | M | The structure is trivial. The data need remapping off PERF-1's sound and kit indices and its param enum. |
| Mood macro | ui.c L801-829 | S | One table, linear and clamped. Choosing Deluge targets is the real work. |
| Slow arc (Motion) | L7555-7567 | S | A random-target smoother. Its consumers are sound params. |
| Sections, fills, auto-DJ | L2349-2559, L3300-3550, ui.c L1653-1756 | M | Logic is small but tangled into `tick()`. Audio-side moves become automation. |
| Drum lane pipeline (density, chance, conditions, polymeter, chain, slice, time, ghosts, drift) | L2410-2525, L3243-3550 | S-M | Bitmasks. Several features match Deluge Note fields. |
| Step tricks (ratchet, push, level) | L2645-2675, L3501-3507 | S | Must become extra notes and offsets in Compose. |
| Acid generation, mutate, answer | L1729-1809, L2183-2395 | S | Pure. Slide needs a Deluge representation. |
| Pluck styles, deja vu, motif | L2841-3096 | S-M | Rung-indexed memory is the key idea. |
| Bass line modes and figures | L3069-3200 | S | Tables plus about 50 lines. |
| Pad breathing and comping | L1403-1565, L3243-3251 | M | Comping is a mask (S). Breathing is DSP-level and needs redesign as notes and envelopes. |
| Synth voices, drum synthesis, FX, master bus | most of amb.c, fm6*.inc | Not portable | Use Deluge engines. Carry over intent only. |
| DX7 voice data | `fm6_dy.h` | S | Standard packed DX7 format. Check provenance before shipping. |

Cross-cutting costs:
- **Extraction.** The generator is spread across `tick()`, `cm_step`, `acid_step`, `bl_step` and `chord_change`. These write straight into voice structs (`bass.target`, `pad[i].target`, `acid.inc_t`) and call `midi()`. A port has to replace those writes with note events on an output. PERF-1's MIDI-out path (`midi()`, `en_midi_map`) already emits most generator decisions as note on/off messages, so it shows where those cut points are.
- **Timing.** PERF-1 runs the generator inside the audio render, one sixteenth at a time, and expresses swing, push and humanize as sample delays. The Deluge's sequencer clock and tick resolution are different. Swing and push would have to become tick offsets.
- **Reproducibility.** One shared RNG, consumed by DSP as well, and seeded from the millisecond clock. A Deluge port that wants "same Recipe plus seed gives the same song" needs its own generator RNG, and probably one stream per part.
- **Lookahead.** PERF-1 decides everything at the moment a step plays. Mutate, drift and deja vu change state as they go, so there is no score to render in advance. Compose would have to run the same stepper offline, bar by bar, to produce a song.

## Open questions

- HYP-1's source is not public, so the AMB-1 → HYP-1 → PERF-1 split above is inferred from comments.
- Whether the Deluge firmware is GPL-3.0-only or GPL-3.0-or-later. Also whether the Melodee, Dexed and MSFA licences constrain any reuse beyond the generator logic.
- PERF-1's manual says Mood moves "the bass's cutoff and drive", but `P_DRIVE` is drum saturation in the code. It is a small discrepancy, but anyone copying the Mood table should follow the code.
