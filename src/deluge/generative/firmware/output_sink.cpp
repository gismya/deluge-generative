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

// The Perform adapter's OutputSink: plays the Generator's events into the roster's Outputs, and hears the
// transition's points from the tick (spec §12.5, §12.9).

#include "generative/firmware/adapter_timing.h"
#include "generative/firmware/log_header.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/sound_controls.h"
#include "generative/firmware/sound_swaps.h"
#include "generative/perform/riser_timing.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"

namespace deluge::generative::firmware {

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

/// Test builds: whether the genre playing gives any part, row or kit a delay setup (spec §10.6), whose Throws allocate
/// delay buffers in the render: Dub's and Dub techno's, or the delay_setups hold's test roster.
bool playsDelaySetups() {
	const Genre* genre = currentRecipe.recipe.genre;
	return TestPort::delaySetupsHeld() || (genre != nullptr && genreHasDelaySetup(*genre));
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
		reportRiserTiming(event);
#endif
		note(roster, event, true);
		break;
	case EventKind::NoteOff:
#if ENABLE_SYSEX_TEST
		reportRiserTiming(event);
#endif
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
			// bar= is the piece's bar it starts on, so a script checks where a Section landed in the music, not when.
			TEST_EVENT("gen_section %s %u clip_pos=%ld bar=%ld", sectionName(event.section),
			           static_cast<unsigned>(event.occurrence), clipPos,
			           static_cast<long>(event.tick / (kStepsPerBar * kTicksPerStep)));
			if (playsDelaySetups()) {
				// Delay Throws allocate their buffers in the render and free them once the repeats run out (spec
				// §10.6): SDRAM at each Section line, around a phrase of them.
				reportFreeMemory("section");
			}
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
		soundSwaps.swap(event);
		break;
	}
}

int32_t OutputSink::sampleMicros(const Event& noteOn) {
	if (noteOn.part >= kMaxParts || noteOn.lane == Lane::None) {
		return 0;
	}
	const PartOutput& part = roster.parts[noteOn.part];
	return sampleRowMicros(part.output != nullptr ? part.drums[laneRow(noteOn.lane)] : nullptr);
}

