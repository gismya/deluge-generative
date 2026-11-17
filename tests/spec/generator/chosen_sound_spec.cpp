// A sound pick the player chose in Perform's preset browser (spec §10.1, §10.7, §13.5): SYNTH on a synth part opens
// it, and the confirmed preset becomes the part's pick, whether or not its palette lists it. It then loads as a rolled
// pick does (sound_loading_spec), through the same preload and swap.

#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <cstring>
#include <string>

using namespace deluge::generative;

namespace {

const Genre& acid() {
	return genres::technoAcid;
}

/// Acid techno's roster: DRUMS, ACID, DRONE, FX.
constexpr int32_t kDrums = 0;
constexpr int32_t kAcid = 1;
constexpr int32_t kDrone = 2;
constexpr int32_t kFx = 3;

/// A factory preset Acid techno's palette doesn't list for any part.
constexpr const char* kUnlisted = "SYNTHS/002 Basic Square Bass.XML";

/// Picks in every part and lane, so a spec sees any of them move.
SoundPicks filledPicks() {
	SoundPicks picks;
	for (int32_t p = 0; p < kMaxParts; p++) {
		picks.part[p].set("SYNTHS", ("P" + std::to_string(p) + ".XML").c_str());
	}
	for (int32_t l = 0; l < kNumLanes; l++) {
		picks.lane[l].set("SAMPLES", ("L" + std::to_string(l) + ".wav").c_str());
	}
	return picks;
}

bool samePicks(const SoundPicks& a, const SoundPicks& b) {
	for (int32_t p = 0; p < kMaxParts; p++) {
		if (std::strcmp(a.part[p].c_str(), b.part[p].c_str()) != 0) {
			return false;
		}
	}
	for (int32_t l = 0; l < kNumLanes; l++) {
		if (std::strcmp(a.lane[l].c_str(), b.lane[l].c_str()) != 0) {
			return false;
		}
	}
	return true;
}

/// `text` as a pick holds it.
SoundPath pathOf(const std::string& text) {
	SoundPath path;
	path.set(text.c_str());
	return path;
}

} // namespace

// clang-format off
describe chosen_sound("A chosen sound pick", $ {
	it("tells synth parts from Kit parts and parts the genre doesn't have: only a synth part has a preset to choose", _ {
		expect(acid().parts[kDrums].synth()).to_be_false();
		expect(acid().parts[kAcid].synth()).to_be_true();
		expect(synthPart(acid(), kDrums)).to_be_false();
		expect(synthPart(acid(), kAcid)).to_be_true();
		expect(synthPart(acid(), kDrone)).to_be_true();
		expect(synthPart(acid(), kFx)).to_be_false();
		expect(synthPart(acid(), -1)).to_be_false();
		expect(synthPart(acid(), static_cast<int32_t>(acid().parts.size()))).to_be_false();
		expect(synthPart(acid(), kMaxParts)).to_be_false();
	});

	it("makes a preset the palette doesn't list a synth part's pick, every other pick kept", _ {
		const SoundPicks before = filledPicks();
		SoundPicks picks = before;
		expect(choosePartSound(acid(), kAcid, pathOf(kUnlisted), picks)).to_be_true();
		expect(std::string(picks.part[kAcid].c_str())).to_equal(std::string(kUnlisted));
		SoundPicks expected = before;
		expected.part[kAcid].set(kUnlisted);
		expect(samePicks(picks, expected)).to_be_true();
	});

	it("takes a preset the palette lists for another part too", _ {
		SoundPicks picks = filledPicks();
		expect(choosePartSound(acid(), kDrone, pathOf("SYNTHS/165 Acid Arp.XML"), picks)).to_be_true();
		expect(std::string(picks.part[kDrone].c_str())).to_equal(std::string("SYNTHS/165 Acid Arp.XML"));
	});

	it("refuses a Kit part, a part the genre doesn't have and an empty path, changing nothing", _ {
		const SoundPicks before = filledPicks();
		SoundPicks picks = before;
		expect(choosePartSound(acid(), kDrums, pathOf(kUnlisted), picks)).to_be_false();
		expect(choosePartSound(acid(), kFx, pathOf(kUnlisted), picks)).to_be_false();
		expect(choosePartSound(acid(), -1, pathOf(kUnlisted), picks)).to_be_false();
		expect(choosePartSound(acid(), static_cast<int32_t>(acid().parts.size()), pathOf(kUnlisted), picks))
		    .to_be_false();
		expect(choosePartSound(acid(), kAcid, SoundPath{}, picks)).to_be_false();
		expect(samePicks(picks, before)).to_be_true();
	});

	it("holds a path of up to 127 characters, and none longer: the browser refuses those before choosing", _ {
		SoundPath path;
		const std::string longest = "SYNTHS/" + std::string(kMaxSoundPath - 8, 'A');
		expect(path.set(longest.c_str())).to_be_true();
		SoundPicks picks = filledPicks();
		expect(choosePartSound(acid(), kAcid, path, picks)).to_be_true();
		expect(std::string(picks.part[kAcid].c_str())).to_equal(longest);
		const std::string tooLong = longest + "A";
		expect(static_cast<int32_t>(tooLong.size())).to_equal(kMaxSoundPath);
		expect(path.set(tooLong.c_str())).to_be_false();
		expect(path.empty()).to_be_true();
	});
});

CPPSPEC_SPEC(chosen_sound)
