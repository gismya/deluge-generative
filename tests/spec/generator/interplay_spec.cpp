// Interplay rules (spec §5.5): filters that only remove a part's own onsets, keyed so a higher strength only ever
// removes more, reading their target's planned onsets. Rules only target earlier parts and earlier Drums lanes.

#include "harness.h"

#include "cppspec.hpp"

#include <set>
#include <vector>

using namespace harness;

namespace {

constexpr Pattern kFour[] = {{"X...X...X...X...", "................", 0}};
constexpr Pattern kBusy[] = {{"9999999999999999", "................", 0}};
constexpr DegreeWeight kRoot[] = {{0, 1}};

constexpr InterplayRule kHatLocksKick[] = {{.kind = RuleKind::Lock, .target = {0, Lane::Kick}}};
constexpr InterplayRule kKickLocksHat[] = {{.kind = RuleKind::Lock, .target = {0, Lane::ClosedHat}}};
constexpr InterplayRule kLocksItself[] = {{.kind = RuleKind::Lock, .target = {0, Lane::ClosedHat}}};
constexpr InterplayRule kLocksMissingLane[] = {{.kind = RuleKind::Lock, .target = {0, Lane::Snare}}};
constexpr InterplayRule kLocksBass[] = {{.kind = RuleKind::Lock, .target = {1}}};
constexpr InterplayRule kLocksDrums[] = {{.kind = RuleKind::Lock, .target = {0}}};
constexpr InterplayRule kLocksMissingPart[] = {{.kind = RuleKind::Lock, .target = {5}}};

constexpr LaneSpec kLanes[] = {
    {.lane = Lane::Kick, .pool = kFour},
    {.lane = Lane::ClosedHat, .pool = kBusy, .rules = kHatLocksKick},
};
constexpr LaneSpec kKickLocksLaterHat[] = {
    {.lane = Lane::Kick, .pool = kFour, .rules = kKickLocksHat},
    {.lane = Lane::ClosedHat, .pool = kBusy},
};
constexpr LaneSpec kHatLocksItself[] = {
    {.lane = Lane::Kick, .pool = kFour},
    {.lane = Lane::ClosedHat, .pool = kBusy, .rules = kLocksItself},
};
constexpr LaneSpec kHatLocksSnare[] = {
    {.lane = Lane::Kick, .pool = kFour},
    {.lane = Lane::ClosedHat, .pool = kBusy, .rules = kLocksMissingLane},
};
constexpr LaneSpec kHatLocksBass[] = {
    {.lane = Lane::Kick, .pool = kFour},
    {.lane = Lane::ClosedHat, .pool = kBusy, .rules = kLocksBass},
};

constexpr PartSpec bass(std::span<const InterplayRule> rules = {}) {
	return {.role = Role::Bass, .flavour = Flavour::Loop, .pool = kBusy, .rules = rules, .loop = {.degrees = kRoot}};
}

constexpr PartSpec kInOrder[] = {{.role = Role::Drums, .lanes = kLanes}, bass(kLocksDrums)};
constexpr PartSpec kBassOnKick[] = {{.role = Role::Drums, .lanes = kLanes}, bass(kHatLocksKick)};
constexpr PartSpec kLaterLane[] = {{.role = Role::Drums, .lanes = kKickLocksLaterHat}, bass()};
constexpr PartSpec kSelf[] = {{.role = Role::Drums, .lanes = kHatLocksItself}, bass()};
constexpr PartSpec kMissingLane[] = {{.role = Role::Drums, .lanes = kHatLocksSnare}, bass()};
constexpr PartSpec kLaterPart[] = {{.role = Role::Drums, .lanes = kHatLocksBass}, bass()};
constexpr PartSpec kMissingPart[] = {{.role = Role::Drums, .lanes = kLanes}, bass(kLocksMissingPart)};
constexpr PartSpec kBassFirst[] = {bass(kLocksDrums), {.role = Role::Drums, .lanes = kLanes}};

constexpr Genre genreOf(std::span<const PartSpec> parts) {
	return {.name = "interplay", .parts = parts};
}

constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// One-bar phrases and no Arc offsets, so the Recipe's macros are what plays.
constexpr Genre streamGenre(std::span<const PartSpec> parts) {
	return {
	    .name = "interplay",
	    .facts = {.scales = kScales},
	    .phraseBars = 1,
	    .parts = parts,
	    .arc = {.buildBars = kBuildBars, .dropLiftEnergy = 0, .dropLiftDensity = 0, .offsets = {}},
	};
}

/// A kick on the beats and a hat that plays every step at full Density, under `rule`.
template <int8_t Strength, int8_t Sensitivity, bool Off>
struct HatOnKick {
	static constexpr InterplayRule rules[] = {{.kind = RuleKind::Lock,
	                                           .target = {0, Lane::Kick},
	                                           .off = Off,
	                                           .strength = Strength,
	                                           .sensitivity = Sensitivity}};
	static constexpr LaneSpec lanes[] = {
	    {.lane = Lane::Kick, .pool = kFour},
	    {.lane = Lane::ClosedHat, .pool = kBusy, .rules = rules},
	};
	static constexpr PartSpec parts[] = {{.role = Role::Drums, .lanes = lanes}};
	static constexpr Genre genre = streamGenre(parts);
};

Run runOf(const Genre& genre, uint32_t seed = 1, int32_t complexity = 25, int32_t density = 50) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Density, macroPoints(density));
	run.recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	run.recipe.macros.set(Macro::Motion, 0); // no re-picks at phrase lines: every pick holds
	run.seed = seed;
	return run;
}

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;

