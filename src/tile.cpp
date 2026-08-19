// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Per-tile infrastructure: tile init, NOC routing, TDMA, Ethernet, DRAM, debug/config registers.
#include "sim.h"
#include "topology.h"
#if TT_ARCH_VERSION == 0
#include "eth_fw_blob.h" // generated
#endif

#if TT_ARCH_VERSION == 0
static constexpr uint64_t PCIE_HOST_WINDOW_BASE = 0x800000000ull;
#endif

static uint64_t translate_pci_dma_addr(uint64_t addr, uint32_t size);
static std::pair<char, uint32_t> coord_to_tile(uint32_t coord);
static uint32_t tile_to_coord(char tile_type, uint32_t tile_id);

template<char tile_type>
static inline auto get_tile(uint32_t tile_id) {
    static_assert((tile_type == 'T') || (tile_type == 'E'));
    if constexpr (tile_type == 'T') {
        return &g_t_tiles[tile_id];
    } else {
        return &g_e_tiles[tile_id];
    }
}

static constexpr uint32_t logical_harvesting_mask(uint32_t chip_id) {
    TTSIM_ASSERT(chip_id < NUM_CHIPS);
#if TT_ARCH_VERSION == 0
#if NUM_CHIPS == 1
    // proper harvest mask breaks metal tests for n150
    // constexpr uint32_t LOGICAL_HARVESTING_MASKS[] = {0x40}; // N150
    constexpr uint32_t LOGICAL_HARVESTING_MASKS[] = {0x0}; // N150
#elif NUM_CHIPS == 2
    constexpr uint32_t LOGICAL_HARVESTING_MASKS[] = {0x41, 0x05}; // N300
#elif NUM_CHIPS == 8
    constexpr uint32_t LOGICAL_HARVESTING_MASKS[] = {
        0x220, 0x201, 0x280, 0x201, 0x240, 0x201, 0x201, 0x210};
#elif NUM_CHIPS == 32
    constexpr uint32_t LOGICAL_HARVESTING_MASKS[NUM_CHIPS] = {}; // WH Galaxy
#else
#error unsupported Wormhole topology
#endif
    return LOGICAL_HARVESTING_MASKS[chip_id];
#else
    return 0;
#endif
}

#if TT_ARCH_VERSION == 0
static constexpr uint8_t TENSIX_COLUMNS[] = {1, 2, 3, 4, 6, 7, 8, 9};
static constexpr uint8_t TENSIX_ROWS[] = {1, 2, 3, 4, 5, 7, 8, 9, 10, 11};
#endif

uint32_t tensix_harvested_row_mask() {
#if TT_ARCH_VERSION == 0
    // Harvesting mask bits index TENSIX_ROWS, but router_cfg_3 is indexed by physical row.
    uint32_t logical_mask = logical_harvesting_mask(g_current_chip_id);
    uint32_t physical_mask = 0;
    for (uint32_t row = 0; row < std::size(TENSIX_ROWS); row++) {
        if (logical_mask & (1u << row)) {
            physical_mask |= 1u << TENSIX_ROWS[row];
        }
    }
    return physical_mask;
#else
    return 0;
#endif
}

#if TT_ARCH_VERSION == 0
static uint32_t noc_translation_table_get(const uint32_t table[4], uint32_t index) {
    TTSIM_ASSERT(index < 32);
    return (table[index / 8] >> (4 * (index & 7))) & 0xF;
}

static void noc_translation_table_set(uint32_t table[4], uint32_t index, uint32_t value) {
    TTSIM_ASSERT(index < 32);
    TTSIM_ASSERT(value < 16);
    uint32_t shift = 4 * (index & 7);
    table[index / 8] = (table[index / 8] & ~(0xFu << shift)) | (value << shift);
}

static uint32_t noc_translation_table_find(
    const uint32_t table[4], uint32_t physical_coord, uint32_t first, uint32_t last) {
    for (uint32_t logical_coord = first; logical_coord <= last; logical_coord++) {
        if (noc_translation_table_get(table, logical_coord) == physical_coord) {
            return logical_coord;
        }
    }
    TTSIM_ERROR(AssertionFailure, "physical_coord=%d first=%d last=%d", physical_coord, first, last);
}
#endif

template<char tile_type>
void base_tile_init(BaseTile *p_tile, uint32_t tile_id) {
    static_assert((tile_type == 'T') || (tile_type == 'E') || (tile_type == 'P') || (tile_type == 'A'));
#if TT_ARCH_VERSION == 0
    uint32_t physical_coord = tile_to_coord(tile_type, tile_id);
    uint32_t logical_mask = logical_harvesting_mask(g_current_chip_id);
#endif
    for (uint32_t noc = 0; noc < NUM_NOCS; noc++) {
        p_tile->router_cfg_1[noc] = NONTENSIX_COL_MASK;
        p_tile->router_cfg_3[noc] = NONTENSIX_ROW_MASK | tensix_harvested_row_mask();
        p_tile->niu_cfg_0[noc] = NOC_REGS_NIU_CFG_0_RESET_VALUE | (1u << 14);
#if TT_ARCH_VERSION == 0
        // noc translation tables
        for (uint32_t coord = 0; coord < 16; coord++) {
            noc_translation_table_set(p_tile->noc_translation_x[noc], coord, coord);
            noc_translation_table_set(p_tile->noc_translation_y[noc], coord, coord);
        }
        noc_translation_table_set(p_tile->noc_translation_x[noc], 16, noc ? 9 : 0);
        noc_translation_table_set(p_tile->noc_translation_x[noc], 17, noc ? 4 : 5);
        noc_translation_table_set(p_tile->noc_translation_y[noc], 16, noc ? 11 : 0);
        noc_translation_table_set(p_tile->noc_translation_y[noc], 17, noc ? 5 : 6);
        for (uint32_t column = 0; column < std::size(TENSIX_COLUMNS); column++) {
            uint32_t physical_column = TENSIX_COLUMNS[column];
            noc_translation_table_set(p_tile->noc_translation_x[noc], 18 + column, noc ? (9 - physical_column) : physical_column);
        }
        uint32_t translated_row = 0;
        // Firmware assigns logical rows to active physical rows first, then appends harvested rows.
        for (uint32_t harvested = 0; harvested <= 1; harvested++) {
            for (uint32_t physical_row = 0; physical_row < std::size(TENSIX_ROWS); physical_row++) {
                if (((logical_mask >> physical_row) & 1) == harvested) {
                    uint32_t physical_y = TENSIX_ROWS[physical_row];
                    noc_translation_table_set(
                        p_tile->noc_translation_y[noc], 18 + translated_row, noc ? (11 - physical_y) : physical_y);
                    translated_row++;
                }
            }
        }
        // noc logical id
        uint32_t physical_x = physical_coord & 63;
        uint32_t physical_y = physical_coord >> 6;
        if (noc) {
            physical_x = 9 - physical_x;
            physical_y = 11 - physical_y;
        }
        uint32_t logical_x;
        uint32_t logical_y;
        if constexpr (tile_type == 'T') {
            logical_x = noc_translation_table_find(p_tile->noc_translation_x[noc], physical_x, 18, 25);
            logical_y = noc_translation_table_find(p_tile->noc_translation_y[noc], physical_y, 18, 27);
        } else if constexpr (tile_type == 'E') {
            logical_x = noc_translation_table_find(p_tile->noc_translation_x[noc], physical_x, 18, 25);
            logical_y = noc_translation_table_find(p_tile->noc_translation_y[noc], physical_y, 16, 17);
        } else {
            static_assert((tile_type == 'P') || (tile_type == 'A'));
            logical_x = noc_translation_table_find(p_tile->noc_translation_x[noc], physical_x, 16, 17);
            logical_y = physical_y;
        }
        p_tile->noc_id_logical[noc] = logical_x | (logical_y << 6);
#endif
    }
}

void t_tile_init(uint32_t tile_id) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
    base_tile_init<'T'>(&p_tile->base, tile_id);
    for (uint32_t rv32_id = 0; rv32_id < std::size(p_tile->rv32); rv32_id++) {
        rv32_init(&p_tile->rv32[rv32_id], 'T', tile_id, rv32_id);
    }
#if RV64_CORES_PER_T_TILE
    for (uint32_t rv64_id = 0; rv64_id < std::size(p_tile->rv64); rv64_id++) {
        rv64_init(&p_tile->rv64[rv64_id], 'T', tile_id, rv64_id);
    }
#endif
    for (uint32_t tensix_id = 0; tensix_id < std::size(p_tile->tensix); tensix_id++) {
        tensix_init(&p_tile->tensix[tensix_id], tile_id);
    }
    p_tile->soft_reset_0 = RISCV_DEBUG_REGS_SOFT_RESET_0_RESET_VALUE;
}

// Look up the inter-chip Ethernet peer of (current chip, tile_id). Returns false when
// the tile has no peer (single-chip builds have no table; an unconnected tile misses).
static bool eth_peer(uint32_t tile_id, uint32_t *p_chip_id, uint32_t *p_tile_id) {
#if NUM_CHIPS > 1
    for (const EthLink &link : ETH_PEER_TABLE) {
        if ((link.src_chip == g_current_chip_id) && (link.src_tile == tile_id)) {
            *p_chip_id = link.dst_chip;
            *p_tile_id = link.dst_tile;
            return true;
        }
    }
#endif
    return false;
}

#if TT_ARCH_VERSION == 0
static void wh_x2_eth_link_init(uint32_t tile_id);
#elif TT_ARCH_VERSION == 1
static void bh_eth_link_init(uint32_t tile_id);
#endif

void e_tile_init(uint32_t tile_id) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    base_tile_init<'E'>(&p_tile->base, tile_id);
    for (uint32_t rv32_id = 0; rv32_id < std::size(p_tile->rv32); rv32_id++) {
        rv32_init(&p_tile->rv32[rv32_id], 'E', tile_id, rv32_id);
    }
    p_tile->soft_reset_0 = TT_ARCH_VERSION ? 0x1800 : 0x800;
    constexpr uint32_t riscv_ret_inst = 0x8067;
    p_tile->ierisc_reset_pc = 0;
#if TT_ARCH_VERSION == 0
    mem_wr<uint32_t>(&p_tile->sram[0x210], (6 << 16) | (14 << 12)); // ETH_FW_VERSION_ADDR = 6.14.0
    mem_wr<uint32_t>(&p_tile->sram[0x1104], 3); // ETH_TRAIN_STATUS_ADDR = NOT_CONNECTED
    constexpr uint32_t jmp_addr = 0x440; // This is the address jumped to by the context switch
    mem_wr<uint32_t>(&p_tile->sram[0x9020], jmp_addr); // This is the jump table
    mem_wr<uint32_t>(&p_tile->sram[jmp_addr], riscv_ret_inst); // Make this function a no-op
    p_tile->erisc_mac_0 = ETH_MAC_REGS_ERISC_MAC_0_RESET_VALUE;
    p_tile->erisc_mac_1 = ETH_MAC_REGS_ERISC_MAC_1_RESET_VALUE;
    for (uint32_t i = 0; i < ETH_NUM_TX_RX_QUEUES; i++) {
        p_tile->eth_txq_control[i] = ETH_TXQ_CONTROL_RESET_VALUE;
        p_tile->eth_txq_dest_mac_addr_hi[i] = 0;
    }
    p_tile->eth_txq_dest_mac_addr_lo[0] = 0xAB;
    p_tile->eth_txq_dest_mac_addr_lo[1] = 0xAA;
    wh_x2_eth_link_init(tile_id);
#elif TT_ARCH_VERSION == 1
    uint32_t jmp_addr = 0x71574;
    mem_wr<uint32_t>(&p_tile->sram[0x7CF00], jmp_addr); // send_eth_msg function address
    mem_wr<uint32_t>(&p_tile->sram[jmp_addr], riscv_ret_inst); // Make this function a no-op
    jmp_addr = 0x759AC;
    mem_wr<uint32_t>(&p_tile->sram[0x7CF04], jmp_addr); // service_eth_msg function address
    mem_wr<uint32_t>(&p_tile->sram[jmp_addr], riscv_ret_inst); // Make this function a no-op
    jmp_addr = 0x75414;
    mem_wr<uint32_t>(&p_tile->sram[0x7CF08], jmp_addr); // eth_link_status_check function address
    mem_wr<uint32_t>(&p_tile->sram[jmp_addr], riscv_ret_inst); // Make this function a no-op
    // Queue 2 has zero values for all of the following registers
    p_tile->eth_txq_control[0] = ETH_TXQ_CONTROL_RESET_VALUE;
    p_tile->eth_txq_control[1] = ETH_TXQ_CONTROL_RESET_VALUE;
    p_tile->eth_mac_rx_routing = 0;
    p_tile->eth_mac_rx_addr_routing = ETH_CTRL_REGS_MAC_RX_ADDR_ROUTING_RESET_VALUE;
    // Broadcast MAC address
    p_tile->eth_txpkt_cfg_mac_da_lo[0] = 0xFFFFFFFF;
    p_tile->eth_txpkt_cfg_mac_da_hi[0] = 0xFFFF;
    // Multicast MAC address
    p_tile->eth_txpkt_cfg_mac_da_lo[1] = 1;
    p_tile->eth_txpkt_cfg_mac_da_hi[1] = 0x100;
    p_tile->eth_rxq_control[0] = ETH_RXQ_CONTROL_RESET_VALUE;
    p_tile->eth_rxq_control[1] = ETH_RXQ_CONTROL_RESET_VALUE;
    p_tile->eth_txq_txpkt_cfg_sel_sw[0] = 0;
    p_tile->eth_txq_txpkt_cfg_sel_sw[1] = 0x111;
    p_tile->eth_txq_txpkt_cfg_sel_hw[0] = 0;
    p_tile->eth_txq_txpkt_cfg_sel_hw[1] = 1;
    bh_eth_link_init(tile_id);
#endif
}

void p_tile_init() {
    base_tile_init<'P'>(&g_p_tile.base, 0);
}

#define ARC_TELEMETRY_TABLE_CSM_OFFSET 0x100
#define ARC_TELEMETRY_VALUES_CSM_OFFSET 0x200
#define GDDR_STATUS_TRAINED (0x55555555u >> (32 - 2 * NUM_DRAM_CHANNELS)) // Mark every channel trained.
#if TT_ARCH_VERSION == 0
#define ARC_SMBUS_TELEMETRY_CSM_OFFSET 0x000
#define LEGACY_TELEM_FW_BUNDLE_VERSION 49
#define FLASH_BUNDLE_VERSION 0x12040000 // v18.4
#define ETH_LIVE_STATUS_MASK 0xFFFF     // all 16 eth tiles live
#elif TT_ARCH_VERSION == 1
#define FLASH_BUNDLE_VERSION 0x12050000 // v18.5
#define ETH_LIVE_STATUS_MASK 0x0FFF     // eth tiles 12,13 harvested
#define ARC_MSG_QCB_CSM_OFFSET 0x300
#define ARC_MSG_QUEUE_CSM_OFFSET 0x400
#define ARC_MSG_NUM_QUEUES 4
#define ARC_MSG_QUEUE_NUM_ENTRIES 8
#define ARC_MSG_QUEUE_HEADER_SIZE 32
#define ARC_MSG_ENTRY_SIZE 32
#endif

static uint64_t board_id(uint32_t chip_id) {
#if TT_ARCH_VERSION == 0
#if NUM_CHIPS == 1
    return (uint64_t(0x180) << 32) | 1; // N150
#elif NUM_CHIPS == 32
    return (uint64_t(0x350) << 32) | 1; // WH Galaxy
#else
    constexpr uint64_t N300_BOARD_ID = uint64_t(0x01000146) << 32 | 0x118320AE;
    uint32_t serial_offset = (NUM_CHIPS > NUM_MMIO_CHIPS) ? chip_id % NUM_MMIO_CHIPS : 0;
    return N300_BOARD_ID + serial_offset;
#endif
#else
#if NUM_CHIPS == 1
    return (uint64_t(0x400) << 32) | 1; // P150
#elif NUM_CHIPS == 4
    return ((uint64_t(0x440) << 32) | 1) + (chip_id / 2); // QuietBox 2: two P300 cards
#elif NUM_CHIPS == 32
    return (uint64_t(0x470) << 32) | 1; // BH Galaxy
#else
    return (uint64_t(0x440) << 32) | 1; // P300
#endif
#endif
}

static uint32_t asic_location(uint32_t chip_id) {
#if TT_ARCH_VERSION == 0
    return chip_id;
#else
#if NUM_CHIPS == 4
    return 1 - (chip_id & 1);
#else
    return chip_id;
#endif
#endif
}

#if TT_ARCH_VERSION == 1
static uint32_t pcie_usage(uint32_t chip_id) {
#if NUM_CHIPS <= 4
    return asic_location(chip_id) ? 4 : 1; // the P300's loc-1 ASIC owns the x4 link
#else
    return (chip_id & 1) ? 4 : 1;
#endif
}
#endif

#if TT_ARCH_VERSION == 0
#if NUM_CHIPS > 1
static uint32_t wh_mangled_board_id(uint32_t chip_id) {
    uint64_t id = board_id(chip_id);
    return ((id >> 4) & 0xF0000000) | (id & 0x0FFFFFFF);
}

// Map a remote-queue command's destination mesh coordinate (chip_x, chip_y from sys_addr bits
// 48-53/54-59) to a chip id. Must match the cluster descriptor's chips: [x, y, ...] coords.
// n300 is a single row (id == chip_x); the 4x2 mesh (wh_x8/T3000) needs the 2-D grid.
static uint32_t chip_coord_to_id(uint32_t chip_x, uint32_t chip_y) {
#if NUM_CHIPS == 8
    static constexpr uint8_t GRID[4][2] = {{4, 5}, {0, 1}, {2, 3}, {6, 7}};  // [chip_x][chip_y]
    return (chip_x < 4 && chip_y < 2) ? GRID[chip_x][chip_y] : NUM_CHIPS;
#else
    return chip_y ? NUM_CHIPS : chip_x;
#endif
}

static uint32_t chip_id_to_coord(uint32_t chip_id) {
    for (uint32_t chip_y = 0; chip_y < 64; chip_y++) {
        for (uint32_t chip_x = 0; chip_x < 64; chip_x++) {
            if (chip_coord_to_id(chip_x, chip_y) == chip_id) {
                return chip_x | (chip_y << 6);
            }
        }
    }
    TTSIM_ERROR(AssertionFailure, "chip id has no matching coord %d", chip_id);
}

static constexpr uint32_t ETH_FW_ROUTE_TABLE_ADDR = 0x14D00; // must match eth fw
static constexpr uint32_t ETH_FW_BCAST_TABLE_ADDR = 0x14F00; // must match eth fw
static void eth_fw_write_topology_tables(uint32_t tile_id) {
    uint8_t *sram = &g_e_tiles[tile_id].sram[0];
    uint32_t n = 0;
    // Route forward table: The sibling eth tiles that are directly connected to the remote chip
    for (const EthLink &link : ETH_PEER_TABLE) {
        if (link.src_chip != g_current_chip_id) {
            continue;
        }
        mem_wr<uint32_t>(&sram[ETH_FW_ROUTE_TABLE_ADDR + 4 + 8 * n + 0], chip_id_to_coord(link.dst_chip));
        mem_wr<uint32_t>(&sram[ETH_FW_ROUTE_TABLE_ADDR + 4 + 8 * n + 4], tile_to_coord('E', link.src_tile));
        n++;
    }
    mem_wr<uint32_t>(&sram[ETH_FW_ROUTE_TABLE_ADDR], n);

    // Broadcast forward table: the distinct tunneled-remote peer chips (id >= NUM_MMIO_CHIPS) this chip
    // forwards an eth broadcast to. MMIO-sibling peers are excluded here.
    uint32_t bn = 0;
    for (const EthLink &link : ETH_PEER_TABLE) {
        if (link.src_chip != g_current_chip_id || link.dst_chip < NUM_MMIO_CHIPS) {
            continue;
        }
        uint32_t coord = chip_id_to_coord(link.dst_chip);
        bool dup = false;
        for (uint32_t j = 0; j < bn; j++) {
            if (mem_rd<uint32_t>(&sram[ETH_FW_BCAST_TABLE_ADDR + 4 + 4 * j]) == coord) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            mem_wr<uint32_t>(&sram[ETH_FW_BCAST_TABLE_ADDR + 4 + 4 * bn], coord);
            bn++;
        }
    }
    mem_wr<uint32_t>(&sram[ETH_FW_BCAST_TABLE_ADDR], bn);
}

// Encode `jal x0, target`
static constexpr uint32_t eth_jal_x0(uint32_t target) {
    return 0x6Fu | (((target >> 20) & 1) << 31) | (((target >> 1) & 0x3FF) << 21) |
           (((target >> 11) & 1) << 20) | (((target >> 12) & 0xFF) << 12);
}
#endif

