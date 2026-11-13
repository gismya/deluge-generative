// A transition in the core and the Perform driver (spec §9.7, §12.9): the playing instance plays its Build, its Outro
// or on to the cut line, then decides nothing from the start step on, while the incoming instance, started with the
// pending Recipe, decides from it. Tempo glides to the incoming Recipe's, and swing switches on the start step.

#include "generative/perform/queue_text.h"
#include "transition_piece.h"

#include "cppspec.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace harness;

/// The transition goes through the real lifecycle (transition_piece.h): confirmed, and loaded at once.
using Transition = TransitionPiece;

// clang-format off
describe transitions("Transitions", $ {
	const int32_t phrase = genres::techno.phraseSteps();
	// runToTick() takes song ticks. The pieces here run at magnitude 2 from tick 0, where a song tick is a Generator
	// tick, so stepTick(n) is step n's line.

	it("plays a Build placed like a forced Build, then the new roster from the Drop", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Drop);
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
		expect(t.driver.playing() == &t.lifecycle.current()).to_be_true();
	});

	it("starts the new roster on a Drop", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Drop);
		t.runBars(4 * phrase / kStepsPerBar);
		ArcPhrase first;
		expect(t.driver.history().at(0, first)).to_be_true();
		expect(first.drop).to_be_true();
	});

	it("ends every outgoing note by the start step and drains the outgoing instance", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Drop);
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

	context("Throw on the outgoing parts (spec §11.4, §12.9)", _ {
		// No drift, so Space moves only by the macro layer and the Throw.
		const auto plain = technoEdited([](PartSpec& part) { part.drift.weight = 0; });
		const auto noThrow = technoEdited([](PartSpec& part) {
			part.drift.weight = 0;
			part.gestures.off |= momentBit(GestureMoment::ThrowTransition);
		});
		constexpr int32_t kBeat = kStepsPerBeat * kTicksPerStep;
		/// The outgoing instance's Space events for (part, lane): before the start step, or drained after it.
		const auto space = [](const Transition& t, uint8_t part, Lane lane, bool after) {
			std::vector<Played> out;
			for (const Played& p : t.sink.played) {
				if (!p.incoming && p.event.kind == EventKind::Control && p.event.control == SoundControl::Space
				    && p.event.part == part && p.event.lane == lane && p.outgoing == after) {
					out.push_back(p);
				}
			}
			return out;
		};

		it("spikes the outgoing parts' hits in the last beat before a cut, then holds and falls past the start step", _ {
			// The Drums' kit-wide set: its hats play into the cut line, where a phrase-end fill may rest the Bass.
			Transition t(&plain->genre);
			t.confirmAt(3, TransitionStyle::Cut);
			t.runBars(2 * phrase / kStepsPerBar);
			expect(t.sink.starts).to_equal(1);
			std::vector<Played> spikes;
			for (const Played& p : space(t, 0, Lane::None, false)) {
				if (p.at >= t.sink.startAt - kBeat && p.event.snap) {
					spikes.push_back(p);
				}
			}
			expect(spikes.empty()).to_be_false();
			for (const Played& p : spikes) {
				expect(p.event.offset).to_equal(100); // a Groove at the default Energy: the Throw alone
			}
			// The hold lasts a bar from the last hit, past the start step, then Space falls over a beat, ramped.
			std::vector<int32_t> falling;
			for (const Played& p : space(t, 0, Lane::None, true)) {
				expect(p.event.snap).to_be_false();
				expect(p.at).to_be_greater_than(spikes.front().at + (kStepsPerBar - 1) * kTicksPerStep);
				falling.push_back(p.event.offset);
			}
			expect(falling == std::vector<int32_t>{75, 50, 25, 0}).to_be_true();
			expect(t.sink.drained).to_equal(1);
			expect(t.driver.outgoing() == nullptr).to_be_true();
		});

		it("falls past the start step after the last hits before a Drop transition's Gap", _ {
			Transition t(&plain->genre);
			t.confirmAt(3, TransitionStyle::Drop);
			t.runBars(4 * phrase / kStepsPerBar);
			Transition without(&noThrow->genre);
			without.confirmAt(3, TransitionStyle::Drop);
			without.runBars(4 * phrase / kStepsPerBar);
			const int32_t gapAt = t.sink.startAt - kBeat; // Techno's Gap is a beat
			bool spiked = false;
			for (const Played& p : space(t, 0, Lane::None, false)) {
				spiked = spiked || (p.event.snap && p.at >= gapAt - kBeat && p.at < gapAt);
			}
			expect(spiked).to_be_true();
			// It falls to where the kit's Space stood on the outgoing instance's last step without the Throw.
			const std::vector<Played> after = space(t, 0, Lane::None, true);
			const std::vector<Played> plainSpace = space(without, 0, Lane::None, false);
			expect(after.size()).to_be_greater_than(1U);
			expect(plainSpace.empty()).to_be_false();
			expect(static_cast<int32_t>(after.back().event.offset))
			    .to_equal(static_cast<int32_t>(plainSpace.back().event.offset));
			expect(t.sink.drained).to_equal(1);
		});

		it("is switched per part: with it off, the outgoing parts don't throw", _ {
			Transition t(&noThrow->genre);
			t.confirmAt(3, TransitionStyle::Cut);
			t.runBars(2 * phrase / kStepsPerBar);
			for (const Played& p : space(t, 0, Lane::None, false)) {
				expect(p.event.snap && p.at > 0).to_be_false();
			}
			expect(space(t, 0, Lane::None, true).empty()).to_be_true();
		});
	});

	it("plays the Outro from the next phrase line for FADE THROUGH, and the new roster's Intro from the seam", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Fade);
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
		t.confirmAt(3, TransitionStyle::Cut);
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
		t.confirmAt(3, TransitionStyle::Drop);
		t.runToTick(stepTick(5));
		expect(t.out->transitionWaiting()).to_be_true();
		expect(t.lifecycle.cancel()).to_be_true();
		t.runBars(3 * phrase / kStepsPerBar);
		expect(t.sink.starts).to_equal(0);
		expect(t.out->transitionWaiting()).to_be_false();
		for (const Played& p : t.sink.from(false, EventKind::Section)) {
			expect(p.event.section == SectionKind::Build).to_be_false();
		}
	});

	it("can't be cancelled once it has landed", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Fade);
		t.runToTick(stepTick(phrase + 1));
		expect(t.out->transitionStart()).to_equal(phrase + genres::techno.arc.outroPhrases * phrase);
		t.push({CommandKind::Transition, 0, kCancelTransition});
		t.runBars(3 * phrase / kStepsPerBar);
		expect(t.sink.starts).to_equal(1);
	});

	it("glides the tempo across the Build to arrive on the Drop", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Drop);
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
		const int32_t bar = kBarTicks;
		t.confirmAt(3, TransitionStyle::Cut, bar);
		t.runBars(2 * phrase / kStepsPerBar);
		expect(t.sink.tempos.size()).to_be_greater_than(size_t{1});
		if (t.sink.tempos.size() > 1) {
			expect(t.sink.tempos.front().first).to_be_greater_than(t.sink.startAt - bar - 1);
			expect(t.sink.tempos.back().first).to_equal(t.sink.startAt);
			expect(t.sink.tempos.back().second).to_equal(kInTempo);
		}
	});

	it("glides a cancelled cut's tempo back to the playing Recipe's over the same length", _ {
		Transition t;
		const int32_t glide = phrase * kTicksPerStep; // a phrase: it starts at once, the cut line under a phrase away
		t.confirmAt(3, TransitionStyle::Cut, glide);
		t.runToTick(stepTick(64));
		expect(t.sink.tempos.empty()).to_be_false();
		const int32_t reached = t.sink.tempos.empty() ? kOutTempo : t.sink.tempos.back().second;
		expect(reached).to_be_greater_than(kOutTempo); // part way up to 134
		expect(reached).to_be_less_than(kInTempo);
		expect(t.lifecycle.cancel()).to_be_true();
		const int32_t cancelledAt = t.driver.clock();
		const size_t before = t.sink.tempos.size();
		t.runBars(2 * phrase / kStepsPerBar);
		expect(t.sink.starts).to_equal(0);
		expect(t.sink.tempos.size()).to_be_greater_than(before);
		int32_t last = reached;
		for (size_t i = before; i < t.sink.tempos.size(); i++) {
			expect(t.sink.tempos[i].second).to_be_less_than(last + 1); // only falls, back to 128
			last = t.sink.tempos[i].second;
		}
		expect(last).to_equal(kOutTempo);
		// It arrives a glide's length after the cancel, give or take the call it started on.
		const int32_t arrived = t.sink.tempos.back().first;
		expect(arrived).to_be_between(cancelledAt + glide - kTicksPerStep, cancelledAt + glide + kTicksPerStep);
	});

	it("jumps a cut's tempo on the cut line with an Instant glide", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Cut, 0);
		t.runBars(2 * phrase / kStepsPerBar);
		expect(t.sink.tempos.size()).to_equal(size_t{1});
		if (t.sink.tempos.size() == 1) {
			expect(t.sink.tempos.front().first).to_equal(t.sink.startAt);
			expect(t.sink.tempos.front().second).to_equal(kInTempo);
		}
	});

	it("switches swing on the start step, in one step", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Drop);
		t.runBars(4 * phrase / kStepsPerBar);
		expect(t.sink.swings.size()).to_equal(size_t{1});
		if (t.sink.swings.size() == 1) {
			expect(t.sink.swings.front().first).to_equal(t.sink.startAt);
			expect(t.sink.swings.front().second).to_equal(kInSwing);
		}
	});

	it("starts the new roster with the macros as they are at the start step", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Cut);
		t.runToTick(stepTick(20));
		t.steering.setMacro(Macro::Energy, macroPoints(40)); // a knob turn
		t.runBars(2 * phrase / kStepsPerBar);
		expect(t.driver.playing()->recipe().macros[Macro::Energy]).to_equal(macroPoints(40));
		expect(t.driver.playing()->recipe().keyRoot).to_equal(uint8_t{2}); // the pending Recipe's
	});

	it("keeps a hold through the Build, and holds the new roster's Groove", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Drop);
		t.runToTick(stepTick(5));
		t.push({CommandKind::SetHold, 0, kPress}); // after the confirm: it still carries over
		t.runBars(4 * phrase / kStepsPerBar);
		expect(t.sink.starts).to_equal(1);
		expect(t.driver.playing()->arc().held()).to_be_true();
		expect(t.driver.playing()->arc().section() == SectionKind::Groove).to_be_true();
	});

	it("holds the new roster from its first Groove after a FADE THROUGH, not in its Intro", _ {
		Transition t;
		t.push({CommandKind::SetHold, 0, kPress});
		t.confirmAt(3, TransitionStyle::Fade);
		t.runBars(2 * phrase / kStepsPerBar + genres::techno.arc.introPhrases * phrase / kStepsPerBar + 2);
		expect(t.driver.playing()->arc().section() == SectionKind::Groove).to_be_true();
		t.runBars(8 * phrase / kStepsPerBar);
		expect(t.driver.playing()->arc().section() == SectionKind::Groove).to_be_true(); // held
	});

	it("drops the incoming instance on stop, flushing both", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Drop);
		t.runToTick(stepTick(phrase + 4));
		t.driver.stop(t.sink);
		expect(t.driver.incoming() == nullptr).to_be_true();
		expect(t.driver.running()).to_be_false();
		expect(t.sink.starts).to_equal(0);
	});

	it("plans the transition's Build, and nothing it can know from the start step on", _ {
		Transition t;
		t.confirmAt(3, TransitionStyle::Drop);
		t.runToTick(stepTick(5));
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

	context("a genre change to a sibling with a larger roster (spec §9.7)", _ {
		it("plays the Build, then the new roster, tempo and key from the Drop", _ {
			Transition t(&genres::techno, &genres::technoMelodic);
			t.confirmAt(3, TransitionStyle::Drop);
			t.runBars(4 * phrase / kStepsPerBar);
			expect(t.sink.starts).to_equal(1);
			expect(t.sink.last(false, EventKind::Section).event.section == SectionKind::Build).to_be_true();
			// Melodic techno's fourth part (its FX), which Techno's roster lacks, plays from the start step.
			const int32_t lastPart = partCount(genres::technoMelodic) - 1;
			expect(lastPart).to_be_greater_than(partCount(genres::techno) - 1);
			bool newPartPlays = false;
			for (const Played& p : t.sink.played) {
				if (p.event.kind == EventKind::NoteOn && p.event.part == lastPart) {
					expect(p.incoming).to_be_true();
					expect(p.at).to_be_greater_than(t.sink.startAt - 1);
					newPartPlays = true;
				}
			}
			expect(newPartPlays).to_be_true();
			expect(t.lifecycle.current().recipe().genre == &genres::technoMelodic).to_be_true();
			expect(t.sink.tempos.back().first).to_equal(t.sink.startAt);
			expect(t.sink.tempos.back().second).to_equal(kInTempo);
			const Played key = t.sink.first(true, EventKind::KeyScale);
			expect(key.at).to_equal(t.sink.startAt);
			expect(static_cast<int32_t>(key.event.keyRoot())).to_equal(2); // D
		});

		it("keeps the macros the player has", _ {
			Transition t(&genres::techno, &genres::technoMelodic);
			t.currentRecipe.macros.value[0] = macroPoints(40);
			t.lifecycle.pendingRecipe.macros.value[0] = macroPoints(10);
			t.confirmAt(3, TransitionStyle::Cut);
			t.runBars(2 * phrase / kStepsPerBar);
			expect(t.sink.starts).to_equal(1);
			expect(t.currentRecipe.genre == &genres::technoMelodic).to_be_true();
			expect(t.currentRecipe.macros.value[0]).to_equal(macroPoints(40));
		});
	});
});

CPPSPEC_SPEC(transitions)
