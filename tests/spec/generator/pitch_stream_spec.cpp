// Pitch in the event stream (spec §7, §9.7, §12.8): every scale, Degree sets, the home chord, and key and scale
// changes landing on the next phrase line or an Arc Drop, with KeyScale events and note-offs.

#include "harness.h"

#include "cppspec.hpp"

#include <map>
#include <tuple>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars

Command keyChange(uint8_t root, Scale scale) {
	return {CommandKind::SetKey, root, static_cast<int32_t>(scale)};
}

/// Steers one command onto the queue when the driver is about to decide `step`.
std::function<void(int32_t, SteeringQueue&)> at(int32_t step, Command command) {
	auto pushed = std::make_shared<bool>(false);
	return [=](int32_t nextStep, SteeringQueue& queue) {
		if (nextStep == step && !*pushed) {
			*pushed = queue.push(command);
		}
	};
}

std::vector<Event> pitchedNoteOns(const std::vector<Event>& events) {
	std::vector<Event> out;
	for (const Event& e : only(events, EventKind::NoteOn)) {
		if (e.lane == Lane::None) {
			out.push_back(e);
		}
	}
	return out;
}

/// A Loop Lead that lands on every degree, with notes long enough to sound across the next step.
constexpr Pattern kEveryStep[] = {{"XXXXXXXXXXXXXXXX", "................", 0}};
constexpr DegreeWeight kEveryDegree[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}};
constexpr PaletteEntry kLeadSound[] = {{"SYNTHS/000 Test.XML"}};
constexpr PartSpec kPentatonicLead[] = {{
    .role = Role::Lead,
    .flavour = Flavour::Loop,
    .name = "LEAD",
    .pool = kEveryStep,
    .degreeSet = DegreeSet::Pentatonic,
    .loop = {.degrees = kEveryDegree, .octaveUp = 300, .octaveDown = 300, .slide = 0, .gateTicks = 2 * kTicksPerStep},
    .palette = kLeadSound,
}};
constexpr Genre kPentatonic{.name = "pentatonic", .parts = kPentatonicLead};

/// The same Lead playing every degree, in a genre whose 4-bar Build lands its Drop halfway through a phrase.
constexpr PartSpec kFullLead[] = {{
    .role = Role::Lead,
    .flavour = Flavour::Loop,
    .name = "LEAD",
    .pool = kEveryStep,
    .loop = {.degrees = kEveryDegree, .slide = 0, .gateTicks = 2 * kTicksPerStep},
    .palette = kLeadSound,
}};
constexpr WeightedBars kShortBuild[] = {{4, 1}};
constexpr Genre kMidPhraseDrop{
    .name = "midphrasedrop",
    .parts = kFullLead,
    .arc = {.introPhrases = 1, .groovePhrases = {1, 1}, .breakPhrases = {1, 1}, .buildBars = kShortBuild},
};

int32_t firstTickOf(const std::vector<Event>& events, EventKind kind, int32_t from = 0) {
	for (const Event& e : events) {
		if (e.kind == kind && e.tick >= from) {
			return e.tick;
		}
	}
	return -1;
}

} // namespace

