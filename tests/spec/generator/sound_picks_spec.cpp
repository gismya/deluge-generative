// Rolling a Recipe's sound picks from its genre's palette (spec §10.1–§10.3, §10.5).

#include "fake_card.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <map>
#include <string>

using namespace deluge::generative;
using harness::FakeCard;

namespace {

constexpr PaletteEntry kAcid[] = {{"SYNTHS/A.XML", 3}, {"SYNTHS/B.XML", 1}};
constexpr PaletteEntry kBassDefault[] = {{"SYNTHS/C.XML", 1}, {"SYNTHS/D.XML", 1}, {"SYNTHS/E.XML", 1}};
constexpr PartSounds kPartSounds[] = {{"ACID", kAcid}};
constexpr RoleSounds kRoleSounds[] = {{Role::Bass, kBassDefault}};
constexpr Palette kSynthPalette{.parts = kPartSounds, .roles = kRoleSounds};
constexpr PartSpec kSynthParts[] = {
    {.role = Role::Bass, .flavour = Flavour::Loop, .name = "ACID"},
    {.role = Role::Bass, .flavour = Flavour::Loop, .name = "SUB"},
};
constexpr Genre kSynthGenre{.name = "synths", .parts = kSynthParts, .palette = &kSynthPalette};

constexpr TagWeight kKickTags[] = {{"909 kick", 3}, {"deep sky", 1}};
constexpr TagWeight kRimTags[] = {{"SAMPLES/DRUMS/Rim/808 Rim.wav", 1}};
constexpr LaneSounds kDrumSounds[] = {
    {.lane = Lane::Kick, .tags = kKickTags},
    {.lane = Lane::Rim, .tags = kRimTags},
    {.lane = Lane::Tom},
};
constexpr Palette kDrumPalette{.lanes = kDrumSounds};
constexpr LaneSpec kDrumLanes[] = {{.lane = Lane::Kick}, {.lane = Lane::Rim}, {.lane = Lane::Tom}};
constexpr PartSpec kDrumParts[] = {{.role = Role::Drums, .name = "DRUMS", .lanes = kDrumLanes}};
constexpr Genre kDrumGenre{.name = "drums", .parts = kDrumParts, .palette = &kDrumPalette};

constexpr TagWeight kClosedHatTags[] = {{"909", 3}, {"808", 1}, {"DDD1", 1}};
constexpr TagWeight kOpenHatTags[] = {{"909", 1}, {"808", 1}};
constexpr LaneSounds kHatSounds[] = {
    {.lane = Lane::ClosedHat, .tags = kClosedHatTags},
    {.lane = Lane::OpenHat, .tags = kOpenHatTags},
};
constexpr Palette kHatPalette{.lanes = kHatSounds};
constexpr LaneSpec kHatLanes[] = {{.lane = Lane::ClosedHat}, {.lane = Lane::OpenHat}};
constexpr PartSpec kHatParts[] = {{.role = Role::Drums, .name = "DRUMS", .lanes = kHatLanes}};
constexpr Genre kHatGenre{.name = "hats", .parts = kHatParts, .palette = &kHatPalette};

FakeCard drumCard() {
	FakeCard card;
	card.add("SAMPLES/DRUMS/Kick", {"909 Kick.wav", "909 KICK dry.wav", "909 Kick distorted.txt", "808 Kick.wav",
	                                "Deep Sky Kick HQ.wav", ".909 Kick hidden.wav"});
	card.add("SAMPLES/DRUMS/Rim", {"808 Rim long.wav", "808 Rim.wav"});
	card.add("SAMPLES/DRUMS/TomL", {"808 Tom low.wav", "808 Tom low.wav.reapeaks"});
	card.add("SAMPLES/DRUMS/TomM", {"808 Tom medium.aif"});
	card.add("SAMPLES/DRUMS/TomH", {"909 Tom high.AIFF"});
	return card;
}

/// The sound picks rolled for `genre` from its built-in palette. `ok` is false if rolling hit a card error.
struct Rolled {
	SoundPicks sounds;
};
Rolled rolled(const Genre& genre, uint32_t entropy, FakeCard& card, bool* ok = nullptr) {
	Rolled result;
	bool rolledOk = rollSounds(genre, *genre.palette, result.sounds, entropy, card);
	if (ok != nullptr) {
		*ok = rolledOk;
	}
	return result;
}

} // namespace

