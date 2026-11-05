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

#include "generative/debug/compose_shape.h"

#if ENABLE_SYSEX_TEST

#include "OSLikeStuff/scheduler_api.h"
#include "OSLikeStuff/timers_interrupts/clock_type.h"
#include "definitions_cxx.hpp"
#include "memory/general_memory_allocator.h"
#include "model/clip/clip_instance.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/storage_manager.h"
#include <algorithm>
#include <new>

namespace deluge::generative::compose_shape {

namespace {

namespace params = deluge::modulation::params;

constexpr int32_t kParts = 6;
constexpr int32_t kDrumLanes = 11;
constexpr int32_t kFxLanes = 4;
constexpr int32_t kMaxSessionSections = 23; ///< §15.2: the 24th is the Outro's
/// Stop building rather than let a clone run out of RAM: several clone paths FREEZE_WITH_ERROR on failure. General
/// allocations spill into the sample (stealable) region once the fast and SDRAM heaps are full, so that counts too.
constexpr uint32_t kMinFreeBytes = 1024 * 1024;

uint32_t now() {
	return getSystemTicks();
}

uint32_t toUs(uint32_t ticks) {
	return static_cast<uint32_t>(static_cast<uint64_t>(ticks) * 1000000u / DELUGE_CLOCKS_PER);
}

volatile bool cancelFlag = false;
volatile uint32_t cancelTicks = 0;

// ---- Yielding ----

struct Runner {
	uint32_t budgetTicks;
	bool schedulerYield;
	uint32_t lastYield;
	uint32_t longestRun;
	uint32_t yields;

	/// Returns false once cancelled.
	bool checkpoint() {
		uint32_t t = now();
		if (t - lastYield >= budgetTicks) {
			longestRun = std::max(longestRun, t - lastYield);
			if (schedulerYield) {
				yield([]() { return true; }); // one round of the scheduler: audio, MIDI, UI tasks
			}
			else {
				AudioEngine::routineWithClusterLoading(); // what stock builds do: audio only
			}
			yields++;
			lastYield = now();
		}
		return !cancelFlag;
	}

