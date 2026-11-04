// Tests for the portable Generator core (src/deluge/generative/core/).

#include "CppUTest/TestHarness.h"
#include "generative/core/generator.h"
#include "generative/core/genre.h"

#include <cstdint>
#include <vector>

using namespace deluge::generative;

namespace {

struct Ev {
	bool on;
	int32_t tick;
	PartId part;
	uint8_t lane;
	int16_t note;
	uint8_t velocity;

	bool operator==(const Ev& o) const {
		return on == o.on && tick == o.tick && part == o.part && lane == o.lane && note == o.note
		       && velocity == o.velocity;
	}
	/// Same event, ignoring when it was delivered.
	bool sameContent(const Ev& o) const {
		return on == o.on && part == o.part && lane == o.lane && note == o.note && velocity == o.velocity;
	}
};

class RecordingSink : public EventSink {
public:
	std::vector<Ev> events;
	void noteOn(int32_t tick, PartId part, uint8_t lane, int16_t note, uint8_t velocity) override {
		events.push_back({true, tick, part, lane, note, velocity});
	}
	void noteOff(int32_t tick, PartId part, uint8_t lane, int16_t note) override {
		events.push_back({false, tick, part, lane, note, 0});
	}
};

constexpr int32_t kTicksPerBar = kStepsPerBar * kTicksPerStep;

/// Runs a fresh Generator for `bars` bars. `every` = 0 honours the returned waits; otherwise tick() is called
/// every `every` ticks regardless. The run ends with tick(end) and, unless `stop` is false, stop().
std::vector<Ev> play(const Recipe& recipe, uint32_t seed, int32_t bars, int32_t every = 0, bool stop = true) {
	static Generator gen;
	RecordingSink sink;
	gen.start(recipe, seed);
	int32_t end = bars * kTicksPerBar;
	int32_t now = 0;
	while (now < end) {
		int32_t wait = gen.tick(now, sink);
		CHECK(wait >= 1);
		now += every > 0 ? every : wait;
	}
	gen.tick(end, sink); // Every driver ends on the same tick, whatever its stride.
	if (stop) {
		gen.stop(sink);
	}
	return sink.events;
}

int32_t priority(const Ev& e) {
	return e.part == PartId::Drums ? e.lane : kNumLanes;
}

/// The step a note-on belongs to: the nearest grid line (Lean in this genre stays well inside ±12).
int32_t stepOf(int32_t tick) {
	return (tick + kTicksPerStep / 2) / kTicksPerStep;
}

int32_t countOns(const std::vector<Ev>& events, PartId part, int32_t lane = -1) {
	int32_t n = 0;
	for (const Ev& e : events) {
		if (e.on && e.part == part && (lane < 0 || e.lane == lane)) {
			n++;
		}
	}
	return n;
}

std::vector<Ev> onsOnly(const std::vector<Ev>& events) {
	std::vector<Ev> out;
	for (const Ev& e : events) {
		if (e.on) {
			out.push_back(e);
		}
	}
	return out;
}

constexpr int8_t kIntervals[3][7] = {
    {0, 2, 3, 5, 7, 8, 10},
    {0, 1, 3, 5, 7, 8, 10},
    {0, 2, 3, 5, 7, 9, 10},
};

} // namespace

TEST_GROUP(GenerativeCore){};

TEST(GenerativeCore, sameRecipeAndSeedGiveIdenticalStreams) {
	Recipe r;
	r.key = 5;
	r.scale = Scale::Phrygian;
	std::vector<Ev> a = play(r, 1234, 24);
	std::vector<Ev> b = play(r, 1234, 24);
	CHECK(a.size() > 100);
	CHECK(a == b);
}

TEST(GenerativeCore, differentSeedsDiffer) {
	Recipe r;
	std::vector<Ev> a = play(r, 1, 8);
	std::vector<Ev> b = play(r, 2, 8);
	CHECK(!(a == b));
}

TEST(GenerativeCore, tickingEveryTickMatchesHonouringWaits) {
	Recipe r;
	r.density = 50;
	r.motion = 50;
	std::vector<Ev> waits = play(r, 99, 24, 0);
	std::vector<Ev> every = play(r, 99, 24, 1);
	CHECK(waits == every);
}

