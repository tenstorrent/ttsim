// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Full-system RV64 board: guest DRAM + the Sv39 MMU, the M/S CSR file, etc. RV64GC instruction
// semantics are reused from riscv_impl.h.

#define XLEN 64
#define RiscvHartState Rv64SysHartState
#include "riscv_impl.h" // pulls in rv64_system.h

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/mman.h>

#define MAX_HARTS 16

// Interrupt bits that wake a hart from WFI
#define MIP_PLATFORM_MASK (MIP_MSIP | MIP_MTIP | MIP_SEIP | MIP_MEIP)

#define CLINT_BASE   0x02000000ull
#define CLINT_SIZE   0x00010000ull
#define PLIC_BASE    0x0c000000ull
#define PLIC_SIZE    0x00600000ull
#define UART_BASE    0x10000000ull
#define UART_SIZE    0x00000100ull
#define UART_IRQ     10
#define RTC_BASE     0x00101000ull
#define RTC_SIZE     0x00001000ull
#define SYSCON_BASE  0x00100000ull
#define SYSCON_SIZE  0x00001000ull
#define VIRTIO_BASE  0x10001000ull
#define VIRTIO_SIZE  0x00008000ull
#define RESET_ROM_BASE 0x00001000ull
#define RESET_ROM_SIZE 0x00001000ull

#define PLIC_NDEV 53

#define UART_RX_FIFO_SIZE 4096
#define INTERACTIVE_SERIAL_POLL_INSNS 65536

#define RTC_BASE_NS 946684800000000000ull // 2000-01-01

#define VIO_MAGIC 0x74726976u
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_T_GET_ID 8
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2
#define VRING_DESC_F_INDIRECT 4

// One modern (v2) virtio-blk transport in slot 0 (0x10001000, IRQ 1), advertising this sim's own
// vendor id ("TTSM").
#define VIO_BLK_SLOT 0
#define VIRTIO_BLK_IRQ 1
#define VIO_VERSION 2
#define VIO_VENDOR 0x4d535454u // "TTSM"

struct Uart16550 {
    uint8_t dll, dlm, ier, fcr, lcr, mcr, scr;
    bool thr_ipending;
    uint8_t rx_fifo[UART_RX_FIFO_SIZE];
    uint32_t rx_head, rx_tail, rx_count;

    // Deterministic input injection (--uart-inject-at N + --uart-inject-text/-file): push the bytes
    // into the RX FIFO once the guest has retired N instructions. Useful for scripted boots.
    bool inject_enabled;
    uint64_t inject_at;
    uint8_t *inject_data;
    size_t inject_len, inject_pos;

    // Interactive console (-i): raw, non-blocking stdin polled into the RX FIFO on a coarse cadence.
    bool interactive;
    uint64_t interactive_next_poll;
    int stdin_flags; // saved stdin O_* flags for restore (-1 if not saved)
    struct termios stdin_termios;
    bool stdin_termios_valid;
};

struct RvSystem {
    uint8_t *dram;
    uint64_t dram_base;
    uint64_t dram_size;
    uint8_t reset_rom[RESET_ROM_SIZE];
    uint32_t num_harts;
    uint32_t hart_quantum;
    uint32_t timer_insns_per_tick; // mtime advances once per this many retired instructions
    uint64_t insn_count;
    uint64_t visible_icount;
    uint64_t cause12_count;

    Rv64SysHartState harts[MAX_HARTS];

    uint64_t mtimecmp[MAX_HARTS];
    uint32_t msip[MAX_HARTS];
    uint64_t mtime_base;
    Rv64SysHartState *active_hart;
    uint32_t yield_hart;

    Uart16550 uart;

    FILE *disk;
    uint8_t *disk_map;
    uint64_t disk_bytes;
    uint64_t disk_sectors;
    uint32_t vio_status;
    uint32_t vio_dev_feat_sel;
    uint32_t vio_drv_feat_sel;
    uint32_t vio_queue_num;
    uint32_t vio_queue_ready;
    uint64_t vio_desc;
    uint64_t vio_avail;
    uint64_t vio_used;
    uint16_t vio_last_avail;
    uint16_t vio_used_idx;
    uint32_t vio_int_status;

    uint32_t rtc_latched_high;

    uint32_t plic_priority[64];
    uint32_t plic_pending[2];
    uint32_t plic_level[2];
    uint32_t plic_enable[2 * MAX_HARTS][2];
    uint32_t plic_threshold[2 * MAX_HARTS];
    uint32_t plic_claimed[2];
};

RvSystem g_sys;
static Uart16550 *s_interactive_uart = nullptr;

uint64_t ttsim_get_clock() {
    return g_sys.insn_count; // use retired instruction count for prints/errors
}

static void clint_refresh_irq(Rv64SysHartState *p_hart);
static void irq_reschedule(Rv64SysHartState *p_hart);
static void irq_recompute_deliverable(Rv64SysHartState *p_hart);
static void hart_write_mip(Rv64SysHartState *p_hart, uint64_t v);
static void fault(Rv64SysHartState *p_hart, uint64_t cause, uint64_t tval);
static void take_trap(Rv64SysHartState *p_hart, uint64_t *npc, uint64_t cause, uint64_t tval, bool is_interrupt);
static void check_interrupts(Rv64SysHartState *p_hart);
static inline uint64_t current_insn_pc(const Rv64SysHartState *p_hart);

static inline void tlb_flush_l1(Rv64SysHartState *p_hart) {
    p_hart->itlb[0].vpn = ~uint64_t(0);
    for (uint32_t e = 0; e < DTLB_N; e++) {
        p_hart->dtlb_load[e].vpn = ~uint64_t(0);
        p_hart->dtlb_store[e].vpn = ~uint64_t(0);
    }
}

static inline void tlb_flush(Rv64SysHartState *p_hart) {
    tlb_flush_l1(p_hart);
    memset(p_hart->l2tlb, 0, sizeof(p_hart->l2tlb));
}

