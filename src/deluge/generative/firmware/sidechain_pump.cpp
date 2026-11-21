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

ParamDescriptor duckDestination() {
	ParamDescriptor volume;
	volume.setToHaveParamOnly(deluge::modulation::params::GLOBAL_VOLUME_POST_REVERB_SEND);
	return volume;
}

ModelStackWithThreeMainThings* partStack(Output* output, InstrumentClip* clip, char* memory) {
	return setupModelStackWithSong(memory, currentSong)
	    ->addTimelineCounter(clip)
	    ->addOtherTwoThingsButNoNoteRow(output->toModControllable(), &clip->paramManager);
}

/// The param that ducks `output`: a synth's sidechain → volume cable, made if it has none and `make` (`made` says
/// so), a Kit's kit-wide sidechain level. Built in `memory`; nullptr where there is none.
ModelStackWithAutoParam* duckParam(Output* output, InstrumentClip* clip, char* memory, bool make, bool* made) {
	ParamManagerForTimeline* manager = &clip->paramManager;
	ModelStackWithThreeMainThings* stack = partStack(output, clip, memory);
	if (output->type == OutputType::KIT) {
		ParamCollectionSummary* summary = manager->getUnpatchedParamSetSummary();
		const int32_t id = deluge::modulation::params::UNPATCHED_SIDECHAIN_VOLUME;
		return stack->addParam(summary->paramCollection, summary, id, &manager->getUnpatchedParamSet()->params[id]);
	}
	ParamCollectionSummary* summary = manager->getPatchCableSetSummary();
	PatchCableSet* cables = manager->getPatchCableSet();
	ModelStackWithParamCollection* withCables = stack->addParamCollection(summary->paramCollection, summary);
	const bool had = cables->getPatchCableIndex(PatchSource::SIDECHAIN, duckDestination()) != 255;
	AutoParam* cable = cables->getParam(withCables, PatchSource::SIDECHAIN, duckDestination(), make);
	if (cable == nullptr) {
		return nullptr;
	}
	if (made != nullptr) {
		*made = !had;
	}
	return stack->addParam(summary->paramCollection, summary,
	                       PatchCableSet::getParamId(duckDestination(), PatchSource::SIDECHAIN), cable);
}

} // namespace

void SidechainPump::duck(const Roster& roster) {
	const Genre* genre = currentRecipe.recipe.genre;
	const int32_t drums = genre != nullptr ? drumsPart(*genre) : -1;
	for (int32_t p = 0; p < kMaxParts; p++) {
		const PartOutput& part = roster.parts[p];
		Saved& saved = saved_[p];
		if (part.output == nullptr || part.clip == nullptr || p == drums || saved.output == part.output) {
			continue; // none, the Drums, or ducked already
		}
		// A part swapped while ducked keeps no record of its old Output, which retires.
		char memory[MODEL_STACK_MAX_SIZE];
		bool made = false;
		ModelStackWithAutoParam* stack = duckParam(part.output, part.clip, memory, true, &made);
		if (stack == nullptr || stack->autoParam == nullptr) {
			saved = {};
			continue;
		}
		saved = {part.output, part.clip, stack->autoParam->getCurrentValue(), made};
		stack->autoParam->setCurrentValueWithNoReversionOrRecording(
		    stack, stack->paramCollection->knobPosToParamValue(kPumpKnob, stack));
	}
	report(true);
}

void SidechainPump::release() {
	for (Saved& saved : saved_) {
		if (saved.output == nullptr) {
			continue;
		}
		char memory[MODEL_STACK_MAX_SIZE];
		ModelStackWithAutoParam* stack = duckParam(saved.output, saved.clip, memory, false, nullptr);
		if (stack != nullptr && stack->autoParam != nullptr) {
			if (saved.made) {
				// The pump's own cable goes, so the song keeps none it never had (Compose, a leave that keeps it).
				ParamManagerForTimeline* manager = &saved.clip->paramManager;
				ParamCollectionSummary* summary = manager->getPatchCableSetSummary();
				char cableMemory[MODEL_STACK_MAX_SIZE];
				ModelStackWithParamCollection* withCables = partStack(saved.output, saved.clip, cableMemory)
				                                                ->addParamCollection(summary->paramCollection, summary);
				PatchCableSet* cables = manager->getPatchCableSet();
				cables->deletePatchCable(withCables,
				                         cables->getPatchCableIndex(PatchSource::SIDECHAIN, duckDestination()));
			}
			else {
				stack->autoParam->setCurrentValueWithNoReversionOrRecording(stack, saved.value);
			}
		}
		saved = {};
	}
	report(false);
}

void SidechainPump::report(bool on) {
	if (on != on_) {
		on_ = on;
		TEST_EVENT("gen_pump on=%d", on ? 1 : 0);
	}
}

} // namespace deluge::generative::firmware
