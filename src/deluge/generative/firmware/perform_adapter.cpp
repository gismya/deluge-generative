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

#include "generative/firmware/perform_adapter.h"
#include "OSLikeStuff/scheduler_api.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_midi.h"
#include "generative/firmware/retirements.h"
#include "generative/firmware/runner.h"
#include "generative/firmware/sound_controls.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/firmware/transitions.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "io/midi/test_port.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/global_effectable/global_effectable.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/sample/sample.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include <algorithm>
#include <iterator>
#include <new>

extern void setUIForLoadedSong(Song* song);

namespace deluge::generative::firmware {

PerformAdapter performAdapter{};

namespace {

#if ENABLE_SYSEX_TEST
/// Nanoseconds in a span of getSystemTicks() (33.33 MHz).
unsigned long ticksToNs(uint32_t ticks) {
	return static_cast<unsigned long>(uint64_t{ticks} * 1'000'000'000 / DELUGE_CLOCKS_PER);
}

/// H1 (spec §20): the cost of the step decisions in the tick. Reported as a test event every 4 bars, then reset.
class DecisionTiming final : public perform::DecisionTimer {
public:
	void decisionsBegin() override { started_ = getSystemTicks(); }
	void decisionsEnd(int32_t decided) override {
		uint32_t counts = getSystemTicks() - started_;
		steps_ += decided;
		totalCounts_ += counts;
		uint32_t perStep = counts / decided;
		maxCounts_ = perStep > maxCounts_ ? perStep : maxCounts_;
		if (steps_ >= 4 * kStepsPerBar) {
			TEST_EVENT("gen_h1 steps=%lu mean_ns=%lu max_ns=%lu", static_cast<unsigned long>(steps_),
			           ticksToNs(totalCounts_ / steps_), ticksToNs(maxCounts_));
			steps_ = totalCounts_ = maxCounts_ = 0;
		}
	}

private:
	uint32_t started_ = 0;
	uint32_t steps_ = 0;
	uint32_t totalCounts_ = 0;
	uint32_t maxCounts_ = 0;
};
DecisionTiming decisionTiming;

/// H2 (spec §20): what applying Control events costs in the tick, per step that sends any. The ramps' cost in the
/// render is AudioEngine::renderTiming.
class ControlTiming {
public:
	void begin() { started_ = getSystemTicks(); }
	void end(int32_t tick) {
		const uint32_t counts = getSystemTicks() - started_;
		if (!stepOpen_ || tick != stepTick_) {
			closeStep(); // the events of one step share its grid line's tick
			stepTick_ = tick;
		}
		stepCounts_ += counts;
		stepOpen_ = true;
	}
	/// Reports and resets, with the render timing since the last report.
	void report(const char* ramps) {
		closeStep();
		AudioEngine::RenderTiming render = AudioEngine::renderTiming;
		AudioEngine::renderTiming = {};
		const auto renderMean =
		    static_cast<uint32_t>(render.samples > 0 ? uint64_t{render.counts} * 128 / render.samples : 0);
		TEST_EVENT("gen_h2 ramps=%s moved=%s steps=%lu params=%lu mean_ns=%lu max_ns=%lu render_ns=%lu "
		           "render_max_ns=%lu",
		           ramps, stepsSending_ > 0 ? "yes" : "no", static_cast<unsigned long>(stepsSending_),
		           static_cast<unsigned long>(soundControls.takeParamsMoved()),
		           stepsSending_ > 0 ? ticksToNs(totalCounts_ / stepsSending_) : 0UL, ticksToNs(maxCounts_),
		           ticksToNs(renderMean), ticksToNs(render.maxCounts));
		// H16 (spec §20): voices culled for CPU in the same 4 bars, the voices sounding now, the slowest render.
		TEST_EVENT("gen_h16 culls=%lu voices=%ld render_max_us=%lu", static_cast<unsigned long>(render.culls),
		           static_cast<long>(AudioEngine::getNumVoices()), ticksToNs(render.maxCounts) / 1000);
		stepsSending_ = totalCounts_ = maxCounts_ = 0;
	}
	void reset() {
		stepOpen_ = false;
		stepCounts_ = stepsSending_ = totalCounts_ = maxCounts_ = 0;
		soundControls.takeParamsMoved();
		AudioEngine::renderTiming = {};
	}

private:
	void closeStep() {
		if (!stepOpen_) {
			return;
		}
		stepsSending_++;
		totalCounts_ += stepCounts_;
		maxCounts_ = std::max(maxCounts_, stepCounts_);
		stepCounts_ = 0;
		stepOpen_ = false;
	}
	uint32_t started_ = 0;
	int32_t stepTick_ = 0;
	bool stepOpen_ = false;
	uint32_t stepCounts_ = 0;
	uint32_t stepsSending_ = 0;
	uint32_t totalCounts_ = 0;
	uint32_t maxCounts_ = 0;
};
ControlTiming controlTiming;

} // namespace

/// Free bytes on the heaps Perform uses, for the leave check (spec §13.8: memory after leaving matches memory before
/// entry) and a sound re-roll's. The stealable region holds the sample cache, so it's reported apart.
void reportFreeMemory(const char* when) {
	auto& allocator = GeneralMemoryAllocator::get();
	auto freeIn = [&](int32_t region) {
		uint32_t total = 0;
		auto& spaces = allocator.regions[region].emptySpaces;
		for (int32_t i = 0; i < spaces.getNumElements(); i++) {
			total += static_cast<EmptySpaceRecord*>(spaces.getElementAddress(i))->length;
		}
		return static_cast<unsigned long>(total);
	};
	TEST_EVENT("gen_mem %s internal=%lu external=%lu stealable=%lu", when,
	           freeIn(MEMORY_REGION_INTERNAL) + freeIn(MEMORY_REGION_INTERNAL_SMALL),
	           freeIn(MEMORY_REGION_EXTERNAL) + freeIn(MEMORY_REGION_EXTERNAL_SMALL), freeIn(MEMORY_REGION_STEALABLE));
}

unsigned long msSince(uint32_t start) {
	return static_cast<unsigned long>((getSystemTicks() - start) / (DELUGE_CLOCKS_PER / 1000));
}

namespace {

/// Test hook: while the test port holds entry ("hold entry"), the entry job waits here, at its first boundary, so a
/// script can press BACK at a known point instead of racing the build. @return whether entry was cancelled meanwhile.
bool waitWhileEntryHeld(Chunks& chunks) {
	if (!TestPort::entryHeld()) {
		return false;
	}
	TEST_EVENT("gen_entry_held");
	while (TestPort::entryHeld()) {
		if (chunks.boundary()) {
			TestPort::releaseEntry();
			return true;
		}
	}
	return false;
}
#endif

} // namespace

// ---- OutputSink ----

namespace {
/// The ModelStack that plays `drum` on its row of `clip`, or nullptr if the drum has no row.
ModelStackWithThreeMainThings* drumModelStack(ModelStackWithTimelineCounter* clipStack, InstrumentClip* clip,
                                              Drum* drum) {
	int32_t rowIndex = -1;
	NoteRow* noteRow = drum ? clip->getNoteRowForDrum(drum, &rowIndex) : nullptr;
	if (noteRow == nullptr) {
		return nullptr;
	}
	return clipStack->addNoteRow(rowIndex, noteRow)
	    ->addOtherTwoThings(drum->toModControllable(), &noteRow->paramManager);
}

/// The Current Recipe's key and Perform's song key become `key` (spec §12.8): as a key change lands, or at once while
/// stopped.
void followKey(Key key) {
	currentRecipe.recipe.keyRoot = key.root;
	currentRecipe.recipe.scale = key.scale;
	writeSongKey(currentSong, key);
}

#if ENABLE_SYSEX_TEST
/// Test builds: whether a Generator tick is on a phrase line of the genre playing.
bool onPhraseLine(int32_t tick) {
	const Genre* genre = currentRecipe.recipe.genre;
	return genre != nullptr && tick % (genre->phraseSteps() * kTicksPerStep) == 0;
}
#endif
} // namespace

void OutputSink::play(const Event& event) {
	switch (event.kind) {
	case EventKind::NoteOn:
#if ENABLE_SYSEX_TEST
		if (event.tick >= fillTo) {
			TEST_EVENT("gen_fill_notes kick=%ld pitched=%ld drums=%ld", static_cast<long>(fillKicks),
			           static_cast<long>(fillPitched), static_cast<long>(fillDrums));
			fillFrom = fillTo = INT32_MAX;
		}
		else if (event.tick >= fillFrom && !event.muted) {
			fillKicks += event.lane == Lane::Kick ? 1 : 0;
			fillPitched += event.lane == Lane::None ? 1 : 0;
			fillDrums += event.lane != Lane::None && !isFxLane(event.lane) ? 1 : 0;
		}
#endif
		note(roster, event, true);
		break;
	case EventKind::NoteOff:
		note(roster, event, false);
		break;
	case EventKind::Section:
		section = event.section;
		occurrence = event.occurrence;
		sectionTick = event.tick;
		sectionKnown = true;
#if ENABLE_SYSEX_TEST
		{
			// Where the stock clips are in their 1-bar loop as the Section starts: 0 on the master's bar line.
			long clipPos = -1;
			for (const PartOutput& part : roster.parts) {
				if (part.clip != nullptr) {
					clipPos = static_cast<long>(part.clip->lastProcessedPos);
					break;
				}
			}
			TEST_EVENT("gen_section %s %u clip_pos=%ld", sectionName(event.section),
			           static_cast<unsigned>(event.occurrence), clipPos);
		}
#endif
		break;
	case EventKind::KeyScale:
		// A resolved key or scale change: the Current Recipe and Perform's song key follow it (spec §12.8).
		followKey({event.keyRoot(), event.scale});
		// Test builds: where it landed, a phrase line or not (a Drop can land one mid-phrase, spec §9.7).
		TEST_EVENT("gen_key root=%u scale=%u phrase_line=%s", static_cast<unsigned>(event.keyRoot()),
		           static_cast<unsigned>(event.scale), onPhraseLine(event.tick) ? "yes" : "no");
		break;
	case EventKind::Control:
#if ENABLE_SYSEX_TEST
		if (TestPort::rampsHeld() || TestPort::rampsStressed()) {
			break; // H2's comparison runs: the same piece with no live ramps, or every param ramping every step
		}
		controlTiming.begin();
#endif
		soundControls.apply(event, rampTicks);
#if ENABLE_SYSEX_TEST
		controlTiming.end(event.tick);
#endif
		break;
	case EventKind::Sound:
		// After the part's note-offs on this line, before its note-ons: the new sound plays them (spec §10.7).
		soundSwaps.swap(event, roster);
		break;
	}
}

void OutputSink::setTempo(int32_t tempo) {
	if (playbackHandler.isExternalClockActive() || tempo <= 0) {
		return; // the master owns tempo, and the glide is off (spec §12.7)
	}
	// setTempoFromNumSamples underneath, never logged (spec §12.7). Not setBPM: that writes UNPATCHED_TEMPO through
	// the live-input path.
	currentSong->setBPMInner(static_cast<float>(tempo) / 100.f, false);
}

void OutputSink::setSwing(int32_t amount) {
	currentSong->swingAmount = static_cast<int8_t>(amount);
}

void OutputSink::transitionStarts(int32_t /*startTick*/) {
	transitions.started();
}

void OutputSink::playOutgoing(const Event& event) {
	// Only note-offs and hits leaning past the start step come from the outgoing instance after it.
	if (event.kind == EventKind::NoteOn || event.kind == EventKind::NoteOff) {
		note(outgoing, event, event.kind == EventKind::NoteOn);
	}
}

void OutputSink::outgoingDrained() {
	allNotesOff(outgoing); // the safety net, as after a flush (spec §12.5); tails ring out
	transitions.outgoingDrained();
}

void OutputSink::note(const Roster& parts, const Event& event, bool on) {
	if (event.part >= kMaxParts) {
		return;
	}
	const PartOutput& part = parts.parts[event.part];
	if (part.output == nullptr || part.clip == nullptr) {
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(part.clip);
	int16_t mpeValues[kNumExpressionDimensions] = {};

	if (event.lane != Lane::None) {
		// Every Kit has a row per lane in the fixed order, so the lane's kind gives its drum (spec §5.4, §12.8).
		Drum* drum = part.drums[laneRow(event.lane)];
		ModelStackWithThreeMainThings* withDrum = drumModelStack(modelStack, part.clip, drum);
		if (withDrum == nullptr) {
			return;
		}
		auto* kit = static_cast<Kit*>(part.output);
		if (on) {
			kit->noteOnPreKitArp(withDrum, drum, event.velocity, mpeValues);
		}
		else {
			kit->noteOffPreKitArp(withDrum, drum);
		}
		return;
	}

	auto* synth = static_cast<SoundInstrument*>(part.output);
	ModelStackWithThreeMainThings* withSynth =
	    modelStack->addNoteRow(event.pitch, nullptr)
	        ->addOtherTwoThings(synth->toModControllable(), &part.clip->paramManager);
	static_cast<MelodicInstrument*>(synth)->sendNote(withSynth, on, event.pitch, on ? mpeValues : nullptr,
	                                                 MIDI_CHANNEL_NONE, event.velocity);
}

void OutputSink::allNotesOff() {
	allNotesOff(roster);
	allNotesOff(outgoing);
}

void OutputSink::allNotesOff(const Roster& parts) {
	for (const PartOutput& part : parts.parts) {
		if (part.output == nullptr || part.clip == nullptr) {
			continue;
		}
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack =
		    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(part.clip);
		if (part.output->type == OutputType::KIT) {
			// No Output-level allNotesOff: Kits need it per drum (spec §12.5).
			for (Drum* drum : part.drums) {
				ModelStackWithThreeMainThings* withDrum = drumModelStack(modelStack, part.clip, drum);
				if (withDrum != nullptr && drum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(drum);
					soundDrum->allNotesOff(withDrum, soundDrum->getArp());
				}
			}
		}
		else {
			auto* synth = static_cast<SoundInstrument*>(part.output);
			synth->allNotesOff(
			    modelStack->addOtherTwoThingsButNoNoteRow(synth->toModControllable(), &part.clip->paramManager),
			    synth->getArp());
		}
	}
}

// ---- Entry and leaving, as runner jobs (spec §12.10) ----

/// Perform entry (spec §14.3): modal; BACK cancels it until the swap.
class EntryJob final : public Job {
public:
	EntryJob() : Job(Kind::Modal) {}

	/// Before each submit: a job whose task can't start ends Failed without run(), so nothing else resets these.
	void prepare(const Genre& genre, EntryEnded ended) {
		genre_ = &genre;
		ended_ = ended;
		error_ = Error::NONE;
		soundsMissing_ = 0;
		ran_ = false;
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& chunks) override {
		ran_ = true;
		// A fresh Recipe for the genre (spec §14.1). Its sound picks read the card, so it rolls here, in the job; on a
		// card error the Current Recipe stays as it was. Once rolled, it stays the Current Recipe: a later cancel or a
		// load error leaves the new roll in place (spec §14.2).
		error_ = currentRecipe.rollFromGenre(*genre_, &chunks);
		if (error_ == Error::ABORTED_BY_USER) {
			return JobOutcome::Cancelled;
		}
		if (error_ != Error::NONE) {
			return JobOutcome::Failed;
		}
		if (chunks.boundary()) {
			return JobOutcome::Cancelled;
		}
#if ENABLE_SYSEX_TEST
		if (waitWhileEntryHeld(chunks)) {
			return JobOutcome::Cancelled;
		}
#endif
		Song* song = nullptr;
		Roster roster;
		error_ = buildPerformSong(currentRecipe.recipe, currentRecipe.sounds, currentRecipe.palette(), &song, &roster,
		                          chunks, &soundsMissing_);
		if (error_ == Error::ABORTED_BY_USER) {
			return JobOutcome::Cancelled;
		}
		if (error_ != Error::NONE) {
			return JobOutcome::Failed;
		}
		error_ = performAdapter.allocateBlocks();
		if (error_ == Error::NONE && chunks.boundary()) {
			error_ = Error::ABORTED_BY_USER;
		}
		if (error_ != Error::NONE) {
			performAdapter.freeBlocks();
			deletePrivateSong(song);
			return error_ == Error::ABORTED_BY_USER ? JobOutcome::Cancelled : JobOutcome::Failed;
		}
		chunks.commit(); // BACK no longer cancels, and nothing yields from here to the swap's end
		performAdapter.swapIn(song, roster);
		return JobOutcome::Done;
	}

	void finished(JobOutcome outcome) override {
#if ENABLE_SYSEX_TEST
		// H9 (spec §20): from the OK press to Perform's song swapped in.
		// ran=0: cancelled before its task ran.
		TEST_EVENT("gen_entry %s ran=%d ms=%lu",
		           outcome == JobOutcome::Done        ? "done"
		           : outcome == JobOutcome::Cancelled ? "cancelled"
		                                              : "failed",
		           ran_ ? 1 : 0, msSince(submitted_));
#endif
		if (outcome == JobOutcome::Done && soundsMissing_ > 0) {
			// One warning however many sounds fell back: a part on the default synth or a silent lane (spec §10.1).
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SOUND_MISSING));
			TEST_EVENT("gen_sound_missing sounds=%ld", static_cast<long>(soundsMissing_));
		}
		if (ended_ != nullptr) {
			// Failed with no error of its own: the task couldn't start.
			ended_(outcome, outcome == JobOutcome::Failed && error_ == Error::NONE ? Error::UNSPECIFIED : error_);
		}
	}

private:
	const Genre* genre_ = nullptr;
	EntryEnded ended_ = nullptr;
	uint32_t submitted_ = 0;
	bool ran_ = false;
	int32_t soundsMissing_ = 0;
	Error error_ = Error::NONE;
};

/// Tearing Perform's song down on leave (spec §12.10, §13.8): modal and never cancelled. With nothing kept it lands
/// on a new blank song in session view, as SHIFT+LOAD → NEW builds it, and the sample cache is empty.
class LeaveJob final : public Job {
public:
	LeaveJob() : Job(Kind::Modal) {}

