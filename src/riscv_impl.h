// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// RISC-V instruction implementations, parametric on XLEN (instantiated by rv32.cpp and rv64.cpp).
// Quick references:
// https://csg.csail.mit.edu/6.375/6_375_2019_www/resources/riscv-spec.pdf
// https://five-embeddev.com/quickref/isa_ext.html

#if !defined(TTSIM_RV64_SYSTEM)
#define TTSIM_RV64_SYSTEM 0 // default off; the standalone full-system riscv64 build sets it to 1
#endif

#if TTSIM_RV64_SYSTEM
#define TT_ARCH_VERSION -1 // means "no TT hardware present"
#include "rv64_system.h"
#include "rv64_fpu.h"
#else
#include "sim.h"
#endif
#include "riscv_defines.h"
#if (TT_ARCH_VERSION >= 1) && (XLEN == 32)
#include "riscv_float.h"
#endif
#include <algorithm>
#include <type_traits>

#define RV_XLEN_PREFIX(name) RV_XLEN_PREFIX_(XLEN, name)
#define RV_XLEN_PREFIX_(xlen, name) RV_XLEN_PREFIX__(xlen, name)
#define RV_XLEN_PREFIX__(xlen, name) rv##xlen##_##name

#define RISCV_I_IMM(inst) (int32_t(inst) >> 20)
#define RISCV_U_IMM(inst) (int32_t((inst) & 0xFFFFF000))
#define RISCV_S_IMM(inst) (int32_t(((inst) & 0xFE000000) | (((inst) & 0xF80) << 13)) >> 20)

#define X_SP 2

// Full-system RV64 currently does not support bitmanip
#define HAS_ZBA_ZBB (((TT_ARCH_VERSION >= 1) || (XLEN == 64)) && !TTSIM_RV64_SYSTEM)
#define HAS_ZBC ((XLEN == 64) && !TTSIM_RV64_SYSTEM)
#define HAS_ZBS ((XLEN == 64) && !TTSIM_RV64_SYSTEM)
#define HAS_PACK_BREV8 ((TT_ARCH_VERSION >= 1) && (XLEN == 32)) // small subset of Zbkb that is in BH babyrisc

using int_xlen_t = int_types<XLEN>::int_t;
using uint_xlen_t = int_types<XLEN>::uint_t;
using int_2_times_xlen_t = int_types<2*XLEN>::int_t;
using uint_2_times_xlen_t = int_types<2*XLEN>::uint_t;

#if XLEN == 64
static
#endif
void RV_XLEN_PREFIX(icache_invalidate)(RiscvHartState *p_hart) {
}

#if TT_ARCH_VERSION >= 1
// note that this doesn't do writebacks; this is OK for initialization (nothing dirty yet)
// and for the babyrisc fence path that calls it (write through)
static void dcache_invalidate(RiscvHartState *p_hart) {
}
#endif

void RV_XLEN_PREFIX(init)(RiscvHartState *p_hart, char tile_type, uint32_t tile_id, uint32_t riscv_id) {
    memset(p_hart, 0, sizeof(RiscvHartState));
#if !TTSIM_RV64_SYSTEM
    p_hart->tile_type = tile_type;
    p_hart->tile_id = tile_id;
    p_hart->riscv_id = riscv_id;
#if XLEN == 64
    p_hart->riscv_id |= 0x80000000; // so that fabrics can distinguish type of core initiating request
    p_hart->mstatus = 3ull << 11; // MPP=3, most other bits not relevant with U/S/H unsupported
#endif

    if (tile_type == 'T') {
        p_hart->p_sram = g_t_tiles[tile_id].sram;
        p_hart->sram_size = sizeof(g_t_tiles[tile_id].sram);
#if XLEN == 32
        p_hart->p_local_mem = g_t_tiles[tile_id].rv32_local_ram[riscv_id];
        p_hart->local_mem_base = RISCV_LOCAL_MEM_BASE;
        switch (riscv_id) {
            case RV32_ID_BRISC: p_hart->local_mem_size = BRISC_LOCAL_MEM_SIZE; break;
            case RV32_ID_TRISC0: p_hart->local_mem_size = TRISC_LOCAL_MEM_SIZE; break;
            case RV32_ID_TRISC1: p_hart->local_mem_size = TRISC_LOCAL_MEM_SIZE; break;
            case RV32_ID_TRISC2: p_hart->local_mem_size = TRISC_LOCAL_MEM_SIZE; break;
            case RV32_ID_NCRISC: p_hart->local_mem_size = NCRISC_LOCAL_MEM_SIZE; break;
            default: TTSIM_ERROR(AssertionFailure, "riscv_id=%d", riscv_id);
        }
        TTSIM_ASSERT(p_hart->local_mem_size <= sizeof(g_t_tiles[tile_id].rv32_local_ram[riscv_id]));
#endif
    } else {
        TTSIM_VERIFY(tile_type == 'E', AssertionFailure, "tile_type=%c", tile_type);
#if XLEN == 32
        p_hart->p_sram = g_e_tiles[tile_id].sram;
        p_hart->sram_size = sizeof(g_e_tiles[tile_id].sram);
        p_hart->p_local_mem = g_e_tiles[tile_id].rv32_local_ram[riscv_id];
        p_hart->local_mem_base = RISCV_LOCAL_MEM_BASE;
        p_hart->local_mem_size = sizeof(g_e_tiles[tile_id].rv32_local_ram[riscv_id]);
#else
        TTSIM_ERROR(AssertionFailure, "eth cores should only be rv32");
#endif
    }

    RV_XLEN_PREFIX(icache_invalidate)(p_hart);
#if TT_ARCH_VERSION >= 1
    dcache_invalidate(p_hart);
#endif
#endif
}

static inline uint_xlen_t mulh(uint_xlen_t src0, uint_xlen_t src1) {
    return uint_xlen_t((int_2_times_xlen_t(int_xlen_t(src0)) *
                        int_2_times_xlen_t(int_xlen_t(src1))) >> XLEN);
}

static inline uint_xlen_t mulhsu(uint_xlen_t src0, uint_xlen_t src1) {
    return uint_xlen_t((int_2_times_xlen_t(int_xlen_t(src0)) *
                        uint_2_times_xlen_t(src1)) >> XLEN);
}

static inline uint_xlen_t mulhu(uint_xlen_t src0, uint_xlen_t src1) {
    return uint_xlen_t((uint_2_times_xlen_t(src0) *
                        uint_2_times_xlen_t(src1)) >> XLEN);
}

#if HAS_ZBC
static inline uint_xlen_t clmul(uint_xlen_t src0, uint_xlen_t src1) {
    uint_xlen_t value = 0;
    for (uint32_t i = 0; i < XLEN; i++) {
        if ((src1 >> i) & 1) {
            value ^= src0 << i;
        }
    }
    return value;
}

static inline uint_xlen_t clmulh(uint_xlen_t src0, uint_xlen_t src1) {
    uint_xlen_t value = 0;
    for (uint32_t i = 1; i < XLEN; i++) {
        if ((src1 >> i) & 1) {
            value ^= src0 >> (XLEN - i);
        }
    }
    return value;
}

static inline uint_xlen_t clmulr(uint_xlen_t src0, uint_xlen_t src1) {
    uint_xlen_t value = 0;
    for (uint32_t i = 0; i < XLEN; i++) {
        if ((src1 >> i) & 1) {
            value ^= src0 >> (XLEN - i - 1);
        }
    }
    return value;
}
#endif

static inline uint_xlen_t div(uint_xlen_t src0, uint_xlen_t src1) {
    if ((src0 == (uint_xlen_t(1) << (XLEN - 1)) && (src1 == ~uint_xlen_t(0)))) {
        return uint_xlen_t(1) << (XLEN - 1); // signed overflow
    }
    return src1 ? uint_xlen_t(int_xlen_t(src0) / int_xlen_t(src1)) : ~uint_xlen_t(0);
}

static inline uint_xlen_t rem(uint_xlen_t src0, uint_xlen_t src1) {
    if ((src0 == (uint_xlen_t(1) << (XLEN - 1)) && (src1 == ~uint_xlen_t(0)))) {
        return 0; // signed overflow
    }
    return src1 ? uint_xlen_t(int_xlen_t(src0) % int_xlen_t(src1)) : src0;
}

template<uint32_t funct3> static void RV_XLEN_PREFIX(alu)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r_src0 = bits<19,15>(inst);
    uint32_t r_src1 = bits<24,20>(inst);
    uint32_t funct7 = inst >> 25;

    uint_xlen_t src0 = p_hart->x_regs[r_src0];
    uint_xlen_t src1 = p_hart->x_regs[r_src1];
    uint_xlen_t value;
    switch ((funct7 << 3) | funct3) {
        case ( 0 << 3) | 0: value = src0 + src1; break; // ADD
        case (32 << 3) | 0: value = src0 - src1; break; // SUB
        case ( 0 << 3) | 1: value = src0 << (src1 & (XLEN - 1)); break; // SLL
        case ( 0 << 3) | 2: value = int_xlen_t(src0) < int_xlen_t(src1); break; // SLT
        case ( 0 << 3) | 3: value = src0 < src1; break; // SLTU
        case ( 0 << 3) | 4: value = src0 ^ src1; break; // XOR
        case ( 0 << 3) | 5: value = src0 >> (src1 & (XLEN - 1)); break; // SRL
        case (32 << 3) | 5: value = uint_xlen_t(int_xlen_t(src0) >> (src1 & (XLEN - 1))); break; // SRA
        case ( 0 << 3) | 6: value = src0 | src1; break; // OR
        case ( 0 << 3) | 7: value = src0 & src1; break; // AND
        case ( 1 << 3) | 0: value = src0 * src1; break; // MUL
        case ( 1 << 3) | 1: value = mulh(src0, src1); break; // MULH
        case ( 1 << 3) | 2: value = mulhsu(src0, src1); break; // MULHSU
        case ( 1 << 3) | 3: value = mulhu(src0, src1); break; // MULHU
        case ( 1 << 3) | 4: value = div(src0, src1); break; // DIV
        case ( 1 << 3) | 5: value = src1 ? (src0 / src1) : ~uint_xlen_t(0); break; // DIVU
        case ( 1 << 3) | 6: value = rem(src0, src1); break; // REM
        case ( 1 << 3) | 7: value = src1 ? (src0 % src1) : src0; break; // REMU
#if HAS_ZBA_ZBB
        case ( 5 << 3) | 4: value = uint_xlen_t(std::min(int_xlen_t(src0), int_xlen_t(src1))); break; // MIN
        case ( 5 << 3) | 5: value = std::min(src0, src1); break; // MINU
        case ( 5 << 3) | 6: value = uint_xlen_t(std::max(int_xlen_t(src0), int_xlen_t(src1))); break; // MAX
        case ( 5 << 3) | 7: value = std::max(src0, src1); break; // MAXU
        case (16 << 3) | 2: value = (src0 << 1) + src1; break; // SH1ADD
        case (16 << 3) | 4: value = (src0 << 2) + src1; break; // SH2ADD
        case (16 << 3) | 6: value = (src0 << 3) + src1; break; // SH3ADD
        case (32 << 3) | 4: value = src0 ^ ~src1; break; // XNOR
        case (32 << 3) | 6: value = src0 | ~src1; break; // ORN
        case (32 << 3) | 7: value = src0 & ~src1; break; // ANDN
        case (48 << 3) | 1: value = std::rotl(src0, src1); break; // ROL
        case (48 << 3) | 5: value = std::rotr(src0, src1); break; // ROR
        // note: we don't currently support the XLEN == 32 flavor of ZEXT.H except in combination with PACK below
#endif
#if HAS_PACK_BREV8
        case ( 4 << 3) | 4: static_assert(XLEN == 32); value = (src0 & 0xFFFF) | (src1 << 16); break; // ZEXT.H + PACK
#endif
#if HAS_ZBC
        case ( 5 << 3) | 1: value = clmul(src0, src1); break; // CLMUL
        case ( 5 << 3) | 2: value = clmulr(src0, src1); break; // CLMULR
        case ( 5 << 3) | 3: value = clmulh(src0, src1); break; // CLMULH
#endif
#if HAS_ZBS
        case (20 << 3) | 1: value = src0 | (uint_xlen_t(1) << (src1 & (XLEN - 1))); break; // BSET
        case (36 << 3) | 1: value = src0 & ~(uint_xlen_t(1) << (src1 & (XLEN - 1))); break; // BCLR
        case (36 << 3) | 5: value = (src0 >> (src1 & (XLEN - 1))) & 1; break; // BEXT
        case (52 << 3) | 1: value = src0 ^ (uint_xlen_t(1) << (src1 & (XLEN - 1))); break; // BINV
#endif
#if !TTSIM_RV64_SYSTEM
        default: TTSIM_ERROR(UndefinedBehavior, "funct3=%d funct7=%d", funct3, funct7);
#else
        default: TTSIM_ERROR(UnimplementedFunctionality, "funct3=%d funct7=%d", funct3, funct7);
#endif
    }
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = value;
    }
}

