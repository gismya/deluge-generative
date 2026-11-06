// The rough render's command: genre + Recipe overrides + seed + length in, a WAV, a MIDI file and a piano roll out
// (spec §21.5 step 1). The arguments match the delugemu capture script's, so both play the same Recipe.

#pragma once

#include "render/render.h"

#include "generative/core/recipe_overrides.h"

#include <string>
#include <vector>

namespace harness::render {

struct Request {
	RecipeOverrides overrides;
	int32_t bars = 16;
	SoundLibrary library;
	/// Path prefix: writes <out>.wav, <out>.mid and <out>.txt.
	std::string out;
};

struct Parsed {
	Request request;
	std::string error; ///< Empty when the arguments parse.
	bool help = false;
};

extern const char* const kUsage;

/// Parses the command line. Sample roots default to the repo's pack/ and then ~/source/delugemu-sd/sdcard.
Parsed parseArgs(int argc, const char* const* argv);

/// Plays the Recipe through the harness for the request's bars and writes the three files.
/// @return The render's warnings.
std::vector<std::string> render(const Request& request);

} // namespace harness::render