RvSystem *rv64_sys_create(uint64_t dram_base, uint64_t dram_size, uint32_t num_harts,
                          uint32_t timer_insns_per_tick) {
    TTSIM_VERIFY((num_harts >= 1) && (num_harts <= MAX_HARTS), ConfigurationError, "num_harts=%u", num_harts);
    memset(&g_sys, 0, sizeof(g_sys));
    g_sys.dram_base = dram_base;
    g_sys.dram_size = dram_size;
    g_sys.num_harts = num_harts;
    g_sys.hart_quantum = 100;
    g_sys.timer_insns_per_tick = timer_insns_per_tick;
    g_sys.dram = (uint8_t *)mmap(nullptr, dram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TTSIM_VERIFY(g_sys.dram != MAP_FAILED, SystemError, "failed to allocate %llu bytes of guest DRAM", dram_size);
    for (uint32_t i = 0; i < MAX_HARTS; i++) {
        g_sys.mtimecmp[i] = ~0ull; // CLINT mtimecmp resets to all-ones (no timer alarm pending)
    }
    for (uint32_t i = 0; i < num_harts; i++) {
        Rv64SysHartState *p_hart = &g_sys.harts[i];
        p_hart->p_sys = &g_sys;
        p_hart->hart_id = i;
        p_hart->priv = PRIV_M;
        tlb_flush_l1(p_hart);
        // misa: RV64 (MXL=2) with extensions IMAFDC
        const uint64_t ext = (1u << ('I' - 'A')) | (1u << ('M' - 'A')) | (1u << ('A' - 'A')) |
                             (1u << ('F' - 'A')) | (1u << ('D' - 'A')) | (1u << ('C' - 'A')) |
                             (1u << ('S' - 'A')) | (1u << ('U' - 'A'));
        p_hart->misa = (2ull << 62) | ext;
        p_hart->mstatus = MSTATUS_UXL | MSTATUS_SXL; // 64-bit S/U
    }
    return &g_sys;
}

Rv64SysHartState *rv64_sys_hart(RvSystem *p_sys, uint32_t hart_id) {
    TTSIM_VERIFY(hart_id < p_sys->num_harts, AssertionFailure, "hart_id=%u", hart_id);
    return &p_sys->harts[hart_id];
}

void rv64_sys_init_reset_rom(RvSystem *p_sys, uint64_t entry, uint64_t fdt_addr) {
    static const uint32_t rom[] = {
        0x00000297, // auipc t0,0      -> t0 = RESET_ROM_BASE
        0x02828613, // addi  a2,t0,40
        0xF1402573, // csrr  a0,mhartid
        0x0202B583, // ld    a1,32(t0) -> a1 = fdt_addr
        0x0182B283, // ld    t0,24(t0) -> t0 = entry
        0x00028067, // jr    t0        -> jump to entry
    };
    memset(p_sys->reset_rom, 0, sizeof(p_sys->reset_rom));
    memcpy(p_sys->reset_rom, rom, sizeof(rom));
    mem_wr<uint64_t>(p_sys->reset_rom + 0x18, entry);    // read by `ld t0,24(t0)`
    mem_wr<uint64_t>(p_sys->reset_rom + 0x20, fdt_addr); // read by `ld a1,32(t0)`
}

uint64_t rv64_sys_reset_rom_base() {
    return RESET_ROM_BASE;
}

void rv64_sys_pause_yield(Rv64SysHartState *p_hart) {
    RvSystem *p_sys = p_hart->p_sys;
    if (p_sys->num_harts > 1) {
        p_sys->yield_hart = (p_hart->hart_id + 1) % p_sys->num_harts;
        p_hart->steps_left = 0;
    }
}

// Recompute the (derived) SD bit from FS/XS, keeping mstatus self-consistent after any change.
static inline void rv64_normalize_mstatus(uint64_t *mstatus) {
    bool dirty = ((*mstatus & MSTATUS_FS) == MSTATUS_FS) || ((*mstatus & MSTATUS_XS) == MSTATUS_XS);
    if (dirty) {
        *mstatus |= MSTATUS_SD;
    } else {
        *mstatus &= ~MSTATUS_SD;
    }
}

[[maybe_unused]] static inline uint64_t cyc_mtime_now(const RvSystem *p_sys) {
    return (p_sys->visible_icount - p_sys->cause12_count) / p_sys->timer_insns_per_tick;
}

static inline uint64_t mtime_now(const RvSystem *p_sys) {
    return p_sys->visible_icount / p_sys->timer_insns_per_tick;
}

static uint64_t clint_read(RvSystem *sys, uint32_t offset, uint32_t size) {
    TTSIM_VERIFY((size == 4) || (size == 8), UnimplementedFunctionality, "size=%d", size);
    TTSIM_VERIFY(!(offset & (size - 1)), UnimplementedFunctionality, "misaligned offset=0x%x", offset);
    if ((offset >= 0x4000) && (offset < 0x4000 + 8 * MAX_HARTS)) {
        uint64_t v = sys->mtimecmp[(offset - 0x4000) / 8];
        return (size == 4 && (offset & 4)) ? (v >> 32) : v;
    }
    if ((offset >= 0xBFF8) && (offset < 0xC000)) {
        uint64_t mt = mtime_now(sys);
        return (size == 4 && (offset & 4)) ? (mt >> 32) : mt;
    }
    if (offset < 4 * MAX_HARTS) {
        return sys->msip[offset / 4];
    }
    TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
}

static void clint_write(RvSystem *sys, uint32_t offset, uint32_t size, uint64_t val) {
    TTSIM_VERIFY((size == 4) || (size == 8), UnimplementedFunctionality, "size=%d", size);
    TTSIM_VERIFY(!(offset & (size - 1)), UnimplementedFunctionality, "misaligned offset=0x%x", offset);
    if ((offset >= 0x4000) && (offset < 0x4000 + 8 * MAX_HARTS)) {
        uint64_t *p = &sys->mtimecmp[(offset - 0x4000) / 8];
        if (size == 4) {
            *p = (offset & 4) ? ((*p & 0xFFFFFFFFull) | (val << 32)) : ((*p & ~0xFFFFFFFFull) | uint32_t(val));
        } else {
            *p = val;
        }
        clint_refresh_irq(&sys->harts[(offset - 0x4000) / 8]);
        return;
    }
    if ((offset >= 0xBFF8) && (offset < 0xC000)) {
        return;
    }
    if (offset < 4 * MAX_HARTS) {
        uint32_t hart_id = uint32_t(offset / 4);
        sys->msip[hart_id] = val & 1;
        if ((hart_id < sys->num_harts) && (val & 1) && sys->active_hart && (sys->active_hart->hart_id != hart_id)) {
            sys->yield_hart = hart_id;
            sys->active_hart->steps_left = 0;
        }
        clint_refresh_irq(&sys->harts[hart_id]); // MSIP changed -> re-latch + reschedule
        return;
    }
    TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
}

static void uart_update_irq(RvSystem *sys) {
    Uart16550 *u = &sys->uart;
    // RX-data-available (IER bit 0) outranks THR-empty (IER bit 1)
    bool pending = ((u->ier & 0x01) && u->rx_count) || ((u->ier & 0x02) && u->thr_ipending);
    rv64_sys_set_irq(sys, UART_IRQ, pending);
}

[[maybe_unused]] static bool uart_rx_push(RvSystem *sys, uint8_t v) {
    Uart16550 *u = &sys->uart;
    if (u->rx_count == UART_RX_FIFO_SIZE) {
        return false;
    }
    u->rx_fifo[u->rx_tail] = v;
    u->rx_tail = (u->rx_tail + 1) % UART_RX_FIFO_SIZE;
    u->rx_count++;
    uart_update_irq(sys);
    return true;
}

static uint8_t uart_rx_pop(RvSystem *sys) {
    Uart16550 *u = &sys->uart;
    uint8_t v = u->rx_fifo[u->rx_head];
    u->rx_head = (u->rx_head + 1) % UART_RX_FIFO_SIZE;
    u->rx_count--;
    uart_update_irq(sys);
    return v;
}

static uint64_t uart_read(RvSystem *p_sys, uint32_t offset) {
    Uart16550 *u = &p_sys->uart;
    switch (offset) {
        case 0: // RBR (pop one RX byte)/DLL
            if (u->lcr & 0x80) {
                return u->dll;
            }
            return u->rx_count ? uart_rx_pop(p_sys) : 0;
        case 1: return (u->lcr & 0x80) ? u->dlm : u->ier;
        case 2: {
            uint8_t fifo = (u->fcr & 0x01) ? 0xc0 : 0x00;
            if ((u->ier & 0x01) && u->rx_count) {
                return fifo | 0x04;
            }
            if (u->thr_ipending && (u->ier & 0x02)) {
                u->thr_ipending = false;
                uart_update_irq(p_sys);
                return fifo | 0x02;
            }
            return fifo | 0x01; // no interrupt pending
        }
        case 3: return u->lcr;
        case 5: return 0x60 | (u->rx_count ? 0x01 : 0); // LSR: THRE+TEMT, plus Data-Ready when RX queued
        case 6: return 0xB0; // MSR
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
}

static void uart_write(RvSystem *p_sys, uint32_t offset, uint8_t val) {
    Uart16550 *u = &p_sys->uart;
    switch (offset) {
        case 0:
            if (u->lcr & 0x80) {
                u->dll = val;
            } else {
                putchar(val); // UART -> console
                fflush(stdout);
                u->thr_ipending = true;
            }
            break;
        case 1:
            if (u->lcr & 0x80) {
                u->dlm = val;
            } else {
                uint8_t old_ier = u->ier;
                u->ier = val;
                if ((val & 0x02) && !(old_ier & 0x02)) {
                    u->thr_ipending = true;
                }
            }
            break;
        case 2: u->fcr = val; break;
        case 3: u->lcr = val; break;
        case 4: u->mcr = val; break;
        case 7: u->scr = val; break;
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
    uart_update_irq(p_sys); // THR/IER (and others) may have changed the pending THRE interrupt
}

static void interactive_serial_restore() {
    Uart16550 *u = s_interactive_uart;
    if (!u) {
        return;
    }
    if (u->stdin_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, u->stdin_flags);
        u->stdin_flags = -1;
    }
    if (u->stdin_termios_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &u->stdin_termios);
        u->stdin_termios_valid = false;
    }
}

// Put stdin into raw, non-blocking mode so guest keystrokes pass through unbuffered and a poll never
// blocks. Restored on exit. Ctrl-\ still kills the simulator; Ctrl-C is delivered to the guest.
static void interactive_serial_enable(Uart16550 *u) {
    u->interactive = true;
    u->stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    TTSIM_VERIFY(u->stdin_flags >= 0, SystemError, "fcntl(F_GETFL) failed: %s", strerror(errno));
    TTSIM_VERIFY(fcntl(STDIN_FILENO, F_SETFL, u->stdin_flags | O_NONBLOCK) == 0,
                 SystemError, "fcntl(F_SETFL O_NONBLOCK) failed: %s", strerror(errno));
    if (isatty(STDIN_FILENO)) {
        TTSIM_VERIFY(tcgetattr(STDIN_FILENO, &u->stdin_termios) == 0,
                     SystemError, "tcgetattr(stdin) failed: %s", strerror(errno));
        u->stdin_termios_valid = true;
        struct termios t = u->stdin_termios;
        t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        t.c_oflag &= ~OPOST;
        t.c_cflag |= CS8;
        t.c_lflag &= ~(ECHO | ICANON | IEXTEN); // keep ISIG (Ctrl-\); clear VINTR so Ctrl-C reaches guest
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        t.c_cc[VINTR] = _POSIX_VDISABLE;
        TTSIM_VERIFY(tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0,
                     SystemError, "tcsetattr(stdin) failed: %s", strerror(errno));
    }
    s_interactive_uart = u;
    atexit(interactive_serial_restore);
}

static void interactive_serial_poll(RvSystem *sys) {
    Uart16550 *u = &sys->uart;
    uint8_t buf[256];
    while (u->rx_count < UART_RX_FIFO_SIZE) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (!uart_rx_push(sys, buf[i] == '\n' ? '\r' : buf[i])) { // guest TTYs expect CR
                    break;
                }
            }
            continue;
        }
        if ((n == 0) || (errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
            return; // EOF or nothing queued
        }
        TTSIM_ERROR(SystemError, "read(stdin) failed: %s", strerror(errno));
    }
}

static void interactive_serial_poll_if_due(RvSystem *sys) {
    Uart16550 *u = &sys->uart;
    if (!u->interactive || (sys->visible_icount < u->interactive_next_poll)) {
        return;
    }
    u->interactive_next_poll = sys->visible_icount + INTERACTIVE_SERIAL_POLL_INSNS;
    interactive_serial_poll(sys);
}

// Deterministic scripted input: once the guest has retired inject_at instructions, drain inject_data
// into the RX FIFO (over multiple calls if the FIFO fills). '\n' is normalized to CR like a real TTY.
static void uart_inject_pump(RvSystem *sys) {
    Uart16550 *u = &sys->uart;
    if (!u->inject_enabled || (sys->insn_count < u->inject_at)) {
        return;
    }
    while ((u->inject_pos < u->inject_len) && (u->rx_count < UART_RX_FIFO_SIZE)) {
        uint8_t ch = u->inject_data[u->inject_pos++];
        uart_rx_push(sys, ch == '\n' ? '\r' : ch);
    }
    if (u->inject_pos == u->inject_len) {
        u->inject_enabled = false;
    }
}

static uint32_t plic_num_contexts(RvSystem *p_sys) {
    return 2 * p_sys->num_harts;
}

// Highest-priority enabled+pending source above this context's threshold (ties: lowest id),
// skipping sources already claimed (in service). 0 = none.
static int plic_best(RvSystem *p_sys, uint32_t ctx) {
    int best = 0;
    uint32_t best_pri = 0;
    for (uint32_t s = 1; s <= PLIC_NDEV; s++) {
        uint32_t w = s >> 5;
        uint32_t bit = 1u << (s & 31);
        if (!(p_sys->plic_pending[w] & bit)) {
            continue;
        }
        if (!(p_sys->plic_enable[ctx][w] & bit)) {
            continue;
        }
        if (p_sys->plic_claimed[w] & bit) {
            continue;
        }
        uint32_t pri = p_sys->plic_priority[s];
        if ((pri > p_sys->plic_threshold[ctx]) && (pri > best_pri)) {
            best_pri = pri;
            best = int(s);
        }
    }
    return best;
}

static void plic_update(RvSystem *p_sys) {
    for (uint32_t ctx = 0; ctx < plic_num_contexts(p_sys); ctx++) {
        bool pend = plic_best(p_sys, ctx) != 0;
        Rv64SysHartState *h = &p_sys->harts[ctx / 2];
        uint64_t bit = (ctx & 1) ? MIP_SEIP : MIP_MEIP;
        uint64_t old = h->mip;
        if (pend) {
            h->mip |= bit;
        } else {
            h->mip &= ~bit;
        }
        if (h->mip != old) {
            irq_reschedule(h); // external IRQ (de)asserted -> recompute irq_next_check (deliver promptly)
        }
    }
}