#if XLEN == 64
static inline int64_t sext32_to_64(uint32_t x) {
    return int64_t(int32_t(x));
}

static inline uint32_t divw(uint32_t src0, uint32_t src1) {
    if ((src0 == 0x80000000) && (src1 == 0xFFFFFFFF)) {
        return 0x80000000; // signed overflow
    }
    return src1 ? uint32_t(int32_t(src0) / int32_t(src1)) : 0xFFFFFFFF;
}

static inline uint32_t remw(uint32_t src0, uint32_t src1) {
    if ((src0 == 0x80000000) && (src1 == 0xFFFFFFFF)) {
        return 0; // signed overflow
    }
    return src1 ? uint32_t(int32_t(src0) % int32_t(src1)) : src0;
}

template<uint32_t funct3> static void RV_XLEN_PREFIX(alu_32)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r_src0 = bits<19,15>(inst);
    uint32_t r_src1 = bits<24,20>(inst);
    uint32_t funct7 = inst >> 25;

    uint32_t src0 = p_hart->x_regs[r_src0];
    uint32_t src1 = p_hart->x_regs[r_src1];
    uint64_t value;
    switch ((funct7 << 3) | funct3) {
        case ( 0 << 3) | 0: value = sext32_to_64(src0 + src1); break; // ADDW
        case (32 << 3) | 0: value = sext32_to_64(src0 - src1); break; // SUBW
        case ( 0 << 3) | 1: value = sext32_to_64(src0 << (src1 & 31)); break; // SLLW
        case ( 0 << 3) | 5: value = sext32_to_64(src0 >> (src1 & 31)); break; // SRLW
        case (32 << 3) | 5: value = sext32_to_64(int32_t(src0) >> (src1 & 31)); break; // SRAW
        case ( 1 << 3) | 0: value = sext32_to_64(src0 * src1); break; // MULW
        case ( 1 << 3) | 4: value = sext32_to_64(divw(src0, src1)); break; // DIVW
        case ( 1 << 3) | 5: value = sext32_to_64(src1 ? (src0 / src1) : ~uint32_t(0)); break; // DIVUW
        case ( 1 << 3) | 6: value = sext32_to_64(remw(src0, src1)); break; // REMW
        case ( 1 << 3) | 7: value = sext32_to_64(src1 ? (src0 % src1) : src0); break; // REMUW
#if HAS_ZBA_ZBB
        case ( 4 << 3) | 0: value = uint64_t(src0) + p_hart->x_regs[r_src1]; break; // ADD.UW
#if XLEN == 64
        case ( 4 << 3) | 4: TTSIM_VERIFY(!r_src1, UnimplementedFunctionality, "zext.h r_src1=%d", r_src1); value = src0 & 0xFFFF; break; // ZEXT.H
#endif
        case (16 << 3) | 2: value = (uint64_t(src0) << 1) + p_hart->x_regs[r_src1]; break; // SH1ADD.UW
        case (16 << 3) | 4: value = (uint64_t(src0) << 2) + p_hart->x_regs[r_src1]; break; // SH2ADD.UW
        case (16 << 3) | 6: value = (uint64_t(src0) << 3) + p_hart->x_regs[r_src1]; break; // SH3ADD.UW
        case (48 << 3) | 1: value = sext32_to_64(std::rotl(src0, src1)); break; // ROLW
        case (48 << 3) | 5: value = sext32_to_64(std::rotr(src0, src1)); break; // RORW
#endif
        default: TTSIM_ERROR(UnimplementedFunctionality, "funct3=%d funct7=%d", funct3, funct7);
    }
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = value;
    }
}
#endif

#if HAS_ZBA_ZBB
static inline uint_xlen_t orc_b(uint_xlen_t x) {
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    return (x & uint_xlen_t(0x0101010101010101ull)) * 0xFF;
}
#endif

#if HAS_PACK_BREV8
static inline uint_xlen_t brev8(uint_xlen_t x) {
    static_assert(XLEN == 32); // for now
    x = ((x & 0x55555555) << 1) | ((x & 0xAAAAAAAA) >> 1);
    x = ((x & 0x33333333) << 2) | ((x & 0xCCCCCCCC) >> 2);
    x = ((x & 0x0F0F0F0F) << 4) | ((x & 0xF0F0F0F0) >> 4);
    return x;
}
#endif

template<uint32_t funct3> static void RV_XLEN_PREFIX(alu_imm)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r_src = bits<19,15>(inst);
    int_xlen_t imm = RISCV_I_IMM(inst);

    uint_xlen_t src = p_hart->x_regs[r_src];
    uint_xlen_t value;
    switch (funct3) {
        case 0: value = src + imm; break; // ADDI
        case 1:
            switch (imm) {
                case 0 ... XLEN-1: value = src << imm; break; // SLLI
#if HAS_ZBA_ZBB
                case 0x600: // CLZ
#if XLEN == 32
                    value = src ? __builtin_clz(src) : 32;
#else
                    value = src ? __builtin_clzll(src) : 64;
#endif
                    break;
                case 0x601: // CTZ
#if XLEN == 32
                    value = src ? __builtin_ctz(src) : 32;
#else
                    value = src ? __builtin_ctzll(src) : 64;
#endif
                    break;
                case 0x602: // CPOP
#if XLEN == 32
                    value = __builtin_popcount(src);
#else
                    value = __builtin_popcountll(src);
#endif
                    break;
                case 0x604: value = int_xlen_t(int8_t(uint8_t(src))); break; // SEXT.B
                case 0x605: value = int_xlen_t(int16_t(uint16_t(src))); break; // SEXT.H
#endif
#if HAS_ZBS
                case 0x280 ... 0x280 + XLEN-1: value = src | (uint_xlen_t(1) << (imm & (XLEN - 1))); break; // BSETI
                case 0x480 ... 0x480 + XLEN-1: value = src & ~(uint_xlen_t(1) << (imm & (XLEN - 1))); break; // BCLRI
                case 0x680 ... 0x680 + XLEN-1: value = src ^ (uint_xlen_t(1) << (imm & (XLEN - 1))); break; // BINVI
#endif
#if !TTSIM_RV64_SYSTEM
                default: TTSIM_ERROR(UndefinedBehavior, "funct3=%d imm=0x%x", funct3, imm);
#else
                default: TTSIM_ERROR(UnimplementedFunctionality, "funct3=%d imm=0x%x", funct3, imm);
#endif
            }
            break;
        case 2: value = int_xlen_t(src) < imm; break; // SLTI
        case 3: value = src < uint_xlen_t(imm); break; // SLTUI
        case 4: value = src ^ imm; break; // XORI
        case 5:
            switch (imm) {
                case 0 ... XLEN-1: value = src >> imm; break; // SRLI
                case 0x400 ... 0x400 + XLEN-1: value = uint_xlen_t(int_xlen_t(src) >> (imm & (XLEN-1))); break; // SRAI
#if HAS_ZBA_ZBB
                case 0x287: value = orc_b(src); break; // ORC.B
                case 0x600 ... 0x600 + XLEN-1: value = std::rotr(src, imm); break; // RORI
#if XLEN == 32
                case 0x698: value = __builtin_bswap32(src); break; // REV8
#else
                case 0x6B8: value = __builtin_bswap64(src); break; // REV8
#endif
#endif
#if HAS_PACK_BREV8
                case 0x687: value = brev8(src); break;
#endif
#if HAS_ZBS
                case 0x480 ... 0x480 + XLEN-1: value = (src >> (imm & (XLEN - 1))) & 1; break; // BEXTI
#endif
#if TT_ARCH_VERSION == 1
                case 0x680 ... 0x686: case 0x688 ... 0x697: case 0x699 ... 0x69F: TTSIM_ERROR(UnsupportedFunctionality, "GREVI was removed from final Bitmanip spec");
#endif
#if !TTSIM_RV64_SYSTEM
                default: TTSIM_ERROR(UndefinedBehavior, "funct3=%d imm=0x%x", funct3, imm);
#else
                default: TTSIM_ERROR(UnimplementedFunctionality, "funct3=%d imm=0x%x", funct3, imm);
#endif
            }
            break;
        case 6: value = src | imm; break; // ORI
        case 7: value = src & imm; break; // ANDI
#if !TTSIM_RV64_SYSTEM
        default: TTSIM_ERROR(UndefinedBehavior, "funct3=%d", funct3);
#else
        default: TTSIM_ERROR(UnimplementedFunctionality, "funct3=%d", funct3);
#endif
    }
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = value;
    }
}

