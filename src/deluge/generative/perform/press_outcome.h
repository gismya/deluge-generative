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

// What a steering press in Perform did (spec §2.6, §12.9, §12.10), and which presses a pending transition refuses.
// Every press returns a PressOutcome, and the firmware turns it into a popup in one place
// (gui/ui/generative_popups.h).

#pragma once

#include <cstdint>

namespace deluge::generative::perform {

/// What a steering press did.
enum class PressOutcome : uint8_t {
	Accepted,      ///< it acted, queued, or cancelled what waited
	NothingToDo,   ///< nothing to act on (stopped, no Perform song, leaving, or no take that way): no popup
	QueueFull,     ///< the steering queue had no room for a discrete command: refused (QUEUE FULL, spec §2.6)
	ChangePending, ///< a transition is pending: refused (CHANGE PENDING, spec §12.9)
	Busy,          ///< the runner is busy: refused (spec §12.10)
};

/// Whether `outcome` refused the press: the press changed nothing, and its popup says why.
[[nodiscard]] constexpr bool refused(PressOutcome outcome) {
	return outcome == PressOutcome::QueueFull || outcome == PressOutcome::ChangePending
	       || outcome == PressOutcome::Busy;
}

/// A discrete command's push: refused when the queue had no room.
[[nodiscard]] constexpr PressOutcome queued(bool pushed) {
	return pushed ? PressOutcome::Accepted : PressOutcome::QueueFull;
}

/// The player's steering presses, for the rule below. A transition request has its own rule
/// (TransitionLifecycle::request()). Only the kinds the rule refuses are passed to TransitionLifecycle::gate() today;
/// the rest (Mute, Solo, Hold, Force, Back, Freeze, EndCancel) are here so the enum is the whole §12.9 table, and for
/// stage 2 of #197, where every press goes through the gate.
enum class PressKind : uint8_t {
	Mute,
	Solo,
	Hold,
	Force,
	Back, ///< a BACK tap: cancels the newest queued action
	Freeze,
	Take, ///< a take pad, a Y turn, or a SEED + TAKES turn
	Key,  ///< the key and scale editor, or MIDI TRANSPOSE
	Tempo,
	Swing,
	SwingInterval,
	EndArm,
	EndCancel,
	Sound, ///< NEW SOUND, or back to the previous sound
};

/// While a transition is pending, tempo, swing and swing interval, key edits, End, take and sound actions are refused;
/// macros, force, hold, mute, solo, freeze and BACK work as usual (spec §12.9). A tap that cancels End is not refused:
/// it only takes back an End armed before the confirm.
[[nodiscard]] constexpr bool refusedWhilePending(PressKind kind) {
	switch (kind) {
	case PressKind::Take:
	case PressKind::Key:
	case PressKind::Tempo:
	case PressKind::Swing:
	case PressKind::SwingInterval:
	case PressKind::EndArm:
	case PressKind::Sound:
		return true;
	case PressKind::Mute:
	case PressKind::Solo:
	case PressKind::Hold:
	case PressKind::Force:
	case PressKind::Back:
	case PressKind::Freeze:
	case PressKind::EndCancel:
		return false;
	}
	return false;
}

} // namespace deluge::generative::perform
