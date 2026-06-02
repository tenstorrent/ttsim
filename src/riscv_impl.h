// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// RISC-V instruction implementations, parametric on XLEN (instantiated by rv32.cpp and rv64.cpp).
// Quick references:
// https://csg.csail.mit.edu/6.375/6_375_2019_www/resources/riscv-spec.pdf
// https://five-embeddev.com/quickref/isa_ext.html

#include "sim.h"
#include "riscv_defines.h"
#include <algorithm>
#include <type_traits>

#define RV_XLEN_PREFIX(name) RV_XLEN_PREFIX_(XLEN, name)
#define RV_XLEN_PREFIX_(xlen, name) RV_XLEN_PREFIX__(xlen, name)
#define RV_XLEN_PREFIX__(xlen, name) rv##xlen##_##name

#define RISCV_I_IMM(inst) (int32_t(inst) >> 20)
#define RISCV_U_IMM(inst) (int32_t((inst) & 0xFFFFF000))
#define RISCV_S_IMM(inst) (int32_t(((inst) & 0xFE000000) | (((inst) & 0xF80) << 13)) >> 20)

#define X_SP 2

#define HAS_ZBA_ZBB ((TT_ARCH_VERSION >= 1) || (XLEN == 64))
#define HAS_ZBC (XLEN == 64)
#define HAS_ZBS (XLEN == 64)
#define HAS_PACK_BREV8 ((TT_ARCH_VERSION >= 1) && (XLEN == 32)) // small subset of Zbkb that is in BH babyrisc

using int_xlen_t = int_types<XLEN>::int_t;
using uint_xlen_t = int_types<XLEN>::uint_t;
using int_2_times_xlen_t = int_types<2*XLEN>::int_t;
using uint_2_times_xlen_t = int_types<2*XLEN>::uint_t;

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
    p_hart->tile_type = tile_type;
    p_hart->tile_id = tile_id;
    p_hart->riscv_id = riscv_id;

    if (tile_type == 'T') {
        p_hart->p_sram = g_t_tiles[tile_id].sram;
        p_hart->sram_size = sizeof(g_t_tiles[tile_id].sram);
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
    } else {
        TTSIM_VERIFY(tile_type == 'E', AssertionFailure, "tile_type=%c", tile_type);
        p_hart->p_sram = g_e_tiles[tile_id].sram;
        p_hart->sram_size = sizeof(g_e_tiles[tile_id].sram);
        p_hart->p_local_mem = g_e_tiles[tile_id].rv32_local_ram[riscv_id];
        p_hart->local_mem_base = RISCV_LOCAL_MEM_BASE;
        p_hart->local_mem_size = sizeof(g_e_tiles[tile_id].rv32_local_ram[riscv_id]);
    }

    RV_XLEN_PREFIX(icache_invalidate)(p_hart);
#if TT_ARCH_VERSION >= 1
    dcache_invalidate(p_hart);
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
        default: TTSIM_ERROR(UndefinedBehavior, "funct3=%d funct7=%d", funct3, funct7);
    }
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = value;
    }
}

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
                    value = src ? __builtin_clz(src) : 32;
                    break;
                case 0x601: // CTZ
                    value = src ? __builtin_ctz(src) : 32;
                    break;
                case 0x602: // CPOP
                    value = __builtin_popcount(src);
                    break;
                case 0x604: value = int_xlen_t(int8_t(uint8_t(src))); break; // SEXT.B
                case 0x605: value = int_xlen_t(int16_t(uint16_t(src))); break; // SEXT.H
#endif
#if HAS_ZBS
                case 0x280 ... 0x280 + XLEN-1: value = src | (uint_xlen_t(1) << (imm & (XLEN - 1))); break; // BSETI
                case 0x480 ... 0x480 + XLEN-1: value = src & ~(uint_xlen_t(1) << (imm & (XLEN - 1))); break; // BCLRI
                case 0x680 ... 0x680 + XLEN-1: value = src ^ (uint_xlen_t(1) << (imm & (XLEN - 1))); break; // BINVI
