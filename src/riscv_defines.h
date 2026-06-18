// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// RISC-V CSR numbers, opcode constants, and encoding helpers shared by the RV32/RV64 builds.
#pragma once

#define CSR_FFLAGS        0x001
#define CSR_FRM           0x002
#define CSR_FCSR          0x003
#define CSR_SSTATUS       0x100
#define CSR_SIE           0x104
#define CSR_STVEC         0x105
#define CSR_SCOUNTEREN    0x106
#define CSR_SENVCFG       0x10A
#define CSR_SSCRATCH      0x140
#define CSR_SEPC          0x141
#define CSR_SCAUSE        0x142
#define CSR_STVAL         0x143
#define CSR_SIP           0x144
#define CSR_SATP          0x180
#define CSR_MSTATUS       0x300
#define CSR_MISA          0x301
#define CSR_MEDELEG       0x302
#define CSR_MIDELEG       0x303
#define CSR_MIE           0x304
#define CSR_MTVEC         0x305
#define CSR_MCOUNTEREN    0x306
#define CSR_MENVCFG       0x30A
#define CSR_MCOUNTINHIBIT 0x320
#define CSR_MSCRATCH      0x340
#define CSR_MEPC          0x341
#define CSR_MCAUSE        0x342
#define CSR_MTVAL         0x343
#define CSR_MIP           0x344
#define CSR_PMPCFG0       0x3A0
#define CSR_PMPADDR0      0x3B0
#define CSR_CFG0          0x7C0
#define CSR_CYCLE         0xC00
#define CSR_TIME          0xC01
#define CSR_INSTRET       0xC02
#define CSR_MVENDORID     0xF11
#define CSR_MARCHID       0xF12
#define CSR_MIMPID        0xF13
#define CSR_MHARTID       0xF14
#define CSR_MCONFIGPTR    0xF15

#define PRIV_U 0
#define PRIV_S 1
#define PRIV_M 3

#define MSTATUS_SIE  (1ull << 1)
#define MSTATUS_MIE  (1ull << 3)
#define MSTATUS_SPIE (1ull << 5)
#define MSTATUS_MPIE (1ull << 7)
#define MSTATUS_SPP  (1ull << 8)
#define MSTATUS_MPP_SHIFT 11
#define MSTATUS_MPP  (3ull << MSTATUS_MPP_SHIFT)
#define MSTATUS_FS_SHIFT 13
#define MSTATUS_FS   (3ull << MSTATUS_FS_SHIFT)
#define MSTATUS_XS_SHIFT 15
#define MSTATUS_XS   (3ull << MSTATUS_XS_SHIFT)
#define MSTATUS_MPRV (1ull << 17)
#define MSTATUS_SUM  (1ull << 18)
#define MSTATUS_MXR  (1ull << 19)
#define MSTATUS_TVM  (1ull << 20)
#define MSTATUS_TW   (1ull << 21)
#define MSTATUS_TSR  (1ull << 22)
#define MSTATUS_SD   (1ull << 63) // reads as 1 when FS or XS == Dirty (3); WARL-derived, not stored
#define MSTATUS_UXL  (2ull << 32) // UXL=2 (64-bit)
#define MSTATUS_SXL  (2ull << 34) // SXL=2 (64-bit)
#define SSTATUS_MASK (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | MSTATUS_FS | MSTATUS_XS | MSTATUS_SUM | \
                      MSTATUS_MXR | MSTATUS_UXL | MSTATUS_SD)

#define IRQ_S_SOFT  1
#define IRQ_M_SOFT  3
#define IRQ_S_TIMER 5
#define IRQ_M_TIMER 7
#define IRQ_S_EXT   9
#define IRQ_M_EXT   11
#define MIP_SSIP (1ull << IRQ_S_SOFT)
#define MIP_MSIP (1ull << IRQ_M_SOFT)
#define MIP_STIP (1ull << IRQ_S_TIMER)
#define MIP_MTIP (1ull << IRQ_M_TIMER)
#define MIP_SEIP (1ull << IRQ_S_EXT)
#define MIP_MEIP (1ull << IRQ_M_EXT)

#define EXC_INST_MISALIGNED   0
#define EXC_INST_ACCESS       1
#define EXC_ILLEGAL_INST      2
#define EXC_BREAKPOINT        3
#define EXC_LOAD_MISALIGNED   4
#define EXC_LOAD_ACCESS       5
#define EXC_STORE_MISALIGNED  6
#define EXC_STORE_ACCESS      7
#define EXC_ECALL_U           8
#define EXC_ECALL_S           9
#define EXC_ECALL_M           11
#define EXC_INST_PAGE_FAULT   12
#define EXC_LOAD_PAGE_FAULT   13
#define EXC_STORE_PAGE_FAULT  15
#define CAUSE_INTERRUPT (1ull << 63)

#define SATP_MODE_SHIFT 60
#define SATP_MODE_BARE  0
#define SATP_MODE_SV39  8
#define PTE_V (1u << 0)
#define PTE_R (1u << 1)
#define PTE_W (1u << 2)
#define PTE_X (1u << 3)
#define PTE_U (1u << 4)
#define PTE_G (1u << 5)
#define PTE_A (1u << 6)
#define PTE_D (1u << 7)
