// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Tensix functional model: instruction expansion, FPU/SFPU/unpacker/packer execution, per-pipe state.
// https://github.com/tenstorrent/tt-isa-documentation
// https://github.com/tenstorrent/tt-llk/blob/main/tt_llk_wormhole_b0/instructions/assembly.yaml
// https://github.com/tenstorrent/tt-llk/blob/main/tt_llk_blackhole/instructions/assembly.yaml

#include "sim.h"
#include "tensix_decode.h" // generated
#include <algorithm>

#define IS_TENSIX_NOP(inst) (bits<31,24>(inst) == 0x02)
#define TENSIX_NOP (0x02 << 24)

void tensix_init(TensixState *p_tensix, uint32_t tile_id) {
    memset(p_tensix, 0, sizeof(TensixState));
    p_tensix->tile_id = tile_id;
    for (uint32_t lane = 0; lane < 32; lane++) {
        p_tensix->l_regs[8][lane] = 0x3F56594B;
    }
    for (uint32_t lane = 0; lane < 32; lane++) {
        p_tensix->l_regs[10][lane] = 0x3F800000;
    }
    for (uint32_t lane = 0; lane < 32; lane++) {
        p_tensix->l_regs[15][lane] = lane << 1;
    }
    p_tensix->cc_en = true;
    p_tensix->cc = 0xFFFFFFFF;
}

static void tensix_push_inst_fifo(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    if (inst == TENSIX_NOP) {
        return; // eat NOPs here so we don't overflow as quickly on MOP
    }
    uint32_t inst_wr_ptr = p_tensix->inst_wr_ptr[pipe];
    p_tensix->inst[pipe][inst_wr_ptr] = inst;
    inst_wr_ptr = (inst_wr_ptr + 1) % TENSIX_INST_FIFO_SIZE;
    p_tensix->inst_wr_ptr[pipe] = inst_wr_ptr;
    p_tensix->inst_pipes_active |= 1 << pipe;

    TTSIM_VERIFY(inst_wr_ptr != p_tensix->inst_rd_ptr[pipe], UnimplementedFunctionality, "pipe %d inst fifo full", pipe); // require 1 entry always empty
}

static void replay_expander(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    if (p_tensix->replay_left[pipe]) {
        uint32_t index = p_tensix->replay_index[pipe];
        p_tensix->replay_buf[pipe][index] = inst;
        p_tensix->replay_index[pipe] = index + 1;
        p_tensix->replay_left[pipe]--;
        if (p_tensix->replay_execute_while_loading[pipe]) {
            tensix_push_inst_fifo(p_tensix, pipe, inst);
        }
    } else if (bits<31,24>(inst) == 0x04) { // REPLAY
        TTSIM_VERIFY(!(inst & 0xC), NonContractualBehavior, "REPLAY inst=0x%x", inst); // unused bits in encoding
        uint32_t load_mode = bits<0,0>(inst);
        uint32_t execute_while_loading = bits<1,1>(inst);
        if (!load_mode) {
            TTSIM_VERIFY(!execute_while_loading, UnimplementedFunctionality, "execute_while_loading=%d with load_mode=%d", execute_while_loading, load_mode);
        }
        uint32_t len = bits<13,4>(inst);
        TTSIM_VERIFY((len >= 1) && (len <= 32), UnimplementedFunctionality, "len=%d", len);
        uint32_t start_idx = bits<23,14>(inst);
        TTSIM_VERIFY(start_idx <= 28, UnimplementedFunctionality, "start_idx=%d", start_idx);
        TTSIM_VERIFY(start_idx + len <= std::size(p_tensix->replay_buf[pipe]), UndefinedBehavior, "overflow replay_buf: start_idx=%d len=%d", start_idx, len);

        if (load_mode) {
            p_tensix->replay_index[pipe] = start_idx;
            p_tensix->replay_left[pipe] = len;
            p_tensix->replay_execute_while_loading[pipe] = execute_while_loading;
        } else {
            for (uint32_t i = 0; i < len; i++) {
                tensix_push_inst_fifo(p_tensix, pipe, p_tensix->replay_buf[pipe][start_idx + i]);
            }
        }
    } else {
        tensix_push_inst_fifo(p_tensix, pipe, inst);
    }
}

static void mop_cfg(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    TTSIM_VERIFY(!(inst & 0xFF0000), NonContractualBehavior, "MOP_CFG inst=0x%x", inst); // unused bits in encoding
    p_tensix->mop_zmask_hi16[pipe] = bits<15,0>(inst);
}

static void mop_expander(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    uint32_t zlo = bits<15,0>(inst);
    uint32_t loop_count = bits<22,16>(inst);
    uint32_t mop_type = bits<23,23>(inst);
    if (!mop_type) {
        TTSIM_VERIFY(loop_count <= 31, UnsupportedFunctionality, "loop_count=%d (mop_type=%d)", loop_count, mop_type);
        uint32_t zmask = zlo | (uint32_t(p_tensix->mop_zmask_hi16[pipe]) << 16);
        uint32_t flags = p_tensix->mop_cfg[pipe][1];
        TTSIM_VERIFY(flags <= 3, UnsupportedFunctionality, "flags=0x%x", flags);
        for (uint32_t i = 0; i <= loop_count; i++) {
            if (zmask & 1) {
                replay_expander(p_tensix, pipe, p_tensix->mop_cfg[pipe][7]);
                if (flags & 1) {
                    replay_expander(p_tensix, pipe, p_tensix->mop_cfg[pipe][8]);
                }
            } else {
                replay_expander(p_tensix, pipe, p_tensix->mop_cfg[pipe][3]);
                if (flags & 2) {
                    replay_expander(p_tensix, pipe, p_tensix->mop_cfg[pipe][4]);
                    replay_expander(p_tensix, pipe, p_tensix->mop_cfg[pipe][5]);
                    replay_expander(p_tensix, pipe, p_tensix->mop_cfg[pipe][6]);
                }
                if (flags & 1) {
                    replay_expander(p_tensix, pipe, p_tensix->mop_cfg[pipe][2]);
                }
            }
            zmask >>= 1;
        }
        return;
    }

    TTSIM_VERIFY(!zlo, UnimplementedFunctionality, "zlo=%d (mop_type=%d)", zlo, mop_type);
    TTSIM_VERIFY(!loop_count, UnimplementedFunctionality, "loop_count=%d (mop_type=%d)", loop_count, mop_type);
    uint32_t outer_loop_len = p_tensix->mop_cfg[pipe][0];
    TTSIM_VERIFY((outer_loop_len >= 1) && (outer_loop_len <= 32), UnimplementedFunctionality, "outer_loop_len=%d", outer_loop_len);
    uint32_t inner_loop_len = p_tensix->mop_cfg[pipe][1];
    TTSIM_VERIFY((inner_loop_len >= 1) && (inner_loop_len <= 64), UnimplementedFunctionality, "inner_loop_len=%d", inner_loop_len);
    uint32_t start_op = p_tensix->mop_cfg[pipe][2];
    uint32_t end_op0 = p_tensix->mop_cfg[pipe][3];
    uint32_t end_op1 = p_tensix->mop_cfg[pipe][4];
    uint32_t loop_op = p_tensix->mop_cfg[pipe][5];
    uint32_t loop_op1 = p_tensix->mop_cfg[pipe][6];
    uint32_t loop0_last_instr = p_tensix->mop_cfg[pipe][7];
    TTSIM_VERIFY(loop0_last_instr != TENSIX_NOP, UntestedFunctionality, "loop0_last_instr=0x%x", loop0_last_instr);
    uint32_t loop1_last_instr = p_tensix->mop_cfg[pipe][8];
    TTSIM_VERIFY(loop1_last_instr != TENSIX_NOP, UntestedFunctionality, "loop1_last_instr=0x%x", loop1_last_instr);

    uint32_t loop_op_flip = 0;
    if (!IS_TENSIX_NOP(loop_op1)) {
        loop_op_flip = loop_op ^ loop_op1;
        inner_loop_len *= 2;
    }

    for (uint32_t i = 0; i < outer_loop_len; i++) {
        if (!IS_TENSIX_NOP(start_op)) {
            replay_expander(p_tensix, pipe, start_op);
        }
        for (uint32_t j = 0; j < inner_loop_len; j++) {
            if (j + 1 < inner_loop_len) {
                replay_expander(p_tensix, pipe, loop_op);
            } else if (i + 1 < outer_loop_len) {
                replay_expander(p_tensix, pipe, loop1_last_instr);
            } else {
                replay_expander(p_tensix, pipe, loop0_last_instr);
            }
            loop_op ^= loop_op_flip;
        }
        if (!IS_TENSIX_NOP(end_op0)) {
            replay_expander(p_tensix, pipe, end_op0);
            if (!IS_TENSIX_NOP(end_op1)) {
                replay_expander(p_tensix, pipe, end_op1);
            }
        }
    }
}

bool tensix_can_push_inst(TensixState *p_tensix, uint32_t pipe) {
    int32_t fifo_size = p_tensix->inst_wr_ptr[pipe] - p_tensix->inst_rd_ptr[pipe];
    if (fifo_size < 0) {
        fifo_size += TENSIX_INST_FIFO_SIZE;
    }
    return (fifo_size <= 52); // XXX this can easily still overflow in a MOP or REPLAY
}

void tensix_push_inst(TensixState *p_tensix, uint32_t pipe, uint32_t inst, bool bypass_mop_expander) {
    if (bypass_mop_expander) {
        return replay_expander(p_tensix, pipe, inst);
    }
    uint32_t opcode = bits<31,24>(inst);
    switch (opcode) {
        case 0x01: mop_expander(p_tensix, pipe, inst); break; // MOP
        case 0x03: mop_cfg(p_tensix, pipe, inst); break; // MOP_CFG
        default: replay_expander(p_tensix, pipe, inst); break; // everything else is just passthrough
    }
}

static inline uint32_t step_prng(uint32_t state) {
    uint32_t taps = __builtin_popcount(state & 0x80200003);
    return (~taps << 31) | (state >> 1);
}

static void seed_prng(TensixState *p_tensix, uint32_t seed) {
    for (uint32_t i = 0; i < 32; i++) {
        seed = step_prng(seed);
    }
    for (uint32_t i = 0; i < 32; i++) {
        seed = step_prng(seed);
        seed = step_prng(seed);
        p_tensix->prng_state[31 - i] = seed;
    }
}

uint32_t tensix_cfg_rd32(TensixState *p_tensix, uint32_t bank, uint32_t offset) {
    TTSIM_VERIFY(!(offset & 3), AssertionFailure, "misaligned offset=0x%x", offset);
    switch (uint32_t reg = offset / 4) {
#define CFG_REG_BANKED_RD(i) case i: return p_tensix->config[bank].cfg##i;
#define CFG_REG_RD(i) case i: return p_tensix->cfg##i;
        CFG_REG_BANKED_RD(0)
        CFG_REG_BANKED_RD(1)
        CFG_REG_BANKED_RD(2)
        CFG_REG_BANKED_RD(3)
#if TT_ARCH_VERSION == 0
        CFG_REG_BANKED_RD(14)
        CFG_REG_BANKED_RD(20)
        CFG_REG_BANKED_RD(21)
        CFG_REG_BANKED_RD(22)
        CFG_REG_BANKED_RD(23)
        CFG_REG_BANKED_RD(24)
        CFG_REG_BANKED_RD(25)
        CFG_REG_BANKED_RD(26)
        CFG_REG_BANKED_RD(27)
        CFG_REG_BANKED_RD(44)
        CFG_REG_BANKED_RD(45)
        CFG_REG_BANKED_RD(47)
        CFG_REG_BANKED_RD(52)
        CFG_REG_BANKED_RD(53)
        case 54: return 0;
        case 55: return 0;
        CFG_REG_BANKED_RD(56)
        CFG_REG_BANKED_RD(57)
        CFG_REG_BANKED_RD(58)
        CFG_REG_BANKED_RD(59)
        CFG_REG_BANKED_RD(60)
        CFG_REG_BANKED_RD(61)
        case 62: return 0;
        case 63: return 0;
        CFG_REG_BANKED_RD(64)
        CFG_REG_BANKED_RD(65)
        CFG_REG_BANKED_RD(72)
        CFG_REG_BANKED_RD(74)
        CFG_REG_BANKED_RD(84)
        CFG_REG_BANKED_RD(85)
        CFG_REG_BANKED_RD(86)
        CFG_REG_BANKED_RD(87)
        CFG_REG_BANKED_RD(92)
        CFG_REG_BANKED_RD(93)
        case 94: return 0;
        case 95: return 0;
        CFG_REG_BANKED_RD(96)
        CFG_REG_BANKED_RD(97)
        CFG_REG_BANKED_RD(98)
        CFG_REG_BANKED_RD(99)
        CFG_REG_BANKED_RD(100)
        CFG_REG_BANKED_RD(101)
        case 102: return 0;
        case 103: return 0;
        CFG_REG_BANKED_RD(104)
        CFG_REG_BANKED_RD(105)
        CFG_REG_BANKED_RD(124)
        CFG_REG_BANKED_RD(125)
        CFG_REG_BANKED_RD(126)
        CFG_REG_BANKED_RD(127)
#elif TT_ARCH_VERSION == 1
        CFG_REG_BANKED_RD(12)
        CFG_REG_BANKED_RD(13)
        CFG_REG_BANKED_RD(14)
        CFG_REG_BANKED_RD(15)
        CFG_REG_BANKED_RD(17)
        CFG_REG_BANKED_RD(18)
        CFG_REG_BANKED_RD(24)
        CFG_REG_BANKED_RD(28)
        CFG_REG_BANKED_RD(56)
        CFG_REG_BANKED_RD(57)
        CFG_REG_BANKED_RD(59)
        CFG_REG_BANKED_RD(64)
        CFG_REG_BANKED_RD(65)
        case 66: return 0;
        case 67: return 0;
        CFG_REG_BANKED_RD(68)
        CFG_REG_BANKED_RD(69)
        CFG_REG_BANKED_RD(70)
        CFG_REG_BANKED_RD(71)
        CFG_REG_BANKED_RD(72)
        CFG_REG_BANKED_RD(73)
        case 74: return 0;
        case 75: return 0;
        CFG_REG_BANKED_RD(76)
        CFG_REG_BANKED_RD(77)
        CFG_REG_BANKED_RD(84)
        CFG_REG_BANKED_RD(86)
        CFG_REG_BANKED_RD(112)
        CFG_REG_BANKED_RD(113)
        case 114: return 0;
        case 115: return 0;
        case 119: return 0;
        CFG_REG_BANKED_RD(120)
        CFG_REG_BANKED_RD(121)
        case 122: return 0;
        case 123: return 0;
        CFG_REG_BANKED_RD(124)
        CFG_REG_BANKED_RD(125)
        CFG_REG_RD(220)
#endif
#undef CFG_REG_BANKED_RD
#undef CFG_REG_RD
        default: TTSIM_ERROR(UnimplementedFunctionality, "reg=%d", reg);
    }
}

void tensix_cfg_wr32(TensixState *p_tensix, uint32_t bank, uint32_t offset, uint32_t data) {
#if TT_ARCH_VERSION == 0
    TensixTile *p_tile = &g_t_tiles[p_tensix->tile_id];
#endif
    TTSIM_VERIFY(!(offset & 3), AssertionFailure, "misaligned offset=0x%x", offset);
    switch (uint32_t reg = offset / 4) {
#define CFG_REG_BANKED_WR(i) case i: p_tensix->config[bank].cfg##i = data & CFG##i##_REG_MASK; break;
#define CFG_REG_WR(i) case i: p_tensix->cfg##i = data & CFG##i##_REG_MASK; break;
        CFG_REG_BANKED_WR(0)
        CFG_REG_BANKED_WR(1)
        CFG_REG_BANKED_WR(2)
        CFG_REG_BANKED_WR(3)
#if TT_ARCH_VERSION == 0
        case 4 ... 7: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(8)
        CFG_REG_BANKED_WR(9)
        CFG_REG_BANKED_WR(14)
        CFG_REG_BANKED_WR(16)
        CFG_REG_BANKED_WR(17)
        CFG_REG_BANKED_WR(20)
        CFG_REG_BANKED_WR(21)
        CFG_REG_BANKED_WR(22)
        CFG_REG_BANKED_WR(23)
        CFG_REG_BANKED_WR(24)
        CFG_REG_BANKED_WR(25)
        CFG_REG_BANKED_WR(26)
        CFG_REG_BANKED_WR(27)
        case 28 ... 31: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(41)
        CFG_REG_BANKED_WR(44)
        CFG_REG_BANKED_WR(45)
        CFG_REG_BANKED_WR(47)
        CFG_REG_BANKED_WR(52)
        case 53: p_tensix->config[bank].cfg53 = data; break;
        case 54 ... 55: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(56)
        CFG_REG_BANKED_WR(57)
        CFG_REG_BANKED_WR(58)
        CFG_REG_BANKED_WR(59)
        CFG_REG_BANKED_WR(60)
        CFG_REG_BANKED_WR(61)
        case 62 ... 63: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(64)
        CFG_REG_BANKED_WR(65)
        CFG_REG_BANKED_WR(72)
        CFG_REG_BANKED_WR(74)
        CFG_REG_BANKED_WR(80)
        CFG_REG_BANKED_WR(81)
        CFG_REG_BANKED_WR(84)
        CFG_REG_BANKED_WR(85)
        CFG_REG_BANKED_WR(86)
        CFG_REG_BANKED_WR(87)
        CFG_REG_BANKED_WR(92)
        case 93: p_tensix->config[bank].cfg93 = data; break;
        case 94 ... 95: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(96)
        CFG_REG_BANKED_WR(97)
        CFG_REG_BANKED_WR(98)
        CFG_REG_BANKED_WR(99)
        CFG_REG_BANKED_WR(100)
        CFG_REG_BANKED_WR(101)
        case 102 ... 103: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(104)
        CFG_REG_BANKED_WR(105)
        CFG_REG_BANKED_WR(124)
        CFG_REG_BANKED_WR(125)
        CFG_REG_BANKED_WR(126)
        CFG_REG_BANKED_WR(127)
        CFG_REG_WR(152)
        CFG_REG_WR(153)
        CFG_REG_WR(154)
        CFG_REG_WR(155)
        case 157: // RISCV_IC_INVALIDATE_InvalidateAll
            TTSIM_VERIFY(data, UnsupportedFunctionality, "no-op mask: RISCV_IC_INVALIDATE_InvalidateAll: data=0x%x", data);
            TTSIM_VERIFY(data <= 31, UnsupportedFunctionality, "RISCV_IC_INVALIDATE_InvalidateAll: data=0x%x", data);
            for (uint32_t rv32_id = 0; rv32_id < RV32_CORES_PER_T_TILE; rv32_id++) {
                if (data & (1u << rv32_id)) {
                    rv32_icache_invalidate(&g_t_tiles[p_tensix->tile_id].rv32[rv32_id]);
                }
            }
            break;
        case 158: p_tile->trisc0_reset_pc = data; break; // TRISC_RESET_PC_SEC0_PC
        case 159: p_tile->trisc1_reset_pc = data; break; // TRISC_RESET_PC_SEC1_PC
        case 160: p_tile->trisc2_reset_pc = data; break; // TRISC_RESET_PC_SEC2_PC
        case 161: p_tile->trisc_reset_pc_override = data; break; // TRISC_RESET_PC_OVERRIDE_Reset_PC_Override_en
        case 162: p_tile->ncrisc_reset_pc = data; break; // NCRISC_RESET_PC_PC
        case 163: p_tile->ncrisc_reset_pc_override = data; break; // NCRISC_RESET_PC_OVERRIDE_Reset_PC_Override_en
        case 164: seed_prng(p_tensix, data); break; // PRNG_SEED_Seed_Val
#elif TT_ARCH_VERSION == 1
        case 4 ... 11: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(12)
        CFG_REG_BANKED_WR(13)
        CFG_REG_BANKED_WR(14)
        CFG_REG_BANKED_WR(15)
        CFG_REG_BANKED_WR(17)
        CFG_REG_BANKED_WR(18)
        CFG_REG_BANKED_WR(20)
        CFG_REG_BANKED_WR(21)
        CFG_REG_BANKED_WR(24)
        CFG_REG_BANKED_WR(25)
        CFG_REG_BANKED_WR(28)
        case 29: break;
        case 30: break;
        case 31: break;
        case 32 ... 35: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        case 40 ... 43: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(50)
        CFG_REG_BANKED_WR(56)
        CFG_REG_BANKED_WR(57)
        CFG_REG_BANKED_WR(59)
        CFG_REG_BANKED_WR(64)
        case 65: p_tensix->config[bank].cfg65 = data; break;
        case 66 ... 67: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(68)
        CFG_REG_BANKED_WR(69)
        CFG_REG_BANKED_WR(70)
        CFG_REG_BANKED_WR(71)
        CFG_REG_BANKED_WR(72)
        CFG_REG_BANKED_WR(73)
        case 74 ... 75: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(76)
        CFG_REG_BANKED_WR(77)
        CFG_REG_BANKED_WR(84)
        CFG_REG_BANKED_WR(86)
        CFG_REG_BANKED_WR(92)
        CFG_REG_BANKED_WR(93)
        CFG_REG_BANKED_WR(112)
        case 113: p_tensix->config[bank].cfg113 = data; break;
        case 114 ... 115: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        case 119: TTSIM_VERIFY(!data, UnimplementedFunctionality, "reg=%d data=0x%x", reg, data); break;
        CFG_REG_BANKED_WR(120)
        CFG_REG_BANKED_WR(121)
        case 122 ... 123: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(124)
        CFG_REG_BANKED_WR(125)
        CFG_REG_WR(180)
        CFG_REG_WR(181)
        CFG_REG_WR(182)
        CFG_REG_WR(183)
        case 185: // RISCV_IC_INVALIDATE_InvalidateAll
            TTSIM_VERIFY(data, UnsupportedFunctionality, "no-op mask: RISCV_IC_INVALIDATE_InvalidateAll: data=0x%x", data);
            TTSIM_VERIFY(data <= 31, UnsupportedFunctionality, "RISCV_IC_INVALIDATE_InvalidateAll: data=0x%x", data);
            for (uint32_t rv32_id = 0; rv32_id < RV32_CORES_PER_T_TILE; rv32_id++) {
                if (data & (1u << rv32_id)) {
                    rv32_icache_invalidate(&g_t_tiles[p_tensix->tile_id].rv32[rv32_id]);
                }
            }
            break;
        case 186: seed_prng(p_tensix, data); break; // PRNG_SEED_Seed_Val
        CFG_REG_WR(209)
        CFG_REG_WR(210)
        CFG_REG_WR(211)
        CFG_REG_WR(220)
#endif
#undef CFG_REG_BANKED_WR
#undef CFG_REG_WR
        default: TTSIM_ERROR(UnimplementedFunctionality, "reg=%d", reg);
    }

    // Registers/fields where we enforce that values cannot be set; this simplifies state machines elsewhere
    // XXX Move this into code generators so we can check these only on the specific case statements that modify them
    const TensixConfigState *p_config = &p_tensix->config[bank];
    TTSIM_VERIFY(!p_config->ALU_FORMAT_SPEC_REG_SrcB_override, UnsupportedFunctionality, "ALU_FORMAT_SPEC_REG_SrcB_override");
    TTSIM_VERIFY(!p_config->ALU_FORMAT_SPEC_REG_Dstacc_override, UnsupportedFunctionality, "ALU_FORMAT_SPEC_REG_Dstacc_override");
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_Fpu_srnd_en, UnsupportedFunctionality,
        "ALU_ROUNDING_MODE_Fpu_srnd_en: stochastic rounding is explicitly out of scope");
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_Gasket_srnd_en, UnsupportedFunctionality,
        "ALU_ROUNDING_MODE_Gasket_srnd_en: stochastic rounding is explicitly out of scope");
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_Packer_srnd_en, UnsupportedFunctionality,
        "ALU_ROUNDING_MODE_Packer_srnd_en: stochastic rounding is explicitly out of scope");
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_Padding, UnsupportedFunctionality, "ALU_ROUNDING_MODE_Padding");
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_GS_LF, UnsupportedFunctionality, "ALU_ROUNDING_MODE_GS_LF");
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_Bfp8_HF, UnsupportedFunctionality, "ALU_ROUNDING_MODE_Bfp8_HF");
}

static inline uint32_t get_state_id(TensixState *p_tensix, uint32_t pipe) {
    return p_tensix->thread[pipe].CFG_STATE_ID_StateID;
}

static inline void math_update_rwc(uint32_t *p_rwc, uint32_t *p_rwc_cr, uint32_t incr, uint32_t clr, uint32_t cr, uint32_t c_to_cr, uint32_t rows) {
    if (c_to_cr) {
        TTSIM_VERIFY(!clr, UnsupportedFunctionality, "clr && c_to_cr");
        TTSIM_VERIFY(!cr, UnsupportedFunctionality, "cr && c_to_cr");
        *p_rwc = (*p_rwc + incr) & (rows - 1);
        *p_rwc_cr = *p_rwc;
    } else if (clr) {
        *p_rwc = *p_rwc_cr = 0;
    } else if (cr) {
        *p_rwc = *p_rwc_cr = (*p_rwc_cr + incr) & (rows - 1);
    } else {
        *p_rwc = (*p_rwc + incr) & (rows - 1);
    }
}

// note addr_mode is 2 bits in WH and 3 bits in BH; accessing higher address modes requires bias on WH
// XXX switch statement is clunky and perhaps slow; would be better if these were stored as an array
// could hack this by hardcoding the fact that bit positions are all the same?
static void math_update_counters(TensixState *p_tensix, uint32_t pipe, uint32_t addr_mode, bool update_fidelity_phase) {
#if TT_ARCH_VERSION == 0
    TTSIM_ASSERT(addr_mode < 4);
    if (p_tensix->bias[pipe] || p_tensix->thread[pipe].ADDR_MOD_SET_Base) { // note that ADDR_MOD_SET_Base was removed in BH
        addr_mode += 4;
    }
#else
    TTSIM_VERIFY(!p_tensix->bias[pipe], UnsupportedFunctionality, "bias=%d", p_tensix->bias[pipe]);
#endif
    uint32_t src_a_incr, src_a_cr, src_a_clear;
    uint32_t src_b_incr, src_b_cr, src_b_clear;
    uint32_t dst_incr, dst_cr, dst_clear, dst_c_to_cr;
    uint32_t fidelity_incr, fidelity_clear, bias_incr, bias_clear;
    switch (addr_mode) {
#define ADDR_MODE(i) \
        case i: \
            src_a_incr     = p_tensix->thread[pipe].ADDR_MOD_AB_SEC##i##_SrcAIncr; \
            src_a_cr       = p_tensix->thread[pipe].ADDR_MOD_AB_SEC##i##_SrcACR; \
            src_a_clear    = p_tensix->thread[pipe].ADDR_MOD_AB_SEC##i##_SrcAClear; \
            src_b_incr     = p_tensix->thread[pipe].ADDR_MOD_AB_SEC##i##_SrcBIncr; \
            src_b_cr       = p_tensix->thread[pipe].ADDR_MOD_AB_SEC##i##_SrcBCR; \
            src_b_clear    = p_tensix->thread[pipe].ADDR_MOD_AB_SEC##i##_SrcBClear; \
            dst_incr       = p_tensix->thread[pipe].ADDR_MOD_DST_SEC##i##_DestIncr; \
            dst_cr         = p_tensix->thread[pipe].ADDR_MOD_DST_SEC##i##_DestCR; \
            dst_clear      = p_tensix->thread[pipe].ADDR_MOD_DST_SEC##i##_DestClear; \
            dst_c_to_cr    = p_tensix->thread[pipe].ADDR_MOD_DST_SEC##i##_DestCToCR; \
            fidelity_incr  = p_tensix->thread[pipe].ADDR_MOD_DST_SEC##i##_FidelityIncr; \
            fidelity_clear = p_tensix->thread[pipe].ADDR_MOD_DST_SEC##i##_FidelityClear; \
            bias_incr      = p_tensix->thread[pipe].ADDR_MOD_BIAS_SEC##i##_BiasIncr; \
            bias_clear     = p_tensix->thread[pipe].ADDR_MOD_BIAS_SEC##i##_BiasClear; \
            break;
        ADDR_MODE(0)
        ADDR_MODE(1)
        ADDR_MODE(2)
        ADDR_MODE(3)
        ADDR_MODE(4)
        ADDR_MODE(5)
        ADDR_MODE(6)
        ADDR_MODE(7)
#undef ADDR_MODE
        default:
            TTSIM_ERROR(AssertionFailure, "addr_mode=%d", addr_mode);
    }
    math_update_rwc(&p_tensix->src_a_rwc[pipe], &p_tensix->src_a_rwc_cr[pipe], src_a_incr, src_a_clear, src_a_cr, 0, SRC_ROWS);
    math_update_rwc(&p_tensix->src_b_rwc[pipe], &p_tensix->src_b_rwc_cr[pipe], src_b_incr, src_b_clear, src_b_cr, 0, SRC_ROWS);
    math_update_rwc(&p_tensix->dst_rwc[pipe], &p_tensix->dst_rwc_cr[pipe], dst_incr, dst_clear, dst_cr, dst_c_to_cr, DST_ROWS);

    if (update_fidelity_phase) {
        if (fidelity_clear) {
            p_tensix->fidelity[pipe] = 0;
        } else {
            p_tensix->fidelity[pipe] = (p_tensix->fidelity[pipe] + fidelity_incr) & 3;
        }
    }

    TTSIM_VERIFY(!bias_clear, UnimplementedFunctionality, "bias_clear=%d", bias_clear);
    TTSIM_VERIFY(bias_incr <= 1, UnsupportedFunctionality, "bias_incr=%d", bias_incr);
    if (bias_incr) {
        p_tensix->bias[pipe] ^= 1;
    }
}

