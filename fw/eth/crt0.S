// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Entry stub for mock Wormhole eth base firmware.
.section .start, "ax"
.global _start

_start:
    la sp, ETH_FW_STACK_TOP // small stack at the top of the fw's reserved L1 region; supplied at link time (--defsym)
    call eth_fw_main
loop:
    j loop