TEST(GenerativeCore, skippedTicksAreCaughtUpInPlayOrder) {
	Recipe r;
	r.density = 50;
	std::vector<Ev> every = play(r, 7, 16, 1);
	for (int32_t stride : {2, 5, 37, 200, 1000}) {
		std::vector<Ev> jumpy = play(r, 7, 16, stride);
		LONGS_EQUAL(every.size(), jumpy.size());
		for (size_t i = 0; i < every.size(); i++) {
			CHECK(every[i].sameContent(jumpy[i]));
			CHECK(jumpy[i].tick >= every[i].tick); // Late, never early.
		}
	}
}

TEST(GenerativeCore, offsBeforeOnsAndOnsInRolePriorityWithinATick) {
	for (uint8_t density : {10, 25, 50}) {
		Recipe r;
		r.density = density;
		std::vector<Ev> events = play(r, 3, 32, 0, false);
		for (size_t i = 1; i < events.size(); i++) {
			const Ev& a = events[i - 1];
			const Ev& b = events[i];
			CHECK(b.tick >= a.tick);
			if (a.tick != b.tick) {
				continue;
			}
			CHECK(!(a.on && !b.on)); // No off after an on in the same tick.
			if (a.on && b.on) {
				CHECK(priority(a) <= priority(b));
			}
		}
	}
}

TEST(GenerativeCore, everyNoteOnIsClosedAndNoNoteIsRestartedWhileSounding) {
	Recipe r;
	r.density = 50;
	r.motion = 50;
	r.energy = 50;
	for (int32_t bars : {1, 3, 9, 17}) {
		std::vector<Ev> events = play(r, 42, bars, 1);
		std::vector<Ev> sounding;
		for (const Ev& e : events) {
			auto same = [&](const Ev& s) {
				return s.part == e.part && (e.part == PartId::Drums ? s.lane == e.lane : s.note == e.note);
			};
			size_t found = sounding.size();
			for (size_t i = 0; i < sounding.size(); i++) {
				if (same(sounding[i])) {
					found = i;
				}
			}
			if (e.on) {
				CHECK(found == sounding.size()); // A retrigger must be preceded by its note-off.
				CHECK(e.velocity >= 1 && e.velocity <= 127);
				sounding.push_back(e);
			}
			else {
				CHECK(found < sounding.size()); // No stray offs.
				sounding.erase(sounding.begin() + static_cast<long>(found));
			}
		}
		LONGS_EQUAL(0, sounding.size()); // stop() closed everything still sounding.
	}
}

TEST(GenerativeCore, stopMidNoteClosesEverythingAndClearsPending) {
	Recipe r;
	r.density = 50;
	static Generator gen;
	RecordingSink sink;
	gen.start(r, 5);
	for (int32_t now = 0; now <= 1000; now++) {
		gen.tick(now, sink);
	}
	gen.stop(sink);
	int32_t balance = 0;
	for (const Ev& e : sink.events) {
		balance += e.on ? 1 : -1;
	}
	LONGS_EQUAL(0, balance);
	size_t before = sink.events.size();
	gen.stop(sink); // Nothing left to stop.
	LONGS_EQUAL(before, sink.events.size());
}

TEST(GenerativeCore, leanStaysInRangeAndNothingPlaysEarly) {
	Recipe r;
	r.density = 50;
	static Generator gen;
	RecordingSink sink;
	gen.start(r, 11);
	for (int32_t now = 0; now < 16 * kTicksPerBar; now++) {
		size_t before = sink.events.size();
		gen.tick(now, sink);
		for (size_t i = before; i < sink.events.size(); i++) {
			LONGS_EQUAL(now, sink.events[i].tick);
		}
		// Every step up to the one decided at `now` is the only one that can have played.
		for (size_t i = before; i < sink.events.size(); i++) {
			const Ev& e = sink.events[i];
			if (!e.on) {
				continue;
			}
			int32_t step = stepOf(e.tick);
			int32_t lean = e.tick - step * kTicksPerStep;
			CHECK(lean >= -kMaxLean && lean <= kMaxLean);
			CHECK(e.tick >= 0);
			CHECK(e.tick >= step * kTicksPerStep - kLookaheadTicks); // Not before its decision point.
			if (e.part == PartId::Drums) {
				const DrumLaneTable& t = kGenreTechno.lanes[e.lane];
				int32_t expected = (step % kStepsPerBar) & 1 ? t.leanOffBeat : t.leanOnBeat;
				if (step == 0 && expected < 0) {
					expected = 0;
				}
				LONGS_EQUAL(expected, lean);
			}
		}
	}
	gen.stop(sink);
	// The genre's own Lean values are in range, and the hats really do push.
	for (const DrumLaneTable& t : kGenreTechno.lanes) {
		CHECK(t.leanOnBeat >= -kMaxLean && t.leanOnBeat <= kMaxLean);
		CHECK(t.leanOffBeat >= -kMaxLean && t.leanOffBeat <= kMaxLean);
	}
	CHECK(kGenreTechno.lanes[static_cast<int>(Lane::ClosedHat)].leanOffBeat < 0);
}

