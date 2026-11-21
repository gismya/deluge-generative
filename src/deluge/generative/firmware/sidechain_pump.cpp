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

#include "generative/firmware/sidechain_pump.h"
#include "generative/firmware/current_recipe.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/output.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "modulation/patch/patch_cable_set.h"

namespace deluge::generative::firmware {

PLACE_SDRAM_BSS SidechainPump sidechainPump{};

namespace {

/// How deep the pump ducks, as a knob position (-64 to 64) on the duck's param: VOLUME DUCKING on a synth, the
/// sidechain level on a Kit (tuning).
constexpr int32_t kPumpKnob = 20;

/// The param that ducks `part`'s Output: a synth's sidechain → volume cable (made if it has none), a Kit's kit-wide
/// sidechain level. Built in `memory`; nullptr where there is none (no room for another cable).
ModelStackWithAutoParam* duckParam(const PartOutput& part, char* memory) {
	ParamManagerForTimeline* manager = &part.clip->paramManager;
	ModelStackWithThreeMainThings* stack =
	    setupModelStackWithSong(memory, currentSong)
	        ->addTimelineCounter(part.clip)
	        ->addOtherTwoThingsButNoNoteRow(part.output->toModControllable(), manager);
	if (part.output->type == OutputType::KIT) {
		ParamCollectionSummary* summary = manager->getUnpatchedParamSetSummary();
		const int32_t id = deluge::modulation::params::UNPATCHED_SIDECHAIN_VOLUME;
		return stack->addParam(summary->paramCollection, summary, id, &manager->getUnpatchedParamSet()->params[id]);
	}
	ParamCollectionSummary* summary = manager->getPatchCableSetSummary();
	ParamDescriptor volume;
	volume.setToHaveParamOnly(deluge::modulation::params::GLOBAL_VOLUME_POST_REVERB_SEND);
	ModelStackWithParamCollection* cables = stack->addParamCollection(summary->paramCollection, summary);
	AutoParam* cable = manager->getPatchCableSet()->getParam(cables, PatchSource::SIDECHAIN, volume, true);
	if (cable == nullptr) {
		return nullptr;
	}
	return stack->addParam(summary->paramCollection, summary, PatchCableSet::getParamId(volume, PatchSource::SIDECHAIN),
	                       cable);
}

} // namespace

void SidechainPump::set(const Roster& roster, bool on) {
	const Genre* genre = currentRecipe.recipe.genre;
	for (int32_t p = 0; p < kMaxParts; p++) {
		const PartOutput& part = roster.parts[p];
		Saved& saved = saved_[p];
		const bool drums = genre != nullptr && p < partCount(*genre) && genre->parts[p].role == Role::Drums;
		if (part.output == nullptr || part.clip == nullptr || drums) {
			continue;
		}
		if (on == (saved.output == part.output)) {
			continue; // ducked already, or never ducked
		}
		char memory[MODEL_STACK_MAX_SIZE];
		ModelStackWithAutoParam* stack = duckParam(part, memory);
		if (stack == nullptr || stack->autoParam == nullptr) {
			continue;
		}
		if (on) {
			saved = {part.output, stack->autoParam->getCurrentValue()};
			stack->autoParam->setCurrentValueWithNoReversionOrRecording(
			    stack, stack->paramCollection->knobPosToParamValue(kPumpKnob, stack));
		}
		else {
			stack->autoParam->setCurrentValueWithNoReversionOrRecording(stack, saved.value);
			saved = {};
		}
	}
	// A part swapped while ducked keeps no record: its old Output is gone.
	if (!on) {
		std::fill(std::begin(saved_), std::end(saved_), Saved{});
	}
	if (on != on_) {
		on_ = on;
		TEST_EVENT("gen_pump on=%d", on ? 1 : 0);
	}
}

} // namespace deluge::generative::firmware
