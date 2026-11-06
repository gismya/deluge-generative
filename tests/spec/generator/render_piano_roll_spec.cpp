// Rough render (spec §21.2): a text piano roll, one character per 16th.

#include "harness.h"
#include "render/render.h"

#include "cppspec.hpp"

#include <string>

using namespace deluge::generative;

namespace {

harness::render::Rendering oneBar() {
	harness::render::Rendering rendering;
	rendering.recipe = harness::technoRecipe(); // A minor
	rendering.recipe.tempo = 12800;
	rendering.seed = 1;
	rendering.bars = 1;
	rendering.events = {
	    Event::sectionStart(0, SectionKind::Intro, 0),
	    Event::noteOn(0, 0, Lane::Kick, 0, 100),
	    Event::noteOff(6, 0, Lane::Kick, 0),
	    Event::noteOn(98, 0, Lane::ClosedHat, 0, 40), // leans 2 ticks late into step 4
	    Event::noteOff(104, 0, Lane::ClosedHat, 0),
	    Event::noteOn(48, 1, Lane::None, 45, 88),
	    Event::noteOff(64, 1, Lane::None, 45),
	    Event::noteOn(192, 1, Lane::None, 57, 112), // held three 16ths
	    Event::noteOff(260, 1, Lane::None, 57),
	};
	return rendering;
}

Event mutedNoteOn(int32_t tick, uint8_t part, Lane lane, uint8_t pitch) {
	Event e = Event::noteOn(tick, part, lane, pitch, 100);
	e.muted = true;
	return e;
}

} // namespace

// clang-format off
describe render_piano_roll("Rough render piano roll", $ {
	it("draws each lane and each sounding pitch, highest first, with hits by velocity and held notes as '='", _ {
		const std::string expected =
		    "techno seed 1, A minor, 128.00 BPM, 1 bar\n"
		    "\n"
		    "           |1               |\n"
		    "Section    |INTRO 1         |\n"
		    "DRUMS Kick |X...............|\n"
		    "DRUMS Rim  |................|\n"
		    "DRUMS HatC |....o...........|\n"
		    "DRUMS HatO |................|\n"
		    "ACID A3    |........X==.....|\n"
		    "ACID A2    |..x.............|\n";
		expect(harness::render::pianoRoll(oneBar())).to_equal(expected);
	});

	it("leaves out muted notes, as Perform drops them at the output", _ {
		harness::render::Rendering silent = oneBar();
		silent.events = {};
		harness::render::Rendering muted = oneBar();
		muted.events = {mutedNoteOn(0, 0, Lane::Kick, 0), mutedNoteOn(48, 1, Lane::None, 45)};
		expect(harness::render::pianoRoll(muted)).to_equal(harness::render::pianoRoll(silent));
	});

	it("starts a new block every 4 bars", _ {
		harness::render::Rendering rendering = oneBar();
		rendering.bars = 5;
		std::string roll = harness::render::pianoRoll(rendering);
		expect(roll.find("           |1               |2               |3               |4               |\n"))
		    .not_().to_equal(std::string::npos);
		expect(roll.find("\n\n           |5               |\nSection    |                |\n"))
		    .not_().to_equal(std::string::npos);
	});
});

CPPSPEC_SPEC(render_piano_roll)
