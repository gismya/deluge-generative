// Loading a part's or lane's sound, falling back through the palette (spec §10.1, §10.2, §10.5, §10.7).

#include "fake_card.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"

#include "cppspec.hpp"

#include <string>
#include <vector>

using namespace deluge::generative;
using harness::FakeCard;
using harness::FakeLoader;
using Paths = std::vector<std::string>;

namespace {

constexpr PaletteEntry kAcid[] = {{"SYNTHS/A.XML", 3}, {"SYNTHS/B.XML", 1}, {"SYNTHS/C.XML", 1}};
constexpr PartSounds kPartSounds[] = {{"ACID", kAcid}};
constexpr TagWeight kKickTags[] = {{"909", 1}};
constexpr PaletteEntry kRiserSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Riser 1.XML"},
                                         {"SYNTHS/GENERATIVE/FX/GEN Riser 2.XML"}};
constexpr LaneSounds kLaneSounds[] = {
    {.lane = Lane::Kick, .tags = kKickTags},
    {.lane = Lane::Riser, .synths = kRiserSynths},
};
constexpr Palette kPalette{.parts = kPartSounds, .lanes = kLaneSounds};
constexpr PartSpec kAcidPart{.role = Role::Bass, .flavour = Flavour::Loop, .name = "ACID"};

FakeCard card() {
	FakeCard card;
	card.add("SAMPLES/DRUMS/Kick", {"808 Kick.wav", "909 Kick.wav", "909 Kick dry.wav", "909 Kick distorted.wav"});
	card.add("SAMPLES/FX/Riser", {"GEN Riser 1.wav"});
	return card;
}

} // namespace

// clang-format off
describe sound_loading("Loading sounds with fallback", $ {
	it("loads a part's pick", _ {
		FakeLoader loader;
		SoundLoad load = loadPartSound(kPalette, kAcidPart, "SYNTHS/B.XML", loader);
		expect(load.source == SoundSource::Pick).to_be_true();
		expect(load.warns()).to_be_false();
		expect(loader.tried).to_equal(Paths{"SYNTHS/B.XML"});
	});

	it("falls through the palette in order when the pick doesn't load", _ {
		FakeLoader loader;
		loader.missing = {"SYNTHS/B.XML", "SYNTHS/A.XML"};
		SoundLoad load = loadPartSound(kPalette, kAcidPart, "SYNTHS/B.XML", loader);
		expect(load.source == SoundSource::Fallback).to_be_true();
		expect(load.warns()).to_be_false();
		expect(loader.tried).to_equal(Paths{"SYNTHS/B.XML", "SYNTHS/A.XML", "SYNTHS/C.XML"});
	});

	it("falls back to the default synth with one warning when nothing loads", _ {
		FakeLoader loader;
		loader.missing = {"SYNTHS/MINE.XML", "SYNTHS/A.XML", "SYNTHS/B.XML", "SYNTHS/C.XML"};
		SoundLoad load = loadPartSound(kPalette, kAcidPart, "SYNTHS/MINE.XML", loader);
		expect(load.source == SoundSource::DefaultSynth).to_be_true();
		expect(load.warns()).to_be_true();
		expect(loader.tried.size()).to_equal(size_t{4});

		FakeLoader empty;
		expect(loadPartSound(kPalette, kAcidPart, "", empty).source == SoundSource::Fallback).to_be_true();
		expect(empty.tried).to_equal(Paths{"SYNTHS/A.XML"});
	});

	it("stops on a card error instead of falling back", _ {
		FakeLoader loader;
		loader.cardErrors = {"SYNTHS/B.XML"};
		expect(loadPartSound(kPalette, kAcidPart, "SYNTHS/B.XML", loader).source == SoundSource::CardError).to_be_true();
		expect(loader.tried.size()).to_equal(size_t{1});

		FakeCard pulled = card();
		pulled.cardError = true;
		FakeLoader laneLoader;
		laneLoader.missing = {"SAMPLES/DRUMS/Kick/909 Kick.wav"};
		expect(loadLaneSound(kPalette, Lane::Kick, "SAMPLES/DRUMS/Kick/909 Kick.wav", pulled, laneLoader).source
		       == SoundSource::CardError).to_be_true();
	});

	it("falls back to the lane's other matching files in card order, then leaves it silent with a warning", _ {
		FakeCard kicks = card();
		FakeLoader loader;
		loader.missing = {"SAMPLES/DRUMS/Kick/909 Kick dry.wav", "SAMPLES/DRUMS/Kick/909 Kick.wav"};
		SoundLoad load = loadLaneSound(kPalette, Lane::Kick, "SAMPLES/DRUMS/Kick/909 Kick dry.wav", kicks, loader);
		expect(load.source == SoundSource::Fallback).to_be_true();
		expect(loader.tried).to_equal(Paths{"SAMPLES/DRUMS/Kick/909 Kick dry.wav", "SAMPLES/DRUMS/Kick/909 Kick.wav",
		                                    "SAMPLES/DRUMS/Kick/909 Kick distorted.wav"});

		FakeLoader none;
		none.missing = {"SAMPLES/DRUMS/Kick/909 Kick dry.wav", "SAMPLES/DRUMS/Kick/909 Kick.wav",
		                "SAMPLES/DRUMS/Kick/909 Kick distorted.wav"};
		SoundLoad silent = loadLaneSound(kPalette, Lane::Kick, "SAMPLES/DRUMS/Kick/909 Kick dry.wav", kicks, none);
		expect(silent.source == SoundSource::Silent).to_be_true(); // no synth fallback for drums
		expect(silent.warns()).to_be_true();
		expect(none.tried.size()).to_equal(size_t{3});
	});

	it("loads an FX lane's synth presets when it has no sample", _ {
		FakeCard noFx;
		FakeLoader loader;
		loader.missing = {"SYNTHS/GENERATIVE/FX/GEN Riser 1.XML"};
		SoundLoad load = loadLaneSound(kPalette, Lane::Riser, "", noFx, loader);
		expect(load.source == SoundSource::Fallback).to_be_true();
		expect(load.synth).to_be_true();
		expect(loader.tried).to_equal(Paths{"SYNTHS/GENERATIVE/FX/GEN Riser 1.XML", "SYNTHS/GENERATIVE/FX/GEN Riser 2.XML"});

		FakeLoader sample;
		FakeCard withFx = card();
		SoundLoad picked = loadLaneSound(kPalette, Lane::Riser, "SAMPLES/FX/Riser/GEN Riser 1.wav", withFx, sample);
		expect(picked.source == SoundSource::Pick).to_be_true();
		expect(picked.synth).to_be_false();
	});

	it("leaves a lane with no palette entry and no pick silent with a warning", _ {
		FakeCard empty;
		FakeLoader loader;
		SoundLoad load = loadLaneSound(kPalette, Lane::Clap, "", empty, loader);
		expect(load.source == SoundSource::Silent).to_be_true();
		expect(loader.tried.empty()).to_be_true();
	});
});

CPPSPEC_SPEC(sound_loading)