#endif
                default: TTSIM_ERROR(UndefinedBehavior, "funct3=%d imm=0x%x", funct3, imm);
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
                case 0x698: value = __builtin_bswap32(src); break; // REV8
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
                default: TTSIM_ERROR(UndefinedBehavior, "funct3=%d imm=0x%x", funct3, imm);
            }
            break;
        case 6: value = src | imm; break; // ORI
        case 7: value = src & imm; break; // ANDI
        default: TTSIM_ERROR(UndefinedBehavior, "funct3=%d", funct3);
    }
    if (r_dst) [[likely]] {
        p_hart->x_regs[r_dst] = value;
    }
}

template<bool neg_product, bool neg_addend> static void RV_XLEN_PREFIX(f_fma)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zfh/F/D/Q");
#else
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#endif
}

static void RV_XLEN_PREFIX(f_alu)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zfh/F/D/Q");
#else
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#endif
}

static void RV_XLEN_PREFIX(v_alu)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support V");
#else
    TTSIM_ERROR(UnsupportedFunctionality, "babyrisc non-compliant V extension is explicitly out of scope");
#endif
}

static void dcache_access(RiscvHartState *p_hart, uint_xlen_t addr, bool store) {
}

static uint8_t *get_local_mem_ptr(RiscvHartState *p_hart, uint_xlen_t addr) {
    uint32_t local_mem_base = p_hart->local_mem_base;
    if ((addr >= local_mem_base) && (addr < local_mem_base + p_hart->local_mem_size)) {
        return &p_hart->p_local_mem[addr - local_mem_base];
    }
    return nullptr;
}

template<class T> static bool RV_XLEN_PREFIX(mem_rd)(RiscvHartState *p_hart, uint_xlen_t addr, T *p_data) {
    const uint32_t size = sizeof(T);
    static_assert((size == 1) || (size == 2) || (size == 4) || (size == 8), "unsupported object size");
    TTSIM_VERIFY(!(addr & (size - 1)), NonContractualBehavior, "unaligned addr=0x%llx size=%d", uint64_t(addr), size);
    if (addr < p_hart->sram_size) {
        dcache_access(p_hart, addr, false);
        *p_data = mem_rd<T>(&p_hart->p_sram[addr]);
        return true;
    }
    if (uint8_t *p_mem = get_local_mem_ptr(p_hart, addr)) {
        *p_data = mem_rd<T>(p_mem);
        return true;
    }
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
}

