// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/frame_pacer.h"

using Vulkan::FramePacer;

namespace {

using Clock = FramePacer::Clock;
constexpr Clock::time_point Start{std::chrono::seconds{1}};

/// Presents at each deadline the pacer gives and returns when the last one went.
Clock::time_point PresentGameFrames(FramePacer& pacer, double target_fps, u32 presents,
                                    int game_frames, Clock::time_point now) {
    for (int present = 0; present < game_frames * static_cast<int>(presents); ++present) {
        const auto deadline = pacer.Next(now, target_fps, presents);
        REQUIRE(deadline);
        now = *deadline;
    }
    return now;
}

} // Anonymous namespace

TEST_CASE("Frame pacing targets count game frames, not generated presents", "[frame_pacer]") {
    for (const double target : {30.0, 60.0, 90.0, 120.0}) {
        const auto interval =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / target));
        for (const u32 presents : {1U, 2U, 3U, 4U}) {
            FramePacer pacer;
            const auto end = PresentGameFrames(pacer, target, presents, 90, Start);
            // Target_90 with 2x generation: 90 game frames in a second, 180 presents.
            REQUIRE(end - Start >= interval * 90 - std::chrono::microseconds{1});
            REQUIRE(end - Start <= interval * 91);
        }
    }
}

TEST_CASE("Generated presents split their game frame's slot evenly", "[frame_pacer]") {
    FramePacer pacer;
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 90.0));
    const auto first = pacer.Next(Start, 90.0, 2);
    REQUIRE(first == Start + interval / 2);
    const auto second = pacer.Next(*first, 90.0, 2);
    REQUIRE(second == Start + interval);
    const auto third = pacer.Next(*second, 90.0, 2);
    REQUIRE(third == Start + interval + interval / 2);
}

TEST_CASE("A change in generated frames keeps the schedule", "[frame_pacer]") {
    FramePacer pacer;
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
    auto now = PresentGameFrames(pacer, 60.0, 1, 3, Start);
    now = PresentGameFrames(pacer, 60.0, 3, 3, now);
    now = PresentGameFrames(pacer, 60.0, 2, 3, now);
    REQUIRE(now - Start >= interval * 9 - std::chrono::microseconds{1});
    REQUIRE(now - Start <= interval * 9 + std::chrono::microseconds{1});
}

TEST_CASE("A late present goes at once and restarts the schedule", "[frame_pacer]") {
    FramePacer pacer;
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
    REQUIRE(pacer.Next(Start, 60.0, 1) == Start + interval);
    const auto late = Start + std::chrono::seconds{1};
    // 60 FPS restarts once six frames are late.
    for (int frame = 0; frame < 6; ++frame) {
        REQUIRE_FALSE(pacer.Next(late, 60.0, 1));
    }
    REQUIRE(pacer.Next(late, 60.0, 1) == late + interval);
}