	void finish() { longestRun = std::max(longestRun, now() - lastYield); }
};

uint32_t freeBytes() {
	GeneralMemoryAllocator& allocator = GeneralMemoryAllocator::get();
	uint32_t free = 0;
	for (int32_t region : {MEMORY_REGION_INTERNAL, MEMORY_REGION_EXTERNAL, MEMORY_REGION_STEALABLE}) {
		MemoryRegion& r = allocator.regions[region];
		for (int32_t j = 0; j < r.emptySpaces.getNumElements(); j++) {
			free += static_cast<EmptySpaceRecord*>(r.emptySpaces.getElementAddress(j))->length;
		}
	}
	return free;
}

// ---- Outputs ----

Kit* makeKit(Song* song, const char* name, int32_t lanes, Error* error) {
	ParamManagerForTimeline kitParamManager;
	Kit* kit = static_cast<Kit*>(StorageManager::createNewInstrument(OutputType::KIT, &kitParamManager));
	if (kit == nullptr) {
		*error = Error::INSUFFICIENT_RAM;
		return nullptr;
	}
	kit->name.set(name);
	kit->dirPath.set("KITS");
	song->backUpParamManager(kit, nullptr, &kitParamManager, true);
	static const char* const kNames[] = {"Kick", "Snare", "Clap", "Rim",   "HatC", "HatO",
	                                     "Ride", "Crash", "Tom",  "Perc1", "Perc2"};
	for (int32_t lane = 0; lane < lanes; lane++) {
		ParamManagerForTimeline paramManager;
		*error = paramManager.setupWithPatching();
		if (*error != Error::NONE) {
			return kit; // the song deletes it with everything else
		}
		auto* drum = static_cast<SoundDrum*>(StorageManager::createNewDrum(DrumType::SOUND));
		if (drum == nullptr) {
			*error = Error::INSUFFICIENT_RAM;
			return kit;
		}
		Sound::initParams(&paramManager);
		kit->addDrum(drum);
		drum->setupAsSample(&paramManager); // no sample file: the song's structure is what's measured
		drum->drumName = kNames[lane % 11];
		song->backUpParamManager(drum, nullptr, &paramManager, true);
	}
	song->addOutput(kit, false);
	return kit;
}

SoundInstrument* makeSynth(Song* song, const char* name, Error* error) {
	ParamManagerForTimeline paramManager;
	auto* synth = static_cast<SoundInstrument*>(StorageManager::createNewInstrument(OutputType::SYNTH, &paramManager));
	if (synth == nullptr) {
		*error = Error::INSUFFICIENT_RAM;
		return nullptr;
	}
	synth->name.set(name);
	synth->dirPath.set("SYNTHS");
	synth->setupAsDefaultSynth(&paramManager);
	song->backUpParamManager(synth, nullptr, &paramManager, true);
	song->addOutput(synth, false);
	return synth;
}

// ---- Content ----

/// Notes a bar per kit lane: 4 kicks, 2 snares, 2 claps, 1 rim, 8 closed hats, 2 open hats, crash every 8 bars, tom,
/// 2 percs: 22. The FX kit gets 2 a bar. Bass 8, Lead 4, Chords 3, Pad 1: 40 a bar in all.
constexpr int8_t kDrumHits[kDrumLanes] = {4, 2, 2, 1, 8, 2, 0, 0, 1, 1, 1};
constexpr int8_t kFxHits[kFxLanes] = {1, 0, 1, 0};
constexpr int8_t kSynthNotes[4] = {8, 4, 3, 1};
/// Drift: one node a bar on 2 params per kit row (11 + 4 rows) and 10 per synth: 22 + 8 + 40 = 70 params.
constexpr int32_t kRowDriftParams = 2;
constexpr int32_t kSynthDriftParams = 10;
constexpr params::ParamType kDriftParams[kSynthDriftParams] = {
    params::LOCAL_LPF_FREQ,        params::LOCAL_LPF_RESONANCE, params::LOCAL_HPF_FREQ, params::GLOBAL_VOLUME_POST_FX,
    params::LOCAL_ENV_0_RELEASE,   params::LOCAL_ENV_0_DECAY,   params::LOCAL_PAN,      params::GLOBAL_REVERB_AMOUNT,
    params::GLOBAL_DELAY_FEEDBACK, params::LOCAL_LPF_MORPH,
};

/// Appends evenly spaced notes for `bars` bars. Returns how many, or -1 if out of RAM.
int32_t addNotes(NoteRow* row, int32_t bars, int32_t perBar, int32_t barLength, int32_t barOffset) {
	if (perBar <= 0) {
		// A crash on the first bar of every 8.
		perBar = 0;
	}
	int32_t added = 0;
	for (int32_t bar = 0; bar < bars; bar++) {
		int32_t hits = perBar;
		if (perBar == 0 && (bar + barOffset) % 8 == 0) {
			hits = 1;
		}
		for (int32_t h = 0; h < hits; h++) {
			int32_t pos = bar * barLength + h * (barLength / hits);
			int32_t index = row->notes.getNumElements();
			if (row->notes.insertAtIndex(index) != Error::NONE) {
				return -1;
			}
			Note* note = row->notes.getElement(index);
			note->pos = pos;
			note->setLength(std::max<int32_t>(1, barLength / hits / 2));
			note->setVelocity(64 + (h * 13 + bar) % 60);
			note->setLift(kDefaultLiftValue);
			note->setProbability(kNumProbabilityValues);
			note->setIterance(kDefaultIteranceValue);
			note->setFill(FillMode::OFF);
			added++;
		}
	}
	return added;
}

/// One drift node a bar on `count` patched params. Returns how many, or -1 if out of RAM.
int32_t addDrift(ParamManager& paramManager, int32_t count, int32_t bars, int32_t barLength, Runner& runner) {
	PatchedParamSet* set = paramManager.getPatchedParamSet();
	if (set == nullptr) {
		return 0;
	}
	int32_t added = 0;
	for (int32_t p = 0; p < count; p++) {
		AutoParam& param = set->params[kDriftParams[p]];
		for (int32_t bar = 0; bar < bars; bar++) {
			int32_t value = static_cast<int32_t>((bar * 2654435761u + p * 40503u) >> 1) - (1 << 30);
			if (param.setNodeAtPos(bar * barLength, value, true) < 0) {
				return -1;
			}
			added++;
		}
		if (!runner.checkpoint()) {
			return added;
		}
	}
	return added;
}

} // namespace

void cancel() {
	if (!cancelFlag) {
		cancelTicks = now();
		cancelFlag = true;
	}
}

Song* build(const Shape& shape, Result* result) {
	*result = Result{};
	cancelFlag = false;
	uint32_t t0 = now();
	Runner runner{shape.yieldBudgetUs > 0
	                  ? static_cast<uint32_t>(static_cast<uint64_t>(shape.yieldBudgetUs) * DELUGE_CLOCKS_PER / 1000000u)
	                  : static_cast<uint32_t>(DELUGE_CLOCKS_PER / 1000u), // audio every 1 ms, as stock builds
	              shape.yieldBudgetUs > 0, t0, 0, 0};

	Song* song = currentSong;
	if (!shape.intoCurrentSong) {
		void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
		if (memory == nullptr) {
			result->error = static_cast<int32_t>(Error::INSUFFICIENT_RAM);
			return nullptr;
		}
		song = new (memory) Song();
		song->paramManager.setupUnpatched();
		GlobalEffectable::initParams(&song->paramManager);
		song->setupDefault();
	}

	Error error = Error::NONE;
	Instrument* outputs[kParts] = {};
	outputs[0] = makeKit(song, "GDRUMS", kDrumLanes, &error);
	const char* synthNames[4] = {"GBASS", "GLEAD", "GCHORDS", "GPAD"};
	for (int32_t i = 0; i < 4 && error == Error::NONE; i++) {
		outputs[1 + i] = makeSynth(song, synthNames[i], &error);
	}
	if (error == Error::NONE) {
		outputs[5] = makeKit(song, "GFX", kFxLanes, &error);
	}

	const int32_t barLength = static_cast<int32_t>(song->getBarLength());
	const int32_t occurrences = std::max<int32_t>(1, shape.occurrences);
	// Section lengths in whole 8-bar phrases where the song allows, the last one taking what's left.
	int32_t basePhrases = std::max<int32_t>(1, (shape.bars / occurrences + 4) / 8);
	int32_t pos = 0;
	int32_t barsDone = 0;
	InstrumentClip* previous[kParts] = {};
	int32_t sessionIndex = song->sessionClips.getNumElements();
	int32_t arrangementIndex = song->arrangementOnlyClips.getNumElements();

	for (int32_t o = 0; o < occurrences && error == Error::NONE && !cancelFlag; o++) {
		int32_t bars = (o == occurrences - 1) ? std::max<int32_t>(8, shape.bars - barsDone) : basePhrases * 8;
		bool inSession = !shape.intoCurrentSong || o < kMaxSessionSections;
		for (int32_t p = 0; p < kParts && error == Error::NONE; p++) {
			if (outputs[p] == nullptr) {
				continue;
			}
			if (freeBytes() < kMinFreeBytes) {
				error = Error::INSUFFICIENT_RAM;
				break;
			}
			void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(InstrumentClip));
			if (memory == nullptr) {
				error = Error::INSUFFICIENT_RAM;
				break;
			}
			auto* clip = new (memory) InstrumentClip(song);
			clip->loopLength = bars * barLength;
			clip->section = static_cast<uint8_t>(std::min<int32_t>(o, kMaxSessionSections));
			clip->activeIfNoSolo = false;
			error = clip->setAudioInstrument(outputs[p], song, true, nullptr, previous[p]);
			if (error != Error::NONE) {
				clip->~InstrumentClip();
				delugeDealloc(memory);
				break;
			}
			// Into the song before its rows are set up, so later clips of this Output can clone from it.
			if (inSession) {
				error = song->sessionClips.insertClipAtIndex(clip, sessionIndex++);
			}
			else {
				error = song->arrangementOnlyClips.insertClipAtIndex(clip, arrangementIndex++);
			}
			if (error != Error::NONE) {
				song->deleteClipObject(clip, false, InstrumentRemoval::NONE);
				break;
			}
			result->clips++;
			previous[p] = clip;

			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			ModelStackWithTimelineCounter* modelStack =
			    setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip);

			int32_t notes = 0;
			int32_t nodes = 0;
			if (outputs[p]->type == OutputType::KIT) {
				clip->setupAsNewKitClipIfNecessary(modelStack); // a row per lane, each with its own params
				const int8_t* hits = (p == 0) ? kDrumHits : kFxHits;
				for (int32_t r = 0; r < clip->noteRows.getNumElements() && notes >= 0 && nodes >= 0; r++) {
					NoteRow* row = clip->noteRows.getElement(r);
					int32_t perBar = r < ((p == 0) ? kDrumLanes : kFxLanes) ? hits[r] : 0;
					int32_t n = addNotes(row, bars, perBar, barLength, barsDone);
					int32_t d = addDrift(row->paramManager, kRowDriftParams, bars, barLength, runner);
					notes = n < 0 ? -1 : notes + n;
					nodes = d < 0 ? -1 : nodes + d;
					if (!runner.checkpoint()) {
						break;
					}
				}
			}
			else {
				int32_t synth = p - 1;
				ModelStackWithNoteRow* rowStack = clip->getOrCreateNoteRowForYNote(48 + synth * 7, modelStack);
				NoteRow* row = rowStack->getNoteRowAllowNull();
				if (row != nullptr) {
					notes = addNotes(row, bars, kSynthNotes[synth], barLength, barsDone);
				}
				nodes = addDrift(clip->paramManager, kSynthDriftParams, bars, barLength, runner);
			}
			if (notes < 0 || nodes < 0) {
				error = Error::INSUFFICIENT_RAM;
				break;
			}
			result->notes += notes;
			result->nodes += nodes;

			// The arranger plays the same clip at the occurrence's place.
			int32_t index = outputs[p]->clipInstances.insertAtKey(pos);
			if (index < 0) {
				error = Error::INSUFFICIENT_RAM;
				break;
			}
			ClipInstance* instance = outputs[p]->clipInstances.getElement(index);
			instance->length = clip->loopLength;
			instance->clip = clip;
			if (!runner.checkpoint()) {
				break;
			}
		}
		pos += bars * barLength;
		barsDone += bars;
		result->barsBuilt = barsDone;
	}
	runner.finish();
	if (cancelFlag) {
		result->cancelled = true;
		result->cancelLatencyUs = toUs(now() - cancelTicks);
	}
	result->buildUs = toUs(now() - t0);
	result->longestRunUs = toUs(runner.longestRun);
	result->yields = runner.yields;
	result->error = static_cast<int32_t>(error);
	return song;
}

void deleteSong(Song* song) {
	if (song == nullptr || song == currentSong) {
		return;
	}
	void* toDealloc = dynamic_cast<void*>(song);
	song->~Song();
	delugeDealloc(toDealloc);
}

} // namespace deluge::generative::compose_shape

#endif // ENABLE_SYSEX_TEST
