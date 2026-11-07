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

#include "generative/core/genre.h"
#include <cstring>

namespace deluge::generative {

namespace {
const Genre* const kGenres[] = {&genres::techno};
static_assert(sizeof(kGenres) / sizeof(kGenres[0]) <= kMaxGenres);
} // namespace

std::span<const Genre* const> allGenres() {
	return kGenres;
}

namespace {

/// Whether `rule`, listed on part `part` (and on `lane` of it, or Lane::None for the part itself), reads something
/// generated before it.
bool targetsEarlier(const Genre& genre, int32_t part, Lane lane, const InterplayRule& rule) {
	const RuleTarget& target = rule.target;
	if (target.part >= genre.parts.size() || target.part > part) {
		return false;
	}
	const PartSpec& targetSpec = genre.parts[target.part];
	if (target.lane != Lane::None) {
		bool found = false;
		for (const LaneSpec& spec : targetSpec.lanes) {
			found = found || spec.lane == target.lane;
		}
		if (!found) {
			return false;
		}
	}
	if (target.part < part) {
		return true;
	}
	// Within one part, only a lane may read an earlier lane, in the fixed lane order (spec §5.4).
	return lane != Lane::None && target.lane != Lane::None
	       && static_cast<int32_t>(target.lane) < static_cast<int32_t>(lane);
}

} // namespace

bool interplayRulesValid(const Genre& genre) {
	for (int32_t p = 0; p < static_cast<int32_t>(genre.parts.size()); p++) {
		const PartSpec& part = genre.parts[p];
		if (!part.lanes.empty() && !part.rules.empty()) {
			return false; // a Kit part plays only its lanes' rules
		}
		for (const InterplayRule& rule : part.rules) {
			if (!targetsEarlier(genre, p, Lane::None, rule)) {
				return false;
			}
		}
		for (const LaneSpec& lane : part.lanes) {
			for (const InterplayRule& rule : lane.rules) {
				if (!targetsEarlier(genre, p, lane.lane, rule)) {
					return false;
				}
			}
		}
	}
	return true;
}

const Genre* findGenre(const char* name) {
	for (const Genre* genre : kGenres) {
		if (std::strcmp(genre->name, name) == 0) {
			return genre;
		}
	}
	return nullptr;
}

} // namespace deluge::generative
