// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Per-architecture sizing constants (tile counts, channels, memory map) keyed by TT_ARCH_VERSION.
// https://github.com/tenstorrent/tt-metal/blob/main/tt_metal/hw/inc/wormhole/dev_mem_map.h
// https://github.com/tenstorrent/tt-metal/blob/main/tt_metal/hw/inc/wormhole/tensix.h
// https://github.com/tenstorrent/tt-metal/blob/main/tt_metal/hw/inc/wormhole/noc/noc_parameters.h
// https://github.com/tenstorrent/tt-metal/blob/main/tt_metal/hw/inc/blackhole/dev_mem_map.h
// https://github.com/tenstorrent/tt-metal/blob/main/tt_metal/hw/inc/blackhole/tensix.h
// https://github.com/tenstorrent/tt-metal/blob/main/tt_metal/hw/inc/blackhole/noc/noc_parameters.h
#pragma once

#if TT_ARCH_VERSION == 0
#define NUM_DRAM_CHANNELS 6 // technically it's 12 channels, 2 for each of the 6 DRAM tiles, but doesn't matter
#define NUM_E_TILES 16
#define NUM_T_TILES 80

#define RV32_ID_BRISC 0
#define RV32_ID_TRISC0 1
#define RV32_ID_TRISC1 2
#define RV32_ID_TRISC2 3
#define RV32_ID_NCRISC 4
#define RV32_CORES_PER_T_TILE 5
#define RV64_CORES_PER_T_TILE 0
#define TENSIX_CORES_PER_T_TILE 1

#define RV32_CORES_PER_E_TILE 1

#define TENSIX_SRAM_SIZE (1464 * 1024)
#define BRISC_LOCAL_MEM_SIZE (4 * 1024)
#define NCRISC_LOCAL_MEM_SIZE (4 * 1024)
#define TRISC_LOCAL_MEM_SIZE (2 * 1024)
#define ERISC_LOCAL_MEM_SIZE (4 * 1024)
#define ETH_SRAM_SIZE (256 * 1024)
#define DRAM_CHANNEL_SIZE 0x80000000ull // 2GB for each of the 6 DRAM tiles (12GB total)

#define RV32_IRAM_BASE 0xFFC00000 // for both NCRISC and ERISC
#define RV32_IRAM_SIZE (16 * 1024)

#define NUM_NOCS 2
#define NUM_NOC_CMD_BUFS 4
#define TENSIX_NUM_NOC_OVERLAY_STREAMS 64
#define ETH_NUM_NOC_OVERLAY_STREAMS 32
#define NUM_NOC_TRANSACTION_IDS 16
#define NOC_LINK_WIDTH 32
#define NOC_MAX_PACKET_SIZE 8192

#define ETH_NUM_TX_RX_QUEUES 2
#elif TT_ARCH_VERSION == 1
#define NUM_DRAM_CHANNELS 8
#define NUM_E_TILES 14
#define NUM_T_TILES 140

#define RV32_ID_BRISC 0
#define RV32_ID_TRISC0 1
#define RV32_ID_TRISC1 2
#define RV32_ID_TRISC2 3
#define RV32_ID_NCRISC 4
#define RV32_CORES_PER_T_TILE 5
#define RV64_CORES_PER_T_TILE 0
#define TENSIX_CORES_PER_T_TILE 1

#define RV32_CORES_PER_E_TILE 2

#define TENSIX_SRAM_SIZE (1536 * 1024)
#define BRISC_LOCAL_MEM_SIZE (8 * 1024)
#define NCRISC_LOCAL_MEM_SIZE (8 * 1024)
#define TRISC_LOCAL_MEM_SIZE (4 * 1024)
#define ERISC_LOCAL_MEM_SIZE (8 * 1024)
#define ETH_SRAM_SIZE (512 * 1024)
#define DRAM_CHANNEL_SIZE 0x100000000ull // 4GB for each of the 8 DRAM tiles (32GB total)

#define NUM_NOCS 2
#define NUM_NOC_CMD_BUFS 4
#define TENSIX_NUM_NOC_OVERLAY_STREAMS 64
#define ETH_NUM_NOC_OVERLAY_STREAMS 32
#define NUM_NOC_TRANSACTION_IDS 16
#define NOC_LINK_WIDTH 64
#define NOC_MAX_PACKET_SIZE 16384

#define ETH_NUM_TX_RX_QUEUES 3
#define ETH_NUM_TX_HEADER_TABLE_ENTRIES 10
#else
#error unknown TT_ARCH_VERSION
#endif