TEST(GenerativeCore, moreDensityMeansMoreNotesAndDensityZeroKeepsTheKick) {
	Recipe r;
	int32_t previous = -1;
	for (uint8_t density : {0, 10, 25, 40, 50}) {
		r.density = density;
		std::vector<Ev> events = play(r, 8, 16);
		int32_t total = static_cast<int32_t>(onsOnly(events).size());
		CHECK(total >= previous);
		previous = total;
		if (density == 0) {
			CHECK(countOns(events, PartId::Drums, static_cast<int>(Lane::Kick)) >= 16 * 4 - 2 * 2);
		}
	}
	r.density = 0;
	int32_t sparse = static_cast<int32_t>(onsOnly(play(r, 8, 16)).size());
	r.density = 50;
	int32_t dense = static_cast<int32_t>(onsOnly(play(r, 8, 16)).size());
	CHECK(sparse < dense);
	CHECK(countOns(play(r, 8, 16), PartId::Drums, static_cast<int>(Lane::OpenHat)) > 0);
}

TEST(GenerativeCore, kickOnEveryBeatExceptThePhraseEndDrop) {
	const int32_t phraseBars = kGenreTechno.phraseBars;
	const DrumLaneTable& kick = kGenreTechno.lanes[static_cast<int>(Lane::Kick)];
	for (uint32_t seed : {1u, 2u, 3u, 77u}) {
		Recipe r;
		r.density = static_cast<uint8_t>(seed * 13 % 51);
		std::vector<Ev> events = play(r, seed, 4 * phraseBars);
		std::vector<bool> kickOnBeat(4 * phraseBars * 4 + 1, false); // +1: the run ends with tick(end).
		for (const Ev& e : events) {
			if (e.on && e.part == PartId::Drums && e.lane == static_cast<uint8_t>(Lane::Kick)) {
				LONGS_EQUAL(0, e.tick % (kTicksPerStep * 4)); // Only on beats, on the grid.
				kickOnBeat[e.tick / (kTicksPerStep * 4)] = true;
			}
		}
		for (int32_t phrase = 0; phrase < 4; phrase++) {
			int32_t firstBeat = phrase * phraseBars * 4;
			int32_t lastBeat = firstBeat + phraseBars * 4 - 1;
			for (int32_t beat = firstBeat; beat <= lastBeat - kick.phraseEndDropBeatsMax; beat++) {
				CHECK(kickOnBeat[beat]);
			}
			int32_t dropped = 0;
			for (int32_t beat = lastBeat - kick.phraseEndDropBeatsMax + 1; beat <= lastBeat; beat++) {
				dropped += kickOnBeat[beat] ? 0 : 1;
			}
			CHECK(!kickOnBeat[lastBeat]);
			CHECK(dropped >= kick.phraseEndDropBeatsMin && dropped <= kick.phraseEndDropBeatsMax);
		}
	}
}

TEST(GenerativeCore, clapsOnTwoAndFour) {
	Recipe r;
	std::vector<Ev> events = play(r, 21, 8);
	int32_t claps = 0;
	for (const Ev& e : events) {
		if (e.on && e.part == PartId::Drums && e.lane == static_cast<uint8_t>(Lane::Clap)) {
			int32_t stepInBar = (e.tick / kTicksPerStep) % kStepsPerBar;
			CHECK(stepInBar == 4 || stepInBar == 12);
			claps++;
		}
	}
	LONGS_EQUAL(16, claps);
}

TEST(GenerativeCore, bassNotesAreInScaleAndInRegister) {
	const LoopTable& bass = kGenreTechno.bass;
	int32_t lo = bass.registerAnchor + bass.spanLow;
	int32_t hi = bass.registerAnchor + bass.spanHigh;
	for (int32_t scale = 0; scale < 3; scale++) {
		for (uint8_t key = 0; key < 12; key++) {
			Recipe r;
			r.key = key;
			r.scale = static_cast<Scale>(scale);
			r.density = 50;
			r.motion = 50;
			std::vector<Ev> events = play(r, 1000 + key, 24);
			int32_t notes = 0;
			for (const Ev& e : events) {
				if (!e.on || e.part != PartId::Bass) {
					continue;
				}
				notes++;
				CHECK(e.note >= lo && e.note <= hi);
				LONGS_EQUAL(0, e.lane);
				int32_t pc = ((e.note - key) % 12 + 12) % 12;
				bool inScale = false;
				for (int8_t interval : kIntervals[scale]) {
					inScale = inScale || interval == pc;
				}
				CHECK(inScale);
			}
			CHECK(notes > 50);
		}
	}
}

