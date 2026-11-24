/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// Drop changes and live steering (spec §3.6, §13.6, ADR 0008): a ChangeAt::Drop set's keyed draw at each Drop, the
// genre copy applied again there, a live set's switch waiting for the next Drop (QUEUE, BACK, the log), the Perform
// steering's VARIANTS presses and the Recipe keeping the variant playing, and Dub's drum styles (#460 S5 C).

#include "generative/core/variants.h"
#include "generative/perform/queue_text.h"
#include "harness.h"

#include "cppspec.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace harness;
using deluge::generative::perform::PressOutcome;

namespace {

/// A made-up Drop-changing set: four variants, weights 1, 2, 1 and 1.
constexpr Variant kFour[] = {{.name = "a", .oledName = "A", .weight = 1},
                             {.name = "b", .oledName = "B", .weight = 2},
                             {.name = "c", .oledName = "C", .weight = 1},
                             {.name = "d", .oledName = "D", .weight = 1}};
constexpr VariantSet kChanging{
    .name = "four", .oledName = "FOUR", .variants = kFour, .changeAt = ChangeAt::Drop, .changeChance = 300};

/// Dub's sets, by index in its list.
constexpr int32_t kSkank = 0;
constexpr int32_t kDrums = 1;
constexpr int32_t kOneDrop = 0;
constexpr int32_t kRockers = 1;
constexpr int32_t kSteppers = 2;

/// Dub's default Recipe with its sets fixed by `fixes` (`set=name`).
Recipe dubWith(std::initializer_list<const char*> fixes, uint32_t entropy = 0) {
	Recipe recipe = newRecipe(genres::dub, entropy);
	for (const char* fix : fixes) {
		fixVariant(recipe.variants, parseVariantFix(genres::dub, fix).value());
	}
	return recipe;
}

/// What a piece did, step by step: the Drops' steps, each step's picks (after its decision), and its events.
struct Stepped {
	std::vector<int32_t> drops;
	std::vector<VariantPicks> picks;
	std::vector<Event> events;
};

/// Plays `recipe` from SHIFT+PLAY at `seed` for `steps` steps on an instance with a genre copy, deciding one step at a
/// time; `steer(step, queue)` may push before each decision.
Stepped stepThrough(const Recipe& recipe, uint32_t seed, int32_t steps,
                    const std::function<void(int32_t, SteeringQueue&)>& steer = {}) {
	Stepped out;
	auto generator = std::make_unique<Instance>();
	generator->start(recipe, seed, Takes{}, true);
	SteeringQueue queue;
	for (int32_t step = 0; step < steps; step++) {
		if (steer) {
			steer(step, queue);
		}
		generator->decideNextStep(queue, nullptr);
		if (generator->arc().dropStartsHere()) {
			out.drops.push_back(step);
		}
		out.picks.push_back(generator->variants());
		Event e;
		while (generator->popDue(INT32_MAX, &e)) {
			out.events.push_back(e);
		}
	}
	return out;
}

/// The Kick's hits on an 8th off the beat (the & of a beat), on steps [from, to): steppers plays them, the one drop
/// and rockers never do.
int32_t offbeatKicks(const Stepped& run, int32_t from, int32_t to) {
	int32_t count = 0;
	for (const Event& e : run.events) {
		const int32_t step = e.tick / kTicksPerStep;
		if (e.kind == EventKind::NoteOn && e.part == 0 && e.lane == Lane::Kick && !e.muted && step >= from && step < to
		    && step % 4 == 2) {
			count++;
		}
	}
	return count;
}

/// A Dub piece from its Groove, with its steering log, for the Perform steering's presses.
struct DubPiece : StepwisePiece<> {
	explicit DubPiece(const Recipe& recipe)
	    : StepwisePiece({.recipe = recipe, .seed = 3, .skipIntro = true, .logEntries = 64}) {}
	/// Has the instance that plays decide `steps` more steps.
	void decideMore(int32_t steps) { decideBefore(driver.playing()->nextStep() + steps); }
	/// Decides one step at a time until a Drop's first step is decided; false if none came in `bars` bars.
	bool decideToDrop(int32_t bars = 200) {
		const int32_t last = generator.nextStep() + bars * kStepsPerBar;
		while (generator.nextStep() < last) {
			if (!decideBefore(generator.nextStep() + 1)) {
				return false;
			}
			if (generator.arc().dropStartsHere()) {
				return true;
			}
		}
		return false;
	}
};

/// Dub with `sets` instead of its own.
Genre dubWithSets(std::span<const VariantSet> sets) {
	Genre genre = genres::dub;
	genre.variantSets = sets;
	return genre;
}

} // namespace

