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

// SPIKE, measurement only, not for merge: would streaming the steering log to the card during playback disturb the
// Deluge? While Perform plays, a main-loop task appends synthetic log bytes (5 B per decided step, a 7 KB header at
// each piece's opening) to /GENTEST/CLOGn.BIN in chunks, times every card call, and reports it as test events.

#pragma once

#if ENABLE_SYSEX_TEST

#include <cstdint>

namespace deluge::generative::firmware::card_log_spike {

/// Test-port holds: the chunk size (0: off; the files are closed and deleted), an f_sync after every chunk (else only
/// at the piece's end), and busy mode (64 B per step instead of 5, to sample many writes).
void setChunk(uint32_t bytes);
void setSyncEach(bool on);
/// #448 condition 5: a simulated card stall of `ms` every 2 s (0: off); steady 4 KB writes + f_sync every 500 ms.
void setStall(uint32_t ms);
void setSteady(bool on);
void setBusy(bool on);

/// From the tick: steps decided, a piece opening. Counters only, no card access.
void stepsDecided(int32_t steps);
void pieceOpening();

/// Every 4 bars, with gen_h16: gen_cardlog (this window's writes) and gen_stream (renders, streaming misses).
void report();

} // namespace deluge::generative::firmware::card_log_spike

#endif
