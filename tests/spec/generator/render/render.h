// The rough render (spec §21.2, §21.5 step 1): an offline, deterministic render of a Recipe through the harness to a
// WAV, a Standard MIDI File with one track per part, and a text piano roll. The voices are our own small stand-ins,
// not the firmware's DSP. Renders are made locally and never committed.

#pragma once

#include "generative/core/event.h"
#include "generative/core/recipe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace harness::render {

using namespace deluge::generative;

/// A played stretch of a Recipe: the events the harness recorded for `bars` bars.
struct Rendering {
	Recipe recipe;
	uint32_t seed = 1;
	int32_t bars = 0;
	std::vector<Event> events;
};

/// Format 1 at 96 PPQN (24 Generator ticks per 16th): a conductor track with the tempo, 4/4 and a marker per Section,
/// then one track per part in roster order. Drums and FX parts play on channel 10 with their lanes on GM drum notes;
/// pitched parts play on their own channel.
std::vector<uint8_t> midiFile(const Rendering& rendering);

/// A text piano roll, one character per 16th, in blocks of 4 bars: a row per lane of each Drums or FX part and a row
/// per pitch each pitched part sounds, highest first. A hit is 'X' at velocity 100 and up, 'x' from 64, else 'o'; a
/// pitched note held past its 16th shows '=' until its note-off.
std::string pianoRoll(const Rendering& rendering);

/// Where a Recipe's sample paths resolve: each root in order, so pack/ goes before a card-shaped folder.
struct SoundLibrary {
	std::vector<std::string> roots;
};

struct Audio {
	std::vector<uint8_t> wav;
	/// What the render couldn't play as asked: a sample it couldn't find or read (a stand-in hit plays instead), and
	/// sound parameters it doesn't model (ignored). One line each.
	std::vector<std::string> warnings;
};

constexpr int32_t kSampleRate = 44100;

/// 16-bit stereo at 44.1 kHz: the bars, then a second of tail. Kit rows play their sample once per hit, each hit
/// cutting the row's last; pitched parts play a stand-in subtractive voice per note. Nothing here is random or
/// threaded, so the same input gives the same bytes.
Audio renderAudio(const Rendering& rendering, const SoundLibrary& library);

// Names the tools share with their command lines.

/// Lowercase scale names, in Scale order ("minor", "harmonic-minor").
extern const char* const kScaleNames[kNumScales];
/// Short lane names, in Lane order ("Kick", "HatC").
extern const char* const kLaneNames[kNumLanes];
/// Pitch-class names with sharps, 0 = C.
extern const char* const kPitchNames[12];

} // namespace harness::render
