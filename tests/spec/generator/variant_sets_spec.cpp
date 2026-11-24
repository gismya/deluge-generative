/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// Variant sets (spec §3.6, ADR 0008): the data rules, the per-piece draw and its shares, the copy an instance plays,
// the validity and patch-diff rules over every genre's variants, the Capture header, and Dub's skank (Q359).

#include "generative/core/capacity.h"
#include "generative/core/golden_case.h"
#include "generative/core/recipe_overrides.h"
#include "generative/core/variants.h"
#include "harness.h"
#include "log_store.h"

#include "cppspec.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace harness;

namespace {

void wider(ResolvedGenre& resolved) {
	resolved.genre.harmony.extensionCeiling = 2;
}
void longerGrooves(ResolvedGenre& resolved) {
	resolved.genre.arc.groovePhrases = {8, 8};
}
void otherRegister(ResolvedGenre& resolved) {
	resolved.parts[1].reg.anchor += 12;
}
void deeperBreak(ResolvedGenre& resolved) {
	resolved.genre.arc.offsets[static_cast<int32_t>(SectionKind::Break)].densityTo -= macroPoints(5);
}
void longerRoll(ResolvedGenre& resolved) {
	resolved.genre.arc.roll.top = RollStage::SixtyFourth;
	resolved.genre.arc.roll.stageFraction[3] = 32;
}
void fewerRepicks(ResolvedGenre& resolved) {
	resolved.genre.repickChance = 50;
}
/// Dub's loop graph with `edit` applied to a copy, for a patch to point at.
template <typename Edit>
const ChordGraph* dubGraphWith(Edit edit) {
	static ChordGraph graph = [&] {
		ChordGraph copy = *genres::dub.harmony.graph;
		edit(copy);
		return copy;
	}();
	return &graph;
}
void otherGrid(ResolvedGenre& resolved) {
	resolved.genre.harmony.graph = dubGraphWith([](ChordGraph& graph) { graph.barsPerChord = 2; });
}
void otherLoopLength(ResolvedGenre& resolved) {
	resolved.genre.harmony.graph = dubGraphWith([](ChordGraph& graph) { graph.halfLoopChance = 0; });
}
void otherMoves(ResolvedGenre& resolved) {
	resolved.genre.harmony.graph = dubGraphWith([](ChordGraph& graph) { graph.repeatWeight += 5; });
}
void otherLoopChances(ResolvedGenre& resolved) {
	resolved.genre.harmony.graph = dubGraphWith([](ChordGraph& graph) {
		graph.loopChance = 500;
		graph.returnChance = 333;
	});
}
/// The Bass's pool cut to its first entry: a frozen Bass's pick could point past it.
void shorterPool(ResolvedGenre& resolved) {
	resolved.parts[1].pool = resolved.parts[1].pool.first(1);
}

/// A made-up set of weights 1 and 3, and one of 2, 1 and 1.
constexpr Variant kOneThree[] = {{.name = "a", .oledName = "A", .weight = 1},
                                 {.name = "b", .oledName = "B", .weight = 3, .apply = wider}};
constexpr Variant kTwoOneOne[] = {{.name = "x", .oledName = "X", .weight = 2},
                                  {.name = "y", .oledName = "Y", .weight = 1, .apply = fewerRepicks},
                                  {.name = "z", .oledName = "Z", .weight = 1}};
constexpr VariantSet kMadeUpSets[] = {{.name = "one", .oledName = "ONE", .variants = kOneThree},
                                      {.name = "two", .oledName = "TWO", .variants = kTwoOneOne}};

/// Dub with the made-up sets instead of its own.
Genre madeUp() {
	Genre genre = genres::dub;
	genre.variantSets = kMadeUpSets;
	return genre;
}

/// Whether two Chord graphs differ at most in their loop chances: a live or Drop-changing set may change only
/// `loopChance` (within looping or not) and `returnChance`, which the Drop's own step reads (spec §3.6, §7.4). The
/// harmonic rhythm, the moves and the loop's length draw are read on every bar for the loop playing.
bool sameChordLines(const ChordGraph* a, const ChordGraph* b) {
	if (a == nullptr || b == nullptr) {
		return a == b;
	}
	return a->homePull == b->homePull && a->moves.data() == b->moves.data() && a->moves.size() == b->moves.size()
	       && a->repeatWeight == b->repeatWeight && a->abaAllowed == b->abaAllowed
	       && a->cadenceTarget == b->cadenceTarget && a->cadencePeriod == b->cadencePeriod
	       && a->barsPerChord == b->barsPerChord && (a->loopChance >= 0) == (b->loopChance >= 0)
	       && a->halfLoopChance == b->halfLoopChance;
}

/// The fields a live or Drop-changing set's patch may never touch (ADR 0008): what the Arc's look-ahead reads across
/// a Drop's line, the Arc's lengths, its Section offsets, the Build (with its roll), the Break and the Registers, and
/// the chord lines (sameChordLines()). True when `a` and `b` agree on them.
bool sameLookAhead(const Genre& a, const Genre& b) {
	const ArcSpec& x = a.arc;
	const ArcSpec& y = b.arc;
	bool same = x.introPhrases == y.introPhrases && x.outroPhrases == y.outroPhrases
	            && x.groovePhrases.low == y.groovePhrases.low && x.groovePhrases.high == y.groovePhrases.high
	            && x.breakPhrases.low == y.breakPhrases.low && x.breakPhrases.high == y.breakPhrases.high
	            && x.buildBars.data() == y.buildBars.data() && x.buildBars.size() == y.buildBars.size()
	            && x.grooveToBuild == y.grooveToBuild && x.dropDecayPhrases == y.dropDecayPhrases
	            && x.breakKeeps == y.breakKeeps && x.buildKeeps == y.buildKeeps && x.buildCut == y.buildCut
	            && x.buildCutBars == y.buildCutBars && x.gap == y.gap && a.phraseBars == b.phraseBars
	            && x.roll.lane == y.roll.lane && x.roll.top == y.roll.top
	            && std::ranges::equal(x.roll.stageFraction, y.roll.stageFraction)
	            && sameChordLines(a.harmony.graph, b.harmony.graph);
	for (int32_t k = 0; k < kNumSectionKinds; k++) {
		const SectionOffsets& o = x.offsets[k];
		const SectionOffsets& q = y.offsets[k];
		same = same && o.energyFrom == q.energyFrom && o.energyTo == q.energyTo && o.densityFrom == q.densityFrom
		       && o.densityTo == q.densityTo;
	}
	for (int32_t p = 0; p < partCount(a); p++) {
		same = same && a.parts[p].reg.anchor == b.parts[p].reg.anchor
		       && a.parts[p].reg.spanLow == b.parts[p].reg.spanLow
		       && a.parts[p].reg.spanHigh == b.parts[p].reg.spanHigh;
	}
	return same;
}

/// Two pools a pick reads the same way: as many entries, each as long.
bool samePickRange(std::span<const Pattern> a, std::span<const Pattern> b) {
	return std::ranges::equal(a, b, [](const Pattern& x, const Pattern& y) { return x.length == y.length; });
}

/// What a pick held across a Drop reads (a frozen part keeps its pool entry, spec §6.7): each part's lanes, in their
/// order, and every pool, as many entries, each as long. True when `a` and `b` agree on them.
bool samePicks(const Genre& a, const Genre& b) {
	bool same = partCount(a) == partCount(b);
	for (int32_t p = 0; same && p < partCount(a); p++) {
		const PartSpec& x = a.parts[p];
		const PartSpec& y = b.parts[p];
		same = samePickRange(x.pool, y.pool) && x.lanes.size() == y.lanes.size();
		for (size_t l = 0; same && l < x.lanes.size(); l++) {
			same = x.lanes[l].lane == y.lanes[l].lane && samePickRange(x.lanes[l].pool, y.lanes[l].pool)
			       && samePickRange(x.lanes[l].fillPool, y.lanes[l].fillPool);
		}
	}
	return same;
}

/// Every set of `genre` that may change mid-piece (live, or at a Drop) keeps the look-ahead, and what a pick held
/// across the Drop reads, in each of its variants.
bool patchesKeepLookAhead(const Genre& genre) {
	for (size_t s = 0; s < genre.variantSets.size(); s++) {
		const VariantSet& set = genre.variantSets[s];
		if (!set.live && set.changeAt == ChangeAt::Never) {
			continue;
		}
		for (size_t v = 0; v < set.variants.size(); v++) {
			VariantPicks picks;
			picks.pick[s] = static_cast<uint8_t>(v);
			auto copy = std::make_unique<ResolvedGenre>();
			const Genre& resolved = resolveGenre(genre, picks, copy.get());
			if (!sameLookAhead(genre, resolved) || !samePicks(genre, resolved)) {
				return false;
			}
		}
	}
	return true;
}

/// Plays `recipe` from SHIFT+PLAY for `bars` bars and gives the most pitch classes part `part` starts at one tick.
int32_t widestChord(const Recipe& recipe, int32_t part, int32_t bars) {
	Run run;
	run.recipe = recipe;
	run.seed = 7;
	run.skipIntro = true;
	const Result result = play(run, bars);
	std::map<int32_t, std::set<int32_t>> classes;
	for (const Event& e : result.events) {
		if (e.kind == EventKind::NoteOn && e.part == part) {
			classes[e.tick].insert(e.pitch % 12);
		}
	}
	size_t widest = 0;
	for (const auto& [tick, pitches] : classes) {
		widest = std::max(widest, pitches.size());
	}
	return static_cast<int32_t>(widest);
}

constexpr int32_t kSkankPart = 2;

} // namespace

