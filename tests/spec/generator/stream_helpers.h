// What specs share to steer a piece and read its stream (spec §21.2): commands, genres and filters that used to be
// copied from spec to spec. harness.h runs the piece.

#pragma once

#include "core_walk.h"
#include "generative/core/variants.h"
#include "harness.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace harness {

// Genres.

/// A genre's new Recipe with Motion high enough that the Arc reaches a Break, a Build and a Drop within a walk (spec
/// §9.3), its key root from the entropy.
inline Recipe movingRecipe(const Genre& genre, uint32_t entropy, int32_t density = 25, int32_t complexity = 25) {
	Recipe recipe = newRecipe(genre, entropy);
	recipe.keyRoot = static_cast<uint8_t>(entropy % 12);
	recipe.macros.set(Macro::Motion, macroPoints(50));
	recipe.macros.set(Macro::Density, macroPoints(density));
	recipe.macros.set(Macro::Complexity, macroPoints(complexity));
	return recipe;
}
/// `recipe` with its genre's variant sets fixed by `fixes`, `set=name` each (the tuning tools' --variant, spec §3.6).
inline Recipe withVariants(Recipe recipe, std::initializer_list<const char*> fixes) {
	for (const char* fix : fixes) {
		fixVariant(recipe.variants, parseVariantFix(*recipe.genre, fix).value());
	}
	return recipe;
}

/// `recipe` with every variant set of its genre fixed to its first variant: the genre as written (spec §3.6).
inline Recipe withFirstVariants(Recipe recipe) {
	for (size_t s = 0; s < recipe.genre->variantSets.size() && s < static_cast<size_t>(kMaxVariantSets); s++) {
		fixVariant(recipe.variants, {.set = static_cast<int32_t>(s), .variant = 0});
	}
	return recipe;
}

/// movingRecipe() in `scale`.
inline Recipe movingRecipe(const Genre& genre, Scale scale, uint32_t entropy, int32_t density = 25,
                           int32_t complexity = 25) {
	Recipe recipe = movingRecipe(genre, entropy, density, complexity);
	recipe.scale = scale;
	return recipe;
}

/// What whole pieces lost to full buffers.
struct Lost {
	uint32_t dropped = 0; ///< Events a full buffer dropped.
	uint32_t evicted = 0; ///< Notes a full sounding-note table ended early.
};
/// Walks 128-bar pieces of `genre` (seed 2) at every corner of Complexity and Density (0, 25, 50), each on a
/// movingRecipe() of entropy complexity + density + 1, in each of `scales` (none: the scale each Recipe rolls), and
/// sums what they lost (spec §2.7: the capacities hold every step).
inline Lost lostAtEveryCorner(const Genre& genre, std::span<const Scale> scales = {}) {
	Lost lost;
	const auto walkCorners = [&](const Scale* scale) {
		for (int32_t complexity : {0, 25, 50}) {
			for (int32_t density : {0, 25, 50}) {
				const auto entropy = static_cast<uint32_t>(complexity + density + 1);
				Recipe recipe = movingRecipe(genre, entropy, density, complexity);
				recipe.scale = scale != nullptr ? *scale : recipe.scale;
				const Walked walked = walk(recipe, 2, 128 * kStepsPerBar);
				lost.dropped += walked.dropped;
				lost.evicted += walked.evicted;
			}
		}
	};
	if (scales.empty()) {
		walkCorners(nullptr);
	}
	for (const Scale& scale : scales) {
		walkCorners(&scale);
	}
	return lost;
}

/// A Kit part's lane spec. Throws if the part has no such lane.
inline const LaneSpec& laneOf(const Genre& genre, int32_t part, Lane lane) {
	const int32_t l = laneIndexOf(genre.parts[part], lane);
	if (l < 0) {
		throw "no such lane";
	}
	return genre.parts[part].lanes[l];
}

/// Respond's count as the spec reads it (§5.5): the target's planned onsets in `step`'s half of its bar and in the
/// other half, at `density`. For a spec's Generator::testDecided_.
struct RespondHalves {
	int32_t mine = 0;
	int32_t other = 0;
	bool inFirst = false;
	/// Against: the busier half, the first on a non-zero tie.
	[[nodiscard]] bool against() const { return mine > other || (inFirst && mine == other && mine > 0); }
};
inline RespondHalves respondHalves(const Generator& g, const RuleTarget& target, int32_t step, int32_t density) {
	const int32_t bar = step - step % kStepsPerBar;
	auto count = [&](int32_t from) {
		int32_t n = 0;
		for (int32_t s = from; s < from + kHalfBarSteps; s++) {
			n += g.testTargetPlanned(target, s, density) ? 1 : 0;
		}
		return n;
	};
	const int32_t first = count(bar);
	const int32_t second = count(bar + kHalfBarSteps);
	const bool inFirst = halfOf(step) == 0;
	return {.mine = inFirst ? first : second, .other = inFirst ? second : first, .inFirst = inFirst};
}

/// While it lives, Generator::testDecided_ calls the derived spec struct's decided(generator, step, density) after
/// each decided step: `struct Seen : DecidedHook<Seen> { void decided(...); };`. One at a time.
template <typename Derived>
struct DecidedHook {
	DecidedHook() {
		current = static_cast<Derived*>(this);
		Generator::testDecided_ = [](const Generator& g, int32_t step, int32_t density) {
			current->decided(g, step, density);
		};
	}
	~DecidedHook() {
		Generator::testDecided_ = nullptr;
		current = nullptr;
	}
	DecidedHook(const DecidedHook&) = delete;
	DecidedHook& operator=(const DecidedHook&) = delete;

private:
	static inline Derived* current = nullptr;
};