static void math_clear_src_valid(TensixState *p_tensix, uint32_t pipe, uint32_t clear_dvalid) {
    if (clear_dvalid & 1) {
        if (!p_tensix->thread[pipe].CLR_DVALID_SrcA_Disable) {
            TTSIM_VERIFY(p_tensix->src_a_valid & (1 << p_tensix->src_a_matrix_bank), NonContractualBehavior, "SrcA bank is not valid");
            p_tensix->src_a_valid &= ~(1 << p_tensix->src_a_matrix_bank);
        }
        p_tensix->src_a_matrix_bank ^= 1;
    }
    if (clear_dvalid & 2) {
        if (!p_tensix->thread[pipe].CLR_DVALID_SrcB_Disable) {
            TTSIM_VERIFY(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank), NonContractualBehavior, "SrcB bank is not valid");
            p_tensix->src_b_valid &= ~(1 << p_tensix->src_b_matrix_bank);
        }
        p_tensix->src_b_matrix_bank ^= 1;
    }
}

// XXX handle Adj16/Adj32 on BH
static inline uint32_t dst32b_adjust_row(uint32_t row) {
    return ((row & 0x1F8) << 1) | (row & 0x207);
}

template<bool is_gmpool = false>
static inline uint32_t read_dst32b(TensixState *p_tensix, uint32_t row, uint32_t col) {
    uint32_t adj_row = dst32b_adjust_row(row);
    if (!p_tensix->dst_row_valid[adj_row]) {
        return is_gmpool ? 0xFFFFFFFF : 0;
    }
    return (uint32_t(p_tensix->dst[adj_row][col]) << 16) | p_tensix->dst[adj_row + 8][col];
}

template<bool set_valid_on_last_column_only = false>
static inline void write_dst32b(TensixState *p_tensix, uint32_t row, uint32_t col, uint32_t data) {
    uint32_t adj_row = dst32b_adjust_row(row);
    p_tensix->dst[adj_row][col] = data >> 16;
    p_tensix->dst[adj_row + 8][col] = data & 0xFFFF;
    if (!set_valid_on_last_column_only || (col == 15)) {
        p_tensix->dst_row_valid[adj_row] = true;
    }
}

template<bool is_gmpool = false>
static inline uint16_t read_dst16b(TensixState *p_tensix, uint32_t row, uint32_t col) {
    if (p_tensix->dst_32bit_addr_en) {
        return read_dst32b<is_gmpool>(p_tensix, row, col) >> 16;
    } else if (!p_tensix->dst_row_valid[row]) {
        return is_gmpool ? 0xFFFF : 0;
    } else {
        return p_tensix->dst[row][col];
    }
}

template<bool set_valid_on_last_column_only = false>
static inline void write_dst16b(TensixState *p_tensix, uint32_t row, uint32_t col, uint16_t data) {
    // XXX docs say this writes "something - generally garbage - to the low 16 bits", what to do?
    if (p_tensix->dst_32bit_addr_en) {
        write_dst32b<set_valid_on_last_column_only>(p_tensix, row, col, uint32_t(data) << 16);
    } else {
        p_tensix->dst[row][col] = data;
        if (!set_valid_on_last_column_only || (col == 15)) {
            p_tensix->dst_row_valid[row] = true;
        }
    }
}

static inline uint16_t dst_decode_bf16(uint16_t x) {
    uint16_t e = x & 255;
    uint16_t m = (x >> 8) & 127;
    return (x & 0x8000) | (e << 7) | m;
}

static inline uint16_t dst_encode_bf16(uint16_t x) {
    uint16_t e = (x >> 7) & 255;
    uint16_t m = x & 127;
    return (x & 0x8000) | (m << 8) | e;
}

static inline uint16_t dst_decode_fp16(uint16_t x) {
    uint16_t e = x & 31;
    uint16_t m = (x >> 5) & 1023;
    return (x & 0x8000) | (e << 10) | m;
}

static inline uint16_t dst_encode_fp16(uint16_t x) {
    uint16_t e = (x >> 10) & 31;
    uint16_t m = x & 1023;
    return (x & 0x8000) | (m << 5) | e;
}

// Note that the "FP32" encoding is also used for INT32
static inline uint32_t dst_decode_fp32(uint32_t x) {
    return (uint32_t(dst_decode_bf16(x >> 16)) << 16) | (x & 0xFFFF);
}

static inline uint32_t dst_encode_fp32(uint32_t x) {
    return (uint32_t(dst_encode_bf16(x >> 16)) << 16) | (x & 0xFFFF);
}

static inline int32_t sign_mag32_total_order(uint32_t x) {
    if (x & 0x80000000) {
        return x ^ 0x7FFFFFFF;
    } else {
        return x;
    }
}

TENSIX_EXECUTE_MOVD2A() {
    TTSIM_VERIFY(instr_mod == 2, UnsupportedFunctionality, "instr_mod=%d", instr_mod);
    TTSIM_VERIFY(!(dst & 3), UnsupportedFunctionality, "dst=%d", dst);
    TTSIM_VERIFY(!(src & 3), UnsupportedFunctionality, "src=%d", src);
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no FP16A_FORCE_Enable
    TTSIM_VERIFY(!p_config->ALU_FORMAT_SPEC_REG_SrcA_override, UnsupportedFunctionality, "ALU_FORMAT_SPEC_REG_SrcA_override");
    uint32_t src_a_fmt = p_config->ALU_FORMAT_SPEC_REG0_SrcA;
#if TT_ARCH_VERSION >= 1
    if (!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCA_FMT_Base) {
        src_a_fmt = p_tensix->src_a_format[src_a_bank];
    }
#endif
    TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 1) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6), // fp32, fp16, tf32, bf16, bfp8
        UnimplementedFunctionality, "src_a_fmt=%d", src_a_fmt);
    bool use_dst32b = p_config->ALU_ACC_CTRL_Fp32_enabled || p_config->ALU_ACC_CTRL_INT8_math_enabled;

    uint32_t src_a_row = src + p_tensix->src_a_rwc[pipe];
    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dst + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    dst_row &= DST_ROWS-1;
    TTSIM_VERIFY(!(src_a_row & 3) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(dst_row & 3), UnimplementedFunctionality, "dst_row=%d", dst_row);
    for (uint32_t row = 0; row < 4; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value;
            if (use_dst32b) {
                TTSIM_VERIFY(!dest_32b_lo, UnimplementedFunctionality, "use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
                TTSIM_VERIFY(src_a_fmt != 1, UnimplementedFunctionality, "use_dst32b fp16");
                uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                if (src_a_fmt != 4) {
                    value = uint32_t(dst_decode_bf16(dst_val >> 16)) << 16;
                } else {
                    TTSIM_VERIFY(!dest_32b_lo, UnimplementedFunctionality, "tf32 use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
                    value = dst_decode_fp32(dst_val) & 0xFFFFE000;
                }
            } else {
                TTSIM_VERIFY(!dest_32b_lo, UndefinedBehavior, "incompatible use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
                TTSIM_VERIFY(src_a_fmt != 4, UndefinedBehavior, "incompatible use_dst32b=%d src_a_fmt=%d", use_dst32b, src_a_fmt);
                value = read_dst16b(p_tensix, dst_row + row, col);
                if (src_a_fmt == 1) {
                    value = dst_decode_fp16(value);
                    value = ((value & 0x8000) << 16) | ((value & 0x7FFF) << 13);
                } else {
                    value = dst_decode_bf16(value) << 16;
                }
            }
            p_tensix->src_a[src_a_bank][src_a_row + row][col] = value;
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    return true;
}

TENSIX_EXECUTE_MOVD2B() {
    TTSIM_VERIFY(!instr_mod || (instr_mod == 2), UnsupportedFunctionality, "instr_mod=%d", instr_mod);
    TTSIM_VERIFY(!(dst & 3), UnsupportedFunctionality, "dst=%d", dst);
    TTSIM_VERIFY(!(src & 3), UnsupportedFunctionality, "src=%d", src);
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no FP16A_FORCE_Enable
    uint32_t src_a_fmt = p_config->ALU_FORMAT_SPEC_REG_SrcA_override ? p_config->ALU_FORMAT_SPEC_REG_SrcA_val : p_config->ALU_FORMAT_SPEC_REG0_SrcA;
#if TT_ARCH_VERSION >= 1
    if (!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCA_FMT_Base) {
        src_a_fmt = p_tensix->src_b_format[src_b_bank];
    }
#endif
    TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) ||
                 (src_a_fmt == 7) || (src_a_fmt == 8), // fp32, tf32, bf16, bfp8, bfp4, int32
        UnimplementedFunctionality, "src_a_fmt=%d", src_a_fmt);
    bool use_dst32b = p_config->ALU_ACC_CTRL_Fp32_enabled || p_config->ALU_ACC_CTRL_INT8_math_enabled;

    uint32_t src_b_row = src + p_tensix->src_b_rwc[pipe];
    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dst + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    dst_row &= DST_ROWS-1;
    TTSIM_VERIFY(!(src_b_row & 3) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    TTSIM_VERIFY(!(dst_row & 3), UnimplementedFunctionality, "dst_row=%d", dst_row);
    uint32_t n_rows = (instr_mod == 2) ? 4 : 1;
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value;
            if (use_dst32b) {
                uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                if (dest_32b_lo) {
                    dst_val = (dst_val << 16) | (dst_val & 0xFFFF);
                }
                if (src_a_fmt != 4) {
                    value = uint32_t(dst_decode_bf16(dst_val >> 16)) << 16;
                } else {
                    TTSIM_VERIFY(!dest_32b_lo, UnimplementedFunctionality, "tf32 use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
                    value = dst_decode_fp32(dst_val) & 0xFFFFE000;
                }
            } else {
                TTSIM_VERIFY(!dest_32b_lo, UndefinedBehavior, "incompatible use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
                TTSIM_VERIFY(src_a_fmt != 4, UndefinedBehavior, "incompatible use_dst32b=%d src_a_fmt=%d", use_dst32b, src_a_fmt);
                value = uint32_t(dst_decode_bf16(read_dst16b(p_tensix, dst_row + row, col))) << 16;
            }
            p_tensix->src_b[src_b_bank][src_b_row + row][col] = value;
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    return true;
}

TENSIX_EXECUTE_MOVB2A() {
    TTSIM_VERIFY(instr_mod == 2, UnsupportedFunctionality, "instr_mod=%d", instr_mod);
    TTSIM_VERIFY(!(srcb & 3), UnsupportedFunctionality, "srcb=%d", srcb);
    TTSIM_VERIFY(!(srca & 3), UnsupportedFunctionality, "srca=%d", srca);
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until SrcB valid
    }

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    bool flush_denormals = !p_config->ALU_ACC_CTRL_Zero_Flag_disabled_src;

    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    uint32_t src_a_row = srca + p_tensix->src_a_rwc[pipe];
    uint32_t src_b_row = srcb + p_tensix->src_b_rwc[pipe];
    TTSIM_VERIFY(!(src_a_row & 3) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(src_b_row & 3) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    uint32_t n_rows = (instr_mod == 2) ? 4 : 1;
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value = p_tensix->src_b[src_b_bank][src_b_row + row][col];
            if (flush_denormals && !(value & 0x7F800000)) {
                value = 0;
            }
            p_tensix->src_a[src_a_bank][src_a_row + row][col] = value;
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    return true;
}

TENSIX_EXECUTE_ZEROACC() {
#if TT_ARCH_VERSION == 1
    uint32_t dst = where; // this field was renamed
    clear_mode |= use_32_bit_mode << 2; // remap to be equivalent to WH fields
    if (clear_zero_flags) { // UndefinedBehavior() in spec, only supported in very limited cases for a HW bug workaround
        TTSIM_VERIFY(clear_mode == 5, UndefinedBehavior, "clear_zero_flags=%d is dangerous and should not be used (clear_mode=%d)", clear_zero_flags, clear_mode);
    }
#endif

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no DEST_REGW_BASE_Base
    uint32_t dst_offset = p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset + p_tensix->dst_rwc[pipe];
    switch (clear_mode) {
        case 0: // clear 1 row
            dst += dst_offset;
            TTSIM_VERIFY(dst < DST_ROWS, UnimplementedFunctionality, "clear 1 row: dst=%d", dst);
            TTSIM_VERIFY(!p_config->ALU_ACC_CTRL_INT8_math_enabled, UnimplementedFunctionality, "ALU_ACC_CTRL_INT8_math_enabled");
            if (p_config->ALU_ACC_CTRL_Fp32_enabled || p_tensix->dst_32bit_addr_en) {
                p_tensix->dst_row_valid[dst32b_adjust_row(dst)] = false;
            } else {
                p_tensix->dst_row_valid[dst] = false;
            }
            break;
        case 1: // clear 16 rows
#if TT_ARCH_VERSION == 1
            if (!p_tensix->DEST_ACCESS_CFG_zeroacc_absolute_tile_mode && (dst_offset & 512)) {
                dst += 32;
            }
#endif
            TTSIM_VERIFY(dst < DST_ROWS / 16, NonContractualBehavior, "clear 16 rows: dst=%d", dst);
            TTSIM_VERIFY(!p_tensix->dst_32bit_addr_en, UnimplementedFunctionality, "clear 16 rows: dst_32bit_addr_en=%d", p_tensix->dst_32bit_addr_en);
            for (uint32_t row = 0; row < 16; row++) {
                p_tensix->dst_row_valid[dst*16 + row] = false;
            }
            break;
#if TT_ARCH_VERSION == 1
        case 5: // clear 32b 16 rows -- XXX only supported in very limited cases for a HW bug workaround
            if (!p_tensix->DEST_ACCESS_CFG_zeroacc_absolute_tile_mode && (dst_offset & 768)) {
                dst += 16;
            }
            TTSIM_VERIFY(dst < DST_ROWS / 32, NonContractualBehavior, "clear 32b 16 rows: dst=%d", dst);
            if (clear_zero_flags) { // validate that the HW bug workaround is not being used in a dangerous way
                for (uint32_t row = 0; row < 16; row++) {
                    // only allow safe usage where valid bit is already set and we're just re-setting it
                    TTSIM_VERIFY(p_tensix->dst_row_valid[dst*32 + (row & 8)*2 + (row & 7)], UndefinedBehavior,
                        "clear 32b 16 rows: clear_zero_flags=%d: dst=%d row=%d: dst_row_valid not already set", clear_zero_flags, dst, row);
                }
            } else {
                for (uint32_t row = 0; row < 16; row++) {
                    p_tensix->dst_row_valid[dst*32 + (row & 8)*2 + (row & 7)] = false;
                }
            }
            break;
#endif
        case 2: // clear half
        case 6: // clear 32b half
            TTSIM_VERIFY(dst <= 1, NonContractualBehavior, "clear half: dst=%d", dst);
            memset(&p_tensix->dst_row_valid[dst * (DST_ROWS / 2)], 0, sizeof(p_tensix->dst_row_valid) / 2);
            break;
        case 3: // clear all
        case 7: // clear 32b all
            TTSIM_VERIFY(!dst, NonContractualBehavior, "clear all: dst=%d", dst);
            memset(p_tensix->dst_row_valid, 0, sizeof(p_tensix->dst_row_valid));
            break;
        default:
            TTSIM_ERROR(UnsupportedFunctionality, "clear_mode=%d", clear_mode);
    }
    if ((clear_mode & 3) <= 1) { // only for 1-row and 16-rows modes, not for "all" or "half" modes
        math_update_counters(p_tensix, pipe, addr_mode, true);
    }
    return true;
}

// XXX Unclear what src_*_format should be set to here, if anything
TENSIX_EXECUTE_ZEROSRC() {
    TTSIM_VERIFY(src_mask, UnsupportedFunctionality, "no-op mask: src_mask=%d", src_mask);
    TTSIM_VERIFY(!zero_val, UnimplementedFunctionality, "zero_val=%d", zero_val);

    if (bank_mask == 1) {
        TTSIM_VERIFY(!write_mode, UnimplementedFunctionality, "write_mode=%d", write_mode);
        TTSIM_VERIFY(src_mask == 3, UntestedFunctionality, "src_mask=%d", src_mask);
        if (src_mask & 1) {
            memset(p_tensix->src_a, 0, sizeof(p_tensix->src_a));
        }
        if (src_mask & 2) {
            memset(p_tensix->src_b, 0, sizeof(p_tensix->src_b));
        }
    } else {
        TTSIM_VERIFY(!bank_mask, UnimplementedFunctionality, "bank_mask=%d", bank_mask);
        TTSIM_VERIFY(write_mode == 1, UnimplementedFunctionality, "write_mode=%d", write_mode);
        uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
        uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
        if (src_mask & 1) {
            memset(p_tensix->src_a[src_a_bank], 0, sizeof(p_tensix->src_a[src_a_bank]));
        }
        if (src_mask & 2) {
            memset(p_tensix->src_b[src_b_bank], 0, sizeof(p_tensix->src_b[src_b_bank]));
        }
    }
    return true;
}

TENSIX_EXECUTE_MOVA2D() {
    TTSIM_VERIFY(instr_mod == 2, UnsupportedFunctionality, "instr_mod=%d", instr_mod); // move 8 rows
    TTSIM_VERIFY(!(dst & 7), UnsupportedFunctionality, "dst=%d", dst);
    TTSIM_VERIFY(!(src & 7), UnsupportedFunctionality, "src=%d", src);
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    if (!(p_tensix->src_a_valid & (1 << src_a_bank))) {
        return false; // stall until SrcA valid
    }

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no FP16A_FORCE_Enable
    uint32_t src_a_fmt = p_config->ALU_FORMAT_SPEC_REG_SrcA_override ? p_config->ALU_FORMAT_SPEC_REG_SrcA_val : p_config->ALU_FORMAT_SPEC_REG0_SrcA;
#if TT_ARCH_VERSION >= 1
    if (!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCA_FMT_Base) {
        src_a_fmt = p_tensix->src_a_format[src_a_bank];
    }
#endif
    bool use_8b_exponent;
    if ((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7) || // fp32, tf32, bf16, bfp8, bfp4
        (src_a_fmt == 8) || (src_a_fmt == 9)) { // int32, int16
        use_8b_exponent = true;
    } else { // fp16
        TTSIM_VERIFY(src_a_fmt == 1, UnimplementedFunctionality, "src_a_fmt=%d", src_a_fmt); // fp16
        use_8b_exponent = false;
    }
    bool flush_denormals = !p_config->ALU_ACC_CTRL_Zero_Flag_disabled_src;

    uint32_t src_a_row = src + p_tensix->src_a_rwc[pipe];
    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dst + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    dst_row &= DST_ROWS-1;
    TTSIM_VERIFY(!(src_a_row & 7) && (src_a_row < SRC_ROWS), UnimplementedFunctionality, "invalid src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(dst_row & 7), UnimplementedFunctionality, "invalid dst_row=%d", dst_row);
    for (uint32_t row = 0; row < 8; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value = p_tensix->src_a[src_a_bank][src_a_row + row][col];
            if (flush_denormals && !(value & 0x7F800000)) {
                value = 0;
            }
            if (src_a_fmt == 4) {
                TTSIM_VERIFY(!dest_32b_lo, UndefinedBehavior, "tf32 with dest_32b_lo=%d (HW erratum TEN-4245)", dest_32b_lo);
                TTSIM_VERIFY(use_8b_exponent, UnsupportedFunctionality, "tf32 use_8b_exponent=%d", use_8b_exponent);
                write_dst32b(p_tensix, dst_row + row, col, dst_encode_fp32(value));
            } else if (!use_8b_exponent) {
                TTSIM_VERIFY(!dest_32b_lo, UnsupportedFunctionality, "dest_32b_lo=%d use_8b_exponent=%d", dest_32b_lo, use_8b_exponent);
                value = ((value & 0x80000000) >> 16) | ((value & 0x0FFFE000) >> 13);
                write_dst16b(p_tensix, dst_row + row, col, dst_encode_fp16(value));
            } else {
                value = dst_encode_bf16(value >> 16);
                if (dest_32b_lo) {
                    uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                    dst_val = dst_val & 0xFFFF0000;
                    write_dst32b<true>(p_tensix, dst_row + row, col, dst_val | value);
                } else {
                    write_dst16b(p_tensix, dst_row + row, col, value);
                }
            }
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    return true;
}

TENSIX_EXECUTE_MOVB2D() {
#if TT_ARCH_VERSION == 1
    uint32_t instr_mod = movb2d_instr_mod; // this field was renamed
#endif
    TTSIM_VERIFY((instr_mod == 2) || (instr_mod == 3) || (instr_mod == 4) || (instr_mod == 5), UnsupportedFunctionality, "instr_mod=%d", instr_mod);
    TTSIM_VERIFY(!(dst & 3), UnsupportedFunctionality, "dst=%d", dst);
    TTSIM_VERIFY(!(src & 3), UnsupportedFunctionality, "src=%d", src);
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until SrcB valid
    }

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no FP16A_FORCE_Enable
    uint32_t src_a_fmt = p_config->ALU_FORMAT_SPEC_REG_SrcA_override ? p_config->ALU_FORMAT_SPEC_REG_SrcA_val : p_config->ALU_FORMAT_SPEC_REG0_SrcA;
    uint32_t src_b_fmt = p_config->ALU_FORMAT_SPEC_REG1_SrcB;
#if TT_ARCH_VERSION >= 1
    if (!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCA_FMT_Base) {
        src_a_fmt = p_tensix->src_b_format[src_b_bank];
    }
    if (!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCB_FMT_Base) {
        src_b_fmt = p_tensix->src_b_format[src_b_bank];
    }
#endif
    bool use_8b_exponent;
    if ((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7) || // fp32, tf32, bf16, bfp8, bfp4
        (src_a_fmt == 8) || (src_a_fmt == 9)) { // int32, int16
        use_8b_exponent = true;
    } else { // fp16
        TTSIM_VERIFY(src_a_fmt == 1, UnimplementedFunctionality, "src_a_fmt=%d", src_a_fmt); // fp16
        use_8b_exponent = false;
    }
    bool flush_denormals = !p_config->ALU_ACC_CTRL_Zero_Flag_disabled_src;

    uint32_t src_b_row = src + p_tensix->src_b_rwc[pipe];
    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dst + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    dst_row &= DST_ROWS-1;
    uint32_t n_rows = (instr_mod & 2) ? 8 : 4;
    TTSIM_VERIFY(!(src_b_row & (n_rows - 1)) && (src_b_row < SRC_ROWS), UnimplementedFunctionality, "invalid src_b_row=%d", src_b_row);
    TTSIM_VERIFY(!(dst_row & (n_rows - 1)), UnimplementedFunctionality, "invalid dst_row=%d", dst_row);
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value = p_tensix->src_b[src_b_bank][src_b_row][(instr_mod & 1) ? 0 : col];
            if (flush_denormals && !(value & 0x7F800000)) {
                value = 0;
            }
            if (src_b_fmt == 1) {
                value &= 0x8FFFE000; // drop high 3 exp bits
            } else if (src_b_fmt != 4) {
                value &= 0xFFFF0000; // drop low 3 man bits
            }
            if (src_a_fmt == 4) {
                TTSIM_VERIFY(!dest_32b_lo, UndefinedBehavior, "tf32 with dest_32b_lo=%d (HW erratum TEN-4245)", dest_32b_lo);
                TTSIM_VERIFY(use_8b_exponent, UnsupportedFunctionality, "tf32 use_8b_exponent=%d", use_8b_exponent);
                write_dst32b(p_tensix, dst_row + row, col, dst_encode_fp32(value));
            } else if (!use_8b_exponent) {
                TTSIM_VERIFY(!dest_32b_lo, UnsupportedFunctionality, "dest_32b_lo=%d use_8b_exponent=%d", dest_32b_lo, use_8b_exponent);
                value = ((value & 0x80000000) >> 16) | ((value & 0x0FFFE000) >> 13);
                write_dst16b(p_tensix, dst_row + row, col, dst_encode_fp16(value));
            } else {
                value = dst_encode_bf16(value >> 16);
                if (dest_32b_lo) {
                    uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                    dst_val = dst_val & 0xFFFF0000;
                    write_dst32b<true>(p_tensix, dst_row + row, col, dst_val | value);
                } else {
                    write_dst16b(p_tensix, dst_row + row, col, value);
                }
            }
        }
        if (!(instr_mod & 2)) {
            src_b_row++;
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    return true;
}

TENSIX_EXECUTE_TRNSPSRCB() {
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until SrcB valid
    }

    uint32_t row_base = 16;
    for (uint32_t i = 0; i < 16; i++) {
        for (uint32_t j = 0; j < i; j++) {
            uint32_t ij = p_tensix->src_b[src_b_bank][row_base + i][j];
            uint32_t ji = p_tensix->src_b[src_b_bank][row_base + j][i];
            p_tensix->src_b[src_b_bank][row_base + i][j] = ji;
            p_tensix->src_b[src_b_bank][row_base + j][i] = ij;
        }
    }
    return true;
}

// Normalize sop and dst terms to a common exponent, then add, renormalize, and encode
static uint32_t fpu_accum_normalize_encode(uint32_t sign_sop0, int32_t exp_sop0, uint32_t man_sop0,
                                           uint32_t sign_sop1, int32_t exp_sop1, uint32_t man_sop1,
                                           uint32_t dst_value, bool fp32_acc, bool fp_exponent_8b) {
    uint32_t sign_dst = dst_value >> 31;
    int32_t exp_dst = (dst_value >> 23) & 255;
    uint32_t man_dst = (dst_value & 0x7FFFFF) | 0x800000; // 24 bits
    if (!exp_dst) {
        man_dst = 0;
    }

    int32_t exp = std::max(std::max(exp_sop0, exp_sop1), exp_dst);
    if (exp <= 0) { // early underflow case (before normalization/rounding)
        return 0;
    }
    if (exp_sop0 < exp) {
        uint32_t exp_diff = exp - exp_sop0;
        if (exp_diff < 31) {
            man_sop0 = (man_sop0 + (1 << (exp_diff - 1)) - sign_sop0) >> exp_diff;
        } else {
            man_sop0 = 0;
        }
    }
    if (exp_sop1 < exp) {
        uint32_t exp_diff = exp - exp_sop1;
        if (exp_diff < 31) {
            man_sop1 = (man_sop1 + (1 << (exp_diff - 1)) - sign_sop1) >> exp_diff;
        } else {
            man_sop1 = 0;
        }
    }
    if (exp_dst < exp) {
        uint32_t exp_diff = exp - exp_dst;
        if (exp_diff < 31) {
            man_dst = (man_dst + (1 << (exp_diff - 1))) >> exp_diff;
        } else {
            man_dst = 0;
        }
    }
    if (!fp32_acc) { // low 13 mantissa bits are rounded away unless fp32_acc
        man_sop0 = (man_sop0 + (1 << 12) - sign_sop0) & ~0x1FFF;
        man_sop1 = (man_sop1 + (1 << 12) - sign_sop1) & ~0x1FFF;
        man_dst = (man_dst + (1 << 12)) & ~0x1FFF;
    }

    uint32_t sign = sign_dst;
    int32_t man = man_dst;
    if (sign_sop0 == sign) {
        man += man_sop0;
    } else {
        man -= man_sop0;
    }
    if (sign_sop1 == sign) {
        man += man_sop1;
    } else {
        man -= man_sop1;
    }
    if (man < 0) {
        man = -man;
        sign ^= 1;
    }
    if (!man) { // exact cancellation
        return 0;
    }

    int32_t shift = __builtin_clz(man) - 8; // amount to shift left to normalize to 24 bits
#if TT_ARCH_VERSION == 0
    if (sign && (man == 1)) { // mimic bug in renormalization of -1 (all bits set in leading sign count)
        shift -= 27; // correct shift is 23, handle as if shift is -4
    }
#endif
    exp -= shift;
    if (!fp32_acc) {
        shift -= fp_exponent_8b ? 16 : 13;
    }
    if (shift < 0) { // right-shift
        shift = -shift;
        man = (man + (1 << (shift - 1))) >> shift;
    } else { // left-shift
        man <<= shift;
    }
    if (!fp32_acc) {
        man <<= fp_exponent_8b ? 16 : 13;
    }
    if (man & 0x1000000) {
        exp++;
    }
    man = man & 0x7FFFFF;
    if (exp <= 0) {
        return 0;
    } else if (fp32_acc || fp_exponent_8b) {
        if (exp >= 255) {
            return (sign << 31) | (255 << 23);
        } else {
            return (sign << 31) | (exp << 23) | man;
        }
    } else {
        if (exp >= 32) {
            return (sign << 31) | (31 << 23) | (0x3FF << 13);
        } else {
            return (sign << 31) | (exp << 23) | man;
        }
    }
}

static inline int32_t fpu_exp_prod_adj(bool fp32_acc, uint32_t fidelity_phase, bool fp_exponent_8b) {
    int32_t exp_prod_adj = fp_exponent_8b ? -127 : (fp32_acc ? (127 - 30) : -15); // rebias exponent to desired range
    if (fidelity_phase & 1) {
        exp_prod_adj -= 5;
    }
    if (fidelity_phase & 2) {
        exp_prod_adj -= 7;
    }
    return exp_prod_adj;
}

static uint32_t elwmul(uint32_t a_value, uint32_t b_value, uint32_t dst_value, bool zero_a, bool zero_b, bool fp32_acc, uint32_t fidelity_phase, bool fp_exponent_8b) {
    int32_t exp_prod_adj = fpu_exp_prod_adj(fp32_acc, fidelity_phase, fp_exponent_8b);
    int32_t zero_term_exp = std::max(exp_prod_adj, 0); // clamped exponent to apply for computation of shared exponent

    uint16_t sign_a = a_value >> 18;
    uint16_t sign_b = b_value >> 18;
    uint32_t exp_a = (a_value >> 10) & 255;
    uint32_t exp_b = (b_value >> 10) & 255;
    uint32_t man_a = (a_value & 0x3FF) | 0x400; // 11 bits
    uint32_t man_b = (b_value & 0x3FF) | 0x400;

    int32_t exp_sop = exp_a + exp_b + exp_prod_adj;
    if (fidelity_phase & 1) {
        man_a = (man_a >> 1) & 31; // 5 bits
    } else {
        man_a >>= 6; // 5 bits
    }
    if (fidelity_phase & 2) {
        man_b = (man_b & 15) << 3; // 7 bits
    } else {
        man_b >>= 4; // 7 bits
    }
    uint16_t sign_sop = sign_a ^ sign_b;
    uint32_t man_sop = (man_a * man_b) << 13; // 25 bits
    if (zero_a || zero_b || (exp_sop <= 0)) {
        sign_sop = 0;
        exp_sop = zero_term_exp;
        man_sop = 0;
    }

    return fpu_accum_normalize_encode(sign_sop, exp_sop, man_sop, 0, 0, 0, dst_value, fp32_acc, fp_exponent_8b);
}

static uint32_t elwadd(uint32_t a_value, uint32_t b_value, uint32_t dst_value, bool zero_a, bool zero_b, bool fp32_acc, bool elwadd_accum_en, bool fp_exponent_8b) {
    if (zero_a) {
        a_value = 0;
    }
    if (zero_b) {
        b_value = 0;
    }
    if (!elwadd_accum_en) {
        dst_value = 0;
    }
    if ((a_value & 0x3FFFF) < (b_value & 0x3FFFF)) {
        std::swap(a_value, b_value); // ensure abs(a) >= abs(b)
    }
    uint16_t sign_a = a_value >> 18;
    uint16_t sign_b = b_value >> 18;
    int32_t exp_a = (a_value >> 10) & 255;
    int32_t exp_b = (b_value >> 10) & 255;
    uint32_t man_a = a_value ? ((a_value & 0x3FF) | 0x400) : 0; // 11 bits
    uint32_t man_b = b_value ? ((b_value & 0x3FF) | 0x400) : 0;

    // Compute sum of products term with shared exponent
    int32_t exp_sop = exp_a;
    if (exp_b < exp_sop) {
        uint32_t exp_diff = exp_sop - exp_b;
        if (exp_diff > 0) {
            if (exp_diff < 12) {
                man_b = (man_b + (1 << (exp_diff - 1))) >> exp_diff;
            } else {
                man_b = 0; // all bits shifted out
            }
        }
    }
    uint16_t sign_sop = sign_a;
    uint32_t man_sop;
    if (sign_a == sign_b) {
        man_sop = man_a + man_b; // 12 bit sum (11 bit inputs)
    } else {
        man_sop = man_a - man_b;
    }
    man_sop <<= 13; // now a 25 bit sum
    if (fp32_acc && !fp_exponent_8b) {
        if (exp_sop) {
            exp_sop += 127 - 15;
        } else {
            exp_sop = 127 - 30; // clamped exponent to apply for computation of shared exponent
        }
    }

    return fpu_accum_normalize_encode(sign_sop, exp_sop, man_sop, 0, 0, 0, dst_value, fp32_acc, fp_exponent_8b);
}

static uint32_t mvmul(const uint32_t *a_values, const uint32_t *b_values, uint32_t dst_value,
                      int32_t exp_prod_adj, bool fp32_acc, uint32_t fidelity_phase, bool fp_exponent_8b) {
    int32_t zero_term_exp = std::max(exp_prod_adj, 0); // clamped exponent to apply for computation of shared exponent

    uint16_t signs[16];
    int32_t exps[16];
    uint32_t mans[16];
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t a = a_values[i];
        uint32_t b = b_values[i];
        bool zero_a = ((a & 0x3FFFF) < 0x400);
        bool zero_b = ((b & 0x3FFFF) < 0x400);
        if (zero_a || zero_b) {
            signs[i] = 0;
            exps[i] = zero_term_exp;
            mans[i] = 0;
        } else {
            uint16_t sign_a = a >> 18;
            uint16_t sign_b = b >> 18;
            uint32_t exp_a = (a >> 10) & 255;
            uint32_t exp_b = (b >> 10) & 255;
            uint32_t man_a = (a & 0x3FF) | 0x400; // 11 bits
            uint32_t man_b = (b & 0x3FF) | 0x400;

            int32_t exp_prod = exp_a + exp_b + exp_prod_adj;
            if (fidelity_phase & 1) {
                man_a = (man_a >> 1) & 31; // 5 bits
            } else {
                man_a >>= 6; // 5 bits
            }
            if (fidelity_phase & 2) {
                man_b = (man_b & 15) << 3; // 7 bits
            } else {
                man_b >>= 4; // 7 bits
            }

            signs[i] = sign_a ^ sign_b;
            exps[i] = exp_prod;
            mans[i] = man_a * man_b; // up to 12 bits
        }
    }

    // Align all products to max exp for their group of 8 and combine into a sop for each group
    int32_t exp_sops[2] = {0, 0};
    int32_t man_sops[2] = {0, 0};
    uint32_t sign_sops[2] = {0, 0};
    for (uint32_t group = 0; group < 2; group++) {
        int32_t exp_sop = exps[8*group];
        for (uint32_t i = 1; i < 8; i++) {
            exp_sop = std::max(exp_sop, exps[8*group + i]);
        }
        if (exp_sop <= 0) {
            continue; // entire sop is zero
        }
        int32_t man_sop = 0;
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t lane = 8*group + i;
            int32_t man = mans[lane];
            uint32_t exp_diff = exp_sop - exps[lane];
            exp_diff = std::min(exp_diff, 30u); // clamp to avoid UB on shift -- result will always be zero
            man = ((man << 1) + (1 << exp_diff)) >> (exp_diff + 1);
            if (signs[lane]) {
                man = -man;
            }
            man_sop += man;
        }
        if (man_sop < 0) {
            sign_sops[group] = 1;
            man_sop = -man_sop;
        }
        man_sops[group] = man_sop << 13;
        exp_sops[group] = exp_sop;
    }
    return fpu_accum_normalize_encode(sign_sops[0], exp_sops[0], man_sops[0], sign_sops[1], exp_sops[1], man_sops[1], dst_value, fp32_acc, fp_exponent_8b);
}

static inline int32_t read_src_int8(uint32_t x) {
    uint32_t s = x >> 31;
    int32_t m = (x >> 13) & 0x3FF;
    return s ? -m : m;
}

static inline int32_t dst_decode_int32(uint32_t x) {
    x = dst_decode_fp32(x);
    int32_t result = x & 0x7FFFFFFF;
    if (x & 0x80000000) {
        result = -result;
    }
    return result;
}

static inline uint32_t dst_encode_int32(int32_t x) {
    if (x & 0x80000000) {
        x = 0x80000000 | -x; // two's complement to sign/magnitude
    }
    return dst_encode_fp32(x);
}

static inline int32_t saturate_add_int32(int32_t x, int32_t y) {
    int64_t result64 = int64_t(x) + int64_t(y);
    if (result64 > 0x7FFFFFFFLL) {
        return 0x7FFFFFFFLL;
    } else if (result64 < -0x7FFFFFFFLL) {
        return -0x7FFFFFFFLL;
    } else {
        return int32_t(result64);
    }
}

template<bool is_gapool>
static bool tensix_matmul_op(TensixState *p_tensix, uint32_t pipe, uint32_t dst, uint32_t addr_mode, uint32_t instr_mod19, uint32_t clear_dvalid) {
    if (is_gapool) {
        TTSIM_VERIFY(instr_mod19 == 1, UnsupportedFunctionality, "instr_mod19=%d", instr_mod19);
        TTSIM_VERIFY(!(dst & 3), UnsupportedFunctionality, "dst=%d", dst);
    } else {
        TTSIM_VERIFY(!instr_mod19, UnsupportedFunctionality, "instr_mod19=%d", instr_mod19);
        TTSIM_VERIFY(!(dst & 7), UnsupportedFunctionality, "dst=%d", dst);
    }
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_a_valid & (1 << src_a_bank)) || !(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until both valid
    }

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no FP16A_FORCE_Enable
    bool is_int8 = p_config->ALU_ACC_CTRL_INT8_math_enabled;
    TTSIM_VERIFY(!p_config->ALU_FORMAT_SPEC_REG_SrcA_override, UnsupportedFunctionality, "ALU_FORMAT_SPEC_REG_SrcA_override");
    uint32_t src_a_fmt = p_config->ALU_FORMAT_SPEC_REG0_SrcA;
    uint32_t src_b_fmt = p_config->ALU_FORMAT_SPEC_REG1_SrcB;
#if TT_ARCH_VERSION >= 1
    TTSIM_VERIFY(!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCA_FMT_Base, UnimplementedFunctionality, "DISABLE_IMPLIED_SRCA_FMT_Base");
    TTSIM_VERIFY(!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCB_FMT_Base, UnimplementedFunctionality, "DISABLE_IMPLIED_SRCB_FMT_Base");
    src_a_fmt = p_tensix->src_a_format[src_a_bank];
    src_b_fmt = p_tensix->src_b_format[src_b_bank];
#endif
    if (is_int8) {
        TTSIM_VERIFY(src_a_fmt == 14, UnsupportedFunctionality, "int8 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
        TTSIM_VERIFY(src_b_fmt == 14, UnsupportedFunctionality, "int8 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
    } else if (src_a_fmt == 1) { // fp16
        TTSIM_VERIFY(src_b_fmt == 1, UnsupportedFunctionality, "fp16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp16
    } else {
        TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7),
            UnimplementedFunctionality, "bf16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4
        TTSIM_VERIFY((src_b_fmt == 0) || (src_b_fmt == 4) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7) || (src_b_fmt == 9),
            UnimplementedFunctionality, "bf16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4, int16
    }
    bool use_dst32b = is_int8 || p_config->ALU_ACC_CTRL_Fp32_enabled;

    // XXX for GAPOOL, instr_mod1 is being ignored here per the tt-isa-documentation; otherwise GAPOOL is as MVMUL with 4 rows
    uint32_t src_a_row = p_tensix->src_a_rwc[pipe];
    uint32_t src_b_row = p_tensix->src_b_rwc[pipe];
    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dst + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    dst_row &= DST_ROWS-1;
    constexpr uint32_t N_ROWS = is_gapool ? 4 : 8;
    TTSIM_VERIFY(!(src_a_row & 15) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(src_b_row & 7) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    TTSIM_VERIFY(!(dst_row & (N_ROWS - 1)), UnimplementedFunctionality, "dst_row=%d", dst_row);
    uint32_t fidelity_phase = (p_tensix->fidelity[pipe] + p_tensix->thread[pipe].FIDELITY_BASE_Phase) & 3;
    bool fp_exponent_8b = (src_a_fmt != 1);
    uint32_t src_b[N_ROWS][16], src_a[ROW_SIZE][16];
    if (is_int8) {
        for (uint32_t row = 0; row < N_ROWS; row++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_b[src_b_bank][src_b_row + row][i];
                src_b[row][i] = read_src_int8(value & ((fidelity_phase & 2) ? 0x8001E000 : 0x807E0000));
            }
        }
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_a[src_a_bank][src_a_row + i][col];
                src_a[col][i] = read_src_int8(value & ((fidelity_phase & 1) ? 0x8003E000 : 0x801C0000));
            }
        }
        for (uint32_t row = 0; row < N_ROWS; row++) {
            for (uint32_t col = 0; col < ROW_SIZE; col++) {
                uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                int32_t result = 0;
                for (uint32_t i = 0; i < 16; i++) {
                    result += int32_t(src_a[col][i]) * int32_t(src_b[row][i]); // cannot overflow, both inputs in [-1023,1023]
                }
                result = saturate_add_int32(dst_decode_int32(dst_val), result);
                write_dst32b<true>(p_tensix, dst_row + row, col, dst_encode_int32(result));
            }
        }
    } else {
        for (uint32_t row = 0; row < N_ROWS; row++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_b[src_b_bank][src_b_row + row][i];
                if (src_b_fmt == 1) {
                    value &= 0x8FFFE000;
                } else if (src_b_fmt != 4) {
                    value &= 0xFFFF0000;
                }
                src_b[row][i] = value >> 13;
            }
        }
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_a[src_a_bank][src_a_row + i][col];
                if (src_a_fmt == 1) {
                    value &= 0x8FFFE000;
                } else if (src_a_fmt != 4) {
                    value &= 0xFFFF0000;
                }
                src_a[col][i] = value >> 13;
            }
        }
        int32_t exp_prod_adj = fpu_exp_prod_adj(use_dst32b, fidelity_phase, fp_exponent_8b);
        if (use_dst32b) {
            for (uint32_t row = 0; row < N_ROWS; row++) {
                for (uint32_t col = 0; col < ROW_SIZE; col++) {
                    uint32_t dst_val = dst_decode_fp32(read_dst32b(p_tensix, dst_row + row, col));
                    uint32_t result = mvmul(src_a[col], src_b[row], dst_val, exp_prod_adj, use_dst32b, fidelity_phase, fp_exponent_8b);
                    write_dst32b<true>(p_tensix, dst_row + row, col, dst_encode_fp32(result));
                }
            }
        } else if (fp_exponent_8b) {
            for (uint32_t row = 0; row < N_ROWS; row++) {
                for (uint32_t col = 0; col < ROW_SIZE; col++) {
                    uint32_t dst_val = read_dst16b(p_tensix, dst_row + row, col);
                    dst_val = uint32_t(dst_decode_bf16(dst_val)) << 16;
                    uint32_t result = mvmul(src_a[col], src_b[row], dst_val, exp_prod_adj, use_dst32b, fidelity_phase, fp_exponent_8b);
                    result = dst_encode_bf16(result >> 16);
                    write_dst16b<true>(p_tensix, dst_row + row, col, result);
                }
            }
        } else {
            for (uint32_t row = 0; row < N_ROWS; row++) {
                for (uint32_t col = 0; col < ROW_SIZE; col++) {
                    uint32_t dst_val = read_dst16b(p_tensix, dst_row + row, col);
                    dst_val = dst_decode_fp16(dst_val);
                    dst_val = ((dst_val & 0x8000) << 16) | ((dst_val & 0x7FFF) << 13);
                    uint32_t result = mvmul(src_a[col], src_b[row], dst_val, exp_prod_adj, use_dst32b, fidelity_phase, fp_exponent_8b);
                    result = dst_encode_fp16(((result & 0x80000000) >> 16) | ((result & 0x0FFFE000) >> 13));
                    write_dst16b<true>(p_tensix, dst_row + row, col, result);
                }
            }
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    math_clear_src_valid(p_tensix, pipe, clear_dvalid);
    return true;
}

