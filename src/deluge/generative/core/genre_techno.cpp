/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// Generative mode POC: Techno's tables. Our own values (ADR 0001), first-pass guesses to be tuned by ear in
// delugemu. Brief (#11): four-on-the-floor kick, claps on 2 and 4, offbeat hats filling towards 16ths, a mutating
// acid line, a kick drop at phrase ends.
//
// Step columns below are the 16ths of a bar: 1 e & a 2 e & a 3 e & a 4 e & a.

#include "generative/core/genre.h"

namespace deluge::generative {

namespace {
constexpr uint8_t A = kImportanceAnchor;
} // namespace

const GenreTables kGenreTechno =
    {
        .name = "TECHNO",
        .phraseBars = 8,
        // Density 0 lets only anchors through; Density 50 lets every non-zero step through, jitter included.
        .thresholdAtDensityMin = 100,
        .thresholdAtDensityMax = -12,
        .importanceJitter = 8,
        .drumVariationAtMaxMotion = 60,
        .lanes =
            {
                // Kick: four on the floor, always. Drops for the last 1-2 beats of each phrase.
                {
                    .importance = {A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0, A, 0, 0, 0},
                    .accent = {24, 0, 0, 0, 8, 0, 0, 0, 16, 0, 0, 0, 8, 0, 0, 0},
                    .densityGate = 0,
                    .velocityLow = 96,
                    .velocityHigh = 118,
                    .accentLow = 4,
                    .accentHigh = 9,
                    .velocityJitter = 2,
                    .leanOnBeat = 0,
                    .leanOffBeat = 0,
                    .gateTicks = 6,
                    .phraseEndDropBeatsMin = 1,
                    .phraseEndDropBeatsMax = 2,
                },
                // Clap: 2 and 4 from low Density, a soft ghost on the last 16th near the top.
                {
                    .importance = {0, 0, 0, 0, 90, 0, 0, 0, 0, 0, 0, 0, 90, 0, 0, 16},
                    .accent = {0, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0},
                    .densityGate = 0,
                    .velocityLow = 58,
                    .velocityHigh = 80,
                    .accentLow = 26,
                    .accentHigh = 38,
                    .velocityJitter = 3,
                    .leanOnBeat = 0,
                    .leanOffBeat = 0,
                    .gateTicks = 6,
                    .phraseEndDropBeatsMin = 0,
                    .phraseEndDropBeatsMax = 0,
                },
                // Closed hat: offbeat 8ths first, then 16ths by importance. Off-beat 16ths push 2 ticks ahead.
                {
                    .importance = {36, 16, 80, 46, 28, 20, 80, 52, 34, 14, 80, 48, 26, 24, 80, 58},
                    .accent = {8, 16, 64, 16, 8, 16, 64, 16, 8, 16, 64, 16, 8, 16, 64, 16},
                    .densityGate = 0,
                    .velocityLow = 48,
                    .velocityHigh = 68,
                    .accentLow = 18,
                    .accentHigh = 32,
                    .velocityJitter = 5,
                    .leanOnBeat = 0,
                    .leanOffBeat = -2,
                    .gateTicks = 4,
                    .phraseEndDropBeatsMin = 0,
                    .phraseEndDropBeatsMax = 0,
                },
                // Open hat: offbeat 8ths, let in only at higher Density.
                {
                    .importance = {0, 0, 36, 0, 0, 0, 44, 0, 0, 0, 40, 0, 0, 0, 50, 0},
                    .accent = {0, 0, 32, 0, 0, 0, 32, 0, 0, 0, 32, 0, 0, 0, 40, 0},
                    .densityGate = 28,
                    .velocityLow = 54,
                    .velocityHigh = 78,
                    .accentLow = 8,
                    .accentHigh = 16,
                    .velocityJitter = 4,
                    .leanOnBeat = 0,
                    .leanOffBeat = 0,
                    .gateTicks = 10,
                    .phraseEndDropBeatsMin = 0,
                    .phraseEndDropBeatsMax = 0,
                },
            },
        // Bass: an acid Loop. Strongest on the offbeat 8ths, weakest under the kick, rolling 16ths at high Density.
        .bass =
            {
                .importance = {60, 30, 95, 50, 30, 26, 85, 56, 42, 32, 90, 50, 28, 24, 80, 58},
                .restChance = 12,
                .accentChance = 28,
                .tieChance = 8,
                .slideChance = 14,
                // Mostly tonic, then the 5th, the flat 7th below, the 3rd, the octave and a little 2nd (b2 in
                // Phrygian).
                .degrees =
                    {
                        {.degree = 0, .octave = 0, .weight = 40},
                        {.degree = 0, .octave = 1, .weight = 10},
                        {.degree = 4, .octave = 0, .weight = 10},
                        {.degree = 4, .octave = -1, .weight = 6},
                        {.degree = 6, .octave = -1, .weight = 8},
                        {.degree = 6, .octave = 0, .weight = 4},
                        {.degree = 2, .octave = 0, .weight = 8},
                        {.degree = 1, .octave = 0, .weight = 4},
                    },
                .numDegrees = 8,
                .tonicOnFirstStep = true,
                .lengths =
                    {
                        {.ticks = 6, .weight = 3},
                        {.ticks = 10, .weight = 4},
                        {.ticks = 14, .weight = 3},
                        {.ticks = 20, .weight = 1},
                    },
                .numLengths = 4,
                .lengthEnergyBonus = 4,
                .slideOverlapTicks = 3,
                .minGapTicks = 2,
                .velocityLow = 72,
                .velocityHigh = 100,
                .accentLow = 12,
                .accentHigh = 24,
                .velocityJitter = 3,
                .leanOnBeat = 0,
                .leanOffBeat = 0,
                .registerAnchor = 36,
                .spanLow = -6,
                .spanHigh = 15,
                .mutateStepsAtMaxMotion = 6,
            },
};

} // namespace deluge::generative