// clang-format off
describe sound_picks("Rolling sound picks", $ {
	it("rolls a part's preset by weight from its own entry, else from its role's", _ {
		FakeCard card;
		std::map<std::string, int> acid, sub;
		for (uint32_t entropy = 0; entropy < 400; entropy++) {
			Rolled recipe = rolled(kSynthGenre, entropy, card);
			acid[recipe.sounds.part[0].c_str()]++;
			sub[recipe.sounds.part[1].c_str()]++;
		}
		expect(acid.size()).to_equal(size_t{2});
		expect(acid["SYNTHS/A.XML"]).to_be_between(260, 340); // weight 3 of 4
		expect(sub.size()).to_equal(size_t{3});
		expect(sub["SYNTHS/C.XML"]).to_be_between(100, 170);
	});

	it("rolls a lane's tag by weight, then one matching audio file at random", _ {
		FakeCard card = drumCard();
		std::map<std::string, int> kicks;
		for (uint32_t entropy = 0; entropy < 600; entropy++) {
			kicks[rolled(kDrumGenre, entropy, card).sounds.lane[static_cast<int32_t>(Lane::Kick)].c_str()]++;
		}
		// "909 kick" (weight 3) matches two audio files whatever their case, so 3/8 each; "deep sky" (weight 1)
		// matches one, so 1/4. The text file, the dotfile and the 808 never play.
		expect(kicks.size()).to_equal(size_t{3});
		expect(kicks["SAMPLES/DRUMS/Kick/909 Kick.wav"]).to_be_between(180, 270);
		expect(kicks["SAMPLES/DRUMS/Kick/909 KICK dry.wav"]).to_be_between(180, 270);
		expect(kicks["SAMPLES/DRUMS/Kick/Deep Sky Kick HQ.wav"]).to_be_between(110, 190);
	});

	it("picks any audio file in the lane's default folders when it lists no tags", _ {
		FakeCard card = drumCard();
		std::map<std::string, int> toms;
		for (uint32_t entropy = 0; entropy < 300; entropy++) {
			toms[rolled(kDrumGenre, entropy, card).sounds.lane[static_cast<int32_t>(Lane::Tom)].c_str()]++;
		}
		// Tom's default folders are TomL, TomM and TomH (spec §10.2); dotfiles and .reapeaks files never play.
		expect(toms.size()).to_equal(size_t{3});
		expect(toms.contains("SAMPLES/DRUMS/TomL/808 Tom low.wav")).to_be_true();
		expect(toms.contains("SAMPLES/DRUMS/TomM/808 Tom medium.aif")).to_be_true();
		expect(toms.contains("SAMPLES/DRUMS/TomH/909 Tom high.AIFF")).to_be_true();
	});

	it("picks the named file for an exact-path tag, and only if it is on the card", _ {
		FakeCard card = drumCard();
		for (uint32_t entropy = 0; entropy < 50; entropy++) {
			expect(std::string(rolled(kDrumGenre, entropy, card).sounds.lane[static_cast<int32_t>(Lane::Rim)].c_str()))
			    .to_equal(std::string("SAMPLES/DRUMS/Rim/808 Rim.wav"));
		}
		card.folders.erase("samples/drums/rim");
		card.add("SAMPLES/DRUMS/Rim", {"808 Rim long.wav"});
		expect(rolled(kDrumGenre, 0, card).sounds.lane[static_cast<int32_t>(Lane::Rim)].empty()).to_be_true();
	});

	it("rolls the Open hat from the Closed hat's tag when it can, else from its own weights", _ {
		FakeCard card;
		card.add("SAMPLES/DRUMS/HatC", {"909 Closed hihat.wav", "808 Closed hihat.wav", "DDD1 Closed hihat.wav"});
		card.add("SAMPLES/DRUMS/HatO", {"909 Open hihat.wav", "808 Open hihat.wav"});
		std::map<std::string, int> afterDdd1;
		for (uint32_t entropy = 0; entropy < 400; entropy++) {
			Rolled recipe = rolled(kHatGenre, entropy, card);
			std::string closed = recipe.sounds.lane[static_cast<int32_t>(Lane::ClosedHat)].c_str();
			std::string open = recipe.sounds.lane[static_cast<int32_t>(Lane::OpenHat)].c_str();
			if (closed.find("DDD1") != std::string::npos) {
				afterDdd1[open]++; // not in the Open hat's list: its own weights
			}
			else {
				expect(open.substr(open.rfind('/') + 1, 3)).to_equal(closed.substr(closed.rfind('/') + 1, 3));
			}
		}
		expect(afterDdd1.size()).to_equal(size_t{2});

		// The Closed hat's tag matches no Open hat file: its own weights again.
		card.folders.erase("samples/drums/hato");
		card.add("SAMPLES/DRUMS/HatO", {"909 Open hihat.wav"});
		for (uint32_t entropy = 0; entropy < 100; entropy++) {
			expect(std::string(rolled(kHatGenre, entropy, card).sounds.lane[static_cast<int32_t>(Lane::OpenHat)].c_str()))
			    .to_equal(std::string("SAMPLES/DRUMS/HatO/909 Open hihat.wav"));
		}
	});

	it("leaves a lane empty when nothing on the card matches, and reports a card error", _ {
		FakeCard card;
		Rolled recipe = rolled(kDrumGenre, 7, card);
		expect(recipe.sounds.lane[static_cast<int32_t>(Lane::Kick)].empty()).to_be_true();
		card.cardError = true;
		bool ok = true;
		rolled(kDrumGenre, 7, card, &ok);
		expect(ok).to_be_false();
	});

	it("re-rolls one part's picks, leaving every other part's as they were", _ {
		FakeCard card;
		SoundPicks sounds = rolled(kSynthGenre, 1, card).sounds;
		const std::string sub = sounds.part[1].c_str();
		for (uint32_t entropy = 2; entropy < 40; entropy++) {
			SoundPicks rerolled = sounds;
			expect(rollPartSounds(kSynthGenre, *kSynthGenre.palette, 0, rerolled, entropy, card)).to_be_true();
			expect(std::string(rerolled.part[1].c_str())).to_equal(sub);
		}
	});

	it("re-rolls a part away from the sound it has, where its palette offers another", _ {
		FakeCard card;
		std::map<std::string, int> subs;
		for (uint32_t entropy = 0; entropy < 200; entropy++) {
			SoundPicks sounds;
			sounds.part[1].set("SYNTHS/C.XML");
			expect(rollPartSounds(kSynthGenre, *kSynthGenre.palette, 1, sounds, entropy, card)).to_be_true();
			subs[sounds.part[1].c_str()]++;
		}
		expect(subs.contains("SYNTHS/C.XML")).to_be_false();
		expect(subs.size()).to_equal(size_t{2});
	});

	it("re-rolls every lane of a Kit part away from its file, where its tags allow another", _ {
		FakeCard card = drumCard();
		for (uint32_t entropy = 0; entropy < 100; entropy++) {
			SoundPicks sounds = rolled(kDrumGenre, entropy, card).sounds;
			const std::string kick = sounds.lane[static_cast<int32_t>(Lane::Kick)].c_str();
			expect(rollPartSounds(kDrumGenre, *kDrumGenre.palette, 0, sounds, entropy + 1000, card)).to_be_true();
			expect(std::string(sounds.lane[static_cast<int32_t>(Lane::Kick)].c_str())).not_().to_equal(kick);
			// The Rim's tag names one file: it stays.
			expect(std::string(sounds.lane[static_cast<int32_t>(Lane::Rim)].c_str()))
			    .to_equal(std::string("SAMPLES/DRUMS/Rim/808 Rim.wav"));
		}
	});

	it("re-rolls every part away from its sound for a Regenerate, where its palette offers another", _ {
		FakeCard card = drumCard();
		for (uint32_t entropy = 0; entropy < 100; entropy++) {
			SoundPicks synths;
			synths.part[0].set("SYNTHS/A.XML");
			synths.part[1].set("SYNTHS/C.XML");
			expect(rerollSounds(kSynthGenre, *kSynthGenre.palette, synths, entropy, card)).to_be_true();
			expect(std::string(synths.part[0].c_str())).to_equal(std::string("SYNTHS/B.XML"));
			expect(std::string(synths.part[1].c_str())).not_().to_equal(std::string("SYNTHS/C.XML"));

			SoundPicks drums = rolled(kDrumGenre, entropy, card).sounds;
			const std::string kick = drums.lane[static_cast<int32_t>(Lane::Kick)].c_str();
			expect(rerollSounds(kDrumGenre, *kDrumGenre.palette, drums, entropy + 1000, card)).to_be_true();
			expect(std::string(drums.lane[static_cast<int32_t>(Lane::Kick)].c_str())).not_().to_equal(kick);
		}
	});

	it("rolls a genre change's picks afresh for the new roster, keeping none of the old roster's (spec §9.7)", _ {
		FakeCard card = drumCard();
		for (uint32_t entropy = 0; entropy < 50; entropy++) {
			SoundPicks sounds;
			sounds.part[0].set("SYNTHS/A.XML");
			sounds.part[1].set("SYNTHS/C.XML");
			expect(rollIncomingSounds(kDrumGenre, kSynthGenre, *kDrumGenre.palette, sounds, entropy, card))
			    .to_be_true();
			const SoundPicks first = rolled(kDrumGenre, entropy, card).sounds;
			for (int32_t p = 0; p < kMaxParts; p++) {
				expect(std::string(sounds.part[p].c_str())).to_equal(std::string(first.part[p].c_str()));
			}
			for (int32_t l = 0; l < kNumLanes; l++) {
				expect(std::string(sounds.lane[l].c_str())).to_equal(std::string(first.lane[l].c_str()));
			}
		}
	});

	it("rolls a Regenerate's picks away from the playing ones (spec §9.7)", _ {
		FakeCard card = drumCard();
		for (uint32_t entropy = 0; entropy < 50; entropy++) {
			SoundPicks sounds;
			sounds.part[0].set("SYNTHS/A.XML");
			sounds.part[1].set("SYNTHS/C.XML");
			expect(rollIncomingSounds(kSynthGenre, kSynthGenre, *kSynthGenre.palette, sounds, entropy, card))
			    .to_be_true();
			expect(std::string(sounds.part[0].c_str())).to_equal(std::string("SYNTHS/B.XML"));
			expect(std::string(sounds.part[1].c_str())).not_().to_equal(std::string("SYNTHS/C.XML"));
		}
	});

	it("reports a card error", _ {
		FakeCard card = drumCard();
		SoundPicks sounds = rolled(kDrumGenre, 3, card).sounds;
		card.cardError = true;
		expect(rollPartSounds(kDrumGenre, *kDrumGenre.palette, 0, sounds, 99, card)).to_be_false();
	});
});

CPPSPEC_SPEC(sound_picks)