	/// Before each submit, as EntryJob's.
	void prepare(LeaveEnded ended) {
		ended_ = ended;
		submitted_ = getSystemTicks();
	}

	JobOutcome run(Chunks& /*chunks*/) override {
		// MIDI Start or Continue, or a learned PLAY, may have restarted the transport since leave() stopped it.
		if (playbackHandler.playbackState) {
			playbackHandler.endPlayback();
		}
		performAdapter.dropSong();

		// From here this mirrors stock ClearSong::acceptCurrentOption() (gui/context_menu/clear_song.cpp) on its
		// stopped path, landing in session view. No yield from the delete to the swap: nothing may run without a
		// current song but the card loader's waits, as in Clear song.
		actionLogger.deleteAllLogs();
		nullifyUIs();
#if ENABLE_SYSEX_TEST
		uint32_t started = getSystemTicks();
#endif
		deleteOldSongBeforeLoadingNew(); // Perform's song: every Output of both rosters
		performAdapter.freeBlocks();
		deleteUnusedAudioFiles(); // before the blank song exists, so it lands in the same place every time
#if ENABLE_SYSEX_TEST
		deleteCounts_ = getSystemTicks() - started;
#endif

		void* songMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
		if (songMemory == nullptr) {
			// As Clear song: the old song is gone and the UIs are nullified, so there's nothing to resume into.
			FREEZE_WITH_ERROR("E458");
			while (true) {}
		}
		preLoadedSong = new (songMemory) Song();
		preLoadedSong->paramManager.setupUnpatched();
		GlobalEffectable::initParams(&preLoadedSong->paramManager);
		preLoadedSong->setupDefault();
		preLoadedSong->inClipMinderViewOnLoad = false; // session view (spec §13.8), not stock NEW's clip view
		preLoadedSong->ensureAtLeastOneSessionClip();  // loads a synth preset from the card
		playbackHandler.doSongSwap(false);
		audioFileManager.deleteAnyTempRecordedSamplesFromMemory();
		currentSong->loadAllSamples();
		return JobOutcome::Done;
	}

