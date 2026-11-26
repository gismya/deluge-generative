// A genre with no variant sets, for specs of what such a genre does (spec §3.6).

#pragma once

#include "generative/core/genre.h"

namespace harness {

/// Dub with its variant sets taken off. Every built-in genre lists sets since Techno's (#531).
inline const deluge::generative::Genre& genreWithNoSets() {
	static const deluge::generative::Genre genre = [] {
		deluge::generative::Genre copy = deluge::generative::genres::dub;
		copy.variantSets = {};
		return copy;
	}();
	return genre;
}

} // namespace harness