TENSIX_EXECUTE_MVMUL() { return tensix_matmul_op<false>(p_tensix, pipe, dst, addr_mode, instr_mod19, clear_dvalid); }

struct elw_op_mul {
    static inline bool is_mul() { return true; }
    static inline bool is_sub() { return false; }
};
struct elw_op_add {
    static inline bool is_mul() { return false; }
    static inline bool is_sub() { return false; }
};
struct elw_op_sub {
    static inline bool is_mul() { return false; }
    static inline bool is_sub() { return true; }
};

template<class elw_op>
static bool tensix_elw_op(TensixState *p_tensix, uint32_t pipe, uint32_t dst, uint32_t addr_mode, uint32_t instr_mod19,
                          uint32_t dest_accum_en, uint32_t clear_dvalid) {
    TTSIM_VERIFY(!(dst & 7), UnsupportedFunctionality, "dst=%d", dst);
    if (elw_op::is_mul()) {
        TTSIM_VERIFY(!dest_accum_en, NonContractualBehavior, "dest_accum_en=%d", dest_accum_en);
    }
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_a_valid & (1 << src_a_bank)) || !(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until both valid
    }

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no FP16A_FORCE_Enable
    bool is_int8 = p_config->ALU_ACC_CTRL_INT8_math_enabled;
    TTSIM_VERIFY(!p_config->ALU_FORMAT_SPEC_REG_SrcA_override, UnsupportedFunctionality, "ALU_FORMAT_SPEC_REG_SrcA_override");
    uint32_t src_a_fmt = p_config->ALU_FORMAT_SPEC_REG0_SrcA;
    uint32_t src_b_fmt = p_config->ALU_FORMAT_SPEC_REG1_SrcB;
#if TT_ARCH_VERSION >= 1
    TTSIM_VERIFY(!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCA_FMT_Base, UnimplementedFunctionality, "DISABLE_IMPLIED_SRCA_FMT_Base");
    TTSIM_VERIFY(!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCB_FMT_Base, UnimplementedFunctionality, "DISABLE_IMPLIED_SRCB_FMT_Base");
    src_a_fmt = p_tensix->src_a_format[src_a_bank];
    src_b_fmt = p_tensix->src_b_format[src_b_bank];
