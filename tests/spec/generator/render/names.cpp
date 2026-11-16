// Rough render: shared names.

#include "render/names.h"

#include <algorithm>

namespace harness::render {

using namespace deluge::generative;

const char* const kScaleNames[kNumScales] = {
    "major",   "minor",         "dorian",         "phrygian",        "lydian", "mixolydian",
    "locrian", "melodic-minor", "harmonic-minor", "hungarian-minor", "marva",  "arabian",
};

const char* const kLaneNames[kNumLanes] = {"Kick", "Snare", "Clap",  "Rim",   "HatC", "HatO",   "Ride", "Crash",
                                           "Tom",  "Perc1", "Perc2", "Riser", "Fall", "Impact", "Siren"};

const char* const kPitchNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

const char* const kControlNames[kNumSoundControls] = {"Brightness", "Decay", "Drive", "Space", "Wobble", "Thin"};

const char* const kMacroNames[kNumMacros] = {"energy", "density", "complexity", "motion"};

std::string noteName(uint8_t pitch) {
	return kPitchNames[pitch % 12] + std::to_string(pitch / 12 - 1);
}

std::string sectionLabel(const Event& section) {
	return std::string(sectionName(section.section)) + " " + std::to_string(section.occurrence + 1);
}

} // namespace harness::render