#if XLEN == 64
template<uint32_t funct3> static void RV_XLEN_PREFIX(alu_imm_32)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r_src = bits<19,15>(inst);
    int32_t imm = RISCV_I_IMM(inst);

    uint32_t src = p_hart->x_regs[r_src];
    uint64_t value;
    switch (funct3) {
        case 0: value = sext32_to_64(src + imm); break;
        case 1:
            switch (imm) {
                case 0 ... 0x1F: value = sext32_to_64(src << imm); break; // SLLIW
#if HAS_ZBA_ZBB
                case 0x80 ... 0xBF: value = uint64_t(src) << (imm & 63); break; // SLLI.UW
                case 0x600: value = src ? __builtin_clz(src) : 32; break; // CLZW
                case 0x601: value = src ? __builtin_ctz(src) : 32; break; // CTZW
                case 0x602: value = __builtin_popcount(src); break; // CPOPW
#endif
                default: TTSIM_ERROR(UnimplementedFunctionality, "funct3=%d imm=0x%x", funct3, imm);
            }
            break;
        case 5:
            switch (imm) {
                case 0 ... 0x1F: value = sext32_to_64(src >> imm); break; // SRLIW
                case 0x400 ... 0x41F: value = sext32_to_64(int32_t(src) >> (imm & 31)); break; // SRAIW
#if HAS_ZBA_ZBB
                case 0x600 ... 0x61F: value = sext32_to_64(std::rotr(src, imm & 31)); break; // RORIW
#endif
                default: TTSIM_ERROR(UnimplementedFunctionality, "funct3=%d imm=0x%x", funct3, imm);
            }
            break;
        default: TTSIM_ERROR(UnimplementedFunctionality, "funct3=%d", funct3);
    }
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = value;
    }
}
#endif

template<bool neg_product, bool neg_addend> static void RV_XLEN_PREFIX(f_fma)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zfh/F/D/Q");
#elif XLEN == 64
    rv64_fpu_fma(p_hart, inst, neg_product, neg_addend);
#else
    uint32_t fmt = bits<26,25>(inst);
    TTSIM_VERIFY(fmt != 1, UndefinedBehavior, "babyrisc does not support D");
    TTSIM_VERIFY(fmt != 3, UndefinedBehavior, "babyrisc does not support Q");
    TTSIM_VERIFY(fmt != 2, UnsupportedFunctionality, "babyrisc non-compliant Zfh extension is out of scope");
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t rm = bits<14,12>(inst);
    TTSIM_VERIFY((rm <= 4) || (rm == 7), UndefinedBehavior, "rm=%d", rm);
    TTSIM_VERIFY(rm == 7, UnsupportedFunctionality, "rm=%d", rm);
    uint32_t r_src0 = bits<19,15>(inst);
    uint32_t r_src1 = bits<24,20>(inst);
    uint32_t r_src2 = bits<31,27>(inst);

    uint32_t a = p_hart->f_regs[r_src0];
    uint32_t b = p_hart->f_regs[r_src1];
    uint32_t c = p_hart->f_regs[r_src2];
    if (neg_product) {
        a ^= 0x80000000;
    }
    if (neg_addend) {
        c ^= 0x80000000;
    }
    p_hart->f_regs[r_dst] = fma_model(a, b, c); // note: uses same non-IEEE FMA as SFPU
#endif
}

static void RV_XLEN_PREFIX(f_alu)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zfh/F/D/Q");
#elif XLEN == 64
    rv64_fpu_op(p_hart, inst);
#else
    uint32_t funct7 = bits<31,25>(inst);
    uint32_t funct3 = bits<14,12>(inst);
    uint32_t fmt = funct7 & 3;
    TTSIM_VERIFY(fmt != 1, UndefinedBehavior, "babyrisc does not support D");
    TTSIM_VERIFY(fmt != 3, UndefinedBehavior, "babyrisc does not support Q");
    TTSIM_VERIFY(fmt != 2, UnsupportedFunctionality, "babyrisc non-compliant Zfh extension is out of scope");
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r_src0 = bits<19,15>(inst);
    uint32_t r_src1 = bits<24,20>(inst);

    uint32_t a = p_hart->f_regs[r_src0];
    uint32_t b = p_hart->f_regs[r_src1];
    switch (funct7) {
        case 0x00: // FADD.S
            TTSIM_VERIFY((funct3 <= 4) || (funct3 == 7), UndefinedBehavior, "FADD.S rm=%d", funct3);
            TTSIM_VERIFY(funct3 == 7, UnsupportedFunctionality, "FADD.S rm=%d", funct3);
            p_hart->f_regs[r_dst] = fma_model(a, 0x3F800000, b);
            break;
        case 0x04: // FSUB.S
            TTSIM_VERIFY((funct3 <= 4) || (funct3 == 7), UndefinedBehavior, "FSUB.S rm=%d", funct3);
            TTSIM_VERIFY(funct3 == 7, UnsupportedFunctionality, "FSUB.S rm=%d", funct3);
            p_hart->f_regs[r_dst] = fma_model(a, 0x3F800000, b ^ 0x80000000);
            break;
        case 0x08: // FMUL.S
            TTSIM_VERIFY((funct3 <= 4) || (funct3 == 7), UndefinedBehavior, "FMUL.S rm=%d", funct3);
            TTSIM_VERIFY(funct3 == 7, UnsupportedFunctionality, "FMUL.S rm=%d", funct3);
            p_hart->f_regs[r_dst] = fma_model(a, b, (a ^ b) & 0x80000000);
            break;
        case 0x0C: TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support FDIV");
        case 0x10:
            switch (funct3) {
                case 0: p_hart->f_regs[r_dst] = (a & 0x7FFFFFFF) | (b & 0x80000000); break; // FSGNJ.S
                case 1: p_hart->f_regs[r_dst] = (a & 0x7FFFFFFF) | (~b & 0x80000000); break; // FSGNJN.S
                case 2: p_hart->f_regs[r_dst] = a ^ (b & 0x80000000); break; // FSGNJX.S
                default: TTSIM_ERROR(UndefinedBehavior, "FSGNJ funct3=%d", funct3);
            }
            break;
        case 0x14:
            switch (funct3) {
                case 0: p_hart->f_regs[r_dst] = fp32_min_max(a, b, false); break; // FMIN.S
                case 1: p_hart->f_regs[r_dst] = fp32_min_max(a, b, true); break; // FMAX.S
                default: TTSIM_ERROR(UndefinedBehavior, "FMINMAX funct3=%d", funct3);
            }
            break;
        case 0x2C: TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support FSQRT");
        case 0x50: {
            uint32_t value;
            bool ordered = !fp32_is_nan(a) && !fp32_is_nan(b);
            switch (funct3) {
                case 0: value = ordered && (fp32_lt_ordered(a, b) || fp32_eq_ordered(a, b)); break; // FLE.S
                case 1: value = ordered && fp32_lt_ordered(a, b); break; // FLT.S
                case 2: value = ordered && fp32_eq_ordered(a, b); break; // FEQ.S
                default: TTSIM_ERROR(UndefinedBehavior, "FCMP funct3=%d", funct3);
            }
            if (r_dst) [[likely]] {
                p_hart->x_regs[r_dst] = value;
            }
            break;
        }
        case 0x60: {
            TTSIM_VERIFY((funct3 <= 4) || (funct3 == 7), UndefinedBehavior, "FCVT rm=%d", funct3);
            TTSIM_VERIFY((funct3 == 0) || (funct3 == 7), UnsupportedFunctionality, "FCVT rm=%d", funct3);
            uint32_t value;
            switch (r_src1) {
                case 0: value = fp32_to_i32(a); break; // FCVT.W.S
                case 1: value = fp32_to_u32(a); break; // FCVT.WU.S
                default: TTSIM_ERROR(UndefinedBehavior, "FCVT.W r_src1=%d", r_src1);
            }
            if (r_dst) [[likely]] {
                p_hart->x_regs[r_dst] = value;
            }
            break;
        }
        case 0x68: {
            TTSIM_VERIFY((funct3 <= 4) || (funct3 == 7), UndefinedBehavior, "FCVT rm=%d", funct3);
            TTSIM_VERIFY((funct3 == 0) || (funct3 == 7), UnsupportedFunctionality, "FCVT rm=%d", funct3);
            uint32_t value;
            switch (r_src1) {
                case 0: value = i32_to_fp32(p_hart->x_regs[r_src0]); break; // FCVT.S.W
                case 1: value = u32_to_fp32(p_hart->x_regs[r_src0]); break; // FCVT.S.WU
                default: TTSIM_ERROR(UndefinedBehavior, "FCVT.S r_src1=%d", r_src1);
            }
            p_hart->f_regs[r_dst] = value;
            break;
        }
        case 0x70: {
            TTSIM_VERIFY(!r_src1, UndefinedBehavior, "FMV.X.W/FCLASS.S r_src1=%d", r_src1);
            uint32_t value;
            switch (funct3) {
                case 0: value = a; break; // FMV.X.W
                case 1: value = fp32_classify(a); break; // FCLASS.S
                default: TTSIM_ERROR(UndefinedBehavior, "funct7=0x%x funct3=%d", funct7, funct3);
            }
            if (r_dst) [[likely]] {
                p_hart->x_regs[r_dst] = value;
            }
            break;
        }
        case 0x78:
            TTSIM_VERIFY(!r_src1, UndefinedBehavior, "FMV.W.X r_src1=%d", r_src1);
            switch (funct3) {
                case 0: p_hart->f_regs[r_dst] = p_hart->x_regs[r_src0]; break; // FMV.W.X
                default: TTSIM_ERROR(UndefinedBehavior, "funct7=0x%x funct3=%d", funct7, funct3);
            }
            break;
        default:
            TTSIM_ERROR(UndefinedBehavior, "funct7=0x%x funct3=%d", funct7, funct3);
    }
#endif
}

static void RV_XLEN_PREFIX(v_alu)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support V");
#elif XLEN == 64
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#else
    TTSIM_ERROR(UnsupportedFunctionality, "babyrisc non-compliant V extension is explicitly out of scope");
#endif
}

#if !TTSIM_RV64_SYSTEM
static void dcache_access(RiscvHartState *p_hart, uint_xlen_t addr, bool store) {
}
#endif

#if XLEN == 32
static uint8_t *get_local_mem_ptr(RiscvHartState *p_hart, uint_xlen_t addr) {
    uint32_t local_mem_base = p_hart->local_mem_base;
    if ((addr >= local_mem_base) && (addr < local_mem_base + p_hart->local_mem_size)) {
        return &p_hart->p_local_mem[addr - local_mem_base];
    }
    return nullptr;
}
#endif

template<class T> static bool RV_XLEN_PREFIX(mem_rd)(RiscvHartState *p_hart, uint_xlen_t addr, T *p_data) {
    const uint32_t size = sizeof(T);
    static_assert((size == 1) || (size == 2) || (size == 4) || (size == 8), "unsupported object size");
#if TTSIM_RV64_SYSTEM
    if (const uint8_t *p_mem = rv64_dtlb_host(p_hart, addr, size, false)) [[likely]] {
        *p_data = mem_rd<T>(p_mem);
        return true;
    }
    return rv64_sys_load(p_hart, addr, p_data, size);
#else
    TTSIM_VERIFY(!(addr & (size - 1)), NonContractualBehavior, "unaligned addr=0x%llx size=%d", uint64_t(addr), size);
    if (addr < p_hart->sram_size) {
        dcache_access(p_hart, addr, false);
        *p_data = mem_rd<T>(&p_hart->p_sram[addr]);
        return true;
    }
#if XLEN == 32
    if (uint8_t *p_mem = get_local_mem_ptr(p_hart, addr)) {
        *p_data = mem_rd<T>(p_mem);
        return true;
    }
#endif
    if constexpr (size == 1) {
        TTSIM_ERROR(UntestedFunctionality, "8-bit MMIO read");
        auto [done, data] = tile_mmio_rd32(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id, addr & ~uint_xlen_t(3));
        *p_data = (data >> (8 * (addr & 3))) & 0xFF;
        return done;
    } else if constexpr (size == 2) {
        TTSIM_ERROR(UntestedFunctionality, "16-bit MMIO read");
        auto [done, data] = tile_mmio_rd32(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id, addr & ~uint_xlen_t(3));
        *p_data = (data >> (8 * (addr & 3))) & 0xFFFF;
        return done;
    } else if constexpr (size == 4) {
        auto [done, data] = tile_mmio_rd32(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id, addr);
        *p_data = data;
        return done;
    } else {
        static_assert(size == 8);
        auto [done, data] = tile_mmio_rd64(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id, addr);
        *p_data = data;
        return done;
    }
#endif
}

