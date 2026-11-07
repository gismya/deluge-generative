// The transport (spec §13.2): Start, SHIFT+PLAY and End, as they show in the event stream and the log.

#include "harness.h"

#include "cppspec.hpp"

#include <functional>
#include <string>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kPhraseSteps = 8 * kStepsPerBar; // Techno's phrase is 8 bars
constexpr int32_t kGroove0 = 2 * kPhraseSteps;     // Techno's Intro is 2 phrases

Command end(bool arm = true) {
	return {CommandKind::End, 0, arm ? 1 : 0};
}

Run techno() {
	Run run;
	run.recipe = technoRecipe();
	run.seed = 3;
	return run;
}

int32_t count(const std::vector<Event>& events, EventKind kind) {
	int32_t n = 0;
	for (const Event& e : events) {
		n += e.kind == kind ? 1 : 0;
	}
	return n;
}

/// A sink that notes how many song ticks the driver had been fed when each event played, and where each jump was
/// reported: the index of the next event to play.
struct TimedSink final : perform::NoteSink {
	int64_t fed = 0;
	std::vector<std::pair<Event, int64_t>> played;
	std::vector<size_t> jumps;
	std::vector<std::pair<int64_t, int32_t>> tempos; ///< (song ticks fed, tempo written)
	void play(const Event& event) override { played.push_back({event, fed}); }
	void jumped() override { jumps.push_back(played.size()); }
	void setTempo(int32_t tempo) override { tempos.push_back({fed, tempo}); }
};

/// A Techno piece driven one song tick a call at `magnitude` until `songTicks` have been fed.
/// Notes the driver clock at which each step was decided.
struct DecisionClock final : perform::DecisionTimer {
	const Generator* generator = nullptr;
	const perform::PerformDriver* driver = nullptr;
	std::vector<std::pair<int32_t, int32_t>> decided; ///< (step, clock)
	void decisionsBegin() override { decided.push_back({generator->nextStep(), driver->clock()}); }
	void decisionsEnd(int32_t) override {}
	[[nodiscard]] int32_t clockOf(int32_t step) const {
		for (auto [s, clock] : decided) {
			if (s == step) {
				return clock;
			}
		}
		return INT32_MIN;
	}
};

struct Driven {
	Generator generator;
	SteeringQueue queue;
	TimedSink sink;
	perform::PerformDriver driver;
	DecisionClock decisions;
	int32_t magnitude = kGeneratorTickMagnitude;
	int32_t wait = 1; ///< What the driver last asked for.

	explicit Driven(int32_t startAt = 0, int32_t mag = kGeneratorTickMagnitude, bool groove = true) : magnitude(mag) {
		decisions.generator = &generator;
		decisions.driver = &driver;
		driver.setDecisionTimer(&decisions);
		generator.start(technoRecipe(), 3, {}, groove);
		driver.begin(generator, startAt, magnitude);
		wait = driver.advance(0, magnitude, queue, nullptr, sink);
	}
	/// One call a song tick, as when something else makes the hook run every tick.
	void feed(int64_t songTicks) {
		for (int64_t t = 0; t < songTicks; t++) {
			sink.fed++;
			wait = driver.advance(1, magnitude, queue, nullptr, sink);
		}
	}
	/// Calls the driver only when it asked to be called, as the firmware hook does when nothing else plays, for
	/// `songTicks`. Each of `pushes` (song ticks fed, command) reaches the queue between calls, before the first call
	/// at or after it.
	void honour(int64_t songTicks, std::vector<std::pair<int64_t, Command>> pushes = {}) {
		std::vector<std::pair<int64_t, std::function<void()>>> actions;
		for (auto& [at, command] : pushes) {
			actions.push_back({at, [this, command] { queue.push(command); }});
		}
		honourWith(songTicks, std::move(actions));
	}
	/// As honour(), with any UI action between calls, such as a tempo turn.
	void honourWith(int64_t songTicks, std::vector<std::pair<int64_t, std::function<void()>>> actions) {
		const int64_t end = sink.fed + songTicks;
		size_t next = 0;
		while (sink.fed < end && driver.running()) {
			const int64_t at = std::min<int64_t>(sink.fed + wait, end);
			for (; next < actions.size() && actions[next].first <= at; next++) {
				actions[next].second();
			}
			const int32_t delta = static_cast<int32_t>(at - sink.fed);
			sink.fed = at;
			wait = driver.advance(delta, magnitude, queue, nullptr, sink);
		}
	}
	/// One call with `delta` song ticks, as a hook call after a jump would make it.
	void call(int32_t delta) { driver.advance(delta, magnitude, queue, nullptr, sink); }
	/// The events played from index `from` on.
	[[nodiscard]] std::vector<std::pair<Event, int64_t>> since(size_t from) const {
		return {sink.played.begin() + static_cast<int64_t>(from), sink.played.end()};
	}
};

