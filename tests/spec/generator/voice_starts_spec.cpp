// Voice starts (spec §12.6): each step sends note-offs first, then note-ons in role priority (Drums, with the Kick
// first, then Bass, Lead, Chords, Pad and FX), and Ornament hits last.

#include "harness.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;

namespace {

constexpr Pattern kOnTheOne[] = {{"X...............", "................", 0}};
constexpr DegreeWeight kRoot[] = {{0, 1}};
constexpr LaneSpec kDrums[] = {
    {.lane = Lane::Kick, .pool = kOnTheOne},
    {.lane = Lane::Snare, .pool = kOnTheOne},
    {.lane = Lane::ClosedHat, .pool = kOnTheOne},
};
constexpr LaneSpec kFx[] = {{.lane = Lane::Riser, .pool = kOnTheOne}};

constexpr PartSpec loop(Role role) {
	return {.role = role, .flavour = Flavour::Loop, .pool = kOnTheOne, .loop = {.degrees = kRoot}};
}

/// Generated in role order, so the Chords part is decided before the Lead.
constexpr PartSpec kParts[] = {
    {.role = Role::Drums, .lanes = kDrums}, loop(Role::Bass), loop(Role::Chords), loop(Role::Pad), loop(Role::Lead),
    {.role = Role::FX, .lanes = kFx},
};
constexpr Scale kScales[] = {Scale::Minor};
constexpr Genre kEveryRole{.name = "voices", .facts = {.scales = kScales}, .parts = kParts};

} // namespace

// clang-format off
describe voice_starts("Voice starts", $ {
	it("start in role priority within a tick: Drums from the Kick, Bass, Lead, Chords, Pad, FX", _ {
		Run run;
		run.recipe = newRecipe(kEveryRole, 0);
		std::vector<Role> roles;
		std::vector<Lane> lanes;
		for (const Event& e : only(play(run, 1).events, EventKind::NoteOn)) {
			if (e.tick == 0) {
				roles.push_back(kParts[e.part].role);
				lanes.push_back(e.lane);
			}
		}
		expect(roles == std::vector<Role>{Role::Drums, Role::Drums, Role::Drums, Role::Bass, Role::Lead, Role::Chords,
		                                  Role::Pad, Role::FX}).to_be_true();
		expect(lanes[0]).to_equal(Lane::Kick);
		expect(lanes[1]).to_equal(Lane::Snare);
	});

	it("start Ornament hits last within a tick", _ {
		// The Kick's second ratchet hit and the hat, leaning 12 late, share tick 12 of each beat.
		static constexpr LaneSpec kLanes[] = {
		    {.lane = Lane::Kick,
		     .pool = kOnTheOne,
		     .expression = {.ornaments = {.rateFloor = 1000, .rateCeiling = 1000, .ratchet = {1, 0, 0}}}},
		    {.lane = Lane::ClosedHat, .pool = kOnTheOne, .expression = {.lean = {12, 0, 0, 0}}},
		};
		static constexpr PartSpec kDrumsOnly[] = {{.role = Role::Drums, .lanes = kLanes}};
		static constexpr Genre kGenre{.name = "voices", .facts = {.scales = kScales}, .parts = kDrumsOnly};
		Run run;
		run.recipe = newRecipe(kGenre, 0);
		std::vector<Event> atTwelve;
		for (const Event& e : only(play(run, 1).events, EventKind::NoteOn)) {
			if (e.tick == 12) {
				atTwelve.push_back(e);
			}
		}
		expect(atTwelve.size()).to_equal(size_t{2});
		atTwelve.resize(2);
		expect(atTwelve[0].lane).to_equal(Lane::ClosedHat);
		expect(atTwelve[1].lane).to_equal(Lane::Kick);
		expect(atTwelve[1].ornament).to_be_true();
	});
});

CPPSPEC_SPEC(voice_starts)
