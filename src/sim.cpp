// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Definitions of the central global simulator state and shared runtime utilities (logging, etc.).
#include <time.h>
#include <sys/mman.h>
#include "sim.h"

uint32_t g_current_chip_id;
uint64_t g_clock;
ChipState g_chips[NUM_CHIPS];

uint64_t ttsim_get_clock() {
    return g_clock;
}

void ttsim_init() {
    for (uint32_t chip_id = 0; chip_id < NUM_CHIPS; chip_id++) {
        ttsim_select_chip(chip_id);
        for (uint32_t tile_id = 0; tile_id < std::size(g_t_tiles); tile_id++) {
            t_tile_init(tile_id);
        }
        for (uint32_t tile_id = 0; tile_id < std::size(g_e_tiles); tile_id++) {
            e_tile_init(tile_id);
        }
        a_tile_init();
        for (uint32_t i = 0; i < std::size(g_dram); i++) {
            // Note that MAP_PRIVATE is required if we ever fork(), or else child processes will share DRAM with parent
            g_dram[i].p_mem = (uint8_t *)mmap(nullptr, DRAM_CHANNEL_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            TTSIM_VERIFY(g_dram[i].p_mem != MAP_FAILED, SystemError, "DRAM mmap() failed");
#if defined(MADV_HUGEPAGE)
            madvise(g_dram[i].p_mem, DRAM_CHANNEL_SIZE, MADV_HUGEPAGE);
#endif
            // XXX It might be interesting to use MADV_DONTNEED or MADV_FREE in the future to release DRAM pages if we see them cleared with all zero
        }
    }
    ttsim_select_chip(0);
}

void ttsim_select_chip(uint32_t chip_id) {
    TTSIM_VERIFY(chip_id < NUM_CHIPS, ConfigurationError, "chip_id=%d", chip_id);
    g_current_chip_id = chip_id;
}

void ttsim_exit() {
    ttsim_heartbeat();
    // XXX Would be good to munmap() DRAM here
}

bool ttsim_rv32_get_core_active(char tile_type, uint32_t tile_id, uint32_t rv32_id) {
    if (tile_type == 'T') {
        return (g_t_tiles[tile_id].rv32_cores_active >> rv32_id) & 1;
    } else {
        TTSIM_VERIFY(tile_type == 'E', AssertionFailure, "tile_type=%c", tile_type);
        uint32_t core_index = tile_id * RV32_CORES_PER_E_TILE + rv32_id;
        return (g_rv32_cores_active >> core_index) & 1;
    }
}

void ttsim_rv32_set_core_active(char tile_type, uint32_t tile_id, uint32_t rv32_id, bool active) {
    if (tile_type == 'T') {
        if (active) {
            g_t_tiles[tile_id].rv32_cores_active |= 1 << rv32_id;
        } else {
            g_t_tiles[tile_id].rv32_cores_active &= ~(1 << rv32_id);
        }
    } else {
        TTSIM_VERIFY(tile_type == 'E', AssertionFailure, "tile_type=%c", tile_type);
        uint32_t core_index = tile_id * RV32_CORES_PER_E_TILE + rv32_id;
        if (active) {
            g_rv32_cores_active |= 1ull << core_index;
        } else {
            g_rv32_cores_active &= ~(1ull << core_index);
        }
    }
}

void ttsim_heartbeat() {
    float seconds_elapsed = float(clock()) / CLOCKS_PER_SEC; // XXX this is "seconds of CPU time used" and not "time elapsed"; what do we really want?
    float mhz = g_clock / (seconds_elapsed * 1000000);
    if (mhz >= 10.0f) {
        ttsim_printf("%.1f seconds (%.1f MHz)\n", seconds_elapsed, mhz);
    } else {
        ttsim_printf("%.1f seconds (%.1f KHz)\n", seconds_elapsed, mhz*1000);
    }
}