template<class T> static bool RV_XLEN_PREFIX(mem_wr)(RiscvHartState *p_hart, uint_xlen_t addr, T data) {
    const uint32_t size = sizeof(T);
    static_assert((size == 1) || (size == 2) || (size == 4) || (size == 8), "unsupported object size");
    TTSIM_VERIFY(!(addr & (size - 1)), NonContractualBehavior, "unaligned addr=0x%llx size=%d", uint64_t(addr), size);
    if (addr < p_hart->sram_size) {
        mem_wr<T>(&p_hart->p_sram[addr], data);
        dcache_access(p_hart, addr, true);
        return true;
    }
    if (uint8_t *p_mem = get_local_mem_ptr(p_hart, addr)) {
        mem_wr<T>(p_mem, data);
        return true;
    }
    if constexpr (size == 4) {
        return tile_mmio_wr32(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id, addr, data);
    }
    if constexpr (size == 8) {
        return tile_mmio_wr64(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id, addr, data);
    }
    TTSIM_ERROR(UnsupportedFunctionality, "addr=0x%llx size=%d", uint64_t(addr), size);
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
    if constexpr (sizeof(T) == 8) {
        TTSIM_ERROR(UntestedFunctionality, "64-bit atomic");
    }
    uint32_t funct5 = bits<31,27>(inst); // aq/rl flags are ignored
    uint32_t r_addr = bits<19,15>(inst);
    uint32_t r_src = bits<24,20>(inst);
    uint32_t r_dst = bits<11,7>(inst);

    uint_xlen_t addr = p_hart->x_regs[r_addr];
    TTSIM_VERIFY(!(addr & (sizeof(T) - 1)), UndefinedBehavior, "unaligned addr=0x%llx", uint64_t(addr));
    TTSIM_VERIFY(addr < p_hart->sram_size, UndefinedBehavior, "addr=0x%llx not in sram", uint64_t(addr));
    auto *p_sram = &p_hart->p_sram[addr];
    T old_val = mem_rd<T>(p_sram);
    T src_val = T(p_hart->x_regs[r_src]);
    using S = std::make_signed_t<T>;
    switch (funct5) {
        case 0x00: mem_wr<T>(p_sram, old_val + src_val); break; // AMOADD
        case 0x01: mem_wr<T>(p_sram, src_val); break; // AMOSWAP
        case 0x02: // LR
        case 0x03: // SC
            TTSIM_ERROR(UndefinedBehavior, "babyrisc supports Zaamo but not LR/SC");
        case 0x04: mem_wr<T>(p_sram, old_val ^ src_val); break; // AMOXOR
        case 0x08: mem_wr<T>(p_sram, old_val | src_val); break; // AMOOR
        case 0x0C: mem_wr<T>(p_sram, old_val & src_val); break; // AMOAND
        case 0x10: mem_wr<T>(p_sram, T(std::min<S>(old_val, src_val))); break; // AMOMIN
        case 0x14: mem_wr<T>(p_sram, T(std::max<S>(old_val, src_val))); break; // AMOMAX
        case 0x18: mem_wr<T>(p_sram, std::min(old_val, src_val)); break; // AMOMINU
        case 0x1C: mem_wr<T>(p_sram, std::max(old_val, src_val)); break; // AMOMAXU
        default: TTSIM_ERROR(UndefinedBehavior, "funct5=%d", funct5);
    }

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
#else
    if constexpr (sizeof(T) == 8) {
        TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support D");
    } else if constexpr (sizeof(T) == 16) {
        TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support Q");
    }
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#endif
}

template<class T> static void RV_XLEN_PREFIX(f_store)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zfh/F/D/Q");
#else
    if constexpr (sizeof(T) == 8) {
        TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support D");
    } else if constexpr (sizeof(T) == 16) {
        TTSIM_ERROR(UndefinedBehavior, "babyrisc does not support Q");
    }
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
#endif
}

template<class T> static void RV_XLEN_PREFIX(v_load)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support V");
#else
    TTSIM_ERROR(UnsupportedFunctionality, "babyrisc non-compliant V extension is explicitly out of scope");
#endif
}

template<class T> static void RV_XLEN_PREFIX(v_store)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support V");
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
    TTSIM_VERIFY(!(target_pc & 3), UndefinedBehavior, "unaligned target_pc=0x%llx", uint64_t(target_pc));
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
    TTSIM_VERIFY(!(target_pc & 3), UndefinedBehavior, "unaligned target_pc=0x%llx", uint64_t(target_pc));
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
        TTSIM_VERIFY(!(target_pc & 3), UndefinedBehavior, "unaligned target_pc=0x%llx", uint64_t(target_pc));
        branch_taken(p_hart, pc, target_pc);
    } else {
        branch_not_taken(p_hart, pc);
    }
}

