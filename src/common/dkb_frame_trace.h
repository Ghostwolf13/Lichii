// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "common/common_types.h"

namespace Common::DkbFrameTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_DKB_FRAME_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

/// EDEN_DKB_GPU_SITES=1 with EDEN_DKB_FRAME_TRACE=1: the Vulkan scheduler timestamps every recorded
/// command of the sampled frame for the `DKB gpu_sites` report. Off by default because thousands
/// of timestamps make that frame slower.
inline bool GpuSitesEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_DKB_GPU_SITES");
        return Enabled() && value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

struct RateLimit {
    std::chrono::steady_clock::time_point next{};

    bool Ready(std::chrono::steady_clock::time_point now) {
        if (now < next) {
            return false;
        }
        next = now + std::chrono::seconds{1};
        return true;
    }
};

constexpr u64 TimestampDelta(u64 begin, u64 end, u32 valid_bits) {
    if (valid_bits == 0 || valid_bits > 64) {
        return 0;
    }
    const u64 mask = valid_bits == 64 ? ~u64{0} : (u64{1} << valid_bits) - 1;
    return (end - begin) & mask;
}

} // namespace Common::DkbFrameTrace
