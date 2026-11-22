// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/writeback_switches.h"

TEST_CASE("Overlap write-backs retain the tested Duskbloods default",
          "[video_core][texture_cache][writeback]") {
    using namespace VideoCommon::WritebackSwitches;
    REQUIRE(DuskbloodsDefault(DuskbloodsNetworkTestProgramId));
    REQUIRE_FALSE(DuskbloodsDefault(0));
    REQUIRE_FALSE(DuskbloodsDefault(0x04007EF01F8AE000ULL));
    REQUIRE_FALSE(DuskbloodsDefault(0x0400486024DEC000ULL));
}
