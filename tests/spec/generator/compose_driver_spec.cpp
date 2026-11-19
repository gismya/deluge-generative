// The portable Compose driver (spec §2.5, §6.8, §15.1–§15.6): LENGTH and where End goes, the clip notes and the
// automation nodes it writes, and an untouched Perform driver run matching a Compose run for every genre (§21.2).

#include "generative/compose/compose_driver.h"
#include "generative/compose/song_size.h"
#include "generative/perform/riser_timing.h"
#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

using namespace harness;
using namespace deluge::generative::compose;

namespace {

/// Keeps what Compose writes, in call order.
struct RecordingWriter final : SongWriter {
	const Recipe* recipe = nullptr;
	std::vector<Note> notes;
	std::vector<std::pair<Automated, Node>> nodes;
	std::vector<std::pair<Automated, int8_t>> bases;
	int32_t endTick = -1;
	uint32_t soundingParts = 0;
	int32_t calls = 0;
	int32_t endCall = -1;
	int32_t riserMicros = 0;
	void begin(const Recipe& r) override {
		recipe = &r;
		calls++;
	}
	void note(const Note& n) override {
		notes.push_back(n);
		calls++;
	}
	void node(const Automated& a, const Node& n) override {
		nodes.push_back({a, n});
		calls++;
	}
	void base(const Automated& a, int8_t value) override {
		bases.push_back({a, value});
		calls++;
	}
	void end(int32_t tick, uint32_t sounding) override {
		endTick = tick;
		soundingParts = sounding;
		endCall = calls++;
	}
	int32_t sampleMicros(const Event& /*noteOn*/) override { return riserMicros; }

