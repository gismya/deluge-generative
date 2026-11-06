// Rough render (spec §21.2): a Standard MIDI File with one track per part.

#include "harness.h"
#include "render/render.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace deluge::generative;

namespace {

harness::render::Rendering tinyRendering() {
	harness::render::Rendering rendering;
	rendering.recipe = harness::technoRecipe();
	rendering.recipe.tempo = 12800;
	rendering.seed = 1;
	rendering.bars = 1;
	rendering.events = {
	    Event::sectionStart(0, SectionKind::Intro, 0), Event::noteOn(0, 0, Lane::Kick, 0, 100),
	    Event::noteOff(6, 0, Lane::Kick, 0),           Event::noteOn(48, 1, Lane::None, 45, 88),
	    Event::noteOff(64, 1, Lane::None, 45),
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
describe render_midi("Rough render MIDI file", $ {
	it("writes format 1 at 96 PPQN: a conductor track, then one track per part named after it", _ {
		const std::vector<uint8_t> expected = {
		    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 4, 0, 96,
		    // Conductor: 128 BPM (468750 µs per quarter), 4/4, the Section as a marker.
		    'M', 'T', 'r', 'k', 0, 0, 0, 30,
		    0x00, 0xFF, 0x51, 0x03, 0x07, 0x27, 0x0E,
		    0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08,
		    0x00, 0xFF, 0x06, 0x07, 'I', 'N', 'T', 'R', 'O', ' ', '1',
		    0x00, 0xFF, 0x2F, 0x00,
		    // DRUMS on channel 10, lanes on GM drum notes: Kick 36.
		    'M', 'T', 'r', 'k', 0, 0, 0, 21,
		    0x00, 0xFF, 0x03, 0x05, 'D', 'R', 'U', 'M', 'S',
		    0x00, 0x99, 36, 100,
		    0x06, 0x89, 36, 0x40,
		    0x00, 0xFF, 0x2F, 0x00,
		    // ACID, the second part, on channel 2.
		    'M', 'T', 'r', 'k', 0, 0, 0, 20,
		    0x00, 0xFF, 0x03, 0x04, 'A', 'C', 'I', 'D',
		    0x30, 0x91, 45, 88,
		    0x10, 0x81, 45, 0x40,
		    0x00, 0xFF, 0x2F, 0x00,
		    // FX, the third part: no notes yet (section rules play it).
		    'M', 'T', 'r', 'k', 0, 0, 0, 10,
		    0x00, 0xFF, 0x03, 0x02, 'F', 'X',
		    0x00, 0xFF, 0x2F, 0x00,
		};
		expect(harness::render::midiFile(tinyRendering()) == expected).to_be_true();
	});

	it("leaves out muted notes, as Perform drops them at the output", _ {
		harness::render::Rendering silent = tinyRendering();
		silent.events = {};
		harness::render::Rendering muted = tinyRendering();
		muted.events = {mutedNoteOn(0, 0, Lane::Kick, 0), mutedNoteOn(48, 1, Lane::None, 45)};
		expect(harness::render::midiFile(muted) == harness::render::midiFile(silent)).to_be_true();
	});

	it("writes deltas past 127 ticks as variable-length quantities", _ {
		harness::render::Rendering rendering = tinyRendering();
		rendering.events = {
		    Event::noteOn(200, 1, Lane::None, 45, 88),
		    Event::noteOff(20000, 1, Lane::None, 45),
		};
		std::vector<uint8_t> file = harness::render::midiFile(rendering);
		// The ACID track's events: 200 = 0x81 0x48, then 19800 = 0x81 0x9A 0x58.
		const std::vector<uint8_t> acid = {0x81, 0x48, 0x91, 45, 88, 0x81, 0x9A, 0x58, 0x81, 45, 0x40};
		expect(std::search(file.begin(), file.end(), acid.begin(), acid.end()) != file.end()).to_be_true();
	});
});

CPPSPEC_SPEC(render_midi)
