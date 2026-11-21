// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>

namespace VideoCore {
class ShaderNotify {
public:
    [[nodiscard]] int ShadersBuilding() noexcept;

    /// Builds started and not done yet (ShadersBuilding counts all since the last idle moment).
    [[nodiscard]] int ShadersLeft() const noexcept {
        return num_building.load(std::memory_order::relaxed) -
               num_complete.load(std::memory_order::relaxed);
    }

    void MarkShaderComplete() noexcept {
        ++num_complete;
    }

    void MarkShaderBuilding() noexcept {
        ++num_building;
    }

private:
    std::atomic_int num_building{};
    std::atomic_int num_complete{};
    int report_base{};

    bool completed{};
    int num_when_completed{};
    std::chrono::steady_clock::time_point complete_time;
};
} // namespace VideoCore