	[[nodiscard]] bool cancellable() const override { return false; }

	void finished(JobOutcome outcome) override {
		if (outcome != JobOutcome::Done) {
			// Only when the job's task couldn't start: run() never ran, and Perform's song is still whole and
			// current. Perform carries on; the caller says why.
			performAdapter.undoLeave();
			display->removeWorkingAnimation();
			if (ended_ != nullptr) {
				ended_(outcome);
			}
			return;
		}
		setUIForLoadedSong(currentSong);
		currentUIMode = UI_MODE_NONE;
		display->removeWorkingAnimation();
#if ENABLE_SYSEX_TEST
		TEST_EVENT("gen_left ms=%lu delete_us=%lu", msSince(submitted_), ticksToNs(deleteCounts_) / 1000);
		reportFreeMemory("after_leave");
#endif
		if (ended_ != nullptr) {
			ended_(outcome);
		}
	}

private:
	LeaveEnded ended_ = nullptr;
	uint32_t submitted_ = 0;
#if ENABLE_SYSEX_TEST
	uint32_t deleteCounts_ = 0;
#endif
};

namespace {
EntryJob entryJob;
LeaveJob leaveJob;
} // namespace

// ---- PerformAdapter ----

bool PerformAdapter::beginEntry(const Genre& genre, EntryEnded ended) {
	if (active() || runner.busy()) {
		return false;
	}
#if ENABLE_SYSEX_TEST
	reportFreeMemory("before_entry");
#endif
	entryJob.prepare(genre, ended);
	return runner.submit(entryJob); // only false if entryJob is already queued, which the busy check rules out
}

Error PerformAdapter::allocateBlocks() {
	// Two Generator blocks, from the internal heap, once per entry (spec §2.1).
	for (Generator*& block : blocks_) {
		void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Generator));
		if (memory == nullptr) {
			freeBlocks();
			return Error::INSUFFICIENT_RAM;
		}
		block = new (memory) Generator();
	}
	return Error::NONE;
}

