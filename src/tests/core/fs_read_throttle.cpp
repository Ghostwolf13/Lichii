// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "common/game_presets.h"
#include "core/hle/service/filesystem/fsp/fs_read_throttle.h"

using Service::FileSystem::ReadThrottle;
using namespace std::chrono_literals;

TEST_CASE("Age of Imprisonment's large reads are charged in full", "[fsp]") {
    const auto throttle = ReadThrottle::For(true, Settings::HyruleWarriorsProgramId);
    REQUIRE(throttle.Delay(64 * 1024) == 655'360ns);
    REQUIRE(throttle.Delay(4 * 1024 * 1024) == 41'943'040ns);
}

TEST_CASE("Switch 1 reads charge only their first 64 KiB", "[fsp]") {
    const auto throttle = ReadThrottle::For(false, 0x0100152000022000ULL);
    REQUIRE(throttle.Delay(16 * 1024) == 409'600ns);
    REQUIRE(throttle.Delay(4 * 1024 * 1024) == 1'638'400ns);
}

TEST_CASE("Other Switch 2 titles read with no delay", "[fsp]") {
    const auto throttle = ReadThrottle::For(true, Settings::MarioKartWorldProgramId);
    REQUIRE(throttle.Delay(4 * 1024 * 1024) == 0ns);
}