/// The steps within the bar where `lane` hit, in `bar`.
std::set<int32_t> hits(const Result& result, Lane lane, int32_t bar = 0) {
	std::set<int32_t> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.lane == lane && e.tick / kBarTicks == bar) {
			out.insert((e.tick % kBarTicks) / kTicksPerStep);
		}
	}
	return out;
}

bool subset(const std::set<int32_t>& inner, const std::set<int32_t>& outer) {
	for (int32_t step : inner) {
		if (outer.count(step) == 0) {
			return false;
		}
	}
	return true;
}

} // namespace

// clang-format off
describe interplay("Interplay rules", $ {
	it("only target earlier parts and earlier Drums lanes, and a genre that breaks it is rejected", _ {
		for (const Genre* genre : allGenres()) {
			expect(interplayRulesValid(*genre)).to_be_true();
		}
		expect(interplayRulesValid(genreOf(kInOrder))).to_be_true();
		expect(interplayRulesValid(genreOf(kBassOnKick))).to_be_true();

		expect(interplayRulesValid(genreOf(kLaterLane))).to_be_false();
		expect(interplayRulesValid(genreOf(kSelf))).to_be_false();
		expect(interplayRulesValid(genreOf(kMissingLane))).to_be_false();
		expect(interplayRulesValid(genreOf(kLaterPart))).to_be_false();
		expect(interplayRulesValid(genreOf(kMissingPart))).to_be_false();
		expect(interplayRulesValid(genreOf(kBassFirst))).to_be_false();
	});

	it("Lock keeps a part to the target's onsets, or with off to the 8ths after them", _ {
		Result locked = play(runOf(HatOnKick<100, 0, false>::genre), 2);
		expect(hits(locked, Lane::ClosedHat) == std::set<int32_t>{0, 4, 8, 12}).to_be_true();
		Result off = play(runOf(HatOnKick<100, 0, true>::genre), 2);
		expect(hits(off, Lane::ClosedHat) == std::set<int32_t>{2, 6, 10, 14}).to_be_true();
		Result none = play(runOf(HatOnKick<0, 0, true>::genre), 2);
		expect(hits(none, Lane::ClosedHat).size()).to_equal(size_t{16});
	});

	it("only ever removes more onsets at a higher strength, and loops its shape while the pick holds", _ {
		for (uint32_t seed = 1; seed <= 20; seed++) {
			std::vector<std::set<int32_t>> byStrength = {
			    hits(play(runOf(HatOnKick<0, 0, true>::genre, seed), 1), Lane::ClosedHat),
			    hits(play(runOf(HatOnKick<25, 0, true>::genre, seed), 1), Lane::ClosedHat),
			    hits(play(runOf(HatOnKick<50, 0, true>::genre, seed), 1), Lane::ClosedHat),
			    hits(play(runOf(HatOnKick<75, 0, true>::genre, seed), 1), Lane::ClosedHat),
			    hits(play(runOf(HatOnKick<100, 0, true>::genre, seed), 1), Lane::ClosedHat),
			};
			for (size_t i = 1; i < byStrength.size(); i++) {
				expect(subset(byStrength[i], byStrength[i - 1])).to_be_true();
			}
			Result half = play(runOf(HatOnKick<50, 0, true>::genre, seed), 4);
			for (int32_t bar = 1; bar < 4; bar++) {
				expect(hits(half, Lane::ClosedHat, bar) == hits(half, Lane::ClosedHat, 0)).to_be_true();
			}
		}
	});

	it("thins about as many against steps as its strength says", _ {
		int32_t kept = 0;
		for (uint32_t seed = 1; seed <= 50; seed++) {
			kept += static_cast<int32_t>(hits(play(runOf(HatOnKick<50, 0, true>::genre, seed), 1), Lane::ClosedHat).size()) - 4;
		}
		expect(kept).to_be_between(50 * 12 * 40 / 100, 50 * 12 * 60 / 100); // 12 against steps a bar
	});

	it("reads its target's planned onsets: after Density thinning and the target's own rules, all of a Kit's lanes", _ {
		// Importance 9 plays at Density 20 and 5 doesn't (spec §8.1's default curve).
		static constexpr Pattern kKick[] = {{"X.......5.......", "................", 0}};
		static constexpr InterplayRule kHatRules[] = {{.kind = RuleKind::Lock, .target = {0, Lane::Kick}, .off = true}};
		static constexpr LaneSpec kLanes[] = {
		    {.lane = Lane::Kick, .pool = kKick},
		    {.lane = Lane::ClosedHat, .pool = kBusy, .rules = kHatRules},
		};
		static constexpr InterplayRule kBassRules[] = {{.kind = RuleKind::Lock, .target = {0}}};
		static constexpr PartSpec kParts[] = {{.role = Role::Drums, .lanes = kLanes}, bass(kBassRules)};
		static constexpr Genre kGenre = streamGenre(kParts);
		auto bassHits = [](const Result& result) {
			std::set<int32_t> out;
			for (const Event& e : only(result.events, EventKind::NoteOn)) {
				if (e.part == 1 && e.tick < kBarTicks) {
					out.insert(e.tick / kTicksPerStep);
				}
			}
			return out;
		};
		Result sparse = play(runOf(kGenre, 1, 25, 20), 1);
		expect(hits(sparse, Lane::Kick) == std::set<int32_t>{0}).to_be_true();
		expect(hits(sparse, Lane::ClosedHat) == std::set<int32_t>{2}).to_be_true();
		expect(bassHits(sparse) == std::set<int32_t>{0, 2}).to_be_true();
		Result full = play(runOf(kGenre, 1, 25, 50), 1);
		expect(hits(full, Lane::ClosedHat) == std::set<int32_t>{2, 10}).to_be_true();
		expect(bassHits(full) == std::set<int32_t>{0, 2, 8, 10}).to_be_true();
	});

	it("never removes an anchor", _ {
		static constexpr Pattern kAnchors[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
		static constexpr LaneSpec kAnchoredHat[] = {
		    {.lane = Lane::Kick, .pool = kFour},
		    {.lane = Lane::ClosedHat, .pool = kAnchors, .rules = HatOnKick<100, 0, true>::rules},
		};
		static constexpr PartSpec kParts[] = {{.role = Role::Drums, .lanes = kAnchoredHat}};
		static constexpr Genre kGenre = streamGenre(kParts);
		expect(hits(play(runOf(kGenre), 1), Lane::ClosedHat).size()).to_equal(size_t{16});
	});

	it("moves its strength with the Recipe's Complexity by its sensitivity", _ {
		// The genre's default Complexity is 25. Strength 0 with sensitivity +100 is 0 at 25 and below, 50 at 50.
		for (int32_t complexity : {0, 25}) {
			expect(hits(play(runOf(HatOnKick<0, 100, true>::genre, 1, complexity), 1), Lane::ClosedHat).size())
			    .to_equal(size_t{16});
		}
		int32_t rising = 0;
		int32_t falling = 0;
		for (uint32_t seed = 1; seed <= 20; seed++) {
			auto up = hits(play(runOf(HatOnKick<0, 100, true>::genre, seed, 50), 1), Lane::ClosedHat);
			rising += static_cast<int32_t>(up.size()) - 4;
			// Strength 100 with sensitivity -100 is 100 at 25 and below, 50 at 50.
			expect(hits(play(runOf(HatOnKick<100, -100, true>::genre, seed, 0), 1), Lane::ClosedHat).size())
			    .to_equal(size_t{4});
			auto down = hits(play(runOf(HatOnKick<100, -100, true>::genre, seed, 50), 1), Lane::ClosedHat);
			falling += static_cast<int32_t>(down.size()) - 4;
		}
		expect(rising).to_be_between(20 * 12 * 35 / 100, 20 * 12 * 65 / 100);
		expect(falling).to_be_between(20 * 12 * 35 / 100, 20 * 12 * 65 / 100);
	});
});

CPPSPEC_SPEC(interplay)
