// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// XLEN=32 instantiation of riscv_impl.h (compiled for all chips).
#define XLEN 32
#define RiscvHartState Rv32HartState

#include "riscv_impl.h"
