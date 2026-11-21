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

// The goldens' case (spec §21.2): what each Techno subgenre's golden plays, which the harness's golden_spec checks and
// the firmware check (spec §21.3) builds through Compose in delugemu under the test port's "hold golden".

#pragma once

#include <cstdint>

namespace deluge::generative::golden_case {

constexpr uint32_t kEntropy = 0; ///< The default Recipe: the genre's new Recipe from entropy 0.
constexpr uint32_t kSeed = 1;
/// From SHIFT+PLAY: the Groove at the Recipe's macros from the first bar, with no Intro.
constexpr bool kSkipIntro = true;

} // namespace deluge::generative::golden_case
