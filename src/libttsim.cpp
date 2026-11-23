// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// libttsim.so exposes the device's physical I/O interfaces (e.g. PCIE, Ethernet).  These
// symbols form a stable, binary-compatible API intended for long-term use; changes should
// be rare and should always correspond to physical ports/connectivity on the device.
// The API/ABI contract and compatibility policy are documented in docs/libttsim_api.md.
#include "sim.h"
#include "pcie.h"

#define API_EXPORT __attribute((visibility("default")))

// Host-float results are exact only under the default FP environment (RNE, no FTZ/DAZ, exceptions masked), which
// the host may have changed. Every entry point runs under it, and the host's environment is back in place whenever
// the host runs: on return and in the DMA callbacks.
#if defined(__x86_64__)
#include <xmmintrin.h>
using FpEnv = uint32_t; // MXCSR
static constexpr FpEnv DEFAULT_FPENV = 0x1F80;
static inline FpEnv fpenv_read() {
    return _mm_getcsr();
}
static inline void fpenv_write(FpEnv env) {
    _mm_setcsr(env);
}
#elif defined(__aarch64__)
struct FpEnv {
    uint64_t fpcr;
    uint64_t fpsr;
};
static constexpr FpEnv DEFAULT_FPENV = {0, 0};
static inline FpEnv fpenv_read() {
    FpEnv env;
    asm volatile("mrs %0, fpcr" : "=r"(env.fpcr) :: "memory");
    asm volatile("mrs %0, fpsr" : "=r"(env.fpsr) :: "memory");
    return env;
}
static inline void fpenv_write(FpEnv env) {
    asm volatile("msr fpcr, %0" :: "r"(env.fpcr) : "memory");
    asm volatile("msr fpsr, %0" :: "r"(env.fpsr) : "memory");
}
#elif defined(__riscv)
using FpEnv = uint32_t; // fcsr
static constexpr FpEnv DEFAULT_FPENV = 0;
static inline FpEnv fpenv_read() {
    FpEnv env;
    asm volatile("frcsr %0" : "=r"(env) :: "memory");
    return env;
}
static inline void fpenv_write(FpEnv env) {
    asm volatile("fscsr %0" :: "r"(env) : "memory");
}
#else
#error "unsupported host"
#endif

static FpEnv s_host_fpenv;

struct FpEnvGuard {
    FpEnvGuard() {
        s_host_fpenv = fpenv_read();
        fpenv_write(DEFAULT_FPENV);
    }
    ~FpEnvGuard() {
        fpenv_write(s_host_fpenv);
    }
};

static bool s_ttsim_running = false;
static void (*s_pfn_libttsim_pci_dma_mem_rd_bytes)(uint64_t paddr, void *p, uint32_t size);
static void (*s_pfn_libttsim_pci_dma_mem_wr_bytes)(uint64_t paddr, const void *p, uint32_t size);

extern "C" API_EXPORT void libttsim_init() {
    FpEnvGuard fpenv_guard;
    TTSIM_VERIFY(!s_ttsim_running, ConfigurationError, "sim is already running");
    ttsim_init();
    pcie_init();
    s_ttsim_running = true;
}

extern "C" API_EXPORT void libttsim_exit() {
    FpEnvGuard fpenv_guard;
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    ttsim_exit();
    s_ttsim_running = false;
}

extern "C" API_EXPORT void libttsim_set_pci_dma_mem_callbacks(
    decltype(s_pfn_libttsim_pci_dma_mem_rd_bytes) pci_dma_mem_rd,
    decltype(s_pfn_libttsim_pci_dma_mem_wr_bytes) pci_dma_mem_wr
) {
    FpEnvGuard fpenv_guard;
    TTSIM_VERIFY(!s_ttsim_running, ConfigurationError, "sim is already running");
    s_pfn_libttsim_pci_dma_mem_rd_bytes = pci_dma_mem_rd;
    s_pfn_libttsim_pci_dma_mem_wr_bytes = pci_dma_mem_wr;
}