// Fake the eth link-training / boot_results state that UMD topology discovery reads, so
// the n300's two chips present as a trained, connected pair. This is faithful
// initialization data (nothing in the simulator consumes it; the actual inter-chip
// transport is modeled separately), derived from the same ETH_PEER_TABLE used for
// delivery so the advertised topology cannot drift from the routed one.
static void wh_x2_eth_link_init(uint32_t tile_id) {
#if NUM_CHIPS > 1
    constexpr uint32_t eth_fw_base = 0x13000;
    constexpr uint32_t eth_fw_scratch_base = 0x14800;
    EthTile *p_tile = &g_e_tiles[tile_id];
    uint32_t remote_chip_id = 0;
    uint32_t remote_eth_id = 0;
    static_assert(eth_fw_base + sizeof(eth_fw_blob) <= eth_fw_scratch_base);
    memcpy(&p_tile->sram[eth_fw_base], eth_fw_blob, sizeof(eth_fw_blob));
    ttsim_rv32_set_core_active('E', tile_id, 0, true);
    p_tile->soft_reset_0 = 0;
    p_tile->rv32[0].pc = eth_fw_base;
    mem_wr<uint32_t>(&p_tile->sram[0], eth_jal_x0(eth_fw_base)); // seed reset pc with jump to fw
    eth_fw_write_topology_tables(tile_id);
    if (eth_peer(tile_id, &remote_chip_id, &remote_eth_id)) {
        p_tile->ierisc_reset_pc = eth_fw_base;
        mem_wr<uint32_t>(&p_tile->sram[0x1104], 1); // ETH_TRAIN_STATUS_ADDR = LINK_TRAIN_SUCCESS
        mem_wr<uint32_t>(&p_tile->sram[0x104C], NUM_CHIPS == 32); // ROUTING_FIRMWARE_STATE: 1 = disabled on 6U
        mem_wr<uint32_t>(&p_tile->sram[0x1EC0 + 4 * 64], wh_mangled_board_id(g_current_chip_id)); // local board id lo
        mem_wr<uint32_t>(&p_tile->sram[0x1EC0 + 4 * 65], g_current_chip_id); // local ASIC id hi
        mem_wr<uint32_t>(&p_tile->sram[0x1EC0 + 4 * 72], wh_mangled_board_id(remote_chip_id)); // remote board id lo
        mem_wr<uint32_t>(&p_tile->sram[0x1EC0 + 4 * 73], remote_chip_id); // remote ASIC id hi
        mem_wr<uint32_t>(&p_tile->sram[0x1EC0 + 4 * 76], remote_eth_id); // remote eth id
        mem_wr<uint32_t>(&p_tile->sram[0x1EC0 + 4 * 77], wh_mangled_board_id(remote_chip_id)); // remote board type
        uint32_t local_coord = chip_id_to_coord(g_current_chip_id);
        uint32_t remote_coord = chip_id_to_coord(remote_chip_id);
        mem_wr<uint32_t>(&p_tile->sram[0x1100 + 4 * 2],
            ((local_coord & 0x3F) << 16) | (((local_coord >> 6) & 0x3F) << 24)); // NODE_INFO local x[23:16] y[31:24]
        mem_wr<uint32_t>(&p_tile->sram[0x1100 + 4 * 9],
            ((remote_coord & 0x3F) << 16) | (((remote_coord >> 6) & 0x3F) << 22)); // NODE_INFO remote x[21:16] y[27:22]
        mem_wr<uint32_t>(&p_tile->sram[0x1100 + 4 * 10], 0); // NODE_INFO remote rack[7:0] shelf[15:8] (both 0)
    }
#endif
}
#endif

#if TT_ARCH_VERSION == 1
// Fake the eth base-FW boot_results that the active-erisc app (and UMD) read to confirm a
// link state -- the base FW that writes these after link training is faked out. A tile with
// an inter-chip peer advertises a trained, connected link; every other tile reports the link
// down. UMD polls port_status until it leaves PORT_UNKNOWN, so an unpeered tile must still
// report a terminal PORT_DOWN. Offsets per tt-metal blackhole eth_fw_api.h boot_results_t at
// MEM_SYSENG_BOOT_RESULTS_BASE 0x7CC00 (eth_status @ 0, eth_live_status @ 512, eth_fw_ver @ 0x3BC,
// local_info @ 0x3C0, remote_info @ 0x3E0; chip_info fields: asic_location +1, eth_id +2,
// logical_eth_id +3, board_id_hi +4, board_id_lo +8).
static void bh_eth_link_init(uint32_t tile_id) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    uint32_t remote_chip = 0, remote_eth = 0;
    if (eth_peer(tile_id, &remote_chip, &remote_eth)) {
        mem_wr<uint32_t>(&p_tile->sram[0x7CC04], 1); // eth_status.port_status = PORT_UP
        mem_wr<uint32_t>(&p_tile->sram[0x7CC08], 2); // eth_status.train_status = LINK_TRAIN_PASS
        mem_wr<uint32_t>(&p_tile->sram[0x7CE04], 1); // eth_live_status.rx_link_up = 1
        constexpr uint32_t LOCAL = 0x7CFC0, REMOTE = 0x7CFE0;
        uint64_t local_board_id = board_id(g_current_chip_id);
        uint64_t remote_board_id = board_id(remote_chip);
        mem_wr<uint8_t>(&p_tile->sram[LOCAL + 1], uint8_t(asic_location(g_current_chip_id))); // asic_location
        mem_wr<uint8_t>(&p_tile->sram[LOCAL + 2], uint8_t(tile_id)); // eth_id
        mem_wr<uint32_t>(&p_tile->sram[LOCAL + 4], uint32_t(local_board_id >> 32)); // board_id_hi
        mem_wr<uint32_t>(&p_tile->sram[LOCAL + 8], uint32_t(local_board_id)); // board_id_lo
        mem_wr<uint8_t>(&p_tile->sram[REMOTE + 1], uint8_t(asic_location(remote_chip))); // asic_location
        mem_wr<uint32_t>(&p_tile->sram[LOCAL + 20], 0); // asic_id_hi
        mem_wr<uint32_t>(&p_tile->sram[LOCAL + 24], 1 + g_current_chip_id); // asic_id_lo
        mem_wr<uint32_t>(&p_tile->sram[REMOTE + 20], 0); // asic_id_hi
        mem_wr<uint32_t>(&p_tile->sram[REMOTE + 24], 1 + remote_chip); // asic_id_lo
        mem_wr<uint8_t>(&p_tile->sram[REMOTE + 2], uint8_t(remote_eth)); // eth_id (remote channel)
        mem_wr<uint8_t>(&p_tile->sram[REMOTE + 3], uint8_t(remote_eth)); // logical_eth_id
        mem_wr<uint32_t>(&p_tile->sram[REMOTE + 4], uint32_t(remote_board_id >> 32)); // board_id_hi
        mem_wr<uint32_t>(&p_tile->sram[REMOTE + 8], uint32_t(remote_board_id)); // board_id_lo
        p_tile->soft_reset_0 &= ~0x800u;
    } else {
        mem_wr<uint32_t>(&p_tile->sram[0x7CC04], 2); // eth_status.port_status = PORT_DOWN (no link)
    }
    mem_wr<uint8_t>(&p_tile->sram[0x7CFBE], 1); // major
    mem_wr<uint8_t>(&p_tile->sram[0x7CFBD], 6); // minor
    mem_wr<uint8_t>(&p_tile->sram[0x7CFBC], 0); // patch
    // eth_status.postcode reports how far the base FW's eth_init() got; tt-metal waits for a terminal
    // value (PASS/FAIL/SKIP) on every idle eth core before resetting it. Init completes -> PASS.
    mem_wr<uint32_t>(&p_tile->sram[0x7CC00], 0xC0DEA000); // eth_status.postcode = POSTCODE_ETH_INIT_PASS
}
#endif

void a_tile_init() {
    base_tile_init<'A'>(&g_a_tile.base, 0);
#if TT_ARCH_VERSION == 0
    g_a_tile.reset_unit_scratch[0] = 0xC0DE0001;

    // UMD requires firmware version written to legacy csm region
    mem_wr<uint32_t>(
        &g_a_tile.csm[ARC_SMBUS_TELEMETRY_CSM_OFFSET + LEGACY_TELEM_FW_BUNDLE_VERSION * 4], FLASH_BUNDLE_VERSION);
#elif TT_ARCH_VERSION == 1
    g_a_tile.scratch_ram[13] = ARC_CSM_BASE + ARC_TELEMETRY_TABLE_CSM_OFFSET;
    g_a_tile.scratch_ram[12] = ARC_CSM_BASE + ARC_TELEMETRY_VALUES_CSM_OFFSET;
    g_a_tile.scratch_ram[2] = 0x5;  // ARC_BOOT_STATUS: bit 2 = ARC core started. bit 0 = ready to receive messages
    mem_wr<uint32_t>(&g_a_tile.csm[ARC_MSG_QCB_CSM_OFFSET + 0], ARC_CSM_BASE + ARC_MSG_QUEUE_CSM_OFFSET);
    // QCB word 1: entries-per-queue in byte 0, queue count in byte 1
    mem_wr<uint32_t>(&g_a_tile.csm[ARC_MSG_QCB_CSM_OFFSET + 4], ARC_MSG_QUEUE_NUM_ENTRIES | (ARC_MSG_NUM_QUEUES << 8));
    g_a_tile.scratch_ram[11] = ARC_CSM_BASE + ARC_MSG_QCB_CSM_OFFSET; // message-queue control block ptr
#endif

    // Not static: the table captures g_current_chip_id (ASIC_ID / ASIC_LOCATION below), so
    // it must be rebuilt for each chip rather than frozen at the first chip's a_tile_init.
    uint64_t chip_board_id = board_id(g_current_chip_id);
    uint32_t chip_asic_location = asic_location(g_current_chip_id);
#if TT_ARCH_VERSION == 1
    uint32_t chip_pcie_usage = pcie_usage(g_current_chip_id);
#endif
    bool any_rows_harvested = logical_harvesting_mask(g_current_chip_id) != 0;

    const struct {
        uint16_t tag;
        uint32_t value;
    } telem[] = {
        {1, uint32_t(chip_board_id >> 32)},
        {2, uint32_t(chip_board_id)},
        {3, 1 + g_current_chip_id},     // ASIC_ID
        {4, any_rows_harvested},        // HARVESTING_STATE
        {11, 40u << 16},                // ASIC_TEMPERATURE: 40 C
        {13, 35u << 16},                // BOARD_TEMPERATURE: 35 C
        {14, 1000},                     // AICLK (MHz)
        {15, 900},                      // AXICLK (MHz)
        {16, 540},                      // ARCCLK (MHz)
        {21, ETH_LIVE_STATUS_MASK},     // ETH_LIVE_STATUS: one bit per live eth tile
        {22, GDDR_STATUS_TRAINED},      // GDDR_STATUS: all channels trained (2 bits/channel, 0b01)
        {28, FLASH_BUNDLE_VERSION},
        {32, 1},                        // TIMER_HEARTBEAT
        {52, chip_asic_location},       // ASIC_LOCATION
#if TT_ARCH_VERSION == 1
        {38, chip_pcie_usage},          // PCIE_USAGE
        {35, 0x0FFF},                   // ENABLED_ETH: harvest the top 2 eth channels (12,13)
        {61, 0x0},                      // ASIC_ID_HIGH
        {62, 1 + g_current_chip_id},    // ASIC_ID_LOW
#endif
    };
    constexpr uint32_t n = std::size(telem);
    mem_wr<uint32_t>(&g_a_tile.csm[ARC_TELEMETRY_TABLE_CSM_OFFSET + 0], 1); // version
    mem_wr<uint32_t>(&g_a_tile.csm[ARC_TELEMETRY_TABLE_CSM_OFFSET + 4], n); // entry_count
    for (uint32_t i = 0; i < n; i++) {
        mem_wr<uint32_t>(&g_a_tile.csm[(ARC_TELEMETRY_TABLE_CSM_OFFSET + 8) + 4 * i], telem[i].tag | (i << 16));
        mem_wr<uint32_t>(&g_a_tile.csm[ARC_TELEMETRY_VALUES_CSM_OFFSET + 4 * i], telem[i].value);
    }
}

static uint32_t tile_to_coord(char tile_type, uint32_t tile_id) {
    if (tile_type == 'T') {
        TTSIM_ASSERT(tile_id < NUM_T_TILES);
#if TT_ARCH_VERSION == 0
        uint32_t tile_x = tile_id % 8;
        uint32_t tile_y = tile_id / 8;
        tile_x += (tile_x >= 4) ? 2 : 1;
        tile_y += (tile_y >= 5) ? 2 : 1;
        return tile_x | (tile_y << 6);
#elif TT_ARCH_VERSION == 1
        uint32_t tile_x = tile_id % 14;
        uint32_t tile_y = tile_id / 14;
        tile_x += (tile_x >= 7) ? 3 : 1;
        tile_y += 2;
        return tile_x | (tile_y << 6);
#else
#error unsupported
#endif
    } else
    if (tile_type == 'E') {
        TTSIM_ASSERT(tile_id < NUM_E_TILES);
#if TT_ARCH_VERSION == 0
        uint32_t tile_x = tile_id % 8;
        uint32_t tile_y = (tile_id / 8) * 6;
        if (tile_x & 1) {
            tile_x = tile_x / 2;
        } else {
            tile_x = (6 - tile_x) / 2 + 4;
        }
        tile_x += (tile_x >= 4) ? 2 : 1;
        return tile_x | (tile_y << 6);
#else
        uint32_t tile_x;
        if (tile_id & 1) {
            tile_x = (13 - tile_id) / 2 + 7;
        } else {
            tile_x = tile_id / 2;
        }
        tile_x += (tile_x >= 7) ? 3 : 1;
        return tile_x | (1 << 6);
#endif
    } else if (tile_type == 'A') {
        TTSIM_ASSERT(tile_id == 0);
#if TT_ARCH_VERSION == 0
        return (0 | (10 << 6));
#else
        return (8 | (0 << 6));
#endif
    } else if (tile_type == 'P') {
#if TT_ARCH_VERSION == 0
        TTSIM_ASSERT(tile_id == 0);
        return (0 | (3 << 6));
#else
        TTSIM_ASSERT(tile_id <= 1);
        TTSIM_VERIFY(!tile_id, UnsupportedFunctionality, "PCIE tile 1 access");
        return (2 | (0 << 6));
#endif
    } else
    {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}

static std::pair<char, uint32_t> coord_to_tile(uint32_t coord) {
    switch (coord) {
#if TT_ARCH_VERSION == 0
        case 0 | (0 << 6):
        case 0 | (1 << 6):
        case 0 | (11 << 6):
            return {'D', 0}; // DRAM channel 0
        case 0 | (5 << 6):
        case 0 | (6 << 6):
        case 0 | (7 << 6):
            return {'D', 1}; // DRAM channel 1
        case 5 | (0 << 6):
        case 5 | (1 << 6):
        case 5 | (11 << 6):
            return {'D', 2}; // DRAM channel 2
        case 5 | (2 << 6):
        case 5 | (9 << 6):
        case 5 | (10 << 6):
            return {'D', 3}; // DRAM channel 3
        case 5 | (3 << 6):
        case 5 | (4 << 6):
        case 5 | (8 << 6):
            return {'D', 4}; // DRAM channel 4
        case 5 | (5 << 6):
        case 5 | (6 << 6):
        case 5 | (7 << 6):
            return {'D', 5}; // DRAM channel 5
        case 0 | (3 << 6):
            return {'P', 0}; // PCIE tile (just one)
        case 0 | (10 << 6):
            return {'A', 0}; // ARC tile (just one)
#elif TT_ARCH_VERSION == 1
        case 0 | (0 << 6):
        case 0 | (1 << 6):
        case 0 | (11 << 6):
            return {'D', 0}; // DRAM channel 0
        case 0 | (2 << 6):
        case 0 | (3 << 6):
        case 0 | (10 << 6):
            return {'D', 1}; // DRAM channel 1
        case 0 | (4 << 6):
        case 0 | (8 << 6):
        case 0 | (9 << 6):
            return {'D', 2}; // DRAM channel 2
        case 0 | (5 << 6):
        case 0 | (6 << 6):
        case 0 | (7 << 6):
            return {'D', 3}; // DRAM channel 3
        case 9 | (0 << 6):
        case 9 | (1 << 6):
        case 9 | (11 << 6):
            return {'D', 4}; // DRAM channel 4
        case 9 | (2 << 6):
        case 9 | (3 << 6):
        case 9 | (10 << 6):
            return {'D', 5}; // DRAM channel 5
        case 9 | (4 << 6):
        case 9 | (8 << 6):
        case 9 | (9 << 6):
            return {'D', 6}; // DRAM channel 6
        case 9 | (5 << 6):
        case 9 | (6 << 6):
        case 9 | (7 << 6):
            return {'D', 7}; // DRAM channel 7
        case 2 | (0 << 6):
            return {'P', 0}; // PCIE tile 0
        case 8 | (0 << 6):
            return {'A', 0}; // ARC tile (just one)
        case 11 | (0 << 6):
            TTSIM_ERROR(UnsupportedFunctionality, "PCIE tile 1 access"); // would be {'P', 1}
#endif
        default:
            break;
    }
    uint32_t coord_x = coord & 63;
    uint32_t coord_y = coord >> 6;
    uint32_t tile_x, tile_y;
    switch (coord_x) {
#if TT_ARCH_VERSION == 0
        case 1 ... 4:
            tile_x = coord_x - 1;
            break;
        case 6 ... 9:
            tile_x = coord_x - 2;
            break;
#elif TT_ARCH_VERSION == 1
        case 1 ... 7:
            tile_x = coord_x - 1;
            break;
        case 10 ... 16:
            tile_x = coord_x - 3;
            break;
#endif
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "coord (%d,%d)", coord_x, coord_y);
    }
    uint32_t tile_id;
    switch (coord_y) {
#if TT_ARCH_VERSION == 0
        case 0: case 6:
            if (tile_x < 4) {
                tile_id = 2*tile_x + 1;
            } else {
                tile_id = 6 - 2*(tile_x - 4);
            }
            if (coord_y) {
                tile_id += 8;
            }
            TTSIM_ASSERT(tile_id < NUM_E_TILES);
            return {'E', tile_id};
        case 1 ... 5:
            tile_y = coord_y - 1;
            tile_id = tile_x + tile_y*8;
            TTSIM_ASSERT(tile_id < NUM_T_TILES);
            return {'T', tile_id};
        case 7 ... 11:
            tile_y = coord_y - 2;
            tile_id = tile_x + tile_y*8;
            TTSIM_ASSERT(tile_id < NUM_T_TILES);
            return {'T', tile_id};
#elif TT_ARCH_VERSION == 1
        case 1:
            if (tile_x < 7) {
                tile_id = 2*tile_x;
            } else {
                tile_id = 13 - 2*(tile_x - 7);
            }
            TTSIM_ASSERT(tile_id < NUM_E_TILES);
            return {'E', tile_id};
        case 2 ... 11:
            tile_y = coord_y - 2;
            tile_id = tile_x + tile_y*14;
            TTSIM_ASSERT(tile_id < NUM_T_TILES);
            return {'T', tile_id};
#endif
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "coord (%d,%d)", coord_x, coord_y);
    }
}

static uint32_t riscv_tdma_regs_rd32(uint32_t tile_id, uint32_t offset) {
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case RISCV_TDMA_REGS_STATUS: return RISCV_TDMA_REGS_STATUS_RESET_VALUE;
#endif
        RISCV_TDMA_REGS_RD_DEFAULT_CASES()
        default: TTSIM_ERROR(UnsupportedFunctionality, "offset=0x%x", offset);
    }
}

static void riscv_tdma_regs_wr32(uint32_t tile_id, uint32_t offset, uint32_t data) {
#if TT_ARCH_VERSION == 0
    TensixTile *p_tile = &g_t_tiles[tile_id];
#endif
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case RISCV_TDMA_REGS_XMOV_SRC_ADDR: p_tile->tdma_xmov_src_addr = data; break; // L1 address >> 4
        case RISCV_TDMA_REGS_XMOV_DST_ADDR: // must be MEM_MOVER_VIEW_IRAM_BASE_ADDR (0x4 << 12)
            TTSIM_VERIFY(data == 0x4000, UnsupportedFunctionality, "XMOV_DST_ADDR=0x%x", data);
            break;
        case RISCV_TDMA_REGS_XMOV_SIZE: p_tile->tdma_xmov_size = data; break; // size in bytes >> 4
        case RISCV_TDMA_REGS_XMOV_DIRECTION: // must be XMOV_L1_TO_L0
            TTSIM_VERIFY(data == 1, UnsupportedFunctionality, "XMOV_DIRECTION=0x%x", data);
            break;
        case RISCV_TDMA_REGS_COMMAND_ADDR: {
            TTSIM_VERIFY(data == 0x40, UnsupportedFunctionality, "data=0x%x", data); // cmd=CMD_TDMA_XMOV, mover_number=0
            uint32_t src_addr = p_tile->tdma_xmov_src_addr;
            uint32_t size = p_tile->tdma_xmov_size;
            TTSIM_VERIFY(size <= (RV32_IRAM_SIZE / 16), UndefinedBehavior, "TDMA xmov_size=%d", size);
            TTSIM_VERIFY(src_addr + size <= (TENSIX_SRAM_SIZE / 16), UndefinedBehavior, "TDMA xmov_src_addr=0x%x size=%d", src_addr, size);
            memcpy(p_tile->ncrisc_iram, &p_tile->sram[src_addr * 16], size * 16);
            break;
        }
#endif
        case RISCV_TDMA_REGS_CLK_GATE_EN: break;
        RISCV_TDMA_REGS_WR_DEFAULT_CASES()
        default: TTSIM_ERROR(UnsupportedFunctionality, "offset=0x%x", offset);
    }
}

static inline void set_bits(uint32_t *array, uint32_t n_words, uint32_t shift, uint32_t data) {
    uint32_t word = shift / 32;
    array[word] |= uint64_t(data) << (shift & 31);
    if (word + 1 < n_words) {
        array[word + 1] |= uint64_t(data) >> (32 - (shift & 31));
    }
}