#endif
    if (is_int8) {
        TTSIM_VERIFY(src_a_fmt == 14, UnsupportedFunctionality, "int8 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
        TTSIM_VERIFY((src_b_fmt == 14) || (src_b_fmt == 15), UnsupportedFunctionality, "int8 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // XXX why bfp2?
    } else if (src_a_fmt == 1) { // fp16
        TTSIM_VERIFY(src_b_fmt == 1, UnsupportedFunctionality, "fp16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp16
    } else {
        TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7),
            UnimplementedFunctionality, "bf16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4
        TTSIM_VERIFY((src_b_fmt == 0) || (src_b_fmt == 4) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7) || (src_b_fmt == 9),
            UnimplementedFunctionality, "bf16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4, int16
    }
    bool use_dst32b = is_int8 || p_config->ALU_ACC_CTRL_Fp32_enabled;
    bool fp_exponent_8b = (src_a_fmt != 1);

    uint32_t src_a_row = p_tensix->src_a_rwc[pipe];
    uint32_t src_b_row = p_tensix->src_b_rwc[pipe];
    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dst + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    TTSIM_VERIFY(!(src_a_row & 7) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(src_b_row & 7) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    TTSIM_VERIFY(!(dst_row & 7) && (dst_row < DST_ROWS), UnsupportedFunctionality, "dst_row=%d", dst_row);
    uint32_t fidelity_phase = (p_tensix->fidelity[pipe] + p_tensix->thread[pipe].FIDELITY_BASE_Phase) & 3;
    if (!elw_op::is_mul()) {
        TTSIM_VERIFY(!fidelity_phase, NonContractualBehavior, "ELWADD/SUB should not be used with fidelity_phase=%d", fidelity_phase);
    }
    for (uint32_t row = 0; row < 8; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value_a = p_tensix->src_a[src_a_bank][src_a_row + row][col];
            uint32_t value_b = p_tensix->src_b[src_b_bank][src_b_row + ((instr_mod19 & 2) ? 0 : row)][(instr_mod19 & 1) ? 0 : col];
            if (elw_op::is_sub()) {
                value_b ^= 0x80000000;
            }
            if (is_int8) {
                if (elw_op::is_mul()) {
                    value_a &= (fidelity_phase & 1) ? 0x8003E000 : 0x801C0000;
                    value_b &= (fidelity_phase & 2) ? 0x8001E000 : 0x807E0000;
                }
                int32_t src_a_val = read_src_int8(value_a);
                int32_t src_b_val = read_src_int8(value_b);
                int32_t result = elw_op::is_mul() ? (src_a_val * src_b_val) : (src_a_val + src_b_val); // cannot overflow, both inputs in [-1023,1023]
                if (elw_op::is_mul() || dest_accum_en) { // ELWMUL forces accumulation, independent of dest_accum_en
                    uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                    result = saturate_add_int32(dst_decode_int32(dst_val), result);
                }
                write_dst32b<true>(p_tensix, dst_row + row, col, dst_encode_int32(result));
                continue;
            }
            if (src_a_fmt == 1) {
                value_a &= 0x8FFFE000;
            } else if (src_a_fmt != 4) {
                value_a &= 0xFFFF0000;
            }
            if (src_b_fmt == 1) {
                value_b &= 0x8FFFE000;
            } else if (src_b_fmt != 4) {
                value_b &= 0xFFFF0000;
            }
            value_a >>= 13;
            value_b >>= 13;
            bool zero_a = ((value_a & 0x3FFFF) < 0x400);
            bool zero_b = ((value_b & 0x3FFFF) < 0x400);
            uint32_t dst = 0;
            if (elw_op::is_mul() || dest_accum_en) { // ELWMUL forces accumulation, independent of dest_accum_en
                if (use_dst32b) {
                    dst = dst_decode_fp32(read_dst32b(p_tensix, dst_row + row, col));
                } else {
                    dst = read_dst16b(p_tensix, dst_row + row, col);
                    if (fp_exponent_8b) {
                        dst = uint32_t(dst_decode_bf16(dst)) << 16;
                    } else {
                        dst = dst_decode_fp16(dst);
                        dst = ((dst & 0x8000) << 16) | ((dst & 0x7FFF) << 13);
                    }
                }
            }
            uint32_t result;
            if (elw_op::is_mul()) {
                result = elwmul(value_a, value_b, dst, zero_a, zero_b, use_dst32b, fidelity_phase, fp_exponent_8b);
            } else {
                result = elwadd(value_a, value_b, dst, zero_a, zero_b, use_dst32b, dest_accum_en, fp_exponent_8b);
            }
            if (use_dst32b) {
                write_dst32b<true>(p_tensix, dst_row + row, col, dst_encode_fp32(result));
            } else {
                if (fp_exponent_8b) {
                    result = dst_encode_bf16(result >> 16);
                } else {
                    result = dst_encode_fp16(((result & 0x80000000) >> 16) | ((result & 0x0FFFE000) >> 13));
                }
                write_dst16b<true>(p_tensix, dst_row + row, col, result);
            }
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    math_clear_src_valid(p_tensix, pipe, clear_dvalid);
    return true;
}

TENSIX_EXECUTE_ELWMUL() { return tensix_elw_op<elw_op_mul>(p_tensix, pipe, dst, addr_mode, instr_mod19, dest_accum_en, clear_dvalid); }
TENSIX_EXECUTE_ELWADD() { return tensix_elw_op<elw_op_add>(p_tensix, pipe, dst, addr_mode, instr_mod19, dest_accum_en, clear_dvalid); }
TENSIX_EXECUTE_ELWSUB() { return tensix_elw_op<elw_op_sub>(p_tensix, pipe, dst, addr_mode, instr_mod19, dest_accum_en, clear_dvalid); }

TENSIX_EXECUTE_GMPOOL() {
#if TT_ARCH_VERSION == 1
    uint32_t addr_mode = pool_addr_mode; // this field was renamed
#endif
    TTSIM_VERIFY(!max_pool_index_en, UnsupportedFunctionality, "max_pool_index_en=%d", max_pool_index_en);
    TTSIM_VERIFY(instr_mod19 == 1, UnsupportedFunctionality, "instr_mod19=%d", instr_mod19);
    TTSIM_VERIFY(!(dst & 3), UnsupportedFunctionality, "dst=%d", dst);
    TTSIM_VERIFY(dst < 8, UntestedFunctionality, "dst=%d", dst);
    TTSIM_VERIFY(addr_mode < 2, UntestedFunctionality, "addr_mode=%d", addr_mode);
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_a_valid & (1 << src_a_bank)) || !(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until both valid
    }

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no FP16A_FORCE_Enable
    TTSIM_VERIFY(!p_config->ALU_ACC_CTRL_INT8_math_enabled, UnsupportedFunctionality, "ALU_ACC_CTRL_INT8_math_enabled");
    TTSIM_VERIFY(!p_config->ALU_FORMAT_SPEC_REG_SrcA_override, UnsupportedFunctionality, "ALU_FORMAT_SPEC_REG_SrcA_override");
    uint32_t src_a_fmt = p_config->ALU_FORMAT_SPEC_REG0_SrcA;
    uint32_t src_b_fmt = p_config->ALU_FORMAT_SPEC_REG1_SrcB;
#if TT_ARCH_VERSION >= 1
    TTSIM_VERIFY(!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCA_FMT_Base, UnimplementedFunctionality, "DISABLE_IMPLIED_SRCA_FMT_Base");
    TTSIM_VERIFY(!p_tensix->thread[pipe].DISABLE_IMPLIED_SRCB_FMT_Base, UnimplementedFunctionality, "DISABLE_IMPLIED_SRCB_FMT_Base");
    src_a_fmt = p_tensix->src_a_format[src_a_bank];
    src_b_fmt = p_tensix->src_b_format[src_b_bank];
#endif
    TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7),
        UnsupportedFunctionality, "src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4
    TTSIM_VERIFY((src_b_fmt == 0) || (src_b_fmt == 4) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7),
        UnsupportedFunctionality, "src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4
    bool use_dst32b = p_config->ALU_ACC_CTRL_Fp32_enabled;

    uint32_t src_a_row = p_tensix->src_a_rwc[pipe];
    uint32_t src_b_row = p_tensix->src_b_rwc[pipe];
    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dst + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    TTSIM_VERIFY(!(src_a_row & 15) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(src_b_row & 7) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    TTSIM_VERIFY(!(dst_row & 3) && (dst_row < DST_ROWS), UnsupportedFunctionality, "dst_row=%d", dst_row);

    for (uint32_t i = 0; i < 16; i++) {
        uint32_t src_b_val = p_tensix->src_b[src_b_bank][src_b_row][i];
        TTSIM_VERIFY(src_b_val == 0x3F800000, UnsupportedFunctionality, "src_b_val=0x%x must be 1.0f", src_b_val);
    }
    for (uint32_t j = 0; j < 16; j++) {
        uint32_t max;
        if (use_dst32b) {
            max = dst_decode_fp32(read_dst32b<true>(p_tensix, dst_row, j));
        } else {
            max = uint32_t(dst_decode_bf16(read_dst16b<true>(p_tensix, dst_row, j))) << 16;
        }
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t src_a_val = p_tensix->src_a[src_a_bank][src_a_row + i][j];
            if (src_a_fmt != 4) {
                src_a_val &= 0xFFFF0000;
            }
            if (sign_mag32_total_order(src_a_val) > sign_mag32_total_order(max)) {
                max = src_a_val;
            }
        }
        if (use_dst32b) {
            write_dst32b<true>(p_tensix, dst_row, j, dst_encode_fp32(max));
            for (uint32_t i = 1; i < 4; i++) {
                write_dst32b(p_tensix, dst_row + i, j, 0);
            }
        } else {
            write_dst16b<true>(p_tensix, dst_row, j, dst_encode_bf16(max >> 16));
            for (uint32_t i = 1; i < 4; i++) {
                write_dst16b(p_tensix, dst_row + i, j, 0);
            }
        }
    }

    math_update_counters(p_tensix, pipe, addr_mode, true);
    math_clear_src_valid(p_tensix, pipe, clear_dvalid);
    return true;
}

TENSIX_EXECUTE_GAPOOL() {
    TTSIM_VERIFY(!max_pool_index_en, UnsupportedFunctionality, "max_pool_index_en=%d", max_pool_index_en);
#if TT_ARCH_VERSION == 1
    uint32_t addr_mode = pool_addr_mode; // this field was renamed
    TTSIM_VERIFY(addr_mode < 4, UntestedFunctionality, "addr_mode=%d", addr_mode);
#endif
    return tensix_matmul_op<true>(p_tensix, pipe, dst, addr_mode, instr_mod19, clear_dvalid);
}

TENSIX_EXECUTE_GATESRCRST() {
    // XXX This should verify reset_srca/b_gate_control bits
    return true;
}

TENSIX_EXECUTE_CLEARDVALID() {
    TTSIM_VERIFY(reset <= 1, UnsupportedFunctionality, "reset=%d", reset);
    if (reset & 1) {
        TTSIM_ERROR(UnsupportedFunctionality, "reset=%d is unsafe and drops SrcA/B banks: see https://github.com/tenstorrent/tt-metal/issues/22383", reset);
    } else {
        TTSIM_VERIFY(cleardvalid, UnsupportedFunctionality, "reset=%d cleardvalid=%d", reset, cleardvalid);

        if (cleardvalid & 1) {
            TTSIM_VERIFY(p_tensix->src_a_valid & (1 << p_tensix->src_a_matrix_bank), NonContractualBehavior, "SrcA bank is not valid");
            p_tensix->src_a_valid &= ~(1 << p_tensix->src_a_matrix_bank);
            if (!(reset & 2)) {
                p_tensix->src_a_matrix_bank ^= 1;
            }
        }
        if (cleardvalid & 2) {
            TTSIM_VERIFY(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank), NonContractualBehavior, "SrcB bank is not valid");
            p_tensix->src_b_valid &= ~(1 << p_tensix->src_b_matrix_bank);
            if (!(reset & 2)) {
                p_tensix->src_b_matrix_bank ^= 1;
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_SETRWC() {
    TTSIM_VERIFY(!rwc_a || (rwc_a == 8), UnsupportedFunctionality, "rwc_a=%d", rwc_a);
    TTSIM_VERIFY(!rwc_b || (rwc_b == 8), UnsupportedFunctionality, "rwc_b=%d", rwc_b);
    TTSIM_VERIFY(!(rwc_d & 3) && (rwc_d <= 12), UnsupportedFunctionality, "rwc_d=%d", rwc_d);
    TTSIM_VERIFY(rwc_cr <= 7, UnsupportedFunctionality, "rwc_cr=%d", rwc_cr);
    TTSIM_VERIFY(rwc_cr <= 4, UntestedFunctionality, "rwc_cr=%d", rwc_cr);

    if (rwc_cr & 1) {
        TTSIM_VERIFY(bit_mask & 1, UnsupportedFunctionality, "rwc_cr=%d without bit_mask=%d", rwc_cr, bit_mask);
        p_tensix->src_a_rwc[pipe] = p_tensix->src_a_rwc_cr[pipe] = (p_tensix->src_a_rwc_cr[pipe] + rwc_a) & (SRC_ROWS - 1);
    } else if (bit_mask & 1) {
        p_tensix->src_a_rwc_cr[pipe] = p_tensix->src_a_rwc[pipe] = rwc_a;
    }
    if (rwc_cr & 2) {
        TTSIM_VERIFY(bit_mask & 2, UnsupportedFunctionality, "rwc_cr=%d without bit_mask=%d", rwc_cr, bit_mask);
        p_tensix->src_b_rwc[pipe] = p_tensix->src_b_rwc_cr[pipe] = (p_tensix->src_b_rwc_cr[pipe] + rwc_b) & (SRC_ROWS - 1);
    } else if (bit_mask & 2) {
        p_tensix->src_b_rwc_cr[pipe] = p_tensix->src_b_rwc[pipe] = rwc_b;
    }
    if (bit_mask & 4) {
        if (rwc_cr & 4) {
            rwc_d = (p_tensix->dst_rwc_cr[pipe] + rwc_d) & (DST_ROWS - 1);
        }
        p_tensix->dst_rwc_cr[pipe] = p_tensix->dst_rwc[pipe] = rwc_d;
    }
    if (bit_mask & 8) {
        p_tensix->fidelity[pipe] = 0;
    }

    math_clear_src_valid(p_tensix, pipe, clear_ab_vld);
    return true;
}

TENSIX_EXECUTE_INCRWC() {
    TTSIM_VERIFY(!rwc_a || (rwc_a == 8), UnsupportedFunctionality, "rwc_a=%d", rwc_a);
    TTSIM_VERIFY(!rwc_b || (rwc_b == 8), UnsupportedFunctionality, "rwc_b=%d", rwc_b);
    TTSIM_VERIFY(!rwc_cr || (rwc_cr == 4), UnsupportedFunctionality, "rwc_cr=%d", rwc_cr);

    p_tensix->src_a_rwc[pipe] = (p_tensix->src_a_rwc[pipe] + rwc_a) & (SRC_ROWS - 1);
    p_tensix->src_b_rwc[pipe] = (p_tensix->src_b_rwc[pipe] + rwc_b) & (SRC_ROWS - 1);
    if (rwc_cr & 4) {
        p_tensix->dst_rwc_cr[pipe] = (p_tensix->dst_rwc_cr[pipe] + rwc_d) & (DST_ROWS - 1);
        p_tensix->dst_rwc[pipe] = p_tensix->dst_rwc_cr[pipe];
    } else {
        p_tensix->dst_rwc[pipe] = (p_tensix->dst_rwc[pipe] + rwc_d) & (DST_ROWS - 1);
    }
    return true;
}

static inline bool is_bfp_format(uint32_t format) {
    return (format == 2) || (format == 3) || (format == 6) || (format == 7) || (format == 11) || (format == 15); // BFP[2,4,8][a]
}

static inline uint32_t get_element_size(uint32_t format) {
    switch (format & 3) {
        case 0: return 32;
        case 1: return 16;
        case 2: return 8;
        default: break;
    }
    switch (format) {
        case 7: return 4; // bfp4
        case 15: return 2; // bfp2
        default: TTSIM_ERROR(UnimplementedFunctionality, "format=%d", format);
    }
}

static inline uint32_t denormals_as_zeros(uint32_t u) {
    if ((u & 0x7FFFFFFF) < 0x800000) {
        u &= 0x80000000;
    }
    return u;
}

static uint32_t pack_l1_acc_fp32(uint32_t dst, uint32_t src) {
    dst = denormals_as_zeros(dst);
    src = denormals_as_zeros(src);
    uint32_t ret = std::bit_cast<uint32_t>(std::bit_cast<float>(dst) + std::bit_cast<float>(src));
    if ((ret & 0x7FFFFFFF) > 0x7F800000) {
        return 0x7FC00000; // replace all NaNs with a canonical NaN
    }
    return denormals_as_zeros(ret);
}

static uint16_t pack_l1_acc_bf16(uint16_t dst, uint16_t src) {
    if (((dst == 0x7F80) && (src == 0xFF80)) || ((dst == 0xFF80) && (src == 0x7F80))) { // +inf + -inf or vice versa
        return 0x7FC0; // canonical NaN
    }
    if ((dst & 0x7FFF) < 0x80) {
        dst &= 0x8000; // denormals to zeros on input
    }
    if ((src & 0x7FFF) < 0x80) {
        src &= 0x8000; // denormals to zeros on input
    }
    double f_dst = std::bit_cast<float>(uint32_t(dst) << 16);
    if ((dst & 0x7FFF) > 0x7F80) { // NaNs are, oddly, interpreted as incredibly large normals (but infinity is handled)
        f_dst = std::bit_cast<double>((uint64_t(dst & 0x8000) << 48) | ((0x7FFull << 52) - 1)); // max fp64 normal
    }
    double f_src = std::bit_cast<float>(uint32_t(src) << 16);
    if ((src & 0x7FFF) > 0x7F80) { // NaNs are, oddly, interpreted as very large normals (but infinity is handled)
        f_src = std::bit_cast<double>((uint64_t(src & 0x8000) << 48) | (uint64_t(255 - 127 + 1023) << 52) | (uint64_t(src & 0x7F) << (52 - 7)));
    }
    uint64_t result64 = std::bit_cast<uint64_t>(f_dst + f_src);
    uint32_t s = result64 >> 63;
    int32_t e = ((result64 >> 52) & 0x7FF) - 1023 + 127;
    uint64_t m = result64 & ((1ull << 52) - 1);
    if (e <= 0) {
        return s << 15;
    } else if (e >= 255) {
        return (s << 15) | 0x7F80;
    } else {
        return (s << 15) | ((e << 7) + ((m + (1ull << (51 - 7)) - 1 + ((m >> (52 - 7)) & 1)) >> (52 - 7))); // RTNE
    }
}

TENSIX_EXECUTE_PACR() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!flush, MissingSpecification, "flush=%d", flush);
    TTSIM_VERIFY(!ctxt_ctrl, MissingSpecification, "ctxt_ctrl=%d", ctxt_ctrl);
#endif
    TTSIM_VERIFY(!ovrd_thread_id, UnsupportedFunctionality, "ovrd_thread_id=%d", ovrd_thread_id);
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!read_intf_sel || (read_intf_sel == 1) || (read_intf_sel == 3) || (read_intf_sel == 5) || (read_intf_sel == 10),
        UnimplementedFunctionality, "read_intf_sel=%d", read_intf_sel);
#else
    TTSIM_VERIFY((pack_sel == 1) || (pack_sel == 3) || (pack_sel == 15), UnimplementedFunctionality, "pack_sel=%d", pack_sel);
#endif
    TTSIM_VERIFY(!zero_write, UnimplementedFunctionality, "zero_write=%d", zero_write);
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!addr_cnt_context, MissingSpecification, "addr_cnt_context=%d", addr_cnt_context);
    TTSIM_VERIFY(!row_pad_zero, MissingSpecification, "row_pad_zero=%d", row_pad_zero);
    TTSIM_VERIFY(!cfg_context, MissingSpecification, "cfg_context=%d", cfg_context);

    // We currently require strided mode to be tied to the swizzle_32b and remap_addrs features
    if (dst_access_mode) {
        TTSIM_VERIFY(p_tensix->DEST_ACCESS_CFG_swizzle_32b, UnimplementedFunctionality,
            "dst_access_mode=%d swizzle_32b=%d", dst_access_mode, p_tensix->DEST_ACCESS_CFG_swizzle_32b);
        TTSIM_VERIFY(p_tensix->DEST_ACCESS_CFG_remap_addrs, UnimplementedFunctionality,
            "dst_access_mode=%d remap_addrs=%d", dst_access_mode, p_tensix->DEST_ACCESS_CFG_remap_addrs);
    }
#endif

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // XXX no PCK_EDGE_TILE_FACE_SET_SELECT_enable
    uint32_t tile_row_set_mapping[2][16] = { // XXX This is clunky/slow relative to using a shift, but that requires us to hardcode cfg reg nums?
        {
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_0,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_1,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_2,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_3,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_4,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_5,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_6,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_7,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_8,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_9,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_10,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_11,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_12,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_13,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_14,
            p_config->TILE_ROW_SET_MAPPING_0_row_set_mapping_15,
        },
        {
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_0,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_1,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_2,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_3,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_4,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_5,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_6,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_7,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_8,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_9,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_10,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_11,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_12,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_13,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_14,
            p_config->TILE_ROW_SET_MAPPING_1_row_set_mapping_15,
        }
    };
    TTSIM_VERIFY(!p_config->PACK_COUNTERS_SEC0_pack_yz_transposed, UnsupportedFunctionality, "pack_yz_transposed");
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(!p_config->PACK_COUNTERS_SEC1_pack_yz_transposed, UnsupportedFunctionality, "pack_yz_transposed");
    TTSIM_VERIFY(!p_config->PACK_COUNTERS_SEC2_pack_yz_transposed, UnsupportedFunctionality, "pack_yz_transposed");
    TTSIM_VERIFY(!p_config->PACK_COUNTERS_SEC3_pack_yz_transposed, UnsupportedFunctionality, "pack_yz_transposed");
#endif
    uint32_t pack_reads_per_xy_plane = p_config->PACK_COUNTERS_SEC0_pack_reads_per_xy_plane;
    TTSIM_VERIFY((pack_reads_per_xy_plane >= 1) && (pack_reads_per_xy_plane <= 16), UnsupportedFunctionality, "pack_reads_per_xy_plane=%d", pack_reads_per_xy_plane);
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(p_config->PACK_COUNTERS_SEC1_pack_reads_per_xy_plane == pack_reads_per_xy_plane, UnsupportedFunctionality, "pack_reads_per_xy_plane inconsistent");
    TTSIM_VERIFY(p_config->PACK_COUNTERS_SEC2_pack_reads_per_xy_plane == pack_reads_per_xy_plane, UnsupportedFunctionality, "pack_reads_per_xy_plane inconsistent");
    TTSIM_VERIFY(p_config->PACK_COUNTERS_SEC3_pack_reads_per_xy_plane == pack_reads_per_xy_plane, UnsupportedFunctionality, "pack_reads_per_xy_plane inconsistent");
#endif
    TTSIM_VERIFY(!p_tensix->DEST_TARGET_REG_CFG_PACK_SEC0_ZOffset, UnsupportedFunctionality, "ZOffset");
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(!p_tensix->DEST_TARGET_REG_CFG_PACK_SEC1_ZOffset, UnsupportedFunctionality, "ZOffset");
    TTSIM_VERIFY(!p_tensix->DEST_TARGET_REG_CFG_PACK_SEC2_ZOffset, UnsupportedFunctionality, "ZOffset");
    TTSIM_VERIFY(!p_tensix->DEST_TARGET_REG_CFG_PACK_SEC3_ZOffset, UnsupportedFunctionality, "ZOffset");
#endif

#if TT_ARCH_VERSION == 0
#define PACK_VERIFY_THCON0(f) \
    TTSIM_VERIFY(!p_config->THCON_SEC0_REG1_##f && !p_config->THCON_SEC0_REG8_##f && \
                 !p_config->THCON_SEC1_REG1_##f && !p_config->THCON_SEC1_REG8_##f, \
        UnsupportedFunctionality, #f)
#else
#define PACK_VERIFY_THCON0(f) \
    TTSIM_VERIFY(!p_config->THCON_SEC0_REG1_##f, UnsupportedFunctionality, #f)
#endif
    PACK_VERIFY_THCON0(Row_start_section_size);
    PACK_VERIFY_THCON0(Dis_shared_exp_assembler);
    PACK_VERIFY_THCON0(Enable_out_fifo);
    PACK_VERIFY_THCON0(Add_l1_dest_addr_offset);
    PACK_VERIFY_THCON0(Sub_l1_tile_header_size);
    PACK_VERIFY_THCON0(Source_interface_selection);
    PACK_VERIFY_THCON0(Exp_threshold_en);
#if TT_ARCH_VERSION == 1
    PACK_VERIFY_THCON0(Add_tile_header_size);
#endif
    PACK_VERIFY_THCON0(Downsample_mask);
    PACK_VERIFY_THCON0(Downsample_rate);
#if TT_ARCH_VERSION == 0
    PACK_VERIFY_THCON0(Force_pack_per_max_xy_plane);
    PACK_VERIFY_THCON0(Addr_cnt_context);
    PACK_VERIFY_THCON0(Read_mode);
#else
    PACK_VERIFY_THCON0(Pac_LF8_4b_exp);
    PACK_VERIFY_THCON0(Auto_set_last_pacr_intf_sel);
    PACK_VERIFY_THCON0(pack_start_intf_pos);
#endif
#undef PACK_VERIFY_THCON0

    uint32_t intermediate_format = p_config->ALU_FORMAT_SPEC_REG2_Dstacc;
    uint32_t pack_src_format = p_config->THCON_SEC0_REG1_In_data_format;
    uint32_t pack_dst_format = p_config->THCON_SEC0_REG1_Out_data_format;
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(pack_src_format == p_config->THCON_SEC0_REG8_In_data_format, UnsupportedFunctionality, "In_data_format inconsistent");
    TTSIM_VERIFY(pack_src_format == p_config->THCON_SEC1_REG1_In_data_format, UnsupportedFunctionality, "In_data_format inconsistent");
    TTSIM_VERIFY(pack_src_format == p_config->THCON_SEC1_REG8_In_data_format, UnsupportedFunctionality, "In_data_format inconsistent");
    TTSIM_VERIFY(pack_dst_format == p_config->THCON_SEC0_REG8_Out_data_format, UnsupportedFunctionality, "Out_data_format inconsistent");
    TTSIM_VERIFY(pack_dst_format == p_config->THCON_SEC1_REG1_Out_data_format, UnsupportedFunctionality, "Out_data_format inconsistent");
    TTSIM_VERIFY(pack_dst_format == p_config->THCON_SEC1_REG8_Out_data_format, UnsupportedFunctionality, "Out_data_format inconsistent");
#endif
    TTSIM_VERIFY(intermediate_format == pack_src_format, UndefinedBehavior,
        "intermediate_format=%d mismatches late_from_format=%d", intermediate_format, pack_src_format);
    uint32_t pack_fmt_conv_mode = (p_config->PCK_DEST_RD_CTRL_Read_32b_data << 8) | (pack_src_format << 4) | pack_dst_format;
    TTSIM_VERIFY((pack_fmt_conv_mode == 0x10) || (pack_fmt_conv_mode == 0x11) || (pack_fmt_conv_mode == 0x15) || // fp16 src, fp32/fp16/bf16 dst
                 (pack_fmt_conv_mode == 0x50) || (pack_fmt_conv_mode == 0x55) || // bf16 src, fp32/bf16 dst
                 (pack_fmt_conv_mode == 0x56) || (pack_fmt_conv_mode == 0x57) || // bf16 src, bfp8/bfp4 dst
                 (pack_fmt_conv_mode == 0x60) || (pack_fmt_conv_mode == 0x65) || (pack_fmt_conv_mode == 0x66) || // bfp8 src, fp32/bf16/bfp8 dst
                 (pack_fmt_conv_mode == 0x67) || // bfp8 src, bfp4 dst
                 (pack_fmt_conv_mode == 0x99) ||
                 (pack_fmt_conv_mode == 0x100) || (pack_fmt_conv_mode == 0x101) || (pack_fmt_conv_mode == 0x106) || (pack_fmt_conv_mode == 0x107) ||
                 ((pack_fmt_conv_mode == 0x111) && (TT_ARCH_VERSION == 1)) || // bh only fp32 src, fp16 dst
                 (pack_fmt_conv_mode == 0x155) || (pack_fmt_conv_mode == 0x166) || (pack_fmt_conv_mode == 0x167) ||
                 (pack_fmt_conv_mode == 0x188) || (pack_fmt_conv_mode == 0x199) || (pack_fmt_conv_mode == 0x1EE),
        UnimplementedFunctionality, "pack_fmt_conv_mode=0x%x", pack_fmt_conv_mode);
    if (pack_fmt_conv_mode == 0x111) {
        TTSIM_VERIFY(p_config->PCK_DEST_RD_CTRL_Round_10b_mant, UnsupportedFunctionality,
            "pack_fmt_conv_mode=0x%x round_10b_mant=%d", pack_fmt_conv_mode, p_config->PCK_DEST_RD_CTRL_Round_10b_mant);
    } else {
        TTSIM_VERIFY(!p_config->PCK_DEST_RD_CTRL_Round_10b_mant, UnimplementedFunctionality,
            "pack_fmt_conv_mode=0x%x round_10b_mant=%d", pack_fmt_conv_mode, p_config->PCK_DEST_RD_CTRL_Round_10b_mant);
    }
    uint32_t src_element_size_bits = get_element_size(pack_src_format);
    uint32_t dst_element_size_bits = get_element_size(pack_dst_format);
    uint32_t src_element_align = (src_element_size_bits + 7) / 8; // no alignment needed for 2/4-bit formats
    uint32_t ch0_x_stride = p_config->PCK0_ADDR_CTRL_XY_REG_0_Xstride;
    uint32_t ch0_y_stride = p_config->PCK0_ADDR_CTRL_XY_REG_0_Ystride;
    uint32_t ch0_z_stride = p_config->PCK0_ADDR_CTRL_ZW_REG_0_Zstride;
    uint32_t ch0_w_stride = p_config->PCK0_ADDR_CTRL_ZW_REG_0_Wstride;
    TTSIM_VERIFY(!(ch0_x_stride & (src_element_align - 1)), UndefinedBehavior, "misaligned ch0_x_stride=%d", ch0_x_stride);
    TTSIM_VERIFY(!(ch0_y_stride & (src_element_align - 1)), UndefinedBehavior, "misaligned ch0_y_stride=%d", ch0_y_stride);
    TTSIM_VERIFY(!(ch0_z_stride & (src_element_align - 1)), UndefinedBehavior, "misaligned ch0_z_stride=%d", ch0_z_stride);
    TTSIM_VERIFY(!(ch0_w_stride & (src_element_align - 1)), UndefinedBehavior, "misaligned ch0_w_stride=%d", ch0_w_stride);

    TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][2];
    TTSIM_VERIFY(!p_addr_ctrl->ch0_x, UnimplementedFunctionality, "ch0_x=%d", p_addr_ctrl->ch0_x);
    // XXX no PCK0_ADDR_BASE_REG_0_Base
    uint32_t src_addr = (p_addr_ctrl->ch0_y * ch0_y_stride +
                         p_addr_ctrl->ch0_z * ch0_z_stride +
                         p_addr_ctrl->ch0_w * ch0_w_stride) / src_element_align;
    TTSIM_VERIFY(!(src_addr % ROW_SIZE), UnimplementedFunctionality, "misaligned src_addr=%d", src_addr);

    uint32_t packer_addrs[4] = {
        p_config->THCON_SEC0_REG1_L1_Dest_addr + 1,
#if TT_ARCH_VERSION == 0
        p_config->THCON_SEC0_REG8_L1_Dest_addr + 1,
        p_config->THCON_SEC1_REG1_L1_Dest_addr + 1,
        p_config->THCON_SEC1_REG8_L1_Dest_addr + 1,
#endif
    };
#if TT_ARCH_VERSION == 0
    if (packer_addrs[0] & 0x80000000) {
        for (uint32_t i = 1; i < 4; i++) {
            packer_addrs[i] += packer_addrs[0];
        }
    }
#endif
    bool pack_l1_acc = p_config->THCON_SEC0_REG1_Pack_L1_Acc;
    if (pack_l1_acc) {
        TTSIM_VERIFY((pack_dst_format == 0) || (pack_dst_format == 5), UnsupportedFunctionality, "pack_l1_acc: pack_dst_format=%d", pack_dst_format); // fp32, bf16
    }
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(pack_l1_acc == p_config->THCON_SEC0_REG8_Pack_L1_Acc, UnsupportedFunctionality, "Pack_L1_Acc inconsistent");
    TTSIM_VERIFY(pack_l1_acc == p_config->THCON_SEC1_REG1_Pack_L1_Acc, UnsupportedFunctionality, "Pack_L1_Acc inconsistent");
    TTSIM_VERIFY(pack_l1_acc == p_config->THCON_SEC1_REG8_Pack_L1_Acc, UnsupportedFunctionality, "Pack_L1_Acc inconsistent");
#endif

    uint32_t count = flush ? 0 : (p_addr_ctrl->ch1_x - p_addr_ctrl->ch0_x + 1);
#if TT_ARCH_VERSION == 1
    if (read_intf_sel == 1) {
        TTSIM_VERIFY((count == 8) || (count == 16), UnimplementedFunctionality, "count=%d read_intf_sel=%d", count, read_intf_sel);
    } else {
        TTSIM_VERIFY(count == 16, UnimplementedFunctionality, "count=%d read_intf_sel=%d", count, read_intf_sel);
    }
    if (!read_intf_sel) { // enables all 4 read interfaces
        count *= 4;
    } else {
        count *= __builtin_popcount(read_intf_sel);
    }
    constexpr uint32_t n_packers = 1;
#else
    // XXX for now, just allow zero or any power of two <= 256
    TTSIM_VERIFY((count <= 256) && !(count & (count - 1)), UnimplementedFunctionality, "count=%d", count);
    uint32_t n_packers = (pack_sel == 15) ? 4 : (pack_sel == 3) ? 2 : 1;
#endif

    for (uint32_t packer = 0; packer < n_packers; packer++) {
        uint32_t tpg_x = p_tensix->packer_valid ? p_tensix->packer_tpg_x[packer] : 0;
        uint32_t tpg_y = p_tensix->packer_valid ? p_tensix->packer_tpg_y[packer] : 0;
        [[maybe_unused]] uint32_t tpg_z = p_tensix->packer_valid ? p_tensix->packer_tpg_z[packer] : 0;
        uint32_t edge_mask_b = (p_config->PCK_EDGE_TILE_ROW_SET_SELECT_select >> (2 * packer)) & 3;
        TTSIM_VERIFY(edge_mask_b <= 1, UnimplementedFunctionality, "edge_mask_b=%d", edge_mask_b);
        uint32_t pack_row = src_addr / ROW_SIZE;
        switch (packer) {
            default: TTSIM_ASSERT(!"invalid packer");
            case 0: pack_row += p_tensix->DEST_TARGET_REG_CFG_PACK_SEC0_Offset; break;
#if TT_ARCH_VERSION == 0
            case 1: pack_row += p_tensix->DEST_TARGET_REG_CFG_PACK_SEC1_Offset; break;
            case 2: pack_row += p_tensix->DEST_TARGET_REG_CFG_PACK_SEC2_Offset; break;
            case 3: pack_row += p_tensix->DEST_TARGET_REG_CFG_PACK_SEC3_Offset; break;
#endif
        }
        pack_row &= DST_ROWS-1;
        TTSIM_VERIFY(pack_row + ((count + ROW_SIZE-1) / ROW_SIZE) <= DST_ROWS, UnimplementedFunctionality, "pack_row=%d count=%d", pack_row, count);
        if (!p_tensix->packer_valid) {
            uint32_t addr = packer_addrs[packer];
            TTSIM_VERIFY(!p_addr_ctrl->ch1_z, UnimplementedFunctionality, "ch1_z=%d", p_addr_ctrl->ch1_z);
            TTSIM_VERIFY(!p_addr_ctrl->ch1_w, UnimplementedFunctionality, "ch1_w=%d", p_addr_ctrl->ch1_w);
#if TT_ARCH_VERSION == 1
            TTSIM_VERIFY(!p_config->PCK0_ADDR_BASE_REG_1_Base, UnimplementedFunctionality, "PCK0_ADDR_BASE_REG_1_Base=0x%x", p_config->PCK0_ADDR_BASE_REG_1_Base);
            uint32_t yzw_addr = p_addr_ctrl->ch1_y * p_config->PCK0_ADDR_CTRL_XY_REG_1_Ystride;
            addr += yzw_addr >> 4;
#else
            // XXX no PCK0_ADDR_BASE_REG_1_Base
            TTSIM_VERIFY(!p_addr_ctrl->ch1_y, UnimplementedFunctionality, "ch1_y=%d", p_addr_ctrl->ch1_y);
#endif
            p_tensix->packer_dst_exp_addr[packer] = addr << 4;
            if (dst_element_size_bits <= 8) {
                uint32_t exp_section_size;
                switch (packer) {
                    default: TTSIM_ASSERT(!"invalid packer");
                    case 0: exp_section_size = p_config->THCON_SEC0_REG1_Exp_section_size; break;
#if TT_ARCH_VERSION == 0
                    case 1: exp_section_size = p_config->THCON_SEC0_REG8_Exp_section_size; break;
                    case 2: exp_section_size = p_config->THCON_SEC1_REG1_Exp_section_size; break;
                    case 3: exp_section_size = p_config->THCON_SEC1_REG8_Exp_section_size; break;
#endif
                }
                addr += exp_section_size;
            }
            p_tensix->packer_dst_addr[packer] = addr << 4;
        }
        if (dst_element_size_bits <= 8) {
            TTSIM_VERIFY(!(count & 15), UnimplementedFunctionality, "bfp with count=%d", count);
        }
        uint32_t dst_addr = p_tensix->packer_dst_addr[packer];
        uint32_t dst_exp_addr = p_tensix->packer_dst_exp_addr[packer];
        uint16_t bfp_buffer[16];
        for (uint32_t i = 0; i < count; i++) {
            uint32_t edge_mask_c = tile_row_set_mapping[edge_mask_b][tpg_y & 15];
            TTSIM_VERIFY(edge_mask_c <= 1, UnimplementedFunctionality, "edge_mask_c=%d", edge_mask_c);
            uint32_t edge_mask = edge_mask_c ? p_config->PCK_EDGE_OFFSET_SEC1_mask : p_config->PCK_EDGE_OFFSET_SEC0_mask;
            TTSIM_VERIFY((edge_mask == 0) || (edge_mask == 1) || (edge_mask == 0xFFFF), UntestedFunctionality, "edge_mask=0x%x", edge_mask);
            TTSIM_VERIFY(!p_config->PCK_EDGE_MODE_mode, UnimplementedFunctionality, "edge_mode=%d", p_config->PCK_EDGE_MODE_mode);
            tpg_x++;
            if (tpg_x == 16) {
                tpg_x = 0;
                tpg_y++;
                if (tpg_y == pack_reads_per_xy_plane) {
                    tpg_y = 0;
                    tpg_z++;
                }
            }

            uint32_t value = 0;
            uint32_t col = i % ROW_SIZE;
            if (edge_mask & (1 << col)) {
                uint32_t row = pack_row + i / ROW_SIZE;
#if TT_ARCH_VERSION == 1
                if (dst_access_mode) {
                    TTSIM_VERIFY(!read_intf_sel || (read_intf_sel == 1) || (read_intf_sel == 3), UnimplementedFunctionality,
                        "dst_access_mode=%d read_intf_sel=%d", dst_access_mode, read_intf_sel);
                    row = pack_row + 16*(i / ROW_SIZE); // remap and swizzle cause stride to be 16 rows and not 8 here
                } else {
                    if (read_intf_sel == 5) {
                        row = pack_row + 2*(i / ROW_SIZE);
                    } else if (read_intf_sel == 10) {
                        row = pack_row + 2*(i / ROW_SIZE) + 1;
                    }
                }
#endif
                bool read_raw = p_config->PCK_DEST_RD_CTRL_Read_int8;
                bool read_unsigned = p_config->PCK_DEST_RD_CTRL_Read_unsigned;
                if (p_config->PCK_DEST_RD_CTRL_Read_32b_data) {
                    value = read_dst32b(p_tensix, row, col);
                    if (intermediate_format == 9) { // uint16
                        value >>= 16; // XXX tt-isa-docs calls this mode out as "No", should this case be illegal?
                    } else {
                        value = dst_decode_fp32(value);
                        if ((intermediate_format == 5) || (intermediate_format == 6)) {
                            TTSIM_VERIFY(!read_raw, UnimplementedFunctionality, "fp32 to bf16/bfp8: read_raw=%d", read_raw);
                            if ((value & 0x7FFFFFFF) > 0x7F800000) {
                                value = (value & 0x80000000) | 0x7F800000;
                            }
                            if (intermediate_format == 5) {
                                value = (value + 0x8000) >> 16;
                            } else {
                                value = ((value >> 16) + 1) & ~1; // reduce to m6 instead of m7
                            }
                            if ((value & 0x7FFF) < 0x80) {
                                value = 0;
                            }
                        } else if (intermediate_format == 14) {
                            TTSIM_VERIFY(!read_raw, UnimplementedFunctionality, "int32 to int8: read_raw=%d", read_raw);
                            if (read_unsigned) {
                                if (value & 0x80000000) {
                                    value = 0; // clamp negative to 0
                                } else if (value > 255) {
                                    value = 255; // clamp positive to 255
                                }
                            } else { // preserve sign bit, clamp magnitude to 127
                                value = ((value & 0x80000000) >> 24) | std::min(value & 0x7FFFFFFFu, 127u);
                            }
#if TT_ARCH_VERSION == 1
                        } else if (intermediate_format == 1) {
                            TTSIM_ERROR(UntestedFunctionality, "fp32 to fp16 intermediate_format=1");
                            if ((value & 0x7FFFFFFF) > 0x7F800000) {
                                value = (value & 0x80000000) | 0x7F800000;
                            }
                            value = (value + 0x1000) >> 13; // round to TF32
                            if ((value & 0x3FFFF) < 0x400) {
                                value = 0;
                            }
#endif
                        } else if ((intermediate_format != 0) && (intermediate_format != 8)) {
                            TTSIM_ERROR(UnimplementedFunctionality, "dst32b intermediate_format=%d", intermediate_format);
                        }
                    }
                } else {
                    value = read_dst16b(p_tensix, row, col);
                    if (intermediate_format == 1) {
                        value = dst_decode_fp16(value);
                        TTSIM_VERIFY(!read_raw, UnimplementedFunctionality, "fp16 to fp16: read_raw=%d", read_raw);
                        if ((value & 0x7FFF) < 0x400) {
                            value = 0;
                        }
                    } else if ((intermediate_format == 5) || (intermediate_format == 6)) {
                        value = dst_decode_bf16(value);
                        TTSIM_VERIFY(!read_raw, UnimplementedFunctionality, "bf16 to bf16: read_raw=%d", read_raw);
                        if ((value & 0x7FFF) > 0x7F80) {
                            value = (value & 0x8000) | 0x7F80;
                        } else if ((value & 0x7FFF) < 0x80) {
                            value = 0;
                        }
                        if (intermediate_format == 6) {
                            value = (value + 1) & ~1; // reduce to m6 instead of m7
                        }
                    } else if (intermediate_format != 9) {
                        TTSIM_ERROR(UnimplementedFunctionality, "dst16b intermediate_format=%d", intermediate_format);
                    }
                }
                if (p_config->STACC_RELU_ApplyRelu) {
                    TTSIM_VERIFY(p_config->STACC_RELU_ApplyRelu <= 3,
                        UnsupportedFunctionality, "apply_relu=%d", p_config->STACC_RELU_ApplyRelu);
                    TTSIM_VERIFY(!(p_config->STACC_RELU_ReluThreshold & 0x8000), UndefinedBehavior,
                        "negative relu_threshold=0x%x", p_config->STACC_RELU_ReluThreshold);
                    TTSIM_VERIFY((intermediate_format == 5) || (intermediate_format == 6), UnimplementedFunctionality,
                        "relu intermediate_format=%d", intermediate_format);
                    if (value & 0x8000) {
                        value = 0;
                    }
                    if ((p_config->STACC_RELU_ApplyRelu == 2) && (value <= p_config->STACC_RELU_ReluThreshold)) {
                        value = 0;
                    }
                    if ((p_config->STACC_RELU_ApplyRelu == 3) && (value > p_config->STACC_RELU_ReluThreshold)) {
                        value = p_config->STACC_RELU_ReluThreshold;
                    }
                }
#if TT_ARCH_VERSION == 1
                if ((p_config->PCK_DEST_RD_CTRL_Read_32b_data == 1) && (intermediate_format == 1)) {
                    TTSIM_ERROR(UntestedFunctionality, "fp32 to fp16 intermediate_format=1");
                    uint32_t s = value >> 18;
                    int32_t e = int32_t((value >> 10) & 255) - 112;
                    uint32_t m = value & 0x3FF;
                    if (e < 0) {
                        value = s << 15;
                    } else if (e > 31) {
                        value = (s << 15) | 0x7FFF;
                    } else {
                        value = (s << 15) | (e << 10) | m;
                    }
                }
#endif
                if ((intermediate_format == 1) && (pack_dst_format == 0)) { // fp16 -> fp32 late conversion
                    if (value & 0x7C00) { // nonzero exponent: rebias from 5-bit to 8-bit
                        value = ((value & 0x8000) << 16) | (((value & 0x7FFF) + (112 << 10)) << 13);
                    } else { // zero exponent: just widen the mantissa (no rebias), producing FP32 zero or denormal
                        value = ((value & 0x8000) << 16) | ((value & 0x3FF) << 13);
                    }
                } else if ((intermediate_format == 1) && (pack_dst_format == 5)) { // fp16 -> bf16 late conversion
                    uint32_t e = (value >> 10) & 0x1F;
                    uint32_t m = value & 0x3FF;
                    if (e == 0) {
                        value = value & 0x8000;
                    } else {
                        value = (value & 0x8000) | ((e + 112) << 7) | (m >> 3);
                    }
                } else if ((intermediate_format == 0) && (pack_dst_format == 1)) { // fp32 -> fp16 late conversion
                    uint32_t s = value >> 31;
                    int32_t e = int32_t((value >> 23) & 255) - 112;
                    uint32_t m = value & 0x7FFFFF;
                    if (e < 0) { // note: e = 0 is not flushed to zero
                        value = s << 15;
                    } else if (e > 31) {
                        value = (s << 15) | 0x7FFF;
                    } else {
                        value = (s << 15) | (e << 10) | (m >> 13);
                    }
                } else if ((intermediate_format == 0) && ((pack_dst_format == 6) || (pack_dst_format == 7))) { // fp32 -> bfp8/bfp4 late conversion
                    value >>= 16;
                    if ((value & 0x7FFF) < 0x80) {
                        value = 0; // flush denormal to zero
                    }
                } else if (((intermediate_format == 5) || (intermediate_format == 6)) && (pack_dst_format == 0)) { // bf16/bfp8 -> fp32 late conversion
                    value = value << 16;
                }
            }
            uint32_t sram_addr_bits = (dst_addr << 3) + dst_element_size_bits*i;
            uint32_t sram_addr = sram_addr_bits >> 3;
            TTSIM_VERIFY(sram_addr_bits + dst_element_size_bits <= TENSIX_SRAM_SIZE*8, UndefinedBehavior, "out of bounds sram_addr=0x%x", sram_addr);
            if (dst_element_size_bits == 32) {
                if (pack_l1_acc) {
                    value = pack_l1_acc_fp32(mem_rd<uint32_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr]), value);
                }
                mem_wr<uint32_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr], value);
            } else if (dst_element_size_bits == 16) {
                if (pack_l1_acc) {
                    value = pack_l1_acc_bf16(mem_rd<uint16_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr]), value);
                }
                mem_wr<uint16_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr], value);
            } else if (!is_bfp_format(pack_dst_format)) {
                mem_wr<uint8_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr], value);
            } else {
                bfp_buffer[i & 15] = value;
                if ((i & 15) == 15) {
                    uint32_t max_exp = 0;
                    for (uint32_t j = 0; j < 16; j++) {
                        uint32_t e = (bfp_buffer[j] >> 7) & 255;
                        max_exp = std::max(max_exp, e); // note that e=255 is treated as an extended normal here
                    }
                    uint32_t sram_exp_addr = dst_exp_addr + i/16;
                    TTSIM_VERIFY(sram_exp_addr < TENSIX_SRAM_SIZE, UndefinedBehavior, "out of bounds sram_exp_addr=0x%x", sram_exp_addr);
                    mem_wr<uint8_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_exp_addr], max_exp);
                    for (uint32_t j = 0; j < 16; j++) {
                        uint32_t s = bfp_buffer[j] >> 15;
                        uint32_t e = (bfp_buffer[j] >> 7) & 255;
                        uint32_t m = bfp_buffer[j] & 127;
                        if (e) {
                            m |= 128;
                        }
                        uint32_t shift = max_exp - e;
                        if (shift >= 8) {
                            m = 0;
                        } else {
                            m = (m + (1 << shift)) >> (shift + 1);
                            uint32_t max_m = 1 << 7;
                            if (m == max_m) {
                                m = max_m - 1;
                            }
                        }
                        m >>= 8 - dst_element_size_bits;
                        if (m) { // only set sign bit if mantissa is nonzero, since negative zero is used for -inf
                            m |= s << (dst_element_size_bits - 1);
                        }
                        uint32_t sram_addr_element_bits = sram_addr_bits - dst_element_size_bits*(15 - j);
                        uint8_t *p_byte = &g_t_tiles[p_tensix->tile_id].sram[sram_addr_element_bits >> 3];
                        if (dst_element_size_bits == 4) {
                            if (sram_addr_element_bits & 4) {
                                m = (m << 4) | (*p_byte & 0xF);
                            } else {
                                m = (m & 15) | (*p_byte & 0xF0);
                            }
                        }
                        *p_byte = m;
                    }
                }
            }
        }
        p_tensix->packer_dst_addr[packer] = dst_addr + (dst_element_size_bits * count) / 8;
        p_tensix->packer_dst_exp_addr[packer] = dst_exp_addr + count/16;
        p_tensix->packer_tpg_x[packer] = tpg_x;
        p_tensix->packer_tpg_y[packer] = tpg_y;
        p_tensix->packer_tpg_z[packer] = tpg_z;
    }
    p_tensix->packer_valid = !last && !flush;

    uint32_t y0_incr, y0_cr, y0_clear;
    uint32_t y1_incr, y1_cr, y1_clear;
    uint32_t z0_incr, z0_clear;
    uint32_t z1_incr, z1_clear;
    switch (addr_mode) {
#define ADDR_MODE(i) \
        case i: \
            y0_incr  = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_YsrcIncr; \
            y0_cr    = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_YsrcCR; \
            y0_clear = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_YsrcClear; \
            y1_incr  = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_YdstIncr; \
            y1_cr    = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_YdstCR; \
            y1_clear = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_YdstClear; \
            z0_incr  = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_ZsrcIncr; \
            z0_clear = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_ZsrcClear; \
            z1_incr  = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_ZdstIncr; \
            z1_clear = p_tensix->thread[pipe].ADDR_MOD_PACK_SEC##i##_ZdstClear; \
            break;
        ADDR_MODE(0)
        ADDR_MODE(1)
        ADDR_MODE(2)
        ADDR_MODE(3)
#undef ADDR_MODE
        default:
            TTSIM_ERROR(AssertionFailure, "addr_mode=%d", addr_mode);
    }
    TTSIM_VERIFY(!y1_cr, UnimplementedFunctionality, "y1_cr=%d", y1_cr);
    if (y0_clear) {
        p_addr_ctrl->ch0_y = p_addr_ctrl->ch0_y_cr = 0;
    } else if (y0_cr) {
        p_addr_ctrl->ch0_y_cr = (p_addr_ctrl->ch0_y_cr + y0_incr) & ADC_Y_MASK;
        p_addr_ctrl->ch0_y = p_addr_ctrl->ch0_y_cr;
    } else {
        p_addr_ctrl->ch0_y = (p_addr_ctrl->ch0_y + y0_incr) & ADC_Y_MASK;
    }
    if (y1_clear) {
        p_addr_ctrl->ch1_y = p_addr_ctrl->ch1_y_cr = 0;
    } else {
        p_addr_ctrl->ch1_y = (p_addr_ctrl->ch1_y + y1_incr) & ADC_Y_MASK;
    }
    if (z0_clear) {
        p_addr_ctrl->ch0_z = p_addr_ctrl->ch0_z_cr = 0;
    } else {
        p_addr_ctrl->ch0_z = (p_addr_ctrl->ch0_z + z0_incr) & ADC_Z_MASK;
    }
    if (z1_clear) {
        p_addr_ctrl->ch1_z = p_addr_ctrl->ch1_z_cr = 0;
    } else {
        p_addr_ctrl->ch1_z = (p_addr_ctrl->ch1_z + z1_incr) & ADC_Z_MASK;
    }

    return true;
}

