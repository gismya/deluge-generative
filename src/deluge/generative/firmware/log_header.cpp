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

#include "generative/firmware/log_header.h"
#include "generative/core/genre.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/params/param_collection_summary.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "modulation/patch/patch_cable_set.h"
#include <cstring>

namespace deluge::generative::firmware {

namespace {

void put(uint8_t*& at, const void* value, size_t size) {
	std::memcpy(at, value, size);
	at += size;
}

/// One block for each of `paramManager`'s main collections: its param sets' values and its patch cables.
void logParams(SteeringLog& log, uint8_t output, Lane lane, ParamManager& paramManager) {
	const int32_t collections = paramManager.getExpressionParamSetOffset();
	for (int32_t c = 0; c < collections; c++) {
		ParamCollection* collection = paramManager.summaries[c].paramCollection;
		if (collection == nullptr) {
			continue;
		}
		// A Sound's ParamManager holds unpatched params, patched params, then patch cables (ParamManager's getters).
		const bool cables = collections == 3 && c == 2;
		const int32_t count = cables ? static_cast<PatchCableSet*>(collection)->numPatchCables
		                             : static_cast<ParamSet*>(collection)->getNumParams();
		const int32_t bytes = cables ? count * kCableValueBytes : count * static_cast<int32_t>(sizeof(int32_t));
		uint8_t* at = log.snapshotSpace(static_cast<int32_t>(sizeof(SnapshotBlock)) + bytes);
		if (at == nullptr) {
			return; // the log is full: the piece is cut at its step 0
		}
		const SnapshotBlock block{output, lane, static_cast<uint8_t>(c), static_cast<uint16_t>(count)};
		put(at, &block, sizeof block);
		if (cables) {
			auto* set = static_cast<PatchCableSet*>(collection);
			for (int32_t i = 0; i < count; i++) {
				PatchCable& cable = set->patchCables[i];
				const CableValue value{cable.destinationParamDescriptor.data, cable.param.getCurrentValue(),
				                       static_cast<uint8_t>(cable.from)};
				put(at, &value.destination, sizeof value.destination);
				put(at, &value.value, sizeof value.value);
				put(at, &value.source, sizeof value.source);
			}
		}
		else {
			auto* set = static_cast<ParamSet*>(collection);
			for (int32_t p = 0; p < count; p++) {
				const int32_t value = set->params[p].getCurrentValue();
				put(at, &value, sizeof value);
			}
		}
	}
}

} // namespace

void logPieceHeader(SteeringLog& log, const SoundPicks& picks, const Roster& roster, const Genre& genre, Song& song) {
	const int32_t parts = partCount(genre);
	for (int32_t p = 0; p < parts; p++) {
		log.addPick(pickSlot(p), picks.part[p].c_str());
		for (const LaneSpec& lane : genre.parts[p].lanes) {
			log.addPick(pickSlot(lane.lane), picks.lane[static_cast<int32_t>(lane.lane)].c_str());
		}
	}
	// Song params (song FX among them), then each part's Output, then its Kit's rows (spec §17.2).
	logParams(log, kSongParams, Lane::None, song.paramManager);
	for (int32_t p = 0; p < parts; p++) {
		const PartOutput& part = roster.parts[p];
		if (part.clip == nullptr) {
			continue;
		}
		const auto output = static_cast<uint8_t>(p);
		logParams(log, output, Lane::None, part.clip->paramManager);
		for (const LaneSpec& lane : genre.parts[p].lanes) {
			Drum* drum = part.drums[laneRow(lane.lane)];
			NoteRow* row = drum != nullptr ? part.clip->getNoteRowForDrum(drum) : nullptr;
			if (row != nullptr && row->paramManager.containsAnyMainParamCollections()) {
				logParams(log, output, lane.lane, row->paramManager);
			}
		}
	}
}

void logSoundPaths(SteeringLog& log, int32_t step, uint8_t part, uint8_t token, const SoundPicks& picks,
                   const Genre& genre) {
	if (part >= partCount(genre)) {
		return;
	}
	if (!picks.part[part].empty()) {
		log.appendSoundPath(step, part, token, pickSlot(part), picks.part[part].c_str());
	}
	for (const LaneSpec& lane : genre.parts[part].lanes) {
		const SoundPath& path = picks.lane[static_cast<int32_t>(lane.lane)];
		if (!path.empty()) {
			log.appendSoundPath(step, part, token, pickSlot(lane.lane), path.c_str());
		}
	}
}

std::optional<Command> paramEdit(const ModelStackWithAutoParam& stack, int32_t value, const Roster& roster,
                                 const Genre& genre, Song& song) {
	Command command{CommandKind::SetParam, 0, value};
	ParamManager* manager = stack.paramManager;
	bool found = manager == &song.paramManager;
	if (found) {
		command.target = kSongParams;
	}
	for (int32_t p = 0; p < partCount(genre) && !found; p++) {
		const PartOutput& part = roster.parts[p];
		if (part.clip == nullptr) {
			continue;
		}
		command.target = static_cast<uint8_t>(p);
		found = manager == &part.clip->paramManager;
		for (const LaneSpec& lane : genre.parts[p].lanes) {
			Drum* drum = part.drums[laneRow(lane.lane)];
			NoteRow* row = drum != nullptr && !found ? part.clip->getNoteRowForDrum(drum) : nullptr;
			if (row != nullptr && manager == &row->paramManager) {
				command.lane = lane.lane;
				found = true;
			}
		}
	}
	if (!found || stack.summary == nullptr) {
		return std::nullopt;
	}
	const auto collection = static_cast<int32_t>(stack.summary - manager->summaries);
	int32_t param = stack.paramId;
	int32_t source = 0;
	if (collection == 2 && manager->getExpressionParamSetOffset() == 3) {
		ParamDescriptor destination;
		PatchSource from = PatchSource::NONE;
		PatchCableSet::dissectParamId(static_cast<uint32_t>(stack.paramId), &destination, &from);
		param = destination.getJustTheParam();
		source = static_cast<int32_t>(from);
	}
	command.length = static_cast<uint16_t>(collection << 13 | (source & 0x1F) << 8 | (param & 0xFF));
	return command;
}

} // namespace deluge::generative::firmware
