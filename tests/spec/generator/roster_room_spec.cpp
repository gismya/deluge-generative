// Room in the Generator block for the largest roster (spec §2.7, §4.5, §20 H18): Detroit techno's six parts, with a
// 4-note Pad and Stab (spec §5.8, §7.4) and one-note Bass and Lead.

#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

using namespace harness;

namespace deluge::generative {

/// Stands in for Stab, Sustain and Motif until they land (#123, #128, #129), at their worst for the block: on every
/// onset a Chords or Pad part plays a 4-note voicing (spec §5.8, §7.4) and a Lead one note, each lasting to the next
/// onset and overlapping it as a slide does, through the shared expression stage (Ornaments, Lean, late entry).
/// Consecutive onsets share no pitch, so two whole voicings sound at once. The Pad and the Stab play every step; the
/// Motif plays once a pattern, so its notes last two bars and a gate opening between its onsets re-strikes them.
struct TestFlavour {
	/// Plays the parts with no flavour code through decide() while it lives.
	struct Installed {
		Installed() { Generator::testFlavour_ = decide; }
		~Installed() { Generator::testFlavour_ = nullptr; }
		Installed(const Installed&) = delete;
		Installed& operator=(const Installed&) = delete;
	};

	static void decide(Generator& g, int32_t partIndex, const Generator::Step& step) {
		const PartSpec& spec = g.genre_->parts[partIndex];
		PatternState& state = g.parts_[partIndex].lanes[0];
		g.updatePick(state, spec.pool, spec.band, static_cast<uint8_t>(partIndex), Lane::None, step);
		const Pattern& pattern = spec.pool[state.entry];
		const int32_t patternStep = step.index % pattern.length;
		if (!g.planned(partIndex, -1, step.index, step.density) || g.keptOut(step, partIndex, Lane::None)) {
			return;
		}
		const int32_t notes = spec.role == Role::Lead ? 1 : kMaxChordNotes;
		const int32_t spacing = spec.role == Role::Lead ? pattern.length : 1; // steps from one onset to the next
		const int32_t length = spacing * kTicksPerStep + spec.loop.slideOverlapTicks;
		const int32_t onset = step.index / spacing;
		for (int32_t n = 0; n < notes; n++) {
			const int32_t steps = 2 * n + onset % 2; // root, 3rd, 5th, 7th, then the scale steps between
			g.express(partIndex, spec.expression, state, patternStep,
			          {Lane::None, g.resolvePitch(spec, steps), pattern.level[patternStep], length, false}, step);
		}
	}
};

} // namespace deluge::generative

namespace {

constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
/// One onset every two bars, mid-pattern: its notes last over the bar line where gates change and Sections start.
constexpr Pattern kEveryTwoBars[] = {{"................X...............", "................................", 0}};
/// Half the notes ratchet 4 times: the most hits in the buffer, and the rest overlap the next onset.
constexpr Expression kRatchetHalf{.spread = 6,
                                  .ornaments = {.rateFloor = 500, .rateCeiling = 500, .ratchet = {0, 0, 1}}};
constexpr Register kWide{60, -24, 24};

/// Detroit techno's roster (spec §4.5): Techno's Drums, Bass and FX, and a test Pad, Stab and Motif. The Motif is
/// gated at 30, so the Arc's Density moves it in and out: late entry holds its notes, and re-strikes those that last
/// to the end of the bar its gate opens on.
const Genre& detroitShaped() {
	static const PartSpec kParts[] = {
	    genres::techno.parts[0],
	    genres::techno.parts[1],
	    {.role = Role::Pad,
	     .flavour = Flavour::Sustain,
	     .name = "STRINGS",
	     .pool = kEveryStep,
	     .reg = kWide,
	     .expression = kRatchetHalf},
	    {.role = Role::Chords,
	     .flavour = Flavour::Stab,
	     .name = "STAB",
	     .pool = kEveryStep,
	     .reg = kWide,
	     .expression = kRatchetHalf},
	    {.role = Role::Lead,
	     .flavour = Flavour::Motif,
	     .name = "MOTIF",
	     .pool = kEveryTwoBars,
	     .densityGate = static_cast<int16_t>(macroPoints(30)),
	     .reg = kWide}, // no Ornaments: its one onset a pattern would ratchet on every pass or on none
	    genres::techno.parts[2],
	};
	static const Genre kGenre = [] {
		Genre genre = genres::techno;
		genre.name = "detroit-shaped";
		genre.parts = kParts;
		return genre;
	}();
	return kGenre;
}

using NoteKey = std::tuple<uint8_t, Lane, uint8_t>;

} // namespace