void PerformAdapter::freeBlocks() {
	for (Generator*& block : blocks_) {
		if (block != nullptr) {
			block->~Generator();
			delugeDealloc(block);
			block = nullptr;
		}
	}
}

void PerformAdapter::swapIn(Song* song, const Roster& roster) {
	// Swap it in stopped (spec §14.3), as Clear song does. Perform keeps the undo log empty from entry (spec §12.9).
	if (playbackHandler.playbackState) {
		playbackHandler.endPlayback();
	}
	// RECORD armed in a stock view would stay on, and Perform can't turn it off (F9): disarm it.
	playbackHandler.recording = RecordingMode::OFF;
	playbackHandler.setLedStates();
	actionLogger.deleteAllLogs();
	nullifyUIs();
	deleteOldSongBeforeLoadingNew();
	preLoadedSong = song;
	playbackHandler.doSongSwap(false);
	audioFileManager.deleteAnyTempRecordedSamplesFromMemory(); // the old song's, as Clear song does
	// The swap leaves its loading mode and animation up for the caller to clear, as Clear song does.
	currentUIMode = UI_MODE_NONE;
	display->removeWorkingAnimation();

	sink_ = OutputSink{};
	sink_.roster = roster;
	queue_.clear();
	macroSteering_ = perform::MacroSteering{};
	song_ = song;
	generator_ = blocks_[0];
	newRecipe(); // a new Recipe: every take at 0 (spec §6.6)
}

