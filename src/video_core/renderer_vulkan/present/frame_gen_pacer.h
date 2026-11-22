// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

#include "common/common_types.h"

namespace Vulkan {

struct FrameGenPlan {
    size_t generations{};
    bool warm{};
};

/// The generations a game frame can present without the renderer waiting for a present frame:
/// those free now, less one kept for the next game frame. Frame generation runs on FIFO, so when
/// the display takes fewer presents than the game and its generated frames make (2x at 60 Hz),
/// waiting for them would hold the game below its own rate. Generation steps back instead.
[[nodiscard]] inline size_t GenerationsWithoutWaiting(size_t planned, size_t free_frames) {
    return std::min(planned, free_frames > 0 ? free_frames - 1 : 0);
}

class FrameGenPacer {
public:
    /// blocked: of the time since the last call, how long the renderer waited for the present
    /// thread to free a frame.
    [[nodiscard]] FrameGenPlan Plan(size_t capacity,
                                    std::chrono::steady_clock::duration blocked = {});

    void Reset();

    /// The smoothed time between real frames in seconds, or 0 while it is unknown.
    [[nodiscard]] f32 SmoothedInterval() const {
        return smoothed_interval;
    }

private:
    using Clock = std::chrono::steady_clock;

    void Stabilize(Clock::time_point now);
    [[nodiscard]] f32 PlanningInterval(f32 interval_seconds, f32 blocked_seconds);
    void LogPlans(f32 interval_seconds, f32 blocked_seconds, size_t generations);
    void DeferEvaluations(Clock::duration amount);
    void UpdateLimit(Clock::time_point now, f32 base_rate, f32 target_rate, size_t ceiling);

    std::optional<Clock::time_point> last_frame;
    std::optional<Clock::time_point> stable_until;
    std::optional<Clock::time_point> probe_until;
    std::optional<Clock::time_point> next_probe;
    std::optional<Clock::time_point> deficit_since;
    f32 smoothed_interval{};
    /// The interval generations are planned against (PlanningInterval), smoothed likewise.
    f32 planning_interval{};
    struct {
        u32 frames{};
        u32 generations{};
        f32 interval{};
        f32 blocked{};
    } plan_log;
    f32 output_credit{};
    f32 probe_base_rate{};
    f32 unloaded_base_rate{};
    size_t issued_generations{};
    size_t probe_previous_limit{};
    size_t limit{};
    u32 probe_failures{};
};

} // namespace Vulkan
