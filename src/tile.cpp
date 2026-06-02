// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Per-tile infrastructure: tile init, NOC routing, TDMA, Ethernet, DRAM, debug/config registers.
#include "sim.h"

#if TT_ARCH_VERSION == 0
#define NONTENSIX_COL_MASK 0x21
#define NONTENSIX_ROW_MASK 0x41
#elif TT_ARCH_VERSION == 1
#define NONTENSIX_COL_MASK 0x301
#define NONTENSIX_ROW_MASK 0x03
#endif

template<char tile_type>
static inline auto get_tile(uint32_t tile_id) {
    static_assert((tile_type == 'T') || (tile_type == 'E'));
    if constexpr (tile_type == 'T') {
        return &g_t_tiles[tile_id];
    } else {
        return &g_e_tiles[tile_id];
    }
}

void t_tile_init(uint32_t tile_id) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
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
    for (uint32_t noc = 0; noc < NUM_NOCS; noc++) {
        // enable all tensix rows/cols by default
        p_tile->router_cfg_1[noc] = NONTENSIX_COL_MASK;
        p_tile->router_cfg_3[noc] = NONTENSIX_ROW_MASK;
    }
}

void e_tile_init(uint32_t tile_id) {
    EthTile *p_tile = &g_e_tiles[tile_id];
    for (uint32_t rv32_id = 0; rv32_id < std::size(p_tile->rv32); rv32_id++) {
        rv32_init(&p_tile->rv32[rv32_id], 'E', tile_id, rv32_id);
    }
    p_tile->soft_reset_0 = TT_ARCH_VERSION ? 0x1800 : 0x800;
    p_tile->ierisc_reset_pc = 0; // 0 is used as a guard for BH base fw
    constexpr uint32_t riscv_ret_inst = 0x8067;
    for (uint32_t noc = 0; noc < NUM_NOCS; noc++) {
        // enable all tensix rows/cols by default
        p_tile->router_cfg_1[noc] = NONTENSIX_COL_MASK;
        p_tile->router_cfg_3[noc] = NONTENSIX_ROW_MASK;
    }
#if TT_ARCH_VERSION == 0
    mem_wr<uint32_t>(&p_tile->sram[0x210], 0x06060000); // FW_VERSION_ADDR
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
#endif
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
#elif TT_ARCH_VERSION == 1
        uint32_t tile_x;
        if (tile_id & 1) {
            tile_x = (13 - tile_id) / 2 + 7;
        } else {
            tile_x = tile_id / 2;
        }
        tile_x += (tile_x >= 7) ? 3 : 1;
        return tile_x | (1 << 6);
#else
#error unsupported
#endif
    } else
    {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}