void PerformAdapter::leave(LeaveEnded ended) {
	if (leaving_) {
		return; // the teardown already queued ends with its own callback
	}
	if (!active()) {
		if (ended != nullptr) {
			ended(JobOutcome::Failed);
		}
		return;
	}
	if (playbackHandler.playbackState) {
		playbackHandler.endPlayback(); // the view's playbackEnded() stops the piece
	}
	stopPiece();
	leaving_ = true; // the tick plays nothing from here; the teardown drops the song, roster and blocks
	leaveJob.prepare(ended);
	if (!runner.cancelAllThen(leaveJob)) {
		// Only if leaveJob were already queued, which leaving_ rules out: Perform carries on.
		leaving_ = false;
		if (ended != nullptr) {
			ended(JobOutcome::Failed);
		}
	}
}

void PerformAdapter::startPiece(int32_t swungTick, int32_t magnitude) {
	// Every start is fresh: the Arc resets and a new piece begins (spec §13.2). SHIFT+PLAY skips the Intro.
	const bool groove = startInGroove_;
	startInGroove_ = false;
	pieceEnded_ = false;
	pieces_++;
	generator_->start(currentRecipe.recipe, currentRecipe.seed, currentRecipe.takes, groove);
	// Every control starts at home: read from the parts' Outputs as they are now (spec §11.6).
	soundControls.begin(sink_.roster, *currentRecipe.recipe.genre);
	queue_.clear();
	macroSteering_.startFrom(generator_->recipe().macros);
	sink_.sectionKnown = false;
	// Where the master is: playback started from metronomeOffset, a Continue's song position pointer, or 0. Step 0
	// lands on its next bar line (spec §12.4).
	const int32_t master = playbackHandler.metronomeOffset + swungTick;
	driver_.begin(*generator_, master, magnitude);
#if ENABLE_SYSEX_TEST
	driver_.setDecisionTimer(&decisionTiming);
#endif
	// The next piece starts at the Recipe's tempo: the last one tracked from an external clock, too (spec §13.7).
	driver_.setTempo(currentRecipe.recipe.tempo, sink_);
	driver_.setSwing(currentRecipe.recipe.swing, sink_);
	lastSwungTick_ = swungTick; // the first delta is 0: no catch-up burst when the hook first runs late
	magnitude_ = magnitude;
	positionPending_ = false;
	instanceStarts();
#if ENABLE_SYSEX_TEST
	macrosShown_ = generator_->recipe().macros;
#endif
	TEST_EVENT("gen_start master=%ld to_step0=%ld groove=%d tempo=%ld", static_cast<long>(master),
	           static_cast<long>(-driver_.clock()), groove ? 1 : 0, static_cast<long>(currentRecipe.recipe.tempo));
}

void PerformAdapter::stopPiece() {
	if (driver_.running()) {
		driver_.stop(sink_);
	}
	sink_.allNotesOff();
	soundControls.restoreHomes(); // every sound back as its preset was, or as the player left it while stopped
}

void PerformAdapter::dropSong() {
	soundControls.forget();
	soundSwaps.forget();
	transitions.forget();
	retirements.forget();
	generator_ = nullptr;
	song_ = nullptr;
	sink_ = OutputSink{};
	leaving_ = false;
}

