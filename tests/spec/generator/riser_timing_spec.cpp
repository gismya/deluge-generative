// A sample Riser ends on the Drop (spec §9.5, §10.5): the Perform driver holds a sample Riser's note back until the
// time left to the Drop, at the tempo planned across the Build, is the sample's length. A synth Riser, or a sample
// longer than the Build, starts on the Build's first step as the core places it.
//
// These specs measure the time from the note-on to the Drop independently of the driver: a sink logs every tempo the
// driver writes, and the specs add up each Generator tick's length at the tempo it played at.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <cmath>
#include <memory>
#include <vector>

using namespace harness;

namespace {

/// One event as played, on one timeline: Generator ticks since the first piece's step 0.
struct Played {
	Event event;
	int32_t at = 0;
};

/// How long one Generator tick lasts at `tempo` (hundredths of a BPM), in microseconds: 96 ticks a beat.
double tickMicros(int32_t tempo) {
	return 60e6 / (tempo / 100.0) / (4 * kTicksPerStep);
}

/// One 16th at `tempo`, in microseconds: the timing the ticket asks for.
double sixteenthMicros(int32_t tempo) {
	return tickMicros(tempo) * kTicksPerStep;
}

/// Records what the driver plays and the tempo it writes, and answers for the Riser's sample length.
struct RiserSink final : perform::NoteSink {
	const perform::PerformDriver* driver = nullptr;
	int32_t riserMicros = 0; ///< The Riser's sample length; 0 for a synth Riser.
	int32_t startTempo = 0;
	int32_t base = 0;
	int32_t outgoingBase = 0;
	std::vector<Played> played;
	std::vector<std::pair<int32_t, int32_t>> tempos; ///< {timeline tick, tempo}

	void attach(const perform::PerformDriver& playing, const Hook& /*hook*/) { driver = &playing; }
	void play(const Event& event) override { played.push_back({event, event.tick + base}); }
	void playOutgoing(const Event& event) override { played.push_back({event, event.tick + outgoingBase}); }
	void transitionStarts(int32_t startTick) override {
		outgoingBase = base;
		base += startTick;
	}
	/// An external clock: the master owns tempo, so writes are dropped, as OutputSink does (spec §9.7, §12.7).
	bool master = false;
	void setTempo(int32_t tempo) override {
		if (!master) {
			tempos.emplace_back(driver->clock() + base, tempo);
		}
	}
	bool tempoFollowsMaster() override { return master; }
	int32_t sampleMicros(const Event& noteOn) override { return noteOn.lane == Lane::Riser ? riserMicros : 0; }

	[[nodiscard]] int32_t tempoAt(int32_t tick) const {
		int32_t tempo = startTempo;
		for (const auto& [at, t] : tempos) {
			if (at > tick) {
				break;
			}
			tempo = t;
		}
		return tempo;
	}
	/// Microseconds from `from` to `to` on the timeline, each tick at the tempo written for it.
	[[nodiscard]] double micros(int32_t from, int32_t to) const {
		double sum = 0;
		for (int32_t tick = from; tick < to; tick++) {
			sum += tickMicros(tempoAt(tick));
		}
		return sum;
	}
	/// The Riser's events of `kind`, in play order.
	[[nodiscard]] std::vector<Played> riser(EventKind kind) const {
		std::vector<Played> out;
		for (const Played& p : played) {
			if (p.event.kind == kind && p.event.lane == Lane::Riser) {
				out.push_back(p);
			}
		}
		return out;
	}
	/// The first Section event of `kind` at or after `from`, or a default one at -1.
	[[nodiscard]] Played section(SectionKind kind, int32_t from = 0) const {
		for (const Played& p : played) {
			if (p.event.kind == EventKind::Section && p.event.section == kind && p.at >= from) {
				return p;
			}
		}
		return {Event{}, -1};
	}
};

constexpr Command kForceBuild = {CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 0};
constexpr Command kForceGroove = {CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Groove), 0};

/// Techno from its first Groove at `tempo`, with a Build forced on step 1 (unless `forceBuild` is false) and a Riser
/// of `riserMicros`.
struct Piece : StepwisePiece<RiserSink> {
	std::unique_ptr<Generator> incoming = std::make_unique<Generator>();

