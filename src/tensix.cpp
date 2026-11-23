// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Tensix functional model: instruction expansion, FPU/SFPU/unpacker/packer execution, per-pipe state.
// https://github.com/tenstorrent/tt-isa-documentation
// https://github.com/tenstorrent/tt-llk/blob/main/tt_llk_wormhole_b0/instructions/assembly.yaml
// https://github.com/tenstorrent/tt-llk/blob/main/tt_llk_blackhole/instructions/assembly.yaml

#include "sim.h"
#include "simd.h"
#include "tensix_decode.h" // generated
#include <algorithm>

#define IS_TENSIX_NOP(inst) (bits<31,24>(inst) == TENSIX_OPCODE_NOP)

void tensix_init(TensixState *p_tensix, uint32_t tile_id, uint32_t tensix_id) {
    memset(p_tensix, 0, sizeof(TensixState));
    memset(p_tensix->sem_max, 15, sizeof(p_tensix->sem_max));
    p_tensix->tile_id = tile_id;
    p_tensix->tensix_id = tensix_id;
    for (uint32_t row = 0; row < DST_ROWS; row++) {
        p_tensix->dst_row_valid[row] = true; // hardware resets zero flags to 0, not 1
    }
    for (uint32_t lane = 0; lane < 32; lane++) {
        p_tensix->l_regs[8][lane] = 0x3F56594B;
        p_tensix->l_regs[10][lane] = 0x3F800000;
        p_tensix->l_regs[11][lane] = 0xBF800000;
        p_tensix->l_regs[12][lane] = 0x3B000000;
        p_tensix->l_regs[13][lane] = 0xBF2CC4C7;
        p_tensix->l_regs[14][lane] = 0xBEB08FF9;
        p_tensix->l_regs[15][lane] = lane << 1;
    }
}

uint32_t tensix_debug_array_rd32(const TensixState *p_tensix, uint32_t cmd) {
    uint32_t row = bits<11,0>(cmd);
    uint32_t sel = bits<15,12>(cmd);
    uint32_t upper = bits<31,16>(cmd);
    TTSIM_VERIFY(row < DST_ROWS, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD: row=%d", row);
    TTSIM_VERIFY(sel < 8, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD: sel=%d", sel);
    TTSIM_VERIFY(upper == 2, UnsupportedFunctionality, "DBG_ARRAY_RD_CMD: upper=0x%x", upper);
    // Note: BH does not appear to apply dst_row_valid in most cases, though in some cases
    // it will pick up carried state from a previous packer instruction (not modeled here)
    if ((TT_ARCH_VERSION == 1) || p_tensix->dst_row_valid[row]) {
        return p_tensix->dst[row][2*sel] | (uint32_t(p_tensix->dst[row][2*sel + 1]) << 16);
    }
    return 0;
}

static void tensix_push_inst_fifo(TensixState *p_tensix, uint32_t pipe, uint32_t inst, InstOrigin origin) {
    if (IS_TENSIX_NOP(inst)) {
        return; // eat NOPs here so we don't overflow as quickly on MOP
    }
    uint32_t inst_wr_ptr = p_tensix->inst_wr_ptr[pipe];
    p_tensix->inst[pipe][inst_wr_ptr] = inst;
    p_tensix->inst_origin[pipe][inst_wr_ptr] = origin;
    p_tensix->inst_origin_count[pipe][origin]++;
    inst_wr_ptr = (inst_wr_ptr + 1) % TENSIX_INST_FIFO_SIZE;
    p_tensix->inst_wr_ptr[pipe] = inst_wr_ptr;
    p_tensix->inst_pipes_active |= 1 << pipe;
    ttsim_t_tile_set_busy(p_tensix->tile_id);

    TTSIM_VERIFY(inst_wr_ptr != p_tensix->inst_rd_ptr[pipe], UnimplementedFunctionality, "pipe %d inst fifo full", pipe); // require 1 entry always empty
}

static void replay_expander(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    if (p_tensix->replay_left[pipe]) {
        TTSIM_VERIFY((bits<31,24>(inst) != TENSIX_OPCODE_REPLAY), UndefinedBehavior,
            "nested REPLAY instruction loaded inside a REPLAY buffer: inst=0x%x", inst);
        uint32_t index = p_tensix->replay_index[pipe];
        p_tensix->replay_buf[pipe][index] = inst;
        p_tensix->replay_index[pipe] = index + 1;
        p_tensix->replay_left[pipe]--;
        if (p_tensix->replay_execute_while_loading[pipe]) {
            tensix_push_inst_fifo(p_tensix, pipe, inst, p_tensix->mop[pipe].busy ? INST_MOP : INST_PUSH);
        }
    } else if (bits<31,24>(inst) == TENSIX_OPCODE_REPLAY) {
        TTSIM_VERIFY(!(inst & 0xF83C0C), NonContractualBehavior, "REPLAY inst=0x%x", inst); // unused bits in encoding
        uint32_t load_mode = bits<0,0>(inst);
        uint32_t execute_while_loading = bits<1,1>(inst);
        if (!load_mode) {
            TTSIM_VERIFY(!execute_while_loading, NonContractualBehavior, "execute_while_loading=%d ignored without load_mode", execute_while_loading);
        }
        uint32_t len = bits<13,4>(inst);
        uint32_t start_idx = bits<23,14>(inst);
#if TT_ARCH_VERSION == 0
        TTSIM_VERIFY(len, NonContractualBehavior, "len=%d (32 entries)", len);
#elif TT_ARCH_VERSION == 1
        // Recording Count 0 without Exec can stop RISCV T's pushes for good: its retire tracking then expects none of the
        // 64 recorded
        TTSIM_VERIFY(len || !load_mode || execute_while_loading, UndefinedBehavior, "len=%d", len);
        // Not checked: the tracking also loses entries or retires ones never queued, which can stop RISCV T's pushes
        // for good, when a recording without Exec is a REPLAY the MOP expander emits, records a MOP's words, or has a
        // MOP_CFG pushed during it; and a MOP whose last instruction is a REPLAY with Load (Exec or not) never retires
        TTSIM_VERIFY(len, NonContractualBehavior, "len=%d (64 entries)", len);
#else
        TTSIM_VERIFY(len, UndefinedBehavior, "len=%d", len);
#endif
#if TT_ARCH_VERSION == 0
        // The count left is 5 bits: Count 33..63 runs Count - 32 entries, but a replay of 33 runs 33 or 1 by timing
        TTSIM_VERIFY(load_mode || (len != 33), UndefinedBehavior, "start_idx=%d len=%d", start_idx, len);
        TTSIM_VERIFY(start_idx + len <= REPLAY_BUF_ENTRIES, NonContractualBehavior, "start_idx=%d len=%d", start_idx, len);
#elif TT_ARCH_VERSION == 1
        TTSIM_VERIFY(start_idx + len <= REPLAY_BUF_ENTRIES, NonContractualBehavior, "start_idx=%d len=%d wraps", start_idx, len);
#else
        TTSIM_VERIFY(start_idx + len <= REPLAY_BUF_ENTRIES, UndefinedBehavior, "start_idx=%d len=%d", start_idx, len);
#endif

        uint32_t base = 0;
        if (load_mode) {
            p_tensix->replay_index[pipe] = base + start_idx;
            p_tensix->replay_left[pipe] = len;
            p_tensix->replay_execute_while_loading[pipe] = execute_while_loading;
        } else {
            // Emitted by tensix_frontend_step as the FIFO has room
            p_tensix->replay_play_index[pipe] = base + start_idx;
            p_tensix->replay_play_left[pipe] = len;
        }
    } else {
        tensix_push_inst_fifo(p_tensix, pipe, inst, p_tensix->mop[pipe].busy ? INST_MOP : INST_PUSH);
    }
}

static void mop_cfg(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    TTSIM_VERIFY(!(inst & 0xFF0000), NonContractualBehavior, "MOP_CFG inst=0x%x", inst); // unused bits in encoding
    p_tensix->mop_zmask_hi16[pipe] = bits<15,0>(inst);
}

// The MOP configuration is sampled when the MOP is ingested, and the expanders emit one instruction at a time as the
// instruction FIFO has room, so a thread's pushes stall while its expanders are busy
static void mop_ingest(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    uint32_t zlo = bits<15,0>(inst);
    uint32_t mop_type = bits<23,23>(inst);
    const uint32_t *cfg = p_tensix->mop_cfg[pipe];
    uint32_t cfg0 = cfg[0];
    if (!mop_type) {
        TTSIM_VERIFY(cfg[1] <= 3, UnsupportedFunctionality, "flags=0x%x", cfg[1]);
    } else {
        uint32_t loop_count = bits<22,16>(inst);
        TTSIM_VERIFY(!zlo, UnimplementedFunctionality, "zlo=%d (mop_type=%d)", zlo, mop_type);
        TTSIM_VERIFY(!loop_count, UnimplementedFunctionality, "loop_count=%d (mop_type=%d)", loop_count, mop_type);
#if TT_ARCH_VERSION == 0
        TTSIM_VERIFY((cfg0 >= 1) && (cfg0 <= 32), UnimplementedFunctionality, "outer_loop_len=%d", cfg0);
        TTSIM_VERIFY((cfg[1] >= 1) && (cfg[1] <= 127), UnsupportedFunctionality, "inner_loop_len=%d", cfg[1]);
#else
        TTSIM_VERIFY((cfg0 | cfg[1]) <= 0x3FF, NonContractualBehavior, "outer_loop_len=0x%x inner_loop_len=0x%x", cfg0, cfg[1]);
        // A MOP that expands to nothing never retires (the retire tracking retires a MOP when its last instruction issues)
        TTSIM_VERIFY(cfg0 && (cfg[1] || !IS_TENSIX_NOP(cfg[2]) || !IS_TENSIX_NOP(cfg[3])), UndefinedBehavior,
            "outer_loop_len=%d inner_loop_len=%d", cfg0, cfg[1]);
        TTSIM_VERIFY(cfg[1], UnsupportedFunctionality, "inner_loop_len=0 (outer_loop_len=%d)", cfg0);
#endif
    }
    TensixMopState *p_mop = &p_tensix->mop[pipe];
    p_mop->busy = true;
    p_mop->inst = inst;
    p_mop->zmask = zlo | (uint32_t(p_tensix->mop_zmask_hi16[pipe]) << 16);
    std::copy(cfg, cfg + 9, p_mop->cfg);
    p_mop->cfg[0] = cfg0;
    p_mop->iter = 0;
    p_mop->step = 0;
}

enum MopStep { MOP_STEP_END, MOP_STEP_EMIT, MOP_STEP_SKIP };

// What step `step` of iteration `iter` of the MOP being expanded does: emit *p_inst, emit nothing, or (past the last
// step) end the iteration
static MopStep mop_step_inst(const TensixMopState *p_mop, uint32_t iter, uint32_t step, uint32_t *p_inst) {
    const uint32_t *cfg = p_mop->cfg;
    if (!bits<23,23>(p_mop->inst)) {
        uint32_t flags = cfg[1];
        if ((iter < 32) && ((p_mop->zmask >> iter) & 1)) {
            // emits cfg[7], then cfg[8] if flags bit 0
            static const uint8_t seq[2] = {7, 8};
            if (step >= 1 + (flags & 1)) {
                return MOP_STEP_END;
            }
            *p_inst = cfg[seq[step]];
        } else {
            // emits cfg[3], then cfg[4..6] if flags bit 1, then cfg[2] if flags bit 0
            uint32_t n = 1 + ((flags & 2) ? 3 : 0) + (flags & 1);
            if (step >= n) {
                return MOP_STEP_END;
            }
            *p_inst = (step == 0) ? cfg[3] : ((flags & 2) && (step <= 3)) ? cfg[3 + step] : cfg[2];
        }
        return MOP_STEP_EMIT;
    }
    uint32_t outer_loop_len = cfg[0];
    uint32_t start_op = cfg[2];
    uint32_t end_op0 = cfg[3];
    uint32_t end_op1 = cfg[4];
    uint32_t loop_op = cfg[5];
    uint32_t loop_op1 = cfg[6];
    uint32_t inner_loop_len = cfg[1];
    uint32_t loop_op_flip = 0;
    if (!IS_TENSIX_NOP(loop_op1)) {
        loop_op_flip = loop_op ^ loop_op1;
        inner_loop_len *= 2;
    }
    // step 0: start_op; steps 1..inner_loop_len: the inner loop; then end_op0 and end_op1. NOP start and end ops
    // are skipped, and end_op1 is used only with end_op0.
    if (step == 0) {
        *p_inst = start_op;
        return IS_TENSIX_NOP(start_op) ? MOP_STEP_SKIP : MOP_STEP_EMIT;
    } else if (step <= inner_loop_len) {
        uint32_t j = step - 1;
        if (j + 1 < inner_loop_len) {
            *p_inst = loop_op ^ ((j & 1) ? loop_op_flip : 0);
        } else if (iter + 1 < outer_loop_len) {
            *p_inst = cfg[8]; // loop1_last_instr
        } else {
            *p_inst = cfg[7]; // loop0_last_instr
        }
    } else if (step == inner_loop_len + 1) {
        *p_inst = end_op0;
        return IS_TENSIX_NOP(end_op0) ? MOP_STEP_SKIP : MOP_STEP_EMIT;
    } else if (step == inner_loop_len + 2) {
        *p_inst = end_op1;
        return (IS_TENSIX_NOP(end_op0) || IS_TENSIX_NOP(end_op1)) ? MOP_STEP_SKIP : MOP_STEP_EMIT;
    } else {
        return MOP_STEP_END;
    }
    return MOP_STEP_EMIT;
}

// Next instruction the MOP expander emits; false once the expansion is complete
static bool mop_next(TensixState *p_tensix, uint32_t pipe, uint32_t *p_inst) {
    TensixMopState *p_mop = &p_tensix->mop[pipe];
    uint32_t iters = bits<23,23>(p_mop->inst) ? p_mop->cfg[0] : bits<22,16>(p_mop->inst) + 1;
    while (p_mop->iter < iters) {
        MopStep step = mop_step_inst(p_mop, p_mop->iter, p_mop->step, p_inst);
        if (step == MOP_STEP_END) {
            p_mop->iter++;
            p_mop->step = 0;
            continue;
        }
        p_mop->step++;
        if (step == MOP_STEP_EMIT) {
            return true;
        }
    }
    p_mop->busy = false;
    return false;
}

static uint32_t inst_fifo_used(const TensixState *p_tensix, uint32_t pipe) {
    return (p_tensix->inst_wr_ptr[pipe] + TENSIX_INST_FIFO_SIZE - p_tensix->inst_rd_ptr[pipe]) % TENSIX_INST_FIFO_SIZE;
}

static inline bool tensix_expanding(const TensixState *p_tensix, uint32_t pipe) {
    return p_tensix->mop[pipe].busy || p_tensix->replay_play_left[pipe];
}

// Moves instructions out of the MOP and Replay Expanders into the instruction FIFO while it has room
void tensix_frontend_step(TensixState *p_tensix, uint32_t pipe) {
    bool was_expanding = tensix_expanding(p_tensix, pipe);
    while (inst_fifo_used(p_tensix, pipe) < TENSIX_INST_FIFO_SIZE - 1) { // the FIFO keeps one entry empty
        if (p_tensix->replay_play_left[pipe]) {
            // A REPLAY the MOP Expander emitted holds a place behind it until its last instruction
            bool mop_replay_end = p_tensix->mop[pipe].busy && (p_tensix->replay_play_left[pipe] == 1);
            tensix_push_inst_fifo(p_tensix, pipe, p_tensix->replay_buf[pipe][p_tensix->replay_play_index[pipe]],
                mop_replay_end ? INST_MOP : INST_REPLAY);
            p_tensix->replay_play_index[pipe]++;
            p_tensix->replay_play_left[pipe]--;
            continue;
        }
        uint32_t inst;
        if (!p_tensix->mop[pipe].busy || !mop_next(p_tensix, pipe, &inst)) {
            break;
        }
        replay_expander(p_tensix, pipe, inst);
    }
    uint32_t last = (p_tensix->inst_wr_ptr[pipe] + TENSIX_INST_FIFO_SIZE - 1) % TENSIX_INST_FIFO_SIZE;
    uint8_t *p_origin = &p_tensix->inst_origin[pipe][last];
    bool fifo_empty = p_tensix->inst_rd_ptr[pipe] == p_tensix->inst_wr_ptr[pipe];
    if (was_expanding && !tensix_expanding(p_tensix, pipe) && !fifo_empty && (*p_origin != INST_PUSH)) {
        // The expansion's last instruction ends the push
        p_tensix->inst_origin_count[pipe][*p_origin]--;
        p_tensix->inst_origin_count[pipe][INST_PUSH]++;
        *p_origin = INST_PUSH;
    }
    if (tensix_expanding(p_tensix, pipe)) {
        p_tensix->expand_pipes_active |= 1 << pipe;
        ttsim_t_tile_set_busy(p_tensix->tile_id);
    } else {
        p_tensix->expand_pipes_active &= ~(1 << pipe);
    }
}

// Whether every instruction this thread has pushed has finished executing
bool tensix_thread_idle(const TensixState *p_tensix, uint32_t pipe) {
    if (tensix_expanding(p_tensix, pipe)) {
        return false;
    }
    return p_tensix->inst_rd_ptr[pipe] == p_tensix->inst_wr_ptr[pipe];
}

// Whether the thread's MOP Expander has finished expanding
bool tensix_mop_expander_idle(const TensixState *p_tensix, uint32_t pipe) {
    return !p_tensix->mop[pipe].busy;
}

bool tensix_can_push_inst(const TensixState *p_tensix, uint32_t pipe, bool bypass_mop_expander) {
    if (tensix_expanding(p_tensix, pipe)) {
        return false; // a thread's pushes stall while its expanders are busy
    }
    // A MOP or REPLAY takes one place until the last instruction it expands to has left the FIFO, and what it expands
    // to fills only the places between its expander and the Wait Gate: 2 behind the Replay Expander, and on WH 9 more
    // behind the MOP Expander
    const uint32_t *count = p_tensix->inst_origin_count[pipe];
    uint32_t replayed = std::min(count[INST_REPLAY], 2u);
    if (bypass_mop_expander) {
        // BRISC's instructions go to the thread's 8-entry request FIFO, and from there through the Replay Expander to
        // its 2-entry instruction buffer, so a push waits while 10 are waiting to issue
        return count[INST_PUSH] + replayed < 10;
    }
#if TT_ARCH_VERSION == 0
    // T0's MOP buffer holds 32 instructions and T1's and T2's 16; 11 more fit between it and the Wait Gate
    return count[INST_PUSH] + std::min(count[INST_MOP], 9u) + replayed < (pipe ? 27 : 43);
#else
    // A push stalls while 29 of the thread's pushes are waiting to pass the Wait Gate, however many instructions the
    // MOPs and REPLAYs among them expand to
    return count[INST_PUSH] < 29;
#endif
}

void tensix_push_inst(TensixState *p_tensix, uint32_t pipe, uint32_t inst, bool bypass_mop_expander) {
    if (bypass_mop_expander) {
        replay_expander(p_tensix, pipe, inst);
        tensix_frontend_step(p_tensix, pipe);
        return;
    }
    uint32_t opcode = bits<31,24>(inst);
    switch (opcode) {
        case TENSIX_OPCODE_MOP: mop_ingest(p_tensix, pipe, inst); break;
        case TENSIX_OPCODE_MOP_CFG: mop_cfg(p_tensix, pipe, inst); break;
        default: replay_expander(p_tensix, pipe, inst); break; // everything else is just passthrough
    }
    tensix_frontend_step(p_tensix, pipe);
}

static inline uint32_t step_prng(uint32_t state) {
    uint32_t taps = __builtin_popcount(state & 0x80200003);
    return (~taps << 31) | (state >> 1);
}

static void seed_prng(TensixState *p_tensix, uint32_t seed) {
    for (uint32_t i = 0; i < 32 + TT_ARCH_VERSION; i++) { // Blackhole's seeder steps once more than Wormhole's
        seed = step_prng(seed);
    }
    for (uint32_t i = 0; i < 32; i++) {
        seed = step_prng(seed);
        seed = step_prng(seed);
        p_tensix->prng_state[31 - i] = seed;
    }
}

uint32_t tensix_cfg_rd32(TensixState *p_tensix, uint32_t bank, uint32_t offset) {
    TTSIM_ASSERT(!(offset & 3), "misaligned offset=0x%x", offset);
    switch (uint32_t reg = offset / 4) {
#define CFG_REG_BANKED_RD(i) case i: return p_tensix->config[bank].cfg##i;
#define CFG_REG_RD(i) case i: return p_tensix->cfg##i;
        CFG_REG_BANKED_RD(0)
        CFG_REG_BANKED_RD(1)
        CFG_REG_BANKED_RD(2)
        CFG_REG_BANKED_RD(3)
#if TT_ARCH_VERSION == 0
        CFG_REG_BANKED_RD(8)
        CFG_REG_BANKED_RD(9)
        case 10: return 0;
        case 11: return 0;
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
        case RISC_PREFETCH_CTRL_Enable_Trisc_ADDR32: return g_t_tiles[p_tensix->tile_id].risc_prefetch_ctrl;
#elif TT_ARCH_VERSION == 1
        CFG_REG_BANKED_RD(12)
        CFG_REG_BANKED_RD(13)
        CFG_REG_BANKED_RD(14)
        CFG_REG_BANKED_RD(15)
        CFG_REG_BANKED_RD(17)
        CFG_REG_BANKED_RD(18)
        CFG_REG_BANKED_RD(19)
        CFG_REG_BANKED_RD(24)
        CFG_REG_BANKED_RD(28)
        CFG_REG_BANKED_RD(36)
        CFG_REG_BANKED_RD(40)
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
        CFG_REG_BANKED_RD(119)
        CFG_REG_BANKED_RD(120)
        CFG_REG_BANKED_RD(121)
        case 122: return 0;
        case 123: return 0;
        CFG_REG_BANKED_RD(124)
        CFG_REG_BANKED_RD(125)
        CFG_REG_BANKED_RD(140)
        CFG_REG_BANKED_RD(141)
        case RISC_PREFETCH_CTRL_Enable_Trisc_ADDR32: return g_t_tiles[p_tensix->tile_id].risc_prefetch_ctrl;
        CFG_REG_RD(220)
#endif
#undef CFG_REG_BANKED_RD
#undef CFG_REG_RD
        default: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
    }
}

void tensix_cfg_wr32(TensixState *p_tensix, uint32_t bank, uint32_t offset, uint32_t data) {
#if TT_ARCH_VERSION == 0
    TensixTile *p_tile = &g_t_tiles[p_tensix->tile_id];
#endif
    TTSIM_ASSERT(!(offset & 3), "misaligned offset=0x%x", offset);
    switch (uint32_t reg = offset / 4) {
#define CFG_REG_BANKED_WR(i) case i: p_tensix->config[bank].cfg##i = data & CFG##i##_REG_MASK; break;
#define CFG_REG_WR(i) case i: p_tensix->cfg##i = data & CFG##i##_REG_MASK; break;
        CFG_REG_BANKED_WR(0)
        CFG_REG_BANKED_WR(1)
        CFG_REG_BANKED_WR(2)
        CFG_REG_BANKED_WR(3)
#if TT_ARCH_VERSION == 0
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
        CFG_REG_BANKED_WR(41)
        CFG_REG_BANKED_WR(44)
        CFG_REG_BANKED_WR(45)
        case UNP1_ADDR_CTRL_XY_REG_1_Xstride_ADDR32: // unpacker 1's copy is not modeled
            TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(47)
        case UNP1_FORCED_SHARED_EXP_shared_exp_ADDR32: // unpacker 1's copy is not modeled
            TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(52)
        case 53: p_tensix->config[bank].cfg53 = data; break;
        CFG_REG_BANKED_WR(56)
        CFG_REG_BANKED_WR(57)
        CFG_REG_BANKED_WR(58)
        CFG_REG_BANKED_WR(59)
        CFG_REG_BANKED_WR(60)
        CFG_REG_BANKED_WR(61)
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
        CFG_REG_BANKED_WR(96)
        CFG_REG_BANKED_WR(97)
        CFG_REG_BANKED_WR(98)
        CFG_REG_BANKED_WR(99)
        CFG_REG_BANKED_WR(100)
        CFG_REG_BANKED_WR(101)
        CFG_REG_BANKED_WR(104)
        CFG_REG_BANKED_WR(105)
        case THCON_SEC1_REG7_Offset_address_ADDR32: // unpacker 1's copy is not modeled
        case THCON_SEC1_REG7_Offset_cntx1_address_ADDR32:
            TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(124)
        CFG_REG_BANKED_WR(125)
        CFG_REG_BANKED_WR(126)
        CFG_REG_BANKED_WR(127)
        CFG_REG_WR(152)
        CFG_REG_WR(153)
        CFG_REG_WR(154)
        CFG_REG_WR(155)
        case RISCV_IC_INVALIDATE_InvalidateAll_ADDR32:
            TTSIM_VERIFY(data, UnsupportedFunctionality, "no-op mask: RISCV_IC_INVALIDATE_InvalidateAll: data=0x%x", data);
            TTSIM_VERIFY(data <= 31, UnsupportedFunctionality, "RISCV_IC_INVALIDATE_InvalidateAll: data=0x%x", data);
            for (uint32_t rv32_id = 0; rv32_id < RV32_CORES_PER_T_TILE; rv32_id++) {
                if (data & (1u << rv32_id)) {
                    rv32_icache_invalidate(&g_t_tiles[p_tensix->tile_id].rv32[rv32_id]);
                }
            }
            break;
        case TRISC_RESET_PC_SEC0_PC_ADDR32: p_tile->trisc0_reset_pc = data; break;
        case TRISC_RESET_PC_SEC1_PC_ADDR32: p_tile->trisc1_reset_pc = data; break;
        case TRISC_RESET_PC_SEC2_PC_ADDR32: p_tile->trisc2_reset_pc = data; break;
        case TRISC_RESET_PC_OVERRIDE_Reset_PC_Override_en_ADDR32: p_tile->trisc_reset_pc_override = data; break;
        case NCRISC_RESET_PC_PC_ADDR32: p_tile->ncrisc_reset_pc = data; break;
        case NCRISC_RESET_PC_OVERRIDE_Reset_PC_Override_en_ADDR32: p_tile->ncrisc_reset_pc_override = data; break;
        case PRNG_SEED_Seed_Val_ADDR32: seed_prng(p_tensix, data); break;
        case RISC_PREFETCH_CTRL_Enable_Trisc_ADDR32: risc_prefetch_ctrl_wr(p_tensix->tile_id, 0, data); break;
#elif TT_ARCH_VERSION == 1
        CFG_REG_BANKED_WR(12)
        CFG_REG_BANKED_WR(13)
        CFG_REG_BANKED_WR(14)
        CFG_REG_BANKED_WR(15)
        CFG_REG_BANKED_WR(17)
        CFG_REG_BANKED_WR(18)
        CFG_REG_BANKED_WR(19)
        CFG_REG_BANKED_WR(20)
        CFG_REG_BANKED_WR(21)
        CFG_REG_BANKED_WR(24)
        CFG_REG_BANKED_WR(25)
        CFG_REG_BANKED_WR(28)
        CFG_REG_BANKED_WR(36)
        CFG_REG_BANKED_WR(40) // PACK_GLOBAL_CFG_CTL: the tile-end drain; changes timing only, not modeled
        CFG_REG_BANKED_WR(50)
        CFG_REG_BANKED_WR(56)
        CFG_REG_BANKED_WR(57)
        case UNP1_ADDR_CTRL_XY_REG_1_Xstride_ADDR32: // unpacker 1's copy is not modeled
            TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(59)
        case UNP1_FORCED_SHARED_EXP_shared_exp_ADDR32: // unpacker 1's copy is not modeled
            TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
        CFG_REG_BANKED_WR(64)
        case 65: p_tensix->config[bank].cfg65 = data; break;
        CFG_REG_BANKED_WR(68)
        CFG_REG_BANKED_WR(69)
        CFG_REG_BANKED_WR(70)
        CFG_REG_BANKED_WR(71)
        CFG_REG_BANKED_WR(72)
        CFG_REG_BANKED_WR(73)
        CFG_REG_BANKED_WR(76)
        CFG_REG_BANKED_WR(77)
        CFG_REG_BANKED_WR(84)
        CFG_REG_BANKED_WR(86)
        CFG_REG_BANKED_WR(92)
        CFG_REG_BANKED_WR(93)
        CFG_REG_BANKED_WR(112)
        case 113: p_tensix->config[bank].cfg113 = data; break;
        CFG_REG_BANKED_WR(119)
        CFG_REG_BANKED_WR(120)
        CFG_REG_BANKED_WR(121)
        CFG_REG_BANKED_WR(124)
        CFG_REG_BANKED_WR(125)
        CFG_REG_BANKED_WR(140)
        CFG_REG_BANKED_WR(141)
        CFG_REG_WR(180)
        CFG_REG_WR(181)
        CFG_REG_WR(182)
        CFG_REG_WR(183)
        case RISCV_IC_INVALIDATE_InvalidateAll_ADDR32:
            TTSIM_VERIFY(data, UnsupportedFunctionality, "no-op mask: RISCV_IC_INVALIDATE_InvalidateAll: data=0x%x", data);
            TTSIM_VERIFY(data <= 31, UnsupportedFunctionality, "RISCV_IC_INVALIDATE_InvalidateAll: data=0x%x", data);
            for (uint32_t rv32_id = 0; rv32_id < RV32_CORES_PER_T_TILE; rv32_id++) {
                if (data & (1u << rv32_id)) {
                    rv32_icache_invalidate(&g_t_tiles[p_tensix->tile_id].rv32[rv32_id]);
                }
            }
            break;
        case PRNG_SEED_Seed_Val_ADDR32: seed_prng(p_tensix, data); break;
        case RISC_PREFETCH_CTRL_Enable_Trisc_ADDR32: risc_prefetch_ctrl_wr(p_tensix->tile_id, 0, data); break;
        CFG_REG_WR(209)
        CFG_REG_WR(210)
        CFG_REG_WR(211)
        CFG_REG_WR(220)
#endif
#undef CFG_REG_BANKED_WR
#undef CFG_REG_WR
        default: TTSIM_ERROR(UnsupportedFunctionality, "reg=%d", reg);
    }

    // Registers/fields where we enforce that values cannot be set; this simplifies state machines elsewhere
    // XXX Move this into code generators so we can check these only on the specific case statements that modify them
    const TensixConfigState *p_config = &p_tensix->config[bank];
    CHECK_UNSUPPORTED_CFG_FIELDS(p_config);
    CHECK_UNIMPLEMENTED_CFG_FIELDS(p_config);
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_Fpu_srnd_en, UnsupportedFunctionality,
        "ALU_ROUNDING_MODE_Fpu_srnd_en: stochastic rounding is explicitly out of scope");
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_Gasket_srnd_en, UnsupportedFunctionality,
        "ALU_ROUNDING_MODE_Gasket_srnd_en: stochastic rounding is explicitly out of scope");
    TTSIM_VERIFY(!p_config->ALU_ROUNDING_MODE_Packer_srnd_en, UnsupportedFunctionality,
        "ALU_ROUNDING_MODE_Packer_srnd_en: stochastic rounding is explicitly out of scope");
    TTSIM_VERIFY(!p_tensix->DEST_TARGET_REG_CFG_PACK_SEC0_ZOffset, UnsupportedFunctionality, "DEST_TARGET_REG_CFG_PACK_SEC0_ZOffset");
    TTSIM_VERIFY(!p_tensix->DEST_TARGET_REG_CFG_PACK_SEC1_ZOffset, UnsupportedFunctionality, "DEST_TARGET_REG_CFG_PACK_SEC1_ZOffset");
    TTSIM_VERIFY(!p_tensix->DEST_TARGET_REG_CFG_PACK_SEC2_ZOffset, UnsupportedFunctionality, "DEST_TARGET_REG_CFG_PACK_SEC2_ZOffset");
    TTSIM_VERIFY(!p_tensix->DEST_TARGET_REG_CFG_PACK_SEC3_ZOffset, UnsupportedFunctionality, "DEST_TARGET_REG_CFG_PACK_SEC3_ZOffset");
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!p_tensix->DEST_ACCESS_CFG_disable_full_write_dest_q_bypass, UnsupportedFunctionality, "DEST_ACCESS_CFG_disable_full_write_dest_q_bypass");
    TTSIM_VERIFY(!p_tensix->DEST_ACCESS_CFG_zeroacc_absolute_tile_mode, UnsupportedFunctionality, "DEST_ACCESS_CFG_zeroacc_absolute_tile_mode");