static uint32_t debug_bus_rd_data(const TensixTile *p_tile) {
    uint32_t ctrl = p_tile->dbg_bus_ctrl;
    TTSIM_VERIFY((bits<29,29>(ctrl)), UnsupportedFunctionality, "DBG_BUS_CTRL must be enabled");
    uint32_t signal_sel = bits<15,0>(ctrl);
    uint32_t daisy_sel = bits<23,16>(ctrl);
    uint32_t read32_sel = bits<26,25>(ctrl);

    const TensixState *p_tensix = &p_tile->tensix[0];
    uint32_t data[4] = {0};
    if (daisy_sel == 6) {
        if (signal_sel <= 5) { // ADCs
            uint32_t which = signal_sel >> 1;
            uint32_t channel = signal_sel & 1;
            uint32_t pipe = (which == 2) ? 2 : 0; // always reads thread 2's ADCs for packer, thread 0's ADCs for both unpackers
            const TensixAddrCtrl *p = &p_tensix->addr_ctrl[pipe][which];
            set_bits(data, 4, 0,   channel ? p->ch1_x    : p->ch0_x);
            set_bits(data, 4, 18,  channel ? p->ch1_x_cr : p->ch0_x_cr);
            set_bits(data, 4, 64,  channel ? p->ch1_y    : p->ch0_y);
            set_bits(data, 4, 80,  channel ? p->ch1_y_cr : p->ch0_y_cr);
            set_bits(data, 4, 96,  channel ? p->ch1_z    : p->ch0_z);
            set_bits(data, 4, 104, channel ? p->ch1_z_cr : p->ch0_z_cr);
            set_bits(data, 4, 112, channel ? p->ch1_w    : p->ch0_w);
            set_bits(data, 4, 120, channel ? p->ch1_w_cr : p->ch0_w_cr);
            return data[read32_sel];
        }
        if (signal_sel == 9) { // SrcA/SrcB control
            set_bits(data, 4, 84, !(p_tensix->src_a_valid & (1 << p_tensix->src_a_unpack_bank)));
            set_bits(data, 4, 85, !!(p_tensix->src_a_valid & (1 << p_tensix->src_a_matrix_bank)));
            // XXX may want to fill in bits 89:86 (slight variations on these other signals)
            set_bits(data, 4, 90, !!(p_tensix->src_a_valid & 2));
            set_bits(data, 4, 91, !!(p_tensix->src_a_valid & 1));
            set_bits(data, 4, 92, p_tensix->src_a_matrix_bank);
            set_bits(data, 4, 93, p_tensix->src_a_matrix_bank);
            set_bits(data, 4, 94, p_tensix->src_a_unpack_bank);
#if TT_ARCH_VERSION == 0
            set_bits(data, 4, 100, !(p_tensix->src_b_valid & (1 << p_tensix->src_b_unpack_bank)));
            set_bits(data, 4, 101, !!(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank)));
            // XXX may want to fill in bits 105:102 (slight variations on these other signals)
            set_bits(data, 4, 106, !!(p_tensix->src_b_valid & 2));
            set_bits(data, 4, 107, !!(p_tensix->src_b_valid & 1));
            set_bits(data, 4, 108, p_tensix->src_b_matrix_bank);
            set_bits(data, 4, 109, p_tensix->src_b_matrix_bank);
            set_bits(data, 4, 110, p_tensix->src_b_unpack_bank);
#else
            set_bits(data, 4, 100, p_tensix->src_b_matrix_bank);
            set_bits(data, 4, 101, p_tensix->src_b_matrix_bank);
            set_bits(data, 4, 102, p_tensix->src_b_unpack_bank);
            // XXX may want to fill in bits 104:103 (slight variations on these other signals)
            set_bits(data, 4, 105, !!(p_tensix->src_b_valid & 1));
            set_bits(data, 4, 106, !!(p_tensix->src_b_valid & 2));
#endif
            return data[read32_sel];
        }
    } else if (daisy_sel == 3) {
        if (signal_sel == 4) { // fidelity phases
            constexpr uint32_t FIDELITY_BASE_BIT = TT_ARCH_VERSION ? 60 : 62;
            for (uint32_t pipe = 0; pipe < 3; pipe++) {
                set_bits(data, 4, FIDELITY_BASE_BIT + 2*pipe, p_tensix->fidelity[pipe]);
            }
            return data[read32_sel];
        }
#if TT_ARCH_VERSION == 0
        if (signal_sel == 2) { // RWCs
            for (uint32_t pipe = 0; pipe < 3; pipe++) {
                set_bits(data, 4, 16*pipe,      p_tensix->src_a_rwc[pipe]);
                set_bits(data, 4, 16*pipe + 8,  p_tensix->src_a_rwc_cr[pipe]);
                set_bits(data, 4, 16*pipe + 48, p_tensix->src_b_rwc[pipe]);
                set_bits(data, 4, 16*pipe + 56, p_tensix->src_b_rwc_cr[pipe]);
            }
            set_bits(data, 4, 96,  p_tensix->dst_rwc[0]);
            set_bits(data, 4, 112, p_tensix->dst_rwc_cr[0]);
            return data[read32_sel];
        } else if (signal_sel == 3) {
            set_bits(data, 4, 0,  p_tensix->dst_rwc[1]);
            set_bits(data, 4, 16, p_tensix->dst_rwc_cr[1]);
            set_bits(data, 4, 32, p_tensix->dst_rwc[2]);
            set_bits(data, 4, 48, p_tensix->dst_rwc_cr[2]);
            return data[read32_sel];
        }
#else
        if ((signal_sel == 2) || (signal_sel == 3)) { // RWCs
            uint32_t dbus[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (uint32_t pipe = 0; pipe < 3; pipe++) {
                set_bits(dbus, 8, 16*pipe,       p_tensix->src_a_rwc[pipe]);
                set_bits(dbus, 8, 16*pipe + 8,   p_tensix->src_a_rwc_cr[pipe]);
                set_bits(dbus, 8, 16*pipe + 60,  p_tensix->src_b_rwc[pipe]);
                set_bits(dbus, 8, 16*pipe + 68,  p_tensix->src_b_rwc_cr[pipe]);
                set_bits(dbus, 8, 20*pipe + 120, p_tensix->dst_rwc[pipe]);
                set_bits(dbus, 8, 20*pipe + 130, p_tensix->dst_rwc_cr[pipe]);
            }
            return dbus[((signal_sel == 3) ? 4 : 0) + read32_sel];
        }
#endif
    }
    TTSIM_ERROR(UnimplementedFunctionality, "daisy_sel=%d signal_sel=%d read32_sel=%d", daisy_sel, signal_sel, read32_sel);
}

template<char tile_type>
static uint32_t riscv_debug_regs_rd32(uint32_t tile_id, uint32_t tensix_id, uint32_t offset) {
    auto *p_tile = get_tile<tile_type>(tile_id);
    switch (offset) {
        case RISCV_DEBUG_REGS_DBG_ARRAY_RD_DATA:
            if constexpr (tile_type == 'T') {
                TTSIM_VERIFY(p_tile->dbg_array_rd_en, UnsupportedFunctionality, "DBG_ARRAY_RD_DATA when DBG_ARRAY_RD_EN=0");
                uint32_t row = bits<11,0>(p_tile->dbg_array_rd_cmd);
                uint32_t sel = bits<15,12>(p_tile->dbg_array_rd_cmd);
                uint32_t upper = bits<31,16>(p_tile->dbg_array_rd_cmd);
                TTSIM_VERIFY(row < DST_ROWS, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD: row=%d", row);
                TTSIM_VERIFY(sel < 8, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD: sel=%d", sel);
                TTSIM_VERIFY(upper == 2, MissingSpecification, "DBG_ARRAY_RD_CMD: upper=0x%x", upper);
                // Note: BH does not appear to apply this condition in most cases, though in some cases
                // it will pick up carried state from a previous packer instruction (not modeled here)
                if ((TT_ARCH_VERSION == 1) || p_tile->tensix[0].dst_row_valid[row]) {
                    return p_tile->tensix[0].dst[row][2*sel] | (uint32_t(p_tile->tensix[0].dst[row][2*sel + 1]) << 16);
                } else {
                    return 0;
                }
            }
            TTSIM_ERROR(UnsupportedFunctionality, "DBG_ARRAY_RD_DATA in eth tile");
        case RISCV_DEBUG_REGS_DBG_RD_DATA:
            if constexpr (tile_type == 'T') {
                return debug_bus_rd_data(p_tile);
            }
            TTSIM_ERROR(UnsupportedFunctionality, "DBG_RD_DATA in eth tile");
        case RISCV_DEBUG_REGS_DBG_INSTRN_BUF_STATUS: TTSIM_ERROR(UnimplementedFunctionality, "DBG_INSTRN_BUF_STATUS");
        case RISCV_DEBUG_REGS_DBG_FEATURE_DISABLE:
            if constexpr (tile_type == 'T') {
                return 0;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "DBG_FEATURE_DISABLE in eth tile");
        case RISCV_DEBUG_REGS_TENSIX_CREG_RDDATA:
            if constexpr (tile_type == 'T') {
                return p_tile->tensix_creg_rddata;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "TENSIX_CREG_RDDATA in eth tile");
        case RISCV_DEBUG_REGS_SOFT_RESET_0: return p_tile->soft_reset_0;
        case RISCV_DEBUG_REGS_WALL_CLOCK_0: return uint32_t(g_clock);
        case RISCV_DEBUG_REGS_WALL_CLOCK_1: return uint32_t(g_clock >> 32); // high part, unlatched
        case RISCV_DEBUG_REGS_WALL_CLOCK_1_AT: return uint32_t(g_clock >> 32); // XXX add the latching behavior on this reg
#if TT_ARCH_VERSION == 1
        case RISCV_DEBUG_REGS_TRISC_RESET_PC_OVERRIDE:
            if constexpr (tile_type == 'T') {
                return p_tile->trisc_reset_pc_override;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "TRISC_RESET_PC_OVERRIDE in eth tile");
        case RISCV_DEBUG_REGS_TRISC0_RESET_PC:
            if constexpr (tile_type == 'T') {
                return p_tile->trisc0_reset_pc;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "TRISC0_RESET_PC in eth tile");
        case RISCV_DEBUG_REGS_TRISC1_RESET_PC:
            if constexpr (tile_type == 'T') {
                return p_tile->trisc1_reset_pc;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "TRISC1_RESET_PC in eth tile");
        case RISCV_DEBUG_REGS_TRISC2_RESET_PC:
            if constexpr (tile_type == 'T') {
                return p_tile->trisc2_reset_pc;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "TRISC2_RESET_PC in eth tile");
#endif
        RISCV_DEBUG_REGS_RD_DEFAULT_CASES()
        default: TTSIM_ERROR(UndefinedBehavior, "offset=0x%x", offset);
    }
}

static void riscv_debug_regs_wr32(uint32_t tile_id, uint32_t tensix_id, uint32_t offset, uint32_t data) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
    switch (offset) {
        case RISCV_DEBUG_REGS_DBG_BUS_CTRL:
            TTSIM_VERIFY(!(data & 0xD9000000), UnsupportedFunctionality, "reserved bit set in DBG_BUS_CTRL=0x%x", data);
            p_tile->dbg_bus_ctrl = data;
            break;
        case RISCV_DEBUG_REGS_TENSIX_CREG_READ:
            TTSIM_VERIFY(data < TENSIX_CFG_STATE_SIZE*4, UnimplementedFunctionality,
                "TENSIX_CREG_READ: data=0x%x", data);
            p_tile->tensix_creg_rddata = tensix_cfg_rd32(&p_tile->tensix[0], 0, 4*data);
            break;
        case RISCV_DEBUG_REGS_DBG_ARRAY_RD_EN:
            TTSIM_VERIFY(data <= 1, UnsupportedFunctionality, "DBG_ARRAY_RD_EN: data=0x%x", data);
            p_tile->dbg_array_rd_en = data;
            break;
        case RISCV_DEBUG_REGS_DBG_ARRAY_RD_CMD:
            TTSIM_VERIFY(p_tile->dbg_array_rd_en, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD when DBG_ARRAY_RD_EN=0");
            p_tile->dbg_array_rd_cmd = data;
            break;
        case RISCV_DEBUG_REGS_DBG_FEATURE_DISABLE:
            TTSIM_VERIFY(!data, UnsupportedFunctionality, "DBG_FEATURE_DISABLE=0x%x", data);
            break;
        case RISCV_DEBUG_REGS_SOFT_RESET_0: {
            data &= 0x7FFFFFFF; // ignore bit 31 written by UMD
            TTSIM_VERIFY(!(data & ~0x47800), UnimplementedFunctionality, "SOFT_RESET_0=0x%x", data);
            static const uint32_t soft_reset_bits[] = {0x800, 0x1000, 0x2000, 0x4000, 0x40000};
            static_assert(std::size(soft_reset_bits) == RV32_CORES_PER_T_TILE);
            for (uint32_t rv32_id = 0; rv32_id < RV32_CORES_PER_T_TILE; rv32_id++) {
                if (data & soft_reset_bits[rv32_id]) { // put core into reset
                    ttsim_rv32_set_core_active('T', tile_id, rv32_id, false);
                } else if (!ttsim_rv32_get_core_active('T', tile_id, rv32_id)) { // take core out of reset
                    ttsim_rv32_set_core_active('T', tile_id, rv32_id, true);
                    uint32_t reset_pc;
                    switch (rv32_id) {
                        case RV32_ID_BRISC: reset_pc = 0; break; // reset PC for brisc is always zero
                        case RV32_ID_TRISC0:
                            reset_pc = (p_tile->trisc_reset_pc_override & 1) ? p_tile->trisc0_reset_pc : 0x6000;
                            break;
                        case RV32_ID_TRISC1:
                            reset_pc = (p_tile->trisc_reset_pc_override & 2) ? p_tile->trisc1_reset_pc : 0xA000;
                            break;
                        case RV32_ID_TRISC2:
                            reset_pc = (p_tile->trisc_reset_pc_override & 4) ? p_tile->trisc2_reset_pc : 0xE000;
                            break;
                        case RV32_ID_NCRISC:
                            reset_pc = (p_tile->ncrisc_reset_pc_override & 1) ? p_tile->ncrisc_reset_pc : 0x12000;
                            break;
                        default: TTSIM_ERROR(AssertionFailure, "rv32_id=%d", rv32_id);
                    }
                    p_tile->rv32[rv32_id].pc = reset_pc;
                }
            }
            p_tile->soft_reset_0 = data;
            break;
        }
#if TT_ARCH_VERSION == 1
        case RISCV_DEBUG_REGS_TRISC0_RESET_PC: p_tile->trisc0_reset_pc = data; break;
        case RISCV_DEBUG_REGS_TRISC1_RESET_PC: p_tile->trisc1_reset_pc = data; break;
        case RISCV_DEBUG_REGS_TRISC2_RESET_PC: p_tile->trisc2_reset_pc = data; break;
        case RISCV_DEBUG_REGS_TRISC_RESET_PC_OVERRIDE: p_tile->trisc_reset_pc_override = data; break;
        case RISCV_DEBUG_REGS_NCRISC_RESET_PC: p_tile->ncrisc_reset_pc = data; break;
        case RISCV_DEBUG_REGS_NCRISC_RESET_PC_OVERRIDE: p_tile->ncrisc_reset_pc_override = data; break;
        case RISCV_DEBUG_REGS_DEST_CG_CTRL: break;
#endif
        RISCV_DEBUG_REGS_WR_DEFAULT_CASES()
        default: TTSIM_ERROR(UndefinedBehavior, "offset=0x%x", offset);
    }
}

// XXX This only handles some cases for WH/BH; need to finish it out
// XXX Need to do this based on the remapping registers
uint32_t remap_virtual_coordinate(const BaseTile *base_tile, uint32_t noc_instance, uint32_t coord) {
    uint32_t coord_x = coord & 63;
    uint32_t coord_y = coord >> 6;
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(coord <= 0xFFF, AssertionFailure, "coord=0x%x", coord);
    TTSIM_VERIFY((coord_x < 32) && (coord_y < 32), UnimplementedFunctionality, "invalid virtual coordinate %d,%d", coord_x, coord_y);
    if (base_tile->niu_cfg_0[noc_instance] & (1u << 14)) {
        coord_x = noc_translation_table_get(base_tile->noc_translation_x[noc_instance], coord_x);
        coord_y = noc_translation_table_get(base_tile->noc_translation_y[noc_instance], coord_y);
    }
    TTSIM_VERIFY((coord_x <= 9) && (coord_y <= 11), UnimplementedFunctionality, "invalid coordinate after virtualization %d,%d", coord_x, coord_y);
    if (noc_instance) {
        coord_x = 9 - coord_x;
        coord_y = 11 - coord_y;
    }
    return coord_x | (coord_y << 6);
#elif TT_ARCH_VERSION == 1
    if (coord_y <= 1) { // first two rows have no translation at all
        // do nothing
    } else if (coord_y <= 11) { // Tensix tiles
        TTSIM_VERIFY((coord_x >= 1) && (coord_x <= 16), UnimplementedFunctionality, "invalid coordinate (%d,%d)", coord_x, coord_y);
        // no translation applied, at least not until we support harvesting
    } else if (coord_y == 25) { // Logical Ethernet tiles
        TTSIM_VERIFY((coord_x >= 20) && (coord_x <= 31), UnimplementedFunctionality, "invalid coordinate (%d,%d)", coord_x, coord_y);
        uint32_t tile_id = coord_x - 20; // E tile ID
        if (tile_id & 1) {
            coord_x = 16 - (tile_id >> 1);
        } else {
            coord_x = 1 + (tile_id >> 1);
        }
        coord_y = 1;
    } else if ((coord_x == 17) || (coord_x == 18)) { // DRAM tiles
        switch (coord_y) {
            case 12: coord_y = 0; break;
            case 13: coord_y = 1; break;
            case 14: coord_y = 11; break;
            case 15: coord_y = 2; break;
            case 16: coord_y = 10; break;
            case 17: coord_y = 3; break;
            case 18: coord_y = 5; break;
            case 19: coord_y = 7; break;
            case 20: coord_y = 6; break;
            case 21: coord_y = 9; break;
            case 22: coord_y = 4; break;
            case 23: coord_y = 8; break;
            default: TTSIM_ERROR(UnimplementedFunctionality, "invalid coordinate (%d,%d)", coord_x, coord_y);
        }
        coord_x = (coord_x == 18) ? 0 : 9;
    } else if ((coord_x == 19) && (coord_y == 24)) { // PCIE tile -- assumes PCIE 0 for now
        coord_x = 2;
        coord_y = 0;
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "don't know how to translate (%d,%d)", coord_x, coord_y);
    }
#endif
    return coord_x | (coord_y << 6);
}

static uint32_t noc_node_id(uint32_t noc_instance, uint32_t coord) {
#if TT_ARCH_VERSION == 0
    if (noc_instance) {
        coord = (9 | (11 << 6)) - coord;
    }
    // XXX other fields of this register need to be filled in
    uint32_t routing_dir = !noc_instance; // true on NOC0, false on NOC1
    return coord | (10 << 12) | (12 << 19) | (routing_dir << 28);
#else
    if (noc_instance) {
        coord = (16 | (11 << 6)) - coord;
    }
    // XXX other fields of this register need to be filled in
    uint32_t routing_dir = !noc_instance; // true on NOC0, false on NOC1
    return coord | (17 << 12) | (12 << 19) | (routing_dir << 28);
#endif
}

// NOC registers common to every tile type, backed by BaseTile storage.
static uint32_t base_noc_regs_rd32(const BaseTile *p_tile, char tile_type, uint32_t tile_id, uint32_t noc_instance, uint32_t offset) {
    switch (offset) {
        case NOC_REGS_NOC_NODE_ID: {
            uint32_t coord = tile_to_coord(tile_type, tile_id);
            return noc_node_id(noc_instance, coord);
        }
        case NOC_REGS_NIU_CFG_0: return p_tile->niu_cfg_0[noc_instance];
        case NOC_REGS_ROUTER_CFG_0: return p_tile->router_cfg_0[noc_instance];
        case NOC_REGS_ROUTER_CFG_1: return p_tile->router_cfg_1[noc_instance];
        case NOC_REGS_ROUTER_CFG_2: return p_tile->router_cfg_2[noc_instance];
        case NOC_REGS_ROUTER_CFG_3: return p_tile->router_cfg_3[noc_instance];
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_0:
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_1:
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_2:
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_3:
            return p_tile->noc_translation_x[noc_instance][(offset - NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_0) / 4];
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_0:
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_1:
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_2:
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_3:
            return p_tile->noc_translation_y[noc_instance][(offset - NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_0) / 4];
#if TT_ARCH_VERSION == 0
        case NOC_REGS_NOC_ID_LOGICAL: return p_tile->noc_id_logical[noc_instance];
#elif TT_ARCH_VERSION == 1
        case NOC_REGS_NOC_ID_LOGICAL:
            if (tile_type == 'T') { // Without harvesting, logical == physical for Tensix tiles
                return tile_to_coord(tile_type, tile_id);
            } else if (tile_type == 'E') { // Ethernet tiles use logical coordinates (20+tile_id, 25) which the NIU translates to physical
                TTSIM_VERIFY(tile_id < 12, UnimplementedFunctionality, "NOC_ID_LOGICAL: tile_type=%c tile_id=%d", tile_type, tile_id);
                return (20 + tile_id) | (25 << 6);
            } else {
                TTSIM_ERROR(UnimplementedFunctionality, "noc_id_logical not implemented for tile_type=%c", tile_type);
            }
#endif
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c tile_id=%d offset=0x%x", tile_type, tile_id, offset);
    }
}

#if TT_ARCH_VERSION == 1
uint32_t pcie_niu_rd32(uint32_t noc_instance, uint32_t offset) {
    return base_noc_regs_rd32(&g_p_tile.base, 'P', 0, noc_instance, offset);
}

static uint32_t pcie_dbi_rd32(uint64_t offset) {
    switch (offset) {
        case DBI_DEVICE_CONTROL_DEVICE_STATUS: return g_p_tile.dbi_device_control;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "pcie_dbi: offset=0x%llx", offset);
    }
}

