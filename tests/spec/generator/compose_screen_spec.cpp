// The Compose screen's text (spec §14.4): its title, the SCALE, LENGTH and SOUNDS rows' values.

#include "generative/compose/compose_screen.h"
#include "generative/core/genre.h"
#include "generative/core/lane_names.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <string>

using namespace deluge::generative;
using namespace deluge::generative::compose;

namespace {

std::string title(const Recipe& recipe) {
	char out[kTitleChars + 1];
	composeTitle(recipe, out);
	return out;
}

std::string length(int32_t seconds) {
	char out[kLengthChars + 1];
	lengthText(seconds, out);
	return out;
}

std::string pick(const char* path) {
	SoundPath sound;
	sound.set(path);
	char out[kPickChars + 1];
	pickName(sound, out);
	return out;
}

std::string kitRow(const PartSpec& part) {
	char out[kRowChars + 1];
	kitRowName(part, out);
	return out;
}

std::string parts(int32_t count) {
	char out[kRowChars + 1];
	partsText(count, out);
	return out;
}

const Genre& genreNamed(const char* name) {
	const Genre* genre = findGenre(name);
	return *genre;
}

} // namespace

// clang-format off
describe compose_screen("The Compose screen's text", $ {
	it("titles the screen with genre, tempo, key and scale (spec §14.4)", _ {
		Recipe recipe;
		recipe.genre = &genreNamed("psytrance-classic");
		recipe.tempo = 14600;
		recipe.keyRoot = 10;
		recipe.scale = Scale::HungarianMinor;
		expect(title(recipe)).to_equal(std::string("CLASSIC PSY 146 A# HUNGARIAN MINOR"));
	});

	it("shows the tempo in whole BPM, rounded", _ {
		Recipe recipe;
		recipe.genre = &genreNamed("techno-acid");
		recipe.tempo = 13250;
		recipe.keyRoot = 0;
		recipe.scale = Scale::Minor;
		expect(title(recipe)).to_equal(std::string("ACID TECHNO 133 C MINOR"));
		expect(tempoBpm(13249)).to_equal(132);
	});

	it("gives SCALE its full name when it fits, else Perform's short one (HARM MINOR, HUNG MINOR)", _ {
		expect(std::string(scaleRowName(Scale::HarmonicMinor, 14))).to_equal(std::string("HARMONIC MINOR"));
		expect(std::string(scaleRowName(Scale::HarmonicMinor, 13))).to_equal(std::string("HARM MINOR"));
		expect(std::string(scaleRowName(Scale::HungarianMinor, 14))).to_equal(std::string("HUNG MINOR"));
		expect(std::string(scaleRowName(Scale::MelodicMinor, 12))).to_equal(std::string("MEL MINOR"));
		expect(std::string(scaleRowName(Scale::Mixolydian, 10))).to_equal(std::string("MIXOLYDIAN"));
		for (int32_t s = 0; s < kNumScales; s++) {
			expect(std::string(scaleRowName(static_cast<Scale>(s), 0)).size()).to_be_less_than(size_t{11});
		}
	});

	it("shows LENGTH as m:ss", _ {
		expect(length(60)).to_equal(std::string("1:00"));
		expect(length(240)).to_equal(std::string("4:00"));
		expect(length(750)).to_equal(std::string("12:30"));
		expect(length(1200)).to_equal(std::string("20:00"));
	});

	it("names a pick by its file, without folder or extension", _ {
		expect(pick("SYNTHS/GENERATIVE/BASS/017 ACID LINE.XML")).to_equal(std::string("017 ACID LINE"));
		expect(pick("SAMPLES/DRUMS/Kick/909 Kick.wav")).to_equal(std::string("909 Kick"));
		expect(pick("SYNTHS/NO EXTENSION")).to_equal(std::string("NO EXTENSION"));
		expect(pick("")).to_equal(std::string("NONE"));
	});

	it("labels a Kit part's row with its lane count, and SOUNDS with the parts (spec §14.4)", _ {
		const Genre& acid = genreNamed("techno-acid");
		bool sawKit = false;
		for (const PartSpec& part : acid.parts) {
			if (!part.synth()) {
				sawKit = true;
				expect(kitRow(part)).to_equal(std::string(part.name) + ": " + std::to_string(part.lanes.size())
				                              + " LANES");
			}
		}
		expect(sawKit).to_be_true();
		expect(parts(5)).to_equal(std::string("5 PARTS"));
		expect(parts(1)).to_equal(std::string("1 PART"));
	});

	it("names every lane as its Kit row is named", _ {
		expect(std::string(laneName(Lane::Kick))).to_equal(std::string("KICK"));
		expect(std::string(laneName(Lane::ClosedHat))).to_equal(std::string("CLOSED HAT"));
		expect(std::string(laneName(Lane::Siren))).to_equal(std::string("SIREN"));
	});
});

CPPSPEC_SPEC(compose_screen)
