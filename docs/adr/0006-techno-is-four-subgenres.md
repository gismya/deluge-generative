# Techno is four subgenres, not one genre with alternative parts

Techno was briefed as drums, a mutating acid line and one chord throughout. Once M1's skeleton played, that was too thin: the player wanted pads, chords and melodies to bring in, and basslines other than acid. Six parts can't hold every bassline and every melodic part at once, so something has to choose which ones play. We made the choice at the genre level. Techno is a **parent genre**, a table that is never played, and Acid, Melodic, Detroit and Dub techno are **subgenres** written as overrides of it. Each subgenre has its own roster, Harmony slot, tempo range, scales and palette. A part offers no alternatives inside a genre: another bassline is another subgenre. The flavours the subgenres play (Sustain, Stab, Arp, Motif and the Chord-graph walk) move from M2 into M1, ahead of the Techno goldens, so the goldens are cut once. Decision: the user, in the session that planned melodic Techno.

## Considered options

- **Alternatives per part slot.** Each Techno part would list alternatives (bass: acid, rolling, offbeat, sub; lead: arp, motif), rolled at Regenerate and re-rollable from the CLIP page like a sound. It gives more combinations, but some of them clash (an acid bass under a rolling arp, a busy stab over a busy lead). It also needs a new Recipe field, a new steering action with its own log entry and preload, and a part whose rules change mid-piece. The genre model says variety comes from facts, sound picks and the seed, never from pieces to pick from, and this would have bent that rule.
- **A subgenre rolled inside one Techno genre.** Regenerate would roll the subgenre as a musical fact. The player couldn't pick one, and a change mid-piece would still need a new mechanism, where a genre change already has its transitions.
- **Keep Techno sparse, and wait for M2.** The melodic flavours would land with Ambient and House, and Techno would stay acid. It would keep M1 small, but Beta 1 would ship without the sound the player wants from it.

## Consequences

- Subgenres reuse everything a genre has: the genre change and its transitions, Regenerate, palette override files by genre name, Recipe files, goldens and versions. The new pieces are a parent field, a short name and a group in the GENRE menu.
- Moving between subgenres is a genre change, so it plays a Build into a Drop, a Fade through or a hard cut. Changing only the bassline in place isn't possible. It was a feature of the alternatives option and is lost.
- The built-in genre count rises from five to eight, and the genre table's cap with it.
- Each subgenre is tuned and signed off on its own, and each has its own golden.
- An edit to the parent's table can change every subgenre's event stream, and then bumps each changed subgenre's version.