static inline uint16_t bfp8_to_bf16(uint8_t x, uint8_t exp_bits) {
    uint32_t sign = x >> 7;
    uint8_t mag = x << 1;
    if (!mag) {
        return sign ? 0xFF80 : 0; // zero-magnitude encodings map to -inf and +0
    }
    uint32_t lz = __builtin_clz(mag) - 24; // perform an 8-bit CLZ
    mag <<= lz;
    exp_bits -= lz; // this will often wrap; seems odd, but spec/silicon are consistent here
    return (sign << 15) | (uint16_t(exp_bits) << 7) | (mag & 0x7E);
}

TENSIX_EXECUTE_UNPACR() {
    TTSIM_VERIFY(last, UnsupportedFunctionality, "last=%d", last);
    TTSIM_VERIFY(!search_cache_flush, UnsupportedFunctionality, "search_cache_flush=%d", search_cache_flush);
    TTSIM_VERIFY(!row_search, UnsupportedFunctionality, "row_search=%d", row_search);
    TTSIM_VERIFY(!auto_inc_context_id, UnsupportedFunctionality, "auto_inc_context_id=%d", auto_inc_context_id);
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!srcb_bcast, UnsupportedFunctionality, "srcb_bcast=%d", srcb_bcast);
#else
    TTSIM_VERIFY(!rareb_en, UnsupportedFunctionality, "rareb_en=%d", rareb_en);
#endif
    TTSIM_VERIFY(ovrd_thread_id, UnsupportedFunctionality, "ovrd_thread_id=%d", ovrd_thread_id);
    TTSIM_VERIFY(!addr_cnt_context_id, UnsupportedFunctionality, "addr_cnt_context_id=%d", addr_cnt_context_id);
    TTSIM_VERIFY(!cfg_context_id, UnsupportedFunctionality, "cfg_context_id=%d", cfg_context_id);
    TTSIM_VERIFY(!cfg_context_cnt_inc, UnsupportedFunctionality, "cfg_context_cnt_inc=%d", cfg_context_cnt_inc);

    uint32_t src_bank = unpack_block_selection ? p_tensix->src_b_unpack_bank : p_tensix->src_a_unpack_bank;
    uint32_t src_valid = unpack_block_selection ? p_tensix->src_b_valid : p_tensix->src_a_valid;
    if (src_valid & (1 << src_bank)) {
        return false; // stall until not valid
    }

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    uint32_t unpack_context = 0;
    if (ovrd_thread_id) {
        unpack_context = unpack_block_selection ? p_tensix->thread[pipe].UNPACK_MISC_CFG_CfgContextOffset_1
                                                : p_tensix->thread[pipe].UNPACK_MISC_CFG_CfgContextOffset_0;
    }
    TTSIM_VERIFY(unpack_context <= 1, UnimplementedFunctionality, "unpack_context=%d", unpack_context);

    uint32_t tile_desc0 = unpack_block_selection ? p_config->THCON_SEC1_REG0_TileDescriptor : p_config->THCON_SEC0_REG0_TileDescriptor;
#if TT_ARCH_VERSION == 1
    uint32_t tile_desc1 = unpack_block_selection ? p_config->cfg113 : p_config->cfg65;
#else
    uint32_t tile_desc1 = unpack_block_selection ? p_config->cfg93 : p_config->cfg53;
#endif
    bool is_uncompressed = bits<4,4>(tile_desc0);
    if (ovrd_thread_id) {
        if (unpack_block_selection) {
            is_uncompressed = unpack_context ? p_config->THCON_SEC1_REG2_Disable_zero_compress_cntx1
                                             : p_config->THCON_SEC1_REG2_Disable_zero_compress_cntx0;
        } else {
            is_uncompressed = unpack_context ? p_config->THCON_SEC0_REG2_Disable_zero_compress_cntx1
                                             : p_config->THCON_SEC0_REG2_Disable_zero_compress_cntx0;
        }
    }
    TTSIM_VERIFY(is_uncompressed, UnsupportedFunctionality, "is_uncompressed=0");

    uint32_t ovrd_data_format = unpack_block_selection ? p_config->THCON_SEC1_REG2_Ovrd_data_format : p_config->THCON_SEC0_REG2_Ovrd_data_format;
    if (ovrd_thread_id && ovrd_data_format) {
        TTSIM_ERROR(UnsupportedFunctionality, "ovrd_data_format");
    }
    uint32_t in_data_format = bits<3,0>(tile_desc0);
    uint32_t out_data_format = unpack_block_selection ? p_config->THCON_SEC1_REG2_Out_data_format : p_config->THCON_SEC0_REG2_Out_data_format;
    if (in_data_format == 0) { // fp32
        TTSIM_VERIFY((out_data_format == 0) || (out_data_format == 1) || (out_data_format == 4) || (out_data_format == 5), // fp32, fp16, tf32, bf16
            UndefinedBehavior, "in_data_format=%d incompatible with out_data_format=%d", in_data_format, out_data_format);
    } else {
        TTSIM_VERIFY(in_data_format == out_data_format, UndefinedBehavior, "in_data_format=%d mismatches out_data_format=%d", in_data_format, out_data_format);
    }
    uint32_t tile_x_dim = bits<31,16>(tile_desc0);
    if (ovrd_thread_id && !unpack_block_selection) { // x_dim obtained from different registers for unpack0 in this mode
        tile_x_dim = unpack_context ? p_config->THCON_SEC0_REG5_Tile_x_dim_cntx1
                                    : p_config->THCON_SEC0_REG5_Tile_x_dim_cntx0;
    }
    uint32_t tile_y_dim = bits<7,0>(tile_desc1);
    uint32_t tile_z_dim = bits<23,16>(tile_desc1);
    if (!tile_z_dim) {
        tile_z_dim = 1;
    }
    uint32_t tile_w_dim = 1; // no tile_desc2 yet, so override to 1
    uint32_t in_element_size_bits = get_element_size(in_data_format);
    uint32_t out_element_size_bits = get_element_size(out_data_format);
    uint32_t out_element_align = (out_element_size_bits + 7) / 8; // no alignment needed for 2/4-bit formats
    uint32_t ch0_y_stride = tile_x_dim;
    uint32_t ch0_z_stride = ch0_y_stride * tile_y_dim;
    uint32_t ch0_w_stride = ch0_z_stride * tile_z_dim;
    uint32_t ch1_y_stride = unpack_block_selection ? p_config->UNP1_ADDR_CTRL_XY_REG_1_Ystride : p_config->UNP0_ADDR_CTRL_XY_REG_1_Ystride;
    uint32_t ch1_z_stride = unpack_block_selection ? p_config->UNP1_ADDR_CTRL_ZW_REG_1_Zstride : p_config->UNP0_ADDR_CTRL_ZW_REG_1_Zstride;
    uint32_t ch1_w_stride = unpack_block_selection ? p_config->UNP1_ADDR_CTRL_ZW_REG_1_Wstride : p_config->UNP0_ADDR_CTRL_ZW_REG_1_Wstride;
    TTSIM_VERIFY(!(ch1_y_stride & (out_element_align - 1)), UnsupportedFunctionality, "misaligned ch1_y_stride=%d", ch1_y_stride);
    TTSIM_VERIFY(!(ch1_z_stride & (out_element_align - 1)), UnsupportedFunctionality, "misaligned ch1_z_stride=%d", ch1_z_stride);
    TTSIM_VERIFY(!(ch1_w_stride & (out_element_align - 1)), UnsupportedFunctionality, "misaligned ch1_w_stride=%d", ch1_w_stride);

    TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][unpack_block_selection];
    bool haloize = unpack_block_selection ? p_config->THCON_SEC1_REG2_Haloize_mode
                                          : p_config->THCON_SEC0_REG2_Haloize_mode;
    if (unpack_block_selection) {
        TTSIM_VERIFY(!haloize, NonContractualBehavior, "unpack1 does not support haloize");
    }
    bool tileize = unpack_block_selection ? p_config->THCON_SEC1_REG2_Tileize_mode
                                          : p_config->THCON_SEC0_REG2_Tileize_mode;
#if TT_ARCH_VERSION == 1
    const uint32_t unpack_row_width = (in_element_size_bits <= 8) ? 16 : 32;
#else
    const uint32_t unpack_row_width = 16;
