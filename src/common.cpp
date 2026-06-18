// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Architecture-independent simulator core implementation.
#include <inttypes.h>
#include <stdarg.h>
#include "common.h"

const char *ttsim_error_category_name(TTSimErrorCategory category) {
    switch (category) {
        case TTSimErrorCategory::UndefinedBehavior: return "UndefinedBehavior";
        case TTSimErrorCategory::UnpredictableValueUsed: return "UnpredictableValueUsed";
        case TTSimErrorCategory::NonContractualBehavior: return "NonContractualBehavior";
        case TTSimErrorCategory::AssertionFailure: return "AssertionFailure";
        case TTSimErrorCategory::MissingSpecification: return "MissingSpecification";
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

void ttsim_error(TTSimErrorCategory category, const char *func, const char *fmt, ...) {
    uint64_t clock = ttsim_get_clock();
    const char *category_str = ttsim_error_category_name(category);
    printf("[%" PRId64 "] ERROR: %s: %s%s", clock, category_str, func, fmt ? ": " : "\n");
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    fflush(stdout);
    _Exit(1);
}