#endif
}

static inline uint32_t get_state_id(const TensixState *p_tensix, uint32_t pipe) {
    return p_tensix->thread[pipe].CFG_STATE_ID_StateID;
}

static inline uint32_t math_dest_offset(const TensixState *p_tensix, const TensixConfigState *p_config, uint32_t pipe) {
    return p_tensix->thread[pipe].DEST_TARGET_REG_CFG_MATH_Offset;
}

static inline uint32_t math_fidelity_base(const TensixState *p_tensix, uint32_t pipe) {
    return p_tensix->thread[pipe].FIDELITY_BASE_Phase;
}

static inline uint32_t math_srca_cfg_format(const TensixConfigState *p_config) {
    return p_config->ALU_FORMAT_SPEC_REG_SrcA_override ? p_config->ALU_FORMAT_SPEC_REG_SrcA_val : p_config->ALU_FORMAT_SPEC_REG0_SrcA;
}

static inline uint32_t math_srcb_cfg_format(const TensixConfigState *p_config) {
    return p_config->ALU_FORMAT_SPEC_REG_SrcB_override ? p_config->ALU_FORMAT_SPEC_REG_SrcB_val : p_config->ALU_FORMAT_SPEC_REG1_SrcB;
}

#if TT_ARCH_VERSION >= 1
static inline uint32_t math_disable_implied_srca_fmt(const TensixState *p_tensix, uint32_t pipe) {
    return p_tensix->thread[pipe].DISABLE_IMPLIED_SRCA_FMT_Base;
}

static inline uint32_t math_disable_implied_srcb_fmt(const TensixState *p_tensix, uint32_t pipe) {
    return p_tensix->thread[pipe].DISABLE_IMPLIED_SRCB_FMT_Base;
}
#endif

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

static inline uint32_t math_src_rwc(uint32_t row) {
    return row; // SRC_RWC_ROWS is equal to SRC_ROWS
}

// The Dest counter's part of a Dest row address
static inline uint32_t math_dst_rwc(const TensixState *p_tensix, uint32_t pipe) {
#if TT_ARCH_VERSION == 0
    return p_tensix->dst_rwc[pipe] & (DST_ROWS - 1);
#else
    return p_tensix->dst_rwc[pipe]; // DST_RWC_ROWS is equal to DST_ROWS
#endif
}

static uint32_t math_srca_format(const TensixState *p_tensix, const TensixConfigState *p_config,
                                 uint32_t pipe, uint32_t bank) {
#if TT_ARCH_VERSION >= 1
    if (!math_disable_implied_srca_fmt(p_tensix, pipe)) {
        return p_tensix->src_a_format[bank];
    }
#endif
    return math_srca_cfg_format(p_config);
}

static uint32_t math_srcb_format(const TensixState *p_tensix, const TensixConfigState *p_config,
                                 uint32_t pipe, uint32_t bank) {
#if TT_ARCH_VERSION >= 1
    if (!math_disable_implied_srcb_fmt(p_tensix, pipe)) {
        return p_tensix->src_b_format[bank];
    }
#endif
    return math_srcb_cfg_format(p_config);
}

// note addr_mode is 2 bits in WH and 3 bits in BH; accessing higher address modes requires bias on WH
// XXX switch statement is clunky and perhaps slow; would be better if these were stored as an array
// could hack this by hardcoding the fact that bit positions are all the same?
static void math_update_counters(TensixState *p_tensix, uint32_t pipe, uint32_t addr_mode, bool update_fidelity_phase) {
    uint32_t src_a_incr, src_a_cr, src_a_clear;
    uint32_t src_b_incr, src_b_cr, src_b_clear;
    uint32_t dst_incr, dst_cr, dst_clear, dst_c_to_cr;
    uint32_t fidelity_incr, fidelity_clear;
#if TT_ARCH_VERSION == 0
    TTSIM_ASSERT(addr_mode < 4);
    if (p_tensix->bias[pipe] || p_tensix->thread[pipe].ADDR_MOD_SET_Base) { // note that ADDR_MOD_SET_Base was removed in BH
        addr_mode += 4;
    }
#else
    TTSIM_VERIFY(!p_tensix->bias[pipe], UnsupportedFunctionality, "bias=%d", p_tensix->bias[pipe]);
#endif
    uint32_t bias_incr, bias_clear;
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
            TTSIM_UNREACHABLE("addr_mode=%d", addr_mode);
    }
    math_update_rwc(&p_tensix->src_a_rwc[pipe], &p_tensix->src_a_rwc_cr[pipe], src_a_incr, src_a_clear, src_a_cr, 0, SRC_RWC_ROWS);
    math_update_rwc(&p_tensix->src_b_rwc[pipe], &p_tensix->src_b_rwc_cr[pipe], src_b_incr, src_b_clear, src_b_cr, 0, SRC_RWC_ROWS);
    math_update_rwc(&p_tensix->dst_rwc[pipe], &p_tensix->dst_rwc_cr[pipe], dst_incr, dst_clear, dst_cr, dst_c_to_cr, DST_RWC_ROWS);

    if (update_fidelity_phase) {
        if (fidelity_clear) {
            p_tensix->fidelity[pipe] = 0;
        } else {
            p_tensix->fidelity[pipe] = (p_tensix->fidelity[pipe] + fidelity_incr) & 3;
        }
    }

    TTSIM_VERIFY(!bias_clear, UnsupportedFunctionality, "bias_clear=%d", bias_clear);
    TTSIM_VERIFY(bias_incr <= 1, UnsupportedFunctionality, "bias_incr=%d", bias_incr);
    if (bias_incr) {
        p_tensix->bias[pipe] ^= 1;
    }
}

// A move into a Src bank (MOVD2A, MOVD2B, MOVB2A) doesn't wait for the Matrix Unit to have the bank, and an unpacker
// writing the same bank before handing it over races it. Until an instruction of the Matrix Unit's has waited for the
// bank, a move is fine only while no unpacker write comes before the handover, earlier or later.
static inline void math_src_waited(TensixState *p_tensix, bool is_src_b) {
    p_tensix->src_waited[is_src_b] |= 1 << (is_src_b ? p_tensix->src_b_matrix_bank : p_tensix->src_a_matrix_bank);
}

static inline void math_src_released(TensixState *p_tensix, bool is_src_b, uint32_t bank) {
    p_tensix->src_waited[is_src_b] &= ~(1 << bank);
    for (uint32_t pipe = 0; pipe < TENSIX_INST_PIPES; pipe++) {
        p_tensix->src_unpacked[is_src_b][pipe] &= ~(1 << bank);
    }
    p_tensix->src_moved[is_src_b] &= ~(1 << bank);
}

static inline void math_src_move(TensixState *p_tensix, bool is_src_b) {
    uint32_t bank = is_src_b ? p_tensix->src_b_matrix_bank : p_tensix->src_a_matrix_bank;
    if (!(p_tensix->src_waited[is_src_b] & (1 << bank))) {
        uint32_t unpacked = 0;
        for (uint32_t pipe = 0; pipe < TENSIX_INST_PIPES; pipe++) {
            unpacked |= p_tensix->src_unpacked[is_src_b][pipe];
        }
        TTSIM_VERIFY(!(unpacked & (1 << bank)), UndefinedBehavior,
            "move into Src%c bank %d before waiting for it, which an unpacker wrote", is_src_b ? 'B' : 'A', bank);
        p_tensix->src_moved[is_src_b] |= 1 << bank;
    }
}

static inline void unpack_src_written(TensixState *p_tensix, uint32_t pipe, bool is_src_b, uint32_t bank) {
    TTSIM_VERIFY(!(p_tensix->src_moved[is_src_b] & (1 << bank)), UndefinedBehavior,
        "unpack into Src%c bank %d, which a move wrote before the Matrix Unit waited for it", is_src_b ? 'B' : 'A', bank);
    p_tensix->src_unpacked[is_src_b][pipe] |= 1 << bank;
}

static void math_clear_src_valid(TensixState *p_tensix, uint32_t pipe, uint32_t clear_dvalid) {
    if (clear_dvalid & 1) {
        bool clear_a = !p_tensix->thread[pipe].CLR_DVALID_SrcA_Disable;
        if (clear_a) {
            TTSIM_VERIFY(p_tensix->src_a_valid & (1 << p_tensix->src_a_matrix_bank), NonContractualBehavior, "SrcA bank is not valid");
            p_tensix->src_a_valid &= ~(1 << p_tensix->src_a_matrix_bank);
        }
        math_src_released(p_tensix, false, p_tensix->src_a_matrix_bank);
        p_tensix->src_a_matrix_bank ^= 1;
    }
    if (clear_dvalid & 2) {
        bool clear_b = !p_tensix->thread[pipe].CLR_DVALID_SrcB_Disable;
        if (clear_b) {
            TTSIM_VERIFY(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank), NonContractualBehavior, "SrcB bank is not valid");
            p_tensix->src_b_valid &= ~(1 << p_tensix->src_b_matrix_bank);
        }
        math_src_released(p_tensix, true, p_tensix->src_b_matrix_bank);
        p_tensix->src_b_matrix_bank ^= 1;
    }
}

// XXX handle Adj16/Adj32 on BH
static inline uint32_t dst32b_adjust_row(uint32_t row) {
    return ((row & 0x1F8) << 1) | (row & 0x207);
}

static constexpr uint32_t DST32B_LO_ROW_OFFSET = 8;

#if TT_ARCH_VERSION == 1
// DEST_ACCESS_CFG (BH) changes where Dest datums are stored. remap_addrs rotates bits 3-5 of the row address of the
// access's width (16-bit rows for 16-bit accesses, 32-bit rows for 32-bit ones); swizzle_32b rearranges 32-bit
// datums across the two halves of each 16-bit row group. Both apply to every client that reads or writes datums
// (FPU, SFPU, unpack to Dest, pack); the debug array read sees the storage as it is. dst[][] holds the storage as a
// 16-bit access with both off sees it; these give the stored row for an access's row.
static inline uint32_t dst_remap_row(uint32_t row) {
    return (row & ~0x38u) | (((row >> 4) & 1) << 3) | (((row >> 5) & 1) << 4) | (((row >> 3) & 1) << 5);
}

static inline uint32_t dst_stored_row16(const TensixState *p_tensix, uint32_t row) {
    return p_tensix->DEST_ACCESS_CFG_remap_addrs ? dst_remap_row(row) : row;
}

// The stored row of a 32-bit datum's high half; its low half is DST32B_LO_ROW_OFFSET rows further on
static inline uint32_t dst_stored_row32(const TensixState *p_tensix, uint32_t row) {
    if (p_tensix->DEST_ACCESS_CFG_remap_addrs) {
        row = dst_remap_row(row);
    }
    if (p_tensix->DEST_ACCESS_CFG_swizzle_32b) { // bit 9 passes through, as in dst32b_adjust_row
        return (row & 0x203) | (((row >> 3) & 1) << 2) | ((((row >> 2) ^ (row >> 4)) & 1) << 4) |
               (((row & 0x1FF) >> 4) << 5);
    }
    return dst32b_adjust_row(row);
}
#endif

// The row of the valid flag an access's row uses. On BH remap_addrs moves it with the datum; swizzle_32b moves only the
// datum.
static inline uint32_t dst_valid_row(const TensixState *p_tensix, uint32_t row, bool use_dst32b) {
#if TT_ARCH_VERSION == 1
    row = dst_stored_row16(p_tensix, row);
#endif
    return use_dst32b ? dst32b_adjust_row(row) : row;
}

// Where an access row's datums are stored (a 32-bit datum's high half; its low half is DST32B_LO_ROW_OFFSET rows
// further on) and the row of the valid flag it uses
struct DstRowRef {
    uint32_t stored;
    uint32_t valid;
};

static inline DstRowRef dst_row_ref(const TensixState *p_tensix, uint32_t row, bool is_32b) {
#if TT_ARCH_VERSION == 1
    if (is_32b) {
        return {dst_stored_row32(p_tensix, row), dst_valid_row(p_tensix, row, true)};
    }
    row = dst_stored_row16(p_tensix, row); // remap_addrs moves the datum and its valid flag
#else
    row = is_32b ? dst32b_adjust_row(row) : row;
#endif
    return {row, row};
}

template<bool is_gmpool = false>
static inline uint32_t read_dst32b(TensixState *p_tensix, uint32_t row, uint32_t col) {
    DstRowRef r = dst_row_ref(p_tensix, row, true);
    if (!p_tensix->dst_row_valid[r.valid]) {
        return is_gmpool ? 0xFFFFFFFF : 0;
    }
    return (uint32_t(p_tensix->dst[r.stored][col]) << 16) | p_tensix->dst[r.stored + DST32B_LO_ROW_OFFSET][col];
}

template<bool set_valid_on_last_column_only = false>
static inline void write_dst32b(TensixState *p_tensix, uint32_t row, uint32_t col, uint32_t data) {
    DstRowRef r = dst_row_ref(p_tensix, row, true);
    p_tensix->dst[r.stored][col] = data >> 16;
    p_tensix->dst[r.stored + DST32B_LO_ROW_OFFSET][col] = data & 0xFFFF;
    if (!set_valid_on_last_column_only || (col == 15)) {
        p_tensix->dst_row_valid[r.valid] = true;
    }
}

// A move's dest_32b_lo write: the cell's low half, its high half left as stored even in a row never written, and the
// valid flag of the row at the Dest width the configuration selects, a 16-bit row's with a 16-bit Dest
static inline void write_dst32b_lo(TensixState *p_tensix, uint32_t row, uint32_t col, uint16_t data, bool cfg_dst32b) {
    p_tensix->dst[dst_row_ref(p_tensix, row, true).stored + DST32B_LO_ROW_OFFSET][col] = data;
    p_tensix->dst_row_valid[dst_row_ref(p_tensix, row, cfg_dst32b).valid] = true;
}

template<bool is_gmpool = false>
static inline uint16_t read_dst16b(TensixState *p_tensix, uint32_t row, uint32_t col) {
    DstRowRef r = dst_row_ref(p_tensix, row, false);
    if (!p_tensix->dst_row_valid[r.valid]) {
        return is_gmpool ? 0xFFFF : 0;
    }
    return p_tensix->dst[r.stored][col];
}

template<bool set_valid_on_last_column_only = false>
static inline void write_dst16b(TensixState *p_tensix, uint32_t row, uint32_t col, uint16_t data) {
    DstRowRef r = dst_row_ref(p_tensix, row, false);
    p_tensix->dst[r.stored][col] = data;
    if (!set_valid_on_last_column_only || (col == 15)) {
        p_tensix->dst_row_valid[r.valid] = true;
    }
}

#define SFPU_CELL_DEST 2

static inline uint16_t sfpu_read_cell16(TensixState *p_tensix, uint32_t bank, uint32_t row, uint32_t col) {
    TTSIM_ASSERT(bank == SFPU_CELL_DEST);
    return read_dst16b(p_tensix, row, col);
}

static inline uint32_t sfpu_read_cell32(TensixState *p_tensix, uint32_t bank, uint32_t row, uint32_t col) {
    TTSIM_ASSERT(bank == SFPU_CELL_DEST);
    return read_dst32b(p_tensix, row, col);
}

static inline void sfpu_write_cell16(TensixState *p_tensix, uint32_t bank, uint32_t row, uint32_t col, uint16_t data) {
    TTSIM_ASSERT(bank == SFPU_CELL_DEST);
    write_dst16b(p_tensix, row, col, data);
}

