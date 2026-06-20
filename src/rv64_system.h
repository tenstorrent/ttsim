// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Definitions and state for the full-system RV64 (TTSIM_RV64_SYSTEM) machine.
#pragma once
#include "common.h"
#include "riscv_defines.h"

enum class MemAccess { Fetch, Load, Store };

struct RvSystem; // the board (DRAM + devices), defined in rv64_system.cpp

#define DTLB_N 16u
#define L2TLB_N 128u
#define TLB_VALID_BIT (1ull << 63)
#define TLB_INVALID_VPN UINT64_MAX

struct TlbEntry {
    uint64_t vpn;
    uint8_t *host_page;
};
static_assert(sizeof(TlbEntry) == 16, "L1 TLB entry must stay 16 bytes");
struct L2TlbEntry {
    uint64_t vpn;
    uint64_t tag;
    uint64_t pa_page;
    uint64_t pte;
};

struct Rv64SysHartState {
    RvSystem *p_sys;
    uint64_t pc;
    uint64_t x_regs[32];
    uint64_t f_regs[32];
    uint32_t priv;
    uint32_t hart_id;

    uint64_t mstatus;
    uint64_t misa;
    uint64_t medeleg;
    uint64_t mideleg;
    uint64_t mie;
    uint64_t mip;
    uint64_t mtvec;
    uint64_t mscratch;
    uint64_t mepc;
    uint64_t mcause;
    uint64_t mtval;
    uint64_t mcounteren;
    uint64_t mcountinhibit;
    uint64_t menvcfg;
    uint64_t sstatus_extra; // unused bits kept in mstatus; sstatus is a view of mstatus
    uint64_t stvec;
    uint64_t sscratch;
    uint64_t sepc;
    uint64_t scause;
    uint64_t stval;
    uint64_t satp;
    uint64_t scounteren;
    uint64_t senvcfg;
    uint64_t sie_unused; // sie/sip are views of mie/mip masked by mideleg
    uint32_t fcsr;
    uint64_t pmpcfg[2];
    uint64_t pmpaddr[16];
    uint64_t mhpmcounter[32]; // plain storage so OpenSBI's accesses do not trap; not a real performance model.
    uint64_t mhpmevent[32];

    bool reservation_valid;
    uint64_t reservation_addr;
    uint32_t reservation_size;
    uint64_t reservation_value;
    uint64_t reservation_paddr;

    bool wfi;
    bool wfi_retired;
    bool wfi_sleeping;
    bool trap_pending;
    uint64_t trap_cause;
    uint64_t trap_tval;
    uint64_t trap_pc;
    uint64_t trap_npc;
    uint64_t pc_adjust;
    uint64_t irq_deliverable;
    uint64_t irq_next_check;

    TlbEntry itlb[1];
    TlbEntry dtlb_load[DTLB_N];
    TlbEntry dtlb_store[DTLB_N];
    L2TlbEntry l2tlb[L2TLB_N];

    uint64_t steps_left;
};

static inline uint8_t *rv64_dtlb_host(Rv64SysHartState *p_hart, uint64_t vaddr, uint32_t size, bool is_store) {
    if ((uint32_t(vaddr) & (size - 1)) != 0) {
        return nullptr; // misaligned
    }
    if (((uint32_t(vaddr) & 0xFFF) + size) > 0x1000) {
        return nullptr; // crosses a page -- XXX alignment check should be sufficient here to avoid?
    }
    uint64_t vpn = vaddr >> 12;
    auto *e = is_store ? &p_hart->dtlb_store[vpn & (DTLB_N-1)] : &p_hart->dtlb_load[vpn & (DTLB_N-1)];
    if (e->vpn == vpn) [[likely]] {
        return e->host_page + (uint32_t(vaddr) & 0xFFF);
    }
    return nullptr;
}

// Translate+access; on fault, set the hart's pending trap and return false.
bool rv64_sys_load(Rv64SysHartState *p_hart, uint64_t vaddr, void *dst, uint32_t size);
bool rv64_sys_store(Rv64SysHartState *p_hart, uint64_t vaddr, const void *src, uint32_t size);

// LR/SC/AMO target check: true only if vaddr translates to DRAM. A device/MMIO target raises an
// access fault (LR -> load-access, else store/AMO-access); a translation fault sets its own trap.
bool rv64_sys_atomic_dram(Rv64SysHartState *p_hart, uint64_t vaddr, uint32_t size, bool is_load);

// Physical access used by loaders, device DMA, and page-table walks (no translation, no trap).
void rv64_phys_read(RvSystem *p_sys, uint64_t paddr, void *dst, uint32_t size);
void rv64_phys_write(RvSystem *p_sys, uint64_t paddr, const void *src, uint32_t size);
bool rv64_sys_in_dram(RvSystem *p_sys, uint64_t paddr, uint64_t size);

uint64_t rv64_sys_read_csr(Rv64SysHartState *p_hart, uint32_t csr, bool *ok);
void rv64_sys_write_csr(Rv64SysHartState *p_hart, uint32_t csr, uint64_t val, bool *ok);
void rv64_sys_raise(Rv64SysHartState *p_hart, uint64_t cause, uint64_t tval);
void rv64_sys_take_trap(Rv64SysHartState *p_hart, uint64_t cause, uint64_t tval, uint64_t epc);
void rv64_sys_xret(Rv64SysHartState *p_hart, bool from_machine);
void rv64_sys_sfence(Rv64SysHartState *p_hart, uint64_t vaddr, uint64_t asid);

RvSystem *rv64_sys_create(uint64_t dram_base, uint64_t dram_size, uint32_t num_harts,
                          uint32_t timer_insns_per_tick);
Rv64SysHartState *rv64_sys_hart(RvSystem *p_sys, uint32_t hart_id);
void rv64_sys_init_reset_rom(RvSystem *p_sys, uint64_t entry, uint64_t fdt_addr);
uint64_t rv64_sys_reset_rom_base();
void rv64_sys_pause_yield(Rv64SysHartState *p_hart);
void rv64_sys_lr_reserve(Rv64SysHartState *p_hart, uint64_t vaddr, uint32_t size);
void rv64_sys_run(RvSystem *p_sys, uint64_t max_insns);
void rv64_sys_set_disk(RvSystem *p_sys, const char *path);
void rv64_sys_set_irq(RvSystem *p_sys, uint32_t source, bool level);
void rv64_sys_set_interactive(RvSystem *p_sys);
void rv64_sys_set_hart_quantum(RvSystem *p_sys, uint32_t q);
void rv64_sys_uart_inject(RvSystem *p_sys, uint64_t at, const uint8_t *data, size_t len);
void rv64_sys_tt_attach(RvSystem *p_sys, const char *path);
