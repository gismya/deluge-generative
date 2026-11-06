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

// Generative mode POC: the Perform Driver. It owns the running Generator, builds the Techno roster's Outputs into
// the song, and plays the Generator's events straight into those Outputs from the Session tick (ADR 0002).

#pragma once

#include "definitions_cxx.hpp"
#include "generative/core/generator.h"
#include <cstdint>

class Song;
class Kit;
class Drum;
class SoundInstrument;
class InstrumentClip;
class Instrument;

class Sound;

namespace deluge::generative::perform {

/// Forwards the Generator's events to the roster's Outputs: Drums lanes to the Kit's drums through
/// Kit::noteOnPreKitArp, Bass to the synth through MelodicInstrument::sendNote. Builds its ModelStacks per call.
class OutputSink final : public EventSink {
public:
	void noteOn(int32_t tick, PartId part, uint8_t lane, int16_t note, uint8_t velocity) override;
	void noteOff(int32_t tick, PartId part, uint8_t lane, int16_t note) override;

	/// Safety net after Generator::stop(): note-off every voice on every part's Output. Tails ring out.
	void allNotesOff();

	Kit* kit = nullptr;
	InstrumentClip* kitClip = nullptr;
	Drum* drums[kNumLanes] = {};
	SoundInstrument* bass = nullptr;
	InstrumentClip* bassClip = nullptr;

private:
	Drum* drumForLane(uint8_t lane, int32_t* noteRowIndex);
};

class PerformDriver {
public:
	/// Builds the roster into `song` (card reads; UI context only), sets the tempo and arms the tick hook.
	/// On failure nothing is left in the song.
	Error enter(Song* song);
	/// Stops the Generator, sends note-offs and removes the roster from `song`.
	void exit(Song* song);

	/// The Session tick hook. `swungPos` is the swung tick count since playback started.
	/// @return Swung ticks until the hook next needs calling (INT32_MAX while not armed).
	int32_t doTickForward(int64_t swungPos);

	/// Transport stopped: stop the Generator and silence the roster. The next tick restarts it.
	void transportStopped();

#if ENABLE_SYSEX_TEST
	/// H3 stand-in: up to `count` note-ons now, in §12.6 role order: the roster's Kick, its other drum lanes, the
	/// song's other kit rows, the Bass, then the song's other synths. Returns how many it sent; `soundsOut` gets each
	/// note's Sound in send order.
	int32_t testBurst(int32_t count, ::Sound** soundsOut);
	/// Note-offs for the last testBurst().
	void testBurstOff();
	/// #83 stand-in for a Build roll: one hit on the Clap lane, with its note-off at once.
	void testRollHit(uint8_t velocity);
#endif

	void setRecipe(const Recipe& recipe);
	[[nodiscard]] const Recipe& recipe() const { return recipe_; }
	[[nodiscard]] bool armed() const { return armed_; }
	[[nodiscard]] bool running() const { return running_; }
	/// Last step that started playing, counted from the Generator's tick 0; -1 before the first step or when stopped.
	[[nodiscard]] int32_t currentStep() const;

private:
	void stopGenerator();
	void removeRoster(Song* song);

	Generator generator_;
	OutputSink sink_;
	Recipe recipe_;
	uint32_t seed_ = 1;
	bool armed_ = false;
	bool running_ = false;
	int64_t originTick_ = 0;   ///< Swung firmware tick that is the Generator's tick 0.
	int64_t lastSwungPos_ = 0; ///< To spot a transport restart without a stop.
};

extern PerformDriver performDriver;

/// Assembles a Kit named `kitName` with one card sample per drum (#28): drum i plays `samplePaths[i]` and is named
/// `drumNames[i]` (names must be unique). The first drum gets the kick's sidechain send. The Kit stays out of the
/// song's Output list; its ParamManager and each drum's are backed up in `song`, ready for
/// InstrumentClip::setAudioInstrument. Reads the card, so UI context only. On failure nothing is left in the song.
Kit* buildSampleKit(Song* song, char const* kitName, char const* const samplePaths[], char const* const drumNames[],
                    int32_t numDrums, Drum* drumsOut[], Error* error);

/// Gives `instrument` an empty, always-active, looping 1-bar session clip and links it into the song.
/// On failure the instrument is deleted and nothing is left in the song.
InstrumentClip* linkWithClip(Song* song, Instrument* instrument, Error* error);
/// Removes a clip that linkWithClip() added, and its Output with it.
void unlinkClip(Song* song, InstrumentClip* clip);

/// Deletes an Instrument that was never linked into the song's Output list.
void deleteUnlinkedInstrument(Song* song, Instrument* instrument);

/// The default POC Recipe: A minor, every macro at 25.
Recipe defaultRecipe();

} // namespace deluge::generative::perform
