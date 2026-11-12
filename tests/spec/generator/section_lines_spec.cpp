// The Section-line cut and re-strike (spec §12.5, §21.2): every sounding note ends on a Section line, and a note
// still meant to hold to the end of the first bar is re-struck there by late entry (spec §5.6).

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <set>
#include <vector>

using namespace harness;

namespace {

/// One note at the start of every 2 bars.
constexpr Scale kScales[] = {Scale::Minor};
constexpr WeightedBars kBuildBars[] = {{1, 1}};

/// A Bass of notes lasting `Steps`, one every 2 bars. One-bar phrases and a 1-bar Intro, so bar 1 is a Section
/// line; no Arc offsets.
template <int32_t Steps>
struct LongBass {
	static constexpr Genre genre{
	    .name = "lines",
	    .facts = {.scales = kScales},
	    .phraseBars = 1,
	    .parts = LongBassParts<Steps>::parts,
	    .arc = {.introPhrases = 1, .buildBars = kBuildBars, .dropLiftEnergy = 0, .dropLiftDensity = 0, .offsets = {}},
	};
};

Run runOf(const Genre& genre) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.recipe.macros.set(Macro::Motion, 0);
	return run;
}

/// The Bass's events of `kind` up to bar 2's line, where its next note starts.
std::vector<Event> notes(const Result& result, EventKind kind) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, kind)) {
		if (e.lane == Lane::None
		    && (e.tick < 2 * kBarTicks || (kind == EventKind::NoteOff && e.tick == 2 * kBarTicks))) {
			out.push_back(e);
		}
	}
	return out;
}

} // namespace

// clang-format off
describe section_lines("Section lines", $ {
	it("end a held note on the line and re-strike it by late entry", _ {
		// A 2-bar note from step 0 has a whole bar left on bar 1's line. The next note, on bar 2, ends it.
		Result result = play(runOf(LongBass<32>::genre), 3);
		auto ons = notes(result, EventKind::NoteOn);
		auto offs = notes(result, EventKind::NoteOff);
		expect(ticksOf(ons) == std::set<int32_t>{0, kBarTicks}).to_be_true();
		expect(ticksOf(offs) == std::set<int32_t>{kBarTicks, 2 * kBarTicks}).to_be_true();
		expect(ons.size()).to_equal(size_t{2});
		expect(ons[1].pitch).to_equal(ons[0].pitch);
		expect(ons[1].velocity).to_equal(ons[0].velocity);
		// The note-off comes before the re-strike on the line.
		int32_t offAt = -1;
		int32_t onAt = -1;
		for (int32_t i = 0; i < static_cast<int32_t>(result.events.size()); i++) {
			const Event& e = result.events[i];
			if (e.tick == kBarTicks && e.lane == Lane::None) {
				(e.kind == EventKind::NoteOff ? offAt : onAt) = i;
			}
		}
		expect(offAt).to_be_less_than(onAt);
	});

	it("drop a held note whose remainder is shorter than the first bar", _ {
		Result result = play(runOf(LongBass<20>::genre), 3);
		expect(ticksOf(notes(result, EventKind::NoteOn)) == std::set<int32_t>{0}).to_be_true();
		expect(ticksOf(notes(result, EventKind::NoteOff)) == std::set<int32_t>{kBarTicks}).to_be_true();
	});

	it("re-strike with the Energy of that moment", _ {
		Run run = runOf(LongBass<32>::genre);
		run.steer = [](int32_t nextStep, SteeringQueue& queue) {
			if (nextStep == 8) {
				queue.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), kMacroFull});
			}
		};
		auto ons = notes(play(run, 3), EventKind::NoteOn);
		expect(ons.size()).to_equal(size_t{2});
		expect(ons[1].velocity).to_be_greater_than(ons[0].velocity);
	});
});

CPPSPEC_SPEC(section_lines)
