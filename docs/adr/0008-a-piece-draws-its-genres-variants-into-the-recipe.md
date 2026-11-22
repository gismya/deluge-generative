---
status: proposed
---

# A piece draws its genre's variants into the Recipe

The owner asked in several listening sessions for a rule's choice to be drawn per piece instead of fixed (#461: House S1, S4a, S6; Psytrance S1, S3; Dub's skank 7ths from #359 and its drum styles from #460; swing; Ambient's HOLD), and wants one system for all of them. Decision: a genre lists up to four **variant sets**, each a rule with weighted **variants**. A new Recipe draws one variant per set from fresh entropy, as it rolls key and tempo, and stores the pick as a Recipe field. Each variant is a small patch applied to a RAM copy of the genre when a Generator instance takes the Recipe, so the step path reads genre data as it does today. Regenerate re-draws every set the player left on RANDOM. The Compose screen and the Perform menu get a VARIANTS submenu where the player fixes a set to one variant or leaves it on RANDOM. One kind of set, used by Dub's drum styles, may also change its variant at a Drop. That change is a keyed draw from the seed, so nothing new is logged. Decision: the owner's triage of #461 (stored, not derived; Regenerate re-draws; a menu in Compose and Regenerate). This ADR proposes the rest.

This reopens a rule ADR 0006 relied on: "variety comes from facts, sound picks and the seed, never from pieces to pick from". A variant set is a piece to pick from. ADR 0006's other objections don't apply: a set never changes the roster, and only a Drop-changing set changes a rule mid-piece, at a line where patterns re-pick anyway.

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
	ChangeAt changeAt = ChangeAt::Never; // or Drop
	int16_t changeChance = 0;            // per-mille at each Drop, for ChangeAt::Drop
};
// Genre: std::span<const VariantSet> variantSets;  at most kMaxVariantSets = 4
```

`ResolvedGenre` is a copy of the `Genre` (260 B on ARM) and of its parts (188 B each, 6 at most), 1.4 KB in all. A patch writes fields of the copy. It sets a scalar (a chance, a Build length) or points a span or pointer at another constexpr table (a part's pattern pool, a Chord graph, an Arp figure, a Break's keeps). One patch function per variant covers every row in #461:

| Row | Patch target |
|---|---|
| House S1, how often a Drop brings a new loop | `ChordGraph::loopChance`, through an alternate graph |
| House S4a, the Build's shape | `ArcSpec` Build fields and roll |
| House S6, the Bass's pickup | the Bass part's pattern pool |
| Psytrance S1, the Arp's figure | the Arp part's `ArpSpec` |
| Psytrance S3, the Break | `ArcSpec` Break offsets and keeps |
| Dub Q359, 7th or plain skank | the skank's chord Extensions (where its retune puts them) |
| Dub S5, drum style (#460) | the Drums lanes' pools, `ChangeAt::Drop` |

Two rows don't need a set. **Swing** is already a musical fact with a Recipe field, a SWING row and a Regenerate roll. It only lacks a range, so it becomes a fact range like tempo (`Facts::swingLow/High`, and maybe an interval list). Swing is driver-side, outside the event stream (§6.3), so no golden moves. **Ambient's HOLD** varies per bar line, not per piece, and stays the per-bar chance it is. A set over `holdSharedChance` values is possible if the owner wants each piece to lean one way.

A genre with no sets keeps its Generator pointing at its constexpr table. No copy is made and nothing changes for it.

## The Recipe fields

- `uint8_t variant[kMaxVariantSets]`: each set's pick, by index in the genre's list. For a Drop-changing set, this is the variant the piece opens with.
- `uint8_t variantFixed`: a bit per set. Set means the player fixed it, and Regenerate keeps it. Clear means RANDOM.

That is 5 bytes. They are stored, not derived, as the owner decided, so a Recipe file, a Capture header and AS IT BEGAN carry them as they carry the key.

- **Drawing.** `newRecipe()` draws each set by weight from the same fresh-entropy stream as the facts, after tempo, root and scale. The facts any entropy gave before stay the same, and a genre with no sets draws nothing.
- **Regenerate** re-draws every RANDOM set and keeps every fixed one. **A genre change** draws all of the new genre's sets, and all of them are RANDOM, because the sets belong to the old genre. **A Recipe LOAD** plays the file's picks.
- **Other genres' streams (§6).** The per-piece draw uses fresh entropy, not the seed, so it moves no keyed draw. A Drop change appends one `Purpose`, `VariantChange`, at the end of the enum. A new purpose changes only the draws that use it, and only genres with a `ChangeAt::Drop` set use it. A genre's sets change only its own stream, and only through its retune, which bumps its version.
- **Recipe file.** One child per set: `<variant set="drumStyle" pick="rockers" fixed="1"/>`, by stable names, so reordering the data doesn't break a file. An unknown set is skipped, as the reader skips anything it doesn't know. A set the file leaves out, or a pick it doesn't know, is drawn fresh and left on RANDOM, as a missing sound is re-rolled. A genre that gains a set has a new version, so the `OLDER <GENRE>` notice already tells the player the file plays differently.
- **Capture header.** The header stores the Recipe as `PieceStart` holds it, so the 5 bytes come with it. `PieceInfo::kFormat` goes from 2 to 3, and an image from before can't be read. Unmarked pieces are cleaned at every Perform entry and marked ones once Capture saves them, so only a marked piece left on the card across a firmware update is lost.
- **Capture replay** builds the Generator from the header's Recipe, picks included, and re-runs Drop changes from the seed. Nothing new is logged.

## When the genre copy is built, and Drop changes

- An instance builds its `ResolvedGenre` when it takes a Recipe: Perform entry, a transition's preload, LOAD, the Compose build, the harness. This happens in task context, alongside the roster preload.
- **ChangeAt::Drop** (Dub S5). At each Drop, one keyed draw `(seed, kArcPart, VariantChange, Drop occurrence, set)` against `changeChance` decides whether the variant changes. A second draw picks the new variant by weight, leaving out the one playing. The instance re-applies its copy from the constexpr genre with the new picks, at the decision of the Drop's first step. That is a 1.4 KB copy and a patch, once a Drop. A fixed set never changes, and a RANDOM set opens on its drawn pick.
- **Rule for a Drop-changing set:** its patches may touch only data the Drop reads afresh, such as pattern pools, fills and drop-out entries. They may never touch what the look-ahead reads across the line: Arc lengths, the chord walk, Registers. A harness spec checks it by diffing each variant's copy against the genre's.
- **House S1 isn't a Drop change.** The Chord-loop rule already draws a new loop at each Drop by `loopChance`. The per-piece variant is that chance (1 Drop in 3, 1 in 2, …). "Going back to an earlier loop" is a Chord-loop change in House's retune: the `ChordLoop` draw already keys on the loop's index, so a return re-keys an earlier index.

## The menu

Both screens use stock submenu rows and the stock enum editor, in the stock fonts (§13.5).

- **Compose screen** (§14.4): a row **VARIANTS** `n ▸` after SCALE, shown only when the genre has sets. Its submenu is titled `VARIANTS`, with one row per set in the genre's order: the set's OLED name on the left and the setting on the right.
  - A fixed set shows its variant: `SKANK      7THS`.
  - A RANDOM set shows its drawn variant in brackets: `DRUMS  (ROCKERS)`.
  - Pushing a row opens the stock enum editor on `RANDOM`, then each variant by OLED name. Picking a variant fixes the set to it. Picking RANDOM draws again at once from fresh entropy, so the Recipe always holds a concrete pick and the brackets show it.
  - REGENERATE re-draws only the bracketed rows.
- **Perform menu** (§13.6): **VARIANTS ▸** after REGENERATE, with the same submenu. A change sets what the **next Regenerate** does and doesn't touch the playing piece. A bracketed row shows what plays now. A fixed row shows the variant the next Regenerate will play.
- **Names.** A set's OLED name is at most 8 characters and a variant's at most 9, so the longest row (`(` + 9 + `)` beside 8) fits 128 px in the menu font. A harness spec checks every genre's names against the font metrics `oled_page_spec` already uses.

## Costs

- **Flash:** about 24 B per set and 16 B per variant, plus each patch function (tens of bytes) and any alternate table it points at, which is the retune's data, not the mechanism's.
- **RAM:** 5 B per Recipe (the Current Recipe, each instance's copy, the header). A `ResolvedGenre` of 1.4 KB per Generator instance, 2.8 KB for Perform's two, kept outside the Generator block. Inside it, it would take half of H18's 2,776 B headroom. It is internal RAM, because the step reads it. The build ticket measures it on ARM and states it in §20.
- **Step path (§20):** no new work. The step reads `genre_` as it does now. Each Drop adds one keyed draw per Drop-changing set, plus the copy when the variant changes.
- **Steering log:** 5 B more in each header and no new record kinds.
- **Recipe file:** about 50 B per set.

## Testing

- **Goldens.** The golden case pins each set to its variant 0: `RecipeOverrides` gains `variant[]`, and `golden_case` sets it. A genre lists the variant that plays today first, so adding a set whose variant 0 is today moves no golden. A retune that changes what plays still bumps the version and recuts the golden (spec §21.2).
- **Share specs.** Over 1,000 entropies, each variant's share of new Recipes is within tolerance of its weight. Over 1,000 seeds, a Drop-changing set changes at its `changeChance` per Drop and never at a non-Drop line.
- **Validity.** Every variant of every set, alone and in each combination (at most 8⁴, in practice under 30), passes the genre's checks (`harmonyValid`, `loopNotesValid`, …), in a harness spec.
- **Contract specs.** The existing Capture-replay and Perform-matches-Compose specs run on a genre with a Drop-changing set. A Recipe file round-trip spec covers picks, fixed bits, unknown names and missing sets.
- **Tuning tools.** `gen render` and listening sessions take `--variant set=name`, so a round can render each variant.

## Considered options

- **Derived from the seed, nothing stored.** No new Recipe fields, but the owner ruled it out: a hand-set variant needs a stored value, and NEW SEED would change the variant.
- **One genre table per combination,** precomposed as constexpr overrides, as subgenres are. The step path doesn't change and there's no RAM copy, but the count multiplies: House's three sets of three are 27 tables of 260 B plus up to 1.1 KB of parts each.
- **Variant lookups at each point of use** (`variant(Rule::ArpFigure)` inside the flavours). No copy, but every rule grows a branch on the step path, and each new set touches core code instead of genre data.
- **Declarative patches** (`{field, part, value}` records) instead of functions. They are easier to diff and print, but they need a field enum covering most of `Genre` and `PartSpec`. The function patch plus the copy diff spec gets the same check more cheaply.
- **Subgenres for each variant** (ADR 0006's route). That is right for a different roster or palette. For a 7th-or-plain skank it would double Dub's genre rows and lose the per-piece draw.

## Open questions for the owner

1. **The word "variant".** The glossary lists "Variant" as a term to avoid for Subgenre. *Recommend:* keep "variant" and "variant set", which are the words you used, and change Subgenre's avoid list to say a variant is a set's option. The build ticket adds both terms to the glossary.
2. **RANDOM vs fixed on the OLED.** *Recommend:* brackets for a drawn variant (`(ROCKERS)`), the bare name for a fixed one, and `RANDOM` as the editor's first entry. The alternative is the word `RANDOM` in the row, which hides what plays.
3. **Perform: does a fixed pick act now?** *Recommend:* no, only at the next Regenerate. Acting at the next Drop would need a new steering command, a log record and a mid-piece re-resolve for sets that aren't Drop-changing.
4. **A fixed Drop-changing set.** *Recommend:* it never changes. RANDOM opens on a drawn pick and may change at Drops.
5. **Swing as a fact range, not a set.** *Recommend:* yes, with a range per genre, and a swing interval list only if a genre asks for one.
6. **Ambient's HOLD.** *Recommend:* leave it as the per-bar chance (#456). Make it a set only if you want each piece to lean towards holding or re-striking.
7. **House S1 as a per-piece chance plus a "return" in the Chord-loop rule,** not a Drop change. *Recommend:* yes.
8. **Golden pins variant 0.** *Recommend:* yes, and each genre lists today's behaviour first, so the mechanism lands without moving a golden.

## Proposed build tickets (not filed)

1. **Variant sets: the mechanism.** `VariantSet`, `ResolvedGenre`, the Recipe fields and their draw, Regenerate and genre-change rules, the Recipe file, Capture header format 3, `RecipeOverrides` and the golden pin, the share, validity and round-trip specs, `--variant` in the tuning tools, and §3, §6, §16, §17 and §20 updated. A first set proves it: Dub's skank 7ths (Q359), as Dub's retune.
2. **Variant sets: the menu.** VARIANTS ▸ on the Compose screen and in the Perform menu, the OLED name spec, §13.6 and §14.4. A hardware run is in the test plan.
3. **Drop changes.** `ChangeAt::Drop`, the `VariantChange` purpose, re-resolving at the Drop, and the patch-field spec. It lands with Dub's drum styles (#460 item 1) as Dub's retune.
4. **House retune:** S1 (a per-piece new-loop chance, and a return to an earlier loop), S4a (Build A/B/C) and S6 (pickup today/A/B).
5. **Psytrance retune:** S1 (Arp figures A, C, D, E) and S3 (the Break today/B/C, today weighted highest).
6. **Swing ranges:** a swing range per genre's facts, rolled with tempo. No golden moves.
7. **Ambient HOLD:** only if question 6 goes the other way.