static inline void sfpu_write_cell32(TensixState *p_tensix, uint32_t bank, uint32_t row, uint32_t col, uint32_t data) {
    TTSIM_ASSERT(bank == SFPU_CELL_DEST);
    write_dst32b(p_tensix, row, col, data);
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

#if TT_ARCH_VERSION >= 1
uint32_t tensix_dst_rd32(TensixState *p_tensix, uint32_t pipe, uint32_t offset) {
#if TT_ARCH_VERSION == 1
    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    TTSIM_VERIFY(pipe == 1, UnimplementedFunctionality, "pipe=%d", pipe);
    uint32_t fmt = p_config->RISC_DEST_ACCESS_CTRL_SEC1_fmt;
    bool unsigned_int = p_config->RISC_DEST_ACCESS_CTRL_SEC1_unsigned_int;
    TTSIM_VERIFY(!fmt || (fmt == 2) || (fmt == 3), UnimplementedFunctionality, "fmt=%d", fmt);
    TTSIM_VERIFY(!unsigned_int, UnimplementedFunctionality, "unsigned_int=%d", unsigned_int);

    TTSIM_ASSERT(!(offset & 3), "misaligned offset=0x%x", offset);
    uint32_t addr = offset >> 2;
    uint32_t row, col, value;
    if (fmt == 0) { // FP32
        row = addr / 16;
        col = addr % 16;
        TTSIM_ASSERT(row < DST_ROWS / 2, "row=%d", row); // aperture is sized to exact size of Dst
        value = dst_decode_fp32(read_dst32b(p_tensix, row, col));
    } else {
        row = addr / 8;
        col = addr % 8;
        TTSIM_ASSERT(row < DST_ROWS, "row=%d", row); // aperture is sized to exact size of Dst
        if (fmt == 2) { // FP16
            value = dst_decode_fp16(read_dst16b(p_tensix, row, 2*col));
            value |= dst_decode_fp16(read_dst16b(p_tensix, row, 2*col + 1)) << 16;
        } else { // BF16
            value = dst_decode_bf16(read_dst16b(p_tensix, row, 2*col));
            value |= dst_decode_bf16(read_dst16b(p_tensix, row, 2*col + 1)) << 16;
        }
    }
    return value;
#else
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#endif
}
#endif

#if TT_ARCH_VERSION == 0
static inline uint32_t src_format_mask(uint32_t value, uint32_t fmt) {
    if ((fmt == 1) || (fmt == 2) || (fmt == 10) || (fmt == 14)) { // fp16, bfp8a, fp8, int8
        value &= 0x8FFFE000; // drop high 3 exp bits
    } else if ((fmt == 3) || (fmt == 11)) { // bfp4a, bfp2a
        value &= 0x8FFF0000; // drop high 3 exp bits and low 3 man bits
    } else if (fmt != 4) { // anything but tf32
        value &= 0xFFFF0000; // drop low 3 man bits
    }
    return value;
}
#endif

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

TENSIX_EXECUTE_MOVD2A() {
    TTSIM_VERIFY(instr_mod == 2, UnsupportedFunctionality, "instr_mod=%d", instr_mod);
    uint32_t n_rows = 4;
    TTSIM_VERIFY(!(dst & (n_rows - 1)), UnsupportedFunctionality, "dst=%d", dst);
    TTSIM_VERIFY(!(src & (n_rows - 1)), UnsupportedFunctionality, "src=%d", src);
    math_src_move(p_tensix, false);
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // Note: FP16A_FORCE_Enable (thread_cfg56/55) is not instantiated and errors on write; always 0
    uint32_t src_a_fmt = math_srca_format(p_tensix, p_config, pipe, src_a_bank);
    TTSIM_VERIFY((src_a_fmt != 12) && (src_a_fmt != 13), UndefinedBehavior, "src_a_fmt=%d", src_a_fmt);
    TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 1) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) ||
                 (src_a_fmt == 7), // fp32, fp16, tf32, bf16, bfp8, bfp4
        UnsupportedFunctionality, "src_a_fmt=%d", src_a_fmt);
    bool use_dst32b = p_config->ALU_ACC_CTRL_Fp32_enabled || p_config->ALU_ACC_CTRL_INT8_math_enabled;

    uint32_t src_a_row = src + p_tensix->src_a_rwc[pipe];
    uint32_t dst_row = math_dst_rwc(p_tensix, pipe) + dst + math_dest_offset(p_tensix, p_config, pipe);
    // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
    src_a_row &= SRC_ROWS - 1;
    dst_row &= DST_ROWS - 1;
    TTSIM_VERIFY(!(src_a_row & (n_rows - 1)), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(dst_row & (n_rows - 1)), UnsupportedFunctionality, "dst_row=%d", dst_row);
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value;
            if (use_dst32b) {
                TTSIM_VERIFY(!dest_32b_lo, UnsupportedFunctionality, "use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
                TTSIM_VERIFY(src_a_fmt != 1, UnsupportedFunctionality, "use_dst32b fp16");
                uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                if (src_a_fmt != 4) {
                    value = uint32_t(dst_decode_bf16(dst_val >> 16)) << 16;
                } else {
                    TTSIM_VERIFY(!dest_32b_lo, UnsupportedFunctionality, "tf32 use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
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
                    value = uint32_t(dst_decode_bf16(value)) << 16;
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
    math_src_move(p_tensix, true);
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // Note: FP16A_FORCE_Enable (thread_cfg56/55) is not instantiated and errors on write; always 0
    uint32_t src_a_fmt = math_srca_cfg_format(p_config);
#if TT_ARCH_VERSION >= 1
    if (!math_disable_implied_srca_fmt(p_tensix, pipe)) {
        src_a_fmt = p_tensix->src_b_format[src_b_bank];
    }
#endif
    TTSIM_VERIFY((src_a_fmt != 12) && (src_a_fmt != 13), UndefinedBehavior, "src_a_fmt=%d", src_a_fmt);
    TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 1) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) ||
                 (src_a_fmt == 7) || (src_a_fmt == 8), // fp32, fp16, tf32, bf16, bfp8, bfp4, int32
        UnsupportedFunctionality, "src_a_fmt=%d", src_a_fmt);
    bool use_dst32b = p_config->ALU_ACC_CTRL_Fp32_enabled || p_config->ALU_ACC_CTRL_INT8_math_enabled;

    uint32_t src_b_row = src + p_tensix->src_b_rwc[pipe];
    uint32_t dst_row = math_dst_rwc(p_tensix, pipe) + dst + math_dest_offset(p_tensix, p_config, pipe);
    // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
    dst_row &= DST_ROWS - 1;
    uint32_t n_rows = (instr_mod == 2) ? 4 : 1;
    TTSIM_VERIFY(!(src_b_row & (n_rows - 1)) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    TTSIM_VERIFY(!(dst_row & (n_rows - 1)), UnsupportedFunctionality, "dst_row=%d", dst_row);
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value;
            if (use_dst32b) {
                TTSIM_VERIFY(src_a_fmt != 1, UnsupportedFunctionality, "use_dst32b fp16");
                uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                if (dest_32b_lo) {
                    dst_val = (dst_val << 16) | (dst_val & 0xFFFF);
                }
                if (src_a_fmt != 4) {
                    value = uint32_t(dst_decode_bf16(dst_val >> 16)) << 16;
                } else {
                    TTSIM_VERIFY(!dest_32b_lo, UnsupportedFunctionality, "tf32 use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
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
                    value = uint32_t(dst_decode_bf16(value)) << 16;
                }
            }
            p_tensix->src_b[src_b_bank][src_b_row + row][col] = value;
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    return true;
}

TENSIX_EXECUTE_MOVB2A() {
    // Note: HW erratum: a MOVB2A starting exactly three cycles after a Matrix Unit computation loses a Dest write
    // tag, after which later computations' Dest writes go to the wrong rows or are dropped; not modeled
    TTSIM_VERIFY(instr_mod == 2, UnsupportedFunctionality, "instr_mod=%d", instr_mod);
    uint32_t n_rows = 4;
    TTSIM_VERIFY(!(srcb & (n_rows - 1)), UnsupportedFunctionality, "srcb=%d", srcb);
    TTSIM_VERIFY(!(srca & (n_rows - 1)), UnsupportedFunctionality, "srca=%d", srca);
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until SrcB valid
    }
    math_src_waited(p_tensix, true);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    math_src_move(p_tensix, false);
#if TT_ARCH_VERSION == 0
    uint32_t src_b_fmt = math_srcb_format(p_tensix, p_config, pipe, src_b_bank);
    TTSIM_VERIFY((src_b_fmt != 12) && (src_b_fmt != 13), UndefinedBehavior, "src_b_fmt=%d", src_b_fmt);
#endif
    bool flush_denormals = !p_config->ALU_ACC_CTRL_Zero_Flag_disabled_src;

    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    uint32_t src_a_row = srca + p_tensix->src_a_rwc[pipe];
    uint32_t src_b_row = srcb + p_tensix->src_b_rwc[pipe];
    TTSIM_VERIFY(!(src_a_row & (n_rows - 1)) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(src_b_row & (n_rows - 1)) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value = p_tensix->src_b[src_b_bank][src_b_row + row][col];
            if (flush_denormals && !(value & 0x7F800000)) {
                value = 0;
            }
#if TT_ARCH_VERSION == 0
            value = src_format_mask(value, src_b_fmt);
#endif
            p_tensix->src_a[src_a_bank][src_a_row + row][col] = value;
        }
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    return true;
}

TENSIX_EXECUTE_ZEROACC() {
#if TT_ARCH_VERSION >= 1
    uint32_t dst = where; // this field was renamed
    clear_mode |= use_32_bit_mode << 2; // remap to be equivalent to WH fields
#endif
#if TT_ARCH_VERSION == 1
    if (clear_zero_flags) { // NonContractualBehavior() in spec, only supported in very limited cases for a HW bug workaround
        TTSIM_VERIFY(clear_mode == 5, NonContractualBehavior, "clear_zero_flags=%d is dangerous and should not be used (clear_mode=%d)", clear_zero_flags, clear_mode);
    }
#endif

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
    uint32_t dst_offset = math_dest_offset(p_tensix, p_config, pipe) + math_dst_rwc(p_tensix, pipe);
    switch (clear_mode) {
        case 0: // clear 1 row
            dst += dst_offset;
            TTSIM_VERIFY(dst < DST_ROWS, UnsupportedFunctionality, "clear 1 row: dst=%d", dst);
            TTSIM_VERIFY(!p_config->ALU_ACC_CTRL_INT8_math_enabled, UnsupportedFunctionality, "ALU_ACC_CTRL_INT8_math_enabled");
#if TT_ARCH_VERSION == 0
            if (p_config->ALU_ACC_CTRL_Fp32_enabled) {
                uint32_t row = dst32b_adjust_row(dst);
                p_tensix->dst_row_valid[row] = false;
                // HW erratum: the 16-bit row numbered dst is marked unwritten too when it is in another quarter of Dest
                if ((row ^ dst) & (DST_ROWS / 4 * 3)) {
                    p_tensix->dst_row_valid[dst] = false;
                }
            } else {
                p_tensix->dst_row_valid[dst] = false;
            }
#else
            // the width is use_32_bit_mode's (clear_mode 4), not the accumulator's
            p_tensix->dst_row_valid[dst_valid_row(p_tensix, dst, false)] = false;
#endif
            break;
        case 1: // clear 16 rows
#if TT_ARCH_VERSION == 1
            if ((dst + dst_offset) & 512) { // dst counts as a row when picking the half
                dst += 32;
            }
#endif
            TTSIM_VERIFY(dst < DST_ROWS / 16, NonContractualBehavior, "clear 16 rows: dst=%d", dst);
            for (uint32_t row = 0; row < 16; row++) {
                p_tensix->dst_row_valid[dst_valid_row(p_tensix, dst * 16 + row, false)] = false;
            }
            break;
#if TT_ARCH_VERSION == 1
        case 5: // clear 32b 16 rows -- XXX only supported in very limited cases for a HW bug workaround
            if ((dst + dst_offset) & 768) {
                dst += 16;
            }
            TTSIM_VERIFY(dst < DST_ROWS / 32, NonContractualBehavior, "clear 32b 16 rows: dst=%d", dst);
            if (clear_zero_flags) { // validate that the HW bug workaround is not being used in a dangerous way
                for (uint32_t row = 0; row < 16; row++) {
                    // would have liked to enforce that it's already true, but this is not realistic in all sub-tile cases
                    p_tensix->dst_row_valid[dst_valid_row(p_tensix, dst * 16 + row, true)] = true;
                }
            } else {
                for (uint32_t row = 0; row < 16; row++) {
                    p_tensix->dst_row_valid[dst_valid_row(p_tensix, dst * 16 + row, true)] = false;
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

TENSIX_EXECUTE_ZEROSRC() {
    TTSIM_VERIFY(src_mask, UnsupportedFunctionality, "no-op mask: src_mask=%d", src_mask);
    TTSIM_VERIFY(!zero_val, UnsupportedFunctionality, "zero_val=%d", zero_val);

    // Note: ZEROSRC does not wait for an UNPACR still writing a bank it clears; on WH some of that UNPACR's datums can
    // land after the clear unless a STALLWAIT on the unpackers comes first; not modeled (timing)
    if (bank_mask == 1) {
        TTSIM_VERIFY(!write_mode, UnsupportedFunctionality, "write_mode=%d", write_mode);
        TTSIM_VERIFY(src_mask == 3, UnsupportedFunctionality, "src_mask=%d", src_mask);
        if (src_mask & 1) {
            memset(p_tensix->src_a, 0, sizeof(p_tensix->src_a));
        }
        if (src_mask & 2) {
            memset(p_tensix->src_b, 0, sizeof(p_tensix->src_b));
        }
    } else {
        TTSIM_VERIFY(!bank_mask, UnsupportedFunctionality, "bank_mask=%d", bank_mask);
        TTSIM_VERIFY(write_mode == 1, UnsupportedFunctionality, "write_mode=%d", write_mode);
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

// Not checked: on Wormhole, a MOVA2D or MOVB2D issued while the thread's earlier Matrix Unit instructions are still
// running starts late, and neither SFPLOAD nor PACR's Dest interlock waits for it, so either can read the old data
// unless a STALLWAIT on C7 separates them. ttsim moves at once.
TENSIX_EXECUTE_MOVA2D() {
    TTSIM_VERIFY(instr_mod == 2, UnsupportedFunctionality, "instr_mod=%d", instr_mod); // move 8 rows
    TTSIM_VERIFY(!(dst & 7), UnsupportedFunctionality, "dst=%d", dst);
    TTSIM_VERIFY(!(src & 7), UnsupportedFunctionality, "src=%d", src);
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    if (!(p_tensix->src_a_valid & (1 << src_a_bank))) {
        return false; // stall until SrcA valid
    }
    math_src_waited(p_tensix, false);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // Note: FP16A_FORCE_Enable (thread_cfg56/55) is not instantiated and errors on write; always 0
    uint32_t src_a_fmt = math_srca_format(p_tensix, p_config, pipe, src_a_bank);
    TTSIM_VERIFY((src_a_fmt != 12) && (src_a_fmt != 13), UndefinedBehavior, "src_a_fmt=%d", src_a_fmt);
    if ((TT_ARCH_VERSION == 1) && p_config->ALU_ACC_CTRL_Fp32_enabled && !dest_32b_lo) {
        src_a_fmt = 4;
    }
    bool use_8b_exponent;
    if ((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7) || // fp32, tf32, bf16, bfp8, bfp4
        (src_a_fmt == 8) || (src_a_fmt == 9) || (src_a_fmt == 15)) { // int32, int16, bfp2
        use_8b_exponent = true;
    } else { // fp16
        TTSIM_VERIFY((src_a_fmt == 1) || (src_a_fmt == 10), UnsupportedFunctionality, "src_a_fmt=%d", src_a_fmt); // fp16, fp8
        use_8b_exponent = false;
    }
    bool use_dst32b = (src_a_fmt == 4);
    // XXX tt-llk's BH 32-bit transpose_dest and broadcast datacopy still write low halves into a 16-bit Dest
    TTSIM_VERIFY(((use_dst32b || dest_32b_lo) == (p_config->ALU_ACC_CTRL_Fp32_enabled || p_config->ALU_ACC_CTRL_INT8_math_enabled)) ||
                 (dest_32b_lo && !use_dst32b),
        UndefinedBehavior, "Dest width: use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
    bool flush_denormals = !p_config->ALU_ACC_CTRL_Zero_Flag_disabled_src;

    uint32_t src_a_row = src + p_tensix->src_a_rwc[pipe];
    uint32_t dst_row = math_dst_rwc(p_tensix, pipe) + dst + math_dest_offset(p_tensix, p_config, pipe);
    // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
    dst_row &= DST_ROWS - 1;
    TTSIM_VERIFY(!(src_a_row & 7) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(dst_row & 7), UnsupportedFunctionality, "dst_row=%d", dst_row);
    for (uint32_t row = 0; row < 8; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value = p_tensix->src_a[src_a_bank][src_a_row + row][col];
            if (flush_denormals && !(value & 0x7F800000)) {
                value = 0;
            }
            if (use_dst32b) {
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
                    write_dst32b_lo(p_tensix, dst_row + row, col, uint16_t(value),
                        p_config->ALU_ACC_CTRL_Fp32_enabled || p_config->ALU_ACC_CTRL_INT8_math_enabled);
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
    TTSIM_VERIFY((instr_mod == 0) || (instr_mod == 2) || (instr_mod == 3) || (instr_mod == 4) || (instr_mod == 5),
        UnsupportedFunctionality, "instr_mod=%d", instr_mod);
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until SrcB valid
    }
    math_src_waited(p_tensix, true);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // Note: FP16A_FORCE_Enable (thread_cfg56/55) is not instantiated and errors on write; always 0
    uint32_t src_a_fmt = math_srca_cfg_format(p_config);
#if TT_ARCH_VERSION == 0
    uint32_t src_b_fmt = math_srcb_format(p_tensix, p_config, pipe, src_b_bank);
    TTSIM_VERIFY((src_b_fmt != 12) && (src_b_fmt != 13), UndefinedBehavior, "src_b_fmt=%d", src_b_fmt);
#endif
#if TT_ARCH_VERSION >= 1
    if (!math_disable_implied_srca_fmt(p_tensix, pipe)) {
        src_a_fmt = p_tensix->src_b_format[src_b_bank];
    }
#endif
    TTSIM_VERIFY((src_a_fmt != 12) && (src_a_fmt != 13), UndefinedBehavior, "src_a_fmt=%d", src_a_fmt);
    if ((TT_ARCH_VERSION == 1) && p_config->ALU_ACC_CTRL_Fp32_enabled) {
        src_a_fmt = 4;
    }
    bool use_8b_exponent;
    if ((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7) || // fp32, tf32, bf16, bfp8, bfp4
        (src_a_fmt == 8) || (src_a_fmt == 9)) { // int32, int16
        use_8b_exponent = true;
    } else { // fp16
        TTSIM_VERIFY(src_a_fmt == 1, UnsupportedFunctionality, "src_a_fmt=%d", src_a_fmt); // fp16
        use_8b_exponent = false;
    }
    bool use_dst32b = (src_a_fmt == 4);
    // XXX tt-llk's BH 32-bit transpose_dest and broadcast datacopy still write low halves into a 16-bit Dest
    TTSIM_VERIFY(((use_dst32b || dest_32b_lo) == (p_config->ALU_ACC_CTRL_Fp32_enabled || p_config->ALU_ACC_CTRL_INT8_math_enabled)) ||
                 (dest_32b_lo && !use_dst32b),
        UndefinedBehavior, "Dest width: use_dst32b=%d dest_32b_lo=%d", use_dst32b, dest_32b_lo);
    bool flush_denormals = !p_config->ALU_ACC_CTRL_Zero_Flag_disabled_src;

    uint32_t src_b_row = src + p_tensix->src_b_rwc[pipe];
    uint32_t dst_row = math_dst_rwc(p_tensix, pipe) + dst + math_dest_offset(p_tensix, p_config, pipe);
    // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
    dst_row &= DST_ROWS - 1;
    uint32_t n_rows = (instr_mod & 2) ? 8 : (instr_mod & 4) ? 4 : 1;
    TTSIM_VERIFY(!(src_b_row & (n_rows - 1)) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    TTSIM_VERIFY(!(dst_row & (n_rows - 1)), UnsupportedFunctionality, "dst_row=%d", dst_row);
    for (uint32_t row = 0; row < n_rows; row++) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value = p_tensix->src_b[src_b_bank][src_b_row][(instr_mod & 1) ? 0 : col];
            if (flush_denormals && !(value & 0x7F800000)) {
                value = 0;
            }
#if TT_ARCH_VERSION == 0
            value = src_format_mask(value, src_b_fmt);
#endif
            if (use_dst32b) {
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
                    write_dst32b_lo(p_tensix, dst_row + row, col, uint16_t(value),
                        p_config->ALU_ACC_CTRL_Fp32_enabled || p_config->ALU_ACC_CTRL_INT8_math_enabled);
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
    math_src_waited(p_tensix, true);

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

// The Matrix Unit's math takes a row of Dest columns at once, a vector of FPU_LANES 32-bit lanes at a time (simd.h)
static constexpr uint32_t FPU_LANES = sizeof(vec_u32) / sizeof(uint32_t);

// A Src datum as one fidelity phase's multipliers take it, decoded once for every product it is in: its sign, its
// exponent (SrcB's with the product's rebias added), and the phase's slice of its mantissa. A zero datum has a zero
// mantissa and the exponent FPU_ZERO_EXP, so that every product it is in sums to below FPU_ZERO_PRODUCT, and every
// other product (exponents 1 to 255 each, rebiased by less than 200) to above it. U and I are lanes of SrcA columns,
// unsigned and signed, or plain integers for a SrcB datum.
static const int32_t FPU_ZERO_EXP = -4096;
static const int32_t FPU_ZERO_PRODUCT = -2048;

template<class U, class I>
struct fpu_operand {
    U value; // the datum as Src holds it
    U sign;
    I exp;
    U man;
};

// A SrcB row's products with a row of SrcA columns, summed in two halves of eight: each half's products aligned to its
// largest exponent, rounded there, and summed with their signs (a two's-complement sum in man_sops). A product with a
// zero operand has a zero mantissa and the exponent zero_exp.
template<class I, class U, class A, class B>
static inline void fpu_sum_halves(const A *a, const B *b, I zero_exp, I *exp_sops, U *man_sops) {
    I exps[16];
    U mans[16];
    U signs[16];
    for (uint32_t i = 0; i < 16; i++) {
        I exp = a[i].exp + b[i].exp;
        exps[i] = (exp < FPU_ZERO_PRODUCT) ? zero_exp : exp;
        mans[i] = a[i].man * b[i].man;
        signs[i] = a[i].sign ^ b[i].sign; // a zero product's sign doesn't matter, its mantissa is zero
    }
    for (uint32_t group = 0; group < 2; group++) {
        const I *e = exps + 8 * group;
        I exp_sop = vmax(vmax(vmax(e[0], e[1]), vmax(e[2], e[3])), vmax(vmax(e[4], e[5]), vmax(e[6], e[7])));
        U man_sop = {};
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t lane = 8 * group + i;
            U exp_diff = U(exp_sop - exps[lane]);
            exp_diff = (exp_diff < 30) ? exp_diff : 30; // clamp to avoid UB on shift -- result will always be zero
            U man = ((mans[lane] << 1) + ((U{} + 1) << exp_diff)) >> (exp_diff + 1);
            man_sop = (signs[lane] != 0) ? man_sop - man : man_sop + man;
        }
        exp_sops[group] = exp_sop;
        man_sops[group] = man_sop;
    }
}

typedef fpu_operand<vec_u32, vec_i32> fpu_operand_lanes;
typedef fpu_operand<uint32_t, int32_t> fpu_operand_one;

// The two terms the normalizer adds to Dest in each lane: a SrcB row's products with a row of SrcA columns, summed in
// two halves, or an elementwise op's one term and an empty one
struct fpu_sops {
    vec_u32 sign[2];
    vec_i32 exp[2];
    vec_u32 man[2];
};

// man shifted right by exp_diff and rounded to a multiple of 1 << lsb, to nearest with ties away from zero (towards
// zero for a negative term)
template<class U>
static inline U fpu_align_term(U man, U sign, U exp_diff, uint32_t lsb) {
    const U one = U{} + 1;
    U shift = exp_diff + lsb;
    auto in_range = (shift - 1) < 30; // 1 to 30; the other lanes take a shift of 1, then aren't used
    U s = in_range ? shift : one;
    U rounded = ((man + (one << (s - 1)) - sign) >> s) << lsb;
    return (shift == 0) ? man : (in_range ? rounded : U{});
}

// Normalize sop and dst terms to a common exponent, then add, renormalize, and encode: U and I are unsigned and signed
// 32-bit integers, or lanes of them
template<class U, class I>
static U fpu_accum_normalize_encode(U sign_sop0, I exp_sop0, U man_sop0, U sign_sop1, I exp_sop1, U man_sop1,
    U dst_value, bool fp32_acc, bool fp_exponent_8b) {
    U sign_dst = dst_value >> 31;
    I exp_dst = I((dst_value >> 23) & 255);
    U man_dst = (exp_dst == 0) ? U{} : ((dst_value & 0x7FFFFF) | 0x800000); // 24 bits

    I exp = vmax(vmax(exp_sop0, exp_sop1), exp_dst);
    auto zero = exp <= 0; // early underflow case (before normalization/rounding)
    // Low 13 mantissa bits are rounded away unless fp32_acc, in the same step as the alignment: rounding at both
    // would round a term just below half an ulp up
    uint32_t lsb = fp32_acc ? 0 : 13;
    man_sop0 = fpu_align_term(man_sop0, sign_sop0, U(exp - exp_sop0), lsb);
    man_sop1 = fpu_align_term(man_sop1, sign_sop1, U(exp - exp_sop1), lsb);
    man_dst = fpu_align_term(man_dst, U{}, U(exp - exp_dst), lsb);

    U sign = sign_dst;
    U man = man_dst;
    man = (sign_sop0 == sign) ? man + man_sop0 : man - man_sop0;
    man = (sign_sop1 == sign) ? man + man_sop1 : man - man_sop1;
    auto negative = I(man) < 0;
    man = negative ? -man : man;
    sign = negative ? sign ^ 1 : sign;
    zero |= man == 0; // exact cancellation

    I shift = I(vclz<32>(man)) - 8; // amount to shift left to normalize to 24 bits
#if TT_ARCH_VERSION == 0
    // mimic bug in renormalization of -1 (all bits set in leading sign count): correct shift is 23, handle as if -4
    shift = ((sign != 0) & (man == 1)) ? shift - 27 : shift;
#endif
    exp -= shift;
    if (!fp32_acc) {
        shift -= fp_exponent_8b ? 16 : 13;
    }
    auto right = shift < 0;
    U right_shift = U(right ? -shift : 1);
    U left_shift = U(right ? 0 : shift);
    man = right ? (man + ((U{} + 1) << (right_shift - 1))) >> right_shift : man << left_shift;
    if (!fp32_acc) {
        man <<= fp_exponent_8b ? 16 : 13;
    }
    exp = ((man & 0x1000000) != 0) ? exp + 1 : exp;
    man &= 0x7FFFFF;
    zero |= exp <= 0;
    U result = (sign << 31) | (U(exp) << 23) | man;
    if (fp32_acc || fp_exponent_8b) {
        result = (exp >= 255) ? (sign << 31) | (255 << 23) : result;
    } else {
        result = (exp >= 32) ? (sign << 31) | ((31 << 23) | (0x3FF << 13)) : result;
    }
    return zero ? U{} : result;
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

// The multipliers' input from an 11-bit Src mantissa in this fidelity phase: SrcA's five-bit slice by phase bit 0,
// SrcB's seven-bit one by bit 1
template<class T>
static inline T fpu_mul_man(T man, bool is_src_b, uint32_t fidelity_phase) {
    man |= 0x400;
    if (is_src_b) {
        return (fidelity_phase & 2) ? (man & 15) << 3 : man >> 4;
    }
    return (fidelity_phase & 1) ? (man >> 1) & 31 : man >> 6;
}

static fpu_sops mvmul_sops(const fpu_operand_lanes *a, const fpu_operand_one *b, bool specials, int32_t exp_prod_adj,
    uint32_t fidelity_phase, bool fp_exponent_8b) {
    fpu_sops s;
    vec_u32 man_sops[2];
    // a product with a zero operand takes this clamped exponent in the shared exponent
    fpu_sum_halves(a, b, vec_i32{} + std::max(exp_prod_adj, 0), s.exp, man_sops);
    for (uint32_t group = 0; group < 2; group++) {
        vec_i32 used = s.exp[group] > 0; // else the entire sop is zero
        vec_i32 negative = vec_i32(man_sops[group]) < 0;
        s.sign[group] = used ? man_sops[group] >> 31 : vec_u32{};
        s.man[group] = used ? (negative ? -man_sops[group] : man_sops[group]) << 13 : vec_u32{};
        s.exp[group] = used ? s.exp[group] : vec_i32{};
    }
    return s;
}

static vec_u32 fpu_accumulate(const fpu_sops &s, vec_u32 dst_value, bool fp32_acc, bool fp_exponent_8b) {
    return fpu_accum_normalize_encode(s.sign[0], s.exp[0], s.man[0], s.sign[1], s.exp[1], s.man[1], dst_value,
        fp32_acc, fp_exponent_8b);
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

// A Dest row as the FPU reads it, its 16-bit datums widened to the fp32 layout
static inline void fpu_dst_read_row(TensixState *p_tensix, uint32_t row, uint32_t *data, bool use_dst32b,
    bool fp_exponent_8b, uint32_t src_a_fmt) {
    DstRowRef r = dst_row_ref(p_tensix, row, use_dst32b);
    const uint16_t *hi = p_tensix->dst[r.stored];
    bool valid = p_tensix->dst_row_valid[r.valid];
    if (!valid) {
        memset(data, 0, ROW_SIZE * sizeof(data[0])); // an unwritten row reads as zeros
    } else if (use_dst32b) {
        const uint16_t *lo = p_tensix->dst[r.stored + DST32B_LO_ROW_OFFSET];
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            data[col] = dst_decode_fp32((uint32_t(hi[col]) << 16) | lo[col]);
        }
    } else {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value = fp_exponent_8b ? uint32_t(dst_decode_bf16(hi[col])) << 16 : dst_decode_fp16(hi[col]);
            data[col] = fp_exponent_8b ? value : ((value & 0x8000) << 16) | ((value & 0x7FFF) << 13);
        }
    }
}

// The row of results written back, which marks it valid
static inline void fpu_dst_write_row(TensixState *p_tensix, uint32_t row, const uint32_t *data, bool use_dst32b,
    bool fp_exponent_8b, uint32_t src_a_fmt) {
    DstRowRef r = dst_row_ref(p_tensix, row, use_dst32b);
    uint16_t *hi = p_tensix->dst[r.stored];
    if (use_dst32b) {
        uint16_t *lo = p_tensix->dst[r.stored + DST32B_LO_ROW_OFFSET];
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            hi[col] = dst_encode_fp32(data[col]) >> 16;
            lo[col] = data[col] & 0xFFFF;
        }
    } else {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value = data[col];
            hi[col] = fp_exponent_8b ? dst_encode_bf16(value >> 16)
                                     : dst_encode_fp16(((value & 0x80000000) >> 16) | ((value & 0x0FFFE000) >> 13));
        }
    }
    p_tensix->dst_row_valid[r.valid] = true;
}

// A Src value (shifted down to its 19 bits) as an fpu_operand; with 'flush' clear, an exponent of zero is not a zero
template<class Op>
static inline Op fpu_operand_of(decltype(Op::value) value, bool is_src_b, uint32_t fidelity_phase, int32_t exp_adj,
    bool flush = true) {
    decltype(Op::value) exp = (value >> 10) & 255;
    decltype(Op::value) man = fpu_mul_man(value & 0x3FF, is_src_b, fidelity_phase);
    auto zero = flush ? exp == 0 : exp != exp;
    return {value, value >> 18, zero ? decltype(Op::exp){} + FPU_ZERO_EXP : decltype(Op::exp)(exp) + exp_adj,
            zero ? decltype(man){} : man};
}

// ELWMUL: each lane's one product, through the multipliers as an MVMUL's products are, the normalizer's second term
// empty
static fpu_sops elwmul_sops(const fpu_operand_lanes &a, const fpu_operand_lanes &b, int32_t exp_prod_adj,
    uint32_t fidelity_phase, bool fp_exponent_8b) {
    fpu_sops s = {};
    vec_i32 exp = a.exp + b.exp;
    vec_i32 zero = exp <= 0; // a product that underflows is dropped too
    vec_i32 zero_exp = vec_i32{} + std::max(exp_prod_adj, 0); // the clamped exponent it takes in the shared one
    uint32_t lsb = 13;
    s.sign[0] = zero ? 0 : a.sign ^ b.sign;
    s.exp[0] = zero ? zero_exp : exp;
    s.man[0] = zero ? 0 : (a.man * b.man) << lsb;
    return s;
}

// The bits an ELWADD addend keeps below its own mantissa's LSB when it is aligned to the other: the addends bypass
// the multipliers
static const uint32_t ELWADD_PAD_BITS = 0;

// ELWADD and ELWSUB (negate_b): each lane's sum of the two addends, the smaller aligned to the larger and rounded
// there, the normalizer's second term empty. zero_a and zero_b are the Src zero flags; zf_b is SrcB's stored zero flag.
static fpu_sops elwadd_sops(vec_u32 a_value, vec_u32 b_value, vec_i32 zero_a, vec_i32 zero_b, vec_i32 zf_b,
    bool negate_b, bool fp32_acc, bool fp_exponent_8b) {
    fpu_sops s = {};
    b_value ^= uint32_t(negate_b) << 18; // ELWSUB is a sign flip in the SrcB read mux
    a_value = zero_a ? 0 : a_value;
    b_value = zero_b ? 0 : b_value;
    vec_i32 swap = (a_value & 0x3FFFF) < (b_value & 0x3FFFF); // so that abs(a) >= abs(b)
    vec_u32 hi = swap ? b_value : a_value;
    vec_u32 lo = swap ? a_value : b_value;
    vec_i32 exp_hi = vec_i32((hi >> 10) & 255);
    vec_i32 exp_lo = vec_i32((lo >> 10) & 255);
    vec_u32 man_hi = (hi != 0) ? (hi & 0x3FF) | 0x400 : 0;
    vec_u32 man_lo = (lo != 0) ? (lo & 0x3FF) | 0x400 : 0;
    // Rounded half up, unless every bit is shifted out; the lanes that don't shift take a shift of 1, unused
    vec_u32 exp_diff = vec_u32(exp_hi - exp_lo);
    vec_i32 shifted = (exp_diff - 1) < 11 + ELWADD_PAD_BITS;
    vec_u32 shift = shifted ? exp_diff : 1;
    man_lo = shifted ? (man_lo + ((vec_u32{} + 1) << (shift - 1))) >> shift : (exp_diff == 0) ? man_lo : 0;
    vec_u32 man = ((hi >> 18) == (lo >> 18)) ? man_hi + man_lo : man_hi - man_lo;
    s.sign[0] = hi >> 18;
    s.man[0] = man << (13 - ELWADD_PAD_BITS); // a 25-bit sum
    if (fp32_acc && !fp_exponent_8b) {
        exp_hi = (exp_hi != 0) ? exp_hi + (127 - 15) : 127 - 30; // the clamped exponent it takes in the shared one
    }
    s.exp[0] = exp_hi;
    return s;
}

template<bool is_gapool, bool broadcast = false>
static bool tensix_matmul_op(TensixState *p_tensix, uint32_t pipe, uint32_t dst, uint32_t addr_mode, uint32_t clear_dvalid,
    uint32_t src_a_offset = 0, uint32_t src_b_offset = 0) {
    static_assert(!is_gapool || !broadcast);
    if (is_gapool) {
        TTSIM_VERIFY(!(dst & 3), UnsupportedFunctionality, "dst=%d", dst);
    } else if (!broadcast) {
        TTSIM_VERIFY(!(dst & 7), UnsupportedFunctionality, "dst=%d", dst);
    }
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_a_valid & (1 << src_a_bank)) || !(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until both valid
    }
    math_src_waited(p_tensix, false);
    math_src_waited(p_tensix, true);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // Note: FP16A_FORCE_Enable (thread_cfg56/55) is not instantiated and errors on write; always 0
    bool is_int8 = p_config->ALU_ACC_CTRL_INT8_math_enabled;
    uint32_t src_a_fmt = math_srca_format(p_tensix, p_config, pipe, src_a_bank);
    uint32_t src_b_fmt = math_srcb_format(p_tensix, p_config, pipe, src_b_bank);
    TTSIM_VERIFY((src_a_fmt != 12) && (src_a_fmt != 13), UndefinedBehavior, "src_a_fmt=%d", src_a_fmt);
    TTSIM_VERIFY((src_b_fmt != 12) && (src_b_fmt != 13), UndefinedBehavior, "src_b_fmt=%d", src_b_fmt);
    if (is_int8) {
        TTSIM_VERIFY(src_a_fmt == 14, UnsupportedFunctionality, "int8 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
        TTSIM_VERIFY(src_b_fmt == 14, UnsupportedFunctionality, "int8 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
    } else if ((src_a_fmt == 1) || (src_a_fmt == 10)) { // fp16/fp8
        TTSIM_VERIFY(src_a_fmt == src_b_fmt, UnsupportedFunctionality, "fp16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
    } else {
        TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7) || (src_a_fmt == 15),
            UnsupportedFunctionality, "bf16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4, bfp2
        TTSIM_VERIFY((src_b_fmt == 0) || (src_b_fmt == 4) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7) || (src_b_fmt == 9) || (src_b_fmt == 15),
            UnsupportedFunctionality, "bf16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4, int16, bfp2
    }
    bool use_dst32b = is_int8 || p_config->ALU_ACC_CTRL_Fp32_enabled;
    bool flush_denormals = !p_config->ALU_ACC_CTRL_Zero_Flag_disabled_src;
    TTSIM_VERIFY(flush_denormals, UnsupportedFunctionality, "ALU_ACC_CTRL_Zero_Flag_disabled_src=1 (keep SrcB denormals) not modeled");

    uint32_t src_a_row = math_src_rwc(p_tensix->src_a_rwc[pipe] + src_a_offset);
    uint32_t src_b_row = math_src_rwc(p_tensix->src_b_rwc[pipe] + src_b_offset);
    uint32_t dst_row = math_dst_rwc(p_tensix, pipe) + dst + math_dest_offset(p_tensix, p_config, pipe);
    // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
    dst_row &= DST_ROWS - 1;
    constexpr uint32_t N_ROWS = (is_gapool || broadcast) ? 4 : 8;
    constexpr uint32_t B_ROWS = broadcast ? 1 : N_ROWS;
    constexpr uint32_t DST_STRIDE = broadcast ? 2 : 1;
    TTSIM_VERIFY(!(src_a_row & 15) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    if (broadcast) {
        TTSIM_VERIFY(src_b_row < SRC_ROWS, UnsupportedFunctionality, "src_b_row=%d", src_b_row);
        TTSIM_VERIFY(!(dst_row & 6), UnsupportedFunctionality, "dst_row=%d", dst_row);
    } else {
        TTSIM_VERIFY(!(src_b_row & 7) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
        TTSIM_VERIFY(!(dst_row & (N_ROWS - 1)), UnsupportedFunctionality, "dst_row=%d", dst_row);
    }
    uint32_t unflagged_row = broadcast ? dst_valid_row(p_tensix, dst_row + 2, use_dst32b) : 0;
    bool unflagged_row_valid = p_tensix->dst_row_valid[unflagged_row];
    uint32_t fidelity_phase = (p_tensix->fidelity[pipe] + math_fidelity_base(p_tensix, pipe)) & 3;
    bool fp_exponent_8b = (src_a_fmt != 1) && (src_a_fmt != 10) && !is_int8;
    uint32_t high_exp_bits = 0x70000000;
    if (!fp_exponent_8b) {
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_a[src_a_bank][src_a_row + i][col];
                TTSIM_VERIFY(!(value & high_exp_bits), UnsupportedFunctionality, "SrcA 5b-exp datum=0x%x has nonzero high-3 exp bits", value);
            }
        }
        for (uint32_t row = 0; row < B_ROWS; row++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_b[src_b_bank][src_b_row + row][i];
                TTSIM_VERIFY(!(value & high_exp_bits), UnsupportedFunctionality, "SrcB 5b-exp datum=0x%x has nonzero high-3 exp bits", value);
            }
        }
    }
    uint32_t src_b[B_ROWS][16], src_a[ROW_SIZE][16];
    if (is_int8) {
        for (uint32_t row = 0; row < B_ROWS; row++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_b[src_b_bank][src_b_row + row][i];
                if (flush_denormals && !(value & 0x7F800000)) {
                    value = 0;
                }
                value &= (fidelity_phase & 2) ? 0x8001E000 : 0x807E0000;
                src_b[row][i] = read_src_int8(value);
            }
        }
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_a[src_a_bank][src_a_row + i][col];
                if (!(value & 0x7F800000)) {
                    value = 0;
                }
                value &= (fidelity_phase & 1) ? 0x8003E000 : 0x801C0000;
                src_a[col][i] = read_src_int8(value);
            }
        }
        for (uint32_t row = 0; row < N_ROWS; row++) {
            for (uint32_t col = 0; col < ROW_SIZE; col++) {
                uint32_t dst_val = read_dst32b(p_tensix, dst_row + DST_STRIDE * row, col);
                int32_t result = 0;
                for (uint32_t i = 0; i < 16; i++) {
                    result += int32_t(src_a[col][i]) * int32_t(src_b[broadcast ? 0 : row][i]); // cannot overflow, both inputs in [-1023,1023]
                }
                result = saturate_add_int32(dst_decode_int32(dst_val), result);
                write_dst32b<true>(p_tensix, dst_row + DST_STRIDE * row, col, dst_encode_int32(result));
            }
        }
    } else {
        // The Src data as this fidelity phase's multipliers take them, decoded once for every row: SrcA a row of columns
        // per vector, as the register file holds it, so that mvmul takes the columns together
        int32_t exp_prod_adj = fpu_exp_prod_adj(use_dst32b, fidelity_phase, fp_exponent_8b);
        fpu_operand_one b_ops[B_ROWS][16];
        for (uint32_t row = 0; row < B_ROWS; row++) {
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t value = p_tensix->src_b[src_b_bank][src_b_row + row][i];
#if TT_ARCH_VERSION == 0
                value = src_format_mask(value, src_b_fmt);
#endif
                b_ops[row][i] = fpu_operand_of<fpu_operand_one>(value >> 13, true, fidelity_phase, exp_prod_adj);
            }
        }
#if TT_ARCH_VERSION == 0
        uint32_t a_mask = src_format_mask(0xFFFFFFFF, src_a_fmt);
#else
        uint32_t a_mask = 0xFFFFFFFF;
#endif
        fpu_operand_lanes a_ops[ROW_SIZE / FPU_LANES][16];
        for (uint32_t i = 0; i < 16; i++) {
            for (uint32_t c = 0; c < ROW_SIZE / FPU_LANES; c++) {
                vec_u32 value = vec_load<vec_u32>(&p_tensix->src_a[src_a_bank][src_a_row + i][c * FPU_LANES]);
                a_ops[c][i] = fpu_operand_of<fpu_operand_lanes>((value & a_mask) >> 13, false, fidelity_phase, 0);
            }
        }
        // Every row is read before any is written, which changes nothing: they are distinct rows
        uint32_t dst_vals[N_ROWS][ROW_SIZE];
        for (uint32_t row = 0; row < N_ROWS; row++) {
            fpu_dst_read_row(p_tensix, dst_row + DST_STRIDE * row, dst_vals[row], use_dst32b, fp_exponent_8b, src_a_fmt);
        }
        bool specials = false;
        fpu_sops sops[ROW_SIZE / FPU_LANES];
        for (uint32_t row = 0; row < N_ROWS; row++) {
            for (uint32_t c = 0; c < ROW_SIZE / FPU_LANES; c++) {
                if (!broadcast || !row) { // a broadcast's rows all have the same products
                    sops[c] = mvmul_sops(a_ops[c], b_ops[broadcast ? 0 : row], specials, exp_prod_adj, fidelity_phase,
                        fp_exponent_8b);
                }
                vec_store(&dst_vals[row][c * FPU_LANES], fpu_accumulate(sops[c],
                    vec_load<vec_u32>(&dst_vals[row][c * FPU_LANES]), use_dst32b, fp_exponent_8b));
            }
            fpu_dst_write_row(p_tensix, dst_row + DST_STRIDE * row, dst_vals[row], use_dst32b, fp_exponent_8b,
                src_a_fmt);
        }
    }
    if (broadcast) {
        p_tensix->dst_row_valid[unflagged_row] = unflagged_row_valid;
        p_tensix->dst_row_valid[dst_valid_row(p_tensix, (dst_row ^ 1) + 4, use_dst32b)] = true;
        p_tensix->dst_row_valid[dst_valid_row(p_tensix, (dst_row ^ 1) + 6, use_dst32b)] = true;
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    math_clear_src_valid(p_tensix, pipe, clear_dvalid);
    return true;
}

TENSIX_EXECUTE_MVMUL() {
    TTSIM_ASSERT(instr_mod19 <= 1, "instr_mod19=%d", instr_mod19);
    if (instr_mod19) {
        return tensix_matmul_op<false, true>(p_tensix, pipe, dst, addr_mode, clear_dvalid);
    }
    return tensix_matmul_op<false>(p_tensix, pipe, dst, addr_mode, clear_dvalid);
}

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
                          uint32_t dest_accum_en, uint32_t clear_dvalid, uint32_t src_a_offset = 0,
                          uint32_t src_b_offset = 0) {
    TTSIM_VERIFY(!(dst & 7), UnsupportedFunctionality, "dst=%d", dst);
    if (elw_op::is_mul()) {
#if TT_ARCH_VERSION == 0
        TTSIM_VERIFY(!dest_accum_en, NonContractualBehavior, "dest_accum_en=%d", dest_accum_en);
#else
        TTSIM_VERIFY(!dest_accum_en, UndefinedBehavior, "dest_accum_en=%d", dest_accum_en);
#endif
        dest_accum_en = true; // ELWMUL forces accumulation, independent of dest_accum_en
    }
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_a_valid & (1 << src_a_bank)) || !(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until both valid
    }
    math_src_waited(p_tensix, false);
    math_src_waited(p_tensix, true);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // Note: FP16A_FORCE_Enable (thread_cfg56/55) is not instantiated and errors on write; always 0
    bool is_int8 = p_config->ALU_ACC_CTRL_INT8_math_enabled;
    uint32_t src_a_fmt = math_srca_format(p_tensix, p_config, pipe, src_a_bank);
    uint32_t src_b_fmt = math_srcb_format(p_tensix, p_config, pipe, src_b_bank);
    TTSIM_VERIFY((src_a_fmt != 12) && (src_a_fmt != 13), UndefinedBehavior, "src_a_fmt=%d", src_a_fmt);
    TTSIM_VERIFY((src_b_fmt != 12) && (src_b_fmt != 13), UndefinedBehavior, "src_b_fmt=%d", src_b_fmt);
    if (is_int8) {
        TTSIM_VERIFY(src_a_fmt == 14, UnsupportedFunctionality, "int8 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
        TTSIM_VERIFY(src_b_fmt == 14, UnsupportedFunctionality, "int8 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
    } else if ((src_a_fmt == 1) || (src_a_fmt == 10)) { // fp16/fp8
        TTSIM_VERIFY(src_a_fmt == src_b_fmt, UnsupportedFunctionality, "fp16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt);
    } else {
        TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7) || (src_a_fmt == 15),
            UnsupportedFunctionality, "bf16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4, bfp2
        TTSIM_VERIFY((src_b_fmt == 0) || (src_b_fmt == 4) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7) ||
                     (src_b_fmt == 8) || (src_b_fmt == 9) || (src_b_fmt == 15),
            UnimplementedFunctionality, "bf16 src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4, int32, int16, bfp2
    }
    bool use_dst32b = is_int8 || p_config->ALU_ACC_CTRL_Fp32_enabled;
    bool flush_denormals = !p_config->ALU_ACC_CTRL_Zero_Flag_disabled_src;
    bool fp_exponent_8b = (src_a_fmt != 1) && (src_a_fmt != 10) && !is_int8;

    uint32_t src_a_row = math_src_rwc(p_tensix->src_a_rwc[pipe] + src_a_offset);
    uint32_t src_b_row = math_src_rwc(p_tensix->src_b_rwc[pipe] + src_b_offset);
    uint32_t dst_row = math_dst_rwc(p_tensix, pipe) + dst + math_dest_offset(p_tensix, p_config, pipe);
    // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
    TTSIM_VERIFY(!(src_a_row & 7) && (src_a_row < SRC_ROWS), UnsupportedFunctionality, "src_a_row=%d", src_a_row);
    TTSIM_VERIFY(!(src_b_row & 7) && (src_b_row < SRC_ROWS), UnsupportedFunctionality, "src_b_row=%d", src_b_row);
    TTSIM_VERIFY(!(dst_row & 7) && (dst_row < DST_ROWS), UnsupportedFunctionality, "dst_row=%d", dst_row);
    uint32_t fidelity_phase = (p_tensix->fidelity[pipe] + math_fidelity_base(p_tensix, pipe)) & 3;
    if (!elw_op::is_mul()) {
        TTSIM_VERIFY(!fidelity_phase, UnsupportedFunctionality, "ELWADD/SUB should not be used with fidelity_phase=%d", fidelity_phase);
    }
    int32_t exp_prod_adj = fpu_exp_prod_adj(use_dst32b, fidelity_phase, fp_exponent_8b);
    bool flush_b_mul = flush_denormals;
    for (uint32_t row = 0; row < 8; row++) {
        uint32_t a_values[ROW_SIZE], b_values[ROW_SIZE];
        int32_t zf_b[ROW_SIZE] = {};
        for (uint32_t col = 0; col < ROW_SIZE; col++) {
            uint32_t value_a = p_tensix->src_a[src_a_bank][src_a_row + row][col];
            uint32_t src_b_datum_row = src_b_row + ((instr_mod19 & 2) ? 0 : row);
            uint32_t src_b_datum_col = (instr_mod19 & 1) ? 0 : col;
            uint32_t value_b = p_tensix->src_b[src_b_bank][src_b_datum_row][src_b_datum_col];
            if (!fp_exponent_8b) {
                TTSIM_VERIFY(!(value_a & 0x70000000), UnsupportedFunctionality, "SrcA 5b-exp datum=0x%x has nonzero high-3 exp bits", value_a);
                TTSIM_VERIFY(!(value_b & 0x70000000), UnsupportedFunctionality, "SrcB 5b-exp datum=0x%x has nonzero high-3 exp bits", value_b);
            }
#if TT_ARCH_VERSION == 0
            value_a = src_format_mask(value_a, src_a_fmt);
            value_b = src_format_mask(value_b, src_b_fmt);
#endif
            a_values[col] = value_a >> 13;
            b_values[col] = value_b >> 13;
            if (!is_int8) {
                continue;
            }
            if (elw_op::is_sub()) {
                value_b ^= 0x80000000; // int8 lives in the Src registers in sign-magnitude, so the read mux's flip negates it
            }
            bool zero_a = !(value_a & 0x7F800000); // note: flush denormals to zeros applies to INT8 too
            bool zero_b = flush_denormals && !(value_b & 0x7F800000);
            if (elw_op::is_mul()) {
                value_a &= (fidelity_phase & 1) ? 0x8003E000 : 0x801C0000;
                value_b &= (fidelity_phase & 2) ? 0x8001E000 : 0x807E0000;
            }
            int32_t src_a_val = zero_a ? 0 : read_src_int8(value_a);
            int32_t src_b_val = zero_b ? 0 : read_src_int8(value_b);
            int32_t result = elw_op::is_mul() ? (src_a_val * src_b_val) : (src_a_val + src_b_val); // cannot overflow, both inputs in [-1023,1023]
            if (dest_accum_en) {
                uint32_t dst_val = read_dst32b(p_tensix, dst_row + row, col);
                result = saturate_add_int32(dst_decode_int32(dst_val), result);
            }
            write_dst32b<true>(p_tensix, dst_row + row, col, dst_encode_int32(result));
        }
        if (is_int8) {
            continue;
        }
        // Without accumulation, Dest is a zero to the adder that never decides a zero result's sign
        uint32_t dst_vals[ROW_SIZE];
        if (dest_accum_en) {
            fpu_dst_read_row(p_tensix, dst_row + row, dst_vals, use_dst32b, fp_exponent_8b, src_a_fmt);
        } else {
            std::fill_n(dst_vals, ROW_SIZE, 0x80000000);
        }
        for (uint32_t c = 0; c < ROW_SIZE / FPU_LANES; c++) {
            vec_u32 a = vec_load<vec_u32>(&a_values[c * FPU_LANES]);
            vec_u32 b = vec_load<vec_u32>(&b_values[c * FPU_LANES]);
            fpu_sops sops;
            if (elw_op::is_mul()) {
                sops = elwmul_sops(fpu_operand_of<fpu_operand_lanes>(a, false, fidelity_phase, 0),
                    fpu_operand_of<fpu_operand_lanes>(b, true, fidelity_phase, exp_prod_adj, flush_b_mul), exp_prod_adj,
                    fidelity_phase, fp_exponent_8b);
            } else {
                sops = elwadd_sops(a, b, ((a >> 10) & 255) == 0, (((b >> 10) & 255) == 0) & -int32_t(flush_denormals),
                    vec_load<vec_i32>(&zf_b[c * FPU_LANES]), elw_op::is_sub(), use_dst32b, fp_exponent_8b);
            }
            vec_store(&dst_vals[c * FPU_LANES], fpu_accumulate(sops, vec_load<vec_u32>(&dst_vals[c * FPU_LANES]),
                use_dst32b, fp_exponent_8b));
        }
        fpu_dst_write_row(p_tensix, dst_row + row, dst_vals, use_dst32b, fp_exponent_8b, src_a_fmt);
    }
    math_update_counters(p_tensix, pipe, addr_mode, true);
    math_clear_src_valid(p_tensix, pipe, clear_dvalid);
    return true;
}

TENSIX_EXECUTE_ELWMUL() {
    return tensix_elw_op<elw_op_mul>(p_tensix, pipe, dst, addr_mode, instr_mod19, dest_accum_en, clear_dvalid);
}
TENSIX_EXECUTE_ELWADD() {
    return tensix_elw_op<elw_op_add>(p_tensix, pipe, dst, addr_mode, instr_mod19, dest_accum_en, clear_dvalid);
}
TENSIX_EXECUTE_ELWSUB() {
    return tensix_elw_op<elw_op_sub>(p_tensix, pipe, dst, addr_mode, instr_mod19, dest_accum_en, clear_dvalid);
}

TENSIX_EXECUTE_GMPOOL() {
#if TT_ARCH_VERSION >= 1
    uint32_t addr_mode = pool_addr_mode; // this field was renamed
#endif
    TTSIM_VERIFY(!max_pool_index_en, UnsupportedFunctionality, "max_pool_index_en=%d", max_pool_index_en);
    TTSIM_VERIFY(instr_mod19 == 1, UnsupportedFunctionality, "instr_mod19=%d", instr_mod19);
    TTSIM_VERIFY(!(dst & 3), UnsupportedFunctionality, "dst=%d", dst);
    uint32_t src_a_bank = p_tensix->src_a_matrix_bank;
    uint32_t src_b_bank = p_tensix->src_b_matrix_bank;
    if (!(p_tensix->src_a_valid & (1 << src_a_bank)) || !(p_tensix->src_b_valid & (1 << src_b_bank))) {
        return false; // stall until both valid
    }
    math_src_waited(p_tensix, false);
    math_src_waited(p_tensix, true);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    // Note: FP16A_FORCE_Enable (thread_cfg56/55) is not instantiated and errors on write; always 0
    TTSIM_VERIFY(!p_config->ALU_ACC_CTRL_INT8_math_enabled, UnsupportedFunctionality, "ALU_ACC_CTRL_INT8_math_enabled");
    uint32_t src_a_fmt = math_srca_format(p_tensix, p_config, pipe, src_a_bank);
    uint32_t src_b_fmt = math_srcb_format(p_tensix, p_config, pipe, src_b_bank);
    TTSIM_VERIFY((src_a_fmt != 12) && (src_a_fmt != 13), UndefinedBehavior, "src_a_fmt=%d", src_a_fmt);
    TTSIM_VERIFY((src_b_fmt != 12) && (src_b_fmt != 13), UndefinedBehavior, "src_b_fmt=%d", src_b_fmt);
    TTSIM_VERIFY((src_a_fmt == 0) || (src_a_fmt == 4) || (src_a_fmt == 5) || (src_a_fmt == 6) || (src_a_fmt == 7),
        UnsupportedFunctionality, "src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4
    TTSIM_VERIFY((src_b_fmt == 0) || (src_b_fmt == 4) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7),
        UnsupportedFunctionality, "src_a_fmt=%d src_b_fmt=%d", src_a_fmt, src_b_fmt); // fp32, tf32, bf16, bfp8, bfp4
    bool use_dst32b = p_config->ALU_ACC_CTRL_Fp32_enabled;

    uint32_t src_a_row = p_tensix->src_a_rwc[pipe];
    uint32_t src_b_row = p_tensix->src_b_rwc[pipe];
    uint32_t dst_row = math_dst_rwc(p_tensix, pipe) + dst + math_dest_offset(p_tensix, p_config, pipe);
    // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
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
            max = dst_decode_fp32(read_dst32b<true>(p_tensix, dst_row, j)) & 0xFFFFE000; // truncate to TF32
        } else {
            max = uint32_t(dst_decode_bf16(read_dst16b<true>(p_tensix, dst_row, j))) << 16;
        }
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t src_a_val = p_tensix->src_a[src_a_bank][src_a_row + i][j];
            if (src_a_fmt != 4) { // the max array takes a bf16-class SrcA with seven mantissa bits
                src_a_val &= 0xFFFF0000;
            }
            if (!(src_a_val & 0x7F800000)) {
                src_a_val = 0;
            }
            if (sign_mag32_total_order(src_a_val) > sign_mag32_total_order(max)) {
                max = src_a_val;
            }
        }
        if (use_dst32b) {
            if (src_a_fmt == 4) { // bug for bug: a tf32 max's low three mantissa bits are written again at bits 12:10
                max |= (max >> 3) & 0x1C00;
            }
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
    TTSIM_VERIFY(instr_mod19 == 1, UnsupportedFunctionality, "instr_mod19=%d", instr_mod19);
    TTSIM_VERIFY(!max_pool_index_en, UnsupportedFunctionality, "max_pool_index_en=%d", max_pool_index_en);
#if TT_ARCH_VERSION >= 1
    uint32_t addr_mode = pool_addr_mode; // this field was renamed
#endif
    return tensix_matmul_op<true>(p_tensix, pipe, dst, addr_mode, clear_dvalid);
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
            math_src_released(p_tensix, false, p_tensix->src_a_matrix_bank);
            if (!(reset & 2)) {
                p_tensix->src_a_matrix_bank ^= 1;
            }
        }
        if (cleardvalid & 2) {
            TTSIM_VERIFY(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank), NonContractualBehavior, "SrcB bank is not valid");
            p_tensix->src_b_valid &= ~(1 << p_tensix->src_b_matrix_bank);
            math_src_released(p_tensix, true, p_tensix->src_b_matrix_bank);
            if (!(reset & 2)) {
                p_tensix->src_b_matrix_bank ^= 1;
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_SETRWC() {
    TTSIM_VERIFY(rwc_cr <= 7, UnsupportedFunctionality, "rwc_cr=%d", rwc_cr);

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
            rwc_d = (p_tensix->dst_rwc_cr[pipe] + rwc_d) & (DST_RWC_ROWS - 1);
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
    TTSIM_VERIFY(!rwc_cr || (rwc_cr == 4), UnsupportedFunctionality, "rwc_cr=%d", rwc_cr);

    p_tensix->src_a_rwc[pipe] = (p_tensix->src_a_rwc[pipe] + rwc_a) & (SRC_RWC_ROWS - 1);
    p_tensix->src_b_rwc[pipe] = (p_tensix->src_b_rwc[pipe] + rwc_b) & (SRC_RWC_ROWS - 1);
    if (rwc_cr & 4) {
        p_tensix->dst_rwc[pipe] = p_tensix->dst_rwc_cr[pipe] =
            (p_tensix->dst_rwc_cr[pipe] + rwc_d) & (DST_RWC_ROWS - 1);
    } else {
        p_tensix->dst_rwc[pipe] = (p_tensix->dst_rwc[pipe] + rwc_d) & (DST_RWC_ROWS - 1);
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
        default: TTSIM_ERROR(UnsupportedFunctionality, "format=%d", format);
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

static uint16_t pack_l1_acc_fp16(uint16_t dst, uint16_t src) {
    uint32_t dst32 = uint32_t(dst & 0x8000) << 16;
    uint32_t src32 = uint32_t(src & 0x8000) << 16;
    if ((dst & 0x7FFF) >= 0x400) { // any nonzero exponent becomes an FP32 normal (no inf/NaN encodings)
        dst32 |= (uint32_t(dst & 0x7FFF) << 13) + (112 << 23);
    }
    if ((src & 0x7FFF) >= 0x400) { // any nonzero exponent becomes an FP32 normal (no inf/NaN encodings)
        src32 |= (uint32_t(src & 0x7FFF) << 13) + (112 << 23);
    }
    uint32_t result32 = std::bit_cast<uint32_t>(std::bit_cast<float>(dst32) + std::bit_cast<float>(src32));
    uint16_t sign = (result32 >> 16) & 0x8000;
    if (!(result32 & 0x7FFFFFFF)) { // exact cancellation: |dst| == |src|
        return sign | ((((dst >> 10) & 31) >= 30) ? 0x7FFF : 0); // some cancellation cases produce overflow (!)
    }
    int32_t e = ((result32 >> 23) & 0xFF) - 112;
    if (e <= 0) {
        return sign | ((e < 0) ? 0x7FFF : 0); // underflow saturates; a zero exponent field is signed zero
    }
    uint32_t m = result32 & 0x7FFFFF;
    uint32_t packed = (e << 10) + ((m + (1u << (22 - 10)) - 1 + ((m >> (23 - 10)) & 1)) >> (23 - 10)); // RTNE
    return sign | ((packed < 0x8000) ? packed : 0x7FFF); // saturate on overflow
}

TENSIX_EXECUTE_PACR() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!flush, UnsupportedFunctionality, "flush=%d", flush);
    TTSIM_VERIFY(!ctxt_ctrl, UnsupportedFunctionality, "ctxt_ctrl=%d", ctxt_ctrl);
#endif
    TTSIM_VERIFY(!ovrd_thread_id, UnsupportedFunctionality, "ovrd_thread_id=%d", ovrd_thread_id);
#if TT_ARCH_VERSION == 1
    if (!read_intf_sel) {
        read_intf_sel = 15; // a zero mask enables all four Dst read interfaces
    }
    TTSIM_VERIFY((read_intf_sel == 1) || (read_intf_sel == 3) || (read_intf_sel == 5) ||
                 (read_intf_sel == 7) || (read_intf_sel == 10) || (read_intf_sel == 15),
        UnsupportedFunctionality, "read_intf_sel=%d", read_intf_sel);
#else
    TTSIM_VERIFY((pack_sel == 1) || (pack_sel == 3) || (pack_sel == 15), UnsupportedFunctionality, "pack_sel=%d", pack_sel);
#endif
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!zero_write, UnimplementedFunctionality, "zero_write=%d", zero_write);
    TTSIM_VERIFY(!addr_cnt_context, UnsupportedFunctionality, "addr_cnt_context=%d", addr_cnt_context);
    TTSIM_VERIFY(!row_pad_zero, UnsupportedFunctionality, "row_pad_zero=%d", row_pad_zero);
    TTSIM_VERIFY(!cfg_context, UnsupportedFunctionality, "cfg_context=%d", cfg_context);

    // Read interface j reads the Dest row whose interface position is j: bits 1:0 of the row, or in strided mode
    // (dst_access_mode) bits 4:3 of its address after remap_addrs, so the interfaces' rows are 8 rows apart in storage
    // and 16 rows apart before a remap
    uint32_t intf_shift = !dst_access_mode ? 0 : p_tensix->DEST_ACCESS_CFG_remap_addrs ? 4 : 3;
#endif

    // Note: HW erratum (WH): a PACR that starts while an earlier one is still packing, with the Dst read format
    // (Read_32b_data, Dstacc, the packers' formats) changed in between, can hang the packers; not modeled
    // Note: HW erratum: these Dst reads aren't ordered against later Dst writes (Matrix Unit, SFPSTORE, ZEROACC; on BH
    // any that reaches Dst before the last read, e.g. an SFPU stream right behind) nor an earlier unpack; not modeled
    // Note: HW erratum (WH only): while these reads are under way, a Matrix Unit Dst read (MVMUL, accumulating ELWADD,
    // ...) or an SFPLOAD in the same half of Dst can get the row the packer is reading instead of its own; not modeled
    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
#if TT_ARCH_VERSION == 1
    // A 32-bit strided read relies on the swizzle_32b layout: without it the hardware reads the wrong rows
    TTSIM_VERIFY(!dst_access_mode || !p_config->PCK_DEST_RD_CTRL_Read_32b_data || p_tensix->DEST_ACCESS_CFG_swizzle_32b,
        UnsupportedFunctionality, "dst_access_mode=%d with Read_32b_data and swizzle_32b=%d", dst_access_mode,
        p_tensix->DEST_ACCESS_CFG_swizzle_32b);
#endif
    // Note: on WH, PCK_EDGE_TILE_FACE_SET_SELECT_enable (cfg15) is not instantiated and errors on write; always 0
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
#if TT_ARCH_VERSION == 1
    // Note: TILE_FACE_SET_MAPPING_1..3 (cfg37-39) are not instantiated and error on write; always 0
    uint32_t tile_face_set_mapping[16] = {
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_0,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_1,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_2,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_3,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_4,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_5,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_6,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_7,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_8,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_9,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_10,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_11,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_12,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_13,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_14,
        p_config->TILE_FACE_SET_MAPPING_0_face_set_mapping_15,
    };
#endif
    uint32_t pack_reads_per_xy_plane = p_config->PACK_COUNTERS_SEC0_pack_reads_per_xy_plane;
    TTSIM_VERIFY((pack_reads_per_xy_plane >= 1) && (pack_reads_per_xy_plane <= 16), UnsupportedFunctionality, "pack_reads_per_xy_plane=%d", pack_reads_per_xy_plane);
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(p_config->PACK_COUNTERS_SEC1_pack_reads_per_xy_plane == pack_reads_per_xy_plane, UnsupportedFunctionality, "pack_reads_per_xy_plane inconsistent");
    TTSIM_VERIFY(p_config->PACK_COUNTERS_SEC2_pack_reads_per_xy_plane == pack_reads_per_xy_plane, UnsupportedFunctionality, "pack_reads_per_xy_plane inconsistent");
    TTSIM_VERIFY(p_config->PACK_COUNTERS_SEC3_pack_reads_per_xy_plane == pack_reads_per_xy_plane, UnsupportedFunctionality, "pack_reads_per_xy_plane inconsistent");
#endif

#if TT_ARCH_VERSION == 0
#define PACK_VERIFY_THCON(f, val) \
    TTSIM_VERIFY((p_config->THCON_SEC0_REG1_##f == val) && (p_config->THCON_SEC0_REG8_##f == val) && \
                 (p_config->THCON_SEC1_REG1_##f == val) && (p_config->THCON_SEC1_REG8_##f == val), \
        UnsupportedFunctionality, #f)
#else
#define PACK_VERIFY_THCON(f, val) \
    TTSIM_VERIFY(p_config->THCON_SEC0_REG1_##f == val, UnsupportedFunctionality, #f)
#endif
#define PACK_VERIFY_THCON0(f) PACK_VERIFY_THCON(f, 0)
    PACK_VERIFY_THCON(Disable_zero_compress, 1);
#if TT_ARCH_VERSION == 0
    PACK_VERIFY_THCON0(Addr_cnt_context);
    PACK_VERIFY_THCON0(Sub_l1_tile_header_size);
    PACK_VERIFY_THCON0(Source_interface_selection);
    PACK_VERIFY_THCON0(Add_tile_header_size);
    PACK_VERIFY_THCON0(L1_source_addr);
    PACK_VERIFY_THCON0(Downsample_mask);
    PACK_VERIFY_THCON0(Downsample_rate);
    PACK_VERIFY_THCON0(Read_mode);
#endif
    PACK_VERIFY_THCON0(Exp_threshold_en);
    PACK_VERIFY_THCON0(Exp_threshold);
    PACK_VERIFY_THCON0(Disable_pack_zero_flags);
#undef PACK_VERIFY_THCON0

#if TT_ARCH_VERSION == 1
    // Blackhole's packer doesn't read ALU_FORMAT_SPEC_REG2_Dstacc: it reads Dest in In_data_format (or in
    // Dstacc_val with Dstacc_override, which is refused)
    uint32_t intermediate_format = p_config->THCON_SEC0_REG1_In_data_format;
#else
    uint32_t intermediate_format = p_config->ALU_FORMAT_SPEC_REG2_Dstacc;
#endif
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
                 ((pack_fmt_conv_mode == 0x00) && (TT_ARCH_VERSION == 1)) || // bh only, refused below
                 ((pack_fmt_conv_mode == 0x1A) && (TT_ARCH_VERSION == 1)) || // bh only fp16 src, fp8 dst
                 (pack_fmt_conv_mode == 0x26) || // bfp8a src, bfp8 dst
                 (pack_fmt_conv_mode == 0x50) || (pack_fmt_conv_mode == 0x55) || // bf16 src, fp32/bf16 dst
                 (pack_fmt_conv_mode == 0x56) || (pack_fmt_conv_mode == 0x57) || (pack_fmt_conv_mode == 0x5F) || // bf16 src, bfp8/bfp4/bfp2 dst
                 (pack_fmt_conv_mode == 0x60) || (pack_fmt_conv_mode == 0x65) || (pack_fmt_conv_mode == 0x66) || // bfp8 src, fp32/bf16/bfp8 dst
                 (pack_fmt_conv_mode == 0x67) || // bfp8 src, bfp4 dst
                 (pack_fmt_conv_mode == 0x99) ||
                 (pack_fmt_conv_mode == 0x100) || (pack_fmt_conv_mode == 0x101) || (pack_fmt_conv_mode == 0x105) ||
                 (pack_fmt_conv_mode == 0x106) || (pack_fmt_conv_mode == 0x107) || (pack_fmt_conv_mode == 0x10F) ||
                 ((pack_fmt_conv_mode == 0x111) && (TT_ARCH_VERSION == 1)) || // bh only fp32 src, fp16 dst
                 ((pack_fmt_conv_mode == 0x11A) && (TT_ARCH_VERSION == 1)) || // bh only fp32 src, fp8 dst
                 (pack_fmt_conv_mode == 0x155) || (pack_fmt_conv_mode == 0x166) || (pack_fmt_conv_mode == 0x167) ||
                 (pack_fmt_conv_mode == 0x188) || (pack_fmt_conv_mode == 0x199) || (pack_fmt_conv_mode == 0x1EE),
        UnimplementedFunctionality, "pack_fmt_conv_mode=0x%x", pack_fmt_conv_mode);
    if ((pack_fmt_conv_mode == 0x111) || (pack_fmt_conv_mode == 0x11A)) {
        TTSIM_VERIFY(p_config->PCK_DEST_RD_CTRL_Round_10b_mant, UnsupportedFunctionality,
            "pack_fmt_conv_mode=0x%x round_10b_mant=%d", pack_fmt_conv_mode, p_config->PCK_DEST_RD_CTRL_Round_10b_mant);
    } else if (pack_fmt_conv_mode != 0x1A) {
        TTSIM_VERIFY(!p_config->PCK_DEST_RD_CTRL_Round_10b_mant, UnsupportedFunctionality,
            "pack_fmt_conv_mode=0x%x round_10b_mant=%d", pack_fmt_conv_mode, p_config->PCK_DEST_RD_CTRL_Round_10b_mant);
    }
#if TT_ARCH_VERSION == 1
    if (pack_fmt_conv_mode == 0x00) {
        TTSIM_VERIFY(!p_config->STACC_RELU_ApplyRelu && !p_config->PCK_DEST_RD_CTRL_Read_int8, UnsupportedFunctionality,
            "pack_fmt_conv_mode=0x0 apply_relu=%d read_raw=%d", p_config->STACC_RELU_ApplyRelu, p_config->PCK_DEST_RD_CTRL_Read_int8);
        // Each datum's low 16 bits come from another Dest row
        TTSIM_ERROR(UnsupportedFunctionality, "pack_fmt_conv_mode=0x0");
    }
#endif
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
    TTSIM_VERIFY(!p_addr_ctrl->ch0_x, UnsupportedFunctionality, "ch0_x=%d", p_addr_ctrl->ch0_x);
    // Note: PCK0_ADDR_BASE_REG_0_Base (cfg12/16) is not instantiated and errors on write; always 0
    uint32_t src_addr = (p_addr_ctrl->ch0_y * ch0_y_stride +
                         p_addr_ctrl->ch0_z * ch0_z_stride +
                         p_addr_ctrl->ch0_w * ch0_w_stride) / src_element_align;
    TTSIM_VERIFY(!(src_addr % ROW_SIZE), UnsupportedFunctionality, "misaligned src_addr=%d", src_addr);

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
        TTSIM_VERIFY((pack_dst_format == 0) || (pack_dst_format == 1) || (pack_dst_format == 5),
            UnsupportedFunctionality, "pack_l1_acc: pack_dst_format=%d", pack_dst_format); // fp32, fp16, bf16
    }
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(pack_l1_acc == p_config->THCON_SEC0_REG8_Pack_L1_Acc, UnsupportedFunctionality, "Pack_L1_Acc inconsistent");
    TTSIM_VERIFY(pack_l1_acc == p_config->THCON_SEC1_REG1_Pack_L1_Acc, UnsupportedFunctionality, "Pack_L1_Acc inconsistent");
    TTSIM_VERIFY(pack_l1_acc == p_config->THCON_SEC1_REG8_Pack_L1_Acc, UnsupportedFunctionality, "Pack_L1_Acc inconsistent");
#endif

    uint32_t count = flush ? 0 : (p_addr_ctrl->ch1_x - p_addr_ctrl->ch0_x + 1);
#if TT_ARCH_VERSION == 1
    uint32_t row_shift = __builtin_ctz(ROW_SIZE); // log2 of the datums packed from each read interface's Dst row
    if (read_intf_sel == 1) {
        TTSIM_VERIFY((count == 8) || (count == 16), UnsupportedFunctionality, "count=%d read_intf_sel=%d", count, read_intf_sel);
    } else if ((count == 1) && (read_intf_sel == 15) && !dst_access_mode && (src_element_size_bits >= 16) &&
               !pack_l1_acc) {
        row_shift = 0; // X end 0: datum 0 of each of the four rows, the four datums one after another in L1
        count = 4;
    } else {
        TTSIM_VERIFY(count == 16, UnsupportedFunctionality, "count=%d read_intf_sel=%d", count, read_intf_sel);
        count *= __builtin_popcount(read_intf_sel); // one 16 datum Dst row per enabled read interface
    }
    constexpr uint32_t n_packers = 1;
    // the tile row for edge masking starts at 4 * ((Offset >> k) & 3), k the lowest set bit of the row stride in rows
    uint32_t y_stride_rows = (ch0_y_stride >> (6 - std::min(pack_src_format & 3, 2u))) & 0x1FF;
    uint32_t y_stride_shift = y_stride_rows ? __builtin_ctz(y_stride_rows) : 0;
    uint32_t tpg_y_offset = 4 * ((p_tensix->DEST_TARGET_REG_CFG_PACK_SEC0_Offset >> y_stride_shift) & 3);
#else
    // XXX for now, just allow zero or any power of two <= 256
    TTSIM_VERIFY((count <= 256) && !(count & (count - 1)), UnsupportedFunctionality, "count=%d", count);
    uint32_t n_packers = (pack_sel == 15) ? 4 : (pack_sel == 3) ? 2 : 1;
    constexpr uint32_t row_shift = 4; // log2 ROW_SIZE: WH packs whole 16 datum rows
#endif

    for (uint32_t packer = 0; packer < n_packers; packer++) {
#if TT_ARCH_VERSION == 0
        // WH's tile position generator is not reset by Last: it runs on from tile to tile
        uint32_t tpg_x = p_tensix->packer_tpg_x[packer];
        uint32_t tpg_y = p_tensix->packer_tpg_y[packer];
        [[maybe_unused]] uint32_t tpg_z = p_tensix->packer_tpg_z[packer];
#else
        uint32_t tpg_x = p_tensix->packer_valid[packer] ? p_tensix->packer_tpg_x[packer] : 0;
        uint32_t tpg_y = p_tensix->packer_valid[packer] ? p_tensix->packer_tpg_y[packer] : 0;
        [[maybe_unused]] uint32_t tpg_z = p_tensix->packer_valid[packer] ? p_tensix->packer_tpg_z[packer] : 0;
#endif
        uint32_t edge_mask_b = (p_config->PCK_EDGE_TILE_ROW_SET_SELECT_select >> (2 * packer)) & 3;
#if TT_ARCH_VERSION == 1
        if (p_config->PCK_EDGE_TILE_FACE_SET_SELECT_enable) { // the face's row set mapping replaces the select
            uint32_t edge_mask_a = p_config->PCK_EDGE_TILE_FACE_SET_SELECT_select & 3;
            TTSIM_VERIFY(!edge_mask_a, UnsupportedFunctionality, "edge_mask_a=%d", edge_mask_a);
            edge_mask_b = tile_face_set_mapping[tpg_z & 15];
        }
#endif
        TTSIM_VERIFY(edge_mask_b <= 1, UnsupportedFunctionality, "edge_mask_b=%d", edge_mask_b);
#if TT_ARCH_VERSION == 0
        // WH's tile position generator counts 16-byte Dst words, a row being 4 words of fp32/tf32/int32, 2 of
        // fp16/bf16 and 1 of any other format (int16 too), and a word read in part counts whole. ttsim counts 16
        // datums a row, which picks the same rows only for whole words of a format with 16 datums a row, so edge masks
        // that differ by row are refused otherwise
        uint32_t datums_per_word = 16 / src_element_align;
        bool words_differ = ((src_element_size_bits == 16) && (pack_src_format != 1) && (pack_src_format != 5)) ||
                            (count % datums_per_word) || (tpg_x % datums_per_word);
        if (count && words_differ && ((pack_reads_per_xy_plane > 1) || tpg_y)) { // with one row a plane, Y stays 0
            bool rows_differ = false;
            for (uint32_t y = 1; y < 16; y++) {
                rows_differ |= tile_row_set_mapping[edge_mask_b][y] != tile_row_set_mapping[edge_mask_b][0];
            }
            TTSIM_VERIFY(!rows_differ || (p_config->PCK_EDGE_OFFSET_SEC0_mask == p_config->PCK_EDGE_OFFSET_SEC1_mask),
                UnsupportedFunctionality, "edge masks by row with In_data_format=%d count=%d at datum %d of a row",
                pack_src_format, count, tpg_x);
        }
#endif
        uint32_t pack_row = src_addr / ROW_SIZE;
        switch (packer) {
            default: TTSIM_UNREACHABLE("invalid packer");
            case 0: pack_row += p_tensix->DEST_TARGET_REG_CFG_PACK_SEC0_Offset; break;
#if TT_ARCH_VERSION == 0
            case 1: pack_row += p_tensix->DEST_TARGET_REG_CFG_PACK_SEC1_Offset; break;
            case 2: pack_row += p_tensix->DEST_TARGET_REG_CFG_PACK_SEC2_Offset; break;
            case 3: pack_row += p_tensix->DEST_TARGET_REG_CFG_PACK_SEC3_Offset; break;
#endif
        }
        pack_row &= DST_ROWS - 1;
#if TT_ARCH_VERSION == 1
        // the read interfaces' rows must lie within one group of four interface positions
        TTSIM_VERIFY((read_intf_sel << ((pack_row >> intf_shift) & 3)) <= 15, UnsupportedFunctionality,
            "read_intf_sel=%d with pack_row=%d dst_access_mode=%d", read_intf_sel, pack_row, dst_access_mode);
        TTSIM_VERIFY(dst_access_mode || (pack_row + ((count + ROW_SIZE - 1) / ROW_SIZE) <= DST_ROWS),
            UnsupportedFunctionality, "pack_row=%d count=%d", pack_row, count);
#else
        TTSIM_VERIFY(pack_row + ((count + ROW_SIZE - 1) / ROW_SIZE) <= DST_ROWS, UnsupportedFunctionality, "pack_row=%d count=%d", pack_row, count);
#endif
        // A BFP tile takes part of its L1 placement from a later PACR when its address inputs change while it is
        // open: on BH the whole tile moves (within its first four Dst rows), on WH its exponents do. A Flush may
        // follow new addresses: it drains the open tile to the old ones.
        // Not checked: on BH a tile with any other late format conversion (In_data_format and Out_data_format differ)
        // moves whole too when the change comes before it has read 128 bytes of Dst (measured for 16- and 32-bit
        // inputs and BFP8_b, a BFP8_b datum counted as 2 bytes); ttsim keeps the first address
#if TT_ARCH_VERSION == 1
        // Channel 1's address (Y * Ystride + Z * Zstride) is 18 bits: one of 256 KB or more wraps, an artifact of the
        // address arithmetic
        uint32_t yzw_addr = p_addr_ctrl->ch1_y * p_config->PCK0_ADDR_CTRL_XY_REG_1_Ystride +
                            p_addr_ctrl->ch1_z * p_config->PCK0_ADDR_CTRL_ZW_REG_1_Zstride;
        TTSIM_VERIFY(yzw_addr <= 0x3FFFF, NonContractualBehavior, "channel 1 address 0x%x wraps (ch1_y=%d ch1_z=%d)",
            yzw_addr, p_addr_ctrl->ch1_y, p_addr_ctrl->ch1_z);
        uint32_t l1_base = packer_addrs[packer] + (yzw_addr >> 4);
#else
        uint32_t l1_base = packer_addrs[packer] + ((p_addr_ctrl->ch1_y + (p_addr_ctrl->ch1_z << 8)) << 12);
#endif
        TTSIM_VERIFY(!p_tensix->packer_valid[packer] || flush || !is_bfp_format(pack_dst_format) ||
                     (l1_base == p_tensix->packer_l1_base[packer]),
            UnsupportedFunctionality, "bfp tile's L1 address changed mid-tile: 0x%x then 0x%x",
            p_tensix->packer_l1_base[packer] << 4, l1_base << 4);
        if (!p_tensix->packer_valid[packer]) {
            p_tensix->packer_l1_base[packer] = l1_base;
        }
        if (!p_tensix->packer_valid[packer]) { // each packer takes new addresses on its first PACR of a tile
            uint32_t addr = packer_addrs[packer];
#if TT_ARCH_VERSION == 0
            TTSIM_VERIFY(!p_addr_ctrl->ch1_z, UnsupportedFunctionality, "ch1_z=%d", p_addr_ctrl->ch1_z);
#endif
            TTSIM_VERIFY(!p_addr_ctrl->ch1_w, UnsupportedFunctionality, "ch1_w=%d", p_addr_ctrl->ch1_w);
#if TT_ARCH_VERSION == 1
            TTSIM_VERIFY(!p_config->PCK0_ADDR_BASE_REG_1_Base, UnsupportedFunctionality, "PCK0_ADDR_BASE_REG_1_Base=0x%x", p_config->PCK0_ADDR_BASE_REG_1_Base);
            addr += yzw_addr >> 4;
#else
            // Note: PCK0_ADDR_BASE_REG_1_Base (cfg13) is not instantiated and errors on write; always 0
            TTSIM_VERIFY(!p_addr_ctrl->ch1_y, UnsupportedFunctionality, "ch1_y=%d", p_addr_ctrl->ch1_y);
#endif
            p_tensix->packer_dst_exp_addr[packer] = addr << 4;
            if (dst_element_size_bits <= 8) {
                uint32_t exp_section_size;
                switch (packer) {
                    default: TTSIM_UNREACHABLE("invalid packer");
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
            p_tensix->packer_dst_data_start[packer] = addr << 4;
        }
        if (dst_element_size_bits <= 8) {
            TTSIM_VERIFY(!(count & 15), UnsupportedFunctionality, "bfp with count=%d", count);
        }
        if (is_bfp_format(pack_dst_format)) { // exponents past their section overlap the data, in no defined order
            TTSIM_VERIFY(p_tensix->packer_dst_exp_addr[packer] + count / 16 <= p_tensix->packer_dst_data_start[packer],
                UnpredictableValue, "bfp exponents past Exp_section_size");
        }
        uint32_t dst_addr = p_tensix->packer_dst_addr[packer];
        uint32_t dst_exp_addr = p_tensix->packer_dst_exp_addr[packer];
        uint16_t bfp_buffer[16];
        for (uint32_t i = 0; i < count; i++) {
#if TT_ARCH_VERSION == 1
            uint32_t edge_mask_c = tile_row_set_mapping[edge_mask_b][(tpg_y + tpg_y_offset + (i >> row_shift)) & 15];
#else
            uint32_t edge_mask_c = tile_row_set_mapping[edge_mask_b][tpg_y & 15];
#endif
            TTSIM_VERIFY(edge_mask_c <= 1, UnsupportedFunctionality, "edge_mask_c=%d", edge_mask_c);
            uint32_t edge_mask = edge_mask_c ? p_config->PCK_EDGE_OFFSET_SEC1_mask : p_config->PCK_EDGE_OFFSET_SEC0_mask;
            TTSIM_VERIFY((edge_mask == 0) || (edge_mask == 1) || (edge_mask == 0xFFFF), UntestedFunctionality, "edge_mask=0x%x", edge_mask);
            TTSIM_VERIFY(!p_config->PCK_EDGE_MODE_mode, UnsupportedFunctionality, "edge_mode=%d", p_config->PCK_EDGE_MODE_mode);
#if TT_ARCH_VERSION == 0
            tpg_x += !zero_write; // the tile position generator steps on Dst reads, and a ZeroWrite makes none
            if (tpg_x == 16) {
                tpg_x = 0;
                tpg_y++;
                if (tpg_y == pack_reads_per_xy_plane) {
                    tpg_y = 0;
                    tpg_z++;
                }
            }
#endif

            uint32_t value = 0;
            uint32_t col = i & ((1 << row_shift) - 1);
            // A ZeroWrite (Wormhole) packs a zero for every datum, with no edge mask or ReLU applied
            if (!zero_write && (edge_mask & (1 << col))) {
                uint32_t row = pack_row + (i >> row_shift);
#if TT_ARCH_VERSION == 1
                uint32_t intf_mask = read_intf_sel; // the read interface of row i >> row_shift: its enabled one in order
                for (uint32_t k = i >> row_shift; k; k--) {
                    intf_mask &= intf_mask - 1;
                }
                row = pack_row + (__builtin_ctz(intf_mask) << intf_shift);
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
                            TTSIM_VERIFY(!read_raw, UnsupportedFunctionality, "fp32 to bf16/bfp8: read_raw=%d", read_raw);
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
                            TTSIM_VERIFY(!read_raw, UnsupportedFunctionality, "int32 to int8: read_raw=%d", read_raw);
                            if (read_unsigned) { // the sign is dropped: a negative value packs as its magnitude
                                value = std::min(value & 0x7FFFFFFFu, 255u);
                            } else { // -0 packs as +0
                                uint32_t mag = std::min(value & 0x7FFFFFFFu, 127u);
                                value = mag ? (((value & 0x80000000) >> 24) | mag) : 0;
                            }
#if TT_ARCH_VERSION == 1
                        } else if (intermediate_format == 1) {
                            if (read_raw) {
                                value >>= 13; // truncate to TF32, keeping NaN, denormals and -0
                            } else {
                                if ((value & 0x7FFFFFFF) > 0x7F800000) {
                                    value = (value & 0x80000000) | 0x7F800000;
                                }
                                value = (value + 0x1000) >> 13; // round to TF32
                                if ((value & 0x3FFFF) < 0x400) {
                                    value = 0;
                                }
                            }
#endif
                        } else if ((intermediate_format != 0) && (intermediate_format != 8)) {
                            TTSIM_ERROR(UnsupportedFunctionality, "dst32b intermediate_format=%d", intermediate_format);
                        }
                    }
                } else {
                    value = read_dst16b(p_tensix, row, col);
#if TT_ARCH_VERSION == 1
                    if ((intermediate_format == 1) && p_config->PCK_DEST_RD_CTRL_Round_10b_mant) {
                        value = dst_decode_bf16(value) << 3; // as e8m10, like the fp32 path; rebiased after ReLU
                    } else
#endif
                    if ((intermediate_format == 1) || (intermediate_format == 2)) {
                        value = dst_decode_fp16(value);
                        TTSIM_VERIFY(!read_raw || (pack_fmt_conv_mode == 0x11), UnsupportedFunctionality,
                            "pack_fmt_conv_mode=0x%x read_raw=%d", pack_fmt_conv_mode, read_raw);
                        if (intermediate_format == 2) {
                            if ((value & 0x7FFF) > 0x7FF7) {
                                value = (value & 0x8000) | 0x7FF0;
                            }
                            value = (value + 8) & ~15; // round to E5M6, keep 16 bits
                        }
                        if (!read_raw && ((value & 0x7FFF) < 0x400)) { // read_raw keeps denormals and -0
                            value = 0;
                        }
                    } else if ((intermediate_format == 5) || (intermediate_format == 6)) {
                        value = dst_decode_bf16(value);
                        TTSIM_VERIFY(!read_raw || (pack_fmt_conv_mode == 0x55), UnsupportedFunctionality,
                            "pack_fmt_conv_mode=0x%x read_raw=%d", pack_fmt_conv_mode, read_raw);
                        if (!read_raw && ((value & 0x7FFF) > 0x7F80)) { // read_raw keeps NaN, denormals and -0
                            value = (value & 0x8000) | 0x7F80;
                        }
                        if (intermediate_format == 6) {
                            value = (value + 1) & ~1; // reduce to m6 instead of m7
                        }
                        if (!read_raw && ((value & 0x7FFF) < 0x80)) {
                            value = 0;
                        }
                    } else if (intermediate_format != 9) {
                        TTSIM_ERROR(UnsupportedFunctionality, "dst16b intermediate_format=%d", intermediate_format);
                    }
                }
                if (uint32_t apply_relu = p_config->STACC_RELU_ApplyRelu) {
                    TTSIM_VERIFY(apply_relu <= 3, UnsupportedFunctionality, "apply_relu=%d", apply_relu);
                    TTSIM_VERIFY((apply_relu == 1) || !(p_config->STACC_RELU_ReluThreshold & 0x8000),
                        NonContractualBehavior, "negative relu_threshold=0x%x", p_config->STACC_RELU_ReluThreshold);
                    TTSIM_VERIFY((apply_relu != 3) || !is_bfp_format(pack_dst_format) || (intermediate_format == 0) ||
                                 !(p_config->STACC_RELU_ReluThreshold & 0x7FFF) ||
                                 (p_config->STACC_RELU_ReluThreshold & ((intermediate_format == 2) ? 0x7C00 : 0x7F80)),
                        UnsupportedFunctionality, "denormal relu_threshold=0x%x to bfp", p_config->STACC_RELU_ReluThreshold);
                    TTSIM_VERIFY((intermediate_format == 0) || (intermediate_format == 1) || (intermediate_format == 2) ||
                                 (intermediate_format == 5) || (intermediate_format == 6) || (intermediate_format == 8),
                        UnsupportedFunctionality, "relu intermediate_format=%d",
                        intermediate_format);
                    // Read_raw keeps the FP16 or BF16 value unrounded, and the hardware's threshold compare then isn't
                    // a compare of that value: in some columns, a value equal to the threshold is kept or zeroed
                    // depending on data left from earlier Dest reads
                    TTSIM_VERIFY((apply_relu != 2) || !read_raw || ((intermediate_format != 1) && (intermediate_format != 5)),
                        UnsupportedFunctionality, "read_raw apply_relu=%d intermediate_format=%d", apply_relu,
                        intermediate_format);
#if TT_ARCH_VERSION == 1
                    if ((intermediate_format == 1) && p_config->PCK_DEST_RD_CTRL_Round_10b_mant) {
                        // a 16-bit read keeps a value equal to the threshold in columns 0-7 when another Dest cell isn't 0
                        TTSIM_VERIFY((apply_relu != 2) || p_config->PCK_DEST_RD_CTRL_Read_32b_data,
                            UnsupportedFunctionality, "bf16 to fp16 apply_relu=%d", apply_relu);
                        uint32_t threshold = p_config->STACC_RELU_ReluThreshold << 3;
                        if (value & 0x40000) {
                            value = 0;
                        }
                        if ((apply_relu == 2) && (value <= threshold)) {
                            value = 0;
                        }
                        if ((apply_relu == 3) && (value > threshold)) {
                            value = threshold;
                        }
                    } else
#endif
                    if ((intermediate_format == 1) || (intermediate_format == 2) || (intermediate_format == 5) || (intermediate_format == 6)) {
                        // the first half of each group of datums (columns 0-7 on Blackhole, 0-3 and 8-11 on Wormhole)
                        // keeps an fp16 value equal to the threshold whose low 3 mantissa bits aren't 0
#if TT_ARCH_VERSION == 1
                        bool first_half = col < 8;
#else
                        bool first_half = !(col & 4);
#endif
                        bool keep_equal = (intermediate_format == 1) && first_half &&
                            (p_config->STACC_RELU_ReluThreshold & 7);
                        if (value & 0x8000) {
                            value = 0;
                        }
                        if ((apply_relu == 2) && (value <= p_config->STACC_RELU_ReluThreshold) &&
                            !((value == p_config->STACC_RELU_ReluThreshold) && keep_equal)) {
                            value = 0;
                        }
                        if ((apply_relu == 3) && (value > p_config->STACC_RELU_ReluThreshold)) {
                            value = p_config->STACC_RELU_ReluThreshold;
                        }
                    } else if (intermediate_format == 8) {
                        TTSIM_VERIFY(apply_relu == 1, UnsupportedFunctionality,
                            "int32 relu apply_relu=%d", apply_relu);
                        if (value & 0x80000000) {
                            value = 0;
                        }
                    } else { // intermediate_format=0
                        uint32_t threshold = p_config->STACC_RELU_ReluThreshold << 16;
                        if (value & 0x80000000) {
                            value = 0;
                        }
                        if ((apply_relu == 2) && (value <= threshold)) {
                            value = 0;
                        }
                        if ((apply_relu == 3) && ((value >> 13) > (threshold >> 13))) {
                            value = threshold;
                        }
                    }
                }
#if TT_ARCH_VERSION == 1
                if ((intermediate_format == 1) && p_config->PCK_DEST_RD_CTRL_Round_10b_mant) {
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
                } else if (((intermediate_format == 1) && (pack_dst_format == 5)) ||
                           ((intermediate_format == 2) && (pack_dst_format == 6))) { // fp16 -> bf16/bfp8 late conversion
                    uint32_t e = (value >> 10) & 0x1F;
                    uint32_t m = value & 0x3FF;
                    if (e == 0) {
                        value = 0; // flush denormal to +0
                    } else {
                        value = (value & 0x8000) | ((e + 112) << 7) | (m >> 3);
                    }
#if TT_ARCH_VERSION == 1
                } else if ((intermediate_format == 1) && (pack_dst_format == 10)) { // fp16 -> fp8 late conversion
                    TTSIM_VERIFY(p_config->THCON_SEC0_REG1_Pac_LF8_4b_exp, UnsupportedFunctionality, "fp16 to fp8 late conversion: Pac_LF8_4b_exp=0");
                    uint32_t s = (value >> 15) & 1;
                    uint32_t e = (value >> 10) & 0x1F;
                    uint32_t m = value & 0x3FF;
                    if (e < 8) { // e = 8 falls through, keeping m >> 7 under a zero exponent
                        value = s << 7; // flush denormal to zero (preserving sign)
                    } else if (e >= 24) {
                        value = (s << 7) | 0x7F;
                    } else {
                        value = (s << 7) | ((e - 8) << 3) | (m >> 7);
                    }
#endif
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
                } else if ((intermediate_format == 0) && (pack_dst_format == 5)) { // fp32 -> bf16 late conversion: truncate
                    value = (value & 0x7F800000) ? (value >> 16) : ((value >> 16) & 0x8000); // zero exponent: keep the sign
                } else if ((intermediate_format == 0) &&
                           ((pack_dst_format == 6) || (pack_dst_format == 7) || (pack_dst_format == 15))) { // fp32 -> bfp8/bfp4/bfp2 late conversion
                    value >>= 16;
                    if ((value & 0x7FFF) < 0x80) {
                        value = 0; // flush denormal to +0
                    }
                } else if (((intermediate_format == 5) || (intermediate_format == 6)) && (pack_dst_format == 0)) { // bf16/bfp8 -> fp32 late conversion
                    value = value << 16;
                }
            }
            uint32_t sram_addr_bits = (dst_addr << 3) + dst_element_size_bits*i;
            uint32_t sram_addr = sram_addr_bits >> 3;
            TTSIM_VERIFY(in_bounds(sram_addr_bits, dst_element_size_bits, TENSIX_SRAM_SIZE * 8), UndefinedBehavior, "out of bounds sram_addr=0x%x", sram_addr);
            if (dst_element_size_bits == 32) {
                if (pack_l1_acc) {
                    value = pack_l1_acc_fp32(mem_rd<uint32_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr]), value);
                }
                mem_wr<uint32_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr], value);
            } else if (dst_element_size_bits == 16) {
                if (pack_l1_acc) {
                    if (pack_dst_format == 5) {
                        value = pack_l1_acc_bf16(mem_rd<uint16_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr]), value);
                    } else {
                        value = pack_l1_acc_fp16(mem_rd<uint16_t>(&g_t_tiles[p_tensix->tile_id].sram[sram_addr]), value);
                    }
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
                        } else if (dst_element_size_bits == 2) {
                            uint32_t shift = sram_addr_element_bits & 6;
                            uint32_t mask = 3 << shift;
                            m = (m << shift) | (*p_byte & ~mask);
                        }
                        *p_byte = m;
                    }
                }
            }
        }
        p_tensix->packer_dst_addr[packer] = dst_addr + (dst_element_size_bits * count) / 8;
        p_tensix->packer_dst_exp_addr[packer] = dst_exp_addr + count/16;
