// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/eviction_age.h"

namespace {

constexpr u64 MiB = u64{1} << 20;

} // namespace

TEST_CASE("The collector keeps recently used images until the device runs short",
          "[video_core][texture_cache][eviction_age]") {
    using VideoCommon::EvictionAge;
    using VideoCommon::MinEvictionAge;
    // Mario Kart World's race: 6268 MiB in use, past the 5120 MiB critical threshold, under the
    // 7678 MiB budget. Aggressive (10), high priority (25) and normal (50) modes all wait.
    // (The limit here is the budget alone; the cache also takes its margins and host commit off.)
    for (const u64 mode_age : {u64{10}, u64{25}, u64{50}}) {
        REQUIRE(EvictionAge(mode_age, 0, 6268 * MiB, 7678 * MiB, false) == MinEvictionAge);
    }
    REQUIRE(EvictionAge(600, 0, 6268 * MiB, 7678 * MiB, false) == 600);
    // At the limit, after a failed image, or with no limit to compare: the mode's own age.
    REQUIRE(EvictionAge(10, 0, 7678 * MiB, 7678 * MiB, false) == 10);
    REQUIRE(EvictionAge(10, 0, 7900 * MiB, 7678 * MiB, false) == 10);
    REQUIRE(EvictionAge(25, 0, 6268 * MiB, 7678 * MiB, true) == 25);
    REQUIRE(EvictionAge(50, 0, 6268 * MiB, 0, false) == 50);
}

TEST_CASE("EDEN_NX2_TC_MIN_AGE can't stop eviction when the device runs short",
          "[video_core][texture_cache][eviction_age]") {
    using VideoCommon::EvictionAge;
    // #1290: Ocarina of Time on a 16 GB card with a leftover 600-frame override filled the heap
    // and crashed. While there is room the override still applies.
    REQUIRE(EvictionAge(10, 600, 6268 * MiB, 7678 * MiB, false) == 600);
    REQUIRE(EvictionAge(10, 600, 7678 * MiB, 7678 * MiB, false) == 10);
    REQUIRE(EvictionAge(10, 600, 6268 * MiB, 7678 * MiB, true) == 10);
    REQUIRE(EvictionAge(10, 9000, 6268 * MiB, 0, false) == 10);
}

TEST_CASE("The retention limit follows the lowest budget read lately",
          "[video_core][texture_cache][eviction_age]") {
    using VideoCommon::RetentionBudget;
    using VideoCommon::RetentionBudgetRise;
    // Ocarina of Time at 2x on a 16 GB card: the budget falls from 15226 to 14158 MiB and back.
    REQUIRE(RetentionBudget(0, 15226 * MiB) == 15226 * MiB);
    REQUIRE(RetentionBudget(15226 * MiB, 14158 * MiB) == 14158 * MiB);
    // Back up at the next check, the held budget rises one step at a time.
    REQUIRE(RetentionBudget(14158 * MiB, 15226 * MiB) == 14158 * MiB + RetentionBudgetRise);
    u64 held = 14158 * MiB;
    u64 checks = 0;
    while (held < 15226 * MiB) {
        held = RetentionBudget(held, 15226 * MiB);
        ++checks;
    }
    REQUIRE(held == 15226 * MiB);
    REQUIRE(checks == 67);
    // A budget that doesn't move is followed as is.
    REQUIRE(RetentionBudget(7678 * MiB, 7678 * MiB) == 7678 * MiB);
}