// clang-format off
describe variant_sets("Variant sets", $ {
	context("the data (spec §4.4)", _ {
		it("holds every genre's sets to ADR 0008's rules", _ {
			for (const Genre* genre : allGenres()) {
				expect(variantSetsValid(*genre)).to_be_true();
				expect(genreValid(*genre)).to_be_true();
			}
		});
		it("refuses a set with one variant, a weight of 0, a repeated name, or a stable or OLED name too long", _ {
			static constexpr Variant kOne[] = {{.name = "a", .oledName = "A"}};
			static constexpr Variant kZero[] = {{.name = "a", .oledName = "A"}, {.name = "b", .oledName = "B", .weight = 0}};
			static constexpr Variant kTwice[] = {{.name = "a", .oledName = "A"}, {.name = "a", .oledName = "B"}};
			static constexpr Variant kLong[] = {{.name = "a", .oledName = "ABCDEFGHIJ"}, {.name = "b", .oledName = "B"}};
			static constexpr Variant kFits[] = {{.name = "a", .oledName = "ABCDEFGHI"}, {.name = "b", .oledName = "B"}};
			static constexpr Variant kLongName[] = {{.name = "abcdefghijklmnopqrstuvwx", .oledName = "A"},
			                                        {.name = "b", .oledName = "B"}}; // 24 characters: one past the cap
			static constexpr VariantSet kSets[][1] = {
			    {{.name = "s", .oledName = "S", .variants = kOne}},
			    {{.name = "s", .oledName = "S", .variants = kZero}},
			    {{.name = "s", .oledName = "S", .variants = kTwice}},
			    {{.name = "s", .oledName = "S", .variants = kLong}},
			    {{.name = "s", .oledName = "ABCDEFGHI", .variants = kFits}},
			    {{.name = "s", .oledName = "ABCDEFGH", .variants = kFits}},
			    {{.name = "s", .oledName = "S", .variants = kLongName}},
			};
			std::vector<bool> valid;
			for (const auto& sets : kSets) {
				Genre genre = genres::dub;
				genre.variantSets = sets;
				valid.push_back(variantSetsValid(genre));
			}
			expect(valid).to_equal(std::vector<bool>{false, false, false, false, false, true, false});
			static constexpr VariantSet kSameName[] = {{.name = "s", .oledName = "S", .variants = kFits},
			                                           {.name = "s", .oledName = "T", .variants = kFits}};
			Genre twice = genres::dub;
			twice.variantSets = kSameName;
			expect(variantSetsValid(twice)).to_be_false();
		});
	});

	context("the draw (spec §3.6)", _ {
		it("lands each variant at its weight's share over 1,000 new Recipes", _ {
			const Genre genre = madeUp();
			int32_t counts[2][3] = {};
			for (uint32_t entropy = 0; entropy < 1000; entropy++) {
				const Recipe recipe = newRecipe(genre, entropy * 2654435761U);
				counts[0][recipe.variants.pick[0]]++;
				counts[1][recipe.variants.pick[1]]++;
				expect(static_cast<int32_t>(recipe.variants.allowed[0])).to_equal(0b11);
				expect(static_cast<int32_t>(recipe.variants.allowed[1])).to_equal(0b111);
			}
			// Within 5 points of 25/75 and 50/25/25.
			expect(counts[0][0] >= 200 && counts[0][0] <= 300).to_be_true();
			expect(counts[0][1] >= 700 && counts[0][1] <= 800).to_be_true();
			expect(counts[1][0] >= 450 && counts[1][0] <= 550).to_be_true();
			expect(counts[1][1] >= 200 && counts[1][1] <= 300).to_be_true();
			expect(counts[1][2] >= 200 && counts[1][2] <= 300).to_be_true();
		});
		it("draws within a narrowed subset at the subset's weights, and stores the subset", _ {
			const Genre genre = madeUp();
			VariantPicks narrowed;
			narrowed.allowed[0] = 0b10;  // a fixed variant
			narrowed.allowed[1] = 0b110; // y and z, 1 to 1
			int32_t counts[3] = {};
			for (uint32_t entropy = 0; entropy < 1000; entropy++) {
				const Recipe recipe = newRecipe(genre, entropy * 2654435761U, std::nullopt, &narrowed);
				expect(static_cast<int32_t>(recipe.variants.pick[0])).to_equal(1);
				counts[recipe.variants.pick[1]]++;
				expect(recipe.variants.allowed[0] == 0b10 && recipe.variants.allowed[1] == 0b110).to_be_true();
			}
			expect(counts[0]).to_equal(0);
			expect(counts[1] >= 450 && counts[1] <= 550).to_be_true();
			expect(counts[2] >= 450 && counts[2] <= 550).to_be_true();
		});
		it("draws every variant for a set narrowed to none of its own", _ {
			const Genre genre = madeUp();
			VariantPicks narrowed;
			narrowed.allowed[1] = 0b1000; // past the set's three variants
			std::set<int32_t> drawn;
			for (uint32_t entropy = 0; entropy < 200; entropy++) {
				drawn.insert(newRecipe(genre, entropy, std::nullopt, &narrowed).variants.pick[1]);
			}
			expect(drawn.size()).to_equal(size_t{3});
		});
		it("moves no fact: the tempo, key, scale and swing any entropy gave stay, with or without a lean", _ {
			const Genre with = madeUp();
			Genre without = genres::dub;
			without.variantSets = {};
			for (uint32_t entropy = 0; entropy < 300; entropy++) {
				for (const std::optional<Key> lean : {std::optional<Key>{}, std::optional<Key>{Key{2, Scale::Dorian}}}) {
					const Recipe a = newRecipe(with, entropy * 7919U, lean);
					const Recipe b = newRecipe(without, entropy * 7919U, lean);
					expect(a.tempo == b.tempo && a.keyRoot == b.keyRoot && a.scale == b.scale && a.swing == b.swing)
					    .to_be_true();
				}
			}
		});
		it("leaves the picks at 0 for a genre with no sets", _ {
			expect(newRecipe(genres::technoAcid, 12345).variants == VariantPicks{}).to_be_true();
		});
	});

	context("the copy an instance plays (spec §3.6)", _ {
		it("is the genre itself when no pick patches it: no copy is made", _ {
			const Genre genre = madeUp();
			auto storage = std::make_unique<ResolvedGenre>();
			VariantPicks picks; // a and x: neither patches
			expect(&resolveGenre(genre, picks, storage.get()) == &genre).to_be_true();
			picks.pick[1] = 2; // z: no patch either
			expect(&resolveGenre(genre, picks, storage.get()) == &genre).to_be_true();
			expect(&resolveGenre(genres::technoAcid, picks, storage.get()) == &genres::technoAcid).to_be_true();
		});
		it("is a copy with each picked variant's patch applied, its parts its own", _ {
			const Genre genre = madeUp();
			auto storage = std::make_unique<ResolvedGenre>();
			VariantPicks picks;
			picks.pick[0] = 1; // wider
			picks.pick[1] = 1; // fewerRepicks
			const Genre& resolved = resolveGenre(genre, picks, storage.get());
			expect(&resolved == &storage->genre).to_be_true();
			expect(static_cast<int32_t>(resolved.harmony.extensionCeiling)).to_equal(2);
			expect(static_cast<int32_t>(resolved.repickChance)).to_equal(50);
			expect(resolved.parts.data() == storage->parts).to_be_true();
			expect(partCount(resolved)).to_equal(partCount(genre));
			for (int32_t p = 0; p < partCount(genre); p++) {
				expect(std::string(resolved.parts[p].name)).to_equal(std::string(genre.parts[p].name));
			}
			// The constant table is untouched.
			expect(static_cast<int32_t>(genres::dub.harmony.extensionCeiling)).to_equal(1);
		});
		it("is the genre as written for an instance with nowhere to copy it", _ {
			const Genre genre = madeUp();
			VariantPicks picks;
			picks.pick[0] = 1;
			expect(&resolveGenre(genre, picks, nullptr) == &genre).to_be_true();
		});
	});

	context("validity (ADR 0008)", _ {
		it("passes every genre check for each variant alone, each pair across two sets and 200 random combinations", _ {
			auto storage = std::make_unique<ResolvedGenre>();
			const Genre madeUpGenre = madeUp();
			std::vector<const Genre*> genres(allGenres().begin(), allGenres().end());
			genres.push_back(&madeUpGenre);
			int32_t checked = 0;
			for (const Genre* genre : genres) {
				const auto sets = static_cast<int32_t>(genre->variantSets.size());
				auto valid = [&](const VariantPicks& picks) {
					checked++;
					return genreValid(resolveGenre(*genre, picks, storage.get()));
				};
				for (int32_t s = 0; s < sets; s++) {
					for (size_t v = 0; v < genre->variantSets[s].variants.size(); v++) {
						VariantPicks alone;
						alone.pick[s] = static_cast<uint8_t>(v);
						expect(valid(alone)).to_be_true();
						for (int32_t t = s + 1; t < sets; t++) {
							for (size_t w = 0; w < genre->variantSets[t].variants.size(); w++) {
								VariantPicks pair = alone;
								pair.pick[t] = static_cast<uint8_t>(w);
								expect(valid(pair)).to_be_true();
							}
						}
					}
				}
				if (sets == 0) {
					continue;
				}
				EntropyStream rolls(genre->version * 7919U);
				for (int32_t combination = 0; combination < 200; combination++) {
					VariantPicks picks;
					drawVariants(*genre, {}, rolls, picks);
					expect(valid(picks)).to_be_true();
				}
			}
			expect(checked).to_be_greater_than(400); // Dub's and the made-up genre's sets were walked
		});
	});

	context("the patch-diff rule (ADR 0008)", _ {
		it("keeps every live or Drop-changing set's patches off what the look-ahead reads, in every genre", _ {
			for (const Genre* genre : allGenres()) {
				expect(patchesKeepLookAhead(*genre)).to_be_true();
			}
		});
		it("catches a live or Drop-changing set that moves the Arc's lengths, offsets or roll, a Register, a pool's size, the Chord graph beyond its loop chances, and lets one that moves the repicks or the loop chances by", _ {
			static constexpr Variant kArc[] = {{.name = "a", .oledName = "A"},
			                                   {.name = "b", .oledName = "B", .apply = longerGrooves}};
			static constexpr Variant kRegister[] = {{.name = "a", .oledName = "A"},
			                                        {.name = "b", .oledName = "B", .apply = otherRegister}};
			static constexpr Variant kRepicks[] = {{.name = "a", .oledName = "A"},
			                                       {.name = "b", .oledName = "B", .apply = fewerRepicks}};
			static constexpr Variant kOffsets[] = {{.name = "a", .oledName = "A"},
			                                       {.name = "b", .oledName = "B", .apply = deeperBreak}};
			static constexpr Variant kRoll[] = {{.name = "a", .oledName = "A"},
			                                    {.name = "b", .oledName = "B", .apply = longerRoll}};
			static constexpr Variant kPool[] = {{.name = "a", .oledName = "A"},
			                                    {.name = "b", .oledName = "B", .apply = shorterPool}};
			static constexpr Variant kGrid[] = {{.name = "a", .oledName = "A"},
			                                    {.name = "b", .oledName = "B", .apply = otherGrid}};
			static constexpr Variant kLoopLength[] = {{.name = "a", .oledName = "A"},
			                                          {.name = "b", .oledName = "B", .apply = otherLoopLength}};
			static constexpr Variant kMoves[] = {{.name = "a", .oledName = "A"},
			                                     {.name = "b", .oledName = "B", .apply = otherMoves}};
			static constexpr Variant kLoopChance[] = {{.name = "a", .oledName = "A"},
			                                          {.name = "b", .oledName = "B", .apply = otherLoopChances}};
			static constexpr VariantSet kLive[][1] = {
			    {{.name = "s", .oledName = "S", .variants = kArc, .live = true}},
			    {{.name = "s", .oledName = "S", .variants = kRegister, .live = true}},
			    {{.name = "s", .oledName = "S", .variants = kRepicks, .live = true}},
			    {{.name = "s", .oledName = "S", .variants = kArc}}, // not live: Regenerate takes it
			    {{.name = "s", .oledName = "S", .variants = kOffsets, .live = true}},
			    {{.name = "s", .oledName = "S", .variants = kRoll, .live = true}},
			    {{.name = "s", .oledName = "S", .variants = kPool, .changeAt = ChangeAt::Drop, .changeChance = 10}},
			    {{.name = "s", .oledName = "S", .variants = kGrid, .live = true}},
			    {{.name = "s", .oledName = "S", .variants = kLoopLength, .live = true}},
			    {{.name = "s", .oledName = "S", .variants = kMoves, .live = true}},
			    {{.name = "s", .oledName = "S", .variants = kLoopChance, .live = true}},
			};
			std::vector<bool> kept;
			for (const auto& sets : kLive) {
				Genre genre = genres::dub;
				genre.variantSets = sets;
				kept.push_back(patchesKeepLookAhead(genre));
			}
			expect(kept).to_equal(std::vector<bool>{false, false, true, true, false, false, false, false, false, false, true});
		});
	});

	context("the Recipe overrides and the goldens (spec §21.2, §21.5)", _ {
		it("parses set=name into the set and variant, and fixes the pick and allowed variants to that one alone", _ {
			const Genre& dub = genres::dub;
			VariantPicks picks = newRecipe(dub, 3).variants;
			fixVariant(picks, parseVariantFix(dub, "skank=plain").value());
			expect(picks.pick[0] == 1 && picks.allowed[0] == 0b10).to_be_true();
			fixVariant(picks, parseVariantFix(dub, "skank=sevenths").value());
			expect(picks.pick[0] == 0 && picks.allowed[0] == 0b01).to_be_true();
			const std::optional<VariantFix> plain = parseVariantFix(dub, "skank=plain");
			expect(plain.has_value() && plain->set == 0 && plain->variant == 1).to_be_true();
			for (const char* unknown : {"skank=jazz", "drums=plain", "plain", "skank=", "=plain"}) {
				expect(parseVariantFix(dub, unknown).has_value()).to_be_false();
			}
		});
		it("pins every set of every genre to its first variant in the golden case", _ {
			for (const Genre* genre : allGenres()) {
				for (uint32_t entropy = 0; entropy < 16; entropy++) {
					RecipeOverrides overrides = golden_case::overrides();
					overrides.entropy = entropy;
					const Recipe recipe = rollWithOverrides(*genre, overrides).recipe;
					for (size_t s = 0; s < genre->variantSets.size(); s++) {
						expect(recipe.variants.pick[s] == 0 && recipe.variants.allowed[s] == 1).to_be_true();
					}
				}
			}
		});
	});

	context("the Capture header (spec §17.2)", _ {
		it("carries the Recipe's picks and allowed variants through a stored piece, at format 3", _ {
			expect(PieceInfo::kFormat).to_equal(static_cast<uint16_t>(3));
			StoredLog stored;
			PieceStart start;
			start.recipe = newRecipe(genres::dub, 11);
			start.recipe.variants.pick[0] = 1;
			start.recipe.variants.allowed[0] = 0b10;
			start.seed = 4;
			stored.log.beginPiece(start);
			stored.log.mark(true);
			stored.log.endPiece(16, PieceEnd::Stopped);
			const PieceImage image = stored.store.stored(0);
			expect(image.bytes.empty()).to_be_false();
			expect(image.piece().start().recipe == start.recipe).to_be_true();
		});
	});

	context("Dub's skank (Q359, #359)", _ {
		it("lists the skank set: the 7ths it played before, first, and plain triads, evenly", _ {
			const Genre& dub = genres::dub;
			expect(dub.variantSets.size()).to_equal(size_t{3}); // and the drum styles (#495, drop_changes_spec), the Break (dub_spec)
			const VariantSet& skank = dub.variantSets[0];
			expect(std::string(skank.name)).to_equal(std::string("skank"));
			expect(std::string(skank.variants[0].name)).to_equal(std::string("sevenths"));
			expect(skank.variants[0].apply == nullptr).to_be_true();
			expect(std::string(skank.variants[1].name)).to_equal(std::string("plain"));
			expect(skank.variants[0].weight).to_equal(skank.variants[1].weight);
			expect(skank.live).to_be_true(); // #495: no look-ahead reads the Extensions across a Drop
		});
		it("plays 7th chords on the Skank with sevenths, and never more than a triad with plain", _ {
			Recipe recipe = newRecipe(genres::dub, 0);
			fixVariant(recipe.variants, parseVariantFix(genres::dub, "skank=sevenths").value());
			expect(widestChord(recipe, kSkankPart, 16)).to_equal(4);
			fixVariant(recipe.variants, parseVariantFix(genres::dub, "skank=plain").value());
			expect(widestChord(recipe, kSkankPart, 16)).to_equal(3);
		});
		it("plays plain as written on a bare Generator, which has nowhere to copy the genre", _ {
			Recipe recipe = newRecipe(genres::dub, 0);
			fixVariant(recipe.variants, parseVariantFix(genres::dub, "skank=plain").value());
			Generator bare;
			bare.start(recipe, 7, Takes{}, true);
			SteeringQueue queue;
			std::map<int32_t, std::set<int32_t>> classes;
			for (int32_t step = 0; step < 16 * kStepsPerBar; step++) {
				bare.decideNextStep(queue, nullptr);
				Event e;
				while (bare.popDue(INT32_MAX, &e)) {
					if (e.kind == EventKind::NoteOn && e.part == kSkankPart) {
						classes[e.tick].insert(e.pitch % 12);
					}
				}
			}
			size_t widest = 0;
			for (const auto& [tick, pitches] : classes) {
				widest = std::max(widest, pitches.size());
			}
			expect(widest).to_equal(size_t{4});
		});
	});
});

CPPSPEC_SPEC(variant_sets)
