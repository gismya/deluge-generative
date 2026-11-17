// Acid techno's event stream against Techno's from before the split (#226, #218). Its own fingerprint is pinned in its
// golden (golden_spec, #121), with every Techno subgenre's.
//
// Acid techno is today's Techno under its new name, with a Drone added (#218). Without its Drone, its table plays the
// stream Techno played before the split, hash for hash: the split moved fields between tables and changed nothing.
// With the Drone, the drums, the acid line and the Arc stay as they were; only the FX part moved up a row, and its
// drift is drawn per part index, so its Control events are drawn afresh.
//
// The first check pins a stream from before the split. A retune of Acid's drums, acid line, Arc or FX breaks it: the
// split is then history, so delete that check in the retune's commit. The second holds as long as the Drone plays
// beside the other parts without changing them.

#include "stream_fingerprint.h"

#include "cppspec.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace harness;

namespace {

/// Techno's stream before the split (Drums, Acid, FX), measured on 586687df0 (the base of #226).
constexpr uint64_t kTechnoStream = 0x6c96b6b2967c7984ULL;
constexpr uint64_t kTechnoEvents = 1342372;

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
	it("is Techno's from before the split, without the Drone: 60 pieces, random macros, scales, keys and seeds", _ {
		expect(std::string(genres::technoAcid.parts[kDronePart].name)).to_equal(std::string("DRONE"));
		const StreamFingerprint stream = fingerprintOf(kWithoutDrone, 60);
		std::printf("acid stream without the Drone %016llx, %llu events\n", static_cast<unsigned long long>(stream.hash),
		            static_cast<unsigned long long>(stream.events));
		expect(stream.lost).to_equal(0U);
		expect(stream.events).to_equal(kTechnoEvents);
		expect(stream.hash).to_equal(kTechnoStream);
	});

	it("plays the same drums, acid line, Arc and FX notes with the Drone in it", _ {
		const StreamFingerprint with = fingerprintOf(genres::technoAcid, 60, {.filter = withoutDroneOrFxDrift});
		const StreamFingerprint without = fingerprintOf(kWithoutDrone, 60, {.filter = [](Event& e) { return !fxDrift(e); }});
		expect(with.lost).to_equal(0U);
		expect(with.events).to_equal(without.events);
		expect(with.hash).to_equal(without.hash);
	});
});

CPPSPEC_SPEC(acid_fingerprint)
