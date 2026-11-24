---
status: accepted
---

# A piece draws its genre's variants into the Recipe

The owner asked in several listening sessions for a rule's choice to be drawn per piece instead of fixed (#461: House S1, S4a, S6; Psytrance S1, S3; Dub's skank 7ths from #359 and its drum styles from #460; swing; Ambient's HOLD). They want one system for all of them. Decision: a genre lists up to eight **variant sets**. Each is a rule with 2–8 weighted **variants**. A new Recipe draws one variant per set from fresh entropy, as it rolls key and tempo, and stores the pick as a Recipe field. Each variant is a small patch applied to a RAM copy of the genre when a Generator instance takes the Recipe, so the step path reads genre data as it does today. A Regenerate or genre-change page lets the player narrow each set before the draw. The Compose screen does the same. Perform can switch a **live** set at the next Drop. Decision: the owner's triage of #461, and the owner's grilling of this ADR with the orchestrator.

This reopens a rule ADR 0006 relied on: "variety comes from facts, sound picks and the seed, never from pieces to pick from". A variant set is a piece to pick from. ADR 0006's other objections don't apply. A set never changes the roster, and a rule changes mid-piece only at a Drop, where patterns re-pick anyway. A subgenre still covers a different roster or palette.

## The shape

Genre data, constexpr, beside the genre's other slots:

```cpp
struct Variant {
	const char* name;      // stable, for files: "rockers"
	const char* oledName;  // "ROCKERS"
	uint16_t weight;
	void (*apply)(ResolvedGenre&); // nullptr: the genre as written
};
struct VariantSet {
	const char* name;      // stable, for files: "drumStyle"
	const char* oledName;  // "DRUMS"
	std::span<const Variant> variants;  // 2..8
	bool live = false;                   // Perform may switch it at a Drop
	ChangeAt changeAt = ChangeAt::Never; // or Drop
	int16_t changeChance = 0;            // per-mille at each Drop, for ChangeAt::Drop
};
// Genre: std::span<const VariantSet> variantSets;  at most kMaxVariantSets = 8
```

`ResolvedGenre` is a copy of the `Genre` (260 B on ARM) and of its parts (188 B each, 6 at most), 1.4 KB in all. A patch writes fields of the copy. It sets a scalar (a chance, a Build length) or points a span or pointer at another constexpr table (a part's pattern pool, a Chord graph, an Arp figure, a Break's keeps).