// Map a bus/device/function tuple to an MMIO chip index, or g_num_mmio_chips if the slot is empty.
// An absent-but-well-formed BDF is not an error: hosts discover endpoints by probing empty slots.
static uint32_t pci_device_from_bdf(uint32_t bus_device_function) {
    // normally only 16 bits, but validate none above these are set
    TTSIM_VERIFY(!(bus_device_function >> 16), UndefinedBehavior, "bus_device_function=0x%x", bus_device_function);
    uint32_t function = bus_device_function & 7;
    uint32_t bdf_device = (bus_device_function >> 3) & 0x1F;
    uint32_t bus = (bus_device_function >> 8) & 0xFF;
    if (function) {
        return g_num_mmio_chips; // single-function endpoints; functions 1-7 are absent
    }
    if (g_num_chips == 32) {
        // Galaxy: chip N sits at the bus its cluster descriptor gives it. As on silicon,
        // each ASIC is its own PCI bus at device 0; the bus's high nibble is the tray and
        // its low nibble the 1-based ASIC slot. Every Galaxy chip is an MMIO chip.
        static constexpr uint8_t CHIP_BUS[32] = {
#if TT_ARCH_VERSION == 0
            0xC1, 0x41, 0x01, 0x81, 0xC2, 0x42, 0x02, 0x82,
            0xC3, 0x43, 0x03, 0x83, 0xC4, 0x44, 0x04, 0x84,
            0xC5, 0x45, 0x05, 0x85, 0xC6, 0x46, 0x06, 0x86,
            0xC7, 0x47, 0x07, 0x87, 0xC8, 0x48, 0x08, 0x88,
#else
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
            0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
            0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8,
#endif
        };
        TTSIM_ASSERT(g_num_mmio_chips == 32);
        for (uint32_t device = 0; device < 32; device++) {
            if ((bus == CHIP_BUS[device]) && (bdf_device == 0)) {
                return device;
            }
        }
        return g_num_mmio_chips;
    }
    return (bus == 0 && (bdf_device < g_num_mmio_chips)) ? bdf_device : g_num_mmio_chips;
}

extern "C" API_EXPORT uint32_t libttsim_pci_config_rd32(uint32_t bus_device_function, uint32_t offset) {
    FpEnvGuard fpenv_guard;
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    TTSIM_VERIFY(!(offset & 3), UndefinedBehavior, "misaligned offset=0x%x", offset);
    uint32_t device = pci_device_from_bdf(bus_device_function);
    if (device >= g_num_mmio_chips) {
        return 0xFFFFFFFFu; // non-existent device
    }
    return pcie_config_rd32(device, offset);
}

extern "C" API_EXPORT void libttsim_pci_config_wr32(uint32_t bus_device_function, uint32_t offset, uint32_t data) {
    FpEnvGuard fpenv_guard;
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    TTSIM_VERIFY(!(offset & 3), UndefinedBehavior, "misaligned offset=0x%x", offset);
    uint32_t device = pci_device_from_bdf(bus_device_function);
    TTSIM_VERIFY(device < g_num_mmio_chips, UndefinedBehavior, "bus_device_function=0x%x has no device",
        bus_device_function);
    pcie_config_wr32(device, offset, data);
}

// Route a host physical access to the owning MMIO device's chip, then translate within
// that device's BAR windows.
extern "C" API_EXPORT void libttsim_pci_mem_rd_bytes(uint64_t paddr, void *p, uint32_t size) {
    FpEnvGuard fpenv_guard;
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    TTSIM_VERIFY(size, UndefinedBehavior, "size=%d", size);
    uint32_t device = pcie_device_from_paddr(paddr, size);
    uint32_t saved_chip_id = g_current_chip_id;
    ttsim_select_chip(device);
    pcie_mem_rd_bytes(paddr, p, size);
    ttsim_select_chip(saved_chip_id);
}

