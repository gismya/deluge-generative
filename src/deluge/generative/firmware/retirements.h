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

// Retiring Outputs (spec §12.9): an Output a sound re-roll or a transition has replaced, or one built for something
// cancelled, is freed in task context by a live runner job. One that played is freed once it is silent, or at its cap,
// a phrase after it stopped playing: if it still sounds there (an echo at the highest delay feedback never dies), its
// volume fades over a beat first, then it gets note-offs and is freed.

#pragma once

#include "generative/core/types.h"
#include "generative/firmware/perform_song.h"
#include "generative/perform/sound_retirements.h"
#include <cstdint>

namespace deluge::generative::firmware {

/// The UI's answer when it is asked to let go of an Output.
enum class ReleaseAnswer : uint8_t {
	Released, ///< No UI reference is on the Output now.
	Held,     ///< A UI still uses it (a UI over a sound editor open on it): it waits, and is asked again.
};
/// Asks the UI to let go of `output`, which is due to be freed: the gold knobs, and what follows them, move off it,
/// and a sound editor open on it closes if it can. Task context.
using ReleaseUi = ReleaseAnswer (*)(const PartOutput& output);

class Retirements {
public:
	/// Who retired an Output: a transition waits for its outgoing roster to go before it is over.
	enum class Owner : uint8_t { Sound, Transition };
	/// When an Output is freed.
	enum class When : uint8_t {
		Now,       ///< it never played: something cancelled
		AfterTail, ///< it played: once silent, or at its cap, faded over a beat first
	};

	/// Each owner has a share of the pool, and checks it before it can retire (spec §12.9): a sound press is refused
	/// with BUSY when its share hasn't room for every re-roll that may still retire an Output
	/// (perform::SoundSwapLifecycle::retirementsOwed()), and a transition request when its share hasn't room for the
	/// rosters it may still retire (Transitions::retirementRoom()). So no retire() finds the pool full.
	/// Sound re-rolls: perform::kSoundRetirements, which the portable lifecycle and the harness read too.
	static constexpr int32_t kSoundRetirements = perform::kSoundRetirements;
	/// Transitions: two rosters, the outgoing one and the incoming one of a transition a later request replaced.
	static constexpr int32_t kTransitionRetirements = 2 * kMaxParts;
	/// Every Output that can be retiring at once.
	static constexpr int32_t kCapacity = kSoundRetirements + kTransitionRetirements;

	/// What a When::Now caller passes for the cap, which Now ignores.
	static constexpr uint32_t kNoCap = 0;

	/// Queues `output` to be freed. AfterTail's cap is `capSamples` from now, with no default: 0 makes it due at once,
	/// so a tail still sounding gets only the beat's fade. Now ignores the cap.
	void retire(const PartOutput& output, When when, Owner owner, uint32_t capSamples);
	/// Queues each Output of `roster` to be freed, as retire() does, and empties it.
	void retireRoster(Roster& roster, When when, Owner owner, uint32_t capSamples);
	/// Outputs of `owner` waiting to be freed or being freed.
	[[nodiscard]] int32_t pending(Owner owner) const;
	/// How many more Outputs `owner` may retire: its share less pending(owner).
	[[nodiscard]] int32_t room(Owner owner) const {
		return (owner == Owner::Sound ? kSoundRetirements : kTransitionRetirements) - pending(owner);
	}

	/// Perform's song is going: every Output here goes with it.
	void forget();

	/// One phrase of `genre` at the song's tempo now, in samples: an Output's cap, which runs on while stopped too.
	[[nodiscard]] static uint32_t phraseSamples(const Genre& genre);

#if ENABLE_SYSEX_TEST
	/// freePartOutput()'s test-build check found a reference still on an Output it was about to free: nothing more is
	/// handed to the free job, so a script fails. Until Perform is left.
	void stopAtReference() { referenceFound_ = true; }
#endif

private:
	friend class FreeJob;
	friend class Pump;
	/// The pump's call: starts fades at caps, and hands what is due to the free job once the runner is free. Each
	/// Output due is first released by the UI (`release`); one it still holds waits (a sound editor open on it under
	/// another UI, spec §13.7), and the others go on.
	void service(ReleaseUi release);

	enum class State : uint8_t { Waiting, Fading };
	struct Retiring {
		PartOutput output;
		When when = When::Now;
		State state = State::Waiting;
		Owner owner = Owner::Sound;
		uint32_t until = 0; ///< AfterTail: the cap, then the fade's end, in AudioEngine::audioSampleTimer
	};
	Retiring retiring_[kCapacity];
	int32_t count_ = 0;
#if ENABLE_SYSEX_TEST
	/// See stopAtReference().
	bool referenceFound_ = false;
#endif
};

extern Retirements retirements;

} // namespace deluge::generative::firmware