int32_t PerformAdapter::tick(int32_t swungTick) {
	if (!active() || leaving_ || currentSong != song_) {
		return INT32_MAX; // the hook runs for every song; only Perform's plays the Generator
	}
	const int32_t magnitude = currentSong->getInputTickMagnitude();
	// Matching can end on the same input tick that changes the magnitude, so a change counts as matching if it was
	// running at the last call or is now.
	const bool matching = matchingBefore_ || playbackHandler.tempoMagnitudeMatchingActiveNow;
	matchingBefore_ = playbackHandler.tempoMagnitudeMatchingActiveNow;
	if (!driver_.running()) {
		if (pieceEnded_) {
			return INT32_MAX; // the piece ended; the transport is stopping
		}
		if (transitions.holdsStart()) {
			// A stop committed a transition whose preload still runs: the new roster starts once it has loaded, on
			// the master's next bar line (spec §12.10).
			lastSwungTick_ = swungTick;
			return 1;
		}
		startPiece(swungTick, magnitude);
	}
	else if (magnitude != magnitude_) {
		// The firmware rescales its tick count with the magnitude, so the count moves only at the new rate.
		lastSwungTick_ = magnitude > magnitude_ ? lastSwungTick_ << (magnitude - magnitude_)
		                                        : lastSwungTick_ >> (magnitude_ - magnitude);
		// While an external Start's magnitude matching settles, that's all. Later, a clock scale change mid-piece
		// is a jump (spec §12.3): it would otherwise move the piece by its whole elapsed length.
		if (!matching) {
			driver_.jump(sink_);
			// Count from here: a small step back in the rescaled tick count must not read as a second jump.
			lastSwungTick_ = swungTick;
			TEST_EVENT("gen_jump scale magnitude=%ld", static_cast<long>(magnitude));
		}
		magnitude_ = magnitude;
	}
	if (positionPending_) {
		positionPending_ = false;
		driver_.jumpTo(positionTick_, magnitude, sink_);
		TEST_EVENT("gen_jump spp master=%ld to_line=%ld", static_cast<long>(positionTick_),
		           static_cast<long>(stepTick(generator_->nextStep()) - driver_.clock()));
	}
	const int32_t delta = swungTick - lastSwungTick_;
	lastSwungTick_ = swungTick;
	sink_.rampTicks = 6 << magnitude; // one unswung 16th in internal ticks (spec §11.7)
	const uint32_t jumps = driver_.jumps();
	sampleFill();
	int32_t wait = driver_.advance(delta, magnitude, queue_, nullptr, sink_);
#if ENABLE_SYSEX_TEST
	if (generator_->heldFill() != fillShown_) {
		// A held fill landed on the last decided step, or ended there.
		fillShown_ = generator_->heldFill();
		const int32_t step = generator_->nextStep() - 1;
		TEST_EVENT("gen_fill entry=%ld step=%ld", static_cast<long>(fillShown_), static_cast<long>(step));
		if (fillShown_ >= 0) {
			sink_.fillFrom = stepTick(step);
			sink_.fillTo = INT32_MAX;
			sink_.fillKicks = sink_.fillPitched = sink_.fillDrums = 0;
		}
		else if (sink_.fillFrom != INT32_MAX) {
			sink_.fillTo = stepTick(step);
		}
	}
	for (int32_t m = 0; m < kNumMacros; m++) {
		// A SetMacro (a knob turn, a KEYBOARD fader's hold or its spring back) landed on the last decided step.
		const auto macro = static_cast<Macro>(m);
		if (generator_->recipe().macros[macro] != macrosShown_[macro]) {
			macrosShown_.set(macro, generator_->recipe().macros[macro]);
			TEST_EVENT("gen_macro_landed macro=%ld units=%ld step=%ld", static_cast<long>(m),
			           static_cast<long>(macrosShown_[macro]), static_cast<long>(generator_->nextStep() - 1));
		}
	}
#endif
	// The Recipe keeps each part's take as it lands, so the next start, a save or a Compose has it (spec §6.6, §15).
	for (int32_t p = 0; p < kMaxParts; p++) {
		recordTake(p, generator_->take(p));
	}
	if (driver_.jumps() != jumps) {
		TEST_EVENT("gen_jump clock delta=%ld", static_cast<long>(delta));
	}
	if (driver_.ended()) {
		pieceEnded_ = true;
		// The Outro is over: the transport stops on its last line (spec §13.2). As stock's arrangement end, from
		// inside the tick.
		sink_.allNotesOff();
		TEST_EVENT("gen_piece_ended");
		playbackHandler.endPlayback();
		return INT32_MAX;
	}
	if (playbackHandler.isExternalClockActive() && generator_->nextStep() >= nextTempoSample_) {
		sampleExternalTempo();
	}
#if ENABLE_SYSEX_TEST
	if (TestPort::rampsStressed() && generator_->nextStep() != stressStep_) {
		// H2's worst case: every bound param rewritten on every step, alternating up and down.
		stressStep_ = generator_->nextStep();
		controlTiming.begin();
		soundControls.stress((stressStep_ & 1) != 0, sink_.rampTicks);
		controlTiming.end(stepTick(stressStep_));
	}
	if (generator_->nextStep() >= nextH2Report_) {
		nextH2Report_ += 4 * kStepsPerBar;
		controlTiming.report(TestPort::rampsStressed() ? "stress" : TestPort::rampsHeld() ? "off" : "on");
	}
#endif
	return wait;
}

void PerformAdapter::sampleExternalTempo() {
	nextTempoSample_ = (generator_->nextStep() / kStepsPerBar + 1) * kStepsPerBar;
	if (playbackHandler.tempoMagnitudeMatchingActiveNow) {
		return; // not settled yet
	}
	const int32_t bpm = static_cast<int32_t>(playbackHandler.calculateBPMForDisplay() + 0.5f);
	const int32_t tempo = std::clamp(bpm * 100, kMinTempo, kMaxTempo);
	if (tempo == currentRecipe.recipe.tempo) {
		return;
	}
	// Logged only when the rounded value changes (spec §13.7): through the queue, as driver-side steering.
	currentRecipe.recipe.tempo = tempo;
	queue_.push({CommandKind::SetTempo, 0, currentRecipe.recipe.tempo});
	TEST_EVENT("gen_ext_tempo bpm=%ld", static_cast<long>(tempo / 100));
}

void PerformAdapter::sampleFill() {
	// Fill state is the song's fill mode, so the MIDI-learnable FILL command works as the button does (spec §8.7). A
	// MIDI FILL has no SHIFT latched: it plays the held-fill entry.
	const bool down = currentSong->isFillModeActive();
	if (!down) {
		fillShift_ = false; // a SHIFT latched for a press the core never saw (FILL let go while stopped) goes with it
	}
	if (down == (fillSent_ != kFillReleased)) {
		return;
	}
	const int32_t value = !down ? kFillReleased : fillShift_ ? kFillFreshPick : kFillHeldEntry;
	if (!queue_.push({CommandKind::SetFill, 0, value})) {
		return; // a full queue: the next tick tries again
	}
	fillSent_ = value;
	fillShift_ = false;
	TEST_EVENT("gen_fill_press value=%ld", static_cast<long>(value));
}

void PerformAdapter::playbackEnded() {
	if (active() && currentSong == song_) {
		stopPiece();
		soundSwaps.playbackEnded();
		transitions.playbackEnded(); // a stop commits a transition (spec §12.9)
	}
	startInGroove_ = false;
	pieceEnded_ = false; // the next start is a fresh piece
}