static uint64_t plic_read(RvSystem *sys, uint32_t offset) {
    TTSIM_VERIFY(!(offset & 3), UnimplementedFunctionality, "misaligned offset=0x%x", offset);
    if (offset < 0x1000) {
        return sys->plic_priority[offset / 4];
    }
    if ((offset >= 0x1000) && (offset < 0x1080)) {
        return sys->plic_pending[(offset - 0x1000) / 4];
    }
    if ((offset >= 0x2000) && (offset < 0x2000 + 0x80 * 2 * MAX_HARTS)) {
        uint32_t ctx = (offset - 0x2000) / 0x80;
        uint32_t w = ((offset - 0x2000) % 0x80) / 4;
        return (ctx < plic_num_contexts(sys) && w < 2) ? sys->plic_enable[ctx][w] : 0;
    }
    if (offset >= 0x200000) {
        uint32_t ctx = (offset - 0x200000) / 0x1000;
        uint32_t reg = (offset - 0x200000) % 0x1000;
        if (ctx >= plic_num_contexts(sys)) {
            return 0;
        }
        if (reg == 0) {
            return sys->plic_threshold[ctx];
        }
        if (reg == 4) { // CLAIM
            int s = plic_best(sys, ctx);
            if (s) {
                uint32_t w = uint32_t(s) >> 5;
                uint32_t b = 1u << (uint32_t(s) & 31);
                sys->plic_pending[w] &= ~b;
                sys->plic_claimed[w] |= b;
            }
            plic_update(sys);
            return uint32_t(s);
        }
    }
    TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
}

static void plic_write(RvSystem *p_sys, uint32_t offset, uint64_t val) {
    TTSIM_VERIFY(!(offset & 3), UnimplementedFunctionality, "misaligned offset=0x%x", offset);
    if (offset < 0x1000) {
        if (offset / 4 <= PLIC_NDEV) {
            p_sys->plic_priority[offset / 4] = uint32_t(val);
        }
    } else if ((offset >= 0x2000) && (offset < 0x2000 + 0x80 * 2 * MAX_HARTS)) {
        uint32_t ctx = (offset - 0x2000) / 0x80;
        uint32_t w = ((offset - 0x2000) % 0x80) / 4;
        if ((ctx < plic_num_contexts(p_sys)) && (w < 2)) {
            p_sys->plic_enable[ctx][w] = uint32_t(val);
        }
    } else if (offset >= 0x200000) {
        uint32_t ctx = (offset - 0x200000) / 0x1000;
        uint32_t reg = (offset - 0x200000) % 0x1000;
        if (ctx >= plic_num_contexts(p_sys)) {
            return;
        }
        if (reg == 0) {
            p_sys->plic_threshold[ctx] = uint32_t(val);
        } else if (reg == 4) { // COMPLETE
            uint32_t s = uint32_t(val);
            if ((s >= 1) && (s <= PLIC_NDEV)) {
                uint32_t w = s >> 5;
                uint32_t b = 1u << (s & 31);
                p_sys->plic_claimed[w] &= ~b;
                // Gateway re-pends if the source line is still asserted at completion (level source).
                if (p_sys->plic_level[w] & b) {
                    p_sys->plic_pending[w] |= b;
                }
            }
        }
    } else {
        TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%x", offset);
    }
    plic_update(p_sys);
}

void rv64_sys_set_irq(RvSystem *p_sys, uint32_t source, bool level) {
    uint32_t w = source >> 5;
    uint32_t b = 1u << (source & 31);
    if (level) {
        p_sys->plic_level[w] |= b;
        p_sys->plic_pending[w] |= b;
    } else {
        p_sys->plic_level[w] &= ~b;
    }
    plic_update(p_sys);
}

void rv64_sys_set_interactive(RvSystem *sys) {
    interactive_serial_enable(&sys->uart);
}

void rv64_sys_set_hart_quantum(RvSystem *p_sys, uint32_t q) {
    TTSIM_VERIFY(q, ConfigurationError, "invalid hart_quantum=%d", q);
    p_sys->hart_quantum = q;
}

void rv64_sys_uart_inject(RvSystem *p_sys, uint64_t at, const uint8_t *data, size_t len) {
    Uart16550 *u = &p_sys->uart;
    u->inject_data = (uint8_t *)malloc(len);
    TTSIM_VERIFY(u->inject_data || len == 0, SystemError, "malloc(%zu) for UART injection failed", len);
    memcpy(u->inject_data, data, len);
    u->inject_len = len;
    u->inject_pos = 0;
    u->inject_at = at;
    u->inject_enabled = true;
}

static inline uint8_t *virtio_dram_ptr(RvSystem *sys, uint64_t addr, uint64_t len) {
    if ((addr >= sys->dram_base) && (len <= sys->dram_size) &&
        (addr + len <= sys->dram_base + sys->dram_size)) {
        return sys->dram + (addr - sys->dram_base);
    }
    return nullptr; // not contained within DRAM
}

static void virtio_blk_process(RvSystem *sys) {
    if (!sys->vio_queue_ready || !sys->disk || !sys->vio_queue_num) {
        return;
    }
    uint32_t qsz = sys->vio_queue_num;
    TTSIM_VERIFY((qsz <= 1024) && ((qsz & (qsz - 1)) == 0), NonContractualBehavior,
        "virtio: invalid QueueNum %u (must be a power of two <= 1024)", qsz);
    uint16_t avail_idx;
    rv64_phys_read(sys, sys->vio_avail + 2, &avail_idx, 2);
    while (sys->vio_last_avail != avail_idx) {
        uint16_t head;
        uint64_t ring_addr = sys->vio_avail + 4 + 2 * (sys->vio_last_avail % qsz);
        rv64_phys_read(sys, ring_addr, &head, 2);
        TTSIM_VERIFY(head < qsz, NonContractualBehavior, "virtio: avail head %u >= qsz %u", head, qsz);
        uint8_t hd[16];
        rv64_phys_read(sys, sys->vio_desc + 16 * head, hd, 16);
        uint16_t hflags;
        memcpy(&hflags, hd + 12, 2);
        uint64_t desc_table = sys->vio_desc;
        uint16_t d = head;
        bool need_read = false;
        uint32_t desc_count = qsz;
        if (hflags & VRING_DESC_F_INDIRECT) {
            uint32_t ind_len;
            memcpy(&ind_len, hd + 8, 4);
            TTSIM_VERIFY(ind_len && (ind_len % 16) == 0, NonContractualBehavior,
                "virtio: bad indirect table length %u", ind_len);
            memcpy(&desc_table, hd, 8);
            d = 0;
            need_read = true;
            desc_count = ind_len / 16;
        }
        uint32_t type = 0xFFFFFFFFu;
        uint64_t sector = 0, disk_off = 0;
        uint32_t used_len = 0;
        bool first = true;
        for (uint32_t chain = 0;; chain++) {
            TTSIM_VERIFY((d < desc_count) && (chain < desc_count), NonContractualBehavior,
                "virtio: bad descriptor chain (d=%u count=%u chain=%u)", d, desc_count, chain);
            uint8_t dd[16];
            if (need_read) {
                rv64_phys_read(sys, desc_table + 16 * d, dd, 16);
            } else {
                memcpy(dd, hd, 16);
                need_read = true;
            }
            uint64_t addr;
            uint32_t len;
            uint16_t flags;
            uint16_t next;
            memcpy(&addr, dd, 8);
            memcpy(&len, dd + 8, 4);
            memcpy(&flags, dd + 12, 2);
            memcpy(&next, dd + 14, 2);
            TTSIM_VERIFY(!(flags & VRING_DESC_F_INDIRECT), NonContractualBehavior,
                "virtio: nested indirect descriptor (d=%u)", d);
            if (flags & VRING_DESC_F_WRITE) {
                used_len += len;
            }
            if (first) {
                uint8_t *hp = virtio_dram_ptr(sys, addr, len);
                TTSIM_VERIFY(hp && len >= 16 && !(flags & VRING_DESC_F_WRITE), NonContractualBehavior,
                    "virtio: bad request header desc (addr=0x%llx len=%u flags=0x%x)", addr, len, flags);
                memcpy(&type, hp, 4);
                memcpy(&sector, hp + 8, 8);
                TTSIM_VERIFY(sector <= sys->disk_sectors, NonContractualBehavior,
                    "virtio: start sector %llu past end-of-disk (%llu sectors)", sector, sys->disk_sectors);
                disk_off = sector * 512;
                first = false;
            } else if ((flags & VRING_DESC_F_WRITE) && (len == 1)) {
                uint8_t s = (type == VIRTIO_BLK_T_IN || type == VIRTIO_BLK_T_OUT ||
                             type == VIRTIO_BLK_T_FLUSH || type == VIRTIO_BLK_T_GET_ID) ? 0 : 1;
                rv64_phys_write(sys, addr, &s, 1);
            } else {
                uint8_t *dp = virtio_dram_ptr(sys, addr, len);
                TTSIM_VERIFY(dp, NonContractualBehavior, "virtio: data buffer 0x%llx+%u not in DRAM", addr, len);
                if ((type == VIRTIO_BLK_T_IN) || (type == VIRTIO_BLK_T_OUT)) {
                    TTSIM_VERIFY(disk_off + len <= sys->disk_bytes, NonContractualBehavior,
                        "virtio: %s past end-of-disk (off=%llu len=%u disk=%llu)",
                        type == VIRTIO_BLK_T_IN ? "read" : "write", disk_off, len, sys->disk_bytes);
                    TTSIM_VERIFY((type == VIRTIO_BLK_T_IN) == ((flags & VRING_DESC_F_WRITE) != 0), NonContractualBehavior,
                        "virtio: data buffer direction mismatch (type=0x%x flags=0x%x)", type, flags);
                    if (type == VIRTIO_BLK_T_IN) {
                        memcpy(dp, sys->disk_map + disk_off, len);
                    } else {
                        memcpy(sys->disk_map + disk_off, dp, len);
                    }
                } else if (type == VIRTIO_BLK_T_GET_ID) {
                    uint8_t z = 0;
                    rv64_phys_write(sys, addr, &z, 1);
                }
                disk_off += len;
            }
            if (!(flags & VRING_DESC_F_NEXT)) {
                break;
            }
            d = next;
        }
        if (type == VIRTIO_BLK_T_FLUSH) {
            fflush(sys->disk);
        }
        uint32_t id32 = head;
        uint32_t len32 = used_len;
        uint8_t ue[8];
        memcpy(ue, &id32, 4);
        memcpy(ue + 4, &len32, 4);
        uint64_t ue_addr = sys->vio_used + 4 + 8 * (sys->vio_used_idx % qsz);
        rv64_phys_write(sys, ue_addr, ue, 8);
        sys->vio_used_idx++;
        uint16_t uidx = sys->vio_used_idx;
        rv64_phys_write(sys, sys->vio_used + 2, &uidx, 2);
        sys->vio_last_avail++;
    }
    sys->vio_int_status |= 1;
    rv64_sys_set_irq(sys, VIRTIO_BLK_IRQ, true);
}

