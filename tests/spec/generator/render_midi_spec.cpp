// Rough render (spec §21.2): a Standard MIDI File with one track per part, and a THEORY track for the theory review
// (spec §21.5).

#include "harness.h"
#include "render/render.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace deluge::generative;

namespace {

harness::render::Rendering tinyRendering() {
	harness::render::Rendering rendering;
	rendering.recipe = harness::acidRecipe();
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

/// Every Text event (FF 01) in each track, with its absolute tick. The writer never uses running status.
std::vector<std::vector<std::pair<int32_t, std::string>>> textEvents(const std::vector<uint8_t>& file) {
	std::vector<std::vector<std::pair<int32_t, std::string>>> tracks;
	size_t at = 14;
	auto vlq = [&]() {
		uint32_t v = 0;
		uint8_t b = 0;
		do {
			b = file[at++];
			v = (v << 7) | (b & 0x7F);
		} while ((b & 0x80) != 0);
		return v;
	};
	while (at + 8 <= file.size()) {
		const size_t end = at + 8
		                   + ((size_t{file[at + 4]} << 24) | (size_t{file[at + 5]} << 16) | (size_t{file[at + 6]} << 8)
		                      | file[at + 7]);
		at += 8;
		tracks.emplace_back();
		int32_t tick = 0;
		while (at < end) {
			tick += static_cast<int32_t>(vlq());
			if (file[at] != 0xFF) {
				at += 3;
				continue;
			}
			const uint8_t type = file[at + 1];
			at += 2;
			const uint32_t length = vlq();
			if (type == 0x01) {
				tracks.back().emplace_back(tick, std::string(file.begin() + at, file.begin() + at + length));
			}
			at += length;
		}
	}
	return tracks;
}

using Lines = std::vector<std::pair<int32_t, std::string>>;

/// tinyRendering()'s THEORY lines: the Recipe and Acid techno's parts, lanes and rules, all at tick 0.
const Lines kTinyTheory = {
    {0, "recipe genre=techno-acid seed=1 entropy=0 tempo=128.00 swing=0 energy=25.0 density=25.0 "
        "complexity=25.0 motion=25.0 phrase=8 hysteresis=2.0"},
    {0, "part DRUMS role=Drums flavour=None gate=0.0"},
    {0, "lane DRUMS.Kick gate=0.0 note=36"},
    {0, "lane DRUMS.Snare gate=0.0 note=38"},
    {0, "lane DRUMS.Rim gate=0.0 note=37"},
    {0, "lane DRUMS.HatC gate=0.0 note=42"},
    {0, "rule source=DRUMS.HatC kind=Lock target=DRUMS.Kick off=1 strength=90.0"},
    {0, "lane DRUMS.HatO gate=0.0 note=46"},
    {0, "rule source=DRUMS.HatO kind=Lock target=DRUMS.Kick off=1 strength=90.0"},
    {0, "lane DRUMS.Crash gate=0.0 note=49"},
    {0, "lane DRUMS.Tom gate=0.0 note=45"},
    {0, "part ACID role=Bass flavour=Loop gate=0.0"},
    {0, "part DRONE role=Pad flavour=Sustain gate=0.0"},
    {0, "part FX role=FX flavour=None gate=0.0"},
    {0, "lane FX.Riser gate=0.0 note=84"},
    {0, "lane FX.Fall gate=0.0 note=85"},
    {0, "lane FX.Impact gate=0.0 note=86"},
    {0, "key root=A scale=minor pcs=9,11,0,2,4,5,7"},
};

harness::render::StepState stepState(std::initializer_list<int8_t> chord, int32_t densityPoints) {
	harness::render::StepState state;
	for (int8_t s : chord) {
		state.chord.steps[state.chord.size++] = s;
	}
	state.energy = macroPoints(25);
	state.density = macroPoints(densityPoints);
	return state;
}

} // namespace

// clang-format off
describe render_midi("Rough render MIDI file", $ {
	it("writes format 1 at 96 PPQN: a conductor track, then one track per part named after it, each ending on the last bar line", _ {
		std::vector<uint8_t> expected = {
		    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 6, 0, 96,
		    // Conductor: 128 BPM (468750 µs per quarter), 4/4, the Section as a marker.
		    'M', 'T', 'r', 'k', 0, 0, 0, 31,
		    0x00, 0xFF, 0x51, 0x03, 0x07, 0x27, 0x0E,
		    0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08,
		    0x00, 0xFF, 0x06, 0x07, 'I', 'N', 'T', 'R', 'O', ' ', '1',
		    0x83, 0x00, 0xFF, 0x2F, 0x00, // End of Track at 384, the bar line
		    // DRUMS on channel 10, lanes on GM drum notes: Kick 36.
		    'M', 'T', 'r', 'k', 0, 0, 0, 22,
		    0x00, 0xFF, 0x03, 0x05, 'D', 'R', 'U', 'M', 'S',
		    0x00, 0x99, 36, 100,
		    0x06, 0x89, 36, 0x40,
		    0x82, 0x7A, 0xFF, 0x2F, 0x00, // 378 after the note-off at 6
		    // ACID, the second part, on channel 2.
		    'M', 'T', 'r', 'k', 0, 0, 0, 21,
		    0x00, 0xFF, 0x03, 0x04, 'A', 'C', 'I', 'D',
		    0x30, 0x91, 45, 88,
		    0x10, 0x81, 45, 0x40,
		    0x82, 0x40, 0xFF, 0x2F, 0x00, // 320 after the note-off at 64
		    // DRONE, the third part: no notes in this bar.
		    'M', 'T', 'r', 'k', 0, 0, 0, 14,
		    0x00, 0xFF, 0x03, 0x05, 'D', 'R', 'O', 'N', 'E',
		    0x83, 0x00, 0xFF, 0x2F, 0x00,
		    // FX, the fourth part: no notes yet (section rules play it).
		    'M', 'T', 'r', 'k', 0, 0, 0, 11,
		    0x00, 0xFF, 0x03, 0x02, 'F', 'X',
		    0x83, 0x00, 0xFF, 0x2F, 0x00,
		    // THEORY, last: 887 bytes of its name and kTinyTheory's lines as Text events at tick 0, then End of Track.
		    'M', 'T', 'r', 'k', 0, 0, 0x03, 0x77,
		    0x00, 0xFF, 0x03, 0x06, 'T', 'H', 'E', 'O', 'R', 'Y',
		};
		for (const auto& [tick, line] : kTinyTheory) {
			expected.insert(expected.end(), {0x00, 0xFF, 0x01});
			if (line.size() > 127) { // the Recipe line's length takes two bytes
				expected.push_back(static_cast<uint8_t>(0x80 | (line.size() >> 7)));
			}
			expected.push_back(static_cast<uint8_t>(line.size() & 0x7F));
			expected.insert(expected.end(), line.begin(), line.end());
		}
		expected.insert(expected.end(), {0x83, 0x00, 0xFF, 0x2F, 0x00}); // End of Track at 384, the bar line
		expect(harness::render::midiFile(tinyRendering()) == expected).to_be_true();
	});

	it("ends with a THEORY track: the Recipe, then each part's, lane's and interplay rule's facts at tick 0", _ {
		harness::render::Rendering rendering = tinyRendering();
		expect(harness::render::theoryText(rendering) == kTinyTheory).to_be_true();
		const auto tracks = textEvents(harness::render::midiFile(rendering));
		expect(tracks.size()).to_equal(size_t{6});
		expect(tracks.back() == kTinyTheory).to_be_true();
		rendering.entropy = 7;
		expect(harness::render::theoryText(rendering)[0].second.starts_with("recipe genre=techno-acid seed=1 entropy=7 "))
		    .to_be_true();
	});

	it("moves a rule's strength by its sensitivity over the Recipe's Complexity", _ {
		harness::render::Rendering rendering = tinyRendering();
		rendering.recipe.macros.set(Macro::Complexity, macroPoints(50)); // 25 points over: -40 x 25/50 = -20
		const Lines lines = harness::render::theoryText(rendering);
		expect(std::find(lines.begin(), lines.end(),
		                 std::pair<int32_t, std::string>{0, "rule source=DRUMS.HatC kind=Lock target=DRUMS.Kick off=1 "
		                                                    "strength=70.0"})
		       != lines.end())
		    .to_be_true();
	});

	it("writes each key change, ornament hit, chord change and bar line's macro change on the THEORY track", _ {
		harness::render::Rendering rendering = tinyRendering();
		rendering.bars = 2;
		// A ratchet's second hit on the closed hat and a flam's grace on the acid line are ornaments; a plain hit and a
		// muted ornament aren't written.
		Event ratchet = Event::noteOn(54, 0, Lane::ClosedHat, 0, 70);
		ratchet.ornament = true;
		Event grace = Event::noteOn(93, 1, Lane::None, 45, 40);
		grace.ornament = true;
		Event muted = harness::mutedNoteOn(120, 1, Lane::None, 45);
		muted.ornament = true;
		rendering.events = {Event::noteOn(48, 0, Lane::ClosedHat, 0, 80), ratchet, grace, muted,
		                    Event::keyScale(400, 2, Scale::Dorian)};
		// The step that decided the ratchet's hit; the grace's isn't known, so its line has no step.
		rendering.ornamentSteps = {{ratchet, 2}};
		for (int32_t step = 0; step < 32; step++) {
			// The chord and Density both change on step 8, but Density is written only on bar lines, exactly.
			rendering.steps.push_back(step < 8 ? stepState({0, 2, 4}, 25) : stepState({3, 5, 7, 9}, 30));
		}
		rendering.steps[16].density = macroPoints(30) - 1;
		Lines lines = harness::render::theoryText(rendering);
		lines.erase(lines.begin(), std::find_if(lines.begin(), lines.end(),
		                                        [](const auto& line) { return line.second.starts_with("key"); }));
		const Lines expected = {
		    {0, "key root=A scale=minor pcs=9,11,0,2,4,5,7"},
		    {0, "macros energy=25.0 density=25.0"},
		    {0, "chord 0,2,4"},
		    {54, "ornament part=DRUMS note=42 step=2"},
		    {93, "ornament part=ACID note=45"},
		    {192, "chord 3,5,7,9"},
		    {384, "macros energy=25.0 density=29.984375"},
		    {400, "key root=D scale=dorian pcs=2,4,5,7,9,11,0"},
		};
		expect(lines == expected).to_be_true();
	});

	it("leaves out muted notes, as Perform drops them at the output", _ {
		harness::render::Rendering silent = tinyRendering();
		silent.events = {};
		harness::render::Rendering muted = tinyRendering();
		muted.events = {harness::mutedNoteOn(0, 0, Lane::Kick, 0), harness::mutedNoteOn(48, 1, Lane::None, 45)};
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

	it("ends a track after a note-off that runs past the last bar line", _ {
		harness::render::Rendering rendering = tinyRendering();
		rendering.events = {Event::noteOn(0, 1, Lane::None, 45, 88), Event::noteOff(400, 1, Lane::None, 45)};
		std::vector<uint8_t> file = harness::render::midiFile(rendering);
		// The ACID track: the note-off 400 ticks in (0x83 0x10), then End of Track at once.
		const std::vector<uint8_t> acid = {0x83, 0x10, 0x81, 45, 0x40, 0x00, 0xFF, 0x2F, 0x00};
		expect(std::search(file.begin(), file.end(), acid.begin(), acid.end()) != file.end()).to_be_true();
	});
});

CPPSPEC_SPEC(render_midi)