template<class T> static bool RV_XLEN_PREFIX(mem_wr)(RiscvHartState *p_hart, uint_xlen_t addr, T data) {
    const uint32_t size = sizeof(T);
    static_assert((size == 1) || (size == 2) || (size == 4) || (size == 8), "unsupported object size");
#if TTSIM_RV64_SYSTEM
    if (uint8_t *p_mem = rv64_dtlb_host(p_hart, addr, size, true)) [[likely]] {
        mem_wr<T>(p_mem, data);
        return true;
    }
    return rv64_sys_store(p_hart, addr, &data, size);
#else
    TTSIM_VERIFY(!(addr & (size - 1)), NonContractualBehavior, "unaligned addr=0x%llx size=%d", uint64_t(addr), size);
    if (addr < p_hart->sram_size) {
        mem_wr<T>(&p_hart->p_sram[addr], data);
        dcache_access(p_hart, addr, true);
        return true;
    }
#if XLEN == 32
    if (uint8_t *p_mem = get_local_mem_ptr(p_hart, addr)) {
        mem_wr<T>(p_mem, data);
        return true;
    }
#endif
    if constexpr (size == 4) {
        return tile_mmio_wr32(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id, addr, data);
    }
    if constexpr (size == 8) {
        return tile_mmio_wr64(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id, addr, data);
    }
    TTSIM_ERROR(UnsupportedFunctionality, "addr=0x%llx size=%d", uint64_t(addr), size);
#endif
}

template<class T, class T_SEXT> static void RV_XLEN_PREFIX(load)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_base = bits<19,15>(inst);
    uint32_t r_dst = bits<11,7>(inst);
    int_xlen_t imm = RISCV_I_IMM(inst);

    uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
    T value;
    if (!RV_XLEN_PREFIX(mem_rd)<T>(p_hart, addr, &value)) [[unlikely]] {
        p_hart->pc -= 4; // replay load to handle stall
        return;
    }
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = T_SEXT(value);
    }
}

template<class T> static void RV_XLEN_PREFIX(store)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_base = bits<19,15>(inst);
    uint32_t r_src = bits<24,20>(inst);
    int_xlen_t imm = RISCV_S_IMM(inst);

    uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
    uint_xlen_t value = p_hart->x_regs[r_src];
    if (!RV_XLEN_PREFIX(mem_wr)<T>(p_hart, addr, T(value))) [[unlikely]] {
        p_hart->pc -= 4; // replay store to handle stall
    }
}

template<class T> static void RV_XLEN_PREFIX(atomic)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zaamo");
#else
#if !TTSIM_RV64_SYSTEM
    if constexpr (sizeof(T) == 8) {
        TTSIM_ERROR(UntestedFunctionality, "64-bit atomic");
    }
#endif
    uint32_t funct5 = bits<31,27>(inst); // aq/rl flags are ignored
    uint32_t r_addr = bits<19,15>(inst);
    uint32_t r_src = bits<24,20>(inst);
    uint32_t r_dst = bits<11,7>(inst);

    using S = std::make_signed_t<T>;
    uint_xlen_t addr = p_hart->x_regs[r_addr];
#if TTSIM_RV64_SYSTEM
    TTSIM_VERIFY(!(addr & (sizeof(T) - 1)), UnimplementedFunctionality, "unaligned addr=0x%llx", uint64_t(addr));
    if (!rv64_sys_atomic_dram(p_hart, addr, sizeof(T), funct5 == 0x02)) {
        return; // reject LR/SC/AMO on device/MMIO memory
    }
    if (funct5 == 0x02) { // LR
        TTSIM_VERIFY(!r_src, UnimplementedFunctionality, "LR r_src=%d", r_src);
        T value;
        if (!rv64_sys_load(p_hart, addr, &value, sizeof(T))) {
            return;
        }
        p_hart->reservation_valid = true;
        p_hart->reservation_addr = addr;
        p_hart->reservation_size = sizeof(T);
        p_hart->reservation_value = uint64_t(value);
        rv64_sys_lr_reserve(p_hart, addr, sizeof(T)); // evict this page from all other harts' DTLBs
        if (r_dst) {
            if constexpr (sizeof(T) == 4) {
                p_hart->x_regs[r_dst] = uint_xlen_t(int64_t(int32_t(value)));
            } else {
                p_hart->x_regs[r_dst] = value;
            }
        }
        return;
    }
    if (funct5 == 0x03) { // SC
        bool hit = p_hart->reservation_valid && (p_hart->reservation_addr == addr) && (p_hart->reservation_size == sizeof(T));
        if (hit) { // require that value in memory has not changed underneath us
            T cur_value;
            if (!rv64_sys_load(p_hart, addr, &cur_value, sizeof(T))) {
                return; // re-read (sets trap on fault)
            }
            if (uint64_t(cur_value) != p_hart->reservation_value) {
                hit = false;
            }
        }
        p_hart->reservation_valid = false;
        if (hit) {
            T src_val = T(p_hart->x_regs[r_src]);
            if (!rv64_sys_store(p_hart, addr, &src_val, sizeof(T))) {
                return;
            }
        }
        if (r_dst) {
            p_hart->x_regs[r_dst] = hit ? 0 : 1;
        }
        return;
    }
    T old_val;
    if (!rv64_sys_load(p_hart, addr, &old_val, sizeof(T))) {
        return;
    }
    T src_val = T(p_hart->x_regs[r_src]);
    T new_val;
    switch (funct5) {
        case 0x00: new_val = old_val + src_val; break; // AMOADD
        case 0x01: new_val = src_val; break; // AMOSWAP
        case 0x04: new_val = old_val ^ src_val; break; // AMOXOR
        case 0x08: new_val = old_val | src_val; break; // AMOOR
        case 0x0C: new_val = old_val & src_val; break; // AMOAND
        case 0x10: new_val = T(std::min<S>(S(old_val), S(src_val))); break; // AMOMIN
        case 0x14: new_val = T(std::max<S>(S(old_val), S(src_val))); break; // AMOMAX
        case 0x18: new_val = std::min(old_val, src_val); break; // AMOMINU
        case 0x1C: new_val = std::max(old_val, src_val); break; // AMOMAXU
        default: TTSIM_ERROR(UnimplementedFunctionality, "funct5=%d", funct5);
    }
    if (!rv64_sys_store(p_hart, addr, &new_val, sizeof(T))) {
        return;
    }
#else
    TTSIM_VERIFY(!(addr & (sizeof(T) - 1)), UndefinedBehavior, "unaligned addr=0x%llx", uint64_t(addr));
    TTSIM_VERIFY(addr < p_hart->sram_size, UndefinedBehavior, "addr=0x%llx not in sram", uint64_t(addr));
    auto *p_sram = &p_hart->p_sram[addr];
    T old_val = mem_rd<T>(p_sram);
    T src_val = T(p_hart->x_regs[r_src]);
    switch (funct5) {
        case 0x00: mem_wr<T>(p_sram, old_val + src_val); break; // AMOADD
        case 0x01: mem_wr<T>(p_sram, src_val); break; // AMOSWAP
        case 0x02: // LR
        case 0x03: // SC
#if XLEN == 64
            TTSIM_ERROR(UnsupportedFunctionality, "DM core use of LR/SC is discouraged and unlikely to be reliable");
#else
            TTSIM_ERROR(UndefinedBehavior, "babyrisc supports Zaamo but not LR/SC");
#endif
        case 0x04: mem_wr<T>(p_sram, old_val ^ src_val); break; // AMOXOR
        case 0x08: mem_wr<T>(p_sram, old_val | src_val); break; // AMOOR
        case 0x0C: mem_wr<T>(p_sram, old_val & src_val); break; // AMOAND
        case 0x10: mem_wr<T>(p_sram, T(std::min<S>(old_val, src_val))); break; // AMOMIN
        case 0x14: mem_wr<T>(p_sram, T(std::max<S>(old_val, src_val))); break; // AMOMAX
        case 0x18: mem_wr<T>(p_sram, std::min(old_val, src_val)); break; // AMOMINU
        case 0x1C: mem_wr<T>(p_sram, std::max(old_val, src_val)); break; // AMOMAXU
        default: TTSIM_ERROR(UndefinedBehavior, "funct5=%d", funct5);
    }
#endif

    if (r_dst) {
        if constexpr (sizeof(T) == 4) {
            p_hart->x_regs[r_dst] = int_xlen_t(int32_t(old_val));
        } else {
            p_hart->x_regs[r_dst] = old_val;
        }
    }
#endif
}

template<class T> static void RV_XLEN_PREFIX(f_load)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zfh/F/D/Q");
#elif XLEN == 64
    if constexpr ((sizeof(T) == 4) || (sizeof(T) == 8)) {
        rv64_fpu_load(p_hart, inst, sizeof(T));
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "Zfh/Q are unsupported");
    }
#else
    if constexpr (sizeof(T) == 2) {
        TTSIM_ERROR(UnsupportedFunctionality, "babyrisc non-compliant Zfh extension is out of scope");
    } else if constexpr (sizeof(T) == 4) {
        uint32_t r_base = bits<19,15>(inst);
        uint32_t r_dst = bits<11,7>(inst);
        int_xlen_t imm = RISCV_I_IMM(inst);

        uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
        uint32_t value;
        if (!RV_XLEN_PREFIX(mem_rd)<uint32_t>(p_hart, addr, &value)) [[unlikely]] {
            p_hart->pc -= 4; // replay load to handle stall
            return;
        }
        p_hart->f_regs[r_dst] = value;
    } else if constexpr (sizeof(T) == 8) {
        TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support D");
    } else {
        static_assert(sizeof(T) == 16);
        TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support Q");
    }
#endif
}

template<class T> static void RV_XLEN_PREFIX(f_store)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zfh/F/D/Q");
#elif XLEN == 64
    if constexpr ((sizeof(T) == 4) || (sizeof(T) == 8)) {
        rv64_fpu_store(p_hart, inst, sizeof(T));
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "Zfh/Q are unsupported");
    }
#else
    if constexpr (sizeof(T) == 2) {
        TTSIM_ERROR(UnsupportedFunctionality, "babyrisc non-compliant Zfh extension is out of scope");
    } else if constexpr (sizeof(T) == 4) {
        uint32_t r_base = bits<19,15>(inst);
        uint32_t r_src = bits<24,20>(inst);
        int_xlen_t imm = RISCV_S_IMM(inst);

        uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
        uint32_t value = p_hart->f_regs[r_src];
        if (!RV_XLEN_PREFIX(mem_wr)<uint32_t>(p_hart, addr, value)) [[unlikely]] {
            p_hart->pc -= 4; // replay store to handle stall
        }
    } else if constexpr (sizeof(T) == 8) {
        TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support D");
    } else {
        static_assert(sizeof(T) == 16);
        TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support Q");
    }