#endif
    uint32_t row_stride;
    if (tileize) {
        TTSIM_VERIFY(in_element_size_bits >= 8, UndefinedBehavior, "tileize incompatible with in_element_size_bits=%d", in_element_size_bits);
        if (unpack_block_selection) {
            TTSIM_ERROR(UnimplementedFunctionality, "tileize unpack1");
        } else {
            row_stride = (p_config->THCON_SEC0_REG2_Shift_amount_cntx0 << 4) |
                         (p_config->THCON_SEC0_REG2_Shift_amount_cntx1 << 8) |
                         (p_config->THCON_SEC0_REG2_Shift_amount_cntx2 << 12);
        }
    } else {
        row_stride = (in_element_size_bits * unpack_row_width) / 8;
    }
    uint32_t base_addr = unpack_block_selection ?
        ((ovrd_thread_id && unpack_context) ? p_config->THCON_SEC1_REG3_Base_cntx1_address : p_config->THCON_SEC1_REG3_Base_address) :
        ((ovrd_thread_id && unpack_context) ? p_config->THCON_SEC0_REG3_Base_cntx1_address : p_config->THCON_SEC0_REG3_Base_address);
    uint32_t offset_addr = unpack_block_selection ?
        ((ovrd_thread_id && unpack_context) ? p_config->THCON_SEC1_REG7_Offset_cntx1_address : p_config->THCON_SEC1_REG7_Offset_address) :
        ((ovrd_thread_id && unpack_context) ? p_config->THCON_SEC0_REG7_Offset_cntx1_address : p_config->THCON_SEC0_REG7_Offset_address);
    base_addr = (base_addr + (offset_addr & 0xFFFF) + 1) << 4; // XXX no ConfigDescriptor.DigestSize
    bool force_shared_exp = unpack_block_selection ? p_config->THCON_SEC1_REG2_Force_shared_exp
                                                   : p_config->THCON_SEC0_REG2_Force_shared_exp;
    uint32_t in_addr_exponents = base_addr << 4; // slightly odd: byte address in .4 fixed point
    if (is_bfp_format(in_data_format) && !force_shared_exp) { // requires much more complex address computations and loading separate exponent stream
        TTSIM_VERIFY((!bits<5,5>(tile_desc0)), UnsupportedFunctionality, "no_bfp_exp_section");
        uint32_t num_elements = tile_x_dim * tile_y_dim * tile_z_dim * tile_w_dim; // XXX hopefully this cannot be uint64_t
        uint32_t num_exponents = (num_elements + 15) / 16; // 1 exponent per 16 sign/magnitudes
        base_addr += (num_exponents + 15) & ~15; // always make exponents a multiple of 16B
    }
    uint32_t first_datum = p_addr_ctrl->ch0_x +
                           p_addr_ctrl->ch0_y * ch0_y_stride +
                           p_addr_ctrl->ch0_z * ch0_z_stride +
                           p_addr_ctrl->ch0_w * ch0_w_stride;
    in_addr_exponents += first_datum; // 1/16 of a byte per datum
    uint32_t src_addr_bits = (base_addr << 3) + first_datum * in_element_size_bits;
    if (haloize || tileize) {
        TTSIM_VERIFY(!(src_addr_bits & 127), UndefinedBehavior, "misaligned src_addr_bits=0x%x", src_addr_bits);
    }

    bool unpack_to_dst = false;
    if (!unpack_block_selection) {
        if (ovrd_thread_id) {
            unpack_to_dst = unpack_context ? p_config->THCON_SEC0_REG2_Unpack_if_sel_cntx1 : p_config->THCON_SEC0_REG2_Unpack_if_sel_cntx0;
        } else {
            unpack_to_dst = p_config->THCON_SEC0_REG2_Unpack_If_Sel;
        }
    }
    if (unpack_to_dst) {
        TTSIM_VERIFY(!haloize, UndefinedBehavior, "unpack_to_dst cannot be used with haloize");
    }

    TTSIM_VERIFY(!p_addr_ctrl->ch1_w, UntestedFunctionality, "ch1_w=%d", p_addr_ctrl->ch1_w);
    // XXX no UNP0/1_ADDR_BASE_REG_1_Base
    uint32_t dst_addr = (p_addr_ctrl->ch1_y * ch1_y_stride +
                         p_addr_ctrl->ch1_z * ch1_z_stride +
                         p_addr_ctrl->ch1_w * ch1_w_stride) / out_element_align;
    if (ovrd_thread_id && !unpack_block_selection) {
        uint32_t context_addr = unpack_context ? p_config->THCON_SEC0_REG5_Dest_cntx1_address
                                               : p_config->THCON_SEC0_REG5_Dest_cntx0_address;
        if (unpack_to_dst || p_config->UNP0_ADD_DEST_ADDR_CNTR_add_dest_addr_cntr) {
            dst_addr += context_addr;
        } else {
            dst_addr = context_addr;
        }
    }
    if (!unpack_block_selection) {
        TTSIM_VERIFY(dst_addr >= 64, UnsupportedFunctionality, "cannot skip first 4 rows on unpack0 with dst_addr=%d", dst_addr);
        dst_addr -= 64;
    }
    TTSIM_VERIFY(!(dst_addr % ROW_SIZE), UnsupportedFunctionality, "misaligned dst_addr=%d", dst_addr);

    uint32_t upsample_rate = unpack_block_selection ? p_config->THCON_SEC1_REG2_Upsample_rate : p_config->THCON_SEC0_REG2_Upsample_rate;
    bool upsample_interleave = unpack_block_selection ? p_config->THCON_SEC1_REG2_Upsample_and_interleave : p_config->THCON_SEC0_REG2_Upsample_and_interleave;
    uint32_t col_shift = 0;
    if (!tileize && !unpack_block_selection) {
        col_shift = unpack_context ? p_config->THCON_SEC0_REG2_Shift_amount_cntx1 :
                                     p_config->THCON_SEC0_REG2_Shift_amount_cntx0;
    }
    TTSIM_VERIFY(!upsample_rate, UnsupportedFunctionality, "upsample_rate=%d", upsample_rate);
    TTSIM_VERIFY(!upsample_interleave, UnsupportedFunctionality, "upsample_interleave=%d", upsample_interleave);
    TTSIM_VERIFY(!col_shift, UnsupportedFunctionality, "col_shift=%d", col_shift);

    TTSIM_VERIFY(p_addr_ctrl->ch0_x <= p_addr_ctrl->ch1_x, UnsupportedFunctionality, "invalid ch0_x=%d ch1_x=%d", p_addr_ctrl->ch0_x, p_addr_ctrl->ch1_x);
    uint32_t count = p_addr_ctrl->ch1_x - p_addr_ctrl->ch0_x + 1;
    if (unpack_to_dst) {
        TTSIM_VERIFY(!p_tensix->thread[pipe].SRCA_SET_SetOvrdWithAddr, UndefinedBehavior, "unpack_to_dst: SRCA_SET_SetOvrdWithAddr=1");
    } else {
        TTSIM_VERIFY(p_tensix->thread[pipe].SRCA_SET_SetOvrdWithAddr, UnsupportedFunctionality, "!unpack_to_dst: SRCA_SET_SetOvrdWithAddr=0");
    }

    uint32_t throttle_mode = unpack_block_selection ? p_config->THCON_SEC1_REG2_Throttle_mode
                                                    : p_config->THCON_SEC0_REG2_Throttle_mode;
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(throttle_mode <= 2, UndefinedBehavior, "invalid throttle_mode=%d", throttle_mode);
#endif
    if (!unpack_to_dst && !unpack_block_selection && (count > ROW_SIZE)) { // check for SrcA burst drop cases
        // This logic is mildly simplified and conservative vs. exact (rather complex) silicon behavior
        uint32_t start_row = dst_addr / ROW_SIZE;
        if (in_element_size_bits == 2) {
            throttle_mode = 0; // BFP2a/BFP2 always run at x1
        } else if (tileize) {
            throttle_mode = 2; // tileize always runs at x4, regardless of Throttle_mode
#if TT_ARCH_VERSION == 1
        } else if (!p_config->THCON_SEC0_REG1_ovrd_default_throttle_mode) {
            throttle_mode = (in_element_size_bits == 8) ? 3 : 2; // 8-bit modes use x8, others use x4
#endif
        }
        uint32_t throttle_bytes = 16u << throttle_mode;
#if TT_ARCH_VERSION == 1
        if ((throttle_mode == 2) && (in_element_size_bits >= 16)) {
            throttle_bytes = 128; // upgrade to x4 "2x"
        }
#endif
        uint32_t burst_rows = throttle_bytes / (2 * in_element_size_bits); // throughput / row_bytes
        burst_rows = std::max(burst_rows, 1u);
        burst_rows = std::min(burst_rows, 8u);
        TTSIM_VERIFY(!(start_row & (burst_rows - 1)), UnsupportedFunctionality,
            "src_a start_row=%d burst_rows=%d may span 16-row set boundary and drop writes", start_row, burst_rows);
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t sram_addr_bits = src_addr_bits + in_element_size_bits*(i % unpack_row_width) + row_stride*8*(i / unpack_row_width);
        uint32_t sram_addr = sram_addr_bits >> 3;
        TTSIM_VERIFY(sram_addr_bits + in_element_size_bits <= TENSIX_SRAM_SIZE*8, UndefinedBehavior, "out of bounds sram_addr=0x%x", sram_addr);
        uint32_t value;
        if (in_element_size_bits == 32) {
            value = mem_rd<uint32_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr]);
        } else if (in_element_size_bits == 16) {
            value = mem_rd<uint16_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr]);
        } else {
            value = mem_rd<uint8_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr]);
            if (in_element_size_bits == 4) {
                value = (value >> (sram_addr_bits & 4)) & 15;
            } else if (in_element_size_bits == 2) {
                value = (value >> (sram_addr_bits & 6)) & 3;
            }
        }
        uint8_t exp_bits = 0;
        if (is_bfp_format(in_data_format)) {
            if (force_shared_exp) {
                exp_bits = unpack_block_selection ? p_config->UNP1_FORCED_SHARED_EXP_shared_exp
                                                  : p_config->UNP0_FORCED_SHARED_EXP_shared_exp;
            } else {
                uint32_t exp_addr = (in_addr_exponents + i) / 16;
                TTSIM_VERIFY(exp_addr < TENSIX_SRAM_SIZE, UndefinedBehavior, "out of bounds exp_addr=0x%x", exp_addr);
                exp_bits = mem_rd<uint8_t>(&g_t_tiles[p_tensix->tile_id].sram[exp_addr]);
            }
        }
        if (in_data_format == 0) { // fp32
            if (out_data_format == 0) { // fp32
                TTSIM_VERIFY(unpack_to_dst, UndefinedBehavior, "unpack_to_dst=%d in_data_format=%d out_data_format=%d",
                    unpack_to_dst, in_data_format, out_data_format);
                value = dst_encode_fp32(value);
            } else if (out_data_format == 1) { // fp16
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d out_data_format=%d",
                    unpack_to_dst, in_data_format, out_data_format);
                uint32_t s = value & 0x80000000;
                int32_t e = ((value >> 23) & 255) - 112;
                if (e < 0) {
                    value = s;
                } else if (e > 31) {
                    value = s | (31 << 23) | 0x7FE000;
                } else {
                    value = s | (e << 23) | (value & 0x7FE000);
                }
            } else if (out_data_format == 4) { // tf32
                if (unpack_to_dst) {
                    value = dst_encode_fp32(value);
                } else {
                    value &= 0xFFFFE000; // drop low 13 bits of mantissa (no rounding)
                }
            } else if (out_data_format == 5) { // bf16
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d out_data_format=%d",
                    unpack_to_dst, in_data_format, out_data_format);
                value = denormals_as_zeros(value);
                value &= 0xFFFF0000; // drop low 16 bits of mantissa (no rounding)
            } else {
                TTSIM_ERROR(AssertionFailure, "unpack_to_dst=%d in_data_format=%d out_data_format=%d", unpack_to_dst, in_data_format, out_data_format);
            }
        } else {
            if (in_data_format == 1) {
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                value = ((value & 0x8000) << 16) | ((value & 0x7FFF) << 13);
            } else if (in_data_format == 5) {
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                value <<= 16; // bf16 -> fp32 in Src
            } else if (in_data_format == 6) {
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                value = uint32_t(bfp8_to_bf16(value, exp_bits)) << 16; // bfp8 -> bf16 -> fp32 in Src
            } else if (in_data_format == 7) {
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                value = uint32_t(bfp8_to_bf16(value << 4, exp_bits)) << 16; // bfp4 -> bfp8 -> bf16 -> fp32 in Src
            } else if (in_data_format == 15) {
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                value = uint32_t(bfp8_to_bf16(value << 6, exp_bits)) << 16; // bfp2 -> bfp8 -> bf16 -> fp32 in Src
            } else if (in_data_format == 8) {
                TTSIM_VERIFY(unpack_to_dst, UndefinedBehavior, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                value = dst_encode_fp32(value);
            } else if (in_data_format == 9) {
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                value = uint32_t(dst_decode_bf16(value)) << 16;
            } else if (in_data_format == 14) {
                TTSIM_VERIFY(!unpack_to_dst, UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                bool is_unsigned = unpack_block_selection ? p_config->ALU_FORMAT_SPEC_REG0_SrcBUnsigned
                                                          : p_config->ALU_FORMAT_SPEC_REG0_SrcAUnsigned;
                uint32_t sign = is_unsigned ? 0 : (value & 0x80);
                value -= sign;
                if (value) {
                    value |= 16 << 10;
                }
                value |= sign << 8;
                value = ((value & 0x8000) << 16) | ((value & 0x7FFF) << 13);
            } else {
               TTSIM_ERROR(UnimplementedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
            }
        }
        if (zero_write2) {
            TTSIM_ERROR(UntestedFunctionality, "zero_write2=%d", zero_write2);
            value = 0;
        }
        uint32_t row = (dst_addr + i) / ROW_SIZE;
        uint32_t col = (dst_addr + i) % ROW_SIZE;
        if (unpack_to_dst) {
            TTSIM_VERIFY(row < DST_ROWS, UnimplementedFunctionality, "dst row=%d", row);
            if (out_element_size_bits == 32) {
                write_dst32b(p_tensix, row, col, value);
            } else {
                write_dst16b(p_tensix, row, col, value);
            }
        } else if (unpack_block_selection) {
            TTSIM_VERIFY(row < SRC_ROWS, UndefinedBehavior, "src_b row=%d", row);
            p_tensix->src_b[src_bank][row][col] = value;
        } else {
#if TT_ARCH_VERSION == 1
            row &= SRC_ROWS-1;
#else
            TTSIM_VERIFY(row < SRC_ROWS, UnsupportedFunctionality, "src_a row=%d", row);
#endif
            if (haloize) {
                uint32_t row_low_bits = row & 15;
                std::swap(row_low_bits, col);
                row = (row & ~15) | row_low_bits;
            }
            p_tensix->src_a[src_bank][row][col] = value;
        }
    }

    p_addr_ctrl->ch0_z = (p_addr_ctrl->ch0_z + bits<1,0>(addr_mode)) & ADC_Z_MASK;
    p_addr_ctrl->ch0_y = (p_addr_ctrl->ch0_y + bits<3,2>(addr_mode)) & ADC_Y_MASK;
    p_addr_ctrl->ch1_z = (p_addr_ctrl->ch1_z + bits<5,4>(addr_mode)) & ADC_Z_MASK;
    p_addr_ctrl->ch1_y = (p_addr_ctrl->ch1_y + bits<7,6>(addr_mode)) & ADC_Y_MASK;

    if (set_dat_valid) {
        if (unpack_block_selection) {
            TTSIM_VERIFY(!(p_tensix->src_b_valid & (1 << src_bank)), NonContractualBehavior, "SrcB bank is already valid");
            p_tensix->src_b_valid |= 1 << src_bank;
            p_tensix->src_b_unpack_bank ^= 1;
            // XXX no SRCB_SET_Base
        } else {
            TTSIM_VERIFY(!(p_tensix->src_a_valid & (1 << src_bank)), NonContractualBehavior, "SrcA bank is already valid");
            p_tensix->src_a_valid |= 1 << src_bank;
            p_tensix->src_a_unpack_bank ^= 1;
            TTSIM_VERIFY(!p_tensix->thread[pipe].SRCA_SET_Base, UnimplementedFunctionality, "SRCA_SET_Base");
        }
#if TT_ARCH_VERSION >= 1
        if (unpack_block_selection) {
            p_tensix->src_b_format[src_bank] = out_data_format;
        } else {
            p_tensix->src_a_format[src_bank] = out_data_format;
        }
#endif
    } else {
        TTSIM_VERIFY(!p_config->THCON_SEC0_REG2_Unpack_Src_Reg_Set_Upd, UnsupportedFunctionality, "unpack0 Unpack_Src_Reg_Set_Upd");
        TTSIM_VERIFY(!p_config->THCON_SEC1_REG2_Unpack_Src_Reg_Set_Upd, UnsupportedFunctionality, "unpack1 Unpack_Src_Reg_Set_Upd");
    }
    return true;
}

TENSIX_EXECUTE_UNPACR_NOP() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(unpack_pop == 1, UnimplementedFunctionality, "unpack_pop=%d", unpack_pop);
    TTSIM_VERIFY(src_clr_val_ctrl <= 1, UnimplementedFunctionality, "src_clr_val_ctrl=%d", src_clr_val_ctrl);
    TTSIM_VERIFY(!bank_clr_ctrl, UnimplementedFunctionality, "bank_clr_ctrl=%d", bank_clr_ctrl);
    TTSIM_VERIFY(!clr_to1_fmt_ctrl, UnimplementedFunctionality, "clr_to1_fmt_ctrl=%d", clr_to1_fmt_ctrl);
    TTSIM_VERIFY(!msg_clr_cnt, UnsupportedFunctionality, "msg_clr_cnt=%d", msg_clr_cnt);
    TTSIM_VERIFY(!stream_id, UnsupportedFunctionality, "stream_id=%d", stream_id);
    uint32_t unpack_block_selection = unpacker_select; // this field was renamed
    uint32_t stall_and_clear = 1; // implied by unpack_pop == 1

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
#else
    TTSIM_VERIFY((no_op == 1) || (no_op == 5) || (no_op == 7), UnsupportedFunctionality, "no_op=%d", no_op);
    uint32_t stall_and_clear = (no_op == 1) || (no_op == 5);
    uint32_t src_clr_val_ctrl = (no_op == 5); // clear to negative inf
    uint32_t stall_clr_cntrl = 0; // called wait_like_unpacr in tt-isa-documentation
    uint32_t set_dvalid = (no_op == 7);
#endif

    uint32_t unpack_bank = unpack_block_selection ? p_tensix->src_b_unpack_bank : p_tensix->src_a_unpack_bank;
    if (stall_and_clear) {
        uint32_t matrix_bank = unpack_block_selection ? p_tensix->src_b_matrix_bank : p_tensix->src_a_matrix_bank;
        uint32_t src_valid = unpack_block_selection ? p_tensix->src_b_valid : p_tensix->src_a_valid;
        uint32_t wait_bank = stall_clr_cntrl ? unpack_bank : matrix_bank;
        if (src_valid & (1 << wait_bank)) {
            return false; // stall until not valid
        }

        if (unpack_block_selection) {
            TTSIM_VERIFY(!src_clr_val_ctrl, UnimplementedFunctionality, "unpack_block_selection=%d src_clr_val_ctrl=%d", unpack_block_selection, src_clr_val_ctrl);
            memset(p_tensix->src_b[unpack_bank], 0, sizeof(p_tensix->src_b[unpack_bank]));
        } else if (src_clr_val_ctrl) {
            for (uint32_t row = 0; row < SRC_ROWS; row++) {
                for (uint32_t col = 0; col < ROW_SIZE; col++) {
                    p_tensix->src_a[unpack_bank][row][col] = 0xFFFFE000; // "negative inf" from an FPU perspective
                }
            }
        } else {
            memset(p_tensix->src_a[unpack_bank], 0, sizeof(p_tensix->src_a[unpack_bank]));
        }
    }
    if (set_dvalid) {
        if (unpack_block_selection) {
            TTSIM_VERIFY(!(p_tensix->src_b_valid & (1 << unpack_bank)), NonContractualBehavior, "SrcB bank is already valid");
            p_tensix->src_b_valid |= 1 << unpack_bank;
            p_tensix->src_b_unpack_bank ^= 1;
            // XXX no SRCB_SET_Base
        } else {
            TTSIM_VERIFY(!(p_tensix->src_a_valid & (1 << unpack_bank)), NonContractualBehavior, "SrcA bank is already valid");
            p_tensix->src_a_valid |= 1 << unpack_bank;
            p_tensix->src_a_unpack_bank ^= 1;
            TTSIM_VERIFY(!p_tensix->thread[pipe].SRCA_SET_Base, UnimplementedFunctionality, "SRCA_SET_Base");
        }
#if TT_ARCH_VERSION >= 1
        if (unpack_block_selection) {
            p_tensix->src_b_format[unpack_bank] = p_config->THCON_SEC1_REG2_Out_data_format;
        } else {
            p_tensix->src_a_format[unpack_bank] = p_config->THCON_SEC0_REG2_Out_data_format;
        }
#endif
    }
    return true;
}

TENSIX_EXECUTE_SETDMAREG() {
    uint32_t hi = reg_index_16b & 1;
    uint32_t reg = reg_index_16b >> 1;
    TTSIM_ASSERT(reg < std::size(p_tensix->dma_regs[pipe]));
    TTSIM_VERIFY(!set_signals_mode, UnsupportedFunctionality, "set_signals_mode=%d", set_signals_mode);
    uint32_t data = payload_sig_sel | (payload_sig_sel_size << 14);

    if (hi) {
        p_tensix->dma_regs[pipe][reg] = (p_tensix->dma_regs[pipe][reg] & 0xFFFF) | (data << 16);
    } else {
        p_tensix->dma_regs[pipe][reg] = (p_tensix->dma_regs[pipe][reg] & 0xFFFF0000) | data;
    }
    return true;
}

TENSIX_EXECUTE_REG2FLOP() {
#if TT_ARCH_VERSION == 1
    TTSIM_ERROR_NOFMT(UnsupportedFunctionality);
#else
    TTSIM_ASSERT(reg_index < std::size(p_tensix->dma_regs[pipe]));
    TTSIM_VERIFY(flop_index < 152 - 52, UndefinedBehavior, "flop_index=%d", flop_index);
    TTSIM_VERIFY(!context_id_2, UnsupportedFunctionality, "context_id_2=%d", context_id_2);
    TTSIM_VERIFY(!byte_offset, UnsupportedFunctionality, "byte_offset=%d", byte_offset);
    TTSIM_VERIFY(!target_sel, UnsupportedFunctionality, "target_sel=%d", target_sel);
    TTSIM_VERIFY((size_sel == 1) || (size_sel == 2), UnsupportedFunctionality, "size_sel=%d", size_sel); // XXX we are ignoring this right now

    uint32_t state_id = get_state_id(p_tensix, pipe);
    tensix_cfg_wr32(p_tensix, state_id, 4*(flop_index + 52), p_tensix->dma_regs[pipe][reg_index]);
    return true;
#endif
}

TENSIX_EXECUTE_SETADC() {
    TTSIM_VERIFY(value <= 0xFFFF, UnsupportedFunctionality, "value=0x%x", value);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);
    TTSIM_VERIFY(value <= 15, UntestedFunctionality, "value=%d", value);
    TTSIM_VERIFY(!channel_index, UnimplementedFunctionality, "channel_index=%d", channel_index); // set CH0
    TTSIM_VERIFY((cnt_set_mask >= 1) && (cnt_set_mask <= 4), UntestedFunctionality, "cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            switch (dimension_index) {
                case 2: p_addr_ctrl->ch0_z_cr = p_addr_ctrl->ch0_z = value; break;
                case 3: p_addr_ctrl->ch0_w_cr = p_addr_ctrl->ch0_w = value; break;
                default: TTSIM_ERROR(UnimplementedFunctionality, "dimension_index=%d", dimension_index);
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_SETADCXY() {
    TTSIM_VERIFY(bit_mask, UnsupportedFunctionality, "no-op mask: bit_mask=%d", bit_mask);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);
    TTSIM_VERIFY(!ch0_x, UntestedFunctionality, "ch0_x=%d", ch0_x);
    TTSIM_VERIFY(ch0_y <= 1, UntestedFunctionality, "ch0_y=%d", ch0_y);
    TTSIM_VERIFY(!ch1_x, UntestedFunctionality, "ch1_x=%d", ch1_x);
    TTSIM_VERIFY(!ch1_y, UntestedFunctionality, "ch1_y=%d", ch1_y);
    TTSIM_VERIFY((cnt_set_mask >= 1) && (cnt_set_mask <= 4), UntestedFunctionality, "cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            if (bit_mask & 1) {
                p_addr_ctrl->ch0_x_cr = p_addr_ctrl->ch0_x = ch0_x;
            }
            if (bit_mask & 2) {
                p_addr_ctrl->ch0_y_cr = p_addr_ctrl->ch0_y = ch0_y;
            }
            if (bit_mask & 4) {
                p_addr_ctrl->ch1_x_cr = p_addr_ctrl->ch1_x = ch1_x;
            }
            if (bit_mask & 8) {
                p_addr_ctrl->ch1_y_cr = p_addr_ctrl->ch1_y = ch1_y;
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_INCADCXY() {
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);
    TTSIM_VERIFY((cnt_set_mask >= 1) && (cnt_set_mask <= 4), UntestedFunctionality, "cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            p_addr_ctrl->ch0_x = (p_addr_ctrl->ch0_x + ch0_x) & ADC_X_MASK;
            p_addr_ctrl->ch0_y = (p_addr_ctrl->ch0_y + ch0_y) & ADC_Y_MASK;
            p_addr_ctrl->ch1_x = (p_addr_ctrl->ch1_x + ch1_x) & ADC_X_MASK;
            p_addr_ctrl->ch1_y = (p_addr_ctrl->ch1_y + ch1_y) & ADC_Y_MASK;
        }
    }
    return true;
}

TENSIX_EXECUTE_ADDRCRXY() {
    TTSIM_VERIFY(bit_mask, UnsupportedFunctionality, "no-op mask: bit_mask=%d", bit_mask);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);
    TTSIM_VERIFY(bit_mask == 2, UntestedFunctionality, "bit_mask=%d", bit_mask);
    TTSIM_VERIFY(ch0_x <= 1, UntestedFunctionality, "ch0_x=%d", ch0_x);
    TTSIM_VERIFY(ch0_y <= 1, UntestedFunctionality, "ch0_y=%d", ch0_y);
    TTSIM_VERIFY(ch1_x <= 1, UntestedFunctionality, "ch1_x=%d", ch1_x);
    TTSIM_VERIFY(ch1_y <= 1, UntestedFunctionality, "ch1_y=%d", ch1_y);
    TTSIM_VERIFY((cnt_set_mask >= 1) && (cnt_set_mask <= 4), UntestedFunctionality, "cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            if (bit_mask & 1) {
                TTSIM_ERROR(UntestedFunctionality, "ch0_x");
                p_addr_ctrl->ch0_x_cr = (p_addr_ctrl->ch0_x_cr + ch0_x) & ADC_X_MASK;
                p_addr_ctrl->ch0_x = p_addr_ctrl->ch0_x_cr;
            }
            if (bit_mask & 2) {
                p_addr_ctrl->ch0_y_cr = (p_addr_ctrl->ch0_y_cr + ch0_y) & ADC_Y_MASK;
                p_addr_ctrl->ch0_y = p_addr_ctrl->ch0_y_cr;
            }
            if (bit_mask & 4) {
                TTSIM_ERROR(UntestedFunctionality, "ch1_x");
                p_addr_ctrl->ch1_x_cr = (p_addr_ctrl->ch1_x_cr + ch1_x) & ADC_X_MASK;
                p_addr_ctrl->ch1_x = p_addr_ctrl->ch1_x_cr;
            }
            if (bit_mask & 8) {
                TTSIM_ERROR(UntestedFunctionality, "ch1_y");
                p_addr_ctrl->ch1_y_cr = (p_addr_ctrl->ch1_y_cr + ch1_y) & ADC_Y_MASK;
                p_addr_ctrl->ch1_y = p_addr_ctrl->ch1_y_cr;
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_SETADCZW() {
    TTSIM_VERIFY(bit_mask, UnsupportedFunctionality, "no-op mask: bit_mask=%d", bit_mask);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);
    TTSIM_VERIFY(ch0_x <= 2, UntestedFunctionality, "ch0_x=%d", ch0_x);
    TTSIM_VERIFY(ch0_y <= 1, UntestedFunctionality, "ch0_y=%d", ch0_y);
    TTSIM_VERIFY(ch1_x <= 2, UntestedFunctionality, "ch1_x=%d", ch1_x);
    TTSIM_VERIFY(!ch1_y, UntestedFunctionality, "ch1_y=%d", ch1_y);
    TTSIM_VERIFY((cnt_set_mask >= 1) && (cnt_set_mask <= 4), UntestedFunctionality, "cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            if (bit_mask & 1) {
                p_addr_ctrl->ch0_z_cr = p_addr_ctrl->ch0_z = ch0_x;
            }
            if (bit_mask & 2) {
                p_addr_ctrl->ch0_w_cr = p_addr_ctrl->ch0_w = ch0_y;
            }
            if (bit_mask & 4) {
                p_addr_ctrl->ch1_z_cr = p_addr_ctrl->ch1_z = ch1_x;
            }
            if (bit_mask & 8) {
                p_addr_ctrl->ch1_w_cr = p_addr_ctrl->ch1_w = ch1_y;
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_INCADCZW() {
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);
    TTSIM_VERIFY(ch0_x <= 6, UntestedFunctionality, "ch0_x=%d", ch0_x);
    TTSIM_VERIFY(ch0_y <= 2, UntestedFunctionality, "ch0_y=%d", ch0_y);
    TTSIM_VERIFY(!ch1_x, UntestedFunctionality, "ch1_x=%d", ch1_x);
    TTSIM_VERIFY(!ch1_y, UntestedFunctionality, "ch1_y=%d", ch1_y);
    TTSIM_VERIFY((cnt_set_mask >= 1) && (cnt_set_mask <= 4), UntestedFunctionality, "cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            p_addr_ctrl->ch0_z = (p_addr_ctrl->ch0_z + ch0_x) & ADC_Z_MASK;
            p_addr_ctrl->ch0_w = (p_addr_ctrl->ch0_w + ch0_y) & ADC_W_MASK;
            p_addr_ctrl->ch1_z = (p_addr_ctrl->ch1_z + ch1_x) & ADC_Z_MASK;
            p_addr_ctrl->ch1_w = (p_addr_ctrl->ch1_w + ch1_y) & ADC_W_MASK;
        }
    }
    return true;
}

TENSIX_EXECUTE_ADDRCRZW() {
    TTSIM_VERIFY(bit_mask, UnsupportedFunctionality, "no-op mask: bit_mask=%d", bit_mask);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);
    TTSIM_VERIFY((bit_mask == 1) || (bit_mask == 2) || (bit_mask == 3), UntestedFunctionality, "bit_mask=%d", bit_mask);
    TTSIM_VERIFY(!ch0_x, UntestedFunctionality, "ch0_x=%d", ch0_x);
    TTSIM_VERIFY(ch0_y <= 1, UntestedFunctionality, "ch0_y=%d", ch0_y);
    TTSIM_VERIFY(!ch1_x, UntestedFunctionality, "ch1_x=%d", ch1_x);
    TTSIM_VERIFY(!ch1_y, UntestedFunctionality, "ch1_y=%d", ch1_y);
    TTSIM_VERIFY((cnt_set_mask >= 1) && (cnt_set_mask <= 4), UntestedFunctionality, "cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            if (bit_mask & 1) {
                p_addr_ctrl->ch0_z_cr = (p_addr_ctrl->ch0_z_cr + ch0_x) & ADC_Z_MASK;
                p_addr_ctrl->ch0_z = p_addr_ctrl->ch0_z_cr;
            }
            if (bit_mask & 2) {
                p_addr_ctrl->ch0_w_cr = (p_addr_ctrl->ch0_w_cr + ch0_y) & ADC_W_MASK;
                p_addr_ctrl->ch0_w = p_addr_ctrl->ch0_w_cr;
            }
            if (bit_mask & 4) {
                TTSIM_ERROR(UntestedFunctionality, "ch1_z");
                p_addr_ctrl->ch1_z_cr = (p_addr_ctrl->ch1_z_cr + ch1_x) & ADC_Z_MASK;
                p_addr_ctrl->ch1_z = p_addr_ctrl->ch1_z_cr;
            }
            if (bit_mask & 8) {
                TTSIM_ERROR(UntestedFunctionality, "ch1_w");
                p_addr_ctrl->ch1_w_cr = (p_addr_ctrl->ch1_w_cr + ch1_y) & ADC_W_MASK;
                p_addr_ctrl->ch1_w = p_addr_ctrl->ch1_w_cr;
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_SETDVALID() {
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(setvalid, UnsupportedFunctionality, "setvalid=%d", setvalid);
    TTSIM_VERIFY((setvalid == 2) || (setvalid == 3), UntestedFunctionality, "setvalid=%d", setvalid);

    if (setvalid & 1) {
        uint32_t unpack_bank = p_tensix->src_a_unpack_bank;
        TTSIM_VERIFY(!(p_tensix->src_a_valid & (1 << unpack_bank)), NonContractualBehavior, "SrcA bank is already valid");
        p_tensix->src_a_valid |= 1 << unpack_bank;
        p_tensix->src_a_unpack_bank = unpack_bank ^ 1;
        TTSIM_VERIFY(!p_tensix->thread[pipe].SRCA_SET_Base, UnimplementedFunctionality, "SRCA_SET_Base");
    }
    if (setvalid & 2) {
        uint32_t unpack_bank = p_tensix->src_b_unpack_bank;
        TTSIM_VERIFY(!(p_tensix->src_b_valid & (1 << unpack_bank)), NonContractualBehavior, "SrcB bank is already valid");
        p_tensix->src_b_valid |= 1 << unpack_bank;
        p_tensix->src_b_unpack_bank = unpack_bank ^ 1;
        // XXX no SRCB_SET_Base
    }
    return true;
#else
    TTSIM_ERROR_NOFMT(MissingSpecification); // XXX Unclear what src_*_format should be set to here
#endif
}

TENSIX_EXECUTE_ADDDMAREG() {
    TTSIM_VERIFY(op_a_reg_index < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "op_a_reg_index=%d out of range", op_a_reg_index);
    TTSIM_VERIFY(op_b_reg_index < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "op_b_reg_index=%d out of range", op_b_reg_index);
    TTSIM_VERIFY(result_reg_index < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "result_reg_index=%d out of range", result_reg_index);
    TTSIM_VERIFY(!op_b_is_const, UnimplementedFunctionality, "op_b_is_const=%d", op_b_is_const);

    p_tensix->dma_regs[pipe][result_reg_index] = p_tensix->dma_regs[pipe][op_a_reg_index] + p_tensix->dma_regs[pipe][op_b_reg_index];
    return true;
}

TENSIX_EXECUTE_MULDMAREG() {
    TTSIM_VERIFY(op_a_reg_index < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "op_a_reg_index=%d out of range", op_a_reg_index);
    TTSIM_VERIFY(op_b_reg_index < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "op_b_reg_index=%d out of range", op_b_reg_index);
    TTSIM_VERIFY(result_reg_index < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "result_reg_index=%d out of range", result_reg_index);
    TTSIM_VERIFY(!op_b_is_const, UnimplementedFunctionality, "op_b_is_const=%d", op_b_is_const);

    uint32_t src0 = p_tensix->dma_regs[pipe][op_a_reg_index] & 0xFFFF;
    uint32_t src1 = p_tensix->dma_regs[pipe][op_b_reg_index] & 0xFFFF;
    p_tensix->dma_regs[pipe][result_reg_index] = src0 * src1;
    return true;
}

TENSIX_EXECUTE_SETADCXX() {
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);
    TTSIM_VERIFY(!x_start, UnsupportedFunctionality, "x_start=%d", x_start);
    TTSIM_VERIFY(x_end2 >= 7, UntestedFunctionality, "x_end2=%d", x_end2);
    TTSIM_VERIFY((cnt_set_mask >= 1) && (cnt_set_mask <= 4), UntestedFunctionality, "cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            p_addr_ctrl->ch0_x = p_addr_ctrl->ch0_x_cr = x_start;
            p_addr_ctrl->ch1_x = p_addr_ctrl->ch1_x_cr = x_end2;
        }
    }
    return true;
}

TENSIX_EXECUTE_DMANOP() { return true; }

TENSIX_EXECUTE_STOREREG() {
    TTSIM_VERIFY(tdma_data_reg_index < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "tdma_data_reg_index=%d out of range", tdma_data_reg_index);
    uint32_t addr = 0xFFB00000 | (reg_addr << 2);

    // Be careful here to avoid infinite recursion by injecting instructions into pipe
    // For safety, we only allow overlay registers to be written via this path
    switch (addr) {
        case NOC_OVERLAY_BASE ... NOC_OVERLAY_BASE + 0x3FFFF: {
            bool done = tile_mmio_wr32('T', p_tensix->tile_id, 0xFFFFFFFF, addr, p_tensix->dma_regs[pipe][tdma_data_reg_index]); // no riscv_id in this path
            TTSIM_VERIFY(done, AssertionFailure, "tile_mmio_wr32 failed: addr=0x%llx", addr); // needs to be nonblocking
            break;
        }
        default:
            TTSIM_ERROR(UnsupportedFunctionality, "disallowed addr=0x%x", addr);
    }
    return true;
}

template<typename Func>
static inline void for_each_lane(uint32_t mask, Func f) {
    for (; mask; mask &= mask-1) {
        uint32_t lane = __builtin_ctz(mask);
        f(lane);
    }
}

TENSIX_EXECUTE_SFPLOAD() {
    TTSIM_VERIFY(!(dest_reg_addr & 1), UnimplementedFunctionality, "dest_reg_addr=%d", dest_reg_addr);
    TTSIM_VERIFY(!instr_mod0 || (instr_mod0 == 2) || (instr_mod0 == 3) || (instr_mod0 == 4) || (instr_mod0 == 6) ||
                 (instr_mod0 == 12) || (instr_mod0 == 14) || (instr_mod0 == 15),
        UnimplementedFunctionality, "instr_mod0=%d", instr_mod0);
    TTSIM_VERIFY(lreg_ind < 12, UnimplementedFunctionality, "lreg_ind=%d", lreg_ind);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    if (!instr_mod0) {
        if (p_config->ALU_ACC_CTRL_SFPU_Fp32_enabled) {
            instr_mod0 = 3;
        } else {
            uint32_t src_b_fmt = p_config->ALU_FORMAT_SPEC_REG1_SrcB;
            if ((src_b_fmt == 0) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7) || (src_b_fmt == 9)) { // fp32, bf16, bfp8, bfp4, int16
                instr_mod0 = 2;
            } else {
                TTSIM_VERIFY(src_b_fmt == 1, UnimplementedFunctionality, "src_b_fmt=%d", src_b_fmt); // fp16
                instr_mod0 = 1;
            }
        }
    }

    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dest_reg_addr + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    dst_row &= DST_ROWS-1;
    TTSIM_VERIFY(!(dst_row & 1), UnsupportedFunctionality, "dst_row=%d", dst_row);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t row = (dst_row & ~3) + lane / 8;
        uint32_t col = 2*(lane & 7) + ((dst_row & 2) >> 1);
        uint32_t value;
        if (instr_mod0 == 1) {
            value = read_dst16b(p_tensix, row, col);
            uint32_t s = value >> 15;
            uint32_t e = value & 31;
            uint32_t m = (value >> 5) & 1023;
            // XXX no ENABLE_FP16A_INF
            if (e) {
                e += 112;
            }
            value = (s << 31) | (e << 23) | (m << 13);
        } else if (instr_mod0 == 2) {
            value = dst_decode_bf16(read_dst16b(p_tensix, row, col));
            value <<= 16;
        } else if ((instr_mod0 == 3) || (instr_mod0 == 4)) {
            value = dst_decode_fp32(read_dst32b(p_tensix, row, col));
        } else if (instr_mod0 == 6) {
            value = read_dst16b(p_tensix, row, col);
        } else if (instr_mod0 == 12) {
            value = dst_decode_fp32(read_dst32b(p_tensix, row, col));
#if TT_ARCH_VERSION == 0
            if (value & 0x80000000) {
                value = -int32_t(value & 0x7FFFFFFF);
            }
#endif
        } else if (instr_mod0 == 14) {
            value = (p_tensix->l_regs[lreg_ind][lane] & 0xFFFF0000) | uint32_t(read_dst16b(p_tensix, row, col));
        } else if (instr_mod0 == 15) {
            value = (uint32_t(read_dst16b(p_tensix, row, col)) << 16) | (p_tensix->l_regs[lreg_ind][lane] & 0xFFFF);
        } else {
            TTSIM_ERROR(AssertionFailure, "instr_mod0=%d", instr_mod0);
        }
        if (lreg_ind < 8) {
            p_tensix->l_regs[lreg_ind][lane] = value;
        }
    });

    math_update_counters(p_tensix, pipe, sfpu_addr_mode, false);
    return true;
}