extern "C" API_EXPORT void libttsim_pci_mem_wr_bytes(uint64_t paddr, const void *p, uint32_t size) {
    FpEnvGuard fpenv_guard;
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    TTSIM_VERIFY(size, UndefinedBehavior, "size=%d", size);
    uint32_t device = pcie_device_from_paddr(paddr, size);
    uint32_t saved_chip_id = g_current_chip_id;
    ttsim_select_chip(device);
    pcie_mem_wr_bytes(paddr, p, size);
    ttsim_select_chip(saved_chip_id);
}

void libttsim_pci_dma_mem_rd_bytes(uint64_t paddr, void *p, uint32_t size) {
    TTSIM_VERIFY(s_pfn_libttsim_pci_dma_mem_rd_bytes, ConfigurationError, "DMA read callback not installed");
    fpenv_write(s_host_fpenv);
    s_pfn_libttsim_pci_dma_mem_rd_bytes(paddr, p, size);
    s_host_fpenv = fpenv_read();
    fpenv_write(DEFAULT_FPENV);
}

void libttsim_pci_dma_mem_wr_bytes(uint64_t paddr, const void *p, uint32_t size) {
    TTSIM_VERIFY(s_pfn_libttsim_pci_dma_mem_wr_bytes, ConfigurationError, "DMA write callback not installed");
    fpenv_write(s_host_fpenv);
    s_pfn_libttsim_pci_dma_mem_wr_bytes(paddr, p, size);
    s_host_fpenv = fpenv_read();
    fpenv_write(DEFAULT_FPENV);
}

// transitional: tt-umd still uses these where they are legal (see docs/libttsim_api.md)
extern "C" API_EXPORT void libttsim_tile_rd_bytes(uint32_t x, uint32_t y, uint64_t addr, void *p, uint32_t size) {
    FpEnvGuard fpenv_guard;
    TTSIM_ERROR_NOFMT(UnsupportedFunctionality);
}

// transitional: tt-umd still uses these where they are legal (see docs/libttsim_api.md)
extern "C" API_EXPORT void libttsim_tile_wr_bytes(uint32_t x, uint32_t y, uint64_t addr, const void *p, uint32_t size) {
    FpEnvGuard fpenv_guard;
    TTSIM_ERROR_NOFMT(UnsupportedFunctionality);
}

// The first busy T tile at or after tile_id, or NUM_T_TILES if none
static inline uint32_t next_busy_t_tile(uint32_t tile_id) {
    uint64_t mask = ~0ull << (tile_id % 64);
    for (uint32_t word = tile_id / 64; word < std::size(g_t_tiles_busy); word++, mask = ~0ull) {
        if (uint64_t busy = g_t_tiles_busy[word] & mask) {
            return 64 * word + __builtin_ctzll(busy);
        }
    }
    return NUM_T_TILES;
}

static bool t_tile_active(const TensixTile *p_tile) {
    uint32_t active = p_tile->rv32_cores_active;
    for (uint32_t tensix_id = 0; tensix_id < std::size(p_tile->tensix); tensix_id++) {
        active |= p_tile->tensix[tensix_id].inst_pipes_active;
        active |= p_tile->tensix[tensix_id].expand_pipes_active;
    }
    return active;
}

#define PAUSE_CLOCKS 256 // a core that executes PAUSE sits out the clocks up to the next multiple of this

static void resume_paused_cores() {
    for (uint32_t tile_id = 0; tile_id < NUM_T_TILES; tile_id++) {
        TensixTile *p_tile = &g_t_tiles[tile_id];
        if (p_tile->rv32_cores_paused) {
            p_tile->rv32_cores_active |= p_tile->rv32_cores_paused;
            p_tile->rv32_cores_paused = 0;
            ttsim_t_tile_set_busy(tile_id);
        }
    }
    g_chip->t_tile_cores_paused = false;
}