static uint8_t vio_cfg_byte(RvSystem *sys, uint32_t i) {
    if (i < 8) {
        return uint8_t(sys->disk_sectors >> (8 * i));
    }
    return 0;
}

static uint64_t virtio_read(RvSystem *sys, uint64_t paddr) {
    uint32_t slot = uint32_t((paddr - VIRTIO_BASE) / 0x1000);
    uint64_t off = (paddr - VIRTIO_BASE) % 0x1000;
    if ((slot != VIO_BLK_SLOT) || !sys->disk) {
        return 0;
    }
    switch (off) {
        case 0x000: return VIO_MAGIC;
        case 0x004: return VIO_VERSION;
        case 0x008: return 2;
        case 0x00c: return VIO_VENDOR;
        case 0x010:
            return (sys->vio_dev_feat_sel == 1) ? 1 : 0;
        case 0x034: return 1024;
        case 0x040: return 0;
        case 0x044: return sys->vio_queue_ready;
        case 0x060: return sys->vio_int_status;
        case 0x070: return sys->vio_status;
        case 0x0fc: return 0;
        case 0x100 ... 0x13F: {
            uint64_t v = 0;
            for (int k = 0; k < 8; k++) {
                uint32_t i = uint32_t(off - 0x100) + k;
                if (i < 0x40) {
                    v |= uint64_t(vio_cfg_byte(sys, i)) << (8 * k);
                }
            }
            return v;
        }
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%llx", off);
    }
}

static void virtio_write(RvSystem *sys, uint64_t paddr, uint32_t val) {
    uint32_t slot = uint32_t((paddr - VIRTIO_BASE) / 0x1000);
    uint64_t off = (paddr - VIRTIO_BASE) % 0x1000;
    if (slot != VIO_BLK_SLOT) {
        return;
    }
    switch (off) {
        case 0x014: sys->vio_dev_feat_sel = val; break;
        case 0x020: break;
        case 0x024: sys->vio_drv_feat_sel = val; break;
        case 0x030: break;
        case 0x038: sys->vio_queue_num = val; break;
        case 0x050: virtio_blk_process(sys); break;
        case 0x064:
            sys->vio_int_status &= ~val;
            rv64_sys_set_irq(sys, VIRTIO_BLK_IRQ, sys->vio_int_status != 0);
            break;
        case 0x070:
            sys->vio_status = val;
            if (val == 0) {
                sys->vio_queue_ready = 0;
                sys->vio_last_avail = 0;
                sys->vio_used_idx = 0;
                sys->vio_int_status = 0;
            }
            break;
        case 0x044: sys->vio_queue_ready = val; break; // QueueReady
        case 0x080: sys->vio_desc = (sys->vio_desc & ~0xFFFFFFFFull) | val; break;
        case 0x084: sys->vio_desc = (sys->vio_desc & 0xFFFFFFFFull) | (uint64_t(val) << 32); break;
        case 0x090: sys->vio_avail = (sys->vio_avail & ~0xFFFFFFFFull) | val; break;
        case 0x094: sys->vio_avail = (sys->vio_avail & 0xFFFFFFFFull) | (uint64_t(val) << 32); break;
        case 0x0a0: sys->vio_used = (sys->vio_used & ~0xFFFFFFFFull) | val; break;
        case 0x0a4: sys->vio_used = (sys->vio_used & 0xFFFFFFFFull) | (uint64_t(val) << 32); break;
        default: TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%llx", off);
    }
}

void rv64_sys_set_disk(RvSystem *p_sys, const char *path) {
    p_sys->disk = fopen(path, "rb");
    TTSIM_VERIFY(p_sys->disk, ConfigurationError, "cannot open --disk '%s'", path);
    fseeko(p_sys->disk, 0, SEEK_END);
    p_sys->disk_bytes = uint64_t(ftello(p_sys->disk));
    p_sys->disk_sectors = p_sys->disk_bytes / 512;
    p_sys->disk_map = (uint8_t *)mmap(nullptr, p_sys->disk_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, fileno(p_sys->disk), 0);
    TTSIM_VERIFY(p_sys->disk_map != MAP_FAILED, ConfigurationError, "cannot mmap --disk '%s'", path);
}

static uint64_t rtc_read(RvSystem *sys, uint64_t off) {
    switch (off) {
        case 0x00: {
            uint64_t insns = sys->visible_icount;
            uint64_t ns = RTC_BASE_NS + insns * (100 / sys->timer_insns_per_tick);
            sys->rtc_latched_high = uint32_t(ns >> 32);
            return uint32_t(ns);
        }
        case 0x04: return sys->rtc_latched_high;
        case 0x08: case 0x0C: case 0x18: return 0;
    }
    TTSIM_ERROR(UnimplementedFunctionality, "offset=0x%llx", off);
}

static uint64_t device_read(RvSystem *p_sys, uint64_t paddr, uint32_t size) {
    if ((paddr >= RESET_ROM_BASE) && (paddr - RESET_ROM_BASE <= RESET_ROM_SIZE - size)) {
        TTSIM_ERROR(UnimplementedFunctionality, "reset rom: paddr=0x%llx size=%d", paddr, size);
    }
    if ((paddr >= CLINT_BASE) && (paddr < CLINT_BASE + CLINT_SIZE)) {
        return clint_read(p_sys, paddr - CLINT_BASE, size);
    }
    if ((paddr >= PLIC_BASE) && (paddr < PLIC_BASE + PLIC_SIZE)) {
        TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "plic read size=%d", size);
        return plic_read(p_sys, paddr - PLIC_BASE);
    }
    if ((paddr >= VIRTIO_BASE) && (paddr < VIRTIO_BASE + VIRTIO_SIZE)) {
        TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "virtio read size=%d", size);
        return virtio_read(p_sys, paddr);
    }
    if ((paddr >= RTC_BASE) && (paddr < RTC_BASE + RTC_SIZE)) {
        TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "rtc read size=%d", size);
        return rtc_read(p_sys, paddr - RTC_BASE);
    }
    if ((paddr >= UART_BASE) && (paddr < UART_BASE + UART_SIZE)) {
        TTSIM_VERIFY(size == 1, UnimplementedFunctionality, "uart read size=%d", size);
        return uart_read(p_sys, paddr - UART_BASE);
    }
    if ((paddr >= SYSCON_BASE) && (paddr < SYSCON_BASE + SYSCON_SIZE)) {
        TTSIM_ERROR(UnimplementedFunctionality, "syscon: paddr=0x%llx size=%d", paddr, size);
    }
    TTSIM_ERROR(UnimplementedFunctionality, "paddr=0x%llx size=%d", paddr, size);
}

static void device_write(RvSystem *p_sys, uint64_t paddr, uint32_t size, uint64_t val) {
    if ((paddr >= CLINT_BASE) && (paddr < CLINT_BASE + CLINT_SIZE)) {
        return clint_write(p_sys, paddr - CLINT_BASE, size, val);
    }
    if ((paddr >= PLIC_BASE) && (paddr < PLIC_BASE + PLIC_SIZE)) {
        TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "plic write size=%d", size);
        return plic_write(p_sys, paddr - PLIC_BASE, val);
    }
    if ((paddr >= VIRTIO_BASE) && (paddr < VIRTIO_BASE + VIRTIO_SIZE)) {
        TTSIM_VERIFY(size == 4, UnimplementedFunctionality, "virtio write size=%d", size);
        return virtio_write(p_sys, paddr, uint32_t(val));
    }
    if ((paddr >= RTC_BASE) && (paddr < RTC_BASE + RTC_SIZE)) {
        TTSIM_ERROR(UnimplementedFunctionality, "rtc: paddr=0x%llx size=%d val=0x%llx", paddr, size, val);
    }
    if ((paddr >= UART_BASE) && (paddr < UART_BASE + UART_SIZE)) {
        TTSIM_VERIFY(size == 1, UnimplementedFunctionality, "uart write size=%d", size);
        return uart_write(p_sys, paddr - UART_BASE, uint8_t(val));
    }
    if ((paddr >= SYSCON_BASE) && (paddr < SYSCON_BASE + SYSCON_SIZE)) {
        TTSIM_ERROR(UnimplementedFunctionality, "syscon: paddr=0x%llx size=%d val=0x%llx", paddr, size, val);
    }
    TTSIM_ERROR(UnimplementedFunctionality, "paddr=0x%llx size=%d val=0x%llx", paddr, size, val);
}

bool rv64_sys_tt_attach(RvSystem *sys, const char *path, uint64_t ecam_base, uint64_t clock_burst) {
    TTSIM_ERROR_NOFMT(UnimplementedFunctionality);
}

static inline bool in_dram(RvSystem *p_sys, uint64_t paddr, uint32_t size) {
    return (paddr >= p_sys->dram_base) && (paddr + size <= p_sys->dram_base + p_sys->dram_size);
}

// Public 64-bit-size variant for the loader (file sizes can exceed 32 bits); overflow-safe form.
bool rv64_sys_in_dram(RvSystem *p_sys, uint64_t paddr, uint64_t size) {
    return (paddr >= p_sys->dram_base) && (size <= p_sys->dram_size) &&
           (paddr - p_sys->dram_base) <= (p_sys->dram_size - size);
}

void rv64_phys_read(RvSystem *p_sys, uint64_t paddr, void *dst, uint32_t size) {
    if (in_dram(p_sys, paddr, size)) {
        memcpy(dst, p_sys->dram + (paddr - p_sys->dram_base), size);
        return;
    }
    uint64_t v = device_read(p_sys, paddr, size);
    memcpy(dst, &v, size);
}