// clang-format off
describe drop_changes("Drop changes and live variant switches", $ {
	context("the Drop change draw (spec §3.6)", _ {
		it("changes at its chance over 1,000 seeds, by weight within the allowed variants, never to the one playing", _ {
			const uint8_t allowed = 0b0111; // a, b and c: d is never drawn
			int32_t changes = 0;
			int32_t toB = 0;
			int32_t toC = 0;
			for (uint32_t seed = 0; seed < 1000; seed++) {
				const uint8_t next = dropChange(kChanging, allowed, 0, seed, 2, 0);
				changes += next != 0 ? 1 : 0;
				toB += next == 1 ? 1 : 0;
				toC += next == 2 ? 1 : 0;
				expect(next != 3).to_be_true();
			}
			expect(changes).to_be_greater_than(250);
			expect(changes).to_be_less_than(350);
			// b weighs twice c.
			expect(toB).to_be_greater_than(toC * 3 / 2);
			expect(toB).to_be_less_than(toC * 5 / 2);
		});
		it("keys on the Drop's Groove and the set, so each Drop draws afresh and replays the same", _ {
			std::vector<uint8_t> first;
			std::vector<uint8_t> again;
			for (uint16_t groove = 1; groove < 40; groove++) {
				first.push_back(dropChange(kChanging, 0b1111, 1, 5, groove, 0));
				again.push_back(dropChange(kChanging, 0b1111, 1, 5, groove, 0));
			}
			expect(first == again).to_be_true();
			expect(std::ranges::count(first, uint8_t{1}) < static_cast<long>(first.size())).to_be_true();
			int32_t sameAsSetOne = 0;
			for (uint16_t groove = 1; groove < 40; groove++) {
				sameAsSetOne += dropChange(kChanging, 0b1111, 1, 5, groove, 0) == dropChange(kChanging, 0b1111, 1, 5, groove, 1) ? 1 : 0;
			}
			expect(sameAsSetOne).to_be_less_than(39);
		});
		it("never changes a set allowed only the variant playing, or one that never changes at a Drop", _ {
			VariantSet never = kChanging;
			never.changeAt = ChangeAt::Never;
			never.changeChance = 0;
			for (uint32_t seed = 0; seed < 200; seed++) {
				expect(static_cast<int32_t>(dropChange(kChanging, 0b0100, 2, seed, 3, 0))).to_equal(2);
				expect(static_cast<int32_t>(dropChange(never, 0b1111, 2, seed, 3, 0))).to_equal(2);
			}
		});
		it("refuses a Drop-changing set with no chance, and a chance on one that never changes", _ {
			static constexpr VariantSet kNoChance[] = {
			    {.name = "s", .oledName = "S", .variants = kFour, .changeAt = ChangeAt::Drop}};
			static constexpr VariantSet kTooLikely[] = {
			    {.name = "s", .oledName = "S", .variants = kFour, .changeAt = ChangeAt::Drop, .changeChance = 1001}};
			static constexpr VariantSet kStray[] = {{.name = "s", .oledName = "S", .variants = kFour, .changeChance = 5}};
			static constexpr VariantSet kGood[] = {
			    {.name = "s", .oledName = "S", .variants = kFour, .changeAt = ChangeAt::Drop, .changeChance = 1000}};
			expect(variantSetsValid(dubWithSets(kNoChance))).to_be_false();
			expect(variantSetsValid(dubWithSets(kTooLikely))).to_be_false();
			expect(variantSetsValid(dubWithSets(kStray))).to_be_false();
			expect(variantSetsValid(dubWithSets(kGood))).to_be_true();
		});
	});

	context("in a piece (spec §3.6, §9)", _ {
		it("moves Dub's drum style only on a Drop's first step, within its allowed styles, over 12 seeds", _ {
			int32_t drops = 0;
			int32_t changes = 0;
			for (uint32_t seed = 1; seed <= 12; seed++) {
				Recipe recipe = dubWith({"skank=sevenths"});
				recipe.variants.pick[kDrums] = kOneDrop;
				recipe.variants.allowed[kDrums] = 0b011; // one drop and rockers: steppers never plays
				const Stepped run = stepThrough(recipe, seed, 160 * kStepsPerBar);
				drops += static_cast<int32_t>(run.drops.size());
				uint8_t before = kOneDrop;
				for (size_t step = 0; step < run.picks.size(); step++) {
					const uint8_t now = run.picks[step].pick[kDrums];
					if (now != before) {
						changes++;
						expect(std::ranges::count(run.drops, static_cast<int32_t>(step))).to_equal(1);
					}
					expect(static_cast<int32_t>(now) != kSteppers).to_be_true();
					expect(static_cast<int32_t>(run.picks[step].pick[kSkank])).to_equal(0); // never changes at a Drop
					before = now;
				}
			}
			// About 40 % of Drops change, the chance Dub's data gives.
			expect(drops).to_be_greater_than(20);
			expect(changes * 1000).to_be_greater_than(drops * 200);
			expect(changes * 1000).to_be_less_than(drops * 600);
		});
		it("keeps a set allowed one style all piece", _ {
			for (uint32_t seed = 1; seed <= 6; seed++) {
				const Stepped run = stepThrough(dubWith({"drums=rockers"}), seed, 120 * kStepsPerBar);
				expect(run.drops.empty()).to_be_false();
				expect(std::ranges::all_of(run.picks, [](const VariantPicks& p) { return p.pick[kDrums] == kRockers; })).to_be_true();
			}
		});
		it("plays the new style from the Drop on: the genre copy is applied again there", _ {
			// A seed whose first Drop changes the one drop to steppers.
			bool found = false;
			for (uint32_t seed = 1; seed <= 60 && !found; seed++) {
				Recipe recipe = dubWith({"skank=sevenths", "drums=onedrop"});
				recipe.variants.allowed[kDrums] = 0b101; // one drop or steppers
				const Stepped run = stepThrough(recipe, seed, 120 * kStepsPerBar);
				if (run.drops.empty() || run.picks[run.drops[0]].pick[kDrums] != kSteppers) {
					continue;
				}
				found = true;
				const int32_t drop = run.drops[0];
				expect(static_cast<int32_t>(run.picks[drop - 1].pick[kDrums])).to_equal(kOneDrop);
				expect(offbeatKicks(run, 0, drop)).to_equal(0);
				expect(offbeatKicks(run, drop, drop + 8 * kStepsPerBar)).to_be_greater_than(8);
			}
			expect(found).to_be_true();
		});
	});

	context("a live switch (spec §13.6, ADR 0005)", _ {
		it("waits for the next Drop, lands there, is logged there, and the Recipe keeps the variant playing", _ {
			DubPiece piece(dubWith({"skank=sevenths", "drums=onedrop"}));
			piece.runToStepLine(4);
			expect(piece.steering.pressVariant(kDrums, kSteppers) == PressOutcome::Accepted).to_be_true();
			expect(piece.steering.variantAhead(kDrums)).to_equal(kSteppers);
			expect(piece.steering.variantPlaying(kDrums)).to_equal(kOneDrop);
			piece.decideMore(2);
			expect(piece.generator.variantWaiting(kDrums)).to_equal(kSteppers);
			// QUEUE names it, counting down to the Drop once the Build is on.
			int32_t countedTo = -1;
			int32_t before = kOneDrop;
			const int32_t last = piece.generator.nextStep() + 200 * kStepsPerBar;
			while (piece.generator.nextStep() < last && !piece.generator.arc().dropStartsHere()) {
				before = piece.generator.variants().pick[kDrums];
				if (piece.generator.arc().section() == SectionKind::Build) {
					countedTo = piece.generator.landsAt({CommandKind::SetVariant, kDrums, kSteppers});
				}
				piece.decideBefore(piece.generator.nextStep() + 1);
			}
			const int32_t drop = piece.generator.nextStep() - 1;
			expect(piece.generator.arc().dropStartsHere()).to_be_true();
			expect(before).to_equal(kOneDrop);
			expect(countedTo).to_equal(drop);
			expect(static_cast<int32_t>(piece.generator.variants().pick[kDrums])).to_equal(kSteppers);
			expect(piece.generator.variantWaiting(kDrums)).to_equal(-1);
			bool logged = false;
			for (int32_t i = 0; i < logSize(piece.log); i++) {
				const LogEntry entry = logAt(piece.log, i);
				if (entry.command.kind == CommandKind::SetVariant) {
					logged = entry.step == drop && entry.command.target == kDrums && entry.command.value == kSteppers;
				}
			}
			expect(logged).to_be_true();
			// The tick after the decision records it: SAVE stores the variant playing, which the set now allows.
			piece.runToStepLine(drop + 1);
			expect(static_cast<int32_t>(piece.currentRecipe.variants.pick[kDrums])).to_equal(kSteppers);
			expect(static_cast<int32_t>(piece.currentRecipe.variants.allowed[kDrums])).to_equal(0b101);
		});
		it("shows as VARIANT DROP in QUEUE, with no countdown while no Drop is ahead", _ {
			const Command command{CommandKind::SetVariant, kDrums, kRockers};
			char label[24];
			perform::queueLabel(command, genres::dub, label);
			expect(std::string(label)).to_equal(std::string("VARIANT DROP"));
			expect(std::string(perform::queueTag(command))).to_equal(std::string("VAR"));
			const perform::QueuedAction waiting[] = {{command, Generator::kNoLine}};
			perform::QueueLines lines;
			perform::queueLines(waiting, 0, genres::dub, lines);
			expect(std::string(lines.when[0])).to_equal(std::string("--"));
			char tag[16];
			perform::statusTag(waiting, 0, tag);
			expect(std::string(tag)).to_equal(std::string(">VAR--"));
		});
		it("is cancelled by picking the variant playing, and by BACK", _ {
			DubPiece piece(dubWith({"skank=sevenths", "drums=onedrop"}));
			piece.runToStepLine(4);
			piece.steering.pressVariant(kDrums, kRockers);
			piece.decideMore(2);
			expect(piece.generator.variantWaiting(kDrums)).to_equal(kRockers);
			piece.steering.pressVariant(kDrums, kOneDrop);
			piece.decideMore(2);
			expect(piece.generator.variantWaiting(kDrums)).to_equal(-1);
			expect(piece.steering.variantAhead(kDrums)).to_equal(kOneDrop);
			piece.steering.pressVariant(kSkank, 1);
			piece.decideMore(2);
			expect(piece.generator.variantWaiting(kSkank)).to_equal(1);
			piece.steering.cancelNewest();
			piece.decideMore(2);
			expect(piece.generator.variantWaiting(kSkank)).to_equal(-1);
			expect(piece.decideToDrop()).to_be_true();
			expect(static_cast<int32_t>(piece.generator.variants().pick[kSkank])).to_equal(0);
			expect(static_cast<int32_t>(piece.generator.variants().pick[kDrums])).to_equal(kOneDrop);
		});
		it("does nothing for a set that isn't live, or a variant past the set's", _ {
			static constexpr VariantSet kNotLive[] = {{.name = "s", .oledName = "S", .variants = kFour}};
			static const Genre notLive = dubWithSets(kNotLive);
			Recipe recipe = newRecipe(notLive, 0);
			DubPiece piece(recipe);
			piece.runToStepLine(4);
			expect(piece.steering.pressVariant(0, 1) == PressOutcome::NothingToDo).to_be_true();
			expect(piece.steering.pressVariant(1, 0) == PressOutcome::NothingToDo).to_be_true();
			// Pushed past the steering, the core drops it.
			piece.push({CommandKind::SetVariant, 0, 1});
			piece.decideMore(2);
			expect(piece.generator.variantWaiting(0)).to_equal(-1);
			DubPiece dub(dubWith({"drums=onedrop"}));
			dub.runToStepLine(4);
			expect(dub.steering.pressVariant(kDrums, 3) == PressOutcome::NothingToDo).to_be_true();
		});
		it("stopped, is the Recipe's at once, joining the allowed variants, and the next start plays it", _ {
			DubPiece piece(dubWith({"skank=sevenths", "drums=onedrop"}));
			piece.runToStepLine(4);
			piece.driver.stop(piece.sink);
			expect(piece.steering.pressVariant(kDrums, kRockers) == PressOutcome::Accepted).to_be_true();
			expect(static_cast<int32_t>(piece.currentRecipe.variants.pick[kDrums])).to_equal(kRockers);
			expect(static_cast<int32_t>(piece.currentRecipe.variants.allowed[kDrums])).to_equal(0b011);
			piece.playFromRecipe(true);
			expect(piece.steering.variantPlaying(kDrums)).to_equal(kRockers);
		});
	});

	context("Dub's drum styles (#460 S5 C)", _ {
		it("lists the drums set: the one drop it played before, first, then rockers and steppers, changing at Drops", _ {
			const VariantSet& drums = genres::dub.variantSets[kDrums];
			expect(std::string(drums.name)).to_equal(std::string("drums"));
			expect(std::string(drums.variants[kOneDrop].name)).to_equal(std::string("onedrop"));
			expect(drums.variants[kOneDrop].apply == nullptr).to_be_true();
			expect(std::string(drums.variants[kRockers].name)).to_equal(std::string("rockers"));
			expect(std::string(drums.variants[kSteppers].name)).to_equal(std::string("steppers"));
			expect(drums.live).to_be_true();
			expect(drums.changeAt == ChangeAt::Drop).to_be_true();
			expect(genres::dub.variantSets[kSkank].live).to_be_true(); // #495: no look-ahead reads the Extensions
		});
	});
});

CPPSPEC_SPEC(drop_changes)
