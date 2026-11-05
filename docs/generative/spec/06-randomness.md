# 6. Randomness and seeding

Part of the [Generative mode spec](../spec.md). Section numbers are shared across all files.

### 6.1 Own randomness

The core uses only its own integer hash. It never calls the firmware's `getRandom255`, `getNoise` or `CONG`, and never reseeds `jcong`. That generator is one global LCG (`util/waves.h`) seeded from a timer, and probability, the arp and about a dozen DSP paths all draw from it, so its sequence depends on what is sounding.

### 6.2 Keyed draws

- Every draw is a pure hash of `(seed, part, take, purpose, position)`. Draws don't affect each other. A macro change moves the threshold over the *same* underlying choices, so the piece stays recognisably itself. Computing a muted part, or skipping work, never shifts any other draw.
- Things that walk, such as drift and the chord walk, keep small state, but each step's draw is keyed.
- The hash is a cheap 32-bit integer mix, chosen at implementation.
- **Position means Arc counters**: the Section index (such as the 3rd Groove), the phrase within the Section, the bar in the phrase and the step. Each purpose keys on the counters it chooses, so repetition is designed in. A Loop keyed on the phrase repeats bars within a phrase and mutates between phrases. Forcing a Section changes *when* things happen, not *what* they are.

### 6.3 The contract

The same Recipe, seed and takes, with the same steering at the same positions, give an identical **event stream**: notes, velocities, sound-control values and Arc sections. This holds in the firmware and the harness, whatever the CPU load, voice culling, muting or wall-clock time. Audio is not promised bit-identical, because firmware DSP randomness and culling are outside the Generator.

### 6.4 What the seed covers

- The seed covers only the music the Generator makes: patterns, notes, chords, Arc section lengths, fills, Ornaments, Velocity spread and drift.
- Musical facts and sound picks are rolled from fresh entropy and stored as concrete values. A Recipe and seed reproduce exactly only on a card with the same files and on the same genre version.
- **No firmware chance.** Perform and Compose write notes at probability 100 with no iterance and no fill flags, and use no arp random modes and no firmware randomizer.

### 6.5 Seed source and display

- A fresh 32-bit seed is drawn from a hardware timer on Recipe creation, Regenerate and any genre change.
- The player sees it read-only as a **seed code** of 7 base-32 characters (for example `1A2B3C4`), used in names and on the SEED + TAKES title. The alphabet is chosen at implementation, from characters `QwertyUI` accepts (A–Z, 0–9).
- The player can draw a new seed (NEW SEED) but can't type one in v1.

### 6.6 Takes

- Each part has a **take**, starting at 0, that is part of every key for that part's notes. Re-rolling a part's pattern gives it the next take: new patterns for that part only, with the rest of the piece unchanged.
- Earlier takes replay exactly, so stepping back is free. A new take or a step back starts at the next phrase line.
- A take covers the part's notes and Ornaments, not its drift. Re-rolling the pattern and re-rolling the sound are separate actions.
- v1 has 16 takes per part, 0–15, shown as 1–16 (§23.5).
- A new seed, Regenerate and a genre change reset every take to 0.

### 6.7 Freeze

- Freezing a part holds the counters its pattern keys on, so it replays its current phrase's draws.
- Macros and section rules still act on top: a frozen hat line still thins with Density and still drops out in a Break.
- Drift keeps wandering, because freeze holds notes, not sound.
- Unfreeze jumps to where the counters would be now, not back to where it froze, which keeps the timeline reproducible.

### 6.8 Perform and Compose agree

A Compose of a Recipe, seed and takes plays the same notes at the same Arc positions as an untouched Perform of it from Start (with the Intro). Compose may differ only where its song shape forces it: length and Outro placement. A harness spec checks this ([§21.2](21-testing.md#212-harness)).

Sources: [Generator randomness and seeding][t17], [Compose output shape][t16], [Sub-step expression][t20], [POC gaps][t52], [Perform control layout][t38], [Compose screen and Generative menu prototype][t50].

[t16]: https://github.com/gismya/deluge-generative/issues/16
[t17]: https://github.com/gismya/deluge-generative/issues/17
[t20]: https://github.com/gismya/deluge-generative/issues/20
[t38]: https://github.com/gismya/deluge-generative/issues/38
[t50]: https://github.com/gismya/deluge-generative/issues/50
[t52]: https://github.com/gismya/deluge-generative/issues/52
