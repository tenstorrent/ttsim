// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// ttsim: entry point for the standalone full-system RV64 simulator.
#include "rv64_system.h"
#include <errno.h>
#include <ctype.h>

#define MAX_LOAD_BINS 16

struct LoadBin {
    const char *path;
    uint64_t addr;
};

static uint64_t parse_u64(const char *s) {
    TTSIM_VERIFY(s && *s, ConfigurationError, "expected a numeric argument");
    TTSIM_VERIFY((*s != '-') && !isspace((unsigned char)*s), ConfigurationError, "invalid numeric argument '%s'", s);
    errno = 0;
    char *end = nullptr;
    unsigned long long v = strtoull(s, &end, 0);
    TTSIM_VERIFY(end && (*end == '\0'), ConfigurationError, "invalid numeric argument '%s'", s);
    TTSIM_VERIFY(errno == 0, ConfigurationError, "numeric argument '%s' out of range", s);
    return v;
}

static uint32_t parse_u32(const char *s) {
    uint64_t v = parse_u64(s);
    TTSIM_VERIFY(v <= 0xFFFFFFFFull, ConfigurationError, "value '%s' exceeds 32 bits", s);
    return uint32_t(v);
}

static uint64_t load_bin(RvSystem *sys, const char *path, uint64_t addr) {
    FILE *f = fopen(path, "rb");
    TTSIM_VERIFY(f, ConfigurationError, "cannot open --load-bin file '%s'", path);
    TTSIM_VERIFY(fseek(f, 0, SEEK_END) == 0, ConfigurationError, "cannot seek --load-bin file '%s'", path);
    long fsz = ftell(f);
    TTSIM_VERIFY(fsz >= 0, ConfigurationError, "cannot size --load-bin file '%s'", path);
    rewind(f);
    TTSIM_VERIFY(rv64_sys_in_dram(sys, addr, uint64_t(fsz)), ConfigurationError,
        "--load-bin '%s' (%ld bytes) at 0x%llx does not fit in guest DRAM", path, fsz, addr);
    static uint8_t buf[1 << 16];
    uint64_t off = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        rv64_phys_write(sys, addr + off, buf, uint32_t(n));
        off += n;
    }
    fclose(f);
    return off;
}