	[[nodiscard]] std::vector<Node> nodesOf(const Automated& a) const {
		std::vector<Node> out;
		for (const auto& [id, node] : nodes) {
			if (id == a) {
				out.push_back(node);
			}
		}
		return out;
	}
};

/// A Compose run, one step a call as a runner job would make them in chunks, with the Section each step played in.
struct Composed {
	RecordingWriter writer;
	std::vector<SectionKind> sections; ///< By step.
	int32_t endQueuedAt = -1;
	int32_t endTick = 0;
	int32_t target = 0;
	uint32_t overflowed = 0;
	uint32_t droppedSets = 0;
};

std::unique_ptr<Composed> composeSong(const Recipe& recipe, uint32_t seed, int32_t target, int32_t riserMicros = 0,
                                      const Takes& takes = Takes{}) {
	auto out = std::make_unique<Composed>();
	out->target = target;
	out->writer.riserMicros = riserMicros;
	auto generator = std::make_unique<Generator>();
	ComposeDriver driver;
	driver.begin(*generator, recipe, seed, takes, target, out->writer);
	// Each call decides one step.
	bool done = false;
	while (!done && out->sections.size() < 20000) { // a song that never ends fails below
		done = driver.run(1);
		out->sections.push_back(generator->arc().section());
	}
	out->endQueuedAt = driver.endQueuedAt();
	out->endTick = driver.endTick();
	out->overflowed = driver.clips().unpaired();
	out->droppedSets = driver.clips().droppedSets();
	return out;
}

/// The Sections an untouched Perform plays, step by step: the core with nothing on its queue.
std::vector<SectionKind> untouchedSections(const Recipe& recipe, uint32_t seed, int32_t steps) {
	auto generator = std::make_unique<Generator>();
	SteeringQueue queue;
	generator->start(recipe, seed, Takes{});
	std::vector<SectionKind> out;
	for (int32_t step = 0; step < steps; step++) {
		generator->decideNextStep(queue, nullptr);
		out.push_back(generator->arc().section());
		Event e;
		while (generator->popDue(INT32_MAX, &e)) {}
	}
	return out;
}

/// Perform's played stream paired into notes, as Compose pairs Compose's: the note-on's tick, to its note-off.
std::vector<Note> pairNotes(const std::vector<Event>& events) {
	std::vector<Note> out;
	std::vector<Event> open;
	for (const Event& e : events) {
		if (e.kind == EventKind::NoteOn && !e.muted) {
			open.push_back(e);
		}
		else if (e.kind == EventKind::NoteOff) {
			auto found = std::find_if(open.begin(), open.end(), [&](const Event& o) { return o.sameKey(e); });
			if (found != open.end()) {
				out.push_back(
				    {found->tick, e.tick - found->tick, found->part, found->lane, found->pitch, found->velocity});
				open.erase(found);
			}
		}
	}
	return out;
}

/// Notes that start before `cutTick` less the most a note leans early, so only the decisions before the cut's own
/// made them, ended no later than `cutTick`, sorted: what both runs agree on.
std::vector<Note> notesBefore(std::vector<Note> notes, int32_t cutTick) {
	std::vector<Note> out;
	for (Note n : notes) {
		if (n.tick < cutTick - kMaxLean) {
			n.length = std::min(n.tick + n.length, cutTick) - n.tick;
			out.push_back(n);
		}
	}
	std::sort(out.begin(), out.end(), [](const Note& a, const Note& b) {
		return std::tie(a.tick, a.part, a.lane, a.pitch, a.length, a.velocity)
		       < std::tie(b.tick, b.part, b.lane, b.pitch, b.length, b.velocity);
	});
	return out;
}

using LaneKey = std::tuple<uint8_t, Lane, SoundControl, bool>;
LaneKey keyOf(const Automated& a) {
	return {a.part, a.lane, a.control, a.throwDelay};
}

/// The value each automation lane has on each step line before `cut`, as Perform plays its Control events: a snap
/// jumps on its line, a ramp reaches its value on the next. Every lane starts at home, 0.
std::map<LaneKey, std::vector<int32_t>> performCurves(const std::vector<Event>& events, int32_t cut) {
	std::map<LaneKey, std::vector<int32_t>> out;
	auto apply = [&](const Automated& a, int32_t value, const Event& e) {
		auto [entry, added] = out.try_emplace(keyOf(a), std::vector<int32_t>(cut + 1, 0));
		std::vector<int32_t>& curve = entry->second;
		const int32_t step = e.tick / kTicksPerStep;
		for (int32_t s = e.snap ? step : step + 1; s <= cut; s++) {
			curve[s] = value;
		}
	};
	for (const Event& e : events) {
		if (e.kind != EventKind::Control || e.tick >= stepTick(cut)) {
			continue;
		}
		apply({e.part, e.lane, e.control, false}, e.offset, e);
		if (e.control == SoundControl::Space) {
			apply({e.part, e.lane, e.control, true}, e.throwDelay(), e);
		}
	}
	return out;
}

/// What a lane's nodes play at `tick`, as the firmware's automation does: a straight line into an interpolated node,
/// a jump onto any other; `base` with no node. `from` is a node index at or before `tick`'s, kept from call to call as
/// ticks rise.
int32_t nodeValue(const std::vector<Node>& nodes, int32_t base, int32_t tick, size_t& from) {
	if (nodes.empty()) {
		return base;
	}
	while (from + 1 < nodes.size() && nodes[from + 1].tick <= tick) {
		from++;
	}
	const Node& a = nodes[from];
	if (from + 1 == nodes.size() || tick <= a.tick) {
		return a.value;
	}
	const Node& b = nodes[from + 1];
	return b.interpolated ? a.value + (b.value - a.value) * (tick - a.tick) / (b.tick - a.tick) : a.value;
}

/// Compose's notes and automation against an untouched Perform run of the same Recipe and seed, up to the Outro's
/// first step: the only place Compose may differ is its length and where the Outro goes (spec §6.8).
struct Agreement {
	int32_t cut = 0;
	size_t notes = 0;         ///< Perform's, before the cut.
	bool sameNotes = false;   ///< Compose wrote exactly those.
	uint32_t overflowed = 0;  ///< Notes Compose couldn't pair.
	uint32_t droppedSets = 0; ///< Control sets Compose had no room for.
	size_t risers = 0;        ///< Riser notes among Perform's.
	int32_t worstControl = 0; ///< The furthest a lane's nodes stray from Perform's value on a step line.
	/// The same, only in bars where Perform's value doesn't move, from the bar's line to the next.
	int32_t worstFlat = 0;
};

/// A sample Riser `riserMicros` long plays in both runs, and both play `takes`.
Agreement agreement(const Recipe& recipe, uint32_t seed, int32_t target, int32_t riserMicros = 0,
                    const Takes& takes = Takes{}) {
	Agreement out;
	auto composed = composeSong(recipe, seed, target, riserMicros, takes);
	const int32_t cut = composed->endQueuedAt;
	out.cut = cut;
	if (cut <= 0) {
		return out;
	}
	Run run;
	run.recipe = recipe;
	run.seed = seed;
	run.takes = takes;
	run.riserMicros = riserMicros;
	Result performed = play(run, cut / kStepsPerBar + 2);

	const std::vector<Note> expected = notesBefore(pairNotes(performed.events), stepTick(cut));
	out.notes = expected.size();
	out.sameNotes = notesBefore(composed->writer.notes, stepTick(cut)) == expected;
	out.overflowed = composed->overflowed;
	out.droppedSets = composed->droppedSets;
	out.risers = std::count_if(expected.begin(), expected.end(), [](const Note& n) { return n.lane == Lane::Riser; });
	std::map<LaneKey, std::vector<Node>> nodes;
	for (const auto& [id, node] : composed->writer.nodes) {
		nodes[keyOf(id)].push_back(node);
	}
	std::map<LaneKey, int32_t> bases;
	for (const auto& [id, value] : composed->writer.bases) {
		bases[keyOf(id)] = value;
	}
	for (const auto& [key, curve] : performCurves(performed.events, cut)) {
		const std::vector<Node>& lane = nodes[key];
		size_t from = 0;
		bool flat = false; // Perform's value doesn't move in this bar, from its line to the next
		for (int32_t s = 0; s < cut; s++) {
			const int32_t value = nodeValue(lane, bases[key], stepTick(s), from);
			const int32_t off = std::abs(value - curve[s]);
			out.worstControl = std::max(out.worstControl, off);
			if (s % kStepsPerBar == 0) {
				const int32_t barEnd = std::min(s + kStepsPerBar, cut);
				flat = std::all_of(curve.begin() + s, curve.begin() + barEnd + 1,
				                   [&](int32_t v) { return v == curve[s]; });
			}
			if (flat) {
				out.worstFlat = std::max(out.worstFlat, off);
			}
		}
	}
	return out;
}

/// What every agreement must show, beyond the notes.
void expectAgreed(auto& self, const Agreement& agreed) {
	expect(agreed.overflowed).to_equal(0U);
	expect(agreed.droppedSets).to_equal(0U);
	expect(agreed.worstControl).to_be_less_than(ClipBuilder::kBarTolerance + ClipBuilder::kNodeTolerance + 2);
	// A bar Perform holds still holds in Compose, but for the line through the kept points.
	expect(agreed.worstFlat).to_be_less_than(ClipBuilder::kNodeTolerance + 2);
}

Recipe recipeOf(const Genre& genre, int32_t motion = -1) {
	Recipe recipe = newRecipe(genre, 0);
	if (motion >= 0) {
		recipe.macros.set(Macro::Motion, macroPoints(motion));
	}
	return recipe;
}

/// Events fed straight to a ClipBuilder, for the pairing and the node rules alone.
struct Built {
	RecordingWriter writer;
	ClipBuilder builder;
	explicit Built(const std::vector<Event>& events, int32_t end = 100 * kTicksPerStep) {
		builder.begin(writer);
		for (const Event& e : events) {
			builder.take(e);
		}
		builder.finish(end, [](const Automated&) -> int8_t { return 7; });
	}
};

Event control(int32_t step, SoundControl control, int8_t offset, bool snap) {
	return Event::controlChange(stepTick(step), 0, Lane::None, control, offset, snap);
}

constexpr Automated kBrightness{0, Lane::None, SoundControl::Brightness, false};

/// `genre` with only drift moving its controls: no gesture, no Section offset, no Drop lift.
std::shared_ptr<EditedGenre> driftOnly(const Genre& genre) {
	auto edited = std::make_shared<EditedGenre>();
	edited->parts.assign(genre.parts.begin(), genre.parts.end());
	for (PartSpec& part : edited->parts) {
		part.gestures.off = kAllMoments;
	}
	edited->genre = genre;
	edited->genre.parts = edited->parts;
	for (SectionOffsets& offsets : edited->genre.arc.offsets) {
		offsets = {};
	}
	edited->genre.arc.dropLiftEnergy = 0;
	edited->genre.arc.dropLiftDensity = 0;
	edited->genre.dropOuts = {}; // their Throws are gestures
	return edited;
}

constexpr WeightedBars kOneBarBuild[] = {{1, 1}};

/// `genre` where every Groove lasts `groovePhrases` and goes into a one-bar Build, which starts at the last bar line of
/// the Groove's phrase: no Groove ends on a phrase line.
std::shared_ptr<EditedGenre> shortBuildsOnly(const Genre& genre, uint8_t groovePhrases) {
	auto edited = std::make_shared<EditedGenre>();
	edited->parts.assign(genre.parts.begin(), genre.parts.end());
	edited->genre = genre;
	edited->genre.parts = edited->parts;
	edited->genre.arc.groovePhrases = {groovePhrases, groovePhrases};
	edited->genre.arc.grooveToBuild = 1000;
	edited->genre.arc.buildBars = kOneBarBuild;
	return edited;
}

/// The most interpolated nodes any lane writes in one bar, for `genre` with only drift moving, at `motion`.
int32_t mostDriftNodesInABar(const Genre& genre, int32_t motion) {
	auto edited = driftOnly(genre);
	Run run;
	run.recipe = newRecipe(edited->genre, 0);
	run.recipe.macros.set(Macro::Motion, macroPoints(motion));
	run.skipIntro = true;
	constexpr int32_t kBars = 64;
	Result performed = play(run, kBars);
	Built built(performed.events, kBars * kBarTicks);
	// Where a ramp starts after the lane held still a bar or more: that node is the hold's end, not drift's.
	std::set<std::pair<LaneKey, int32_t>> holdEnds;
	std::map<LaneKey, std::pair<int32_t, int8_t>> moved; // where each lane's last change ended, and its value
	for (const Event& e : performed.events) {
		if (e.kind != EventKind::Control) {
			continue;
		}
		const LaneKey key = keyOf({e.part, e.lane, e.control, false});
		auto [last, added] = moved.try_emplace(key, 0, 0);
		if (last->second.second == e.offset) {
			continue;
		}
		if (!e.snap && e.tick - last->second.first >= kBarTicks) {
			holdEnds.insert({key, e.tick});
		}
		last->second = {e.snap ? e.tick : e.tick + kTicksPerStep, e.offset};
	}
	// A node on a bar line ends that bar. A Throw that always plays (Dub's Siren release) snaps: its bar and the next,
	// where it falls, are a gesture's, not drift's.
	std::map<std::pair<LaneKey, int32_t>, int32_t> perBar;
	std::set<std::pair<LaneKey, int32_t>> thrown;
	for (const auto& [id, node] : built.writer.nodes) {
		const int32_t bar = (node.tick - 1) / kBarTicks;
		if (holdEnds.erase({keyOf(id), node.tick}) > 0) {
			continue; // one node per hold's end: a second on the same tick is drift's
		}
		if (node.interpolated) {
			perBar[{keyOf(id), bar}]++;
		}
		else if (node.tick > 0) {
			thrown.insert({keyOf(id), node.tick / kBarTicks});
			thrown.insert({keyOf(id), node.tick / kBarTicks + 1});
		}
	}
	int32_t most = perBar.empty() ? -1 : 0;
	for (const auto& [where, count] : perBar) {
		if (!thrown.contains(where)) {
			most = std::max(most, count);
		}
	}
	return most;
}

} // namespace