	Piece(const Genre& genre, int32_t tempo, int32_t riserMicros, bool forceBuild = true)
	    : StepwisePiece(startOf(genre, tempo)) {
		sink.riserMicros = riserMicros;
		sink.startTempo = tempo;
		driver.setTempo(tempo, sink);
		sink.tempos.clear();
		runToStepLine(1, -1);
		if (forceBuild) {
			push(kForceBuild);
		}
	}
	static Start startOf(const Genre& genre, int32_t tempo) {
		Start start{.seed = 3, .skipIntro = true};
		start.recipe.genre = &genre;
		start.recipe.tempo = tempo;
		return start;
	}
	/// A Regenerate into a Drop at `tempo`, loaded now: the transition's Build glides to it (spec §9.7).
	void transition(int32_t tempo) {
		Recipe recipe = generator.recipe();
		recipe.tempo = tempo;
		incoming->startIncoming(recipe, 7, Takes{}, TransitionStyle::Drop);
		driver.queueIncoming(*incoming, kStepsPerBar * kTicksPerStep);
		push({CommandKind::Transition, static_cast<uint8_t>(TransitionStyle::Drop), 0});
	}
	/// Runs until the forced Build's first step has played.
	void reachBuild() {
		hook.runUntil([&] { return build().at >= 0; });
	}
	/// The forced Build's first step, once it has played.
	[[nodiscard]] Played build() const { return sink.section(SectionKind::Build); }
	/// The Drop that ends it.
	[[nodiscard]] Played drop() const { return sink.section(SectionKind::Groove, build().at + 1); }
};

/// Where the Riser's sample ended against the Drop at `dropAt`.
struct Landing {
	size_t ons = 0;
	size_t offs = 0;
	int32_t offAt = -1;
	double error = 1e12; ///< |time from the note-on to the Drop - the sample's length|, in microseconds
};

Landing landing(const RiserSink& sink, int32_t dropAt) {
	std::vector<Played> ons = sink.riser(EventKind::NoteOn);
	std::vector<Played> offs = sink.riser(EventKind::NoteOff);
	Landing out{ons.size(), offs.size()};
	if (!ons.empty() && !offs.empty()) {
		out.offAt = offs[0].at;
		out.error = std::abs(sink.micros(ons[0].at, dropAt) - sink.riserMicros);
	}
	return out;
}

// The Riser's note-on lands its sample's end on the Drop at `dropAt`, within `tolerance` microseconds, and its
// note-off is on the Drop.
#define expectEndsOnDrop(sink, dropAt, tolerance)                                                                      \
	do {                                                                                                               \
		const Landing l = landing(sink, dropAt);                                                                       \
		expect(l.ons).to_equal(1U);                                                                                    \
		expect(l.offs).to_equal(1U);                                                                                   \
		expect(l.offAt).to_equal(dropAt);                                                                              \
		expect(l.error).to_be_less_than(tolerance);                                                                    \
	} while (false)

constexpr int32_t kSeconds = 1000000;

} // namespace

