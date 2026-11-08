// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "common/common_types.h"

namespace VideoCommon {

/// Custom (EDEN_NX2_PREFLUSH=1): set right before a SignalFence call whose callback may be held
/// back until the fence's buffer downloads are in guest memory (syncpoint increments).
/// FenceManager::SignalFence reads and clears it on the same thread. Callers that must run their
/// callback at once (the query cache) never set it.
inline thread_local bool g_nx2_fence_holdable = false;

/// Compute semaphore releases set this instead (a kind bit): held only with
/// EDEN_NX2_HOLD_SEMAPHORES=1 or 2, and only kinds in EDEN_NX2_HOLD_MASK (default 7 = all).
inline constexpr u32 Nx2SemReport = 1;  ///< compute report semaphore with reduction
inline constexpr u32 Nx2SemQmdAdd = 2;  ///< QMD release, add
inline constexpr u32 Nx2SemQmdMax = 4;  ///< QMD release, max (NVN2 completion tracking)
inline thread_local u32 g_nx2_fence_holdable_semaphore = 0;

[[nodiscard]] inline u32 Nx2HoldMask() {
    static const u32 mask = [] {
        const char* value = std::getenv("EDEN_NX2_HOLD_MASK");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 7u;
    }();
    return mask;
}

/// EDEN_NX2_HOLD_SEMAPHORES: 0 (default) releases compute semaphores at once. 1 holds their
/// memory write until the fence's downloads are in guest memory (no flicker, but the GPU's own
/// acquires wait for each round trip: Castle Town ~30 -> ~21 fps). 2 holds the write the same
/// way, but computes the new value at once, keeps it in Nx2SemaphoreShadow and latches GPU
/// waiters, so only the guest CPU, which reads memory, sees it late.
[[nodiscard]] inline int Nx2HoldSemaphoresMode() {
    static const int mode = [] {
        const char* value = std::getenv("EDEN_NX2_HOLD_SEMAPHORES");
        return value != nullptr && (value[0] == '1' || value[0] == '2') ? value[0] - '0' : 0;
    }();
    return mode;
}

/// Newest value of each compute semaphore whose memory write is still held (mode 2), by device
/// address. GPU-side semaphore reads look here first.
class Nx2SemaphoreShadow {
public:
    static Nx2SemaphoreShadow& Get() {
        static Nx2SemaphoreShadow shadow;
        return shadow;
    }

    [[nodiscard]] std::optional<u32> Lookup(u64 device_address) {
        if (count.load(std::memory_order_acquire) == 0) {
            return std::nullopt;
        }
        std::scoped_lock lk{mutex};
        const auto it = values.find(device_address);
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second.value;
    }

    /// Returns a ticket for Clear.
    u64 Set(u64 device_address, u32 value) {
        std::scoped_lock lk{mutex};
        const u64 ticket = ++next_ticket;
        values[device_address] = Entry{value, ticket};
        count.store(values.size(), std::memory_order_release);
        return ticket;
    }

    /// The held write with this ticket reached memory: forget the shadow unless a newer one exists.
    void Clear(u64 device_address, u64 ticket) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        const auto it = values.find(device_address);
        if (it != values.end() && it->second.ticket == ticket) {
            values.erase(it);
        }
        count.store(values.size(), std::memory_order_release);
    }

private:
    struct Entry {
        u32 value;
        u64 ticket;
    };
    std::mutex mutex;
    std::unordered_map<u64, Entry> values;
    std::atomic<size_t> count{};
    u64 next_ticket{};
};

} // namespace VideoCommon