// clang-format off
describe compose_driver("The Compose driver", $ {
	context("LENGTH (spec §15.1)", _ {
		it("becomes a target step count once, in integer math", _ {
			expect(targetSteps(240, 12800)).to_equal(2048); // 4:00 at 128 BPM: 128 bars
			expect(targetSteps(60, 12000)).to_equal(480);
			expect(targetSteps(90, 12345)).to_equal(740); // 740.7 rounds down
			expect(targetSteps(1200, kMaxTempo)).to_equal(79920);
		});

		it("queues End once the target is reached, where a Groove ends on a phrase line, so the Outro takes its place", _ {
			for (const Genre* genre : allGenres()) {
				for (uint32_t seed : {1U, 2U, 3U}) {
					const Recipe recipe = recipeOf(*genre);
					const int32_t target = targetSteps(90, recipe.tempo);
					auto composed = composeSong(recipe, seed, target);
					const int32_t cut = composed->endQueuedAt;
					const int32_t phrase = genre->phraseSteps();
					expect(cut).to_be_greater_than(target - 1);
					expect(cut % phrase).to_equal(0);
					expect(composed->sections[cut - 1] == SectionKind::Groove).to_be_true();
					expect(composed->sections[cut] == SectionKind::Outro).to_be_true();
					// Untouched, the Groove ends there by itself: nothing is cut short.
					const std::vector<SectionKind> untouched = untouchedSections(recipe, seed, cut + 1);
					expect(untouched[cut] != SectionKind::Groove).to_be_true();
					// No Build or Break before it is cut, and the song ends where the Outro does.
					for (int32_t s = cut; s < static_cast<int32_t>(composed->sections.size()); s++) {
						expect(composed->sections[s] == SectionKind::Outro).to_be_true();
					}
					expect(composed->endTick).to_equal(stepTick(static_cast<int32_t>(composed->sections.size())));
					expect(composed->endTick).to_equal(stepTick(cut + genre->arc.outroPhrases * phrase));
				}
			}
		});

		it("plays the Intro, the Groove up to the target and the Outro at Motion 0", _ {
			for (const Genre* genre : allGenres()) {
				const Recipe recipe = recipeOf(*genre, 0);
				const int32_t target = targetSteps(60, recipe.tempo);
				auto composed = composeSong(recipe, 1, target);
				const int32_t phrase = genre->phraseSteps();
				// The first phrase line at or after the target with a Groove before it: a target in the Intro gets a
				// phrase of Groove.
				const int32_t introEnd = genre->arc.introPhrases * phrase;
				expect(composed->endQueuedAt).to_equal(std::max((target + phrase - 1) / phrase * phrase, introEnd + phrase));
				SectionKind last = SectionKind::Intro;
				std::vector<SectionKind> order{SectionKind::Intro};
				for (SectionKind kind : composed->sections) {
					if (kind != last) {
						order.push_back(kind);
						last = kind;
					}
				}
				expect(order == std::vector<SectionKind>{SectionKind::Intro, SectionKind::Groove, SectionKind::Outro})
				    .to_be_true();
			}
		});

		it("plays on past the target through two Grooves that go into a short Build, then ends the third", _ {
			// A Groove of two phrases sees a phrase line of its own, where the third ends; one of a phrase sees none,
			// and End takes the place of the Groove after the third's Build.
			for (uint8_t phrases : {1, 2}) {
				auto edited = shortBuildsOnly(genres::technoAcid, phrases);
				const Recipe recipe = recipeOf(edited->genre);
				const int32_t phrase = edited->genre.phraseSteps();
				const int32_t target = targetSteps(60, recipe.tempo);
				auto composed = composeSong(recipe, 1, target);
				const std::vector<SectionKind>& sections = composed->sections;
				const int32_t cut = composed->endQueuedAt;
				expect(cut).to_be_greater_than(target - 1);
				expect(cut % phrase).to_equal(0);
				expect(sections[cut] == SectionKind::Outro).to_be_true();
				expect(sections[cut - 1] == (phrases == 1 ? SectionKind::Build : SectionKind::Groove)).to_be_true();
				// The Builds that start past the target played on, up to the cap; past it a Groove with no phrase line of
				// its own plays its Build too.
				int32_t playedOn = 0;
				for (int32_t s = target; s < cut; s++) {
					if (sections[s] == SectionKind::Build && sections[s - 1] != SectionKind::Build) {
						playedOn++;
					}
				}
				expect(playedOn).to_equal(ComposeDriver::kMaxShortBuildGrooves + (phrases == 1 ? 1 : 0));
			}
		});
	});

	context("notes (spec §15.3, §15.4)", _ {
		it("pairs each note-on with its note-off into a length, at its exact tick", _ {
			Built built({Event::noteOn(5, 1, Lane::None, 60, 100), Event::noteOn(7, 0, Lane::Kick, 36, 90),
			             Event::noteOff(30, 0, Lane::Kick, 36), Event::noteOff(53, 1, Lane::None, 60)});
			expect(built.writer.notes.size()).to_equal(2U);
			expect(built.writer.notes[0] == Note{7, 23, 0, Lane::Kick, 36, 90}).to_be_true();
			expect(built.writer.notes[1] == Note{5, 48, 1, Lane::None, 60, 100}).to_be_true();
			expect(built.writer.soundingParts).to_equal(0b11U);
		});

		it("counts a note-on whose note-off never comes", _ {
			Built built({Event::noteOn(5, 1, Lane::None, 60, 100)});
			expect(built.writer.notes.empty()).to_be_true();
			expect(built.builder.unpaired()).to_equal(1U);
		});

		it("drops a muted note with its note-off, so a part that never sounds gets no clip", _ {
			Event muted = Event::noteOn(0, 2, Lane::None, 48, 100);
			muted.muted = true;
			Built built({muted, Event::noteOn(0, 1, Lane::None, 60, 100), Event::noteOff(24, 2, Lane::None, 48),
			             Event::noteOff(24, 1, Lane::None, 60)});
			expect(built.writer.notes.size()).to_equal(1U);
			expect(built.writer.notes[0].part).to_equal(1);
			expect(built.writer.soundingParts).to_equal(0b10U);
			expect(built.builder.unpaired()).to_equal(0U);
		});

		it("writes nothing for Section, KeyScale and Sound events", _ {
			Built built({Event::sectionStart(0, SectionKind::Groove, 0), Event::keyScale(0, 2, Scale::Dorian),
			             Event::sound(0, 1, 3)});
			expect(built.writer.notes.empty()).to_be_true();
			expect(built.writer.nodes.empty()).to_be_true();
			expect(built.writer.bases.empty()).to_be_true();
		});

		it("ends every clip at the Outro's end, after every note and node", _ {
			auto composed = composeSong(recipeOf(genres::technoAcid), 1, targetSteps(60, 12800));
			const RecordingWriter& w = composed->writer;
			expect(w.endCall).to_equal(w.calls - 1);
			for (const Note& n : w.notes) {
				expect(n.length).to_be_greater_than(0);
				expect(n.tick + n.length).to_be_less_than(composed->endTick + 1);
			}
			// A node is a point in the clip: before its end.
			expect(w.nodes.empty()).to_be_false();
			for (const auto& [id, node] : w.nodes) {
				expect(node.tick).to_be_less_than(composed->endTick);
			}
			uint32_t played = 0;
			for (const Note& n : w.notes) {
				played |= 1U << n.part;
			}
			expect(w.soundingParts).to_equal(played);
			expect(played).to_be_greater_than(0U);
		});

		it("starts a sample Riser late, so its file ends on the Drop at the Recipe's tempo (spec §9.5)", _ {
			const Recipe recipe = recipeOf(genres::technoAcid);
			const int32_t target = targetSteps(240, recipe.tempo);
			auto synth = composeSong(recipe, 1, target);
			const int32_t micros = 2 * 4 * perform::stepMicros(recipe.tempo) * kStepsPerBar / 4; // two bars of 4 beats
			auto sample = composeSong(recipe, 1, target, micros);
			int32_t risers = 0;
			for (size_t i = 0; i < synth->writer.notes.size(); i++) {
				const Note& a = synth->writer.notes[i];
				if (a.lane != Lane::Riser) {
					continue;
				}
				risers++;
				auto match = std::find_if(sample->writer.notes.begin(), sample->writer.notes.end(), [&](const Note& b) {
					return b.lane == Lane::Riser && b.tick + b.length == a.tick + a.length;
				});
				expect(match != sample->writer.notes.end()).to_be_true();
				const int32_t twoBars = 2 * kBarTicks;
				expect(match->length).to_equal(std::min(a.length, twoBars));
			}
			expect(risers).to_be_greater_than(0);
		});
	});

	context("automation (spec §15.6)", _ {
		it("writes no node for a control that never moves, and stores its one value", _ {
			Built built({control(0, SoundControl::Brightness, 12, true), control(0, SoundControl::Decay, 0, true)});
			expect(built.writer.nodes.empty()).to_be_true();
			expect(built.writer.bases.size()).to_equal(1U);
			expect(built.writer.bases[0].first == kBrightness).to_be_true();
			expect(built.writer.bases[0].second).to_equal(12);
		});

		it("stores the Recipe-centre value for a control that moves", _ {
			Built built({control(0, SoundControl::Brightness, 12, true), control(4, SoundControl::Brightness, 40, true)});
			expect(built.writer.bases[0].second).to_equal(7);
		});

		it("stores the Recipe's centre for a part's moving lane, and none on a Drums row or the delay lift", _ {
			for (const Genre* genre : allGenres()) {
				Recipe recipe = recipeOf(*genre);
				recipe.macros.set(Macro::Energy, macroPoints(45));
				recipe.macros.set(Macro::Density, macroPoints(10));
				recipe.macros.set(Macro::Complexity, macroPoints(40));
				auto composed = composeSong(recipe, 1, targetSteps(60, recipe.tempo));
				auto centres = std::make_unique<Generator>();
				centres->start(recipe, 1, Takes{});
				int32_t away = 0;
				for (const auto& [id, value] : composed->writer.bases) {
					if (composed->writer.nodesOf(id).empty()) {
						continue; // a lane that doesn't move stores its one value
					}
					const bool row = id.lane != Lane::None && genre->parts[id.part].role == Role::Drums;
					if (id.throwDelay || row) {
						expect(value).to_equal(0);
						continue;
					}
					const int32_t layer = centres->centreLayer(id.part, id.lane, id.control);
					expect(value).to_equal(layer);
					away += layer != 0 ? 1 : 0;
				}
				expect(away).to_be_greater_than(0);
			}
		});

		it("ramps a lane's first step from home, as Perform plays it", _ {
			Built built({control(0, SoundControl::Brightness, 20, false)});
			expect(built.writer.nodesOf(kBrightness) == std::vector<Node>{{0, 0, false}, {stepTick(1), 20, true}})
			    .to_be_true();
			expect(built.writer.bases[0].second).to_equal(7);
		});

		it("ends a ramp on the song's last step a tick before the end", _ {
			Built built({control(99, SoundControl::Brightness, 30, false)}, stepTick(100));
			expect(built.writer.nodesOf(kBrightness) == std::vector<Node>{{0, 0, false}, {stepTick(99), 0, true},
			                                                              {stepTick(100) - 1, 30, true}})
			    .to_be_true();
		});

		it("makes a snap a step-exact jump and a ramp an interpolated node a step later", _ {
			Built built({control(0, SoundControl::Brightness, 10, true), control(4, SoundControl::Brightness, 30, true),
			             control(8, SoundControl::Brightness, 90, false)});
			const std::vector<Node> nodes = built.writer.nodesOf(kBrightness);
			expect(nodes == std::vector<Node>{{0, 10, false}, {stepTick(4), 30, false}, {stepTick(8), 30, true},
			                                  {stepTick(9), 90, true}})
			    .to_be_true();
		});

		it("moves a ramp's node a tick before a snap on its line", _ {
			Built built({control(4, SoundControl::Brightness, 30, false), control(5, SoundControl::Brightness, -20, true)});
			const std::vector<Node> nodes = built.writer.nodesOf(kBrightness);
			expect(nodes == std::vector<Node>{{0, 0, false}, {stepTick(4), 0, true}, {stepTick(5) - 1, 30, true},
			                                  {stepTick(5), -20, false}})
			    .to_be_true();
		});

		it("keeps the Throw's delay lift as its own lane, starting only when it leaves home", _ {
			Event hit = Event::controlChange(stepTick(4), 0, Lane::None, SoundControl::Space, 60, true, 100);
			Event fall = Event::controlChange(stepTick(6), 0, Lane::None, SoundControl::Space, 60, false, 50);
			Built built({control(0, SoundControl::Space, 0, true), hit, fall});
			const Automated lift{0, Lane::None, SoundControl::Space, true};
			expect(built.writer.nodesOf(lift) == std::vector<Node>{{0, 0, false}, {stepTick(4), 100, false},
			                                                       {stepTick(6), 100, true}, {stepTick(7), 50, true}})
			    .to_be_true();
			const Automated space{0, Lane::None, SoundControl::Space, false};
			expect(built.writer.nodesOf(space) == std::vector<Node>{{0, 0, false}, {stepTick(4), 60, false}}).to_be_true();
		});

		it("follows a straight ramp with its two ends", _ {
			std::vector<Event> events;
			for (int32_t s = 1; s <= 16; s++) {
				events.push_back(control(s, SoundControl::Brightness, static_cast<int8_t>(s * 5), false));
			}
			Built built(events);
			expect(built.writer.nodesOf(kBrightness) == std::vector<Node>{{0, 0, false}, {stepTick(17), 80, true}})
			    .to_be_true();
		});

		it("gives a ramp after a hold nodes at its start and its end", _ {
			Built built({control(100, SoundControl::Brightness, 10, false)}, 200 * kTicksPerStep);
			expect(built.writer.nodesOf(kBrightness) == std::vector<Node>{{0, 0, false}, {stepTick(100), 0, true},
			                                                              {stepTick(101), 10, true}})
			    .to_be_true();
		});

		it("keeps only a bar's end where its curve bends no more than the bar tolerance", _ {
			// Up 2 a step for 8 steps, then flat: 7 at most off the line across the bar.
			std::vector<Event> events;
			for (int32_t s = 1; s <= 8; s++) {
				events.push_back(control(s, SoundControl::Brightness, static_cast<int8_t>(s * 2), false));
			}
			events.push_back(control(16, SoundControl::Brightness, 17, false));
			Built built(events);
			expect(built.writer.nodesOf(kBrightness) == std::vector<Node>{{0, 0, false}, {stepTick(17), 17, true}})
			    .to_be_true();
		});

		it("thins drift to at most one interpolated node a bar per control, and one where a hold of a bar ends", _ {
			for (const Genre* genre : allGenres()) {
				for (int32_t motion : {25, 100}) {
					const int32_t most = mostDriftNodesInABar(*genre, motion);
					expect(most).to_equal(1);
				}
			}
		});
	});

	context("the size estimate and the progress bar (spec §14.5, §15.9)", _ {
		it("counts at least the notes of the densest bar at full Energy, Density and Complexity", _ {
			for (const Genre* genre : allGenres()) {
				Recipe recipe = recipeOf(*genre, 25);
				for (Macro macro : {Macro::Energy, Macro::Density, Macro::Complexity}) {
					recipe.macros.set(macro, macroPoints(50));
				}
				auto composed = composeSong(recipe, 1, targetSteps(300, recipe.tempo));
				std::map<int32_t, int32_t> perBar;
				for (const Note& note : composed->writer.notes) {
					perBar[note.tick / kBarTicks]++;
				}
				int32_t most = 0;
				for (const auto& [bar, notes] : perBar) {
					most = std::max(most, notes);
				}
				expect(most).to_be_greater_than(0);
				expect(most).to_be_less_than(notesPerBar(*genre) + 1);
			}
		});

		it("never sees a song run past maxBars()", _ {
			for (const Genre* genre : allGenres()) {
				for (int32_t motion : {0, 1, 25, 50}) {
					for (uint32_t seed : {1U, 2U}) {
						const Recipe recipe = recipeOf(*genre, motion);
						const int32_t target = targetSteps(90, recipe.tempo);
						auto composed = composeSong(recipe, seed, target);
						const auto bars = static_cast<int32_t>(composed->sections.size()) / kStepsPerBar;
						expect(composed->endTick).to_be_greater_than(0);
						expect(bars).to_be_less_than(maxBars(recipe, target) + 1);
					}
				}
			}
		});

		it("counts the progress bar to the target plus one Outro phrase", _ {
			const Recipe recipe = recipeOf(genres::technoAcid);
			const int32_t outroBars = genres::technoAcid.arc.outroPhrases * genres::technoAcid.phraseBars;
			expect(expectedBars(recipe, 4 * kStepsPerBar)).to_equal(4 + outroBars);
			expect(expectedBars(recipe, 4 * kStepsPerBar + 1)).to_equal(5 + outroBars);
		});

		it("never shrinks as LENGTH grows, and finds the longest LENGTH that fits", _ {
			for (const Genre* genre : allGenres()) {
				const Recipe recipe = recipeOf(*genre);
				int64_t last = 0;
				for (int32_t seconds = kMinLengthSeconds; seconds <= kMaxLengthSeconds; seconds += kLengthStepSeconds) {
					// LENGTHs whose targets round up to the same phrase share an estimate.
					const int64_t bytes = estimateBytes(recipe, seconds);
					expect(bytes).to_be_greater_than(last - 1);
					last = bytes;
					const int32_t longest = longestFitting(recipe, bytes);
					expect(longest).to_be_greater_than(seconds - 1);
					expect(estimateBytes(recipe, longest)).to_equal(bytes);
					expect(longestFitting(recipe, bytes - 1)).to_be_less_than(seconds);
				}
				expect(estimateBytes(recipe, kMaxLengthSeconds)).to_be_greater_than(estimateBytes(recipe, kMinLengthSeconds));
				expect(longestFitting(recipe, INT64_MAX)).to_equal(kMaxLengthSeconds);
				expect(longestFitting(recipe, estimateBytes(recipe, kMinLengthSeconds) - 1)).to_equal(0);
			}
		});
	});

	context("Perform and Compose agree (spec §6.8)", _ {
		it("plays an untouched Perform run's notes and controls up to the Outro, for every genre", _ {
			for (const Genre* genre : allGenres()) {
				for (uint32_t seed : {1U, 5U}) {
					const Recipe recipe = recipeOf(*genre);
					const Agreement agreed = agreement(recipe, seed, targetSteps(120, recipe.tempo));
					expect(agreed.cut).to_be_greater_than(0);
					expect(agreed.notes).to_be_greater_than(0U);
					expect(agreed.sameNotes).to_be_true();
					expectAgreed(self, agreed);
				}
			}
		});

		it("with a sample Riser, which starts late in both", _ {
			size_t risers = 0;
			for (const Genre* genre : allGenres()) {
				const Recipe recipe = recipeOf(*genre);
				// A bar: shorter than any Build, so it starts late.
				const int32_t bar = kStepsPerBar * perform::stepMicros(recipe.tempo);
				const Agreement agreed = agreement(recipe, 1, targetSteps(240, recipe.tempo), bar);
				expect(agreed.cut).to_be_greater_than(0);
				expect(agreed.sameNotes).to_be_true();
				expectAgreed(self, agreed);
				risers += agreed.risers;
			}
			expect(risers).to_be_greater_than(0U);
		});

		it("with re-rolled takes", _ {
			Takes takes;
			for (uint8_t& take : takes.take) {
				take = 2;
			}
			for (const Genre* genre : allGenres()) {
				const Recipe recipe = recipeOf(*genre);
				const int32_t target = targetSteps(120, recipe.tempo);
				const Agreement agreed = agreement(recipe, 1, target, 0, takes);
				expect(agreed.cut).to_be_greater_than(0);
				expect(agreed.sameNotes).to_be_true();
				expectAgreed(self, agreed);
				// The takes play: the song differs from the first takes'.
				expect(composeSong(recipe, 1, target, 0, takes)->writer.notes != composeSong(recipe, 1, target)->writer.notes)
				    .to_be_true();
			}
		});

		it("at Motion 0 and at full Motion too", _ {
			for (const Genre* genre : allGenres()) {
				for (int32_t motion : {0, 100}) {
					const Recipe recipe = recipeOf(*genre, motion);
					const Agreement agreed = agreement(recipe, 3, targetSteps(60, recipe.tempo));
					expect(agreed.cut).to_be_greater_than(0);
					expect(agreed.sameNotes).to_be_true();
					expectAgreed(self, agreed);
				}
			}
		});
	});
});

CPPSPEC_SPEC(compose_driver);
