// Names and counts the rough render's writers and its command line share, so the MIDI file, the piano roll, the WAV
// warnings and the arguments always agree.

#pragma once

#include "generative/core/event.h"
#include "generative/core/genre.h"
#include "generative/core/types.h"

#include <cstdint>
#include <string>

namespace harness::render {

/// Lowercase scale names, in Scale order ("minor", "harmonic-minor").
extern const char* const kScaleNames[deluge::generative::kNumScales];
/// Short lane names, in Lane order ("Kick", "HatC").
extern const char* const kLaneNames[deluge::generative::kNumLanes];
/// Pitch-class names with sharps, 0 = C.
extern const char* const kPitchNames[12];
/// Section kinds in capitals, in SectionKind order ("INTRO").
extern const char* const kSectionNames[deluge::generative::kNumSectionKinds];

/// A Section event's label, counting occurrences from 1: "GROOVE 2".
std::string sectionLabel(const deluge::generative::Event& section);

/// The parts the render plays: the genre's roster, at most kMaxParts.
int32_t partCount(const deluge::generative::Genre& genre);

} // namespace harness::render
