// Palette override files on the card: parsing, and merging over the built-in palette (spec §10.1, §10.2).

#include "fake_card.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/palette_file.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace deluge::generative;
using harness::FakeCard;

namespace {

const Palette& builtIn = genres::technoPalette;

std::string fixture() {
	std::ifstream in(std::filesystem::path(GENERATOR_REPO_DIR)
	                 / "tests/spec/generator/fixtures/palette_override/GENERATIVE/PALETTES/TECHNO.XML");
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

/// Whether the merged palette still uses the built-in entry for `lane`, untouched.
bool laneIsBuiltIn(const Palette& palette, Lane lane) {
	return palette.laneSounds(lane) != nullptr && builtIn.laneSounds(lane) != nullptr
	       && palette.laneSounds(lane)->tags.data() == builtIn.laneSounds(lane)->tags.data();
}

bool unchanged(const PaletteOverride& override) {
	const Palette& p = override.palette();
	return p.parts.data() == builtIn.parts.data() && p.roles.data() == builtIn.roles.data()
	       && p.lanes.data() == builtIn.lanes.data();
}

std::string wrap(const std::string& body) {
	return "<palette>" + body + "</palette>";
}

} // namespace

// clang-format off
describe palette_file("Palette override files", $ {
	it("replaces the parts and lanes it lists and keeps every other built-in entry", _ {
		auto override = std::make_unique<PaletteOverride>();
		expect(override->parse(fixture(), builtIn)).to_be_true();
		const Palette& palette = override->palette();

		PartSpec acid{.role = Role::Bass, .flavour = Flavour::Loop, .name = "acid"}; // names match in any case
		auto entries = palette.partEntries(acid);
		expect(entries.size()).to_equal(size_t{1});
		expect(std::string(entries[0].path)).to_equal(std::string("SYNTHS/047 Basic Dirty Bass.XML"));
		expect(entries[0].weight).to_equal(uint8_t{2});
		PartSpec pad{.role = Role::Pad, .name = "DRONE"};
		expect(palette.partEntries(pad).data() == builtIn.partEntries(pad).data()).to_be_true();

		const LaneSounds* kick = palette.laneSounds(Lane::Kick);
		expect(kick->folders.size()).to_equal(size_t{1});
		expect(std::string(kick->folders[0])).to_equal(std::string("SAMPLES/DRUMS/MyKicks"));
		expect(kick->tags.empty()).to_be_true();
		expect(laneIsBuiltIn(palette, Lane::ClosedHat)).to_be_true();
		expect(laneIsBuiltIn(palette, Lane::Riser)).to_be_true();
		expect(laneIsBuiltIn(palette, Lane::Rim)).to_be_false();
	});

	it("rolls from the merged palette: a lane pointed at any folder, and an exact file", _ {
		auto override = std::make_unique<PaletteOverride>();
		expect(override->parse(fixture(), builtIn)).to_be_true();
		FakeCard card = harness::factoryCardWithPack();
		card.add("SAMPLES/DRUMS/MyKicks", {"Mine 1.wav", "Mine 2.wav"});
		for (uint32_t entropy = 0; entropy < 40; entropy++) {
			SoundPicks sounds;
			expect(rollSounds(genres::techno, override->palette(), sounds, entropy, card)).to_be_true();
			expect(std::string(sounds.lane[static_cast<int32_t>(Lane::Kick)].c_str()).rfind("SAMPLES/DRUMS/MyKicks/Mine ", 0))
			    .to_equal(size_t{0});
			expect(std::string(sounds.lane[static_cast<int32_t>(Lane::Rim)].c_str()))
			    .to_equal(std::string("SAMPLES/DRUMS/Rim/808 Rim.wav"));
			expect(std::string(sounds.part[1].c_str())).to_equal(std::string("SYNTHS/047 Basic Dirty Bass.XML"));
		}
	});

	it("reads either quote style, entities, comments and roles, and skips what it doesn't know", _ {
		auto override = std::make_unique<PaletteOverride>();
		bool ok = override->parse(wrap("<!-- mine --><role name='PAD'><preset path='SYNTHS/A &amp; B.XML'/></role>"
		                               "<lane name=\"kick\"><tag text=\"909\" weight=\"7\"/><unknown/></lane>"
		                               "<lane name=\"gong\"><tag text=\"x\"/></lane><part name=\"X\"><nested><deep/></nested></part>"),
		                          builtIn);
		expect(ok).to_be_true();
		PartSpec pad{.role = Role::Pad, .name = "DRONE"};
		expect(std::string(override->palette().partEntries(pad)[0].path)).to_equal(std::string("SYNTHS/A & B.XML"));
		const LaneSounds* kick = override->palette().laneSounds(Lane::Kick);
		expect(kick->folders.empty()).to_be_true(); // the lane's default folders
		expect(kick->tags.size()).to_equal(size_t{1});
		expect(kick->tags[0].weight).to_equal(uint8_t{7});
	});

	it("ignores a malformed or oversized file and keeps the built-in palette", _ {
		const std::string bad[] = {
		    "",
		    "<palette>",
		    "<palette><part name=\"A\"></palette>",
		    "<other/>",
		    wrap("<part><preset path=\"x\"/></part>"),                       // a part needs a name
		    wrap("<lane name=\"kick\"><tag text=\"909\" weight=\"300\"/></lane>"),
		    wrap("<lane name=\"kick\"><tag text=\"909\" weight=\"-1\"/></lane>"),
		    wrap("<lane name=\"kick\"><file path=\"808 Kick.wav\"/></lane>"),  // a file needs its folder
		    wrap("<lane name=\"kick\"><tag text=\"SAMPLES/x\"/></lane>"),     // a tag mustn't have one
		    wrap("<role name=\"bass\"><preset path=\"a &bogus; b\"/></role>"),
		    wrap("<role name=\"bass\"><preset path=\"unterminated/></role>"),
		    wrap("<role name=\"bass\"><preset/></role>"),
		};
		for (const std::string& text : bad) {
			auto override = std::make_unique<PaletteOverride>();
			expect(override->parse(text, builtIn)).to_be_false();
			expect(unchanged(*override)).to_be_true();
		}
		std::string huge;
		for (int i = 0; i < 100; i++) {
			huge += "<role name=\"bass\"><preset path=\"SYNTHS/" + std::string(40, 'a') + ".XML\"/></role>";
		}
		auto override = std::make_unique<PaletteOverride>();
		expect(override->parse(wrap(huge), builtIn)).to_be_false();
		expect(unchanged(*override)).to_be_true();
	});
});

CPPSPEC_SPEC(palette_file)
