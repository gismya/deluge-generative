// A harsher Motif than Detroit techno's (#220, spec §4.5): Lean on the a, ratchets and flams, which push the note bound
// to its edges. motif_spec plays it over whole Arcs, and genre_data_spec holds it to motifNotesValid() beside the
// built-in genres.

#pragma once

#include "harness.h"

namespace harness {

/// The Motif part's index in kTechnoMotif.
inline constexpr uint8_t kMotifPart = 2;

/// Techno's Drums, Bass and FX, and a tiered Motif lead over Melodic techno's Chord graph with up to two Extensions:
/// one- and two-bar riffs, with Lean on the a, ratchets and flams that Detroit techno's lead doesn't have.
inline constexpr Pattern kRiffs[] = {
    {"X..5..X...7.X...", "................", 0},
    {"X..5..X..3..X.7.X..5..X...7..9..", "................................", 1},
    {"X.5.7.X..5.3X.7.X..5..X.7.5..9.3", "................................", 2},
};
inline constexpr LineWeight kRiffPositions[] = {{-1, 1}, {0, 3}, {1, 2}, {2, 2}, {3, 2}, {4, 1}};
inline constexpr Expression kRiffExpression{
    .lean = {0, 4, 0, 10},
    .spread = 6,
    .ornaments = {.rateFloor = 100, .rateCeiling = 400, .ratchet = {1, 0, 0}, .flam = 1, .flamGap = 4}};
inline const PartSpec kTechnoMotifParts[] = {
    genres::technoAcid.parts[0],
    genres::technoAcid.parts[1],
    {.role = Role::Lead,
     .flavour = Flavour::Motif,
     .name = "MOTIF",
     .pool = kRiffs,
     .band = {0, 2},
     .motif = {.positions = kRiffPositions, .draws = {.leap = {2, 4}, .octaveUp = {0, 200}}, .gateTicks = 72},
     .expression = kRiffExpression},
    genres::technoAcid.parts.back(), // FX
};
inline const Genre kTechnoMotif = [] {
	Genre genre = genres::technoMelodic;
	genre.name = "techno-motif";
	genre.parts = kTechnoMotifParts;
	genre.harmony.extensionCeiling = 2;
	// Its own Break keeps, not Melodic's part bits: the Motif over the closed hat.
	genre.arc.breakKeeps = {.parts = partBit(kMotifPart), .lanes = laneBit(Lane::ClosedHat)};
	return genre;
}();

} // namespace harness
