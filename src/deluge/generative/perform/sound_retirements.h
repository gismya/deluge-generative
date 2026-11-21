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

#pragma once

#include "generative/core/types.h"
#include <cstdint>

namespace deluge::generative::perform {

/// The sound re-rolls' share of the Outputs waiting to be freed (spec §12.9): each part's swapped-out Output, a phrase
/// at most, plus each part's waiting or preloading one that a transition's confirm drops (one past its last boundary
/// ends after the confirm and is retired then). A part has one waiting or preloading Output at most, besides its
/// swapped-out one: a preload starts only for a part with nothing pending. The firmware's Retirements gives them this
/// share; a press is refused with BUSY when it hasn't room. Its own header, so the firmware's Retirements reads it
/// without the lifecycle.
constexpr int32_t kSoundRetirements = 2 * kMaxParts;

} // namespace deluge::generative::perform
