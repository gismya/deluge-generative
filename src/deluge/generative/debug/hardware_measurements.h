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

// Hardware measurements for issue #37. Debug and RelWithDebInfo builds only (GENERATIVE_MEASUREMENTS); a release
// build compiles none of this. Results go to the debug log (D_PRINTLN, read over USB with sysex logging), one line per
// value starting with "MEAS", and a short summary goes to an OLED popup.
//
// Triggers: hold LEARN and press
//   SYNTH      load a synth preset (parse + samples), report time and RAM, delete it
//   KIT        load a factory kit (parse + samples), report time and RAM, delete it
//   MIDI       build a 12-lane kit from single samples on Perform's code path, report time and RAM per lane, delete it
//   CV         memory report: free RAM per allocator region, sample cache, audio gaps since the last report
//   KEYBOARD   pad redraw: render, queue and UART time of 16 full main-grid + sidebar redraws
//   SCALE      show the last summary popup again
// Automatic: entering Perform (SHIFT+SONG) times the roster build; loading a song times the load and, when it loads
// while another song plays, reports the RAM free with both songs in memory.
//
// Every timed measurement also watches the audio routine: "gap" is the longest time between two audio renders, and
// "late" counts gaps longer than the 128-sample output buffer (2.9 ms), which are audible dropouts.

#pragma once

#if GENERATIVE_MEASUREMENTS

#include "definitions_cxx.hpp"
#include "hid/button.h"
#include <cstdint>
#include <optional>

#define GEN_MEASURE(...) __VA_ARGS__

namespace deluge::generative::measure {

/// Called at the top of every AudioEngine::routine(). Cheap: one timer read and a compare.
void audioRoutineEntered();

/// The LEARN+button triggers. Returns a result if the press was one, otherwise nothing and the press goes on as usual.
std::optional<ActionResult> buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine);

/// A timed measurement: time per phase, RAM change per region, audio gaps. Reports when it goes out of scope. Kit
/// lanes built while one is open are timed and sized one by one (laneDone()). Only one is open at a time; a nested
/// one is ignored.
class Timed {
public:
	/// `error`, if given, is read when the measurement reports, so early returns report their error.
	explicit Timed(char const* name, Error const* error = nullptr);
	~Timed();
	Timed(const Timed&) = delete;
	Timed& operator=(const Timed&) = delete;

	/// Ends the current phase and starts the next. Phases are reported in order with their own time.
	void phase(char const* name);
	/// Also reports the free RAM per region at this point, e.g. while two songs are in memory.
	void phaseWithMemory(char const* name);
	void setError(Error error) { error_ = error; }

private:
	bool active_;
	Error const* errorSource_;
	Error error_ = Error::NONE;
};

/// A kit lane (drum) is complete. Called from the kit builder; does nothing unless a Timed is open.
void laneDone(int32_t lane);

} // namespace deluge::generative::measure

#else

#define GEN_MEASURE(...)

#endif
