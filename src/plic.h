// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// A RISC-V platform-level interrupt controller: a priority, a pending bit and a gateway per source, and an enable mask,
// a threshold and a claim/complete register per context. The system around it decides how many sources and contexts
// there are, drives each source's level with plic_set_level, and after each access turns plic_best into its harts'
// external interrupt lines.
#pragma once
#include "common.h"

#define PLIC_MAX_WORDS 3 // source ids 1..95
#define PLIC_MAX_CONTEXTS 32

struct Plic {
    uint32_t num_sources; // ids 1..num_sources; 0 means none
    uint32_t num_contexts;
    uint32_t priority[PLIC_MAX_WORDS * 32];
    uint32_t pending[PLIC_MAX_WORDS];
    uint32_t level[PLIC_MAX_WORDS];
    uint32_t claimed[PLIC_MAX_WORDS];
    uint32_t enable[PLIC_MAX_CONTEXTS][PLIC_MAX_WORDS];
    uint32_t threshold[PLIC_MAX_CONTEXTS];
};

static inline uint32_t plic_words(const Plic *p_plic) {
    return (p_plic->num_sources + 32) / 32;
}

// Highest-priority enabled+pending source above this context's threshold (ties: lowest id),
// skipping sources already claimed (in service). 0 = none.
static inline uint32_t plic_best(const Plic *p_plic, uint32_t ctx) {
    uint32_t best = 0;
    uint32_t best_pri = 0;
    for (uint32_t s = 1; s <= p_plic->num_sources; s++) {
        uint32_t w = s >> 5;
        uint32_t bit = 1u << (s & 31);
        if (!(p_plic->pending[w] & bit)) {
            continue;
        }
        if (!(p_plic->enable[ctx][w] & bit)) {
            continue;
        }
        if (p_plic->claimed[w] & bit) {
            continue;
        }
        uint32_t pri = p_plic->priority[s];
        if ((pri > p_plic->threshold[ctx]) && (pri > best_pri)) {
            best_pri = pri;
            best = s;
        }
    }
    return best;
}

static inline uint32_t plic_read(Plic *p_plic, uint32_t offset) {
    TTSIM_VERIFY(!(offset & 3), UnimplementedFunctionality, "misaligned offset=0x%x", offset);
    if (offset < 0x1000) {
        TTSIM_VERIFY((offset >= 4) && (offset / 4 <= p_plic->num_sources), UnimplementedFunctionality, "offset=0x%x", offset);
        return p_plic->priority[offset / 4];
    }
    if ((offset >= 0x1000) && (offset < 0x1080)) {
        uint32_t w = (offset - 0x1000) / 4;
        TTSIM_VERIFY(w < plic_words(p_plic), UnimplementedFunctionality, "offset=0x%x", offset);
        return p_plic->pending[w];
    }
    if ((offset >= 0x2000) && (offset < 0x2000 + 0x80 * PLIC_MAX_CONTEXTS)) {
        uint32_t ctx = (offset - 0x2000) / 0x80;
        uint32_t w = ((offset - 0x2000) % 0x80) / 4;
        TTSIM_VERIFY((ctx < p_plic->num_contexts) && (w < plic_words(p_plic)), UnimplementedFunctionality, "offset=0x%x", offset);
        return p_plic->enable[ctx][w];
    }
    if (offset >= 0x200000) {
        uint32_t ctx = (offset - 0x200000) / 0x1000;
        uint32_t reg = (offset - 0x200000) % 0x1000;
        TTSIM_VERIFY(ctx < p_plic->num_contexts, UnimplementedFunctionality, "offset=0x%x", offset);
        if (reg == 0) {
            return p_plic->threshold[ctx];
        }
        if (reg == 4) { // CLAIM
            uint32_t s = plic_best(p_plic, ctx);
            if (s) {
                uint32_t w = s >> 5;
                uint32_t b = 1u << (s & 31);
                p_plic->pending[w] &= ~b;
                p_plic->claimed[w] |= b;
            }
            return s;
        }
    }
    TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
}

static inline void plic_write(Plic *p_plic, uint32_t offset, uint32_t val) {
    TTSIM_VERIFY(!(offset & 3), UnimplementedFunctionality, "misaligned offset=0x%x", offset);
    if (offset < 0x1000) {
        TTSIM_VERIFY((offset >= 4) && (offset / 4 <= p_plic->num_sources), UnimplementedFunctionality, "offset=0x%x", offset);
        p_plic->priority[offset / 4] = val;
    } else if ((offset >= 0x2000) && (offset < 0x2000 + 0x80 * PLIC_MAX_CONTEXTS)) {
        uint32_t ctx = (offset - 0x2000) / 0x80;
        uint32_t w = ((offset - 0x2000) % 0x80) / 4;
        TTSIM_VERIFY((ctx < p_plic->num_contexts) && (w < plic_words(p_plic)), UnimplementedFunctionality, "offset=0x%x", offset);
        p_plic->enable[ctx][w] = val;
    } else if (offset >= 0x200000) {
        uint32_t ctx = (offset - 0x200000) / 0x1000;
        uint32_t reg = (offset - 0x200000) % 0x1000;
        TTSIM_VERIFY((ctx < p_plic->num_contexts) && ((reg == 0) || (reg == 4)), UnimplementedFunctionality, "offset=0x%x", offset);
        if (reg == 0) {
            p_plic->threshold[ctx] = val;
        } else { // COMPLETE
            uint32_t w = val >> 5;
            uint32_t b = 1u << (val & 31);
            TTSIM_VERIFY((val >= 1) && (val <= p_plic->num_sources) && (p_plic->enable[ctx][w] & b) && (p_plic->claimed[w] & b), UnimplementedFunctionality,
                "complete id=%u context=%u", val, ctx);
            p_plic->claimed[w] &= ~b;
            // Gateway re-pends if the source line is still asserted at completion (level source).
            if (p_plic->level[w] & b) {
                p_plic->pending[w] |= b;
            }
        }
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static inline void plic_set_level(Plic *p_plic, uint32_t source, bool level) {
    uint32_t w = source >> 5;
    uint32_t b = 1u << (source & 31);
    if (level) {
        p_plic->level[w] |= b;
        // Pending held until the interrupt is claimed, even if deasserted; ignored while in service, where the
        // completion pends it if the level is still up
        if (!(p_plic->claimed[w] & b)) {
            p_plic->pending[w] |= b;
        }
    } else {
        p_plic->level[w] &= ~b;
    }
}