#endif
}

template<class T> static void RV_XLEN_PREFIX(v_load)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support V");
#elif XLEN == 64
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#else
    TTSIM_ERROR(UnsupportedFunctionality, "babyrisc non-compliant V extension is explicitly out of scope");
#endif
}

template<class T> static void RV_XLEN_PREFIX(v_store)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support V");
#elif XLEN == 64
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#else
    TTSIM_ERROR(UnsupportedFunctionality, "babyrisc non-compliant V extension is explicitly out of scope");
#endif
}

static void RV_XLEN_PREFIX(auipc)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    int_xlen_t imm = RISCV_U_IMM(inst);
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = p_hart->pc - 4 + imm; // pc is really pc_plus_4 here
    }
}

static void RV_XLEN_PREFIX(lui)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    int_xlen_t imm = RISCV_U_IMM(inst);
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = imm;
    }
}

static void branch_taken(RiscvHartState *p_hart, uint_xlen_t pc, uint_xlen_t target_pc) {
    p_hart->pc = target_pc;
}

static void branch_not_taken(RiscvHartState *p_hart, uint_xlen_t pc) {
}

static void RV_XLEN_PREFIX(jalr)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r_src = bits<19,15>(inst);
    int_xlen_t imm = RISCV_I_IMM(inst);

    uint_xlen_t pc_plus_4 = p_hart->pc; // pc is really pc_plus_4 here
    uint_xlen_t target_pc = (p_hart->x_regs[r_src] + imm) & ~uint_xlen_t(1); // always clear LSB
#if XLEN == 32
    TTSIM_VERIFY(!(target_pc & 3), UndefinedBehavior, "unaligned target_pc=0x%llx", uint64_t(target_pc));
#endif
    if (r_dst) {
        p_hart->x_regs[r_dst] = pc_plus_4;
    }
    uint_xlen_t pc = pc_plus_4 - 4;
    branch_taken(p_hart, pc, target_pc);
}

static void RV_XLEN_PREFIX(jal)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    // XXX can optimize this imm decode
    uint_xlen_t imm = 0;
    imm |= ((inst >> 21) & 0x3FF) << 1; // imm[10:1] at inst[30:21]
    imm |= ((inst >> 20) & 1) << 11; // imm[11] at inst[20]
    imm |= inst & 0x000FF000; // imm[19:12] at inst[19:12]
    imm |= ((inst >> 31) & 1) << 20; // imm[20] at inst[31]
    imm = int_xlen_t(int32_t(imm << 11)) >> 11; // sign extend imm[20] to imm[31:21] or imm[63:21]

    uint_xlen_t pc_plus_4 = p_hart->pc;
    if (r_dst) {
        p_hart->x_regs[r_dst] = pc_plus_4;
    }
    uint_xlen_t pc = pc_plus_4 - 4;
    uint_xlen_t target_pc = pc + imm;
#if XLEN == 32
    TTSIM_VERIFY(!(target_pc & 3), UndefinedBehavior, "unaligned target_pc=0x%llx", uint64_t(target_pc));
#endif
    branch_taken(p_hart, pc, target_pc);
}

struct branch_op_beq {
    static bool is_taken(uint_xlen_t src0, uint_xlen_t src1) { return src0 == src1; }
};
struct branch_op_bne {
    static bool is_taken(uint_xlen_t src0, uint_xlen_t src1) { return src0 != src1; }
};
struct branch_op_blt {
    static bool is_taken(uint_xlen_t src0, uint_xlen_t src1) { return int_xlen_t(src0) < int_xlen_t(src1); }
};
struct branch_op_bge {
    static bool is_taken(uint_xlen_t src0, uint_xlen_t src1) { return int_xlen_t(src0) >= int_xlen_t(src1); }
};
struct branch_op_bltu {
    static bool is_taken(uint_xlen_t src0, uint_xlen_t src1) { return src0 < src1; }
};
struct branch_op_bgeu {
    static bool is_taken(uint_xlen_t src0, uint_xlen_t src1) { return src0 >= src1; }
};

template<class branch_op> static void RV_XLEN_PREFIX(branch)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_src0 = bits<19,15>(inst);
    uint32_t r_src1 = bits<24,20>(inst);
    // XXX can optimize this imm decode; could also move inside "if taken"?
    uint_xlen_t imm = 0;
    imm |= ((inst >> 8) & 15) << 1; // imm[4:1] at inst[11:8]
    imm |= ((inst >> 25) & 0x3F) << 5; /// imm[10:5] at inst[30:25]
    imm |= ((inst >> 7) & 1) << 11; // imm[11] at inst[7]
    imm |= ((inst >> 31) & 1) << 12; // imm[12] at inst[31]
    imm = int_xlen_t(int32_t(imm << 19)) >> 19; // sign extend imm[12] to imm[31:12] or imm[63:12]

    uint_xlen_t src0 = p_hart->x_regs[r_src0];
    uint_xlen_t src1 = p_hart->x_regs[r_src1];
    uint_xlen_t pc = p_hart->pc - 4; // pc is really pc_plus_4 here
    if (branch_op::is_taken(src0, src1)) {
        uint_xlen_t target_pc = pc + imm;
#if XLEN == 32
        TTSIM_VERIFY(!(target_pc & 3), UndefinedBehavior, "unaligned target_pc=0x%llx", uint64_t(target_pc));
#endif
        branch_taken(p_hart, pc, target_pc);
    } else {
        branch_not_taken(p_hart, pc);
    }
}

static void RV_XLEN_PREFIX(fence)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UnsupportedFunctionality, "fence instructions do not enforce memory ordering on Wormhole and should not be used");
#elif TTSIM_RV64_SYSTEM
    // Any FENCE other than PAUSE is currently a NOP
    if (inst == 0x0100000F) {
        rv64_sys_pause_yield(p_hart);
    }
#else
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(!r_dst, UnsupportedFunctionality, "r_dst=%d", r_dst);
    uint32_t r_src = bits<19,15>(inst);
    TTSIM_VERIFY(!r_src, UnsupportedFunctionality, "r_src=%d", r_src);
    uint32_t fence_mode = bits<31,20>(inst); // concatenation of many different fence bits

    // For future reference: PAUSE instruction appears to be 0x0100000F, i.e., fence_mode == 0x010
    // This is probably a safe NOP on silicon, but don't know for sure and don't know its performance
    // Can generate it via __builtin_riscv_pause()
#if XLEN == 32
    // 0xFF is invalidate_l1_cache() on BH; others are from std::atomic
    if ((fence_mode == 0x023) || (fence_mode == 0x031) || (fence_mode == 0x033) || (fence_mode == 0x0FF)) {
        return dcache_invalidate(p_hart); // all fences flush the D$
    }
#endif
#if XLEN == 64
    if ((fence_mode == 0x023) || (fence_mode == 0x031) || (fence_mode == 0x033) || (fence_mode == 0x0FF)) {
        return; // XXX cases seen so far
    }
#endif
    TTSIM_ERROR(UnsupportedFunctionality, "fence_mode=0x%x", fence_mode);
#endif
}

static void RV_XLEN_PREFIX(fence_i)(RiscvHartState *p_hart, uint32_t inst) {
#if XLEN == 64
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(!r_dst, UnimplementedFunctionality, "r_dst=%d", r_dst);
    uint32_t r_src = bits<19,15>(inst);
    TTSIM_VERIFY(!r_src, UnimplementedFunctionality, "r_src=%d", r_src);
    uint32_t imm = bits<31,20>(inst);
    TTSIM_VERIFY(!imm, UnimplementedFunctionality, "imm=%d", imm);

    RV_XLEN_PREFIX(icache_invalidate)(p_hart);
#else
    TTSIM_ERROR(UnsupportedFunctionality, "fence.i does not flush the instruction cache on babyrisc and should not be used");
#endif
}

static void RV_XLEN_PREFIX(ecall_ebreak)(RiscvHartState *p_hart, uint32_t inst) {
#if TTSIM_RV64_SYSTEM
    if (inst == 0x00000073) { // ECALL
        uint64_t cause = (p_hart->priv == PRIV_M) ? EXC_ECALL_M :
                         (p_hart->priv == PRIV_S) ? EXC_ECALL_S : EXC_ECALL_U;
        rv64_sys_raise(p_hart, cause, 0);
    } else if (inst == 0x00100073) { // EBREAK
        rv64_sys_raise(p_hart, EXC_BREAKPOINT, p_hart->pc - 4);
    } else if (inst == 0x30200073) { // MRET
        if (p_hart->priv != PRIV_M) {
            return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
        }
        rv64_sys_xret(p_hart, true);
    } else if (inst == 0x10200073) { // SRET
        if ((p_hart->priv < PRIV_S) || ((p_hart->priv == PRIV_S) && (p_hart->mstatus & MSTATUS_TSR))) {
            return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
        }
        rv64_sys_xret(p_hart, false);
    } else if (inst == 0x10500073) { // WFI
        if ((p_hart->priv != PRIV_M) && (p_hart->mstatus & MSTATUS_TW)) {
            return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
        }
        p_hart->wfi_retired = true;
        p_hart->steps_left = 0;
    } else if ((inst & 0xFE007FFFu) == 0x12000073u) { // SFENCE.VMA rs1, rs2
        if ((p_hart->priv < PRIV_S) || ((p_hart->priv == PRIV_S) && (p_hart->mstatus & MSTATUS_TVM))) {
            return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
        }
        rv64_sys_sfence(p_hart, p_hart->x_regs[bits<19,15>(inst)], p_hart->x_regs[bits<24,20>(inst)]);
    } else {
        rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
    }
#else
    if (inst == 0x73) {
        p_hart->x_regs[10] = libttsim_syscall(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id,
            p_hart->x_regs[17], p_hart->x_regs[10], p_hart->x_regs[11], p_hart->x_regs[12]);
#if XLEN == 64
    } else if (inst == 0xFC000073) {
        // tt.cache.cflush.d.l1 zero -- writes back and invalidates entire L1 D$ (src reg in bits 19:15)
    } else if (inst == 0xFC200073) {
        TTSIM_ERROR(UnsupportedFunctionality, "tt.cache.cdiscard.d.l1 use is discouraged and unlikely to be reliable");
#endif
    } else {
        TTSIM_ERROR(UnsupportedFunctionality, "inst=0x%x", inst);
    }
#endif
}

#if TT_ARCH_VERSION >= 1
static uint_xlen_t read_csr(RiscvHartState *p_hart, uint32_t csr) {
    switch (csr) {
#if XLEN == 64
        case CSR_MSTATUS: return p_hart->mstatus;
        case CSR_MIE: return p_hart->mie;
        case CSR_MTVEC: return p_hart->mtvec;
        case CSR_MHARTID: return p_hart->riscv_id & 0x7FFFFFFF;
#else
        case CSR_CFG0: return p_hart->chicken_bits;
#endif
#if TT_ARCH_VERSION == 1
        default: TTSIM_ERROR(UnsupportedFunctionality, "csr=0x%x", csr);
#else
        default: TTSIM_ERROR(UnimplementedFunctionality, "csr=0x%x", csr);
#endif
    }
}

