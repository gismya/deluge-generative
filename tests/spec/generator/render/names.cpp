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

const char* const kSectionNames[kNumSectionKinds] = {"INTRO", "GROOVE", "BREAK", "BUILD", "OUTRO"};

std::string sectionLabel(const Event& section) {
	return std::string(kSectionNames[static_cast<int32_t>(section.section)]) + " "
	       + std::to_string(section.occurrence + 1);
}

} // namespace harness::render