void rv64_phys_write(RvSystem *p_sys, uint64_t paddr, const void *src, uint32_t size) {
    if (in_dram(p_sys, paddr, size)) {
        memcpy(p_sys->dram + (paddr - p_sys->dram_base), src, size);
        return;
    }
    uint64_t v = 0;
    memcpy(&v, src, size);
    device_write(p_sys, paddr, size, v);
}

static inline bool page_has_reservation(const RvSystem *sys, uint64_t pa_page) {
    for (uint32_t i = 0; i < sys->num_harts; i++) {
        const Rv64SysHartState *o = &sys->harts[i];
        if (o->reservation_valid && ((o->reservation_paddr & ~0xFFFull) == pa_page)) {
            return true;
        }
    }
    return false;
}

static inline void clear_load_reservations_if_overlap(RvSystem *sys, const Rv64SysHartState *storer,
                                                      uint64_t pa, uint64_t len) {
    for (uint32_t i = 0; i < sys->num_harts; i++) {
        Rv64SysHartState *o = &sys->harts[i];
        if (o != storer && o->reservation_valid &&
            (pa < o->reservation_paddr + o->reservation_size) && (o->reservation_paddr < pa + len)) {
            o->reservation_valid = false;
        }
    }
}

static inline bool pte_access_ok(const Rv64SysHartState *h, uint64_t pte, MemAccess acc, uint32_t eff_priv) {
    if (!(pte & PTE_A)) {
        return false;
    }
    if ((acc == MemAccess::Store) && !(pte & PTE_D)) {
        return false;
    }
    bool u = pte & PTE_U;
    if (acc == MemAccess::Fetch) {
        return (pte & PTE_X) && (eff_priv == PRIV_S ? !u : u);
    }
    if (acc == MemAccess::Load) {
        bool allow_r = (pte & PTE_R) || ((h->mstatus & MSTATUS_MXR) && (pte & PTE_X));
        return allow_r && (eff_priv == PRIV_S ? (!u || (h->mstatus & MSTATUS_SUM)) : u);
    }
    return (pte & PTE_W) && (eff_priv == PRIV_S ? (!u || (h->mstatus & MSTATUS_SUM)) : u);
}

static inline uint64_t l2_tlb_tag(const Rv64SysHartState *h) {
    return (h->satp & ((1ull << 60) - 1)) | TLB_VALID_BIT;
}

static __attribute__((noinline)) bool translate_slow(Rv64SysHartState *h, uint64_t vaddr, MemAccess acc,
                                                     uint32_t eff_priv, uint64_t vpn_full, uint64_t *paddr) {
    uint32_t mode = (h->satp >> SATP_MODE_SHIFT) & 15;
    TTSIM_VERIFY(mode == SATP_MODE_SV39, UnimplementedFunctionality, "satp mode=%d (only Sv39 supported)", mode);

    uint64_t fault_cause = (acc == MemAccess::Fetch) ? EXC_INST_PAGE_FAULT :
                           (acc == MemAccess::Store) ? EXC_STORE_PAGE_FAULT : EXC_LOAD_PAGE_FAULT;

    L2TlbEntry *l2 = &h->l2tlb[vpn_full & (L2TLB_N - 1)];
    uint64_t l2tag = l2_tlb_tag(h);
    uint64_t ppn_base;
    if ((l2->tag == l2tag) && (l2->vpn == vpn_full) && pte_access_ok(h, l2->pte, acc, eff_priv)) {
        ppn_base = l2->pa_page;
    } else {
        if (((int64_t(vaddr) >> 39) != 0) && ((int64_t(vaddr) >> 39) != -1)) {
            rv64_sys_raise(h, fault_cause, vaddr);
            return false;
        }
        uint64_t vpn[3] = {(vaddr >> 12) & 0x1FF, (vaddr >> 21) & 0x1FF, (vaddr >> 30) & 0x1FF};
        uint64_t pte = 0, pte_addr = 0; uint32_t level = 0;
        uint64_t a = (h->satp & ((1ull << 44) - 1)) << 12;
        bool found = false;
        for (int lv = 2; lv >= 0; lv--) {
            pte_addr = a + vpn[lv] * 8;
            if (!in_dram(h->p_sys, pte_addr, 8)) {
                rv64_sys_raise(h, fault_cause, vaddr);
                return false;
            }
            rv64_phys_read(h->p_sys, pte_addr, &pte, 8);
            if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W))) {
                rv64_sys_raise(h, fault_cause, vaddr);
                return false;
            }
            if (pte & (PTE_R | PTE_X)) {
                uint64_t ppn = (pte >> 10) & ((1ull << 44) - 1);
                if ((lv > 0) && (ppn & ((1ull << (9 * lv)) - 1))) {
                    rv64_sys_raise(h, fault_cause, vaddr);
                    return false;
                }
                level = uint32_t(lv); found = true; break;
            }
            a = (((pte >> 10) & ((1ull << 44) - 1)) << 12);
        }
        if (!found) {
            rv64_sys_raise(h, fault_cause, vaddr);
            return false;
        }

        bool ok;
        if (acc == MemAccess::Fetch) {
            ok = pte & PTE_X;
        } else if (acc == MemAccess::Store) {
            ok = pte & PTE_W;
        } else {
            ok = (pte & PTE_R) || ((h->mstatus & MSTATUS_MXR) && (pte & PTE_X));
        }
        bool user_page = pte & PTE_U;
        if ((eff_priv == PRIV_U) && !user_page) {
            ok = false;
        }
        if ((eff_priv == PRIV_S) && user_page) {
            if ((acc == MemAccess::Fetch) || !(h->mstatus & MSTATUS_SUM)) {
                ok = false;
            }
        }
        if (!ok) {
            rv64_sys_raise(h, fault_cause, vaddr);
            return false;
        }
        uint64_t newpte = pte | PTE_A | ((acc == MemAccess::Store) ? PTE_D : 0);
        if (newpte != pte) {
            rv64_phys_write(h->p_sys, pte_addr, &newpte, 8);
            if (h->p_sys->num_harts > 1) {
                clear_load_reservations_if_overlap(h->p_sys, h, pte_addr, 8);
            }
            pte = newpte;
        }
        uint64_t ppn = (pte >> 10) & ((1ull << 44) - 1);
        if (level == 0) {
            ppn_base = (ppn << 12);
        } else if (level == 1) {
            ppn_base = ((ppn >> 9) << 21) | (vpn[0] << 12);
        } else {
            ppn_base = ((ppn >> 18) << 30) | (vpn[1] << 21) | (vpn[0] << 12);
        }
        if ((ppn_base >= h->p_sys->dram_base) && (ppn_base + 4096 <= h->p_sys->dram_base + h->p_sys->dram_size)) {
            l2->vpn = vpn_full;
            l2->tag = l2tag;
            l2->pa_page = ppn_base;
            l2->pte = pte;
        } else {
            l2->tag = 0;
        }
    }

    bool page_in_dram = (ppn_base >= h->p_sys->dram_base) &&
                        (ppn_base + 4096 <= h->p_sys->dram_base + h->p_sys->dram_size);
    TlbEntry *l1 = (acc == MemAccess::Fetch) ? &h->itlb[0] :
                   (acc == MemAccess::Store) ? &h->dtlb_store[vpn_full & (DTLB_N - 1)] :
                                               &h->dtlb_load[vpn_full & (DTLB_N - 1)];
    if (!(h->p_sys->num_harts > 1 && acc == MemAccess::Store && page_has_reservation(h->p_sys, ppn_base))) {
        if (page_in_dram) {
            l1->vpn = vpn_full;
            l1->host_page = h->p_sys->dram + (ppn_base - h->p_sys->dram_base);
        } else {
            l1->vpn = ~uint64_t(0);
        }
    }
    *paddr = ppn_base | (vaddr & 0xFFF);
    return true;
}