static std::pair<char, uint32_t> coord_to_tile(uint32_t coord)
{
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
        case 1: case 2: case 3: case 4:
            tile_x = coord_x - 1;
            break;
        case 6: case 7: case 8: case 9:
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
        case 1: case 2: case 3: case 4: case 5:
            tile_y = coord_y - 1;
            tile_id = tile_x + tile_y*8;
            TTSIM_ASSERT(tile_id < NUM_T_TILES);
            return {'T', tile_id};
        case 7: case 8: case 9: case 10: case 11:
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
        case RISCV_TDMA_REGS_XMOV_DIRECTION: TTSIM_VERIFY(data == 1, UnsupportedFunctionality, "XMOV_DIRECTION=0x%x", data); break; // must be XMOV_L1_TO_L0
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

template<char tile_type>
static uint32_t riscv_debug_regs_rd32(uint32_t tile_id, uint32_t tensix_id, uint32_t offset) {
    auto *p_tile = get_tile<tile_type>(tile_id);
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case RISCV_DEBUG_REGS_DBG_ARRAY_RD_DATA:
            if constexpr (tile_type == 'T') {
                TTSIM_VERIFY(p_tile->dbg_array_rd_en, UnsupportedFunctionality, "DBG_ARRAY_RD_DATA when DBG_ARRAY_RD_EN=0");
                return p_tile->dbg_array_rd_data;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "DBG_ARRAY_RD_DATA in eth tile");
        case RISCV_DEBUG_REGS_CFGREG_RDDATA: TTSIM_ERROR(UnimplementedFunctionality, "CFGREG_RDDATA");
#endif
#if TT_ARCH_VERSION == 1
        case RISCV_DEBUG_REGS_DBG_INSTRN_BUF_STATUS: TTSIM_ERROR(UnimplementedFunctionality, "DBG_INSTRN_BUF_STATUS");
#endif
        case RISCV_DEBUG_REGS_DBG_FEATURE_DISABLE:
            if constexpr (tile_type == 'T') {
                return p_tile->tensix[0].dst_32bit_addr_en ? 0x800 : 0;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "DBG_FEATURE_DISABLE in eth tile");
        case RISCV_DEBUG_REGS_SOFT_RESET_0: return p_tile->soft_reset_0;
        case RISCV_DEBUG_REGS_WALL_CLOCK_L: return uint32_t(g_clock);
        case RISCV_DEBUG_REGS_WALL_CLOCK_1: return uint32_t(g_clock >> 32); // high part, unlatched
        case RISCV_DEBUG_REGS_WALL_CLOCK_H: return uint32_t(g_clock >> 32); // XXX add the latching behavior on this reg
#if TT_ARCH_VERSION == 1
        case RISCV_DEBUG_REGS_TRISC_RESET_PC_OVERRIDE:
            if constexpr (tile_type == 'T') {
                return p_tile->trisc_reset_pc_override;
            }
            TTSIM_ERROR(UnsupportedFunctionality, "TRISC_RESET_PC_OVERRIDE in eth tile");
#endif
        RISCV_DEBUG_REGS_RD_DEFAULT_CASES()
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static void riscv_debug_regs_wr32(uint32_t tile_id, uint32_t tensix_id, uint32_t offset, uint32_t data) {
    TensixTile *p_tile = &g_t_tiles[tile_id];
    switch (offset) {
#if TT_ARCH_VERSION == 0
        case RISCV_DEBUG_REGS_CFGREG_RD_CNTL: TTSIM_ERROR(UnimplementedFunctionality, "CFGREG_RD_CNTL");
        case RISCV_DEBUG_REGS_DBG_ARRAY_RD_EN:
            TTSIM_VERIFY(data <= 1, UnsupportedFunctionality, "DBG_ARRAY_RD_EN: data=0x%x", data);
            p_tile->dbg_array_rd_en = data;
            break;
        case RISCV_DEBUG_REGS_DBG_ARRAY_RD_CMD: {
            TTSIM_VERIFY(p_tile->dbg_array_rd_en, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD when DBG_ARRAY_RD_EN=0");
            uint32_t row = bits<11,0>(data);
            uint32_t sel = bits<15,12>(data);
            uint32_t upper = bits<31,16>(data);
            TTSIM_VERIFY(row < DST_ROWS, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD: row=%d", row);
            TTSIM_VERIFY(sel < 8, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD: sel=%d", sel);
            TTSIM_VERIFY(upper == 2, MissingSpecification, "DBG_ARRAY_RD_CMD: upper=0x%x", upper);
            p_tile->dbg_array_rd_data = p_tile->tensix[0].dst[row][2*sel] | (uint32_t(p_tile->tensix[0].dst[row][2*sel+1]) << 16);
            break;
        }
#elif TT_ARCH_VERSION == 1
        case RISCV_DEBUG_REGS_TENSIX_CREG_READ: TTSIM_ERROR(UnimplementedFunctionality, "TENSIX_CREG_READ");
#endif
        case RISCV_DEBUG_REGS_DBG_FEATURE_DISABLE:
            TTSIM_VERIFY(!data || (data == 0x800), UnimplementedFunctionality, "DBG_FEATURE_DISABLE=0x%x", data);
            p_tile->tensix[0].dst_32bit_addr_en = bits<11,11>(data);
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
                            TTSIM_VERIFY(p_tile->trisc_reset_pc_override & 1, UntestedFunctionality, "trisc_reset_pc_override=0x%x", p_tile->trisc_reset_pc_override);
                            reset_pc = (p_tile->trisc_reset_pc_override & 1) ? p_tile->trisc0_reset_pc : 0x6000;
                            break;
                        case RV32_ID_TRISC1:
                            TTSIM_VERIFY(p_tile->trisc_reset_pc_override & 2, UntestedFunctionality, "trisc_reset_pc_override=0x%x", p_tile->trisc_reset_pc_override);
                            reset_pc = (p_tile->trisc_reset_pc_override & 2) ? p_tile->trisc1_reset_pc : 0xA000;
                            break;
                        case RV32_ID_TRISC2:
                            TTSIM_VERIFY(p_tile->trisc_reset_pc_override & 4, UntestedFunctionality, "trisc_reset_pc_override=0x%x", p_tile->trisc_reset_pc_override);
                            reset_pc = (p_tile->trisc_reset_pc_override & 4) ? p_tile->trisc2_reset_pc : 0xE000;
                            break;
                        case RV32_ID_NCRISC:
                            TTSIM_VERIFY(p_tile->ncrisc_reset_pc_override == 1, UnimplementedFunctionality,
                                "ncrisc_reset_pc_override=0x%x", p_tile->ncrisc_reset_pc_override);
                            reset_pc = p_tile->ncrisc_reset_pc;
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
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

// XXX This only handles some cases for WH/BH; need to finish it out
// XXX Need to do this based on the remapping registers
uint32_t remap_virtual_coordinate(uint32_t noc_instance, uint32_t coord) {
    TTSIM_VERIFY(coord <= 0xFFF, AssertionFailure, "coord=0x%x", coord);
    uint32_t coord_x = coord & 63;
    uint32_t coord_y = coord >> 6;
#if TT_ARCH_VERSION == 0
    switch (coord_x) {
        case 18: coord_x = noc_instance ? 8 : 1; break;
        case 19: coord_x = noc_instance ? 7 : 2; break;
        case 20: coord_x = noc_instance ? 6 : 3; break;
        case 21: coord_x = noc_instance ? 5 : 4; break;
        case 22: coord_x = noc_instance ? 3 : 6; break;
        case 23: coord_x = noc_instance ? 2 : 7; break;
        case 24: coord_x = noc_instance ? 1 : 8; break;
        case 25: coord_x = noc_instance ? 0 : 9; break;
    }
    switch (coord_y) {
        case 16: coord_y = noc_instance ? 11 : 0; break;
        case 17: coord_y = noc_instance ? 5 : 6; break;
        case 18: coord_y = noc_instance ? 10 : 1; break;
        case 19: coord_y = noc_instance ? 9 : 2; break;
        case 20: coord_y = noc_instance ? 8 : 3; break;
        case 21: coord_y = noc_instance ? 7 : 4; break;
        case 22: coord_y = noc_instance ? 6 : 5; break;
        case 23: coord_y = noc_instance ? 4 : 7; break;
        case 24: coord_y = noc_instance ? 3 : 8; break;
        case 25: coord_y = noc_instance ? 2 : 9; break;
        case 26: coord_y = noc_instance ? 1 : 10; break;
        case 27: coord_y = noc_instance ? 0 : 11; break;
    }
    TTSIM_VERIFY((coord_x <= 9) && (coord_y <= 11), UnimplementedFunctionality, "invalid coordinate after virtualization %d,%d", coord_x, coord_y);
    if (noc_instance) {
        coord_x = 9 - coord_x;
        coord_y = 11 - coord_y;
    }
    return coord_x | (coord_y << 6);
#elif TT_ARCH_VERSION == 1
    if (coord_y <= 1) { // first two rows have no translation at all
        ;
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
#if TT_ARCH_VERSION == 0
        case NOC_REGS_NOC_NODE_ID: {
            uint32_t coord = tile_to_coord(tile_type, tile_id);
            if (noc_instance) {
                coord = (9 | (11 << 6)) - coord;
            }
            // XXX other fields of this register need to be filled in
            uint32_t routing_dir = !noc_instance; // true on NOC0, false on NOC1
            return coord | (10 << 12) | (12 << 19) | (routing_dir << 28);
        }
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_0: return 0x76543210; // entries 0-7, identity
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_1: return 0xFEDCBA98; // entries 8-15, identity
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_2: return noc_instance ? 0x23567849 : 0x76432150; // entries 16-23
        case NOC_REGS_NOC_X_ID_TRANSLATE_TABLE_3: return noc_instance ? 0x00000001 : 0x00000098; // entries 24-31
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_0: return 0x76543210; // entries 0-7, identity
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_1: return 0xFEDCBA98; // entries 8-15, identity
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_2: return noc_instance ? 0x46789A5B : 0x75432160; // entries 16-23
        case NOC_REGS_NOC_Y_ID_TRANSLATE_TABLE_3: return noc_instance ? 0x00000123 : 0x0000BA98; // entries 24-31
        case NOC_REGS_NOC_ID_LOGICAL: {
            uint32_t tile_x = tile_id % 8;
            uint32_t tile_y = tile_id / 8;
            if constexpr (tile_type == 'T') {
                return (18 + tile_x) | ((18 + tile_y) << 6);
            } else {
                if (tile_id & 1) {
                    tile_x = tile_x / 2;
                } else {
                    tile_x = (6 - tile_x) / 2 + 4;
                }
                return (18 + tile_x) | ((16 + tile_y) << 6);
            }
        }
#elif TT_ARCH_VERSION == 1
        case NOC_REGS_NOC_NODE_ID: {
            uint32_t coord = tile_to_coord(tile_type, tile_id);
            if (noc_instance) {
                coord = (16 | (11 << 6)) - coord;
            }
            // XXX other fields of this register need to be filled in
            uint32_t routing_dir = !noc_instance; // true on NOC0, false on NOC1
            return coord | (17 << 12) | (12 << 19) | (routing_dir << 28);
        }
        case NOC_REGS_NOC_ID_LOGICAL:
            if constexpr (tile_type == 'T') { // Without harvesting, logical == physical for Tensix tiles
                return tile_to_coord(tile_type, tile_id);
            } else { // Ethernet tiles use logical coordinates (20+tile_id, 25) which the NIU translates to physical
                TTSIM_VERIFY(tile_id < 12, UnimplementedFunctionality, "NOC_ID_LOGICAL: tile_type=%c tile_id=%d", tile_type, tile_id);
                return (20 + tile_id) | (25 << 6);
            }
#endif
        case NOC_REGS_NIU_CFG_0: return p_tile->niu_cfg_0[noc_instance];
        case NOC_REGS_ROUTER_CFG_0: return p_tile->router_cfg_0[noc_instance];
        case NOC_REGS_ROUTER_CFG_1: return p_tile->router_cfg_1[noc_instance];
        case NOC_REGS_ROUTER_CFG_2: return p_tile->router_cfg_2[noc_instance];
        case NOC_REGS_ROUTER_CFG_3: return p_tile->router_cfg_3[noc_instance];
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
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
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
    src_coordinate = remap_virtual_coordinate(noc_instance, src_coordinate);
    dst_coordinate = remap_virtual_coordinate(noc_instance, dst_coordinate);
    dst_coordinate_start = remap_virtual_coordinate(noc_instance, dst_coordinate_start);
    // XXX Apparently on WH, this writes the translated coords back to the ADDR_MID registers -- not the most straightforward
    auto [src_tile_type, src_tile_id] = coord_to_tile(src_coordinate);
    auto [dst_tile_type, dst_tile_id] = coord_to_tile(dst_coordinate);

    if (bits<5,5>(noc_ctrl)) { // multicast -- always L1 to L1 right now
        TTSIM_VERIFY((noc_at_len_be >= 1) && (noc_at_len_be <= NOC_MAX_PACKET_SIZE), UnsupportedFunctionality, "multicast: noc_at_len_be=%d", noc_at_len_be);
        TTSIM_VERIFY(src_tile_type == tile_type, UnimplementedFunctionality, "multicast: src_tile_type=%c", src_tile_type);
        TTSIM_VERIFY(src_tile_id == tile_id, UnimplementedFunctionality, "multicast: src_tile_id=0x%x", src_tile_id); // write must be from current tile
        TTSIM_VERIFY(src_addr + noc_at_len_be <= sizeof(p_tile->sram), UnsupportedFunctionality, "multicast: src_addr=0x%llx", src_addr);
        TTSIM_VERIFY(dst_addr + noc_at_len_be <= TENSIX_SRAM_SIZE, UnsupportedFunctionality, "multicast: dst_addr=0x%llx", dst_addr);
        TTSIM_VERIFY((src_addr & 15) == (dst_addr & 15), UnimplementedFunctionality, "multicast: alignment of src_addr=0x%llx and dst_addr=0x%llx does not match",
            src_addr, dst_addr);

        uint32_t dst_start_x = dst_coordinate_start & 63;
        uint32_t dst_start_y = dst_coordinate_start >> 6;
        uint32_t dst_end_x = dst_coordinate & 63;
        uint32_t dst_end_y = dst_coordinate >> 6;
        // XXX This is somewhat wrong, but it accounts for our (1) missing implementation of coordinate virtualization and
        // (2) lack of handling of spans that wrap around (i.e. start > end).  For NOC1, the virtualization will flip the
        // order of start and end, and then the span will go in the direction we support.
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
                if ((p_tile->router_cfg_1[noc_instance] & (1ull << dst_x)) ||
                    (p_tile->router_cfg_3[noc_instance] & (1ull << dst_y))) {
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
        TTSIM_VERIFY(noc_at_len_be == 0x107C, UnimplementedFunctionality, "atomic: noc_at_len_be=0x%x", noc_at_len_be); // NOC_AT_WRAP(31) | NOC_AT_INS_INCR_GET
        TTSIM_VERIFY(!(src_addr & 15), UnimplementedFunctionality, "atomic: misaligned src_addr=0x%llx", src_addr);
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
        TTSIM_VERIFY((dst_tile_type == 'D') || (dst_tile_type == 'T') || (dst_tile_type == 'E') || (dst_tile_type == 'P'),
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
        TTSIM_VERIFY((src_tile_type == 'D') || (src_tile_type == 'T') || (src_tile_type == 'E') || (src_tile_type == 'P'),
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
        case NOC_REGS_NIU_CFG_0: p_tile->niu_cfg_0[noc_instance] = data; break;
        case NOC_REGS_ROUTER_CFG_0: p_tile->router_cfg_0[noc_instance] = data; break;
        case NOC_REGS_ROUTER_CFG_1:
            TTSIM_VERIFY((data & NONTENSIX_COL_MASK) == NONTENSIX_COL_MASK, UndefinedBehavior, "router_cfg_1=0x%x", data); // cannot clear non-tensix cols
            p_tile->router_cfg_1[noc_instance] = data;
            break;
        case NOC_REGS_ROUTER_CFG_2: p_tile->router_cfg_2[noc_instance] = data; break;
        case NOC_REGS_ROUTER_CFG_3:
            TTSIM_VERIFY((data & NONTENSIX_ROW_MASK) == NONTENSIX_ROW_MASK, UndefinedBehavior, "router_cfg_3=0x%x", data); // cannot clear non-tensix rows
            p_tile->router_cfg_3[noc_instance] = data;
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
static void tensix_inst_wr32(uint32_t tile_id, uint32_t riscv_id, uint32_t offset, uint32_t data) {
    TTSIM_VERIFY(!offset, UnimplementedFunctionality, "offset=0x%x", offset);
    TensixTile *p_tile = &g_t_tiles[tile_id];
    if (riscv_id == RV32_ID_BRISC) {
        return tensix_push_inst(&p_tile->tensix[0], 0, data, true); // this is the aperture for use pipe 0, and bypass MOP expander
    }
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
                TTSIM_ERROR(UnimplementedFunctionality, "eth_txq_cmd=0x%x", data);
            } else if (data == 4) {
#if TT_ARCH_VERSION == 0
                TTSIM_VERIFY(queue_id != 1, UnimplementedFunctionality, "queue_id=%d", queue_id);
#else
                TTSIM_VERIFY(queue_id != 2, UnimplementedFunctionality, "queue_id=%d", queue_id);
#endif
                TTSIM_VERIFY(!(p_tile->eth_txq_dest_addr[queue_id] & 3), UnimplementedFunctionality, "eth_txq_dest_addr=0x%x", p_tile->eth_txq_dest_addr[queue_id]);
                TTSIM_ERROR(UnimplementedFunctionality, "eth_txq_cmd=0x%x", data);
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
#elif TT_ARCH_VERSION == 1
        case ETH_CTRL_REGS_MAC_RX_ADDR_ROUTING: return p_tile->eth_mac_rx_addr_routing;
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
            p_tile->erisc_mac_0 = data; break;
        case ETH_MAC_REGS_ERISC_MAC_1:
            TTSIM_VERIFY((data == 6) || (data == 7), UnimplementedFunctionality, "ERISC_MAC_1=0x%x", data);
            p_tile->erisc_mac_1 = data; break;
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
        case TENSIX_REGFILE_BASE ... TENSIX_REGFILE_LIMIT:
            tensix_regfile_wr32(tile_id, riscv_id, addr - TENSIX_REGFILE_BASE, data);
            return true;
        case TENSIX_INST_BASE ... TENSIX_INST_LIMIT:
            tensix_inst_wr32(tile_id, riscv_id, addr - TENSIX_INST_BASE, data);
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
                p_tile->rv32[1].pc = p_tile->subordinate_ierisc_reset_pc;
                ttsim_rv32_set_core_active('E', tile_id, 1, true);
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
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_IERISC_RESET_PC: p_tile->ierisc_reset_pc = data; return true;
        case ETH_RISC_CTRL_BASE + ETH_RISC_CTRL_SUBORDINATE_IERISC_RESET_PC: p_tile->subordinate_ierisc_reset_pc = data; return true;
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
    // XXX Note that both of these paths assume outbound iATU is configured as passthrough
    // This is believed to be configured by FW and not KMD, so will need to be faked out in ttsim
    // XXX Need to do more research on this topic and add support for configurable outbound iATU
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY((addr >= 0x800000000ull) && (addr + size <= 0x8FFFE0000ull),
        UnimplementedFunctionality, "addr=0x%llx size=%d", addr, size);
    return addr - 0x800000000ull;
#elif TT_ARCH_VERSION == 1
    // https://github.com/tenstorrent/tt-isa-documentation/blob/main/BlackholeA0/PCIExpressTile/README.md#noc-to-host-264-bytes
    constexpr uint64_t WINDOW_BASE = 1ull << 60;
    constexpr uint64_t WINDOW_SIZE = 1ull << 58;
    TTSIM_VERIFY((addr >= WINDOW_BASE) && (addr + size <= WINDOW_BASE + WINDOW_SIZE),
        UnimplementedFunctionality, "addr=0x%llx size=%d", addr, size);
    return addr - WINDOW_BASE;
#else
    TTSIM_ERROR_NOFMT(MissingSpecification);
#endif
}

void tile_rd_bytes(uint32_t coord, uint64_t addr, void *p, uint32_t size) {
    auto [tile_type, tile_id] = coord_to_tile(coord);
    if (tile_type == 'D') {
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
        libttsim_pci_dma_mem_rd_bytes(translate_pci_dma_addr(addr, size), p, size);
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
                if (data && !mem_rd<uint32_t>(&g_e_tiles[tile_id].sram[addr])) { // Start active erisc core
                    g_e_tiles[tile_id].rv32[0].pc = 0x9040; // FIRMWARE_BASE
                    g_e_tiles[tile_id].rv32[0].x_regs[2] = 0xFFB01000;
                    ttsim_rv32_set_core_active('E', tile_id, 0, true);
                } else if (!data && mem_rd<uint32_t>(&g_e_tiles[tile_id].sram[addr])) { // Stop active erisc core
                    ttsim_rv32_set_core_active('E', tile_id, 0, false);
                }
            }
#elif TT_ARCH_VERSION == 1
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
        libttsim_pci_dma_mem_wr_bytes(translate_pci_dma_addr(addr, size), p, size);
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "tile_type=%c", tile_type);
    }
}