// clang-format off
describe roster_room("Room in the Generator block", $ {
	it("counts a voicing on Chords and Pad, one note on Bass and Lead, and a hit per lane on Drums and FX", _ {
		// A part's table holds two onsets' worth on a pitched part (the next overlapping it), one per lane on a Kit.
		expect(maxSounding(Role::Chords)).to_equal(8);
		expect(maxSounding(Role::Pad)).to_equal(8);
		expect(maxSounding(Role::Bass)).to_equal(2);
		expect(maxSounding(Role::Lead)).to_equal(2);
		expect(maxSounding(Role::Drums)).to_equal(11);
		expect(maxSounding(Role::FX)).to_equal(4);
	});

	it("sizes the sounding-note pool and the event buffer for Detroit techno's roster", _ {
		// Drums 11 + Bass 2 + Pad 8 + Stab 8 + Motif 2 + FX 4.
		expect(SoundingNotes::kTotal).to_equal(35);
		// Per part on a bar line: each source's 4 hits, a note-off for each inside the step, a note-off for each note
		// sounding and a re-strike for each: Drums 11·10, FX 4·10, Pad and Stab 4·8 + 8·2, Bass and Motif 1·8 + 2·2,
		// plus a Section, a KeyScale and 6 Sound events: 278. Off a bar line, no re-strikes: 243. Control events: 108.
		expect(kEventBufferSize).to_equal(278 + 243 + 108);
		expect(kMaxControlSets).to_equal(20);
	});

	it("never overflows the event buffer or a sounding-note table with a 4-note Pad and Stab", _ {
		TestFlavour::Installed flavour;
		constexpr uint32_t kSeeds[] = {1, 2, 3, 4, 5, 0xDEADBEEF};
		std::map<uint8_t, size_t> most;  // part → the most notes it had sounding at once
		std::map<uint8_t, int32_t> hits; // part → its Ornament hits
		bool fourAtOnce = false;
		int32_t restrikes = 0; // the Motif's late entries: a note-on on a bar line between its onsets
		for (uint32_t seed : kSeeds) {
			Run run;
			run.recipe = technoRecipe();
			run.recipe.genre = &detroitShaped();
			run.recipe.macros.set(Macro::Complexity, macroPoints(50));
			run.recipe.macros.set(Macro::Density, macroPoints(50));
			run.seed = seed;
			// The Intro, a Groove, a Break, a Build and a Drop at any seed.
			Result result = play(run, 8 * (2 + 8 + 2 + 2 + 2));
			expect(result.droppedEvents).to_equal(0U);
			expect(result.evictedNotes).to_equal(0U);
			std::map<NoteKey, int32_t> sounding;
			std::map<std::pair<uint8_t, int32_t>, int32_t> onsAt; // (part, tick) → note-ons
			for (const Event& e : result.events) {
				const NoteKey key{e.part, e.lane, e.pitch};
				if (e.kind == EventKind::NoteOn) {
					sounding[key] = e.tick;
					hits[e.part] += e.ornament ? 1 : 0;
					constexpr int32_t kBar = kStepsPerBar * kTicksPerStep;
					restrikes += e.part == 4 && e.tick % kBar == 0 && e.tick / kBar % 2 == 0 ? 1 : 0;
					fourAtOnce = fourAtOnce || ++onsAt[{e.part, e.tick}] == kMaxChordNotes;
					size_t part = std::count_if(sounding.begin(), sounding.end(),
					                            [&](const auto& s) { return std::get<0>(s.first) == e.part; });
					most[e.part] = std::max(most[e.part], part);
				}
				else if (e.kind == EventKind::NoteOff) {
					sounding.erase(key);
				}
			}
		}
		// The roster really sounds its worst: two whole voicings at once on the Pad and the Stab, ratcheted hits, and
		// the Motif re-struck on bar lines.
		expect(fourAtOnce).to_be_true();
		expect(most[2]).to_equal(size_t{8});
		expect(most[3]).to_equal(size_t{8});
		expect(most[4]).to_equal(size_t{2});
		expect(restrikes).to_be_greater_than(0);
		expect(hits[2]).to_be_greater_than(0);
		expect(hits[3]).to_be_greater_than(0);
	});

	it("never evicts a Loop part's slid notes at the edge of what its genre data may give (spec §8.5, §8.6)", _ {
		// Every step slides. A slid note leaned +12 on the 1 with 12 ticks of overlap ends on the tick the & starts
		// (12 + 24 + 12 = 48), and with a flammed & leaned +6, on the tick its grace hit starts (48 + 6 − 6). Both
		// notes are still in the table when the & is decided, 12 ticks before it starts.
		static constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
/// One onset every two bars, mid-pattern: its notes last over the bar line where gates change and Sections start.
constexpr Pattern kEveryTwoBars[] = {{"................X...............", "................................", 0}};
		static constexpr DegreeWeight kDegrees[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}};
		static constexpr LoopSpec kSliding{.degrees = kDegrees, .slide = 1000, .slideOverlapTicks = 12};
		static constexpr PartSpec kPlain[] = {{.role = Role::Bass,
		                                       .flavour = Flavour::Loop,
		                                       .pool = kEveryStep,
		                                       .reg = kWide,
		                                       .loop = kSliding,
		                                       .expression = {.lean = {12, 0, 0, 0}}}};
		static constexpr PartSpec kFlammed[] = {
		    {.role = Role::Bass,
		     .flavour = Flavour::Loop,
		     .pool = kEveryStep,
		     .reg = kWide,
		     .loop = kSliding,
		     .expression = {.lean = {12, 0, 6, 0},
		                    .ornaments = {.rateFloor = 1000, .rateCeiling = 1000, .flam = 1, .flamGap = 6}}}};
		static constexpr Genre kPlainEdge{.name = "plain-edge", .parts = kPlain};
		static constexpr Genre kFlammedEdge{.name = "flammed-edge", .parts = kFlammed};
		for (const Genre* genre : {&kPlainEdge, &kFlammedEdge}) {
			expect(loopNotesValid(*genre)).to_be_true();
			bool edge = false; // a note ended on the tick a note two onsets later started
			for (uint32_t seed : {1U, 2U, 3U}) {
				Run run;
				run.recipe = technoRecipe();
				run.recipe.genre = genre;
				run.seed = seed;
				Result result = play(run, 8 * 16);
				expect(result.evictedNotes).to_equal(0U);
				expect(result.droppedEvents).to_equal(0U);
				std::vector<std::pair<int32_t, int32_t>> notes; // each note's on and off ticks, in note-on order
				std::map<NoteKey, size_t> sounding;               // key → its note in `notes`
				for (const Event& e : result.events) {
					const NoteKey key{e.part, e.lane, e.pitch};
					if (e.kind == EventKind::NoteOn) {
						sounding[key] = notes.size();
						notes.emplace_back(e.tick, 0);
					}
					else if (e.kind == EventKind::NoteOff) {
						notes[sounding[key]].second = e.tick;
					}
				}
				for (size_t i = 0; i + 2 < notes.size(); i++) {
					edge = edge || notes[i].second == notes[i + 2].first;
				}
			}
			expect(edge).to_be_true();
		}
	});
});

CPPSPEC_SPEC(roster_room)
