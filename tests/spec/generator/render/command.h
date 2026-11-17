// The rough render's command: genre + Recipe overrides + seed + length in, a WAV, a MIDI file and a piano roll out
// (spec §21.5 step 1). The arguments match the delugemu capture script's, so both play the same Recipe.

#pragma once

#include "render/render.h"

#include "generative/core/recipe_overrides.h"

#include <string>
#include <vector>

namespace harness::render {

struct Request {
	deluge::generative::RecipeOverrides overrides;
	int32_t bars = 16;
	SoundLibrary library;
	/// Path prefix: writes <out>.wav, <out>.mid and <out>.txt, making its folder if needed.
	std::string out;
};

struct Parsed {
	Request request;
	std::string error; ///< Empty when the arguments parse.
	bool help = false;
	/// --list-palettes: print pitchedPalettePresets() instead of rendering.
	bool listPalettes = false;
};

extern const char* const kUsage;

/// Parses the command line. Sample roots default to the repo's pack/ and then ~/source/delugemu-sd/sdcard; --out
/// defaults to the repo's build/renders/<genre>-seed<seed>.
Parsed parseArgs(int argc, const char* const* argv);

struct Rendered {
	std::vector<std::string> warnings;
	std::string error; ///< Empty when all three files were written.
};

/// One line per preset each built-in genre's pitched parts (Bass, Lead, Chords, Pad) can pick from their palette,
/// "genre<TAB>PART<TAB>path\n" in genre and roster order: what osc_octaves.py checks against the card (spec §10.6).
std::string pitchedPalettePresets();

/// Plays the Recipe through the harness for the request's bars and writes the three files.
Rendered render(const Request& request);

} // namespace harness::render
