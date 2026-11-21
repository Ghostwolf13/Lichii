// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/async_draw_policy.h"
#include "video_core/renderer_vulkan/pipeline_build.h"
#include "video_core/shader_jobs.h"

using Vulkan::DecideNativePending;
using Vulkan::NativeDrawAction;
using Vulkan::NativePendingMode;
using Vulkan::WaitWithinBudget;

TEST_CASE("Wait mode never skips or times out", "[video_core][native_pending]") {
    REQUIRE(DecideNativePending(NativePendingMode::Wait, true, 8000) == NativeDrawAction::Wait);
    REQUIRE(DecideNativePending(NativePendingMode::Wait, false, 0) == NativeDrawAction::Wait);
}

TEST_CASE("Skip mode skips what the heuristic allows", "[video_core][native_pending]") {
    REQUIRE(DecideNativePending(NativePendingMode::Skip, true, 8000) == NativeDrawAction::Skip);
    REQUIRE(DecideNativePending(NativePendingMode::Skip, false, 8000) == NativeDrawAction::Wait);
}

TEST_CASE("Bounded mode waits while budget remains, then skips", "[video_core][native_pending]") {
    REQUIRE(DecideNativePending(NativePendingMode::Bounded, true, 1) ==
            NativeDrawAction::WaitTimed);
    REQUIRE(DecideNativePending(NativePendingMode::Bounded, true, 0) == NativeDrawAction::Skip);
    REQUIRE(DecideNativePending(NativePendingMode::Bounded, true, -500) == NativeDrawAction::Skip);
}

TEST_CASE("Bounded mode still waits fully for draws that cannot be skipped",
          "[video_core][native_pending]") {
    REQUIRE(DecideNativePending(NativePendingMode::Bounded, false, 8000) ==
            NativeDrawAction::Wait);
    REQUIRE(DecideNativePending(NativePendingMode::Bounded, false, 0) == NativeDrawAction::Wait);
}

TEST_CASE("A slow build costs each frame at most its Bounded budget",
          "[video_core][native_pending]") {
    using namespace std::chrono_literals;
    VideoCommon::ShaderJobScheduler scheduler{
        {.cpu_threads = 1, .driver_threads = 1, .name = "PendingTest"}};
    const Vulkan::PipelineBuildQueue queue{.jobs = &scheduler};
    // A driver compile as slow as the multi-second ones in player logs
    Vulkan::PipelineBuild build{queue, [] { std::this_thread::sleep_for(1s); }};
    constexpr s64 budget_us = 8000;
    s64 used_us{};
    const auto begin = std::chrono::steady_clock::now();
    int frames{};
    while (!build.IsBuilt()) {
        const auto frame_begin = std::chrono::steady_clock::now();
        s64 budget_left_us = budget_us;
        // Each frame's draws and dispatches of the pipeline: the first waits out the budget, the
        // rest are skipped.
        for (int lookup = 0; lookup < 4 && !build.IsBuilt(); ++lookup) {
            const auto action =
                DecideNativePending(NativePendingMode::Bounded, true, budget_left_us);
            if (action == NativeDrawAction::Skip) {
                continue;
            }
            REQUIRE(action == NativeDrawAction::WaitTimed);
            WaitWithinBudget(budget_left_us, used_us, [&](std::chrono::microseconds timeout) {
                return build.WaitFor(timeout);
            });
        }
        REQUIRE(std::chrono::steady_clock::now() - frame_begin < 8ms + 50ms);
        ++frames;
        std::this_thread::sleep_for(8ms);
    }
    REQUIRE(std::chrono::steady_clock::now() - begin >= 1s);
    REQUIRE(frames > 30);
    REQUIRE(used_us <= frames * (budget_us + 50000));
}