static inline bool translate(Rv64SysHartState *h, uint64_t vaddr, MemAccess acc, uint64_t *paddr) {
    uint32_t eff_priv = h->priv; // loads/stores honor MPRV (use mstatus.MPP); fetches use current priv
    if ((acc != MemAccess::Fetch) && (h->mstatus & MSTATUS_MPRV)) {
        eff_priv = (h->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;
    }
    uint32_t mode = (h->satp >> SATP_MODE_SHIFT) & 15;
    if ((eff_priv == PRIV_M) || (mode == SATP_MODE_BARE)) {
        *paddr = vaddr;
        return true;
    }
    return translate_slow(h, vaddr, acc, eff_priv, vaddr >> 12, paddr);
}

void rv64_sys_lr_reserve(Rv64SysHartState *h, uint64_t vaddr, uint32_t size) {
    RvSystem *p_sys = h->p_sys;
    if (p_sys->num_harts <= 1) {
        return; // single hart: the SC value-check suffices; no cross-hart coherency needed
    }
    uint64_t paddr;
    if (!translate(h, vaddr, MemAccess::Load, &paddr)) { // cannot fault here; defensive
        TTSIM_ERROR(AssertionFailure, "unexpected fault");
    }
    h->reservation_paddr = paddr;
    uint64_t pa_page = paddr & ~0xFFFull;
    if ((pa_page < p_sys->dram_base) || (pa_page >= p_sys->dram_base + p_sys->dram_size)) {
        return;
    }
    uint8_t *host_page = p_sys->dram + (pa_page - p_sys->dram_base);
    for (uint32_t i = 0; i < p_sys->num_harts; i++) {
        Rv64SysHartState *o = &p_sys->harts[i];
        for (uint32_t e = 0; e < 16; e++) {
            if ((o->dtlb_store[e].vpn != ~uint64_t(0)) && (o->dtlb_store[e].host_page == host_page)) {
                o->dtlb_store[e].vpn = ~uint64_t(0); // evict: force a re-translate (-> fill-refused)
            }
        }
    }
}

static bool rv64_load_crossing(Rv64SysHartState *h, uint64_t vaddr, uint8_t *dst, uint32_t size) {
    while (size) {
        uint32_t chunk = 0x1000 - (uint32_t(vaddr) & 0xFFF);
        if (chunk > size) {
            chunk = size;
        }
        if (!rv64_sys_load(h, vaddr, dst, chunk)) {
            return false;
        }
        vaddr += chunk;
        dst += chunk;
        size -= chunk;
    }
    return true;
}

static bool rv64_store_crossing(Rv64SysHartState *h, uint64_t vaddr, const uint8_t *src, uint32_t size) {
    while (size) {
        uint32_t chunk = 0x1000 - (uint32_t(vaddr) & 0xFFF);
        if (chunk > size) {
            chunk = size;
        }
        if (!rv64_sys_store(h, vaddr, src, chunk)) {
            return false;
        }
        vaddr += chunk;
        src += chunk;
        size -= chunk;
    }
    return true;
}

bool rv64_sys_load(Rv64SysHartState *h, uint64_t vaddr, void *dst, uint32_t size) {
    if ((uint32_t(vaddr) & 0xFFF) + size > 0x1000) [[unlikely]] {
        return rv64_load_crossing(h, vaddr, (uint8_t *)dst, size);
    }
    uint64_t paddr;
    if (!translate(h, vaddr, MemAccess::Load, &paddr)) [[unlikely]] {
        return false;
    }
    RvSystem *p_sys = h->p_sys;
    if (in_dram(p_sys, paddr, size)) [[likely]] {
        memcpy(dst, p_sys->dram + (paddr - p_sys->dram_base), size);
    } else {
        uint64_t v = device_read(p_sys, paddr, size);
        memcpy(dst, &v, size);
        h->steps_left = 0; // a device read can change interrupt state
    }
    return true;
}

bool rv64_sys_store(Rv64SysHartState *h, uint64_t vaddr, const void *src, uint32_t size) {
    if ((uint32_t(vaddr) & 0xFFF) + size > 0x1000) [[unlikely]] {
        return rv64_store_crossing(h, vaddr, (const uint8_t *)src, size);
    }
    uint64_t paddr;
    if (!translate(h, vaddr, MemAccess::Store, &paddr)) [[unlikely]] {
        return false;
    }
    RvSystem *p_sys = h->p_sys;
    if (in_dram(p_sys, paddr, size)) [[likely]] {
        memcpy(p_sys->dram + (paddr - p_sys->dram_base), src, size);
        if (p_sys->num_harts > 1) {
            clear_load_reservations_if_overlap(p_sys, h, paddr, size);
        }
    } else {
        uint64_t v = 0;
        memcpy(&v, src, size);
        device_write(p_sys, paddr, size, v);
        h->steps_left = 0; // a device write may raise an interrupt/etc.
    }
    return true;
}

bool rv64_sys_atomic_dram(Rv64SysHartState *h, uint64_t vaddr, uint32_t size, bool is_load) {
    uint64_t paddr;
    if (!translate(h, vaddr, MemAccess::Load, &paddr)) [[unlikely]] {
        return false; // trap already set
    }
    if (!in_dram(h->p_sys, paddr, size)) [[unlikely]] {
        rv64_sys_raise(h, is_load ? EXC_LOAD_ACCESS : EXC_STORE_ACCESS, vaddr);
        return false;
    }
    return true;
}

void rv64_sys_sfence(Rv64SysHartState *h, uint64_t vaddr, uint64_t asid) {
    tlb_flush(h);
    h->reservation_valid = false; // LR/SC reservation is conservatively dropped on any fence
}

uint64_t rv64_sys_read_csr(Rv64SysHartState *h, uint32_t csr, bool *ok) {
    *ok = true;
    switch (csr) {
        case CSR_MSTATUS: return h->mstatus;
        case CSR_MISA: return h->misa;
        case CSR_MEDELEG: return h->medeleg;
        case CSR_MIDELEG: return h->mideleg;
        case CSR_MIE: return h->mie;
        case CSR_MIP: return h->mip;
        case CSR_MTVEC: return h->mtvec;
        case CSR_MSCRATCH: return h->mscratch;
        case CSR_MEPC: return h->mepc;
        case CSR_MCAUSE: return h->mcause;
        case CSR_MTVAL: return h->mtval;
        case CSR_MCOUNTEREN: return h->mcounteren;
        case CSR_MCOUNTINHIBIT: return h->mcountinhibit;
        case CSR_MENVCFG: return h->menvcfg;
        case CSR_MHARTID: return h->hart_id;
        case CSR_MVENDORID: case CSR_MARCHID: case CSR_MIMPID: case CSR_MCONFIGPTR: return 0;
        case CSR_SSTATUS: return h->mstatus & SSTATUS_MASK;
        case CSR_SIE: return h->mie & h->mideleg;
        case CSR_SIP: return h->mip & h->mideleg;
        case CSR_STVEC: return h->stvec;
        case CSR_SSCRATCH: return h->sscratch;
        case CSR_SEPC: return h->sepc;
        case CSR_SCAUSE: return h->scause;
        case CSR_STVAL: return h->stval;
        case CSR_SATP: return h->satp;
        case CSR_SCOUNTEREN: return h->scounteren;
        case CSR_SENVCFG: return h->senvcfg;
        case CSR_TIME: return mtime_now(h->p_sys);
        case CSR_CYCLE: case CSR_INSTRET: return h->p_sys->visible_icount - h->p_sys->cause12_count;
        case CSR_FFLAGS: return h->fcsr & 0x1F;
        case CSR_FRM: return (h->fcsr >> 5) & 7;
        case CSR_FCSR: return h->fcsr & 0xFF;
        case CSR_PMPCFG0: return h->pmpcfg[0];
        case CSR_PMPCFG0 + 2: return h->pmpcfg[1];
        default:
            if ((csr >= CSR_PMPADDR0) && (csr <= CSR_PMPADDR0 + 63)) {
                uint32_t idx = csr - CSR_PMPADDR0;
                return idx < 16 ? h->pmpaddr[idx] : 0;
            }
            if ((csr >= 0x3A0) && (csr <= 0x3AE) && !(csr & 1)) {
                return 0;
            }
            if ((csr >= 0xB03) && (csr <= 0xB12)) {
                return h->mhpmcounter[csr - 0xB00];
            }
            if ((csr >= 0x323) && (csr <= 0x332)) {
                return h->mhpmevent[csr - 0x320];
            }
            if ((csr >= 0xC03) && (csr <= 0xC12)) {
                return h->mhpmcounter[csr - 0xC00];
            }
            *ok = false;
            return 0;
    }
}

void rv64_sys_write_csr(Rv64SysHartState *h, uint32_t csr, uint64_t val, bool *ok) {
    *ok = true;
    switch (csr) {
        case CSR_MSTATUS: case CSR_SSTATUS: case CSR_MIE: case CSR_SIE:
        case CSR_MIP: case CSR_SIP: case CSR_MIDELEG:
            h->steps_left = 0; // can make an already-pending interrupt deliverable
            break;
    }
    switch (csr) {
        case CSR_MSTATUS: {
            TTSIM_VERIFY((val & 0x7FFFFFF0FF818655ull) == 0, NonContractualBehavior,
                "mstatus write sets reserved (WPRI/hardwired) bits: 0x%llx", val);
            uint64_t m = (val & ~(MSTATUS_UXL | MSTATUS_SXL)) | MSTATUS_UXL | MSTATUS_SXL;
            if (((m & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT) == 2) {
                m &= ~MSTATUS_MPP; // legalize unsupported H -> U
            }
            h->mstatus = m;
            rv64_normalize_mstatus(&h->mstatus);
            irq_recompute_deliverable(h);
            tlb_flush_l1(h);
            break;
        }
        case CSR_MEDELEG: h->medeleg = val; break;
        case CSR_MIDELEG:
            h->mideleg = val & (MIP_SSIP | MIP_STIP | MIP_SEIP);
            irq_recompute_deliverable(h);
            break;
        case CSR_MIE:
            h->mie = val & (MIP_SSIP | MIP_MSIP | MIP_STIP | MIP_MTIP | MIP_SEIP | MIP_MEIP);
            irq_recompute_deliverable(h);
            break;
        case CSR_MIP:
            hart_write_mip(h, val);
            break;
        case CSR_MTVEC: h->mtvec = val & ~2ull; break; // WARL: MODE bit 1 reserved (only Direct/Vectored)
        case CSR_MSCRATCH: h->mscratch = val; break;
        case CSR_MEPC: h->mepc = val & ~1ull; break;
        case CSR_MCOUNTEREN: h->mcounteren = val & 0x7FFFF; break;
        case CSR_MCOUNTINHIBIT: h->mcountinhibit = val & 0x7FFFD; break;
        case CSR_MENVCFG:
            TTSIM_VERIFY(val <= 1ull, NonContractualBehavior,
                "menvcfg write sets reserved/unimplemented bits: 0x%llx", val);
            h->menvcfg = val;
            break;
        case CSR_SSTATUS:
            h->mstatus = (h->mstatus & ~SSTATUS_MASK) | (val & SSTATUS_MASK);
            rv64_normalize_mstatus(&h->mstatus);
            irq_recompute_deliverable(h);
            tlb_flush_l1(h);
            break;
        case CSR_SIE:
            h->mie = (h->mie & ~h->mideleg) | (val & h->mideleg);
            irq_recompute_deliverable(h);
            break;
        case CSR_SIP:
            h->mip = (h->mip & ~(h->mideleg & MIP_SSIP)) | (val & h->mideleg & MIP_SSIP);
            irq_recompute_deliverable(h);
            break;
        case CSR_STVEC: h->stvec = val & ~2ull; break; // WARL: MODE bit 1 reserved (only Direct/Vectored)
        case CSR_SSCRATCH: h->sscratch = val; break;
        case CSR_SEPC: h->sepc = val & ~1ull; break;
        case CSR_SATP: {
            uint32_t mode = (val >> 60) & 15;
            TTSIM_VERIFY((mode == 0) || (mode == 8), NonContractualBehavior,
                "menvcfg write sets reserved mode: %d", mode);
            h->satp = val;
            tlb_flush(h);
            break;
        }
        case CSR_SCOUNTEREN:
            TTSIM_VERIFY((val & ~0x7ffffull) == 0, NonContractualBehavior,
                "scounteren write sets reserved counter bits: 0x%llx", val);
            h->scounteren = val;
            break;
        case CSR_FCSR: h->fcsr = val & 0xFF; break;
        case CSR_PMPCFG0: h->pmpcfg[0] = val; break;
        case CSR_PMPADDR0 ... CSR_PMPADDR0 + 15: h->pmpaddr[csr - CSR_PMPADDR0] = val; break;
        case CSR_PMPADDR0 + 16 ... CSR_PMPADDR0 + 63: break; // WARL for OpenSBI probing
        case 0xB03 ... 0xB12: h->mhpmcounter[csr - 0xB00] = val; break;
        case 0x323 ... 0x332: h->mhpmevent[csr - 0x320] = val; break;
        default:
            TTSIM_ERROR(UnimplementedFunctionality, "csr=0x%x", csr);
    }
}

void rv64_sys_raise(Rv64SysHartState *h, uint64_t cause, uint64_t tval) {
    if ((cause == EXC_BREAKPOINT) || (cause == 8) || (cause == 9) || (cause == 11)) {
        uint64_t pc = current_insn_pc(h);
        uint64_t npc = pc;
        take_trap(h, &npc, cause, tval, false);
        h->trap_pending = true;
        h->trap_npc = npc;
        h->steps_left = 0;
        return;
    }
    fault(h, cause, tval);
}

void rv64_sys_take_trap(Rv64SysHartState *h, uint64_t cause, uint64_t tval, uint64_t epc) {
    bool interrupt = (cause & CAUSE_INTERRUPT) != 0;
    uint64_t code = cause & ~CAUSE_INTERRUPT;
    h->reservation_valid = false;
    bool to_s = false;
    if (h->priv <= PRIV_S) {
        uint64_t deleg = interrupt ? h->mideleg : h->medeleg;
        if ((deleg >> code) & 1) {
            to_s = true;
        }
    }
    if (to_s) {
        h->sepc = epc;
        h->scause = cause;
        h->stval = tval;
        uint64_t st = h->mstatus;
        st = (st & ~MSTATUS_SPIE) | ((st & MSTATUS_SIE) ? MSTATUS_SPIE : 0);
        st = (st & ~MSTATUS_SPP) | ((h->priv == PRIV_S) ? MSTATUS_SPP : 0);
        st &= ~MSTATUS_SIE;
        h->mstatus = st;
        h->priv = PRIV_S;
        uint64_t base = h->stvec & ~3ull;
        h->pc = ((h->stvec & 1) && interrupt) ? (base + 4 * code) : base;
    } else {
        h->mepc = epc;
        h->mcause = cause;
        h->mtval = tval;
        uint64_t st = h->mstatus;
        st = (st & ~MSTATUS_MPIE) | ((st & MSTATUS_MIE) ? MSTATUS_MPIE : 0);
        st = (st & ~MSTATUS_MPP) | (uint64_t(h->priv) << MSTATUS_MPP_SHIFT);
        st &= ~MSTATUS_MIE;
        h->mstatus = st;
        h->priv = PRIV_M;
        uint64_t base = h->mtvec & ~3ull;
        h->pc = ((h->mtvec & 1) && interrupt) ? (base + 4 * code) : base;
    }
    tlb_flush_l1(h);
}

void rv64_sys_xret(Rv64SysHartState *h, bool from_machine) {
    h->steps_left = 0;
    h->reservation_valid = false;
    if (from_machine) {
        uint32_t mpp = (h->mstatus & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;
        uint64_t st = h->mstatus;
        st = (st & ~MSTATUS_MIE) | ((st & MSTATUS_MPIE) ? MSTATUS_MIE : 0);
        st |= MSTATUS_MPIE;
        st &= ~MSTATUS_MPP;
        if (mpp != PRIV_M) {
            st &= ~MSTATUS_MPRV;
        }
        h->mstatus = st;
        h->priv = mpp;
        h->pc = h->mepc;
    } else {
        uint32_t spp = (h->mstatus & MSTATUS_SPP) ? PRIV_S : PRIV_U;
        uint64_t st = h->mstatus;
        st = (st & ~MSTATUS_SIE) | ((st & MSTATUS_SPIE) ? MSTATUS_SIE : 0);
        st |= MSTATUS_SPIE;
        st &= ~MSTATUS_SPP;
        if (spp != PRIV_M) {
            st &= ~MSTATUS_MPRV;
        }
        h->mstatus = st;
        h->priv = spp;
        h->pc = h->sepc;
    }
    tlb_flush_l1(h);
    irq_recompute_deliverable(h);
}

static inline uint64_t current_insn_pc(const Rv64SysHartState *p_hart) {
    return p_hart->pc - p_hart->pc_adjust;
}

static inline void clear_load_reservation(Rv64SysHartState *p_hart) {
    p_hart->reservation_valid = false;
}

[[maybe_unused]] static inline void scheduler_request_yield(Rv64SysHartState *p_hart) {
    if (g_sys.num_harts <= 1) {
        return;
    }
    g_sys.yield_hart = (p_hart->hart_id + 1) % g_sys.num_harts;
    p_hart->steps_left = 0;
}

static uint64_t clint_mtime_ticks_visible(const RvSystem *p_clint) {
    return g_sys.visible_icount / p_clint->timer_insns_per_tick;
}

static uint64_t clint_mtime_visible(const RvSystem *p_clint) {
    return p_clint->mtime_base + clint_mtime_ticks_visible(p_clint);
}

static uint64_t clint_mtip_event_count(const RvSystem *p_clint, const Rv64SysHartState *p_hart) {
    uint64_t mtimecmp = p_clint->mtimecmp[p_hart->hart_id];
    if (mtimecmp == ~0ULL) {
        return UINT64_MAX;
    }
    if (clint_mtime_visible(p_clint) >= mtimecmp) {
        return g_sys.visible_icount;
    }
    if (mtimecmp < p_clint->mtime_base) {
        return g_sys.visible_icount;
    }
    uint64_t target_ticks = mtimecmp - p_clint->mtime_base;
    if (target_ticks > UINT64_MAX / p_clint->timer_insns_per_tick) return UINT64_MAX;
    return target_ticks * p_clint->timer_insns_per_tick;
}

static void irq_reschedule(Rv64SysHartState *p_hart) {
    uint64_t next = UINT64_MAX;
    uint64_t static_pending = p_hart->mip & p_hart->irq_deliverable & ~MIP_MTIP;
    if (static_pending) {
        next = g_sys.visible_icount;
    } else if (p_hart->irq_deliverable & MIP_MTIP) {
        uint64_t mtip_count = clint_mtip_event_count(p_hart->p_sys, p_hart);
        if (mtip_count < next) {
            next = mtip_count;
        }
    }
    p_hart->irq_next_check = next;
    p_hart->steps_left = 0;
}

static uint8_t *translate_host_cached(Rv64SysHartState *p_hart, uint64_t vaddr, MemAccess acc, uint64_t *pa_out) {
    uint64_t pa;
    if (!translate(p_hart, vaddr, acc, &pa)) {
        *pa_out = 0;
        return nullptr; // fault raised
    }
    *pa_out = pa;
    RvSystem *p_sys = p_hart->p_sys;
    uint64_t page = pa & ~uint64_t(0xFFF);
    uint8_t *host = nullptr;
    if ((pa >= p_sys->dram_base) && (pa < p_sys->dram_base + p_sys->dram_size)) {
        host = p_sys->dram + (page - p_sys->dram_base);
    } else if ((pa >= rv64_sys_reset_rom_base()) && (pa < rv64_sys_reset_rom_base() + RESET_ROM_SIZE)) {
        host = p_sys->reset_rom + (page - rv64_sys_reset_rom_base());
    }
    if (!host) {
        p_hart->itlb[0].vpn = ~uint64_t(0);
        return nullptr;
    }
    p_hart->itlb[0].vpn = vaddr >> 12;
    p_hart->itlb[0].host_page = host;
    return host;
}

static void take_trap(Rv64SysHartState *p_hart, uint64_t *npc, uint64_t cause, uint64_t tval, bool is_interrupt) {
    uint32_t old_prv = p_hart->priv;
    uint64_t code_bit = 1ull << (cause & 63);
    uint64_t deleg = is_interrupt ? p_hart->mideleg : p_hart->medeleg;
    bool to_s = (p_hart->priv <= 1) && (deleg & code_bit);

    uint64_t cause_val = cause | (is_interrupt ? (1ull << 63) : 0);
    uint64_t epc = current_insn_pc(p_hart);
    if (to_s) {
        p_hart->sepc = epc;
        p_hart->scause = cause_val;
        p_hart->stval = tval;
        // sstatus: SPIE = SIE, SIE = 0, SPP = prv(0|1)
        uint64_t ms = p_hart->mstatus;
        uint64_t sie = (ms >> 1) & 1;
        ms &= ~((1ull << 1) | (1ull << 5) | (1ull << 8));
        ms |= (sie << 5) | (uint64_t(p_hart->priv & 1) << 8);
        p_hart->mstatus = ms;
        p_hart->priv = 1;
        clear_load_reservation(p_hart);
        uint64_t tvec = p_hart->stvec;
        *npc = (tvec & ~3ull);
    } else {
        p_hart->mepc = epc;
        p_hart->mcause = cause_val;
        p_hart->mtval = tval;
        uint64_t ms = p_hart->mstatus;
        uint64_t mie = (ms >> 3) & 1;
        ms &= ~((1ull << 3) | (1ull << 7) | (3ull << 11));
        ms |= (mie << 7) | (uint64_t(p_hart->priv) << 11);
        p_hart->mstatus = ms;
        p_hart->priv = 3;
        clear_load_reservation(p_hart);
        uint64_t tvec = p_hart->mtvec;
        *npc = (tvec & ~3ull);
    }
    if (p_hart->priv != old_prv) {
        tlb_flush_l1(p_hart);
    }
    irq_recompute_deliverable(p_hart);
}

// Raise a synchronous exception. Callers must stop mutating architectural
// state once trap_pending is set; the main loop handles final accounting.
static void fault(Rv64SysHartState *p_hart, uint64_t cause, uint64_t tval) {
    uint64_t fault_pc = current_insn_pc(p_hart);
    uint64_t npc = fault_pc;
    take_trap(p_hart, &npc, cause, tval, false);
    p_hart->trap_pending = true;
    p_hart->trap_pc = fault_pc;
    p_hart->trap_npc = npc;
    if (cause != 12) { // for anything other than an instruction fetch fault...
        g_sys.visible_icount--; // cancel out the later increment
    }
    p_hart->steps_left = 0;
}

static void irq_recompute_deliverable(Rv64SysHartState *p_hart) {
    uint64_t enabled = p_hart->mie;
    uint64_t mideleg = p_hart->mideleg;
    uint64_t ms = p_hart->mstatus;
    bool mie = (ms >> 3) & 1;
    bool sie = (ms >> 1) & 1;

    uint64_t m_enabled = enabled & ~mideleg;
    uint64_t s_enabled = enabled & mideleg;
    if (!((p_hart->priv < 3) || ((p_hart->priv == 3) && mie))) {
        m_enabled = 0;
    }
    if (!((p_hart->priv < 1) || ((p_hart->priv == 1) && sie))) {
        s_enabled = 0;
    }
    p_hart->irq_deliverable = m_enabled | s_enabled;
    irq_reschedule(p_hart);
}

[[maybe_unused]] static void hart_write_mip(Rv64SysHartState *p_hart, uint64_t v) {
    clint_refresh_irq(p_hart);
    p_hart->mip = (v & ~MIP_PLATFORM_MASK) | (p_hart->mip & MIP_PLATFORM_MASK);
    irq_reschedule(p_hart);
}

static void wfi_advance_virtual_time(Rv64SysHartState *p_hart) {
    RvSystem *p_sys = p_hart->p_sys;
    uint64_t mtimecmp = p_sys->mtimecmp[p_hart->hart_id];
    if ((mtimecmp == ~0ULL) || (clint_mtime_visible(p_sys) >= mtimecmp)) {
        return;
    }
    if (mtimecmp < p_sys->mtime_base) {
        return;
    }

    uint64_t target_ticks = mtimecmp - p_sys->mtime_base;
    if (target_ticks > UINT64_MAX / p_sys->timer_insns_per_tick) {
        target_ticks = UINT64_MAX / p_sys->timer_insns_per_tick;
    }
    uint64_t target_count = target_ticks * p_sys->timer_insns_per_tick;
    if (target_count > g_sys.visible_icount) {
        g_sys.visible_icount = target_count;
    }
}

static bool hart_has_wfi_wake_event(Rv64SysHartState *p_hart) {
    clint_refresh_irq(p_hart);
    return (p_hart->mip & MIP_PLATFORM_MASK) != 0;
}

static bool hart_can_run_after_schedule_yield(Rv64SysHartState *p_hart) {
    if (!p_hart->wfi_sleeping) {
        return true;
    }
    if (hart_has_wfi_wake_event(p_hart)) {
        return true;
    }
    return false;
}

static Rv64SysHartState *scheduler_choose_hart(Rv64SysHartState *p_current_hart, uint32_t *p_next_hart) {
    if (g_sys.yield_hart < g_sys.num_harts) {
        uint32_t start = g_sys.yield_hart;
        g_sys.yield_hart = UINT32_MAX;
        for (uint32_t i = 0; i < g_sys.num_harts; i++) {
            uint32_t h = (start + i) % g_sys.num_harts;
            Rv64SysHartState *p_hart = &g_sys.harts[h];
            if (!hart_can_run_after_schedule_yield(p_hart)) {
                continue;
            }
            p_hart->wfi_sleeping = false;
            *p_next_hart = (h + 1) % g_sys.num_harts;
            return p_hart;
        }
    }

    if (p_current_hart && !p_current_hart->wfi_sleeping) {
        return p_current_hart;
    }

    bool allow_reset_wait = p_current_hart && p_current_hart->wfi_sleeping;
    for (uint32_t i = 0; i < g_sys.num_harts; i++) {
        uint32_t h = (*p_next_hart + i) % g_sys.num_harts;
        Rv64SysHartState *p_hart = &g_sys.harts[h];
        bool reset_vector_wait = p_hart->wfi_sleeping && false; // XXX reset ROM
        if (p_hart->wfi_sleeping && !(allow_reset_wait && reset_vector_wait) && !hart_has_wfi_wake_event(p_hart)) {
            continue;
        }
        p_hart->wfi_sleeping = false;
        *p_next_hart = (h + 1) % g_sys.num_harts;
        return p_hart;
    }

    uint64_t next = UINT64_MAX;
    for (uint32_t h = 0; h < g_sys.num_harts; h++) {
        uint64_t mtip_count = clint_mtip_event_count(g_sys.harts[h].p_sys, &g_sys.harts[h]);
        if (mtip_count < next) {
            next = mtip_count;
        }
    }
    if (next != UINT64_MAX && next > g_sys.visible_icount) {
        g_sys.visible_icount = next;
    }
    for (uint32_t i = 0; i < g_sys.num_harts; i++) {
        uint32_t h = (*p_next_hart + i) % g_sys.num_harts;
        Rv64SysHartState *p_hart = &g_sys.harts[h];
        if (hart_has_wfi_wake_event(p_hart)) {
            p_hart->wfi_sleeping = false;
            *p_next_hart = (h + 1) % g_sys.num_harts;
            return p_hart;
        }
    }
    TTSIM_ERROR(AssertionFailure, "all harts sleeping with no scheduled interrupt");
}

static void clint_refresh_irq(Rv64SysHartState *p_hart) {
    RvSystem *p_clint = p_hart->p_sys;
    uint32_t hart_id = p_hart->hart_id;
    uint64_t mip = p_hart->mip;
    if (clint_mtime_visible(p_clint) >= p_clint->mtimecmp[hart_id]) {
        mip |= MIP_MTIP;
    } else {
        mip &= ~MIP_MTIP;
    }
    if (p_clint->msip[hart_id] & 1) {
        mip |= MIP_MSIP;
    } else {
        mip &= ~MIP_MSIP;
    }
    p_hart->mip = mip;
    irq_reschedule(p_hart);
}

static void check_interrupts(Rv64SysHartState *p_hart) {
    RvSystem *p_sys = p_hart->p_sys;

    uint64_t deliverable = p_hart->irq_deliverable;
    if (!deliverable) [[likely]] {
        return irq_reschedule(p_hart);
    }

    uint64_t mip = p_hart->mip;
    if (deliverable & MIP_MTIP) {
        if (clint_mtime_visible(p_sys) >= p_sys->mtimecmp[p_hart->hart_id]) {
            mip |= MIP_MTIP;
        } else {
            mip &= ~MIP_MTIP;
        }
        p_hart->mip = mip;
    }
    uint64_t all = mip & deliverable;
    if (!all) [[likely]] {
        return irq_reschedule(p_hart);
    }

    int cause = -1;
    static const int pri[] = {11, 7, 3, 9, 1, 5};
    for (size_t i = 0; i < sizeof(pri) / sizeof(pri[0]); i++) {
        if (all & (1ULL << pri[i])) {
            cause = pri[i];
            break;
        }
    }
    if (cause < 0) {
        return;
    }
    uint64_t npc = current_insn_pc(p_hart);
    take_trap(p_hart, &npc, uint64_t(cause), 0, true);
    p_hart->pc = npc;
}

void rv64_sys_run(RvSystem *sys, uint64_t max_insns) {
    Rv64SysHartState *p_hart = nullptr;
    uint32_t next_hart = 0;
    while (g_sys.insn_count < max_insns) {
        p_hart = scheduler_choose_hart(p_hart, &next_hart);
        g_sys.active_hart = p_hart;
        interactive_serial_poll_if_due(sys);
        uart_inject_pump(sys);
        if (g_sys.visible_icount >= p_hart->irq_next_check) [[unlikely]] {
            check_interrupts(p_hart);
        }

        if (p_hart->irq_next_check <= g_sys.visible_icount) {
            p_hart->steps_left = 1;
        } else {
            p_hart->steps_left = max_insns - g_sys.insn_count;
            uint64_t irq_steps = p_hart->irq_next_check - g_sys.visible_icount;
            if (irq_steps < p_hart->steps_left) {
                p_hart->steps_left = irq_steps;
            }
        }

        while (p_hart->steps_left) {
            p_hart->steps_left--;
            uint64_t fetch_vpn = p_hart->pc >> 12;
            auto *p_itlb = &p_hart->itlb[0];
            if (p_itlb->vpn != fetch_vpn) [[unlikely]] {
                uint64_t pa = 0;
                uint8_t *p = translate_host_cached(p_hart, p_hart->pc, MemAccess::Fetch, &pa);
                if (!p) [[unlikely]] {
                    TTSIM_VERIFY(p_hart->trap_pending, AssertionFailure, "ifetch mmio: pa=0x%lx", pa);
                    break;
                }
            }
            uint32_t fetch_off = p_hart->pc & 0xFFF;
            uint32_t raw = mem_rd<uint32_t>(p_itlb->host_page + fetch_off);
            if ((fetch_off == 0xFFE) && ((raw & 3) == 3)) [[unlikely]] {
                uint64_t pa = 0;
                uint8_t *p = translate_host_cached(p_hart, p_hart->pc + 2, MemAccess::Fetch, &pa);
                if (!p) [[unlikely]] {
                    TTSIM_VERIFY(p_hart->trap_pending, AssertionFailure, "ifetch mmio: pa=0x%lx", pa);
                    break;
                }
                uint16_t hw1 = mem_rd<uint16_t>(p_itlb->host_page);
                raw = (raw & 0xFFFF) | (uint32_t(hw1) << 16);
            }
            uint64_t insn_pc = p_hart->pc;
            if ((raw & 3) == 3) [[likely]] {
                p_hart->pc_adjust = 4;
                p_hart->pc = insn_pc + 4;
                uint32_t f3 = (raw >> 12) & 7;
                uint32_t opcode = ((raw & 0x7f) >> 2) | (f3 << 5);
                s_decode_table[opcode](p_hart, raw);
            } else {
                p_hart->pc_adjust = 2;
                p_hart->pc = insn_pc + 2;
                uint32_t opcode = ((raw >> 11) & 0x1c) | (raw & 3);
                s_rv64c_decode_table[opcode](p_hart, raw);
            }
            p_hart->pc_adjust = 0;
            g_sys.visible_icount++;
            g_sys.insn_count++;
            if ((g_sys.num_harts > 1) && (g_sys.hart_quantum != 0) &&
                ((g_sys.insn_count % g_sys.hart_quantum) == 0)) [[unlikely]] {
                scheduler_request_yield(p_hart);
            }
        }
        if (p_hart->trap_pending) [[unlikely]] {
            TTSIM_VERIFY(!p_hart->wfi_retired, AssertionFailure, "should not have WFI retired with trap pending");
            TTSIM_VERIFY(!p_hart->pc_adjust, AssertionFailure, "should not have PC adjust set with trap pending");
            p_hart->pc = p_hart->trap_npc;
            p_hart->trap_pending = false;
            continue;
        }
        if (p_hart->wfi_retired) [[unlikely]] {
            p_hart->wfi_retired = false;
            uart_inject_pump(sys);
            if (p_hart->irq_next_check <= g_sys.visible_icount) {
                continue;
            }
            if (sys->uart.interactive) {
                interactive_serial_poll(sys);
                if (p_hart->irq_next_check <= g_sys.visible_icount) {
                    continue;
                }
            }
            if (g_sys.num_harts == 1) {
                wfi_advance_virtual_time(p_hart);
            }
            p_hart->wfi_sleeping = true;
        }
    }
}