static void write_csr(RiscvHartState *p_hart, uint32_t csr, uint_xlen_t data) {
    switch (csr) {
#if XLEN == 64
        case CSR_MSTATUS: // XXX only allow writes that don't change value for now
            TTSIM_VERIFY(data == p_hart->mstatus, UnimplementedFunctionality, "mstatus: data=0x%llx", data);
            break;
        case CSR_MIE: // XXX only allow writes that don't change value for now
            TTSIM_VERIFY(data == p_hart->mie, UnimplementedFunctionality, "mie: data=0x%llx", data);
            break;
        case CSR_MTVEC:
            // XXX how many address bits are implemented?
            if (!(data & 1)) { // direct mode (should be 4B aligned)
                TTSIM_VERIFY(!(data & 3) && (data <= 0xFFFF), UnimplementedFunctionality, "mtvec: data=0x%llx", data);
            } else { // vectored mode (should be 256B aligned)
                TTSIM_VERIFY(!(data & 0xFE) && (data <= 0xFFFF), UnimplementedFunctionality, "mtvec: data=0x%llx", data);
            }
            p_hart->mtvec = data;
            break;
#else
        case CSR_CFG0:
#if TT_ARCH_VERSION == 1
            TTSIM_VERIFY(!(data & ~0x104000A), UnsupportedFunctionality, "chicken_bits: data=0x%x", data);
#else
            TTSIM_VERIFY(!(data & ~0x40001), UnimplementedFunctionality, "chicken_bits: data=0x%x", data);
#endif
            p_hart->chicken_bits = data;
            break;
#endif
#if TT_ARCH_VERSION == 1
        default: TTSIM_ERROR(UnsupportedFunctionality, "csr=0x%x", csr);
#else
        default: TTSIM_ERROR(UnimplementedFunctionality, "csr=0x%x", csr);
#endif
    }
}
#endif

#if TTSIM_RV64_SYSTEM
// Shared Zicsr read-modify-write. op: 0=write, 1=set, 2=clear.
// An illegal/absent CSR or privilege violation raises an illegal-instruction trap and writes no rd.
static void rv64_csr_op(RiscvHartState *p_hart, uint32_t inst, uint64_t src, bool do_write, uint32_t op) {
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t csr = bits<31,20>(inst);

    // Privilege + read-only enforcement (priv-spec 2.1, "CSR Field Specifications").
    // csr[9:8] encodes the lowest privilege that may access the CSR; csr[11:10]==3 marks it read-only.
    // A read-only write attempt (do_write true) and any access from too low a privilege both raise an
    // illegal-instruction trap before any read/write side effect.
    if (((csr >> 8) & 3) > p_hart->priv) {
        return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
    }
    if (do_write && ((csr >> 10) & 3) == 3) {
        return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
    }

    // S-mode access to satp traps when mstatus.TVM is set. M-mode is exempt.
    if ((csr == CSR_SATP) && (p_hart->priv == PRIV_S) && (p_hart->mstatus & MSTATUS_TVM)) {
        return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
    }

    // mcounteren/scounteren gate reads of the unprivileged cycle/time/instret/hpm shadows
    // (0xC00..0xC1F). A bit clear in mcounteren makes the counter inaccessible below M-mode; a bit
    // clear in scounteren makes it inaccessible below S-mode. M-mode is always permitted.
    if (csr >= 0xC00 && csr <= 0xC1F && unsigned(p_hart->priv) < unsigned(PRIV_M)) {
        uint32_t bit = csr - 0xC00;
        if (!((p_hart->mcounteren >> bit) & 1) ||
            ((p_hart->priv == PRIV_U) && !((p_hart->scounteren >> bit) & 1))) {
            return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
        }
    }

    // FP CSRs (fflags/frm/fcsr) are inaccessible while mstatus.FS == Off.
    if (((csr == CSR_FFLAGS) || (csr == CSR_FRM) || (csr == CSR_FCSR)) && !(p_hart->mstatus & MSTATUS_FS)) {
        return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
    }

    bool ok = true;
    uint64_t old = 0;
    if ((r_dst != 0) || (op != 0)) { // CSRRW with rd==x0 performs no read (no read side effects)
        old = rv64_sys_read_csr(p_hart, csr, &ok);
        if (!ok) {
            return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
        }
    }
    uint64_t newv = (op == 0) ? src : (op == 1) ? (old | src) : (old & ~src);
    if (do_write) {
        rv64_sys_write_csr(p_hart, csr, newv, &ok);
        if (!ok) {
            return rv64_sys_raise(p_hart, EXC_ILLEGAL_INST, inst);
        }
    }
    if (r_dst) {
        p_hart->x_regs[r_dst] = old;
    }
}
#endif

static void RV_XLEN_PREFIX(csrrw)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zicsr");
#elif TTSIM_RV64_SYSTEM
    rv64_csr_op(p_hart, inst, p_hart->x_regs[bits<19,15>(inst)], true, 0);
#else
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(!r_dst, UntestedFunctionality, "r_dst=%d", r_dst);
    uint32_t r_src = bits<19,15>(inst);
    TTSIM_VERIFY(r_src, UntestedFunctionality, "r_src=%d", r_src);
    uint32_t csr = bits<31,20>(inst);

    if (r_dst) {
        uint_xlen_t old_value = read_csr(p_hart, csr);
        write_csr(p_hart, csr, p_hart->x_regs[r_src]);
        p_hart->x_regs[r_dst] = old_value;
    } else {
        write_csr(p_hart, csr, p_hart->x_regs[r_src]);
    }
#endif
}

static void RV_XLEN_PREFIX(csrrs)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zicsr");
#elif TTSIM_RV64_SYSTEM
    uint32_t r_src = bits<19,15>(inst);
    rv64_csr_op(p_hart, inst, r_src ? p_hart->x_regs[r_src] : 0, r_src != 0, 1);
#else
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t r_src = bits<19,15>(inst);
    uint32_t csr = bits<31,20>(inst);

    uint_xlen_t old_value = read_csr(p_hart, csr);
    if (r_src) {
        write_csr(p_hart, csr, old_value | p_hart->x_regs[r_src]);
    }
    if (r_dst) {
        p_hart->x_regs[r_dst] = old_value;
    }
#endif
}

static void RV_XLEN_PREFIX(csrrc)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zicsr");
#elif TTSIM_RV64_SYSTEM
    uint32_t r_src = bits<19,15>(inst);
    rv64_csr_op(p_hart, inst, r_src ? p_hart->x_regs[r_src] : 0, r_src != 0, 2);
#else
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(!r_dst, UntestedFunctionality, "r_dst=%d", r_dst);
    uint32_t r_src = bits<19,15>(inst);
    TTSIM_VERIFY(r_src, UntestedFunctionality, "r_src=%d", r_src);
    uint32_t csr = bits<31,20>(inst);

    uint_xlen_t src = p_hart->x_regs[r_src];
    uint_xlen_t old_value = read_csr(p_hart, csr);
    if (r_src) {
        write_csr(p_hart, csr, old_value & ~src);
    }
    if (r_dst) {
        p_hart->x_regs[r_dst] = old_value;
    }
#endif
}

static void RV_XLEN_PREFIX(csrrwi)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zicsr");
#elif TTSIM_RV64_SYSTEM
    rv64_csr_op(p_hart, inst, bits<19,15>(inst), true, 0);
#else
    TTSIM_ERROR_NOFMT(UntestedFunctionality);
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t imm = bits<19,15>(inst);
    uint32_t csr = bits<31,20>(inst);

    if (r_dst) {
        uint_xlen_t old_value = read_csr(p_hart, csr);
        write_csr(p_hart, csr, imm);
        p_hart->x_regs[r_dst] = old_value;
    } else {
        write_csr(p_hart, csr, imm);
    }
#endif
}

static void RV_XLEN_PREFIX(csrrsi)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zicsr");
#elif TTSIM_RV64_SYSTEM
    uint32_t imm = bits<19,15>(inst);
    rv64_csr_op(p_hart, inst, imm, imm != 0, 1);
#else
    TTSIM_ERROR_NOFMT(UntestedFunctionality);
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t imm = bits<19,15>(inst);
    uint32_t csr = bits<31,20>(inst);

    uint_xlen_t old_value = read_csr(p_hart, csr);
    if (imm) {
        write_csr(p_hart, csr, old_value | imm);
    }
    if (r_dst) {
        p_hart->x_regs[r_dst] = old_value;
    }
#endif
}

static void RV_XLEN_PREFIX(csrrci)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zicsr");
#elif TTSIM_RV64_SYSTEM
    uint32_t imm = bits<19,15>(inst);
    rv64_csr_op(p_hart, inst, imm, imm != 0, 2);
#else
    TTSIM_ERROR_NOFMT(UntestedFunctionality);
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t imm = bits<19,15>(inst);
    uint32_t csr = bits<31,20>(inst);

    uint_xlen_t old_value = read_csr(p_hart, csr);
    if (imm) {
        write_csr(p_hart, csr, old_value & ~uint_xlen_t(imm));
    }
    if (r_dst) {
        p_hart->x_regs[r_dst] = old_value;
    }
#endif
}

#if XLEN == 64
static void RV_XLEN_PREFIX(custom_0)(RiscvHartState *p_hart, uint32_t inst) {
    TTSIM_ERROR(UnimplementedFunctionality, "inst=0x%x", inst);
}

static void RV_XLEN_PREFIX(custom_1)(RiscvHartState *p_hart, uint32_t inst) {
    TTSIM_ERROR(UnimplementedFunctionality, "inst=0x%x", inst);
}

static void RV_XLEN_PREFIX(custom_2)(RiscvHartState *p_hart, uint32_t inst) {
    TTSIM_ERROR(UnimplementedFunctionality, "inst=0x%x", inst);
}

static void RV_XLEN_PREFIX(custom_3)(RiscvHartState *p_hart, uint32_t inst) {
    TTSIM_ERROR(UnimplementedFunctionality, "inst=0x%x", inst);
}
#endif

#if XLEN == 32
static void RV_XLEN_PREFIX(tti)(RiscvHartState *p_hart, uint32_t inst) {
    TTSIM_VERIFY(p_hart->tile_type == 'T', UndefinedBehavior, "tile_type=%c", p_hart->tile_type);
    uint32_t riscv_id = p_hart->riscv_id;
    bool bypass_mop_expander = false;
    uint32_t tensix_id;
    uint32_t pipe;
    if (riscv_id == RV32_ID_BRISC) {
        pipe = 0; // this writes the MMIO aperture for pipe 0
        bypass_mop_expander = true;
    } else {
        static_assert(RV32_ID_TRISC1 == RV32_ID_TRISC0 + 1);
        static_assert(RV32_ID_TRISC2 == RV32_ID_TRISC0 + 2);
        TTSIM_VERIFY((riscv_id >= RV32_ID_TRISC0) && (riscv_id <= RV32_ID_TRISC2), UndefinedBehavior, "riscv_id=%d", riscv_id);
        pipe = riscv_id - RV32_ID_TRISC0;
    }
    tensix_id = 0;

    TensixState *p_tensix = &g_t_tiles[p_hart->tile_id].tensix[tensix_id];
    if (tensix_can_push_inst(p_tensix, pipe)) {
        uint32_t tensix_inst = (inst << 30) | (inst >> 2);
        tensix_push_inst(p_tensix, pipe, tensix_inst, bypass_mop_expander);
    } else {
        p_hart->pc -= 4; // replay TTI instruction
    }
}
#endif

