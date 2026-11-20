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

#include "generative/firmware/compose_edits.h"
#include "definitions.h"
#include "generative/compose/song_size.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/recipe_files.h"
#include "generative/firmware/runner.h"
#include "gui/ui/generative_modal.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include <algorithm>
#include <utility>

namespace deluge::generative::firmware {

namespace {

/// What a card-reading edit does.
enum class RecipeRoll : uint8_t { Genre, Regenerate, Part, Lane, Load };

/// The picks before each row's last re-roll (spec §14.4's previous pick), and which rows have one, for the Current
/// Recipe of `generation`. A Recipe replaced anywhere (a genre pick, REGENERATE, Perform's genre change, `reset
/// recipe`) bumps CurrentRecipe::generation, which forgets them all. Zero at boot: no row has one.
struct PreviousPicks {
	SoundPicks picks;
	uint32_t parts = 0; ///< bit per part
	uint32_t lanes = 0; ///< bit per Lane
	uint32_t generation = 0;
};
PLACE_SDRAM_BSS PreviousPicks previous;

/// Drops the previous picks if they belong to a Recipe that was replaced since.
void forgetStalePrevious() {
	if (previous.generation != currentRecipe.generation) {
		previous.parts = 0;
		previous.lanes = 0;
		previous.generation = currentRecipe.generation;
	}
}
/// The picks a re-roll started from, kept by the job until it is done.
PLACE_SDRAM_BSS SoundPicks before;

/// A LOAD's file and name, kept by the job until it is done.
struct Loading {
	RecipeFile file;
	perform::RecipeName name;
};
PLACE_SDRAM_BSS Loading loading;

constexpr uint32_t bit(int32_t index) {
	return 1U << static_cast<uint32_t>(index);
}

const char* rollName(RecipeRoll roll) {
	switch (roll) {
	case RecipeRoll::Genre:
		return "genre";
	case RecipeRoll::Regenerate:
		return "regenerate";
	case RecipeRoll::Part:
		return "part";
	case RecipeRoll::Load:
		return "load";
	default:
		return "lane";
	}
}

class RollJob final : public Job {
public:
	RollJob() : Job(Kind::Modal) {}

	void prepare(RecipeRoll roll, const Genre* genre, int32_t part, Lane lane, RecipeRollEnded ended) {
		roll_ = roll;
		genre_ = genre;
		part_ = part;
		lane_ = lane;
		ended_ = ended;
		error_ = Error::NONE;
		replaced_ = 0;
	}

	JobOutcome run(Chunks& chunks) override {
#if ENABLE_SYSEX_TEST
		if (TestPort::preloadHeld()) {
			// Test hook ("hold preload"): wait here, yielding, so a script's BACK lands while the edit runs.
			TEST_EVENT("gen_recipe_roll %s held", rollName(roll_));
			TestPort::waitWhilePreloadHeld(chunks);
			if (chunks.cancelled()) {
				return JobOutcome::Cancelled; // the Current Recipe as it was
			}
		}
#endif
		before = currentRecipe.sounds;
		switch (roll_) {
		case RecipeRoll::Genre:
			error_ = currentRecipe.rollFromGenre(*genre_, &chunks);
			break;
		case RecipeRoll::Regenerate:
			error_ = currentRecipe.regenerate(&chunks);
			break;
		case RecipeRoll::Part:
			error_ = currentRecipe.rerollPart(part_, &chunks);
			break;
		case RecipeRoll::Lane:
			error_ = currentRecipe.rerollLane(lane_, &chunks);
			break;
		case RecipeRoll::Load:
			error_ = currentRecipe.load(loading.file, loading.name, replaced_, &chunks);
			break;
		}
		if (error_ == Error::ABORTED_BY_USER) {
			error_ = Error::NONE;
			return JobOutcome::Cancelled; // the Current Recipe as it was
		}
		if (error_ != Error::NONE) {
			return JobOutcome::Failed;
		}
		keepPrevious();
		return JobOutcome::Done;
	}