static void pcie_dbi_wr32(uint64_t offset, uint32_t value) {
    switch (offset) {
        case DBI_DEVICE_CONTROL_DEVICE_STATUS: g_p_tile.dbi_device_control = value; break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "pcie_dbi: offset=0x%llx", offset);
    }
}
#endif

template<char tile_type>
static uint32_t noc_regs_rd32(uint32_t tile_id, uint32_t noc_instance, uint32_t offset) {
#if TT_ARCH_VERSION == 0
    uint32_t cmd_buf = offset / 0x400;
#elif TT_ARCH_VERSION == 1
    uint32_t cmd_buf = offset / 0x800;
#endif
    auto *p_tile = get_tile<tile_type>(tile_id);
    switch (offset) {
        case NOC_REGS_NOC_TARG_ADDR_LO(0):
        case NOC_REGS_NOC_TARG_ADDR_LO(1):
        case NOC_REGS_NOC_TARG_ADDR_LO(2):
        case NOC_REGS_NOC_TARG_ADDR_LO(3):
            return p_tile->noc_targ_addr_lo[noc_instance][cmd_buf];
        case NOC_REGS_NOC_TARG_ADDR_MID(0):
        case NOC_REGS_NOC_TARG_ADDR_MID(1):
        case NOC_REGS_NOC_TARG_ADDR_MID(2):
        case NOC_REGS_NOC_TARG_ADDR_MID(3):
            return p_tile->noc_targ_addr_mid[noc_instance][cmd_buf];
#if TT_ARCH_VERSION == 1
        case NOC_REGS_NOC_TARG_ADDR_HI(0):
        case NOC_REGS_NOC_TARG_ADDR_HI(1):
        case NOC_REGS_NOC_TARG_ADDR_HI(2):
        case NOC_REGS_NOC_TARG_ADDR_HI(3):
            return p_tile->noc_targ_addr_hi[noc_instance][cmd_buf];
#endif
        case NOC_REGS_NOC_RET_ADDR_LO(0):
        case NOC_REGS_NOC_RET_ADDR_LO(1):
        case NOC_REGS_NOC_RET_ADDR_LO(2):
        case NOC_REGS_NOC_RET_ADDR_LO(3):
            return p_tile->noc_ret_addr_lo[noc_instance][cmd_buf];
        case NOC_REGS_NOC_RET_ADDR_MID(0):
        case NOC_REGS_NOC_RET_ADDR_MID(1):
        case NOC_REGS_NOC_RET_ADDR_MID(2):
        case NOC_REGS_NOC_RET_ADDR_MID(3):
            return p_tile->noc_ret_addr_mid[noc_instance][cmd_buf];
#if TT_ARCH_VERSION == 1
        case NOC_REGS_NOC_RET_ADDR_HI(0):
        case NOC_REGS_NOC_RET_ADDR_HI(1):
        case NOC_REGS_NOC_RET_ADDR_HI(2):
        case NOC_REGS_NOC_RET_ADDR_HI(3):
            return p_tile->noc_ret_addr_hi[noc_instance][cmd_buf];
#endif
        case NOC_REGS_NOC_PACKET_TAG(0):
        case NOC_REGS_NOC_PACKET_TAG(1):
        case NOC_REGS_NOC_PACKET_TAG(2):
        case NOC_REGS_NOC_PACKET_TAG(3):
            return p_tile->noc_packet_tag[noc_instance][cmd_buf];
        case NOC_REGS_NOC_CTRL(0):
        case NOC_REGS_NOC_CTRL(1):
        case NOC_REGS_NOC_CTRL(2):
        case NOC_REGS_NOC_CTRL(3):
            return p_tile->noc_ctrl[noc_instance][cmd_buf];
        case NOC_REGS_NOC_AT_LEN_BE(0):
        case NOC_REGS_NOC_AT_LEN_BE(1):
        case NOC_REGS_NOC_AT_LEN_BE(2):
        case NOC_REGS_NOC_AT_LEN_BE(3):
            return p_tile->noc_at_len_be[noc_instance][cmd_buf];
        case NOC_REGS_NOC_AT_DATA(0):
        case NOC_REGS_NOC_AT_DATA(1):
        case NOC_REGS_NOC_AT_DATA(2):
        case NOC_REGS_NOC_AT_DATA(3):
            return p_tile->noc_at_data[noc_instance][cmd_buf];
        case NOC_REGS_NOC_CMD_CTRL(0):
        case NOC_REGS_NOC_CMD_CTRL(1):
        case NOC_REGS_NOC_CMD_CTRL(2):
        case NOC_REGS_NOC_CMD_CTRL(3):
            return 0; // always return "done" right now
        case NOC_REGS_NIU_MST_ATOMIC_RESP_RECEIVED: return p_tile->niu_mst_atomic_resp_received[noc_instance];
        case NOC_REGS_NIU_MST_WR_ACK_RECEIVED: return p_tile->niu_mst_wr_ack_received[noc_instance];
        case NOC_REGS_NIU_MST_RD_RESP_RECEIVED: return p_tile->niu_mst_rd_resp_received[noc_instance];
        case NOC_REGS_NIU_MST_POSTED_ATOMIC_SENT: return p_tile->niu_mst_posted_atomic_sent[noc_instance];
        case NOC_REGS_NIU_MST_NONPOSTED_WR_REQ_SENT: return p_tile->niu_mst_nonposted_wr_req_sent[noc_instance];
        case NOC_REGS_NIU_MST_POSTED_WR_REQ_SENT: return p_tile->niu_mst_posted_wr_req_sent[noc_instance];
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(0):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(1):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(2):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(3):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(4):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(5):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(6):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(7):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(8):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(9):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(10):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(11):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(12):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(13):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(14):
        case NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(15):
            return p_tile->niu_mst_reqs_outstanding[noc_instance][(offset - NOC_REGS_NIU_MST_REQS_OUTSTANDING_ID(0)) >> 2];
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(0):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(1):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(2):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(3):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(4):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(5):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(6):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(7):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(8):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(9):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(10):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(11):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(12):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(13):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(14):
        case NOC_REGS_NIU_MST_WRITE_REQS_OUTGOING_ID(15):
            return 0; // NIU_MST_WRITE_REQS_OUTGOING_ID would be incremented and decremented by the same amount for write transactions
        default:
            return base_noc_regs_rd32(&p_tile->base, tile_type, tile_id, noc_instance, offset);
    }
}

template<char tile_type>
static void noc_cmd_ctrl(uint32_t tile_id, uint32_t noc_instance, uint32_t cmd_buf) {
    auto *p_tile = get_tile<tile_type>(tile_id);
    uint32_t noc_at_len_be = p_tile->noc_at_len_be[noc_instance][cmd_buf];
    uint32_t txn_id = bits<13,10>(p_tile->noc_packet_tag[noc_instance][cmd_buf]);
    uint32_t noc_ctrl = p_tile->noc_ctrl[noc_instance][cmd_buf];
    TTSIM_VERIFY((!bits<2,2>(noc_ctrl)), UnimplementedFunctionality, "byte enable writes");
    // ignore bit 6 for now (NOC_CMD_VC_LINKED) -- need error checks
    // ignore bit 7 for now (NOC_CMD_VC_STATIC)
    TTSIM_VERIFY((!bits<9,9>(noc_ctrl)), UnimplementedFunctionality, "mem rd drop ack");
    TTSIM_VERIFY((!bits<12,10>(noc_ctrl)), UnsupportedFunctionality, "noc_ctrl=0x%x", noc_ctrl);
    // ignore bits 15:13 for now (NOC_CMD_STATIC_VC)
    TTSIM_VERIFY((!bits<16,16>(noc_ctrl)), UnimplementedFunctionality, "noc_ctrl=0x%x", noc_ctrl);
    TTSIM_VERIFY((!bits<26,18>(noc_ctrl)), UnsupportedFunctionality, "noc_ctrl=0x%x", noc_ctrl);
    TTSIM_VERIFY((!bits<30,27>(noc_ctrl)), UnimplementedFunctionality, "noc_ctrl=0x%x", noc_ctrl);
    TTSIM_VERIFY((!bits<31,31>(noc_ctrl)), UnsupportedFunctionality, "noc_ctrl=0x%x", noc_ctrl);
    if (bits<5,5>(noc_ctrl)) { // multicast
        TTSIM_VERIFY((!bits<0,0>(noc_ctrl)), UnimplementedFunctionality, "multicast atomics");
        TTSIM_VERIFY((bits<1,1>(noc_ctrl)), UnsupportedFunctionality, "multicast reads");
        TTSIM_VERIFY((!bits<3,3>(noc_ctrl)), UnimplementedFunctionality, "multicast inline writes");
        TTSIM_VERIFY((bits<8,8>(noc_ctrl)), UnimplementedFunctionality, "multicast must use path reserve");
    } else {
        TTSIM_VERIFY((!bits<8,8>(noc_ctrl)), UnimplementedFunctionality, "unicast must not use path reserve");
        TTSIM_VERIFY((!bits<17,17>(noc_ctrl)), UnimplementedFunctionality, "unicast must not use broadcast src_include");
        if (bits<0,0>(noc_ctrl)) { // atomic
            TTSIM_VERIFY((!bits<1,1>(noc_ctrl)), UnsupportedFunctionality, "atomic should be a read, not a write");
        } else if (!bits<1,1>(noc_ctrl)) { // read
            TTSIM_VERIFY((!bits<3,3>(noc_ctrl)), UnimplementedFunctionality, "read should not be inline");
            TTSIM_VERIFY((bits<4,4>(noc_ctrl)), UnimplementedFunctionality, "read should not be posted");
        }
    }

    uint64_t noc_targ_addr = uint64_t(p_tile->noc_targ_addr_lo[noc_instance][cmd_buf]) | (uint64_t(p_tile->noc_targ_addr_mid[noc_instance][cmd_buf]) << 32);
    uint64_t noc_ret_addr = uint64_t(p_tile->noc_ret_addr_lo[noc_instance][cmd_buf]) | (uint64_t(p_tile->noc_ret_addr_mid[noc_instance][cmd_buf]) << 32);
#if TT_ARCH_VERSION == 1
    uint64_t src_addr = noc_targ_addr;
    uint64_t dst_addr = noc_ret_addr;
#else
    uint64_t src_addr = noc_targ_addr & ((1ull << 36) - 1);
    uint64_t dst_addr = noc_ret_addr & ((1ull << 36) - 1);
#endif

#if TT_ARCH_VERSION == 1
    uint32_t src_coordinate = p_tile->noc_targ_addr_hi[noc_instance][cmd_buf];
    uint32_t dst_coordinate = p_tile->noc_ret_addr_hi[noc_instance][cmd_buf];
#else
    uint32_t src_coordinate = noc_targ_addr >> 36;
    uint32_t dst_coordinate = noc_ret_addr >> 36;
#endif
    uint32_t dst_coordinate_start = dst_coordinate;
    if (bits<5,5>(noc_ctrl)) { // multicast
        dst_coordinate_start = dst_coordinate >> 12;
        dst_coordinate &= 0xFFF;
    }
    TTSIM_VERIFY(src_coordinate <= 0xFFF, UnsupportedFunctionality, "src_coordinate=0x%x", src_coordinate);
    TTSIM_VERIFY(dst_coordinate <= 0xFFF, UnsupportedFunctionality, "dst_coordinate=0x%x", dst_coordinate);
    TTSIM_VERIFY(dst_coordinate_start <= 0xFFF, UnsupportedFunctionality, "dst_coordinate_start=0x%x", dst_coordinate_start);
    src_coordinate = remap_virtual_coordinate(&p_tile->base, noc_instance, src_coordinate);
    dst_coordinate = remap_virtual_coordinate(&p_tile->base, noc_instance, dst_coordinate);
    dst_coordinate_start = remap_virtual_coordinate(&p_tile->base, noc_instance, dst_coordinate_start);
    // XXX Apparently on WH, this writes the translated coords back to the ADDR_MID registers -- not the most straightforward
    auto [src_tile_type, src_tile_id] = coord_to_tile(src_coordinate);
    auto [dst_tile_type, dst_tile_id] = coord_to_tile(dst_coordinate);

    if (bits<5,5>(noc_ctrl)) { // multicast
        TTSIM_VERIFY((noc_at_len_be >= 1) && (noc_at_len_be <= NOC_MAX_PACKET_SIZE), UnsupportedFunctionality, "multicast: noc_at_len_be=%d", noc_at_len_be);
        TTSIM_VERIFY(src_tile_type == tile_type, UnimplementedFunctionality, "multicast: src_tile_type=%c", src_tile_type);
        TTSIM_VERIFY(src_tile_id == tile_id, UnimplementedFunctionality, "multicast: src_tile_id=0x%x", src_tile_id); // write must be from current tile
        TTSIM_VERIFY(src_addr + noc_at_len_be <= sizeof(p_tile->sram), UnsupportedFunctionality, "multicast: src_addr=0x%llx", src_addr);

        if (dst_addr < TENSIX_SRAM_SIZE) { // L1 -> L1
            TTSIM_VERIFY(dst_addr + noc_at_len_be <= TENSIX_SRAM_SIZE, UnsupportedFunctionality, "multicast: dst_addr=0x%llx", dst_addr);
        } else { // L1 -> MMIO
            TTSIM_VERIFY(noc_at_len_be == 4, UnsupportedFunctionality, "multicast register write: dst_addr=0x%llx len=%d", dst_addr, noc_at_len_be);
            TTSIM_VERIFY(!(dst_addr & 3), UnimplementedFunctionality, "multicast register write: misaligned dst_addr=0x%llx", dst_addr);
        }
        TTSIM_VERIFY((src_addr & 15) == (dst_addr & 15), UnimplementedFunctionality, "multicast: alignment of src_addr=0x%llx and dst_addr=0x%llx does not match",
            src_addr, dst_addr);

        uint32_t dst_start_x = dst_coordinate_start & 63;
        uint32_t dst_start_y = dst_coordinate_start >> 6;
        uint32_t dst_end_x = dst_coordinate & 63;
        uint32_t dst_end_y = dst_coordinate >> 6;
        // XXX This does not handle spans that wrap around (i.e. start > end). For NOC1, virtualization flips the
        // order of start and end, and then the span goes in the direction we support.
        if (noc_instance) {
            TTSIM_VERIFY(dst_start_x >= dst_end_x, UnimplementedFunctionality, "dst_start_x=%d dst_end_x=%d", dst_start_x, dst_end_x);
            TTSIM_VERIFY(dst_start_y >= dst_end_y, UnimplementedFunctionality, "dst_start_y=%d dst_end_y=%d", dst_start_y, dst_end_y);
            std::swap(dst_start_x, dst_end_x);
            std::swap(dst_start_y, dst_end_y);
        } else {
            TTSIM_VERIFY(dst_start_x <= dst_end_x, UnimplementedFunctionality, "dst_start_x=%d dst_end_x=%d", dst_start_x, dst_end_x);
            TTSIM_VERIFY(dst_start_y <= dst_end_y, UnimplementedFunctionality, "dst_start_y=%d dst_end_y=%d", dst_start_y, dst_end_y);
        }
        for (uint32_t dst_y = dst_start_y; dst_y <= dst_end_y; dst_y++) {
            for (uint32_t dst_x = dst_start_x; dst_x <= dst_end_x; dst_x++) {
                if ((p_tile->base.router_cfg_1[noc_instance] & (1ull << dst_x)) ||
                    (p_tile->base.router_cfg_3[noc_instance] & (1ull << dst_y))) {
                    continue;
                }
                dst_coordinate = dst_x | (dst_y << 6);
                auto [multicast_tile_type, multicast_tile_id] = coord_to_tile(dst_coordinate);
                TTSIM_VERIFY(multicast_tile_type == 'T', UnimplementedFunctionality, "multicast: multicast_tile_type=%c", multicast_tile_type);
                if ((multicast_tile_type == tile_type) && (multicast_tile_id == tile_id) && !bits<17,17>(noc_ctrl)) { // to source tile with BRCST_SRC_INCLUDE not set
                    continue;
                }
                tile_wr_bytes(dst_coordinate, dst_addr, &p_tile->sram[src_addr], noc_at_len_be);
                if (bits<4,4>(noc_ctrl)) { // nonposted
                    p_tile->niu_mst_wr_ack_received[noc_instance]++;
                    // NIU_MST_REQS_OUTSTANDING_ID is decremented once per response
                    p_tile->niu_mst_reqs_outstanding[noc_instance][txn_id]--;
                }
            }
        }
        if (bits<4,4>(noc_ctrl)) { // nonposted
            p_tile->niu_mst_nonposted_wr_req_sent[noc_instance]++;
            // NIU_MST_REQS_OUTSTANDING_ID is incremented once per request
            // This is the only transaction type that does not result in a net zero change in the counter
            p_tile->niu_mst_reqs_outstanding[noc_instance][txn_id]++;
        } else {
            p_tile->niu_mst_posted_wr_req_sent[noc_instance]++;
        }
    } else if (bits<0,0>(noc_ctrl)) { // atomic -- src is the remote tile, dst is the current tile
        // NOC_AT_WRAP(31) | NOC_AT_INS_INCR_GET, with bits 1:0 selecting the word within the 16B line
        TTSIM_VERIFY((noc_at_len_be & ~3u) == 0x107C, UnimplementedFunctionality, "atomic: noc_at_len_be=0x%x", noc_at_len_be);
        TTSIM_VERIFY(!(src_addr & 3) && (bits<3,2>(src_addr) == bits<1,0>(noc_at_len_be)), UnimplementedFunctionality,
            "atomic: misaligned src_addr=0x%llx noc_at_len_be=0x%x", src_addr, noc_at_len_be);
        TTSIM_VERIFY(!(dst_addr & 3), UnimplementedFunctionality, "atomic: misaligned dst_addr=0x%llx", dst_addr);

        TTSIM_VERIFY(dst_tile_type == tile_type, UnimplementedFunctionality, "atomic: dst_tile_type=%c", dst_tile_type);
        TTSIM_VERIFY(dst_tile_id == tile_id, UnimplementedFunctionality, "atomic: dst_tile_id=0x%x", dst_tile_id); // must be to current tile
        TTSIM_VERIFY(dst_addr + 4 <= sizeof(p_tile->sram), UnsupportedFunctionality, "atomic: dst_addr=0x%llx", dst_addr);
        TTSIM_VERIFY((src_tile_type == 'T') || (src_tile_type == 'E'), UnimplementedFunctionality, "atomic: src_tile_type=%c", src_tile_type);

        uint32_t noc_at_data = p_tile->noc_at_data[noc_instance][cmd_buf];
        uint32_t old_data;
        tile_rd_bytes(src_coordinate, src_addr, &old_data, 4);
        uint32_t data = old_data + noc_at_data;
        tile_wr_bytes(src_coordinate, src_addr, &data, 4);
        mem_wr<uint32_t>(&p_tile->sram[dst_addr], old_data);

        if (bits<4,4>(noc_ctrl)) { // nonposted
            p_tile->niu_mst_atomic_resp_received[noc_instance]++;
        } else {
            p_tile->niu_mst_posted_atomic_sent[noc_instance]++;
        }
    } else if (bits<3,3>(noc_ctrl)) { // inline write -- src is the remote tile, dst is the current tile
        TTSIM_VERIFY((src_tile_type == 'T') || (src_tile_type == 'E'), UnimplementedFunctionality, "inline write: src_tile_type=%c", src_tile_type);
        TTSIM_VERIFY(dst_tile_type == tile_type, UnimplementedFunctionality, "inline write: dst_tile_type=%c", dst_tile_type);
        TTSIM_VERIFY(dst_tile_id == tile_id, UnimplementedFunctionality, "inline write: dst_tile_id=0x%x", dst_tile_id); // must be from current tile
        if (src_addr >= RISCV_LOCAL_MEM_BASE) { // inline data -> MMIO
            // noc_at_len_be is ignored for inline MMIO writes, and a 4B write is always performed
            TTSIM_VERIFY(!(src_addr & 3), UnimplementedFunctionality, "inline write: misaligned src_addr=0x%llx", src_addr);
        } else { // inline data -> L1
#if TT_ARCH_VERSION == 1
            TTSIM_ERROR(UndefinedBehavior, "inline write to an L1 address is unsafe on Blackhole");
#else
            TTSIM_VERIFY(noc_at_len_be, UnimplementedFunctionality, "inline write: noc_at_len_be=0x%x", noc_at_len_be);
            uint32_t shift = __builtin_ctz(noc_at_len_be);
            uint32_t be = noc_at_len_be >> shift;
            // Only support aligned 4B writes for now
            TTSIM_VERIFY(!(shift & 3) && (be == 15), UnimplementedFunctionality, "inline write: noc_at_len_be=0x%x", noc_at_len_be);
            TTSIM_VERIFY((src_addr & 15) == (shift & 15), UnimplementedFunctionality, "inline write: src_addr=0x%llx shift=%d", src_addr, shift);
            if (src_tile_type == 'T') {
                TTSIM_VERIFY(src_addr + 4 <= TENSIX_SRAM_SIZE, UnsupportedFunctionality, "inline write: src_addr=0x%llx", src_addr);
            } else {
                TTSIM_VERIFY(src_addr + 4 <= ETH_SRAM_SIZE, UnsupportedFunctionality, "inline write: src_addr=0x%llx", src_addr);
            }
#endif
        }
        uint32_t noc_at_data = p_tile->noc_at_data[noc_instance][cmd_buf];
        tile_wr_bytes(src_coordinate, src_addr, &noc_at_data, sizeof(noc_at_data));
        if (bits<4,4>(noc_ctrl)) { // nonposted
            p_tile->niu_mst_wr_ack_received[noc_instance]++;
            p_tile->niu_mst_nonposted_wr_req_sent[noc_instance]++;
        } else {
            p_tile->niu_mst_posted_wr_req_sent[noc_instance]++;
        }
    } else if (bits<1,1>(noc_ctrl)) { // write -- either L1 -> L1 or L1 -> DRAM, alignment is the same for both cases
        TTSIM_VERIFY((noc_at_len_be >= 1) && (noc_at_len_be <= NOC_MAX_PACKET_SIZE), UnsupportedFunctionality, "write: noc_at_len_be=%d", noc_at_len_be);
        TTSIM_VERIFY(src_tile_type == tile_type, UnimplementedFunctionality, "write: src_tile_type=%c", src_tile_type);
        TTSIM_VERIFY(src_tile_id == tile_id, UnimplementedFunctionality, "write: src_tile_id=0x%x", src_tile_id); // write must be from current tile
        TTSIM_VERIFY(src_addr + noc_at_len_be <= sizeof(p_tile->sram), UnsupportedFunctionality, "write: src_addr=0x%llx", src_addr);
        TTSIM_VERIFY((dst_tile_type == 'D') || (dst_tile_type == 'T') || (dst_tile_type == 'E') || (dst_tile_type == 'P') || (dst_tile_type == 'A'),
            UnimplementedFunctionality, "write: dst_tile_type=%c", dst_tile_type);
        TTSIM_VERIFY((src_addr & 15) == (dst_addr & 15), UndefinedBehavior, "write: alignment of src_addr=0x%llx and dst_addr=0x%llx does not match",
            src_addr, dst_addr);
        tile_wr_bytes(dst_coordinate, dst_addr, &p_tile->sram[src_addr], noc_at_len_be);
        if (bits<4,4>(noc_ctrl)) { // nonposted
            p_tile->niu_mst_wr_ack_received[noc_instance]++;
            p_tile->niu_mst_nonposted_wr_req_sent[noc_instance]++;
        } else {
            p_tile->niu_mst_posted_wr_req_sent[noc_instance]++;
        }
    } else { // read
        TTSIM_VERIFY((noc_at_len_be >= 1) && (noc_at_len_be <= NOC_MAX_PACKET_SIZE), UnsupportedFunctionality, "read: noc_at_len_be=%d", noc_at_len_be);
        TTSIM_VERIFY(dst_tile_type == tile_type, UnimplementedFunctionality, "read: dst_tile_type=%c", dst_tile_type);
        TTSIM_VERIFY(dst_tile_id == tile_id, UnimplementedFunctionality, "read: dst_tile_id=0x%x", dst_tile_id); // read must be to current tile
        TTSIM_VERIFY(dst_addr + noc_at_len_be <= sizeof(p_tile->sram), UnsupportedFunctionality, "read: dst_addr=0x%llx", dst_addr);
        TTSIM_VERIFY((src_tile_type == 'D') || (src_tile_type == 'T') || (src_tile_type == 'E') || (src_tile_type == 'P') || (src_tile_type == 'A'),
            UnimplementedFunctionality, "read: src_tile_type=%c", src_tile_type);
        if (src_tile_type == 'D') { // DRAM -> L1
#if TT_ARCH_VERSION == 1
            TTSIM_VERIFY((src_addr & 63) == (dst_addr & 63), UndefinedBehavior, "read: alignment of src_addr=0x%llx and dst_addr=0x%llx does not match",
                src_addr, dst_addr);
#else
            TTSIM_VERIFY((src_addr & 31) == (dst_addr & 31), UndefinedBehavior, "read: alignment of src_addr=0x%llx and dst_addr=0x%llx does not match",
                src_addr, dst_addr);
#endif
        } else if ((src_addr >= RISCV_LOCAL_MEM_BASE) && (src_tile_type != 'P')) { // MMIO -> L1
            TTSIM_VERIFY((noc_at_len_be <= (4 - (dst_addr & 3))), UnimplementedFunctionality, "read: noc_at_len_be=%d", noc_at_len_be);
            TTSIM_VERIFY((src_addr & 3) == (dst_addr & 3), UndefinedBehavior, "read: misaligned src_addr=0x%llx or dst_addr=0x%llx", src_addr, dst_addr);
        } else if (src_tile_type != 'P') { // L1 -> L1
            TTSIM_VERIFY((src_addr & 15) == (dst_addr & 15), UndefinedBehavior, "read: alignment of src_addr=0x%llx and dst_addr=0x%llx does not match",
                src_addr, dst_addr);
        }
        tile_rd_bytes(src_coordinate, src_addr, &p_tile->sram[dst_addr], noc_at_len_be);
        p_tile->niu_mst_rd_resp_received[noc_instance]++;
    }
}