TEST(GenerativeCore, motionZeroRepeatsPhrasesAndMotionMutatesThem) {
	const int32_t phraseTicks = kGenreTechno.phraseBars * kTicksPerBar;
	auto bassOfPhrase = [&](const std::vector<Ev>& events, int32_t phrase) {
		std::vector<Ev> out;
		for (const Ev& e : events) {
			if (e.on && e.part == PartId::Bass && e.tick >= phrase * phraseTicks
			    && e.tick < (phrase + 1) * phraseTicks) {
				Ev shifted = e;
				shifted.tick -= phrase * phraseTicks;
				out.push_back(shifted);
			}
		}
		return out;
	};
	Recipe r;
	r.motion = 0;
	std::vector<Ev> still = play(r, 5, 4 * kGenreTechno.phraseBars);
	// Phrase 0 is compared only from phrase 1 on: a tie from the loop's last step can rest every later phrase's
	// first step, but nothing comes before the piece's very first one.
	CHECK(bassOfPhrase(still, 1).size() > 20);
	for (int32_t p = 2; p < 4; p++) {
		CHECK(bassOfPhrase(still, 1) == bassOfPhrase(still, p));
	}
	r.motion = 50;
	std::vector<Ev> moving = play(r, 5, 4 * kGenreTechno.phraseBars);
	CHECK(bassOfPhrase(moving, 0) == bassOfPhrase(still, 0)); // Same starting loop.
	bool changed = false;
	for (int32_t p = 1; p < 4; p++) {
		changed = changed || !(bassOfPhrase(moving, 0) == bassOfPhrase(moving, p));
	}
	CHECK(changed);
}

TEST(GenerativeCore, setRecipeMidRunOnlyAffectsUndecidedSteps) {
	Recipe before;
	before.density = 15;
	before.energy = 10;
	Recipe after = before;
	after.density = 50;
	after.energy = 50;
	const int32_t bars = 4;
	const int32_t changeAt = 2 * kTicksPerBar + 30;

	static Generator gen;
	RecordingSink sink;
	gen.start(before, 9);
	for (int32_t now = 0; now <= bars * kTicksPerBar; now++) { // play() also ends with tick(end).
		gen.tick(now, sink);
		if (now == changeAt) {
			gen.setRecipe(after);
		}
	}
	gen.stop(sink);
	std::vector<Ev> steered = onsOnly(sink.events);
	std::vector<Ev> baseline = onsOnly(play(before, 9, bars, 1));
	std::vector<Ev> fresh = onsOnly(play(after, 9, bars, 1));

	auto select = [](const std::vector<Ev>& events, bool decided, bool drumsOnly) {
		std::vector<Ev> out;
		for (const Ev& e : events) {
			int32_t step = stepOf(e.tick);
			bool d = step <= (changeAt + kLookaheadTicks) / kTicksPerStep; // Decided by the time of the change.
			if (d == decided && (!drumsOnly || e.part == PartId::Drums)) {
				out.push_back(e);
			}
		}
		return out;
	};
	// Steps already decided when setRecipe arrived play exactly as without the change.
	CHECK(select(steered, true, false) == select(baseline, true, false));
	// Later steps follow the new Recipe: the drums match a run that had it from the start.
	CHECK(select(steered, false, true) == select(fresh, false, true));
	CHECK(!(select(steered, false, false) == select(baseline, false, false)));
}

TEST(GenerativeCore, currentStepFollowsPlayback) {
	Recipe r;
	static Generator gen;
	RecordingSink sink;
	gen.start(r, 1);
	LONGS_EQUAL(-1, gen.currentStep());
	for (int32_t now = 0; now < 10 * kTicksPerStep; now++) {
		gen.tick(now, sink);
		CHECK(gen.currentStep() >= now / kTicksPerStep);
		CHECK(gen.currentStep() <= (now + kLookaheadTicks) / kTicksPerStep);
	}
}
