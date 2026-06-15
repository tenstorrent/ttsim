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