// clang-format off
describe riser_timing("Riser timing", $ {
	it("ends on the Drop at 125 and 135 BPM, in 8- and 16-bar Builds", _ {
		for (int32_t tempo : {12500, 13500}) {
			for (int32_t seconds : {4, 6, 8}) {
				{
					Piece p(BuildOf<8>::genre, tempo, seconds * kSeconds);
					p.runBars(48);
					expect(p.drop().at - p.build().at).to_equal(8 * kStepsPerBar * kTicksPerStep);
					expectEndsOnDrop(p.sink, p.drop().at, sixteenthMicros(tempo));
				}
				{
					Piece p(BuildOf<16>::genre, tempo, seconds * kSeconds);
					p.runBars(64);
					expect(p.drop().at - p.build().at).to_equal(16 * kStepsPerBar * kTicksPerStep);
					expectEndsOnDrop(p.sink, p.drop().at, sixteenthMicros(tempo));
				}
			}
		}
	});

	it("ends on the Drop of a transition's Build, across its tempo glide", _ {
		for (auto [from, to] : {std::pair{12500, 13500}, std::pair{13500, 12500}}) {
			for (int32_t seconds : {4, 8}) {
				Piece p(BuildOf<8>::genre, from, seconds * kSeconds, false);
				p.transition(to);
				p.runBars(48);
				expect(p.driver.playing() == p.incoming.get()).to_be_true();
				// The tempo glided across the Build: the Riser plays at more than one tempo.
				const std::vector<Played> ons = p.sink.riser(EventKind::NoteOn);
				expect(ons.empty()).to_be_false();
				if (!ons.empty()) {
					expect(p.sink.tempoAt(ons[0].at)).not_().to_equal(p.sink.tempoAt(p.drop().at - 1));
				}
				expect(p.sink.tempoAt(p.drop().at)).to_equal(to);
				expectEndsOnDrop(p.sink, p.drop().at, sixteenthMicros(std::max(from, to)));
			}
		}
	});

	it("ends on the Drop of a transition's Build under an external clock, where nothing glides", _ {
		for (auto [from, to] : {std::pair{12500, 13500}, std::pair{13500, 12500}}) {
			Piece p(BuildOf<8>::genre, from, 8 * kSeconds, false);
			p.sink.master = true;
			p.transition(to);
			p.runBars(48);
			expect(p.driver.playing() == p.incoming.get()).to_be_true();
			expect(p.sink.tempos.empty()).to_be_true(); // the master's tempo throughout
			expectEndsOnDrop(p.sink, p.drop().at, sixteenthMicros(from));
		}
	});

	it("starts late, after the Build's first step", _ {
		Piece p(BuildOf<8>::genre, 12500, 4 * kSeconds);
		p.runBars(48);
		std::vector<Played> ons = p.sink.riser(EventKind::NoteOn);
		expect(ons.empty()).to_be_false();
		if (!ons.empty()) {
			expect(ons[0].at).to_be_greater_than(p.build().at + 4 * kStepsPerBar * kTicksPerStep);
			expect(ons[0].event.tick).to_equal(ons[0].at); // it plays at the tick it says
		}
	});

	it("ends on the Drop when an F4 tempo glide runs across the Build", _ {
		Piece p(BuildOf<16>::genre, 12500, 8 * kSeconds);
		p.reachBuild();
		const int32_t buildAt = p.build().at;
		expect(buildAt).to_be_greater_than(-1);
		// Two bars into the Build, a glide to 135 BPM over 8 bars: it ends before the Riser starts.
		const int64_t at = buildAt + 2 * kStepsPerBar * kTicksPerStep;
		p.runToTick(static_cast<int32_t>(at) - 1);
		p.driver.glideTempo(13500, 8 * kStepsPerBar * kTicksPerStep, p.sink);
		p.hook.wake();
		p.runBars(32);
		expectEndsOnDrop(p.sink, p.drop().at, sixteenthMicros(13500));
		// And one that is still gliding when the Riser starts.
		Piece q(BuildOf<16>::genre, 13500, 8 * kSeconds);
		q.reachBuild();
		q.runToTick(q.build().at + 6 * kStepsPerBar * kTicksPerStep);
		q.driver.glideTempo(12500, 10 * kStepsPerBar * kTicksPerStep, q.sink);
		q.hook.wake();
		q.runBars(32);
		expectEndsOnDrop(q.sink, q.drop().at, sixteenthMicros(12500));
	});

	it("starts on the Build's first step when it is longer than the Build", _ {
		Piece p(BuildOf<8>::genre, 12500, 40 * kSeconds);
		p.runBars(48);
		std::vector<Played> ons = p.sink.riser(EventKind::NoteOn);
		expect(ons.size()).to_equal(1U);
		if (!ons.empty()) {
			expect(ons[0].at).to_equal(p.build().at);
		}
	});

	it("never sounds when a forced Groove cuts the Build before it starts", _ {
		Piece p(BuildOf<16>::genre, 12500, 4 * kSeconds);
		p.reachBuild();
		const int32_t buildAt = p.build().at;
		p.runToTick(buildAt + 2 * kStepsPerBar * kTicksPerStep);
		p.push(kForceGroove);
		p.runBars(32);
		const Played groove = p.sink.section(SectionKind::Groove, buildAt + 1);
		expect(groove.at).to_be_less_than(buildAt + 16 * kStepsPerBar * kTicksPerStep);
		expect(p.sink.riser(EventKind::NoteOn).empty()).to_be_true();
		expect(p.sink.riser(EventKind::NoteOff).empty()).to_be_true();
	});

	it("rings out from the forced line when a forced Groove cuts it after it started", _ {
		Piece p(BuildOf<16>::genre, 12500, 20 * kSeconds);
		p.reachBuild();
		const int32_t buildAt = p.build().at;
		p.runToTick(buildAt + 7 * kStepsPerBar * kTicksPerStep);
		expect(p.sink.riser(EventKind::NoteOn).size()).to_equal(1U);
		p.push(kForceGroove);
		p.runBars(32);
		const Played groove = p.sink.section(SectionKind::Groove, buildAt + 1);
		std::vector<Played> offs = p.sink.riser(EventKind::NoteOff);
		expect(offs.size()).to_equal(1U);
		if (!offs.empty()) {
			expect(offs[0].at).to_equal(groove.at);
		}
	});

	it("never sounds after a jump before it starts", _ {
		Piece p(BuildOf<16>::genre, 12500, 4 * kSeconds);
		p.reachBuild();
		p.runToTick(p.build().at + 2 * kStepsPerBar * kTicksPerStep);
		p.driver.jump(p.sink);
		p.runBars(32);
		expect(p.sink.riser(EventKind::NoteOn).empty()).to_be_true();
	});

	it("leaves the Riser's sound controls as a synth Riser's", _ {
		Piece sample(BuildOf<8>::genre, 12500, 4 * kSeconds);
		Piece synth(BuildOf<8>::genre, 12500, 0);
		sample.runBars(48);
		synth.runBars(48);
		auto controls = [](const RiserSink& sink) {
			std::vector<Played> out;
			for (const Played& p : sink.played) {
				if (p.event.kind == EventKind::Control) {
					out.push_back(p);
				}
			}
			return out;
		};
		std::vector<Played> a = controls(sample.sink);
		std::vector<Played> b = controls(synth.sink);
		expect(a.size()).to_equal(b.size());
		bool riserMoves = false;
		for (size_t i = 0; i < a.size() && i < b.size(); i++) {
			expect(a[i].event == b[i].event).to_be_true();
			expect(a[i].at).to_equal(b[i].at);
			riserMoves |= a[i].event.lane == Lane::Riser && a[i].at > sample.build().at;
		}
		expect(riserMoves).to_be_true(); // Rise moves the Riser's row across the Build
	});

	it("plays a synth Riser exactly as before: from the Build's first step to the Drop", _ {
		Piece p(BuildOf<8>::genre, 12500, 0);
		p.runBars(48);
		std::vector<Played> ons = p.sink.riser(EventKind::NoteOn);
		std::vector<Played> offs = p.sink.riser(EventKind::NoteOff);
		expect(ons.size()).to_equal(1U);
		expect(offs.size()).to_equal(1U);
		if (!ons.empty() && !offs.empty()) {
			expect(ons[0].at).to_equal(p.build().at);
			expect(offs[0].at).to_equal(p.drop().at);
		}
	});

	it("plays a synth Riser's piece as a sink that knows no sample lengths hears it", _ {
		Recipe recipe = technoRecipe();
		recipe.genre = &BuildOf<8>::genre;
		Run run;
		run.recipe = recipe;
		run.seed = 3;
		run.skipIntro = true;
		run.steer = at(2, kForceBuild); // where Piece's force lands
		Piece p(BuildOf<8>::genre, recipe.tempo, 0);
		p.runBars(48);
		const int32_t until = p.drop().at + kStepsPerBar * kTicksPerStep;
		std::vector<Event> plain;
		for (const Event& e : play(run, 48).events) {
			if (e.tick < until) {
				plain.push_back(e);
			}
		}
		std::vector<Event> heard;
		for (const Played& e : p.sink.played) {
			if (e.at < until) {
				heard.push_back(e.event);
			}
		}
		expect(p.drop().at).to_be_greater_than(0);
		expect(heard.size()).to_equal(plain.size());
		for (size_t i = 0; i < plain.size() && i < heard.size(); i++) {
			expect(heard[i] == plain[i]).to_be_true();
		}
	});
});
// clang-format on

CPPSPEC_SPEC(riser_timing)
