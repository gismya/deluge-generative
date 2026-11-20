// Acid techno's event stream with and without its Drone (#218). Its own fingerprint is pinned in its golden
// (golden_spec, #121), with every Techno subgenre's.
//
// With the Drone, the drums, the acid line and the Arc play as without it; only the FX part moved up a row, and its
// drift is drawn per part index, so its Control events are drawn afresh. The check holds as long as the Drone plays
// beside the other parts without changing them. (A check that pinned Techno's stream from before the split went with
// Acid's first retune, #120 round 1's soft Break dip.)

#include "stream_fingerprint.h"

#include "cppspec.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace harness;

namespace {

constexpr uint8_t kDronePart = 2;
constexpr uint8_t kOldFxPart = 2; ///< The FX part's index in Techno's three-part roster, before the Drone.

const std::vector<PartSpec> kPartsWithoutDrone = [] {
	std::vector<PartSpec> parts(genres::technoAcid.parts.begin(), genres::technoAcid.parts.end());
	parts.erase(parts.begin() + kDronePart);
	return parts;
}();

/// Acid techno's table with its Drone taken out, and out of the Break keeps: Techno's roster as it was.
const Genre kWithoutDrone = [] {
	Genre genre = genres::technoAcid;
	genre.parts = kPartsWithoutDrone;
	genre.arc.breakKeeps.parts = 0;
	return genre;
}();

bool partEvent(const Event& e) {
	return e.kind == EventKind::NoteOn || e.kind == EventKind::NoteOff || e.kind == EventKind::Control
	       || e.kind == EventKind::Sound;
}

/// The FX part's drift, drawn per part index: left out of both streams compared. It takes every FX Control, gesture
/// ones too, because it can't tell them apart: a Control event carries the control's summed offset, gestures and drift
/// together (spec §11.3), and once the drift differs (the FX part moved from index 2 to 3) every one of them does.
bool fxDrift(const Event& e) {
	return e.kind == EventKind::Control && e.part == kOldFxPart;
}

/// Acid's stream as Techno's roster had it: no Drone, FX back at index 2, and no FX drift.
bool withoutDroneOrFxDrift(Event& e) {
	if (partEvent(e) && e.part == kDronePart) {
		return false;
	}
	if (partEvent(e) && e.part > kDronePart) {
		e.part--;
	}
	return !fxDrift(e);
}

} // namespace

// clang-format off
describe acid_fingerprint("Acid techno's event stream", $ {
	it("plays the same drums, acid line, Arc and FX notes with the Drone in it: 60 pieces, random macros, scales, keys and seeds", _ {
		expect(std::string(genres::technoAcid.parts[kDronePart].name)).to_equal(std::string("DRONE"));
		const StreamFingerprint with = fingerprintOf(genres::technoAcid, 60, {.filter = withoutDroneOrFxDrift});
		const StreamFingerprint without = fingerprintOf(kWithoutDrone, 60, {.filter = [](Event& e) { return !fxDrift(e); }});
		expect(with.lost).to_equal(0U);
		expect(with.events).to_equal(without.events);
		expect(with.hash).to_equal(without.hash);
	});
});

CPPSPEC_SPEC(acid_fingerprint)
