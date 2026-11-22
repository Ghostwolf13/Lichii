// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host_vsync.h"

namespace HostVsync = VideoCore::HostVsync;
using namespace std::chrono_literals;

TEST_CASE("HostVsync counts timer starts against the FPS limit", "[video_core][host_vsync]") {
    HostVsync::Reset();
    const int owner = 0;
    int starts = 0;
    HostVsync::AddListener(&owner, [&] { ++starts; });
    HostVsync::SetPacing(&owner, true, 20);
    HostVsync::StartFrame();
    REQUIRE(starts == 1);

    // The display stops for over two 50 ms intervals: the timer starts the next frame.
    std::this_thread::sleep_for(110ms);
    const auto before_timer = HostVsync::Clock::now();
    REQUIRE(HostVsync::TimerTick(&owner, 16ms, [&] { ++starts; }));
    REQUIRE(starts == 2);

    // A frame reaching the display right after waits out the limit from the timer's start.
    const auto next = HostVsync::NextStart(before_timer);
    REQUIRE(next.has_value());
    REQUIRE(*next >= before_timer + 50ms);

    // The timer keeps stepping in while the display stays silent.
    std::this_thread::sleep_for(110ms);
    REQUIRE(HostVsync::TimerTick(&owner, 16ms, [&] { ++starts; }));
    REQUIRE(starts == 3);
    HostVsync::Reset();
}