static void clock_current_chip() {
    if (g_chip->t_tile_cores_paused && !(g_clock % PAUSE_CLOCKS)) {
        resume_paused_cores();
    }
    // Idle tiles are skipped. The mask is reread after each tile, so a tile woken by a lower-numbered one is still
    // clocked this cycle.
    for (uint32_t tile_id = next_busy_t_tile(0); tile_id < NUM_T_TILES; tile_id = next_busy_t_tile(tile_id + 1)) {
        TensixTile *p_tile = &g_t_tiles[tile_id];
        // Hack: for simulator perf, we clock a tile repeatedly as long as it successfully executes at least one Tensix instruction
        for (;;) {
            for (uint32_t rv32_mask = p_tile->rv32_cores_active; rv32_mask; rv32_mask &= rv32_mask - 1) {
                uint32_t rv32_index = __builtin_ctz(rv32_mask);
                rv32_step(&p_tile->rv32[rv32_index]);
            }
            [[maybe_unused]] bool any_tensix = false;
            for (uint32_t tensix_id = 0; tensix_id < std::size(p_tile->tensix); tensix_id++) {
                TensixState *p_tensix = &p_tile->tensix[tensix_id];
                for (uint32_t pipe_mask = p_tensix->expand_pipes_active; pipe_mask; pipe_mask &= pipe_mask - 1) {
                    tensix_frontend_step(p_tensix, __builtin_ctz(pipe_mask));
                }
                for (uint32_t pipe_mask = p_tensix->inst_pipes_active; pipe_mask; pipe_mask &= pipe_mask - 1) {
                    uint32_t pipe = __builtin_ctz(pipe_mask);
                    uint32_t inst_rd_ptr = p_tensix->inst_rd_ptr[pipe];
                    if (inst_rd_ptr != p_tensix->inst_wr_ptr[pipe]) {
                        uint32_t inst = p_tensix->inst[pipe][inst_rd_ptr];
                        if (tensix_decode_and_execute(p_tensix, pipe, inst)) {
                            any_tensix = true;
                            p_tensix->inst_origin_count[pipe][p_tensix->inst_origin[pipe][inst_rd_ptr]]--;
                            inst_rd_ptr = (inst_rd_ptr + 1) % TENSIX_INST_FIFO_SIZE;
                            p_tensix->inst_rd_ptr[pipe] = inst_rd_ptr;
                            if (inst_rd_ptr == p_tensix->inst_wr_ptr[pipe]) {
                                p_tensix->inst_pipes_active &= ~(1 << pipe);
                            }
                        }
                    }
                }
            }
            if (!any_tensix) {
                break; // no Tensix instructions executed, bail out
            }
        }
        if (!t_tile_active(p_tile)) {
            g_t_tiles_busy[tile_id / 64] &= ~(1ull << (tile_id % 64));
        }
    }
    for (uint64_t rv32_mask = g_rv32_cores_active; rv32_mask; rv32_mask &= rv32_mask - 1) {
        uint32_t core_index = __builtin_ctzll(rv32_mask);
        uint32_t tile_id = core_index / RV32_CORES_PER_E_TILE;
        uint32_t rv32_index = core_index % RV32_CORES_PER_E_TILE;
        auto *p_hart = &g_e_tiles[tile_id].rv32[rv32_index];
        // Hack: for simulator perf, an eth core busy lately (its sram written to, or it issued a NOC or eth command)
        // takes several steps per clock, so relayed traffic doesn't wait on it; an idle one takes one
        uint32_t steps = (g_clock < g_e_tiles[tile_id].busy_until) ? ETH_BUSY_STEPS : 1;
        for (uint32_t i = 0; (i < steps) && ((g_rv32_cores_active >> core_index) & 1); i++) {
            rv32_step(p_hart);
        }
    }
}

extern "C" API_EXPORT void libttsim_clock(uint32_t n_clocks) {
    FpEnvGuard fpenv_guard;
    TTSIM_VERIFY(s_ttsim_running, ConfigurationError, "sim is not running");
    uint32_t saved_chip_id = g_current_chip_id;
    for (uint32_t i = 0; i < n_clocks; i++) {
        for (uint32_t chip_id = 0; chip_id < g_num_chips; chip_id++) {
            ttsim_select_chip(chip_id);
            clock_current_chip();
        }
        g_clock++; // single global timebase, advanced once per clock across all chips
    }
    ttsim_select_chip(saved_chip_id);
}