void PerformAdapter::pressPlay(bool groove) {
	if (playbackHandler.playbackState) {
		playbackHandler.playButtonPressed(kInternalButtonPressLatency); // a stop
		return;
	}
	if (active() && !leaving_ && transitions.startWhenLoaded(groove)) {
		return; // it starts when the incoming roster has loaded
	}
	startPlayback(groove);
}

void PerformAdapter::startPlayback(bool groove) {
	setStartInGroove(groove);
	playbackHandler.playButtonPressed(kInternalButtonPressLatency);
	if (!playbackHandler.playbackState) {
		setStartInGroove(false); // nothing started
	}
}

void PerformAdapter::songPositionPointer(int32_t songTick) {
	if (active() && !leaving_ && currentSong == song_ && driver_.running()) {
		positionPending_ = true;
		positionTick_ = songTick;
	}
}

void PerformAdapter::armEnd() {
	if (active() && driver_.running() && !transitions.pending()) { // refused while a transition is pending (§12.9)
		queue_.push({CommandKind::End, 0, 1});
	}
}

void PerformAdapter::cancelEnd() {
	if (active() && driver_.running()) {
		queue_.push({CommandKind::End, 0, 0});
	}
}

bool PerformAdapter::endQueued() const {
	return active() && driver_.running() && generator_->endQueued();
}

bool PerformAdapter::steer(const Command& command) {
	if (!active() || leaving_ || !driver_.running()) {
		return true; // nothing plays: mutes, solos, holds and forces are lost on stop anyway
	}
	return queue_.push(command);
}

void PerformAdapter::playerEdited(const ModelStackWithAutoParam& stack, int32_t value) {
	if (!active() || leaving_ || !driver_.running()) {
		return; // stopped, the edit is on the Output: the next piece reads it as home (spec §11.7)
	}
	if (const auto edit = soundControls.playerEdited(stack, value)) {
		// Acts at once; the core only logs it, at the next undecided step (spec §11.6). Continuous, so it coalesces
		// per param and a full queue only loses the log entry.
		queue_.push({CommandKind::SetHome, edit->part, edit->home, edit->param, edit->lane});
	}
}

bool PerformAdapter::pressMute(uint8_t part, bool now) {
	return steer({CommandKind::SetMute, part, now ? kPressNow : kPress});
}

bool PerformAdapter::pressSolo(uint8_t part, bool now) {
	return steer({CommandKind::SetSolo, part, now ? kPressNow : kPress});
}

bool PerformAdapter::pressHold() {
	return steer({CommandKind::SetHold, 0, kPress});
}

bool PerformAdapter::forceSection(SectionKind kind) {
	return steer({CommandKind::ForceSection, static_cast<uint8_t>(kind), 0});
}

bool PerformAdapter::cancelNewest() {
	// It may cancel a waiting key change or take: menus showing one read it again.
	keyPresses_++;
	for (uint32_t& presses : takePresses_) {
		presses++;
	}
	return steer({CommandKind::CancelNewest});
}

bool PerformAdapter::pressTake(uint8_t part, int32_t take) {
	const Genre* genre = currentRecipe.recipe.genre;
	if (!active() || leaving_ || genre == nullptr || part >= partCount(*genre) || take < 0 || take >= kMaxTakes
	    || transitions.pending()) {
		return true; // a take press while a transition is pending is refused (spec §12.9): the caller says so
	}
	takePresses_[part]++;
	if (driver_.running()) {
		return queue_.push({CommandKind::SetTake, part, take});
	}
	// Stopped: no line to wait for. The take is the Recipe's, which the next start plays.
	recordTake(part, take);
	TEST_EVENT("gen_take part=%u take=%ld stopped", static_cast<unsigned>(part), static_cast<long>(take));
	return true;
}

bool PerformAdapter::pressFreeze(uint8_t part) {
	return steer({CommandKind::SetFreeze, part, kPress});
}

void PerformAdapter::instanceStarts() {
	restartStepCounters();
	// The Generator starts with no fill held: a FILL still down is sent again at once, with its SHIFT (a fresh pick
	// sent to the last instance, or one latched and not yet sent). A SHIFT latched for a FILL already let go goes.
	fillShift_ = currentSong->isFillModeActive() && (fillShift_ || fillSent_ == kFillFreshPick);
	fillSent_ = kFillReleased;
	// A KEYBOARD fader still held is sent again, as FILL is, unless the core has it or will (spec §13.5).
	for (int32_t m = 0; m < kNumMacros; m++) {
		const auto macro = static_cast<Macro>(m);
		if (const int32_t held = macroSteering_.resend(macro); held != perform::MacroSteering::kNotHeld) {
			pushMacro(macro, held);
		}
	}
#if ENABLE_SYSEX_TEST
	fillShown_ = -1;
	sink_.fillFrom = sink_.fillTo = INT32_MAX;
	controlTiming.reset();
#endif
}

void PerformAdapter::restartStepCounters() {
	// The external clock's tempo is first sampled on the first bar line after step 0: at the start the firmware's
	// measure of it hasn't settled.
	nextTempoSample_ = kStepsPerBar;
#if ENABLE_SYSEX_TEST
	nextH2Report_ = 4 * kStepsPerBar;
	stressStep_ = -1;
#endif
}

void PerformAdapter::newRecipe() {
	std::fill(std::begin(newestTake_), std::end(newestTake_), 0);
	pieces_++; // an open menu reads its key and takes again
}

void PerformAdapter::recordTake(int32_t part, int32_t take) {
	const auto value = static_cast<uint8_t>(take);
	currentRecipe.takes.take[part] = value;
	newestTake_[part] = std::max(newestTake_[part], value);
}

int32_t PerformAdapter::take(int32_t part) const {
	const Generator* piece = playingGenerator();
	return piece != nullptr ? piece->take(part) : currentRecipe.takes.take[part];
}

