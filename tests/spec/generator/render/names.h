// Names and counts the rough render's writers and its command line share, so the MIDI file, the piano roll, the WAV
// warnings and the arguments always agree.

#pragma once

#include "generative/core/event.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
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
/// Sound controls, in SoundControl order ("Brightness").
extern const char* const kControlNames[deluge::generative::kNumSoundControls];
/// Macros in lowercase, in Macro order ("energy"): the golden's header and the command's flags ("--energy").
extern const char* const kMacroNames[deluge::generative::kNumMacros];

/// A Role's, Flavour's or interplay RuleKind's name as the code spells it ("Drums", "Sustain", "Lock").
const char* roleName(deluge::generative::Role role);
const char* flavourName(deluge::generative::Flavour flavour);
const char* ruleKindName(deluge::generative::RuleKind kind);

/// A MIDI note's name, C4 = 60, with sharps: "C4", "F#1", "C-1".
std::string noteName(uint8_t pitch);

/// A Section event's label, its kind as sectionName() gives it, counting occurrences from 1: "GROOVE 2".
std::string sectionLabel(const deluge::generative::Event& section);

} // namespace harness::render
