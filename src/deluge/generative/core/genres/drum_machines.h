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

// The drum machines the genres' kits roll (spec §10.2): the tag that plays each machine on a Drums lane. A machine
// tag is a filename prefix (`909 Snare`); where that prefix names more than the machine's plain sound, the lane names
// its file (the 909's and the 808's kicks: `909 Kick dry`, `808 Kick sustained`). The drum-kit listening session's
// table of which machine has which lane: ~/.cache/deluge-generative-m2/tuning/drum-kits/round-1/kits.md.

#pragma once

#include "generative/core/palette.h"

namespace deluge::generative::genres::machines {

/// The plain 909 kick, not its distorted or dry ones (#375).
inline constexpr KitLane k909Lanes[] = {{Lane::Kick, "SAMPLES/DRUMS/Kick/909 Kick.wav"}};
inline constexpr DrumMachine k909{.name = "909", .tag = "909", .lanes = k909Lanes};

/// The plain 808 kick, not its distorted, sustained or descending ones.
inline constexpr KitLane k808Lanes[] = {{Lane::Kick, "SAMPLES/DRUMS/Kick/808 Kick.wav"}};
inline constexpr DrumMachine k808{.name = "808", .tag = "808", .lanes = k808Lanes};

/// Techno's 808: its distorted kick, the one Techno's Kick list has always named.
inline constexpr KitLane k808DistortedLanes[] = {{Lane::Kick, "808 Kick distorted"}};
inline constexpr DrumMachine k808Distorted{.name = "808", .tag = "808", .lanes = k808DistortedLanes};

inline constexpr DrumMachine kCR78{.name = "CR78", .tag = "CR78"};
inline constexpr DrumMachine kKR55{.name = "KR55", .tag = "KR55"};
inline constexpr DrumMachine kLinndrum{.name = "Linndrum", .tag = "Linndrum"};

/// Dean Daughters' Psy set (`SAMPLES/Artists/Dean Daughters/Psy`): a folder, its files named by sound (`KICK-Radar`),
/// so each lane names its prefix. A lane reaches it only if the lane's folders include the set's. No rim, crash or tom.
/// Its Kick is the pack's copies with their tails cut (`SAMPLES/DRUMS/Kick/Psy/GEN PSYKICK-*`, #497): PSYKICK- is in no
/// factory name, so the kit never rolls the set's own long kicks (only Psytrance plays the set).
inline constexpr KitLane kPsyLanes[] = {
    {Lane::Kick, "PSYKICK-"},  {Lane::Snare, "SNARE-"}, {Lane::Clap, "CLAP-"},  {Lane::ClosedHat, "CLHAT-"},
    {Lane::OpenHat, "OPHAT-"}, {Lane::Ride, "RIDE-"},   {Lane::Perc1, "PERC-"}, {Lane::Perc2, "SHAKER-"},
};
inline constexpr DrumMachine kPsy{.name = "Psy", .lanes = kPsyLanes};

} // namespace deluge::generative::genres::machines
