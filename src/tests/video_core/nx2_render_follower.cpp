// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <latch>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_render_follower.h"

TEST_CASE("Render follower keeps a dequeued packet pending until it plays",
          "[nx2_render_follower]") {
    VideoCommon::Nx2RenderFollower follower;
    std::latch executing{1};
    std::latch release{1};
    std::vector<size_t> played;
    std::jthread render{[&] {
        while (auto packet = follower.Take()) {
            packet();
            packet = {};
            follower.Retire();
        }
    }};
    const auto engine_id = std::this_thread::get_id();
    bool on_render = false;
    follower.Publish([&] {
        on_render = std::this_thread::get_id() != engine_id;
        executing.count_down();
        release.wait();
        played.push_back(0);
    });
    executing.wait();
    const bool pending_while_executing = follower.Pending();
    release.count_down();
    follower.Wait();
    const bool pending_after_wait = follower.Pending();
    for (size_t sequence = 1; sequence <= 256; ++sequence) {
        follower.Publish([&, sequence] { played.push_back(sequence); });
    }
    follower.Publish({});
    render.join();

    REQUIRE(on_render);
    REQUIRE(pending_while_executing);
    REQUIRE_FALSE(pending_after_wait);
    REQUIRE(played.size() == 257);
    for (size_t sequence = 0; sequence < played.size(); ++sequence) {
        CHECK(played[sequence] == sequence);
    }
}
