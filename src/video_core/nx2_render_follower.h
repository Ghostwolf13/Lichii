// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <utility>

#include "common/common_types.h"
#include "common/unique_function.h"

namespace VideoCommon {

class Nx2RenderFollower {
public:
    using Work = Common::UniqueFunction<void>;

    [[nodiscard]] bool Pending() const {
        return submitted.load(std::memory_order_relaxed) != played.load(std::memory_order_acquire);
    }

    void Wait() const {
        const u64 target = submitted.load(std::memory_order_relaxed);
        for (u64 done = played.load(std::memory_order_acquire); done != target;
             done = played.load(std::memory_order_acquire)) {
            played.wait(done, std::memory_order_acquire);
        }
    }

    void Publish(Work work) {
        Wait();
        packet = std::move(work);
        submitted.store(submitted.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        submitted.notify_one();
    }

    [[nodiscard]] Work Take() {
        const u64 last = played.load(std::memory_order_relaxed);
        submitted.wait(last, std::memory_order_acquire);
        return std::move(packet);
    }

    void Retire() {
        played.store(played.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        played.notify_one();
    }

private:
    alignas(64) std::atomic<u64> submitted{};
    Work packet;
    alignas(64) std::atomic<u64> played{};
};

} // namespace VideoCommon
