// A transition in the core and the Perform driver (spec §9.7, §12.9): the playing instance plays its Build, its Outro
// or on to the cut line, then decides nothing from the start step on, while the incoming instance, started with the
// pending Recipe, decides from it. Tempo glides to the incoming Recipe's, and swing switches on the start step.

#include "generative/perform/queue_text.h"
#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace harness;

namespace {

/// One event as played, on one timeline: Generator ticks since the outgoing piece's step 0.
struct Played {
	Event event;
	bool incoming = false; ///< From the incoming instance, played from the start step on.
	bool outgoing = false; ///< Drained from the outgoing instance after the start step.
	int32_t at = 0;
};

/// Records what the driver plays, with the transition's callbacks and the tempo and swing it writes.
struct TransitionSink final : perform::NoteSink {
	const perform::PerformDriver* driver = nullptr;
	std::vector<Played> played;
	int32_t starts = 0;
	int32_t drained = 0;
	int32_t base = 0;                                ///< Where the playing instance's tick 0 is on the timeline.
	int32_t outgoingBase = 0;                        ///< The outgoing one's, once it drains.
	int32_t startAt = -1;                            ///< The start step's tick on the timeline.
	std::vector<std::pair<int32_t, int32_t>> tempos; ///< {driver clock on the timeline, tempo}
	std::vector<std::pair<int32_t, int32_t>> swings;

	void play(const Event& event) override { played.push_back({event, starts > 0, false, event.tick + base}); }
	void playOutgoing(const Event& event) override {
		played.push_back({event, false, true, event.tick + outgoingBase});
	}
	void transitionStarts(int32_t startTick) override {
		starts++;
		outgoingBase = base;
		base += startTick;
		startAt = base;
	}
	void outgoingDrained() override { drained++; }
	void setTempo(int32_t tempo) override { tempos.emplace_back(driver->clock() + base, tempo); }
	void setSwing(int32_t amount) override { swings.emplace_back(driver->clock() + base, amount); }

	/// The first and last of from(): a default event when there are none, so a failing spec fails instead of crashing.
	[[nodiscard]] Played first(bool incoming, EventKind kind) const {
		std::vector<Played> all = from(incoming, kind);
		return all.empty() ? Played{} : all.front();
	}
	[[nodiscard]] Played last(bool incoming, EventKind kind) const {
		std::vector<Played> all = from(incoming, kind);
		return all.empty() ? Played{} : all.back();
	}
	/// The played events of one kind, from the incoming instance or the outgoing one.
	[[nodiscard]] std::vector<Played> from(bool incoming, EventKind kind) const {
		std::vector<Played> out;
		for (const Played& p : played) {
			if (p.event.kind == kind && p.incoming == incoming) {
				out.push_back(p);
			}
		}
		return out;
	}
};

constexpr int32_t kOutTempo = 12800;
constexpr int32_t kInTempo = 13400;
constexpr int32_t kInSwing = 12;

/// Two instances and a driver: Techno plays from its first Groove, and a transition is confirmed and loaded at
/// `queuedAt` (a step), so its command reaches the playing instance's next decision.
struct Transition {
	std::unique_ptr<Generator> out = std::make_unique<Generator>();
	std::unique_ptr<Generator> in = std::make_unique<Generator>();
	SteeringQueue queue;
	TransitionSink sink;
	perform::PerformDriver driver;
	Hook hook{driver, queue, sink};
	Recipe incoming = technoRecipe();

	Transition() {
		sink.driver = &driver;
		Recipe recipe = technoRecipe();
		recipe.tempo = kOutTempo;
		out->start(recipe, 3, Takes{}, true);
		driver.begin(*out);
		driver.setTempo(kOutTempo, sink);
		sink.tempos.clear();
		incoming.tempo = kInTempo;
		incoming.swing = kInSwing;
		incoming.keyRoot = 2; // D
	}

	/// The preload ended: the incoming instance starts, and the transition's command goes to the playing one.
	void queue_(TransitionStyle style, int32_t cutGlideTicks = kTicksPerStep * kStepsPerBar) {
		in->startIncoming(incoming, 7, Takes{}, style);
		driver.queueIncoming(*in, cutGlideTicks);
		queue.push({CommandKind::Transition, static_cast<uint8_t>(style), 0});
	}
	void queueAt(int32_t step, TransitionStyle style, int32_t cutGlideTicks = kTicksPerStep * kStepsPerBar) {
		hook.runTo(stepTick(step) - 1);
		queue_(style, cutGlideTicks);
	}
	void runBars(int32_t bars) { hook.run(int64_t{bars} * kStepsPerBar * kTicksPerStep); }
};

} // namespace