bool OutputSink::tempoFollowsMaster() {
	return playbackHandler.isExternalClockActive();
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

void OutputSink::transitionLanded() {
	transitions.landed();
}

void OutputSink::transitionDropped() {
	transitions.dropped();
}

#if ENABLE_SYSEX_TEST
void OutputSink::reportRiserTiming(const Event& event) {
	if (event.lane != Lane::Riser) {
		return;
	}
	if (event.kind == EventKind::NoteOn) {
		riserOnSampleTime = AudioEngine::audioSampleTimer | 1; // never 0, which means none sounding
		riserSampleMicros = sampleMicros(event);
		TEST_EVENT("gen_riser_on sample_ms=%ld", static_cast<long>(riserSampleMicros / 1000));
		return;
	}
	if (riserOnSampleTime == 0) {
		return;
	}
	// From the Riser's note-on to its note-off on the audio clock, against its sample's length: the file should end
	// within one 16th of the Drop (spec §9.5). A forced Section's note-off reads early, so ok=0.
	const auto toDropMs =
	    static_cast<long>(uint64_t{AudioEngine::audioSampleTimer - riserOnSampleTime} * 1000 / kSampleRate);
	const auto sampleMs = static_cast<long>(riserSampleMicros / 1000);
	const auto tempo = static_cast<int32_t>(playbackHandler.calculateBPMForDisplay() * 100); // hundredths of a BPM
	const long sixteenthMs = tempo > 0 ? perform::stepMicros(tempo) / 1000 : 0;
	const bool ok = sampleMs > 0 && std::abs(toDropMs - sampleMs) <= sixteenthMs;
	TEST_EVENT("gen_riser ok=%d sample_ms=%ld to_drop_ms=%ld sixteenth_ms=%ld", ok ? 1 : 0, sampleMs, toDropMs,
	           sixteenthMs);
	riserOnSampleTime = 0;
}
#endif

void OutputSink::playOutgoing(const Event& event) {
	// Only note-offs, hits leaning past the start step and its Throws' Space (spec §11.4) come from the outgoing
	// instance after it.
	if (event.kind == EventKind::NoteOn || event.kind == EventKind::NoteOff) {
#if ENABLE_SYSEX_TEST
		reportRiserTiming(event); // a transition Build's Riser ends on the start step, from the outgoing instance
#endif
		note(outgoing, event, event.kind == EventKind::NoteOn);
	}
	else if (event.kind == EventKind::Control) {
		soundControls.applyOutgoing(event, rampTicks);
#if ENABLE_SYSEX_TEST
		if (event.control == SoundControl::Space) {
			outgoingSpaces++;
			TEST_EVENT("gen_outgoing_throw part=%u space=%d", static_cast<unsigned>(event.part),
			           static_cast<int>(event.offset));
		}
#endif
	}
}

void OutputSink::outgoingThrowsCut() {
	soundControls.restoreOutgoingHomes();
	TEST_EVENT("gen_outgoing_throws_cut");
}

#if ENABLE_SYSEX_TEST
namespace {
/// When the piece being opened began opening: gen_log_piece's us= counts from here.
uint32_t pieceOpeningTicks = 0;
} // namespace
#endif

void OutputSink::pieceOpening() {
#if ENABLE_SYSEX_TEST
	pieceOpeningTicks = getSystemTicks();
#endif
}

void OutputSink::logPieceHeader(SteeringLog& log) {
	const Generator* playing = performAdapter.driver().playing();
	if (playing == nullptr || performAdapter.song_ == nullptr) {
		return;
	}
	firmware::logPieceHeader(log, currentRecipe.sounds, roster, *playing->recipe().genre, *performAdapter.song_);
	// header: the newest piece's opening and header records, in bytes; dropping: the bytes of dropped pieces whose
	// room compactStep() is still to close (spec §17.4); us: what opening the piece and its header cost in the tick.
	TEST_EVENT("gen_log_piece pieces=%ld header=%ld used=%ld full=%d dropping=%ld us=%lu",
	           static_cast<long>(log.pieces()), static_cast<long>(log.piece(log.pieces() - 1).headerBytes()),
	           static_cast<long>(log.used()), log.full() ? 1 : 0, static_cast<long>(log.droppedBytes()),
	           ticksToNs(getSystemTicks() - pieceOpeningTicks) / 1000);
}

void OutputSink::compactLog(SteeringLog& log) {
#if ENABLE_SYSEX_TEST
	const uint32_t started = getSystemTicks();
#endif
	[[maybe_unused]] const int32_t moved = log.compactStep(SteeringLog::kCompactStepBytes);
#if ENABLE_SYSEX_TEST
	const unsigned long us = ticksToNs(getSystemTicks() - started) / 1000;
#endif
	// moved: the bytes this step moved down; left: dropped bytes still to close (0: done, used= has come down); us:
	// what it cost in the tick, outside the decision (spec §17.4, §20).
	TEST_EVENT("gen_log_compact moved=%ld left=%ld used=%ld us=%lu", static_cast<long>(moved),
	           static_cast<long>(log.droppedBytes()), static_cast<long>(log.used()), us);
}

void OutputSink::logSoundPaths(SteeringLog& log, int32_t step, uint8_t part, uint8_t token) {
	if (const Generator* playing = performAdapter.driver().playing()) {
		firmware::logSoundPaths(log, step, part, token, SoundSwaps::waitingPicks(), *playing->recipe().genre);
	}
}

void OutputSink::outgoingDrained() {
	allNotesOff(outgoing); // the safety net, as after a flush (spec §12.5); tails ring out
	soundControls.forgetOutgoing();
#if ENABLE_SYSEX_TEST
	TEST_EVENT("gen_outgoing_drained throw_spaces=%lu", static_cast<unsigned long>(outgoingSpaces));
	outgoingSpaces = 0;
#endif
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

#if ENABLE_SYSEX_TEST
const char* OutputSink::partName(const Sound* sound, const Genre* genre) const {
	for (int32_t p = 0; genre != nullptr && p < partCount(*genre); p++) {
		if (soundsOf(roster.parts[p]).contains(sound)) {
			return genre->parts[p].name;
		}
	}
	for (const PartOutput& part : outgoing.parts) {
		if (soundsOf(part).contains(sound)) {
			return "outgoing";
		}
	}
	return "other";
}
#endif

} // namespace deluge::generative::firmware