// clang-format off
describe pitch_stream("Pitch in the event stream", $ {
	it("plays every pitched note in the key's scale and in its Register, in all 12 scales", _ {
		for (int32_t s = 0; s < kNumScales; s++) {
			for (uint8_t root : {uint8_t{0}, uint8_t{6}, uint8_t{11}}) {
				Run run;
				run.recipe = technoRecipe();
				run.recipe.scale = static_cast<Scale>(s);
				run.recipe.keyRoot = root;
				run.recipe.macros.set(Macro::Complexity, kMacroFull);
				run.recipe.macros.set(Macro::Motion, kMacroFull);
				run.seed = static_cast<uint32_t>(s * 12 + root);
				Result result = play(run, 32);
				std::vector<Event> notes = pitchedNoteOns(result.events);
				expect(notes.empty()).to_be_false();
				for (const Event& e : notes) {
					Register reg = genres::techno.parts[e.part].registerOrDefault();
					int32_t tonic = tonicNote(reg, root);
					expect(inScale(e.pitch % 12, root, run.recipe.scale)).to_be_true();
					expect(static_cast<int32_t>(e.pitch)).to_be_between(tonic + reg.spanLow, tonic + reg.spanHigh);
				}
			}
		}
	});

	it("keeps a Pentatonic part on its Degree set's notes, where a Full part strays off them", _ {
		for (Scale scale : {Scale::Minor, Scale::Major, Scale::HungarianMinor}) {
			auto outsideSet = [scale](const Event& e) {
				int32_t pitchClass = (e.pitch - 9 + 12) % 12; // technoRecipe() is in A
				for (int32_t degree = 0; degree < kDegreesPerScale; degree++) {
					if (kScaleSemitones[static_cast<int32_t>(scale)][degree] == pitchClass) {
						return snap(degree, DegreeSet::Pentatonic, scale, 0) != degree;
					}
				}
				return true;
			};
			for (const Genre* genre : {&kPentatonic, &kMidPhraseDrop}) {
				Run run;
				run.recipe = technoRecipe();
				run.recipe.genre = genre;
				run.recipe.scale = scale;
				std::map<uint8_t, int32_t> pitchClasses;
				int32_t outside = 0;
				for (const Event& e : pitchedNoteOns(play(run, 16).events)) {
					pitchClasses[e.pitch % 12]++;
					outside += outsideSet(e) ? 1 : 0;
				}
				expect(pitchClasses.size()).to_be_greater_than(size_t{2});
				if (genre == &kPentatonic) {
					expect(outside).to_equal(0);
				}
				else {
					expect(outside).to_be_greater_than(0);
				}
			}
		}
	});

	it("sends the key and scale once, on step 0, while nothing changes them", _ {
		Run run;
		run.recipe = technoRecipe();
		std::vector<Event> keys = only(play(run, 16).events, EventKind::KeyScale);
		expect(keys.size()).to_equal(size_t{1});
		expect(keys[0].tick).to_equal(0);
		expect(keys[0].keyRoot).to_equal(uint8_t{9});
		expect(keys[0].scale).to_equal(Scale::Minor);
	});

	it("lands a queued key change on the next phrase line, and changes the notes from there", _ {
		Run run;
		run.recipe = technoRecipe(); // A Minor
		run.steer = at(20, keyChange(2, Scale::Phrygian));
		Result result = play(run, 24);
		std::vector<Event> keys = only(result.events, EventKind::KeyScale);
		expect(keys.size()).to_equal(size_t{2});
		const int32_t line = stepTick(kPhraseSteps);
		expect(keys[1].tick).to_equal(line);
		expect(keys[1].keyRoot).to_equal(uint8_t{2});
		expect(keys[1].scale).to_equal(Scale::Phrygian);
		int32_t before = 0;
		int32_t after = 0;
		for (const Event& e : pitchedNoteOns(result.events)) {
			if (e.tick < line) {
				expect(inScale(e.pitch % 12, 9, Scale::Minor)).to_be_true();
				before++;
			}
			else {
				expect(inScale(e.pitch % 12, 2, Scale::Phrygian)).to_be_true();
				after++;
			}
		}
		expect(before).to_be_greater_than(0);
		expect(after).to_be_greater_than(0);
		expect(result.generator->recipe().keyRoot).to_equal(uint8_t{2});
		expect(result.generator->recipe().scale).to_equal(Scale::Phrygian);
	});

	it("lands a change queued at a phrase line's decision on that line, and a later one on the next", _ {
		Run onTime;
		onTime.recipe = technoRecipe();
		onTime.steer = at(kPhraseSteps, keyChange(4, Scale::Minor));
		expect(only(play(onTime, 24).events, EventKind::KeyScale)[1].tick).to_equal(stepTick(kPhraseSteps));
		Run late;
		late.recipe = technoRecipe();
		late.steer = at(kPhraseSteps + 1, keyChange(4, Scale::Minor));
		expect(only(play(late, 24).events, EventKind::KeyScale)[1].tick).to_equal(stepTick(2 * kPhraseSteps));
	});

	it("lands a pending key change on an Arc Drop when the Drop comes before the phrase line", _ {
		// Intro, Groove and Break are a phrase each and the Build is 4 bars, so the Drop is at step 448, halfway
		// through the phrase that ends at step 512.
		Run run;
		run.recipe = technoRecipe();
		run.recipe.genre = &kMidPhraseDrop;
		run.steer = at(3 * kPhraseSteps + 2, keyChange(0, Scale::Dorian));
		Result result = play(run, 40);
		int32_t drop = -1;
		for (const Event& e : only(result.events, EventKind::Section)) {
			if (e.section == SectionKind::Groove && e.occurrence == 1) {
				drop = e.tick;
			}
		}
		expect(drop).to_equal(stepTick(3 * kPhraseSteps + 4 * kStepsPerBar));
		std::vector<Event> keys = only(result.events, EventKind::KeyScale);
		expect(keys.size()).to_equal(size_t{2});
		expect(keys[1].tick).to_equal(drop);
	});

	it("ends every held pitched note where the key changes, and plays the new key's notes fresh", _ {
		Run run;
		run.recipe = technoRecipe();
		run.recipe.genre = &kMidPhraseDrop; // every note is held into the next step
		run.steer = at(20, keyChange(2, Scale::Minor));
		Result result = play(run, 16);
		const int32_t line = stepTick(kPhraseSteps);
		expect(firstTickOf(result.events, EventKind::KeyScale, 1)).to_equal(line);
		std::map<uint8_t, int32_t> sounding; // pitch → note-on tick
		int32_t endedAtLine = 0;
		for (const Event& e : result.events) {
			if (e.kind == EventKind::NoteOn) {
				sounding[e.pitch] = e.tick;
			}
			else if (e.kind == EventKind::NoteOff) {
				auto found = sounding.find(e.pitch);
				expect(found != sounding.end()).to_be_true();
				if (found != sounding.end()) {
					bool startedBefore = found->second < line;
					expect(!startedBefore || e.tick <= line).to_be_true();
					endedAtLine += startedBefore && e.tick == line ? 1 : 0;
					sounding.erase(found);
				}
			}
		}
		expect(endedAtLine).to_be_greater_than(0);
	});

	it("shows a key change pending until it lands, and logs it at the step where it lands", _ {
		Generator generator;
		generator.start(technoRecipe(), 1, Takes{});
		SteeringQueue queue;
		LogEntry entries[4];
		SteeringLog log(entries, 4);
		queue.push(keyChange(5, Scale::Dorian));
		while (generator.nextStep() <= kPhraseSteps) {
			generator.decideNextStep(queue, &log);
			Event event;
			while (generator.popDue(INT32_MAX, &event)) {
			}
		}
		expect(log.size()).to_equal(1);
		expect(log.at(0).step).to_equal(0); // step 0 is a phrase line too
		generator.start(technoRecipe(), 1, Takes{});
		log.clear();
		generator.decideNextStep(queue, &log);
		queue.push(keyChange(5, Scale::Dorian));
		expect(generator.keyChangePending()).to_be_false(); // still in the queue
		generator.decideNextStep(queue, &log);
		expect(generator.keyChangePending()).to_be_true();
		while (generator.nextStep() <= kPhraseSteps) {
			generator.decideNextStep(queue, &log);
			Event event;
			while (generator.popDue(INT32_MAX, &event)) {
			}
		}
		expect(generator.keyChangePending()).to_be_false();
		expect(log.size()).to_equal(1);
		expect(log.at(0).step).to_equal(kPhraseSteps);
	});

	it("plays the home chord with as many Extensions as Complexity and the genre allow", _ {
		Run plain;
		plain.recipe = technoRecipe();
		plain.recipe.macros.set(Macro::Complexity, 0);
		Chord triad = play(plain, 1).generator->chord();
		expect(triad.size).to_equal(uint8_t{3});
		expect(triad.steps[0]).to_equal(int8_t{0});
		Run rich;
		rich.recipe = technoRecipe();
		rich.recipe.macros.set(Macro::Complexity, kMacroFull);
		Chord ninth = play(rich, 1).generator->chord();
		expect(ninth.size).to_equal(uint8_t{4});
		expect(ninth.steps[0]).to_equal(int8_t{0});
		expect(ninth.steps[3]).to_equal(int8_t{8}); // A Minor i at level 2: R ♭3 ♭7 9
	});
});

CPPSPEC_SPEC(pitch_stream)