int32_t PerformAdapter::newestTake(int32_t part) const {
	return std::max<int32_t>(newestTake_[part], take(part));
}

void PerformAdapter::setTempo(int32_t tempo) {
	if (!active() || playbackHandler.isExternalClockActive() || transitions.pending()) {
		return;
	}
	currentRecipe.recipe.tempo = tempo;
	const int32_t ticks = glideLengthTicks();
	driver_.glideTempo(tempo, ticks, sink_);
	queue_.push({CommandKind::SetTempo, 0, tempo, static_cast<uint16_t>(ticks)});
	wakeForGlide();
}

void PerformAdapter::wakeForGlide() {
	// The glide starts at the hook's next call: ask for it on the next swung tick, so it starts at the press, and an
	// Instant swing lands on the next step line even when nothing else would run the hook there.
	if (driver_.running()) {
		playbackHandler.expectEvent();
	}
}

void PerformAdapter::setSwing(int32_t amount) {
	if (!active() || transitions.pending()) {
		return;
	}
	currentRecipe.recipe.swing = static_cast<int8_t>(amount);
	const int32_t ticks = glideLengthTicks();
	driver_.glideSwing(amount, ticks, sink_);
	queue_.push({CommandKind::SetSwing, 0, amount, static_cast<uint16_t>(ticks)});
	wakeForGlide();
}

int32_t PerformAdapter::phraseSteps() {
	const Genre* genre = currentRecipe.recipe.genre;
	return genre != nullptr ? genre->phraseSteps() : kStepsPerBar * 8;
}

int32_t PerformAdapter::glideLengthTicks() const {
	return perform::glideTicks(glide, phraseSteps());
}

void PerformAdapter::setSwingInterval(int32_t interval) {
	if (!active() || transitions.pending()) {
		return;
	}
	currentRecipe.recipe.swingInterval = static_cast<uint8_t>(interval);
	currentSong->changeSwingInterval(swingSyncLevel(interval));
	queue_.push({CommandKind::SetSwingInterval, 0, interval});
}

void PerformAdapter::setMacro(Macro macro, int32_t units) {
	currentRecipe.recipe.macros.set(macro, units);
	if (macroSteering_.centreSteers(macro)) {
		pushMacro(macro, currentRecipe.recipe.macros[macro]);
	}
	sendMacroFeedback(macro);
}

void PerformAdapter::holdMacro(Macro macro, int32_t units) {
	units = std::clamp<int32_t>(units, 0, kMacroFull);
	macroSteering_.hold(macro, units);
	pushMacro(macro, units);
}

void PerformAdapter::keepMacroCentre(Macro macro, int32_t units) {
	macroSteering_.letGo(macro);
	currentRecipe.recipe.macros.set(macro, units);
	sendMacroFeedback(macro);
	// The press steered the core already, unless a piece started during the tap (a start clears the queue): steer it
	// again then.
	if (macroSteering_.differs(macro, currentRecipe.recipe.macros[macro])) {
		pushMacro(macro, currentRecipe.recipe.macros[macro]);
	}
}

void PerformAdapter::springMacroBack(Macro macro) {
	macroSteering_.letGo(macro);
	pushMacro(macro, currentRecipe.recipe.macros[macro]);
}

void PerformAdapter::pushMacro(Macro macro, int32_t units) {
	if (active() && queue_.push({CommandKind::SetMacro, static_cast<uint8_t>(macro), units})) {
		macroSteering_.sent(macro, units);
	}
}

bool PerformAdapter::queueKeyChange(Key key) {
	if (!active() || !validKey(key) || transitions.pending()) {
		return false;
	}
	keyPresses_++;
	if (playingGenerator() == nullptr) {
		// Stopped: the Recipe's key at once, as a macro is, and the next start plays it (spec §13.5).
		followKey(key);
		TEST_EVENT("gen_key_set root=%u scale=%u stopped", static_cast<unsigned>(key.root),
		           static_cast<unsigned>(key.scale));
		return true;
	}
	const bool accepted = queue_.push({CommandKind::SetKey, key.root, static_cast<int32_t>(key.scale)});
	TEST_EVENT("gen_key_set root=%u scale=%u %s", static_cast<unsigned>(key.root), static_cast<unsigned>(key.scale),
	           accepted ? "queued" : "refused");
	return accepted;
}

bool PerformAdapter::keyChangePending() const {
	const Generator* piece = playingGenerator();
	return piece != nullptr && piece->keyChangePending();
}

const Genre* PerformAdapter::rosterGenre() const {
	const Generator* piece = playingGenerator();
	return piece != nullptr ? piece->recipe().genre : currentRecipe.recipe.genre;
}

int32_t PerformAdapter::rosterParts() const {
	const Genre* genre = rosterGenre();
	return genre != nullptr ? partCount(*genre) : 0;
}

perform::TakeRow PerformAdapter::takeRow(int32_t part) const {
	perform::TakeRow row;
	if (part < 0 || part >= rosterParts() || !active()) {
		return row;
	}
	const Generator* piece = playingGenerator();
	row.playing = take(part);
	row.waiting = piece != nullptr ? piece->takeWaiting(part) : -1;
	row.newest = newestTake(part);
	return row;
}

int32_t PerformAdapter::stepTake(uint8_t part, const perform::TakeRow& row, int32_t direction, bool* accepted) {
	const int32_t take = perform::steppedTake(row, direction);
	*accepted = take < 0 || pressTake(part, take);
	return take;
}

Key PerformAdapter::keyAhead() const {
	const Generator* piece = playingGenerator();
	return piece != nullptr ? piece->keyAhead() : Key{currentRecipe.recipe.keyRoot, currentRecipe.recipe.scale};
}

} // namespace deluge::generative::firmware
