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
#include "generative/firmware/sound_controls.h"
#include "model/clip/instrument_clip.h"
#include "model/global_effectable/global_effectable.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/params/param_collection_summary.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "modulation/patch/patch_cable_set.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include <cstring>

namespace deluge::generative::firmware {

namespace params = deluge::modulation::params;

namespace {

void put(uint8_t*& at, const void* value, size_t size) {
	std::memcpy(at, value, size);
	at += size;
}

/// Whether main collection `c` of `paramManager` is its patch cables: a Sound's ParamManager holds unpatched params,
/// patched params, then patch cables (ParamManager's getters); a song's or a Kit's only unpatched ones.
bool isCableCollection(ParamManager& paramManager, int32_t c) {
	return paramManager.getExpressionParamSetOffset() == 3 && c == 2;
}

/// One block for each of `paramManager`'s main collections: its param sets' values and its patch cables.
void logParams(SteeringLog& log, uint8_t output, Lane lane, ParamManager& paramManager) {
	const int32_t collections = paramManager.getExpressionParamSetOffset();
	for (int32_t c = 0; c < collections; c++) {
		ParamCollection* collection = paramManager.summaries[c].paramCollection;
		if (collection == nullptr) {
			continue;
		}
		const bool cables = isCableCollection(paramManager, c);
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

/// Where a snapshot block's params live: a ParamManager, and the Sound that owns it, if any (patched again once
/// written).
struct BlockParams {
	ParamManager* manager = nullptr;
	Sound* sound = nullptr;
};

/// The ParamManager a block names, as logPieceHeader() walked them: the song's, a part's Output's (its clip's), or a
/// Kit row's. Empty for a part, row or collection that isn't there now.
BlockParams paramsOf(const SnapshotBlock& block, const Roster& roster, const Genre& genre, Song& song) {
	if (block.output == kSongParams) {
		return {&song.paramManager, nullptr};
	}
	if (block.output >= partCount(genre)) {
		return {};
	}
	const PartOutput& part = roster.parts[block.output];
	if (part.clip == nullptr || part.output == nullptr) {
		return {};
	}
	if (block.lane == Lane::None) {
		Sound* sound = part.output->type == OutputType::SYNTH ? static_cast<SoundInstrument*>(part.output) : nullptr;
		return {&part.clip->paramManager, sound};
	}
	Drum* drum = part.drums[laneRow(block.lane)];
	NoteRow* row = drum != nullptr ? part.clip->getNoteRowForDrum(drum) : nullptr;
	if (row == nullptr || !row->paramManager.containsAnyMainParamCollections()) {
		return {};
	}
	return {&row->paramManager, drum->type == DrumType::SOUND ? static_cast<SoundDrum*>(drum) : nullptr};
}

/// Writes a block's values into `manager`'s collection. @return the params written.
int32_t restoreBlock(const SnapshotBlock& block, const uint8_t* values, int32_t length, ParamManager& manager) {
	if (block.collection >= manager.getExpressionParamSetOffset()) {
		return 0;
	}
	ParamCollection* collection = manager.summaries[block.collection].paramCollection;
	if (collection == nullptr) {
		return 0;
	}
	int32_t written = 0;
	if (isCableCollection(manager, block.collection)) {
		auto* set = static_cast<PatchCableSet*>(collection);
		const int32_t count = std::min<int32_t>(block.count, length / kCableValueBytes);
		for (int32_t i = 0; i < count; i++) {
			const uint8_t* at = values + i * kCableValueBytes;
			CableValue value{};
			std::memcpy(&value.destination, at, sizeof value.destination);
			std::memcpy(&value.value, at + sizeof value.destination, sizeof value.value);
			std::memcpy(&value.source, at + sizeof value.destination + sizeof value.value, sizeof value.source);
			for (int32_t c = 0; c < set->numPatchCables; c++) {
				PatchCable& cable = set->patchCables[c];
				if (cable.destinationParamDescriptor.data == value.destination
				    && static_cast<uint8_t>(cable.from) == value.source) {
					if (cable.param.getCurrentValue() != value.value) {
						cable.param.setCurrentValueBasicForSetup(value.value);
						written++;
					}
					break;
				}
			}
		}
		return written;
	}
	auto* set = static_cast<ParamSet*>(collection);
	const int32_t count = std::min<int32_t>(
	    {static_cast<int32_t>(block.count), set->getNumParams(), length / static_cast<int32_t>(sizeof(int32_t))});
	// The song's tempo is the piece's Recipe's and its tempo entries' (spec §17.6), not a param to write back: at a
	// transition's start step it still holds the outgoing piece's glide.
	const bool songTempo = block.output == kSongParams && block.collection == 0;
	for (int32_t p = 0; p < count; p++) {
		if (songTempo && p == params::UNPATCHED_TEMPO) {
			continue;
		}
		int32_t value = 0;
		std::memcpy(&value, values + p * static_cast<int32_t>(sizeof(int32_t)), sizeof value);
		AutoParam& param = set->params[p];
		if (param.getCurrentValue() != value) {
			param.resetInterpolationIncrement(); // a ramp still running mustn't carry on past the restore
			param.setCurrentValueBasicForSetup(value);
			written++;
		}
	}
	return written;
}

} // namespace

std::optional<LoggedParam> loggedParam(const Command& edit, const Roster& roster, const Genre& genre, Song& song) {
	const int32_t collection = edit.length >> 13;
	const auto source = static_cast<uint8_t>((edit.length >> 8) & 0x1F);
	const int32_t id = edit.length & 0xFF;
	SnapshotBlock block{edit.target, edit.lane, static_cast<uint8_t>(collection), 0};
	const BlockParams params = paramsOf(block, roster, genre, song);
	ParamManager* manager = params.manager;
	if (manager == nullptr || collection >= manager->getExpressionParamSetOffset()) {
		return std::nullopt;
	}
	ParamCollection* found = manager->summaries[collection].paramCollection;
	if (found == nullptr) {
		return std::nullopt;
	}
	if (isCableCollection(*manager, collection)) {
		auto* set = static_cast<PatchCableSet*>(found);
		for (int32_t c = 0; c < set->numPatchCables; c++) {
			PatchCable& cable = set->patchCables[c];
			if (cable.destinationParamDescriptor.getJustTheParam() == id
			    && static_cast<uint8_t>(cable.from) == source) {
				return LoggedParam{manager, &cable.param, collection, c};
			}
		}
		return std::nullopt;
	}
	auto* set = static_cast<ParamSet*>(found);
	if (id >= set->getNumParams()) {
		return std::nullopt;
	}
	return LoggedParam{manager, &set->params[id], collection, id};
}

int32_t restoreSnapshot(const SteeringLog::Piece& piece, const Roster& roster, const Genre& genre, Song& song) {
	int32_t written = 0;
	piece.forEachSnapshot([&](const void* data, int32_t length) {
		if (length < static_cast<int32_t>(sizeof(SnapshotBlock))) {
			return;
		}
		SnapshotBlock block{};
		std::memcpy(&block, data, sizeof block);
		const BlockParams params = paramsOf(block, roster, genre, song);
		if (params.manager == nullptr) {
			return;
		}
		const int32_t n = restoreBlock(block, static_cast<const uint8_t*>(data) + sizeof block,
		                               length - static_cast<int32_t>(sizeof block), *params.manager);
		if (n > 0 && params.sound != nullptr) {
			params.sound->patcher.performInitialPatching(*params.sound, *params.manager);
		}
		written += n;
	});
	return written;
}

void logPieceHeader(SteeringLog& log, const SoundPicks& picks, const Roster& roster, const Genre& genre, Song& song,
                    uint8_t editedParts, bool songEdited) {
	const int32_t parts = partCount(genre);
	for (int32_t p = 0; p < parts; p++) {
		log.addPick(pickSlot(p), picks.part[p].c_str());
		for (const LaneSpec& lane : genre.parts[p].lanes) {
			log.addPick(pickSlot(lane.lane), picks.lane[static_cast<int32_t>(lane.lane)].c_str());
		}
	}
	// Song params (song FX among them), then each part's Output, then its Kit's rows (spec §17.2): only those edited
	// since they loaded.
	if (songEdited) {
		logParams(log, kSongParams, Lane::None, song.paramManager);
	}
	for (int32_t p = 0; p < parts; p++) {
		const PartOutput& part = roster.parts[p];
		if (part.clip == nullptr || (editedParts & (1U << p)) == 0) {
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

int32_t editedOutput(const ParamManager* manager, const Roster& roster, const Genre& genre, Song& song) {
	if (manager == nullptr) {
		return -1;
	}
	if (manager == &song.paramManager) {
		return kSongParams;
	}
	for (int32_t p = 0; p < partCount(genre); p++) {
		const PartOutput& part = roster.parts[p];
		if (part.clip == nullptr) {
			continue;
		}
		if (manager == &part.clip->paramManager) {
			return p;
		}
		for (const LaneSpec& lane : genre.parts[p].lanes) {
			Drum* drum = part.drums[laneRow(lane.lane)];
			NoteRow* row = drum != nullptr ? part.clip->getNoteRowForDrum(drum) : nullptr;
			if (row != nullptr && manager == &row->paramManager) {
				return p;
			}
		}
	}
	return -1;
}

uint8_t snapshotParts(const SteeringLog::Piece& piece, bool* song) {
	uint8_t parts = 0;
	*song = false;
	piece.forEachSnapshot([&](const void* data, int32_t length) {
		if (length < static_cast<int32_t>(sizeof(SnapshotBlock))) {
			return;
		}
		SnapshotBlock block{};
		std::memcpy(&block, data, sizeof block);
		if (block.output == kSongParams) {
			*song = true;
		}
		else if (block.output < kMaxParts) {
			parts |= static_cast<uint8_t>(1U << block.output);
		}
	});
	return parts;
}

void resetSongParams(Song& song) {
	UnpatchedParamSet* set = song.paramManager.getUnpatchedParamSet();
	const int32_t tempo = set->getValue(params::UNPATCHED_TEMPO);
	GlobalEffectable::initParams(&song.paramManager); // as buildPerformSong() sets a Perform song up
	set->params[params::UNPATCHED_TEMPO].setCurrentValueBasicForSetup(tempo);
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
	PatchSource source{}; // 0 in the key: a param set's param has no source
	if (isCableCollection(*manager, collection)) {
		ParamDescriptor destination;
		PatchCableSet::dissectParamId(static_cast<uint32_t>(stack.paramId), &destination, &source);
		param = destination.getJustTheParam();
	}
	command.length = SoundControls::paramKey(collection, source, param);
	return command;
}

} // namespace deluge::generative::firmware