constexpr int32_t kBarTicks = kStepsPerBar * kTicksPerStep;

/// Song ticks at `magnitude` for `ticks` Generator ticks, rounded up: when the driver's clock has reached them.
int64_t songTicksFor(int32_t ticks, int32_t magnitude) {
	if (magnitude >= kGeneratorTickMagnitude) {
		return int64_t{ticks} << (magnitude - kGeneratorTickMagnitude);
	}
	const int32_t per = 1 << (kGeneratorTickMagnitude - magnitude);
	return (ticks + per - 1) / per;
}

/// Checks a jump made when `fed` song ticks had been fed: the notes sounding end at once, then nothing plays until
/// the Generator's next bar line `resume` lands `silent` song ticks later (a note leaning ahead of it may start that
/// much early), and the piece carries on from there, with no fresh start. @return What's wrong, or "".
std::string resumed(const Driven& d, size_t from, int64_t fed, int64_t silent, int32_t resume) {
	auto after = d.since(from);
	size_t i = 0;
	for (; i < after.size() && after[i].second == fed; i++) {
		if (after[i].first.kind != EventKind::NoteOff) {
			return "something other than a note-off at the jump";
		}
	}
	bool sawLine = false;
	for (; i < after.size(); i++) {
		const auto& [event, when] = after[i];
		if (event.section == SectionKind::Intro && event.kind == EventKind::Section) {
			return "a fresh start";
		}
		if (event.tick < stepTick(resume) - kTicksPerStep) {
			return "a step before the resume line played: tick " + std::to_string(event.tick);
		}
		const int32_t fromLine = event.tick - stepTick(resume);
		const int64_t due =
		    fed + silent
		    + (fromLine >= 0 ? songTicksFor(fromLine, d.magnitude) : -songTicksFor(-fromLine, d.magnitude));
		if (fromLine >= 0 ? when != due : when < due) {
			return "tick " + std::to_string(event.tick) + " played after " + std::to_string(when - fed)
			       + " song ticks, due after " + std::to_string(due - fed);
		}
		sawLine |= fromLine >= 0;
	}
	return sawLine ? "" : "nothing played from the resume line";
}

/// The Generator's next bar line at or after `step`.
constexpr int32_t nextBarLine(int32_t step) {
	return (step + kStepsPerBar - 1) / kStepsPerBar * kStepsPerBar;
}

} // namespace

