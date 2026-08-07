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

// Call a launched kernel with the erisc kernel ABI in place: a0 = entry, a1 = gp
// (__global_pointer$), a2 = sp (stack top). The base fw runs with its own gp/sp, so save them and
// restore on return. s0/s1 hold the fw's sp/gp across the call -- the kernel preserves callee-saved
// registers per the RISC-V ABI (and yields back through eth_yield_service, which does not depend on gp).
.section .text, "ax"
.global eth_call_kernel
eth_call_kernel:
    addi sp, sp, -16
    sw   ra, 0(sp)
    sw   s0, 4(sp)
    sw   s1, 8(sp)
    mv   s0, sp    // save fw sp
    mv   s1, gp    // save fw gp
    mv   gp, a1    // kernel gp
    mv   sp, a2    // kernel sp
    jalr a0        // run the kernel
    mv   sp, s0    // restore fw sp
    mv   gp, s1    // restore fw gp
    lw   ra, 0(sp)
    lw   s0, 4(sp)
    lw   s1, 8(sp)
    addi sp, sp, 16
    ret

// Yield entry the active-erisc app calls (via the *0x9020 pointer) to let the base fw run its
// housekeeping while the app holds the core. Per the eth ABI (CallingIntoCustomerCode.md) the only
// register this call may perturb is ra, so save/restore every caller-saved temporary/argument
// register around the C service routine (gp/tp are untouched by the C code; s-regs are callee-saved).
.section .text, "ax"
.global eth_yield_entry
eth_yield_entry:
    addi sp, sp, -64
    sw   ra, 0(sp)
    sw   t0, 4(sp)
    sw   t1, 8(sp)
    sw   t2, 12(sp)
    sw   t3, 16(sp)
    sw   t4, 20(sp)
    sw   t5, 24(sp)
    sw   t6, 28(sp)
    sw   a0, 32(sp)
    sw   a1, 36(sp)
    sw   a2, 40(sp)
    sw   a3, 44(sp)
    sw   a4, 48(sp)
    sw   a5, 52(sp)
    sw   a6, 56(sp)
    sw   a7, 60(sp)
    call eth_yield_service
    lw   ra, 0(sp)
    lw   t0, 4(sp)
    lw   t1, 8(sp)
    lw   t2, 12(sp)
    lw   t3, 16(sp)
    lw   t4, 20(sp)
    lw   t5, 24(sp)
    lw   t6, 28(sp)
    lw   a0, 32(sp)
    lw   a1, 36(sp)
    lw   a2, 40(sp)
    lw   a3, 44(sp)
    lw   a4, 48(sp)
    lw   a5, 52(sp)
    lw   a6, 56(sp)
    lw   a7, 60(sp)
    addi sp, sp, 64
    ret