	void finished(JobOutcome outcome) override {
		generativeModalUI.jobEnded();
		TEST_EVENT("gen_recipe_roll %s %s error=%d", rollName(roll_),
		           outcome == JobOutcome::Done        ? "done"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           static_cast<int>(error_));
		if (outcome == JobOutcome::Failed) {
			display->displayError(error_ != Error::NONE ? error_ : Error::UNSPECIFIED);
		}
		if (outcome == JobOutcome::Done) {
			showSoundsReplaced(replaced_);
		}
		if (ended_ != nullptr) {
			ended_(outcome);
		}
	}

private:
	/// After a roll: a fresh Recipe forgets every previous pick (its generation moved on), a re-roll keeps what its
	/// row had.
	void keepPrevious() {
		forgetStalePrevious();
		if (roll_ == RecipeRoll::Genre || roll_ == RecipeRoll::Regenerate || roll_ == RecipeRoll::Load) {
			return;
		}
		if (roll_ == RecipeRoll::Lane) {
			const auto lane = static_cast<int32_t>(lane_);
			previous.picks.lane[lane] = before.lane[lane];
			previous.lanes |= bit(lane);
			return;
		}
		// A Kit part's re-roll rolls every lane it plays (rollPartSounds()).
		for (const LaneSpec& spec : currentRecipe.recipe.genre->parts[part_].lanes) {
			const auto lane = static_cast<int32_t>(spec.lane);
			previous.picks.lane[lane] = before.lane[lane];
			previous.lanes |= bit(lane);
		}
		previous.picks.part[part_] = before.part[part_];
		previous.parts |= bit(part_);
	}

	RecipeRoll roll_ = RecipeRoll::Genre;
	const Genre* genre_ = nullptr;
	int32_t part_ = 0;
	Lane lane_ = Lane::Kick;
	RecipeRollEnded ended_ = nullptr;
	Error error_ = Error::NONE;
	int32_t replaced_ = 0; ///< A LOAD's missing sounds, re-rolled
};

RollJob job;

/// LENGTH; 0 until set, read as the default.
int32_t lengthSeconds = 0;

bool beginRoll(RecipeRoll roll, const Genre* genre, int32_t part, Lane lane, RecipeRollEnded ended) {
	if (performAdapter.active() || rootUIIsGenerativePerform() || runner.busy()) {
		return false;
	}
	if (roll != RecipeRoll::Genre && roll != RecipeRoll::Load && !currentRecipe.exists) {
		return false;
	}
	job.prepare(roll, genre, part, lane, ended);
	generativeModalUI.open(job);
	if (!runner.submit(job)) {
		generativeModalUI.jobEnded();
		return false;
	}
	return true;
}

} // namespace

bool beginGenreRoll(const Genre& genre, RecipeRollEnded ended) {
	return beginRoll(RecipeRoll::Genre, &genre, 0, Lane::Kick, ended);
}

bool beginRegenerate(RecipeRollEnded ended) {
	return beginRoll(RecipeRoll::Regenerate, nullptr, 0, Lane::Kick, ended);
}

bool beginRecipeLoad(const RecipeFile& file, const perform::RecipeName& name, RecipeRollEnded ended) {
	if (performAdapter.active() || rootUIIsGenerativePerform() || runner.busy()) {
		return false; // before the copy: a job still running reads `loading`
	}
	loading.file = file;
	loading.name = name;
	return beginRoll(RecipeRoll::Load, nullptr, 0, Lane::Kick, ended);
}

bool beginPartRoll(int32_t part, RecipeRollEnded ended) {
	if (currentRecipe.exists && (part < 0 || part >= partCount(*currentRecipe.recipe.genre))) {
		return false;
	}
	return beginRoll(RecipeRoll::Part, nullptr, part, Lane::Kick, ended);
}

bool beginLaneRoll(Lane lane, RecipeRollEnded ended) {
	if (static_cast<int32_t>(lane) >= kNumLanes) {
		return false;
	}
	return beginRoll(RecipeRoll::Lane, nullptr, 0, lane, ended);
}

bool previousPartPick(int32_t part) {
	forgetStalePrevious();
	if (!currentRecipe.exists || part < 0 || part >= kMaxParts || (previous.parts & bit(part)) == 0) {
		return false;
	}
	std::swap(currentRecipe.sounds.part[part], previous.picks.part[part]);
	return true;
}

bool previousLanePick(Lane lane) {
	forgetStalePrevious();
	const auto index = static_cast<int32_t>(lane);
	if (!currentRecipe.exists || index >= kNumLanes || (previous.lanes & bit(index)) == 0) {
		return false;
	}
	std::swap(currentRecipe.sounds.lane[index], previous.picks.lane[index]);
	return true;
}

void newSeed() {
	currentRecipe.seed = freshEntropy();
	currentRecipe.takes = Takes{};
}

int32_t composeLength() {
	return lengthSeconds != 0 ? lengthSeconds : compose::kDefaultLengthSeconds;
}

void setComposeLength(int32_t seconds) {
	lengthSeconds = std::clamp(seconds, compose::kMinLengthSeconds, compose::kMaxLengthSeconds);
}

} // namespace deluge::generative::firmware