static void RV_XLEN_PREFIX(fence)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UnsupportedFunctionality, "fence instructions do not enforce memory ordering on Wormhole and should not be used");
#else
    uint32_t r_dst = bits<11,7>(inst);
    TTSIM_VERIFY(!r_dst, UnsupportedFunctionality, "r_dst=%d", r_dst);
    uint32_t r_src = bits<19,15>(inst);
    TTSIM_VERIFY(!r_src, UnsupportedFunctionality, "r_src=%d", r_src);
    uint32_t fence_mode = bits<31,20>(inst); // concatenation of many different fence bits

    // For future reference: PAUSE instruction appears to be 0x0100000F, i.e., fence_mode == 0x010
    // This is probably a safe NOP on silicon, but don't know for sure and don't know its performance
    // Can generate it via __builtin_riscv_pause()
    // 0xFF is invalidate_l1_cache() on BH; others are from std::atomic
    if ((fence_mode == 0x023) || (fence_mode == 0x031) || (fence_mode == 0x033) || (fence_mode == 0x0FF)) {
        return dcache_invalidate(p_hart); // all fences flush the D$
    }
    TTSIM_ERROR(UnsupportedFunctionality, "fence_mode=0x%x", fence_mode);
#endif
}

static void RV_XLEN_PREFIX(fence_i)(RiscvHartState *p_hart, uint32_t inst) {
    TTSIM_ERROR(UnsupportedFunctionality, "fence.i does not flush the instruction cache on babyrisc and should not be used");
}

static void RV_XLEN_PREFIX(ecall_ebreak)(RiscvHartState *p_hart, uint32_t inst) {
    if (inst == 0x73) {
        p_hart->x_regs[10] = libttsim_syscall(p_hart->tile_type, p_hart->tile_id, p_hart->riscv_id,
            p_hart->x_regs[17], p_hart->x_regs[10], p_hart->x_regs[11], p_hart->x_regs[12]);
    } else {
        TTSIM_ERROR(UnsupportedFunctionality, "inst=0x%x", inst);
    }
}

#if TT_ARCH_VERSION >= 1
static uint_xlen_t read_csr(RiscvHartState *p_hart, uint32_t csr) {
    switch (csr) {
        case CSR_CFG0: return p_hart->chicken_bits;
#if TT_ARCH_VERSION == 1
        default: TTSIM_ERROR(UnsupportedFunctionality, "csr=0x%x", csr);
#else
        default: TTSIM_ERROR(UnimplementedFunctionality, "csr=0x%x", csr);
#endif
    }
}

static void write_csr(RiscvHartState *p_hart, uint32_t csr, uint_xlen_t data) {
    switch (csr) {
        case CSR_CFG0:
#if TT_ARCH_VERSION == 1
            TTSIM_VERIFY(!(data & ~0x104000A), UnsupportedFunctionality, "chicken_bits: data=0x%x", data);
#else
            TTSIM_VERIFY(!(data & ~0x40001), UnimplementedFunctionality, "chicken_bits: data=0x%x", data);
#endif
            p_hart->chicken_bits = data;
            break;
#if TT_ARCH_VERSION == 1
        default: TTSIM_ERROR(UnsupportedFunctionality, "csr=0x%x", csr);
#else
        default: TTSIM_ERROR(UnimplementedFunctionality, "csr=0x%x", csr);
#endif
    }
}
#endif

static void RV_XLEN_PREFIX(csrrw)(RiscvHartState *p_hart, uint32_t inst) {
#if TT_ARCH_VERSION == 0
    TTSIM_ERROR(UndefinedBehavior, "Wormhole does not support Zicsr");
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

static void RV_XLEN_PREFIX(unimplemented)(RiscvHartState *p_hart, uint32_t inst) {
    TTSIM_ERROR(UndefinedBehavior, "could not decode instruction inst=0x%x at pc=0x%llx", inst, uint64_t(p_hart->pc - 4));
}

using DecodeAndExecuteFunc = void (*)(RiscvHartState *p_hart, uint32_t inst);

static DecodeAndExecuteFunc s_decode_table[256] = {
#include "_out/riscv_decode.h"
};

void RV_XLEN_PREFIX(step)(RiscvHartState *p_hart) {
    uint_xlen_t pc = p_hart->pc;
    uint32_t inst;
    if (pc >= p_hart->sram_size) [[unlikely]]
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
        RV_XLEN_PREFIX(tti)(p_hart, inst);
        // XXX In BH, can dispatch up to 4 TTIs in one cycle in some cases
    }
}
