// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Central simulator header: shared types, global state, and the TTSimErrorCategory contract.
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <bit>
#include <iterator>
#include "config.h"
#include "tensix_regs.h" // generated
#include "tile_regs.h" // generated

enum class TTSimErrorCategory {
    UndefinedBehavior, // as defined in tt-isa-documentation
    UnpredictableValueUsed, // as defined in tt-isa-documentation
    NonContractualBehavior, // as defined in tt-isa-documentation
    AssertionFailure, // internal bug in simulator
    MissingSpecification, // tt-isa-documentation missing or inadequate to implement feature
    UntestedFunctionality, // implemented, but inadequate test coverage to enable yet
    UnimplementedFunctionality, // not implemented yet
    UnsupportedFunctionality, // planned to never be implemented
    SystemError, // OS errors and similar
    ConfigurationError, // bad command line options, env vars, configuration files, etc.
};

#define TTSIM_ERROR(category, fmt, ...) \
    ttsim_error(TTSimErrorCategory::category, __func__, fmt "\n", ##__VA_ARGS__)

#define TTSIM_ERROR_NOFMT(category) \
    ttsim_error(TTSimErrorCategory::category, __func__, nullptr)

#define TTSIM_VERIFY(cond, category, fmt, ...) \
    do { \
        if (!(cond)) [[unlikely]] { \
            ttsim_error(TTSimErrorCategory::category, __func__, fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

// Use ASSERT instead of VERIFY only in cases where a sufficiently smart compiler could
// prove that the condition will never be violated.
#if defined(DEBUG)
#define TTSIM_ASSERT(cond) \
    do { \
        if (!(cond)) [[unlikely]] { \
            ttsim_error(TTSimErrorCategory::AssertionFailure, __func__, "%s\n", #cond); \
        } \
    } while (0)
#else
#define TTSIM_ASSERT(cond) do {} while (0)
#endif

// Bitfield extracts analogous to Verilog x[hi:lo] notation
// Avoiding actual "class T" template arg to optimize compile times
template<uint32_t hi, uint32_t lo>
[[nodiscard]] constexpr uint32_t bits(uint32_t x) {
    static_assert((lo <= hi) && (hi < 32));
    return ((x >> lo) & ((2u << (hi - lo)) - 1u));
}
template<uint32_t hi, uint32_t lo>
[[nodiscard]] constexpr uint64_t bits(uint64_t x) {
    static_assert((lo <= hi) && (hi < 64));
    return ((x >> lo) & ((2ull << (hi - lo)) - 1ull));
}
template<uint32_t hi, uint32_t lo>
[[nodiscard]] constexpr int32_t signed_bits(uint32_t x) {
    static_assert((lo <= hi) && (hi < 32));
    return (int32_t(x << (31 - hi)) >> (31 - hi + lo));
}
template<uint32_t hi, uint32_t lo>
[[nodiscard]] constexpr int64_t signed_bits(uint64_t x) {
    static_assert((lo <= hi) && (hi < 64));
    return (int64_t(x << (63 - hi)) >> (63 - hi + lo));
}

template<int N> struct int_types;
template<> struct int_types<32> { using int_t = int32_t; using uint_t = uint32_t; };
template<> struct int_types<64> { using int_t = int64_t; using uint_t = uint64_t; };
template<> struct int_types<128> { using int_t = __int128; using uint_t = unsigned __int128; };

// Wrappers to provide value semantics without UB (similar to read/write_unaligned in Rust)
template<class T> [[nodiscard]] T mem_rd(const void *p) {
    T data;
    memcpy(&data, p, sizeof(data));
    return data;
}
template<class T> void mem_wr(void *p, T data) {
    memcpy(p, &data, sizeof(data));
}

struct Rv32HartState {
    char tile_type;
    uint32_t tile_id;
    uint32_t riscv_id;
    uint32_t pc;
    uint32_t x_regs[32];
#if TT_ARCH_VERSION >= 1
    uint32_t f_regs[32];
    uint32_t chicken_bits;
#endif

    uint8_t *p_sram;
    uint8_t *p_local_mem;
    uint32_t sram_size;
    uint32_t local_mem_base;
    uint32_t local_mem_size;
};

struct Rv64HartState {
    char tile_type;
    uint32_t tile_id;
    uint32_t riscv_id;
    uint64_t pc;
    uint64_t x_regs[32];

    uint64_t mstatus;
    uint64_t mie;
    uint64_t mtvec;

    uint8_t *p_sram;
    uint32_t sram_size;
};

#define TENSIX_INST_PIPES 3
#define TENSIX_INST_FIFO_SIZE 512
#define SRC_ROWS 64
#define DST_ROWS 1024
#define ROW_SIZE 16
#define CC_STACK_SIZE 8
#define ADC_X_MASK 0x3FFFF // 18 bits
#define ADC_Y_MASK 0x1FFF // 13 bits
#define ADC_Z_MASK 0xFF // 8 bits
#define ADC_W_MASK 0xFF // 8 bits
#if TT_ARCH_VERSION == 1
#define TENSIX_CFG_STATE_SIZE 56
#define TENSIX_THD_STATE_SIZE 68
#else
#define TENSIX_CFG_STATE_SIZE 47
#define TENSIX_THD_STATE_SIZE 57
#endif

struct TensixThreadState {
    THREAD_CFG0_REG_UNION()
    THREAD_CFG1_REG_UNION()
#if TT_ARCH_VERSION == 1
    THREAD_CFG2_REG_UNION()
    THREAD_CFG3_REG_UNION()
    THREAD_CFG5_REG_UNION()
    THREAD_CFG7_REG_UNION()
    THREAD_CFG11_REG_UNION()
    THREAD_CFG12_REG_UNION()
    THREAD_CFG13_REG_UNION()
    THREAD_CFG14_REG_UNION()
    THREAD_CFG15_REG_UNION()
    THREAD_CFG16_REG_UNION()
    THREAD_CFG17_REG_UNION()
    THREAD_CFG18_REG_UNION()
    THREAD_CFG19_REG_UNION()
    THREAD_CFG28_REG_UNION()
    THREAD_CFG29_REG_UNION()
    THREAD_CFG30_REG_UNION()
    THREAD_CFG31_REG_UNION()
    THREAD_CFG32_REG_UNION()
    THREAD_CFG33_REG_UNION()
    THREAD_CFG34_REG_UNION()
    THREAD_CFG35_REG_UNION()
    THREAD_CFG37_REG_UNION()
    THREAD_CFG38_REG_UNION()
    THREAD_CFG39_REG_UNION()
    THREAD_CFG40_REG_UNION()
    THREAD_CFG41_REG_UNION()
    THREAD_CFG47_REG_UNION()
    THREAD_CFG48_REG_UNION()
    THREAD_CFG49_REG_UNION()
    THREAD_CFG50_REG_UNION()
    THREAD_CFG51_REG_UNION()
    THREAD_CFG52_REG_UNION()
    THREAD_CFG53_REG_UNION()
    THREAD_CFG54_REG_UNION()
#else
    THREAD_CFG2_REG_UNION()
    THREAD_CFG3_REG_UNION()
    THREAD_CFG5_REG_UNION()
    THREAD_CFG6_REG_UNION()
    THREAD_CFG7_REG_UNION()
    THREAD_CFG9_REG_UNION()
    THREAD_CFG11_REG_UNION()
    THREAD_CFG13_REG_UNION()
    THREAD_CFG15_REG_UNION()
    THREAD_CFG17_REG_UNION()
    THREAD_CFG19_REG_UNION()
    THREAD_CFG21_REG_UNION()
    THREAD_CFG23_REG_UNION()
    THREAD_CFG24_REG_UNION()
    THREAD_CFG25_REG_UNION()
    THREAD_CFG26_REG_UNION()
    THREAD_CFG27_REG_UNION()
    THREAD_CFG28_REG_UNION()
    THREAD_CFG29_REG_UNION()
    THREAD_CFG30_REG_UNION()
    THREAD_CFG31_REG_UNION()
    THREAD_CFG32_REG_UNION()
    THREAD_CFG33_REG_UNION()
    THREAD_CFG34_REG_UNION()
    THREAD_CFG39_REG_UNION()
    THREAD_CFG48_REG_UNION()
    THREAD_CFG49_REG_UNION()
    THREAD_CFG50_REG_UNION()
    THREAD_CFG51_REG_UNION()
    THREAD_CFG52_REG_UNION()
    THREAD_CFG53_REG_UNION()
    THREAD_CFG54_REG_UNION()
    THREAD_CFG55_REG_UNION()
#endif
};

struct TensixConfigState {
    CFG0_REG_UNION()
    CFG1_REG_UNION()
    CFG2_REG_UNION()
    CFG3_REG_UNION()
#if TT_ARCH_VERSION == 0
    CFG8_REG_UNION()
    CFG9_REG_UNION()
    CFG14_REG_UNION()
    CFG16_REG_UNION()
    CFG17_REG_UNION()
    CFG20_REG_UNION()
    CFG21_REG_UNION()
    CFG22_REG_UNION()
    CFG23_REG_UNION()
    CFG24_REG_UNION()
    CFG25_REG_UNION()
    CFG26_REG_UNION()
    CFG27_REG_UNION()
    CFG41_REG_UNION()
    CFG44_REG_UNION()
    CFG45_REG_UNION()
    CFG46_REG_UNION()
    CFG47_REG_UNION()
    CFG49_REG_UNION()
    CFG52_REG_UNION()
    uint32_t cfg53; // XXX 128-bit register not supported by our flow
    CFG56_REG_UNION()
    CFG57_REG_UNION()
    CFG58_REG_UNION()
    CFG59_REG_UNION()
    CFG60_REG_UNION()
    CFG61_REG_UNION()
    CFG64_REG_UNION()
    CFG65_REG_UNION()
    CFG72_REG_UNION()
    CFG74_REG_UNION()
    CFG80_REG_UNION()
    CFG81_REG_UNION()
    CFG84_REG_UNION()
    CFG85_REG_UNION()
    CFG86_REG_UNION()
    CFG87_REG_UNION()
    CFG92_REG_UNION()
    uint32_t cfg93; // XXX 128-bit register not supported by our flow
    CFG96_REG_UNION()
    CFG97_REG_UNION()
    CFG98_REG_UNION()
    CFG99_REG_UNION()
    CFG100_REG_UNION()
    CFG101_REG_UNION()
    CFG104_REG_UNION()
    CFG105_REG_UNION()
    CFG120_REG_UNION()
    CFG121_REG_UNION()
    CFG124_REG_UNION()
    CFG125_REG_UNION()
    CFG126_REG_UNION()
    CFG127_REG_UNION()
#elif TT_ARCH_VERSION == 1
    CFG12_REG_UNION()
    CFG13_REG_UNION()
    CFG14_REG_UNION()
    CFG15_REG_UNION()
    CFG17_REG_UNION()
    CFG18_REG_UNION()
    CFG20_REG_UNION()
    CFG21_REG_UNION()
    CFG24_REG_UNION()
    CFG25_REG_UNION()
    CFG28_REG_UNION()
    CFG50_REG_UNION()
    CFG56_REG_UNION()
    CFG57_REG_UNION()
    CFG58_REG_UNION()
    CFG59_REG_UNION()
    CFG62_REG_UNION()
    CFG64_REG_UNION()
    uint32_t cfg65; // XXX 128-bit register not supported by our flow
    CFG68_REG_UNION()
    CFG69_REG_UNION()
    CFG70_REG_UNION()
    CFG71_REG_UNION()
    CFG72_REG_UNION()
    CFG73_REG_UNION()
    CFG76_REG_UNION()
    CFG77_REG_UNION()
    CFG84_REG_UNION()
    CFG86_REG_UNION()
    CFG92_REG_UNION()
    CFG93_REG_UNION()
    CFG97_REG_UNION()
    CFG112_REG_UNION()
    uint32_t cfg113; // XXX 128-bit register not supported by our flow
    CFG117_REG_UNION()
    CFG120_REG_UNION()
    CFG121_REG_UNION()
    CFG124_REG_UNION()
    CFG125_REG_UNION()
    CFG140_REG_UNION()
    CFG141_REG_UNION()
    CFG145_REG_UNION()
#endif
};

struct TensixAddrCtrl {
    uint32_t ch0_x;
    uint32_t ch0_y;
    uint32_t ch0_z;
    uint32_t ch0_w;
    uint32_t ch1_x;
    uint32_t ch1_y;
    uint32_t ch1_z;
    uint32_t ch1_w;
    uint32_t ch0_x_cr;
    uint32_t ch0_y_cr;
    uint32_t ch0_z_cr;
    uint32_t ch0_w_cr;
    uint32_t ch1_x_cr;
    uint32_t ch1_y_cr;
    uint32_t ch1_z_cr;
    uint32_t ch1_w_cr;
};

struct TensixState {
    uint32_t tile_id;

    uint32_t inst_pipes_active;
    uint32_t inst[TENSIX_INST_PIPES][TENSIX_INST_FIFO_SIZE];
    uint32_t inst_rd_ptr[TENSIX_INST_PIPES];
    uint32_t inst_wr_ptr[TENSIX_INST_PIPES];

    uint16_t mop_zmask_hi16[TENSIX_INST_PIPES];
    uint32_t mop_cfg[TENSIX_INST_PIPES][9];
    uint32_t replay_buf[TENSIX_INST_PIPES][32];
    uint32_t replay_index[TENSIX_INST_PIPES];
    uint32_t replay_left[TENSIX_INST_PIPES];
    bool replay_execute_while_loading[TENSIX_INST_PIPES];

    uint8_t mutex[TT_ARCH_VERSION ? 5 : 8]; // odd, but per docs, BH has fewer mutexes than WH
    uint8_t sem[8];
    uint8_t sem_max[8];

    uint32_t dma_regs[TENSIX_INST_PIPES][64];
    uint32_t src_a[2][SRC_ROWS][ROW_SIZE]; // only 19 real bits per cell; make sure low 13 bits are always zero
    uint32_t src_b[2][SRC_ROWS][ROW_SIZE]; // only 19 real bits per cell; make sure low 13 bits are always zero
    uint16_t dst[DST_ROWS][ROW_SIZE];
    bool dst_row_valid[DST_ROWS]; // would be nice to make this a bitmask, but this is easier for bringup
    uint32_t src_a_valid, src_a_unpack_bank, src_a_matrix_bank;
    uint32_t src_b_valid, src_b_unpack_bank, src_b_matrix_bank;
#if TT_ARCH_VERSION >= 1
    uint32_t src_a_format[2], src_b_format[2];
#endif
    uint32_t src_a_rwc[TENSIX_INST_PIPES];
    uint32_t src_a_rwc_cr[TENSIX_INST_PIPES];
    uint32_t src_b_rwc[TENSIX_INST_PIPES];
    uint32_t src_b_rwc_cr[TENSIX_INST_PIPES];
    uint32_t dst_rwc[TENSIX_INST_PIPES];
    uint32_t dst_rwc_cr[TENSIX_INST_PIPES];
    uint32_t bias[TENSIX_INST_PIPES];
    uint32_t fidelity[TENSIX_INST_PIPES];
    uint32_t l_regs[16][32]; // only 8 fully usable regs, but 16 sources easier to implement this way
    uint32_t lane_config;
    uint32_t prng_state[32];
    uint32_t load_macro_instruction_template[4];
    uint32_t load_macro_sequence[4];
    uint32_t load_macro_misc;
    bool dst_32bit_addr_en;
    bool cc_en;
    uint32_t cc;
    bool cc_en_stack[CC_STACK_SIZE];
    uint32_t cc_stack[CC_STACK_SIZE];
    uint32_t cc_sp;

    // Tensix cfg registers
    TensixThreadState thread[TENSIX_INST_PIPES];
    TensixAddrCtrl addr_ctrl[TENSIX_INST_PIPES][3]; // [pipe][unpack0, unpack1, pack]
    uint32_t packer_dst_addr[4];
    uint32_t packer_dst_exp_addr[4];
    bool packer_dst_addr_valid;
    TensixConfigState config[2];
#if TT_ARCH_VERSION == 0
    CFG152_REG_UNION()
    CFG153_REG_UNION()
    CFG154_REG_UNION()
    CFG155_REG_UNION()
#elif TT_ARCH_VERSION == 1
    CFG180_REG_UNION()
    CFG181_REG_UNION()
    CFG182_REG_UNION()
    CFG183_REG_UNION()
    CFG209_REG_UNION()
    CFG210_REG_UNION()
    CFG211_REG_UNION()
    CFG220_REG_UNION()
#endif
};

struct TensixTile {
    uint32_t rv32_cores_active;
#if RV64_CORES_PER_T_TILE
    uint32_t rv64_cores_active;
#endif
    Rv32HartState rv32[RV32_CORES_PER_T_TILE];
#if RV64_CORES_PER_T_TILE
    Rv64HartState rv64[RV64_CORES_PER_T_TILE];
#endif
    TensixState tensix[TENSIX_CORES_PER_T_TILE];

    uint8_t sram[TENSIX_SRAM_SIZE];
    uint8_t rv32_local_ram[RV32_CORES_PER_T_TILE][BRISC_LOCAL_MEM_SIZE]; // different size for TRISC, use larger B/NCRISC size for simplicity
#if TT_ARCH_VERSION == 0
    uint8_t ncrisc_iram[RV32_IRAM_SIZE];
    uint32_t tdma_xmov_src_addr;
    uint32_t tdma_xmov_size;
#endif

    uint32_t noc_targ_addr_lo[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_targ_addr_mid[NUM_NOCS][NUM_NOC_CMD_BUFS];
#if TT_ARCH_VERSION >= 1
    uint32_t noc_targ_addr_hi[NUM_NOCS][NUM_NOC_CMD_BUFS];
#endif
    uint32_t noc_ret_addr_lo[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_ret_addr_mid[NUM_NOCS][NUM_NOC_CMD_BUFS];
#if TT_ARCH_VERSION >= 1
    uint32_t noc_ret_addr_hi[NUM_NOCS][NUM_NOC_CMD_BUFS];
#endif
    uint32_t noc_packet_tag[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_ctrl[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_at_len_be[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_at_data[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t niu_cfg_0[NUM_NOCS];
    uint32_t router_cfg_0[NUM_NOCS];
    uint32_t router_cfg_1[NUM_NOCS];
    uint32_t router_cfg_2[NUM_NOCS];
    uint32_t router_cfg_3[NUM_NOCS];
    uint32_t niu_mst_atomic_resp_received[NUM_NOCS];
    uint32_t niu_mst_wr_ack_received[NUM_NOCS];
    uint32_t niu_mst_rd_resp_received[NUM_NOCS];
    uint32_t niu_mst_posted_atomic_sent[NUM_NOCS];
    uint32_t niu_mst_nonposted_wr_req_sent[NUM_NOCS];
    uint32_t niu_mst_posted_wr_req_sent[NUM_NOCS];
    uint8_t niu_mst_reqs_outstanding[NUM_NOCS][NUM_NOC_TRANSACTION_IDS];
    uint32_t overlay_stream_remote_src[TENSIX_NUM_NOC_OVERLAY_STREAMS];
    uint32_t overlay_stream_remote_dest_buf_start[TENSIX_NUM_NOC_OVERLAY_STREAMS];
    uint32_t overlay_stream_remote_dest_buf_size[TENSIX_NUM_NOC_OVERLAY_STREAMS];
    uint32_t overlay_stream_remote_dest_buf_space_available[TENSIX_NUM_NOC_OVERLAY_STREAMS];
    uint32_t dbg_array_rd_en;
    uint32_t dbg_array_rd_data;
    uint32_t soft_reset_0;
    uint32_t trisc0_reset_pc;
    uint32_t trisc1_reset_pc;
    uint32_t trisc2_reset_pc;
    uint32_t trisc_reset_pc_override;
    uint32_t ncrisc_reset_pc;
    uint32_t ncrisc_reset_pc_override;
    bool mailbox_has_data[4][4]; // [from][to]
    uint32_t mailbox_data[4][4]; // [from][to]
};

struct EthTile {
    uint8_t sram[ETH_SRAM_SIZE];
    Rv32HartState rv32[RV32_CORES_PER_E_TILE];
    uint8_t rv32_local_ram[RV32_CORES_PER_E_TILE][ERISC_LOCAL_MEM_SIZE];
#if TT_ARCH_VERSION == 0
    uint8_t erisc_iram[RV32_IRAM_SIZE];
    uint32_t erisc_iram_load;
    uint32_t erisc_mac_0;
    uint32_t erisc_mac_1;
#endif
    uint32_t noc_targ_addr_lo[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_targ_addr_mid[NUM_NOCS][NUM_NOC_CMD_BUFS];
#if TT_ARCH_VERSION == 1
    uint32_t noc_targ_addr_hi[NUM_NOCS][NUM_NOC_CMD_BUFS];
#endif
    uint32_t noc_ret_addr_lo[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_ret_addr_mid[NUM_NOCS][NUM_NOC_CMD_BUFS];
#if TT_ARCH_VERSION == 1
    uint32_t noc_ret_addr_hi[NUM_NOCS][NUM_NOC_CMD_BUFS];
#endif
    uint32_t noc_packet_tag[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_ctrl[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_at_len_be[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t noc_at_data[NUM_NOCS][NUM_NOC_CMD_BUFS];
    uint32_t niu_cfg_0[NUM_NOCS];
    uint32_t router_cfg_0[NUM_NOCS];
    uint32_t router_cfg_1[NUM_NOCS];
    uint32_t router_cfg_2[NUM_NOCS];
    uint32_t router_cfg_3[NUM_NOCS];
    uint32_t niu_mst_atomic_resp_received[NUM_NOCS];
    uint32_t niu_mst_wr_ack_received[NUM_NOCS];
    uint32_t niu_mst_rd_resp_received[NUM_NOCS];
    uint32_t niu_mst_posted_atomic_sent[NUM_NOCS];
    uint32_t niu_mst_nonposted_wr_req_sent[NUM_NOCS];
    uint32_t niu_mst_posted_wr_req_sent[NUM_NOCS];
    uint8_t niu_mst_reqs_outstanding[NUM_NOCS][NUM_NOC_TRANSACTION_IDS];
    uint32_t overlay_stream_remote_src[ETH_NUM_NOC_OVERLAY_STREAMS];
    uint32_t overlay_stream_remote_dest_buf_start[ETH_NUM_NOC_OVERLAY_STREAMS];
    uint32_t overlay_stream_remote_dest_buf_size[ETH_NUM_NOC_OVERLAY_STREAMS];
    uint32_t overlay_stream_remote_dest_buf_space_available[ETH_NUM_NOC_OVERLAY_STREAMS];
    uint32_t soft_reset_0;
    uint32_t eth_txq_control[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txq_cmd[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txq_status[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txq_transfer_start_addr[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txq_transfer_size_bytes[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txq_dest_addr[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txq_remote_reg_data[ETH_NUM_TX_RX_QUEUES];
#if TT_ARCH_VERSION == 0
    uint32_t eth_txq_dest_mac_addr_hi[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txq_dest_mac_addr_lo[ETH_NUM_TX_RX_QUEUES];
#endif
#if TT_ARCH_VERSION == 1
    uint32_t eth_txq_txpkt_cfg_sel_sw[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txq_txpkt_cfg_sel_hw[ETH_NUM_TX_RX_QUEUES];
    uint32_t eth_txpkt_cfg_mac_da_lo[ETH_NUM_TX_HEADER_TABLE_ENTRIES];
    uint32_t eth_txpkt_cfg_mac_da_hi[ETH_NUM_TX_HEADER_TABLE_ENTRIES];
    uint32_t eth_mac_rx_routing;
    uint32_t eth_mac_rx_addr_routing;
#endif
    uint32_t eth_rxq_control[ETH_NUM_TX_RX_QUEUES];
    uint32_t ierisc_reset_pc;
#if TT_ARCH_VERSION == 1
    uint32_t subordinate_ierisc_reset_pc;
#endif
};

#define ARC_CSM_SIZE (ARC_XBAR_CSM_LIMIT - ARC_XBAR_CSM_BASE + 1)

struct ArcTile {
#if TT_ARCH_VERSION == 0
    uint8_t csm[ARC_CSM_SIZE];
    uint32_t reset_unit_scratch[8];
    uint32_t arc_misc_cntl;

    uint32_t niu_cfg_0[NUM_NOCS];
#endif
};

struct DramChannel {
    uint8_t *p_mem;
};

extern TensixTile g_t_tiles[NUM_T_TILES];
extern EthTile g_e_tiles[NUM_E_TILES];
extern ArcTile g_a_tile;
extern DramChannel g_dram[NUM_DRAM_CHANNELS];
extern uint64_t g_clock;
extern uint64_t g_rv32_cores_active;

void libttsim_pci_dma_mem_rd_bytes(uint64_t paddr, void *p, uint32_t size);
void libttsim_pci_dma_mem_wr_bytes(uint64_t paddr, const void *p, uint32_t size);
uint64_t libttsim_syscall(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t syscall, uint64_t arg0, uint64_t arg1, uint64_t arg2);

void ttsim_printf(const char *fmt, ...);
[[noreturn, gnu::cold]] void ttsim_error(TTSimErrorCategory category, const char *func, const char *fmt, ...);
void ttsim_init();
void ttsim_exit();
bool ttsim_rv32_get_core_active(char tile_type, uint32_t tile_id, uint32_t rv32_id);
void ttsim_rv32_set_core_active(char tile_type, uint32_t tile_id, uint32_t rv32_id, bool active);
void ttsim_heartbeat();

void rv32_init(Rv32HartState *p_hart, char tile_type, uint32_t tile_id, uint32_t riscv_id);
void rv32_icache_invalidate(Rv32HartState *p_hart);
void rv32_step(Rv32HartState *p_hart);

void rv64_init(Rv64HartState *p_hart, char tile_type, uint32_t tile_id, uint32_t riscv_id);
void rv64_step(Rv64HartState *p_hart);

void tensix_init(TensixState *p_tensix, uint32_t tile_id);
bool tensix_can_push_inst(TensixState *p_tensix, uint32_t pipe);
void tensix_push_inst(TensixState *p_tensix, uint32_t pipe, uint32_t inst, bool bypass_mop_expander);
uint32_t tensix_cfg_rd32(TensixState *p_tensix, uint32_t bank, uint32_t offset);
void tensix_cfg_wr32(TensixState *p_tensix, uint32_t bank, uint32_t offset, uint32_t data);
bool tensix_decode_and_execute(TensixState *p_tensix, uint32_t pipe, uint32_t inst);

void t_tile_init(uint32_t tile_id);
void e_tile_init(uint32_t tile_id);
void a_tile_init();
uint32_t remap_virtual_coordinate(uint32_t noc_instance, uint32_t coord);
// XXX probably turn these into template functions so we can do 8-bit/16-bit MMIOs as well
std::pair<bool, uint32_t> tile_mmio_rd32(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t addr);
std::pair<bool, uint64_t> tile_mmio_rd64(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t addr);
[[nodiscard]] bool tile_mmio_wr32(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t addr, uint32_t data);
[[nodiscard]] bool tile_mmio_wr64(char tile_type, uint32_t tile_id, uint32_t riscv_id, uint64_t addr, uint64_t data);
void tile_rd_bytes(uint32_t coord, uint64_t addr, void *p, uint32_t size);
void tile_wr_bytes(uint32_t coord, uint64_t addr, const void *p, uint32_t size);

template<class T> inline T tile_rd(uint32_t coord, uint64_t addr) {
    T data;
    tile_rd_bytes(coord, addr, &data, sizeof(data));
    return data;
}

template<class T> inline void tile_wr(uint32_t coord, uint64_t addr, T data) {
    tile_wr_bytes(coord, addr, &data, sizeof(data));
}

uint32_t fma_model_wh(uint32_t x, uint32_t y, uint32_t z);
uint32_t fma_model_bh(uint32_t x, uint32_t y, uint32_t z);

#if TT_ARCH_VERSION == 0
#define fma_model fma_model_wh
#else
#define fma_model fma_model_bh
#endif
