// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present/frame_gen_pacer.h"

using Vulkan::GenerationsWithoutWaiting;

TEST_CASE("Frame generation keeps a present frame for the next game frame", "[frame_gen_pacer]") {
    // The display holds every other frame (2x at 60 Hz): nothing is generated.
    REQUIRE(GenerationsWithoutWaiting(1, 0) == 0);
    REQUIRE(GenerationsWithoutWaiting(1, 1) == 0);
    // The display keeps up: the planned generations go ahead.
    REQUIRE(GenerationsWithoutWaiting(1, 2) == 1);
    REQUIRE(GenerationsWithoutWaiting(3, 4) == 3);
    // Room for part of them: the rest are dropped rather than waited for.
    REQUIRE(GenerationsWithoutWaiting(3, 3) == 2);
    REQUIRE(GenerationsWithoutWaiting(3, 2) == 1);
    REQUIRE(GenerationsWithoutWaiting(0, 4) == 0);
}
