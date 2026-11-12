// Replace pairs (spec §5.7): "A replaces B" drops B's hit when both land on the same step, applied in the Generator.
// The Generator-wide default is Open hat replaces Closed hat.

#include "harness.h"

#include "cppspec.hpp"

#include <set>

using namespace harness;

namespace {

constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr Pattern kOnTheAnds[] = {{"..X...X...X...X.", "................", 0}};
constexpr Pattern kBusy[] = {{"9999999999999999", "................", 0}};
constexpr InterplayRule kLocksClosedHat[] = {{.kind = RuleKind::Lock, .target = {0, Lane::ClosedHat}}};
constexpr Pattern kOnTwo[] = {{"..X.............", "................", 0}};

constexpr LaneSpec kHats[] = {
    {.lane = Lane::ClosedHat, .pool = kOnTwo},
    {.lane = Lane::OpenHat, .pool = kOnTheAnds},
    // Reads the closed hat as planned, before the open hat replaces it.
    {.lane = Lane::Perc1, .pool = kBusy, .rules = kLocksClosedHat},
};
constexpr LaneSpec kHatsEveryStep[] = {
    {.lane = Lane::ClosedHat, .pool = kEveryStep},
    {.lane = Lane::OpenHat, .pool = kOnTheAnds},
};
constexpr PartSpec kParts[] = {{.role = Role::Drums, .lanes = kHatsEveryStep}};
constexpr PartSpec kLockParts[] = {{.role = Role::Drums, .lanes = kHats}};
constexpr Scale kScales[] = {Scale::Minor};

constexpr Genre kDefault{.name = "replace", .facts = {.scales = kScales}, .parts = kParts};
constexpr ReplacePair kClosedReplacesOpen[] = {{Lane::ClosedHat, Lane::OpenHat}};
constexpr Genre kReversed{
    .name = "replace", .facts = {.scales = kScales}, .parts = kParts, .replacePairs = kClosedReplacesOpen};
constexpr Genre kNone{.name = "replace", .facts = {.scales = kScales}, .parts = kParts, .replacePairs = {}};
constexpr Genre kLocking{.name = "replace", .facts = {.scales = kScales}, .parts = kLockParts};

std::set<int32_t> hits(const Genre& genre, Lane lane) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Density, kMacroFull);
	run.skipIntro = true; // full Density from the first step
	std::set<int32_t> out;
	for (const Event& e : only(play(run, 1).events, EventKind::NoteOn)) {
		if (e.lane == lane && e.tick < kBarTicks) {
			out.insert(e.tick / kTicksPerStep);
		}
	}
	return out;
}

} // namespace

// clang-format off
describe replace_pairs("Replace pairs", $ {
	it("drop the closed hat where the open hat lands on the same step, by default", _ {
		expect(hits(kDefault, Lane::OpenHat) == std::set<int32_t>{2, 6, 10, 14}).to_be_true();
		expect(hits(kDefault, Lane::ClosedHat)
		       == std::set<int32_t>{0, 1, 3, 4, 5, 7, 8, 9, 11, 12, 13, 15}).to_be_true();
	});

	it("follow the genre's list, one way", _ {
		expect(hits(kReversed, Lane::OpenHat).empty()).to_be_true();
		expect(hits(kReversed, Lane::ClosedHat).size()).to_equal(size_t{16});
		expect(hits(kNone, Lane::OpenHat).size()).to_equal(size_t{4});
		expect(hits(kNone, Lane::ClosedHat).size()).to_equal(size_t{16});
	});

	it("come after interplay rules, which read the replaced hit as planned", _ {
		expect(hits(kLocking, Lane::ClosedHat).empty()).to_be_true();
		expect(hits(kLocking, Lane::Perc1) == std::set<int32_t>{2}).to_be_true();
	});
});

CPPSPEC_SPEC(replace_pairs)
