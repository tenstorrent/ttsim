// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Architecture-independent simulator core implementation.
#include <inttypes.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "common.h"

const char *ttsim_error_category_name(TTSimErrorCategory category) {
    switch (category) {
        case TTSimErrorCategory::UndefinedBehavior: return "UndefinedBehavior";
        case TTSimErrorCategory::UnpredictableValue: return "UnpredictableValue";
        case TTSimErrorCategory::NonContractualBehavior: return "NonContractualBehavior";
        case TTSimErrorCategory::AssertionFailure: return "AssertionFailure";
        case TTSimErrorCategory::UntestedFunctionality: return "UntestedFunctionality";
        case TTSimErrorCategory::UnimplementedFunctionality: return "UnimplementedFunctionality";
        case TTSimErrorCategory::UnsupportedFunctionality: return "UnsupportedFunctionality";
        case TTSimErrorCategory::SystemError: return "SystemError";
        case TTSimErrorCategory::ConfigurationError: return "ConfigurationError";
        default: return "???";
    }
}

void ttsim_printf(const char *fmt, ...) {
    uint64_t clock = ttsim_get_clock();
    va_list args;
    va_start(args, fmt);
    printf("[%" PRId64 "] ", clock);
    vprintf(fmt, args);
    va_end(args);
}

static void print_error(FILE *f, uint64_t clock, const char *category_str, const char *func, const char *fmt,
    va_list args) {
    fprintf(f, "[%" PRId64 "] ERROR: %s: %s%s", clock, category_str, func, fmt ? ": " : "\n");
    if (fmt) {
        vfprintf(f, fmt, args);
    }
    fflush(f);
}

// True if fds 1 and 2 are different files (a terminal, or both redirected to one log, is the same file).
static bool stdout_stderr_differ() {
    struct stat out, err;
    if (fstat(1, &out) || fstat(2, &err)) {
        return false;
    }
    return (out.st_dev != err.st_dev) || (out.st_ino != err.st_ino);
}

void ttsim_error(TTSimErrorCategory category, const char *func, const char *fmt, ...) {
    uint64_t clock = ttsim_get_clock();
    const char *category_str = ttsim_error_category_name(category);
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    print_error(stdout, clock, category_str, func, fmt, args);
    // Also print the line to stderr when it's a different file, e.g. pytest-xdist points each worker's stdout at
    // /dev/null, so only stderr reaches the log.
    if (stdout_stderr_differ()) {
        print_error(stderr, clock, category_str, func, fmt, args2);
    }
    va_end(args2);
    va_end(args);
    _Exit(1);
}