#if TT_ARCH_VERSION == 1
        tpg_y += __builtin_popcount(read_intf_sel); // advance by a whole 16 datum row per enabled read interface
        if (tpg_y == pack_reads_per_xy_plane) {
            tpg_y = 0;
            tpg_z++;
        }
        tpg_y &= 255;
#endif
        p_tensix->packer_tpg_x[packer] = tpg_x;
        p_tensix->packer_tpg_y[packer] = tpg_y;
        p_tensix->packer_tpg_z[packer] = tpg_z;
        if (last || flush) { // zero-pad partial section tails to the next 16B boundary
            uint32_t data_end = p_tensix->packer_dst_addr[packer];
            uint32_t data_pad_end = (data_end + 15) & ~15u;
#if TT_ARCH_VERSION == 1
            // these narrowing late conversions write a partial last group of 128 input bytes as twice its input, up
            // to the whole group
            if (((pack_src_format == 0) && ((pack_dst_format == 1) || (pack_dst_format == 5))) ||
                ((pack_src_format == 1) && (pack_dst_format == 10)) || ((pack_src_format == 5) && is_bfp_format(pack_dst_format))) {
                uint32_t group = (128 * dst_element_size_bits) / src_element_size_bits;
                uint32_t tail = (data_end - p_tensix->packer_dst_data_start[packer]) % group;
                if (tail) {
                    data_pad_end = std::max(data_pad_end, data_end - tail + std::min(2 * tail, group));
                }
            }
            // a tile of part rows ending partway into 8 datums is completed by rules not modeled here
            uint32_t datums = ((data_end - p_tensix->packer_dst_data_start[packer]) * 8) / dst_element_size_bits;
            TTSIM_VERIFY(!(datums % 8), UnsupportedFunctionality, "tile of %d datums", datums);
            // a tile read in datums narrower than 32 bits ends on a whole 16 datum row: a half row is followed by zeros
            if (src_element_size_bits != 32) {
                if (datums % 16) {
                    data_pad_end = std::max(data_pad_end, data_end + ((16 - datums % 16) * dst_element_size_bits) / 8);
                }
            }
#else
            // a tile whose input ends partway into a 16-byte Dest word gets the rest of that word's datums as zeros
            // before the padding, which shows when the output format is wider than the input
            uint32_t datums = ((data_end - p_tensix->packer_dst_data_start[packer]) * 8) / dst_element_size_bits;
            uint32_t word_tail = (datums * src_element_align) % 16;
            TTSIM_VERIFY(!word_tail ||
                         (data_end + (((16 - word_tail) / src_element_align) * dst_element_size_bits) / 8 <= data_pad_end),
                UnsupportedFunctionality, "tile of %d datums ends inside a Dest word: pack_fmt_conv_mode=0x%x", datums,
                pack_fmt_conv_mode);
#endif
            if (data_end != data_pad_end) {
                TTSIM_VERIFY(data_pad_end <= TENSIX_SRAM_SIZE, UndefinedBehavior, "pad out of bounds data_pad_end=0x%x", data_pad_end);
                uint8_t *sram = g_t_tiles[p_tensix->tile_id].sram;
                if (!pack_l1_acc) {
                    for (uint32_t addr = data_end; addr < data_pad_end; addr++) {
                        sram[addr] = 0;
                    }
                } else if (dst_element_size_bits == 32) { // the padding is accumulated too, as +0
                    for (uint32_t addr = data_end; addr < data_pad_end; addr += 4) {
                        mem_wr<uint32_t>(&sram[addr], pack_l1_acc_fp32(mem_rd<uint32_t>(&sram[addr]), 0));
                    }
                } else {
                    for (uint32_t addr = data_end; addr < data_pad_end; addr += 2) {
                        uint16_t old = mem_rd<uint16_t>(&sram[addr]);
                        mem_wr<uint16_t>(&sram[addr], (pack_dst_format == 5) ? pack_l1_acc_bf16(old, 0) : pack_l1_acc_fp16(old, 0));
                    }
                }
            }
            if (is_bfp_format(pack_dst_format)) {
                uint32_t exp_end = p_tensix->packer_dst_exp_addr[packer];
                uint32_t exp_pad_end = (exp_end + 15) & ~15u;
                if (exp_end != exp_pad_end) {
                    TTSIM_VERIFY(exp_pad_end <= TENSIX_SRAM_SIZE, UndefinedBehavior, "pad out of bounds exp_pad_end=0x%x", exp_pad_end);
                    for (uint32_t addr = exp_end; addr < exp_pad_end; addr++) {
                        g_t_tiles[p_tensix->tile_id].sram[addr] = 0;
                    }
                }
            }
        }
    }
    for (uint32_t packer = 0; packer < n_packers; packer++) { // a packer the PACR didn't use keeps its tile open
        p_tensix->packer_valid[packer] = !last && !flush;
    }

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
            TTSIM_UNREACHABLE("addr_mode=%d", addr_mode);
    }
    TTSIM_VERIFY(!y1_cr, UnsupportedFunctionality, "y1_cr=%d", y1_cr);
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