template<char tile_type>
static void noc_regs_wr32(uint32_t tile_id, uint32_t noc_instance, uint32_t offset, uint32_t data) {
    auto *p_tile = get_tile<tile_type>(tile_id);
#if TT_ARCH_VERSION == 0
    uint32_t cmd_buf = offset / 0x400;
#else
    uint32_t cmd_buf = offset / 0x800;
#endif
    switch (offset) {
        case NOC_REGS_NOC_TARG_ADDR_LO(0):
        case NOC_REGS_NOC_TARG_ADDR_LO(1):
        case NOC_REGS_NOC_TARG_ADDR_LO(2):
        case NOC_REGS_NOC_TARG_ADDR_LO(3):
            p_tile->noc_targ_addr_lo[noc_instance][cmd_buf] = data;
            break;
        case NOC_REGS_NOC_TARG_ADDR_MID(0):
        case NOC_REGS_NOC_TARG_ADDR_MID(1):
        case NOC_REGS_NOC_TARG_ADDR_MID(2):
        case NOC_REGS_NOC_TARG_ADDR_MID(3):
            p_tile->noc_targ_addr_mid[noc_instance][cmd_buf] = data;
            break;
#if TT_ARCH_VERSION >= 1
        case NOC_REGS_NOC_TARG_ADDR_HI(0):
        case NOC_REGS_NOC_TARG_ADDR_HI(1):
        case NOC_REGS_NOC_TARG_ADDR_HI(2):
        case NOC_REGS_NOC_TARG_ADDR_HI(3):
            p_tile->noc_targ_addr_hi[noc_instance][cmd_buf] = data;
            break;
#endif
        case NOC_REGS_NOC_RET_ADDR_LO(0):
        case NOC_REGS_NOC_RET_ADDR_LO(1):
        case NOC_REGS_NOC_RET_ADDR_LO(2):
        case NOC_REGS_NOC_RET_ADDR_LO(3):
            p_tile->noc_ret_addr_lo[noc_instance][cmd_buf] = data;
            break;
        case NOC_REGS_NOC_RET_ADDR_MID(0):
        case NOC_REGS_NOC_RET_ADDR_MID(1):
        case NOC_REGS_NOC_RET_ADDR_MID(2):
        case NOC_REGS_NOC_RET_ADDR_MID(3):
            p_tile->noc_ret_addr_mid[noc_instance][cmd_buf] = data;
            break;
#if TT_ARCH_VERSION >= 1
        case NOC_REGS_NOC_RET_ADDR_HI(0):
        case NOC_REGS_NOC_RET_ADDR_HI(1):
        case NOC_REGS_NOC_RET_ADDR_HI(2):
        case NOC_REGS_NOC_RET_ADDR_HI(3):
            p_tile->noc_ret_addr_hi[noc_instance][cmd_buf] = data;
            break;
#endif
        case NOC_REGS_NOC_PACKET_TAG(0):
        case NOC_REGS_NOC_PACKET_TAG(1):
        case NOC_REGS_NOC_PACKET_TAG(2):
        case NOC_REGS_NOC_PACKET_TAG(3):
            // Only support having packet id set in the packet tag
            TTSIM_VERIFY(!(data & ~(0xF << 10)), UnimplementedFunctionality, "noc_packet_tag=0x%x", data);
            p_tile->noc_packet_tag[noc_instance][cmd_buf] = data;
            break;
        case NOC_REGS_NOC_CTRL(0):
        case NOC_REGS_NOC_CTRL(1):
        case NOC_REGS_NOC_CTRL(2):
        case NOC_REGS_NOC_CTRL(3):
            p_tile->noc_ctrl[noc_instance][cmd_buf] = data;
            break;
        case NOC_REGS_NOC_AT_LEN_BE(0):
        case NOC_REGS_NOC_AT_LEN_BE(1):
        case NOC_REGS_NOC_AT_LEN_BE(2):
        case NOC_REGS_NOC_AT_LEN_BE(3):
            p_tile->noc_at_len_be[noc_instance][cmd_buf] = data;
            break;
        case NOC_REGS_NOC_AT_DATA(0):
        case NOC_REGS_NOC_AT_DATA(1):
        case NOC_REGS_NOC_AT_DATA(2):
        case NOC_REGS_NOC_AT_DATA(3):
            p_tile->noc_at_data[noc_instance][cmd_buf] = data;
            break;
#if TT_ARCH_VERSION == 1
        case NOC_REGS_NOC_BRCST_EXCLUDE(0):
        case NOC_REGS_NOC_BRCST_EXCLUDE(1):
        case NOC_REGS_NOC_BRCST_EXCLUDE(2):
        case NOC_REGS_NOC_BRCST_EXCLUDE(3):
            TTSIM_VERIFY(!data, UnimplementedFunctionality, "noc_brcst_exclude=0x%x", data);
            break;
#endif
        case NOC_REGS_NOC_CMD_CTRL(0):
        case NOC_REGS_NOC_CMD_CTRL(1):
        case NOC_REGS_NOC_CMD_CTRL(2):
        case NOC_REGS_NOC_CMD_CTRL(3):
            TTSIM_VERIFY(data == 1, UnimplementedFunctionality, "noc_cmd_ctrl[%d] data=0x%x", cmd_buf, data);
            noc_cmd_ctrl<tile_type>(tile_id, noc_instance, cmd_buf);
            break;
        case NOC_REGS_NOC_CLEAR_OUTSTANDING_REQ_CNT:
            // Only support clearing all the transaction ids
            TTSIM_VERIFY(data, UnsupportedFunctionality, "no-op mask: noc_clear_outstanding_req_cnt=0x%x", data);
            TTSIM_VERIFY(data == ((1 << NUM_NOC_TRANSACTION_IDS) - 1), UnimplementedFunctionality, "noc_clear_outstanding_req_cnt=0x%x", data);
            for (uint32_t i = 0; i < NUM_NOC_TRANSACTION_IDS; i++) {
                p_tile->niu_mst_reqs_outstanding[noc_instance][i] = 0;
            }
            break;
        case NOC_REGS_NIU_CFG_0: p_tile->base.niu_cfg_0[noc_instance] = data; break;
#if TT_ARCH_VERSION == 0
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_0:
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_1:
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_2:
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_3:
            p_tile->base.noc_translation_x[noc_instance][(offset - NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_0) / 4] = data;
            break;
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_0:
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_1:
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_2:
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_3:
            p_tile->base.noc_translation_y[noc_instance][(offset - NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_0) / 4] = data;
            break;
        case NOC_REGS_NOC_ID_LOGICAL: p_tile->base.noc_id_logical[noc_instance] = data; break;
#endif
        case NOC_REGS_ROUTER_CFG_0: p_tile->base.router_cfg_0[noc_instance] = data; break;
        case NOC_REGS_ROUTER_CFG_1:
            TTSIM_VERIFY((data & NONTENSIX_COL_MASK) == NONTENSIX_COL_MASK, UndefinedBehavior, "router_cfg_1=0x%x", data); // cannot clear non-tensix cols
            p_tile->base.router_cfg_1[noc_instance] = data;
            break;
        case NOC_REGS_ROUTER_CFG_2: p_tile->base.router_cfg_2[noc_instance] = data; break;
        case NOC_REGS_ROUTER_CFG_3:
            TTSIM_VERIFY((data & NONTENSIX_ROW_MASK) == NONTENSIX_ROW_MASK, UndefinedBehavior, "router_cfg_3=0x%x", data); // cannot clear non-tensix rows
            p_tile->base.router_cfg_3[noc_instance] = data;
            break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

template<char tile_type>
static uint32_t noc_overlay_rd32(uint32_t tile_id, uint32_t offset) {
    auto *p_tile = get_tile<tile_type>(tile_id);
    uint32_t stream = offset / 0x1000;
    TTSIM_VERIFY(stream < std::size(p_tile->overlay_stream_remote_src), UndefinedBehavior, "stream=%d", stream);
    offset &= 0xFFF; // offset within stream
    switch (offset) {
        case NOC_OVERLAY_STREAM_REMOTE_SRC: return p_tile->overlay_stream_remote_src[stream];
        case NOC_OVERLAY_STREAM_REMOTE_DEST_BUF_START: return p_tile->overlay_stream_remote_dest_buf_start[stream];
        case NOC_OVERLAY_STREAM_REMOTE_DEST_BUF_SIZE: return p_tile->overlay_stream_remote_dest_buf_size[stream];
        case NOC_OVERLAY_STREAM_REMOTE_DEST_BUF_SPACE_AVAILABLE: return p_tile->overlay_stream_remote_dest_buf_space_available[stream];
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

template<char tile_type>
static void noc_overlay_wr32(uint32_t tile_id, uint32_t offset, uint32_t data) {
    auto *p_tile = get_tile<tile_type>(tile_id);
    uint32_t stream = offset / 0x1000;
    TTSIM_VERIFY(stream < std::size(p_tile->overlay_stream_remote_src), UndefinedBehavior, "stream=%d", stream);
    offset &= 0xFFF; // offset within stream
    switch (offset) {
        case NOC_OVERLAY_STREAM_REMOTE_SRC:
            TTSIM_VERIFY(data <= 0xFFFFFF, UnsupportedFunctionality, "stream_remote_src: data=0x%x", data);
            p_tile->overlay_stream_remote_src[stream] = data;
            break;
        case NOC_OVERLAY_STREAM_REMOTE_DEST_BUF_START:
            p_tile->overlay_stream_remote_dest_buf_start[stream] = data & 0x1FFFF;
            break;
        case NOC_OVERLAY_STREAM_REMOTE_DEST_BUF_SIZE:
            p_tile->overlay_stream_remote_dest_buf_size[stream] = data & 0x1FFFF;
            p_tile->overlay_stream_remote_dest_buf_space_available[stream] = data & 0x1FFFF;
            break;
        case NOC_OVERLAY_STREAM_REMOTE_DEST_BUF_SPACE_AVAILABLE_UPDATE:
            TTSIM_VERIFY(!(data & 0x3F), UnsupportedFunctionality, "stream_remote_dest_buf_space_available_update: data=0x%x", data);
            data = (p_tile->overlay_stream_remote_dest_buf_space_available[stream] + (data >> 6)) & 0x1FFFF;
            p_tile->overlay_stream_remote_dest_buf_space_available[stream] = data;
            break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static inline std::pair<uint32_t, uint32_t> trisc_pipe(uint32_t riscv_id) {
    static_assert(RV32_ID_TRISC1 == RV32_ID_TRISC0 + 1);
    static_assert(RV32_ID_TRISC2 == RV32_ID_TRISC0 + 2);
    TTSIM_VERIFY((riscv_id >= RV32_ID_TRISC0) && (riscv_id <= RV32_ID_TRISC2), UnimplementedFunctionality, "riscv_id=%d", riscv_id);
    return {0, riscv_id - RV32_ID_TRISC0};
}

static void tensix_mop_cfg_wr32(uint32_t tile_id, uint32_t riscv_id, uint32_t offset, uint32_t data) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
    auto [tensix_id, pipe] = trisc_pipe(riscv_id);
    TTSIM_VERIFY(!(offset & 3), AssertionFailure, "misaligned offset=0x%x", offset);
    switch (offset / 4) {
        case 0x0 / 4 ... 0x20 / 4:
            p_tile->tensix[tensix_id].mop_cfg[pipe][offset / 4] = data;
            break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static uint32_t tensix_regfile_rd32(uint32_t tile_id, uint32_t riscv_id, uint32_t offset) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
    auto [tensix_id, pipe] = trisc_pipe(riscv_id);
    TTSIM_VERIFY(!(offset & 3), AssertionFailure, "misaligned offset=0x%x", offset);
    switch (offset / 4) {
        case 0x0 / 4 ... 0xFC / 4:
            return p_tile->tensix[tensix_id].dma_regs[pipe][offset / 4];
        default:
            TTSIM_ERROR(UndefinedBehavior, "offset=0x%x", offset);
    }
}

static void tensix_regfile_wr32(uint32_t tile_id, uint32_t riscv_id, uint32_t offset, uint32_t data) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
    auto [tensix_id, pipe] = trisc_pipe(riscv_id);
    TTSIM_VERIFY(!(offset & 3), AssertionFailure, "misaligned offset=0x%x", offset);
    switch (offset / 4) {
        case 0x0 / 4 ... 0xFC / 4:
            p_tile->tensix[tensix_id].dma_regs[pipe][offset / 4] = data;
            break;
        default:
            TTSIM_ERROR(UndefinedBehavior, "offset=0x%x", offset);
    }
}

// XXX we do not call tensix_can_push_inst in this path, so this can currently overflow the FIFO
static void tensix_inst_wr32(uint32_t tile_id, uint32_t riscv_id, uint32_t pipe_aperture, uint32_t offset, uint32_t data) {
    TTSIM_VERIFY(!offset, UnimplementedFunctionality, "offset=0x%x", offset);
    TensixTile *p_tile = &g_t_tiles[tile_id];
    if (riscv_id == RV32_ID_BRISC) {
        return tensix_push_inst(&p_tile->tensix[0], pipe_aperture, data, true); // bypasses MOP expander
    }
    TTSIM_VERIFY(!pipe_aperture, UnsupportedFunctionality, "TENSIX_INST%d aperture only mapped for brisc", pipe_aperture);
    auto [tensix_id, pipe] = trisc_pipe(riscv_id);
    tensix_push_inst(&p_tile->tensix[tensix_id], pipe, data, false);
}

static std::pair<bool, uint32_t> tensix_pc_buf_rd32(uint32_t tile_id, uint32_t riscv_id, uint32_t offset) {
    auto [tensix_id, pipe] = trisc_pipe(riscv_id);
    TensixTile *p_tile = &g_t_tiles[tile_id];
    TTSIM_VERIFY(!(offset & 3), AssertionFailure, "misaligned offset=0x%x", offset);
    switch (offset / 4) {
        case TENSIX_PC_BUF_TENSIX_SYNC / 4:
            if (p_tile->tensix[tensix_id].inst_rd_ptr[pipe] != p_tile->tensix[tensix_id].inst_wr_ptr[pipe]) {
                return {false, 0}; // stall until previously issued Tensix instructions from this thread have finished
            }
            return {true, 0};
        case TENSIX_PC_BUF_MOP_SYNC / 4: return {true, 0};
        case TENSIX_PC_BUF_SEMAPHORE(0) / 4 ... TENSIX_PC_BUF_SEMAPHORE(7) / 4: {
            uint32_t sem_index = (offset - TENSIX_PC_BUF_SEMAPHORE(0)) / 4;
            TTSIM_ASSERT(sem_index < std::size(p_tile->tensix[tensix_id].sem));
            return {true, p_tile->tensix[tensix_id].sem[sem_index]};
        }
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static void tensix_pc_buf_wr32(uint32_t tile_id, uint32_t riscv_id, uint32_t offset, uint32_t data) {
    auto [tensix_id, pipe] = trisc_pipe(riscv_id);
    TensixTile *p_tile = &g_t_tiles[tile_id];
    TTSIM_VERIFY(!(offset & 3), AssertionFailure, "misaligned offset=0x%x", offset);
    switch (offset / 4) {
        case TENSIX_PC_BUF_TENSIX_SYNC / 4: break;
        case TENSIX_PC_BUF_MOP_SYNC / 4: break;
        case TENSIX_PC_BUF_SEMAPHORE(0) / 4 ... TENSIX_PC_BUF_SEMAPHORE(7) / 4: {
            uint32_t sem_index = (offset - TENSIX_PC_BUF_SEMAPHORE(0)) / 4;
            TTSIM_ASSERT(sem_index < std::size(p_tile->tensix[tensix_id].sem));
            if (data == 1) {
                TTSIM_VERIFY(p_tile->tensix[tensix_id].sem[sem_index], NonContractualBehavior, "sem%d underflow", sem_index);
                p_tile->tensix[tensix_id].sem[sem_index]--;
            } else {
                TTSIM_VERIFY(!data, UnimplementedFunctionality, "sem%d: data=0x%x", sem_index, data);
                TTSIM_VERIFY(p_tile->tensix[tensix_id].sem[sem_index] < 15, NonContractualBehavior, "sem%d overflow", sem_index);
                p_tile->tensix[tensix_id].sem[sem_index]++;
            }
            break;
        }
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

// XXX we only allow a mailbox to have 1 value, when in reality it can have up to 4 values (though 4 slots shared across multiple mailboxes)
// XXX we don't implement the query whether the mailbox has data in it
static std::pair<bool, uint32_t> tensix_mailbox_rd32(uint32_t tile_id, uint32_t riscv_id, uint32_t instance, uint32_t offset) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
    TTSIM_VERIFY(!offset, UnimplementedFunctionality, "offset=0x%x", offset);
    TTSIM_VERIFY(riscv_id < 4, UndefinedBehavior, "riscv_id=%d", riscv_id); // note that NCRISC cannot use the mailboxes
    if (!p_tile->mailbox_has_data[instance][riscv_id]) {
        return {false, 0};
    }
    p_tile->mailbox_has_data[instance][riscv_id] = false;
    return {true, p_tile->mailbox_data[instance][riscv_id]};
}

static bool tensix_mailbox_wr32(uint32_t tile_id, uint32_t riscv_id, uint32_t instance, uint32_t offset, uint32_t data) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
    TTSIM_VERIFY(!offset, UnimplementedFunctionality, "offset=0x%x", offset);
    TTSIM_VERIFY(riscv_id < 4, UndefinedBehavior, "riscv_id=%d", riscv_id); // note that NCRISC cannot use the mailboxes
    if (p_tile->mailbox_has_data[riscv_id][instance]) {
        return false;
    }
    p_tile->mailbox_has_data[riscv_id][instance] = true;
    p_tile->mailbox_data[riscv_id][instance] = data;
    return true;
}

static uint32_t eth_txq_regs_rd32(uint32_t tile_id, uint32_t offset) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    uint32_t queue_id = offset / 0x1000;
    offset &= 0xFFF;
    TTSIM_VERIFY(queue_id < ETH_NUM_TX_RX_QUEUES, UndefinedBehavior, "queue_id=%d", queue_id);
    switch (offset) {
        case ETH_TXQ_CONTROL: return p_tile->eth_txq_control[queue_id];
        case ETH_TXQ_CMD: return p_tile->eth_txq_cmd[queue_id];
#if TT_ARCH_VERSION == 1
        case ETH_TXQ_STATUS: return p_tile->eth_txq_status[queue_id];
#endif
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static void eth_txq_regs_wr32(uint32_t tile_id, uint32_t offset, uint32_t data) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    uint32_t queue_id = offset / 0x1000;
    offset &= 0xFFF;
    TTSIM_VERIFY(queue_id < ETH_NUM_TX_RX_QUEUES, UndefinedBehavior, "queue_id=%d", queue_id);
    switch (offset) {
#if TT_ARCH_VERSION == 1
        case ETH_TXQ_CONTROL:
            TTSIM_VERIFY(data == 1, UnimplementedFunctionality, "eth_txq_control=0x%x", data);
            p_tile->eth_txq_control[queue_id] = data;
            break;
#endif
        case ETH_TXQ_CMD:
#if TT_ARCH_VERSION == 0
            TTSIM_VERIFY(!p_tile->eth_txq_cmd[queue_id], UnimplementedFunctionality, "eth_txq_cmd=0x%x", p_tile->eth_txq_cmd[queue_id]);
            TTSIM_VERIFY(p_tile->erisc_mac_0 == 0x30000001, UnimplementedFunctionality, "ERISC_MAC_0=0x%x", p_tile->erisc_mac_0);
            TTSIM_VERIFY(p_tile->erisc_mac_1 == 7, UnimplementedFunctionality, "ERISC_MAC_1=0x%x", p_tile->erisc_mac_1);
#else
            TTSIM_VERIFY(!p_tile->eth_txq_status[queue_id], UnimplementedFunctionality, "eth_txq_status=0x%x", p_tile->eth_txq_status[queue_id]);
#endif
            if (data == 2) {
                uint32_t start_addr = p_tile->eth_txq_transfer_start_addr[queue_id];
                uint32_t size_bytes = p_tile->eth_txq_transfer_size_bytes[queue_id];
                uint32_t dest_addr = p_tile->eth_txq_dest_addr[queue_id];
                TTSIM_VERIFY(!(start_addr & 15), UnimplementedFunctionality, "eth_txq_transfer_start_addr=0x%x", start_addr);
                TTSIM_VERIFY(!(size_bytes & 15), UnimplementedFunctionality, "eth_txq_transfer_size_bytes=0x%x", size_bytes);
                TTSIM_VERIFY(!(dest_addr & 15), UnimplementedFunctionality, "eth_txq_dest_addr=0x%x", dest_addr);
                uint32_t remote_chip_id = 0;
                uint32_t remote_tile_id = 0;
                TTSIM_VERIFY(eth_peer(tile_id, &remote_chip_id, &remote_tile_id), UnimplementedFunctionality,
                    "eth_txq_cmd=0x%x tile_id=%d", data, tile_id);
                TTSIM_VERIFY(start_addr + size_bytes <= ETH_SRAM_SIZE, UndefinedBehavior,
                    "eth_txq_transfer_start_addr=0x%x eth_txq_transfer_size_bytes=0x%x", start_addr, size_bytes);
                TTSIM_VERIFY(dest_addr + size_bytes <= ETH_SRAM_SIZE, UndefinedBehavior,
                    "eth_txq_dest_addr=0x%x eth_txq_transfer_size_bytes=0x%x", dest_addr, size_bytes);
                // eth_send_packet (ETH_TXQ_CMD_START_DATA): copy local L1 to the peer eth tile's L1
                uint32_t saved_chip_id = g_current_chip_id;
                ttsim_select_chip(remote_chip_id);
                memcpy(&g_e_tiles[remote_tile_id].sram[dest_addr], &p_tile->sram[start_addr], size_bytes);
                ttsim_select_chip(saved_chip_id);
                p_tile->eth_txq_cmd[queue_id] = 0;
            } else if (data == 4) {
#if TT_ARCH_VERSION == 0
                TTSIM_VERIFY(queue_id != 1, UnimplementedFunctionality, "queue_id=%d", queue_id);
#else
                TTSIM_VERIFY(queue_id != 2, UnimplementedFunctionality, "queue_id=%d", queue_id);
#endif
                TTSIM_VERIFY(!(p_tile->eth_txq_dest_addr[queue_id] & 3), UnimplementedFunctionality, "eth_txq_dest_addr=0x%x", p_tile->eth_txq_dest_addr[queue_id]);
                uint32_t remote_chip_id = 0;
                uint32_t remote_tile_id = 0;
                TTSIM_VERIFY(eth_peer(tile_id, &remote_chip_id, &remote_tile_id), UnimplementedFunctionality,
                    "eth_txq_cmd=0x%x tile_id=%d", data, tile_id);
                // eth_write_remote_reg (ETH_TXQ_CMD_START_REG): 32-bit MMIO write to the peer eth tile
                uint32_t saved_chip_id = g_current_chip_id;
                uint32_t dest_addr = p_tile->eth_txq_dest_addr[queue_id];
                uint32_t remote_reg_data = p_tile->eth_txq_remote_reg_data[queue_id];
                ttsim_select_chip(remote_chip_id);
                uint32_t coord = tile_to_coord('E', remote_tile_id);
                tile_wr_bytes(coord, dest_addr, &remote_reg_data, sizeof(remote_reg_data));
                ttsim_select_chip(saved_chip_id);
                p_tile->eth_txq_cmd[queue_id] = 0;
            } else {
                TTSIM_ERROR(UnimplementedFunctionality, "eth_txq_cmd=0x%x", data);
            }
            break;
        case ETH_TXQ_TRANSFER_START_ADDR:
            p_tile->eth_txq_transfer_start_addr[queue_id] = data;
            break;
        case ETH_TXQ_TRANSFER_SIZE_BYTES:
            p_tile->eth_txq_transfer_size_bytes[queue_id] = data;
            break;
        case ETH_TXQ_DEST_ADDR:
            p_tile->eth_txq_dest_addr[queue_id] = data;
            break;
        case ETH_TXQ_REMOTE_REG_DATA:
            p_tile->eth_txq_remote_reg_data[queue_id] = data;
            break;
        case ETH_TXQ_DATA_PACKET_ACCEPT_AHEAD:
            break;
#if TT_ARCH_VERSION == 1
        case ETH_TXQ_TXPKT_CFG_SEL_SW:
            TTSIM_VERIFY((data & 15) < ETH_NUM_TX_HEADER_TABLE_ENTRIES, UnsupportedFunctionality, "eth_txq_txpkt_cfg_sel_sw=0x%x", data);
            TTSIM_VERIFY(((data >> 4) & 15) < ETH_NUM_TX_HEADER_TABLE_ENTRIES, UnsupportedFunctionality, "eth_txq_txpkt_cfg_sel_sw=0x%x", data);
            TTSIM_VERIFY((data >> 8) < ETH_NUM_TX_HEADER_TABLE_ENTRIES, UnsupportedFunctionality, "eth_txq_txpkt_cfg_sel_sw=0x%x", data);
            p_tile->eth_txq_txpkt_cfg_sel_sw[queue_id] = data;
            break;
        case ETH_TXQ_TXPKT_CFG_SEL_HW:
            TTSIM_VERIFY(data < ETH_NUM_TX_HEADER_TABLE_ENTRIES, UnsupportedFunctionality, "eth_txq_txpkt_cfg_sel_hw=0x%x", data);
            p_tile->eth_txq_txpkt_cfg_sel_hw[queue_id] = data;
            break;
#endif
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

#if TT_ARCH_VERSION == 1
static void eth_txpkt_cfg_wr32(uint32_t tile_id, uint32_t offset, uint32_t data) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    uint32_t header_entry = offset / 0x80;
    offset &= 0x7F;
    TTSIM_VERIFY(header_entry < ETH_NUM_TX_HEADER_TABLE_ENTRIES, UndefinedBehavior, "header_entry=%d", header_entry);
    switch (offset) {
        case ETH_TXPKT_CFG_MAC_DA_LO:
            p_tile->eth_txpkt_cfg_mac_da_lo[header_entry] = data;
            break;
        case ETH_TXPKT_CFG_MAC_DA_HI:
            TTSIM_VERIFY(data <= 0xFFFF, UnsupportedFunctionality, "eth_txpkt_cfg_mac_da_hi=0x%x", data);
            p_tile->eth_txpkt_cfg_mac_da_hi[header_entry] = data;
            break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}
#endif

static uint32_t eth_rxq_regs_rd32(uint32_t tile_id, uint32_t offset) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    uint32_t queue_id = offset / 0x1000;
    offset &= 0xFFF;
    TTSIM_VERIFY(queue_id < ETH_NUM_TX_RX_QUEUES, UndefinedBehavior, "queue_id=%d", queue_id);
    switch (offset) {
        case ETH_RXQ_CONTROL: return p_tile->eth_rxq_control[queue_id];
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static void eth_rxq_regs_wr32(uint32_t tile_id, uint32_t offset, uint32_t data) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    uint32_t queue_id = offset / 0x1000;
    offset &= 0xFFF;
    TTSIM_VERIFY(queue_id < ETH_NUM_TX_RX_QUEUES, UndefinedBehavior, "queue_id=%d", queue_id);
    switch (offset) {
        case ETH_RXQ_CONTROL:
            TTSIM_VERIFY(data == 2, UnimplementedFunctionality, "eth_rxq_control=0x%x", data);
            p_tile->eth_rxq_control[queue_id] = data;
            break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static uint32_t eth_ctrl_regs_rd32(uint32_t tile_id, uint32_t offset) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case ETH_CTRL_REGS_ERISC_IRAM_LOAD: return p_tile->erisc_iram_load;
        case ETH_CTRL_REGS_IERISC_RESET_PC: return p_tile->ierisc_reset_pc;
#elif TT_ARCH_VERSION == 1
        case ETH_CTRL_REGS_MAC_RX_ADDR_ROUTING: return p_tile->eth_mac_rx_addr_routing;
        case ETH_CTRL_REGS_PCS_STATUS: return 1; // link is always trained/up
#endif
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static void eth_ctrl_regs_wr32(uint32_t tile_id, uint32_t offset, uint32_t data) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case ETH_CTRL_REGS_ERISC_IRAM_LOAD:
            TTSIM_VERIFY(p_tile->erisc_mac_0 == 0x30000000, UnimplementedFunctionality, "ERISC_MAC_0=0x%x", p_tile->erisc_mac_0);
            TTSIM_VERIFY(p_tile->erisc_mac_1 == 6, UnimplementedFunctionality, "ERISC_MAC_1=0x%x", p_tile->erisc_mac_1);
            p_tile->erisc_iram_load = data; // Last bit gets set, then unset after copy is complete
            TTSIM_VERIFY(data*16 + RV32_IRAM_SIZE <= sizeof(p_tile->sram), UnsupportedFunctionality, "invalid data=0x%x", data);
            memcpy(p_tile->erisc_iram, &p_tile->sram[data * 16], RV32_IRAM_SIZE);
            break;
        case ETH_CTRL_REGS_IERISC_RESET_PC: p_tile->ierisc_reset_pc = data; break;
#endif
#if TT_ARCH_VERSION == 1
        case ETH_CTRL_REGS_MAC_RX_ROUTING:
            TTSIM_VERIFY(!data, UnimplementedFunctionality, "mac_rx_routing=0x%x", data);
            p_tile->eth_mac_rx_routing = data;
            break;
        case ETH_CTRL_REGS_MAC_RX_ADDR_ROUTING:
            p_tile->eth_mac_rx_addr_routing = data;
            break;
#endif
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

#if TT_ARCH_VERSION == 0
static void eth_mac_regs_wr32(uint32_t tile_id, uint32_t offset, uint32_t data) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    switch (offset) {
        case ETH_MAC_REGS_ERISC_MAC_0:
            TTSIM_VERIFY((data == 0x30000000) || (data == 0x30000001), UnimplementedFunctionality, "ERISC_MAC_0=0x%x", data);
            p_tile->erisc_mac_0 = data;
            break;
        case ETH_MAC_REGS_ERISC_MAC_1:
            TTSIM_VERIFY((data == 6) || (data == 7), UnimplementedFunctionality, "ERISC_MAC_1=0x%x", data);
            p_tile->erisc_mac_1 = data;
            break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}
#endif

static std::pair<bool, uint32_t> t_tile_mmio_rd32(uint32_t tile_id, uint32_t riscv_id, uint64_t addr) {
    switch (addr) {
        case RISCV_TDMA_REGS_BASE ... RISCV_TDMA_REGS_LIMIT:
            return {true, riscv_tdma_regs_rd32(tile_id, addr - RISCV_TDMA_REGS_BASE)};
        case RISCV_DEBUG_REGS_BASE ... RISCV_DEBUG_REGS_LIMIT:
            return {true, riscv_debug_regs_rd32<'T'>(tile_id, 0, addr - RISCV_DEBUG_REGS_BASE)};
        case NOC0_REGS_BASE ... NOC0_REGS_LIMIT:
            return {true, noc_regs_rd32<'T'>(tile_id, 0, addr - NOC0_REGS_BASE)};
#if NUM_NOCS > 1
        case NOC1_REGS_BASE ... NOC1_REGS_LIMIT:
            return {true, noc_regs_rd32<'T'>(tile_id, 1, addr - NOC1_REGS_BASE)};
#endif
        case NOC_OVERLAY_BASE ... NOC_OVERLAY_BASE + 0x3FFFF:
            return {true, noc_overlay_rd32<'T'>(tile_id, addr - NOC_OVERLAY_BASE)};
#if TT_ARCH_VERSION == 1
        case TENSIX_DST_BASE ... TENSIX_DST_LIMIT:
            TTSIM_ERROR(UnimplementedFunctionality, "tensix_dst: offset=0x%x", uint32_t(addr - TENSIX_DST_BASE));
#endif
        case TENSIX_REGFILE_BASE ... TENSIX_REGFILE_LIMIT:
            return {true, tensix_regfile_rd32(tile_id, riscv_id, addr - TENSIX_REGFILE_BASE)};
        case TENSIX_PC_BUF_BASE ... TENSIX_PC_BUF_LIMIT:
            return tensix_pc_buf_rd32(tile_id, riscv_id, addr - TENSIX_PC_BUF_BASE);
        case TENSIX_MAILBOX0_BASE ... TENSIX_MAILBOX0_LIMIT:
            return tensix_mailbox_rd32(tile_id, riscv_id, 0, addr - TENSIX_MAILBOX0_BASE);
        case TENSIX_MAILBOX1_BASE ... TENSIX_MAILBOX1_LIMIT:
            return tensix_mailbox_rd32(tile_id, riscv_id, 1, addr - TENSIX_MAILBOX1_BASE);
        case TENSIX_MAILBOX2_BASE ... TENSIX_MAILBOX2_LIMIT:
            return tensix_mailbox_rd32(tile_id, riscv_id, 2, addr - TENSIX_MAILBOX2_BASE);
        case TENSIX_MAILBOX3_BASE ... TENSIX_MAILBOX3_LIMIT:
            return tensix_mailbox_rd32(tile_id, riscv_id, 3, addr - TENSIX_MAILBOX3_BASE);
        case TENSIX_CFG_BASE ... TENSIX_CFG_LIMIT:
            TTSIM_VERIFY(riscv_id < 4, UnimplementedFunctionality, "riscv_id=%d", riscv_id); // note that this intentionally excludes NCRISC on WH/BH
            return {true, tensix_cfg_rd32(&g_t_tiles[tile_id].tensix[0], 0, addr - TENSIX_CFG_BASE)};
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "addr=0x%llx", addr);
    }
}

static std::pair<bool, uint64_t> t_tile_mmio_rd64(uint32_t tile_id, uint32_t riscv_id, uint64_t addr) {
    switch (addr) {
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "addr=0x%llx", addr);
    }
}

static bool t_tile_mmio_wr32(uint32_t tile_id, uint32_t riscv_id, uint64_t addr, uint32_t data) {
    switch (addr) {
        case RISCV_TDMA_REGS_BASE ... RISCV_TDMA_REGS_LIMIT:
            riscv_tdma_regs_wr32(tile_id, addr - RISCV_TDMA_REGS_BASE, data);
            return true;
        case RISCV_DEBUG_REGS_BASE ... RISCV_DEBUG_REGS_LIMIT:
            riscv_debug_regs_wr32(tile_id, 0, addr - RISCV_DEBUG_REGS_BASE, data);
            return true;
        case NOC0_REGS_BASE ... NOC0_REGS_LIMIT:
            noc_regs_wr32<'T'>(tile_id, 0, addr - NOC0_REGS_BASE, data);
            return true;
#if NUM_NOCS > 1
        case NOC1_REGS_BASE ... NOC1_REGS_LIMIT:
            noc_regs_wr32<'T'>(tile_id, 1, addr - NOC1_REGS_BASE, data);
            return true;
#endif
        case NOC_OVERLAY_BASE ... NOC_OVERLAY_BASE + 0x3FFFF:
            noc_overlay_wr32<'T'>(tile_id, addr - NOC_OVERLAY_BASE, data);
            return true;
        case TENSIX_MOP_CFG_BASE ... TENSIX_MOP_CFG_LIMIT:
            tensix_mop_cfg_wr32(tile_id, riscv_id, addr - TENSIX_MOP_CFG_BASE, data);
            return true;
#if TT_ARCH_VERSION == 1
        case TENSIX_DST_BASE ... TENSIX_DST_LIMIT:
            TTSIM_ERROR(UnimplementedFunctionality, "tensix_dst: offset=0x%x", uint32_t(addr - TENSIX_DST_BASE));
#endif
        case TENSIX_REGFILE_BASE ... TENSIX_REGFILE_LIMIT:
            tensix_regfile_wr32(tile_id, riscv_id, addr - TENSIX_REGFILE_BASE, data);
            return true;
        case TENSIX_INST_BASE ... TENSIX_INST_LIMIT:
            tensix_inst_wr32(tile_id, riscv_id, 0, addr - TENSIX_INST_BASE, data);
            return true;
        case TENSIX_INST1_BASE ... TENSIX_INST1_LIMIT:
            tensix_inst_wr32(tile_id, riscv_id, 1, addr - TENSIX_INST1_BASE, data);
            return true;
        case TENSIX_INST2_BASE ... TENSIX_INST2_LIMIT:
            tensix_inst_wr32(tile_id, riscv_id, 2, addr - TENSIX_INST2_BASE, data);
            return true;
        case TENSIX_PC_BUF_BASE ... TENSIX_PC_BUF_LIMIT:
            tensix_pc_buf_wr32(tile_id, riscv_id, addr - TENSIX_PC_BUF_BASE, data);
            return true;
        case TENSIX_MAILBOX0_BASE ... TENSIX_MAILBOX0_LIMIT:
            return tensix_mailbox_wr32(tile_id, riscv_id, 0, addr - TENSIX_MAILBOX0_BASE, data);
        case TENSIX_MAILBOX1_BASE ... TENSIX_MAILBOX1_LIMIT:
            return tensix_mailbox_wr32(tile_id, riscv_id, 1, addr - TENSIX_MAILBOX1_BASE, data);
        case TENSIX_MAILBOX2_BASE ... TENSIX_MAILBOX2_LIMIT:
            return tensix_mailbox_wr32(tile_id, riscv_id, 2, addr - TENSIX_MAILBOX2_BASE, data);
        case TENSIX_MAILBOX3_BASE ... TENSIX_MAILBOX3_LIMIT:
            return tensix_mailbox_wr32(tile_id, riscv_id, 3, addr - TENSIX_MAILBOX3_BASE, data);
        case TENSIX_CFG_BASE ... TENSIX_CFG_LIMIT:
            TTSIM_VERIFY(riscv_id < 4, UnimplementedFunctionality, "riscv_id=%d", riscv_id); // note that this intentionally excludes NCRISC on WH/BH
            tensix_cfg_wr32(&g_t_tiles[tile_id].tensix[0], 0, addr - TENSIX_CFG_BASE, data);
            return true;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "addr=0x%llx", addr);
    }
}

static bool t_tile_mmio_wr64(uint32_t tile_id, uint32_t riscv_id, uint64_t addr, uint64_t data) {
    switch (addr) {
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "addr=0x%llx", addr);
    }
}

static uint32_t e_tile_mmio_rd32(uint32_t tile_id, uint64_t addr) {
#if TT_ARCH_VERSION == 1
    EthTile *p_tile = &g_e_tiles[tile_id];
#endif
    switch (addr) {
        case RISCV_DEBUG_REGS_BASE ... RISCV_DEBUG_REGS_LIMIT:
            return riscv_debug_regs_rd32<'E'>(tile_id, 0, addr - RISCV_DEBUG_REGS_BASE);
#if TT_ARCH_VERSION == 1
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_IERISC_RESET_PC:
            return p_tile->ierisc_reset_pc;
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_SUBORDINATE_IERISC_RESET_PC:
            return p_tile->subordinate_ierisc_reset_pc;
#endif
        case NOC0_REGS_BASE ... NOC0_REGS_LIMIT:
            return noc_regs_rd32<'E'>(tile_id, 0, addr - NOC0_REGS_BASE);
#if NUM_NOCS > 1
        case NOC1_REGS_BASE ... NOC1_REGS_LIMIT:
            return noc_regs_rd32<'E'>(tile_id, 1, addr - NOC1_REGS_BASE);
#endif
        case NOC_OVERLAY_BASE ... NOC_OVERLAY_BASE + 0x3FFFF:
            return noc_overlay_rd32<'E'>(tile_id, addr - NOC_OVERLAY_BASE);
        case ETH_RXQ_BASE ... ETH_RXQ_LIMIT:
            return eth_rxq_regs_rd32(tile_id, addr - ETH_RXQ_BASE);
        case ETH_TXQ_BASE ... ETH_TXQ_LIMIT:
            return eth_txq_regs_rd32(tile_id, addr - ETH_TXQ_BASE);
        case ETH_CTRL_REGS_BASE ... ETH_CTRL_REGS_LIMIT:
            return eth_ctrl_regs_rd32(tile_id, addr - ETH_CTRL_REGS_BASE);
        default: TTSIM_ERROR(UnimplementedFunctionality, "addr=0x%llx", addr);
    }
}

static bool e_tile_mmio_wr32(uint32_t tile_id, uint64_t addr, uint32_t data) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    switch (addr) {
#if TT_ARCH_VERSION == 0
        case 0xFFB12FF0: // ETH_FW_SIM_ERROR_ADDR (must match fw/eth/eth_fw.c)
            TTSIM_ERROR(UndefinedBehavior, "eth base fw reported error code %u on eth tile %u", data, tile_id);
#endif
        case RISCV_DEBUG_REGS_BASE + RISCV_DEBUG_REGS_SOFT_RESET_0:
            data &= ~(TT_ARCH_VERSION ? 0x80046000 : 0x80047000); // ignored bits written by UMD
#if TT_ARCH_VERSION == 1
            TTSIM_VERIFY((data & ~0x1800) == 0, UnimplementedFunctionality, "soft reset 0x%x", data);
            if (data & 0x800) { // Put first erisc into reset
                ttsim_rv32_set_core_active('E', tile_id, 0, false);
            } else if (!ttsim_rv32_get_core_active('E', tile_id, 0)) { // Take first erisc out of reset
                // Take out of reset only if the reset PC has been programmed
                // This is a workaround for not running base fw on erisc0
                // This is the idle erisc case, or erisc1 enabling erisc0
                if (p_tile->ierisc_reset_pc) {
                    p_tile->rv32[0].pc = p_tile->ierisc_reset_pc;
                    ttsim_rv32_set_core_active('E', tile_id, 0, true);
                }
            }
            if (data & 0x1000) { // Put second erisc into reset
                ttsim_rv32_set_core_active('E', tile_id, 1, false);
            } else if (!ttsim_rv32_get_core_active('E', tile_id, 1)) { // Take second erisc out of reset
                // Only start the subordinate erisc once its reset PC has been programmed.
                // 0 guards against running it when multi-erisc mode is disabled and metal
                // never set it up -- mirrors the primary erisc guard above (without this,
                // erisc1 runs from a garbage PC with an invalid stack pointer).
                if (p_tile->subordinate_ierisc_reset_pc) {
                    p_tile->rv32[1].pc = p_tile->subordinate_ierisc_reset_pc;
                    // Establish the stack pointer the faked base FW would have set before
                    // jumping to the app entry (the app starts with a C prologue that
                    // assumes a valid sp); matches the ETH_MAILBOX release-core path.
                    p_tile->rv32[1].x_regs[2] = 0xFFB02000;
                    ttsim_rv32_set_core_active('E', tile_id, 1, true);
                }
            }
#else
            TTSIM_VERIFY((data & ~0x800) == 0, UnimplementedFunctionality, "soft reset 0x%x", data);
            if (data == 0x800) { // Put core into reset
                ttsim_rv32_set_core_active('E', tile_id, 0, false);
            } else if (!ttsim_rv32_get_core_active('E', tile_id, 0)) { // Take core out of reset
                p_tile->rv32[0].pc = p_tile->ierisc_reset_pc;
                ttsim_rv32_set_core_active('E', tile_id, 0, true);
            }
#endif
            p_tile->soft_reset_0 = data;
            return true;
#if TT_ARCH_VERSION == 1
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_IERISC_RESET_PC:
            p_tile->ierisc_reset_pc = data;
            return true;
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_SUBORDINATE_IERISC_RESET_PC:
            p_tile->subordinate_ierisc_reset_pc = data;
            return true;
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_INTERRUPT_MODE(0):
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_INTERRUPT_MODE(1):
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_INTERRUPT_MODE(2):
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_INTERRUPT_MODE(3):
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_INTERRUPT_MODE(4):
            TTSIM_VERIFY(data == 0, UnimplementedFunctionality, "eth interrupt mode=0x%x", data);
            return true;
#endif
        case NOC0_REGS_BASE ... NOC0_REGS_LIMIT:
            noc_regs_wr32<'E'>(tile_id, 0, addr - NOC0_REGS_BASE, data);
            return true;
#if NUM_NOCS > 1
        case NOC1_REGS_BASE ... NOC1_REGS_LIMIT:
            noc_regs_wr32<'E'>(tile_id, 1, addr - NOC1_REGS_BASE, data);
            return true;
#endif
        case NOC_OVERLAY_BASE ... NOC_OVERLAY_BASE + 0x3FFFF:
            noc_overlay_wr32<'E'>(tile_id, addr - NOC_OVERLAY_BASE, data);
            return true;
        case ETH_TXQ_BASE ... ETH_TXQ_LIMIT:
            eth_txq_regs_wr32(tile_id, addr - ETH_TXQ_BASE, data);
            return true;
        case ETH_RXQ_BASE ... ETH_RXQ_LIMIT:
            eth_rxq_regs_wr32(tile_id, addr - ETH_RXQ_BASE, data);
            return true;
        case ETH_CTRL_REGS_BASE ... ETH_CTRL_REGS_LIMIT:
            eth_ctrl_regs_wr32(tile_id, addr - ETH_CTRL_REGS_BASE, data);
            return true;
#if TT_ARCH_VERSION == 1
        case ETH_TXPKT_CFG_BASE ... ETH_TXPKT_CFG_LIMIT:
            eth_txpkt_cfg_wr32(tile_id, addr - ETH_TXPKT_CFG_BASE, data);
            return true;
#endif
#if TT_ARCH_VERSION == 0
        case ETH_MAC_REGS_BASE ... ETH_MAC_REGS_LIMIT:
            eth_mac_regs_wr32(tile_id, addr - ETH_MAC_REGS_BASE, data);
            return true;
#endif
        default: TTSIM_ERROR(UnimplementedFunctionality, "addr=0x%llx", addr);
    }
}

std::pair<bool, uint32_t> tile_mmio_rd32(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t addr) {
    if (tile_type == 'T') {
        return t_tile_mmio_rd32(tile_id, riscv_id, addr);
    } else if (tile_type == 'E') {
        return {true, e_tile_mmio_rd32(tile_id, addr)};
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}

std::pair<bool, uint64_t> tile_mmio_rd64(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t addr) {
    if (tile_type == 'T') {
        return t_tile_mmio_rd64(tile_id, riscv_id, addr);
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}

bool tile_mmio_wr32(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t addr, uint32_t data) {
    if (tile_type == 'T') {
        return t_tile_mmio_wr32(tile_id, riscv_id, addr, data);
    } else if (tile_type == 'E') {
        return e_tile_mmio_wr32(tile_id, addr, data);
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}

bool tile_mmio_wr64(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t addr, uint64_t data) {
    if (tile_type == 'T') {
        return t_tile_mmio_wr64(tile_id, riscv_id, addr, data);
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}

static uint64_t translate_pci_dma_addr(uint64_t addr, uint32_t size) {
    TTSIM_VERIFY(size, UndefinedBehavior, "size=%d", size);
#if TT_ARCH_VERSION == 0
    constexpr uint64_t WINDOW_SIZE = 0xFFFE0000ull;
    TTSIM_VERIFY(addr >= PCIE_HOST_WINDOW_BASE, UnimplementedFunctionality, "addr=0x%llx size=%d", addr, size);
    uint64_t offset = addr - PCIE_HOST_WINDOW_BASE;
    TTSIM_VERIFY(offset + size <= WINDOW_SIZE, UnimplementedFunctionality, "addr=0x%llx size=%d", addr, size);
#else
    // NoC-to-host is 64 windows of 2^58 bytes. Windows 0-3 go straight to the host IOMMU.
    // Windows 4-7 pass through the outbound iATU first, then onwards to the host IOMMU.
    constexpr uint64_t SUBWINDOW_SIZE = 1ull << 58;
    uint64_t offset = addr & (SUBWINDOW_SIZE - 1);
    TTSIM_VERIFY(offset + size <= SUBWINDOW_SIZE, UnimplementedFunctionality, "addr=0x%llx size=%d", addr, size);
    uint64_t window = addr >> 58;
    if (window < 4) {
        return offset;
    }
    TTSIM_VERIFY(window < 8, UnimplementedFunctionality, "addr=0x%llx size=%d", addr, size);
#endif
    bool matched = false;
    uint64_t translated = offset; // identity mapping for offsets outside any configured region
    for (size_t i = 0; i < 16; i++) {
        const IatuRegion &r = g_p_tile.iatu_outbound[i];
        if (!(r.ctrl_2 & IATU_REGION_ENABLE)) {
            continue;
        }
        uint64_t base = (uint64_t(r.upper_base) << 32) | r.lower_base;
#if TT_ARCH_VERSION == 0
        uint64_t limit = (uint64_t(r.upper_base) << 32) | r.lower_limit;
#else
        uint64_t limit = (uint64_t(r.upper_limit) << 32) | r.lower_limit;
#endif
        if ((offset >= base) && (offset + size - 1 <= limit)) {
            TTSIM_VERIFY(!matched, UndefinedBehavior,
                "bar2: outbound iATU offset=0x%llx size=%u matches multiple enabled regions", offset, size);
            uint64_t target = (uint64_t(r.upper_target) << 32) | r.lower_target;
            translated = target + (offset - base);
            matched = true;
        }
    }
    return translated;
}

#define ARC_MISC_CNTL_IRQ0 (1u << 16)

static uint32_t arc_reset_unit_rd32(uint32_t offset) {
    TTSIM_ASSERT(!(offset & 3));
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case RESET_UNIT_SCRATCH(1):
            TTSIM_ERROR(UndefinedBehavior, "scratch 1 is reserved");
        case RESET_UNIT_SCRATCH(0):
        case RESET_UNIT_SCRATCH(2) ... RESET_UNIT_SCRATCH(7):
#else
        case RESET_UNIT_SCRATCH(0) ... RESET_UNIT_SCRATCH(7):
#endif
            return g_a_tile.reset_unit_scratch[(offset - RESET_UNIT_SCRATCH(0)) / 4];
        case RESET_UNIT_ARC_MISC_CNTL:
            return g_a_tile.arc_misc_cntl;
#if TT_ARCH_VERSION == 0
        case RESET_UNIT_NOC_NODEID_X_0:
            return ARC_CSM_BASE + ARC_TELEMETRY_TABLE_CSM_OFFSET;
        case RESET_UNIT_NOC_NODEID_Y_0:
            return ARC_CSM_BASE + ARC_TELEMETRY_VALUES_CSM_OFFSET;
        case RESET_UNIT_ARC_MSG_QCB_PTR:
            return 0; // indicates firmware does not support ARC msg queue
#elif TT_ARCH_VERSION == 1
        case RESET_UNIT_SCRATCH_RAM(0) ... RESET_UNIT_SCRATCH_RAM(15):
            return g_a_tile.scratch_ram[(offset - RESET_UNIT_SCRATCH_RAM(0)) / 4];
#endif
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static uint32_t arc_harvesting_mask(uint32_t chip_id) {
    static constexpr uint8_t LOGICAL_TO_ARC_BIT[] = {1, 3, 5, 7, 9, 8, 6, 4, 2, 0};
    uint32_t logical_mask = logical_harvesting_mask(chip_id);
    uint32_t arc_mask = 0;
    for (uint32_t row = 0; row < std::size(LOGICAL_TO_ARC_BIT); row++) {
        if (logical_mask & (1u << row)) {
            arc_mask |= 1u << LOGICAL_TO_ARC_BIT[row];
        }
    }
    return arc_mask;
}

static void arc_service_message(uint8_t code, uint32_t *resp) {
#if TT_ARCH_VERSION == 0
    uint32_t exit_code = 0;
#endif
    switch (code) {
#if TT_ARCH_VERSION == 0
        case 0x2C: // GET_SMBUS_TELEMETRY_ADDR
            *resp = ARC_SMBUS_TELEMETRY_CSM_OFFSET;
            break;
        case 0x34: // GET_AICLK
            *resp = 1000; // AI CLK = 1000MHz
            break;
#endif
        case 0x57: // ARC_GET_HARVESTING
            *resp = arc_harvesting_mask(g_current_chip_id);
            break;
        case 0x58: // SET_ETH_DRAM_TRAINED_STATUS
            *resp = 1; // report DRAM training complete
            break;
        case 0x11: // NOP
        case 0x21: // POWER_SETTING
        case 0x51: // PCIE_INDEX
        case 0x52: // ARC_GO_BUSY
        case 0x53: // ARC_GO_SHORT_IDLE
        case 0x54: // ARC_GO_LONG_IDLE
        case 0x56: // TRIGGER_RESET
        case 0x90: // TEST
        case 0xA0: // ASIC_STATE0
        case 0xA3: // ASIC_STATE3
        case 0xB6: // PCIE_RETRAIN (WH) / TOGGLE_GDDR_RESET (BH)
        case 0xB7: // CURR_DATE
        case 0xBC: // UPDATE_M3_AUTO_RESET_TIMEOUT
        case 0xBA: // DEASSERT_RISCV_RESET
        case 0xC1: // SET_WDT_TIMEOUT
#if TT_ARCH_VERSION == 1
        case 0xC7: // TT_PCIE_LOG
#endif
            break; // does not modify modeled subsystems
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "arc message code=0x%x", code);
    }
#if TT_ARCH_VERSION == 0
    g_a_tile.reset_unit_scratch[5] = (exit_code << 16) | code;
#endif
}

#if TT_ARCH_VERSION == 1
// Drain one queue's request ring, writing a response for each request.
static void arc_drain_queue(uint32_t queue, uint32_t num_entries) {
    uint32_t req_wr_ptr = mem_rd<uint32_t>(&g_a_tile.csm[queue + 0x00]);
    uint32_t req_rd_ptr = mem_rd<uint32_t>(&g_a_tile.csm[queue + 0x10]);
    uint32_t res_wr_ptr = mem_rd<uint32_t>(&g_a_tile.csm[queue + 0x14]);
    uint32_t req_base = queue + ARC_MSG_QUEUE_HEADER_SIZE;
    uint32_t res_base = req_base + num_entries * ARC_MSG_ENTRY_SIZE;
    uint32_t occupied = (req_wr_ptr - req_rd_ptr) % (2 * num_entries);
    TTSIM_VERIFY(occupied <= num_entries, UndefinedBehavior, "arc msg queue: occupied=%u num_entries=%u", occupied, num_entries);
    for (uint32_t k = 0; k < occupied; k++) {
        uint32_t req_off = req_base + (req_rd_ptr % num_entries) * ARC_MSG_ENTRY_SIZE;
        uint32_t res_off = res_base + (res_wr_ptr % num_entries) * ARC_MSG_ENTRY_SIZE;
        uint32_t req[8], resp[8];
        for (uint32_t i = 0; i < 8; i++) {
            req[i] = mem_rd<uint32_t>(&g_a_tile.csm[req_off + 4 * i]);
        }
        uint8_t code = req[0] & 0xff;
        for (uint32_t i = 0; i < 8; i++) {
            resp[i] = 0;
        }
        arc_service_message(code, resp);
        for (uint32_t i = 0; i < 8; i++) {
            mem_wr<uint32_t>(&g_a_tile.csm[res_off + 4 * i], resp[i]);
        }
        res_wr_ptr = (res_wr_ptr + 1) % (2 * num_entries);
        req_rd_ptr = (req_rd_ptr + 1) % (2 * num_entries);
    }
    mem_wr<uint32_t>(&g_a_tile.csm[queue + 0x10], req_rd_ptr);
    mem_wr<uint32_t>(&g_a_tile.csm[queue + 0x14], res_wr_ptr);
}

// host<->ARC message queues, laid out in CSM: the queue control block points at an array of ARC_MSG_NUM_QUEUES queues.
// Each queue is a 32-byte header + num_entries 32-byte request slots + the same response slots.
static void arc_process_message_queue() {
    TTSIM_VERIFY(g_a_tile.scratch_ram[11] >= ARC_CSM_BASE, UndefinedBehavior, "arc msg queue ptr=%x", g_a_tile.scratch_ram[11]);
    uint32_t qcb = g_a_tile.scratch_ram[11] - ARC_CSM_BASE;
    uint32_t queue_base = mem_rd<uint32_t>(&g_a_tile.csm[qcb]) - ARC_CSM_BASE;
    uint32_t num_entries = mem_rd<uint32_t>(&g_a_tile.csm[qcb + 4]) & 0xff;
    TTSIM_VERIFY(num_entries, UndefinedBehavior, "arc msg queue: num_entries is zero");
    uint32_t stride = ARC_MSG_QUEUE_HEADER_SIZE + 2 * num_entries * ARC_MSG_ENTRY_SIZE;
    for (uint32_t q = 0; q < ARC_MSG_NUM_QUEUES; q++) {
        arc_drain_queue(queue_base + q * stride, num_entries);
    }
}
#endif

static void arc_reset_unit_wr32(uint32_t offset, uint32_t value) {
    TTSIM_ASSERT(!(offset & 3));
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case RESET_UNIT_SCRATCH(1):
            TTSIM_ERROR(UndefinedBehavior, "scratch 1 is reserved");
        case RESET_UNIT_SCRATCH(0):
        case RESET_UNIT_SCRATCH(2) ... RESET_UNIT_SCRATCH(7):
#else
        case RESET_UNIT_SCRATCH(0) ... RESET_UNIT_SCRATCH(7):
#endif
            g_a_tile.reset_unit_scratch[(offset - RESET_UNIT_SCRATCH(0)) / 4] = value;
            break;
#if TT_ARCH_VERSION == 1
        case RESET_UNIT_SCRATCH_RAM(0) ... RESET_UNIT_SCRATCH_RAM(15):
            g_a_tile.scratch_ram[(offset - RESET_UNIT_SCRATCH_RAM(0)) / 4] = value;
            break;
#endif
        case RESET_UNIT_ARC_MISC_CNTL:
            g_a_tile.arc_misc_cntl = value;
            if (value & ARC_MISC_CNTL_IRQ0) {
#if TT_ARCH_VERSION == 0
                uint8_t code = g_a_tile.reset_unit_scratch[5] & 0xff;
                arc_service_message(code, &g_a_tile.reset_unit_scratch[3]);
#else
                arc_process_message_queue();
#endif
                g_a_tile.arc_misc_cntl &= ~ARC_MISC_CNTL_IRQ0;
            }
            break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static uint32_t arc_apb_rd32(uint32_t apb_offset) {
    switch (apb_offset) {
        case ARC_APB_RESET_UNIT_BASE ... ARC_APB_RESET_UNIT_LIMIT:
            return arc_reset_unit_rd32(apb_offset - ARC_APB_RESET_UNIT_BASE);
        case ARC_APB_NIU0_BASE ... ARC_APB_NIU0_LIMIT:
            return base_noc_regs_rd32(&g_a_tile.base, 'A', 0, 0, apb_offset - ARC_APB_NIU0_BASE);
        case ARC_APB_NIU1_BASE ... ARC_APB_NIU1_LIMIT:
            return base_noc_regs_rd32(&g_a_tile.base, 'A', 0, 1, apb_offset - ARC_APB_NIU1_BASE);
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", apb_offset);
    }
}

static void arc_apb_wr32(uint32_t apb_offset, uint32_t value) {
    switch (apb_offset) {
        case ARC_APB_RESET_UNIT_BASE ... ARC_APB_RESET_UNIT_LIMIT:
            arc_reset_unit_wr32(apb_offset - ARC_APB_RESET_UNIT_BASE, value);
            break;
#if TT_ARCH_VERSION == 1
        case ARC_APB_MSI_FIFO_BASE ... ARC_APB_MSI_FIFO_LIMIT:
            arc_process_message_queue();
            break;
#endif
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", apb_offset);
    }
}

static void arc_tile_rd_bytes(uint64_t addr, void *p, uint32_t size) {
#if TT_ARCH_VERSION == 0
    switch (addr) {
        case ARC_NOC_NIU0_BASE ... ARC_NOC_NIU0_LIMIT:
            TTSIM_VERIFY((size == 4) && !(addr & 3), UndefinedBehavior, "arc_niu: addr=0x%llx size=%d", addr, size);
            mem_wr<uint32_t>(p, base_noc_regs_rd32(&g_a_tile.base, 'A', 0, 0, uint32_t(addr - ARC_NOC_NIU0_BASE)));
            return;
        case ARC_NOC_NIU1_BASE ... ARC_NOC_NIU1_LIMIT:
            TTSIM_VERIFY((size == 4) && !(addr & 3), UndefinedBehavior, "arc_niu: addr=0x%llx size=%d", addr, size);
            mem_wr<uint32_t>(p, base_noc_regs_rd32(&g_a_tile.base, 'A', 0, 1, uint32_t(addr - ARC_NOC_NIU1_BASE)));
            return;
    }
    TTSIM_VERIFY((addr >= ARC_NOC_XBAR_BASE) && (addr <= ARC_NOC_XBAR_LIMIT), UnimplementedFunctionality,
        "arc: addr=0x%llx size=%d", addr, size);
    uint64_t offset = addr - ARC_NOC_XBAR_BASE;
#else
    uint64_t offset = addr;
#endif
    switch (offset) {
        case ARC_CSM_BASE ... ARC_CSM_LIMIT: {
            uint32_t csm_offset = offset - ARC_CSM_BASE;
            TTSIM_VERIFY(uint64_t(csm_offset) + uint64_t(size) <= ARC_CSM_SIZE, UndefinedBehavior,
                "arc_csm overrun: offset=0x%x size=%d", csm_offset, size);
            memcpy(p, &g_a_tile.csm[csm_offset], size);
            break;
        }
        case ARC_APB_BASE ... ARC_APB_LIMIT: {
            uint32_t apb_offset = offset - ARC_APB_BASE;
            TTSIM_VERIFY((size == 4) && !(apb_offset & 3), UndefinedBehavior,
                "arc_apb: offset=0x%x size=%d", apb_offset, size);
            mem_wr<uint32_t>(p, arc_apb_rd32(apb_offset));
            break;
        }
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "arc: offset=0x%llx size=%d", offset, size);
    }
}

static void arc_tile_wr_bytes(uint64_t addr, const void *p, uint32_t size) {
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY((addr >= ARC_NOC_XBAR_BASE) && (addr + size <= ARC_NOC_XBAR_LIMIT + 1),
        UnimplementedFunctionality, "arc: addr=0x%llx size=%d", addr, size);
    uint64_t offset = addr - ARC_NOC_XBAR_BASE;
#else
    uint64_t offset = addr;
#endif
    switch (offset) {
        case ARC_CSM_BASE ... ARC_CSM_LIMIT: {
            uint32_t csm_offset = offset - ARC_CSM_BASE;
            TTSIM_VERIFY(uint64_t(csm_offset) + uint64_t(size) <= ARC_CSM_SIZE, UndefinedBehavior,
                "arc_csm overrun: offset=0x%x size=%d", csm_offset, size);
            memcpy(&g_a_tile.csm[csm_offset], p, size);
            break;
        }
        case ARC_APB_BASE ... ARC_APB_LIMIT: {
            uint32_t apb_offset = offset - ARC_APB_BASE;
            TTSIM_VERIFY((size == 4) && !(apb_offset & 3), UndefinedBehavior,
                "arc_apb: offset=0x%x size=%d", apb_offset, size);
            arc_apb_wr32(apb_offset, mem_rd<uint32_t>(p));
            break;
        }
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "arc: offset=0x%llx size=%d", offset, size);
    }
}

void tile_rd_bytes(uint32_t coord, uint64_t addr, void *p, uint32_t size) {
    auto [tile_type, tile_id] = coord_to_tile(coord);
    if (tile_type == 'D') {
#if TT_ARCH_VERSION == 0
        if ((addr >= DRAM_APB_BASE) && (addr <= DRAM_APB_LIMIT)) {
            uint32_t apb_offset = addr - DRAM_APB_BASE;
            TTSIM_VERIFY((size == 4) && !(apb_offset & 3), UndefinedBehavior, "dram apb offset=0x%x, size=%d", apb_offset, size);
            TTSIM_VERIFY((apb_offset >= DRAM_APB_NIU_BASE) && (apb_offset <= DRAM_APB_NIU_LIMIT),
                UnimplementedFunctionality, "dram apb offset=0x%x", apb_offset);
            uint32_t niu_offset = apb_offset % 0x8000;
            uint32_t noc_instance = (apb_offset / 0x8000) % 2;
            TTSIM_VERIFY(niu_offset == NOC_REGS_NOC_NODE_ID, UnimplementedFunctionality, "dram apb niu offset=0x%x", niu_offset);
            mem_wr<uint32_t>(p, noc_node_id(noc_instance, coord));
            return;
        }
#endif
        TTSIM_VERIFY(addr + size <= DRAM_CHANNEL_SIZE, UndefinedBehavior, "DRAM read overrun");
        memcpy(p, &g_dram[tile_id].p_mem[addr], size);
    } else if (tile_type == 'E') {
        if (addr < ETH_SRAM_SIZE) {
            TTSIM_VERIFY(addr + size <= ETH_SRAM_SIZE, UndefinedBehavior, "ETH read overrun");
            memcpy(p, &g_e_tiles[tile_id].sram[addr], size);
#if TT_ARCH_VERSION == 1
            // TODO: This is a hack to simulate base fw heartbeat
            if (addr == 0x7CC70 && !ttsim_rv32_get_core_active('E', tile_id, 0)) {
                mem_wr<uint32_t>(&g_e_tiles[tile_id].sram[addr], mem_rd<uint32_t>(&g_e_tiles[tile_id].sram[addr]) + 1);
            }
#endif
            return;
        }
        TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "tile=%c%d addr=0x%llx size=%d", tile_type, tile_id, addr, size);
        mem_wr<uint32_t>(p, e_tile_mmio_rd32(tile_id, addr));
    } else if (tile_type == 'T') {
        if (addr < sizeof(g_t_tiles[tile_id].sram)) {
            TTSIM_VERIFY(addr + size <= sizeof(g_t_tiles[tile_id].sram), UndefinedBehavior, "access past end of tensix_sram");
            memcpy(p, &g_t_tiles[tile_id].sram[addr], size);
            return;
        }
        if (size == 4) {
            auto [done, data] = t_tile_mmio_rd32(tile_id, 0xFFFFFFFF, addr); // no riscv_id in this path
            TTSIM_VERIFY(done, UnimplementedFunctionality, "t_tile_mmio_rd32 failed: addr=0x%llx", addr);
            mem_wr<uint32_t>(p, data);
        } else {
            TTSIM_VERIFY(size == 8, UnsupportedFunctionality, "tile=%c%d addr=0x%llx size=%d", tile_type, tile_id, addr, size);
            auto [done, data] = t_tile_mmio_rd64(tile_id, 0xFFFFFFFF, addr); // no riscv_id in this path
            TTSIM_VERIFY(done, UnimplementedFunctionality, "t_tile_mmio_rd64 failed: addr=0x%llx", addr);
            mem_wr<uint64_t>(p, data);
        }
    } else if (tile_type == 'P') {
        TTSIM_VERIFY(!tile_id, UnimplementedFunctionality, "tile=%c%d", tile_type, tile_id);
#if TT_ARCH_VERSION == 1
        if ((addr >= PCIE_DBI_BASE) && (addr <= PCIE_DBI_LIMIT)) {
            TTSIM_VERIFY((size == 4) && !(addr & 3), UndefinedBehavior, "pcie_dbi: addr=0x%llx size=%d", addr, size);
            mem_wr<uint32_t>(p, pcie_dbi_rd32(addr - PCIE_DBI_BASE));
            return;
        }
#endif
        libttsim_pci_dma_mem_rd_bytes(translate_pci_dma_addr(addr, size), p, size);
    } else if (tile_type == 'A') {
        TTSIM_VERIFY(!tile_id, UnimplementedFunctionality, "tile=%c%d", tile_type, tile_id);
        arc_tile_rd_bytes(addr, p, size);
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}

void tile_wr_bytes(uint32_t coord, uint64_t addr, const void *p, uint32_t size) {
    auto [tile_type, tile_id] = coord_to_tile(coord);
    if (tile_type == 'D') {
        TTSIM_VERIFY(addr + size <= DRAM_CHANNEL_SIZE, UndefinedBehavior, "DRAM write overrun");
        memcpy(&g_dram[tile_id].p_mem[addr], p, size);
    } else if (tile_type == 'E') {
        if (addr < ETH_SRAM_SIZE) {
            TTSIM_VERIFY(addr + size <= ETH_SRAM_SIZE, UndefinedBehavior, "ETH write overrun");
#if TT_ARCH_VERSION == 0
            // Base FW switches to user code when LAUNCH_ERISC_APP_FLAG is set
            // This is the active erisc case
            if (addr == 0x9004) {
                TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "LAUNCH_ERISC_APP_FLAG size=0x%x", size);
                uint32_t data = mem_rd<uint32_t>(p);
                uint32_t old_data = mem_rd<uint32_t>(&g_e_tiles[tile_id].sram[addr]);
                if ((data == 1) && (old_data != 1)) { // Start active erisc core
                    g_e_tiles[tile_id].rv32[0].pc = 0x9040; // FIRMWARE_BASE
                    g_e_tiles[tile_id].rv32[0].x_regs[1] = g_e_tiles[tile_id].ierisc_reset_pc;
                    g_e_tiles[tile_id].rv32[0].x_regs[2] = 0xFFB01000;
                    ttsim_rv32_set_core_active('E', tile_id, 0, true);
                } else if ((data == 0) && (old_data == 1)) { // Stop active erisc core
                    ttsim_rv32_set_core_active('E', tile_id, 0, false);
                }
            }
#elif TT_ARCH_VERSION == 1
            // Fake the cooperative eth base-FW + active-erisc launch handshake. metal posts a go
            // message (go_msg_t) with signal=RUN_MSG_INIT (0x40, the top byte) to the active-erisc
            // mailbox go_messages[0] at L1 0x590, then polls for RUN_MSG_DONE. The real eth base FW
            // and app (which ttsim fakes as no-ops) would process the launch and ack; emulate the
            // ack by storing the message with the signal byte cleared to RUN_MSG_DONE (0) so device
            // init proceeds. (RUN_MSG_* and go_msg_t.signal per tt-metal dev_msgs.h.)
            if (addr == 0x590 && size == 4 && (mem_rd<uint32_t>(p) >> 24) == 0x40) {
                mem_wr<uint32_t>(&g_e_tiles[tile_id].sram[addr], mem_rd<uint32_t>(p) & 0x00FFFFFF);
                return;
            }
            // RUN_MSG_GO (signal 0x80) launches the active-erisc app kernel (e.g. the fabric erisc
            // router / EDM). On silicon the active-erisc base FW, on GO, computes the kernel entry
            // from the launch message, sets up gp/sp, runs setup_kernel_launch_args(), and jumps in;
            // ttsim fakes the base FW, so emulate that launch here. The launch message lives in the
            // active-erisc mailbox (base 0x100; mailboxes_t go_messages[0] offset 0x490,
            // L1 address 0x590). Field offsets per tt-metal dev_msgs.h
            // (BH ProgrammableCoreType::COUNT=4, MaxProcessorsPerCoreType=5):
            // launch_msg_rd_ptr @ base+12; launch[] @ base+16 (stride sizeof(launch_msg_t)=144);
            // kernel_config_base[ACTIVE_ETH=1] @ launch+4; rta_offset[DM0=0] @ launch+28 (rta) / +30
            // (crta); kernel_text_offset[DM0=0] @ launch+52.
            //
            // ONLY for ACTIVE eth cores (those with an inter-chip peer, running the active-erisc app).
            // An IDLE eth core posts the same GO to 0x590 but is launched by the idle-erisc base FW,
            // which reads kernel_config_base[IDLE_ETH] (index 2), not [ACTIVE_ETH] -- and ttsim runs
            // its kernel via the soft-reset / ierisc_reset_pc path instead (see SOFT_RESET_0 above).
            // Intercepting an idle core's GO here would jump to a bogus ACTIVE_ETH entry (e.g. on a
            // single-chip part every eth core is idle: regression at b0bca86). So gate on eth_peer and
            // otherwise fall through to the plain mailbox store.
            uint32_t go_peer_chip, go_peer_tile;
            if (addr == 0x590 && size == 4 && (mem_rd<uint32_t>(p) >> 24) == 0x80 &&
                eth_peer(tile_id, &go_peer_chip, &go_peer_tile)) {
                memcpy(&g_e_tiles[tile_id].sram[addr], p, size);
                const uint8_t *l1 = g_e_tiles[tile_id].sram;
                uint32_t rd_ptr = mem_rd<uint32_t>(&l1[0x100 + 12]);
                uint32_t launch = 0x100 + 16 + rd_ptr * 144;
                uint32_t kcfg_base = mem_rd<uint32_t>(&l1[launch + 4]); // kernel_config_base[ACTIVE_ETH]
                uint32_t entry = kcfg_base + mem_rd<uint32_t>(&l1[launch + 52]);
                // The base FW runs setup_kernel_launch_args() (firmware_common.h) before jumping: it
                // sets the kernel's rta_l1_base / crta_l1_base globals from the launch message so
                // get_arg_val() / get_common_arg_val() find the runtime args. Without it both stay 0
                // and every get_arg_val() reads L1 address 0 (e.g. the fabric router's per-channel
                // connection_live_semaphore comes back null and the worker data path hangs). These
                // FW-owned LDM globals sit below the kernel's .bss so crt0 never clears them; their
                // addresses are the active-erisc FW/kernel ABI: rta_l1_base @ 0xFFB00714, crta @ 0xFFB00710.
                uint8_t *ldm = g_e_tiles[tile_id].rv32_local_ram[0];
                mem_wr<uint32_t>(&ldm[0xFFB00714 - RISCV_LOCAL_MEM_BASE], kcfg_base + mem_rd<uint16_t>(&l1[launch + 28]));
                mem_wr<uint32_t>(&ldm[0xFFB00710 - RISCV_LOCAL_MEM_BASE], kcfg_base + mem_rd<uint16_t>(&l1[launch + 30]));
                // Likewise fake firmware risc_init()'s my_x[]/my_y[] (FW-owned LDM globals my_y @ 0xFFB00708,
                // my_x @ 0xFFB0070C, each uint8_t[NUM_NOCS]). risc_init sets my_x[n]/my_y[n] from
                // NOC_CFG(NOC_ID_LOGICAL); the kernel later writes WorkerXY(my_x[0], my_y[0]) into a peer EDM's
                // connection info at connect (edm_fabric_worker_adapters.hpp), and the peer credits back to that
                // coord. Without this they stay 0, so the fabric router's free-space credit targets NOC (0,0)
                // (DRAM) -- the multi-hop forwarding hang/abort. NOC_ID_LOGICAL here matches NOC_REGS_NOC_ID_LOGICAL.
                uint32_t nidl = (20 + tile_id) | (25 << 6); // logical eth coord (20+tile_id, 25), both NOCs
                for (uint32_t n = 0; n < 2; n++) { // NUM_NOCS == 2 on BH
                    ldm[(0xFFB0070C - RISCV_LOCAL_MEM_BASE) + n] = nidl & 0x3F;        // my_x[n]
                    ldm[(0xFFB00708 - RISCV_LOCAL_MEM_BASE) + n] = (nidl >> 6) & 0x3F; // my_y[n]
                }
                g_e_tiles[tile_id].rv32[0].pc = entry;
                g_e_tiles[tile_id].rv32[0].x_regs[1] = 0; // ra=0: kernel_main returns here on exit
                g_e_tiles[tile_id].rv32[0].x_regs[2] = 0xFFB02000; // sp = top of eth LDM
                g_e_tiles[tile_id].rv32[0].x_regs[3] = 0xFFB00EF0; // gp = __global_pointer$
                ttsim_rv32_set_core_active('E', tile_id, 0, true);
                return;
            }
            if (addr == 0x7D000 || addr == 0x7D010 || addr == 0x7D020 || addr == 0x7D030) { // ETH_MAILBOX
                TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "ETH_MAILBOX size=0x%x", size);
                uint32_t data = mem_rd<uint32_t>(p);
                if (data == 0xCA110002) { // ETH_MSG_CALL and ETH_MSG_RELEASE_CORE
                    uint8_t *mailbox = &g_e_tiles[tile_id].sram[addr];
                    g_e_tiles[tile_id].rv32[0].pc = mem_rd<uint32_t>(mailbox + 4);
                    g_e_tiles[tile_id].rv32[0].x_regs[2] = 0xFFB02000;
                    mem_wr<uint32_t>(mailbox + 0, 0xD0E50002); // ETH_MSG_DONE
                    mem_wr<uint32_t>(mailbox + 4, 0);
                    mem_wr<uint32_t>(mailbox + 8, 0);
                    mem_wr<uint32_t>(mailbox + 12, 0);
                    ttsim_rv32_set_core_active('E', tile_id, 0, true);
                    return;
                } else {
                    TTSIM_ERROR(UnimplementedFunctionality, "ETH_MAILBOX data=0x%x", data);
                }
            } else if (addr == 0x7E8) { // Metal ETH_FW_RUN_FLAG
                TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "Metal ETH_FW_RUN_FLAG size=0x%x", size);
                uint32_t data = mem_rd<uint32_t>(p);
                if (!data && mem_rd<uint32_t>(&g_e_tiles[tile_id].sram[addr])) {
                    ttsim_rv32_set_core_active('E', tile_id, 0, false);
                    g_e_tiles[tile_id].ierisc_reset_pc = 0; // Reset back to guard value for BH base fw
                }
            }
#endif
            memcpy(&g_e_tiles[tile_id].sram[addr], p, size);
            return;
        }
        TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "tile=%c%d addr=0x%llx size=%d", tile_type, tile_id, addr, size);
        bool done = e_tile_mmio_wr32(tile_id, addr, mem_rd<uint32_t>(p));
        TTSIM_VERIFY(done, UnimplementedFunctionality, "e_tile_mmio_wr32 failed: addr=0x%llx", addr);
    } else if (tile_type == 'T') {
        if (addr < sizeof(g_t_tiles[tile_id].sram)) {
            TTSIM_VERIFY(addr + size <= sizeof(g_t_tiles[tile_id].sram), UndefinedBehavior, "access past end of tensix_sram");
            memcpy(&g_t_tiles[tile_id].sram[addr], p, size);
            return;
        }
        if (size == 4) {
            bool done = t_tile_mmio_wr32(tile_id, 0xFFFFFFFF, addr, mem_rd<uint32_t>(p)); // no riscv_id in this path
            TTSIM_VERIFY(done, UnimplementedFunctionality, "t_tile_mmio_wr32 failed: addr=0x%llx", addr);
        } else {
            TTSIM_VERIFY(size == 8, UnsupportedFunctionality, "tile=%c%d addr=0x%llx size=%d", tile_type, tile_id, addr, size);
            bool done = t_tile_mmio_wr64(tile_id, 0xFFFFFFFF, addr, mem_rd<uint64_t>(p)); // no riscv_id in this path
            TTSIM_VERIFY(done, UnimplementedFunctionality, "t_tile_mmio_wr64 failed: addr=0x%llx", addr);
        }
    } else if (tile_type == 'P') {
        TTSIM_VERIFY(!tile_id, UnimplementedFunctionality, "tile=%c%d", tile_type, tile_id);
#if TT_ARCH_VERSION == 1
        if ((addr >= PCIE_DBI_BASE) && (addr <= PCIE_DBI_LIMIT)) {
            TTSIM_VERIFY((size == 4) && !(addr & 3), UndefinedBehavior, "pcie_dbi: addr=0x%llx size=%d", addr, size);
            pcie_dbi_wr32(addr - PCIE_DBI_BASE, mem_rd<uint32_t>(p));
            return;
        }
#endif
        libttsim_pci_dma_mem_wr_bytes(translate_pci_dma_addr(addr, size), p, size);
    } else if (tile_type == 'A') {
        TTSIM_VERIFY(!tile_id, UnimplementedFunctionality, "tile=%c%d", tile_type, tile_id);
        arc_tile_wr_bytes(addr, p, size);
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}