// clang-format off
describe transitions("Transitions", $ {
	const int32_t phrase = genres::techno.phraseSteps();

	it("plays a Build placed like a forced Build, then the new roster from the Drop", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.runBars(4 * phrase / kStepsPerBar);
		expect(t.sink.starts).to_equal(1);
		// Techno's Build is 8 or 16 bars; either ends on a phrase line, here the first after the Build starts.
		const Played build = t.sink.last(false, EventKind::Section);
		expect(build.event.section == SectionKind::Build).to_be_true();
		expect(build.at % (kStepsPerBar * kTicksPerStep)).to_equal(0);
		expect(t.sink.startAt % (phrase * kTicksPerStep)).to_equal(0);
		expect(t.sink.startAt).to_be_greater_than(build.at);
		// The new roster starts on a Drop: a Groove whose first step the Drop opens.
		const Played incoming = t.sink.first(true, EventKind::Section);
		expect(incoming.event.section == SectionKind::Groove).to_be_true();
		expect(incoming.at).to_equal(t.sink.startAt);
		expect(t.driver.playing() == t.in.get()).to_be_true();
	});

	it("starts the new roster on a Drop", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.runBars(4 * phrase / kStepsPerBar);
		ArcPhrase first;
		expect(t.driver.history().at(0, first)).to_be_true();
		expect(first.drop).to_be_true();
	});

	it("ends every outgoing note by the start step and drains the outgoing instance", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.runBars(4 * phrase / kStepsPerBar);
		expect(t.sink.drained).to_equal(1);
		expect(t.driver.outgoing() == nullptr).to_be_true();
		int32_t sounding = 0;
		int32_t lastOff = 0;
		for (const Played& p : t.sink.played) {
			if (p.incoming) {
				continue;
			}
			if (p.event.kind == EventKind::NoteOn) {
				expect(p.at).to_be_less_than(t.sink.startAt + kTicksPerStep / 2); // at most a lean past it
				sounding++;
			}
			if (p.event.kind == EventKind::NoteOff) {
				sounding--;
				lastOff = std::max(lastOff, p.at);
			}
		}
		expect(sounding).to_equal(0);
		expect(lastOff).to_be_greater_than(t.sink.startAt - 1);
		expect(t.out->droppedEvents()).to_equal(0U);
	});

	it("plays the Outro from the next phrase line for FADE THROUGH, and the new roster's Intro from the seam", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Fade);
		t.runBars(4 * phrase / kStepsPerBar);
		const Played outro = t.sink.last(false, EventKind::Section);
		expect(outro.event.section == SectionKind::Outro).to_be_true();
		expect(outro.at).to_equal(stepTick(phrase));
		expect(t.sink.startAt).to_equal(stepTick(phrase + genres::techno.arc.outroPhrases * phrase));
		const Played intro = t.sink.first(true, EventKind::Section);
		expect(intro.event.section == SectionKind::Intro).to_be_true();
		expect(intro.at).to_equal(t.sink.startAt);
		expect(t.driver.ended()).to_be_false(); // the transition's Outro never ends the piece
	});

	it("cuts to the new roster in Groove on the next phrase line, with no Build or Outro", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Cut);
		t.runBars(2 * phrase / kStepsPerBar);
		expect(t.sink.startAt).to_equal(stepTick(phrase));
		expect(t.sink.last(false, EventKind::Section).event.section == SectionKind::Groove).to_be_true();
		expect(t.sink.first(true, EventKind::Section).event.section == SectionKind::Groove).to_be_true();
		ArcPhrase first;
		expect(t.driver.history().at(0, first)).to_be_true();
		expect(first.drop).to_be_false();
	});

	it("is cancelled while it waits, and the playing roster plays on", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.hook.runTo(stepTick(5));
		expect(t.out->transitionWaiting()).to_be_true();
		t.queue.push({CommandKind::Transition, 0, kCancelTransition});
		t.driver.cancelIncoming(t.sink);
		t.runBars(3 * phrase / kStepsPerBar);
		expect(t.sink.starts).to_equal(0);
		expect(t.out->transitionWaiting()).to_be_false();
		for (const Played& p : t.sink.from(false, EventKind::Section)) {
			expect(p.event.section == SectionKind::Build).to_be_false();
		}
	});

	it("can't be cancelled once it has landed", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Fade);
		t.hook.runTo(stepTick(phrase + 1));
		expect(t.out->transitionStart()).to_equal(phrase + genres::techno.arc.outroPhrases * phrase);
		t.queue.push({CommandKind::Transition, 0, kCancelTransition});
		t.runBars(3 * phrase / kStepsPerBar);
		expect(t.sink.starts).to_equal(1);
	});

	it("glides the tempo across the Build to arrive on the Drop", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.runBars(4 * phrase / kStepsPerBar);
		const int32_t buildAt = t.sink.last(false, EventKind::Section).at;
		expect(t.sink.tempos.empty()).to_be_false();
		int32_t last = kOutTempo;
		for (const auto& [at, tempo] : t.sink.tempos) {
			expect(at).to_be_greater_than(buildAt - kLookaheadTicks - 1);
			expect(at).to_be_less_than(t.sink.startAt + 1);
			expect(tempo).to_be_greater_than(last - 1); // only rises, 128 to 134
			last = tempo;
		}
		if (!t.sink.tempos.empty()) {
			expect(t.sink.tempos.back().first).to_equal(t.sink.startAt);
			expect(t.sink.tempos.back().second).to_equal(kInTempo);
		}
		// Half way across the Build, about half way there.
		const int32_t middle = (buildAt + t.sink.startAt) / 2;
		for (const auto& [at, tempo] : t.sink.tempos) {
			if (at >= middle) {
				expect(tempo).to_be_between(13000, 13200);
				break;
			}
		}
	});

	it("glides a cut's tempo over the Glide setting, ending on the cut line", _ {
		Transition t;
		const int32_t bar = kStepsPerBar * kTicksPerStep;
		t.queueAt(3, TransitionStyle::Cut, bar);
		t.runBars(2 * phrase / kStepsPerBar);
		expect(t.sink.tempos.size()).to_be_greater_than(size_t{1});
		if (t.sink.tempos.size() > 1) {
			expect(t.sink.tempos.front().first).to_be_greater_than(t.sink.startAt - bar - 1);
			expect(t.sink.tempos.back().first).to_equal(t.sink.startAt);
			expect(t.sink.tempos.back().second).to_equal(kInTempo);
		}
	});

	it("jumps a cut's tempo on the cut line with an Instant glide", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Cut, 0);
		t.runBars(2 * phrase / kStepsPerBar);
		expect(t.sink.tempos.size()).to_equal(size_t{1});
		if (t.sink.tempos.size() == 1) {
			expect(t.sink.tempos.front().first).to_equal(t.sink.startAt);
			expect(t.sink.tempos.front().second).to_equal(kInTempo);
		}
	});

	it("switches swing on the start step, in one step", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.runBars(4 * phrase / kStepsPerBar);
		expect(t.sink.swings.size()).to_equal(size_t{1});
		if (t.sink.swings.size() == 1) {
			expect(t.sink.swings.front().first).to_equal(t.sink.startAt);
			expect(t.sink.swings.front().second).to_equal(kInSwing);
		}
	});

	it("starts the new roster with the macros as they are at the start step", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Cut);
		t.hook.runTo(stepTick(20));
		t.queue.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), macroPoints(40)});
		t.runBars(2 * phrase / kStepsPerBar);
		expect(t.driver.playing()->recipe().macros[Macro::Energy]).to_equal(macroPoints(40));
		expect(t.driver.playing()->recipe().keyRoot).to_equal(uint8_t{2}); // the pending Recipe's
	});

	it("keeps a hold through the Build, and holds the new roster's Groove", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.hook.runTo(stepTick(5));
		t.queue.push({CommandKind::SetHold, 0, kPress}); // after the confirm: it still carries over
		t.runBars(4 * phrase / kStepsPerBar);
		expect(t.sink.starts).to_equal(1);
		expect(t.driver.playing()->arc().held()).to_be_true();
		expect(t.driver.playing()->arc().section() == SectionKind::Groove).to_be_true();
	});

	it("holds the new roster from its first Groove after a FADE THROUGH, not in its Intro", _ {
		Transition t;
		t.queue.push({CommandKind::SetHold, 0, kPress});
		t.queueAt(3, TransitionStyle::Fade);
		t.runBars(2 * phrase / kStepsPerBar + genres::techno.arc.introPhrases * phrase / kStepsPerBar + 2);
		expect(t.driver.playing()->arc().section() == SectionKind::Groove).to_be_true();
		t.runBars(8 * phrase / kStepsPerBar);
		expect(t.driver.playing()->arc().section() == SectionKind::Groove).to_be_true(); // held
	});

	it("drops the incoming instance on stop, flushing both", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.hook.runTo(stepTick(phrase + 4));
		t.driver.stop(t.sink);
		expect(t.driver.incoming() == nullptr).to_be_true();
		expect(t.driver.running()).to_be_false();
		expect(t.sink.starts).to_equal(0);
	});

	it("plans the transition's Build, and nothing it can know from the start step on", _ {
		Transition t;
		t.queueAt(3, TransitionStyle::Drop);
		t.hook.runTo(stepTick(5));
		ArcPlan plan;
		t.out->planArc(plan);
		const Command* waiting = t.out->waitingTransition();
		expect(waiting != nullptr).to_be_true();
		const int32_t start = t.out->landsAt(*waiting) + waiting->value; // the Build's end: the Drop
		expect(start % phrase).to_equal(0);
		const int32_t startPhrase = start / phrase;
		expect(plan.phrase[startPhrase - 1].kind == SectionKind::Build).to_be_true();
		for (int32_t i = startPhrase; i < ArcPlan::kMaxPhrases; i++) {
			expect(plan.phrase[i].next).to_be_true();
		}
		expect(plan.phrase[0].next).to_be_false();
		expect(std::string(perform::queueTag(*waiting))).to_equal(std::string("CHG"));
	});
});

CPPSPEC_SPEC(transitions)