#if TT_ARCH_VERSION == 1
    // A PACR with Last clear and an fp32 or int32 intermediate format that reads three or four Dest rows holds its
    // output, and the packer stays busy, until the next PACR
    bool held = !last && (__builtin_popcount(read_intf_sel) >= 3) &&
                ((intermediate_format == 0) || (intermediate_format == 8));
    p_tensix->pack_held_pipe = held ? pipe + 1 : 0;
#endif
    return true;
}

static inline vec_u32 bfp8_to_fp32(vec_u32 x, uint32_t exp_bits) {
    vec_u32 sign = (x >> 7) & 1;
    vec_u32 mag = (x << 1) & 0xFE;
    // an 8-bit CLZ, from mag's exponent as a float (exact); a zero's, kept in shift range, is unused
    vec_u32 lz = (134 - (std::bit_cast<vec_u32>(__builtin_convertvector(vec_i32(mag), vec_f32)) >> 23)) & 7;
    vec_u32 exp = (exp_bits - lz) & 255; // this will often wrap; seems odd, but spec/silicon are consistent here
    vec_u32 value = (sign << 31) | (exp << 23) | (((mag << lz) & 0x7E) << 16);
    return (mag != 0) ? value : ((vec_u32{} - sign) & 0xFF800000); // zero-magnitude encodings map to -inf and +0
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
    TTSIM_VERIFY(unpack_context <= 1, UnsupportedFunctionality, "unpack_context=%d", unpack_context);

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
#if TT_ARCH_VERSION == 1
    bool exponent_4b = unpack_block_selection ? p_config->THCON_SEC1_REG1_Unp_LF8_4b_exp
                                              : p_config->THCON_SEC0_REG1_Unp_LF8_4b_exp;
#else
    bool exponent_4b = false;
#endif
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

    // the instruction's address counter context (0) supplies X and Y, the issuing thread Z, W and channel 1 Y/Z/W
    TensixAddrCtrl *p_xy_ctrl = &p_tensix->addr_ctrl[0][unpack_block_selection];
    TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][unpack_block_selection];
    bool haloize = unpack_block_selection ? p_config->THCON_SEC1_REG2_Haloize_mode
                                          : p_config->THCON_SEC0_REG2_Haloize_mode;
    if (unpack_block_selection) {
        TTSIM_VERIFY(!haloize, NonContractualBehavior, "unpack1 does not support haloize");
    }
    bool tileize = unpack_block_selection ? p_config->THCON_SEC1_REG2_Tileize_mode
                                          : p_config->THCON_SEC0_REG2_Tileize_mode;
#if TT_ARCH_VERSION == 1
    const uint32_t unpack_row_shift = (in_element_size_bits <= 8) ? 4 : 5;
#else
    const uint32_t unpack_row_shift = 4;
