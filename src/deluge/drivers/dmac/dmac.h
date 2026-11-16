/*
 * Copyright © 2019-2023 Synthstrom Audible Limited
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

#pragma once

#include "RZA1/system/iobitmasks/dmac_iobitmask.h"
#include "stdint.h"

#define DMA_INTERRUPT_0 INTC_ID_DMAINT0

#define NUM_DMA_CHANNELS 16

#ifdef __cplusplus
extern "C" {
#endif

void setDMARS(int32_t dmaChannel, uint32_t dmarsValue);
void initDMAWithLinkDescriptor(int32_t dma_channel, const uint32_t* linkDescriptor, uint32_t dmarsValue);
void dmaChannelStart(const uint32_t dma_channel);

/// Stops every DMA channel and clears its status. The audio and UART channels run in link-descriptor mode and re-read
/// their descriptor from RAM at the end of every block, forever. Before anything overwrites the memory they use
/// (chainloading a new firmware over this one) or reuses it (a firmware starting after a chainload), they must stop:
/// otherwise they read whatever now sits at the old descriptor's address as a descriptor and write to wherever it
/// points (#166).
void dmaStopAllChannels(void);

#if ENABLE_SYSEX_TEST
/// One channel's state as the firmware found it at boot, before dmaStopAllChannels(). Test builds only, for the
/// test port's "report boot_dma".
struct DmaBootState {
	uint32_t chstat; ///< CHSTAT_n: bit 0 EN (running), bit 2 TACT, bit 4 ER, bit 10 DER
	uint32_t crla;   ///< CRLA_n: the link descriptor it last loaded
	uint32_t crsa;   ///< CRSA_n: current source address
	uint32_t crda;   ///< CRDA_n: current destination address
};
extern struct DmaBootState dmaBootStates[NUM_DMA_CHANNELS];
void dmaRecordBootStates(void);
#endif

#ifdef __cplusplus
}
#endif
