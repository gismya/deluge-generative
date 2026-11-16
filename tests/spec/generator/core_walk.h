// A piece walked straight through the core, step by step, with where the Arc and the chord walk stood at each step:
// what the Chord-graph specs share (chord_stream_spec, melodic_techno_spec). harness.h runs a piece through the driver.

#pragma once

#include "harness.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace harness {

/// What each decided step played: the chord, where the Arc stood, and its note-ons.
struct Walked {
	struct Step {
		int32_t chord = 0;
		SectionKind section = SectionKind::Intro;
		bool sectionStart = false;
		bool drop = false;
		int32_t stepInPhrase = 0;
		int32_t phraseInSection = 0;
		std::vector<uint8_t> notes; ///< The pitches of the note-ons that start before the next step.
	};
	std::vector<Step> steps;
	std::vector<Event> noteOns; ///< Every note-on, in the order the core sent them.
	std::vector<Event> notes;   ///< Note-ons and note-offs, in the order they play.
	std::vector<Event> keys;    ///< KeyScale events, in the order they play.
	uint32_t dropped = 0;       ///< Events a full buffer dropped.
	uint32_t evicted = 0;       ///< Notes a full sounding-note table ended early.
	std::unique_ptr<Generator> generator;

	/// The steps where the chord differs from the step before.
	[[nodiscard]] std::vector<int32_t> changes() const {
		std::vector<int32_t> out;
		for (size_t n = 1; n < steps.size(); n++) {
			if (steps[n].chord != steps[n - 1].chord) {
				out.push_back(static_cast<int32_t>(n));
			}
		}
		return out;
	}
};

using WalkSteer = std::function<void(int32_t, const Generator&, SteeringQueue&)>;

/// Decides up to `count` steps of `recipe` straight through the core, with `steer` pushing commands before each, as
/// the Generator stands after the step before. Stops early once the piece has ended (an End's Outro is over).
/// `skipIntro` starts in a Groove, as SHIFT+PLAY does; `takes` are the parts' takes (spec §6.6).
inline Walked walk(const Recipe& recipe, uint32_t seed, int32_t count, const WalkSteer& steer = {},
                   bool skipIntro = false, const Takes& takes = Takes{}) {
	Walked out;
	out.generator = std::make_unique<Generator>();
	Generator& generator = *out.generator;
	generator.start(recipe, seed, takes, skipIntro);
	SteeringQueue queue;
	for (int32_t n = 0; n < count && !generator.ended(); n++) {
		if (steer) {
			steer(n, generator, queue);
		}
		generator.decideNextStep(queue, nullptr);
		const Arc& arc = generator.arc();
		Walked::Step step{generator.chordRoot(), arc.section(),      arc.sectionStartsHere(),
		                  arc.dropStartsHere(),  arc.stepInPhrase(), arc.phraseInSection()};
		Event event;
		while (generator.popDue(INT32_MAX, &event)) {
			if (event.kind == EventKind::NoteOn || event.kind == EventKind::NoteOff) {
				out.notes.push_back(event);
			}
			if (event.kind == EventKind::KeyScale) {
				out.keys.push_back(event);
			}
			if (event.kind != EventKind::NoteOn) {
				continue;
			}
			out.noteOns.push_back(event);
			if (event.tick < stepTick(n + 1)) {
				step.notes.push_back(event.pitch);
			}
		}
		out.steps.push_back(step);
	}
	out.dropped = generator.droppedEvents();
	out.evicted = generator.evictedNotes();
	return out;
}

/// The step a note-on belongs to: the nearest grid line, since Lean moves it at most half a step.
inline int32_t stepOf(const Event& event) {
	return (event.tick + kTicksPerStep / 2) / kTicksPerStep;
}

} // namespace harness