static void RV_XLEN_PREFIX(unimplemented)(RiscvHartState *p_hart, uint32_t inst) {
#if !TTSIM_RV64_SYSTEM
    TTSIM_ERROR(UndefinedBehavior, "could not decode instruction inst=0x%x at pc=0x%llx", inst, uint64_t(p_hart->pc - 4));
#else
    TTSIM_ERROR(UnimplementedFunctionality, "could not decode instruction inst=0x%x at pc=0x%llx", inst, uint64_t(p_hart->pc - 4));
#endif
}

#if XLEN == 64
static void RV_XLEN_PREFIX(c_addi4spn)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = 8 | bits<4,2>(inst);
    uint32_t imm = (bits<6,6>(inst) << 2) | (bits<5,5>(inst) << 3) | (bits<12,11>(inst) << 4) | (bits<10,7>(inst) << 6);
    TTSIM_VERIFY(imm, NonContractualBehavior, "imm=%d", imm); // "the code points with nzuimm=0 are reserved"

    p_hart->x_regs[r_dst] = p_hart->x_regs[X_SP] + imm;
}

static void RV_XLEN_PREFIX(c_addi)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    int32_t imm = bits<6,2>(inst) | (signed_bits<12,12>(inst) << 5);
    if (!r_dst && !imm) {
        return; // C.NOP special case
    }
    TTSIM_VERIFY(r_dst, UnimplementedFunctionality, "r_dst=%d", r_dst);
    TTSIM_VERIFY(imm, UnimplementedFunctionality, "imm=%d", imm);

    p_hart->x_regs[r_dst] += int_xlen_t(imm);
}

static void RV_XLEN_PREFIX(c_slli)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(r_dst, UnimplementedFunctionality, "r_dst=%d", r_dst);
    uint32_t imm = bits<6,2>(inst) | (bits<12,12>(inst) << 5);
    TTSIM_VERIFY(imm, UnimplementedFunctionality, "imm=%d", imm);

    p_hart->x_regs[r_dst] <<= imm;
}

static void RV_XLEN_PREFIX(c_fld)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_base = 8 | bits<9,7>(inst);
    uint32_t r_dst = 8 | bits<4,2>(inst);
    uint32_t imm = (bits<12,10>(inst) << 3) | (bits<6,5>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
    uint64_t value;
    if (!RV_XLEN_PREFIX(mem_rd)<uint64_t>(p_hart, addr, &value)) {
        return;
    }
    p_hart->f_regs[r_dst] = value;
    rv64_fpu_mark_dirty(p_hart);
}

static void RV_XLEN_PREFIX(c_addiw)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    int32_t imm = bits<6,2>(inst) | (signed_bits<12,12>(inst) << 5);
    TTSIM_VERIFY(r_dst, UnimplementedFunctionality, "r_dst=%d", r_dst);

    uint32_t src = uint32_t(p_hart->x_regs[r_dst]);
    uint32_t value = src + imm;
    p_hart->x_regs[r_dst] = int64_t(int32_t(value));
}

static void RV_XLEN_PREFIX(c_fldsp)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    uint32_t imm = (bits<6,5>(inst) << 3) | (bits<12,12>(inst) << 5) | (bits<4,2>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[X_SP] + imm;
    uint64_t value;
    if (!RV_XLEN_PREFIX(mem_rd)<uint64_t>(p_hart, addr, &value)) {
        return;
    }
    p_hart->f_regs[r_dst] = value;
    rv64_fpu_mark_dirty(p_hart);
}

static void RV_XLEN_PREFIX(c_lw)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_base = 8 | bits<9,7>(inst);
    uint32_t r_dst = 8 | bits<4,2>(inst);
    uint32_t imm = (bits<6,6>(inst) << 2) | (bits<12,10>(inst) << 3) | (bits<5,5>(inst) << 6);

    uint64_t addr = p_hart->x_regs[r_base] + imm;
    uint32_t value;
    if (!RV_XLEN_PREFIX(mem_rd)<uint32_t>(p_hart, addr, &value)) {
#if TTSIM_RV64_SYSTEM
        return;
#else
        TTSIM_ERROR(UnimplementedFunctionality, "mem_rd failed");
#endif
    }
    p_hart->x_regs[r_dst] = int64_t(int32_t(value));
}

static void RV_XLEN_PREFIX(c_li)(RiscvHartState *p_hart, uint32_t inst) {
    int32_t imm = bits<6,2>(inst) | (signed_bits<12,12>(inst) << 5);
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(r_dst, UnimplementedFunctionality, "r_dst=%d", r_dst);

    p_hart->x_regs[r_dst] = imm;
}

static void RV_XLEN_PREFIX(c_lwsp)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(r_dst, UnimplementedFunctionality, "r_dst=%d", r_dst);
    uint32_t imm = (bits<6,4>(inst) << 2) | (bits<12,12>(inst) << 5) | (bits<3,2>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[X_SP] + imm;
    uint32_t value;
    if (!RV_XLEN_PREFIX(mem_rd)<uint32_t>(p_hart, addr, &value)) {
#if TTSIM_RV64_SYSTEM
        return;
#else
        TTSIM_ERROR(UnimplementedFunctionality, "mem_rd failed");
#endif
    }
    p_hart->x_regs[r_dst] = int64_t(int32_t(value));
}

static void RV_XLEN_PREFIX(c_ld)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_base = 8 | bits<9,7>(inst);
    uint32_t r_dst = 8 | bits<4,2>(inst);
    uint32_t imm = (bits<12,10>(inst) << 3) | (bits<6,5>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
    uint64_t value;
    if (!RV_XLEN_PREFIX(mem_rd)<uint64_t>(p_hart, addr, &value)) [[unlikely]] {
#if TTSIM_RV64_SYSTEM
        return;
#else
        TTSIM_ERROR(UnimplementedFunctionality, "mem_rd failed");
#endif
    }
    p_hart->x_regs[r_dst] = value;
}

static void RV_XLEN_PREFIX(c_addi16sp_lui)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    if (r_dst == X_SP) {
        int32_t imm = 0;
        imm |= bits<6,6>(inst) << 4;
        imm |= bits<2,2>(inst) << 5;
        imm |= bits<5,5>(inst) << 6;
        imm |= bits<4,3>(inst) << 7;
        imm |= signed_bits<12,12>(inst) << 9;
        TTSIM_VERIFY(imm, UnimplementedFunctionality, "c_addi16sp: imm=%d", imm);

        p_hart->x_regs[X_SP] += int_xlen_t(imm);
    } else {
        TTSIM_VERIFY(r_dst, UnimplementedFunctionality, "r_dst=%d", r_dst);
        int32_t imm = (bits<6,2>(inst) << 12) | (signed_bits<12,12>(inst) << 17);
        TTSIM_VERIFY(imm, UnimplementedFunctionality, "c_lui: imm=%d", imm);

        p_hart->x_regs[r_dst] = int_xlen_t(imm);
    }
}

static void RV_XLEN_PREFIX(c_ldsp)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t imm = (bits<6,5>(inst) << 3) | (bits<12,12>(inst) << 5) | (bits<4,2>(inst) << 6);
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(r_dst, UnimplementedFunctionality, "c_ldsp: r_dst=%d", r_dst);

    uint_xlen_t addr = p_hart->x_regs[X_SP] + imm;
    uint64_t value;
    if (!RV_XLEN_PREFIX(mem_rd)<uint64_t>(p_hart, addr, &value)) [[unlikely]] {
        TTSIM_ERROR(UnimplementedFunctionality, "mem_rd failed");
    }
    p_hart->x_regs[r_dst] = value;
}

static void RV_XLEN_PREFIX(c_misc_alu)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = 8 | bits<9,7>(inst);
    uint32_t sel = bits<11,10>(inst);
    if ((sel == 0) || (sel == 1)) { // C.SRLI/C.SRAI
        uint32_t imm = bits<6,2>(inst) | (bits<12,12>(inst) << 5);
        TTSIM_VERIFY(imm, UnimplementedFunctionality, "c_sr*i: imm=%d", imm);
        if (sel == 0) {
            p_hart->x_regs[r_dst] >>= imm;
        } else {
            p_hart->x_regs[r_dst] = int_xlen_t(p_hart->x_regs[r_dst]) >> imm;
        }
    } else if (sel == 2) { // C.ANDI
        int32_t imm = bits<6,2>(inst) | (signed_bits<12,12>(inst) << 5);
        p_hart->x_regs[r_dst] = int_xlen_t(p_hart->x_regs[r_dst]) & int_xlen_t(imm);
    } else { // sel == 3
        uint32_t r_src = 8 | bits<4,2>(inst);
        if (!bits<12,12>(inst)) {
            switch (bits<6,5>(inst)) {
                case 0: p_hart->x_regs[r_dst] -= p_hart->x_regs[r_src]; break; // C.SUB
                case 1: p_hart->x_regs[r_dst] ^= p_hart->x_regs[r_src]; break; // C.XOR
                case 2: p_hart->x_regs[r_dst] |= p_hart->x_regs[r_src]; break; // C.OR
                default: p_hart->x_regs[r_dst] &= p_hart->x_regs[r_src]; break; // C.AND
            }
        } else {
            uint32_t src = p_hart->x_regs[r_src];
            uint32_t dst = p_hart->x_regs[r_dst];
            switch (bits<6,5>(inst)) {
                case 0: p_hart->x_regs[r_dst] = int64_t(int32_t(dst - src)); break; // C.SUBW
                case 1: p_hart->x_regs[r_dst] = int64_t(int32_t(dst + src)); break; // C.ADDW
                default: TTSIM_ERROR(UnimplementedFunctionality, "could not decode instruction inst=0x%x at pc=0x%llx", inst & 0xFFFF, uint64_t(p_hart->pc - 2));
            }
        }
    }
}

static void RV_XLEN_PREFIX(c_jr_mv_ebreak_jalr_add)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(r_dst, UnimplementedFunctionality, "r_dst=%d", r_dst);
    uint32_t r_src = bits<6,2>(inst);
    if (bits<12,12>(inst)) {
        if (r_src) { // C.ADD
            p_hart->x_regs[r_dst] += p_hart->x_regs[r_src];
        } else { // C.JALR
            uint_xlen_t pc = p_hart->pc - 2; // pc is really pc_plus_2 here
            uint_xlen_t target_pc = p_hart->x_regs[r_dst] & ~uint_xlen_t(1); // always clear LSB
            p_hart->x_regs[1] = pc + 2;
            branch_taken(p_hart, pc, target_pc);
        }
    } else {
        if (r_src) { // C.MV
            p_hart->x_regs[r_dst] = p_hart->x_regs[r_src];
        } else { // C.JR
            uint_xlen_t pc = p_hart->pc - 2; // pc is really pc_plus_2 here
            uint_xlen_t target_pc = p_hart->x_regs[r_dst] & ~uint_xlen_t(1); // always clear LSB
            branch_taken(p_hart, pc, target_pc);
        }
    }
}