// clang-format off
describe transport("The transport", $ {
	it("plays the Outro from the phrase line after End, and stops where it ends", _ {
		static LogEntry entries[8];
		SteeringLog log(entries, 8);
		Run run = techno();
		run.log = &log;
		run.steer = at(kGroove0 + 40, end());
		Result result = play(run, 128);

		auto sections = only(result.events, EventKind::Section);
		const Event& last = sections.back();
		expect(last.section).to_equal(SectionKind::Outro);
		expect(last.tick).to_equal(stepTick(kGroove0 + kPhraseSteps));

		// Techno's Outro is a phrase: the piece stops on the line where it ends, every note released by then.
		const int32_t stopTick = stepTick(kGroove0 + 2 * kPhraseSteps);
		expect(result.ended).to_be_true();
		expect(result.endedAt).to_equal(stopTick);
		for (const Event& e : result.events) {
			expect(e.tick <= stopTick).to_be_true();
			if (e.tick == stopTick) {
				expect(e.kind).to_equal(EventKind::NoteOff);
			}
		}
		expect(count(result.events, EventKind::NoteOff)).to_equal(count(result.events, EventKind::NoteOn));
		expect(result.generator->ended()).to_be_true();

		expect(log.size()).to_equal(1);
		expect(log.at(0).step).to_equal(kGroove0 + kPhraseSteps);
		expect(log.at(0).command == end()).to_be_true();
	});

	it("cancels End with a tap while it waits, and keeps it when armed again", _ {
		Run run = techno();
		run.steer = steerAt({{kGroove0 + 40, end()}, {kGroove0 + 60, end(false)}});
		Result cancelled = play(run, 64);
		expect(cancelled.ended).to_be_false();
		for (const Event& e : only(cancelled.events, EventKind::Section)) {
			expect(e.section != SectionKind::Outro).to_be_true();
		}

		run.steer = steerAt({{kGroove0 + 40, end()}, {kGroove0 + 60, end()}});
		Result armedTwice = play(run, 64);
		expect(armedTwice.ended).to_be_true();
		expect(armedTwice.endedAt).to_equal(stepTick(kGroove0 + 2 * kPhraseSteps));
	});

	it("shares the force's slot: the later of End and a force wins", _ {
		const Command breakNow{CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0};
		Run run = techno();
		run.steer = steerAt({{kGroove0 + 40, end()}, {kGroove0 + 60, breakNow}});
		Result forced = play(run, 40);
		expect(forced.ended).to_be_false();
		auto sections = only(forced.events, EventKind::Section);
		bool brokeOnLine = false;
		for (const Event& e : sections) {
			expect(e.section != SectionKind::Outro).to_be_true();
			brokeOnLine |= e.section == SectionKind::Break && e.tick == stepTick(kGroove0 + kPhraseSteps);
		}
		expect(brokeOnLine).to_be_true();

		// A tap on End leaves a waiting force alone.
		run.steer = steerAt({{kGroove0 + 40, breakNow}, {kGroove0 + 60, end(false)}});
		expect(play(run, 40).events == forced.events).to_be_true();

		run.steer = steerAt({{kGroove0 + 40, breakNow}, {kGroove0 + 60, end()}});
		expect(play(run, 64).ended).to_be_true();
	});

	it("goes on past the Outro when a force lands on its end line, and ignores End again in the Outro", _ {
		constexpr int32_t kOutro = kGroove0 + kPhraseSteps; // End at kGroove0 + 40 starts it here
		constexpr int32_t kEndLine = kOutro + kPhraseSteps; // Techno's Outro is a phrase
		for (SectionKind kind : {SectionKind::Groove, SectionKind::Break}) {
			Run run = techno();
			run.steer = steerAt({{kGroove0 + 40, end()},
			                     {kOutro + 20, {CommandKind::ForceSection, static_cast<uint8_t>(kind), 0}}});
			Result result = play(run, 64);
			expect(result.ended).to_be_false();
			bool forcedOnLine = false;
			bool playedOn = false;
			for (const Event& e : result.events) {
				forcedOnLine |= e.kind == EventKind::Section && e.section == kind && e.tick == stepTick(kEndLine);
				playedOn |= e.kind == EventKind::NoteOn && e.tick > stepTick(kEndLine + kStepsPerBar);
			}
			expect(forcedOnLine).to_be_true();
			expect(playedOn).to_be_true();
		}

		// End pressed again in the Outro is dropped: with a 2-phrase Outro it would otherwise land on the Outro's
		// own phrase line and start it over. The piece still ends where the Outro does, logged once.
		static const Genre longOutro = [] {
			Genre g = genres::techno;
			g.arc.outroPhrases = 2;
			return g;
		}();
		static LogEntry entries[8];
		SteeringLog log(entries, 8);
		Run again = techno();
		again.recipe.genre = &longOutro;
		again.log = &log;
		again.steer = steerAt({{kGroove0 + 40, end()}, {kOutro + 20, end()}});
		Result result = play(again, 96);
		expect(result.ended).to_be_true();
		expect(result.endedAt).to_equal(stepTick(kOutro + 2 * kPhraseSteps));
		expect(log.size()).to_equal(1);
	});

	it("takes a force pressed during the Outro's last step, until its end line's decision is due", _ {
		// From Groove: End at step 40 plays the Outro from 128, which ends on 256.
		constexpr int32_t kEndLine = 2 * kPhraseSteps;
		const Command groove{CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Groove), 0};
		for (int32_t late : {5, 14}) { // ticks after the last step's decision; 24 later is the end line's
			Driven d;
			d.feed(stepTick(40));
			d.queue.push(end());
			d.feed(decisionTick(kEndLine - 1) + late - d.sink.fed);
			expect(d.generator.nextStep()).to_equal(kEndLine);
			d.queue.push(groove);
			d.feed(2 * kBarTicks);
			bool forced = false;
			for (auto& [event, when] : d.sink.played) {
				forced |= event.kind == EventKind::Section && event.section == SectionKind::Groove
				          && event.tick == stepTick(kEndLine);
			}
			expect(d.driver.ended()).to_be_false();
			expect(forced).to_be_true();
		}

		// Pressed after the end line's decision was due: too late, as for any line (spec §12.2).
		Driven d;
		d.feed(stepTick(40));
		d.queue.push(end());
		d.feed(decisionTick(kEndLine) + 3 - d.sink.fed);
		d.queue.push(groove);
		d.feed(2 * kBarTicks);
		expect(d.driver.ended()).to_be_true();
	});

	it("drains the end line's steering when its decision is due, called only when it asks", _ {
		// The firmware hook runs when the driver's wait says, so the end line's drain must be asked for at its
		// decision tick, not at the line: a force pressed in time keeps its lookahead, a later one is too late.
		constexpr int32_t kEndLine = 2 * kPhraseSteps;
		const Command groove{CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Groove), 0};
		{
			Driven d;
			d.feed(stepTick(40));
			d.queue.push(end());
			d.honour(decisionTick(kEndLine - 1) + 14 - d.sink.fed); // the last step is decided
			d.honour(4 * kBarTicks, {{decisionTick(kEndLine) - 2, groove}});
			expect(d.driver.ended()).to_be_false();
			expect(d.decisions.clockOf(kEndLine)).to_equal(decisionTick(kEndLine));
			bool forced = false;
			for (auto& [event, when] : d.sink.played) {
				forced |= event.kind == EventKind::Section && event.section == SectionKind::Groove
				          && event.tick == stepTick(kEndLine);
				if (event.tick >= stepTick(kEndLine)) {
					expect(when).to_equal(int64_t{event.tick}); // every event on its tick
				}
			}
			expect(forced).to_be_true();
		}
		{
			Driven d;
			d.feed(stepTick(40));
			d.queue.push(end());
			d.honour(decisionTick(kEndLine - 1) + 14 - d.sink.fed);
			d.honour(4 * kBarTicks, {{decisionTick(kEndLine) + 3, groove}});
			expect(d.driver.ended()).to_be_true();
		}
	});

	it("starts a glide counting from the first call after it, not from the last one before", _ {
		Driven d;
		d.driver.setTempo(12800, d.sink);
		d.honour(stepTick(40));
		while (d.wait < 4) {
			d.honour(1);
		}
		// The UI starts a glide 2 ticks into a wait: the hook isn't called until the wait is up.
		const int64_t call = d.sink.fed + d.wait;
		d.sink.tempos.clear();
		d.honourWith(kBarTicks + 16, {{d.sink.fed + 2, [&d] { d.driver.glideTempo(13184, kBarTicks, d.sink); }}});
		// A hundredth a tick from that call's clock: the ticks before it don't count.
		expect(d.sink.tempos.empty()).to_be_false();
		expect(d.sink.tempos.front().first).to_equal(call + 1);
		expect(d.sink.tempos.front().second).to_equal(12801);
		expect(d.sink.tempos.back().first).to_equal(call + kBarTicks);
		expect(d.sink.tempos.back().second).to_equal(13184);
	});

	it("still plays a Section start that was decided but not yet played, at a jump", _ {
		Driven d;
		d.feed(stepTick(40));
		d.queue.push(end()); // the Outro starts on 128
		d.feed(stepTick(kPhraseSteps) - 5 - d.sink.fed);
		size_t from = d.sink.played.size();
		d.driver.jump(d.sink);
		bool outro = false;
		for (size_t i = from; i < d.sink.played.size(); i++) {
			const Event& e = d.sink.played[i].first;
			outro |= e.kind == EventKind::Section && e.section == SectionKind::Outro;
		}
		expect(outro).to_be_true();
	});

	it("starts in Groove with SHIFT+PLAY, and every start is fresh", _ {
		Run run = techno();
		run.skipIntro = true;
		auto sections = only(play(run, 4).events, EventKind::Section);
		expect(sections.front().section).to_equal(SectionKind::Groove);
		expect(sections.front().tick).to_equal(0);

		// End queued in one piece doesn't carry into the next: a stop cancels the pending list (spec §2.6).
		Generator generator;
		SteeringQueue queue;
		RecordingSink sink;
		perform::PerformDriver driver;
		generator.start(technoRecipe(), 3, {});
		driver.begin(generator);
		driver.advance(0, kGeneratorTickMagnitude, queue, nullptr, sink);
		queue.push(end());
		driver.advance(kTicksPerStep, kGeneratorTickMagnitude, queue, nullptr, sink);
		expect(generator.endQueued()).to_be_true();
		driver.stop(sink);
		generator.start(technoRecipe(), 3, {}, true);
		expect(generator.endQueued()).to_be_false();
	});

	it("counts a negative clock move as a jump: notes end, then the piece resumes at the next master bar line", _ {
		Driven d;
		d.feed(300 * kTicksPerStep + 5); // a Groove, 12 steps into a bar
		size_t from = d.sink.played.size();
		int64_t fed = d.sink.fed;
		int32_t resume = nextBarLine(d.generator.nextStep());
		d.call(-1000); // the master moved back 1000 ticks: 61 ticks past a bar line
		d.feed(2 * kBarTicks);
		expect(resumed(d, from, fed, kBarTicks - 61, resume)).to_equal(std::string{});
	});

	it("counts a clock move longer than a bar as a jump", _ {
		Driven d;
		d.feed(300 * kTicksPerStep + 5);
		size_t from = d.sink.played.size();
		int64_t fed = d.sink.fed;
		int32_t resume = nextBarLine(d.generator.nextStep());
		d.call(kBarTicks + 100); // master now at 7205 + 484 = 7689, 9 ticks past a bar line
		d.feed(2 * kBarTicks);
		expect(resumed(d, from, fed, kBarTicks - 9, resume)).to_equal(std::string{});
	});

	it("takes a moved clock of exactly a bar as late, not as a jump", _ {
		Driven d;
		d.feed(300 * kTicksPerStep + 5);
		d.call(kBarTicks);
		for (auto& [event, when] : d.sink.played) {
			expect(event.kind == EventKind::NoteOff && when == d.sink.fed && event.tick < stepTick(300)).to_be_false();
		}
	});

	it("jumps on a mid-play song position pointer, resuming at the next master bar line", _ {
		for (int32_t magnitude : {1, 2, 3}) {
			Driven d(0, magnitude);
			const int32_t perStep = 6 << magnitude;
			d.feed(300 * int64_t{perStep} + 5);
			size_t from = d.sink.played.size();
			int64_t fed = d.sink.fed;
			int32_t resume = nextBarLine(d.generator.nextStep());
			const int32_t bar = kStepsPerBar * perStep;
			d.driver.jumpTo(3 * bar + perStep * 5, magnitude, d.sink); // 5 steps into a bar
			d.feed(2 * bar);
			expect(resumed(d, from, fed, bar - perStep * 5, resume)).to_equal(std::string{});
		}
	});

	it("jumps on a clock scale change: silent to its own next bar line, which stays on time", _ {
		Driven d;
		d.feed(300 * kTicksPerStep + 5);
		size_t from = d.sink.played.size();
		int64_t fed = d.sink.fed;
		d.driver.jump(d.sink);
		d.feed(2 * kBarTicks);
		expect(resumed(d, from, fed, stepTick(304) - (300 * kTicksPerStep + 5), 304)).to_equal(std::string{});
	});

	it("stays silent after Continue until the next master bar line, and puts step 0 there", _ {
		for (int32_t startAt : {0, 1000, 3 * kBarTicks}) {
			Driven d(startAt, kGeneratorTickMagnitude, false);
			d.feed(2 * kBarTicks);
			const int64_t toLine = (kBarTicks - startAt % kBarTicks) % kBarTicks;
			const auto& first = d.sink.played.front();
			expect(first.first.kind).to_equal(EventKind::Section);
			expect(first.first.section).to_equal(SectionKind::Intro);
			expect(first.second).to_equal(toLine);
			for (auto& [event, when] : d.sink.played) {
				expect(when - toLine >= event.tick).to_be_true(); // never early
			}
		}
	});

	it("reports every jump to the sink once, after its note-offs and before anything resumes", _ {
		// The adapter sends its all-notes-off safety net from there (spec §12.5), whichever way the jump came.
		for (int32_t path = 0; path < 4; path++) {
			Driven d;
			d.feed(300 * kTicksPerStep + 5);
			size_t from = d.sink.played.size();
			switch (path) {
			case 0:
				d.call(-1000);
				break;
			case 1:
				d.call(kBarTicks + 100);
				break;
			case 2:
				d.driver.jumpTo(3 * kBarTicks + 5 * kTicksPerStep, kGeneratorTickMagnitude, d.sink);
				break;
			default:
				d.driver.jump(d.sink);
				break;
			}
			d.feed(2 * kBarTicks);
			expect(d.sink.jumps.size()).to_equal(size_t{1});
			expect(d.driver.jumps()).to_equal(1U);
			size_t at = d.sink.jumps.front();
			expect(at > from).to_be_true(); // the sounding notes ended first
			for (size_t i = from; i < at; i++) {
				expect(d.sink.played[i].first.kind).to_equal(EventKind::NoteOff);
			}
			expect(at < d.sink.played.size()).to_be_true();
			expect(d.sink.played[at].second).to_be_greater_than(d.sink.played[at - 1].second); // later
		}
		// A late call that isn't a jump reports none.
		Driven late;
		late.feed(300 * kTicksPerStep + 5);
		late.call(kBarTicks);
		late.feed(kBarTicks);
		expect(late.sink.jumps.empty()).to_be_true();
	});

	it("plays a key change decided but not yet heard at a jump, and drops it on stop", _ {
		// A stop cancels what nobody heard, like the rest of the pending list (spec §2.6, §12.5, §13.2). A jump plays
		// on, so the key it lands in is the one the piece carries on in.
		for (bool stop : {true, false}) {
			Driven d;
			d.queue.push({CommandKind::SetKey, 2, static_cast<int32_t>(Scale::Minor)}); // D minor at the next phrase line
			const int32_t line = kPhraseSteps;
			d.feed(stepTick(line) - 5); // decided 12 ticks ahead, not played
			if (stop) {
				d.driver.stop(d.sink);
			}
			else {
				d.driver.jump(d.sink);
			}
			bool keyed = false;
			for (auto& [event, when] : d.sink.played) {
				keyed |= event.kind == EventKind::KeyScale && event.keyRoot == 2;
			}
			expect(keyed).to_equal(!stop);
		}
	});

	it("drops a Section start nobody heard on stop", _ {
		Driven d;
		d.feed(stepTick(40));
		d.queue.push(end()); // the Outro starts on 128
		d.feed(stepTick(kPhraseSteps) - 5 - d.sink.fed);
		d.driver.stop(d.sink);
		for (auto& [event, when] : d.sink.played) {
			expect(event.kind == EventKind::Section && event.section == SectionKind::Outro).to_be_false();
		}
	});
});

CPPSPEC_SPEC(transport)
