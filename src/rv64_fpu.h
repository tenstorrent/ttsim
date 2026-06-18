// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Full-system RV64 F/D floating point interfaces/helpers.
#pragma once
#include "riscv_defines.h"
#include "rv64_system.h"

static inline void rv64_fpu_mark_dirty(Rv64SysHartState *h) {
    h->mstatus |= MSTATUS_FS | MSTATUS_SD;
}

void rv64_fpu_load(Rv64SysHartState *h, uint32_t inst, uint32_t size);
void rv64_fpu_store(Rv64SysHartState *h, uint32_t inst, uint32_t size);
void rv64_fpu_op(Rv64SysHartState *h, uint32_t inst);
void rv64_fpu_fma(Rv64SysHartState *h, uint32_t inst, bool neg_product, bool neg_addend);
