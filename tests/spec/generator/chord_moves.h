// What the Chord-graph walk's specs share: a graph's moves shaped like Melodic techno's (spec §4.5), with weights
// picked for the arithmetic.

#pragma once

#include "generative/core/chord_walk.h"

namespace chord_specs {

using deluge::generative::ChordMove;

constexpr ChordMove kMelodicMoves[] = {
    {0, 5, 30}, // i–♭VI
    {5, 6, 30}, // ♭VI–♭VII
    {6, 0, 30}, // ♭VII–i
    {0, 3, 20}, // i–iv
    {3, 5, 20}, // iv–♭VI
    {0, 6, 20}, // i–♭VII
};

} // namespace chord_specs