// Commands.

/// A press on a MUTE pad: lands on the next bar line, or with `now` (SHIFT held) on the next undecided step.
inline Command mute(uint8_t part, bool now = false) {
	return {CommandKind::SetMute, part, now ? kPressNow : kPress};
}
/// A forced Section, from the next phrase line.
inline Command force(SectionKind kind) {
	return {CommandKind::ForceSection, static_cast<uint8_t>(kind), 0};
}

/// Plays Techno from its first Groove (seed 3), so every part plays from bar 1, pushing each command when the driver
/// is about to decide its step.
inline Result playSteered(int32_t bars, std::vector<std::pair<int32_t, Command>> commands, SteeringLog* log = nullptr) {
	Run run;
	run.recipe = acidRecipe();
	run.seed = 3;
	run.skipIntro = true;
	run.log = log;
	run.steer = steerAt(std::move(commands));
	return play(run, bars);
}

// Genres.

/// Techno with a fixed Build length.
template <uint8_t Bars>
struct BuildOf {
	static constexpr WeightedBars kBars[] = {{Bars, 1}};
	static inline const Genre genre = [] {
		Genre g = genres::technoAcid;
		g.arc.buildBars = kBars;
		return g;
	}();
};

/// A Bass of long notes for a spec's own genre: one every 2 bars, lasting `Steps`, its part gated at `Gate` points.
template <int32_t Steps, int32_t Gate = 0>
struct LongBassParts {
	static constexpr Pattern kEveryTwoBars[] = {
	    {"X...............................", "................................", 0}};
	static constexpr LineWeight kRoot[] = {{0, 1}};
	static constexpr PartSpec parts[] = {{
	    .role = Role::Bass,
	    .flavour = Flavour::Loop,
	    .pool = kEveryTwoBars,
	    .densityGate = macroPoints(Gate),
	    .loop =
	        {.degrees = kRoot, .draws = {.octaveUp = 0, .accent = 0}, .slide = 0, .gateTicks = Steps * kTicksPerStep},
	}};
};

// Reading a stream.

/// Note-ons of `part` (and `lane`, unless Lane::None stands for any) from step `from` up to `to`.
inline std::vector<Event> ons(const Result& result, uint8_t part, Lane lane, int32_t from, int32_t to) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.part == part && (lane == Lane::None || e.lane == lane) && e.tick >= stepTick(from)
		    && e.tick < stepTick(to)) {
			out.push_back(e);
		}
	}
	return out;
}

/// The ticks any of `events` is on.
inline std::set<int32_t> ticksOf(const std::vector<Event>& events) {
	std::set<int32_t> out;
	for (const Event& e : events) {
		out.insert(e.tick);
	}
	return out;
}

/// The first tick at or after `from` a Section of `kind` starts, or -1.
inline int32_t sectionStart(const Result& result, SectionKind kind, int32_t from = 0) {
	for (const Event& e : only(result.events, EventKind::Section)) {
		if (e.section == kind && e.tick >= from) {
			return e.tick;
		}
	}
	return -1;
}

/// The sound-control events.
inline std::vector<Event> controls(const Result& result) {
	return only(result.events, EventKind::Control);
}

/// The last Control event for (part, lane, control) at or before `tick`, or nullptr (home).
inline const Event* lastAt(const Result& result, uint8_t part, Lane lane, SoundControl control, int32_t tick) {
	const Event* last = nullptr;
	for (const Event& e : result.events) {
		if (e.kind == EventKind::Control && e.tick <= tick && e.part == part && e.lane == lane
		    && e.control == control) {
			last = &e;
		}
	}
	return last;
}
/// The last offset sent for (part, lane, control) at or before `tick`, or 0 (home) if none was.
inline int32_t offsetAt(const Result& result, uint8_t part, Lane lane, SoundControl control, int32_t tick) {
	const Event* e = lastAt(result, part, lane, control, tick);
	return e != nullptr ? e->offset : 0;
}

/// A note from its note-on to its note-off.
struct Held {
	int32_t on = 0;
	int32_t off = 0;
	uint8_t pitch = 0;
};

/// `part`'s notes among `notes` (note-ons and note-offs in the order they play), each paired from its note-on to its
/// note-off, in onset order. A note still sounding at the end runs to INT32_MAX.
inline std::vector<Held> heldNotes(const std::vector<Event>& notes, int32_t part) {
	std::vector<Held> out;
	std::map<uint8_t, size_t> open;
	for (const Event& e : notes) {
		if (e.part != part) {
			continue;
		}
		if (e.kind == EventKind::NoteOn) {
			open[e.pitch] = out.size();
			out.push_back({e.tick, INT32_MAX, e.pitch});
		}
		else if (e.kind == EventKind::NoteOff && open.contains(e.pitch)) {
			out[open[e.pitch]].off = e.tick;
			open.erase(e.pitch);
		}
	}
	return out;
}

/// A note-on the mute gate closed: sent with its muted flag (spec §12.5).
inline Event mutedNoteOn(int32_t tick, uint8_t part, Lane lane, uint8_t pitch) {
	Event e = Event::noteOn(tick, part, lane, pitch, 100);
	e.muted = true;
	return e;
}

} // namespace harness