TENSIX_EXECUTE_SFPLOADI() {
    TTSIM_VERIFY(lreg_ind < 8, UnsupportedFunctionality, "lreg_ind=%d", lreg_ind);

    uint32_t val;
    switch (instr_mod0) {
        case 0: val = imm16 << 16; break; // treat imm16 as bfloat16
        case 1: { // fp16 -> fp32 by biasing the exponent (no special cases for inf/nan)
            uint32_t s = imm16 & 0x8000;
            uint32_t em = imm16 & 0x7FFF;
            val = (s << 16) | ((em + (112 << 10)) << 13);
            break;
        }
        case 2: val = imm16; break; // treat imm16 as uint16
        case 4: val = uint32_t(int32_t(int16_t(uint16_t(imm16)))); break; // treat imm16 as sint16
        case 8: val = imm16 << 16; break; // will put immediate in upper part
        case 10: val = imm16; break; // will put immediate in lower part
        default: TTSIM_ERROR(UndefinedBehavior, "instr_mod0=%d", instr_mod0);
    }

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    switch (instr_mod0) {
        case 0:
        case 1:
        case 2:
        case 4:
            for_each_lane(mask, [=](uint32_t lane) {
                p_tensix->l_regs[lreg_ind][lane] = val;
            });
            break;
        case 8:
            for_each_lane(mask, [=](uint32_t lane) {
                p_tensix->l_regs[lreg_ind][lane] = (p_tensix->l_regs[lreg_ind][lane] & 0xFFFF) | val;
            });
            break;
        case 10:
            for_each_lane(mask, [=](uint32_t lane) {
                p_tensix->l_regs[lreg_ind][lane] = (p_tensix->l_regs[lreg_ind][lane] & 0xFFFF0000) | val;
            });
            break;
        default:
            TTSIM_ERROR(UndefinedBehavior, "instr_mod0=%d", instr_mod0);
    }
    return true;
}

static uint16_t sfpu_store_to_fp16(uint32_t x) {
    uint32_t s = x >> 31;
    int32_t e = int32_t((x >> 23) & 255) - 112;
    uint32_t m = x & 0x7FFFFF;
    if (e <= 0) {
        return (s << 15);
    } else if (e > 31) {
        return (s << 15) | 0x7FFF;
    } else {
        return (s << 15) | (uint32_t(e) << 10) | (m >> 13);
    }
}

TENSIX_EXECUTE_SFPSTORE() {
    TTSIM_VERIFY(!(dest_reg_addr & 1), UnimplementedFunctionality, "dest_reg_addr=%d", dest_reg_addr);
    TTSIM_VERIFY(!instr_mod0 || (instr_mod0 == 2) || (instr_mod0 == 3) || (instr_mod0 == 4) || (instr_mod0 == 6) || (instr_mod0 == 7) ||
                 (instr_mod0 == 12) || (instr_mod0 == 14) || (instr_mod0 == 15),
        UnimplementedFunctionality, "instr_mod0=%d", instr_mod0);
    TTSIM_VERIFY(lreg_ind < 12, UnimplementedFunctionality, "lreg_ind=%d", lreg_ind); // note some of the constant LRegs can be stored

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    if (!instr_mod0) {
        if (p_config->ALU_ACC_CTRL_SFPU_Fp32_enabled) {
            instr_mod0 = 3;
        } else {
            uint32_t src_b_fmt = p_config->ALU_FORMAT_SPEC_REG1_SrcB;
            if ((src_b_fmt == 0) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7) || // fp32, bf16, bfp8, bfp4
                (src_b_fmt == 9) || (src_b_fmt == 15)) { // int16, bfp2
                instr_mod0 = 2;
            } else {
                TTSIM_VERIFY(src_b_fmt == 1, UnimplementedFunctionality, "src_b_fmt=%d", src_b_fmt); // fp16
                instr_mod0 = 1;
            }
        }
    }

    uint32_t dst_row = p_tensix->dst_rwc[pipe] + dest_reg_addr + p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
    // XXX no DEST_REGW_BASE_Base
    TTSIM_VERIFY(!(dst_row & 1) && (dst_row < DST_ROWS), UnsupportedFunctionality, "dst_row=%d", dst_row);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t row = (dst_row & ~3) + lane / 8;
        uint32_t col = 2*(lane & 7) + ((dst_row & 2) >> 1);
        uint32_t value = p_tensix->l_regs[lreg_ind][lane];
        if (instr_mod0 == 1) {
            write_dst16b(p_tensix, row, col, dst_encode_fp16(sfpu_store_to_fp16(value)));
        } else if (instr_mod0 == 2) {
            value = denormals_as_zeros(value);
            write_dst16b(p_tensix, row, col, dst_encode_bf16(value >> 16));
        } else if (instr_mod0 == 3) {
#if TT_ARCH_VERSION >= 1
            value = denormals_as_zeros(value);
#endif
            write_dst32b(p_tensix, row, col, dst_encode_fp32(value));
        } else if (instr_mod0 == 4) {
            write_dst32b(p_tensix, row, col, dst_encode_fp32(value));
        } else if ((instr_mod0 == 6) || (instr_mod0 == 14)) {
            write_dst16b(p_tensix, row, col, value & 0xFFFF);
        } else if (instr_mod0 == 7) {
            write_dst32b(p_tensix, row, col, value);
        } else if (instr_mod0 == 12) {
#if TT_ARCH_VERSION == 0
            if (value & 0x80000000) {
                value = 0x80000000 | uint32_t(-int32_t(value));
            }
#endif
            write_dst32b(p_tensix, row, col, dst_encode_fp32(value));
        } else if (instr_mod0 == 15) {
            write_dst16b(p_tensix, row, col, value >> 16);
        } else {
            TTSIM_ERROR(AssertionFailure, "instr_mod0=%d", instr_mod0);
        }
    });

    math_update_counters(p_tensix, pipe, sfpu_addr_mode, false);
    return true;
}

static inline uint32_t lut8_to_fp32(uint8_t x) {
    if (x == 255) {
        return 0;
    }
    uint32_t s = x >> 7;
    uint32_t e = (x >> 4) & 7;
    uint32_t m = x & 15;
    return (s << 31) | ((127 - e) << 23) | (m << 19);
}

TENSIX_EXECUTE_SFPLUT() {
    TTSIM_VERIFY(instr_mod0 == 4, UnsupportedFunctionality, "instr_mod0=%d", instr_mod0);
    TTSIM_VERIFY(lreg_ind < 8, UnsupportedFunctionality, "lreg_ind=%d", lreg_ind);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t l3 = p_tensix->l_regs[3][lane];
        uint32_t b = l3 & 0x7FFFFFFF; // absolute value
        uint32_t coeffs = (b < 0x3F800000) ? p_tensix->l_regs[0][lane] :
                          (b < 0x40000000) ? p_tensix->l_regs[1][lane] :
                          p_tensix->l_regs[2][lane];
        uint32_t a = lut8_to_fp32(bits<15,8>(coeffs));
        uint32_t c = lut8_to_fp32(bits<7,0>(coeffs));
        uint32_t d = fma_model(a, b, c);
        if (instr_mod0 & 4) {
            d = (d & 0x7FFFFFFF) | (l3 & 0x80000000); // copy sign bit from l3
        }
        p_tensix->l_regs[lreg_ind][lane] = d;
    });
    return true;
}

TENSIX_EXECUTE_SFPMULI() {
    TTSIM_VERIFY(!instr_mod1, UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    uint32_t imm = imm16_math << 16;

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t result = fma_model(imm, p_tensix->l_regs[lreg_dest][lane], 0);
        p_tensix->l_regs[lreg_dest][lane] = result;
    });
    return true;
}

TENSIX_EXECUTE_SFPADDI() {
    TTSIM_VERIFY(!instr_mod1, UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    uint32_t imm = imm16_math << 16;

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t result = fma_model(imm, 0x3F800000, p_tensix->l_regs[lreg_dest][lane]);
        p_tensix->l_regs[lreg_dest][lane] = result;
    });
    return true;
}

TENSIX_EXECUTE_SFPDIVP2() {
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY((imm12_math <= 0x7F) || (imm12_math >= 0xF80), NonContractualBehavior, "imm12_math=%d", imm12_math);
    imm12_math &= 0xFF;

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane];
        uint32_t e = (src >> 23) & 255;
        if (instr_mod1 & 1) {
            if (e != 255) {
                e = (e + imm12_math) & 255;
            }
        } else {
            e = imm12_math;
        }
        p_tensix->l_regs[lreg_dest][lane] = (e << 23) | (src & 0x807FFFFF);
    });
    return true;
}

TENSIX_EXECUTE_SFPEXEXP() {
    TTSIM_VERIFY(!(instr_mod1 & 4), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY((instr_mod1 <= 2) || (instr_mod1 == 10), UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    uint32_t bias = (instr_mod1 & 1) ? 0 : 127;

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane];
        uint32_t exp = (src >> 23) & 255;
        int32_t dst = exp - bias;
        p_tensix->l_regs[lreg_dest][lane] = dst;
        if (instr_mod1 == 10) {
            if (dst < 0) {
                p_tensix->cc &= ~(1 << lane);
            } else {
                p_tensix->cc |= 1 << lane;
            }
        } else if (instr_mod1 == 2) {
            if (dst < 0) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        }
    });
    return true;
}

TENSIX_EXECUTE_SFPEXMAN() {
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    uint32_t hidden_bit = instr_mod1 ? 0 : 0x800000;

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane];
        p_tensix->l_regs[lreg_dest][lane] = (src & 0x7FFFFF) | hidden_bit;
    });
    return true;
}

TENSIX_EXECUTE_SFPIADD() {
    TTSIM_VERIFY(instr_mod1 <= 10, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY((instr_mod1 & 3) <= 2, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane];
        if (instr_mod1 & 1) {
            src += int32_t(uint32_t(imm12_math << 20)) >> 20; // sign extend imm12
        } else if (instr_mod1 & 2) {
            src -= p_tensix->l_regs[lreg_dest][lane];
        } else {
            src += p_tensix->l_regs[lreg_dest][lane];
        }
        if (instr_mod1 & 8) {
            if (src & 0x80000000) {
                p_tensix->cc &= ~(1 << lane);
            } else {
                p_tensix->cc |= 1 << lane;
            }
        } else if (!(instr_mod1 & 4)) {
            if (src & 0x80000000) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        }
        p_tensix->l_regs[lreg_dest][lane] = src;
    });
    return true;
}

TENSIX_EXECUTE_SFPSHFT() {
#if TT_ARCH_VERSION >= 1
    TTSIM_VERIFY((instr_mod1 <= 3) || (instr_mod1 == 5) || (instr_mod1 == 7), NonContractualBehavior, "invalid instr_mod1=%d", instr_mod1);
#else
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "instr_mod1=%d", instr_mod1);
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    if (!(instr_mod1 & 1)) {
        TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
    }
    int32_t imm = signed_bits<11,0>(imm12_math);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
#if TT_ARCH_VERSION >= 1
        uint32_t src = (instr_mod1 & 4) ? p_tensix->l_regs[lreg_c][lane] : p_tensix->l_regs[lreg_dest][lane];
#else
        uint32_t src = p_tensix->l_regs[lreg_dest][lane];
#endif
        int32_t shift_amount = (instr_mod1 & 1) ? imm : int32_t(p_tensix->l_regs[lreg_c][lane]);
        if (shift_amount >= 0) {
            src <<= shift_amount & 31;
#if TT_ARCH_VERSION == 1
        } else if (instr_mod1 & 2) {
            src = int32_t(src) >> ((-shift_amount) & 31);
#endif
        } else {
            src >>= (-shift_amount) & 31;
        }
        p_tensix->l_regs[lreg_dest][lane] = src;
    });
    return true;
}

TENSIX_EXECUTE_SFPSETCC() {
    TTSIM_VERIFY(!(instr_mod1 & 1) && (instr_mod1 <= 6), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);

    TTSIM_VERIFY(p_tensix->cc_en, UnsupportedFunctionality, "cc_en=%d", p_tensix->cc_en); // very strange behavior that clears all CC bits
    uint32_t mask = p_tensix->cc;
    for_each_lane(mask, [=](uint32_t lane) {
        int32_t src = p_tensix->l_regs[lreg_c][lane];
        if (instr_mod1 == 0) {
            if (src < 0) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        } else if (instr_mod1 == 2) {
            if (src != 0) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        } else if (instr_mod1 == 4) {
            if (src >= 0) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        } else { // instr_mod1 == 6
            if (src == 0) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        }
    });
    return true;
}

TENSIX_EXECUTE_SFPMOV() {
    TTSIM_VERIFY(!(instr_mod1 & 4), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY((instr_mod1 <= 2) || (instr_mod1 == 8), UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    if (instr_mod1 == 2) { // all lanes enabled
        mask = 0xFFFFFFFF;
    } else if (instr_mod1 == 8) {
        TTSIM_VERIFY(lreg_c == 9, UnimplementedFunctionality, "instr_mod1=%d lreg_c=%d", instr_mod1, lreg_c);
    }
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src;
        if (instr_mod1 == 8) {
            src = p_tensix->prng_state[lane];
            p_tensix->prng_state[lane] = step_prng(p_tensix->prng_state[lane]);
        } else {
            src = p_tensix->l_regs[lreg_c][lane];
            if (instr_mod1 & 1) { // negate
                src ^= 0x80000000;
            }
        }
        p_tensix->l_regs[lreg_dest][lane] = src;
    });
    return true;
}

TENSIX_EXECUTE_SFPABS() {
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane];
        if (instr_mod1 & 1) {
            if (src <= 0xFF800000) { // do not apply absolute value to negative NaNs
                src &= 0x7FFFFFFF;
            }
        } else {
            if (src >= 0x80000000) {
                src = -src;
            }
        }
        p_tensix->l_regs[lreg_dest][lane] = src;
    });
    return true;
}

template<typename Func>
static bool tensix_execute_sfpu_int32(TensixState *p_tensix, uint32_t pipe, uint32_t lreg_dest, uint32_t lreg_b, uint32_t lreg_c, Func op) {
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src_b = p_tensix->l_regs[lreg_b][lane];
        uint32_t src_c = p_tensix->l_regs[lreg_c][lane];
        p_tensix->l_regs[lreg_dest][lane] = op(src_b, src_c);
    });
    return true;
}

TENSIX_EXECUTE_SFPAND() {
    uint32_t lreg_b = lreg_dest;
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
#elif TT_ARCH_VERSION == 1
    if (instr_mod1 == 1) {
        TTSIM_VERIFY(imm12_math <= 15, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
        lreg_b = imm12_math;
    } else {
        TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
        TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
    }
#endif
    return tensix_execute_sfpu_int32(p_tensix, pipe, lreg_dest, lreg_b, lreg_c, [](uint32_t src_b, uint32_t src_c) { return src_b & src_c; });
}

TENSIX_EXECUTE_SFPOR() {
    uint32_t lreg_b = lreg_dest;
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
#elif TT_ARCH_VERSION == 1
    if (instr_mod1 == 1) {
        TTSIM_VERIFY(imm12_math <= 15, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
        lreg_b = imm12_math;
    } else {
        TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
        TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
    }
#endif
    return tensix_execute_sfpu_int32(p_tensix, pipe, lreg_dest, lreg_b, lreg_c, [](uint32_t src_b, uint32_t src_c) { return src_b | src_c; });
}

TENSIX_EXECUTE_SFPNOT() {
    return tensix_execute_sfpu_int32(p_tensix, pipe, lreg_dest, lreg_dest, lreg_c, [](uint32_t src_b, uint32_t src_c) { return ~src_c; });
}

TENSIX_EXECUTE_SFPLZ() {
    TTSIM_VERIFY(!(instr_mod1 & 1), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 <= 4, UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane];
        if (instr_mod1 & 4) {
            src &= 0x7FFFFFFF;
        }
        p_tensix->l_regs[lreg_dest][lane] = src ? __builtin_clz(src) : 32;
        if (instr_mod1 & 2) {
            if (src) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        }
    });
    return true;
}

TENSIX_EXECUTE_SFPSETEXP() {
    TTSIM_VERIFY(instr_mod1 <= 2, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(imm12_math <= 255, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
    if (instr_mod1 != 1) {
        TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "instr_mod1=%d imm12_math=%d", instr_mod1, imm12_math);
    }

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane] & 0x807FFFFF; // always discard exponent of source
        uint32_t exp;
        if (instr_mod1 == 1) {
            exp = imm12_math;
        } else if (instr_mod1 == 2) {
            exp = (p_tensix->l_regs[lreg_dest][lane] >> 23) & 0xFF;
        } else {
            exp = p_tensix->l_regs[lreg_dest][lane] & 0xFF;
        }
        p_tensix->l_regs[lreg_dest][lane] = src | (exp << 23);
    });
    return true;
}

TENSIX_EXECUTE_SFPSETMAN() {
    TTSIM_VERIFY(instr_mod1 <= 1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane] & 0xFF800000;
        if (instr_mod1 & 1) {
            src |= imm12_math << 11;
        } else {
            src |= p_tensix->l_regs[lreg_dest][lane] & 0x7FFFFF;
        }
        p_tensix->l_regs[lreg_dest][lane] = src;
    });
    return true;
}

TENSIX_EXECUTE_SFPMAD() {
#if TT_ARCH_VERSION >= 1
    TTSIM_VERIFY(instr_mod1 <= 3, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#else
    TTSIM_VERIFY(!(instr_mod1 & 3), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    uint32_t lreg_a = lreg_src_a;
    uint32_t lreg_b = lreg_src_b;
    uint32_t lreg_c = lreg_src_c;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t a = p_tensix->l_regs[lreg_a][lane];
        uint32_t b = p_tensix->l_regs[lreg_b][lane];
        uint32_t c = p_tensix->l_regs[lreg_c][lane];
#if TT_ARCH_VERSION >= 1
        if (instr_mod1 & 1) {
            a ^= 0x80000000;
        }
        if (instr_mod1 & 2) {
            c ^= 0x80000000;
        }
#endif
        p_tensix->l_regs[lreg_dest][lane] = fma_model(a, b, c);
    });
    return true;
}

TENSIX_EXECUTE_SFPADD() {
    uint32_t lreg_a = lreg_src_a;
    uint32_t lreg_b = lreg_src_b;
    uint32_t lreg_c = lreg_src_c;
#if TT_ARCH_VERSION >= 1
    TTSIM_VERIFY(instr_mod1 <= 3, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#else
    TTSIM_VERIFY(!(instr_mod1 & 3), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    if (lreg_a != 10) { // require that one mul term of the MAD is of the constant +1.0
        TTSIM_VERIFY(lreg_b == 10, NonContractualBehavior, "invalid lreg_a=%d lreg_b=%d", lreg_a, lreg_b);
        std::swap(lreg_a, lreg_b); // make lreg_a consistently LReg[10] by swapping if needed
    }

#if TT_ARCH_VERSION >= 1
    uint32_t a = (instr_mod1 & 1) ? 0xBF800000 : 0x3F800000;
#else
    uint32_t a = 0x3F800000;
#endif
    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t b = p_tensix->l_regs[lreg_b][lane];
        uint32_t c = p_tensix->l_regs[lreg_c][lane];
#if TT_ARCH_VERSION >= 1
        if (instr_mod1 & 2) {
            c ^= 0x80000000;
        }
#endif
        p_tensix->l_regs[lreg_dest][lane] = fma_model(a, b, c);
    });
    return true;
}

TENSIX_EXECUTE_SFPMUL() {
    uint32_t lreg_a = lreg_src_a;
    uint32_t lreg_b = lreg_src_b;
    uint32_t lreg_c = lreg_src_c;
#if TT_ARCH_VERSION >= 1
    TTSIM_VERIFY(instr_mod1 <= 1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#else
    TTSIM_VERIFY(!(instr_mod1 & 3), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(lreg_c == 9, NonContractualBehavior, "lreg_c=%d", lreg_c); // require that the add term of the MAD is of the constant +0.0

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t a = p_tensix->l_regs[lreg_a][lane];
        uint32_t b = p_tensix->l_regs[lreg_b][lane];
#if TT_ARCH_VERSION >= 1
        if (instr_mod1 & 1) {
            a ^= 0x80000000;
        }
#endif
        p_tensix->l_regs[lreg_dest][lane] = fma_model(a, b, 0);
    });
    return true;
}

TENSIX_EXECUTE_SFPPUSHC() {
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    TTSIM_VERIFY(p_tensix->cc_sp < CC_STACK_SIZE, UndefinedBehavior, "CC stack overflow");
    p_tensix->cc_en_stack[p_tensix->cc_sp] = p_tensix->cc_en;
    p_tensix->cc_stack[p_tensix->cc_sp] = p_tensix->cc;
    p_tensix->cc_sp++;
    return true;
}

TENSIX_EXECUTE_SFPPOPC() {
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    TTSIM_VERIFY(p_tensix->cc_sp, UndefinedBehavior, "CC stack underflow");
    p_tensix->cc_sp--;
    p_tensix->cc_en = p_tensix->cc_en_stack[p_tensix->cc_sp];
    p_tensix->cc = p_tensix->cc_stack[p_tensix->cc_sp];
    return true;
}

TENSIX_EXECUTE_SFPSETSGN() {
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(imm12_math <= 1, AssertionFailure, "imm12_math=%d", imm12_math);
    if (!(instr_mod1 & 1)) {
        TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "instr_mod1=%d imm12_math=%d", instr_mod1, imm12_math);
    }

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane] & 0x7FFFFFFF;
        if (instr_mod1 & 1) {
            src |= imm12_math << 31;
        } else {
            src |= p_tensix->l_regs[lreg_dest][lane] & 0x80000000;
        }
        p_tensix->l_regs[lreg_dest][lane] = src;
    });
    return true;
}

TENSIX_EXECUTE_SFPENCC() {
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    switch (instr_mod1) {
        case 0:
            TTSIM_VERIFY(!imm12_math, UnimplementedFunctionality, "instr_mod1=%d imm12_math=%d", instr_mod1, imm12_math);
            p_tensix->cc = 0xFFFFFFFF;
            break;
        case 10:
            p_tensix->cc = ((imm12_math >> 1) & 1) ? 0xFFFFFFFF : 0;
            p_tensix->cc_en = imm12_math & 1;
            break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
    }
    return true;
}

TENSIX_EXECUTE_SFPCOMPC() {
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    TTSIM_VERIFY(p_tensix->cc_en, UnimplementedFunctionality, "cc_en=%d", p_tensix->cc_en);
    if (p_tensix->cc_sp) {
        uint32_t top = p_tensix->cc_sp - 1;
        TTSIM_VERIFY(p_tensix->cc_en_stack[top], UnimplementedFunctionality, "cc_en_stack[top]=%d", p_tensix->cc_en_stack[top]);
        p_tensix->cc = p_tensix->cc_stack[top] & ~p_tensix->cc;
    } else {
        p_tensix->cc = ~p_tensix->cc;
    }
    return true;
}

TENSIX_EXECUTE_SFPTRANSP() {
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for (uint32_t r_base = 0; r_base < 8; r_base += 4) {
        for (uint32_t col = 0; col < 8; col++) {
            for (uint32_t i = 0; i < 4; i++) {
                for (uint32_t j = 0; j < i; j++) {
                    uint32_t ij = p_tensix->l_regs[r_base + i][j*8 + col];
                    uint32_t ji = p_tensix->l_regs[r_base + j][i*8 + col];
                    if (mask & (1 << (j*8 + col))) {
                        p_tensix->l_regs[r_base + i][j*8 + col] = ji;
                    }
                    if (mask & (1 << (i*8 + col))) {
                        p_tensix->l_regs[r_base + j][i*8 + col] = ij;
                    }
                }
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_SFPXOR() {
    return tensix_execute_sfpu_int32(p_tensix, pipe, lreg_dest, lreg_dest, lreg_c, [](uint32_t src_b, uint32_t src_c) { return src_b ^ src_c; });
}

TENSIX_EXECUTE_SFP_STOCH_RND() {
    TTSIM_VERIFY((instr_mod1 <= 7) || (instr_mod1 == 12) || (instr_mod1 == 13), UndefinedBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY((instr_mod1 == 1) || (instr_mod1 == 2) || (instr_mod1 == 3) || (instr_mod1 == 6) || (instr_mod1 == 7),
        UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    // XXX check lreg_src_b?
    TTSIM_VERIFY(!imm8_math, UnsupportedFunctionality, "imm8_math=%d", imm8_math);
    TTSIM_VERIFY(!rnd_mode, UnsupportedFunctionality, "rnd_mode=%d", rnd_mode);

    // XXX docs seem to imply that this always advances the PRNG even when we aren't using the stochastic rounding mode
    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_src_c][lane];
        if (instr_mod1 == 1) { // FP32_TO_FP16B
            uint32_t exp = (src >> 23) & 255;
            if (!exp) {
                src = 0; // denormal/zero flushed to +0
            } else if (exp == 255) {
                src &= 0xFF800000; // inf/nan becomes inf of same sign
            } else { // XXX this path can be simplified
                uint32_t discarded_bits = src & 0xFFFF;
                src -= discarded_bits;
                if ((discarded_bits << 7) >= 0x400000) { // XXX this disagrees with the pseudocode
                    src += 0x10000;
                }
            }
            p_tensix->l_regs[lreg_dest][lane] = src;
        } else { // FP32_TO_[U][INT8,INT16]
            bool keep_sign = instr_mod1 & 1;
            uint32_t max_magnitude = (instr_mod1 == 6) ? 65535 : (instr_mod1 == 7) ? 32767 : (instr_mod1 == 2) ? 255 : 127;
            uint32_t sign = keep_sign ? (src & 0x80000000) : 0;
            int32_t exp = ((src >> 23) & 255) - 127;
            if (exp < -1) {
                src = 0;
            } else if (exp >= 16) {
                src = sign | max_magnitude;
            } else {
                uint64_t mag = 0x800000 | (src & 0x7FFFFF);
                mag = (exp >= 0) ? mag << exp : mag >> -exp;
                mag = (mag >> 23) + ((mag & 0x7FFFFF) >= 0x400000);
                if (mag > max_magnitude) {
                    mag = max_magnitude;
                }
                if (mag == 0) {
                    sign = 0;
                }
                src = sign + mag;
            }
            p_tensix->l_regs[lreg_dest][lane] = src;
        }
    });
    return true;
}

TENSIX_EXECUTE_SFPNOP() { return true; }

TENSIX_EXECUTE_SFPCAST() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(instr_mod1 <= 3, NonContractualBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 != 1, UnsupportedFunctionality, "stochastic rounding is explicitly out of scope");
    TTSIM_VERIFY(instr_mod1 != 2, UnsupportedFunctionality, "instr_mod1=%d should be replaced by SFPABS", instr_mod1);
#else
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "stochastic rounding is explicitly out of scope");
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_src_c][lane];
        uint32_t sign = src & 0x80000000;
        uint32_t dst;
#if TT_ARCH_VERSION == 1
        if (instr_mod1 == 3) {
            dst = sign | (sign ? -src : src);
        } else
#endif
        {
            dst = sign; // preserve sign bit, including for -0
            src &= 0x7FFFFFFF; // extract magnitude
            if (src) { // if magnitude is nonzero
                uint32_t lz = __builtin_clz(src);
                src <<= lz;
                dst |= ((157 - lz) << 23) + (src >> 8);
                if ((src & 0x80) && (src & 0x17F)) { // RTNE
                    dst++;
                }
            }
        }
        p_tensix->l_regs[lreg_dest][lane] = dst;
    });
    return true;
}

