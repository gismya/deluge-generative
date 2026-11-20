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
	/// The most bars it plays. With endAfterDrop the piece may end sooner.
	int32_t bars = 16;
	/// Arms End on the Drop's first step (the first Groove after a Build), so the Drop plays to the next phrase line,
	/// the Outro follows and the piece ends by itself: one Arc cycle, for the theory review (spec §21.5).
	bool endAfterDrop = false;
	/// Writes only <out>.mid: no WAV or piano roll, and no sound picks.
	bool midiOnly = false;
	/// --mute: bit p leaves part p (sidebar row p + 1, the genre's part order) out of the WAV from step 0, as a Perform
	/// mute does. The Generator plays as without it, and the MIDI file and piano roll keep every part.
	uint32_t mutedParts = 0;
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
	/// --list-risers: print riserLanes() instead of rendering.
	bool listRisers = false;
};

extern const char* const kUsage;

/// Parses the command line. Sample roots default to the repo's pack/ and then ~/source/delugemu-sd/sdcard; --out
/// defaults to the repo's build/renders/<genre>-seed<seed>.
Parsed parseArgs(int argc, const char* const* argv);

struct Rendered {
	std::vector<std::string> warnings;
	std::string error; ///< Empty when all the files were written.
};

/// One line per preset each built-in genre's pitched parts (Bass, Lead, Chords, Pad) can pick from their palette,
/// "genre<TAB>PART<TAB>path\n" in genre and roster order: what osc_octaves.py checks against the card (spec §10.6).
std::string pitchedPalettePresets();
/// Each built-in genre's Riser lane: its name, its shortest Build in ms at its top tempo, and the lane's folders and
/// tags, each |-separated, a line each (spec §10.5's Riser fit), for scripts/generative/riser_fit.py.
std::string riserLanes();

/// Plays the Recipe through the harness for the request's bars, or until the piece ends, with each decided step's
/// chord and macros. Its `bars` is the bars it played. No sound picks.
/// `endArmed`: whether endAfterDrop found its Drop and armed End.
Rendering playRequest(const Request& request, uint32_t* droppedEvents = nullptr, bool* endArmed = nullptr);

/// Plays the Recipe (playRequest()) and writes the three files, or only the MIDI file with midiOnly.
Rendered render(const Request& request);

} // namespace harness::render