| Row | Patch target | Live |
|---|---|---|
| House S1: a per-piece new-loop chance at each Drop | `ChordGraph::loopChance`, through an alternate graph | probably |
| House S4a: the Build's shape | `ArcSpec` Build fields and roll | no |
| House S6: the Bass's pickup | the Bass part's pattern pool | yes |
| House feel-1 Build (#518): the `filter` Build and `nokick`'s lead-in | `ArcSpec` keeps, roll, Gap, and the Build fields `buildCut`, `buildCutBars`, `buildCutLeads`, `buildRiser` | no |
| House feel-1 Bass (#518): the riff, or the offbeat line | the Bass part's pool, tier band and `LoopSpec` | no |
| House feel-1 chords (#518): the loop or the vamp | `HarmonySpec::graph`, through alternate graphs (the vamp's moves, cadence and loop length, keeping S1's loop chance) | no: it moves the chord lines |
| Psytrance S1: the Arp's figure | the Arp part's `ArpSpec` | yes |
| Psytrance S3: the Break | `ArcSpec` Break offsets and keeps | no |
| Dub Q359: 7th or plain skank | the skank's chord Extensions | probably |
| Dub S5: drum style (#460) | the Drums lanes' pools, `ChangeAt::Drop` | yes |

For the "probably" rows, the build ticket confirms the voicing look-ahead, and that a change lands before the Drop's loop draw. House S1's "return to an earlier loop" is a Chord-loop change in House's retune: the `ChordLoop` draw already keys on the loop's index, so a return re-keys an earlier index.

Two rows don't need a set. **Swing** becomes a range per genre on the swing fact it already is (`Facts::swingLow/High`), rolled with tempo. Swing is driver-side, outside the event stream (§6.3), so no golden moves. **Ambient's HOLD** varies per bar, not per piece, and stays the per-bar chance it is (#456).

A genre with no sets keeps its Generator pointing at its constexpr table. No copy is made and nothing changes for it.

## The Recipe fields

Per set, `uint8_t pick` (the variant playing, by index in the genre's list) and `uint8_t allowed` (a bit per variant): 16 B for 8 sets. Every bit set is RANDOM. One bit is a fixed variant. Several bits narrow the set to a subset. The fields are stored, not derived, so a Recipe file, a Capture header and AS IT BEGAN carry them as they carry the key.

- **Drawing.** A new Recipe and Regenerate draw each set by weight within its allowed variants. The draw uses the same fresh-entropy stream as the facts, after tempo, root and scale, so the facts any entropy gave before stay the same.
- **NEW SEED keeps every variant.** A variant is a Recipe value, not a seeded draw.
- **A genre change** draws the new genre's sets. **A Recipe LOAD** plays the file's picks and allowed masks.
- **Other genres' streams (§6).** The per-piece draw uses fresh entropy, not the seed, so it moves no keyed draw. A Drop change appends one `Purpose`, `VariantChange`, at the end of the enum. A new purpose changes only the draws that use it, and only genres with a `ChangeAt::Drop` set use it. A genre's sets change only its own stream, and only through its retune, which bumps its version.
- **Recipe file.** One child per set, by stable names, so reordering the data doesn't break a file: `<variant set="drumStyle" pick="rockers" allow="rockers,steppers"/>`. When `allow` is absent, every variant is allowed. An unknown set is skipped, as the reader skips anything it doesn't know. An unknown variant name in `allow` is dropped. A set the file leaves out, or a pick it doesn't know, is drawn fresh within what's allowed, as a missing sound is re-rolled. A genre that gains a set has a new version, so the `OLDER <GENRE>` notice already tells the player the file plays differently.
- **Capture header.** The header stores the Recipe as `PieceStart` holds it, so the fields come with it. `PieceInfo::kFormat` goes from 2 to 3. A marked piece that wasn't saved is lost across a firmware update, which is acceptable because M4 isn't released.
- **SAVE** after a live change stores the variant playing now.

## Drop changes and live steering

- An instance builds its `ResolvedGenre` when it begins (`Generator::begin()`, #493): Perform's start, a restart and AS IT BEGAN, a transition's incoming instance, the Compose build, Capture's replay, the harness. A transition's incoming instance begins in task context; a Perform start, a restart and AS IT BEGAN begin in the tick, which the orchestrator accepted for #493 (#503 R1): it is a 1,396 B copy on ARM and the patches, next to the larger reset `begin()` already does. It isn't timed on its own (estimate: a few µs at 400 MHz). On hardware (#503 R1, Release test-port build at c60280e19), the H1 windows around starts held a mean of 77–121 µs a decision. Transitions' start steps into Dub took 266–275 µs, against 212–390 µs for the session's other starts; the incoming copy is built outside that timed window.
- **ChangeAt::Drop** (Dub's drums). At each Drop, one keyed draw `(seed, kArcPart, VariantChange, the Drop's Groove occurrence, set)` against `changeChance` decides whether the variant changes. A second draw picks the new one by weight within the allowed variants, leaving out the one playing. A set allowed only one variant never changes. Capture replays it from the seed, with nothing logged.
- **Live switch.** The Perform menu's ticks for a set are a new steering command carrying them as a mask, queued for the next Drop and logged where it lands (ADR 0005), so Capture replays it. Only a `live` set takes one. At that Drop the set allows the ticks: the variant playing stays if ticked, else a keyed draw picks within them, so replay needs nothing more logged. That replaces the Drop's own change for the set; later Drop changes draw within the ticks, and one tick fixes the variant. A set that never changes at a Drop plays what the ticks decide there until a Regenerate. (Owner, #508 R1: a multi-select, as on the Regenerate page, rather than a plain pick.)
- **Both** re-apply the instance's copy from the constexpr genre with the new picks, at the decision of the Drop's first step. That is a 1.4 KB copy and a patch, once at a Drop.
- **Rule for a live or Drop-changing set:** its patches may touch only data the Drop reads afresh, such as pattern pools, fills, drop-out entries and a part's flavour spec. They may never touch what the look-ahead reads across the line: Arc lengths, the Build, the Break, Registers. A harness spec checks this by diffing each variant's copy against the genre's.

## The menus

All of them are stock submenus and editors in the stock fonts (§13.5).

- **The Regenerate and genre-change page** replaces today's transition context menu (§13.6). Picking REGENERATE, or a genre, opens a stock submenu titled `REGENERATE` or `<GENRE>?`, with these rows:
  - **TRANSITION**: DROP, FADE or CUT, defaulting to the incoming genre's transition entry.
  - **VARIANTS ▸**: one row per set of the incoming genre. The row shows `RANDOM` when every variant is allowed, the variant's name when one is, and `n OF m` for a subset. A row is edited with a stock multi-select, a tick per variant.
  - **OPTIONS ▸**: later, for things like keeping MIDI instruments or the reload wait. Not built now.
  - **GO**, last. The cursor starts on GO, so SELECT, SELECT is as quick as today.

  BACK cancels. The page always opens with every set on RANDOM: drawing between variants is the point, and narrowing a set is for that one Regenerate. A mid-set LOAD shows the same page with only TRANSITION (and later OPTIONS), because the file brings its own variants.
- **The Perform menu** gets **VARIANTS ▸**: the page's rows and multi-selects on the playing piece's sets, each row showing its ticks (`RANDOM`, a name, `n OF m`). Pushing a live set's row opens its multi-select. A change queues for the next Drop, shows as `VARIANT DROP` in QUEUE, and BACK cancels it like other queued actions; leaving with the ticks as they were queues nothing. A set that isn't live shows `AT REGEN` when pushed.
- **The Compose screen** gets a single **VARIANTS ▸** row after SCALE, shown only when the genre has sets. It opens the same per-set submenu, with the multi-select. REGENERATE there draws within each set's allowed variants, which the Recipe keeps.
- **Names.** A set's OLED name is at most 8 characters and a variant's at most 9, so a row fits 128 px in the menu font. `regenerate_page_spec` checks every genre's names, and the page's titles, against the stock fonts' metrics (`src/deluge/gui/fonts/fonts.c`, linked into the harness).

## Costs

- **Flash:** about 24 B per set and 16 B per variant, plus each patch function (tens of bytes) and any alternate table it points at. Those tables are the retune's data, not the mechanism's.
- **RAM:** 16 B per Recipe (the Current Recipe, each instance's copy, the header). One `ResolvedGenre` of 1.4 KB per Generator instance, 2.8 KB for Perform's two. It sits outside the Generator block, where it would otherwise take half of H18's 2,776 B headroom, and in internal RAM, because the step reads it. The build ticket measures it on ARM and states it in §20. The fallback is to copy only the parts a patch touches.
- **Step path (§20):** no new work. The step reads `genre_` as it does now. A Drop adds one keyed draw per Drop-changing set, plus the copy when a variant changes.
- **Steering log:** 16 B more in each header, and one new record kind for a live switch, about 4 B.
- **Recipe file:** about 60 B per set.

## Testing

- **Goldens.** The golden case pins each set to variant 0: `RecipeOverrides` gains the picks, and `golden_case` sets them. Each genre lists today's behaviour first, so adding a set whose variant 0 is today moves no golden. A retune that changes what plays still bumps the version and recuts the golden (spec §21.2).
- **Share specs.** Over 1,000 entropies, each variant's share of new Recipes is within tolerance of its weight, also within a subset. Over 1,000 seeds, a Drop-changing set changes at its `changeChance` per Drop, only within its allowed variants and never off a Drop.
- **Validity specs.** These pass the genre's checks (`harmonyValid`, `loopNotesValid`, …): every variant alone (the other sets on variant 0), every pair of variants across two sets, and 200 random full combinations per genre. For today's sets that is exhaustive.
- **Patch-diff spec.** It checks the live and Drop-changing rule above.
- **Contract specs.**
  - The Capture-replay and Perform-matches-Compose specs run on a genre with a Drop-changing set.
  - A replay spec covers a live switch.
  - A Recipe file round-trip spec covers picks, `allow`, unknown names and missing sets.
- **Tuning tools.** `gen render` and listening sessions take `--variant set=name`, so a round can render each variant.

## Considered options

- **Derived from the seed, with nothing stored.** No new Recipe fields, but a hand-set variant needs a stored value, and NEW SEED would change the variants.
- **One genre table per combination,** precomposed as constexpr overrides, as subgenres are. The step path wouldn't change and there would be no RAM copy, but the count multiplies: three sets of three is 27 tables of up to 1.4 KB each.
- **Variant lookups at each point of use** (`variant(Rule::ArpFigure)` inside the flavours). No copy, but every rule grows a branch on the step path, and each new set touches core code instead of genre data.
- **Declarative patches** (`{field, part, value}` records) instead of functions. They are easier to diff and print, but they need a field enum covering most of `Genre` and `PartSpec`. The patch-diff spec gets the same check more cheaply.
- **Subgenres for each variant** (ADR 0006's route). That is right for a different roster or palette. For a 7th-or-plain skank, it would double Dub's genre rows and lose the per-piece draw.
- **A fixed bit per set** instead of an allowed mask. It is simpler, but it can't narrow a set to a subset.

## Decided in the grilling

1. The terms are **variant set** and **variant**. "Variant" comes off Subgenre's avoid list in the glossary.
2. A genre has at most 8 sets, each with 2–8 variants.
3. Each set stores a pick and an allowed mask. NEW SEED keeps every variant. The Capture header format goes from 2 to 3.
4. A Regenerate and genre-change page (TRANSITION, VARIANTS, later OPTIONS, then GO) replaces the transition context menu. It always opens on RANDOM.
5. Perform's VARIANTS menu switches a live set at the next Drop, as a logged steering command. A set that isn't live waits for Regenerate.
6. A Drop-changing set changes only within its allowed variants.
7. The Compose screen has a single VARIANTS ▸ row after SCALE.
8. Swing is a fact range. Ambient's HOLD stays per bar. House S1 is a per-piece chance plus a return. The golden pins variant 0. Each instance gets a full copy outside the Generator block.
9. The validity spec covers single variants, pairs, and 200 random combinations.

## Build tickets

For a new milestone after M4. The orchestrator files them.

1. **Variant sets: the mechanism.** `VariantSet`, `ResolvedGenre`, the Recipe fields and their draw; the NEW SEED, Regenerate, genre-change and LOAD rules; the Recipe file; Capture header format 3; `RecipeOverrides` and the golden pin; the share, validity, patch-diff and round-trip specs; `--variant` in the tuning tools; §3, §6, §16, §17 and §20 updated. Dub's skank 7ths (Q359) proves it, as Dub's retune.
2. **The Regenerate and genre-change page,** where the transition choice moves, and the Compose VARIANTS row. §13.6 and §14.4, the OLED name spec, and a hardware run.
3. **Drop changes and live steering.** `ChangeAt::Drop`, the `VariantChange` purpose, the Perform VARIANTS menu, the live-switch command and its log record, the replay spec. With Dub's drum styles (#460 item 1).
4. **House retune:** S1 (a per-piece new-loop chance, and a return to an earlier loop), S4a (Build A/B/C), S6 (pickup today/A/B).
5. **Psytrance retune:** S1 (Arp figures A, C, D, E) and S3 (the Break today/B/C, today weighted highest).
6. **Swing ranges:** a swing range in each genre's facts, rolled with tempo.