int main(int argc, char **argv) {
    uint64_t max_insns = ~0ull;
    uint32_t timer_insns_per_tick = 25;
    uint64_t entry = 0x80000000ull;
    uint64_t dram_base = 0x80000000ull;
    uint64_t dram_size = 0x40000000ull; // 1 GiB (per minimal.dts)
    uint32_t num_harts = 1;
    LoadBin loads[MAX_LOAD_BINS];
    uint32_t num_loads = 0;
    uint64_t init_a0 = 0, init_a1 = 0;
    const char *disk_path = nullptr;
    bool interactive = false;
    const char *inject_text = nullptr;
    const char *inject_file = nullptr;
    uint64_t inject_at = 0;
    const char *tt_path = nullptr;
    uint64_t tt_ecam = 0x30000000ull;
    uint64_t tt_clock_burst = 0;

    // Bounds-checked option-argument accessor
    auto val = [&](int k) -> const char * {
        TTSIM_VERIFY((k >= 0) && (k < argc), ConfigurationError,
            "option '%s' requires an argument", argv[((k >= 1) && (k <= argc)) ? k - 1 : 0]);
        return argv[k];
    };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--mem-size")) {
            dram_size = parse_u64(val(++i));
        } else if (!strcmp(a, "--harts")) {
            num_harts = parse_u32(val(++i));
        } else if (!strcmp(a, "--timer-insns-per-tick")) {
            timer_insns_per_tick = parse_u32(val(++i));
            TTSIM_VERIFY(timer_insns_per_tick >= 1, ConfigurationError, "--timer-insns-per-tick must be >= 1");
        } else if (!strcmp(a, "--max")) {
            max_insns = parse_u64(val(++i));
        } else if (!strcmp(a, "--entry")) {
            entry = parse_u64(val(++i));
        } else if (!strcmp(a, "--set-reg")) {
            const char *name = val(++i);
            uint64_t v = parse_u64(val(++i));
            if (!strcmp(name, "a0")) {
                init_a0 = v;
            } else if (!strcmp(name, "a1")) {
                init_a1 = v;
            } else {
                TTSIM_ERROR(ConfigurationError, "--set-reg only supports a0 and a1, not '%s'", name);
            }
        } else if (!strcmp(a, "--load-bin")) {
            TTSIM_VERIFY(num_loads < MAX_LOAD_BINS, ConfigurationError, "too many --load-bin entries");
            loads[num_loads].path = val(++i);
            loads[num_loads].addr = parse_u64(val(++i));
            num_loads++;
        } else if (!strcmp(a, "--disk")) {
            disk_path = val(++i);
        } else if (!strcmp(a, "-i")) {
            interactive = true;
        } else if (!strcmp(a, "--uart-inject-at")) {
            inject_at = parse_u64(val(++i));
        } else if (!strcmp(a, "--uart-inject-text")) {
            inject_text = val(++i);
        } else if (!strcmp(a, "--uart-inject-file")) {
            inject_file = val(++i);
        } else if (!strcmp(a, "--tt-device")) {
            tt_path = val(++i);
        } else if (!strcmp(a, "--tt-ecam")) {
            tt_ecam = parse_u64(val(++i));
        } else if (!strcmp(a, "--tt-clock-burst")) {
            tt_clock_burst = parse_u64(val(++i));
        } else {
            TTSIM_ERROR(ConfigurationError, "unknown argument '%s'", a);
        }
    }

    RvSystem *sys = rv64_sys_create(dram_base, dram_size, num_harts, timer_insns_per_tick);
    Rv64SysHartState *h0 = rv64_sys_hart(sys, 0);
    if (tt_path) {
        TTSIM_VERIFY(rv64_sys_tt_attach(sys, tt_path, tt_ecam, tt_clock_burst),
            ConfigurationError, "--tt-device: failed to attach '%s'", tt_path);
    }
    if (disk_path) {
        rv64_sys_set_disk(sys, disk_path);
    }
    if (interactive) {
        rv64_sys_set_interactive(sys);
    }

    TTSIM_VERIFY(!(inject_text && inject_file), ConfigurationError,
        "--uart-inject-text and --uart-inject-file are mutually exclusive");
    if (inject_text) {
        rv64_sys_uart_inject(sys, inject_at, (const uint8_t *)inject_text, strlen(inject_text));
    } else if (inject_file) {
        FILE *f = fopen(inject_file, "rb");
        TTSIM_VERIFY(f, ConfigurationError, "--uart-inject-file: cannot open '%s'", inject_file);
        TTSIM_VERIFY(fseek(f, 0, SEEK_END) == 0, SystemError, "--uart-inject-file: seek '%s'", inject_file);
        long sz = ftell(f);
        TTSIM_VERIFY(sz >= 0, SystemError, "--uart-inject-file: ftell '%s' failed", inject_file);
        rewind(f);
        uint8_t *buf = (uint8_t *)malloc(size_t(sz) ? size_t(sz) : 1);
        size_t got = fread(buf, 1, size_t(sz), f);
        TTSIM_VERIFY(got == size_t(sz), SystemError, "--uart-inject-file: short read on '%s'", inject_file);
        fclose(f);
        rv64_sys_uart_inject(sys, inject_at, buf, got);
        free(buf);
    } else {
        TTSIM_VERIFY(inject_at == 0, ConfigurationError,
                     "--uart-inject-at requires --uart-inject-text or --uart-inject-file");
    }

    for (uint32_t k = 0; k < num_loads; k++) {
        [[maybe_unused]] uint64_t sz = load_bin(sys, loads[k].path, loads[k].addr);
    }
    h0->x_regs[10] = init_a0;
    h0->x_regs[11] = init_a1;

    h0->pc = entry; // hart0 entry point
    rv64_sys_init_reset_rom(sys, entry, h0->x_regs[11]);
    for (uint32_t i = 1; i < num_harts; i++) {
        Rv64SysHartState *hi = rv64_sys_hart(sys, i);
        hi->pc = rv64_sys_reset_rom_base(); // other HARTs start at the reset ROM in WFI
        hi->wfi_sleeping = true;
    }
    rv64_sys_run(sys, max_insns);
    return 0;
}