TENSIX_EXECUTE_SFPCONFIG() {
    TTSIM_VERIFY(instr_mod1 <= 1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    if (!instr_mod1) { // require all lanes of LReg[0] to be identical to one another
        for (uint32_t lane = 1; lane < 32; lane++) {
            TTSIM_VERIFY(p_tensix->l_regs[0][0] == p_tensix->l_regs[0][lane],
                UnsupportedFunctionality, "instr_mod1=%d: l_regs[0]: lane[%d] mismatch with lane[0]", instr_mod1, lane);
        }
        TTSIM_VERIFY(!imm16_math, UnsupportedFunctionality, "instr_mod1=%d imm16_math=0x%x", instr_mod1, imm16_math);
    }

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    TTSIM_VERIFY(mask == 0xFFFFFFFF, UnsupportedFunctionality, "not all lanes enabled: mask=0x%x", mask);
    switch (config_dest) {
        case 0 ... 3:
            TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "config_dest=%d instr_mod1=%d", config_dest, instr_mod1);
            p_tensix->load_macro_instruction_template[config_dest] = p_tensix->l_regs[0][0];
            break;
        case 4 ... 7:
            p_tensix->load_macro_sequence[config_dest - 4] = (instr_mod1 & 1) ? imm16_math : p_tensix->l_regs[0][0];
            break;
        case 8:
            TTSIM_VERIFY(instr_mod1 == 1, UnsupportedFunctionality, "config_dest=%d instr_mod1=%d", config_dest, instr_mod1);
            TTSIM_VERIFY(imm16_math <= 0xFFF, UnsupportedFunctionality, "config_dest=%d imm16_math=0x%x", config_dest, imm16_math);
            p_tensix->load_macro_misc = imm16_math;
            break;
        case 9: case 10:
            TTSIM_ERROR(NonContractualBehavior, "config_dest=%d", config_dest);
        case 11 ... 14:
            TTSIM_VERIFY(!imm16_math, UnsupportedFunctionality, "config_dest=%d imm16_math=0x%x", config_dest, imm16_math);
            if (instr_mod1 & 1) { // write "default" immediate to LReg
                TTSIM_VERIFY(config_dest == 11, UnsupportedFunctionality, "config_dest=%d instr_mod1=%d", config_dest, instr_mod1);
                for (uint32_t lane = 0; lane < 32; lane++) {
                    p_tensix->l_regs[config_dest][lane] = 0xBF800000;
                }
            } else {
                for (uint32_t lane = 0; lane < 32; lane++) {
                    p_tensix->l_regs[config_dest][lane] = p_tensix->l_regs[0][0];
                }
            }
            break;
        case 15: { // LaneConfig
            uint32_t value = (instr_mod1 & 1) ? imm16_math : p_tensix->l_regs[0][0];
            TTSIM_VERIFY(!(value & ~0x104), UnsupportedFunctionality, "lane_config=0x%x", value);
            p_tensix->lane_config = value;
            break;
        }
        default:
            TTSIM_ERROR(AssertionFailure, "config_dest=%d", config_dest);
    }
    return true;
}

TENSIX_EXECUTE_SFPSWAP() {
    uint32_t lreg_c = lreg_src_c;
    TTSIM_VERIFY(lreg_dest < 12, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    uint32_t vd_gets_min = 0xFFFFFFFF;
    switch (instr_mod1) {
        case 0: case 1: break;
        case 2: vd_gets_min = 0x0000FFFF; break;
        case 3: vd_gets_min = 0x00FF00FF; break;
        case 9: vd_gets_min = 0x00000000; break;
        case 10 ... 15: TTSIM_ERROR(NonContractualBehavior, "instr_mod1=%d", instr_mod1);
        default: TTSIM_ERROR(UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
    }

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    uint32_t lane_config = p_tensix->lane_config;
    if (lane_config & 4) {
        TTSIM_VERIFY(lreg_c < 4, UnsupportedFunctionality, "lreg_c=%d", lreg_c);
        TTSIM_VERIFY(lreg_dest < 4, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    }
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t c = p_tensix->l_regs[lreg_c][lane];
        uint32_t d = p_tensix->l_regs[lreg_dest][lane];
        bool should_swap = true;
        if (instr_mod1) {
            int32_t c_unpacked = sign_mag32_total_order(c);
            int32_t d_unpacked = sign_mag32_total_order(d);
            if (vd_gets_min & (1 << lane)) {
                should_swap = c_unpacked < d_unpacked;
            } else {
                should_swap = c_unpacked >= d_unpacked;
            }
            if (lane_config & 0x100) {
                should_swap = !should_swap;
            }
        }
        if (should_swap) {
            if (lane_config & 4) {
                p_tensix->l_regs[lreg_c][lane] = d;
                p_tensix->l_regs[lreg_dest][lane] = c;
                uint32_t vc_a = 4 + lreg_c;
                uint32_t vd_a = 4 + lreg_dest;
                std::swap(p_tensix->l_regs[vc_a][lane], p_tensix->l_regs[vd_a][lane]);
            } else {
                if (lreg_c < 8) {
                    p_tensix->l_regs[lreg_c][lane] = d;
                }
                if (lreg_dest < 8) {
                    p_tensix->l_regs[lreg_dest][lane] = c;
                }
            }
        }
    });
    return true;
}

TENSIX_EXECUTE_SFPLOADMACRO() {
    TTSIM_ERROR(UnsupportedFunctionality, "explicitly out of scope: set TT_METAL_DISABLE_SFPLOADMACRO=1 to disable usage");
}

TENSIX_EXECUTE_SFPSHFT2() {
    TTSIM_VERIFY(instr_mod1 <= 6, UndefinedBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY((instr_mod1 == 3) || (instr_mod1 == 4) || (instr_mod1 == 5), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#if TT_ARCH_VERSION == 0
    // XXX Would be good to add a checker for the WH-only scheduling restrictions on some modes
    TTSIM_VERIFY(instr_mod1 != 4, UnsupportedFunctionality, "SUBVEC_SHFLSHR1 should not be used on Wormhole due to a hardware bug");
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    if (instr_mod1 == 5) { // imm12_math is used as a source register for this mode
        TTSIM_VERIFY(imm12_math < 16, UnsupportedFunctionality, "instr_mod1=%d imm12_math=%d", instr_mod1, imm12_math);
    } else {
        TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "instr_mod1=%d imm12_math=%d", instr_mod1, imm12_math);
    }

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    switch (instr_mod1) {
        case 3: {
            uint32_t vc[32];
            memcpy(vc, p_tensix->l_regs[lreg_src_c], sizeof(vc));
            for_each_lane(mask, [=](uint32_t lane) {
                p_tensix->l_regs[lreg_dest][lane] = (lane & 7) ? vc[lane - 1] : vc[lane + 7];
            });
            break;
        }
#if TT_ARCH_VERSION == 1
        case 4: {
            uint32_t vc[32];
            memcpy(vc, p_tensix->l_regs[lreg_src_c], sizeof(vc));
            for_each_lane(mask, [=](uint32_t lane) {
                p_tensix->l_regs[lreg_dest][lane] = (lane & 7) ? vc[lane - 1] : 0;
            });
            break;
        }
#endif
        case 5:
            for_each_lane(mask, [=](uint32_t lane) {
                int32_t src_c = p_tensix->l_regs[lreg_src_c][lane];
                uint32_t src_b = p_tensix->l_regs[imm12_math][lane];
                if (src_c >= 0) {
                    src_b <<= src_c & 31;
                } else {
                    src_b >>= (-src_c) & 31;
                }
                p_tensix->l_regs[lreg_dest][lane] = src_b;
            });
            break;
        default:
            TTSIM_ERROR(AssertionFailure, "instr_mod1=%d", instr_mod1);
    }
    return true;
}

static inline uint32_t lut16_to_fp32(uint16_t x) {
    uint32_t s = x >> 15;
    uint32_t e = (x >> 10) & 31;
    uint32_t m = x & 0x3FF;
    return (s << 31) | (((e == 31) ? 0 : (112 + e)) << 23) | (m << 13);
}

TENSIX_EXECUTE_SFPLUTFP32() {
    TTSIM_VERIFY(instr_mod1 == 2, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t l3 = p_tensix->l_regs[3][lane];
        uint32_t b = l3 & 0x7FFFFFFF; // absolute value
        uint32_t i = (b < 0x3F800000) ? 0 : (b < 0x40000000) ? 1 : 2;
        uint32_t j = (b < 0x3F000000) ?  0 :
                     (b < 0x3F800000) ? 16 :
                     (b < 0x3FC00000) ?  0 :
                     (b < 0x40000000) ? 16 :
                     (b < 0x40400000) ?  0 :
                     16;
        uint32_t a = lut16_to_fp32((p_tensix->l_regs[0 + i][lane] >> j) & 0xFFFF);
        uint32_t c = lut16_to_fp32((p_tensix->l_regs[4 + i][lane] >> j) & 0xFFFF);
        p_tensix->l_regs[lreg_dest][lane] = fma_model(a, b, c);
    });
    return true;
}

#if TT_ARCH_VERSION >= 1
TENSIX_EXECUTE_SFPLE() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY((instr_mod1 == 1) || (instr_mod1 == 8), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t c = p_tensix->l_regs[lreg_c][lane];
        uint32_t d = p_tensix->l_regs[lreg_dest][lane];
        bool is_le = sign_mag32_total_order(d) <= sign_mag32_total_order(c);
        if (instr_mod1 == 1) {
            if (is_le) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        } else {
            p_tensix->l_regs[lreg_dest][lane] = is_le ? 0xFFFFFFFF : 0;
        }
    });
    return true;
#else
    TTSIM_ERROR_NOFMT(MissingSpecification);
#endif
}

TENSIX_EXECUTE_SFPGT() {
#if TT_ARCH_VERSION >= 1
    // instr_mod1 bit0 -> update CC result with (d > c); bit3 -> write lreg_dest all-1s/0s.
    TTSIM_VERIFY((instr_mod1 == 1) || (instr_mod1 == 8), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t c = p_tensix->l_regs[lreg_c][lane];
        uint32_t d = p_tensix->l_regs[lreg_dest][lane];
        bool is_gt = sign_mag32_total_order(d) > sign_mag32_total_order(c);
        if (instr_mod1 == 1) {
            if (is_gt) {
                p_tensix->cc |= 1 << lane;
            } else {
                p_tensix->cc &= ~(1 << lane);
            }
        } else {
            p_tensix->l_regs[lreg_dest][lane] = is_gt ? 0xFFFFFFFF : 0;
        }
    });
    return true;
#else
    TTSIM_ERROR_NOFMT(MissingSpecification);
#endif
}

TENSIX_EXECUTE_SFPMUL24() {
#if TT_ARCH_VERSION >= 1
    TTSIM_VERIFY(!(instr_mod1 & 2), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 <= 1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(lreg_src_c == 9, NonContractualBehavior, "lreg_src_c=%d", lreg_src_c);
    uint32_t lreg_a = lreg_src_a;
    uint32_t lreg_b = lreg_src_b;

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t a = p_tensix->l_regs[lreg_a][lane];
        uint32_t b = p_tensix->l_regs[lreg_b][lane];
        uint32_t d;
        if (instr_mod1 & 1) {
            d = (uint64_t(a & 0x7FFFFF) * uint64_t(b & 0x7FFFFF)) >> 23;
        } else {
            d = (a * b) & 0x7FFFFF;
        }
        p_tensix->l_regs[lreg_dest][lane] = d;
    });
    return true;
#else
    TTSIM_ERROR_NOFMT(MissingSpecification);
#endif
}

#if TT_ARCH_VERSION == 1
static uint32_t approx_recip(uint32_t x) {
    static const uint8_t lut[] = {
        127, 125, 123, 121, 119, 117, 116, 114, 112, 110, 109, 107, 105, 104, 102, 100, 99,
        97, 96, 94, 93, 91, 90, 88, 87, 85, 84, 83, 81, 80, 79, 77, 76, 75, 74, 72, 71, 70,
        69, 68, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48,
        47, 46, 45, 44, 43, 42, 41, 40, 40, 39, 38, 37, 36, 35, 35, 34, 33, 32, 31, 31, 30,
        29, 28, 28, 27, 26, 25, 25, 24, 23, 23, 22, 21, 21, 20, 19, 19, 18, 17, 17, 16, 15,
        15, 14, 14, 13, 12, 12, 11, 11, 10, 9, 9, 8, 8, 7, 7, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1,
        1, 0,
    };
    static_assert(std::size(lut) == 128);
    if (x < 0x800000) { // zero/denormal
        return 0x7F800000; // inf
    } else if (x < 0x7E800000) { // x < 2**126
        return ((253 - (x >> 23)) << 23) | (lut[(x >> 16) & 0x7F] << 16);
    } else {
        return 0;
    }
}
#endif

TENSIX_EXECUTE_SFPARECIP() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t x = p_tensix->l_regs[lreg_c][lane];
        x = (x & 0x80000000) | approx_recip(x & 0x7FFFFFFF);
        p_tensix->l_regs[lreg_dest][lane] = x;
    });
    return true;
#else
    TTSIM_ERROR_NOFMT(MissingSpecification);
#endif
}
#endif

TENSIX_EXECUTE_ATGETM() {
    TTSIM_VERIFY(mutex_index < std::size(p_tensix->mutex), UndefinedBehavior, "mutex_index=%d", mutex_index);
    TTSIM_VERIFY(mutex_index != 1, UndefinedBehavior, "mutex_index=%d", mutex_index); // mutex 1 is not valid, per the docs
    TTSIM_VERIFY(!mutex_index, UntestedFunctionality, "mutex_index=%d", mutex_index);

    TTSIM_VERIFY(p_tensix->mutex[mutex_index] != pipe + 1, NonContractualBehavior, "mutex %d already acquired by current pipe", mutex_index);
    if (p_tensix->mutex[mutex_index]) {
        return false;
    }
    p_tensix->mutex[mutex_index] = pipe + 1;
    return true;
}

TENSIX_EXECUTE_ATRELM() {
    TTSIM_VERIFY(mutex_index < std::size(p_tensix->mutex), UndefinedBehavior, "mutex_index=%d", mutex_index);
    TTSIM_VERIFY(mutex_index != 1, UndefinedBehavior, "mutex_index=%d", mutex_index); // mutex 1 is not valid, per the docs
    TTSIM_VERIFY(!mutex_index, UntestedFunctionality, "mutex_index=%d", mutex_index);

    TTSIM_VERIFY(p_tensix->mutex[mutex_index] == pipe + 1, NonContractualBehavior, "mutex %d not acquired by this pipe", mutex_index);
    p_tensix->mutex[mutex_index] = 0;
    return true;
}

// XXX we ignore stall_res and currently block all downstream instructions from issuing, regardless of type
TENSIX_EXECUTE_STALLWAIT() {
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(wait_res, UnsupportedFunctionality, "wait_res=0x%x", wait_res);
    TTSIM_VERIFY(stall_res, UnsupportedFunctionality, "stall_res=0x%x", stall_res);
    wait_res &= ~0x60FF; // never need to wait for ThCon memory requests, Unpack0/1, Pack0/1/2/3, FPU, RISCV MMIO, or SFPU
    if (wait_res & 0x100) {
        if (p_tensix->src_a_valid & (1 << p_tensix->src_a_unpack_bank)) {
            return false; // stall until SrcA not valid
        }
        wait_res &= ~0x100;
    }
    if (wait_res & 0x200) {
        if (p_tensix->src_b_valid & (1 << p_tensix->src_b_unpack_bank)) {
            return false; // stall until SrcB not valid
        }
        wait_res &= ~0x200;
    }
    if (wait_res & 0x400) {
        if (!(p_tensix->src_a_valid & (1 << p_tensix->src_a_matrix_bank))) {
            return false; // stall until SrcA valid
        }
        wait_res &= ~0x400;
    }
    if (wait_res & 0x800) {
        if (!(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank))) {
            return false; // stall until SrcB valid
        }
        wait_res &= ~0x800;
    }
    TTSIM_VERIFY(!wait_res, UnimplementedFunctionality, "wait_res=0x%x", wait_res);
#elif TT_ARCH_VERSION == 1
    TTSIM_VERIFY(wait_res, UnsupportedFunctionality, "wait_res=0x%x", wait_res);
    TTSIM_VERIFY(stall_res, UnsupportedFunctionality, "stall_res=0x%x", stall_res);
    wait_res &= ~0xC1F; // never need to wait for ThCon memory requests, Unpack0/1, Pack, FPU, RISCV MMIO, or SFPU
    if (wait_res & 0x80) {
        if (!(p_tensix->src_a_valid & (1 << p_tensix->src_a_matrix_bank))) {
            return false; // stall until SrcA valid
        }
        wait_res &= ~0x80;
    }
    if (wait_res & 0x100) {
        if (!(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank))) {
            return false; // stall until SrcB valid
        }
        wait_res &= ~0x100;
    }
    TTSIM_VERIFY(!wait_res, UnimplementedFunctionality, "wait_res=0x%x", wait_res);
#else
#error unknown TT_ARCH_VERSION
#endif
    return true;
}

TENSIX_EXECUTE_SEMINIT() {
    TTSIM_VERIFY(__builtin_popcount(sem_sel) == 1, UnsupportedFunctionality, "sem_sel=0x%x is not one-hot", sem_sel);
    uint32_t sem_index = __builtin_ctz(sem_sel);
    TTSIM_VERIFY(sem_index < std::size(p_tensix->sem), AssertionFailure, "sem_index=%d out of range", sem_index);
    TTSIM_VERIFY(init_value <= max_value, NonContractualBehavior, "init_value=%d is greater than max_value=%d", init_value, max_value);

    p_tensix->sem[sem_index] = init_value;
    p_tensix->sem_max[sem_index] = max_value;
    return true;
}

TENSIX_EXECUTE_SEMPOST() {
    TTSIM_VERIFY(__builtin_popcount(sem_sel) == 1, UnsupportedFunctionality, "sem_sel=0x%x is not one-hot", sem_sel);
    uint32_t sem_index = __builtin_ctz(sem_sel);
    TTSIM_VERIFY(sem_index < std::size(p_tensix->sem), AssertionFailure, "sem_index=%d out of range", sem_index);

    TTSIM_VERIFY(p_tensix->sem[sem_index] < p_tensix->sem_max[sem_index], NonContractualBehavior,
        "sem=%d sem_max=%d", p_tensix->sem[sem_index], p_tensix->sem_max[sem_index]);
    p_tensix->sem[sem_index]++;
    return true;
}

TENSIX_EXECUTE_SEMGET() {
    TTSIM_VERIFY(__builtin_popcount(sem_sel) == 1, UnsupportedFunctionality, "sem_sel=0x%x is not one-hot", sem_sel);
    uint32_t sem_index = __builtin_ctz(sem_sel);
    TTSIM_VERIFY(sem_index < std::size(p_tensix->sem), AssertionFailure, "sem_index=%d out of range", sem_index);

    TTSIM_VERIFY(p_tensix->sem[sem_index], NonContractualBehavior, "sem=%d sem_max=%d", p_tensix->sem[sem_index], p_tensix->sem_max[sem_index]);
    p_tensix->sem[sem_index]--;
    return true;
}

TENSIX_EXECUTE_SEMWAIT() {
    TTSIM_VERIFY(__builtin_popcount(sem_sel) == 1, UnsupportedFunctionality, "sem_sel=0x%x is not one-hot", sem_sel);
    uint32_t sem_index = __builtin_ctz(sem_sel);
    TTSIM_VERIFY(sem_index < std::size(p_tensix->sem), AssertionFailure, "sem_index=%d out of range", sem_index);
    TTSIM_VERIFY(wait_sem_cond, UndefinedBehavior, "wait_sem_cond=%d", wait_sem_cond);
    TTSIM_VERIFY((wait_sem_cond == 1) || (wait_sem_cond == 2), UnsupportedFunctionality, "wait_sem_cond=%d", wait_sem_cond);
    // XXX stall_res ignored for now

    if (wait_sem_cond == 2) { // wait while max
        return p_tensix->sem[sem_index] < p_tensix->sem_max[sem_index];
    } else { // wait while zero
        return p_tensix->sem[sem_index] > 0;
    }
}

TENSIX_EXECUTE_WRCFG() {
    TTSIM_VERIFY(cfg_reg < 4*TENSIX_CFG_STATE_SIZE, UndefinedBehavior, "cfg_reg=%d", cfg_reg);
    TTSIM_VERIFY(gpr_address < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "gpr_address=%d out of range", gpr_address);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    if (wr128b) {
        TTSIM_VERIFY(!(cfg_reg & 3), NonContractualBehavior, "misaligned cfg_reg=%d", cfg_reg);
        TTSIM_VERIFY(!(gpr_address & 3), NonContractualBehavior, "misaligned gpr_address=%d", gpr_address);
        for (uint32_t i = 0; i < 4; i++) {
            tensix_cfg_wr32(p_tensix, state_id, 4*(cfg_reg + i), p_tensix->dma_regs[pipe][gpr_address + i]);
        }
    } else {
        tensix_cfg_wr32(p_tensix, state_id, 4*cfg_reg, p_tensix->dma_regs[pipe][gpr_address]);
    }
    return true;
}

TENSIX_EXECUTE_RDCFG() {
    TTSIM_VERIFY(cfg_reg < 4*TENSIX_CFG_STATE_SIZE, UndefinedBehavior, "cfg_reg=%d", cfg_reg);
    TTSIM_VERIFY(gpr_address < std::size(p_tensix->dma_regs[pipe]), AssertionFailure, "gpr_address=%d out of range", gpr_address);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    p_tensix->dma_regs[pipe][gpr_address] = tensix_cfg_rd32(p_tensix, state_id, 4*cfg_reg);
    return true;
}

TENSIX_EXECUTE_SETC16() {
    TTSIM_VERIFY(setc16_reg < TENSIX_THD_STATE_SIZE, UndefinedBehavior, "setc16_reg=%d", setc16_reg);

    // Note that there's no way to read these back, so we don't apply the THREAD_CFG*_REG_MASK at present
    switch (setc16_reg) {
#define THREAD_CFG_REG_WR(i) case i: p_tensix->thread[pipe].thread_cfg##i = setc16_value; break;
        THREAD_CFG_REG_WR(0)
        THREAD_CFG_REG_WR(1)
#if TT_ARCH_VERSION == 1
        THREAD_CFG_REG_WR(2)
        THREAD_CFG_REG_WR(3)
        THREAD_CFG_REG_WR(5)
        THREAD_CFG_REG_WR(7)
        THREAD_CFG_REG_WR(12)
        THREAD_CFG_REG_WR(13)
        THREAD_CFG_REG_WR(14)
        THREAD_CFG_REG_WR(15)
        THREAD_CFG_REG_WR(16)
        THREAD_CFG_REG_WR(17)
        THREAD_CFG_REG_WR(18)
        THREAD_CFG_REG_WR(19)
        THREAD_CFG_REG_WR(28)
        THREAD_CFG_REG_WR(29)
        THREAD_CFG_REG_WR(30)
        THREAD_CFG_REG_WR(31)
        THREAD_CFG_REG_WR(32)
        THREAD_CFG_REG_WR(33)
        THREAD_CFG_REG_WR(34)
        THREAD_CFG_REG_WR(35)
        THREAD_CFG_REG_WR(37)
        THREAD_CFG_REG_WR(38)
        THREAD_CFG_REG_WR(39)
        THREAD_CFG_REG_WR(40)
        THREAD_CFG_REG_WR(41)
        THREAD_CFG_REG_WR(47)
        THREAD_CFG_REG_WR(48)
        THREAD_CFG_REG_WR(49)
        THREAD_CFG_REG_WR(50)
        THREAD_CFG_REG_WR(51)
        THREAD_CFG_REG_WR(52)
        THREAD_CFG_REG_WR(53)
        THREAD_CFG_REG_WR(54)
#else
        THREAD_CFG_REG_WR(2)
        THREAD_CFG_REG_WR(3)
        THREAD_CFG_REG_WR(5)
        THREAD_CFG_REG_WR(6)
        THREAD_CFG_REG_WR(7)
        THREAD_CFG_REG_WR(9)
        THREAD_CFG_REG_WR(11)
        THREAD_CFG_REG_WR(13)
        THREAD_CFG_REG_WR(15)
        THREAD_CFG_REG_WR(17)
        THREAD_CFG_REG_WR(19)
        THREAD_CFG_REG_WR(21)
        THREAD_CFG_REG_WR(23)
        THREAD_CFG_REG_WR(24)
        THREAD_CFG_REG_WR(25)
        THREAD_CFG_REG_WR(26)
        THREAD_CFG_REG_WR(27)
        THREAD_CFG_REG_WR(28)
        THREAD_CFG_REG_WR(29)
        THREAD_CFG_REG_WR(30)
        THREAD_CFG_REG_WR(31)
        THREAD_CFG_REG_WR(32)
        THREAD_CFG_REG_WR(33)
        THREAD_CFG_REG_WR(34)
        THREAD_CFG_REG_WR(39)
        THREAD_CFG_REG_WR(48)
        THREAD_CFG_REG_WR(49)
        THREAD_CFG_REG_WR(50)
        THREAD_CFG_REG_WR(51)
        THREAD_CFG_REG_WR(52)
        THREAD_CFG_REG_WR(53)
        THREAD_CFG_REG_WR(54)
        THREAD_CFG_REG_WR(55)
#endif
#undef THREAD_CFG_REG_WR
        default: TTSIM_ERROR(UnimplementedFunctionality, "setc16_reg=%d", setc16_reg);
    }
    return true;
}

static bool tensix_execute_rmwcib(TensixState *p_tensix, uint32_t pipe, uint32_t data, uint32_t mask, uint32_t cfg_reg_addr) {
    uint32_t state_id = get_state_id(p_tensix, pipe);
    uint32_t value = tensix_cfg_rd32(p_tensix, state_id, 4*cfg_reg_addr);
    value = (value & ~mask) | (data & mask);
    tensix_cfg_wr32(p_tensix, state_id, 4*cfg_reg_addr, value);
    return true;
}

TENSIX_EXECUTE_RMWCIB0() { return tensix_execute_rmwcib(p_tensix, pipe, data << 0,  mask << 0,  cfg_reg_addr); }
TENSIX_EXECUTE_RMWCIB1() { return tensix_execute_rmwcib(p_tensix, pipe, data << 8,  mask << 8,  cfg_reg_addr); }
TENSIX_EXECUTE_RMWCIB2() { return tensix_execute_rmwcib(p_tensix, pipe, data << 16, mask << 16, cfg_reg_addr); }
TENSIX_EXECUTE_RMWCIB3() { return tensix_execute_rmwcib(p_tensix, pipe, data << 24, mask << 24, cfg_reg_addr); }

#if TT_ARCH_VERSION >= 1
TENSIX_EXECUTE_CFGSHIFTMASK() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!right_cshift_amt, UnsupportedFunctionality, "right_cshift_amt=%d", right_cshift_amt);
    TTSIM_VERIFY(mask_width == 31, UnsupportedFunctionality, "mask_width=%d", mask_width);
    TTSIM_VERIFY(operation == 3, UnsupportedFunctionality, "operation=%d", operation);
    TTSIM_VERIFY(disable_mask_on_old_val == 1, UnsupportedFunctionality, "disable_mask_on_old_val=%d", disable_mask_on_old_val);

    uint32_t scratch_index = (scratch_sel == 3) ? pipe : scratch_sel;
    uint32_t scratch_value;
    switch (scratch_index) {
        case 0: scratch_value = p_tensix->SCRATCH_SEC0_val; break;
        case 1: scratch_value = p_tensix->SCRATCH_SEC1_val; break;
        case 2: scratch_value = p_tensix->SCRATCH_SEC2_val; break;
        default: TTSIM_ERROR(AssertionFailure, "scratch_index=%d", scratch_index);
    }
    uint32_t state_id = get_state_id(p_tensix, pipe);
    uint32_t cfg_value = tensix_cfg_rd32(p_tensix, state_id, 4*cfg_reg);
    cfg_value += scratch_value;
    tensix_cfg_wr32(p_tensix, state_id, 4*cfg_reg, cfg_value);
    return true;
#else
    TTSIM_ERROR_NOFMT(MissingSpecification);
#endif
}
#endif

#define UNIMPLEMENTED_TENSIX_INST(name) \
    TENSIX_EXECUTE_##name() { TTSIM_ERROR_NOFMT(UnimplementedFunctionality); }

TENSIX_DECODERS()

bool tensix_decode_and_execute(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    switch (bits<31,24>(inst)) {
        case 0x79: // SFPIADD
        case 0x7A: // SFPSHFT
        case 0x7B: // SFPSETCC
        case 0x7F: // SFPOR
        case 0x80: // SFPNOT
        case 0x84: // SFPMAD
        case 0x89: // SFPSETSGN
        case 0x8A: // SFPENCC
        case 0x8E: // SFP_STOCH_RND
        case 0x90: // SFPCAST
        case 0x92: // SFPSWAP
        case 0x94: // SFPSHFT2
#if TT_ARCH_VERSION == 1
        case 0x96: // SFPLE
        case 0x97: // SFPGT
        case 0x98: // SFPMUL24
        case 0x99: // SFPARECIP
#endif
        {
            uint32_t lreg_dest = bits<7,4>(inst);
            if ((lreg_dest >= 12) && (lreg_dest <= 15)) {
                uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
                TTSIM_VERIFY(mask == 0xFFFFFFFF, UnsupportedFunctionality, "inst=0x%x: not all lanes enabled: mask=0x%x", inst, mask);
                p_tensix->load_macro_instruction_template[lreg_dest - 12] = inst;
                return true;
            }
            break;
        }
    }
    switch (uint32_t opcode = bits<31,24>(inst)) {
        case 0x01: TTSIM_ERROR(AssertionFailure, "MOP should not exist at this stage"); // MOP
        case 0x02: TTSIM_ERROR(AssertionFailure, "NOP should not exist at this stage"); // NOP
        case 0x03: TTSIM_ERROR(AssertionFailure, "MOP_CFG should not exist at this stage"); // MOP_CFG
        case 0x04: TTSIM_ERROR(AssertionFailure, "REPLAY should not exist at this stage"); // REPLAY
        TENSIX_OPCODE_CASES() // use the generated decoder for the rest
        default: TTSIM_ERROR(UndefinedBehavior, "opcode=0x%x", opcode);
    }
    return true;
}
