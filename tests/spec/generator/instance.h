// A Generator with its ResolvedGenre beside it (spec §3.6), as the firmware's owners hold them, so a harness instance
// plays a Recipe's variant picks. A bare Generator plays every genre as written.

#pragma once

#include "generative/core/generator.h"
#include "generative/core/variants.h"

namespace harness {

struct Instance : deluge::generative::Generator {
	Instance() { useResolvedGenre(&resolved); }
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	deluge::generative::ResolvedGenre resolved;
};

} // namespace harness