static void RV_XLEN_PREFIX(c_fsd)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_base = 8 | bits<9,7>(inst);
    uint32_t r_src = 8 | bits<4,2>(inst);
    uint32_t imm = (bits<12,10>(inst) << 3) | (bits<6,5>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
    if (!RV_XLEN_PREFIX(mem_wr)<uint64_t>(p_hart, addr, p_hart->f_regs[r_src])) {
        return;
    }
}

static void RV_XLEN_PREFIX(c_j)(RiscvHartState *p_hart, uint32_t inst) {
    int32_t imm = 0;
    imm |= bits<5,3>(inst) << 1;
    imm |= bits<11,11>(inst) << 4;
    imm |= bits<2,2>(inst) << 5;
    imm |= bits<7,7>(inst) << 6;
    imm |= bits<6,6>(inst) << 7;
    imm |= bits<10,9>(inst) << 8;
    imm |= bits<8,8>(inst) << 10;
    imm |= signed_bits<12,12>(inst) << 11;

    uint_xlen_t pc = p_hart->pc - 2; // pc is really pc_plus_2 here
    uint_xlen_t target_pc = pc + int_xlen_t(imm);
    branch_taken(p_hart, pc, target_pc);
}

static void RV_XLEN_PREFIX(c_fsdsp)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_src = bits<6,2>(inst);
    uint32_t imm = (bits<12,10>(inst) << 3) | (bits<9,7>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[X_SP] + imm;
    if (!RV_XLEN_PREFIX(mem_wr)<uint64_t>(p_hart, addr, p_hart->f_regs[r_src])) {
        return;
    }
}

static void RV_XLEN_PREFIX(c_sw)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_base = 8 | bits<9,7>(inst);
    uint32_t r_src = 8 | bits<4,2>(inst);
    uint32_t imm = (bits<6,6>(inst) << 2) | (bits<12,10>(inst) << 3) | (bits<5,5>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
    uint_xlen_t value = p_hart->x_regs[r_src];
    if (!RV_XLEN_PREFIX(mem_wr)<uint32_t>(p_hart, addr, value)) [[unlikely]] {
#if TTSIM_RV64_SYSTEM
        return;
#else
        TTSIM_ERROR(UnimplementedFunctionality, "mem_wr failed");
#endif
    }
}

static void RV_XLEN_PREFIX(c_beqz)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_src = 8 | bits<9,7>(inst);
    int32_t imm = (bits<4,3>(inst) << 1) | (bits<11,10>(inst) << 3) | (bits<2,2>(inst) << 5) |
                  (bits<6,5>(inst) << 6) | (signed_bits<12,12>(inst) << 8);

    uint_xlen_t src = p_hart->x_regs[r_src];
    uint_xlen_t pc = p_hart->pc - 2; // pc is really pc_plus_2 here
    if (!src) {
        uint_xlen_t target_pc = pc + int_xlen_t(imm);
        branch_taken(p_hart, pc, target_pc);
    } else {
        branch_not_taken(p_hart, pc);
    }
}

static void RV_XLEN_PREFIX(c_swsp)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_src = bits<6,2>(inst);
    uint32_t imm = (bits<12,9>(inst) << 2) | (bits<8,7>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[X_SP] + imm;
    uint32_t value = uint32_t(p_hart->x_regs[r_src]);
    if (!RV_XLEN_PREFIX(mem_wr)<uint32_t>(p_hart, addr, value)) {
#if TTSIM_RV64_SYSTEM
        return;
#else
        TTSIM_ERROR(UnimplementedFunctionality, "mem_wr failed");
#endif
    }
}

static void RV_XLEN_PREFIX(c_sd)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_base = 8 | bits<9,7>(inst);
    uint32_t r_src = 8 | bits<4,2>(inst);
    uint32_t imm = (bits<12,10>(inst) << 3) | (bits<6,5>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[r_base] + imm;
    uint_xlen_t value = p_hart->x_regs[r_src];
    if (!RV_XLEN_PREFIX(mem_wr)<uint64_t>(p_hart, addr, value)) [[unlikely]] {
#if TTSIM_RV64_SYSTEM
        return;
#else
        TTSIM_ERROR(UnimplementedFunctionality, "mem_wr failed");
#endif
    }
}

static void RV_XLEN_PREFIX(c_bnez)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_src = 8 | bits<9,7>(inst);
    int32_t imm = (bits<4,3>(inst) << 1) | (bits<11,10>(inst) << 3) | (bits<2,2>(inst) << 5) |
                  (bits<6,5>(inst) << 6) | (signed_bits<12,12>(inst) << 8);

    uint_xlen_t src = p_hart->x_regs[r_src];
    uint_xlen_t pc = p_hart->pc - 2; // pc is really pc_plus_2 here
    if (src) {
        uint_xlen_t target_pc = pc + int_xlen_t(imm);
        branch_taken(p_hart, pc, target_pc);
    } else {
        branch_not_taken(p_hart, pc);
    }
}

static void RV_XLEN_PREFIX(c_sdsp)(RiscvHartState *p_hart, uint32_t inst) {
    uint32_t r_src = bits<6,2>(inst);
    uint32_t imm = (bits<12,10>(inst) << 3) | (bits<9,7>(inst) << 6);

    uint_xlen_t addr = p_hart->x_regs[X_SP] + imm;
    uint_xlen_t value = p_hart->x_regs[r_src];
    if (!RV_XLEN_PREFIX(mem_wr)<uint64_t>(p_hart, addr, value)) [[unlikely]] {
#if TTSIM_RV64_SYSTEM
        return;
#else
        TTSIM_ERROR(UnimplementedFunctionality, "mem_wr failed");
#endif
    }
}

static void RV_XLEN_PREFIX(unimplemented_c)(RiscvHartState *p_hart, uint32_t inst) {
    TTSIM_ERROR(UnimplementedFunctionality, "could not decode instruction inst=0x%x at pc=0x%llx", inst & 0xFFFF, uint64_t(p_hart->pc - 2));
}
#endif

using DecodeAndExecuteFunc = void (*)(RiscvHartState *p_hart, uint32_t inst);

static DecodeAndExecuteFunc s_decode_table[256] = {
#include "_out/riscv_decode.h"
};

#if XLEN == 64
static DecodeAndExecuteFunc s_rv64c_decode_table[32] = {
    RV_XLEN_PREFIX(c_addi4spn),              // 000...00
    RV_XLEN_PREFIX(c_addi),                  // 000...01
    RV_XLEN_PREFIX(c_slli),                  // 000...10
    RV_XLEN_PREFIX(unimplemented_c),         // 000...11

    RV_XLEN_PREFIX(c_fld),                   // 001...00
    RV_XLEN_PREFIX(c_addiw),                 // 001...01
    RV_XLEN_PREFIX(c_fldsp),                 // 001...10
    RV_XLEN_PREFIX(unimplemented_c),         // 001...11

    RV_XLEN_PREFIX(c_lw),                    // 010...00
    RV_XLEN_PREFIX(c_li),                    // 010...01
    RV_XLEN_PREFIX(c_lwsp),                  // 010...10
    RV_XLEN_PREFIX(unimplemented_c),         // 010...11

    RV_XLEN_PREFIX(c_ld),                    // 011...00
    RV_XLEN_PREFIX(c_addi16sp_lui),          // 011...01
    RV_XLEN_PREFIX(c_ldsp),                  // 011...10
    RV_XLEN_PREFIX(unimplemented_c),         // 011...11

    RV_XLEN_PREFIX(unimplemented_c),         // 100...00
    RV_XLEN_PREFIX(c_misc_alu),              // 100...01
    RV_XLEN_PREFIX(c_jr_mv_ebreak_jalr_add), // 100...10
    RV_XLEN_PREFIX(unimplemented_c),         // 100...11

    RV_XLEN_PREFIX(c_fsd),                   // 101...00
    RV_XLEN_PREFIX(c_j),                     // 101...01
    RV_XLEN_PREFIX(c_fsdsp),                 // 101...10
    RV_XLEN_PREFIX(unimplemented_c),         // 101...11

    RV_XLEN_PREFIX(c_sw),                    // 110...00
    RV_XLEN_PREFIX(c_beqz),                  // 110...01
    RV_XLEN_PREFIX(c_swsp),                  // 110...10
    RV_XLEN_PREFIX(unimplemented_c),         // 110...11

    RV_XLEN_PREFIX(c_sd),                    // 111...00
    RV_XLEN_PREFIX(c_bnez),                  // 111...01
    RV_XLEN_PREFIX(c_sdsp),                  // 111...10
    RV_XLEN_PREFIX(unimplemented_c),         // 111...11
};
#endif

#if !TTSIM_RV64_SYSTEM
void RV_XLEN_PREFIX(step)(RiscvHartState *p_hart) {
    uint_xlen_t pc = p_hart->pc;
    uint32_t inst;
#if XLEN == 32
    if (pc >= p_hart->sram_size) [[unlikely]]
#else
    if (pc > TENSIX_SRAM_SIZE-4) [[unlikely]] // XXX as written, cannot execute a compressed instruction in the last 2B
#endif
    {
#if TT_ARCH_VERSION == 0
        if ((pc >= RV32_IRAM_BASE) && (pc < RV32_IRAM_BASE + RV32_IRAM_SIZE)) {
            if (p_hart->riscv_id == RV32_ID_NCRISC) {
                inst = mem_rd<uint32_t>(&g_t_tiles[p_hart->tile_id].ncrisc_iram[pc - RV32_IRAM_BASE]);
            } else if (p_hart->tile_type == 'E') {
                inst = mem_rd<uint32_t>(&g_e_tiles[p_hart->tile_id].erisc_iram[pc - RV32_IRAM_BASE]);
            } else {
                TTSIM_ERROR(UndefinedBehavior, "invalid pc=0x%llx", uint64_t(pc));
            }
        } else {
            TTSIM_ERROR(UndefinedBehavior, "invalid pc=0x%llx", uint64_t(pc));
        }
#else
        TTSIM_ERROR(UndefinedBehavior, "invalid pc=0x%llx", uint64_t(pc));
#endif
    } else {
        inst = mem_rd<uint32_t>(&p_hart->p_sram[pc]);
    }
    p_hart->pc = pc + 4; // faster to do this here; requires some fixes downstream for instructions that reference PC

    if ((inst & 3) == 3) [[likely]] { // RISCV
        uint32_t opcode = ((inst >> 2) & 31) | ((inst & 0x7000) >> 7);
        s_decode_table[opcode](p_hart, inst);
    } else {
#if XLEN == 32
        RV_XLEN_PREFIX(tti)(p_hart, inst);
        // XXX In BH, can dispatch up to 4 TTIs in one cycle in some cases
#else
        p_hart->pc = pc + 2; // again, optimizes for the common case
        uint32_t opcode = (inst & 3) | (bits<15,13>(inst) << 2);
        s_rv64c_decode_table[opcode](p_hart, inst);
#endif
    }
}
#endif