#endif
    const uint32_t unpack_row_width = 1u << unpack_row_shift;
    uint32_t row_stride;
    if (tileize) {
        TTSIM_VERIFY(in_element_size_bits >= 8, UnsupportedFunctionality, "tileize incompatible with in_element_size_bits=%d", in_element_size_bits);
        if (unpack_block_selection) {
            TTSIM_ERROR(UnsupportedFunctionality, "tileize unpack1");
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
    // Note: ConfigDescriptor.DigestSize (cfg55/95, cfg67/115) is not instantiated and errors on write; always 0
    base_addr = (base_addr + (offset_addr & 0xFFFF) + 1) << 4;
    uint32_t in_addr_exponents = base_addr << 4; // slightly odd: byte address in .4 fixed point
    bool bfp_exp_stream = is_bfp_format(in_data_format); // Force_shared_exp is refused on write
    if (bfp_exp_stream) { // requires much more complex address computations and loading separate exponent stream
        TTSIM_VERIFY((!bits<5,5>(tile_desc0)), UnsupportedFunctionality, "no_bfp_exp_section");
        uint32_t num_elements = tile_x_dim * tile_y_dim * tile_z_dim * tile_w_dim; // below 2^32: X and Y * Z * W are 16 bits
        uint32_t num_exponents = (num_elements + 15) / 16; // 1 exponent per 16 sign/magnitudes
        uint32_t exp_section_size = (num_exponents + 15) / 16; // always make exponents a multiple of 16B
        // the hardware counts the section's datums in 24 bits and its 16B units in 16, each wrapping
        TTSIM_VERIFY(exp_section_size <= 0xFFFF, UnsupportedFunctionality, "bfp exponent section of 0x%x datums", num_elements);
        base_addr += exp_section_size << 4;
    }
    uint32_t first_datum = p_xy_ctrl->ch0_x +
                           (p_xy_ctrl->ch0_y & 0xFF) * ch0_y_stride +
                           (p_addr_ctrl->ch0_z & 0xFF) * ch0_z_stride +
                           (p_addr_ctrl->ch0_w & 0xFF) * ch0_w_stride;
    if (bfp_exp_stream) {
        TTSIM_VERIFY(!(first_datum & 15), UnsupportedFunctionality, "bfp_exp_stream: misaligned first_datum=0x%x", first_datum);
    }
    in_addr_exponents += first_datum; // 1/16 of a byte per datum
    uint32_t src_addr_bits = (base_addr << 3) + first_datum * in_element_size_bits;
    if (haloize || tileize) {
        TTSIM_VERIFY(!(src_addr_bits & 127), UnsupportedFunctionality, "misaligned src_addr_bits=0x%x", src_addr_bits);
    }

    // Note: HW erratum (Blackhole): an UNPACR into SrcA that starts while an unpack to Dst on the same unpacker is still
    // writing can also write one or two of that unpack's last Dst rows into SrcA (row = Dst row mod 64); not modeled
    bool unpack_to_dst = false;
    if (!unpack_block_selection) {
        if (ovrd_thread_id) {
            unpack_to_dst = unpack_context ? p_config->THCON_SEC0_REG2_Unpack_if_sel_cntx1 : p_config->THCON_SEC0_REG2_Unpack_if_sel_cntx0;
        } else {
            unpack_to_dst = p_config->THCON_SEC0_REG2_Unpack_If_Sel;
        }
    }
    if (unpack_to_dst) {
        TTSIM_VERIFY(!haloize, UnsupportedFunctionality, "unpack_to_dst cannot be used with haloize");
    }
    if (!unpack_block_selection) {
        p_tensix->src_a_wr_to_dst = unpack_to_dst;
    }

    // Note: UNP0/1_ADDR_BASE_REG_1_Base (cfg40/48, cfg49/61) is not instantiated and errors on write; always 0
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

    uint32_t col_shift = 0;
    if (!tileize && !unpack_block_selection) {
        col_shift = unpack_context ? p_config->THCON_SEC0_REG2_Shift_amount_cntx1 :
                                     p_config->THCON_SEC0_REG2_Shift_amount_cntx0;
    }
    TTSIM_VERIFY(!col_shift, UnsupportedFunctionality, "col_shift=%d", col_shift);

    uint32_t x_limit = 65536 / std::max(8u, in_element_size_bits);
    TTSIM_VERIFY(p_xy_ctrl->ch1_x < x_limit, UnsupportedFunctionality, "ch1_x=%d exceeds x_limit=%d", p_xy_ctrl->ch1_x, x_limit);
    TTSIM_VERIFY(p_xy_ctrl->ch0_x <= p_xy_ctrl->ch1_x, UndefinedBehavior, "invalid ch0_x=%d ch1_x=%d", p_xy_ctrl->ch0_x, p_xy_ctrl->ch1_x);
    uint32_t count = p_xy_ctrl->ch1_x - p_xy_ctrl->ch0_x + 1;
    if (bfp_exp_stream) {
        TTSIM_VERIFY(in_bounds(first_datum, count, 65536), UnsupportedFunctionality,
            "bfp exponent index out of range: first_datum=%d count=%d", first_datum, count);
        // BFP4/BFP2 datums may start mid-word only if they also end in it; otherwise silicon garbles them
        TTSIM_VERIFY(!(src_addr_bits & 127) || ((src_addr_bits & 127) + count * in_element_size_bits <= 128), UndefinedBehavior,
            "bfp_exp_stream: misaligned src_addr_bits=0x%x count=%d", src_addr_bits, count);
    }
    // BFP2 unpacks in pairs of datums: an odd count also writes the position after the last datum
    TTSIM_VERIFY(!((in_element_size_bits == 2) && (count & 1)), UnsupportedFunctionality,
        "bfp2 with an odd count=%d", count);
    if (unpack_to_dst) {
        // Note: HW erratum: SetOvrdWithAddr applies at each Dst write, so a SETC16 that sets it while this unpack is still
        // writing sends its remaining datums to row & ~0x30; not modeled
        // Note: HW erratum (Wormhole and Blackhole): these Dst writes share the Matrix Unit's write path, so one in the
        // same cycle as a Matrix Unit result write, in either half, merges with it (ORed row and data); not modeled
        TTSIM_VERIFY(!p_tensix->thread[pipe].SRCA_SET_SetOvrdWithAddr, UnsupportedFunctionality, "unpack_to_dst: SRCA_SET_SetOvrdWithAddr=1");
    } else {
        TTSIM_VERIFY(p_tensix->thread[pipe].SRCA_SET_SetOvrdWithAddr, UnsupportedFunctionality, "!unpack_to_dst: SRCA_SET_SetOvrdWithAddr=0");
        unpack_src_written(p_tensix, pipe, unpack_block_selection, src_bank);
    }

    uint32_t throttle_mode = unpack_block_selection ? p_config->THCON_SEC1_REG2_Throttle_mode
                                                    : p_config->THCON_SEC0_REG2_Throttle_mode;
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(throttle_mode <= 2, UnsupportedFunctionality, "invalid throttle_mode=%d", throttle_mode);
    // At x4, a BFP4 run that starts within a group of 16 exponents must end within it: crossing into the next group
    // gives datums past the boundary wrong exponents
    TTSIM_VERIFY(!(bfp_exp_stream && (in_element_size_bits == 4) && (throttle_mode == 2) && (first_datum & 255) &&
                   (first_datum + count > ((first_datum + 255) & ~255u))),
        UndefinedBehavior, "bfp_exp_stream: bfp4 first_datum=%d count=%d crosses an exponent group", first_datum, count);
#elif TT_ARCH_VERSION == 1
    // Forced x8 works only for 8-bit (and BFP2, which always runs at x1) input: BFP4 hangs, 16- and 32-bit input
    // loses half its rows, and FP32 converted to FP16 or BF16 hangs
    TTSIM_VERIFY(!(p_config->THCON_SEC0_REG1_ovrd_default_throttle_mode && (throttle_mode == 3) &&
                   ((in_element_size_bits == 4) || (in_element_size_bits >= 16))),
        UndefinedBehavior, "throttle_mode=%d with in_data_format=%d", throttle_mode, in_data_format);
    // At x4 (BFP4's default), a BFP4 run that starts within a group of 16 exponents must end within it: crossing
    // into the next group gives datums past the boundary wrong exponents
    bool bfp4_x4 = bfp_exp_stream && (in_element_size_bits == 4) &&
                   !(p_config->THCON_SEC0_REG1_ovrd_default_throttle_mode && (throttle_mode < 2));
    TTSIM_VERIFY(!(bfp4_x4 && (first_datum & 255) && (first_datum + count > ((first_datum + 255) & ~255u))),
        UndefinedBehavior, "bfp_exp_stream: bfp4 first_datum=%d count=%d crosses an exponent group", first_datum, count);
#endif
    // check for SrcA burst drop cases, and on Blackhole SrcB's
    if (!unpack_to_dst && (!unpack_block_selection || (TT_ARCH_VERSION == 1)) && (count > ROW_SIZE)) {
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
            "src_%c start_row=%d burst_rows=%d may span 16-row set boundary and drop writes", 'a' + unpack_block_selection,
            start_row, burst_rows);
        // Datums that don't start at a 16-byte L1 word reach Src in groups that straddle rows, and a group that
        // straddles a 16-row set boundary loses its datums past the boundary
        uint32_t end_row = (dst_addr + count - 1) / ROW_SIZE;
        TTSIM_VERIFY(!((src_addr_bits & 127) && ((start_row ^ end_row) & ~15u)), UndefinedBehavior,
            "src_%c datums from a misaligned src_addr_bits=0x%x span 16-row set boundary at rows %d..%d and drop writes",
            'a' + unpack_block_selection, src_addr_bits, start_row, end_row);
    }
    // Datums that don't start at a 16-byte L1 word reach Src shifted, so the last L1 read group's datums may not fit
    // in its Src write; the rest go out in one more write after the instruction is done. With SetDatValid that write
    // comes after the bank switch: those datums are missing from the bank marked valid, and the unpacker waits until
    // the other bank is free.
    if (set_dat_valid && !unpack_to_dst && (src_addr_bits & 127) && (in_element_size_bits >= 8)) {
#if TT_ARCH_VERSION == 0
        uint32_t group_bytes = (throttle_mode == 2) ? 64 : 0; // groups of 16 or 32 bytes always fit
#elif TT_ARCH_VERSION == 1
        uint32_t group_throttle = p_config->THCON_SEC0_REG1_ovrd_default_throttle_mode ? throttle_mode :
                                  (in_element_size_bits == 8) ? 3 : 2;
        uint32_t group_bytes = (group_throttle < 2) ? 0 : (in_element_size_bits == 8) ? 64 : 128;
#endif
        uint32_t in_bytes = in_element_size_bits / 8, out_bytes = out_element_size_bits / 8;
        uint32_t start_byte = (src_addr_bits & 127) / 8;
        uint32_t end_byte = start_byte + count * in_bytes;
        if (group_bytes && (end_byte > group_bytes)) {
            uint32_t last_group_bytes = (end_byte - 1) % group_bytes + 1;
            uint32_t shift = (16 - start_byte * out_bytes / in_bytes) % 16;
            TTSIM_VERIFY(shift + last_group_bytes * out_bytes / in_bytes <= group_bytes, UndefinedBehavior,
                "set_dat_valid: datums from a misaligned src_addr_bits=0x%x count=%d end in a write after the bank switch",
                src_addr_bits, count);
        }
    }
#if TT_ARCH_VERSION == 1
    // At x4 or x8 (8-bit input's default), 8-bit datums that don't start at a 16-byte L1 word and run past the 128
    // bytes from that word's start can take values left by the unpacker's previous unpack; nothing else is affected
    if (!unpack_to_dst && (in_element_size_bits == 8) && (src_addr_bits & 127)) {
        uint32_t group_throttle = p_config->THCON_SEC0_REG1_ovrd_default_throttle_mode ? throttle_mode : 3;
        TTSIM_VERIFY(!((group_throttle >= 2) && ((src_addr_bits & 127) / 8 + count > 128)), UnpredictableValue,
            "datums from a misaligned src_addr_bits=0x%x count=%d at throttle_mode=%d may take the previous unpack's values",
            src_addr_bits, count, group_throttle);
    }
#endif

    // a row at a time: its 16 datums are contiguous in L1
    const uint8_t *sram = g_t_tiles[p_tensix->tile_id].sram;
    uint8_t partial_row[4 * ROW_SIZE + 1] = {}; // the last row's datums when it has fewer than 16
    for (uint32_t i = 0; i < count; i += ROW_SIZE) {
        uint32_t n = std::min(count - i, uint32_t(ROW_SIZE));
        uint32_t sram_addr_bits = src_addr_bits + in_element_size_bits*(i & (unpack_row_width - 1)) + row_stride*8*(i >> unpack_row_shift);
        uint32_t sram_addr = sram_addr_bits >> 3;
        TTSIM_VERIFY(in_bounds(sram_addr_bits, in_element_size_bits * n, TENSIX_SRAM_SIZE * 8), UndefinedBehavior, "out of bounds sram_addr=0x%x", sram_addr);
        // past a partial row's end, zeros: converted, never written
        const uint8_t *row_src = &sram[sram_addr];
        if (n < ROW_SIZE) {
            memcpy(partial_row, row_src, ((sram_addr_bits & 7) + in_element_size_bits * n + 7) / 8);
            row_src = partial_row;
        }
        uint32_t value[ROW_SIZE];
        if (in_element_size_bits == 32) {
            for (uint32_t c = 0; c < ROW_SIZE; c++) {
                value[c] = mem_rd<uint32_t>(&row_src[4 * c]);
            }
        } else if (in_element_size_bits == 16) {
            for (uint32_t c = 0; c < ROW_SIZE; c++) {
                value[c] = mem_rd<uint16_t>(&row_src[2 * c]);
            }
        } else if (in_element_size_bits == 8) {
            for (uint32_t c = 0; c < ROW_SIZE; c++) {
                value[c] = row_src[c];
            }
        } else {
            for (uint32_t c = 0; c < ROW_SIZE; c++) {
                uint32_t bits = (sram_addr_bits & 7) + in_element_size_bits * c;
                value[c] = (row_src[bits >> 3] >> (bits & 7)) & ((1u << in_element_size_bits) - 1);
            }
        }
        if (is_bfp_format(in_data_format)) {
            uint32_t exp_addr = (in_addr_exponents + i) / 16; // one exponent per row, as first_datum is a multiple of 16
            TTSIM_VERIFY(exp_addr < TENSIX_SRAM_SIZE, UndefinedBehavior, "out of bounds exp_addr=0x%x", exp_addr);
            uint8_t exp_bits = sram[exp_addr];
            TTSIM_VERIFY(!unpack_to_dst, UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
            uint32_t shift; // to bfp8
            if (in_data_format == 6) {
                shift = 0;
            } else if (in_data_format == 7) {
                shift = 4;
            } else if (in_data_format == 15) {
                shift = 6;
            } else {
                TTSIM_ERROR(UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
            }
            for (uint32_t c = 0; c < ROW_SIZE; c += sizeof(vec_u32) / sizeof(uint32_t)) {
                vec_store(&value[c], bfp8_to_fp32(vec_load<vec_u32>(&value[c]) << shift, exp_bits));
            }
        } else if (in_data_format == 0) { // fp32
            if (out_data_format == 0) { // fp32
                TTSIM_VERIFY(unpack_to_dst, UndefinedBehavior, "unpack_to_dst=%d in_data_format=%d out_data_format=%d",
                    unpack_to_dst, in_data_format, out_data_format);
                for (uint32_t c = 0; c < ROW_SIZE; c++) {
                    value[c] = dst_encode_fp32(value[c]);
                }
            } else if (out_data_format == 1) { // fp16
                TTSIM_VERIFY(!unpack_to_dst, UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d out_data_format=%d",
                    unpack_to_dst, in_data_format, out_data_format);
                for (uint32_t c = 0; c < ROW_SIZE; c++) {
                    uint32_t s = value[c] & 0x80000000;
                    int32_t e = ((value[c] >> 23) & 255) - 112;
                    if (e < 0) {
                        value[c] = s;
                    } else if (e > 31) {
                        value[c] = s | (31 << 23) | 0x7FE000;
                    } else {
                        value[c] = s | (e << 23) | (value[c] & 0x7FE000);
                    }
                }
            } else if (out_data_format == 4) { // tf32
                if (unpack_to_dst) {
                    for (uint32_t c = 0; c < ROW_SIZE; c++) {
                        value[c] = dst_encode_fp32(value[c]);
                    }
                } else {
                    for (uint32_t c = 0; c < ROW_SIZE; c++) {
                        value[c] &= 0xFFFFE000; // drop low 13 bits of mantissa (no rounding)
                    }
                }
            } else if (out_data_format == 5) { // bf16
                TTSIM_VERIFY(!unpack_to_dst, UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d out_data_format=%d",
                    unpack_to_dst, in_data_format, out_data_format);
                for (uint32_t c = 0; c < ROW_SIZE; c++) {
                    value[c] = denormals_as_zeros(value[c]);
                    value[c] &= 0xFFFF0000; // drop low 16 bits of mantissa (no rounding)
                }
            } else {
                TTSIM_UNREACHABLE("unpack_to_dst=%d in_data_format=%d out_data_format=%d", unpack_to_dst, in_data_format, out_data_format);
            }
        } else { // non-bfp, non-fp32
            if (in_data_format == 1) {
                TTSIM_VERIFY(!unpack_to_dst, UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                for (uint32_t c = 0; c < ROW_SIZE; c++) {
                    value[c] = ((value[c] & 0x8000) << 16) | ((value[c] & 0x7FFF) << 13);
                }
            } else if (in_data_format == 5) {
#if TT_ARCH_VERSION == 0
                TTSIM_VERIFY(!unpack_to_dst, UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
#endif
                if (unpack_to_dst) {
                    for (uint32_t c = 0; c < ROW_SIZE; c++) {
                        value[c] = dst_encode_bf16(value[c]);
                    }
                } else {
                    for (uint32_t c = 0; c < ROW_SIZE; c++) {
                        value[c] <<= 16; // bf16 -> fp32 in Src
                    }
                }
            } else if (in_data_format == 8) {
                TTSIM_VERIFY(unpack_to_dst, UndefinedBehavior, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                for (uint32_t c = 0; c < ROW_SIZE; c++) {
                    value[c] = dst_encode_fp32(value[c]);
                }
            } else if (in_data_format == 9) {
                TTSIM_VERIFY(!unpack_to_dst, UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                for (uint32_t c = 0; c < ROW_SIZE; c++) {
                    value[c] = uint32_t(dst_decode_bf16(value[c])) << 16;
                }
            } else if (in_data_format == 10) {
                TTSIM_VERIFY(!unpack_to_dst, UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                for (uint32_t c = 0; c < ROW_SIZE; c++) {
                    if (exponent_4b) { // S1E4M3
                        uint32_t s = value[c] & 0x80;
                        uint32_t e = (value[c] >> 3) & 15;
                        uint32_t m = value[c] & 7;
                        uint32_t m_pad = ((e == 15) && (m == 7)) ? 0x7F : 0;
                        e = e ? (e + 8) : 0;
                        value[c] = (s << 8) | (e << 10) | (m << 7) | m_pad;
#if TT_ARCH_VERSION == 1
                    } else if ((value[c] & 0x7F) == 0x7F) {
                        value[c] = (value[c] << 8) | 0xFF; // BH-specific special case for exponent and mantissa all 1s
#endif
                    } else { // S1E5M2
                        value[c] <<= 8;
                    }
                    value[c] = ((value[c] & 0x8000) << 16) | ((value[c] & 0x7FFF) << 13);
                }
            } else if (in_data_format == 14) {
                TTSIM_VERIFY(!unpack_to_dst, UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
                bool is_unsigned = unpack_block_selection ? p_config->ALU_FORMAT_SPEC_REG0_SrcBUnsigned
                                                          : p_config->ALU_FORMAT_SPEC_REG0_SrcAUnsigned;
                for (uint32_t c = 0; c < ROW_SIZE; c++) {
                    uint32_t sign = is_unsigned ? 0 : (value[c] & 0x80);
                    value[c] -= sign;
                    if (value[c]) {
                        value[c] |= 16 << 10;
                    }
                    value[c] |= sign << 8;
                    value[c] = ((value[c] & 0x8000) << 16) | ((value[c] & 0x7FFF) << 13);
                }
            } else {
                TTSIM_ERROR(UnsupportedFunctionality, "unpack_to_dst=%d in_data_format=%d", unpack_to_dst, in_data_format);
            }
        }
        if (zero_write2) {
            TTSIM_ERROR(UntestedFunctionality, "zero_write2=%d", zero_write2);
        }
        uint32_t row = (dst_addr + i) / ROW_SIZE;
        uint32_t *src_row = nullptr;
        if (unpack_to_dst) {
            TTSIM_VERIFY(row < DST_ROWS, UnsupportedFunctionality, "dst row=%d", row);
            for (uint32_t c = 0; c < n; c++) {
                if (out_element_size_bits == 32) {
                    write_dst32b(p_tensix, row, c, value[c]);
                } else {
                    write_dst16b(p_tensix, row, c, value[c]);
                }
            }
        } else if (unpack_block_selection) {
            TTSIM_VERIFY(row < SRC_ROWS, UnsupportedFunctionality, "src_b row=%d", row);
            src_row = p_tensix->src_b[src_bank][row];
        } else {
#if TT_ARCH_VERSION == 1
            row &= SRC_ROWS - 1;
#else
            TTSIM_VERIFY(row < SRC_ROWS, UnsupportedFunctionality, "src_a row=%d", row);
#endif
            if (haloize) {
                for (uint32_t c = 0; c < n; c++) {
                    p_tensix->src_a[src_bank][(row & ~15) | c][row & 15] = value[c];
                }
            } else {
                src_row = p_tensix->src_a[src_bank][row];
            }
        }
        if (src_row) {
            if (n < ROW_SIZE) { // the rest of a partial row keeps its datums
                memcpy(&value[n], &src_row[n], 4 * (ROW_SIZE - n));
            }
            memcpy(src_row, value, sizeof(value));
        }
    }

    auto increment = [=](TensixAddrCtrl *p) {
        p->ch0_z = (p->ch0_z + bits<1,0>(addr_mode)) & ADC_Z_MASK;
        p->ch0_y = (p->ch0_y + bits<3,2>(addr_mode)) & ADC_Y_MASK;
        p->ch1_z = (p->ch1_z + bits<5,4>(addr_mode)) & ADC_Z_MASK;
        p->ch1_y = (p->ch1_y + bits<7,6>(addr_mode)) & ADC_Y_MASK;
    };
    increment(p_addr_ctrl);
    if (pipe) {
        increment(p_xy_ctrl); // the context's counters advance too
    }

    if (set_dat_valid) {
        if (unpack_block_selection) {
            TTSIM_VERIFY(!(p_tensix->src_b_valid & (1 << src_bank)), NonContractualBehavior, "SrcB bank is already valid");
            p_tensix->src_b_valid |= 1 << src_bank;
            p_tensix->src_b_unpack_bank ^= 1;
            // Note: SRCB_SET_Base (thread_cfg4/6) is not instantiated and errors on write; always 0
        } else {
            TTSIM_VERIFY(!(p_tensix->src_a_valid & (1 << src_bank)), NonContractualBehavior, "SrcA bank is already valid");
            TTSIM_VERIFY(!p_tensix->src_a_wr_to_dst, UndefinedBehavior, "SrcA set valid after unpack to Dst (HW erratum TEN-3868)");
            p_tensix->src_a_valid |= 1 << src_bank;
            p_tensix->src_a_unpack_bank ^= 1;
            TTSIM_VERIFY(!p_tensix->thread[pipe].SRCA_SET_Base, UnsupportedFunctionality, "SRCA_SET_Base");
        }
#if TT_ARCH_VERSION >= 1
        if (unpack_block_selection) {
            p_tensix->src_b_format[src_bank] = out_data_format;
        } else {
            p_tensix->src_a_format[src_bank] = out_data_format;
        }
#endif
    }
    return true;
}

TENSIX_EXECUTE_UNPACR_NOP() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(unpack_pop == 1, UnsupportedFunctionality, "unpack_pop=%d", unpack_pop);
    TTSIM_VERIFY(src_clr_val_ctrl <= 1, UnsupportedFunctionality, "src_clr_val_ctrl=%d", src_clr_val_ctrl);
    TTSIM_VERIFY(!bank_clr_ctrl, UnsupportedFunctionality, "bank_clr_ctrl=%d", bank_clr_ctrl);
    TTSIM_VERIFY(!clr_to1_fmt_ctrl, UnsupportedFunctionality, "clr_to1_fmt_ctrl=%d", clr_to1_fmt_ctrl);
    TTSIM_VERIFY(!msg_clr_cnt, UnsupportedFunctionality, "msg_clr_cnt=%d", msg_clr_cnt);
    TTSIM_VERIFY(!stream_id, UnsupportedFunctionality, "stream_id=%d", stream_id);
    uint32_t unpack_block_selection = unpacker_select; // this field was renamed
    uint32_t stall_and_clear = 1; // implied by unpack_pop == 1

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
#else
    TTSIM_VERIFY((no_op == 1) || (no_op == 5) || (no_op == 7) || (no_op == 17) || (no_op == 21),
        UnsupportedFunctionality, "no_op=%d", no_op);
    uint32_t stall_and_clear = (no_op != 7);
    uint32_t src_clr_val_ctrl = bits<2,2>(no_op); // clear to negative inf
    uint32_t stall_clr_cntrl = bits<4,4>(no_op); // called wait_like_unpacr in tt-isa-documentation
    uint32_t set_dvalid = (no_op == 7);
    // Note: a clear in the same cycle as a ZEROSRC with SingleBankMatrixUnit or BothBanks clears the ZEROSRC's bank
    // choice instead of the unpack bank, and the two share the SrcA clear value; not modeled (timing)
#endif

    uint32_t unpack_bank = unpack_block_selection ? p_tensix->src_b_unpack_bank : p_tensix->src_a_unpack_bank;
    if (stall_and_clear) {
        uint32_t matrix_bank = unpack_block_selection ? p_tensix->src_b_matrix_bank : p_tensix->src_a_matrix_bank;
        uint32_t src_valid = unpack_block_selection ? p_tensix->src_b_valid : p_tensix->src_a_valid;
        uint32_t wait_bank = stall_clr_cntrl ? unpack_bank : matrix_bank;
        if (src_valid & (1 << wait_bank)) {
            return false; // stall until not valid
        }
        unpack_src_written(p_tensix, pipe, unpack_block_selection, unpack_bank);

        if (unpack_block_selection) {
#if TT_ARCH_VERSION == 1
            TTSIM_VERIFY(!src_clr_val_ctrl, UnsupportedFunctionality, "unpack_block_selection=%d src_clr_val_ctrl=%d", unpack_block_selection, src_clr_val_ctrl);
#else
            TTSIM_VERIFY(!src_clr_val_ctrl, NonContractualBehavior, "unpack_block_selection=%d src_clr_val_ctrl=%d", unpack_block_selection, src_clr_val_ctrl);
#endif
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
            // Note: SRCB_SET_Base (thread_cfg4/6) is not instantiated and errors on write; always 0
        } else {
            TTSIM_VERIFY(!(p_tensix->src_a_valid & (1 << unpack_bank)), NonContractualBehavior, "SrcA bank is already valid");
#if TT_ARCH_VERSION == 0
            TTSIM_VERIFY(!p_tensix->src_a_wr_to_dst, UndefinedBehavior, "SrcA set valid after unpack to Dst (HW erratum TEN-3868)");
#endif
            p_tensix->src_a_valid |= 1 << unpack_bank;
            p_tensix->src_a_unpack_bank ^= 1;
            TTSIM_VERIFY(!p_tensix->thread[pipe].SRCA_SET_Base, UnsupportedFunctionality, "SRCA_SET_Base");
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

TENSIX_EXECUTE_SETGPR() {
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
    tensix_cfg_wr32(p_tensix, state_id, 4 * (flop_index + 52), p_tensix->dma_regs[pipe][reg_index]);
    return true;
#endif
}

TENSIX_EXECUTE_SETADC() {
    TTSIM_VERIFY(value <= 0xFFFF, UnsupportedFunctionality, "value=0x%x (ThreadOverride unsupported)", value);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            switch (dimension_index | (channel_index << 2)) {
                case 0: p_addr_ctrl->ch0_x_cr = p_addr_ctrl->ch0_x = value & ADC_X_MASK; break;
                case 1: p_addr_ctrl->ch0_y_cr = p_addr_ctrl->ch0_y = value & ADC_Y_MASK; break;
                case 2: p_addr_ctrl->ch0_z_cr = p_addr_ctrl->ch0_z = value & ADC_Z_MASK; break;
                case 3: p_addr_ctrl->ch0_w_cr = p_addr_ctrl->ch0_w = value & ADC_W_MASK; break;
                case 4: p_addr_ctrl->ch1_x_cr = p_addr_ctrl->ch1_x = value & ADC_X_MASK; break;
                case 5: p_addr_ctrl->ch1_y_cr = p_addr_ctrl->ch1_y = value & ADC_Y_MASK; break;
                case 6: p_addr_ctrl->ch1_z_cr = p_addr_ctrl->ch1_z = value & ADC_Z_MASK; break;
                case 7: p_addr_ctrl->ch1_w_cr = p_addr_ctrl->ch1_w = value & ADC_W_MASK; break;
                default: TTSIM_UNREACHABLE("dimension_index=%d channel_index=%d", dimension_index, channel_index);
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_SETADCXY() {
    TTSIM_VERIFY(!thread_override, UnsupportedFunctionality, "thread_override=%d", thread_override);
    TTSIM_VERIFY(bit_mask, UnsupportedFunctionality, "no-op mask: bit_mask=%d", bit_mask);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);

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
    TTSIM_VERIFY(!thread_override, UnsupportedFunctionality, "thread_override=%d", thread_override);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);

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
    TTSIM_VERIFY(!thread_override, UnsupportedFunctionality, "thread_override=%d", thread_override);
    TTSIM_VERIFY(bit_mask, UnsupportedFunctionality, "no-op mask: bit_mask=%d", bit_mask);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            if (bit_mask & 1) {
                p_addr_ctrl->ch0_x_cr = (p_addr_ctrl->ch0_x_cr + ch0_x) & ADC_X_MASK;
                p_addr_ctrl->ch0_x = p_addr_ctrl->ch0_x_cr;
            }
            if (bit_mask & 2) {
                p_addr_ctrl->ch0_y_cr = (p_addr_ctrl->ch0_y_cr + ch0_y) & ADC_Y_MASK;
                p_addr_ctrl->ch0_y = p_addr_ctrl->ch0_y_cr;
            }
            if (bit_mask & 4) {
                p_addr_ctrl->ch1_x_cr = (p_addr_ctrl->ch1_x_cr + ch1_x) & ADC_X_MASK;
                p_addr_ctrl->ch1_x = p_addr_ctrl->ch1_x_cr;
            }
            if (bit_mask & 8) {
                p_addr_ctrl->ch1_y_cr = (p_addr_ctrl->ch1_y_cr + ch1_y) & ADC_Y_MASK;
                p_addr_ctrl->ch1_y = p_addr_ctrl->ch1_y_cr;
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_SETADCZW() {
    TTSIM_VERIFY(!thread_override, UnsupportedFunctionality, "thread_override=%d", thread_override);
    TTSIM_VERIFY(bit_mask, UnsupportedFunctionality, "no-op mask: bit_mask=%d", bit_mask);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            if (bit_mask & 1) {
                p_addr_ctrl->ch0_z_cr = p_addr_ctrl->ch0_z = ch0_z;
            }
            if (bit_mask & 2) {
                p_addr_ctrl->ch0_w_cr = p_addr_ctrl->ch0_w = ch0_w;
            }
            if (bit_mask & 4) {
                p_addr_ctrl->ch1_z_cr = p_addr_ctrl->ch1_z = ch1_z;
            }
            if (bit_mask & 8) {
                p_addr_ctrl->ch1_w_cr = p_addr_ctrl->ch1_w = ch1_w;
            }
        }
    }
    return true;
}

TENSIX_EXECUTE_INCADCZW() {
    TTSIM_VERIFY(!thread_override, UnsupportedFunctionality, "thread_override=%d", thread_override);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            p_addr_ctrl->ch0_z = (p_addr_ctrl->ch0_z + ch0_z) & ADC_Z_MASK;
            p_addr_ctrl->ch0_w = (p_addr_ctrl->ch0_w + ch0_w) & ADC_W_MASK;
            p_addr_ctrl->ch1_z = (p_addr_ctrl->ch1_z + ch1_z) & ADC_Z_MASK;
            p_addr_ctrl->ch1_w = (p_addr_ctrl->ch1_w + ch1_w) & ADC_W_MASK;
        }
    }
    return true;
}

TENSIX_EXECUTE_ADDRCRZW() {
    TTSIM_VERIFY(!thread_override, UnsupportedFunctionality, "thread_override=%d", thread_override);
    TTSIM_VERIFY(bit_mask, UnsupportedFunctionality, "no-op mask: bit_mask=%d", bit_mask);
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            if (bit_mask & 1) {
                p_addr_ctrl->ch0_z_cr = (p_addr_ctrl->ch0_z_cr + ch0_z) & ADC_Z_MASK;
                p_addr_ctrl->ch0_z = p_addr_ctrl->ch0_z_cr;
            }
            if (bit_mask & 2) {
                p_addr_ctrl->ch0_w_cr = (p_addr_ctrl->ch0_w_cr + ch0_w) & ADC_W_MASK;
                p_addr_ctrl->ch0_w = p_addr_ctrl->ch0_w_cr;
            }
            if (bit_mask & 4) {
                p_addr_ctrl->ch1_z_cr = (p_addr_ctrl->ch1_z_cr + ch1_z) & ADC_Z_MASK;
                p_addr_ctrl->ch1_z = p_addr_ctrl->ch1_z_cr;
            }
            if (bit_mask & 8) {
                p_addr_ctrl->ch1_w_cr = (p_addr_ctrl->ch1_w_cr + ch1_w) & ADC_W_MASK;
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
        TTSIM_VERIFY(!p_tensix->src_a_wr_to_dst, UndefinedBehavior, "SrcA set valid after unpack to Dst (HW erratum TEN-3868)");
        p_tensix->src_a_valid |= 1 << unpack_bank;
        p_tensix->src_a_unpack_bank = unpack_bank ^ 1;
        TTSIM_VERIFY(!p_tensix->thread[pipe].SRCA_SET_Base, UnsupportedFunctionality, "SRCA_SET_Base");
    }
    if (setvalid & 2) {
        uint32_t unpack_bank = p_tensix->src_b_unpack_bank;
        TTSIM_VERIFY(!(p_tensix->src_b_valid & (1 << unpack_bank)), NonContractualBehavior, "SrcB bank is already valid");
        p_tensix->src_b_valid |= 1 << unpack_bank;
        p_tensix->src_b_unpack_bank = unpack_bank ^ 1;
        // Note: SRCB_SET_Base (thread_cfg4/6) is not instantiated and errors on write; always 0
    }
    return true;
#else
    TTSIM_ERROR(UnsupportedFunctionality, "interaction between SETDVALID and implied src format is ill-specified (use UNPACR_NOP instead)");
#endif
}

TENSIX_EXECUTE_ADDGPR() {
    TTSIM_ASSERT(op_a_reg_index < std::size(p_tensix->dma_regs[pipe]), "op_a_reg_index=%d out of range", op_a_reg_index);
    TTSIM_ASSERT(op_b_reg_index < std::size(p_tensix->dma_regs[pipe]), "op_b_reg_index=%d out of range", op_b_reg_index);
    TTSIM_ASSERT(result_reg_index < std::size(p_tensix->dma_regs[pipe]), "result_reg_index=%d out of range", result_reg_index);
    TTSIM_VERIFY(!op_b_is_const, UnsupportedFunctionality, "op_b_is_const=%d", op_b_is_const);

    p_tensix->dma_regs[pipe][result_reg_index] = p_tensix->dma_regs[pipe][op_a_reg_index] + p_tensix->dma_regs[pipe][op_b_reg_index];
    return true;
}

TENSIX_EXECUTE_MULGPR() {
    TTSIM_ASSERT(op_a_reg_index < std::size(p_tensix->dma_regs[pipe]), "op_a_reg_index=%d out of range", op_a_reg_index);
    TTSIM_ASSERT(op_b_reg_index < std::size(p_tensix->dma_regs[pipe]), "op_b_reg_index=%d out of range", op_b_reg_index);
    TTSIM_ASSERT(result_reg_index < std::size(p_tensix->dma_regs[pipe]), "result_reg_index=%d out of range", result_reg_index);
    TTSIM_VERIFY(!op_b_is_const, UnsupportedFunctionality, "op_b_is_const=%d", op_b_is_const);

    uint32_t src0 = p_tensix->dma_regs[pipe][op_a_reg_index] & 0xFFFF;
    uint32_t src1 = p_tensix->dma_regs[pipe][op_b_reg_index] & 0xFFFF;
    p_tensix->dma_regs[pipe][result_reg_index] = src0 * src1;
    return true;
}

TENSIX_EXECUTE_SETADCXX() {
    TTSIM_VERIFY(cnt_set_mask, UnsupportedFunctionality, "no-op mask: cnt_set_mask=%d", cnt_set_mask);

    for (uint32_t i = 0; i < 3; i++) {
        if (cnt_set_mask & (1 << i)) {
            TensixAddrCtrl *p_addr_ctrl = &p_tensix->addr_ctrl[pipe][i];
            p_addr_ctrl->ch0_x = p_addr_ctrl->ch0_x_cr = x_start;
            p_addr_ctrl->ch1_x = p_addr_ctrl->ch1_x_cr = x_end2;
        }
    }
    return true;
}

TENSIX_EXECUTE_DMANOP() {
    return true;
}

TENSIX_EXECUTE_STOREREG() {
    TTSIM_ASSERT(tdma_data_reg_index < std::size(p_tensix->dma_regs[pipe]), "tdma_data_reg_index=%d out of range", tdma_data_reg_index);
    uint32_t addr = 0xFFB00000 | (reg_addr << 2);

    // Be careful here to avoid infinite recursion by injecting instructions into pipe
    // For safety, we only allow overlay registers to be written via this path
    switch (addr) {
        case NOC_OVERLAY_BASE ... NOC_OVERLAY_LIMIT: {
            bool done = tile_mmio_wr32('T', p_tensix->tile_id, 0xFFFFFFFF, addr, p_tensix->dma_regs[pipe][tdma_data_reg_index]); // no riscv_id in this path
            TTSIM_ASSERT(done, "tile_mmio_wr32 failed: addr=0x%x", addr); // needs to be nonblocking
            break;
        }
        default:
            TTSIM_ERROR(UnsupportedFunctionality, "disallowed addr=0x%x", addr);
    }
    return true;
}

template<typename Func>
static inline void for_each_lane(uint32_t mask, Func f) {
    for (; mask; mask &= mask - 1) {
        uint32_t lane = __builtin_ctz(mask);
        f(lane);
    }
}

TENSIX_EXECUTE_SFPLOAD() {
    TTSIM_VERIFY(!(dest_reg_addr & 1), UnsupportedFunctionality, "dest_reg_addr=%d", dest_reg_addr);
    TTSIM_VERIFY(lreg_ind < 12, UnsupportedFunctionality, "lreg_ind=%d", lreg_ind);
    // Not checked: on Wormhole, an SFPLOAD with fewer than three instructions between it and a Matrix Unit instruction
    // that writes Dest isn't ordered against that write, so it can read the old data
    TTSIM_VERIFY(!instr_mod0 || (instr_mod0 == 1) || (instr_mod0 == 2) || (instr_mod0 == 3) || (instr_mod0 == 4) ||
                 (instr_mod0 == 6) || (instr_mod0 == 12) || (instr_mod0 == 14) || (instr_mod0 == 15),
        UnsupportedFunctionality, "instr_mod0=%d", instr_mod0);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    if (!instr_mod0) {
        if (p_config->ALU_ACC_CTRL_SFPU_Fp32_enabled) {
            instr_mod0 = 3;
        } else {
            uint32_t src_b_fmt = math_srcb_cfg_format(p_config);
            if ((src_b_fmt == 0) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7) || // fp32, bf16, bfp8, bfp4
                (src_b_fmt == 9) || (src_b_fmt == 15)) { // int16, bfp2
                instr_mod0 = 2;
            } else {
                TTSIM_VERIFY(src_b_fmt == 1, UnsupportedFunctionality, "src_b_fmt=%d", src_b_fmt); // fp16
                instr_mod0 = 1;
            }
        }
    }

    uint32_t cell_bank = SFPU_CELL_DEST;
    uint32_t dst_row;
    {
        // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
        dst_row = math_dst_rwc(p_tensix, pipe) + dest_reg_addr + math_dest_offset(p_tensix, p_config, pipe);
        dst_row &= DST_ROWS - 1;
    }
    TTSIM_VERIFY(!(dst_row & 1), UnsupportedFunctionality, "dst_row=%d", dst_row);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t row = (dst_row & ~3) + lane / 8;
        uint32_t col = 2*(lane & 7) + ((dst_row & 2) >> 1);
        uint32_t value;
        if (instr_mod0 == 1) {
            value = sfpu_read_cell16(p_tensix, cell_bank, row, col);
            uint32_t s = value >> 15;
            uint32_t e = value & 31;
            uint32_t m = (value >> 5) & 1023;
            // Note: ENABLE_FP16A_INF errors on write in SFPCONFIG; always 0
            if (e) {
                e += 112;
            }
            value = (s << 31) | (e << 23) | (m << 13);
        } else if (instr_mod0 == 2) {
            value = dst_decode_bf16(sfpu_read_cell16(p_tensix, cell_bank, row, col));
            value <<= 16;
        } else if ((instr_mod0 == 3) || (instr_mod0 == 4)) {
            value = dst_decode_fp32(sfpu_read_cell32(p_tensix, cell_bank, row, col));
        } else if (instr_mod0 == 6) {
            value = sfpu_read_cell16(p_tensix, cell_bank, row, col);
        } else
        if (instr_mod0 == 12) {
            value = dst_decode_fp32(sfpu_read_cell32(p_tensix, cell_bank, row, col));
#if TT_ARCH_VERSION == 0
            if (value & 0x80000000) {
                value = -int32_t(value & 0x7FFFFFFF);
            }
#endif
        } else if (instr_mod0 == 14) {
            value = (p_tensix->l_regs[lreg_ind][lane] & 0xFFFF0000) | uint32_t(sfpu_read_cell16(p_tensix, cell_bank, row, col));
        } else
        if (instr_mod0 == 15) {
            value = (uint32_t(sfpu_read_cell16(p_tensix, cell_bank, row, col)) << 16) | (p_tensix->l_regs[lreg_ind][lane] & 0xFFFF);
        } else {
            TTSIM_UNREACHABLE("instr_mod0=%d", instr_mod0);
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
        default: TTSIM_ERROR(NonContractualBehavior, "instr_mod0=%d", instr_mod0);
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
            TTSIM_UNREACHABLE("instr_mod0=%d", instr_mod0);
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
    TTSIM_VERIFY(!(dest_reg_addr & 1), UnsupportedFunctionality, "dest_reg_addr=%d", dest_reg_addr);
    TTSIM_VERIFY(lreg_ind < 12, UnsupportedFunctionality, "lreg_ind=%d", lreg_ind); // note some of the constant LRegs can be stored
    // Not checked: on Blackhole, a Matrix Unit instruction issued right after an SFPSTORE that the SFPMAD interlock
    // holds can reach Dest first, so a move from Dest reads the old row and a write to it is overwritten
    // Note: HW erratum (WH): an SFPSTORE whose Dest write lands in the same cycle as a Matrix Unit result write (ELW*,
    // MVMUL, DOTPV, pools) in the same half drops that result write, all of it when every lane stores, whatever rows
    // either targets; from one thread, that is an SFPSTORE three cycles after the Matrix Unit instruction (e.g. ELWADD,
    // two NOPs, SFPSTORE). On BH a plain SFPSTORE never meets one; a store SFPLOADMACRO schedules can. Not modeled
    TTSIM_VERIFY(!instr_mod0 || (instr_mod0 == 1) || (instr_mod0 == 2) || (instr_mod0 == 3) || (instr_mod0 == 4) || (instr_mod0 == 6) ||
                 (instr_mod0 == 7) || (instr_mod0 == 9) || (instr_mod0 == 12) || (instr_mod0 == 14) || (instr_mod0 == 15),
        UnsupportedFunctionality, "instr_mod0=%d", instr_mod0);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    const TensixConfigState *p_config = &p_tensix->config[state_id];
    if (!instr_mod0) {
        if (p_config->ALU_ACC_CTRL_SFPU_Fp32_enabled) {
            instr_mod0 = 3;
        } else {
            uint32_t src_b_fmt = math_srcb_cfg_format(p_config);
            if ((src_b_fmt == 0) || (src_b_fmt == 5) || (src_b_fmt == 6) || (src_b_fmt == 7) || // fp32, bf16, bfp8, bfp4
                (src_b_fmt == 9) || (src_b_fmt == 15)) { // int16, bfp2
                instr_mod0 = 2;
            } else {
                TTSIM_VERIFY(src_b_fmt == 1, UnsupportedFunctionality, "src_b_fmt=%d", src_b_fmt); // fp16
                instr_mod0 = 1;
            }
        }
    }

    uint32_t cell_bank = SFPU_CELL_DEST;
    uint32_t dst_row;
    {
        // Note: DEST_REGW_BASE_Base (cfg6) is not instantiated and errors on write; always 0
        dst_row = math_dst_rwc(p_tensix, pipe) + dest_reg_addr + math_dest_offset(p_tensix, p_config, pipe);
        dst_row &= DST_ROWS - 1;
    }
    TTSIM_VERIFY(!(dst_row & 1), UnsupportedFunctionality, "dst_row=%d", dst_row);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t row = (dst_row & ~3) + lane / 8;
        uint32_t col = 2*(lane & 7) + ((dst_row & 2) >> 1);
        uint32_t value = p_tensix->l_regs[lreg_ind][lane];
        if (instr_mod0 == 1) {
            sfpu_write_cell16(p_tensix, cell_bank, row, col, dst_encode_fp16(sfpu_store_to_fp16(value)));
        } else if (instr_mod0 == 2) {
            value = denormals_as_zeros(value);
            sfpu_write_cell16(p_tensix, cell_bank, row, col, dst_encode_bf16(value >> 16));
        } else if (instr_mod0 == 3) {
            value = denormals_as_zeros(value);
            sfpu_write_cell32(p_tensix, cell_bank, row, col, dst_encode_fp32(value));
        } else if (instr_mod0 == 4) {
            sfpu_write_cell32(p_tensix, cell_bank, row, col, dst_encode_fp32(value));
        } else if ((instr_mod0 == 6) || (instr_mod0 == 14)) {
            sfpu_write_cell16(p_tensix, cell_bank, row, col, value & 0xFFFF);
        } else if (instr_mod0 == 7) {
            sfpu_write_cell32(p_tensix, cell_bank, row, col, value);
        } else if (instr_mod0 == 9) {
            sfpu_write_cell32(p_tensix, cell_bank, row, col, (value << 16) | (value >> 16));
        } else if (instr_mod0 == 12) {
#if TT_ARCH_VERSION == 0
            if (value & 0x80000000) {
                value = 0x80000000 | uint32_t(-int32_t(value));
            }
#endif
            sfpu_write_cell32(p_tensix, cell_bank, row, col, dst_encode_fp32(value));
        } else if (instr_mod0 == 15) {
            sfpu_write_cell16(p_tensix, cell_bank, row, col, value >> 16);
        } else {
            TTSIM_UNREACHABLE("instr_mod0=%d", instr_mod0);
        }
    });
    // The store marks all four rows of its group written, the rows of lanes it leaves disabled too; their cells keep
    // their old contents
    bool is_32b = (instr_mod0 == 3) || (instr_mod0 == 4) || (instr_mod0 == 7) || (instr_mod0 == 9) || (instr_mod0 == 12);
    for (uint32_t row = dst_row & ~3u; row < (dst_row & ~3u) + 4; row++) {
        p_tensix->dst_row_valid[dst_valid_row(p_tensix, row, is_32b)] = true;
    }
    math_update_counters(p_tensix, pipe, sfpu_addr_mode, false);
    return true;
}

template<bool is_sfpmuli>
static uint32_t sfpu_mul(uint32_t a, uint32_t b) {
    a = denormals_as_zeros(a);
    b = denormals_as_zeros(b);
    uint32_t ret = std::bit_cast<uint32_t>(std::bit_cast<float>(a) * std::bit_cast<float>(b));
#if TT_ARCH_VERSION >= 1
    if ((ret & 0x7FFFFFFF) > 0x7F800000) {
        return 0x7FC00000; // replace all NaNs with a canonical NaN
    }
    ret = denormals_as_zeros(ret);
    uint32_t a_exp = (a >> 23) & 255;
    uint32_t b_exp = (b >> 23) & 255;
    if (!(ret & 0x7FFFFFFF) && !(a_exp && b_exp && ((a_exp + b_exp) >= 127))) {
        ret = 0; // a zero result keeps its sign only if the operand exponents summed into range
    }
#else
    uint32_t magnitude = ret & 0x7FFFFFFF;
    if ((magnitude <= 0x800000) || (magnitude > 0x7F800000)) {
        if (magnitude < 0x800000) {
            return 0; // always flush denormal to +0
        }
        // Round the exact 48-bit significand product at normal precision, even for underflow and NaNs.
        uint32_t a_exp = (a >> 23) & 255;
        uint32_t b_exp = (b >> 23) & 255;
        double a_man = a_exp ? ((a & 0x7FFFFF) | 0x800000) : 0;
        double b_man = b_exp ? ((b & 0x7FFFFF) | 0x800000) : 0;
        uint32_t man = std::bit_cast<uint32_t>(float(a_man * b_man));
        // 300 = two exponent biases plus the two 23-bit fraction shifts.
        int32_t exp = int32_t((man >> 23) & 255) + int32_t(a_exp) + int32_t(b_exp) - 300;
        if (magnitude > 0x7F800000) {
            return ((a ^ b) & 0x80000000) | 0x7F800001 | ((exp < 255) ? (man & 0x7FFFFF) : 0);
        }
        if (exp < 1) {
            return 0;
        }
    }
#endif
    return ret;
}

static uint32_t sfpu_add(uint32_t a, uint32_t b) {
    a = denormals_as_zeros(a);
    b = denormals_as_zeros(b);
    uint32_t ret = std::bit_cast<uint32_t>(std::bit_cast<float>(a) + std::bit_cast<float>(b));
#if TT_ARCH_VERSION >= 1
    if ((ret & 0x7FFFFFFF) > 0x7F800000) {
        return 0x7FC00000; // replace all NaNs with a canonical NaN
    }
    return denormals_as_zeros(ret);
#else
    if ((ret & 0x7FFFFFFF) > 0x7F800000) {
        // WH NaNs leak the fraction of an addition that treats exponent 255 as finite exponent +128.
        // FP64 holds these extended-range operands exactly, and 53 >= 2 * 24 + 2 bits of precision make
        // the rounding back to FP32 exact; a sum beyond FP32 range becomes an infinity, leaking zero.
        uint64_t a64 = (a & 0x7FFFFFFF) ? (uint64_t(a & 0x7FFFFFFF) << 29) + (896ull << 52) : 0;
        uint64_t b64 = (b & 0x7FFFFFFF) ? (uint64_t(b & 0x7FFFFFFF) << 29) + (896ull << 52) : 0;
        a64 |= uint64_t(a & 0x80000000) << 32;
        b64 |= uint64_t(b & 0x80000000) << 32;
        uint32_t sum = std::bit_cast<uint32_t>(float(std::bit_cast<double>(a64) + std::bit_cast<double>(b64)));
        // Prefer the first NaN's sign, then the second's; opposite infinities use the first operand's sign.
        uint32_t nan = (a & 0x7FFFFFFF) > 0x7F800000 ? a : (b & 0x7FFFFFFF) > 0x7F800000 ? b : a;
        return (nan & 0x80000000) | 0x7F800001 | (sum & 0x7FFFFF);
    }
    return ((ret & 0x7FFFFFFF) < 0x800000) ? 0 : ret; // a zero result always loses its sign
#endif
}

uint32_t sfpu_mad(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t a_exp = (a >> 23) & 255;
    uint32_t b_exp = (b >> 23) & 255;
    uint32_t c_exp = (c >> 23) & 255;
    uint32_t product_sign = (a ^ b) & 0x80000000;
    uint32_t c_sign = c & 0x80000000;
    int32_t product_exp = int32_t(a_exp + b_exp) - 127;
#if TT_ARCH_VERSION == 0
    uint32_t nan_result = 0;
#endif
    if ((a_exp == 255) || (b_exp == 255) || (c_exp == 255) || (product_exp >= 255)) {
        bool invalid_product = ((a & 0x7FFFFFFF) > 0x7F800000) || ((b & 0x7FFFFFFF) > 0x7F800000) ||
                               ((a_exp == 255) && !b_exp) || ((b_exp == 255) && !a_exp);
#if TT_ARCH_VERSION >= 1
        bool product_inf = (a_exp == 255) || (b_exp == 255); // note absence of product_exp >= 255 term here
        if (invalid_product || ((c & 0x7FFFFFFF) > 0x7F800000) ||
            ((c_exp == 255) && product_inf && (c_sign != product_sign))) {
            return 0x7FC00000;
        }
        return (c_exp == 255) ? c : (product_sign | 0x7F800000);
#else
        if (invalid_product || (((c & 0x7FFFFFFF) == 0x7F800000) &&
            ((a_exp == 255) || (b_exp == 255) || (product_exp >= 255)) && (c_sign != product_sign))) {
            nan_result = product_sign | 0x7F800001;
        } else if ((c & 0x7FFFFFFF) > 0x7F800000) {
            nan_result = c_sign | 0x7F800001;
        } else {
            return (c_exp == 255) ? c : (product_sign | 0x7F800000);
        }
        product_exp = std::min(product_exp, 255);
#endif
    }

    uint32_t product_man = 0;
    if (!a_exp || !b_exp || (product_exp < 0)) {
#if TT_ARCH_VERSION >= 1
        return c_exp ? c : c_sign & product_sign;
#else
        if (!nan_result) {
            return c_exp ? c : 0;
        }
        product_exp = 0;
#endif
    } else {
        uint64_t product = uint64_t((a & 0x7FFFFF) | 0x800000) * ((b & 0x7FFFFF) | 0x800000);
        product_man = uint32_t(product >> 20) | ((product & 0xFFFFF) != 0);
    }
    uint32_t c_man = c_exp ? ((c & 0x7FFFFF) | 0x800000) << 3 : 0;
    int32_t aligned_exp = product_exp;
    if (product_exp < int32_t(c_exp)) {
        uint32_t shift = c_exp - product_exp;
        uint32_t original = product_man;
        product_man = (shift < 28) ? (product_man >> shift) : 0;
        if (product_man) {
            product_man |= ((product_man << shift) != original);
        }
        aligned_exp = c_exp;
    } else if (product_exp > int32_t(c_exp)) {
        uint32_t shift = product_exp - c_exp;
        uint32_t original = c_man;
        c_man = (shift < 27) ? (c_man >> shift) : 0;
        if (c_man) {
            c_man |= ((c_man << shift) != original);
        }
    }

    // Aligned magnitudes are below 2^28 and 2^27; their signed sum cannot overflow int32_t.
    int32_t sum = (product_sign ? -int32_t(product_man) : int32_t(product_man)) +
        (c_sign ? -int32_t(c_man) : int32_t(c_man));
    if (sum == 0) {
#if TT_ARCH_VERSION >= 1
        return product_sign & c_sign;
#else
        return nan_result;
#endif
    }
#if TT_ARCH_VERSION >= 1
    // sum * 2^(aligned_exp - 153), exactly: the adjusted FP64 exponent stays in its normal range.
    uint64_t result64 = std::bit_cast<uint64_t>(double(sum)) + ((uint64_t(aligned_exp) - 153) << 52);
    uint32_t ret = std::bit_cast<uint32_t>(float(std::bit_cast<double>(result64)));
    return (ret & 0x7F800000) ? ret : (ret & 0x80000000);
#else
    uint32_t magnitude = uint32_t((sum < 0) ? -sum : sum);
    uint32_t rounded = std::bit_cast<uint32_t>(float(sum));
    uint32_t sign = rounded & 0x80000000;
    rounded &= 0x7FFFFFFF;
    // WH's two-bit normalization loses bit 1 instead of jamming it: an even tie can round down.
    if ((magnitude >= 0x10000000) && ((magnitude & 63) == 18)) {
        --rounded;
    }
    int32_t exponent = int32_t(rounded >> 23) + aligned_exp - 153;
    if (exponent >= 255) {
        return nan_result ? nan_result : sign | 0x7F800000;
    }
    if (exponent < 1) {
        return nan_result;
    }
    return (nan_result ? nan_result : sign) | (uint32_t(exponent) << 23) | (rounded & 0x7FFFFF);
#endif
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
        uint32_t i = (b < 0x3F800000) ? 0 : (b < 0x40000000) ? 1 : 2;
        uint32_t coeffs = p_tensix->l_regs[0 + i][lane];
        uint32_t a = lut8_to_fp32(bits<15,8>(coeffs));
        uint32_t c = lut8_to_fp32(bits<7,0>(coeffs));
        uint32_t d = sfpu_mad(a, b, c);
        if (instr_mod0 & 4) {
            d = (d & 0x7FFFFFFF) | (l3 & 0x80000000); // copy sign bit from l3
        }
        p_tensix->l_regs[lreg_ind][lane] = d;
    });
    return true;
}

TENSIX_EXECUTE_SFPMULI() {
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(!(instr_mod1 & 7), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!(instr_mod1 & 8), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#else
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!(instr_mod1 & 5), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!(instr_mod1 & 8), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#else
    TTSIM_VERIFY(!(instr_mod1 & 1), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!(instr_mod1 & 12), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#endif
    TTSIM_VERIFY(!(instr_mod1 & 2), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    uint32_t imm = imm16_math << 16;

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        p_tensix->l_regs[lreg_dest][lane] = sfpu_mul<true>(imm, p_tensix->l_regs[lreg_dest][lane]);
    });
    return true;
}

TENSIX_EXECUTE_SFPADDI() {
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    uint32_t imm = imm16_math << 16;

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        p_tensix->l_regs[lreg_dest][lane] = sfpu_add(imm, p_tensix->l_regs[lreg_dest][lane]);
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
    TTSIM_VERIFY((instr_mod1 <= 2) || (instr_mod1 == 10), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
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
        bool left = shift_amount >= 0;
        uint32_t amt = left ? uint32_t(shift_amount) : (0u - uint32_t(shift_amount));
        amt &= 31;
        if (left) {
            src <<= amt;
#if TT_ARCH_VERSION >= 1
        } else if (instr_mod1 & 2) {
            src = int32_t(src) >> amt;
#endif
        } else {
            src >>= amt;
        }
        p_tensix->l_regs[lreg_dest][lane] = src;
    });
    return true;
}

TENSIX_EXECUTE_SFPSETCC() {
    TTSIM_VERIFY(!(instr_mod1 & 1) && (instr_mod1 <= 6), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
    bool sign_magnitude = false;

    TTSIM_VERIFY(p_tensix->cc_en, UnsupportedFunctionality, "cc_en=%d", p_tensix->cc_en); // very strange behavior that clears all CC bits
    uint32_t mask = p_tensix->cc;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane];
        bool sign = (src >> 31) != 0;
        bool cc;
        if (instr_mod1 & 2) {
            cc = ((src & 0x7FFFFFFF) != 0) || (!sign_magnitude && sign);
        } else {
            cc = sign;
        }
        if (instr_mod1 & 4) {
            cc = !cc;
        }
        if (cc) {
            p_tensix->cc |= 1 << lane;
        } else {
            p_tensix->cc &= ~(1 << lane);
        }
    });
    return true;
}

TENSIX_EXECUTE_SFPMOV() {
    TTSIM_VERIFY(!(instr_mod1 & 4), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY((instr_mod1 <= 2) || (instr_mod1 == 8), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    if (instr_mod1 == 2) { // all lanes enabled
        mask = 0xFFFFFFFF;
    } else if (instr_mod1 == 8) {
        TTSIM_VERIFY(lreg_c == 9, UnsupportedFunctionality, "instr_mod1=%d lreg_c=%d", instr_mod1, lreg_c);
    }
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src;
        if (instr_mod1 == 8) {
            p_tensix->prng_state[lane] = step_prng(p_tensix->prng_state[lane]);
            src = p_tensix->prng_state[lane];
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
    TTSIM_VERIFY(!(instr_mod1 & 8), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t src = p_tensix->l_regs[lreg_c][lane];
        if (instr_mod1 & 4) {
            src &= 0x7FFFFFFF;
        }
        p_tensix->l_regs[lreg_dest][lane] = std::countl_zero(src);
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
    TTSIM_VERIFY(instr_mod1 <= 3, NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 <= 2, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_ASSERT(imm12_math <= 255, "imm12_math=%d", imm12_math);
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
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
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

// Not checked: on Wormhole, SFPMAD and the other two-cycle Vector Unit ops (SFPADD, SFPMUL, SFPADDI, SFPMULI, SFPLUT,
// SFPLUTFP32) have no interlock, so the Vector Unit instruction issued right after one must not read an LReg it writes
// (Blackhole stalls instead, except for the reads its interlock misses, such as SFPSWAP's compare and SFPIADD's VD).
// ttsim writes the result at once; on hardware the reader sees the old value.
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
        p_tensix->l_regs[lreg_dest][lane] = sfpu_mad(a, b, c);
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

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t b = p_tensix->l_regs[lreg_b][lane];
        uint32_t c = p_tensix->l_regs[lreg_c][lane];
#if TT_ARCH_VERSION >= 1
        if (instr_mod1 & 1) {
            b ^= 0x80000000;
        }
        if (instr_mod1 & 2) {
            c ^= 0x80000000;
        }
#endif
        p_tensix->l_regs[lreg_dest][lane] = sfpu_add(b, c);
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
        p_tensix->l_regs[lreg_dest][lane] = sfpu_mul<false>(a, b);
    });
    return true;
}

TENSIX_EXECUTE_SFPPUSHC() {
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    TTSIM_VERIFY(p_tensix->cc_sp < CC_STACK_SIZE, NonContractualBehavior, "CC stack overflow");
    p_tensix->cc_en_stack[p_tensix->cc_sp] = p_tensix->cc_en;
    p_tensix->cc_stack[p_tensix->cc_sp] = p_tensix->cc;
    p_tensix->cc_sp++;
    return true;
}

TENSIX_EXECUTE_SFPPOPC() {
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    TTSIM_VERIFY(p_tensix->cc_sp, NonContractualBehavior, "CC stack underflow");
    p_tensix->cc_sp--;
    p_tensix->cc_en = p_tensix->cc_en_stack[p_tensix->cc_sp];
    p_tensix->cc = p_tensix->cc_stack[p_tensix->cc_sp];
    return true;
}

TENSIX_EXECUTE_SFPSETSGN() {
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_ASSERT(imm12_math <= 1, "imm12_math=%d", imm12_math);
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
    TTSIM_VERIFY(!(instr_mod1 & 4), NonContractualBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    switch (instr_mod1) {
        case 0:
            TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "instr_mod1=%d imm12_math=%d", instr_mod1, imm12_math);
            p_tensix->cc = 0xFFFFFFFF;
            break;
        case 10:
            p_tensix->cc = ((imm12_math >> 1) & 1) ? 0xFFFFFFFF : 0;
            p_tensix->cc_en = imm12_math & 1;
            break;
        default:
            TTSIM_ERROR(UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    }
    return true;
}

TENSIX_EXECUTE_SFPCOMPC() {
    TTSIM_VERIFY(!lreg_dest, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    TTSIM_VERIFY(p_tensix->cc_en, UnsupportedFunctionality, "cc_en=%d", p_tensix->cc_en);
    if (p_tensix->cc_sp) {
        uint32_t top = p_tensix->cc_sp - 1;
        TTSIM_VERIFY(p_tensix->cc_en_stack[top], UnsupportedFunctionality, "cc_en_stack[top]=%d", p_tensix->cc_en_stack[top]);
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
    TTSIM_VERIFY(rnd_mode != 1, UnsupportedFunctionality, "stochastic rounding is explicitly out of scope");
    // UseImm5 (instr_mod1 bit 3) and the immediate are read only by the int32 to int8/uint8 flavors
    TTSIM_VERIFY((((instr_mod1 & 6) == 4)) || (!(instr_mod1 & 8) && !imm8_math), NonContractualBehavior,
        "instr_mod1=%d imm8_math=%d", instr_mod1, imm8_math);
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY((rnd_mode == 0) || (rnd_mode == 2), UnsupportedFunctionality, "rnd_mode=%d", rnd_mode);
#else
    TTSIM_VERIFY(!rnd_mode, UnsupportedFunctionality, "rnd_mode=%d", rnd_mode);
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(!imm8_math, UnsupportedFunctionality, "imm8_math=%d", imm8_math);
    TTSIM_VERIFY((instr_mod1 <= 7) || (instr_mod1 == 12) || (instr_mod1 == 13), UndefinedBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY((instr_mod1 == 1) || (instr_mod1 == 2) || (instr_mod1 == 3) || (instr_mod1 == 6) || (instr_mod1 == 7),
        UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    // XXX tighten this to lreg_src_b == 9
    TTSIM_VERIFY(!lreg_src_b || (lreg_src_b == 9), UnsupportedFunctionality, "lreg_src_b=%d", lreg_src_b);

    // The discarded bits round up when they are at least this
#if TT_ARCH_VERSION == 1
    // Note: HW erratum: round to zero rounds away from zero when the discarded bits are all ones
    uint32_t round_up_bits = (rnd_mode == 2) ? 0x7FFFFF : 0x400000;
#else
    constexpr uint32_t round_up_bits = 0x400000;
#endif
    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        p_tensix->prng_state[lane] = step_prng(p_tensix->prng_state[lane]); // always step PRNG, even if unused
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
                if ((discarded_bits << 7) >= (round_up_bits & ~0x7F)) { // the top 16 of its 23 bits
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
                mag = (mag >> 23) + ((mag & 0x7FFFFF) >= round_up_bits);
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

TENSIX_EXECUTE_SFPNOP() {
    return true;
}

TENSIX_EXECUTE_SFPCAST() {
    TTSIM_VERIFY(instr_mod1 != 1, UnsupportedFunctionality, "stochastic rounding is explicitly out of scope");
#if TT_ARCH_VERSION == 0
    TTSIM_VERIFY(instr_mod1 <= 1, NonContractualBehavior, "instr_mod1=%d", instr_mod1);
#elif TT_ARCH_VERSION == 1
    TTSIM_VERIFY(instr_mod1 <= 3, NonContractualBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 != 2, UnsupportedFunctionality, "instr_mod1=%d should be replaced by SFPABS", instr_mod1);
#else
    TTSIM_VERIFY((instr_mod1 & 6) != 6, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 <= 5, NonContractualBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 != 5, UnsupportedFunctionality, "stochastic rounding is explicitly out of scope");
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
            dst = sign | std::bit_cast<uint32_t>(float(int32_t(src & 0x7FFFFFFF))); // RTNE, keeping the sign of -0
        }
        p_tensix->l_regs[lreg_dest][lane] = dst;
    });
    return true;
}

TENSIX_EXECUTE_SFPCONFIG() {
    TTSIM_VERIFY((instr_mod1 <= 1) || ((TT_ARCH_VERSION == 1) && (instr_mod1 == 8) && (config_dest == 15)),
        UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    if (!instr_mod1) { // require all lanes of LReg[0] to be identical to one another
        if ((config_dest < 11) || (config_dest > 14)) { // ...with exception of cases where this is supported below
            for (uint32_t lane = 1; lane < 32; lane++) {
                TTSIM_VERIFY(p_tensix->l_regs[0][0] == p_tensix->l_regs[0][lane],
                    UnsupportedFunctionality, "config_dest=%d: l_regs[0]: lane[%d] mismatch with lane[0]", config_dest, lane);
            }
        }
        TTSIM_VERIFY(!imm16_math, UnsupportedFunctionality, "instr_mod1=%d imm16_math=0x%x", instr_mod1, imm16_math);
    }

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    TTSIM_VERIFY(mask == 0xFFFFFFFF, UnsupportedFunctionality, "not all lanes enabled: mask=0x%x", mask);
    switch (config_dest) {
        case 0 ... 3:
            TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "config_dest=%d instr_mod1=%d", config_dest, instr_mod1);
            break;
        case 4 ... 7: break;
        case 8:
            TTSIM_VERIFY(instr_mod1 == 1, UnsupportedFunctionality, "config_dest=%d instr_mod1=%d", config_dest, instr_mod1);
            TTSIM_VERIFY(imm16_math <= 0xFFF, UnsupportedFunctionality, "config_dest=%d imm16_math=0x%x", config_dest, imm16_math);
            break;
        case 9: case 10:
            TTSIM_ERROR(UnsupportedFunctionality, "config_dest=%d", config_dest);
        case 11 ... 14:
            TTSIM_VERIFY(!imm16_math, UnsupportedFunctionality, "config_dest=%d imm16_math=0x%x", config_dest, imm16_math);
            if (instr_mod1 & 1) { // write "default" immediate to LReg
                TTSIM_VERIFY(config_dest == 11, UnsupportedFunctionality, "config_dest=%d instr_mod1=%d", config_dest, instr_mod1);
                for (uint32_t lane = 0; lane < 32; lane++) {
                    p_tensix->l_regs[config_dest][lane] = 0xBF800000;
                }
            } else {
                for (uint32_t lane = 0; lane < 32; lane++) {
                    p_tensix->l_regs[config_dest][lane] = p_tensix->l_regs[0][lane & 7];
                }
            }
            break;
        case 15: { // LaneConfig
#if TT_ARCH_VERSION == 1
            if (instr_mod1 == 8) { // MOD1_IMM16_IS_LANE_MASK: only EXCHANGE_SRCB_SRCC may differ between SFPU instances
                TTSIM_VERIFY(!(imm16_math & 0xAAAA), UnsupportedFunctionality, "imm16_math=0x%x", imm16_math);
                for (uint32_t c = 0; c < 8; c++) { // instance c is written from LReg[0] lane c if imm16_math bit 2c is set
                    uint32_t value = p_tensix->l_regs[0][c];
                    if ((imm16_math >> (2 * c)) & 1) {
                        TTSIM_VERIFY(!((value ^ p_tensix->lane_config[c]) & ~0x100), UnsupportedFunctionality, "lane_config=0x%x", value);
                        p_tensix->lane_config[c] = value;
                    }
                }
                break;
            }
#endif
            uint32_t value = (instr_mod1 & 1) ? imm16_math : p_tensix->l_regs[0][0];
            TTSIM_VERIFY(!(value & ~0x104), UnsupportedFunctionality, "lane_config=0x%x", value);
            for (uint32_t c = 0; c < 8; c++) {
                p_tensix->lane_config[c] = value;
            }
            break;
        }
        default:
            TTSIM_UNREACHABLE("config_dest=%d", config_dest);
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
        default: TTSIM_ERROR(UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    }

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    // Note: HW erratum (WH only): a half-width write of VC or VD (SFPLOADI Mod0 8/10, SFPLOAD Mod0 14/15) as the very
    // next Vector Unit instruction also zeroes the half it should keep; not modeled
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t lane_config = p_tensix->lane_config[lane & 7];
        uint32_t c = p_tensix->l_regs[lreg_c][lane];
        uint32_t d = p_tensix->l_regs[lreg_dest][lane];
        bool should_swap = true;
        if (instr_mod1) {
            int32_t c_unpacked = sign_mag32_total_order(c);
            int32_t d_unpacked = sign_mag32_total_order(d);
            should_swap = (c_unpacked < d_unpacked) || ((c_unpacked == d_unpacked) && (c_unpacked < 0));
            if (!(vd_gets_min & (1 << lane))) {
                should_swap = !should_swap;
            }
            if (lane_config & 0x100) {
                should_swap = !should_swap;
            }
        }
        if (should_swap) {
            if (lane_config & 4) {
                if (lreg_c < 4) {
                    p_tensix->l_regs[lreg_c][lane] = d;
                }
                if (lreg_dest < 4) {
                    p_tensix->l_regs[lreg_dest][lane] = c;
                }
                uint32_t vc_a = 4 + (lreg_c & 3);
                uint32_t vd_a = 4 + (lreg_dest & 3);
                uint32_t vd_index = p_tensix->l_regs[vd_a][lane];
                if (lreg_dest < 8) { // VD's index is written only along with VD itself
                    p_tensix->l_regs[vd_a][lane] = p_tensix->l_regs[vc_a][lane];
                }
                p_tensix->l_regs[vc_a][lane] = vd_index;
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
    TTSIM_VERIFY(instr_mod1 <= 6, NonContractualBehavior, "instr_mod1=%d: shifts as 5", instr_mod1);
    TTSIM_VERIFY(instr_mod1 >= 3, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1); // allow 3..6
#if TT_ARCH_VERSION == 0
    // XXX Would be good to add a checker for the WH-only scheduling restrictions on some modes
    TTSIM_VERIFY(instr_mod1 != 4, UnsupportedFunctionality, "SUBVEC_SHFLSHR1 should not be used on Wormhole due to a hardware bug");
#else
    TTSIM_VERIFY(instr_mod1 != 6, NonContractualBehavior, "SHFT_IMM is only for SFPLOADMACRO templates on Blackhole; use SFPSHFT with ARG_IMM_USE_VC instead");
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    if (instr_mod1 == 5) { // imm12_math is used as a source register for this mode
        TTSIM_VERIFY(imm12_math < 16, UnsupportedFunctionality, "instr_mod1=%d imm12_math=%d", instr_mod1, imm12_math);
#if TT_ARCH_VERSION == 0
    } else if (instr_mod1 == 6) { // imm12_math is used as both a source register and a shift count for this mode
        TTSIM_VERIFY((imm12_math < 32) || (imm12_math > 0xFE0), UnsupportedFunctionality, "instr_mod1=%d imm12_math=%d", instr_mod1, imm12_math);
#endif
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
#if TT_ARCH_VERSION >= 1
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
#if TT_ARCH_VERSION == 0
        case 6:
            for_each_lane(mask, [=](uint32_t lane) {
                uint32_t src_b = p_tensix->l_regs[imm12_math & 15][lane];
                if (!(imm12_math & 0x800)) {
                    src_b <<= imm12_math & 31;
                } else {
                    src_b >>= (-imm12_math) & 31;
                }
                p_tensix->l_regs[lreg_dest][lane] = src_b;
            });
            break;
#endif
        default:
            TTSIM_UNREACHABLE("instr_mod1=%d", instr_mod1);
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
    // Mod1[0] is read only with Mod1[1] set and Mod1[3] clear
    TTSIM_VERIFY(!(instr_mod1 & 1) || ((instr_mod1 & 0xA) == 2), NonContractualBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY((instr_mod1 == 2) || (instr_mod1 == 6), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
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
        uint32_t a_word = p_tensix->l_regs[0 + i][lane];
        uint32_t c_word = p_tensix->l_regs[4 + i][lane];
        uint32_t a = lut16_to_fp32((a_word >> j) & 0xFFFF);
        uint32_t c = lut16_to_fp32((c_word >> j) & 0xFFFF);
        uint32_t d = sfpu_mad(a, b, c);
        if (instr_mod1 & 4) {
            d = (d & 0x7FFFFFFF) | (l3 & 0x80000000); // copy sign bit from l3
        }
        p_tensix->l_regs[lreg_dest][lane] = d;
    });
    return true;
}

#if TT_ARCH_VERSION >= 1
TENSIX_EXECUTE_SFPLE() {
    TTSIM_VERIFY((instr_mod1 == 1) || (instr_mod1 == 8), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    if (instr_mod1 == 8) {
        TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    }

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
}

TENSIX_EXECUTE_SFPGT() {
    TTSIM_VERIFY((instr_mod1 == 1) || (instr_mod1 == 8), UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    if (instr_mod1 == 8) {
        TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    }

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
}

TENSIX_EXECUTE_SFPMUL24() {
    TTSIM_VERIFY(!(instr_mod1 & 2), NonContractualBehavior, "reserved bit set in instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 <= 1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);
    TTSIM_VERIFY(lreg_src_c == 9, UnsupportedFunctionality, "lreg_src_c=%d", lreg_src_c);
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
}

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

TENSIX_EXECUTE_SFPARECIP() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(instr_mod1 <= 2, NonContractualBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!instr_mod1, UnsupportedFunctionality, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(!imm12_math, UnsupportedFunctionality, "imm12_math=%d", imm12_math);
#else
    TTSIM_VERIFY(instr_mod1 <= 5, NonContractualBehavior, "instr_mod1=%d", instr_mod1);
    TTSIM_VERIFY(instr_mod1 != 1, UnimplementedFunctionality, "instr_mod1=%d", instr_mod1);
#endif
    TTSIM_VERIFY(lreg_dest < 8, UnsupportedFunctionality, "lreg_dest=%d", lreg_dest);

    uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
    for_each_lane(mask, [=](uint32_t lane) {
        uint32_t x = p_tensix->l_regs[lreg_c][lane];
        {
            x = (x & 0x80000000) | approx_recip(x & 0x7FFFFFFF);
        }
        p_tensix->l_regs[lreg_dest][lane] = x;
    });
    return true;
}
#endif

TENSIX_EXECUTE_ATGETM() {
    TTSIM_VERIFY(mutex_index < std::size(p_tensix->mutex), UndefinedBehavior, "mutex_index=%d", mutex_index);
    TTSIM_VERIFY(mutex_index != 1, UndefinedBehavior, "mutex_index=%d", mutex_index); // mutex 1 is not valid, per the docs

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

    TTSIM_VERIFY(p_tensix->mutex[mutex_index] == pipe + 1, NonContractualBehavior, "mutex %d not acquired by this pipe", mutex_index);
    p_tensix->mutex[mutex_index] = 0;
    return true;
}

// Whether a latched wait with this block mask holds the instruction at the Wait Gate (STALLWAIT.md, "Block mask"). A
// later wait instruction waits for the latched one to go, and 0x1FF holds everything, NOPs included.
static bool block_mask_holds(uint32_t block, uint32_t inst) {
    if (block == 0x1FF) {
        return true;
    }
    uint32_t held_by;
    switch (bits<31,24>(inst)) {
        case TENSIX_OPCODE_NOP:
            held_by = 0;
            break;
        case TENSIX_OPCODE_ATGETM:
        case TENSIX_OPCODE_ATRELM:
        case TENSIX_OPCODE_SEMINIT:
        case TENSIX_OPCODE_SEMPOST:
        case TENSIX_OPCODE_SEMGET:
#if TT_ARCH_VERSION == 0
        case TENSIX_OPCODE_SEMWAIT:
#endif
            held_by = 0x002;
            break;
        case TENSIX_OPCODE_STALLWAIT:
#if TT_ARCH_VERSION == 1
        case TENSIX_OPCODE_SEMWAIT:
        case TENSIX_OPCODE_STREAMWAIT:
#endif
            return true;
        case 0x08 ... 0x3F:
            held_by = 0x040;
            break;
        case TENSIX_OPCODE_XMOV:
            held_by = 0x011;
            break;
        case TENSIX_OPCODE_PACR:
        case TENSIX_OPCODE_PACR_SETREG:
        case TENSIX_OPCODE_TBUFCMD:
            held_by = 0x005;
            break;
        case TENSIX_OPCODE_UNPACR:
        case TENSIX_OPCODE_UNPACR_NOP:
            held_by = 0x009;
            break;
        case TENSIX_OPCODE_SETGPR ... TENSIX_OPCODE_LOADIND:
        case TENSIX_OPCODE_ADDGPR ... TENSIX_OPCODE_CMPGPR:
        case TENSIX_OPCODE_DMANOP ... TENSIX_OPCODE_LOADREG:
            held_by = 0x021;
            break;
        case TENSIX_OPCODE_SFPLOAD ... 0x9F:
            held_by = 0x100;
            break;
        case 0xB0 ... 0xB9:
            held_by = 0x080;
            break;
        default:
            held_by = 0x001;
            break;
    }
    return held_by & block;
}

// XXX we ignore stall_res and currently block all downstream instructions from issuing, regardless of type
TENSIX_EXECUTE_STALLWAIT() {
    TTSIM_VERIFY(wait_res, UnsupportedFunctionality, "wait_res=0x%x", wait_res);
    TTSIM_VERIFY(stall_res, UnsupportedFunctionality, "stall_res=0x%x", stall_res);
    // Waiting for an unpacker waits for this thread's writes to Src through it
    if (wait_res & 0x2) {
        p_tensix->src_unpacked[0][pipe] = 0;
    }
    if (wait_res & 0x4) {
        p_tensix->src_unpacked[1][pipe] = 0;
    }
#if TT_ARCH_VERSION == 0
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
        math_src_waited(p_tensix, false);
        wait_res &= ~0x400;
    }
    if (wait_res & 0x800) {
        if (!(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank))) {
            return false; // stall until SrcB valid
        }
        math_src_waited(p_tensix, true);
        wait_res &= ~0x800;
    }
#else
    TTSIM_VERIFY(!((wait_res & 0x8) && (p_tensix->pack_held_pipe == pipe + 1)), UndefinedBehavior,
        "wait_res=0x%x waits on a packer that holds a non-last PACR's output, which never finishes", wait_res);
    wait_res &= ~0x1C1F; // never need to wait for ThCon memory requests, Unpack0/1, Pack, FPU, RISCV MMIO, SFPU, or Config
    if (wait_res & 0x20) {
        if (p_tensix->src_a_valid & (1 << p_tensix->src_a_unpack_bank)) {
            return false; // stall until SrcA not valid
        }
        wait_res &= ~0x20;
    }
    if (wait_res & 0x80) {
        if (!(p_tensix->src_a_valid & (1 << p_tensix->src_a_matrix_bank))) {
            return false; // stall until SrcA valid
        }
        math_src_waited(p_tensix, false);
        wait_res &= ~0x80;
    }
    if (wait_res & 0x100) {
        if (!(p_tensix->src_b_valid & (1 << p_tensix->src_b_matrix_bank))) {
            return false; // stall until SrcB valid
        }
        math_src_waited(p_tensix, true);
        wait_res &= ~0x100;
    }
#endif
    TTSIM_VERIFY(!wait_res, UnsupportedFunctionality, "wait_res=0x%x", wait_res);
    p_tensix->wait_block[pipe] = stall_res;
    p_tensix->wait_sems[pipe] = 0;
    return true;
}

TENSIX_EXECUTE_SEMINIT() {
    TTSIM_VERIFY(__builtin_popcount(sem_sel) == 1, UnsupportedFunctionality, "sem_sel=0x%x is not one-hot", sem_sel);
    uint32_t sem_index = __builtin_ctz(sem_sel);
    TTSIM_ASSERT(sem_index < std::size(p_tensix->sem), "sem_index=%d out of range", sem_index);
    // A SEMWAIT on this semaphore, block mask without B1, may still be latched: on Wormhole and Blackhole it is
    // evaluated before this change, in program order (likewise in SEMPOST and SEMGET)
    TTSIM_VERIFY(!(p_tensix->wait_block[pipe] && (sem_sel & p_tensix->wait_sems[pipe])), NonContractualBehavior,
        "sem_sel=0x%x: after this thread's SEMWAIT on it, whose block mask 0x%x lacks B1", sem_sel, p_tensix->wait_block[pipe]);
    TTSIM_VERIFY(init_value <= max_value, NonContractualBehavior, "init_value=%d is greater than max_value=%d", init_value, max_value);

    p_tensix->sem[sem_index] = init_value;
    p_tensix->sem_max[sem_index] = max_value;
    return true;
}

TENSIX_EXECUTE_SEMPOST() {
    TTSIM_VERIFY(__builtin_popcount(sem_sel) == 1, UnsupportedFunctionality, "sem_sel=0x%x is not one-hot", sem_sel);
    uint32_t sem_index = __builtin_ctz(sem_sel);
    TTSIM_ASSERT(sem_index < std::size(p_tensix->sem), "sem_index=%d out of range", sem_index);
    TTSIM_VERIFY(!(p_tensix->wait_block[pipe] && (sem_sel & p_tensix->wait_sems[pipe])), NonContractualBehavior,
        "sem_sel=0x%x: after this thread's SEMWAIT on it, whose block mask 0x%x lacks B1", sem_sel, p_tensix->wait_block[pipe]);

    TTSIM_VERIFY(p_tensix->sem[sem_index] < p_tensix->sem_max[sem_index], NonContractualBehavior,
        "sem=%d sem_max=%d", p_tensix->sem[sem_index], p_tensix->sem_max[sem_index]);
    p_tensix->sem[sem_index]++;
    return true;
}

TENSIX_EXECUTE_SEMGET() {
    TTSIM_VERIFY(__builtin_popcount(sem_sel) == 1, UnsupportedFunctionality, "sem_sel=0x%x is not one-hot", sem_sel);
    uint32_t sem_index = __builtin_ctz(sem_sel);
    TTSIM_ASSERT(sem_index < std::size(p_tensix->sem), "sem_index=%d out of range", sem_index);
    TTSIM_VERIFY(!(p_tensix->wait_block[pipe] && (sem_sel & p_tensix->wait_sems[pipe])), NonContractualBehavior,
        "sem_sel=0x%x: after this thread's SEMWAIT on it, whose block mask 0x%x lacks B1", sem_sel, p_tensix->wait_block[pipe]);

    TTSIM_VERIFY(p_tensix->sem[sem_index], NonContractualBehavior, "sem=%d sem_max=%d", p_tensix->sem[sem_index], p_tensix->sem_max[sem_index]);
    p_tensix->sem[sem_index]--;
    return true;
}

TENSIX_EXECUTE_SEMWAIT() {
    TTSIM_VERIFY(__builtin_popcount(sem_sel) == 1, UnsupportedFunctionality, "sem_sel=0x%x is not one-hot", sem_sel);
    // XXX the thread stops here whatever stall_res says
    uint32_t block = stall_res ? stall_res : 0x040;
#if TT_ARCH_VERSION == 0
    // Replacing a latched wait with an identical one loses nothing
    TTSIM_VERIFY(!p_tensix->wait_block[pipe] || ((p_tensix->wait_block[pipe] == block) &&
                                                 (p_tensix->wait_sems[pipe] == sem_sel) &&
                                                 (p_tensix->wait_sem_cond[pipe] == wait_sem_cond)),
        UndefinedBehavior, "may replace this thread's latched wait, whose block mask 0x%x lacks B1", p_tensix->wait_block[pipe]);
#endif
    TTSIM_VERIFY(wait_sem_cond, NonContractualBehavior, "wait_sem_cond=%d waits as STALLWAIT", wait_sem_cond);
    TTSIM_VERIFY((wait_sem_cond == 1) || (wait_sem_cond == 2), UnsupportedFunctionality, "wait_sem_cond=%d", wait_sem_cond);

    for (uint32_t sems = sem_sel; sems; sems &= sems - 1) {
        uint32_t sem_index = __builtin_ctz(sems);
        TTSIM_ASSERT(sem_index < std::size(p_tensix->sem), "sem_index=%d out of range", sem_index);
        if (wait_sem_cond == 2) { // wait while max
            if (p_tensix->sem[sem_index] >= p_tensix->sem_max[sem_index]) {
                return false;
            }
        } else { // wait while zero
            if (!p_tensix->sem[sem_index]) {
                return false;
            }
        }
    }
    p_tensix->wait_block[pipe] = block;
    p_tensix->wait_sems[pipe] = sem_sel;
#if TT_ARCH_VERSION == 0
    p_tensix->wait_sem_cond[pipe] = wait_sem_cond;
#endif
    return true;
}

// Not checked: the instruction right after a WRCFG must not consume what it writes (e.g. a PACR right after a WRCFG of
// its L1 destination address packs to the previous destination on Blackhole). Nor that a WRCFG (or RISC store) of the
// packer's edge masks or ReLU configuration waits for earlier PACRs to leave the packer: the hardware reads those as a
// PACR's data goes through, so the write can change an earlier PACR's output, where ttsim uses the values at the PACR
// (other packer fields, e.g. the L1 address, are taken when the packer accepts the PACR).
TENSIX_EXECUTE_WRCFG() {
    TTSIM_VERIFY(cfg_reg < 4 * TENSIX_CFG_STATE_SIZE, NonContractualBehavior, "cfg_reg=%d writes nothing", cfg_reg);
    TTSIM_ASSERT(gpr_address < std::size(p_tensix->dma_regs[pipe]), "gpr_address=%d out of range", gpr_address);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    if (wr128b) {
        TTSIM_VERIFY(!(cfg_reg & 3), NonContractualBehavior, "misaligned cfg_reg=%d", cfg_reg);
        TTSIM_VERIFY(!(gpr_address & 3), NonContractualBehavior, "misaligned gpr_address=%d", gpr_address);
        for (uint32_t i = 0; i < 4; i++) {
            tensix_cfg_wr32(p_tensix, state_id, 4 * (cfg_reg + i), p_tensix->dma_regs[pipe][gpr_address + i]);
        }
    } else {
        tensix_cfg_wr32(p_tensix, state_id, 4 * cfg_reg, p_tensix->dma_regs[pipe][gpr_address]);
    }
    return true;
}

TENSIX_EXECUTE_RDCFG() {
    TTSIM_VERIFY(cfg_reg < 4 * TENSIX_CFG_STATE_SIZE, NonContractualBehavior, "cfg_reg=%d", cfg_reg);
    TTSIM_ASSERT(gpr_address < std::size(p_tensix->dma_regs[pipe]), "gpr_address=%d out of range", gpr_address);

    uint32_t state_id = get_state_id(p_tensix, pipe);
    p_tensix->dma_regs[pipe][gpr_address] = tensix_cfg_rd32(p_tensix, state_id, 4 * cfg_reg);
    return true;
}

// Not checked: on Wormhole, a SETC16 of CFG_STATE_ID_StateID must wait for earlier Matrix Unit instructions to start:
// one still queued behind earlier Matrix Unit work reads its configuration (formats, Fp32_enabled, the zero flag) from
// the bank the SETC16 selects, where ttsim uses the bank at the instruction.
TENSIX_EXECUTE_SETC16() {
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(setc16_reg != 128, UndefinedBehavior, "setc16_reg=%d: Auto TTSync misses the state ID", setc16_reg);
#endif
    TTSIM_VERIFY(setc16_reg < TENSIX_THD_STATE_SIZE, NonContractualBehavior, "setc16_reg=%d", setc16_reg);

    // Note that there's no way to read these back, so we don't apply the THREAD_CFG*_REG_MASK at present
    switch (setc16_reg) {
#define THREAD_CFG_REG_WR(i) case i: p_tensix->thread[pipe].thread_cfg##i = setc16_value; break;
        FOR_EACH_THREAD_CFG_REG(THREAD_CFG_REG_WR)
#undef THREAD_CFG_REG_WR
        default: TTSIM_ERROR(UnsupportedFunctionality, "setc16_reg=%d", setc16_reg);
    }
#if TT_ARCH_VERSION == 1
    TTSIM_VERIFY(!p_tensix->thread[pipe].CFG_STATE_ID_StateID, UnsupportedFunctionality, "CFG_STATE_ID_StateID");
#endif
    return true;
}

// Not checked: on Wormhole, a write of ALU_ACC_CTRL_Zero_Flag_disabled_src or of the SrcA format
// (ALU_FORMAT_SPEC_REG0_SrcA, ALU_FORMAT_SPEC_REG_SrcA_val/override) must wait for earlier Matrix Unit instructions to
// start: one still queued behind earlier Matrix Unit work reads the new value, where ttsim uses the value at the
// instruction. Blackhole takes the flag, the SrcA format and Fp32_enabled as the instruction is issued, as ttsim does.
static bool tensix_execute_rmwcib(TensixState *p_tensix, uint32_t pipe, uint32_t data, uint32_t mask, uint32_t cfg_reg_addr) {
    TTSIM_VERIFY(cfg_reg_addr < 4 * TENSIX_CFG_STATE_SIZE, NonContractualBehavior, "cfg_reg_addr=%d writes nothing", cfg_reg_addr);
    uint32_t state_id = get_state_id(p_tensix, pipe);
    uint32_t value = tensix_cfg_rd32(p_tensix, state_id, 4 * cfg_reg_addr);
    value = (value & ~mask) | (data & mask);
    tensix_cfg_wr32(p_tensix, state_id, 4 * cfg_reg_addr, value);
    return true;
}

TENSIX_EXECUTE_RMWCIB0() {
    return tensix_execute_rmwcib(p_tensix, pipe, data << 0, mask << 0, cfg_reg_addr);
}
TENSIX_EXECUTE_RMWCIB1() {
    return tensix_execute_rmwcib(p_tensix, pipe, data << 8, mask << 8, cfg_reg_addr);
}
TENSIX_EXECUTE_RMWCIB2() {
    return tensix_execute_rmwcib(p_tensix, pipe, data << 16, mask << 16, cfg_reg_addr);
}
TENSIX_EXECUTE_RMWCIB3() {
    return tensix_execute_rmwcib(p_tensix, pipe, data << 24, mask << 24, cfg_reg_addr);
}

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
        default: TTSIM_UNREACHABLE("scratch_index=%d", scratch_index);
    }
    uint32_t state_id = get_state_id(p_tensix, pipe);
    uint32_t cfg_value = tensix_cfg_rd32(p_tensix, state_id, 4 * cfg_reg);
    cfg_value += scratch_value;
    tensix_cfg_wr32(p_tensix, state_id, 4 * cfg_reg, cfg_value);
    return true;
#else
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#endif
}
#endif

#define UNIMPLEMENTED_TENSIX_INST(name) \
    TENSIX_EXECUTE_##name() { TTSIM_ERROR_NOFMT(UnimplementedFunctionality); }

TENSIX_DECODERS()

static bool tensix_execute_inst(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    uint32_t opcode = bits<31,24>(inst);
    switch (opcode) {
        case TENSIX_OPCODE_SFPLOAD:
        case TENSIX_OPCODE_SFPLOADI:
        case TENSIX_OPCODE_SFPSTORE:
        case TENSIX_OPCODE_SFPLUT: {
            uint32_t lreg_ind = bits<23,20>(inst); // dest register lives in different bits for these
            TTSIM_VERIFY(lreg_ind < 12, UnsupportedFunctionality, "inst=0x%x: instruction template load", inst);
            break;
        }
        case TENSIX_OPCODE_SFPMULI:
        case TENSIX_OPCODE_SFPADDI:
        case TENSIX_OPCODE_SFPDIVP2:
        case TENSIX_OPCODE_SFPEXEXP:
        case TENSIX_OPCODE_SFPEXMAN:
        case TENSIX_OPCODE_SFPIADD:
        case TENSIX_OPCODE_SFPSHFT:
        case TENSIX_OPCODE_SFPSETCC:
        case TENSIX_OPCODE_SFPMOV:
        case TENSIX_OPCODE_SFPABS:
        case TENSIX_OPCODE_SFPAND:
        case TENSIX_OPCODE_SFPOR:
        case TENSIX_OPCODE_SFPNOT:
        case TENSIX_OPCODE_SFPLZ:
        case TENSIX_OPCODE_SFPSETEXP:
        case TENSIX_OPCODE_SFPSETMAN:
        case TENSIX_OPCODE_SFPMAD:
        case TENSIX_OPCODE_SFPADD:
        case TENSIX_OPCODE_SFPMUL:
        case TENSIX_OPCODE_SFPPUSHC:
        case TENSIX_OPCODE_SFPPOPC:
        case TENSIX_OPCODE_SFPSETSGN:
        case TENSIX_OPCODE_SFPENCC:
        case TENSIX_OPCODE_SFPCOMPC:
        case TENSIX_OPCODE_SFPTRANSP:
        case TENSIX_OPCODE_SFPXOR:
        case TENSIX_OPCODE_SFP_STOCH_RND:
        case TENSIX_OPCODE_SFPNOP:
        case TENSIX_OPCODE_SFPCAST:
        case TENSIX_OPCODE_SFPSWAP:
        case TENSIX_OPCODE_SFPSHFT2:
        case TENSIX_OPCODE_SFPLUTFP32:
#if TT_ARCH_VERSION >= 1
        case TENSIX_OPCODE_SFPLE:
        case TENSIX_OPCODE_SFPGT:
        case TENSIX_OPCODE_SFPMUL24:
        case TENSIX_OPCODE_SFPARECIP:
#endif
        {
            uint32_t lreg_dest = bits<7,4>(inst);
            if (lreg_dest >= 12) { // an SFPLOADMACRO template load, which nothing uses (SFPLOADMACRO is refused)
                uint32_t mask = p_tensix->cc_en ? p_tensix->cc : 0xFFFFFFFF;
                TTSIM_VERIFY(mask == 0xFFFFFFFF, UnsupportedFunctionality, "inst=0x%x: not all lanes enabled: mask=0x%x", inst, mask);
                // Note: HW erratum (WH only): an SFPSWAP captured this way makes the very next Vector Unit
                // instruction execute twice; not modeled
                return true;
            }
            break;
        }
    }

    // WH/BH erratum TEN-2932: with ENABLE_DEST_INDEX set (the same in every SFPU instance), most instructions writing
    // LReg[4..7] write an architecturally-undefined index bank value instead of the intended result. Load/store
    // instructions, SFPCONFIG/SFPSWAP/SFPTRANSP, and CC-only/SFPNOP instructions unaffected.
    if ((p_tensix->lane_config[0] & 4) && (opcode >= 0x73) && (opcode <= 0x99) &&
        (opcode != TENSIX_OPCODE_SFPCONFIG) && (opcode != TENSIX_OPCODE_SFPSWAP) &&
        (opcode != TENSIX_OPCODE_SFPLOADMACRO)) {
        // SFPLUT's VD is in [23:20]. Should be zero already for CC-only instructions and SFPNOP/SFPTRANSP
        uint32_t lreg_dest = (opcode == TENSIX_OPCODE_SFPLUT) ? bits<23,20>(inst) : bits<7,4>(inst);
        TTSIM_VERIFY((lreg_dest < 4) || (lreg_dest >= 8), UnsupportedFunctionality,
            "TEN-2932: SFPU result write to LReg[%d] is corrupted when ENABLE_DEST_INDEX is set (opcode=0x%x)", lreg_dest, opcode);
    }
    switch (opcode) {
        case TENSIX_OPCODE_MOP: TTSIM_ERROR(UndefinedBehavior, "MOP should not exist at this stage");
        case TENSIX_OPCODE_NOP: TTSIM_UNREACHABLE("NOP should not exist at this stage");
        case TENSIX_OPCODE_MOP_CFG: TTSIM_ERROR(UndefinedBehavior, "MOP_CFG should not exist at this stage");
        case TENSIX_OPCODE_REPLAY: TTSIM_UNREACHABLE("REPLAY should not exist at this stage");
        TENSIX_OPCODE_CASES() // use the generated decoder for the rest
        default: TTSIM_ERROR(UndefinedBehavior, "opcode=0x%x", opcode);
    }
    return true;
}

bool tensix_decode_and_execute(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {
    if (p_tensix->wait_block[pipe] && block_mask_holds(p_tensix->wait_block[pipe], inst)) {
        p_tensix->wait_block[pipe] = 0;
    }
    return tensix_execute_inst(p_tensix, pipe, inst);
}
